// SPDX-License-Identifier: GPL-3.0-only
// C2 prototype (Beamer Camera on Windows): shared definitions for the media source DLL, the feeder
// (user-session helper stand-in), the registrar and the probe.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

namespace c2 {

// {EF41A4B9-7102-4C33-A218-3AF46FED9B0D}
static const GUID CLSID_BeamerCamC2 = {0xef41a4b9, 0x7102, 0x4c33, {0xa2, 0x18, 0x3a, 0xf4, 0x6f, 0xed, 0x9b, 0x0d}};
static const wchar_t* kClsidStr = L"{EF41A4B9-7102-4C33-A218-3AF46FED9B0D}";
static const wchar_t* kFriendlyName = L"Beamer Camera";

// Named kernel objects. The section is created by the media source (Frame Server runs as LocalService,
// which holds SeCreateGlobalPrivilege; a user-session process does not). The demand event is created by
// the helper (events in Global\ need no privilege) and signalled by the source.
static const wchar_t* kShmName = L"Global\\BeamerCamC2.Shm";
static const wchar_t* kFrameEventName = L"Global\\BeamerCamC2.Frame";    // helper -> source: new frame
static const wchar_t* kDemandEventName = L"Global\\BeamerCamC2.Demand";  // source -> helper: state changed
// SYSTEM, LocalService, Administrators, Interactive users: full access. Medium integrity label is implicit.
static const wchar_t* kSddl = L"D:(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;BA)(A;;GA;;;IU)";

static const char* kDataDir = "C:\\ProgramData\\BeamerCamC2";

inline int64_t now_us() {  // wall clock, microseconds since 1601; same clock in every process and in JS (Date)
  FILETIME ft;
  GetSystemTimePreciseAsFileTime(&ft);
  return (int64_t)((((uint64_t)ft.dwHighDateTime) << 32 | ft.dwLowDateTime) / 10);
}
inline int64_t unix_us() { return now_us() - 11644473600000000LL; }

// ---- Shared memory layout -------------------------------------------------------------------------
constexpr uint32_t kMagic = 0x43324243;  // "CB2C"
constexpr uint32_t kSlots = 3;
constexpr uint32_t kMaxW = 1920, kMaxH = 1080;
constexpr uint32_t kSlotBytes = kMaxW * kMaxH * 3 / 2;  // NV12

struct alignas(64) Slot {
  volatile LONG64 seq;      // seqlock: odd while the helper writes
  volatile LONG64 frame;    // helper frame number
  int64_t write_us;         // helper time when the write completed
  uint32_t width, height;   // NV12, stride == width
  uint32_t pad[8];
};

struct alignas(64) Header {
  uint32_t magic, version;
  // source -> helper
  volatile LONG streaming;       // 1 while a consumer streams (source Start .. Stop)
  volatile LONG want_w, want_h;  // media type the consumer picked
  volatile LONG64 state_seq;     // bumped at every state change
  int64_t state_us;              // time of the last change (source clock = wall clock)
  volatile LONG64 source_heartbeat_us;
  volatile LONG64 delivered;     // samples handed to Frame Server
  volatile LONG64 placeholders;  // of which placeholders
  volatile LONG64 dropped;       // helper frames never delivered (overwritten before a request)
  uint32_t pad0[6];
  // helper -> source
  volatile LONG latest;          // index of the newest complete slot, -1 none
  volatile LONG64 helper_heartbeat_us;
  uint32_t pad1[12];
  Slot slots[kSlots];
};
constexpr size_t kShmBytes = ((sizeof(Header) + 4095) & ~4095ull) + (size_t)kSlots * kSlotBytes;
inline uint8_t* slot_data(Header* h, uint32_t i) {
  return (uint8_t*)h + ((sizeof(Header) + 4095) & ~4095ull) + (size_t)i * kSlotBytes;
}

inline bool make_sa(SECURITY_ATTRIBUTES* sa) {
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kSddl, SDDL_REVISION_1, &sd, nullptr)) return false;
  sa->nLength = sizeof(*sa);
  sa->lpSecurityDescriptor = sd;  // leaked on purpose (tiny, process lifetime)
  sa->bInheritHandle = FALSE;
  return true;
}

// ---- Frame stamp ----------------------------------------------------------------------------------
// Three rows of 40 blocks in the top-left of the frame; a block is (W/40) x (H/22) pixels, white = 1.
// Row 0: magic 0xA5 (8 bits) + frame counter (32). Row 1: 0x5A + low 32 bits of unix time in us.
// Row 2: 0xC3 + origin (8: 1 = source test pattern, 2 = source placeholder, 3 = helper) + 24 spare.
// Bits are MSB first. Decoding samples the block centres and thresholds luma at 128, so it survives
// scaling, NV12->RGB conversion and H.264 at normal bitrates.
constexpr int kStampCols = 40, kStampRowsDiv = 22;
enum Origin : uint8_t { kOriginPattern = 1, kOriginPlaceholder = 2, kOriginHelper = 3 };

