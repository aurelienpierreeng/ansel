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

// Per-region gather/composite + the region reconstruction driver (CPU + OpenCL). (implementation; see region.h for
// the public API.)

#include "system/macros.h"
#include "system/mem_alloc.h"
#include "system/simd.h"
#include "system/target_clones.h"
#include "common/pixelpipe_cache_alloc.h"
#include "pixel/gaussian.h"
#include "iop/noise_generator.h"
#include "iop/highlights/blur.h"
#include "iop/highlights/chroma.h"
#include "iop/highlights/coefficient_field.h"
#include "iop/highlights/core.h"
#include "iop/highlights/region.h"
#include <math.h>
#include <string.h>

__DT_CLONE_TARGETS__
static void _region_gather(_hl_region_ctx_t *const ctx)
{
  float *const restrict interp = ctx->interp;
  const float *const restrict mask = ctx->mask;
  const float *const restrict depth = ctx->depth;
  const int width = ctx->width;
  const _hl_region_t *const region = ctx->region;
  const int region_w = ctx->region_w;
  const int region_h = ctx->region_h;
  const size_t region_pixels = ctx->region_pixels;
  float *const restrict estimate = ctx->estimate;
  float *const restrict valid = ctx->valid;
  float *const restrict clip_depth = ctx->clip_depth;
  float *const restrict clip0 = ctx->clip0;

  // gather region into contiguous buffers
  HL_PFOR(collapse(2))
  for(int y = 0; y < region_h; y++)
    for(int x = 0; x < region_w; x++)
    {
      const size_t pixel_index = (size_t)(region->ry0 + y) * width + (region->rx0 + x);
      const size_t src_offset = pixel_index * 4;
      const size_t dst_offset = ((size_t)y * region_w + x) * 4;
      clip_depth[(size_t)y * region_w + x] = depth[pixel_index];
      for(int k = 0; k < 4; k++)
      {
        estimate[dst_offset + k] = interp[src_offset + k];
        clip0[dst_offset + k] = interp[src_offset + k]; // saturated value, physical floor for clipped ch.
        valid[dst_offset + k] = fmaxf(1.f - mask[src_offset + k], 0.f); // per-channel validity
      }
    }

  // A clipped channel saturated, so its true value is >= its clip level: floor the reconstruction at
  // the saturated value so a low-guide fit cannot push it below saturation (the amber -> magenta
  // collapse). Monotone (only raises), so no overshoot and no per-pixel switching. Applied before the
  // joint core, so the all-clip dome and chroma diffusion are fed the corrected (brighter) rim.
  HL_PFOR()
  for(size_t i = 0; i < region_pixels; i++)
    for(int c = 0; c < 3; c++)
      if(valid[i * 4 + c] < 0.5f) estimate[i * 4 + c] = fmaxf(estimate[i * 4 + c], clip0[i * 4 + c]);
}

