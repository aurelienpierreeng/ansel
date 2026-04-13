/*
    This file is part of darktable,
    Copyright (C) 2010-2011 Bruce Guenter.
    Copyright (C) 2010-2016 johannes hanika.
    Copyright (C) 2011 Antony Dovgal.
    Copyright (C) 2011 Henrik Andersson.
    Copyright (C) 2011 Kaminsky Andrey.
    Copyright (C) 2011 Olivier Tribout.
    Copyright (C) 2011 Robert Bieber.
    Copyright (C) 2011 Sergey Pavlov.
    Copyright (C) 2011-2014, 2016, 2019 Tobias Ellinghaus.
    Copyright (C) 2011-2012, 2014-2017 Ulrich Pegelow.
    Copyright (C) 2012, 2015, 2020-2022 Aldric Renaudin.
    Copyright (C) 2012, 2014-2015 Pascal de Bruijn.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2013-2016, 2019 Roman Lebedev.
    Copyright (C) 2013 Thomas Pryds.
    Copyright (C) 2014-2017 Dan Torop.
    Copyright (C) 2015-2016 Pedro Côrte-Real.
    Copyright (C) 2017-2019 Heiko Bauke.
    Copyright (C) 2017, 2019 Ingo Liebhardt.
    Copyright (C) 2017, 2019, 2021 luzpaz.
    Copyright (C) 2017 Peter Budai.
    Copyright (C) 2018-2020, 2023-2026 Aurélien PIERRE.
    Copyright (C) 2018-2019 Edgardo Hoszowski.
    Copyright (C) 2018 Kelvie Wong.
    Copyright (C) 2018 Maurizio Paglia.
    Copyright (C) 2018, 2020-2022 Pascal Obry.
    Copyright (C) 2018 rawfiner.
    Copyright (C) 2019 Andreas Schneider.
    Copyright (C) 2019-2022 Hanno Schwalm.
    Copyright (C) 2020 Chris Elston.
    Copyright (C) 2020, 2022 Diederik Ter Rahe.
    Copyright (C) 2020 Felipe Contreras.
    Copyright (C) 2020-2021 Hubert Kowalski.
    Copyright (C) 2020-2021 Ralf Brown.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Philipp Lutz.
    Copyright (C) 2022 Victor Forsiuk.
    Copyright (C) 2023 Alynx Zhou.
    Copyright (C) 2025 Guillaume Stutin.
    
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

#include "common/darktable.h"
#include "common/imagebuf.h"
#include "common/image_cache.h"
#include "common/interpolation.h"
#include "common/math.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/format.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "develop/masks.h"
#include "develop/openmp_maths.h"
#include "develop/tiling.h"

#include "bauhaus/bauhaus.h"
#include "common/colorspaces.h"
#include "control/conf.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/bspline.h"

#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <memory.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <complex.h>
#include <glib.h>

#ifdef __GNUC__
  #define INLINE __inline
#else
  #define INLINE inline
#endif

#define DEMOSAIC_XTRANS 1024 // masks for non-Bayer demosaic ops
#define DEMOSAIC_DUAL 2048   // masks for dual demosaicing methods
#define REDUCESIZE 64

#define XTRANS_SNAPPER 3
#define BAYER_SNAPPER 2
#define DOWNSAMPLE_GUIDED_SCALES 3

DT_MODULE_INTROSPECTION(4, dt_iop_demosaic_params_t)

typedef enum dt_iop_demosaic_method_t
{
  // methods for Bayer images
  DT_IOP_DEMOSAIC_PPG = 0,   // $DESCRIPTION: "PPG"
  DT_IOP_DEMOSAIC_AMAZE = 1, // $DESCRIPTION: "AMaZE"
  DT_IOP_DEMOSAIC_VNG4 = 2,  // $DESCRIPTION: "VNG4"
  DT_IOP_DEMOSAIC_RCD = 5,   // $DESCRIPTION: "RCD"
  DT_IOP_DEMOSAIC_LMMSE = 6, // $DESCRIPTION: "LMMSE"
  DT_IOP_DEMOSAIC_RCD_VNG = DEMOSAIC_DUAL | DT_IOP_DEMOSAIC_RCD, // $DESCRIPTION: "RCD + VNG4"
  DT_IOP_DEMOSAIC_AMAZE_VNG = DEMOSAIC_DUAL | DT_IOP_DEMOSAIC_AMAZE, // $DESCRIPTION: "AMaZE + VNG4"
  DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME = 3, // $DESCRIPTION: "passthrough (monochrome)"
  DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR = 4, // $DESCRIPTION: "photosite color (debug)"
  // methods for x-trans images
  DT_IOP_DEMOSAIC_VNG = DEMOSAIC_XTRANS | 0,           // $DESCRIPTION: "VNG"
  DT_IOP_DEMOSAIC_MARKESTEIJN = DEMOSAIC_XTRANS | 1,   // $DESCRIPTION: "Markesteijn 1-pass"
  DT_IOP_DEMOSAIC_MARKESTEIJN_3 = DEMOSAIC_XTRANS | 2, // $DESCRIPTION: "Markesteijn 3-pass"
  DT_IOP_DEMOSAIC_FDC = DEMOSAIC_XTRANS | 4,           // $DESCRIPTION: "frequency domain chroma"
  DT_IOP_DEMOSAIC_MARKEST3_VNG = DEMOSAIC_DUAL | DT_IOP_DEMOSAIC_MARKESTEIJN_3, // $DESCRIPTION: "Markesteijn 3-pass + VNG"
  DT_IOP_DEMOSAIC_PASSTHR_MONOX = DEMOSAIC_XTRANS | 3, // $DESCRIPTION: "passthrough (monochrome)"
  DT_IOP_DEMOSAIC_PASSTHR_COLORX = DEMOSAIC_XTRANS | 5, // $DESCRIPTION: "photosite color (debug)"
  DT_IOP_DEMOSAIC_DOWNSAMPLE = 7, // $DESCRIPTION: "downsample"
} dt_iop_demosaic_method_t;

typedef enum dt_iop_demosaic_greeneq_t
{
  DT_IOP_GREEN_EQ_NO = 0,    // $DESCRIPTION: "disabled"
  DT_IOP_GREEN_EQ_LOCAL = 1, // $DESCRIPTION: "local average"
  DT_IOP_GREEN_EQ_FULL = 2,  // $DESCRIPTION: "full average"
  DT_IOP_GREEN_EQ_BOTH = 3   // $DESCRIPTION: "full and local average"
} dt_iop_demosaic_greeneq_t;


typedef enum dt_iop_demosaic_smooth_t
{
  DEMOSAIC_SMOOTH_OFF = 0, // $DESCRIPTION: "disabled"
  DEMOSAIC_SMOOTH_1 = 1,   // $DESCRIPTION: "once"
  DEMOSAIC_SMOOTH_2 = 2,   // $DESCRIPTION: "twice"
  DEMOSAIC_SMOOTH_3 = 3,   // $DESCRIPTION: "three times"
  DEMOSAIC_SMOOTH_4 = 4,   // $DESCRIPTION: "four times"
  DEMOSAIC_SMOOTH_5 = 5,   // $DESCRIPTION: "five times"
} dt_iop_demosaic_smooth_t;

typedef enum dt_iop_demosaic_lmmse_t
{
  LMMSE_REFINE_0 = 0,   // $DESCRIPTION: "basic"
  LMMSE_REFINE_1 = 1,   // $DESCRIPTION: "median"
  LMMSE_REFINE_2 = 2,   // $DESCRIPTION: "3x median"
  LMMSE_REFINE_3 = 3,   // $DESCRIPTION: "refine & medians"
  LMMSE_REFINE_4 = 4,   // $DESCRIPTION: "2x refine + medians"
} dt_iop_demosaic_lmmse_t;

typedef struct dt_iop_demosaic_global_data_t
{
  // demosaic pattern
  int kernel_green_eq_lavg;
  int kernel_green_eq_favg_reduce_first;
  int kernel_green_eq_favg_reduce_second;
  int kernel_green_eq_favg_apply;
  int kernel_pre_median;
  int kernel_passthrough_monochrome;
  int kernel_passthrough_color;
  int kernel_ppg_green;
  int kernel_ppg_redblue;
  int kernel_zoom_half_size;
  int kernel_downsample;
  int kernel_border_interpolate;
  int kernel_color_smoothing;
  int kernel_zoom_passthrough_monochrome;
  int kernel_vng_border_interpolate;
  int kernel_vng_lin_interpolate;
  int kernel_zoom_third_size;
  int kernel_zoom_half_size_xtrans;
  int kernel_guided_laplacian_coefficients;
  int kernel_guided_laplacian_normalize;
  int kernel_guided_laplacian_apply;
  int kernel_guided_laplacian_finalize;
  int kernel_bspline_horizontal;
  int kernel_bspline_vertical;
  int kernel_bspline_horizontal_local;
  int kernel_bspline_vertical_local;
  int kernel_vng_green_equilibrate;
  int kernel_vng_interpolate;
  int kernel_markesteijn_initial_copy;
  int kernel_markesteijn_green_minmax;
  int kernel_markesteijn_interpolate_green;
  int kernel_markesteijn_solitary_green;
  int kernel_markesteijn_recalculate_green;
  int kernel_markesteijn_red_and_blue;
  int kernel_markesteijn_interpolate_twoxtwo;
  int kernel_markesteijn_convert_yuv;
  int kernel_markesteijn_differentiate;
  int kernel_markesteijn_homo_threshold;
  int kernel_markesteijn_homo_set;
  int kernel_markesteijn_homo_sum;
  int kernel_markesteijn_homo_max;
  int kernel_markesteijn_homo_max_corr;
  int kernel_markesteijn_homo_quench;
  int kernel_markesteijn_zero;
  int kernel_markesteijn_accu;
  int kernel_markesteijn_final;
  int kernel_rcd_populate;
  int kernel_rcd_write_output;
  int kernel_rcd_step_1;
  int kernel_rcd_step_2_1;
  int kernel_rcd_step_3_1;
  int kernel_rcd_step_4_1;
  int kernel_rcd_step_4_2;
  int kernel_rcd_step_5_1;
  int kernel_rcd_step_5_2;
  int kernel_rcd_border_redblue;
  int kernel_rcd_border_green;
  int kernel_write_blended_dual;
  float *lmmse_gamma_in;
  float *lmmse_gamma_out;
} dt_iop_demosaic_global_data_t;


typedef struct dt_iop_demosaic_data_t
{
  uint32_t green_eq;
  uint32_t color_smoothing;
  uint32_t demosaicing_method;
  uint32_t lmmse_refine;
  float median_thrs;
  double CAM_to_RGB[3][4];
  float dual_thrs;
} dt_iop_demosaic_data_t;


static inline __attribute__((always_inline)) float intp(float a, float b, float c)
{   // taken from rt code
    // calculate a * b + (1 - a) * c
    // following is valid:
    // intp(a, b+x, c+x) = intp(a, b, c) + x
    // intp(a, b*x, c*x) = intp(a, b, c) * x
    return a * (b - c) + c;
}

typedef enum dt_iop_demosaic_quality_t
{
  DT_DEMOSAIC_FAST = 0,
  DT_DEMOSAIC_FAIR = 1,
  DT_DEMOSAIC_BEST = 2
} dt_iop_demosaic_quality_t;


typedef struct dt_iop_demosaic_params_t
{
  dt_iop_demosaic_greeneq_t green_eq; // $DEFAULT: DT_IOP_GREEN_EQ_NO $DESCRIPTION: "match greens"
  float median_thrs; // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "edge threshold"
  dt_iop_demosaic_smooth_t color_smoothing; // $DEFAULT: DEMOSAIC_SMOOTH_OFF $DESCRIPTION: "color smoothing"
  dt_iop_demosaic_method_t demosaicing_method; // $DEFAULT: DT_IOP_DEMOSAIC_RCD $DESCRIPTION: "demosaicing method"
  dt_iop_demosaic_lmmse_t lmmse_refine; // $DEFAULT: LMMSE_REFINE_1 $DESCRIPTION: "LMMSE refine"
  float dual_thrs; // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.20 $DESCRIPTION: "dual threshold"
} dt_iop_demosaic_params_t;

typedef struct dt_iop_demosaic_gui_data_t
{
  GtkWidget *median_thrs;
  GtkWidget *greeneq;
  GtkWidget *color_smoothing;
  GtkWidget *demosaic_method_bayer;
  GtkWidget *demosaic_method_xtrans;
  GtkWidget *dual_thrs;
  GtkWidget *lmmse_refine;
  gboolean visual_mask;
} dt_iop_demosaic_gui_data_t;

// Implemented on amaze_demosaic_RT.cc
void amaze_demosaic_RT(
    const dt_dev_pixelpipe_iop_t *piece,
    const float *const in,
    float *out,
    const dt_iop_roi_t *const roi_in,
    const dt_iop_roi_t *const roi_out,
    const uint32_t filters);


// Mind the order of includes, there are internal dependencies
// FIXME: handle all the branching uniformingly
#include "demosaic/basic.c"
#include "demosaic/passthrough.c"
#include "demosaic/rcd.c"
#include "demosaic/lmmse.c"
#include "demosaic/ppg.c"
#include "demosaic/vng.c"
#include "demosaic/markesteijn.c"
#include "demosaic/dual.c"


const char *name()
{
  return _("demosaic");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("reconstruct full RGB pixels from a sensor color filter array reading"),
                                      _("mandatory"),
                                      _("linear, raw, scene-referred"),
                                      _("linear, raw"),
                                      _("linear, RGB, scene-referred"));
}

int default_group()
{
  return IOP_GROUP_TECHNICAL;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_ONE_INSTANCE;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RAW;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  typedef struct dt_iop_demosaic_params_t dt_iop_demosaic_params_v4_t;
  typedef struct dt_iop_demosaic_params_v3_t
  {
    dt_iop_demosaic_greeneq_t green_eq;
    float median_thrs;
    uint32_t color_smoothing;
    dt_iop_demosaic_method_t demosaicing_method;
    dt_iop_demosaic_lmmse_t lmmse_refine;
  } dt_iop_demosaic_params_v3_t;

  if(old_version == 3 && new_version == 4)
  {
    dt_iop_demosaic_params_v3_t *o = (dt_iop_demosaic_params_v3_t *)old_params;
    dt_iop_demosaic_params_v4_t *n = (dt_iop_demosaic_params_v4_t *)new_params;
    memcpy(n, o, sizeof *o);
    n->dual_thrs = 0.20f;
    return 0;
  }

  if(old_version == 2 && new_version == 3)
  {
    dt_iop_demosaic_params_t *o = (dt_iop_demosaic_params_t *)old_params;
    dt_iop_demosaic_params_t *n = (dt_iop_demosaic_params_t *)new_params;
    n->green_eq = o->green_eq;
    n->median_thrs = o->median_thrs;
    n->color_smoothing = 0;
    n->demosaicing_method = DT_IOP_DEMOSAIC_PPG;
    n->lmmse_refine = LMMSE_REFINE_1;
    return 0;
  }
  return 1;
}

void input_format(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                  dt_iop_buffer_dsc_t *dsc)
{
  default_input_format(self, pipe, piece, dsc);
  dsc->channels = 1;
  dt_iop_buffer_dsc_update_bpp(dsc);
}

void output_format(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                   dt_iop_buffer_dsc_t *dsc)
{
  dsc->channels = 4;
  dsc->datatype = TYPE_FLOAT;
  dsc->cst = IOP_CS_RGB;
}

static inline __attribute__((always_inline)) const char* method2string(dt_iop_demosaic_method_t method)
{
  const char *string;

  switch(method)
  {
    case DT_IOP_DEMOSAIC_PPG:
      string = "PPG";
      break;
    case DT_IOP_DEMOSAIC_AMAZE:
      string = "AMaZE";
      break;
    case DT_IOP_DEMOSAIC_VNG4:
      string = "VNG4";
      break;
    case DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME:
      string = "passthrough monochrome";
      break;
    case DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR:
      string = "photosites";
      break;
    case DT_IOP_DEMOSAIC_RCD:
      string = "RCD";
      break;
    case DT_IOP_DEMOSAIC_LMMSE:
      string = "LMMSE";
      break;
    case DT_IOP_DEMOSAIC_RCD_VNG:
      string = "RCD + VNG4";
      break;
    case DT_IOP_DEMOSAIC_AMAZE_VNG:
      string = "AMaZE + VNG4";
      break;
    case DT_IOP_DEMOSAIC_VNG:
      string = "VNG (xtrans)";
      break;
    case DT_IOP_DEMOSAIC_MARKESTEIJN:
      string = "Markesteijn-1 (XTrans)";
      break;
    case DT_IOP_DEMOSAIC_MARKESTEIJN_3:
      string = "Markesteijn-3 (XTrans)";
      break;
    case DT_IOP_DEMOSAIC_MARKEST3_VNG:
      string = "Markesteijn 3-pass + VNG";
      break;
    case DT_IOP_DEMOSAIC_FDC:
      string = "Frequency Domain Chroma (XTrans)";
      break;
    case DT_IOP_DEMOSAIC_PASSTHR_MONOX:
      string = "passthrough monochrome (XTrans)";
      break;
    case DT_IOP_DEMOSAIC_PASSTHR_COLORX:
      string = "photosites (XTrans)";
      break;
    case DT_IOP_DEMOSAIC_DOWNSAMPLE:
      string = "downsample";
      break;
    default:
      string = "(unknown method)";
  }
  return string;
}

static inline gboolean _is_downsample_method(const dt_iop_demosaic_method_t method)
{
  return method == DT_IOP_DEMOSAIC_DOWNSAMPLE;
}

/**
 * @brief Build one half-size RGB pixel from the 2x2 CFA block backing it.
 *
 * The loop walks the sensor block that maps to the output pixel and keeps only the
 * real photosites that actually exist there. Bayer gets one red, one blue and two
 * greens. 4Bayer keeps the four camera primaries, which are converted to RGB right
 * away so the caller stays in the regular RGB pipe.
 */
