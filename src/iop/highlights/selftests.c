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

// CPU/GPU parity self-tests of the harmonic OpenCL port (each vs a CPU replica). (implementation; see selftests.h
// for the public API.)

#include "common/darktable.h"
#include "common/gaussian.h"
#include "develop/imageop_math.h"
#include "iop/highlights/blur.h"
#include "iop/highlights/chroma.h"
#include "iop/highlights/coefficient_field.h"
#include "iop/highlights/core.h"
#include "iop/highlights/dome.h"
#include "iop/highlights/knee.h"
#include "iop/highlights/pde.h"
#include "iop/highlights/region.h"
#include "iop/highlights/selftests.h"
#include <math.h>
#include <string.h>

#ifdef HAVE_OPENCL

void _sp_chol_cl_selftest(const int devid, void *gd_void, const dt_dev_pixelpipe_t *pipe)
{
  static int done = 0;
  if(done || !getenv("HL_SPCL_TEST")) return;
  done = 1;

  const int grid = 96;
  uint8_t *hole = calloc((size_t)grid * grid, 1);
  int *grid_index = malloc(sizeof(int) * grid * grid);
  int dimension = 0;
  for(int y = 0; y < grid; y++)
    for(int x = 0; x < grid; x++)
    {
      const int delta_x = x - grid / 2;
      const int delta_y = y - grid / 2;
      hole[y * grid + x] = (delta_x * delta_x + delta_y * delta_y < 34 * 34);
      grid_index[y * grid + x] = hole[y * grid + x] ? dimension++ : -1;
    }

  static const int stencil_dy[13] = { 0, -1, 1, 0, 0, -1, -1, 1, 1, -2, 2, 0, 0 };
  static const int stencil_dx[13] = { 0, 0, 0, -1, 1, -1, 1, -1, 1, 0, 0, -2, 2 };
  static const double stencil_coeff[13] = { 20., -8., -8., -8., -8., 2., 2., 2., 2., 1., 1., 1., 1. };

  int *matrix_col_ptr = calloc(dimension + 1, sizeof(int));
  int *matrix_row_index = malloc(sizeof(int) * (size_t)dimension * 13);
  double *matrix_values = malloc(sizeof(double) * (size_t)dimension * 13);
  double *rhs = malloc(sizeof(double) * dimension);
  double *solution_cpu = malloc(sizeof(double) * dimension);
  int n_nonzero = 0;
  for(int y = 0; y < grid; y++)
    for(int x = 0; x < grid; x++)
    {
      const int j = grid_index[y * grid + x];
      if(j < 0) continue;
      rhs[j] = 0.5 + 0.001 * (double)((x * 131 + y * 17) % 97);
      matrix_col_ptr[j] = n_nonzero;
      for(int tap_index = 0; tap_index < 13; tap_index++)
      {
        const int neighbour_y = CLAMP(y + stencil_dy[tap_index], 0, grid - 1);
        const int neighbour_x = CLAMP(x + stencil_dx[tap_index], 0, grid - 1);
        const int neighbour_index = grid_index[neighbour_y * grid + neighbour_x];
        if(neighbour_index < 0 || neighbour_index > j) continue;
        int scan = matrix_col_ptr[j];
        for(; scan < n_nonzero; scan++)
          if(matrix_row_index[scan] == neighbour_index)
          {
            matrix_values[scan] += stencil_coeff[tap_index];
            break;
          }
        if(scan == n_nonzero)
        {
          matrix_row_index[n_nonzero] = neighbour_index;
          matrix_values[n_nonzero] = stencil_coeff[tap_index];
          n_nonzero++;
        }
      }
    }
  matrix_col_ptr[dimension] = n_nonzero;

  // CPU reference
  memcpy(solution_cpu, rhs, sizeof(double) * dimension);
  _sp_chol_t *factor_cpu = _sp_chol_factor(dimension, matrix_col_ptr, matrix_row_index, matrix_values, pipe);
  if(factor_cpu) _sp_chol_solve(factor_cpu, solution_cpu);

  // GPU
  _sp_chol_cl_t *factor_gpu = _sp_chol_factor_cl(devid, _hl_sp_chol_kernels(gd_void), dimension, matrix_col_ptr,
                                                 matrix_row_index, matrix_values);
  double max_rel_diff = -1.0;
  if(factor_cpu && factor_gpu)
  {
    cl_mem rhs_device = _sp_cl_upload(devid, rhs, sizeof(double) * dimension);
    if(rhs_device && !_sp_chol_solve_cl(factor_gpu, _hl_sp_chol_kernels(gd_void), rhs_device))
    {
      double *solution_gpu = malloc(sizeof(double) * dimension);
      if(dt_opencl_read_buffer_from_device(devid, solution_gpu, rhs_device, 0, sizeof(double) * dimension, CL_TRUE)
         == CL_SUCCESS)
      {
        max_rel_diff = 0.0;
        for(int i = 0; i < dimension; i++)
        {
          const double rel_diff = fabs(solution_gpu[i] - solution_cpu[i]) / fmax(fabs(solution_cpu[i]), 1e-12);
          if(rel_diff > max_rel_diff) max_rel_diff = rel_diff;
        }
      }
      free(solution_gpu);
    }
    dt_opencl_release_mem_object(rhs_device);
  }
  fprintf(stderr, "[hl sparse-cl selftest] n=%d nnz=%d levels=%d/%d cpu=%s gpu=%s max rel diff=%.3e\n", dimension,
          factor_cpu ? factor_cpu->col_ptr[dimension] : -1, factor_gpu ? factor_gpu->nlev : -1,
          factor_gpu ? factor_gpu->nlev_bwd : -1, factor_cpu ? "ok" : "FAIL", factor_gpu ? "ok" : "FAIL",
          max_rel_diff);

  _sp_chol_free(factor_cpu);
  _sp_chol_cl_free(factor_gpu);
  free(hole);
  free(grid_index);
  free(matrix_col_ptr);
  free(matrix_row_index);
  free(matrix_values);
  free(rhs);
  free(solution_cpu);
}

void _region_blur_cl_selftest(const int devid, const dt_dev_pixelpipe_t *pipe)
{
  static int done = 0;
  if(done || !getenv("HL_BLURCL_TEST") || devid < 0) return;
  done = 1;

  const int region_w = 731;
  const int region_h = 517;
  const size_t region_pixels = (size_t)region_w * region_h;
  float *input = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *output_cpu = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *output_gpu = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  if(IS_NULL_PTR(input) || IS_NULL_PTR(output_cpu) || IS_NULL_PTR(output_gpu)) goto done_;

  for(size_t i = 0; i < region_pixels; i++)
    for(int c = 0; c < 4; c++)
      input[i * 4 + c] = 0.5f + 0.5f * sinf(0.013f * (float)(i % region_w) + 0.007f * (float)(i / region_w) + c);

  static const float test_sigmas[3] = { 4.f, 16.f, 64.f };
  for(int sigma_index = 0; sigma_index < 3; sigma_index++)
  {
    const float sigma = test_sigmas[sigma_index];
    _region_blur(input, output_cpu, region_w, region_h, sigma);

    cl_mem in_device = dt_opencl_alloc_device(devid, region_w, region_h, sizeof(float) * 4);
    cl_mem out_device = dt_opencl_alloc_device(devid, region_w, region_h, sizeof(float) * 4);
    float max_diff = -1.f;
    if(in_device && out_device
       && dt_opencl_write_host_to_device(devid, input, in_device, region_w, region_h, sizeof(float) * 4)
              == CL_SUCCESS
       && _region_blur_cl(devid, in_device, out_device, region_w, region_h, sigma) == CL_SUCCESS
       && dt_opencl_copy_device_to_host(devid, output_gpu, out_device, region_w, region_h, sizeof(float) * 4)
              == CL_SUCCESS)
    {
      max_diff = 0.f;
      for(size_t i = 0; i < region_pixels * 4; i++)
        max_diff = fmaxf(max_diff, fabsf(output_gpu[i] - output_cpu[i]));
    }
    dt_opencl_release_mem_object(in_device);
    dt_opencl_release_mem_object(out_device);
    fprintf(stderr, "[hl blur-cl selftest] %dx%d sigma=%.0f max|gpu-cpu|=%.3e\n", region_w, region_h, sigma,
            max_diff);
  }

done_:
  dt_pixelpipe_cache_free_align(input);
  dt_pixelpipe_cache_free_align(output_cpu);
  dt_pixelpipe_cache_free_align(output_gpu);
}

void _cf_harmonic_fill_cl_selftest(const int devid, void *gd_void, const dt_dev_pixelpipe_t *pipe)
{
  static int done = 0;
  if(done || !getenv("HL_FILLCL_TEST") || devid < 0) return;
  done = 1;

  const int region_w = 613;
  const int region_h = 419;
  const size_t region_pixels = (size_t)region_w * region_h;
  float *val_cpu = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
  float *val_gpu = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
  uint8_t *hole = (uint8_t *)dt_pixelpipe_cache_alloc_align(region_pixels, pipe); // 1 = to fill
  uint8_t *anchor
      = (uint8_t *)dt_pixelpipe_cache_alloc_align(region_pixels, pipe); // 1 = trusted (GPU kernel convention)
  if(IS_NULL_PTR(val_cpu) || IS_NULL_PTR(val_gpu) || IS_NULL_PTR(hole) || IS_NULL_PTR(anchor)) goto done_;

  for(int y = 0; y < region_h; y++)
    for(int x = 0; x < region_w; x++)
    {
      const size_t i = (size_t)y * region_w + x;
      const int delta_x = x - region_w / 2;
      const int delta_y = y - (region_h - 30);
      hole[i] = (delta_x * delta_x + delta_y * delta_y < 120 * 120);
      anchor[i] = !hole[i];
      val_cpu[i] = hole[i] ? 0.f : 0.3f + 0.4f * sinf(0.02f * x) * cosf(0.017f * y);
    }
  memcpy(val_gpu, val_cpu, sizeof(float) * region_pixels);

  _cf_harmonic_fill(val_cpu, hole, region_w, region_h, 2, NULL, pipe);

  cl_mem dval = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem danc = dt_opencl_alloc_device_buffer(devid, region_pixels);
  float max_diff = -1.f;
  if(dval && danc
     && dt_opencl_write_buffer_to_device(devid, val_gpu, dval, 0, sizeof(float) * region_pixels, CL_TRUE)
            == CL_SUCCESS
     && dt_opencl_write_buffer_to_device(devid, anchor, danc, 0, region_pixels, CL_TRUE) == CL_SUCCESS
     && _cf_harmonic_fill_cl(devid, gd_void, dval, danc, region_w, region_h, 2, 0, NULL) == CL_SUCCESS
     && dt_opencl_read_buffer_from_device(devid, val_gpu, dval, 0, sizeof(float) * region_pixels, CL_TRUE)
            == CL_SUCCESS)
  {
    max_diff = 0.f;
    for(size_t i = 0; i < region_pixels; i++)
      if(hole[i]) max_diff = fmaxf(max_diff, fabsf(val_gpu[i] - val_cpu[i]));
  }
  dt_opencl_release_mem_object(dval);
  dt_opencl_release_mem_object(danc);
  fprintf(stderr, "[hl fill-cl selftest] %dx%d hole=disc r120 max|gpu-cpu|=%.3e\n", region_w, region_h, max_diff);

  // aniso leg: same fill with the variance-adaptive tensor (mode 3) steered by a synthetic
  // plane carrying both a smooth ramp and a hard dark bar through the hole (occluder-like).
  {
    float *steer = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
    if(steer)
    {
      for(int y = 0; y < region_h; y++)
        for(int x = 0; x < region_w; x++)
        {
          const size_t i = (size_t)y * region_w + x;
          const int dark_bar = (x > region_w / 2 - 12 && x < region_w / 2 + 12);
          steer[i] = dark_bar ? 0.05f : (0.4f + 0.5f * (float)y / region_h);
          val_cpu[i] = hole[i] ? 0.f : 0.3f + 0.4f * sinf(0.02f * x) * cosf(0.017f * y);
        }
      memcpy(val_gpu, val_cpu, sizeof(float) * region_pixels);

      _cf_harmonic_fill(val_cpu, hole, region_w, region_h, 2, steer, pipe);

      cl_mem aniso_val_device = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
      cl_mem aniso_anchor_device = dt_opencl_alloc_device_buffer(devid, region_pixels);
      cl_mem aniso_steer_device = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
      float max_diff_aniso = -1.f;
      if(aniso_val_device && aniso_anchor_device && aniso_steer_device
         && dt_opencl_write_buffer_to_device(devid, val_gpu, aniso_val_device, 0, sizeof(float) * region_pixels,
                                             CL_TRUE)
                == CL_SUCCESS
         && dt_opencl_write_buffer_to_device(devid, anchor, aniso_anchor_device, 0, region_pixels, CL_TRUE)
                == CL_SUCCESS
         && dt_opencl_write_buffer_to_device(devid, steer, aniso_steer_device, 0, sizeof(float) * region_pixels,
                                             CL_TRUE)
                == CL_SUCCESS
         && _cf_harmonic_fill_cl(devid, gd_void, aniso_val_device, aniso_anchor_device, region_w, region_h, 2, 0,
                                 aniso_steer_device)
                == CL_SUCCESS
         && dt_opencl_read_buffer_from_device(devid, val_gpu, aniso_val_device, 0, sizeof(float) * region_pixels,
                                              CL_TRUE)
                == CL_SUCCESS)
      {
        max_diff_aniso = 0.f;
        for(size_t i = 0; i < region_pixels; i++)
          if(hole[i]) max_diff_aniso = fmaxf(max_diff_aniso, fabsf(val_gpu[i] - val_cpu[i]));
      }
      dt_opencl_release_mem_object(aniso_val_device);
      dt_opencl_release_mem_object(aniso_anchor_device);
      dt_opencl_release_mem_object(aniso_steer_device);
      fprintf(stderr, "[hl fill-cl ANISO selftest] %dx%d adaptive-tensor max|gpu-cpu|=%.3e\n", region_w, region_h,
              max_diff_aniso);
    }
    dt_pixelpipe_cache_free_align(steer);
  }

done_:
  dt_pixelpipe_cache_free_align(val_cpu);
  dt_pixelpipe_cache_free_align(val_gpu);
  dt_pixelpipe_cache_free_align(hole);
  dt_pixelpipe_cache_free_align(anchor);
}

