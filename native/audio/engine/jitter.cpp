// SPDX-License-Identifier: GPL-3.0-only
#include "jitter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace beamer::audio {

namespace {

// numpy.quantile(v, q) with the default linear interpolation; v is reordered.
double quantile(double* v, int n, double q) {
  if (n <= 0) return 0;
  double pos = q * (n - 1);
  int lo = static_cast<int>(std::floor(pos));
  double frac = pos - lo;
  std::nth_element(v, v + lo, v + n);
  double a = v[lo];
  if (frac <= 0 || lo + 1 >= n) return a;
  double b = *std::min_element(v + lo + 1, v + n);
  return a + (b - a) * frac;
}

constexpr double kMsPerSample = 1000.0 / kRate;

}  // namespace

JitterBuffer::JitterBuffer(const JbConfig& cfg)
    : cfg_(cfg),
      rs_(cfg.channels, kMaxPull * 2 + kMaxDecode + 64),
      slots_(new Slot[kSlots]),
      scratch_(new float[size_t(kMaxDecode) * cfg.channels]),
      frame_(cfg.frame),
      block_(cfg.frame),
      tr_arr_(new double[kTransitCap]),
      tr_val_(new double[kTransitCap]),
      tr_tmp_(new double[kTransitCap]),
      pl_t_(new double[kTransitCap]),
      pl_d_(new double[kTransitCap]) {
  drift_.max_ppm = cfg.max_ppm;
  drift_.catchup_ppm = std::max(cfg.catchup_ppm, cfg.max_ppm);
  target_ms_ = cfg.fixed_ms >= 0 ? cfg.fixed_ms : std::min(cfg.max_ms, std::max(cfg.min_ms, 25.0));
}

bool JitterBuffer::init(std::string* err) { return dec_.init(cfg_.channels, err); }

int64_t JitterBuffer::unwrap(uint32_t ts32) {
  if (!has_last32_) {
    has_last32_ = true;
    last_ts32_ = ts32;
    ts_hi_ = ts32;
    return ts_hi_;
  }
  int64_t d = static_cast<int64_t>(static_cast<uint32_t>(ts32 - last_ts32_));
  if (d >= 0x80000000LL) d -= 0x100000000LL;
  ts_hi_ += d;
  last_ts32_ = ts32;
  return ts_hi_;
}

bool JitterBuffer::slot_index(int64_t ts, int* idx) const {
  if (!has_base_) return false;
  int64_t d = ts - base_;
  int64_t q = d / frame_, r = d % frame_;
  if (r != 0) return false;
  int64_t m = q % kSlots;
  if (m < 0) m += kSlots;
  *idx = static_cast<int>(m);
  return true;
}

JitterBuffer::Slot* JitterBuffer::get(int64_t ts) {
  int i;
  if (!slot_index(ts, &i)) return nullptr;
  Slot& s = slots_[i];
  return (s.used && s.ts == ts) ? &s : nullptr;
}

bool JitterBuffer::min_ts(int64_t* out) const {
  if (!has_max_) return false;
  int64_t lo = has_next_ ? next_ts_ : max_ts_ - int64_t(kSlots - 1) * frame_;
  bool found = false;
  int64_t best = 0;
  for (int i = 0; i < kSlots; ++i) {
    const Slot& s = slots_[i];
    if (s.used && s.ts >= lo && s.ts <= max_ts_ && (!found || s.ts < best)) {
      best = s.ts;
      found = true;
    }
  }
  if (found) *out = best;
  return found;
}

int JitterBuffer::clear_slots() {
  int n = 0;
  for (int i = 0; i < kSlots; ++i) {
    if (slots_[i].used && (!has_next_ || slots_[i].ts >= next_ts_)) n++;
    slots_[i].used = false;
  }
  return n;
}

void JitterBuffer::reset_timeline() {
  est_cur_seg_++;  // a timestamp jump is a step for the clock estimate: a new segment
  est_miss_ = 0;
  c_.skipped += clear_slots();
  has_base_ = false;
  has_next_ = false;
  has_max_ = false;
  playing_ = false;
  expand_run_ = 0;
  expand_credit_ = 0;
  c_.resets++;
}

