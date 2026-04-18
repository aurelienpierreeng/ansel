/*
    This file is part of darktable,
    Copyright (C) 2020 Harold le Clément de Saint-Marcq.
    Copyright (C) 2020-2021 Ralf Brown.
    Copyright (C) 2021 Hubert Kowalski.
    Copyright (C) 2021 Pascal Obry.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2026 Aurélien PIERRE.
    
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

#include "common/imagebuf.h"
#include "common/math.h"
#include "develop/blend.h"
#include "develop/imageop.h"
#include "develop/openmp_maths.h"
#include <math.h>


typedef void(_blend_row_func)(const float *const restrict a, const float *const restrict b,
                              float *const restrict out, const float *const restrict mask, const size_t stride);


void dt_develop_blendif_raw_make_mask(const struct dt_dev_pixelpipe_iop_t *piece, const float *const restrict a,
                                      const float *const restrict b, float *const restrict mask)
{
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  const dt_develop_blend_params_t *const d = (const dt_develop_blend_params_t *const)piece->blendop_data;

  if(piece->dsc_in.channels != 1) return;

  const int owidth = roi_out->width;
  const int oheight = roi_out->height;
  const size_t buffsize = (size_t)owidth * oheight;

  // get the clipped opacity value  0 - 1
  const float global_opacity = fminf(fmaxf(0.0f, (d->opacity / 100.0f)), 1.0f);

  // get parametric mask (if any) and apply global opacity
  if(d->mask_combine & DEVELOP_COMBINE_INV)
  {
    __OMP_FOR_SIMD__(aligned(mask: 64))
    for(size_t x = 0; x < buffsize; x++) mask[x] = global_opacity * (1.0f - mask[x]);
  }
  else
  {
    dt_iop_image_mul_const(mask,global_opacity,owidth,oheight,1); // mask[k] *= global_opacity;
  }
}


/* normal blend with clamping */
__OMP_DECLARE_SIMD__(aligned(a, b, out:16) uniform(stride))
static void _blend_normal_bounded(const float *const restrict a, const float *const restrict b,
                                  float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    out[j] = clamp_simd(a[j] * (1.0f - local_opacity) + b[j] * local_opacity);
  }
}

/* normal blend without any clamping */
__OMP_DECLARE_SIMD__(aligned(a, b, out:16) uniform(stride))
static void _blend_normal_unbounded(const float *const restrict a, const float *const restrict b,
                                    float *const restrict out, const float *const restrict mask,
                                    const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    out[j] = a[j] * (1.0f - local_opacity) + b[j] * local_opacity;
  }
}

/* lighten */
__OMP_DECLARE_SIMD__(aligned(a, b, out:16) uniform(stride))
static void _blend_lighten(const float *const restrict a, const float *const restrict b,
                           float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    out[j] = clamp_simd(a[j] * (1.0f - local_opacity) + fmaxf(a[j], b[j]) * local_opacity);
  }
}

/* darken */
__OMP_DECLARE_SIMD__(aligned(a, b, out:16) uniform(stride))
static void _blend_darken(const float *const restrict a, const float *const restrict b,
                          float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    out[j] = clamp_simd(a[j] * (1.0f - local_opacity) + fminf(a[j], b[j]) * local_opacity);
  }
}

/* multiply */
__OMP_DECLARE_SIMD__(aligned(a, b, out:16) uniform(stride))
static void _blend_multiply(const float *const restrict a, const float *const restrict b,
                            float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    out[j] = clamp_simd(a[j] * (1.0f - local_opacity) + (a[j] * b[j]) * local_opacity);
  }
}

/* average */
__OMP_DECLARE_SIMD__(aligned(a, b, out:16) uniform(stride))
static void _blend_average(const float *const restrict a, const float *const restrict b,
                           float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    out[j] = clamp_simd(a[j] * (1.0f - local_opacity) + (a[j] + b[j]) / 2.0f * local_opacity);
  }
}

/* add */
__OMP_DECLARE_SIMD__(aligned(a, b, out:16) uniform(stride))
static void _blend_add(const float *const restrict a, const float *const restrict b,
                       float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    out[j] = clamp_simd(a[j] * (1.0f - local_opacity) + (a[j] + b[j]) * local_opacity);
  }
}

/* subtract */
__OMP_DECLARE_SIMD__(aligned(a, b, out:16) uniform(stride))
static void _blend_subtract(const float *const restrict a, const float *const restrict b,
                            float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    out[j] = clamp_simd(a[j] * (1.0f - local_opacity) + ((b[j] + a[j]) - 1.0f) * local_opacity);
  }
}

