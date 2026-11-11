// SPDX-License-Identifier: GPL-3.0-only
// Video receive stream (camera PHASE2 §4.5). VideoRecvCore holds every rule with explicit time (reassembly, loss
// on evidence only, clean runs, playout target, presenter, NACK/PLI/RR state); VideoRecvStream runs it on three
// threads: the network thread (ingest, and the only sealer of NACK/PLI/RR: ingest() and net_tick()), the video
// thread (decode hand-off, presenter, heartbeat: it calls exactly one of present/hold/show_placeholder on every
// display tick) and the backend's decoder thread (on_decoded / on_sink_event, posted to a mailbox).
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "events.h"
#include "streams.h"
#include "sync.h"
#include "video_backend.h"
#include "video_packet.h"

namespace beamer::audio {

class Flow;

// One action for the sink, executed by the video thread in order, outside the core's lock.
struct SinkAction {
  enum Kind { kDecode, kPresent, kHold, kDrop, kPlaceholder } kind;
  uint32_t frame = 0;
  bool keep = false;
  std::shared_ptr<std::vector<uint8_t>> au;
  std::string state;
};

struct RecvTiming {  // §4.2 / §4.5; overridable for tests
  int64_t rr_ns = 50'000'000;
  int64_t start_pli_ns = 100'000'000;
  int64_t broken_pli_ns = 300'000'000;
  int64_t hold_to_reconnecting_ns = 2'000'000'000;
  int64_t stall_ns = 500'000'000;
};

// Thread-ownership checks for the tests (§10.2 TSan storm): every receiver-direction seal must happen on one
// thread (the network thread), every sender-direction seal of a stream on its sender thread. Violations counted.
extern std::atomic<uint64_t> g_video_seal_thread_violations;

class VideoRecvCore {
 public:
  VideoRecvCore(uint32_t stream, const uint8_t key[kKeyLen], int fps, int w, int h);
  ~VideoRecvCore();
  RecvTiming timing;
  LipSyncHold* lipsync = nullptr;

  // ---- clock offset source (flow), set once
  std::function<bool(int64_t now, int64_t* off, int64_t* age)> offset_source;
  void set_link_mode(bool link) { link_mode_.store(link); }

  // ---- network thread
  // Authenticates one media packet; may seal NACK packets into `out` (sealed here: network thread).
  OpenResult ingest(const uint8_t* data, size_t len, int64_t now, bool via_link, std::vector<std::vector<uint8_t>>* out);
  void on_time_map(uint32_t ts, uint64_t sender_ns);
  // NACK retries, PLI, RR: sealed packets to send now.
  void net_tick(int64_t now, double rtt_ms, std::vector<std::vector<uint8_t>>* out);
  int64_t next_net_deadline() const;
  bool media_recent(int64_t now) const;

  // ---- video thread
  void start(int64_t now, std::vector<SinkAction>* acts);  // shows `starting`, arms the start PLI
  // Decoder results and sink events, then decode hand-off, loss evaluation, presenter.
  void on_decoded(uint32_t frame, bool ok, double ms, int64_t now);
  void on_sink_event(const std::string& what, const std::string& detail, int64_t now);
  void pump(int64_t now, bool display_tick, std::vector<SinkAction>* acts);
  int64_t next_video_deadline(int64_t now) const;
  bool take_wake_net();  // a PLI or immediate RR is due: the network thread should run
  bool take_failure(std::string* detail);
  void set_floor_ms(double f);

  Json stats(double dt_s);
  // tests
  struct Probe {
    double target_ms, p99_ms;
    uint64_t lost, late, skipped, holes, presented, nack_tx, retx_ok, pli_tx, rr_tx;
    uint32_t clean_since, acked;
    bool broken, started, have_map, provisional;
    uint32_t next_dec;
    double period_ms;
    int64_t offset_ns;
    std::string placeholder;
  };
  Probe probe() const;
  int64_t present_time_of(uint32_t frame) const;  // 0 = unknown

