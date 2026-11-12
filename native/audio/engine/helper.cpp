// SPDX-License-Identifier: GPL-3.0-only
#include "helper.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#include "codec.h"
#include "events.h"
#include "howl.h"
#include "json.h"
#include "null_backend.h"
#include "null_video.h"
#include "streams.h"
#include "util.h"
#include "video_recv.h"
#include "video_send.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <csignal>
#include <signal.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/event.h>
#endif
#endif

namespace beamer::audio {

namespace {

const char* form_name(Form f) {
  switch (f) {
    case Form::Microphone: return "microphone";
    case Form::Speakers: return "speakers";
    case Form::Headphones: return "headphones";
    case Form::Headset: return "headset";
    default: return "unknown";
  }
}

struct CmdError {
  std::string code, msg;
};

bool parse_stream_id(const Json& j, uint32_t* out) {
  int64_t v;
  if (!jget_int(j, "stream", &v) || v <= 0 || v > 0x7FFFFFFF) return false;
  *out = static_cast<uint32_t>(v);
  return true;
}

bool parse_key(const Json& j, const char* field, uint8_t out[kKeyLen]) {
  std::string s;
  std::vector<uint8_t> b;
  if (!jget_str(j, field, &s) || s.size() > 200 || !b64_decode(s, &b) || b.size() != kKeyLen) return false;
  std::memcpy(out, b.data(), kKeyLen);
  secure_wipe(b.data(), b.size());
  return true;
}

bool valid_flow_id(const std::string& s) {
  if (s.size() != 8) return false;
  for (char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
  return true;
}

// optional {"<field>": key|null} -> key ("" = default)
bool parse_device_ref_field(const Json& obj, const char* field, std::string* key) {
  auto it = obj.find(field);
  if (it == obj.end() || it->is_null()) {
    key->clear();
    return true;
  }
  if (!it->is_string() || it->get<std::string>().size() > 200) return false;
  *key = it->get<std::string>();
  return true;
}

// optional {"device": key|null} -> key ("" = default)
bool parse_device_ref(const Json& obj, std::string* key) {
  if (!obj.is_object()) return false;
  auto it = obj.find("device");
  if (it == obj.end() || it->is_null()) {
    key->clear();
    return true;
  }
  if (!it->is_string() || it->get<std::string>().size() > 200) return false;
  *key = it->get<std::string>();
  return true;
}

bool has_word(const std::vector<std::string>& v, const char* w) { return std::find(v.begin(), v.end(), w) != v.end(); }

std::string fmt_str(const VideoFormat& f) {
  return std::to_string(f.w) + "x" + std::to_string(f.h) + "@" + std::to_string(f.fps);
}

class Helper : public BackendListener, public VideoBackendListener {
 public:
  Helper(Backend& backend, const HelperOptions& opt, EventSink* ev) : backend_(backend), opt_(opt), ev_(ev), video_(opt.video) {
    if (video_) video_features_ = video_->features();
  }

  int run(const HelperIo& io) {
    backend_.set_listener(this);
    if (video_) video_->set_listener(this);
    net_.start();
    const int restored = backend_.restore_journal();
    std::vector<std::string> features = engine_features(backend_);
    if (video_) {
      // camera PHASE2 §3.1: video1 (engine has §4) + the backend's words (+ camera_synth with --dev)
      features.push_back("video1");
      for (auto& w : video_features_) features.push_back(w);
      if (opt_.dev && has_word(video_features_, "camera_capture")) features.push_back("camera_synth");
    }
    ev_->emit(Json{{"ev", "ready"}, {"proto", kControlProto}, {"version", kEngineVersion},
                   {"platform", kPlatform}, {"features", features}, {"restored", restored}});
    emit_devices(true);
    emit_permission(true);
    if (video_ && has_word(video_features_, "camera_capture")) emit_cameras();
    if (video_ && has_word(video_features_, "virtual_camera")) vcam_refresh(nullptr);
    last_stats_ns_ = now_ns();

    // stdin reader (may outlive this object when detached: it posts through ReaderState only)
    auto reader_state = std::make_shared<ReaderState>();
    reader_state->read_line = io.read_line;
    reader_state->helper = this;
    std::thread reader([reader_state] {
      set_thread_name("beamer-audio-stdin");
      std::string line;
      bool eof = true;
      while (reader_state->read_line(&line)) {
        std::lock_guard<std::mutex> lock(reader_state->mu);
        if (!reader_state->helper) {
          eof = false;
          break;
        }
        Helper* h = reader_state->helper;
        std::string l = line;
        h->post([h, l] { h->handle_line(l); });
      }
      std::lock_guard<std::mutex> lock(reader_state->mu);
      if (eof && reader_state->helper) {
        Helper* h = reader_state->helper;
        h->post([h] { h->quit_ = true; });
      }
    });
    // 50 ms timer
    std::thread timer([this] {
      set_thread_name("beamer-audio-timer");
      while (!stop_threads_.load()) {
        sleep_ns(50'000'000);
        if (!tick_pending_.exchange(true)) post([this] {
            tick_pending_.store(false);
            tick();
          });
      }
    });
    // parent watch
    std::thread parent;
    if (opt_.parent_pid > 0) parent = std::thread([this] { watch_parent(); });

    while (!quit_) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(qmu_);
        qcv_.wait(lock, [this] { return !queue_.empty(); });
        task = std::move(queue_.front());
        queue_.pop_front();
      }
      task();
    }
    shutdown();
    stop_threads_.store(true);
    {
      std::lock_guard<std::mutex> lock(reader_state->mu);
      reader_state->helper = nullptr;
    }
    timer.join();
    if (parent.joinable()) parent.join();
    if (io.detach_reader) reader.detach();
    else reader.join();
    backend_.set_listener(nullptr);
    if (video_) video_->set_listener(nullptr);
    return 0;
  }

  // BackendListener (any backend thread)
  void devices_changed() override {
    post([this] {
      emit_devices(false);
      emit_permission(false);
    });
  }
  void stream_lost(StreamHandle* h, const std::string& why) override {
    post([this, h, why] { on_stream_lost(h, why); });
  }
  // VideoBackendListener (any backend thread)
  void cameras_changed() override {
    post([this] { emit_cameras(); });
  }
  void vcam_changed() override {
    post([this] { emit_vcam(false); });
  }
  void permission_changed() override {
    post([this] {
      emit_permission(false);
      if (has_word(video_features_, "camera_capture")) emit_cameras();
    });
  }
  void camera_lost(CameraHandle* h, const std::string& why) override {
    // Resolved now, while `h` is alive (backend contract): by the time the helper thread runs, the handle may be
    // freed and its address reused by another stream's handle (R3 #13). The serial tells a reused stream id apart.
    uint32_t id = 0;
    uint64_t serial = 0;
    if (VideoSendStream::camera_owner(h, &id, &serial)) {
      post([this, id, serial, why] {
        auto it = streams_.find(id);
        if (it == streams_.end() || !it->second.vsend || it->second.vsend->serial() != serial) return;  // gone already
        BA_WARN("video stream %u lost its camera: %s", id, why.c_str());
        stop_stream(id, "no_camera");
      });
      return;
    }
    // Not registered yet: lost while open_camera() was still returning it. The stream registers the handle right
    // after, before the helper thread can run this, so the pointer match is still the same handle there.
    post([this, h, why] {
      for (auto& kv : streams_)
        if (kv.second.vsend && kv.second.vsend->owns_camera(h)) {
          BA_WARN("video stream %u lost its camera: %s", kv.first, why.c_str());
          stop_stream(kv.first, "no_camera");
          return;
        }
    });
  }

 private:
  struct ReaderState {
    std::function<bool(std::string*)> read_line;
    std::mutex mu;
    Helper* helper = nullptr;
  };
  struct Entry {
    std::shared_ptr<StreamBase> base;
    std::shared_ptr<SendStream> send;
    std::shared_ptr<RecvStream> recv;
    std::shared_ptr<LoopStream> loop;
    std::string flow;
    std::shared_ptr<VideoSendStream> vsend;
    std::shared_ptr<VideoRecvStream> vrecv;
    bool video() const { return vsend || vrecv; }
  };

