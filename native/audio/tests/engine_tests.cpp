// SPDX-License-Identifier: GPL-3.0-only
// Engine tests (§11.2): one binary, tiny assert macros, no framework.
//   engine_tests [--vectors DIR] [filter...]
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "aec.h"
#include "codec.h"
#include "crypto.h"
#include "engine.h"
#include "flow.h"
#include "helper.h"
#include "howl.h"
#include "jitter.h"
#include "json.h"
#include "packet.h"
#include "resampler.h"
#include "spsc.h"
#include "streams.h"
#include "test_signal.h"
#include "util.h"

using namespace beamer::audio;

static constexpr double M_PI_ = 3.14159265358979323846;

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define SANITIZED 1
#endif
#ifndef SANITIZED
#define SANITIZED 0
#endif

static int g_checks = 0, g_failures = 0;
static std::string g_vectors = "native/audio/tests/vectors";
static std::string g_golden = "native/audio/tests/golden_control.txt";

#define CHECK(c)                                                              \
  do {                                                                        \
    g_checks++;                                                               \
    if (!(c)) {                                                               \
      g_failures++;                                                           \
      std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
    }                                                                         \
  } while (0)
#define CHECKF(c, ...)                                                        \
  do {                                                                        \
    g_checks++;                                                               \
    if (!(c)) {                                                               \
      g_failures++;                                                           \
      std::fprintf(stderr, "  FAIL %s:%d: %s: ", __FILE__, __LINE__, #c);     \
      std::fprintf(stderr, __VA_ARGS__);                                      \
      std::fprintf(stderr, "\n");                                             \
    }                                                                         \
  } while (0)

static std::vector<uint8_t> hex(const std::string& s) {
  std::vector<uint8_t> v;
  if (s != "-") hex_decode(s, &v);
  return v;
}

// ===================================================================== crypto

static void test_aead_rfc8439() {
  std::vector<uint8_t> key(32);
  for (int i = 0; i < 32; ++i) key[i] = uint8_t(0x80 + i);
  auto nonce = hex("070000004041424344454647");
  auto aad = hex("50515253c0c1c2c3c4c5c6c7");
  const char* pt =
      "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, "
      "sunscreen would be it.";
  auto ct = hex(
      "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da92728b"
      "1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
      "3ff4def08e4b7a9de576d26586cec64b6116");
  auto tag = hex("1ae10b594f09e26a7e902ecbd0600691");
  size_t n = std::strlen(pt);
  std::vector<uint8_t> out(n + 16);
  aead_seal(key.data(), nonce.data(), aad.data(), aad.size(), reinterpret_cast<const uint8_t*>(pt), n, out.data());
  CHECK(std::equal(ct.begin(), ct.end(), out.begin()));
  CHECK(std::equal(tag.begin(), tag.end(), out.begin() + n));
  std::vector<uint8_t> back(n);
  CHECK(aead_open(key.data(), nonce.data(), aad.data(), aad.size(), out.data(), out.size(), back.data()));
  CHECK(std::memcmp(back.data(), pt, n) == 0);
  out[5] ^= 1;
  CHECK(!aead_open(key.data(), nonce.data(), aad.data(), aad.size(), out.data(), out.size(), back.data()));
}

static void test_aead_pynacl_vectors() {
  std::ifstream f(g_vectors + "/aead.txt");
  CHECKF(f.good(), "cannot open %s/aead.txt", g_vectors.c_str());
  std::string line;
  int n = 0;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream is(line);
    std::string k, no, a, p, c;
    is >> k >> no >> a >> p >> c;
    auto key = hex(k), nonce = hex(no), aad = hex(a), pt = hex(p), ct = hex(c);
    std::vector<uint8_t> out(pt.size() + 16);
    aead_seal(key.data(), nonce.data(), aad.data(), aad.size(), pt.data(), pt.size(), out.data());
    CHECKF(out == ct, "vector %d seal", n);
    std::vector<uint8_t> back(pt.size() + 1);
    CHECKF(aead_open(key.data(), nonce.data(), aad.data(), aad.size(), ct.data(), ct.size(), back.data()), "vector %d open", n);
    CHECKF(std::equal(pt.begin(), pt.end(), back.begin()), "vector %d plaintext", n);
    n++;
  }
  CHECKF(n >= 20, "only %d vectors", n);
}

static void test_packet_replay_tamper() {
  uint8_t key[32], other[32];
  random_bytes(key, 32);
  random_bytes(other, 32);
  const uint32_t sid = 0x12345678;
  Sealer s(key, sid);
  std::vector<std::vector<uint8_t>> pk;
  for (int i = 0; i < 200; ++i) {
    Header h;
    h.ptype = kMedia;
    h.flags = kFlagFec;
    h.codec = kCodecOpus;
    h.stream = sid;
    h.ts = 1000u + 480u * i;
    h.nsamp = 480;
    h.channels = 1;
    uint8_t payload[40];
    std::memset(payload, i, sizeof payload);
    std::vector<uint8_t> b(kMaxPacket);
    b.resize(s.seal(h, payload, sizeof payload, b.data()));
    pk.push_back(b);
  }
  // header layout (big endian)
  CHECK(pk[0].size() == 24 + 40 + 16);
  CHECK(pk[0][0] == 1 && pk[0][1] == kMedia && pk[0][2] == kFlagFec && pk[0][3] == kCodecOpus);
  CHECK(get_u32(&pk[0][4]) == sid && get_u64(&pk[0][8]) == 1 && get_u32(&pk[0][16]) == 1000);
  CHECK(get_u16(&pk[0][20]) == 480 && pk[0][22] == 1 && pk[0][23] == 0);
  Header rh;
  CHECK(read_header(pk[3].data(), pk[3].size(), &rh) && rh.seq == 4 && rh.ts == 1000u + 3 * 480);

  ReplayWindow w;
  Header h;
  uint8_t out[kMaxPacket];
  size_t n;
  auto open = [&](const std::vector<uint8_t>& p, const uint8_t* k, uint32_t dir) {
    return open_packet(k, dir, &w, p.data(), p.size(), &h, out, &n);
  };
  CHECK(open(pk[0], key, sid) == OpenResult::Ok && n == 40 && out[0] == 0);
  CHECK(open(pk[2], key, sid) == OpenResult::Ok);
  CHECK(open(pk[1], key, sid) == OpenResult::Ok);        // reordered within the window
  CHECK(open(pk[1], key, sid) == OpenResult::Replayed);  // duplicate
  CHECK(open(pk[150], key, sid) == OpenResult::Ok);
  CHECK(open(pk[10], key, sid) == OpenResult::Replayed);  // older than 128
  CHECK(open(pk[30], key, sid) == OpenResult::Ok);        // 150 - 30 = 120 < 128, unseen
  CHECK(open(pk[30], key, sid) == OpenResult::Replayed);
  auto t = pk[160];
  t[30] ^= 0x40;  // payload
  CHECK(open(t, key, sid) == OpenResult::Rejected);
  t = pk[160];
  t[17] ^= 1;  // header (ts) is associated data
  CHECK(open(t, key, sid) == OpenResult::Rejected);
  CHECK(open(pk[160], other, sid) == OpenResult::Rejected);         // wrong key
  CHECK(open(pk[160], key, sid | kDirBit) == OpenResult::Rejected); // wrong direction
  CHECK(open(pk[160], key, sid) == OpenResult::Ok);                 // failures did not burn the seq
  t = pk[161];
  t[0] = 2;  // version
  CHECK(open(t, key, sid) == OpenResult::Rejected);
  CHECK(open(std::vector<uint8_t>(pk[161].begin(), pk[161].begin() + 30), key, sid) == OpenResult::Rejected);
  // window shifts by more than 64 and 128
  ReplayWindow w2;
  w2.mark(1);
  w2.mark(70);
  CHECK(!w2.fresh(1) && w2.fresh(2) && !w2.fresh(70) && w2.fresh(69));
  w2.mark(400);
  CHECK(w2.fresh(399) && !w2.fresh(272) && w2.fresh(273) && !w2.fresh(70));
}

static void test_base64() {
  for (int n = 0; n < 40; ++n) {
    std::vector<uint8_t> v(n), back;
    random_bytes(v.data(), n);
    std::string s = b64_encode(v);
    CHECK(s.size() == size_t((n + 2) / 3 * 4));
    CHECK(b64_decode(s, &back) && back == v);
  }
  std::vector<uint8_t> b;
  CHECK(b64_decode("Zm9vYmFy", &b) && std::string(b.begin(), b.end()) == "foobar");
  CHECK(!b64_decode("Zm9v!mFy", &b));
}

// ===================================================================== codec

static std::vector<float> voice(int64_t s0, int n) {  // sig.py-like voiced stand-in (12 harmonics)
  std::vector<float> x(n);
  for (int i = 0; i < n; ++i) {
    double t = double(s0 + i) / kRate, env = 0.55 + 0.45 * std::sin(2 * M_PI_ * 3 * t), v = 0;
    for (int k = 1; k <= 12; ++k) v += std::sin(2 * M_PI_ * k * 180.0 * t) / k;
    x[i] = static_cast<float>(0.12 * env * v);
  }
  return x;
}

static double xcorr_snr(const std::vector<float>& ref, const std::vector<float>& got, int delay, int from, int to) {
  double s = 0, e = 0;
  for (int i = from; i < to; ++i) {
    double r = ref[i - delay], d = got[i] - r;
    s += r * r;
    e += d * d;
  }
  return 10 * std::log10(s / std::max(e, 1e-20));
}

