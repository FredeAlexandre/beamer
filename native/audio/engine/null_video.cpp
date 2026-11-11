// SPDX-License-Identifier: GPL-3.0-only
#include "null_video.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <set>
#include <thread>

#include "util.h"
#include "video_packet.h"

namespace beamer::audio {

namespace {

constexpr uint32_t kAuMagic = 0x4E564155;  // 'NVAU'
constexpr size_t kAuHead = 24;
enum AuType : uint8_t { kAuP = 0, kAuIdr = 1, kAuRefresh = 2 };

uint8_t pattern(uint32_t frame, size_t i) { return static_cast<uint8_t>(frame * 131u + uint32_t(i) * 7u); }

bool parse_wh(const std::string& s, int* w, int* h) {
  auto x = s.find('x');
  if (x == std::string::npos) return false;
  *w = std::atoi(s.substr(0, x).c_str());
  *h = std::atoi(s.substr(x + 1).c_str());
  return *w > 0 && *h > 0;
}

struct Options {
  int cams = 2;
  std::string perm = "granted", vcam = "ok";
  double ip = 120000, p = 10000, refresh = 14000;
  int enc_ms = 5, dec_ms = 2, cam_start_ms = 5, dec_err_ms = 0;
  int native_w = 1920, native_h = 1440;
};

Options parse_options(const std::string& text) {
  Options o;
  size_t i = 0;
  while (i <= text.size()) {
    size_t j = text.find(';', i);
    if (j == std::string::npos) j = text.size();
    std::string kv = text.substr(i, j - i);
    i = j + 1;
    auto eq = kv.find('=');
    if (eq == std::string::npos) continue;
    std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
    if (k == "cams") o.cams = std::max(0, std::min(16, std::atoi(v.c_str())));
    else if (k == "perm") o.perm = v;
    else if (k == "vcam") o.vcam = v;
    else if (k == "ip") o.ip = std::atof(v.c_str());
    else if (k == "p") o.p = std::atof(v.c_str());
    else if (k == "refresh") o.refresh = std::atof(v.c_str());
    else if (k == "enc_ms") o.enc_ms = std::atoi(v.c_str());
    else if (k == "dec_ms") o.dec_ms = std::atoi(v.c_str());
    else if (k == "cam_start_ms") o.cam_start_ms = std::atoi(v.c_str());
    else if (k == "dec_err_ms") o.dec_err_ms = std::atoi(v.c_str());
    else if (k == "native") parse_wh(v, &o.native_w, &o.native_h);
  }
  return o;
}

}  // namespace

class NullCamera;
class NullSink;

class NullVideoBackend : public VideoBackend {
 public:
  explicit NullVideoBackend(const std::string& options) : opt_(parse_options(options)) {
    vcam_.install = opt_.vcam;
    vcam_.bundled_version = 1;
    vcam_.dll_version = opt_.vcam == "ok" ? 1 : 0;
    vcam_.present = false;
  }
  ~NullVideoBackend() override = default;

