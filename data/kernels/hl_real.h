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

#ifndef DT_KERNELS_HL_REAL_H
#define DT_KERNELS_HL_REAL_H

// hl_real_t: the arithmetic of the sparse Cholesky solvers (highlights_sparse.cl), which need
// more than single precision -- the biharmonic systems they factor are conditioned like h^-4,
// about 3e8 at the DT_HL_SPARSE_MAX cap, and a float factorization loses every digit and
// hits a negative pivot. It is one of two things, and the HOST decides which, per device:
//
//   DT_DEVICE_FP64 defined   -> native double. The host sets -DDT_DEVICE_FP64=1 from
//                               CL_DEVICE_DOUBLE_FP_CONFIG (common/opencl.c), the same way
//                               it sets NVIDIA_SM_20. Every device with real fp64 keeps the
//                               arithmetic it always had, bit for bit.
//   otherwise                -> double-float: a float2 (hi, lo), |lo| <= ulp(hi)/2, carrying
//                               ~48 bits of mantissa out of two 24-bit floats. Knuth two-sum,
//                               Dekker/fma two-product, Newton division and square root.
//                               Apple's OpenCL-on-Metal has no fp64 at all; this is what lets
//                               the solver kernels build there.
//
// Do NOT decide this on the cl_khr_fp64 preprocessor macro: Apple's runtime DEFINES it without
// having the extension, the program then builds, and every kernel touching a double fails at
// clCreateKernel (-48). Measured on Apple M1, see CLAUDE.md.
//
// Both representations are 8 bytes, so the host allocates the same buffers either way and
// converts a double vector on its way in and out (_sp_cl_upload_real / _sp_cl_read_real in
// src/math/sparse_cholesky_cl.h). The two-product relies on fma() being a fused, exactly
// rounded operation; two-sum has no multiply, so -cl-mad-enable cannot contract it.

#ifdef DT_DEVICE_FP64

#pragma OPENCL EXTENSION cl_khr_fp64 : enable
typedef double hl_real_t;

static inline hl_real_t hl_real(const float x) { return (double)x; }
static inline float hl_float(const hl_real_t x) { return (float)x; }
static inline hl_real_t hl_zero(void) { return 0.0; }
static inline hl_real_t hl_add(const hl_real_t a, const hl_real_t b) { return a + b; }
static inline hl_real_t hl_sub(const hl_real_t a, const hl_real_t b) { return a - b; }
static inline hl_real_t hl_mul(const hl_real_t a, const hl_real_t b) { return a * b; }
static inline hl_real_t hl_div(const hl_real_t a, const hl_real_t b) { return a / b; }
static inline hl_real_t hl_sqrt(const hl_real_t a) { return sqrt(a); }

#else // double-float

typedef float2 hl_real_t;

// (s, e) with s = fl(a + b) and s + e == a + b exactly, whatever the magnitudes (Knuth)
static inline float2 hl_two_sum(const float a, const float b)
{
  const float s = a + b;
  const float bb = s - a;
  return (float2)(s, (a - (s - bb)) + (b - bb));
}

// same, valid when |a| >= |b| (Dekker): one operation fewer
static inline float2 hl_quick_two_sum(const float a, const float b)
{
  const float s = a + b;
  return (float2)(s, b - (s - a));
}

// (p, e) with p = fl(a * b) and p + e == a * b exactly, through the fused multiply-add
static inline float2 hl_two_prod(const float a, const float b)
{
  const float p = a * b;
  return (float2)(p, fma(a, b, -p));
}

static inline hl_real_t hl_real(const float x) { return (float2)(x, 0.f); }
static inline float hl_float(const hl_real_t x) { return x.x + x.y; }
static inline hl_real_t hl_zero(void) { return (float2)(0.f, 0.f); }

static inline hl_real_t hl_add(const hl_real_t a, const hl_real_t b)
{
  float2 s = hl_two_sum(a.x, b.x);
  const float2 t = hl_two_sum(a.y, b.y);
  s.y += t.x;
  s = hl_quick_two_sum(s.x, s.y);
  s.y += t.y;
  return hl_quick_two_sum(s.x, s.y);
}

static inline hl_real_t hl_sub(const hl_real_t a, const hl_real_t b)
{
  return hl_add(a, (float2)(-b.x, -b.y));
}

static inline hl_real_t hl_mul(const hl_real_t a, const hl_real_t b)
{
  float2 p = hl_two_prod(a.x, b.x);
  p.y += a.x * b.y + a.y * b.x;
  return hl_quick_two_sum(p.x, p.y);
}

// a / b: a float quotient corrected twice by the double-float remainder
static inline hl_real_t hl_div(const hl_real_t a, const hl_real_t b)
{
  const float q1 = a.x / b.x;
  hl_real_t r = hl_sub(a, hl_mul(b, hl_real(q1)));
  const float q2 = r.x / b.x;
  r = hl_sub(r, hl_mul(b, hl_real(q2)));
  const float q3 = r.x / b.x;
  const float2 q = hl_quick_two_sum(q1, q2);
  return hl_add(q, hl_real(q3));
}

// sqrt(a): a float root corrected once by Newton. A non-positive argument goes through the
// float sqrt so that zero stays zero and a negative pivot becomes NaN, as it does in double --
// the host validates the solution for finiteness and takes the same fallback either way.
static inline hl_real_t hl_sqrt(const hl_real_t a)
{
  if(a.x <= 0.f) return (float2)(sqrt(a.x), 0.f);
  const float s = sqrt(a.x);
  const hl_real_t r = hl_sub(a, hl_two_prod(s, s));
  return hl_quick_two_sum(s, r.x / (2.f * s));
}

#endif // DT_DEVICE_FP64

#endif // DT_KERNELS_HL_REAL_H