 private:
  struct Frame {
    uint32_t ts = 0;
    uint16_t nfrag = 0, got = 0;
    std::vector<std::vector<uint8_t>> frags;
    std::vector<bool> have;
    uint8_t vflags = 0;
    uint16_t enc_delay = 0;
    uint64_t max_seq = 0;
    int64_t first_ns = 0, complete_ns = 0;
    bool complete = false;
    bool handed = false;    // given to the decoder
    bool clean = false, keep = false, revoked = false, handed_keep = false;
    bool decoded = false, ok = false, presented = false;
    int64_t decoded_ns = 0;
  };
  void seal_feedback(uint8_t ptype, const uint8_t* p, size_t n, std::vector<std::vector<uint8_t>>* out);
  bool mapped_out(const Frame& f, uint32_t frame, int64_t now, int64_t* out_ns);  // encoder output, receiver clock
  int64_t present_est(uint32_t frame, int64_t now);
  int64_t frame_period_ns() const;
  void go_broken(uint8_t reason, int64_t now);
  void need_idr(uint8_t reason, int64_t now);
  void revoke_from(uint32_t frame, std::vector<SinkAction>* acts);
  void complete_frame(uint32_t frame, Frame& f, int64_t now);
  void mark_lost(uint32_t frame, bool hole, int64_t now);
  void request_pli_now(uint8_t reason, int64_t now);
  int64_t offset_now(int64_t now, bool* ok);

  mutable std::mutex mu_;
  const uint32_t stream_;
  uint8_t key_[kKeyLen];
  int fps_;
  int w_, h_;
  std::atomic<bool> link_mode_{false};
  // network side
  VideoReplayWindow window_;
  Sealer fb_sealer_;  // receiver direction: NACK, PLI, RR (network thread only)
  std::thread::id fb_thread_{};
  uint64_t hi_seq_ = 0;
  uint32_t last_pkt_frame_ = 0;
  bool last_pkt_end_ = false;
  int64_t last_pkt_ns_ = 0, stall_end_ns_ = 0;
  uint32_t newest_ts_ = 0;
  struct Nack {
    int tries;
    int64_t next;
    uint32_t lo, hi;
  };
  std::map<uint64_t, Nack> nacks_;
  std::vector<uint64_t> nack_now_;
  std::set<uint32_t> lost_evidence_;
  // frames
  std::map<uint32_t, Frame> frames_;
  bool have_any_ = false;
  uint32_t newest_seen_ = 0;
  // decode state
  bool started_ = false;            // a complete IDR has been taken since the last (re)start
  uint32_t next_dec_ = 0;
  bool chain_clean_ = false;        // decode-time chain state
  bool broken_ = true;
  uint32_t run_start_ = kNoFrame;   // IDR/refresh that will open a new clean run once decoded ok
  uint32_t clean_since_ = kNoFrame, acked_ = kNoFrame;
  std::deque<int64_t> dec_errors_;
  std::map<std::string, std::deque<int64_t>> size_rejects_;
  bool failed_ = false, fail_reported_ = false;
  std::string fail_detail_;
  std::vector<SinkAction> pending_;  // drops / placeholders raised outside pump()
  // PLI state
  bool idr_wanted_ = true;          // start/size: repeat every 100 ms until an IDR newer than idr_after_
  uint8_t idr_reason_ = kPliStart;
  bool idr_after_valid_ = false;
  uint32_t idr_after_ = 0;
  uint8_t broken_reason_ = kPliLoss;
  int64_t next_pli_ = 0;
  uint8_t pli_reason_now_ = kPliStart;
  bool wake_net_ = false;
  bool rr_now_ = false;
  int64_t next_rr_ = 0;
  // timing
  TimeMapBook map_{90000};
  bool provisional_ = false;
  int64_t prov_offset_ = 0;
  int prov_samples_ = 0;
  PlayoutTarget playout_;
  double d_now_ms_ = 70;
  int64_t period_ns_ = 33'333'333;
  // presenter
  std::map<uint32_t, int64_t> ready_;  // decoded kept pictures -> present time (0 = on decode)
  bool have_presented_ = false;
  uint32_t last_presented_ = 0;
  int64_t last_present_ns_ = 0;
  std::string shown_ = "";          // "" = live, else the placeholder state shown
  bool live_ = false;
  bool presented_since_tick_ = false;
  int64_t last_tick_ = 0;
  // RR interval state
  int64_t margin_min_01ms_ = -1;
  uint64_t rx_bytes_iv_ = 0;
  int64_t rr_iv_start_ = 0;
  // counters
  uint64_t lost_ = 0, late_ = 0, skipped_ = 0, holes_ = 0, nack_tx_ = 0, retx_ok_ = 0, pli_tx_ = 0, rr_tx_ = 0,
           presented_ = 0, rx_pkts_ = 0, rx_bytes_ = 0, frozen_ns_ = 0, held_ns_ = 0, dec_n_ = 0, d_samples_ = 0;
  double dec_ms_sum_ = 0;
  std::string decoder_ = "hw";
  uint64_t p_presented_ = 0, p_bytes_ = 0, p_frozen_ = 0, p_held_ = 0, p_dec_n_ = 0;
  double p_dec_sum_ = 0;
  int64_t last_media_ns_ = 0;
};

class VideoRecvStream : public StreamBase, public VideoSinkListener {
 public:
  struct Config {
    uint32_t id = 0;
    uint8_t key[kKeyLen] = {0};
    int w = 1280, h = 720, fps = 30;
    double floor_ms = 50;
  };
  using FailFn = std::function<void(uint32_t stream, const std::string& code, const std::string& detail)>;
  VideoRecvStream(const Config& cfg, std::shared_ptr<Flow> flow, EventSink* events, FailFn on_fail, LipSyncHold* sync);
  ~VideoRecvStream() override;
  bool start(VideoBackend& backend, std::string* err_code, std::string* err_msg);
  void stop() override;
  Json stats(double dt_s) override;
  bool is_output() const override { return true; }
  Flow* flow() const override { return flow_.get(); }
  void set_floor_ms(double f) { core_.set_floor_ms(f); }
  void set_frozen(bool f) { frozen_.store(f); }  // dev vcam_kill_beat: the presenter stops calling the sink