void JitterBuffer::store(int64_t ts, const MediaFrame& p) {
  int i;
  if (!slot_index(ts, &i)) {
    // first frame, or a frame off the grid (frame size change): restart the slot grid
    c_.skipped += clear_slots();
    has_base_ = true;
    base_ = ts;
    slot_index(ts, &i);
  }
  Slot& s = slots_[i];
  if (s.used && s.ts != ts && (!has_next_ || s.ts >= next_ts_)) c_.skipped++;
  s.ts = ts;
  s.used = true;
  s.flags = p.flags;
  s.nsamp = p.nsamp;
  s.len = p.len;
  std::memcpy(s.data, p.data, p.len);
  if (!has_max_ || ts > max_ts_) {
    max_ts_ = ts;
    has_max_ = true;
  }
}

void JitterBuffer::push(const MediaFrame& p) {
  int64_t ts = unwrap(p.ts);
  c_.rx++;
  c_.rx_bytes += p.len + kMinPacket + 28;
  const double arr = static_cast<double>(p.arrival_ns) * 1e-9;
  const double tr = arr - static_cast<double>(ts) / kRate;
  // transit window
  if (tr_n_ == kTransitCap) {
    tr_head_ = (tr_head_ + 1) % kTransitCap;
    tr_n_--;
  }
  int w = (tr_head_ + tr_n_) % kTransitCap;
  tr_arr_[w] = arr;
  tr_val_[w] = tr;
  tr_n_++;
  if (cfg_.peak_hold_s > 0 && tr_n_ > 50) {
    if (!has_tmin_ || arr - tmin_t_ > cfg_.window_s) {
      double m = tr_val_[tr_head_];
      for (int k = 1; k < tr_n_; ++k) m = std::min(m, tr_val_[(tr_head_ + k) % kTransitCap]);
      tmin_ = m;
      tmin_t_ = arr;
      has_tmin_ = true;
    }
    tmin_ = std::min(tmin_, tr);
    double ex = (tr - tmin_) * 1000.0;
    if (std::max(jit_cache_ + 5.0, cfg_.peak_min_ms) < ex && ex < kSpikeMs) {
      int last = (pk_head_ + pk_n_ - 1) % kPeakCap;
      if (pk_n_ > 0 && arr - pk_t_[last] < 0.2) {
        if (ex > pk_ex_[last]) pk_ex_[last] = ex;
      } else {
        if (pk_n_ == kPeakCap) {
          pk_head_ = (pk_head_ + 1) % kPeakCap;
          pk_n_--;
        }
        int k = (pk_head_ + pk_n_) % kPeakCap;
        pk_t_[k] = arr;
        pk_ex_[k] = ex;
        pk_n_++;
      }
    }
  }
  while (tr_n_ > 0 && tr_arr_[tr_head_] < arr - cfg_.window_s) {
    tr_head_ = (tr_head_ + 1) % kTransitCap;
    tr_n_--;
  }
  jit_dirty_ = true;
  if (!est_has_tr_ || tr < est_tr_min_) {
    est_tr_min_ = tr;
    est_has_tr_ = true;
  }

  if (p.nsamp > 0 && p.nsamp != frame_ && p.nsamp <= kMaxDecode) frame_ = p.nsamp;
  if (has_next_ && ts < next_ts_) {
    c_.late++;
    return;
  }
  if (has_next_ && ts >= next_ts_ + int64_t(kSlots) * frame_) {
    reset_timeline();  // a jump far beyond the buffer: start over at this frame
  } else if (!has_next_ && has_max_ && ts <= max_ts_ - int64_t(kSlots) * frame_) {
    c_.late++;
    return;
  }
  if (get(ts)) {
    c_.dup++;
    return;
  }
  if (opus_has_lbrr(p.data, p.len)) c_.lbrr++;
  store(ts, p);
}

