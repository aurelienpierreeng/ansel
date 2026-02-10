/*
    This file is part of darktable,
    Copyright (C) 2012-2021 darktable developers.

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
#include "common/bilateral.h"
#include "common/bilateralcl.h"
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
#include <stdlib.h>
#include <string.h>

#include <gtk/gtk.h>
#include <inttypes.h>

#define REDUCESIZE 64

DT_MODULE_INTROSPECTION(3, dt_iop_global_tonemap_params_t)

typedef enum _iop_operator_t
{
  OPERATOR_REINHARD, // $DESCRIPTION: "reinhard"
  OPERATOR_FILMIC,   // $DESCRIPTION: "filmic"
  OPERATOR_DRAGO     // $DESCRIPTION: "drago"
} _iop_operator_t;

typedef struct dt_iop_global_tonemap_params_t
{
  _iop_operator_t operator; // $DEFAULT: OPERATOR_DRAGO
  struct
  {
    float bias;      // $MIN: 0.5 $MAX: 1 $DEFAULT: 0.85 $DESCRIPTION: "bias"
    float max_light; // cd/m2 $MIN: 1 $MAX: 500 $DEFAULT: 100.0 $DESCRIPTION: "target"
  } drago;
  float detail; // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0
} dt_iop_global_tonemap_params_t;

typedef struct dt_iop_global_tonemap_data_t
{
  _iop_operator_t operator;
  struct
  {
    float bias;
    float max_light; // cd/m2
  } drago;
  float detail;
} dt_iop_global_tonemap_data_t;

typedef struct dt_iop_global_tonemap_gui_data_t
{
  GtkWidget *operator;
  struct
  {
    GtkWidget *bias;
    GtkWidget *max_light;
  } drago;
  GtkWidget *detail;
  float lwmax;
  uint64_t hash;
} dt_iop_global_tonemap_gui_data_t;

const char *name()
{
  return _("global tonemap");
}

const char *deprecated_msg()
{
  return _("this module is deprecated. please use the filmic rgb module instead.");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_DEPRECATED;
}

int default_group()
{
  return IOP_GROUP_TONES;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_LAB;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version < 3 && new_version == 3)
  {
    dt_iop_global_tonemap_params_t *o = (dt_iop_global_tonemap_params_t *)old_params;
    dt_iop_global_tonemap_params_t *n = (dt_iop_global_tonemap_params_t *)new_params;

    // only appended detail, 0 is no-op
    memcpy(n, o, sizeof(dt_iop_global_tonemap_params_t) - sizeof(float));
    n->detail = 0.0f;
    return 0;
  }
  return 1;
}

static inline void process_reinhard(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                    const void *const ivoid, void *const ovoid,
                                    const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                                    dt_iop_global_tonemap_data_t *data)
{
  float *in = (float *)ivoid;
  float *out = (float *)ovoid;
  const int ch = piece->colors;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ch, roi_out) \
  shared(in, out, data) \
  schedule(static)
#endif
  for(size_t k = 0; k < (size_t)roi_out->width * roi_out->height; k++)
  {
    float *inp = in + ch * k;
    float *outp = out + ch * k;
    float l = inp[0] / 100.0;
    outp[0] = 100.0 * (l / (1.0f + l));
    outp[1] = inp[1];
    outp[2] = inp[2];
  }
}

static inline void process_drago(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                 const void *const ivoid, void *const ovoid, const dt_iop_roi_t *const roi_in,
                                 const dt_iop_roi_t *const roi_out, dt_iop_global_tonemap_data_t *data)
{
  dt_iop_global_tonemap_gui_data_t *g = (dt_iop_global_tonemap_gui_data_t *)self->gui_data;
  float *in = (float *)ivoid;
  float *out = (float *)ovoid;
  const int ch = piece->colors;

  /* precalcs */
  const float eps = 0.0001f;
  float lwmax;
  float tmp_lwmax = NAN;

  // Drago needs the absolute Lmax value of the image. In pixelpipe FULL we can not reliably get this value
  // as the pixelpipe might only see part of the image (region of interest). Therefore we try to get lwmax from
  // the PREVIEW pixelpipe which luckily stores it for us.
  if(self->dev->gui_attached && g && piece->pipe->type == DT_DEV_PIXELPIPE_FULL)
  {
    dt_iop_gui_enter_critical_section(self);
    const uint64_t hash = g->hash;
    dt_iop_gui_leave_critical_section(self);

    // note that the case 'hash == 0' on first invocation in a session implies that g->lwmax
    // is NAN which initiates special handling below to avoid inconsistent results. in all
    // other cases we make sure that the preview pipe has left us with proper readings for
    // lwmax. if data are not yet there we need to wait (with timeout).
    if(hash != piece->global_hash)
      dt_control_log(_("inconsistent output"));

    dt_iop_gui_enter_critical_section(self);
    tmp_lwmax = g->lwmax;
    dt_iop_gui_leave_critical_section(self);
  }

  // in all other cases we calculate lwmax here
  if(isnan(tmp_lwmax))
  {
    lwmax = eps;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(roi_out, in, ch) reduction(max : lwmax)      \
  schedule(static)
#endif
    for(size_t k = 0; k < (size_t)roi_out->width * roi_out->height; k++)
    {
      const float *inp = in + ch * k;
      lwmax = fmaxf(lwmax, (inp[0] * 0.01f));
    }
  }
  else
  {
    lwmax = tmp_lwmax;
  }

  // PREVIEW pixelpipe stores lwmax
  if(self->dev->gui_attached && g && piece->pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
  {
    uint64_t hash = piece->global_hash;
    dt_iop_gui_enter_critical_section(self);
    g->lwmax = lwmax;
    g->hash = hash;
    dt_iop_gui_leave_critical_section(self);
  }

  const float ldc = data->drago.max_light * 0.01 / log10f(lwmax + 1);
  const float bl = logf(fmaxf(eps, data->drago.bias)) / logf(0.5);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ch, bl, ldc, roi_out, eps) \
  shared(in, out, lwmax) \
  schedule(static)
