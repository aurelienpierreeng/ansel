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
#ifndef DT_SYSTEM_SIMD_H
#define DT_SYSTEM_SIMD_H

/* SIMD pixel primitives: the 4-float aligned pixel type, its vector helpers and the
 * per-channel loop macros. Self-contained on purpose: low-level compute units include
 * this instead of darktable.h. */

#include "system/mem_alloc.h"
#include "system/openmp.h"

#include <glib.h>
#include <math.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h> // needed for _mm_stream_ps
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Most code in dt assumes that the compiler is capable of auto-vectorization.  In some cases, this will yield
// suboptimal code if the compiler in fact does NOT auto-vectorize.  Uncomment the following line for such a
// compiler.
//#define DT_NO_VECTORIZATION

// For some combinations of compiler and architecture, the compiler may actually emit inferior code if given
// a hint to vectorize a loop.  Uncomment the following line if such a combination is the compilation target.
//#define DT_NO_SIMD_HINTS

// utility type to ease declaration of aligned small arrays to hold a pixel (and document their purpose)
typedef DT_ALIGNED_PIXEL float dt_aligned_pixel_t[4];
// SIMD view matching dt_aligned_pixel_t layout, for explicit 4-float vector math.
typedef float dt_aligned_pixel_simd_t __attribute__((vector_size(16), aligned(16)));

// To be able to vectorize per-pixel loops, we need to operate on all four channels, but if the compiler does
// not auto-vectorize, doing so increases computation by 1/3 for a channel which typically is ignored anyway.
// Select the appropriate number of channels over which to loop to produce the fastest code.
#ifdef DT_NO_VECTORIZATION
#define DT_PIXEL_SIMD_CHANNELS 3
#else
#define DT_PIXEL_SIMD_CHANNELS 4
#endif

// A macro which gives us a configurable shorthand to produce the optimal performance when processing all of the
// channels in a pixel.  Its first argument is the name of the variable to be used inside the 'for' loop it creates,
// while the optional second argument is a set of OpenMP directives, typically specifying variable alignment.
// If indexing off of the begining of any buffer allocated with dt's image or aligned allocation functions, the
// alignment to specify is 64; otherwise, use 16, as there may have been an odd number of pixels from the start.
// Sample usage:
//         for_each_channel(k,aligned(src,dest:16))
//         {
//           src[k] = dest[k] / 3.0f;
//         }
#if defined(_OPENMP) && defined(OPENMP_SIMD_) && !defined(DT_NO_SIMD_HINTS)
//https://stackoverflow.com/questions/45762357/how-to-concatenate-strings-in-the-arguments-of-pragma
#define _DT_Pragma_(x) _Pragma(#x)
#define _DT_Pragma(x) _DT_Pragma_(x)
#define for_each_channel(_var, ...) \
  _DT_Pragma(omp simd __VA_ARGS__) \
  for (size_t _var = 0; _var < DT_PIXEL_SIMD_CHANNELS; _var++)
#define for_four_channels(_var, ...) \
  _DT_Pragma(omp simd __VA_ARGS__) \
  for (size_t _var = 0; _var < 4; _var++)
#else
#define for_each_channel(_var, ...) \
  for (size_t _var = 0; _var < DT_PIXEL_SIMD_CHANNELS; _var++)
#define for_four_channels(_var, ...) \
  for (size_t _var = 0; _var < 4; _var++)
#endif

static inline __attribute__((always_inline)) dt_aligned_pixel_simd_t dt_simd_set1(const float value)
{
  return (dt_aligned_pixel_simd_t){ value, value, value, value };
}

static inline __attribute__((always_inline)) dt_aligned_pixel_simd_t
dt_simd_abs(const dt_aligned_pixel_simd_t value)
{
  dt_aligned_pixel_simd_t out = value;
  for(int c = 0; c < 4; c++)
    out[c] = fabsf(value[c]);
  return out;
}

static inline __attribute__((always_inline)) dt_aligned_pixel_simd_t
dt_simd_max_zero(const dt_aligned_pixel_simd_t value)
{
  dt_aligned_pixel_simd_t out = value;
  for(int c = 0; c < 4; c++)
    out[c] = (isfinite(value[c])) ? MAX(value[c], 0.0f) : 0.f;
  return out;
}

