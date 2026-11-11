// SPDX-License-Identifier: GPL-3.0-only
#include "video_packet.h"

#include <algorithm>

#include "util.h"

namespace beamer::audio {

void write_video_header(const VideoHeader& v, uint8_t out[kVideoHeaderLen]) {
  put_u32(out, v.frame);
  put_u16(out + 4, v.frag);
  put_u16(out + 6, v.nfrag);
  out[8] = v.vflags;
  out[9] = v.ltr;
  put_u16(out + 10, v.enc_delay);
}

bool read_video_header(const uint8_t* p, size_t len, VideoHeader* v) {
  if (len < kVideoHeaderLen) return false;
  v->frame = get_u32(p);
  v->frag = get_u16(p + 4);
  v->nfrag = get_u16(p + 6);
  v->vflags = p[8];
  v->ltr = p[9];
  v->enc_delay = get_u16(p + 10);
  if (v->nfrag == 0 || v->nfrag > kMaxNfrag || v->frag >= v->nfrag) return false;
  if (v->vflags & 0xF0) return false;
  return true;
}

uint16_t enc_delay_units(int64_t ns) {
  if (ns <= 0) return 0;
  const int64_t u = ns / 100'000;
  return u >= 65535 ? 65535 : static_cast<uint16_t>(u);
}

size_t write_nack(const std::vector<NackEntry>& e, uint8_t* out) {
  const size_t n = std::min(e.size(), kMaxNackEntries);
  out[0] = static_cast<uint8_t>(n);
  for (size_t i = 0; i < n; ++i) {
    put_u64(out + 1 + i * 10, e[i].seq_base);
    put_u16(out + 1 + i * 10 + 8, e[i].mask);
  }
  return 1 + n * 10;
}

bool read_nack(const uint8_t* p, size_t len, std::vector<NackEntry>* out) {
  out->clear();
  if (len < 1) return false;
  const size_t n = p[0];
  if (n < 1 || n > kMaxNackEntries || len < 1 + n * 10) return false;
  for (size_t i = 0; i < n; ++i) out->push_back(NackEntry{get_u64(p + 1 + i * 10), get_u16(p + 1 + i * 10 + 8)});
  return true;
}

size_t pack_nack(const std::vector<uint64_t>& s, std::vector<NackEntry>* out) {
  out->clear();
  size_t i = 0;
  while (i < s.size() && out->size() < kMaxNackEntries) {
    NackEntry e;
    e.seq_base = s[i++];
    while (i < s.size() && s[i] > e.seq_base && s[i] - e.seq_base <= 16) {
      e.mask |= static_cast<uint16_t>(1u << (s[i] - e.seq_base - 1));
      ++i;
    }
    out->push_back(e);
  }
  return i;
}

void unpack_nack(const std::vector<NackEntry>& e, std::vector<uint64_t>* seqs) {
  seqs->clear();
  for (auto& x : e) {
    seqs->push_back(x.seq_base);
    for (int i = 0; i < 16; ++i)
      if (x.mask & (1u << i)) seqs->push_back(x.seq_base + 1 + uint64_t(i));
  }
}

void write_pli(const Pli& p, uint8_t out[kPliLen]) {
  put_u32(out, p.last_clean);
  out[4] = p.reason;
  out[5] = 0;
}

bool read_pli(const uint8_t* p, size_t len, Pli* out) {
  if (len < kPliLen || p[4] > kPliSize) return false;
  out->last_clean = get_u32(p);
  out->reason = p[4];
  return true;
}

const char* pli_reason_name(uint8_t r) {
  switch (r) {
    case kPliLoss: return "loss";
    case kPliStart: return "start";
    case kPliDecoder: return "decoder";
    case kPliStalled: return "stalled";
    case kPliSize: return "size";
  }
  return "?";
}

size_t write_time_map(const std::vector<TimeMapEntry>& e, uint8_t* out) {
  const size_t n = std::min(e.size(), kMaxTimeMapEntries);
  out[0] = static_cast<uint8_t>(n);
  for (size_t i = 0; i < n; ++i) {
    uint8_t* q = out + 1 + i * 16;
    put_u32(q, e[i].stream);
    put_u32(q + 4, e[i].ts);
    put_u64(q + 8, e[i].sender_ns);
  }
  return 1 + n * 16;
}

bool read_time_map(const uint8_t* p, size_t len, std::vector<TimeMapEntry>* out) {
  out->clear();
  if (len < 1) return false;
  const size_t n = p[0];
  if (n < 1 || n > kMaxTimeMapEntries || len < 1 + n * 16) return false;
  for (size_t i = 0; i < n; ++i) {
    const uint8_t* q = p + 1 + i * 16;
    out->push_back(TimeMapEntry{get_u32(q), get_u32(q + 4), get_u64(q + 8)});
  }
  return true;
}

void write_video_rr(const VideoRR& r, uint8_t out[kVideoRRLen]) {
  put_u32(out, r.newest_ts);
  put_u16(out + 4, r.margin);
  put_u32(out + 6, r.hi_seq_lo);
  put_u32(out + 10, r.lost);
  put_u16(out + 14, r.late);
  put_u32(out + 16, r.clean_since);
  put_u32(out + 20, r.acked);
  put_u16(out + 24, r.recv_kbps);
  out[26] = r.flags;
  out[27] = 0;
}

bool read_video_rr(const uint8_t* p, size_t len, VideoRR* r) {
  if (len < kVideoRRLen) return false;
  r->newest_ts = get_u32(p);
  r->margin = get_u16(p + 4);
  r->hi_seq_lo = get_u32(p + 6);
  r->lost = get_u32(p + 10);
  r->late = get_u16(p + 14);
  r->clean_since = get_u32(p + 16);
  r->acked = get_u32(p + 20);
  r->recv_kbps = get_u16(p + 24);
  r->flags = p[26];
  return true;
}

}  // namespace beamer::audio
