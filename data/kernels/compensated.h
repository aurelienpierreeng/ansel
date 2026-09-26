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

#ifndef DT_KERNELS_COMPENSATED_H
#define DT_KERNELS_COMPENSATED_H

// Compensated single-precision summation, for the reductions that used to accumulate in
// double and narrow their result to float on the way out. A sum is carried as a float2
// (value, running error): every addition captures the bit its rounding lost through
// Knuth's two-sum -- exact, s + e == a + b, whatever the magnitudes -- and folds it into
// the error lane. The bound is a couple of ulp on the result, independent of the number of
// terms, which is at least what the double gave once narrowed. Nothing here multiplies, so
// the -cl-mad-enable option every device is built with cannot contract any of it.
//
// It exists because cl_khr_fp64 is an OPTIONAL OpenCL extension: Apple's runtime has none,
// and the reductions in highlights_harmonic.cl / highlights_sparse.cl were the reason five
// of that module's GPU stages fell back to the CPU there.

static inline float2 csum_zero(void)
{
  return (float2)(0.f, 0.f);
}

// Knuth two-sum: (s, e) with s = fl(a + b) and s + e == a + b exactly.
static inline float2 csum_two_sum(const float a, const float b)
{
  const float s = a + b;
  const float bb = s - a;
  const float e = (a - (s - bb)) + (b - bb);
  return (float2)(s, e);
}

// acc + x, the rounding error of the addition kept in the error lane
static inline float2 csum_add(const float2 acc, const float x)
{
  const float2 t = csum_two_sum(acc.x, x);
  return (float2)(t.x, acc.y + t.y);
}

// merge two partial sums, for the tree reductions in local memory
static inline float2 csum_merge(const float2 a, const float2 b)
{
  const float2 t = csum_two_sum(a.x, b.x);
  return (float2)(t.x, a.y + b.y + t.y);
}

// the value of a compensated sum
static inline float csum_value(const float2 acc)
{
  return acc.x + acc.y;
}

#endif // DT_KERNELS_COMPENSATED_H