  void post(std::function<void()> f) {
    {
      std::lock_guard<std::mutex> lock(qmu_);
      queue_.push_back(std::move(f));
    }
    qcv_.notify_one();
  }

  void ok(const Json& req) {
    if (!req.is_null()) ev_->emit(Json{{"ev", "ok"}, {"req", req}});
  }
  void error(const Json& req, const std::string& code, const std::string& msg) {
    Json e{{"ev", "error"}, {"code", code}, {"msg", msg}};
    if (!req.is_null()) e["req"] = req;
    ev_->emit(e);
    BA_DEBUG("error %s: %s", code.c_str(), msg.c_str());
  }

  void handle_line(const std::string& line) {
    if (line.empty() || line == "\r") return;
    Json j = Json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
      error(nullptr, "bad_args", "not a JSON object");
      return;
    }
    Json req = nullptr;
    auto r = j.find("req");
    if (r != j.end() && r->is_number_integer()) req = *r;
    std::string cmd;
    if (!jget_str(j, "cmd", &cmd)) {
      error(req, "bad_args", "missing cmd");
      return;
    }
    try {
      dispatch(cmd, j, req);
    } catch (const CmdError& e) {
      error(req, e.code, e.msg);
    } catch (const std::exception& e) {
      error(req, "internal", e.what());
    }
  }

  void dispatch(const std::string& cmd, const Json& j, const Json& req) {
    if (cmd == "devices") {
      emit_devices(true);
      ok(req);
    } else if (cmd == "flow_open") {
      cmd_flow_open(j, req);
    } else if (cmd == "flow_remote") {
      cmd_flow_remote(j, req);
    } else if (cmd == "flow_close") {
      cmd_flow_close(j, req);
    } else if (cmd == "link_in") {
      cmd_link_in(j, req);
    } else if (cmd == "send") {
      cmd_send(j, req);
    } else if (cmd == "recv") {
      cmd_recv(j, req);
    } else if (cmd == "loop") {
      cmd_loop(j, req);
    } else if (cmd == "stop") {
      uint32_t id;
      if (!parse_stream_id(j, &id)) throw CmdError{"bad_args", "stream"};
      if (!streams_.count(id)) throw CmdError{"unknown_stream", std::to_string(id)};
      stop_stream(id, "");
      ok(req);
    } else if (cmd == "meter") {
      bool on;
      if (!jget_bool(j, "on", &on)) throw CmdError{"bad_args", "on"};
      int64_t p = 100;
      if (j.contains("period_ms") && (!jget_int(j, "period_ms", &p) || p < 50 || p > 500))
        throw CmdError{"bad_args", "period_ms must be 50..500"};
      meter_on_ = on;
      meter_period_ns_ = p * 1'000'000;
      next_levels_ns_ = now_ns();
      ok(req);
    } else if (cmd == "test_input") {
      cmd_test_input(j, req);
    } else if (cmd == "ping") {
      ev_->emit(Json{{"ev", "pong"}, {"req", req}});
    } else if (cmd == "quit") {
      ok(req);
      quit_ = true;
    } else if (cmd == "dev" && opt_.dev) {
      cmd_dev(j, req);
    } else if (cmd == "video_config") {
      cmd_video_config(j, req);
    } else if (cmd == "video_warm") {
      cmd_video_warm(j, req);
    } else if (cmd == "video_link") {
      cmd_video_link(j, req);
    } else if (cmd == "cameras") {
      need_video();
      emit_cameras();
      ok(req);
    } else if (cmd == "permission_request") {
      std::string what;
      if (!jget_str(j, "what", &what) || what != "camera") throw CmdError{"bad_args", "what: camera"};
      need_video();
      if (!has_word(video_features_, "camera_capture")) throw CmdError{"unsupported", "no camera capture here"};
      ok(req);
      video_->request_permission();  // the answer comes as a `permission` event
      next_cam_perm_ns_ = now_ns();
    } else if (cmd == "vcam") {
      cmd_vcam(j, req);
    } else {
      throw CmdError{"unknown_cmd", cmd};
    }
  }

  std::shared_ptr<Flow> flow_of(const Json& j) {
    std::string id;
    if (!jget_str(j, "flow", &id)) throw CmdError{"bad_args", "flow"};
    auto it = flows_.find(id);
    if (it == flows_.end()) throw CmdError{"unknown_flow", id};
    return it->second;
  }

  void cmd_flow_open(const Json& j, const Json& req) {
    std::string id, peer, role;
    if (!jget_str(j, "flow", &id) || !valid_flow_id(id)) throw CmdError{"bad_args", "flow: 8 hex chars"};
    if (flows_.count(id)) throw CmdError{"bad_args", "flow " + id + " is already open"};
    jget_str(j, "peer", &peer);
    if (!jget_str(j, "role", &role) || (role != "offer" && role != "answer"))
      throw CmdError{"bad_args", "role: offer|answer"};
    int64_t port = 0, rr = 10;
    if (j.contains("port") && !j["port"].is_null() && (!jget_int(j, "port", &port) || port < 0 || port > 65535))
      throw CmdError{"bad_args", "port"};
    if (j.contains("rr_ms") && (!jget_int(j, "rr_ms", &rr) || rr < 0 || rr > 1000)) throw CmdError{"bad_args", "rr_ms"};
    uint8_t key[kKeyLen];
    if (!parse_key(j, "key", key)) throw CmdError{"bad_args", "key: base64 of 32 bytes"};
    std::string rhost;
    int64_t rport = 0;
    auto rit = j.find("remote");
    if (rit != j.end() && !rit->is_null()) {
      if (!rit->is_object() || !jget_str(*rit, "host", &rhost)) throw CmdError{"bad_args", "remote"};
      if (rit->contains("port") && (!jget_int(*rit, "port", &rport) || rport < 0 || rport > 65535))
        throw CmdError{"bad_args", "remote.port"};
    }
    std::string host;  // the peer's IP (from the link); preferred over remote.host for the socket
    if (j.contains("host") && !j["host"].is_null() && (!jget_str(j, "host", &host) || host.size() > 255))
      throw CmdError{"bad_args", "host"};
    if (host.empty()) host = rhost;
    bool udp = true;
    jget_bool(j, "udp", &udp);  // extension: false = link-only flow (loopback / no host)
    auto f = std::make_shared<Flow>(id, peer, role == "offer", key, static_cast<int>(rr), ev_);
    secure_wipe(key, sizeof key);
    f->timing = opt_.timing;
    if (udp) {
      std::string err;
      if (!f->open_socket(host, static_cast<int>(port), &err)) throw CmdError{"bind_failed", err};
    } else {
      f->set_link_only();
    }
    ev_->emit(Json{{"ev", "flow_bound"}, {"flow", id}, {"port", f->port()}});
    if (udp && !rhost.empty() && rport > 0) {
      std::string err;
      if (!f->set_remote(rhost, static_cast<int>(rport), &err)) {
        f->close_now();
        throw CmdError{"bad_args", err};
      }
    }
    f->start();
    flows_[id] = f;
    net_.add(f);
    ok(req);
  }

  void cmd_flow_remote(const Json& j, const Json& req) {
    auto f = flow_of(j);
    std::string host, err;
    int64_t port;
    if (!jget_str(j, "host", &host) || !jget_int(j, "port", &port)) throw CmdError{"bad_args", "host/port"};
    if (!f->set_remote(host, static_cast<int>(port), &err)) throw CmdError{"bad_args", err};
    ok(req);
  }

  void cmd_flow_close(const Json& j, const Json& req) {
    auto f = flow_of(j);
    std::vector<uint32_t> ids;
    for (auto& kv : streams_)
      if (kv.second.flow == f->id()) ids.push_back(kv.first);
    for (uint32_t id : ids) stop_stream(id, "");
    net_.close(f);
    flows_.erase(f->id());
    ev_->emit(Json{{"ev", "flow_state"}, {"flow", f->id()}, {"state", "closed"}, {"rtt_ms", nullptr}});
    ok(req);
  }

