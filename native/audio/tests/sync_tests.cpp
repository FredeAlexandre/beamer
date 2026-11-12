// SPDX-License-Identifier: GPL-3.0-only
// Lip-sync tests (camera PHASE2 §4.9, §12 CW10), linked into engine_tests (same CHECK counters, cases appended by
// sync_test_cases()). The policy runs on synthetic time; the presenter cases drive the real VideoRecvCore with
// both clock-offset signs, offset errors, drift and audio path changes; the end-to-end cases run in-process
// helpers with the null device and null video backends over 127.0.0.1.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "codec.h"
#include "engine.h"
#include "helper.h"
#include "json.h"
#include "null_video.h"
#include "packet.h"
#include "sync.h"
#include "util.h"
#include "video_backend.h"
#include "video_packet.h"
#include "video_recv.h"

using namespace beamer::audio;

extern int g_checks, g_failures;

#define SCHECK(c)                                                             \
  do {                                                                        \
    g_checks++;                                                               \
    if (!(c)) {                                                               \
      g_failures++;                                                           \
      std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
    }                                                                         \
  } while (0)
#define SCHECKF(c, ...)                                                       \
  do {                                                                        \
    g_checks++;                                                               \
    if (!(c)) {                                                               \
      g_failures++;                                                           \
      std::fprintf(stderr, "  FAIL %s:%d: %s: ", __FILE__, __LINE__, #c);     \
      std::fprintf(stderr, __VA_ARGS__);                                      \
      std::fprintf(stderr, "\n");                                             \
    }                                                                         \
  } while (0)

namespace {

constexpr int64_t kMs = 1'000'000;
constexpr int64_t kS = 1'000'000'000;
constexpr int64_t kTick = 50 * kMs;  // the helper's tick

double formula(double audio, double video) { return std::min(150.0, std::max(0.0, audio - video)); }

// Runs the policy at the helper's 50 ms tick from `t` for `ns`, with inputs from `in(t)`. Returns the end time.
int64_t drive(LipSyncPolicy& p, int64_t t, int64_t ns, const std::function<LipSyncInput(int64_t)>& in,
              const std::function<void(int64_t, double)>& each = nullptr) {
  const int64_t end = t + ns;
  for (; t < end; t += kTick) {
    const double h = p.update(t, in(t));
    if (each) each(t, h);
  }
  return t;
}

LipSyncInput both(double audio, double video) {
  LipSyncInput in;
  in.audio = in.video = true;
  in.audio_ms = audio;
  in.video_ms = video;
  return in;
}

// ===================================================================== policy (synthetic time)

// hold within +-2 ms of clamp(audio - video, 0, 150) once settled; the skew left after the hold has the right
// sign (negative = audio leads: nothing done)
void test_sync_policy_formula() {
  const double video = 82;  // C4: ~57 ms + enc + D at the floor
  for (double audio : {172.0, 127.0, 120.0, 89.0, 82.0, 60.0, 232.0, 300.0, 600.0, 0.0}) {
    LipSyncPolicy p;
    int64_t t = 5 * kS;
    t = drive(p, t, 30 * kS, [&](int64_t) { return both(audio, video); });
    const double want = formula(audio, video);
    SCHECKF(std::fabs(p.hold_ms() - want) <= 2.0, "audio %.0f video %.0f: hold %.2f want %.2f", audio, video,
            p.hold_ms(), want);
    SCHECK(p.active() && p.have_skew());
    const double skew = p.skew_ms();
    if (audio - video < 0) SCHECKF(skew < -1.0 && std::fabs(skew - (audio - video)) <= 2.0, "skew %.2f", skew);
    else if (audio - video > 150) SCHECKF(std::fabs(skew - (audio - video - 150)) <= 2.0, "skew %.2f", skew);
    else SCHECKF(std::fabs(skew) <= 2.0, "skew %.2f", skew);
  }
}

// never outside [0, 150] whatever the input; never applied before both streams ran 2 s
void test_sync_policy_bounds() {
  std::mt19937 rng(7);
  std::uniform_real_distribution<double> u(-500, 800);
  LipSyncPolicy p;
  int64_t t = kS;
  double lo = 1e9, hi = -1e9;
  t = drive(p, t, 600 * kS, [&](int64_t) { return both(u(rng), u(rng) / 4); }, [&](int64_t, double h) {
    lo = std::min(lo, h);
    hi = std::max(hi, h);
  });
  SCHECKF(lo >= 0.0 && hi <= 150.0, "hold range [%.2f, %.2f]", lo, hi);
  // saturation both ways, held there
  LipSyncPolicy a, b;
  drive(a, kS, 60 * kS, [](int64_t) { return both(900, 50); });
  drive(b, kS, 60 * kS, [](int64_t) { return both(10, 140); });
  SCHECKF(a.hold_ms() == 150.0 && b.hold_ms() == 0.0, "%.2f %.2f", a.hold_ms(), b.hold_ms());
  SCHECKF(b.skew_ms() < -125.0, "audio leads: skew %.2f", b.skew_ms());
}

// smoothing: EWMA 2 s on the difference, the applied hold changes by at most 10 ms per second
void test_sync_policy_smoothing() {
  // step 0 -> 100 ms difference after warm-up: hold rises at 10 ms/s, never faster
  LipSyncPolicy p;
  int64_t t = kS;
  t = drive(p, t, 10 * kS, [](int64_t) { return both(80, 80); });
  SCHECK(p.active() && p.hold_ms() == 0.0);
  std::vector<std::pair<int64_t, double>> h;
  const int64_t step_at = t;
  double max_rate = 0;
  double prev = p.hold_ms();
  int64_t prev_t = t;
  t = drive(p, t, 30 * kS, [](int64_t) { return both(180, 80); }, [&](int64_t now, double v) {
    h.emplace_back(now, v);
    if (now > prev_t) max_rate = std::max(max_rate, std::fabs(v - prev) / (double(now - prev_t) * 1e-9));
    prev = v;
    prev_t = now;
  });
  SCHECKF(max_rate <= 10.0 + 1e-6, "max slew %.4f ms/s", max_rate);
  for (auto& x : h) {
    // 1.05 s and 5.05 s of slew since the last tick before the step
    if (x.first - step_at == 1 * kS) SCHECKF(x.second <= 10.5 + 1e-6 && x.second >= 9.5, "after 1 s: %.2f", x.second);
    if (x.first - step_at == 5 * kS) SCHECKF(x.second <= 50.5 + 1e-6 && x.second >= 45.0, "after 5 s: %.2f", x.second);
  }
  SCHECKF(std::fabs(p.hold_ms() - 100) <= 2.0, "settled %.2f", p.hold_ms());
  // the EWMA: 63 % of a step after 2 s (time constant), +-3 %
  LipSyncPolicy q;
  t = drive(q, kS, 10 * kS, [](int64_t) { return both(100, 100); });
  t = drive(q, t, 2 * kS + kTick, [](int64_t) { return both(200, 100); });
  SCHECKF(std::fabs(q.diff_ms() - 63.2) <= 3.0, "EWMA after 2 s: %.2f", q.diff_ms());
  // one 50 ms outlier of +200 ms (a JB spike) moves the target by <= 2.5 % of it
  LipSyncPolicy r;
  t = drive(r, kS, 30 * kS, [](int64_t) { return both(150, 100); });
  const double before = r.target_ms();
  r.update(t, both(350, 100));
  SCHECKF(r.target_ms() - before <= 5.0 + 0.01 && r.target_ms() - before > 0, "outlier moved target by %.2f",
          r.target_ms() - before);
  // a hold going down (path change, D spike) also moves at <= 10 ms/s
  prev = r.hold_ms();
  prev_t = t;
  max_rate = 0;
  drive(r, t + kTick, 20 * kS, [](int64_t) { return both(100, 100); }, [&](int64_t now, double v) {
    max_rate = std::max(max_rate, std::fabs(v - prev) / (double(now - prev_t) * 1e-9));
    prev = v;
    prev_t = now;
  });
  SCHECKF(max_rate <= 10.0 + 1e-6 && r.hold_ms() <= 2.0, "down: max slew %.4f, end %.2f", max_rate, r.hold_ms());
  // audio jitter (JB level moves +-15 ms around its mean, every tick): the hold stays within +-5 ms of the mean
  std::mt19937 rng(3);
  std::uniform_real_distribution<double> j(-15, 15);
  LipSyncPolicy s;
  t = drive(s, kS, 30 * kS, [&](int64_t) { return both(160 + j(rng), 90); });
  double worst = 0;
  drive(s, t, 300 * kS, [&](int64_t) { return both(160 + j(rng), 90); },
        [&](int64_t, double v) { worst = std::max(worst, std::fabs(v - 70)); });
  SCHECKF(worst <= 5.0, "jitter: worst %.2f", worst);
}

// applied only after both streams have run 2 s; an interruption restarts the warm-up, and the hold leaves
// smoothly (no jump of the picture)
void test_sync_policy_warmup() {
  LipSyncPolicy p;
  const int64_t t0 = 100 * kS;
  double first_nonzero = -1;
  bool jump = false;
  double prev = 0;
  drive(p, t0, 30 * kS, [&](int64_t t) {
    LipSyncInput in;
    in.audio = true;  // the mic from the start
    in.audio_ms = 170;
    in.video = t >= t0 + 5 * kS;  // the camera 5 s later
    in.video_ms = 80;
    return in;
  }, [&](int64_t t, double h) {
    if (h > 0 && first_nonzero < 0) first_nonzero = double(t - t0) * 1e-9;
    jump = jump || std::fabs(h - prev) > 0.5 + 1e-9;
    prev = h;
  });
  SCHECKF(first_nonzero >= 7.0 - 1e-9 && first_nonzero <= 7.06, "first hold at %.2f s", first_nonzero);
  SCHECK(!jump);
  SCHECKF(std::fabs(p.hold_ms() - 90) <= 2.0, "%.2f", p.hold_ms());
  // audio gone for 300 ms: inactive at once, the hold ramps down; back: 2 s warm-up again before it rises
  int64_t t = t0 + 30 * kS;
  double h_at_gap = p.hold_ms(), h_after_gap = 0, h_warm_end = 0;
  bool rose_in_warmup = false;
  prev = p.hold_ms();
  drive(p, t, 10 * kS, [&](int64_t now) {
    LipSyncInput in = both(170, 80);
    if (now < t + 300 * kMs) in.audio = false;
    return in;
  }, [&](int64_t now, double h) {
    if (now == t + 250 * kMs) h_after_gap = h;
    if (now > t + 300 * kMs && now < t + 2300 * kMs) {
      rose_in_warmup = rose_in_warmup || h > prev + 1e-9;
      SCHECK(!p.active());
    }
    if (now == t + 2250 * kMs) h_warm_end = h;
    prev = h;
  });
  // 6 ticks (t .. t + 250 ms) at <= 0.5 ms each
  SCHECKF(h_after_gap < h_at_gap && h_at_gap - h_after_gap <= 3.0 + 1e-9, "gap: %.2f -> %.2f", h_at_gap, h_after_gap);
  SCHECK(!rose_in_warmup && h_warm_end < h_at_gap);
  SCHECKF(p.active() && std::fabs(p.hold_ms() - 90) <= 2.0, "back: %.2f", p.hold_ms());
}

// no mic stream forwarded (or no time map on it, or a mic from another machine: the helper gives no audio
// input): no hold, no skew; video alone never moves
void test_sync_policy_no_audio() {
  LipSyncPolicy p;
  double hi = 0;
  drive(p, kS, 60 * kS, [](int64_t) {
    LipSyncInput in;
    in.video = true;
    in.video_ms = 60;
    return in;
  }, [&](int64_t, double h) { hi = std::max(hi, h); });
  SCHECK(hi == 0.0 && !p.active() && !p.have_skew());
  LipSyncPolicy q;
  drive(q, kS, 60 * kS, [](int64_t) {
    LipSyncInput in;
    in.audio = true;
    in.audio_ms = 170;
    return in;
  }, [&](int64_t, double h) { hi = std::max(hi, h); });
  SCHECK(hi == 0.0 && !q.active() && !q.have_skew());
  // non-finite inputs are no input
  LipSyncPolicy r;
  drive(r, kS, 20 * kS, [](int64_t) { return both(std::nan(""), 60); });
  SCHECK(r.hold_ms() == 0.0 && !r.active());
  // the hook: dev override wins over the policy, null (-1) gives the policy back
  LipSyncHold h;
  SCHECK(h.hold_ms() == 0.0 && !h.overridden() && std::isnan(h.skew_ms()));
  h.set_policy(42.0, -3.0, 160.0, 67.0);
  SCHECK(h.hold_ms() == 42.0);
  h.set_override_ms(0.0);
  SCHECK(h.hold_ms() == 0.0 && h.overridden());
  h.set_override_ms(120.0);
  SCHECK(h.hold_ms() == 120.0);
  h.set_override_ms(-1.0);
  SCHECK(h.hold_ms() == 42.0);
}

// ===================================================================== the delays

void test_sync_delay_math() {
  // §4.5.3 worked example: S reads 1,000,000.000 ms when T reads 5,000,000.000 ms: offset -4,000,000 ms
  const int64_t off = -4'000'000LL * kMs;
  SCHECK(std::fabs(present_delay_ms(5'000'030LL * kMs, 1'000'005LL * kMs, off, 0) - 25.0) < 1e-9);
  // audio: a sample captured at S 1,000,000 ms rendered at T 5,000,100 ms, CABLE shared 67 ms -> 167 ms
  for (int64_t o : {off, -off, 3600LL * 3 * kS, -3600LL * 3 * kS, int64_t(0)}) {
    const int64_t cap_s = 7'000'000LL * kMs;     // S clock
    const int64_t render_t = cap_s - o + 100 * kMs;  // T clock, 100 ms after capture
    // time map: ts 1000 at S 6,999,000 ms (1 s earlier); the rendered sample's ts = 1000 + 48000
    const double a = audio_present_delay_ms(1000 + 48000, render_t, 1000, cap_s - kS, 48000, o, 67.0);
    SCHECKF(std::fabs(a - 167.0) < 1e-6, "offset %lld: %.6f", (long long)o, a);
    // ts wrap: the map before 2^32, the rendered sample after it
    const uint32_t map_ts = 0xFFFFFF00u;
    const uint32_t rts = map_ts + 4800;  // wraps: 100 ms later
    const double w = audio_present_delay_ms(rts, render_t, map_ts, cap_s - 100 * kMs, 48000, o, 0.0);
    SCHECKF(std::fabs(w - 100.0) < 1e-6, "wrap: %.6f", w);
    // the map after the rendered sample (an entry newer than the render position)
    const double n = audio_present_delay_ms(1000, render_t, 1000 + 4800, cap_s + 100 * kMs, 48000, o, 0.0);
    SCHECKF(std::fabs(n - 100.0) < 1e-6, "newer map: %.6f", n);
  }
  // an offset error e moves both delays by e in opposite senses of the true one... and cancels in the difference:
  // audio computed = true + e; video present = true - e with the same hold (checked end to end below)
  SCHECK(std::fabs(video_present_delay_ms(4.5, 50) - 56.5) < 1e-9);
  SCHECK(cable_render_path_ms(CableMode::Exclusive) < cable_render_path_ms(CableMode::SharedLowLatency) &&
         cable_render_path_ms(CableMode::SharedLowLatency) < cable_render_path_ms(CableMode::SharedDefault));
}

// ===================================================================== the real presenter

// One video recv core fed with a 30 fps stream (a synchronous fake decoder), the audio side simulated: the
// mic sample captured at S time c is heard by apps at T time c - true_offset + audio_true_ms. The policy runs at
// the helper's tick exactly as Helper::lipsync_tick does (offset from lipsync_video, audio delay computed from
// time-map and render pairs with that offset). Measures the A/V skew apps see per presented picture.
struct Sim {
  uint8_t key[kKeyLen];
  Sealer tx;
  VideoRecvCore core;
  LipSyncHold hold;
  LipSyncPolicy policy;
  int64_t t = 1'000'000'000'000LL;  // receiver clock
  int64_t t0 = t;
  int64_t off0 = 0;                 // true offset (S - T) at t0
  double ppm = 0;                   // S runs this fast against T
  int64_t est_err = 0;              // offset estimate error (both streams use the same estimate)
  double audio_true_ms = 172;       // capture -> app, audio
  double audio_path_ms = 67;        // the part of it after the render callback
  int64_t enc = 4 * kMs, transit = 15 * kMs;
  std::vector<SinkAction> acts;
  int64_t next_tick = 0;
  uint32_t frame = 0;
  int64_t s0 = 0;
  std::map<uint32_t, int64_t> cap_of;  // frame -> S capture time
  // per presented picture: (T time, skew audio - video as apps see it)
  std::vector<std::pair<int64_t, double>> skews;
  std::vector<std::pair<int64_t, double>> holds;

  static const uint8_t* k() {
    static uint8_t kk[kKeyLen];
    for (size_t i = 0; i < kKeyLen; ++i) kk[i] = uint8_t(i * 5 + 3);
    return kk;
  }
  int64_t true_off(int64_t at) const { return off0 + int64_t(double(at - t0) * ppm * 1e-6); }
  Sim(int64_t offset, double drift_ppm, int64_t err) : core(91, k(), 30, 1280, 720) {
    std::memcpy(key, k(), kKeyLen);
    tx.init(key, 91);
    off0 = offset;
    ppm = drift_ppm;
    est_err = err;
    core.lipsync = &hold;
    core.offset_source = [this](int64_t now, int64_t* off, int64_t* age) {
      *off = true_off(now) + est_err;  // a pong per second would follow it within the drift of 1 s: negligible
      *age = 0;
      return true;
    };
    core.start(t, &acts);
    s0 = t + true_off(t) + 500 * kMs;
    core.on_time_map(0, uint64_t(s0));
    next_tick = t;
  }
  void deliver_frame() {
    const uint32_t f = frame++;
    const int64_t cap_s = s0 + int64_t(f) * 33'333'333;
    cap_of[f] = cap_s;
    const uint8_t vflags = f == 0 ? kVIdr : 0;
    const size_t bytes = f == 0 ? 20000 : 2500;
    const size_t n = (bytes + kMaxFragment - 1) / kMaxFragment;
    for (size_t i = 0; i < n; ++i) {
      VideoHeader vh;
      vh.frame = f;
      vh.frag = uint16_t(i);
      vh.nfrag = uint16_t(n);
      vh.vflags = vflags;
      vh.enc_delay = uint16_t(enc / 100'000);
      const size_t len = std::min(kMaxFragment, bytes - std::min(bytes, i * kMaxFragment));
      std::vector<uint8_t> plain(kVideoHeaderLen + len, uint8_t(f));
      write_video_header(vh, plain.data());
      Header h;
      h.ptype = kMedia;
      h.codec = kCodecH264;
      h.stream = 91;
      h.ts = f * 3000;
      std::vector<uint8_t> out(kMaxPacket);
      out.resize(tx.seal(h, plain.data(), plain.size(), out.data()));
      std::vector<std::vector<uint8_t>> fb;
      core.ingest(out.data(), out.size(), t, false, &fb);
    }
    if (f % 300 == 0 && f) core.on_time_map(f * 3000, uint64_t(cap_s));  // a time map every 10 s (exact)
  }
  void pump(bool tick) {
    for (int round = 0; round < 8; ++round) {
      core.pump(t, tick && round == 0, &acts);
      if (acts.empty()) break;
      std::vector<SinkAction> a;
      a.swap(acts);
      bool any = false;
      for (auto& x : a) {
        if (x.kind == SinkAction::kDecode) {
          core.on_decoded(x.frame, true, 1.0, t);
          any = true;
        } else if (x.kind == SinkAction::kPresent) {
          // apps see the picture 2 ms later (source delivery); its capture at T: cap_s - true_off
          const double video_ms = double(t + 2 * kMs - (cap_of[x.frame] - true_off(t))) * 1e-6;
          skews.emplace_back(t, audio_true_ms - video_ms);
        }
      }
      if (!any) break;
    }
  }
  // the helper tick: audio input from a render pair (T now) and the stream's time map (S clock, 48 kHz)
  void tick() {
    LipSyncInput in;
    int64_t off = 0;
    in.video = core.lipsync_video(t, &in.video_ms, &off);
    if (in.video) {
      const int64_t render_at = t - 3 * kMs;  // the last render callback
      // the sample being rendered then was captured (T clock) audio_true - path earlier
      const int64_t cap_t = render_at - int64_t((audio_true_ms - audio_path_ms) * 1e6);
      const int64_t cap_s = cap_t + true_off(cap_t);
      const int64_t amap_ns = s0 - 2 * kS;  // the mic's time map: ts 0 at S s0 - 2 s
      const uint32_t rts = uint32_t(int64_t(double(cap_s - amap_ns) * 48000.0 / 1e9));
      in.audio = true;
      in.audio_ms = audio_present_delay_ms(rts, render_at, 0, amap_ns, 48000, off, audio_path_ms);
    }
    const double h = policy.update(t, in);
    hold.set_policy(h, policy.have_skew() ? policy.skew_ms() : LipSyncHold::kNone, in.audio_ms, audio_path_ms);
    holds.emplace_back(t, h);
  }
  // runs `ns` of receiver time: frames arrive enc + transit after capture, the tick every 50 ms
  void run(int64_t ns) {
    const int64_t end = t + ns;
    while (t < end) {
      // next frame arrival: its S capture mapped to T + enc + transit
      const int64_t cap_s = s0 + int64_t(frame) * 33'333'333;
      int64_t arrive = cap_s - true_off(t) + enc + transit;
      const int64_t next = std::min({arrive, next_tick, t + kMs});
      t = std::max(t, next);
      if (t >= arrive) deliver_frame();
      const bool tk = t >= next_tick;
      pump(tk && (t / kMs) % 33 == 0);
      if (tk) {
        tick();
        next_tick += kTick;
      }
    }
  }
  // worst |skew| of pictures presented in [from, to)
  double worst(int64_t from, int64_t to, size_t* n = nullptr) const {
    double w = 0;
    size_t c = 0;
    for (auto& s : skews)
      if (s.first >= from && s.first < to) {
        w = std::max(w, std::fabs(s.second));
        c++;
      }
    if (n) *n = c;
    return w;
  }
  double max_slew() const {  // ms per second
    double m = 0;
    for (size_t i = 1; i < holds.size(); ++i)
      m = std::max(m, std::fabs(holds[i].second - holds[i - 1].second) / (double(holds[i].first - holds[i - 1].first) * 1e-9));
    return m;
  }
};

// both offset signs (+-3 h: the Mac's uptime larger, then smaller than the PC's), offset estimate errors of
// +-20 ms applied to both streams, +-25 ppm drift: after settling, what apps see is in sync within +-2 ms
void test_sync_presenter_offsets() {
  const int64_t h3 = 3600LL * 3 * kS;
  struct C {
    int64_t off;
    double ppm;
    int64_t err;
  };
  const C cases[] = {{+h3, 0, 0},       {-h3, 0, 0},       {+h3, 0, +20 * kMs}, {+h3, 0, -20 * kMs},
                     {-h3, 0, +20 * kMs}, {-h3, 0, -20 * kMs}, {+h3, +25, 0},       {-h3, -25, +20 * kMs},
                     {123'456'789'000LL, +25, -20 * kMs}};
  for (const C& c : cases) {
    Sim s(c.off, c.ppm, c.err);
    s.run(5 * 60 * kS);  // 5 min (7.5 ms of drift at 25 ppm)
    size_t n = 0;
    const double w = s.worst(s.t0 + 30 * kS, s.t, &n);
    SCHECKF(w <= 2.0 && n > 8000, "offset %+.0f h, %+.0f ppm, err %+lld ms: worst skew %.2f ms over %zu pictures",
            double(c.off) / 3.6e12, c.ppm, (long long)(c.err / kMs), w, n);
    // the hold is the difference of the computed delays (shared CABLE: 172 vs enc 4 + D 50 + 2 = 56 -> 116),
    // moved by the offset error, which the presenter's mapping takes back (the skew above)
    const double want = 172 - 56 + double(c.err) * 1e-6;
    SCHECKF(std::fabs(s.hold.hold_ms() - want) <= 2.0, "hold %.2f want %.2f", s.hold.hold_ms(), want);
    SCHECKF(s.max_slew() <= 10.0 + 1e-6, "slew %.3f ms/s", s.max_slew());
    // nothing before both ran 2 s (the video's first live picture ~0.1 s in)
    for (auto& h : s.holds)
      if (h.first < s.t0 + 2 * kS) SCHECK(h.second == 0.0);
  }
}

// the mic path changes (shared -> low-latency vmic, and back): the hold follows at <= 10 ms/s and settles; the
// audio timing itself is an input only (never changed by the policy)
void test_sync_presenter_path_change() {
  Sim s(-3600LL * 3 * kS, +10, +7 * kMs);
  s.run(40 * kS);
  const double shared = s.hold.hold_ms();
  SCHECKF(std::fabs(shared - (172 - 56 + 7)) <= 2.0, "shared: %.2f", shared);  // + the 7 ms offset error
  // low-latency (exclusive) vmic: C4's 120 ms, the path part 38 ms
  const int64_t t_ll = s.t;
  s.audio_true_ms = 120;
  s.audio_path_ms = 38;
  s.run(30 * kS);
  SCHECKF(std::fabs(s.hold.hold_ms() - (120 - 56 + 7)) <= 2.0, "low-latency: %.2f", s.hold.hold_ms());
  SCHECKF(s.worst(t_ll + 15 * kS, s.t) <= 2.0, "low-latency skew %.2f", s.worst(t_ll + 15 * kS, s.t));
  SCHECKF(s.max_slew() <= 10.0 + 1e-6, "slew %.3f", s.max_slew());
  // back to shared
  const int64_t t_sh = s.t;
  s.audio_true_ms = 172;
  s.audio_path_ms = 67;
  s.run(30 * kS);
  SCHECKF(s.worst(t_sh + 15 * kS, s.t) <= 2.0, "shared again skew %.2f", s.worst(t_sh + 15 * kS, s.t));
  // bounds through the real presenter: audio 300 ms -> hold 150, audio still later by 300 - 56 - 150 = 94
  Sim b(3600LL * 3 * kS, 0, 0);
  b.audio_true_ms = 300;
  b.run(40 * kS);
  SCHECKF(std::fabs(b.hold.hold_ms() - 150) < 1e-9, "hold %.2f", b.hold.hold_ms());
  size_t n = 0;
  double mean = 0;
  for (auto& x : b.skews)
    if (x.first > b.t0 + 30 * kS) {
      mean += x.second;
      n++;
    }
  mean /= double(std::max<size_t>(1, n));
  SCHECKF(std::fabs(mean - 94) <= 2.0, "skew at the bound %.2f", mean);
  // audio faster than video (40 ms vs 56): no hold, the picture as early as D allows, audio leads by 16
  Sim l(-3600LL * 3 * kS, 0, 0);
  l.audio_true_ms = 40;
  l.audio_path_ms = 30;
  l.run(30 * kS);
  SCHECK(l.hold.hold_ms() == 0.0);
  mean = 0;
  n = 0;
  for (auto& x : l.skews)
    if (x.first > l.t0 + 10 * kS) {
      mean += x.second;
      n++;
    }
  mean /= double(std::max<size_t>(1, n));
  SCHECKF(std::fabs(mean + 16) <= 2.0, "audio leads: %.2f", mean);
  SCHECKF(std::fabs(l.policy.skew_ms() + 16) <= 2.0, "policy skew %.2f", l.policy.skew_ms());
}

// ===================================================================== end to end (in-process helpers)

class SProc {
 public:
  SProc(const std::string& null_opts) {
    backend_ = make_null_backend(null_opts);
    video_ = make_null_video_backend("");
    HelperOptions opt;
    opt.dev = true;
    opt.stats_period_ms = 250;
    opt.video = video_.get();
    HelperIo io;
    io.read_line = [this](std::string* l) {
      std::unique_lock<std::mutex> lock(imu_);
      icv_.wait(lock, [this] { return !in_.empty() || closed_; });
      if (in_.empty()) return false;
      *l = in_.front();
      in_.pop_front();
      return true;
    };
    io.write_line = [this](const std::string& l) {
      Json j = Json::parse(l, nullptr, false);
      std::lock_guard<std::mutex> lock(omu_);
      log_.push_back(j);
      ocv_.notify_all();
    };
    thread_ = std::thread([this, opt, io] { run_control(*backend_, opt, io); });
  }
  ~SProc() {
    send(Json{{"cmd", "quit"}});
    {
      std::lock_guard<std::mutex> lock(imu_);
      closed_ = true;
    }
    icv_.notify_all();
    thread_.join();
  }
  void send(const Json& j) {
    {
      std::lock_guard<std::mutex> lock(imu_);
      in_.push_back(j.dump());
    }
    icv_.notify_all();
  }
  size_t cursor() {
    std::lock_guard<std::mutex> lock(omu_);
    return log_.size();
  }
  bool wait(size_t* from, const std::function<bool(const Json&)>& pred, int timeout_ms, Json* out = nullptr) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::unique_lock<std::mutex> lock(omu_);
    for (;;) {
      for (size_t i = *from; i < log_.size(); ++i)
        if (pred(log_[i])) {
          *from = i + 1;
          if (out) *out = log_[i];
          return true;
        }
      *from = log_.size();
      if (ocv_.wait_until(lock, deadline) == std::cv_status::timeout) return false;
    }
  }
  Json call(Json cmd, int timeout_ms = 3000) {
    static std::atomic<int> req{5000};
    const int r = req++;
    cmd["req"] = r;
    size_t c = cursor();
    send(cmd);
    Json out;
    if (!wait(&c, [r](const Json& j) { return j.value("req", -1) == r && (j["ev"] == "ok" || j["ev"] == "error"); },
              timeout_ms, &out))
      return Json{{"ev", "timeout"}};
    return out;
  }
  // the next stats of `stream`; `at_ns`: the helper's now_ns of that stats event
  Json next_stats(uint32_t stream, int64_t* at_ns = nullptr, int timeout_ms = 2000) {
    size_t c = cursor();
    Json st;
    const std::string k = std::to_string(stream);
    if (!wait(&c, [&](const Json& j) { return j.value("ev", "") == "stats" && j["streams"].contains(k); }, timeout_ms, &st))
      return Json();
    if (at_ns) *at_ns = st.value("now_ns", int64_t(0));
    return st["streams"][k];
  }

 private:
  std::unique_ptr<Backend> backend_;
  std::unique_ptr<VideoBackend> video_;
  std::thread thread_;
  std::mutex imu_, omu_;
  std::condition_variable icv_, ocv_;
  std::deque<std::string> in_;
  bool closed_ = false;
  std::vector<Json> log_;
};

std::string skey(uint8_t fill) { return b64_encode(std::vector<uint8_t>(32, fill)); }

std::function<bool(const Json&)> ev_is(const std::string& ev, const std::string& field = "", const Json& v = nullptr) {
  return [=](const Json& j) {
    if (!j.is_object() || j.value("ev", "") != ev) return false;
    if (field.empty()) return true;
    auto it = j.find(field);
    return it != j.end() && *it == v;
  };
}

bool open_flow(SProc& a, SProc& b, const std::string& flow) {
  size_t ca = a.cursor(), cb = b.cursor();
  Json fa, fb;
  a.send(Json{{"cmd", "flow_open"}, {"req", 1}, {"flow", flow}, {"peer", "B"}, {"role", "offer"}, {"port", 0},
              {"host", "127.0.0.1"}, {"remote", nullptr}, {"key", skey(7)}, {"rr_ms", 10}});
  if (!a.wait(&ca, ev_is("flow_bound", "flow", flow), 3000, &fa)) return false;
  b.send(Json{{"cmd", "flow_open"}, {"req", 1}, {"flow", flow}, {"peer", "A"}, {"role", "answer"}, {"port", 0},
              {"remote", {{"host", "127.0.0.1"}, {"port", fa["port"]}}}, {"key", skey(7)}, {"rr_ms", 10}});
  if (!b.wait(&cb, ev_is("flow_bound", "flow", flow), 3000, &fb)) return false;
  a.send(Json{{"cmd", "flow_remote"}, {"req", 2}, {"flow", flow}, {"host", "127.0.0.1"}, {"port", fb["port"]}});
  auto up = [&](const Json& j) { return ev_is("flow_state", "state", "up")(j) && j.value("flow", "") == flow; };
  return a.wait(&ca, up, 3000) && b.wait(&cb, up, 3000);
}

Json mic_send(const std::string& flow, uint32_t id, bool sr) {
  return Json{{"cmd", "send"}, {"stream", id}, {"flow", flow}, {"kind", "mic"}, {"source", {{"device", nullptr}}},
              {"codec", CodecConfig::mic_default().to_json()}, {"key", skey(uint8_t(id))}, {"sr", sr}};
}
Json mic_recv(const std::string& flow, uint32_t id, bool vmic) {
  Json sink = vmic ? Json{{"virtual_mic", true}} : Json{{"device", nullptr}};
  return Json{{"cmd", "recv"}, {"stream", id}, {"flow", flow}, {"kind", "mic"},
              {"codec", CodecConfig::mic_default().to_json()}, {"key", skey(uint8_t(id))}, {"sink", sink}};
}
Json vcodec() {
  return Json{{"name", "h264"}, {"profile", "constrained_high"}, {"w", 1280}, {"h", 720}, {"fps", 30},
              {"bitrate", 2500000}, {"mtu", 1200}, {"ltr", true}};
}
Json video_send(const std::string& flow, uint32_t id) {
  return Json{{"cmd", "send"}, {"stream", id}, {"flow", flow}, {"kind", "video"}, {"source", {{"camera", nullptr}}},
              {"codec", vcodec()}, {"key", skey(3)}};
}
Json video_recv(const std::string& flow, uint32_t id) {
  return Json{{"cmd", "recv"}, {"stream", id}, {"flow", flow}, {"kind", "video"}, {"codec", vcodec()},
              {"key", skey(3)}, {"sink", {{"virtual_camera", true}}}};
}

double num(const Json& j, const char* k, double dflt = -1e9) {
  auto it = j.find(k);
  return it != j.end() && it->is_number() ? it->get<double>() : dflt;
}

// Mac -> PC with the null backends: mic `sr:true` into the virtual mic + camera on the same flow. The audio path
// latency is set by the dev key (the null render has none), so the audio is the slower path; the hold rises at
// <= 10 ms/s to audio - video and the skew left goes to ~0; the audio delay is the same before and after the hold
// (audio is never delayed); the dev override wins and `null` gives the policy back.
void test_sync_e2e_hold() {
  SProc a("mic=sine:440"), b("");
  SCHECK(open_flow(a, b, "0000c10a"));
  SCHECK(b.call(Json{{"cmd", "dev"}, {"sync_audio_path_ms", 60}})["ev"] == "ok");
  SCHECK(a.call(mic_send("0000c10a", 11, true))["ev"] == "ok");
  SCHECK(b.call(mic_recv("0000c10a", 11, true))["ev"] == "ok");
  SCHECK(a.call(video_send("0000c10a", 507))["ev"] == "ok");
  SCHECK(b.call(video_recv("0000c10a", 507))["ev"] == "ok");
  // stats every 250 ms for ~14 s
  std::vector<Json> st;
  std::vector<int64_t> at;
  for (int i = 0; i < 56; ++i) {
    int64_t t = 0;
    Json s = b.next_stats(507, &t);
    if (s.is_object()) {
      st.push_back(s);
      at.push_back(t);
    }
  }
  SCHECKF(st.size() >= 40, "%zu stats", st.size());
  if (st.size() < 40) return;
  const Json& last = st.back();
  const double hold = num(last, "sync_ms"), audio = num(last, "sync_audio_ms"), skew = num(last, "sync_skew_ms");
  const double video = num(last, "video_present_delay_ms");  // includes the hold
  std::printf("    hold %.1f ms, audio %.1f ms, video %.1f ms (with hold), skew %.1f ms, path %.1f ms, D %.1f ms\n", hold,
              audio, video, skew, num(last, "sync_path_ms"), num(last, "target_ms"));
  SCHECKF(num(last, "sync_path_ms") == 60.0, "%s", last.dump().c_str());
  SCHECKF(hold > 10 && hold <= 150, "hold %.1f", hold);
  SCHECKF(std::fabs(skew) <= 6.0, "skew left %.1f (audio %.1f, video %.1f)", skew, audio, video);
  SCHECKF(std::fabs(audio - video) <= 8.0, "audio %.1f vs video with hold %.1f", audio, video);
  // the hold is 0 for the first 2 s of both streams, then never moves faster than 10 ms/s (stats rounding 0.1)
  double max_rate = 0, audio_early = -1;
  for (size_t i = 1; i < st.size(); ++i) {
    const double dt = double(at[i] - at[i - 1]) * 1e-9 + 0.05;  // + one tick: stats and tick are not in phase
    max_rate = std::max(max_rate, (std::fabs(num(st[i], "sync_ms") - num(st[i - 1], "sync_ms")) - 0.1) / dt);
  }
  SCHECKF(max_rate <= 10.0, "max hold rate %.2f ms/s", max_rate);
  SCHECK(num(st[0], "sync_ms") == 0.0 && num(st[3], "sync_ms") == 0.0);
  for (auto& s : st)
    if (num(s, "sync_ms") == 0.0 && num(s, "sync_audio_ms", -1) > 0) audio_early = num(s, "sync_audio_ms");
  // audio is never delayed for video: its present delay with the hold on equals the one before the hold (JB noise)
  SCHECKF(audio_early > 0 && std::fabs(audio - audio_early) <= 12.0, "audio before the hold %.1f, after %.1f",
          audio_early, audio);
  Json m = b.next_stats(11);
  SCHECKF(m.value("dec_err", 1) == 0 && m.value("lost", 0) == 0, "%s", m.dump().substr(0, 200).c_str());
  // dev override: forced, then back to the policy (which kept its value)
  SCHECK(b.call(Json{{"cmd", "dev"}, {"sync_offset_ms", 0}})["ev"] == "ok");
  Json o = b.next_stats(507);
  SCHECKF(num(o, "sync_ms") == 0.0 && o.value("sync_override", false), "%s", o.dump().c_str());
  SCHECK(b.call(Json{{"cmd", "dev"}, {"sync_offset_ms", 151}})["ev"] == "error");
  SCHECK(b.call(Json{{"cmd", "dev"}, {"sync_offset_ms", nullptr}})["ev"] == "ok");
  Json p = b.next_stats(507);
  SCHECKF(std::fabs(num(p, "sync_ms") - hold) <= 6.0 && !p.value("sync_override", true), "%s", p.dump().c_str());
  SCHECK(b.call(Json{{"cmd", "dev"}, {"sync_audio_path_ms", nullptr}})["ev"] == "ok");
  Json q = b.next_stats(507);
  SCHECKF(num(q, "sync_path_ms") == 0.0, "the null render reports no path: %s", q.dump().c_str());
}

// No hold when: no mic is forwarded; the mic comes from another machine (another flow) than the camera; the mic
// from the camera's machine is rendered to a speaker; or it carries no time map (`sr` off).
void test_sync_e2e_no_hold() {
  SProc a("mic=sine:440"), b(""), c("mic=sine:440");
  SCHECK(open_flow(a, b, "0000c10b"));
  SCHECK(open_flow(c, b, "0000c10c"));
  SCHECK(b.call(Json{{"cmd", "dev"}, {"sync_audio_path_ms", 120}})["ev"] == "ok");  // audio would be the slower path
  SCHECK(a.call(video_send("0000c10b", 508))["ev"] == "ok");
  SCHECK(b.call(video_recv("0000c10b", 508))["ev"] == "ok");
  auto phase = [&](const char* what, int n) {
    double hi = 0;
    bool audio_seen = false;
    for (int i = 0; i < n; ++i) {
      Json s = b.next_stats(508);
      hi = std::max(hi, num(s, "sync_ms", 0));
      audio_seen = audio_seen || num(s, "sync_audio_ms", -1) != -1;
    }
    SCHECKF(hi == 0.0 && !audio_seen, "%s: hold %.1f, audio paired %d", what, hi, audio_seen);
  };
  phase("no mic", 12);
  // the mic of another machine (C) into the virtual mic
  SCHECK(c.call(mic_send("0000c10c", 21, true))["ev"] == "ok");
  SCHECK(b.call(mic_recv("0000c10c", 21, true))["ev"] == "ok");
  phase("mic from another machine", 14);
  // the camera machine's mic, with a time map, to a speaker
  SCHECK(a.call(mic_send("0000c10b", 12, true))["ev"] == "ok");
  SCHECK(b.call(mic_recv("0000c10b", 12, false))["ev"] == "ok");
  phase("mic to a speaker", 14);
  // the camera machine's mic into the virtual mic, without `sr`
  SCHECK(a.call(mic_send("0000c10b", 13, false))["ev"] == "ok");
  SCHECK(b.call(mic_recv("0000c10b", 13, true))["ev"] == "ok");
  phase("mic without a time map", 14);
  Json m = b.next_stats(13);
  SCHECKF(m.value("dec_err", 1) == 0 && num(m, "jb_ms", 0) > 0, "%s", m.dump().substr(0, 200).c_str());
}

const struct {
  const char* name;
  void (*fn)();
} kCases[] = {
    {"sync_policy_formula", test_sync_policy_formula},
    {"sync_policy_bounds", test_sync_policy_bounds},
    {"sync_policy_smoothing", test_sync_policy_smoothing},
    {"sync_policy_warmup", test_sync_policy_warmup},
    {"sync_policy_no_audio", test_sync_policy_no_audio},
    {"sync_delay_math", test_sync_delay_math},
    {"sync_presenter_offsets", test_sync_presenter_offsets},
    {"sync_presenter_path_change", test_sync_presenter_path_change},
    {"sync_e2e_hold", test_sync_e2e_hold},
    {"sync_e2e_no_hold", test_sync_e2e_no_hold},
};

}  // namespace

struct Case {
  const char* name;
  void (*fn)();
};

const Case* sync_test_cases(size_t* n) {
  static std::vector<Case> v;
  if (v.empty())
    for (auto& c : kCases) v.push_back(Case{c.name, c.fn});
  *n = v.size();
  return v.data();
}
