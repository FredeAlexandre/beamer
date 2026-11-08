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

#include "codec.h"
#include "crypto.h"
#include "engine.h"
#include "flow.h"
#include "helper.h"
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
