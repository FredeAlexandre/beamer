// SPDX-License-Identifier: GPL-3.0-only
// C4 Mac sender: an always-running daemon that starts a stream when the PC asks (kStart) and stops it on
// kStop. Stream = C1's synthetic source (code strip: frame number + Mac capture time) -> VideoToolbox
// low-latency H.264 with LTR -> C3's packetization and pacing (P/LTR frames through a 32-packet / 25 Mbit/s
// token bucket, IDR paced at k x bitrate, retransmissions first) -> UDP. Real feedback from the PC decoder:
// kRR acknowledges cleanly decoded frames (their LTR tokens go to AcknowledgedLTRTokens), kNack asks for
// retransmissions of the identical bytes (1 s history), kPli asks for ForceLTRRefresh (IDR if no token is
// acknowledged, or after 3 refreshes for the same loss).
//
//   camsend --peer 192.168.1.134 [--port 40160] [--peer-port 24860] --log FILE [--seconds N]
//
// Log: JSON lines (one per frame once its last packet left, plus events), Mac host clock in ns.
#import <Foundation/Foundation.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreMedia/CoreMedia.h>
#import <VideoToolbox/VideoToolbox.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <atomic>
#include <condition_variable>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "../proto.h"

using namespace e2e;

// C1's synthetic source (dev/camera/proto/mac/src/synth.mm)
void synth_fill(CVPixelBufferRef pb, uint32_t frame, int noise);
void synth_stamp(CVPixelBufferRef pb, uint32_t frame, uint64_t host_ns);

// ---------------------------------------------------------------------------------------------- utils
static mach_timebase_info_data_t g_tb;
static uint64_t host_to_ns(uint64_t h) { return (uint64_t)((__uint128_t)h * g_tb.numer / g_tb.denom); }
static uint64_t ns_to_host(uint64_t ns) { return (uint64_t)((__uint128_t)ns * g_tb.denom / g_tb.numer); }
static uint64_t now_ns() { return host_to_ns(mach_absolute_time()); }
static void sleep_until_ns(uint64_t t) { mach_wait_until(ns_to_host(t)); }

static FILE *g_log = stderr;
static std::mutex g_log_mu;
static void logj(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logj(const char *fmt, ...) {
  std::lock_guard<std::mutex> lk(g_log_mu);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(g_log, fmt, ap);
  va_end(ap);
  fputc('\n', g_log);
}
static double cpu_s() {
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}
static double rss_mb() {
  mach_task_basic_info_data_t info;
  mach_msg_type_number_t n = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &n) != KERN_SUCCESS) return -1;
  return info.resident_size / 1048576.0;
}

// ---------------------------------------------------------------------------------------------- net
static int g_sock = -1;
static sockaddr_in g_peer;
static std::atomic<uint64_t> g_tx_bytes{0}, g_tx_pk{0};
static void tx(const void *p, size_t n) {
  if (send(g_sock, p, n, 0) > 0) { g_tx_bytes += n; g_tx_pk++; }
}

// ---------------------------------------------------------------------------------------------- pacer
struct Pkt {
  std::vector<uint8_t> b;
  uint32_t seq = 0, frame = 0;
  uint16_t frag = 0, nfrag = 0;
  bool idr = false, rtx = false;
};
struct FrameMeta {
  uint32_t f;
  uint16_t sess;
  uint64_t cap, in, out, s0 = 0;
  int size, npk;
  bool key, refresh, behind;
  int64_t token;
};

class Pacer {
 public:
  std::mutex mu;
  std::condition_variable cv;
  std::deque<Pkt> q, rq;  // new data, retransmissions
  std::map<uint32_t, FrameMeta> meta;  // frames until their last packet left
  std::deque<std::pair<uint64_t, Pkt>> hist;  // (sent ns, packet) for 1 s
  std::map<uint32_t, size_t> hist_index_unused;
  double bucket_bps = 25e6, idr_bps = 6.25e6;
  double cap_bytes = 32 * 1200.0, tokens = 32 * 1200.0;
  uint64_t t_tok = 0, idr_next = 0;
  bool stop = false;
  uint64_t rtx_sent = 0, rtx_miss = 0;

