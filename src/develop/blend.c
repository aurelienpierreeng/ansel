/*
    This file is part of darktable,
    Copyright (C) 2011-2012 Henrik Andersson.
    Copyright (C) 2011-2016, 2018-2019 Tobias Ellinghaus.
    Copyright (C) 2011-2014, 2016-2017, 2019 Ulrich Pegelow.
    Copyright (C) 2011 Yclept Nemo.
    Copyright (C) 2012 James C. McPherson.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2013 Aldric Renaudin.
    Copyright (C) 2013 johannes hanika.
    Copyright (C) 2013-2014, 2016 Roman Lebedev.
    Copyright (C) 2017-2020 Heiko Bauke.
    Copyright (C) 2017 luzpaz.
    Copyright (C) 2018-2019 Edgardo Hoszowski.
    Copyright (C) 2018-2021 Pascal Obry.
    Copyright (C) 2019 Andreas Schneider.
    Copyright (C) 2019, 2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2020 Felipe Contreras.
    Copyright (C) 2020 Harold le Clément de Saint-Marcq.
    Copyright (C) 2020-2021 Hubert Kowalski.
    Copyright (C) 2020 U-DESKTOP-HQME86J\marco.
    Copyright (C) 2021-2022 Hanno Schwalm.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Philipp Lutz.
    Copyright (C) 2025 Miguel Moquillon.
    Copyright (C) 2026 Guillaume Stutin.
    
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
#include "common/darktable.h"
#include "blend.h"
#include "common/gaussian.h"
#include "common/guided_filter.h"
#include "common/imagebuf.h"
#include "common/interpolation.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "develop/pixelpipe_hb.h"
#include "develop/tiling.h"
#include "develop/imageop_math.h"
#include <inttypes.h>
#include <math.h>
#include <string.h>

typedef enum _develop_mask_post_processing
{
  DEVELOP_MASK_POST_NONE = 0,
  DEVELOP_MASK_POST_BLUR = 1,
  DEVELOP_MASK_POST_FEATHER_IN = 2,
  DEVELOP_MASK_POST_FEATHER_OUT = 3,
  DEVELOP_MASK_POST_TONE_CURVE = 4,
} _develop_mask_post_processing;

static const char *dt_pipe_type_to_str(dt_dev_pixelpipe_type_t pipe_type)
{
  switch(pipe_type)
  {
    case DT_DEV_PIXELPIPE_PREVIEW:
      return "PREVIEW";
    case DT_DEV_PIXELPIPE_FULL:
      return "FULL";
    case DT_DEV_PIXELPIPE_THUMBNAIL:
      return "THUMBNAIL";
    case DT_DEV_PIXELPIPE_EXPORT:
      return "EXPORT";
    default:
      return "UNKNOWN";
  }
}

static dt_develop_blend_params_t _default_blendop_params
    = { DEVELOP_MASK_SHAPE | DEVELOP_MASK_PARAMETRIC | DEVELOP_MASK_RASTER,
        DEVELOP_BLEND_CS_NONE,
        DEVELOP_BLEND_NORMAL2,
        0.0f,
        100.0f,
        DEVELOP_COMBINE_NORM_EXCL,
        0,
        0,
        0.0f,
        DEVELOP_MASK_GUIDE_IN_AFTER_BLUR,
        0.0f,
        0.0f,
        0.0f,
        0.0f, // detail mask threshold
        { 0, 0, 0 },
        { 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f,
          0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f,
          0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f,
          0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f },
        { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
        { 0 }, 0, 0, FALSE };

static inline dt_develop_blend_colorspace_t _blend_default_module_blend_colorspace(dt_iop_module_t *module,
                                                                                   gboolean is_scene_referred)
{
  if(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
  {
    switch(module->blend_colorspace(module, NULL, NULL))
    {
      case IOP_CS_RAW:
        return DEVELOP_BLEND_CS_RAW;
      case IOP_CS_LAB:
      case IOP_CS_LCH:
        return DEVELOP_BLEND_CS_LAB;
      case IOP_CS_RGB:
      case IOP_CS_RGB_DISPLAY:
        return is_scene_referred ? DEVELOP_BLEND_CS_RGB_SCENE : DEVELOP_BLEND_CS_RGB_DISPLAY;
      case IOP_CS_HSL:
        return DEVELOP_BLEND_CS_RGB_DISPLAY;
      case IOP_CS_JZCZHZ:
        return DEVELOP_BLEND_CS_RGB_SCENE;
      default:
        return DEVELOP_BLEND_CS_NONE;
    }
  }
  else
    return DEVELOP_BLEND_CS_NONE;
}

dt_develop_blend_colorspace_t dt_develop_blend_default_module_blend_colorspace(dt_iop_module_t *module)
{
  return _blend_default_module_blend_colorspace(module, TRUE);
}

static void _blend_init_blendif_boost_parameters(dt_develop_blend_params_t *blend_params,
                                                 dt_develop_blend_colorspace_t cst)
{
  if(cst == DEVELOP_BLEND_CS_RGB_SCENE)
  {
    // update the default boost parameters for Jz and Cz so that the sRGB white is represented by a value
    // "close" to 1.0. sRGB white (R=1.0, G=1.0, B=1.0) after conversion becomes Jz=0.01758 and will be shown
    // as 1.8. In order to allow enough sensitivity in the low values, the boost factor should be set to
    // log2(0.001) = -6.64385619. To keep the minimum boost factor at zero an offset of that value is added in
    // the GUI. To display the initial boost factor at zero, the default value will be set to that value also.
    blend_params->blendif_boost_factors[DEVELOP_BLENDIF_Jz_in] = -6.64385619f;
    blend_params->blendif_boost_factors[DEVELOP_BLENDIF_Cz_in] = -6.64385619f;
    blend_params->blendif_boost_factors[DEVELOP_BLENDIF_Jz_out] = -6.64385619f;
    blend_params->blendif_boost_factors[DEVELOP_BLENDIF_Cz_out] = -6.64385619f;
  }
}

void dt_develop_blend_init_blend_parameters(dt_develop_blend_params_t *blend_params,
                                            dt_develop_blend_colorspace_t cst)
{
  memcpy(blend_params, &_default_blendop_params, sizeof(dt_develop_blend_params_t));
  blend_params->blend_cst = cst;
  _blend_init_blendif_boost_parameters(blend_params, cst);
}

void dt_develop_blend_init_blendif_parameters(dt_develop_blend_params_t *blend_params,
                                              dt_develop_blend_colorspace_t cst)
{
  blend_params->blend_cst = cst;
  blend_params->blend_mode = _default_blendop_params.blend_mode;
  blend_params->blend_parameter = _default_blendop_params.blend_parameter;
  blend_params->blendif = _default_blendop_params.blendif;
  memcpy(blend_params->blendif_parameters, _default_blendop_params.blendif_parameters,
         sizeof(_default_blendop_params.blendif_parameters));
  memcpy(blend_params->blendif_boost_factors, _default_blendop_params.blendif_boost_factors,
         sizeof(_default_blendop_params.blendif_boost_factors));
  _blend_init_blendif_boost_parameters(blend_params, cst);
}

dt_iop_colorspace_type_t dt_develop_blend_colorspace(const dt_dev_pixelpipe_iop_t *const piece,
                                                     dt_iop_colorspace_type_t cst)
{
  const dt_develop_blend_params_t *const bp = (const dt_develop_blend_params_t *)piece->blendop_data;
  if(IS_NULL_PTR(bp)) return cst;
  switch(bp->blend_cst)
  {
    case DEVELOP_BLEND_CS_RAW:
      return IOP_CS_RAW;
    case DEVELOP_BLEND_CS_LAB:
      return IOP_CS_LAB;
    case DEVELOP_BLEND_CS_RGB_DISPLAY:
    case DEVELOP_BLEND_CS_RGB_SCENE:
      return IOP_CS_RGB;
    default:
      return cst;
  }
}

void dt_develop_blendif_process_parameters(float *const restrict parameters,
                                           const dt_develop_blend_params_t *const params)
{
  const int32_t blend_csp = params->blend_cst;
  const uint32_t blendif = params->blendif;
  const float *blendif_parameters = params->blendif_parameters;
  const float *boost_factors = params->blendif_boost_factors;
  for(size_t i = 0, j = 0; i < DEVELOP_BLENDIF_SIZE; i++, j += DEVELOP_BLENDIF_PARAMETER_ITEMS)
  {
    if(blendif & (1 << i))
    {
      float offset = 0.0f;
      if(blend_csp == DEVELOP_BLEND_CS_LAB && (i == DEVELOP_BLENDIF_A_in || i == DEVELOP_BLENDIF_A_out
          || i == DEVELOP_BLENDIF_B_in || i == DEVELOP_BLENDIF_B_out))
      {
        offset = 0.5f;
      }
      parameters[j + 0] = (blendif_parameters[i * 4 + 0] - offset) * exp2f(boost_factors[i]);
      parameters[j + 1] = (blendif_parameters[i * 4 + 1] - offset) * exp2f(boost_factors[i]);
      parameters[j + 2] = (blendif_parameters[i * 4 + 2] - offset) * exp2f(boost_factors[i]);
      parameters[j + 3] = (blendif_parameters[i * 4 + 3] - offset) * exp2f(boost_factors[i]);
      // pre-compute increasing slope and decreasing slope
      parameters[j + 4] = 1.0f / fmaxf(0.001f, parameters[j + 1] - parameters[j + 0]);
      parameters[j + 5] = 1.0f / fmaxf(0.001f, parameters[j + 3] - parameters[j + 2]);
      // handle the case when one end is open to avoid clipping input/output values
      if(blendif_parameters[i * 4 + 0] <= 0.0f && blendif_parameters[i * 4 + 1] <= 0.0f)
      {
        parameters[j + 0] = -INFINITY;
        parameters[j + 1] = -INFINITY;
      }
      if(blendif_parameters[i * 4 + 2] >= 1.0f && blendif_parameters[i * 4 + 3] >= 1.0f)
      {
        parameters[j + 2] = INFINITY;
        parameters[j + 3] = INFINITY;
      }
    }
    else
    {
      parameters[j + 0] = -INFINITY;
      parameters[j + 1] = -INFINITY;
      parameters[j + 2] = INFINITY;
      parameters[j + 3] = INFINITY;
      parameters[j + 4] = 0.0f;
      parameters[j + 5] = 0.0f;
    }
  }
}

void dt_develop_blend_get_mask_usage(const dt_iop_module_t *module,
                                     const dt_develop_blend_params_t *params,
                                     gboolean *top_enabled,
                                     gboolean *raster_used,
                                     gboolean *drawn_used,
                                     gboolean *parametric_used)
{
  if(!IS_NULL_PTR(top_enabled)) *top_enabled = FALSE;
  if(!IS_NULL_PTR(raster_used)) *raster_used = FALSE;
  if(!IS_NULL_PTR(drawn_used)) *drawn_used = FALSE;
  if(!IS_NULL_PTR(parametric_used)) *parametric_used = FALSE;
  if(IS_NULL_PTR(params)) return;

  const uint32_t mask_mode = params->mask_mode;
  const gboolean top = (mask_mode & DEVELOP_MASK_ENABLED) != 0;
  const gboolean raster_mode = (mask_mode & DEVELOP_MASK_RASTER) != 0;
  const gboolean drawn_mode = (mask_mode & DEVELOP_MASK_SHAPE) != 0;
  const gboolean parametric_mode = (mask_mode & DEVELOP_MASK_PARAMETRIC) != 0;

  gboolean raster = raster_mode
                    && (params->raster_mask_id > 0
                        || params->raster_mask_source[0] != '\0'
                        || (!IS_NULL_PTR(module) && !IS_NULL_PTR(module->raster_mask.sink.source)));

  gboolean drawn = FALSE;
  if(drawn_mode && !IS_NULL_PTR(module) && !IS_NULL_PTR(module->dev))
  {
    dt_masks_form_t *grp = dt_masks_get_from_id(module->dev, params->mask_id);
    drawn = !IS_NULL_PTR(grp) && (grp->type & DT_MASKS_GROUP) && g_list_length(grp->points) > 0;
  }

  gboolean parametric = FALSE;
  if(parametric_mode)
  {
    const float threshold_epsilon = 1e-6f;
    const uint32_t channel_mask = params->blend_cst == DEVELOP_BLEND_CS_LAB
                                      ? DEVELOP_BLENDIF_Lab_MASK
                                      : DEVELOP_BLENDIF_RGB_MASK;
    uint32_t active_channels = 0;
    for(uint32_t ch = 0; ch < DEVELOP_BLENDIF_SIZE; ch++)
    {
      const uint32_t bit = 1u << ch;
      if(!(channel_mask & bit) || !(params->blendif & bit)) continue;
      const float *channel = &params->blendif_parameters[ch * 4];
      if(fabsf(channel[0]) > threshold_epsilon
         || fabsf(channel[1]) > threshold_epsilon
         || fabsf(channel[2] - 1.0f) > threshold_epsilon
         || fabsf(channel[3] - 1.0f) > threshold_epsilon)
        active_channels |= bit;
    }
    parametric = active_channels != 0;
  }

  if(!IS_NULL_PTR(top_enabled)) *top_enabled = top;
  if(!IS_NULL_PTR(raster_used)) *raster_used = raster;
  if(!IS_NULL_PTR(drawn_used)) *drawn_used = drawn;
  if(!IS_NULL_PTR(parametric_used)) *parametric_used = parametric;
}

// See function definition in blend.h for important information
int dt_develop_blendif_init_masking_profile(const struct dt_dev_pixelpipe_t *pipe,
                                            const struct dt_dev_pixelpipe_iop_t *piece,
                                            dt_iop_order_iccprofile_info_t *blending_profile,
                                            dt_develop_blend_colorspace_t cst)
{
  // Bradford adaptation matrix from http://www.brucelindbloom.com/index.html?Eqn_ChromAdapt.html
  const dt_colormatrix_t M = {
      {  0.9555766f, -0.0230393f,  0.0631636f, 0.0f },
      { -0.0282895f,  1.0099416f,  0.0210077f, 0.0f },
      {  0.0122982f, -0.0204830f,  1.3299098f, 0.0f },
  };

  const dt_iop_order_iccprofile_info_t *const profile = (cst == DEVELOP_BLEND_CS_RGB_SCENE)
      ? dt_ioppr_get_pipe_current_profile_info(piece->module, pipe)
      : dt_ioppr_get_iop_work_profile_info(piece->module, piece->module->dev->iop);
  if(IS_NULL_PTR(profile)) return 0;

  memcpy(blending_profile, profile, sizeof(dt_iop_order_iccprofile_info_t));
  for(size_t y = 0; y < 3; y++)
  {
    for(size_t x = 0; x < 3; x++)
    {
      float sum = 0.0f;
      for(size_t i = 0; i < 3; i++)
        sum += M[y][i] * profile->matrix_in[i][x];
      blending_profile->matrix_out[y][x] = sum;
      blending_profile->matrix_out_transposed[x][y] = sum;
    }
  }

  return 1;
}

static inline float _detail_mask_threshold(const float level, const gboolean detail)
{
  // this does some range calculation for smoother ui experience
  return 0.005f * (detail ? powf(level, 2.0f) : 1.0f - powf(fabs(level), 0.5f ));
}

static void _refine_with_detail_mask(struct dt_iop_module_t *self, const struct dt_dev_pixelpipe_t *pipe,
                                     const struct dt_dev_pixelpipe_iop_t *piece, float *mask,
                                     const float level)
{
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  if(level == 0.0f) return;
  const gboolean info = ((darktable.unmuted & DT_DEBUG_MASKS) && (pipe->type == DT_DEV_PIXELPIPE_FULL));

  const gboolean detail = (level > 0.0f);
  const float threshold = _detail_mask_threshold(level, detail);

  float *tmp = NULL;
  float *lum = NULL;
  float *warp_mask = NULL;
  float *rawdetail_mask = NULL;

  const dt_dev_pixelpipe_t *p = pipe;
  rawdetail_mask = dt_dev_retrieve_rawdetail_mask(pipe, self);
  if(IS_NULL_PTR(rawdetail_mask)) return;

  const int iwidth  = p->rawdetail_mask_roi.width;
  const int iheight = p->rawdetail_mask_roi.height;
  const int owidth  = roi_out->width;
  const int oheight = roi_out->height;
  if(info) fprintf(stderr, "[_refine_with_detail_mask] in module %s %ix%i --> %ix%i\n", self->op, iwidth, iheight, owidth, oheight);

  const int bufsize = MAX(iwidth * iheight, owidth * oheight);

  tmp = dt_pixelpipe_cache_alloc_align_float(bufsize, pipe);
  lum = dt_pixelpipe_cache_alloc_align_float(bufsize, pipe);
  if((IS_NULL_PTR(tmp)) || (IS_NULL_PTR(lum))) goto error;

  dt_masks_calc_detail_mask(rawdetail_mask, lum, tmp, iwidth, iheight, threshold, detail);
  dt_pixelpipe_cache_free_align(tmp);
  tmp = NULL;

  // here we have the slightly blurred full detail mask available
  warp_mask = dt_dev_distort_detail_mask(p, lum, self);
  // dt_dev_distort_detail_mask() may return `lum` unchanged when no geometric distortion is needed.
  const gboolean warp_mask_aliases_lum = (warp_mask == lum);
  if(!warp_mask_aliases_lum)
  {
    dt_pixelpipe_cache_free_align(lum);
    lum = NULL;
  }

  if(IS_NULL_PTR(warp_mask)) goto error;

  const int msize = owidth * oheight;
  __OMP_PARALLEL_FOR_SIMD__(aligned(mask, warp_mask : 64))
  for(int idx =0; idx < msize; idx++)
  {
    mask[idx] = mask[idx] * warp_mask[idx];
  }
  dt_pixelpipe_cache_free_align(warp_mask);
  if(warp_mask_aliases_lum) lum = NULL;

  return;

  error:
  dt_control_log(_("detail mask blending error"));
  dt_pixelpipe_cache_free_align(warp_mask);
  dt_pixelpipe_cache_free_align(lum);
  dt_pixelpipe_cache_free_align(tmp);
}

static size_t _develop_mask_get_post_operations(const dt_develop_blend_params_t *const params,
                                                const dt_dev_pixelpipe_iop_t *const piece,
                                                _develop_mask_post_processing operations[3])
{
  const gboolean mask_feather = params->feathering_radius > 0.1f && piece->dsc_in.channels >= 3;
  const gboolean mask_blur = params->blur_radius > 0.1f;
  const gboolean mask_tone_curve = fabsf(params->contrast) >= 0.01f || fabsf(params->brightness) >= 0.01f;
  const gboolean mask_feather_before = params->feathering_guide == DEVELOP_MASK_GUIDE_IN_BEFORE_BLUR
                                       || params->feathering_guide == DEVELOP_MASK_GUIDE_OUT_BEFORE_BLUR;
  const gboolean mask_feather_out = params->feathering_guide == DEVELOP_MASK_GUIDE_OUT_BEFORE_BLUR
                                    || params->feathering_guide == DEVELOP_MASK_GUIDE_OUT_AFTER_BLUR;
  const float opacity = fminf(fmaxf(params->opacity / 100.0f, 0.0f), 1.0f);

  memset(operations, 0, sizeof(_develop_mask_post_processing) * 3);
  size_t index = 0;

  if(mask_feather)
  {
    if(mask_blur && mask_feather_before)
    {
      operations[index++] = mask_feather_out ? DEVELOP_MASK_POST_FEATHER_OUT : DEVELOP_MASK_POST_FEATHER_IN;
      operations[index++] = DEVELOP_MASK_POST_BLUR;
    }
    else
    {
      if(mask_blur)
        operations[index++] = DEVELOP_MASK_POST_BLUR;
      operations[index++] = mask_feather_out ? DEVELOP_MASK_POST_FEATHER_OUT : DEVELOP_MASK_POST_FEATHER_IN;
    }
  }
  else if(mask_blur)
  {
    operations[index++] = DEVELOP_MASK_POST_BLUR;
  }

  if(mask_tone_curve && opacity > 1e-4f)
  {
    operations[index++] = DEVELOP_MASK_POST_TONE_CURVE;
  }

  return index;
}


static inline float *_develop_blend_process_copy_region(const float *const restrict input, const size_t iwidth,
                                                        const size_t xoffs, const size_t yoffs,
                                                        const size_t owidth, const size_t oheight)
{
  const size_t ioffset = yoffs * iwidth + xoffs;
  float *const restrict output =
    dt_pixelpipe_cache_alloc_align_float_cache(owidth * oheight, 0);
  if(IS_NULL_PTR(output))
  {
    return NULL;
  }
  __OMP_PARALLEL_FOR__()
  for(size_t y = 0; y < oheight; y++)
  {
    const size_t iindex = y * iwidth + ioffset;
    const size_t oindex = y * owidth;
    memcpy(output + oindex, input + iindex, sizeof(float) * owidth);
  }

  return output;
}

static inline void _develop_blend_process_free_region(float *const restrict input)
{
  dt_pixelpipe_cache_free_align(input);
}

static const dt_iop_module_t *_develop_blend_get_raster_source_module(const dt_develop_blend_params_t *const params,
                                                                      const dt_iop_module_t *const self)
{
  if(IS_NULL_PTR(params) || IS_NULL_PTR(self)) return NULL;

  const dt_iop_module_t *source = self->raster_mask.sink.source;
  if(source && !strcmp(source->op, params->raster_mask_source)
     && source->multi_priority == params->raster_mask_instance)
    return source;

  if(IS_NULL_PTR(self->dev) || params->raster_mask_source[0] == '\0') return NULL;

  for(GList *iter = g_list_first(self->dev->iop); iter; iter = g_list_next(iter))
  {
    const dt_iop_module_t *candidate = (const dt_iop_module_t *)iter->data;
    if(!strcmp(candidate->op, params->raster_mask_source)
       && candidate->multi_priority == params->raster_mask_instance)
      return candidate;
  }

  return NULL;
}

static void _develop_blend_init_raster_mask(const dt_develop_blend_params_t *const params,
                                            dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            const dt_dev_pixelpipe_iop_t *piece,
                                            float *const restrict mask,
                                            const size_t owidth,
                                            const size_t oheight,
                                            int *const raster_error)
{
  int local_raster_error = 0;
  const dt_iop_module_t *raster_source = _develop_blend_get_raster_source_module(params, self);
  float *raster_mask = raster_source
      ? dt_dev_get_raster_mask(pipe, raster_source, params->raster_mask_id,
                               self, &local_raster_error)
      : NULL;

  if(!IS_NULL_PTR(raster_mask))
  {
    if(params->raster_mask_invert)
    {
      __OMP_FOR_SIMD__(aligned(mask, raster_mask:64) )
      for(size_t i = 0; i < owidth * oheight; i++)
        mask[i] = 1.0f - raster_mask[i];
    }
    else
    {
      dt_iop_image_scaled_copy(mask, raster_mask, 1.0f, owidth, oheight, 1);
    }

    dt_pixelpipe_cache_free_align(raster_mask);
  }
  else
  {
    const float value = params->raster_mask_invert ? 0.0f : 1.0f;
    dt_iop_image_fill(mask, value, owidth, oheight, 1);
  }

  if(!IS_NULL_PTR(raster_error)) *raster_error = local_raster_error;
}

static int _develop_blend_init_drawn_mask(const dt_develop_blend_params_t *const params,
                                          dt_iop_module_t *self,
                                          dt_dev_pixelpipe_t *pipe,
                                          const dt_dev_pixelpipe_iop_t *piece,
                                          const struct dt_iop_roi_t *const roi_out,
                                          float *const restrict mask,
                                          const size_t owidth,
                                          const size_t oheight)
{
  dt_masks_form_t *form = dt_masks_get_from_id(self->dev, params->mask_id);

  if(form && (!(self->flags() & IOP_FLAGS_NO_MASKS)) && (params->mask_mode & DEVELOP_MASK_SHAPE))
  {
    if(dt_masks_group_render_roi(self, pipe, piece, form, roi_out, mask) != 0) return 1;

    if(params->mask_combine & DEVELOP_COMBINE_MASKS_POS)
      dt_iop_image_invert(mask, 1.0f, owidth, oheight, 1);
  }
  else if((!(self->flags() & IOP_FLAGS_NO_MASKS)) && (params->mask_mode & DEVELOP_MASK_SHAPE))
  {
    const float fill = (params->mask_combine & DEVELOP_COMBINE_MASKS_POS) ? 0.0f : 1.0f;
    dt_iop_image_fill(mask, fill, owidth, oheight, 1);
  }
  else
  {
    const float fill = (params->mask_combine & DEVELOP_COMBINE_INCL) ? 0.0f : 1.0f;
    dt_iop_image_fill(mask, fill, owidth, oheight, 1);
  }

  return 0;
}

static void _develop_blend_combine_masks(float *const restrict mask,
                                         const float *const restrict other_mask,
                                         const size_t buffsize)
{
  __OMP_FOR_SIMD__(aligned(mask, other_mask:64) )
  for(size_t i = 0; i < buffsize; i++)
    mask[i] *= other_mask[i];
}


static int _develop_blend_process_feather(const float *const guide, float *const mask, const size_t width,
                                          const size_t height, const int ch, const float guide_weight,
                                          const float feathering_radius, const float scale)
{
  const float sqrt_eps = 1.f;
  int w = (int)(2 * feathering_radius * scale + 0.5f);
  if(w < 1) w = 1;

  float *const restrict mask_bak =
    dt_pixelpipe_cache_alloc_align_float_cache( width * height, 0);
  if(IS_NULL_PTR(mask_bak)) return 1;

  memcpy(mask_bak, mask, sizeof(float) * width * height);
  if(guided_filter(guide, mask_bak, mask, width, height, ch, w, sqrt_eps, guide_weight, 0.f, 1.f) != 0)
  {
    dt_pixelpipe_cache_free_align(mask_bak);
    return 1;
  }
  dt_pixelpipe_cache_free_align(mask_bak);
  return 0;
}


static void _develop_blend_process_mask_tone_curve(float *const restrict mask, const size_t buffsize,
                                                   const float contrast, const float brightness,
                                                   const float opacity)
{
  const float mask_epsilon = 16 * FLT_EPSILON;  // empirical mask threshold for fully transparent masks
  const float e = expf(3.f * contrast);
  __OMP_PARALLEL_FOR_SIMD__(aligned(mask:64))
  for(size_t k = 0; k < buffsize; k++)
  {
    float x = mask[k] / opacity;
    x = 2.f * x - 1.f;
    if (1.f - brightness <= 0.f)
      x = mask[k] <= mask_epsilon ? -1.f : 1.f;
    else if (1.f + brightness <= 0.f)
      x = mask[k] >= 1.f - mask_epsilon ? 1.f : -1.f;
    else if (brightness > 0.f)
    {
      x = (x + brightness) / (1.f - brightness);
      x = fminf(x, 1.f);
    }
    else
    {
      x = (x + brightness) / (1.f + brightness);
      x = fmaxf(x, -1.f);
    }
    mask[k] = clamp_range_f(
        ((x * e / (1.f + (e - 1.f) * fabsf(x))) / 2.f + 0.5f) * opacity, 0.f, 1.f);
  }
}


int dt_develop_blend_process(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                             const struct dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
                             void *const ovoid)
{
  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  if(pipe->bypass_blendif && self->dev->gui_attached && (self == self->dev->gui_module)) return 0;

  const dt_develop_blend_params_t *const d = (const dt_develop_blend_params_t *const)piece->blendop_data;
  if(IS_NULL_PTR(d)) return 0;

  gboolean top_enabled = FALSE;
  gboolean raster_used = FALSE;
  gboolean drawn_used = FALSE;
  gboolean parametric_used = FALSE;
  dt_develop_blend_get_mask_usage(self, d, &top_enabled, &raster_used, &drawn_used, &parametric_used);
  if(!top_enabled) return 0;
  // A mask preview request must also be honored when every sub-mask is neutral.
  // In that case no raster/drawn/parametric mask is "used", but the effective
  // blend mask is still the uniform opacity mask written into alpha below.
  const gboolean preview_mask_mode = top_enabled;

  const size_t ch = piece->dsc_in.channels;           // the number of channels in the buffer
  const int xoffs = roi_out->x - roi_in->x;
  const int yoffs = roi_out->y - roi_in->y;
  const int iwidth = roi_in->width;
  const int iheight = roi_in->height;
  const int owidth = roi_out->width;
  const int oheight = roi_out->height;
  const size_t buffsize = (size_t)owidth * oheight;
  const float iscale = roi_in->scale;
  const float oscale = roi_out->scale;
  const gboolean rois_equal = iwidth == owidth || iheight == oheight || xoffs == 0 || yoffs == 0;

  // In most cases of blending-enabled modules input and output of the module have
  // the exact same dimensions. Only in very special cases we allow a module's input
  // to exceed its output. This is namely the case for the spot removal module where
  // the source of a patch might lie outside the roi of the output image. Therefore:
  // We can only handle blending if roi_out and roi_in have the same scale and
  // if roi_out fits into the area given by roi_in. xoffs and yoffs describe the relative
  // offset of the input image to the output image.
  if(oscale != iscale || xoffs < 0 || yoffs < 0
     || ((xoffs > 0 || yoffs > 0) && (owidth + xoffs > iwidth || oheight + yoffs > iheight)))
  {
    dt_control_log(_("skipped blending in module '%s': roi's do not match"), self->op);
    return 0;
  }

  // does user want us to display a specific channel?
  const dt_dev_pixelpipe_display_mask_t request_mask_display =
    (self->dev->gui_attached && (self == self->dev->gui_module) && (pipe == self->dev->pipe)
     && preview_mask_mode)
        ? self->request_mask_display
        : DT_DEV_PIXELPIPE_DISPLAY_NONE;

  // get channel max values depending on colorspace
  const dt_develop_blend_colorspace_t blend_csp = d->blend_cst;
  const dt_iop_colorspace_type_t cst = dt_develop_blend_colorspace(piece, IOP_CS_NONE);

  // obtaining the list of mask operations to perform
  _develop_mask_post_processing post_operations[3];
  const size_t post_operations_size = _develop_mask_get_post_operations(d, piece, post_operations);

  // get the clipped opacity value  0 - 1
  const float opacity = fminf(fmaxf(d->opacity / 100.0f, 0.0f), 1.0f);

  // allocate space for blend mask
  float *const restrict _mask = dt_pixelpipe_cache_alloc_align_float(buffsize, pipe);
  if(IS_NULL_PTR(_mask))
  {
    dt_control_log(_("could not allocate buffer for blending"));
    return 1;
  }
  int raster_error = 0;
  float *const restrict mask = _mask;
  const gboolean use_masks = raster_used || drawn_used || parametric_used;
  const gboolean raster_only = raster_used && !drawn_used && !parametric_used;

  if(!use_masks)
  {
    // blend uniformly (no drawn or parametric mask)
    dt_iop_image_fill(mask,opacity,owidth,oheight,1);  //mask[k] = opacity;
  }
  else if(raster_only)
  {
    /* use a raster mask from another module earlier in the pipe */
    _develop_blend_init_raster_mask(d, self, pipe, piece, mask, owidth, oheight, &raster_error);
    dt_iop_image_mul_const(mask, opacity, owidth, oheight, 1);
  }
  else
  {
    if(!raster_used && !drawn_used)
    {
      // Parametric-only blending has no raster/drawn form mask to seed `mask`.
      // The combination code below loops on parametric channels and composes them
      // with that form mask, so initialize it as the neutral value of the chosen
      // mask-combine operator.
      const float fill = (d->mask_combine & DEVELOP_COMBINE_INCL) ? 0.0f : 1.0f;
      dt_iop_image_fill(mask, fill, owidth, oheight, 1);
    }
    else if(raster_used)
    {
      _develop_blend_init_raster_mask(d, self, pipe, piece, mask, owidth, oheight, &raster_error);

      if(drawn_used)
      {
        float *const restrict drawn_mask = dt_pixelpipe_cache_alloc_align_float(buffsize, pipe);
        if(IS_NULL_PTR(drawn_mask))
        {
          dt_control_log(_("could not allocate buffer for blending"));
          dt_pixelpipe_cache_free_align(_mask);
          return 1;
        }

        if(_develop_blend_init_drawn_mask(d, self, pipe, piece, roi_out, drawn_mask, owidth, oheight) != 0)
        {
          dt_pixelpipe_cache_free_align(drawn_mask);
          dt_pixelpipe_cache_free_align(_mask);
          return 1;
        }

        _develop_blend_combine_masks(mask, drawn_mask, buffsize);
        dt_pixelpipe_cache_free_align(drawn_mask);
      }
    }
    else if(drawn_used
            && _develop_blend_init_drawn_mask(d, self, pipe, piece, roi_out, mask, owidth, oheight) != 0)
    {
      dt_pixelpipe_cache_free_align(_mask);
      return 1;
    }

    _refine_with_detail_mask(self, pipe, piece, mask, d->details);

    // get parametric mask (if any) and apply global opacity
    switch(blend_csp)
    {
      case DEVELOP_BLEND_CS_LAB:
        dt_develop_blendif_lab_make_mask(piece, (const float *const restrict)ivoid,
                                         (const float *const restrict)ovoid, mask);
        break;
      case DEVELOP_BLEND_CS_RGB_DISPLAY:
        dt_develop_blendif_rgb_hsl_make_mask(pipe, piece, (const float *const restrict)ivoid,
                                             (const float *const restrict)ovoid, mask);
        break;
      case DEVELOP_BLEND_CS_RGB_SCENE:
        dt_develop_blendif_rgb_jzczhz_make_mask(pipe, piece, (const float *const restrict)ivoid,
                                                (const float *const restrict)ovoid, mask);
        break;
      case DEVELOP_BLEND_CS_RAW:
        dt_develop_blendif_raw_make_mask(piece, (const float *const restrict)ivoid,
                                         (const float *const restrict)ovoid, mask);
        break;
      default:
        break;
    }

    // post processing the mask
    for(size_t index = 0; index < post_operations_size; ++index)
    {
      _develop_mask_post_processing operation = post_operations[index];
      if(operation == DEVELOP_MASK_POST_FEATHER_IN)
      {
        const float guide_weight = dt_iop_colorspace_is_rgb(cst) ? 100.0f : 1.0f;
        float *restrict guide = (float *restrict)ivoid;
        if(!rois_equal)
          guide = _develop_blend_process_copy_region(guide, ch * iwidth, ch * xoffs, ch * yoffs,
                                                     ch * owidth, ch * oheight);
        if(!guide)
        {
          dt_pixelpipe_cache_free_align(_mask);
          return 1;
        }
        if(_develop_blend_process_feather(guide, mask, owidth, oheight, ch, guide_weight,
                                          d->feathering_radius, roi_out->scale) != 0)
        {
          if(!rois_equal)
            _develop_blend_process_free_region(guide);
          dt_pixelpipe_cache_free_align(_mask);
          return 1;
        }
        if(!rois_equal)
          _develop_blend_process_free_region(guide);
      }
      else if(operation == DEVELOP_MASK_POST_FEATHER_OUT)
      {
        const float guide_weight = dt_iop_colorspace_is_rgb(cst) ? 100.0f : 1.0f;
        if(_develop_blend_process_feather((const float *const restrict)ovoid, mask, owidth, oheight, ch,
                                          guide_weight, d->feathering_radius, roi_out->scale) != 0)
        {
          dt_pixelpipe_cache_free_align(_mask);
          return 1;
        }
      }
      else if(operation == DEVELOP_MASK_POST_BLUR)
      {
        const float sigma = d->blur_radius * roi_out->scale;
        const float mmax[] = { 1.0f };
        const float mmin[] = { 0.0f };

        dt_gaussian_t *g = dt_gaussian_init(owidth, oheight, 1, mmax, mmin, sigma, 0);
        if(g)
        {
          dt_gaussian_blur(g, mask, mask);
          dt_gaussian_free(g);
        }
      }
      else if(operation == DEVELOP_MASK_POST_TONE_CURVE)
      {
        _develop_blend_process_mask_tone_curve(mask, buffsize, d->contrast, d->brightness, opacity);
      }
    }
  }

  // now apply blending with per-pixel opacity value as defined in mask
  // select the blend operator
  switch(blend_csp)
  {
    case DEVELOP_BLEND_CS_LAB:
      dt_develop_blendif_lab_blend(pipe, piece, (const float *const restrict)ivoid, (float *const restrict)ovoid,
                                   mask, request_mask_display);
      break;
    case DEVELOP_BLEND_CS_RGB_DISPLAY:
      dt_develop_blendif_rgb_hsl_blend(pipe, piece, (const float *const restrict)ivoid, (float *const restrict)ovoid,
                                       mask, request_mask_display);
      break;
    case DEVELOP_BLEND_CS_RGB_SCENE:
      dt_develop_blendif_rgb_jzczhz_blend(pipe, piece, (const float *const restrict)ivoid, (float *const restrict)ovoid,
                                          mask, request_mask_display);
      break;
    case DEVELOP_BLEND_CS_RAW:
      dt_develop_blendif_raw_blend(pipe, piece, (const float *const restrict)ivoid, (float *const restrict)ovoid,
                                   mask, request_mask_display);
      break;
    default:
      break;
  }

  // register if _this_ module should expose mask or display channel
  if(request_mask_display & (DT_DEV_PIXELPIPE_DISPLAY_MASK | DT_DEV_PIXELPIPE_DISPLAY_CHANNEL))
  {
    pipe->mask_display = request_mask_display;
  }

  // check if we should store the mask for export or use in subsequent modules
  if(pipe->store_all_raster_masks || dt_iop_is_raster_mask_used(self, 0))
  {
    const uint64_t mask_hash = dt_dev_pixelpipe_raster_mask_hash(piece, 0);
    dt_pixel_cache_entry_t *mask_entry = NULL;
    void *cache_data = NULL;
    const int created = dt_dev_pixelpipe_cache_get(
        darktable.pixelpipe_cache, mask_hash, sizeof(float) * buffsize,
        "raster mask", pipe->type, TRUE, &cache_data, &mask_entry);

    if(IS_NULL_PTR(cache_data) || IS_NULL_PTR(mask_entry))
    {
      if(created && !IS_NULL_PTR(mask_entry))
        dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, FALSE, mask_entry);
      if(!IS_NULL_PTR(mask_entry))
      {
        dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, FALSE, mask_entry);
        if(created)
          dt_dev_pixelpipe_cache_remove(darktable.pixelpipe_cache, TRUE, mask_entry);
      }
      dt_pixelpipe_cache_free_align(_mask);
      return 1;
    }

    // Cache entries are immutable for a given provider state. Only the thread
    // which created this dedicated key writes the mask; exact hits reuse it.
    if(created)
    {
      memcpy(cache_data, _mask, sizeof(float) * buffsize);
      dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, FALSE, mask_entry);
    }
    // Transfer cache_get()'s reference to the pipe. It keeps this side-band
    // output alive until the next graph state is prepared or the pipe closes.
    g_array_append_val(pipe->raster_mask_hashes, mask_hash);
    dt_pixelpipe_cache_free_align(_mask);

    dt_print(DT_DEBUG_MASKS,
             "[raster masks] %s mask id 0 for module %s (%s) for pipe %s"
             " with cache hash %" PRIu64 "\n",
             created ? "published" : "reused cached",
             piece->module->op, piece->module->multi_name,
             dt_pipe_type_to_str(pipe->type), mask_hash);
  }
  else
  {
    dt_print(DT_DEBUG_MASKS,
             "[raster masks] discarding unpublished mask id 0 for module %s (%s) for pipe %s\n",
             piece->module->op, piece->module->multi_name,
             dt_pipe_type_to_str(pipe->type));
    dt_pixelpipe_cache_free_align(_mask);
  }
  // raster error is the only one we catch
  return raster_error;
}

