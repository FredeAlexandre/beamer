// SPDX-License-Identifier: GPL-3.0-only
// Echo cancellation (PHASE2 §7): on the machine that captures the forwarded mic, the sound its
// output device plays (the "reference", a CaptureKind::Reference capture) is removed from the mic
// between capture and the encoder.
//
// - EchoCanceller: one 10 ms mono frame at a time; speexdsp 1.2.1 MDF (100 ms filter) + the
//   preprocessor's residual echo suppression (no denoise, AGC, VAD or dereverb). An interface so
//   that WebRTC AEC3 can replace it later.
// - HostClock: sample index <-> backend host time (host_ns of each capture callback, smoothed).
// - AecPath: everything a send stream needs: the reference capture (IO thread side: SPSC ring +
//   time marks), the alignment of the reference to the mic by host timestamps (coarse delay), the
//   drift correction (the engine's Resampler, steered by a PI loop on the position error, keeps the
//   reference read position on the mic's timeline when the two devices run on different clocks),
//   and the canceller. Runs on
//   the send stream's encoder thread.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine.h"
#include "json.h"
#include "resampler.h"
#include "spsc.h"
#include "util.h"

namespace beamer::audio {

// True when the canceller is compiled into this engine (the `aec` feature word also needs a
// backend that can capture a reference: see engine_features()).
constexpr bool kAecCompiled = true;
constexpr int kAecFrame = 480;   // 10 ms at 48 kHz
constexpr int kAecTail = 4800;   // filter length, 100 ms

struct EchoConfig {
  int rate = kRate;
  int frame = kAecFrame;
  int tail = kAecTail;
  bool residual = true;           // preprocessor residual echo suppression
  int echo_suppress_db = -40;     // RES attenuation of residual echo (far end only)
  int echo_suppress_active_db = -15;  // ... while the near end talks (double talk)
};

class EchoCanceller {
 public:
  virtual ~EchoCanceller() = default;
  // mic: near end + echo; ref: what the speakers play, aligned so that its echo lags it by
  // 0..tail; out: mic without the echo. `frame` mono floats each (full scale +-1); out may be mic.
  virtual void process(const float* mic, const float* ref, float* out) = 0;
  virtual void reset() = 0;
  // Delay of the strongest tap of the adapted filter, ms (-1 before it has adapted). Not cheap
  // (one inverse FFT per filter block): call it about once per second.
  virtual double echo_delay_ms() = 0;
};

std::unique_ptr<EchoCanceller> make_echo_canceller(const EchoConfig& cfg = EchoConfig());

// Engine-level feature words: the backend's list, where "aec" (= the backend can capture a
// reference) is kept only when the canceller is compiled in.
std::vector<std::string> engine_features(Backend& backend);

// Sample index -> host time of one capture stream, from (index of a callback's first sample,
// host_ns of that callback). The offset t - index/rate is smoothed (tau ~0.5 s at 10 ms callbacks);
// a jump of more than 20 ms (stream restarted, samples dropped) restarts it.
class HostClock {
 public:
  void add(uint64_t index, uint64_t host_ns);
  bool valid() const { return have_; }
  double time_ns(double index) const { return off_ + index * kNsPerSample; }
  double index_at(double t_ns) const { return (t_ns - off_) / kNsPerSample; }
  uint64_t last_host_ns() const { return last_ns_; }
  int jumps() const { return jumps_; }
  void reset() { have_ = false; }

  static constexpr double kNsPerSample = 1e9 / kRate;

