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
#include "bauhaus/bauhaus.h"
#include "common/box_filters.h"
#include "common/bspline.h"
#include "common/fast_guided_filter.h"
#include "common/gaussian.h"
#include "common/imagebuf.h"
#include "common/opencl.h"
#include "common/solvers/choleski.h" // dense Cholesky solve (SPD) for the direct biharmonic dome (needs control.h)
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/imageop_math.h"
#include "develop/noise_generator.h"
#include "develop/tiling.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "gui/gtk.h"
#include "iop/iop_api.h"
// Highlights internals: harmonic mode is a set of compiled per-stage TUs; include only the
// shared types (params + per-module OpenCL global data) and the high-level harmonic driver.
#include "iop/highlights/clip.h"
#include "iop/highlights/common.h"
#include "iop/highlights/inpaint.h"
#include "iop/highlights/laplacian.h"
#include "iop/highlights/lch.h"
#include "iop/highlights/process.h"

#include <gtk/gtk.h>
#include <inttypes.h>

// Downsampling factor for guided-laplacian

DT_MODULE_INTROSPECTION(4, dt_iop_highlights_params_t)

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

const char *name()
{
  return _("_highlight reconstruction");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("avoid magenta highlights and try to recover highlights colors"),
                                _("corrective"), _("linear, raw, scene-referred"), _("reconstruction, raw"),
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
  if(piece && piece->dsc_in.cst != IOP_CS_RAW) return IOP_CS_RGB;
  return IOP_CS_RAW;
}

void output_format(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                   dt_iop_buffer_dsc_t *dsc)
{
  default_output_format(self, pipe, piece, dsc);
}

void autoset(struct dt_iop_module_t *self, const struct dt_dev_pixelpipe_t *pipe,
             const struct dt_dev_pixelpipe_iop_t *piece, const void *input)
{
  dt_iop_highlights_params_t *p = (dt_iop_highlights_params_t *)self->params;
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  const float *const restrict in = (const float *)input;
  float max_RGB[3] = { 0.0f };

  __OMP_PARALLEL_FOR__(reduction(max : max_RGB[:3]) collapse(2))
  for(size_t i = 0; i < roi_out->height; i++)
    for(size_t j = 0; j < roi_out->width; j++)
    {
      const size_t channel = (piece->dsc_in.filters == 9u)
                                 ? FCxtrans(i, j, roi_out, piece->dsc_in.xtrans)
                                 : FC(i + roi_out->y, j + roi_out->x, piece->dsc_in.filters);
      const float pixel_max = in[i * roi_out->width + j] / piece->dsc_in.processed_maximum[channel];
      max_RGB[channel] = MAX(max_RGB[channel], pixel_max);
    }

  p->clip = MIN(MIN(max_RGB[0], max_RGB[1]), max_RGB[2]);
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version, void *new_params,
                  const int new_version)
{
  if(old_version == 1 && new_version == 4)
  {
    /*
      params of v2 :
        float clip
      + params of v3
      + params of v4
    */
    memcpy(new_params, old_params,
           sizeof(dt_iop_highlights_params_t) - 5 * sizeof(float) - 2 * sizeof(int)
               - sizeof(dt_atrous_wavelets_scales_t));
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
    memcpy(new_params, old_params,
           sizeof(dt_iop_highlights_params_t) - 4 * sizeof(float) - 2 * sizeof(int)
               - sizeof(dt_atrous_wavelets_scales_t));
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

// The per-mode OpenCL helpers below carry the kernel-argument boilerplate so process_cl() reads as a
// clean mode switch, symmetric to process(). Each returns a cl_int (CL_SUCCESS or an error to bubble up).

// Clip-visualization false-colour quad (raw only; the caller sets mask_display/bypass_blendif on success).
static cl_int _hl_cl_visualize(dt_iop_highlights_global_data_t *gd, const int devid, cl_mem dev_in,
                               cl_mem dev_out, const int width, const int height,
                               const dt_iop_roi_t *const roi_out, const uint32_t filters,
                               const dt_aligned_pixel_t clips)
{
  cl_mem dev_clips = dt_opencl_copy_host_to_device_constant(devid, 4 * sizeof(float), (float *)clips);
  if(IS_NULL_PTR(dev_clips)) return DT_OPENCL_DEFAULT_ERROR;
  size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_false_color, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_false_color, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_false_color, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_false_color, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_false_color, 4, sizeof(int), (void *)&roi_out->x);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_false_color, 5, sizeof(int), (void *)&roi_out->y);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_false_color, 6, sizeof(int), (void *)&filters);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_false_color, 7, sizeof(cl_mem), (void *)&dev_clips);
  const cl_int err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_false_color, sizes);
  dt_opencl_release_mem_object(dev_clips);
  return err;
}

// Plain clip. Raw mosaic uses the single-channel kernel (roi-aware); non-raw uses the 4-channel kernel.
// This is also the fallback for the raw-only reconstruction modes (LCh / colour inpainting) on non-raw.
static cl_int _hl_cl_clip(dt_iop_highlights_global_data_t *gd, const int devid, cl_mem dev_in, cl_mem dev_out,
                          const int width, const int height, const dt_iop_roi_t *const roi_out,
                          const uint32_t filters, const int mode, const float clip)
{
  size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
  if(filters)
  {
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_clip, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_clip, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_clip, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_clip, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_clip, 4, sizeof(float), (void *)&clip);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_clip, 5, sizeof(int), (void *)&roi_out->x);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_clip, 6, sizeof(int), (void *)&roi_out->y);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_clip, 7, sizeof(int), (void *)&filters);
    return dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_1f_clip, sizes);
  }
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_4f_clip, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_4f_clip, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_4f_clip, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_4f_clip, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_4f_clip, 4, sizeof(int), (void *)&mode);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_4f_clip, 5, sizeof(float), (void *)&clip);
  return dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_4f_clip, sizes);
}

// LCh reconstruction, Bayer.
static cl_int _hl_cl_lch_bayer(dt_iop_highlights_global_data_t *gd, const int devid, cl_mem dev_in,
                               cl_mem dev_out, const int width, const int height,
                               const dt_iop_roi_t *const roi_out, const uint32_t filters, const float clip)
{
  size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_lch_bayer, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_lch_bayer, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_lch_bayer, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_lch_bayer, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_lch_bayer, 4, sizeof(float), (void *)&clip);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_lch_bayer, 5, sizeof(int), (void *)&roi_out->x);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_lch_bayer, 6, sizeof(int), (void *)&roi_out->y);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f_lch_bayer, 7, sizeof(int), (void *)&filters);
  return dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_1f_lch_bayer, sizes);
}

