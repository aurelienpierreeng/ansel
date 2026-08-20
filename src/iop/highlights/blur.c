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

// Gaussian blur helpers + per-region blur, and the OpenCL blur / device-timing runtime prelude. (implementation;
// see blur.h for the public API.)

#include "iop/highlights/blur.h"
#include "iop/highlights/common.h"

static __thread _hl_gauss_slot_t _hl_gauss_cache[HL_GAUSS_SLOTS] = { { 0 } };
static __thread int _hl_gauss_rr = 0;

dt_gaussian_t *_hl_gauss_get(const int width, const int height, const int channels, const float sigma)
{
  for(int i = 0; i < HL_GAUSS_SLOTS; i++)
    if(_hl_gauss_cache[i].gaussian && _hl_gauss_cache[i].width == width && _hl_gauss_cache[i].height == height
       && _hl_gauss_cache[i].channels == channels && _hl_gauss_cache[i].sigma == sigma)
      return _hl_gauss_cache[i].gaussian;

  _hl_gauss_slot_t *slot = &_hl_gauss_cache[(_hl_gauss_rr++) % HL_GAUSS_SLOTS];
  if(slot->gaussian) dt_gaussian_free(slot->gaussian);
  float vmax[4] = { 1e9f, 1e9f, 1e9f, 1e9f };
  float vmin[4] = { -1e9f, -1e9f, -1e9f, -1e9f };
  slot->gaussian = dt_gaussian_init(width, height, channels, vmax, vmin, sigma, 0);
  slot->width = width;
  slot->height = height;
  slot->channels = channels;
  slot->sigma = sigma;
  return slot->gaussian;
}

void _hl_gauss_cache_flush(void)
{
  for(int i = 0; i < HL_GAUSS_SLOTS; i++)
  {
    if(_hl_gauss_cache[i].gaussian) dt_gaussian_free(_hl_gauss_cache[i].gaussian);
    _hl_gauss_cache[i] = (_hl_gauss_slot_t){ 0 };
  }
}

// and main pipes can run _region_guided_filter concurrently; each accumulates on its own thread.

// ============================ OpenCL ============================

#ifdef HAVE_OPENCL
// host spends BLOCKED on the device (reads, finishes) vs pure enqueue counts, plus the
// host-side sparse Cholesky work. Accumulated per thread, reset at the middle's entry,
// printed with the "gpu middle" line.
#endif // HAVE_OPENCL

#ifdef HAVE_OPENCL
dt_gaussian_cl_t *_region_blur_handle(const int devid, const int region_w, const int region_h, const float sigma)
{
  const float vmax[4] = { 1e9f, 1e9f, 1e9f, 1e9f };
  const float vmin[4] = { -1e9f, -1e9f, -1e9f, -1e9f };
  return dt_gaussian_init_cl(devid, region_w, region_h, 4, vmax, vmin, sigma, 0);
}

cl_int _region_blur_cl(const int devid, cl_mem in, cl_mem out, const int region_w, const int region_h,
                       const float sigma)
{
  dt_gaussian_cl_t *gaussian = _region_blur_handle(devid, region_w, region_h, sigma);
  if(!gaussian) return DT_OPENCL_DEFAULT_ERROR;
  const cl_int cl_err = dt_gaussian_blur_cl(gaussian, in, out);
  dt_gaussian_free_cl(gaussian);
  return cl_err;
}

#endif // HAVE_OPENCL