  void set_listener(VideoBackendListener* l) override {
    std::lock_guard<std::mutex> lock(mu_);
    listener_ = l;
  }
  std::vector<std::string> features() override { return {"camera_capture", "virtual_camera"}; }
  CameraSnapshot cameras() override {
    std::lock_guard<std::mutex> lock(mu_);
    CameraSnapshot s;
    for (int i = 1; i <= opt_.cams; ++i) {
      CameraInfo c;
      c.key = "null-cam-" + std::to_string(i);
      c.name = "Null Camera " + std::to_string(i);
      c.transport = i == 1 ? "builtin" : "usb";
      c.native.push_back(VideoFormat{opt_.native_w, opt_.native_h, 30});
      c.preferred = i == 1;
      s.cameras.push_back(c);
    }
    s.preferred = opt_.cams > 0 ? "null-cam-1" : "";
    s.permission = opt_.perm;
    return s;
  }
  void request_permission() override {
    VideoBackendListener* l;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (opt_.perm == "undetermined") opt_.perm = "granted";
      l = listener_;
    }
    if (l) l->permission_changed();
  }
  std::unique_ptr<CameraHandle> open_camera(const CameraSpec& spec, EncoderSink* sink, std::string* err) override;
  void set_encoder_standby(const EncoderStandby& s) override {
    std::lock_guard<std::mutex> lock(mu_);
    standby_ = s;
    n_standby_++;
  }
  VcamState vcam() override {
    std::lock_guard<std::mutex> lock(mu_);
    return vcam_;
  }
  bool vcam_enable(bool on, std::string* err) override {
    VideoBackendListener* l;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (on && vcam_.install != "ok") {
        *err = "not_installed: " + vcam_.install;
        return false;
      }
      vcam_.present = on;
      vcam_.loaded_version = on ? 1 : 0;
      l = listener_;
    }
    if (l) l->vcam_changed();
    return true;
  }
  void vcam_placeholders(const std::map<std::string, Placeholder>& texts, const std::string& idle) override {
    std::lock_guard<std::mutex> lock(mu_);
    texts_ = texts;
    idle_ = idle;
    n_placeholder_cmds_++;
  }
  std::unique_ptr<VideoSinkHandle> open_sink(VideoSinkListener* l, std::string* err) override;

  // ---- shared with handles
  std::mutex mu_;
  Options opt_;
  VideoBackendListener* listener_ = nullptr;
  EncoderStandby standby_;
  VcamState vcam_;
  std::map<std::string, Placeholder> texts_;
  std::string idle_;
  bool sink_open_ = false;
  bool used_encoder_ = false;
  // counters
  uint64_t n_standby_ = 0, n_placeholder_cmds_ = 0, cameras_open_ = 0, cameras_opened_ = 0, sinks_opened_ = 0;
  int64_t last_camera_close_ns_ = 0, last_camera_open_ns_ = 0;
  uint64_t enc_idr_ = 0, enc_refresh_ = 0, enc_p_ = 0, enc_frames_ = 0, enc_number_mismatch_ = 0, enc_ack_ = 0,
           enc_ack_bad_ = 0, refresh_as_idr_ = 0, start_encoding_ = 0;
  int64_t first_au_after_start_ms_ = -1;
  uint64_t dec_frames_ = 0, dec_ok_ = 0, dec_err_ = 0, dec_corrupt_input_ = 0, incorrect_kept_ = 0, kept_ = 0,
           presented_ = 0, incorrect_presented_ = 0, backwards_ = 0, holds_ = 0, drops_ = 0, size_rejected_ = 0,
           present_unknown_ = 0, decoded_before_idr_ = 0;
  int last_w_ = 0, last_h_ = 0;
  std::set<uint32_t> suspects_;
  int64_t mark_ns_ = 0, kept_after_mark_ns_ = 0;  // tests: first correct kept picture after a mark          // kept, not correct, not yet revoked
  uint64_t kept_wrong_at_decode_ = 0;
  int64_t first_present_ns_ = 0;
  std::vector<std::string> placeholders_shown_;
  int pending_dec_errors_ = 0;
};

// ------------------------------------------------------------------ fake encoder