double JitterBuffer::jitter_ms(double now) {
  if (!jit_dirty_) return jit_value_;
  if (now >= 0 && cfg_.jitter_recalc_ms > 0 && (now - jit_time_) * 1000.0 < cfg_.jitter_recalc_ms) return jit_value_;
  jit_time_ = now;
  jit_dirty_ = false;
  if (tr_n_ < 10) {
    jit_value_ = 10.0;
    return jit_value_;
  }
  double m = tr_val_[tr_head_];
  for (int k = 1; k < tr_n_; ++k) m = std::min(m, tr_val_[(tr_head_ + k) % kTransitCap]);
  int n = 0;
  for (int k = 0; k < tr_n_; ++k) {
    double v = (tr_val_[(tr_head_ + k) % kTransitCap] - m) * 1000.0;
    if (v < kSpikeMs) tr_tmp_[n++] = v;
  }
  jit_value_ = quantile(tr_tmp_.get(), n, 0.97);
  return jit_value_;
}

double JitterBuffer::pull_jitter_ms() const {
  if (pl_n_ < 20) return 0.0;
  double m = pl_d_[pl_head_];
  for (int k = 1; k < pl_n_; ++k) m = std::min(m, pl_d_[(pl_head_ + k) % kTransitCap]);
  int n = 0;
  double* tmp = tr_tmp_.get();
  for (int k = 0; k < pl_n_; ++k) {
    double v = (pl_d_[(pl_head_ + k) % kTransitCap] - m) * 1000.0;
    if (v < kSpikeMs) tmp[n++] = v;
  }
  return quantile(tmp, n, 0.97);
}

double JitterBuffer::level_ms() const {
  double lv = rs_.available();
  if (has_next_ && has_max_ && max_ts_ >= next_ts_) lv += static_cast<double>(max_ts_ + frame_ - next_ts_);
  return lv * kMsPerSample;
}

void JitterBuffer::decode_discard(Slot* s) {
  // The decoder must see every frame the encoder coded, or its state no longer matches the
  // encoder's and artefacts recur long after (Phase 1, "Mac->PC jitter" point 5).
  int r = dec_.decode(s->data, s->len, scratch_.get(), kMaxDecode);
  if (r < 0) c_.dec_err++;
  s->used = false;
}

int JitterBuffer::decode_next(float* out) {
  const int n = frame_;
  const int64_t ts = next_ts_;
  Slot* p = get(ts);
  int r;
  if (p) {
    r = dec_.decode(p->data, p->len, out, kMaxDecode);
    int ns = p->nsamp ? p->nsamp : n;
    if (r < 0) {
      c_.dec_err++;
      std::memset(out, 0, sizeof(float) * size_t(ns) * cfg_.channels);
      r = ns;
    }
    c_.ok++;
    next_ts_ += ns;
    p->used = false;
    expand_run_ = 0;
    expand_credit_ = 0;
    return r;
  }
  if (has_max_ && max_ts_ > ts && expand_credit_ > 0) {
    // the frame we expanded for is lost: that expansion was its concealment
    expand_credit_--;
    c_.lost++;
    c_.merged++;
    next_ts_ += n;
    expand_run_ = 0;
    return decode_next(out);
  }
  if (has_max_ && max_ts_ > ts) {
    Slot* nxt = get(ts + n);
    r = -1;
    if (nxt && cfg_.fec) {
      r = dec_.decode_fec(nxt->data, nxt->len, out, n);
      if (r >= 0) c_.fec++;
    }
    if (r < 0) {
      r = dec_.plc(out, n);
      c_.plc++;
    }
    c_.lost++;
    next_ts_ += n;
    expand_run_ = 0;
  } else {
    // nothing newer has arrived: the packet is late. Conceal without advancing the timeline
    // (expand); after 3 frames of this, give up and re-buffer.
    c_.expand++;
    if (expand_run_ == 0) c_.underrun++;
    expand_run_++;
    if (cfg_.merge_expand) expand_credit_++;
    r = dec_.plc(out, n);
    if (expand_run_ > 3) {
      playing_ = false;
      c_.rebuffer++;
    }
  }
  if (r < 0) {
    c_.dec_err++;
    std::memset(out, 0, sizeof(float) * size_t(n) * cfg_.channels);
    r = n;
  }
  return r;
}

