// SPDX-License-Identifier: GPL-3.0-only
// Pull-model adaptive jitter buffer with Opus FEC/PLC and buffer-level drift control: a faithful
// port of proto/transport/media.py `JitterBuffer` (parameters measured in Phase 1, see
// proto/transport/README.md "Jitter buffer and drift control" and "Mac->PC jitter").
//
// Single-threaded: push() and pull() run on the render IO thread (the network thread hands packets
// over through an SPSC ring and push() is called when draining it right before pull()). Nothing
// here allocates after construction.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "codec.h"
#include "engine.h"
#include "packet.h"
#include "resampler.h"

namespace beamer::audio {

// One authenticated media packet as handed from the network thread to the render thread.
struct MediaFrame {
  int64_t arrival_ns = 0;
  uint64_t seq = 0;
  uint32_t ts = 0;
  uint16_t nsamp = 0;
  uint8_t flags = 0;
  uint8_t channels = 0;
  uint16_t len = 0;
  uint8_t data[kMaxPayload];
};

struct JbConfig {
  int channels = 1;
  int frame = 480;            // samples per channel per packet (updated from packets)
  double min_ms = 10.0;       // §4.4: clamped 10..120 ms
  double max_ms = 120.0;
  double fixed_ms = -1.0;     // >= 0 pins the target
  double margin_ms = 2.0;
  double max_ppm = 500.0;
  double window_s = 10.0;
  bool drift = true;
  bool fec = true;
  double catchup_ppm = 2000.0;
  double drop_ms = 30.0;
  double peak_hold_s = 10.0;
  int peak_count = 2;
  double peak_min_ms = 15.0;
  bool pull_jitter = false;   // provision playback-clock jitter (only for irregular callbacks)
  // Engine addition (not in media.py): when frames concealed by expansion turn out to be lost
  // (a newer frame arrived, this one never will), count the expansion as their concealment
  // instead of concealing them again, so a loss at the head of the buffer does not add a frame
  // of latency. Late frames keep the reference behaviour (the expansion stays as a stretch).
  // false = exactly media.py (tests/jb_crosscheck.py).
  bool merge_expand = true;
  // Recompute the p97 transit statistic at most this often (0 = every pull with new packets,
  // as media.py). It moves by one sample in ~1000 per packet; 20 ms saves most of the IO-thread CPU.
  double jitter_recalc_ms = 20.0;
  // Engine addition (WP11): once the clock ratio has been measured from the timestamps
  // (drift_est_ppm, valid after 60 s), it is the drift integral instead of the integrated level error.
  // The level also rises by a frame at each expansion whose late frame then plays; integrating that
  // pinned the integral at +200 ppm live (WP10 soak) for clocks ~20 ppm apart. false = media.py.
  bool drift_est = true;
};

struct JbCounters {
  uint64_t rx = 0, rx_bytes = 0, late = 0, dup = 0, lbrr = 0, ok = 0, fec = 0, plc = 0, lost = 0,
           expand = 0, underrun = 0, rebuffer = 0, drop = 0, flushed = 0, dec_err = 0, pulls = 0,
           silent_pulls = 0, resets = 0,
           skipped = 0, merged = 0;  // received frames dropped without being decoded (must stay 0)
};

class JitterBuffer {
 public:
  static constexpr int kSlots = 512;          // 5.12 s of 10 ms frames
  static constexpr int kMaxPull = 2048;       // larger pulls are split by the caller
  static constexpr int kMaxDecode = 5760;     // 120 ms at 48 kHz
  static constexpr double kSpikeMs = 150.0;   // transit excess treated as an outage

  explicit JitterBuffer(const JbConfig& cfg);
  bool init(std::string* err);  // creates the decoder (not real-time)

  void push(const MediaFrame& p);
  // out: n frames x channels. now_s: monotonic seconds of the pull (same clock as arrival_ns).
  void pull(float* out, int n, double now_s);

  double level_ms() const;
  double target_ms() const { return target_ms_; }
  double ppm() const { return drift_.ppm; }
  double integ_ppm() const { return drift_.integ; }
  double last_level_ms() const { return last_level_; }  // level measured before the last pull
  // Clock ratio measured from timestamps alone (sender ts vs arrival, frames pulled vs pull time):
  // the long-run resampling ppm the drift control has to apply. Valid (and then followed by the drift
  // integral) once it spans 60 s with a standard error <= 5 ppm.
  double drift_est_ppm() const { return est_ppm_; }
  bool drift_est_valid() const { return est_valid_; }
  double drift_est_se_ppm() const { return est_se_ppm_; }
  bool playing() const { return playing_; }
  int channels() const { return cfg_.channels; }
  int frame() const { return frame_; }
  uint32_t newest_ts32() const { return last_ts32_; }
  double jitter_ms(double now = -1);
  const JbCounters& counters() const { return c_; }

