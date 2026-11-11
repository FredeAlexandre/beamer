// SPDX-License-Identifier: GPL-3.0-only
// macOS video backend: one VideoToolbox H.264 session (camera PHASE2 §4.11, C1 settings): low-latency rate
// control, hardware required, real time, no frame reordering, Constrained High, LTR. Output is Annex-B with
// SPS/PPS before every IDR, handed to the owner's callback on VideoToolbox's output thread.
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

#include <atomic>
#include <vector>

#include "video_backend_mac.h"

namespace beamer::audio::macvideo {

namespace {
std::atomic<int> g_sessions{0};
std::mutex g_enc_times_mu;
std::vector<double> g_enc_times;

struct FrameCtx {
  uint64_t capture_ns = 0, submit_ns = 0;
  bool refresh = false;
  bool discard = false;  // wake-up frame: its output never leaves the encoder
};
}  // namespace

struct Encoder::Impl {
  std::recursive_mutex mu;  // guards out, w, h; held while the output callback runs
  std::function<void(const EncOut&)> out;
  int w = 0, h = 0;
  bool dead = false;
  std::vector<uint8_t> buf;  // output thread only (VideoToolbox serialises a session's callbacks)
};

namespace {

void append_annexb(std::vector<uint8_t>& o, CMSampleBufferRef sb, bool key) {
  static const uint8_t sc[4] = {0, 0, 0, 1};
  if (key) {
    CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sb);
    size_t n = 0;
    int nal_len = 4;
    CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, 0, nullptr, nullptr, &n, &nal_len);
    for (size_t i = 0; i < n; i++) {
      const uint8_t* p = nullptr;
      size_t len = 0;
      if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, i, &p, &len, nullptr, nullptr) == noErr) {
        o.insert(o.end(), sc, sc + 4);
        o.insert(o.end(), p, p + len);
      }
    }
  }
  CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
  if (!bb) return;
  const size_t len = CMBlockBufferGetDataLength(bb);
  const size_t base = o.size();
  o.resize(base + len);
  // AVCC (4-byte big-endian lengths) -> Annex-B in place: same size, each length becomes a start code
  if (CMBlockBufferCopyDataBytes(bb, 0, len, o.data() + base) != kCMBlockBufferNoErr) { o.resize(base); return; }
  size_t off = base;
  while (off + 4 <= o.size()) {
    const uint32_t n = (uint32_t)o[off] << 24 | (uint32_t)o[off + 1] << 16 | (uint32_t)o[off + 2] << 8 | o[off + 3];
    if (off + 4 + n > o.size()) { o.resize(off); break; }
    memcpy(o.data() + off, sc, 4);
    off += 4 + n;
  }
}

void output_cb(void* refcon, void* frame_ref, OSStatus st, VTEncodeInfoFlags info, CMSampleBufferRef sb) {
  auto* weak = static_cast<std::weak_ptr<Encoder::Impl>*>(refcon);
  std::unique_ptr<FrameCtx> ctx(static_cast<FrameCtx*>(frame_ref));
  std::shared_ptr<Encoder::Impl> d = weak->lock();
  if (!d) return;
  const uint64_t out_ns = (uint64_t)now_ns();
  if (ctx && ctx->discard) return;
  if (st != noErr || !sb || (info & kVTEncodeInfo_FrameDropped)) {
    vlog("warn", "video encoder dropped a frame (status %d, flags 0x%x)", (int)st, (unsigned)info);
    return;
  }
  bool key = true;
  int64_t token = -1;
  CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray(sb, false);
  if (att && CFArrayGetCount(att)) {
    CFDictionaryRef a = (CFDictionaryRef)CFArrayGetValueAtIndex(att, 0);
    CFBooleanRef ns = (CFBooleanRef)CFDictionaryGetValue(a, kCMSampleAttachmentKey_NotSync);
    if (ns && CFBooleanGetValue(ns)) key = false;
    CFNumberRef tok = (CFNumberRef)CFDictionaryGetValue(a, kVTSampleAttachmentKey_RequireLTRAcknowledgementToken);
    if (tok) CFNumberGetValue(tok, kCFNumberSInt64Type, &token);
  }
  if (ctx) {
    std::lock_guard<std::mutex> lk(g_enc_times_mu);
    if (g_enc_times.size() < 100000) g_enc_times.push_back((out_ns - ctx->submit_ns) / 1e6);
  }
  std::lock_guard<std::recursive_mutex> lk(d->mu);
  if (d->dead || !d->out) return;
  d->buf.clear();
  append_annexb(d->buf, sb, key);
  EncOut o;
  o.data = d->buf.data();
  o.len = d->buf.size();
  o.capture_ns = ctx ? ctx->capture_ns : 0;
  o.submit_ns = ctx ? ctx->submit_ns : 0;
  o.out_ns = out_ns;
  o.idr = key;
  o.refresh = ctx && ctx->refresh && !key;
  o.token = token;
  o.w = d->w;
  o.h = d->h;
  d->out(o);
}

}  // namespace

std::vector<double> take_encode_times() {
  std::lock_guard<std::mutex> lk(g_enc_times_mu);
  std::vector<double> v;
  v.swap(g_enc_times);
  return v;
}

int Encoder::sessions_created() { return g_sessions.load(); }