#ifdef HAVE_OPENCL
static void _refine_with_detail_mask_cl(struct dt_iop_module_t *self, const struct dt_dev_pixelpipe_t *pipe,
                                        const struct dt_dev_pixelpipe_iop_t *piece, float *mask,
                                        const float level, const int devid)
{
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  if(level == 0.0f) return;
  const gboolean info = (darktable.unmuted & DT_DEBUG_MASKS);

  const int detail = (level > 0.0f);
  const float threshold = _detail_mask_threshold(level, detail);
  float *lum = NULL;
  float *rawdetail_mask = NULL;
  cl_mem tmp = NULL;
  cl_mem blur = NULL;
  cl_mem out = NULL;

  const dt_dev_pixelpipe_t *p = pipe;
  rawdetail_mask = dt_dev_retrieve_rawdetail_mask(pipe, self);
  if(IS_NULL_PTR(rawdetail_mask)) return;

  const int iwidth  = p->rawdetail_mask_roi.width;
  const int iheight = p->rawdetail_mask_roi.height;
  const int owidth  = roi_out->width;
  const int oheight = roi_out->height;
  if(info) fprintf(stderr, "[_refine_with_detail_mask_cl] in module %s %ix%i --> %ix%i\n", self->op, iwidth, iheight, owidth, oheight);

  lum = dt_pixelpipe_cache_alloc_align_float((size_t)iwidth * iheight, pipe);
  if(IS_NULL_PTR(lum)) goto error;
  tmp = dt_opencl_alloc_device(devid, iwidth, iheight, sizeof(float));
  if(IS_NULL_PTR(tmp)) goto error;
  out = dt_opencl_alloc_device_buffer(devid, sizeof(float) * iwidth * iheight);
  if(IS_NULL_PTR(out)) goto error;
  blur = dt_opencl_alloc_device_buffer(devid, sizeof(float) * iwidth * iheight);
  if(IS_NULL_PTR(blur)) goto error;

  {
    const int err = dt_opencl_write_host_to_device(devid, rawdetail_mask, tmp, iwidth, iheight, sizeof(float));
    if(err != CL_SUCCESS) goto error;
  }

  {
    size_t sizes[3] = { ROUNDUPDWD(iwidth, devid), ROUNDUPDHT(iheight, devid), 1 };
    const int kernel = darktable.opencl->blendop->kernel_read_mask;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &out);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &tmp);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &iwidth);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &iheight);
    const int err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
    if(err != CL_SUCCESS) goto error;
  }

  {
    size_t sizes[3] = { ROUNDUPDWD(iwidth, devid), ROUNDUPDHT(iheight, devid), 1 };
    const int kernel = darktable.opencl->blendop->kernel_calc_blend;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &out);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &blur);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &iwidth);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &iheight);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(float), &threshold);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &detail);
    const int err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
    if(err != CL_SUCCESS) goto error;
  }

  {
    float blurmat[13];
    dt_masks_blur_9x9_coeff(blurmat, 2.0f);
    cl_mem dev_blurmat = NULL;
    dev_blurmat = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 13, blurmat);
    if(!IS_NULL_PTR(dev_blurmat))
    {
      size_t sizes[3] = { ROUNDUPDWD(iwidth, devid), ROUNDUPDHT(iheight, devid), 1 };
      const int clkernel = darktable.opencl->blendop->kernel_mask_blur;
      dt_opencl_set_kernel_arg(devid, clkernel, 0, sizeof(cl_mem), &blur);
      dt_opencl_set_kernel_arg(devid, clkernel, 1, sizeof(cl_mem), &out);
      dt_opencl_set_kernel_arg(devid, clkernel, 2, sizeof(int), &iwidth);
      dt_opencl_set_kernel_arg(devid, clkernel, 3, sizeof(int), &iheight);
      dt_opencl_set_kernel_arg(devid, clkernel, 4, sizeof(cl_mem), (void *) &dev_blurmat);
      const int err = dt_opencl_enqueue_kernel_2d(devid, clkernel, sizes);
      dt_opencl_release_mem_object(dev_blurmat);
      if(err != CL_SUCCESS) goto error;
    }
    else
    {
      dt_opencl_release_mem_object(dev_blurmat);
      goto error;
    }
  }

  {
    size_t sizes[3] = { ROUNDUPDWD(iwidth, devid), ROUNDUPDHT(iheight, devid), 1 };
    const int kernel = darktable.opencl->blendop->kernel_write_mask;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &out);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &tmp);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &iwidth);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &iheight);
    const int err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
    if(err != CL_SUCCESS) goto error;
  }

  {
    const int err = dt_opencl_read_host_from_device(devid, lum, tmp, iwidth, iheight, sizeof(float));
    if(err != CL_SUCCESS) goto error;
  }

  dt_opencl_release_mem_object(tmp);
  dt_opencl_release_mem_object(blur);
  dt_opencl_release_mem_object(out);
  tmp = blur = out = NULL;

  // here we have the slightly blurred full detail available
  float *warp_mask = dt_dev_distort_detail_mask(p, lum, self);
  if(IS_NULL_PTR(warp_mask)) goto error;
  // dt_dev_distort_detail_mask() may return `lum` unchanged when no geometric distortion is needed.
  const gboolean warp_mask_aliases_lum = (warp_mask == lum);
  if(!warp_mask_aliases_lum)
  {
    dt_pixelpipe_cache_free_align(lum);
    lum = NULL;
  }

  const int msize = owidth * oheight;
  __OMP_PARALLEL_FOR_SIMD__(aligned(mask, warp_mask : 64))
  for(int idx = 0; idx < msize; idx++)
  {
    mask[idx] = mask[idx] * warp_mask[idx];
  }
  dt_pixelpipe_cache_free_align(warp_mask);
  if(warp_mask_aliases_lum) lum = NULL;
  return;

  error:
  dt_control_log(_("detail mask CL blending problem"));
  dt_pixelpipe_cache_free_align(lum);
  dt_opencl_release_mem_object(tmp);
  dt_opencl_release_mem_object(blur);
  dt_opencl_release_mem_object(out);
}