static void test_opus_roundtrip() {
  // mono VOIP 32k FEC, 10 ms
  CodecConfig mic = CodecConfig::mic_default();
  OpusEnc enc;
  OpusDec dec, dec2;
  std::string err;
  CHECK(enc.init(mic, &err));
  CHECK(dec.init(1, &err) && dec2.init(1, &err));
  const int F = mic.frame(), N = 300;
  std::vector<float> ref = voice(0, F * N), out(F * N), out2(F * N);
  std::vector<std::vector<uint8_t>> pk;
  int lbrr = 0;
  for (int k = 0; k < N; ++k) {
    uint8_t b[1400];
    int n = enc.encode(&ref[size_t(k) * F], b, sizeof b);
    CHECK(n > 0 && n < 200);
    pk.emplace_back(b, b + n);
    if (opus_has_lbrr(b, n)) lbrr++;
    CHECK(dec.decode(b, n, &out[size_t(k) * F], 5760) == F);
  }
  CHECKF(lbrr > N / 2, "lbrr %d/%d", lbrr, N);
  const int delay = 312;  // Opus lookahead (6.5 ms)
  double best = -100;
  int bestd = 0;
  for (int d = 0; d <= 1200; ++d) {
    double v = xcorr_snr(ref, out, d, F * 50, F * N);
    if (v > best) {
      best = v;
      bestd = d;
    }
  }
  std::printf("    mono voip 32k: waveform snr %.1f dB at delay %d\n", best, bestd);
  double er = 0, eo = 0;
  for (int i = F * 50; i < F * N; ++i) {
    er += double(ref[i - bestd]) * ref[i - bestd];
    eo += double(out[i]) * out[i];
  }
  double gain_db = 10 * std::log10(eo / er);
  // SILK is not waveform-exact: require alignment at the 6.5 ms lookahead, level, and > 3 dB
  CHECKF(best > 3 && std::abs(bestd - delay) <= 16 && std::fabs(gain_db) < 1.5, "mono voip snr %.1f dB delay %d gain %.1f dB",
         best, bestd, gain_db);
  // FEC recovers a single loss: frame 150 lost, recovered from 151's LBRR
  for (int k = 0; k < N; ++k) {
    float* o = &out2[size_t(k) * F];
    if (k == 150) CHECK(dec2.decode_fec(pk[151].data(), int(pk[151].size()), o, F) == F);
    else CHECK(dec2.decode(pk[k].data(), int(pk[k].size()), o, 5760) == F);
  }
  double snr_fec = xcorr_snr(out, out2, 0, F * 150, F * 151);
  double e = 0;
  for (int i = F * 150; i < F * 151; ++i) e += double(out2[i]) * out2[i];
  CHECKF(e > 0.1 * 480 * 0.01 && snr_fec > 3, "fec frame snr %.1f dB energy %.3f", snr_fec, e);
  // PLC produces audio of the right length
  float plc[480];
  CHECK(dec2.plc(plc, 480) == 480);

  // stereo AUDIO 96k
  CodecConfig sys = CodecConfig::system_default();
  OpusEnc es;
  OpusDec ds;
  CHECK(es.init(sys, &err) && ds.init(2, &err));
  std::vector<float> st(size_t(F) * N * 2), so(size_t(F) * N * 2);
  for (int i = 0; i < F * N; ++i) {
    st[2 * i] = static_cast<float>(0.3 * std::sin(2 * M_PI_ * 440 * i / kRate));
    st[2 * i + 1] = static_cast<float>(0.3 * std::sin(2 * M_PI_ * 1000 * i / kRate));
  }
  for (int k = 0; k < N; ++k) {
    uint8_t b[1400];
    int n = es.encode(&st[size_t(k) * F * 2], b, sizeof b);
    CHECK(n > 0);
    CHECK(ds.decode(b, n, &so[size_t(k) * F * 2], 5760) == F);
  }
  std::vector<float> l(F * N), r(F * N), lo(F * N), ro(F * N);
  for (int i = 0; i < F * N; ++i) {
    l[i] = st[2 * i];
    r[i] = st[2 * i + 1];
    lo[i] = so[2 * i];
    ro[i] = so[2 * i + 1];
  }
  double bl = -100, br = -100;
  for (int d = delay - 8; d <= delay + 8; ++d) {
    bl = std::max(bl, xcorr_snr(l, lo, d, F * 50, F * N));
    br = std::max(br, xcorr_snr(r, ro, d, F * 50, F * N));
  }
  CHECKF(bl > 20 && br > 20, "stereo snr L %.1f R %.1f", bl, br);
  CodecConfig bad;
  Json j = {{"name", "pcm"}};
  CHECK(!CodecConfig::parse(j, &bad, &err));
}

// ===================================================================== resampler

static void test_resampler_snr() {
  for (double ppm : {-500.0, -100.0, 0.0, 100.0, 500.0}) {
    Resampler rs(1, 4096);
    const double f = 1000.0, step = 1.0 + ppm * 1e-6;
    int64_t fed = 0;
    double sig = 0, err = 0;
    std::vector<float> in(480), out(480);
    for (int blk = 0; blk < 400; ++blk) {
      while (rs.need(480, step) > 0) {
        for (int i = 0; i < 480; ++i) in[i] = static_cast<float>(0.5 * std::sin(2 * M_PI_ * f * double(fed + i) / kRate));
        rs.feed(in.data(), 480);
        fed += 480;
      }
      double p0 = rs.abs_pos();
      rs.out(out.data(), 480, step);
      if (blk < 20) continue;
      for (int i = 0; i < 480; ++i) {
        double p = p0 + i * step;
        double ideal = 0.5 * std::sin(2 * M_PI_ * f * p / kRate);
        sig += ideal * ideal;
        err += (out[i] - ideal) * (out[i] - ideal);
      }
    }
    double snr = 10 * std::log10(sig / err);
    CHECKF(snr >= 80.0, "resampler snr %.1f dB at %+.0f ppm", snr, ppm);
  }
}

static void test_signal_pilot_level() {
  TestSignal a, a2;
  CHECK(TestSignal::parse("A", &a) && TestSignal::parse("A:0.2", &a2));
  TestSignal bad;
  CHECK(!TestSignal::parse("A:0.9", &bad));
  std::vector<float> x(480), y(480);  // 10 ms before the first marker (2 s): pilot only
  a.render(0, 480, x.data());
  a2.render(0, 480, y.data());
  float px = 0, py = 0;
  for (int i = 0; i < 480; ++i) {
    px = std::max(px, std::fabs(x[i]));
    py = std::max(py, std::fabs(y[i]));
  }
  CHECK(std::fabs(px - 0.02f) < 1e-3f && std::fabs(py - 0.2f) < 1e-3f);
}

static void test_map_channels() {
  float st[6] = {1, 3, 2, 4, -1, 1}, mono[3], back[6], eight[24];
  map_channels(st, 2, mono, 1, 3);
  CHECK(mono[0] == 2 && mono[1] == 3 && mono[2] == 0);
  map_channels(mono, 1, back, 2, 3);
  CHECK(back[0] == 2 && back[1] == 2 && back[5] == 0);
  map_channels(st, 2, eight, 8, 3);
  CHECK(eight[0] == 1 && eight[1] == 3 && eight[2] == 0 && eight[8] == 2);
}

// ===================================================================== jitter buffer simulation

struct Encoded {
  std::vector<std::vector<uint8_t>> frames;
};
static const Encoded& encoded_voice() {
  static Encoded e = [] {
    Encoded r;
    OpusEnc enc;
    std::string err;
    enc.init(CodecConfig::mic_default(), &err);
    auto x = voice(0, 480 * 1000);
    for (int k = 0; k < 1000; ++k) {
      uint8_t b[1400];
      int n = enc.encode(&x[size_t(k) * 480], b, sizeof b);
      r.frames.emplace_back(b, b + n);
    }
    return r;
  }();
  return e;
}

struct SimSpec {
  double seconds = 60;
  double ppm = 0;          // sender clock relative to the receiver's
  double loss = 0;         // Bernoulli
  double burst = 1;        // Gilbert mean burst length (> 1)
  double jitter_ms = 0;    // uniform 0..J
  double stall_at = -1, stall_s = 0;
  double spike_every_s = 0, spike_ms = 0;  // periodic transit spikes: held, then a burst
  double step_at = -1, step_ms = 0;        // the path gets slower for good
  double episode_at = -1, episode_ms = 0, episode_s = 0;  // slower at once, then back over episode_s
  int block = 128;         // render period (samples)
  uint64_t seed = 1;
};
struct SimOut {
  std::vector<double> sec_level, sec_target, sec_max, sec_integ, sec_est;
  JbCounters c;
  uint64_t sent = 0;
  double wall_s = 0;
};

static SimOut simulate(const SimSpec& sp) {
  auto t0 = std::chrono::steady_clock::now();
  const Encoded& enc = encoded_voice();
  JbConfig cfg;
  if (std::getenv("BA_SIM_REFERENCE")) {  // exactly media.py
    cfg.merge_expand = false;
    cfg.jitter_recalc_ms = 0;
    cfg.drift_est = false;
    cfg.period_floor_ms = 0;
  }
  if (std::getenv("BA_SIM_NO_DRIFT_EST")) cfg.drift_est = false;
  JitterBuffer jb(cfg);
  std::string err;
  jb.init(&err);
  std::mt19937_64 rng(sp.seed);
  std::uniform_real_distribution<double> uni(0, 1);
  struct Arr {
    double t;
    uint32_t k;
  };
  std::vector<Arr> arr;
  const double fdur = 0.010 / (1.0 + sp.ppm * 1e-6);
  const uint32_t nframes = static_cast<uint32_t>(sp.seconds / fdur) + 50;
  bool in_loss = false;
  for (uint32_t k = 0; k < nframes; ++k) {
    double ts = 0.5 + k * fdur;  // send time
    bool lost;
    if (sp.burst > 1) {
      double p_exit = 1.0 / sp.burst, p_enter = sp.loss * p_exit / (1 - sp.loss);
      in_loss = in_loss ? uni(rng) >= p_exit : uni(rng) < p_enter;
      lost = in_loss;
    } else {
      lost = uni(rng) < sp.loss;
    }
    if (lost) continue;
    double a = ts + 0.003 + sp.jitter_ms * 1e-3 * uni(rng);
    if (sp.stall_at >= 0 && ts >= sp.stall_at && ts < sp.stall_at + sp.stall_s)
      a = std::max(a, sp.stall_at + sp.stall_s + 0.001 * uni(rng));  // held, then a burst
    if (sp.step_at >= 0 && ts >= sp.step_at) a += sp.step_ms * 1e-3;
    if (sp.episode_at >= 0 && ts >= sp.episode_at && ts < sp.episode_at + sp.episode_s)
      a += sp.episode_ms * 1e-3 * (1.0 - (ts - sp.episode_at) / sp.episode_s);
    if (sp.spike_every_s > 0) {
      double k0 = std::floor(ts / sp.spike_every_s) * sp.spike_every_s + sp.spike_every_s / 2;
      if (ts >= k0 && ts < k0 + sp.spike_ms * 1e-3) a = std::max(a, k0 + sp.spike_ms * 1e-3 + 0.001 * uni(rng));
    }
    arr.push_back({a, k});
  }
  std::sort(arr.begin(), arr.end(), [](const Arr& x, const Arr& y) { return x.t < y.t; });
  // BA_SIM_DUMP=prefix: arrivals and per-pull level/target, for a differential run against
  // proto/transport/media.py (tests/jb_crosscheck.py)
  FILE* dump_pulls = nullptr;
  if (const char* pfx = std::getenv("BA_SIM_DUMP")) {
    FILE* fa = std::fopen((std::string(pfx) + ".arrivals").c_str(), "w");
    std::fprintf(fa, "%d %.9f\n", sp.block, sp.seconds);
    for (auto& a : arr) std::fprintf(fa, "%.9f %u\n", a.t, a.k);
    std::fclose(fa);
    dump_pulls = std::fopen((std::string(pfx) + ".pulls").c_str(), "w");
  }
  SimOut out;
  out.sent = nframes;
  std::vector<float> buf(size_t(sp.block) * 1);
  size_t ai = 0;
  MediaFrame mf;
  const uint32_t ts0 = 0xFFF00000u;  // wraps during the run
  double acc_l = 0, acc_t = 0, mx = 0;
  int acc_n = 0, sec = 0;
  for (int64_t pull = 0;; ++pull) {
    double now = pull * double(sp.block) / kRate;
    if (now > sp.seconds) break;
    while (ai < arr.size() && arr[ai].t <= now) {
      const auto& fr = enc.frames[arr[ai].k % enc.frames.size()];
      mf.arrival_ns = static_cast<int64_t>(arr[ai].t * 1e9);
      mf.seq = arr[ai].k + 1;
      mf.ts = ts0 + arr[ai].k * 480u;
      mf.nsamp = 480;
      mf.flags = kFlagFec;
      mf.channels = 1;
      mf.len = static_cast<uint16_t>(fr.size());
      std::memcpy(mf.data, fr.data(), fr.size());
      jb.push(mf);
      ai++;
    }
    jb.pull(buf.data(), sp.block, now);
    if (dump_pulls) std::fprintf(dump_pulls, "%.6f %.4f %.4f\n", now, jb.last_level_ms(), jb.target_ms());
    acc_l += jb.last_level_ms();
    acc_t += jb.target_ms();
    mx = std::max(mx, jb.last_level_ms());
    acc_n++;
    if (int(now) != sec) {
      out.sec_level.push_back(acc_l / acc_n);
      out.sec_target.push_back(acc_t / acc_n);
      out.sec_max.push_back(mx);
      out.sec_integ.push_back(jb.integ_ppm());
      out.sec_est.push_back(jb.drift_est_ppm());
      acc_l = acc_t = mx = 0;
      acc_n = 0;
      sec = int(now);
    }
  }
  if (dump_pulls) std::fclose(dump_pulls);
  out.c = jb.counters();
  out.wall_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return out;
}