  bool idr_queued() {
    for (auto &p : q) if (p.idr) return true;
    return false;
  }
  void push_frame(std::vector<Pkt> &&pk, const FrameMeta &m) {
    std::lock_guard<std::mutex> lk(mu);
    meta[m.f] = m;
    for (auto &p : pk) q.push_back(std::move(p));
    cv.notify_one();
  }
  void nack(const uint32_t *seqs, int n, uint16_t sess) {
    std::lock_guard<std::mutex> lk(mu);
    for (int i = 0; i < n; i++) {
      bool found = false;
      for (auto it = hist.rbegin(); it != hist.rend(); ++it) {
        const DataHdr *h = (const DataHdr *)it->second.b.data();
        if (h->seq == seqs[i] && h->sess == sess) {
          bool dup = false;
          for (auto &r : rq) if (r.seq == seqs[i]) dup = true;
          if (!dup) { Pkt c = it->second; c.rtx = true; rq.push_back(std::move(c)); }
          found = true;
          break;
        }
      }
      if (!found) rtx_miss++;
    }
    cv.notify_one();
  }
  void clear() {
    std::lock_guard<std::mutex> lk(mu);
    q.clear();
    rq.clear();
    meta.clear();
    hist.clear();
  }
  void run() {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    std::unique_lock<std::mutex> lk(mu);
    t_tok = now_ns();
    while (!stop) {
      if (q.empty() && rq.empty()) { cv.wait_for(lk, std::chrono::milliseconds(50)); continue; }
      uint64_t now = now_ns();
      tokens = std::min(cap_bytes, tokens + (now - t_tok) * 1e-9 * bucket_bps / 8);
      t_tok = now;
      bool from_r = !rq.empty();
      Pkt &p = from_r ? rq.front() : q.front();
      uint64_t due = now;
      if (!from_r && p.idr) {
        due = std::max(now, idr_next);
      } else if (tokens < p.b.size()) {
        due = now + (uint64_t)((p.b.size() - tokens) / (bucket_bps / 8) * 1e9);
      }
      if (due > now + 20000) {  // > 20 us away: sleep (unlocked), then re-evaluate
        lk.unlock();
        sleep_until_ns(due);
        lk.lock();
        continue;
      }
      Pkt pk = std::move(p);
      if (from_r) rq.pop_front(); else q.pop_front();
      if (pk.idr && !from_r) {  // keep the schedule (sleep overshoot must not accumulate); restart it for a new IDR
        uint64_t base = idr_next + 5000000ULL < now ? now : idr_next;
        idr_next = base + (uint64_t)(pk.b.size() * 8 / idr_bps * 1e9);
      }
      else tokens -= pk.b.size();
      lk.unlock();
      tx(pk.b.data(), pk.b.size());
      uint64_t ts = now_ns();
      lk.lock();
      if (from_r) { rtx_sent++; continue; }
      auto it = meta.find(pk.frame);
      if (it != meta.end()) {
        if (pk.frag == 0) it->second.s0 = ts;
        if (pk.frag + 1 == pk.nfrag) {
          FrameMeta m = it->second;
          meta.erase(it);
          logj("{\"ev\":\"f\",\"f\":%u,\"sess\":%u,\"cap\":%llu,\"in\":%llu,\"out\":%llu,\"s0\":%llu,\"s1\":%llu,\"size\":%d,\"npk\":%d,"
               "\"key\":%d,\"refresh\":%d,\"behind\":%d,\"tok\":%lld}",
               m.f, m.sess, m.cap, m.in, m.out, m.s0, ts, m.size, m.npk, m.key, m.refresh, m.behind, m.token);
        }
      }
      hist.push_back({ts, std::move(pk)});
      while (!hist.empty() && ts - hist.front().first > 1000000000ULL) hist.pop_front();
    }
  }
};
static Pacer g_pacer;

