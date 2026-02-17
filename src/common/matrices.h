/*
    This file is part of darktable,
    Copyright (C) 2010-2011 Henrik Andersson.
    Copyright (C) 2010 johannes hanika.
    Copyright (C) 2010 Pascal de Bruijn.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2013-2014 Jérémy Rosen.
    Copyright (C) 2016 Tobias Ellinghaus.
    Copyright (C) 2020 Pascal Obry.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2025 Aurélien PIERRE.
    
    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    
    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "common/math.h"

// a 3x3 matrix, padded to permit SSE instructions to be used for multiplication and addition
typedef float DT_ALIGNED_ARRAY dt_colormatrix_t[4][4];

/** inverts the given padded 3x3 matrix */
static inline int mat3SSEinv(dt_colormatrix_t dst, const dt_colormatrix_t src)
{
#define A(y, x) src[(y - 1)][(x - 1)]
#define B(y, x) dst[(y - 1)][(x - 1)]

  const float det = A(1, 1) * (A(3, 3) * A(2, 2) - A(3, 2) * A(2, 3))
                    - A(2, 1) * (A(3, 3) * A(1, 2) - A(3, 2) * A(1, 3))
                    + A(3, 1) * (A(2, 3) * A(1, 2) - A(2, 2) * A(1, 3));

  const float epsilon = 1e-7f;
  if(fabsf(det) < epsilon) return 1;

  const float invDet = 1.f / det;

  B(1, 1) = invDet * (A(3, 3) * A(2, 2) - A(3, 2) * A(2, 3));
  B(1, 2) = -invDet * (A(3, 3) * A(1, 2) - A(3, 2) * A(1, 3));
  B(1, 3) = invDet * (A(2, 3) * A(1, 2) - A(2, 2) * A(1, 3));

  B(2, 1) = -invDet * (A(3, 3) * A(2, 1) - A(3, 1) * A(2, 3));
  B(2, 2) = invDet * (A(3, 3) * A(1, 1) - A(3, 1) * A(1, 3));
  B(2, 3) = -invDet * (A(2, 3) * A(1, 1) - A(2, 1) * A(1, 3));

  B(3, 1) = invDet * (A(3, 2) * A(2, 1) - A(3, 1) * A(2, 2));
  B(3, 2) = -invDet * (A(3, 2) * A(1, 1) - A(3, 1) * A(1, 2));
  B(3, 3) = invDet * (A(2, 2) * A(1, 1) - A(2, 1) * A(1, 2));
#undef A
#undef B
  return 0;
}


// transpose a padded 3x3 matrix
static inline void transpose_3xSSE(const dt_colormatrix_t input, dt_colormatrix_t output)
{
  output[0][0] = input[0][0];
  output[0][1] = input[1][0];
  output[0][2] = input[2][0];
  output[0][3] = 0.0f;

  output[1][0] = input[0][1];
  output[1][1] = input[1][1];
  output[1][2] = input[2][1];
  output[1][3] = 0.0f;

  output[2][0] = input[0][2];
  output[2][1] = input[1][2];
  output[2][2] = input[2][2];
  output[2][3] = 0.0f;

  for_four_channels(c, aligned(output))
    output[3][c] = 0.0f;
}


// transpose and pad a 3x3 matrix into the padded format optimized for vectorization
static inline void transpose_3x3_to_3xSSE(const float input[9], dt_colormatrix_t output)
{
  output[0][0] = input[0];
  output[0][1] = input[3];
  output[0][2] = input[6];
  output[0][3] = 0.0f;

  output[1][0] = input[1];
  output[1][1] = input[4];
  output[1][2] = input[7];
  output[1][3] = 0.0f;

  output[2][0] = input[2];
  output[2][1] = input[5];
  output[2][2] = input[8];
  output[2][3] = 0.0f;

  for_four_channels(c, aligned(output))
    output[3][c] = 0.0f;
}

// convert a 3x3 matrix into the padded format optimized for vectorization
static inline void repack_double3x3_to_3xSSE(const double input[9], dt_colormatrix_t output)
{
  output[0][0] = input[0];
  output[0][1] = input[1];
  output[0][2] = input[2];
  output[0][3] = 0.0f;

  output[1][0] = input[3];
  output[1][1] = input[4];
  output[1][2] = input[5];
  output[1][3] = 0.0f;

  output[2][0] = input[6];
  output[2][1] = input[7];
  output[2][2] = input[8];
  output[2][3] = 0.0f;

  for(size_t c = 0; c < 4; c++)
    output[3][c] = 0.0f;
}

// convert a 3x3 matrix into the padded format optimized for vectorization
static inline void pack_3xSSE_to_3x3(const dt_colormatrix_t input, float output[9])
{
  output[0] = input[0][0];
  output[1] = input[0][1];
  output[2] = input[0][2];
  output[3] = input[1][0];
  output[4] = input[1][1];
  output[5] = input[1][2];
  output[6] = input[2][0];
  output[7] = input[2][1];
  output[8] = input[2][2];
}

// vectorized multiplication of padded 3x3 matrices
static inline void dt_colormatrix_mul(dt_colormatrix_t dst, const dt_colormatrix_t m1, const dt_colormatrix_t m2)
{
  for(int k = 0; k < 3; ++k)
  {
    dt_aligned_pixel_t sum = { 0.0f };
    for_each_channel(i)
    {
      for(int j = 0; j < 3; j++)
        sum[i] += m1[k][j] * m2[j][i];
      dst[k][i] = sum[i];
    }
  }
}

// multiply two padded 3x3 matrices
// dest needs to be different from m1 and m2
// dest = m1 * m2 in this order
// TODO: see if that refactors with the previous
#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline void mat3SSEmul(dt_colormatrix_t dest, const dt_colormatrix_t m1, const dt_colormatrix_t m2)
{
  for(int k = 0; k < 3; k++)
  {
    for(int i = 0; i < 3; i++)
    {
      float x = 0.0f;
      for(int j = 0; j < 3; j++)
        x += m1[k][j] * m2[j][i];
      dest[k][i] = x;
    }
  }
}

#ifdef _OPENMP
#pragma omp declare simd uniform(M) aligned(M:64) aligned(v_in, v_out:16)
#endif
static inline void dot_product(const dt_aligned_pixel_t v_in, const dt_colormatrix_t M, dt_aligned_pixel_t v_out)
{
  // specialized 3x4 dot products of 4x1 RGB-alpha pixels
  #ifdef _OPENMP
  #pragma omp simd aligned(M:64) aligned(v_in, v_out:16)
  #endif
  for(size_t i = 0; i < 3; ++i) v_out[i] = scalar_product(v_in, M[i]);
}



// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
