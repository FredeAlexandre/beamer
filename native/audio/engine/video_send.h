// SPDX-License-Identifier: GPL-3.0-only
// Video send stream (camera PHASE2 §4.3): numbers each AU in on_frame, queues it, and its sender thread (the only
// user of the stream's sender-direction Sealer) fragments, seals, keeps the history and hands the packets to the
// flow's pacer. Feedback (RR, NACK, PLI) arrives on the network thread and is handed to the sender thread through
// a mailbox; every CameraHandle call except the first open/close is made on the sender thread.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "events.h"
#include "pacer.h"
#include "streams.h"
#include "sync.h"
#include "video_backend.h"
#include "video_packet.h"

namespace beamer::audio {

class Flow;

// The §4.3 recovery and acknowledgement rules, with explicit time (pure; one owner thread or a mutex).
class SenderRecovery {
 public:
  enum Action { kNone = 0, kStart, kIdr, kRefresh };
  struct Counters {
    uint64_t pli = 0, dup = 0, dropped_window = 0, idr_req = 0, refresh_req = 0, deferred = 0;
  };
  void set_rtt_ns(int64_t r) { rtt_ns_ = r > 0 ? r : 20'000'000; }
  bool started() const { return started_; }
  Action on_pli(const Pli& p, int64_t now);
  Action on_drop(int64_t now);   // an AU dropped after numbering (hard cap, video_link dropped, video_drop_au)
  Action on_timer(int64_t now);  // deferred class-2 IDR, refresh failures
  int64_t next_timer() const;    // 0 = none
  void on_idr_numbered(uint32_t frame, int64_t now);  // any AU with idr set
  void on_idr_first_packet(int64_t now, int64_t pace_ns);
  void on_ltr_sent(uint32_t frame);
  // A video RR: returns the LTRs newly verified by its clean run (each once).
  std::vector<uint32_t> on_rr(uint32_t clean_since, uint32_t acked, bool broken, int64_t now);
  bool has_verified() const { return !verified_.empty(); }
  uint32_t newest_verified() const { return verified_.empty() ? kNoFrame : *verified_.rbegin(); }
  const Counters& counters() const { return c_; }

  static constexpr int64_t kClass1Window = 1'000'000'000, kClass2Window = 2'000'000'000;

