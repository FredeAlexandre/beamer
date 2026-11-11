// SPDX-License-Identifier: GPL-3.0-only
// Per-flow video pacer (camera PHASE2 §4.4). Video media packets only (new data and retransmissions); every
// other packet bypasses it. Token bucket of 32 packets refilled at 25 Mbit/s for P frames, refreshes and
// retransmissions; packets of an IDR are released at their own rate (idr_pace_x x the stream's bitrate) on an
// absolute schedule: packet i at t0 + bytes_before(i) / rate, t0 = the release of its first packet, so a late
// wake-up sends every due packet back to back and the IDR still ends on time. Retransmissions go ahead of new
// data. A dedicated thread waits on a condition variable; the pacer only sends stored (sealed) bytes.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace beamer::audio {

struct PacedPacket {
  std::shared_ptr<const std::vector<uint8_t>> bytes;  // sealed packet (shared with the sender's history)
  uint32_t stream = 0, frame = 0;
  bool idr = false;           // released on the IDR schedule
  bool retx = false;
  bool first = false;         // first packet of its AU
  bool end = false;           // last packet of its AU (or of a retransmission batch)
  double idr_bps = 0;         // IDR release rate, bit/s
  int64_t enqueued_ns = 0;
};

class PacerClient {  // a video send stream; called on the pacer thread after each release
 public:
  virtual ~PacerClient() = default;
  virtual void on_paced(const PacedPacket& p, int64_t now) = 0;
};

class Pacer {
 public:
  using SendFn = std::function<void(const PacedPacket&)>;
  static constexpr double kBucketBps = 25e6;
  static constexpr size_t kBucketPackets = 32;
  static constexpr size_t kBucketBytes = kBucketPackets * 1200;

  explicit Pacer(SendFn send);
  ~Pacer();
  Pacer(const Pacer&) = delete;
  Pacer& operator=(const Pacer&) = delete;

  void add_client(uint32_t stream, std::weak_ptr<PacerClient> c);
  // Drops its queued packets and returns once the pacer thread holds no reference to that stream's client (R3 #9):
  // after it, the pacer can never be the last owner of the stream (whose destruction can end the flow and with it
  // this pacer, which would then join its own thread). The owner calls it before it drops its references.
  void remove_stream(uint32_t stream);
  void enqueue(std::vector<PacedPacket>&& pkts);       // one AU, FIFO behind earlier AUs
  void enqueue_retx(std::vector<PacedPacket>&& pkts);  // ahead of new data
  bool idr_queued(uint32_t stream) const;              // packets of an IDR of this stream still waiting
  size_t queued_bytes(uint32_t stream) const;

  // tests: extra sleep after every wake-up (C4's 0.35 ms overshoot)
  std::atomic<int64_t> test_overshoot_ns{0};
  std::atomic<uint64_t> n_sent{0}, n_retx{0}, wait_ns_sum{0};

 private:
  void run();
  SendFn send_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<PacedPacket> retx_, data_;
  std::map<uint32_t, std::weak_ptr<PacerClient>> clients_;
  std::map<uint32_t, size_t> idr_left_, bytes_left_;
  double tokens_ = double(kBucketBytes);
  int64_t last_refill_ = 0;
  // current IDR schedule
  bool idr_active_ = false;
  uint32_t idr_stream_ = 0, idr_frame_ = 0;
  int64_t idr_t0_ = 0;
  double idr_bytes_before_ = 0;
  bool stop_ = false;
  bool in_flight_ = false;        // the pacer thread holds a client reference (on_paced) of in_flight_stream_
  uint32_t in_flight_stream_ = 0;
  std::condition_variable idle_cv_;
  std::thread thread_;
};

}  // namespace beamer::audio
