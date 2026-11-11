// SPDX-License-Identifier: GPL-3.0-only
// Shared by the vcam-win test tools (camprobe, feeder, shmfuzz): object names from the install key, clocks,
// the C2 frame stamp (counter + wall-clock time + origin), C1's code strip reader, NV12 test patterns and
// security-descriptor helpers. Not used by the DLL.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../shm_protocol.h"

namespace tools {
namespace vc = beamer::vcam;

inline int64_t unix_us() {   // wall clock, same in every process on the PC (and in a browser's Date)
  FILETIME ft;
  GetSystemTimePreciseAsFileTime(&ft);
  return (int64_t)((((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10) - 11644473600000000LL;
}
inline int64_t qpc_freq() {
  static const int64_t f = [] {
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li);
    return li.QuadPart;
  }();
  return f;
}
inline int64_t qpc_now() {
  LARGE_INTEGER li;
  QueryPerformanceCounter(&li);
  return li.QuadPart;
}
inline double qpc_to_ms(int64_t d) { return (double)d * 1000.0 / (double)qpc_freq(); }

inline std::string narrow(const std::wstring& w) {
  if (w.empty()) return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? n - 1 : 0, '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
  for (auto& c : s)
    if (c == '\\' || c == '"') c = '/';
  return s;
}

// ---- names (HKLM\SOFTWARE\Beamer\Camera InstallGuid, used verbatim like the DLL does) ----------------------
struct Names {
  std::wstring guid, owner, shm, frame, demand;
  bool ok = false;
};
inline Names names(const wchar_t* guid_override = nullptr) {
  Names n;
  wchar_t buf[200];
  DWORD sz = sizeof buf;
  if (guid_override) n.guid = guid_override;
  else if (RegGetValueW(HKEY_LOCAL_MACHINE, vc::kRegKey, L"InstallGuid", RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS) n.guid = buf;
  sz = sizeof buf;
  if (RegGetValueW(HKEY_LOCAL_MACHINE, vc::kRegKey, L"OwnerSid", RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS) n.owner = buf;
  if (n.guid.empty()) return n;
  std::wstring base = L"Global\\BeamerCamera." + n.guid;
  n.shm = base + L".Shm";
  n.frame = base + L".Frame";
  n.demand = base + L".Demand";
  n.ok = true;
  return n;
}

inline std::wstring my_sid() {
  HANDLE tok;
  std::wstring r;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return r;
  BYTE buf[256];
  DWORD n;
  LPWSTR s = nullptr;
  if (GetTokenInformation(tok, TokenUser, buf, sizeof buf, &n) && ConvertSidToStringSidW(((TOKEN_USER*)buf)->User.Sid, &s)) {
    r = s;
    LocalFree(s);
  }
  CloseHandle(tok);
  return r;
}

// Owner and DACL of a kernel object, as SDDL ("" if unreadable).
inline std::string object_sddl(HANDLE h, std::string* owner_out = nullptr) {
  PSECURITY_DESCRIPTOR sd = nullptr;
  PSID owner = nullptr;
  if (GetSecurityInfo(h, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &sd) !=
      ERROR_SUCCESS)
    return "";
  LPWSTR s = nullptr, o = nullptr;
  std::string r;
  if (ConvertSecurityDescriptorToStringSecurityDescriptorW(sd, SDDL_REVISION_1, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &s, nullptr)) {
    r = narrow(s);
    LocalFree(s);
  }
  if (owner_out && owner && ConvertSidToStringSidW(owner, &o)) {
    *owner_out = narrow(o);
    LocalFree(o);
  }
  LocalFree(sd);
  return r;
}

// ---- C2 stamp: three rows of 40 blocks top-left; block = (W/40) x (H/22); white = 1, MSB first ------------
// Row 0: 0xA5 + counter (32). Row 1: 0x5A + low 32 bits of unix µs. Row 2: 0xC3 + origin (8) + 24 spare.
enum Origin : uint8_t { kOriginPattern = 1, kOriginPlaceholder = 2, kOriginHelper = 3 };
constexpr int kStampCols = 40, kStampRowsDiv = 22;

inline void stamp_nv12(uint8_t* y, size_t stride, int w, int h, uint32_t counter, uint32_t time_lo, uint8_t origin) {
  uint64_t rows[3] = {(0xA5ull << 32) | counter, (0x5Aull << 32) | time_lo, (0xC3ull << 32) | ((uint64_t)origin << 24)};
  int bw = w / kStampCols, bh = h / kStampRowsDiv;
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < kStampCols; c++) {
      uint8_t v = ((rows[r] >> (39 - c)) & 1) ? 235 : 16;
      for (int yy = r * bh; yy < (r + 1) * bh; yy++) memset(y + (size_t)yy * stride + (size_t)c * bw, v, bw);
    }
  uint8_t* uv = y + stride * h;
  for (int yy = 0; yy < 3 * bh / 2 + 1; yy++) memset(uv + (size_t)yy * stride, 128, (size_t)kStampCols * bw);
}

template <class F>
inline bool unstamp(int w, int h, F luma, uint32_t* counter, uint32_t* time_lo, uint8_t* origin) {
  int bw = w / kStampCols, bh = h / kStampRowsDiv;
  if (bw < 1 || bh < 1) return false;
  uint64_t rows[3] = {0, 0, 0};
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < kStampCols; c++) rows[r] = (rows[r] << 1) | (luma(c * bw + bw / 2, r * bh + bh / 2) >= 128 ? 1 : 0);
  if ((rows[0] >> 32) != 0xA5 || (rows[1] >> 32) != 0x5A || (rows[2] >> 32) != 0xC3) return false;
  *counter = (uint32_t)rows[0];
  *time_lo = (uint32_t)rows[1];
  *origin = (uint8_t)(rows[2] >> 24);
  return true;
}

// ---- C1 code strip (dev/camera/proto/mac/README.md): 80 blocks of B = W/80 px on rows [0, B) -----------------
// bytes: 0xA5, frame (u32 BE), Mac host time / 100 µs (u32 BE), xor of bytes 1-8.
template <class F>
inline bool read_c1(int w, int h, F luma, uint32_t* frame, uint32_t* ts100us) {
  int b = w / 80;
  if (b < 1 || h < b) return false;
  uint8_t by[10] = {0};
  for (int i = 0; i < 80; i++) by[i / 8] = (uint8_t)((by[i / 8] << 1) | (luma(i * b + b / 2, b / 2) >= 128 ? 1 : 0));
  if (by[0] != 0xA5) return false;
  uint8_t x = 0;
  for (int i = 1; i <= 8; i++) x ^= by[i];
  if (x != by[9]) return false;
  *frame = ((uint32_t)by[1] << 24) | ((uint32_t)by[2] << 16) | ((uint32_t)by[3] << 8) | by[4];
  *ts100us = ((uint32_t)by[5] << 24) | ((uint32_t)by[6] << 16) | ((uint32_t)by[7] << 8) | by[8];
  return true;
}

// Moving diagonal bars (a frozen picture is obvious), NV12, stride `stride`.
inline void pattern_nv12(uint8_t* y, size_t stride, int w, int h, uint32_t n) {
  for (int yy = 0; yy < h; yy++) {
    uint8_t* row = y + (size_t)yy * stride;
    for (int x = 0; x < w; x++) row[x] = (uint8_t)(((x + yy + (int)n * 4) & 255) / 2 + 64);
  }
  uint8_t* uv = y + stride * h;
  for (int yy = 0; yy < h / 2; yy++) {
    uint8_t* row = uv + (size_t)yy * stride;
    for (int x = 0; x < w; x += 2) {
      row[x] = (uint8_t)(128 + ((x + (int)n) & 63) - 32);
      row[x + 1] = (uint8_t)(128 + ((yy * 2) & 63) - 32);
    }
  }
}
// Uniform picture (value v in every byte, Y and UV), used for slot 3 and by shmfuzz.
inline void uniform_nv12(uint8_t* p, size_t stride, int h, uint8_t v) { memset(p, v, stride * h * 3 / 2); }

inline double pct(std::vector<double> v, double p) {
  if (v.empty()) return -1;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, (size_t)(p * (v.size() - 1) + 0.5))];
}

// ---- the helper's view of the section (feeder, shmfuzz) -------------------------------------------------------
struct Section {
  HANDLE map = nullptr, frame = nullptr;
  vc::Header* hdr = nullptr;
  bool open(const Names& n, bool quiet = false) {
    if (hdr) return true;
    map = OpenFileMappingW(FILE_MAP_WRITE, FALSE, n.shm.c_str());   // FILE_MAP_WRITE = read + write views
    if (!map) return false;
    hdr = (vc::Header*)MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, (SIZE_T)vc::kSectionBytes);
    if (!hdr) {
      CloseHandle(map);
      map = nullptr;
      return false;
    }
    frame = OpenEventW(EVENT_MODIFY_STATE, FALSE, n.frame.c_str());
    bool known = hdr->magic == vc::kMagic && hdr->version == vc::kVersion && hdr->slot_count == vc::kSlotCount;
    if (!quiet)
      printf("{\"event\":\"section_open\",\"t\":%lld,\"known\":%s,\"dll_version\":%u,\"source_pid\":%u,\"frame_event\":%s}\n", (long long)unix_us(),
             known ? "true" : "false", hdr->dll_version, hdr->source_pid, frame ? "true" : "false");
    if (!known) close();
    return hdr != nullptr;
  }
  void close() {
    if (hdr) UnmapViewOfFile(hdr);
    if (map) CloseHandle(map);
    if (frame) CloseHandle(frame);
    hdr = nullptr;
    map = frame = nullptr;
  }
  uint8_t* pixels(uint32_t i) { return (uint8_t*)hdr + vc::kDataOffset + (uint64_t)i * vc::kSlotBytes; }
  // Writes slot i under the seqlock: fill(pixels) then geometry, as the helper does.
  template <class Fill>
  void write_slot(uint32_t i, uint32_t w, uint32_t h, uint32_t stride, uint32_t picture, Fill fill) {
    vc::Slot& s = hdr->slots[i];
    uint32_t q = s.seq.load(std::memory_order_relaxed);
    s.seq.store(q | 1, std::memory_order_relaxed);   // odd: writing
    std::atomic_thread_fence(std::memory_order_release);
    fill(pixels(i));
    s.w = w;
    s.h = h;
    s.stride = stride;
    s.written_qpc = (uint64_t)qpc_now();
    s.picture = picture;
    s.seq.store((q | 1) + 1, std::memory_order_release);   // even: done
  }
};
}  // namespace tools
