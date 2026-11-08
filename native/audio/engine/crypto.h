// SPDX-License-Identifier: GPL-3.0-only
// ChaCha20-Poly1305 AEAD exactly as RFC 8439 §2.8, on Monocypher's chacha20_ietf + poly1305
// primitives (one independent construction per packet; not Monocypher's ratcheting AEAD).
#pragma once

#include <cstddef>
#include <cstdint>

namespace beamer::audio {

constexpr size_t kKeyLen = 32;
constexpr size_t kNonceLen = 12;
constexpr size_t kTagLen = 16;

// out receives pt_len bytes of ciphertext followed by the 16-byte tag. out may alias pt.
void aead_seal(const uint8_t key[kKeyLen], const uint8_t nonce[kNonceLen], const uint8_t* aad,
               size_t aad_len, const uint8_t* pt, size_t pt_len, uint8_t* out);

// in = ciphertext || tag (in_len >= 16). On success writes in_len - 16 bytes to out and returns
// true; on a bad tag writes nothing and returns false. out may alias in.
bool aead_open(const uint8_t key[kKeyLen], const uint8_t nonce[kNonceLen], const uint8_t* aad,
               size_t aad_len, const uint8_t* in, size_t in_len, uint8_t* out);

void secure_wipe(void* p, size_t n);

}  // namespace beamer::audio
