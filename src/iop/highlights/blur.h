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

#pragma once

// Gaussian blur helpers + per-region blur, and the OpenCL blur / device-timing runtime prelude.
// Public API of this highlights harmonic-transposition module (a compiled TU). Include
// this header to call into the module; internals are static in the .c. See common.h.

#include "common/darktable.h"
#include "common/gaussian.h"
#include "common/opencl.h"
#include "iop/highlights/common.h"

dt_gaussian_t *_hl_gauss_get(const int width, const int height, const int channels, const float sigma);

void _hl_gauss_cache_flush(void);

static inline void _region_blur(const float *const restrict in, float *const restrict out, const int region_w,
                                const int region_h, const float sigma)
{
  dt_gaussian_t *const gaussian = _hl_gauss_get(region_w, region_h, 4, sigma); // cached, do not free

  if(!gaussian)
  {
    memcpy(out, in, (size_t)region_w * region_h * 4 * sizeof(float));
    return;
  }

  dt_gaussian_blur_4c(gaussian, in, out);
}

#ifdef HAVE_OPENCL
// GPU counterpart of _region_blur: the exact same Young-van-Vliet recursive gaussian, through
// the existing dt_gaussian OpenCL implementation. in/out are 4-channel image2d of the region.
dt_gaussian_cl_t *_region_blur_handle(const int devid, const int region_w, const int region_h, const float sigma);

// One-shot gaussian blur of a 4-channel region image on the GPU (init + blur + free).
// Mirrors _region_blur on the CPU: any change here must be mirrored there and re-validated
// with the HL_BLURCL_TEST self-test (_region_blur_cl_selftest).
cl_int _region_blur_cl(const int devid, cl_mem in, cl_mem out, const int region_w, const int region_h,
                       const float sigma);
#endif