// ===== edge-aware transport of the fit windows (domain transform) =============================
// The coefficient field fits a colour line inside a window of sigma = radius/6 (40 px on the
// DSC_1267.NEF ridge, so a +/-120 px reach). An isotropic gaussian carries samples straight across
// a silhouette, so at a pixel 10 px from a mountain edge the "local" colour line is a mixture of
// sky and rock, and the predicted channel is biased -- measured as a ring of +6% magenta peaking
// 8-12 px from the edge, gone by 30 px. Shrinking the window removes the ring but costs a third of
// the reconstruction strength everywhere, and no PER-SAMPLE photometric test can separate the two
// materials here: they differ by 0.0105 in chromaticity while the sky's own spread is 0.0229, and
// their luminances overlap. What does separate them is the EDGE between them, which is exactly what
// a domain transform keys on.
//
// Gastal & Oliveira 2011, "Domain Transform for Edge-Aware Image and Video Processing", recursive
// (RF) variant: warp each row/column so that distance grows with the guide's gradient, then run a
// first-order IIR in the warped domain. Three iterations with halving sigma approximate a gaussian
// while refusing to transport across an edge. O(pixels) per pass, like the Young-van-Vliet gaussian
// it replaces, and normalized by construction (unit DC gain) so the moments stay consistent with
// the mass plane they are divided by.
void _region_edge_blur(const float *const restrict in, float *const restrict out,
                       const float *const restrict guide, float *const restrict step_x,
                       float *const restrict step_y, const int region_w, const int region_h,
                       const float sigma_s, const float sigma_r)
{
  const size_t region_pixels = (size_t)region_w * region_h;
  memcpy(out, in, region_pixels * 4 * sizeof(float));
  if(!(sigma_s > 0.f) || !(sigma_r > 0.f)) return;

  // domain transform: the local warp rate, 1 where the guide is flat, large across an edge
  const float range_scale = sigma_s / sigma_r;
  HL_PFOR()
  for(int y = 0; y < region_h; y++)
    for(int x = 0; x < region_w; x++)
    {
      const size_t i = (size_t)y * region_w + x;
      const float dx = (x > 0) ? fabsf(guide[i] - guide[i - 1]) : 0.f;
      const float dy = (y > 0) ? fabsf(guide[i] - guide[i - region_w]) : 0.f;
      step_x[i] = 1.f + range_scale * dx;
      step_y[i] = 1.f + range_scale * dy;
    }

  const int n_iterations = 3;
  for(int iteration = 0; iteration < n_iterations; iteration++)
  {
    // sigma of this iteration: halving, so the three together approximate one gaussian of sigma_s
    const float sigma_i = sigma_s * sqrtf(3.f) * (float)(1 << (n_iterations - 1 - iteration))
                          / sqrtf((float)((1 << (2 * n_iterations)) - 1));
    const float feedback = expf(-sqrtf(2.f) / fmaxf(sigma_i, 1e-6f));

    HL_PFOR() // rows: forward then backward, in the warped domain
    for(int y = 0; y < region_h; y++)
    {
      float *const restrict row = out + (size_t)y * region_w * 4;
      const float *const restrict step = step_x + (size_t)y * region_w;
      for(int x = 1; x < region_w; x++)
      {
        const float a = powf(feedback, step[x]);
        for(int c = 0; c < 4; c++) row[x * 4 + c] += a * (row[(x - 1) * 4 + c] - row[x * 4 + c]);
      }
      for(int x = region_w - 2; x >= 0; x--)
      {
        const float a = powf(feedback, step[x + 1]);
        for(int c = 0; c < 4; c++) row[x * 4 + c] += a * (row[(x + 1) * 4 + c] - row[x * 4 + c]);
      }
    }

    HL_PFOR() // columns: same, down then up
    for(int x = 0; x < region_w; x++)
    {
      for(int y = 1; y < region_h; y++)
      {
        const size_t i = (size_t)y * region_w + x;
        const float a = powf(feedback, step_y[i]);
        for(int c = 0; c < 4; c++)
          out[i * 4 + c] += a * (out[(i - region_w) * 4 + c] - out[i * 4 + c]);
      }
      for(int y = region_h - 2; y >= 0; y--)
      {
        const size_t i = (size_t)y * region_w + x;
        const float a = powf(feedback, step_y[i + region_w]);
        for(int c = 0; c < 4; c++)
          out[i * 4 + c] += a * (out[(i + region_w) * 4 + c] - out[i * 4 + c]);
      }
    }
  }
}

#ifdef HAVE_OPENCL
// Device twin of _region_edge_blur. The moment planes are images, but a recursive sweep must read
// and write the same line, which an OpenCL 1.2 image cannot do, so the filter runs on a buffer:
// image -> buffer, warp + three iterations of row/column sweeps, buffer -> image. `guide` is the
// already-smoothed single-channel guide buffer (the caller smooths it once per region, not once
// per moment plane). All scratch is supplied by the caller so nothing allocates per call.
cl_int _region_edge_blur_cl(const int devid, void *gd_void, cl_mem in_image, cl_mem out_image,
                            cl_mem guide, cl_mem data, cl_mem step_x, cl_mem step_y, const int region_w,
                            const int region_h, const float sigma_s, const float sigma_r)
{
  dt_iop_highlights_global_data_t *const global_data = (dt_iop_highlights_global_data_t *)gd_void;
  const size_t origin[] = { 0, 0, 0 };
  const size_t region[] = { (size_t)region_w, (size_t)region_h, 1 };
  size_t size_2d[3] = { ROUNDUPDWD(region_w, devid), ROUNDUPDHT(region_h, devid), 1 };

  cl_int cl_err = dt_opencl_enqueue_copy_image_to_buffer(devid, in_image, data, (size_t *)origin,
                                                         (size_t *)region, 0);
  if(cl_err != CL_SUCCESS) return cl_err;
  if(!(sigma_s > 0.f) || !(sigma_r > 0.f))
    return dt_opencl_enqueue_copy_buffer_to_image(devid, data, out_image, 0, (size_t *)origin,
                                                  (size_t *)region);

  const float range_scale = sigma_s / sigma_r;
  {
    const int kernel = global_data->kernel_hl_dt_warp;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &guide);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &step_x);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &step_y);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(float), &range_scale);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size_2d);
    if(cl_err != CL_SUCCESS) return cl_err;
  }

  const int n_iterations = 3;
  for(int iteration = 0; iteration < n_iterations && cl_err == CL_SUCCESS; iteration++)
  {
    const float sigma_i = sigma_s * sqrtf(3.f) * (float)(1 << (n_iterations - 1 - iteration))
                          / sqrtf((float)((1 << (2 * n_iterations)) - 1));
    const float feedback = expf(-sqrtf(2.f) / fmaxf(sigma_i, 1e-6f));

    { // one work-item per row
      const int kernel = global_data->kernel_hl_dt_rows;
      size_t size_rows[3] = { ROUNDUP(region_h, 64), 1, 1 };
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &data);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &step_x);
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &region_w);
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region_h);
      dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(float), &feedback);
      cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size_rows);
    }
    if(cl_err != CL_SUCCESS) break;
    { // one work-item per column
      const int kernel = global_data->kernel_hl_dt_cols;
      size_t size_cols[3] = { ROUNDUP(region_w, 64), 1, 1 };
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &data);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &step_y);
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &region_w);
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region_h);
      dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(float), &feedback);
      cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size_cols);
    }
  }
  if(cl_err != CL_SUCCESS) return cl_err;

  return dt_opencl_enqueue_copy_buffer_to_image(devid, data, out_image, 0, (size_t *)origin,
                                                (size_t *)region);
}
#endif
