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
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include <math.h>

#if defined(__x86_64__) || defined(__i386__)
  #include <xmmintrin.h>
#endif

#if defined(__aarch64__)
  #include <fenv.h>
#endif

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DT_FP_MODE_DEFAULT = 0,
    DT_FP_MODE_FAST,   // fast
    DT_FP_MODE_STRICT  // debug/scientific
} dt_cpu_fp_mode_t;

#if defined(_MSC_VER)
#define DT_ALWAYS_INLINE __forceinline
#else
#define DT_ALWAYS_INLINE __attribute__((always_inline)) inline
#endif

static DT_ALWAYS_INLINE void set_fast_mode(void)
{
#if defined(__x86_64__) || defined(__i386__)
  unsigned int mxcsr = _mm_getcsr();

  // Flush denormals to zero
  mxcsr |= _MM_FLUSH_ZERO_ON;

  // (optional if available)
#ifdef _MM_DENORMALS_ZERO_ON
  mxcsr |= _MM_DENORMALS_ZERO_ON;
#endif

  _mm_setcsr(mxcsr);
#endif

#if defined(__aarch64__)
  // Best-effort: ARM usually already fast for denormals in SIMD paths
  fesetenv(FE_DFL_ENV);
#endif
}

static DT_ALWAYS_INLINE void set_strict_mode(void)
{
#if defined(__x86_64__) || defined(__i386__)
  unsigned int mxcsr = _mm_getcsr();

  // Disable FTZ
  mxcsr &= ~_MM_FLUSH_ZERO_ON;

  _mm_setcsr(mxcsr);
#endif

#if defined(__aarch64__)
  fesetenv(FE_DFL_ENV);
#endif
}

/**
 * @brief Enable aggressive floating-point arithmetic optimizations, in
 * denormals handling. Set through user preference `cpu_fp_mode`
 * 
 * @param mode 
 */
static DT_ALWAYS_INLINE void dt_fp_init(const dt_cpu_fp_mode_t mode)
{
  switch(mode)
  {
  case DT_FP_MODE_FAST:
    set_fast_mode();
    break;

  case DT_FP_MODE_STRICT:
    set_strict_mode();
    break;

  default:
    // leave defaults unchanged
    break;
  }
}

static inline void dt_fp_print(const char *tag)
{
#if defined(__x86_64__) || defined(__i386__)
  unsigned int mxcsr = _mm_getcsr();

  fprintf(stdout, "[%s] MXCSR = 0x%08x\n", tag, mxcsr);

  fprintf(stdout, "  FTZ  : %s\n", (mxcsr & _MM_FLUSH_ZERO_ON) ? "ON" : "OFF");

#ifdef _MM_DENORMALS_ZERO_ON
  fprintf(stdout, "  DAZ  : %s\n", (mxcsr & _MM_DENORMALS_ZERO_ON) ? "ON" : "OFF");
#endif

  fprintf(stdout, "  exceptions mask: 0x%04x\n", (mxcsr >> 7) & 0x3f);
#endif
}

#ifdef __cplusplus
}
#endif