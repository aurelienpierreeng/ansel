/*
   This file is part of darktable,
   Copyright (C) 2010 Bruce Guenter.
   Copyright (C) 2010-2011 Henrik Andersson.
   Copyright (C) 2010-2014, 2016 johannes hanika.
   Copyright (C) 2010 Stuart Henderson.
   Copyright (C) 2011 Antony Dovgal.
   Copyright (C) 2011 Robert Bieber.
   Copyright (C) 2011-2014, 2016, 2019 Tobias Ellinghaus.
   Copyright (C) 2011-2012, 2014, 2016-2017 Ulrich Pegelow.
   Copyright (C) 2012, 2015 Edouard Gomez.
   Copyright (C) 2012 Jérémy Rosen.
   Copyright (C) 2012 Richard Wonka.
   Copyright (C) 2013, 2020 Aldric Renaudin.
   Copyright (C) 2014, 2016 Dan Torop.
   Copyright (C) 2014-2016 Roman Lebedev.
   Copyright (C) 2015-2016 Pedro Côrte-Real.
   Copyright (C) 2017 Heiko Bauke.
   Copyright (C) 2017 luzpaz.
   Copyright (C) 2018, 2020-2026 Aurélien PIERRE.
   Copyright (C) 2018 Edgardo Hoszowski.
   Copyright (C) 2018 Maurizio Paglia.
   Copyright (C) 2018-2020, 2022 Pascal Obry.
   Copyright (C) 2018 rawfiner.
   Copyright (C) 2019 Andreas Schneider.
   Copyright (C) 2019 Diederik ter Rahe.
   Copyright (C) 2019-2020, 2022 Hanno Schwalm.
   Copyright (C) 2020 Chris Elston.
   Copyright (C) 2020, 2022 Diederik Ter Rahe.
   Copyright (C) 2020-2021 Ralf Brown.
   Copyright (C) 2021 Hubert Kowalski.
   Copyright (C) 2022 Martin Bařinka.
   Copyright (C) 2022 Philipp Lutz.
   Copyright (C) 2022 Victor Forsiuk.
   Copyright (C) 2023 Alynx Zhou.
   Copyright (C) 2023 Guillaume Stutin.
   Copyright (C) 2023 Luca Zulberti.
   
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
#include "bauhaus/bauhaus.h"
#include "common/box_filters.h"
#include "common/bspline.h"
#include "common/opencl.h"
#include "common/imagebuf.h"
#include "common/fast_guided_filter.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "develop/noise_generator.h"
#include "develop/tiling.h"

#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <inttypes.h>

#define MAX_NUM_SCALES 12
#define REDUCESIZE 64

// Downsampling factor for guided-laplacian
#define DS_FACTOR 4

// Set to one to output intermediate image steps as PFM in /tmp
#define DEBUG_DUMP_PFM 0

static const float DT_ALIGNED_ARRAY anisotropic_kernel_isophote[9]
  = { 0.25f, 0.5f, 0.25f, 0.5f, -3.f, 0.5f, 0.25f, 0.5f, 0.25f };

#if DEBUG_DUMP_PFM
__DT_CLONE_TARGETS__
static void dump_PFM(const char *filename, const float* out, const uint32_t w, const uint32_t h)
{
  FILE *f = g_fopen(filename, "wb");
  fprintf(f, "PF\n%d %d\n-1.0\n", w, h);
  for(int j = h - 1 ; j >= 0 ; j--)
    for(int i = 0 ; i < w ; i++)
      for(int c = 0 ; c < 3 ; c++)
        fwrite(out + (j * w + i) * 4 + c, 1, sizeof(float), f);
  fclose(f);
}
#endif

DT_MODULE_INTROSPECTION(4, dt_iop_highlights_params_t)

typedef enum dt_iop_highlights_mode_t
{
  DT_IOP_HIGHLIGHTS_CLIP = 0,    // $DESCRIPTION: "clip highlights"
  DT_IOP_HIGHLIGHTS_LCH = 1,     // $DESCRIPTION: "reconstruct in LCh"
  DT_IOP_HIGHLIGHTS_INPAINT = 2, // $DESCRIPTION: "reconstruct color"
  DT_IOP_HIGHLIGHTS_LAPLACIAN = 3, //$DESCRIPTION: "guided laplacians"
} dt_iop_highlights_mode_t;

typedef enum dt_atrous_wavelets_scales_t
{
  WAVELETS_1_SCALE = 0,   // $DESCRIPTION: "2 px"
  WAVELETS_2_SCALE = 1,   // $DESCRIPTION: "4 px"
  WAVELETS_3_SCALE = 2,   // $DESCRIPTION: "8 px"
  WAVELETS_4_SCALE = 3,   // $DESCRIPTION: "16 px"
  WAVELETS_5_SCALE = 4,   // $DESCRIPTION: "32 px"
  WAVELETS_6_SCALE = 5,   // $DESCRIPTION: "64 px"
  WAVELETS_7_SCALE = 6,   // $DESCRIPTION: "128 px (slow)"
  WAVELETS_8_SCALE = 7,   // $DESCRIPTION: "256 px (slow)"
  WAVELETS_9_SCALE = 8,   // $DESCRIPTION: "512 px (very slow)"
  WAVELETS_10_SCALE = 9,  // $DESCRIPTION: "1024 px (very slow)"
  WAVELETS_11_SCALE = 10, // $DESCRIPTION: "2048 px (insanely slow)"
  WAVELETS_12_SCALE = 11, // $DESCRIPTION: "4096 px (insanely slow)"
} dt_atrous_wavelets_scales_t;

typedef struct dt_iop_highlights_params_t
{
  // params of v1
  dt_iop_highlights_mode_t mode; // $DEFAULT: DT_IOP_HIGHLIGHTS_CLIP $DESCRIPTION: "method"
  float blendL; // unused $DEFAULT: 1.0
  float blendC; // unused $DEFAULT: 0.0
  float blendh; // unused $DEFAULT: 0.0
  // params of v2
  float clip; // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 1.0 $DESCRIPTION: "clipping threshold"
  // params of v3
  float noise_level; // $MIN: 0. $MAX: 1.0 $DEFAULT: 0.00 $DESCRIPTION: "noise level"
  int iterations; // $MIN: 1 $MAX: 512 $DEFAULT: 30 $DESCRIPTION: "iterations"
  dt_atrous_wavelets_scales_t scales; // $DEFAULT: 8 $DESCRIPTION: "diameter of reconstruction"
  float reconstructing;    // $MIN: 0.0 $MAX: 1.0  $DEFAULT: 0.4 $DESCRIPTION: "cast balance"
  float combine;           // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 2.0 $DESCRIPTION: "combine segments"
  int debugmode;
  // params of v4
  float solid_color; // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.5 $DESCRIPTION: "inpaint a flat color"
} dt_iop_highlights_params_t;

typedef struct dt_iop_highlights_gui_data_t
{
  GtkWidget *clip;
  GtkWidget *mode;
  GtkWidget *noise_level;
  GtkWidget *iterations;
  GtkWidget *scales;
  GtkWidget *solid_color;
  gboolean show_visualize;
} dt_iop_highlights_gui_data_t;

typedef dt_iop_highlights_params_t dt_iop_highlights_data_t;

typedef struct dt_iop_highlights_global_data_t
{
  int kernel_highlights_1f_clip;
  int kernel_highlights_1f_lch_bayer;
  int kernel_highlights_1f_lch_xtrans;
  int kernel_highlights_4f_clip;
  int kernel_highlights_bilinear_and_mask;
  int kernel_highlights_bilinear_and_mask_xtrans;
  int kernel_highlights_normalize_reduce_first;
  int kernel_highlights_normalize_reduce_first_xtrans;
  int kernel_highlights_normalize_reduce_second;
  int kernel_highlights_remosaic_and_replace;
  int kernel_highlights_remosaic_and_replace_xtrans;
  int kernel_highlights_guide_laplacians;
  int kernel_highlights_diffuse_color;
  int kernel_highlights_box_blur;
  int kernel_highlights_false_color;

  int kernel_filmic_bspline_vertical;
  int kernel_filmic_bspline_horizontal;
  int kernel_filmic_bspline_vertical_local;
  int kernel_filmic_bspline_horizontal_local;

  int kernel_interpolate_bilinear;
} dt_iop_highlights_global_data_t;


const char *name()
{
  return _("_highlight reconstruction");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("avoid magenta highlights and try to recover highlights colors"),
                                      _("corrective"),
                                      _("linear, raw, scene-referred"),
                                      _("reconstruction, raw"),
                                      _("linear, raw, scene-referred"));
}

int default_group()
{
  return IOP_GROUP_REPAIR;
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece)
{
  if(piece && piece->dsc_in.cst != IOP_CS_RAW)
    return IOP_CS_RGB;
  return IOP_CS_RAW;
}

void output_format(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                   dt_iop_buffer_dsc_t *dsc)
{
  default_output_format(self, pipe, piece, dsc);
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version == 1 && new_version == 4)
  {
    /*
      params of v2 :
        float clip
      + params of v3
      + params of v4
    */
    memcpy(new_params, old_params, sizeof(dt_iop_highlights_params_t) - 5 * sizeof(float) - 2 * sizeof(int) - sizeof(dt_atrous_wavelets_scales_t));
    dt_iop_highlights_params_t *n = (dt_iop_highlights_params_t *)new_params;
    n->clip = 1.0f;
    n->noise_level = 0.0f;
    n->reconstructing = 0.4f;
    n->combine = 2.f;
    n->debugmode = 0;
    n->iterations = 1;
    n->scales = 5;
    n->solid_color = 0.f;
    return 0;
  }
  if(old_version == 2 && new_version == 4)
  {
    /*
      params of v3 :
        float noise_level;
        int iterations;
        dt_atrous_wavelets_scales_t scales;
        float reconstructing;
        float combine;
        int debugmode;
      + params of v4
    */
    memcpy(new_params, old_params, sizeof(dt_iop_highlights_params_t) - 4 * sizeof(float) - 2 * sizeof(int) - sizeof(dt_atrous_wavelets_scales_t));
    dt_iop_highlights_params_t *n = (dt_iop_highlights_params_t *)new_params;
    n->noise_level = 0.0f;
    n->reconstructing = 0.4f;
    n->combine = 2.f;
    n->debugmode = 0;
    n->iterations = 1;
    n->scales = 5;
    n->solid_color = 0.f;
    return 0;
  }
  if(old_version == 3 && new_version == 4)
  {
    /*
      params of v4 :
        float solid_color;
    */
    memcpy(new_params, old_params, sizeof(dt_iop_highlights_params_t) - sizeof(float));
    dt_iop_highlights_params_t *n = (dt_iop_highlights_params_t *)new_params;
    n->solid_color = 0.f;
    return 0;
  }

  return 1;
}

#ifdef HAVE_OPENCL
static cl_int process_laplacian_bayer_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                                         const dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
                                         const dt_iop_roi_t *const roi_in,
                                         const dt_iop_roi_t *const roi_out, const dt_aligned_pixel_t clips);
static cl_int process_laplacian_xtrans_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                                          const dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
                                          const dt_iop_roi_t *const roi_in,
                                          const dt_iop_roi_t *const roi_out, const dt_aligned_pixel_t clips);

int process_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out)
{
  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  dt_iop_highlights_data_t *d = (dt_iop_highlights_data_t *)piece->data;
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;
  dt_iop_highlights_global_data_t *gd = (dt_iop_highlights_global_data_t *)self->global_data;

  const uint32_t filters = piece->dsc_in.filters;
  const int devid = pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  const gboolean fullpipe = !dt_dev_pixelpipe_has_preview_output(self->dev, pipe, roi_out);
  const gboolean visualizing = (!IS_NULL_PTR(g)) ? g->show_visualize && fullpipe : FALSE;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  cl_mem dev_xtrans = NULL;

  // this works for bayer and X-Trans sensors
  if(visualizing)
  {
    float clips[4] = { 0.995f * d->clip * piece->dsc_in.processed_maximum[0],
                       0.995f * d->clip * piece->dsc_in.processed_maximum[1],
                       0.995f * d->clip * piece->dsc_in.processed_maximum[2],
                       d->clip};


    cl_mem dev_clips = dt_opencl_copy_host_to_device_constant(devid, 4 * sizeof(float), clips);
    if(IS_NULL_PTR(dev_clips)) goto error;

    // bayer sensor raws with LCH mode
    size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_false_color, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_false_color, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_false_color, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_false_color, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_false_color, 4, sizeof(int), (void *)&roi_out->x);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_false_color, 5, sizeof(int), (void *)&roi_out->y);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_false_color, 6, sizeof(int), (void *)&filters);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_false_color, 7, sizeof(cl_mem), (void *)&dev_clips);

    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_false_color, sizes);
    if(err != CL_SUCCESS) goto error;

    ((dt_dev_pixelpipe_t *)pipe)->mask_display = DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU;
    dt_opencl_release_mem_object(dev_clips);
    return TRUE;
  }

  const float clip = d->clip
                     * fminf(piece->dsc_in.processed_maximum[0],
                             fminf(piece->dsc_in.processed_maximum[1], piece->dsc_in.processed_maximum[2]));

  if(!filters)
  {
    // non-raw images use dedicated kernel which just clips
    size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_4f_clip, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_4f_clip, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_4f_clip, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_4f_clip, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_4f_clip, 4, sizeof(int), (void *)&d->mode);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_4f_clip, 5, sizeof(float), (void *)&clip);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_4f_clip, sizes);
    if(err != CL_SUCCESS) goto error;
  }
  else if(d->mode == DT_IOP_HIGHLIGHTS_CLIP || d->mode > DT_IOP_HIGHLIGHTS_LAPLACIAN)
  {
    // raw images with clip mode (both bayer and xtrans)
    // This is also the fallback if d->mode is set with something invalid
    size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_clip, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_clip, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_clip, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_clip, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_clip, 4, sizeof(float), (void *)&clip);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_clip, 5, sizeof(int), (void *)&roi_out->x);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_clip, 6, sizeof(int), (void *)&roi_out->y);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_clip, 7, sizeof(int), (void *)&filters);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_1f_clip, sizes);
    if(err != CL_SUCCESS) goto error;
  }
  else if(d->mode == DT_IOP_HIGHLIGHTS_LCH && filters != 9u)
  {
    // bayer sensor raws with LCH mode
    size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_lch_bayer, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_lch_bayer, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_lch_bayer, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_lch_bayer, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_lch_bayer, 4, sizeof(float), (void *)&clip);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_lch_bayer, 5, sizeof(int), (void *)&roi_out->x);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_lch_bayer, 6, sizeof(int), (void *)&roi_out->y);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_lch_bayer, 7, sizeof(int), (void *)&filters);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_1f_lch_bayer, sizes);
    if(err != CL_SUCCESS) goto error;
  }
  else if(d->mode == DT_IOP_HIGHLIGHTS_LCH && filters == 9u)
  {
    // xtrans sensor raws with LCH mode
    int blocksizex, blocksizey;

    dt_opencl_local_buffer_t locopt
      = (dt_opencl_local_buffer_t){ .xoffset = 2 * 2, .xfactor = 1, .yoffset = 2 * 2, .yfactor = 1,
                                    .cellsize = sizeof(float), .overhead = 0,
                                    .sizex = 1 << 8, .sizey = 1 << 8 };

    if(dt_opencl_local_buffer_opt(devid, gd->kernel_highlights_1f_lch_xtrans, &locopt))
    {
      blocksizex = locopt.sizex;
      blocksizey = locopt.sizey;
    }
    else
      blocksizex = blocksizey = 1;

    dev_xtrans
        = dt_opencl_copy_host_to_device_constant(devid, sizeof(piece->dsc_in.xtrans), (void *)piece->dsc_in.xtrans);
    if(IS_NULL_PTR(dev_xtrans)) goto error;

    size_t sizes[] = { ROUNDUP(width, blocksizex), ROUNDUP(height, blocksizey), 1 };
    size_t local[] = { blocksizex, blocksizey, 1 };
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_lch_xtrans, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_lch_xtrans, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_lch_xtrans, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_lch_xtrans, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_lch_xtrans, 4, sizeof(float), (void *)&clip);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_lch_xtrans, 5, sizeof(int), (void *)&roi_out->x);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_lch_xtrans, 6, sizeof(int), (void *)&roi_out->y);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_lch_xtrans, 7, sizeof(cl_mem), (void *)&dev_xtrans);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_lch_xtrans, 8,
                               sizeof(float) * (blocksizex + 4) * (blocksizey + 4), NULL);

    err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_highlights_1f_lch_xtrans, sizes, local);
    if(err != CL_SUCCESS) goto error;
  }
  else if(d->mode == DT_IOP_HIGHLIGHTS_LAPLACIAN)
  {
    const dt_aligned_pixel_t clips = {  0.995f * d->clip * piece->dsc_in.processed_maximum[0],
                                        0.995f * d->clip * piece->dsc_in.processed_maximum[1],
                                        0.995f * d->clip * piece->dsc_in.processed_maximum[2], clip };
    err = (filters == 9u)
              ? process_laplacian_xtrans_cl(self, pipe, piece, dev_in, dev_out, roi_in, roi_out, clips)
              : process_laplacian_bayer_cl(self, pipe, piece, dev_in, dev_out, roi_in, roi_out, clips);
    if(err != CL_SUCCESS) goto error;
  }

  dt_opencl_release_mem_object(dev_xtrans);
  return TRUE;

