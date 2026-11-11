// SPDX-License-Identifier: GPL-3.0-only
#include "video_recv.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

#include "flow.h"
#include "util.h"

namespace beamer::audio {

namespace {
constexpr int64_t kNever = INT64_MAX;
constexpr int64_t kDisplayTickNs = 33'333'333;
double r1(double v) { return std::round(v * 10) / 10; }
}  // namespace

std::atomic<uint64_t> g_video_seal_thread_violations{0};

// ================================================================== VideoRecvCore

VideoRecvCore::VideoRecvCore(uint32_t stream, const uint8_t key[kKeyLen], int fps, int w, int h)
    : stream_(stream), fps_(fps > 0 ? fps : 30), w_(w), h_(h) {
  std::memcpy(key_, key, kKeyLen);
  fb_sealer_.init(key_, stream_ | kDirBit);
  period_ns_ = 1'000'000'000LL / fps_;
}

VideoRecvCore::~VideoRecvCore() { secure_wipe(key_, sizeof key_); }

void VideoRecvCore::set_floor_ms(double f) {
  std::lock_guard<std::mutex> lock(mu_);
  playout_.set_floor_ms(f);
}

void VideoRecvCore::seal_feedback(uint8_t ptype, const uint8_t* p, size_t n, std::vector<std::vector<uint8_t>>* out) {
  const auto me = std::this_thread::get_id();
  if (fb_thread_ == std::thread::id()) fb_thread_ = me;
  else if (fb_thread_ != me) g_video_seal_thread_violations.fetch_add(1);
  Header h;
  h.ptype = ptype;
  h.stream = stream_;
  std::vector<uint8_t> pkt(kHeaderLen + n + kTagLen);
  pkt.resize(fb_sealer_.seal(h, p, n, pkt.data()));
  out->push_back(std::move(pkt));
}

int64_t VideoRecvCore::offset_now(int64_t now, bool* ok) {
  int64_t off = 0, age = 0;
  if (offset_source && offset_source(now, &off, &age)) {
    *ok = true;
    provisional_ = false;  // replaced by the first pong estimate
    return off;
  }
  *ok = provisional_;
  return prov_offset_;
}

bool VideoRecvCore::mapped_out(const Frame& f, uint32_t frame, int64_t now, int64_t* out_ns) {
  (void)frame;
  if (!map_.valid()) return false;
  bool ok;
  const int64_t off = offset_now(now, &ok);
  if (!ok) return false;
  *out_ns = map_.to_sender_ns(f.ts) + int64_t(f.enc_delay) * 100'000 - off;
  return true;
}

int64_t VideoRecvCore::frame_period_ns() const { return period_ns_; }

int64_t VideoRecvCore::present_est(uint32_t frame, int64_t now) {
  const double hold = lipsync ? lipsync->hold_ms() : 0.0;
  auto it = frames_.find(frame);
  if (it != frames_.end() && it->second.nfrag) {
    int64_t out;
    if (mapped_out(it->second, frame, now, &out)) return out + int64_t((d_now_ms_ + hold) * 1e6);
    return it->second.first_ns + int64_t(PlayoutTarget::kInitialMs * 1e6);  // bootstrap: arrival + 70 ms
  }
  // unseen: the next seen frame's present time minus one frame period per missing number
  auto nx = frames_.upper_bound(frame);
  while (nx != frames_.end() && !nx->second.nfrag) ++nx;
  if (nx == frames_.end()) return kNever;
  const int64_t pt = present_est(nx->first, now);
  if (pt == kNever) return kNever;
  return pt - frame_period_ns() * int64_t(nx->first - frame);
}

int64_t VideoRecvCore::present_time_of(uint32_t frame) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = ready_.find(frame);
  return it == ready_.end() ? 0 : it->second;
}

void VideoRecvCore::request_pli_now(uint8_t reason, int64_t now) {
  pli_reason_now_ = reason;
  next_pli_ = now;
  wake_net_ = true;
}

