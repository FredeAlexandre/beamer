// beamer-camera: commands. Camera inventory, permission, encoder inventory, synthetic and camera runs.
#import "common.h"
#import <AVFoundation/AVFoundation.h>
#import <VideoToolbox/VideoToolbox.h>
#include <cmath>
#include <ctime>
#include <atomic>

static NSString *fourcc(FourCharCode c) {
  char s[5] = {(char)(c >> 24), (char)(c >> 16), (char)(c >> 8), (char)c, 0};
  for (int i = 0; i < 4; i++) if (s[i] < 32 || s[i] > 126) return [NSString stringWithFormat:@"0x%08x", (unsigned)c];
  return @(s);
}
static FourCharCode parse_fourcc(const std::string &s) {
  if (s.size() != 4) return kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
  return (FourCharCode)s[0] << 24 | (FourCharCode)s[1] << 16 | (FourCharCode)s[2] << 8 | (FourCharCode)s[3];
}
static NSString *auth_str(AVAuthorizationStatus s) {
  switch (s) {
    case AVAuthorizationStatusAuthorized: return @"authorized";
    case AVAuthorizationStatusDenied: return @"denied";
    case AVAuthorizationStatusRestricted: return @"restricted";
    default: return @"not_determined";
  }
}
static id range_obj(AVFrameRateRange *r) { return r ? @[ @(r.minFrameRate), @(r.maxFrameRate) ] : [NSNull null]; }

static NSArray<AVCaptureDevice *> *all_cameras() {
  NSMutableArray *types = [@[ AVCaptureDeviceTypeBuiltInWideAngleCamera, AVCaptureDeviceTypeExternal ] mutableCopy];
  if (@available(macOS 14.0, *)) [types addObject:AVCaptureDeviceTypeContinuityCamera];
  AVCaptureDeviceDiscoverySession *ds = [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:types
                                                                                               mediaType:AVMediaTypeVideo
                                                                                                position:AVCaptureDevicePositionUnspecified];
  return ds.devices;
}

static NSDictionary *effects_global() {
  NSMutableDictionary *e = [NSMutableDictionary dictionary];
  e[@"center_stage_enabled"] = @(AVCaptureDevice.centerStageEnabled);
  e[@"center_stage_control_mode"] = @[ @"user", @"app", @"cooperative" ][MIN((NSInteger)AVCaptureDevice.centerStageControlMode, 2)];
  e[@"portrait_effect_enabled"] = @(AVCaptureDevice.portraitEffectEnabled);
  e[@"studio_light_enabled"] = @(AVCaptureDevice.studioLightEnabled);
  e[@"reaction_effects_enabled"] = @(AVCaptureDevice.reactionEffectsEnabled);
  e[@"reaction_gestures_enabled"] = @(AVCaptureDevice.reactionEffectGesturesEnabled);
  if (@available(macOS 15.0, *)) e[@"background_replacement_enabled"] = @(AVCaptureDevice.backgroundReplacementEnabled);
  if (@available(macOS 26.2, *)) e[@"edge_light_enabled"] = @(AVCaptureDevice.edgeLightEnabled);
  return e;
}

static NSDictionary *effects_active(AVCaptureDevice *d) {
  NSMutableDictionary *e = [NSMutableDictionary dictionary];
  e[@"center_stage"] = @(d.centerStageActive);
  e[@"portrait"] = @(d.portraitEffectActive);
  e[@"studio_light"] = @(d.studioLightActive);
  e[@"can_react"] = @(d.canPerformReactionEffects);
  e[@"reactions_in_progress"] = @(d.reactionEffectsInProgress.count);
  if (@available(macOS 15.0, *)) e[@"background_replacement"] = @(d.backgroundReplacementActive);
  if (@available(macOS 26.2, *)) e[@"edge_light"] = @(AVCaptureDevice.edgeLightActive);
  return e;
}

