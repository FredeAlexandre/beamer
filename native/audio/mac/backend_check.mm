// SPDX-License-Identifier: GPL-3.0-only
// backend_check: a test harness for the macOS backend alone (no engine), used for WP3's live
// acceptance checks. Results are JSON lines on stdout; the backend logs on stderr.
//
//   backend_check list
//   backend_check watch [--seconds N]                       devices_changed events + virtual mic state
//   backend_check capture --kind mic|system|reference [--device KEY] [--channels N] [--mute-local]
//                 [--exclude-pid PID]... [--seconds S] [--tone HZ] [--out FILE.f32]
//   backend_check play [--device KEY | --vmic] [--channels N] [--seconds S]
//                 [--tone HZ --level DBFS | --stdin] [--report-io]
//   backend_check selftap [--to vmic|default|KEY] [--tone HZ] [--level DBFS] [--seconds S]
//
// `play --stdin` reads f32le interleaved at 48 kHz (like the prototype's `play`, with backpressure),
// so live_check.py can drive the feed through this backend. `selftap` renders a tone from this
// process and captures the system tap in the same process: the tone must be absent (self-exclusion).
#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "backend_mac.h"
#include "mac_internal.h"

using namespace beamer::audio;

namespace {

struct Args {
  std::vector<std::string> pos;
  std::map<std::string, std::vector<std::string>> opt;
  std::string get(const std::string& k, const std::string& d = "") const {
    auto it = opt.find(k);
    return it == opt.end() || it->second.empty() ? d : it->second.back();
  }
  bool has(const std::string& k) const { return opt.count(k) > 0; }
  double num(const std::string& k, double d) const { return has(k) ? atof(get(k).c_str()) : d; }
};

Args parse(int argc, char** argv) {
  static const char* flags[] = {"mute-local", "vmic", "stdin", "report-io"};
  Args a;
  for (int i = 1; i < argc; ++i) {
    std::string s = argv[i];
    if (s.rfind("--", 0) == 0) {
      std::string k = s.substr(2);
      bool flag = false;
      for (auto* f : flags) flag |= k == f;
      if (flag || i + 1 >= argc) a.opt[k].push_back("1");
      else a.opt[k].push_back(argv[++i]);
    } else {
      a.pos.push_back(s);
    }
  }
  return a;
}

std::string jstr(const std::string& s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\', o += c;
    else if (static_cast<unsigned char>(c) < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += c;
  }
  return o + "\"";
}

double t0 = 0;
std::atomic<bool> g_stop{false};  // SIGINT/SIGTERM: finish the current command cleanly
void on_signal(int) { g_stop.store(true); }
double now_s() { return double(mac::now_ns()) / 1e9; }
double ms_since_start() { return std::round((now_s() - t0) * 10000) / 10; }

std::mutex out_mu;
void emit(const std::string& line) {
  std::lock_guard<std::mutex> lock(out_mu);
  fputs(line.c_str(), stdout);
  fputc('\n', stdout);
  fflush(stdout);
}

double db(double x) { return x > 1e-10 ? std::round(200 * std::log10(x)) / 10 : -200.0; }

const char* form_word(Form f) {
  switch (f) {
    case Form::Microphone: return "microphone";
    case Form::Speakers: return "speakers";
    case Form::Headphones: return "headphones";
    case Form::Headset: return "headset";
    default: return "unknown";
  }
}

const char* role_word(Role r) {
  switch (r) {
    case Role::VirtualMic: return "virtual_mic";
    case Role::VirtualMicFeed: return "virtual_mic_feed";
    default: return "normal";
  }
}

// The §3.3 `devices` event, plus role and manufacturer per device; Role != Normal is not a source
// or output (the engine applies the same rule).
std::string devices_json(const DeviceSnapshot& s) {
  auto list = [&](bool input) {
    std::string o = "[";
    bool first = true;
    for (const auto& d : s.devices) {
      if ((input ? !d.input : !d.output) || d.role != Role::Normal) continue;
      if (!first) o += ",";
      first = false;
      o += "{\"key\":" + jstr(d.key) + ",\"name\":" + jstr(d.name) + ",\"transport\":" + jstr(d.transport) +
           ",\"channels\":" + std::to_string(d.channels) + ",\"form\":\"" + form_word(d.form) + "\"}";
    }
    return o + "]";
  };
  std::string all = "[";
  for (size_t i = 0; i < s.devices.size(); ++i) {
    const auto& d = s.devices[i];
    if (i) all += ",";
    char rate[32];
    snprintf(rate, sizeof rate, "%.0f", d.nominal_rate);
    all += "{\"key\":" + jstr(d.key) + ",\"name\":" + jstr(d.name) + ",\"manufacturer\":" + jstr(d.manufacturer) +
           ",\"dir\":\"" + (d.input ? "in" : "out") + "\",\"role\":\"" + role_word(d.role) + "\",\"rate\":" + rate +
           "}";
  }
  all += "]";
  auto key = [](const std::string& k) { return k.empty() ? std::string("null") : jstr(k); };
  return "{\"ev\":\"devices\",\"inputs\":" + list(true) + ",\"outputs\":" + list(false) +
         ",\"default_input\":" + key(s.default_input) + ",\"default_output\":" + key(s.default_output) +
         ",\"virtual_mic\":{\"kind\":" + jstr(s.vmic_kind) + ",\"present\":" + (s.vmic_present ? "true" : "false") +
         ",\"in_use\":" + (s.vmic_in_use ? "true" : "false") + "},\"all\":" + all + "}";
}

struct Listener : BackendListener {
  Backend* backend = nullptr;
  bool vmic_in_use = false;
  void devices_changed() override {
    DeviceSnapshot s = backend->snapshot();
    char b[160];
    snprintf(b, sizeof b, "{\"ev\":\"devices_changed\",\"t_ms\":%.1f,\"vmic_in_use\":%s,\"devices\":%zu}", ms_since_start(),
             s.vmic_in_use ? "true" : "false", s.devices.size());
    if (s.vmic_in_use != vmic_in_use) {
      vmic_in_use = s.vmic_in_use;
      snprintf(b, sizeof b, "{\"ev\":\"vmic_in_use\",\"t_ms\":%.1f,\"in_use\":%s,\"wall\":%.3f}", ms_since_start(),
               vmic_in_use ? "true" : "false", std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count());
    }
    emit(b);
  }
  void stream_lost(StreamHandle*, const std::string& why) override {
    emit("{\"ev\":\"stream_lost\",\"t_ms\":" + std::to_string(ms_since_start()) + ",\"why\":" + jstr(why) + "}");
  }
};

double cpu_seconds() {
  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
  return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}

// ---- capture: records everything into a preallocated buffer (IO thread: memcpy + atomic store)
struct Recorder : CaptureSink {
  std::vector<float> buf;
  std::atomic<size_t> n{0};
  std::atomic<uint64_t> calls{0};
  int ch = 1;
  Recorder(int channels, double seconds) : buf(size_t((seconds + 2) * kRate) * size_t(channels)), ch(channels) {}
  void on_capture(const float* x, int frames, int channels, uint64_t) override {
    calls.fetch_add(1, std::memory_order_relaxed);
    size_t at = n.load(std::memory_order_relaxed);
    size_t k = std::min(buf.size() - at, size_t(frames) * size_t(channels));
    memcpy(buf.data() + at, x, k * sizeof(float));
    n.store(at + k, std::memory_order_release);
  }
};

// ---- render: a tone, or f32 from stdin through an SPSC ring
struct Tone : RenderSource {
  double hz, amp, phase = 0;
  std::atomic<uint64_t> calls{0}, last_ns{0};
  Tone(double h, double a) : hz(h), amp(a) {}
  void on_render(float* out, int frames, int ch, uint64_t) override {
    calls.fetch_add(1, std::memory_order_relaxed);
    last_ns.store(mac::now_ns(), std::memory_order_relaxed);
    const double step = 2 * M_PI * hz / kRate;
    for (int f = 0; f < frames; ++f) {
      float v = float(amp * std::sin(phase));
      phase += step;
      if (phase > 2 * M_PI) phase -= 2 * M_PI;
      for (int c = 0; c < ch; ++c) out[f * ch + c] = v;
    }
  }
};

struct StdinSource : RenderSource {
  std::vector<float> ring;
  std::atomic<uint64_t> w{0}, r{0};
  std::atomic<uint64_t> calls{0}, last_ns{0}, underruns{0};
  std::atomic<bool> eof{false};
  bool primed = false;
  size_t prefill;
  explicit StdinSource(size_t cap, size_t pre) : ring(cap), prefill(pre) {}
  void on_render(float* out, int frames, int ch, uint64_t) override {
    calls.fetch_add(1, std::memory_order_relaxed);
    last_ns.store(mac::now_ns(), std::memory_order_relaxed);
    const size_t need = size_t(frames) * size_t(ch);
    uint64_t rr = r.load(std::memory_order_relaxed), ww = w.load(std::memory_order_acquire);
    size_t avail = size_t(ww - rr);
    if (!primed && avail >= prefill) primed = true;
    size_t k = primed ? std::min(avail, need) : 0;
    for (size_t i = 0; i < k; ++i) out[i] = ring[(rr + i) % ring.size()];
    for (size_t i = k; i < need; ++i) out[i] = 0;
    if (primed && k < need && !eof.load()) {
      underruns.fetch_add(1, std::memory_order_relaxed);
      primed = false;
    }
    r.store(rr + k, std::memory_order_release);
  }
  void reader() {
    std::vector<float> chunk(4096);
    size_t have = 0;
    for (;;) {
      ssize_t got = read(0, reinterpret_cast<char*>(chunk.data()) + have, chunk.size() * 4 - have);
      if (got <= 0) break;
      have += size_t(got);
      size_t floats = have / 4;
      size_t i = 0;
      while (i < floats) {
        uint64_t ww = w.load(std::memory_order_relaxed), rr = r.load(std::memory_order_acquire);
        size_t space = ring.size() - size_t(ww - rr);
        if (!space) {
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
          continue;
        }
        size_t k = std::min(space, floats - i);
        for (size_t j = 0; j < k; ++j) ring[(ww + j) % ring.size()] = chunk[i + j];
        w.store(ww + k, std::memory_order_release);
        i += k;
      }
      size_t rem = have - floats * 4;
      memmove(chunk.data(), reinterpret_cast<char*>(chunk.data()) + floats * 4, rem);
      have = rem;
    }
    eof = true;
  }
};

struct Levels {
  double rms_db, peak_db, tone_db;
};

// Mono mix of `x` (interleaved, ch channels): RMS, peak, and the tone's amplitude by lock-in
// (dBFS; a full-scale sine is 0 dBFS).
Levels levels(const float* x, size_t frames, int ch, double tone_hz) {
  double ss = 0, peak = 0;
  std::complex<double> acc = 0;
  for (size_t f = 0; f < frames; ++f) {
    double v = 0;
    for (int c = 0; c < ch; ++c) v += x[f * ch + c];
    v /= ch;
    ss += v * v;
    peak = std::max(peak, std::fabs(v));
    if (tone_hz > 0) acc += v * std::polar(1.0, -2 * M_PI * tone_hz * double(f) / kRate);
  }
  double rms = frames ? std::sqrt(ss / double(frames)) : 0;
  double amp = frames ? 2 * std::abs(acc) / double(frames) : 0;
  return {db(rms), db(peak), tone_hz > 0 ? db(amp) : -200};
}

std::string levels_json(const Levels& l) {
  char b[160];
  snprintf(b, sizeof b, "\"rms_dbfs\":%.1f,\"peak_dbfs\":%.1f,\"tone_dbfs\":%.1f", l.rms_db, l.peak_db, l.tone_db);
  return b;
}

void sleep_s(double s) {
  const double end = now_s() + s;
  while (!g_stop.load() && now_s() < end) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

int cmd_list(Backend& b) {
  emit(devices_json(b.snapshot()));
  std::string f = "[";
  auto feats = b.features();
  for (size_t i = 0; i < feats.size(); ++i) f += (i ? "," : "") + jstr(feats[i]);
  emit("{\"ev\":\"backend\",\"features\":" + f + "],\"permission_mic\":" + jstr(b.permission("mic")) +
       ",\"restored\":" + std::to_string(b.restore_journal()) + "}");
  return 0;
}

int cmd_watch(Backend& b, Listener& l, const Args& a) {
  DeviceSnapshot s = b.snapshot();
  l.vmic_in_use = s.vmic_in_use;
  emit("{\"ev\":\"watching\",\"vmic_present\":" + std::string(s.vmic_present ? "true" : "false") +
       ",\"vmic_in_use\":" + (s.vmic_in_use ? "true" : "false") + "}");
  sleep_s(a.num("seconds", 30));
  return 0;
}

int cmd_capture(Backend& b, const Args& a) {
  CaptureSpec spec;
  std::string kind = a.get("kind", "mic");
  spec.kind = kind == "system" ? CaptureKind::System : kind == "reference" ? CaptureKind::Reference : CaptureKind::Mic;
  spec.device = a.get("device");
  spec.channels = int(a.num("channels", spec.kind == CaptureKind::Mic ? 1 : 2));
  spec.mute_local = a.has("mute-local");
  if (a.opt.count("exclude-pid"))
    for (auto& p : a.opt.at("exclude-pid")) spec.exclude_pids.push_back(atoll(p.c_str()));
  const double secs = a.num("seconds", 5), tone = a.num("tone", 0);
  Recorder rec(spec.channels, secs);
  std::string err;
  const double c0 = cpu_seconds(), w0 = now_s();
  auto h = b.open_capture(spec, &rec, &err);
  if (!h) {
    emit("{\"ev\":\"error\",\"msg\":" + jstr(err) + "}");
    return 1;
  }
  const double opened_ms = (now_s() - w0) * 1000;
  emit("{\"ev\":\"capturing\",\"kind\":" + jstr(kind) + ",\"device\":" + jstr(h->device_in_use()) +
       ",\"open_ms\":" + std::to_string(int(opened_ms)) + "}");
  const double start = now_s();
  while (!g_stop.load() && now_s() - start < secs) {
    sleep_s(1.0);
    size_t n = rec.n.load(std::memory_order_acquire) / size_t(spec.channels);
    size_t from = n > size_t(kRate) ? n - size_t(kRate) : 0;
    Levels l = levels(rec.buf.data() + from * spec.channels, n - from, spec.channels, tone);
    char b2[80];
    snprintf(b2, sizeof b2, "{\"ev\":\"level\",\"t_s\":%.1f,", now_s() - start);
    emit(b2 + levels_json(l) + "}");
  }
  const double cpu = cpu_seconds() - c0, wall = now_s() - w0;
  h.reset();
  size_t n = rec.n.load() / size_t(spec.channels);
  Levels l = n > size_t(kRate) / 2 ? levels(rec.buf.data() + size_t(kRate) / 2 * spec.channels, n - size_t(kRate) / 2,
                                             spec.channels, tone)
                                   : levels(rec.buf.data(), n, spec.channels, tone);
  if (a.has("out")) {
    if (FILE* f = fopen(a.get("out").c_str(), "wb")) {
      fwrite(rec.buf.data(), sizeof(float), n * spec.channels, f);
      fclose(f);
    }
  }
  char b2[200];
  snprintf(b2, sizeof b2, "{\"ev\":\"captured\",\"frames\":%zu,\"seconds\":%.2f,\"callbacks\":%llu,\"cpu_pct\":%.2f,",
           n, double(n) / kRate, (unsigned long long)rec.calls.load(), 100 * cpu / wall);
  emit(b2 + levels_json(l) + "}");
  return 0;
}

int cmd_play(Backend& b, const Args& a) {
  RenderSpec spec;
  spec.virtual_mic = a.has("vmic");
  spec.device = a.get("device");
  spec.channels = int(a.num("channels", spec.virtual_mic ? 1 : 2));
  const double secs = a.num("seconds", a.has("stdin") ? 1e9 : 5);
  std::unique_ptr<Tone> tone;
  std::unique_ptr<StdinSource> in;
  RenderSource* src;
  std::atomic<uint64_t>* last_ns;
  if (a.has("stdin")) {
    in = std::make_unique<StdinSource>(size_t(0.08 * kRate) * spec.channels, size_t(0.01 * kRate) * spec.channels);
    src = in.get();
    last_ns = &in->last_ns;
  } else {
    tone = std::make_unique<Tone>(a.num("tone", 1000), std::pow(10.0, a.num("level", -40) / 20));
    src = tone.get();
    last_ns = &tone->last_ns;
  }
  std::string err;
  const double c0 = cpu_seconds(), w0 = now_s();
  auto h = b.open_render(spec, src, &err);
  if (!h) {
    emit("{\"ev\":\"error\",\"msg\":" + jstr(err) + "}");
    return 1;
  }
  emit("{\"ev\":\"playing\",\"device\":" + jstr(h->device_in_use()) + ",\"pid\":" + std::to_string(getpid()) + "}");
  std::thread reader;
  if (in) reader = std::thread([&] { in->reader(); });
  const bool report = a.has("report-io");
  double gap_start = 0;
  const double start = now_s();
  while (!g_stop.load() && now_s() - start < secs) {
    if (in && in->eof && in->w.load() == in->r.load()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!report) continue;
    double last = double(last_ns->load()) / 1e9, t = now_s();
    if (!gap_start && last > 0 && t - last > 0.1) {
      gap_start = last;
      emit("{\"ev\":\"io_stopped\",\"t_ms\":" + std::to_string(int((last - t0) * 1000)) + ",\"wall\":" +
           std::to_string(std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count()) + "}");
    } else if (gap_start && t - last < 0.05) {
      emit("{\"ev\":\"io_resumed\",\"t_ms\":" + std::to_string(int((last - t0) * 1000)) + ",\"gap_ms\":" +
           std::to_string(int((last - gap_start) * 1000)) + ",\"device\":" + jstr(h->device_in_use()) + "}");
      gap_start = 0;
    }
  }
  const double cpu = cpu_seconds() - c0, wall = now_s() - w0;
  h.reset();
  if (reader.joinable()) reader.detach();
  char b2[160];
  snprintf(b2, sizeof b2, "{\"ev\":\"played\",\"seconds\":%.2f,\"cpu_pct\":%.2f,\"underruns\":%llu}", wall,
           100 * cpu / wall, (unsigned long long)(in ? in->underruns.load() : 0));
  emit(b2);
  return 0;
}

int cmd_selftap(Backend& b, const Args& a) {
  const double hz = a.num("tone", 1000), secs = a.num("seconds", 4);
  std::string to = a.get("to", "default");
  RenderSpec rs;
  rs.virtual_mic = to == "vmic";
  rs.device = (to == "vmic" || to == "default") ? "" : to;
  rs.channels = rs.virtual_mic ? 1 : 2;
  Tone tone(hz, std::pow(10.0, a.num("level", -40) / 20));
  CaptureSpec cs;
  cs.kind = CaptureKind::System;
  cs.channels = 2;
  Recorder rec(2, secs);
  std::string err;
  auto cap = b.open_capture(cs, &rec, &err);
  if (!cap) {
    emit("{\"ev\":\"error\",\"msg\":" + jstr(err) + "}");
    return 1;
  }
  auto play = b.open_render(rs, &tone, &err);
  if (!play) {
    emit("{\"ev\":\"error\",\"msg\":" + jstr(err) + "}");
    return 1;
  }
  sleep_s(secs);
  play.reset();
  cap.reset();
  size_t n = rec.n.load() / 2;
  size_t skip = std::min(n, size_t(kRate) / 2);
  Levels l = levels(rec.buf.data() + skip * 2, n - skip, 2, hz);
  emit("{\"ev\":\"selftap\",\"rendered_to\":" + jstr(to) + ",\"tone_hz\":" + std::to_string(int(hz)) +
       ",\"frames\":" + std::to_string(n) + "," + levels_json(l) + "}");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  t0 = now_s();
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  signal(SIGPIPE, SIG_IGN);
  Args a = parse(argc, argv);
  if (a.pos.empty()) {
    fprintf(stderr, "usage: backend_check list|watch|capture|play|selftap [options]\n");
    return 2;
  }
  auto backend = make_mac_backend("");
  Listener listener;
  listener.backend = backend.get();
  backend->set_listener(&listener);
  const std::string& cmd = a.pos[0];
  int rc = 2;
  if (cmd == "list") rc = cmd_list(*backend);
  else if (cmd == "watch") rc = cmd_watch(*backend, listener, a);
  else if (cmd == "capture") rc = cmd_capture(*backend, a);
  else if (cmd == "play") rc = cmd_play(*backend, a);
  else if (cmd == "selftap") rc = cmd_selftap(*backend, a);
  else fprintf(stderr, "unknown command %s\n", cmd.c_str());
  backend->set_listener(nullptr);
  backend.reset();
  return rc;
}
