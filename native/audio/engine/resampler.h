// SPDX-License-Identifier: GPL-3.0-only
// Variable-ratio windowed-sinc interpolator, port of proto/transport/media.py `Resampler`:
// 16 taps, 512 phases, Kaiser beta 8, cutoff 0.97 x Nyquist, rows normalised to unity DC gain.
// step = input samples consumed per output sample (1 + ppm * 1e-6). Allocation-free after
// construction; single-threaded (used on the render IO thread).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace beamer::audio {

class Resampler {
 public:
  static constexpr int kTaps = 16;
  static constexpr int kPhases = 512;

  // max_frames: largest input backlog it must hold (feed + history).
  Resampler(int channels, int max_frames);

  int channels() const { return ch_; }
  // Input samples buffered beyond the read position (fractional).
  double available() const { return static_cast<double>(len_) - kTaps / 2 - pos_; }
  // Input frames still needed to produce n outputs at this step.
  int need(int n, double step) const;
  // Appends n input frames (interleaved). Returns false (and appends nothing) when full.
  bool feed(const float* x, int n);
  bool feed_zeros(int n);
  // Produces n output frames; requires need(n, step) == 0.
  void out(float* y, int n, double step);
  void reset();
  // Absolute input position of the next output, in input samples since construction/reset
  // (tests: ideal fractional-delay reference).
  double abs_pos() const { return static_cast<double>(dropped_) + pos_ - kTaps; }

 private:
  static const float* table();
  const int ch_;
  const int cap_;
  std::unique_ptr<float[]> buf_;
  int len_ = 0;        // frames in buf_
  double pos_ = 0;     // fractional read position into buf_
  int64_t dropped_ = 0;
};

// PI drift controller on (level - target), from JitterBuffer._control: P 40 ppm/ms, integral
// 0.1 ppm/(ms.s) clamped to +-200 ppm, total +-max_ppm; catch-up (speed-up only) up to
// catchup_ppm when more than 4 ms above target.
// hold_integ: the integral is set from outside (a measured clock ratio) and not integrated.
struct DriftController {
  double max_ppm = 500, catchup_ppm = 2000;
  double integ = 0, ppm = 0;
  bool hold_integ = false;
  double update(double err_ms, double block_s);
  void reset() { integ = ppm = 0; }
};

}  // namespace beamer::audio