bool JitterBuffer::start_if_ready() {
  int64_t first;
  if (!min_ts(&first)) return false;
  double span = static_cast<double>(max_ts_ + frame_ - first) * kMsPerSample;
  if (!has_next_ || first > next_ts_) {
    if (span < target_ms_ - block_ * 500.0 / kRate) return false;
    next_ts_ = first;
    has_next_ = true;
    expand_credit_ = 0;
  } else if (level_ms() < target_ms_) {
    return false;
  }
  flush_to_target();
  playing_ = true;
  expand_run_ = 0;
  return true;
}

void JitterBuffer::flush_to_target() {
  // after an outage the burst of late packets would otherwise sit in the buffer as latency
  double excess = level_ms() - target_ms_;
  const double fms = frame_ * kMsPerSample;
  int n = 0;
  while (excess > fms && has_next_ && next_ts_ < max_ts_) {
    Slot* p = get(next_ts_);
    if (p) decode_discard(p);
    next_ts_ += frame_;
    excess -= fms;
    n++;
  }
  if (n) {
    expand_credit_ = 0;
    c_.flushed += n;
    has_avg_ = false;
  }
}

void JitterBuffer::control(double now, double block_s) {
  if (cfg_.pull_jitter) {
    if (pl_n_ == kTransitCap) {
      pl_head_ = (pl_head_ + 1) % kTransitCap;
      pl_n_--;
    }
    int k = (pl_head_ + pl_n_) % kTransitCap;
    pl_t_[k] = now;
    pl_d_[k] = now - static_cast<double>(pulled_) / kRate;
    pl_n_++;
    while (pl_n_ > 0 && pl_t_[pl_head_] < now - cfg_.window_s) {
      pl_head_ = (pl_head_ + 1) % kTransitCap;
      pl_n_--;
    }
  }
  if (cfg_.fixed_ms < 0) {
    double jit = jitter_ms(now);
    jit_cache_ = jit;
    if (cfg_.peak_hold_s > 0) {
      while (pk_n_ > 0 && pk_t_[pk_head_] < now - cfg_.peak_hold_s) {
        pk_head_ = (pk_head_ + 1) % kPeakCap;
        pk_n_--;
      }
      if (pk_n_ >= cfg_.peak_count) {
        for (int k = 0; k < pk_n_; ++k) jit = std::max(jit, pk_ex_[(pk_head_ + k) % kPeakCap]);
      }
    }
    double t = block_ * kMsPerSample + frame_ * 500.0 / kRate + jit +
               (cfg_.pull_jitter ? pull_jitter_ms() : 0.0) + cfg_.margin_ms;
    t = std::min(cfg_.max_ms, std::max(cfg_.min_ms, t));
    // rise fast, decay slowly
    target_ms_ = t > target_ms_ ? t : target_ms_ + (t - target_ms_) * block_s / 2.0;
  }
  if (level_ms() > target_ms_ + std::max(40.0, target_ms_)) flush_to_target();  // far too much
  const double lv = level_ms();
  last_level_ = lv;
  const double a = std::min(1.0, block_s / 1.0);
  level_avg_ = has_avg_ ? level_avg_ + a * (lv - level_avg_) : lv;
  has_avg_ = true;
  const double err = level_avg_ - target_ms_;
  if (cfg_.drift) {
    // The integral term is the clock difference. Once measured from the timestamps it is taken from
    // there: integrating the level error also integrates the level that expansions add (a late frame
    // played after its concealment), which pinned the integral at +200 ppm live (WP11).
    // Moved towards the estimate at <= 2 ppm/s, so no single estimate can jump it. While the
    // estimate restarts (after a step) the integral keeps the value it had: the clocks did not change.
    if (est_valid_) est_ever_ = true;
    drift_.hold_integ = cfg_.drift_est && est_ever_;
    if (drift_.hold_integ && est_valid_) {
      const double want = std::max(-200.0, std::min(200.0, est_ppm_)), step = 2.0 * block_s;
      drift_.integ += std::max(-step, std::min(step, want - drift_.integ));
    }
    drift_.update(err, block_s);
  }
  // big excursion: drop a frame; a silent one as soon as the excess exceeds max(5 ms, 0.75 frame),
  // a non-silent one only after 2 s above max(drop_ms, frame) (catch-up resampling does the rest)
  const double fms = frame_ * kMsPerSample;
  if (err > std::max(5.0, 0.75 * fms)) {
    if (!over_) {
      over_ = true;
      over_since_ = now;
    }
    const double elapsed = now - over_since_;
    Slot* p = has_next_ ? get(next_ts_) : nullptr;
    if (p && max_ts_ > next_ts_) {
      bool quiet = (p->flags & kFlagSilence) != 0;
      if (quiet || (elapsed > 2.0 && err > std::max(cfg_.drop_ms, fms))) {
        int ns = p->nsamp ? p->nsamp : frame_;
        decode_discard(p);
        next_ts_ += ns;
        c_.drop++;
        level_avg_ -= ns * kMsPerSample;
        over_ = false;
      }
    }
  } else {
    over_ = false;
  }
}