class NullCamera : public CameraHandle {
 public:
  NullCamera(NullVideoBackend* be, const CameraSpec& spec, EncoderSink* sink, int native_w, int native_h,
             int start_ms, const std::string& encoder)
      : be_(be), spec_(spec), sink_(sink), fmt_(spec.fmt), bitrate_(spec.bitrate), native_w_(native_w),
        native_h_(native_h), start_ms_(start_ms), encoder_(encoder), rng_(random_u32()) {
    thread_ = std::thread([this] { run(); });
  }
  ~NullCamera() override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
    std::lock_guard<std::mutex> lock(be_->mu_);
    be_->cameras_open_--;
    be_->last_camera_close_ns_ = now_ns();
  }
  void start_encoding() override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (started_) return;
      started_ = true;
      start_ns_ = now_ns();
      need_idr_ = true;
    }
    {
      std::lock_guard<std::mutex> lock(be_->mu_);
      be_->start_encoding_++;
    }
    cv_.notify_all();
  }
  void request_idr() override {
    std::lock_guard<std::mutex> lock(mu_);
    need_idr_ = true;
  }
  void request_ltr_refresh() override {
    std::lock_guard<std::mutex> lock(mu_);
    want_refresh_ = true;
  }
  void acknowledge_ltr(uint32_t frame) override {
    std::lock_guard<std::mutex> lock(mu_);
    const bool known = ltrs_.count(frame) > 0;
    if (known) acked_.insert(frame);
    std::lock_guard<std::mutex> l2(be_->mu_);
    if (known) be_->enc_ack_++;
    else be_->enc_ack_bad_++;
  }
  void set_bitrate(int bps) override {
    std::lock_guard<std::mutex> lock(mu_);
    bitrate_ = bps;
  }
  void set_fps(int fps) override {
    std::lock_guard<std::mutex> lock(mu_);
    fps_ = fps <= 15 ? 15 : 30;
  }
  void set_format(const VideoFormat& f) override {
    std::lock_guard<std::mutex> lock(mu_);
    fmt_.w = f.w;
    fmt_.h = f.h;
    need_idr_ = true;
  }
  void set_input_paused(bool p) override {
    std::lock_guard<std::mutex> lock(mu_);
    paused_ = p;
  }
  std::string device_in_use() const override {
    if (!spec_.test.empty()) return "test:" + spec_.test;
    return spec_.device.empty() ? "null-cam-1" : spec_.device;  // "" = the preferred camera
  }

 private:
  void run() {
    set_thread_name("null-video-enc");
    sink_->on_camera_event("running", "");
    sink_->on_camera_event("native", std::to_string(native_w_) + "x" + std::to_string(native_h_) + "@30");
    sink_->on_camera_event("encoder", encoder_);
    std::vector<uint8_t> au;
    int64_t next = 0;
    uint64_t tick = 0;
    for (;;) {
      std::unique_lock<std::mutex> lock(mu_);
      if (stop_) return;
      if (!started_) {
        cv_.wait(lock);
        continue;
      }
      if (next == 0) next = start_ns_ + int64_t(start_ms_) * 1'000'000;
      const int64_t now = now_ns();
      if (now < next) {
        cv_.wait_until(lock, std::chrono::steady_clock::time_point(
                                 std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::nanoseconds(next))));
        continue;
      }
      const int64_t capture = next;
      next += 1'000'000'000LL / 30;
      if (now - next > 200'000'000) next = now;  // the process was stalled: do not burst
      const uint64_t t = tick++;
      if (paused_) continue;                       // the frame is not given to the encoder
      if (fps_ == 15 && (t & 1)) continue;         // 15 = every other captured frame
      // ---- encode
      const uint32_t number = number_;
      AuType type = kAuP;
      uint32_t ref = number - 1;
      int refresh_as_idr = 0;
      if (number == 0 || need_idr_) {
        type = kAuIdr;
      } else if (want_refresh_) {
        uint32_t newest = kNoFrame;
        for (uint32_t f : acked_)
          if (newest == kNoFrame || f > newest) newest = f;
        if (newest == kNoFrame) {
          type = kAuIdr;  // an IDR inside when no LTR is acknowledged
          refresh_as_idr++;
        } else {
          type = kAuRefresh;
          ref = newest;
        }
      }
      if (type == kAuIdr) ref = kNoFrame;
      if (refresh_as_idr) {
        std::lock_guard<std::mutex> l2(be_->mu_);
        be_->refresh_as_idr_ += refresh_as_idr;
        refresh_as_idr = 0;
      }
      need_idr_ = false;
      want_refresh_ = false;
      const bool ltr = type == kAuIdr || number % 30 == 0;
      if (type == kAuIdr) {
        // the IDR clears every reference: older LTRs are forgotten on both sides
        for (auto it = ltrs_.begin(); it != ltrs_.end();) it = *it < number ? ltrs_.erase(it) : std::next(it);
        acked_.clear();
      }
      if (ltr) {
        ltrs_.insert(number);
        while (ltrs_.size() > 64) ltrs_.erase(ltrs_.begin());
      }
      const double scale = double(bitrate_) / 2.5e6;
      double bytes;
      Options o;
      {
        std::lock_guard<std::mutex> l2(be_->mu_);
        o = be_->opt_;
      }
      if (type == kAuIdr) bytes = o.ip * scale;
      else if (type == kAuRefresh) bytes = o.refresh * scale;
      else {
        const double cv = 0.34, sigma = std::sqrt(std::log(1 + cv * cv)), mu = std::log(o.p * scale) - sigma * sigma / 2;
        std::normal_distribution<double> g(mu, sigma);
        bytes = std::exp(g(rng_));
      }
      const size_t n = std::max(kAuHead + 1, std::min(kMaxAuBytes - 64, static_cast<size_t>(bytes)));
      au.resize(n);
      put_u32(au.data(), kAuMagic);
      put_u32(au.data() + 4, number);
      au[8] = type;
      au[9] = ltr ? 1 : 0;
      const int w = fmt_.w, h = fmt_.h;
      const int coded_h = (h + 15) / 16 * 16;
      put_u16(au.data() + 10, static_cast<uint16_t>(w));
      put_u16(au.data() + 12, static_cast<uint16_t>(h));
      put_u16(au.data() + 14, static_cast<uint16_t>(coded_h));
      put_u32(au.data() + 16, ref);
      put_u32(au.data() + 20, static_cast<uint32_t>(n));
      for (size_t i = kAuHead; i < n; ++i) au[i] = pattern(number, i);
      number_++;
      const int enc_ms = o.enc_ms;
      const bool first = !first_done_;
      first_done_ = true;
      lock.unlock();
      // encoder latency: the AU reaches the engine enc_ms after its capture
      sleep_ns(capture + int64_t(enc_ms) * 1'000'000 - now_ns());
      if (first) {
        const int64_t ms = (now_ns() - start_ns_) / 1'000'000;
        sink_->on_camera_event("camera_ms", std::to_string(std::max<int64_t>(0, (capture - start_ns_) / 1'000'000)));
        std::lock_guard<std::mutex> l2(be_->mu_);
        be_->first_au_after_start_ms_ = ms;
      }
      EncodedFrame ef;
      ef.data = au.data();
      ef.len = au.size();
      ef.capture_ns = static_cast<uint64_t>(capture);
      ef.idr = type == kAuIdr;
      ef.refresh = type == kAuRefresh;
      ef.ltr = ltr;
      ef.w = w;
      ef.h = h;
      const uint32_t got = sink_->on_frame(ef);
      std::lock_guard<std::mutex> l2(be_->mu_);
      be_->enc_frames_++;
      if (got != number) be_->enc_number_mismatch_++;
      if (type == kAuIdr) be_->enc_idr_++;
      else if (type == kAuRefresh) be_->enc_refresh_++;
      else be_->enc_p_++;
    }
  }

  NullVideoBackend* be_;
  CameraSpec spec_;
  EncoderSink* sink_;
  std::mutex mu_;
  std::condition_variable cv_;
  VideoFormat fmt_;
  int bitrate_, fps_ = 30;
  int native_w_, native_h_, start_ms_;
  std::string encoder_;
  bool started_ = false, stop_ = false, paused_ = false, need_idr_ = false, want_refresh_ = false, first_done_ = false;
  int64_t start_ns_ = 0;
  uint32_t number_ = 0;
  std::set<uint32_t> ltrs_, acked_;
  std::mt19937 rng_;
  std::thread thread_;
};