void VideoRecvCore::go_broken(uint8_t reason, int64_t now) {
  // `stalled`: the frames were lost because no packet at all arrived for >= 500 ms (a Wi-Fi stall)
  if (reason == kPliLoss && stall_end_ns_ && now - stall_end_ns_ < 1'000'000'000LL) reason = kPliStalled;
  const bool was = broken_;
  chain_clean_ = false;
  broken_ = true;
  broken_reason_ = reason;
  if (!was && !idr_wanted_ && started_) request_pli_now(reason, now);
}

void VideoRecvCore::revoke_from(uint32_t frame, std::vector<SinkAction>* acts) {
  for (auto it = frames_.lower_bound(frame); it != frames_.end(); ++it) {
    if (it->second.handed && it->second.keep) {
      it->second.keep = false;
      it->second.revoked = true;
    }
  }
  for (auto it = ready_.lower_bound(frame); it != ready_.end();) {
    acts->push_back(SinkAction{SinkAction::kDrop, it->first, false, nullptr, ""});
    skipped_++;
    it = ready_.erase(it);
  }
}

void VideoRecvCore::need_idr(uint8_t reason, int64_t now) {
  started_ = false;
  if (idr_wanted_) return;  // coalesced: the pending request (and its IDR, maybe already arriving) answers it
  idr_wanted_ = true;
  idr_reason_ = reason;
  idr_after_valid_ = have_any_;
  idr_after_ = newest_seen_;
  chain_clean_ = false;
  broken_ = true;
  run_start_ = kNoFrame;
  request_pli_now(reason, now);
}

void VideoRecvCore::mark_lost(uint32_t frame, bool hole, int64_t now) {
  (void)frame;
  lost_++;
  if (hole) holes_++;
  go_broken(kPliLoss, now);
}

void VideoRecvCore::complete_frame(uint32_t frame, Frame& f, int64_t now) {
  f.complete = true;
  f.complete_ns = now;
  if (!map_.valid()) return;
  bool have_off;
  int64_t off = 0, age = 0;
  have_off = offset_source && offset_source(now, &off, &age);
  if (!have_off && !(f.vflags & (kVIdr | kVBehindIdr)) && prov_samples_ < 30) {
    // no pong yet (stream opened in link state): provisional offset assuming d = 20 ms (§4.5.3):
    // offset = sender_ns + enc_delay + 20 ms - completion. Taken from frames that measure the network (not an
    // IDR, nor one behind it: their delay is the deliberate pacing, ~240 ms on the link), and refined over the
    // first 30 of them by keeping the least delayed one (the largest estimate), like a min-RTT pick.
    const int64_t est = map_.to_sender_ns(f.ts) + int64_t(f.enc_delay) * 100'000 + 20'000'000 - now;
    prov_offset_ = provisional_ ? std::max(prov_offset_, est) : est;
    provisional_ = true;
    prov_samples_++;
  }
  int64_t out;
  if (!mapped_out(f, frame, now, &out)) return;
  const double d = double(now - out) * 1e-6;
  if (!(f.vflags & (kVIdr | kVBehindIdr))) {
    playout_.add(now, d);
    d_samples_++;
  }
  const double hold = lipsync ? lipsync->hold_ms() : 0.0;
  const int64_t margin = out + int64_t((d_now_ms_ + hold) * 1e6) - now;
  const int64_t m01 = std::max<int64_t>(0, margin / 100'000);
  if (margin_min_01ms_ < 0 || m01 < margin_min_01ms_) margin_min_01ms_ = m01;
}

OpenResult VideoRecvCore::ingest(const uint8_t* data, size_t len, int64_t now, bool via_link,
                                 std::vector<std::vector<uint8_t>>* out) {
  (void)via_link;
  Header h;
  uint8_t payload[kMaxPacket];
  size_t plen = 0;
  std::lock_guard<std::mutex> lock(mu_);
  OpenResult r = open_packet(key_, stream_ & ~kDirBit, &window_, data, len, &h, payload, &plen);
  if (r != OpenResult::Ok) return r;
  VideoHeader vh;
  if (h.codec != kCodecH264 || !read_video_header(payload, plen, &vh)) return OpenResult::Rejected;
  rx_pkts_++;
  rx_bytes_ += len + 28;
  rx_bytes_iv_ += len + 28;
  if (last_pkt_ns_ && now - last_pkt_ns_ >= timing.stall_ns) stall_end_ns_ = now;
  last_pkt_ns_ = now;
  last_media_ns_ = now;
  if (rx_pkts_ == 1 || ts_diff(h.ts, newest_ts_) > 0) newest_ts_ = h.ts;
  // a NACK repaired this one
  auto nk = nacks_.find(h.seq);
  if (nk != nacks_.end()) {
    nacks_.erase(nk);
    retx_ok_++;
  }
  // gap -> NACK at once (reorder was 0 in every C3 run)
  if (hi_seq_ != 0 && h.seq > hi_seq_ + 1) {
    const uint64_t gap = h.seq - hi_seq_ - 1;
    const uint32_t lo = last_pkt_end_ ? last_pkt_frame_ + 1 : last_pkt_frame_;
    uint32_t hi = vh.frag == 0 && vh.frame > 0 ? vh.frame - 1 : vh.frame;
    if (hi < lo) hi = lo;
    if (gap > 64) {
      for (uint32_t f = lo; f <= hi && f - lo < 256; ++f) lost_evidence_.insert(f);  // hole > 64: give up at once
    } else {
      for (uint64_t s = hi_seq_ + 1; s < h.seq; ++s) {
        nacks_[s] = Nack{1, now + 15'000'000, lo, hi};
        nack_now_.push_back(s);
      }
    }
  }
  if (h.seq > hi_seq_) {
    hi_seq_ = h.seq;
    last_pkt_frame_ = vh.frame;
    last_pkt_end_ = vh.frag + 1 == vh.nfrag;
  }
  // the first packet of an IDR newer than the request ends the start/size repeats
  if (idr_wanted_ && (vh.vflags & kVIdr) && (!idr_after_valid_ || vh.frame > idr_after_)) {
    idr_wanted_ = false;
    next_pli_ = 0;
  }
  if (!have_any_ || vh.frame > newest_seen_) newest_seen_ = vh.frame;
  have_any_ = true;
  if (!(started_ && vh.frame < next_dec_)) {  // else: already decoded or given up (a repair too late)
    if (frames_.size() >= 160 && !frames_.count(vh.frame)) frames_.erase(frames_.begin());
    Frame& f = frames_[vh.frame];
    if (f.nfrag == 0) {
      f.nfrag = vh.nfrag;
      f.frags.resize(vh.nfrag);
      f.have.assign(vh.nfrag, false);
      f.ts = h.ts;
      f.vflags = vh.vflags;
      f.enc_delay = vh.enc_delay;
      f.first_ns = now;
      // frame period from consecutive numbers (15 fps in link state)
      auto prev = frames_.find(vh.frame - 1);
      if (vh.frame > 0 && prev != frames_.end() && prev->second.nfrag) {
        const int64_t d = int64_t(ts_diff(h.ts, prev->second.ts)) * 1'000'000'000LL / 90000;
        if (d >= 10'000'000 && d <= 200'000'000) period_ns_ = (period_ns_ * 4 + d) / 5;
      }
    }
    if (f.nfrag == vh.nfrag && !f.complete && !f.have[vh.frag]) {
      f.have[vh.frag] = true;
      f.frags[vh.frag].assign(payload + kVideoHeaderLen, payload + plen);
      f.got++;
      if (h.seq > f.max_seq) f.max_seq = h.seq;
      if (f.got == f.nfrag) complete_frame(vh.frame, f, now);
    } else if (h.seq > f.max_seq && f.nfrag == vh.nfrag) {
      f.max_seq = h.seq;
    }
  }
  // NACKs are built and sealed in the same network-thread pass that detects the gap
  if (!nack_now_.empty()) {
    std::sort(nack_now_.begin(), nack_now_.end());
    size_t i = 0;
    while (i < nack_now_.size()) {
      std::vector<uint64_t> rest(nack_now_.begin() + long(i), nack_now_.end());
      std::vector<NackEntry> e;
      const size_t used = pack_nack(rest, &e);
      uint8_t buf[1 + kMaxNackEntries * 10];
      const size_t n = write_nack(e, buf);
      seal_feedback(kNack, buf, n, out);
      nack_tx_++;
      i += used;
    }
    nack_now_.clear();
  }
  return r;
}

void VideoRecvCore::on_time_map(uint32_t ts, uint64_t sender_ns) {
  std::lock_guard<std::mutex> lock(mu_);
  map_.set(ts, sender_ns);
}

void VideoRecvCore::net_tick(int64_t now, double rtt_ms, std::vector<std::vector<uint8_t>>* out) {
  std::lock_guard<std::mutex> lock(mu_);
  // NACK retries: after max(2 x RTT, 15 ms), <= 3 tries, only while the frame's present time has not passed
  const int64_t retry = std::max<int64_t>(15'000'000, rtt_ms > 0 ? int64_t(2 * rtt_ms * 1e6) : 0);
  std::vector<uint64_t> resend;
  for (auto it = nacks_.begin(); it != nacks_.end();) {
    Nack& n = it->second;
    if (now < n.next) {
      ++it;
      continue;
    }
    if (n.tries >= 3) {
      for (uint32_t f = n.lo; f <= n.hi && f - n.lo < 256; ++f) lost_evidence_.insert(f);  // NACK gave up
      it = nacks_.erase(it);
      continue;
    }
    const int64_t pt = present_est(n.lo, now);
    if (pt != kNever && now >= pt) {
      it = nacks_.erase(it);  // too late to repair: the present-time rule decides
      continue;
    }
    n.tries++;
    n.next = now + retry;
    resend.push_back(it->first);
    ++it;
  }
  size_t i = 0;
  while (i < resend.size()) {
    std::vector<uint64_t> rest(resend.begin() + long(i), resend.end());
    std::vector<NackEntry> e;
    i += pack_nack(rest, &e);
    uint8_t buf[1 + kMaxNackEntries * 10];
    seal_feedback(kNack, buf, write_nack(e, buf), out);
    nack_tx_++;
  }
  // PLI
  bool want = false;
  uint8_t reason = kPliStart;
  int64_t iv = timing.start_pli_ns;
  if (failed_) {
    want = false;
  } else if (idr_wanted_) {
    want = true;
    reason = idr_reason_;
  } else if (started_ && broken_ && run_start_ == kNoFrame) {
    want = true;
    reason = broken_reason_;
    iv = timing.broken_pli_ns;
  }
  if (!want) {
    next_pli_ = 0;
  } else {
    if (next_pli_ == 0) next_pli_ = now;
    if (now >= next_pli_) {
      Pli p;
      p.last_clean = acked_;
      p.reason = reason;
      uint8_t b[kPliLen];
      write_pli(p, b);
      seal_feedback(kPli, b, sizeof b, out);
      pli_tx_++;
      next_pli_ = now + iv;
    }
  }
  // video RR every 50 ms, regardless of what this side sends, and at once for a new clean run
  if (next_rr_ == 0) next_rr_ = now;
  if (rr_now_ || now >= next_rr_) {
    VideoRR rr;
    rr.newest_ts = newest_ts_;
    rr.margin = margin_min_01ms_ < 0 ? 0xFFFF : static_cast<uint16_t>(std::min<int64_t>(65535, margin_min_01ms_));
    rr.hi_seq_lo = static_cast<uint32_t>(hi_seq_);
    rr.lost = static_cast<uint32_t>(lost_);
    rr.late = static_cast<uint16_t>(late_);
    rr.clean_since = clean_since_;
    rr.acked = acked_;
    const double iv_s = rr_iv_start_ ? double(now - rr_iv_start_) * 1e-9 : 0.05;
    rr.recv_kbps = static_cast<uint16_t>(std::min(65535.0, iv_s > 0 ? double(rx_bytes_iv_) * 8.0 / iv_s / 1000.0 : 0.0));
    rr.flags = broken_ ? 1 : 0;
    uint8_t b[kVideoRRLen];
    write_video_rr(rr, b);
    seal_feedback(kRR, b, sizeof b, out);
    rr_tx_++;
    // absolute schedule (a late tick does not push every later RR back); an immediate RR restarts it
    if (rr_now_ || now - next_rr_ > timing.rr_ns) next_rr_ = now + timing.rr_ns;
    else next_rr_ += timing.rr_ns;
    rr_now_ = false;
    margin_min_01ms_ = -1;
    rx_bytes_iv_ = 0;
    rr_iv_start_ = now;
  }
  wake_net_ = false;
}

int64_t VideoRecvCore::next_net_deadline() const {
  std::lock_guard<std::mutex> lock(mu_);
  int64_t d = next_rr_ ? next_rr_ : 0;
  auto upd = [&d](int64_t t) {
    if (t && (d == 0 || t < d)) d = t;
  };
  if (!failed_ && (idr_wanted_ || (started_ && broken_ && run_start_ == kNoFrame))) upd(next_pli_);
  for (auto& kv : nacks_) upd(kv.second.next);
  if (rr_now_ || wake_net_) return 1;  // now
  return d;
}

bool VideoRecvCore::media_recent(int64_t now) const {
  std::lock_guard<std::mutex> lock(mu_);
  return last_media_ns_ != 0 && now - last_media_ns_ < 1'000'000'000;
}

void VideoRecvCore::start(int64_t now, std::vector<SinkAction>* acts) {
  std::lock_guard<std::mutex> lock(mu_);
  shown_ = "starting";
  acts->push_back(SinkAction{SinkAction::kPlaceholder, 0, false, nullptr, "starting"});
  idr_wanted_ = true;
  idr_reason_ = kPliStart;
  request_pli_now(kPliStart, now);
  next_rr_ = now + timing.rr_ns;
  last_tick_ = now;
}

void VideoRecvCore::on_decoded(uint32_t frame, bool ok, double ms, int64_t now) {
  std::lock_guard<std::mutex> lock(mu_);
  dec_n_++;
  dec_ms_sum_ += ms;
  auto it = frames_.find(frame);
  if (it == frames_.end() || !it->second.handed || it->second.decoded) return;  // reset meanwhile
  Frame& f = it->second;
  f.decoded = true;
  f.ok = ok;
  f.decoded_ns = now;
  if (!ok) {
    // revoke this and every later kept picture; broken, PLI decoder; 3 errors in 10 s -> reopen -> start
    f.keep = false;
    f.revoked = true;
    std::vector<SinkAction> drops;
    revoke_from(frame, &drops);
    pending_.insert(pending_.end(), drops.begin(), drops.end());
    dec_errors_.push_back(now);
    while (!dec_errors_.empty() && now - dec_errors_.front() > 10'000'000'000LL) dec_errors_.pop_front();
    if (dec_errors_.size() >= 3) {
      dec_errors_.clear();
      need_idr(kPliStart, now);
    } else {
      if (run_start_ != kNoFrame && run_start_ >= frame) run_start_ = kNoFrame;
      chain_clean_ = false;
      broken_ = true;
      broken_reason_ = kPliDecoder;
      if (!idr_wanted_ && started_) request_pli_now(kPliDecoder, now);
    }
    return;
  }
  if (f.revoked) {
    if (f.handed_keep) pending_.push_back(SinkAction{SinkAction::kDrop, frame, false, nullptr, ""});  // never shown
    return;
  }
  if (f.clean) {
    if (run_start_ == frame) {
      clean_since_ = frame;
      acked_ = frame;
      run_start_ = kNoFrame;
      broken_ = !chain_clean_;  // a loss after the IDR/refresh keeps the stream broken (and asking)
      if (broken_) next_pli_ = 0;
      rr_now_ = true;  // a refresh/IDR started a new clean run: RR at once
      wake_net_ = true;
    } else if (!broken_ && acked_ != kNoFrame && frame == acked_ + 1) {
      acked_ = frame;
    }
  }
  if (f.keep) {
    int64_t pt = 0, out;
    if (!(f.vflags & kVIdr) && mapped_out(f, frame, now, &out)) {
      const double hold = lipsync ? lipsync->hold_ms() : 0.0;
      pt = out + int64_t((d_now_ms_ + hold) * 1e6);
    }
    ready_[frame] = pt == 0 ? 1 : pt;  // 1 = present on decode (IDR, bootstrap)
  }
}

void VideoRecvCore::on_sink_event(const std::string& what, const std::string& detail, int64_t now) {
  std::lock_guard<std::mutex> lock(mu_);
  if (what == "decoder_sw" || what == "decoder_hw") {
    decoder_ = what == "decoder_sw" ? "sw" : "hw";
    // a new decoder is ready (§4.6.1): the AUs handed to the old one while it was re-created were discarded, so
    // ask for an IDR (PLI start). Nothing handed since the last (re)start: the coming IDR reaches the new one.
    if (started_) {
      std::vector<SinkAction> drops;
      revoke_from(0, &drops);
      pending_.insert(pending_.end(), drops.begin(), drops.end());
      need_idr(kPliStart, now);
    }
  } else if (what == "size_rejected") {
    auto& q = size_rejects_[detail];
    q.push_back(now);
    while (!q.empty() && now - q.front() > 10'000'000'000LL) q.pop_front();
    std::vector<SinkAction> drops;
    revoke_from(0, &drops);
    pending_.insert(pending_.end(), drops.begin(), drops.end());
    if (q.size() >= 3) {
      // no loop: stop asking, show the error, end the stream (decoder, size_rejected WxH)
      failed_ = true;
      fail_detail_ = "size_rejected " + detail;
      idr_wanted_ = false;
      started_ = false;
      shown_ = "error";
      live_ = false;
      pending_.push_back(SinkAction{SinkAction::kPlaceholder, 0, false, nullptr, "error"});
    } else {
      need_idr(kPliSize, now);
    }
  } else if (what == "device_lost") {
    std::vector<SinkAction> drops;
    revoke_from(0, &drops);
    pending_.insert(pending_.end(), drops.begin(), drops.end());
    need_idr(kPliStart, now);
  }
}

bool VideoRecvCore::take_wake_net() {
  std::lock_guard<std::mutex> lock(mu_);
  const bool w = wake_net_ || rr_now_;
  return w;
}

bool VideoRecvCore::take_failure(std::string* detail) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!failed_ || fail_reported_) return false;
  fail_reported_ = true;
  *detail = fail_detail_;
  return true;
}

void VideoRecvCore::pump(int64_t now, bool display_tick, std::vector<SinkAction>* acts) {
  std::lock_guard<std::mutex> lock(mu_);
  acts->insert(acts->end(), pending_.begin(), pending_.end());
  pending_.clear();
  d_now_ms_ = playout_.target_ms(now);
  if (!failed_) {
    // ---- start: the first complete IDR newer than the request (AUs before it are never decoded)
    if (!started_) {
      for (auto it = frames_.begin(); it != frames_.end(); ++it) {
        if (idr_after_valid_ && it->first <= idr_after_) continue;
        if ((it->second.vflags & kVIdr) && it->second.complete) {
          started_ = true;
          next_dec_ = it->first;
          frames_.erase(frames_.begin(), it);
          lost_evidence_.erase(lost_evidence_.begin(), lost_evidence_.lower_bound(next_dec_));
          break;
        }
      }
      if (!started_) {
        // the awaited IDR was lost (NACK gave up, or its present time passed behind newer packets): ask again
        if (!idr_wanted_) {
          bool pending = false;
          for (auto& kv : frames_) {
            if (idr_after_valid_ && kv.first <= idr_after_) continue;
            if (!(kv.second.vflags & kVIdr)) continue;
            const bool lost = lost_evidence_.count(kv.first) ||
                              (hi_seq_ > kv.second.max_seq && now >= present_est(kv.first, now));
            if (!lost) pending = true;
          }
          if (!pending) need_idr(idr_reason_, now);
        }
        // keep the map bounded while waiting
        while (frames_.size() > 64) frames_.erase(frames_.begin());
      }
    }
    // ---- decode hand-off in frame order; a frame is lost only on evidence
    while (started_) {
      auto it = frames_.find(next_dec_);
      if (it != frames_.end() && it->second.complete) {
        Frame& f = it->second;
        const bool idr = f.vflags & kVIdr, refresh = f.vflags & kVRefresh;
        if (idr || refresh) {
          if (broken_ || !chain_clean_ || clean_since_ == kNoFrame) run_start_ = next_dec_;
          chain_clean_ = true;
        }
        const bool clean = chain_clean_;
        bool keep = clean;
        int64_t out;
        if (keep && !idr && mapped_out(f, next_dec_, now, &out)) {  // an IDR is shown at once, never dropped as backlog
          const double hold = lipsync ? lipsync->hold_ms() : 0.0;
          if (double(now - out) * 1e-6 > d_now_ms_ + hold + 150.0) keep = false;  // backlog: decoded, not kept
        }
        auto au = std::make_shared<std::vector<uint8_t>>();
        size_t total = 0;
        for (auto& fr : f.frags) total += fr.size();
        au->reserve(total);
        for (auto& fr : f.frags) au->insert(au->end(), fr.begin(), fr.end());
        f.frags.clear();
        f.frags.shrink_to_fit();
        f.handed = true;
        f.clean = clean;
        f.keep = keep;
        f.handed_keep = keep;
        acts->push_back(SinkAction{SinkAction::kDecode, next_dec_, keep, au, ""});
        next_dec_++;
        continue;
      }
      bool lost = lost_evidence_.count(next_dec_) > 0;
      const bool hole = it == frames_.end() || !it->second.nfrag;
      if (!lost) {
        if (hole) {
          if (have_any_ && newest_seen_ > next_dec_) {
            const int64_t pt = present_est(next_dec_, now);
            lost = pt != kNever && now >= pt;
          }
        } else if (hi_seq_ > it->second.max_seq) {
          lost = now >= present_est(next_dec_, now);
        }
      }
      if (!lost) break;
      // drop pending NACKs of this frame only: they can no longer help
      mark_lost(next_dec_, hole, now);
      if (it != frames_.end()) frames_.erase(it);
      next_dec_++;
      if (!started_) break;
    }
    lost_evidence_.erase(lost_evidence_.begin(), lost_evidence_.lower_bound(next_dec_));
    // forget decoded frames behind the decode point
    for (auto it = frames_.begin(); it != frames_.end() && it->first + 8 < next_dec_;) {
      if (it->second.handed && (it->second.decoded || now - it->second.first_ns > 2'000'000'000LL)) it = frames_.erase(it);
      else ++it;
    }
    // ---- presenter: the newest due kept picture; never backwards; never one with an incomplete chain
    while (ready_.size() > 12) {  // pool exhausted: the oldest unpresented picture is dropped
      acts->push_back(SinkAction{SinkAction::kDrop, ready_.begin()->first, false, nullptr, ""});
      skipped_++;
      ready_.erase(ready_.begin());
    }
    uint32_t pick = 0;
    bool have_pick = false;
    for (auto& kv : ready_)
      if (kv.second <= now) {
        pick = kv.first;
        have_pick = true;
      }
    if (have_pick) {
      for (auto it = ready_.begin(); it != ready_.end() && it->first <= pick;) {
        if (it->first == pick && !(have_presented_ && pick <= last_presented_)) {
          const int64_t pt = it->second;
          auto fr = frames_.find(pick);
          if (pt > 1 && fr != frames_.end() && fr->second.decoded_ns > pt) late_++;
          acts->push_back(SinkAction{SinkAction::kPresent, pick, true, nullptr, ""});
          presented_++;
          have_presented_ = true;
          last_presented_ = pick;
          last_present_ns_ = now;
          presented_since_tick_ = true;
          shown_.clear();
          live_ = true;
        } else {
          acts->push_back(SinkAction{SinkAction::kDrop, it->first, false, nullptr, ""});
          skipped_++;
        }
        it = ready_.erase(it);
      }
    }
  }
  // ---- display tick: hold (heartbeat) when nothing new; reconnecting after 2 s of hold
  if (display_tick) {
    const int64_t dt = last_tick_ ? now - last_tick_ : 0;
    if (!presented_since_tick_) {
      if (live_ && shown_.empty() && !failed_) {
        const int64_t since = now - last_present_ns_;
        if (since > frame_period_ns() * 3 / 2) {
          frozen_ns_ += uint64_t(dt);
          held_ns_ += uint64_t(dt);
        }
        if (since >= timing.hold_to_reconnecting_ns) {
          shown_ = "reconnecting";
          live_ = false;
          acts->push_back(SinkAction{SinkAction::kPlaceholder, 0, false, nullptr, "reconnecting"});
        } else {
          acts->push_back(SinkAction{SinkAction::kHold, 0, false, nullptr, ""});
        }
      } else {
        if (shown_ == "reconnecting") frozen_ns_ += uint64_t(dt);
        acts->push_back(SinkAction{SinkAction::kHold, 0, false, nullptr, ""});
      }
    }
    presented_since_tick_ = false;
    last_tick_ = now;
  }
}

int64_t VideoRecvCore::next_video_deadline(int64_t now) const {
  std::lock_guard<std::mutex> lock(mu_);
  // packets and decoder results wake the thread; between them: the display tick and the next present time
  int64_t d = (last_tick_ ? last_tick_ : now) + kDisplayTickNs;
  for (auto& kv : ready_) d = std::min(d, std::max(now, kv.second));
  return d;
}

VideoRecvCore::Probe VideoRecvCore::probe() const {
  std::lock_guard<std::mutex> lock(mu_);
  Probe p;
  p.target_ms = d_now_ms_;
  p.p99_ms = playout_.p99_ms();
  p.lost = lost_;
  p.late = late_;
  p.skipped = skipped_;
  p.holes = holes_;
  p.presented = presented_;
  p.nack_tx = nack_tx_;
  p.retx_ok = retx_ok_;
  p.pli_tx = pli_tx_;
  p.rr_tx = rr_tx_;
  p.clean_since = clean_since_;
  p.acked = acked_;
  p.broken = broken_;
  p.started = started_;
  p.have_map = map_.valid();
  p.provisional = provisional_;
  p.offset_ns = prov_offset_;
  p.placeholder = shown_;
  p.next_dec = next_dec_;
  p.period_ms = double(period_ns_) * 1e-6;
  return p;
}

Json VideoRecvCore::stats(double dt) {
  std::lock_guard<std::mutex> lock(mu_);
  Json j{{"dir", "recv"}, {"kind", "video"}, {"path", link_mode_.load() ? "link" : "udp"}};
  j["kbps"] = dt > 0 ? r1(double(rx_bytes_ - p_bytes_) * 8.0 / dt / 1000.0) : 0.0;
  j["fps_shown"] = dt > 0 ? r1(double(presented_ - p_presented_) / dt) : 0.0;
  j["frozen_ms"] = (frozen_ns_ - p_frozen_) / 1'000'000;
  j["held_ms"] = (held_ns_ - p_held_) / 1'000'000;
  j["late"] = late_;
  j["skipped"] = skipped_;
  j["lost"] = lost_;
  j["holes"] = holes_;
  j["nack_tx"] = nack_tx_;
  j["retx_ok"] = retx_ok_;
  j["pli_tx"] = pli_tx_;
  j["rr_tx"] = rr_tx_;
  j["presented"] = presented_;
  j["target_ms"] = r1(d_now_ms_);
  j["floor_ms"] = r1(playout_.floor_ms());
  j["d_p99_ms"] = r1(playout_.p99_ms());
  j["d_samples"] = d_samples_;
  j["dec_ms"] = dec_n_ > p_dec_n_ ? std::round((dec_ms_sum_ - p_dec_sum_) / double(dec_n_ - p_dec_n_) * 100) / 100 : 0.0;
  j["decoder"] = decoder_;
  const double hold = lipsync ? lipsync->hold_ms() : 0.0;
  j["sync_ms"] = r1(hold);
  int64_t off = 0, age = 0;
  if (offset_source && offset_source(now_ns(), &off, &age)) {
    j["offset_ms"] = r1(double(off) * 1e-6);
    j["offset_age_s"] = r1(double(age) * 1e-9);
  } else if (provisional_) {
    j["offset_ms"] = r1(double(prov_offset_) * 1e-6);
    j["offset_age_s"] = nullptr;
  } else {
    j["offset_ms"] = nullptr;
    j["offset_age_s"] = nullptr;
  }
  j["w"] = w_;
  j["h"] = h_;
  j["broken"] = broken_;
  j["clean_since"] = clean_since_ == kNoFrame ? Json(nullptr) : Json(clean_since_);
  j["acked"] = acked_ == kNoFrame ? Json(nullptr) : Json(acked_);
  j["shown"] = shown_.empty() ? "live" : shown_;
  // lip sync data (§4.9): from capture, like the audio side (+ ~2 ms source delivery)
  j["video_present_delay_ms"] = r1(d_now_ms_ + hold + 2.0);
  p_bytes_ = rx_bytes_;
  p_presented_ = presented_;
  p_frozen_ = frozen_ns_;
  p_held_ = held_ns_;
  p_dec_n_ = dec_n_;
  p_dec_sum_ = dec_ms_sum_;
  return j;
}

// ================================================================== VideoRecvStream

VideoRecvStream::VideoRecvStream(const Config& cfg, std::shared_ptr<Flow> flow, EventSink* events, FailFn on_fail,
                                 LipSyncHold* sync)
    : StreamBase(cfg.id, "video"), cfg_(cfg), flow_(std::move(flow)), events_(events), on_fail_(std::move(on_fail)),
      core_(cfg.id, cfg.key, cfg.fps, cfg.w, cfg.h) {
  (void)events_;
  core_.lipsync = sync;
  core_.set_floor_ms(cfg.floor_ms);
  device_label_ = "vcam";
  Flow* f = flow_.get();
  core_.offset_source = [f](int64_t now, int64_t* off, int64_t* age) { return f->clock_offset(now, off, age); };
}

VideoRecvStream::~VideoRecvStream() {
  stop();
  secure_wipe(cfg_.key, sizeof cfg_.key);
}

bool VideoRecvStream::start(VideoBackend& backend, std::string* code, std::string* msg) {
  std::string e;
  sink_ = backend.open_sink(this, &e);
  if (!sink_) {
    auto c = e.find(':');
    *code = c == std::string::npos ? "internal" : e.substr(0, c);
    *msg = c == std::string::npos ? e : e.substr(std::min(e.size(), c + 2));
    return false;
  }
  thread_ = std::thread([this] { video_loop(); });
  return true;
}

void VideoRecvStream::stop() {
  if (stopped_) return;
  stopped_ = true;
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
  sink_.reset();
}

void VideoRecvStream::on_decoded(uint32_t frame, bool ok, double ms) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    mail_.push_back(Mail{true, frame, ok, ms, "", ""});
  }
  cv_.notify_all();
}

void VideoRecvStream::on_sink_event(const std::string& what, const std::string& detail) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    mail_.push_back(Mail{false, 0, false, 0, what, detail});
  }
  cv_.notify_all();
}