 private:
  struct Slot {
    int64_t ts = 0;
    bool used = false;
    uint8_t flags = 0;
    uint16_t nsamp = 0;
    uint16_t len = 0;
    uint8_t data[kMaxPayload];
  };

  int64_t unwrap(uint32_t ts32);
  Slot* get(int64_t ts);
  bool slot_index(int64_t ts, int* idx) const;
  bool min_ts(int64_t* out) const;
  void reset_timeline();
  int clear_slots();  // returns the number of undecoded frames thrown away
  void store(int64_t ts, const MediaFrame& p);
  int decode_next(float* out);
  void decode_discard(Slot* s);
  bool start_if_ready();
  void flush_to_target();
  void control(double now, double block_s);
  double pull_jitter_ms() const;
  void est_pull(double now, int n);
  void est_fit();

  JbConfig cfg_;
  OpusDec dec_;
  Resampler rs_;
  std::unique_ptr<Slot[]> slots_;
  std::unique_ptr<float[]> scratch_;
  JbCounters c_;
  DriftController drift_;

  int frame_;
  int block_;
  bool has_base_ = false;
  int64_t base_ = 0;
  bool has_next_ = false;
  int64_t next_ts_ = 0;
  bool has_last32_ = false;
  uint32_t last_ts32_ = 0;
  int64_t ts_hi_ = 0;
  bool has_max_ = false;
  int64_t max_ts_ = 0;
  bool playing_ = false;
  int expand_run_ = 0;
  int expand_credit_ = 0;  // expansion frames played at the current next_ts_ (merge_expand)
  double target_ms_;
  bool has_avg_ = false;
  double level_avg_ = 0;
  double last_level_ = 0;
  bool over_ = false;
  double over_since_ = 0;
  int64_t pulled_ = 0;

  // transit window (arrival s, transit s), ring
  static constexpr int kTransitCap = 4096;
  std::unique_ptr<double[]> tr_arr_, tr_val_, tr_tmp_;
  int tr_head_ = 0, tr_n_ = 0;
  bool jit_dirty_ = true;
  double jit_value_ = 10.0;
  double jit_cache_ = 10.0;
  double jit_time_ = -1e9;
  bool has_tmin_ = false;
  double tmin_ = 0, tmin_t_ = 0;
  // peaks (time, excess ms), ring
  static constexpr int kPeakCap = 64;
  double pk_t_[kPeakCap], pk_ex_[kPeakCap];
  int pk_head_ = 0, pk_n_ = 0;
  // clock-ratio estimate: per 1 s of pull time, the lowest transit (arrival - ts/48k) and the lowest
  // pull lag (now - pulled/48k); the slope of (lag - transit) is the ratio of the two clocks.
  static constexpr int kEstCap = 600;  // 10 min of 1 s points
  int64_t est_bucket_ = 0;
  bool est_has_bucket_ = false, est_has_tr_ = false, est_has_pl_ = false, est_valid_ = false;
  double est_tr_min_ = 0, est_pl_min_ = 0, est_ppm_ = 0;
  double est_t_[kEstCap], est_d_[kEstCap];
  int est_seg_[kEstCap];
  int est_head_ = 0, est_n_ = 0, est_cur_seg_ = 0;
  // est_fit scratch, per segment in ring order: kept points, their sums of t and d, the offset
  double est_sn_[kEstCap], est_st_[kEstCap], est_sd_[kEstCap], est_icpts_[kEstCap] = {};
  // last fit's slope for the step check, and how many points in a row missed it by more than kEstStep
  static constexpr double kEstStep = 0.015;
  static constexpr int kEstStepPoints = 20;
  static constexpr double kEstMinSpanS = 60.0, kEstMaxSePpm = 5.0;
  double est_se_ppm_ = 1e9;
  bool est_ever_ = false;  // a valid estimate was seen: the integral is held from then on
  bool est_has_fit_ = false;
  double est_slope_ = 0;
  int est_miss_ = 0;
  void est_add(double t, double d);
  // pull-clock deviations (time, deviation s), ring
  std::unique_ptr<double[]> pl_t_, pl_d_;
  int pl_head_ = 0, pl_n_ = 0;
};

}  // namespace beamer::audio
