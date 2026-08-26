/*
    This file is part of Ansel,
    Copyright (C) 2026 Aurélien PIERRE.

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Ansel is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Ansel.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifndef DT_COMMON_HASH_H
#define DT_COMMON_HASH_H

/* dt_hash(): the content-addressing primitive of the whole app (history, pipeline,
 * caches). Pure and dependency-free on purpose: low-level compute units include this
 * instead of darktable.h. */

#include "system/macros.h"  // DT_FALLTHROUGH

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Cryptographic-strength hash of `str` representing its state, with negligible
// collision probability compared to a plain multiplicative hash.
// This is SipHash-2-4 (Aumasson & Bernstein, https://131002.net/siphash/), a
// keyed pseudo-random function with 64-bit output. The incoming `hash` is folded
// in as the key material, so calls can be chained to combine several buffers:
//   hash = dt_hash(hash, a, sizeof(a));
//   hash = dt_hash(hash, b, sizeof(b));
// `hash` should be seeded to 5381 (or any constant) on the first call, or carried
// over from a previous dt_hash() result.
// NOTE: the digest is computed over the raw bytes in native endianness, so it is
// stable within a run/machine but not portable across architectures of differing
// endianness (same constraint as the previous implementation when hashing structs).
#define DT_SIPROUND                                                            \
  do                                                                           \
  {                                                                            \
    v0 += v1; v1 = (v1 << 13) | (v1 >> 51); v1 ^= v0; v0 = (v0 << 32) | (v0 >> 32); \
    v2 += v3; v3 = (v3 << 16) | (v3 >> 48); v3 ^= v2;                          \
    v0 += v3; v3 = (v3 << 21) | (v3 >> 43); v3 ^= v0;                          \
    v2 += v1; v1 = (v1 << 17) | (v1 >> 47); v1 ^= v2; v2 = (v2 << 32) | (v2 >> 32); \
  } while(0)

static inline uint64_t dt_hash(uint64_t hash, const char *str, size_t size)
{
  // Derive the 128-bit SipHash key from the chained seed so that chaining keeps
  // mixing prior state into the new digest. The second key word is a fixed
  // constant (fractional bits of the golden ratio) to add entropy.
  const uint64_t k0 = hash;
  const uint64_t k1 = 0x9e3779b97f4a7c15ULL;

  uint64_t v0 = 0x736f6d6570736575ULL ^ k0;
  uint64_t v1 = 0x646f72616e646f6dULL ^ k1;
  uint64_t v2 = 0x6c7967656e657261ULL ^ k0;
  uint64_t v3 = 0x7465646279746573ULL ^ k1;

  const uint8_t *in = (const uint8_t *)str;
  const size_t blocks = size & ~(size_t)7;
  size_t i = 0;
  for(; i < blocks; i += 8)
  {
    uint64_t m;
    __builtin_memcpy(&m, in + i, sizeof(m));
    v3 ^= m;
    DT_SIPROUND;
    DT_SIPROUND;
    v0 ^= m;
  }

  // Tail: remaining 0..7 bytes plus the length in the top byte.
  uint64_t b = (uint64_t)size << 56;
  switch(size & 7)
  {
    case 7: b |= (uint64_t)in[i + 6] << 48; DT_FALLTHROUGH;
    case 6: b |= (uint64_t)in[i + 5] << 40; DT_FALLTHROUGH;
    case 5: b |= (uint64_t)in[i + 4] << 32; DT_FALLTHROUGH;
    case 4: b |= (uint64_t)in[i + 3] << 24; DT_FALLTHROUGH;
    case 3: b |= (uint64_t)in[i + 2] << 16; DT_FALLTHROUGH;
    case 2: b |= (uint64_t)in[i + 1] << 8;  DT_FALLTHROUGH;
    case 1: b |= (uint64_t)in[i + 0];       DT_FALLTHROUGH;
    case 0: break;
  }
  v3 ^= b;
  DT_SIPROUND;
  DT_SIPROUND;
  v0 ^= b;

  // Finalization.
  v2 ^= 0xff;
  DT_SIPROUND;
  DT_SIPROUND;
  DT_SIPROUND;
  DT_SIPROUND;

  return v0 ^ v1 ^ v2 ^ v3;
}
#undef DT_SIPROUND

#ifdef __cplusplus
}
#endif

#endif // DT_COMMON_HASH_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
