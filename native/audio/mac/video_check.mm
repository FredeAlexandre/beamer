// SPDX-License-Identifier: GPL-3.0-only
// video_check: CW3's local harness for the macOS video backend alone (no engine; camera PHASE2 §10.4, §12 CW3).
// It plays the engine's part: numbers AUs in on_frame (0, 1, 2...), acknowledges LTRs, requests refreshes,
// and decodes the stream back with VideoToolbox to check sizes, crop geometry and the code strip. Results are
// JSON lines on stdout; the backend logs on stderr.
//
//   video_check selftest                       pure rules (crop, format choice, synth spec), clock, strip
//   video_check cameras                        inventory + permission (never prompts)
//   video_check run [--test SPEC | --camera KEY|default] [--size WxH] [--bitrate BPS] [--seconds S]
//                   [--start-after-ms N] [--verify] [--standby] [--bitrate-at S:BPS] [--quiet]
//   video_check ltr [--size WxH]               LTR token map across pause and set_format; refresh vs IDR
//   video_check warm [--n N]                   cold / warm / standby open -> first AU
//   video_check idle --mode session|prewarm --minutes 1,10,60
//   video_check idlecost --mode session|prewarm|none --minutes M
//   video_check guard [--disclaim]             real-camera open without permission fails `permission`, no prompt
//
// --disclaim re-runs the command in a child that is its own TCC responsible process (posix_spawn with
// responsibility disclaimed), so the camera permission is this binary's own (not_determined), not the
// agent host's. Nothing here ever calls requestAccessForMediaType.
#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

#include <dlfcn.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "video_backend_mac.h"

using namespace beamer::audio;
using namespace beamer::audio::macvideo;

extern char** environ;

namespace {

// ------------------------------------------------------------------------------------------- utils
double pct(std::vector<double> v, double p) {
  if (v.empty()) return -1;
  std::sort(v.begin(), v.end());
  size_t i = (size_t)std::ceil(p / 100.0 * v.size());
  if (i > 0) i--;
  return v[std::min(i, v.size() - 1)];
}
double mean(const std::vector<double>& v) {
  double s = 0;
  for (double x : v) s += x;
  return v.empty() ? -1 : s / v.size();
}
double cpu_s() {
  rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}
double rss_mb() {
  mach_task_basic_info_data_t info;
  mach_msg_type_number_t n = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &n) != KERN_SUCCESS) return -1;
  return info.resident_size / 1048576.0;
}
double phys_mb() {  // phys_footprint: what Activity Monitor calls Memory
  task_vm_info_data_t vi;
  mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &n) != KERN_SUCCESS) return -1;
  return vi.phys_footprint / 1048576.0;
}
void sleep_ms(double ms) { std::this_thread::sleep_for(std::chrono::microseconds((int64_t)(ms * 1000))); }
std::string jnum(double v, int digits = 2) {
  char b[64];
  snprintf(b, sizeof b, "%.*f", digits, v);
  return b;
}
std::string jstr(const std::string& s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    if ((unsigned char)c < 0x20) { o += ' '; continue; }
    o += c;
  }
  return o + "\"";
}
struct J {  // tiny JSON object builder
  std::string s = "{";
  J& k(const std::string& key) { if (s.size() > 1) s += ","; s += jstr(key) + ":"; return *this; }
  J& str(const std::string& key, const std::string& v) { return k(key), s += jstr(v), *this; }
  J& num(const std::string& key, double v, int d = 2) { return k(key), s += jnum(v, d), *this; }
  J& i(const std::string& key, long long v) { return k(key), s += std::to_string(v), *this; }
  J& b(const std::string& key, bool v) { return k(key), s += v ? "true" : "false", *this; }
  J& raw(const std::string& key, const std::string& v) { return k(key), s += v, *this; }
  std::string done() const { return s + "}"; }
};
J dist(const std::vector<double>& v) {
  J j;
  j.i("n", (long long)v.size());
  if (!v.empty()) {
    j.num("p50", pct(v, 50), 3).num("p95", pct(v, 95), 3).num("max", *std::max_element(v.begin(), v.end()), 3);
    j.num("mean", mean(v), 3);
  }
  return j;
}
void out(const J& j) {
  printf("%s\n", j.done().c_str());
  fflush(stdout);
}
bool parse_wh(const std::string& s, int* w, int* h) { return sscanf(s.c_str(), "%dx%d", w, h) == 2; }

