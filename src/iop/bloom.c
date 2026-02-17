/*
    This file is part of darktable,
    Copyright (C) 2009-2013, 2016 johannes hanika.
    Copyright (C) 2010-2011 Bruce Guenter.
    Copyright (C) 2010-2012 Henrik Andersson.
    Copyright (C) 2010 jan rinze.
    Copyright (C) 2010 Pascal de Bruijn.
    Copyright (C) 2011 Antony Dovgal.
    Copyright (C) 2011 Olivier Tribout.
    Copyright (C) 2011 Robert Bieber.
    Copyright (C) 2011-2016, 2019 Tobias Ellinghaus.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012, 2014, 2016-2017 Ulrich Pegelow.
    Copyright (C) 2013 Simon Spannagel.
    Copyright (C) 2014-2016 Roman Lebedev.
    Copyright (C) 2015, 2018, 2020-2022 Pascal Obry.
    Copyright (C) 2015 Pedro Côrte-Real.
    Copyright (C) 2017 Heiko Bauke.
    Copyright (C) 2018, 2020, 2022-2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2018 Edgardo Hoszowski.
    Copyright (C) 2018 Maurizio Paglia.
    Copyright (C) 2018 rawfiner.
    Copyright (C) 2019 Andreas Schneider.
    Copyright (C) 2020 Aldric Renaudin.
    Copyright (C) 2020, 2022 Diederik Ter Rahe.
    Copyright (C) 2020 Hubert Kowalski.
    Copyright (C) 2020-2021 Ralf Brown.
    Copyright (C) 2022 Hanno Schwalm.
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
#include "config.h"
#endif
#include "bauhaus/bauhaus.h"
#include "common/box_filters.h"
#include "common/imagebuf.h"
#include "common/math.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "develop/tiling.h"

#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <assert.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define NUM_BUCKETS 4 /* OpenCL bucket chain size for tmp buffers; minimum 2 */

DT_MODULE_INTROSPECTION(1, dt_iop_bloom_params_t)

typedef struct dt_iop_bloom_params_t
{
  float size;       // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 20.0
  float threshold;  // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 90.0
  float strength;   // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 25.0
} dt_iop_bloom_params_t;

typedef struct dt_iop_bloom_gui_data_t
{
  GtkWidget *size, *threshold, *strength; // size,threshold,strength
} dt_iop_bloom_gui_data_t;

typedef struct dt_iop_bloom_data_t
{
  float size;
  float threshold;
  float strength;
} dt_iop_bloom_data_t;

const char *name()
{
  return _("bloom");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("apply Orton effect for a dreamy aetherical look"),
                                      _("creative"),
                                      _("non-linear, Lab, display-referred"),
                                      _("non-linear, Lab"),
                                      _("non-linear, Lab, display-referred"));
}


int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_DEPRECATED;
}

int default_group()
{
  return IOP_GROUP_EFFECTS;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_LAB;
}

int process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_bloom_data_t *const data = (dt_iop_bloom_data_t *)piece->data;
  if (!dt_iop_have_required_input_format(4 /*we need full-color pixels*/, self, piece->colors,
                                         ivoid, ovoid, roi_in, roi_out))
    return 0; // image has been copied through to output and module's trouble flag has been updated

  float *restrict blurlightness;
  if (dt_iop_alloc_image_buffers(self, roi_in, roi_out, 1, &blurlightness, 0))
  {
    // out of memory, so just copy image through to output
    dt_iop_copy_image_roi(ovoid, ivoid, piece->colors, roi_in, roi_out, TRUE);
    return 1;
  }

  const float *const restrict in = DT_IS_ALIGNED((float *)ivoid);
  float *const restrict out = DT_IS_ALIGNED((float *)ovoid);
  const size_t npixels = (size_t)roi_out->width * roi_out->height;

  /* gather light by threshold */
  const int rad = 256.0f * (fmin(100.0f, data->size + 1.0f) / 100.0f);
  const float _r = ceilf(rad * roi_in->scale);
  const int radius = MIN(256.0f, _r);

  const float scale = 1.0f / exp2f(-1.0f * (fmin(100.0f, data->strength + 1.0f) / 100.0f));

  const float threshold = data->threshold;
/* get the thresholded lights into buffer */
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(npixels, scale, threshold) \
  shared(blurlightness) \
  dt_omp_sharedconst(in) \
  schedule(static)
#endif
  for(size_t k = 0; k < npixels; k++)
  {
    const float L = in[4*k] * scale;
    blurlightness[k] = (L > threshold) ? L : 0.0f;
  }

  /* horizontal blur into memchannel lightness */
  const int range = 2 * radius + 1;
  const int hr = range / 2;

  if(dt_box_mean(blurlightness, roi_out->height, roi_out->width, 1, hr, BOX_ITERATIONS) != 0)
  {
    dt_pixelpipe_cache_free_align(blurlightness);
    return 1;
  }

/* screen blend lightness with original */
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(npixels) \
  shared(blurlightness) \
  dt_omp_sharedconst(in, out) \
  schedule(static)
#endif
  for(size_t k = 0; k < npixels; k++)
  {
    out[4*k+0] = 100.0f - (((100.0f - in[4*k]) * (100.0f - blurlightness[k])) / 100.0f); // Screen blend
    out[4*k+1] = in[4*k+1];
    out[4*k+2] = in[4*k+2];
    out[4*k+3] = in[4*k+3];
  }
  dt_pixelpipe_cache_free_align(blurlightness);

//  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
//    dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);

  return 0;
}

void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  const dt_iop_bloom_data_t *d = (dt_iop_bloom_data_t *)piece->data;

  const int rad = 256.0f * (fmin(100.0f, d->size + 1.0f) / 100.0f);
  const float _r = ceilf(rad * roi_in->scale);
  const int radius = MIN(256.0f, _r);

  tiling->factor = 2.0f + 0.25f + 0.05f; // in + out + blurlightness + slice for dt_box_mean
  tiling->factor_cl = 2.0f + NUM_BUCKETS * 0.25f; // in + out + NUM_BUCKETS * 0.25 tmp
  tiling->maxbuf = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = 5 * radius; // This is a guess. TODO: check if that's sufficiently large
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_bloom_params_t *p = (dt_iop_bloom_params_t *)p1;
  dt_iop_bloom_data_t *d = (dt_iop_bloom_data_t *)piece->data;

  d->strength = p->strength;
  d->size = p->size;
  d->threshold = p->threshold;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc_align(sizeof(dt_iop_bloom_data_t));
  piece->data_size = sizeof(dt_iop_bloom_data_t);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_free_align(piece->data);
  piece->data = NULL;
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_bloom_gui_data_t *g = IOP_GUI_ALLOC(bloom);

  g->size = dt_bauhaus_slider_from_params(self, N_("size"));
  dt_bauhaus_slider_set_format(g->size, "%");
  gtk_widget_set_tooltip_text(g->size, _("the size of bloom"));

  g->threshold = dt_bauhaus_slider_from_params(self, N_("threshold"));
  dt_bauhaus_slider_set_format(g->threshold, "%");
  gtk_widget_set_tooltip_text(g->threshold, _("the threshold of light"));

  g->strength = dt_bauhaus_slider_from_params(self, N_("strength"));
  dt_bauhaus_slider_set_format(g->strength, "%");
  gtk_widget_set_tooltip_text(g->strength, _("the strength of bloom"));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
