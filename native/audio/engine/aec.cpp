// SPDX-License-Identifier: GPL-3.0-only
#include "aec.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "speex/speex_echo.h"
#include "speex/speex_preprocess.h"
#include "streams.h"  // map_channels

namespace beamer::audio {

namespace {

constexpr double kJumpNs = 20e6;          // HostClock: a larger offset jump restarts the estimate
constexpr double kClockAlpha = 0.02;      // HostClock smoothing per callback
constexpr double kResyncMs = 20;          // alignment error beyond which the reference jumps
constexpr double kStallNs = 100e6;        // no reference callback for this long = no reference
constexpr double kKp = 400, kKi = 40;    // alignment loop: ppm per ms, ppm per ms.s
constexpr double kMaxPpm = 2000, kMaxIntegPpm = 1000;

inline int16_t to_i16(float v) {
  float s = v * 32768.0f;
  if (s > 32767.0f) return 32767;
  if (s < -32768.0f) return -32768;
  return static_cast<int16_t>(std::lrintf(s));
}

class SpeexCanceller final : public EchoCanceller {
 public:
  explicit SpeexCanceller(const EchoConfig& c)
      : n_(c.frame), rate_(c.rate), mic_(c.frame), ref_(c.frame), out_(c.frame) {
    st_ = speex_echo_state_init(c.frame, c.tail);
    int rate = c.rate;
    speex_echo_ctl(st_, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
    int ir = 0;
    speex_echo_ctl(st_, SPEEX_ECHO_GET_IMPULSE_RESPONSE_SIZE, &ir);
    ir_.assign(size_t(std::max(ir, 0)), 0);
    if (c.residual) {
      pp_ = speex_preprocess_state_init(c.frame, c.rate);
      // Residual echo suppression only: the preprocessor's gain stage must run (denoise on), but
      // with a 0 dB noise floor it leaves noise and speech alone; no AGC or dereverb (VAD is off by
      // default; setting it prints a warning).
      int on = 1, off = 0, zero = 0;
      int sup = c.echo_suppress_db, act = c.echo_suppress_active_db;
      speex_preprocess_ctl(pp_, SPEEX_PREPROCESS_SET_DENOISE, &on);
      speex_preprocess_ctl(pp_, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &zero);
      speex_preprocess_ctl(pp_, SPEEX_PREPROCESS_SET_AGC, &off);
      speex_preprocess_ctl(pp_, SPEEX_PREPROCESS_SET_DEREVERB, &off);
      speex_preprocess_ctl(pp_, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS, &sup);
      speex_preprocess_ctl(pp_, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS_ACTIVE, &act);
      speex_preprocess_ctl(pp_, SPEEX_PREPROCESS_SET_ECHO_STATE, st_);
    }
  }
  ~SpeexCanceller() override {
    if (pp_) speex_preprocess_state_destroy(pp_);
    speex_echo_state_destroy(st_);
  }

  void process(const float* mic, const float* ref, float* out) override {
    for (int i = 0; i < n_; ++i) {
      mic_[i] = to_i16(mic[i]);
      ref_[i] = to_i16(ref[i]);
    }
    speex_echo_cancellation(st_, mic_.data(), ref_.data(), out_.data());
    if (pp_) speex_preprocess_run(pp_, out_.data());
    constexpr float k = 1.0f / 32768.0f;
    for (int i = 0; i < n_; ++i) out[i] = out_[i] * k;
  }

  void reset() override { speex_echo_state_reset(st_); }  // the preprocessor re-adapts by itself

  double echo_delay_ms() override {
    if (ir_.empty()) return -1;
    speex_echo_ctl(st_, SPEEX_ECHO_GET_IMPULSE_RESPONSE, ir_.data());
    size_t best = 0;
    int64_t peak = 0;
    for (size_t i = 0; i < ir_.size(); ++i) {
      int64_t a = ir_[i] < 0 ? -int64_t(ir_[i]) : int64_t(ir_[i]);
      if (a > peak) {
        peak = a;
        best = i;
      }
    }
    return peak > 0 ? double(best) * 1000.0 / rate_ : -1;
  }

 private:
  int n_, rate_;
  SpeexEchoState* st_ = nullptr;
  SpeexPreprocessState* pp_ = nullptr;
  std::vector<spx_int16_t> mic_, ref_, out_;
  std::vector<spx_int32_t> ir_;
};

}  // namespace

std::unique_ptr<EchoCanceller> make_echo_canceller(const EchoConfig& cfg) {
  return std::make_unique<SpeexCanceller>(cfg);
}

std::vector<std::string> engine_features(Backend& backend) {
  std::vector<std::string> f = backend.features();
  if (!kAecCompiled) f.erase(std::remove(f.begin(), f.end(), std::string("aec")), f.end());
  return f;
}

// ------------------------------------------------------------------ HostClock

void HostClock::add(uint64_t index, uint64_t host_ns) {
  if (!host_ns) return;
  const double obs = double(host_ns) - double(index) * kNsPerSample;
  if (!have_) {
    off_ = obs;
    have_ = true;
  } else {
    const double d = obs - off_;
    if (std::fabs(d) > kJumpNs) {
      off_ = obs;
      ++jumps_;
    } else {
      off_ += kClockAlpha * d;
    }
  }
  last_ns_ = host_ns;
}

// ------------------------------------------------------------------ AecPath

AecPath::AecPath(const AecOptions& opt)
    : opt_(opt),
      ec_(make_echo_canceller(opt.echo)),
      ref_ring_(kRate, 1),
      ref_marks_(256),
      mic_marks_(256),
      ref_conv_(new float[4096]),
      rs_(1, 8192),
      tmp_(new float[8192]),
      ref_frame_(new float[kAecFrame]) {}

AecPath::~AecPath() { close_reference(); }

bool AecPath::open_reference(Backend& backend, std::string* err) {
  std::string e;
  CaptureSpec spec{CaptureKind::Reference, opt_.reference, 1, false, opt_.exclude_pids};
  ref_handle_ = backend.open_capture(spec, this, &e);
  if (!ref_handle_) {
    ref_error_ = e.empty() ? std::string("internal") : e;
    if (err) *err = ref_error_;
    return false;
  }
  ref_error_.clear();
  has_ref_.store(true);
  return true;
}

void AecPath::close_reference() {  // the backend stops calling on_capture before reset() returns
  has_ref_.store(false);
  ref_handle_.reset();
}

void AecPath::on_capture(const float* x, int frames, int channels, uint64_t host_ns) {
  if (frames <= 0 || channels <= 0) return;
  ref_marks_.push(ClockMark{ref_written_, host_ns});
  for (int off = 0; off < frames;) {
    const int k = std::min(4096, frames - off);
    map_channels(x + size_t(off) * channels, channels, ref_conv_.get(), 1, k);
    ref_written_ += ref_ring_.write(ref_conv_.get(), size_t(k));
    off += k;
  }
  if (wake_ && waiting_.load(std::memory_order_relaxed)) wake_->signal();
}

void AecPath::drain_marks() {
  ClockMark m;
  while (mic_marks_.pop(&m)) mic_clk_.add(m.index, m.host_ns);
  while (ref_marks_.pop(&m)) ref_clk_.add(m.index, m.host_ns);
}

void AecPath::align(uint64_t m) {
  const double want = ref_clk_.index_at(mic_clk_.time_ns(double(m)) + opt_.lead_ms * 1e6);
  const double cur = double(base_) + rs_.abs_pos();
  const double err = want - cur;  // > 0: the reference read position is behind the mic
  if (!started_ || std::fabs(err) > kResyncMs * kRate / 1000.0) {
    // Jump: the next resampler output is reference sample round(want). After a reset the next
    // output is input index -(kTaps/2 + 1) (zero history), so input 0 is that much later.
    rs_.reset();
    step_ = 1.0;  // the integrator keeps the clock ratio learnt so far
    base_ = std::llround(want) + Resampler::kTaps / 2 + 1;
    next_in_ = base_;
    started_ = true;
    resyncs_.fetch_add(1, std::memory_order_relaxed);
    err_ms_.store(0, std::memory_order_relaxed);
    return;
  }
  // PI on the position error (ms). The error moves at (true ppm - applied ppm) / 1000 ms per second,
  // so these gains give a second-order loop with wn 0.5 rad/s, zeta 0.7: a 100 ppm clock offset is
  // locked in a few seconds with no steady error (the jitter buffer's DriftController, tuned for
  // buffer levels, would take ~25 s and leave 2.5 ms).
  const double err_ms = err * 1000.0 / kRate, dt = double(kAecFrame) / kRate;
  integ_ = std::max(-kMaxIntegPpm, std::min(kMaxIntegPpm, integ_ + kKi * err_ms * dt));
  const double ppm = std::max(-kMaxPpm, std::min(kMaxPpm, kKp * err_ms + integ_));
  step_ = 1.0 + ppm * 1e-6;
  err_ms_.store(err_ms, std::memory_order_relaxed);
  ppm_.store(ppm, std::memory_order_relaxed);
}

bool AecPath::ready(uint64_t m, size_t mic_backlog) {
  drain_marks();
  waiting_.store(false, std::memory_order_relaxed);
  plan_zero_ = true;
  if (!has_ref_.load(std::memory_order_relaxed) && !ref_clk_.valid()) return true;  // no reference capture
  if (!mic_clk_.valid() || !ref_clk_.valid()) return true;
  const double t_m = mic_clk_.time_ns(double(m));
  if (t_m - double(ref_clk_.last_host_ns()) > kStallNs) {  // reference stalled or gone
    started_ = false;
    return true;
  }
  if (!aligned_once_ || m != aligned_m_) {
    align(m);
    aligned_m_ = m;
    aligned_once_ = true;
  }
  plan_zero_ = false;
  const int64_t need_end = next_in_ + rs_.need(kAecFrame, step_);
  const int64_t have_end = ring_r_ + int64_t(ref_ring_.available());
  if (need_end <= have_end) return true;
  if (double(mic_backlog) * 1000.0 / kRate >= opt_.max_wait_ms) return true;  // late: zero-fill the rest
  waiting_.store(true, std::memory_order_relaxed);
  waits_.fetch_add(1, std::memory_order_relaxed);
  // re-check: a reference block may have landed between the test and the flag
  return ring_r_ + int64_t(ref_ring_.available()) >= need_end;
}

void AecPath::fill_reference(float* ref, int n) {
  const int need = rs_.need(n, step_);
  int left = need;
  bool short_ref = false;
  if (left > 0 && next_in_ < ring_r_) {  // history no longer in the ring: silence
    const int z = int(std::min<int64_t>(left, ring_r_ - next_in_));
    rs_.feed_zeros(z);
    next_in_ += z;
    left -= z;
    short_ref = true;
  }
  if (left > 0 && next_in_ > ring_r_) {  // stale reference: drop it
    ring_r_ += int64_t(ref_ring_.skip(size_t(next_in_ - ring_r_)));
  }
  if (left > 0 && next_in_ == ring_r_) {
    const int got = int(ref_ring_.read(tmp_.get(), size_t(left)));
    rs_.feed(tmp_.get(), got);
    ring_r_ += got;
    next_in_ += got;
    left -= got;
  }
  if (left > 0) {  // not captured (yet): silence; the late samples are skipped later
    rs_.feed_zeros(left);
    next_in_ += left;
    short_ref = true;
  }
  if (short_ref) ref_short_.fetch_add(1, std::memory_order_relaxed);
  rs_.out(ref, n, step_);
}

void AecPath::process(const float* mic, uint64_t m, float* out) {
  const int64_t t0 = now_ns();
  float* ref = ref_frame_.get();
  if (plan_zero_) {
    std::memset(ref, 0, sizeof(float) * kAecFrame);
    no_ref_.fetch_add(1, std::memory_order_relaxed);
  } else {
    if (!aligned_once_ || m != aligned_m_) {  // process() without ready(): align now
      align(m);
      aligned_m_ = m;
      aligned_once_ = true;
    }
    fill_reference(ref, kAecFrame);
  }
  double ein = 0;
  for (int i = 0; i < kAecFrame; ++i) ein += double(mic[i]) * mic[i];
  ec_->process(mic, ref, out);
  double eout = 0;
  for (int i = 0; i < kAecFrame; ++i) eout += double(out[i]) * out[i];
  e_in_.store(e_in_.load(std::memory_order_relaxed) + ein, std::memory_order_relaxed);
  e_out_.store(e_out_.load(std::memory_order_relaxed) + eout, std::memory_order_relaxed);
  if (++frames_since_delay_ >= 100) {
    frames_since_delay_ = 0;
    const double d = ec_->echo_delay_ms();
    delay_ms_.store(d < 0 ? -1.0 : d - opt_.lead_ms, std::memory_order_relaxed);
  }
  frames_.fetch_add(1, std::memory_order_relaxed);
  proc_ns_.fetch_add(uint64_t(now_ns() - t0), std::memory_order_relaxed);
}

Json AecPath::stats(double dt_s) {
  const uint64_t fr = frames_.load(), pn = proc_ns_.load();
  const double ein = e_in_.load(), eout = e_out_.load();
  const double din = ein - p_e_in_, dout = eout - p_e_out_;
  Json j{{"ref", ref_handle_ ? Json(ref_handle_->device_in_use()) : Json(nullptr)}};
  if (!ref_error_.empty()) j["error"] = error_code_of(ref_error_);
  // attenuation of the mic by the canceller over the interval (echo + whatever else it removed)
  j["atten_db"] = din > 0 && dout > 0 ? std::round(10.0 * std::log10(din / dout) * 10) / 10 : 0.0;
  j["echo_ms"] = std::round(delay_ms_.load() * 10) / 10;
  j["err_ms"] = std::round(err_ms_.load() * 100) / 100;
  j["ppm"] = std::round(ppm_.load() * 10) / 10;
  j["resync"] = resyncs_.load();
  j["ref_short"] = ref_short_.load();
  j["no_ref"] = no_ref_.load();
  j["waits"] = waits_.load();
  j["us"] = fr > p_frames_ ? std::round((pn - p_proc_ns_) / 100.0 / double(fr - p_frames_)) / 10 : 0.0;
  j["cpu_pct"] = dt_s > 0 ? std::round((pn - p_proc_ns_) / dt_s / 1e9 * 1e4) / 100 : 0.0;
  p_frames_ = fr;
  p_proc_ns_ = pn;
  p_e_in_ = ein;
  p_e_out_ = eout;
  return j;
}

}  // namespace beamer::audio