error:
  dt_opencl_release_mem_object(dev_xtrans);
  dt_print(DT_DEBUG_OPENCL, "[opencl_highlights] couldn't enqueue kernel! %i\n", err);
  return FALSE;
}
#endif

void tiling_callback(struct dt_iop_module_t *self, const struct dt_dev_pixelpipe_t *pipe, const struct dt_dev_pixelpipe_iop_t *piece, struct dt_develop_tiling_t *tiling)
{
  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  dt_iop_highlights_data_t *d = (dt_iop_highlights_data_t *)piece->data;
  const uint32_t filters = piece->dsc_in.filters;

  if(d->mode == DT_IOP_HIGHLIGHTS_LAPLACIAN && filters)
  {
    // Mosaic CFA and guided laplacian method: prepare for wavelets decomposition.
    const float scale = fmaxf(DS_FACTOR * dt_dev_get_module_scale(pipe, roi_in), 1.f);
    const float final_radius = (float)((int)(1 << d->scales)) / scale;
    const int scales = CLAMP((int)ceilf(log2f(final_radius)), 1, MAX_NUM_SCALES);
    const int max_filter_radius = (1 << scales);

    // Warning: in and out are single-channel in RAW mode.
    // in + out + interpolated + ds_interpolated + ds_tmp + 2 * ds_LF + ds_HF + mask + ds_mask
    tiling->factor = 2.f + 2.f * 4 + 6.f * 4 / DS_FACTOR;
    // OpenCL adds a downsampled scratch accumulator to keep the guided-laplacian read/write images distinct.
    // in + out + interpolated + temp + mask + ds_interpolated + reconstructed_scratch + 2 * ds_LF + ds_HF + ds_mask
    tiling->factor_cl = 2.f + 3.f * 4 + 6.f * 4 / DS_FACTOR;

    // The wavelets decomposition uses a temp buffer of size 4 x ds_width
    tiling->maxbuf = 1.f / roi_in->height * 4.f / DS_FACTOR;

    // No temp buffer on GPU
    tiling->maxbuf_cl = 1.0f;
    tiling->overhead = 0;

    // Note : if we were not doing anything iterative,
    // max_filter_radius would not need to be factored more.
    // Since we are iterating within tiles, we need more padding.
    // The clean way of doing it would be an internal tiling mechanism
    // where we restitch the tiles between each new iteration.
    tiling->overlap = max_filter_radius * 1.5f / DS_FACTOR;
    tiling->xalign = (filters == 9u) ? 6 : 2;
    tiling->yalign = (filters == 9u) ? 6 : 2;

    return;
  }

  tiling->factor = 2.0f;  // in + out
  tiling->maxbuf = 1.0f;
  tiling->overhead = 0;

  if(filters == 9u)
  {
    // xtrans
    tiling->xalign = 6;
    tiling->yalign = 6;
    tiling->overlap = (d->mode == DT_IOP_HIGHLIGHTS_LCH) ? 2 : 0;
  }
  else if(filters)
  {
    // bayer
    tiling->xalign = 2;
    tiling->yalign = 2;
    tiling->overlap = (d->mode == DT_IOP_HIGHLIGHTS_LCH) ? 1 : 0;
  }
  else
  {
    // non-raw
    tiling->xalign = 1;
    tiling->yalign = 1;
    tiling->overlap = 0;
  }
}

/* interpolate value for a pixel, ideal via ratio to nearby pixel */
static inline float interp_pix_xtrans(const int ratio_next,
                                      const ssize_t offset_next,
                                      const float clip0, const float clip_next,
                                      const float *const in,
                                      const float *const ratios)
{
  assert(ratio_next != 0);
  // it's OK to exceed clipping of current pixel's color based on a
  // neighbor -- that is the purpose of interpolating highlight
  // colors
  const float clip_val = fmaxf(clip0, clip_next);
  if(in[offset_next] >= clip_next - 1e-5f)
  {
    // next pixel is also clipped
    return clip_val;
  }
  else
  {
    // set this pixel in ratio to the next
    assert(ratio_next != 0);
    if (ratio_next > 0)
      return fminf(in[offset_next] / ratios[ratio_next], clip_val);
    else
      return fminf(in[offset_next] * ratios[-ratio_next], clip_val);
  }
}

static inline void interpolate_color_xtrans(const void *const ivoid, void *const ovoid,
                                            const dt_iop_roi_t *const roi_in,
                                            const dt_iop_roi_t *const roi_out,
                                            int dim, int dir, int other,
                                            const float *const clip,
                                            const uint8_t (*const xtrans)[6],
                                            const int pass)
{
  // In Bayer each row/col has only green/red or green/blue
  // transitions, hence can reconstruct color by single ratio per
  // row. In x-trans there can be transitions between arbitrary colors
  // in a row/col (and 2x2 green blocks which provide no color
  // transition information). Hence calculate multiple color ratios
  // for each row/col.

  // Lookup for color ratios, e.g. red -> blue is roff[0][2] and blue
  // -> red is roff[2][0]. Returned value is an index into ratios. If
  // negative, then need to invert the ratio. Identity color
  // transitions aren't used.
  const int roff[3][3] = {{ 0, -1, -2},
                          { 1,  0, -3},
                          { 2,  3,  0}};
  // record ratios of color transitions 0:unused, 1:RG, 2:RB, and 3:GB
  dt_aligned_pixel_t ratios = {1.0f, 1.0f, 1.0f, 1.0f};

  // passes are 0:+x, 1:-x, 2:+y, 3:-y
  // dims are 0:traverse a row, 1:traverse a column
  // dir is 1:left to right, -1: right to left
  int i = (dim == 0) ? 0 : other;
  int j = (dim == 0) ? other : 0;
  const ssize_t offs = (ssize_t)(dim ? roi_out->width : 1) * ((dir < 0) ? -1 : 1);
  const ssize_t offl = offs - (dim ? 1 : roi_out->width);
  const ssize_t offr = offs + (dim ? 1 : roi_out->width);
  int beg, end;
  if(dir == 1)
  {
    beg = 0;
    end = (dim == 0) ? roi_out->width : roi_out->height;
  }
  else
  {
    beg = ((dim == 0) ? roi_out->width : roi_out->height) - 1;
    end = -1;
  }

  float *in, *out;
  if(dim == 1)
  {
    out = (float *)ovoid + (size_t)i + (size_t)beg * roi_out->width;
    in = (float *)ivoid + (size_t)i + (size_t)beg * roi_in->width;
  }
  else
  {
    out = (float *)ovoid + (size_t)beg + (size_t)j * roi_out->width;
    in = (float *)ivoid + (size_t)beg + (size_t)j * roi_in->width;
  }

  for(int k = beg; k != end; k += dir)
  {
    if(dim == 1)
      j = k;
    else
      i = k;

    const uint8_t f0 = FCxtrans(j, i, roi_in, xtrans);
    const uint8_t f1 = FCxtrans(dim ? (j + dir) : j, dim ? i : (i + dir), roi_in, xtrans);
    const uint8_t fl = FCxtrans(dim ? (j + dir) : (j - 1), dim ? (i - 1) : (i + dir), roi_in, xtrans);
    const uint8_t fr = FCxtrans(dim ? (j + dir) : (j + 1), dim ? (i + 1) : (i + dir), roi_in, xtrans);
    const float clip0 = clip[f0];
    const float clip1 = clip[f1];
    const float clipl = clip[fl];
    const float clipr = clip[fr];
    const float clip_max = fmaxf(fmaxf(clip[0], clip[1]), clip[2]);

    if(i == 0 || i == roi_out->width - 1 || j == 0 || j == roi_out->height - 1)
    {
      if(pass == 3) out[0] = fminf(clip_max, in[0]);
    }
    else
    {
      // ratio to next pixel if this & next are unclamped and not in
      // 2x2 green block
      if ((f0 != f1) &&
          (in[0] < clip0 && in[0] > 1e-5f) &&
          (in[offs] < clip1 && in[offs] > 1e-5f))
      {
        const int r = roff[f0][f1];
        assert(r != 0);
        if (r > 0)
          ratios[r] = (3.f * ratios[r] + (in[offs] / in[0])) / 4.f;
        else
          ratios[-r] = (3.f * ratios[-r] + (in[0] / in[offs])) / 4.f;
      }

      if(in[0] >= clip0 - 1e-5f)
      {
        // interplate color for clipped pixel
        float add;
        if(f0 != f1)
          // next pixel is different color
          add =
            interp_pix_xtrans(roff[f0][f1], offs, clip0, clip1, in, ratios);
        else
          // at start of 2x2 green block, look diagonally
          add = (fl != f0) ?
            interp_pix_xtrans(roff[f0][fl], offl, clip0, clipl, in, ratios) :
            interp_pix_xtrans(roff[f0][fr], offr, clip0, clipr, in, ratios);

        if(pass == 0)
          out[0] = add;
        else if(pass == 3)
          out[0] = fminf(clip_max, (out[0] + add) / 4.0f);
        else
          out[0] += add;
      }
      else
      {
        // pixel is not clipped
        if(pass == 3) out[0] = in[0];
      }
    }
    out += offs;
    in += offs;
  }
}

static inline void interpolate_color(const void *const ivoid, void *const ovoid,
                                     const dt_iop_roi_t *const roi_out, int dim, int dir, int other,
                                     const float *clip, const uint32_t filters, const int pass)
{
  float ratio = 1.0f;
  float *in, *out;

  int i = 0, j = 0;
  if(dim == 0)
    j = other;
  else
    i = other;
  ssize_t offs = dim ? roi_out->width : 1;
  if(dir < 0) offs = -offs;
  int beg, end;
  if(dim == 0 && dir == 1)
  {
    beg = 0;
    end = roi_out->width;
  }
  else if(dim == 0 && dir == -1)
  {
    beg = roi_out->width - 1;
    end = -1;
  }
  else if(dim == 1 && dir == 1)
  {
    beg = 0;
    end = roi_out->height;
  }
  else if(dim == 1 && dir == -1)
  {
    beg = roi_out->height - 1;
    end = -1;
  }
  else
    return;

  if(dim == 1)
  {
    out = (float *)ovoid + i + (size_t)beg * roi_out->width;
    in = (float *)ivoid + i + (size_t)beg * roi_out->width;
  }
  else
  {
    out = (float *)ovoid + beg + (size_t)j * roi_out->width;
    in = (float *)ivoid + beg + (size_t)j * roi_out->width;
  }
  for(int k = beg; k != end; k += dir)
  {
    if(dim == 1)
      j = k;
    else
      i = k;
    const float clip0 = clip[FC(j, i, filters)];
    const float clip1 = clip[FC(dim ? (j + 1) : j, dim ? i : (i + 1), filters)];
    if(i == 0 || i == roi_out->width - 1 || j == 0 || j == roi_out->height - 1)
    {
      if(pass == 3) out[0] = in[0];
    }
    else
    {
      if(in[0] < clip0 && in[0] > 1e-5f)
      { // both are not clipped
        if(in[offs] < clip1 && in[offs] > 1e-5f)
        { // update ratio, exponential decay. ratio = in[odd]/in[even]
          if(k & 1)
            ratio = (3.0f * ratio + in[0] / in[offs]) / 4.0f;
          else
            ratio = (3.0f * ratio + in[offs] / in[0]) / 4.0f;
        }
      }

      if(in[0] >= clip0 - 1e-5f)
      { // in[0] is clipped, restore it as in[1] adjusted according to ratio
        float add = 0.0f;
        if(in[offs] >= clip1 - 1e-5f)
          add = fmaxf(clip0, clip1);
        else if(k & 1)
          add = in[offs] * ratio;
        else
          add = in[offs] / ratio;

        if(pass == 0)
          out[0] = add;
        else if(pass == 3)
          out[0] = (out[0] + add) / 4.0f;
        else
          out[0] += add;
      }
      else
      {
        if(pass == 3) out[0] = in[0];
      }
    }
    out += offs;
    in += offs;
  }
}

/*
 * these 2 constants were computed using following Sage code:
 *
 * sqrt3 = sqrt(3)
 * sqrt12 = sqrt(12) # 2*sqrt(3)
 *
 * print 'sqrt3 = ', sqrt3, ' ~= ', RealField(128)(sqrt3)
 * print 'sqrt12 = ', sqrt12, ' ~= ', RealField(128)(sqrt12)
 */
#define SQRT3 1.7320508075688772935274463415058723669L
#define SQRT12 3.4641016151377545870548926830117447339L // 2*SQRT3

__DT_CLONE_TARGETS__
static void process_lch_bayer(dt_iop_module_t *self, const dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
                              void *const ovoid, const dt_iop_roi_t *const roi_in,
                              const dt_iop_roi_t *const roi_out, const float clip)
{
  const uint32_t filters = piece->dsc_in.filters;
  __OMP_PARALLEL_FOR__(collapse(2))
  for(int j = 0; j < roi_out->height; j++)
  {
    for(int i = 0; i < roi_out->width; i++)
    {
      float *const out = (float *)ovoid + (size_t)roi_out->width * j + i;
      const float *const in = (float *)ivoid + (size_t)roi_out->width * j + i;

      if(i == roi_out->width - 1 || j == roi_out->height - 1)
      {
        // fast path for border
        out[0] = MIN(clip, in[0]);
      }
      else
      {
        int clipped = 0;

        // sample 1 bayer block. thus we will have 2 green values.
        float R = 0.0f, Gmin = FLT_MAX, Gmax = -FLT_MAX, B = 0.0f;
        for(int jj = 0; jj <= 1; jj++)
        {
          for(int ii = 0; ii <= 1; ii++)
          {
            const float val = in[(size_t)jj * roi_out->width + ii];

            clipped = (clipped || (val > clip));

            const int c = FC(j + jj + roi_out->y, i + ii + roi_out->x, filters);
            switch(c)
            {
              case 0:
                R = val;
                break;
              case 1:
                Gmin = MIN(Gmin, val);
                Gmax = MAX(Gmax, val);
                break;
              case 2:
                B = val;
                break;
            }
          }
        }

        if(clipped)
        {
          const float Ro = MIN(R, clip);
          const float Go = MIN(Gmin, clip);
          const float Bo = MIN(B, clip);

          const float L = (R + Gmax + B) / 3.0f;

          float C = SQRT3 * (R - Gmax);
          float H = 2.0f * B - Gmax - R;

          const float Co = SQRT3 * (Ro - Go);
          const float Ho = 2.0f * Bo - Go - Ro;

          if(R != Gmax && Gmax != B)
          {
            const float ratio = sqrtf((Co * Co + Ho * Ho) / (C * C + H * H));
            C *= ratio;
            H *= ratio;
          }

          dt_aligned_pixel_t RGB = { 0.0f, 0.0f, 0.0f };

          /*
           * backtransform proof, sage:
           *
           * R,G,B,L,C,H = var('R,G,B,L,C,H')
           * solve([L==(R+G+B)/3, C==sqrt(3)*(R-G), H==2*B-G-R], R, G, B)
           *
           * result:
           * [[R == 1/6*sqrt(3)*C - 1/6*H + L, G == -1/6*sqrt(3)*C - 1/6*H + L, B == 1/3*H + L]]
           */
          RGB[0] = L - H / 6.0f + C / SQRT12;
          RGB[1] = L - H / 6.0f - C / SQRT12;
          RGB[2] = L + H / 3.0f;

          out[0] = RGB[FC(j + roi_out->y, i + roi_out->x, filters)];
        }
        else
        {
          out[0] = in[0];
        }
      }
    }
  }
}