// "Level within target +-10 ms": the mean of (level - target) over 10 s windows after from_s.
// (Per second the reference design itself leaves the level ~10 ms above target for ~5 s after each
// expansion, which catch-up resampling at <= 2000 ppm removes; tests/jb_crosscheck.py shows the
// engine reproduces media.py exactly on such traces.)
static void check_tracking(const SimOut& o, const char* name, double from_s, double tol_ms,
                           double skip_from = -1, double skip_to = -1) {
  int bad = 0;
  double worst = 0, maxlv = 0;
  for (size_t w = static_cast<size_t>(from_s); w + 10 <= o.sec_level.size(); w += 10) {
    if (skip_to >= 0 && w + 10 > skip_from && w <= skip_to) continue;
    double d = 0;
    for (size_t s = w; s < w + 10; ++s) {
      d += o.sec_level[s] - o.sec_target[s];
      maxlv = std::max(maxlv, o.sec_max[s]);
    }
    d = std::fabs(d / 10);
    worst = std::max(worst, d);
    if (d > tol_ms) {
      bad++;
      if (std::getenv("BA_SIM_TRACE")) std::printf("      window t=%zu..%zu |level-target| %.1f\n", w, w + 10, d);
    }
  }
  CHECKF(bad == 0, "%s: %d windows of 10 s off target by > %.0f ms (worst %.1f)", name, bad, tol_ms, worst);
  CHECKF(maxlv < 200, "%s: level max %.1f ms (unbounded?)", name, maxlv);
  CHECKF(o.c.dec_err == 0, "%s: %llu decode errors", name, (unsigned long long)o.c.dec_err);
  CHECKF(o.c.skipped == 0, "%s: %llu frames skipped without decoding", name, (unsigned long long)o.c.skipped);
  std::printf("    %-22s level %.1f ms target %.1f ms (last s), worst 10 s |err| %.1f, max %.1f; ok %llu fec %llu plc %llu "
              "expand %llu merged %llu drop %llu flushed %llu late %llu; %.2f s wall\n",
              name, o.sec_level.back(), o.sec_target.back(), worst, maxlv, (unsigned long long)o.c.ok,
              (unsigned long long)o.c.fec, (unsigned long long)o.c.plc, (unsigned long long)o.c.expand,
              (unsigned long long)o.c.merged, (unsigned long long)o.c.drop, (unsigned long long)o.c.flushed, (unsigned long long)o.c.late, o.wall_s);
}

static void test_jb_loss_jitter_drift_10min() {
  SimSpec sp;
  sp.seconds = 600;
  sp.ppm = 100;
  sp.loss = 0.02;
  sp.jitter_ms = 20;
  SimOut o = simulate(sp);
  check_tracking(o, "2% loss +100ppm 0-20ms", 30, 10);
  CHECK(o.c.fec > 0);
  CHECKF(std::fabs(o.sec_est.back() - 100) < 5 && std::fabs(o.sec_integ.back() - 100) < 5,
         "clock estimate %.1f / integral %.1f ppm for +100", o.sec_est.back(), o.sec_integ.back());
  CHECKF(SANITIZED || o.wall_s < 10.0, "10 min simulated in %.1f s", o.wall_s);
}

static void test_jb_gilbert_negative_drift() {
  SimSpec sp;
  sp.seconds = 180;
  sp.ppm = -100;
  sp.loss = 0.02;
  sp.burst = 3;
  sp.jitter_ms = 10;
  sp.seed = 7;
  SimOut o = simulate(sp);
  // Bursts at the head of the buffer read as "late" -> expand. With merge_expand off (exactly
  // media.py, tests/jb_crosscheck.py) each such loss adds a frame of level and the buffer sits
  // 10-20 ms above target here; with it, the expansion is the loss's concealment.
  check_tracking(o, "Gilbert 2%/3 -100ppm", 30, 10);
  CHECKF(std::fabs(o.sec_est.back() + 100) < 10, "clock estimate %.1f ppm for -100", o.sec_est.back());
  CHECK(o.c.merged > 0);
  CHECK(o.c.plc > 0);
}

static void test_jb_stall_flush() {
  SimSpec sp;
  sp.seconds = 90;
  sp.jitter_ms = 5;
  sp.stall_at = 40;
  sp.stall_s = 0.7;
  SimOut o = simulate(sp);
  check_tracking(o, "0.7 s stall", 30, 10, 39, 45);
  CHECKF(o.c.flushed > 30, "flushed %llu", (unsigned long long)o.c.flushed);
  CHECK(o.c.rebuffer >= 1);
  // back on target within 2 s of the stall's end
  CHECKF(std::fabs(o.sec_level[43] - o.sec_target[43]) < 10, "after stall %.1f vs %.1f", o.sec_level[43], o.sec_target[43]);
}

// WP11: late bursts that make the buffer run dry (an expansion, then the late frame arrives) add a
// frame of level each time. Live (WP10 soak, PC sound -> Mac) ~3.7/min pinned the drift integral at its
// +200 ppm clamp while the clocks differed by ~20 ppm. The integral must track the clock, and the
// rate estimate (stats `drift_est_ppm`) must find it from timestamps alone.
static void test_jb_drift_integral_with_expansions() {
  for (double ppm : {20.0, -60.0}) {
    SimSpec sp;
    sp.seconds = 600;
    sp.ppm = ppm;
    sp.jitter_ms = 3;
    sp.spike_every_s = 20;
    sp.spike_ms = 30;
    sp.seed = 11;
    SimOut o = simulate(sp);
    char name[64];
    std::snprintf(name, sizeof name, "spikes/20 s %+.0fppm", ppm);
    // each late burst leaves its expansions as level for several seconds (catch-up drains them):
    // the 10 ms tracking bound of the other cases does not hold here, by design and before WP11 too
    check_tracking(o, name, 60, 20);
    CHECKF(o.c.expand >= 25, "%s: %llu expansions (the case needs them)", name, (unsigned long long)o.c.expand);
    double worst_i = 0, worst_e = 0;
    for (size_t s = 120; s < o.sec_integ.size(); ++s) {
      worst_i = std::max(worst_i, std::fabs(o.sec_integ[s] - ppm));
      worst_e = std::max(worst_e, std::fabs(o.sec_est[s] - ppm));
    }
    std::printf("    %-22s integral %.1f ppm at the end (worst |integ-drift| after 120 s %.1f), estimate %.1f (worst %.1f)\n",
                name, o.sec_integ.back(), worst_i, o.sec_est.back(), worst_e);
    CHECKF(worst_i < 30, "%s: drift integral off the clock drift by %.1f ppm", name, worst_i);
    CHECKF(worst_e < 10, "%s: drift estimate off by %.1f ppm", name, worst_e);
  }
}

// A lasting step in transit (route change, link fallback, a capture restart) is not a clock ratio:
// the estimate starts over instead of bending its line through the step.
static void test_jb_drift_estimate_step() {
  SimSpec sp;
  sp.seconds = 300;
  sp.ppm = 30;
  sp.jitter_ms = 5;
  sp.step_at = 100;
  sp.step_ms = 60;
  SimOut o = simulate(sp);
  check_tracking(o, "+60 ms step at 100 s", 30, 10, 99, 110);
  double worst_e = 0, worst_i = 0;
  for (size_t s = 180; s < o.sec_est.size(); ++s) {  // valid again 60 s after the restart
    worst_e = std::max(worst_e, std::fabs(o.sec_est[s] - 30));
    worst_i = std::max(worst_i, std::fabs(o.sec_integ[s] - 30));
  }
  std::printf("    estimate after the step: worst |est-drift| %.1f, |integ-drift| %.1f ppm (est at 99 s %.1f)\n", worst_e,
              worst_i, o.sec_est[99]);
  CHECKF(worst_e < 10 && worst_i < 30, "after a transit step: estimate off by %.1f, integral by %.1f ppm", worst_e, worst_i);

  // a Wi-Fi episode: 40 ms slower at once, back over 40 s (WP11 live: a window restarted inside one
  // read -177 ppm). Twice, the second inside the 10 min window of the first.
  SimSpec ep;
  ep.seconds = 600;
  ep.ppm = 16;
  ep.jitter_ms = 5;
  ep.episode_at = 200;
  ep.episode_ms = 40;
  ep.episode_s = 40;
  SimOut e1 = simulate(ep);
  ep.episode_at = 290;
  ep.step_at = 400;
  ep.step_ms = -25;  // and a path that gets faster
  SimOut e2 = simulate(ep);
  for (const SimOut* o : {&e1, &e2}) {
    double we = 0, wi = 0;
    for (size_t s = 90; s < o->sec_est.size(); ++s) {
      we = std::max(we, std::fabs(o->sec_est[s] - 16));
      wi = std::max(wi, std::fabs(o->sec_integ[s] - 16));
    }
    std::printf("    episode: worst |est-drift| %.1f, |integ-drift| %.1f ppm after 90 s\n", we, wi);
    CHECKF(we < 10 && wi < 30, "Wi-Fi episode: estimate off by %.1f, integral by %.1f ppm", we, wi);
  }
}

