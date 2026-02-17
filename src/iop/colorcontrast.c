/*
    This file is part of darktable,
    Copyright (C) 2010 Bruce Guenter.
    Copyright (C) 2010-2013 johannes hanika.
    Copyright (C) 2011-2012 Henrik Andersson.
    Copyright (C) 2011 Jérémy Rosen.
    Copyright (C) 2011 Robert Bieber.
    Copyright (C) 2011 Rostyslav Pidgornyi.
    Copyright (C) 2011 Sergey Astanin.
    Copyright (C) 2011-2014, 2016, 2019 Tobias Ellinghaus.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012, 2014 Ulrich Pegelow.
    Copyright (C) 2013 Simon Spannagel.
    Copyright (C) 2014-2016 Roman Lebedev.
    Copyright (C) 2015 Pedro Côrte-Real.
    Copyright (C) 2017 Heiko Bauke.
    Copyright (C) 2018, 2020, 2022-2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2018 Edgardo Hoszowski.
    Copyright (C) 2018 Maurizio Paglia.
    Copyright (C) 2018-2020, 2022 Pascal Obry.
    Copyright (C) 2018 rawfiner.
    Copyright (C) 2019 Andreas Schneider.
    Copyright (C) 2020 Aldric Renaudin.
    Copyright (C) 2020 Diederik Ter Rahe.
    Copyright (C) 2020-2021 Hubert Kowalski.
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
/* -*- Mode: c; c-basic-offset: 2; -*- */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "bauhaus/bauhaus.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"

#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <assert.h>
#include <gtk/gtk.h>
#include <stdlib.h>

#if defined(__SSE__)
#include <xmmintrin.h>
#endif

DT_MODULE_INTROSPECTION(2, dt_iop_colorcontrast_params_t)

typedef struct dt_iop_colorcontrast_params1_t
{
  float a_steepness;
  float a_offset;
  float b_steepness;
  float b_offset;
} dt_iop_colorcontrast_params1_t;

typedef struct dt_iop_colorcontrast_params_t
{
  float a_steepness; // $MIN: 0.0 $MAX: 5.0 $DEFAULT: 1.0 $DESCRIPTION: "green-magenta contrast"
  float a_offset;
  float b_steepness; // $MIN: 0.0 $MAX: 5.0 $DEFAULT: 1.0 $DESCRIPTION: "blue-yellow contrast"
  float b_offset;
  int unbound;       // $DEFAULT: 1
} dt_iop_colorcontrast_params_t;

typedef struct dt_iop_colorcontrast_gui_data_t
{
  // whatever you need to make your gui happy.
  // stored in self->gui_data
  GtkBox *vbox;
  GtkWidget *a_scale; // this is needed by gui_update
  GtkWidget *b_scale;
} dt_iop_colorcontrast_gui_data_t;

typedef struct dt_iop_colorcontrast_data_t
{
  // this is stored in the pixelpipeline after a commit (not the db),
  // you can do some precomputation and get this data in process().
  // stored in piece->data
  float a_steepness;
  float a_offset;
  float b_steepness;
  float b_offset;
  int unbound;
} dt_iop_colorcontrast_data_t;

const char *name()
{
  return _("color contrast");
}

const char *aliases()
{
  return _("saturation");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("increase saturation and separation between\n"
                                        "opposite colors"),
                                      _("creative"),
                                      _("non-linear, Lab, display-referred"),
                                      _("non-linear, Lab"),
                                      _("non-linear, Lab, display-referred"));
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_DEPRECATED;
}

int default_group()
{
  return IOP_GROUP_COLOR;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_LAB;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version == 1 && new_version == 2)
  {
    const dt_iop_colorcontrast_params1_t *old = old_params;
    dt_iop_colorcontrast_params_t *new = new_params;

    new->a_steepness = old->a_steepness;
    new->a_offset = old->a_offset;
    new->b_steepness = old->b_steepness;
    new->b_offset = old->b_offset;
    new->unbound = 0;
    return 0;
  }
  return 1;
}

