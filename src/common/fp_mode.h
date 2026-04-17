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

/**
 * @brief Enable aggressive floating-point arithmetic optimizations, in
 * denormals handling. Set through user preference `cpu_fp_mode`
 * 
 * @param mode 
 */
void dt_fp_init(dt_cpu_fp_mode_t mode);

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