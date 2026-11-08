// SPDX-License-Identifier: GPL-3.0-only
// Level metering: the IO thread posts one record per callback into an SPSC ring; the control
// thread drains it and reports RMS/peak in dBFS (0.1 dB, floor -100).
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

#include "spsc.h"

namespace beamer::audio {

struct LevelRecord {
  float sumsq = 0;
  float peak = 0;
  uint32_t frames = 0;
};

inline LevelRecord measure(const float* x, int frames, int channels) {
  LevelRecord r;
  const int n = frames * channels;
  float s = 0, p = 0;
  for (int i = 0; i < n; ++i) {
    float v = x[i];
    s += v * v;
    float a = v < 0 ? -v : v;
    if (a > p) p = a;
  }
  r.sumsq = channels > 0 ? s / channels : 0;  // sum over frames of the mean channel power
  r.peak = p;
  r.frames = static_cast<uint32_t>(frames);
  return r;
}

inline double to_db(double power_or_amp, bool power) {
  if (power_or_amp <= 0) return -100.0;
  double db = power ? 10.0 * std::log10(power_or_amp) : 20.0 * std::log10(power_or_amp);
  if (db < -100.0) db = -100.0;
  return std::round(db * 10.0) / 10.0;
}

class Meter {
 public:
  Meter() : ring_(1024) {}
  // IO thread
  void post(const float* x, int frames, int channels) {
    LevelRecord r = measure(x, frames, channels);
    ring_.push(r);  // drops when the control thread is not draining (meter off)
    if (r.peak > 0) nonzero_.store(1, std::memory_order_relaxed);
    frames_total_.fetch_add(static_cast<uint64_t>(frames), std::memory_order_relaxed);
  }
  // control thread: accumulate everything posted since the last call
  void drain(double* sumsq, double* peak, uint64_t* frames) {
    LevelRecord r;
    while (ring_.pop(&r)) {
      *sumsq += static_cast<double>(r.sumsq);  // r.sumsq: sum over frames of the per-frame power
      if (r.peak > *peak) *peak = r.peak;
      *frames += r.frames;
    }
  }
  // true once if any non-zero sample arrived since the last call
  bool take_nonzero() { return nonzero_.exchange(0, std::memory_order_relaxed) != 0; }
  uint64_t frames_total() const { return frames_total_.load(std::memory_order_relaxed); }

 private:
  SpscRing<LevelRecord> ring_;
  std::atomic<int> nonzero_{0};
  std::atomic<uint64_t> frames_total_{0};
};

}  // namespace beamer::audio