__DT_CLONE_TARGETS__
static void process_lch_xtrans(dt_iop_module_t *self, const dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
                               void *const ovoid, const dt_iop_roi_t *const roi_in,
                               const dt_iop_roi_t *const roi_out, const float clip)
{
  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->dsc_in.xtrans;
  __OMP_PARALLEL_FOR__()
  for(int j = 0; j < roi_out->height; j++)
  {
    float *out = (float *)ovoid + (size_t)roi_out->width * j;
    float *in = (float *)ivoid + (size_t)roi_in->width * j;

    // bit vector used as ring buffer to remember clipping of current
    // and last two columns, checking current pixel and its vertical
    // neighbors
    int cl = 0;

    for(int i = 0; i < roi_out->width; i++)
    {
      // update clipping ring buffer
      cl = (cl << 1) & 6;
      if(j >= 2 && j <= roi_out->height - 3)
      {
        cl |= (in[-roi_in->width] > clip) | (in[0] > clip) | (in[roi_in->width] > clip);
      }

      if(i < 2 || i > roi_out->width - 3 || j < 2 || j > roi_out->height - 3)
      {
        // fast path for border
        out[0] = MIN(clip, in[0]);
      }
      else
      {
        // if current pixel is clipped, always reconstruct
        int clipped = (in[0] > clip);
        if(!clipped)
        {
          clipped = cl;
          if(clipped)
          {
            // If the ring buffer can't show we are in an obviously
            // unclipped region, this is the slow case: check if there
            // is any 3x3 block touching the current pixel which has
            // no clipping, as then don't need to reconstruct the
            // current pixel. This avoids zippering in edge
            // transitions from clipped to unclipped areas. The
            // X-Trans sensor seems prone to this, unlike Bayer, due
            // to its irregular pattern.
            for(int offset_j = -2; offset_j <= 0; offset_j++)
            {
              for(int offset_i = -2; offset_i <= 0; offset_i++)
              {
                if(clipped)
                {
                  clipped = 0;
                  for(int jj = offset_j; jj <= offset_j + 2; jj++)
                  {
                    for(int ii = offset_i; ii <= offset_i + 2; ii++)
                    {
                      const float val = in[(ssize_t)jj * roi_in->width + ii];
                      clipped = (clipped || (val > clip));
                    }
                  }
                }
              }
            }
          }
        }

        if(clipped)
        {
          dt_aligned_pixel_t mean = { 0.0f, 0.0f, 0.0f };
          dt_aligned_pixel_t RGBmax = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
          int cnt[3] = { 0, 0, 0 };

          for(int jj = -1; jj <= 1; jj++)
          {
            for(int ii = -1; ii <= 1; ii++)
            {
              const float val = in[(ssize_t)jj * roi_in->width + ii];
              const int c = FCxtrans(j+jj, i+ii, roi_in, xtrans);
              mean[c] += val;
              cnt[c]++;
              RGBmax[c] = MAX(RGBmax[c], val);
            }
          }

          const float Ro = MIN(mean[0]/cnt[0], clip);
          const float Go = MIN(mean[1]/cnt[1], clip);
          const float Bo = MIN(mean[2]/cnt[2], clip);

          const float R = RGBmax[0];
          const float G = RGBmax[1];
          const float B = RGBmax[2];

          const float L = (R + G + B) / 3.0f;

          float C = SQRT3 * (R - G);
          float H = 2.0f * B - G - R;

          const float Co = SQRT3 * (Ro - Go);
          const float Ho = 2.0f * Bo - Go - Ro;

          if(R != G && G != B)
          {
            const float ratio = sqrtf((Co * Co + Ho * Ho) / (C * C + H * H));
            C *= ratio;
            H *= ratio;
          }

          dt_aligned_pixel_t RGB = { 0.0f, 0.0f, 0.0f };

          RGB[0] = L - H / 6.0f + C / SQRT12;
          RGB[1] = L - H / 6.0f - C / SQRT12;
          RGB[2] = L + H / 3.0f;

          out[0] = RGB[FCxtrans(j, i, roi_out, xtrans)];
        }
        else
          out[0] = in[0];
      }
      out++;
      in++;
    }
  }
}

#undef SQRT3
#undef SQRT12

__DT_CLONE_TARGETS__
static void _interpolate_and_mask(const float *const restrict input,
                                  float *const restrict interpolated,
                                  float *const restrict clipping_mask,
                                  const dt_aligned_pixel_t clips,
                                  const dt_aligned_pixel_t wb,
                                  const uint32_t filters,
                                  const size_t width, const size_t height)
{
  // Bilinear interpolation
  __OMP_PARALLEL_FOR__()
  for(size_t i = 0; i < height; i++)
    for(size_t j = 0; j < width; j++)
    {
      const size_t c = FC(i, j, filters);
      const size_t i_center = i * width;
      const float center = input[i_center + j];

      float R = 0.f;
      float G = 0.f;
      float B = 0.f;

      int R_clipped = 0;
      int G_clipped = 0;
      int B_clipped = 0;

      if(i == 0 || j == 0 || i == height - 1 || j == width - 1)
      {
        // We are on the image edges. We don't need to demosaic,
        // just set R = G = B = center and record clipping.
        // This will introduce a marginal error close to edges, mostly irrelevant
        // because we are dealing with local averages anyway, later on.
        // Also we remosaic the image at the end, so only the relevant channel gets picked.
        // Finally, it's unlikely that the borders of the image get clipped due to vignetting.
        R = G = B = center;
        R_clipped = G_clipped = B_clipped = (center > clips[c]);
      }
      else
      {
        const size_t i_prev = (i - 1) * width;
        const size_t i_next = (i + 1) * width;
        const size_t j_prev = (j - 1);
        const size_t j_next = (j + 1);

        const float north = input[i_prev + j];
        const float south = input[i_next + j];
        const float west = input[i_center + j_prev];
        const float east = input[i_center + j_next];

        const float north_east = input[i_prev + j_next];
        const float north_west = input[i_prev + j_prev];
        const float south_east = input[i_next + j_next];
        const float south_west = input[i_next + j_prev];

        if(c == GREEN) // green pixel
        {
          G = center;
          G_clipped = (center > clips[GREEN]);
        }
        else // non-green pixel
        {
          // interpolate inside an X/Y cross
          G = (north + south + east + west) / 4.f;
          G_clipped = (north > clips[GREEN] || south > clips[GREEN] || east > clips[GREEN] || west > clips[GREEN]);
        }

        if(c == RED ) // red pixel
        {
          R = center;
          R_clipped = (center > clips[RED]);
        }
        else // non-red pixel
        {
          if(FC(i - 1, j, filters) == RED && FC(i + 1, j, filters) == RED)
          {
            // we are on a red column, so interpolate column-wise
            R = (north + south) / 2.f;
            R_clipped = (north > clips[RED] || south > clips[RED]);
          }
          else if(FC(i, j - 1, filters) == RED && FC(i, j + 1, filters) == RED)
          {
            // we are on a red row, so interpolate row-wise
            R = (west + east) / 2.f;
            R_clipped = (west > clips[RED] || east > clips[RED]);
          }
          else
          {
            // we are on a blue row, so interpolate inside a square
            R = (north_west + north_east + south_east + south_west) / 4.f;
            R_clipped = (north_west > clips[RED] || north_east > clips[RED] || south_west > clips[RED]
                          || south_east > clips[RED]);
          }
        }

        if(c == BLUE ) // blue pixel
        {
          B = center;
          B_clipped = (center > clips[BLUE]);
        }
        else // non-blue pixel
        {
          if(FC(i - 1, j, filters) == BLUE && FC(i + 1, j, filters) == BLUE)
          {
            // we are on a blue column, so interpolate column-wise
            B = (north + south) / 2.f;
            B_clipped = (north > clips[BLUE] || south > clips[BLUE]);
          }
          else if(FC(i, j - 1, filters) == BLUE && FC(i, j + 1, filters) == BLUE)
          {
            // we are on a red row, so interpolate row-wise
            B = (west + east) / 2.f;
            B_clipped = (west > clips[BLUE] || east > clips[BLUE]);
          }
          else
          {
            // we are on a red row, so interpolate inside a square
            B = (north_west + north_east + south_east + south_west) / 4.f;

            B_clipped = (north_west > clips[BLUE] || north_east > clips[BLUE] || south_west > clips[BLUE]
                        || south_east > clips[BLUE]);
          }
        }
      }

      dt_aligned_pixel_t RGB = { R, G, B, sqrtf(sqf(R) + sqf(G) + sqf(B)) };
      dt_aligned_pixel_t clipped = { R_clipped, G_clipped, B_clipped, (R_clipped || G_clipped || B_clipped) };

      for_each_channel(k, aligned(RGB, interpolated, clipping_mask, clipped, wb))
      {
        const size_t idx = (i * width + j) * 4 + k;
        interpolated[idx] = fmaxf(RGB[k] / wb[k], 0.f);
        clipping_mask[idx] = clipped[k];
      }
    }
}

/** Compute channel normalization factors from the current raw ROI.
 *
 * Guided Laplacians only needs a relative RGB normalization before the temporary
 * bilinear reconstruction. Using the average measured value of each CFA color in
 * the current tile keeps the normalization explicit and local to the data being
 * reconstructed, instead of relying on the white balance declared upstream.
 */
__DT_CLONE_TARGETS__
static void _compute_laplacian_normalization(const float *const restrict input,
                                             const dt_iop_roi_t *const roi_in,
                                             const uint32_t filters,
                                             const uint8_t (*const xtrans)[6],
                                             dt_aligned_pixel_t normalization)
{
  float sum_R = 0.f;
  float sum_G = 0.f;
  float sum_B = 0.f;
  const float n_pixels = roi_in->height * roi_in->width;
  __OMP_PARALLEL_FOR__(collapse(2) reduction(+:sum_R, sum_G, sum_B) )
  for(size_t i = 0; i < roi_in->height; i++)
    for(size_t j = 0; j < roi_in->width; j++)
    {
      const int c = (filters == 9u) ? FCxtrans((int)i, (int)j, roi_in, xtrans) : FC(i, j, filters);
      if(c < 0 || c > 2) continue;

      const float value = input[i * roi_in->width + j] / n_pixels;
      if(c == RED)
        sum_R += value;
      else if(c == GREEN)
        sum_G += value;
      else
        sum_B += value;
    }

  normalization[RED] = sum_R;
  normalization[GREEN] = sum_G;
  normalization[BLUE] = sum_B;
  normalization[ALPHA] = 1.f;
}

/** Build the X-Trans bilinear interpolation lookup for the current ROI phase.
 *
 * The lookup keeps the contributing 3x3 neighbours explicit for each position of
 * the 6x6 X-Trans period so CPU and OpenCL guided-laplacian paths start from the
 * same simple bilinear reconstruction.
 */
__DT_CLONE_TARGETS__
static void _build_xtrans_bilinear_lookup(int32_t lookup[6][6][32],
                                          const dt_iop_roi_t *const roi_in,
                                          const uint8_t (*const xtrans)[6])
{
  for(int row = 0; row < 6; row++)
    for(int col = 0; col < 6; col++)
    {
      int32_t *ip = &(lookup[row][col][1]);
      int sum[3] = { 0 };
      const int f = FCxtrans(row, col, roi_in, xtrans);

      // Loop over the local 3x3 support and keep every weighted contributor of
      // the missing colors visible in the lookup table.
      for(int y = -1; y <= 1; y++)
        for(int x = -1; x <= 1; x++)
        {
          const int weight = 1 << ((y == 0) + (x == 0));
          const int color = FCxtrans(row + y, col + x, roi_in, xtrans);
          if(color == f) continue;
          *ip++ = (y << 16) | (x & 0xffffu);
          *ip++ = weight;
          *ip++ = color;
          sum[color] += weight;
        }

      lookup[row][col][0] = (ip - &(lookup[row][col][0])) / 3;
      for(int c = 0; c < 3; c++)
        if(c != f)
        {
          *ip++ = c;
          *ip++ = sum[c];
        }
      *ip = f;
    }
}

/** Bilinearly demosaic the X-Trans raw mosaic and record clipped colors.
 *
 * Guided Laplacians operates on temporary RGB data. For X-Trans we use the same
 * lightweight bilinear neighbourhood as the linear VNG stage so the diffusion
 * begins from a simple and explicit reconstruction.
 */
__DT_CLONE_TARGETS__
static void _interpolate_and_mask_xtrans(const float *const restrict input,
                                         float *const restrict interpolated,
                                         float *const restrict clipping_mask,
                                         const dt_aligned_pixel_t clips,
                                         const dt_aligned_pixel_t wb,
                                         const dt_iop_roi_t *const roi_in,
                                         const int32_t lookup[6][6][32],
                                         const uint8_t (*const xtrans)[6],
                                         const size_t width, const size_t height)
{
  __OMP_PARALLEL_FOR__()
  for(size_t i = 0; i < height; i++)
    for(size_t j = 0; j < width; j++)
    {
      const size_t idx = i * width + j;
      const float center = input[idx];

      dt_aligned_pixel_t RGB = { 0.f };
      dt_aligned_pixel_t clipped = { 0.f };

      if(i == 0 || j == 0 || i == height - 1 || j == width - 1)
      {
        dt_aligned_pixel_t sum = { 0.f };
        int count[3] = { 0 };
        int used_clipped[3] = { 0 };
        const int f = FCxtrans((int)i, (int)j, roi_in, xtrans);

        // Along tile borders we average only the available neighbours because
        // the full 3x3 support would otherwise leave the current ROI.
        for(int y = MAX((int)i - 1, 0); y <= MIN((int)i + 1, (int)height - 1); y++)
          for(int x = MAX((int)j - 1, 0); x <= MIN((int)j + 1, (int)width - 1); x++)
          {
            const int color = FCxtrans(y, x, roi_in, xtrans);
            const float value = input[(size_t)y * width + x];
            sum[color] += value;
            count[color]++;
            used_clipped[color] |= (value > clips[color]);
          }

        for(int c = 0; c < 3; c++)
        {
          const int has_samples = (count[c] > 0);
          RGB[c] = (c == f || !has_samples) ? center : sum[c] / count[c];
          clipped[c] = (c == f || !has_samples) ? (center > clips[c]) : used_clipped[c];
        }
      }
      else
      {
        const int32_t *ip = &(lookup[i % 6][j % 6][0]);
        dt_aligned_pixel_t sum = { 0.f };
        int used_clipped[3] = { 0 };
        const int neighbours = *ip++;

        // We are looping on every neighbour that contributes to a missing color
        // so the interpolation follows the X-Trans CFA geometry exactly.
        for(int k = 0; k < neighbours; k++, ip += 3)
        {
          const int32_t offset = ip[0];
          const int x = (int16_t)(offset & 0xffffu);
          const int y = (int16_t)(offset >> 16);
          const size_t neighbour = ((size_t)((int)i + y) * width + (size_t)((int)j + x));
          const int color = ip[2];
          const float value = input[neighbour];
          sum[color] += value * ip[1];
          used_clipped[color] |= (value > clips[color]);
        }

        // Normalize the two missing colors from the accumulated weights, then
        // restore the measured center color unchanged.
        for(int k = 0; k < 2; k++, ip += 2)
        {
          const int color = ip[0];
          const int total = ip[1];
          RGB[color] = (total > 0) ? sum[color] / total : center;
          clipped[color] = used_clipped[color];
        }

        const int f = *ip;
        RGB[f] = center;
        clipped[f] = (center > clips[f]);
      }

      RGB[ALPHA] = sqrtf(sqf(RGB[RED]) + sqf(RGB[GREEN]) + sqf(RGB[BLUE]));
      clipped[ALPHA] = (clipped[RED] || clipped[GREEN] || clipped[BLUE]);

      for_each_channel(k, aligned(RGB, interpolated, clipping_mask, clipped, wb))
      {
        const size_t index = idx * 4 + k;
        interpolated[index] = fmaxf(RGB[k] / wb[k], 0.f);
        clipping_mask[index] = clipped[k];
      }
    }
}

