// SPDX-License-Identifier: GPL-3.0-only
#include "crypto.h"

#include "monocypher.h"

namespace beamer::audio {

namespace {

void put_le64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (8 * i));
}

// RFC 8439 §2.8: mac_data = aad | pad16 | ct | pad16 | le64(aad_len) | le64(ct_len)
void poly_tag(const uint8_t otk[32], const uint8_t* aad, size_t aad_len, const uint8_t* ct,
              size_t ct_len, uint8_t tag[16]) {
  static const uint8_t zeros[16] = {0};
  crypto_poly1305_ctx ctx;
  crypto_poly1305_init(&ctx, otk);
  crypto_poly1305_update(&ctx, aad, aad_len);
  if (aad_len % 16) crypto_poly1305_update(&ctx, zeros, 16 - aad_len % 16);
  crypto_poly1305_update(&ctx, ct, ct_len);
  if (ct_len % 16) crypto_poly1305_update(&ctx, zeros, 16 - ct_len % 16);
  uint8_t lens[16];
  put_le64(lens, aad_len);
  put_le64(lens + 8, ct_len);
  crypto_poly1305_update(&ctx, lens, 16);
  crypto_poly1305_final(&ctx, tag);
}

void one_time_key(const uint8_t key[32], const uint8_t nonce[12], uint8_t otk[32]) {
  // §2.6: first 32 bytes of the ChaCha20 block with counter 0
  uint8_t block[64] = {0};
  crypto_chacha20_ietf(block, block, 64, key, nonce, 0);
  for (int i = 0; i < 32; ++i) otk[i] = block[i];
  crypto_wipe(block, sizeof block);
}

}  // namespace

void aead_seal(const uint8_t key[kKeyLen], const uint8_t nonce[kNonceLen], const uint8_t* aad,
               size_t aad_len, const uint8_t* pt, size_t pt_len, uint8_t* out) {
  uint8_t otk[32];
  one_time_key(key, nonce, otk);
  if (pt_len) crypto_chacha20_ietf(out, pt, pt_len, key, nonce, 1);
  poly_tag(otk, aad, aad_len, out, pt_len, out + pt_len);
  crypto_wipe(otk, sizeof otk);
}

bool aead_open(const uint8_t key[kKeyLen], const uint8_t nonce[kNonceLen], const uint8_t* aad,
               size_t aad_len, const uint8_t* in, size_t in_len, uint8_t* out) {
  if (in_len < kTagLen) return false;
  size_t ct_len = in_len - kTagLen;
  uint8_t otk[32], tag[16];
  one_time_key(key, nonce, otk);
  poly_tag(otk, aad, aad_len, in, ct_len, tag);
  crypto_wipe(otk, sizeof otk);
  if (crypto_verify16(tag, in + ct_len) != 0) return false;
  if (ct_len) crypto_chacha20_ietf(out, in, ct_len, key, nonce, 1);
  return true;
}

void secure_wipe(void* p, size_t n) { crypto_wipe(p, n); }

}  // namespace beamer::audio