static inline __attribute__((always_inline)) dt_aligned_pixel_simd_t
dt_simd_copysign(const dt_aligned_pixel_simd_t magnitude, const dt_aligned_pixel_simd_t sign)
{
  dt_aligned_pixel_simd_t out = magnitude;
  for(int c = 0; c < 4; c++)
    out[c] = copysignf(magnitude[c], sign[c]);
  return out;
}

static inline __attribute__((always_inline)) dt_aligned_pixel_simd_t
dt_simd_pow(const dt_aligned_pixel_simd_t base, const dt_aligned_pixel_simd_t exponent)
{
  dt_aligned_pixel_simd_t out = base;
  for(int c = 0; c < 4; c++)
    out[c] = powf(base[c], exponent[c]);
  return out;
}

static inline __attribute__((always_inline)) dt_aligned_pixel_simd_t
dt_load_simd(const float *const pixel)
{
  dt_aligned_pixel_simd_t out;
  __builtin_memcpy(&out, pixel, sizeof(out));
  return out;
}

static inline __attribute__((always_inline)) void
dt_store_simd(float *const pixel, const dt_aligned_pixel_simd_t value)
{
  __builtin_memcpy(pixel, &value, sizeof(value));
}

static inline __attribute__((always_inline)) dt_aligned_pixel_simd_t
dt_load_simd_aligned(const float *const pixel)
{
  const float *const in = (const float *const)__builtin_assume_aligned(pixel, 16);
  return dt_load_simd(in);
}

static inline __attribute__((always_inline)) void
dt_store_simd_aligned(float *const pixel, const dt_aligned_pixel_simd_t value)
{
  float *const out = (float *const)__builtin_assume_aligned(pixel, 16);
  dt_store_simd(out, value);
}

static inline __attribute__((always_inline)) void
dt_store_simd_nontemporal(float *const pixel, const dt_aligned_pixel_simd_t value)
{
  float *const out = (float *const)__builtin_assume_aligned(pixel, 16);

#if defined(__x86_64__) || defined(__i386__)
  const union
  {
    dt_aligned_pixel_simd_t simd;
    __m128 sse;
  } cast = { .simd = value };
  _mm_stream_ps(out, cast.sse);
#elif defined(__aarch64__)
  const union
  {
    dt_aligned_pixel_simd_t simd;
    float32x4_t neon;
  } cast = { .simd = value };
  vst1q_f32(out, cast.neon);
#elif (__clang__+0 > 7) && (__clang__+0 < 10)
  for_each_channel(k,aligned(out:16)) __builtin_nontemporal_store(value[k], out[k]);
#else
  for_each_channel(k,aligned(out:16) dt_omp_nontemporal(out)) out[k] = value[k];
#endif
}

static inline __attribute__((always_inline)) dt_aligned_pixel_simd_t
dt_mat3x4_mul_vec4(const dt_aligned_pixel_simd_t in, const dt_aligned_pixel_simd_t row0,
                   const dt_aligned_pixel_simd_t row1, const dt_aligned_pixel_simd_t row2)
{
  // Keep the multiply first in each accumulation step so GCC contracts this
  // into chained FMA instructions in the multiversioned FMA clones too.
  dt_aligned_pixel_simd_t out = row0 * in[0];
  out = row1 * in[1] + out;
  return row2 * in[2] + out;
}

// copy the RGB channels of a pixel using nontemporal stores if
// possible; includes the 'alpha' channel as well if faster due to
// vectorization, but subsequent code should ignore the value of the
// alpha unless explicitly set afterwards (since it might not have
// been copied).  NOTE: nontemporal stores will actually be *slower*
// if we immediately access the pixel again.  This function should
// only be used when processing an entire image before doing anything
// else with the destination buffer.
static inline void copy_pixel_nontemporal(
	float *const __restrict__ out,
        const float *const __restrict__ in)
{
  dt_store_simd_nontemporal(out, dt_load_simd(in));
}

// copy the RGB channels of a pixel; includes the 'alpha' channel as well if faster due to vectorization, but
// subsequent code should ignore the value of the alpha unless explicitly set afterwards (since it might not have
// been copied)
static inline void copy_pixel(float *const __restrict__ out, const float *const __restrict__ in)
{
  for_each_channel(k,aligned(in,out:16)) out[k] = in[k];
}

#ifdef __cplusplus
}
#endif

#endif // DT_SYSTEM_SIMD_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
