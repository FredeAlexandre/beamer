// SPDX-License-Identifier: GPL-3.0-only
#include "video_send.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "flow.h"
#include "util.h"
#include "video_recv.h"

namespace beamer::audio {

// ================================================================== SenderRecovery (§4.3)

SenderRecovery::Action SenderRecovery::idr(int64_t now) {
  (void)now;
  idr_pending_ = true;
  deferred_idr_at_ = 0;
  c_.idr_req++;
  return kIdr;
}

SenderRecovery::Action SenderRecovery::on_pli(const Pli& p, int64_t now) {
  c_.pli++;
  if (!started_) {
    // nothing is encoded before the receiver's first PLI: it is the start signal, whatever its reason
    started_ = true;
    idr_pending_ = true;  // start_encoding(): the first AU is an IDR (it may come late: the camera's start)
    return kStart;
  }
  if (p.reason == kPliStart || p.reason == kPliSize) {
    // class 1: coalesced into the pending IDR, else at most one per 1 s; never deferred, never a refresh
    if (idr_pending_ || now < pending_until_) {
      c_.dup++;
      return kNone;
    }
    if (have_idr_ && now - last_idr_ < kClass1Window) {
      c_.dropped_window++;
      return kNone;
    }
    return idr(now);
  }
  // class 2: loss, decoder, stalled
  if (idr_pending_) {
    c_.dup++;
    return kNone;
  }
  if (p.last_clean == last_refresh_clean_ && last_refresh_ != 0 && now - last_refresh_ < rtt_ns_ + 50'000'000) {
    c_.dup++;
    return kNone;
  }
  return class2(now, true, p.last_clean);
}

SenderRecovery::Action SenderRecovery::on_drop(int64_t now) {
  if (!started_ || idr_pending_) return kNone;
  return class2(now, false, kNoFrame);
}

SenderRecovery::Action SenderRecovery::class2(int64_t now, bool from_pli, uint32_t last_clean) {
  const bool want_idr = failed_refreshes_ >= 3 || verified_.empty();
  const bool window_open = !have_idr_ || now - last_idr_ >= kClass2Window;
  if (!want_idr) {
    if (from_pli) last_refresh_clean_ = last_clean;
    last_refresh_ = now;
    refresh_check_at_ = now + 1'000'000'000;
    clean_at_refresh_ = newest_clean_since_;
    c_.refresh_req++;
    return kRefresh;
  }
  if (window_open) {
    failed_refreshes_ = 0;
    refresh_check_at_ = 0;
    return idr(now);
  }
  if (!verified_.empty()) {
    // a class-2 IDR inside its window becomes a refresh when a verified LTR exists
    if (from_pli) last_refresh_clean_ = last_clean;
    last_refresh_ = now;
    c_.refresh_req++;
    return kRefresh;
  }
  if (!deferred_idr_at_) c_.deferred++;
  deferred_idr_at_ = last_idr_ + kClass2Window;  // waits for the window's end
  return kNone;
}

SenderRecovery::Action SenderRecovery::on_timer(int64_t now) {
  if (refresh_check_at_ && now >= refresh_check_at_) {
    refresh_check_at_ = 0;
    failed_refreshes_++;  // no newer clean run within 1 s of the refresh
  }
  if (deferred_idr_at_ && now >= deferred_idr_at_) {
    deferred_idr_at_ = 0;
    if (!idr_pending_ && (!have_idr_ || now - last_idr_ >= kClass2Window)) return idr(now);
  }
  return kNone;
}

int64_t SenderRecovery::next_timer() const {
  int64_t t = 0;
  for (int64_t x : {refresh_check_at_, deferred_idr_at_})
    if (x && (!t || x < t)) t = x;
  return t;
}

void SenderRecovery::on_idr_numbered(uint32_t frame, int64_t now) {
  // every AU with idr set counts toward both IDR limits, whatever was requested
  have_idr_ = true;
  last_idr_ = now;
  last_idr_frame_ = frame;
  // the IDR cleared every reference on both sides: forget older sent and verified LTRs
  while (!sent_ltrs_.empty() && sent_ltrs_.front() < frame) sent_ltrs_.pop_front();
  sent_ltrs_.erase(std::remove_if(sent_ltrs_.begin(), sent_ltrs_.end(), [frame](uint32_t f) { return f < frame; }),
                   sent_ltrs_.end());
  verified_.erase(verified_.begin(), verified_.lower_bound(frame));
  failed_refreshes_ = 0;
  refresh_check_at_ = 0;
}

void SenderRecovery::on_idr_first_packet(int64_t now, int64_t pace_ns) {
  idr_pending_ = false;
  pending_until_ = now + rtt_ns_ + pace_ns + 50'000'000;
}

void SenderRecovery::on_ltr_sent(uint32_t frame) {
  if (have_idr_ && frame < last_idr_frame_) return;
  sent_ltrs_.push_back(frame);
  while (sent_ltrs_.size() > 64) sent_ltrs_.pop_front();
}

std::vector<uint32_t> SenderRecovery::on_rr(uint32_t clean_since, uint32_t acked, bool broken, int64_t now) {
  (void)now;
  std::vector<uint32_t> out;
  if (clean_since == kNoFrame || acked == kNoFrame || acked < clean_since) return out;
  if (!clean_seen_ || clean_since != newest_clean_since_) {
    // a newer clean run: the last refresh (or IDR) worked
    if (clean_at_refresh_ == kNoFrame || clean_since != clean_at_refresh_) {
      failed_refreshes_ = 0;
      refresh_check_at_ = 0;
    }
    clean_seen_ = true;
    newest_clean_since_ = clean_since;
  }
  if (!broken) deferred_idr_at_ = 0;
  for (uint32_t f : sent_ltrs_) {
    if (f < clean_since || f > acked) continue;
    if (have_idr_ && f < last_idr_frame_) continue;
    if (verified_.insert(f).second) out.push_back(f);
  }
  while (verified_.size() > 64) verified_.erase(verified_.begin());
  return out;
}

// ================================================================== VideoSendStream

VideoSendStream::VideoSendStream(const Config& cfg, std::shared_ptr<Flow> flow, EventSink* events, FailFn on_fail)
    : StreamBase(cfg.id, "video"), cfg_(cfg), flow_(std::move(flow)), events_(events), on_fail_(std::move(on_fail)) {
  sealer_.init(cfg_.key, cfg_.id & ~kDirBit);
  base_bitrate_ = cfg_.spec.bitrate;
  base_fps_ = cfg_.fps;
  fps_now_ = cfg_.fps;
  bitrate_now_ = cfg_.spec.bitrate;
  fmt_w_ = cfg_.spec.fmt.w;
  fmt_h_ = cfg_.spec.fmt.h;
  pace_x_.store(cfg_.idr_pace_x);
  device_label_ = cfg_.spec.test.empty() ? cfg_.spec.device : "test:" + cfg_.spec.test;
}

VideoSendStream::~VideoSendStream() {
  stop();
  secure_wipe(cfg_.key, sizeof cfg_.key);
}

bool VideoSendStream::start(VideoBackend& backend, std::string* code, std::string* msg) {
  std::string e;
  start_ns_ = now_ns();
  auto cam = backend.open_camera(cfg_.spec, this, &e);
  if (!cam) {
    auto c = e.find(':');
    *code = c == std::string::npos ? "internal" : e.substr(0, c);
    *msg = c == std::string::npos ? e : e.substr(std::min(e.size(), c + 2));
    static const char* known[] = {"no_camera", "permission", "not_installed", "unsupported", "decoder", "internal", "bad_args"};
    if (std::none_of(std::begin(known), std::end(known), [&](const char* k) { return *code == k; })) *code = "internal";
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(cam_mu_);
    cam_ = std::move(cam);
    device_label_ = cam_->device_in_use();
  }
  last_fb_ns_.store(start_ns_);
  thread_ = std::thread([this] { sender_loop(); });
  return true;
}

void VideoSendStream::stop() {
  if (stopped_) return;
  stopped_ = true;
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
  {
    std::lock_guard<std::mutex> lock(cam_mu_);
    cam_.reset();  // the encoder stops now; the backend stops the capture when no other stream uses it
  }
  if (flow_) {
    if (Pacer* p = flow_->pacer_if_any()) p->remove_stream(cfg_.id);
  }
  std::lock_guard<std::mutex> lock(cam_ev_mu_);
  cam_state_["state"] = "stopped";
  cam_dirty_ = true;
}

std::string VideoSendStream::device_in_use() const { return device_label_; }

void VideoSendStream::fail(const std::string& code) {
  if (failed_) return;
  failed_ = true;
  {
    // close the camera at once (the helper then ends the stream)
    std::lock_guard<std::mutex> lock(cam_mu_);
    cam_.reset();
  }
  BA_INFO("video stream %u: %s", cfg_.id, code.c_str());
  if (on_fail_) on_fail_(cfg_.id, code);
}

// ---- encoder thread

uint32_t VideoSendStream::on_frame(const EncodedFrame& f) {
  const int64_t now = now_ns();
  uint32_t number;
  bool drop_now = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    number = next_frame_++;
    if (!have_first_capture_) {
      have_first_capture_ = true;
      first_capture_ns_ = f.capture_ns;
    }
    if (stop_ || failed_) return number;
    bool dev_drop = false;
    if (!f.idr) {
      int n = drop_au_.load();
      while (n > 0 && !drop_au_.compare_exchange_weak(n, n - 1)) {
      }
      dev_drop = n > 0;
    }
    if (dev_drop) {
      // dev video_drop_au: dropped after numbering (a hole the receiver must see)
      dropped_.insert(number);
      drop_pending_ = true;
      dev_dropped_au_.fetch_add(1);
      drop_now = true;
    } else if (q_.size() >= 16 || q_bytes_ + f.len > 4u * 1024 * 1024) {
      // hard cap (defence): drop after numbering; an IDR drops every queued non-IDR instead
      if (f.idr) {
        for (auto it = q_.begin(); it != q_.end();) {
          if (!it->idr) {
            dropped_.insert(it->frame);
            q_bytes_ -= it->data.size();
            q_drop_.fetch_add(1);
            it = q_.erase(it);
          } else {
            ++it;
          }
        }
      } else {
        dropped_.insert(number);
        q_drop_.fetch_add(1);
        drop_pending_ = true;
        drop_now = true;
      }
      if (f.idr) drop_pending_ = false;  // the IDR restarts the chain
    }
    if (!drop_now) {
      Au au;
      au.data.assign(f.data, f.data + f.len);
      au.frame = number;
      au.capture_ns = f.capture_ns;
      au.out_ns = now;
      au.idr = f.idr;
      au.refresh = f.refresh;
      au.ltr = f.ltr;
      au.w = f.w;
      au.h = f.h;
      q_bytes_ += au.data.size();
      q_.push_back(std::move(au));
    }
    while (dropped_.size() > 512) dropped_.erase(dropped_.begin());
  }
  if (f.idr) {
    std::lock_guard<std::mutex> lock(pol_mu_);
    rec_.on_idr_numbered(number, now);
  }
  cv_.notify_all();
  return number;
}