// Stage 9 -- optional Poissonian grain on the reconstructed channels, then scatter the
// padded-window estimate back into the full-res interp buffer at the region's offset.
__DT_CLONE_TARGETS__
static void _region_composite(_hl_region_ctx_t *const ctx)
{
  float *const restrict interp = ctx->interp;
  const float *const restrict mask = ctx->mask;
  const int width = ctx->width;
  const _hl_region_t *const region = ctx->region;
  const int region_w = ctx->region_w;
  const int region_h = ctx->region_h;
  const float noise_level = ctx->noise_level;
  float *const restrict estimate = ctx->estimate;
  float *const restrict valid = ctx->valid;

  // Optional grain: reconstructed highlights are very smooth, so break them up with Poissonian noise
  // whose amplitude scales with the local value (the "noise level" user parameter). Only clipped
  // channels get it; valid channels keep their real data. Matches the legacy last-scale noise.
  if(noise_level > 0.f)
  {
    HL_PFOR(collapse(2))
    for(int y = 0; y < region_h; y++)
    {
      for(int x = 0; x < region_w; x++)
      {
        const size_t i = ((size_t)y * region_w + x) * 4;

        // per-pixel RNG, deterministic in region coordinates so the render is reproducible
        uint32_t DT_ALIGNED_ARRAY state[4]
            = { splitmix32(x + 1), splitmix32((y + 1) * (x + 3)), splitmix32(1337), splitmix32(666) };
        xoshiro128plus(state);
        xoshiro128plus(state);
        xoshiro128plus(state);
        xoshiro128plus(state);

        // per-channel noise standard deviation = value * noise_level
        dt_aligned_pixel_t current = { estimate[i], estimate[i + 1], estimate[i + 2], estimate[i + 3] };
        dt_aligned_pixel_t nsigma = { current[0] * noise_level, current[1] * noise_level, current[2] * noise_level,
                                      current[3] * noise_level };
        const int DT_ALIGNED_ARRAY flip[4] = { TRUE, FALSE, TRUE, FALSE };
        dt_aligned_pixel_t noise = { 0.f };
        dt_noise_generator_simd(DT_NOISE_POISSONIAN, current, nsigma, flip, state, noise);

        // one-sided (brightening) grain, only on the reconstructed (clipped) channels
        for(int c = 0; c < 3; c++)
          if(valid[i + c] < 0.5f) estimate[i + c] = fmaxf(current[c] + fabsf(noise[c] - current[c]), 0.f);
      }
    }
  }

  // FLOW: final per-region composite (article §"The algorithm", the flowchart's remosaic-feeding step).
  // Scatter the reconstructed clipped channels from the padded window back into the full-res interp
  // buffer at the region's absolute offset (region->rx0/ry0). Only the channels that were ACTUALLY
  // clipped (mask > 0.5) are overwritten -- valid channels keep their measured values untouched -- and
  // the write is floored at 0 (no negative radiance). Unclipped pixels outside every region are never
  // visited, so the reconstruction only ever edits the holes.
  HL_PFOR(collapse(2))
  for(int y = 0; y < region_h; y++)
  {
    for(int x = 0; x < region_w; x++)
    {
      const size_t src_offset = ((size_t)y * region_w + x) * 4;
      const size_t dst_offset = ((size_t)(region->ry0 + y) * width + (region->rx0 + x)) * 4;

      // only overwrite the channels that were actually clipped
      for(int c = 0; c < 3; c++)
        if(mask[dst_offset + c] > 0.5f) interp[dst_offset + c] = fmaxf(estimate[src_offset + c], 0.f);
    }
  }
}

