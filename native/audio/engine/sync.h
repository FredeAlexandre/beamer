// SPDX-License-Identifier: GPL-3.0-only
// Timing shared by the video receiver and lip sync (camera PHASE2 §4.5.3, §4.9): the flow's clock offset from
// ping/pong, the time map (ts -> sender clock), the adaptive playout target D, and the lip-sync hold hook.
//
// Sign convention (§4.5.3, used everywhere): on the receiver T, offset = S's clock minus T's clock. A sender
// time x maps to the receiver clock as x - offset; a receiver time y maps to the sender clock as y + offset.
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <vector>

namespace beamer::audio {

// Lock-free (ts, ns) pair published by one thread, read by others (seqlock over two atomics).
class PairPublisher {
 public:
  void publish(uint32_t ts, int64_t ns) {
    const uint32_t s = seq_.load(std::memory_order_relaxed);
    seq_.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    ts_.store(ts, std::memory_order_relaxed);
    ns_.store(ns, std::memory_order_relaxed);
    seq_.store(s + 2, std::memory_order_release);
  }
  // false: nothing published yet
  bool read(uint32_t* ts, int64_t* ns) const {
    for (int i = 0; i < 64; ++i) {
      const uint32_t s1 = seq_.load(std::memory_order_acquire);
      if (s1 == 0) return false;
      if (s1 & 1) continue;
      const uint32_t t = ts_.load(std::memory_order_relaxed);
      const int64_t n = ns_.load(std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_acquire);
      if (seq_.load(std::memory_order_relaxed) == s1) {
        *ts = t;
        *ns = n;
        return true;
      }
    }
    return false;
  }

 private:
  std::atomic<uint32_t> seq_{0};
  std::atomic<uint32_t> ts_{0};
  std::atomic<int64_t> ns_{0};
};

// Clock offset from pong samples (§4.5.3): offset = ((t1 - t0) + (t2 - t3)) / 2 (peer minus ours); keeps the
// sample with the lowest RTT among the last 10. Written by the network thread, read anywhere.
class OffsetEstimator {
 public:
  void add(int64_t t0, int64_t t1, int64_t t2, int64_t t3);
  // false: no sample yet. age_ns: now - receive time of the chosen sample.
  bool get(int64_t now, int64_t* offset_ns, int64_t* age_ns, int64_t* rtt_ns = nullptr) const;
  bool valid() const { return valid_.load(std::memory_order_acquire); }

 private:
  struct Sample {
    int64_t offset, rtt, at;
  };
  std::deque<Sample> samples_;  // network thread only
  std::atomic<bool> valid_{false};
  std::atomic<int64_t> offset_{0}, rtt_{0}, at_{0};
  mutable std::mutex mu_;  // keeps the three atomics coherent for readers
};

// The newest time-map entry of one stream: maps a ts (modulo 2^32, `rate` Hz) to the sender clock.
class TimeMapBook {
 public:
  explicit TimeMapBook(int rate_hz = 90000) : rate_(rate_hz) {}
  void set(uint32_t ts, uint64_t sender_ns);
  bool valid() const { return valid_; }
  int64_t to_sender_ns(uint32_t ts) const;  // only when valid()
  int entries() const { return n_; }

 private:
  int rate_;
  bool valid_ = false;
  uint32_t ts_ = 0;
  int64_t ns_ = 0;
  int n_ = 0;
};

// Adaptive playout target D (§4.5.3): p99 of d over the last 10 s + 8 ms, clamped to [floor, 150] ms, rises at
// once, falls by at most 15 ms per 0.5 s; 70 ms until 20 samples exist. Times in ns, D in ms.
class PlayoutTarget {
 public:
  static constexpr double kInitialMs = 70, kCeilMs = 150, kMarginMs = 8;
  static constexpr int kMinSamples = 20;
  void set_floor_ms(double f) { floor_ms_ = f; }
  double floor_ms() const { return floor_ms_; }
  void add(int64_t now, double d_ms);
  double target_ms(int64_t now);  // applies the fall rule
  double p99_ms() const { return p99_; }
  size_t samples() const { return win_.size(); }