__DT_CLONE_TARGETS__
static void _downsample_bayer_half_size(float *const out, const float *const in,
                                        const dt_iop_roi_t *const roi_out,
                                        const dt_iop_roi_t *const roi_in, const uint32_t filters,
                                        const gboolean is_4bayer, const double CAM_to_RGB[3][4])
{
  __OMP_PARALLEL_FOR__(collapse(2))
  for(int y = 0; y < roi_out->height; y++)
  {
    for(int x = 0; x < roi_out->width; x++)
    {
      float *const outc = out + 4 * ((size_t)y * roi_out->width + x);
      dt_aligned_pixel_t cam = { 0.0f };
      int samples[4] = { 0 };
      const int px = MIN(2 * x, roi_in->width - 1);
      const int py = MIN(2 * y, roi_in->height - 1);

      // Collect the 2x2 source block feeding this output pixel and average only
      // same-colour photosites together so we do not invent new chroma detail.
      for(int j = 0; j < 2; j++)
      {
        for(int i = 0; i < 2; i++)
        {
          const int xx = MIN(px + i, roi_in->width - 1);
          const int yy = MIN(py + j, roi_in->height - 1);
          const int c = FC(yy, xx, filters);
          cam[c] += in[(size_t)yy * roi_in->width + xx];
          samples[c]++;
        }
      }

      for(int c = 0; c < 4; c++)
        if(samples[c] > 0) cam[c] /= (float)samples[c];

      if(is_4bayer)
      {
        for(int c = 0; c < 3; c++)
        {
          outc[c] = 0.0f;
          for(int k = 0; k < 4; k++) outc[c] += CAM_to_RGB[c][k] * cam[k];
        }
      }
      else
      {
        outc[0] = cam[RED];
        outc[1] = cam[GREEN];
        outc[2] = cam[BLUE];
      }

      outc[3] = 0.0f;
    }
  }
}

/**
 * @brief Reconstruct one missing X-Trans colour from the nearest surrounding photosites.
 *
 * We first look for the closest sample in each quadrant around the half-size pixel centre.
 * When the CFA leaves a 2-pixel chroma gap, these four neighbours define the smallest local
 * rectangle of same-colour samples and we bilinearly interpolate inside it. If the search hits
 * an image edge, we fall back to averaging the available neighbours instead of extrapolating.
 */
__DT_CLONE_TARGETS__
static float _downsample_xtrans_missing_colour(const float *const in, const dt_iop_roi_t *const roi_in,
                                               const int px, const int py,
                                               const uint8_t (*const xtrans)[6], const int colour)
{
  const float cx = px + 0.5f;
  const float cy = py + 0.5f;
  const int xmin = MAX(0, px - 3);
  const int xmax = MIN(roi_in->width - 1, px + 4);
  const int ymin = MAX(0, py - 3);
  const int ymax = MIN(roi_in->height - 1, py + 4);

  float quadrant_value[4] = { 0.0f };
  float quadrant_dist[4] = { INFINITY, INFINITY, INFINITY, INFINITY };
  int quadrant_x[4] = { 0 };
  int quadrant_y[4] = { 0 };
  gboolean quadrant_valid[4] = { FALSE, FALSE, FALSE, FALSE };

  float nearest_value = 0.0f;
  float nearest_dist = INFINITY;

  // Search the local 7x7 neighbourhood because X-Trans can place the next sample
  // of a given colour two pixels away from the 2x2 block feeding the output.
  for(int yy = ymin; yy <= ymax; yy++)
  {
    for(int xx = xmin; xx <= xmax; xx++)
    {
      if(FCxtrans(yy, xx, roi_in, xtrans) != colour) continue;

      const float dx = xx - cx;
      const float dy = yy - cy;
      const float dist2 = dx * dx + dy * dy;
      if(dist2 < nearest_dist)
      {
        nearest_dist = dist2;
        nearest_value = in[(size_t)yy * roi_in->width + xx];
      }

      const int quadrant = ((yy > cy) ? 2 : 0) + ((xx > cx) ? 1 : 0);
      if(dist2 < quadrant_dist[quadrant])
      {
        quadrant_dist[quadrant] = dist2;
        quadrant_value[quadrant] = in[(size_t)yy * roi_in->width + xx];
        quadrant_x[quadrant] = xx;
        quadrant_y[quadrant] = yy;
        quadrant_valid[quadrant] = TRUE;
      }
    }
  }

  if(quadrant_valid[0] && quadrant_valid[1] && quadrant_valid[2] && quadrant_valid[3])
  {
    const float x_left = 0.5f * (quadrant_x[0] + quadrant_x[2]);
    const float x_right = 0.5f * (quadrant_x[1] + quadrant_x[3]);
    const float y_top = 0.5f * (quadrant_y[0] + quadrant_y[1]);
    const float y_bottom = 0.5f * (quadrant_y[2] + quadrant_y[3]);
    const float tx = CLAMP((cx - x_left) / MAX(x_right - x_left, 1e-6f), 0.0f, 1.0f);
    const float ty = CLAMP((cy - y_top) / MAX(y_bottom - y_top, 1e-6f), 0.0f, 1.0f);
    const float top = quadrant_value[0] + tx * (quadrant_value[1] - quadrant_value[0]);
    const float bottom = quadrant_value[2] + tx * (quadrant_value[3] - quadrant_value[2]);
    return top + ty * (bottom - top);
  }

  float sum = 0.0f;
  int count = 0;
  for(int q = 0; q < 4; q++)
  {
    if(!quadrant_valid[q]) continue;
    sum += quadrant_value[q];
    count++;
  }

  return (count > 0) ? sum / (float)count : nearest_value;
}

/**
 * @brief Build one half-size RGB pixel from a 2x2 X-Trans block.
 *
 * We first reuse any real photosites that fall inside the 2x2 block mapped to the
 * output pixel. When a colour is absent from that block, we reconstruct it from the
 * nearest surrounding same-colour samples so the half-size image keeps all channels.
 */
__DT_CLONE_TARGETS__
static void _downsample_xtrans_half_size(float *const out, const float *const in,
                                         const dt_iop_roi_t *const roi_out,
                                         const dt_iop_roi_t *const roi_in,
                                         const uint8_t (*const xtrans)[6])
{
  __OMP_PARALLEL_FOR__(collapse(2))
  for(int y = 0; y < roi_out->height; y++)
  {
    for(int x = 0; x < roi_out->width; x++)
    {
      float *const outc = out + 4 * ((size_t)y * roi_out->width + x);
      dt_aligned_pixel_t rgb = { 0.0f };
      int samples[3] = { 0 };
      const int px = MIN(2 * x, roi_in->width - 1);
      const int py = MIN(2 * y, roi_in->height - 1);

      // Reuse the real X-Trans photosites that already fall inside the source 2x2 block
      // and only interpolate the colours that the local pattern does not sample there.
      for(int j = 0; j < 2; j++)
      {
        for(int i = 0; i < 2; i++)
        {
          const int xx = MIN(px + i, roi_in->width - 1);
          const int yy = MIN(py + j, roi_in->height - 1);
          const int c = FCxtrans(yy, xx, roi_in, xtrans);
          rgb[c] += in[(size_t)yy * roi_in->width + xx];
          samples[c]++;
        }
      }

      for(int c = 0; c < 3; c++)
      {
        if(samples[c] > 0)
          outc[c] = rgb[c] / (float)samples[c];
        else
          outc[c] = _downsample_xtrans_missing_colour(in, roi_in, px, py, xtrans, c);
      }

      outc[3] = 0.0f;
    }
  }
}

/**
 * @brief Fit one local affine RGB model for the current high-frequency scale.
 *
 * For each pixel we collect a dense immediate 5x5 patch and accumulate its raw RGB
 * and guide moments. We then recover the channel-wise means plus the variance of the
 * shared RGB-average guide from those fixed-size sums, which keeps the hot loop free
 * from running-mean updates and per-sample divisions.
 * The three target-channel slopes are stored as one RGB pixel so the apply stage can
 * evaluate them from the same scalar guide with minimal scratch memory. The intercept
 * is kept in a separate RGB image so its three channel-wise components can be blurred
 * just like the slopes before we reconstruct the filtered detail.
 */