void _region_guided_filter(float *const restrict interp, const float *const restrict mask,
                           const float *const restrict depth, const int width, const _hl_region_t *const region,
                           const dt_dev_pixelpipe_t *pipe, const float solid_color, const int max_iter,
                           const float noise_level, const float floor_gate)
{
  const int region_w = region->rx1 - region->rx0 + 1;
  const int region_h = region->ry1 - region->ry0 + 1;
  if(region_w < 2 || region_h < 2) return;
  const size_t region_pixels = (size_t)region_w * region_h;
  // Sanity guard only (the pipe-cache arena handles memory): skip a pathologically huge region.
  // Normal clipped regions in a full raw stay well under this; keep it high so nothing is missed.
  if(region_pixels > (size_t)64 * 1024 * 1024) return;

  float *const restrict estimate
      = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe); // running estimate (RGB+norm)
  float *const restrict prev_scale
      = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe); // snapshot at scale start
  float *const restrict valid
      = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe); // per-channel validity (0..1)
  float *const restrict blur_in
      = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe); // blur scratch (in)
  float *const restrict plane1
      = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe); // per-channel fit accumulator
  float *const restrict plane2
      = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe); // blur scratch (out)
  float *const restrict plane3
      = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe); // blur scratch (out)
  float *const restrict valid_variance
      = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe); // per-channel valid variance
  float *const restrict guide_score
      = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe); // per-channel best guide score
  float *const restrict clip_depth
      = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe); // per-pixel clip-to-valid depth
  float *const restrict clip0
      = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe); // saturated (clipped) value per channel
  if(!estimate || !prev_scale || !valid || !blur_in || !plane1 || !plane2 || !plane3 || !valid_variance
     || !guide_score || !clip_depth || !clip0)
  {
    dt_pixelpipe_cache_free_align(estimate);
    dt_pixelpipe_cache_free_align(prev_scale);
    dt_pixelpipe_cache_free_align(valid);
    dt_pixelpipe_cache_free_align(blur_in);
    dt_pixelpipe_cache_free_align(plane1);
    dt_pixelpipe_cache_free_align(plane2);
    dt_pixelpipe_cache_free_align(plane3);
    dt_pixelpipe_cache_free_align(valid_variance);
    dt_pixelpipe_cache_free_align(guide_score);
    dt_pixelpipe_cache_free_align(clip_depth);
    dt_pixelpipe_cache_free_align(clip0);
    return;
  }

  const int extent = MAX(region->x1 - region->x0, region->y1 - region->y0) + 1;
  const float epsilon = 1e-6f;
  const int max_cg_iter = CLAMP(2 * extent, 200, 2000);
  // The prototype solves the seam regulariser with a direct sparse solve (exact). C has no sparse
  // direct solver, so run the FULL CG budget instead of capping at the user "iterations" param:
  // an under-converged biharmonic CG stops each channel at a different point -> per-channel
  // inconsistency -> chroma drift. maxit (not max_iter) is the honest best-effort here.
  (void)max_iter;

  uint8_t *const restrict hole = (uint8_t *)dt_pixelpipe_cache_alloc_align(sizeof(uint8_t) * region_pixels, pipe);
  float *const restrict solver_field
      = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe); // solver working field
  float *const restrict fill_planes
      = dt_pixelpipe_cache_alloc_align_float(region_pixels * 3, pipe);                        // fused-fill planes
  float *const restrict dome_lum = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe); // luminance dome
  float *const restrict lum_accum
      = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe); // luminance accum (chroma denom)
  float *const restrict reaction_weight
      = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe); // chroma reaction weight
  float *const restrict flat_target
      = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe); // chroma flat target
  float *const restrict cg_residual = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe); // CG scratch
  float *const restrict cg_dir = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
  float *const restrict cg_operator = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
  float *const restrict cg_tmp1 = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
  float *const restrict cg_tmp2 = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);

  _hl_region_ctx_t ctx = {
    .interp = interp,
    .mask = mask,
    .depth = depth,
    .width = width,
    .region = region,
    .pipe = pipe,
    .region_w = region_w,
    .region_h = region_h,
    .region_pixels = region_pixels,
    .extent = extent,
    .epsilon = epsilon,
    .max_cg_iter = max_cg_iter,
    .solid_color = solid_color,
    .noise_level = noise_level,
    .floor_gate = floor_gate,
    .estimate = estimate,
    .prev_scale = prev_scale,
    .valid = valid,
    .blur_in = blur_in,
    .plane1 = plane1,
    .plane2 = plane2,
    .plane3 = plane3,
    .valid_variance = valid_variance,
    .guide_score = guide_score,
    .clip_depth = clip_depth,
    .clip0 = clip0,
    .hole = hole,
    .solver_field = solver_field,
    .fill_planes = fill_planes,
    .dome_lum = dome_lum,
    .lum_accum = lum_accum,
    .reaction_weight = reaction_weight,
    .flat_target = flat_target,
    .cg_residual = cg_residual,
    .cg_dir = cg_dir,
    .cg_operator = cg_operator,
    .cg_tmp1 = cg_tmp1,
    .cg_tmp2 = cg_tmp2,
  };

  _region_gather(&ctx);

  if(hole && solver_field && fill_planes && dome_lum && lum_accum && reaction_weight && flat_target && cg_residual
     && cg_dir && cg_operator && cg_tmp1 && cg_tmp2)
  {
    _cf_reconstruct(&ctx);
    _selfdome(&ctx);
    _joint_core(&ctx);
    _aniso_chroma(&ctx);
    _chromaticity_gradient(&ctx);
    // Per-region timing breakdown. Only for regions big enough to matter (small ones are noise) and
  }
  dt_pixelpipe_cache_free_align(hole);
  dt_pixelpipe_cache_free_align(solver_field);
  dt_pixelpipe_cache_free_align(fill_planes);
  dt_pixelpipe_cache_free_align(dome_lum);
  dt_pixelpipe_cache_free_align(lum_accum);
  dt_pixelpipe_cache_free_align(reaction_weight);
  dt_pixelpipe_cache_free_align(flat_target);
  dt_pixelpipe_cache_free_align(cg_residual);
  dt_pixelpipe_cache_free_align(cg_dir);
  dt_pixelpipe_cache_free_align(cg_operator);
  dt_pixelpipe_cache_free_align(cg_tmp1);
  dt_pixelpipe_cache_free_align(cg_tmp2);

  _region_composite(&ctx);

  dt_pixelpipe_cache_free_align(estimate);
  dt_pixelpipe_cache_free_align(prev_scale);
  dt_pixelpipe_cache_free_align(valid);
  dt_pixelpipe_cache_free_align(blur_in);
  dt_pixelpipe_cache_free_align(plane1);
  dt_pixelpipe_cache_free_align(plane2);
  dt_pixelpipe_cache_free_align(plane3);
  dt_pixelpipe_cache_free_align(valid_variance);
  dt_pixelpipe_cache_free_align(guide_score);
  dt_pixelpipe_cache_free_align(clip_depth);
  dt_pixelpipe_cache_free_align(clip0);
}

