/*
    This file is part of darktable,
    Copyright (C) 2010-2020 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif
#include <cairo.h>

#include "common/opencl.h"
#include "common/imagebuf.h"
#include "common/iop_profile.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"

#include "develop/tiling.h"
#include "iop/iop_api.h"

DT_MODULE(3)

typedef enum dt_iop_overexposed_colorscheme_t
{
  DT_IOP_OVEREXPOSED_BLACKWHITE = 0,
  DT_IOP_OVEREXPOSED_REDBLUE = 1,
  DT_IOP_OVEREXPOSED_PURPLEGREEN = 2
} dt_iop_overexposed_colorscheme_t;

static const float DT_ALIGNED_ARRAY dt_iop_overexposed_colors[][2][4]
    = { {
          { 0.0f, 0.0f, 0.0f, 1.0f }, // black
          { 1.0f, 1.0f, 1.0f, 1.0f }  // white
        },
        {
          { 1.0f, 0.0f, 0.0f, 1.0f }, // red
          { 0.0f, 0.0f, 1.0f, 1.0f }  // blue
        },
        {
          { 0.371f, 0.434f, 0.934f, 1.0f }, // purple (#5f6fef)
          { 0.512f, 0.934f, 0.371f, 1.0f }  // green  (#83ef5f)
        } };

typedef struct dt_iop_overexposed_global_data_t
{
  int kernel_overexposed;
} dt_iop_overexposed_global_data_t;

typedef struct dt_iop_overexposed_t
{
  int dummy;
} dt_iop_overexposed_t;

const char *name()
{
  return _("overexposed");
}

int default_group()
{
  return IOP_GROUP_TECHNICAL;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_HIDDEN | IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_NO_HISTORY_STACK;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}


int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  // we do no longer have module params in here and just ignore any legacy entries
  return 0;
}


int process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  if (!dt_iop_have_required_input_format(4 /*we need full-color pixels*/, piece->module, piece->colors,
                                         ivoid, ovoid, roi_in, roi_out))
    return 0; // image has been copied through to output and module's trouble flag has been updated

  dt_develop_t *dev = self->dev;

  const int ch = 4;

  const float lower = exp2f(fminf(dev->overexposed.lower, -4.f));   // in EV
  const float upper = dev->overexposed.upper / 100.0f;              // in %

  const int colorscheme = dev->overexposed.colorscheme;
  const float *const upper_color = dt_iop_overexposed_colors[colorscheme][0];
  const float *const lower_color = dt_iop_overexposed_colors[colorscheme][1];

  const float *const restrict in = __builtin_assume_aligned((const float *const restrict)ivoid, 64);
  float *const restrict out = __builtin_assume_aligned((float *const restrict)ovoid, 64);

  const dt_iop_order_iccprofile_info_t *const current_profile = dt_ioppr_get_pipe_current_profile_info(self, piece->pipe);

  #ifdef __SSE2__
    // flush denormals to zero to avoid performance penalty if there are a lot of near-zero values
    const unsigned int oldMode = _MM_GET_FLUSH_ZERO_MODE();
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
  #endif

  if(dev->overexposed.mode == DT_CLIPPING_PREVIEW_ANYRGB)
  {
    // Any of the RGB channels is out of bounds
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ch, in, lower, lower_color, out, roi_out, \
                      upper, upper_color) \
  schedule(static)
#endif
    for(size_t k = 0; k < (size_t)ch * roi_out->width * roi_out->height; k += ch)
    {
      if(in[k + 0] >= upper || in[k + 1] >= upper || in[k + 2] >= upper)
      {
        copy_pixel(out + k, upper_color);
      }
      else if(in[k + 0] <= lower && in[k + 1] <= lower && in[k + 2] <= lower)
      {
        copy_pixel(out + k, lower_color);
      }
      else
      {
        copy_pixel(out + k, in + k);
      }
    }
  }

  else if(dev->overexposed.mode == DT_CLIPPING_PREVIEW_GAMUT && current_profile)
  {
    // Gamut is out of bounds
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ch, in, lower, lower_color, out, roi_out, \
                      upper, upper_color, current_profile) \
  schedule(static)
