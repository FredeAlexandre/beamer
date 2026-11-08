// SPDX-License-Identifier: GPL-3.0-only
// Lock-free single-producer single-consumer rings. Storage is allocated once in the constructor;
// push/pop never allocate, lock or block, so either end may be a real-time IO thread.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

namespace beamer::audio {

inline size_t next_pow2(size_t n) {
  size_t p = 1;
  while (p < n) p <<= 1;
  return p;
}

// Ring of fixed-size records (T trivially copyable).
template <typename T>
class SpscRing {
 public:
  explicit SpscRing(size_t capacity)
      : cap_(next_pow2(capacity < 2 ? 2 : capacity)), mask_(cap_ - 1), buf_(new T[cap_]) {}
  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  size_t capacity() const { return cap_; }

  // Producer: a slot to fill in place, or nullptr when full. Then commit().
  T* claim() {
    size_t w = write_.load(std::memory_order_relaxed);
    if (w - read_.load(std::memory_order_acquire) >= cap_) return nullptr;
    return &buf_[w & mask_];
  }
  void commit() { write_.store(write_.load(std::memory_order_relaxed) + 1, std::memory_order_release); }
  bool push(const T& v) {
    T* s = claim();
    if (!s) return false;
    *s = v;
    commit();
    return true;
  }

  // Consumer: the oldest record or nullptr when empty. Then release().
  const T* front() {
    size_t r = read_.load(std::memory_order_relaxed);
    if (r == write_.load(std::memory_order_acquire)) return nullptr;
    return &buf_[r & mask_];
  }
  void release() { read_.store(read_.load(std::memory_order_relaxed) + 1, std::memory_order_release); }
  bool pop(T* out) {
    const T* f = front();
    if (!f) return false;
    *out = *f;
    release();
    return true;
  }
  size_t size() const {
    return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire);
  }

 private:
  const size_t cap_, mask_;
  std::unique_ptr<T[]> buf_;
  alignas(64) std::atomic<size_t> write_{0};
  alignas(64) std::atomic<size_t> read_{0};
};

// Ring of interleaved float samples, written and read in bulk (counts are in frames).
class SpscFloatRing {
 public:
  SpscFloatRing(size_t frames, int channels)
      : ch_(channels), cap_(next_pow2(frames)), mask_(cap_ - 1), buf_(new float[cap_ * channels]) {
    std::memset(buf_.get(), 0, sizeof(float) * cap_ * channels);
  }
  SpscFloatRing(const SpscFloatRing&) = delete;
  SpscFloatRing& operator=(const SpscFloatRing&) = delete;

  int channels() const { return ch_; }
  size_t capacity() const { return cap_; }
  size_t available() const {
    return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire);
  }
  size_t space() const { return cap_ - available(); }

  // Writes up to n frames; returns frames written (the rest is dropped by the caller).
  size_t write(const float* src, size_t n) {
    size_t w = write_.load(std::memory_order_relaxed);
    size_t r = read_.load(std::memory_order_acquire);
    size_t room = cap_ - (w - r);
    if (n > room) n = room;
    copy_in(w, src, n);
    write_.store(w + n, std::memory_order_release);
    return n;
  }
  // Consumer: reads up to n frames; returns frames read.
  size_t read(float* dst, size_t n) {
    size_t r = read_.load(std::memory_order_relaxed);
    size_t w = write_.load(std::memory_order_acquire);
    if (n > w - r) n = w - r;
    copy_out(r, dst, n);
    read_.store(r + n, std::memory_order_release);
    return n;
  }
  // Consumer: discard up to n frames.
  size_t skip(size_t n) {
    size_t r = read_.load(std::memory_order_relaxed);
    size_t w = write_.load(std::memory_order_acquire);
    if (n > w - r) n = w - r;
    read_.store(r + n, std::memory_order_release);
    return n;
  }

 private:
  void copy_in(size_t w, const float* src, size_t n) {
    size_t i = w & mask_;
    size_t first = n < cap_ - i ? n : cap_ - i;
    std::memcpy(&buf_[i * ch_], src, sizeof(float) * first * ch_);
    if (n > first) std::memcpy(&buf_[0], src + first * ch_, sizeof(float) * (n - first) * ch_);
  }
  void copy_out(size_t r, float* dst, size_t n) const {
    size_t i = r & mask_;
    size_t first = n < cap_ - i ? n : cap_ - i;
    std::memcpy(dst, &buf_[i * ch_], sizeof(float) * first * ch_);
    if (n > first) std::memcpy(dst + first * ch_, &buf_[0], sizeof(float) * (n - first) * ch_);
  }

  const int ch_;
  const size_t cap_, mask_;
  std::unique_ptr<float[]> buf_;
  alignas(64) std::atomic<size_t> write_{0};
  alignas(64) std::atomic<size_t> read_{0};
};

}  // namespace beamer::audio