static NSDictionary *format_obj(AVCaptureDeviceFormat *f, NSUInteger i) {
  CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(f.formatDescription);
  NSMutableArray *rates = [NSMutableArray array];
  for (AVFrameRateRange *r in f.videoSupportedFrameRateRanges) [rates addObject:range_obj(r)];
  NSMutableDictionary *o = [@{
    @"index" : @(i), @"subtype" : fourcc(CMFormatDescriptionGetMediaSubType(f.formatDescription)),
    @"width" : @(dim.width), @"height" : @(dim.height), @"fps" : rates,
    @"center_stage" : @(f.centerStageSupported), @"center_stage_fps" : range_obj(f.videoFrameRateRangeForCenterStage),
    @"portrait" : @(f.portraitEffectSupported), @"portrait_fps" : range_obj(f.videoFrameRateRangeForPortraitEffect),
    @"studio_light" : @(f.studioLightSupported), @"studio_light_fps" : range_obj(f.videoFrameRateRangeForStudioLight),
    @"reactions" : @(f.reactionEffectsSupported), @"reactions_fps" : range_obj(f.videoFrameRateRangeForReactionEffectsInProgress),
  } mutableCopy];
  if (@available(macOS 15.0, *)) {
    o[@"background_replacement"] = @(f.backgroundReplacementSupported);
    o[@"background_replacement_fps"] = range_obj(f.videoFrameRateRangeForBackgroundReplacement);
  }
  if (@available(macOS 26.2, *)) o[@"edge_light"] = @(f.edgeLightSupported);
  return o;
}

static NSDictionary *device_obj(AVCaptureDevice *d, bool formats) {
  NSMutableDictionary *o = [@{
    @"name" : d.localizedName ?: @"", @"uid" : d.uniqueID ?: @"", @"model" : d.modelID ?: @"", @"manufacturer" : d.manufacturer ?: @"",
    @"type" : d.deviceType ?: @"", @"connected" : @(d.connected), @"in_use_by_another_app" : @(d.inUseByAnotherApplication),
    @"suspended" : @(d.suspended),
    @"active_format" : format_obj(d.activeFormat, [d.formats indexOfObject:d.activeFormat]),
    @"active_min_frame_ms" : @(CMTimeGetSeconds(d.activeVideoMinFrameDuration) * 1000),
    @"active_max_frame_ms" : @(CMTimeGetSeconds(d.activeVideoMaxFrameDuration) * 1000),
    @"exposure_mode" : @(d.exposureMode), @"exposure_modes_supported" : @[ @([d isExposureModeSupported:AVCaptureExposureModeLocked]), @([d isExposureModeSupported:AVCaptureExposureModeAutoExpose]), @([d isExposureModeSupported:AVCaptureExposureModeContinuousAutoExposure]), @([d isExposureModeSupported:AVCaptureExposureModeCustom]) ],
    @"effects_active" : effects_active(d),
  } mutableCopy];
  if (formats) {
    NSMutableArray *fs = [NSMutableArray array];
    NSUInteger i = 0;
    for (AVCaptureDeviceFormat *f in d.formats) [fs addObject:format_obj(f, i++)];
    o[@"formats"] = fs;
  }
  return o;
}

int cmd_list(const Opts &, bool json) {
  NSMutableArray *devs = [NSMutableArray array];
  for (AVCaptureDevice *d in all_cameras()) [devs addObject:device_obj(d, true)];
  NSDictionary *o = @{
    @"permission" : auth_str([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo]),
    @"default" : AVCaptureDevice.systemPreferredCamera.uniqueID ?: [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo].uniqueID ?: @"",
    @"effects" : effects_global(), @"devices" : devs,
  };
  NSData *j = [NSJSONSerialization dataWithJSONObject:o options:(json ? 0 : NSJSONWritingPrettyPrinted) | NSJSONWritingSortedKeys error:nil];
  fwrite(j.bytes, 1, j.length, stdout);
  fputc('\n', stdout);
  return 0;
}

