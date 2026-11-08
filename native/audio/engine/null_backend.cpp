// SPDX-License-Identifier: GPL-3.0-only
#include "null_backend.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "test_signal.h"
#include "util.h"

namespace beamer::audio {

namespace {

struct GroupCfg {
  std::string signal;  // capture: signal spec; render: "" | "record:<path>"
  double ppm = 0;
  int period = 240;
  bool present = true;
};

void write_wav_header(FILE* f, uint32_t frames, int channels) {
  uint32_t data = frames * channels * 4;
  uint8_t h[44];
  auto le32 = [&](int o, uint32_t v) {
    for (int i = 0; i < 4; ++i) h[o + i] = uint8_t(v >> (8 * i));
  };
  auto le16 = [&](int o, uint16_t v) {
    h[o] = uint8_t(v);
    h[o + 1] = uint8_t(v >> 8);
  };
  std::memcpy(h, "RIFF", 4);
  le32(4, 36 + data);
  std::memcpy(h + 8, "WAVEfmt ", 8);
  le32(16, 16);
  le16(20, 3);  // IEEE float
  le16(22, uint16_t(channels));
  le32(24, kRate);
  le32(28, kRate * channels * 4);
  le16(32, uint16_t(channels * 4));
  le16(34, 32);
  std::memcpy(h + 36, "data", 4);
  le32(40, data);
  std::fseek(f, 0, SEEK_SET);
  std::fwrite(h, 1, 44, f);
  std::fseek(f, 0, SEEK_END);
}

// One open stream = one thread paced by the steady clock scaled by (1 + ppm).
class NullStream : public StreamHandle {
 public:
  NullStream(std::string key, bool capture, int channels, const GroupCfg& g, CaptureSink* sink,
             RenderSource* src)
      : key_(std::move(key)), capture_(capture), ch_(channels), g_(g), sink_(sink), src_(src) {
    if (capture_) TestSignal::parse(g_.signal.empty() ? "silence" : g_.signal, &sig_);
    if (!capture_ && g_.signal.rfind("record:", 0) == 0) {
      file_ = std::fopen(g_.signal.substr(7).c_str(), "wb");
      if (file_) write_wav_header(file_, 0, ch_);
    }
    thread_ = std::thread([this] { run(); });
  }
  ~NullStream() override {
    stop_.store(true);
    thread_.join();
    if (file_) {
      write_wav_header(file_, recorded_, ch_);
      std::fclose(file_);
    }
  }
  std::string device_in_use() const override { return key_; }
  const std::string& key() const { return key_; }

 private:
  void run() {
    set_thread_name(capture_ ? "null-capture" : "null-render");
    set_thread_interactive();  // stands in for a real-time device IO thread
    const int P = g_.period;
    std::vector<float> mono(P), buf(size_t(P) * ch_);
    const double rate = kRate * (1.0 + g_.ppm * 1e-6);
    const auto t0 = std::chrono::steady_clock::now();
    int64_t s = 0;
    while (!stop_.load()) {
      auto due = t0 + std::chrono::nanoseconds(static_cast<int64_t>((s + P) * 1e9 / rate));
      std::this_thread::sleep_until(due);
      if (stop_.load()) break;
      const uint64_t host = static_cast<uint64_t>(now_ns());
      if (capture_) {
        sig_.render(s, P, mono.data());
        for (int i = 0; i < P; ++i)
          for (int c = 0; c < ch_; ++c) buf[size_t(i) * ch_ + c] = mono[i];
        sink_->on_capture(buf.data(), P, ch_, host);
      } else {
        src_->on_render(buf.data(), P, ch_, host);
        if (file_) {
          std::fwrite(buf.data(), sizeof(float), buf.size(), file_);
          recorded_ += static_cast<uint32_t>(P);
        }
      }
      s += P;
    }
  }

