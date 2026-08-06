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
#include <string.h>

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
