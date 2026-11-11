// SPDX-License-Identifier: GPL-3.0-only
// Video media plane payloads (camera PHASE2 §4.1-4.2): the 12 B video header inside the AEAD plaintext of a
// kMedia packet with codec kCodecH264, and the plaintexts of kNack, kPli, kTimeMap and the 28 B video kRR.
// All big endian. Pure functions, no state.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "packet.h"

namespace beamer::audio {

constexpr size_t kVideoHeaderLen = 12;
constexpr size_t kVideoUdpPayload = 1200;                                            // Tailscale MTU 1280
constexpr size_t kMaxFragment = kVideoUdpPayload - kHeaderLen - kVideoHeaderLen - kTagLen;  // 1148
constexpr uint32_t kMaxNfrag = 1024;
constexpr size_t kMaxAuBytes = size_t(kMaxNfrag) * kMaxFragment;                     // ~1.1 MB
constexpr int kVideoReplayWindow = 1024;
using VideoReplayWindow = ReplayWindowN<kVideoReplayWindow>;
constexpr uint32_t kNoFrame = 0xFFFFFFFFu;  // "none" in PLI last_clean and RR clean_since/acked

enum VFlag : uint8_t { kVIdr = 1, kVRefresh = 2, kVLtr = 4, kVBehindIdr = 8 };

struct VideoHeader {
  uint32_t frame = 0;
  uint16_t frag = 0, nfrag = 1;
  uint8_t vflags = 0, ltr = 0;
  uint16_t enc_delay = 0;  // encoder output - capture, 0.1 ms, saturating
};
void write_video_header(const VideoHeader& v, uint8_t out[kVideoHeaderLen]);
// false: too short, nfrag 0 or > kMaxNfrag, frag >= nfrag, reserved vflags bits set
bool read_video_header(const uint8_t* p, size_t len, VideoHeader* v);
uint16_t enc_delay_units(int64_t ns);  // ns -> 0.1 ms, saturating at 65535

// 90 kHz media timestamp of a capture time, relative to the stream's first captured frame (wraps after 13 h).
inline uint32_t video_ts(uint64_t capture_ns, uint64_t first_capture_ns) {
  const uint64_t d = capture_ns >= first_capture_ns ? capture_ns - first_capture_ns : 0;
  return static_cast<uint32_t>(d * 9 / 100000);  // d < 64 years
}

// ---- kNack: n u8 (1..32), then n x (seq_base u64, mask u16: bit i = seq_base+1+i also missing)
struct NackEntry {
  uint64_t seq_base = 0;
  uint16_t mask = 0;
};
constexpr size_t kMaxNackEntries = 32;
size_t write_nack(const std::vector<NackEntry>& e, uint8_t* out);  // <= 32 entries written; returns bytes
bool read_nack(const uint8_t* p, size_t len, std::vector<NackEntry>* out);
// Packs sorted missing seqs into entries (<= 32; the rest is left for the next packet). Returns how many
// seqs were consumed.
size_t pack_nack(const std::vector<uint64_t>& sorted_seqs, std::vector<NackEntry>* out);
void unpack_nack(const std::vector<NackEntry>& e, std::vector<uint64_t>* seqs);

// ---- kPli: last_clean u32, reason u8, rsv u8
enum PliReason : uint8_t { kPliLoss = 0, kPliStart = 1, kPliDecoder = 2, kPliStalled = 3, kPliSize = 4 };
struct Pli {
  uint32_t last_clean = kNoFrame;
  uint8_t reason = kPliLoss;
};
constexpr size_t kPliLen = 6;
void write_pli(const Pli& p, uint8_t out[kPliLen]);
bool read_pli(const uint8_t* p, size_t len, Pli* out);  // false: short or reason > 4
const char* pli_reason_name(uint8_t r);

// ---- kTimeMap (flow control, stream 0): n u8 (1..32), then n x (stream u32, ts u32, sender_ns u64)
struct TimeMapEntry {
  uint32_t stream = 0, ts = 0;
  uint64_t sender_ns = 0;
};
constexpr size_t kMaxTimeMapEntries = 32;
size_t write_time_map(const std::vector<TimeMapEntry>& e, uint8_t* out);  // <= 32 entries
bool read_time_map(const uint8_t* p, size_t len, std::vector<TimeMapEntry>* out);

// ---- video kRR, 28 B
struct VideoRR {
  uint32_t newest_ts = 0;
  uint16_t margin = 0;  // min playout margin over the interval, 0.1 ms, 0 = late
  uint32_t hi_seq_lo = 0, lost = 0;
  uint16_t late = 0;
  uint32_t clean_since = kNoFrame, acked = kNoFrame;
  uint16_t recv_kbps = 0;
  uint8_t flags = 0;  // bit0 broken now
};
constexpr size_t kVideoRRLen = 28;
void write_video_rr(const VideoRR& r, uint8_t out[kVideoRRLen]);
bool read_video_rr(const uint8_t* p, size_t len, VideoRR* out);

// Modulo-2^32 helpers for ts and frame numbers.
inline int32_t ts_diff(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b); }

}  // namespace beamer::audio