// LCh reconstruction, X-Trans (needs the local-memory tile plus a device copy of the xtrans matrix).
static cl_int _hl_cl_lch_xtrans(dt_iop_highlights_global_data_t *gd, const int devid, cl_mem dev_in,
                                cl_mem dev_out, const int width, const int height,
                                const dt_iop_roi_t *const roi_out, const dt_dev_pixelpipe_iop_t *piece,
                                const float clip)
{
  int blocksizex, blocksizey;
  dt_opencl_local_buffer_t locopt = (dt_opencl_local_buffer_t){ .xoffset = 2 * 2,
                                                                .xfactor = 1,
                                                                .yoffset = 2 * 2,
                                                                .yfactor = 1,
                                                                .cellsize = sizeof(float),
                                                                .overhead = 0,
                                                                .sizex = 1 << 8,
                                                                .sizey = 1 << 8 };
  if(dt_opencl_local_buffer_opt(devid, gd->kernel_highlights_1f_lch_xtrans, &locopt))
  {
    blocksizex = locopt.sizex;
    blocksizey = locopt.sizey;
  }
  else
    blocksizex = blocksizey = 1;

  cl_mem dev_xtrans = dt_opencl_copy_host_to_device_constant(devid, sizeof(piece->dsc_in.xtrans),
                                                             (void *)piece->dsc_in.xtrans);
  if(IS_NULL_PTR(dev_xtrans)) return DT_OPENCL_DEFAULT_ERROR;

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
  const cl_int err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_highlights_1f_lch_xtrans, sizes,
                                                            local);
  dt_opencl_release_mem_object(dev_xtrans);
  return err;
}

int process_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in, cl_mem dev_out)
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

  /* This transient preview belongs to the central darkroom view. Do not infer
   * its owner from ROI geometry: at zoom-to-fit the main and navigation pipes
   * can produce identical dimensions while both still need distinct outputs. */
  // The clip-visualization quad reads the buffer as a raw mosaic (FC / false-colour kernel), so it is
  // raw-only; the toggle is also hidden on non-raw images in gui_update().
  const gboolean visualizing = !IS_NULL_PTR(g) && g->show_visualize && filters && self->dev->gui_attached
                               && pipe == self->dev->pipe;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  if(visualizing)
  {
    const dt_aligned_pixel_t clips = { 0.995f * d->clip * piece->dsc_in.processed_maximum[0],
                                       0.995f * d->clip * piece->dsc_in.processed_maximum[1],
                                       0.995f * d->clip * piece->dsc_in.processed_maximum[2], d->clip };
    err = _hl_cl_visualize(gd, devid, dev_in, dev_out, width, height, roi_out, filters, clips);
    if(err != CL_SUCCESS) goto error;
    /* The clipping preview is the final output of this module. Blending would interpret PASSTHRU as a
     * channel-display request and replace RAW output with zeroes before the downstream demosaic stage
     * can display it. */
    ((dt_dev_pixelpipe_t *)pipe)->mask_display = DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU;
    ((dt_dev_pixelpipe_t *)pipe)->bypass_blendif = 1;
    return TRUE;
  }

  // Clip white point -- default non-positive channels (non-raw, where rawprepare never set it) to 1.0,
  // mirroring process(). Without this the non-raw clip threshold collapses to 0 and the reconstruction
  // treats the whole frame as clipped. Raw is unchanged (rawprepare already made every channel 1.0).
  dt_aligned_pixel_t pmax;
  for(int c = 0; c < 4; c++)
    pmax[c] = (piece->dsc_in.processed_maximum[c] > 0.f) ? piece->dsc_in.processed_maximum[c] : 1.0f;
  const float clip = d->clip * fminf(pmax[0], fminf(pmax[1], pmax[2]));
  const dt_aligned_pixel_t clips
      = { 0.995f * d->clip * pmax[0], 0.995f * d->clip * pmax[1], 0.995f * d->clip * pmax[2], clip };

  // Mode switch, symmetric to process(). The reconstruction modes run on the GPU for raw and non-raw
  // alike -- the non-raw passthrough drivers use their own device gather/remosaic kernels, no CPU
  // roundtrip. The raw-only modes (LCh / colour inpainting) fall back to a device clip on non-raw input.
  switch(d->mode)
  {
    case DT_IOP_HIGHLIGHTS_LCH:
      if(filters == 9u)
        err = _hl_cl_lch_xtrans(gd, devid, dev_in, dev_out, width, height, roi_out, piece, clip);
      else if(filters)
        err = _hl_cl_lch_bayer(gd, devid, dev_in, dev_out, width, height, roi_out, filters, clip);
      else
        err = _hl_cl_clip(gd, devid, dev_in, dev_out, width, height, roi_out, filters, d->mode, clip);
      break;
    case DT_IOP_HIGHLIGHTS_LAPLACIAN:
      if(filters == 9u)
        err = process_laplacian_xtrans_cl(self, pipe, piece, dev_in, dev_out, roi_in, roi_out, clips);
      else if(filters)
        err = process_laplacian_bayer_cl(self, pipe, piece, dev_in, dev_out, roi_in, roi_out, clips);
      else
        err = process_laplacian_passthrough_cl(self, pipe, piece, dev_in, dev_out, roi_in, roi_out, clips);
      break;
    case DT_IOP_HIGHLIGHTS_HARMONIC:
      // process_harmonic_cl handles Bayer, X-Trans and non-raw passthrough (selected internally); its
      // reconstruction middle is CFA-agnostic and reused for all three.
      err = process_harmonic_cl(self, pipe, piece, dev_in, dev_out, roi_in, roi_out, clips);
      break;
    case DT_IOP_HIGHLIGHTS_INPAINT:
      // Colour inpainting has no device kernel; commit_params() clears process_cl_ready so this is not
      // normally reached. Signal a CPU fallback defensively.
      return FALSE;
    case DT_IOP_HIGHLIGHTS_CLIP:
    default:
      err = _hl_cl_clip(gd, devid, dev_in, dev_out, width, height, roi_out, filters, d->mode, clip);
      break;
  }

  if(err != CL_SUCCESS) goto error;
  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_highlights] couldn't enqueue kernel! %i\n", err);
  return FALSE;
}
#endif