static inline void _blend_process_cl_exchange(cl_mem *a, cl_mem *b)
{
  cl_mem tmp = *a;
  *a = *b;
  *b = tmp;
}

int dt_develop_blend_process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                                const struct dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out)
{
  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  if(pipe->bypass_blendif && self->dev->gui_attached && (self == self->dev->gui_module)) return 0;

  dt_develop_blend_params_t *const d = (dt_develop_blend_params_t *const)piece->blendop_data;
  if(IS_NULL_PTR(d)) return 0;

  gboolean top_enabled = FALSE;
  gboolean raster_used = FALSE;
  gboolean drawn_used = FALSE;
  gboolean parametric_used = FALSE;
  dt_develop_blend_get_mask_usage(self, d, &top_enabled, &raster_used, &drawn_used, &parametric_used);
  if(!top_enabled) return 0;
  const unsigned int mask_mode = d->mask_mode;
  // A mask preview request must also be honored when every sub-mask is neutral.
  // In that case no raster/drawn/parametric mask is "used", but the effective
  // blend mask is still the uniform opacity mask written into alpha below.
  const gboolean preview_mask_mode = top_enabled;

  const int ch = piece->dsc_in.channels; // the number of channels in the buffer
  const int xoffs = roi_out->x - roi_in->x;
  const int yoffs = roi_out->y - roi_in->y;
  const int iwidth = roi_in->width;
  const int iheight = roi_in->height;
  const int owidth = roi_out->width;
  const int oheight = roi_out->height;
  const size_t buffsize = (size_t)owidth * oheight;
  const float iscale = roi_in->scale;
  const float oscale = roi_out->scale;
  const gboolean rois_equal = iwidth == owidth || iheight == oheight || xoffs == 0 || yoffs == 0;

  // In most cases of blending-enabled modules input and output of the module have
  // the exact same dimensions. Only in very special cases we allow a module's input
  // to exceed its output. This is namely the case for the spot removal module where
  // the source of a patch might lie outside the roi of the output image. Therefore:
  // We can only handle blending if roi_out and roi_in have the same scale and
  // if roi_out fits into the area given by roi_in. xoffs and yoffs describe the relative
  // offset of the input image to the output image. */
  if(oscale != iscale || xoffs < 0 || yoffs < 0
     || ((xoffs > 0 || yoffs > 0) && (owidth + xoffs > iwidth || oheight + yoffs > iheight)))
  {
    dt_control_log(_("skipped blending in module '%s': roi's do not match"), self->op);
    return 0;
  }

  // only non-zero if mask_display was set by an _earlier_ module
  const dt_dev_pixelpipe_display_mask_t mask_display = pipe->mask_display;

  // does user want us to display a specific channel?
  const dt_dev_pixelpipe_display_mask_t request_mask_display
      = (self->dev->gui_attached && (self == self->dev->gui_module) && (pipe == self->dev->pipe)
         && preview_mask_mode)
            ? self->request_mask_display
            : DT_DEV_PIXELPIPE_DISPLAY_NONE;

  // get channel max values depending on colorspace
  const dt_develop_blend_colorspace_t blend_csp = d->blend_cst;
  const dt_iop_colorspace_type_t cst = dt_develop_blend_colorspace(piece, IOP_CS_NONE);

  // obtaining the list of mask operations to perform
  _develop_mask_post_processing post_operations[3];
  const size_t post_operations_size = _develop_mask_get_post_operations(d, piece, post_operations);

  // get the clipped opacity value  0 - 1
  const float opacity = fminf(fmaxf(d->opacity / 100.0f, 0.0f), 1.0f);
  const gboolean use_masks = raster_used || drawn_used || parametric_used;
  const gboolean raster_only = raster_used && !drawn_used && !parametric_used;

  // allocate space for blend mask
  float *_mask = dt_pixelpipe_cache_alloc_align_float(buffsize, pipe);
  if(IS_NULL_PTR(_mask))
  {
    dt_control_log(_("could not allocate buffer for blending"));
    return 1;
  }
  float *const mask = _mask;

  // setup some kernels
  int kernel_mask;
  int kernel;
  switch(blend_csp)
  {
    case DEVELOP_BLEND_CS_RAW:
      kernel = darktable.opencl->blendop->kernel_blendop_RAW;
      kernel_mask = darktable.opencl->blendop->kernel_blendop_mask_RAW;
      break;

    case DEVELOP_BLEND_CS_RGB_DISPLAY:
      kernel = darktable.opencl->blendop->kernel_blendop_rgb_hsl;
      kernel_mask = darktable.opencl->blendop->kernel_blendop_mask_rgb_hsl;
      break;

    case DEVELOP_BLEND_CS_RGB_SCENE:
      kernel = darktable.opencl->blendop->kernel_blendop_rgb_jzczhz;
      kernel_mask = darktable.opencl->blendop->kernel_blendop_mask_rgb_jzczhz;
      break;

    case DEVELOP_BLEND_CS_LAB:
    default:
      kernel = darktable.opencl->blendop->kernel_blendop_Lab;
      kernel_mask = darktable.opencl->blendop->kernel_blendop_mask_Lab;
      break;
  }
  int kernel_mask_tone_curve = darktable.opencl->blendop->kernel_blendop_mask_tone_curve;
  int kernel_set_mask = darktable.opencl->blendop->kernel_blendop_set_mask;
  int kernel_display_channel = darktable.opencl->blendop->kernel_blendop_display_channel;

  const int devid = pipe->devid;
  const int offs[2] = { xoffs, yoffs };
  const size_t sizes[] = { ROUNDUPDWD(owidth, devid), ROUNDUPDHT(oheight, devid), 1 };

  cl_int err = -999;
  cl_mem dev_blendif_params = NULL;
  cl_mem dev_boost_factors = NULL;
  cl_mem dev_mask_1 = NULL;
  cl_mem dev_mask_2 = NULL;
  cl_mem dev_tmp = NULL;
  cl_mem dev_guide = NULL;

  cl_mem dev_profile_info = NULL;
  cl_mem dev_profile_lut = NULL;
  dt_colorspaces_iccprofile_info_cl_t *profile_info_cl = NULL;
  cl_float *profile_lut_cl = NULL;

  cl_mem dev_work_profile_info = NULL;
  cl_mem dev_work_profile_lut = NULL;
  dt_colorspaces_iccprofile_info_cl_t *work_profile_info_cl = NULL;
  cl_float *work_profile_lut_cl = NULL;

  size_t origin[] = { 0, 0, 0 };
  size_t region[] = { owidth, oheight, 1 };

  // parameters, for every channel the 4 limits + pre-computed increasing slope and decreasing slope
  float parameters[DEVELOP_BLENDIF_PARAMETER_ITEMS * DEVELOP_BLENDIF_SIZE] DT_ALIGNED_ARRAY;
  dt_develop_blendif_process_parameters(parameters, d);

  // copy blend parameters to constant device memory
  dev_blendif_params = dt_opencl_copy_host_to_device_constant(devid, sizeof(parameters), parameters);
  if(IS_NULL_PTR(dev_blendif_params)) goto error;

  dev_mask_1 = dt_opencl_alloc_device(devid, owidth, oheight, sizeof(float));
  if(IS_NULL_PTR(dev_mask_1)) goto error;

  dt_iop_order_iccprofile_info_t profile;
  const int use_profile = dt_develop_blendif_init_masking_profile(pipe, piece, &profile, blend_csp);

  err = dt_ioppr_build_iccprofile_params_cl(use_profile ? &profile : NULL, devid, &profile_info_cl,
                                            &profile_lut_cl, &dev_profile_info, &dev_profile_lut);
  if(err != CL_SUCCESS) goto error;

  if(!use_masks)
  {
    // blend uniformly (no drawn or parametric mask)

    // set dev_mask with global opacity value
    dt_opencl_set_kernel_arg(devid, kernel_set_mask, 0, sizeof(cl_mem), (void *)&dev_mask_1);
    dt_opencl_set_kernel_arg(devid, kernel_set_mask, 1, sizeof(int), (void *)&owidth);
    dt_opencl_set_kernel_arg(devid, kernel_set_mask, 2, sizeof(int), (void *)&oheight);
    dt_opencl_set_kernel_arg(devid, kernel_set_mask, 3, sizeof(float), (void *)&opacity);
    err = dt_opencl_enqueue_kernel_2d(devid, kernel_set_mask, sizes);
    if(err != CL_SUCCESS) goto error;
  }
  else if(raster_only)
  {
    int raster_error = 0;
    _develop_blend_init_raster_mask(d, self, pipe, piece, mask, owidth, oheight, &raster_error);
    if(raster_error) goto error;
    dt_iop_image_mul_const(mask, opacity, owidth, oheight, 1);

    err = dt_opencl_write_host_to_device(devid, mask, dev_mask_1, owidth, oheight, sizeof(float));
    if(err != CL_SUCCESS) goto error;
  }
  else
  {
    if(!raster_used && !drawn_used)
    {
      // Parametric-only blending has no raster/drawn form mask to seed `mask`.
      // The OpenCL mask kernel reads that form mask from `dev_mask_1`, so keep
      // the CPU staging buffer explicit and initialized before upload.
      const float fill = (d->mask_combine & DEVELOP_COMBINE_INCL) ? 0.0f : 1.0f;
      dt_iop_image_fill(mask, fill, owidth, oheight, 1);
    }
    else if(raster_used)
    {
      int raster_error = 0;
      _develop_blend_init_raster_mask(d, self, pipe, piece, mask, owidth, oheight, &raster_error);
      if(raster_error) goto error;

      if(drawn_used)
      {
        float *const restrict drawn_mask = dt_pixelpipe_cache_alloc_align_float(buffsize, pipe);
        if(IS_NULL_PTR(drawn_mask)) goto error;

        if(_develop_blend_init_drawn_mask(d, self, pipe, piece, roi_out, drawn_mask, owidth, oheight) != 0)
        {
          dt_pixelpipe_cache_free_align(drawn_mask);
          goto error;
        }

        _develop_blend_combine_masks(mask, drawn_mask, buffsize);
        dt_pixelpipe_cache_free_align(drawn_mask);
      }
    }
    else if(drawn_used
            && _develop_blend_init_drawn_mask(d, self, pipe, piece, roi_out, mask, owidth, oheight) != 0)
    {
      goto error;
    }
    _refine_with_detail_mask_cl(self, pipe, piece, mask, d->details, devid);

    // write mask from host to device
    dev_mask_2 = dt_opencl_alloc_device(devid, owidth, oheight, sizeof(float));
    if(IS_NULL_PTR(dev_mask_2)) goto error;
    err = dt_opencl_write_host_to_device(devid, mask, dev_mask_1, owidth, oheight, sizeof(float));
    if(err != CL_SUCCESS) goto error;

    // get parametric mask (if any) and apply global opacity
    const unsigned blendif = d->blendif;
    const unsigned int mask_combine = d->mask_combine;
    dt_opencl_set_kernel_arg(devid, kernel_mask, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 2, sizeof(cl_mem), (void *)&dev_mask_1);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 3, sizeof(cl_mem), (void *)&dev_mask_2);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 4, sizeof(int), (void *)&owidth);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 5, sizeof(int), (void *)&oheight);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 6, sizeof(float), (void *)&opacity);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 7, sizeof(unsigned), (void *)&blendif);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 8, sizeof(cl_mem), (void *)&dev_blendif_params);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 9, sizeof(unsigned), (void *)&mask_mode);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 10, sizeof(unsigned), (void *)&mask_combine);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 11, 2 * sizeof(int), (void *)&offs);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 12, sizeof(cl_mem), (void *)&dev_profile_info);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 13, sizeof(cl_mem), (void *)&dev_profile_lut);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 14, sizeof(int), (void *)&use_profile);
    err = dt_opencl_enqueue_kernel_2d(devid, kernel_mask, sizes);
    if(err != CL_SUCCESS)
    {
      fprintf(stderr, "[dt_develop_blend_process_cl] error %i enqueue kernel\n", err);
      goto error;
    }

    // the mask is now located in dev_mask_2, put it in dev_mask_1
    _blend_process_cl_exchange(&dev_mask_1, &dev_mask_2);

    // post processing the mask (it will always be stored in dev_mask_1)
    for(size_t index = 0; index < post_operations_size; ++index)
    {
      _develop_mask_post_processing operation = post_operations[index];
      if(operation == DEVELOP_MASK_POST_FEATHER_IN)
      {
        int w = (int)(2 * d->feathering_radius * roi_out->scale + 0.5f);
        if (w < 1) w = 1;
        const float sqrt_eps = 1.0f;
        const float guide_weight = dt_iop_colorspace_is_rgb(cst) ? 100.0f : 1.0f;

        cl_mem guide = dev_in;
        if(!rois_equal)
        {
          dev_guide = dt_opencl_alloc_device(devid, owidth, oheight, sizeof(float) * 4);
          if(IS_NULL_PTR(dev_guide)) goto error;
          guide = dev_guide;
          size_t origin_1[] = { xoffs, yoffs, 0 };
          size_t origin_2[] = { 0, 0, 0 };
          err = dt_opencl_enqueue_copy_image(devid, dev_in, guide, origin_2, origin_1, region);
          if(err != CL_SUCCESS) goto error;
        }
        if(guided_filter_cl(devid, guide, dev_mask_1, dev_mask_2, owidth, oheight, ch, w, sqrt_eps, guide_weight,
                            0.0f, 1.0f) != 0)
          goto error;
        if(!rois_equal)
        {
          dt_opencl_release_mem_object(dev_guide);
          dev_guide = NULL;
        }
        _blend_process_cl_exchange(&dev_mask_1, &dev_mask_2);
      }
      else if(operation == DEVELOP_MASK_POST_FEATHER_OUT)
      {
        int w = (int)(2 * d->feathering_radius * roi_out->scale + 0.5f);
        if (w < 1) w = 1;
        const float sqrt_eps = 1.0f;
        const float guide_weight = dt_iop_colorspace_is_rgb(cst) ? 100.0f : 1.0f;

        if(guided_filter_cl(devid, dev_out, dev_mask_1, dev_mask_2, owidth, oheight, ch, w, sqrt_eps, guide_weight,
                            0.0f, 1.0f) != 0)
          goto error;
        _blend_process_cl_exchange(&dev_mask_1, &dev_mask_2);
      }
      else if(operation == DEVELOP_MASK_POST_BLUR)
      {
        const float sigma = d->blur_radius * roi_out->scale;
        const float mmax[] = { 1.0f };
        const float mmin[] = { 0.0f };

        dt_gaussian_cl_t *g = dt_gaussian_init_cl(devid, owidth, oheight, 1, mmax, mmin, sigma, 0);
        if(IS_NULL_PTR(g)) goto error;
        err = dt_gaussian_blur_cl(g, dev_mask_1, dev_mask_2);
        dt_gaussian_free_cl(g);
        if(err != CL_SUCCESS) goto error;
        _blend_process_cl_exchange(&dev_mask_1, &dev_mask_2);
      }
      else if(operation == DEVELOP_MASK_POST_TONE_CURVE)
      {
        const float e = expf(3.f * d->contrast);
        const float brightness = d->brightness;
        dt_opencl_set_kernel_arg(devid, kernel_mask_tone_curve, 0, sizeof(cl_mem), (void *)&dev_mask_1);
        dt_opencl_set_kernel_arg(devid, kernel_mask_tone_curve, 1, sizeof(cl_mem), (void *)&dev_mask_2);
        dt_opencl_set_kernel_arg(devid, kernel_mask_tone_curve, 2, sizeof(int), (void *)&owidth);
        dt_opencl_set_kernel_arg(devid, kernel_mask_tone_curve, 3, sizeof(int), (void *)&oheight);
        dt_opencl_set_kernel_arg(devid, kernel_mask_tone_curve, 4, sizeof(float), (void *)&e);
        dt_opencl_set_kernel_arg(devid, kernel_mask_tone_curve, 5, sizeof(float), (void *)&brightness);
        dt_opencl_set_kernel_arg(devid, kernel_mask_tone_curve, 6, sizeof(float), (void *)&opacity);
        err = dt_opencl_enqueue_kernel_2d(devid, kernel_mask_tone_curve, sizes);
        if(err != CL_SUCCESS) goto error;
        _blend_process_cl_exchange(&dev_mask_1, &dev_mask_2);
      }
    }

    // get rid of dev_mask_2
    dt_opencl_release_mem_object(dev_mask_2);
    dev_mask_2 = NULL;
  }

  // get temporary buffer for output image to overcome readonly/writeonly limitation
  dev_tmp = dt_opencl_alloc_device(devid, owidth, oheight, sizeof(float) * 4);
  if(IS_NULL_PTR(dev_tmp)) goto error;

  err = dt_opencl_enqueue_copy_image(devid, dev_out, dev_tmp, origin, origin, region);
  if(err != CL_SUCCESS) goto error;

  if(request_mask_display & DT_DEV_PIXELPIPE_DISPLAY_ANY)
  {
    // load the boost factors in the device memory
    dev_boost_factors = dt_opencl_copy_host_to_device_constant(devid, sizeof(d->blendif_boost_factors),
                                                               d->blendif_boost_factors);
    if(IS_NULL_PTR(dev_boost_factors)) goto error;

    // the display channel of Lab blending is generated in RGB and should be transformed to Lab
    // the transformation in the pipeline is currently always using the work profile
    dt_iop_order_iccprofile_info_t *work_profile = dt_ioppr_get_pipe_work_profile_info(pipe);
    const int use_work_profile = !IS_NULL_PTR(work_profile);

    err = dt_ioppr_build_iccprofile_params_cl(work_profile, devid, &work_profile_info_cl, &work_profile_lut_cl,
                                              &dev_work_profile_info, &dev_work_profile_lut);
    if(err != CL_SUCCESS) goto error;

    // let us display a specific channel
    dt_opencl_set_kernel_arg(devid, kernel_display_channel, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, kernel_display_channel, 1, sizeof(cl_mem), (void *)&dev_tmp);
    dt_opencl_set_kernel_arg(devid, kernel_display_channel, 2, sizeof(cl_mem), (void *)&dev_mask_1);
    dt_opencl_set_kernel_arg(devid, kernel_display_channel, 3, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, kernel_display_channel, 4, sizeof(int), (void *)&owidth);
    dt_opencl_set_kernel_arg(devid, kernel_display_channel, 5, sizeof(int), (void *)&oheight);
    dt_opencl_set_kernel_arg(devid, kernel_display_channel, 6, 2 * sizeof(int), (void *)&offs);
    dt_opencl_set_kernel_arg(devid, kernel_display_channel, 7, sizeof(int), (void *)&request_mask_display);
    dt_opencl_set_kernel_arg(devid, kernel_display_channel, 8, sizeof(cl_mem), (void*)&dev_boost_factors);
    dt_opencl_set_kernel_arg(devid, kernel_display_channel, 9, sizeof(cl_mem), (void *)&dev_profile_info);
    dt_opencl_set_kernel_arg(devid, kernel_display_channel, 10, sizeof(cl_mem), (void *)&dev_profile_lut);
    dt_opencl_set_kernel_arg(devid, kernel_display_channel, 11, sizeof(int), (void *)&use_profile);
    dt_opencl_set_kernel_arg(devid, kernel_display_channel, 12, sizeof(cl_mem), (void *)&dev_work_profile_info);
    dt_opencl_set_kernel_arg(devid, kernel_display_channel, 13, sizeof(cl_mem), (void *)&dev_work_profile_lut);
    dt_opencl_set_kernel_arg(devid, kernel_display_channel, 14, sizeof(int), (void *)&use_work_profile);
    err = dt_opencl_enqueue_kernel_2d(devid, kernel_display_channel, sizes);
    if(err != CL_SUCCESS)
    {
      fprintf(stderr, "[dt_develop_blend_process_cl] error %i enqueue kernel\n", err);
      goto error;
    }
  }
  else
  {
    // apply blending with per-pixel opacity value as defined in dev_mask_1
    const unsigned int blend_mode = d->blend_mode;
    const float blend_parameter = exp2f(d->blend_parameter);
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), (void *)&dev_tmp);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), (void *)&dev_mask_1);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), (void *)&owidth);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), (void *)&oheight);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(unsigned), (void *)&blend_mode);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(float), (void *)&blend_parameter);
    dt_opencl_set_kernel_arg(devid, kernel, 8, 2 * sizeof(int), (void *)&offs);
    dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(int), (void *)&mask_display);
    err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
    if(err != CL_SUCCESS) goto error;
  }

  // register if _this_ module should expose mask or display channel
  if(request_mask_display & (DT_DEV_PIXELPIPE_DISPLAY_MASK | DT_DEV_PIXELPIPE_DISPLAY_CHANNEL))
  {
    pipe->mask_display = request_mask_display;
  }


  // check if we should store the mask for export or use in subsequent modules
  if(pipe->store_all_raster_masks || dt_iop_is_raster_mask_used(self, 0))
  {
    //  get back final mask from the device to store it for later use
    if(!raster_only)
    {
      err = dt_opencl_copy_device_to_host(devid, mask, dev_mask_1, owidth, oheight, sizeof(float));
      if(err != CL_SUCCESS) goto error;
    }

    const uint64_t mask_hash = dt_dev_pixelpipe_raster_mask_hash(piece, 0);
    dt_pixel_cache_entry_t *mask_entry = NULL;
    void *cache_data = NULL;
    const int created = dt_dev_pixelpipe_cache_get(
        darktable.pixelpipe_cache, mask_hash, sizeof(float) * buffsize,
        "raster mask", pipe->type, TRUE, &cache_data, &mask_entry);

    if(IS_NULL_PTR(cache_data) || IS_NULL_PTR(mask_entry))
    {
      if(created && !IS_NULL_PTR(mask_entry))
        dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, FALSE, mask_entry);
      if(!IS_NULL_PTR(mask_entry))
      {
        dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, FALSE, mask_entry);
        if(created)
          dt_dev_pixelpipe_cache_remove(darktable.pixelpipe_cache, TRUE, mask_entry);
      }
      goto error;
    }

    // The OpenCL mask has been materialized into `_mask` above. Publish that
    // host payload once so CPU and GPU consumers share the same cache entry.
    if(created)
    {
      memcpy(cache_data, _mask, sizeof(float) * buffsize);
      dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, FALSE, mask_entry);
    }
    // Transfer cache_get()'s reference to the pipe. It keeps this side-band
    // output alive until the next graph state is prepared or the pipe closes.
    g_array_append_val(pipe->raster_mask_hashes, mask_hash);
    dt_pixelpipe_cache_free_align(_mask);
    _mask = NULL;

    dt_print(DT_DEBUG_MASKS,
             "[raster masks] %s mask id 0 for module %s (%s) for pipe %s"
             " with cache hash %" PRIu64 "\n",
             created ? "published" : "reused cached",
             piece->module->op, piece->module->multi_name,
             dt_pipe_type_to_str(pipe->type), mask_hash);
  }
  else
  {
    dt_print(DT_DEBUG_MASKS,
             "[raster masks] discarding unpublished mask id 0 for module %s (%s) for pipe %s\n",
             piece->module->op, piece->module->multi_name,
             dt_pipe_type_to_str(pipe->type));
    dt_pixelpipe_cache_free_align(_mask);
  }

  dt_opencl_release_mem_object(dev_blendif_params);
  dt_opencl_release_mem_object(dev_boost_factors);
  dt_opencl_release_mem_object(dev_mask_1);
  dt_opencl_release_mem_object(dev_tmp);
  dt_ioppr_free_iccprofile_params_cl(&profile_info_cl, &profile_lut_cl, &dev_profile_info, &dev_profile_lut);
  dt_ioppr_free_iccprofile_params_cl(&work_profile_info_cl, &work_profile_lut_cl, &dev_work_profile_info,
                                     &dev_work_profile_lut);
  return 0;