std::unique_ptr<CameraHandle> NullVideoBackend::open_camera(const CameraSpec& spec, EncoderSink* sink, std::string* err) {
  int nw, nh, start_ms;
  std::string encoder;
  {
    std::lock_guard<std::mutex> lock(mu_);
    nw = opt_.native_w;
    nh = opt_.native_h;
    start_ms = opt_.cam_start_ms;
    if (spec.test.empty()) {
      if (opt_.cams == 0) {
        *err = "no_camera: no camera";
        return nullptr;
      }
      bool found = spec.device.empty();
      for (int i = 1; i <= opt_.cams && !found; ++i) found = spec.device == "null-cam-" + std::to_string(i);
      if (!found) {
        *err = "no_camera: " + spec.device;
        return nullptr;
      }
      if (opt_.perm != "granted") {
        *err = "permission: " + opt_.perm;
        return nullptr;
      }
    } else {
      // "synth[:WxH][;start_ms=N]": no camera, no permission check
      std::string t = spec.test;
      if (t.rfind("synth", 0) != 0) {
        *err = "bad_args: test " + t;
        return nullptr;
      }
      auto semi = t.find(';');
      std::string head = t.substr(0, semi), tail = semi == std::string::npos ? "" : t.substr(semi + 1);
      if (head.size() > 6 && head[5] == ':' && !parse_wh(head.substr(6), &nw, &nh)) {
        *err = "bad_args: test " + t;
        return nullptr;
      }
      if (tail.rfind("start_ms=", 0) == 0) start_ms = std::atoi(tail.substr(9).c_str());
    }
    if (standby_.on && standby_.fmt.w == spec.fmt.w && standby_.fmt.h == spec.fmt.h) encoder = "standby:0";
    else encoder = used_encoder_ ? "warm:0" : "cold:0";
    used_encoder_ = true;
    cameras_open_++;
    cameras_opened_++;
    last_camera_open_ns_ = now_ns();
  }
  return std::make_unique<NullCamera>(this, spec, sink, nw, nh, start_ms, encoder);
}

