// SPDX-License-Identifier: GPL-3.0-only
// Media plane packet (PHASE2 §4.1, version 1): 24-byte clear header used as AEAD associated data,
// then ChaCha20-Poly1305 ciphertext + tag. Nonce = dir_stream u32 || seq u64 (big endian).
#pragma once

#include <cstddef>
#include <cstdint>

#include "crypto.h"

namespace beamer::audio {

constexpr uint8_t kPacketVersion = 1;
// kNack/kPli: video stream, receiver direction; kTimeMap: flow control channel (stream 0). Camera
// PHASE2 §4.2.
enum PType : uint8_t { kMedia = 1, kHello = 2, kPing = 3, kPong = 4, kRR = 5, kBye = 6, kNack = 7, kPli = 8, kTimeMap = 9 };
enum PFlag : uint8_t { kFlagFec = 1, kFlagMarker = 2, kFlagSilence = 4 };
enum PCodec : uint8_t { kCodecPcm = 0, kCodecOpus = 1, kCodecH264 = 2 };

constexpr size_t kHeaderLen = 24;
constexpr size_t kMaxPayload = 1400;
constexpr size_t kMaxPacket = kHeaderLen + kMaxPayload + kTagLen;
constexpr size_t kMinPacket = kHeaderLen + kTagLen;
constexpr uint32_t kDirBit = 0x80000000u;
constexpr int kReplayWindow = 128;

struct Header {
  uint8_t version = kPacketVersion;
  uint8_t ptype = 0;
  uint8_t flags = 0;
  uint8_t codec = 0;
  uint32_t stream = 0;
  uint64_t seq = 0;
  uint32_t ts = 0;
  uint16_t nsamp = 0;
  uint8_t channels = 0;
  uint8_t reserved = 0;
};

void write_header(const Header& h, uint8_t out[kHeaderLen]);
bool read_header(const uint8_t* data, size_t len, Header* h);  // false: too short or bad version
void make_nonce(uint32_t dir_stream, uint64_t seq, uint8_t out[kNonceLen]);

// Seals packets of one (key, direction). Not thread-safe: one owner thread.
class Sealer {
 public:
  Sealer() = default;
  Sealer(const uint8_t key[kKeyLen], uint32_t dir_stream) { init(key, dir_stream); }
  void init(const uint8_t key[kKeyLen], uint32_t dir_stream);
  // h.seq is assigned here (1, 2, ...). Returns the packet length (header + payload + tag).
  size_t seal(Header h, const uint8_t* payload, size_t len, uint8_t* out);
  uint64_t last_seq() const { return seq_; }

 private:
  uint8_t key_[kKeyLen] = {0};
  uint32_t dir_ = 0;
  uint64_t seq_ = 0;
};

// N-packet sliding window over seq per (key, direction): 128 for audio and control (ReplayWindow),
// 1024 for video (camera PHASE2 §4.1).
template <int N>
class ReplayWindowN {
  static_assert(N % 64 == 0 && N >= 64, "window: a multiple of 64");
  static constexpr int kWords = N / 64;

 public:
  bool fresh(uint64_t seq) const {  // not too old and not seen
    if (seq == 0) return false;
    if (seq > hi_) return true;
    const uint64_t d = hi_ - seq;
    if (d >= static_cast<uint64_t>(N)) return false;
    return !((bits_[d >> 6] >> (d & 63)) & 1);
  }
  void mark(uint64_t seq) {  // call after authentication succeeded
    if (seq > hi_) {
      shift(seq - hi_);
      bits_[0] |= 1;
      hi_ = seq;
    } else {
      const uint64_t d = hi_ - seq;
      if (d < static_cast<uint64_t>(N)) bits_[d >> 6] |= uint64_t(1) << (d & 63);
    }
  }
  uint64_t highest() const { return hi_; }

 private:
  void shift(uint64_t s) {  // bit i -> bit i + s
    if (s >= static_cast<uint64_t>(N)) {
      for (auto& w : bits_) w = 0;
      return;
    }
    const int words = static_cast<int>(s >> 6), bits = static_cast<int>(s & 63);
    for (int i = kWords - 1; i >= 0; --i) {
      uint64_t v = 0;
      const int src = i - words;
      if (src >= 0) {
        v = bits_[src] << bits;
        if (bits && src > 0) v |= bits_[src - 1] >> (64 - bits);
      }
      bits_[i] = v;
    }
  }
  uint64_t hi_ = 0;
  uint64_t bits_[kWords] = {};  // bit i: seq hi - i seen
};
using ReplayWindow = ReplayWindowN<kReplayWindow>;

enum class OpenResult { Ok, Rejected, Replayed };

// Authenticates and decrypts (no replay check). payload_out must hold len - kMinPacket bytes.
OpenResult open_packet_auth(const uint8_t key[kKeyLen], uint32_t dir_stream, const uint8_t* data, size_t len,
                            Header* h, uint8_t* payload_out, size_t* payload_len);

// Authenticates and decrypts with a replay window (may be null). payload_out must hold len - kMinPacket bytes.
template <int N>
OpenResult open_packet(const uint8_t key[kKeyLen], uint32_t dir_stream, ReplayWindowN<N>* window,
                       const uint8_t* data, size_t len, Header* h, uint8_t* payload_out, size_t* payload_len) {
  if (len < kMinPacket || len > kMaxPacket || !read_header(data, len, h)) return OpenResult::Rejected;
  if (window && !window->fresh(h->seq)) return OpenResult::Replayed;
  const OpenResult r = open_packet_auth(key, dir_stream, data, len, h, payload_out, payload_len);
  if (r == OpenResult::Ok && window) window->mark(h->seq);
  return r;
}
inline OpenResult open_packet(const uint8_t key[kKeyLen], uint32_t dir_stream, std::nullptr_t, const uint8_t* data,
                              size_t len, Header* h, uint8_t* payload_out, size_t* payload_len) {
  return open_packet<kReplayWindow>(key, dir_stream, static_cast<ReplayWindow*>(nullptr), data, len, h, payload_out,
                                    payload_len);
}

}  // namespace beamer::audio