inline void stamp_nv12(uint8_t* y, int stride, int w, int h, uint32_t counter, uint32_t time_lo, uint8_t origin) {
  uint64_t rows[3] = {(0xA5ull << 32) | counter, (0x5Aull << 32) | time_lo, (0xC3ull << 32) | ((uint64_t)origin << 24)};
  int bw = w / kStampCols, bh = h / kStampRowsDiv;
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < kStampCols; c++) {
      uint8_t v = ((rows[r] >> (39 - c)) & 1) ? 235 : 16;
      for (int yy = r * bh; yy < (r + 1) * bh; yy++) memset(y + (size_t)yy * stride + c * bw, v, bw);
    }
  // chroma under the stamp: neutral, so RGB conversion gives pure grey levels
  uint8_t* uv = y + (size_t)stride * h;
  for (int yy = 0; yy < 3 * bh / 2 + 1; yy++) memset(uv + (size_t)yy * stride, 128, kStampCols * bw);
}

// luma(x, y) callback form, so the probe can decode NV12, YUY2 or RGB32
template <class F>
inline bool unstamp(int w, int h, F luma, uint32_t* counter, uint32_t* time_lo, uint8_t* origin) {
  int bw = w / kStampCols, bh = h / kStampRowsDiv;
  uint64_t rows[3] = {0, 0, 0};
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < kStampCols; c++) rows[r] = (rows[r] << 1) | (luma(c * bw + bw / 2, r * bh + bh / 2) >= 128 ? 1 : 0);
  if ((rows[0] >> 32) != 0xA5 || (rows[1] >> 32) != 0x5A || (rows[2] >> 32) != 0xC3) return false;
  *counter = (uint32_t)rows[0];
  *time_lo = (uint32_t)rows[1];
  *origin = (uint8_t)(rows[2] >> 24);
  return true;
}

// Body of a test frame: moving diagonal bars, so a frozen picture is obvious.
inline void pattern_nv12(uint8_t* y, int stride, int w, int h, uint32_t n, bool placeholder) {
  for (int yy = 0; yy < h; yy++) {
    uint8_t* row = y + (size_t)yy * stride;
    if (placeholder) {
      memset(row, 48, w);
      int bar = (int)((n * 8) % (unsigned)w);
      if (yy > h / 2 - 8 && yy < h / 2 + 8) memset(row + (bar > w - 64 ? w - 64 : bar), 200, 64);
    } else {
      for (int x = 0; x < w; x++) row[x] = (uint8_t)(((x + yy + (int)n * 4) & 255) / 2 + 64);
    }
  }
  uint8_t* uv = y + (size_t)stride * h;
  for (int yy = 0; yy < h / 2; yy++) {
    uint8_t* row = uv + (size_t)yy * stride;
    for (int x = 0; x < w; x += 2) {
      row[x] = placeholder ? 128 : (uint8_t)(128 + ((x + (int)n) & 63) - 32);
      row[x + 1] = placeholder ? 128 : (uint8_t)(128 + ((yy * 2) & 63) - 32);
    }
  }
}

// Nearest-neighbour NV12 scale (used when the helper's size differs from the consumer's type).
inline void scale_nv12(const uint8_t* src, int sw, int sh, uint8_t* dst, int dstride, int dw, int dh) {
  for (int y = 0; y < dh; y++) {
    const uint8_t* s = src + (size_t)(y * sh / dh) * sw;
    uint8_t* d = dst + (size_t)y * dstride;
    for (int x = 0; x < dw; x++) d[x] = s[x * sw / dw];
  }
  const uint8_t* suv = src + (size_t)sw * sh;
  uint8_t* duv = dst + (size_t)dstride * dh;
  for (int y = 0; y < dh / 2; y++) {
    const uint16_t* s = (const uint16_t*)(suv + (size_t)(y * sh / dh) * sw);
    uint16_t* d = (uint16_t*)(duv + (size_t)y * dstride);
    for (int x = 0; x < dw / 2; x++) d[x] = s[x * sw / dw];
  }
}

inline void nv12_to_yuy2(const uint8_t* y, int ystride, int w, int h, uint8_t* dst, int dstride) {
  const uint8_t* uv = y + (size_t)ystride * h;
  for (int r = 0; r < h; r++) {
    const uint8_t* yr = y + (size_t)r * ystride;
    const uint8_t* uvr = uv + (size_t)(r / 2) * ystride;
    uint8_t* d = dst + (size_t)r * dstride;
    for (int x = 0; x < w; x += 2) {
      d[2 * x] = yr[x];
      d[2 * x + 1] = uvr[x];
      d[2 * x + 2] = yr[x + 1];
      d[2 * x + 3] = uvr[x + 1];
    }
  }
}

}  // namespace c2