std::unique_ptr<Encoder> Encoder::create(int w, int h, int bitrate, int fps, bool ltr, std::string* err, double* ms) {
  const int64_t t0 = now_ns();
  std::unique_ptr<Encoder> e(new Encoder());
  e->impl_ = std::make_shared<Impl>();
  e->impl_->w = w;
  e->impl_->h = h;
  e->w_ = w;
  e->h_ = h;
  e->bitrate_ = bitrate;
  e->fps_ = fps;
  NSDictionary* spec = @{
    (id)kVTVideoEncoderSpecification_EnableLowLatencyRateControl : @YES,
    // fail rather than fall back to the 436 ms software encoder (C1)
    (id)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder : @YES,
  };
  NSDictionary* src = @{
    (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
    (id)kCVPixelBufferWidthKey : @(w),
    (id)kCVPixelBufferHeightKey : @(h),
    (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
  };
  // The refcon outlives the session (freed in ~Encoder after invalidation), and only ever weakly refers
  // to the state, so a late callback after teardown is a no-op.
  auto* refcon = new std::weak_ptr<Impl>(e->impl_);
  VTCompressionSessionRef s = nullptr;
  g_sessions++;
  OSStatus st = VTCompressionSessionCreate(nullptr, w, h, kCMVideoCodecType_H264, (__bridge CFDictionaryRef)spec,
                                           (__bridge CFDictionaryRef)src, nullptr, output_cb, refcon, &s);
  if (st != noErr || !s) {
    delete refcon;
    if (err) *err = "internal: VTCompressionSessionCreate " + std::to_string((int)st) + " (hardware H.264 encoder)";
    return nullptr;
  }
  e->session_ = s;
  e->refcon_ = refcon;
  auto set = [&](CFStringRef k, CFTypeRef v) {
    OSStatus r = VTSessionSetProperty(s, k, v);
    if (r != noErr) vlog("warn", "video encoder: %s refused (%d)", [(__bridge NSString*)k UTF8String], (int)r);
  };
  set(kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
  set(kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_ConstrainedHigh_AutoLevel);
  set(kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
  set(kVTCompressionPropertyKey_ExpectedFrameRate, (__bridge CFTypeRef)@(fps));
  set(kVTCompressionPropertyKey_AverageBitRate, (__bridge CFTypeRef)@(bitrate));
  if (ltr) set(kVTCompressionPropertyKey_EnableLTR, kCFBooleanTrue);
  VTCompressionSessionPrepareToEncodeFrames(s);
  if (ms) *ms = (now_ns() - t0) / 1e6;
  return e;
}

Encoder::~Encoder() {
  if (impl_) {
    std::lock_guard<std::recursive_mutex> lk(impl_->mu);
    impl_->dead = true;
    impl_->out = nullptr;
  }
  if (session_) {
    VTCompressionSessionInvalidate((VTCompressionSessionRef)session_);
    CFRelease((CFTypeRef)session_);
  }
  delete static_cast<std::weak_ptr<Impl>*>(refcon_);
}

void Encoder::set_output(std::function<void(const EncOut&)> out) {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  impl_->out = std::move(out);
}

bool Encoder::encode(CVPixelBufferRef pb, uint64_t capture_ns, bool force_idr, bool refresh,
                     const std::vector<int64_t>& acked, std::string* err, bool discard) {
  NSMutableDictionary* fo = [NSMutableDictionary dictionary];
  if (force_idr) fo[(id)kVTEncodeFrameOptionKey_ForceKeyFrame] = @YES;
  if (!acked.empty()) {
    NSMutableArray* a = [NSMutableArray arrayWithCapacity:acked.size()];
    for (int64_t t : acked) [a addObject:@(t)];
    fo[(id)kVTEncodeFrameOptionKey_AcknowledgedLTRTokens] = a;
  }
  if (refresh && !force_idr) fo[(id)kVTEncodeFrameOptionKey_ForceLTRRefresh] = @YES;
  auto* ctx = new FrameCtx{capture_ns, (uint64_t)now_ns(), refresh && !force_idr, discard};
  // pts in the capture clock (ns), so VideoToolbox's rate control sees the real frame spacing
  CMTime pts = CMTimeMake((int64_t)capture_ns, 1000000000);
  CMTime dur = CMTimeMake(1000000000LL / fps_, 1000000000);
  OSStatus st = VTCompressionSessionEncodeFrame((VTCompressionSessionRef)session_, pb, pts, dur,
                                                (__bridge CFDictionaryRef)fo, ctx, nullptr);
  if (st != noErr) {
    // ctx is not freed here: whether VideoToolbox still calls back for a refused frame is undocumented
    if (err) *err = "internal: VTCompressionSessionEncodeFrame " + std::to_string((int)st);
    return false;
  }
  return true;
}

void Encoder::flush() {
  if (session_) VTCompressionSessionCompleteFrames((VTCompressionSessionRef)session_, kCMTimeInvalid);
}

bool Encoder::set_bitrate(int bps) {
  if (bps <= 0) return false;
  OSStatus st = VTSessionSetProperty((VTCompressionSessionRef)session_, kVTCompressionPropertyKey_AverageBitRate,
                                     (__bridge CFTypeRef)@(bps));
  if (st == noErr) bitrate_ = bps;
  return st == noErr;
}

void Encoder::set_fps(int fps) {
  if (fps <= 0 || fps == fps_) return;
  fps_ = fps;
  VTSessionSetProperty((VTCompressionSessionRef)session_, kVTCompressionPropertyKey_ExpectedFrameRate,
                       (__bridge CFTypeRef)@(fps));
}

}  // namespace beamer::audio::macvideo
