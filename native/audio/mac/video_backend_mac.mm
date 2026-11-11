// SPDX-License-Identifier: GPL-3.0-only
// The macOS VideoBackend (camera PHASE2 §2.4, §4.11): camera handles (one capture tap + one encoder session
// each, the capture shared per device), the LTR token map keyed by the engine's frame numbers, the standby
// encoder session, the permission guard, the preferred-camera follower and the inventory notifications.
#import <AVFoundation/AVFoundation.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <set>

#include "video_backend_mac.h"

// KVO target for +[AVCaptureDevice systemPreferredCamera] (documented as key-value observable).
@interface BMPreferredObserver : NSObject
- (instancetype)initWithBlock:(void (^)(void))block;
@end
@implementation BMPreferredObserver {
  void (^block_)(void);
}
- (instancetype)initWithBlock:(void (^)(void))block {
  if ((self = [super init])) block_ = [block copy];
  return self;
}
- (void)observeValueForKeyPath:(NSString*)keyPath ofObject:(id)object change:(NSDictionary*)change context:(void*)context {
  if (block_) block_();
}
@end

namespace beamer::audio {
namespace macvideo {

namespace {

std::string fmt1(double v) {
  char b[32];
  snprintf(b, sizeof b, "%.1f", v);
  return b;
}

class MacVideoBackend;

// One video send stream: a listener on a capture source and one VT session (§4.11).
// Locks: mu_ (recursive: encoder, format, request flags) before out_mu_ (token map, pending acks; never held
// across a call out). sink_mu_ (recursive) serialises every EncoderSink call and guards closed_; no
// CameraHandle method takes it, so the engine may call any of them from inside on_frame / on_camera_event.
class MacCameraHandle : public CameraHandle, public FrameListener {
 public:
  MacCameraHandle(MacVideoBackend* be, const CameraSpec& spec, EncoderSink* sink, std::unique_ptr<Encoder> enc,
                  std::shared_ptr<Source> src)
      : be_(be), spec_(spec), sink_(sink), fmt_(spec.fmt), enc_(std::move(enc)), src_(std::move(src)) {
    open_ns_ = now_ns();
    fps_ = spec.fmt.fps == 15 ? 15 : 30;
    wire_encoder(enc_.get());
  }
  ~MacCameraHandle() override;

  void begin(const std::string& encoder_detail) {
    emit("native", src_->native());
    emit("encoder", encoder_detail);
    src_->add(this);
    timer_q_ = dispatch_queue_create("beamer.video.stream", DISPATCH_QUEUE_SERIAL);
    timer_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, timer_q_);
    dispatch_source_set_timer(timer_, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, 20 * NSEC_PER_MSEC);
    MacCameraHandle* self = this;
    dispatch_source_set_event_handler(timer_, ^{
      self->tick();
    });
    dispatch_resume(timer_);
  }

  // ---- CameraHandle ----
  void start_encoding() override {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (started_) return;
    started_ = true;
    start_ns_ = now_ns();
    want_idr_ = true;  // the first AU after this call is an IDR
  }
  void request_idr() override {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    want_idr_ = true;
  }
  void request_ltr_refresh() override {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (acked_since_idr_.load()) want_refresh_ = true;
    else want_idr_ = true;  // no acknowledged LTR since the last IDR: an IDR inside (§2.4)
  }
  void acknowledge_ltr(uint32_t frame) override {
    std::lock_guard<std::mutex> lk(out_mu_);
    if (have_idr_ && frame < last_idr_frame_) return;  // before the newest IDR: gone on both sides
    auto it = tokens_.find(frame);
    if (it == tokens_.end() || it->second.acked) return;  // unknown (other session) or already passed
    it->second.acked = true;
    acks_pending_.push_back(it->second.token);
    acked_since_idr_ = true;
  }
  void set_bitrate(int bps) override {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (bps <= 0) return;
    spec_.bitrate = bps;
    if (enc_) enc_->set_bitrate(bps);
  }
  void set_fps(int fps) override {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    fps_ = fps <= 15 ? 15 : 30;
    if (enc_) enc_->set_fps(fps_);
  }
  void set_format(const VideoFormat& f) override;
  void set_input_paused(bool p) override {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    paused_ = p;
  }
  std::string device_in_use() const override {
    std::lock_guard<std::mutex> lk(src_mu_);
    return src_->key();
  }

