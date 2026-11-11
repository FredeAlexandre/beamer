// beamer-camera: VideoToolbox H.264 encode -> Annex-B, per-frame trace, optional decode-back + PSNR.
#import "common.h"
#import <VideoToolbox/VideoToolbox.h>
#include <cmath>
#include <deque>

struct Encoder::Impl {
  Opts o;
  VTCompressionSessionRef s = nullptr;
  FILE *out = nullptr, *trace = nullptr;
  std::mutex mu;
  std::map<uint32_t, FrameRec> pending;      // by frame number until the output callback
  std::vector<FrameRec> done;                // in output order
  std::deque<std::pair<int64_t, uint32_t>> pending_ltr_acks;  // (token, frame from which it is acked)
  bool rx_broken = false;                    // simulated receiver: lost a frame, not yet refreshed
  bool hw = false;
  NSString *encoderID = nil;
  // verify
  VTDecompressionSessionRef dec = nullptr;
  CMVideoFormatDescriptionRef decFmt = nullptr;
  dispatch_queue_t decq = nullptr;
  std::map<uint32_t, std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> src;  // Y, UV copies
  int dec_errors = 0, dec_frames = 0;
  // periodic stats
  size_t stats_from = 0;
  double last_tick = 0;
};

static int64_t annexb_size_and_write(CMSampleBufferRef sb, bool key, FILE *out) {
  CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sb);
  static const uint8_t sc[4] = {0, 0, 0, 1};
  int64_t total = 0;
  if (key) {
    size_t n = 0;
    int nalHdr = 4;
    CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, 0, nullptr, nullptr, &n, &nalHdr);
    for (size_t i = 0; i < n; i++) {
      const uint8_t *p;
      size_t len;
      if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, i, &p, &len, nullptr, nullptr) == noErr) {
        if (out) { fwrite(sc, 1, 4, out); fwrite(p, 1, len, out); }
        total += 4 + len;
      }
    }
  }
  CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
  size_t len = 0;
  char *data = nullptr;
  std::vector<char> copy;
  if (CMBlockBufferGetDataPointer(bb, 0, nullptr, &len, &data) != noErr || len != CMBlockBufferGetDataLength(bb)) {
    len = CMBlockBufferGetDataLength(bb);
    copy.resize(len);
    CMBlockBufferCopyDataBytes(bb, 0, len, copy.data());
    data = copy.data();
  }
  size_t off = 0;
  while (off + 4 <= len) {
    uint32_t n = ((uint8_t)data[off] << 24) | ((uint8_t)data[off + 1] << 16) | ((uint8_t)data[off + 2] << 8) | (uint8_t)data[off + 3];
    if (off + 4 + n > len) break;
    if (out) { fwrite(sc, 1, 4, out); fwrite(data + off + 4, 1, n, out); }
    total += 4 + n;
    off += 4 + n;
  }
  return total;
}

static double plane_psnr(const uint8_t *a, size_t as, const uint8_t *b, size_t bs, int w, int h) {
  double se = 0;
  for (int y = 0; y < h; y++) {
    const uint8_t *ra = a + y * as, *rb = b + y * bs;
    for (int x = 0; x < w; x++) { int d = (int)ra[x] - (int)rb[x]; se += d * d; }
  }
  double mse = se / ((double)w * h);
  return mse <= 0 ? 99.0 : 10 * std::log10(255.0 * 255.0 / mse);
}

static void decode_cb(void *ctx, void *ref, OSStatus st, VTDecodeInfoFlags, CVImageBufferRef img, CMTime, CMTime) {
  auto *d = (Encoder::Impl *)ctx;
  uint32_t f = (uint32_t)(uintptr_t)ref;
  std::lock_guard<std::mutex> lk(d->mu);
  if (st != noErr || !img) { d->dec_errors++; return; }
  d->dec_frames++;
  auto it = d->src.find(f);
  if (it == d->src.end()) return;
  CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
  int w = (int)CVPixelBufferGetWidth(img), h = (int)CVPixelBufferGetHeight(img);
  double py = plane_psnr((uint8_t *)CVPixelBufferGetBaseAddressOfPlane(img, 0), CVPixelBufferGetBytesPerRowOfPlane(img, 0),
                         it->second.first.data(), w, w, h);
  double puv = plane_psnr((uint8_t *)CVPixelBufferGetBaseAddressOfPlane(img, 1), CVPixelBufferGetBytesPerRowOfPlane(img, 1),
                          it->second.second.data(), w, w, h / 2);
  CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
  d->src.erase(it);
  for (auto &r : d->done)
    if (r.f == f) { r.psnr_y = py; r.psnr_uv = puv; break; }
}

