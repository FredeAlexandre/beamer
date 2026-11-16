// SPDX-License-Identifier: GPL-3.0-only
// Hostile writer for the Beamer Camera shared memory (PHASE2 §4.6.3, CW5 acceptance). Runs as the owner (an
// ordinary process of the user's session, like any program that user runs) and writes random and boundary
// values into every helper-written field at --hz while an app reads the camera:
//
//   shmfuzz [--seconds N] [--hz 1000] [--fuzz-ms 1500] [--legit-ms 1000] [--seed S]
//
// Phases alternate: FUZZ (every helper field, every tick: helper_pid, beat_qpc, mode, latest, and per slot
// seq, w, h, stride, written_qpc, picture, rsv; the frame event is set every tick) and LEGIT (a well-behaved
// helper: beat, mode live, a C2-stamped 1280x720 picture at 30 fps). During FUZZ the slots' pixels hold one
// value (0x50 in the live slots, 0x60 in slot 3), filled under an odd seq at the phase start, so any slot the
// source accepts shows as a uniform picture; camprobe then classifies every frame as builtin, helper (a
// stamped picture, fresh or previous), uniform or other. Expected: other = 0, Frame Server alive, rejected up.
#include "tool_common.h"
#include <random>

using namespace tools;

int wmain(int argc, wchar_t** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  double seconds = 60, hz = 1000, fuzz_ms = 1500, legit_ms = 1000;
  unsigned seed = (unsigned)GetTickCount();
  for (int i = 1; i + 1 < argc; i++) {
    std::wstring a = argv[i];
    if (a == L"--seconds") seconds = _wtof(argv[++i]);
    else if (a == L"--hz") hz = _wtof(argv[++i]);
    else if (a == L"--fuzz-ms") fuzz_ms = _wtof(argv[++i]);
    else if (a == L"--legit-ms") legit_ms = _wtof(argv[++i]);
    else if (a == L"--seed") seed = (unsigned)_wtoi(argv[++i]);
  }
  Names n = names();
  Section sec;
  if (!n.ok || !sec.open(n)) {
    printf("{\"ok\":false,\"error\":\"section not open (no app streaming, or not installed)\",\"err\":%lu}\n", GetLastError());
    return 1;
  }
  vc::Header* h = sec.hdr;
  std::mt19937_64 rng(seed);
  auto r64 = [&]() { return (uint64_t)rng(); };
  auto pick = [&](std::initializer_list<uint64_t> l) { return *(l.begin() + (size_t)(rng() % l.size())); };
  const uint64_t f = (uint64_t)qpc_freq();
  auto u32 = [&]() -> uint32_t {   // boundaries of the §4.6.3 checks, and noise
    switch (rng() % 3) {
      case 0: return (uint32_t)r64();
      case 1: return (uint32_t)(rng() % 5000);
      default:
        return (uint32_t)pick({0, 1, 2, 3, 4, 63, 64, 65, 66, 506, 507, 508, 640, 720, 1079, 1080, 1081, 1082, 1088, 1279, 1280, 1918, 1919, 1920,
                               1921, 1922, 4094, 4095, 4096, 4097, 8192, 0x7fffffff, 0x80000000, 0xfffffffe, 0xffffffff});
    }
  };
  auto beat = [&]() -> uint64_t {
    uint64_t now = (uint64_t)qpc_now();
    switch (rng() % 3) {
      case 0: return r64();
      case 1: return now - f * (uint64_t)(rng() % 3000) / 1000 + f * (uint64_t)(rng() % 3000) / 1000;
      default:
        return pick({0, 1, now, now - f / 2 + f / 1000, now - f / 2 - f / 1000, now + f - f / 1000, now + f + f / 1000, now + f * 1000,
                     UINT64_MAX, (uint64_t)INT64_MAX, (uint64_t)INT64_MAX + 1});
    }
  };
  uint64_t rej0 = h->rejected.load(), del0 = h->delivered.load(), ph0 = h->placeholders.load();
  printf("{\"event\":\"start\",\"t\":%lld,\"seed\":%u,\"rejected\":%llu,\"delivered\":%llu}\n", (long long)unix_us(), seed, (unsigned long long)rej0,
         (unsigned long long)del0);
  timeBeginPeriod(1);
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  const int64_t period = (int64_t)(qpc_freq() / hz);
  int64_t t_end = unix_us() + (int64_t)(seconds * 1e6), phase_end = 0;
  bool fuzz = false;
  uint64_t ticks = 0, fuzz_ticks = 0, legit_frames = 0, phases = 0;
  uint32_t latest = 0, pn = 0;
  int64_t next_legit = 0, due = qpc_now();
  std::vector<uint8_t> pic((size_t)1280 * 720 * 3 / 2);
  while (unix_us() < t_end) {
    int64_t now = qpc_now();
    if (now >= phase_end) {
      fuzz = !fuzz;
      phases++;
      phase_end = now + (int64_t)((fuzz ? fuzz_ms : legit_ms) * qpc_freq() / 1000);
      if (fuzz) {
        // uniform pixels, written under an odd seq so the source never accepts a half-filled slot
        for (uint32_t i = 0; i < vc::kSlotCount; i++) {
          uint32_t q = h->slots[i].seq.load();
          h->slots[i].seq.store(q | 1);
          memset(sec.pixels(i), i < vc::kLiveSlots ? 0x50 : 0x60, vc::kSlotBytes);
          h->slots[i].seq.store((q | 1) + 1);
        }
      }
      next_legit = now;
    }
    if (fuzz) {
      h->helper_pid = u32();
      h->beat_qpc.store(beat());
      h->mode.store(rng() % 2 ? (uint32_t)(rng() % 5) : u32());
      h->latest.store(rng() % 2 ? (uint32_t)(rng() % 5) : u32());
      for (uint32_t i = 0; i < vc::kSlotCount; i++) {
        vc::Slot& s = h->slots[i];
        s.seq.store(rng() % 4 ? (uint32_t)(rng() % 64) : u32());
        if (rng() % 3) {   // mostly plausible geometry, so the accepting path runs too
          uint32_t w = 64 + 2 * (uint32_t)(rng() % 929), hh = 64 + 2 * (uint32_t)(rng() % 509);
          s.w = rng() % 8 ? w : u32();
          s.h = rng() % 8 ? hh : u32();
          s.stride = rng() % 2 ? w + 2 * (uint32_t)(rng() % 64) : u32();
        } else {
          s.w = u32();
          s.h = u32();
          s.stride = u32();
        }
        s.written_qpc = r64();
        s.picture = u32();
        for (auto& x : s.rsv) x = u32();
      }
      for (auto& x : h->rsv) x = u32();
      fuzz_ticks++;
    } else if (now >= next_legit) {
      next_legit += qpc_freq() / 30;
      uint32_t next = (latest + 1) % vc::kLiveSlots;
      uint32_t c = pn++;
      pattern_nv12(pic.data(), 1280, 1280, 720, c);
      stamp_nv12(pic.data(), 1280, 1280, 720, c, (uint32_t)unix_us(), kOriginHelper);
      sec.write_slot(next, 1280, 720, 1280, c, [&](uint8_t* p) { memcpy(p, pic.data(), pic.size()); });
      latest = next;
      h->latest.store(next);
      h->mode.store(vc::kModeLive);
      legit_frames++;
    }
    if (!fuzz) h->beat_qpc.store((uint64_t)qpc_now());
    if (sec.frame) SetEvent(sec.frame);
    ticks++;
    due += period;
    now = qpc_now();
    if (due < now - period * 50) due = now;
    if (due > now) {
      LARGE_INTEGER li;
      li.QuadPart = -(int64_t)((double)(due - now) * 1e7 / qpc_freq());
      SetWaitableTimer(timer, &li, 0, nullptr, nullptr, FALSE);
      WaitForSingleObject(timer, INFINITE);
    }
  }
  // leave a sane state: no beat (the source shows its built-in image)
  h->beat_qpc.store(0);
  h->mode.store(vc::kModeNone);
  uint64_t rej1 = h->rejected.load(), del1 = h->delivered.load(), ph1 = h->placeholders.load();
  printf("{\"event\":\"end\",\"t\":%lld,\"ok\":true,\"ticks\":%llu,\"fuzz_ticks\":%llu,\"legit_frames\":%llu,\"phases\":%llu,\"rate_hz\":%.0f,"
         "\"rejected_delta\":%llu,\"delivered_delta\":%llu,\"placeholders_delta\":%llu}\n",
         (long long)unix_us(), (unsigned long long)ticks, (unsigned long long)fuzz_ticks, (unsigned long long)legit_frames, (unsigned long long)phases,
         ticks / seconds, (unsigned long long)(rej1 - rej0), (unsigned long long)(del1 - del0), (unsigned long long)(ph1 - ph0));
  return 0;
}