error:
  dt_pixelpipe_cache_free_align(_mask);
  dt_opencl_release_mem_object(dev_blendif_params);
  dt_opencl_release_mem_object(dev_boost_factors);
  dt_opencl_release_mem_object(dev_mask_1);
  dt_opencl_release_mem_object(dev_mask_2);
  dt_opencl_release_mem_object(dev_tmp);
  dt_opencl_release_mem_object(dev_guide);
  if(profile_info_cl) dt_ioppr_free_iccprofile_params_cl(&profile_info_cl, &profile_lut_cl, &dev_profile_info, &dev_profile_lut);
  if(work_profile_info_cl) dt_ioppr_free_iccprofile_params_cl(&work_profile_info_cl, &work_profile_lut_cl, &dev_work_profile_info,
                                     &dev_work_profile_lut);
  dt_print(DT_DEBUG_OPENCL, "[opencl_blendop] couldn't enqueue kernel! %d\n", err);
  return 1;
}
#endif

/** global init of blendops */
dt_blendop_cl_global_t *dt_develop_blend_init_cl_global(void)
{
#ifdef HAVE_OPENCL
  dt_blendop_cl_global_t *b = (dt_blendop_cl_global_t *)calloc(1, sizeof(dt_blendop_cl_global_t));

  const int program = 3; // blendop.cl, from programs.conf
  b->kernel_blendop_mask_Lab = dt_opencl_create_kernel(program, "blendop_mask_Lab");
  b->kernel_blendop_mask_RAW = dt_opencl_create_kernel(program, "blendop_mask_RAW");
  b->kernel_blendop_mask_rgb_hsl = dt_opencl_create_kernel(program, "blendop_mask_rgb_hsl");
  b->kernel_blendop_mask_rgb_jzczhz = dt_opencl_create_kernel(program, "blendop_mask_rgb_jzczhz");
  b->kernel_blendop_Lab = dt_opencl_create_kernel(program, "blendop_Lab");
  b->kernel_blendop_RAW = dt_opencl_create_kernel(program, "blendop_RAW");
  b->kernel_blendop_rgb_hsl = dt_opencl_create_kernel(program, "blendop_rgb_hsl");
  b->kernel_blendop_rgb_jzczhz = dt_opencl_create_kernel(program, "blendop_rgb_jzczhz");
  b->kernel_blendop_mask_tone_curve = dt_opencl_create_kernel(program, "blendop_mask_tone_curve");
  b->kernel_blendop_set_mask = dt_opencl_create_kernel(program, "blendop_set_mask");
  b->kernel_blendop_display_channel = dt_opencl_create_kernel(program, "blendop_display_channel");

  const int program_rcd = 31;
  b->kernel_calc_Y0_mask = dt_opencl_create_kernel(program_rcd, "calc_Y0_mask");
  b->kernel_calc_scharr_mask = dt_opencl_create_kernel(program_rcd, "calc_scharr_mask");
  b->kernel_write_mask = dt_opencl_create_kernel(program_rcd, "writeout_mask");
  b->kernel_read_mask  = dt_opencl_create_kernel(program_rcd, "readin_mask");
  b->kernel_calc_blend = dt_opencl_create_kernel(program_rcd, "calc_detail_blend");
  b->kernel_mask_blur  = dt_opencl_create_kernel(program_rcd, "fastblur_mask_9x9");

  return b;
#else
  return NULL;
#endif
}