__DT_CLONE_TARGETS__
static void _remosaic_and_replace(const float *const restrict input,
                                  const float *const restrict interpolated,
                                  const float *const restrict clipping_mask,
                                  float *const restrict output,
                                  const dt_aligned_pixel_t wb,
                                  const uint32_t filters,
                                  const size_t width, const size_t height)
{
  // Take RGB ratios and norm, reconstruct RGB and remosaic the image
  __OMP_PARALLEL_FOR__()
  for(size_t i = 0; i < height; i++)
    for(size_t j = 0; j < width; j++)
    {
      const size_t c = FC(i, j, filters);
      const size_t idx = i * width + j;
      const size_t index = idx * 4;
      const float opacity = clipping_mask[index + ALPHA];
      output[idx] = opacity * fmaxf(interpolated[index + c] * wb[c], 0.f)
                    + (1.f - opacity) * input[idx];
    }
}

/** Reproject the reconstructed RGB back onto the X-Trans mosaic. */
__DT_CLONE_TARGETS__
static void _remosaic_and_replace_xtrans(const float *const restrict input,
                                         const float *const restrict interpolated,
                                         const float *const restrict clipping_mask,
                                         float *const restrict output,
                                         const dt_aligned_pixel_t wb,
                                         const dt_iop_roi_t *const roi_in,
                                         const uint8_t (*const xtrans)[6],
                                         const size_t width, const size_t height)
{
  __OMP_PARALLEL_FOR__()
  for(size_t i = 0; i < height; i++)
    for(size_t j = 0; j < width; j++)
    {
      const size_t idx = i * width + j;
      const size_t index = idx * 4;
      const int c = FCxtrans((int)i, (int)j, roi_in, xtrans);
      const float opacity = clipping_mask[index + ALPHA];
      output[idx] = opacity * fmaxf(interpolated[index + c] * wb[c], 0.f)
                    + (1.f - opacity) * input[idx];
    }
}

typedef enum diffuse_reconstruct_variant_t
{
  DIFFUSE_RECONSTRUCT_RGB = 0,
  DIFFUSE_RECONSTRUCT_CHROMA
} diffuse_reconstruct_variant_t;


enum wavelets_scale_t
{
  ANY_SCALE   = 1 << 0, // any wavelets scale   : reconstruct += HF
  FIRST_SCALE = 1 << 1, // first wavelets scale : reconstruct = 0
  LAST_SCALE  = 1 << 2, // last wavelets scale  : reconstruct += residual
};


static inline __attribute__((always_inline)) uint8_t scale_type(const int s, const int scales)
{
  uint8_t scale = ANY_SCALE;
  if(s == 0) scale |= FIRST_SCALE;
  if(s == scales - 1) scale |= LAST_SCALE;
  return scale;
}


__DT_CLONE_TARGETS__
static inline void guide_laplacians(const float *const restrict high_freq, const float *const restrict low_freq,
                                    const float *const restrict clipping_mask,
                                    float *const restrict output, const size_t width, const size_t height,
                                    const int mult, const float noise_level, const int salt,
                                    const uint8_t scale, const float radius_sq)
{
  float *const restrict out = DT_IS_ALIGNED(output);
  const float *const restrict LF = DT_IS_ALIGNED(low_freq);
  const float *const restrict HF = DT_IS_ALIGNED(high_freq);
  const dt_aligned_pixel_simd_t zero = dt_simd_set1(0.f);
  const dt_aligned_pixel_simd_t ones = dt_simd_set1(1.f);
  const dt_aligned_pixel_simd_t inv_patch = dt_simd_set1(1.f / 9.f);
  const dt_aligned_pixel_simd_t scale_multiplier = dt_simd_set1(1.f / radius_sq);
  const float eps = 1e-12f;
  __OMP_PARALLEL_FOR__()
  for(size_t row = 0; row < height; ++row)
  {
    // interleave the order in which we process the rows so that we minimize cache misses
    const int i = dwt_interleave_rows(row, height, mult);
    const float *const row0 = HF + 4 * ((size_t)MAX(i - mult, 0) * width);
    const float *const row1 = HF + 4 * ((size_t)i * width);
    const float *const row2 = HF + 4 * ((size_t)MIN(i + mult, (int)height - 1) * width);
    const float *const rows[3] = { row0, row1, row2 };
    const int max_col = (int)width - 1;

    for(int j = 0; j < width; ++j)
    {
      const size_t idx = (i * width + j);
      const size_t index = idx * 4;
      const float alpha = clipping_mask[index + ALPHA];
      const float alpha_comp = 1.f - alpha;
      dt_aligned_pixel_simd_t high_frequency = dt_load_simd_aligned(HF + index);

      if(alpha > 0.f) // reconstruct
      {
        const int col_offsets[3]
          = { 4 * MAX(j - mult, 0),
              4 * j,
              4 * MIN(j + mult, max_col) };
        dt_aligned_pixel_simd_t sum = zero;
        dt_aligned_pixel_simd_t sum_sq = zero;
        dt_aligned_pixel_simd_t prod_r = zero;
        dt_aligned_pixel_simd_t prod_g = zero;
        dt_aligned_pixel_simd_t prod_b = zero;

        // Walk the dense 3x3 neighbourhood as counted loops so GCC keeps the
        // fit as a regular reduction instead of fully unrolling all 9 taps and
        // spilling the intermediate moments to the stack.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC unroll 1
#endif
        for(int jj = 0; jj < 3; ++jj)
        {
          const float *const row_ptr = rows[jj];
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC unroll 1
#endif
          for(int ii = 0; ii < 3; ++ii)
          {
            const dt_aligned_pixel_simd_t sample = dt_load_simd_aligned(row_ptr + col_offsets[ii]);

            sum += sample;
            sum_sq += sample * sample;
            prod_r += sample * dt_simd_set1(sample[RED]);
            prod_g += sample * dt_simd_set1(sample[GREEN]);
            prod_b += sample * dt_simd_set1(sample[BLUE]);
          }
        }

        dt_aligned_pixel_simd_t means = sum * inv_patch;
        dt_aligned_pixel_simd_t variance = sum_sq * inv_patch - means * means;
        variance = dt_simd_max_zero(variance);
        variance[ALPHA] = 0.f;

        size_t guiding_channel = RED;
        float guide_variance = variance[RED];
        if(variance[GREEN] > guide_variance)
        {
          guiding_channel = GREEN;
          guide_variance = variance[GREEN];
        }
        if(variance[BLUE] > guide_variance)
        {
          guiding_channel = BLUE;
          guide_variance = variance[BLUE];
        }

        if(guide_variance > eps)
        {
          const float guide_mean = means[guiding_channel];
          dt_aligned_pixel_simd_t covariance
            = (guiding_channel == RED ? prod_r : (guiding_channel == GREEN ? prod_g : prod_b)) * inv_patch
              - means * dt_simd_set1(guide_mean);
          dt_aligned_pixel_simd_t slope = covariance / dt_simd_set1(guide_variance);
          slope = dt_simd_max_zero(slope);
          dt_aligned_pixel_simd_t intercept = means - slope * dt_simd_set1(guide_mean);
          const dt_aligned_pixel_simd_t blend = dt_load_simd_aligned(clipping_mask + index) * scale_multiplier;
          const dt_aligned_pixel_simd_t guide = dt_simd_set1(high_frequency[guiding_channel]);
          high_frequency = blend * (slope * guide + intercept) + (ones - blend) * high_frequency;
        }
      }

      dt_aligned_pixel_simd_t out_pixel = high_frequency;
      if((scale & FIRST_SCALE))
      {
        // out is not inited yet
      }
      else
      {
        // just accumulate HF
        out_pixel += dt_load_simd_aligned(out + index);
      }

      if((scale & LAST_SCALE))
      {
        // add the residual and clamp
        out_pixel = dt_simd_max_zero(out_pixel + dt_load_simd_aligned(LF + index));
      }

      // Last step of RGB reconstruct : add noise
      if((scale & LAST_SCALE) && salt && alpha > 0.f)
      {
        // Init random number generator
        uint32_t DT_ALIGNED_ARRAY state[4] = { splitmix32(j + 1), splitmix32((j + 1) * (i + 3)), splitmix32(1337), splitmix32(666) };
        xoshiro128plus(state);
        xoshiro128plus(state);
        xoshiro128plus(state);
        xoshiro128plus(state);

        dt_aligned_pixel_t noise = { 0.f };
        dt_aligned_pixel_t sigma = { 0.20f };
        const int DT_ALIGNED_ARRAY flip[4] = { TRUE, FALSE, TRUE, FALSE };

        sigma[RED] = out_pixel[RED] * noise_level;
        sigma[GREEN] = out_pixel[GREEN] * noise_level;
        sigma[BLUE] = out_pixel[BLUE] * noise_level;
        sigma[ALPHA] = out_pixel[ALPHA] * noise_level;

        // create statistical noise
        dt_aligned_pixel_t current = { out_pixel[RED], out_pixel[GREEN], out_pixel[BLUE], out_pixel[ALPHA] };
        dt_noise_generator_simd(DT_NOISE_POISSONIAN, current, sigma, flip, state, noise);

        // Save the noisy interpolated image
        for_each_channel(c, aligned(noise, current))
          noise[c] = current[c] + fabsf(noise[c] - current[c]);

        out_pixel[RED] = fmaxf(alpha * noise[RED] + alpha_comp * current[RED], 0.f);
        out_pixel[GREEN] = fmaxf(alpha * noise[GREEN] + alpha_comp * current[GREEN], 0.f);
        out_pixel[BLUE] = fmaxf(alpha * noise[BLUE] + alpha_comp * current[BLUE], 0.f);
        out_pixel[ALPHA] = fmaxf(alpha * noise[ALPHA] + alpha_comp * current[ALPHA], 0.f);
      }

      if((scale & LAST_SCALE))
      {
        // Break the RGB channels into ratios/norm for the next step of reconstruction
        const float norm = fmaxf(sqrtf(sqf(out_pixel[RED]) + sqf(out_pixel[GREEN]) + sqf(out_pixel[BLUE])), 1e-6f);
        out_pixel /= dt_simd_set1(norm);
        out_pixel[ALPHA] = norm;
      }

      dt_store_simd_aligned(out + index, out_pixel);
    }
  }
}

__DT_CLONE_TARGETS__
static inline void heat_PDE_diffusion(const float *const restrict high_freq, const float *const restrict low_freq,
                                      const float *const restrict clipping_mask,
                                      float *const restrict output, const size_t width, const size_t height,
                                      const int mult, const uint8_t scale,
                                      const float first_order_factor)
{
  // Simultaneous inpainting for image structure and texture using anisotropic heat transfer model
  // https://www.researchgate.net/publication/220663968
  // modified as follow :
  //  * apply it in a multi-scale wavelet setup : we basically solve it twice, on the wavelets LF and HF layers.
  //  * replace the manual texture direction/distance selection by an automatic detection similar to the structure one,
  //  * generalize the framework for isotropic diffusion and anisotropic weighted on the isophote direction
  //  * add a variance regularization to better avoid edges.
  // The sharpness setting mimics the contrast equalizer effect by simply multiplying the HF by some gain.

  float *const restrict out = DT_IS_ALIGNED(output);
  const float *const restrict LF = DT_IS_ALIGNED(low_freq);
  const float *const restrict HF = DT_IS_ALIGNED(high_freq);
  __OMP_PARALLEL_FOR__(firstprivate(anisotropic_kernel_isophote))
  for(size_t row = 0; row < height; ++row)
  {
    // interleave the order in which we process the rows so that we minimize cache misses
    const size_t i = dwt_interleave_rows(row, height, mult);
    // compute the 'above' and 'below' coordinates, clamping them to the image, once for the entire row
    const size_t i_neighbours[3]
      = { MAX((int)(i - mult), (int)0) * width,            // x - mult
          i * width,                                       // x
          MIN((int)(i + mult), (int)height - 1) * width }; // x + mult
    for(size_t j = 0; j < width; ++j)
    {
      const size_t idx = (i * width + j);
      const size_t index = idx * 4;

      // fetch the clipping mask opacity : opaque (alpha = 100 %) where clipped
      const dt_aligned_pixel_t alpha = { clipping_mask[index + RED],
                                         clipping_mask[index + GREEN],
                                         clipping_mask[index + BLUE],
                                         clipping_mask[index + ALPHA] };

      dt_aligned_pixel_t high_frequency = { HF[index + 0], HF[index + 1], HF[index + 2], HF[index + 3] };

      // The for_each_channel macro uses 4 floats SIMD instructions or 3 float regular ops,
      // depending on system. Since we don't want to diffuse the norm, make sure to store and restore it later.
      // This is not much of an issue when processing image at full-res, but more harmful since
      // we reconstruct highlights on a downscaled variant
      const float norm_backup = high_frequency[3];

      if(alpha[ALPHA] > 0.f)  // reconstruct
      {
        // non-local neighbours coordinates
        const size_t j_neighbours[3]
          = { MAX((int)(j - mult), (int)0),           // y - mult
              j,                                      // y
              MIN((int)(j + mult), (int)width - 1) }; // y + mult

        // fetch non-local pixels and store them locally and contiguously
        dt_aligned_pixel_t neighbour_pixel_HF[9];
        for_four_channels(c, aligned(neighbour_pixel_HF, HF: 16))
        {
          neighbour_pixel_HF[3 * 0 + 0][c] = HF[4 * (i_neighbours[0] + j_neighbours[0]) + c];
          neighbour_pixel_HF[3 * 0 + 1][c] = HF[4 * (i_neighbours[0] + j_neighbours[1]) + c];
          neighbour_pixel_HF[3 * 0 + 2][c] = HF[4 * (i_neighbours[0] + j_neighbours[2]) + c];

          neighbour_pixel_HF[3 * 1 + 0][c] = HF[4 * (i_neighbours[1] + j_neighbours[0]) + c];
          neighbour_pixel_HF[3 * 1 + 1][c] = HF[4 * (i_neighbours[1] + j_neighbours[1]) + c];
          neighbour_pixel_HF[3 * 1 + 2][c] = HF[4 * (i_neighbours[1] + j_neighbours[2]) + c];

          neighbour_pixel_HF[3 * 2 + 0][c] = HF[4 * (i_neighbours[2] + j_neighbours[0]) + c];
          neighbour_pixel_HF[3 * 2 + 1][c] = HF[4 * (i_neighbours[2] + j_neighbours[1]) + c];
          neighbour_pixel_HF[3 * 2 + 2][c] = HF[4 * (i_neighbours[2] + j_neighbours[2]) + c];
        }

        // Compute the laplacian in the direction parallel to the steepest gradient on the norm
        // Convolve the filter to get the laplacian
        dt_aligned_pixel_t laplacian_HF = { 0.f, 0.f, 0.f, 0.f };
        for(int k = 0; k < 9; k++)
        {
          for_each_channel(c, aligned(laplacian_HF, neighbour_pixel_HF:16) aligned(anisotropic_kernel_isophote: 64))
            laplacian_HF[c] += neighbour_pixel_HF[k][c] * anisotropic_kernel_isophote[k];
        }

        // Diffuse
        const dt_aligned_pixel_t multipliers_HF = { 1.f / B_SPLINE_TO_LAPLACIAN, 1.f / B_SPLINE_TO_LAPLACIAN, 1.f / B_SPLINE_TO_LAPLACIAN, 0.f };
        for_each_channel(c, aligned(high_frequency, multipliers_HF, laplacian_HF, alpha))
          high_frequency[c] += alpha[c] * multipliers_HF[c] * (laplacian_HF[c] - first_order_factor * high_frequency[c]);

        // Restore. See above.
        high_frequency[3] = norm_backup;
      }

      if((scale & FIRST_SCALE))
      {
        // out is not inited yet
        for_each_channel(c, aligned(out, high_frequency : 64))
          out[index + c] = high_frequency[c];
      }
      else
      {
        // just accumulate HF
        for_each_channel(c, aligned(out, high_frequency : 64))
          out[index + c] += high_frequency[c];
      }

      if((scale & LAST_SCALE))
      {
        // add the residual and clamp
        for_each_channel(c, aligned(out, LF, high_frequency : 64))
          out[index + c] = fmaxf(out[index + c] + LF[index + c], 0.f);

        // renormalize ratios
        if(alpha[ALPHA] > 0.f)
        {
          const float norm = sqrtf(sqf(out[index + RED]) + sqf(out[index + GREEN]) + sqf(out[index + BLUE]));
          for_each_channel(c, aligned(out, LF, high_frequency : 64))
            out[index + c] /= (c != ALPHA && norm > 1e-4f) ? norm : 1.f;
        }

        // Last scale : reconstruct RGB from ratios and norm - norm stays in the 4th channel
        // we need it to evaluate the gradient
        for_four_channels(c, aligned(out))
          out[index + c] = (c == ALPHA) ? out[index + ALPHA] : out[index + c] * out[index + ALPHA];
      }
    }
  }
}

