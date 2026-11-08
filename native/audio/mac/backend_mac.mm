// SPDX-License-Identifier: GPL-3.0-only
// macOS backend (PHASE2 §2.4, WP3): streams on Core Audio IOProcs, device and default following,
// coreaudiod-restart and stall recovery, devices_changed debounce, virtual mic demand.
//
// Threads: every non-IO action (opening, closing, restarting, HAL listeners, the 250 ms watchdog)
// runs on one serial dispatch queue `q_`; listener callbacks to the engine are delivered on a second
// serial queue `notify_q_`, so the engine may open or close streams from inside them. The HAL's IO
// threads only run Port::io_proc: no locks, no allocation, no Objective-C, no syscalls.
#import <Foundation/Foundation.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

#include "backend_mac.h"
#include "mac_internal.h"

namespace beamer::audio {
namespace mac {
namespace {

constexpr int kTickMs = 250;                // watchdog period
constexpr double kStallS = 1.5;             // no IO callback for this long -> restart the stream
constexpr double kGraceS = 3.0;             // a device missing this long -> stream_lost
constexpr double kFeedGraceS = 6.0;         // the virtual mic feed (driver reinstall restarts coreaudiod)
constexpr int kDebounceMs = 300;            // devices_changed
constexpr OSStatus kNoMoreInput = 'nomr';

double seconds() { return double(now_ns()) / 1e9; }

// Interleaved channel conversion: down to mono = average, mono up = copy, otherwise the first
// min(src, dst) channels and silence for the rest.
inline void map_channels(const float* src, int sc, float* dst, int dc, int frames) {
  if (sc == dc) {
    memcpy(dst, src, sizeof(float) * size_t(frames) * size_t(sc));
  } else if (dc == 1) {
    const float inv = 1.0f / float(sc);
    for (int f = 0; f < frames; ++f) {
      float acc = 0;
      for (int c = 0; c < sc; ++c) acc += src[f * sc + c];
      dst[f] = acc * inv;
    }
  } else if (sc == 1) {
    for (int f = 0; f < frames; ++f)
      for (int c = 0; c < dc; ++c) dst[f * dc + c] = src[f];
  } else {
    for (int f = 0; f < frames; ++f)
      for (int c = 0; c < dc; ++c) dst[f * dc + c] = c < sc ? src[f * sc + c] : 0.0f;
  }
}

AudioStreamBasicDescription float_asbd(double rate, int ch) {
  AudioStreamBasicDescription f{};
  f.mSampleRate = rate;
  f.mFormatID = kAudioFormatLinearPCM;
  f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
  f.mBytesPerPacket = f.mBytesPerFrame = UInt32(4 * ch);
  f.mFramesPerPacket = 1;
  f.mChannelsPerFrame = UInt32(ch);
  f.mBitsPerChannel = 32;
  return f;
}

// Turns the streams of the other direction off for our IOProc: rendering to a USB headset must not
// run its microphone (privacy indicator), capturing must not keep its output running.
void disable_scope(AudioObjectID dev, AudioDeviceIOProcID proc, bool input_scope) {
  auto a = addr(kAudioDevicePropertyIOProcStreamUsage,
                input_scope ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput);
  UInt32 size = 0;
  if (!AudioObjectHasProperty(dev, &a) || AudioObjectGetPropertyDataSize(dev, &a, 0, nullptr, &size) != noErr ||
      size < sizeof(AudioHardwareIOProcStreamUsage))
    return;
  std::vector<uint8_t> raw(size);
  auto* u = reinterpret_cast<AudioHardwareIOProcStreamUsage*>(raw.data());
  u->mIOProc = reinterpret_cast<void*>(proc);
  if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, u) != noErr) return;
  if (u->mNumberStreams == 0) return;
  for (UInt32 i = 0; i < u->mNumberStreams; ++i) u->mStreamIsOn[i] = 0;
  AudioObjectSetPropertyData(dev, &a, 0, nullptr, size, u);
}

// ---------------------------------------------------------------------------------------------
// Port: one IOProc on one device, one direction, converting to/from 48 kHz float32 interleaved.

class Port {
 public:
  static std::unique_ptr<Port> open(AudioObjectID dev, bool input, int client_ch, CaptureSink* sink,
                                    RenderSource* source, int buffer_frames, std::string* err) {
    std::unique_ptr<Port> p(new Port());
    p->dev_ = dev;
    p->input_ = input;
    p->ch_ = std::max(1, client_ch);
    p->sink_ = sink;
    p->source_ = source;
    p->dev_ch_ = channel_count(dev, input);
    p->rate_ = nominal_rate(dev);
    if (p->dev_ch_ <= 0 || p->rate_ <= 0) {
      *err = "no_device: no " + std::string(input ? "input" : "output") + " channels";
      return nullptr;
    }
    if (!streams_are_float32(dev, input)) {
      *err = "unsupported: device stream format is not float32";
      return nullptr;
    }
    if (buffer_frames > 0) {
      UInt32 n = UInt32(buffer_frames);
      auto a = addr(kAudioDevicePropertyBufferFrameSize);
      AudioObjectSetPropertyData(dev, &a, 0, nullptr, sizeof(n), &n);
    }
    AudioValueRange range{0, 4096};
    {
      UInt32 size = sizeof(range);
      auto a = addr(kAudioDevicePropertyBufferFrameSizeRange);
      AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, &range);
    }
    p->max_frames_ = int(std::clamp(range.mMaximum, 4096.0, 16384.0));
    const double ratio = double(kRate) / p->rate_;
    p->max_client_frames_ = int(std::ceil(p->max_frames_ * std::max(1.0, ratio))) + 1024;
    const int wide = std::max(p->dev_ch_, p->ch_);
    p->dev_buf_.assign(size_t(p->max_frames_) * size_t(wide), 0.0f);
    p->client_buf_.assign(size_t(p->max_client_frames_) * size_t(p->ch_), 0.0f);
    p->conv_buf_.assign(size_t(p->max_client_frames_) * size_t(p->ch_), 0.0f);
    if (std::fabs(p->rate_ - kRate) > 0.01) {
      AudioStreamBasicDescription from = input ? float_asbd(p->rate_, p->ch_) : float_asbd(kRate, p->ch_);
      AudioStreamBasicDescription to = input ? float_asbd(kRate, p->ch_) : float_asbd(p->rate_, p->ch_);
      OSStatus st = AudioConverterNew(&from, &to, &p->conv_);
      if (st != noErr) {
        *err = "internal: AudioConverterNew " + os_status(st);
        return nullptr;
      }
      UInt32 q = kAudioConverterQuality_High;
      AudioConverterSetProperty(p->conv_, kAudioConverterSampleRateConverterQuality, sizeof(q), &q);
    }
    AudioDeviceIOProcID proc = nullptr;
    OSStatus st = AudioDeviceCreateIOProcID(dev, &Port::io_proc, p.get(), &proc);
    if (st != noErr || !proc) {
      *err = "internal: AudioDeviceCreateIOProcID " + os_status(st);
      return nullptr;
    }
    p->proc_ = proc;
    disable_scope(dev, proc, !input);
    st = AudioDeviceStart(dev, proc);
    if (st != noErr) {
      *err = "device_busy: AudioDeviceStart " + os_status(st);
      return nullptr;  // the destructor destroys the IOProc
    }
    p->running_ = true;
    UInt32 granted = 0;
    {
      UInt32 size = sizeof(granted);
      auto a = addr(kAudioDevicePropertyBufferFrameSize);
      AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, &granted);
    }
    p->granted_frames_ = int(granted);
    return p;
  }

  ~Port() {
    if (proc_) {
      if (running_) AudioDeviceStop(dev_, proc_);  // synchronous: the IOProc is not called afterwards
      AudioDeviceDestroyIOProcID(dev_, proc_);
    }
    if (conv_) AudioConverterDispose(conv_);
  }

  AudioObjectID device() const { return dev_; }
  double rate() const { return rate_; }
  int device_channels() const { return dev_ch_; }
  int granted_frames() const { return granted_frames_; }
  uint64_t callbacks() const { return callbacks_.load(std::memory_order_relaxed); }

 private:
  Port() = default;

  static OSStatus io_proc(AudioObjectID, const AudioTimeStamp*, const AudioBufferList* in, const AudioTimeStamp* in_time,
                          AudioBufferList* out, const AudioTimeStamp* out_time, void* ctx) {
    auto* p = static_cast<Port*>(ctx);
    p->callbacks_.fetch_add(1, std::memory_order_relaxed);
    if (p->input_) {
      if (in && in->mNumberBuffers) p->capture(in, in_time);
    } else if (out && out->mNumberBuffers) {
      p->render(out, out_time);
    }
    return noErr;
  }

  static uint64_t stamp_ns(const AudioTimeStamp* t) {
    if (t && (t->mFlags & kAudioTimeStampHostTimeValid)) return host_to_ns(t->mHostTime);
    return now_ns();
  }

  void capture(const AudioBufferList* in, const AudioTimeStamp* t) {
    const AudioBuffer& b0 = in->mBuffers[0];
    if (!b0.mData || !b0.mNumberChannels) return;
    int frames = int(b0.mDataByteSize / (4 * b0.mNumberChannels));
    frames = std::min(frames, max_frames_);
    if (frames <= 0) return;
    // Gather every input stream into one interleaved frame of dev_ch_ channels.
    const float* dev;
    if (in->mNumberBuffers == 1 && int(b0.mNumberChannels) == dev_ch_) {
      dev = static_cast<const float*>(b0.mData);
    } else {
      float* g = dev_buf_.data();
      int off = 0;
      for (UInt32 i = 0; i < in->mNumberBuffers && off < dev_ch_; ++i) {
        const AudioBuffer& b = in->mBuffers[i];
        const int nc = int(b.mNumberChannels);
        const float* s = static_cast<const float*>(b.mData);
        const int n = s ? std::min(frames, int(b.mDataByteSize / (4 * std::max(1, nc)))) : 0;
        for (int f = 0; f < frames; ++f)
          for (int c = 0; c < nc && off + c < dev_ch_; ++c) g[f * dev_ch_ + off + c] = f < n ? s[f * nc + c] : 0.0f;
        off += nc;
      }
      dev = g;
    }
    const float* client = dev;
    if (dev_ch_ != ch_) {
      map_channels(dev, dev_ch_, client_buf_.data(), ch_, frames);
      client = client_buf_.data();
    }
    const uint64_t ns = stamp_ns(t);
    if (!conv_) {
      sink_->on_capture(client, frames, ch_, ns);
      return;
    }
    push_ptr_ = client;
    push_frames_ = UInt32(frames);
    UInt32 packets = UInt32(max_client_frames_);
    AudioBufferList abl;
    abl.mNumberBuffers = 1;
    abl.mBuffers[0].mNumberChannels = UInt32(ch_);
    abl.mBuffers[0].mDataByteSize = packets * 4 * UInt32(ch_);
    abl.mBuffers[0].mData = conv_buf_.data();
    AudioConverterFillComplexBuffer(conv_, &Port::push_input, this, &packets, &abl, nullptr);
    if (packets) sink_->on_capture(conv_buf_.data(), int(packets), ch_, ns);
  }

  static OSStatus push_input(AudioConverterRef, UInt32* io_packets, AudioBufferList* data,
                             AudioStreamPacketDescription**, void* ctx) {
    auto* p = static_cast<Port*>(ctx);
    if (!p->push_frames_) {
      *io_packets = 0;
      return kNoMoreInput;
    }
    data->mNumberBuffers = 1;
    data->mBuffers[0].mNumberChannels = UInt32(p->ch_);
    data->mBuffers[0].mData = const_cast<float*>(p->push_ptr_);
    data->mBuffers[0].mDataByteSize = p->push_frames_ * 4 * UInt32(p->ch_);
    *io_packets = p->push_frames_;
    p->push_frames_ = 0;
    return noErr;
  }

  void render(AudioBufferList* out, const AudioTimeStamp* t) {
    AudioBuffer& b0 = out->mBuffers[0];
    if (!b0.mData || !b0.mNumberChannels) return;
    int frames = int(b0.mDataByteSize / (4 * b0.mNumberChannels));
    frames = std::min(frames, max_frames_);
    if (frames <= 0) return;
    const bool direct = !conv_ && out->mNumberBuffers == 1 && int(b0.mNumberChannels) == ch_ && dev_ch_ == ch_;
    float* client = direct ? static_cast<float*>(b0.mData) : client_buf_.data();
    pull_ns_ = stamp_ns(t);
    if (!conv_) {
      source_->on_render(client, frames, ch_, pull_ns_);
    } else {
      UInt32 packets = UInt32(frames);
      AudioBufferList abl;
      abl.mNumberBuffers = 1;
      abl.mBuffers[0].mNumberChannels = UInt32(ch_);
      abl.mBuffers[0].mDataByteSize = packets * 4 * UInt32(ch_);
      abl.mBuffers[0].mData = client;
      AudioConverterFillComplexBuffer(conv_, &Port::pull_input, this, &packets, &abl, nullptr);
      if (int(packets) < frames) memset(client + packets * ch_, 0, sizeof(float) * size_t(frames - int(packets)) * size_t(ch_));
    }
    if (direct) return;
    // Map to the device's channels and scatter into its output streams.
    const float* dev = client;
    if (dev_ch_ != ch_) {
      map_channels(client, ch_, dev_buf_.data(), dev_ch_, frames);
      dev = dev_buf_.data();
    }
    int off = 0;
    for (UInt32 i = 0; i < out->mNumberBuffers; ++i) {
      AudioBuffer& b = out->mBuffers[i];
      const int nc = int(b.mNumberChannels);
      float* d = static_cast<float*>(b.mData);
      if (!d || !nc) continue;
      const int n = std::min(frames, int(b.mDataByteSize / (4 * nc)));
      for (int f = 0; f < n; ++f)
        for (int c = 0; c < nc; ++c) d[f * nc + c] = off + c < dev_ch_ ? dev[f * dev_ch_ + off + c] : 0.0f;
      off += nc;
    }
  }

  static OSStatus pull_input(AudioConverterRef, UInt32* io_packets, AudioBufferList* data,
                             AudioStreamPacketDescription**, void* ctx) {
    auto* p = static_cast<Port*>(ctx);
    const UInt32 n = std::min(*io_packets, UInt32(p->max_client_frames_));
    p->source_->on_render(p->conv_buf_.data(), int(n), p->ch_, p->pull_ns_);
    p->pull_ns_ += uint64_t(n) * 1000000000ull / uint64_t(kRate);
    data->mNumberBuffers = 1;
    data->mBuffers[0].mNumberChannels = UInt32(p->ch_);
    data->mBuffers[0].mData = p->conv_buf_.data();
    data->mBuffers[0].mDataByteSize = n * 4 * UInt32(p->ch_);
    *io_packets = n;
    return noErr;
  }

  AudioObjectID dev_ = 0;
  bool input_ = true;
  int ch_ = 1, dev_ch_ = 0, max_frames_ = 4096, max_client_frames_ = 4096, granted_frames_ = 0;
  double rate_ = 0;
  CaptureSink* sink_ = nullptr;
  RenderSource* source_ = nullptr;
  AudioDeviceIOProcID proc_ = nullptr;
  bool running_ = false;
  AudioConverterRef conv_ = nullptr;
  std::vector<float> dev_buf_, client_buf_, conv_buf_;
  const float* push_ptr_ = nullptr;
  UInt32 push_frames_ = 0;
  uint64_t pull_ns_ = 0;
  std::atomic<uint64_t> callbacks_{0};
};