#endif
    for(size_t k = 0; k < (size_t)ch * roi_out->width * roi_out->height; k += ch)
    {
      const float luminance = dt_ioppr_get_rgb_matrix_luminance(in + k,
                                                                current_profile->matrix_in, current_profile->lut_in,
                                                                current_profile->unbounded_coeffs_in,
                                                                current_profile->lutsize, current_profile->nonlinearlut);

      // luminance is out of bounds
      if(luminance >= upper)
      {
        copy_pixel(out + k, upper_color);
      }
      else if(luminance <= lower)
      {
        copy_pixel(out + k, lower_color);
      }
      // luminance is ok, so check for saturation
      else
      {
        dt_aligned_pixel_t saturation = { 0.f };

        for_each_channel(c,aligned(saturation, in : 64))
        {
          saturation[c] = (in[k + c] - luminance);
          saturation[c] = sqrtf(saturation[c] * saturation[c] / (luminance * luminance + in[k + c] * in[k + c]));
        }

        // we got over-saturation, relatively to luminance or absolutely over RGB
        if(saturation[0] > upper || saturation[1] > upper || saturation[2] > upper ||
          in[k + 0] >= upper || in[k + 1] >= upper || in[k + 2] >= upper)
        {
          copy_pixel(out + k, upper_color);
        }

        // saturation is fine but we got out-of-bounds RGB
        else if(in[k + 0] <= lower && in[k + 1] <= lower && in[k + 2] <= lower)
        {
          copy_pixel(out + k, lower_color);
        }

        // evererything is fine
        else
        {
          copy_pixel(out + k, in + k);
        }
      }
    }
  }

  else if(dev->overexposed.mode == DT_CLIPPING_PREVIEW_LUMINANCE && current_profile)
  {
    // Luminance channel is out of bounds
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ch, in, lower, lower_color, out, roi_out, \
                      upper, upper_color, current_profile) \
  schedule(static)
#endif
    for(size_t k = 0; k < (size_t)ch * roi_out->width * roi_out->height; k += ch)
    {
      const float luminance = dt_ioppr_get_rgb_matrix_luminance(in + k,
                                                                current_profile->matrix_in, current_profile->lut_in,
                                                                current_profile->unbounded_coeffs_in,
                                                                current_profile->lutsize, current_profile->nonlinearlut);

      if(luminance >= upper)
      {
        copy_pixel(out + k, upper_color);
      }

      else if(luminance <= lower)
      {
        copy_pixel(out + k, lower_color);
      }
      else
      {
        copy_pixel(out + k, in + k);
      }
    }
  }

  else if(dev->overexposed.mode == DT_CLIPPING_PREVIEW_SATURATION && current_profile)
  {
    // Show saturation out of bounds where luminance is valid
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ch, in, lower, lower_color, out, roi_out, \
                      upper, upper_color, current_profile) \
  schedule(static)