void VideoSendStream::on_camera_event(const std::string& what, const std::string& detail) {
  std::lock_guard<std::mutex> lock(cam_ev_mu_);
  if (what == "running" || what == "interrupted" || what == "resumed" || what == "lost") {
    cam_state_["state"] = what == "resumed" ? "running" : what;
  } else if (what == "native") {
    cam_state_["native"] = detail;
  } else if (what == "fps") {
    cam_state_["fps"] = std::atof(detail.c_str());
  } else if (what == "effects") {
    Json arr = Json::array();
    size_t i = 0;
    while (i < detail.size()) {
      size_t j = detail.find(',', i);
      if (j == std::string::npos) j = detail.size();
      if (j > i) arr.push_back(detail.substr(i, j - i));
      i = j + 1;
    }
    cam_state_["effects"] = arr;
  } else if (what == "by_other") {
    cam_state_["by_other"] = detail == "1" || detail == "true";
  } else if (what == "encoder") {
    auto c = detail.find(':');
    cam_state_["encoder"] = detail.substr(0, c);
    if (c != std::string::npos) cam_state_["encoder_ms"] = std::atof(detail.substr(c + 1).c_str());
  } else if (what == "camera_ms") {
    cam_state_["camera_ms"] = std::atof(detail.c_str());
  } else {
    return;
  }
  cam_dirty_ = true;
}