/* difference */
__OMP_DECLARE_SIMD__(aligned(a, b, out:16) uniform(stride))
static void _blend_difference(const float *const restrict a, const float *const restrict b,
                              float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    out[j] = clamp_simd(a[j] * (1.0f - local_opacity) + fabsf(a[j] - b[j]) * local_opacity);
  }
}

/* screen */
__OMP_DECLARE_SIMD__(aligned(a, b, out:16) uniform(stride))
static void _blend_screen(const float *const restrict a, const float *const restrict b,
                          float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    const float la = clamp_simd(a[j]);
    const float lb = clamp_simd(b[j]);
    out[j] = clamp_simd(la * (1.0f - local_opacity) + (1.0f - (1.0f - la) * (1.0f - lb)) * local_opacity);
  }
}

/* overlay */
__OMP_DECLARE_SIMD__(aligned(a, b, out:16) uniform(stride))
static void _blend_overlay(const float *const restrict a, const float *const restrict b,
                           float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    const float local_opacity2 = local_opacity * local_opacity;
    const float la = clamp_simd(a[j]);
    const float lb = clamp_simd(b[j]);
    out[j] = clamp_simd(
        la * (1.0f - local_opacity2)
        + (la > 0.5f ? 1.0f - (1.0f - 2.0f * (la - 0.5f)) * (1.0f - lb) : 2.0f * la * lb) * local_opacity2);
  }
}

/* softlight */
__OMP_DECLARE_SIMD__(aligned(a, b, out:16) uniform(stride))
static void _blend_softlight(const float *const restrict a, const float *const restrict b,
                             float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    const float local_opacity2 = local_opacity * local_opacity;
    const float la = clamp_simd(a[j]);
    const float lb = clamp_simd(b[j]);
    out[j] = clamp_simd(
        la * (1.0f - local_opacity2)
        + (lb > 0.5f ? 1.0f - (1.0f - la) * (1.0f - (lb - 0.5f)) : la * (lb + 0.5f)) * local_opacity2);
  }
}

/* hardlight */
__OMP_DECLARE_SIMD__(aligned(a, b, out:16) uniform(stride))
static void _blend_hardlight(const float *const restrict a, const float *const restrict b,
                             float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    const float local_opacity2 = local_opacity * local_opacity;
    const float la = clamp_simd(a[j]);
    const float lb = clamp_simd(b[j]);
    out[j] = clamp_simd(
        la * (1.0f - local_opacity2)
        + (lb > 0.5f ? 1.0f - (1.0f - 2.0f * (la - 0.5f)) * (1.0f - lb) : 2.0f * la * lb) * local_opacity2);
  }
}

/* vividlight */
__OMP_DECLARE_SIMD__(aligned(a, b, out:16) uniform(stride))
static void _blend_vividlight(const float *const restrict a, const float *const restrict b,
                              float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    const float local_opacity2 = local_opacity * local_opacity;
    const float la = clamp_simd(a[j]);
    const float lb = clamp_simd(b[j]);
    out[j] = clamp_simd(
        la * (1.0f - local_opacity2)
        + (lb > 0.5f ? (lb >= 1.0f ? 1.0f : la / (2.0f * (1.0f - lb)))
                     : (lb <= 0.0f ? 0.0f : 1.0f - (1.0f - la) / (2.0f * lb)))
          * local_opacity2);
  }
}

/* linearlight */
__OMP_DECLARE_SIMD__(aligned(a, b, out:16) uniform(stride))
static void _blend_linearlight(const float *const restrict a, const float *const restrict b,
                               float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    const float local_opacity2 = local_opacity * local_opacity;
    const float la = clamp_simd(a[j]);
    const float lb = clamp_simd(b[j]);
    out[j] = clamp_simd(la * (1.0f - local_opacity2) + (la + 2.0f * lb - 1.0f) * local_opacity2);
  }
}

/* pinlight */
__OMP_DECLARE_SIMD__(aligned(a, b, out:16) uniform(stride))
static void _blend_pinlight(const float *const restrict a, const float *const restrict b,
                            float *const restrict out, const float *const restrict mask, const size_t stride)
{
  for(size_t j = 0; j < stride; j++)
  {
    const float local_opacity = mask[j];
    const float local_opacity2 = local_opacity * local_opacity;
    const float la = clamp_simd(a[j]);
    const float lb = clamp_simd(b[j]);
    out[j] = clamp_simd(
        la * (1.0f - local_opacity2)
        + (lb > 0.5f ? fmaxf(la, 2.0f * (lb - 0.5f)) : fminf(la, 2.0f * lb)) * local_opacity2);
  }
}