__DT_CLONE_TARGETS__
static void _downsample_guided_laplacian_fit(const float *const restrict HF,
                                             float *const restrict coeff,
                                             float *const restrict bias,
                                             const size_t width, const size_t height)
{
  const float eps = 1e-12f;
  const dt_aligned_pixel_simd_t zero = dt_simd_set1(0.f);
  const dt_aligned_pixel_simd_t inv_patch = dt_simd_set1(1.f / 25.f);
  __OMP_PARALLEL_FOR__()
  for(size_t row = 0; row < height; ++row)
  {
    const float *const row0 = HF + 4 * ((size_t)CLAMP((int)row - 2, 0, (int)height - 1) * width);
    const float *const row1 = HF + 4 * ((size_t)CLAMP((int)row - 1, 0, (int)height - 1) * width);
    const float *const row2 = HF + 4 * (row * width);
    const float *const row3 = HF + 4 * ((size_t)CLAMP((int)row + 1, 0, (int)height - 1) * width);
    const float *const row4 = HF + 4 * ((size_t)CLAMP((int)row + 2, 0, (int)height - 1) * width);
    const float *const rows[BSPLINE_FSIZE] = { row0, row1, row2, row3, row4 };
    const int max_col = (int)width - 1;

    for(size_t col = 0; col < width; ++col)
    {
      dt_aligned_pixel_simd_t sum_rgb = zero;
      dt_aligned_pixel_simd_t sum_rgb_guide = zero;
      float sum_guide = 0.f;
      float sum_guide_sq = 0.f;
      const int col_offsets[BSPLINE_FSIZE]
        = { 4 * CLAMP((int)col - 2, 0, max_col),
            4 * CLAMP((int)col - 1, 0, max_col),
            4 * (int)col,
            4 * CLAMP((int)col + 1, 0, max_col),
            4 * CLAMP((int)col + 2, 0, max_col) };

      // Walk the dense 5x5 neighbourhood once. The 5 clamped column offsets are
      // hoisted once per output pixel so the inner loop keeps only loads and moment
      // accumulation for the RGB channels and their shared guide. Keep those loops
      // as counted loops so GCC does not fully unroll all 25 taps and spill the
      // intermediate guide terms to the stack.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC unroll 1
#endif
      for(int jj = 0; jj < BSPLINE_FSIZE; ++jj)
      {
        const float *const row_ptr = rows[jj];
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC unroll 1
#endif
        for(int ii = 0; ii < BSPLINE_FSIZE; ++ii)
        {
          const dt_aligned_pixel_simd_t sample = dt_load_simd_aligned(row_ptr + col_offsets[ii]);
          const float guide = (sample[RED] + sample[GREEN] + sample[BLUE]) / 3.f;

          sum_rgb += sample;
          sum_guide += guide;
          sum_guide_sq += guide * guide;
          sum_rgb_guide += sample * dt_simd_set1(guide);
        }
      }

      dt_aligned_pixel_simd_t means = sum_rgb * inv_patch;
      const float guide_mean = sum_guide * (1.f / 25.f);
      float variance = sum_guide_sq * (1.f / 25.f) - sqf(guide_mean);
      dt_aligned_pixel_simd_t covariance = sum_rgb_guide * inv_patch - means * dt_simd_set1(guide_mean);
      means[ALPHA] = 0.f;
      covariance[ALPHA] = 0.f;

      if(variance < 0.f) variance = 0.f;

      dt_aligned_pixel_simd_t slope = zero;
      if(variance > eps) slope = covariance / dt_simd_set1(variance);
      slope[ALPHA] = 0.f;

      dt_aligned_pixel_simd_t intercept = means - slope * dt_simd_set1(guide_mean);
      intercept[ALPHA] = 0.f;

      dt_store_simd_aligned(coeff + 4 * (row * width + col), slope);
      dt_store_simd_aligned(bias + 4 * (row * width + col), intercept);
    }
  }
}

/**
 * @brief Apply the locally averaged affine RGB model to one high-frequency layer.
 *
 * The coefficients and intercept have already been smoothed with the same a-trous
 * kernel as the wavelet scale, so we simply predict each channel from the local RGB
 * HF vector, add the channel-wise intercept, and accumulate the filtered detail bands
 * for later resynthesis.
 */
__DT_CLONE_TARGETS__
static void _downsample_guided_laplacian_apply(const float *const restrict HF,
                                               const float *const restrict coeff,
                                               const float *const restrict bias,
                                               const float *const restrict LF,
                                               float *const restrict reconstructed,
                                               const size_t width, const size_t height,
                                               const gboolean reset)
{
  __OMP_PARALLEL_FOR__()
  for(size_t row = 0; row < height; ++row)
  {
    for(size_t col = 0; col < width; ++col)
    {
      const size_t index = 4 * (row * width + col);
      const dt_aligned_pixel_simd_t hf = dt_load_simd_aligned(HF + index);
      const dt_aligned_pixel_simd_t guide = dt_simd_set1((hf[RED] + hf[GREEN] + hf[BLUE]) / 3.f);
      dt_aligned_pixel_simd_t filtered = (dt_load_simd_aligned(coeff + index) * guide
                                          + dt_load_simd_aligned(bias + index))
                                         * dt_load_simd_aligned(LF + index);

      if(!reset) filtered += dt_load_simd_aligned(reconstructed + index);
      filtered[ALPHA] = 0.f;
      dt_store_simd_aligned(reconstructed + index, filtered);
    }
  }
}

/**
 * @brief Denoise the half-size demosaic result by filtering its wavelet details.
 *
 * We decompose the half-size RGB image over three a-trous B-spline scales. At each
 * scale we normalize the high-frequency layer by the blur that produced it, fit a
 * local affine RGB model on that relative detail, blur one RGB slope field plus one
 * RGB intercept field over the immediate neighbourhood, then reconstruct a filtered relative
 * detail layer and rescale it back by the current blur. Repeating the full three-scale
 * pass lets the module gradually even out chroma details over several user-selected
 * iterations. The output is the sum of all filtered high frequencies plus the final
 * low-frequency residual.
 */
__DT_CLONE_TARGETS__
static int _downsample_guided_laplacian_postfilter(float *const out,
                                                   const size_t width, const size_t height,
                                                   const int iterations)
{
  if(iterations <= 0) return 0;

  const size_t pixels = width * height;
  float *const restrict LF_even = dt_pixelpipe_cache_alloc_align_float_cache(4 * pixels, 0);
  float *const restrict LF_odd = dt_pixelpipe_cache_alloc_align_float_cache(4 * pixels, 0);
  float *const restrict HF = dt_pixelpipe_cache_alloc_align_float_cache(4 * pixels, 0);
  float *const restrict reconstructed = dt_pixelpipe_cache_alloc_align_float_cache(4 * pixels, 0);
  float *const restrict coeff = dt_pixelpipe_cache_alloc_align_float_cache(4 * pixels, 0);
  float *const restrict bias = dt_pixelpipe_cache_alloc_align_float_cache(4 * pixels, 0);
  float *const restrict coeff_tmp = dt_pixelpipe_cache_alloc_align_float_cache(4 * pixels, 0);
  size_t padded_size;
  float *const restrict tempbuf = dt_pixelpipe_cache_alloc_perthread_float(4 * width, &padded_size);
  const float *restrict residual = out;
  int err = 0;

  if(IS_NULL_PTR(LF_even) || IS_NULL_PTR(LF_odd) || IS_NULL_PTR(HF) || IS_NULL_PTR(reconstructed) || IS_NULL_PTR(coeff) || IS_NULL_PTR(bias) || IS_NULL_PTR(coeff_tmp) || IS_NULL_PTR(tempbuf))
  {
    err = 1;
    goto cleanup;
  }

  for(int iteration = 0; iteration < iterations; ++iteration)
  {
    residual = out;

    for(int s = 0; s < DOWNSAMPLE_GUIDED_SCALES; ++s)
    {
      const int mult = 1 << s;
      const float *restrict buffer_in;
      float *restrict buffer_out;

      if(s == 0)
      {
        buffer_in = out;
        buffer_out = LF_odd;
      }
      else if(s % 2 != 0)
      {
        buffer_in = LF_odd;
        buffer_out = LF_even;
      }
      else
      {
        buffer_in = LF_even;
        buffer_out = LF_odd;
      }

      decompose_2D_Bspline(buffer_in, HF, buffer_out, width, height, mult, tempbuf, padded_size);
      // Express the current wavelet band as relative detail over the blur that created it
      // so the linear RGB model follows local chroma ratios instead of absolute amplitudes.
      __OMP_PARALLEL_FOR__()
      for(size_t row = 0; row < height; ++row)
      {
        for(size_t col = 0; col < width; ++col)
        {
          const size_t index = 4 * (row * width + col);
          dt_aligned_pixel_simd_t lf = dt_load_simd_aligned(buffer_out + index);
          lf[RED] = fmaxf(lf[RED], 1e-8f);
          lf[GREEN] = fmaxf(lf[GREEN], 1e-8f);
          lf[BLUE] = fmaxf(lf[BLUE], 1e-8f);
          lf[ALPHA] = 1.f;

          dt_aligned_pixel_simd_t normalized = dt_load_simd_aligned(HF + index) / lf;
          normalized[ALPHA] = 0.f;
          dt_store_simd_aligned(HF + index, normalized);
        }
      }

      _downsample_guided_laplacian_fit(HF, coeff, bias, width, height);

      blur_2D_Bspline(coeff, coeff_tmp, tempbuf, width, height, 1, FALSE);
      dt_iop_image_copy_by_size(coeff, coeff_tmp, width, height, 4);
      blur_2D_Bspline(bias, coeff_tmp, tempbuf, width, height, 1, FALSE);
      dt_iop_image_copy_by_size(bias, coeff_tmp, width, height, 4);

      _downsample_guided_laplacian_apply(HF, coeff, bias, buffer_out, reconstructed,
                                         width, height, s == 0);
      residual = buffer_out;
    }

    const gboolean last_iteration = (iteration == iterations - 1);
    __OMP_PARALLEL_FOR__()
    for(size_t row = 0; row < height; ++row)
    {
      for(size_t col = 0; col < width; ++col)
      {
        const size_t index = 4 * (row * width + col);
        dt_aligned_pixel_simd_t pixel
            = dt_simd_max_zero(dt_load_simd_aligned(reconstructed + index)
                               + dt_load_simd_aligned(residual + index));
        pixel[ALPHA] = 0.f;

        if(last_iteration)
          dt_store_simd_nontemporal(out + index, pixel);
        else
          dt_store_simd_aligned(out + index, pixel);
      }
    }
  }
  dt_omploop_sfence();  // ensure the final nontemporal writeback completes before the caller reads out

cleanup:
  dt_pixelpipe_cache_free_align(tempbuf);
  dt_pixelpipe_cache_free_align(coeff_tmp);
  dt_pixelpipe_cache_free_align(bias);
  dt_pixelpipe_cache_free_align(coeff);
  dt_pixelpipe_cache_free_align(reconstructed);
  dt_pixelpipe_cache_free_align(HF);
  dt_pixelpipe_cache_free_align(LF_odd);
  dt_pixelpipe_cache_free_align(LF_even);
  return err;
}


void distort_mask(struct dt_iop_module_t *self, const struct dt_dev_pixelpipe_t *pipe, struct dt_dev_pixelpipe_iop_t *piece,
                  const float *const in, float *const out, const dt_iop_roi_t *const roi_in,
                  const dt_iop_roi_t *const roi_out)
{
  (void)pipe;
  const struct dt_interpolation *itor = dt_interpolation_new(DT_INTERPOLATION_USERPREF);
  dt_interpolation_resample_roi_1c(itor, out, roi_out, in, roi_in);
}

void modify_roi_out(struct dt_iop_module_t *self, const struct dt_dev_pixelpipe_t *pipe,
                    struct dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out,
                    const dt_iop_roi_t *const roi_in)
{
  *roi_out = *roi_in;

  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;
  if(_is_downsample_method(data->demosaicing_method))
  {
    roi_out->width = (roi_in->width + 1) / 2;
    roi_out->height = (roi_in->height + 1) / 2;
  }

  // snap to start of mosaic block:
  roi_out->x = 0; // MAX(0, roi_out->x & ~1);
  roi_out->y = 0; // MAX(0, roi_out->y & ~1);
}

// which roi input is needed to process to this output?
// roi_out is unchanged, full buffer in is full buffer out.
// see ../../doc/resizing-scaling.md for details
void modify_roi_in(struct dt_iop_module_t *self, const struct dt_dev_pixelpipe_t *pipe,
                   struct dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *roi_out, dt_iop_roi_t *roi_in)
{
  // this op is disabled for filters == 0
  *roi_in = *roi_out;

  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;
  const int method = data->demosaicing_method;
  const gboolean passthrough = (method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME) ||
                               (method == DT_IOP_DEMOSAIC_PASSTHR_MONOX);
  const gboolean downsample = _is_downsample_method(method);

  if(downsample)
  {
    roi_in->x *= 2;
    roi_in->y *= 2;
    roi_in->width *= 2;
    roi_in->height *= 2;

    // Half-size mode maps each output pixel to one 2x2 raw block, so keep the exact
    // 2x addressing and clamp only the tail block against the available input buffer.
    roi_in->x = CLAMP(roi_in->x, 0, MAX(0, piece->buf_in.width - 1));
    roi_in->y = CLAMP(roi_in->y, 0, MAX(0, piece->buf_in.height - 1));
    roi_in->width = CLAMP(roi_in->width, 1, piece->buf_in.width - roi_in->x);
    roi_in->height = CLAMP(roi_in->height, 1, piece->buf_in.height - roi_in->y);
    return;
  }

  // set position to closest sensor pattern snap
  if(!passthrough)
  {
    // ROI planning happens during history -> pipeline resync, before recursion has initialized piece->dsc_in.
    // The snap period must therefore come from the immutable RAW input descriptor attached to the image.
    const int aligner = (piece->module->dev->image_storage.dsc.filters != 9u) ? BAYER_SNAPPER : XTRANS_SNAPPER;
    const int dx = roi_in->x % aligner;
    const int dy = roi_in->y % aligner;
    const int shift_x = (dx > aligner / 2) ? aligner - dx : -dx;
    const int shift_y = (dy > aligner / 2) ? aligner - dy : -dy;

    roi_in->x = MAX(0, roi_in->x + shift_x);
    roi_in->y = MAX(0, roi_in->y + shift_y);
  }
}


