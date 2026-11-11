// SPDX-License-Identifier: GPL-3.0-only
// macOS video backend: AVFoundation camera inventory, preferred camera, permission state, and the capture
// source (one AVCaptureSession per camera device, shared by every stream on it; camera PHASE2 §4.11).
// Reading the inventory and the authorization status never prompts; only the backend's
// request_permission() (Beamer's `permission_request`) calls requestAccessForMediaType.
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <map>

#include "video_backend_mac.h"

namespace beamer::audio::macvideo {

namespace {
std::atomic<int> g_capture_creations{0};
std::atomic<int> g_sessions_running{0};

NSArray<AVCaptureDevice*>* all_cameras() {
  NSMutableArray* types = [NSMutableArray arrayWithObject:AVCaptureDeviceTypeBuiltInWideAngleCamera];
  if (@available(macOS 14.0, *)) {
    [types addObject:AVCaptureDeviceTypeExternal];
    [types addObject:AVCaptureDeviceTypeContinuityCamera];
  } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [types addObject:AVCaptureDeviceTypeExternalUnknown];
#pragma clang diagnostic pop
  }
  AVCaptureDeviceDiscoverySession* ds =
      [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:types
                                                             mediaType:AVMediaTypeVideo
                                                              position:AVCaptureDevicePositionUnspecified];
  return ds.devices ?: @[];
}

AVCaptureDevice* preferred_device() {
  // systemPreferredCamera is nil until the system has a preference (seen on this Mac with one built-in
  // camera): fall back to the default video device, then to the first camera.
  AVCaptureDevice* d = AVCaptureDevice.systemPreferredCamera;
  if (!d) d = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
  if (!d) d = all_cameras().firstObject;
  return d;
}

std::string transport_of(AVCaptureDevice* d) {
  if (@available(macOS 14.0, *)) {
    if ([d.deviceType isEqualToString:AVCaptureDeviceTypeContinuityCamera]) return "continuity";
  }
  switch ((uint32_t)d.transportType) {
    case 'bltn': return "builtin";
    case 'usb ': return "usb";
    case 'virt': return "virtual";
    default: break;
  }
  if ([d.deviceType isEqualToString:AVCaptureDeviceTypeBuiltInWideAngleCamera]) return "builtin";
  return "other";
}

std::vector<FormatCand> candidates(AVCaptureDevice* d) {
  std::vector<FormatCand> v;
  for (AVCaptureDeviceFormat* f in d.formats) {
    CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(f.formatDescription);
    double mx = 0;
    for (AVFrameRateRange* r in f.videoSupportedFrameRateRanges) mx = std::max(mx, r.maxFrameRate);
    FormatCand c;
    c.w = dim.width;
    c.h = dim.height;
    c.max_fps = mx;
    c.subtype = CMFormatDescriptionGetMediaSubType(f.formatDescription);
    v.push_back(c);
  }
  return v;
}

std::string fmt_str(int w, int h, double fps) {
  char b[48];
  snprintf(b, sizeof b, "%dx%d@%d", w, h, (int)std::lround(fps));
  return b;
}

}  // namespace

void vlog(const char* level, const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  fprintf(stderr, "%s: video: %s\n", level, buf);
}

int capture_device_creations() { return g_capture_creations.load(); }
int capture_sessions_running() { return g_sessions_running.load(); }

int64_t host_time_to_ns(CMTime t) {
  // The host time clock is mach_absolute_time, which is now_ns()'s clock on macOS (video_check selftest).
  CMTime ns = CMTimeConvertScale(t, 1000000000, kCMTimeRoundingMethod_RoundHalfAwayFromZero);
  return ns.value;
}

std::string camera_permission() {
  switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo]) {
    case AVAuthorizationStatusAuthorized: return "granted";
    case AVAuthorizationStatusDenied: return "denied";
    case AVAuthorizationStatusRestricted: return "restricted";
    case AVAuthorizationStatusNotDetermined: return "undetermined";
  }
  return "unknown";
}

std::string preferred_camera_key() {
  AVCaptureDevice* d = preferred_device();
  return d ? std::string(d.uniqueID.UTF8String ?: "") : std::string();
}