void _cf_joint_stage_cl_selftest(const int devid, void *gd_void, const dt_dev_pixelpipe_t *pipe)
{
  static int done = 0;
  if(done || !getenv("HL_CFCL_TEST") || devid < 0) return;
  done = 1;

  const int region_w = 509;
  const int region_h = 371;
  const size_t region_pixels = (size_t)region_w * region_h;
  const float cf_sigma = 24.f;
  const float cf_fmin = 0.05f;
  const int c = 1;
  const int guide1 = 0;
  const int guide2 = 2;

  float *estimate = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *valid = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *model_quality = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *estimate_gpu = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *input = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *moment1 = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *moment2 = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *moment3 = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *coeff_field = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *plane = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
  uint8_t *anchor = (uint8_t *)dt_pixelpipe_cache_alloc_align(region_pixels, pipe);
  uint8_t *border = (uint8_t *)dt_pixelpipe_cache_alloc_align(region_pixels, pipe);
  if(IS_NULL_PTR(estimate) || IS_NULL_PTR(valid) || IS_NULL_PTR(model_quality) || IS_NULL_PTR(estimate_gpu)
     || IS_NULL_PTR(input) || IS_NULL_PTR(moment1) || IS_NULL_PTR(moment2) || IS_NULL_PTR(moment3)
     || IS_NULL_PTR(coeff_field) || IS_NULL_PTR(plane) || IS_NULL_PTR(anchor) || IS_NULL_PTR(border))
    goto done_;

  for(int y = 0; y < region_h; y++)
    for(int x = 0; x < region_w; x++)
    {
      const size_t i = (size_t)y * region_w + x;
      const float base = 0.4f + 0.3f * sinf(0.011f * x) * cosf(0.014f * y);
      estimate[i * 4 + 0] = 0.9f * base + 0.05f;
      estimate[i * 4 + 1] = 1.2f * base + 0.02f;
      estimate[i * 4 + 2] = 0.7f * base + 0.08f;
      estimate[i * 4 + 3] = 0.f;
      const int delta_x = x - region_w / 2;
      const int delta_y = y - region_h / 2;
      const int clip = (delta_x * delta_x + delta_y * delta_y < 90 * 90);
      valid[i * 4 + 0] = 1.f;
      valid[i * 4 + 1] = clip ? 0.f : 1.f;
      valid[i * 4 + 2] = 1.f;
      valid[i * 4 + 3] = clip ? 0.f : 1.f;
      for(int k = 0; k < 4; k++) model_quality[i * 4 + k] = 0.f;
      if(clip) estimate[i * 4 + 1] = 0.55f;
    }
  memcpy(estimate_gpu, estimate, region_pixels * 4 * sizeof(float));

  double lum_accum = 0.0;
  size_t lum_count = 0;
  for(size_t i = 0; i < region_pixels; i++)
    if(valid[i * 4 + 1] < 0.5f)
    {
      lum_accum += estimate[i * 4 + 0] + estimate[i * 4 + 1] + estimate[i * 4 + 2];
      lum_count++;
    }
  const float cf_lref = lum_count ? (float)(lum_accum / (double)lum_count) : 0.f;
  const float cf_binv = (cf_lref > 1e-9f) ? 1.f / (0.35f * cf_lref) : 0.f;

  // ---- CPU replica ----
  for(int mode = 0; mode < 3; mode++)
  {
    for(size_t i = 0; i < region_pixels; i++)
    {
      const float val_r = estimate[i * 4 + 0];
      const float val_g = estimate[i * 4 + 1];
      const float val_b = estimate[i * 4 + 2];
      const float rgb_sum = val_r + val_g + val_b;
      const float bright_weight = (cf_binv > 0.f) ? sqf(fminf(rgb_sum * cf_binv, 1.f)) : 1.f;
      const int all_valid = (valid[i * 4 + 0] >= 0.5f && valid[i * 4 + 1] >= 0.5f && valid[i * 4 + 2] >= 0.5f);
      const float weight = all_valid ? bright_weight : 0.f;
      if(mode == 0)
      {
        input[i * 4 + 0] = weight;
        input[i * 4 + 1] = weight * val_r;
        input[i * 4 + 2] = weight * val_g;
        input[i * 4 + 3] = weight * val_b;
      }
      else if(mode == 1)
      {
        input[i * 4 + 0] = weight * val_r * val_r;
        input[i * 4 + 1] = weight * val_g * val_g;
        input[i * 4 + 2] = weight * val_b * val_b;
        input[i * 4 + 3] = weight * val_r * val_g;
      }
      else
      {
        input[i * 4 + 0] = weight * val_r * val_b;
        input[i * 4 + 1] = weight * val_g * val_b;
        input[i * 4 + 2] = all_valid ? 1.f : 0.f;
        input[i * 4 + 3] = 0.f;
      }
    }
    _region_blur(input, (mode == 0) ? moment1 : (mode == 1) ? moment2 : moment3, region_w, region_h, cf_sigma);
  }

  for(size_t i = 0; i < region_pixels; i++)
  {
    const float norm = fmaxf(moment1[i * 4 + 0], 1e-9f);
    const float inv_det = 1.f / norm;
    const float mean[3]
        = { moment1[i * 4 + 1] * inv_det, moment1[i * 4 + 2] * inv_det, moment1[i * 4 + 3] * inv_det };
    const float second_moment[3]
        = { moment2[i * 4 + 0] * inv_det, moment2[i * 4 + 1] * inv_det, moment2[i * 4 + 2] * inv_det };
    const float cross_rg = moment2[i * 4 + 3] * inv_det;
    const float cross_rb = moment3[i * 4 + 0] * inv_det;
    const float cross_gb = moment3[i * 4 + 1] * inv_det;
#define OFF2(chan_a, chan_b)                                                                                      \
  (((chan_a) + (chan_b)) == 1 ? cross_rg : (((chan_a) + (chan_b)) == 2 ? cross_rb : cross_gb))
    const float mean1 = mean[guide1];
    const float mean2 = mean[guide2];
    const float mean_target = mean[c];
    const float var11 = fmaxf(second_moment[guide1] - mean1 * mean1, 0.f);
    const float var22 = fmaxf(second_moment[guide2] - mean2 * mean2, 0.f);
    const float var12 = OFF2(guide1, guide2) - mean1 * mean2;
    const float cov_tg1 = OFF2(c, guide1) - mean_target * mean1;
    const float cov_tg2 = OFF2(c, guide2) - mean_target * mean2;
    const float var_target = fmaxf(second_moment[c] - mean_target * mean_target, 0.f);
#undef OFF2
    const float lambda = 1e-3f * 0.5f * (var11 + var22) + 1e-12f;
    const float determinant = fmaxf((var11 + lambda) * (var22 + lambda) - var12 * var12, 1e-18f);
    const float slope_a = ((var22 + lambda) * cov_tg1 - var12 * cov_tg2) / determinant;
    const float slope_b = ((var11 + lambda) * cov_tg2 - var12 * cov_tg1) / determinant;
    const float r_sq = CLAMP((slope_a * cov_tg1 + slope_b * cov_tg2) / (var_target + 1e-12f), 0.f, 1.f);
    coeff_field[i * 4 + 0] = slope_a;
    coeff_field[i * 4 + 1] = slope_b;
    coeff_field[i * 4 + 2] = mean_target - slope_a * mean1 - slope_b * mean2;
    coeff_field[i * 4 + 3] = r_sq;
    const int mass_ok = (moment3[i * 4 + 2] > cf_fmin && moment1[i * 4 + 0] > 0.25f * moment3[i * 4 + 2]);
    const int valid_ok = (valid[i * 4 + c] >= 0.5f);
    anchor[i] = (mass_ok && valid_ok && r_sq > 0.25f && fabsf(slope_a) < 64.f && fabsf(slope_b) < 64.f);
    border[i] = (mass_ok && valid_ok);
  }
  {
    uint8_t *hole = (uint8_t *)dt_pixelpipe_cache_alloc_align(region_pixels, pipe);
    uint8_t *hole2 = (uint8_t *)dt_pixelpipe_cache_alloc_align(region_pixels, pipe);
    if(hole && hole2)
    {
      for(size_t i = 0; i < region_pixels; i++)
      {
        hole[i] = !anchor[i];
        hole2[i] = !border[i];
      }
      const int base_downsample = (int)(cf_sigma / 4.f);
      for(int k = 0; k < 4; k++)
      {
        for(size_t i = 0; i < region_pixels; i++) plane[i] = coeff_field[i * 4 + k];
        _cf_harmonic_fill(plane, (k == 3) ? hole2 : hole, region_w, region_h, base_downsample, NULL, pipe);
        for(size_t i = 0; i < region_pixels; i++) coeff_field[i * 4 + k] = plane[i];
      }
    }
    dt_pixelpipe_cache_free_align(hole);
    dt_pixelpipe_cache_free_align(hole2);
  }
  for(size_t i = 0; i < region_pixels; i++)
    if(valid[i * 4 + c] < 0.5f && valid[i * 4 + guide1] >= 0.5f && valid[i * 4 + guide2] >= 0.5f)
      estimate[i * 4 + c] = coeff_field[i * 4 + 0] * estimate[i * 4 + guide1]
                            + coeff_field[i * 4 + 1] * estimate[i * 4 + guide2] + coeff_field[i * 4 + 2];

  // ---- GPU stage ----
  {
    cl_mem dest = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels * 4);
    cl_mem dvld = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels * 4);
    cl_mem dbsc = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels * 4);
    cl_mem dlsb = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
    float *luminance = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
    float max_diff = -1.f;
    if(luminance)
      for(size_t i = 0; i < region_pixels; i++)
        luminance[i] = estimate_gpu[i * 4 + 0] + estimate_gpu[i * 4 + 1] + estimate_gpu[i * 4 + 2];
    size_t work_size[3] = { ROUNDUPDWD(region_w, devid), ROUNDUPDHT(region_h, devid), 1 };
    cl_mem packed_device = dt_opencl_alloc_device(devid, work_size[0], work_size[1], sizeof(float) * 4);
    cl_mem moment0_device = dt_opencl_alloc_device(devid, work_size[0], work_size[1], sizeof(float) * 4);
    cl_mem moment1_device = dt_opencl_alloc_device(devid, work_size[0], work_size[1], sizeof(float) * 4);
    cl_mem moment2_device = dt_opencl_alloc_device(devid, work_size[0], work_size[1], sizeof(float) * 4);
    cl_mem moments_device[3];
    moments_device[0] = moment0_device;
    moments_device[1] = moment1_device;
    moments_device[2] = moment2_device;
    int moms_ok = (packed_device && moment0_device && moment1_device && moment2_device);
    if(moms_ok && dest && dvld && dbsc && dlsb && luminance
       && dt_opencl_write_buffer_to_device(devid, estimate_gpu, dest, 0, sizeof(float) * region_pixels * 4, CL_TRUE)
              == CL_SUCCESS
       && dt_opencl_write_buffer_to_device(devid, valid, dvld, 0, sizeof(float) * region_pixels * 4, CL_TRUE)
              == CL_SUCCESS
       && dt_opencl_write_buffer_to_device(devid, luminance, dlsb, 0, sizeof(float) * region_pixels, CL_TRUE)
              == CL_SUCCESS)
    {
      dt_iop_highlights_global_data_t *test_global_data = (dt_iop_highlights_global_data_t *)gd_void;
      cl_int test_cl_err = CL_SUCCESS;
      for(int mode = 0; mode < 3 && test_cl_err == CL_SUCCESS; mode++)
      {
        const int kernel = test_global_data->kernel_hl_cf_pack_joint;
        dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &dest);
        dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &dvld);
        dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &dlsb);
        dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &packed_device);
        dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_w);
        dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &region_h);
        dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(float), &cf_binv);
        dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &mode);
        const float zero_shift = 0.f; // uncentered, matching the inline CPU replica
        dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(float), &zero_shift);
        dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(float), &zero_shift);
        dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(float), &zero_shift);
        test_cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, work_size);
        if(test_cl_err == CL_SUCCESS)
          test_cl_err = _region_blur_cl(devid, packed_device, moments_device[mode], region_w, region_h, cf_sigma);
      }
      moms_ok = (test_cl_err == CL_SUCCESS);
    }
    else
      moms_ok = 0;
    const float zero_means[3] = { 0.f, 0.f, 0.f }; // isolated stage test runs uncentered
    if(moms_ok
       && dt_opencl_write_buffer_to_device(devid, model_quality, dbsc, 0, sizeof(float) * region_pixels * 4,
                                           CL_TRUE)
              == CL_SUCCESS
       && _cf_joint_stage_cl(devid, gd_void, dest, dvld, dbsc, moment0_device, moment1_device, moment2_device,
                             NULL, zero_means, region_w, region_h, cf_sigma, cf_fmin, c, guide1, guide2)
              == CL_SUCCESS
       && dt_opencl_read_buffer_from_device(devid, estimate_gpu, dest, 0, sizeof(float) * region_pixels * 4,
                                            CL_TRUE)
              == CL_SUCCESS)
    {
      max_diff = 0.f;
      for(size_t i = 0; i < region_pixels; i++)
        if(valid[i * 4 + c] < 0.5f && valid[i * 4 + guide1] >= 0.5f && valid[i * 4 + guide2] >= 0.5f)
          max_diff = fmaxf(max_diff, fabsf(estimate_gpu[i * 4 + c] - estimate[i * 4 + c]));
    }
    dt_opencl_release_mem_object(dest);
    dt_opencl_release_mem_object(dvld);
    dt_opencl_release_mem_object(dbsc);
    dt_opencl_release_mem_object(dlsb);
    dt_opencl_release_mem_object(packed_device);
    dt_opencl_release_mem_object(moment0_device);
    dt_opencl_release_mem_object(moment1_device);
    dt_opencl_release_mem_object(moment2_device);
    dt_pixelpipe_cache_free_align(luminance);
    fprintf(stderr, "[hl cf-joint-cl selftest] %dx%d G-disc r90 max|gpu-cpu|=%.3e\n", region_w, region_h, max_diff);
  }