__DT_CLONE_TARGETS__
int process(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece,
            const void *const i, void *const o)
{
  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  const dt_image_t *img = &self->dev->image_storage;
  const float threshold = 0.0001f * img->exif_iso;
  dt_times_t start_time = { 0 }, end_time = { 0 };

  dt_iop_roi_t roi = *roi_in;
  dt_iop_roi_t roo = *roi_out;
  roo.x = roo.y = 0;
  // roi_out->scale = global scale: (iscale == 1.0, always when demosaic is on)
  const gboolean info = ((darktable.unmuted & (DT_DEBUG_DEMOSAIC | DT_DEBUG_PERF))
                         && (pipe->type == DT_DEV_PIXELPIPE_FULL));

  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->dsc_in.xtrans;

  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)self->global_data;

  int demosaicing_method = data->demosaicing_method;

  gboolean showmask = FALSE;
  if(self->dev->gui_attached && pipe->type == DT_DEV_PIXELPIPE_FULL)
  {
    dt_iop_demosaic_gui_data_t *g = (dt_iop_demosaic_gui_data_t *)self->gui_data;
    if(g) showmask = (g->visual_mask);
    // take care of passthru modes
    if(pipe->mask_display == DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU)
      demosaicing_method = (piece->dsc_in.filters != 9u) ? DT_IOP_DEMOSAIC_RCD : DT_IOP_DEMOSAIC_MARKESTEIJN;
    else if(pipe->mask_display == DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU_MONO)
      demosaicing_method = DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME;
  }

  const float *const pixels = (float *)i;

  // Full demosaic and then scaling if needed
  if(info) dt_get_times(&start_time);

  if(_is_downsample_method(demosaicing_method))
  {
    if(piece->dsc_in.filters == 9u)
      _downsample_xtrans_half_size(o, pixels, &roo, &roi, xtrans);
    else
      _downsample_bayer_half_size(o, pixels, &roo, &roi, piece->dsc_in.filters,
                                  img->flags & DT_IMAGE_4BAYER, data->CAM_to_RGB);

    if(_downsample_guided_laplacian_postfilter(o, roo.width, roo.height, data->color_smoothing)) return 1;
  }
  else if(demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME)
  {
    passthrough_monochrome(o, pixels, &roo, &roi);
  }
  else if(demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR)
  {
    passthrough_color(o, pixels, &roo, &roi, piece->dsc_in.filters, xtrans);
  }
  else if(piece->dsc_in.filters == 9u)
  {
    const int passes = (demosaicing_method == DT_IOP_DEMOSAIC_MARKESTEIJN) ? 1 : 3;
    if(demosaicing_method == DT_IOP_DEMOSAIC_MARKEST3_VNG)
      xtrans_markesteijn_interpolate(o, pixels, &roo, &roi, xtrans, passes);
    else if(demosaicing_method == DT_IOP_DEMOSAIC_FDC)
      xtrans_fdc_interpolate(self, o, pixels, &roo, &roi, xtrans);
    else if(demosaicing_method >= DT_IOP_DEMOSAIC_MARKESTEIJN)
      xtrans_markesteijn_interpolate(o, pixels, &roo, &roi, xtrans, passes);
    else
      if(vng_interpolate(o, pixels, &roo, &roi, piece->dsc_in.filters, xtrans, FALSE))
        return 1;
  }
  else
  {
    float *in = (float *)pixels;
    float *aux;

    if(!(img->flags & DT_IMAGE_4BAYER) && data->green_eq != DT_IOP_GREEN_EQ_NO)
    {
      in = dt_pixelpipe_cache_alloc_align_float((size_t)roi_in->height * roi_in->width, pipe);
      if(!IS_NULL_PTR(in))
      {
        switch(data->green_eq)
        {
          case DT_IOP_GREEN_EQ_FULL:
            green_equilibration_favg(in, pixels, roi_in->width, roi_in->height, piece->dsc_in.filters,
                                    roi_in->x, roi_in->y);
            break;
          case DT_IOP_GREEN_EQ_LOCAL:
            green_equilibration_lavg(in, pixels, roi_in->width, roi_in->height, piece->dsc_in.filters,
                                    roi_in->x, roi_in->y, threshold);
            break;
          case DT_IOP_GREEN_EQ_BOTH:
            aux = dt_pixelpipe_cache_alloc_align_float((size_t)roi_in->height * roi_in->width, pipe);
            if(IS_NULL_PTR(aux))
            {
              dt_pixelpipe_cache_free_align(in);
              return 1;
            }
            green_equilibration_favg(aux, pixels, roi_in->width, roi_in->height, piece->dsc_in.filters,
                                    roi_in->x, roi_in->y);
            green_equilibration_lavg(in, aux, roi_in->width, roi_in->height, piece->dsc_in.filters, roi_in->x,
                                    roi_in->y, threshold);
            dt_pixelpipe_cache_free_align(aux);
            break;
        }
      }
      else
      {
        return 1;
      }
    }

    if(demosaicing_method == DT_IOP_DEMOSAIC_VNG4 || (img->flags & DT_IMAGE_4BAYER))
    {
      if(vng_interpolate(o, in, &roo, &roi, piece->dsc_in.filters, xtrans, FALSE))
        return 1;
      if(img->flags & DT_IMAGE_4BAYER)
      {
        dt_colorspaces_cygm_to_rgb(o, roo.width*roo.height, data->CAM_to_RGB);
        dt_aligned_pixel_t processed_maximum = { piece->dsc_in.processed_maximum[0], piece->dsc_in.processed_maximum[1],
                                                 piece->dsc_in.processed_maximum[2], 0.0f };
        dt_colorspaces_cygm_to_rgb(processed_maximum, 1, data->CAM_to_RGB);
      }
    }
    else if((demosaicing_method & ~DEMOSAIC_DUAL) == DT_IOP_DEMOSAIC_RCD)
    {
      rcd_demosaic(piece, o, in, &roo, &roi, piece->dsc_in.filters);
    }
    else if(demosaicing_method == DT_IOP_DEMOSAIC_LMMSE)
    {
      if(IS_NULL_PTR(gd->lmmse_gamma_in))
      {
        gd->lmmse_gamma_in = dt_pixelpipe_cache_alloc_align_float_cache(65536, 0);
        gd->lmmse_gamma_out = dt_pixelpipe_cache_alloc_align_float_cache(65536, 0);
        if(IS_NULL_PTR(gd->lmmse_gamma_in) || IS_NULL_PTR(gd->lmmse_gamma_out))
        {
          dt_pixelpipe_cache_free_align(gd->lmmse_gamma_in);
          dt_pixelpipe_cache_free_align(gd->lmmse_gamma_out);
          gd->lmmse_gamma_in = NULL;
          gd->lmmse_gamma_out = NULL;
          if(!(img->flags & DT_IMAGE_4BAYER) && data->green_eq != DT_IOP_GREEN_EQ_NO)
            dt_pixelpipe_cache_free_align(in);
          return 1;
        }
#ifdef _OPENMP
  #pragma omp for
#endif
        for(int j = 0; j < 65536; j++)
        {
          const double x = (double)j / 65535.0;
          gd->lmmse_gamma_in[j]  = (x <= 0.001867) ? x * 17.0 : 1.044445 * exp(log(x) / 2.4) - 0.044445;
          gd->lmmse_gamma_out[j] = (x <= 0.031746) ? x / 17.0 : exp(log((x + 0.044445) / 1.044445) * 2.4);
        }
      }
      lmmse_demosaic(piece, o, in, &roo, &roi, piece->dsc_in.filters, data->lmmse_refine, gd->lmmse_gamma_in, gd->lmmse_gamma_out);
    }
    else if((demosaicing_method & ~DEMOSAIC_DUAL) != DT_IOP_DEMOSAIC_AMAZE)
    {
      if(demosaic_ppg(o, in, &roo, &roi, piece->dsc_in.filters, data->median_thrs))
      {
        if(!(img->flags & DT_IMAGE_4BAYER) && data->green_eq != DT_IOP_GREEN_EQ_NO)
          dt_pixelpipe_cache_free_align(in);
        return 1;
      }
    } // wanted ppg or zoomed out a lot and quality is limited to 1
    else
      amaze_demosaic_RT(piece, in, o, &roi, &roo, piece->dsc_in.filters);

    if(!(img->flags & DT_IMAGE_4BAYER) && data->green_eq != DT_IOP_GREEN_EQ_NO) 
      dt_pixelpipe_cache_free_align(in);
  }

  if(info)
  {
    const float mpixels = (roo.width * roo.height) / 1.0e6;
    dt_get_times(&end_time);
    const float tclock = end_time.clock - start_time.clock;
    const float uclock = end_time.user - start_time.user;
    fprintf(stderr," [demosaic] process CPU `%s' did %.2fmpix, %.4f secs (%.4f CPU), %.2f pix/us\n",
      method2string(demosaicing_method & ~DEMOSAIC_DUAL), mpixels, tclock, uclock, mpixels / tclock);
  }

  if((demosaicing_method & DEMOSAIC_DUAL))
  {
    if(dual_demosaic(pipe, piece, o, pixels, &roo, &roi, piece->dsc_in.filters, xtrans, showmask, data->dual_thrs))
      return 1;
  }

  if(data->color_smoothing && !_is_downsample_method(demosaicing_method))
    color_smoothing(o, roi_out, data->color_smoothing);
    
  return 0;
}

#ifdef HAVE_OPENCL
static int process_default_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                              const dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in,
                              cl_mem dev_out, const dt_iop_roi_t *const roi_in,
                              const dt_iop_roi_t *const roi_out, const int demosaicing_method)
{
  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)self->global_data;

  const int devid = pipe->devid;

  cl_mem dev_aux = NULL;
  cl_mem dev_tmp = NULL;
  cl_mem dev_med = NULL;
  cl_mem dev_green_eq = NULL;
  cl_int err = -999;

  int width = roi_out->width;
  int height = roi_out->height;

  // green equilibration
  if(data->green_eq != DT_IOP_GREEN_EQ_NO)
  {
    dev_green_eq = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float));
    if(IS_NULL_PTR(dev_green_eq)) goto error;

    if(!green_equilibration_cl(self, pipe, piece, dev_in, dev_green_eq, roi_in))
      goto error;

    dev_in = dev_green_eq;
  }

  // need to reserve scaled auxiliary buffer or use dev_out
  dev_aux = dev_out;

  if(demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME)
  {
    size_t sizes[3] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
    dt_opencl_set_kernel_arg(devid, gd->kernel_passthrough_monochrome, 0, sizeof(cl_mem), &dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_passthrough_monochrome, 1, sizeof(cl_mem), &dev_aux);
    dt_opencl_set_kernel_arg(devid, gd->kernel_passthrough_monochrome, 2, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_passthrough_monochrome, 3, sizeof(int), &height);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_passthrough_monochrome, sizes);
    if(err != CL_SUCCESS) goto error;
  }
  else if(demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR)
  {
    size_t sizes[3] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
    dt_opencl_set_kernel_arg(devid, gd->kernel_passthrough_color, 0, sizeof(cl_mem), &dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_passthrough_color, 1, sizeof(cl_mem), &dev_aux);
    dt_opencl_set_kernel_arg(devid, gd->kernel_passthrough_color, 2, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_passthrough_color, 3, sizeof(int), &height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_passthrough_color, 4, sizeof(int), (void *)&roi_in->x);
    dt_opencl_set_kernel_arg(devid, gd->kernel_passthrough_color, 5, sizeof(int), (void *)&roi_in->y);
    dt_opencl_set_kernel_arg(devid, gd->kernel_passthrough_color, 6, sizeof(uint32_t), (void *)&piece->dsc_in.filters);

    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_passthrough_color, sizes);
    if(err != CL_SUCCESS) goto error;
  }
  else if(demosaicing_method == DT_IOP_DEMOSAIC_PPG)
  {
    dev_tmp = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float) * 4);
    if(IS_NULL_PTR(dev_tmp)) goto error;

    {
      const int myborder = 3;
      // manage borders
      size_t sizes[3] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
      dt_opencl_set_kernel_arg(devid, gd->kernel_border_interpolate, 0, sizeof(cl_mem), &dev_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_border_interpolate, 1, sizeof(cl_mem), &dev_tmp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_border_interpolate, 2, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_border_interpolate, 3, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_border_interpolate, 4, sizeof(uint32_t), (void *)&piece->dsc_in.filters);
      dt_opencl_set_kernel_arg(devid, gd->kernel_border_interpolate, 5, sizeof(int), (void *)&myborder);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_border_interpolate, sizes);
      if(err != CL_SUCCESS) goto error;
    }

    if(data->median_thrs > 0.0f)
    {
      dev_med = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float) * 4);
      if(IS_NULL_PTR(dev_med)) goto error;

      dt_opencl_local_buffer_t locopt
        = (dt_opencl_local_buffer_t){ .xoffset = 2*2, .xfactor = 1, .yoffset = 2*2, .yfactor = 1,
                                      .cellsize = 1 * sizeof(float), .overhead = 0,
                                      .sizex = 1 << 8, .sizey = 1 << 8 };

      if(!dt_opencl_local_buffer_opt(devid, gd->kernel_pre_median, &locopt))
      goto error;

      size_t sizes[3] = { ROUNDUP(width, locopt.sizex), ROUNDUP(height, locopt.sizey), 1 };
      size_t local[3] = { locopt.sizex, locopt.sizey, 1 };
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 0, sizeof(cl_mem), &dev_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 1, sizeof(cl_mem), &dev_med);
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 2, sizeof(int), &width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 3, sizeof(int), &height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 4, sizeof(uint32_t),
                                (void *)&piece->dsc_in.filters);
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 5, sizeof(float), (void *)&data->median_thrs);
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 6,
                            sizeof(float) * (locopt.sizex + 4) * (locopt.sizey + 4), NULL);
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_pre_median, sizes, local);
      if(err != CL_SUCCESS) goto error;
      dev_in = dev_aux;
    }
    else
      dev_med = dev_in;

    {
      dt_opencl_local_buffer_t locopt
        = (dt_opencl_local_buffer_t){ .xoffset = 2*3, .xfactor = 1, .yoffset = 2*3, .yfactor = 1,
                                      .cellsize = sizeof(float) * 1, .overhead = 0,
                                      .sizex = 1 << 8, .sizey = 1 << 8 };

      if(!dt_opencl_local_buffer_opt(devid, gd->kernel_ppg_green, &locopt))
      goto error;

      size_t sizes[3] = { ROUNDUP(width, locopt.sizex), ROUNDUP(height, locopt.sizey), 1 };
      size_t local[3] = { locopt.sizex, locopt.sizey, 1 };
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green, 0, sizeof(cl_mem), &dev_med);
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green, 1, sizeof(cl_mem), &dev_tmp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green, 2, sizeof(int), &width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green, 3, sizeof(int), &height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green, 4, sizeof(uint32_t),
                                (void *)&piece->dsc_in.filters);
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green, 5,
                            sizeof(float) * (locopt.sizex + 2*3) * (locopt.sizey + 2*3), NULL);

      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_ppg_green, sizes, local);
      if(err != CL_SUCCESS) goto error;
    }

    {
      dt_opencl_local_buffer_t locopt
        = (dt_opencl_local_buffer_t){ .xoffset = 2*1, .xfactor = 1, .yoffset = 2*1, .yfactor = 1,
                                      .cellsize = 4 * sizeof(float), .overhead = 0,
                                      .sizex = 1 << 8, .sizey = 1 << 8 };

      if(!dt_opencl_local_buffer_opt(devid, gd->kernel_ppg_redblue, &locopt))
      goto error;

      size_t sizes[3] = { ROUNDUP(width, locopt.sizex), ROUNDUP(height, locopt.sizey), 1 };
      size_t local[3] = { locopt.sizex, locopt.sizey, 1 };
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_redblue, 0, sizeof(cl_mem), &dev_tmp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_redblue, 1, sizeof(cl_mem), &dev_aux);
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_redblue, 2, sizeof(int), &width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_redblue, 3, sizeof(int), &height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_redblue, 4, sizeof(uint32_t),
                                (void *)&piece->dsc_in.filters);
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_redblue, 5,
                            sizeof(float) * 4 * (locopt.sizex + 2) * (locopt.sizey + 2), NULL);

      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_ppg_redblue, sizes, local);
      if(err != CL_SUCCESS) goto error;
    }
  }

  if(dev_aux != dev_out) dt_opencl_release_mem_object(dev_aux);
  if(dev_med != dev_in) dt_opencl_release_mem_object(dev_med);
  dt_opencl_release_mem_object(dev_green_eq);
  dt_opencl_release_mem_object(dev_tmp);
  dev_aux = dev_green_eq = dev_tmp = dev_med = NULL;

  // color smoothing
  if(data->color_smoothing)
  {
    if(!color_smoothing_cl(self, pipe, piece, dev_out, dev_out, roi_out, data->color_smoothing))
      goto error;
  }

  return TRUE;

