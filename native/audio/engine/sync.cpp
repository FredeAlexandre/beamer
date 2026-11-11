// SPDX-License-Identifier: GPL-3.0-only
#include "sync.h"

#include <algorithm>
#include <cmath>

namespace beamer::audio {

void OffsetEstimator::add(int64_t t0, int64_t t1, int64_t t2, int64_t t3) {
  const int64_t rtt = (t3 - t0) - (t2 - t1);
  if (rtt < 0 || rtt > 10'000'000'000LL) return;
  const int64_t off = ((t1 - t0) + (t2 - t3)) / 2;
  samples_.push_back(Sample{off, rtt, t3});
  while (samples_.size() > 10) samples_.pop_front();
  const Sample* best = &samples_.front();
  for (const auto& s : samples_)
    if (s.rtt < best->rtt) best = &s;
  std::lock_guard<std::mutex> lock(mu_);
  offset_.store(best->offset, std::memory_order_relaxed);
  rtt_.store(best->rtt, std::memory_order_relaxed);
  at_.store(best->at, std::memory_order_relaxed);
  valid_.store(true, std::memory_order_release);
}

bool OffsetEstimator::get(int64_t now, int64_t* offset_ns, int64_t* age_ns, int64_t* rtt_ns) const {
  if (!valid_.load(std::memory_order_acquire)) return false;
  std::lock_guard<std::mutex> lock(mu_);
  *offset_ns = offset_.load(std::memory_order_relaxed);
  if (age_ns) *age_ns = now - at_.load(std::memory_order_relaxed);
  if (rtt_ns) *rtt_ns = rtt_.load(std::memory_order_relaxed);
  return true;
}

void TimeMapBook::set(uint32_t ts, uint64_t sender_ns) {
  ts_ = ts;
  ns_ = static_cast<int64_t>(sender_ns);
  valid_ = true;
  n_++;
}

int64_t TimeMapBook::to_sender_ns(uint32_t ts) const {
  const int64_t d = static_cast<int32_t>(ts - ts_);  // modulo 2^32, +-6.6 h at 90 kHz
  return ns_ + d * 1'000'000'000LL / rate_;
}

void PlayoutTarget::add(int64_t now, double d_ms) {
  win_.emplace_back(now, d_ms);
  total_++;
  recompute(now);
}

void PlayoutTarget::recompute(int64_t now) {
  while (!win_.empty() && now - win_.front().first > 10'000'000'000LL) win_.pop_front();
  if (win_.size() < size_t(kMinSamples)) {
    p99_ = 0;
    return;
  }
  std::vector<double> v;
  v.reserve(win_.size());
  for (auto& s : win_) v.push_back(s.second);
  const size_t k = std::min(v.size() - 1, static_cast<size_t>(std::ceil(0.99 * double(v.size()))) - 1);
  std::nth_element(v.begin(), v.begin() + static_cast<long>(k), v.end());
  p99_ = v[k];
}

double PlayoutTarget::target_ms(int64_t now) {
  if (win_.size() < size_t(kMinSamples)) {
    // too few samples (start, or a quiet window): keep 70 ms until the first 20, then keep the last D
    if (!have_) {
      d_ms_ = std::max(kInitialMs, floor_ms_);
      last_ns_ = now;
      return std::min(d_ms_, kCeilMs);
    }
  }
  if (!win_.empty() && now - win_.front().first > 10'000'000'000LL) recompute(now);
  const double want = win_.size() >= size_t(kMinSamples) ? std::min(kCeilMs, std::max(floor_ms_, p99_ + kMarginMs))
                                                          : d_ms_;
  if (!have_) {
    have_ = true;
    d_ms_ = want;
  } else if (want >= d_ms_) {
    d_ms_ = want;  // rises at once
  } else {
    const double dt_s = double(std::max<int64_t>(0, now - last_ns_)) * 1e-9;
    d_ms_ = std::max(want, d_ms_ - 30.0 * dt_s);  // 15 ms per 0.5 s
  }
  d_ms_ = std::min(kCeilMs, std::max(floor_ms_, d_ms_));
  last_ns_ = now;
  return d_ms_;
}

}  // namespace beamer::audio
