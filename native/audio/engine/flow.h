// SPDX-License-Identifier: GPL-3.0-only
// One UDP flow per directly linked peer (§4.2, §4.5): probing, ping/pong, receiver reports,
// replay protection, link fallback. All packet reception and timers run on the network thread
// (NetLoop); encoder threads send media through send_media().
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "events.h"
#include "net.h"
#include "packet.h"

namespace beamer::audio {

class RecvStream;
class SendStream;

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

  std::mutex streams_mu_;
  std::map<uint32_t, std::shared_ptr<RecvStream>> recv_;
  std::map<uint32_t, std::shared_ptr<SendStream>> send_;

  mutable std::mutex link_mu_;
  std::vector<std::vector<uint8_t>> inbox_;
  std::deque<std::vector<uint8_t>> outbox_;

  std::atomic<bool> close_req_{false};
  std::mutex closed_mu_;
  std::condition_variable closed_cv_;
  bool closed_ = false;
};

// The network thread: poll over all flow sockets (2 ms), timers, link queues.
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
};

}  // namespace beamer::audio