error:
  if(dev_aux != dev_out) dt_opencl_release_mem_object(dev_aux);
  if(dev_med != dev_in) dt_opencl_release_mem_object(dev_med);
  dt_opencl_release_mem_object(dev_green_eq);
  dt_opencl_release_mem_object(dev_tmp);
  dt_print(DT_DEBUG_OPENCL, "[opencl_demosaic] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}

static gboolean _downsample_guided_laplacian_postfilter_cl(struct dt_iop_module_t *self,
                                                           const dt_dev_pixelpipe_t *pipe,
                                                           cl_mem dev_out,
                                                           const dt_iop_roi_t *const roi_out,
                                                           const int iterations)
{
  if(iterations <= 0) return TRUE;

  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)self->global_data;
  const int devid = pipe->devid;
  const int width = roi_out->width;
  const int height = roi_out->height;
  const int clip_negatives = 1;
  const int keep_signed = 0;
  const int dense_mult = 1;
  size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };

  cl_mem LF_even = NULL;
  cl_mem LF_odd = NULL;
  cl_mem temp = NULL;
  cl_mem coeff = NULL;
  cl_mem bias = NULL;
  cl_mem coeff_tmp = NULL;
  cl_mem reconstructed_a = NULL;
  cl_mem reconstructed_b = NULL;
  cl_mem residual = NULL;
  cl_mem reconstructed_read = NULL;
  cl_mem reconstructed_write = NULL;
  cl_mem reconstructed_final = NULL;
  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  LF_even = dt_opencl_alloc_device(devid, width, height, sizeof(float) * 4);
  LF_odd = dt_opencl_alloc_device(devid, width, height, sizeof(float) * 4);
  temp = dt_opencl_alloc_device(devid, width, height, sizeof(float) * 4);
  coeff = dt_opencl_alloc_device(devid, width, height, sizeof(float) * 4);
  bias = dt_opencl_alloc_device(devid, width, height, sizeof(float) * 4);
  coeff_tmp = dt_opencl_alloc_device(devid, width, height, sizeof(float) * 4);
  reconstructed_a = dt_opencl_alloc_device(devid, width, height, sizeof(float) * 4);
  reconstructed_b = dt_opencl_alloc_device(devid, width, height, sizeof(float) * 4);
  if(IS_NULL_PTR(LF_even) || IS_NULL_PTR(LF_odd) || IS_NULL_PTR(temp) || IS_NULL_PTR(coeff) || IS_NULL_PTR(bias) || IS_NULL_PTR(coeff_tmp)
     || IS_NULL_PTR(reconstructed_a) || IS_NULL_PTR(reconstructed_b))
    goto error;

  for(int iteration = 0; iteration < iterations; ++iteration)
  {
    reconstructed_read = reconstructed_a;
    reconstructed_write = reconstructed_b;
    reconstructed_final = NULL;
    residual = dev_out;

    for(int s = 0; s < DOWNSAMPLE_GUIDED_SCALES; ++s)
    {
      const int mult = 1 << s;
      const int first_scale = (s == 0);
      cl_mem buffer_in;
      cl_mem buffer_out;

      if(s == 0)
      {
        buffer_in = dev_out;
        buffer_out = LF_odd;
      }
      else if(s % 2 != 0)
      {
        buffer_in = LF_odd;
        buffer_out = LF_even;
      }
      else
      {
        buffer_in = LF_even;
        buffer_out = LF_odd;
      }

      int hblocksize;
      dt_opencl_local_buffer_t hlocopt = (dt_opencl_local_buffer_t){ .xoffset = 2 * mult, .xfactor = 1,
                                                                      .yoffset = 0, .yfactor = 1,
                                                                      .cellsize = 4 * sizeof(float), .overhead = 0,
                                                                      .sizex = 1 << 16, .sizey = 1 };
      if(dt_opencl_local_buffer_opt(devid, gd->kernel_bspline_horizontal_local, &hlocopt))
        hblocksize = hlocopt.sizex;
      else
        hblocksize = 1;

      if(hblocksize > 1)
      {
        const size_t horizontal_sizes[3] = { ROUNDUP(width, hblocksize), ROUNDUPDHT(height, devid), 1 };
        const size_t horizontal_local[3] = { hblocksize, 1, 1 };
        dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_horizontal_local, 0, sizeof(cl_mem), &buffer_in);
        dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_horizontal_local, 1, sizeof(cl_mem), &temp);
        dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_horizontal_local, 2, sizeof(int), &width);
        dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_horizontal_local, 3, sizeof(int), &height);
        dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_horizontal_local, 4, sizeof(int), &mult);
        dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_horizontal_local, 5, sizeof(int), &clip_negatives);
        dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_horizontal_local, 6,
                                 (hblocksize + 4 * mult) * 4 * sizeof(float), NULL);
        err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_bspline_horizontal_local,
                                                     horizontal_sizes, horizontal_local);
      }
      else
      {
        dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_horizontal, 0, sizeof(cl_mem), &buffer_in);
        dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_horizontal, 1, sizeof(cl_mem), &temp);
        dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_horizontal, 2, sizeof(int), &width);
        dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_horizontal, 3, sizeof(int), &height);
        dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_horizontal, 4, sizeof(int), &mult);
        dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_horizontal, 5, sizeof(int), &clip_negatives);
        err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_bspline_horizontal, sizes);
      }
      if(err != CL_SUCCESS) goto error;

      int vblocksize;
      dt_opencl_local_buffer_t vlocopt = (dt_opencl_local_buffer_t){ .xoffset = 0, .xfactor = 1,
                                                                      .yoffset = 2 * mult, .yfactor = 1,
                                                                      .cellsize = 4 * sizeof(float), .overhead = 0,
                                                                      .sizex = 1, .sizey = 1 << 16 };
      if(dt_opencl_local_buffer_opt(devid, gd->kernel_bspline_vertical_local, &vlocopt))
        vblocksize = vlocopt.sizey;
      else
        vblocksize = 1;

      if(vblocksize > 1)
      {
        const size_t vertical_sizes[3] = { ROUNDUPDWD(width, devid), ROUNDUP(height, vblocksize), 1 };
        const size_t vertical_local[3] = { 1, vblocksize, 1 };
        dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_vertical_local, 0, sizeof(cl_mem), &temp);
        dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_vertical_local, 1, sizeof(cl_mem), &buffer_out);
        dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_vertical_local, 2, sizeof(int), &width);
        dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_vertical_local, 3, sizeof(int), &height);
        dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_vertical_local, 4, sizeof(int), &mult);
        dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_vertical_local, 5, sizeof(int), &clip_negatives);
        dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_vertical_local, 6,
                                 (vblocksize + 4 * mult) * 4 * sizeof(float), NULL);
        err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_bspline_vertical_local,
                                                     vertical_sizes, vertical_local);
      }
      else
      {
        dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_vertical, 0, sizeof(cl_mem), &temp);
        dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_vertical, 1, sizeof(cl_mem), &buffer_out);
        dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_vertical, 2, sizeof(int), &width);
        dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_vertical, 3, sizeof(int), &height);
        dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_vertical, 4, sizeof(int), &mult);
        dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_vertical, 5, sizeof(int), &clip_negatives);
        err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_bspline_vertical, sizes);
      }
      if(err != CL_SUCCESS) goto error;

      dt_opencl_set_kernel_arg(devid, gd->kernel_guided_laplacian_normalize, 0, sizeof(cl_mem), &buffer_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_guided_laplacian_normalize, 1, sizeof(cl_mem), &buffer_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_guided_laplacian_normalize, 2, sizeof(cl_mem), &temp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_guided_laplacian_normalize, 3, sizeof(int), &width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_guided_laplacian_normalize, 4, sizeof(int), &height);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_guided_laplacian_normalize, sizes);
      if(err != CL_SUCCESS) goto error;

      dt_opencl_set_kernel_arg(devid, gd->kernel_guided_laplacian_coefficients, 0, sizeof(cl_mem), &temp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_guided_laplacian_coefficients, 1, sizeof(cl_mem), &coeff);
      dt_opencl_set_kernel_arg(devid, gd->kernel_guided_laplacian_coefficients, 2, sizeof(cl_mem), &bias);
      dt_opencl_set_kernel_arg(devid, gd->kernel_guided_laplacian_coefficients, 3, sizeof(int), &width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_guided_laplacian_coefficients, 4, sizeof(int), &height);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_guided_laplacian_coefficients, sizes);
      if(err != CL_SUCCESS) goto error;

      dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_horizontal, 0, sizeof(cl_mem), &coeff);
      dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_horizontal, 1, sizeof(cl_mem), &coeff_tmp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_horizontal, 2, sizeof(int), &width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_horizontal, 3, sizeof(int), &height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_horizontal, 4, sizeof(int), &dense_mult);
      dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_horizontal, 5, sizeof(int), &keep_signed);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_bspline_horizontal, sizes);
      if(err != CL_SUCCESS) goto error;

      dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_vertical, 0, sizeof(cl_mem), &coeff_tmp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_vertical, 1, sizeof(cl_mem), &coeff);
      dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_vertical, 2, sizeof(int), &width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_vertical, 3, sizeof(int), &height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_vertical, 4, sizeof(int), &dense_mult);
      dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_vertical, 5, sizeof(int), &keep_signed);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_bspline_vertical, sizes);
      if(err != CL_SUCCESS) goto error;

      dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_horizontal, 0, sizeof(cl_mem), &bias);
      dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_horizontal, 1, sizeof(cl_mem), &coeff_tmp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_horizontal, 2, sizeof(int), &width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_horizontal, 3, sizeof(int), &height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_horizontal, 4, sizeof(int), &dense_mult);
      dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_horizontal, 5, sizeof(int), &keep_signed);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_bspline_horizontal, sizes);
      if(err != CL_SUCCESS) goto error;

      dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_vertical, 0, sizeof(cl_mem), &coeff_tmp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_vertical, 1, sizeof(cl_mem), &bias);
      dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_vertical, 2, sizeof(int), &width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_vertical, 3, sizeof(int), &height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_vertical, 4, sizeof(int), &dense_mult);
      dt_opencl_set_kernel_arg(devid, gd->kernel_bspline_vertical, 5, sizeof(int), &keep_signed);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_bspline_vertical, sizes);
      if(err != CL_SUCCESS) goto error;

      dt_opencl_set_kernel_arg(devid, gd->kernel_guided_laplacian_apply, 0, sizeof(cl_mem), &temp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_guided_laplacian_apply, 1, sizeof(cl_mem), &coeff);
      dt_opencl_set_kernel_arg(devid, gd->kernel_guided_laplacian_apply, 2, sizeof(cl_mem), &bias);
      dt_opencl_set_kernel_arg(devid, gd->kernel_guided_laplacian_apply, 3, sizeof(cl_mem), &buffer_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_guided_laplacian_apply, 4, sizeof(cl_mem), &reconstructed_read);
      dt_opencl_set_kernel_arg(devid, gd->kernel_guided_laplacian_apply, 5, sizeof(cl_mem), &reconstructed_write);
      dt_opencl_set_kernel_arg(devid, gd->kernel_guided_laplacian_apply, 6, sizeof(int), &width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_guided_laplacian_apply, 7, sizeof(int), &height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_guided_laplacian_apply, 8, sizeof(int), &first_scale);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_guided_laplacian_apply, sizes);
      if(err != CL_SUCCESS) goto error;

      residual = buffer_out;
      reconstructed_final = reconstructed_write;
      cl_mem tmp = reconstructed_read;
      reconstructed_read = reconstructed_write;
      reconstructed_write = tmp;
    }

    dt_opencl_set_kernel_arg(devid, gd->kernel_guided_laplacian_finalize, 0, sizeof(cl_mem), &reconstructed_final);
    dt_opencl_set_kernel_arg(devid, gd->kernel_guided_laplacian_finalize, 1, sizeof(cl_mem), &residual);
    dt_opencl_set_kernel_arg(devid, gd->kernel_guided_laplacian_finalize, 2, sizeof(cl_mem), &dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_guided_laplacian_finalize, 3, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_guided_laplacian_finalize, 4, sizeof(int), &height);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_guided_laplacian_finalize, sizes);
    if(err != CL_SUCCESS) goto error;
  }

  dt_opencl_release_mem_object(reconstructed_b);
  dt_opencl_release_mem_object(reconstructed_a);
  dt_opencl_release_mem_object(coeff_tmp);
  dt_opencl_release_mem_object(bias);
  dt_opencl_release_mem_object(coeff);
  dt_opencl_release_mem_object(temp);
  dt_opencl_release_mem_object(LF_odd);
  dt_opencl_release_mem_object(LF_even);
  return TRUE;