#endif
    for(size_t k = 0; k < (size_t)ch * roi_out->width * roi_out->height; k += ch)
    {
      const float luminance = dt_ioppr_get_rgb_matrix_luminance(in + k,
                                                                current_profile->matrix_in, current_profile->lut_in,
                                                                current_profile->unbounded_coeffs_in,
                                                                current_profile->lutsize, current_profile->nonlinearlut);
      if(luminance < upper && luminance > lower)
      {
        dt_aligned_pixel_t saturation = { 0.f };

        for_each_channel(c,aligned(saturation, in : 64))
        {
          saturation[c] = (in[k + c] - luminance);
          saturation[c] = sqrtf(saturation[c] * saturation[c] / (luminance * luminance + in[k + c] * in[k + c]));
        }

        // we got over-saturation, relatively to luminance or absolutely over RGB
        if(saturation[0] > upper || saturation[1] > upper || saturation[2] > upper ||
           in[k + 0] >= upper || in[k + 1] >= upper || in[k + 2] >= upper)
        {
          copy_pixel(out + k, upper_color);
        }
        else if(in[k + 0] <= lower && in[k + 1] <= lower && in[k + 2] <= lower)
        {
          copy_pixel(out + k, lower_color);
        }
        else
        {
          copy_pixel(out + k, in + k);
        }
      }

      else
      {
        copy_pixel(out + k, in + k);
      }
    }
  }

  #ifdef __SSE2__
    _MM_SET_FLUSH_ZERO_MODE(oldMode);
  #endif

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
    dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);

  return 0;
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_develop_t *dev = self->dev;
  dt_iop_overexposed_global_data_t *gd = (dt_iop_overexposed_global_data_t *)self->global_data;

  cl_int err = -999;
  const int devid = piece->pipe->devid;
  const int width = roi_out->width;
  const int height = roi_out->height;

  const dt_iop_order_iccprofile_info_t *const current_profile = dt_ioppr_get_pipe_current_profile_info(self, piece->pipe);

  const int use_work_profile = (current_profile == NULL) ? 0 : 1;
  cl_mem dev_profile_info = NULL;
  cl_mem dev_profile_lut = NULL;
  dt_colorspaces_iccprofile_info_cl_t *profile_info_cl;
  cl_float *profile_lut_cl = NULL;

  err = dt_ioppr_build_iccprofile_params_cl(current_profile, devid, &profile_info_cl, &profile_lut_cl,
                                            &dev_profile_info, &dev_profile_lut);
  if(err != CL_SUCCESS) goto error;

  const float lower = exp2f(fminf(dev->overexposed.lower, -4.f));   // in EV
  const float upper = dev->overexposed.upper / 100.0f;              // in %
  const int colorscheme = dev->overexposed.colorscheme;

  const float *upper_color = dt_iop_overexposed_colors[colorscheme][0];
  const float *lower_color = dt_iop_overexposed_colors[colorscheme][1];
  const int mode = dev->overexposed.mode;

  size_t sizes[2] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid) };
  dt_opencl_set_kernel_arg(devid, gd->kernel_overexposed, 0, sizeof(cl_mem), &dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_overexposed, 1, sizeof(cl_mem), &dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_overexposed, 2, sizeof(int), &width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_overexposed, 3, sizeof(int), &height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_overexposed, 4, sizeof(float), &lower);
  dt_opencl_set_kernel_arg(devid, gd->kernel_overexposed, 5, sizeof(float), &upper);
  dt_opencl_set_kernel_arg(devid, gd->kernel_overexposed, 6, 4 * sizeof(float), lower_color);
  dt_opencl_set_kernel_arg(devid, gd->kernel_overexposed, 7, 4 * sizeof(float), upper_color);
  dt_opencl_set_kernel_arg(devid, gd->kernel_overexposed, 8, sizeof(cl_mem), (void *)&dev_profile_info);
  dt_opencl_set_kernel_arg(devid, gd->kernel_overexposed, 9, sizeof(cl_mem), (void *)&dev_profile_lut);
  dt_opencl_set_kernel_arg(devid, gd->kernel_overexposed, 10, sizeof(int), (void *)&use_work_profile);
  dt_opencl_set_kernel_arg(devid, gd->kernel_overexposed, 11, sizeof(int), (void *)&mode);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_overexposed, sizes);
  if(err != CL_SUCCESS) goto error;
  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_overexposed] couldn't enqueue kernel! %i\n", err);
  return FALSE;
}
#endif

void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  tiling->factor = 3.0f;  // in + out + temp
  tiling->factor_cl = 3.0f;
  tiling->maxbuf = 1.0f;
  tiling->maxbuf_cl = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = 0;
  tiling->xalign = 1;
  tiling->yalign = 1;
}


void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl from programs.conf
  dt_iop_overexposed_global_data_t *gd
      = (dt_iop_overexposed_global_data_t *)malloc(sizeof(dt_iop_overexposed_global_data_t));
  module->data = gd;
  gd->kernel_overexposed = dt_opencl_create_kernel(program, "overexposed");
}


void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_overexposed_global_data_t *gd = (dt_iop_overexposed_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_overexposed);
  free(module->data);
  module->data = NULL;
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  if(pipe->type != DT_DEV_PIXELPIPE_FULL || !self->dev->overexposed.enabled || !self->dev->gui_attached)
    piece->enabled = 0;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = NULL;
  piece->data_size = 0;
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_overexposed_t));
  module->default_params = calloc(1, sizeof(dt_iop_overexposed_t));
  module->hide_enable_button = 1;
  module->default_enabled = 1;
  module->params_size = sizeof(dt_iop_overexposed_t);
  module->gui_data = NULL;

  // This module permanently bypasses the cache because it takes input from GUI
  // and doesn't leave internal parameters to compute an integrity hash on.
  dt_iop_set_cache_bypass(module, TRUE);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