  // ---- FrameListener (source delivery thread) ----
  void on_captured(const Frame& f) override;
  void on_source_event(const std::string& what, const std::string& detail) override;

  // ---- backend ----
  bool follows_preferred() const { return spec_.device.empty() && spec_.test.empty(); }
  void switch_source(std::shared_ptr<Source> next);  // backend queue; preferred camera changed
  std::shared_ptr<Source> source() const {
    std::lock_guard<std::mutex> lk(src_mu_);
    return src_;
  }
  int fmt_w() {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    return fmt_.w;
  }
  int fmt_h() {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    return fmt_.h;
  }
  void emit_lost() { emit("lost", "gone"); }

 private:
  struct Tok {
    int64_t token;
    bool acked;
  };
  void wire_encoder(Encoder* e) {
    if (!e) return;
    e->set_output([this](const EncOut& o) { on_encoded(o); });
  }
  void on_encoded(const EncOut& o) {
    std::lock_guard<std::recursive_mutex> lk(sink_mu_);
    if (closed_) return;
    EncodedFrame ef;
    ef.data = o.data;
    ef.len = o.len;
    ef.capture_ns = o.capture_ns;
    ef.idr = o.idr;
    ef.refresh = o.refresh;
    ef.ltr = o.token >= 0;
    ef.w = o.w;
    ef.h = o.h;
    const uint32_t n = sink_->on_frame(ef);
    std::lock_guard<std::mutex> lk2(out_mu_);
    if (o.idr) {  // an IDR clears every reference on both sides (§4.3)
      tokens_.clear();
      acks_pending_.clear();
      acked_since_idr_ = false;
      have_idr_ = true;
      last_idr_frame_ = n;
    }
    if (o.token >= 0) {
      tokens_[n] = Tok{o.token, false};
      while (tokens_.size() > 64) tokens_.erase(tokens_.begin());
    }
    aus_++;
    if (!first_au_ns_) first_au_ns_ = now_ns();
  }
  void emit(const std::string& what, const std::string& detail) {
    std::lock_guard<std::recursive_mutex> lk(sink_mu_);
    if (!closed_) sink_->on_camera_event(what, detail);
  }
  void tick() {
    const int n = captured_.exchange(0);
    emit("fps", fmt1(n));
  }

  MacVideoBackend* be_;
  CameraSpec spec_;
  EncoderSink* sink_;
  // mu_: encoder, format, flags
  std::recursive_mutex mu_;
  VideoFormat fmt_;
  std::unique_ptr<Encoder> enc_;
  bool started_ = false, paused_ = false, want_idr_ = false, want_refresh_ = false;
  int fps_ = 30;
  uint64_t parity_ = 0;  // fps 15: every other captured frame
  std::recursive_mutex sink_mu_;
  bool closed_ = false;
  // out_mu_: token map
  std::mutex out_mu_;
  std::map<uint32_t, Tok> tokens_;  // frame number -> LTR token, last 64
  std::vector<int64_t> acks_pending_;
  std::atomic<bool> acked_since_idr_{false};
  bool have_idr_ = false;
  uint32_t last_idr_frame_ = 0;
  // source
  mutable std::mutex src_mu_;
  std::shared_ptr<Source> src_;
  // stats
  std::atomic<int> captured_{0};
  std::atomic<bool> first_frame_{false};
  int64_t open_ns_ = 0, start_ns_ = 0;
  std::atomic<int64_t> first_au_ns_{0};
  std::atomic<uint64_t> aus_{0};
  dispatch_queue_t timer_q_ = nil;
  dispatch_source_t timer_ = nil;