error:
  dt_opencl_release_mem_object(reconstructed_b);
  dt_opencl_release_mem_object(reconstructed_a);
  dt_opencl_release_mem_object(coeff_tmp);
  dt_opencl_release_mem_object(bias);
  dt_opencl_release_mem_object(coeff);
  dt_opencl_release_mem_object(temp);
  dt_opencl_release_mem_object(LF_odd);
  dt_opencl_release_mem_object(LF_even);
  dt_print(DT_DEBUG_OPENCL, "[opencl_demosaic] guided laplacian postfilter failed: %d\n", err);
  return FALSE;
}

int process_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in, cl_mem dev_out)
{
  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  dt_times_t start_time = { 0 }, end_time = { 0 };
  const gboolean info = ((darktable.unmuted & (DT_DEBUG_DEMOSAIC | DT_DEBUG_PERF))
                         && (pipe->type == DT_DEV_PIXELPIPE_FULL));

  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)self->global_data;

  int demosaicing_method = data->demosaicing_method;

  gboolean showmask = FALSE;
  if(self->dev->gui_attached && pipe->type == DT_DEV_PIXELPIPE_FULL)
  {
    dt_iop_demosaic_gui_data_t *g = (dt_iop_demosaic_gui_data_t *)self->gui_data;
    if(g) showmask = (g->visual_mask);
    // take care of passthru modes
    if(pipe->mask_display == DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU)
      demosaicing_method = (piece->dsc_in.filters != 9u) ? DT_IOP_DEMOSAIC_RCD : DT_IOP_DEMOSAIC_MARKESTEIJN;
    else if(pipe->mask_display == DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU_MONO)
      demosaicing_method = DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME;
  }

  cl_mem high_image = NULL;
  cl_mem low_image = NULL;
  cl_mem blend = NULL;
  cl_mem details = NULL;
  cl_mem dev_aux = NULL;
  cl_mem dev_xtrans = NULL;
  const gboolean dual = ((demosaicing_method & DEMOSAIC_DUAL) && (data->dual_thrs > 0.0f));
  const int devid = pipe->devid;
  gboolean retval = FALSE;

  if(info) dt_get_times(&start_time);

  if(demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME ||
     demosaicing_method == DT_IOP_DEMOSAIC_PPG ||
     demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR )
  {
    if(!process_default_cl(self, pipe, piece, dev_in, dev_out, roi_in, roi_out, demosaicing_method)) return FALSE;
  }
  else if(_is_downsample_method(demosaicing_method))
  {
    const int width = roi_out->width;
    const int height = roi_out->height;
    size_t sizes[3] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };

    if(piece->dsc_in.filters == 9u)
    {
      dev_xtrans = dt_opencl_copy_host_to_device_constant(devid, sizeof(piece->dsc_in.xtrans),
                                                          (void *)piece->dsc_in.xtrans);
      if(IS_NULL_PTR(dev_xtrans)) goto finish;

      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size_xtrans, 0, sizeof(cl_mem), &dev_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size_xtrans, 1, sizeof(cl_mem), &dev_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size_xtrans, 2, sizeof(int), &width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size_xtrans, 3, sizeof(int), &height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size_xtrans, 4, sizeof(int), (void *)&roi_in->x);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size_xtrans, 5, sizeof(int), (void *)&roi_in->y);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size_xtrans, 6, sizeof(int), (void *)&roi_in->width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size_xtrans, 7, sizeof(int), (void *)&roi_in->height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size_xtrans, 8, sizeof(cl_mem), (void *)&dev_xtrans);
      const cl_int err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_zoom_half_size_xtrans, sizes);
      if(err != CL_SUCCESS) goto finish;
    }
    else
    {
      const int zero = 0;
      const int is_4bayer = self->dev->image_storage.flags & DT_IMAGE_4BAYER;
      const float cam_to_rgb_0[4] = { data->CAM_to_RGB[0][0], data->CAM_to_RGB[0][1],
                                      data->CAM_to_RGB[0][2], data->CAM_to_RGB[0][3] };
      const float cam_to_rgb_1[4] = { data->CAM_to_RGB[1][0], data->CAM_to_RGB[1][1],
                                      data->CAM_to_RGB[1][2], data->CAM_to_RGB[1][3] };
      const float cam_to_rgb_2[4] = { data->CAM_to_RGB[2][0], data->CAM_to_RGB[2][1],
                                      data->CAM_to_RGB[2][2], data->CAM_to_RGB[2][3] };

      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 0, sizeof(cl_mem), &dev_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 1, sizeof(cl_mem), &dev_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 2, sizeof(int), &width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 3, sizeof(int), &height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 4, sizeof(int), &zero);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 5, sizeof(int), &zero);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 6, sizeof(int), (void *)&roi_in->width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 7, sizeof(int), (void *)&roi_in->height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 8, sizeof(float), (void *)&roi_out->scale);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 9, sizeof(uint32_t), (void *)&piece->dsc_in.filters);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 10, sizeof(int), &is_4bayer);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 11, sizeof(cam_to_rgb_0), cam_to_rgb_0);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 12, sizeof(cam_to_rgb_1), cam_to_rgb_1);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 13, sizeof(cam_to_rgb_2), cam_to_rgb_2);
      const cl_int err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_zoom_half_size, sizes);
      if(err != CL_SUCCESS) goto finish;
    }

    if(!_downsample_guided_laplacian_postfilter_cl(self, pipe, dev_out, roi_out, data->color_smoothing)) goto finish;

    retval = TRUE;
    goto finish;
  }
  else if((demosaicing_method & ~DEMOSAIC_DUAL) == DT_IOP_DEMOSAIC_RCD)
  {
    if(dual)
    {
      high_image = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float) * 4);
      if(IS_NULL_PTR(high_image)) return FALSE;
      if(!process_rcd_cl(self, pipe, piece, dev_in, high_image, roi_in, roi_in, FALSE)) goto finish;
    }
    else
    {
     if(!process_rcd_cl(self, pipe, piece, dev_in, dev_out, roi_in, roi_out, TRUE)) return FALSE;
    }
  }
  else if(demosaicing_method ==  DT_IOP_DEMOSAIC_VNG4 || demosaicing_method == DT_IOP_DEMOSAIC_VNG)
  {
    if(!process_vng_cl(self, pipe, piece, dev_in, dev_out, roi_in, roi_out, TRUE, FALSE)) return FALSE;
  }
  else if(((demosaicing_method & ~DEMOSAIC_DUAL) == DT_IOP_DEMOSAIC_MARKESTEIJN ) ||
          ((demosaicing_method & ~DEMOSAIC_DUAL) == DT_IOP_DEMOSAIC_MARKESTEIJN_3))
  {
    if(dual)
    {
      high_image = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float) * 4);
      if(IS_NULL_PTR(high_image)) return FALSE;
      if(!process_markesteijn_cl(self, pipe, piece, dev_in, high_image, roi_in, roi_in, FALSE)) return FALSE;
    }
    else
    {
      if(!process_markesteijn_cl(self, pipe, piece, dev_in, dev_out, roi_in, roi_out, TRUE)) return FALSE;
    }
  }
  else
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_demosaic] demosaicing method '%s' not yet supported by opencl code\n", method2string(demosaicing_method));
    return FALSE;
  }

  if(info)
  {
    const float mpixels = (roi_in->width * roi_in->height) / 1.0e6;
    dt_get_times(&end_time);
    const float tclock = end_time.clock - start_time.clock;
    const float uclock = end_time.user - start_time.user;
    fprintf(stderr," [demosaic] process GPU `%s' did %.2fmpix, %.4f secs (%.4f CPU), %.2f pix/us\n",
      method2string(demosaicing_method & ~DEMOSAIC_DUAL), mpixels, tclock, uclock, mpixels / tclock);
  }
  if(!dual)
  {
    retval = TRUE;
    goto finish;
  }

  // This is dual demosaicing only stuff
  const int scaled = (roi_out->width != roi_in->width || roi_out->height != roi_in->height);

  int width = roi_out->width;
  int height = roi_out->height;
  // need to reserve scaled auxiliary buffer or use dev_out
  if(scaled)
  {
    dev_aux = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float) * 4);
    if(IS_NULL_PTR(dev_aux)) goto finish;
    width = roi_in->width;
    height = roi_in->height;
  }
  else
    dev_aux = dev_out;

  // here we have work to be done only for dual demosaicers
  blend = dt_opencl_alloc_device_buffer(devid, sizeof(float) * width * height);
  details = dt_opencl_alloc_device_buffer(devid, sizeof(float) * width * height);
  low_image = dt_opencl_alloc_device(devid, width, height, sizeof(float) * 4);
  if((IS_NULL_PTR(blend)) || (IS_NULL_PTR(low_image)) || (IS_NULL_PTR(details))) goto finish;

  if(info) dt_get_times(&start_time);
  if(process_vng_cl(self, pipe, piece, dev_in, low_image, roi_in, roi_in, FALSE, FALSE))
  {
    if(!color_smoothing_cl(self, pipe, piece, low_image, low_image, roi_in, 2))
    {
      retval = FALSE;
      goto finish;
    }
    retval = dual_demosaic_cl(self, pipe, piece, details, blend, high_image, low_image, dev_aux, width, height, showmask);
  }

  if(info)
  {
    dt_get_times(&end_time);
    fprintf(stderr," [demosaic] GPU dual blending %.4f secs (%.4f CPU)\n", end_time.clock - start_time.clock, end_time.user - start_time.user);
  }

  if(scaled)
  {
    // scale aux buffer to output buffer
    const int err = dt_iop_clip_and_zoom_roi_cl(devid, dev_out, dev_aux, roi_out, roi_in);
    if(err != CL_SUCCESS)
      retval = FALSE;
  }

  finish:
  dt_opencl_release_mem_object(high_image);
  dt_opencl_release_mem_object(low_image);
  dt_opencl_release_mem_object(details);
  dt_opencl_release_mem_object(blend);
  if(dev_aux != dev_out) dt_opencl_release_mem_object(dev_aux);
  dt_opencl_release_mem_object(dev_xtrans);
  if(!retval && dual) dt_control_log(_("[dual demosaic_cl] internal problem"));
  return retval;
}
#endif

void tiling_callback(struct dt_iop_module_t *self, const struct dt_dev_pixelpipe_t *pipe, const struct dt_dev_pixelpipe_iop_t *piece, struct dt_develop_tiling_t *tiling)
{
  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;

  const float ioratio = (float)roi_out->width * roi_out->height / ((float)roi_in->width * roi_in->height);
  const float smooth = data->color_smoothing ? ioratio : 0.0f;
  const float greeneq
      = ((piece->dsc_in.filters != 9u) && (data->green_eq != DT_IOP_GREEN_EQ_NO)) ? 0.25f : 0.0f;
  const dt_iop_demosaic_method_t demosaicing_method = data->demosaicing_method & ~DEMOSAIC_DUAL;

  if(demosaicing_method == DT_IOP_DEMOSAIC_DOWNSAMPLE)
  {
    tiling->factor = 1.0f + ioratio + (data->color_smoothing ? 7.0f * ioratio : 0.0f);
    tiling->maxbuf = 1.0f;
    tiling->overhead = 0;
    tiling->xalign = 1;
    tiling->yalign = 1;
    tiling->overlap = (piece->dsc_in.filters == 9u) ? 18 : 16;
  }
  else if((demosaicing_method == DT_IOP_DEMOSAIC_PPG) ||
      (demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME) ||
      (demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR) ||
      (demosaicing_method == DT_IOP_DEMOSAIC_AMAZE))
  {
    // Bayer pattern with PPG, Passthrough or Amaze
    tiling->factor = 1.0f + ioratio;         // in + out
    tiling->factor += fmax(1.0f + greeneq, smooth);  // + tmp + geeneq | + smooth
    tiling->maxbuf = 1.0f;
    tiling->overhead = 0;
    tiling->xalign = 2;
    tiling->yalign = 2;
    tiling->overlap = 5; // take care of border handling
  }
  else if(((demosaicing_method == DT_IOP_DEMOSAIC_MARKESTEIJN) ||
           (demosaicing_method == DT_IOP_DEMOSAIC_MARKESTEIJN_3) ||
           (demosaicing_method == DT_IOP_DEMOSAIC_FDC)))
  {
    // X-Trans pattern full Markesteijn processing
    const int ndir = (demosaicing_method == DT_IOP_DEMOSAIC_MARKESTEIJN_3) ? 8 : 4;
    const int overlap = (demosaicing_method == DT_IOP_DEMOSAIC_MARKESTEIJN_3) ? 18 : 12;

    tiling->factor = 1.0f + ioratio;
    tiling->factor += ndir * 1.0f      // rgb
                      + ndir * 0.25f   // drv
                      + ndir * 0.125f  // homo + homosum
                      + 1.0f;          // aux

    tiling->factor += fmax(1.0f + greeneq, smooth);
    tiling->maxbuf = 1.0f;
    tiling->overhead = 0;
    tiling->xalign = XTRANS_SNAPPER;
    tiling->yalign = XTRANS_SNAPPER;
    tiling->overlap = overlap;
  }
  else if(demosaicing_method == DT_IOP_DEMOSAIC_RCD)
  {
    tiling->factor = 1.0f + ioratio;
    tiling->factor += fmax(1.0f + greeneq, smooth);  // + tmp + geeneq | + smooth
    tiling->maxbuf = 1.0f;
    tiling->overhead = sizeof(float) * RCD_TILESIZE * RCD_TILESIZE * 8 * MAX(1, darktable.num_openmp_threads);
    tiling->xalign = 2;
    tiling->yalign = 2;
    tiling->overlap = 10;
    tiling->factor_cl = tiling->factor + 3.0f;
  }
  else if(demosaicing_method == DT_IOP_DEMOSAIC_LMMSE)
  {
    tiling->factor = 1.0f + ioratio;
    tiling->factor += fmax(1.0f + greeneq, smooth);  // + tmp + geeneq | + smooth
    tiling->maxbuf = 1.0f;
    tiling->overhead = sizeof(float) * LMMSE_GRP * LMMSE_GRP * 6 * MAX(1, darktable.num_openmp_threads);
    tiling->xalign = 2;
    tiling->yalign = 2;
    tiling->overlap = 10;
  }
  else
  {
    // VNG
    tiling->factor = 1.0f + ioratio;
    tiling->factor += fmax(1.0f + greeneq, smooth);
    tiling->maxbuf = 1.0f;
    tiling->overhead = 0;
    tiling->xalign = 6; // covering Bayer pattern for VNG4 as well as xtrans for VNG
    tiling->yalign = 6; // covering Bayer pattern for VNG4 as well as xtrans for VNG
    tiling->overlap = 6;
  }
  if(data->demosaicing_method & DEMOSAIC_DUAL)
  {
    // make sure VNG4 is also possible
    tiling->factor += 1.0f;
    tiling->xalign = MAX(6, tiling->xalign);
    tiling->yalign = MAX(6, tiling->yalign);
    tiling->overlap = MAX(6, tiling->overlap);
  }
  return;
}
#undef LMMSE_GRP

