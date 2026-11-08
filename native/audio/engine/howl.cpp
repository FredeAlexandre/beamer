// SPDX-License-Identifier: GPL-3.0-only
#include "howl.h"

#include <algorithm>
#include <cmath>

namespace beamer::audio {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kRateHz = 48000.0;
constexpr double kBinHz = kRateHz / HowlGuard::kFft;
constexpr int kMinBin = 3;    // 140 Hz
constexpr int kMaxBin = 341;  // 16 kHz
// Tones are looked for from 800 Hz. Below, a voice's first formant lifts one harmonic far above the
// rest, and a syllable that swells looks like feedback building up: WP16 saw 23 false ducks at
// 307-523 Hz on room speech (Wave Link's monitor in the PC's sound). Laptop and desk speakers hardly
// feed back down there (the one live howl was at 7.1 kHz), and 47 Hz bins cannot tell a chord from
// one line either.
constexpr int kMinPeakBin = 17;  // 797 Hz
constexpr int kHist = 216;    // frames of 10.7 ms: 2.3 s
constexpr int kWindows[] = {24, 72, 216};  // 0.26 s, 0.77 s, 2.3 s
constexpr int kMaxMisses = 4;
constexpr double kRecentS = 60;
}  // namespace

HowlGuard::HowlGuard(int channels, HowlConfig cfg)
    : channels_(channels > 0 ? channels : 1),
      cfg_(cfg),
      buf_(kFft, 0.0f),
      win_(kFft),
      re_(kFft),
      im_(kFft),
      cos_(kFft / 2),
      sin_(kFft / 2),
      power_(kFft / 2 + 1, 0.0f),
      rev_(kFft),
      hist_(kHist) {
  for (int n = 0; n < kFft; ++n) win_[n] = float(0.5 - 0.5 * std::cos(2 * kPi * n / kFft));
  for (int k = 0; k < kFft / 2; ++k) {
    cos_[k] = float(std::cos(2 * kPi * k / kFft));
    sin_[k] = float(-std::sin(2 * kPi * k / kFft));
  }
  int bits = 0;
  while ((1 << bits) < kFft) ++bits;
  for (int i = 0; i < kFft; ++i) {
    int r = 0;
    for (int b = 0; b < bits; ++b) r |= ((i >> b) & 1) << (bits - 1 - b);
    rev_[i] = r;
  }
  step_down_ = 1.0 / 48.0;  // 1 dB per ms: a 20 dB duck takes 20 ms
}

void HowlGuard::process(float* x, int frames) {
  const int C = channels_;
  for (int i = 0; i < frames; ++i) {
    float* s = x + size_t(i) * C;
    float m = 0;
    for (int c = 0; c < C; ++c) m += s[c];
    buf_[fill_++] = m / C;
    ++samples_;
    if (fill_ == kFft) {
      analyse();
      std::copy(buf_.begin() + kHop, buf_.end(), buf_.begin());
      fill_ = kFft - kHop;
    }
    if (gain_db_ != target_db_) {
      if (gain_db_ > target_db_) gain_db_ = std::max(target_db_, gain_db_ - step_down_);
      else gain_db_ = std::min(target_db_, gain_db_ + step_up_);
    }
    if (gain_db_ != 0) {
      const float g = float(std::pow(10.0, gain_db_ / 20.0));
      for (int c = 0; c < C; ++c) s[c] *= g;
    } else if (state_ == State::Releasing) {
      state_ = State::Idle;
      active_.store(false, std::memory_order_relaxed);
      duck_.store(0, std::memory_order_relaxed);
    }
  }
}

bool HowlGuard::other_lines(int bin, double freq, double tone) const {
  // Feedback grows one line; music carries several. Another line is a partial at f/2, 2f or 3f
  // within 20 dB of the tone (a local peak 4x its neighbours), or any other local peak in the band
  // within 15 dB of the tone's own peak bin (a chord, a voice, a bed of music under the tone).
  static constexpr double ratios[] = {0.5, 2.0, 3.0};
  for (double r : ratios) {
    const int k = int(std::lround(freq * r / kBinHz));
    if (k - 4 < kMinBin || k + 4 > kMaxBin) continue;
    const double peak = double(power_[k - 1]) + power_[k] + power_[k + 1];
    const double side = (double(power_[k - 4]) + power_[k - 3] + power_[k + 3] + power_[k + 4]) / 4.0;
    if (peak >= 0.01 * tone && peak / 3.0 >= 4.0 * side) return true;
  }
  const double near = power_[bin] * 0.0316;  // -15 dB
  for (int k = kMinBin + 1; k < kMaxBin; ++k) {
    if (std::abs(k - bin) <= 6) continue;  // the tone's own skirt (early feedback is a few lines wide)
    if (power_[k] >= near && power_[k] >= power_[k - 1] && power_[k] > power_[k + 1]) return true;
  }
  return false;
}

