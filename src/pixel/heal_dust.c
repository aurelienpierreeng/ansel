/*
    This file is part of Ansel,
    Copyright (C) 2026 Guillaume Stutin.

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

#ifdef HAVE_CONFIG_H
#include "config.h" // the OpenCL version, before heal_dust.h brings CL/cl.h in
#endif
#include "pixel/heal_dust.h"

#include "caches/pixelpipe_cache_alloc.h"
#include "pixel/box_filters.h"
#include "system/macros.h"
#include "system/openmp.h"
#ifdef HAVE_OPENCL
#include "common/opencl.h"
#endif

#include <stdio.h>
#include <string.h>

int dt_heal_dust(const float *const src, float *const dest, const float *const mask, const int width,
                 const int height, const dt_heal_dust_t *const repair)
{
  const size_t n = (size_t)width * height;
  int err = 1;
  float *source = dt_pixelpipe_cache_alloc_align_float_cache(4 * n, 0);
  float *mean = dt_pixelpipe_cache_alloc_align_float_cache(4 * n, 0);
  float *encoded = dt_pixelpipe_cache_alloc_align_float_cache(4 * n, 0);
  if(IS_NULL_PTR(source) || IS_NULL_PTR(mean) || IS_NULL_PTR(encoded))
  {
    fprintf(stderr, "dt_heal_dust: error allocating memory for healing\n");
    goto cleanup;
  }
  dt_heal_encode(dest, encoded, n, repair->solver.domain);
  dt_heal_encode(src, source, n, repair->solver.domain);

  // the source's detail finer than the box
  memcpy(mean, source, 4 * n * sizeof(float));
  if(dt_box_mean(mean, height, width, 4, repair->texture_radius, 1))
  {
    fprintf(stderr, "dt_heal_dust: error allocating memory for healing\n");
    goto cleanup;
  }
  __OMP_PARALLEL_FOR_SIMD__(aligned(source, mean:64))
  for(size_t k = 0; k < 4 * n; k++) source[k] -= mean[k];

  // that detail over the harmonic interpolation of the border, decoded in the mask only
  dt_heal(source, encoded, mask, width, height, 4,
          (dt_heal_solver_t){ .max_iter = repair->solver.max_iter, .domain = DT_HEAL_DOMAIN_LINEAR });
  dt_heal_decode_masked(encoded, dest, mask, n, repair->solver.domain);
  err = 0;

cleanup:
  dt_pixelpipe_cache_free_align(source);
  dt_pixelpipe_cache_free_align(mean);
  dt_pixelpipe_cache_free_align(encoded);
  return err;
}

#ifdef HAVE_OPENCL
cl_int dt_heal_dust_cl(const int devid, cl_mem dev_src, cl_mem dev_dest, const float *const mask, const int width,
                       const int height, const dt_heal_dust_t *const repair)
{
  // on the host, as dt_heal_cl() does
  const size_t floats = (size_t)4 * width * height;
  cl_int err = DT_OPENCL_SYSMEM_ALLOCATION;
  float *src = dt_pixelpipe_cache_alloc_align_float_cache(floats, 0);
  float *dest = dt_pixelpipe_cache_alloc_align_float_cache(floats, 0);
  if(IS_NULL_PTR(src) || IS_NULL_PTR(dest))
  {
    fprintf(stderr, "dt_heal_dust_cl: error allocating memory for healing\n");
    goto cleanup;
  }
  err = dt_opencl_read_buffer_from_device(devid, src, dev_src, 0, floats * sizeof(float), CL_TRUE);
  if(err != CL_SUCCESS) goto cleanup;
  err = dt_opencl_read_buffer_from_device(devid, dest, dev_dest, 0, floats * sizeof(float), CL_TRUE);
  if(err != CL_SUCCESS) goto cleanup;
  err = DT_OPENCL_SYSMEM_ALLOCATION;
  if(dt_heal_dust(src, dest, mask, width, height, repair)) goto cleanup;
  err = dt_opencl_write_buffer_to_device(devid, dest, dev_dest, 0, floats * sizeof(float), CL_TRUE);

cleanup:
  dt_pixelpipe_cache_free_align(src);
  dt_pixelpipe_cache_free_align(dest);
  return err;
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