// A step in (lag - transit) is not a clock ratio: the sender's capture or our render restarted, the
// path changed, the flow fell back to the link. A second off the line by more than kEstStep is left
// out (a Wi-Fi episode that holds packets for a while leaves no trace); kEstStepPoints of them in a
// row start a new segment. Segments share the slope and each has its own
// offset (est_fit), so a step neither bends the line nor throws away the history.
void JitterBuffer::est_add(double t, double d) {
  if (est_n_ > 0) {
    const int last = (est_head_ + est_n_ - 1) % kEstCap;
    const double pred = est_d_[last] + (est_has_fit_ ? est_slope_ : 0.0) * (t - est_t_[last]);
    if (std::fabs(d - pred) > kEstStep) {
      if (++est_miss_ < kEstStepPoints) return;
      est_cur_seg_++;
    }
  }
  est_miss_ = 0;
  if (est_n_ == kEstCap) {
    est_head_ = (est_head_ + 1) % kEstCap;
    est_n_--;
  }
  const int w = (est_head_ + est_n_) % kEstCap;
  est_t_[w] = t;
  est_d_[w] = d;
  est_seg_[w] = est_cur_seg_;
  est_n_++;
  est_fit();
}

void JitterBuffer::est_pull(double now, int n) {
  const int64_t b = static_cast<int64_t>(std::floor(now));
  if (!est_has_bucket_ || b != est_bucket_) {
    if (est_has_bucket_ && est_has_tr_ && est_has_pl_) est_add(static_cast<double>(est_bucket_), est_pl_min_ - est_tr_min_);
    est_has_bucket_ = true;
    est_bucket_ = b;
    est_has_tr_ = est_has_pl_ = false;
  }
  const double lag = now - static_cast<double>(pulled_ - n) / kRate;
  if (!est_has_pl_ || lag < est_pl_min_) {
    est_pl_min_ = lag;
    est_has_pl_ = true;
  }
}

static inline bool est_keep(double r, double lo, double hi) { return r >= -lo && r <= hi; }