void VideoRecvStream::video_loop() {
  set_thread_name("beamer-audio-video");
  set_thread_interactive();
  enable_fine_timers();  // Windows: 1 ms timer resolution (the pacer's schedule and the presenter need it)
  std::vector<SinkAction> acts;
  auto exec = [this](std::vector<SinkAction>& a) {
    if (frozen_.load()) {
      a.clear();
      return;
    }
    for (auto& x : a) {
      switch (x.kind) {
        case SinkAction::kDecode: sink_->decode(x.au->data(), x.au->size(), x.frame, x.keep); break;
        case SinkAction::kPresent: sink_->present(x.frame); break;
        case SinkAction::kHold: sink_->hold(); break;
        case SinkAction::kDrop: sink_->drop(x.frame); break;
        case SinkAction::kPlaceholder: sink_->show_placeholder(x.state); break;
      }
    }
    a.clear();
  };
  int64_t now = now_ns();
  core_.start(now, &acts);
  exec(acts);
  flow_->wake();
  int64_t next_tick = now + kDisplayTickNs;
  bool fail_sent = false;
  for (;;) {
    std::deque<Mail> mail;
    {
      std::unique_lock<std::mutex> lock(mu_);
      const int64_t d = std::min(core_.next_video_deadline(now_ns()), next_tick);
      if (mail_.empty() && !stop_)
        cv_.wait_until(lock, std::chrono::steady_clock::time_point(std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                 std::chrono::nanoseconds(d))));
      if (stop_) return;
      mail.swap(mail_);
    }
    now = now_ns();
    core_.set_link_mode(flow_->link_mode());
    for (auto& m : mail) {
      if (m.decoded) core_.on_decoded(m.frame, m.ok, m.ms, now);
      else core_.on_sink_event(m.what, m.detail, now);
    }
    const bool tick = now >= next_tick;
    if (tick) {
      next_tick += kDisplayTickNs;
      if (next_tick < now) next_tick = now + kDisplayTickNs;
    }
    core_.pump(now, tick, &acts);
    exec(acts);
    if (core_.take_wake_net()) flow_->wake();
    std::string detail;
    if (!fail_sent && core_.take_failure(&detail)) {
      fail_sent = true;
      if (on_fail_) on_fail_(cfg_.id, "decoder", detail);
    }
  }
}

Json VideoRecvStream::stats(double dt) { return core_.stats(dt); }

}  // namespace beamer::audio
