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

// Lip-sync hold (§4.9): the policy is CW10's. CW2 provides the data and this hook; the hold is 0 unless a dev
// override (`sync_offset_ms`) is set.
class LipSyncHold {
 public:
  void set_override_ms(double v) { override_ms_.store(v); }
  double hold_ms() const {
    const double o = override_ms_.load();
    return o > 0 ? o : 0.0;
  }

 private:
  std::atomic<double> override_ms_{0};
};

// Audio/video present delays for lip sync (§4.9): now - the sender capture time of what is presented, mapped
// with the flow offset, plus the presentation path's latency. ms.
inline double present_delay_ms(int64_t now, int64_t sender_capture_ns, int64_t offset_ns, double path_ms) {
  return double(now - (sender_capture_ns - offset_ns)) * 1e-6 + path_ms;
}

}  // namespace beamer::audio