static void verify_decode(Encoder::Impl *d, CMSampleBufferRef sb, uint32_t f) {
  if ((int)f < d->o.verify_from || std::find(d->o.verify_drop.begin(), d->o.verify_drop.end(), (int)f) != d->o.verify_drop.end()) {
    std::lock_guard<std::mutex> lk(d->mu);
    d->src.erase(f);
    return;
  }
  CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sb);
  if (!d->dec || !CMFormatDescriptionEqual(fmt, d->decFmt)) {
    if (d->dec) { VTDecompressionSessionWaitForAsynchronousFrames(d->dec); VTDecompressionSessionInvalidate(d->dec); CFRelease(d->dec); d->dec = nullptr; }
    if (d->decFmt) CFRelease(d->decFmt);
    d->decFmt = (CMVideoFormatDescriptionRef)CFRetain(fmt);
    NSDictionary *attrs = @{(id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)};
    VTDecompressionOutputCallbackRecord cb = {decode_cb, d};
    OSStatus st = VTDecompressionSessionCreate(nullptr, d->decFmt, nullptr, (__bridge CFDictionaryRef)attrs, &cb, &d->dec);
    if (st != noErr) { infof("decoder create failed %d", (int)st); d->dec = nullptr; return; }
  }
  uint64_t t0 = now_ns();
  VTDecodeInfoFlags fl = 0;
  OSStatus st = VTDecompressionSessionDecodeFrame(d->dec, sb, 0, (void *)(uintptr_t)f, &fl);  // synchronous
  double ms = (now_ns() - t0) / 1e6;
  if (st != noErr) { std::lock_guard<std::mutex> lk(d->mu); d->dec_errors++; }
  std::lock_guard<std::mutex> lk(d->mu);
  for (auto &r : d->done)
    if (r.f == f) { r.dec_ms = ms; break; }
}

static void encode_cb(void *ctx, void *ref, OSStatus st, VTEncodeInfoFlags info, CMSampleBufferRef sb) {
  auto *d = (Encoder::Impl *)ctx;
  uint64_t t = now_ns();
  uint32_t f = (uint32_t)(uintptr_t)ref;
  FrameRec r;
  {
    std::lock_guard<std::mutex> lk(d->mu);
    auto it = d->pending.find(f);
    if (it != d->pending.end()) { r = it->second; d->pending.erase(it); }
    r.f = f;
  }
  r.out_ns = t;
  if (st != noErr || !sb || (info & kVTEncodeInfo_FrameDropped)) {
    r.dropped = true;
  } else {
    CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray(sb, false);
    bool notSync = false;
    if (att && CFArrayGetCount(att)) {
      CFDictionaryRef a = (CFDictionaryRef)CFArrayGetValueAtIndex(att, 0);
      notSync = CFDictionaryContainsKey(a, kCMSampleAttachmentKey_NotSync) &&
                CFBooleanGetValue((CFBooleanRef)CFDictionaryGetValue(a, kCMSampleAttachmentKey_NotSync));
      CFNumberRef tok = (CFNumberRef)CFDictionaryGetValue(a, kVTSampleAttachmentKey_RequireLTRAcknowledgementToken);
      if (tok) { int64_t v = 0; CFNumberGetValue(tok, kCFNumberSInt64Type, &v); r.ltr_token = v; }
    }
    r.key = !notSync;
    std::lock_guard<std::mutex> lk(d->mu);
    r.size = annexb_size_and_write(sb, r.key, d->out);
    // Simulated receiver (same loss as --verify-drop/--verify-from): it acks an LTR frame only when it
    // decoded it cleanly, i.e. it was not lost and nothing since the last loss was missing.
    bool lost = (int)f < d->o.verify_from ||
                std::find(d->o.verify_drop.begin(), d->o.verify_drop.end(), (int)f) != d->o.verify_drop.end();
    if (lost) d->rx_broken = true;
    else if (r.key || r.ltr_refresh) d->rx_broken = false;
    if (r.ltr_token >= 0 && !lost && !d->rx_broken) d->pending_ltr_acks.push_back({r.ltr_token, f + d->o.ltr_ack_delay});
  }
  {
    std::lock_guard<std::mutex> lk(d->mu);
    d->done.push_back(r);
  }
  if (d->o.verify && !r.dropped) verify_decode(d, sb, f);
}