// ---------------------------------------------------------------------------------------------
// R9 sensor-rolloff (knee) estimation + inversion. See the DT_HL_KNEE macro comment for the why.
// All values are handled in CLIP-NORMALIZED units: x = value / (clip level), so the detection
// threshold sits at DT_HL_KNEE_DET (the clips[] passed around equal 0.995 * clip level) and the
// band under estimation is [DT_HL_KNEE_LO, DT_HL_KNEE_DET).
// ---------------------------------------------------------------------------------------------

// ============================ OpenCL ============================

#if defined(HAVE_OPENCL) && DT_HL_COEFF_FIELD && DT_HL_SPARSE_SOLVE && (DT_HL_ANISO_SOLVER == 2)
// Device counterpart (per-region GPU orchestrator) of _region_guided_filter: gathers the
// padded region window, derives the stage parameters from one on-device reduction
// (union-hole plateau brightness -> cf_binv, per-channel clip counts -> deep channel, union
// count -> shared dome grid), then chains the proven stages -- coefficient field
// (_cf_stage_cl), high-frequency detail hybrid (_hf_stage_cl), floors + gated self-dome
// (_selfdome_stage_cl), all-clip joint core (_joint_core_stage_cl), divergence-form
// anisotropic chroma (_aniso_stage_cl) -- and scatters the clipped channels back. Everything
// stays on the device except the reduction partials. Caller must handle noise_level > 0 on
// the CPU (the grain epilogue is not ported).
// Any change here must be mirrored in _region_guided_filter (CPU) and re-validated with the
// HL_REGCL_TEST self-test (_region_guided_filter_cl_selftest).
// Regions below this pixel count are reconstructed on the CPU even when the pipe runs on the
// GPU: a device region pays ~1000 kernel launches (iterative stages, per-level sparse solves)