#endif
  for(size_t k = 0; k < (size_t)roi_out->width * roi_out->height; k++)
  {
    float *inp = in + ch * k;
    float *outp = out + ch * k;
    float lw = inp[0] * 0.01f;
    outp[0] = 100.0f
              * (ldc * logf(fmaxf(eps, lw + 1.0f)) / logf(fmaxf(eps, 2.0f + (powf(lw / lwmax, bl)) * 8.0f)));
    outp[1] = inp[1];
    outp[2] = inp[2];
  }
}

static inline void process_filmic(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                  const void *const ivoid, void *const ovoid,
                                  const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                                  dt_iop_global_tonemap_data_t *data)
{
  float *in = (float *)ivoid;
  float *out = (float *)ovoid;
  const int ch = piece->colors;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ch, roi_out) \
  shared(in, out, data) \
  schedule(static)
#endif
  for(size_t k = 0; k < (size_t)roi_out->width * roi_out->height; k++)
  {
    float *inp = in + ch * k;
    float *outp = out + ch * k;
    float l = inp[0] / 100.0;
    float x = fmaxf(0.0f, l - 0.004f);
    outp[0] = 100.0 * ((x * (6.2 * x + .5)) / (x * (6.2 * x + 1.7) + 0.06));
    outp[1] = inp[1];
    outp[2] = inp[2];
  }
}

int process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_global_tonemap_data_t *data = (dt_iop_global_tonemap_data_t *)piece->data;
  const float scale = fmaxf(1.f / roi_in->scale, 1.f);
  const float sigma_r = 8.0f; // does not depend on scale
  const float iw = piece->buf_in.width / scale;
  const float ih = piece->buf_in.height / scale;
  const float sigma_s = fminf(iw, ih) * 0.03f;
  dt_bilateral_t *b = NULL;
  if(data->detail != 0.0f)
  {
    b = dt_bilateral_init(roi_in->width, roi_in->height, sigma_s, sigma_r);
    if(!b) return 1;
    // get detail from unchanged input buffer
    dt_bilateral_splat(b, (float *)ivoid);
  }

  switch(data->operator)
  {
    case OPERATOR_REINHARD:
      process_reinhard(self, piece, ivoid, ovoid, roi_in, roi_out, data);
      break;
    case OPERATOR_DRAGO:
      process_drago(self, piece, ivoid, ovoid, roi_in, roi_out, data);
      break;
    case OPERATOR_FILMIC:
      process_filmic(self, piece, ivoid, ovoid, roi_in, roi_out, data);
      break;
  }

  if(data->detail != 0.0f)
  {
    dt_bilateral_blur(b);
    // and apply it to output buffer after logscale
    dt_bilateral_slice_to_output(b, (float *)ivoid, (float *)ovoid, data->detail);
    dt_bilateral_free(b);
  }

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
  return 0;
}