Encoder::Encoder(const Opts &o) : d(new Impl) { d->o = o; }
Encoder::~Encoder() {
  if (d->s) { VTCompressionSessionInvalidate(d->s); CFRelease(d->s); }
  if (d->dec) { VTDecompressionSessionInvalidate(d->dec); CFRelease(d->dec); }
  if (d->decFmt) CFRelease(d->decFmt);
  delete d;
}
int Encoder::frames_out() const { std::lock_guard<std::mutex> lk(d->mu); return (int)d->done.size(); }

static NSString *st_str(OSStatus st) { return st == noErr ? @"ok" : [NSString stringWithFormat:@"%d", (int)st]; }

bool Encoder::open(NSMutableDictionary *info) {
  const Opts &o = d->o;
  NSMutableDictionary *spec = [NSMutableDictionary dictionary];
  if (o.lowlatency) spec[(id)kVTVideoEncoderSpecification_EnableLowLatencyRateControl] = @YES;
  if (o.hw_required) spec[(id)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder] = @YES;
  if (!o.encoder_id.empty()) spec[(id)kVTVideoEncoderSpecification_EncoderID] = @(o.encoder_id.c_str());
  NSDictionary *srcAttrs = @{
    (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
    (id)kCVPixelBufferWidthKey : @(o.width), (id)kCVPixelBufferHeightKey : @(o.height),
    (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
  };
  uint64_t t0 = now_ns();
  OSStatus st = VTCompressionSessionCreate(nullptr, o.width, o.height, kCMVideoCodecType_H264, (__bridge CFDictionaryRef)spec,
                                           (__bridge CFDictionaryRef)srcAttrs, nullptr, encode_cb, d, &d->s);
  info[@"create_ms"] = @((now_ns() - t0) / 1e6);
  if (st != noErr) { info[@"create_status"] = @(st); return false; }
  NSMutableDictionary *props = [NSMutableDictionary dictionary];
  auto set = [&](CFStringRef k, id v) {
    OSStatus s2 = VTSessionSetProperty(d->s, k, (__bridge CFTypeRef)v);
    props[(__bridge NSString *)k] = @{@"value" : v, @"status" : st_str(s2)};
  };
  if (!o.preset.empty()) {
    CFDictionaryRef presets = nullptr;
    OSStatus s2 = VTSessionCopyProperty(d->s, kVTCompressionPropertyKey_SupportedPresetDictionaries, nullptr, &presets);
    NSDictionary *pd = (__bridge_transfer NSDictionary *)presets;
    CFStringRef name = o.preset == "vc" ? kVTCompressionPreset_VideoConferencing : o.preset == "hs" ? kVTCompressionPreset_HighSpeed
                       : o.preset == "bal" ? kVTCompressionPreset_Balanced : kVTCompressionPreset_HighQuality;
    NSDictionary *chosen = pd[(__bridge NSString *)name];
    info[@"preset"] = @{@"name" : (__bridge NSString *)name, @"status" : st_str(s2), @"available" : pd ? pd.allKeys : @[],
                        @"settings" : chosen ? [chosen description] : @"(none)"};
    if (chosen) VTSessionSetProperties(d->s, (__bridge CFDictionaryRef)chosen);
  }
  set(kVTCompressionPropertyKey_RealTime, @(o.realtime));
  CFStringRef prof = o.profile == "high" ? kVTProfileLevel_H264_High_AutoLevel
                     : o.profile == "main" ? kVTProfileLevel_H264_Main_AutoLevel
                     : o.profile == "baseline" ? kVTProfileLevel_H264_Baseline_AutoLevel
                     : o.profile == "cbaseline" ? kVTProfileLevel_H264_ConstrainedBaseline_AutoLevel
                                                : kVTProfileLevel_H264_ConstrainedHigh_AutoLevel;
  set(kVTCompressionPropertyKey_ProfileLevel, (__bridge NSString *)prof);
  set(kVTCompressionPropertyKey_AllowFrameReordering, @NO);
  set(kVTCompressionPropertyKey_ExpectedFrameRate, @(o.fps));
  set(kVTCompressionPropertyKey_AverageBitRate, @(o.bitrate));
  if (o.limit_factor > 0) set(kVTCompressionPropertyKey_DataRateLimits, @[ @((int64_t)(o.bitrate * o.limit_factor / 8)), @1.0 ]);
  if (o.keyint > 0) set(kVTCompressionPropertyKey_MaxKeyFrameInterval, @(o.keyint));
  set(kVTCompressionPropertyKey_MaxFrameDelayCount, @0);
  if (o.speed) set(kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, @YES);
  if (o.ltr) set(kVTCompressionPropertyKey_EnableLTR, @YES);
  if (o.max_qp > 0) set(kVTCompressionPropertyKey_MaxAllowedFrameQP, @(o.max_qp));
  for (auto &kv : o.props) {
    NSString *v = @(kv.second.c_str());
    id val = [v isEqualToString:@"true"] ? (id)@YES : [v isEqualToString:@"false"] ? (id)@NO
             : [[NSScanner scannerWithString:v] scanDouble:nil] ? (id)@(v.doubleValue) : (id)v;
    set((__bridge CFStringRef)@(kv.first.c_str()), val);
  }
  info[@"props"] = props;
  CFBooleanRef hw = nullptr;
  if (VTSessionCopyProperty(d->s, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder, nullptr, &hw) == noErr && hw) {
    d->hw = CFBooleanGetValue(hw);
    CFRelease(hw);
  }
  CFStringRef eid = nullptr;
  if (VTSessionCopyProperty(d->s, kVTCompressionPropertyKey_EncoderID, nullptr, &eid) == noErr && eid) d->encoderID = (__bridge_transfer NSString *)eid;
  info[@"hw"] = @(d->hw);
  info[@"encoder_id"] = d->encoderID ?: @"?";
  // read back what the session actually uses
  NSMutableDictionary *rb = [NSMutableDictionary dictionary];
  for (NSString *k in @[ (id)kVTCompressionPropertyKey_ProfileLevel, (id)kVTCompressionPropertyKey_MaxKeyFrameInterval,
                          (id)kVTCompressionPropertyKey_AverageBitRate, (id)kVTCompressionPropertyKey_MaxFrameDelayCount,
                          (id)kVTCompressionPropertyKey_H264EntropyMode, (id)kVTCompressionPropertyKey_BaseLayerFrameRateFraction,
                          (id)kVTCompressionPropertyKey_AllowFrameReordering, (id)kVTCompressionPropertyKey_RealTime,
                          @"PeriodicRefreshMode", @"MaxRefreshFrameIntervalDuration", @"NumberOfTemporalLayers", @"ReferenceBufferCount",
                          @"EnableFrameDropping", @"RequestedMaxEncoderLatency", @"NumberOfSlices", @"EnableLTR", @"QualityMode" ]) {
    CFTypeRef v = nullptr;
    if (VTSessionCopyProperty(d->s, (__bridge CFStringRef)k, nullptr, &v) == noErr && v) rb[k] = [(__bridge_transfer id)v description];
  }
  info[@"readback"] = rb;
  VTCompressionSessionPrepareToEncodeFrames(d->s);
  if (!o.out.empty()) d->out = o.out == "-" ? stdout : fopen(o.out.c_str(), "wb");
  if (!o.trace.empty()) d->trace = fopen(o.trace.c_str(), "w");
  return true;
}

void Encoder::encode(CVPixelBufferRef pb, const FrameRec &meta, bool forceKey, bool ltrRefresh) {
  NSMutableDictionary *fo = [NSMutableDictionary dictionary];
  if (forceKey) fo[(id)kVTEncodeFrameOptionKey_ForceKeyFrame] = @YES;
  FrameRec r = meta;
  r.forced = forceKey;
  r.ltr_refresh = ltrRefresh;
  {
    std::lock_guard<std::mutex> lk(d->mu);
    if (d->o.ltr && !d->pending_ltr_acks.empty()) {
      NSMutableArray *a = [NSMutableArray array];
      while (!d->pending_ltr_acks.empty() && d->pending_ltr_acks.front().second <= meta.f) {
        [a addObject:@(d->pending_ltr_acks.front().first)];
        d->pending_ltr_acks.pop_front();
      }
      if (a.count) fo[(id)kVTEncodeFrameOptionKey_AcknowledgedLTRTokens] = a;
    }
    if (ltrRefresh) fo[(id)kVTEncodeFrameOptionKey_ForceLTRRefresh] = @YES;
    if (d->o.verify) {
      CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
      int w = (int)CVPixelBufferGetWidth(pb), h = (int)CVPixelBufferGetHeight(pb);
      std::vector<uint8_t> Y(w * h), UV(w * h / 2);
      for (int y = 0; y < h; y++) memcpy(&Y[y * w], (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pb, 0) + y * CVPixelBufferGetBytesPerRowOfPlane(pb, 0), w);
      for (int y = 0; y < h / 2; y++) memcpy(&UV[y * w], (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pb, 1) + y * CVPixelBufferGetBytesPerRowOfPlane(pb, 1), w);
      CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
      d->src[meta.f] = {std::move(Y), std::move(UV)};
    }
    r.in_ns = now_ns();
    d->pending[meta.f] = r;
  }
  for (auto &fb : d->o.bitrate_at)
    if (fb.first == (int)meta.f) {
      OSStatus s2 = VTSessionSetProperty(d->s, kVTCompressionPropertyKey_AverageBitRate, (__bridge CFTypeRef) @(fb.second));
      emit(@{@"ev" : @"bitrate_set", @"f" : @(meta.f), @"bps" : @(fb.second), @"status" : st_str(s2)});
    }
  CMTime pts = CMTimeMake((int64_t)meta.f * 1000, (int32_t)std::lround(d->o.fps * 1000));
  CMTime dur = CMTimeMake(1000, (int32_t)std::lround(d->o.fps * 1000));
  VTEncodeInfoFlags fl = 0;
  OSStatus st = VTCompressionSessionEncodeFrame(d->s, pb, pts, dur, (__bridge CFDictionaryRef)fo, (void *)(uintptr_t)meta.f, &fl);
  if (st != noErr) infof("encode frame %u failed %d", meta.f, (int)st);
}

void Encoder::finish() {
  if (d->s) VTCompressionSessionCompleteFrames(d->s, kCMTimeInvalid);
  if (d->dec) VTDecompressionSessionWaitForAsynchronousFrames(d->dec);
  std::lock_guard<std::mutex> lk(d->mu);
  if (d->trace) {
    for (auto &r : d->done) {
      fprintf(d->trace,
              "{\"f\":%u,\"cap_ns\":%llu,\"arr_ns\":%llu,\"in_ns\":%llu,\"out_ns\":%llu,\"size\":%lld,\"key\":%s,\"forced\":%s,"
              "\"dropped\":%s,\"ltr_token\":%lld,\"ltr_refresh\":%s,\"enc_ms\":%.3f,\"cap_to_out_ms\":%.3f",
              r.f, (unsigned long long)r.cap_ns, (unsigned long long)r.arr_ns, (unsigned long long)r.in_ns,
              (unsigned long long)r.out_ns, (long long)r.size, r.key ? "true" : "false", r.forced ? "true" : "false",
              r.dropped ? "true" : "false", (long long)r.ltr_token, r.ltr_refresh ? "true" : "false",
              (r.out_ns - r.in_ns) / 1e6, (r.out_ns - r.cap_ns) / 1e6);
      if (r.psnr_y >= 0) fprintf(d->trace, ",\"psnr_y\":%.2f,\"psnr_uv\":%.2f,\"dec_ms\":%.3f", r.psnr_y, r.psnr_uv, r.dec_ms);
      fputs("}\n", d->trace);
    }
    fclose(d->trace);
    d->trace = nullptr;
  }
  if (d->out && d->out != stdout) { fclose(d->out); d->out = nullptr; }
  if (d->out == stdout) fflush(stdout);
}

static NSDictionary *dist(const std::vector<double> &v, int digits = 2) {
  auto r = [&](double x) { double m = std::pow(10, digits); return @(std::round(x * m) / m); };
  if (v.empty()) return @{@"n" : @0};
  return @{@"n" : @(v.size()), @"mean" : r(mean(v)), @"p50" : r(pct(v, 50)), @"p95" : r(pct(v, 95)), @"p99" : r(pct(v, 99)),
           @"max" : r(*std::max_element(v.begin(), v.end())), @"min" : r(*std::min_element(v.begin(), v.end()))};
}

void Encoder::tick_stats(double t_s) {
  std::lock_guard<std::mutex> lk(d->mu);
  std::vector<double> enc;
  int64_t bytes = 0;
  int n = 0, keys = 0, drops = 0;
  for (size_t i = d->stats_from; i < d->done.size(); i++) {
    auto &r = d->done[i];
    n++;
    if (r.dropped) { drops++; continue; }
    bytes += r.size;
    keys += r.key;
    enc.push_back((r.out_ns - r.in_ns) / 1e6);
  }
  double dt = t_s - d->last_tick;
  d->stats_from = d->done.size();
  d->last_tick = t_s;
  if (dt <= 0) return;
  emit(@{@"ev" : @"enc_stats", @"t" : @(std::round(t_s * 100) / 100), @"frames" : @(n), @"keys" : @(keys), @"dropped" : @(drops),
         @"kbps" : @(std::lround(bytes * 8 / dt / 1000)), @"enc_ms_p50" : @(pct(enc, 50)), @"enc_ms_max" : @(enc.empty() ? -1 : *std::max_element(enc.begin(), enc.end()))});
}

NSDictionary *Encoder::summary(double wall_s) {
  std::lock_guard<std::mutex> lk(d->mu);
  std::vector<double> enc, cap2out, keyB, pB, dec, psy, psuv, arr2in;
  int drops = 0, keys = 0, forcedKeys = 0, ltrTokens = 0;
  int64_t bytes = 0;
  double fps = d->o.fps;
  std::map<int, int64_t> win1s, win200;  // by frame number buckets
  for (auto &r : d->done) {
    if (r.dropped) { drops++; continue; }
    bytes += r.size;
    enc.push_back((r.out_ns - r.in_ns) / 1e6);
    cap2out.push_back((r.out_ns - r.cap_ns) / 1e6);
    arr2in.push_back((r.in_ns - r.arr_ns) / 1e6);
    if (r.key) { keys++; keyB.push_back(r.size); if (r.forced) forcedKeys++; }
    else pB.push_back(r.size);
    if (r.ltr_token >= 0) ltrTokens++;
    if (r.dec_ms >= 0) dec.push_back(r.dec_ms);
    if (r.psnr_y >= 0) { psy.push_back(r.psnr_y); psuv.push_back(r.psnr_uv); }
    win1s[(int)(r.f / fps)] += r.size;
    win200[(int)(r.f / (fps / 5))] += r.size;
  }
  // 1 s windows (full ones only) and 200 ms windows, in kbit/s
  std::vector<double> w1, w2;
  int lastFull = (int)(d->done.empty() ? 0 : (d->done.back().f + 1) / fps);
  for (auto &kv : win1s) if (kv.first < lastFull) w1.push_back(kv.second * 8 / 1000.0);
  for (auto &kv : win200) w2.push_back(kv.second * 8 / 1000.0 * 5);
  double sd = 0;
  if (!w1.empty()) { double m = mean(w1); for (double x : w1) sd += (x - m) * (x - m); sd = std::sqrt(sd / w1.size()); }
  // ltr-refresh frame sizes and forced-key sizes, individually
  NSMutableArray *refresh = [NSMutableArray array], *forced = [NSMutableArray array];
  for (auto &r : d->done) {
    if (r.ltr_refresh) [refresh addObject:@{@"f" : @(r.f), @"size" : @(r.size), @"key" : @(r.key)}];
    if (r.forced) [forced addObject:@{@"f" : @(r.f), @"size" : @(r.size), @"key" : @(r.key), @"enc_ms" : @((r.out_ns - r.in_ns) / 1e6)}];
  }
  NSMutableDictionary *s = [@{
    @"frames" : @(d->done.size()), @"dropped" : @(drops), @"keys" : @(keys), @"forced_keys" : @(forcedKeys), @"ltr_tokens" : @(ltrTokens),
    @"hw" : @(d->hw), @"encoder_id" : d->encoderID ?: @"?",
    @"kbps_mean" : @(std::lround(bytes * 8 / ((double)d->done.size() / fps) / 1000)),
    @"kbps_1s" : dist(w1, 0), @"kbps_1s_sd" : @(std::lround(sd)), @"kbps_200ms" : dist(w2, 0),
    @"enc_ms" : dist(enc, 3), @"cap_to_out_ms" : dist(cap2out, 3), @"arr_to_enc_in_ms" : dist(arr2in, 3),
    @"key_bytes" : dist(keyB, 0), @"p_bytes" : dist(pB, 0),
    @"forced" : forced, @"ltr_refresh" : refresh,
  } mutableCopy];
  if (!psy.empty()) {
    s[@"psnr_y"] = dist(psy, 2);
    s[@"psnr_uv"] = dist(psuv, 2);
    s[@"dec_ms"] = dist(dec, 3);
    s[@"dec_errors"] = @(d->dec_errors);
  }
  (void)wall_s;
  return s;
}