static inline int wavelets_process(const float *const restrict in, float
                                   *const restrict reconstructed,
                                   const float *const restrict clipping_mask,
                                   const size_t width, const size_t height,
                                   const int scales,
                                   float *const restrict HF,
                                   float *const restrict LF_odd,
                                   float *const restrict LF_even,
                                   const diffuse_reconstruct_variant_t variant,
                                   const float noise_level,
                                   const int salt, const float first_order_factor)
{
  // À trous decimated wavelet decompose
  // there is a paper from a guy we know that explains it : https://jo.dreggn.org/home/2010_atrous.pdf
  // the wavelets decomposition here is the same as the equalizer/atrous module,

  // allocate a one-row temporary buffer for the decomposition
  size_t padded_size;
  float *const tempbuf = dt_pixelpipe_cache_alloc_perthread_float(4 * width, &padded_size); //TODO: alloc in caller
  if(IS_NULL_PTR(tempbuf)) return 1;

  for(int s = 0; s < scales; ++s)
  {
    //fprintf(stderr, "CPU Wavelet decompose : scale %i\n", s);
    const int mult = 1 << s;

    const float *restrict buffer_in;
    float *restrict buffer_out;

    if(s == 0)
    {
      buffer_in = in;
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

    uint8_t current_scale_type = scale_type(s, scales);
    const float radius = sqf(equivalent_sigma_at_step(B_SPLINE_SIGMA, s * DS_FACTOR));

    if(variant == DIFFUSE_RECONSTRUCT_RGB)
      guide_laplacians(HF, buffer_out, clipping_mask, reconstructed, width, height, mult, noise_level, salt, current_scale_type, radius);
    else
      heat_PDE_diffusion(HF, buffer_out, clipping_mask, reconstructed, width, height, mult, current_scale_type, first_order_factor);

#if DEBUG_DUMP_PFM
    char name[64];
    sprintf(name, "/tmp/scale-input-%i.pfm", s);
    dump_PFM(name, buffer_in, width, height);

    sprintf(name, "/tmp/scale-blur-%i.pfm", s);
    dump_PFM(name, buffer_out, width, height);
#endif
  }
  dt_pixelpipe_cache_free_align(tempbuf);

  return 0;
}


__DT_CLONE_TARGETS__
static int process_laplacian_bayer(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                                   const dt_dev_pixelpipe_iop_t *piece, const void *const restrict ivoid,
                                   void *const restrict ovoid,
                                   const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                                   const dt_aligned_pixel_t clips)
{
  dt_iop_highlights_data_t *data = (dt_iop_highlights_data_t *)piece->data;
  int err = 0;

  const uint32_t filters = piece->dsc_in.filters;

  const size_t height = roi_in->height;
  const size_t width = roi_in->width;
  const size_t size = roi_in->width * roi_in->height;

  const size_t ds_height = height / DS_FACTOR;
  const size_t ds_width = width / DS_FACTOR;
  const size_t ds_size = ds_height * ds_width;

  float *const restrict interpolated = dt_pixelpipe_cache_alloc_align_float(size * 4, pipe);  // [R, G, B, norm] for each pixel
  float *const restrict clipping_mask = dt_pixelpipe_cache_alloc_align_float(size * 4, pipe); // [R, G, B, norm] for each pixel

  // temp buffer for blurs. We will need to cycle between them for memory efficiency
  float *const restrict LF_odd = dt_pixelpipe_cache_alloc_align_float(ds_size * 4, pipe);
  float *const restrict LF_even = dt_pixelpipe_cache_alloc_align_float(ds_size * 4, pipe);
  float *const restrict temp = dt_pixelpipe_cache_alloc_align_float(ds_size * 4, pipe);

  const float scale = fmaxf(DS_FACTOR * dt_dev_get_module_scale(pipe, roi_in), 1.f);
  const float final_radius = (float)((int)(1 << data->scales)) / scale;
  const int scales = CLAMP((int)ceilf(log2f(final_radius)), 1, MAX_NUM_SCALES);

  const float noise_level = data->noise_level / scale;

  // wavelets scales buffers
  float *restrict HF = dt_pixelpipe_cache_alloc_align_float(ds_size * 4, pipe);
  float *restrict ds_interpolated = dt_pixelpipe_cache_alloc_align_float(ds_size * 4, pipe);
  float *restrict ds_clipping_mask = dt_pixelpipe_cache_alloc_align_float(ds_size * 4, pipe);

  if(!interpolated || !clipping_mask || !LF_odd || !LF_even || !temp || !HF || !ds_interpolated || !ds_clipping_mask)
  {
    err = 1;
    goto error;
  }

  const float *const restrict input = (const float *const restrict)ivoid;
  float *const restrict output = (float *const restrict)ovoid;
  dt_aligned_pixel_t normalization = { 1.f, 1.f, 1.f, 1.f };
  _compute_laplacian_normalization(input, roi_in, filters, NULL, normalization);

  _interpolate_and_mask(input, interpolated, clipping_mask, clips, normalization, filters, width, height);
  if(dt_box_mean(clipping_mask, height, width, 4, 2, 1) != 0)
  {
    err = 1;
    goto error;
  }

  // Downsample
  interpolate_bilinear(clipping_mask, width, height, ds_clipping_mask, ds_width, ds_height, 4);
  interpolate_bilinear(interpolated, width, height, ds_interpolated, ds_width, ds_height, 4);

  for(int i = 0; i < data->iterations; i++)
  {
    const int salt = (i == data->iterations - 1); // add noise on the last iteration only
    if(wavelets_process(ds_interpolated, temp, ds_clipping_mask, ds_width, ds_height, scales, HF, LF_odd,
                        LF_even, DIFFUSE_RECONSTRUCT_RGB, noise_level, salt, data->solid_color))
    {
      err = 1;
      goto error;
    }
    if(wavelets_process(temp, ds_interpolated, ds_clipping_mask, ds_width, ds_height, scales, HF, LF_odd,
                        LF_even, DIFFUSE_RECONSTRUCT_CHROMA, noise_level, salt, data->solid_color))
    {
      err = 1;
      goto error;
    }
  }

  // Upsample
  interpolate_bilinear(ds_interpolated, ds_width, ds_height, interpolated, width, height, 4);
  _remosaic_and_replace(input, interpolated, clipping_mask, output, normalization, filters, width, height);

#if DEBUG_DUMP_PFM
  dump_PFM("/tmp/interpolated.pfm", interpolated, width, height);
  dump_PFM("/tmp/clipping_mask.pfm", clipping_mask, width, height);
#endif

error:;
  dt_pixelpipe_cache_free_align(interpolated);
  dt_pixelpipe_cache_free_align(clipping_mask);
  dt_pixelpipe_cache_free_align(temp);
  dt_pixelpipe_cache_free_align(LF_even);
  dt_pixelpipe_cache_free_align(LF_odd);
  dt_pixelpipe_cache_free_align(HF);
  dt_pixelpipe_cache_free_align(ds_interpolated);
  dt_pixelpipe_cache_free_align(ds_clipping_mask);
  return err;
}

__DT_CLONE_TARGETS__
static int process_laplacian_xtrans(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                                    const dt_dev_pixelpipe_iop_t *piece, const void *const restrict ivoid,
                                    void *const restrict ovoid,
                                    const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                                    const dt_aligned_pixel_t clips)
{
  dt_iop_highlights_data_t *data = (dt_iop_highlights_data_t *)piece->data;
  int err = 0;
  (void)roi_out;

  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->dsc_in.xtrans;

  const size_t height = roi_in->height;
  const size_t width = roi_in->width;
  const size_t size = roi_in->width * roi_in->height;

  const size_t ds_height = height / DS_FACTOR;
  const size_t ds_width = width / DS_FACTOR;
  const size_t ds_size = ds_height * ds_width;

  float *const restrict interpolated = dt_pixelpipe_cache_alloc_align_float(size * 4, pipe);
  float *const restrict clipping_mask = dt_pixelpipe_cache_alloc_align_float(size * 4, pipe);
  float *const restrict LF_odd = dt_pixelpipe_cache_alloc_align_float(ds_size * 4, pipe);
  float *const restrict LF_even = dt_pixelpipe_cache_alloc_align_float(ds_size * 4, pipe);
  float *const restrict temp = dt_pixelpipe_cache_alloc_align_float(ds_size * 4, pipe);

  const float scale = fmaxf(DS_FACTOR * dt_dev_get_module_scale(pipe, roi_in), 1.f);
  const float final_radius = (float)((int)(1 << data->scales)) / scale;
  const int scales = CLAMP((int)ceilf(log2f(final_radius)), 1, MAX_NUM_SCALES);
  const float noise_level = data->noise_level / scale;

  float *restrict HF = dt_pixelpipe_cache_alloc_align_float(ds_size * 4, pipe);
  float *restrict ds_interpolated = dt_pixelpipe_cache_alloc_align_float(ds_size * 4, pipe);
  float *restrict ds_clipping_mask = dt_pixelpipe_cache_alloc_align_float(ds_size * 4, pipe);

  if(!interpolated || !clipping_mask || !LF_odd || !LF_even || !temp || !HF || !ds_interpolated || !ds_clipping_mask)
  {
    err = 1;
    goto error;
  }

  const float *const restrict input = (const float *const restrict)ivoid;
  float *const restrict output = (float *const restrict)ovoid;
  dt_aligned_pixel_t normalization = { 1.f, 1.f, 1.f, 1.f };
  int32_t lookup[6][6][32] = { { { 0 } } };

  _compute_laplacian_normalization(input, roi_in, 9u, xtrans, normalization);
  _build_xtrans_bilinear_lookup(lookup, roi_in, xtrans);
  _interpolate_and_mask_xtrans(input, interpolated, clipping_mask, clips, normalization, roi_in, lookup, xtrans, width, height);
  if(dt_box_mean(clipping_mask, height, width, 4, 2, 1) != 0)
  {
    err = 1;
    goto error;
  }

  interpolate_bilinear(clipping_mask, width, height, ds_clipping_mask, ds_width, ds_height, 4);
  interpolate_bilinear(interpolated, width, height, ds_interpolated, ds_width, ds_height, 4);

  for(int i = 0; i < data->iterations; i++)
  {
    const int salt = (i == data->iterations - 1);
    if(wavelets_process(ds_interpolated, temp, ds_clipping_mask, ds_width, ds_height, scales, HF, LF_odd,
                        LF_even, DIFFUSE_RECONSTRUCT_RGB, noise_level, salt, data->solid_color))
    {
      err = 1;
      goto error;
    }
    if(wavelets_process(temp, ds_interpolated, ds_clipping_mask, ds_width, ds_height, scales, HF, LF_odd,
                        LF_even, DIFFUSE_RECONSTRUCT_CHROMA, noise_level, salt, data->solid_color))
    {
      err = 1;
      goto error;
    }
  }

  interpolate_bilinear(ds_interpolated, ds_width, ds_height, interpolated, width, height, 4);
  _remosaic_and_replace_xtrans(input, interpolated, clipping_mask, output, normalization, roi_in, xtrans, width, height);

error:
  dt_pixelpipe_cache_free_align(interpolated);
  dt_pixelpipe_cache_free_align(clipping_mask);
  dt_pixelpipe_cache_free_align(temp);
  dt_pixelpipe_cache_free_align(LF_even);
  dt_pixelpipe_cache_free_align(LF_odd);
  dt_pixelpipe_cache_free_align(HF);
  dt_pixelpipe_cache_free_align(ds_interpolated);
  dt_pixelpipe_cache_free_align(ds_clipping_mask);
  return err;
}

#ifdef HAVE_OPENCL
static inline cl_int wavelets_process_cl(const int devid,
                                         cl_mem in, cl_mem reconstructed,
                                         cl_mem reconstructed_scratch,
                                         cl_mem clipping_mask,
                                         const size_t sizes[3], const int width, const int height,
                                         dt_iop_highlights_global_data_t *const gd,
                                         const int scales,
                                         cl_mem HF,
                                         cl_mem LF_odd,
                                         cl_mem LF_even,
                                         const diffuse_reconstruct_variant_t variant,
                                         const float noise_level,
                                         const int salt, const float solid_color)
{
  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  cl_mem reconstruct_read = reconstructed_scratch;

  // À trous wavelet decompose
  // there is a paper from a guy we know that explains it : https://jo.dreggn.org/home/2010_atrous.pdf
  // the wavelets decomposition here is the same as the equalizer/atrous module,
  for(int s = 0; s < scales; ++s)
  {
    //fprintf(stderr, "GPU Wavelet decompose : scale %i\n", s);
    const int mult = 1 << s;

    cl_mem buffer_in;
    cl_mem buffer_out;

    if(s == 0)
    {
      buffer_in = in;
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

    // Compute wavelets low-frequency scales
    const int clamp_lf = 1;
    int hblocksize;
    dt_opencl_local_buffer_t hlocopt = (dt_opencl_local_buffer_t){ .xoffset = 2 * mult, .xfactor = 1,
                                                                    .yoffset = 0, .yfactor = 1,
                                                                    .cellsize = 4 * sizeof(float), .overhead = 0,
                                                                    .sizex = 1 << 16, .sizey = 1 };
    if(dt_opencl_local_buffer_opt(devid, gd->kernel_filmic_bspline_horizontal_local, &hlocopt))
      hblocksize = hlocopt.sizex;
    else
      hblocksize = 1;

    if(hblocksize > 1)
    {
      const size_t horizontal_sizes[3] = { ROUNDUP(width, hblocksize), ROUNDUPDHT(height, devid), 1 };
      const size_t horizontal_local[3] = { hblocksize, 1, 1 };
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal_local, 0, sizeof(cl_mem), (void *)&buffer_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal_local, 1, sizeof(cl_mem), (void *)&HF);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal_local, 2, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal_local, 3, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal_local, 4, sizeof(int), (void *)&mult);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal_local, 5, sizeof(int), (void *)&clamp_lf);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal_local, 6,
                               (hblocksize + 4 * mult) * 4 * sizeof(float), NULL);
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_filmic_bspline_horizontal_local,
                                                   horizontal_sizes, horizontal_local);
    }
    else
    {
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal, 0, sizeof(cl_mem), (void *)&buffer_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal, 1, sizeof(cl_mem), (void *)&HF);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal, 2, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal, 3, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal, 4, sizeof(int), (void *)&mult);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_horizontal, 5, sizeof(int), (void *)&clamp_lf);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_filmic_bspline_horizontal, sizes);
    }
    if(err != CL_SUCCESS) return err;

    int vblocksize;
    dt_opencl_local_buffer_t vlocopt = (dt_opencl_local_buffer_t){ .xoffset = 0, .xfactor = 1,
                                                                    .yoffset = 2 * mult, .yfactor = 1,
                                                                    .cellsize = 4 * sizeof(float), .overhead = 0,
                                                                    .sizex = 1, .sizey = 1 << 16 };
    if(dt_opencl_local_buffer_opt(devid, gd->kernel_filmic_bspline_vertical_local, &vlocopt))
      vblocksize = vlocopt.sizey;
    else
      vblocksize = 1;

    if(vblocksize > 1)
    {
      const size_t vertical_sizes[3] = { ROUNDUPDWD(width, devid), ROUNDUP(height, vblocksize), 1 };
      const size_t vertical_local[3] = { 1, vblocksize, 1 };
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical_local, 0, sizeof(cl_mem), (void *)&HF);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical_local, 1, sizeof(cl_mem), (void *)&buffer_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical_local, 2, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical_local, 3, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical_local, 4, sizeof(int), (void *)&mult);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical_local, 5, sizeof(int), (void *)&clamp_lf);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical_local, 6,
                               (vblocksize + 4 * mult) * 4 * sizeof(float), NULL);
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_filmic_bspline_vertical_local,
                                                   vertical_sizes, vertical_local);
    }
    else
    {
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical, 0, sizeof(cl_mem), (void *)&HF);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical, 1, sizeof(cl_mem), (void *)&buffer_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical, 2, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical, 3, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical, 4, sizeof(int), (void *)&mult);
      dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_bspline_vertical, 5, sizeof(int), (void *)&clamp_lf);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_filmic_bspline_vertical, sizes);
    }
    if(err != CL_SUCCESS) return err;

    uint8_t current_scale_type = scale_type(s, scales);
    const float radius = sqf(equivalent_sigma_at_step(B_SPLINE_SIGMA, s * DS_FACTOR));
    cl_mem reconstruct_write = (s == scales - 1)
                                 ? reconstructed
                                 : (reconstruct_read == reconstructed ? reconstructed_scratch : reconstructed);

    // Keep the accumulation image read/write handles distinct at each scale.
    // Some AMD OpenCL drivers get unstable when the same image is bound for both roles.
    if(variant == DIFFUSE_RECONSTRUCT_RGB)
    {
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_guide_laplacians, 0, sizeof(cl_mem), (void *)&buffer_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_guide_laplacians, 1, sizeof(cl_mem), (void *)&buffer_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_guide_laplacians, 2, sizeof(cl_mem), (void *)&clipping_mask);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_guide_laplacians, 3, sizeof(cl_mem), (void *)&reconstruct_read);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_guide_laplacians, 4, sizeof(cl_mem), (void *)&reconstruct_write);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_guide_laplacians, 5, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_guide_laplacians, 6, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_guide_laplacians, 7, sizeof(int), (void *)&mult);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_guide_laplacians, 8, sizeof(float), (void *)&noise_level);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_guide_laplacians, 9, sizeof(int), (void *)&salt);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_guide_laplacians, 10, sizeof(uint8_t), (void *)&current_scale_type);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_guide_laplacians, 11, sizeof(float), (void *)&radius);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_guide_laplacians, sizes);
      if(err != CL_SUCCESS) return err;
    }
    else // DIFFUSE_RECONSTRUCT_CHROMA
    {
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_diffuse_color, 0, sizeof(cl_mem), (void *)&buffer_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_diffuse_color, 1, sizeof(cl_mem), (void *)&buffer_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_diffuse_color, 2, sizeof(cl_mem), (void *)&clipping_mask);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_diffuse_color, 3, sizeof(cl_mem), (void *)&reconstruct_read);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_diffuse_color, 4, sizeof(cl_mem), (void *)&reconstruct_write);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_diffuse_color, 5, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_diffuse_color, 6, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_diffuse_color, 7, sizeof(int), (void *)&mult);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_diffuse_color, 8, sizeof(uint8_t), (void *)&current_scale_type);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_diffuse_color, 9, sizeof(float), (void *)&solid_color);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_diffuse_color, sizes);
      if(err != CL_SUCCESS) return err;
    }

    reconstruct_read = reconstruct_write;
  }

  return err;
}

