/*
    This file is part of Ansel,
    Copyright (C) 2022-2026 Aurélien PIERRE.

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

#include "fp_mode.h"
#include <math.h>

#if defined(__x86_64__) || defined(__i386__)
  #include <xmmintrin.h>
#endif

#if defined(__aarch64__)
  #include <fenv.h>
#endif

static void set_fast_mode(void)
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

static void set_strict_mode(void)
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

void dt_fp_init(dt_cpu_fp_mode_t mode)
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