static void test_jb_quiet_and_fixed() {
  // 10 ms pulls, no jitter: level settles near frame/2 + block + margin within the 10 ms floor
  SimSpec sp;
  sp.seconds = 60;
  sp.block = 480;
  SimOut o = simulate(sp);
  check_tracking(o, "10 ms pulls, clean", 20, 6);
  CHECK(o.c.expand < 5);
  CHECK(o.c.plc == 0 && o.c.lost == 0);
}

// ===================================================================== SPSC

static void test_spsc_threads() {
  SpscRing<uint64_t> ring(256);
  const uint64_t N = 2'000'000;
  std::atomic<bool> ok{true};
  std::thread prod([&] {
    for (uint64_t i = 1; i <= N;) {
      if (ring.push(i)) ++i;
      else std::this_thread::yield();
    }
  });
  uint64_t expect = 1;
  while (expect <= N) {
    uint64_t v;
    if (ring.pop(&v)) {
      if (v != expect) ok = false;
      ++expect;
    } else {
      std::this_thread::yield();
    }
  }
  prod.join();
  CHECK(ok.load());
  SpscFloatRing fr(1024, 2);
  const int M = 200000;
  std::thread p2([&] {
    float b[2 * 97];
    int i = 0;
    while (i < M) {
      int n = std::min(97, M - i);
      for (int k = 0; k < n; ++k) b[2 * k] = b[2 * k + 1] = float(i + k);
      int w = static_cast<int>(fr.write(b, n));
      i += w;
      if (w == 0) std::this_thread::yield();
    }
  });
  int got = 0;
  bool fok = true;
  float b[2 * 131];
  while (got < M) {
    int n = static_cast<int>(fr.read(b, 131));
    for (int k = 0; k < n; ++k)
      if (b[2 * k] != float(got + k) || b[2 * k + 1] != float(got + k)) fok = false;
    got += n;
    if (n == 0) std::this_thread::yield();
  }
  p2.join();
  CHECK(fok);
}

// ===================================================================== in-process helpers

class InProc {
 public:
  explicit InProc(const std::string& null_opts, FlowTiming timing = FlowTiming()) {
    backend_ = make_null_backend(null_opts);
    HelperOptions opt;
    opt.dev = true;
    opt.timing = timing;
    opt.stats_period_ms = 500;
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
    thread_ = std::thread([this, opt, io] { rc_ = run_control(*backend_, opt, io); });
  }
  ~InProc() { finish(); }
  void finish() {
    if (!thread_.joinable()) return;
    send(R"({"cmd":"quit"})");
    {
      std::lock_guard<std::mutex> lock(imu_);
      closed_ = true;
    }
    icv_.notify_all();
    thread_.join();
  }
  void send(const std::string& line) {
    {
      std::lock_guard<std::mutex> lock(imu_);
      in_.push_back(line);
    }
    icv_.notify_all();
  }
  void send(const Json& j) { send(j.dump()); }
  void send(const char* s) { send(std::string(s)); }
  size_t cursor() {
    std::lock_guard<std::mutex> lock(omu_);
    return log_.size();
  }
  // Waits for an event at index >= *from matching pred; advances *from past it.
  bool wait(size_t* from, const std::function<bool(const Json&)>& pred, int timeout_ms, Json* out = nullptr) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::unique_lock<std::mutex> lock(omu_);
    for (;;) {
      for (size_t i = *from; i < log_.size(); ++i) {
        if (pred(log_[i])) {
          *from = i + 1;
          if (out) *out = log_[i];
          return true;
        }
      }
      if (ocv_.wait_until(lock, deadline) == std::cv_status::timeout) {
        for (size_t i = *from; i < log_.size(); ++i)
          if (pred(log_[i])) {
            *from = i + 1;
            if (out) *out = log_[i];
            return true;
          }
        return false;
      }
    }
  }
  std::vector<Json> events_since(size_t from) {
    std::lock_guard<std::mutex> lock(omu_);
    return std::vector<Json>(log_.begin() + static_cast<long>(std::min(from, log_.size())), log_.end());
  }
  int rc() const { return rc_; }

 private:
  std::unique_ptr<Backend> backend_;
  std::thread thread_;
  std::mutex imu_, omu_;
  std::condition_variable icv_, ocv_;
  std::deque<std::string> in_;
  bool closed_ = false;
  std::vector<Json> log_;
  int rc_ = -1;
};

static std::function<bool(const Json&)> ev_is(const std::string& ev, const std::string& field = "",
                                              const Json& value = nullptr) {
  return [=](const Json& j) {
    if (!j.is_object() || j.value("ev", "") != ev) return false;
    if (field.empty()) return true;
    auto it = j.find(field);
    return it != j.end() && *it == value;
  };
}

// Relays link_out of one helper into link_in of the other (what Beamer does over the link).
class Relay {
 public:
  Relay(InProc& a, InProc& b) : a_(a), b_(b) {
    t_ = std::thread([this] { run(); });
  }
  ~Relay() {
    stop_ = true;
    t_.join();
  }
  std::atomic<uint64_t> moved{0};