done_:
  dt_pixelpipe_cache_free_align(estimate);
  dt_pixelpipe_cache_free_align(valid);
  dt_pixelpipe_cache_free_align(model_quality);
  dt_pixelpipe_cache_free_align(estimate_gpu);
  dt_pixelpipe_cache_free_align(input);
  dt_pixelpipe_cache_free_align(moment1);
  dt_pixelpipe_cache_free_align(moment2);
  dt_pixelpipe_cache_free_align(moment3);
  dt_pixelpipe_cache_free_align(coeff_field);
  dt_pixelpipe_cache_free_align(plane);
  dt_pixelpipe_cache_free_align(anchor);
  dt_pixelpipe_cache_free_align(border);
}

void _cf_stage_cl_selftest(const int devid, void *gd_void, const dt_dev_pixelpipe_t *pipe)
{
  static int done = 0;
  if(done || !getenv("HL_CFCL_TEST") || devid < 0) return;
  done = 1;

  const int region_w = 509;
  const int region_h = 371;
  const size_t region_pixels = (size_t)region_w * region_h;
  const float cf_sigma = 24.f;
  const float cf_fmin = 0.05f;
  const int cdeep = 1; // G carries the most clipped pixels by construction

  float *estimate = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *valid = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *model_quality = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *estimate_gpu = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *input = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *moment1 = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *moment2 = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *moment3 = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *coeff_field_green = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *plane = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
  uint8_t *anchor = (uint8_t *)dt_pixelpipe_cache_alloc_align(region_pixels, pipe);
  uint8_t *border = (uint8_t *)dt_pixelpipe_cache_alloc_align(region_pixels, pipe);
  uint8_t *hole = (uint8_t *)dt_pixelpipe_cache_alloc_align(region_pixels, pipe);
  uint8_t *hole_border = (uint8_t *)dt_pixelpipe_cache_alloc_align(region_pixels, pipe);
  if(IS_NULL_PTR(estimate) || IS_NULL_PTR(valid) || IS_NULL_PTR(model_quality) || IS_NULL_PTR(estimate_gpu)
     || IS_NULL_PTR(input) || IS_NULL_PTR(moment1) || IS_NULL_PTR(moment2) || IS_NULL_PTR(moment3)
     || IS_NULL_PTR(coeff_field_green) || IS_NULL_PTR(plane) || IS_NULL_PTR(anchor) || IS_NULL_PTR(border)
     || IS_NULL_PTR(hole) || IS_NULL_PTR(hole_border))
    goto done_;

  for(int y = 0; y < region_h; y++)
    for(int x = 0; x < region_w; x++)
    {
      const size_t i = (size_t)y * region_w + x;
      const float base = 0.4f + 0.3f * sinf(0.011f * x) * cosf(0.014f * y);
      estimate[i * 4 + 0] = 0.9f * base + 0.05f;
      estimate[i * 4 + 1] = 1.2f * base + 0.02f;
      estimate[i * 4 + 2] = 0.7f * base + 0.08f;
      estimate[i * 4 + 3] = 0.f;
      const int delta_x = x - region_w / 2;
      const int delta_y = y - region_h / 2;
      const int gclip = (delta_x * delta_x + delta_y * delta_y < 100 * 100);
      const int rclip = (delta_x * delta_x + delta_y * delta_y < 45 * 45);
      valid[i * 4 + 0] = rclip ? 0.f : 1.f;
      valid[i * 4 + 1] = gclip ? 0.f : 1.f;
      valid[i * 4 + 2] = 1.f;
      valid[i * 4 + 3] = gclip ? 0.f : 1.f;
      for(int k = 0; k < 4; k++) model_quality[i * 4 + k] = 0.f;
      if(gclip) estimate[i * 4 + 1] = 0.58f;
      if(rclip) estimate[i * 4 + 0] = 0.47f;
    }
  memcpy(estimate_gpu, estimate, region_pixels * 4 * sizeof(float));

  double lum_accum = 0.0;
  size_t lum_count = 0;
  for(size_t i = 0; i < region_pixels; i++)
    if(valid[i * 4 + 0] < 0.5f || valid[i * 4 + 1] < 0.5f || valid[i * 4 + 2] < 0.5f)
    {
      lum_accum += estimate[i * 4 + 0] + estimate[i * 4 + 1] + estimate[i * 4 + 2];
      lum_count++;
    }
  const float cf_lref = lum_count ? (float)(lum_accum / (double)lum_count) : 0.f;
  const float cf_binv = (cf_lref > 1e-9f) ? 1.f / (0.35f * cf_lref) : 0.f;
  const int base_downsample = (int)(cf_sigma / 4.f);

  // pre-ladder luminance: production freezes lsb before the first fit
  float *luminance = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
  if(IS_NULL_PTR(luminance)) goto done_;
  for(size_t i = 0; i < region_pixels; i++)
    luminance[i] = estimate[i * 4 + 0] + estimate[i * 4 + 1] + estimate[i * 4 + 2];

  // ---- CPU replica ----
  // joint fit for G (deep): coefficients diffused, evaluation deferred
  {
    const int c = 1;
    const int guide1 = 0;
    const int guide2 = 2;
    for(int mode = 0; mode < 3; mode++)
    {
      for(size_t i = 0; i < region_pixels; i++)
      {
        const float val_r = estimate[i * 4 + 0];
        const float val_g = estimate[i * 4 + 1];
        const float val_b = estimate[i * 4 + 2];
        const float rgb_sum = luminance[i];
        const float bright_weight = (cf_binv > 0.f) ? sqf(fminf(rgb_sum * cf_binv, 1.f)) : 1.f;
        const int all_valid = (valid[i * 4 + 0] >= 0.5f && valid[i * 4 + 1] >= 0.5f && valid[i * 4 + 2] >= 0.5f);
        const float weight = all_valid ? bright_weight : 0.f;
        if(mode == 0)
        {
          input[i * 4 + 0] = weight;
          input[i * 4 + 1] = weight * val_r;
          input[i * 4 + 2] = weight * val_g;
          input[i * 4 + 3] = weight * val_b;
        }
        else if(mode == 1)
        {
          input[i * 4 + 0] = weight * val_r * val_r;
          input[i * 4 + 1] = weight * val_g * val_g;
          input[i * 4 + 2] = weight * val_b * val_b;
          input[i * 4 + 3] = weight * val_r * val_g;
        }
        else
        {
          input[i * 4 + 0] = weight * val_r * val_b;
          input[i * 4 + 1] = weight * val_g * val_b;
          input[i * 4 + 2] = all_valid ? 1.f : 0.f;
          input[i * 4 + 3] = 0.f;
        }
      }
      _region_blur(input, (mode == 0) ? moment1 : (mode == 1) ? moment2 : moment3, region_w, region_h, cf_sigma);
    }
    for(size_t i = 0; i < region_pixels; i++)
    {
      const float norm = fmaxf(moment1[i * 4 + 0], 1e-9f);
      const float inv_det = 1.f / norm;
      const float mean[3]
          = { moment1[i * 4 + 1] * inv_det, moment1[i * 4 + 2] * inv_det, moment1[i * 4 + 3] * inv_det };
      const float second_moment[3]
          = { moment2[i * 4 + 0] * inv_det, moment2[i * 4 + 1] * inv_det, moment2[i * 4 + 2] * inv_det };
      const float cross_rg = moment2[i * 4 + 3] * inv_det;
      const float cross_rb = moment3[i * 4 + 0] * inv_det;
      const float cross_gb = moment3[i * 4 + 1] * inv_det;
      const float mean1 = mean[guide1];
      const float mean2 = mean[guide2];
      const float mean_target = mean[c];
      const float var11 = fmaxf(second_moment[guide1] - mean1 * mean1, 0.f);
      const float var22 = fmaxf(second_moment[guide2] - mean2 * mean2, 0.f);
      const float var12 = cross_rb - mean1 * mean2;         // (g1,g2) = (0,2)
      const float cov_tg1 = cross_rg - mean_target * mean1; // (c,g1) = (1,0)
      const float cov_tg2 = cross_gb - mean_target * mean2; // (c,g2) = (1,2)
      const float var_target = fmaxf(second_moment[c] - mean_target * mean_target, 0.f);
      const float lambda = 1e-3f * 0.5f * (var11 + var22) + 1e-12f;
      const float determinant = fmaxf((var11 + lambda) * (var22 + lambda) - var12 * var12, 1e-18f);
      const float slope_a = ((var22 + lambda) * cov_tg1 - var12 * cov_tg2) / determinant;
      const float slope_b = ((var11 + lambda) * cov_tg2 - var12 * cov_tg1) / determinant;
      const float r_sq = CLAMP((slope_a * cov_tg1 + slope_b * cov_tg2) / (var_target + 1e-12f), 0.f, 1.f);
      coeff_field_green[i * 4 + 0] = slope_a;
      coeff_field_green[i * 4 + 1] = slope_b;
      coeff_field_green[i * 4 + 2] = mean_target - slope_a * mean1 - slope_b * mean2;
      coeff_field_green[i * 4 + 3] = r_sq;
      const int mass_ok = (moment3[i * 4 + 2] > cf_fmin && moment1[i * 4 + 0] > 0.25f * moment3[i * 4 + 2]);
      const int valid_ok = (valid[i * 4 + c] >= 0.5f);
      anchor[i] = (mass_ok && valid_ok && r_sq > 0.25f && fabsf(slope_a) < 64.f && fabsf(slope_b) < 64.f);
      border[i] = (mass_ok && valid_ok);
    }
    for(size_t i = 0; i < region_pixels; i++)
    {
      hole[i] = !anchor[i];
      hole_border[i] = !border[i];
    }
    for(int k = 0; k < 4; k++)
    {
      for(size_t i = 0; i < region_pixels; i++) plane[i] = coeff_field_green[i * 4 + k];
      _cf_harmonic_fill(plane, (k == 3) ? hole_border : hole, region_w, region_h, base_downsample, NULL, pipe);
      for(size_t i = 0; i < region_pixels; i++) coeff_field_green[i * 4 + k] = plane[i];
    }
  }

  // pair fits that fire: {R,B} tc=R (o=0), then {G,B} tc=G (o=0); same math as production
  const int pair_a[2] = { 0, 1 };
  const int pair_b[2] = { 2, 2 };
  for(int pair = 0; pair < 2; pair++)
  {
    const int chan_a = pair_a[pair];
    const int chan_b = pair_b[pair];
    const int target_chan = chan_a;
    const int guide_chan = chan_b;
    const int other_chan = 3 - chan_a - chan_b;
    for(int mode = 0; mode < 2; mode++)
    {
      for(size_t i = 0; i < region_pixels; i++)
      {
        const float value_a = estimate[i * 4 + chan_a];
        const float value_b = estimate[i * 4 + chan_b];
        const float rgb_sum = luminance[i];
        const float bright_weight = (cf_binv > 0.f) ? sqf(fminf(rgb_sum * cf_binv, 1.f)) : 1.f;
        const int pair_valid = (valid[i * 4 + chan_a] >= 0.5f && valid[i * 4 + chan_b] >= 0.5f);
        const float weight = pair_valid ? bright_weight : 0.f;
        if(mode == 0)
        {
          input[i * 4 + 0] = weight;
          input[i * 4 + 1] = weight * value_a;
          input[i * 4 + 2] = weight * value_b;
          input[i * 4 + 3] = weight * value_a * value_a;
        }
        else
        {
          input[i * 4 + 0] = weight * value_b * value_b;
          input[i * 4 + 1] = weight * value_a * value_b;
          input[i * 4 + 2] = pair_valid ? 1.f : 0.f;
          input[i * 4 + 3] = 0.f;
        }
      }
      _region_blur(input, mode ? moment2 : moment1, region_w, region_h, cf_sigma);
    }
    for(size_t i = 0; i < region_pixels; i++)
    {
      const float norm = fmaxf(moment1[i * 4 + 0], 1e-9f);
      const float inv_det = 1.f / norm;
      const float mean_target = moment1[i * 4 + 1] * inv_det; // o = 0: target = a
      const float mean_guide = moment1[i * 4 + 2] * inv_det;
      const float var_guide = fmaxf(moment2[i * 4 + 0] * inv_det - mean_guide * mean_guide, 0.f);
      const float var_t = fmaxf(moment1[i * 4 + 3] * inv_det - mean_target * mean_target, 0.f);
      const float covariance = moment2[i * 4 + 1] * inv_det - mean_target * mean_guide;
      const float slope_a = covariance / (var_guide * (1.f + 1e-3f) + 1e-12f);
      const float r_sq = CLAMP(covariance * covariance / (var_guide * var_t + 1e-18f), 0.f, 1.f);
      moment3[i * 4 + 0] = slope_a;
      moment3[i * 4 + 1] = mean_target - slope_a * mean_guide;
      moment3[i * 4 + 2] = r_sq;
      const int mass_ok = (moment2[i * 4 + 2] > cf_fmin && moment1[i * 4 + 0] > 0.25f * moment2[i * 4 + 2]);
      const int valid_ok = (valid[i * 4 + target_chan] >= 0.5f);
      anchor[i] = (mass_ok && valid_ok && r_sq > 0.25f && fabsf(slope_a) < 64.f);
      border[i] = (mass_ok && valid_ok);
    }
    for(size_t i = 0; i < region_pixels; i++)
    {
      hole[i] = !anchor[i];
      hole_border[i] = !border[i];
    }
    for(int k = 0; k < 3; k++)
    {
      for(size_t i = 0; i < region_pixels; i++) plane[i] = moment3[i * 4 + k];
      _cf_harmonic_fill(plane, (k == 2) ? hole_border : hole, region_w, region_h, base_downsample, NULL, pipe);
      for(size_t i = 0; i < region_pixels; i++) moment3[i * 4 + k] = plane[i];
    }
    for(size_t i = 0; i < region_pixels; i++)
      if(valid[i * 4 + target_chan] < 0.5f && valid[i * 4 + guide_chan] >= 0.5f && valid[i * 4 + other_chan] < 0.5f)
      {
        estimate[i * 4 + target_chan] = moment3[i * 4 + 0] * estimate[i * 4 + guide_chan] + moment3[i * 4 + 1];
        model_quality[i * 4 + target_chan] = CLAMP(moment3[i * 4 + 2], 0.f, 1.f);
      }
  }

  // deferred deep evaluation for G with the depth-split blend
  {
    const int guide1 = 0;
    const int guide2 = 2;
    for(size_t i = 0; i < region_pixels; i++)
    {
      const int multi_clip
          = (valid[i * 4 + cdeep] < 0.5f && (valid[i * 4 + guide1] < 0.5f || valid[i * 4 + guide2] < 0.5f));
      input[i * 4 + 0] = multi_clip ? 1.f : 0.f;
      input[i * 4 + 1] = input[i * 4 + 2] = input[i * 4 + 3] = 0.f;
    }
    _region_blur(input, moment1, region_w, region_h, cf_sigma);
    for(size_t i = 0; i < region_pixels; i++)
    {
      const int anyvalid = (valid[i * 4 + 0] >= 0.5f) || (valid[i * 4 + 1] >= 0.5f) || (valid[i * 4 + 2] >= 0.5f);
      if(valid[i * 4 + cdeep] >= 0.5f || !anyvalid) continue;
      const float joint = coeff_field_green[i * 4 + 0] * estimate[i * 4 + guide1]
                          + coeff_field_green[i * 4 + 1] * estimate[i * 4 + guide2] + coeff_field_green[i * 4 + 2];
      const int has_pair = (valid[i * 4 + guide1] < 0.5f || valid[i * 4 + guide2] < 0.5f);
      const float mass = CLAMP(moment1[i * 4 + 0], 0.f, 1.f);
      const float smooth_t = CLAMP((mass - 0.7f) / 0.25f, 0.f, 1.f);
      const float weight_depth = has_pair ? smooth_t * smooth_t * (3.f - 2.f * smooth_t) : 0.f;
      estimate[i * 4 + cdeep] = weight_depth * estimate[i * 4 + cdeep] + (1.f - weight_depth) * joint;
      model_quality[i * 4 + cdeep] = weight_depth * model_quality[i * 4 + cdeep]
                                     + (1.f - weight_depth) * CLAMP(coeff_field_green[i * 4 + 3], 0.f, 1.f);
    }
  }

  // ---- GPU: the complete CF stage ----
  {
    cl_mem dest = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels * 4);
    cl_mem dvld = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels * 4);
    cl_mem dbsc = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels * 4);
    float max_diff = -1.f;
    float max_bsc_diff = -1.f;
    if(dest && dvld && dbsc
       && dt_opencl_write_buffer_to_device(devid, estimate_gpu, dest, 0, sizeof(float) * region_pixels * 4, CL_TRUE)
              == CL_SUCCESS
       && dt_opencl_write_buffer_to_device(devid, valid, dvld, 0, sizeof(float) * region_pixels * 4, CL_TRUE)
              == CL_SUCCESS
       && dt_opencl_read_buffer_from_device(devid, estimate_gpu, dest, 0, sizeof(float), CL_TRUE) == CL_SUCCESS)
    {
      float *zero = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
      if(zero)
      {
        memset(zero, 0, sizeof(float) * region_pixels * 4);
        dt_opencl_write_buffer_to_device(devid, zero, dbsc, 0, sizeof(float) * region_pixels * 4, CL_TRUE);
        dt_pixelpipe_cache_free_align(zero);
      }
      cl_mem dlsb = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
      const float zero_means[3] = { 0.f, 0.f, 0.f }; // isolated stage test runs uncentered
      if(dlsb
         && dt_opencl_write_buffer_to_device(devid, luminance, dlsb, 0, sizeof(float) * region_pixels, CL_TRUE)
                == CL_SUCCESS
         && _cf_stage_cl(devid, gd_void, dest, dvld, dbsc, dlsb, NULL, zero_means, NULL, region_w, region_h,
                         cf_sigma, cf_fmin, cf_binv, cdeep)
                == CL_SUCCESS
         && dt_opencl_read_buffer_from_device(devid, estimate_gpu, dest, 0, sizeof(float) * region_pixels * 4,
                                              CL_TRUE)
                == CL_SUCCESS)
      {
        max_diff = 0.f;
        for(size_t i = 0; i < region_pixels; i++)
          for(int c = 0; c < 3; c++)
            if(valid[i * 4 + c] < 0.5f)
              max_diff = fmaxf(max_diff, fabsf(estimate_gpu[i * 4 + c] - estimate[i * 4 + c]));

        // the fit-quality plane steers the HF damping and the dome blend downstream: compare it too
        if(dt_opencl_read_buffer_from_device(devid, estimate_gpu, dbsc, 0, sizeof(float) * region_pixels * 4,
                                             CL_TRUE)
           == CL_SUCCESS)
        {
          max_bsc_diff = 0.f;
          size_t arg_index = 0;
          int arg_chan = 0;
          for(size_t i = 0; i < region_pixels; i++)
            for(int c = 0; c < 3; c++)
            {
              const float diff = fabsf(estimate_gpu[i * 4 + c] - model_quality[i * 4 + c]);
              if(diff > max_bsc_diff)
              {
                max_bsc_diff = diff;
                arg_index = i;
                arg_chan = c;
              }
            }
          if(getenv("HL_CFCL_VERBOSE"))
            fprintf(stderr, "[hl cf-full bsc argmax] px=(%zu,%zu) c=%d gpu=%f cpu=%f\n", arg_index % region_w,
                    arg_index / region_w, arg_chan, estimate_gpu[arg_index * 4 + arg_chan],
                    model_quality[arg_index * 4 + arg_chan]);
        }
      }
      dt_opencl_release_mem_object(dlsb);
    }
    dt_opencl_release_mem_object(dest);
    dt_opencl_release_mem_object(dvld);
    dt_opencl_release_mem_object(dbsc);
    fprintf(stderr, "[hl cf-full-cl selftest] %dx%d G-disc r100 + R-core r45 max|gpu-cpu|=%.3e bsc=%.3e\n",
            region_w, region_h, max_diff, max_bsc_diff);
  }

