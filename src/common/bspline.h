/*
    This file is part of the Ansel project.
    Copyright (C) 2021-2023, 2025 Aurélien PIERRE.
    Copyright (C) 2021 Pascal Obry.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2023 Luca Zulberti.
    
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
#pragma once

#include "common/darktable.h"
#include "common/dwt.h"
#include "develop/openmp_maths.h"
#include "math.h"

// B spline filter
#define BSPLINE_FSIZE 5

// The B spline best approximate a Gaussian of standard deviation :
// see https://eng.aurelienpierre.com/2021/03/rotation-invariant-laplacian-for-2d-grids/
#define B_SPLINE_SIGMA 1.0553651328015339f

static inline float normalize_laplacian(const float sigma)
{
  // Normalize the wavelet scale to approximate a laplacian
  // see https://eng.aurelienpierre.com/2021/03/rotation-invariant-laplacian-for-2d-grids/#Scaling-coefficient
  return 2.f / sqf(sigma);
}

// Normalization scaling of the wavelet to approximate a laplacian
// from the function above for sigma = B_SPLINE_SIGMA as a constant
#define B_SPLINE_TO_LAPLACIAN 3.182727439285017f
#define B_SPLINE_TO_LAPLACIAN_2 10.129753952777762f // square

static inline float equivalent_sigma_at_step(const float sigma, const unsigned int s)
{
  // If we stack several gaussian blurs of standard deviation sigma on top of each other,
  // this is the equivalent standard deviation we get at the end (after s steps)
  // First step is s = 0
  // see
  // https://eng.aurelienpierre.com/2021/03/rotation-invariant-laplacian-for-2d-grids/#Multi-scale-iterative-scheme
  if(s == 0)
    return sigma;
  else
    return sqrtf(sqf(equivalent_sigma_at_step(sigma, s - 1)) + sqf(exp2f((float)s) * sigma));
}

static inline unsigned int num_steps_to_reach_equivalent_sigma(const float sigma_filter, const float sigma_final)
{
  // The inverse of the above : compute the number of scales needed to reach the desired equivalent sigma_final
  // after sequential blurs of constant sigma_filter
  unsigned int s = 0;
  float radius = sigma_filter;
  while(radius < sigma_final)
  {
    ++s;
    radius = sqrtf(sqf(radius) + sqf((float)(1 << s) * sigma_filter));
  }
  return s + 1;
}

static inline size_t decimated_bspline_size(const size_t size)
{
  return (size - 1u) / 2u + 1u;
}
static inline void sparse_scalar_product(const dt_aligned_pixel_t buf, const size_t indices[BSPLINE_FSIZE],
                                         dt_aligned_pixel_t result, const gboolean clip_negatives)
{
  // scalar product of 2 3x5 vectors stored as RGB planes and B-spline filter,
  // e.g. RRRRR - GGGGG - BBBBB
  static const float filter[BSPLINE_FSIZE] = { 1.0f / 16.0f,
                                               4.0f / 16.0f,
                                               6.0f / 16.0f,
                                               4.0f / 16.0f,
                                               1.0f / 16.0f };

  if(clip_negatives)
  {
    for_each_channel(c, aligned(buf,indices,result))
    {
      result[c] = MAX(0.0f, filter[0] * buf[indices[0] + c] +
                            filter[1] * buf[indices[1] + c] +
                            filter[2] * buf[indices[2] + c] +
                            filter[3] * buf[indices[3] + c] +
                            filter[4] * buf[indices[4] + c]);
    }
  }
  else
  {
    for_each_channel(c, aligned(buf,indices,result))
    {
      result[c] = filter[0] * buf[indices[0] + c] +
                  filter[1] * buf[indices[1] + c] +
                  filter[2] * buf[indices[2] + c] +
                  filter[3] * buf[indices[3] + c] +
                  filter[4] * buf[indices[4] + c];
    }
  }
}
static inline void _bspline_vertical_pass(const float *const restrict in, float *const restrict temp,
                                          size_t row, size_t width, size_t height, int mult, const gboolean clip_negatives)
{
  size_t DT_ALIGNED_ARRAY indices[BSPLINE_FSIZE];
  // compute the index offsets of the pixels of interest; since the offsets are the same for the entire row,
  // we only need to do this once and can then process the entire row
  indices[0] = 4 * width * MAX((int)row - 2 * mult, 0);
  indices[1] = 4 * width * MAX((int)row - mult, 0);
  indices[2] = 4 * width * row;
  indices[3] = 4 * width * MIN(row + mult, height-1);
  indices[4] = 4 * width * MIN(row + 2 * mult, height-1);
  for(size_t j = 0; j < width; j++)
  {
    // Compute the vertical blur of the current pixel and store it in the temp buffer for the row
    sparse_scalar_product(in + j * 4, indices, temp + j * 4, clip_negatives);
  }
}

#ifdef _OPENMP
#pragma omp declare simd aligned(temp, out)
#endif
static inline void _bspline_horizontal(const float *const restrict temp, float *const restrict out,
                                       size_t col, size_t width, int mult, const gboolean clip_negatives)
{
  // Compute the array indices of the pixels of interest; since the offsets will change near the ends of
  // the row, we need to recompute for each pixel
  size_t DT_ALIGNED_ARRAY indices[BSPLINE_FSIZE];
  indices[0] = 4 * MAX((int)col - 2 * mult, 0);
  indices[1] = 4 * MAX((int)col - mult,  0);
  indices[2] = 4 * col;
  indices[3] = 4 * MIN(col + mult, width-1);
  indices[4] = 4 * MIN(col + 2 * mult, width-1);
  // Compute the horizontal blur of the already vertically-blurred pixel and store the result at the proper
  //  row/column location in the output buffer
  sparse_scalar_product(temp, indices, out, clip_negatives);
}

#ifdef _OPENMP
#pragma omp declare simd aligned(temp, out)
#endif
static inline void _bspline_horizontal_decimated(const float *const restrict temp, float *const restrict out,
                                                 const size_t col, const size_t width,
                                                 const gboolean clip_negatives)
{
  // The vertical pass has already been evaluated on the fine grid. We now only
  // sample the horizontal convolution on the even columns kept by the decimated
  // spline pyramid.
  const size_t center = col * 2u;
  size_t DT_ALIGNED_ARRAY indices[BSPLINE_FSIZE];
  indices[0] = 4 * MAX((int)center - 2, 0);
  indices[1] = 4 * MAX((int)center - 1, 0);
  indices[2] = 4 * center;
  indices[3] = 4 * MIN(center + 1, width - 1);
  indices[4] = 4 * MIN(center + 2, width - 1);
  sparse_scalar_product(temp, indices, out, clip_negatives);
}
inline static void reduce_2D_Bspline(const float *const restrict in, float *const restrict out,
                                     const size_t width, const size_t height,
                                     float *const restrict tempbuf, const size_t padded_size,
                                     const gboolean clip_negatives)
{
  const size_t coarse_width = decimated_bspline_size(width);
  const size_t coarse_height = decimated_bspline_size(height);
  const gboolean use_replicated_boundary = (coarse_width > 2u && coarse_height > 2u);
  static const float filter[BSPLINE_FSIZE] = { 1.0f / 16.0f,
                                               4.0f / 16.0f,
                                               6.0f / 16.0f,
                                               4.0f / 16.0f,
                                               1.0f / 16.0f };
  (void)tempbuf;
  (void)padded_size;

#ifdef _OPENMP
#pragma omp parallel for default(firstprivate) \
  schedule(static)
#endif
  for(size_t row = 0; row < coarse_height; ++row)
  {
    for(size_t col = 0; col < coarse_width; ++col)
    {
      dt_aligned_pixel_t accum = { 0.f };
      const size_t sample_row = use_replicated_boundary ? CLAMP((int)row, 1, (int)coarse_height - 2) : row;
      const size_t sample_col = use_replicated_boundary ? CLAMP((int)col, 1, (int)coarse_width - 2) : col;
      const size_t center_row = sample_row * 2u;
      const size_t center_col = sample_col * 2u;

      // Evaluate the decimated 5x5 cardinal B-spline on the current grid. This
      // is the Gaussian-pyramid reduce stage used to build the hybrid decimated
      // wavelet stack.
      for(int jj = -2; jj <= 2; ++jj)
      {
        const size_t yy = CLAMP((int)center_row + jj, 0, (int)height - 1);
        for(int ii = -2; ii <= 2; ++ii)
        {
          const size_t xx = CLAMP((int)center_col + ii, 0, (int)width - 1);
          const float weight = filter[ii + 2] * filter[jj + 2];
          const size_t index = 4 * (yy * width + xx);
          for_four_channels(c)
            accum[c] += weight * in[index + c];
        }
      }

      const size_t out_index = 4 * (row * coarse_width + col);
      if(clip_negatives)
      {
        for_four_channels(c)
          out[out_index + c] = MAX(accum[c], 0.f);
      }
      else
      {
        copy_pixel_nontemporal(out + out_index, accum);
      }
    }
  }
}
inline static void expand_2D_Bspline(const float *const restrict in, float *const restrict out,
                                     const size_t width, const size_t height,
                                     const gboolean clip_negatives)
{
  const size_t coarse_width = decimated_bspline_size(width);
  const size_t coarse_height = decimated_bspline_size(height);
  const gboolean use_replicated_boundary = (width > 2u && height > 2u && coarse_width > 1u && coarse_height > 1u);
  static const float filter[BSPLINE_FSIZE] = { 1.0f / 16.0f,
                                               4.0f / 16.0f,
                                               6.0f / 16.0f,
                                               4.0f / 16.0f,
                                               1.0f / 16.0f };

#ifdef _OPENMP
#pragma omp parallel for default(firstprivate) \
  schedule(static) \
  collapse(2)
#endif
  for(size_t row = 0; row < height; ++row)
    for(size_t col = 0; col < width; ++col)
    {
      size_t sample_row = row;
      size_t sample_col = col;
      if(use_replicated_boundary)
      {
        const size_t max_row = (height & 1u) ? height - 2u : height - 3u;
        const size_t max_col = (width & 1u) ? width - 2u : width - 3u;
        sample_row = CLAMP((int)row, 1, (int)max_row);
        sample_col = CLAMP((int)col, 1, (int)max_col);
      }

      const size_t center_row = sample_row >> 1;
      const size_t center_col = sample_col >> 1;
      dt_aligned_pixel_t accum = { 0.f };

      // Rebuild the fine sample with the same parity-dependent Gaussian expand
      // used by the local-laplacian pyramid, but on 4-channel spline data.
      switch((sample_col & 1u) + 2u * (sample_row & 1u))
      {
        case 0:
        {
          for(int jj = -1; jj <= 1; ++jj)
            for(int ii = -1; ii <= 1; ++ii)
            {
              const size_t yy = center_row + jj;
              const size_t xx = center_col + ii;
              const float weight = 4.f * filter[2 * (jj + 1)] * filter[2 * (ii + 1)];
              const size_t index = 4 * (yy * coarse_width + xx);
              for_four_channels(c)
                accum[c] += weight * in[index + c];
            }
          break;
        }
        case 1:
        {
          for(int jj = -1; jj <= 1; ++jj)
            for(int ii = 0; ii <= 1; ++ii)
            {
              const size_t yy = center_row + jj;
              const size_t xx = center_col + ii;
              const float weight = 4.f * filter[2 * (jj + 1)] * filter[2 * ii + 1];
              const size_t index = 4 * (yy * coarse_width + xx);
              for_four_channels(c)
                accum[c] += weight * in[index + c];
            }
          break;
        }
        case 2:
        {
          for(int jj = 0; jj <= 1; ++jj)
            for(int ii = -1; ii <= 1; ++ii)
            {
              const size_t yy = center_row + jj;
              const size_t xx = center_col + ii;
              const float weight = 4.f * filter[2 * jj + 1] * filter[2 * (ii + 1)];
              const size_t index = 4 * (yy * coarse_width + xx);
              for_four_channels(c)
                accum[c] += weight * in[index + c];
            }
          break;
        }
        default:
        {
          for(int jj = 0; jj <= 1; ++jj)
            for(int ii = 0; ii <= 1; ++ii)
            {
              const size_t yy = center_row + jj;
              const size_t xx = center_col + ii;
              const float weight = 4.f * filter[2 * jj + 1] * filter[2 * ii + 1];
              const size_t index = 4 * (yy * coarse_width + xx);
              for_four_channels(c)
                accum[c] += weight * in[index + c];
            }
          break;
        }
      }

      const size_t out_index = 4 * (row * width + col);
      if(clip_negatives)
      {
        for_four_channels(c)
          out[out_index + c] = MAX(accum[c], 0.f);
      }
      else
      {
        copy_pixel_nontemporal(out + out_index, accum);
      }
    }
}
inline static void blur_2D_Bspline(const float *const restrict in, float *const restrict out,
                                   float *const restrict tempbuf,
                                   const size_t width, const size_t height, const int mult, const gboolean clip_negatives)
{
  // À-trous B-spline interpolation/blur shifted by mult
  #ifdef _OPENMP
  #pragma omp parallel for default(firstprivate) \
    schedule(static)
  #endif
  for(size_t row = 0; row < height; row++)
  {
    // get a thread-private one-row temporary buffer
    float *const temp = tempbuf + 4 * width * dt_get_thread_num();
    // interleave the order in which we process the rows so that we minimize cache misses
    const size_t i = dwt_interleave_rows(row, height, mult);
    // Convolve B-spline filter over columns: for each pixel in the current row, compute vertical blur
    _bspline_vertical_pass(in, temp, i, width, height, mult, clip_negatives);
    // Convolve B-spline filter horizontally over current row
    for(size_t j = 0; j < width; j++)
    {
      _bspline_horizontal(temp, out + (i * width + j) * 4, j, width, mult, clip_negatives);
    }
  }
}
inline static void decompose_2D_Bspline(const float *const restrict in,
                                        float *const restrict HF,
                                        float *const restrict LF,
                                        const size_t width, const size_t height, const int mult,
                                        float *const tempbuf, size_t padded_size)
{
  // Blur and compute the wavelet at once
#ifdef _OPENMP
#pragma omp parallel for default(firstprivate)  \
    schedule(static)
#endif
  for(size_t row = 0; row < height; row++)
  {
    // get a thread-private one-row temporary buffer
    float *restrict DT_ALIGNED_ARRAY const temp = dt_get_perthread(tempbuf, padded_size);
    // interleave the order in which we process the rows so that we minimize cache misses
    const size_t i = dwt_interleave_rows(row, height, mult);
    // Convolve B-spline filter over columns: for each pixel in the current row, compute vertical blur
    _bspline_vertical_pass(in, temp, i, width, height, mult, TRUE); // always clip negatives
    // Convolve B-spline filter horizontally over current row
    for(size_t j = 0; j < width; j++)
    {
      const size_t index = 4U * (i * width + j);
      _bspline_horizontal(temp, LF + index, j, width, mult, TRUE); // always clip negatives
      // compute the HF component by subtracting the LF from the original input
      for_four_channels(c)
        HF[index + c] = in[index + c] - LF[index + c];
    }
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
