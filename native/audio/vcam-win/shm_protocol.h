// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <atomic>
#include <cstdint>
namespace beamer::vcam {
// {19FCB9D1-E91E-45D0-B865-75D8F6D98A91}: the media source's COM class (HKLM\Software\Classes\CLSID)
constexpr wchar_t kClsidText[] = L"{19FCB9D1-E91E-45D0-B865-75D8F6D98A91}";
constexpr wchar_t kFriendlyName[] = L"Beamer Camera";
constexpr wchar_t kRegKey[] = L"SOFTWARE\\Beamer\\Camera";      // HKLM: InstallGuid, OwnerSid, DllVersion, DllPath
// Kernel objects: L"Global\\BeamerCamera.<InstallGuid>.Shm" / ".Frame" (both created by the source)
//                 L"Global\\BeamerCamera.<InstallGuid>.Demand" (created by the helper)
// DACL on all three: SYSTEM, LocalService, OwnerSid (full); nobody else. Creators check ownership (§4.6.3).
constexpr uint32_t kMagic = 0x4D414342;   // 'BCAM'
constexpr uint32_t kVersion = 1;          // bump only on a layout change; the helper supports [1, its max]
constexpr int kDllVersion = 1;            // bump on any DLL change that needs a reinstall
constexpr uint32_t kMinW = 64, kMinH = 64, kMaxW = 1920, kMaxH = 1080;   // DISPLAY sizes (1080p is coded 1088)
struct MediaType { uint32_t w, h, fps; };
// The media types the source offers, in this order (the first is the default); the helper accepts a `want_*`
// only if it is one of these, else uses kOffered[0]. Adding one (e.g. 640x480 for an app) changes this table
// and bumps kDllVersion: an operator decision, never a local change in CW4 or CW5.
constexpr MediaType kOffered[] = {{1280, 720, 30}, {1920, 1080, 30}, {640, 360, 30}};
constexpr uint32_t kSlotBytes = 3112960;  // 1920*1080*3/2 rounded up to 4096
constexpr uint32_t kLiveSlots = 3;        // + 1 placeholder slot (index 3)
constexpr uint32_t kSlotCount = 4;
constexpr uint64_t kDataOffset = 4096;    // slot i's pixels: kDataOffset + i * kSlotBytes (fixed, never stored)
constexpr uint64_t kSectionBytes = kDataOffset + uint64_t(kSlotCount) * kSlotBytes;
enum Mode : uint32_t { kModeNone = 0, kModeLive = 1, kModeHold = 2, kModeWaiting = 3 };
struct alignas(64) Slot {                 // written by the helper
  std::atomic<uint32_t> seq;              // seqlock: odd while the helper writes
  uint32_t w, h, stride;                  // NV12, display size: Y at the slot's pixels, UV at + stride*h
                                          // (never the decoder's coded height, §4.6.1)
  uint64_t written_qpc;
  uint32_t picture;                       // helper's picture counter
  uint32_t rsv[9];
};
struct alignas(64) Header {
  uint32_t magic, version, header_bytes, slot_count;   // written by the source at creation; never read back by it
  // written by the source
  std::atomic<uint32_t> state_seq;        // +1 on every change below; the source then sets the Demand event
  uint32_t streaming;                     // 1 between Start and Stop/Shutdown (repeated stops are harmless)
  uint32_t want_w, want_h, want_fps;      // the app's media type
  uint64_t state_qpc;
  uint32_t dll_version, source_pid;       // dll_version = the running DLL's kDllVersion (= loaded_version)
  std::atomic<uint64_t> delivered, placeholders, repeats, rejected;   // rejected: helper data refused (§4.6.3)
  // written by the helper (hostile input for the source, §4.6.3)
  uint32_t helper_pid;                    // informational only
  std::atomic<uint64_t> beat_qpc;         // written by the thread that presents (§4.6.4)
  std::atomic<uint32_t> mode;             // Mode
  std::atomic<uint32_t> latest;           // live slot with the newest picture
  uint32_t rsv[16];
  Slot slots[kSlotCount];
};
static_assert(sizeof(Header) <= kDataOffset, "header must fit before the pixels");
}