bool HowlGuard::ignored(int bin) const {
  for (const auto& e : ignore_)
    if (std::abs(e.first - bin) <= 2 && e.second > now_s()) return true;
  return false;
}

bool HowlGuard::rising() const {
  // Rising level at a stable frequency over one of the windows: the four quarter means never fall,
  // both halves carry part of the rise (an attack, a step and then flat, does not), the tone
  // dominates the band and has no partials at the end, and is loud enough.
  for (int W : kWindows) {
    if (hist_len_ < W) continue;
    const int q = W / 4;
    double mean[4] = {0, 0, 0, 0}, dom = 0;
    int harm = 0;
    for (int j = 0; j < W; ++j) {
      const Frame& f = hist_[(hist_pos_ - W + j + 2 * kHist) % kHist];
      mean[j / q] += f.level_db;
      if (j >= 3 * q) {
        dom += f.dominance;
        harm += f.lines ? 1 : 0;
      }
    }
    for (double& m : mean) m /= q;
    dom /= q;
    const double rise = mean[3] - mean[0];
    if (rise < cfg_.rise_db) continue;
    if (mean[1] < mean[0] - 0.5 || mean[2] < mean[1] - 0.5 || mean[3] < mean[2] - 0.5) continue;
    if (mean[3] - mean[1] < 0.35 * rise || mean[2] - mean[0] < 0.35 * rise) continue;
    if (dom < cfg_.dominance || harm * 4 > q) continue;
    double last = 0;
    for (int j = 1; j <= 3; ++j) last += hist_[(hist_pos_ - j + kHist) % kHist].level_db;
    if (last / 3 < cfg_.trigger_dbfs) continue;
    return true;
  }
  return false;
}

void HowlGuard::enter_duck(double db) {
  const double t = now_s();
  if (state_ != State::Ducked) {
    recent_.erase(std::remove_if(recent_.begin(), recent_.end(), [&](double r) { return t - r > kRecentS; }),
                  recent_.end());
    recent_.push_back(t);
    hold_ = std::min(cfg_.max_hold_s, cfg_.hold_s * std::pow(2.0, double(recent_.size() - 1)));
    episodes_.fetch_add(1, std::memory_order_relaxed);
    double last = 0;
    for (int j = 1; j <= 3; ++j) last += hist_[(hist_pos_ - j + kHist) % kHist].level_db;
    trigger_db_ = last / 3;
    decayed_ever_ = false;
    decayed_at_ = -1;
  }
  state_ = State::Ducked;
  duck_at_ = t;
  target_db_ = db;
  hist_len_ = 0;  // judge what happens under the duck on its own
  freq_.store(track_freq_, std::memory_order_relaxed);
  duck_.store(db, std::memory_order_relaxed);
  active_.store(true, std::memory_order_relaxed);
}