/** global cleanup of blendops */
void dt_develop_blend_free_cl_global(dt_blendop_cl_global_t *b)
{
#ifdef HAVE_OPENCL
  if(IS_NULL_PTR(b)) return;

  dt_opencl_free_kernel(b->kernel_blendop_mask_Lab);
  dt_opencl_free_kernel(b->kernel_blendop_mask_RAW);
  dt_opencl_free_kernel(b->kernel_blendop_mask_rgb_hsl);
  dt_opencl_free_kernel(b->kernel_blendop_mask_rgb_jzczhz);
  dt_opencl_free_kernel(b->kernel_blendop_Lab);
  dt_opencl_free_kernel(b->kernel_blendop_RAW);
  dt_opencl_free_kernel(b->kernel_blendop_rgb_hsl);
  dt_opencl_free_kernel(b->kernel_blendop_rgb_jzczhz);
  dt_opencl_free_kernel(b->kernel_blendop_mask_tone_curve);
  dt_opencl_free_kernel(b->kernel_blendop_set_mask);
  dt_opencl_free_kernel(b->kernel_blendop_display_channel);
  dt_opencl_free_kernel(b->kernel_calc_Y0_mask);
  dt_opencl_free_kernel(b->kernel_calc_scharr_mask);
  dt_opencl_free_kernel(b->kernel_write_mask);
  dt_opencl_free_kernel(b->kernel_read_mask);
  dt_opencl_free_kernel(b->kernel_calc_blend);
  dt_opencl_free_kernel(b->kernel_mask_blur);
  dt_free(b);
#endif
}