// ---------------------------------------------------------------------------------------------- stream
struct Stream {
  // config
  int w = 1280, h = 720, key_every = 0;
  int64_t bitrate = 2500000;
  double idr_pace = 2.5;
  bool audio = false;
  uint16_t sess = 0;
  // state
  VTCompressionSessionRef vt = nullptr;
  std::thread src, aud;
  std::atomic<bool> running{false};
  std::mutex mu;  // guards everything below
  std::map<uint32_t, int64_t> tokens;     // frame -> LTR token (not yet acknowledged)
  std::vector<int64_t> ack_pending;       // tokens to pass with the next frame
  std::set<int64_t> acked;                // tokens acknowledged at least once
  bool want_refresh = false, want_idr = false;
  uint32_t last_refresh_frame = 0;
  uint64_t last_refresh_ns = 0;
  uint32_t pli_lost = UINT32_MAX;
  int refreshes_for_lost = 0;
  uint32_t seq = 0;
  uint64_t t_create_ns = 0, t_first_cap = 0;
  int refresh_n = 0, idr_n = 0, pli_n = 0;
};
static std::mutex g_stream_mu;  // start/stop
static Stream *g_st = nullptr;
static std::atomic<uint32_t> g_frame{0};
static uint16_t g_sess = 0;
static std::atomic<uint64_t> g_last_rr{0};
static std::atomic<uint64_t> g_synth_cpu_ns{0};  // CPU spent generating the synthetic picture (not part of a camera pipeline)
static uint64_t thread_cpu_ns() { timespec ts; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts); return ts.tv_sec * 1000000000ULL + ts.tv_nsec; }

static std::vector<uint8_t> annexb(CMSampleBufferRef sb, bool key) {
  std::vector<uint8_t> o;
  static const uint8_t sc[4] = {0, 0, 0, 1};
  CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sb);
  if (key) {
    size_t n = 0;
    int nh = 4;
    CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, 0, nullptr, nullptr, &n, &nh);
    for (size_t i = 0; i < n; i++) {
      const uint8_t *p;
      size_t len;
      if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, i, &p, &len, nullptr, nullptr) == noErr) {
        o.insert(o.end(), sc, sc + 4);
        o.insert(o.end(), p, p + len);
      }
    }
  }
  CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
  size_t len = CMBlockBufferGetDataLength(bb);
  std::vector<uint8_t> d(len);
  CMBlockBufferCopyDataBytes(bb, 0, len, d.data());
  size_t off = 0;
  while (off + 4 <= len) {
    uint32_t n = (d[off] << 24) | (d[off + 1] << 16) | (d[off + 2] << 8) | d[off + 3];
    if (off + 4 + n > len) break;
    o.insert(o.end(), sc, sc + 4);
    o.insert(o.end(), d.begin() + off + 4, d.begin() + off + 4 + n);
    off += 4 + n;
  }
  return o;
}

struct EncRef { uint32_t f; uint64_t cap, in; bool refresh; };

static void encode_cb(void *ctx, void *ref, OSStatus st, VTEncodeInfoFlags info, CMSampleBufferRef sb) {
  Stream *s = (Stream *)ctx;
  EncRef *r = (EncRef *)ref;
  uint64_t out = now_ns();
  if (st != noErr || !sb || (info & kVTEncodeInfo_FrameDropped)) {
    logj("{\"ev\":\"enc_drop\",\"f\":%u,\"st\":%d}", r->f, (int)st);
    delete r;
    return;
  }
  bool key = true;
  int64_t token = -1;
  CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray(sb, false);
  if (att && CFArrayGetCount(att)) {
    CFDictionaryRef a = (CFDictionaryRef)CFArrayGetValueAtIndex(att, 0);
    CFBooleanRef ns = (CFBooleanRef)CFDictionaryGetValue(a, kCMSampleAttachmentKey_NotSync);
    if (ns && CFBooleanGetValue(ns)) key = false;
    CFNumberRef tok = (CFNumberRef)CFDictionaryGetValue(a, kVTSampleAttachmentKey_RequireLTRAcknowledgementToken);
    if (tok) CFNumberGetValue(tok, kCFNumberSInt64Type, &token);
  }
  std::vector<uint8_t> au = annexb(sb, key);
  std::vector<Pkt> pk;
  int nfrag = (int)((au.size() + kMaxFrag - 1) / kMaxFrag);
  bool behind;
  {
    std::lock_guard<std::mutex> lk(g_pacer.mu);
    behind = g_pacer.idr_queued();
  }
  uint32_t seq0;
  {
    std::lock_guard<std::mutex> lk(s->mu);
    if (token >= 0) s->tokens[r->f] = token;
    seq0 = s->seq;
    s->seq += nfrag;
  }
  uint8_t vf = (key ? kIdr : 0) | (r->refresh ? kRefresh : 0) | (token >= 0 ? kHasToken : 0) | (behind ? kBehindIdr : 0);
  for (int i = 0; i < nfrag; i++) {
    size_t off = (size_t)i * kMaxFrag, n = std::min((size_t)kMaxFrag, au.size() - off);
    Pkt p;
    p.b.resize(sizeof(DataHdr) + n + kTag, 0);
    DataHdr *h = (DataHdr *)p.b.data();
    h->ptype = kData;
    h->codec = 2;
    h->sess = s->sess;
    h->seq = seq0 + i;
    h->cap_ns = r->cap;
    h->out_ns = out;
    h->frame = r->f;
    h->frag = (uint16_t)i;
    h->nfrag = (uint16_t)nfrag;
    h->vflags = vf;
    h->ltr = (uint8_t)(token & 0xff);
    memcpy(p.b.data() + sizeof(DataHdr), au.data() + off, n);
    p.seq = h->seq;
    p.frame = r->f;
    p.frag = (uint16_t)i;
    p.nfrag = (uint16_t)nfrag;
    p.idr = key;
    pk.push_back(std::move(p));
  }
  FrameMeta m{r->f, s->sess, r->cap, r->in, out, 0, (int)au.size(), nfrag, key, r->refresh, behind, token};
  g_pacer.push_frame(std::move(pk), m);
  delete r;
}