CameraSnapshot camera_snapshot() {
  CameraSnapshot s;
  s.preferred = preferred_camera_key();
  for (AVCaptureDevice* d in all_cameras()) {
    CameraInfo c;
    c.key = d.uniqueID.UTF8String ?: "";
    c.name = d.localizedName.UTF8String ?: "";
    c.transport = transport_of(d);
    c.preferred = !s.preferred.empty() && c.key == s.preferred;
    std::map<std::pair<int, int>, int> seen;
    for (const FormatCand& f : candidates(d)) {
      auto k = std::make_pair(f.w, f.h);
      auto it = seen.find(k);
      if (it != seen.end()) {
        c.native[it->second].fps = std::max(c.native[it->second].fps, (int)std::lround(f.max_fps));
        continue;
      }
      if (c.native.size() >= 16) continue;
      seen[k] = (int)c.native.size();
      c.native.push_back(VideoFormat{f.w, f.h, (int)std::lround(f.max_fps)});
    }
    s.cameras.push_back(std::move(c));
  }
  s.permission = camera_permission();
  return s;
}

void* find_camera(const std::string& key) {
  if (key.empty()) return (__bridge void*)preferred_device();
  NSString* want = @(key.c_str());
  for (AVCaptureDevice* d in all_cameras())
    if ([d.uniqueID isEqualToString:want]) return (__bridge void*)d;
  return nullptr;
}

}  // namespace beamer::audio::macvideo

using beamer::audio::macvideo::Frame;
namespace mv = beamer::audio::macvideo;

namespace beamer::audio::macvideo {
class CameraSource;
}

@interface BMVideoDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate> {
 @public
  beamer::audio::macvideo::CameraSource* owner;  // video queue only; cleared before the source goes
}
@end

namespace beamer::audio::macvideo {

// Blocks on ctl_q_ (start, retry, notifications, the 1 s timer) hold a weak_ptr and lock it for their whole run
// (R3 #4): the destructor does not wait for ctl_q_, so a raw `self` checked against `alive` could be freed between
// the check and the use. A block that ends up holding the last reference runs the destructor on ctl_q_, which is
// fine: it only queues the stop there.
class CameraSource : public Source, public std::enable_shared_from_this<CameraSource> {
 public:
  CameraSource() = default;
  ~CameraSource() override;
  bool open(AVCaptureDevice* dev, int wt, int ht, std::string* err);
  void add(FrameListener* l) override {
    std::lock_guard<std::mutex> lk(mu_);
    listeners_.push_back(l);
  }
  void remove(FrameListener* l) override {
    std::lock_guard<std::mutex> lk(mu_);  // held during a delivery: returns once it is over
    listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), l), listeners_.end());
  }
  std::string key() const override { return key_; }
  std::string native() const override { return native_; }
  bool lost() const override { return lost_.load(); }
  void on_sample(CMSampleBufferRef sb);

 private:
  void event(const std::string& what, const std::string& detail) {
    std::lock_guard<std::mutex> lk(mu_);
    auto ls = listeners_;
    for (FrameListener* l : ls) l->on_source_event(what, detail);
  }
  void tick();         // ctl queue, every 1 s: effects, by_other
  void retry_start();  // ctl queue, every 2 s while the session is not running

  std::string key_, native_;
  AVCaptureDevice* dev_ = nil;
  AVCaptureSession* session_ = nil;
  AVCaptureDeviceInput* input_ = nil;
  AVCaptureVideoDataOutput* output_ = nil;
  BMVideoDelegate* delegate_ = nil;
  dispatch_queue_t video_q_ = nil, ctl_q_ = nil;
  dispatch_source_t timer_ = nil;
  CMClockRef clock_ = nullptr;
  std::vector<id> observers_;
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
  std::atomic<bool> lost_{false};
  std::atomic<bool> interrupted_{false};
  std::mutex mu_;  // listeners_, deliveries
  std::vector<FrameListener*> listeners_;
  std::map<std::pair<int, int>, std::unique_ptr<Scaler>> scalers_;  // video queue only
  std::string effects_ = "?", by_other_ = "?";                       // ctl queue only
  uint32_t index_ = 0;                                               // video queue only
  int last_w_ = 0, last_h_ = 0;                                      // video queue only
};

}  // namespace beamer::audio::macvideo

