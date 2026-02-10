/*
    This file is part of darktable,
    Copyright (C) 2011-2021 darktable developers.

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
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "bauhaus/bauhaus.h"
#include "common/box_filters.h"
#include "common/colorspaces.h"
#include "common/imagebuf.h"
#include "common/math.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/tiling.h"
#include "dtgtk/resetlabel.h"

#include "gui/gtk.h"
#include "iop/iop_api.h"
#include <gtk/gtk.h>
#include <inttypes.h>

#if defined(__SSE__)
#include <xmmintrin.h>
#endif

#define MAX_RADIUS 32

DT_MODULE_INTROSPECTION(1, dt_iop_soften_params_t)

typedef struct dt_iop_soften_params_t
{
  float size;       // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 50.0
  float saturation; // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 100.0
  float brightness; // $MIN: -2.0 $MAX: 2.0 $DEFAULT: 0.33
  float amount;     // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 50.0 $DESCRIPTION: "mix"
} dt_iop_soften_params_t;

typedef struct dt_iop_soften_gui_data_t
{
  GtkWidget *size, *saturation, *brightness, *amount;
} dt_iop_soften_gui_data_t;

typedef struct dt_iop_soften_data_t
{
  float size;
  float saturation;
  float brightness;
  float amount;
} dt_iop_soften_data_t;

const char *name()
{
  return _("soften");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_DEPRECATED;
}

int default_group()
{
  return IOP_GROUP_EFFECTS;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("create a softened image using the Orton effect"),
                                      _("creative"),
                                      _("linear, RGB, display-referred"),
                                      _("linear, RGB"),
                                      _("linear, RGB, display-referred"));
}

int process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_soften_data_t *const d = (const dt_iop_soften_data_t *const)piece->data;

  if (!dt_iop_have_required_input_format(4 /*we need full-color pixels*/, self, piece->colors,
                                         ivoid, ovoid, roi_in, roi_out))
    return 0; // image has been copied through to output and module's trouble flag has been updated

  const float brightness = 1.0 / exp2f(-d->brightness);
  const float saturation = d->saturation / 100.0;

  const float *const restrict in = (const float *const)ivoid;
  float *const restrict out = (float *const)ovoid;

  const size_t npixels = (size_t)roi_out->width * roi_out->height;
/* create overexpose image and then blur */
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(brightness, npixels, saturation) \
  dt_omp_sharedconst(in, out) \
  schedule(static)
#endif
  for(size_t k = 0; k < 4 * npixels; k += 4)
  {
    float h, s, l;
    rgb2hsl(&in[k], &h, &s, &l);
    s *= saturation;
    l *= brightness;
    hsl2rgb(&out[k], h, CLIP(s), CLIP(l));
  }

  const float w = piece->iwidth;
  const float h = piece->iheight;
  const int mrad = sqrt(w * w + h * h) * 0.01;
  const int rad = mrad * (fmin(100.0, d->size + 1) / 100.0);
  const int radius = MIN(mrad, ceilf(rad * roi_in->scale));

  if(dt_box_mean(out, roi_out->height, roi_out->width, 4, radius, BOX_ITERATIONS) != 0)
  {
    return 1;
  }

  const float amt = d->amount / 100.0f;
  dt_iop_image_linear_blend(out, amt, in, roi_out->width, roi_out->height, 4);

  return 0;
}

void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  dt_iop_soften_data_t *d = (dt_iop_soften_data_t *)piece->data;

  const float w = piece->iwidth;
  const float h = piece->iheight;
  const int mrad = sqrt(w * w + h * h) * 0.01f;

  const int rad = mrad * (fmin(100.0f, d->size + 1) / 100.0f);
  const int radius = MIN(mrad, ceilf(rad * roi_in->scale));

  /* sigma-radius correlation to match opencl vs. non-opencl. identified by numerical experiments but
   * unproven. ask me if you need details. ulrich */
  const float sigma = sqrtf((radius * (radius + 1) * BOX_ITERATIONS + 2) / 3.0f);
  const int wdh = ceilf(3.0f * sigma);

  tiling->factor = 2.1f; // in + out + small slice for box_mean
  tiling->factor_cl = 3.0f; // in + out + tmp
  tiling->maxbuf = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = wdh;
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_soften_params_t *p = (dt_iop_soften_params_t *)p1;
  dt_iop_soften_data_t *d = (dt_iop_soften_data_t *)piece->data;

  d->size = p->size;
  d->saturation = p->saturation;
  d->brightness = p->brightness;
  d->amount = p->amount;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_soften_data_t));
  piece->data_size = sizeof(dt_iop_soften_data_t);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_soften_gui_data_t *g = IOP_GUI_ALLOC(soften);

  g->size = dt_bauhaus_slider_from_params(self, N_("size"));
  dt_bauhaus_slider_set_format(g->size, "%");
  gtk_widget_set_tooltip_text(g->size, _("the size of blur"));

  g->saturation = dt_bauhaus_slider_from_params(self, N_("saturation"));
  dt_bauhaus_slider_set_format(g->saturation, "%");
  gtk_widget_set_tooltip_text(g->saturation, _("the saturation of blur"));

  g->brightness = dt_bauhaus_slider_from_params(self, N_("brightness"));
  dt_bauhaus_slider_set_format(g->brightness, _(" EV"));
  gtk_widget_set_tooltip_text(g->brightness, _("the brightness of blur"));

  g->amount = dt_bauhaus_slider_from_params(self, "amount");
  dt_bauhaus_slider_set_format(g->amount, "%");
  gtk_widget_set_tooltip_text(g->amount, _("the mix of effect"));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
