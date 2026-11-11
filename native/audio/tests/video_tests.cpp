// SPDX-License-Identifier: GPL-3.0-only
// Engine video tests (camera PHASE2 §10.2), linked into engine_tests (same CHECK counters, cases appended by
// video_test_cases()). Pure parts run on synthetic time; the end-to-end cases run two in-process helpers with
// the null device and null video backends over 127.0.0.1.
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
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "codec.h"
#include "engine.h"
#include "flow.h"
#include "helper.h"
#include "json.h"
#include "null_video.h"
#include "pacer.h"
#include "packet.h"
#include "sync.h"
#include "util.h"
#include "video_backend.h"
#include "video_packet.h"
#include "video_recv.h"
#include "video_send.h"

using namespace beamer::audio;

extern int g_checks, g_failures;

#define VCHECK(c)                                                             \
  do {                                                                        \
    g_checks++;                                                               \
    if (!(c)) {                                                               \
      g_failures++;                                                           \
      std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
    }                                                                         \
  } while (0)
#define VCHECKF(c, ...)                                                       \
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

// ===================================================================== packets

void test_video_packet_formats() {
  VideoHeader v;
  v.frame = 0xA1B2C3D4;
  v.frag = 224;
  v.nfrag = 225;
  v.vflags = kVIdr | kVLtr | kVBehindIdr;
  v.ltr = 1;
  v.enc_delay = enc_delay_units(5'230'000);
  uint8_t b[kVideoHeaderLen];
  write_video_header(v, b);
  VideoHeader w;
  VCHECK(read_video_header(b, sizeof b, &w));
  VCHECK(w.frame == v.frame && w.frag == 224 && w.nfrag == 225 && w.vflags == v.vflags && w.ltr == 1);
  VCHECK(w.enc_delay == 52);
  VCHECK(w.vflags & kVBehindIdr);
  VCHECK(enc_delay_units(-5) == 0 && enc_delay_units(int64_t(7) * 1'000'000'000) == 65535);  // saturates
  VCHECK(!read_video_header(b, 11, &w));
  v.frag = 225;
  write_video_header(v, b);
  VCHECK(!read_video_header(b, sizeof b, &w));  // frag >= nfrag
  v.frag = 0;
  v.nfrag = 1025;
  write_video_header(v, b);
  VCHECK(!read_video_header(b, sizeof b, &w));  // nfrag > 1024
  v.nfrag = 0;
  write_video_header(v, b);
  VCHECK(!read_video_header(b, sizeof b, &w));
  v.nfrag = 1;
  v.vflags = 0x10;
  write_video_header(v, b);
  VCHECK(!read_video_header(b, sizeof b, &w));  // reserved vflags bits
  VCHECK(kMaxFragment == 1148 && kHeaderLen + kVideoHeaderLen + kMaxFragment + kTagLen == 1200);
  // NACK
  std::vector<uint64_t> seqs{100, 101, 103, 116, 117, 200};
  std::vector<NackEntry> e;
  VCHECK(pack_nack(seqs, &e) == seqs.size());
  VCHECK(e.size() == 3 && e[0].seq_base == 100 && e[1].seq_base == 117 && e[2].seq_base == 200);
  uint8_t nb[1 + 32 * 10];
  const size_t nn = write_nack(e, nb);
  std::vector<NackEntry> e2;
  VCHECK(read_nack(nb, nn, &e2) && e2.size() == 3);
  std::vector<uint64_t> back;
  unpack_nack(e2, &back);
  VCHECK(back == seqs);
  std::vector<uint64_t> many;
  for (int i = 0; i < 40; ++i) many.push_back(uint64_t(1000 + i * 100));
  VCHECK(pack_nack(many, &e) == 32 && e.size() == 32);  // <= 32 entries per packet
  nb[0] = 0;
  VCHECK(!read_nack(nb, nn, &e2));
  nb[0] = 33;
  VCHECK(!read_nack(nb, sizeof nb, &e2));
  // PLI
  Pli p{1234, kPliSize};
  uint8_t pb[kPliLen];
  write_pli(p, pb);
  Pli q;
  VCHECK(read_pli(pb, sizeof pb, &q) && q.last_clean == 1234 && q.reason == kPliSize);
  pb[4] = 5;
  VCHECK(!read_pli(pb, sizeof pb, &q));
  // time map
  std::vector<TimeMapEntry> tm{{7, 90000, 123456789012ull}, {9, 4000000000u, 1ull}};
  uint8_t tb[1 + 32 * 16];
  const size_t tn = write_time_map(tm, tb);
  std::vector<TimeMapEntry> tm2;
  VCHECK(tn == 33 && read_time_map(tb, tn, &tm2) && tm2.size() == 2 && tm2[1].ts == 4000000000u && tm2[0].sender_ns == 123456789012ull);
  VCHECK(!read_time_map(tb, tn - 1, &tm2));
  // video RR, 28 B
  VideoRR rr;
  rr.newest_ts = 99;
  rr.margin = 456;
  rr.hi_seq_lo = 0xFFFFFFF0;
  rr.lost = 3;
  rr.late = 7;
  rr.clean_since = 40;
  rr.acked = 77;
  rr.recv_kbps = 2600;
  rr.flags = 1;
  uint8_t rb[kVideoRRLen];
  write_video_rr(rr, rb);
  VideoRR r2;
  VCHECK(kVideoRRLen == 28 && read_video_rr(rb, 28, &r2));
  VCHECK(r2.newest_ts == 99 && r2.margin == 456 && r2.hi_seq_lo == 0xFFFFFFF0 && r2.lost == 3 && r2.late == 7 &&
         r2.clean_since == 40 && r2.acked == 77 && r2.recv_kbps == 2600 && r2.flags == 1);
  VCHECK(!read_video_rr(rb, 27, &r2));
  // ts from capture time (90 kHz since the first frame)
  VCHECK(video_ts(1'000'000'000, 0) == 90000 && video_ts(5, 10) == 0);
}

void test_video_replay_window_1024() {
  VideoReplayWindow w;
  for (uint64_t s = 1; s <= 100; ++s) {
    VCHECK(w.fresh(s));
    w.mark(s);
  }
  for (uint64_t s = 102; s <= 1100; ++s) w.mark(s);  // 101 missing (reorder)
  VCHECK(w.fresh(101));                            // 999 behind: inside 1024
  VCHECK(!w.fresh(1100) && !w.fresh(500));         // replays
  w.mark(1124);
  VCHECK(w.fresh(101));   // d = 1023
  w.mark(1125);
  VCHECK(!w.fresh(101));  // d = 1024: too old
  VCHECK(w.fresh(1123) && !w.fresh(1124));
  w.mark(1'000'000);  // a big jump clears everything
  VCHECK(!w.fresh(1124) && w.fresh(999'999) && !w.fresh(0));
  // the audio window keeps its 128
  ReplayWindow a;
  a.mark(1);
  a.mark(129);
  VCHECK(!a.fresh(1) && a.fresh(2) && !a.fresh(1));
  a.mark(130);
  VCHECK(!a.fresh(2));
}

// ===================================================================== receiver rig (synthetic time)

struct Rig {
  uint8_t key[kKeyLen];
  uint32_t id = 77;
  Sealer tx;
  VideoRecvCore core;
  int64_t t = 1'000'000'000'000LL;  // receiver clock
  bool have_offset = true;
  int64_t offset = 0;               // sender clock minus receiver clock
  std::vector<SinkAction> acts;
  std::vector<std::vector<uint8_t>> fb;
  std::map<uint32_t, std::vector<uint8_t>> decoded;
  std::map<uint32_t, bool> keep;
  std::vector<std::pair<uint32_t, int64_t>> presented;
  std::vector<std::string> placeholders;
  std::set<uint32_t> fail_decode;
  bool auto_decode = true;

  static const uint8_t* k() {
    static uint8_t kk[kKeyLen];
    for (size_t i = 0; i < kKeyLen; ++i) kk[i] = uint8_t(i * 7 + 1);
    return kk;
  }
  Rig() : core(77, k(), 30, 1280, 720) {
    std::memcpy(key, k(), kKeyLen);
    tx.init(key, id & ~kDirBit);
    core.offset_source = [this](int64_t, int64_t* off, int64_t* age) {
      if (!have_offset) return false;
      *off = offset;
      *age = 0;
      return true;
    };
    core.start(t, &acts);
    run();
  }
  // sender clock of a capture at receiver time r (no transit): r + offset
  std::vector<std::vector<uint8_t>> make_au(uint32_t frame, size_t bytes, uint8_t vflags, uint32_t ts, uint16_t enc) {
    std::vector<std::vector<uint8_t>> pk;
    const size_t n = std::max<size_t>(1, (bytes + kMaxFragment - 1) / kMaxFragment);
    for (size_t i = 0; i < n; ++i) {
      VideoHeader vh;
      vh.frame = frame;
      vh.frag = uint16_t(i);
      vh.nfrag = uint16_t(n);
      vh.vflags = vflags;
      vh.ltr = (vflags & kVLtr) ? 1 : 0;
      vh.enc_delay = enc;
      const size_t len = std::min(kMaxFragment, bytes - std::min(bytes, i * kMaxFragment));
      std::vector<uint8_t> plain(kVideoHeaderLen + len);
      write_video_header(vh, plain.data());
      for (size_t j = 0; j < len; ++j) plain[kVideoHeaderLen + j] = uint8_t((frame * 31 + i * kMaxFragment + j) & 0xFF);
      Header h;
      h.ptype = kMedia;
      h.codec = kCodecH264;
      h.stream = id;
      h.ts = ts;
      std::vector<uint8_t> out(kMaxPacket);
      out.resize(tx.seal(h, plain.data(), plain.size(), out.data()));
      pk.push_back(std::move(out));
    }
    return pk;
  }
  void deliver(const std::vector<uint8_t>& p) { core.ingest(p.data(), p.size(), t, false, &fb); }
  void map(uint32_t ts, int64_t sender_ns) { core.on_time_map(ts, uint64_t(sender_ns)); }
  // executes the actions; a synchronous fake decoder answers each decode
  void run(bool tick = false) {
    for (int round = 0; round < 8; ++round) {
      core.pump(t, tick && round == 0, &acts);
      if (acts.empty()) break;
      std::vector<SinkAction> a;
      a.swap(acts);
      bool any_decode = false;
      for (auto& x : a) {
        if (x.kind == SinkAction::kDecode) {
          decoded[x.frame] = *x.au;
          keep[x.frame] = x.keep;
          if (auto_decode) core.on_decoded(x.frame, !fail_decode.count(x.frame), 1.0, t);
          any_decode = true;
        } else if (x.kind == SinkAction::kPresent) {
          presented.emplace_back(x.frame, t);
        } else if (x.kind == SinkAction::kPlaceholder) {
          placeholders.push_back(x.state);
        }
      }
      if (!any_decode) break;
    }
  }
  void advance(int64_t ns, bool ticks = true) {
    const int64_t end = t + ns;
    while (t < end) {
      t = std::min(end, t + kMs);
      run(ticks && (t / kMs) % 33 == 0);
    }
  }
  void net(double rtt_ms = 2) {
    fb.clear();
    core.net_tick(t, rtt_ms, &fb);
  }
  int count_fb(uint8_t ptype) {
    int n = 0;
    for (auto& p : fb) {
      Header h;
      if (read_header(p.data(), p.size(), &h) && h.ptype == ptype) n++;
    }
    return n;
  }
  bool was_presented(uint32_t f) const {
    return std::any_of(presented.begin(), presented.end(), [f](const std::pair<uint32_t, int64_t>& x) { return x.first == f; });
  }
};

void test_video_reassembly_any_order() {
  Rig r;
  std::mt19937 rng(5);
  const size_t sizes[] = {1, 100, 1148, 1149, 5000, 120000, 300000};
  uint32_t f = 0;
  for (size_t sz : sizes) {
    auto pk = r.make_au(f, sz, kVIdr, f * 3000, 50);
    std::shuffle(pk.begin(), pk.end(), rng);
    for (auto& p : pk) r.deliver(p);
    r.run();
    VCHECKF(r.decoded.count(f) && r.decoded[f].size() == sz, "frame %u size %zu", f, sz);
    bool same = r.decoded.count(f) > 0;
    for (size_t j = 0; same && j < sz; ++j)
      same = r.decoded[f][j] == uint8_t((f * 31 + (j / kMaxFragment) * kMaxFragment + j % kMaxFragment) & 0xFF);
    VCHECKF(same, "frame %u bytes", f);
    f++;
  }
  // a duplicate fragment changes nothing; an AU over 1024 fragments is refused by the header check
  VCHECK(r.decoded.size() == 7);
}

// ===================================================================== sender recovery (§4.3)

void test_video_sender_recovery() {
  using A = SenderRecovery;
  // start + start PLIs repeated during a paced IDR: exactly one IDR
  {
    A r;
    int64_t t = 0;
    int starts = 0, idrs = 0;
    Pli st{kNoFrame, kPliStart};
    for (; t < 3000 * kMs; t += 100 * kMs) {
      if (t == 30 * kMs) r.on_idr_numbered(0, t);
      auto a = r.on_pli(st, t);
      starts += a == A::kStart;
      idrs += a == A::kIdr;
      if (t == 0) {
        r.on_idr_numbered(0, 27 * kMs);
        r.on_idr_first_packet(28 * kMs, 100 * kMs);
      }
      if (t >= 200 * kMs) break;  // the receiver saw the IDR's first packet at ~30 ms; its repeats stop
    }
    VCHECK(starts == 1 && idrs == 0);
  }
  // slow camera start (R2 #1): first AU 800 ms after start_encoding(), start PLIs every 100 ms from t = 0
  {
    A r;
    r.set_rtt_ns(3 * kMs);
    int starts = 0, idrs = 0;
    bool idr_seen_by_receiver = false;
    for (int64_t t = 0; t <= 3000 * kMs; t += 10 * kMs) {
      if (t == 800 * kMs) r.on_idr_numbered(0, t);
      if (t == 810 * kMs) r.on_idr_first_packet(t, 100 * kMs);
      if (t >= 813 * kMs) idr_seen_by_receiver = true;
      if (t % (100 * kMs) == 0 && !idr_seen_by_receiver) {
        auto a = r.on_pli(Pli{kNoFrame, kPliStart}, t);
        starts += a == A::kStart;
        idrs += a == A::kIdr;
      }
      auto a = r.on_timer(t);
      idrs += a == A::kIdr;
    }
    VCHECKF(starts == 1 && idrs == 0, "slow start: %d starts, %d IDRs", starts, idrs);
    VCHECK(r.counters().dup == 8);  // 100..800 ms coalesced
  }
  // a non-duplicate class-1 PLI inside the 1 s window is dropped, never served later; then the next one is
  {
    A r;
    r.on_pli(Pli{kNoFrame, kPliStart}, 0);
    r.on_idr_numbered(0, 30 * kMs);
    r.on_idr_first_packet(31 * kMs, 100 * kMs);
    VCHECK(r.on_pli(Pli{kNoFrame, kPliSize}, 500 * kMs) == A::kNone);
    VCHECK(r.counters().dropped_window == 1);
    int later = 0;
    for (int64_t t = 500 * kMs; t < 3000 * kMs; t += 10 * kMs) later += r.on_timer(t) != A::kNone;
    VCHECK(later == 0);
    VCHECK(r.on_pli(Pli{kNoFrame, kPliSize}, 1100 * kMs) == A::kIdr);  // window passed: served
  }
  // loss -> refresh with a verified LTR; duplicate ignored; 3 failed refreshes -> IDR; class-2 IDR limit 2 s
  {
    A r;
    r.set_rtt_ns(5 * kMs);
    r.on_pli(Pli{kNoFrame, kPliStart}, 0);
    r.on_idr_numbered(0, 30 * kMs);
    r.on_idr_first_packet(31 * kMs, 100 * kMs);
    r.on_ltr_sent(0);
    r.on_ltr_sent(30);
    auto v = r.on_rr(0, 40, false, 1500 * kMs);
    VCHECK(v.size() == 2 && r.has_verified());
    int64_t t = 3000 * kMs;
    VCHECK(r.on_pli(Pli{40, kPliLoss}, t) == A::kRefresh);
    VCHECK(r.on_pli(Pli{40, kPliLoss}, t + 30 * kMs) == A::kNone);  // same last_clean within RTT + 50 ms
    VCHECK(r.counters().dup == 1);
    // no newer clean run: three refreshes fail (1 s each), the next loss PLI asks for an IDR
    int refreshes = 1;
    for (int i = 0; i < 4; ++i) {
      t += 1100 * kMs;
      r.on_timer(t);
      auto a = r.on_pli(Pli{40, kPliLoss}, t);
      if (a == A::kRefresh) refreshes++;
      if (a == A::kIdr) break;
    }
    VCHECKF(refreshes == 3, "refreshes before the IDR: %d", refreshes);
    // decoder reopen -> start -> always an IDR, never a refresh (window open)
    A r2;
    r2.on_pli(Pli{kNoFrame, kPliStart}, 0);
    r2.on_idr_numbered(0, 30 * kMs);
    r2.on_idr_first_packet(31 * kMs, 100 * kMs);
    r2.on_ltr_sent(0);
    r2.on_rr(0, 10, false, 500 * kMs);
    VCHECK(r2.on_pli(Pli{10, kPliStart}, 2000 * kMs) == A::kIdr);
  }
  // an IDR clears the verified set: a loss PLI right after a recovery IDR asks for an IDR under the 2 s limit
  {
    A r;
    r.on_pli(Pli{kNoFrame, kPliStart}, 0);
    r.on_idr_numbered(0, 30 * kMs);
    r.on_idr_first_packet(31 * kMs, 100 * kMs);
    r.on_ltr_sent(0);
    r.on_rr(0, 5, false, 400 * kMs);
    VCHECK(r.has_verified());
    r.on_idr_numbered(60, 3000 * kMs);  // e.g. a refresh the backend turned into an IDR
    r.on_idr_first_packet(3001 * kMs, 100 * kMs);
    VCHECK(!r.has_verified());
    VCHECK(r.on_pli(Pli{5, kPliLoss}, 3300 * kMs) == A::kNone);  // deferred to the window's end
    VCHECK(r.on_timer(4900 * kMs) == A::kNone);
    VCHECK(r.on_timer(5000 * kMs) == A::kIdr);
    // the LTR before the IDR is never verified again
    VCHECK(r.on_rr(0, 5, false, 5100 * kMs).empty());
  }
  // PLIs every 300 ms after an IDR, no clean run reported: <= 1 IDR per 2 s, counting every AU with idr set
  {
    A r;
    r.on_pli(Pli{kNoFrame, kPliStart}, 0);
    std::vector<int64_t> idr_at;
    auto numbered = [&](int64_t t) {
      r.on_idr_numbered(uint32_t(t / kMs), t);
      r.on_idr_first_packet(t + kMs, 100 * kMs);
      idr_at.push_back(t);
    };
    numbered(30 * kMs);
    int64_t pending = 0;
    for (int64_t t = 300 * kMs; t <= 10000 * kMs; t += 10 * kMs) {
      if (pending && t >= pending) {
        numbered(t);
        pending = 0;
      }
      A::Action a = r.on_timer(t);
      if (t % (300 * kMs) == 0 && a == A::kNone) a = r.on_pli(Pli{kNoFrame, kPliLoss}, t);
      if ((a == A::kIdr || a == A::kRefresh) && !pending) pending = t + 30 * kMs;  // the encoder answers in 30 ms
    }
    bool spaced = true;
    for (size_t i = 1; i < idr_at.size(); ++i) spaced = spaced && idr_at[i] - idr_at[i - 1] >= 2000 * kMs;
    VCHECKF(spaced && idr_at.size() <= 6, "%zu IDRs in 10 s", idr_at.size());
  }
}

void test_video_verified_ltrs() {
  SenderRecovery r;
  r.on_pli(Pli{kNoFrame, kPliStart}, 0);
  r.on_idr_numbered(0, 0);
  for (uint32_t f : {0u, 30u, 60u, 90u}) r.on_ltr_sent(f);
  auto v = r.on_rr(0, 45, false, kMs);
  VCHECK((v == std::vector<uint32_t>{0, 30}));
  VCHECK(r.on_rr(0, 45, false, 2 * kMs).empty());  // once
  // a broken period: LTR 60 decoded during it is never inside a clean run; the next run starts at a refresh
  v = r.on_rr(70, 95, false, 3 * kMs);
  VCHECK((v == std::vector<uint32_t>{90}));
  VCHECK(r.newest_verified() == 90);
  // lost RRs only delay: a later RR with the union run still verifies
  r.on_ltr_sent(120);
  r.on_ltr_sent(150);
  v = r.on_rr(70, 160, false, 4 * kMs);
  VCHECK((v == std::vector<uint32_t>{120, 150}));
  // none / malformed runs
  VCHECK(r.on_rr(kNoFrame, kNoFrame, true, 5 * kMs).empty());
  VCHECK(r.on_rr(200, 100, false, 5 * kMs).empty());
}

// ===================================================================== timing (§4.5.3)

void test_video_playout_target() {
  PlayoutTarget p;
  int64_t t = 0;
  for (int i = 0; i < 19; ++i) {
    p.add(t, 20);
    t += 33 * kMs;
  }
  VCHECK(std::fabs(p.target_ms(t) - 70) < 1e-9);  // 70 ms until 20 samples
  p.add(t, 20);
  double d = p.target_ms(t);
  VCHECK(std::fabs(d - 70) < 1e-9 || d < 70);
  // falls by at most 15 ms per 0.5 s: 70 -> 50 (floor) takes >= 0.66 s
  double prev = d;
  bool rate_ok = true;
  for (int i = 0; i < 40; ++i) {
    t += 50 * kMs;
    p.add(t, 20);
    d = p.target_ms(t);
    rate_ok = rate_ok && prev - d <= 1.5 + 1e-6;
    prev = d;
  }
  VCHECK(rate_ok);
  VCHECKF(std::fabs(d - 50) < 1e-6, "floor: %.2f", d);
  // a spike rises at once (p99 + 8)
  for (int i = 0; i < 3; ++i) p.add(t, 100);
  d = p.target_ms(t);
  VCHECKF(std::fabs(d - 108) < 1e-6, "spike: %.2f", d);
  // ceiling 150
  for (int i = 0; i < 10; ++i) p.add(t, 400);
  VCHECK(std::fabs(p.target_ms(t) - 150) < 1e-6);
  // the spike leaves the 10 s window: D comes down, never under the floor; a new floor applies live
  for (int i = 0; i < 500; ++i) {
    t += 33 * kMs;
    p.add(t, 15);
    d = p.target_ms(t);
  }
  VCHECK(std::fabs(d - 50) < 1e-6);
  p.set_floor_ms(30);
  for (int i = 0; i < 100; ++i) {
    t += 33 * kMs;
    p.add(t, 15);
    d = p.target_ms(t);
  }
  VCHECKF(std::fabs(d - 30) < 1e-6, "floor 30: %.2f (p99 15 + 8 = 23)", d);
}

void test_video_clock_offset() {
  // min-RTT of the last 10 pong samples; offset = peer clock minus ours
  OffsetEstimator e;
  const int64_t off = 3600LL * 3 * 1'000'000'000;  // the peer is 3 h ahead
  int64_t now = 10'000'000'000LL;
  for (int i = 0; i < 12; ++i) {
    const int64_t up = (i == 4 ? 1 : 5 + i % 3) * kMs, down = (i == 4 ? 1 : 3 + i % 5) * kMs;
    const int64_t t0 = now, t1 = t0 + up + off, t2 = t1 + 200'000, t3 = t0 + up + down + 200'000;
    e.add(t0, t1, t2, t3);
    now += 1'000'000'000;
  }
  int64_t o, age, rtt;
  VCHECK(e.get(now, &o, &age, &rtt));
  VCHECKF(std::llabs(o - off) <= kMs / 2 && rtt == 2 * kMs, "offset err %lld ns, rtt %lld", (long long)(o - off), (long long)rtt);
  // after 6 more samples the min-RTT one is older than the last 10: a newer one is picked
  for (int i = 0; i < 7; ++i) {
    const int64_t t0 = now, t1 = t0 + 4 * kMs + off, t2 = t1, t3 = t0 + 8 * kMs;
    e.add(t0, t1, t2, t3);
    now += 1'000'000'000;
  }
  VCHECK(e.get(now, &o, &age, &rtt) && rtt == 8 * kMs && std::llabs(o - (off - 0)) <= kMs);

  // provisional seed (R2 #5), both directions: stream started in link state, no pong
  for (int dir : {+1, -1}) {
    Rig r;
    r.have_offset = false;
    const int64_t true_off = dir * 3600LL * 3 * 1'000'000'000;  // S - T
    const int64_t transit = 25 * kMs, enc = 5 * kMs;
    // sender capture of frame f at S clock: s0 + f * 33.33 ms; T receives at s - off + enc + transit
    const int64_t s0 = r.t + true_off + 1000 * kMs;
    r.map(0, s0);
    int64_t worst = 0;
    for (uint32_t f = 0; f < 40; ++f) {
      const int64_t cap = s0 + int64_t(f) * 33'333'333;
      const int64_t arrive = cap - true_off + enc + transit;
      r.advance(arrive - r.t);
      for (auto& p : r.make_au(f, f == 0 ? 20000 : 3000, f == 0 ? kVIdr : 0, f * 3000, uint16_t(enc / 100'000))) r.deliver(p);
      r.run();
      if (f == 0) {
        auto pr = r.core.probe();
        VCHECKF(pr.provisional && std::llabs(pr.offset_ns - true_off) <= 20 * kMs, "dir %d seed err %.1f ms", dir,
                double(pr.offset_ns - true_off) * 1e-6);
      } else {
        const int64_t pt = r.core.present_time_of(f);
        const double D = r.core.probe().target_ms;
        if (pt > 1) worst = std::max<int64_t>(worst, std::llabs(pt - (r.t + int64_t(D * 1e6))));
      }
    }
    VCHECKF(worst <= 25 * kMs, "dir %d present time vs completion + D: %.1f ms", dir, double(worst) * 1e-6);
  }
}

void test_video_drift_30min() {
  // 30 min at 30 fps, the sender clock running 25 ppm fast; the pong estimate (min-RTT) follows each second
  for (double ppm : {+25.0, -25.0}) {
    Rig r;
    const int64_t off0 = 123'456'789'000LL, transit = 20 * kMs, enc = 4 * kMs;
    int64_t t0 = r.t;
    auto true_off = [&](int64_t t) { return off0 + int64_t(double(t - t0) * ppm * 1e-6); };
    r.offset = off0;
    const int64_t s0 = t0 + off0;
    r.map(0, s0);
    double worst = 0, d_first = -1, d_last = 0;
    const uint32_t frames = 30 * 60 * 30;
    for (uint32_t f = 0; f < frames; ++f) {
      // capture at S: s0 + f * period (S clock); T time of that = cap - true_off
      const int64_t cap_s = s0 + int64_t(f) * 33'333'333;
      int64_t arrive = cap_s - off0 + enc + transit;  // first guess
      arrive = cap_s - true_off(arrive) + enc + transit;
      r.t = std::max(r.t, arrive);
      if (f % 30 == 0) r.offset = true_off(r.t);  // a pong per second
      if (f % 300 == 0) r.map(f * 3000, cap_s);    // time map every 10 s (exact for video)
      for (auto& p : r.make_au(f, f == 0 ? 20000 : 2000, f == 0 ? kVIdr : 0, f * 3000, uint16_t(enc / 100'000))) r.deliver(p);
      r.run();
      const int64_t pt = r.core.present_time_of(f);
      const double D = r.core.probe().target_ms;
      if (f > 100 && pt > 1) {
        // ideal: encoder output mapped with the true offset + D
        const int64_t ideal = cap_s + enc - true_off(r.t) + int64_t(D * 1e6);
        worst = std::max(worst, std::fabs(double(pt - ideal)) * 1e-6);
        if (d_first < 0) d_first = D;
        d_last = D;
      }
      r.t = std::max(r.t, pt);  // present it
      r.run(true);
    }
    VCHECKF(worst < 2.0, "ppm %+.0f: present-time error %.3f ms", ppm, worst);
    VCHECKF(std::fabs(d_last - d_first) < 1e-6 && std::fabs(d_last - 50) < 1e-6, "D %.2f -> %.2f", d_first, d_last);
  }
}

void test_video_bootstrap_and_unseen() {
  Rig r;
  // no time map yet: decoded and presented on decode, no d sample
  for (uint32_t f = 0; f < 5; ++f) {
    for (auto& p : r.make_au(f, f == 0 ? 30000 : 3000, f == 0 ? kVIdr : 0, f * 3000, 50)) r.deliver(p);
    r.run();
    VCHECKF(r.was_presented(f), "bootstrap frame %u presented on decode", f);
    r.advance(33 * kMs);
  }
  VCHECK(r.core.stats(1.0)["d_samples"] == 0);
  VCHECK(r.placeholders.size() == 1 && r.placeholders[0] == "starting");
  // the first entry arrives: present times from now on; D = 70 until 20 samples
  const int64_t s_of_t = r.t - 5 * 33'333'333LL;  // sender clock = receiver clock (offset 0), frame 0 at this time
  r.map(0, s_of_t);
  uint32_t f = 5;
  for (; f < 24; ++f) {
    r.t = s_of_t + int64_t(f) * 33'333'333 + 25 * kMs;
    for (auto& p : r.make_au(f, 3000, 0, f * 3000, 50)) r.deliver(p);
    r.run();
    VCHECK(std::fabs(r.core.probe().target_ms - 70) < 1e-9);
  }
  r.t = s_of_t + int64_t(f) * 33'333'333 + 25 * kMs;
  for (auto& p : r.make_au(f, 3000, 0, f * 3000, 50)) r.deliver(p);
  r.run();
  VCHECK(r.core.probe().target_ms < 70);  // 20 samples: p99 + 8 (falling at <= 30 ms/s)
  // an unseen number: frame f+1 never arrives, f+2 does; f+1 is lost at f+2's present time minus one period
  r.advance(40 * kMs);
  const uint32_t g = f + 2;
  r.t = s_of_t + int64_t(g) * 33'333'333 + 2 * kMs;  // early enough that f+1's deadline has not passed
  for (auto& p : r.make_au(g, 3000, 0, g * 3000, 50)) r.deliver(p);
  r.run();
  const double D = r.core.probe().target_ms;
  const int64_t pt_g = s_of_t + int64_t(g) * 33'333'333 + 50 * 100'000 + int64_t(D * 1e6);
  const uint64_t holes0 = r.core.probe().holes;
  r.advance(pt_g - 33'333'333 - r.t - 2 * kMs, false);
  VCHECK(r.core.probe().holes == holes0);
  r.advance(4 * kMs, false);
  VCHECK(r.core.probe().holes == holes0 + 1);
  VCHECK(r.core.probe().broken);
}

void test_video_lost_vs_late() {
  Rig r;
  const int64_t s0 = r.t;
  r.map(0, s0);
  auto send = [&](uint32_t f, size_t bytes, uint8_t fl, const std::vector<size_t>& skip = {}) {
    auto pk = r.make_au(f, bytes, fl, f * 3000, 50);
    for (size_t i = 0; i < pk.size(); ++i)
      if (std::find(skip.begin(), skip.end(), i) == skip.end()) r.deliver(pk[i]);
    return pk;
  };
  for (uint32_t f = 0; f < 10; ++f) {
    r.t = s0 + int64_t(f) * 33'333'333 + 20 * kMs;
    send(f, f == 0 ? 30000 : 3000, f == 0 ? kVIdr : 0);
    r.run();
  }
  // frame 10: one packet of two arrives, then nothing newer (a Wi-Fi spike delays everything behind it)
  r.t = s0 + 10 * 33'333'333LL + 20 * kMs;
  auto pk10 = send(10, 2000, 0, {1});
  r.advance(150 * kMs, false);  // late, but within D + 150 ms (older backlog is decoded, not kept)
  VCHECK(r.core.probe().lost == 0 && !r.was_presented(10));  // late, not lost
  r.deliver(pk10[1]);
  r.run();
  VCHECK(r.core.probe().lost == 0 && r.was_presented(10) && r.core.probe().late >= 1);
  // frame 11 incomplete at its present time while frame 12 (higher seq) arrived: lost (no NACK repair here)
  const int64_t base = r.t;
  r.t = s0 + 11 * 33'333'333LL + 20 * kMs;
  send(11, 2000, 0, {1});
  r.t += 33 * kMs;
  send(12, 2000, 0);
  r.run();
  VCHECK(r.core.probe().lost == 0);
  r.advance(400 * kMs, false);
  VCHECKF(r.core.probe().lost == 1, "lost %llu", (unsigned long long)r.core.probe().lost);
  VCHECK(!r.was_presented(11) && !r.was_presented(12));  // 12 has an incomplete reference chain
  VCHECK(r.keep.count(12) && !r.keep[12]);
  (void)base;
  // NACK: immediate on the gap, retried after max(2 x RTT, 15 ms), 3 tries, then lost on that evidence
  Rig n;
  n.map(0, n.t);
  for (auto& p : n.make_au(0, 30000, kVIdr, 0, 50)) n.deliver(p);
  n.run();
  auto pa = n.make_au(1, 3000, 0, 3000, 50);
  n.fb.clear();
  n.deliver(pa[0]);
  n.deliver(pa[2]);  // pa[1] missing
  VCHECK(n.count_fb(kNack) == 1);
  int tries = 1;
  for (int i = 0; i < 10; ++i) {
    n.t += 10 * kMs;
    n.net();
    tries += n.count_fb(kNack);
  }
  VCHECKF(tries == 3, "NACK tries %d", tries);
  n.run();
  VCHECK(n.core.probe().lost == 1);  // NACK gave up before the present time
}

void test_video_presenter() {
  Rig r;
  const int64_t s0 = r.t;
  r.map(0, s0);
  // AUs before the first IDR are never decoded; `starting` stays until the IDR
  for (uint32_t f = 0; f < 5; ++f) {
    r.t = s0 + int64_t(f) * 33'333'333 + 10 * kMs;
    for (auto& p : r.make_au(f, 3000, 0, f * 3000, 50)) r.deliver(p);
    r.advance(kMs);
  }
  VCHECK(r.decoded.empty() && r.presented.empty() && r.core.probe().placeholder == "starting");
  // the IDR (late by construction, ~300 ms after capture) is shown at once
  r.t = s0 + 5 * 33'333'333LL + 300 * kMs;
  for (auto& p : r.make_au(5, 60000, kVIdr | kVLtr, 5 * 3000, 50)) r.deliver(p);
  r.run();
  VCHECK(r.was_presented(5) && r.core.probe().placeholder.empty());
  // the frames behind it arrive late too (shown on arrival), then on time: playout re-converges
  uint64_t late_before = r.core.probe().late;
  for (uint32_t f = 6; f < 40; ++f) {
    const int64_t cap = s0 + int64_t(f) * 33'333'333;
    r.t = std::max(r.t, f < 12 ? cap + 300 * kMs - (f - 6) * 40 * kMs : cap + 20 * kMs);
    for (auto& p : r.make_au(f, 3000, f < 12 ? kVBehindIdr : 0, f * 3000, 50)) r.deliver(p);
    r.advance(kMs);
  }
  r.advance(200 * kMs);
  const uint64_t late = r.core.probe().late - late_before;
  VCHECKF(late >= 1 && late <= 8, "late %llu", (unsigned long long)late);
  // the last frames are presented at their present time (completion + ~D - 20 ms), not on arrival
  int64_t pt39 = 0;
  for (auto& x : r.presented)
    if (x.first == 39) pt39 = x.second;
  const int64_t cap39 = s0 + 39 * 33'333'333LL;
  const double D = r.core.probe().target_ms;
  VCHECKF(pt39 && std::llabs(pt39 - (cap39 + 50 * 100'000 + int64_t(D * 1e6))) <= 2 * kMs, "frame 39 at %.1f ms after capture, D %.1f",
          double(pt39 - cap39) * 1e-6, D);
  // never backwards: a burst presents only the newest; never an older one after it
  r.auto_decode = false;
  for (uint32_t f = 40; f < 45; ++f)
    for (auto& p : r.make_au(f, 3000, 0, f * 3000, 50)) r.deliver(p);
  r.t = s0 + 50 * 33'333'333LL;
  r.run();
  for (uint32_t f : {40u, 41u, 42u, 43u, 44u}) r.core.on_decoded(f, true, 1, r.t);
  r.auto_decode = true;
  const size_t n0 = r.presented.size();
  r.run();
  VCHECK(r.presented.size() == n0 + 1 && r.presented.back().first == 44);
  // hold -> 2 s -> reconnecting -> back to live on the next clean picture (a refresh after a loss)
  r.advance(1900 * kMs);
  VCHECK(r.core.probe().placeholder.empty());
  r.advance(200 * kMs);
  VCHECK(r.core.probe().placeholder == "reconnecting");
  // frames 45.. were never sent: holes; then a refresh at 120 (references a verified LTR) restarts the run
  const uint32_t g = 120;
  r.t = s0 + int64_t(g) * 33'333'333 + 20 * kMs;
  for (auto& p : r.make_au(g, 14000, kVRefresh, g * 3000, 50)) r.deliver(p);
  r.advance(300 * kMs);  // D is at its 150 ms ceiling here (the burst above arrived ~180 ms late)
  VCHECK(r.was_presented(g) && r.core.probe().placeholder.empty() && !r.core.probe().broken);
  VCHECK(r.core.probe().clean_since == g);
  // a decoder error revokes that frame and every later kept picture, goes broken, PLI decoder
  r.fail_decode.insert(g + 2);
  r.auto_decode = false;
  for (uint32_t f = g + 1; f <= g + 4; ++f) {
    r.t += 33 * kMs;
    for (auto& p : r.make_au(f, 3000, 0, f * 3000, 50)) r.deliver(p);
    r.run();
  }
  r.core.on_decoded(g + 1, true, 1, r.t);
  r.core.on_decoded(g + 2, false, 1, r.t);
  r.core.on_decoded(g + 3, true, 1, r.t);
  r.core.on_decoded(g + 4, true, 1, r.t);
  r.auto_decode = true;
  r.advance(300 * kMs);
  VCHECK(!r.was_presented(g + 2) && !r.was_presented(g + 3) && !r.was_presented(g + 4));
  VCHECK(r.core.probe().broken && r.core.probe().acked == g + 1);
  r.net();
  VCHECK(r.count_fb(kPli) == 1);
  // keep = false pictures (broken period) are never presented
  for (auto& kv : r.keep)
    if (!kv.second) VCHECK(!r.was_presented(kv.first));
}

// ===================================================================== pacer (§4.4)

void test_video_pacer_timing() {
  struct Rec {
    int64_t t;
    size_t n;
    bool idr;
  };
  std::mutex mu;
  std::vector<Rec> sent;
  Pacer pacer([&](const PacedPacket& p) {
    std::lock_guard<std::mutex> lock(mu);
    sent.push_back(Rec{now_ns(), p.bytes->size(), p.idr});
  });
  pacer.test_overshoot_ns.store(350'000);  // C4: 0.35 ms of sleep overshoot per wake-up
  auto blob = std::make_shared<std::vector<uint8_t>>(1200, 0);
  for (double bitrate : {2.5e6, 5e6}) {
    sent.clear();
    const int npk = bitrate > 3e6 ? 214 : 100;  // 120 KB / 256 KB IDRs
    std::vector<PacedPacket> idr;
    for (int i = 0; i < npk; ++i) {
      PacedPacket p;
      p.bytes = blob;
      p.stream = 5;
      p.frame = 1;
      p.idr = true;
      p.first = i == 0;
      p.end = i == npk - 1;
      p.idr_bps = 4.0 * bitrate;
      idr.push_back(p);
    }
    pacer.enqueue(std::move(idr));
    VCHECK(pacer.idr_queued(5));
    // a P frame behind it waits (behind-IDR)
    PacedPacket pf;
    pf.bytes = blob;
    pf.stream = 5;
    pf.frame = 2;
    pf.first = pf.end = true;
    pacer.enqueue({pf});
    std::this_thread::sleep_for(std::chrono::milliseconds(npk * 1200 * 8 * 1000 / int(4 * bitrate) + 80));
    VCHECK(!pacer.idr_queued(5));
    std::lock_guard<std::mutex> lock(mu);
    VCHECKF(int(sent.size()) == npk + 1, "%zu sent", sent.size());
    if (int(sent.size()) < npk) continue;
    const double dur = double(sent[size_t(npk - 1)].t - sent[0].t) * 1e-9;
    const double want = double(npk - 1) * 1200 * 8 / (4.0 * bitrate);
    VCHECKF(std::fabs(dur - want) <= 0.05 * want, "IDR at %.1f Mbit/s: %.1f ms for %.1f ms", bitrate * 4e-6, dur * 1e3, want * 1e3);
    // no 10 ms window carries more than 4 x bitrate x 10 ms + 32 packets
    size_t worst = 0;
    for (size_t i = 0; i < size_t(npk); ++i) {
      size_t b = 0;
      for (size_t k = i; k < size_t(npk) && sent[k].t - sent[i].t < 10 * kMs; ++k) b += sent[k].n;
      worst = std::max(worst, b);
    }
    VCHECKF(double(worst) <= 4.0 * bitrate / 8 * 0.010 + 32 * 1200, "10 ms window %zu B", worst);
    VCHECK(!sent.back().idr && sent.back().t >= sent[size_t(npk - 1)].t);
    std::printf("    IDR %d packets at %.0f Mbit/s: %.1f ms (schedule %.1f ms), max 10 ms window %zu B\n", npk,
                bitrate * 4e-6, dur * 1e3, want * 1e3, worst);
  }
  // bucket: 32 packets at once, then 25 Mbit/s
  pacer.test_overshoot_ns.store(0);
  {
    std::lock_guard<std::mutex> lock(mu);
    sent.clear();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(30));  // refill
  std::vector<PacedPacket> pk;
  for (int i = 0; i < 64; ++i) {
    PacedPacket p;
    p.bytes = blob;
    p.stream = 6;
    p.frame = uint32_t(10 + i / 8);
    pk.push_back(p);
  }
  const int64_t t0 = now_ns();
  pacer.enqueue(std::move(pk));
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  std::lock_guard<std::mutex> lock(mu);
  VCHECK(sent.size() == 64);
  if (sent.size() == 64) {
    VCHECKF(sent[31].t - t0 < 3 * kMs, "32nd packet after %.2f ms", double(sent[31].t - t0) * 1e-6);
    const double rest = double(sent[63].t - sent[31].t) * 1e-9, want = 32 * 1200 * 8 / 25e6;
    VCHECKF(rest > 0.8 * want && rest < 1.6 * want, "bucket rest %.2f ms (%.2f)", rest * 1e3, want * 1e3);
  }
}

// ===================================================================== tick latency (wake, §4.2)

void test_video_tick_latency() {
  // One flow (offerer) on the network loop against a raw socket that answers hellos as the peer.
  uint8_t key[kKeyLen];
  for (size_t i = 0; i < kKeyLen; ++i) key[i] = uint8_t(i + 3);
  EventSink ev([](const std::string&) {});
  auto flow = std::make_shared<Flow>("0000cccc", "peer", true, key, 10, &ev);
  std::string err;
  VCHECK(flow->open_socket("", 0, &err));
  net::Socket peer = net::udp_open(&err);
  VCHECK(peer != net::kInvalid && net::udp_bind(peer, 0x7F000001, 0, &err));
  net::Addr fa;
  fa.ip = 0x7F000001;
  fa.port = flow->port();
  VCHECK(net::udp_connect(peer, fa, &err));
  VCHECK(flow->set_remote("127.0.0.1", net::local_port(peer), &err));
  Sealer peer_ctrl(key, kDirBit);
  auto hello = [&] {
    uint8_t hp[4] = {1, 1, 0, 0}, buf[kMaxPacket];
    Header h;
    h.ptype = kHello;
    net::udp_send(peer, buf, peer_ctrl.seal(h, hp, 4, buf));
  };
  NetLoop loop;
  loop.start();
  flow->start();
  loop.add(flow);
  hello();
  for (int i = 0; i < 100 && flow->state() != FlowState::Up; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  VCHECK(flow->state() == FlowState::Up);
  auto vb = make_null_video_backend("dec_ms=0");
  VideoRecvStream::Config cfg;
  cfg.id = 9;
  std::memcpy(cfg.key, key, kKeyLen);
  LipSyncHold hold;
  auto vs = std::make_shared<VideoRecvStream>(cfg, flow, &ev, nullptr, &hold);
  vs->core().timing.rr_ns = 50 * kMs;
  vs->core().timing.start_pli_ns = 60'000 * kMs;  // no periodic repeats: only the posted PLI below
  std::string code, msg;
  VCHECK(vs->start(*vb, &code, &msg));
  flow->attach_video_recv(vs);
  // drain: read the peer socket and classify
  std::vector<double> pli_lat, rr_late;
  int64_t last_rr = 0;
  auto read_for = [&](int64_t until, int64_t trigger, bool* got_pli) {
    uint8_t buf[2048];
    while (now_ns() < until) {
      bool rd = false;
      net::poll_readable(&peer, 1, &rd, 1);
      int n;
      while (rd && (n = net::udp_recv(peer, buf, sizeof buf, nullptr)) > 0) {
        const int64_t t = now_ns();
        Header h;
        if (!read_header(buf, size_t(n), &h) || h.stream != 9) continue;
        if (h.ptype == kPli && trigger && !*got_pli) {
          *got_pli = true;
          pli_lat.push_back(double(t - trigger) * 1e-6);
        }
        if (h.ptype == kRR) {
          // lateness against the 50 ms grid of the first RR seen (the timer runs on an absolute schedule)
          if (!last_rr) last_rr = t;
          else {
            const int64_t k = (t - last_rr + 25 * kMs) / (50 * kMs);
            rr_late.push_back(double(t - last_rr - k * 50 * kMs) * 1e-6);
          }
        }
      }
    }
  };
  bool dummy = false;
  read_for(now_ns() + 300 * kMs, 0, &dummy);
  for (int i = 0; i < 20; ++i) {
    hello();  // keep the flow up
    const int64_t trig = now_ns();
    vs->on_sink_event("device_lost", "");  // the video thread posts a PLI start
    bool got = false;
    read_for(trig + 80 * kMs + int64_t(i % 7) * kMs, trig, &got);
    VCHECK(got);
  }
  std::sort(pli_lat.begin(), pli_lat.end());
  if (!rr_late.empty()) {  // relative to the earliest RR (the first one seen may itself be late)
    const double m = std::min(0.0, *std::min_element(rr_late.begin(), rr_late.end()));
    for (double& x : rr_late) x -= m;
  }
  std::sort(rr_late.begin(), rr_late.end());
  const double p95 = pli_lat.empty() ? 99 : pli_lat[pli_lat.size() * 95 / 100];
  const double rr95 = rr_late.empty() ? 99 : rr_late[rr_late.size() * 95 / 100];
  double rr_mean = 0;
  for (double x : rr_late) rr_mean += x;
  rr_mean = rr_late.empty() ? 99 : rr_mean / double(rr_late.size());
  VCHECKF(p95 < 2.0, "posted PLI -> sent p95 %.2f ms", p95);
  VCHECKF(rr_mean < 1.0 && rr95 < 2.0, "RR lateness mean %.2f ms, p95 %.2f ms", rr_mean, rr95);
  std::printf("    posted PLI on the wire: p50 %.2f ms, p95 %.2f ms; 50 ms RR lateness mean %.2f, p95 %.2f ms (%zu)\n",
              pli_lat.empty() ? 0 : pli_lat[pli_lat.size() / 2], p95, rr_mean, rr95, rr_late.size());
  flow->detach(9);
  vs->stop();
  loop.close(flow);
  loop.stop();
  net::close_socket(peer);
}

// ===================================================================== in-process helpers with video

class VProc {
 public:
  VProc(const std::string& null_opts, const std::string& video_opts, FlowTiming timing = FlowTiming()) {
    backend_ = make_null_backend(null_opts);
    video_ = make_null_video_backend(video_opts);
    HelperOptions opt;
    opt.dev = true;
    opt.timing = timing;
    opt.stats_period_ms = 500;
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
      if (j.is_object()) j["_t"] = now_ns();
      std::lock_guard<std::mutex> lock(omu_);
      log_.push_back(j);
      ocv_.notify_all();
    };
    thread_ = std::thread([this, opt, io] { run_control(*backend_, opt, io); });
  }
  ~VProc() {
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
  std::vector<Json> events_since(size_t from) {
    std::lock_guard<std::mutex> lock(omu_);
    return std::vector<Json>(log_.begin() + long(std::min(from, log_.size())), log_.end());
  }
  Json call(Json cmd, int timeout_ms = 3000) {
    static std::atomic<int> req{1000};
    const int r = req++;
    cmd["req"] = r;
    size_t c = cursor();
    send(cmd);
    Json out;
    if (!wait(&c, [r](const Json& j) { return j.value("req", -1) == r && (j["ev"] == "ok" || j["ev"] == "error"); }, timeout_ms, &out))
      return Json{{"ev", "timeout"}};
    return out;
  }
  Json report() {
    size_t c = cursor();
    call(Json{{"cmd", "dev"}, {"null_video", {{"report", true}}}});
    Json out;
    wait(&c, [](const Json& j) { return j.value("ev", "") == "null_video"; }, 2000, &out);
    return out.is_object() ? out["report"] : Json();
  }
  Json last_stats(uint32_t stream, int timeout_ms = 2000) {
    size_t c = cursor();
    Json st;
    const std::string k = std::to_string(stream);
    if (!wait(&c, [&](const Json& j) { return j.value("ev", "") == "stats" && j["streams"].contains(k); }, timeout_ms, &st))
      return Json();
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

std::function<bool(const Json&)> is_ev(const std::string& ev, const std::string& field = "", const Json& value = nullptr) {
  return [=](const Json& j) {
    if (!j.is_object() || j.value("ev", "") != ev) return false;
    if (field.empty()) return true;
    auto it = j.find(field);
    return it != j.end() && *it == value;
  };
}

std::string vkey(uint8_t fill) { return b64_encode(std::vector<uint8_t>(32, fill)); }

bool vopen_pair(VProc& a, VProc& b, const std::string& flow) {
  size_t ca = a.cursor(), cb = b.cursor();
  Json fa, fb;
  a.send(Json{{"cmd", "flow_open"}, {"req", 1}, {"flow", flow}, {"peer", "B"}, {"role", "offer"}, {"port", 0},
              {"host", "127.0.0.1"}, {"remote", nullptr}, {"key", vkey(7)}, {"rr_ms", 10}});
  if (!a.wait(&ca, is_ev("flow_bound"), 3000, &fa)) return false;
  b.send(Json{{"cmd", "flow_open"}, {"req", 1}, {"flow", flow}, {"peer", "A"}, {"role", "answer"}, {"port", 24833},
              {"remote", {{"host", "127.0.0.1"}, {"port", fa["port"]}}}, {"key", vkey(7)}, {"rr_ms", 10}});
  if (!b.wait(&cb, is_ev("flow_bound"), 3000, &fb)) return false;
  a.send(Json{{"cmd", "flow_remote"}, {"req", 2}, {"flow", flow}, {"host", "127.0.0.1"}, {"port", fb["port"]}});
  return a.wait(&ca, is_ev("flow_state", "state", "up"), 3000) && b.wait(&cb, is_ev("flow_state", "state", "up"), 3000);
}

Json vcodec(int w = 1280, int h = 720, int br = 2500000) {
  return Json{{"name", "h264"}, {"profile", "constrained_high"}, {"w", w}, {"h", h}, {"fps", 30}, {"bitrate", br}, {"mtu", 1200}, {"ltr", true}};
}

Json send_video(const std::string& flow, uint32_t id, Json codec = vcodec(), Json extra = Json::object()) {
  Json j{{"cmd", "send"}, {"stream", id}, {"flow", flow}, {"kind", "video"}, {"source", {{"camera", nullptr}}},
         {"codec", codec}, {"key", vkey(3)}, {"link", {{"bitrate", 1000000}, {"fps", 15}}}};
  for (auto it = extra.begin(); it != extra.end(); ++it) j[it.key()] = it.value();
  return j;
}

Json recv_video(const std::string& flow, uint32_t id, Json codec = vcodec()) {
  return Json{{"cmd", "recv"}, {"stream", id}, {"flow", flow}, {"kind", "video"}, {"codec", codec}, {"key", vkey(3)},
              {"sink", {{"virtual_camera", true}}}};
}

// Relays link_out of one helper into link_in of the other (Beamer's part over the link).
class VRelay {
 public:
  VRelay(VProc& a, VProc& b) : a_(a), b_(b) { t_ = std::thread([this] { run(); }); }
  ~VRelay() {
    stop_ = true;
    t_.join();
  }
  std::atomic<uint64_t> video_events{0}, feedback_events{0}, bad_video_events{0}, video_bytes{0};

 private:
  void run() {
    size_t ca = a_.cursor(), cb = b_.cursor();
    while (!stop_) {
      pump(a_, b_, &ca);
      pump(b_, a_, &cb);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }
  void pump(VProc& from, VProc& to, size_t* c) {
    auto evs = from.events_since(*c);
    *c += evs.size();
    for (auto& e : evs) {
      if (e.value("ev", "") != "link_out") continue;
      if (e.value("video", false)) {
        video_events++;
        if (e["p"].size() > 4 || !e.contains("frame") || !e.contains("end")) bad_video_events++;
        for (auto& p : e["p"]) video_bytes += p.get<std::string>().size() * 3 / 4;
      } else {
        feedback_events++;
      }
      to.send(Json{{"cmd", "link_in"}, {"flow", e["flow"]}, {"p", e["p"]}});
    }
  }
  VProc &a_, &b_;
  std::thread t_;
  std::atomic<bool> stop_{false};
};

int64_t ev_time(const Json& j) { return j.value("_t", int64_t(0)); }

// first frame, exactly one IDR, 0 incorrect kept pictures; the slow camera; 1080p coded 1088
void test_video_e2e_start() {
  struct Case {
    const char* name;
    const char* vopts;
    Json codec;
    double max_first_ms;
  } cases[] = {
      {"720p, camera start 5 ms", "cam_start_ms=5", vcodec(), 300},
      {"720p, camera start 800 ms", "cam_start_ms=800", vcodec(), 1100},
      {"1080p 5 Mbit/s (coded 1088)", "cam_start_ms=5", vcodec(1920, 1080, 5000000), 300},
  };
  for (auto& k : cases) {
    VProc a("", k.vopts), b("", "");
    VCHECK(vopen_pair(a, b, "0000d001"));
    size_t ca = a.cursor(), cb = b.cursor();
    const int64_t t_send = now_ns();
    VCHECK(a.call(send_video("0000d001", 501, k.codec))["ev"] == "ok");
    VCHECK(b.call(recv_video("0000d001", 501, k.codec))["ev"] == "ok");
    Json running;
    VCHECK(a.wait(&ca, is_ev("stream_state", "state", "running"), 3000, &running));
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    Json ra = a.report(), rb = b.report();
    const double first_ms = double(rb["sink"].value("first_present_ns", int64_t(0)) - t_send) * 1e-6;
    VCHECKF(ra["enc"]["idr"] == 1, "%s: %s IDRs", k.name, ra["enc"]["idr"].dump().c_str());
    VCHECKF(first_ms > 0 && first_ms <= k.max_first_ms, "%s: first frame %.0f ms after send", k.name, first_ms);
    VCHECK(rb["dec"]["incorrect_kept"] == 0 && rb["sink"]["incorrect_presented"] == 0 && rb["sink"]["backwards"] == 0);
    VCHECK(rb["dec"]["before_idr"] == 0 && rb["dec"]["size_rejected"] == 0);
    VCHECK(ra["enc"]["number_mismatch"] == 0);
    VCHECKF(rb["sink"]["presented"].get<int>() > 70, "%s: %d presented", k.name, rb["sink"]["presented"].get<int>());
    VCHECK(rb["sink"]["w"] == k.codec["w"] && rb["sink"]["h"] == k.codec["h"]);
    Json st = b.last_stats(501);
    VCHECK(st.is_object() && st.value("lost", 1) == 0 && st.value("pli_tx", 0) >= 1);
    std::printf("    %s: first frame %.0f ms after send, %d presented, B %s\n", k.name, first_ms,
                rb["sink"]["presented"].get<int>(), st.dump().substr(0, 160).c_str());
    (void)cb;
  }
}

// camera-on guards (§6.5): no PLI within 3 s; no feedback for 5 s; a 1.5 s gap is nothing
void test_video_e2e_no_receiver() {
  {
    VProc a("", ""), b("", "");
    VCHECK(vopen_pair(a, b, "0000d002"));
    size_t ca = a.cursor();
    Json ok = a.call(send_video("0000d002", 502));
    VCHECK(ok["ev"] == "ok");
    Json failed;
    VCHECK(a.wait(&ca, is_ev("stream_state", "state", "failed"), 5000, &failed));
    const double dt = double(ev_time(failed) - ev_time(ok)) * 1e-9;
    VCHECKF(failed.value("error", "") == "no_receiver" && std::fabs(dt - 3.0) <= 0.05, "failed %s after %.3f s",
            failed.dump().c_str(), dt);
    Json r = a.report();
    VCHECK(r["camera"]["open"] == 0);
    const double close_dt = double(r["camera"]["last_close_ns"].get<int64_t>() - ev_time(ok)) * 1e-9;
    VCHECKF(std::fabs(close_dt - 3.0) <= 0.05, "handle closed after %.3f s", close_dt);
  }
  {
    VProc a("", ""), b("", "");
    VCHECK(vopen_pair(a, b, "0000d003"));
    size_t ca = a.cursor();
    VCHECK(a.call(send_video("0000d003", 503))["ev"] == "ok");
    VCHECK(b.call(recv_video("0000d003", 503))["ev"] == "ok");
    VCHECK(a.wait(&ca, is_ev("stream_state", "state", "running"), 3000));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    // a 1.5 s feedback gap (B's UDP blocked: no RR, no PLI) is nothing
    VCHECK(b.call(Json{{"cmd", "dev"}, {"drop_udp", true}})["ev"] == "ok");
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    VCHECK(b.call(Json{{"cmd", "dev"}, {"drop_udp", false}})["ev"] == "ok");
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    VCHECK(!a.wait(&ca, is_ev("stream_state", "state", "failed"), 10));
    // the receiver goes away silently: no feedback for 5 s -> failed no_receiver
    Json stopped = b.call(Json{{"cmd", "stop"}, {"stream", 503}});
    Json failed;
    VCHECK(a.wait(&ca, is_ev("stream_state", "state", "failed"), 7000, &failed));
    const double dt = double(ev_time(failed) - ev_time(stopped)) * 1e-9;
    VCHECKF(failed.value("error", "") == "no_receiver" && dt >= 4.9 && dt <= 5.1, "failed after %.3f s", dt);
    VCHECK(a.report()["camera"]["open"] == 0);
  }
}

// recovery: 1 % iid loss, dropped AUs, backpressure and the hard cap; never an incorrect kept picture
void test_video_e2e_recovery() {
  VProc a("", ""), b("", "");
  VCHECK(vopen_pair(a, b, "0000d004"));
  size_t ca = a.cursor();
  VCHECK(a.call(send_video("0000d004", 504))["ev"] == "ok");
  VCHECK(b.call(recv_video("0000d004", 504))["ev"] == "ok");
  VCHECK(a.wait(&ca, is_ev("stream_state", "state", "running"), 3000));
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  // (1) 1 % iid loss for 8 s: NACK repairs most, refreshes the rest, no IDR after the start
  VCHECK(a.call(Json{{"cmd", "dev"}, {"video_loss", {{"pct", 1}, {"burst", 0}}}})["ev"] == "ok");
  std::this_thread::sleep_for(std::chrono::milliseconds(8000));
  VCHECK(a.call(Json{{"cmd", "dev"}, {"video_loss", {{"pct", 0}}}})["ev"] == "ok");
  std::this_thread::sleep_for(std::chrono::milliseconds(800));
  Json ra = a.report(), rb = b.report();
  Json sb = b.last_stats(504);
  VCHECKF(ra["enc"]["idr"] == 1, "IDRs %s", ra["enc"]["idr"].dump().c_str());
  VCHECK(rb["dec"]["incorrect_kept"] == 0);
  std::printf("    1%% loss: enc %s; B %s\n", ra["enc"].dump().c_str(), sb.dump().substr(0, 200).c_str());
  // (2) dropped AUs after numbering: holes, recovered by refresh
  const int ref0 = ra["enc"]["refresh"].get<int>();
  for (int i = 0; i < 3; ++i) {
    VCHECK(a.call(Json{{"cmd", "dev"}, {"video_drop_au", 1}})["ev"] == "ok");
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  }
  ra = a.report();
  rb = b.report();
  VCHECKF(ra["enc"]["refresh"].get<int>() >= ref0 + 3 && ra["enc"]["idr"] == 1, "drop_au: %s", ra["enc"].dump().c_str());
  VCHECK(rb["dec"]["incorrect_kept"] == 0);
  Json sb2 = b.last_stats(504);
  VCHECK(sb2.value("holes", 0) >= 3);
  // (3) backpressure: the sender thread stalls 600 ms; input paused at 6 AUs, 0 AUs lost
  Json sa0 = a.last_stats(504);
  VCHECK(a.call(Json{{"cmd", "dev"}, {"video_stall_ms", 600}})["ev"] == "ok");
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  Json sa1 = a.last_stats(504);
  VCHECKF(sa1.value("q_drop", 1) == 0, "q_drop %s", sa1.dump().c_str());
  ra = a.report();
  VCHECK(ra["enc"]["idr"] == 1);
  // (4) hard cap: the same stall without backpressure; drops after numbering -> holes -> refresh
  VCHECK(a.call(Json{{"cmd", "dev"}, {"video_stall_nopause_ms", 900}})["ev"] == "ok");
  std::this_thread::sleep_for(std::chrono::milliseconds(2500));
  Json sa2 = a.last_stats(504);
  ra = a.report();
  rb = b.report();
  VCHECKF(sa2.value("q_drop", 0) >= 1, "hard cap q_drop %s", sa2.dump().c_str());
  VCHECK(rb["dec"]["incorrect_kept"] == 0 && rb["sink"]["incorrect_presented"] == 0);
  Json sb3 = b.last_stats(504);
  VCHECK(sb3.value("shown", "") == "live");
  std::printf("    after drop_au/backpressure/hard cap: enc %s, B shown %s, q_drop %s\n", ra["enc"].dump().c_str(),
              sb3.value("shown", "?").c_str(), sa2["q_drop"].dump().c_str());
  (void)sa0;
}

// a display size above the limits: 3 size PLIs, then failed decoder + the error placeholder, never a 4th
void test_video_e2e_size_rejected() {
  VProc a("", ""), b("", "");
  VCHECK(vopen_pair(a, b, "0000d005"));
  size_t cb = b.cursor();
  Json big = vcodec(2560, 1440, 8000000);
  VCHECK(a.call(send_video("0000d005", 505, big))["ev"] == "ok");
  VCHECK(b.call(recv_video("0000d005", 505, big))["ev"] == "ok");
  Json failed;
  VCHECK(b.wait(&cb, is_ev("stream_state", "state", "failed"), 6000, &failed));
  VCHECKF(failed.value("error", "") == "decoder" && failed.value("detail", "") == "size_rejected 2560x1440", "%s",
          failed.dump().c_str());
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  Json rb = b.report();
  VCHECKF(rb["dec"]["size_rejected"] == 3, "size_rejected %s", rb["dec"]["size_rejected"].dump().c_str());
  bool error_shown = false;
  for (auto& p : rb["sink"]["placeholders"]) error_shown = error_shown || p == "error";
  VCHECK(error_shown && rb["sink"]["presented"] == 0);
}

// link state: video only in frame-tagged link_out events, feedback over the link, refreshes answer PLIs, and a
// stream opened in link state (no pong: provisional offset) plays within D
void test_video_e2e_link_state() {
  FlowTiming ft;
  VProc a("", "", ft), b("", "", ft);
  VCHECK(vopen_pair(a, b, "0000d006"));
  VRelay relay(a, b);
  size_t ca = a.cursor();
  // cut UDP first: the stream starts in link state
  VCHECK(a.call(Json{{"cmd", "dev"}, {"drop_udp", true}})["ev"] == "ok");
  VCHECK(b.call(Json{{"cmd", "dev"}, {"drop_udp", true}})["ev"] == "ok");
  VCHECK(a.wait(&ca, is_ev("flow_state", "state", "link"), 4000));
  const int64_t t_send = now_ns();
  VCHECK(a.call(send_video("0000d006", 506))["ev"] == "ok");
  VCHECK(b.call(recv_video("0000d006", 506))["ev"] == "ok");
  VCHECK(a.wait(&ca, is_ev("stream_state", "state", "running"), 4000));
  std::this_thread::sleep_for(std::chrono::milliseconds(2500));
  Json rb = b.report();
  const double first_ms = double(rb["sink"].value("first_present_ns", int64_t(0)) - t_send) * 1e-6;
  VCHECKF(first_ms > 0 && first_ms <= 2000, "first frame %.0f ms after send in link state", first_ms);
  Json sb = b.last_stats(506);
  VCHECKF(sb.value("path", "") == "link" && sb["offset_ms"].is_number() && sb.value("target_ms", 0.0) >= 50 &&
              sb.value("target_ms", 999.0) <= 150,
          "%s", sb.dump().c_str());
  // 1 % loss on the link side: PLIs travel over the link, refreshes answer them
  VCHECK(a.call(Json{{"cmd", "dev"}, {"video_loss", {{"pct", 1}}}})["ev"] == "ok");
  std::this_thread::sleep_for(std::chrono::milliseconds(6000));
  Json sa = a.last_stats(506);
  Json ra = a.report();
  rb = b.report();
  VCHECKF(sa.value("path", "") == "link" && sa.value("kbps", 9e9) <= 1100, "%s", sa.dump().c_str());
  VCHECK(ra["enc"]["idr"] == 1 && rb["dec"]["incorrect_kept"] == 0);
  VCHECK(relay.video_events.load() > 50 && relay.bad_video_events.load() == 0 && relay.feedback_events.load() > 20);
  VCHECK(sa.value("rr_rx", 0) > 20);
  std::printf("    link: %llu video events, %llu feedback events, A %s, enc %s\n", (unsigned long long)relay.video_events.load(),
              (unsigned long long)relay.feedback_events.load(), sa.dump().substr(0, 160).c_str(), ra["enc"].dump().c_str());
  // back to UDP
  VCHECK(a.call(Json{{"cmd", "dev"}, {"drop_udp", false}, {"video_loss", {{"pct", 0}}}})["ev"] == "ok");
  VCHECK(b.call(Json{{"cmd", "dev"}, {"drop_udp", false}})["ev"] == "ok");
  VCHECK(a.wait(&ca, is_ev("flow_state", "state", "up"), 5000));
  std::this_thread::sleep_for(std::chrono::milliseconds(800));
  VCHECK(a.last_stats(506).value("path", "") == "udp");
  VCHECK(b.report()["dec"]["incorrect_kept"] == 0);
}

// time map: only covered streams; the first entry twice 20 ms apart; audio loss unchanged with sr:true
void test_video_time_map() {
  for (bool sr : {false, true}) {
    VProc a("mic=sine:440", ""), b("", "");
    VCHECK(vopen_pair(a, b, "0000d007"));
    VCHECK(a.call(Json{{"cmd", "dev"}, {"log_time_map", true}})["ev"] == "ok");
    VCHECK(b.call(Json{{"cmd", "dev"}, {"log_time_map", true}})["ev"] == "ok");
    Json mc = CodecConfig::mic_default().to_json();
    size_t ca = a.cursor();
    VCHECK(a.call(Json{{"cmd", "send"}, {"stream", 11}, {"flow", "0000d007"}, {"kind", "mic"}, {"source", {{"device", nullptr}}},
                       {"codec", mc}, {"key", vkey(1)}, {"sr", sr}})["ev"] == "ok");
    VCHECK(b.call(Json{{"cmd", "recv"}, {"stream", 11}, {"flow", "0000d007"}, {"kind", "mic"}, {"codec", mc}, {"key", vkey(1)},
                       {"sink", {{"virtual_mic", true}}}})["ev"] == "ok");
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    if (!sr) {
      // no covered stream: no time map at all (an old peer never sees one)
      VCHECK(!a.wait(&ca, is_ev("time_map"), 10));
    }
    VCHECK(a.call(send_video("0000d007", 507))["ev"] == "ok");
    VCHECK(b.call(recv_video("0000d007", 507))["ev"] == "ok");
    std::this_thread::sleep_for(std::chrono::milliseconds(3500));
    auto evs = a.events_since(ca);
    std::vector<int64_t> tx;
    bool only_covered = true, mic_seen = false;
    for (auto& e : evs)
      if (e.value("ev", "") == "time_map" && e.value("dir", "") == "tx") {
        tx.push_back(e["t_ns"].get<int64_t>());
        for (auto& x : e["entries"]) {
          const uint32_t s = x["stream"];
          only_covered = only_covered && (s == 507 || (sr && s == 11));
          mic_seen = mic_seen || s == 11;
        }
      }
    VCHECK(only_covered && mic_seen == sr);
    // first entries: two within ~20 ms of each other at the start of the covered stream, then every 1 s
    bool twice = false;
    for (size_t i = 1; i < tx.size(); ++i) twice = twice || (tx[i] - tx[i - 1] >= 18 * kMs && tx[i] - tx[i - 1] <= 25 * kMs);
    VCHECKF(twice && tx.size() >= 4, "%zu time maps", tx.size());
    bool rx = false;
    for (auto& e : b.events_since(0)) rx = rx || (e.value("ev", "") == "time_map" && e.value("dir", "") == "rx");
    VCHECK(rx);
    Json m = b.last_stats(11);
    VCHECKF(m.value("dec_err", 1) == 0 && m.value("lost", 0) == 0 && m.value("late", 0) == 0, "mic with sr %d: %s", sr,
            m.dump().substr(0, 200).c_str());
    Json v = b.last_stats(507);
    VCHECK(v.value("d_samples", 0) > 20 && v.value("target_ms", 0.0) >= 50);
  }
}

// PLI/NACK storm (TSan): 20 % loss, decoder errors every 50 ms, video_drop_ms bursts; only the network thread
// seals receiver-direction packets, only the sender thread seals media; 0 incorrect kept pictures
void test_video_storm() {
  g_video_seal_thread_violations.store(0);
  VProc a("", ""), b("", "dec_err_ms=50");
  VCHECK(vopen_pair(a, b, "0000d008"));
  size_t ca = a.cursor();
  VCHECK(a.call(send_video("0000d008", 508))["ev"] == "ok");
  VCHECK(b.call(recv_video("0000d008", 508))["ev"] == "ok");
  VCHECK(a.wait(&ca, is_ev("stream_state", "state", "running"), 3000));
  VCHECK(a.call(Json{{"cmd", "dev"}, {"video_loss", {{"pct", 20}}}})["ev"] == "ok");
  for (int i = 0; i < 6; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    a.call(Json{{"cmd", "dev"}, {"video_drop_ms", 150}});
  }
  Json sb = b.last_stats(508), sa = a.last_stats(508);
  Json rb = b.report();
  VCHECK(rb["dec"]["incorrect_kept"] == 0 && rb["sink"]["incorrect_presented"] == 0);
  VCHECKF(rb["dec"]["errors"].get<int>() >= 3, "decoder errors %s", rb["dec"].dump().c_str());
  VCHECK(sa.value("pli_rx", 0) > 10 && sa.value("nack_rx", 0) > 10);
  VCHECK(g_video_seal_thread_violations.load() == 0);
  std::printf("    storm: A %s\n           B %s\n", sa.dump().substr(0, 200).c_str(), sb.dump().substr(0, 200).c_str());
}

const struct {
  const char* name;
  void (*fn)();
} kCases[] = {
    {"video_packet_formats", test_video_packet_formats},
    {"video_replay_window_1024", test_video_replay_window_1024},
    {"video_reassembly_any_order", test_video_reassembly_any_order},
    {"video_sender_recovery", test_video_sender_recovery},
    {"video_verified_ltrs", test_video_verified_ltrs},
    {"video_playout_target", test_video_playout_target},
    {"video_clock_offset", test_video_clock_offset},
    {"video_drift_30min", test_video_drift_30min},
    {"video_bootstrap_and_unseen", test_video_bootstrap_and_unseen},
    {"video_lost_vs_late", test_video_lost_vs_late},
    {"video_presenter", test_video_presenter},
    {"video_pacer_timing", test_video_pacer_timing},
    {"video_tick_latency", test_video_tick_latency},
    {"video_e2e_start", test_video_e2e_start},
    {"video_e2e_no_receiver", test_video_e2e_no_receiver},
    {"video_e2e_recovery", test_video_e2e_recovery},
    {"video_e2e_size_rejected", test_video_e2e_size_rejected},
    {"video_e2e_link_state", test_video_e2e_link_state},
    {"video_time_map", test_video_time_map},
    {"video_storm", test_video_storm},
};

}  // namespace

struct Case {
  const char* name;
  void (*fn)();
};

const Case* video_test_cases(size_t* n) {
  static std::vector<Case> v;
  if (v.empty())
    for (auto& c : kCases) v.push_back(Case{c.name, c.fn});
  *n = v.size();
  return v.data();
}