@implementation BMVideoDelegate
- (void)captureOutput:(AVCaptureOutput*)output didOutputSampleBuffer:(CMSampleBufferRef)sb fromConnection:(AVCaptureConnection*)c {
  if (owner) owner->on_sample(sb);
}
@end

namespace beamer::audio::macvideo {

bool CameraSource::open(AVCaptureDevice* dev, int wt, int ht, std::string* err) {
  dev_ = dev;
  key_ = dev.uniqueID.UTF8String ?: "";
  std::vector<FormatCand> cands = candidates(dev);
  int pick = choose_format(cands, wt, ht);
  if (pick < 0) {
    if (err) *err = "no_camera: the camera has no video format";
    return false;
  }
  AVCaptureDeviceFormat* fmt = dev.formats[pick];
  const bool has30 = cands[pick].max_fps >= 29.5;
  native_ = fmt_str(cands[pick].w, cands[pick].h, has30 ? 30 : cands[pick].max_fps);

  NSError* e = nil;
  input_ = [AVCaptureDeviceInput deviceInputWithDevice:dev error:&e];
  g_capture_creations++;
  if (!input_) {
    if (err) {
      bool perm = e && e.code == AVErrorApplicationIsNotAuthorizedToUseDevice;
      *err = std::string(perm ? "permission: " : "internal: ") + "camera input: " +
             (e ? e.localizedDescription.UTF8String : "unknown error");
    }
    return false;
  }
  session_ = [[AVCaptureSession alloc] init];
  g_capture_creations++;
  output_ = [[AVCaptureVideoDataOutput alloc] init];
  output_.videoSettings = @{(id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)};
  output_.alwaysDiscardsLateVideoFrames = YES;
  delegate_ = [[BMVideoDelegate alloc] init];
  delegate_->owner = this;
  dispatch_queue_attr_t ui = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
  video_q_ = dispatch_queue_create("beamer.video.capture", ui);
  ctl_q_ = dispatch_queue_create("beamer.video.camera", DISPATCH_QUEUE_SERIAL);
  [output_ setSampleBufferDelegate:delegate_ queue:video_q_];

  [session_ beginConfiguration];
  if (![session_ canAddInput:input_] || ![session_ canAddOutput:output_]) {
    [session_ commitConfiguration];
    if (err) *err = "internal: the capture session refused the camera";
    return false;
  }
  [session_ addInput:input_];
  [session_ addOutput:output_];
  // The format is picked here, never scaled by AVFoundation (C1). On macOS the session resets the active
  // format when it starts running (there is no InputPriority preset), unless the device stays locked for
  // configuration until startRunning has returned (seen here: unlocked, the camera delivered its default
  // 1920x1080 instead of the chosen 1280x720). So: lock, set, commit, start, re-check, unlock.
  const bool locked = [dev lockForConfiguration:&e];
  if (locked) {
    dev.activeFormat = fmt;
    if (has30) dev.activeVideoMinFrameDuration = CMTimeMake(1, 30);
    // activeVideoMaxFrameDuration stays the device default (auto exposure may drop to 15 fps in low
    // light) until Alex's low-light test decides (§4.11, §11 open point 3).
  } else {
    vlog("warn", "camera %s: lockForConfiguration failed (%s); default format kept", key_.c_str(),
         e.localizedDescription.UTF8String);
  }
  [session_ commitConfiguration];
  if (@available(macOS 13.0, *)) clock_ = session_.synchronizationClock;
  if (clock_) CFRetain(clock_);

  // interruption / runtime error -> interrupted, retried every 2 s; device gone -> lost
  NSNotificationCenter* nc = NSNotificationCenter.defaultCenter;
  std::shared_ptr<std::atomic<bool>> alive = alive_;
  std::weak_ptr<CameraSource> weak = weak_from_this();  // make_camera_source owns it through make_shared
  NSOperationQueue* oq = [[NSOperationQueue alloc] init];
  oq.underlyingQueue = ctl_q_;
  oq.maxConcurrentOperationCount = 1;
  auto on = [&](NSNotificationName n, id obj, void (^block)(NSNotification*)) {
    observers_.push_back([nc addObserverForName:n object:obj queue:oq usingBlock:block]);
  };
  on(AVCaptureSessionRuntimeErrorNotification, session_, ^(NSNotification* note) {
    auto self = weak.lock();
    if (!self || !*alive) return;
    NSError* er = note.userInfo[AVCaptureSessionErrorKey];
    vlog("warn", "camera %s: capture runtime error: %s", self->key_.c_str(), er.localizedDescription.UTF8String ?: "?");
    if (!self->interrupted_.exchange(true)) self->event("interrupted", "runtime_error");
    self->retry_start();
  });
  on(AVCaptureSessionWasInterruptedNotification, session_, ^(NSNotification*) {
    auto self = weak.lock();
    if (!self || !*alive) return;
    if (!self->interrupted_.exchange(true)) self->event("interrupted", "interrupted");
  });
  on(AVCaptureSessionInterruptionEndedNotification, session_, ^(NSNotification*) {
    auto self = weak.lock();
    if (!self || !*alive) return;
    if (self->interrupted_.exchange(false)) self->event("resumed", "");
  });
  on(AVCaptureDeviceWasDisconnectedNotification, dev, ^(NSNotification*) {
    auto self = weak.lock();
    if (!self || !*alive) return;
    if (!self->lost_.exchange(true)) self->event("lost", "gone");
  });

  // start at once (Deviation 3: the camera warms up while the open round trip runs)
  AVCaptureSession* s = session_;
  const std::string key = key_;  // copies: the block may outlive the source (it is not kept alive by it)
  dispatch_async(ctl_q_, ^{
    [s startRunning];  // 0.2-0.8 s; the source may be destroyed meanwhile (its stop is queued behind this block)
    if (locked) {
      if (dev.activeFormat != fmt) {
        vlog("info", "camera %s: the session changed the format at start; set again", key.c_str());
        dev.activeFormat = fmt;
        if (has30) dev.activeVideoMinFrameDuration = CMTimeMake(1, 30);
      }
      [dev unlockForConfiguration];
    }
    if (s.isRunning) {
      g_sessions_running++;
    } else if (auto self = weak.lock()) {
      if (*alive) self->retry_start();
    }
  });
  timer_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, ctl_q_);
  dispatch_source_set_timer(timer_, dispatch_time(DISPATCH_TIME_NOW, 200 * NSEC_PER_MSEC), NSEC_PER_SEC, 50 * NSEC_PER_MSEC);
  dispatch_source_set_event_handler(timer_, ^{
    auto self = weak.lock();
    if (self && *alive) self->tick();
  });
  dispatch_resume(timer_);
  return true;
}