static _blend_row_func *_choose_blend_func(const unsigned int blend_mode)
{
  _blend_row_func *blend = NULL;

  /* select the blend operator */
  switch(blend_mode & DEVELOP_BLEND_MODE_MASK)
  {
    case DEVELOP_BLEND_LIGHTEN:
      blend = _blend_lighten;
      break;
    case DEVELOP_BLEND_DARKEN:
      blend = _blend_darken;
      break;
    case DEVELOP_BLEND_MULTIPLY:
      blend = _blend_multiply;
      break;
    case DEVELOP_BLEND_AVERAGE:
      blend = _blend_average;
      break;
    case DEVELOP_BLEND_ADD:
      blend = _blend_add;
      break;
    case DEVELOP_BLEND_SUBTRACT:
      blend = _blend_subtract;
      break;
    case DEVELOP_BLEND_DIFFERENCE:
    case DEVELOP_BLEND_DIFFERENCE2:
      blend = _blend_difference;
      break;
    case DEVELOP_BLEND_SCREEN:
      blend = _blend_screen;
      break;
    case DEVELOP_BLEND_OVERLAY:
      blend = _blend_overlay;
      break;
    case DEVELOP_BLEND_SOFTLIGHT:
      blend = _blend_softlight;
      break;
    case DEVELOP_BLEND_HARDLIGHT:
      blend = _blend_hardlight;
      break;
    case DEVELOP_BLEND_VIVIDLIGHT:
      blend = _blend_vividlight;
      break;
    case DEVELOP_BLEND_LINEARLIGHT:
      blend = _blend_linearlight;
      break;
    case DEVELOP_BLEND_PINLIGHT:
      blend = _blend_pinlight;
      break;
    case DEVELOP_BLEND_BOUNDED:
      blend = _blend_normal_bounded;
      break;

    /* fallback to normal blend */
    case DEVELOP_BLEND_NORMAL2:
    default:
      blend = _blend_normal_unbounded;
      break;
  }

  return blend;
}


void dt_develop_blendif_raw_blend(const struct dt_dev_pixelpipe_t *pipe,
                                  const struct dt_dev_pixelpipe_iop_t *piece,
                                  const float *const restrict a, float *const restrict b,
                                  const float *const restrict mask,
                                  const dt_dev_pixelpipe_display_mask_t request_mask_display)
{
  (void)pipe;
  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  const dt_develop_blend_params_t *const d = (const dt_develop_blend_params_t *const)piece->blendop_data;

  if(piece->dsc_in.channels != 1) return;

  const int xoffs = roi_out->x - roi_in->x;
  const int yoffs = roi_out->y - roi_in->y;
  const int iwidth = roi_in->width;
  const int owidth = roi_out->width;
  const int oheight = roi_out->height;

  if(request_mask_display & DT_DEV_PIXELPIPE_DISPLAY_ANY)
  {
    dt_iop_image_fill(b, 0.0f, owidth, oheight, 1); //b[k] = 0.0f;
  }
  else
  {
    _blend_row_func *const blend = _choose_blend_func(d->blend_mode);

    float *tmp_buffer = dt_pixelpipe_cache_alloc_align_float_cache((size_t)owidth * oheight, 0);
    if (!IS_NULL_PTR(tmp_buffer))
    {
      dt_iop_image_copy(tmp_buffer, b, (size_t)owidth * oheight);
      if((d->blend_mode & DEVELOP_BLEND_REVERSE) == DEVELOP_BLEND_REVERSE)
      {
        __OMP_PARALLEL_FOR__()
        for(size_t y = 0; y < oheight; y++)
        {
          const size_t a_start = (y + yoffs) * iwidth + xoffs;
          const size_t bm_start = y * owidth;
          blend(tmp_buffer + bm_start, a + a_start, b + bm_start, mask + bm_start, owidth);
        }
      }
      else
      {
        __OMP_PARALLEL_FOR__()
        for(size_t y = 0; y < oheight; y++)
        {
          const size_t a_start = (y + yoffs) * iwidth + xoffs;
          const size_t bm_start = y * owidth;
          blend(a + a_start, tmp_buffer + bm_start, b + bm_start, mask + bm_start, owidth);
        }
      }
      dt_pixelpipe_cache_free_align(tmp_buffer);
    }
  }
}

// tools/update_modelines.sh
// remove-trailing-space on;
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