static cl_int process_laplacian_bayer_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                                         const dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
                                         const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                                         const dt_aligned_pixel_t clips)
{
  dt_iop_highlights_data_t *data = (dt_iop_highlights_data_t *)piece->data;
  dt_iop_highlights_global_data_t *gd = (dt_iop_highlights_global_data_t *)self->global_data;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  const int devid = pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  const int ds_height = height / DS_FACTOR;
  const int ds_width = width / DS_FACTOR;

  size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
  size_t ds_sizes[] = { ROUNDUPDWD(ds_width, devid), ROUNDUPDHT(ds_height, devid), 1 };

  const uint32_t filters = piece->dsc_in.filters;

  cl_mem interpolated = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);  // [R, G, B, norm] for each pixel
  cl_mem clipping_mask = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4); // [R, G, B, norm] for each pixel
  cl_mem normalization = NULL;
  cl_mem normalization_tmp = NULL;
  cl_mem normalization_partials = NULL;
  cl_mem normalization_final = NULL;

  // temp buffer for blurs. We will need to cycle between them for memory efficiency
  cl_mem LF_odd = dt_opencl_alloc_device(devid, ds_sizes[0], ds_sizes[1], sizeof(float) * 4);
  cl_mem LF_even = dt_opencl_alloc_device(devid, ds_sizes[0], ds_sizes[1], sizeof(float) * 4);
  cl_mem temp = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4); // need full size here for blurring

  const float scale = fmaxf(DS_FACTOR * dt_dev_get_module_scale(pipe, roi_in), 1.f);
  const float final_radius = (float)((int)(1 << data->scales)) / scale;
  const int scales = CLAMP((int)ceilf(log2f(final_radius)), 1, MAX_NUM_SCALES);

  const float noise_level = data->noise_level / scale;

  // wavelets scales buffers
  cl_mem HF = dt_opencl_alloc_device(devid, ds_sizes[0], ds_sizes[1], sizeof(float) * 4);
  cl_mem ds_interpolated = dt_opencl_alloc_device(devid, ds_sizes[0], ds_sizes[1], sizeof(float) * 4);
  cl_mem ds_clipping_mask = dt_opencl_alloc_device(devid, ds_sizes[0], ds_sizes[1], sizeof(float) * 4);
  cl_mem reconstructed_scratch = dt_opencl_alloc_device(devid, ds_sizes[0], ds_sizes[1], sizeof(float) * 4);
  cl_mem clips_cl = dt_opencl_copy_host_to_device_constant(devid, 4 * sizeof(float), (float*)clips);

  if(!interpolated || !clipping_mask || !LF_odd || !LF_even || !temp || !HF || !ds_interpolated
     || !ds_clipping_mask || !reconstructed_scratch || !clips_cl)
    goto error;

  {
    dt_opencl_local_buffer_t flocopt
      = (dt_opencl_local_buffer_t){ .xoffset = 0, .xfactor = 1, .yoffset = 0, .yfactor = 1,
                                    .cellsize = 4 * sizeof(float), .overhead = 0,
                                    .sizex = 1 << 4, .sizey = 1 << 4 };

    if(!dt_opencl_local_buffer_opt(devid, gd->kernel_highlights_normalize_reduce_first, &flocopt))
      goto error;

    const size_t bwidth = ROUNDUP(width, flocopt.sizex);
    const size_t bheight = ROUNDUP(height, flocopt.sizey);
    const int bufsize = (int)((bwidth / flocopt.sizex) * (bheight / flocopt.sizey));

    normalization_partials = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 4 * (size_t)bufsize);
    normalization = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 4 * REDUCESIZE);
    normalization_tmp = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 4 * REDUCESIZE);
    if(!normalization_partials || !normalization || !normalization_tmp) goto error;

    size_t fsizes[3] = { bwidth, bheight, 1 };
    size_t flocal[3] = { flocopt.sizex, flocopt.sizey, 1 };
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_first, 0, sizeof(cl_mem), &dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_first, 1, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_first, 2, sizeof(int), &height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_first, 3, sizeof(cl_mem), &normalization_partials);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_first, 4, sizeof(uint32_t), &filters);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_first, 5, sizeof(int), &roi_in->x);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_first, 6, sizeof(int), &roi_in->y);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_first, 7,
                             sizeof(float) * 4 * flocopt.sizex * flocopt.sizey, NULL);
    err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_highlights_normalize_reduce_first, fsizes, flocal);
    if(err != CL_SUCCESS) goto error;

    dt_opencl_local_buffer_t slocopt
      = (dt_opencl_local_buffer_t){ .xoffset = 0, .xfactor = 1, .yoffset = 0, .yfactor = 1,
                                    .cellsize = 4 * sizeof(float), .overhead = 0,
                                    .sizex = 1 << 16, .sizey = 1 };

    if(!dt_opencl_local_buffer_opt(devid, gd->kernel_highlights_normalize_reduce_second, &slocopt))
      goto error;

    int current_length = bufsize;
    cl_mem reduce_in = normalization_partials;
    cl_mem reduce_out = normalization;

    while(TRUE)
    {
      const int reducesize = MIN(REDUCESIZE, ROUNDUP(current_length, slocopt.sizex) / slocopt.sizex);
      size_t ssizes[3] = { (size_t)reducesize * slocopt.sizex, 1, 1 };
      size_t slocal[3] = { slocopt.sizex, 1, 1 };
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_second, 0, sizeof(cl_mem), &reduce_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_second, 1, sizeof(cl_mem), &reduce_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_second, 2, sizeof(int), &current_length);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_second, 3,
                               sizeof(float) * 4 * slocopt.sizex, NULL);
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_highlights_normalize_reduce_second, ssizes, slocal);
      if(err != CL_SUCCESS) goto error;

      if(reducesize == 1) break;
      current_length = reducesize;
      cl_mem swap = reduce_in;
      reduce_in = reduce_out;
      reduce_out = (swap == normalization_partials) ? normalization_tmp : normalization;
    }

    normalization_final = reduce_out;
  }

  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask, 1, sizeof(cl_mem), (void *)&interpolated);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask, 2, sizeof(cl_mem), (void *)&temp);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask, 3, sizeof(cl_mem), (void *)&clips_cl);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask, 4, sizeof(cl_mem), (void *)&normalization_final);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask, 5, sizeof(int), (void *)&filters);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask, 6, sizeof(int), (void *)&roi_out->width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask, 7, sizeof(int),
                           (void *)&roi_out->height);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_bilinear_and_mask, sizes);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_box_blur, 0, sizeof(cl_mem), (void *)&temp);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_box_blur, 1, sizeof(cl_mem), (void *)&clipping_mask);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_box_blur, 2, sizeof(int), (void *)&roi_out->width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_box_blur, 3, sizeof(int), (void *)&roi_out->height);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_box_blur, sizes);
  if(err != CL_SUCCESS) goto error;

  // Downsample
  const int RGBa = TRUE;
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 0, sizeof(cl_mem), (void *)&clipping_mask);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 1, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 2, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 3, sizeof(cl_mem), (void *)&ds_clipping_mask);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 4, sizeof(int), (void *)&ds_width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 5, sizeof(int), (void *)&ds_height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 6, sizeof(int), (void *)&RGBa);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_interpolate_bilinear, ds_sizes);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 0, sizeof(cl_mem), (void *)&interpolated);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 1, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 2, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 3, sizeof(cl_mem), (void *)&ds_interpolated);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 4, sizeof(int), (void *)&ds_width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 5, sizeof(int), (void *)&ds_height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 6, sizeof(int), (void *)&RGBa);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_interpolate_bilinear, ds_sizes);
  if(err != CL_SUCCESS) goto error;

  for(int i = 0; i < data->iterations; i++)
  {
    const int salt = (i == data->iterations - 1); // add noise on the last iteration only
    err = wavelets_process_cl(devid, ds_interpolated, temp, reconstructed_scratch, ds_clipping_mask,
                              ds_sizes, ds_width, ds_height, gd, scales, HF, LF_odd, LF_even,
                              DIFFUSE_RECONSTRUCT_RGB, noise_level, salt, data->solid_color);
    if(err != CL_SUCCESS) goto error;

    err = wavelets_process_cl(devid, temp, ds_interpolated, reconstructed_scratch, ds_clipping_mask,
                              ds_sizes, ds_width, ds_height, gd, scales, HF, LF_odd, LF_even,
                              DIFFUSE_RECONSTRUCT_CHROMA, noise_level, salt, data->solid_color);
    if(err != CL_SUCCESS) goto error;
  }

  // Upsample
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 0, sizeof(cl_mem), (void *)&ds_interpolated);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 1, sizeof(int), (void *)&ds_width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 2, sizeof(int), (void *)&ds_height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 3, sizeof(cl_mem), (void *)&interpolated);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 4, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 5, sizeof(int), (void *)&height);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_interpolate_bilinear, sizes);
  if(err != CL_SUCCESS) goto error;

  // Remosaic
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace, 1, sizeof(cl_mem), (void *)&interpolated);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace, 2, sizeof(cl_mem), (void *)&clipping_mask);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace, 3, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace, 4, sizeof(cl_mem), (void *)&normalization_final);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace, 5, sizeof(int), (void *)&filters);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace, 6, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace, 7, sizeof(int), (void *)&height);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_remosaic_and_replace, sizes);
  if(err != CL_SUCCESS) goto error;

  // cleanup and exit on success
  dt_opencl_release_mem_object(clips_cl);
  dt_opencl_release_mem_object(normalization_partials);
  if(normalization_tmp != normalization_final) dt_opencl_release_mem_object(normalization_tmp);
  if(normalization != normalization_final) dt_opencl_release_mem_object(normalization);
  dt_opencl_release_mem_object(normalization_final);
  dt_opencl_release_mem_object(interpolated);
  dt_opencl_release_mem_object(clipping_mask);
  dt_opencl_release_mem_object(temp);
  dt_opencl_release_mem_object(LF_even);
  dt_opencl_release_mem_object(LF_odd);
  dt_opencl_release_mem_object(HF);
  dt_opencl_release_mem_object(ds_clipping_mask);
  dt_opencl_release_mem_object(ds_interpolated);
  dt_opencl_release_mem_object(reconstructed_scratch);
  return err;