// Returns the status after optionally asking. Never waits more than timeout.
static AVAuthorizationStatus ensure_auth(bool request, double timeout, bool *asked, bool *answered) {
  AVAuthorizationStatus s = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
  *asked = false;
  *answered = false;
  if (s != AVAuthorizationStatusNotDetermined || !request) return s;
  *asked = true;
  dispatch_semaphore_t sem = dispatch_semaphore_create(0);
  __block bool done = false;
  [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo completionHandler:^(BOOL) { done = true; dispatch_semaphore_signal(sem); }];
  dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(timeout * 1e9)));
  *answered = done;
  return [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
}

int cmd_auth(const Opts &o, bool request) {
  bool asked, answered;
  uint64_t t0 = now_ns();
  AVAuthorizationStatus s = ensure_auth(request, o.auth_timeout, &asked, &answered);
  emit(@{@"ev" : @"permission", @"camera" : auth_str(s), @"asked" : @(asked), @"answered" : @(answered),
         @"ms" : @((now_ns() - t0) / 1e6), @"pid" : @(getpid()), @"ppid" : @(getppid())});
  return s == AVAuthorizationStatusAuthorized ? 0 : 3;
}

int cmd_encoders() {
  CFArrayRef list = nullptr;
  VTCopyVideoEncoderList(nullptr, &list);
  NSMutableArray *h264 = [NSMutableArray array];
  for (NSDictionary *e in (__bridge NSArray *)list) {
    if ([e[(id)kVTVideoEncoderList_CodecType] unsignedIntValue] != kCMVideoCodecType_H264) continue;
    NSMutableDictionary *x = [NSMutableDictionary dictionary];
    for (id k in e) x[[k description]] = [e[k] description];
    [h264 addObject:x];
  }
  if (list) CFRelease(list);
  NSMutableDictionary *out = [@{@"h264_encoders" : h264} mutableCopy];
  for (int ll = 0; ll < 2; ll++) {
    NSDictionary *spec = ll ? @{(id)kVTVideoEncoderSpecification_EnableLowLatencyRateControl : @YES} : @{};
    VTCompressionSessionRef s = nullptr;
    OSStatus st = VTCompressionSessionCreate(nullptr, 1280, 720, kCMVideoCodecType_H264, (__bridge CFDictionaryRef)spec, nullptr, nullptr, nullptr, nullptr, &s);
    NSString *key = ll ? @"lowlatency_session" : @"default_session";
    if (st != noErr) { out[key] = @{@"status" : @(st)}; continue; }
    CFDictionaryRef sup = nullptr;
    VTSessionCopySupportedPropertyDictionary(s, &sup);
    NSArray *keys = [[(__bridge NSDictionary *)sup allKeys] sortedArrayUsingSelector:@selector(compare:)];
    CFStringRef eid = nullptr;
    VTSessionCopyProperty(s, kVTCompressionPropertyKey_EncoderID, nullptr, &eid);
    CFBooleanRef hw = nullptr;
    VTSessionCopyProperty(s, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder, nullptr, &hw);
    CFDictionaryRef presets = nullptr;
    VTSessionCopyProperty(s, kVTCompressionPropertyKey_SupportedPresetDictionaries, nullptr, &presets);
    NSMutableDictionary *pd = [NSMutableDictionary dictionary];
    for (id k in (__bridge NSDictionary *)presets) pd[k] = [[(__bridge NSDictionary *)presets objectForKey:k] description];
    out[key] = @{@"encoder_id" : eid ? (__bridge NSString *)eid : @"?", @"hw" : @(hw && CFBooleanGetValue(hw)), @"supported_properties" : keys, @"presets" : pd};
    if (sup) CFRelease(sup);
    if (eid) CFRelease(eid);
    if (hw) CFRelease(hw);
    if (presets) CFRelease(presets);
    VTCompressionSessionInvalidate(s);
    CFRelease(s);
  }
  NSData *j = [NSJSONSerialization dataWithJSONObject:out options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys error:nil];
  fwrite(j.bytes, 1, j.length, stdout);
  fputc('\n', stdout);
  return 0;
}

