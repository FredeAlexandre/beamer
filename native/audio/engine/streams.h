// SPDX-License-Identifier: GPL-3.0-only
// Streams (§3.2): send (capture -> encode -> seal -> flow), recv (flow -> open -> JB -> decode ->
// resample -> render), loop (local mic -> virtual mic through the drift controller), and the
// metering-only test input. IO-thread entry points touch only SPSC rings and atomics.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <thread>

#include "aec.h"
#include "codec.h"
#include "engine.h"
#include "jitter.h"
#include "levels.h"
#include "packet.h"
#include "resampler.h"
#include "spsc.h"
#include "test_signal.h"
#include "util.h"

namespace beamer::audio {

class Flow;
class HowlGuard;  // howl.h

// Channel mapping used by every stream (backends deliver/accept any count).
void map_channels(const float* in, int in_ch, float* out, int out_ch, int frames);

class StreamBase {
 public:
  explicit StreamBase(uint32_t id, std::string kind) : id_(id), kind_(std::move(kind)) {}
  virtual ~StreamBase() = default;
  uint32_t id() const { return id_; }
  const std::string& kind() const { return kind_; }
  virtual void stop() = 0;                       // main thread; idempotent
  virtual Json stats(double dt_s) = 0;           // control thread, once per second
  virtual bool is_output() const { return false; }  // for `levels` in/out grouping
  virtual Flow* flow() const { return nullptr; }
  virtual bool owns(const StreamHandle* h) const { return handle_ && handle_.get() == h; }
  std::string device_in_use() const { return handle_ ? handle_->device_in_use() : device_label_; }
  Meter meter;           // the stream's own signal
  Meter device_meter;    // what the device delivered/received (same here: one stream per handle)
  // permission "suspect": a capture that delivered only digital zero for 5 s
  int64_t zero_since_ns = 0;
  bool suspect_reported = false;

 protected:
  uint32_t id_;
  std::string kind_;
  std::unique_ptr<StreamHandle> handle_;
  std::string device_label_;
};

// ------------------------------------------------------------------ send

class SendStream : public StreamBase, public CaptureSink {
 public:
  struct Config {
    uint32_t id = 0;
    std::string kind = "mic";  // mic | system
    CodecConfig codec;
    uint8_t key[kKeyLen] = {0};
    std::string device;        // mic: key or "" (default)
    std::string test_signal;   // dev: "A" | "B" | sine spec -> synthetic source, no capture
    bool mute_local = false;
    bool aec = false;          // mic only: echo cancellation against aec_opt.reference (§7)
    AecOptions aec_opt;
  };
  SendStream(const Config& cfg, std::shared_ptr<Flow> flow);
  ~SendStream() override;
  // Opens the encoder and the capture (or the dev test source) and starts the encoder thread.
  bool start(Backend& backend, const std::vector<int64_t>& exclude_pids, std::string* err_code,
             std::string* err_msg);
  void stop() override;
  Json stats(double dt_s) override;
  Flow* flow() const override { return flow_.get(); }
  HowlGuard* howl() const { return howl_.get(); }  // system streams only (howl.h)

  // IO thread
  void on_capture(const float* interleaved, int frames, int channels, uint64_t host_ns) override;
  // network thread
  OpenResult ingest_rr(const uint8_t* data, size_t len, int64_t now);

 private:
  void encoder_loop();
  void test_source_loop();
  void push_samples(const float* x, int frames, uint64_t host_ns);  // codec channels, IO thread
  void encode_frame(const float* pcm, std::uniform_real_distribution<double>& uni, int link_cap);
  void aec_drain(std::vector<float>& pcm, std::uniform_real_distribution<double>& uni, int link_cap);

  Config cfg_;
  std::shared_ptr<Flow> flow_;
  int frame_;
  SpscFloatRing ring_;
  Signal wake_;
  OpusEnc enc_;
  Sealer sealer_;
  std::thread enc_thread_, test_thread_;
  std::atomic<bool> stop_{false};
  bool stopped_ = false;
  std::unique_ptr<float[]> conv_;  // IO-thread channel conversion scratch
  static constexpr int kConvFrames = 2048;
  uint32_t ts_;
  bool first_ = true;
  std::mt19937 rng_;
  ReplayWindow rr_window_;  // net thread
  // echo cancellation (null when off or when the reference could not be opened)
  std::unique_ptr<AecPath> aec_;
  std::string aec_error_;
  uint64_t mic_written_ = 0;  // IO thread: frames written to ring_
  uint64_t mic_read_ = 0;     // encoder thread: frames read from ring_
  std::vector<float> aec_in_, aec_out_, aec_pend_;  // encoder thread
  size_t aec_pend_n_ = 0;

  std::atomic<uint64_t> tx_{0}, tx_bytes_{0}, frames_{0}, overflow_{0}, dev_dropped_{0}, rr_rx_{0},
      enc_err_{0}, enc_ns_{0};
  uint64_t last_frames_ = 0, last_enc_ns_ = 0;
  std::atomic<int> peer_level_01ms_{-1};
  std::atomic<int64_t> test_t0_ns_{0};  // dev test source: now_ns() of its sample 0 (E2E timeline)
  std::unique_ptr<HowlGuard> howl_;     // encoder thread: ducks system sound that feeds back
  uint64_t last_tx_ = 0, last_tx_bytes_ = 0;
};

// ------------------------------------------------------------------ recv

class RecvStream : public StreamBase, public RenderSource {
 public:
  struct Config {
    uint32_t id = 0;
    std::string kind = "mic";
    CodecConfig codec;
    uint8_t key[kKeyLen] = {0};
    std::string device;        // "" = default output
    bool virtual_mic = false;
    JbConfig jb;               // tuning (tests)
  };
  RecvStream(const Config& cfg, std::shared_ptr<Flow> flow);
  ~RecvStream() override;
  bool start(Backend& backend, std::string* err_code, std::string* err_msg);
  void stop() override;
  Json stats(double dt_s) override;
  bool is_output() const override { return true; }
  Flow* flow() const override { return flow_.get(); }

