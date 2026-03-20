/*
    This file is part of darktable,
    Copyright (C) 2009-2012 johannes hanika.
    Copyright (C) 2010-2011 Bruce Guenter.
    Copyright (C) 2010-2011 Henrik Andersson.
    Copyright (C) 2010 José Carlos García Sogo.
    Copyright (C) 2010 Pascal de Bruijn.
    Copyright (C) 2011 Robert Bieber.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012, 2014, 2017 Ulrich Pegelow.
    Copyright (C) 2013, 2020 Aldric Renaudin.
    Copyright (C) 2013-2014, 2016-2017, 2019 Tobias Ellinghaus.
    Copyright (C) 2014-2016 Roman Lebedev.
    Copyright (C) 2015 Pedro Côrte-Real.
    Copyright (C) 2018, 2020, 2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2018-2019 Edgardo Hoszowski.
    Copyright (C) 2019 Andreas Schneider.
    Copyright (C) 2020 Diederik Ter Rahe.
    Copyright (C) 2020 Harold le Clément de Saint-Marcq.
    Copyright (C) 2020-2021 Pascal Obry.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Philipp Lutz.
    
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
#include "common/darktable.h"
#include "config.h"
#endif
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "common/colorspaces_inline_conversions.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"

#include "gui/gtk.h"
#include "iop/iop_api.h"

/**
 * @brief This module converts float32 RGBA pixels to uint8 BGRA pixels, for
 * GUI pipelines only (darkroom main preview and navigation thumbnail).
 * It self-disables for export pipelines.
 *
 */

DT_MODULE_INTROSPECTION(1, dt_iop_gamma_params_t)


typedef struct dt_iop_gamma_params_t
{
  float gamma, linear;
} dt_iop_gamma_params_t;

#ifdef HAVE_OPENCL
typedef struct dt_iop_gamma_global_data_t
{
  int kernel_gamma_pack;
} dt_iop_gamma_global_data_t;

typedef enum dt_iop_gamma_kernel_mode_t
{
  DT_IOP_GAMMA_KERNEL_COPY = 0,
  DT_IOP_GAMMA_KERNEL_MASK = 1,
  DT_IOP_GAMMA_KERNEL_CHANNEL_MONO = 2,
  DT_IOP_GAMMA_KERNEL_CHANNEL_FALSE_COLOR = 3
} dt_iop_gamma_kernel_mode_t;

typedef enum dt_iop_gamma_false_color_t
{
  DT_IOP_GAMMA_FALSE_COLOR_MONO = 0,
  DT_IOP_GAMMA_FALSE_COLOR_A = 1,
  DT_IOP_GAMMA_FALSE_COLOR_B = 2,
  DT_IOP_GAMMA_FALSE_COLOR_R = 3,
  DT_IOP_GAMMA_FALSE_COLOR_G = 4,
  DT_IOP_GAMMA_FALSE_COLOR_B_CH = 5,
  DT_IOP_GAMMA_FALSE_COLOR_C = 6,
  DT_IOP_GAMMA_FALSE_COLOR_LCH_H = 7,
  DT_IOP_GAMMA_FALSE_COLOR_HSL_H = 8,
  DT_IOP_GAMMA_FALSE_COLOR_JZ_HZ = 9
} dt_iop_gamma_false_color_t;
#endif

const char *name()
{
  return C_("modulename", "display encoding");
}

int default_group()
{
  return IOP_GROUP_TECHNICAL;
}

int flags()
{
  return IOP_FLAGS_HIDDEN | IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_FENCE | IOP_FLAGS_UNSAFE_COPY | IOP_FLAGS_NO_HISTORY_STACK;
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
}

void output_format(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                   dt_iop_buffer_dsc_t *dsc)
{
  dsc->channels = 4;
  dsc->datatype = TYPE_UINT8;
  dsc->cst = self->default_colorspace(self, pipe, piece);
}


#ifdef _OPENMP
#pragma omp declare simd aligned(in, out, mask_color: 16) uniform(mask_color, alpha)
#endif
static inline void _write_pixel(const float *const restrict in, uint8_t *const restrict out,
                                const float *const restrict mask_color, const float alpha)
{
  // takes a linear RGB pixel as input
  dt_aligned_pixel_t pixel;

  // linear sRGB (REC 709) -> gamma corrected sRGB
  for(size_t c = 0; c < 3; c++)
    pixel[c] = in[c] <= 0.0031308f ? 12.92f * in[c] : (1.0f + 0.055f) * powf(in[c], 1.0f / 2.4f) - 0.055f;

  // the output of this module is BGR(A) instead of RGBA; can't use for_each_channel here due to the index swap
  for(size_t c = 0; c < 3; c++)
  {
    const float value = roundf(255.0f * (pixel[c] * (1.0f - alpha) + mask_color[c] * alpha));
    out[2 - c] = (uint8_t)(fminf(fmaxf(value, 0.0f), 255.0f));
  }
}