done_:
  dt_pixelpipe_cache_free_align(estimate);
  dt_pixelpipe_cache_free_align(valid);
  dt_pixelpipe_cache_free_align(model_quality);
  dt_pixelpipe_cache_free_align(estimate_gpu);
  dt_pixelpipe_cache_free_align(input);
  dt_pixelpipe_cache_free_align(moment1);
  dt_pixelpipe_cache_free_align(moment2);
  dt_pixelpipe_cache_free_align(moment3);
  dt_pixelpipe_cache_free_align(coeff_field_green);
  dt_pixelpipe_cache_free_align(plane);
  dt_pixelpipe_cache_free_align(luminance);
  dt_pixelpipe_cache_free_align(anchor);
  dt_pixelpipe_cache_free_align(border);
  dt_pixelpipe_cache_free_align(hole);
  dt_pixelpipe_cache_free_align(hole_border);
}

void _hf_stage_cl_selftest(const int devid, void *gd_void, const dt_dev_pixelpipe_t *pipe)
{
  static int done = 0;
  if(done || !getenv("HL_HFCL_TEST") || devid < 0) return;
  done = 1;

  const int region_w = 509;
  const int region_h = 371;
  const size_t region_pixels = (size_t)region_w * region_h;
  const float cf_sigma = 24.f;
  const float cf_fmin = 0.05f;
  const float blur_sigma = fmaxf(cf_sigma / 4.f, 2.f);
  const int base_downsample = (int)(cf_sigma / 4.f);

  float *estimate = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *valid = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *model_quality = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *estimate_gpu = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *input = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *lowpass = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *moment1 = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *moment2 = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *moment3 = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *gain_ab = dt_pixelpipe_cache_alloc_align_float(region_pixels * 2, pipe);
  float *energy = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *plane = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
  uint8_t *anchor = (uint8_t *)dt_pixelpipe_cache_alloc_align(region_pixels, pipe);
  uint8_t *hole = (uint8_t *)dt_pixelpipe_cache_alloc_align(region_pixels, pipe);
  if(IS_NULL_PTR(estimate) || IS_NULL_PTR(valid) || IS_NULL_PTR(model_quality) || IS_NULL_PTR(estimate_gpu)
     || IS_NULL_PTR(input) || IS_NULL_PTR(lowpass) || IS_NULL_PTR(moment1) || IS_NULL_PTR(moment2)
     || IS_NULL_PTR(moment3) || IS_NULL_PTR(gain_ab) || IS_NULL_PTR(energy) || IS_NULL_PTR(plane)
     || IS_NULL_PTR(anchor) || IS_NULL_PTR(hole))
    goto done_;

  for(int y = 0; y < region_h; y++)
    for(int x = 0; x < region_w; x++)
    {
      const size_t i = (size_t)y * region_w + x;
      const float base = 0.4f + 0.3f * sinf(0.011f * x) * cosf(0.014f * y);
      const float texture = 0.05f * sinf(0.9f * x) * sinf(0.75f * y);
      estimate[i * 4 + 0] = 0.9f * base + 0.05f + texture;
      estimate[i * 4 + 1] = 1.2f * base + 0.02f + 0.8f * texture;
      estimate[i * 4 + 2] = 0.7f * base + 0.08f + 1.1f * texture;
      estimate[i * 4 + 3] = 0.f;
      const int delta_x = x - region_w / 2;
      const int delta_y = y - region_h / 2;
      const int gclip = (delta_x * delta_x + delta_y * delta_y < 100 * 100);
      const int rclip = (delta_x * delta_x + delta_y * delta_y < 45 * 45);
      valid[i * 4 + 0] = rclip ? 0.f : 1.f;
      valid[i * 4 + 1] = gclip ? 0.f : 1.f;
      valid[i * 4 + 2] = 1.f;
      valid[i * 4 + 3] = gclip ? 0.f : 1.f;
      for(int k = 0; k < 4; k++) model_quality[i * 4 + k] = gclip ? 0.65f : 0.f;
    }
  memcpy(estimate_gpu, estimate, region_pixels * 4 * sizeof(float));

  double lum_accum = 0.0;
  size_t lum_count = 0;
  for(size_t i = 0; i < region_pixels; i++)
    if(valid[i * 4 + 0] < 0.5f || valid[i * 4 + 1] < 0.5f || valid[i * 4 + 2] < 0.5f)
    {
      lum_accum += estimate[i * 4 + 0] + estimate[i * 4 + 1] + estimate[i * 4 + 2];
      lum_count++;
    }
  const float cf_lref = lum_count ? (float)(lum_accum / (double)lum_count) : 0.f;
  const float cf_binv = (cf_lref > 1e-9f) ? 1.f / (0.35f * cf_lref) : 0.f;

  // ---- CPU replica ----
  memcpy(input, estimate, region_pixels * 4 * sizeof(float));
  _region_blur(input, lowpass, region_w, region_h, blur_sigma);
  for(int mode = 0; mode < 3; mode++)
  {
    for(size_t i = 0; i < region_pixels; i++)
    {
      const float detail_r = estimate[i * 4 + 0] - lowpass[i * 4 + 0];
      const float detail_g = estimate[i * 4 + 1] - lowpass[i * 4 + 1];
      const float detail_b = estimate[i * 4 + 2] - lowpass[i * 4 + 2];
      const float rgb_sum = estimate[i * 4 + 0] + estimate[i * 4 + 1] + estimate[i * 4 + 2];
      const float bright_weight = (cf_binv > 0.f) ? sqf(fminf(rgb_sum * cf_binv, 1.f)) : 1.f;
      const int all_valid = (valid[i * 4 + 0] >= 0.5f && valid[i * 4 + 1] >= 0.5f && valid[i * 4 + 2] >= 0.5f);
      const float weight = all_valid ? bright_weight : 0.f;
      if(mode == 0)
      {
        input[i * 4 + 0] = weight;
        input[i * 4 + 1] = weight * detail_r;
        input[i * 4 + 2] = weight * detail_g;
        input[i * 4 + 3] = weight * detail_b;
      }
      else if(mode == 1)
      {
        input[i * 4 + 0] = weight * detail_r * detail_r;
        input[i * 4 + 1] = weight * detail_g * detail_g;
        input[i * 4 + 2] = weight * detail_b * detail_b;
        input[i * 4 + 3] = weight * detail_r * detail_g;
      }
      else
      {
        input[i * 4 + 0] = weight * detail_r * detail_b;
        input[i * 4 + 1] = weight * detail_g * detail_b;
        input[i * 4 + 2] = all_valid ? 1.f : 0.f;
        input[i * 4 + 3] = 0.f;
      }
    }
    _region_blur(input, (mode == 0) ? moment1 : (mode == 1) ? moment2 : moment3, region_w, region_h, cf_sigma);
  }
  for(int c = 0; c < 3; c++)
  {
    const int guide1 = (c == 0) ? 1 : 0;
    const int guide2 = (c == 2) ? 1 : 2;
    for(size_t i = 0; i < region_pixels; i++)
    {
      const float norm = fmaxf(moment1[i * 4 + 0], 1e-9f);
      const float inv_det = 1.f / norm;
      const float mean[3]
          = { moment1[i * 4 + 1] * inv_det, moment1[i * 4 + 2] * inv_det, moment1[i * 4 + 3] * inv_det };
      const float second_moment[3]
          = { moment2[i * 4 + 0] * inv_det, moment2[i * 4 + 1] * inv_det, moment2[i * 4 + 2] * inv_det };
      const float cross_rg = moment2[i * 4 + 3] * inv_det;
      const float cross_rb = moment3[i * 4 + 0] * inv_det;
      const float cross_gb = moment3[i * 4 + 1] * inv_det;
#define OFF3(chan_a, chan_b)                                                                                      \
  (((chan_a) + (chan_b)) == 1 ? cross_rg : (((chan_a) + (chan_b)) == 2 ? cross_rb : cross_gb))
      const float mean1 = mean[guide1];
      const float mean2 = mean[guide2];
      const float mean_target = mean[c];
      const float var11 = fmaxf(second_moment[guide1] - mean1 * mean1, 0.f);
      const float var22 = fmaxf(second_moment[guide2] - mean2 * mean2, 0.f);
      const float var12 = OFF3(guide1, guide2) - mean1 * mean2;
      const float cov_tg1 = OFF3(c, guide1) - mean_target * mean1;
      const float cov_tg2 = OFF3(c, guide2) - mean_target * mean2;
      const float var_target = fmaxf(second_moment[c] - mean_target * mean_target, 0.f);
#undef OFF3
      const float lambda = 1e-3f * 0.5f * (var11 + var22) + 1e-12f;
      const float determinant = fmaxf((var11 + lambda) * (var22 + lambda) - var12 * var12, 1e-18f);
      const float slope_a = ((var22 + lambda) * cov_tg1 - var12 * cov_tg2) / determinant;
      const float slope_b = ((var11 + lambda) * cov_tg2 - var12 * cov_tg1) / determinant;
      const float r_sq = CLAMP((slope_a * cov_tg1 + slope_b * cov_tg2) / (var_target + 1e-12f), 0.f, 1.f);
      gain_ab[i * 2 + 0] = slope_a * r_sq;
      gain_ab[i * 2 + 1] = slope_b * r_sq;
      const int mass_ok = (moment3[i * 4 + 2] > cf_fmin && moment1[i * 4 + 0] > 0.25f * moment3[i * 4 + 2]);
      anchor[i] = (mass_ok && valid[i * 4 + c] >= 0.5f && fabsf(gain_ab[i * 2 + 0]) < 64.f
                   && fabsf(gain_ab[i * 2 + 1]) < 64.f);
    }
    for(size_t i = 0; i < region_pixels; i++) hole[i] = !anchor[i];
    for(int k = 0; k < 2; k++)
    {
      for(size_t i = 0; i < region_pixels; i++) plane[i] = gain_ab[i * 2 + k];
      _cf_harmonic_fill(plane, hole, region_w, region_h, base_downsample, NULL, pipe);
      for(size_t i = 0; i < region_pixels; i++) gain_ab[i * 2 + k] = plane[i];
    }
    for(size_t i = 0; i < region_pixels; i++)
    {
      const float high_guide = gain_ab[i * 2 + 0] * (estimate[i * 4 + guide1] - lowpass[i * 4 + guide1])
                               + gain_ab[i * 2 + 1] * (estimate[i * 4 + guide2] - lowpass[i * 4 + guide2]);
      const float high_damped
          = CLAMP(model_quality[i * 4 + c], 0.f, 1.f) * (estimate[i * 4 + c] - lowpass[i * 4 + c]);
      input[i * 4 + 0] = fabsf(high_guide);
      input[i * 4 + 1] = fabsf(high_damped);
      input[i * 4 + 2] = 0.f;
      input[i * 4 + 3] = 0.f;
    }
    _region_blur(input, energy, region_w, region_h, blur_sigma);
    for(size_t i = 0; i < region_pixels; i++)
      if(valid[i * 4 + c] < 0.5f && valid[i * 4 + guide1] >= 0.5f && valid[i * 4 + guide2] >= 0.5f)
      {
        const float high_guide = gain_ab[i * 2 + 0] * (estimate[i * 4 + guide1] - lowpass[i * 4 + guide1])
                                 + gain_ab[i * 2 + 1] * (estimate[i * 4 + guide2] - lowpass[i * 4 + guide2]);
        const float high_damped
            = CLAMP(model_quality[i * 4 + c], 0.f, 1.f) * (estimate[i * 4 + c] - lowpass[i * 4 + c]);
        const float weight_energy
            = energy[i * 4 + 1] * energy[i * 4 + 1]
              / fmaxf(energy[i * 4 + 1] * energy[i * 4 + 1] + energy[i * 4 + 0] * energy[i * 4 + 0], 1e-18f);
        estimate[i * 4 + c]
            = lowpass[i * 4 + c] + weight_energy * high_guide + (1.f - weight_energy) * high_damped;
      }
  }
  for(size_t i = 0; i < region_pixels; i++)
  {
    const int n_valid = (valid[i * 4 + 0] >= 0.5f) + (valid[i * 4 + 1] >= 0.5f) + (valid[i * 4 + 2] >= 0.5f);
    if(n_valid != 1) continue;
    for(int c = 0; c < 3; c++)
      if(valid[i * 4 + c] < 0.5f)
      {
        const float weight_hf = CLAMP(model_quality[i * 4 + c], 0.f, 1.f);
        estimate[i * 4 + c] = lowpass[i * 4 + c] + weight_hf * (estimate[i * 4 + c] - lowpass[i * 4 + c]);
      }
  }

  // ---- GPU ----
  {
    cl_mem dest = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels * 4);
    cl_mem dvld = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels * 4);
    cl_mem dbsc = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels * 4);
    cl_mem dlsb = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
    float *luminance = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
    float max_diff = -1.f;
    if(luminance)
      for(size_t i = 0; i < region_pixels; i++)
        luminance[i] = estimate_gpu[i * 4 + 0] + estimate_gpu[i * 4 + 1] + estimate_gpu[i * 4 + 2];
    if(dest && dvld && dbsc && dlsb && luminance
       && dt_opencl_write_buffer_to_device(devid, estimate_gpu, dest, 0, sizeof(float) * region_pixels * 4, CL_TRUE)
              == CL_SUCCESS
       && dt_opencl_write_buffer_to_device(devid, valid, dvld, 0, sizeof(float) * region_pixels * 4, CL_TRUE)
              == CL_SUCCESS
       && dt_opencl_write_buffer_to_device(devid, model_quality, dbsc, 0, sizeof(float) * region_pixels * 4,
                                           CL_TRUE)
              == CL_SUCCESS
       && dt_opencl_write_buffer_to_device(devid, luminance, dlsb, 0, sizeof(float) * region_pixels, CL_TRUE)
              == CL_SUCCESS
       && _hf_stage_cl(devid, gd_void, dest, dvld, dbsc, dlsb, NULL, NULL, region_w, region_h, cf_sigma, cf_fmin,
                       cf_binv)
              == CL_SUCCESS
       && dt_opencl_read_buffer_from_device(devid, estimate_gpu, dest, 0, sizeof(float) * region_pixels * 4,
                                            CL_TRUE)
              == CL_SUCCESS)
    {
      max_diff = 0.f;
      for(size_t i = 0; i < region_pixels; i++)
        for(int c = 0; c < 3; c++)
          if(valid[i * 4 + c] < 0.5f)
            max_diff = fmaxf(max_diff, fabsf(estimate_gpu[i * 4 + c] - estimate[i * 4 + c]));
    }
    dt_opencl_release_mem_object(dest);
    dt_opencl_release_mem_object(dvld);
    dt_opencl_release_mem_object(dbsc);
    dt_opencl_release_mem_object(dlsb);
    dt_pixelpipe_cache_free_align(luminance);
    fprintf(stderr, "[hl hf-cl selftest] %dx%d two-disc textured max|gpu-cpu|=%.3e\n", region_w, region_h,
            max_diff);
  }