void CameraSource::retry_start() {
  std::shared_ptr<std::atomic<bool>> alive = alive_;
  std::weak_ptr<CameraSource> weak = weak_from_this();
  AVCaptureSession* s = session_;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), ctl_q_, ^{
    auto self = weak.lock();
    if (!self || !*alive || self->lost_) return;
    if (s.isRunning) {
      if (self->interrupted_.exchange(false)) self->event("resumed", "");
      return;
    }
    [s startRunning];
    if (s.isRunning) {
      g_sessions_running++;
      if (self->interrupted_.exchange(false)) self->event("resumed", "");
    } else {
      self->retry_start();
    }
  });
}

void CameraSource::tick() {
  std::string fx;
  auto add = [&](bool on, const char* name) {
    if (!on) return;
    if (!fx.empty()) fx += ",";
    fx += name;
  };
  add(dev_.centerStageActive, "center_stage");
  add(dev_.portraitEffectActive, "portrait");
  add(dev_.studioLightActive, "studio_light");
  if (@available(macOS 15.0, *)) add(dev_.backgroundReplacementActive, "background");
  if (@available(macOS 14.0, *)) add(dev_.reactionEffectsInProgress.count > 0, "reactions");
  if (@available(macOS 26.2, *)) add(AVCaptureDevice.edgeLightActive, "edge_light");
  if (fx != effects_) {
    effects_ = fx;
    event("effects", fx);
  }
  std::string other = dev_.inUseByAnotherApplication ? "true" : "false";
  if (other != by_other_) {
    by_other_ = other;
    event("by_other", other);
  }
  if (!dev_.connected && !lost_.exchange(true)) event("lost", "gone");
}

