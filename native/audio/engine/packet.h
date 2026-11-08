// SPDX-License-Identifier: GPL-3.0-only
// Media plane packet (PHASE2 §4.1, version 1): 24-byte clear header used as AEAD associated data,
// then ChaCha20-Poly1305 ciphertext + tag. Nonce = dir_stream u32 || seq u64 (big endian).
#pragma once

#include <cstddef>
#include <cstdint>

#include "crypto.h"

namespace beamer::audio {

constexpr uint8_t kPacketVersion = 1;
enum PType : uint8_t { kMedia = 1, kHello = 2, kPing = 3, kPong = 4, kRR = 5, kBye = 6 };
enum PFlag : uint8_t { kFlagFec = 1, kFlagMarker = 2, kFlagSilence = 4 };
enum PCodec : uint8_t { kCodecPcm = 0, kCodecOpus = 1 };

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

// 128-packet sliding window over seq per (key, direction).
class ReplayWindow {
 public:
  bool fresh(uint64_t seq) const;  // not too old and not seen
  void mark(uint64_t seq);         // call after authentication succeeded
  uint64_t highest() const { return hi_; }

 private:
  uint64_t hi_ = 0;
  uint64_t bits_[2] = {0, 0};  // bit i of the 128: seq hi - i seen
};

enum class OpenResult { Ok, Rejected, Replayed };

// Authenticates and decrypts. payload_out must hold len - kMinPacket bytes.
OpenResult open_packet(const uint8_t key[kKeyLen], uint32_t dir_stream, ReplayWindow* window,
                       const uint8_t* data, size_t len, Header* h, uint8_t* payload_out,
                       size_t* payload_len);

}  // namespace beamer::audio