  void cmd_link_in(const Json& j, const Json& req) {
    auto f = flow_of(j);
    auto it = j.find("p");
    if (it == j.end() || !it->is_array() || it->size() > 64) throw CmdError{"bad_args", "p"};
    std::vector<std::vector<uint8_t>> pk;
    for (auto& e : *it) {
      std::vector<uint8_t> b;
      if (!e.is_string() || !b64_decode(e.get<std::string>(), &b) || b.size() > kMaxPacket)
        throw CmdError{"bad_args", "p: base64 packets"};
      pk.push_back(std::move(b));
    }
    f->link_in(std::move(pk));
    ok(req);
  }

  void check_new_stream(uint32_t id) {
    if (streams_.count(id)) throw CmdError{"bad_args", "stream " + std::to_string(id) + " exists"};
  }

  CodecConfig parse_codec(const Json& j, const std::string& kind) {
    CodecConfig c = kind == "system" ? CodecConfig::system_default() : CodecConfig::mic_default();
    auto it = j.find("codec");
    if (it != j.end() && !it->is_null()) {
      std::string err;
      if (!CodecConfig::parse(*it, &c, &err)) throw CmdError{err.rfind("unsupported", 0) == 0 ? "unsupported" : "bad_args", err};
    }
    return c;
  }

  void need_video() {
    if (!video_) throw CmdError{"unsupported", "no video backend"};
  }

