// SPDX-License-Identifier: GPL-3.0-only
#include "test_signal.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace beamer::audio {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kR = 48000.0;
}  // namespace

bool TestSignal::parse(const std::string& spec, TestSignal* out) {
  TestSignal t;
  auto parts = [&](size_t i) -> std::string {
    size_t start = 0;
    for (size_t k = 0; k < i; ++k) {
      start = spec.find(':', start);
      if (start == std::string::npos) return "";
      start++;
    }
    size_t end = spec.find(':', start);
    return spec.substr(start, end == std::string::npos ? std::string::npos : end - start);
  };
  std::string kind = parts(0);
  if (kind == "A" || kind == "B") {
    t.kind_ = Kind::Signature;
    // sig.py: A pilot 1025 Hz, up-chirp 2->5 kHz; B pilot 625 Hz, down-chirp 5->2 kHz; 40 ms
    double f0 = kind == "A" ? 2000.0 : 5000.0, f1 = kind == "A" ? 5000.0 : 2000.0;
    t.freq_ = kind == "A" ? 1025.0 : 625.0;
    const int n = static_cast<int>(kR * 0.040);
    const double k = (f1 - f0) / 0.040, marker_amp = 0.1;
    t.tpl_.resize(n);
    for (int i = 0; i < n; ++i) {
      double tt = i / kR;
      double hann = 0.5 - 0.5 * std::cos(2 * kPi * i / (n - 1));  // np.hanning
      t.tpl_[i] = static_cast<float>(marker_amp * hann * std::sin(2 * kPi * (f0 * tt + 0.5 * k * tt * tt)));
    }
    // "A:<pilot amplitude>": Phase 1 mixed mic signatures with a 0.2 pilot under 0.1 chirps (chirps
    // louder than the pilot make an Opus VOIP decoder that once concealed a frame dip the pilot after
    // each chirp: false glitches, dev/audio/proto/scenarios/README.md)
    std::string pa = parts(1);
    if (!pa.empty()) {
      t.pilot_amp_ = std::atof(pa.c_str());
      if (!(t.pilot_amp_ >= 0 && t.pilot_amp_ <= 0.5)) return false;
    }
  } else if (kind == "sine") {
    t.kind_ = Kind::Sine;
    std::string f = parts(1), a = parts(2);
    if (!f.empty()) t.freq_ = std::atof(f.c_str());
    if (!a.empty()) t.amp_ = std::atof(a.c_str());
    if (!(t.freq_ > 0 && t.freq_ < 24000)) return false;
  } else if (kind == "rise") {
    t.kind_ = Kind::Rise;
    std::string f = parts(1), r = parts(2);
    t.freq_ = f.empty() ? 1000.0 : std::atof(f.c_str());
    if (!r.empty()) t.rise_db_s_ = std::atof(r.c_str());
    t.amp_ = 0.3;
    if (!(t.freq_ > 0 && t.freq_ < 24000) || !(t.rise_db_s_ > 0 && t.rise_db_s_ <= 200)) return false;
  } else if (kind == "noise") {
    t.kind_ = Kind::Noise;
    std::string a = parts(1);
    t.amp_ = a.empty() ? 0.1 : std::atof(a.c_str());
  } else if (kind == "silence") {
    t.kind_ = Kind::Silence;
  } else {
    return false;
  }
  *out = t;
  return true;
}

int64_t TestSignal::marker_index(int64_t k) const {
  return static_cast<int64_t>(std::nearbyint(kR * (start_s_ + k * period_s_ + 0.037 * (k % 7))));
}

void TestSignal::render(int64_t s0, int n, float* out) {
  switch (kind_) {
    case Kind::Silence:
      for (int i = 0; i < n; ++i) out[i] = 0;
      return;
    case Kind::Noise:
      for (int i = 0; i < n; ++i) {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 7;
        rng_ ^= rng_ << 17;
        out[i] = static_cast<float>(amp_ * ((rng_ >> 11) * (2.0 / 9007199254740992.0) - 1.0));
      }
      return;
    case Kind::Rise: {
      const double base = std::fmod(freq_ * double(s0), kR), w = 2 * kPi / kR;
      for (int i = 0; i < n; ++i) {
        const double a = std::min(amp_, std::pow(10.0, (-70.0 + rise_db_s_ * double(s0 + i) / kR) / 20.0));
        out[i] = static_cast<float>(a * std::sin(w * (base + freq_ * i)));
      }
      return;
    }
    case Kind::Sine:
    case Kind::Signature: {
      // phase reduced once per block (exact for the absolute index), then advanced per sample
      const double a = kind_ == Kind::Sine ? amp_ : pilot_amp_;
      const double base = std::fmod(freq_ * double(s0), kR), w = 2 * kPi / kR;
      for (int i = 0; i < n; ++i) out[i] = static_cast<float>(a * std::sin(w * (base + freq_ * i)));
      if (kind_ == Kind::Sine) return;
      const int64_t L = static_cast<int64_t>(tpl_.size());
      int64_t k = static_cast<int64_t>((s0 / kR - start_s_ - 1.0) / period_s_) - 1;
      if (k < 0) k = 0;
      for (;; ++k) {
        int64_t m = marker_index(k);
        if (m >= s0 + n) break;
        if (m + L > s0) {
          int64_t lo = m > s0 ? m : s0, hi = (m + L) < (s0 + n) ? (m + L) : (s0 + n);
          for (int64_t s = lo; s < hi; ++s) out[s - s0] += tpl_[static_cast<size_t>(s - m)];
        }
      }
      return;
    }
  }
}

}  // namespace beamer::audio