// ---------------------------------------------------------------------------------------------

enum class SKind { Mic, System, Reference, Render, Feed };

const char* kind_name(SKind k) {
  switch (k) {
    case SKind::Mic: return "mic";
    case SKind::System: return "system";
    case SKind::Reference: return "reference";
    case SKind::Render: return "render";
    case SKind::Feed: return "virtual_mic";
  }
  return "?";
}

class MacBackend;

class MacStream final : public StreamHandle {
 public:
  MacStream(MacBackend* be, SKind kind) : be_(be), kind(kind) {}
  ~MacStream() override;
  std::string device_in_use() const override {
    std::lock_guard<std::mutex> lock(key_mu_);
    return key_;
  }
  void set_key(const std::string& k) {
    std::lock_guard<std::mutex> lock(key_mu_);
    key_ = k;
  }

  MacBackend* be_;
  const SKind kind;
  // Fixed at open:
  std::string want;  // device key asked for, "" = the default (followed)
  int channels = 1;
  bool mute_local = false;
  std::vector<pid_t> exclude;
  CaptureSink* sink = nullptr;
  RenderSource* source = nullptr;
  // Owned by the backend queue:
  std::unique_ptr<Port> port;
  Tap tap;
  AudioObjectID target = 0;  // the device the stream is bound to (tapped output for Reference)
  bool lost = false;
  double down_since = 0;     // > 0 while the stream has no running port
  double started_at = 0, last_progress = 0;
  uint64_t last_callbacks = 0;
  std::string last_error;