bool VideoSendStream::take_camera_event(Json* ev) {
  std::lock_guard<std::mutex> lock(cam_ev_mu_);
  if (!cam_dirty_) return false;
  cam_dirty_ = false;
  Json e{{"ev", "camera"}, {"stream", cfg_.id}};
  e["state"] = cam_state_.value("state", "starting");
  e["device"] = device_label_.empty() ? Json(nullptr) : Json(device_label_);
  e["native"] = cam_state_.contains("native") ? cam_state_["native"] : Json(nullptr);
  e["format"] = std::to_string(fmt_w_.load()) + "x" + std::to_string(fmt_h_.load()) + "@" + std::to_string(fps_now_.load());
  e["fps"] = cam_state_.value("fps", 0.0);
  e["by_other"] = cam_state_.contains("by_other") ? cam_state_["by_other"] : Json(nullptr);
  e["effects"] = cam_state_.contains("effects") ? cam_state_["effects"] : Json::array();
  const std::string st = e["state"];
  if (st == "starting" || st == "running") {
    for (const char* k : {"encoder", "encoder_ms", "camera_ms"})
      if (cam_state_.contains(k)) e[k] = cam_state_[k];
  }
  *ev = e;
  return true;
}

// ---- pacer thread

void VideoSendStream::on_paced(const PacedPacket& p, int64_t now) {
  if (p.idr && p.first && !p.retx) {
    idr_first_at_.store(now);
    cv_.notify_all();
  }
  if (first_tx_ns_.load(std::memory_order_relaxed) == 0) first_tx_ns_.store(now);
  tx_pkts_.fetch_add(1, std::memory_order_relaxed);
  tx_bytes_.fetch_add(p.bytes->size() + 28, std::memory_order_relaxed);
}