 private:
  void run() {
    size_t ca = a_.cursor(), cb = b_.cursor();
    while (!stop_) {
      pump(a_, b_, &ca);
      pump(b_, a_, &cb);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  void pump(InProc& from, InProc& to, size_t* c) {
    auto evs = from.events_since(*c);
    *c += evs.size();
    for (auto& e : evs)
      if (e.value("ev", "") == "link_out") {
        to.send(Json{{"cmd", "link_in"}, {"flow", e["flow"]}, {"p", e["p"]}});
        moved += e["p"].size();
      }
  }
  InProc &a_, &b_;
  std::thread t_;
  std::atomic<bool> stop_{false};
};

static std::string b64key(uint8_t fill) {
  std::vector<uint8_t> k(32, fill);
  return b64_encode(k);
}

// Opens one flow A(offer) <-> B(answer) over 127.0.0.1; returns false on failure.
static bool open_pair(InProc& a, InProc& b, const std::string& flow, size_t* ca, size_t* cb) {
  Json fa;
  a.send(Json{{"cmd", "flow_open"}, {"req", 1}, {"flow", flow}, {"peer", "B"}, {"role", "offer"}, {"port", 0},
              {"host", "127.0.0.1"}, {"remote", nullptr}, {"key", b64key(7)}, {"rr_ms", 10}});
  if (!a.wait(ca, ev_is("flow_bound"), 3000, &fa)) return false;
  Json fb;
  b.send(Json{{"cmd", "flow_open"}, {"req", 1}, {"flow", flow}, {"peer", "A"}, {"role", "answer"}, {"port", 24823},
              {"remote", {{"host", "127.0.0.1"}, {"port", fa["port"]}}}, {"key", b64key(7)}, {"rr_ms", 10}});
  if (!b.wait(cb, ev_is("flow_bound"), 3000, &fb)) return false;
  a.send(Json{{"cmd", "flow_remote"}, {"req", 2}, {"flow", flow}, {"host", "127.0.0.1"}, {"port", fb["port"]}});
  return a.wait(ca, ev_is("flow_state", "state", "up"), 3000) && b.wait(cb, ev_is("flow_state", "state", "up"), 3000);
}

static Json mic_codec() { return CodecConfig::mic_default().to_json(); }

static void test_rr_timing() {
  InProc a("mic=sine:440"), b("");
  size_t ca = 0, cb = 0;
  CHECK(a.wait(&ca, ev_is("ready"), 3000) && b.wait(&cb, ev_is("ready"), 3000));
  CHECK(open_pair(a, b, "0000aaaa", &ca, &cb));
  // A sends its mic to B; B only receives -> B sends receiver reports every 10 ms
  a.send(Json{{"cmd", "send"}, {"req", 3}, {"stream", 11}, {"flow", "0000aaaa"}, {"kind", "mic"},
              {"source", {{"device", nullptr}}}, {"codec", mic_codec()}, {"key", b64key(1)}});
  b.send(Json{{"cmd", "recv"}, {"req", 3}, {"stream", 11}, {"flow", "0000aaaa"}, {"kind", "mic"},
              {"codec", mic_codec()}, {"key", b64key(1)}, {"sink", {{"virtual_mic", true}}}});
  CHECK(a.wait(&ca, ev_is("ok", "req", 3), 3000) && b.wait(&cb, ev_is("ok", "req", 3), 3000));
  auto rr_count = [&](size_t* c) -> int64_t {
    Json st;
    if (!a.wait(c, ev_is("stats"), 3000, &st)) return -1;
    auto s = st["streams"]["11"];
    return s.is_object() ? s.value("rr_rx", int64_t(-1)) : -1;
  };
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  size_t c1 = a.cursor();
  int64_t r0 = rr_count(&c1);
  std::this_thread::sleep_for(std::chrono::milliseconds(2000));
  size_t c2 = a.cursor();
  int64_t r1 = rr_count(&c2);
  double rate = (r1 - r0) / 2.0;  // stats every 0.5 s; ~2 s apart
  CHECKF(rate > 60 && rate < 140, "rr rate %.1f/s (expect ~100 with rr_ms 10)", rate);
  // B also sends now: its own media keeps the radio awake, receiver reports stop
  b.send(Json{{"cmd", "send"}, {"req", 4}, {"stream", 21}, {"flow", "0000aaaa"}, {"kind", "system"},
              {"source", {{"system", true}}}, {"key", b64key(2)}});
  a.send(Json{{"cmd", "recv"}, {"req", 4}, {"stream", 21}, {"flow", "0000aaaa"}, {"kind", "system"},
              {"key", b64key(2)}, {"sink", {{"device", nullptr}}}});
  CHECK(b.wait(&cb, ev_is("ok", "req", 4), 3000) && a.wait(&ca, ev_is("ok", "req", 4), 3000));
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  size_t c3 = a.cursor();
  int64_t r2 = rr_count(&c3);
  std::this_thread::sleep_for(std::chrono::milliseconds(2000));
  size_t c4 = a.cursor();
  int64_t r3 = rr_count(&c4);
  double rate2 = (r3 - r2) / 2.0;
  // The rule (W3): no RR while this side sent anything within 0.9 x rr_ms. Media sent every 10 ms
  // suppresses nearly all (Mac: ~5/s); a bursty sender leaves gaps the RRs fill, as intended
  // (the Windows null devices, 1 ms timer: up to ~75/s).
  CHECKF(rate2 < rate * 0.9, "rr rate while B sends media %.1f/s vs %.1f/s receive-only", rate2, rate);
  std::printf("    rr: %.1f/s receive-only, %.1f/s while also sending\n", rate, rate2);
  // B's JB plays the mic
  Json st;
  size_t c5 = b.cursor();
  CHECK(b.wait(&c5, ev_is("stats"), 3000, &st));
  auto s11 = st["streams"]["11"];
  CHECKF(s11.value("playing", false) && s11.value("dec_err", 1) == 0, "%s", s11.dump().c_str());
}

static void test_flow_fallback_and_back() {
  InProc a("mic=sine:440"), b("");
  size_t ca = a.cursor(), cb = b.cursor();
  CHECK(open_pair(a, b, "0000bbbb", &ca, &cb));
  Relay relay(a, b);
  a.send(Json{{"cmd", "send"}, {"req", 3}, {"stream", 31}, {"flow", "0000bbbb"}, {"kind", "mic"},
              {"source", {{"device", nullptr}}}, {"key", b64key(3)}});
  b.send(Json{{"cmd", "recv"}, {"req", 3}, {"stream", 31}, {"flow", "0000bbbb"}, {"kind", "mic"},
              {"key", b64key(3)}, {"sink", {{"device", nullptr}}}});
  CHECK(a.wait(&ca, ev_is("ok", "req", 3), 3000) && b.wait(&cb, ev_is("ok", "req", 3), 3000));
  // 20 % loss on the media: the flow stays up (control packets carry on)
  a.send(R"({"cmd":"dev","req":4,"loss_pct":20})");
  CHECK(a.wait(&ca, ev_is("ok", "req", 4), 3000));
  std::this_thread::sleep_for(std::chrono::milliseconds(2500));
  for (auto& e : a.events_since(ca)) CHECK(!(e.value("ev", "") == "flow_state" && e.value("state", "") == "link"));
  a.send(R"({"cmd":"dev","req":5,"loss_pct":0})");
  // block UDP on A (both directions): both sides fall back to the link within 2.2 s
  auto t0 = std::chrono::steady_clock::now();
  a.send(R"({"cmd":"dev","req":6,"drop_udp":true})");
  CHECK(a.wait(&ca, ev_is("flow_state", "state", "link"), 4000));
  double ta = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  CHECK(b.wait(&cb, ev_is("flow_state", "state", "link"), 4000));
  double tb = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  CHECKF(ta < 2.2 && tb < 2.2, "fallback after %.2f / %.2f s", ta, tb);
  std::this_thread::sleep_for(std::chrono::milliseconds(2000));
  CHECKF(relay.moved.load() > 100, "only %llu packets via the link", (unsigned long long)relay.moved.load());
  Json st;
  size_t cs = b.cursor();
  CHECK(b.wait(&cs, ev_is("stats"), 3000, &st));
  auto s31 = st["streams"]["31"];
  CHECKF(s31.value("path", "") == "link" && s31.value("playing", false), "%s", s31.dump().c_str());
  // restore UDP: back to up within 1.5 s
  t0 = std::chrono::steady_clock::now();
  a.send(R"({"cmd":"dev","req":7,"drop_udp":false})");
  CHECK(a.wait(&ca, ev_is("flow_state", "state", "up"), 4000));
  ta = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  CHECK(b.wait(&cb, ev_is("flow_state", "state", "up"), 4000));
  tb = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  CHECKF(ta < 1.5 && tb < 1.5, "back to up after %.2f / %.2f s", ta, tb);
  std::printf("    fallback/return measured: %.2f / %.2f s\n", ta, tb);
}

// ---- golden control session

static bool match(const Json& pat, const Json& got, std::map<std::string, Json>* vars, std::string* why) {
  if (pat.is_string()) {
    const std::string s = pat.get<std::string>();
    if (s == "*") return true;
    if (s.size() > 1 && s[0] == '$') {
      auto it = vars->find(s.substr(1));
      if (it == vars->end()) {
        (*vars)[s.substr(1)] = got;
        return true;
      }
      if (it->second != got) *why = "var " + s + " = " + it->second.dump() + " != " + got.dump();
      return it->second == got;
    }
  }
  if (pat.is_object()) {
    if (!got.is_object()) return false;
    bool partial = pat.contains("...");
    for (auto it = pat.begin(); it != pat.end(); ++it) {
      if (it.key() == "...") continue;
      auto g = got.find(it.key());
      if (g == got.end()) {
        *why = "missing " + it.key();
        return false;
      }
      if (!match(it.value(), *g, vars, why)) {
        if (why->empty()) *why = it.key() + ": " + g->dump();
        return false;
      }
    }
    if (!partial)
      for (auto it = got.begin(); it != got.end(); ++it)
        if (!pat.contains(it.key())) {
          *why = "unexpected field " + it.key();
          return false;
        }
    return true;
  }
  if (pat.is_array()) {
    if (!got.is_array() || got.size() != pat.size()) return false;
    for (size_t i = 0; i < pat.size(); ++i)
      if (!match(pat[i], got[i], vars, why)) return false;
    return true;
  }
  return pat == got;
}

static Json substitute(const Json& j, const std::map<std::string, Json>& vars) {
  if (j.is_string()) {
    std::string s = j.get<std::string>();
    if (s.size() > 1 && s[0] == '$') {
      auto it = vars.find(s.substr(1));
      if (it != vars.end()) return it->second;
    }
    return j;
  }
  if (j.is_object()) {
    Json o = Json::object();
    for (auto it = j.begin(); it != j.end(); ++it) o[it.key()] = substitute(it.value(), vars);
    return o;
  }
  if (j.is_array()) {
    Json a = Json::array();
    for (auto& e : j) a.push_back(substitute(e, vars));
    return a;
  }
  return j;
}

static void test_golden_control() {
  std::ifstream f(g_golden);
  CHECKF(f.good(), "cannot open %s", g_golden.c_str());
  InProc a("vmic=absent"), b("");
  std::map<char, InProc*> h{{'A', &a}, {'B', &b}};
  std::map<char, size_t> cur{{'A', 0}, {'B', 0}};
  std::map<std::string, Json> vars{{"FK", b64key(9)}, {"SK1", b64key(1)}, {"SK2", b64key(2)}};
  std::string line;
  int lineno = 0, cmds = 0;
  std::set<std::string> covered;
  while (std::getline(f, line)) {
    lineno++;
    if (line.empty() || line[0] == '#') continue;
    if (line.size() < 4 || (line[0] != 'A' && line[0] != 'B') || line[3] != ' ') {
      CHECKF(false, "golden line %d malformed", lineno);
      continue;
    }
    InProc& p = *h[line[0]];
    std::string op = line.substr(1, 2), body = line.substr(4);
    if (op == "->") {
      Json cmd = Json::parse(body, nullptr, false);
      if (cmd.is_discarded()) {
        // raw (invalid JSON) line sent as is
        p.send(body);
      } else {
        cmd = substitute(cmd, vars);
        covered.insert(cmd.value("cmd", ""));
        p.send(cmd);
      }
      cmds++;
    } else if (op == "<-") {
      Json pat = Json::parse(body, nullptr, false);
      CHECKF(!pat.is_discarded(), "golden line %d: bad JSON", lineno);
      std::string why;
      std::map<std::string, Json> trial;
      bool ok = p.wait(&cur[line[0]], [&](const Json& got) {
        trial = vars;
        std::string w;
        bool m = match(pat, got, &trial, &w);
        if (!m && got.value("ev", "") == pat.value("ev", "")) why = w + " in " + got.dump();
        return m;
      }, 4000);
      if (ok) vars = trial;
      CHECKF(ok, "golden line %d: no event matching %s (closest: %s)", lineno, body.c_str(), why.c_str());
    }
  }
  for (const char* c : {"devices", "flow_open", "flow_remote", "flow_close", "link_in", "send", "recv", "loop",
                        "stop", "meter", "test_input", "ping", "quit"})
    CHECKF(covered.count(c), "golden session does not cover %s", c);
  std::printf("    %d commands, %zu kinds\n", cmds, covered.size());
}

static void test_run_helper_cli() {
  // diagnostics mode through run_helper, null backend
  auto be = make_null_backend("");
  const char* argv1[] = {"beamer-audio", "version"};
  CHECK(run_helper(*be, 2, const_cast<char**>(argv1)) == 0);
  const char* argv2[] = {"beamer-audio", "restore", "--backend", "null:"};
  CHECK(run_helper(*be, 4, const_cast<char**>(argv2)) == 0);
}

// ===================================================================== AEC (§7, §11.2)

// Speech-like test signal: syllables (120-350 ms) of a gliding harmonic voice with two formants and a
// little breath noise, Hann-shaped, separated by pauses; RMS -20 dBFS over the active parts.
static std::vector<float> speechlike(size_t n, uint32_t seed, std::vector<uint8_t>* active = nullptr) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> u(0.0, 1.0);
  std::normal_distribution<double> g(0.0, 1.0);
  std::vector<float> x(n, 0.0f);
  std::vector<uint8_t> act(n, 0);
  size_t i = size_t(u(rng) * 0.2 * kRate);
  while (i < n) {
    const size_t len = size_t((0.12 + 0.23 * u(rng)) * kRate);
    const double f0 = 90 + 130 * u(rng), glide = (u(rng) - 0.5) * 0.2;
    const double F1 = 300 + 500 * u(rng), F2 = 900 + 1600 * u(rng);
    double ph = 0;
    for (size_t k = 0; k < len && i + k < n; ++k) {
      const double t = double(k) / len;
      const double f = f0 * (1 + glide * t);
      ph += 2 * M_PI_ * f / kRate;
      double v = 0;
      for (int h = 1; h * f < 4000; ++h) {
        const double fh = h * f;
        const double a = (1.0 + 3.0 * std::exp(-std::pow((fh - F1) / 150, 2)) + 2.0 * std::exp(-std::pow((fh - F2) / 250, 2))) / h;
        v += a * std::sin(h * ph);
      }
      v += 0.3 * g(rng);
      const double w = 0.5 - 0.5 * std::cos(2 * M_PI_ * t);
      x[i + k] = float(v * w);
      act[i + k] = w > 0.3;
    }
    i += len + size_t((0.05 + 0.25 * u(rng)) * kRate);
  }
  double ss = 0;
  size_t na = 0;
  for (size_t k = 0; k < n; ++k)
    if (act[k]) ss += double(x[k]) * x[k], ++na;
  const float gain = na ? float(0.1 / std::sqrt(ss / na)) : 1.0f;
  for (float& v : x) v *= gain;
  if (active) *active = std::move(act);
  return x;
}

// Sparse room: direct path after `delay_ms`, then 30 reflections over the next 40 ms decaying with
// tau 8 ms; scaled so that the echo is `gain_db` below its source.
static std::vector<std::pair<int, float>> room_ir(double delay_ms, double gain_db, uint32_t seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> u(0.0, 1.0);
  std::vector<std::pair<int, double>> taps{{0, 1.0}};
  for (int k = 0; k < 30; ++k) {
    int d = 1 + int(u(rng) * 0.040 * kRate);
    taps.push_back({d, (u(rng) < 0.5 ? -1 : 1) * 0.6 * std::exp(-d / (0.008 * kRate))});
  }
  double e = 0;
  for (auto& t : taps) e += t.second * t.second;
  const double scale = std::sqrt(std::pow(10.0, gain_db / 10.0) / e);
  std::vector<std::pair<int, float>> out;
  const int d0 = int(std::lround(delay_ms * kRate / 1000.0));
  for (auto& t : taps) out.push_back({d0 + t.first, float(t.second * scale)});
  return out;
}

static std::vector<float> convolve(const std::vector<float>& x, const std::vector<std::pair<int, float>>& ir) {
  std::vector<float> y(x.size(), 0.0f);
  for (auto& t : ir)
    for (size_t i = size_t(t.first); i < x.size(); ++i) y[i] += t.second * x[i - size_t(t.first)];
  return y;
}

static std::vector<float> white(size_t n, double dbfs, uint32_t seed) {
  std::mt19937 rng(seed);
  std::normal_distribution<double> g(0.0, std::pow(10.0, dbfs / 20.0));
  std::vector<float> v(n);
  for (float& s : v) s = float(g(rng));
  return v;
}

static double energy(const std::vector<float>& x, size_t a, size_t b) {
  double s = 0;
  for (size_t i = a; i < b && i < x.size(); ++i) s += double(x[i]) * x[i];
  return s;
}

// Runs the canceller over whole signals (ref already aligned: the echo lags it by the IR delay).
static std::vector<float> run_canceller(const std::vector<float>& mic, const std::vector<float>& ref, double* us_per_frame = nullptr) {
  auto ec = make_echo_canceller();
  std::vector<float> out(mic.size(), 0.0f);
  const auto t0 = std::chrono::steady_clock::now();
  size_t frames = 0;
  for (size_t i = 0; i + kAecFrame <= mic.size(); i += kAecFrame, ++frames) ec->process(&mic[i], &ref[i], &out[i]);
  if (us_per_frame)
    *us_per_frame = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / double(frames);
  return out;
}

// ERLE over frames where the echo is active: 10 log10(mic energy / output energy).
static double erle_db(const std::vector<float>& mic, const std::vector<float>& out, const std::vector<float>& echo, size_t a, size_t b) {
  double em = 0, eo = 0;
  for (size_t i = a; i + kAecFrame <= b; i += kAecFrame) {
    if (energy(echo, i, i + kAecFrame) / kAecFrame < 1e-6) continue;  // echo quieter than -60 dBFS
    em += energy(mic, i, i + kAecFrame);
    eo += energy(out, i, i + kAecFrame);
  }
  return eo > 0 ? 10 * std::log10(em / eo) : 99;
}

static void test_aec_synthetic_echo() {
  // §11.2: echo -11 dB, 44 ms + room IR, noise: ERLE >= 20 dB after 5 s.
  const size_t n = size_t(12 * kRate);
  auto far = speechlike(n, 11);
  auto echo = convolve(far, room_ir(44, -11, 5));
  auto noise = white(n, -65, 7);
  std::vector<float> mic(n);
  for (size_t i = 0; i < n; ++i) mic[i] = echo[i] + noise[i];
  double us = 0;
  auto out = run_canceller(mic, far, &us);
  const double e1 = erle_db(mic, out, echo, size_t(1 * kRate), size_t(3 * kRate));
  const double e5 = erle_db(mic, out, echo, size_t(5 * kRate), n);
  std::printf("    ERLE 1-3 s %.1f dB, 5-12 s %.1f dB; %.0f us per 10 ms frame (%.2f %% of a core)\n", e1, e5, us, us / 100.0);
  CHECKF(e5 >= 20.0, "ERLE after 5 s %.1f dB (< 20)", e5);
  if (!SANITIZED) CHECKF(us < 300.0, "%.0f us per 10 ms frame (3 %% of a core = 300)", us);
}

static void test_aec_near_end_only() {
  // §11.2: near-end-only speech attenuated < 1 dB: (a) fresh canceller, far end silent; (b) after 5 s
  // of echo-only adaptation, then the far end stops and the near end talks.
  const size_t n = size_t(10 * kRate), half = size_t(5 * kRate);
  auto near = speechlike(n, 21);
  auto noise = white(n, -65, 8);
  std::vector<float> mic(n), ref(n, 0.0f);
  for (size_t i = 0; i < n; ++i) mic[i] = near[i] + noise[i];
  auto out = run_canceller(mic, ref);
  const double a = 10 * std::log10(energy(mic, size_t(kRate), n) / energy(out, size_t(kRate), n));
  // (b)
  auto far = speechlike(n, 22);
  auto echo = convolve(far, room_ir(44, -11, 6));
  std::vector<float> mic2(n), ref2(n, 0.0f);
  for (size_t i = 0; i < n; ++i) {
    ref2[i] = i < half ? far[i] : 0.0f;
    const float e = i < half + size_t(0.1 * kRate) ? echo[i] * (i < half ? 1.0f : 0.0f) : 0.0f;
    mic2[i] = (i < half ? e : near[i]) + noise[i];
  }
  auto out2 = run_canceller(mic2, ref2);
  const size_t b0 = half + size_t(0.2 * kRate);
  const double b = 10 * std::log10(energy(mic2, b0, n) / energy(out2, b0, n));
  std::printf("    near-end attenuation: fresh %.2f dB, after echo adaptation %.2f dB\n", a, b);
  CHECKF(std::fabs(a) < 1.0, "near end changed by %.2f dB (fresh)", a);
  CHECKF(std::fabs(b) < 1.0, "near end changed by %.2f dB (adapted)", b);
}

static void test_aec_double_talk() {
  // Not a §11.2 bound: describes double talk. 5 s echo only, then 5 s near + far together.
  const size_t n = size_t(10 * kRate), half = size_t(5 * kRate);
  auto far = speechlike(n, 31);
  auto near = speechlike(n, 32);
  auto echo = convolve(far, room_ir(44, -11, 9));
  auto noise = white(n, -65, 10);
  std::vector<float> mic(n), nearpart(n, 0.0f);
  for (size_t i = 0; i < n; ++i) {
    nearpart[i] = i >= half ? near[i] : 0.0f;
    mic[i] = echo[i] + nearpart[i] + noise[i];
  }
  auto out = run_canceller(mic, far);
  // near-end level in the output: frames where the near end is active, output vs near alone
  double en = 0, eo = 0, ee = 0, eeo = 0;
  for (size_t i = half; i + kAecFrame <= n; i += kAecFrame) {
    const double fn = energy(nearpart, i, i + kAecFrame), fe = energy(echo, i, i + kAecFrame);
    if (fn > 10 * fe && fn / kAecFrame > 1e-4) {  // near end dominates
      en += fn;
      eo += energy(out, i, i + kAecFrame);
    } else if (fe > 10 * fn && fe / kAecFrame > 1e-5) {  // far end alone in the double-talk period
      ee += energy(mic, i, i + kAecFrame);
      eeo += energy(out, i, i + kAecFrame);
    }
  }
  const double near_change = 10 * std::log10(eo / en), echo_red = 10 * std::log10(ee / eeo);
  const double overall = erle_db(mic, out, echo, size_t(2 * kRate), half);
  std::printf("    double talk: near-end-dominated frames %+.2f dB, echo-only frames -%.1f dB (single talk before: -%.1f dB)\n",
              near_change, echo_red, overall);
  CHECKF(near_change > -3.0, "near end lost %.2f dB in double talk", near_change);
  CHECKF(echo_red > 10.0, "echo reduction during double talk %.1f dB", echo_red);
}

// AecPath end to end without threads: mic and reference capture blocks with their own clocks and
// host timestamps (+-jitter), the reference device running `ppm` fast, the echo `acoustic_ms` after
// the reference's timestamp; the stream is processed like SendStream's encoder does.
struct PathRun {
  double erle_late = 0, echo_ms = -1, err_ms = 0, ppm = 0;
  uint64_t resync = 0, ref_short = 0, waits = 0, no_ref = 0;
};

static PathRun run_path(double ppm, double acoustic_ms, double jitter_ms, double jump_at_s, int ref_block, double seconds) {
  const size_t n = size_t(seconds * kRate);
  // far end on the reference device's grid, room on that grid too
  auto far = speechlike(n + kRate, 41);
  auto y = convolve(far, room_ir(0, -11, 12));
  // mic hears y at reference index n(t) = (t - tr0) * kRate * (1 + ppm): resample y with step 1+ppm
  const double step = 1.0 + ppm * 1e-6;
  const double tm0 = 5e9, tr0 = 5e9 - 0.2e9;  // the reference stream started 200 ms earlier
  // mic sample i at time tm0 + i/kRate hears y at index ((tm0 + i/kRate - acoustic) - tr0) * kRate * step
  const double start_idx = ((tm0 - acoustic_ms * 1e6) - tr0) * 1e-9 * kRate * step;
  // each echo sample by direct windowed-sinc interpolation of y (32 taps, Hann)
  std::vector<float> echo(n);
  const int H = 16;
  for (size_t i = 0; i < n; ++i) {
    const double p = start_idx + double(i) * step;
    const int64_t b = int64_t(std::floor(p));
    const double fr = p - double(b);
    double acc = 0;
    for (int k = -H + 1; k <= H; ++k) {
      const int64_t j = b + k;
      if (j < 0 || j >= int64_t(y.size())) continue;
      const double x = double(k) - fr;
      const double w = 0.5 + 0.5 * std::cos(M_PI_ * x / H);
      const double s = x == 0 ? 1.0 : std::sin(M_PI_ * x) / (M_PI_ * x);
      acc += y[size_t(j)] * s * w;
    }
    echo[i] = float(acc);
  }
  auto noise = white(n, -65, 13);
  std::vector<float> mic(n);
  for (size_t i = 0; i < n; ++i) mic[i] = echo[i] + noise[i];

  AecOptions opt;  // lead -1 ms, max wait 30 ms
  AecPath path(opt);
  std::mt19937 rng(77);
  std::uniform_real_distribution<double> jit(-jitter_ms * 1e6, jitter_ms * 1e6);
  std::vector<float> out(n, 0.0f);
  const int mic_block = 480;
  // event-driven: each block is delivered at the end of its duration (+1 ms), timestamped with
  // its first sample's time (+ jitter); a reference jump shifts its timestamps by 50 ms.
  size_t mi = 0, ri = 0, m = 0;
  std::vector<float> ring;  // the mic ring (SendStream's ring_)
  size_t ring_r = 0;
  const double ref_rate = kRate * step;
  for (;;) {
    const double t_mic = tm0 + double(mi + mic_block) / kRate * 1e9 + 1e6;
    const double t_ref = tr0 + double(ri + size_t(ref_block)) / ref_rate * 1e9 + 1e6;
    if (mi + mic_block > n) break;
    if (t_ref <= t_mic) {
      std::vector<float> blk(static_cast<size_t>(ref_block));
      for (int k = 0; k < ref_block; ++k) blk[size_t(k)] = ri + size_t(k) < far.size() ? far[ri + size_t(k)] : 0.0f;
      const double ts = tr0 + double(ri) / ref_rate * 1e9 + jit(rng);
      ri += size_t(ref_block);
      // a reference restart: 60 ms of blocks never delivered (timestamps stay true)
      if (jump_at_s > 0 && ts - tr0 > jump_at_s * 1e9 && ts - tr0 < jump_at_s * 1e9 + 60e6) continue;
      path.on_capture(blk.data(), ref_block, 1, uint64_t(ts));
      continue;
    }
    path.mic_mark(mi, uint64_t(tm0 + double(mi) / kRate * 1e9 + jit(rng)));
    ring.insert(ring.end(), mic.begin() + long(mi), mic.begin() + long(mi + mic_block));
    mi += mic_block;
    while (ring.size() - ring_r >= size_t(kAecFrame)) {
      if (!path.ready(m, ring.size() - ring_r - kAecFrame)) break;
      path.process(&ring[ring_r], m, &out[m]);
      ring_r += kAecFrame;
      m += kAecFrame;
    }
  }
  Json st = path.stats(1.0);
  PathRun r;
  if (std::getenv("BA_AEC_TRACE")) {
    std::printf("      ERLE per 2 s:");
    for (size_t a0 = 0; a0 + size_t(2 * kRate) <= m; a0 += size_t(2 * kRate))
      std::printf(" %.1f", erle_db(mic, out, echo, a0, a0 + size_t(2 * kRate)));
    std::printf("\n");
  }
  const size_t a = n > size_t(8 * kRate) ? n - size_t(4 * kRate) : n / 2;
  r.erle_late = erle_db(mic, out, echo, a, m);
  r.echo_ms = st["echo_ms"].get<double>();
  r.err_ms = st["err_ms"].get<double>();
  r.ppm = st["ppm"].get<double>();
  r.resync = st["resync"].get<uint64_t>();
  r.ref_short = st["ref_short"].get<uint64_t>();
  r.waits = st["waits"].get<uint64_t>();
  r.no_ref = st["no_ref"].get<uint64_t>();
  return r;
}

static void test_aec_path_alignment() {
  struct C { double ppm, acoustic, jitter, jump; int ref_block; const char* what; };
  const C cases[] = {
      {0, 44, 0, 0, 480, "same clock, 44 ms"},
      {0, 25, 0.5, 0, 512, "Windows-like blocks, 0.5 ms timestamp jitter"},
      {+100, 44, 0.3, 0, 441, "reference +100 ppm"},
      {-100, 44, 0.3, 0, 480, "reference -100 ppm"},
      {0, 44, 0.2, 5, 480, "reference loses 60 ms at 5 s (restart)"},
  };
  for (const C& c : cases) {
    PathRun r = run_path(c.ppm, c.acoustic, c.jitter, c.jump, c.ref_block, 14);
    std::printf("    %-46s ERLE %.1f dB, echo at %.1f ms, err %.2f ms, ppm %.0f, resync %llu, short %llu, waits %llu, no_ref %llu\n",
                c.what, r.erle_late, r.echo_ms, r.err_ms, r.ppm, (unsigned long long)r.resync, (unsigned long long)r.ref_short,
                (unsigned long long)r.waits, (unsigned long long)r.no_ref);
    CHECKF(r.erle_late >= 20.0, "%s: ERLE %.1f dB", c.what, r.erle_late);
    CHECKF(std::fabs(r.echo_ms - c.acoustic) < 4.0, "%s: echo found at %.1f ms, placed at %.0f", c.what, r.echo_ms, c.acoustic);
    CHECKF(r.resync == (c.jump > 0 ? 2u : 1u), "%s: %llu resyncs", c.what, (unsigned long long)r.resync);
  }
}

static void test_aec_wiring() {
  // features: "aec" only when compiled and the backend can capture a reference
  auto nb = make_null_backend("");
  auto f = engine_features(*nb);
  CHECK(std::find(f.begin(), f.end(), "aec") != f.end());
  // send with aec through the control loop: the reference opens on null-out, frames flow through
  // the canceller; without null-out the mic still runs, without echo cancellation.
  for (int variant = 0; variant < 2; ++variant) {
    InProc a(variant == 0 ? "mic=sine:440:0.2;ref=sine:1000:0.2" : "out=absent");
    size_t c = 0;
    a.send(R"({"cmd":"flow_open","req":1,"flow":"0000aec0","peer":"p","role":"offer","port":0,"remote":null,"key":")" +
           b64key(3) + R"(","udp":false})");
    CHECK(a.wait(&c, [](const Json& e) { return e.value("ev", "") == "ok" && e.value("req", 0) == 1; }, 3000));
    a.send(R"({"cmd":"send","req":2,"stream":77,"flow":"0000aec0","kind":"mic","source":{"device":null},"key":")" +
           b64key(4) + R"(","aec":{"reference":null,"lead_ms":5}})");
    CHECK(a.wait(&c, [](const Json& e) { return e.value("ev", "") == "ok" && e.value("req", 0) == 2; }, 3000));
    Json st;
    bool got = a.wait(&c, [](const Json& e) {
      if (e.value("ev", "") != "stats" || !e["streams"].contains("77")) return false;
      const Json& s = e["streams"]["77"];
      return s.contains("aec") && s["frames"].get<uint64_t>() > 40;
    }, 5000, &st);
    CHECK(got);
    if (!got) continue;
    const Json& s = st["streams"]["77"]["aec"];
    std::printf("    %s: %s\n", variant == 0 ? "reference" : "no null-out", s.dump().c_str());
    if (variant == 0) {
      CHECK(s.value("on", false));
      CHECK(s.value("ref", std::string()) == "null-out");
    } else {
      CHECK(!s.value("on", true));
      CHECK(s.value("error", std::string()) == "no_device");
    }
  }
}

