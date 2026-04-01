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

kernel void blur_2D_Bspline_vertical_local(read_only image2d_t in, write_only image2d_t out,
                                           const int width, const int height, const int mult,
                                           const int clip_negative, local float4 *buffer)
{
  // À-trous B-spline interpolation/blur shifted by mult.
  // Reuse the 5-tap vertical support inside local memory so neighbouring
  // work-items do not refetch the same pixels from device memory.
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int lid = get_local_id(1);
  const int blocksize = get_local_size(1);
  const int base = get_group_id(1) * blocksize;
  const int span = blocksize + 4 * mult;
  const int xx = clamp(x, 0, width - 1);
  const bool active = (x < width && y < height);

  for(int k = lid; k < span; k += blocksize)
  {
    const int yy = clamp(base + k - 2 * mult, 0, height - 1);
    buffer[k] = read_imagef(in, samplerA, (int2)(xx, yy));
  }

  barrier(CLK_LOCAL_MEM_FENCE);

  const int center = lid + 2 * mult;
  const float4 accumulator = buffer[center] * (6.0f / 16.0f)
                           + (buffer[center - mult] + buffer[center + mult]) * (4.0f / 16.0f)
                           + (buffer[center - 2 * mult] + buffer[center + 2 * mult]) * (1.0f / 16.0f);

  if(!active)
    return;
  else if(clip_negative)
    write_imagef(out, (int2)(x, y), fmax(accumulator, 0.0f));
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

kernel void blur_2D_Bspline_horizontal_local(read_only image2d_t in, write_only image2d_t out,
                                             const int width, const int height, const int mult,
                                             const int clip_negative, local float4 *buffer)
{
  // À-trous B-spline interpolation/blur shifted by mult.
  // Reuse the 5-tap horizontal support inside local memory so neighbouring
  // work-items do not refetch the same pixels from device memory.
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int lid = get_local_id(0);
  const int blocksize = get_local_size(0);
  const int base = get_group_id(0) * blocksize;
  const int span = blocksize + 4 * mult;
  const int yy = clamp(y, 0, height - 1);
  const bool active = (x < width && y < height);

  for(int k = lid; k < span; k += blocksize)
  {
    const int xx = clamp(base + k - 2 * mult, 0, width - 1);
    buffer[k] = read_imagef(in, samplerA, (int2)(xx, yy));
  }

  barrier(CLK_LOCAL_MEM_FENCE);

  const int center = lid + 2 * mult;
  const float4 accumulator = buffer[center] * (6.0f / 16.0f)
                           + (buffer[center - mult] + buffer[center + mult]) * (4.0f / 16.0f)
                           + (buffer[center - 2 * mult] + buffer[center + 2 * mult]) * (1.0f / 16.0f);

  if(!active)
    return;
  else if(clip_negative)
    write_imagef(out, (int2)(x, y), fmax(accumulator, 0.0f));
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

kernel void guided_laplacian_coefficients(read_only image2d_t HF,
                                          write_only image2d_t coeff,
                                          write_only image2d_t bias,
                                          const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const float inv_patch = 1.0f / 25.0f;
  const float inv_guide = 1.0f / 3.0f;
  const float eps = 1e-12f;
  float sum_r = 0.0f;
  float sum_g = 0.0f;
  float sum_b = 0.0f;
  float sum_r_guide = 0.0f;
  float sum_g_guide = 0.0f;
  float sum_b_guide = 0.0f;
  float sum_guide = 0.0f;
  float sum_guide_sq = 0.0f;

  for(int jj = -2; jj <= 2; ++jj)
  {
    const int yy = clamp(y + jj, 0, height - 1);
    for(int ii = -2; ii <= 2; ++ii)
    {
      const int xx = clamp(x + ii, 0, width - 1);
      const float4 sample = read_imagef(HF, samplerA, (int2)(xx, yy));
      const float guide = (sample.x + sample.y + sample.z) * inv_guide;

      sum_r += sample.x;
      sum_g += sample.y;
      sum_b += sample.z;
      sum_guide += guide;
      sum_guide_sq += guide * guide;
      sum_r_guide += sample.x * guide;
      sum_g_guide += sample.y * guide;
      sum_b_guide += sample.z * guide;
    }
  }

  const float mean_r = sum_r * inv_patch;
  const float mean_g = sum_g * inv_patch;
  const float mean_b = sum_b * inv_patch;
  const float guide_mean = sum_guide * inv_patch;
  const float variance = fmax(sum_guide_sq * inv_patch - guide_mean * guide_mean, 0.0f);
  const float covariance_r = sum_r_guide * inv_patch - mean_r * guide_mean;
  const float covariance_g = sum_g_guide * inv_patch - mean_g * guide_mean;
  const float covariance_b = sum_b_guide * inv_patch - mean_b * guide_mean;

  float a_r = 0.0f;
  float a_g = 0.0f;
  float a_b = 0.0f;

  if(variance > eps)
  {
    a_r = covariance_r / variance;
    a_g = covariance_g / variance;
    a_b = covariance_b / variance;
  }

  const float b_r = mean_r - a_r * guide_mean;
  const float b_g = mean_g - a_g * guide_mean;
  const float b_b = mean_b - a_b * guide_mean;

  write_imagef(coeff, (int2)(x, y), (float4)(a_r, a_g, a_b, 0.0f));
  write_imagef(bias, (int2)(x, y), (float4)(b_r, b_g, b_b, 0.0f));
}

kernel void guided_laplacian_normalize(read_only image2d_t detail,
                                       read_only image2d_t LF,
                                       write_only image2d_t normalized_HF,
                                       const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const float4 lf = read_imagef(LF, samplerA, (int2)(x, y));
  const float4 hf = read_imagef(detail, samplerA, (int2)(x, y)) - lf;
  const float4 normalized = (float4)(hf.x / fmax(lf.x, 1e-8f),
                                     hf.y / fmax(lf.y, 1e-8f),
                                     hf.z / fmax(lf.z, 1e-8f),
                                     0.0f);
  write_imagef(normalized_HF, (int2)(x, y), normalized);
}

kernel void guided_laplacian_apply(read_only image2d_t HF,
                                   read_only image2d_t coeff,
                                   read_only image2d_t bias,
                                   read_only image2d_t LF,
                                   read_only image2d_t reconstructed_read,
                                   write_only image2d_t reconstructed_write,
                                   const int width, const int height, const int first_scale)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const float inv_guide = 1.0f / 3.0f;
  const float3 hf = read_imagef(HF, samplerA, (int2)(x, y)).xyz;
  const float3 a = read_imagef(coeff, samplerA, (int2)(x, y)).xyz;
  const float3 b = read_imagef(bias, samplerA, (int2)(x, y)).xyz;
  const float guide = (hf.x + hf.y + hf.z) * inv_guide;
  const float3 lf = read_imagef(LF, samplerA, (int2)(x, y)).xyz;
  const float3 filtered = (a * guide + b) * lf;

  if(first_scale)
  {
    write_imagef(reconstructed_write, (int2)(x, y), (float4)(filtered.x, filtered.y, filtered.z, 0.0f));
  }
  else
  {
    const float3 accumulated = read_imagef(reconstructed_read, samplerA, (int2)(x, y)).xyz;
    const float3 reconstructed = accumulated + filtered;
    write_imagef(reconstructed_write, (int2)(x, y),
                 (float4)(reconstructed.x, reconstructed.y, reconstructed.z, 0.0f));
  }
}

kernel void guided_laplacian_finalize(read_only image2d_t reconstructed,
                                      read_only image2d_t residual,
                                      write_only image2d_t out,
                                      const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(reconstructed, samplerA, (int2)(x, y))
               + read_imagef(residual, samplerA, (int2)(x, y));
  pixel = fmax(pixel, 0.0f);
  pixel.w = 0.0f;
  write_imagef(out, (int2)(x, y), pixel);
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