 private:
  void recompute(int64_t now);
  std::deque<std::pair<int64_t, double>> win_;
  double floor_ms_ = 50, d_ms_ = kInitialMs, p99_ = 0;
  int64_t last_ns_ = 0;
  bool have_ = false;
  int64_t total_ = 0;
};

// Audio/video present delays for lip sync (§4.9): now - the sender capture time of what is presented, mapped
// with the flow offset, plus the presentation path's latency. ms.
inline double present_delay_ms(int64_t now, int64_t sender_capture_ns, int64_t offset_ns, double path_ms) {
  return double(now - (sender_capture_ns - offset_ns)) * 1e-6 + path_ms;
}

// ===================================================================== lip sync (§4.9, CW10)
//
// On a machine T that receives both the active mic stream from S (into its virtual mic) and a video stream from S
// (same flow: same sender clock, same offset estimate), the video presenter adds
//   hold = clamp(audio_present_delay - video_present_delay, 0, 150 ms),
// smoothed (EWMA 2 s, changes <= 10 ms per second), applied only after both streams have run 2 s. Audio is never
// delayed; when video is the slower path (audio leads) the hold is 0 and the stats show the negative skew.
// Both delays are taken from the sender's capture time with the same offset value, so the offset's error cancels
// in the difference.

// audio_present_delay of an audio recv stream (ms): the sample with ts `render_ts` (`rate_hz` units) was handed to
// the device at `render_at` (receiver clock); the stream's time map maps `map_ts` to the sender time `map_ns`;
// `path_ms` is the latency from the render callback to what an app capturing the device hears (CABLE: below).
inline double audio_present_delay_ms(uint32_t render_ts, int64_t render_at, uint32_t map_ts, int64_t map_ns,
                                     int rate_hz, int64_t offset_ns, double path_ms) {
  const int64_t d = static_cast<int32_t>(render_ts - map_ts);  // modulo 2^32: +-12.4 h at 48 kHz
  const int64_t capture = map_ns + d * 1'000'000'000LL / rate_hz;
  return present_delay_ms(render_at, capture, offset_ns, path_ms);
}

// video_present_delay without the hold (ms): from capture, like the audio side: enc_delay + D + source delivery.
constexpr double kVideoSourceDeliveryMs = 2.0;
inline double video_present_delay_ms(double enc_delay_ms, double target_d_ms) {
  return enc_delay_ms + target_d_ms + kVideoSourceDeliveryMs;
}

// Render path latency of the virtual mic on Windows (VB-CABLE): our render callback -> what an app capturing
// CABLE Output receives, per mode. Measured by audio WP12 (click latency, lebig): shared at the low-latency
// 128-frame period x 3 queued 59-72 ms (300 s: 69.2 p50); shared at the default 480-frame period x 3 102-117 ms;
// exclusive (2 ms period) 35.9 ms (WP4: 43-45). The run-to-run spread (~+-10 ms) is the phase between the two
// CABLE engines; the dev key `sync_audio_path_ms` overrides these for measurement.
enum class CableMode { SharedLowLatency, SharedDefault, Exclusive };
inline double cable_render_path_ms(CableMode m) {
  switch (m) {
    case CableMode::SharedLowLatency: return 67.0;
    case CableMode::SharedDefault: return 110.0;
    case CableMode::Exclusive: return 38.0;
  }
  return 0.0;
}

// What the policy sees at one instant (ms). `audio`: the active mic stream from the same sender as the video
// renders into the virtual mic, with a time map (`sr`) and a current render position; `video`: the video stream
// is live with a time map and an offset.
struct LipSyncInput {
  bool audio = false;
  double audio_ms = 0;  // audio_present_delay
  bool video = false;
  double video_ms = 0;  // video_present_delay without the hold
};

// The pure policy (explicit time; one per video recv stream, driven by the helper's 50 ms tick).
class LipSyncPolicy {
 public:
  static constexpr double kMaxHoldMs = 150.0;
  static constexpr double kTauS = 2.0;                   // EWMA time constant
  static constexpr double kSlewMsPerS = 10.0;            // the applied hold changes by at most this per second
  static constexpr int64_t kWarmupNs = 2'000'000'000LL;  // both streams must have run this long
  // Feeds one observation and returns the hold to apply now (ms).
  double update(int64_t now, const LipSyncInput& in);
  double hold_ms() const { return hold_; }
  double target_ms() const;                // clamp(smoothed difference, 0, 150) while active, else 0
  bool active() const { return active_; }  // both streams present and run >= 2 s
  // While both inputs are present: smoothed audio - video without the hold (diff), and the skew left after the
  // hold, audio - (video + hold): positive = audio later; negative = audio leads (video the slower path).
  bool have_skew() const { return have_diff_; }
  double diff_ms() const { return diff_; }
  double skew_ms() const { return diff_ - hold_; }

 private:
  int64_t last_ = 0;
  bool have_last_ = false;
  int64_t audio_since_ = 0, video_since_ = 0;
  bool audio_on_ = false, video_on_ = false;
  bool active_ = false;
  bool have_diff_ = false;
  double diff_ = 0;  // EWMA of audio_ms - video_ms
  double hold_ = 0;
};

// The hold read by the video presenter (any thread): the policy's value, or the dev override `sync_offset_ms`
// (>= 0; < 0 = none). Also carries what the stats show (NaN = unknown).
class LipSyncHold {
 public:
  static constexpr double kNone = std::numeric_limits<double>::quiet_NaN();
  void set_override_ms(double v) { override_ms_.store(v); }
  double override_ms() const { return override_ms_.load(); }
  bool overridden() const { return override_ms_.load() >= 0; }
  void set_policy(double hold_ms, double skew_ms, double audio_ms, double path_ms) {
    policy_ms_.store(hold_ms);
    skew_ms_.store(skew_ms);
    audio_ms_.store(audio_ms);
    path_ms_.store(path_ms);
  }
  double hold_ms() const {
    const double o = override_ms_.load();
    return o >= 0 ? o : policy_ms_.load();
  }
  double policy_ms() const { return policy_ms_.load(); }
  double skew_ms() const { return skew_ms_.load(); }
  double audio_ms() const { return audio_ms_.load(); }
  double path_ms() const { return path_ms_.load(); }

 private:
  std::atomic<double> override_ms_{-1.0};
  std::atomic<double> policy_ms_{0.0};
  std::atomic<double> skew_ms_{kNone}, audio_ms_{kNone}, path_ms_{kNone};
};

}  // namespace beamer::audio