static bool open_vt(Stream *s) {
  NSDictionary *spec = @{(id)kVTVideoEncoderSpecification_EnableLowLatencyRateControl : @YES,
                         (id)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder : @YES};
  NSDictionary *srcAttrs = @{(id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
                             (id)kCVPixelBufferWidthKey : @(s->w), (id)kCVPixelBufferHeightKey : @(s->h),
                             (id)kCVPixelBufferIOSurfacePropertiesKey : @{}};
  OSStatus st = VTCompressionSessionCreate(nullptr, s->w, s->h, kCMVideoCodecType_H264, (__bridge CFDictionaryRef)spec,
                                           (__bridge CFDictionaryRef)srcAttrs, nullptr, encode_cb, s, &s->vt);
  if (st != noErr) { logj("{\"ev\":\"vt_create_failed\",\"st\":%d}", (int)st); return false; }
  VTSessionSetProperty(s->vt, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
  VTSessionSetProperty(s->vt, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_ConstrainedHigh_AutoLevel);
  VTSessionSetProperty(s->vt, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
  VTSessionSetProperty(s->vt, kVTCompressionPropertyKey_ExpectedFrameRate, (__bridge CFTypeRef) @30);
  VTSessionSetProperty(s->vt, kVTCompressionPropertyKey_AverageBitRate, (__bridge CFTypeRef) @(s->bitrate));
  VTSessionSetProperty(s->vt, kVTCompressionPropertyKey_EnableLTR, kCFBooleanTrue);
  VTCompressionSessionPrepareToEncodeFrames(s->vt);
  return true;
}

static void source_loop(Stream *s) {
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(s->vt);
  const uint64_t period = 1000000000ULL / 30;
  uint64_t t0 = now_ns();
  for (uint64_t i = 0; s->running; i++) {
    sleep_until_ns(t0 + i * period);
    if (!s->running) break;
    CVPixelBufferRef pb = nullptr;
    if (CVPixelBufferPoolCreatePixelBuffer(nullptr, pool, &pb) != kCVReturnSuccess) continue;
    uint32_t f = g_frame++;
    uint64_t c0 = thread_cpu_ns();
    synth_fill(pb, f, 3);
    uint64_t cap = now_ns();
    synth_stamp(pb, f, cap);
    g_synth_cpu_ns += thread_cpu_ns() - c0;
    if (!s->t_first_cap) s->t_first_cap = cap;
    NSMutableDictionary *fo = [NSMutableDictionary dictionary];
    bool refresh = false, idr = false;
    {
      std::lock_guard<std::mutex> lk(s->mu);
      if (!s->ack_pending.empty()) {
        NSMutableArray *a = [NSMutableArray array];
        for (int64_t t : s->ack_pending) [a addObject:@(t)];
        fo[(id)kVTEncodeFrameOptionKey_AcknowledgedLTRTokens] = a;
        s->ack_pending.clear();
      }
      bool periodic = s->key_every > 0 && i > 0 && i % s->key_every == 0;
      if (s->want_idr || periodic) {
        idr = true;
        s->want_idr = s->want_refresh = false;
      } else if (s->want_refresh) {
        refresh = true;
        s->want_refresh = false;
      }
      if (refresh || idr) { s->last_refresh_frame = f; s->last_refresh_ns = cap; }
      if (refresh) s->refresh_n++;
      if (idr && !periodic) s->idr_n++;
    }
    if (idr) fo[(id)kVTEncodeFrameOptionKey_ForceKeyFrame] = @YES;
    if (refresh) fo[(id)kVTEncodeFrameOptionKey_ForceLTRRefresh] = @YES;
    if (refresh || idr) logj("{\"ev\":\"%s\",\"f\":%u,\"t\":%llu}", idr ? "force_idr" : "force_refresh", f, cap);
    EncRef *r = new EncRef{f, cap, now_ns(), refresh};
    CMTime pts = CMTimeMake((int64_t)i, 30);
    OSStatus st = VTCompressionSessionEncodeFrame(s->vt, pb, pts, CMTimeMake(1, 30), (__bridge CFDictionaryRef)fo, r, nullptr);
    if (st != noErr) { logj("{\"ev\":\"encode_failed\",\"f\":%u,\"st\":%d}", f, (int)st); delete r; }
    CVPixelBufferRelease(pb);
  }
}

static void audio_loop(Stream *s) {
  Audio a = {};
  a.ptype = kAudio;
  uint64_t t0 = now_ns();
  for (uint32_t i = 0; s->running; i++) {
    sleep_until_ns(t0 + (uint64_t)i * 10000000ULL);
    a.seq = i;
    a.send_ns = now_ns();
    tx(&a, sizeof a);
  }
}

static void send_state(bool running, uint32_t req, uint16_t sess) {
  State st = {};
  st.ptype = kState;
  st.running = running;
  st.sess = sess;
  st.req = req;
  st.mac_ns = now_ns();
  tx(&st, sizeof st);
}

static void stop_stream(const char *why) {
  Stream *s = g_st;
  if (!s) return;
  uint64_t t0 = now_ns();
  s->running = false;
  if (s->src.joinable()) s->src.join();
  if (s->aud.joinable()) s->aud.join();
  VTCompressionSessionCompleteFrames(s->vt, kCMTimeInvalid);
  VTCompressionSessionInvalidate(s->vt);
  CFRelease(s->vt);
  g_pacer.clear();
  logj("{\"ev\":\"stopped\",\"why\":\"%s\",\"sess\":%u,\"t0\":%llu,\"t\":%llu,\"refreshes\":%d,\"idrs_on_pli\":%d,\"plis\":%d}", why, s->sess, t0,
       now_ns(), s->refresh_n, s->idr_n, s->pli_n);
  delete s;
  g_st = nullptr;
}

static void handle_start(const Start *m, uint64_t t_rx) {
  std::lock_guard<std::mutex> lk(g_stream_mu);
  if (g_st && g_st->running && g_st->w == m->w && g_st->h == m->h) { send_state(true, m->req, g_st->sess); return; }
  if (g_st) stop_stream("restart");
  Stream *s = new Stream;
  s->w = m->w;
  s->h = m->h;
  s->bitrate = m->bitrate;
  s->idr_pace = m->idr_pace_x10 / 10.0;
  s->key_every = m->key_every;
  s->audio = m->audio;
  s->sess = ++g_sess;
  g_pacer.idr_bps = s->bitrate * s->idr_pace;
  logj("{\"ev\":\"start_rx\",\"req\":%u,\"t\":%llu,\"w\":%d,\"h\":%d,\"bitrate\":%lld,\"idr_pace\":%.1f,\"key_every\":%d,\"sess\":%u}", m->req, t_rx,
       s->w, s->h, (long long)s->bitrate, s->idr_pace, s->key_every, s->sess);
  uint64_t c0 = now_ns();
  if (!open_vt(s)) { delete s; return; }
  s->t_create_ns = now_ns();
  logj("{\"ev\":\"vt_created\",\"ms\":%.2f,\"t\":%llu}", (s->t_create_ns - c0) / 1e6, s->t_create_ns);
  s->running = true;
  g_st = s;
  g_last_rr = now_ns();
  s->src = std::thread(source_loop, s);
  if (s->audio) s->aud = std::thread(audio_loop, s);
  send_state(true, m->req, s->sess);
}

static void handle_rr(const RR *m) {
  g_last_rr = now_ns();
  std::lock_guard<std::mutex> lk(g_stream_mu);
  Stream *s = g_st;
  if (!s || m->sess != s->sess || m->acked_frame == UINT32_MAX) return;
  std::lock_guard<std::mutex> lk2(s->mu);
  for (auto it = s->tokens.begin(); it != s->tokens.end();) {
    if (it->first > m->acked_frame) break;
    if (it->first >= m->clean_from) {
      s->ack_pending.push_back(it->second);
      s->acked.insert(it->second);
      it = s->tokens.erase(it);
    } else if (m->acked_frame - it->first > 300) {
      it = s->tokens.erase(it);  // lost or too old
    } else {
      ++it;
    }
  }
}

static void handle_pli(const Pli *m) {
  std::lock_guard<std::mutex> lk(g_stream_mu);
  Stream *s = g_st;
  if (!s || m->sess != s->sess) return;
  std::lock_guard<std::mutex> lk2(s->mu);
  s->pli_n++;
  uint64_t now = now_ns();
  // a refresh produced after the lost frame is in flight: wait for it (300 ms)
  if (s->last_refresh_frame > m->lost_frame && now - s->last_refresh_ns < 300000000ULL) return;
  if (m->lost_frame != s->pli_lost) { s->pli_lost = m->lost_frame; s->refreshes_for_lost = 0; }
  s->refreshes_for_lost++;
  bool idr = s->acked.empty() || s->refreshes_for_lost > 3;
  if (idr) s->want_idr = true; else s->want_refresh = true;
  logj("{\"ev\":\"pli\",\"lost\":%u,\"last_clean\":%u,\"reason\":%u,\"t\":%llu,\"action\":\"%s\"}", m->lost_frame, m->last_clean, m->reason, now,
       idr ? "idr" : "refresh");
}

int main(int argc, char **argv) {
  @autoreleasepool {
    mach_timebase_info(&g_tb);
    signal(SIGPIPE, SIG_IGN);
    const char *peer = "192.168.1.134", *logp = nullptr;
    int port = kMacPort, pport = kPcPort;
    double seconds = 1e9;
    for (int i = 1; i < argc; i++) {
      std::string a = argv[i];
      if (a == "--peer" && i + 1 < argc) peer = argv[++i];
      else if (a == "--port" && i + 1 < argc) port = atoi(argv[++i]);
      else if (a == "--peer-port" && i + 1 < argc) pport = atoi(argv[++i]);
      else if (a == "--log" && i + 1 < argc) logp = argv[++i];
      else if (a == "--seconds" && i + 1 < argc) seconds = atof(argv[++i]);
      else { fprintf(stderr, "usage: camsend --peer IP [--port P] [--peer-port P] --log FILE [--seconds N]\n"); return 2; }
    }
    if (logp) { g_log = fopen(logp, "w"); setvbuf(g_log, nullptr, _IOLBF, 1 << 16); }
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1, buf = 4 << 20;
    setsockopt(g_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(g_sock, SOL_SOCKET, SO_SNDBUF, &buf, sizeof buf);
    setsockopt(g_sock, SOL_SOCKET, SO_RCVBUF, &buf, sizeof buf);
    int tos = 0xA0;  // CS5 like the audio engine's video class would be; harmless on this LAN
    setsockopt(g_sock, IPPROTO_IP, IP_TOS, &tos, sizeof tos);
    sockaddr_in me = {};
    me.sin_family = AF_INET;
    me.sin_port = htons(port);
    if (bind(g_sock, (sockaddr *)&me, sizeof me) != 0) { perror("bind"); return 1; }
    g_peer.sin_family = AF_INET;
    g_peer.sin_port = htons(pport);
    inet_pton(AF_INET, peer, &g_peer.sin_addr);
    // connected socket: the Mac sends first, so the application firewall never sees unsolicited traffic
    if (connect(g_sock, (sockaddr *)&g_peer, sizeof g_peer) != 0) { perror("connect"); return 1; }
    timeval tv = {0, 100000};
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    std::thread pacer([] { g_pacer.run(); });
    uint32_t boot = (uint32_t)now_ns();
    logj("{\"ev\":\"boot\",\"t\":%llu,\"peer\":\"%s\",\"port\":%d}", now_ns(), peer, port);
    uint64_t t_end = now_ns() + (uint64_t)(seconds * 1e9), next_hello = 0, next_stats = now_ns() + 10000000000ULL;
    bool peer_seen = false;
    double cpu0 = cpu_s();
    uint64_t wall0 = now_ns();
    uint8_t b[2048];
    while (now_ns() < t_end) {
      uint64_t now = now_ns();
      if (now >= next_hello) {
        Hello h = {kHello, 1, 0, boot};
        tx(&h, sizeof h);
        next_hello = now + (peer_seen ? 1000000000ULL : 100000000ULL);
      }
      if (now >= next_stats) {
        double c = cpu_s();
        logj("{\"ev\":\"stats\",\"t\":%llu,\"cpu_pct\":%.2f,\"synth_cpu_pct\":%.2f,\"rss_mb\":%.1f,\"tx_kbps\":%.0f,\"tx_pk\":%llu,\"rtx\":%llu,\"rtx_miss\":%llu,\"streaming\":%d}", now,
             (c - cpu0) / ((now - wall0) / 1e9) * 100, g_synth_cpu_ns.exchange(0) / 1e9 / ((now - wall0) / 1e9) * 100, rss_mb(), g_tx_bytes.exchange(0) * 8 / ((now - wall0) / 1e9) / 1000,
             (unsigned long long)g_tx_pk.exchange(0), g_pacer.rtx_sent, g_pacer.rtx_miss, g_st != nullptr);
        cpu0 = c;
        wall0 = now;
        next_stats = now + 10000000000ULL;
        std::lock_guard<std::mutex> lk(g_stream_mu);
        if (g_st && now - g_last_rr > 3000000000ULL) stop_stream("no_rr_3s");
      }
      ssize_t n = recv(g_sock, b, sizeof b, 0);
      if (n <= 0) continue;
      uint64_t t = now_ns();
      switch (b[0]) {
        case kHello:
          if (!peer_seen) logj("{\"ev\":\"peer\",\"t\":%llu}", t);
          peer_seen = true;
          break;
        case kPing:
          if (n >= (ssize_t)sizeof(Ping)) {
            Pong p = {};
            p.ptype = kPong;
            p.id = ((Ping *)b)->id;
            p.pc_us = ((Ping *)b)->pc_us;
            p.mac_ns = t;
            tx(&p, sizeof p);
          }
          break;
        case kStart:
          if (n >= (ssize_t)sizeof(Start)) handle_start((Start *)b, t);
          break;
        case kStop:
          if (n >= (ssize_t)sizeof(Stop)) {
            std::lock_guard<std::mutex> lk(g_stream_mu);
            uint16_t sess = g_st ? g_st->sess : 0;
            if (g_st) {
              logj("{\"ev\":\"stop_rx\",\"req\":%u,\"t\":%llu}", ((Stop *)b)->req, t);
              stop_stream("stop");
            }
            send_state(false, ((Stop *)b)->req, sess);
          }
          break;
        case kRR:
          if (n >= (ssize_t)sizeof(RR)) handle_rr((RR *)b);
          break;
        case kNack:
          if (n >= 4) {
            Nack *k = (Nack *)b;
            int cnt = std::min<int>(k->n, (int)((n - 4) / 4));
            logj("{\"ev\":\"nack\",\"t\":%llu,\"n\":%d,\"seq0\":%u}", t, cnt, cnt ? k->seq[0] : 0);
            g_pacer.nack(k->seq, cnt, k->sess);
          }
          break;
        case kPli:
          if (n >= (ssize_t)sizeof(Pli)) handle_pli((Pli *)b);
          break;
      }
    }
    {
      std::lock_guard<std::mutex> lk(g_stream_mu);
      stop_stream("exit");
    }
    g_pacer.stop = true;
    g_pacer.cv.notify_all();
    pacer.join();
    logj("{\"ev\":\"exit\",\"t\":%llu}", now_ns());
    if (g_log != stderr) fclose(g_log);
  }
  return 0;
}