done_:
  dt_pixelpipe_cache_free_align(estimate);
  dt_pixelpipe_cache_free_align(valid);
  dt_pixelpipe_cache_free_align(model_quality);
  dt_pixelpipe_cache_free_align(estimate_gpu);
  dt_pixelpipe_cache_free_align(input);
  dt_pixelpipe_cache_free_align(lowpass);
  dt_pixelpipe_cache_free_align(moment1);
  dt_pixelpipe_cache_free_align(moment2);
  dt_pixelpipe_cache_free_align(moment3);
  dt_pixelpipe_cache_free_align(gain_ab);
  dt_pixelpipe_cache_free_align(energy);
  dt_pixelpipe_cache_free_align(plane);
  dt_pixelpipe_cache_free_align(anchor);
  dt_pixelpipe_cache_free_align(hole);
}

void _selfdome_stage_cl_selftest(const int devid, void *gd_void, const dt_dev_pixelpipe_t *pipe)
{
  static int done = 0;
  if(done || !getenv("HL_DOMECL_TEST") || devid < 0) return;
  done = 1;

  // replay mode: HL_DOMECL_TEST=1 with HL_REG_DUMP=<file path> pointing at a previous dump ->
  // run the dumped real-region dome through both implementations and compare
  {
    const char *reg_dump_path = getenv("HL_REG_DUMP");
    FILE *dump_file = (reg_dump_path && reg_dump_path[0]) ? g_fopen(reg_dump_path, "rb") : NULL;
    if(dump_file)
    {
      int dump_w;
      int dump_h;
      int dump_downsample;
      if(fread(&dump_w, sizeof(int), 1, dump_file) == 1 && fread(&dump_h, sizeof(int), 1, dump_file) == 1
         && fread(&dump_downsample, sizeof(int), 1, dump_file) == 1)
      {
        const size_t dump_pixels = (size_t)dump_w * dump_h;
        float *dump_cpu = dt_pixelpipe_cache_alloc_align_float(dump_pixels, pipe);
        float *dump_gpu = dt_pixelpipe_cache_alloc_align_float(dump_pixels, pipe);
        uint8_t *dump_hole = (uint8_t *)dt_pixelpipe_cache_alloc_align(dump_pixels, pipe);
        if(dump_cpu && dump_gpu && dump_hole
           && fread(dump_cpu, sizeof(float), dump_pixels, dump_file) == dump_pixels
           && fread(dump_hole, 1, dump_pixels, dump_file) == dump_pixels)
        {
          memcpy(dump_gpu, dump_cpu, dump_pixels * sizeof(float));
          _biharmonic_dome(dump_cpu, dump_hole, dump_w, dump_h, dump_downsample, pipe);
          cl_mem val_device = dt_opencl_alloc_device_buffer(devid, sizeof(float) * dump_pixels);
          cl_mem hole_device = dt_opencl_alloc_device_buffer(devid, dump_pixels);
          float max_diff = -1.f;
          size_t n_nan = 0;
          if(val_device && hole_device
             && dt_opencl_write_buffer_to_device(devid, dump_gpu, val_device, 0, sizeof(float) * dump_pixels,
                                                 CL_TRUE)
                    == CL_SUCCESS
             && dt_opencl_write_buffer_to_device(devid, dump_hole, hole_device, 0, dump_pixels, CL_TRUE)
                    == CL_SUCCESS
             && _biharmonic_dome_cl(devid, gd_void, val_device, hole_device, dump_w, dump_h, dump_downsample, pipe)
                    == CL_SUCCESS
             && dt_opencl_read_buffer_from_device(devid, dump_gpu, val_device, 0, sizeof(float) * dump_pixels,
                                                  CL_TRUE)
                    == CL_SUCCESS)
          {
            max_diff = 0.f;
            for(size_t i = 0; i < dump_pixels; i++)
            {
              if(isnan(dump_gpu[i]))
                n_nan++;
              else
                max_diff = fmaxf(max_diff, fabsf(dump_gpu[i] - dump_cpu[i]));
            }
          }
          fprintf(stderr, "[hl dome-cl REPLAY] %dx%d ds=%d nan=%zu max|gpu-cpu|=%.3e\n", dump_w, dump_h,
                  dump_downsample, n_nan, max_diff);
          dt_opencl_release_mem_object(val_device);
          dt_opencl_release_mem_object(hole_device);
        }
        dt_pixelpipe_cache_free_align(dump_cpu);
        dt_pixelpipe_cache_free_align(dump_gpu);
        dt_pixelpipe_cache_free_align(dump_hole);
      }
      fclose(dump_file);
    }
  }

  const int region_w = 509;
  const int region_h = 371;
  const size_t region_pixels = (size_t)region_w * region_h;
  const float cf_sigma = 24.f;
  const float reg_radius = 100.f;
  const float epsilon = 1e-6f;

  float *estimate = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *valid = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *model_quality = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *clip0 = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *depth = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
  float *estimate_gpu = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *luminance = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
  float *dome_lum = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
  float *ratio = dt_pixelpipe_cache_alloc_align_float(region_pixels * 3, pipe);
  uint8_t *hole = (uint8_t *)dt_pixelpipe_cache_alloc_align(region_pixels, pipe);
  if(IS_NULL_PTR(estimate) || IS_NULL_PTR(valid) || IS_NULL_PTR(model_quality) || IS_NULL_PTR(clip0)
     || IS_NULL_PTR(depth) || IS_NULL_PTR(estimate_gpu) || IS_NULL_PTR(luminance) || IS_NULL_PTR(dome_lum)
     || IS_NULL_PTR(ratio) || IS_NULL_PTR(hole))
    goto done_;

  size_t n_hole_union = 0;
  for(int y = 0; y < region_h; y++)
    for(int x = 0; x < region_w; x++)
    {
      const size_t i = (size_t)y * region_w + x;
      const float base = 0.4f + 0.3f * sinf(0.011f * x) * cosf(0.014f * y);
      estimate[i * 4 + 0] = 0.9f * base + 0.05f;
      estimate[i * 4 + 1] = 1.2f * base + 0.02f;
      estimate[i * 4 + 2] = 0.7f * base + 0.08f;
      estimate[i * 4 + 3] = 0.f;
      const int delta_x = x - region_w / 2;
      const int delta_y = y - region_h / 2;
      const float dist = sqrtf((float)(delta_x * delta_x + delta_y * delta_y));
      const int gclip = (dist < 100.f);
      const int rclip = (dist < 45.f);
      valid[i * 4 + 0] = rclip ? 0.f : 1.f;
      valid[i * 4 + 1] = gclip ? 0.f : 1.f;
      valid[i * 4 + 2] = 1.f;
      valid[i * 4 + 3] = gclip ? 0.f : 1.f;
      for(int k = 0; k < 4; k++)
      {
        model_quality[i * 4 + k] = gclip ? 0.55f : 0.f;
        clip0[i * 4 + k] = 0.5f;
      }
      depth[i] = fmaxf(100.f - dist, 0.f);
      if(gclip) n_hole_union++;
    }
  memcpy(estimate_gpu, estimate, region_pixels * 4 * sizeof(float));

  const int downsample_shared = MAX(1, (int)ceilf(sqrtf((float)n_hole_union / (float)DT_HL_DOME_NMAX_SPARSE)));

  // ---- CPU replica: production order ----
  for(size_t i = 0; i < region_pixels; i++)
    for(int c = 0; c < 3; c++)
      if(valid[i * 4 + c] < 0.5f)
      {
        const float clip_floor = clip0[i * 4 + c];
        const float diff = estimate[i * 4 + c] - clip_floor;
        const float soft_width = 0.02f * fmaxf(clip_floor, 1e-6f);
        estimate[i * 4 + c] = clip_floor + 0.5f * (diff + sqrtf(diff * diff + soft_width * soft_width));
      }
  for(size_t i = 0; i < region_pixels; i++)
  {
    luminance[i] = estimate[i * 4 + 0] + estimate[i * 4 + 1] + estimate[i * 4 + 2];
    hole[i] = (valid[i * 4 + 0] < 0.5f || valid[i * 4 + 1] < 0.5f || valid[i * 4 + 2] < 0.5f);
    dome_lum[i] = luminance[i];
  }
  _biharmonic_dome(dome_lum, hole, region_w, region_h, downsample_shared, pipe);
  {
    const int cf_base = (int)(CLAMP(reg_radius / 6.f, 8.f, 64.f) / 4.f);
    float *plane = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
    if(plane)
    {
      for(int c = 0; c < 3; c++)
      {
        for(size_t i = 0; i < region_pixels; i++) plane[i] = estimate[i * 4 + c] / fmaxf(luminance[i], epsilon);
        _cf_harmonic_fill(plane, hole, region_w, region_h, cf_base, NULL, pipe);
        for(size_t i = 0; i < region_pixels; i++) ratio[i * 3 + c] = fmaxf(plane[i], 0.f);
      }
      dt_pixelpipe_cache_free_align(plane);
    }
  }
  for(size_t i = 0; i < region_pixels; i++)
  {
    if(!hole[i]) continue;
    const float chroma_sum = fmaxf(ratio[i * 3 + 0] + ratio[i * 3 + 1] + ratio[i * 3 + 2], epsilon);
    const int anyvalid = (valid[i * 4 + 0] >= 0.5f) || (valid[i * 4 + 1] >= 0.5f) || (valid[i * 4 + 2] >= 0.5f);
    for(int c = 0; c < 3; c++)
      if(valid[i * 4 + c] < 0.5f)
      {
        const float quality = CLAMP((model_quality[i * 4 + c] - 0.4f) / 0.45f, 0.f, 1.f);
        const float weight_r2 = quality * quality * (3.f - 2.f * quality);
        const float depth_t = depth[i] / (1.5f * cf_sigma);
        const float depth_gauss = expf(-depth_t * depth_t);
        const float weight_sqrt = sqrtf(CLAMP(1.f - (1.f - weight_r2) * depth_gauss, 0.f, 1.f));
        const float weight = weight_sqrt * weight_sqrt;
        const float dome = dome_lum[i] * (ratio[i * 3 + c] / chroma_sum);
        estimate[i * 4 + c] = anyvalid ? (weight * estimate[i * 4 + c] + (1.f - weight) * dome) : dome;
      }
  }
  for(size_t i = 0; i < region_pixels; i++)
    for(int c = 0; c < 3; c++)
      if(valid[i * 4 + c] < 0.5f) estimate[i * 4 + c] = fmaxf(estimate[i * 4 + c], clip0[i * 4 + c]);

  // ---- GPU ----
  {
    cl_mem dest = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels * 4);
    cl_mem dvld = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels * 4);
    cl_mem dbsc = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels * 4);
    cl_mem dclip = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels * 4);
    cl_mem ddep = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
    float max_diff = -1.f;
    if(dest && dvld && dbsc && dclip && ddep
       && dt_opencl_write_buffer_to_device(devid, estimate_gpu, dest, 0, sizeof(float) * region_pixels * 4, CL_TRUE)
              == CL_SUCCESS
       && dt_opencl_write_buffer_to_device(devid, valid, dvld, 0, sizeof(float) * region_pixels * 4, CL_TRUE)
              == CL_SUCCESS
       && dt_opencl_write_buffer_to_device(devid, model_quality, dbsc, 0, sizeof(float) * region_pixels * 4,
                                           CL_TRUE)
              == CL_SUCCESS
       && dt_opencl_write_buffer_to_device(devid, clip0, dclip, 0, sizeof(float) * region_pixels * 4, CL_TRUE)
              == CL_SUCCESS
       && dt_opencl_write_buffer_to_device(devid, depth, ddep, 0, sizeof(float) * region_pixels, CL_TRUE)
              == CL_SUCCESS
       && _selfdome_stage_cl(devid, gd_void, dest, dvld, dbsc, dclip, ddep, region_w, region_h, cf_sigma,
                             reg_radius, downsample_shared, 0.f /* gate 0: replicas are ungated */, pipe)
              == CL_SUCCESS
       && dt_opencl_read_buffer_from_device(devid, estimate_gpu, dest, 0, sizeof(float) * region_pixels * 4,
                                            CL_TRUE)
              == CL_SUCCESS)
    {
      max_diff = 0.f;
      for(size_t i = 0; i < region_pixels; i++)
        for(int c = 0; c < 3; c++)
          if(valid[i * 4 + c] < 0.5f)
            max_diff = fmaxf(max_diff, fabsf(estimate_gpu[i * 4 + c] - estimate[i * 4 + c]));
    }
    dt_opencl_release_mem_object(dest);
    dt_opencl_release_mem_object(dvld);
    dt_opencl_release_mem_object(dbsc);
    dt_opencl_release_mem_object(dclip);
    dt_opencl_release_mem_object(ddep);
    fprintf(stderr, "[hl dome-cl selftest] %dx%d two-disc ds=%d max|gpu-cpu|=%.3e\n", region_w, region_h,
            downsample_shared, max_diff);
  }

