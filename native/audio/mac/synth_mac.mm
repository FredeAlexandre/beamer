// SPDX-License-Identifier: GPL-3.0-only
// macOS video backend: the --dev synthetic camera (camera PHASE2 §4.11, R2 #4). C1's test picture
// (dev/camera/proto/mac/src/synth.mm: moving disc, gradient, scrolling text panel, sensor noise, code strip),
// drawn at any native size inside its 16:9 centre crop so that the crop + scale path is the real one. It
// never touches AVCaptureDevice / AVCaptureSession and never reads the camera permission.
#import <Foundation/Foundation.h>

#include <mach/mach_time.h>
#include <pthread.h>
#include <time.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <map>
#include <thread>

#include "video_backend_mac.h"

namespace beamer::audio::macvideo {

namespace {

uint8_t g_noise[65536];
std::once_flag g_noise_once;
void init_noise() {
  std::call_once(g_noise_once, [] {
    uint32_t s = 0x12345678;
    for (int i = 0; i < 65536; i++) {
      s ^= s << 13;
      s ^= s >> 17;
      s ^= s << 5;
      g_noise[i] = (uint8_t)(s >> 24);
    }
  });
}

bool parse_size(const std::string& s, int* w, int* h) {
  int a = 0, b = 0;
  char x = 0;
  if (sscanf(s.c_str(), "%d%c%d", &a, &x, &b) != 3 || (x != 'x' && x != 'X')) return false;
  if (a < 16 || b < 16 || a > 8192 || b > 8192 || (a & 1) || (b & 1)) return false;
  *w = a;
  *h = b;
  return true;
}

uint64_t thread_cpu_ns() {
  timespec ts;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

}  // namespace

std::atomic<uint64_t> g_synth_cpu_ns{0};  // CPU spent drawing synthetic pictures (video_check reports it apart)

bool parse_synth(const std::string& test, SynthSpec* out, std::string* err) {
  SynthSpec s;
  std::string rest = test;
  auto fail = [&](const std::string& m) {
    if (err) *err = "bad_args: " + m;
    return false;
  };
  // head = "synth" or "synth:WxH"; then ";key=value" options
  std::string head = rest.substr(0, rest.find(';'));
  std::string opts = rest.size() > head.size() ? rest.substr(head.size() + 1) : "";
  if (head.rfind("synth", 0) != 0) return fail("unknown test source '" + test + "'");
  if (head.size() > 5) {
    if (head[5] != ':' || !parse_size(head.substr(6), &s.w, &s.h)) return fail("bad synthetic size in '" + test + "'");
  }
  while (!opts.empty()) {
    std::string kv = opts.substr(0, opts.find(';'));
    opts = opts.size() > kv.size() ? opts.substr(kv.size() + 1) : "";
    if (kv.empty()) continue;
    auto eq = kv.find('=');
    std::string k = kv.substr(0, eq), v = eq == std::string::npos ? "" : kv.substr(eq + 1);
    char* end = nullptr;
    long n = strtol(v.c_str(), &end, 10);
    if (v.empty() || *end) return fail("bad value in '" + kv + "'");
    if (k == "start_ms" && n >= 0 && n <= 10000) s.start_ms = (int)n;
    else if (k == "noise" && n >= 0 && n <= 20) s.noise = (int)n;
    else return fail("unknown or out-of-range option '" + kv + "'");
  }
  *out = s;
  return true;
}

void synth_draw(CVPixelBufferRef pb, uint32_t f, uint64_t capture_ns, const CropRect& crop, int noise, int out_w) {
  init_noise();
  CVPixelBufferLockBaseAddress(pb, 0);
  const int W = (int)CVPixelBufferGetWidth(pb), H = (int)CVPixelBufferGetHeight(pb);
  uint8_t* Y = (uint8_t*)CVPixelBufferGetBaseAddressOfPlane(pb, 0);
  uint8_t* UV = (uint8_t*)CVPixelBufferGetBaseAddressOfPlane(pb, 1);
  const size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), uvs = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
  const int cx0 = crop.x, cy0 = crop.y, w = crop.w, h = crop.h;
  const double s = out_w > 0 ? (double)w / out_w : 1.0;  // native px per output px
  const int strip_h = (int)std::ceil(w / 80.0);
  // outside the crop: black (never visible after a correct crop)
  for (int y = 0; y < H; y++) {
    uint8_t* row = Y + y * ys;
    if (y < cy0 || y >= cy0 + h) { memset(row, 16, W); continue; }
    if (cx0 > 0) memset(row, 16, cx0);
    if (cx0 + w < W) memset(row + cx0 + w, 16, W - cx0 - w);
  }
  for (int y = 0; y < H / 2; y++) memset(UV + y * uvs, 128, W);
  // picture inside the crop, in crop coordinates (u, v). Tables per thread, rebuilt on a size change: the
  // drawing is the synthetic camera's cost, not the pipeline's, so keep it small (contiguous, vectorisable).
  thread_local std::vector<uint8_t> grad;    // grad[k] = 50 + ((k % 1024) * 140 >> 10), k < 1024 + w
  thread_local std::vector<int16_t> nz;      // nz[i] = g_noise[i & 0xFFFF] % amp - noise, i < 65536 + w
  thread_local int nz_level = -1, nz_w = -1;
  thread_local std::vector<int> xs_of_u;     // u (kept as a table: the panel loop gathers)
  thread_local std::vector<uint8_t> ring;    // disc luma by squared distance
  thread_local std::vector<uint8_t> ug;      // chroma U gradient by u2
  thread_local int tab_w = -1, tab_h = -1;
  const double t = f / 30.0;
  const int dcx = (int)(w * (0.5 + 0.22 * std::sin(2 * M_PI * t / 4.0)));
  const int dcy = (int)(h * (0.48 + 0.10 * std::sin(2 * M_PI * t / 6.0)));
  const int r = h / 5;
  const int panel_v0 = h * 3 / 4, panel_v1 = h - h / 20;
  const int scroll = (int)(f * 3);
  if (tab_w != w || tab_h != h) {
    tab_w = w;
    tab_h = h;
    grad.resize(1024 + w);
    for (int k = 0; k < 1024 + w; k++) grad[k] = (uint8_t)(50 + (((k % 1024) * 140) >> 10));
    xs_of_u.resize(w);
    for (int u = 0; u < w; u++) xs_of_u[u] = u;
    ring.resize((size_t)r * r + 1);
    for (int d2 = 0; d2 <= r * r; d2++) ring[d2] = (uint8_t)(120 + ((int)(std::sqrt((double)d2) / 6) & 1) * 30);
    ug.resize(w / 2);
    for (int u2 = 0; u2 < w / 2; u2++) ug[u2] = (uint8_t)(96 + (u2 * 64) / (w / 2));
  }
  if (noise > 0 && (nz_level != noise || nz_w != w)) {
    nz_level = noise;
    nz_w = w;
    const int amp = noise * 2 + 1;
    nz.resize(65536 + w);
    for (int i = 0; i < 65536 + w; i++) nz[i] = (int16_t)(g_noise[i & 0xFFFF] % amp - noise);
  }
  for (int v = strip_h; v < h; v++) {
    uint8_t* row = Y + (cy0 + v) * ys + cx0;
    const int dy = v - dcy;
    if (v >= panel_v0 && v < panel_v1) {
      // text-like detail: 6x12 px glyph cells with pseudo-random strokes, scrolling left (native px, as a
      // camera: an upscaled low-resolution camera shows larger, softer text)
      const int pv = v - panel_v0, gy = pv % 12, cyc = pv / 12 * 977;
      for (int u = 0; u < w; u++) {
        const int xs = xs_of_u[u] + scroll;
        const int bits = g_noise[((xs / 6) * 31 + cyc) & 0xFFFF];
        const int gx = xs % 6;
        const bool on = gx < 5 && gy < 10 && ((bits >> ((gx + gy * 3) & 7)) & 1);
        row[u] = (uint8_t)(on ? 40 : 200);
      }
    } else {
      // smooth moving gradient: 50 + ((u + 2v + 4f) % 1024) * 140 / 1024
      memcpy(row, grad.data() + (int)(((int64_t)(2 * v) + 4 * (int64_t)f) % 1024), w);
    }
    if (dy > -r && dy < r) {  // textured disc ("head")
      const int half = (int)std::sqrt((double)r * r - (double)dy * dy);
      const int u0 = std::max(0, dcx - half), u1 = std::min(w - 1, dcx + half);
      const int dy2 = dy * dy, rr = r * r;
      for (int u = u0; u <= u1; u++) {
        const int dx = u - dcx, d2 = std::min(rr, dx * dx + dy2);
        row[u] = (uint8_t)(ring[d2] + (dx > 0 ? 20 : 0));
      }
    }
    if (noise > 0) {  // sensor noise, different every row and frame
      const int16_t* n = nz.data() + ((uint32_t)(v * 1031 + f * 40503u) & 0xFFFF);
      for (int u = 0; u < w; u++) {
        int x = row[u] + n[u];
        row[u] = (uint8_t)(x < 16 ? 16 : (x > 235 ? 235 : x));
      }
    }
  }
  // chroma (half resolution) inside the crop: gradients, skin tone in the disc
  for (int v2 = strip_h / 2; v2 < h / 2; v2++) {
    uint8_t* row = UV + (cy0 / 2 + v2) * uvs + cx0;
    const int dy = 2 * v2 - dcy;
    const uint8_t vv = (uint8_t)(96 + ((v2 + (int)f) % (h / 2)) * 64 / (h / 2));
    for (int u2 = 0; u2 < w / 2; u2++) {
      row[2 * u2] = ug[u2];
      row[2 * u2 + 1] = vv;
    }
    if (dy > -r && dy < r) {
      const int half = (int)std::sqrt((double)r * r - (double)dy * dy);
      for (int u2 = std::max(0, (dcx - half + 1) / 2); u2 <= std::min(w / 2 - 1, (dcx + half) / 2); u2++) {
        row[2 * u2] = 110;
        row[2 * u2 + 1] = 150;
      }
    }
  }
  // geometry markers (3 output px): white frame on the crop's left, right and bottom edges, centre cross
  const int m = std::max(2, (int)std::lround(3 * s));
  auto fill = [&](int u0, int v0, int uw, int vh) {
    for (int v = std::max(v0, strip_h); v < std::min(h, v0 + vh); v++)
      memset(Y + (cy0 + v) * ys + cx0 + std::max(0, u0), 235, std::min(w, u0 + uw) - std::max(0, u0));
  };
  fill(0, 0, m, h);
  fill(w - m, 0, m, h);
  fill(0, h - m, w, m);
  fill(w / 2 - m / 2, 0, m, h);
  fill(0, h / 2 - m / 2, w, m);
  // code strip on the crop's first rows: 80 blocks across the crop width
  uint32_t ts = (uint32_t)(capture_ns / 100000ULL);
  uint8_t bytes[10] = {0xA5, (uint8_t)(f >> 24), (uint8_t)(f >> 16), (uint8_t)(f >> 8), (uint8_t)f,
                       (uint8_t)(ts >> 24), (uint8_t)(ts >> 16), (uint8_t)(ts >> 8), (uint8_t)ts, 0};
  for (int i = 1; i < 9; i++) bytes[9] ^= bytes[i];
  for (int bit = 0; bit < 80; bit++) {
    const bool on = (bytes[bit / 8] >> (7 - bit % 8)) & 1;
    const int u0 = (int)((int64_t)bit * w / 80), u1 = (int)((int64_t)(bit + 1) * w / 80);
    for (int v = 0; v < strip_h && v < h; v++) memset(Y + (cy0 + v) * ys + cx0 + u0, on ? 235 : 16, u1 - u0);
  }
  for (int v2 = 0; v2 < (strip_h + 1) / 2 && v2 < h / 2; v2++) memset(UV + (cy0 / 2 + v2) * uvs + cx0, 128, w);
  CVPixelBufferUnlockBaseAddress(pb, 0);
}

bool synth_read(CVPixelBufferRef pb, uint32_t* f, uint32_t* ts100us) {
  CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
  const int w = (int)CVPixelBufferGetWidth(pb);
  const uint8_t* Y = (const uint8_t*)CVPixelBufferGetBaseAddressOfPlane(pb, 0);
  const size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
  const int B = w / 80;
  uint8_t bytes[10] = {0};
  for (int bit = 0; bit < 80; bit++) {
    if (Y[(B / 2) * ys + bit * B + B / 2] > 128) bytes[bit / 8] |= 1 << (7 - bit % 8);
  }
  CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
  uint8_t x = 0;
  for (int i = 1; i < 9; i++) x ^= bytes[i];
  if (bytes[0] != 0xA5 || x != bytes[9]) return false;
  *f = (uint32_t)bytes[1] << 24 | bytes[2] << 16 | bytes[3] << 8 | bytes[4];
  *ts100us = (uint32_t)bytes[5] << 24 | bytes[6] << 16 | bytes[7] << 8 | bytes[8];
  return true;
}

namespace {

class SynthSource : public Source {
 public:
  SynthSource(const SynthSpec& spec, int out_w, int out_h)
      : spec_(spec), w_(spec.w ? spec.w : out_w), h_(spec.h ? spec.h : out_h), out_w_(out_w) {
    crop_ = crop_16x9(w_, h_);
    pool_ = make_nv12_pool(w_, h_);
    start_ns_ = now_ns();
    thread_ = std::thread([this] { run(); });
  }
  ~SynthSource() override {
    {
      std::lock_guard<std::mutex> lk(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (pool_) CVPixelBufferPoolRelease(pool_);
  }
  void add(FrameListener* l) override {
    std::lock_guard<std::mutex> lk(deliver_mu_);
    listeners_.push_back(l);
  }
  void remove(FrameListener* l) override {
    std::lock_guard<std::mutex> lk(deliver_mu_);  // waits for a delivery in progress
    listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), l), listeners_.end());
  }
  std::string key() const override { return "synth"; }
  std::string native() const override { return std::to_string(w_) + "x" + std::to_string(h_) + "@30"; }
  bool lost() const override { return false; }

