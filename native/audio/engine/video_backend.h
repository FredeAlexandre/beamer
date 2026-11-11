// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>
namespace beamer::audio {
struct VideoFormat { int w = 0, h = 0, fps = 30; };
struct CameraInfo {
  std::string key, name, transport;          // transport: "builtin","usb","continuity","virtual","other"
  std::vector<VideoFormat> native;           // every native format at its max fps (<= 16 listed; diagnostic only)
  bool preferred = false;                    // macOS systemPreferredCamera
};
struct CameraSnapshot { std::vector<CameraInfo> cameras; std::string preferred;  // key, "" = none
                        std::string permission; };  // granted|denied|undetermined|restricted|unknown
struct EncodedFrame {                         // one Annex-B AU; valid only during on_frame
  const uint8_t* data = nullptr; size_t len = 0;
  uint64_t capture_ns = 0;                    // capture time on now_ns()'s clock (engine util.h)
  bool idr = false, refresh = false, ltr = false;  // refresh: references only acknowledged LTRs
  int w = 0, h = 0;                           // always the CameraSpec / set_format size
};
struct EncoderSink {                          // called on the backend's encoder output thread
  // Copy and return; never block > 1 ms. Returns the frame number the engine gave this AU
  // (0, 1, 2... per CameraHandle, one per call, never reused). The backend keeps it to map LTR tokens.
  virtual uint32_t on_frame(const EncodedFrame&) = 0;
  virtual void on_camera_event(const std::string& what, const std::string& detail) = 0;
      // what: "running","interrupted","resumed","lost","native" (capture format "WxH@fps"),"fps","effects","by_other",
      //       "encoder" (detail "standby:<ms>"|"warm:<ms>"|"cold:<ms>": session adopted/created, time to ready),
      //       "camera_ms" (detail "<ms>": capture start -> first captured frame)
  virtual ~EncoderSink() = default;
};
struct CameraSpec { std::string device;       // "" = system-preferred (followed)
                    VideoFormat fmt;          // output size: 1280x720 or 1920x1080, fps 30 (§4.11)
                    int bitrate = 2500000; bool ltr = true;
                    std::string test;  };     // --dev only: "synth[:WxH][;start_ms=N]" = synthetic camera of that
                                              // native size whose first frame comes N ms after capture start (default
                                              // 5); never touches AVCaptureDevice, needs no camera permission (§6.4)
struct EncoderStandby { bool on = false; VideoFormat fmt; int bitrate = 2500000; };   // §4.11 warm start
class CameraHandle {                          // one capture tap + one encoder session; all calls thread-safe
 public:
  virtual ~CameraHandle() = default;
  virtual void start_encoding() = 0;          // no AU exists before this call; the first AU after it is an IDR
                                              // (it may come late: the camera's first frame, 0.2-0.8 s; §4.3 class 1)
  virtual void request_idr() = 0;
  virtual void request_ltr_refresh() = 0;     // an IDR inside when no LTR is acknowledged
  virtual void acknowledge_ltr(uint32_t frame) = 0;  // that LTR frame is verified (§4.3); idempotent
  virtual void set_bitrate(int bps) = 0;
  virtual void set_fps(int fps) = 0;          // 30 or 15 (15 = every other captured frame)
  virtual void set_format(const VideoFormat&) = 0;   // new output size; the next AU is an IDR at that size
  virtual void set_input_paused(bool) = 0;    // captured frames are not given to the encoder while paused;
                                              // the reference chain is untouched (no AU is lost)
  virtual std::string device_in_use() const = 0;
};
struct VideoSinkListener {                    // called on the backend's thread
  virtual void on_decoded(uint32_t frame, bool ok, double decode_ms) = 0;
  virtual void on_sink_event(const std::string& what, const std::string& detail) = 0;
      // "decoder_sw","decoder_hw","device_lost","size_rejected" (detail "WxH": the DISPLAY size, i.e. after the
      // SPS frame cropping / MF_MT_MINIMUM_DISPLAY_APERTURE, is odd or outside kMin..kMax; the coded size,
      // e.g. 1920x1088 for 1080p, is never checked, §4.6.1)
  virtual ~VideoSinkListener() = default;
};
class VideoSinkHandle {                       // one decoder feeding the virtual camera; engine's video thread
 public:
  virtual ~VideoSinkHandle() = default;
  virtual void decode(const uint8_t* au, size_t len, uint32_t frame, bool keep) = 0;
      // keep = the picture may be shown (§4.5.2); !keep pictures are decoded and dropped
  virtual void present(uint32_t frame) = 0;   // make that kept picture the camera's current picture (live)
  virtual void hold() = 0;                    // keep showing the current picture (stall)
  virtual void drop(uint32_t frame) = 0;      // kept picture no longer needed (or revoked)
  virtual void show_placeholder(const std::string& state) = 0;
      // show the placeholder of that state ("starting","reconnecting","error") with the texts of vcam_placeholders();
      // the next present() goes back to live
};
struct Placeholder { std::string title, line; };
struct VcamState {
  std::string install;                        // ok|missing|outdated|broken|unsupported
  int dll_version = 0;                        // registered in HKLM, 0 = none
  int loaded_version = 0;                     // Header.dll_version of the running source, 0 = none seen
  int bundled_version = 0;                    // the backend's compiled kDllVersion (the portable engine cannot
                                              // include vcam-win/shm_protocol.h; the Windows backend fills it)
  bool present = false, in_use = false, creating = false;
  std::string app;                            // best effort, "" = unknown
  VideoFormat want;                           // the app's media type while in_use (validated, §4.6.3)
  std::string error;
};
struct VideoBackendListener {
  virtual void cameras_changed() = 0;         // debounced 300 ms
  virtual void vcam_changed() = 0;            // in_use on at once, off debounced 300 ms (Frame Server re-creates)
  virtual void permission_changed() = 0;
  virtual void camera_lost(CameraHandle*, const std::string& why) = 0;
  virtual ~VideoBackendListener() = default;
};
class VideoBackend {
 public:
  virtual ~VideoBackend() = default;
  virtual void set_listener(VideoBackendListener*) = 0;
  virtual std::vector<std::string> features() = 0;      // "camera_capture" (Mac), "virtual_camera" (Windows)
  virtual CameraSnapshot cameras() = 0;                 // empty on Windows
  virtual void request_permission() = 0;                // Mac: async prompt; listener on answer
  // Mac, real camera: fails "permission: ..." unless AVCaptureDevice authorizationStatus is authorized at this
  // call (re-read here, never from a cached state), then emits a fresh permission_changed; it never prompts.
  // A CameraSpec with `test` set skips the camera and the permission check entirely (§6.4 rule 2).
  virtual std::unique_ptr<CameraHandle> open_camera(const CameraSpec&, EncoderSink*, std::string* err) = 0;
  // Mac: keep (on) or release (off) one idle encoder session of that size, no camera, no frames; open_camera
  // adopts it when the size matches (§4.11). Windows and null: recorded only. Idempotent.
  virtual void set_encoder_standby(const EncoderStandby&) = 0;
  virtual VcamState vcam() = 0;                         // Windows; Mac returns install "unsupported"
  virtual bool vcam_enable(bool on, std::string* err) = 0;   // create+Start the System/CurrentUser camera
  // Texts per state ("off","waiting","waiting_permission","sharing_off","denied","error","starting",
  // "reconnecting") and the state shown while no sink is open.
  virtual void vcam_placeholders(const std::map<std::string, Placeholder>& texts, const std::string& idle) = 0;
  // Fails ("internal: sink open") while another sink exists: one virtual camera, one picture source.
  virtual std::unique_ptr<VideoSinkHandle> open_sink(VideoSinkListener*, std::string* err) = 0;
};
std::unique_ptr<VideoBackend> make_null_video_backend(const std::string& options);
class Backend;
int run_helper(Backend& backend, VideoBackend* video, int argc, char** argv);   // video may be null
}
