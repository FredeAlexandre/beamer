// SPDX-License-Identifier: GPL-3.0-only
// One UDP flow per directly linked peer (§4.2, §4.5): probing, ping/pong, receiver reports,
// replay protection, link fallback. All packet reception and timers run on the network thread
// (NetLoop); encoder threads send media through send_media().
// Camera (camera PHASE2 §4.2-4.5, §4.10): video streams on the same flow; the network thread is the only
// sealer of receiver-direction video packets (RR, NACK, PLI) and of the flow's time map; a flow-owned pacer
// sends video media through send_video(); the clock offset is estimated continuously from the pongs.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "events.h"
#include "net.h"
#include "packet.h"
#include "pacer.h"
#include "sync.h"
#include "video_packet.h"

namespace beamer::audio {

class RecvStream;
class SendStream;
class VideoRecvStream;
class VideoSendStream;

// Wakes NetLoop::run from any thread: a loopback UDP socket connected to itself, in the poll set (§4.2).
class Waker {
 public:
  Waker() = default;
  ~Waker();
  bool open();
  void wake();   // any thread; coalesced
  void drain();  // network thread
  net::Socket sock() const { return sock_; }

 private:
  net::Socket sock_ = net::kInvalid;
  std::atomic<bool> pending_{false};
};

constexpr uint16_t kDefaultAnswerPort = 24823;

enum class FlowState { Probing, Up, Link, Closed };
const char* flow_state_name(FlowState s);

struct FlowTiming {  // §4.2 / §4.5; overridable for tests
  int64_t hello_fast_ns = 200'000'000;
  int64_t hello_slow_ns = 1'000'000'000;
  int64_t ping_ns = 1'000'000'000;
  int64_t fallback_ns = 2'000'000'000;   // no valid UDP for this long -> link
  int64_t recover_ns = 1'000'000'000;    // this long of received UDP -> back to up
  int64_t run_gap_ns = 1'500'000'000;    // a gap longer than this restarts the "received UDP" run
  int64_t peer_link_ns = 250'000'000;    // link_in this recent -> the peer cannot hear our UDP
  int64_t link_flush_ns = 20'000'000;    // link_out batching
};

class Flow {
 public:
  Flow(const std::string& id, const std::string& peer, bool offerer, const uint8_t key[kKeyLen],
       int rr_ms, EventSink* events);
  ~Flow();
  Flow(const Flow&) = delete;
  Flow& operator=(const Flow&) = delete;

  const std::string& id() const { return id_; }
  const std::string& peer() const { return peer_; }
  bool offerer() const { return offerer_; }
  FlowTiming timing;

  // ---- main thread
  // Opens the UDP socket. host = the peer's IP when known.
  //  - port 0 and host known: no bind; connect()ed towards host:24823 (ephemeral local port, no
  //    listener: the Mac's case), re-connected by set_remote;
  //  - port > 0: bind port..port+7, then any, on the interface that routes to host (0.0.0.0 when
  //    host is unknown);
  //  - port 0 and no host: bind 0.0.0.0:0 (tests on 127.0.0.1 only).
  bool open_socket(const std::string& host, int port, std::string* err);
  void set_link_only();  // no UDP at all: the flow starts in state link
  uint16_t port() const { return port_; }
  bool set_remote(const std::string& host, int port, std::string* err);
  void link_in(std::vector<std::vector<uint8_t>>&& packets);
  void attach_recv(const std::shared_ptr<RecvStream>& s);
  void attach_send(const std::shared_ptr<SendStream>& s);
  void attach_video_recv(const std::shared_ptr<VideoRecvStream>& s);
  void attach_video_send(const std::shared_ptr<VideoSendStream>& s);  // creates the pacer
  void detach(uint32_t stream_id);
  void start();  // emits the initial flow_state

  // ---- any thread
  // Sends one sealed media packet: over UDP, and also queued for link_out while in link mode
  // (duplicates are removed by the receiver's replay window; UDP recovery is then immediate).
  void send_media(const uint8_t* pkt, size_t len);
  bool link_mode() const { return link_mode_.load(std::memory_order_relaxed); }
  FlowState state() const { return static_cast<FlowState>(state_.load(std::memory_order_relaxed)); }
  double rtt_ms() const { return rtt_ms_.load(std::memory_order_relaxed); }  // < 0: unknown
  std::atomic<bool> dev_drop_udp{false};
  std::atomic<double> dev_loss{0.0};  // fraction of media packets dropped at send (dev)
  // ---- video (camera PHASE2)
  // Feedback (video RR, NACK, PLI, time map): UDP always, plus the link outbox in link state (§4.2, §4.10).
  void send_feedback(const uint8_t* pkt, size_t len) { send_media(pkt, len); }
  // Pacer thread: one video media packet. UDP when up; in link state only frame-tagged link_out events
  // ({"video":true,"frame":N,"end":bool}, <= 4 packets of one frame each). Applies the dev video drops.
  void send_video(const PacedPacket& p);
  Pacer* pacer();                   // created on first use
  Pacer* pacer_if_any();
  // Clock offset (§4.5.3): peer clock minus ours, min-RTT pong of the last 10; false before the first pong.
  bool clock_offset(int64_t now, int64_t* offset_ns, int64_t* age_ns) const { return offset_.get(now, offset_ns, age_ns); }
  void wake() {
    Waker* w = waker_.load(std::memory_order_acquire);
    if (w) w->wake();
  }
  void set_waker(Waker* w) { waker_.store(w, std::memory_order_release); }
  std::atomic<double> dev_video_loss{0.0};      // fraction of video packets dropped at send
  std::atomic<double> dev_video_burst{0.0};     // mean burst (packets); 0 = iid, else Gilbert
  std::atomic<int64_t> dev_video_drop_until{0}; // drop all video until this now_ns()
  std::atomic<int64_t> dev_video_drop_pk{0};    // drop the next n video media packets (kept in the history)
  std::atomic<bool> dev_log_time_map{false};    // time_map events for every entry sent or received
  std::atomic<uint64_t> n_video_tx{0}, n_video_dev_dropped{0}, n_video_link_events{0}, n_time_map_tx{0},
      n_time_map_rx{0};
  std::atomic<uint64_t> n_rejected{0}, n_replayed{0}, n_unknown{0}, n_udp_rx{0}, n_link_rx{0},
      n_udp_tx{0}, n_link_tx{0}, n_link_dropped{0}, n_rr_tx{0};