/** blend version */
int dt_develop_blend_version(void)
{
  return DEVELOP_BLEND_VERSION;
}

/** report back specific memory requirements for blend step (only relevant for OpenCL path) */
void tiling_callback_blendop(struct dt_iop_module_t *self, const struct dt_dev_pixelpipe_t *pipe,
                             const struct dt_dev_pixelpipe_iop_t *piece, struct dt_develop_tiling_t *tiling)
{
  tiling->factor = 3.5f; // in + out + (guide, tmp) + two quarter buffers for the mask
  tiling->maxbuf = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = 0;
  tiling->xalign = 1;
  tiling->yalign = 1;

  dt_develop_blend_params_t *const bldata = (dt_develop_blend_params_t *const)piece->blendop_data;
  if(!IS_NULL_PTR(bldata))
  {
    if(bldata->details != 0.0f)
      tiling->factor += 0.75f; // details mask requires 3 additional quarter buffers
  }
}

/** check if content of params is all zero, indicating a non-initialized set of
   blend parameters
    which needs special care. */
gboolean dt_develop_blend_params_is_all_zero(const void *params, size_t length)
{
  const char *data = (const char *)params;

  for(size_t k = 0; k < length; k++)
    if(data[k]) return FALSE;

  return TRUE;
}

static uint32_t _blend_legacy_blend_mode(uint32_t legacy_blend_mode)
{
  uint32_t blend_mode = legacy_blend_mode & DEVELOP_BLEND_MODE_MASK;
  gboolean blend_reverse = FALSE;
  switch(blend_mode) {
    case DEVELOP_BLEND_NORMAL_OBSOLETE:
      blend_mode = DEVELOP_BLEND_BOUNDED;
      break;
    case DEVELOP_BLEND_INVERSE_OBSOLETE:
      blend_mode = DEVELOP_BLEND_BOUNDED;
      blend_reverse = TRUE;
      break;
    case DEVELOP_BLEND_DISABLED_OBSOLETE:
    case DEVELOP_BLEND_UNBOUNDED_OBSOLETE:
      blend_mode = DEVELOP_BLEND_NORMAL2;
      break;
    case DEVELOP_BLEND_MULTIPLY_REVERSE_OBSOLETE:
      blend_mode = DEVELOP_BLEND_MULTIPLY;
      blend_reverse = TRUE;
      break;
    default:
      break;
  }
  return (blend_reverse ? DEVELOP_BLEND_REVERSE : 0) | blend_mode;
}