// ------------------------------------------------------------------ fake decoder / virtual camera

class NullSink : public VideoSinkHandle {
 public:
  NullSink(NullVideoBackend* be, VideoSinkListener* l) : be_(be), l_(l) {
    thread_ = std::thread([this] { run(); });
  }
  ~NullSink() override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
    std::lock_guard<std::mutex> lock(be_->mu_);
    be_->sink_open_ = false;
    be_->vcam_.in_use = be_->vcam_.in_use;  // unchanged: demand belongs to the app
  }
  void decode(const uint8_t* au, size_t len, uint32_t frame, bool keep) override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      q_.push_back(Job{std::vector<uint8_t>(au, au + len), frame, keep});
    }
    cv_.notify_all();
  }
  void present(uint32_t frame) override {
    std::lock_guard<std::mutex> lock(mu_);
    std::lock_guard<std::mutex> l2(be_->mu_);
    auto it = pics_.find(frame);
    if (it == pics_.end()) {
      be_->present_unknown_++;
      return;
    }
    if (have_presented_ && static_cast<int32_t>(frame - last_presented_) <= 0) be_->backwards_++;
    if (!it->second.correct) {
      be_->incorrect_presented_++;
      be_->suspects_.erase(frame);
    }
    be_->presented_++;
    if (!be_->first_present_ns_) be_->first_present_ns_ = now_ns();
    be_->last_w_ = it->second.w;
    be_->last_h_ = it->second.h;
    have_presented_ = true;
    last_presented_ = frame;
    pics_.erase(pics_.begin(), std::next(it));
  }
  void hold() override {
    std::lock_guard<std::mutex> l2(be_->mu_);
    be_->holds_++;
  }
  void drop(uint32_t frame) override {
    std::lock_guard<std::mutex> lock(mu_);
    std::lock_guard<std::mutex> l2(be_->mu_);
    be_->suspects_.erase(frame);  // revoked: never shown
    if (pics_.erase(frame)) be_->drops_++;
  }
  void show_placeholder(const std::string& state) override {
    std::lock_guard<std::mutex> l2(be_->mu_);
    if (be_->placeholders_shown_.size() < 256) be_->placeholders_shown_.push_back(state);
  }

 private:
  struct Job {
    std::vector<uint8_t> au;
    uint32_t frame;
    bool keep;
  };
  struct Pic {
    bool correct;
    int w, h;
  };
  void run() {
    set_thread_name("null-video-dec");
    int64_t next_err = 0;
    for (;;) {
      Job j;
      {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [this] { return stop_ || !q_.empty(); });
        if (stop_) return;
        j = std::move(q_.front());
        q_.pop_front();
      }
      int dec_ms, err_ms;
      bool force_err = false;
      {
        std::lock_guard<std::mutex> l2(be_->mu_);
        dec_ms = be_->opt_.dec_ms;
        err_ms = be_->opt_.dec_err_ms;
        if (be_->pending_dec_errors_ > 0) {
          be_->pending_dec_errors_--;
          force_err = true;
        }
      }
      const int64_t t0 = now_ns();
      if (err_ms > 0) {
        if (next_err == 0) next_err = t0 + int64_t(err_ms) * 1'000'000;
        if (t0 >= next_err) {
          force_err = true;
          next_err = t0 + int64_t(err_ms) * 1'000'000;
        }
      }
      sleep_ns(int64_t(dec_ms) * 1'000'000);
      // parse
      const auto& a = j.au;
      bool intact = a.size() >= kAuHead && get_u32(a.data()) == kAuMagic && get_u32(a.data() + 4) == j.frame &&
                    get_u32(a.data() + 20) == a.size();
      for (size_t i = kAuHead; intact && i < a.size(); ++i) intact = a[i] == pattern(j.frame, i);
      if (!intact) {
        std::lock_guard<std::mutex> l2(be_->mu_);
        be_->dec_corrupt_input_++;
      }
      const uint8_t type = intact ? a[8] : 0;
      const int w = intact ? get_u16(a.data() + 10) : 0, h = intact ? get_u16(a.data() + 12) : 0;
      const uint32_t ref = intact ? get_u32(a.data() + 16) : kNoFrame;
      if (intact && type == kAuIdr) {
        // a new SPS: the decoder checks the DISPLAY size only (1080p is coded 1088 and cropped)
        if ((w & 1) || (h & 1) || w < 64 || h < 64 || w > 1920 || h > 1080) {
          {
            std::lock_guard<std::mutex> l2(be_->mu_);
            be_->size_rejected_++;
            be_->dec_frames_++;
          }
          have_sps_ = false;
          l_->on_sink_event("size_rejected", std::to_string(w) + "x" + std::to_string(h));
          continue;
        }
        have_sps_ = true;
      }
      bool ok = intact && have_sps_ && !force_err;
      bool correct = false;
      if (ok) {
        if (type == kAuIdr) correct = true;
        else {
          auto it = correct_.find(ref);
          correct = it != correct_.end() && it->second;
        }
      }
      if (!have_sps_) {
        std::lock_guard<std::mutex> l2(be_->mu_);
        be_->decoded_before_idr_++;
      }
      correct_[j.frame] = correct;
      while (correct_.size() > 4096) correct_.erase(correct_.begin());
      {
        std::lock_guard<std::mutex> lock(mu_);
        if (ok && j.keep) {
          pics_[j.frame] = Pic{correct, w, h};
          while (pics_.size() > 64) pics_.erase(pics_.begin());
        }
      }
      {
        std::lock_guard<std::mutex> l2(be_->mu_);
        be_->dec_frames_++;
        if (ok) be_->dec_ok_++;
        else be_->dec_err_++;
        if (ok && j.keep && correct && be_->mark_ns_ && !be_->kept_after_mark_ns_) be_->kept_after_mark_ns_ = now_ns();
        if (ok && j.keep) {
          be_->kept_++;
          if (!correct) {
            be_->kept_wrong_at_decode_++;
            be_->suspects_.insert(j.frame);  // an error must be revoked (drop) before it could be shown
          }
        }
      }
      l_->on_decoded(j.frame, ok, double(now_ns() - t0) * 1e-6);
    }
  }

  NullVideoBackend* be_;
  VideoSinkListener* l_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Job> q_;
  bool stop_ = false;
  // decoder thread
  bool have_sps_ = false;
  std::map<uint32_t, bool> correct_;
  // presenter side (mu_)
  std::map<uint32_t, Pic> pics_;
  bool have_presented_ = false;
  uint32_t last_presented_ = 0;
  std::thread thread_;
};

