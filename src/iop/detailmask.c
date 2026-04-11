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
    along with Ansel.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common/imagebuf.h"
#include "common/interpolation.h"
#include "common/opencl.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "develop/pixelpipe.h"
#include "iop/iop_api.h"
#include <glib/gi18n.h>

DT_MODULE_INTROSPECTION(1, dt_iop_detailmask_params_t)

typedef struct dt_iop_detailmask_params_t
{
  int dummy;
} dt_iop_detailmask_params_t;

typedef struct dt_iop_detailmask_params_t dt_iop_detailmask_data_t;

const char *name()
{
  return _("detail mask");
}

int default_group()
{
  return IOP_GROUP_TECHNICAL;
}

int flags()
{
  return IOP_FLAGS_HIDDEN | IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_NO_HISTORY_STACK;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

void input_format(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                  dt_iop_buffer_dsc_t *dsc)
{
  default_input_format(self, pipe, piece, dsc);
  dsc->channels = 4;
  dsc->datatype = TYPE_FLOAT;
  dt_iop_buffer_dsc_update_bpp(dsc);
}

void output_format(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                   dt_iop_buffer_dsc_t *dsc)
{
  default_output_format(self, pipe, piece, dsc);
  dsc->channels = 4;
  dsc->datatype = TYPE_FLOAT;
  dt_iop_buffer_dsc_update_bpp(dsc);
}

void distort_mask(struct dt_iop_module_t *self, const struct dt_dev_pixelpipe_t *pipe,
                  struct dt_dev_pixelpipe_iop_t *piece, const float *const in, float *const out,
                  const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  if((roi_in->scale == roi_out->scale)
     && (roi_in->width == roi_out->width)
     && (roi_in->height == roi_out->height)
     && (roi_in->x == roi_out->x)
     && (roi_in->y == roi_out->y))
  {
    dt_iop_copy_image_roi(out, in, 1, roi_in, roi_out, TRUE);
    return;
  }

  const struct dt_interpolation *itor = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);
  dt_interpolation_resample_roi_1c(itor, out, roi_out, in, roi_in);
}

int process(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece,
            const void *const ivoid, void *const ovoid)
{
  dt_dev_pixelpipe_t *const mutable_pipe = (dt_dev_pixelpipe_t *)pipe;
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  const uint64_t mask_hash = dt_dev_pixelpipe_rawdetail_mask_hash(piece);
  const int width = roi_out->width;
  const int height = roi_out->height;
  dt_pixel_cache_entry_t *entry = NULL;
  void *cache_data = NULL;
  float *mask = NULL;
  float *tmp = NULL;
  int created = 0;

  dt_iop_image_copy_by_size(ovoid, ivoid, width, height, 4);
  dt_dev_clear_rawdetail_mask(mutable_pipe);

  created = dt_dev_pixelpipe_cache_get(darktable.pixelpipe_cache, mask_hash, sizeof(float) * (size_t)width * height,
                                       "detailmask rawdetail", pipe->type, TRUE, &cache_data, &entry);
  mask = (float *)cache_data;
  if(IS_NULL_PTR(mask) || IS_NULL_PTR(entry)) goto error;

  mutable_pipe->rawdetail_mask_hash = mask_hash;
  memcpy(&mutable_pipe->rawdetail_mask_roi, roi_out, sizeof(dt_iop_roi_t));
  if(!created) return 0;

  tmp = dt_pixelpipe_cache_alloc_align_float_cache((size_t)width * height, 0);
  if(IS_NULL_PTR(tmp)) goto error;

  dt_aligned_pixel_t wb = { 1.0f, 1.0f, 1.0f };
  if(piece->dsc_in.temperature.enabled)
  {
    wb[0] = piece->dsc_in.temperature.coeffs[0];
    wb[1] = piece->dsc_in.temperature.coeffs[1];
    wb[2] = piece->dsc_in.temperature.coeffs[2];
  }

  /* This stage always sees tightly-packed RGBA float pixels, so the luminance
   * input uses an explicit 4-float stride while the Scharr operator runs on the
   * 1-float-per-pixel temporary buffer. No CFA layout survives past demosaic. */
  dt_masks_calc_rawdetail_mask((float *const)ovoid, mask, tmp, width, height, wb);
  dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, FALSE, entry);
  dt_pixelpipe_cache_free_align(tmp);
  dt_print(DT_DEBUG_MASKS, "[detailmask process] (%ix%i)\n", width, height);

  return 0;