#ifdef _OPENMP
#pragma omp declare simd aligned(pixel: 16) uniform(norm)
#endif
static void _normalize_color(float *const restrict pixel, const float norm)
{
  // color may not be black!
  const float factor = norm / fmaxf(pixel[0], fmaxf(pixel[1], pixel[2]));
  for_each_channel(x)
    pixel[x] *= factor;
}

#ifdef _OPENMP
#pragma omp declare simd aligned(XYZ, sRGB: 16) uniform(norm)
#endif
static inline void _XYZ_to_REC_709_normalized(const float *const restrict XYZ, float *const restrict sRGB,
                                                  const float norm)
{
  dt_XYZ_to_Rec709_D50(XYZ, sRGB);
  _normalize_color(sRGB, norm);
}

#ifdef _OPENMP
#pragma omp declare simd aligned(in, out: 64) uniform(buffsize, alpha)
#endif
static void _channel_display_monochrome(const float *const restrict in, uint8_t *const restrict out,
                                        const size_t buffsize, const float alpha)
{
  const dt_aligned_pixel_t mask_color = { 1.0f, 1.0f, 0.0f }; // yellow; "unused" element enables vectorization

#ifdef _OPENMP
#pragma omp parallel for simd default(none) schedule(static) aligned(in, out: 64) aligned(mask_color: 16) \
    dt_omp_firstprivate(in,out, buffsize, alpha, mask_color)
#endif
  for(size_t j = 0; j < buffsize; j += 4)
  {
    dt_aligned_pixel_t pixel = { in[j + 1], in[j + 1], in[j + 1], in[j + 1] };
    _write_pixel(pixel, out + j, mask_color, in[j + 3] * alpha);
  }
}