  std::string key_;
  bool capture_;
  int ch_;
  GroupCfg g_;
  CaptureSink* sink_;
  RenderSource* src_;
  TestSignal sig_;
  FILE* file_ = nullptr;
  uint32_t recorded_ = 0;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

}  // namespace

class NullBackend : public Backend {
 public:
  explicit NullBackend(const std::string& options) {
    groups_["mic"].signal = "sine:440:0.25";
    groups_["system"].signal = "sine:1000:0.25";
    groups_["out"].signal = "";
    groups_["vmic"].signal = "";
    groups_["ref"].signal = "silence";  // what null-out "plays" (AEC reference capture)
    double gppm = 0;
    int gperiod = 0;
    bool has_gppm = false;
    size_t start = 0;
    while (start <= options.size()) {
      size_t end = options.find(';', start);
      std::string grp = options.substr(start, end == std::string::npos ? std::string::npos : end - start);
      start = end == std::string::npos ? options.size() + 1 : end + 1;
      if (grp.empty()) continue;
      std::vector<std::string> kv;
      size_t a = 0;
      while (a <= grp.size()) {
        size_t b = grp.find(',', a);
        kv.push_back(grp.substr(a, b == std::string::npos ? std::string::npos : b - a));
        a = b == std::string::npos ? grp.size() + 1 : b + 1;
      }
      size_t eq = kv[0].find('=');
      std::string name = kv[0].substr(0, eq), value = eq == std::string::npos ? "" : kv[0].substr(eq + 1);
      if (name == "ppm") {
        gppm = std::atof(value.c_str());
        has_gppm = true;
        continue;
      }
      if (name == "period") {
        gperiod = std::atoi(value.c_str());
        continue;
      }
      if (!groups_.count(name)) {
        BA_WARN("null backend: unknown group %s", name.c_str());
        continue;
      }
      GroupCfg& g = groups_[name];
      if (value == "absent") g.present = false;
      else if (value == "null") g.signal = "";
      else g.signal = value;
      for (size_t i = 1; i < kv.size(); ++i) {
        size_t e = kv[i].find('=');
        std::string k = kv[i].substr(0, e), v = e == std::string::npos ? "" : kv[i].substr(e + 1);
        if (k == "ppm") g.ppm = std::atof(v.c_str());
        else if (k == "period") g.period = std::max(16, std::atoi(v.c_str()));
        else if (k == "in_use") vmic_in_use_ = v == "1" || v == "true";
        else if (k == "kind") vmic_kind_ = v;
      }
    }
    for (auto& kv : groups_) {
      if (has_gppm && kv.second.ppm == 0) kv.second.ppm = gppm;
      if (gperiod > 0) kv.second.period = gperiod;
    }
    present_["null-mic"] = groups_["mic"].present;
    present_["null-out"] = groups_["out"].present;
    present_["null-vmic"] = groups_["vmic"].present;
  }

  void set_listener(BackendListener* l) override { listener_ = l; }

  DeviceSnapshot snapshot() override {
    std::lock_guard<std::mutex> lock(mu_);
    DeviceSnapshot s;
    if (present_["null-mic"]) {
      DeviceInfo d;
      d.key = "null-mic";
      d.name = "Null Microphone";
      d.manufacturer = "Beamer";
      d.transport = "builtin";
      d.input = true;
      d.channels = 1;
      d.nominal_rate = kRate;
      d.form = Form::Microphone;
      s.devices.push_back(d);
      s.default_input = d.key;
    }
    if (present_["null-out"]) {
      DeviceInfo d;
      d.key = "null-out";
      d.name = "Null Speakers";
      d.manufacturer = "Beamer";
      d.transport = "builtin";
      d.output = true;
      d.channels = 2;
      d.nominal_rate = kRate;
      d.form = Form::Speakers;
      s.devices.push_back(d);
      s.default_output = d.key;
    }
    if (present_["null-vmic"]) {
      DeviceInfo d;
      d.key = "null-vmic";
      d.name = "Null Virtual Microphone";
      d.manufacturer = "Beamer";
      d.transport = "virtual";
      d.input = true;
      d.channels = 1;
      d.nominal_rate = kRate;
      d.role = Role::VirtualMic;
      s.devices.push_back(d);
      s.vmic_kind = vmic_kind_;
      s.vmic_present = true;
      s.vmic_in_use = vmic_in_use_;
    }
    return s;
  }