 public:
  // video_check hooks
  int64_t open_ns() const { return open_ns_; }
};

class MacVideoBackend : public VideoBackend {
 public:
  explicit MacVideoBackend(const std::string& options) {
    dev_ = options.find("dev") != std::string::npos;
    prewarm_ = options.find("standby=prewarm") != std::string::npos;
    if (const char* e = getenv("BEAMER_VIDEO_STANDBY")) prewarm_ = std::string(e) == "prewarm";
    q_ = dispatch_queue_create("beamer.video.backend", DISPATCH_QUEUE_SERIAL);
    standby_q_ = dispatch_queue_create("beamer.video.standby", DISPATCH_QUEUE_SERIAL);
  }
  ~MacVideoBackend() override {
    *alive_ = false;
    for (id o : observers_) [NSNotificationCenter.defaultCenter removeObserver:o];
    if (kvo_) [AVCaptureDevice removeObserver:kvo_ forKeyPath:@"systemPreferredCamera"];
    if (perm_timer_) dispatch_source_cancel(perm_timer_);
    dispatch_sync(q_, ^{});
    dispatch_sync(standby_q_, ^{});
    std::lock_guard<std::mutex> lk(standby_mu_);
    standby_.reset();
  }

  void set_listener(VideoBackendListener* l) override {
    {
      std::lock_guard<std::mutex> lk(mu_);
      listener_ = l;
    }
    start_watching();
  }
  std::vector<std::string> features() override {
    std::vector<std::string> f{"camera_capture"};
    if (dev_) f.push_back("camera_synth");
    return f;
  }
  CameraSnapshot cameras() override { return camera_snapshot(); }

  void request_permission() override {
    // Only ever called for an action on the Mac (Beamer's `permission_request`, §9.1).
    std::shared_ptr<std::atomic<bool>> alive = alive_;
    MacVideoBackend* self = this;
    vlog("info", "asking macOS for camera access");
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                             completionHandler:^(BOOL granted) {
                               vlog("info", "camera access answer: %s", granted ? "granted" : "not granted");
                               dispatch_async(self->q_, ^{
                                 if (*alive) self->permission_check(true);
                               });
                             }];
  }

  std::unique_ptr<CameraHandle> open_camera(const CameraSpec& spec, EncoderSink* sink, std::string* err) override;

  void set_encoder_standby(const EncoderStandby& sb) override {
    std::lock_guard<std::mutex> lk(standby_mu_);
    standby_spec_ = sb;
    if (!sb.on) {
      prewarmed_ = VideoFormat{};
      if (standby_) {
        std::shared_ptr<Encoder> old(std::move(standby_));
        dispatch_async(standby_q_, ^{
          (void)old;  // released off the caller's thread
        });
        vlog("info", "standby encoder released");
      }
      return;
    }
    ensure_standby_locked();
  }

  VcamState vcam() override {
    VcamState v;
    v.install = "unsupported";
    return v;
  }
  bool vcam_enable(bool, std::string* err) override {
    if (err) *err = "unsupported: no virtual camera on macOS";
    return false;
  }
  void vcam_placeholders(const std::map<std::string, Placeholder>&, const std::string&) override {}
  std::unique_ptr<VideoSinkHandle> open_sink(VideoSinkListener*, std::string* err) override {
    if (err) *err = "unsupported: no virtual camera on macOS";
    return nullptr;
  }