  // network thread
  OpenResult ingest(const uint8_t* data, size_t len, int64_t now, bool via_link, std::vector<std::vector<uint8_t>>* out) {
    OpenResult r = core_.ingest(data, len, now, via_link, out);
    if (r == OpenResult::Ok) {
      {
        // a state change under mu_, so the video thread cannot miss it between pump() and its wait (R3 #10)
        std::lock_guard<std::mutex> lock(mu_);
        pkts_++;
      }
      cv_.notify_all();
    }
    return r;
  }
  void on_time_map(uint32_t ts, uint64_t ns) { core_.on_time_map(ts, ns); }
  void net_tick(int64_t now, double rtt_ms, std::vector<std::vector<uint8_t>>* out) { core_.net_tick(now, rtt_ms, out); }
  int64_t next_net_deadline() const { return core_.next_net_deadline(); }
  bool media_recent(int64_t now) const { return core_.media_recent(now); }

  // VideoSinkListener (backend thread)
  void on_decoded(uint32_t frame, bool ok, double decode_ms) override;
  void on_sink_event(const std::string& what, const std::string& detail) override;

  VideoRecvCore& core() { return core_; }
  // tests: called on the video thread after each round (pump + sink calls), just before it waits again
  std::function<void(bool display_tick)> test_before_wait;

 private:
  void video_loop();
  Config cfg_;
  std::shared_ptr<Flow> flow_;
  EventSink* events_;
  FailFn on_fail_;
  VideoRecvCore core_;
  std::unique_ptr<VideoSinkHandle> sink_;
  std::mutex mu_;
  std::condition_variable cv_;
  uint64_t pkts_ = 0;  // under mu_: media packets accepted (wakes the video thread)
  struct Mail {
    bool decoded;
    uint32_t frame;
    bool ok;
    double ms;
    std::string what, detail;
  };
  std::deque<Mail> mail_;
  bool stop_ = false;
  std::atomic<bool> frozen_{false};
  std::thread thread_;
  bool stopped_ = false;
};

}  // namespace beamer::audio