done_:
  dt_pixelpipe_cache_free_align(estimate);
  dt_pixelpipe_cache_free_align(valid);
  dt_pixelpipe_cache_free_align(model_quality);
  dt_pixelpipe_cache_free_align(clip0);
  dt_pixelpipe_cache_free_align(depth);
  dt_pixelpipe_cache_free_align(estimate_gpu);
  dt_pixelpipe_cache_free_align(luminance);
  dt_pixelpipe_cache_free_align(dome_lum);
  dt_pixelpipe_cache_free_align(ratio);
  dt_pixelpipe_cache_free_align(hole);
}

void _joint_core_stage_cl_selftest(const int devid, void *gd_void, const dt_dev_pixelpipe_t *pipe)
{
  static int done = 0;
  if(done || !getenv("HL_CORECL_TEST") || devid < 0) return;
  done = 1;

  const int region_w = 509;
  const int region_h = 371;
  const size_t region_pixels = (size_t)region_w * region_h;
  const float solid_color = 0.4f;
  const float reg_radius = 100.f;
  const float epsilon = 1e-6f;

  float *estimate = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *valid = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *clip0 = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *estimate_gpu = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *luminance = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
  float *dome_lum = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
  float *chroma = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *chroma_work = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
  float *target_buf = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
  float *diffusion_buf = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
  float *scratch1 = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
  float *scratch2 = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
  float *scratch_sc = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
  float *weight_feather = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
  uint8_t *hole = (uint8_t *)dt_pixelpipe_cache_alloc_align(region_pixels, pipe);
  if(IS_NULL_PTR(estimate) || IS_NULL_PTR(valid) || IS_NULL_PTR(clip0) || IS_NULL_PTR(estimate_gpu)
     || IS_NULL_PTR(luminance) || IS_NULL_PTR(dome_lum) || IS_NULL_PTR(chroma) || IS_NULL_PTR(chroma_work)
     || IS_NULL_PTR(target_buf) || IS_NULL_PTR(diffusion_buf) || IS_NULL_PTR(scratch1) || IS_NULL_PTR(scratch2)
     || IS_NULL_PTR(scratch_sc) || IS_NULL_PTR(weight_feather) || IS_NULL_PTR(hole))
    goto done_;

  for(int y = 0; y < region_h; y++)
    for(int x = 0; x < region_w; x++)
    {
      const size_t i = (size_t)y * region_w + x;
      const float base = 0.4f + 0.3f * sinf(0.011f * x) * cosf(0.014f * y);
      estimate[i * 4 + 0] = 0.9f * base + 0.05f;
      estimate[i * 4 + 1] = 1.2f * base + 0.02f;
      estimate[i * 4 + 2] = 0.7f * base + 0.08f;
      estimate[i * 4 + 3] = 0.f;
      const int delta_x = x - region_w / 2;
      const int delta_y = y - (region_h - 40);
      const float dist = sqrtf((float)(delta_x * delta_x + delta_y * delta_y));
      const int allclip = (dist < 95.f); // >16k px core even border-cut: forces the CG path
      const int gclip = (dist < 145.f);
      valid[i * 4 + 0] = allclip ? 0.f : 1.f;
      valid[i * 4 + 1] = gclip ? 0.f : 1.f;
      valid[i * 4 + 2] = allclip ? 0.f : 1.f;
      valid[i * 4 + 3] = gclip ? 0.f : 1.f;
      for(int k = 0; k < 4; k++) clip0[i * 4 + k] = 0.5f;
      if(allclip)
        for(int c = 0; c < 3; c++) estimate[i * 4 + c] = 0.5f;
      else if(gclip)
        estimate[i * 4 + 1] = 0.55f + 0.002f * (100.f - dist); // annulus: reconstructed, bright
    }
  memcpy(estimate_gpu, estimate, region_pixels * 4 * sizeof(float));

  // ---- CPU replica: production joint-core block ----
  {
    for(size_t i = 0; i < region_pixels; i++)
    {
      hole[i] = (valid[i * 4 + 0] < 0.5f && valid[i * 4 + 1] < 0.5f && valid[i * 4 + 2] < 0.5f);
      luminance[i] = estimate[i * 4 + 0] + estimate[i * 4 + 1] + estimate[i * 4 + 2];
      chroma_work[i] = luminance[i];
    }
    _biharmonic_dome(chroma_work, hole, region_w, region_h, 0, pipe);
    memcpy(dome_lum, chroma_work, region_pixels * sizeof(float));
    for(size_t i = 0; i < region_pixels; i++)
      if(hole[i]) dome_lum[i] = fmaxf(dome_lum[i], clip0[i * 4 + 0] + clip0[i * 4 + 1] + clip0[i * 4 + 2]);

    dt_aligned_pixel_t chroma_mean = { 0.f, 0.f, 0.f, 0.f };
    double chroma_accum[3] = { 0.0, 0.0, 0.0 };
    double chroma_count = 0.0;
    for(size_t i = 0; i < region_pixels; i++)
    {
      if(!(valid[i * 4 + 0] >= 0.5f && valid[i * 4 + 1] >= 0.5f && valid[i * 4 + 2] >= 0.5f)) continue;
      const float inv_lum = 1.f / fmaxf(luminance[i], epsilon);
      chroma_accum[0] += (double)(estimate[i * 4 + 0] * inv_lum);
      chroma_accum[1] += (double)(estimate[i * 4 + 1] * inv_lum);
      chroma_accum[2] += (double)(estimate[i * 4 + 2] * inv_lum);
      chroma_count += 1.0;
    }
    if(chroma_count > 0.0)
      for(int c = 0; c < 3; c++) chroma_mean[c] = (float)(chroma_accum[c] / chroma_count);

    const float reaction = solid_color * solid_color * 4.f;
    for(size_t i = 0; i < region_pixels; i++) diffusion_buf[i] = reaction;

    int *sp_pgrid = NULL;
    int sp_n_unknowns = 0;
    _sp_chol_t *sp_factor = _sp_pde_factor(hole, (reaction > 0.f) ? diffusion_buf : NULL, 1, 1.f, region_w,
                                           region_h, &sp_pgrid, &sp_n_unknowns, pipe);
    double *sp_rhs
        = sp_factor ? (double *)dt_pixelpipe_cache_alloc_align(sizeof(double) * sp_n_unknowns, pipe) : NULL;
    float *cg_residual = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
    float *cg_search = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
    float *cg_matvec = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
    const int max_iter = CLAMP(2 * 150, 200, 2000);
    for(int c = 0; c < 3; c++)
    {
      for(size_t i = 0; i < region_pixels; i++)
      {
        chroma_work[i] = hole[i] ? chroma_mean[c] : (estimate[i * 4 + c] / fmaxf(luminance[i], epsilon));
        target_buf[i] = chroma_mean[c];
      }
      if(sp_factor && sp_rhs)
        _sp_pde_solve(sp_factor, sp_pgrid, chroma_work, hole, (reaction > 0.f) ? diffusion_buf : NULL,
                      (reaction > 0.f) ? target_buf : NULL, NULL, 1, 1.f, region_w, region_h, sp_rhs, scratch1,
                      scratch2, scratch_sc);
      else if(cg_residual && cg_search && cg_matvec)
        _region_pde_solve(chroma_work, hole, (reaction > 0.f) ? diffusion_buf : NULL,
                          (reaction > 0.f) ? target_buf : NULL, NULL, 1, 1.f, region_w, region_h, cg_residual,
                          cg_search, cg_matvec, scratch1, scratch2, max_iter);
      for(size_t i = 0; i < region_pixels; i++) chroma[i * 4 + c] = fmaxf(chroma_work[i], 0.f);
    }
    fprintf(stderr, "[hl core-cl selftest] CPU path: %s\n", (sp_factor && sp_rhs) ? "sparse" : "CG");
    _sp_chol_free(sp_factor);
    dt_pixelpipe_cache_free_align(sp_pgrid);
    dt_pixelpipe_cache_free_align(sp_rhs);
    dt_pixelpipe_cache_free_align(cg_residual);
    dt_pixelpipe_cache_free_align(cg_search);
    dt_pixelpipe_cache_free_align(cg_matvec);

    for(size_t i = 0; i < region_pixels; i++) chroma_work[i] = hole[i] ? 1.f : 0.f;
    _knee_blur(chroma_work, weight_feather, region_w, region_h,
               fmaxf(4.f, CLAMP(reg_radius / 6.f, 8.f, 64.f) / 4.f));

    for(size_t i = 0; i < region_pixels; i++)
    {
      const float feather = CLAMP(weight_feather[i], 0.f, 1.f);
      const float chroma_sum = fmaxf(chroma[i * 4 + 0] + chroma[i * 4 + 1] + chroma[i * 4 + 2], epsilon);
      if(hole[i])
      {
        for(int c = 0; c < 3; c++) estimate[i * 4 + c] = dome_lum[i] * (chroma[i * 4 + c] / chroma_sum);
      }
      else if(feather > 1e-4f)
      {
        for(int c = 0; c < 3; c++)
          if(valid[i * 4 + c] < 0.5f)
            estimate[i * 4 + c]
                = feather * dome_lum[i] * (chroma[i * 4 + c] / chroma_sum) + (1.f - feather) * estimate[i * 4 + c];
      }
    }
  }

  // ---- GPU ----
  {
    cl_mem dest = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels * 4);
    cl_mem dvld = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels * 4);
    cl_mem dclip = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels * 4);
    float max_diff = -1.f;
    if(dest && dvld && dclip
       && dt_opencl_write_buffer_to_device(devid, estimate_gpu, dest, 0, sizeof(float) * region_pixels * 4, CL_TRUE)
              == CL_SUCCESS
       && dt_opencl_write_buffer_to_device(devid, valid, dvld, 0, sizeof(float) * region_pixels * 4, CL_TRUE)
              == CL_SUCCESS
       && dt_opencl_write_buffer_to_device(devid, clip0, dclip, 0, sizeof(float) * region_pixels * 4, CL_TRUE)
              == CL_SUCCESS
       && _joint_core_stage_cl(devid, gd_void, dest, dvld, dclip, region_w, region_h, solid_color, reg_radius, 150, 0.f,
                               pipe)
              == CL_SUCCESS
       && dt_opencl_read_buffer_from_device(devid, estimate_gpu, dest, 0, sizeof(float) * region_pixels * 4,
                                            CL_TRUE)
              == CL_SUCCESS)
    {
      max_diff = 0.f;
      for(size_t i = 0; i < region_pixels; i++)
        for(int c = 0; c < 3; c++)
          max_diff = fmaxf(max_diff, fabsf(estimate_gpu[i * 4 + c] - estimate[i * 4 + c]));
    }
    fprintf(stderr, "[hl core-cl selftest] %dx%d all-clip disc + annulus max|gpu-cpu|=%.3e\n", region_w, region_h,
            max_diff);
    dt_opencl_release_mem_object(dest);
    dt_opencl_release_mem_object(dvld);
    dt_opencl_release_mem_object(dclip);
  }

