// SPDX-License-Identifier: GPL-3.0-only
#include "streams.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

#include "flow.h"

namespace beamer::audio {

void map_channels(const float* in, int in_ch, float* out, int out_ch, int frames) {
  if (in_ch == out_ch) {
    std::memcpy(out, in, sizeof(float) * size_t(frames) * in_ch);
    return;
  }
  if (in_ch == 1) {
    for (int i = 0; i < frames; ++i)
      for (int c = 0; c < out_ch; ++c) out[size_t(i) * out_ch + c] = in[i];
    return;
  }
  if (out_ch == 1) {
    const float g = 1.0f / in_ch;
    for (int i = 0; i < frames; ++i) {
      float s = 0;
      for (int c = 0; c < in_ch; ++c) s += in[size_t(i) * in_ch + c];
      out[i] = s * g;
    }
    return;
  }
  for (int i = 0; i < frames; ++i)
    for (int c = 0; c < out_ch; ++c) out[size_t(i) * out_ch + c] = c < in_ch ? in[size_t(i) * in_ch + c] : 0.0f;
}

std::string error_code_of(const std::string& e) {
  static const char* codes[] = {"bad_args", "no_device", "blocked_device", "no_virtual_mic", "device_busy",
                                "permission", "unsupported", "bind_failed", "unknown_flow",
                                "unknown_stream", "internal"};
  for (const char* c : codes) {
    size_t n = std::strlen(c);
    if (e.compare(0, n, c) == 0 && (e.size() == n || e[n] == ':' || e[n] == ' ')) return c;
  }
  return "internal";
}

static void split_err(const std::string& e, std::string* code, std::string* msg) {
  *code = error_code_of(e);
  *msg = e.empty() ? *code : e;
}

static double round1(double v) { return std::round(v * 10.0) / 10.0; }

// ================================================================== SendStream

SendStream::SendStream(const Config& cfg, std::shared_ptr<Flow> flow)
    : StreamBase(cfg.id, cfg.kind),
      cfg_(cfg),
      flow_(std::move(flow)),
      frame_(cfg.codec.frame()),
      ring_(kRate, cfg.codec.channels),
      conv_(new float[size_t(kConvFrames) * 2]),
      ts_(random_u32()),
      rng_(random_u32()) {
  sealer_.init(cfg_.key, cfg_.id & ~kDirBit);
}

SendStream::~SendStream() {
  stop();
  secure_wipe(cfg_.key, sizeof cfg_.key);
}

bool SendStream::start(Backend& backend, const std::vector<int64_t>& exclude_pids, std::string* code,
                       std::string* msg) {
  std::string e;
  if (!enc_.init(cfg_.codec, &e)) {
    *code = "internal";
    *msg = e;
    return false;
  }
  enc_thread_ = std::thread([this] { encoder_loop(); });
  if (!cfg_.test_signal.empty()) {
    TestSignal probe;
    if (!TestSignal::parse(cfg_.test_signal, &probe)) {
      *code = "bad_args";
      *msg = "bad test source " + cfg_.test_signal;
      return false;
    }
    device_label_ = "test:" + cfg_.test_signal;
    test_thread_ = std::thread([this] { test_source_loop(); });
    return true;
  }
  CaptureSpec spec{cfg_.kind == "system" ? CaptureKind::System : CaptureKind::Mic,
                   cfg_.kind == "system" ? std::string() : cfg_.device, cfg_.codec.channels,
                   cfg_.kind == "system" && cfg_.mute_local, exclude_pids};
  handle_ = backend.open_capture(spec, this, &e);
  if (!handle_) {
    split_err(e, code, msg);
    return false;
  }
  return true;
}

void SendStream::stop() {
  if (stopped_) return;
  stopped_ = true;
  handle_.reset();  // the backend stops calling on_capture before this returns
  stop_.store(true);
  wake_.signal();
  if (test_thread_.joinable()) test_thread_.join();
  if (enc_thread_.joinable()) enc_thread_.join();
}

void SendStream::push_samples(const float* x, int frames) {
  const int C = cfg_.codec.channels;
  meter.post(x, frames, C);
  size_t w = ring_.write(x, static_cast<size_t>(frames));
  if (w < static_cast<size_t>(frames)) overflow_.fetch_add(frames - w, std::memory_order_relaxed);
  if (ring_.available() >= static_cast<size_t>(frame_)) wake_.signal();
}

void SendStream::on_capture(const float* x, int frames, int channels, uint64_t) {
  if (channels <= 0 || frames <= 0) return;
  device_meter.post(x, frames, channels);
  const int C = cfg_.codec.channels;
  for (int off = 0; off < frames;) {
    int k = std::min(kConvFrames, frames - off);
    map_channels(x + size_t(off) * channels, channels, conv_.get(), C, k);
    push_samples(conv_.get(), k);
    off += k;
  }
}

void SendStream::test_source_loop() {
  set_thread_name("beamer-audio-test");
  set_thread_interactive();
  TestSignal sig;
  TestSignal::parse(cfg_.test_signal, &sig);
  const int block = 480, C = cfg_.codec.channels;
  std::vector<float> mono(block), buf(size_t(block) * C);
  int64_t s = 0;
  const auto t0 = std::chrono::steady_clock::now();
  test_t0_ns_.store(std::chrono::duration_cast<std::chrono::nanoseconds>(t0.time_since_epoch()).count());
  while (!stop_.load()) {
    auto due = t0 + std::chrono::nanoseconds((s + block) * 1'000'000'000LL / kRate);
    std::this_thread::sleep_until(due);
    sig.render(s, block, mono.data());
    map_channels(mono.data(), 1, buf.data(), C, block);
    device_meter.post(buf.data(), block, C);
    push_samples(buf.data(), block);
    s += block;
  }
}

void SendStream::encoder_loop() {
  set_thread_name("beamer-audio-enc");
  set_thread_interactive();
  const int C = cfg_.codec.channels;
  std::vector<float> pcm(size_t(frame_) * C);
  uint8_t payload[kMaxPayload];
  uint8_t pkt[kMaxPacket];
  std::uniform_real_distribution<double> uni(0.0, 1.0);
  const int link_cap = cfg_.kind == "mic" ? 32000 : 48000;  // §4.5
  while (!stop_.load()) {
    wake_.wait_ms(20);
    while (!stop_.load() && ring_.available() >= static_cast<size_t>(frame_)) {
      ring_.read(pcm.data(), static_cast<size_t>(frame_));
      const uint32_t ts = ts_;
      ts_ += static_cast<uint32_t>(frame_);
      enc_.set_bitrate(flow_->link_mode() ? std::min(cfg_.codec.bitrate, link_cap) : cfg_.codec.bitrate);
      const int64_t t0 = now_ns();
      int n = enc_.encode(pcm.data(), payload, static_cast<int>(sizeof payload));
      enc_ns_.fetch_add(static_cast<uint64_t>(now_ns() - t0), std::memory_order_relaxed);
      if (n < 0) {
        enc_err_.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      double ss = 0;
      for (float v : pcm) ss += double(v) * v;
      Header h;
      h.ptype = kMedia;
      h.codec = kCodecOpus;
      h.flags = cfg_.codec.fec ? kFlagFec : 0;
      if (first_) {
        h.flags |= kFlagMarker;
        first_ = false;
      }
      if (std::sqrt(ss / pcm.size()) < 0.003) h.flags |= kFlagSilence;  // < -50 dBFS
      h.stream = cfg_.id;
      h.ts = ts;
      h.nsamp = static_cast<uint16_t>(frame_);
      h.channels = static_cast<uint8_t>(C);
      size_t len = sealer_.seal(h, payload, static_cast<size_t>(n), pkt);
      frames_.fetch_add(1, std::memory_order_relaxed);
      double loss = flow_->dev_loss.load(std::memory_order_relaxed);
      if (loss > 0 && uni(rng_) < loss) {
        dev_dropped_.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      flow_->send_media(pkt, len);
      tx_.fetch_add(1, std::memory_order_relaxed);
      tx_bytes_.fetch_add(len + 28, std::memory_order_relaxed);
    }
  }
}

OpenResult SendStream::ingest_rr(const uint8_t* data, size_t len, int64_t) {
  Header h;
  uint8_t payload[kMaxPacket];
  size_t plen = 0;
  OpenResult r = open_packet(cfg_.key, cfg_.id | kDirBit, &rr_window_, data, len, &h, payload, &plen);
  if (r == OpenResult::Ok && plen >= 6) {
    peer_level_01ms_.store(get_u16(payload + 4), std::memory_order_relaxed);
    rr_rx_.fetch_add(1, std::memory_order_relaxed);
  }
  return r;
}

Json SendStream::stats(double dt) {
  uint64_t tx = tx_.load(), bytes = tx_bytes_.load();
  double kbps = dt > 0 ? (bytes - last_tx_bytes_) * 8.0 / dt / 1000.0 : 0;
  last_tx_ = tx;
  last_tx_bytes_ = bytes;
  Json j{{"dir", "send"}, {"kind", kind_}, {"path", flow_->link_mode() ? "link" : "udp"},
         {"kbps", round1(kbps)}, {"bitrate", enc_.bitrate()}};
  int pl = peer_level_01ms_.load();
  j["jb_ms"] = pl >= 0 ? Json(pl / 10.0) : Json(nullptr);
  double rtt = flow_->rtt_ms();
  j["rtt_ms"] = rtt >= 0 ? Json(round1(rtt)) : Json(nullptr);
  const uint64_t fr = frames_.load(), en = enc_ns_.load();
  j["enc_us"] = fr > last_frames_ ? round1((en - last_enc_ns_) / 1000.0 / double(fr - last_frames_)) : 0.0;
  last_frames_ = fr;
  last_enc_ns_ = en;
  j["frames"] = fr;
  j["overflow"] = overflow_.load();
  j["dev_dropped"] = dev_dropped_.load();
  j["rr_rx"] = rr_rx_.load();
  j["enc_err"] = enc_err_.load();
  if (const int64_t t0 = test_t0_ns_.load()) j["test_t0_ns"] = t0;  // sample k is due at t0 + k/48 kHz
  return j;
}

// ================================================================== RecvStream

static JbConfig jb_for(const RecvStream::Config& cfg) {
  JbConfig j = cfg.jb;
  j.channels = cfg.codec.channels;
  j.frame = cfg.codec.frame();
  j.fec = cfg.codec.fec;
  return j;
}

RecvStream::RecvStream(const Config& cfg, std::shared_ptr<Flow> flow)
    : StreamBase(cfg.id, cfg.kind),
      cfg_(cfg),
      flow_(std::move(flow)),
      ring_(256),
      jb_(jb_for(cfg)),
      out_(new float[size_t(JitterBuffer::kMaxPull) * cfg.codec.channels]),
      level_samples_(4096) {
  rr_sealer_.init(cfg_.key, cfg_.id | kDirBit);
}

RecvStream::~RecvStream() {
  stop();
  secure_wipe(cfg_.key, sizeof cfg_.key);
}

bool RecvStream::start(Backend& backend, std::string* code, std::string* msg) {
  std::string e;
  if (!jb_.init(&e)) {
    *code = "internal";
    *msg = e;
    return false;
  }
  RenderSpec spec{cfg_.virtual_mic ? std::string() : cfg_.device, cfg_.virtual_mic, cfg_.codec.channels};
  handle_ = backend.open_render(spec, this, &e);
  if (!handle_) {
    split_err(e, code, msg);
    return false;
  }
  return true;
}

void RecvStream::stop() {
  if (stopped_) return;
  stopped_ = true;
  handle_.reset();
}

OpenResult RecvStream::ingest(const uint8_t* data, size_t len, int64_t now, bool via_link) {
  Header h;
  size_t plen = 0;
  MediaFrame* f = ring_.claim();
  uint8_t tmp[kMaxPacket];
  OpenResult r = open_packet(cfg_.key, cfg_.id & ~kDirBit, &window_, data, len, &h, f ? f->data : tmp, &plen);
  if (r != OpenResult::Ok) return r;
  if (plen > kMaxPayload) return OpenResult::Rejected;
  rx_pkts_.fetch_add(1, std::memory_order_relaxed);
  rx_bytes_.fetch_add(len + 28, std::memory_order_relaxed);
  if (via_link) rx_link_.fetch_add(1, std::memory_order_relaxed);
  last_via_link_.store(via_link, std::memory_order_relaxed);
  if (h.seq > hi_seq_) {
    hi_seq_ = h.seq;
    seq_hi_.store(h.seq, std::memory_order_relaxed);
  }
  last_media_ns_.store(now, std::memory_order_relaxed);
  newest_ts_.store(h.ts, std::memory_order_relaxed);
  if (!f) {
    ring_full_.fetch_add(1, std::memory_order_relaxed);
    return r;
  }
  f->arrival_ns = now;
  f->seq = h.seq;
  f->ts = h.ts;
  f->nsamp = h.nsamp;
  f->flags = h.flags;
  f->channels = h.channels;
  f->len = static_cast<uint16_t>(plen);
  ring_.commit();
  return r;
}

bool RecvStream::media_recent(int64_t now) const {
  int64_t t = last_media_ns_.load(std::memory_order_relaxed);
  return t != 0 && now - t < 1'000'000'000;
}

size_t RecvStream::make_rr(uint8_t* out) {
  uint8_t p[6];
  put_u32(p, newest_ts_.load(std::memory_order_relaxed));
  double lv = playing_.load(std::memory_order_relaxed) ? level_ms_.load(std::memory_order_relaxed) : 0.0;
  put_u16(p + 4, static_cast<uint16_t>(std::min(65535.0, std::max(0.0, lv * 10.0))));
  Header h;
  h.ptype = kRR;
  h.stream = cfg_.id;
  return rr_sealer_.seal(h, p, sizeof p, out);
}

void RecvStream::on_render(float* out, int frames, int channels, uint64_t) {
  if (frames <= 0 || channels <= 0) return;
  const double now = now_s();
  while (const MediaFrame* f = ring_.front()) {
    jb_.push(*f);
    ring_.release();
  }
  const int C = cfg_.codec.channels;
  for (int off = 0; off < frames;) {
    int k = std::min(JitterBuffer::kMaxPull, frames - off);
    jb_.pull(out_.get(), k, now);
    level_samples_.push(static_cast<float>(jb_.last_level_ms()));
    meter.post(out_.get(), k, C);
    map_channels(out_.get(), C, out + size_t(off) * channels, channels, k);
    off += k;
  }
  device_meter.post(out, frames, channels);
  publish();
}

void RecvStream::publish() {
  const JbCounters& c = jb_.counters();
  const auto R = std::memory_order_relaxed;
  level_ms_.store(jb_.last_level_ms(), R);
  target_ms_.store(jb_.target_ms(), R);
  ppm_.store(jb_.ppm(), R);
  integ_.store(jb_.integ_ppm(), R);
  playing_.store(jb_.playing(), R);
  c_late_.store(c.late, R);
  c_expand_.store(c.expand, R);
  c_drop_.store(c.drop, R);
  c_flushed_.store(c.flushed, R);
  c_fec_.store(c.fec, R);
  c_plc_.store(c.plc, R);
  c_dec_err_.store(c.dec_err, R);
  c_underrun_.store(c.underrun, R);
  c_ok_.store(c.ok, R);
  c_dup_.store(c.dup, R);
}

Json RecvStream::stats(double dt) {
  std::vector<float> lv;
  float v;
  while (level_samples_.pop(&v)) {
    lv.push_back(v);
    hist_[std::min<size_t>(hist_.size() - 1, static_cast<size_t>(std::max(0.0f, v) * 2))]++;
  }
  double mean = 0, p99 = 0, mx = 0;
  if (!lv.empty()) {
    for (float x : lv) mean += x;
    mean /= lv.size();
    std::sort(lv.begin(), lv.end());
    p99 = lv[std::min(lv.size() - 1, static_cast<size_t>(std::ceil(0.99 * lv.size())) - 1)];
    mx = lv.back();
  } else {
    mean = level_ms_.load();
  }
  auto delta = [](uint64_t now, uint64_t* prev) {
    uint64_t d = now - *prev;
    *prev = now;
    return d;
  };
  uint64_t rx = rx_pkts_.load(), bytes = rx_bytes_.load(), shi = seq_hi_.load();
  uint64_t got = rx - p_rx_;
  uint64_t expected = p_seq_hi_ ? shi - p_seq_hi_ : got;
  p_rx_ = rx;
  p_seq_hi_ = shi;
  double loss = expected > 0 && expected > got ? 100.0 * double(expected - got) / double(expected) : 0.0;
  double kbps = dt > 0 ? (bytes - p_bytes_) * 8.0 / dt / 1000.0 : 0;
  p_bytes_ = bytes;
  Json j{{"dir", "recv"}, {"kind", kind_}, {"path", last_via_link_.load() ? "link" : "udp"}};
  j["jb_ms"] = round1(mean);
  j["jb_p99_ms"] = round1(p99);
  j["jb_max_ms"] = round1(mx);
  j["target_ms"] = round1(target_ms_.load());
  // over the run (since start or dev reset_stats): level p99 from the histogram, mean target
  hist_target_sum_ += target_ms_.load();
  hist_target_n_++;
  uint64_t total = 0, acc = 0;
  for (uint32_t c : hist_) total += c;
  double run_p99 = 0;
  for (size_t i = 0; i < hist_.size() && total; ++i) {
    acc += hist_[i];
    if (acc * 100 >= total * 99) {
      run_p99 = (i + 1) * 0.5;
      break;
    }
  }
  j["run_jb_p99_ms"] = round1(run_p99);
  j["run_target_ms"] = round1(hist_target_n_ ? hist_target_sum_ / hist_target_n_ : 0);
  j["late"] = delta(c_late_.load(), &p_late_);
  j["expand"] = delta(c_expand_.load(), &p_expand_);
  uint64_t dr = delta(c_drop_.load(), &p_drop_), fl = delta(c_flushed_.load(), &p_flushed_);
  j["drop"] = dr + fl;
  j["flushed"] = fl;
  j["loss_pct"] = round1(loss);
  j["fec"] = delta(c_fec_.load(), &p_fec_);
  j["plc"] = delta(c_plc_.load(), &p_plc_);
  j["underrun"] = delta(c_underrun_.load(), &p_underrun_);
  j["dec_err"] = delta(c_dec_err_.load(), &p_dec_err_);
  j["ppm"] = round1(ppm_.load());
  j["ppm_i"] = round1(integ_.load());
  double rtt = flow_->rtt_ms();
  j["rtt_ms"] = rtt >= 0 ? Json(round1(rtt)) : Json(nullptr);
  j["kbps"] = round1(kbps);
  j["rx"] = rx;
  j["rx_link"] = rx_link_.load();
  j["ring_full"] = ring_full_.load();
  j["playing"] = playing_.load();
  return j;
}

// ================================================================== LoopStream

LoopStream::LoopStream(uint32_t id, std::string device)
    : StreamBase(id, "loop"),
      device_(std::move(device)),
      ring_(kRate, 1),
      rs_(1, 8192 + 64),
      conv_(new float[4096]),
      tmp_(new float[8192]),
      out_(new float[4096]) {}

LoopStream::~LoopStream() { stop(); }

bool LoopStream::start(Backend& backend, const std::vector<int64_t>& exclude_pids, std::string* code,
                       std::string* msg) {
  std::string e;
  RenderSpec rspec{std::string(), true, 1};
  render_ = backend.open_render(rspec, this, &e);
  if (!render_) {
    split_err(e, code, msg);
    return false;
  }
  CaptureSpec cspec{CaptureKind::Mic, device_, 1, false, exclude_pids};
  handle_ = backend.open_capture(cspec, this, &e);
  if (!handle_) {
    render_.reset();
    split_err(e, code, msg);
    return false;
  }
  return true;
}

void LoopStream::stop() {
  if (stopped_) return;
  stopped_ = true;
  handle_.reset();
  render_.reset();
}

void LoopStream::on_capture(const float* x, int frames, int channels, uint64_t) {
  if (frames <= 0 || channels <= 0) return;
  device_meter.post(x, frames, channels);
  cap_block_.store(frames, std::memory_order_relaxed);
  for (int off = 0; off < frames;) {
    int k = std::min(4096, frames - off);
    map_channels(x + size_t(off) * channels, channels, conv_.get(), 1, k);
    meter.post(conv_.get(), k, 1);
    size_t w = ring_.write(conv_.get(), static_cast<size_t>(k));
    if (w < static_cast<size_t>(k)) overflow_.fetch_add(k - w, std::memory_order_relaxed);
    off += k;
  }
}

void LoopStream::on_render(float* out, int frames, int channels, uint64_t) {
  if (frames <= 0 || channels <= 0) return;
  const double ms = 1000.0 / kRate;
  for (int off = 0; off < frames;) {
    const int n = std::min(2048, frames - off);
    const double block_s = double(n) / kRate;
    block_max_ = std::max(block_max_, n);
    const double cap_ms = cap_block_.load(std::memory_order_relaxed) * ms;
    const double t = std::min(60.0, std::max(5.0, cap_ms + n * ms + 2.0));
    target_ms_ = t > target_ms_ ? t : target_ms_ + (t - target_ms_) * block_s / 2.0;
    double level = (double(ring_.available()) + rs_.available()) * ms;
    bool produce = true;
    if (!playing_) {
      if (level >= target_ms_) {
        playing_ = true;
        has_avg_ = false;
      } else {
        produce = false;
      }
    }
    if (produce && level > target_ms_ + std::max(40.0, target_ms_)) {
      size_t excess = static_cast<size_t>((level - target_ms_) / ms);
      flushed_.fetch_add(ring_.skip(excess), std::memory_order_relaxed);
      level = (double(ring_.available()) + rs_.available()) * ms;
      has_avg_ = false;
    }
    if (produce) {
      level_avg_ = has_avg_ ? level_avg_ + std::min(1.0, block_s) * (level - level_avg_) : level;
      has_avg_ = true;
      double ppm = drift_.update(level_avg_ - target_ms_, block_s);
      double step = 1.0 + ppm * 1e-6;
      int need = rs_.need(n, step);
      if (need > 0) {
        size_t got = ring_.read(tmp_.get(), static_cast<size_t>(need));
        rs_.feed(tmp_.get(), static_cast<int>(got));
        if (got < static_cast<size_t>(need)) {
          rs_.feed_zeros(need - static_cast<int>(got));
          underruns_.fetch_add(1, std::memory_order_relaxed);
          playing_ = false;
        }
      }
      rs_.out(out_.get(), n, step);
    } else {
      std::memset(out_.get(), 0, sizeof(float) * size_t(n));
    }
    out_meter.post(out_.get(), n, 1);
    map_channels(out_.get(), 1, out + size_t(off) * channels, channels, n);
    level_ms_.store(level, std::memory_order_relaxed);
    off += n;
  }
  tgt_.store(target_ms_, std::memory_order_relaxed);
  ppm_.store(drift_.ppm, std::memory_order_relaxed);
}

Json LoopStream::stats(double) {
  return Json{{"dir", "loop"}, {"kind", "loop"}, {"path", "local"},
              {"jb_ms", round1(level_ms_.load())}, {"target_ms", round1(tgt_.load())},
              {"ppm", round1(ppm_.load())}, {"underrun", underruns_.load()},
              {"flushed", flushed_.load()}, {"overflow", overflow_.load()}};
}

// ================================================================== TestInput

TestInput::TestInput(const std::string& device, int64_t until) : StreamBase(0, "test"), until_ns(until), device_(device) {}

TestInput::~TestInput() { stop(); }

bool TestInput::start(Backend& backend, std::string* code, std::string* msg) {
  std::string e;
  CaptureSpec spec{CaptureKind::Mic, device_, 1, false, {}};
  handle_ = backend.open_capture(spec, this, &e);
  if (!handle_) {
    split_err(e, code, msg);
    return false;
  }
  return true;
}

void TestInput::stop() {
  if (stopped_) return;
  stopped_ = true;
  handle_.reset();
}

void TestInput::on_capture(const float* x, int frames, int channels, uint64_t) {
  if (frames > 0 && channels > 0) device_meter.post(x, frames, channels);
}

}  // namespace beamer::audio