error:
  fprintf(stderr, "[detailmask process] couldn't write detail mask\n");
  if(created && entry != NULL)
    dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, FALSE, entry);
  dt_dev_clear_rawdetail_mask(mutable_pipe);
  if(entry != NULL)
  {
    if(created) dt_dev_pixelpipe_cache_remove(darktable.pixelpipe_cache, TRUE, entry);
  }
  dt_pixelpipe_cache_free_align(tmp);
  return 1;
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in, cl_mem dev_out)
{
  dt_dev_pixelpipe_t *const mutable_pipe = (dt_dev_pixelpipe_t *)pipe;
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  const int devid = pipe->devid;
  const uint64_t mask_hash = dt_dev_pixelpipe_rawdetail_mask_hash(piece);
  const int width = roi_out->width;
  const int height = roi_out->height;
  dt_pixel_cache_entry_t *entry = NULL;
  void *cache_data = NULL;
  cl_mem detail = NULL;
  cl_mem mask_dev = NULL;
  float *mask = NULL;
  cl_int err = CL_SUCCESS;
  int created = 0;

  size_t origin[3] = { 0, 0, 0 };
  size_t region[3] = { (size_t)width, (size_t)height, 1 };
  if(dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, region) != CL_SUCCESS)
    return FALSE;

  dt_dev_clear_rawdetail_mask(mutable_pipe);
  created = dt_dev_pixelpipe_cache_get(darktable.pixelpipe_cache, mask_hash, sizeof(float) * (size_t)width * height,
                                       "detailmask rawdetail", pipe->type, TRUE, &cache_data, &entry);
  mask = (float *)cache_data;
  if(IS_NULL_PTR(mask) || IS_NULL_PTR(entry)) goto error;

  mutable_pipe->rawdetail_mask_hash = mask_hash;
  memcpy(&mutable_pipe->rawdetail_mask_roi, roi_out, sizeof(dt_iop_roi_t));
  if(!created) return TRUE;

  detail = dt_opencl_alloc_device_buffer(devid, sizeof(float) * width * height);
  if(IS_NULL_PTR(detail)) goto error;
  mask_dev = dt_opencl_alloc_device_buffer(devid, sizeof(float) * width * height);
  if(IS_NULL_PTR(mask_dev)) goto error;

  {
    const int kernel = darktable.opencl->blendop->kernel_calc_Y0_mask;
    dt_aligned_pixel_t wb = { 1.0f, 1.0f, 1.0f };
    if(piece->dsc_in.temperature.enabled)
    {
      wb[0] = piece->dsc_in.temperature.coeffs[0];
      wb[1] = piece->dsc_in.temperature.coeffs[1];
      wb[2] = piece->dsc_in.temperature.coeffs[2];
    }

    size_t sizes[3] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &detail);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &dev_out);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &height);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(float), &wb[0]);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(float), &wb[1]);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(float), &wb[2]);
    err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
    if(err != CL_SUCCESS) goto error;
  }

  {
    const int kernel = darktable.opencl->blendop->kernel_calc_scharr_mask;
    size_t sizes[3] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &detail);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &mask_dev);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &height);
    err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
    if(err != CL_SUCCESS) goto error;
  }

  err = dt_opencl_read_buffer_from_device(devid, mask, mask_dev, 0, sizeof(float) * (size_t)width * height, CL_TRUE);
  if(err != CL_SUCCESS) goto error;

  dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, FALSE, entry);
  dt_opencl_release_mem_object(detail);
  dt_opencl_release_mem_object(mask_dev);
  dt_print(DT_DEBUG_MASKS, "[detailmask process_cl] (%ix%i)\n", width, height);

  return TRUE;

error:
  fprintf(stderr, "[detailmask process_cl] couldn't write detail mask: %i\n", err);
  if(created && entry != NULL)
    dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, FALSE, entry);
  dt_dev_clear_rawdetail_mask(mutable_pipe);
  if(entry != NULL)
  {
    if(created) dt_dev_pixelpipe_cache_remove(darktable.pixelpipe_cache, TRUE, entry);
  }
  dt_opencl_release_mem_object(detail);
  dt_opencl_release_mem_object(mask_dev);
  return FALSE;
}
#endif

void commit_params(dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  memcpy(piece->data, params, sizeof(dt_iop_detailmask_params_t));
  piece->process_tiling_ready = 0;
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc_align(sizeof(dt_iop_detailmask_data_t));
  piece->data_size = sizeof(dt_iop_detailmask_data_t);
  piece->enabled = FALSE;
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_free_align(piece->data);
  piece->data = NULL;
}

void init(dt_iop_module_t *module)
{
  dt_iop_default_init(module);
  module->default_enabled = 0;
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