void init_global(dt_iop_module_so_t *module)
{
  const int program = 0; // from programs.conf
  dt_iop_demosaic_global_data_t *gd
      = (dt_iop_demosaic_global_data_t *)malloc(sizeof(dt_iop_demosaic_global_data_t));
  module->data = gd;
  gd->kernel_zoom_half_size = dt_opencl_create_kernel(program, "clip_and_zoom_demosaic_half_size");
  gd->kernel_ppg_green = dt_opencl_create_kernel(program, "ppg_demosaic_green");
  gd->kernel_green_eq_lavg = dt_opencl_create_kernel(program, "green_equilibration_lavg");
  gd->kernel_green_eq_favg_reduce_first = dt_opencl_create_kernel(program, "green_equilibration_favg_reduce_first");
  gd->kernel_green_eq_favg_reduce_second = dt_opencl_create_kernel(program, "green_equilibration_favg_reduce_second");
  gd->kernel_green_eq_favg_apply = dt_opencl_create_kernel(program, "green_equilibration_favg_apply");
  gd->kernel_pre_median = dt_opencl_create_kernel(program, "pre_median");
  gd->kernel_ppg_redblue = dt_opencl_create_kernel(program, "ppg_demosaic_redblue");
  gd->kernel_downsample = dt_opencl_create_kernel(program, "clip_and_zoom");
  gd->kernel_border_interpolate = dt_opencl_create_kernel(program, "border_interpolate");
  gd->kernel_color_smoothing = dt_opencl_create_kernel(program, "color_smoothing");

  const int other = 14; // from programs.conf
  gd->kernel_passthrough_monochrome = dt_opencl_create_kernel(other, "passthrough_monochrome");
  gd->kernel_passthrough_color = dt_opencl_create_kernel(other, "passthrough_color");
  gd->kernel_zoom_passthrough_monochrome = dt_opencl_create_kernel(other, "clip_and_zoom_demosaic_passthrough_monochrome");

  const int vng = 15; // from programs.conf
  gd->kernel_vng_border_interpolate = dt_opencl_create_kernel(vng, "vng_border_interpolate");
  gd->kernel_vng_lin_interpolate = dt_opencl_create_kernel(vng, "vng_lin_interpolate");
  gd->kernel_zoom_third_size = dt_opencl_create_kernel(vng, "clip_and_zoom_demosaic_third_size_xtrans");
  gd->kernel_zoom_half_size_xtrans = dt_opencl_create_kernel(vng, "clip_and_zoom_demosaic_half_size_xtrans");
  gd->kernel_vng_green_equilibrate = dt_opencl_create_kernel(vng, "vng_green_equilibrate");
  gd->kernel_vng_interpolate = dt_opencl_create_kernel(vng, "vng_interpolate");

  const int markesteijn = 16; // from programs.conf
  gd->kernel_markesteijn_initial_copy = dt_opencl_create_kernel(markesteijn, "markesteijn_initial_copy");
  gd->kernel_markesteijn_green_minmax = dt_opencl_create_kernel(markesteijn, "markesteijn_green_minmax");
  gd->kernel_markesteijn_interpolate_green = dt_opencl_create_kernel(markesteijn, "markesteijn_interpolate_green");
  gd->kernel_markesteijn_solitary_green = dt_opencl_create_kernel(markesteijn, "markesteijn_solitary_green");
  gd->kernel_markesteijn_recalculate_green = dt_opencl_create_kernel(markesteijn, "markesteijn_recalculate_green");
  gd->kernel_markesteijn_red_and_blue = dt_opencl_create_kernel(markesteijn, "markesteijn_red_and_blue");
  gd->kernel_markesteijn_interpolate_twoxtwo = dt_opencl_create_kernel(markesteijn, "markesteijn_interpolate_twoxtwo");
  gd->kernel_markesteijn_convert_yuv = dt_opencl_create_kernel(markesteijn, "markesteijn_convert_yuv");
  gd->kernel_markesteijn_differentiate = dt_opencl_create_kernel(markesteijn, "markesteijn_differentiate");
  gd->kernel_markesteijn_homo_threshold = dt_opencl_create_kernel(markesteijn, "markesteijn_homo_threshold");
  gd->kernel_markesteijn_homo_set = dt_opencl_create_kernel(markesteijn, "markesteijn_homo_set");
  gd->kernel_markesteijn_homo_sum = dt_opencl_create_kernel(markesteijn, "markesteijn_homo_sum");
  gd->kernel_markesteijn_homo_max = dt_opencl_create_kernel(markesteijn, "markesteijn_homo_max");
  gd->kernel_markesteijn_homo_max_corr = dt_opencl_create_kernel(markesteijn, "markesteijn_homo_max_corr");
  gd->kernel_markesteijn_homo_quench = dt_opencl_create_kernel(markesteijn, "markesteijn_homo_quench");
  gd->kernel_markesteijn_zero = dt_opencl_create_kernel(markesteijn, "markesteijn_zero");
  gd->kernel_markesteijn_accu = dt_opencl_create_kernel(markesteijn, "markesteijn_accu");
  gd->kernel_markesteijn_final = dt_opencl_create_kernel(markesteijn, "markesteijn_final");

  const int rcd = 31; // from programs.conf
  gd->kernel_rcd_populate = dt_opencl_create_kernel(rcd, "rcd_populate");
  gd->kernel_rcd_write_output = dt_opencl_create_kernel(rcd, "rcd_write_output");
  gd->kernel_rcd_step_1 = dt_opencl_create_kernel(rcd, "rcd_step_1");
  gd->kernel_rcd_step_2_1 = dt_opencl_create_kernel(rcd, "rcd_step_2_1");
  gd->kernel_rcd_step_3_1 = dt_opencl_create_kernel(rcd, "rcd_step_3_1");
  gd->kernel_rcd_step_4_1 = dt_opencl_create_kernel(rcd, "rcd_step_4_1");
  gd->kernel_rcd_step_4_2 = dt_opencl_create_kernel(rcd, "rcd_step_4_2");
  gd->kernel_rcd_step_5_1 = dt_opencl_create_kernel(rcd, "rcd_step_5_1");
  gd->kernel_rcd_step_5_2 = dt_opencl_create_kernel(rcd, "rcd_step_5_2");
  gd->kernel_rcd_border_redblue = dt_opencl_create_kernel(rcd, "rcd_border_redblue");
  gd->kernel_rcd_border_green = dt_opencl_create_kernel(rcd, "rcd_border_green");
  gd->kernel_write_blended_dual  = dt_opencl_create_kernel(rcd, "write_blended_dual");

  const int wavelets = 35; // bspline.cl, from programs.conf
  gd->kernel_guided_laplacian_coefficients = dt_opencl_create_kernel(wavelets, "guided_laplacian_coefficients");
  gd->kernel_guided_laplacian_normalize = dt_opencl_create_kernel(wavelets, "guided_laplacian_normalize");
  gd->kernel_guided_laplacian_apply = dt_opencl_create_kernel(wavelets, "guided_laplacian_apply");
  gd->kernel_guided_laplacian_finalize = dt_opencl_create_kernel(wavelets, "guided_laplacian_finalize");
  gd->kernel_bspline_horizontal = dt_opencl_create_kernel(wavelets, "blur_2D_Bspline_horizontal");
  gd->kernel_bspline_vertical = dt_opencl_create_kernel(wavelets, "blur_2D_Bspline_vertical");
  gd->kernel_bspline_horizontal_local = dt_opencl_create_kernel(wavelets, "blur_2D_Bspline_horizontal_local");
  gd->kernel_bspline_vertical_local = dt_opencl_create_kernel(wavelets, "blur_2D_Bspline_vertical_local");
  gd->lmmse_gamma_in = NULL;
  gd->lmmse_gamma_out = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_zoom_half_size);
  dt_opencl_free_kernel(gd->kernel_ppg_green);
  dt_opencl_free_kernel(gd->kernel_pre_median);
  dt_opencl_free_kernel(gd->kernel_green_eq_lavg);
  dt_opencl_free_kernel(gd->kernel_green_eq_favg_reduce_first);
  dt_opencl_free_kernel(gd->kernel_green_eq_favg_reduce_second);
  dt_opencl_free_kernel(gd->kernel_green_eq_favg_apply);
  dt_opencl_free_kernel(gd->kernel_ppg_redblue);
  dt_opencl_free_kernel(gd->kernel_downsample);
  dt_opencl_free_kernel(gd->kernel_border_interpolate);
  dt_opencl_free_kernel(gd->kernel_color_smoothing);
  dt_opencl_free_kernel(gd->kernel_passthrough_monochrome);
  dt_opencl_free_kernel(gd->kernel_passthrough_color);
  dt_opencl_free_kernel(gd->kernel_zoom_passthrough_monochrome);
  dt_opencl_free_kernel(gd->kernel_vng_border_interpolate);
  dt_opencl_free_kernel(gd->kernel_vng_lin_interpolate);
  dt_opencl_free_kernel(gd->kernel_zoom_third_size);
  dt_opencl_free_kernel(gd->kernel_zoom_half_size_xtrans);
  dt_opencl_free_kernel(gd->kernel_guided_laplacian_coefficients);
  dt_opencl_free_kernel(gd->kernel_guided_laplacian_normalize);
  dt_opencl_free_kernel(gd->kernel_guided_laplacian_apply);
  dt_opencl_free_kernel(gd->kernel_guided_laplacian_finalize);
  dt_opencl_free_kernel(gd->kernel_bspline_horizontal);
  dt_opencl_free_kernel(gd->kernel_bspline_vertical);
  dt_opencl_free_kernel(gd->kernel_bspline_horizontal_local);
  dt_opencl_free_kernel(gd->kernel_bspline_vertical_local);
  dt_opencl_free_kernel(gd->kernel_vng_green_equilibrate);
  dt_opencl_free_kernel(gd->kernel_vng_interpolate);
  dt_opencl_free_kernel(gd->kernel_markesteijn_initial_copy);
  dt_opencl_free_kernel(gd->kernel_markesteijn_green_minmax);
  dt_opencl_free_kernel(gd->kernel_markesteijn_interpolate_green);
  dt_opencl_free_kernel(gd->kernel_markesteijn_solitary_green);
  dt_opencl_free_kernel(gd->kernel_markesteijn_recalculate_green);
  dt_opencl_free_kernel(gd->kernel_markesteijn_red_and_blue);
  dt_opencl_free_kernel(gd->kernel_markesteijn_interpolate_twoxtwo);
  dt_opencl_free_kernel(gd->kernel_markesteijn_convert_yuv);
  dt_opencl_free_kernel(gd->kernel_markesteijn_differentiate);
  dt_opencl_free_kernel(gd->kernel_markesteijn_homo_threshold);
  dt_opencl_free_kernel(gd->kernel_markesteijn_homo_set);
  dt_opencl_free_kernel(gd->kernel_markesteijn_homo_sum);
  dt_opencl_free_kernel(gd->kernel_markesteijn_homo_max);
  dt_opencl_free_kernel(gd->kernel_markesteijn_homo_max_corr);
  dt_opencl_free_kernel(gd->kernel_markesteijn_homo_quench);
  dt_opencl_free_kernel(gd->kernel_markesteijn_zero);
  dt_opencl_free_kernel(gd->kernel_markesteijn_accu);
  dt_opencl_free_kernel(gd->kernel_markesteijn_final);
  dt_opencl_free_kernel(gd->kernel_rcd_populate);
  dt_opencl_free_kernel(gd->kernel_rcd_write_output);
  dt_opencl_free_kernel(gd->kernel_rcd_step_1);
  dt_opencl_free_kernel(gd->kernel_rcd_step_2_1);
  dt_opencl_free_kernel(gd->kernel_rcd_step_3_1);
  dt_opencl_free_kernel(gd->kernel_rcd_step_4_1);
  dt_opencl_free_kernel(gd->kernel_rcd_step_4_2);
  dt_opencl_free_kernel(gd->kernel_rcd_step_5_1);
  dt_opencl_free_kernel(gd->kernel_rcd_step_5_2);
  dt_opencl_free_kernel(gd->kernel_rcd_border_redblue);
  dt_opencl_free_kernel(gd->kernel_rcd_border_green);
  dt_opencl_free_kernel(gd->kernel_write_blended_dual);
  dt_pixelpipe_cache_free_align(gd->lmmse_gamma_in);
  dt_pixelpipe_cache_free_align(gd->lmmse_gamma_out);
  dt_free(module->data);
}