// ===================================================================== howl guard (WP13)

namespace howl_sim {
struct Biquad {
  double b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
  double run(double x) {
    double y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};
static Biquad bandpass(double f, double q) {  // RBJ, 0 dB at f
  const double w = 2 * M_PI_ * f / 48000, al = std::sin(w) / (2 * q), a0 = 1 + al;
  return Biquad{al / a0, 0, -al / a0, -2 * std::cos(w) / a0, (1 - al) / a0};
}
struct LoopResult {
  uint32_t episodes = 0, false_alarms = 0;
  double first_s = -1, freq_hz = 0;
  double loud_after = 0;  // share of 100 ms output blocks above -20 dBFS from detection + 0.5 s on
  double max_db = -200;   // loudest 100 ms block over the run
};
// Feedback through two machines: what the guard sends plays on speakers that a microphone hears
// after `delay_ms` (network, buffers, air), with gain `G` at f0 (a resonance of quality q) and 0.3
// elsewhere, back into the captured sound, plus a -65 dBFS noise floor and an optional music bed.
// The speaker path saturates (tanh) near -10 dBFS RMS, as the live howl did.
static LoopResult loop(double G, double f0, double q, double delay_ms, double secs, bool guard,
                       const std::function<double(int64_t)>& bed) {
  HowlGuard g(2);
  const int D = int(delay_ms * 48);
  std::vector<double> line(size_t(D), 0.0);
  int lp = 0;
  Biquad bp = bandpass(f0, q);
  std::mt19937 rng(1);
  std::normal_distribution<double> nd(0, 1);
  std::vector<float> blk(960);
  const int64_t N = int64_t(secs * 48000);
  LoopResult r;
  double acc = 0;
  int accn = 0, after = 0, loud = 0;
  for (int64_t n = 0; n < N; n += 480) {
    for (int i = 0; i < 480; ++i) {
      const double y = line[size_t((lp + i) % D)];
      const double v = 0.3 * y + (G - 0.3) * bp.run(y) + 0.0006 * nd(rng) + (bed ? bed(n + i) : 0.0);
      blk[2 * i] = blk[2 * i + 1] = float(v);
    }
    if (guard) g.process(blk.data(), 480);
    for (int i = 0; i < 480; ++i) {
      const double o = std::tanh(blk[2 * i] * 2.0) / 2.0;
      line[size_t((lp + i) % D)] = o;
      acc += o * o;
      ++accn;
    }
    lp = (lp + 480) % D;
    if (r.first_s < 0 && g.episodes()) {
      r.first_s = n / 48000.0;
      r.freq_hz = g.freq_hz();
    }
    if (accn >= 4800) {
      const double db = 10 * std::log10(acc / accn + 1e-20);
      r.max_db = std::max(r.max_db, db);
      if (r.first_s >= 0 && n / 48000.0 > r.first_s + 0.5) {
        ++after;
        loud += db > -20 ? 1 : 0;
      }
      acc = 0;
      accn = 0;
    }
  }
  r.episodes = g.episodes();
  r.false_alarms = g.false_alarms();
  r.loud_after = after ? double(loud) / after : 1.0;
  return r;
}
static double chord_bed(int64_t n) {  // a C major chord with partials, slowly swelling and fading
  const double t = n / 48000.0, fs[] = {261.6, 329.6, 392.0};
  double s = 0;
  for (double f : fs)
    for (int h = 1; h <= 5; ++h) s += 0.02 / h * std::sin(2 * M_PI_ * f * h * t);
  return s * (0.6 + 0.4 * std::sin(2 * M_PI_ * 0.5 * t));
}
}  // namespace howl_sim

static void test_howl_feedback_loop() {
  using namespace howl_sim;
  // Without the guard the loop howls at the speakers' limit.
  LoopResult free_run = loop(1.25, 7100, 6, 100, 10, false, nullptr);
  CHECKF(free_run.max_db > -15, "%.1f", free_run.max_db);
  struct Case {
    const char* name;
    double G, f0, q, delay_ms;
    bool bed;
    double max_first_s, max_loud;
  };
  const Case cases[] = {
      {"7.1 kHz, loop gain 1.25 (+19 dB/s)", 1.25, 7100, 6, 100, false, 4, 0.0},
      {"7.1 kHz, loop gain 1.06 (+5 dB/s)", 1.06, 7100, 6, 100, false, 9, 0.0},
      {"7.1 kHz, narrow resonance, gain 2", 2.0, 7100, 20, 100, false, 2, 0.0},
      {"7.1 kHz, narrow resonance, gain 4", 4.0, 7100, 20, 100, false, 1, 0.0},
      {"1.2 kHz, 180 ms loop, gain 1.15", 1.15, 1200, 6, 180, false, 8, 0.0},
      {"7.1 kHz under music, gain 1.25", 1.25, 7100, 6, 100, true, 5, 0.1},
  };
  for (const Case& c : cases) {
    LoopResult r = loop(c.G, c.f0, c.q, c.delay_ms, 20, true, c.bed ? chord_bed : nullptr);
    std::printf("    %-36s first %.2f s at %.0f Hz, %u episodes, loud after %.0f %%, false %u\n", c.name, r.first_s,
                r.freq_hz, r.episodes, 100 * r.loud_after, r.false_alarms);
    CHECKF(r.first_s >= 0 && r.first_s <= c.max_first_s, "%s: first %.2f", c.name, r.first_s);
    CHECKF(std::fabs(r.freq_hz - c.f0) < 200, "%s: %.0f Hz", c.name, r.freq_hz);
    CHECKF(r.loud_after <= c.max_loud, "%s: %.2f", c.name, r.loud_after);
    CHECKF(r.false_alarms == 0, "%s", c.name);
  }
}

// Music with sustained tones never triggers the guard, and passes bit for bit.
static void test_howl_music_untouched() {
  struct Sig {
    const char* name;
    double secs;
    std::function<double(int64_t)> f;
  };
  std::mt19937 rng(7);
  std::normal_distribution<double> nd(0, 1);
  auto flute = [&](double f0, std::function<double(double)> amp) {
    return [=, &nd, &rng](int64_t n) {  // partials at -12/-20 dB, 5 Hz vibrato of 0.5 %, breath
      const double t = n / 48000.0, ph = 2 * M_PI_ * f0 * t + 0.005 * f0 / 5.0 * std::sin(2 * M_PI_ * 5 * t);
      return amp(t) * (std::sin(ph) + 0.25 * std::sin(2 * ph) + 0.1 * std::sin(3 * ph) + 0.003 * nd(rng));
    };
  };
  double voice_ph = 0;
  const Sig sigs[] = {
      {"chord (C major, 4 notes, 6 partials)", 10,
       [](int64_t n) {
         const double t = n / 48000.0, fs[] = {261.6, 329.6, 392.0, 523.3};
         double s = 0;
         for (double f : fs)
           for (int h = 1; h <= 6; ++h) s += 0.05 / h * std::sin(2 * M_PI_ * f * h * t);
         return s;
       }},
      {"chord swell, -40 to -10 dB over 4 s", 8,
       [](int64_t n) {
         const double t = n / 48000.0, fs[] = {220, 277.2, 329.6};
         double s = 0;
         for (double f : fs)
           for (int h = 1; h <= 4; ++h) s += 0.08 / h * std::sin(2 * M_PI_ * f * h * t);
         return s * std::pow(10, (-40 + 30 * std::min(t / 4, 1.0)) / 20);
       }},
      {"log sine sweep 100 Hz-12 kHz", 8,
       [](int64_t n) {
         const double t = n / 48000.0, T = 8, f1 = 100, k = std::log(12000 / f1);
         return 0.25 * std::sin(2 * M_PI_ * f1 * T / k * (std::exp(t / T * k) - 1));
       }},
      {"linear sine sweep 200 Hz-10 kHz", 10,
       [](int64_t n) {
         const double t = n / 48000.0;
         return 0.25 * std::sin(2 * M_PI_ * (200 * t + 0.5 * 1000 * t * t));
       }},
      {"held flute-like A5, constant level", 10, flute(880, [](double t) { return 0.2 * std::min(t / 0.08, 1.0); })},
      {"held flute-like E7, constant level", 10, flute(2637, [](double t) { return 0.2 * std::min(t / 0.08, 1.0); })},
      {"flute-like crescendo, -40 to -10 dB in 3 s", 6,
       flute(880, [](double t) { return std::pow(10, (-40 + 30 * std::min(t / 3, 1.0)) / 20); })},
      {"pure 1 kHz tone switched on", 10, [](int64_t n) { return 0.25 * std::sin(2 * M_PI_ * 1000 * n / 48000.0); }},
      {"voice-like (120-170 Hz, syllables)", 10,
       [&](int64_t n) {
         const double t = n / 48000.0, f = 120 + 50 * std::sin(2 * M_PI_ * 0.7 * t);
         voice_ph += 2 * M_PI_ * f / 48000;
         double s = 0;
         for (int h = 1; h <= 20; ++h) s += std::sin(h * voice_ph) / h;
         return 0.1 * std::max(0.0, std::sin(2 * M_PI_ * 2 * t)) * s;
       }},
  };
  for (const Sig& s : sigs) {
    HowlGuard g(2);
    std::vector<float> blk(960), orig(960);
    bool same = true;
    const int64_t N = int64_t(s.secs * 48000);
    for (int64_t n = 0; n < N; n += 480) {
      for (int i = 0; i < 480; ++i) blk[2 * i] = blk[2 * i + 1] = float(s.f(n + i));
      orig = blk;
      g.process(blk.data(), 480);
      same = same && blk == orig;
    }
    CHECKF(g.episodes() == 0, "%s: %u episodes", s.name, g.episodes());
    CHECKF(same, "%s: samples changed", s.name);
  }
}

// A pure tone that fades in looks like feedback: the guard ducks it, sees it does not decay under the
// duck (no loop), releases within ~5 s and ignores that frequency; poll() reports both changes.
static void test_howl_false_alarm_release() {
  HowlGuard g(1);
  std::vector<float> blk(480);
  HowlReport r;
  CHECK(!g.poll(&r) && !r.active);
  int on = 0, off = 0;
  double on_at = -1, off_at = -1;
  for (int64_t n = 0; n < 12 * 48000; n += 480) {
    for (int i = 0; i < 480; ++i) {
      const double t = (n + i) / 48000.0;
      blk[size_t(i)] = float(0.25 * std::min(t, 1.0) * std::sin(2 * M_PI_ * 1000 * t));
    }
    g.process(blk.data(), 480);
    if (g.poll(&r)) {
      if (r.active) {
        ++on;
        on_at = n / 48000.0;
        CHECKF(std::fabs(r.freq_hz - 1000) < 30 && r.duck_db == -20, "%.0f Hz %.0f dB", r.freq_hz, r.duck_db);
      } else {
        ++off;
        off_at = n / 48000.0;
      }
    }
  }
  std::printf("    ducked at %.2f s, released at %.2f s\n", on_at, off_at);
  CHECK(on == 1 && off == 1);
  CHECK(g.false_alarms() == 1);
  CHECK(off_at > on_at + 4 && off_at < on_at + 6.5);
  CHECK(g.gain_db() == 0);
}

// Through the helper: a system send stream whose sound rises like feedback (dev test source
// "rise:7100") emits `howl` active, then (no loop: the tone stays) inactive after the false-alarm
// release.
static void test_howl_event() {
  InProc a("");
  size_t c = 0;
  a.send(R"({"cmd":"flow_open","req":1,"flow":"0000b0b0","peer":"p","role":"offer","port":0,"remote":null,"key":")" +
         b64key(5) + R"(","udp":false})");
  CHECK(a.wait(&c, [](const Json& e) { return e.value("ev", "") == "ok" && e.value("req", 0) == 1; }, 3000));
  a.send(R"({"cmd":"send","req":2,"stream":88,"flow":"0000b0b0","kind":"system","source":{"test":"rise:7100:30"},"key":")" +
         b64key(6) + R"(","mute_local":false})");
  CHECK(a.wait(&c, [](const Json& e) { return e.value("ev", "") == "ok" && e.value("req", 0) == 2; }, 3000));
  Json ev;
  bool got = a.wait(&c, [](const Json& e) { return e.value("ev", "") == "howl"; }, 8000, &ev);
  CHECK(got);
  if (!got) return;
  std::printf("    %s\n", ev.dump().c_str());
  CHECK(ev.value("stream", 0) == 88 && ev.value("active", false) && ev.value("duck_db", 0.0) == -20.0);
  CHECKF(std::fabs(ev.value("freq_hz", 0.0) - 7100) < 50, "%s", ev.dump().c_str());
  got = a.wait(&c, [](const Json& e) { return e.value("ev", "") == "howl"; }, 8000, &ev);
  CHECK(got && !ev.value("active", true));
}