  // codec object of a video send/recv (§4.3)
  void parse_video_codec(const Json& j, int* w, int* h, int* fps, int* bitrate, bool* ltr) {
    *w = 1280;
    *h = 720;
    *fps = 30;
    *bitrate = 2'500'000;
    *ltr = true;
    auto it = j.find("codec");
    if (it == j.end() || it->is_null()) return;
    if (!it->is_object()) throw CmdError{"bad_args", "codec"};
    std::string name;
    if (jget_str(*it, "name", &name) && name != "h264") throw CmdError{"unsupported", "codec " + name};
    int64_t v;
    if (jget_int(*it, "w", &v)) {
      if (v < 2 || v > 4096 || (v & 1)) throw CmdError{"bad_args", "codec.w"};
      *w = int(v);
    }
    if (jget_int(*it, "h", &v)) {
      if (v < 2 || v > 4096 || (v & 1)) throw CmdError{"bad_args", "codec.h"};
      *h = int(v);
    }
    if (jget_int(*it, "fps", &v)) {
      if (v < 1 || v > 60) throw CmdError{"bad_args", "codec.fps"};
      *fps = int(v);
    }
    if (jget_int(*it, "bitrate", &v)) {
      if (v < 50'000 || v > 50'000'000) throw CmdError{"bad_args", "codec.bitrate"};
      *bitrate = int(v);
    }
    if (jget_int(*it, "mtu", &v) && (v < 600 || v > 1500)) throw CmdError{"bad_args", "codec.mtu"};
    jget_bool(*it, "ltr", ltr);
  }

  void cmd_send_video(const Json& j, const Json& req) {
    need_video();
    VideoSendStream::Config cfg;
    if (!parse_stream_id(j, &cfg.id)) throw CmdError{"bad_args", "stream"};
    check_new_stream(cfg.id);
    auto f = flow_of(j);
    if (!parse_key(j, "key", cfg.key)) throw CmdError{"bad_args", "key: base64 of 32 bytes"};
    int w, h, fps, br;
    bool ltr;
    parse_video_codec(j, &w, &h, &fps, &br, &ltr);
    cfg.spec.fmt = VideoFormat{w, h, fps};
    cfg.spec.bitrate = br;
    cfg.spec.ltr = ltr;
    cfg.fps = fps;
    auto src = j.find("source");
    if (src != j.end() && src->is_object()) {
      std::string test;
      if (jget_str(*src, "test", &test)) {
        if (!opt_.dev) throw CmdError{"bad_args", "source.test needs --dev"};
        if (test.rfind("synth", 0) != 0 || test.size() > 64) throw CmdError{"bad_args", "source.test: synth[:WxH][;start_ms=N]"};
        cfg.spec.test = test;
      } else if (!parse_device_ref_field(*src, "camera", &cfg.spec.device)) {
        throw CmdError{"bad_args", "source.camera"};
      }
    } else if (src != j.end() && !src->is_null()) {
      throw CmdError{"bad_args", "source"};
    }
    auto lk = j.find("link");
    if (lk != j.end() && lk->is_object()) {
      int64_t v;
      if (jget_int(*lk, "bitrate", &v) && v >= 50'000 && v <= 50'000'000) cfg.link_bitrate = int(v);
      if (jget_int(*lk, "fps", &v) && v >= 1 && v <= 60) cfg.link_fps = int(v);
    }
    cfg.idr_pace_x = idr_pace_x_;
    if (opt_.dev) {
      int64_t v;  // tests: the camera-on guards' timings
      if (jget_int(j, "dev_no_pli_ms", &v) && v > 0) cfg.no_pli_ns = v * 1'000'000;
      if (jget_int(j, "dev_no_feedback_ms", &v) && v > 0) cfg.no_feedback_ns = v * 1'000'000;
    }
    auto s = std::make_shared<VideoSendStream>(cfg, f, ev_, [this](uint32_t id, const std::string& code) {
      post([this, id, code] {
        auto it = streams_.find(id);
        if (it != streams_.end() && it->second.vsend) stop_stream(id, code);
      });
    });
    f->attach_video_send(s);
    std::string code, msg;
    if (!s->start(*video_, &code, &msg)) {
      f->detach(cfg.id);
      s->stop();
      ev_->emit(Json{{"ev", "stream_state"}, {"stream", cfg.id}, {"state", "failed"}, {"device", nullptr}, {"error", code}});
      throw CmdError{code, msg};
    }
    Entry e;
    e.base = s;
    e.vsend = s;
    e.flow = f->id();
    streams_[cfg.id] = e;
    ok(req);
    const std::string dev = s->device_in_use();
    ev_->emit(Json{{"ev", "stream_state"}, {"stream", cfg.id}, {"state", "starting"},
                   {"device", dev.empty() ? Json(nullptr) : Json(dev)}});
  }

  void cmd_recv_video(const Json& j, const Json& req) {
    need_video();
    VideoRecvStream::Config cfg;
    if (!parse_stream_id(j, &cfg.id)) throw CmdError{"bad_args", "stream"};
    check_new_stream(cfg.id);
    auto f = flow_of(j);
    if (!parse_key(j, "key", cfg.key)) throw CmdError{"bad_args", "key: base64 of 32 bytes"};
    int br;
    bool ltr;
    parse_video_codec(j, &cfg.w, &cfg.h, &cfg.fps, &br, &ltr);
    auto sink = j.find("sink");
    bool vc = false;
    if (sink == j.end() || !sink->is_object() || !jget_bool(*sink, "virtual_camera", &vc) || !vc)
      throw CmdError{"bad_args", "sink must be {\"virtual_camera\": true}"};
    cfg.floor_ms = playout_floor_ms_;
    auto s = std::make_shared<VideoRecvStream>(cfg, f, ev_, [this](uint32_t id, const std::string& code, const std::string& detail) {
      post([this, id, code, detail] {
        auto it = streams_.find(id);
        if (it != streams_.end() && it->second.vrecv) stop_stream(id, code, detail);
      });
    }, nullptr);  // each video recv stream has its own lip-sync hold (§4.9), driven by lipsync_tick()
    s->sync()->set_override_ms(sync_override_ms_);
    std::string code, msg;
    if (!s->start(*video_, &code, &msg)) {
      s->stop();
      ev_->emit(Json{{"ev", "stream_state"}, {"stream", cfg.id}, {"state", "failed"}, {"device", nullptr}, {"error", code}});
      throw CmdError{code, msg};
    }
    f->attach_video_recv(s);
    Entry e;
    e.base = s;
    e.vrecv = s;
    e.flow = f->id();
    streams_[cfg.id] = e;
    ok(req);
    ev_->emit(Json{{"ev", "stream_state"}, {"stream", cfg.id}, {"state", "running"}, {"device", "vcam"}});
  }

  std::shared_ptr<VideoSendStream> vsend_of(const Json& j) {
    uint32_t id;
    if (!parse_stream_id(j, &id)) throw CmdError{"bad_args", "stream"};
    auto it = streams_.find(id);
    if (it == streams_.end()) throw CmdError{"unknown_stream", std::to_string(id)};
    if (!it->second.vsend) throw CmdError{"bad_args", "not a video send stream"};
    return it->second.vsend;
  }

  void cmd_video_config(const Json& j, const Json& req) {
    auto s = vsend_of(j);
    int64_t w = 0, h = 0, fps = 0, br = 0;
    if (j.contains("w") && (!jget_int(j, "w", &w) || w < 2 || w > 4096 || (w & 1))) throw CmdError{"bad_args", "w"};
    if (j.contains("h") && (!jget_int(j, "h", &h) || h < 2 || h > 4096 || (h & 1))) throw CmdError{"bad_args", "h"};
    if (j.contains("fps") && (!jget_int(j, "fps", &fps) || fps < 1 || fps > 60)) throw CmdError{"bad_args", "fps"};
    if (j.contains("bitrate") && (!jget_int(j, "bitrate", &br) || br < 50'000 || br > 50'000'000))
      throw CmdError{"bad_args", "bitrate"};
    s->configure(int(w), int(h), int(fps), int(br));
    ok(req);
  }

  void cmd_video_warm(const Json& j, const Json& req) {
    need_video();
    EncoderStandby sb;
    if (!jget_bool(j, "on", &sb.on)) throw CmdError{"bad_args", "on"};
    int64_t v;
    if (jget_int(j, "w", &v)) sb.fmt.w = int(v);
    if (jget_int(j, "h", &v)) sb.fmt.h = int(v);
    if (jget_int(j, "bitrate", &v)) sb.bitrate = int(v);
    if (sb.on && (sb.fmt.w < 2 || sb.fmt.h < 2 || sb.fmt.w > 4096 || sb.fmt.h > 4096)) throw CmdError{"bad_args", "w/h"};
    video_->set_encoder_standby(sb);
    ok(req);
  }

  void cmd_video_link(const Json& j, const Json& req) {
    auto s = vsend_of(j);
    bool paused = false, have_p = jget_bool(j, "paused", &paused);
    std::vector<uint32_t> dropped;
    auto d = j.find("dropped");
    if (d != j.end()) {
      if (!d->is_array() || d->size() > 1024) throw CmdError{"bad_args", "dropped"};
      for (auto& x : *d) {
        if (!x.is_number_unsigned()) throw CmdError{"bad_args", "dropped: frame numbers"};
        dropped.push_back(x.get<uint32_t>());
      }
    }
    s->link_control(have_p ? &paused : nullptr, dropped);
    ok(req);
  }

  void cmd_vcam(const Json& j, const Json& req) {
    need_video();
    if (!has_word(video_features_, "virtual_camera")) throw CmdError{"unsupported", "no virtual camera here"};
    auto ph = j.find("placeholders");
    std::string idle = vcam_idle_;
    jget_str(j, "idle", &idle);
    if (ph != j.end() && !ph->is_null()) {
      if (!ph->is_object() || ph->size() > 16) throw CmdError{"bad_args", "placeholders"};
      std::map<std::string, Placeholder> texts;
      for (auto it = ph->begin(); it != ph->end(); ++it) {
        if (!it.value().is_object()) throw CmdError{"bad_args", "placeholders." + it.key()};
        Placeholder p;
        jget_str(it.value(), "title", &p.title);
        jget_str(it.value(), "line", &p.line);
        if (p.title.size() > 256 || p.line.size() > 256) throw CmdError{"bad_args", "placeholder text too long"};
        texts[it.key()] = p;
      }
      vcam_texts_ = texts;
    }
    if (idle.size() > 32) throw CmdError{"bad_args", "idle"};
    if ((ph != j.end() && !ph->is_null()) || idle != vcam_idle_) {
      vcam_idle_ = idle;
      video_->vcam_placeholders(vcam_texts_, vcam_idle_);
    }
    bool enable;
    const bool have_enable = jget_bool(j, "enable", &enable);
    vcam_refresh(have_enable ? &enable : nullptr);
    ok(req);
  }

  // Every `vcam` command (and `ready`) re-reads the install state and, when it is ok and the camera is not
  // present, creates/Starts it (async in the backend); always answers with a fresh `vcam` event (§3.2, §4.6.5).
  void vcam_refresh(const bool* enable) {
    VcamState v = video_->vcam();
    std::string err;
    if (enable && !*enable) {
      if (!video_->vcam_enable(false, &err)) BA_WARN("vcam disable: %s", err.c_str());
    } else if (v.install == "ok" && !v.present && !v.creating) {
      if (!video_->vcam_enable(true, &err)) BA_WARN("vcam enable: %s", err.c_str());
    }
    emit_vcam(true);
  }

  void emit_vcam(bool force) {
    if (!video_ || !has_word(video_features_, "virtual_camera")) return;
    VcamState v = video_->vcam();
    Json ev{{"ev", "vcam"}, {"install", v.install}, {"dll_version", v.dll_version}, {"loaded_version", v.loaded_version},
            {"bundled_version", v.bundled_version}, {"present", v.present}, {"creating", v.creating}, {"in_use", v.in_use},
            {"app", v.app.empty() ? Json(nullptr) : Json(v.app)},
            {"want", v.in_use && v.want.w > 0 ? Json{{"w", v.want.w}, {"h", v.want.h}, {"fps", v.want.fps}} : Json(nullptr)},
            {"error", v.error.empty() ? Json(nullptr) : Json(v.error)}};
    std::string text = ev.dump();
    if (force || text != last_vcam_) {
      last_vcam_ = text;
      ev_->emit(ev);
    }
  }

  void emit_cameras() {
    if (!video_) return;
    CameraSnapshot c = video_->cameras();
    Json cams = Json::array();
    for (size_t i = 0; i < c.cameras.size() && i < 16; ++i) {
      auto& x = c.cameras[i];
      Json nat = Json::array();
      for (size_t k = 0; k < x.native.size() && k < 16; ++k) nat.push_back(fmt_str(x.native[k]));
      cams.push_back(Json{{"key", x.key}, {"name", x.name}, {"transport", x.transport}, {"native", nat}, {"preferred", x.preferred}});
    }
    ev_->emit(Json{{"ev", "cameras"}, {"cameras", cams}, {"preferred", c.preferred.empty() ? Json(nullptr) : Json(c.preferred)},
                   {"permission", c.permission.empty() ? "unknown" : c.permission}});
  }

  void cmd_send(const Json& j, const Json& req) {
    std::string kind;
    if (jget_str(j, "kind", &kind) && kind == "video") {
      cmd_send_video(j, req);
      return;
    }
    SendStream::Config cfg;
    if (!parse_stream_id(j, &cfg.id)) throw CmdError{"bad_args", "stream"};
    check_new_stream(cfg.id);
    auto f = flow_of(j);
    if (!jget_str(j, "kind", &cfg.kind) || (cfg.kind != "mic" && cfg.kind != "system"))
      throw CmdError{"bad_args", "kind: mic|system"};
    cfg.codec = parse_codec(j, cfg.kind);
    if (!parse_key(j, "key", cfg.key)) throw CmdError{"bad_args", "key: base64 of 32 bytes"};
    auto src = j.find("source");
    if (src != j.end() && src->is_object()) {
      std::string test;
      if (jget_str(*src, "test", &test)) {
        if (!opt_.dev) throw CmdError{"bad_args", "source.test needs --dev"};
        cfg.test_signal = test;
      } else if (cfg.kind == "mic" && !parse_device_ref(*src, &cfg.device)) {
        throw CmdError{"bad_args", "source.device"};
      }
    } else if (src != j.end() && !src->is_null()) {
      throw CmdError{"bad_args", "source"};
    }
    jget_bool(j, "mute_local", &cfg.mute_local);
    jget_bool(j, "sr", &cfg.sr);  // camera §4.2: (ts, capture time) pairs into the flow's time map
    parse_aec(j, &cfg);
    auto s = std::make_shared<SendStream>(cfg, f);
    std::string code, msg;
    f->attach_send(s);
    if (!s->start(backend_, opt_.exclude_pids, &code, &msg)) {
      f->detach(cfg.id);
      s->stop();
      ev_->emit(Json{{"ev", "stream_state"}, {"stream", cfg.id}, {"state", "failed"}, {"device", nullptr}, {"error", code}});
      throw CmdError{code, msg};
    }
    s->zero_since_ns = now_ns();
    streams_[cfg.id] = Entry{s, s, nullptr, nullptr, f->id(), nullptr, nullptr};
    ok(req);
    emit_running(*s);
  }

  // `aec`: {"reference": key|null} (null = the default output) or null/absent = off. Mic only (a
  // system stream ignores it). With --dev also "lead_ms", "max_wait_ms" and "exclude_pids" (processes
  // left out of the reference: the live test plays its near-end talker through the same speakers).
  void parse_aec(const Json& j, SendStream::Config* cfg) {
    auto a = j.find("aec");
    if (a == j.end() || a->is_null()) return;
    if (!a->is_object()) throw CmdError{"bad_args", "aec: object or null"};
    if (cfg->kind != "mic") return;
    cfg->aec = true;
    if (!parse_device_ref_field(*a, "reference", &cfg->aec_opt.reference)) throw CmdError{"bad_args", "aec.reference"};
    if (!opt_.dev) return;
    double v;
    if (jget_num(*a, "lead_ms", &v)) {
      if (v < -40 || v > 80) throw CmdError{"bad_args", "aec.lead_ms: -40..80"};
      cfg->aec_opt.lead_ms = v;
    }
    if (jget_num(*a, "max_wait_ms", &v)) {
      if (v < 0 || v > 200) throw CmdError{"bad_args", "aec.max_wait_ms: 0..200"};
      cfg->aec_opt.max_wait_ms = v;
    }
    auto p = a->find("exclude_pids");
    if (p != a->end() && p->is_array())
      for (auto& x : *p)
        if (x.is_number_integer()) cfg->aec_opt.exclude_pids.push_back(x.get<int64_t>());
  }

  void cmd_recv(const Json& j, const Json& req) {
    std::string kind;
    if (jget_str(j, "kind", &kind) && kind == "video") {
      cmd_recv_video(j, req);
      return;
    }
    RecvStream::Config cfg;
    if (!parse_stream_id(j, &cfg.id)) throw CmdError{"bad_args", "stream"};
    check_new_stream(cfg.id);
    auto f = flow_of(j);
    if (!jget_str(j, "kind", &cfg.kind) || (cfg.kind != "mic" && cfg.kind != "system"))
      throw CmdError{"bad_args", "kind: mic|system"};
    cfg.codec = parse_codec(j, cfg.kind);
    if (!parse_key(j, "key", cfg.key)) throw CmdError{"bad_args", "key: base64 of 32 bytes"};
    auto sink = j.find("sink");
    if (sink == j.end() || !sink->is_object()) throw CmdError{"bad_args", "sink"};
    bool vm = false;
    if (jget_bool(*sink, "virtual_mic", &vm) && vm) cfg.virtual_mic = true;
    else if (!parse_device_ref(*sink, &cfg.device)) throw CmdError{"bad_args", "sink.device"};
    auto s = std::make_shared<RecvStream>(cfg, f);
    std::string code, msg;
    if (!s->start(backend_, &code, &msg)) {
      s->stop();
      ev_->emit(Json{{"ev", "stream_state"}, {"stream", cfg.id}, {"state", "failed"}, {"device", nullptr}, {"error", code}});
      throw CmdError{code, msg};
    }
    f->attach_recv(s);
    streams_[cfg.id] = Entry{s, nullptr, s, nullptr, f->id(), nullptr, nullptr};
    ok(req);
    emit_running(*s);
  }

  // No echo cancellation on the loop (§6.4 rule 1: apps on this machine cancel their own playback);
  // an `aec` field is ignored.
  void cmd_loop(const Json& j, const Json& req) {
    uint32_t id;
    if (!parse_stream_id(j, &id)) throw CmdError{"bad_args", "stream"};
    check_new_stream(id);
    std::string device;
    auto src = j.find("source");
    if (src != j.end() && !src->is_null() && !parse_device_ref(*src, &device)) throw CmdError{"bad_args", "source"};
    auto sink = j.find("sink");
    bool vm = false;
    if (sink == j.end() || !sink->is_object() || !jget_bool(*sink, "virtual_mic", &vm) || !vm)
      throw CmdError{"bad_args", "sink must be {\"virtual_mic\": true}"};
    auto s = std::make_shared<LoopStream>(id, device);
    std::string code, msg;
    if (!s->start(backend_, opt_.exclude_pids, &code, &msg)) {
      ev_->emit(Json{{"ev", "stream_state"}, {"stream", id}, {"state", "failed"}, {"device", nullptr}, {"error", code}});
      throw CmdError{code, msg};
    }
    s->zero_since_ns = now_ns();
    streams_[id] = Entry{s, nullptr, nullptr, s, "", nullptr, nullptr};
    ok(req);
    emit_running(*s);
  }

  void cmd_test_input(const Json& j, const Json& req) {
    std::string device;
    int64_t secs = 10;
    if (!jget_str(j, "device", &device) || device.empty() || device.size() > 200) throw CmdError{"bad_args", "device"};
    if (j.contains("seconds") && (!jget_int(j, "seconds", &secs) || secs < 1 || secs > 15))
      throw CmdError{"bad_args", "seconds: 1..15"};
    const int64_t until = now_ns() + secs * 1'000'000'000;
    for (auto& t : tests_)
      if (t->device() == device) {
        t->until_ns = until;
        ok(req);
        return;
      }
    auto t = std::make_unique<TestInput>(device, until);
    std::string code, msg;
    if (!t->start(backend_, &code, &msg)) throw CmdError{code, msg};
    tests_.push_back(std::move(t));
    ok(req);
  }

  void cmd_dev(const Json& j, const Json& req) {
    std::vector<std::shared_ptr<Flow>> targets;
    std::string fid;
    if (jget_str(j, "flow", &fid)) targets.push_back(flow_of(j));
    else
      for (auto& kv : flows_) targets.push_back(kv.second);
    bool b;
    double d;
    if (jget_bool(j, "drop_udp", &b))
      for (auto& f : targets) f->dev_drop_udp.store(b);
    if (jget_num(j, "loss_pct", &d)) {
      if (d < 0 || d > 100) throw CmdError{"bad_args", "loss_pct"};
      for (auto& f : targets) f->dev_loss.store(d / 100.0);
    }
    if (jget_bool(j, "reset_stats", &b) && b)
      for (auto& kv : streams_)
        if (kv.second.recv) kv.second.recv->reset_run_stats();
    // ---- video (camera PHASE2 §3.2)
    auto vl = j.find("video_loss");
    if (vl != j.end() && vl->is_object()) {
      double pct = 0, burst = 0;
      jget_num(*vl, "pct", &pct);
      jget_num(*vl, "burst", &burst);
      if (pct < 0 || pct > 100 || burst < 0 || burst > 1000) throw CmdError{"bad_args", "video_loss"};
      for (auto& f : targets) {
        f->dev_video_loss.store(pct / 100.0);
        f->dev_video_burst.store(burst);
      }
    }
    int64_t iv;
    if (jget_int(j, "video_drop_ms", &iv)) {
      if (iv < 0 || iv > 60000) throw CmdError{"bad_args", "video_drop_ms"};
      for (auto& f : targets) f->dev_video_drop_until.store(now_ns() + iv * 1'000'000);
    }
    if (jget_int(j, "video_drop_pk", &iv)) {
      if (iv < 0 || iv > 100000) throw CmdError{"bad_args", "video_drop_pk"};
      for (auto& f : targets) f->dev_video_drop_pk.store(iv);
    }
    if (jget_int(j, "video_drop_au", &iv)) {
      if (iv < 0 || iv > 1000) throw CmdError{"bad_args", "video_drop_au"};
      for (auto& kv : streams_)
        if (kv.second.vsend && (fid.empty() || kv.second.flow == fid)) kv.second.vsend->dev_drop_au(int(iv));
    }
    if (jget_int(j, "video_stall_ms", &iv)) {  // tests: stall the sender thread (forced queue backlog)
      if (iv < 0 || iv > 10000) throw CmdError{"bad_args", "video_stall_ms"};
      for (auto& kv : streams_)
        if (kv.second.vsend && (fid.empty() || kv.second.flow == fid)) kv.second.vsend->dev_stall_ms(int(iv));
    }
    if (jget_int(j, "video_stall_nopause_ms", &iv)) {  // tests: the same without backpressure (hard cap)
      if (iv < 0 || iv > 10000) throw CmdError{"bad_args", "video_stall_nopause_ms"};
      for (auto& kv : streams_)
        if (kv.second.vsend && (fid.empty() || kv.second.flow == fid)) kv.second.vsend->dev_stall_ms(int(iv), false);
    }
    if (jget_bool(j, "log_time_map", &b))
      for (auto& f : targets) f->dev_log_time_map.store(b);
    if (jget_bool(j, "vcam_kill_beat", &b))  // the presenter stops calling the sink: no heartbeat
      for (auto& kv : streams_)
        if (kv.second.vrecv) kv.second.vrecv->set_frozen(b);
    if (jget_num(j, "idr_pace_x", &d)) {
      if (d < 2.0 || d > 6.0) throw CmdError{"bad_args", "idr_pace_x: 2.0..6.0"};
      idr_pace_x_ = d;
      for (auto& kv : streams_)
        if (kv.second.vsend) kv.second.vsend->set_idr_pace_x(d);
    }
    if (jget_num(j, "playout_floor_ms", &d)) {
      if (d < 20 || d > 150) throw CmdError{"bad_args", "playout_floor_ms: 20..150"};
      playout_floor_ms_ = d;
      for (auto& kv : streams_)
        if (kv.second.vrecv) kv.second.vrecv->set_floor_ms(d);
    }
    // lip sync (§4.9): `sync_offset_ms` n forces the hold of every video recv stream (0..150; null = the policy);
    // `sync_audio_path_ms` n replaces the backend's virtual-mic render path latency (0..300; null = the backend's)
    auto so = j.find("sync_offset_ms");
    if (so != j.end()) {
      if (so->is_null()) {
        sync_override_ms_ = -1;
      } else if (!jget_num(j, "sync_offset_ms", &d) || d < 0 || d > 150) {
        throw CmdError{"bad_args", "sync_offset_ms: 0..150 or null"};
      } else {
        sync_override_ms_ = d;
      }
      for (auto& kv : streams_)
        if (kv.second.vrecv) kv.second.vrecv->sync()->set_override_ms(sync_override_ms_);
    }
    auto sp = j.find("sync_audio_path_ms");
    if (sp != j.end()) {
      if (sp->is_null()) sync_path_override_ms_ = -1;
      else if (!jget_num(j, "sync_audio_path_ms", &d) || d < 0 || d > 300)
        throw CmdError{"bad_args", "sync_audio_path_ms: 0..300 or null"};
      else sync_path_override_ms_ = d;
    }
    auto nv = j.find("null_video");
    if (nv != j.end() && nv->is_object()) {
      NullVideoBackend* n = video_ ? as_null_video(video_) : nullptr;
      if (!n) throw CmdError{"unsupported", "not the null video backend"};
      std::string err;
      if (!null_video_set(n, *nv, &err)) throw CmdError{"bad_args", err};
      bool rep;
      if (jget_bool(*nv, "report", &rep) && rep) ev_->emit(Json{{"ev", "null_video"}, {"report", null_video_report(n)}});
    }
    auto nb = j.find("null");
    if (nb != j.end() && nb->is_object()) {
      NullBackend* n = as_null_backend(&backend_);
      if (!n) throw CmdError{"unsupported", "not the null backend"};
      if (jget_bool(*nb, "vmic_in_use", &b)) null_backend_set_vmic_in_use(n, b);
      std::string key;
      if (jget_str(*nb, "remove", &key) && !null_backend_set_present(n, key, false)) throw CmdError{"no_device", key};
      if (jget_str(*nb, "add", &key) && !null_backend_set_present(n, key, true)) throw CmdError{"no_device", key};
    }
    ok(req);
  }

  void emit_running(StreamBase& s) {
    std::string dev = s.device_in_use();
    ev_->emit(Json{{"ev", "stream_state"}, {"stream", s.id()}, {"state", "running"},
                   {"device", dev.empty() ? Json(nullptr) : Json(dev)}});
  }

  void stop_stream(uint32_t id, const std::string& error_code, const std::string& detail = "") {
    auto it = streams_.find(id);
    if (it == streams_.end()) return;
    Entry e = it->second;
    streams_.erase(it);
    lipsync_.erase(id);
    auto fit = flows_.find(e.flow);
    if (fit != flows_.end()) fit->second->detach(id);
    e.base->stop();
    if (e.vsend) {
      Json cev;
      if (e.vsend->take_camera_event(&cev)) ev_->emit(cev);  // `camera` stopped
    }
    Json ev{{"ev", "stream_state"}, {"stream", id}, {"state", error_code.empty() ? "stopped" : "failed"},
            {"device", nullptr}};
    if (!error_code.empty()) ev["error"] = error_code;
    if (!detail.empty()) ev["detail"] = detail;
    ev_->emit(ev);
  }

  void on_stream_lost(StreamHandle* h, const std::string& why) {
    for (auto& kv : streams_) {
      if (kv.second.base->owns(h)) {
        BA_WARN("stream %u lost its device: %s", kv.first, why.c_str());
        stop_stream(kv.first, "no_device");
        return;
      }
    }
    for (auto it = tests_.begin(); it != tests_.end(); ++it) {
      if ((*it)->owns(h)) {
        (*it)->stop();
        tests_.erase(it);
        return;
      }
    }
  }

  // ---------------------------------------------------------------- devices

  // `permission` after ready and whenever a backend notification finds it changed (a prompt answered).
  void emit_permission(bool force) {
    const std::string mic = backend_.permission("mic"), sys = backend_.permission("system_audio");
    std::string cam;
    if (video_ && has_word(video_features_, "camera_capture")) {
      cam = video_->cameras().permission;
      if (cam.empty()) cam = "unknown";
    }
    if (!force && mic == last_perm_mic_ && sys == last_perm_sys_ && cam == last_perm_cam_) return;
    last_perm_mic_ = mic;
    last_perm_sys_ = sys;
    last_perm_cam_ = cam;
    Json ev{{"ev", "permission"}, {"mic", mic}, {"system_audio", sys}};
    if (!cam.empty()) ev["camera"] = cam;
    ev_->emit(ev);
  }

  void emit_devices(bool force) {
    DeviceSnapshot s = backend_.snapshot();
    const int64_t now = now_ns();
    if (s.vmic_in_use) {
      vmic_eff_ = true;
      vmic_off_since_ = 0;
    } else if (vmic_eff_) {
      if (vmic_off_since_ == 0) vmic_off_since_ = now;
      if (now - vmic_off_since_ >= 3'000'000'000LL) vmic_eff_ = false;  // off after 3 s
    }
    Json in = Json::array(), out = Json::array();
    for (auto& d : s.devices) {
      if (d.role != Role::Normal) continue;
      Json o{{"key", d.key}, {"name", d.name}, {"transport", d.transport}, {"channels", d.channels},
             {"form", form_name(d.form)}};
      if (d.input) in.push_back(o);
      if (d.output) out.push_back(o);
    }
    Json ev{{"ev", "devices"}, {"inputs", in}, {"outputs", out},
            {"default_input", s.default_input.empty() ? Json(nullptr) : Json(s.default_input)},
            {"default_output", s.default_output.empty() ? Json(nullptr) : Json(s.default_output)},
            {"virtual_mic", Json{{"kind", s.vmic_kind.empty() ? Json(nullptr) : Json(s.vmic_kind)},
                                 {"present", s.vmic_present},
                                 {"in_use", vmic_eff_}}}};
    std::string text = ev.dump();
    if (force || text != last_devices_) {
      last_devices_ = text;
      ev_->emit(ev);
    }
  }

  // ---------------------------------------------------------------- timer

  void tick() {
    const int64_t now = now_ns();
    if (vmic_eff_ && vmic_off_since_ && now - vmic_off_since_ >= 3'000'000'000LL) emit_devices(false);
    for (auto it = tests_.begin(); it != tests_.end();) {
      if (now >= (*it)->until_ns) {
        (*it)->stop();
        it = tests_.erase(it);
      } else {
        ++it;
      }
    }
    // permission "suspect": a mic capture that delivered digital zero for 5 s. Not for system
    // sound: a silent machine's tap is digital zero too.
    // camera: `camera` events of video send streams; an undetermined camera permission re-read every 2 s
    for (auto& kv : streams_) {
      Json cev;
      if (kv.second.vsend && kv.second.vsend->take_camera_event(&cev)) ev_->emit(cev);
    }
    if (last_perm_cam_ == "undetermined" && now >= next_cam_perm_ns_) {
      next_cam_perm_ns_ = now + 2'000'000'000LL;
      emit_permission(false);
    }
    for (auto& kv : streams_) {
      StreamBase& s = *kv.second.base;
      const bool mic = (kv.second.send && kv.second.send->kind() == "mic") || kv.second.loop;
      if (!mic || s.device_in_use().rfind("test:", 0) == 0) continue;
      if (s.device_meter.take_nonzero()) {
        s.zero_since_ns = now;
      } else if (!s.suspect_reported && s.zero_since_ns && now - s.zero_since_ns > 5'000'000'000LL) {
        s.suspect_reported = true;
        ev_->emit(Json{{"ev", "permission"}, {"mic", "suspect"}});
      }
    }
    // Howl guard of system send streams: one `howl` event per change (§3.3).
    for (auto& kv : streams_) {
      HowlGuard* guard = kv.second.send ? kv.second.send->howl() : nullptr;
      HowlReport r;
      if (!guard || !guard->poll(&r)) continue;
      if (r.active) BA_WARN("stream %u: feedback at %.0f Hz, sound ducked %.0f dB", kv.first, r.freq_hz, r.duck_db);
      ev_->emit(Json{{"ev", "howl"}, {"stream", kv.first}, {"active", r.active},
                     {"freq_hz", std::round(r.freq_hz)}, {"duck_db", r.duck_db}});
    }
    lipsync_tick(now);
    if (meter_on_ && now >= next_levels_ns_) {
      next_levels_ns_ = now + meter_period_ns_;
      emit_levels();
    } else if (!meter_on_) {
      drain_meters();
    }
    if (now - last_stats_ns_ >= int64_t(opt_.stats_period_ms) * 1'000'000) {
      double dt = (now - last_stats_ns_) * 1e-9;
      last_stats_ns_ = now;
      const double cpu = process_cpu_s();
      const double cpu_pct = last_cpu_s_ > 0 && dt > 0 ? 100.0 * (cpu - last_cpu_s_) / dt : 0.0;
      last_cpu_s_ = cpu;
      if (!streams_.empty()) {
        Json st = Json::object();
        for (auto& kv : streams_) st[std::to_string(kv.first)] = kv.second.base->stats(dt);
        ev_->emit(Json{{"ev", "stats"}, {"t", wall_ms()}, {"now_ns", now_ns()},
                       {"cpu_pct", std::round(cpu_pct * 100) / 100}, {"streams", st}});
      }
    }
  }

  // Camera lip sync (§4.9): for each video recv stream, the policy fed with its video_present_delay and the
  // audio_present_delay of the mic stream from the same sender (same flow) rendering into the virtual mic, both
  // with the offset the video stream maps with. A mic from another machine (another flow), a mic rendered to a
  // speaker, or a mic without a time map (no `sr`) gives no audio input: no hold.
  void lipsync_tick(int64_t now) {
    for (auto& kv : streams_) {
      const auto& v = kv.second.vrecv;
      if (!v) continue;
      LipSyncInput in;
      int64_t off = 0;
      double path = LipSyncHold::kNone;
      in.video = v->core().lipsync_video(now, &in.video_ms, &off);
      if (in.video) {
        for (auto& a : streams_) {
          const auto& r = a.second.recv;
          if (!r || a.second.flow != kv.second.flow || !r->virtual_mic() || r->kind() != "mic") continue;
          const double p = sync_path_override_ms_ >= 0 ? sync_path_override_ms_ : std::max(0.0, r->render_path_ms());
          if (r->lipsync_audio(now, off, p, &in.audio_ms)) {
            in.audio = true;
            path = p;
            break;
          }
        }
      }
      LipSyncPolicy& pol = lipsync_[kv.first];
      const double hold = pol.update(now, in);
      LipSyncHold* h = v->sync();
      const double applied = h->overridden() ? h->override_ms() : hold;
      h->set_policy(hold, pol.have_skew() ? pol.diff_ms() - applied : LipSyncHold::kNone,
                    in.audio ? in.audio_ms : LipSyncHold::kNone, path);
    }
  }

  struct Acc {
    double sumsq = 0, peak = 0;
    uint64_t frames = 0;
    Json value() const {
      return Json::array({to_db(frames ? sumsq / frames : 0, true), to_db(peak, false)});
    }
  };

  void drain_meters() {
    Acc a;
    for (auto& kv : streams_) {
      if (kv.second.video()) continue;
      kv.second.base->meter.drain(&a.sumsq, &a.peak, &a.frames);
      kv.second.base->device_meter.drain(&a.sumsq, &a.peak, &a.frames);
      if (kv.second.loop) kv.second.loop->out_meter.drain(&a.sumsq, &a.peak, &a.frames);
    }
    for (auto& t : tests_) t->device_meter.drain(&a.sumsq, &a.peak, &a.frames);
  }

  void emit_levels() {
    std::map<std::string, Acc> in, out;
    Json streams = Json::object();
    for (auto& kv : streams_) {
      if (kv.second.video()) continue;
      StreamBase& s = *kv.second.base;
      Acc sa;
      s.meter.drain(&sa.sumsq, &sa.peak, &sa.frames);
      streams[std::to_string(kv.first)] = sa.value();
      std::string dev = s.device_in_use();
      Acc& d = (s.is_output() ? out : in)[dev];
      if (dev.rfind("test:", 0) == 0 || dev.empty()) {
        Acc junk;
        s.device_meter.drain(&junk.sumsq, &junk.peak, &junk.frames);
        (s.is_output() ? out : in).erase(dev);
      } else {
        s.device_meter.drain(&d.sumsq, &d.peak, &d.frames);
      }
      if (kv.second.loop) {
        Acc& o = out[kv.second.loop->render_device()];
        kv.second.loop->out_meter.drain(&o.sumsq, &o.peak, &o.frames);
      }
    }
    for (auto& t : tests_) {
      Acc& d = in[t->device_in_use()];
      t->device_meter.drain(&d.sumsq, &d.peak, &d.frames);
    }
    Json ji = Json::object(), jo = Json::object();
    for (auto& kv : in) ji[kv.first] = kv.second.value();
    for (auto& kv : out) jo[kv.first] = kv.second.value();
    ev_->emit(Json{{"ev", "levels"}, {"t", wall_ms()}, {"in", ji}, {"out", jo}, {"streams", streams}});
  }

  // ---------------------------------------------------------------- parent / shutdown

  void watch_parent() {
    set_thread_name("beamer-audio-parent");
    const int64_t pid = opt_.parent_pid;
    bool dead = false;
#if defined(_WIN32)
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!h) dead = GetLastError() == ERROR_INVALID_PARAMETER;
    while (!dead && h && !stop_threads_.load()) {
      if (WaitForSingleObject(h, 200) == WAIT_OBJECT_0) dead = true;
    }
    if (h) CloseHandle(h);
#elif defined(__APPLE__)
    int kq = kqueue();
    struct kevent kev;
    EV_SET(&kev, static_cast<uintptr_t>(pid), EVFILT_PROC, EV_ADD | EV_ENABLE, NOTE_EXIT, 0, nullptr);
    if (kevent(kq, &kev, 1, nullptr, 0, nullptr) < 0) dead = true;  // ESRCH: already gone
    while (!dead && !stop_threads_.load()) {
      struct kevent out;
      struct timespec ts = {0, 200'000'000};
      int n = kevent(kq, nullptr, 0, &out, 1, &ts);
      if (n > 0 && out.filter == EVFILT_PROC && (out.fflags & NOTE_EXIT)) dead = true;
    }
    close(kq);
#else
    while (!dead && !stop_threads_.load()) {
      if (kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) dead = true;
      else sleep_ns(200'000'000);
    }
#endif
    if (dead && !stop_threads_.load()) {
      BA_WARN("parent %lld exited; quitting", static_cast<long long>(pid));
      post([this] { quit_ = true; });
      // last resort if the orderly exit hangs
      std::thread([] {
        sleep_ns(3'000'000'000LL);
        std::_Exit(0);
      }).detach();
    }
  }

  void shutdown() {
    std::vector<uint32_t> ids;
    for (auto& kv : streams_) ids.push_back(kv.first);
    for (uint32_t id : ids) stop_stream(id, "");
    for (auto& t : tests_) t->stop();
    tests_.clear();
    for (auto& kv : flows_) net_.close(kv.second);
    flows_.clear();
    net_.stop();
  }

  Backend& backend_;
  HelperOptions opt_;
  EventSink* ev_;
  NetLoop net_;
  std::map<std::string, std::shared_ptr<Flow>> flows_;
  std::map<uint32_t, Entry> streams_;
  std::vector<std::unique_ptr<TestInput>> tests_;

  std::mutex qmu_;
  std::condition_variable qcv_;
  std::deque<std::function<void()>> queue_;
  bool quit_ = false;
  std::atomic<bool> stop_threads_{false};
  std::atomic<bool> tick_pending_{false};

  std::string last_devices_;
  bool vmic_eff_ = false;
  int64_t vmic_off_since_ = 0;
  bool meter_on_ = false;
  int64_t meter_period_ns_ = 100'000'000, next_levels_ns_ = 0, last_stats_ns_ = 0;
  double last_cpu_s_ = 0;
  std::string last_perm_mic_, last_perm_sys_, last_perm_cam_;
  // video
  VideoBackend* video_ = nullptr;
  std::vector<std::string> video_features_;
  std::map<std::string, Placeholder> vcam_texts_;
  std::string vcam_idle_ = "waiting", last_vcam_;
  double idr_pace_x_ = 4.0, playout_floor_ms_ = 50.0;
  std::map<uint32_t, LipSyncPolicy> lipsync_;  // per video recv stream (§4.9)
  double sync_override_ms_ = -1;               // dev sync_offset_ms (< 0: the policy)
  double sync_path_override_ms_ = -1;          // dev sync_audio_path_ms (< 0: the backend's)
  int64_t next_cam_perm_ns_ = 0;
};

// ------------------------------------------------------------------ command line

void usage() {
  fprintf(stderr,
          "usage: beamer-audio --control --parent-pid <pid> --state-dir <dir> --exclude-pid <pid>\n"
          "                    [--dev] [--log-level debug|info|warn] [--backend null:<options>]\n"
          "                    [--video null:<options> | --no-video]\n"
          "       beamer-audio list | restore | version   [--backend null:<options>]\n");
}

bool read_stdin_line(std::string* line) {
  line->clear();
  char buf[4096];
  for (;;) {
    if (!std::fgets(buf, sizeof buf, stdin)) return !line->empty();
    line->append(buf);
    if (!line->empty() && line->back() == '\n') {
      line->pop_back();
      if (!line->empty() && line->back() == '\r') line->pop_back();
      return true;
    }
    if (line->size() > (1u << 20)) {  // runaway line: drop it
      line->clear();
    }
  }
}

void write_stdout_line(const std::string& s) {
  std::fwrite(s.data(), 1, s.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

}  // namespace

int run_control(Backend& backend, const HelperOptions& opt, const HelperIo& io) {
  EventSink ev(io.write_line);
  Helper h(backend, opt, &ev);
  return h.run(io);
}

int run_helper(Backend& given, int argc, char** argv) { return run_helper(given, nullptr, argc, argv); }

int run_helper(Backend& given, VideoBackend* given_video, int argc, char** argv) {
#if defined(_WIN32)
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#else
  std::signal(SIGPIPE, SIG_IGN);
#endif
  HelperOptions opt;
  bool control = false;
  std::string mode, null_opts, null_video_opts;
  bool use_null = false, use_null_video = false, no_video = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](const char* what) -> std::string {
      if (i + 1 >= argc) {
        fprintf(stderr, "warn: %s needs a value\n", what);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--control") control = true;
    else if (a == "--dev") opt.dev = true;
    else if (a == "--parent-pid") opt.parent_pid = std::atoll(next("--parent-pid").c_str());
    else if (a == "--state-dir") opt.state_dir = next("--state-dir");
    else if (a == "--exclude-pid") opt.exclude_pids.push_back(std::atoll(next("--exclude-pid").c_str()));
    else if (a == "--log-level") {
      LogLevel l;
      if (parse_log_level(next("--log-level"), &l)) set_log_level(l);
    } else if (a == "--backend") {
      std::string b = next("--backend");
      if (b.rfind("null", 0) != 0) {
        fprintf(stderr, "warn: unknown backend %s\n", b.c_str());
        return 2;
      }
      use_null = true;
      null_opts = b.size() > 5 ? b.substr(5) : "";
    } else if (a == "--video") {
      std::string v = next("--video");
      if (v.rfind("null", 0) != 0) {
        fprintf(stderr, "warn: unknown video backend %s\n", v.c_str());
        return 2;
      }
      use_null_video = true;
      null_video_opts = v.size() > 5 ? v.substr(5) : "";
    } else if (a == "--no-video") {
      no_video = true;
    } else if (a == "list" || a == "restore" || a == "version") mode = a;
    else if (a == "--help" || a == "-h") {
      usage();
      return 0;
    } else {
      fprintf(stderr, "warn: unknown argument %s\n", a.c_str());
    }
  }
  std::unique_ptr<Backend> null_backend;
  if (use_null) null_backend = make_null_backend(null_opts);
  Backend& backend = use_null ? *null_backend : given;
  std::unique_ptr<VideoBackend> null_video;
  if (use_null_video && !no_video) null_video = make_null_video_backend(null_video_opts);
  opt.video = no_video ? nullptr : (null_video ? null_video.get() : given_video);

  if (mode == "version") {
    write_stdout_line(Json{{"version", kEngineVersion}, {"proto", kControlProto}, {"platform", kPlatform},
                           {"opus", opus_version()},
                           {"aec", kAecCompiled ? Json("speexdsp 1.2.1") : Json(nullptr)}}.dump());
    return 0;
  }
  if (mode == "restore") {
    int n = backend.restore_journal();
    write_stdout_line(Json{{"restored", n}}.dump());
    return 0;
  }
  if (mode == "list") {
    DeviceSnapshot s = backend.snapshot();
    Json devs = Json::array();
    for (auto& d : s.devices) {
      const char* role = d.role == Role::VirtualMic ? "virtual_mic" : d.role == Role::VirtualMicFeed ? "virtual_mic_feed" : "normal";
      devs.push_back(Json{{"key", d.key}, {"name", d.name}, {"manufacturer", d.manufacturer},
                          {"transport", d.transport}, {"input", d.input}, {"output", d.output},
                          {"channels", d.channels}, {"nominal_rate", d.nominal_rate},
                          {"form", form_name(d.form)}, {"role", role}});
    }
    write_stdout_line(Json{{"devices", devs}, {"default_input", s.default_input}, {"default_output", s.default_output},
                           {"virtual_mic", Json{{"kind", s.vmic_kind}, {"present", s.vmic_present}, {"in_use", s.vmic_in_use}}},
                           {"features", engine_features(backend)}, {"permission_mic", backend.permission("mic")}}
                          .dump(2));
    return 0;
  }
  if (!control) {
    usage();
    return 2;
  }
  enable_fine_timers();
  BA_INFO("beamer-audio %s (%s, opus %s) control, parent %lld", kEngineVersion, kPlatform, opus_version(),
          static_cast<long long>(opt.parent_pid));
  HelperIo io;
  io.read_line = read_stdin_line;
  io.write_line = write_stdout_line;
  io.detach_reader = true;
  return run_control(backend, opt, io);
}

}  // namespace beamer::audio
