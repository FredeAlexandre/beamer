// SPDX-License-Identifier: GPL-3.0-only
#include "pacer.h"

#include <algorithm>
#include <chrono>

#include "util.h"

namespace beamer::audio {

Pacer::Pacer(SendFn send) : send_(std::move(send)) {
  last_refill_ = now_ns();
  thread_ = std::thread([this] { run(); });
}

Pacer::~Pacer() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void Pacer::add_client(uint32_t stream, std::weak_ptr<PacerClient> c) {
  std::lock_guard<std::mutex> lock(mu_);
  clients_[stream] = std::move(c);
}

void Pacer::remove_stream(uint32_t stream) {
  std::unique_lock<std::mutex> lock(mu_);
  clients_.erase(stream);
  if (std::this_thread::get_id() != thread_.get_id())  // never from on_paced itself (it would wait for itself)
    idle_cv_.wait(lock, [&] { return !in_flight_ || in_flight_stream_ != stream; });
  auto drop = [stream](std::deque<PacedPacket>& q) {
    q.erase(std::remove_if(q.begin(), q.end(), [stream](const PacedPacket& p) { return p.stream == stream; }), q.end());
  };
  drop(retx_);
  drop(data_);
  idr_left_.erase(stream);
  bytes_left_.erase(stream);
  if (idr_active_ && idr_stream_ == stream) idr_active_ = false;
}

void Pacer::enqueue(std::vector<PacedPacket>&& pkts) {
  if (pkts.empty()) return;
  const int64_t now = now_ns();
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& p : pkts) {
      p.enqueued_ns = now;
      if (p.idr) idr_left_[p.stream]++;
      bytes_left_[p.stream] += p.bytes->size();
      data_.push_back(std::move(p));
    }
  }
  cv_.notify_all();
}

void Pacer::enqueue_retx(std::vector<PacedPacket>&& pkts) {
  if (pkts.empty()) return;
  const int64_t now = now_ns();
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& p : pkts) {
      p.enqueued_ns = now;
      p.retx = true;
      p.idr = false;  // a repair is never held to the IDR schedule
      bytes_left_[p.stream] += p.bytes->size();
      retx_.push_back(std::move(p));
    }
  }
  cv_.notify_all();
}

bool Pacer::idr_queued(uint32_t stream) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = idr_left_.find(stream);
  return it != idr_left_.end() && it->second > 0;
}

size_t Pacer::queued_bytes(uint32_t stream) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = bytes_left_.find(stream);
  return it == bytes_left_.end() ? 0 : it->second;
}

void Pacer::run() {
  set_thread_name("beamer-audio-pacer");
  set_thread_interactive();
  enable_fine_timers();  // Windows: 1 ms timer resolution (the pacer's schedule and the presenter need it)
  std::unique_lock<std::mutex> lock(mu_);
  while (!stop_) {
    if (retx_.empty() && data_.empty()) {
      cv_.wait(lock);
      continue;
    }
    int64_t now = now_ns();
    // refill the bucket
    tokens_ = std::min(double(kBucketBytes), tokens_ + double(now - last_refill_) * 1e-9 * (kBucketBps / 8.0));
    last_refill_ = now;
    std::deque<PacedPacket>& q = !retx_.empty() ? retx_ : data_;
    PacedPacket& p = q.front();
    const double size = double(p.bytes->size());
    int64_t due = now;
    if (p.idr && p.idr_bps > 0) {
      if (!idr_active_ || idr_stream_ != p.stream || idr_frame_ != p.frame) {
        idr_active_ = true;
        idr_stream_ = p.stream;
        idr_frame_ = p.frame;
        idr_t0_ = now;  // the first packet leaves at once
        idr_bytes_before_ = 0;
      }
      due = idr_t0_ + static_cast<int64_t>(idr_bytes_before_ * 8.0 / p.idr_bps * 1e9);
    } else if (tokens_ < size) {
      due = now + static_cast<int64_t>((size - tokens_) / (kBucketBps / 8.0) * 1e9) + 1;
    }
    if (due > now) {
      cv_.wait_until(lock, std::chrono::steady_clock::time_point(
                                std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::nanoseconds(due))));
      const int64_t over = test_overshoot_ns.load(std::memory_order_relaxed);
      if (over > 0) {
        lock.unlock();
        sleep_ns(over);
        lock.lock();
      }
      continue;  // re-evaluate (a retransmission may have arrived; the schedule is absolute)
    }
    PacedPacket out = std::move(p);
    q.pop_front();
    if (out.idr && out.idr_bps > 0) {
      idr_bytes_before_ += size;
      auto it = idr_left_.find(out.stream);
      if (it != idr_left_.end() && it->second > 0) it->second--;
      if (out.end) idr_active_ = false;
    } else {
      tokens_ -= size;
    }
    auto bl = bytes_left_.find(out.stream);
    if (bl != bytes_left_.end()) bl->second -= std::min(bl->second, out.bytes->size());
    std::shared_ptr<PacerClient> client;
    auto c = clients_.find(out.stream);
    if (c != clients_.end()) client = c->second.lock();
    if (client) {
      in_flight_ = true;
      in_flight_stream_ = out.stream;
    }
    lock.unlock();
    send_(out);
    n_sent.fetch_add(1, std::memory_order_relaxed);
    if (out.retx) n_retx.fetch_add(1, std::memory_order_relaxed);
    wait_ns_sum.fetch_add(static_cast<uint64_t>(std::max<int64_t>(0, now - out.enqueued_ns)), std::memory_order_relaxed);
    if (client) client->on_paced(out, now);
    const bool had = client != nullptr;
    client.reset();  // outside the lock; never the last reference once remove_stream ran (it waits for this)
    lock.lock();
    if (had) {
      in_flight_ = false;
      idle_cv_.notify_all();
    }
  }
}

}  // namespace beamer::audio