// ---- network thread

OpenResult VideoSendStream::ingest_feedback(const uint8_t* data, size_t len, int64_t now) {
  Header h;
  uint8_t payload[kMaxPacket];
  size_t plen = 0;
  OpenResult r = open_packet(cfg_.key, cfg_.id | kDirBit, &fb_window_, data, len, &h, payload, &plen);
  if (r != OpenResult::Ok) return r;
  last_fb_ns_.store(now, std::memory_order_relaxed);
  if (h.ptype == kPli) last_pli_ns_.store(now, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (mail_.size() < 256) mail_.push_back(Mail{h.ptype, std::vector<uint8_t>(payload, payload + plen), now});
  }
  cv_.notify_all();
  return r;
}

// ---- control thread

void VideoSendStream::configure(int w, int h, int fps, int bitrate) {
  std::lock_guard<std::mutex> lock(mu_);
  Mail m;
  m.ptype = 0xF0;  // internal: configure
  m.payload.resize(16);
  put_u32(m.payload.data(), uint32_t(w));
  put_u32(m.payload.data() + 4, uint32_t(h));
  put_u32(m.payload.data() + 8, uint32_t(fps));
  put_u32(m.payload.data() + 12, uint32_t(bitrate));
  m.at = now_ns();
  mail_.push_back(std::move(m));
  cv_.notify_all();
}

void VideoSendStream::link_control(const bool* paused, const std::vector<uint32_t>& dropped) {
  std::lock_guard<std::mutex> lock(mu_);
  if (paused) {
    Mail m;
    m.ptype = 0xF1;  // internal: link pause
    m.payload.push_back(*paused ? 1 : 0);
    m.at = now_ns();
    mail_.push_back(std::move(m));
  }
  if (!dropped.empty()) {
    for (uint32_t f : dropped) dropped_.insert(f);
    drop_pending_ = true;
  }
  cv_.notify_all();
}

// ---- sender thread

int VideoSendStream::effective_bitrate() const {
  if (link_state_) return std::min(cfg_.link_bitrate, base_bitrate_);
  return std::max(1, base_bitrate_ >> step_.load());
}

