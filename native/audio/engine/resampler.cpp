// SPDX-License-Identifier: GPL-3.0-only
#include "resampler.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace beamer::audio {

namespace {

double bessel_i0(double x) {
  double sum = 1, term = 1, q = x * x / 4;
  for (int k = 1; k < 200; ++k) {
    term *= q / (double(k) * k);
    sum += term;
    if (term < sum * 1e-17) break;
  }
  return sum;
}

struct Table {
  std::vector<float> tab;  // (kPhases + 1) x kTaps
  Table() {
    const int T = Resampler::kTaps, P = Resampler::kPhases, half = T / 2;
    const int M = T * 64 + 1;  // np.kaiser(TAPS * 64 + 1, 8.0)
    std::vector<double> w(M);
    const double alpha = (M - 1) / 2.0, i0b = bessel_i0(8.0);
    for (int n = 0; n < M; ++n) {
      double r = (n - alpha) / alpha;
      w[n] = bessel_i0(8.0 * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0b;
    }
    tab.resize(size_t(P + 1) * T);
    const double pi = 3.14159265358979323846;
    for (int p = 0; p <= P; ++p) {
      double ph = double(p) / P;
      double row[64];
      double sum = 0;
      for (int t = 0; t < T; ++t) {
        double x = double(t - half + 1) - ph;
        double xi = std::nearbyint((x + half) / (2.0 * half) * (M - 1));  // numpy round: half-even
        int idx = static_cast<int>(std::min<double>(std::max<double>(xi, 0), M - 1));
        double a = x * 0.97;
        double sinc = a == 0 ? 1.0 : std::sin(pi * a) / (pi * a);
        row[t] = sinc * 0.97 * w[idx];
        sum += row[t];
      }
      for (int t = 0; t < T; ++t) tab[size_t(p) * T + t] = static_cast<float>(row[t] / sum);
    }
  }
};

}  // namespace

const float* Resampler::table() {
  static const Table t;
  return t.tab.data();
}

Resampler::Resampler(int channels, int max_frames)
    : ch_(channels), cap_(max_frames + kTaps * 2), buf_(new float[size_t(cap_) * channels]) {
  table();
  reset();
}

void Resampler::reset() {
  std::memset(buf_.get(), 0, sizeof(float) * size_t(kTaps) * ch_);
  len_ = kTaps;
  pos_ = kTaps / 2 - 1;
  dropped_ = 0;
}

int Resampler::need(int n, double step) const {
  double last = pos_ + (n - 1) * step;
  long long v = static_cast<long long>(std::floor(last)) + kTaps / 2 + 1 - len_;
  return v > 0 ? static_cast<int>(v) : 0;
}

bool Resampler::feed(const float* x, int n) {
  if (n <= 0) return true;
  if (len_ + n > cap_) return false;
  std::memcpy(&buf_[size_t(len_) * ch_], x, sizeof(float) * size_t(n) * ch_);
  len_ += n;
  return true;
}

bool Resampler::feed_zeros(int n) {
  if (n <= 0) return true;
  if (len_ + n > cap_) return false;
  std::memset(&buf_[size_t(len_) * ch_], 0, sizeof(float) * size_t(n) * ch_);
  len_ += n;
  return true;
}

void Resampler::out(float* y, int n, double step) {
  const float* tab = table();
  const int half = kTaps / 2;
  const float* b = buf_.get();
  for (int i = 0; i < n; ++i) {
    double p = pos_ + i * step;
    double base_f = std::floor(p);
    long long base = static_cast<long long>(base_f);
    double frac = p - base_f;
    int phase = static_cast<int>(std::nearbyint(frac * kPhases));
    if (phase > kPhases) phase = kPhases;
    const float* w = tab + size_t(phase) * kTaps;
    long long i0 = base - half + 1;
    if (i0 < 0 || i0 + kTaps > len_) {  // cannot happen when need() == 0; be safe
      for (int c = 0; c < ch_; ++c) y[size_t(i) * ch_ + c] = 0;
      continue;
    }
    if (ch_ == 1) {
      const float* s = b + i0;
      float acc = 0;
      for (int t = 0; t < kTaps; ++t) acc += w[t] * s[t];
      y[i] = acc;
    } else if (ch_ == 2) {
      const float* s = b + i0 * 2;
      float l = 0, r = 0;
      for (int t = 0; t < kTaps; ++t) {
        l += w[t] * s[2 * t];
        r += w[t] * s[2 * t + 1];
      }
      y[2 * i] = l;
      y[2 * i + 1] = r;
    } else {
      for (int c = 0; c < ch_; ++c) {
        float acc = 0;
        for (int t = 0; t < kTaps; ++t) acc += w[t] * b[size_t(i0 + t) * ch_ + c];
        y[size_t(i) * ch_ + c] = acc;
      }
    }
  }
  pos_ += n * step;
  long long drop = static_cast<long long>(std::floor(pos_)) - half + 1;
  if (drop > 0) {
    if (drop > len_) drop = len_;
    std::memmove(buf_.get(), buf_.get() + size_t(drop) * ch_, sizeof(float) * size_t(len_ - drop) * ch_);
    len_ -= static_cast<int>(drop);
    pos_ -= static_cast<double>(drop);
    dropped_ += drop;
  }
}

double DriftController::update(double err, double block_s) {
  integ = std::max(-200.0, std::min(200.0, integ + 0.1 * err * block_s));
  double p = std::max(-max_ppm, std::min(max_ppm, 40.0 * err + integ));
  if (err > 4.0) p = std::min(std::max(catchup_ppm, max_ppm), std::max(p, 200.0 * (err - 4.0) + 40.0 * 4.0));
  ppm = p;
  return ppm;
}

}  // namespace beamer::audio
