/*
    This file is part of the Ansel project.
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

#include "common.h"
#include "color_conversion.h"
#include "colorspace.h"

#define CRYSTGRAIN_LAYER_KERNELS 16

static inline int
reflect_index(const int i, const int max)
{
  if(max <= 1) return 0;

  int value = i;
  while(value < 0 || value >= max)
  {
    if(value < 0)
      value = -value - 1;
    else
      value = 2 * max - value - 1;
  }

  return value;
}

static inline uint
splitmix32_u64(const ulong seed)
{
  ulong result = (seed ^ (seed >> 33)) * 0x62a9d9ed799705f5UL;
  result = (result ^ (result >> 28)) * 0xcb24d0a5c88c35b3UL;
  return (uint)(result >> 32);
}

static inline float
uniform_random(const ulong seed)
{
  return (float)splitmix32_u64(seed) * 0x1.0p-32f;
}

static inline float
gaussian_random(const ulong seed_a, const ulong seed_b)
{
  const float u1 = fmax(uniform_random(seed_a), FLT_MIN);
  const float u2 = uniform_random(seed_b);
  return native_sqrt(-2.0f * native_log(u1)) * native_cos(2.0f * M_PI_F * u2);
}

static inline float
seed_probability(const float filling, const float crystal_area)
{
  const float clamped_filling = clamp(filling, 0.0f, 0.9999f);
  if(crystal_area <= 1.0f) return clamped_filling;
  return 1.0f - pow(1.0f - clamped_filling, 1.0f / crystal_area);
}

void
atomic_add_f(
    global float *val,
    const  float  delta)
{
#ifdef NVIDIA_SM_20
  float res = 0.0f;
  asm volatile ("atom.global.add.f32 %0, [%1], %2;" : "=f"(res) : "l"(val), "f"(delta));
#else
  union
  {
    float f;
    unsigned int i;
  }
  old_val;
  union
  {
    float f;
    unsigned int i;
  }
  new_val;

  global volatile unsigned int *ival = (global volatile unsigned int *)val;

  do
  {
    old_val.i = atomic_add(ival, 0);
    new_val.f = old_val.f + delta;
  }
  while(atomic_cmpxchg(ival, old_val.i, new_val.i) != old_val.i);
#endif
}

void
atomic_sub_clamp0_f(
    global float *val,
    const  float  delta)
{
  union
  {
    float f;
    unsigned int i;
  }
  old_val;
  union
  {
    float f;
    unsigned int i;
  }
  new_val;

  global volatile unsigned int *ival = (global volatile unsigned int *)val;

  do
  {
    old_val.i = atomic_add(ival, 0);
    new_val.f = fmax(old_val.f - delta, 0.0f);
  }
  while(atomic_cmpxchg(ival, old_val.i, new_val.i) != old_val.i);
}

kernel void
crystgrain_zero_scalar(global float *buffer, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;
  buffer[mad24(y, width, x)] = 0.0f;
}

kernel void
crystgrain_zero_rgb(global float *buffer, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const int index = 4 * mad24(y, width, x);
  buffer[index + 0] = 0.0f;
  buffer[index + 1] = 0.0f;
  buffer[index + 2] = 0.0f;
  buffer[index + 3] = 0.0f;
}

static inline float
crystgrain_kernel_coverage(const int dx, const int dy, const float radius_f,
                           const float vertices, const float rotation)
{
  if(vertices >= 5.0f)
  {
    const float signed_distance = radius_f - hypot((float)dx, (float)dy);
    return clamp(signed_distance + 0.5f, 0.0f, 1.0f);
  }

  const float r = hypot((float)dx, (float)dy);
  const float theta = atan2((float)dy, (float)dx);
  const float envelope = native_cos(M_PI_F / vertices)
                         / native_cos((2.0f * asin(cos(vertices * (theta + rotation))) + M_PI_F)
                                      / (2.0f * vertices));
  const float signed_distance = radius_f * envelope - r;
  return clamp(signed_distance + 0.5f, 0.0f, 1.0f);
}

kernel void
crystgrain_extract_luminance(read_only image2d_t in, global float *image, const int width, const int height,
                             constant const dt_colorspaces_iccprofile_info_cl_t *const profile_info,
                             read_only image2d_t profile_lut, const int use_work_profile)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  const float luminance = use_work_profile
    ? get_rgb_matrix_luminance(pixel, profile_info, profile_info->matrix_in, profile_lut)
    : dt_camera_rgb_luminance(pixel);

  image[mad24(y, width, x)] = fmax(luminance, 0.0f);
}

kernel void
crystgrain_extract_rgb(read_only image2d_t in, global float *rgb, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const int index = 4 * mad24(y, width, x);
  const float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  rgb[index + 0] = fmax(pixel.x, 0.0f);
  rgb[index + 1] = fmax(pixel.y, 0.0f);
  rgb[index + 2] = fmax(pixel.z, 0.0f);
  rgb[index + 3] = 0.0f;
}

kernel void
crystgrain_simulate_layer(global const float *image, global float *remaining, global float *result,
                          global const float4 *kernel_bank, const int width, const int height,
                          const int roi_x, const int roi_y, const float inv_scale,
                          const ulong base_seed, const int layer_index, const float layer_scale)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const int index = mad24(y, width, x);
  if(remaining[index] <= 0.0f) return;

  const int world_x = (int)((roi_x + x) * inv_scale);
  const int world_y = (int)((roi_y + y) * inv_scale);
  const ulong pixel_seed = base_seed
                           ^ ((ulong)((uint)world_x) << 32)
                           ^ (ulong)((uint)world_y)
                           ^ ((ulong)(layer_index + 1) * 0x9e3779b97f4a7c15UL);
  const uint kernel_index = splitmix32_u64(pixel_seed ^ 0x94d049bb133111ebUL) & (CRYSTGRAIN_LAYER_KERNELS - 1U);
  const float4 params = kernel_bank[kernel_index];
  const float vertices = params.x;
  const float rotation = params.y;
  const float probability = params.z;
  const float radius_f = fmax(params.w, 0.5f);
  const int radius = max((int)ceil(radius_f + 0.5f), 1);
  const int interior = (x >= radius && x < width - radius && y >= radius && y < height - radius);

  // Each work-item sweeps only its own neighbourhood. If the random draw says
  // this pixel is not a seed, it exits immediately without touching the light
  // field or the reconstruction.
  if(uniform_random(pixel_seed ^ 0xda942042e4dd58b5UL) >= probability) return;

  float sum_remaining = 0.0f;
  float sum_original = 0.0f;
  float total_weight = 0.0f;

  // Print a flat tone inside the crystal by averaging both the current light
  // field and the immutable input signal over the grain support.
  for(int dy = -radius; dy <= radius; dy++)
  {
    for(int dx = -radius; dx <= radius; dx++)
    {
      const float coverage = crystgrain_kernel_coverage(dx, dy, radius_f, vertices, rotation);
      if(coverage <= FLT_EPSILON) continue;

      int xx, yy, dst;
      if(interior)
      {
        xx = x + dx;
        yy = y + dy;
        dst = mad24(yy, width, xx);
      }
      else
      {
        xx = reflect_index(x + dx, width);
        yy = reflect_index(y + dy, height);
        dst = mad24(yy, width, xx);
      }

      sum_remaining += fmax(remaining[dst], 0.0f) * coverage;
      sum_original += image[dst] * coverage;
      total_weight += coverage;
    }
  }

  if(total_weight <= FLT_EPSILON) return;

  float seed_energy = sum_remaining / total_weight;
  // The user layer scale applies to the whole grain surface, so the flat
  // crystal tone cap scales with the grain area instead of staying normalized
  // per covered pixel.
  const float original_energy = sum_original * layer_scale;
  seed_energy = fmin(seed_energy, original_energy);
  if(seed_energy <= 0.0f) return;

  // Atomically add the printed crystal to the output and subtract the same
  // amount from the remaining light so concurrent seeds still update the same
  // buffers in place within this single layer dispatch.
  for(int dy = -radius; dy <= radius; dy++)
  {
    for(int dx = -radius; dx <= radius; dx++)
    {
      const float coverage = crystgrain_kernel_coverage(dx, dy, radius_f, vertices, rotation);
      if(coverage <= FLT_EPSILON) continue;

      int xx, yy, dst;
      if(interior)
      {
        xx = x + dx;
        yy = y + dy;
        dst = mad24(yy, width, xx);
      }
      else
      {
        xx = reflect_index(x + dx, width);
        yy = reflect_index(y + dy, height);
        dst = mad24(yy, width, xx);
      }

      const float deposited = seed_energy * coverage;
      atomic_add_f(result + dst, deposited);
      atomic_sub_clamp0_f(remaining + dst, deposited);
    }
  }
}

kernel void
crystgrain_simulate_layer_color(global const float *image_rgb, global float *remaining_rgb, global float *result_rgb,
                                const int width, const int height, const int roi_x, const int roi_y,
                                const float inv_scale, const ulong base_seed, global const float4 *kernel_bank,
                                const float layer_scale, const int sublayer, const int active_channel,
                                const float channel_correlation)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const ulong channel_salt[3] = {
    0xa24baed4963ee407UL,
    0x9fb21c651e98df25UL,
    0xc13fa9a902a6328fUL
  };

  if(x >= width || y >= height) return;

  const int index = mad24(y, width, x);
  const int rgb_index = 4 * index;
  const float remaining_total = remaining_rgb[rgb_index + 0]
                                + remaining_rgb[rgb_index + 1]
                                + remaining_rgb[rgb_index + 2];
  if(remaining_total <= 0.0f) return;

  const int world_x = (int)((roi_x + x) * inv_scale);
  const int world_y = (int)((roi_y + y) * inv_scale);
  const ulong shared_seed = base_seed
                            ^ ((ulong)((uint)world_x) << 32)
                            ^ (ulong)((uint)world_y)
                            ^ ((ulong)(sublayer + 1) * 0x9e3779b97f4a7c15UL);
  const ulong channel_seed = shared_seed ^ channel_salt[active_channel];
  const int use_shared = uniform_random(channel_seed ^ 0x4f1bbcdc6762f96bUL) < channel_correlation;
  const ulong pixel_seed = use_shared ? shared_seed : channel_seed;
  const uint kernel_index = splitmix32_u64(pixel_seed ^ 0x94d049bb133111ebUL) & (CRYSTGRAIN_LAYER_KERNELS - 1U);
  const float4 params = kernel_bank[kernel_index];
  const float vertices = params.x;
  const float rotation = params.y;
  const float probability = params.z;
  const float radius_f = fmax(params.w, 0.5f);
  const int radius = max((int)ceil(radius_f + 0.5f), 1);
  const int interior = (x >= radius && x < width - radius && y >= radius && y < height - radius);

  if(uniform_random(pixel_seed ^ 0xda942042e4dd58b5UL) >= probability) return;

  float seed_energy = 0.0f;
  float original_energy = 0.0f;
  float total_weight = 0.0f;

  // One spectral sub-stack is active for this layer, so its crystals only
  // capture one channel while deeper sub-stacks keep the remaining light for
  // later.
  for(int dy = -radius; dy <= radius; dy++)
  {
    for(int dx = -radius; dx <= radius; dx++)
    {
      const float coverage = crystgrain_kernel_coverage(dx, dy, radius_f, vertices, rotation);
      if(coverage <= FLT_EPSILON) continue;

      int xx, yy, dst;
      if(interior)
      {
        xx = x + dx;
        yy = y + dy;
        dst = mad24(yy, width, xx);
      }
      else
      {
        xx = reflect_index(x + dx, width);
        yy = reflect_index(y + dy, height);
        dst = mad24(yy, width, xx);
      }

      const int dst_rgb = 4 * dst + active_channel;
      seed_energy += remaining_rgb[dst_rgb] * coverage;
      original_energy += image_rgb[dst_rgb] * coverage;
      total_weight += coverage;
    }
  }

  if(total_weight <= FLT_EPSILON) return;

  seed_energy /= total_weight;
  original_energy *= layer_scale;
  const float captured = fmin(seed_energy, original_energy);
  if(captured <= 0.0f) return;

  for(int dy = -radius; dy <= radius; dy++)
  {
    for(int dx = -radius; dx <= radius; dx++)
    {
      const float coverage = crystgrain_kernel_coverage(dx, dy, radius_f, vertices, rotation);
      if(coverage <= FLT_EPSILON) continue;

      int xx, yy, dst;
      if(interior)
      {
        xx = x + dx;
        yy = y + dy;
        dst = mad24(yy, width, xx);
      }
      else
      {
        xx = reflect_index(x + dx, width);
        yy = reflect_index(y + dy, height);
        dst = mad24(yy, width, xx);
      }

      const float deposited = captured * coverage;
      const int dst_rgb = 4 * dst + active_channel;
      atomic_add_f(result_rgb + dst_rgb, deposited);
      atomic_sub_clamp0_f(remaining_rgb + dst_rgb, deposited);
    }
  }
}

kernel void
crystgrain_reduce_first_scalar(global const float *input, const int width, const int height,
                               global float *accu, local float *buffer)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int xlsz = get_local_size(0);
  const int ylsz = get_local_size(1);
  const int xlid = get_local_id(0);
  const int ylid = get_local_id(1);
  const int lid = mad24(ylid, xlsz, xlid);

  const int in_bounds = (x < width && y < height);
  buffer[lid] = in_bounds ? input[mad24(y, width, x)] : 0.0f;

  barrier(CLK_LOCAL_MEM_FENCE);

  const int lsz = mul24(xlsz, ylsz);
  for(int offset = lsz / 2; offset > 0; offset = offset / 2)
  {
    if(lid < offset) buffer[lid] += buffer[lid + offset];
    barrier(CLK_LOCAL_MEM_FENCE);
  }

  if(lid == 0)
  {
    const int group_index = mad24((int)get_group_id(1), (int)get_num_groups(0), (int)get_group_id(0));
    accu[group_index] = buffer[0];
  }
}

kernel void
crystgrain_reduce_second_scalar(global const float *input, global float *result, const int length, local float *buffer)
{
  int x = get_global_id(0);
  float sum = 0.0f;

  while(x < length)
  {
    sum += input[x];
    x += get_global_size(0);
  }

  const int lid = get_local_id(0);
  buffer[lid] = sum;
  barrier(CLK_LOCAL_MEM_FENCE);

  for(int offset = get_local_size(0) / 2; offset > 0; offset = offset / 2)
  {
    if(lid < offset) buffer[lid] += buffer[lid + offset];
    barrier(CLK_LOCAL_MEM_FENCE);
  }

  if(lid == 0) result[get_group_id(0)] = buffer[0];
}

kernel void
crystgrain_apply_mono(read_only image2d_t in, global const float *image, global const float *result,
                      write_only image2d_t out, const int width, const int height, const float exposure)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const int index = mad24(y, width, x);
  const float4 pixel = read_imagef(in, sampleri, (int2)(x, y));

  // The host predicts the stack transmission on a flat field from the sampled
  // grain statistics. This kernel therefore only applies that global exposure
  // factor and rescales RGB by the grainy-to-original luminance ratio.
  const float grainy = fmax(result[index] * exposure, 0.0f);
  const float ratio = (image[index] > 1e-6f) ? grainy / image[index] : 0.0f;
  const float4 out_pixel = (float4)(fmax(pixel.x * ratio, 0.0f),
                                    fmax(pixel.y * ratio, 0.0f),
                                    fmax(pixel.z * ratio, 0.0f),
                                    pixel.w);

  write_imagef(out, (int2)(x, y), out_pixel);
}

kernel void
crystgrain_finalize_color(read_only image2d_t in, global const float *image_rgb, global const float *result_rgb,
                          write_only image2d_t out, const int width, const int height,
                          const float exposure_r, const float exposure_g, const float exposure_b,
                          const float colorfulness)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const int index = 4 * mad24(y, width, x);
  const float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  // As on CPU, the three exposure factors come from the flat-field recurrence
  // driven by the sampled layer banks. The image-dependent part starts only
  // here, when we apply that global normalization and then mute the chromatic
  // amplitude of the RGB grain residual.
  const float image_r = image_rgb[index + 0];
  const float image_g = image_rgb[index + 1];
  const float image_b = image_rgb[index + 2];
  const float grain_r = (exposure_r > 0.0f) ? fmax(result_rgb[index + 0] * exposure_r, 0.0f) : image_r;
  const float grain_g = (exposure_g > 0.0f) ? fmax(result_rgb[index + 1] * exposure_g, 0.0f) : image_g;
  const float grain_b = (exposure_b > 0.0f) ? fmax(result_rgb[index + 2] * exposure_b, 0.0f) : image_b;
  const float residual_r = grain_r - image_r;
  const float residual_g = grain_g - image_g;
  const float residual_b = grain_b - image_b;
  const float mean = (residual_r + residual_g + residual_b) / 3.0f;

  write_imagef(out, (int2)(x, y), (float4)(pixel.x + mean + (residual_r - mean) * colorfulness,
                                           pixel.y + mean + (residual_g - mean) * colorfulness,
                                           pixel.z + mean + (residual_b - mean) * colorfulness,
                                           pixel.w));
}
