// beamer-camera (Phase-1 prototype): shared helpers.
#pragma once
#import <Foundation/Foundation.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreMedia/CoreMedia.h>
#include <mach/mach_time.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <algorithm>

// ---- time (host clock = mach_absolute_time, the clock AVFoundation stamps frames with) ----
uint64_t now_ns();
uint64_t host_to_ns(uint64_t host);
uint64_t ns_to_host(uint64_t ns);
double cmtime_to_host_ns(CMTime t);  // CMTime in the host-time clock -> ns

// ---- events: JSON lines on stderr ----
void emit(NSDictionary *d);  // adds nothing; one line, flushed
void infof(const char *fmt, ...) __attribute__((format(printf, 1, 2)));  // "info: ..." on stderr

// ---- stats ----
double pct(std::vector<double> v, double p);  // copy, sort, nearest-rank
double mean(const std::vector<double> &v);

// ---- process ----
double cpu_seconds();   // user + sys of this process
double rss_mb();

// ---- options ----
struct Opts {
  std::string source = "synthetic";  // synthetic | camera
  std::string device;                // camera uniqueID or name substring
  int width = 1280, height = 720;
  double fps = 30;
  double seconds = 10;
  int64_t bitrate = 2500000;
  double limit_factor = 0;   // >0: DataRateLimits = bitrate*factor over 1 s
  std::string out;           // Annex-B file, "-" = stdout
  std::string trace;         // per-frame JSON lines
  std::string profile = "chigh";  // chigh | high | main | cbaseline | baseline
  bool lowlatency = true;
  bool realtime = true;
  bool speed = false;        // PrioritizeEncodingSpeedOverQuality
  std::string preset;        // "" | vc | hs | bal | hq  (macOS 26 presets)
  int keyint = 0;            // MaxKeyFrameInterval in frames, 0 = leave (infinite in low-latency mode)
  int force_key_every = 0;   // force an IDR every N frames
  std::vector<int> force_key_at;
  bool ltr = false;          // EnableLTR, ack every token at once
  std::vector<int> ltr_refresh_at;
  int ltr_ack_delay = 3;     // frames between an LTR frame and its (simulated) receiver ack
  int max_qp = 0;
  std::vector<std::pair<int, int64_t>> bitrate_at;  // --bitrate-at frame:bps,... (live AverageBitRate changes)
  bool verify = false;       // decode back + PSNR (synthetic)
  std::vector<int> verify_drop;  // frames withheld from the verify decoder (simulated loss)
  int verify_from = 0;       // first frame fed to the verify decoder
  bool encode = true;        // false: capture only
  bool fast = false;         // synthetic: not paced (throughput)
  bool pin_fps = false;      // camera: min = max frame duration
  int format_index = -1;     // camera: explicit activeFormat index
  std::string pixfmt = "420v";
  int noise = 3;             // synthetic sensor-noise amplitude (luma steps)
  double auth_timeout = 3;
  double stats_every = 1;
  bool hw_required = false;
  bool effects_probe = false;
  std::string tag;
  int cycles = 1;            // camera: start/stop the session N times in one process (warm restarts)
  double gap = 1;            // seconds between cycles
  std::string encoder_id;    // force an encoder (VTCopyVideoEncoderList id)
  std::vector<std::pair<std::string, std::string>> props;  // --prop Key=Value (raw VT property names)
};

// ---- synthetic source (synth.mm) ----
// NV12 video range. Embeds an 80-block code strip on rows [0, block): 8-bit sync 0xA5, 32-bit frame
// counter, 32-bit (host ns / 100000) truncated, 8-bit xor checksum of the 8 bytes. Block = width/80.
void synth_fill(CVPixelBufferRef pb, uint32_t frame, int noise);
void synth_stamp(CVPixelBufferRef pb, uint32_t frame, uint64_t host_ns);
bool synth_read(CVPixelBufferRef pb, uint32_t *frame, uint64_t *ts100us);

// ---- encoder (encode.mm) ----
struct FrameRec {
  uint32_t f = 0;
  uint64_t cap_ns = 0;   // capture (camera PTS on host clock) / generation time
  uint64_t arr_ns = 0;   // delivered to us (delegate / after generation)
  uint64_t in_ns = 0;    // handed to VTCompressionSessionEncodeFrame
  uint64_t out_ns = 0;   // output callback
  int64_t size = 0;      // Annex-B bytes, SPS/PPS included on keyframes
  bool key = false, dropped = false, forced = false, ltr_refresh = false;
  int64_t ltr_token = -1;
  double psnr_y = -1, psnr_uv = -1, dec_ms = -1;
};

class Encoder {
 public:
  explicit Encoder(const Opts &o);
  ~Encoder();
  bool open(NSMutableDictionary *startInfo);  // fills properties/status into startInfo
  void encode(CVPixelBufferRef pb, const FrameRec &meta, bool forceKey, bool ltrRefresh);
  void finish();
  NSDictionary *summary(double wall_s);
  void tick_stats(double t_s);
  int frames_out() const;
 public:  // Impl is used by the C callbacks in encode.mm
  struct Impl;
  Impl *d;
};

// ---- capture (capture.mm) ----
int cmd_list(const Opts &o, bool json);
int cmd_auth(const Opts &o, bool request);
int cmd_run_camera(const Opts &o);
int cmd_run_synthetic(const Opts &o);
int cmd_encoders();