  std::unique_ptr<StreamHandle> open_capture(const CaptureSpec& spec, CaptureSink* sink,
                                             std::string* err) override {
    std::lock_guard<std::mutex> lock(mu_);
    std::string key, group;
    if (spec.kind == CaptureKind::System) {
      key = "null-system";
      group = "system";
    } else if (spec.kind == CaptureKind::Reference) {
      key = spec.device.empty() ? "null-out" : spec.device;
      group = "ref";
      if (key != "null-out" || !present_["null-out"]) {
        *err = "no_device: " + key;
        return nullptr;
      }
    } else {
      key = spec.device.empty() ? "null-mic" : spec.device;
      group = "mic";
      if (key == "null-vmic" || key == "null-out") {
        *err = "blocked_device: " + key;
        return nullptr;
      }
      if (key != "null-mic" || !present_["null-mic"]) {
        *err = "no_device: " + key;
        return nullptr;
      }
    }
    if (spec.channels <= 0 || spec.channels > 8) {
      *err = "bad_args: channels";
      return nullptr;
    }
    auto h = std::make_unique<NullStream>(key, true, spec.channels, groups_[group], sink, nullptr);
    open_.insert(h.get());
    return wrap(std::move(h));
  }

  std::unique_ptr<StreamHandle> open_render(const RenderSpec& spec, RenderSource* src,
                                            std::string* err) override {
    std::lock_guard<std::mutex> lock(mu_);
    std::string key = spec.virtual_mic ? "null-vmic" : (spec.device.empty() ? "null-out" : spec.device);
    if (spec.virtual_mic && !present_["null-vmic"]) {
      *err = "no_virtual_mic";
      return nullptr;
    }
    if (!spec.virtual_mic && (key != "null-out" || !present_["null-out"])) {
      *err = "no_device: " + key;
      return nullptr;
    }
    int ch = spec.virtual_mic ? 1 : 2;  // device channel count; the engine maps
    auto h = std::make_unique<NullStream>(key, false, ch, groups_[spec.virtual_mic ? "vmic" : "out"], nullptr, src);
    open_.insert(h.get());
    return wrap(std::move(h));
  }

  int restore_journal() override { return 0; }
  std::vector<std::string> features() override { return {"system_capture", "mic_demand", "virtual_mic", "aec"}; }
  std::string permission(const std::string&) override { return "granted"; }

  void set_vmic_in_use(bool v) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      vmic_in_use_ = v;
    }
    if (listener_) listener_->devices_changed();
  }

  bool set_present(const std::string& key, bool v) {
    std::vector<StreamHandle*> lost;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!present_.count(key)) return false;
      present_[key] = v;
      if (!v)
        for (auto* h : open_)
          if (h->device_in_use() == key) lost.push_back(handle_of_[h]);
    }
    if (listener_) {
      for (auto* h : lost) listener_->stream_lost(h, "device gone");
      listener_->devices_changed();
    }
    return true;
  }

 private:
  // Outer handle so the backend can track open streams and their removal.
  class Handle : public StreamHandle {
   public:
    Handle(NullBackend* b, std::unique_ptr<NullStream> s) : b_(b), s_(std::move(s)) {}
    ~Handle() override {
      {
        std::lock_guard<std::mutex> lock(b_->mu_);
        b_->open_.erase(s_.get());
        b_->handle_of_.erase(s_.get());
      }
      s_.reset();
    }
    std::string device_in_use() const override { return s_->device_in_use(); }

   private:
    NullBackend* b_;
    std::unique_ptr<NullStream> s_;
  };
  std::unique_ptr<StreamHandle> wrap(std::unique_ptr<NullStream> s) {
    NullStream* raw = s.get();
    auto h = std::make_unique<Handle>(this, std::move(s));
    handle_of_[raw] = h.get();
    return h;
  }

  std::mutex mu_;
  std::map<std::string, GroupCfg> groups_;
  std::map<std::string, bool> present_;
  std::set<NullStream*> open_;
  std::map<NullStream*, StreamHandle*> handle_of_;
  BackendListener* listener_ = nullptr;
  bool vmic_in_use_ = false;
  std::string vmic_kind_ = "beamer";
};

std::unique_ptr<Backend> make_null_backend(const std::string& options) {
  return std::make_unique<NullBackend>(options);
}

NullBackend* as_null_backend(Backend* b) { return dynamic_cast<NullBackend*>(b); }
void null_backend_set_vmic_in_use(NullBackend* nb, bool v) { nb->set_vmic_in_use(v); }
bool null_backend_set_present(NullBackend* nb, const std::string& key, bool v) { return nb->set_present(key, v); }

}  // namespace beamer::audio
