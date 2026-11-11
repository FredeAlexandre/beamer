// SPDX-License-Identifier: GPL-3.0-only
// The macOS video backend (camera PHASE2 §2.4, §4.11): AVFoundation capture on demand, centre crop +
// scale to the cluster size, VideoToolbox low-latency H.264 with LTR, a standby encoder session, and the
// --dev synthetic source. The first part is the public factory; the rest is shared by the video_*.mm /
// camera_mac.mm / scale_mac.mm / videoenc_mac.mm / synth_mac.mm files and the video_check harness only.
#pragma once

#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../engine/video_backend.h"

namespace beamer::audio {

// `options`: ';'-separated words.
//   dev                 the helper runs with --dev: open_camera accepts CameraSpec.test (the engine announces
//                       camera_synth and refuses `test` without --dev; this is the backend's own second check)
//   standby=session     keep one idle VTCompressionSession while warm (§4.11, default)
//   standby=prewarm     create + invalidate one session when warm sets (no idle session; §4.11 fallback)
// Environment (tests/tuning only): BEAMER_VIDEO_STANDBY=session|prewarm overrides the option.
std::unique_ptr<VideoBackend> make_mac_video_backend(const std::string& options);

namespace macvideo {

// ---- clock: the engine's now_ns() is steady_clock (engine/util.cpp); on macOS it is mach host time in
// ns (checked by video_check selftest). Defined here so the backend links without the engine.
inline int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
int64_t host_time_to_ns(CMTime host_time);  // a CMTime on the host time clock -> now_ns() clock

void vlog(const char* level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// ---- test hooks (video_check): capture-device objects created by this process (AVCaptureDeviceInput,
// AVCaptureSession). A synthetic stream must leave both at 0 (§4.11, R2 #4).
int capture_device_creations();
int capture_sessions_running();
std::string camera_permission();
void note_stop_ms(double ms);        // a stream's encoder invalidation time (handle destructor)
double last_stop_ms();
std::vector<double> take_scale_times();   // ms per VTPixelTransferSession call since the last take
std::vector<double> take_encode_times();  // ms submit -> output per AU since the last take  // granted | denied | undetermined | restricted | unknown (re-read each call)

// ---- geometry (scale_mac.mm), pure ----
struct CropRect { int x = 0, y = 0, w = 0, h = 0; };
// The largest centred 16:9 rectangle of a W x H picture, sizes and origin rounded down to even (§4.11).
CropRect crop_16x9(int w, int h);
struct FormatCand {
  int w = 0, h = 0;
  double max_fps = 0;
  uint32_t subtype = 0;  // '420v' / '420f' / other
};
// §4.11 capture format choice for a Wt x Ht output: returns the index into `cands`, -1 when none fits
// (no 420v/420f format at all).
int choose_format(const std::vector<FormatCand>& cands, int wt, int ht);

// One VTPixelTransferSession: NV12 video range in and out, the source cropped to `crop` (clean aperture),
// scaled to dw x dh. Not thread-safe (one per camera source, used on its queue).
class Scaler {
 public:
  Scaler(int dw, int dh);
  ~Scaler();
  Scaler(const Scaler&) = delete;
  Scaler& operator=(const Scaler&) = delete;
  // Returns a +1 retained buffer of dw x dh, or nullptr (err set).
  CVPixelBufferRef convert(CVPixelBufferRef src, const CropRect& crop, std::string* err);
  double last_ms() const { return last_ms_; }

 private:
  bool ensure(std::string* err);
  int dw_, dh_;
  void* session_ = nullptr;  // VTPixelTransferSessionRef
  CVPixelBufferPoolRef pool_ = nullptr;
  double last_ms_ = 0;
};
CVPixelBufferPoolRef make_nv12_pool(int w, int h);

// ---- synthetic source (synth_mac.mm) ----
struct SynthSpec {
  int w = 0, h = 0;      // native size; 0 = the CameraSpec output size
  int start_ms = 5;      // capture start -> first frame
  int noise = 3;
};
// "synth", "synth:1920x1440", "synth:1280x720;start_ms=800", "synth;start_ms=0"
bool parse_synth(const std::string& test, SynthSpec* out, std::string* err);
// C1's test picture (moving disc, gradient, text panel, sensor noise; native pixels) inside the 16:9 crop `crop`, black
// outside it, a 2-output-pixel white frame on the crop border and a centre cross (geometry markers), and
// the code strip on the crop's first rows: 80 blocks across the crop width, sync 0xA5, frame u32,
// (capture ns / 100000) u32, xor checksum. After the crop + scale the strip is C1's (block = width / 80).
void synth_draw(CVPixelBufferRef pb, uint32_t frame, uint64_t capture_ns, const CropRect& crop, int noise, int out_w);
extern std::atomic<uint64_t> g_synth_cpu_ns;  // CPU spent drawing synthetic pictures (reported apart)
// Reads the strip at the top of an output-size picture (C1 reader).
bool synth_read(CVPixelBufferRef pb, uint32_t* frame, uint32_t* ts100us);

// ---- frame sources (camera_mac.mm, synth_mac.mm) ----
// One captured picture, handed to every subscriber on the source's delivery thread. `scaled` returns the
// picture at the output size (shared per size between subscribers of this frame; +0, valid during the
// callback), the native buffer itself when the size already matches.
struct Frame {
  CVPixelBufferRef native = nullptr;
  uint64_t capture_ns = 0;
  uint32_t index = 0;  // captured-frame counter of the source
  std::function<CVPixelBufferRef(int w, int h, double* scale_ms)> scaled;
};
struct FrameListener {
  virtual void on_captured(const Frame& f) = 0;
  // "native" (WxH@fps), "running", "camera_ms", "fps", "interrupted", "resumed", "lost", "by_other", "effects"
  virtual void on_source_event(const std::string& what, const std::string& detail) = 0;
  virtual ~FrameListener() = default;
};
class Source {
 public:
  virtual ~Source() = default;
  // Thread-safe. remove() returns once no callback to that listener runs or will run.
  virtual void add(FrameListener* l) = 0;
  virtual void remove(FrameListener* l) = 0;
  virtual std::string key() const = 0;       // device uniqueID, or "synth"
  virtual std::string native() const = 0;    // "WxH@fps"
  virtual bool lost() const = 0;
};
std::shared_ptr<Source> make_synth_source(const SynthSpec& spec, int out_w, int out_h);
// Real camera: opens the device (format by §4.11 for wt x ht), starts the session at once (async).
// `device` is an AVCaptureDevice* (__bridge). Fails "no_camera: ..." / "internal: ...".
std::shared_ptr<Source> make_camera_source(void* device, int wt, int ht, std::string* err);

// ---- camera inventory (camera_mac.mm) ----
CameraSnapshot camera_snapshot();
void* find_camera(const std::string& key);  // "" = system preferred (else default, else first); +0 AVCaptureDevice*
std::string preferred_camera_key();

// ---- encoder (videoenc_mac.mm) ----
struct EncOut {
  const uint8_t* data = nullptr;
  size_t len = 0;
  uint64_t capture_ns = 0;
  bool idr = false, refresh = false;
  int64_t token = -1;  // RequireLTRAcknowledgementToken, -1 = not an LTR
  int w = 0, h = 0;
  uint64_t submit_ns = 0, out_ns = 0;
};
class Encoder {
 public:
  // Creates and configures one low-latency session (hardware required), ready to encode. `ms`: time to ready.
  static std::unique_ptr<Encoder> create(int w, int h, int bitrate, int fps, bool ltr, std::string* err, double* ms);
  static int sessions_created();  // in this process (0 before the first: the next one is "cold")
  ~Encoder();                     // invalidates the session; no output after it returns
  Encoder(const Encoder&) = delete;
  Encoder& operator=(const Encoder&) = delete;
  void set_output(std::function<void(const EncOut&)> out);  // output thread (VideoToolbox's)
  // discard: a wake-up frame (see MacCameraHandle::wake): encoded, its output dropped inside the encoder
  bool encode(CVPixelBufferRef pb, uint64_t capture_ns, bool force_idr, bool refresh,
              const std::vector<int64_t>& acked_tokens, std::string* err, bool discard = false);
  void flush();  // emits every pending frame
  bool set_bitrate(int bps);
  void set_fps(int fps);
  int w() const { return w_; }
  int h() const { return h_; }
  int bitrate() const { return bitrate_; }

  struct Impl;

 private:
  Encoder() = default;
  void* session_ = nullptr;  // VTCompressionSessionRef
  void* refcon_ = nullptr;   // std::weak_ptr<Impl>*, freed after the session
  int w_ = 0, h_ = 0, bitrate_ = 0, fps_ = 30;
  std::shared_ptr<Impl> impl_;
};

}  // namespace macvideo
}  // namespace beamer::audio