void tiling_callback(struct dt_iop_module_t *self, const struct dt_dev_pixelpipe_t *pipe,
                     const struct dt_dev_pixelpipe_iop_t *piece, struct dt_develop_tiling_t *tiling)
{
  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  dt_iop_highlights_data_t *d = (dt_iop_highlights_data_t *)piece->data;
  const uint32_t filters = piece->dsc_in.filters;

  if((d->mode == DT_IOP_HIGHLIGHTS_LAPLACIAN || d->mode == DT_IOP_HIGHLIGHTS_HARMONIC) && filters)
  {
    // Mosaic CFA and guided laplacian method: prepare for wavelets decomposition.
    // Harmonic transposition reuses the same conservative budget (it disables tiling in
    // commit_params and allocates region buffers well under this envelope).
    const float scale = DS_FACTOR * dt_dev_get_module_scale(pipe, roi_in);
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

  tiling->factor = 2.0f; // in + out
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

#undef SQRT3
#undef SQRT12


// Human-readable mode label (matches the $DESCRIPTION strings on dt_iop_highlights_mode_t), for the
// non-raw fallback warning. Wrap in _() at the call site for translation.
static const char *_highlights_mode_name(const dt_iop_highlights_mode_t mode)
{
  switch(mode)
  {
    case DT_IOP_HIGHLIGHTS_CLIP:      return N_("clip highlights");
    case DT_IOP_HIGHLIGHTS_LCH:       return N_("reconstruct in LCh");
    case DT_IOP_HIGHLIGHTS_INPAINT:   return N_("reconstruct color");
    case DT_IOP_HIGHLIGHTS_LAPLACIAN: return N_("guided laplacians");
    case DT_IOP_HIGHLIGHTS_HARMONIC:  return N_("harmonic transposition");
  }
  return N_("unknown");
}

// Returns TRUE when `mode` has a real path for the given (roi-shifted) CFA descriptor. Any raw mosaic
// (filters != 0) supports every mode. On already-demosaiced input (filters == 0) CLIP thresholds each
// channel and LAPLACIAN/HARMONIC reconstruct via their passthrough gather, but LCh and colour inpainting
// are raw-mosaic-only and silently fall back to clip. The channels!=4 mono-raw case never reaches here:
// it is excluded at history level by enable()/force_enable() (monochrome images are rejected). Keep in
// sync with the process() mode switch.
static gboolean _highlights_mode_supported(const dt_iop_highlights_mode_t mode, const uint32_t filters)
{
  if(filters) return TRUE; // any raw mosaic: every mode has a path
  return (mode == DT_IOP_HIGHLIGHTS_CLIP || mode == DT_IOP_HIGHLIGHTS_LAPLACIAN
          || mode == DT_IOP_HIGHLIGHTS_HARMONIC);
}

__DT_CLONE_TARGETS__
int process(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece,
            const void *const ivoid, void *const ovoid)
{
  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  // process_visualize and interpolate_color (INPAINT mode) below read FC(row, col, filters)
  // with tile-local row/col, so filters must be pre-shifted for roi_in's crop position here.
  // The laplacian dispatch below passes roi_in/roi_out, not this filters value, and derives its
  // own (correctly shifted) copy internally -- shifting this local doesn't affect it. The
  // filters==9u / !filters checks are shift-invariant.
  const uint32_t filters = dt_dev_get_roi_filters(piece, roi_in);
  dt_iop_highlights_data_t *data = (dt_iop_highlights_data_t *)piece->data;
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;

  /* This transient preview belongs to the central darkroom view. Do not infer
   * its owner from ROI geometry: at zoom-to-fit the main and navigation pipes
   * can produce identical dimensions while both still need distinct outputs. */
  // The clip-visualization quad reads the buffer as a raw mosaic (FC / false-colour kernel), so it is
  // raw-only; the toggle is also hidden on non-raw images in gui_update().
  const gboolean visualizing = !IS_NULL_PTR(g) && g->show_visualize && filters && self->dev->gui_attached
                               && pipe == self->dev->pipe;

  if(visualizing)
  {
    process_visualize(piece, ivoid, ovoid, roi_in, roi_out, filters, data);
    /* The clipping preview is the final output of this module. Blending would
     * interpret PASSTHRU as a channel-display request and replace RAW output
     * with zeroes before the downstream demosaic stage can display it. */
    ((dt_dev_pixelpipe_t *)pipe)->mask_display = DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU;
    ((dt_dev_pixelpipe_t *)pipe)->bypass_blendif = 1;
    return 0;
  }

  // Clip white point. rawprepare sets processed_maximum to 1.0 for raw; on non-raw it is never set and
  // stays 0, which would collapse the clip threshold to 0 -- marking the whole image as clipped (a black
  // "clip" and full-frame over-reconstruction / halos in laplacian & harmonic). Default any non-positive
  // channel to 1.0 (the white point of normalized RGB) so non-raw input clips only genuinely blown
  // highlights. Raw is unchanged: rawprepare already made every channel 1.0.
  dt_aligned_pixel_t pmax;
  for(int c = 0; c < 4; c++)
    pmax[c] = (piece->dsc_in.processed_maximum[c] > 0.f) ? piece->dsc_in.processed_maximum[c] : 1.0f;
  const float clip = data->clip * fminf(pmax[0], fminf(pmax[1], pmax[2]));

  // Non-raw input is no longer nuked to a plain clip here: the mode switch below dispatches it too
  // (LAPLACIAN/HARMONIC reconstruct already-demosaiced RGB via their passthrough gather, CLIP thresholds
  // per channel, LCh/INPAINT have no non-raw path and fall back to _highlights_copy_input).

  switch(data->mode)
  {
    case DT_IOP_HIGHLIGHTS_INPAINT: // a1ex's (magiclantern) idea of color inpainting:
    {
      const float clips[4] = { 0.987f * data->clip * pmax[0], 0.987f * data->clip * pmax[1],
                               0.987f * data->clip * pmax[2], clip };
      if(filters == 9u)
        process_inpaint_xtrans(ivoid, ovoid, roi_in, roi_out, clips,
                               (const uint8_t(*const)[6])piece->dsc_in.xtrans);
      else if(filters)
        process_inpaint_bayer(ivoid, ovoid, roi_out, clips, filters);
      else
        process_clip(piece, ivoid, ovoid, roi_in, roi_out, clip); // colour inpainting is raw-mosaic only
      break;
    }
    case DT_IOP_HIGHLIGHTS_LCH:
      if(filters == 9u)
        process_lch_xtrans(self, piece, ivoid, ovoid, roi_in, roi_out, clip);
      else if(filters)
        process_lch_bayer(self, piece, ivoid, ovoid, roi_in, roi_out, clip);
      else
        process_clip(piece, ivoid, ovoid, roi_in, roi_out, clip); // LCh is raw-mosaic only
      break;
    case DT_IOP_HIGHLIGHTS_LAPLACIAN:
    {
      // process_laplacian reconstructs Bayer, X-Trans and already-demosaiced RGB (passthrough gather,
      // selected internally from the CFA descriptor). Non-raw input reaching here is guaranteed 4-channel:
      // the channels!=4 mono-raw case is an image-type decision made once in force_enable()/commit_params(),
      // not per frame -- the module never runs on such images.
      const dt_aligned_pixel_t clips = { 0.995f * data->clip * pmax[0], 0.995f * data->clip * pmax[1],
                                         0.995f * data->clip * pmax[2], clip };
      if(process_laplacian(self, pipe, piece, ivoid, ovoid, roi_in, roi_out, clips))
        return 1;
      break;
    }
    case DT_IOP_HIGHLIGHTS_HARMONIC:
    {
      const dt_aligned_pixel_t clips = { 0.995f * data->clip * pmax[0], 0.995f * data->clip * pmax[1],
                                         0.995f * data->clip * pmax[2], clip };
      if(process_harmonic(self, pipe, piece, ivoid, ovoid, roi_in, roi_out, clips))
        return 1;
      break;
    }
    default:
    case DT_IOP_HIGHLIGHTS_CLIP:
      process_clip(piece, ivoid, ovoid, roi_in, roi_out, clip); // handles raw + non-raw (per channel)
      break;
  }

  // TODO: this should be handled in the pipeline recursion directly
  if(pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
    dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
  return 0;
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_highlights_params_t *p = (dt_iop_highlights_params_t *)p1;
  dt_iop_highlights_data_t *d = (dt_iop_highlights_data_t *)piece->data;

  memcpy(d, p, sizeof(*p));

  // Image-type eligibility (raw colorimetry, not monochrome) is decided once at history level by
  // enable()/force_enable()/reload_defaults(); nothing type-related is decided per frame. process()'s
  // mode switch dispatches non-raw input too: LAPLACIAN/HARMONIC reconstruct already-demosaiced RGB
  // (sRAW / linear DNG) via their passthrough gather, CLIP thresholds per channel, and the raw-only
  // modes (LCh / colour inpainting) fall back to clip.
  const dt_image_t *const img = &self->dev->image_storage;
  dt_iop_fmt_log(self, "commit: class=%s filters=%u mode=%d -> enabled=%d",
                 dt_image_pipe_class_name(dt_image_pipe_class(img)), piece->dsc_in.filters, d->mode,
                 piece->enabled);

  // no OpenCL for DT_IOP_HIGHLIGHTS_INPAINT. Every other mode keeps its CL path, including the non-raw
  // reconstruction paths (LAPLACIAN/HARMONIC on already-demosaiced RGB), which run on the GPU through
  // their dedicated passthrough gather/remosaic kernels -- no CPU roundtrip.
  piece->process_cl_ready = (d->mode == DT_IOP_HIGHLIGHTS_INPAINT) ? 0 : 1;

  // Warn once per pipe setup (not per frame) when a raw-only reconstruction mode (LCh / colour
  // inpainting) has no path for this non-raw image and silently falls back to clipping.
  if(!_highlights_mode_supported(d->mode, piece->dsc_in.filters))
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[highlights] mode \"%s\" has no reconstruction path for this image; falling back to clipping\n",
             _highlights_mode_name(d->mode));
    if(self->dev->gui_attached && pipe == self->dev->pipe)
      dt_control_log(_("highlight reconstruction: \"%s\" is not available for this image type; clipping instead"),
                     _(_highlights_mode_name(d->mode)));
  }

  if(d->mode == DT_IOP_HIGHLIGHTS_LAPLACIAN || d->mode == DT_IOP_HIGHLIGHTS_HARMONIC)
    piece->cache_output_on_ram = TRUE;

  if(d->mode == DT_IOP_HIGHLIGHTS_HARMONIC)
  {
    // The segmented full-resolution reconstruction needs each clipped region intact: tiling
    // would split a large blown highlight (e.g. the sun) across tile borders, and the per-region
    // biharmonic core dome would then be solved on each half against a fake tile edge (-> a
    // half-recovered, dark-cored disc). Process the whole frame instead.
    piece->process_tiling_ready = 0;
  }

  // Non-raw reconstruction (LAPLACIAN/HARMONIC passthrough) runs whole-frame too: the tiling budget in
  // tiling_callback() is written for the single-channel raw layout, and the reconstruction downsamples
  // internally -- keep the frame intact rather than mis-tiling it.
  if(!piece->dsc_in.filters
     && (d->mode == DT_IOP_HIGHLIGHTS_LAPLACIAN || d->mode == DT_IOP_HIGHLIGHTS_HARMONIC))
    piece->process_tiling_ready = 0;

  if(d->mode != DT_IOP_HIGHLIGHTS_LAPLACIAN && d->mode != DT_IOP_HIGHLIGHTS_HARMONIC)
  {
    if(!piece->dsc_in.filters)
    {
      // Non-raw: processed_maximum is left at 0 upstream (no rawprepare). Default it to the normalized-RGB
      // white point (1.0) rather than propagating 0 downstream (which collapses any later clip logic).
      const float m0 = fminf(piece->dsc_in.processed_maximum[0],
                             fminf(piece->dsc_in.processed_maximum[1], piece->dsc_in.processed_maximum[2]));
      const float m = (m0 > 0.f) ? m0 : 1.0f;
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

// Whether the module can run on this image at all (eligibility, NOT auto-enable). The module
// self-disables only on a non-mosaiced image that is not 4-channel -- a mono-raw / greyscale
// (filters==0, channels==1) has no colour to clip or reconstruct. Everything else is eligible: a
// mosaiced raw, an already-demosaiced sRAW/linear-DNG, and a rendered RGB (all 4-channel or mosaic).
// Monochrome is the image-level proxy for "not 4-channel"; mosaiced is filters!=0. Shared by
// force_enable()/reload_defaults()/gui_update() so the self-disable rule lives in exactly one place.
static gboolean _highlights_image_supported(const dt_image_t *image)
{
  return dt_image_is_mosaiced(image) || !dt_image_is_monochrome(image);
}

// Whether the module should be enabled BY DEFAULT (auto-on). Only raw colorimetry (mosaiced raw or
// sRAW/linear-DNG), not monochrome: on already-rendered RGB the module is available but opt-in, never
// auto-enabled. Must match the commit_params() gate above.
static gboolean enable(const dt_image_t *image)
{
  return dt_image_needs_rawprepare(image) && !dt_image_is_monochrome(image);
}

gboolean force_enable(struct dt_iop_module_t *self, const gboolean current_state)
{
  // History sanitization: clamp against the eligibility rule (self-disable only on non-mosaiced &&
  // not-4-channel). This lets an opt-in enable on a rendered RGB / sRAW survive, while still stripping
  // a highlights entry pasted onto a mono-raw. Auto-enable (default_enabled) stays raw-only in
  // reload_defaults()/gui_update(); this only decides whether an already-set enable may stand.
  const gboolean active = _highlights_image_supported(&self->dev->image_storage);
  const gboolean state = current_state && active;
  dt_iop_fmt_log(self, "force_enable: class=%s supported=%d current=%d -> %d",
                 dt_image_pipe_class_name(dt_image_pipe_class(&self->dev->image_storage)), active, current_state,
                 state);
  return state;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_highlights_global_data_t *gd
      = (dt_iop_highlights_global_data_t *)malloc(sizeof(dt_iop_highlights_global_data_t));
  module->data = gd;
  gd->kernel_highlights_1f_clip = dt_opencl_create_kernel(program, "highlights_1f_clip");
  const int harmonic_program = 38; // highlights_harmonic.cl (harmonic transposition, fp32)
  const int sparse_program = 37;   // highlights_sparse.cl (fp64 sparse solvers)
  gd->kernel_sparse_chol_update_level = dt_opencl_create_kernel(sparse_program, "sparse_chol_update_level");
  gd->kernel_sparse_chol_final_level = dt_opencl_create_kernel(sparse_program, "sparse_chol_final_level");
  gd->kernel_sparse_chol_fwd_level = dt_opencl_create_kernel(sparse_program, "sparse_chol_fwd_level");
  gd->kernel_sparse_chol_bwd_level = dt_opencl_create_kernel(sparse_program, "sparse_chol_bwd_level");
  gd->kernel_hl_pde_rhs = dt_opencl_create_kernel(sparse_program, "hl_pde_rhs");
  gd->kernel_hl_pde_scatter = dt_opencl_create_kernel(sparse_program, "hl_pde_scatter");
  gd->kernel_hl_aniso_rhs = dt_opencl_create_kernel(sparse_program, "hl_aniso_rhs");
  gd->kernel_hl_aniso_scatter = dt_opencl_create_kernel(sparse_program, "hl_aniso_scatter");
  gd->kernel_hl_cg_r1 = dt_opencl_create_kernel(sparse_program, "hl_cg_r1");
  gd->kernel_hl_cg_ap = dt_opencl_create_kernel(sparse_program, "hl_cg_ap");
  gd->kernel_hl_cg_update = dt_opencl_create_kernel(sparse_program, "hl_cg_update");
  gd->kernel_hl_cfa_steer = dt_opencl_create_kernel(harmonic_program, "hl_cfa_steer");
  gd->kernel_hl_cfa_down = dt_opencl_create_kernel(harmonic_program, "hl_cfa_down");
  gd->kernel_hl_cfa_box = dt_opencl_create_kernel(harmonic_program, "hl_cfa_box");
  gd->kernel_hl_cfa_grad = dt_opencl_create_kernel(harmonic_program, "hl_cfa_grad");
  gd->kernel_hl_cfa_tensor = dt_opencl_create_kernel(harmonic_program, "hl_cfa_tensor");
  gd->kernel_hl_cfa_gnorm = dt_opencl_create_kernel(harmonic_program, "hl_cfa_gnorm");
  gd->kernel_hl_cfa_weights = dt_opencl_create_kernel(harmonic_program, "hl_cfa_weights");
  gd->kernel_hl_cfa_jacobi = dt_opencl_create_kernel(harmonic_program, "hl_cfa_jacobi");
  gd->kernel_hl_cfa_jacobi_block = dt_opencl_create_kernel(harmonic_program, "hl_cfa_jacobi_block");
  gd->kernel_hl_fill_down = dt_opencl_create_kernel(harmonic_program, "hl_fill_down");
  gd->kernel_hl_fill_seed = dt_opencl_create_kernel(harmonic_program, "hl_fill_seed");
  gd->kernel_hl_fill_seed_up = dt_opencl_create_kernel(harmonic_program, "hl_fill_seed_up");
  gd->kernel_hl_fill_jacobi = dt_opencl_create_kernel(harmonic_program, "hl_fill_jacobi");
  gd->kernel_hl_fill_jacobi_block = dt_opencl_create_kernel(harmonic_program, "hl_fill_jacobi_block");
  gd->kernel_hl_fill_up = dt_opencl_create_kernel(harmonic_program, "hl_fill_up");
  gd->kernel_hl_cf_lref_partials = dt_opencl_create_kernel(harmonic_program, "hl_cf_lref_partials");
  gd->kernel_hl_cf_pack_joint = dt_opencl_create_kernel(harmonic_program, "hl_cf_pack_joint");
  gd->kernel_hl_cf_fit_joint = dt_opencl_create_kernel(harmonic_program, "hl_cf_fit_joint");
  gd->kernel_hl_cf_eval_joint = dt_opencl_create_kernel(harmonic_program, "hl_cf_eval_joint");
  gd->kernel_hl_cf_pack_pair = dt_opencl_create_kernel(harmonic_program, "hl_cf_pack_pair");
  gd->kernel_hl_cf_fit_pair = dt_opencl_create_kernel(harmonic_program, "hl_cf_fit_pair");
  gd->kernel_hl_cf_eval_pair = dt_opencl_create_kernel(harmonic_program, "hl_cf_eval_pair");
  gd->kernel_hl_cf_pack_deepmask = dt_opencl_create_kernel(harmonic_program, "hl_cf_pack_deepmask");
  gd->kernel_hl_cf_eval_deep = dt_opencl_create_kernel(harmonic_program, "hl_cf_eval_deep");
  gd->kernel_hl_buf_to_img = dt_opencl_create_kernel(harmonic_program, "hl_buf_to_img");
  gd->kernel_hl_hf_pack = dt_opencl_create_kernel(harmonic_program, "hl_hf_pack");
  gd->kernel_hl_hf_fit = dt_opencl_create_kernel(harmonic_program, "hl_hf_fit");
  gd->kernel_hl_hf_energy = dt_opencl_create_kernel(harmonic_program, "hl_hf_energy");
  gd->kernel_hl_hf_eval = dt_opencl_create_kernel(harmonic_program, "hl_hf_eval");
  gd->kernel_hl_hf_damp = dt_opencl_create_kernel(harmonic_program, "hl_hf_damp");
  gd->kernel_hl_soft_floor = dt_opencl_create_kernel(harmonic_program, "hl_soft_floor");
  gd->kernel_hl_hard_floor = dt_opencl_create_kernel(harmonic_program, "hl_hard_floor");
  gd->kernel_hl_lsb_hole = dt_opencl_create_kernel(harmonic_program, "hl_lsb_hole");
  gd->kernel_hl_ratio_plane = dt_opencl_create_kernel(harmonic_program, "hl_ratio_plane");
  gd->kernel_hl_dome_down = dt_opencl_create_kernel(harmonic_program, "hl_dome_down");
  gd->kernel_hl_dome_blend = dt_opencl_create_kernel(harmonic_program, "hl_dome_blend");
  gd->kernel_hl_core_floor = dt_opencl_create_kernel(harmonic_program, "hl_core_floor");
  gd->kernel_hl_cmean_reduce = dt_opencl_create_kernel(harmonic_program, "hl_cmean_reduce");
  gd->kernel_hl_ratio_cmean_blend = dt_opencl_create_kernel(harmonic_program, "hl_ratio_cmean_blend");
  gd->kernel_hl_clip0_rehue = dt_opencl_create_kernel(harmonic_program, "hl_clip0_rehue");
  gd->kernel_hl_ring_vote = dt_opencl_create_kernel(harmonic_program, "hl_ring_vote");
  gd->kernel_hl_cgrad_plateau = dt_opencl_create_kernel(harmonic_program, "hl_cgrad_plateau");
  gd->kernel_hl_cgrad_guard = dt_opencl_create_kernel(harmonic_program, "hl_cgrad_guard");
  gd->kernel_hl_cgrad_anchor = dt_opencl_create_kernel(harmonic_program, "hl_cgrad_anchor");
  gd->kernel_hl_cgrad_share = dt_opencl_create_kernel(harmonic_program, "hl_cgrad_share");
  gd->kernel_hl_cgrad_store = dt_opencl_create_kernel(harmonic_program, "hl_cgrad_store");
  gd->kernel_hl_cgrad_gate = dt_opencl_create_kernel(harmonic_program, "hl_cgrad_gate");
  gd->kernel_hl_cgrad_reproject = dt_opencl_create_kernel(harmonic_program, "hl_cgrad_reproject");
  gd->kernel_hl_cgrad_hole1c = dt_opencl_create_kernel(harmonic_program, "hl_cgrad_hole1c");
  gd->kernel_hl_cgrad_write1c = dt_opencl_create_kernel(harmonic_program, "hl_cgrad_write1c");
  gd->kernel_hl_pde_init = dt_opencl_create_kernel(harmonic_program, "hl_pde_init");
  gd->kernel_hl_mask_to_img1 = dt_opencl_create_kernel(harmonic_program, "hl_mask_to_img1");
  gd->kernel_hl_core_blend = dt_opencl_create_kernel(harmonic_program, "hl_core_blend");
  gd->kernel_hl_aniso_prep = dt_opencl_create_kernel(harmonic_program, "hl_aniso_prep");
  gd->kernel_hl_box3 = dt_opencl_create_kernel(harmonic_program, "hl_box3");
  gd->kernel_hl_grad_reduce = dt_opencl_create_kernel(harmonic_program, "hl_grad_reduce");
  gd->kernel_hl_aniso_tensor = dt_opencl_create_kernel(harmonic_program, "hl_aniso_tensor");
  gd->kernel_hl_aniso_weights = dt_opencl_create_kernel(harmonic_program, "hl_aniso_weights");
  gd->kernel_hl_aniso_reassemble = dt_opencl_create_kernel(harmonic_program, "hl_aniso_reassemble");
  gd->kernel_hl_knee_bin = dt_opencl_create_kernel(harmonic_program, "hl_knee_bin");
  gd->kernel_hl_knee_jmom = dt_opencl_create_kernel(harmonic_program, "hl_knee_jmom");
  gd->kernel_hl_knee_pmom = dt_opencl_create_kernel(harmonic_program, "hl_knee_pmom");
  gd->kernel_hl_knee_joint_reg = dt_opencl_create_kernel(harmonic_program, "hl_knee_joint_reg");
  gd->kernel_hl_knee_pair_reg = dt_opencl_create_kernel(harmonic_program, "hl_knee_pair_reg");
  gd->kernel_hl_knee_apply = dt_opencl_create_kernel(harmonic_program, "hl_knee_apply");
  gd->kernel_hl_mask_pack = dt_opencl_create_kernel(harmonic_program, "hl_mask_pack");
  gd->kernel_hl_region_gather = dt_opencl_create_kernel(harmonic_program, "hl_region_gather");
  gd->kernel_hl_region_scatter = dt_opencl_create_kernel(harmonic_program, "hl_region_scatter");
  gd->kernel_hl_region_stats = dt_opencl_create_kernel(harmonic_program, "hl_region_stats");
  gd->kernel_hl_need_self = dt_opencl_create_kernel(harmonic_program, "hl_need_self");
  gd->kernel_hl_knee_apply_interp = dt_opencl_create_kernel(harmonic_program, "hl_knee_apply_interp");
  gd->kernel_hl_cg_embed = dt_opencl_create_kernel(harmonic_program, "hl_cg_embed");
  gd->kernel_hl_cg_op = dt_opencl_create_kernel(harmonic_program, "hl_cg_op");
  gd->kernel_hl_cg_r0 = dt_opencl_create_kernel(harmonic_program, "hl_cg_r0");
  gd->kernel_hl_cg_beta = dt_opencl_create_kernel(harmonic_program, "hl_cg_beta");
  gd->kernel_hl_relu = dt_opencl_create_kernel(harmonic_program, "hl_relu");
  gd->kernel_hl_aniso_pyr_down = dt_opencl_create_kernel(harmonic_program, "hl_aniso_pyr_down");
  gd->kernel_hl_pyr_getc = dt_opencl_create_kernel(harmonic_program, "hl_pyr_getc");
  gd->kernel_hl_pyr_getc4 = dt_opencl_create_kernel(harmonic_program, "hl_pyr_getc4");
  gd->kernel_hl_pyr_putc4 = dt_opencl_create_kernel(harmonic_program, "hl_pyr_putc4");
  gd->kernel_hl_pyr_project = dt_opencl_create_kernel(harmonic_program, "hl_pyr_project");
  gd->kernel_hl_aniso_obs_full = dt_opencl_create_kernel(harmonic_program, "hl_aniso_obs_full");
  gd->kernel_hl_aniso_obs_flags = dt_opencl_create_kernel(harmonic_program, "hl_aniso_obs_flags");
  gd->kernel_hl_window_pack = dt_opencl_create_kernel(harmonic_program, "hl_window_pack");
  gd->kernel_hl_window_unpack = dt_opencl_create_kernel(harmonic_program, "hl_window_unpack");
  gd->kernel_hl_pyr_putc = dt_opencl_create_kernel(harmonic_program, "hl_pyr_putc");
  gd->kernel_hl_aniso_iter = dt_opencl_create_kernel(harmonic_program, "hl_aniso_iter");
  gd->kernel_hl_aniso_iter_block = dt_opencl_create_kernel(harmonic_program, "hl_aniso_iter_block");
  gd->kernel_hl_aniso_splat = dt_opencl_create_kernel(harmonic_program, "hl_aniso_splat");
  gd->kernel_highlights_1f_lch_bayer = dt_opencl_create_kernel(program, "highlights_1f_lch_bayer");
  gd->kernel_highlights_1f_lch_xtrans = dt_opencl_create_kernel(program, "highlights_1f_lch_xtrans");
  gd->kernel_highlights_4f_clip = dt_opencl_create_kernel(program, "highlights_4f_clip");
  gd->kernel_highlights_bilinear_and_mask = dt_opencl_create_kernel(program, "interpolate_and_mask");
  gd->kernel_highlights_bilinear_and_mask_xtrans = dt_opencl_create_kernel(program, "interpolate_and_mask_xtrans");
  gd->kernel_highlights_bilinear_and_mask_passthrough
      = dt_opencl_create_kernel(program, "interpolate_and_mask_passthrough");
  gd->kernel_highlights_normalize_reduce_first
      = dt_opencl_create_kernel(program, "highlights_normalize_reduce_first");
  gd->kernel_highlights_normalize_reduce_first_xtrans
      = dt_opencl_create_kernel(program, "highlights_normalize_reduce_first_xtrans");
  gd->kernel_highlights_normalize_reduce_first_passthrough
      = dt_opencl_create_kernel(program, "highlights_normalize_reduce_first_passthrough");
  gd->kernel_highlights_normalize_reduce_second
      = dt_opencl_create_kernel(program, "highlights_normalize_reduce_second");
  gd->kernel_highlights_remosaic_and_replace = dt_opencl_create_kernel(program, "remosaic_and_replace");
  gd->kernel_highlights_remosaic_and_replace_xtrans
      = dt_opencl_create_kernel(program, "remosaic_and_replace_xtrans");
  gd->kernel_highlights_remosaic_and_replace_passthrough
      = dt_opencl_create_kernel(program, "remosaic_and_replace_passthrough");
  gd->kernel_highlights_box_blur = dt_opencl_create_kernel(program, "box_blur_5x5");
  gd->kernel_highlights_guide_laplacians = dt_opencl_create_kernel(program, "guide_laplacians");
  gd->kernel_highlights_diffuse_color = dt_opencl_create_kernel(program, "diffuse_color");
  gd->kernel_highlights_false_color = dt_opencl_create_kernel(program, "highlights_false_color");
  gd->kernel_interpolate_bilinear = dt_opencl_create_kernel(program, "interpolate_bilinear");

  const int wavelets = 35; // bspline.cl, from programs.conf
  gd->kernel_filmic_bspline_horizontal = dt_opencl_create_kernel(wavelets, "blur_2D_Bspline_horizontal");
  gd->kernel_filmic_bspline_vertical = dt_opencl_create_kernel(wavelets, "blur_2D_Bspline_vertical");
  gd->kernel_filmic_bspline_horizontal_local
      = dt_opencl_create_kernel(wavelets, "blur_2D_Bspline_horizontal_local");
  gd->kernel_filmic_bspline_vertical_local = dt_opencl_create_kernel(wavelets, "blur_2D_Bspline_vertical_local");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_highlights_global_data_t *gd = (dt_iop_highlights_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_sparse_chol_update_level);
  dt_opencl_free_kernel(gd->kernel_sparse_chol_final_level);
  dt_opencl_free_kernel(gd->kernel_sparse_chol_fwd_level);
  dt_opencl_free_kernel(gd->kernel_sparse_chol_bwd_level);
  dt_opencl_free_kernel(gd->kernel_hl_cfa_steer);
  dt_opencl_free_kernel(gd->kernel_hl_cfa_down);
  dt_opencl_free_kernel(gd->kernel_hl_cfa_box);
  dt_opencl_free_kernel(gd->kernel_hl_cfa_grad);
  dt_opencl_free_kernel(gd->kernel_hl_cfa_tensor);
  dt_opencl_free_kernel(gd->kernel_hl_cfa_gnorm);
  dt_opencl_free_kernel(gd->kernel_hl_cfa_weights);
  dt_opencl_free_kernel(gd->kernel_hl_cfa_jacobi);
  dt_opencl_free_kernel(gd->kernel_hl_cfa_jacobi_block);
  dt_opencl_free_kernel(gd->kernel_hl_fill_down);
  dt_opencl_free_kernel(gd->kernel_hl_fill_seed);
  dt_opencl_free_kernel(gd->kernel_hl_fill_seed_up);
  dt_opencl_free_kernel(gd->kernel_hl_fill_jacobi);
  dt_opencl_free_kernel(gd->kernel_hl_fill_jacobi_block);
  dt_opencl_free_kernel(gd->kernel_hl_fill_up);
  dt_opencl_free_kernel(gd->kernel_hl_cf_lref_partials);
  dt_opencl_free_kernel(gd->kernel_hl_cf_pack_joint);
  dt_opencl_free_kernel(gd->kernel_hl_cf_fit_joint);
  dt_opencl_free_kernel(gd->kernel_hl_cf_eval_joint);
  dt_opencl_free_kernel(gd->kernel_hl_cf_pack_pair);
  dt_opencl_free_kernel(gd->kernel_hl_cf_fit_pair);
  dt_opencl_free_kernel(gd->kernel_hl_cf_eval_pair);
  dt_opencl_free_kernel(gd->kernel_hl_cf_pack_deepmask);
  dt_opencl_free_kernel(gd->kernel_hl_cf_eval_deep);
  dt_opencl_free_kernel(gd->kernel_hl_buf_to_img);
  dt_opencl_free_kernel(gd->kernel_hl_hf_pack);
  dt_opencl_free_kernel(gd->kernel_hl_hf_fit);
  dt_opencl_free_kernel(gd->kernel_hl_hf_energy);
  dt_opencl_free_kernel(gd->kernel_hl_hf_eval);
  dt_opencl_free_kernel(gd->kernel_hl_hf_damp);
  dt_opencl_free_kernel(gd->kernel_hl_soft_floor);
  dt_opencl_free_kernel(gd->kernel_hl_hard_floor);
  dt_opencl_free_kernel(gd->kernel_hl_lsb_hole);
  dt_opencl_free_kernel(gd->kernel_hl_ratio_plane);
  dt_opencl_free_kernel(gd->kernel_hl_dome_down);
  dt_opencl_free_kernel(gd->kernel_hl_dome_blend);
  dt_opencl_free_kernel(gd->kernel_hl_core_floor);
  dt_opencl_free_kernel(gd->kernel_hl_cmean_reduce);
  dt_opencl_free_kernel(gd->kernel_hl_ratio_cmean_blend);
  dt_opencl_free_kernel(gd->kernel_hl_clip0_rehue);
  dt_opencl_free_kernel(gd->kernel_hl_ring_vote);
  dt_opencl_free_kernel(gd->kernel_hl_cgrad_plateau);
  dt_opencl_free_kernel(gd->kernel_hl_cgrad_guard);
  dt_opencl_free_kernel(gd->kernel_hl_cgrad_anchor);
  dt_opencl_free_kernel(gd->kernel_hl_cgrad_share);
  dt_opencl_free_kernel(gd->kernel_hl_cgrad_store);
  dt_opencl_free_kernel(gd->kernel_hl_cgrad_gate);
  dt_opencl_free_kernel(gd->kernel_hl_cgrad_reproject);
  dt_opencl_free_kernel(gd->kernel_hl_cgrad_hole1c);
  dt_opencl_free_kernel(gd->kernel_hl_cgrad_write1c);
  dt_opencl_free_kernel(gd->kernel_hl_pde_init);
  dt_opencl_free_kernel(gd->kernel_hl_mask_to_img1);
  dt_opencl_free_kernel(gd->kernel_hl_core_blend);
  dt_opencl_free_kernel(gd->kernel_hl_pde_rhs);
  dt_opencl_free_kernel(gd->kernel_hl_pde_scatter);
  dt_opencl_free_kernel(gd->kernel_hl_aniso_prep);
  dt_opencl_free_kernel(gd->kernel_hl_box3);
  dt_opencl_free_kernel(gd->kernel_hl_grad_reduce);
  dt_opencl_free_kernel(gd->kernel_hl_aniso_tensor);
  dt_opencl_free_kernel(gd->kernel_hl_aniso_weights);
  dt_opencl_free_kernel(gd->kernel_hl_aniso_reassemble);
  dt_opencl_free_kernel(gd->kernel_hl_aniso_rhs);
  dt_opencl_free_kernel(gd->kernel_hl_aniso_scatter);
  dt_opencl_free_kernel(gd->kernel_hl_knee_bin);
  dt_opencl_free_kernel(gd->kernel_hl_knee_jmom);
  dt_opencl_free_kernel(gd->kernel_hl_knee_pmom);
  dt_opencl_free_kernel(gd->kernel_hl_knee_joint_reg);
  dt_opencl_free_kernel(gd->kernel_hl_knee_pair_reg);
  dt_opencl_free_kernel(gd->kernel_hl_knee_apply);
  dt_opencl_free_kernel(gd->kernel_hl_mask_pack);
  dt_opencl_free_kernel(gd->kernel_hl_region_gather);
  dt_opencl_free_kernel(gd->kernel_hl_region_scatter);
  dt_opencl_free_kernel(gd->kernel_hl_region_stats);
  dt_opencl_free_kernel(gd->kernel_hl_need_self);
  dt_opencl_free_kernel(gd->kernel_hl_knee_apply_interp);
  dt_opencl_free_kernel(gd->kernel_hl_cg_embed);
  dt_opencl_free_kernel(gd->kernel_hl_cg_op);
  dt_opencl_free_kernel(gd->kernel_hl_cg_r0);
  dt_opencl_free_kernel(gd->kernel_hl_cg_beta);
  dt_opencl_free_kernel(gd->kernel_hl_relu);
  dt_opencl_free_kernel(gd->kernel_hl_cg_r1);
  dt_opencl_free_kernel(gd->kernel_hl_cg_ap);
  dt_opencl_free_kernel(gd->kernel_hl_cg_update);
  dt_opencl_free_kernel(gd->kernel_hl_aniso_pyr_down);
  dt_opencl_free_kernel(gd->kernel_hl_pyr_getc);
  dt_opencl_free_kernel(gd->kernel_hl_pyr_getc4);
  dt_opencl_free_kernel(gd->kernel_hl_pyr_putc4);
  dt_opencl_free_kernel(gd->kernel_hl_pyr_project);
  dt_opencl_free_kernel(gd->kernel_hl_aniso_obs_full);
  dt_opencl_free_kernel(gd->kernel_hl_aniso_obs_flags);
  dt_opencl_free_kernel(gd->kernel_hl_window_pack);
  dt_opencl_free_kernel(gd->kernel_hl_window_unpack);
  dt_opencl_free_kernel(gd->kernel_hl_pyr_putc);
  dt_opencl_free_kernel(gd->kernel_hl_aniso_iter);
  dt_opencl_free_kernel(gd->kernel_hl_aniso_iter_block);
  dt_opencl_free_kernel(gd->kernel_hl_aniso_splat);
  dt_opencl_free_kernel(gd->kernel_highlights_4f_clip);
  dt_opencl_free_kernel(gd->kernel_highlights_1f_lch_bayer);
  dt_opencl_free_kernel(gd->kernel_highlights_1f_lch_xtrans);
  dt_opencl_free_kernel(gd->kernel_highlights_1f_clip);
  dt_opencl_free_kernel(gd->kernel_highlights_bilinear_and_mask);
  dt_opencl_free_kernel(gd->kernel_highlights_bilinear_and_mask_xtrans);
  dt_opencl_free_kernel(gd->kernel_highlights_bilinear_and_mask_passthrough);
  dt_opencl_free_kernel(gd->kernel_highlights_normalize_reduce_first);
  dt_opencl_free_kernel(gd->kernel_highlights_normalize_reduce_first_xtrans);
  dt_opencl_free_kernel(gd->kernel_highlights_normalize_reduce_first_passthrough);
  dt_opencl_free_kernel(gd->kernel_highlights_normalize_reduce_second);
  dt_opencl_free_kernel(gd->kernel_highlights_remosaic_and_replace);
  dt_opencl_free_kernel(gd->kernel_highlights_remosaic_and_replace_xtrans);
  dt_opencl_free_kernel(gd->kernel_highlights_remosaic_and_replace_passthrough);
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

  // harmonic transposition reads noise_level (regrain) and solid_color (all-clip core reaction);
  // iterations and scales belong to the a-trous guided laplacians only
  gtk_widget_set_visible(g->noise_level,
                         raw && (mode == DT_IOP_HIGHLIGHTS_LAPLACIAN || mode == DT_IOP_HIGHLIGHTS_HARMONIC));
  gtk_widget_set_visible(g->iterations, raw && mode == DT_IOP_HIGHLIGHTS_LAPLACIAN);
  gtk_widget_set_visible(g->scales, raw && mode == DT_IOP_HIGHLIGHTS_LAPLACIAN);
  gtk_widget_set_visible(g->solid_color,
                         raw && (mode == DT_IOP_HIGHLIGHTS_LAPLACIAN || mode == DT_IOP_HIGHLIGHTS_HARMONIC));

  dt_bauhaus_widget_set_quad_visibility(g->clip, israw);
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;
  const dt_image_t *const image = &self->dev->image_storage;
  const gboolean supported = _highlights_image_supported(image);
  // Auto-enable only on raw colorimetry (raw / sRAW, not monochrome); on rendered RGB it is opt-in.
  self->default_enabled = enable(image);

  // Show the on/off button for any eligible image (opt-in on rendered RGB / sRAW). Neuter it only where
  // the module self-disables (mono-raw / greyscale), and even then keep it if already enabled (history
  // copy & paste from a RAW image) so the user can turn it back off.
  self->hide_enable_button = !supported && !self->enabled;
  gtk_stack_set_visible_child_name(GTK_STACK(self->widget), supported ? "default" : "monochrome");

  // capability entries, added once (moved here from reload_defaults so it never touches widgets off
  // the GUI thread / on a widget-less export dev)
  if(dt_bauhaus_combobox_length(g->mode) < DT_IOP_HIGHLIGHTS_LAPLACIAN + 1)
    dt_bauhaus_combobox_add_full(g->mode, _("guided laplacians"), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT,
                                 GINT_TO_POINTER(DT_IOP_HIGHLIGHTS_LAPLACIAN), NULL, TRUE);
  if(dt_bauhaus_combobox_length(g->mode) < DT_IOP_HIGHLIGHTS_HARMONIC + 1)
    dt_bauhaus_combobox_add_full(g->mode, _("harmonic transposition"), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT,
                                 GINT_TO_POINTER(DT_IOP_HIGHLIGHTS_HARMONIC), NULL, TRUE);

  dt_bauhaus_widget_set_quad_active(g->clip, FALSE);
  g->show_visualize = FALSE;
  gui_changed(self, NULL, NULL);
}

void reload_defaults(dt_iop_module_t *module)
{
  // we might be called from presets update infrastructure => there is no image
  if(!module->dev || module->dev->image_storage.id == -1) return;

  // Auto-enable only on raw colorimetry (raw / sRAW, not monochrome). Availability is broader: the
  // button is shown (opt-in) for any eligible image and hidden only on a non-mosaiced non-4-channel
  // image (mono-raw / greyscale), where the module self-disables -- there is nothing to reconstruct.
  module->default_enabled = enable(&module->dev->image_storage);
  module->hide_enable_button = !_highlights_image_supported(&module->dev->image_storage);
  dt_iop_fmt_log(module, "reload_defaults: class=%s default_enabled=%d hidden=%d",
                 dt_image_pipe_class_name(dt_image_pipe_class(&module->dev->image_storage)),
                 module->default_enabled, module->hide_enable_button);
  // Stack visibility and the "guided laplacians" capability entry are set from default_enabled in
  // gui_update() (which already does the stack), so reload_defaults() stays params-only.
}

static void _visualize_callback(GtkWidget *quad, gpointer user_data)
{
  if(dt_gui_widgets_suppressed()) return;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;

  // if blend module is displaying mask do not display it here
  if(self->request_mask_display != DT_DEV_PIXELPIPE_DISPLAY_NONE)
    self->request_mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;

  g->show_visualize = dt_bauhaus_widget_get_quad_active(quad);

  if(g->show_visualize) self->request_mask_display = DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU;

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
  GtkWidget *box_raw = self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);

  g->mode = dt_bauhaus_combobox_from_params(self, "mode");
  gtk_widget_set_tooltip_text(g->mode, _("highlight reconstruction method"));

  g->clip = dt_bauhaus_slider_from_params(self, "clip");
  dt_bauhaus_slider_set_digits(g->clip, 3);
  gtk_widget_set_tooltip_text(g->clip, _("manually adjust the clipping threshold against "
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
  gtk_widget_set_tooltip_text(g->solid_color,
                              _("increase if magenta highlights don't get fully corrected.\n"
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