// Least squares of (lag - transit) over time with one offset per segment (pooled within-segment
// slope), three passes: the later ones leave out the seconds whose minimum was itself late (inside a
// Wi-Fi episode). Segments of fewer than 5 points carry no weight. The estimate counts once
// the points span 60 s and the slope is known to +-kEstMaxSePpm (standard error): a sender that
// stalls or a path that keeps getting slower for a while is no clock and must not steer the integral
// (WP11 live: -977 ppm from 20 s that included a 30 ms excursion; -177 ppm from a window restarted
// inside a Wi-Fi episode).
void JitterBuffer::est_fit() {
  if (est_n_ < 20) return;
  const double t0 = est_t_[est_head_], d0 = est_d_[est_head_];
  double slope = 0, lo = 1e9, hi = 1e9, rms = 0, sxx = 0;
  for (int pass = 0; pass < 3; ++pass) {
    const double ps = slope;  // the previous pass's line decides which points are kept
    // per segment sums over the kept points; segments are contiguous in ring order
    double sxy = 0, m = 0;
    sxx = 0;
    int nseg = 0;
    for (int k = 0; k < est_n_;) {
      const int seg = est_seg_[(est_head_ + k) % kEstCap];
      const int prev_idx = nseg;
      double n = 0, st = 0, sd = 0, stt = 0, std_ = 0;
      int e = k;
      for (; e < est_n_ && est_seg_[(est_head_ + e) % kEstCap] == seg; ++e) {
        const int i = (est_head_ + e) % kEstCap;
        const double t = est_t_[i] - t0, d = est_d_[i] - d0;
        if (pass && !est_keep(d - (est_icpts_[prev_idx] + ps * t), lo, hi)) continue;
        n += 1;
        st += t;
        sd += d;
        stt += t * t;
        std_ += t * d;
      }
      est_sn_[nseg] = n;
      est_st_[nseg] = st;
      est_sd_[nseg] = sd;
      if (n >= 5) {
        sxx += stt - st * st / n;
        sxy += std_ - st * sd / n;
        m += n;
      }
      nseg++;
      k = e;
    }
    if (m < 20 || sxx <= 0) return;
    slope = sxy / sxx;
    double ss = 0, used = 0;
    int si = 0;
    for (int k = 0; k < est_n_;) {
      const int seg = est_seg_[(est_head_ + k) % kEstCap];
      const double old_icpt = est_icpts_[si];
      const double icpt = est_sn_[si] > 0 ? (est_sd_[si] - slope * est_st_[si]) / est_sn_[si] : 0.0;
      int e = k;
      for (; e < est_n_ && est_seg_[(est_head_ + e) % kEstCap] == seg; ++e) {
        if (est_sn_[si] < 5) continue;
        const int i = (est_head_ + e) % kEstCap;
        const double t = est_t_[i] - t0, d = est_d_[i] - d0;
        if (pass && !est_keep(d - (old_icpt + ps * t), lo, hi)) continue;
        const double r = d - (icpt + slope * t);
        ss += r * r;
        used += 1;
      }
      est_icpts_[si] = icpt;
      si++;
      k = e;
    }
    rms = std::sqrt(ss / std::max(1.0, used - si - 1));
    // Lower envelope: extra delay only ever pushes a point below the line (transit up), so the
    // low side is cut harder; a path that recovers slowly from an episode then does not tilt it.
    lo = std::max(0.001, 2.0 * rms);
    hi = std::max(0.003, 4.0 * rms);
  }
  est_has_fit_ = true;
  est_slope_ = slope;
  est_se_ppm_ = sxx > 0 ? rms / std::sqrt(sxx) * 1e6 : 1e9;
  est_ppm_ = slope * 1e6;
  est_valid_ = est_t_[(est_head_ + est_n_ - 1) % kEstCap] - t0 >= kEstMinSpanS - 0.5 && est_se_ppm_ <= kEstMaxSePpm;
}

void JitterBuffer::pull(float* out, int n, double now) {
  block_ = n;
  c_.pulls++;
  pulled_ += n;
  est_pull(now, n);
  if (!playing_ && !start_if_ready()) {
    c_.silent_pulls++;
    last_level_ = level_ms();
    std::memset(out, 0, sizeof(float) * size_t(n) * cfg_.channels);
    return;
  }
  control(now, static_cast<double>(n) / kRate);
  const double step = 1.0 + drift_.ppm * 1e-6;
  while (rs_.need(n, step) > 0) {
    if (!playing_) break;
    int k = decode_next(scratch_.get());
    if (!rs_.feed(scratch_.get(), k)) break;
  }
  int m = rs_.need(n, step);
  if (m > 0) rs_.feed_zeros(m);
  rs_.out(out, n, step);
}

}  // namespace beamer::audio