 private:
  mutable std::mutex key_mu_;
  std::string key_;
};

class MacBackend final : public Backend {
 public:
  MacBackend() {
    q_ = dispatch_queue_create("beamer.audio.mac", DISPATCH_QUEUE_SERIAL);
    notify_q_ = dispatch_queue_create("beamer.audio.mac.notify", DISPATCH_QUEUE_SERIAL);
    if (const char* s = getenv("BEAMER_AUDIO_MAC_BUFFER_FRAMES")) buffer_frames_ = std::max(0, atoi(s));
    // HAL notifications go to the main run loop by default; the helper's main thread reads stdin and
    // runs no run loop, so let the HAL use its own thread (the blocks then hop onto q_).
    CFRunLoopRef no_run_loop = nullptr;
    auto rl = addr(kAudioHardwarePropertyRunLoop);
    AudioObjectSetPropertyData(kAudioObjectSystemObject, &rl, 0, nullptr, sizeof(no_run_loop), &no_run_loop);
    dispatch_sync(q_, ^{
      demand_ = std::make_unique<DemandWatcher>(q_, [this](bool on) {
        log_line("info", "virtual mic in use: %s", on ? "yes" : "no");
        notify_devices_changed();
      });
      demand_->refresh();
      add_system_listeners();
      refresh_device_listeners();
      signature_ = device_signature();
      timer_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q_);
      dispatch_source_set_timer(timer_, dispatch_time(DISPATCH_TIME_NOW, kTickMs * NSEC_PER_MSEC),
                                kTickMs * NSEC_PER_MSEC, 20 * NSEC_PER_MSEC);
      dispatch_source_set_event_handler(timer_, ^{ tick(); });
      dispatch_resume(timer_);
    });
  }

  ~MacBackend() override {
    dispatch_sync(q_, ^{
      dispatch_source_cancel(timer_);
      for (MacStream* s : streams_) {  // the engine should have closed them; detach the rest
        stop_stream(s);
        s->be_ = nullptr;
      }
      streams_.clear();
      remove_system_listeners();
      for (auto& [dev, bl] : dev_blocks_) remove_device_listeners(dev, bl);
      dev_blocks_.clear();
      demand_.reset();
      alive_ = false;
    });
    listener_.store(nullptr);
    dispatch_sync(notify_q_, ^{});
  }

  void set_listener(BackendListener* l) override { listener_.store(l); }

  DeviceSnapshot snapshot() override {
    DeviceSnapshot snap;
    fill_inventory(snap);
    snap.vmic_kind = "beamer";
    snap.vmic_present = device_for_uid(kVmicUID) != 0 && device_for_uid(kFeedUID) != 0;
    snap.vmic_in_use = demand_ && demand_->in_use();
    return snap;
  }

  std::unique_ptr<StreamHandle> open_capture(const CaptureSpec& spec, CaptureSink* sink, std::string* err) override {
    if (!sink) return fail(err, "bad_args: no sink");
    SKind kind = spec.kind == CaptureKind::Mic ? SKind::Mic : spec.kind == CaptureKind::System ? SKind::System : SKind::Reference;
    if (kind != SKind::Mic && !taps_supported()) return fail(err, "unsupported: process taps need macOS 14.2");
    if (kind == SKind::Mic && mic_permission() == "denied") return fail(err, "permission: microphone access denied");
    auto s = std::make_unique<MacStream>(this, kind);
    s->want = spec.device;
    s->channels = std::clamp(spec.channels, 1, 8);
    s->mute_local = spec.mute_local && kind == SKind::System;
    for (int64_t pid : spec.exclude_pids)
      if (pid > 0) s->exclude.push_back(pid_t(pid));
    s->sink = sink;
    return open_stream(std::move(s), err);
  }

  std::unique_ptr<StreamHandle> open_render(const RenderSpec& spec, RenderSource* source, std::string* err) override {
    if (!source) return fail(err, "bad_args: no source");
    auto s = std::make_unique<MacStream>(this, spec.virtual_mic ? SKind::Feed : SKind::Render);
    s->want = spec.virtual_mic ? std::string() : spec.device;
    s->channels = std::clamp(spec.channels, 1, 8);
    s->source = source;
    return open_stream(std::move(s), err);
  }

  int restore_journal() override { return 0; }

  std::vector<std::string> features() override {
    std::vector<std::string> f;
    if (taps_supported()) {
      f.push_back("system_capture");
      f.push_back("local_mute_tap");
    }
    f.push_back("mic_demand");
    f.push_back("virtual_mic");
    if (taps_supported()) f.push_back("aec");  // = CaptureKind::Reference (a tap); engine_features() decides
    return f;
  }

  std::string permission(const std::string& what) override {
    if (what == "mic") return mic_permission();
    return "unknown";  // system audio: a denied tap delivers silence; the engine reports "suspect"
  }

  // Called by ~MacStream (any thread but the queues).
  void close_stream(MacStream* s) {
    {
      std::lock_guard<std::mutex> lock(live_mu_);
      live_.erase(s);
    }
    dispatch_sync(q_, ^{
      auto it = std::find(streams_.begin(), streams_.end(), s);
      if (it != streams_.end()) streams_.erase(it);
      stop_stream(s);
      log_line("info", "%s stream closed", kind_name(s->kind));
    });
  }

 private:
  static std::unique_ptr<StreamHandle> fail(std::string* err, const char* msg) {
    if (err) *err = msg;
    return nullptr;
  }

  std::unique_ptr<StreamHandle> open_stream(std::unique_ptr<MacStream> s, std::string* err) {
    __block bool ok = false;
    __block std::string e;
    MacStream* raw = s.get();
    dispatch_sync(q_, ^{
      ok = start_stream(raw, &e);
      if (ok) streams_.push_back(raw);
    });
    if (!ok) {
      if (err) *err = e;
      log_line("warn", "open %s '%s' failed: %s", kind_name(raw->kind), raw->want.c_str(), e.c_str());
      s->be_ = nullptr;  // never registered: the destructor has nothing to close
      return nullptr;
    }
    {
      std::lock_guard<std::mutex> lock(live_mu_);
      live_.insert(raw);
    }
    return s;
  }

  // ---- on q_ ----

  // The device a stream should run on now (0 = none). For taps: the tapped output device, or the
  // aggregate's absence (System: 0 means "global, no device").
  AudioObjectID resolve(MacStream* s, std::string* err) {
    switch (s->kind) {
      case SKind::Feed: {
        AudioObjectID d = device_for_uid(kFeedUID);
        if (!d) *err = "no_virtual_mic: Beamer Microphone is not installed";
        return d;
      }
      case SKind::System: return 0;
      default: break;
    }
    const bool input = s->kind == SKind::Mic;
    AudioObjectID d = s->want.empty() ? default_device(input) : device_for_uid(s->want);
    if (!d) {
      *err = s->want.empty() ? std::string("no_device: no default ") + (input ? "input" : "output")
                             : "no_device: " + s->want;
      return 0;
    }
    const std::string uid = device_uid(d);
    if (uid == kVmicUID || uid == kFeedUID || is_blocked_device(d)) {
      *err = "blocked_device: " + device_name(d);
      return 0;
    }
    if (channel_count(d, input) == 0) {
      *err = "no_device: " + device_name(d) + " has no " + (input ? "input" : "output");
      return 0;
    }
    return d;
  }

  bool start_stream(MacStream* s, std::string* err) {
    stop_stream(s);
    std::string e;
    AudioObjectID target = resolve(s, &e);
    if (!target && s->kind != SKind::System) {
      *err = e;
      return false;
    }
    std::unique_ptr<Port> port;
    std::string key;
    if (s->kind == SKind::System || s->kind == SKind::Reference) {
      TapSpec ts;
      ts.global = s->kind == SKind::System;
      ts.mute_local = s->mute_local;
      if (s->kind == SKind::System) {
        ts.exclude = s->exclude;
        ts.exclude_self = true;
        AudioObjectID out = default_device(false);
        key = out ? device_uid(out) : std::string();
      } else {
        ts.device_uid = device_uid(target);
        ts.exclude_self = false;  // the AEC reference includes Beamer's own playback
        ts.exclude = s->exclude;  // empty except in tests (engine `aec.exclude_pids`, --dev)
        key = ts.device_uid;
      }
      if (!create_tap(ts, &s->tap, err)) return false;
      port = Port::open(s->tap.aggregate, true, s->channels, s->sink, nullptr, buffer_frames_, err);
      if (!port) {
        destroy_tap(&s->tap);
        return false;
      }
    } else {
      const bool input = s->kind == SKind::Mic;
      port = Port::open(target, input, s->channels, input ? s->sink : nullptr, input ? nullptr : s->source,
                        buffer_frames_, err);
      if (!port) return false;
      key = device_uid(target);
    }
    log_line("info", "%s stream on '%s' (%s): %d ch at %.0f Hz, buffer %d, client %d ch", kind_name(s->kind),
             key.c_str(), device_name(port->device()).c_str(), port->device_channels(), port->rate(),
             port->granted_frames(), s->channels);
    s->target = target;
    s->port = std::move(port);
    s->set_key(key);
    s->down_since = 0;
    s->started_at = s->last_progress = seconds();
    s->last_callbacks = 0;
    s->last_error.clear();
    return true;
  }

  void stop_stream(MacStream* s) {
    s->port.reset();
    destroy_tap(&s->tap);
    s->target = 0;
  }

  void restart(MacStream* s, const char* why) {
    if (s->lost) return;
    if (strcmp(why, "retry") != 0) log_line("info", "restarting %s stream (%s)", kind_name(s->kind), why);
    std::string err;
    if (!start_stream(s, &err)) {
      if (!s->down_since) s->down_since = seconds();
      if (err != s->last_error) log_line("warn", "%s stream down: %s", kind_name(s->kind), err.c_str());
      s->last_error = err;
    }
  }

  // Bring every stream in line with the current devices and defaults.
  void reconcile(const char* why) {
    for (MacStream* s : streams_) {
      if (s->lost) continue;
      if (!s->port) {
        restart(s, why);
        continue;
      }
      if (std::fabs(nominal_rate(s->port->device()) - s->port->rate()) > 0.01) {
        restart(s, "sample rate changed");  // for taps: the tapped output's rate moved the aggregate
        continue;
      }
      if (s->kind == SKind::System) continue;  // a global tap is not tied to a device
      std::string err;
      if (resolve(s, &err) != s->target) restart(s, why);
    }
  }

  void tick() {
    if (!alive_) return;
    const double now = seconds();
    for (MacStream* s : streams_) {
      if (s->lost) continue;
      if (s->port) {
        const uint64_t cb = s->port->callbacks();
        if (cb != s->last_callbacks) {
          s->last_callbacks = cb;
          s->last_progress = now;
        } else if (now - s->last_progress > kStallS) {
          restart(s, "no IO for 1.5 s");
        }
        if (s->kind == SKind::System && !s->tap.pending.empty()) late_exclusions(s);
        continue;
      }
      restart(s, "retry");
      const double grace = s->kind == SKind::Feed ? kFeedGraceS : kGraceS;
      if (!s->port && s->down_since && now - s->down_since > grace) {
        s->lost = true;
        std::string why = s->last_error.empty() ? std::string("device_gone") : s->last_error;
        log_line("warn", "%s stream lost: %s", kind_name(s->kind), why.c_str());
        notify_lost(s, why);
      }
    }
    if (++tick_count_ % 4 == 0) {  // 1 s: poll as a safety net for missed HAL notifications
      demand_->refresh();
      std::string sig = device_signature();
      if (sig != signature_) {
        signature_ = sig;
        on_devices_changed("device poll");
      }
    }
  }

  void late_exclusions(MacStream* s) {
    if (int n = add_late_exclusions(&s->tap)) log_line("info", "added %d late exclusion(s) to the system tap", n);
  }

  std::string device_signature() {
    std::string sig;
    for (AudioObjectID d : all_devices()) sig += std::to_string(d) + ",";
    sig += "|" + std::to_string(default_device(true)) + "|" + std::to_string(default_device(false));
    sig += "|" + std::to_string(device_for_uid(kVmicUID)) + "|" + std::to_string(device_for_uid(kFeedUID));
    return sig;
  }

  void on_devices_changed(const char* why) {
    signature_ = device_signature();
    refresh_device_listeners();
    demand_->refresh();
    reconcile(why);
    schedule_devices_changed();
  }

  void on_service_restarted() {
    log_line("info", "coreaudiod restarted: reopening every stream");
    for (auto& [dev, bl] : dev_blocks_) remove_device_listeners(dev, bl);
    dev_blocks_.clear();
    for (MacStream* s : streams_) {
      if (s->lost) continue;
      stop_stream(s);
      if (!s->down_since) s->down_since = seconds();
    }
    on_devices_changed("coreaudiod restarted");
  }

  void schedule_devices_changed() {
    const uint64_t gen = ++debounce_gen_;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kDebounceMs * NSEC_PER_MSEC), q_, ^{
      if (alive_ && gen == debounce_gen_) notify_devices_changed();
    });
  }

  void notify_devices_changed() {
    dispatch_async(notify_q_, ^{
      if (BackendListener* l = listener_.load()) l->devices_changed();
    });
  }

  void notify_lost(MacStream* s, const std::string& why) {
    std::string w = why;
    dispatch_async(notify_q_, ^{
      BackendListener* l = listener_.load();
      {
        std::lock_guard<std::mutex> lock(live_mu_);
        if (!l || !live_.count(s)) return;
      }
      l->stream_lost(s, w);
    });
  }

  // ---- HAL listeners (q_) ----

  static constexpr AudioObjectPropertySelector kSystemSelectors[] = {
      kAudioHardwarePropertyDevices, kAudioHardwarePropertyDefaultInputDevice,
      kAudioHardwarePropertyDefaultOutputDevice, kAudioHardwarePropertyServiceRestarted,
      kAudioHardwarePropertyProcessObjectList};

  void add_system_listeners() {
    sys_block_ = ^(UInt32 n, const AudioObjectPropertyAddress* addrs) {
      if (!alive_) return;
      bool devices = false, restarted = false, processes = false;
      for (UInt32 i = 0; i < n; ++i) {
        switch (addrs[i].mSelector) {
          case kAudioHardwarePropertyServiceRestarted: restarted = true; break;
          case kAudioHardwarePropertyProcessObjectList: processes = true; break;
          default: devices = true; break;
        }
      }
      if (restarted) on_service_restarted();
      else if (devices) on_devices_changed("devices or defaults changed");
      if (processes)
        for (MacStream* s : streams_)
          if (s->kind == SKind::System && !s->tap.pending.empty()) late_exclusions(s);
    };
    for (auto sel : kSystemSelectors) {
      auto a = addr(sel);
      OSStatus st = AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &a, q_, sys_block_);
      if (st != noErr) log_line("warn", "system listener %s failed: %s", os_status(OSStatus(sel)).c_str(), os_status(st).c_str());
    }
  }

  void remove_system_listeners() {
    if (!sys_block_) return;
    for (auto sel : kSystemSelectors) {
      auto a = addr(sel);
      AudioObjectRemovePropertyListenerBlock(kAudioObjectSystemObject, &a, q_, sys_block_);
    }
    sys_block_ = nil;
  }

  // Per device: nominal rate, stream layout and (outputs) data source, which changes the form when
  // headphones are plugged into the built-in jack.
  static std::vector<AudioObjectPropertyAddress> device_addresses() {
    return {addr(kAudioDevicePropertyNominalSampleRate),
            addr(kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeInput),
            addr(kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput),
            addr(kAudioDevicePropertyDataSource, kAudioObjectPropertyScopeOutput),
            addr(kAudioDevicePropertyDataSource, kAudioObjectPropertyScopeInput)};
  }

  void refresh_device_listeners() {
    std::vector<AudioObjectID> devs = all_devices();
    if (AudioObjectID feed = device_for_uid(kFeedUID)) devs.push_back(feed);
    for (auto it = dev_blocks_.begin(); it != dev_blocks_.end();) {
      if (std::find(devs.begin(), devs.end(), it->first) == devs.end()) {
        remove_device_listeners(it->first, it->second);
        it = dev_blocks_.erase(it);
      } else {
        ++it;
      }
    }
    for (AudioObjectID d : devs) {
      if (dev_blocks_.count(d)) continue;
      AudioObjectPropertyListenerBlock bl = ^(UInt32, const AudioObjectPropertyAddress*) {
        if (!alive_) return;
        reconcile("device property changed");
        schedule_devices_changed();
      };
      for (auto a : device_addresses())
        if (AudioObjectHasProperty(d, &a)) AudioObjectAddPropertyListenerBlock(d, &a, q_, bl);
      dev_blocks_[d] = bl;
    }
  }

  void remove_device_listeners(AudioObjectID d, AudioObjectPropertyListenerBlock bl) {
    for (auto a : device_addresses()) AudioObjectRemovePropertyListenerBlock(d, &a, q_, bl);
  }

  dispatch_queue_t q_ = nil, notify_q_ = nil;
  dispatch_source_t timer_ = nil;
  std::atomic<BackendListener*> listener_{nullptr};
  std::vector<MacStream*> streams_;
  std::unique_ptr<DemandWatcher> demand_;
  AudioObjectPropertyListenerBlock sys_block_ = nil;
  std::map<AudioObjectID, AudioObjectPropertyListenerBlock> dev_blocks_;
  std::string signature_;
  uint64_t debounce_gen_ = 0, tick_count_ = 0;
  int buffer_frames_ = 480;
  bool alive_ = true;
  std::mutex live_mu_;
  std::set<MacStream*> live_;
};

MacStream::~MacStream() {
  if (be_) be_->close_stream(this);
}

}  // namespace
}  // namespace mac

std::unique_ptr<Backend> make_mac_backend(const std::string&) { return std::make_unique<mac::MacBackend>(); }

}  // namespace beamer::audio
