// SPDX-License-Identifier: GPL-3.0-only
// C4 end-to-end prototype: the wire between the Mac sender (camsend) and the PC receiver (camrecv).
//
// Sizes follow C3's proposal: a video packet is a 24 B "engine" header + 12 B video header + <= 1148 B
// of the Annex-B access unit + 16 B of (zero) AEAD tag = <= 1200 B of UDP payload. No crypto: the bytes
// have the sealed sizes. Fields are little endian (both machines are). The 24 B header carries what
// the prototype needs to measure: capture time and encoder-output time on the Mac's host clock.
#pragma once
#include <stdint.h>

namespace e2e {

enum PType : uint8_t {
  kData = 1,   // Mac -> PC  video fragment
  kHello = 2,  // both       punch / keepalive
  kPing = 3,   // PC -> Mac  clock probe
  kPong = 4,   // Mac -> PC
  kStart = 5,  // PC -> Mac  a PC app opened Beamer Camera (repeated until kState says running)
  kStop = 6,   // PC -> Mac  the app closed it (repeated until kState says stopped)
  kState = 7,  // Mac -> PC  answer to start/stop
  kRR = 8,     // PC -> Mac  receiver report every 10 ms: LTR acks, counters (also keeps the PC radio awake)
  kNack = 9,   // PC -> Mac  missing sequence numbers
  kPli = 10,   // PC -> Mac  cannot decode: please refresh (LTR refresh, else IDR)
  kAudio = 11, // Mac -> PC  audio-like 80 B packet every 10 ms on the same socket (optional)
};

enum VFlags : uint8_t { kIdr = 1, kRefresh = 2, kHasToken = 4, kBehindIdr = 8, kRetransmit = 16 };

constexpr int kMaxUdp = 1200, kTag = 16;

#pragma pack(push, 1)
struct DataHdr {           // 36 B
  uint8_t ptype;           // kData
  uint8_t codec;           // 2 = H.264
  uint16_t sess;           // stream session (bumped at every start)
  uint32_t seq;            // per session, every packet; retransmissions resend the identical bytes
  uint64_t cap_ns;         // Mac host clock: capture (synthetic: frame finished) time
  uint64_t out_ns;         // Mac host clock: encoder output callback
  uint32_t frame;          // access unit number (global across sessions)
  uint16_t frag, nfrag;
  uint8_t vflags;
  uint8_t ltr;             // low bits of the LTR token of this frame
  uint16_t rsv;
};
constexpr int kMaxFrag = kMaxUdp - (int)sizeof(DataHdr) - kTag;  // 1148

struct Hello { uint8_t ptype, role; uint16_t rsv; uint32_t boot; };
struct Ping { uint8_t ptype, rsv[3]; uint32_t id; int64_t pc_us; };
struct Pong { uint8_t ptype, rsv[3]; uint32_t id; int64_t pc_us; uint64_t mac_ns; };
struct Start {
  uint8_t ptype, idr_pace_x10;  // IDR pacing in tenths of the bitrate (25 = 2.5x)
  uint16_t w, h, key_every;     // key_every: force an IDR every N frames (test only, 0 = never)
  uint32_t req, bitrate;
  uint8_t audio, rsv[3];
};
struct Stop { uint8_t ptype, rsv[3]; uint32_t req; };
struct State { uint8_t ptype, running; uint16_t sess; uint32_t req; uint64_t mac_ns; };
struct RR {                // 46 B like the engine's
  uint8_t ptype, flags;    // flags bit0: decoder broken (waiting for a refresh)
  uint16_t sess;
  uint32_t acked_frame;    // newest frame decoded cleanly
  uint32_t clean_from;     // first frame of the current clean run: every frame in [clean_from, acked_frame] decoded cleanly
  uint32_t hi_seq, lost, late, shown;
  uint16_t playout_ms;
  uint8_t pad[16];
};
struct Nack { uint8_t ptype, n; uint16_t sess; uint32_t seq[32]; };
struct Pli { uint8_t ptype, reason; uint16_t sess; uint32_t lost_frame, last_clean; };
struct Audio { uint8_t ptype, rsv[3]; uint32_t seq; uint64_t send_ns; uint8_t pad[64]; };
#pragma pack(pop)

static_assert(sizeof(DataHdr) == 36, "data header");
static_assert(sizeof(RR) == 46, "rr");
static_assert(sizeof(Audio) == 80, "audio");

constexpr uint16_t kPcPort = 24860, kMacPort = 40160;  // outside Beamer's 24823-24830 and C3's 24850/40150

}  // namespace e2e