static double thread_cpu_s() {
  struct timespec ts;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

static bool contains(const std::vector<int> &v, int x) { return std::find(v.begin(), v.end(), x) != v.end(); }

int cmd_run_synthetic(const Opts &o) {
  Encoder enc(o);
  NSMutableDictionary *info = [@{@"ev" : @"start", @"source" : @"synthetic", @"width" : @(o.width), @"height" : @(o.height),
                                 @"fps" : @(o.fps), @"bitrate" : @(o.bitrate), @"lowlatency" : @(o.lowlatency), @"profile" : @(o.profile.c_str()),
                                 @"tag" : @(o.tag.c_str())} mutableCopy];
  if (!enc.open(info)) { emit(info); return 1; }
  emit(info);
  CVPixelBufferPoolRef pool = nullptr;
  NSDictionary *pa = @{(id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
                       (id)kCVPixelBufferWidthKey : @(o.width), (id)kCVPixelBufferHeightKey : @(o.height),
                       (id)kCVPixelBufferIOSurfacePropertiesKey : @{}};
  CVPixelBufferPoolCreate(nullptr, nullptr, (__bridge CFDictionaryRef)pa, &pool);
  const int frames = (int)std::lround(o.seconds * o.fps);
  const uint64_t period = (uint64_t)(1e9 / o.fps);
  std::vector<double> gen_ms;
  double gen_cpu = 0;
  const double cpu0 = cpu_seconds();
  const uint64_t t0 = now_ns();
  double next_stats = o.stats_every;
  for (int i = 0; i < frames; i++) {
    if (!o.fast) mach_wait_until(ns_to_host(t0 + i * period));
    CVPixelBufferRef pb = nullptr;
    CVPixelBufferPoolCreatePixelBuffer(nullptr, pool, &pb);
    double c0 = thread_cpu_s();
    uint64_t g0 = now_ns();
    synth_fill(pb, (uint32_t)i, o.noise);
    uint64_t t = now_ns();
    synth_stamp(pb, (uint32_t)i, t);
    gen_cpu += thread_cpu_s() - c0;
    gen_ms.push_back((t - g0) / 1e6);
    FrameRec m;
    m.f = (uint32_t)i;
    m.cap_ns = t;
    m.arr_ns = t;
    bool fk = (o.force_key_every > 0 && i > 0 && i % o.force_key_every == 0) || contains(o.force_key_at, i);
    enc.encode(pb, m, fk, contains(o.ltr_refresh_at, i));
    CVPixelBufferRelease(pb);
    double el = (now_ns() - t0) / 1e9;
    if (o.stats_every > 0 && el >= next_stats) { enc.tick_stats(el); next_stats += o.stats_every; }
  }
  enc.finish();
  const double wall = (now_ns() - t0) / 1e9;
  const double cpu = cpu_seconds() - cpu0;
  NSMutableDictionary *s = [enc.summary(wall) mutableCopy];
  s[@"ev"] = @"summary";
  s[@"tag"] = @(o.tag.c_str());
  s[@"wall_s"] = @(wall);
  s[@"cpu_pct_total"] = @(std::round(cpu / wall * 1000) / 10);
  s[@"cpu_pct_excl_synth"] = @(std::round((cpu - gen_cpu) / wall * 1000) / 10);
  s[@"synth_gen_ms_p50"] = @(pct(gen_ms, 50));
  s[@"rss_mb"] = @(std::round(rss_mb()));
  emit(s);
  CVPixelBufferPoolRelease(pool);
  return 0;
}

// ---------------- camera ----------------

@interface CamDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property(nonatomic) Encoder *enc;
@property(nonatomic) const Opts *opts;
@property(nonatomic, strong) AVCaptureSession *session;
@property(nonatomic) uint64_t startCall;  // ns, before startRunning
@property(nonatomic) uint64_t startReturn;
@property(atomic) int frames;
@property(atomic) int drops;
@property(atomic) int lastW;
@property(atomic) int lastH;
@end

@implementation CamDelegate {
 @public
  std::mutex mu;
  std::vector<double> lat;        // pts -> arrival, ms
  std::vector<double> lat_all;
  std::vector<double> interval;   // pts deltas, ms
  std::vector<double> interval_all;
  double last_pts;
  bool first;
  uint32_t n;
}
- (instancetype)init {
  if ((self = [super init])) { first = true; last_pts = 0; n = 0; }
  return self;
}
- (void)captureOutput:(AVCaptureOutput *)output didOutputSampleBuffer:(CMSampleBufferRef)sb fromConnection:(AVCaptureConnection *)c {
  uint64_t arr = now_ns();
  CMTime pts = CMSampleBufferGetPresentationTimeStamp(sb);
  CMTime hp = CMSyncConvertTime(pts, self.session.synchronizationClock, CMClockGetHostTimeClock());
  double pts_ns = CMTimeGetSeconds(hp) * 1e9;
  CVPixelBufferRef pb = CMSampleBufferGetImageBuffer(sb);
  uint32_t f;
  {
    std::lock_guard<std::mutex> lk(mu);
    f = n++;
    double l = (arr - pts_ns) / 1e6;
    lat.push_back(l);
    lat_all.push_back(l);
    if (last_pts > 0) { interval.push_back((pts_ns - last_pts) / 1e6); interval_all.push_back((pts_ns - last_pts) / 1e6); }
    last_pts = pts_ns;
  }
  if (first) {
    first = false;
    emit(@{@"ev" : @"first_frame", @"since_start_call_ms" : @((arr - self.startCall) / 1e6),
           @"since_start_return_ms" : @(self.startReturn ? (arr - self.startReturn) / 1e6 : -1),
           @"pts_to_arrival_ms" : @((arr - pts_ns) / 1e6), @"width" : @(CVPixelBufferGetWidth(pb)), @"height" : @(CVPixelBufferGetHeight(pb)),
           @"pixfmt" : fourcc(CVPixelBufferGetPixelFormatType(pb)), @"iosurface" : @(CVPixelBufferGetIOSurface(pb) != nullptr)});
  }
  self.frames = self.frames + 1;
  self.lastW = (int)CVPixelBufferGetWidth(pb);
  self.lastH = (int)CVPixelBufferGetHeight(pb);
  if (self.enc) {
    FrameRec m;
    m.f = f;
    m.cap_ns = (uint64_t)pts_ns;
    m.arr_ns = arr;
    const Opts *o = self.opts;
    bool fk = (o->force_key_every > 0 && f > 0 && f % o->force_key_every == 0) || contains(o->force_key_at, (int)f);
    self.enc->encode(pb, m, fk, contains(o->ltr_refresh_at, (int)f));
  }
}
- (void)captureOutput:(AVCaptureOutput *)output didDropSampleBuffer:(CMSampleBufferRef)sb fromConnection:(AVCaptureConnection *)c {
  self.drops = self.drops + 1;
  CFTypeRef reason = CMGetAttachment(sb, kCMSampleBufferAttachmentKey_DroppedFrameReason, nullptr);
  emit(@{@"ev" : @"cam_drop", @"reason" : reason ? (__bridge NSString *)reason : @"?"});
}
@end

static AVCaptureDevice *pick_device(const Opts &o) {
  NSArray *cams = all_cameras();
  if (o.device.empty()) return AVCaptureDevice.systemPreferredCamera ?: cams.firstObject;
  NSString *want = @(o.device.c_str());
  for (AVCaptureDevice *d in cams)
    if ([d.uniqueID isEqualToString:want] || [d.localizedName.lowercaseString containsString:want.lowercaseString]) return d;
  return nil;
}

static AVCaptureDeviceFormat *pick_format(AVCaptureDevice *d, const Opts &o, NSUInteger *idx) {
  if (o.format_index >= 0 && o.format_index < (int)d.formats.count) { *idx = o.format_index; return d.formats[o.format_index]; }
  FourCharCode want = parse_fourcc(o.pixfmt);
  AVCaptureDeviceFormat *best = nil;
  NSUInteger i = 0;
  for (AVCaptureDeviceFormat *f in d.formats) {
    CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(f.formatDescription);
    bool rate = false;
    for (AVFrameRateRange *r in f.videoSupportedFrameRateRanges)
      if (r.minFrameRate <= o.fps + 0.01 && r.maxFrameRate >= o.fps - 0.01) rate = true;
    if (dim.width == o.width && dim.height == o.height && rate) {
      if (!best || CMFormatDescriptionGetMediaSubType(f.formatDescription) == want) { best = f; *idx = i; }
    }
    i++;
  }
  return best;
}

int cmd_run_camera(const Opts &o) {
  bool asked, answered;
  uint64_t ta = now_ns();
  AVAuthorizationStatus as = ensure_auth(true, o.auth_timeout, &asked, &answered);
  emit(@{@"ev" : @"permission", @"camera" : auth_str(as), @"asked" : @(asked), @"answered" : @(answered), @"ms" : @((now_ns() - ta) / 1e6)});
  if (as != AVAuthorizationStatusAuthorized) {
    emit(@{@"ev" : @"error", @"code" : @"permission", @"msg" : @"camera access not authorized for this process (responsible app)"});
    return 3;
  }
  AVCaptureDevice *dev = pick_device(o);
  if (!dev) { emit(@{@"ev" : @"error", @"code" : @"no_device"}); return 1; }
  NSUInteger fidx = 0;
  AVCaptureDeviceFormat *fmt = pick_format(dev, o, &fidx);
  if (!fmt) { emit(@{@"ev" : @"error", @"code" : @"no_format", @"msg" : @"no format with that size and rate"}); return 1; }
  if (o.effects_probe) emit(@{@"ev" : @"effects_before", @"global" : effects_global(), @"device" : effects_active(dev)});

  Encoder *enc = nullptr;
  NSMutableDictionary *einfo = [NSMutableDictionary dictionary];
  if (o.encode) {
    enc = new Encoder(o);
    if (!enc->open(einfo)) { emit(@{@"ev" : @"error", @"code" : @"encoder", @"info" : einfo}); return 1; }
  }
  AVCaptureSession *session = [AVCaptureSession new];
  NSError *err = nil;
  AVCaptureDeviceInput *in = [AVCaptureDeviceInput deviceInputWithDevice:dev error:&err];
  if (!in) { emit(@{@"ev" : @"error", @"code" : @"input", @"msg" : err.localizedDescription ?: @""}); return 1; }
  AVCaptureVideoDataOutput *out = [AVCaptureVideoDataOutput new];
  out.alwaysDiscardsLateVideoFrames = YES;
  out.videoSettings = @{(id)kCVPixelBufferPixelFormatTypeKey : @(parse_fourcc(o.pixfmt))};
  CamDelegate *del = [CamDelegate new];
  del.enc = enc;
  del.opts = &o;
  del.session = session;
  dispatch_queue_t q = dispatch_queue_create("cam", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
  [out setSampleBufferDelegate:del queue:q];
  uint64_t tc0 = now_ns();
  [session beginConfiguration];
  if ([session canAddInput:in]) [session addInput:in];
  if ([session canAddOutput:out]) [session addOutput:out];
  NSString *lockErr = @"";
  if ([dev lockForConfiguration:&err]) {
    dev.activeFormat = fmt;
    dev.activeVideoMinFrameDuration = CMTimeMake(1000, (int32_t)std::lround(o.fps * 1000));
    if (o.pin_fps) dev.activeVideoMaxFrameDuration = CMTimeMake(1000, (int32_t)std::lround(o.fps * 1000));
    [dev unlockForConfiguration];
  } else lockErr = err.localizedDescription ?: @"?";
  [session commitConfiguration];
  double config_ms = (now_ns() - tc0) / 1e6;
  NSString *cs_err = @"";
  if (o.effects_probe) {
    @try {
      AVCaptureDevice.centerStageControlMode = AVCaptureCenterStageControlModeApp;
      AVCaptureDevice.centerStageEnabled = AVCaptureDevice.centerStageEnabled;  // no change in value: proves control only
    } @catch (NSException *ex) { cs_err = ex.reason ?: ex.name; }
  }
  NSMutableArray *notes = [NSMutableArray array];
  id obs1 = [[NSNotificationCenter defaultCenter] addObserverForName:AVCaptureSessionRuntimeErrorNotification object:session queue:nil usingBlock:^(NSNotification *n) {
    NSError *e = n.userInfo[AVCaptureSessionErrorKey];
    emit(@{@"ev" : @"session_runtime_error", @"code" : @(e.code), @"msg" : e.localizedDescription ?: @""});
  }];
  id obs2 = [[NSNotificationCenter defaultCenter] addObserverForName:AVCaptureSessionWasInterruptedNotification object:session queue:nil usingBlock:^(NSNotification *n) {
    emit(@{@"ev" : @"session_interrupted", @"info" : [n.userInfo description] ?: @""});
  }];
  id obs3 = [[NSNotificationCenter defaultCenter] addObserverForName:AVCaptureSessionInterruptionEndedNotification object:session queue:nil usingBlock:^(NSNotification *n) {
    emit(@{@"ev" : @"session_interruption_ended"});
  }];
  id obs4 = [[NSNotificationCenter defaultCenter] addObserverForName:AVCaptureSessionDidStopRunningNotification object:session queue:nil usingBlock:^(NSNotification *n) {
    emit(@{@"ev" : @"session_did_stop"});
  }];
  (void)notes;
  NSMutableDictionary *start = [@{@"ev" : @"start", @"source" : @"camera", @"device" : device_obj(dev, false), @"format_index" : @(fidx),
                                  @"format" : format_obj(fmt, fidx), @"config_ms" : @(config_ms), @"lock_error" : lockErr,
                                  @"center_stage_app_control_error" : cs_err, @"encoder" : einfo, @"tag" : @(o.tag.c_str())} mutableCopy];
  emit(start);
  const double cpu0 = cpu_seconds();
  const uint64_t t0 = now_ns();
  double stop_ms = 0;
  for (int cyc = 0; cyc < o.cycles; cyc++) {
  if (cyc > 0) [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:o.gap]];
  {
    std::lock_guard<std::mutex> lk(del->mu);
    del->first = true;
  }
  del.startReturn = 0;
  del.startCall = now_ns();
  [session startRunning];
  del.startReturn = now_ns();
  emit(@{@"ev" : @"started", @"cycle" : @(cyc), @"start_running_ms" : @((del.startReturn - del.startCall) / 1e6), @"running" : @(session.running),
         @"active_min_frame_ms" : @(CMTimeGetSeconds(dev.activeVideoMinFrameDuration) * 1000),
         @"active_max_frame_ms" : @(CMTimeGetSeconds(dev.activeVideoMaxFrameDuration) * 1000)});
  const uint64_t tc = del.startCall;
  double next = o.stats_every > 0 ? o.stats_every : 1;
  int lastFrames = del.frames;
  while ((now_ns() - tc) / 1e9 < o.seconds) {
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    double el = (now_ns() - tc) / 1e9;
    if (el >= next) {
      std::vector<double> lat, iv;
      {
        std::lock_guard<std::mutex> lk(del->mu);
        lat.swap(del->lat);
        iv.swap(del->interval);
      }
      int fr = del.frames;
      emit(@{@"ev" : @"cam_stats", @"cycle" : @(cyc), @"t" : @(std::round(el * 100) / 100), @"frames" : @(fr - lastFrames), @"drops" : @(del.drops),
             @"size" : [NSString stringWithFormat:@"%dx%d", del.lastW, del.lastH],
             @"fps_from_pts" : @(iv.empty() ? 0 : std::round(1000.0 / mean(iv) * 10) / 10),
             @"interval_ms_max" : @(iv.empty() ? 0 : *std::max_element(iv.begin(), iv.end())),
             @"pts_to_arrival_ms_p50" : @(pct(lat, 50)), @"pts_to_arrival_ms_max" : @(lat.empty() ? 0 : *std::max_element(lat.begin(), lat.end())),
             @"active_format" : [NSString stringWithFormat:@"%dx%d", CMVideoFormatDescriptionGetDimensions(dev.activeFormat.formatDescription).width, CMVideoFormatDescriptionGetDimensions(dev.activeFormat.formatDescription).height],
             @"active_max_frame_ms" : @(CMTimeGetSeconds(dev.activeVideoMaxFrameDuration) * 1000),
             @"in_use_by_another_app" : @(dev.inUseByAnotherApplication), @"effects" : effects_active(dev)});
      if (enc) enc->tick_stats((now_ns() - t0) / 1e9);
      lastFrames = fr;
      next += o.stats_every > 0 ? o.stats_every : 1;
    }
  }
  uint64_t ts0 = now_ns();
  [session stopRunning];
  stop_ms = (now_ns() - ts0) / 1e6;
  {
    std::lock_guard<std::mutex> lk(del->mu);
    del->last_pts = 0;
  }
  emit(@{@"ev" : @"stopped", @"cycle" : @(cyc), @"stop_running_ms" : @(stop_ms)});
  }
  const double wall = (now_ns() - t0) / 1e9;
  const double cpu = cpu_seconds() - cpu0;
  if (o.effects_probe) {
    @try { AVCaptureDevice.centerStageControlMode = AVCaptureCenterStageControlModeUser; } @catch (NSException *) {}
  }
  for (id ob in @[ obs1, obs2, obs3, obs4 ]) [[NSNotificationCenter defaultCenter] removeObserver:ob];
  NSMutableDictionary *s = [NSMutableDictionary dictionary];
  if (enc) {
    enc->finish();
    [s addEntriesFromDictionary:enc->summary(wall)];
  }
  {
    std::lock_guard<std::mutex> lk(del->mu);
    s[@"cam_pts_to_arrival_ms"] = @{@"p50" : @(pct(del->lat_all, 50)), @"p95" : @(pct(del->lat_all, 95)), @"max" : @(pct(del->lat_all, 100)), @"mean" : @(mean(del->lat_all))};
    s[@"cam_interval_ms"] = @{@"p50" : @(pct(del->interval_all, 50)), @"p95" : @(pct(del->interval_all, 95)), @"max" : @(pct(del->interval_all, 100))};
  }
  s[@"ev"] = @"summary";
  s[@"tag"] = @(o.tag.c_str());
  s[@"cam_frames"] = @(del.frames);
  s[@"cam_drops"] = @(del.drops);
  s[@"wall_s"] = @(wall);
  s[@"stop_running_ms"] = @(stop_ms);
  s[@"cpu_pct_total"] = @(std::round(cpu / wall * 1000) / 10);
  s[@"rss_mb"] = @(std::round(rss_mb()));
  if (o.effects_probe) s[@"effects_after"] = effects_global();
  emit(s);
  delete enc;
  return 0;
}