std::unique_ptr<VideoSinkHandle> NullVideoBackend::open_sink(VideoSinkListener* l, std::string* err) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (sink_open_) {
      *err = "internal: sink open";
      return nullptr;
    }
    if (vcam_.install != "ok") {
      *err = "not_installed: " + vcam_.install;
      return nullptr;
    }
    sink_open_ = true;
    sinks_opened_++;
  }
  return std::make_unique<NullSink>(this, l);
}

std::unique_ptr<VideoBackend> make_null_video_backend(const std::string& options) {
  return std::make_unique<NullVideoBackend>(options);
}

NullVideoBackend* as_null_video(VideoBackend* b) { return dynamic_cast<NullVideoBackend*>(b); }

Json null_video_report(NullVideoBackend* b) {
  std::lock_guard<std::mutex> lock(b->mu_);
  Json ph = Json::array();
  for (auto& s : b->placeholders_shown_) ph.push_back(s);
  return Json{{"enc", {{"frames", b->enc_frames_}, {"idr", b->enc_idr_}, {"refresh", b->enc_refresh_}, {"p", b->enc_p_},
                       {"number_mismatch", b->enc_number_mismatch_}, {"acks", b->enc_ack_}, {"acks_unknown", b->enc_ack_bad_},
                       {"refresh_as_idr", b->refresh_as_idr_}, {"start_encoding", b->start_encoding_},
                       {"first_au_ms", b->first_au_after_start_ms_}}},
              {"camera", {{"open", b->cameras_open_}, {"opened", b->cameras_opened_},
                          {"last_open_ns", b->last_camera_open_ns_}, {"last_close_ns", b->last_camera_close_ns_}}},
              {"standby", {{"calls", b->n_standby_}, {"on", b->standby_.on}, {"w", b->standby_.fmt.w}, {"h", b->standby_.fmt.h}}},
              {"dec", {{"frames", b->dec_frames_}, {"ok", b->dec_ok_}, {"errors", b->dec_err_},
                       {"corrupt_input", b->dec_corrupt_input_}, {"kept", b->kept_},
                       {"incorrect_kept", b->incorrect_presented_ + b->suspects_.size()},
                       {"kept_wrong_revoked", b->kept_wrong_at_decode_ - b->incorrect_presented_ - b->suspects_.size()},
                       {"before_idr", b->decoded_before_idr_}, {"size_rejected", b->size_rejected_}}},
              {"sink", {{"opened", b->sinks_opened_}, {"open", b->sink_open_}, {"presented", b->presented_},
                        {"incorrect_presented", b->incorrect_presented_}, {"backwards", b->backwards_},
                        {"holds", b->holds_}, {"drops", b->drops_}, {"present_unknown", b->present_unknown_},
                        {"w", b->last_w_}, {"h", b->last_h_}, {"placeholders", ph},
                        {"first_present_ns", b->first_present_ns_}}},
              {"mark", {{"at_ns", b->mark_ns_}, {"kept_after_ms",
                                                 b->kept_after_mark_ns_ ? Json(double(b->kept_after_mark_ns_ - b->mark_ns_) * 1e-6) : Json(nullptr)}}},
              {"vcam", {{"placeholder_cmds", b->n_placeholder_cmds_}, {"idle", b->idle_}, {"texts", b->texts_.size()}}}};
}