error:
  dt_opencl_release_mem_object(clips_cl);
  dt_opencl_release_mem_object(normalization_partials);
  if(normalization_tmp != normalization_final) dt_opencl_release_mem_object(normalization_tmp);
  if(normalization != normalization_final) dt_opencl_release_mem_object(normalization);
  dt_opencl_release_mem_object(normalization_final);
  dt_opencl_release_mem_object(interpolated);
  dt_opencl_release_mem_object(clipping_mask);
  dt_opencl_release_mem_object(temp);
  dt_opencl_release_mem_object(LF_even);
  dt_opencl_release_mem_object(LF_odd);
  dt_opencl_release_mem_object(HF);
  dt_opencl_release_mem_object(ds_clipping_mask);
  dt_opencl_release_mem_object(ds_interpolated);
  dt_opencl_release_mem_object(reconstructed_scratch);

  dt_print(DT_DEBUG_OPENCL, "[opencl_highlights] couldn't enqueue kernel! %i\n", err);
  return err;
}

static cl_int process_laplacian_xtrans_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                                          const dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
                                          const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                                          const dt_aligned_pixel_t clips)
{
  dt_iop_highlights_data_t *data = (dt_iop_highlights_data_t *)piece->data;
  dt_iop_highlights_global_data_t *gd = (dt_iop_highlights_global_data_t *)self->global_data;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->dsc_in.xtrans;
  const int devid = pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  const int ds_height = height / DS_FACTOR;
  const int ds_width = width / DS_FACTOR;

  size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
  size_t ds_sizes[] = { ROUNDUPDWD(ds_width, devid), ROUNDUPDHT(ds_height, devid), 1 };

  cl_mem interpolated = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);
  cl_mem clipping_mask = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);
  cl_mem normalization = NULL;
  cl_mem normalization_tmp = NULL;
  cl_mem normalization_partials = NULL;
  cl_mem normalization_final = NULL;
  cl_mem LF_odd = dt_opencl_alloc_device(devid, ds_sizes[0], ds_sizes[1], sizeof(float) * 4);
  cl_mem LF_even = dt_opencl_alloc_device(devid, ds_sizes[0], ds_sizes[1], sizeof(float) * 4);
  cl_mem temp = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);

  const float scale = fmaxf(DS_FACTOR * dt_dev_get_module_scale(pipe, roi_in), 1.f);
  const float final_radius = (float)((int)(1 << data->scales)) / scale;
  const int scales = CLAMP((int)ceilf(log2f(final_radius)), 1, MAX_NUM_SCALES);
  const float noise_level = data->noise_level / scale;

  cl_mem HF = dt_opencl_alloc_device(devid, ds_sizes[0], ds_sizes[1], sizeof(float) * 4);
  cl_mem ds_interpolated = dt_opencl_alloc_device(devid, ds_sizes[0], ds_sizes[1], sizeof(float) * 4);
  cl_mem ds_clipping_mask = dt_opencl_alloc_device(devid, ds_sizes[0], ds_sizes[1], sizeof(float) * 4);
  cl_mem reconstructed_scratch = dt_opencl_alloc_device(devid, ds_sizes[0], ds_sizes[1], sizeof(float) * 4);

  cl_mem clips_cl = dt_opencl_copy_host_to_device_constant(devid, 4 * sizeof(float), (float *)clips);
  cl_mem dev_xtrans = dt_opencl_copy_host_to_device_constant(devid, sizeof(piece->dsc_in.xtrans), (void *)piece->dsc_in.xtrans);
  int32_t lookup[6][6][32] = { { { 0 } } };
  _build_xtrans_bilinear_lookup(lookup, roi_in, xtrans);
  cl_mem lookup_cl = dt_opencl_copy_host_to_device_constant(devid, sizeof(lookup), lookup);

  if(!interpolated || !clipping_mask || !LF_odd || !LF_even || !temp || !HF || !ds_interpolated
     || !ds_clipping_mask || !reconstructed_scratch || !clips_cl || !dev_xtrans || !lookup_cl)
    goto error;

  {
    dt_opencl_local_buffer_t flocopt
      = (dt_opencl_local_buffer_t){ .xoffset = 0, .xfactor = 1, .yoffset = 0, .yfactor = 1,
                                    .cellsize = 4 * sizeof(float), .overhead = 0,
                                    .sizex = 1 << 4, .sizey = 1 << 4 };

    if(!dt_opencl_local_buffer_opt(devid, gd->kernel_highlights_normalize_reduce_first_xtrans, &flocopt))
      goto error;

    const size_t bwidth = ROUNDUP(width, flocopt.sizex);
    const size_t bheight = ROUNDUP(height, flocopt.sizey);
    const int bufsize = (int)((bwidth / flocopt.sizex) * (bheight / flocopt.sizey));

    normalization_partials = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 4 * (size_t)bufsize);
    normalization = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 4 * REDUCESIZE);
    normalization_tmp = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 4 * REDUCESIZE);
    if(!normalization_partials || !normalization || !normalization_tmp) goto error;

    size_t fsizes[3] = { bwidth, bheight, 1 };
    size_t flocal[3] = { flocopt.sizex, flocopt.sizey, 1 };
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_first_xtrans, 0, sizeof(cl_mem), &dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_first_xtrans, 1, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_first_xtrans, 2, sizeof(int), &height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_first_xtrans, 3, sizeof(cl_mem), &normalization_partials);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_first_xtrans, 4, sizeof(int), &roi_in->x);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_first_xtrans, 5, sizeof(int), &roi_in->y);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_first_xtrans, 6, sizeof(cl_mem), &dev_xtrans);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_first_xtrans, 7,
                             sizeof(float) * 4 * flocopt.sizex * flocopt.sizey, NULL);
    err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_highlights_normalize_reduce_first_xtrans, fsizes, flocal);
    if(err != CL_SUCCESS) goto error;

    dt_opencl_local_buffer_t slocopt
      = (dt_opencl_local_buffer_t){ .xoffset = 0, .xfactor = 1, .yoffset = 0, .yfactor = 1,
                                    .cellsize = 4 * sizeof(float), .overhead = 0,
                                    .sizex = 1 << 16, .sizey = 1 };

    if(!dt_opencl_local_buffer_opt(devid, gd->kernel_highlights_normalize_reduce_second, &slocopt))
      goto error;

    int current_length = bufsize;
    cl_mem reduce_in = normalization_partials;
    cl_mem reduce_out = normalization;

    while(TRUE)
    {
      const int reducesize = MIN(REDUCESIZE, ROUNDUP(current_length, slocopt.sizex) / slocopt.sizex);
      size_t ssizes[3] = { (size_t)reducesize * slocopt.sizex, 1, 1 };
      size_t slocal[3] = { slocopt.sizex, 1, 1 };
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_second, 0, sizeof(cl_mem), &reduce_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_second, 1, sizeof(cl_mem), &reduce_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_second, 2, sizeof(int), &current_length);
      dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_normalize_reduce_second, 3,
                               sizeof(float) * 4 * slocopt.sizex, NULL);
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_highlights_normalize_reduce_second, ssizes, slocal);
      if(err != CL_SUCCESS) goto error;

      if(reducesize == 1) break;
      current_length = reducesize;
      cl_mem swap = reduce_in;
      reduce_in = reduce_out;
      reduce_out = (swap == normalization_partials) ? normalization_tmp : normalization;
    }

    normalization_final = reduce_out;
  }

  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask_xtrans, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask_xtrans, 1, sizeof(cl_mem), (void *)&interpolated);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask_xtrans, 2, sizeof(cl_mem), (void *)&temp);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask_xtrans, 3, sizeof(cl_mem), (void *)&clips_cl);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask_xtrans, 4, sizeof(cl_mem), (void *)&normalization_final);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask_xtrans, 5, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask_xtrans, 6, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask_xtrans, 7, sizeof(int), (void *)&roi_in->x);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask_xtrans, 8, sizeof(int), (void *)&roi_in->y);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask_xtrans, 9, sizeof(cl_mem), (void *)&dev_xtrans);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_bilinear_and_mask_xtrans, 10, sizeof(cl_mem), (void *)&lookup_cl);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_bilinear_and_mask_xtrans, sizes);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_box_blur, 0, sizeof(cl_mem), (void *)&temp);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_box_blur, 1, sizeof(cl_mem), (void *)&clipping_mask);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_box_blur, 2, sizeof(int), (void *)&roi_out->width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_box_blur, 3, sizeof(int), (void *)&roi_out->height);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_box_blur, sizes);
  if(err != CL_SUCCESS) goto error;

  const int RGBa = TRUE;
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 0, sizeof(cl_mem), (void *)&clipping_mask);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 1, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 2, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 3, sizeof(cl_mem), (void *)&ds_clipping_mask);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 4, sizeof(int), (void *)&ds_width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 5, sizeof(int), (void *)&ds_height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 6, sizeof(int), (void *)&RGBa);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_interpolate_bilinear, ds_sizes);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 0, sizeof(cl_mem), (void *)&interpolated);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 1, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 2, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 3, sizeof(cl_mem), (void *)&ds_interpolated);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 4, sizeof(int), (void *)&ds_width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 5, sizeof(int), (void *)&ds_height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 6, sizeof(int), (void *)&RGBa);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_interpolate_bilinear, ds_sizes);
  if(err != CL_SUCCESS) goto error;

  for(int i = 0; i < data->iterations; i++)
  {
    const int salt = (i == data->iterations - 1);
    err = wavelets_process_cl(devid, ds_interpolated, temp, reconstructed_scratch, ds_clipping_mask,
                              ds_sizes, ds_width, ds_height, gd, scales, HF, LF_odd, LF_even,
                              DIFFUSE_RECONSTRUCT_RGB, noise_level, salt, data->solid_color);
    if(err != CL_SUCCESS) goto error;

    err = wavelets_process_cl(devid, temp, ds_interpolated, reconstructed_scratch, ds_clipping_mask,
                              ds_sizes, ds_width, ds_height, gd, scales, HF, LF_odd, LF_even,
                              DIFFUSE_RECONSTRUCT_CHROMA, noise_level, salt, data->solid_color);
    if(err != CL_SUCCESS) goto error;
  }

  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 0, sizeof(cl_mem), (void *)&ds_interpolated);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 1, sizeof(int), (void *)&ds_width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 2, sizeof(int), (void *)&ds_height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 3, sizeof(cl_mem), (void *)&interpolated);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 4, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_interpolate_bilinear, 5, sizeof(int), (void *)&height);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_interpolate_bilinear, sizes);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace_xtrans, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace_xtrans, 1, sizeof(cl_mem), (void *)&interpolated);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace_xtrans, 2, sizeof(cl_mem), (void *)&clipping_mask);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace_xtrans, 3, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace_xtrans, 4, sizeof(cl_mem), (void *)&normalization_final);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace_xtrans, 5, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace_xtrans, 6, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace_xtrans, 7, sizeof(int), (void *)&roi_in->x);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace_xtrans, 8, sizeof(int), (void *)&roi_in->y);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_remosaic_and_replace_xtrans, 9, sizeof(cl_mem), (void *)&dev_xtrans);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_remosaic_and_replace_xtrans, sizes);
  if(err != CL_SUCCESS) goto error;
  dt_opencl_release_mem_object(clips_cl);
  dt_opencl_release_mem_object(lookup_cl);
  dt_opencl_release_mem_object(dev_xtrans);
  dt_opencl_release_mem_object(normalization_partials);
  if(normalization_tmp != normalization_final) dt_opencl_release_mem_object(normalization_tmp);
  if(normalization != normalization_final) dt_opencl_release_mem_object(normalization);
  dt_opencl_release_mem_object(normalization_final);
  dt_opencl_release_mem_object(interpolated);
  dt_opencl_release_mem_object(clipping_mask);
  dt_opencl_release_mem_object(temp);
  dt_opencl_release_mem_object(LF_even);
  dt_opencl_release_mem_object(LF_odd);
  dt_opencl_release_mem_object(HF);
  dt_opencl_release_mem_object(ds_clipping_mask);
  dt_opencl_release_mem_object(ds_interpolated);
  dt_opencl_release_mem_object(reconstructed_scratch);
  return err;

error:
  dt_opencl_release_mem_object(clips_cl);
  dt_opencl_release_mem_object(lookup_cl);
  dt_opencl_release_mem_object(dev_xtrans);
  dt_opencl_release_mem_object(normalization_partials);
  if(normalization_tmp != normalization_final) dt_opencl_release_mem_object(normalization_tmp);
  if(normalization != normalization_final) dt_opencl_release_mem_object(normalization);
  dt_opencl_release_mem_object(normalization_final);
  dt_opencl_release_mem_object(interpolated);
  dt_opencl_release_mem_object(clipping_mask);
  dt_opencl_release_mem_object(temp);
  dt_opencl_release_mem_object(LF_even);
  dt_opencl_release_mem_object(LF_odd);
  dt_opencl_release_mem_object(HF);
  dt_opencl_release_mem_object(ds_clipping_mask);
  dt_opencl_release_mem_object(ds_interpolated);
  dt_opencl_release_mem_object(reconstructed_scratch);

  dt_print(DT_DEBUG_OPENCL, "[opencl_highlights] couldn't enqueue kernel! %i\n", err);
  return err;
}
#endif

__DT_CLONE_TARGETS__
static void process_clip(const dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
                         const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                         const float clip)
{
  const float *const in = (const float *const)ivoid;
  float *const out = (float *const)ovoid;

  if(piece->dsc_in.filters)
  { // raw mosaic
    __OMP_PARALLEL_FOR_SIMD__()
    for(size_t k = 0; k < (size_t)roi_out->width * roi_out->height; k++)
    {
      out[k] = MIN(clip, in[k]);
    }
  }
  else
  {
    const int ch = piece->dsc_in.channels;
    __OMP_PARALLEL_FOR_SIMD__()
    for(size_t k = 0; k < (size_t)ch * roi_out->width * roi_out->height; k++)
    {
      out[k] = MIN(clip, in[k]);
    }
  }
}

__DT_CLONE_TARGETS__
static void process_visualize(const dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
                         const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                         const uint32_t filters, dt_iop_highlights_data_t *data)
{
  const float *const in = (const float *const)ivoid;
  float *const out = (float *const)ovoid;
  const size_t width = roi_out->width;
  const size_t height = roi_out->height;
  const float clips[4] = { 0.995f * data->clip * piece->dsc_in.processed_maximum[0],
                           0.995f * data->clip * piece->dsc_in.processed_maximum[1],
                           0.995f * data->clip * piece->dsc_in.processed_maximum[2],
                           data->clip};
  __OMP_FOR_SIMD__(aligned(in, out : 64))
  for(size_t row = 0; row < height; row++)
  {
    for(size_t col = 0, i = row*width; col < width; col++, i++)
    {
      const int c = FC(row, col, filters);
      const float ival = in[i];
      out[i] = (ival < clips[c]) ? 0.2f * ival : 1.0f;
    }
  }
}