 private:
  Action class2(int64_t now, bool from_pli, uint32_t last_clean);
  Action idr(int64_t now);
  bool started_ = false;
  int64_t rtt_ns_ = 20'000'000;
  bool idr_pending_ = false;        // requested, first packet not yet left
  int64_t pending_until_ = 0;       // first packet left + RTT + pace + 50 ms
  int64_t last_idr_ = 0;            // last AU with idr set (numbered)
  bool have_idr_ = false;
  uint32_t last_idr_frame_ = 0;
  int64_t deferred_idr_at_ = 0;     // class-2 IDR waiting for its window
  // refresh bookkeeping
  int64_t last_refresh_ = 0;
  uint32_t last_refresh_clean_ = kNoFrame;
  int64_t refresh_check_at_ = 0;    // 1 s after the newest refresh, unless a newer clean run came
  uint32_t clean_at_refresh_ = kNoFrame;
  bool clean_seen_ = false;
  uint32_t newest_clean_since_ = kNoFrame;
  int failed_refreshes_ = 0;
  std::deque<uint32_t> sent_ltrs_;  // last 64
  std::set<uint32_t> verified_;
  Counters c_;
};

class VideoSendStream : public StreamBase, public EncoderSink, public PacerClient,
                        public std::enable_shared_from_this<VideoSendStream> {
 public:
  struct Config {
    uint32_t id = 0;
    uint8_t key[kKeyLen] = {0};
    CameraSpec spec;          // device / test / fmt / bitrate / ltr
    int fps = 30;
    int link_bitrate = 1'000'000, link_fps = 15;
    double idr_pace_x = 4.0;
    int64_t no_pli_ns = 3'000'000'000LL;       // §6.5 camera-on guards
    int64_t no_feedback_ns = 5'000'000'000LL;
  };
  using FailFn = std::function<void(uint32_t stream, const std::string& code)>;
  VideoSendStream(const Config& cfg, std::shared_ptr<Flow> flow, EventSink* events, FailFn on_fail);
  ~VideoSendStream() override;
  // Opens the camera + encoder (the camera starts now, Deviation 3) and the sender thread. No AU before the
  // first PLI.
  bool start(VideoBackend& backend, std::string* err_code, std::string* err_msg);
  void stop() override;
  Json stats(double dt_s) override;
  Flow* flow() const override { return flow_.get(); }
  std::string device_in_use() const;

  // control thread
  void configure(int w, int h, int fps, int bitrate);   // 0 = unchanged
  void link_control(const bool* paused, const std::vector<uint32_t>& dropped);
  void dev_drop_au(int n) { drop_au_.fetch_add(n); }
  // tests: stall the sender thread (forced backlog); `pause` = backpressure still runs during the stall
  void dev_stall_ms(int ms, bool pause = true) {
    stall_pause_.store(pause);
    stall_until_.store(now_ns() + int64_t(ms) * 1'000'000);
  }
  void set_idr_pace_x(double x) { pace_x_.store(x); }
  bool take_camera_event(Json* ev);                     // a changed `camera` event, or false
  bool owns_camera(const CameraHandle* h) {
    std::lock_guard<std::mutex> lock(cam_mu_);
    return h && cam_.get() == h;
  }
  uint64_t serial() const { return serial_; }  // unique per stream object in this process (never reused)
  // R3 #13: the stream that owns the camera handle `h` right now (stream id and serial), from a registry kept while
  // the stream holds the handle. For VideoBackendListener::camera_lost, which runs while `h` is alive: looked up
  // there, not later on the helper thread, where `h` may already be freed and its address reused by a new stream.
  static bool camera_owner(const CameraHandle* h, uint32_t* stream, uint64_t* serial);

  // EncoderSink (backend's encoder thread)
  uint32_t on_frame(const EncodedFrame& f) override;
  void on_camera_event(const std::string& what, const std::string& detail) override;
  // PacerClient (pacer thread)
  void on_paced(const PacedPacket& p, int64_t now) override;
  // network thread: RR / NACK / PLI, receiver direction
  OpenResult ingest_feedback(const uint8_t* data, size_t len, int64_t now);
  // time map (network thread reads)
  bool time_map_pair(uint32_t* ts, int64_t* ns) const { return tm_.read(ts, ns); }
  int64_t first_tx_ns() const { return first_tx_ns_.load(std::memory_order_relaxed); }

 private:
  struct Au {
    std::vector<uint8_t> data;
    uint32_t frame = 0;
    uint64_t capture_ns = 0;
    int64_t out_ns = 0;
    bool idr = false, refresh = false, ltr = false;
    int w = 0, h = 0;
  };
  struct Hist {
    uint64_t seq;
    uint32_t frame;
    int64_t at;
    std::shared_ptr<const std::vector<uint8_t>> bytes;
  };
  struct Mail {
    uint8_t ptype;
    std::vector<uint8_t> payload;
    int64_t at;
  };
  void sender_loop();
  void send_au(Au& au, int64_t now);
  void act(SenderRecovery::Action a);
  void handle_mail(const Mail& m, int64_t now);
  void update_pause(int64_t now);
  void fail(const std::string& code);
  int effective_bitrate() const;

  Config cfg_;
  std::shared_ptr<Flow> flow_;
  EventSink* events_;
  FailFn on_fail_;
  std::unique_ptr<CameraHandle> cam_;
  std::mutex cam_mu_;  // cam_ lifetime vs. sender-thread calls
  const uint64_t serial_;
  void set_camera_locked(std::unique_ptr<CameraHandle> cam);  // cam_mu_ held; keeps the owner registry in step

  // queue (encoder thread -> sender thread)
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Au> q_;
  size_t q_bytes_ = 0;
  std::deque<Mail> mail_;
  bool stop_ = false;
  std::set<uint32_t> dropped_;      // numbered but never sent (mu_)
  bool drop_pending_ = false;       // recovery wanted for a drop (mu_)
  uint32_t next_frame_ = 0;
  uint64_t first_capture_ns_ = 0;
  bool have_first_capture_ = false;
  std::atomic<int> drop_au_{0};
  std::atomic<int64_t> stall_until_{0};
  std::atomic<bool> stall_pause_{true};
  std::atomic<double> pace_x_{4.0};

  // policy (pol_mu_: sender thread and on_frame)
  std::mutex pol_mu_;
  SenderRecovery rec_;

  // sender thread
  std::thread thread_;
  Sealer sealer_;
  std::thread::id seal_thread_{};
  bool first_packet_ = true;
  std::deque<Hist> hist_;
  int64_t start_ns_ = 0;
  bool running_ = false;
  std::atomic<bool> failed_{false};
  bool paused_bp_ = false, paused_applied_ = false;
  std::atomic<bool> paused_link_{false};
  int64_t paused_since_ = 0;
  bool link_state_ = false;
  std::atomic<int> step_{0};
  int64_t step_since_ = 0;
  struct RrSample {
    int64_t at;
    uint32_t lost, late;
    uint64_t frames;
  };
  std::deque<RrSample> rr_win_;
  int base_bitrate_ = 0, base_fps_ = 30;
  std::atomic<int> fps_now_{30}, bitrate_now_{0}, fmt_w_{0}, fmt_h_{0};
  std::atomic<uint32_t> verified_newest_{kNoFrame};
  // IDR first packet tracking (pacer thread -> sender thread)
  std::atomic<int64_t> idr_first_at_{0};
  std::atomic<uint64_t> idr_first_bytes_{0};

  // network thread
  VideoReplayWindow fb_window_;
  std::atomic<int64_t> last_pli_ns_{0}, last_fb_ns_{0};

  PairPublisher tm_;
  std::atomic<int64_t> first_tx_ns_{0};

  // camera event state (cam_ev_mu_)
  std::mutex cam_ev_mu_;
  Json cam_state_;
  bool cam_dirty_ = false;
  std::string cam_device_;

  // counters
  std::atomic<uint64_t> frames_{0}, idr_{0}, refresh_{0}, nack_rx_{0}, retx_{0}, pli_rx_{0}, q_drop_{0}, rr_rx_{0},
      tx_bytes_{0}, tx_pkts_{0}, enc_delay_sum_us_{0}, paused_ns_{0}, dev_dropped_au_{0};
  uint64_t last_frames_ = 0, last_bytes_ = 0, last_enc_sum_ = 0, last_pace_wait_ = 0, last_pace_n_ = 0;
  uint64_t last_paused_ns_ = 0;
  bool stopped_ = false;
};

}  // namespace beamer::audio