// ===================================================================== main

struct Case {
  const char* name;
  void (*fn)();
};

int main(int argc, char** argv) {
  std::vector<std::string> filters;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--vectors" && i + 1 < argc) {
      g_vectors = argv[++i];
      g_golden = g_vectors + "/../golden_control.txt";
    } else {
      filters.push_back(a);
    }
  }
  set_log_level(LogLevel::Warn);
  net::startup();
  const Case cases[] = {
      {"aead_rfc8439", test_aead_rfc8439},
      {"aead_pynacl_vectors", test_aead_pynacl_vectors},
      {"packet_replay_tamper", test_packet_replay_tamper},
      {"base64", test_base64},
      {"opus_roundtrip", test_opus_roundtrip},
      {"resampler_snr", test_resampler_snr},
      {"map_channels", test_map_channels},
      {"signal_pilot_level", test_signal_pilot_level},
      {"jb_quiet", test_jb_quiet_and_fixed},
      {"jb_loss_jitter_drift_10min", test_jb_loss_jitter_drift_10min},
      {"jb_gilbert_negative_drift", test_jb_gilbert_negative_drift},
      {"jb_stall_flush", test_jb_stall_flush},
      {"jb_drift_integral_with_expansions", test_jb_drift_integral_with_expansions},
      {"jb_drift_estimate_step", test_jb_drift_estimate_step},
      {"spsc_threads", test_spsc_threads},
      {"rr_timing", test_rr_timing},
      {"flow_fallback_and_back", test_flow_fallback_and_back},
      {"golden_control", test_golden_control},
      {"run_helper_cli", test_run_helper_cli},
      {"aec_synthetic_echo", test_aec_synthetic_echo},
      {"aec_near_end_only", test_aec_near_end_only},
      {"aec_double_talk", test_aec_double_talk},
      {"aec_path_alignment", test_aec_path_alignment},
      {"aec_wiring", test_aec_wiring},
      {"howl_feedback_loop", test_howl_feedback_loop},
      {"howl_music_untouched", test_howl_music_untouched},
      {"howl_false_alarm_release", test_howl_false_alarm_release},
      {"howl_event", test_howl_event},
  };
  int run = 0;
  for (const Case& c : cases) {
    if (!filters.empty() &&
        std::none_of(filters.begin(), filters.end(), [&](const std::string& f) { return std::strstr(c.name, f.c_str()); }))
      continue;
    int before = g_failures;
    auto t0 = std::chrono::steady_clock::now();
    std::printf("[ run ] %s\n", c.name);
    std::fflush(stdout);
    c.fn();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("[%s] %s (%.0f ms)\n", g_failures == before ? " ok  " : "FAIL ", c.name, ms);
    std::fflush(stdout);
    run++;
  }
  std::printf("%d cases, %d checks, %d failures\n", run, g_checks, g_failures);
  return g_failures ? 1 : 0;
}