void HowlGuard::analyse() {
  // Hann-windowed 1024-point FFT (iterative radix 2) of the newest kFft mono samples.
  for (int n = 0; n < kFft; ++n) {
    re_[rev_[n]] = buf_[n] * win_[n];
    im_[rev_[n]] = 0;
  }
  for (int len = 2; len <= kFft; len <<= 1) {
    const int half = len / 2, step = kFft / len;
    for (int i = 0; i < kFft; i += len) {
      for (int j = 0; j < half; ++j) {
        const float wr = cos_[j * step], wi = sin_[j * step];
        const int a = i + j, b = a + half;
        const float tr = re_[b] * wr - im_[b] * wi, ti = re_[b] * wi + im_[b] * wr;
        re_[b] = re_[a] - tr;
        im_[b] = im_[a] - ti;
        re_[a] += tr;
        im_[a] += ti;
      }
    }
  }
  double band = 0;
  int peak = kMinPeakBin;
  for (int k = 0; k <= kFft / 2; ++k) {
    power_[k] = re_[k] * re_[k] + im_[k] * im_[k];
    if (k >= kMinBin && k <= kMaxBin) band += power_[k];
    if (k >= kMinPeakBin && k <= kMaxBin - 2 && power_[k] > power_[peak]) peak = k;
  }
  // mean square of a sine from its one-sided power: 2 P / (N^2 * mean(w^2)), mean(w^2) = 3/8
  const double to_ms = 2.0 / (double(kFft) * kFft * 0.375);
  auto tone_at = [&](int k) { return double(power_[k - 2]) + power_[k - 1] + power_[k] + power_[k + 1] + power_[k + 2]; };
  auto freq_at = [&](int k) {
    const double a = std::log(power_[k - 1] + 1e-30), b = std::log(power_[k] + 1e-30),
                 c = std::log(power_[k + 1] + 1e-30);
    const double den = a - 2 * b + c;
    const double d = den < 0 ? std::clamp(0.5 * (a - c) / den, -0.5, 0.5) : 0.0;
    return (k + d) * kBinHz;
  };
  const double floor_band = 1e-12;
  // Follow the tracked tone (a local peak within +-1 bin), else start on the band's peak.
  if (track_bin_ >= 0) {
    int k = track_bin_;
    for (int d = -1; d <= 1; ++d) {
      const int c = track_bin_ + d;
      if (c >= kMinPeakBin && c <= kMaxBin - 2 && power_[c] > power_[k]) k = c;
    }
    const double tone = tone_at(k), dom = band > floor_band ? tone / band : 0;
    const bool present = dom >= 0.25 || (k == peak && band > floor_band);
    if (present) {
      track_bin_ = k;
      track_freq_ = freq_at(k);
      misses_ = 0;
    } else if (++misses_ > kMaxMisses) {
      track_bin_ = -1;
      hist_len_ = 0;
    }
    if (present) {  // a miss adds nothing: the history holds the tone only
      Frame f{float(10 * std::log10(tone * to_ms + 1e-12)), float(dom), other_lines(k, freq_at(k), tone)};
      hist_[hist_pos_] = f;
      hist_pos_ = (hist_pos_ + 1) % kHist;
      hist_len_ = std::min(hist_len_ + 1, kHist);
    }
  }
  if (track_bin_ < 0 && band > floor_band) {
    const double tone = tone_at(peak);
    if (tone / band >= 0.25) {
      track_bin_ = peak;
      track_freq_ = freq_at(peak);
      misses_ = 0;
      hist_len_ = 0;
    }
  }
  const double t = now_s();
  const bool tracked = track_bin_ >= 0 && misses_ == 0 && hist_len_ > 0;
  const double level = tracked ? hist_[(hist_pos_ - 1 + kHist) % kHist].level_db : -120.0;
  switch (state_) {
    case State::Idle:
    case State::Releasing:
      if (tracked && !ignored(track_bin_) && rising()) enter_duck(cfg_.duck_db);
      break;
    case State::Ducked: {
      const bool decayed = !tracked || level <= trigger_db_ - 12;
      if (decayed) {
        decayed_ever_ = true;
        if (decayed_at_ < 0) decayed_at_ = t;
      } else {
        decayed_at_ = -1;
      }
      const double elapsed = t - duck_at_;
      if (elapsed >= hold_ && ((decayed && t - decayed_at_ >= 0.3) || (decayed_ever_ && elapsed >= hold_ + 2))) {
        // Decayed; or it did decay and what is left at that frequency is the source's own sound.
        state_ = State::Releasing;
      } else if (!decayed_ever_ && elapsed >= cfg_.false_alarm_s && level > trigger_db_ - 6) {
        // The duck did not break a loop: the tone is the source's own sound.
        false_alarms_.fetch_add(1, std::memory_order_relaxed);
        if (track_bin_ >= 0) ignore_.emplace_back(track_bin_, t + cfg_.ignore_s);
        state_ = State::Releasing;
      }
      if (state_ == State::Releasing) {
        target_db_ = 0;
        step_up_ = std::max(1e-6, -gain_db_ / (cfg_.release_s * kRateHz));
        hist_len_ = 0;
      }
      break;
    }
  }
  ignore_.erase(std::remove_if(ignore_.begin(), ignore_.end(), [&](const auto& e) { return e.second <= t; }),
                ignore_.end());
}

bool HowlGuard::poll(HowlReport* out) {
  HowlReport r;
  r.active = active_.load(std::memory_order_relaxed);
  r.freq_hz = freq_.load(std::memory_order_relaxed);
  r.duck_db = r.active ? duck_.load(std::memory_order_relaxed) : 0.0;
  r.episodes = episodes_.load(std::memory_order_relaxed);
  const bool changed = r.active != reported_.active || r.duck_db != reported_.duck_db ||
                       (r.active && r.episodes != reported_.episodes);
  reported_ = r;
  *out = r;
  return changed;
}

}  // namespace beamer::audio