cl_int _region_cpu_offload_cl(const int devid, void *gd_void, cl_mem interp, cl_mem mask, cl_mem depth,
                              const int width, const _hl_region_t *const region, const dt_dev_pixelpipe_t *pipe,
                              const float solid_color, const int max_iter, const float noise_level,
                              const float floor_gate)
{
  dt_iop_highlights_global_data_t *global_data = (dt_iop_highlights_global_data_t *)gd_void;
  const int region_w = region->rx1 - region->rx0 + 1;
  const int region_h = region->ry1 - region->ry0 + 1;
  if(region_w < 2 || region_h < 2) return CL_SUCCESS;
  const size_t region_pixels = (size_t)region_w * region_h;

  cl_int cl_err = DT_OPENCL_DEFAULT_ERROR;
  size_t work_size[3] = { ROUNDUPDWD(region_w, devid), ROUNDUPDHT(region_h, devid), 1 };

  cl_mem staging = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels * 9);
  float *host = dt_pixelpipe_cache_alloc_align_float(region_pixels * 9, pipe);
  if(!staging || IS_NULL_PTR(host)) goto out;

  {
    const int kernel = global_data->kernel_hl_window_pack;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &interp);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &mask);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &depth);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &staging);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &region->rx0);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &region->ry0);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &region_h);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, work_size);
    if(cl_err != CL_SUCCESS) goto out;
  }

  cl_err = dt_opencl_read_buffer_from_device(devid, host, staging, 0, sizeof(float) * region_pixels * 9, CL_TRUE);
  if(cl_err != CL_SUCCESS) goto out;

  {
    float *const hw_interp = host;
    const float *const hw_mask = host + region_pixels * 4;
    const float *const hw_depth = host + region_pixels * 8;

    _hl_region_t translated_region = *region;
    translated_region.x0 -= region->rx0;
    translated_region.x1 -= region->rx0;
    translated_region.y0 -= region->ry0;
    translated_region.y1 -= region->ry0;
    translated_region.rx1 -= region->rx0;
    translated_region.ry1 -= region->ry0;
    translated_region.rx0 = 0;
    translated_region.ry0 = 0;

    _region_guided_filter(hw_interp, hw_mask, hw_depth, region_w, &translated_region, pipe, solid_color, max_iter,
                          noise_level, floor_gate);
  }

  cl_err = dt_opencl_write_buffer_to_device(devid, host, staging, 0, sizeof(float) * region_pixels * 4, CL_TRUE);
  if(cl_err != CL_SUCCESS) goto out;

  {
    const int kernel = global_data->kernel_hl_window_unpack;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &staging);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &interp);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region->rx0);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region->ry0);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &region_h);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, work_size);
  }

out:
  dt_opencl_release_mem_object(staging);
  dt_pixelpipe_cache_free_align(host);
  return cl_err;
}