  // IO thread
  void on_render(float* interleaved, int frames, int channels, uint64_t host_ns) override;
  // network thread
  OpenResult ingest(const uint8_t* data, size_t len, int64_t now, bool via_link);
  bool media_recent(int64_t now) const;
  // network thread: seal a receiver report (stream key, receiver direction). Returns length.
  size_t make_rr(uint8_t* out);

 private:
  void publish();  // IO thread: counters -> atomics

  Config cfg_;
  std::shared_ptr<Flow> flow_;
  SpscRing<MediaFrame> ring_;
  JitterBuffer jb_;
  std::unique_ptr<float[]> out_;  // JB output scratch (kMaxPull x codec channels)
  // network thread
  ReplayWindow window_;
  Sealer rr_sealer_;
  uint64_t hi_seq_ = 0;
  std::atomic<int64_t> last_media_ns_{0};
  std::atomic<uint32_t> newest_ts_{0};
  std::atomic<uint64_t> rx_pkts_{0}, rx_bytes_{0}, rx_link_{0}, ring_full_{0}, seq_hi_{0};
  std::atomic<bool> last_via_link_{false};
  // IO thread -> control thread
  std::atomic<double> level_ms_{0}, target_ms_{0}, ppm_{0}, integ_{0};
  std::atomic<uint64_t> c_late_{0}, c_expand_{0}, c_drop_{0}, c_flushed_{0}, c_fec_{0}, c_plc_{0},
      c_dec_err_{0}, c_underrun_{0}, c_ok_{0}, c_dup_{0};
  std::atomic<bool> playing_{false};
  SpscRing<float> level_samples_;
  // control thread: cumulative level histogram (0.5 ms bins) since start or dev reset_stats
  std::vector<uint32_t> hist_ = std::vector<uint32_t>(1024, 0);
  double hist_target_sum_ = 0;
  uint64_t hist_target_n_ = 0;
 public:
  void reset_run_stats() {
    std::fill(hist_.begin(), hist_.end(), 0u);
    hist_target_sum_ = 0;
    hist_target_n_ = 0;
  }
 private:
  // control thread (previous snapshot for deltas)
  uint64_t p_late_ = 0, p_expand_ = 0, p_drop_ = 0, p_flushed_ = 0, p_fec_ = 0, p_plc_ = 0,
           p_dec_err_ = 0, p_underrun_ = 0, p_rx_ = 0, p_bytes_ = 0, p_seq_hi_ = 0;
  bool stopped_ = false;
};

// ------------------------------------------------------------------ loop

// Local mic -> virtual mic, no codec, through the same drift controller and resampler.
class LoopStream : public StreamBase, public CaptureSink, public RenderSource {
 public:
  LoopStream(uint32_t id, std::string device);
  ~LoopStream() override;
  bool start(Backend& backend, const std::vector<int64_t>& exclude_pids, std::string* err_code,
             std::string* err_msg);
  void stop() override;
  Json stats(double dt_s) override;
  bool owns(const StreamHandle* h) const override {
    return StreamBase::owns(h) || (render_ && render_.get() == h);
  }
  std::string render_device() const { return render_ ? render_->device_in_use() : ""; }
  Meter out_meter;

  void on_capture(const float* interleaved, int frames, int channels, uint64_t host_ns) override;
  void on_render(float* interleaved, int frames, int channels, uint64_t host_ns) override;

 private:
  std::string device_;
  std::unique_ptr<StreamHandle> render_;
  SpscFloatRing ring_;
  Resampler rs_;
  DriftController drift_;
  std::unique_ptr<float[]> conv_, tmp_, out_;
  std::atomic<int> cap_block_{0};
  // render thread state
  bool playing_ = false;
  double target_ms_ = 10, level_avg_ = 0;
  bool has_avg_ = false;
  int block_max_ = 0;
  std::atomic<double> level_ms_{0}, tgt_{0}, ppm_{0};
  std::atomic<uint64_t> underruns_{0}, flushed_{0}, overflow_{0};
  bool stopped_ = false;
};

// ------------------------------------------------------------------ test_input

class TestInput : public StreamBase, public CaptureSink {
 public:
  TestInput(const std::string& device, int64_t until_ns);
  ~TestInput() override;
  bool start(Backend& backend, std::string* err_code, std::string* err_msg);
  void stop() override;
  Json stats(double) override { return Json(); }
  void on_capture(const float* interleaved, int frames, int channels, uint64_t host_ns) override;
  const std::string& device() const { return device_; }
  int64_t until_ns;

 private:
  std::string device_;
  bool stopped_ = false;
};

// "<code>: message" or "<code>" from a backend error string; unknown codes -> "internal".
std::string error_code_of(const std::string& backend_err);

}  // namespace beamer::audio