void VideoSendStream::act(SenderRecovery::Action a) {
  if (a == SenderRecovery::kNone) return;
  std::lock_guard<std::mutex> lock(cam_mu_);
  if (!cam_) return;
  switch (a) {
    case SenderRecovery::kStart: cam_->start_encoding(); break;
    case SenderRecovery::kIdr: cam_->request_idr(); break;
    case SenderRecovery::kRefresh: cam_->request_ltr_refresh(); break;
    default: break;
  }
}

void VideoSendStream::handle_mail(const Mail& m, int64_t now) {
  if (m.ptype == kPli) {
    Pli p;
    if (!read_pli(m.payload.data(), m.payload.size(), &p)) return;
    pli_rx_.fetch_add(1);
    SenderRecovery::Action a;
    {
      std::lock_guard<std::mutex> lock(pol_mu_);
      const double rtt = flow_->rtt_ms();
      rec_.set_rtt_ns(rtt >= 0 ? int64_t(rtt * 1e6) : 0);
      a = rec_.on_pli(p, now);
    }
    act(a);
  } else if (m.ptype == kNack) {
    std::vector<NackEntry> e;
    if (!read_nack(m.payload.data(), m.payload.size(), &e)) return;
    nack_rx_.fetch_add(1);
    std::vector<uint64_t> seqs;
    unpack_nack(e, &seqs);
    std::set<uint32_t> dropped;
    {
      std::lock_guard<std::mutex> lock(mu_);
      dropped = dropped_;
    }
    std::vector<PacedPacket> out;
    for (uint64_t s : seqs) {
      if (hist_.empty() || s < hist_.front().seq || s > hist_.back().seq) continue;
      const Hist& hx = hist_[size_t(s - hist_.front().seq)];
      if (hx.seq != s || dropped.count(hx.frame)) continue;
      PacedPacket p;
      p.bytes = hx.bytes;
      p.stream = cfg_.id;
      p.frame = hx.frame;
      p.end = true;
      out.push_back(std::move(p));
    }
    retx_.fetch_add(out.size());
    if (!out.empty()) flow_->pacer()->enqueue_retx(std::move(out));
  } else if (m.ptype == kRR) {
    VideoRR rr;
    if (!read_video_rr(m.payload.data(), m.payload.size(), &rr)) return;
    rr_rx_.fetch_add(1);
    std::vector<uint32_t> ver;
    {
      std::lock_guard<std::mutex> lock(pol_mu_);
      ver = rec_.on_rr(rr.clean_since, rr.acked, rr.flags & 1, now);
      verified_newest_ = rec_.newest_verified();
    }
    {
      std::lock_guard<std::mutex> lock(cam_mu_);
      if (cam_)
        for (uint32_t f : ver) cam_->acknowledge_ltr(f);
    }
    // safety step-down (§4.10): over the last 2 s, lost > 2 % of frames or late > 10 %
    const uint64_t fr = frames_.load();
    rr_win_.push_back(RrSample{now, rr.lost, rr.late, fr});
    while (!rr_win_.empty() && now - rr_win_.front().at > 2'000'000'000LL) rr_win_.pop_front();
    if (!link_state_ && rr_win_.size() >= 2 && now - rr_win_.front().at >= 1'500'000'000LL) {
      const RrSample& a = rr_win_.front();
      const double frames = double(fr - a.frames);
      const double lost = double(uint32_t(rr.lost - a.lost)), late = double(uint16_t(rr.late - uint16_t(a.late)));
      if (frames >= 10 && (lost > 0.02 * frames || late > 0.10 * frames)) {
        if (step_ < 2) {
          step_++;
          BA_INFO("video stream %u: rate step down to level %d", cfg_.id, step_.load());
          std::lock_guard<std::mutex> lock(cam_mu_);
          if (cam_) cam_->set_bitrate(effective_bitrate());
        }
        step_since_ = now;
        rr_win_.clear();
      }
    }
  } else if (m.ptype == 0xF0 && m.payload.size() >= 16) {
    const int w = int(get_u32(m.payload.data())), h = int(get_u32(m.payload.data() + 4));
    const int fps = int(get_u32(m.payload.data() + 8)), br = int(get_u32(m.payload.data() + 12));
    std::lock_guard<std::mutex> lock(cam_mu_);
    if (!cam_) return;
    if ((w && w != cfg_.spec.fmt.w) || (h && h != cfg_.spec.fmt.h)) {
      cfg_.spec.fmt.w = w ? w : cfg_.spec.fmt.w;
      cfg_.spec.fmt.h = h ? h : cfg_.spec.fmt.h;
      fmt_w_ = cfg_.spec.fmt.w;
      fmt_h_ = cfg_.spec.fmt.h;
      cam_->set_format(cfg_.spec.fmt);  // the next AU is an IDR at that size (one paced IDR)
    }
    if (fps) {
      base_fps_ = fps;
      if (!link_state_) {
        fps_now_ = fps;
        cam_->set_fps(fps);
      }
    }
    if (br) {
      base_bitrate_ = br;
      bitrate_now_ = effective_bitrate();
      cam_->set_bitrate(bitrate_now_);
    }
  } else if (m.ptype == 0xF1 && !m.payload.empty()) {
    paused_link_ = m.payload[0] != 0;
  }
}

void VideoSendStream::update_pause(int64_t now) {
  // backpressure (§4.3): backlog = AUs queued + packets waiting in the pacer, at the IDR release rate
  size_t qn, qb;
  {
    std::lock_guard<std::mutex> lock(mu_);
    qn = q_.size();
    qb = q_bytes_;
  }
  const size_t pb = flow_->pacer()->queued_bytes(cfg_.id);
  const double rate = std::max(1.0, pace_x_.load() * effective_bitrate() / 8.0);  // bytes/s
  const double backlog_ms = double(qb + pb) / rate * 1000.0;
  if (!paused_bp_ && (backlog_ms > 250 || qn >= 6)) paused_bp_ = true;
  else if (paused_bp_ && backlog_ms < 100 && qn < 2) paused_bp_ = false;
  const bool want = paused_bp_ || paused_link_;
  if (want != paused_applied_) {
    paused_applied_ = want;
    if (want) paused_since_ = now;
    else paused_ns_.fetch_add(uint64_t(now - paused_since_));
    std::lock_guard<std::mutex> lock(cam_mu_);
    if (cam_) cam_->set_input_paused(want);
  }
}

void VideoSendStream::send_au(Au& au, int64_t now) {
  const auto me = std::this_thread::get_id();
  if (seal_thread_ == std::thread::id()) seal_thread_ = me;
  else if (seal_thread_ != me) g_video_seal_thread_violations.fetch_add(1);
  Pacer* pacer = flow_->pacer();
  const uint32_t ts = video_ts(au.capture_ns, first_capture_ns_);
  tm_.publish(ts, static_cast<int64_t>(au.capture_ns));
  const size_t n = std::max<size_t>(1, (au.data.size() + kMaxFragment - 1) / kMaxFragment);
  VideoHeader vh;
  vh.frame = au.frame;
  vh.nfrag = static_cast<uint16_t>(n);
  vh.vflags = (au.idr ? kVIdr : 0) | (au.refresh ? kVRefresh : 0) | (au.ltr ? kVLtr : 0);
  if (!au.idr && pacer->idr_queued(cfg_.id)) vh.vflags |= kVBehindIdr;
  vh.ltr = au.ltr ? 1 : 0;
  vh.enc_delay = enc_delay_units(au.out_ns - static_cast<int64_t>(au.capture_ns));
  enc_delay_sum_us_.fetch_add(uint64_t(std::max<int64_t>(0, au.out_ns - int64_t(au.capture_ns)) / 1000));
  const double idr_bps = pace_x_.load() * effective_bitrate();
  std::vector<PacedPacket> pk;
  pk.reserve(n);
  uint8_t plain[kVideoHeaderLen + kMaxFragment];
  for (size_t i = 0; i < n; ++i) {
    vh.frag = static_cast<uint16_t>(i);
    write_video_header(vh, plain);
    const size_t off = i * kMaxFragment, len = std::min(kMaxFragment, au.data.size() - off);
    std::memcpy(plain + kVideoHeaderLen, au.data.data() + off, len);
    Header h;
    h.ptype = kMedia;
    h.codec = kCodecH264;
    h.stream = cfg_.id;
    h.ts = ts;
    if (first_packet_) {
      h.flags = kFlagMarker;
      first_packet_ = false;
    }
    auto buf = std::make_shared<std::vector<uint8_t>>(kHeaderLen + kVideoHeaderLen + len + kTagLen);
    const size_t sl = sealer_.seal(h, plain, kVideoHeaderLen + len, buf->data());
    buf->resize(sl);
    hist_.push_back(Hist{sealer_.last_seq(), au.frame, now, buf});
    PacedPacket p;
    p.bytes = buf;
    p.stream = cfg_.id;
    p.frame = au.frame;
    p.idr = au.idr;
    p.first = i == 0;
    p.end = i + 1 == n;
    p.idr_bps = au.idr ? idr_bps : 0;
    pk.push_back(std::move(p));
  }
  while (!hist_.empty() && (hist_.size() > 2048 || now - hist_.front().at > 1'000'000'000LL)) hist_.pop_front();
  if (au.ltr) {
    std::lock_guard<std::mutex> lock(pol_mu_);
    rec_.on_ltr_sent(au.frame);
  }
  if (au.idr) {
    idr_.fetch_add(1);
    idr_first_bytes_.store(au.data.size());
  }
  if (au.refresh) refresh_.fetch_add(1);
  frames_.fetch_add(1);
  pacer->enqueue(std::move(pk));
  if (!running_) {
    running_ = true;
    events_->emit(Json{{"ev", "stream_state"}, {"stream", cfg_.id}, {"state", "running"},
                       {"device", device_label_.empty() ? Json(nullptr) : Json(device_label_)}});
  }
}

void VideoSendStream::sender_loop() {
  set_thread_name("beamer-audio-vsend");
  set_thread_interactive();
  flow_->pacer();  // created with the first video send stream
  int64_t last_idr_seen = 0;
  for (;;) {
    std::deque<Mail> mail;
    std::deque<Au> aus;
    bool drop_rec = false;
    int64_t now = now_ns();
    {
      std::unique_lock<std::mutex> lock(mu_);
      int64_t wake = now + 10'000'000;
      {
        std::lock_guard<std::mutex> pl(pol_mu_);
        const int64_t t = rec_.next_timer();
        if (t) wake = std::min(wake, t);
        if (!rec_.started()) wake = std::min(wake, start_ns_ + cfg_.no_pli_ns);
        else wake = std::min(wake, last_fb_ns_.load() + cfg_.no_feedback_ns);
      }
      if (q_.empty() && mail_.empty() && !drop_pending_ && !stop_ && idr_first_at_.load() == last_idr_seen)
        cv_.wait_until(lock, std::chrono::steady_clock::time_point(std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                 std::chrono::nanoseconds(wake))));
      if (stop_) return;
      const int64_t stall = stall_until_.load();
      if (stall > now_ns()) {
        lock.unlock();
        if (stall_pause_.load()) update_pause(now_ns());
        sleep_ns(std::min<int64_t>(stall - now_ns(), 5'000'000));
        continue;
      }
      mail.swap(mail_);
      aus.swap(q_);
      q_bytes_ = 0;
      drop_rec = drop_pending_;
      drop_pending_ = false;
    }
    now = now_ns();
    if (failed_) {
      sleep_ns(5'000'000);
      continue;
    }
    // the first packet of an IDR left the pacer: end of the "pending" state, start of the coalescing window
    const int64_t first_at = idr_first_at_.load();
    if (first_at != last_idr_seen) {
      last_idr_seen = first_at;
      const double bps = pace_x_.load() * effective_bitrate();
      const int64_t pace_ns = bps > 0 ? int64_t(double(idr_first_bytes_.load()) * 8.0 / bps * 1e9) : 0;
      std::lock_guard<std::mutex> lock(pol_mu_);
      rec_.on_idr_first_packet(first_at, pace_ns);
    }
    for (auto& m : mail) handle_mail(m, now);
    // camera-on guards (§6.5): no PLI within 3 s of `send`, or no feedback for 5 s once running
    bool started;
    {
      std::lock_guard<std::mutex> lock(pol_mu_);
      started = rec_.started();
    }
    if ((!started && now - start_ns_ >= cfg_.no_pli_ns) || (started && now - last_fb_ns_.load() >= cfg_.no_feedback_ns)) {
      fail("no_receiver");
      continue;
    }
    // link state: 1 Mbit/s and 15 fps at the current size, video only through the link (§4.10)
    const bool link = flow_->link_mode();
    if (link != link_state_) {
      link_state_ = link;
      fps_now_ = link ? cfg_.link_fps : base_fps_;
      bitrate_now_ = effective_bitrate();
      std::lock_guard<std::mutex> lock(cam_mu_);
      if (cam_) {
        cam_->set_bitrate(bitrate_now_);
        cam_->set_fps(fps_now_);
      }
    }
    // step back up after 10 s clean
    if (step_ > 0 && !link_state_ && now - step_since_ > 10'000'000'000LL) {
      step_--;
      step_since_ = now;
      std::lock_guard<std::mutex> lock(cam_mu_);
      if (cam_) cam_->set_bitrate(effective_bitrate());
    }
    SenderRecovery::Action a;
    {
      std::lock_guard<std::mutex> lock(pol_mu_);
      a = rec_.on_timer(now);
      if (a == SenderRecovery::kNone && drop_rec) a = rec_.on_drop(now);
    }
    act(a);
    for (auto& au : aus) send_au(au, now);
    update_pause(now);
  }
}

Json VideoSendStream::stats(double dt) {
  const uint64_t fr = frames_.load(), bytes = tx_bytes_.load(), es = enc_delay_sum_us_.load();
  Json j{{"dir", "send"}, {"kind", "video"}, {"path", flow_->link_mode() ? "link" : "udp"}};
  j["kbps"] = dt > 0 ? std::round((bytes - last_bytes_) * 8.0 / dt / 100.0) / 10.0 : 0.0;
  j["bitrate"] = bitrate_now_.load();
  j["fps"] = dt > 0 ? std::round((fr - last_frames_) / dt * 10) / 10 : 0.0;
  j["frames"] = fr;
  j["idr"] = idr_.load();
  j["refresh"] = refresh_.load();
  j["nack_rx"] = nack_rx_.load();
  j["retx"] = retx_.load();
  j["pli_rx"] = pli_rx_.load();
  j["q_drop"] = q_drop_.load();
  j["dev_drop_au"] = dev_dropped_au_.load();
  uint64_t pn = paused_ns_.load();
  j["paused_ms"] = (pn - last_paused_ns_) / 1'000'000;
  last_paused_ns_ = pn;
  Pacer* p = flow_->pacer_if_any();
  if (p) {
    const uint64_t w = p->wait_ns_sum.load(), n = p->n_sent.load();
    j["pace_ms"] = n > last_pace_n_ ? std::round(double(w - last_pace_wait_) / double(n - last_pace_n_) / 1e4) / 100 : 0.0;
    last_pace_wait_ = w;
    last_pace_n_ = n;
  }
  j["enc_ms"] = fr > last_frames_ ? std::round(double(es - last_enc_sum_) / double(fr - last_frames_) / 10.0) / 100.0 : 0.0;
  j["cam_fps"] = j["fps"];
  {
    std::lock_guard<std::mutex> lock(cam_ev_mu_);
    if (cam_state_.contains("fps")) j["cam_fps"] = cam_state_["fps"];
  }
  last_frames_ = fr;
  last_bytes_ = bytes;
  last_enc_sum_ = es;
  j["step"] = step_.load();
  j["link_paused"] = paused_link_.load();
  const double rtt = flow_->rtt_ms();
  j["rtt_ms"] = rtt >= 0 ? Json(std::round(rtt * 10) / 10) : Json(nullptr);
  const uint32_t vn = verified_newest_.load();
  j["verified_ltr"] = vn == kNoFrame ? Json(nullptr) : Json(vn);
  j["rr_rx"] = rr_rx_.load();
  {
    std::lock_guard<std::mutex> lock(pol_mu_);
    const auto& c = rec_.counters();
    j["pli_dup"] = c.dup;
    j["pli_window_drop"] = c.dropped_window;
  }
  return j;
}

}  // namespace beamer::audio