  // ---- from handles ----
  void handle_closed(MacCameraHandle* h) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      handles_.erase(h);
    }
    std::lock_guard<std::mutex> lk(standby_mu_);
    if (standby_spec_.on) ensure_standby_locked();  // a new standby right after, while warm holds (§4.11)
  }
  void handle_lost(MacCameraHandle* h) {
    MacVideoBackend* self = this;
    std::shared_ptr<std::atomic<bool>> alive = alive_;
    dispatch_async(q_, ^{
      if (*alive) self->resolve_lost(h);
    });
  }

  // ---- video_check hooks ----
  bool prewarm_mode() const { return prewarm_; }
  bool standby_ready() {
    std::lock_guard<std::mutex> lk(standby_mu_);
    return standby_ != nullptr;
  }
  void wait_standby() { dispatch_sync(standby_q_, ^{}); }

 private:
  std::shared_ptr<Source> source_for(void* dev, int wt, int ht, std::string* err) {
    AVCaptureDevice* d = (__bridge AVCaptureDevice*)dev;
    const std::string key = d.uniqueID.UTF8String ?: "";
    std::lock_guard<std::mutex> lk(sources_mu_);
    auto it = sources_.find(key);
    if (it != sources_.end())
      if (auto s = it->second.lock()) {
        if (!s->lost()) return s;
      }
    auto s = make_camera_source(dev, wt, ht, err);
    if (s) sources_[key] = s;
    return s;
  }

  void ensure_standby_locked() {  // standby_mu_ held
    const EncoderStandby sb = standby_spec_;
    if (prewarm_) {
      // §4.11 fallback: create and invalidate one session when warm sets; keeps the XPC service warm at no
      // idle cost.
      if (prewarmed_.w == sb.fmt.w && prewarmed_.h == sb.fmt.h) return;
      prewarmed_ = sb.fmt;
      dispatch_async(standby_q_, ^{
        std::string e;
        double ms = 0;
        const bool cold = Encoder::sessions_created() == 0;
        auto enc = Encoder::create(sb.fmt.w, sb.fmt.h, sb.bitrate, 30, true, &e, &ms);
        vlog("info", "encoder pre-warmed %dx%d (%s, %.1f ms)%s%s", sb.fmt.w, sb.fmt.h, cold ? "cold" : "warm", ms,
             enc ? "" : ": ", enc ? "" : e.c_str());
      });
      return;
    }
    if (standby_ && standby_->w() == sb.fmt.w && standby_->h() == sb.fmt.h) {
      if (standby_->bitrate() != sb.bitrate) standby_->set_bitrate(sb.bitrate);
      return;
    }
    if (standby_) {
      std::shared_ptr<Encoder> old(std::move(standby_));
      dispatch_async(standby_q_, ^{
        (void)old;
      });
    }
    const uint64_t gen = ++standby_gen_;
    MacVideoBackend* self = this;
    std::shared_ptr<std::atomic<bool>> alive = alive_;
    dispatch_async(standby_q_, ^{
      if (!*alive) return;
      std::string e;
      double ms = 0;
      const bool cold = Encoder::sessions_created() == 0;
      auto enc = Encoder::create(sb.fmt.w, sb.fmt.h, sb.bitrate, 30, true, &e, &ms);
      if (!enc) {
        vlog("warn", "standby encoder: %s", e.c_str());
        return;
      }
      std::lock_guard<std::mutex> lk(self->standby_mu_);
      if (gen != self->standby_gen_ || !self->standby_spec_.on || self->standby_) return;  // superseded
      self->standby_ = std::move(enc);
      vlog("info", "standby encoder ready %dx%d %d bit/s (%s, %.1f ms)", sb.fmt.w, sb.fmt.h, sb.bitrate,
           cold ? "cold" : "warm", ms);
    });
  }

  // The standby when its size matches (bitrate set on adoption); null otherwise. Waits for a standby being
  // created (it is ready sooner than a new session would be).
  std::unique_ptr<Encoder> adopt_standby(const VideoFormat& f, int bitrate) {
    {
      std::lock_guard<std::mutex> lk(standby_mu_);
      if (prewarm_ || !standby_spec_.on || standby_spec_.fmt.w != f.w || standby_spec_.fmt.h != f.h) {
        if (!standby_ || standby_->w() != f.w || standby_->h() != f.h) return nullptr;
      }
    }
    dispatch_sync(standby_q_, ^{});
    std::lock_guard<std::mutex> lk(standby_mu_);
    if (!standby_ || standby_->w() != f.w || standby_->h() != f.h) return nullptr;
    auto e = std::move(standby_);
    if (e->bitrate() != bitrate) e->set_bitrate(bitrate);
    return e;
  }

  void start_watching() {
    if (watching_.exchange(true)) return;
    std::shared_ptr<std::atomic<bool>> alive = alive_;
    MacVideoBackend* self = this;
    NSOperationQueue* oq = [[NSOperationQueue alloc] init];
    oq.underlyingQueue = q_;
    oq.maxConcurrentOperationCount = 1;
    for (NSNotificationName n in @[ AVCaptureDeviceWasConnectedNotification, AVCaptureDeviceWasDisconnectedNotification ]) {
      observers_.push_back([NSNotificationCenter.defaultCenter addObserverForName:n
                                                                            object:nil
                                                                             queue:oq
                                                                        usingBlock:^(NSNotification*) {
                                                                          if (*alive) self->inventory_changed();
                                                                        }]);
    }
    kvo_ = [[BMPreferredObserver alloc] initWithBlock:^{
      dispatch_async(self->q_, ^{
        if (*alive) self->inventory_changed();
      });
    }];
    [AVCaptureDevice addObserver:kvo_ forKeyPath:@"systemPreferredCamera" options:NSKeyValueObservingOptionNew context:nullptr];
    dispatch_async(q_, ^{
      self->last_preferred_ = preferred_camera_key();
    });
    // Permission: an undetermined state is re-read every 2 s (A§3.3 rule); a change notifies the engine.
    last_perm_ = camera_permission();
    perm_timer_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q_);
    dispatch_source_set_timer(perm_timer_, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), 2 * NSEC_PER_SEC,
                              200 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(perm_timer_, ^{
      if (*alive) self->permission_check(false);
    });
    dispatch_resume(perm_timer_);
  }

 public:
  void permission_check(bool force) {  // q_
    if (!force && last_perm_ != "undetermined") return;
    const std::string p = camera_permission();
    if (p == last_perm_ && !force) return;
    last_perm_ = p;
    VideoBackendListener* l = listener();
    if (l) l->permission_changed();
  }
  void notify_permission() {  // any thread: a fresh permission event (permission guard, §4.11)
    std::shared_ptr<std::atomic<bool>> alive = alive_;
    MacVideoBackend* self = this;
    dispatch_async(q_, ^{
      if (!*alive) return;
      self->last_perm_ = camera_permission();
      if (VideoBackendListener* l = self->listener()) l->permission_changed();
    });
  }

 private:
  VideoBackendListener* listener() {
    std::lock_guard<std::mutex> lk(mu_);
    return listener_;
  }

  void inventory_changed() {  // q_: debounce 300 ms
    const uint64_t gen = ++inv_gen_;
    std::shared_ptr<std::atomic<bool>> alive = alive_;
    MacVideoBackend* self = this;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_MSEC), q_, ^{
      if (!*alive || gen != self->inv_gen_) return;
      self->inventory_settled();
    });
  }

  void inventory_settled() {  // q_
    const std::string pref = preferred_camera_key();
    if (pref != last_preferred_) {
      vlog("info", "preferred camera: %s -> %s", last_preferred_.c_str(), pref.c_str());
      last_preferred_ = pref;
      follow_preferred();
    }
    if (VideoBackendListener* l = listener()) l->cameras_changed();
  }

  void follow_preferred() {  // q_: switch every following stream in place (§4.11)
    std::vector<MacCameraHandle*> hs;
    {
      std::lock_guard<std::mutex> lk(mu_);
      for (MacCameraHandle* h : handles_)
        if (h->follows_preferred()) hs.push_back(h);
    }
    for (MacCameraHandle* h : hs) {
      void* dev = find_camera("");
      if (!dev) continue;  // none left: the loss path reports it
      std::lock_guard<std::mutex> lk(mu_);
      if (!handles_.count(h)) continue;
      auto cur = h->source();
      if (cur && cur->key() == std::string(((__bridge AVCaptureDevice*)dev).uniqueID.UTF8String ?: "") && !cur->lost())
        continue;
      std::string err;
      auto next = source_for(dev, h->fmt_w(), h->fmt_h(), &err);
      if (!next) {
        vlog("warn", "switch to the preferred camera failed: %s", err.c_str());
        continue;
      }
      h->switch_source(next);
    }
  }

  void resolve_lost(MacCameraHandle* h) {  // q_
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (!handles_.count(h)) return;
      if (h->follows_preferred()) {
        if (void* dev = find_camera("")) {
          auto cur = h->source();
          AVCaptureDevice* d = (__bridge AVCaptureDevice*)dev;
          if (!cur || cur->key() != std::string(d.uniqueID.UTF8String ?: "")) {
            std::string err;
            if (auto next = source_for(dev, h->fmt_w(), h->fmt_h(), &err)) {
              h->switch_source(next);
              return;
            }
          }
        }
      }
    }
    h->emit_lost();
    VideoBackendListener* l = listener();
    if (l) l->camera_lost(h, "gone");  // the engine closes the handle (maybe from inside this call)
  }

  bool dev_ = false, prewarm_ = false;
  dispatch_queue_t q_, standby_q_;
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
  std::mutex mu_;  // listener_, handles_
  VideoBackendListener* listener_ = nullptr;
  std::set<MacCameraHandle*> handles_;
  std::mutex sources_mu_;
  std::map<std::string, std::weak_ptr<Source>> sources_;
  std::mutex standby_mu_;
  EncoderStandby standby_spec_;
  std::unique_ptr<Encoder> standby_;
  uint64_t standby_gen_ = 0;
  VideoFormat prewarmed_;
  std::atomic<bool> watching_{false};
  std::vector<id> observers_;
  id kvo_ = nil;
  dispatch_source_t perm_timer_ = nil;
  std::string last_perm_, last_preferred_;  // q_
  uint64_t inv_gen_ = 0;                    // q_
  friend class MacCameraHandle;
};