  // ---- network thread
  net::Socket sock() const { return sock_; }
  void on_readable(int64_t now);
  void tick(int64_t now);
  int64_t next_deadline(int64_t now) const;  // earliest timer of this flow (network thread)
  void close_now();  // sends bye x3, closes the socket, state closed
  bool close_requested() const { return close_req_.load(); }
  void request_close() { close_req_.store(true); }
  void mark_closed();
  bool wait_closed(int ms);

 private:
  void handle_packet(const uint8_t* data, size_t len, int64_t now, bool via_link);
  void handle_ctrl(const Header& h, const uint8_t* payload, size_t plen, int64_t now);
  void mark_rx(int64_t now, bool via_link);
  void set_state(FlowState s);
  void send_ctrl(uint8_t ptype, const uint8_t* payload, size_t len);
  bool udp_send_raw(const uint8_t* pkt, size_t len);
  void send_rr(int64_t now);
  void flush_link_out();
  void video_tick(int64_t now);
  void send_time_map(int64_t now);
  void emit_video_event(uint32_t stream, uint32_t frame, std::vector<std::string>&& p, bool end);

  const std::string id_, peer_;
  const bool offerer_;
  uint8_t key_[kKeyLen];
  const int rr_ms_;
  EventSink* events_;

  std::mutex sock_mu_;  // socket send/connect/close
  net::Socket sock_ = net::kInvalid;
  uint16_t port_ = 0;
  bool link_only_ = false;
  std::atomic<bool> has_remote_{false};
  net::Addr remote_;

  // net thread state
  Sealer ctrl_sealer_;
  ReplayWindow ctrl_window_;
  int64_t opened_ns_ = 0, remote_set_ns_ = 0;
  bool got_udp_ = false;
  int64_t last_udp_rx_ = 0, udp_run_start_ = 0, last_link_in_ = 0;
  int64_t next_hello_ = 0, next_ping_ = 0, next_rr_ = 0, next_flush_ = 0;
  std::atomic<int64_t> last_udp_tx_{0};
  std::atomic<int> state_{static_cast<int>(FlowState::Probing)};
  std::atomic<bool> link_mode_{false};
  std::atomic<double> rtt_ms_{-1.0};
  double rtts_[5] = {0, 0, 0, 0, 0};
  int n_rtts_ = 0;
  size_t rr_rr_ = 0;  // round-robin index for receiver reports

  mutable std::mutex streams_mu_;
  std::map<uint32_t, std::shared_ptr<RecvStream>> recv_;
  std::map<uint32_t, std::shared_ptr<SendStream>> send_;
  std::map<uint32_t, std::shared_ptr<VideoRecvStream>> vrecv_;
  std::map<uint32_t, std::shared_ptr<VideoSendStream>> vsend_;

  // video: clock offset (written by the network thread), time map (network thread), link events (pacer thread)
  OffsetEstimator offset_;
  struct TmState {
    bool first_sent = false;
    int64_t second_at = 0;
  };
  std::map<uint32_t, TmState> tm_state_;
  int64_t next_tm_ = 0;
  std::atomic<Waker*> waker_{nullptr};
  std::mutex vlink_mu_;
  struct VEvent {
    uint32_t frame = 0;
    std::vector<std::string> p;
  };
  std::map<uint32_t, VEvent> vpending_;
  bool gilbert_bad_ = false;  // pacer thread
  std::mt19937 vrng_{12345};

  mutable std::mutex link_mu_;
  std::vector<std::vector<uint8_t>> inbox_;
  std::deque<std::vector<uint8_t>> outbox_;

  std::atomic<bool> close_req_{false};
  std::mutex closed_mu_;
  std::condition_variable closed_cv_;
  bool closed_ = false;

  std::mutex pacer_mu_;
  std::unique_ptr<Pacer> pacer_;  // last: destroyed first (its thread calls send_video)
};

// The network thread: poll over all flow sockets and the waker, timers, link queues. It sleeps until the
// earliest Flow::next_deadline (2 ms in link mode or before the remote is set, up to 10 ms while a flow is up,
// sooner for video timers); a packet, or a Waker::wake() from another thread, ends the wait at once.
class NetLoop {
 public:
  NetLoop() = default;
  ~NetLoop() { stop(); }
  void start();
  void stop();
  void add(const std::shared_ptr<Flow>& f);
  // Closes the flow on the network thread (bye, socket close) and waits for it.
  void close(const std::shared_ptr<Flow>& f);

 private:
  void run();
  std::mutex mu_;
  std::vector<std::shared_ptr<Flow>> flows_;
  uint64_t gen_ = 0;  // bumped on every change of flows_
  std::thread thread_;
  std::atomic<bool> stop_{false};
  bool running_ = false;
  Waker waker_;
};

}  // namespace beamer::audio
