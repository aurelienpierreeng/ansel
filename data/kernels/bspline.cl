/*
    This file is part of the Ansel project.
    Copyright (C) 2022, 2025 Aurélien PIERRE.
    
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


#include "common.h"

// B spline filter
#define FSIZE 5
#define FSTART (FSIZE - 1) / 2


kernel void blur_2D_Bspline_vertical(read_only image2d_t in, write_only image2d_t out,
                                     const int width, const int height, const int mult,
                                     const int clip_negative)
{
  // À-trous B-spline interpolation/blur shifted by mult
  // Convolve B-spline filter over lines
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const float4 filter[FSIZE] = { (float4)1.0f / 16.0f,
                                 (float4)4.0f / 16.0f,
                                 (float4)6.0f / 16.0f,
                                 (float4)4.0f / 16.0f,
                                 (float4)1.0f / 16.0f };

  float4 accumulator = (float4)0.f;

  #pragma unroll
  for(int jj = 0; jj < FSIZE; ++jj)
  {
    const int yy = mult * (jj - FSTART) + y;
    accumulator += filter[jj] * read_imagef(in, samplerA, (int2)(x, clamp(yy, 0, height - 1)));
  }

  if(clip_negative)
    write_imagef(out, (int2)(x, y), fmax(accumulator, 0.f));
  else
    write_imagef(out, (int2)(x, y), accumulator);
}


kernel void blur_2D_Bspline_horizontal(read_only image2d_t in, write_only image2d_t out,
                                       const int width, const int height, const int mult,
                                       const int clip_negative)
{
  // À-trous B-spline interpolation/blur shifted by mult
  // Convolve B-spline filter over columns
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const float4 filter[FSIZE] = { (float4)1.0f / 16.0f,
                                 (float4)4.0f / 16.0f,
                                 (float4)6.0f / 16.0f,
                                 (float4)4.0f / 16.0f,
                                 (float4)1.0f / 16.0f };

  float4 accumulator = (float4)0.f;

  #pragma unroll
  for(int ii = 0; ii < FSIZE; ++ii)
  {
    const int xx = mult * (ii - FSTART) + x;
    accumulator += filter[ii] * read_imagef(in, samplerA, (int2)(clamp(xx, 0, width - 1), y));
  }

  if(clip_negative)
    write_imagef(out, (int2)(x, y), fmax(accumulator, 0.f));
  else
    write_imagef(out, (int2)(x, y), accumulator);
}


kernel void wavelets_detail_level(read_only image2d_t detail, read_only image2d_t LF,
                                  write_only image2d_t HF, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const float4 d = read_imagef(detail, samplerA, (int2)(x, y));
  const float4 lf = read_imagef(LF, samplerA, (int2)(x, y));

  write_imagef(HF, (int2)(x, y), d - lf);
}

kernel void reduce_2D_mask(read_only image2d_t in, write_only image2d_t out,
                           const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int coarse_width = (width - 1) / 2 + 1;
  const int coarse_height = (height - 1) / 2 + 1;

  if(x >= coarse_width || y >= coarse_height) return;

  uint masked = 0u;
  const int center_x = x * 2;
  const int center_y = y * 2;

  for(int jj = -1; jj <= 1 && !masked; ++jj)
  {
    const int yy = clamp(center_y + jj, 0, height - 1);
    for(int ii = -1; ii <= 1; ++ii)
    {
      const int xx = clamp(center_x + ii, 0, width - 1);
      masked |= read_imageui(in, sampleri, (int2)(xx, yy)).x;
    }
  }

  write_imageui(out, (int2)(x, y), (uint4)(masked, 0u, 0u, 0u));
}

kernel void reduce_2D_Bspline(read_only image2d_t in, write_only image2d_t out,
                              const int width, const int height, const int clip_negative)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int coarse_width = (width - 1) / 2 + 1;
  const int coarse_height = (height - 1) / 2 + 1;

  if(x >= coarse_width || y >= coarse_height) return;

  const float4 filter[FSIZE] = { (float4)1.0f / 16.0f,
                                 (float4)4.0f / 16.0f,
                                 (float4)6.0f / 16.0f,
                                 (float4)4.0f / 16.0f,
                                 (float4)1.0f / 16.0f };

  float4 accumulator = 0.f;
  const int sample_x = (coarse_width > 2 && coarse_height > 2) ? clamp(x, 1, coarse_width - 2) : x;
  const int sample_y = (coarse_width > 2 && coarse_height > 2) ? clamp(y, 1, coarse_height - 2) : y;
  const int center_x = sample_x * 2;
  const int center_y = sample_y * 2;

  // Evaluate the decimated 5x5 cardinal B-spline on the current grid. This is
  // the Gaussian-pyramid reduce stage used to build the hybrid decimated
  // wavelet stack.
  for(int jj = 0; jj < FSIZE; ++jj)
  {
    const int yy = clamp(center_y + (jj - FSTART), 0, height - 1);
    #pragma unroll
    for(int ii = 0; ii < FSIZE; ++ii)
    {
      const int xx = clamp(center_x + (ii - FSTART), 0, width - 1);
      accumulator += filter[jj] * filter[ii] * read_imagef(in, samplerA, (int2)(xx, yy));
    }
  }

  write_imagef(out, (int2)(x, y), clip_negative ? fmax(accumulator, 0.f) : accumulator);
}

kernel void expand_2D_Bspline(read_only image2d_t in, write_only image2d_t out,
                              const int width, const int height, const int clip_negative)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int coarse_width = (width - 1) / 2 + 1;
  const int coarse_height = (height - 1) / 2 + 1;

  if(x >= width || y >= height) return;

  int sample_x = x;
  int sample_y = y;
  if(width > 2 && height > 2 && coarse_width > 1 && coarse_height > 1)
  {
    sample_x = clamp(x, 1, (width & 1) ? width - 2 : width - 3);
    sample_y = clamp(y, 1, (height & 1) ? height - 2 : height - 3);
  }

  const int center_x = sample_x >> 1;
  const int center_y = sample_y >> 1;
  const float4 filter[FSIZE] = { (float4)1.0f / 16.0f,
                                 (float4)4.0f / 16.0f,
                                 (float4)6.0f / 16.0f,
                                 (float4)4.0f / 16.0f,
                                 (float4)1.0f / 16.0f };
  float4 accumulator = 0.f;

  // Rebuild the fine sample with the same parity-dependent Gaussian expand
  // used by the local-laplacian pyramid, but on 4-channel spline data.
  switch((sample_x & 1) + 2 * (sample_y & 1))
  {
    case 0:
    {
      for(int jj = -1; jj <= 1; ++jj)
        for(int ii = -1; ii <= 1; ++ii)
        {
          const int yy = center_y + jj;
          const int xx = center_x + ii;
          accumulator += 4.f * filter[2 * (jj + 1)] * filter[2 * (ii + 1)]
                       * read_imagef(in, samplerA, (int2)(xx, yy));
        }
      break;
    }
    case 1:
    {
      for(int jj = -1; jj <= 1; ++jj)
        for(int ii = 0; ii <= 1; ++ii)
        {
          const int yy = center_y + jj;
          const int xx = center_x + ii;
          accumulator += 4.f * filter[2 * (jj + 1)] * filter[2 * ii + 1]
                       * read_imagef(in, samplerA, (int2)(xx, yy));
        }
      break;
    }
    case 2:
    {
      for(int jj = 0; jj <= 1; ++jj)
        for(int ii = -1; ii <= 1; ++ii)
        {
          const int yy = center_y + jj;
          const int xx = center_x + ii;
          accumulator += 4.f * filter[2 * jj + 1] * filter[2 * (ii + 1)]
                       * read_imagef(in, samplerA, (int2)(xx, yy));
        }
      break;
    }
    default:
    {
      for(int jj = 0; jj <= 1; ++jj)
        for(int ii = 0; ii <= 1; ++ii)
        {
          const int yy = center_y + jj;
          const int xx = center_x + ii;
          accumulator += 4.f * filter[2 * jj + 1] * filter[2 * ii + 1]
                       * read_imagef(in, samplerA, (int2)(xx, yy));
        }
      break;
    }
  }

  write_imagef(out, (int2)(x, y), clip_negative ? fmax(accumulator, 0.f) : accumulator);
}