#ifdef _OPENMP
#pragma omp declare simd aligned(in,out:64) aligned(slope,offset,low,high)
#endif
static inline void clamped_scaling(float *const restrict out, const float *const restrict in,
                                   const dt_aligned_pixel_t slope, const dt_aligned_pixel_t offset,
                                   const dt_aligned_pixel_t low, const dt_aligned_pixel_t high)
{
  for_each_channel(c,dt_omp_nontemporal(out))
    out[c] = CLAMPS(in[c] * slope[c] + offset[c], low[c], high[c]);
}

int process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  // this is called for preview and full pipe separately, each with its own pixelpipe piece.

  // get our data struct:
  const dt_iop_colorcontrast_params_t *const d = (dt_iop_colorcontrast_params_t *)piece->data;

  // how many colors in our buffer?
  if (!dt_iop_have_required_input_format(4 /*we need full-color pixels*/, self, piece->colors,
                                         ivoid, ovoid, roi_in, roi_out))
    return 0; // image has been copied through to output and module's trouble flag has been updated

  const float *const restrict in = DT_IS_ALIGNED((const float *const)ivoid);
  float *const restrict out = DT_IS_ALIGNED((float *const)ovoid);
  const size_t npixels = (size_t)roi_out->width * roi_out->height;

  const dt_aligned_pixel_t slope = { 1.0f, d->a_steepness, d->b_steepness, 1.0f };
  const dt_aligned_pixel_t offset = { 0.0f, d->a_offset, d->b_offset, 0.0f };
  const dt_aligned_pixel_t lowlimit = { -INFINITY, -128.0f, -128.0f, -INFINITY };
  const dt_aligned_pixel_t highlimit = { INFINITY, 128.0f, 128.0f, INFINITY };

  if(d->unbound)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(in, out, npixels, slope, offset) \
    schedule(static)
#endif
    for(size_t k = 0; k < (size_t)4 * npixels; k += 4)
    {
      for_each_channel(c,dt_omp_nontemporal(out))
      {
        out[k + c] = (in[k + c] * slope[c]) + offset[c];
      }
    }
  }
  else
  {

#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(in, out, npixels, slope, offset, lowlimit, highlimit) \
    schedule(static)
#endif
    for(size_t k = 0; k < npixels; k ++)
    {
      // the inner per-pixel loop needs to be declared in a separate vectorizable function to convince the
      // compiler that it doesn't need to check for overlap or misalignment of the buffers for *every* pixel,
      // which actually makes the code slower than not vectorizing....
      clamped_scaling(out + 4*k, in + 4*k, slope, offset, lowlimit, highlimit);
    }
  }

  return 0;
}

/** commit is the synch point between core and gui, so it copies params to pipe data. */
void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorcontrast_params_t *p = (dt_iop_colorcontrast_params_t *)params;
  dt_iop_colorcontrast_data_t *d = (dt_iop_colorcontrast_data_t *)piece->data;
  d->a_steepness = p->a_steepness;
  d->a_offset = p->a_offset;
  d->b_steepness = p->b_steepness;
  d->b_offset = p->b_offset;
  d->unbound = p->unbound;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc_align(sizeof(dt_iop_colorcontrast_data_t));
  piece->data_size = sizeof(dt_iop_colorcontrast_data_t);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_free_align(piece->data);
  piece->data = NULL;
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_colorcontrast_gui_data_t *g = (dt_iop_colorcontrast_gui_data_t *)self->gui_data;
  dt_iop_colorcontrast_params_t *p = (dt_iop_colorcontrast_params_t *)self->params;
  dt_bauhaus_slider_set(g->a_scale, p->a_steepness);
  dt_bauhaus_slider_set(g->b_scale, p->b_steepness);
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_colorcontrast_gui_data_t *g = IOP_GUI_ALLOC(colorcontrast);

  g->a_scale = dt_bauhaus_slider_from_params(self, "a_steepness");
  gtk_widget_set_tooltip_text(g->a_scale, _("steepness of the a* curve in Lab\nlower values desaturate greens and magenta while higher saturate them"));

  g->b_scale = dt_bauhaus_slider_from_params(self, "b_steepness");
  gtk_widget_set_tooltip_text(g->b_scale, _("steepness of the b* curve in Lab\nlower values desaturate blues and yellows while higher saturate them"));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