__DT_CLONE_TARGETS__
int process(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid)
{
  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  const uint32_t filters = piece->dsc_in.filters;
  dt_iop_highlights_data_t *data = (dt_iop_highlights_data_t *)piece->data;
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;

  const gboolean fullpipe = !dt_dev_pixelpipe_has_preview_output(self->dev, pipe, roi_out);
  const gboolean visualizing = (!IS_NULL_PTR(g)) ? g->show_visualize && fullpipe : FALSE;

  if(visualizing)
  {
    process_visualize(piece, ivoid, ovoid, roi_in, roi_out, filters, data);
    ((dt_dev_pixelpipe_t *)pipe)->mask_display = DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU;
    return 0;
  }

  const float clip
      = data->clip * fminf(piece->dsc_in.processed_maximum[0],
                           fminf(piece->dsc_in.processed_maximum[1], piece->dsc_in.processed_maximum[2]));

  if(!filters)
  {
    process_clip(piece, ivoid, ovoid, roi_in, roi_out, clip);
    return 0;
  }

  switch(data->mode)
  {
    case DT_IOP_HIGHLIGHTS_INPAINT: // a1ex's (magiclantern) idea of color inpainting:
    {
      const float clips[4] = { 0.987 * data->clip * piece->dsc_in.processed_maximum[0],
                               0.987 * data->clip * piece->dsc_in.processed_maximum[1],
                               0.987 * data->clip * piece->dsc_in.processed_maximum[2], clip };

      if(filters == 9u)
      {
        const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->dsc_in.xtrans;
        __OMP_PARALLEL_FOR__()
        for(int j = 0; j < roi_out->height; j++)
        {
          interpolate_color_xtrans(ivoid, ovoid, roi_in, roi_out, 0, 1, j, clips, xtrans, 0);
          interpolate_color_xtrans(ivoid, ovoid, roi_in, roi_out, 0, -1, j, clips, xtrans, 1);
        }
        __OMP_PARALLEL_FOR__()
        for(int i = 0; i < roi_out->width; i++)
        {
          interpolate_color_xtrans(ivoid, ovoid, roi_in, roi_out, 1, 1, i, clips, xtrans, 2);
          interpolate_color_xtrans(ivoid, ovoid, roi_in, roi_out, 1, -1, i, clips, xtrans, 3);
        }
      }
      else
      {
        __OMP_PARALLEL_FOR__()
        for(int j = 0; j < roi_out->height; j++)
        {
          interpolate_color(ivoid, ovoid, roi_out, 0, 1, j, clips, filters, 0);
          interpolate_color(ivoid, ovoid, roi_out, 0, -1, j, clips, filters, 1);
        }

// up/down directions
        __OMP_PARALLEL_FOR__()
        for(int i = 0; i < roi_out->width; i++)
        {
          interpolate_color(ivoid, ovoid, roi_out, 1, 1, i, clips, filters, 2);
          interpolate_color(ivoid, ovoid, roi_out, 1, -1, i, clips, filters, 3);
        }
      }
      break;
    }
    case DT_IOP_HIGHLIGHTS_LCH:
      if(filters == 9u)
        process_lch_xtrans(self, piece, ivoid, ovoid, roi_in, roi_out, clip);
      else
        process_lch_bayer(self, piece, ivoid, ovoid, roi_in, roi_out, clip);
      break;
    case DT_IOP_HIGHLIGHTS_LAPLACIAN:
    {
      const dt_aligned_pixel_t clips = { 0.995f * data->clip * piece->dsc_in.processed_maximum[0],
                                         0.995f * data->clip * piece->dsc_in.processed_maximum[1],
                                         0.995f * data->clip * piece->dsc_in.processed_maximum[2], clip };
      if((filters == 9u && process_laplacian_xtrans(self, pipe, piece, ivoid, ovoid, roi_in, roi_out, clips))
         || (filters != 9u && process_laplacian_bayer(self, pipe, piece, ivoid, ovoid, roi_in, roi_out, clips)))
        return 1;
      break;
    }
    default:
    case DT_IOP_HIGHLIGHTS_CLIP:
      process_clip(piece, ivoid, ovoid, roi_in, roi_out, clip);
      break;
  }

  if(pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
  return 0;
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_highlights_params_t *p = (dt_iop_highlights_params_t *)p1;
  dt_iop_highlights_data_t *d = (dt_iop_highlights_data_t *)piece->data;

  memcpy(d, p, sizeof(*p));

  // No code path for monochrome or JPEG/TIFF
  if(dt_image_is_monochrome(&self->dev->image_storage) || !dt_image_is_raw(&self->dev->image_storage))
    piece->enabled = FALSE;

  // no OpenCL for DT_IOP_HIGHLIGHTS_INPAINT
  piece->process_cl_ready = (d->mode == DT_IOP_HIGHLIGHTS_INPAINT) ? 0 : 1;

  if(d->mode == DT_IOP_HIGHLIGHTS_LAPLACIAN) 
    piece->force_opencl_cache = TRUE;

  if(d->mode != DT_IOP_HIGHLIGHTS_LAPLACIAN)
  {
    if(!piece->dsc_in.filters)
    {
      const float m = fminf(piece->dsc_in.processed_maximum[0],
                            fminf(piece->dsc_in.processed_maximum[1], piece->dsc_in.processed_maximum[2]));
      for(int k = 0; k < 3; k++) piece->dsc_out.processed_maximum[k] = m;
    }
    else
    {
      const float m = fmaxf(piece->dsc_in.processed_maximum[0],
                            fmaxf(piece->dsc_in.processed_maximum[1], piece->dsc_in.processed_maximum[2]));
      for(int k = 0; k < 3; k++) piece->dsc_out.processed_maximum[k] = m;
    }
  }
}

static gboolean enable(dt_image_t *image)
{
  return dt_image_is_raw(image) && !dt_image_is_monochrome(image);
}

gboolean force_enable(struct dt_iop_module_t *self, const gboolean current_state)
{
  // No codepath for non-raw images
  if(current_state && dt_image_is_monochrome(&self->dev->image_storage))
    return FALSE;
  else
    return current_state;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_highlights_global_data_t *gd
      = (dt_iop_highlights_global_data_t *)malloc(sizeof(dt_iop_highlights_global_data_t));
  module->data = gd;
  gd->kernel_highlights_1f_clip = dt_opencl_create_kernel(program, "highlights_1f_clip");
  gd->kernel_highlights_1f_lch_bayer = dt_opencl_create_kernel(program, "highlights_1f_lch_bayer");
  gd->kernel_highlights_1f_lch_xtrans = dt_opencl_create_kernel(program, "highlights_1f_lch_xtrans");
  gd->kernel_highlights_4f_clip = dt_opencl_create_kernel(program, "highlights_4f_clip");
  gd->kernel_highlights_bilinear_and_mask = dt_opencl_create_kernel(program, "interpolate_and_mask");
  gd->kernel_highlights_bilinear_and_mask_xtrans = dt_opencl_create_kernel(program, "interpolate_and_mask_xtrans");
  gd->kernel_highlights_normalize_reduce_first = dt_opencl_create_kernel(program, "highlights_normalize_reduce_first");
  gd->kernel_highlights_normalize_reduce_first_xtrans = dt_opencl_create_kernel(program, "highlights_normalize_reduce_first_xtrans");
  gd->kernel_highlights_normalize_reduce_second = dt_opencl_create_kernel(program, "highlights_normalize_reduce_second");
  gd->kernel_highlights_remosaic_and_replace = dt_opencl_create_kernel(program, "remosaic_and_replace");
  gd->kernel_highlights_remosaic_and_replace_xtrans = dt_opencl_create_kernel(program, "remosaic_and_replace_xtrans");
  gd->kernel_highlights_box_blur = dt_opencl_create_kernel(program, "box_blur_5x5");
  gd->kernel_highlights_guide_laplacians = dt_opencl_create_kernel(program, "guide_laplacians");
  gd->kernel_highlights_diffuse_color = dt_opencl_create_kernel(program, "diffuse_color");
  gd->kernel_highlights_false_color = dt_opencl_create_kernel(program, "highlights_false_color");
  gd->kernel_interpolate_bilinear = dt_opencl_create_kernel(program, "interpolate_bilinear");

  const int wavelets = 35; // bspline.cl, from programs.conf
  gd->kernel_filmic_bspline_horizontal = dt_opencl_create_kernel(wavelets, "blur_2D_Bspline_horizontal");
  gd->kernel_filmic_bspline_vertical = dt_opencl_create_kernel(wavelets, "blur_2D_Bspline_vertical");
  gd->kernel_filmic_bspline_horizontal_local = dt_opencl_create_kernel(wavelets, "blur_2D_Bspline_horizontal_local");
  gd->kernel_filmic_bspline_vertical_local = dt_opencl_create_kernel(wavelets, "blur_2D_Bspline_vertical_local");

}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_highlights_global_data_t *gd = (dt_iop_highlights_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_highlights_4f_clip);
  dt_opencl_free_kernel(gd->kernel_highlights_1f_lch_bayer);
  dt_opencl_free_kernel(gd->kernel_highlights_1f_lch_xtrans);
  dt_opencl_free_kernel(gd->kernel_highlights_1f_clip);
  dt_opencl_free_kernel(gd->kernel_highlights_bilinear_and_mask);
  dt_opencl_free_kernel(gd->kernel_highlights_bilinear_and_mask_xtrans);
  dt_opencl_free_kernel(gd->kernel_highlights_normalize_reduce_first);
  dt_opencl_free_kernel(gd->kernel_highlights_normalize_reduce_first_xtrans);
  dt_opencl_free_kernel(gd->kernel_highlights_normalize_reduce_second);
  dt_opencl_free_kernel(gd->kernel_highlights_remosaic_and_replace);
  dt_opencl_free_kernel(gd->kernel_highlights_remosaic_and_replace_xtrans);
  dt_opencl_free_kernel(gd->kernel_highlights_box_blur);
  dt_opencl_free_kernel(gd->kernel_highlights_guide_laplacians);
  dt_opencl_free_kernel(gd->kernel_highlights_diffuse_color);
  dt_opencl_free_kernel(gd->kernel_highlights_false_color);

  dt_opencl_free_kernel(gd->kernel_filmic_bspline_vertical);
  dt_opencl_free_kernel(gd->kernel_filmic_bspline_horizontal);
  dt_opencl_free_kernel(gd->kernel_filmic_bspline_vertical_local);
  dt_opencl_free_kernel(gd->kernel_filmic_bspline_horizontal_local);

  dt_opencl_free_kernel(gd->kernel_interpolate_bilinear);

  dt_free(module->data);
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc_align(sizeof(dt_iop_highlights_data_t));
  piece->data_size = sizeof(dt_iop_highlights_data_t);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_free_align(piece->data);
  piece->data = NULL;
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;
  dt_iop_highlights_params_t *p = (dt_iop_highlights_params_t *)self->params;

  const gboolean raw = (self->dev->image_storage.dsc.filters != 0);
  const gboolean israw = (self->dev->image_storage.dsc.filters != 0);
  dt_iop_highlights_mode_t mode = p->mode;

  gtk_widget_set_visible(g->noise_level, raw && mode == DT_IOP_HIGHLIGHTS_LAPLACIAN);
  gtk_widget_set_visible(g->iterations, raw && mode == DT_IOP_HIGHLIGHTS_LAPLACIAN);
  gtk_widget_set_visible(g->scales, raw && mode == DT_IOP_HIGHLIGHTS_LAPLACIAN);
  gtk_widget_set_visible(g->solid_color, raw && mode == DT_IOP_HIGHLIGHTS_LAPLACIAN);

  dt_bauhaus_widget_set_quad_visibility(g->clip, israw);
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;
  const gboolean monochrome = dt_image_is_monochrome(&self->dev->image_storage);
  // enable this per default if raw or sraw if not real monochrome
  self->default_enabled = dt_image_is_rawprepare_supported(&self->dev->image_storage) && !monochrome;

  // Neuter the on/off button only if not already enabled.
  // It can be enabled by history copy & paste from a RAW image.
  self->hide_enable_button = monochrome && !self->enabled;
  gtk_stack_set_visible_child_name(GTK_STACK(self->widget), self->default_enabled ? "default" : "monochrome");

  dt_bauhaus_widget_set_quad_active(g->clip, FALSE);
  g->show_visualize = FALSE;
  gui_changed(self, NULL, NULL);
}


void reload_defaults(dt_iop_module_t *module)
{
  // we might be called from presets update infrastructure => there is no image
  if(!module->dev || module->dev->image_storage.id == -1) return;

  // enable this per default if raw or sraw if not real monochrome
  module->default_enabled = enable(&module->dev->image_storage);
  module->hide_enable_button = !enable(&module->dev->image_storage);
  if(module->widget)
    gtk_stack_set_visible_child_name(GTK_STACK(module->widget), module->default_enabled ? "default" : "monochrome");

  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)module->gui_data;

  if(g)
    if(dt_bauhaus_combobox_length(g->mode) < DT_IOP_HIGHLIGHTS_LAPLACIAN + 1)
      dt_bauhaus_combobox_add_full(g->mode, _("guided laplacians"), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT,
                                    GINT_TO_POINTER(DT_IOP_HIGHLIGHTS_LAPLACIAN), NULL, TRUE);
}

static void _visualize_callback(GtkWidget *quad, gpointer user_data)
{
  if(darktable.gui->reset) return;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;

  // if blend module is displaying mask do not display it here
  if(self->request_mask_display != DT_DEV_PIXELPIPE_DISPLAY_NONE)
    self->request_mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;

  g->show_visualize = dt_bauhaus_widget_get_quad_active(quad);

  if(g->show_visualize)
    self->request_mask_display = DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU;

  dt_iop_set_cache_bypass(self, g->show_visualize);
  dt_dev_pixelpipe_update_history_main(self->dev);
}

void gui_focus(struct dt_iop_module_t *self, gboolean in)
{
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;
  if(!in)
  {
    const gboolean was_visualize = g->show_visualize;
    dt_bauhaus_widget_set_quad_active(g->clip, FALSE);
    g->show_visualize = FALSE;
    if(was_visualize) dt_dev_pixelpipe_update_history_main(self->dev);
  }
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_highlights_gui_data_t *g = IOP_GUI_ALLOC(highlights);
  GtkWidget *box_raw = self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->mode = dt_bauhaus_combobox_from_params(self, "mode");
  gtk_widget_set_tooltip_text(g->mode, _("highlight reconstruction method"));

  g->clip = dt_bauhaus_slider_from_params(self, "clip");
  dt_bauhaus_slider_set_digits(g->clip, 3);
  gtk_widget_set_tooltip_text(g->clip,
                              _("manually adjust the clipping threshold against "
                                "magenta highlights\nthe mask icon shows the clipped area\n"
                                "(you shouldn't ever need to touch this)"));
  dt_bauhaus_widget_set_quad_paint(g->clip, dtgtk_cairo_paint_showmask, 0, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->clip, TRUE);
  dt_bauhaus_widget_set_quad_active(g->clip, FALSE);
  g_signal_connect(G_OBJECT(g->clip), "quad-pressed", G_CALLBACK(_visualize_callback), self);

  g->noise_level = dt_bauhaus_slider_from_params(self, "noise_level");
  gtk_widget_set_tooltip_text(g->noise_level, _("add noise to visually blend the reconstructed areas\n"
                                                "into the rest of the noisy image. useful at high ISO."));

  g->iterations = dt_bauhaus_slider_from_params(self, "iterations");
  dt_bauhaus_slider_set_soft_range(g->iterations, 1, 256);
  gtk_widget_set_tooltip_text(g->iterations, _("increase if magenta highlights don't get fully corrected\n"
                                               "each new iteration brings a performance penalty."));

  g->solid_color = dt_bauhaus_slider_from_params(self, "solid_color");
  dt_bauhaus_slider_set_format(g->solid_color, "%");
  gtk_widget_set_tooltip_text(g->solid_color, _("increase if magenta highlights don't get fully corrected.\n"
                                                "this may produce non-smooth boundaries between valid and clipped regions."));

  g->scales = dt_bauhaus_combobox_from_params(self, "scales");
  gtk_widget_set_tooltip_text(g->scales, _("increase to correct larger clipped areas.\n"
                                           "large values bring huge performance penalties"));

  GtkWidget *monochromes = dt_ui_label_new(_("not applicable"));
  gtk_widget_set_tooltip_text(monochromes, _("no highlights reconstruction for monochrome images"));

  // start building top level widget
  self->widget = gtk_stack_new();
  gtk_stack_set_homogeneous(GTK_STACK(self->widget), FALSE);
  gtk_stack_add_named(GTK_STACK(self->widget), monochromes, "monochrome");
  gtk_stack_add_named(GTK_STACK(self->widget), box_raw, "default");
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