void CameraSource::on_sample(CMSampleBufferRef sb) {
  CVPixelBufferRef pb = CMSampleBufferGetImageBuffer(sb);
  if (!pb) return;
  CMTime pts = CMSampleBufferGetPresentationTimeStamp(sb);
  if (clock_) pts = CMSyncConvertTime(pts, clock_, CMClockGetHostTimeClock());
  const uint64_t cap = (uint64_t)host_time_to_ns(pts);
  const int w = (int)CVPixelBufferGetWidth(pb), h = (int)CVPixelBufferGetHeight(pb);
  const CropRect crop = crop_16x9(w, h);
  if (w != last_w_ || h != last_h_) {  // what the camera really delivers (it may differ from the format)
    vlog("info", "camera %s delivers %dx%d '%.4s'", key_.c_str(), w, h,
         [&] { static thread_local char c[5]; uint32_t t = CVPixelBufferGetPixelFormatType(pb); c[0] = t >> 24; c[1] = t >> 16; c[2] = t >> 8; c[3] = t; c[4] = 0; return c; }());
    last_w_ = w;
    last_h_ = h;
  }
  std::lock_guard<std::mutex> lk(mu_);
  const uint32_t idx = index_++;
  if (listeners_.empty()) return;
  std::map<std::pair<int, int>, CVPixelBufferRef> memo;
  Frame fr;
  fr.native = pb;
  fr.capture_ns = cap;
  fr.index = idx;
  fr.scaled = [&](int ow, int oh, double* ms) -> CVPixelBufferRef {
    if (ms) *ms = 0;
    if (ow == w && oh == h) return pb;
    auto k = std::make_pair(ow, oh);
    auto it = memo.find(k);
    if (it != memo.end()) return it->second;
    auto& sc = scalers_[k];
    if (!sc) sc = std::make_unique<Scaler>(ow, oh);
    std::string err;
    CVPixelBufferRef out = sc->convert(pb, crop, &err);
    if (!out) vlog("warn", "camera scaler: %s", err.c_str());
    if (ms) *ms = sc->last_ms();
    memo[k] = out;
    return out;
  };
  auto ls = listeners_;
  for (FrameListener* l : ls) l->on_captured(fr);
  for (auto& kv : memo)
    if (kv.second) CVPixelBufferRelease(kv.second);
}

CameraSource::~CameraSource() {
  *alive_ = false;
  for (id o : observers_) [NSNotificationCenter.defaultCenter removeObserver:o];
  observers_.clear();
  if (timer_) dispatch_source_cancel(timer_);
  if (video_q_) {
    BMVideoDelegate* d = delegate_;
    dispatch_sync(video_q_, ^{
      if (d) d->owner = nullptr;
    });
  }
  // Stop the capture (camera light) asynchronously: stopRunning blocks for a while, the caller is the
  // engine's control thread. ctl_q_ is serial, so a pending startRunning finishes first.
  AVCaptureSession* s = session_;
  AVCaptureVideoDataOutput* out = output_;
  std::string key = key_;
  if (s && ctl_q_) {
    const int64_t t0 = now_ns();
    dispatch_async(ctl_q_, ^{
      const bool was = s.isRunning;
      [out setSampleBufferDelegate:nil queue:nil];
      [s stopRunning];
      if (was) g_sessions_running--;
      vlog("info", "camera %s stopped %.0f ms after its last stream", key.c_str(), (now_ns() - t0) / 1e6);
    });
  }
  if (clock_) CFRelease(clock_);
}

std::shared_ptr<Source> make_camera_source(void* device, int wt, int ht, std::string* err) {
  AVCaptureDevice* dev = (__bridge AVCaptureDevice*)device;
  if (!dev) {
    if (err) *err = "no_camera: no camera";
    return nullptr;
  }
  auto s = std::make_shared<CameraSource>();
  if (!s->open(dev, wt, ht, err)) return nullptr;
  return s;
}

}  // namespace beamer::audio::macvideo