 private:
  bool have_ = false;
  double off_ = 0;
  uint64_t last_ns_ = 0;
  int jumps_ = 0;
};

struct ClockMark {
  uint64_t index;    // samples written to the ring before this callback
  uint64_t host_ns;  // backend host time of that callback's first sample
};

struct AecOptions {
  std::string reference;            // output device key, "" = the default output (followed)
  // The reference sample fed with mic sample t is the one captured at t + lead_ms (host time), so
  // the filter sees the echo at (acoustic delay by timestamps) + lead_ms; it must stay within
  // 0..tail. A positive lead makes mic frames wait longer for their reference; -1 ms lets a mic
  // block go as soon as the reference block of the same instant is in (the resampler looks 9
  // samples ahead).
  double lead_ms = -1;
  double max_wait_ms = 30;          // a mic frame waits at most this long for its reference
  std::vector<int64_t> exclude_pids;  // dev only: processes left out of the reference (tests)
  EchoConfig echo;
};

class AecPath : public CaptureSink {
 public:
  explicit AecPath(const AecOptions& opt);
  ~AecPath() override;
  AecPath(const AecPath&) = delete;
  AecPath& operator=(const AecPath&) = delete;

  // Main thread. Opens the reference capture; without it the path passes the mic through the
  // canceller with a silent reference (and stats say why).
  bool open_reference(Backend& backend, std::string* err);
  void close_reference();
  bool reference_open() const { return ref_handle_ != nullptr; }
  // (Tests may also feed the reference by calling on_capture() directly.)
  // The consumer's wake-up signal (rung when reference samples arrive while it waits).
  void set_wake(Signal* s) { wake_ = s; }

  // Mic IO thread: host time of mic sample `index` (count of mono/codec frames the send stream
  // wrote to its ring before this block).
  void mic_mark(uint64_t index, uint64_t host_ns) { mic_marks_.push(ClockMark{index, host_ns}); }

  // Reference IO thread (CaptureSink).
  void on_capture(const float* interleaved, int frames, int channels, uint64_t host_ns) override;

  // Consumer thread. ready(): may the mic frame that starts at mic index `m` be processed now?
  // `mic_backlog` = mic frames buffered beyond this one. False = wait for the reference.
  bool ready(uint64_t m, size_t mic_backlog);
  // Processes one kAecFrame mono frame that starts at mic index `m` (out may equal mic).
  void process(const float* mic, uint64_t m, float* out);

  // Control thread, once per second.
  Json stats(double dt_s);

 private:
  void drain_marks();
  void align(uint64_t m);              // consumer: resync / drift step for the frame at m
  void fill_reference(float* ref, int n);

  AecOptions opt_;
  std::unique_ptr<EchoCanceller> ec_;
  std::unique_ptr<StreamHandle> ref_handle_;
  Signal* wake_ = nullptr;
  std::string ref_error_;
  std::atomic<bool> has_ref_{false};  // read by the consumer thread

  // reference IO thread
  SpscFloatRing ref_ring_;
  SpscRing<ClockMark> ref_marks_;
  SpscRing<ClockMark> mic_marks_;
  std::unique_ptr<float[]> ref_conv_;
  uint64_t ref_written_ = 0;
  std::atomic<bool> waiting_{false};

  // consumer thread
  HostClock mic_clk_, ref_clk_;
  Resampler rs_;
  double integ_ = 0;      // alignment loop integrator, ppm
  std::unique_ptr<float[]> tmp_, ref_frame_;
  bool started_ = false;
  int64_t ring_r_ = 0;    // reference stream index at the ring's read position
  int64_t next_in_ = 0;   // reference stream index of the next resampler input
  int64_t base_ = 0;      // reference stream index of resampler input 0 (since the last resync)
  double step_ = 1.0;
  bool plan_zero_ = false;  // this frame runs with a silent reference (none or stalled)
  bool aligned_once_ = false;
  uint64_t aligned_m_ = 0;  // mic index of the frame the alignment was last computed for
  uint64_t frames_since_delay_ = 0;

  // consumer -> control thread
  std::atomic<uint64_t> frames_{0}, resyncs_{0}, ref_short_{0}, waits_{0}, no_ref_{0}, proc_ns_{0};
  std::atomic<double> e_in_{0}, e_out_{0}, e_ref_{0}, err_ms_{0}, ppm_{0}, delay_ms_{-1};
  uint64_t p_frames_ = 0, p_proc_ns_ = 0;
  double p_e_in_ = 0, p_e_out_ = 0, p_e_ref_ = 0;
};

}  // namespace beamer::audio
