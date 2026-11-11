// SPDX-License-Identifier: GPL-3.0-only
#include "packet.h"

#include <cstring>

#include "util.h"

namespace beamer::audio {

void write_header(const Header& h, uint8_t out[kHeaderLen]) {
  out[0] = h.version;
  out[1] = h.ptype;
  out[2] = h.flags;
  out[3] = h.codec;
  put_u32(out + 4, h.stream);
  put_u64(out + 8, h.seq);
  put_u32(out + 16, h.ts);
  put_u16(out + 20, h.nsamp);
  out[22] = h.channels;
  out[23] = h.reserved;
}

bool read_header(const uint8_t* d, size_t len, Header* h) {
  if (len < kHeaderLen) return false;
  h->version = d[0];
  h->ptype = d[1];
  h->flags = d[2];
  h->codec = d[3];
  h->stream = get_u32(d + 4);
  h->seq = get_u64(d + 8);
  h->ts = get_u32(d + 16);
  h->nsamp = get_u16(d + 20);
  h->channels = d[22];
  h->reserved = d[23];
  return h->version == kPacketVersion;
}

void make_nonce(uint32_t dir_stream, uint64_t seq, uint8_t out[kNonceLen]) {
  put_u32(out, dir_stream);
  put_u64(out + 4, seq);
}

void Sealer::init(const uint8_t key[kKeyLen], uint32_t dir_stream) {
  std::memcpy(key_, key, kKeyLen);
  dir_ = dir_stream;
  seq_ = 0;
}

size_t Sealer::seal(Header h, const uint8_t* payload, size_t len, uint8_t* out) {
  h.seq = ++seq_;
  write_header(h, out);
  uint8_t nonce[kNonceLen];
  make_nonce(dir_, h.seq, nonce);
  aead_seal(key_, nonce, out, kHeaderLen, payload, len, out + kHeaderLen);
  return kHeaderLen + len + kTagLen;
}

OpenResult open_packet_auth(const uint8_t key[kKeyLen], uint32_t dir_stream, const uint8_t* data, size_t len,
                            Header* h, uint8_t* payload_out, size_t* payload_len) {
  if (len < kMinPacket || len > kMaxPacket || !read_header(data, len, h)) return OpenResult::Rejected;
  uint8_t nonce[kNonceLen];
  make_nonce(dir_stream, h->seq, nonce);
  if (!aead_open(key, nonce, data, kHeaderLen, data + kHeaderLen, len - kHeaderLen, payload_out))
    return OpenResult::Rejected;
  *payload_len = len - kMinPacket;
  return OpenResult::Ok;
}

}  // namespace beamer::audio