cl_int _region_guided_filter_cl(const int devid, void *gd_void, cl_mem interp, cl_mem mask, cl_mem depth,
                                const int width, const _hl_region_t *const region, const dt_dev_pixelpipe_t *pipe,
                                const float solid_color, const float floor_gate)
{
  dt_iop_highlights_global_data_t *global_data = (dt_iop_highlights_global_data_t *)gd_void;
  const int region_w = region->rx1 - region->rx0 + 1;
  const int region_h = region->ry1 - region->ry0 + 1;
  if(region_w < 2 || region_h < 2) return CL_SUCCESS;
  const size_t region_pixels = (size_t)region_w * region_h;
  if(region_pixels > (size_t)64 * 1024 * 1024) return CL_SUCCESS;

  cl_int cl_err = DT_OPENCL_DEFAULT_ERROR;
  size_t work_size[3] = { ROUNDUPDWD(region_w, devid), ROUNDUPDHT(region_h, devid), 1 };
  dt_gaussian_cl_t *cf_gaussian = NULL;

  cl_mem estimate = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels * 4);
  cl_mem valid = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels * 4);
  cl_mem clip0 = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels * 4);
  cl_mem model_quality = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels * 4);
  cl_mem clip_depth = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem lsb0 = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels); // pre-ladder luminance
  cl_mem partials = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 8 * 256);
  cl_mem steer = NULL; // coefficient-fill steering plane (guide structure)
  if(!estimate || !valid || !clip0 || !model_quality || !clip_depth || !lsb0 || !partials) goto out;

  // gather the padded region window into contiguous device buffers (est/clip0/vld/dep/lsb0)
  {
    const int kernel = global_data->kernel_hl_region_gather;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &interp);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &mask);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &depth);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &clip0);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(cl_mem), &clip_depth);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(cl_mem), &model_quality);
    dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(cl_mem), &lsb0);
    dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(int), &region->rx0);
    dt_opencl_set_kernel_arg(devid, kernel, 11, sizeof(int), &region->ry0);
    dt_opencl_set_kernel_arg(devid, kernel, 12, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 13, sizeof(int), &region_h);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, work_size);
    if(cl_err != CL_SUCCESS) goto out;
  }

  // ladder parameters from the pre-ladder statistics
  const float cf_sigma = CLAMP(region->radius / 6.f, 8.f, 64.f);
  const float cf_fmin = 0.05f;
  float cf_binv;
  float channel_means[3] = { 0.f, 0.f, 0.f }; // per-channel valid means (moment-pack centering)
  int cdeep, ds_shared;
  {
    const int local_size = 64, n_groups = 256;
    const int pixel_count = (int)region_pixels;
    const int kernel = global_data->kernel_hl_region_stats;
    size_t sizes[3] = { (size_t)n_groups * local_size, 1, 1 };
    size_t local[3] = { local_size, 1, 1 };
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &partials);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &pixel_count);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(float) * 8 * local_size, NULL);
    cl_err = dt_opencl_enqueue_kernel_2d_with_local(devid, kernel, sizes, local);
    if(cl_err != CL_SUCCESS) goto out;

    float partials_host[8 * 256];
    cl_err = dt_opencl_read_buffer_from_device(devid, partials_host, partials, 0, sizeof(float) * 8 * n_groups,
                                               CL_TRUE);
    if(cl_err != CL_SUCCESS) goto out;
    double lsum = 0.0, lcnt = 0.0, clip_count_r = 0.0, clip_count_g = 0.0, clip_count_b = 0.0;
    double msum[3] = { 0.0, 0.0, 0.0 };
    for(int group = 0; group < n_groups; group++)
    {
      lsum += (double)partials_host[8 * group + 0];
      lcnt += (double)partials_host[8 * group + 1];
      clip_count_r += (double)partials_host[8 * group + 2];
      clip_count_g += (double)partials_host[8 * group + 3];
      clip_count_b += (double)partials_host[8 * group + 4];
      msum[0] += (double)partials_host[8 * group + 5];
      msum[1] += (double)partials_host[8 * group + 6];
      msum[2] += (double)partials_host[8 * group + 7];
    }
    if(lcnt <= 0.0)
    {
      cl_err = CL_SUCCESS; // no clipped pixel in this window: nothing to do
      goto out;
    }
    const float cf_lref = (float)(lsum / lcnt);
    cf_binv = (cf_lref > 1e-9f) ? 1.f / (0.35f * cf_lref) : 0.f;
    cdeep = (clip_count_r >= clip_count_g && clip_count_r >= clip_count_b)
                ? 0
                : ((clip_count_g >= clip_count_b) ? 1 : 2);
    ds_shared = MAX(1, (int)ceilf(sqrtf((float)lcnt / (float)DT_HL_DOME_NMAX_SPARSE)));
    // per-channel means of the VALID values: the moment packs are centered on them (see the
    // CPU counterpart for the cancellation rationale)
    const double valid_count_r = (double)region_pixels - clip_count_r,
                 valid_count_g = (double)region_pixels - clip_count_g,
                 valid_count_b = (double)region_pixels - clip_count_b;
    channel_means[0] = valid_count_r > 0.5 ? (float)(msum[0] / valid_count_r) : 0.f;
    channel_means[1] = valid_count_g > 0.5 ? (float)(msum[1] / valid_count_g) : 0.f;
    channel_means[2] = valid_count_b > 0.5 ? (float)(msum[2] / valid_count_b) : 0.f;
  }

  // one gaussian handle serves every cf_sigma blur of the region (each init allocates two
  // region-sized temp buffers -- 13+ per-blur re-allocations were pure churn)
  cf_gaussian = _region_blur_handle(devid, region_w, region_h, cf_sigma);

  // Steering plane for the coefficient fills = the measured guide structure, built ONCE here
  // (same est state as the CPU: after the saturation floor) and shared by the coefficient-field
  // and HF stages, exactly like the CPU path.
  {
    steer = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
    if(steer)
    {
      const int kernel = global_data->kernel_hl_cfa_steer;
      const int pixel_count = (int)region_pixels;
      size_t work_size_1d[3] = { ROUNDUPDWD(pixel_count, devid), 1, 1 };
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &steer);
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &pixel_count);
      cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, work_size_1d);
      if(cl_err != CL_SUCCESS) goto out;
    }
  }

  cl_err = _cf_stage_cl(devid, gd_void, estimate, valid, model_quality, lsb0, steer, channel_means, cf_gaussian,
                        region_w, region_h, cf_sigma, cf_fmin, cf_binv, cdeep);
  if(cl_err == CL_SUCCESS) dt_opencl_finish(devid);
  if(cl_err != CL_SUCCESS) goto out;
  cl_err = _hf_stage_cl(devid, gd_void, estimate, valid, model_quality, lsb0, steer, cf_gaussian, region_w,
                        region_h, cf_sigma, cf_fmin, cf_binv);
  if(cl_err == CL_SUCCESS) dt_opencl_finish(devid);
  if(cl_err != CL_SUCCESS) goto out;

  // gated self-dome: the soft floor is unconditional (production applies it right after the
  // HF hybrid); the dome + blend + hard floor only run where a clipped channel with a
  // surviving guide sits on a weak colour-line
  int need_self = 0;
  {
    const int local_size = 64, n_groups = 256;
    const int pixel_count = (int)region_pixels;
    const int kernel = global_data->kernel_hl_need_self;
    size_t sizes[3] = { (size_t)n_groups * local_size, 1, 1 };
    size_t local[3] = { local_size, 1, 1 };
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &model_quality);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &clip_depth);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &partials);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &pixel_count);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(float), &cf_sigma);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(float) * local_size, NULL);
    cl_err = dt_opencl_enqueue_kernel_2d_with_local(devid, kernel, sizes, local);
    if(cl_err != CL_SUCCESS) goto out;
    float partials_host[256];
    cl_err
        = dt_opencl_read_buffer_from_device(devid, partials_host, partials, 0, sizeof(float) * n_groups, CL_TRUE);
    if(cl_err != CL_SUCCESS) goto out;
    for(int group = 0; group < n_groups; group++)
      if(partials_host[group] > 0.f) need_self = 1;
  }

  if(need_self)
  {
    cl_err = _selfdome_stage_cl(devid, gd_void, estimate, valid, model_quality, clip0, clip_depth, region_w,
                                region_h, cf_sigma, region->radius, ds_shared, floor_gate, pipe);
    if(cl_err != CL_SUCCESS) goto out;
  }
  else
  {
    const int kernel = global_data->kernel_hl_soft_floor;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &clip0);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(float), &floor_gate);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, work_size);
    if(cl_err != CL_SUCCESS) goto out;
  }

  {
    // Step 7: all-clip joint core (shared biharmonic dome x screened-Poisson diffused chroma)
    const int extent = MAX(region->x1 - region->x0, region->y1 - region->y0) + 1;
    cl_err = _joint_core_stage_cl(devid, gd_void, estimate, valid, clip0, region_w, region_h, solid_color,
                                  region->radius, extent, floor_gate, pipe);
    if(cl_err == CL_SUCCESS) dt_opencl_finish(devid);
  }
  if(cl_err != CL_SUCCESS) goto out;
  // Step 8: structure-steered chrominance coherence (div(D grad r)=0 under the obstacle r >= c0/L)
  cl_err = _aniso_stage_cl(devid, gd_void, estimate, valid, clip0, region_w, region_h, region->radius,
                           floor_gate, solid_color, pipe);
  if(cl_err == CL_SUCCESS) dt_opencl_finish(devid);
  if(cl_err != CL_SUCCESS) goto out;

  // Step 9: gradient-extending chroma (chromaticity-gradient continuation, article addendum)
  cl_err = _chromaticity_gradient_stage_cl(devid, gd_void, estimate, valid, clip0, region_w, region_h,
                                           region->radius, floor_gate, pipe);
  if(cl_err == CL_SUCCESS) dt_opencl_finish(devid);
  if(cl_err != CL_SUCCESS) goto out;

  // scatter the reconstructed clipped channels back into the full-res buffer
  {
    const int kernel = global_data->kernel_hl_region_scatter;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &interp);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &mask);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region->rx0);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &region->ry0);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &region_h);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, work_size);
  }

out:
  dt_gaussian_free_cl(cf_gaussian);
  dt_opencl_release_mem_object(estimate);
  dt_opencl_release_mem_object(valid);
  dt_opencl_release_mem_object(clip0);
  dt_opencl_release_mem_object(model_quality);
  dt_opencl_release_mem_object(clip_depth);
  dt_opencl_release_mem_object(lsb0);
  dt_opencl_release_mem_object(partials);
  dt_opencl_release_mem_object(steer);
  return cl_err;
}

#endif // HAVE_OPENCL && DT_HL_COEFF_FIELD && DT_HL_SPARSE_SOLVE && ANISO_SOLVER 2