gboolean force_enable(struct dt_iop_module_t *self, const gboolean current_state)
{
  // This needs to be enabled for raw images, disabled for other images.
  // There is no messing around.
  return dt_image_is_raw(&self->dev->image_storage);
}


void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_demosaic_params_t *p = (dt_iop_demosaic_params_t *)params;
  dt_iop_demosaic_data_t *d = (dt_iop_demosaic_data_t *)piece->data;

  d->green_eq = p->green_eq;
  d->color_smoothing = p->color_smoothing;
  d->median_thrs = p->median_thrs;
  d->dual_thrs = p->dual_thrs;
  d->lmmse_refine = p->lmmse_refine;
  dt_iop_demosaic_method_t use_method = p->demosaicing_method;
  const gboolean xmethod = use_method & DEMOSAIC_XTRANS;
  const gboolean bayer   = (self->dev->image_storage.dsc.filters != 9u);
  const gboolean downsample = _is_downsample_method(use_method);

  if(!downsample)
  {
    if(bayer && xmethod)   use_method = DT_IOP_DEMOSAIC_RCD;
    if(!bayer && !xmethod) use_method = DT_IOP_DEMOSAIC_MARKESTEIJN;
  }

  if(use_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME || use_method == DT_IOP_DEMOSAIC_PASSTHR_MONOX)
    use_method = DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME;
  if(use_method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR || use_method == DT_IOP_DEMOSAIC_PASSTHR_COLORX)
    use_method = DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR;

  const gboolean passing = (use_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME
                            || use_method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR);

  if(!(use_method == DT_IOP_DEMOSAIC_PPG))
    d->median_thrs = 0.0f;

  if(passing)
  {
    d->green_eq = DT_IOP_GREEN_EQ_NO;
    d->color_smoothing = 0;
    d->dual_thrs = 0.0f;
  }
  else if(downsample)
  {
    d->green_eq = DT_IOP_GREEN_EQ_NO;
    d->dual_thrs = 0.0f;
  }

  if(use_method & DEMOSAIC_DUAL)
    d->color_smoothing = 0;

  d->demosaicing_method = use_method;

  // OpenCL only supported by some of the demosaicing methods
  switch(d->demosaicing_method)
  {
    case DT_IOP_DEMOSAIC_PPG:
      piece->process_cl_ready = 1;
      break;
    case DT_IOP_DEMOSAIC_AMAZE:
      piece->process_cl_ready = 0;
      break;
    case DT_IOP_DEMOSAIC_VNG4:
      piece->process_cl_ready = 1;
      break;
    case DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME:
      piece->process_cl_ready = 1;
      break;
    case DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR:
      piece->process_cl_ready = 1;
      break;
    case DT_IOP_DEMOSAIC_RCD:
      piece->process_cl_ready = 1;
      break;
    case DT_IOP_DEMOSAIC_LMMSE:
      piece->process_cl_ready = 0;
      break;
    case DT_IOP_DEMOSAIC_RCD_VNG:
      piece->process_cl_ready = 1;
      break;
    case DT_IOP_DEMOSAIC_AMAZE_VNG:
      piece->process_cl_ready = 0;
      break;
    case DT_IOP_DEMOSAIC_MARKEST3_VNG:
      piece->process_cl_ready = 1;
      break;
    case DT_IOP_DEMOSAIC_VNG:
      piece->process_cl_ready = 1;
      break;
    case DT_IOP_DEMOSAIC_MARKESTEIJN:
      piece->process_cl_ready = 1;
      break;
    case DT_IOP_DEMOSAIC_MARKESTEIJN_3:
      piece->process_cl_ready = 1;
      break;
    case DT_IOP_DEMOSAIC_FDC:
      piece->process_cl_ready = 0;
      break;
    case DT_IOP_DEMOSAIC_DOWNSAMPLE:
      piece->process_cl_ready = 1;
      break;
    default:
      piece->process_cl_ready = 0;
  }

  // green-equilibrate over full image excludes tiling
  if((d->green_eq == DT_IOP_GREEN_EQ_FULL
      || d->green_eq == DT_IOP_GREEN_EQ_BOTH)
     || ((use_method & DEMOSAIC_DUAL) && (d->dual_thrs > 0.0f)))
  {
    piece->process_tiling_ready = 0;
  }

  if(self->dev->image_storage.flags & DT_IMAGE_4BAYER)
  {
    // 4Bayer images only support the dedicated half-size downsample path in OpenCL.
    if(d->demosaicing_method != DT_IOP_DEMOSAIC_DOWNSAMPLE) piece->process_cl_ready = 0;

    // Get and store the matrix to go from camera to RGB for 4Bayer images
    if(!dt_colorspaces_conversion_matrices_rgb(self->dev->image_storage.adobe_XYZ_to_CAM,
                                               NULL, d->CAM_to_RGB,
                                               self->dev->image_storage.d65_color_matrix, NULL))
    {
      const char *camera = self->dev->image_storage.camera_makermodel;
      fprintf(stderr, "[colorspaces] `%s' color matrix not found for 4bayer image!\n", camera);
      dt_control_log(_("`%s' color matrix not found for 4bayer image!"), camera);
    }
  }
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc_align(sizeof(dt_iop_demosaic_data_t));
  piece->data_size = sizeof(dt_iop_demosaic_data_t);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_free_align(piece->data);
  piece->data = NULL;
}

void reload_defaults(dt_iop_module_t *module)
{
  dt_iop_demosaic_params_t *d = (dt_iop_demosaic_params_t *)module->default_params;

  if(dt_image_is_monochrome(&module->dev->image_storage))
    d->demosaicing_method = DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME;
  else if(module->dev->image_storage.dsc.filters == 9u)
    d->demosaicing_method = DT_IOP_DEMOSAIC_MARKESTEIJN;
  else
    d->demosaicing_method = DT_IOP_DEMOSAIC_RCD;

  module->hide_enable_button = 1;

  module->default_enabled = dt_image_is_raw(&module->dev->image_storage);
  if(module->widget)
    gtk_stack_set_visible_child_name(GTK_STACK(module->widget), module->default_enabled ? "raw" : "non_raw");
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_demosaic_gui_data_t *g = (dt_iop_demosaic_gui_data_t *)self->gui_data;
  dt_iop_demosaic_params_t *p = (dt_iop_demosaic_params_t *)self->params;

  const gboolean bayer = (self->dev->image_storage.dsc.filters != 9u);
  dt_iop_demosaic_method_t use_method = p->demosaicing_method;
  const gboolean xmethod = use_method & DEMOSAIC_XTRANS;

  if(!_is_downsample_method(use_method))
  {
    if(bayer && xmethod)   use_method = DT_IOP_DEMOSAIC_RCD;
    if(!bayer && !xmethod) use_method = DT_IOP_DEMOSAIC_MARKESTEIJN;
  }

  const gboolean isppg = (use_method == DT_IOP_DEMOSAIC_PPG);
  const gboolean isdownsample = _is_downsample_method(use_method);
  const gboolean isdual = !isdownsample && (use_method & DEMOSAIC_DUAL);
  const gboolean islmmse = (use_method == DT_IOP_DEMOSAIC_LMMSE);
  const gboolean passing = ((use_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME) ||
                            (use_method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR) ||
                            (use_method == DT_IOP_DEMOSAIC_PASSTHR_MONOX) ||
                            (use_method == DT_IOP_DEMOSAIC_PASSTHR_COLORX));

  gtk_widget_set_visible(g->demosaic_method_bayer, bayer);
  gtk_widget_set_visible(g->demosaic_method_xtrans, !bayer);
  if(bayer)
    dt_bauhaus_combobox_set_from_value(g->demosaic_method_bayer, p->demosaicing_method);
  else
    dt_bauhaus_combobox_set_from_value(g->demosaic_method_xtrans, p->demosaicing_method);

  gtk_widget_set_visible(g->median_thrs, bayer && isppg);
  gtk_widget_set_visible(g->greeneq, !passing && !isdownsample);
  gtk_widget_set_visible(g->color_smoothing, !passing && !isdual);
  gtk_widget_set_visible(g->dual_thrs, isdual);
  gtk_widget_set_visible(g->lmmse_refine, islmmse);

  dt_image_t *img = dt_image_cache_get(darktable.image_cache, self->dev->image_storage.id, 'w');
  if((p->demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME) ||
     (p->demosaicing_method == DT_IOP_DEMOSAIC_PASSTHR_MONOX))
    img->flags |= DT_IMAGE_MONOCHROME_BAYER;
  else
    img->flags &= ~DT_IMAGE_MONOCHROME_BAYER;
  dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);
}
void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_demosaic_gui_data_t *g = (dt_iop_demosaic_gui_data_t *)self->gui_data;
  dt_bauhaus_widget_set_quad_active(g->dual_thrs, FALSE);

  g->visual_mask = FALSE;
  gui_changed(self, NULL, NULL);

  gtk_stack_set_visible_child_name(GTK_STACK(self->widget), self->default_enabled ? "raw" : "non_raw");
}

static void _visualize_callback(GtkWidget *quad, gpointer user_data)
{
  if(darktable.gui->reset) return;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_demosaic_gui_data_t *g = (dt_iop_demosaic_gui_data_t *)self->gui_data;

  g->visual_mask = dt_bauhaus_widget_get_quad_active(quad);
  dt_dev_pixelpipe_update_history_main(self->dev);
}

void gui_focus(struct dt_iop_module_t *self, gboolean in)
{
  dt_iop_demosaic_gui_data_t *g = (dt_iop_demosaic_gui_data_t *)self->gui_data;
  if(!in)
  {
    const gboolean was_dualmask = g->visual_mask;
    dt_bauhaus_widget_set_quad_active(g->dual_thrs, FALSE);
    g->visual_mask = FALSE;
    if(was_dualmask) dt_dev_pixelpipe_update_history_main(self->dev);
  }
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_demosaic_gui_data_t *g = IOP_GUI_ALLOC(demosaic);

  GtkWidget *box_raw = self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->demosaic_method_bayer = dt_bauhaus_combobox_from_params(self, "demosaicing_method");
  for(int i=0;i<7;i++) dt_bauhaus_combobox_remove_at(g->demosaic_method_bayer, 9);
  gtk_widget_set_tooltip_text(g->demosaic_method_bayer, _("Bayer sensor demosaicing method, PPG and RCD are fast, AMaZE and LMMSE are slow.\nLMMSE is suited best for high ISO images.\ndual demosaicers double processing time."));

  g->demosaic_method_xtrans = dt_bauhaus_combobox_from_params(self, "demosaicing_method");
  for(int i=0;i<9;i++) dt_bauhaus_combobox_remove_at(g->demosaic_method_xtrans, 0);
  gtk_widget_set_tooltip_text(g->demosaic_method_xtrans, _("X-Trans sensor demosaicing method, Markesteijn 3-pass and frequency domain chroma are slow.\ndual demosaicers double processing time."));

  g->median_thrs = dt_bauhaus_slider_from_params(self, "median_thrs");
  dt_bauhaus_slider_set_digits(g->median_thrs, 3);
  gtk_widget_set_tooltip_text(g->median_thrs, _("threshold for edge-aware median.\nset to 0.0 to switch off\n"
                                                "set to 1.0 to ignore edges"));

  g->dual_thrs = dt_bauhaus_slider_from_params(self, "dual_thrs");
  dt_bauhaus_slider_set_digits(g->dual_thrs, 2);
  gtk_widget_set_tooltip_text(g->dual_thrs, _("contrast threshold for dual demosaic.\nset to 0.0 for high frequency content\n"
                                                "set to 1.0 for flat content\ntoggle to visualize the mask"));
  dt_bauhaus_widget_set_quad_paint(g->dual_thrs, dtgtk_cairo_paint_showmask, 0, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->dual_thrs, TRUE);
  dt_bauhaus_widget_set_quad_active(g->dual_thrs, FALSE);
  g_signal_connect(G_OBJECT(g->dual_thrs), "quad-pressed", G_CALLBACK(_visualize_callback), self);

  g->lmmse_refine = dt_bauhaus_combobox_from_params(self, "lmmse_refine");
  gtk_widget_set_tooltip_text(g->lmmse_refine, _("LMMSE refinement steps. the median steps average the output,\nrefine adds some recalculation of red & blue channels"));

  g->color_smoothing = dt_bauhaus_combobox_from_params(self, "color_smoothing");
  gtk_widget_set_tooltip_text(g->color_smoothing, _("how many post-demosaic smoothing passes.\nin downsample mode this sets the guided detail equalization iterations"));

  g->greeneq = dt_bauhaus_combobox_from_params(self, "green_eq");
  gtk_widget_set_tooltip_text(g->greeneq, _("green channels matching method"));

  // start building top level widget
  self->widget = gtk_stack_new();
  gtk_stack_set_homogeneous(GTK_STACK(self->widget), FALSE);

  GtkWidget *label_non_raw = dt_ui_label_new(_("not applicable"));
  gtk_widget_set_tooltip_text(label_non_raw, _("demosaicing is only used for color raw images"));

  gtk_stack_add_named(GTK_STACK(self->widget), label_non_raw, "non_raw");
  gtk_stack_add_named(GTK_STACK(self->widget), box_raw, "raw");
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