// ------------------------------------------------------------------------------------------- decoder
// Annex-B AU -> VTDecompressionSession (synchronous) -> NV12 picture to `on_picture`.
class Decoder {
 public:
  ~Decoder() { reset(); }
  std::function<void(CVPixelBufferRef, uint32_t)> on_picture;
  int errors = 0, pictures = 0;
  // returns false on a decode error
  bool decode(const uint8_t* p, size_t n, uint32_t tag) {
    std::vector<std::pair<const uint8_t*, size_t>> nals;
    split(p, n, nals);
    const uint8_t *sps = nullptr, *pps = nullptr;
    size_t spsn = 0, ppsn = 0;
    std::vector<uint8_t> avcc;
    for (auto& nal : nals) {
      int type = nal.first[0] & 0x1f;
      if (type == 7) { sps = nal.first; spsn = nal.second; continue; }
      if (type == 8) { pps = nal.first; ppsn = nal.second; continue; }
      uint32_t len = (uint32_t)nal.second;
      uint8_t hdr[4] = {(uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len};
      avcc.insert(avcc.end(), hdr, hdr + 4);
      avcc.insert(avcc.end(), nal.first, nal.first + nal.second);
    }
    if (sps && pps) {
      const uint8_t* ps[2] = {sps, pps};
      size_t pn[2] = {spsn, ppsn};
      CMVideoFormatDescriptionRef f = nullptr;
      if (CMVideoFormatDescriptionCreateFromH264ParameterSets(nullptr, 2, ps, pn, 4, &f) != noErr) { errors++; return false; }
      if (!fmt_ || !CMFormatDescriptionEqual(f, fmt_)) {
        reset();
        fmt_ = f;
        NSDictionary* attrs = @{(id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)};
        VTDecompressionOutputCallbackRecord cb = {cb_, this};
        if (VTDecompressionSessionCreate(nullptr, fmt_, nullptr, (__bridge CFDictionaryRef)attrs, &cb, &s_) != noErr) {
          errors++;
          return false;
        }
      } else {
        CFRelease(f);
      }
    }
    if (!s_ || avcc.empty()) { errors++; return false; }
    CMBlockBufferRef bb = nullptr;
    void* mem = malloc(avcc.size());
    memcpy(mem, avcc.data(), avcc.size());
    CMBlockBufferCreateWithMemoryBlock(nullptr, mem, avcc.size(), kCFAllocatorMalloc, nullptr, 0, avcc.size(), 0, &bb);
    CMSampleBufferRef sb = nullptr;
    size_t sz = avcc.size();
    CMSampleBufferCreateReady(nullptr, bb, fmt_, 1, 0, nullptr, 1, &sz, &sb);
    CFRelease(bb);
    VTDecodeInfoFlags fl = 0;
    last_ok_ = false;
    tag_ = tag;
    OSStatus st = VTDecompressionSessionDecodeFrame(s_, sb, 0, nullptr, &fl);
    CFRelease(sb);
    if (st != noErr || !last_ok_) { errors++; return false; }
    return true;
  }
  CMVideoDimensions dims() const { return fmt_ ? CMVideoFormatDescriptionGetDimensions(fmt_) : CMVideoDimensions{0, 0}; }

 private:
  static void cb_(void* ctx, void*, OSStatus st, VTDecodeInfoFlags, CVImageBufferRef img, CMTime, CMTime) {
    auto* d = (Decoder*)ctx;
    if (st != noErr || !img) return;
    d->last_ok_ = true;
    d->pictures++;
    if (d->on_picture) d->on_picture(img, d->tag_);
  }
  static void split(const uint8_t* p, size_t n, std::vector<std::pair<const uint8_t*, size_t>>& o) {
    size_t i = 0, start = SIZE_MAX;
    while (i + 3 <= n) {
      if (p[i] == 0 && p[i + 1] == 0 && (p[i + 2] == 1 || (i + 3 < n && p[i + 2] == 0 && p[i + 3] == 1))) {
        size_t sc = p[i + 2] == 1 ? 3 : 4;
        if (start != SIZE_MAX) o.push_back({p + start, i - start});
        i += sc;
        start = i;
      } else {
        i++;
      }
    }
    if (start != SIZE_MAX && start < n) o.push_back({p + start, n - start});
  }
  void reset() {
    if (s_) { VTDecompressionSessionInvalidate(s_); CFRelease(s_); s_ = nullptr; }
    if (fmt_) { CFRelease(fmt_); fmt_ = nullptr; }
  }
  VTDecompressionSessionRef s_ = nullptr;
  CMVideoFormatDescriptionRef fmt_ = nullptr;
  bool last_ok_ = false;
  uint32_t tag_ = 0;
};

// Geometry of a decoded synthetic picture (synth_draw markers), in output pixels; errors vs. expected.
struct Geo {
  bool strip_ok = false;
  uint32_t strip_frame = 0;
  int left = -1, right = -1, bottom = -1, top = -1, cx = -1, cy = -1;
  int w = 0, h = 0;
  int max_err() const {
    if (!strip_ok || left < 0 || right < 0 || bottom < 0 || top < 0 || cx < 0 || cy < 0) return 9999;
    int e = 0;
    e = std::max(e, std::abs(left - 0));
    e = std::max(e, std::abs(right - (w - 1)));
    e = std::max(e, std::abs(bottom - (h - 1)));
    e = std::max(e, std::abs(top - 0));
    e = std::max(e, std::abs(cx - w / 2));
    e = std::max(e, std::abs(cy - h / 2));
    return e;
  }
};
Geo measure(CVPixelBufferRef pb) {
  Geo g;
  uint32_t ts = 0;
  g.strip_ok = synth_read(pb, &g.strip_frame, &ts);
  CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
  const int w = (int)CVPixelBufferGetWidth(pb), h = (int)CVPixelBufferGetHeight(pb);
  const uint8_t* Y = (const uint8_t*)CVPixelBufferGetBaseAddressOfPlane(pb, 0);
  const size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
  g.w = w;
  g.h = h;
  const int T = 215;
  auto at = [&](int x, int y) { return (int)Y[y * ys + x]; };
  const int row = h * 62 / 100, col = w * 62 / 100;
  for (int x = 0; x < w; x++) if (at(x, row) > T) { g.left = x; break; }
  for (int x = w - 1; x >= 0; x--) if (at(x, row) > T) { g.right = x; break; }
  for (int y = h - 1; y >= 0; y--) if (at(col, y) > T) { g.bottom = y; break; }
  const int B = w / 80;
  for (int y = 0; y < h; y++) if (at(B / 2, y) > T) { g.top = y; break; }  // bit 0 of 0xA5 is on
  // centre cross: white run within the middle quarter (clear of the text panel)
  auto run_center = [&](bool horizontal) {
    int lo = -1, hi = -1;
    const int n = horizontal ? w : h;
    for (int i = n / 2 - n / 8; i < n / 2 + n / 8; i++) {
      int v = horizontal ? at(i, row) : at(col, i);
      if (v > T) { if (lo < 0) lo = i; hi = i; }
    }
    return lo < 0 ? -1 : (lo + hi + 1) / 2;
  };
  g.cx = run_center(true);
  g.cy = run_center(false);
  CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
  return g;
}
// ------------------------------------------------------------------------------------------- engine stand-in
struct Au {
  uint32_t n = 0;
  size_t size = 0;
  bool idr = false, refresh = false, ltr = false;
  int w = 0, h = 0;
  uint64_t capture_ns = 0, rx_ns = 0;
  std::vector<uint8_t> data;
};
class Sink : public EncoderSink {
 public:
  bool keep_data = false;
  bool print_events = true;
  std::function<void(const Au&)> hook;  // called outside the lock, on the encoder output thread
  uint32_t on_frame(const EncodedFrame& f) override {
    Au a;
    a.rx_ns = (uint64_t)now_ns();
    a.size = f.len;
    a.idr = f.idr;
    a.refresh = f.refresh;
    a.ltr = f.ltr;
    a.w = f.w;
    a.h = f.h;
    a.capture_ns = f.capture_ns;
    if (keep_data) a.data.assign(f.data, f.data + f.len);
    {
      std::lock_guard<std::mutex> lk(mu);
      a.n = next++;
      aus.push_back(a);
    }
    cv.notify_all();
    if (hook) hook(a);
    return a.n;
  }
  void on_camera_event(const std::string& what, const std::string& detail) override {
    std::lock_guard<std::mutex> lk(mu);
    events.push_back({what, detail});
    if (print_events && what != "fps") out(J().str("ev", "camera_event").str("what", what).str("detail", detail));
    if (what == "fps") fps.push_back(atof(detail.c_str()));
  }
  bool wait_aus(size_t n, double timeout_s) {
    std::unique_lock<std::mutex> lk(mu);
    return cv.wait_for(lk, std::chrono::milliseconds((int64_t)(timeout_s * 1000)), [&] { return aus.size() >= n; });
  }
  std::string event(const std::string& what) {
    std::lock_guard<std::mutex> lk(mu);
    for (auto& e : events) if (e.first == what) return e.second;
    return "";
  }
  std::mutex mu;
  std::condition_variable cv;
  uint32_t next = 0;
  std::deque<Au> aus;
  std::vector<std::pair<std::string, std::string>> events;
  std::vector<double> fps;
};

struct Listener : VideoBackendListener {
  std::atomic<int> cams{0}, perms{0}, lost{0};
  void cameras_changed() override { cams++; }
  void vcam_changed() override {}
  void permission_changed() override { perms++; }
  void camera_lost(CameraHandle*, const std::string&) override { lost++; }
};

std::unique_ptr<VideoBackend> make_backend(const std::string& extra = "") {
  return make_mac_video_backend("dev" + (extra.empty() ? "" : ";" + extra));
}

std::string arg(int argc, char** argv, const char* name, const char* def) {
  for (int i = 2; i + 1 < argc; i++)
    if (!strcmp(argv[i], name)) return argv[i + 1];
  return def;
}
bool flag(int argc, char** argv, const char* name) {
  for (int i = 2; i < argc; i++)
    if (!strcmp(argv[i], name)) return true;
  return false;
}

// ------------------------------------------------------------------------------------------- selftest
int cmd_selftest() {
  int fails = 0;
  auto check = [&](bool ok, const std::string& what) {
    if (!ok) fails++;
    out(J().str("ev", "check").str("what", what).b("ok", ok));
  };
  // clock: engine now_ns (steady_clock) == mach host time in ns
  {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    int64_t a = now_ns();
    int64_t h = (int64_t)((__uint128_t)mach_absolute_time() * tb.numer / tb.denom);
    int64_t b = now_ns();
    check(h >= a - 50000 && h <= b + 50000, "steady_clock == mach host clock (diff " + std::to_string((h - a) / 1000) + " us)");
    CMTime ht = CMClockGetTime(CMClockGetHostTimeClock());
    int64_t c = host_time_to_ns(ht), d2 = now_ns();
    check(std::llabs(d2 - c) < 1000000, "host_time_to_ns(CMClockGetHostTimeClock) ~ now_ns");
  }
  // §4.11 crop examples
  struct C { int w, h, cw, ch, x, y; };
  for (C c : {C{1920, 1440, 1920, 1080, 0, 180}, C{1760, 1328, 1760, 990, 0, 168}, C{1552, 1552, 1552, 872, 0, 340},
              C{1080, 1920, 1080, 606, 0, 656}, C{640, 480, 640, 360, 0, 60}, C{1280, 720, 1280, 720, 0, 0},
              C{1920, 1080, 1920, 1080, 0, 0}, C{2560, 1080, 1920, 1080, 320, 0}}) {
    CropRect r = crop_16x9(c.w, c.h);
    check(r.w == c.cw && r.h == c.ch && r.x == c.x && r.y == c.y,
          "crop " + std::to_string(c.w) + "x" + std::to_string(c.h) + " -> " + std::to_string(r.w) + "x" +
              std::to_string(r.h) + "@" + std::to_string(r.x) + "," + std::to_string(r.y));
  }
  // §4.11 format choice
  const uint32_t v = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, f = kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
  std::vector<FormatCand> mba = {{1920, 1440, 30, v}, {1280, 720, 30, v}, {1552, 1552, 30, v}, {1760, 1328, 30, v},
                                 {1080, 1920, 30, v}, {1920, 1080, 30, v}, {640, 480, 30, v}};
  check(choose_format(mba, 1280, 720) == 1, "format 720p: exact 1280x720");
  check(choose_format(mba, 1920, 1080) == 5, "format 1080p: exact 1920x1080");
  std::vector<FormatCand> noexact = {{1920, 1440, 30, v}, {1760, 1328, 30, v}, {640, 480, 30, v}, {1080, 1920, 30, v}};
  check(choose_format(noexact, 1280, 720) == 1, "format 720p without exact: smallest landscape covering (1760x1328)");
  check(choose_format(noexact, 1920, 1080) == 0, "format 1080p without exact: 1920x1440");
  std::vector<FormatCand> small = {{640, 480, 30, v}, {1024, 576, 30, v}, {1080, 1920, 30, v}};
  check(choose_format(small, 1920, 1080) == 1, "format: none covers -> landscape with the largest crop");
  std::vector<FormatCand> portrait = {{1080, 1920, 30, v}, {720, 1280, 30, v}};
  check(choose_format(portrait, 1280, 720) == 0, "format: portrait only -> largest crop");
  std::vector<FormatCand> slow = {{1280, 720, 15, v}, {1920, 1080, 30, v}};
  check(choose_format(slow, 1280, 720) == 1, "format: 30 fps formats first (1080p30 over 720p15)");
  std::vector<FormatCand> fr = {{1280, 720, 30, f}, {1280, 720, 30, v}};
  check(choose_format(fr, 1280, 720) == 1, "format: 420v before 420f");
  std::vector<FormatCand> allslow = {{1280, 720, 15, v}, {1920, 1080, 10, v}};
  check(choose_format(allslow, 1920, 1080) == 0, "format: none at 30 -> the highest-rate ones");
  // synth spec
  SynthSpec ss;
  std::string e;
  check(parse_synth("synth", &ss, &e) && ss.w == 0 && ss.start_ms == 5, "synth default");
  check(parse_synth("synth:1920x1440", &ss, &e) && ss.w == 1920 && ss.h == 1440, "synth:1920x1440");
  check(parse_synth("synth:1280x720;start_ms=800", &ss, &e) && ss.w == 1280 && ss.start_ms == 800, "synth:1280x720;start_ms=800");
  check(parse_synth("synth;start_ms=800", &ss, &e) && ss.w == 0 && ss.start_ms == 800, "synth;start_ms=800");
  check(!parse_synth("synth:12x", &ss, &e), "synth:12x refused");
  check(!parse_synth("camera", &ss, &e), "camera refused");
  check(!parse_synth("synth;start_ms=x", &ss, &e), "bad start_ms refused");
  // strip round trip at output size, no scaling
  {
    CVPixelBufferPoolRef pool = make_nv12_pool(1280, 720);
    CVPixelBufferRef pb = nullptr;
    CVPixelBufferPoolCreatePixelBuffer(nullptr, pool, &pb);
    synth_draw(pb, 1234567, 98765432100000ULL, crop_16x9(1280, 720), 3, 1280);
    uint32_t fr2 = 0, ts = 0;
    check(synth_read(pb, &fr2, &ts) && fr2 == 1234567 && ts == (uint32_t)(98765432100000ULL / 100000), "strip round trip");
    Geo g = measure(pb);
    check(g.max_err() <= 0, "geometry markers on an unscaled picture (max err " + std::to_string(g.max_err()) + ")");
    CVPixelBufferRelease(pb);
    CVPixelBufferPoolRelease(pool);
  }
  // the geometry check through the real scaler, and its sensitivity: a picture drawn for a crop 6 native px
  // off must be caught (> 2 px), the right one must pass
  for (int off : {0, 6}) {
    CVPixelBufferPoolRef pool = make_nv12_pool(1920, 1440);
    CVPixelBufferRef pb = nullptr;
    CVPixelBufferPoolCreatePixelBuffer(nullptr, pool, &pb);
    CropRect c = crop_16x9(1920, 1440), drawn = c;
    drawn.y += off;
    synth_draw(pb, 7, 0, drawn, 3, 1280);
    Scaler sc(1280, 720);
    std::string err;
    CVPixelBufferRef o = sc.convert(pb, c, &err);
    Geo g = o ? measure(o) : Geo();
    check(o && (off == 0 ? g.max_err() <= 2 : g.max_err() > 2),
          "scaled geometry, picture drawn " + std::to_string(off) + " px off: max err " + std::to_string(g.max_err()));
    if (o) CVPixelBufferRelease(o);
    CVPixelBufferRelease(pb);
    CVPixelBufferPoolRelease(pool);
  }
  // cost of the synthetic camera's drawing (the stand-in for a camera, not part of the pipeline)
  for (auto wh : {std::make_pair(1280, 720), std::make_pair(1920, 1080), std::make_pair(1920, 1440)}) {
    CVPixelBufferPoolRef pool = make_nv12_pool(wh.first, wh.second);
    CVPixelBufferRef pb = nullptr;
    CVPixelBufferPoolCreatePixelBuffer(nullptr, pool, &pb);
    std::vector<double> ms;
    for (uint32_t i = 0; i < 60; i++) {
      int64_t t0 = now_ns();
      synth_draw(pb, i, 0, crop_16x9(wh.first, wh.second), 3, 1280);
      ms.push_back((now_ns() - t0) / 1e6);
    }
    out(J().str("ev", "synth_draw").str("size", std::to_string(wh.first) + "x" + std::to_string(wh.second))
            .raw("ms", dist(ms).done()).num("core_pct_at_30fps", pct(ms, 50) * 30 / 10, 1));
    CVPixelBufferRelease(pb);
    CVPixelBufferPoolRelease(pool);
  }
  check(capture_device_creations() == 0, "no capture device created");
  out(J().str("ev", "selftest").i("fails", fails));
  return fails ? 1 : 0;
}

// ------------------------------------------------------------------------------------------- cameras
int cmd_cameras() {
  auto be = make_backend();
  CameraSnapshot s = be->cameras();
  std::string cams = "[";
  for (auto& c : s.cameras) {
    std::string nat = "[";
    for (auto& f : c.native) nat += (nat.size() > 1 ? "," : "") + jstr(std::to_string(f.w) + "x" + std::to_string(f.h) + "@" + std::to_string(f.fps));
    nat += "]";
    cams += (cams.size() > 1 ? "," : "") + J().str("key", c.key).str("name", c.name).str("transport", c.transport)
                                                .raw("native", nat).b("preferred", c.preferred).done();
  }
  cams += "]";
  std::string feats = "[";
  for (auto& f : be->features()) feats += (feats.size() > 1 ? "," : "") + jstr(f);
  feats += "]";
  out(J().str("ev", "cameras").raw("cameras", cams).str("preferred", s.preferred).str("permission", s.permission)
          .raw("features", feats).i("capture_device_creations", capture_device_creations()));
  return 0;
}

// ------------------------------------------------------------------------------------------- run
struct RunOpts {
  std::string test = "synth", camera;
  int w = 1280, h = 720, bitrate = 2500000;
  double seconds = 10, start_after_ms = 0;
  bool verify = false, standby = false, quiet = false;
  std::vector<std::pair<double, int>> bitrate_at;
};

int cmd_run(const RunOpts& o) {
  auto be = make_backend();
  Listener lis;
  be->set_listener(&lis);
  if (o.standby) {
    be->set_encoder_standby(EncoderStandby{true, VideoFormat{o.w, o.h, 30}, o.bitrate});
    sleep_ms(400);
  }
  Sink sink;
  sink.print_events = !o.quiet;
  Decoder dec;
  std::mutex geo_mu;
  std::vector<int> geo_err;
  Geo worst;
  int strip_ok = 0, strip_bad = 0, bad_size = 0;
  if (o.verify) {
    sink.keep_data = true;
    dec.on_picture = [&](CVPixelBufferRef pb, uint32_t n) {
      if ((int)CVPixelBufferGetWidth(pb) != o.w || (int)CVPixelBufferGetHeight(pb) != o.h) { bad_size++; return; }
      Geo g = measure(pb);
      std::lock_guard<std::mutex> lk(geo_mu);
      if (g.strip_ok) strip_ok++; else strip_bad++;
      if (n % 15 == 0 || g.max_err() > 2) {
        geo_err.push_back(g.max_err());
        if (g.max_err() >= worst.max_err() || geo_err.size() == 1) worst = g;
      }
    };
    sink.hook = [&](const Au& a) { dec.decode(a.data.data(), a.data.size(), a.n); };
  }
  CameraSpec spec;
  spec.fmt = VideoFormat{o.w, o.h, 30};
  spec.bitrate = o.bitrate;
  if (o.camera.empty()) spec.test = o.test;
  else spec.device = o.camera == "default" ? "" : o.camera;
  take_scale_times();
  take_encode_times();
  const double cpu0 = cpu_s();
  const uint64_t synth0 = g_synth_cpu_ns.load();
  const int64_t t_open = now_ns();
  std::string err;
  auto h = be->open_camera(spec, &sink, &err);
  const double open_ms = (now_ns() - t_open) / 1e6;
  if (!h) {
    out(J().str("ev", "run").b("ok", false).str("error", err).str("permission", camera_permission())
            .i("capture_device_creations", capture_device_creations()).i("permission_events", lis.perms.load()));
    return 2;
  }
  if (o.start_after_ms > 0) sleep_ms(o.start_after_ms);
  const int64_t t_start = now_ns();
  h->start_encoding();
  const bool got = sink.wait_aus(1, 5);
  const int64_t t_first = got ? (int64_t)sink.aus.front().rx_ns : 0;
  // live bitrate changes
  std::vector<std::pair<int64_t, int>> changes;
  const int64_t t_run0 = now_ns();
  for (auto& ba : o.bitrate_at) {
    double at = ba.first * 1e9 - (now_ns() - t_run0);
    if (at > 0) sleep_ms(at / 1e6);
    changes.push_back({now_ns(), ba.second});
    h->set_bitrate(ba.second);
  }
  double left = o.seconds * 1e3 - (now_ns() - t_run0) / 1e6;
  if (left > 0) sleep_ms(left);
  const double cpu = cpu_s() - cpu0;
  const double synth_cpu = (g_synth_cpu_ns.load() - synth0) / 1e9;
  const double wall = (now_ns() - t_open) / 1e9;
  const double rss = rss_mb(), phys = phys_mb();
  const int64_t t_stop = now_ns();
  h.reset();
  const double stop_ms = (now_ns() - t_stop) / 1e6;
  // capture session stop (real camera): wait until no session runs
  double cap_stop_ms = -1;
  if (!o.camera.empty()) {
    for (int i = 0; i < 300 && capture_sessions_running() > 0; i++) sleep_ms(5);
    cap_stop_ms = (now_ns() - t_stop) / 1e6;
  }
  // stats
  std::vector<double> enc_delay, sizes_p, sizes_i;
  int idr = 0, refresh = 0, ltr = 0, wrong_size = 0;
  std::map<int, double> win;  // 1 s windows by capture time since the first AU
  const std::string ev_native = sink.event("native"), ev_encoder = sink.event("encoder"), ev_cam = sink.event("camera_ms");
  std::lock_guard<std::mutex> lk(sink.mu);
  const uint64_t cap0 = sink.aus.empty() ? 0 : sink.aus.front().capture_ns;
  for (auto& a : sink.aus) {
    enc_delay.push_back((a.rx_ns - a.capture_ns) / 1e6);
    if (a.idr) { idr++; sizes_i.push_back(a.size); } else sizes_p.push_back(a.size);
    refresh += a.refresh;
    ltr += a.ltr;
    if (a.w != o.w || a.h != o.h) wrong_size++;
    win[(int)((a.capture_ns - cap0) / 1000000000ULL)] += a.size * 8.0 / 1000.0;
  }
  std::vector<double> w1;
  int last = win.empty() ? 0 : win.rbegin()->first;
  for (auto& kv : win) if (kv.first >= 1 && kv.first < last) w1.push_back(kv.second);  // full windows after the IDR's
  double w1_dev = 0;
  for (double x : w1) w1_dev = std::max(w1_dev, std::fabs(x * 1000 / o.bitrate - 1) * 100);
  // bitrate change: first 300 ms window (from the change) whose rate is within 20 % of the new target
  std::string change_s = "[";
  for (auto& c : changes) {
    double eff = -1;
    for (int k = 0; k < 40; k++) {
      uint64_t a0 = c.first + k * 50000000ULL, a1 = a0 + 300000000ULL;
      double bits = 0;
      int nfr = 0;
      for (auto& a : sink.aus) if (!a.idr && a.rx_ns >= a0 && a.rx_ns < a1) { bits += a.size * 8.0; nfr++; }
      if (nfr < 5) continue;
      double rate = bits / 0.3;
      if (std::fabs(rate / c.second - 1) <= 0.2) { eff = k * 50 + 300; break; }
    }
    change_s += (change_s.size() > 1 ? "," : "") + J().i("bps", c.second).num("effective_within_ms", eff, 0).done();
  }
  change_s += "]";
  std::vector<double> sc = take_scale_times(), en = take_encode_times();
  J j;
  j.str("ev", "run").b("ok", got).str("source", o.camera.empty() ? o.test : "camera:" + o.camera)
      .str("size", std::to_string(o.w) + "x" + std::to_string(o.h)).i("bitrate", o.bitrate)
      .str("native", ev_native).str("encoder", ev_encoder).str("camera_ms", ev_cam)
      .num("open_ms", open_ms).num("first_au_after_start_ms", got ? (t_first - t_start) / 1e6 : -1)
      .num("first_au_after_open_ms", got ? (t_first - t_open) / 1e6 : -1)
      .i("aus", (long long)sink.aus.size()).i("idr", idr).i("refresh", refresh).i("ltr", ltr).i("wrong_size", wrong_size)
      .raw("enc_delay_ms", dist(enc_delay).done()).raw("encode_ms", dist(en).done()).raw("scale_ms", dist(sc).done())
      .raw("kbps_1s", dist(w1).done()).num("kbps_1s_max_dev_pct", w1_dev, 1)
      .raw("idr_bytes", dist(sizes_i).done()).raw("p_bytes", dist(sizes_p).done())
      .raw("bitrate_changes", change_s)
      .num("cpu_pct", cpu / wall * 100, 2).num("cpu_pct_excl_synth", (cpu - synth_cpu) / wall * 100, 2)
      .num("rss_mb", rss, 1).num("phys_mb", phys, 1).num("fps_mean", mean(sink.fps), 1)
      .num("stop_ms", stop_ms, 2).num("encoder_invalidate_ms", last_stop_ms(), 2).num("capture_stop_ms", cap_stop_ms, 1)
      .i("capture_device_creations", capture_device_creations()).str("permission", camera_permission());
  if (o.verify) {
    std::lock_guard<std::mutex> lk2(geo_mu);
    j.i("decoded", dec.pictures).i("decode_errors", dec.errors).i("decoded_wrong_size", bad_size)
        .i("strip_ok", strip_ok).i("strip_bad", strip_bad)
        .num("geo_max_err_px", geo_err.empty() ? 9999 : *std::max_element(geo_err.begin(), geo_err.end()), 0)
        .raw("geo_worst", J().i("left", worst.left).i("right", worst.right).i("top", worst.top).i("bottom", worst.bottom)
                              .i("cx", worst.cx).i("cy", worst.cy).i("w", worst.w).i("h", worst.h).done());
  }
  out(j);
  return got ? 0 : 1;
}

// ------------------------------------------------------------------------------------------- ltr
// The engine's verified-LTR loop against the real encoder: a receiver that decodes everything it gets,
// acknowledges every LTR it decoded inside its clean run (3 frames later, from another thread, as the RR
// path does), loses frames on demand, and asks for a refresh. A wrong frame -> token mapping shows as a
// refresh that does not decode, or decodes to the wrong picture (strip frame / PSNR).
int cmd_ltr(int w0, int h0) {
  auto be = make_backend();
  Sink sink;
  sink.keep_data = true;
  sink.print_events = false;
  Decoder dec;
  struct Rx {
    std::mutex mu;
    std::set<uint32_t> drop;  // frames the receiver never gets
    bool broken = true;
    std::deque<std::pair<uint32_t, uint32_t>> acks;  // (ltr frame, due at frame)
    std::vector<std::string> log;
    int refresh_decoded = 0, refresh_bad = 0, refresh_idr = 0, refresh_ok = 0;
    std::vector<std::pair<uint32_t, size_t>> refresh_sizes;
    std::map<uint32_t, uint32_t> strip_of;  // frame -> strip number of the decoded picture
  } rx;
  CameraHandle* hp = nullptr;
  std::mutex ack_mu;
  std::condition_variable ack_cv;
  std::deque<uint32_t> ack_q;
  bool ack_stop = false;
  std::thread acker([&] {  // the engine's network thread
    std::unique_lock<std::mutex> lk(ack_mu);
    while (true) {
      ack_cv.wait(lk, [&] { return ack_stop || !ack_q.empty(); });
      if (ack_stop) return;
      uint32_t f = ack_q.front();
      ack_q.pop_front();
      lk.unlock();
      if (hp) hp->acknowledge_ltr(f);
      lk.lock();
    }
  });
  dec.on_picture = [&](CVPixelBufferRef pb, uint32_t n) {
    uint32_t sf = 0, ts = 0;
    if (synth_read(pb, &sf, &ts)) rx.strip_of[n] = sf;
  };
  // capture index of each AU's picture: the strip of its source is the synth frame number; record what we
  // submitted by matching capture times is not needed: decoded strip numbers must be strictly increasing
  // and continuous with the previous clean picture.
  uint32_t last_strip = 0;
  bool have_strip = false;
  sink.hook = [&](const Au& a) {
    std::lock_guard<std::mutex> lk(rx.mu);
    while (!rx.acks.empty() && rx.acks.front().second <= a.n) {
      {
        std::lock_guard<std::mutex> lk2(ack_mu);
        ack_q.push_back(rx.acks.front().first);
      }
      ack_cv.notify_one();
      rx.acks.pop_front();
    }
    if (rx.drop.count(a.n)) {
      rx.broken = true;
      return;
    }
    const bool ok = dec.decode(a.data.data(), a.data.size(), a.n);
    auto it = rx.strip_of.find(a.n);
    const bool strip = it != rx.strip_of.end();
    if (a.refresh || (a.idr && a.n > 0)) {
      rx.refresh_sizes.push_back({a.n, a.size});
      if (a.idr) rx.refresh_idr++;
      // a refresh after a loss must decode to its own (newer) picture
      if (ok && strip && (!have_strip || it->second > last_strip)) rx.refresh_ok++;
      else rx.refresh_bad++;
    }
    if (a.idr || a.refresh) rx.broken = !(ok && strip);
    else if (!ok) rx.broken = true;
    if (ok && strip) {
      if (!rx.broken && have_strip && it->second <= last_strip) rx.broken = true;
      last_strip = it->second;
      have_strip = true;
    }
    if (a.ltr && !rx.broken) rx.acks.push_back({a.n, a.n + 3});
  };
  CameraSpec spec;
  spec.fmt = VideoFormat{w0, h0, 30};
  spec.bitrate = w0 >= 1920 ? 5000000 : 2500000;
  spec.test = "synth";
  std::string err;
  auto h = be->open_camera(spec, &sink, &err);
  if (!h) { out(J().str("ev", "ltr").b("ok", false).str("error", err)); return 2; }
  hp = h.get();
  h->start_encoding();
  auto n_now = [&] { std::lock_guard<std::mutex> lk(sink.mu); return sink.next; };
  auto au_at = [&](uint32_t n) { std::lock_guard<std::mutex> lk(sink.mu); return sink.aus[n]; };
  int fails = 0;
  auto check = [&](bool ok, const std::string& what) {
    if (!ok) fails++;
    out(J().str("ev", "check").str("what", what).b("ok", ok));
  };
  auto lose_and_refresh = [&](const std::string& label, int lose) -> uint32_t {
    uint32_t from = n_now() + 1;
    {
      std::lock_guard<std::mutex> lk(rx.mu);
      for (int i = 0; i < lose; i++) rx.drop.insert(from + i);
    }
    sink.wait_aus(from + lose + 1, 3);
    uint32_t req_at = n_now();
    h->request_ltr_refresh();
    sink.wait_aus(req_at + 3, 3);
    // the first refresh/IDR after the request
    uint32_t got = UINT32_MAX;
    for (uint32_t n = req_at; n < n_now(); n++) {
      Au a = au_at(n);
      if (a.refresh || a.idr) { got = n; break; }
    }
    Au a = got == UINT32_MAX ? Au() : au_at(got);
    out(J().str("ev", "refresh").str("case", label).i("lost_from", from).i("lost", lose).i("frame", got)
            .b("idr", a.idr).b("refresh", a.refresh).i("bytes", (long long)a.size).i("w", a.w).i("h", a.h));
    return got;
  };
  // 1. without any ack: a refresh request is an IDR
  sink.wait_aus(2, 3);
  {
    uint32_t req = n_now();
    hp->request_ltr_refresh();  // nothing acknowledged yet: the receiver acks only 3 frames after an LTR
    sink.wait_aus(req + 3, 3);
    bool idr = false;
    for (uint32_t n = req; n < n_now(); n++) if (au_at(n).idr && n > 0) { idr = true; break; }
    check(idr, "refresh before any acknowledged LTR -> IDR");
  }
  sleep_ms(2000);  // LTRs come and get acknowledged
  uint32_t f1 = lose_and_refresh("720p clean", 4);
  check(f1 != UINT32_MAX && au_at(f1).refresh && !au_at(f1).idr, "refresh after acks is not an IDR");
  check(f1 != UINT32_MAX && au_at(f1).size <= (w0 >= 1920 ? 80000u : 40000u), "refresh size within limit");
  // 2. across set_input_paused
  sleep_ms(1500);
  h->set_input_paused(true);
  uint32_t before = n_now();
  sleep_ms(500);
  uint32_t during = n_now();
  h->set_input_paused(false);
  check(during <= before + 1, "no AU while paused (" + std::to_string(during - before) + ")");
  sleep_ms(1500);
  uint32_t f2 = lose_and_refresh("after pause", 3);
  check(f2 != UINT32_MAX && au_at(f2).refresh, "refresh after pause");
  // 3. across set_format
  const int w1 = w0 >= 1920 ? 1280 : 1920, h1 = w0 >= 1920 ? 720 : 1080;
  uint32_t fmt_at = n_now();
  h->set_format(VideoFormat{w1, h1, 30});
  sink.wait_aus(fmt_at + 3, 3);
  uint32_t first_new = UINT32_MAX;
  for (uint32_t n = fmt_at; n < n_now(); n++) if (au_at(n).w == w1) { first_new = n; break; }
  check(first_new != UINT32_MAX && au_at(first_new).idr && au_at(first_new).h == h1, "set_format -> IDR at the new size");
  bool mixed = false;
  for (uint32_t n = first_new; n < n_now() && first_new != UINT32_MAX; n++) if (au_at(n).w != w1) mixed = true;
  check(!mixed, "no old-size AU after the new IDR");
  // a refresh right after the new IDR, before any new ack: must be an IDR (old tokens forgotten)
  hp->request_ltr_refresh();
  sleep_ms(2500);
  uint32_t f3 = lose_and_refresh("after set_format", 4);
  check(f3 != UINT32_MAX && au_at(f3).refresh && !au_at(f3).idr && au_at(f3).w == w1, "refresh after set_format at the new size");
  sleep_ms(500);
  h.reset();
  {
    std::lock_guard<std::mutex> lk(ack_mu);
    ack_stop = true;
  }
  ack_cv.notify_all();
  acker.join();
  std::lock_guard<std::mutex> lk(rx.mu);
  check(rx.refresh_bad == 0, "every refresh/IDR decoded to a newer picture (bad " + std::to_string(rx.refresh_bad) + ")");
  std::string sizes = "[";
  for (auto& s : rx.refresh_sizes) sizes += (sizes.size() > 1 ? "," : "") + J().i("frame", s.first).i("bytes", (long long)s.second).done();
  sizes += "]";
  out(J().str("ev", "ltr").b("ok", fails == 0).i("fails", fails).i("aus", n_now()).i("decode_errors", dec.errors)
          .i("refresh_ok", rx.refresh_ok).i("refresh_bad", rx.refresh_bad).raw("refreshes", sizes));
  return fails ? 1 : 0;
}

// ------------------------------------------------------------------------------------------- warm / idle
// One open -> start_encoding -> first AU on the synthetic source (start_ms=0: the camera is not the term).
struct OpenResult { double first_au_ms = -1, open_ms = -1; std::string encoder; };
OpenResult open_once(VideoBackend& be, int w, int h, const std::string& test = "synth;start_ms=0") {
  Sink sink;
  sink.print_events = false;
  CameraSpec spec;
  spec.fmt = VideoFormat{w, h, 30};
  spec.bitrate = w >= 1920 ? 5000000 : 2500000;
  spec.test = test;
  OpenResult r;
  std::string err;
  const int64_t t0 = now_ns();
  auto hd = be.open_camera(spec, &sink, &err);
  r.open_ms = (now_ns() - t0) / 1e6;
  if (!hd) { r.encoder = "error: " + err; return r; }
  hd->start_encoding();
  if (sink.wait_aus(1, 5)) r.first_au_ms = (sink.aus.front().rx_ns - t0) / 1e6;
  r.encoder = sink.event("encoder");
  hd.reset();
  return r;
}

int cmd_warm(int n) {
  auto be = make_backend();
  std::vector<double> cold, warm, standby;
  std::string ecold;
  {
    OpenResult r = open_once(*be, 1280, 720);
    cold.push_back(r.first_au_ms);
    ecold = r.encoder;
  }
  std::vector<std::string> wenc, senc;
  for (int i = 0; i < n; i++) {
    sleep_ms(300);
    OpenResult r = open_once(*be, 1280, 720);
    warm.push_back(r.first_au_ms);
    wenc.push_back(r.encoder);
  }
  be->set_encoder_standby(EncoderStandby{true, VideoFormat{1280, 720, 30}, 2500000});
  for (int i = 0; i < n; i++) {
    sleep_ms(500);  // the next standby is created after each stream closes
    OpenResult r = open_once(*be, 1280, 720);
    standby.push_back(r.first_au_ms);
    senc.push_back(r.encoder);
  }
  int standby_adopted = 0;
  for (auto& e : senc) standby_adopted += e.rfind("standby", 0) == 0;
  be->set_encoder_standby(EncoderStandby{});
  out(J().str("ev", "warm").str("cold_encoder", ecold).raw("cold_first_au_ms", dist(cold).done())
          .raw("warm_first_au_ms", dist(warm).done()).str("warm_encoder_last", wenc.empty() ? "" : wenc.back())
          .raw("standby_first_au_ms", dist(standby).done()).i("standby_adopted", standby_adopted).i("n", n)
          .str("standby_encoder_last", senc.empty() ? "" : senc.back()));
  return 0;
}

int cmd_idle(const std::string& mode, const std::vector<double>& minutes) {
  auto be = make_backend(mode == "prewarm" ? "standby=prewarm" : "standby=session");
  // the process has used VideoToolbox once (a stream ran, as in a helper that streamed before)
  OpenResult first = open_once(*be, 1280, 720);
  out(J().str("ev", "idle_first").str("mode", mode).num("first_au_ms", first.first_au_ms).str("encoder", first.encoder));
  for (double m : minutes) {
    be->set_encoder_standby(EncoderStandby{true, VideoFormat{1280, 720, 30}, 2500000});  // warm sets
    sleep_ms(m * 60000);
    OpenResult r = open_once(*be, 1280, 720);
    out(J().str("ev", "idle").str("mode", mode).num("idle_min", m, 1).num("first_au_ms", r.first_au_ms)
            .num("open_ms", r.open_ms).str("encoder", r.encoder));
    be->set_encoder_standby(EncoderStandby{});  // warm clears; the next round sets it again
    sleep_ms(200);
  }
  return 0;
}

// RSS / footprint / CPU of VTEncoderXPCService instances (one per client process, spawned with its first
// session): `only` limits it to the given pids (the instance that appeared with our standby session).
struct ProcStat { double rss_mb = 0, phys_mb = 0, cpu_s = 0, energy_nj = 0; int n = 0; std::vector<pid_t> pids; };
ProcStat xpc_stat(const std::set<pid_t>* only = nullptr) {
  ProcStat s;
  int cnt = proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
  std::vector<pid_t> pids(cnt + 16);
  cnt = proc_listpids(PROC_ALL_PIDS, 0, pids.data(), (int)(pids.size() * sizeof(pid_t))) / sizeof(pid_t);
  mach_timebase_info_data_t tb;
  mach_timebase_info(&tb);
  for (int i = 0; i < cnt; i++) {
    if (pids[i] <= 0 || (only && !only->count(pids[i]))) continue;
    char name[2 * MAXCOMLEN + 1] = {0};
    if (proc_name(pids[i], name, sizeof name) <= 0 || strcmp(name, "VTEncoderXPCService") != 0) continue;
    rusage_info_v6 ri;
    if (proc_pid_rusage(pids[i], RUSAGE_INFO_V6, (rusage_info_t*)&ri) != 0) continue;
    s.rss_mb += ri.ri_resident_size / 1048576.0;
    s.phys_mb += ri.ri_phys_footprint / 1048576.0;
    s.cpu_s += (double)(ri.ri_user_time + ri.ri_system_time) * tb.numer / tb.denom / 1e9;
    s.energy_nj += (double)ri.ri_energy_nj;
    s.n++;
    s.pids.push_back(pids[i]);
  }
  return s;
}

int cmd_idlecost(const std::string& mode, double minutes) {
  auto be = make_backend(mode == "prewarm" ? "standby=prewarm" : "standby=session");
  sleep_ms(500);
  const ProcStat x0 = xpc_stat();
  const std::set<pid_t> before(x0.pids.begin(), x0.pids.end());
  const double rss0 = rss_mb(), phys0 = phys_mb();
  if (mode == "session" || mode == "prewarm") {  // prewarm: one session created and invalidated
    be->set_encoder_standby(EncoderStandby{true, VideoFormat{1280, 720, 30}, 2500000});
    sleep_ms(1500);
  }
  std::set<pid_t> mine;
  for (pid_t p : xpc_stat().pids) if (!before.count(p)) mine.insert(p);
  std::string mine_s = "[";
  for (pid_t p : mine) mine_s += (mine_s.size() > 1 ? "," : "") + std::to_string(p);
  mine_s += "]";
  out(J().str("ev", "idlecost_start").str("mode", mode).i("pid", getpid()).raw("xpc_pids", mine_s));
  const ProcStat x1 = xpc_stat(&mine);
  rusage_info_v6 me0;
  proc_pid_rusage(getpid(), RUSAGE_INFO_V6, (rusage_info_t*)&me0);
  const double rss1 = rss_mb(), phys1 = phys_mb(), cpu1 = cpu_s();
  const int64_t t1 = now_ns();
  const int steps = (int)std::lround(minutes * 6);
  for (int i = 0; i < steps; i++) sleep_ms(10000);
  const ProcStat x2 = xpc_stat(&mine);
  rusage_info_v6 me1;
  proc_pid_rusage(getpid(), RUSAGE_INFO_V6, (rusage_info_t*)&me1);
  const double wall = (now_ns() - t1) / 1e9;
  const double cpu = cpu_s() - cpu1;
  out(J().str("ev", "idlecost").str("mode", mode).num("minutes", wall / 60, 2)
          .num("self_rss_before_mb", rss0, 1).num("self_rss_standby_mb", rss1, 1).num("self_rss_end_mb", rss_mb(), 1)
          .num("self_phys_before_mb", phys0, 1).num("self_phys_standby_mb", phys1, 1).num("self_phys_end_mb", phys_mb(), 1)
          .num("self_cpu_pct", cpu / wall * 100, 4)
          .num("self_energy_mw", (double)(me1.ri_energy_nj - me0.ri_energy_nj) / wall / 1e6, 3)
          .i("xpc_n", x2.n).num("xpc_rss_mb", x1.rss_mb, 1).num("xpc_rss_end_mb", x2.rss_mb, 1)
          .num("xpc_phys_mb", x1.phys_mb, 1).num("xpc_phys_end_mb", x2.phys_mb, 1)
          .num("xpc_cpu_pct", (x2.cpu_s - x1.cpu_s) / wall * 100, 4)
          .num("xpc_energy_mw", (x2.energy_nj - x1.energy_nj) / wall / 1e6, 3));
  return 0;
}

// ------------------------------------------------------------------------------------------- guard
int run_disclaimed(int argc, char** argv) {
  // posix_spawnattr responsibility disclaim (libquarantine/libsystem private SPI, used by terminals and
  // debuggers): the child becomes its own TCC responsible process.
  using fn_t = int (*)(posix_spawnattr_t*, int);
  auto fn = (fn_t)dlsym(RTLD_DEFAULT, "responsibility_spawnattrs_setdisclaim");
  if (!fn) { out(J().str("ev", "guard").b("ok", false).str("error", "disclaim SPI not found")); return 2; }
  posix_spawnattr_t at;
  posix_spawnattr_init(&at);
  fn(&at, 1);
  std::vector<char*> av;
  for (int i = 0; i < argc; i++) if (strcmp(argv[i], "--disclaim")) av.push_back(argv[i]);
  av.push_back(nullptr);
  pid_t pid = 0;
  char path[PROC_PIDPATHINFO_MAXSIZE];
  proc_pidpath(getpid(), path, sizeof path);
  int rc = posix_spawn(&pid, path, nullptr, &at, av.data(), environ);
  posix_spawnattr_destroy(&at);
  if (rc != 0) { out(J().str("ev", "guard").b("ok", false).str("error", "spawn failed")); return 2; }
  int st = 0;
  waitpid(pid, &st, 0);
  return WIFEXITED(st) ? WEXITSTATUS(st) : 3;
}

int cmd_guard() {
  auto be = make_backend();
  Listener lis;
  be->set_listener(&lis);
  const std::string perm0 = camera_permission();
  Sink sink;
  sink.print_events = false;
  CameraSpec spec;
  spec.fmt = VideoFormat{1280, 720, 30};
  std::string err;
  const int64_t t0 = now_ns();
  auto h = be->open_camera(spec, &sink, &err);
  const double ms = (now_ns() - t0) / 1e6;
  sleep_ms(300);  // the fresh permission event
  // then the synthetic source in the same process: works without the permission
  OpenResult synth = open_once(*be, 1280, 720, "synth");
  const bool expect_fail = perm0 != "granted";
  const bool ok = expect_fail ? (!h && err.rfind("permission", 0) == 0 && lis.perms.load() >= 1) : (h != nullptr);
  out(J().str("ev", "guard").b("ok", ok && synth.first_au_ms > 0).str("permission", perm0).b("opened", h != nullptr).str("error", err)
          .num("fail_ms", ms).i("permission_events", lis.perms.load()).i("capture_device_creations", capture_device_creations())
          .str("permission_after", camera_permission()).num("synth_first_au_ms", synth.first_au_ms).str("synth_encoder", synth.encoder)
          .i("pid", getpid()));
  h.reset();
  return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  @autoreleasepool {
    if (argc < 2) {
      fprintf(stderr, "usage: video_check selftest|cameras|run|ltr|warm|idle|idlecost|guard ...\n");
      return 2;
    }
    if (flag(argc, argv, "--disclaim")) return run_disclaimed(argc, argv);
    std::string cmd = argv[1];
    if (cmd == "selftest") return cmd_selftest();
    if (cmd == "cameras") return cmd_cameras();
    if (cmd == "run") {
      RunOpts o;
      o.test = arg(argc, argv, "--test", "synth");
      o.camera = arg(argc, argv, "--camera", "");
      parse_wh(arg(argc, argv, "--size", "1280x720"), &o.w, &o.h);
      o.bitrate = atoi(arg(argc, argv, "--bitrate", o.w >= 1920 ? "5000000" : "2500000").c_str());
      o.seconds = atof(arg(argc, argv, "--seconds", "10").c_str());
      o.start_after_ms = atof(arg(argc, argv, "--start-after-ms", "0").c_str());
      o.verify = flag(argc, argv, "--verify");
      o.standby = flag(argc, argv, "--standby");
      o.quiet = flag(argc, argv, "--quiet");
      std::string ba = arg(argc, argv, "--bitrate-at", "");
      if (!ba.empty()) {
        double t = 0;
        int b = 0;
        if (sscanf(ba.c_str(), "%lf:%d", &t, &b) == 2) o.bitrate_at.push_back({t, b});
      }
      return cmd_run(o);
    }
    if (cmd == "ltr") {
      int w = 1280, h = 720;
      parse_wh(arg(argc, argv, "--size", "1280x720"), &w, &h);
      return cmd_ltr(w, h);
    }
    if (cmd == "warm") return cmd_warm(atoi(arg(argc, argv, "--n", "20").c_str()));
    if (cmd == "idle") {
      std::vector<double> m;
      std::string s = arg(argc, argv, "--minutes", "1,10,60");
      for (size_t p = 0; p < s.size();) {
        size_t q = s.find(',', p);
        m.push_back(atof(s.substr(p, q - p).c_str()));
        if (q == std::string::npos) break;
        p = q + 1;
      }
      return cmd_idle(arg(argc, argv, "--mode", "session"), m);
    }
    if (cmd == "idlecost") return cmd_idlecost(arg(argc, argv, "--mode", "session"), atof(arg(argc, argv, "--minutes", "10").c_str()));
    if (cmd == "guard") return cmd_guard();
    fprintf(stderr, "unknown command %s\n", cmd.c_str());
    return 2;
  }
}