bool null_video_set(NullVideoBackend* b, const Json& j, std::string* err) {
  VideoBackendListener* l;
  bool vcam_changed = false, perm_changed = false;
  {
    std::lock_guard<std::mutex> lock(b->mu_);
    std::string s;
    bool v;
    int64_t n;
    if (jget_str(j, "perm", &s)) {
      perm_changed = s != b->opt_.perm;
      b->opt_.perm = s;
    }
    if (jget_bool(j, "vcam_in_use", &v)) {
      vcam_changed = v != b->vcam_.in_use;
      b->vcam_.in_use = v;
    }
    auto w = j.find("want");
    if (w != j.end() && w->is_object()) {
      int64_t ww = 0, hh = 0, ff = 30;
      jget_int(*w, "w", &ww);
      jget_int(*w, "h", &hh);
      jget_int(*w, "fps", &ff);
      b->vcam_.want = VideoFormat{int(ww), int(hh), int(ff)};
      vcam_changed = true;
    }
    if (jget_str(j, "install", &s)) {
      b->vcam_.install = s;
      vcam_changed = true;
    }
    if (jget_int(j, "loaded_version", &n)) {
      b->vcam_.loaded_version = int(n);
      vcam_changed = true;
    }
    if (jget_int(j, "dec_err_ms", &n)) b->opt_.dec_err_ms = int(n);
    if (jget_int(j, "decoder_errors", &n)) b->pending_dec_errors_ = int(n);
    if (jget_int(j, "cam_start_ms", &n)) b->opt_.cam_start_ms = int(n);
    if (jget_int(j, "cams", &n)) b->opt_.cams = int(std::max<int64_t>(0, std::min<int64_t>(16, n)));
    bool r;
    if (jget_bool(j, "mark", &r) && r) {
      b->mark_ns_ = now_ns();
      b->kept_after_mark_ns_ = 0;
    }
    if (jget_bool(j, "reset", &r) && r) {
      b->enc_idr_ = b->enc_refresh_ = b->enc_p_ = b->enc_frames_ = b->enc_number_mismatch_ = b->enc_ack_ = 0;
      b->enc_ack_bad_ = b->refresh_as_idr_ = b->start_encoding_ = 0;
      b->dec_frames_ = b->dec_ok_ = b->dec_err_ = b->dec_corrupt_input_ = b->incorrect_kept_ = b->kept_ = 0;
      b->presented_ = b->incorrect_presented_ = b->backwards_ = b->holds_ = b->drops_ = b->size_rejected_ = 0;
      b->present_unknown_ = b->decoded_before_idr_ = 0;
      b->suspects_.clear();
      b->kept_wrong_at_decode_ = 0;
      b->first_present_ns_ = 0;
      b->placeholders_shown_.clear();
    }
    l = b->listener_;
  }
  (void)err;
  if (l && vcam_changed) l->vcam_changed();
  if (l && perm_changed) l->permission_changed();
  return true;
}

}  // namespace beamer::audio