 private:
  void run() {
    pthread_setname_np("beamer-video-synth");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    const int64_t first = start_ns_ + (int64_t)spec_.start_ms * 1000000;
    const double period = 1e9 / 30.0;
    for (uint32_t i = 0;; i++) {
      const int64_t due = first + (int64_t)(i * period);
      {
        std::unique_lock<std::mutex> lk(mu_);
        if (stop_) return;
      }
      // absolute schedule: mach_wait_until on the host clock (== now_ns()), woken early only by stop
      while (true) {
        int64_t now = now_ns();
        if (now >= due) break;
        std::unique_lock<std::mutex> lk(mu_);
        if (stop_) return;
        int64_t left = due - now;
        if (left > 3000000) { cv_.wait_for(lk, std::chrono::nanoseconds(left - 2000000)); continue; }
        lk.unlock();
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        mach_wait_until((uint64_t)((__uint128_t)due * tb.denom / tb.numer));
      }
      if (!pool_) continue;
      CVPixelBufferRef pb = nullptr;
      if (CVPixelBufferPoolCreatePixelBuffer(nullptr, pool_, &pb) != kCVReturnSuccess || !pb) continue;
      const uint64_t c0 = thread_cpu_ns();
      // capture time = when the picture exists (after drawing, as C1/C4): the strip carries it
      synth_draw(pb, i, 0, crop_, spec_.noise, out_w_);
      const uint64_t cap = (uint64_t)now_ns();
      stamp(pb, i, cap);
      g_synth_cpu_ns += thread_cpu_ns() - c0;
      deliver(pb, i, cap);
      CVPixelBufferRelease(pb);
    }
  }
  void stamp(CVPixelBufferRef pb, uint32_t f, uint64_t cap) {
    // re-draw only the strip with the real capture time (the rest does not depend on it)
    CVPixelBufferLockBaseAddress(pb, 0);
    uint8_t* Y = (uint8_t*)CVPixelBufferGetBaseAddressOfPlane(pb, 0);
    const size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
    const int w = crop_.w, strip_h = (int)std::ceil(w / 80.0);
    uint32_t ts = (uint32_t)(cap / 100000ULL);
    uint8_t bytes[10] = {0xA5, (uint8_t)(f >> 24), (uint8_t)(f >> 16), (uint8_t)(f >> 8), (uint8_t)f,
                         (uint8_t)(ts >> 24), (uint8_t)(ts >> 16), (uint8_t)(ts >> 8), (uint8_t)ts, 0};
    for (int i = 1; i < 9; i++) bytes[9] ^= bytes[i];
    for (int bit = 0; bit < 80; bit++) {
      const bool on = (bytes[bit / 8] >> (7 - bit % 8)) & 1;
      const int u0 = (int)((int64_t)bit * w / 80), u1 = (int)((int64_t)(bit + 1) * w / 80);
      for (int v = 0; v < strip_h && v < crop_.h; v++) memset(Y + (crop_.y + v) * ys + crop_.x + u0, on ? 235 : 16, u1 - u0);
    }
    CVPixelBufferUnlockBaseAddress(pb, 0);
  }
  void deliver(CVPixelBufferRef pb, uint32_t i, uint64_t cap) {
    std::lock_guard<std::mutex> lk(deliver_mu_);
    if (listeners_.empty()) return;
    std::map<std::pair<int, int>, CVPixelBufferRef> memo;
    Frame fr;
    fr.native = pb;
    fr.capture_ns = cap;
    fr.index = i;
    fr.scaled = [&](int w, int h, double* ms) -> CVPixelBufferRef {
      if (ms) *ms = 0;
      if (w == w_ && h == h_) return pb;
      auto key = std::make_pair(w, h);
      auto it = memo.find(key);
      if (it != memo.end()) return it->second;
      auto& sc = scalers_[key];
      if (!sc) sc = std::make_unique<Scaler>(w, h);
      std::string err;
      CVPixelBufferRef out = sc->convert(pb, crop_, &err);
      if (!out && !err.empty()) vlog("warn", "synthetic camera scaler: %s", err.c_str());
      if (ms) *ms = sc->last_ms();
      memo[key] = out;
      return out;
    };
    auto ls = listeners_;
    for (FrameListener* l : ls) l->on_captured(fr);
    for (auto& kv : memo)
      if (kv.second) CVPixelBufferRelease(kv.second);
  }

  SynthSpec spec_;
  int w_, h_, out_w_;
  CropRect crop_;
  CVPixelBufferPoolRef pool_ = nullptr;
  int64_t start_ns_ = 0;
  std::mutex mu_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::mutex deliver_mu_;
  std::vector<FrameListener*> listeners_;
  std::map<std::pair<int, int>, std::unique_ptr<Scaler>> scalers_;  // delivery thread only
  std::thread thread_;
};

}  // namespace

std::shared_ptr<Source> make_synth_source(const SynthSpec& spec, int out_w, int out_h) {
  return std::make_shared<SynthSource>(spec, out_w, out_h);
}

}  // namespace beamer::audio::macvideo