/** update blendop params from older versions */
int dt_develop_blend_legacy_params(dt_iop_module_t *module, const void *const old_params,
                                   const int old_version, void *new_params, const int new_version,
                                   const int length)
{
  // edits before version 10 default to a display referred workflow
  dt_develop_blend_colorspace_t cst = _blend_default_module_blend_colorspace(module, 0);

  dt_develop_blend_params_t default_display_blend_params;
  dt_develop_blend_init_blend_parameters(&default_display_blend_params, cst);

  // first deal with all-zero parameter sets, regardless of version number.
  // these occurred in previous darktable versions when modules without blend support stored zero-initialized data
  // in history stack. that's no problem unless the module gets blend support later (e.g. module exposure).
  // remedy: we simply initialize with the current default blend params in this case.
  if(dt_develop_blend_params_is_all_zero(old_params, length))
  {
    dt_develop_blend_params_t *n = (dt_develop_blend_params_t *)new_params;

    *n = default_display_blend_params;
    return 0;
  }

  if(old_version == 1 && new_version == 11)
  {
    /** blend legacy parameters version 1 */
    typedef struct dt_develop_blend_params1_t
    {
      uint32_t mode;
      float opacity;
      uint32_t mask_id;
    } dt_develop_blend_params1_t;

    if(length != sizeof(dt_develop_blend_params1_t)) return 1;

    dt_develop_blend_params1_t *o = (dt_develop_blend_params1_t *)old_params;
    dt_develop_blend_params_t *n = (dt_develop_blend_params_t *)new_params;

    *n = default_display_blend_params; // start with a fresh copy of default parameters
    n->mask_mode = (o->mode == DEVELOP_BLEND_DISABLED_OBSOLETE) ? DEVELOP_MASK_DISABLED : DEVELOP_MASK_ENABLED;
    n->blend_mode = _blend_legacy_blend_mode(o->mode);
    n->opacity = o->opacity;
    n->mask_id = o->mask_id;
    return 0;
  }

  if(old_version == 2 && new_version == 11)
  {
    /** blend legacy parameters version 2 */
    typedef struct dt_develop_blend_params2_t
    {
      /** blending mode */
      uint32_t mode;
      /** mixing opacity */
      float opacity;
      /** id of mask in current pipeline */
      uint32_t mask_id;
      /** blendif mask */
      uint32_t blendif;
      /** blendif parameters */
      float blendif_parameters[4 * 8];
    } dt_develop_blend_params2_t;

    if(length != sizeof(dt_develop_blend_params2_t)) return 1;

    dt_develop_blend_params2_t *o = (dt_develop_blend_params2_t *)old_params;
    dt_develop_blend_params_t *n = (dt_develop_blend_params_t *)new_params;

    *n = default_display_blend_params; // start with a fresh copy of default parameters
    n->mask_mode = (o->mode == DEVELOP_BLEND_DISABLED_OBSOLETE) ? DEVELOP_MASK_DISABLED : DEVELOP_MASK_ENABLED;
    n->mask_mode |= ((o->blendif & (1u << DEVELOP_BLENDIF_active)) && (n->mask_mode == DEVELOP_MASK_ENABLED))
                        ? DEVELOP_MASK_PARAMETRIC
                        : 0;
    n->blend_mode = _blend_legacy_blend_mode(o->mode);
    n->opacity = o->opacity;
    n->mask_id = o->mask_id;
    n->blendif = o->blendif & 0xff; // only just in case: knock out all bits
                                    // which were undefined in version
                                    // 2; also switch off old "active" bit
    for(int i = 0; i < (4 * 8); i++) n->blendif_parameters[i] = o->blendif_parameters[i];

    return 0;
  }

  if(old_version == 3 && new_version == 11)
  {
    /** blend legacy parameters version 3 */
    typedef struct dt_develop_blend_params3_t
    {
      /** blending mode */
      uint32_t mode;
      /** mixing opacity */
      float opacity;
      /** id of mask in current pipeline */
      uint32_t mask_id;
      /** blendif mask */
      uint32_t blendif;
      /** blendif parameters */
      float blendif_parameters[4 * DEVELOP_BLENDIF_SIZE];
    } dt_develop_blend_params3_t;

    if(length != sizeof(dt_develop_blend_params3_t)) return 1;

    dt_develop_blend_params3_t *o = (dt_develop_blend_params3_t *)old_params;
    dt_develop_blend_params_t *n = (dt_develop_blend_params_t *)new_params;

    *n = default_display_blend_params; // start with a fresh copy of default parameters
    n->mask_mode = (o->mode == DEVELOP_BLEND_DISABLED_OBSOLETE) ? DEVELOP_MASK_DISABLED : DEVELOP_MASK_ENABLED;
    n->mask_mode |= ((o->blendif & (1u << DEVELOP_BLENDIF_active)) && (n->mask_mode == DEVELOP_MASK_ENABLED))
                        ? DEVELOP_MASK_PARAMETRIC
                        : 0;
    n->blend_mode = _blend_legacy_blend_mode(o->mode);
    n->opacity = o->opacity;
    n->mask_id = o->mask_id;
    n->blendif = o->blendif & ~(1u << DEVELOP_BLENDIF_active); // knock out old unused "active" flag
    memcpy(n->blendif_parameters, o->blendif_parameters, sizeof(float) * 4 * DEVELOP_BLENDIF_SIZE);

    return 0;
  }

  if(old_version == 4 && new_version == 11)
  {
    /** blend legacy parameters version 4 */
    typedef struct dt_develop_blend_params4_t
    {
      /** blending mode */
      uint32_t mode;
      /** mixing opacity */
      float opacity;
      /** id of mask in current pipeline */
      uint32_t mask_id;
      /** blendif mask */
      uint32_t blendif;
      /** blur radius */
      float radius;
      /** blendif parameters */
      float blendif_parameters[4 * DEVELOP_BLENDIF_SIZE];
    } dt_develop_blend_params4_t;

    if(length != sizeof(dt_develop_blend_params4_t)) return 1;

    dt_develop_blend_params4_t *o = (dt_develop_blend_params4_t *)old_params;
    dt_develop_blend_params_t *n = (dt_develop_blend_params_t *)new_params;

    *n = default_display_blend_params; // start with a fresh copy of default parameters
    n->mask_mode = (o->mode == DEVELOP_BLEND_DISABLED_OBSOLETE) ? DEVELOP_MASK_DISABLED : DEVELOP_MASK_ENABLED;
    n->mask_mode |= ((o->blendif & (1u << DEVELOP_BLENDIF_active)) && (n->mask_mode == DEVELOP_MASK_ENABLED))
                        ? DEVELOP_MASK_PARAMETRIC
                        : 0;
    n->blend_mode = _blend_legacy_blend_mode(o->mode);
    n->opacity = o->opacity;
    n->mask_id = o->mask_id;
    n->blur_radius = o->radius;
    n->blendif = o->blendif & ~(1u << DEVELOP_BLENDIF_active); // knock out old unused "active" flag
    memcpy(n->blendif_parameters, o->blendif_parameters, sizeof(float) * 4 * DEVELOP_BLENDIF_SIZE);

    return 0;
  }

  if(old_version == 5 && new_version == 11)
  {
    /** blend legacy parameters version 5 (identical to version 6)*/
    typedef struct dt_develop_blend_params5_t
    {
      /** what kind of masking to use: off, non-mask (uniformly), hand-drawn mask and/or conditional mask */
      uint32_t mask_mode;
      /** blending mode */
      uint32_t blend_mode;
      /** mixing opacity */
      float opacity;
      /** how masks are combined */
      uint32_t mask_combine;
      /** id of mask in current pipeline */
      uint32_t mask_id;
      /** blendif mask */
      uint32_t blendif;
      /** blur radius */
      float radius;
      /** some reserved fields for future use */
      uint32_t reserved[4];
      /** blendif parameters */
      float blendif_parameters[4 * DEVELOP_BLENDIF_SIZE];
    } dt_develop_blend_params5_t;

    if(length != sizeof(dt_develop_blend_params5_t)) return 1;

    dt_develop_blend_params5_t *o = (dt_develop_blend_params5_t *)old_params;
    dt_develop_blend_params_t *n = (dt_develop_blend_params_t *)new_params;

    *n = default_display_blend_params; // start with a fresh copy of default parameters
    n->mask_mode = o->mask_mode;
    n->blend_mode = _blend_legacy_blend_mode(o->blend_mode);
    n->opacity = o->opacity;
    n->mask_combine = o->mask_combine;
    n->mask_id = o->mask_id;
    n->blur_radius = o->radius;
    // this is needed as version 5 contained a bug which screwed up history
    // stacks of even older
    // versions. potentially bad history stacks can be identified by an active
    // bit no. 32 in blendif.
    n->blendif = (o->blendif & (1u << DEVELOP_BLENDIF_active) ? o->blendif | 31 : o->blendif)
                 & ~(1u << DEVELOP_BLENDIF_active);
    memcpy(n->blendif_parameters, o->blendif_parameters, sizeof(float) * 4 * DEVELOP_BLENDIF_SIZE);

    return 0;
  }

  if(old_version == 6 && new_version == 11)
  {
    /** blend legacy parameters version 6 (identical to version 7) */
    typedef struct dt_develop_blend_params6_t
    {
      /** what kind of masking to use: off, non-mask (uniformly), hand-drawn mask and/or conditional mask */
      uint32_t mask_mode;
      /** blending mode */
      uint32_t blend_mode;
      /** mixing opacity */
      float opacity;
      /** how masks are combined */
      uint32_t mask_combine;
      /** id of mask in current pipeline */
      uint32_t mask_id;
      /** blendif mask */
      uint32_t blendif;
      /** blur radius */
      float radius;
      /** some reserved fields for future use */
      uint32_t reserved[4];
      /** blendif parameters */
      float blendif_parameters[4 * DEVELOP_BLENDIF_SIZE];
    } dt_develop_blend_params6_t;

    if(length != sizeof(dt_develop_blend_params6_t)) return 1;

    dt_develop_blend_params6_t *o = (dt_develop_blend_params6_t *)old_params;
    dt_develop_blend_params_t *n = (dt_develop_blend_params_t *)new_params;

    *n = default_display_blend_params; // start with a fresh copy of default parameters
    n->mask_mode = o->mask_mode;
    n->blend_mode = _blend_legacy_blend_mode(o->blend_mode);
    n->opacity = o->opacity;
    n->mask_combine = o->mask_combine;
    n->mask_id = o->mask_id;
    n->blur_radius = o->radius;
    n->blendif = o->blendif;
    memcpy(n->blendif_parameters, o->blendif_parameters, sizeof(float) * 4 * DEVELOP_BLENDIF_SIZE);
    return 0;
  }

  if(old_version == 7 && new_version == 11)
  {
    /** blend legacy parameters version 7 */
    typedef struct dt_develop_blend_params7_t
    {
      /** what kind of masking to use: off, non-mask (uniformly), hand-drawn mask and/or conditional mask */
      uint32_t mask_mode;
      /** blending mode */
      uint32_t blend_mode;
      /** mixing opacity */
      float opacity;
      /** how masks are combined */
      uint32_t mask_combine;
      /** id of mask in current pipeline */
      uint32_t mask_id;
      /** blendif mask */
      uint32_t blendif;
      /** blur radius */
      float radius;
      /** some reserved fields for future use */
      uint32_t reserved[4];
      /** blendif parameters */
      float blendif_parameters[4 * DEVELOP_BLENDIF_SIZE];
    } dt_develop_blend_params7_t;

    if(length != sizeof(dt_develop_blend_params7_t)) return 1;

    dt_develop_blend_params7_t *o = (dt_develop_blend_params7_t *)old_params;
    dt_develop_blend_params_t *n = (dt_develop_blend_params_t *)new_params;

    *n = default_display_blend_params; // start with a fresh copy of default parameters
    n->mask_mode = o->mask_mode;
    n->blend_mode = _blend_legacy_blend_mode(o->blend_mode);
    n->opacity = o->opacity;
    n->mask_combine = o->mask_combine;
    n->mask_id = o->mask_id;
    n->blur_radius = o->radius;
    n->blendif = o->blendif;
    memcpy(n->blendif_parameters, o->blendif_parameters, sizeof(float) * 4 * DEVELOP_BLENDIF_SIZE);
    return 0;
  }

  if(old_version == 8 && new_version == 11)
  {
    /** blend legacy parameters version 8 */
    typedef struct dt_develop_blend_params8_t
    {
      /** what kind of masking to use: off, non-mask (uniformly), hand-drawn mask and/or conditional mask */
      uint32_t mask_mode;
      /** blending mode */
      uint32_t blend_mode;
      /** mixing opacity */
      float opacity;
      /** how masks are combined */
      uint32_t mask_combine;
      /** id of mask in current pipeline */
      uint32_t mask_id;
      /** blendif mask */
      uint32_t blendif;
      /** feathering radius */
      float feathering_radius;
      /** feathering guide */
      uint32_t feathering_guide;
      /** blur radius */
      float blur_radius;
      /** mask contrast enhancement */
      float contrast;
      /** mask brightness adjustment */
      float brightness;
      /** some reserved fields for future use */
      uint32_t reserved[4];
      /** blendif parameters */
      float blendif_parameters[4 * DEVELOP_BLENDIF_SIZE];
    } dt_develop_blend_params8_t;

    if(length != sizeof(dt_develop_blend_params8_t)) return 1;

    dt_develop_blend_params8_t *o = (dt_develop_blend_params8_t *)old_params;
    dt_develop_blend_params_t *n = (dt_develop_blend_params_t *)new_params;

    *n = default_display_blend_params; // start with a fresh copy of default parameters
    n->mask_mode = o->mask_mode;
    n->blend_mode = _blend_legacy_blend_mode(o->blend_mode);
    n->opacity = o->opacity;
    n->mask_combine = o->mask_combine;
    n->mask_id = o->mask_id;
    n->blendif = o->blendif;
    n->feathering_radius = o->feathering_radius;
    n->feathering_guide = o->feathering_guide;
    n->blur_radius = o->blur_radius;
    n->contrast = o->contrast;
    n->brightness = o->brightness;
    memcpy(n->blendif_parameters, o->blendif_parameters, sizeof(float) * 4 * DEVELOP_BLENDIF_SIZE);
    return 0;
  }

  if(old_version == 9 && new_version == 11)
  {
    /** blend legacy parameters version 9 */
    typedef struct dt_develop_blend_params9_t
    {
      /** what kind of masking to use: off, non-mask (uniformly), hand-drawn mask and/or conditional mask
       *  or raster mask */
      uint32_t mask_mode;
      /** blending mode */
      uint32_t blend_mode;
      /** mixing opacity */
      float opacity;
      /** how masks are combined */
      uint32_t mask_combine;
      /** id of mask in current pipeline */
      uint32_t mask_id;
      /** blendif mask */
      uint32_t blendif;
      /** feathering radius */
      float feathering_radius;
      /** feathering guide */
      uint32_t feathering_guide;
      /** blur radius */
      float blur_radius;
      /** mask contrast enhancement */
      float contrast;
      /** mask brightness adjustment */
      float brightness;
      /** some reserved fields for future use */
      uint32_t reserved[4];
      /** blendif parameters */
      float blendif_parameters[4 * DEVELOP_BLENDIF_SIZE];
      dt_dev_operation_t raster_mask_source;
      int raster_mask_instance;
      int raster_mask_id;
      gboolean raster_mask_invert;
    } dt_develop_blend_params9_t;

    if(length != sizeof(dt_develop_blend_params9_t)) return 1;

    dt_develop_blend_params9_t *o = (dt_develop_blend_params9_t *)old_params;
    dt_develop_blend_params_t *n = (dt_develop_blend_params_t *)new_params;

    *n = default_display_blend_params; // start with a fresh copy of default parameters
    n->mask_mode = o->mask_mode;
    n->blend_mode = _blend_legacy_blend_mode(o->blend_mode);
    n->opacity = o->opacity;
    n->mask_combine = o->mask_combine;
    n->mask_id = o->mask_id;
    n->blendif = o->blendif;
    n->feathering_radius = o->feathering_radius;
    n->feathering_guide = o->feathering_guide;
    n->blur_radius = o->blur_radius;
    n->contrast = o->contrast;
    n->brightness = o->brightness;
    memcpy(n->blendif_parameters, o->blendif_parameters, sizeof(float) * 4 * DEVELOP_BLENDIF_SIZE);
    memcpy(n->raster_mask_source, o->raster_mask_source, sizeof(n->raster_mask_source));
    n->raster_mask_instance = o->raster_mask_instance;
    n->raster_mask_id = o->raster_mask_id;
    n->raster_mask_invert = o->raster_mask_invert;
    return 0;
  }

  if(old_version == 10 && new_version == 11)
  {
    /** blend legacy parameters version 10 */
    typedef struct dt_develop_blend_params10_t
    {
      /** what kind of masking to use: off, non-mask (uniformly), hand-drawn mask and/or conditional mask
       *  or raster mask */
      uint32_t mask_mode;
      /** blending color space type */
      int32_t blend_cst;
      /** blending mode */
      uint32_t blend_mode;
      /** parameter for the blending */
      float blend_parameter;
      /** mixing opacity */
      float opacity;
      /** how masks are combined */
      uint32_t mask_combine;
      /** id of mask in current pipeline */
      uint32_t mask_id;
      /** blendif mask */
      uint32_t blendif;
      /** feathering radius */
      float feathering_radius;
      /** feathering guide */
      uint32_t feathering_guide;
      /** blur radius */
      float blur_radius;
      /** mask contrast enhancement */
      float contrast;
      /** mask brightness adjustment */
      float brightness;
      /** some reserved fields for future use */
      uint32_t reserved[4];
      /** blendif parameters */
      float blendif_parameters[4 * DEVELOP_BLENDIF_SIZE];
      float blendif_boost_factors[DEVELOP_BLENDIF_SIZE];
      dt_dev_operation_t raster_mask_source;
      int raster_mask_instance;
      int raster_mask_id;
      gboolean raster_mask_invert;
    } dt_develop_blend_params10_t;

    if(length != sizeof(dt_develop_blend_params10_t)) return 1;

    dt_develop_blend_params10_t *o = (dt_develop_blend_params10_t *)old_params;
    dt_develop_blend_params_t *n = (dt_develop_blend_params_t *)new_params;

    *n = default_display_blend_params; // start with a fresh copy of default parameters
    n->mask_mode = o->mask_mode;
    n->blend_cst = o->blend_cst;
    n->blend_mode = _blend_legacy_blend_mode(o->blend_mode);
    n->blend_parameter = o->blend_parameter;
    n->opacity = o->opacity;
    n->mask_combine = o->mask_combine;
    n->mask_id = o->mask_id;
    n->blendif = o->blendif;
    n->feathering_radius = o->feathering_radius;
    n->feathering_guide = o->feathering_guide;
    n->blur_radius = o->blur_radius;
    n->contrast = o->contrast;
    n->brightness = o->brightness;
    // fix intermediate devel versions for details mask and initialize n->details to proper values if something was wrong
    memcpy(&n->details, &o->reserved, sizeof(float));
    if(isnan(n->details)) n->details = 0.0f;
    n->details = fminf(1.0f, fmaxf(-1.0f, n->details));

    memcpy(n->blendif_parameters, o->blendif_parameters, sizeof(float) * 4 * DEVELOP_BLENDIF_SIZE);
    memcpy(n->blendif_boost_factors, o->blendif_boost_factors, sizeof(float) * DEVELOP_BLENDIF_SIZE);
    memcpy(n->raster_mask_source, o->raster_mask_source, sizeof(n->raster_mask_source));
    n->raster_mask_instance = o->raster_mask_instance;
    n->raster_mask_id = o->raster_mask_id;
    n->raster_mask_invert = o->raster_mask_invert;
    return 0;
  }

  return 1;
}

int dt_develop_blend_legacy_params_from_so(dt_iop_module_so_t *module_so, const void *const old_params,
                                           const int old_version, void *new_params, const int new_version,
                                           const int length)
{
  // we need a dt_iop_module_t for dt_develop_blend_legacy_params()
  dt_iop_module_t *module = (dt_iop_module_t *)calloc(1, sizeof(dt_iop_module_t));
  if(dt_iop_load_module_by_so(module, module_so, NULL))
  {
    dt_free(module);
    return 1;
  }

  if(module->params_size == 0)
  {
    dt_iop_cleanup_module(module);
    dt_free(module);
    return 1;
  }

  // convert the old blend params to new
  const int res = dt_develop_blend_legacy_params(module, old_params, old_version,
                                                 new_params, dt_develop_blend_version(),
                                                 length);
  dt_iop_cleanup_module(module);
  dt_free(module);
  return res;
}

// tools/update_modelines.sh
// remove-trailing-space on;
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