// ---------------------------------------------------------------------------------------------- handle

MacCameraHandle::~MacCameraHandle() {
  std::shared_ptr<Source> src;
  {
    std::lock_guard<std::mutex> lk(src_mu_);
    src = src_;
  }
  if (src) src->remove(this);  // no on_captured / on_source_event after this
  if (timer_) {
    dispatch_source_cancel(timer_);
    dispatch_sync(timer_q_, ^{});
  }
  {
    std::lock_guard<std::recursive_mutex> lk(sink_mu_);  // waits for a sink call in progress
    closed_ = true;  // no sink call from here on
  }
  std::unique_ptr<Encoder> e;
  {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    e = std::move(enc_);
  }
  const int64_t t0 = now_ns();
  e.reset();  // VTCompressionSessionInvalidate
  note_stop_ms((now_ns() - t0) / 1e6);
  {
    std::lock_guard<std::mutex> lk(src_mu_);
    src_.reset();  // the last stream on a camera stops its capture (async, camera_mac.mm)
  }
  src.reset();
  be_->handle_closed(this);
}

void MacCameraHandle::set_format(const VideoFormat& f) {
  if (f.w < 16 || f.h < 16 || (f.w & 1) || (f.h & 1)) return;
  int bitrate;
  {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (f.w == fmt_.w && f.h == fmt_.h) {
      want_idr_ = true;
      return;
    }
    bitrate = spec_.bitrate;
  }
  std::string err;
  double ms = 0;
  auto next = Encoder::create(f.w, f.h, bitrate, fps_, spec_.ltr, &err, &ms);
  if (!next) {
    vlog("warn", "set_format %dx%d: %s", f.w, f.h, err.c_str());
    return;
  }
  wire_encoder(next.get());
  std::unique_ptr<Encoder> old;
  {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    old = std::move(enc_);
    if (old) old->flush();  // its last AUs reach the sink before the new session's IDR
    {
      std::lock_guard<std::mutex> lk2(out_mu_);
      tokens_.clear();  // the new session has no LTR; acks of old frames must never reach it
      acks_pending_.clear();
      acked_since_idr_ = false;
    }
    enc_ = std::move(next);
    fmt_.w = f.w;
    fmt_.h = f.h;
    want_idr_ = true;
    want_refresh_ = false;
  }
  old.reset();
  vlog("info", "stream format %dx%d (new session %.1f ms)", f.w, f.h, ms);
}