done_:
  dt_pixelpipe_cache_free_align(estimate);
  dt_pixelpipe_cache_free_align(valid);
  dt_pixelpipe_cache_free_align(clip0);
  dt_pixelpipe_cache_free_align(estimate_gpu);
  dt_pixelpipe_cache_free_align(luminance);
  dt_pixelpipe_cache_free_align(dome_lum);
  dt_pixelpipe_cache_free_align(chroma);
  dt_pixelpipe_cache_free_align(chroma_work);
  dt_pixelpipe_cache_free_align(target_buf);
  dt_pixelpipe_cache_free_align(diffusion_buf);
  dt_pixelpipe_cache_free_align(scratch1);
  dt_pixelpipe_cache_free_align(scratch2);
  dt_pixelpipe_cache_free_align(scratch_sc);
  dt_pixelpipe_cache_free_align(weight_feather);
  dt_pixelpipe_cache_free_align(hole);
}

void _aniso_stage_cl_selftest(const int devid, void *gd_void, const dt_dev_pixelpipe_t *pipe)
{
  static int done = 0;
  if(done || !getenv("HL_ANISOCL_TEST") || devid < 0) return;
  done = 1;

  const int region_w = 509;
  const int region_h = 371;
  const size_t region_pixels = (size_t)region_w * region_h;
  const float epsilon = 1e-6f;

  float *estimate = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *valid = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *clip0 = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *estimate_gpu = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *prev = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *luminance = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
  float *chroma = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *planes = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  if(IS_NULL_PTR(estimate) || IS_NULL_PTR(valid) || IS_NULL_PTR(clip0) || IS_NULL_PTR(estimate_gpu)
     || IS_NULL_PTR(prev) || IS_NULL_PTR(luminance) || IS_NULL_PTR(chroma) || IS_NULL_PTR(planes))
    goto done_;

  for(int y = 0; y < region_h; y++)
    for(int x = 0; x < region_w; x++)
    {
      const size_t i = (size_t)y * region_w + x;
      // textured luminance: oriented stripes steer the isophotes
      const float base = 0.6f + 0.25f * sinf(0.05f * x + 0.08f * y) + 0.1f * cosf(0.021f * y);
      estimate[i * 4 + 0] = 0.9f * base + 0.05f;
      estimate[i * 4 + 1] = 1.1f * base + 0.02f;
      estimate[i * 4 + 2] = 0.8f * base + 0.08f;
      estimate[i * 4 + 3] = 0.f;
      const int delta_x = x - region_w / 2;
      const int delta_y = y - region_h / 2;
      const float dist = sqrtf((float)(delta_x * delta_x + delta_y * delta_y));
      const int allclip = (dist < 55.f);
      const int gclip = (dist < 90.f);
      valid[i * 4 + 0] = allclip ? 0.f : 1.f;
      valid[i * 4 + 1] = gclip ? 0.f : 1.f;
      valid[i * 4 + 2] = allclip ? 0.f : 1.f;
      valid[i * 4 + 3] = gclip ? 0.f : 1.f;
      for(int k = 0; k < 4; k++) clip0[i * 4 + k] = 0.5f;
      if(allclip)
        for(int c = 0; c < 3; c++) estimate[i * 4 + c] = 1.6f + 0.1f * c; // dome-ish core magnitude
    }
  memcpy(estimate_gpu, estimate, region_pixels * 4 * sizeof(float));

  // ---- CPU replica: production aniso block (COEFF_FIELD variant, solver 2) ----
  {
    for(size_t i = 0; i < region_pixels; i++)
    {
      const int all_clip = (valid[i * 4 + 0] < 0.5f && valid[i * 4 + 1] < 0.5f && valid[i * 4 + 2] < 0.5f);
      for(int c = 0; c < 4; c++) prev[i * 4 + c] = all_clip ? valid[i * 4 + c] : fmaxf(valid[i * 4 + c], 0.6f);
    }
    for(size_t i = 0; i < region_pixels; i++)
    {
      const float pixel_lum = fmaxf(estimate[i * 4 + 0] + estimate[i * 4 + 1] + estimate[i * 4 + 2], epsilon);
      luminance[i] = pixel_lum;
      for(int c = 0; c < 3; c++) chroma[i * 4 + c] = estimate[i * 4 + c] / pixel_lum;
    }
    static const dt_aligned_pixel_t no_react_target = { 0.f, 0.f, 0.f, 0.f };
    if(!_aniso_div_solve(chroma, prev, luminance, planes, region_w, region_h, 0.f, no_react_target, pipe))
    {
      fprintf(stderr, "[hl aniso-cl selftest] CPU div solve failed, aborting\n");
      goto done_;
    }

    // full-resolution projected polish (mirrors the production block: obstacle = clip0/L)
    {
      float *const restrict tensor_xx = planes + 0 * region_pixels;
      float *const restrict tensor_xy = planes + 1 * region_pixels;
      float *const restrict tensor_yy = planes + 2 * region_pixels;
      float *const restrict tensor_scale = planes + 3 * region_pixels;
      float *const restrict solve_u = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
      float *const restrict obstacle = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
      float *const restrict scratch = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
      uint8_t *const restrict hole_flag = (uint8_t *)dt_pixelpipe_cache_alloc_align(region_pixels, pipe);

      if(solve_u && obstacle && scratch && hole_flag)
      {
        _aniso_tensor(luminance, tensor_xx, tensor_xy, tensor_yy, tensor_scale, region_w, region_h);

        int box_x0 = region_w;
        int box_y0 = region_h;
        int box_x1 = -1;
        int box_y1 = -1;
        for(int y = 0; y < region_h; y++)
          for(int x = 0; x < region_w; x++)
          {
            const size_t i = (size_t)y * region_w + x;
            hole_flag[i] = (prev[i * 4 + 0] < 0.5f && prev[i * 4 + 1] < 0.5f && prev[i * 4 + 2] < 0.5f);
            if(hole_flag[i])
            {
              box_x0 = MIN(box_x0, x);
              box_x1 = MAX(box_x1, x);
              box_y0 = MIN(box_y0, y);
              box_y1 = MAX(box_y1, y);
            }
          }

        // activity gate (mirrors production): skip channels whose obstacle can never fire
        int active[3] = { 0, 0, 0 };
        for(size_t i = 0; i < region_pixels; i++)
        {
          if(!hole_flag[i]) continue;
          const float inv_lum = 1.f / fmaxf(luminance[i], epsilon);
          for(int c = 0; c < 3; c++) active[c] |= (chroma[i * 4 + c] <= clip0[i * 4 + c] * inv_lum * 1.001f);
        }

        if(box_x1 >= box_x0)
          for(int c = 0; c < 3; c++)
          {
            if(!active[c]) continue;
            for(size_t i = 0; i < region_pixels; i++)
            {
              solve_u[i] = chroma[i * 4 + c];
              obstacle[i] = clip0[i * 4 + c] / fmaxf(luminance[i], epsilon);
            }
            _aniso_iterate_obs(solve_u, obstacle, hole_flag, tensor_xx, tensor_xy, tensor_yy, scratch, region_w,
                               region_h, 60, box_x0, box_y0, box_x1, box_y1, 0.f, 0.f);
            for(size_t i = 0; i < region_pixels; i++) chroma[i * 4 + c] = solve_u[i];
          }
      }
      dt_pixelpipe_cache_free_align(solve_u);
      dt_pixelpipe_cache_free_align(obstacle);
      dt_pixelpipe_cache_free_align(scratch);
      dt_pixelpipe_cache_free_align(hole_flag);
    }

    for(size_t i = 0; i < region_pixels; i++)
    {
      const float ratio_sum = fmaxf(chroma[i * 4 + 0] + chroma[i * 4 + 1] + chroma[i * 4 + 2], epsilon);
      for(int c = 0; c < 3; c++)
        if(prev[i * 4 + c] < 0.5f)
        {
          const float ratio_c = fmaxf(chroma[i * 4 + c], 0.f);
          const float value = luminance[i] * ratio_c / ratio_sum;
          // soft floor, mirrors the production reassembly
          const float clip_floor = clip0[i * 4 + c];
          const float diff = value - clip_floor;
          const float soft_width = 0.02f * fmaxf(clip_floor, 1e-6f);
          estimate[i * 4 + c] = clip_floor + 0.5f * (diff + sqrtf(diff * diff + soft_width * soft_width));
        }
    }
  }

  // ---- GPU ----
  {
    cl_mem dest = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels * 4);
    cl_mem dvld = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels * 4);
    cl_mem dclip = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels * 4);
    float max_diff = -1.f;
    if(dest && dvld && dclip
       && dt_opencl_write_buffer_to_device(devid, estimate_gpu, dest, 0, sizeof(float) * region_pixels * 4, CL_TRUE)
              == CL_SUCCESS
       && dt_opencl_write_buffer_to_device(devid, valid, dvld, 0, sizeof(float) * region_pixels * 4, CL_TRUE)
              == CL_SUCCESS
       && dt_opencl_write_buffer_to_device(devid, clip0, dclip, 0, sizeof(float) * region_pixels * 4, CL_TRUE)
              == CL_SUCCESS
       && _aniso_stage_cl(devid, gd_void, dest, dvld, dclip, region_w, region_h, 55.f, 0.f, 0.f, pipe) == CL_SUCCESS
       && dt_opencl_read_buffer_from_device(devid, estimate_gpu, dest, 0, sizeof(float) * region_pixels * 4,
                                            CL_TRUE)
              == CL_SUCCESS)
    {
      max_diff = 0.f;
      for(size_t i = 0; i < region_pixels; i++)
        for(int c = 0; c < 3; c++)
          max_diff = fmaxf(max_diff, fabsf(estimate_gpu[i * 4 + c] - estimate[i * 4 + c]));
    }
    fprintf(stderr, "[hl aniso-cl selftest] %dx%d all-clip disc textured max|gpu-cpu|=%.3e\n", region_w, region_h,
            max_diff);
    dt_opencl_release_mem_object(dest);
    dt_opencl_release_mem_object(dvld);
    dt_opencl_release_mem_object(dclip);
  }

done_:
  dt_pixelpipe_cache_free_align(estimate);
  dt_pixelpipe_cache_free_align(valid);
  dt_pixelpipe_cache_free_align(clip0);
  dt_pixelpipe_cache_free_align(estimate_gpu);
  dt_pixelpipe_cache_free_align(prev);
  dt_pixelpipe_cache_free_align(luminance);
  dt_pixelpipe_cache_free_align(chroma);
  dt_pixelpipe_cache_free_align(planes);
}