void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  dt_iop_global_tonemap_data_t *d = (dt_iop_global_tonemap_data_t *)piece->data;

  const float scale = 1.f / roi_in->scale;
  const float iw = piece->buf_in.width / scale;
  const float ih = piece->buf_in.height / scale;
  const float sigma_s = fminf(iw, ih) * 0.03f;
  const float sigma_r = 8.0f;
  const int detail = (d->detail != 0.0f);

  const int width = roi_in->width;
  const int height = roi_in->height;
  const int channels = piece->colors;

  const size_t basebuffer = sizeof(float) * channels * width * height;

  tiling->factor = 2.0f + (detail ? (float)dt_bilateral_memory_use2(width, height, sigma_s, sigma_r) / basebuffer : 0.0f);
  tiling->maxbuf
      = (detail ? MAX(1.0f, (float)dt_bilateral_singlebuffer_size2(width, height, sigma_s, sigma_r) / basebuffer) : 1.0f);
  tiling->overhead = 0;
  tiling->overlap = (detail ? ceilf(4 * sigma_s) : 0);
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_global_tonemap_params_t *p = (dt_iop_global_tonemap_params_t *)p1;
  dt_iop_global_tonemap_data_t *d = (dt_iop_global_tonemap_data_t *)piece->data;

  d->operator= p->operator;
  d->drago.bias = p->drago.bias;
  d->drago.max_light = p->drago.max_light;
  d->detail = p->detail;

  // drago needs the maximum L-value of the whole image so it must not use tiling
  if(d->operator == OPERATOR_DRAGO) piece->process_tiling_ready = 0;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_global_tonemap_data_t));
  piece->data_size = sizeof(dt_iop_global_tonemap_data_t);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_global_tonemap_gui_data_t *g = (dt_iop_global_tonemap_gui_data_t *)self->gui_data;
  dt_iop_global_tonemap_params_t *p = (dt_iop_global_tonemap_params_t *)self->params;

  if(!w || w == g->operator)
  {
    gtk_widget_set_visible(g->drago.bias, p->operator == OPERATOR_DRAGO);
    gtk_widget_set_visible(g->drago.max_light, p->operator == OPERATOR_DRAGO);
  }
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_global_tonemap_gui_data_t *g = (dt_iop_global_tonemap_gui_data_t *)self->gui_data;

  gui_changed(self, NULL, 0);

  dt_iop_gui_enter_critical_section(self);
  g->lwmax = NAN;
  g->hash = 0;
  dt_iop_gui_leave_critical_section(self);
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_global_tonemap_gui_data_t *g = IOP_GUI_ALLOC(global_tonemap);

  g->lwmax = NAN;
  g->hash = 0;

  g->operator = dt_bauhaus_combobox_from_params(self, N_("operator"));
  gtk_widget_set_tooltip_text(g->operator, _("the global tonemap operator"));

  g->drago.bias = dt_bauhaus_slider_from_params(self, "drago.bias");
  gtk_widget_set_tooltip_text(g->drago.bias, _("the bias for tonemapper controls the linearity, "
                                               "the higher the more details in blacks"));

  g->drago.max_light = dt_bauhaus_slider_from_params(self, "drago.max_light");
  gtk_widget_set_tooltip_text(g->drago.max_light, _("the target light for tonemapper specified as cd/m2"));

  g->detail = dt_bauhaus_slider_from_params(self, N_("detail"));
  dt_bauhaus_slider_set_digits(g->detail, 3);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  IOP_GUI_FREE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