void MacCameraHandle::on_captured(const Frame& f) {
  captured_++;
  if (!first_frame_.exchange(true)) {
    emit("running", "");
    emit("camera_ms", fmt1((now_ns() - open_ns_) / 1e6));
  }
  std::lock_guard<std::recursive_mutex> lk(mu_);
  if (!started_ || paused_ || !enc_) return;  // not submitted: nothing is encoded, no AU is lost
  if (fps_ == 15 && (parity_++ & 1)) return;
  double scale_ms = 0;
  CVPixelBufferRef pb = f.scaled ? f.scaled(fmt_.w, fmt_.h, &scale_ms) : f.native;
  if (!pb) return;
  if (CVPixelBufferGetWidth(pb) != (size_t)fmt_.w || CVPixelBufferGetHeight(pb) != (size_t)fmt_.h) return;
  const bool idr = want_idr_;
  const bool refresh = want_refresh_ && !idr;
  std::vector<int64_t> acks;
  {
    std::lock_guard<std::mutex> lk2(out_mu_);
    acks.swap(acks_pending_);
  }
  std::string err;
  if (enc_->encode(pb, f.capture_ns, idr, refresh, acks, &err)) {
    want_idr_ = false;
    if (refresh) want_refresh_ = false;
  } else {
    vlog("warn", "%s", err.c_str());
    std::lock_guard<std::mutex> lk2(out_mu_);
    acks_pending_.insert(acks_pending_.begin(), acks.begin(), acks.end());
  }
}