void _chromaticity_gradient_stage_cl_selftest(const int devid, void *gd_void, const dt_dev_pixelpipe_t *pipe)
{
  static int done = 0;
  if(done || !getenv("HL_CGRADCL_TEST") || devid < 0) return;
  done = 1;

  const int region_w = 509;
  const int region_h = 371;
  const size_t region_pixels = (size_t)region_w * region_h;

  float *estimate = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *valid = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *clip0 = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *estimate_gpu = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *plane2 = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe);
  float *solver_field = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
  float *flat_target = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
  float *reaction_weight = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
  float *gate_tmp1 = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
  float *gate_tmp2 = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
  float *gate_res = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
  float *gate_dir = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
  uint8_t *hole = (uint8_t *)dt_pixelpipe_cache_alloc_align(region_pixels, pipe);
  if(IS_NULL_PTR(estimate) || IS_NULL_PTR(valid) || IS_NULL_PTR(clip0) || IS_NULL_PTR(estimate_gpu)
     || IS_NULL_PTR(plane2) || IS_NULL_PTR(solver_field) || IS_NULL_PTR(flat_target)
     || IS_NULL_PTR(reaction_weight) || IS_NULL_PTR(gate_tmp1) || IS_NULL_PTR(gate_tmp2)
     || IS_NULL_PTR(gate_res) || IS_NULL_PTR(gate_dir) || IS_NULL_PTR(hole))
    goto done_;

  // synthetic: a chromaticity-gradient sky (R share rising left->right) with a 2-clip disc and an all-clip core
  for(int y = 0; y < region_h; y++)
    for(int x = 0; x < region_w; x++)
    {
      const size_t i = (size_t)y * region_w + x;
      const float t = (float)x / (float)region_w;
      const float lum = 1.2f + 0.4f * sinf(0.013f * x) * cosf(0.011f * y);
      estimate[i * 4 + 0] = lum * (0.45f + 0.2f * t);
      estimate[i * 4 + 1] = lum * 0.33f;
      estimate[i * 4 + 2] = lum * (0.22f - 0.2f * t + 0.2f);
      estimate[i * 4 + 3] = 0.f;
      const int dx = x - region_w / 2, dy = y - region_h / 2;
      const float dist = sqrtf((float)(dx * dx + dy * dy));
      const int rclip = dist < 120.f, gclip = dist < 95.f, bclip = dist < 40.f;
      valid[i * 4 + 0] = rclip ? 0.f : 1.f;
      valid[i * 4 + 1] = gclip ? 0.f : 1.f;
      valid[i * 4 + 2] = bclip ? 0.f : 1.f;
      valid[i * 4 + 3] = rclip ? 0.f : 1.f;
      for(int k = 0; k < 4; k++) clip0[i * 4 + k] = 0.6f;
      if(rclip)
        for(int c = 0; c < 3; c++) estimate[i * 4 + c] = fmaxf(estimate[i * 4 + c], 0.62f);
    }
  memcpy(estimate_gpu, estimate, region_pixels * 4 * sizeof(float));

  // ---- CPU: the production stage on a minimal ctx ----
  {
    _hl_region_t region = { 0 };
    region.radius = 120.f; // the content-gate blur is sized from the region radius
    _hl_region_ctx_t ctx = { 0 };
    ctx.pipe = pipe;
    ctx.region = &region;
    ctx.region_w = region_w;
    ctx.region_h = region_h;
    ctx.region_pixels = region_pixels;
    ctx.epsilon = 1e-6f;
    ctx.estimate = estimate;
    ctx.valid = valid;
    ctx.clip0 = clip0;
    ctx.plane2 = plane2;
    ctx.solver_field = solver_field;
    ctx.flat_target = flat_target;
    ctx.reaction_weight = reaction_weight;
    ctx.cg_tmp1 = gate_tmp1;
    ctx.cg_tmp2 = gate_tmp2;
    ctx.cg_residual = gate_res;
    ctx.cg_dir = gate_dir;
    ctx.hole = hole;
    _chromaticity_gradient(&ctx);
  }

  // ---- GPU ----
  {
    cl_mem dest = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels * 4);
    cl_mem dvld = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels * 4);
    cl_mem dclip = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels * 4);
    float max_diff = -1.f;
    if(dest && dvld && dclip
       && dt_opencl_write_buffer_to_device(devid, estimate_gpu, dest, 0, sizeof(float) * region_pixels * 4, CL_TRUE)
              == CL_SUCCESS
       && dt_opencl_write_buffer_to_device(devid, valid, dvld, 0, sizeof(float) * region_pixels * 4, CL_TRUE)
              == CL_SUCCESS
       && dt_opencl_write_buffer_to_device(devid, clip0, dclip, 0, sizeof(float) * region_pixels * 4, CL_TRUE)
              == CL_SUCCESS
       && _chromaticity_gradient_stage_cl(devid, gd_void, dest, dvld, dclip, region_w, region_h, 120.f, 1.f /* exercise the authored-1-clip path */, pipe)
              == CL_SUCCESS
       && dt_opencl_read_buffer_from_device(devid, estimate_gpu, dest, 0, sizeof(float) * region_pixels * 4,
                                            CL_TRUE)
              == CL_SUCCESS)
    {
      max_diff = 0.f;
      for(size_t i = 0; i < region_pixels; i++)
        for(int c = 0; c < 3; c++)
          max_diff = fmaxf(max_diff, fabsf(estimate_gpu[i * 4 + c] - estimate[i * 4 + c]));
    }
    fprintf(stderr, "[hl cgrad-cl selftest] %dx%d gradient sky + 3-tier disc max|gpu-cpu|=%.3e\n", region_w,
            region_h, max_diff);
    dt_opencl_release_mem_object(dest);
    dt_opencl_release_mem_object(dvld);
    dt_opencl_release_mem_object(dclip);
  }

done_:
  dt_pixelpipe_cache_free_align(estimate);
  dt_pixelpipe_cache_free_align(valid);
  dt_pixelpipe_cache_free_align(clip0);
  dt_pixelpipe_cache_free_align(estimate_gpu);
  dt_pixelpipe_cache_free_align(plane2);
  dt_pixelpipe_cache_free_align(solver_field);
  dt_pixelpipe_cache_free_align(flat_target);
  dt_pixelpipe_cache_free_align(reaction_weight);
  dt_pixelpipe_cache_free_align(hole);
}

void _region_guided_filter_cl_selftest(const int devid, void *gd_void, const dt_dev_pixelpipe_t *pipe)
{
  static int done = 0;
  if(done || !getenv("HL_REGCL_TEST") || devid < 0) return;
  done = 1;

  const int width = 700;
  const int height = 520;
  const size_t n_pixels = (size_t)width * height;

  float *interp = dt_pixelpipe_cache_alloc_align_float(n_pixels * 4, pipe);
  float *interp_gpu = dt_pixelpipe_cache_alloc_align_float(n_pixels * 4, pipe);
  float *mask = dt_pixelpipe_cache_alloc_align_float(n_pixels * 4, pipe);
  float *depth = dt_pixelpipe_cache_alloc_align_float(n_pixels, pipe);
  if(IS_NULL_PTR(interp) || IS_NULL_PTR(interp_gpu) || IS_NULL_PTR(mask) || IS_NULL_PTR(depth)) goto cleanup;

  const float radii[3] = { 60.f, 75.f, 50.f };
  for(int y = 0; y < height; y++)
    for(int x = 0; x < width; x++)
    {
      const size_t i = (size_t)y * width + x;
      const float base = 0.5f + 0.3f * sinf(0.013f * x) * cosf(0.011f * y) + 0.1f * cosf(0.03f * x);
      const float gain[3] = { 0.9f, 1.15f, 0.75f };
      const int center_x = width - 80;
      const int center_y = height - 70;
      const int delta_x = x - center_x;
      const int delta_y = y - center_y;
      const float dist = sqrtf((float)(delta_x * delta_x + delta_y * delta_y));
      const int occluder = (y > center_y - 8 && y < center_y + 8); // dark occluder bar through the blob
      int any_clip = 0;
      for(int c = 0; c < 3; c++)
      {
        const int clipped = (dist < radii[c]) && !occluder;
        mask[i * 4 + c] = clipped ? 1.f : 0.f;
        interp[i * 4 + c] = clipped ? 0.62f * gain[c] : fmaxf(base * gain[c] * (occluder ? 0.15f : 1.f), 0.f);
        any_clip |= clipped;
      }
      mask[i * 4 + 3] = any_clip ? 1.f : 0.f;
      interp[i * 4 + 3] = interp[i * 4 + 0] + interp[i * 4 + 1] + interp[i * 4 + 2];
      depth[i] = fmaxf(radii[1] - dist, 0.f);
    }
  memcpy(interp_gpu, interp, n_pixels * 4 * sizeof(float));

  _hl_region_t region;
  region.x0 = width - 80 - 75;
  region.x1 = MIN(width - 80 + 75, width - 1);
  region.y0 = height - 70 - 75;
  region.y1 = MIN(height - 70 + 75, height - 1);
  region.pad = 96;
  region.rx0 = MAX(region.x0 - region.pad, 0);
  region.ry0 = MAX(region.y0 - region.pad, 0);
  region.rx1 = MIN(region.x1 + region.pad, width - 1);
  region.ry1 = MIN(region.y1 + region.pad, height - 1);
  region.radius = 52.f;
  const float solid_color = 0.3f;

  _region_guided_filter(interp, mask, depth, width, &region, pipe, solid_color, 30, 0.f, 0.7f);

  {
    cl_mem interp_device = dt_opencl_alloc_device_buffer(devid, sizeof(float) * n_pixels * 4);
    cl_mem mask_device = dt_opencl_alloc_device_buffer(devid, sizeof(float) * n_pixels * 4);
    cl_mem depth_device = dt_opencl_alloc_device_buffer(devid, sizeof(float) * n_pixels);
    float max_diff = -1.f;
    if(interp_device && mask_device && depth_device
       && dt_opencl_write_buffer_to_device(devid, interp_gpu, interp_device, 0, sizeof(float) * n_pixels * 4,
                                           CL_TRUE)
              == CL_SUCCESS
       && dt_opencl_write_buffer_to_device(devid, mask, mask_device, 0, sizeof(float) * n_pixels * 4, CL_TRUE)
              == CL_SUCCESS
       && dt_opencl_write_buffer_to_device(devid, depth, depth_device, 0, sizeof(float) * n_pixels, CL_TRUE)
              == CL_SUCCESS
       && _region_guided_filter_cl(devid, gd_void, interp_device, mask_device, depth_device, width, &region, pipe,
                                   solid_color, 0.7f)
              == CL_SUCCESS
       && dt_opencl_read_buffer_from_device(devid, interp_gpu, interp_device, 0, sizeof(float) * n_pixels * 4,
                                            CL_TRUE)
              == CL_SUCCESS)
    {
      max_diff = 0.f;
      double sum_diff = 0.0;
      size_t n_big = 0;
      for(size_t i = 0; i < n_pixels; i++)
        for(int c = 0; c < 3; c++)
        {
          const float diff = fabsf(interp_gpu[i * 4 + c] - interp[i * 4 + c]);
          max_diff = fmaxf(max_diff, diff);
          sum_diff += (double)diff;
          if(diff > 1e-4f) n_big++;
        }
      fprintf(stderr, "[hl region-cl selftest] mean=%.3e npix>1e-4: %zu/%zu\n", sum_diff / (double)(n_pixels * 3),
              n_big, n_pixels * 3);
    }
    fprintf(stderr, "[hl region-cl selftest] %dx%d staggered blob r=%g max|gpu-cpu|=%.3e\n", width, height,
            region.radius, max_diff);
    dt_opencl_release_mem_object(interp_device);
    dt_opencl_release_mem_object(mask_device);
    dt_opencl_release_mem_object(depth_device);
  }

cleanup:
  dt_pixelpipe_cache_free_align(interp);
  dt_pixelpipe_cache_free_align(interp_gpu);
  dt_pixelpipe_cache_free_align(mask);
  dt_pixelpipe_cache_free_align(depth);
}

void _knee_cl_selftest(const int devid, void *gd_void, const dt_dev_pixelpipe_t *pipe)
{
  static int done_ = 0;
  if(done_ || !getenv("HL_KNEECL_TEST") || devid < 0) return;
  done_ = 1;

  const size_t width = 1462;
  const size_t height = 1034;
  const size_t n_pixels = width * height;
  const uint32_t filters = 0x94949494u; // RGGB
  const dt_aligned_pixel_t clipraw = { 1.f, 1.f, 1.f, 1.f };
  dt_iop_roi_t roi_in = { 0 };

  float *raw_mosaic = dt_pixelpipe_cache_alloc_align_float(n_pixels, pipe);
  float *corr_cpu = dt_pixelpipe_cache_alloc_align_float(n_pixels, pipe);
  float *corr_gpu = dt_pixelpipe_cache_alloc_align_float(n_pixels, pipe);
  if(IS_NULL_PTR(raw_mosaic) || IS_NULL_PTR(corr_cpu) || IS_NULL_PTR(corr_gpu)) goto cleanup;

  // colour-lines scene: smooth chroma-correlated channels; a soft rolloff compresses the band
  for(size_t i = 0; i < height; i++)
    for(size_t j = 0; j < width; j++)
    {
      const int c = FC(i, j, filters);
      const float base = 0.55f + 0.45f * sinf(0.006f * j) * cosf(0.008f * i) + 0.12f * sinf(0.03f * (i + j));
      const float gain[3] = { 0.95f, 1.05f, 0.85f };
      float value = fmaxf(base * gain[c > 2 ? 1 : c], 0.f);
      if(value > 0.8f) value = 0.8f + (value - 0.8f) * 0.65f; // rolloff: measured lags the colour line
      raw_mosaic[i * width + j] = fminf(value, 1.f);
    }

  {
    _hl_knee_curve_t curve_cpu[3];
    _hl_knee_curve_t curve_gpu[3];
    _hl_knee_estimate(raw_mosaic, width, height, filters, &roi_in, NULL, clipraw, curve_cpu, pipe);

    cl_mem in_device = dt_opencl_alloc_device_buffer(devid, sizeof(float) * n_pixels);
    cl_mem out_device = dt_opencl_alloc_device_buffer(devid, sizeof(float) * n_pixels);
    float curve_diff = -1.f;
    float apply_diff = -1.f;
    int engaged_ok = 0;
    if(in_device && out_device
       && dt_opencl_write_buffer_to_device(devid, raw_mosaic, in_device, 0, sizeof(float) * n_pixels, CL_TRUE)
              == CL_SUCCESS
       && _hl_knee_estimate_cl(devid, gd_void, in_device, width, height, filters, &roi_in, NULL, 0, clipraw,
                               curve_gpu, pipe)
              == CL_SUCCESS)
    {
      engaged_ok = (curve_cpu[0].engaged == curve_gpu[0].engaged) && (curve_cpu[1].engaged == curve_gpu[1].engaged)
                   && (curve_cpu[2].engaged == curve_gpu[2].engaged);
      curve_diff = 0.f;
      for(int c = 0; c < 3; c++)
        for(int i = 0; i < DT_HL_KNEE_BINS; i++)
          curve_diff = fmaxf(curve_diff, fabsf(curve_cpu[c].lift[i] - curve_gpu[c].lift[i]));

      // apply parity on the CPU curves (isolates the apply kernel)
      _hl_knee_apply_cfa(raw_mosaic, corr_cpu, width, height, filters, &roi_in, NULL, clipraw, curve_cpu);
      if(_hl_knee_apply_cfa_cl(devid, gd_void, in_device, out_device, width, height, filters, &roi_in, NULL, 0,
                               clipraw, curve_cpu)
             == CL_SUCCESS
         && dt_opencl_read_buffer_from_device(devid, corr_gpu, out_device, 0, sizeof(float) * n_pixels, CL_TRUE)
                == CL_SUCCESS)
      {
        apply_diff = 0.f;
        for(size_t i = 0; i < n_pixels; i++) apply_diff = fmaxf(apply_diff, fabsf(corr_gpu[i] - corr_cpu[i]));
      }
    }
    fprintf(stderr,
            "[hl knee-cl selftest] %zux%zu RGGB engaged cpu=[%d %d %d] match=%d "
            "max|curve dcpu-gpu|=%.3e max|apply gpu-cpu|=%.3e\n",
            width, height, curve_cpu[0].engaged, curve_cpu[1].engaged, curve_cpu[2].engaged, engaged_ok,
            curve_diff, apply_diff);
    dt_opencl_release_mem_object(in_device);
    dt_opencl_release_mem_object(out_device);
  }

cleanup:
  dt_pixelpipe_cache_free_align(raw_mosaic);
  dt_pixelpipe_cache_free_align(corr_cpu);
  dt_pixelpipe_cache_free_align(corr_gpu);
}

#endif // HAVE_OPENCL