#ifdef _OPENMP
#pragma omp declare simd aligned(in, out: 64) uniform(buffsize, alpha, channel)
#endif
static void _channel_display_false_color(const float *const restrict in, uint8_t *const restrict out,
                                         const size_t buffsize, const float alpha,
                                         dt_dev_pixelpipe_display_mask_t channel)
{
  const dt_aligned_pixel_t mask_color = { 1.0f, 1.0f, 0.0f }; // yellow, "unused" element aids vectorization

  switch(channel & DT_DEV_PIXELPIPE_DISPLAY_ANY & ~DT_DEV_PIXELPIPE_DISPLAY_OUTPUT)
  {
    case DT_DEV_PIXELPIPE_DISPLAY_a:
#ifdef _OPENMP
#pragma omp parallel for simd default(none) schedule(static) aligned(in, out: 64) aligned(mask_color: 16) \
    dt_omp_firstprivate(in, out, buffsize, alpha, mask_color)
#endif
      for(size_t j = 0; j < buffsize; j += 4)
      {
        dt_aligned_pixel_t xyz;
        dt_aligned_pixel_t pixel;
        // colors with "a" exceeding the range [-56,56] range will yield colors not representable in sRGB
        const float value = fminf(fmaxf(in[j + 1] * 256.0f - 128.0f, -56.0f), 56.0f);
        const dt_aligned_pixel_t lab = { 79.0f - value * (11.0f / 56.0f), value, 0.0f };
        dt_Lab_to_XYZ(lab, xyz);
        _XYZ_to_REC_709_normalized(xyz, pixel, 0.75f);
        _write_pixel(pixel, out + j, mask_color, in[j + 3] * alpha);
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_b:
#ifdef _OPENMP
#pragma omp parallel for simd default(none) schedule(static) aligned(in, out: 64) aligned(mask_color: 16) \
    dt_omp_firstprivate(in, out, buffsize, alpha, mask_color)
#endif
      for(size_t j = 0; j < buffsize; j += 4)
      {
        dt_aligned_pixel_t xyz, pixel;
        // colors with "b" exceeding the range [-65,65] range will yield colors not representable in sRGB
        const float value = fminf(fmaxf(in[j + 1] * 256.0f - 128.0f, -65.0f), 65.0f);
        const dt_aligned_pixel_t lab = { 60.0f + value * (2.0f / 65.0f), 0.0f, value };
        dt_Lab_to_XYZ(lab, xyz);
        _XYZ_to_REC_709_normalized(xyz, pixel, 0.75f);
        _write_pixel(pixel, out + j, mask_color, in[j + 3] * alpha);
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_R:
#ifdef _OPENMP
#pragma omp parallel for simd default(none) schedule(static) aligned(in, out: 64) aligned(mask_color: 16) \
    dt_omp_firstprivate(in, out, buffsize, alpha, mask_color)
#endif
      for(size_t j = 0; j < buffsize; j += 4)
      {
        const dt_aligned_pixel_t pixel = { in[j + 1], 0.0f, 0.0f, 0.0f };
        _write_pixel(pixel, out + j, mask_color, in[j + 3] * alpha);
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_G:
#ifdef _OPENMP
#pragma omp parallel for simd default(none) schedule(static) aligned(in, out: 64) aligned(mask_color: 16) \
    dt_omp_firstprivate(in, out, buffsize, alpha, mask_color)
#endif
      for(size_t j = 0; j < buffsize; j += 4)
      {
        const dt_aligned_pixel_t pixel = { 0.0f, in[j + 1], 0.0f, 0.0f };
        _write_pixel(pixel, out + j, mask_color, in[j + 3] * alpha);
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_B:
#ifdef _OPENMP
#pragma omp parallel for simd default(none) schedule(static) aligned(in, out: 64) aligned(mask_color: 16) \
    dt_omp_firstprivate(in, out, buffsize, alpha, mask_color)
#endif
      for(size_t j = 0; j < buffsize; j += 4)
      {
        const dt_aligned_pixel_t pixel = { 0.0f, 0.0f, in[j + 1], 0.0f };
        _write_pixel(pixel, out + j, mask_color, in[j + 3] * alpha);
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_LCH_C:
    case DT_DEV_PIXELPIPE_DISPLAY_HSL_S:
    case DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_Cz:
#ifdef _OPENMP
#pragma omp parallel for simd default(none) schedule(static) aligned(in, out: 64) aligned(mask_color: 16) \
    dt_omp_firstprivate(in, out, buffsize, alpha, mask_color)
#endif
      for(size_t j = 0; j < buffsize; j += 4)
      {
        const dt_aligned_pixel_t pixel = { 0.5f, 0.5f * (1.0f - in[j + 1]), 0.5f, 0.0f };
        _write_pixel(pixel, out + j, mask_color, in[j + 3] * alpha);
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_LCH_h:
#ifdef _OPENMP
#pragma omp parallel for simd default(none) schedule(static) aligned(in, out: 64) aligned(mask_color: 16) \
    dt_omp_firstprivate(in, out, buffsize, alpha, mask_color)
#endif
      for(size_t j = 0; j < buffsize; j += 4)
      {
        dt_aligned_pixel_t lch = { 65.0f, 37.0f, in[j + 1], 0.0f };
        dt_aligned_pixel_t lab, xyz, pixel;
        dt_LCH_2_Lab(lch, lab);
        dt_Lab_to_XYZ(lab, xyz);
        _XYZ_to_REC_709_normalized(xyz, pixel, 0.75f);
        _write_pixel(pixel, out + j, mask_color, in[j + 3] * alpha);
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_HSL_H:
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) \
    dt_omp_firstprivate(in, out, buffsize, alpha, mask_color)
#endif
      for(size_t j = 0; j < buffsize; j += 4)
      {
        dt_aligned_pixel_t hsl = { in[j + 1], 0.5f, 0.5f, 0.0f };
        dt_aligned_pixel_t pixel;
        dt_HSL_2_RGB(hsl, pixel);
        _normalize_color(pixel, 0.75f);
        _write_pixel(pixel, out + j, mask_color, in[j + 3] * alpha);
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_hz:
#ifdef _OPENMP
#pragma omp parallel for simd default(none) schedule(static) aligned(in, out: 64) aligned(mask_color: 16) \
    dt_omp_firstprivate(in, out, buffsize, alpha, mask_color)
#endif
      for(size_t j = 0; j < buffsize; j += 4)
      {
        const dt_aligned_pixel_t JzCzhz = { 0.011f, 0.01f, in[j + 1] };
        dt_aligned_pixel_t JzAzBz;
        dt_aligned_pixel_t XYZ_D65;
        dt_aligned_pixel_t pixel;
        dt_JzCzhz_2_JzAzBz(JzCzhz, JzAzBz);
        dt_JzAzBz_2_XYZ(JzAzBz, XYZ_D65);
        dt_XYZ_to_Rec709_D65(XYZ_D65, pixel);
        _normalize_color(pixel, 0.75f);
        _write_pixel(pixel, out + j, mask_color, in[j + 3] * alpha);
      }
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_L:
    case DT_DEV_PIXELPIPE_DISPLAY_GRAY:
    case DT_DEV_PIXELPIPE_DISPLAY_HSL_l:
    case DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_Jz:
    default:
      _channel_display_monochrome(in, out, buffsize, alpha);
      break;
  }
}

#ifdef _OPENMP
#pragma omp declare simd aligned(in, out: 64) uniform(buffsize, alpha)
#endif
static void _mask_display(const float *const restrict in, uint8_t *const restrict out, const size_t buffsize,
                          const float alpha)
{
  const dt_aligned_pixel_t mask_color = { 1.0f, 1.0f, 0.0f, 0.0f };

  #ifdef _OPENMP
  #pragma omp parallel for simd default(none) schedule(static) aligned(in, out: 64) aligned(mask_color: 16) \
      dt_omp_firstprivate(in, out, buffsize, alpha, mask_color)
  #endif
    for(size_t j = 0; j < buffsize; j+= 4)
    {
      const float gray = 0.3f * in[j + 0] + 0.59f * in[j + 1] + 0.11f * in[j + 2];
      const dt_aligned_pixel_t pixel = { gray, gray, gray, gray };
      const float hide = 1.0f - fminf(fmaxf(in[j + 3] * alpha, 0.0f), 1.0f);
      _write_pixel(pixel, out + j, mask_color, hide);
    }
}

#ifdef _OPENMP
#pragma omp declare simd aligned(in, out: 64) uniform(buffsize)
#endif
static void _copy_output(const float *const restrict in, uint8_t *const restrict out, const size_t buffsize)
{
#ifdef _OPENMP
#pragma omp parallel for simd default(none) schedule(static) aligned(in, out: 64) \
    dt_omp_firstprivate(in, out, buffsize)
#endif
  for(size_t j = 0; j < buffsize; j += 4)
  {
    // the output of this module is BGR(A) instead of RGBA, so we can't use for_each_channel
    for(size_t c = 0; c < 3; c++)
    {
      out[j + 2 - c] = (uint8_t)(fminf(fmaxf(roundf(255.0f * in[j + c]), 0.0f), 255.0f));
    }
  }
}


int process(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece, const void *const i, void *const o)
{
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  const dt_dev_pixelpipe_display_mask_t mask_display = pipe->mask_display;
  const gboolean fcolor = dt_conf_is_equal("channel_display", "false color");

  const size_t buffsize = (size_t)roi_out->width * roi_out->height * 4;
  const float alpha = (mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) ? 1.0f : 0.0f;

  if((mask_display & DT_DEV_PIXELPIPE_DISPLAY_CHANNEL) && (mask_display & DT_DEV_PIXELPIPE_DISPLAY_ANY))
  {
    if(fcolor)
    {
      _channel_display_false_color((const float *const restrict)i, (uint8_t *const restrict)o, buffsize, alpha,
                                   mask_display);
    }
    else
    {
      _channel_display_monochrome((const float *const restrict)i, (uint8_t *const restrict)o, buffsize, alpha);
    }
  }
  else if(mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
  {
    _mask_display((const float *const restrict)i, (uint8_t *const restrict)o, buffsize, 1.0f);
  }
  else
  {
    _copy_output((const float *const restrict)i, (uint8_t *const restrict)o, buffsize);
  }

  return 0;
}

#ifdef HAVE_OPENCL
static int _false_color_channel_to_kernel_code(const dt_dev_pixelpipe_display_mask_t mask_display)
{
  switch(mask_display & DT_DEV_PIXELPIPE_DISPLAY_ANY & ~DT_DEV_PIXELPIPE_DISPLAY_OUTPUT)
  {
    case DT_DEV_PIXELPIPE_DISPLAY_a:
      return DT_IOP_GAMMA_FALSE_COLOR_A;
    case DT_DEV_PIXELPIPE_DISPLAY_b:
      return DT_IOP_GAMMA_FALSE_COLOR_B;
    case DT_DEV_PIXELPIPE_DISPLAY_R:
      return DT_IOP_GAMMA_FALSE_COLOR_R;
    case DT_DEV_PIXELPIPE_DISPLAY_G:
      return DT_IOP_GAMMA_FALSE_COLOR_G;
    case DT_DEV_PIXELPIPE_DISPLAY_B:
      return DT_IOP_GAMMA_FALSE_COLOR_B_CH;
    case DT_DEV_PIXELPIPE_DISPLAY_LCH_C:
    case DT_DEV_PIXELPIPE_DISPLAY_HSL_S:
    case DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_Cz:
      return DT_IOP_GAMMA_FALSE_COLOR_C;
    case DT_DEV_PIXELPIPE_DISPLAY_LCH_h:
      return DT_IOP_GAMMA_FALSE_COLOR_LCH_H;
    case DT_DEV_PIXELPIPE_DISPLAY_HSL_H:
      return DT_IOP_GAMMA_FALSE_COLOR_HSL_H;
    case DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_hz:
      return DT_IOP_GAMMA_FALSE_COLOR_JZ_HZ;
    case DT_DEV_PIXELPIPE_DISPLAY_L:
    case DT_DEV_PIXELPIPE_DISPLAY_GRAY:
    case DT_DEV_PIXELPIPE_DISPLAY_HSL_l:
    case DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_Jz:
    default:
      return DT_IOP_GAMMA_FALSE_COLOR_MONO;
  }
}

int process_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out)
{
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  dt_iop_gamma_global_data_t *gd = (dt_iop_gamma_global_data_t *)self->global_data;
  const int devid = pipe->devid;
  cl_int err = CL_SUCCESS;

  const int width = roi_out->width;
  const int height = roi_out->height;

  size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };

  const dt_dev_pixelpipe_display_mask_t mask_display = pipe->mask_display;
  const gboolean fcolor = dt_conf_is_equal("channel_display", "false color");
  int mode = DT_IOP_GAMMA_KERNEL_COPY;
  int channel = DT_IOP_GAMMA_FALSE_COLOR_MONO;
  float alpha = (mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) ? 1.0f : 0.0f;

  if((mask_display & DT_DEV_PIXELPIPE_DISPLAY_CHANNEL)
     && (mask_display & DT_DEV_PIXELPIPE_DISPLAY_ANY))
  {
    if(fcolor)
    {
      mode = DT_IOP_GAMMA_KERNEL_CHANNEL_FALSE_COLOR;
      channel = _false_color_channel_to_kernel_code(mask_display);
    }
    else
    {
      mode = DT_IOP_GAMMA_KERNEL_CHANNEL_MONO;
    }
  }
  else if(mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
  {
    mode = DT_IOP_GAMMA_KERNEL_MASK;
    alpha = 1.0f;
  }
  dt_opencl_set_kernel_arg(devid, gd->kernel_gamma_pack, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_gamma_pack, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_gamma_pack, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_gamma_pack, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_gamma_pack, 4, sizeof(int), (void *)&mode);
  dt_opencl_set_kernel_arg(devid, gd->kernel_gamma_pack, 5, sizeof(int), (void *)&channel);
  dt_opencl_set_kernel_arg(devid, gd->kernel_gamma_pack, 6, sizeof(float), (void *)&alpha);

  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_gamma_pack, sizes);
  if(err == CL_SUCCESS) return TRUE;

  dt_print(DT_DEBUG_OPENCL, "[opencl_gamma] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

#ifdef HAVE_OPENCL
void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_gamma_global_data_t *gd = (dt_iop_gamma_global_data_t *)calloc(1, sizeof(dt_iop_gamma_global_data_t));
  if(!gd) return;
  module->data = gd;
  gd->kernel_gamma_pack = dt_opencl_create_kernel(program, "gamma_pack");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_gamma_global_data_t *gd = (dt_iop_gamma_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_gamma_pack);
  dt_free(module->data);
}
#endif

void init(dt_iop_module_t *module)
{
  // module->data = malloc(sizeof(dt_iop_gamma_data_t));
  module->params = calloc(1, sizeof(dt_iop_gamma_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_gamma_params_t));
  module->params_size = sizeof(dt_iop_gamma_params_t);
  module->gui_data = NULL;
  module->hide_enable_button = 1;
  module->default_enabled = 1;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = NULL;
  piece->data_size = 0;
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  // Only GUI pipes return 8 bits unsigned integer BGRA
  if(pipe->type == DT_DEV_PIXELPIPE_PREVIEW || pipe->type == DT_DEV_PIXELPIPE_FULL)
    piece->enabled = 1;
  else
    piece->enabled = 0;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