void MacCameraHandle::on_source_event(const std::string& what, const std::string& detail) {
  if (what == "lost") {
    be_->handle_lost(this);  // switches a following stream, else reports camera_lost
    return;
  }
  emit(what, detail);
}

void MacCameraHandle::switch_source(std::shared_ptr<Source> next) {
  std::shared_ptr<Source> old;
  {
    std::lock_guard<std::mutex> lk(src_mu_);
    old = src_;
    src_ = next;
  }
  if (old) old->remove(this);
  {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    want_idr_ = true;  // new camera: new format choice, IDR (§4.11)
  }
  emit("native", next->native());
  next->add(this);
  vlog("info", "stream switched to camera %s", next->key().c_str());
}

// ---------------------------------------------------------------------------------------------- backend

std::unique_ptr<CameraHandle> MacVideoBackend::open_camera(const CameraSpec& spec, EncoderSink* sink, std::string* err) {
  auto fail = [&](const std::string& m) -> std::unique_ptr<CameraHandle> {
    if (err) *err = m;
    return nullptr;
  };
  const VideoFormat f = spec.fmt;
  if (!sink) return fail("internal: no encoder sink");
  if (f.w < 16 || f.h < 16 || f.w > 4096 || f.h > 4096 || (f.w & 1) || (f.h & 1))
    return fail("internal: bad output size " + std::to_string(f.w) + "x" + std::to_string(f.h));
  const int bitrate = spec.bitrate > 0 ? spec.bitrate : 2500000;
  std::shared_ptr<Source> src;
  if (!spec.test.empty()) {
    // The synthetic camera: no AVCaptureDevice, no permission read (§4.11, R2 #4).
    if (!dev_) return fail("unsupported: the synthetic camera needs --dev");
    SynthSpec ss;
    std::string e;
    if (!parse_synth(spec.test, &ss, &e)) return fail(e);
    src = make_synth_source(ss, f.w, f.h);
  } else {
    // Permission guard (R2 #13): re-read here, never cached; never prompts.
    const std::string p = camera_permission();
    if (p != "granted") {
      notify_permission();
      return fail("permission: camera access is " + p);
    }
    void* dev = find_camera(spec.device);
    if (!dev) return fail(spec.device.empty() ? "no_camera: no camera on this Mac" : "no_camera: camera not found");
    std::string e;
    src = source_for(dev, f.w, f.h, &e);
    if (!src) return fail(e.empty() ? "internal: camera open failed" : e);
  }
  const int64_t t0 = now_ns();
  std::string detail;
  std::unique_ptr<Encoder> enc;
  if (spec.ltr) enc = adopt_standby(f, bitrate);
  if (enc) {
    detail = "standby:" + fmt1((now_ns() - t0) / 1e6);
  } else {
    const bool cold = Encoder::sessions_created() == 0;
    std::string e;
    double ms = 0;
    enc = Encoder::create(f.w, f.h, bitrate, f.fps == 15 ? 15 : 30, spec.ltr, &e, &ms);
    if (!enc) return fail(e);
    detail = std::string(cold ? "cold:" : "warm:") + fmt1(ms);
  }
  CameraSpec s = spec;
  s.bitrate = bitrate;
  auto h = std::make_unique<MacCameraHandle>(this, s, sink, std::move(enc), src);
  {
    std::lock_guard<std::mutex> lk(mu_);
    handles_.insert(h.get());
  }
  h->begin(detail);
  vlog("info", "camera stream opened: %s, %dx%d, encoder %s", src->native().c_str(), f.w, f.h, detail.c_str());
  return h;
}

}  // namespace

std::atomic<double> g_last_stop_ms{0};
void note_stop_ms(double ms) { g_last_stop_ms = ms; }
double last_stop_ms() { return g_last_stop_ms.load(); }

}  // namespace macvideo

std::unique_ptr<VideoBackend> make_mac_video_backend(const std::string& options) {
  return std::make_unique<macvideo::MacVideoBackend>(options);
}

}  // namespace beamer::audio
