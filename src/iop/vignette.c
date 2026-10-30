/*
    This file is part of darktable,
    Copyright (C) 2010 Alexandre Prokoudine.
    Copyright (C) 2010-2011 Bruce Guenter.
    Copyright (C) 2010-2011 Henrik Andersson.
    Copyright (C) 2010-2013, 2016, 2018 johannes hanika.
    Copyright (C) 2010 Stuart Henderson.
    Copyright (C) 2011 Antony Dovgal.
    Copyright (C) 2011 Jérémy Rosen.
    Copyright (C) 2011 Olivier Tribout.
    Copyright (C) 2011 Robert Bieber.
    Copyright (C) 2011-2014, 2016, 2019 Tobias Ellinghaus.
    Copyright (C) 2012 José Carlos García Sogo.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012, 2014 Ulrich Pegelow.
    Copyright (C) 2013, 2018, 2020-2022 Pascal Obry.
    Copyright (C) 2013 Simon Spannagel.
    Copyright (C) 2014 Pascal de Bruijn.
    Copyright (C) 2014-2016, 2019 Roman Lebedev.
    Copyright (C) 2015 Pedro Côrte-Real.
    Copyright (C) 2017, 2019 Heiko Bauke.
    Copyright (C) 2018, 2020, 2022-2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2018 Edgardo Hoszowski.
    Copyright (C) 2018 Maurizio Paglia.
    Copyright (C) 2018 rawfiner.
    Copyright (C) 2019-2020 Aldric Renaudin.
    Copyright (C) 2019 Andreas Schneider.
    Copyright (C) 2020, 2022 Chris Elston.
    Copyright (C) 2020, 2022 Diederik Ter Rahe.
    Copyright (C) 2020, 2022 Hanno Schwalm.
    Copyright (C) 2020-2021 Ralf Brown.
    Copyright (C) 2021 Hubert Kowalski.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Philipp Lutz.
    Copyright (C) 2025-2026 Guillaume Stutin.
    
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
#include "system/macros.h"
#include "system/openmp.h"
#include "system/target_clones.h"
#include "system/mem_alloc.h"
#include "common/logging.h"
#include "common/module_versioning.h"
#include "database/database.h"
#include <stdlib.h>
#include <string.h>

#include "widgets/bauhaus.h"
#include "widgets/accelerators.h"
#include "math/math.h"
#include "common/opencl.h"
#include "pixel/tea.h"
#include "control/input.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"

#include "gui/presets.h"
#include "widgets/draw.h"
#include "iop/iop_api.h"
#include <gtk/gtk.h>
#include <inttypes.h>
#include "widgets/label.h"

DT_MODULE_INTROSPECTION(4, dt_iop_vignette_params_t)

typedef enum dt_iop_dither_t
{
  DITHER_OFF = 0,  // $DESCRIPTION: "off"
  DITHER_8BIT = 1, // $DESCRIPTION: "8-bit output"
  DITHER_16BIT = 2 // $DESCRIPTION: "16-bit output"
} dt_iop_dither_t;

typedef struct dt_iop_dvector_2d_t
{
  double x;
  double y;
} dt_iop_dvector_2d_t;

typedef struct dt_iop_fvector_2d_t
{
  float x; // $MIN: -1.0 $MAX: 1.0 $DESCRIPTION: "horizontal center"
  float y; // $MIN: -1.0 $MAX: 1.0 $DESCRIPTION: "vertical center"
} dt_iop_vector_2d_t;

typedef struct dt_iop_vignette_params1_t
{
  double scale;         // 0 - 100 Radie
  double falloff_scale; // 0 - 100 Radie for falloff inner radie of falloff=scale and
                        // outer=scale+falloff_scale
  double strength;      // 0 - 1 strength of effect
  double uniformity;    // 0 - 1 uniformity of center
  double bsratio;       // -1 - +1 ratio of brightness/saturation effect
  gboolean invert_falloff;
  gboolean invert_saturation;
  dt_iop_dvector_2d_t center; // Center of vignette
} dt_iop_vignette_params1_t;

typedef struct dt_iop_vignette_params2_t
{
  float scale;               // 0 - 100 Inner radius, percent of largest image dimension
  float falloff_scale;       // 0 - 100 Radius for falloff -- outer radius = inner radius + falloff_scale
  float brightness;          // -1 - 1 Strength of brightness reduction
  float saturation;          // -1 - 1 Strength of saturation reduction
  dt_iop_vector_2d_t center; // Center of vignette
  gboolean autoratio;        //
  float whratio;             // 0-1 = width/height ratio, 1-2 = height/width ratio + 1
  float shape;
} dt_iop_vignette_params2_t;

typedef struct dt_iop_vignette_params3_t
{
  float scale;               // 0 - 100 Inner radius, percent of largest image dimension
  float falloff_scale;       // 0 - 100 Radius for falloff -- outer radius = inner radius + falloff_scale
  float brightness;          // -1 - 1 Strength of brightness reduction
  float saturation;          // -1 - 1 Strength of saturation reduction
  dt_iop_vector_2d_t center; // Center of vignette
  gboolean autoratio;        //
  float whratio;             // 0-1 = width/height ratio, 1-2 = height/width ratio + 1
  float shape;
  int dithering; // if and how to perform dithering
} dt_iop_vignette_params3_t;

typedef struct dt_iop_vignette_params_t
{
  float scale;               // $MIN: 0.0 $MAX: 200.0 $DEFAULT: 80.0 Inner radius, percent of largest image dimension
  float falloff_scale;       // $MIN: 0.0 $MAX: 200.0 $DEFAULT: 50.0 $DESCRIPTION: "fall-off strength" 0 - 100 Radius for falloff -- outer radius = inner radius + falloff_scale
  float brightness;          // $MIN: -1.0 $MAX: 1.0 $DEFAULT: -0.5 -1 - 1 Strength of brightness reduction
  float saturation;          // $MIN: -1.0 $MAX: 1.0 $DEFAULT: -0.5 -1 - 1 Strength of saturation reduction
  dt_iop_vector_2d_t center; // Center of vignette
  gboolean autoratio;        // $DEFAULT: FALSE $DESCRIPTION: "automatic ratio"
  float whratio;             // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 1.0 $DESCRIPTION: "width vs. height ratio" 0-1 = width/height ratio, 1-2 = height/width ratio + 1
  float shape;               // $MIN: 0.0 $MAX: 5.0 $DEFAULT: 1.0 $DESCRIPTION: "shape"
  dt_iop_dither_t dithering; // $DEFAULT: DITHER_OFF if and how to perform dithering
  gboolean unbound;          // $DEFAULT: TRUE whether the values should be clipped
} dt_iop_vignette_params_t;


typedef struct dt_iop_vignette_gui_data_t
{
  GtkWidget *scale;
  GtkWidget *falloff_scale;
  GtkWidget *brightness;
  GtkWidget *saturation;
  GtkWidget *center_x;
  GtkWidget *center_y;
  GtkWidget *autoratio;
  GtkWidget *whratio;
  GtkWidget *shape;
  GtkWidget *dithering;
} dt_iop_vignette_gui_data_t;

typedef struct dt_iop_vignette_data_t
{
  float scale;
  float falloff_scale;
  float brightness;
  float saturation;
  dt_iop_vector_2d_t center; // Center of vignette
  gboolean autoratio;
  float whratio;
  float shape;
  int dithering;
  gboolean unbound;
} dt_iop_vignette_data_t;

typedef struct dt_iop_vignette_global_data_t
{
  int kernel_vignette;
} dt_iop_vignette_global_data_t;


const char *name()
{
  return _("_Vignetting");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("simulate a lens fall-off close to edges"),
                                      _("creative"),
                                      _("non-linear, RGB, display-referred"),
                                      _("non-linear, RGB"),
                                      _("non-linear, RGB, display-referred"));
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING
         | IOP_FLAGS_TILING_FULL_ROI;
}

int default_group()
{
  return IOP_GROUP_EFFECTS;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version == 1 && new_version == 4)
  {
    const dt_iop_vignette_params1_t *old = old_params;
    dt_iop_vignette_params_t *new = new_params;
    new->scale = old->scale;
    new->falloff_scale = old->falloff_scale;
    new->brightness = -(1.0 - MAX(old->bsratio, 0.0)) * old->strength / 100.0;
    new->saturation = -(1.0 + MIN(old->bsratio, 0.0)) * old->strength / 100.0;
    if(old->invert_saturation) new->saturation *= -2.0; // Double effect for increasing saturation
    if(old->invert_falloff) new->brightness = -new->brightness;
    new->center.x = old->center.x;
    new->center.y = old->center.y;
    new->autoratio = TRUE;
    new->whratio = 1.0;
    new->shape = 1.0;
    new->dithering = DITHER_OFF;
    new->unbound = FALSE;
    return 0;
  }
  if(old_version == 2 && new_version == 4)
  {
    const dt_iop_vignette_params2_t *old = old_params;
    dt_iop_vignette_params_t *new = new_params;
    new->scale = old->scale;
    new->falloff_scale = old->falloff_scale;
    new->brightness = old->brightness;
    new->saturation = old->saturation;
    new->center.x = old->center.x;
    new->center.y = old->center.y;
    new->autoratio = old->autoratio;
    new->whratio = old->whratio;
    new->shape = old->shape;
    new->dithering = DITHER_OFF;
    new->unbound = FALSE;
    return 0;
  }
  if(old_version == 3 && new_version == 4)
  {
    const dt_iop_vignette_params3_t *old = old_params;
    dt_iop_vignette_params_t *new = new_params;
    new->scale = old->scale;
    new->falloff_scale = old->falloff_scale;
    new->brightness = old->brightness;
    new->saturation = old->saturation;
    new->center.x = old->center.x;
    new->center.y = old->center.y;
    new->autoratio = old->autoratio;
    new->whratio = old->whratio;
    new->shape = old->shape;
    new->dithering = old->dithering;
    new->unbound = FALSE;
    return 0;
  }

  return 1;
}

/* The vignette is computed on the module's own input frame (piece->buf_in, see process()),
 * which sits upstream of lens, crop, flip, ashift... Its geometry is therefore stated in that
 * frame, at full resolution, and only projected through the downstream distortions to be
 * drawn or hit-tested. */
typedef struct dt_iop_vignette_geometry_t
{
  float width;           // module input frame, full resolution
  float height;
  float longest;         // MAX(width, height), the basis of a fixed w/h ratio
  float cx;              // vignette center
  float cy;
  float rx;              // inner radii: start of the falloff
  float ry;
  float frx;             // outer radii: end of the falloff
  float fry;
  float shape;
} dt_iop_vignette_geometry_t;

#define VIGNETTE_OUTLINE_SAMPLES 128

static gboolean _vignette_geometry(dt_iop_module_t *self, const dt_iop_vignette_params_t *p,
                                   dt_iop_vignette_geometry_t *geo)
{
  dt_iop_roi_t in;
  if(!dt_dev_module_geometry_gui(self->dev, self, &in, NULL)) return FALSE;
  if(in.width <= 0 || in.height <= 0) return FALSE;

  geo->width = in.width;
  geo->height = in.height;
  geo->longest = MAX(geo->width, geo->height);
  geo->cx = (p->center.x + 1.0f) * 0.5f * geo->width;
  geo->cy = (p->center.y + 1.0f) * 0.5f * geo->height;

  // Half-axes of the unit ellipse, mirroring xscale/yscale in process()
  float half_x;
  float half_y;
  if(p->autoratio)
  {
    half_x = 0.5f * geo->width;
    half_y = 0.5f * geo->height;
  }
  else if(p->whratio <= 1.0f)
  {
    half_x = 0.5f * geo->longest * p->whratio;
    half_y = 0.5f * geo->longest;
  }
  else
  {
    half_x = 0.5f * geo->longest;
    half_y = 0.5f * geo->longest * (2.0f - p->whratio);
  }

  const float dscale = p->scale / 100.0f;
  const float min_falloff = 100.0f / MIN(geo->width, geo->height);
  const float fscale = MAX(p->falloff_scale, min_falloff) / 100.0f;
  geo->rx = dscale * half_x;
  geo->ry = dscale * half_y;
  geo->frx = (dscale + fscale) * half_x;
  geo->fry = (dscale + fscale) * half_y;
  geo->shape = MAX(p->shape, 0.001f);
  return TRUE;
}

/* Module frame (full resolution) -> developed image, absolute pixels. */
static gboolean _module_to_image_abs(dt_iop_module_t *self, float *pts, const size_t count)
{
  return dt_dev_distort_transform_gui(self->dev, self->iop_order, DT_DEV_TRANSFORM_DIR_FORW_EXCL, pts, count) != 0;
}

/* Widget pointer -> module frame (full resolution). */
static gboolean _widget_to_module(dt_iop_module_t *self, const double x, const double y, float pt[2])
{
  pt[0] = (float)x;
  pt[1] = (float)y;
  dt_dev_coordinates_widget_to_image_norm(self->dev, pt, 1);
  dt_dev_coordinates_image_norm_to_image_abs(self->dev, pt, 1);
  return dt_dev_distort_backtransform_gui(self->dev, self->iop_order, DT_DEV_TRANSFORM_DIR_FORW_EXCL, pt, 1) != 0;
}

/* The five handles in module frame: center, inner x, inner y, outer x, outer y.
 * Their order is the grab identifiers' bit order below. */
static void _vignette_handles(const dt_iop_vignette_geometry_t *geo, float pts[10])
{
  pts[0] = geo->cx;            pts[1] = geo->cy;
  pts[2] = geo->cx + geo->rx;  pts[3] = geo->cy;
  pts[4] = geo->cx;            pts[5] = geo->cy - geo->ry;
  pts[6] = geo->cx + geo->frx; pts[7] = geo->cy;
  pts[8] = geo->cx;            pts[9] = geo->cy - geo->fry;
}

/* Hit test in developed-image pixels, where DT_GUI_MOUSE_EFFECT_RADIUS is expressed.
 * Returns 1 center, 2 x size, 4 y size, 8 x falloff, 16 y falloff, 0 nothing. */
static int _get_grab(dt_iop_module_t *self, const dt_iop_vignette_geometry_t *geo, const double x,
                     const double y)
{
  float handles[10];
  _vignette_handles(geo, handles);
  if(!_module_to_image_abs(self, handles, 5)) return 0;

  float pointer[2] = { (float)x, (float)y };
  dt_dev_coordinates_widget_to_image_norm(self->dev, pointer, 1);
  dt_dev_coordinates_image_norm_to_image_abs(self->dev, pointer, 1);

  const float radius = DT_GUI_MOUSE_EFFECT_RADIUS;
  const float radius_sq = radius * radius;
  // Size handles first, so they stay reachable on a vignette shrunk onto its center
  static const int order[5] = { 1, 2, 0, 3, 4 };
  for(int k = 0; k < 5; k++)
  {
    const int i = order[k];
    const float dx = pointer[0] - handles[2 * i];
    const float dy = pointer[1] - handles[2 * i + 1];
    if(dx * dx + dy * dy <= radius_sq) return 1 << i;
  }
  return 0;
}

/* Superellipse |x/rx|^(2/shape) + |y/ry|^(2/shape) = 1, the iso-line process() thresholds on. */
static void _outline_sample(const dt_iop_vignette_geometry_t *geo, const float rx, const float ry,
                            float *pts)
{
  for(int i = 0; i < VIGNETTE_OUTLINE_SAMPLES; i++)
  {
    const float t = 2.0f * M_PI * i / VIGNETTE_OUTLINE_SAMPLES;
    const float c = cosf(t);
    const float s = sinf(t);
    pts[2 * i] = geo->cx + rx * copysignf(powf(fabsf(c), geo->shape), c);
    pts[2 * i + 1] = geo->cy + ry * copysignf(powf(fabsf(s), geo->shape), s);
  }
}

static void _draw_closed_path(cairo_t *cr, const float *pts, const int count)
{
  cairo_move_to(cr, pts[0], pts[1]);
  for(int i = 1; i < count; i++) cairo_line_to(cr, pts[2 * i], pts[2 * i + 1]);
  cairo_close_path(cr);
  cairo_stroke(cr);
}

static void _draw_overlay(cairo_t *cr, const float *inner, const float *outer, const float *handles,
                          const int grab, const float zoom_scale)
{
  // half width/height of the crosshair
  const float crosshair = DT_PIXEL_APPLY_DPI(10.0) / zoom_scale;
  cairo_move_to(cr, handles[0] - crosshair, handles[1]);
  cairo_line_to(cr, handles[0] + crosshair, handles[1]);
  cairo_move_to(cr, handles[0], handles[1] - crosshair);
  cairo_line_to(cr, handles[0], handles[1] + crosshair);
  cairo_stroke(cr);

  _draw_closed_path(cr, inner, VIGNETTE_OUTLINE_SAMPLES);
  _draw_closed_path(cr, outer, VIGNETTE_OUTLINE_SAMPLES);

  const float radius_sel = DT_PIXEL_APPLY_DPI(6.0) / zoom_scale;
  const float radius_reg = DT_PIXEL_APPLY_DPI(4.0) / zoom_scale;
  for(int i = 0; i < 5; i++)
  {
    cairo_new_sub_path(cr);
    cairo_arc(cr, handles[2 * i], handles[2 * i + 1], (grab == (1 << i)) ? radius_sel : radius_reg, 0.0,
              M_PI * 2.0);
    cairo_stroke(cr);
  }
}

void gui_post_expose(struct dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height,
                     int32_t pointerx, int32_t pointery)
{
  dt_develop_t *dev = self->dev;
  dt_iop_vignette_gui_data_t *g = (dt_iop_vignette_gui_data_t *)dt_iop_gui_data(self);
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  if(IS_NULL_PTR(g) || IS_NULL_PTR(p)) return;

  dt_iop_vignette_geometry_t geo;
  if(!_vignette_geometry(self, p, &geo)) return;

  // Everything is sampled in the module frame and carried to the preview through the
  // downstream distortions in one batch: inner outline, outer outline, handles.
  const size_t count = 2 * VIGNETTE_OUTLINE_SAMPLES + 5;
  float pts[2 * (2 * VIGNETTE_OUTLINE_SAMPLES + 5)];
  float *inner = pts;
  float *outer = pts + 2 * VIGNETTE_OUTLINE_SAMPLES;
  float *handles = pts + 4 * VIGNETTE_OUTLINE_SAMPLES;
  _outline_sample(&geo, geo.rx, geo.ry, inner);
  _outline_sample(&geo, geo.frx, geo.fry, outer);
  _vignette_handles(&geo, handles);
  if(!_module_to_image_abs(self, pts, count)) return;
  dt_dev_coordinates_image_abs_to_image_norm(dev, pts, count);
  dt_dev_coordinates_image_norm_to_preview_abs(dev, pts, count);

  const int grab = _get_grab(self, &geo, pointerx, pointery);
  const float zoom_scale = dt_dev_get_overlay_scale(dev);
  dt_dev_rescale_roi(dev, cr, width, height);

  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(3.0) / zoom_scale);
  dt_draw_set_color_overlay(cr, FALSE, 0.8);
  _draw_overlay(cr, inner, outer, handles, grab, zoom_scale);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.0) / zoom_scale);
  dt_draw_set_color_overlay(cr, TRUE, 0.8);
  _draw_overlay(cr, inner, outer, handles, grab, zoom_scale);
}

// FIXME: Pumping of the opposite direction when changing width/height. See two FIXMEs further down.
int mouse_moved(struct dt_iop_module_t *self, double x, double y, double pressure, int which)
{
  dt_iop_vignette_gui_data_t *g = (dt_iop_vignette_gui_data_t *)dt_iop_gui_data(self);
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  if(IS_NULL_PTR(g) || IS_NULL_PTR(p)) return 0;

  dt_iop_vignette_geometry_t geo;
  if(!_vignette_geometry(self, p, &geo)) return 0;
  const float longest = geo.longest;

  static int old_grab = -1;
  int grab = old_grab;

  if(grab == 0 || !(dt_control_button_down(1)))
    grab = _get_grab(self, &geo, x, y);

  if(dt_control_button_down(1))
  {
    if(grab == 0) // pan the image
    {
      dt_control_queue_cursor(GDK_HAND1);
      return 0;
    }

    float pointer[2];
    if(!_widget_to_module(self, x, y, pointer)) return 1;
    const float mx = pointer[0];
    const float my = pointer[1];

    if(grab == 1) // move the center
    {
      dt_bauhaus_slider_set(g->center_x, mx / geo.width * 2.0f - 1.0f);
      dt_bauhaus_slider_set(g->center_y, my / geo.height * 2.0f - 1.0f);
    }
    else if(grab == 2) // change the width
    {
      const float max = 0.5f * ((p->whratio <= 1.0f) ? longest * p->whratio : longest);
      const float new_vignette_w = MIN(longest, MAX(0.1f, mx - geo.cx));
      const float ratio = new_vignette_w / geo.ry;
      const float new_scale = 100.0f * new_vignette_w / max;
      // FIXME: When going over the 1.0 boundary from wide to narrow (>1.0 -> <=1.0) the height slightly
      // changes, depending on speed.
      //        I guess we have to split the computation.
      if(ratio <= 1.0f)
      {
        if(dt_modifier_is(which, DT_PRIMARY_MASK))
          dt_bauhaus_slider_set(g->scale, new_scale);
        else
          dt_bauhaus_slider_set(g->whratio, ratio);
      }
      else
      {
        dt_bauhaus_slider_set(g->scale, new_scale);
        if(!dt_modifier_is(which, DT_PRIMARY_MASK))
          dt_bauhaus_slider_set(g->whratio, 2.0f - 1.0f / ratio);
      }
    }
    else if(grab == 4) // change the height
    {
      const float new_vignette_h = MIN(longest, MAX(0.1f, geo.cy - my));
      const float ratio = new_vignette_h / geo.rx;
      const float max = 0.5f * ((ratio <= 1.0f) ? longest * (2.0f - p->whratio) : longest);
      // FIXME: When going over the 1.0 boundary from narrow to wide (>1.0 -> <=1.0) the width slightly
      // changes, depending on speed.
      //        I guess we have to split the computation.
      if(ratio <= 1.0f)
      {
        if(dt_modifier_is(which, DT_PRIMARY_MASK))
          dt_bauhaus_slider_set(g->scale, 100.0f * new_vignette_h / max);
        else
          dt_bauhaus_slider_set(g->whratio, 2.0f - ratio);
      }
      else
      {
        dt_bauhaus_slider_set(g->scale, 100.0f * new_vignette_h / max);
        if(!dt_modifier_is(which, DT_PRIMARY_MASK))
          dt_bauhaus_slider_set(g->whratio, 1.0f / ratio);
      }
    }
    else if(grab == 8) // change the falloff on the right
    {
      const float max = 0.5f * ((p->whratio <= 1.0f) ? longest * p->whratio : longest);
      const float delta_x = MIN(2.0f * max, MAX(0.0f, mx - geo.cx - geo.rx));
      dt_bauhaus_slider_set(g->falloff_scale, 100.0f * delta_x / max);
    }
    else if(grab == 16) // change the falloff on the top
    {
      const float max = 0.5f * ((p->whratio > 1.0f) ? longest * (2.0f - p->whratio) : longest);
      const float delta_y = MIN(2.0f * max, MAX(0.0f, geo.cy - my - geo.ry));
      dt_bauhaus_slider_set(g->falloff_scale, 100.0f * delta_y / max);
    }
    dt_control_queue_redraw_center();
    return 1;
  }
  else if(grab)
  {
    if(grab == 1)
      dt_control_queue_cursor(GDK_FLEUR);
    else if(grab == 2 || grab == 8)
      dt_control_queue_cursor(GDK_SB_H_DOUBLE_ARROW);
    else if(grab == 4 || grab == 16)
      dt_control_queue_cursor(GDK_SB_V_DOUBLE_ARROW);
  }
  else
  {
    if(old_grab != grab) dt_control_queue_cursor(GDK_LEFT_PTR);
  }
  old_grab = grab;
  dt_control_queue_redraw_center();
  return 0;
}

int button_pressed(struct dt_iop_module_t *self, double x, double y, double pressure, int which, int type,
                   uint32_t state)
{
  if(which == 1) return 1;
  return 0;
}

int button_released(struct dt_iop_module_t *self, double x, double y, int which, uint32_t state)
{
  if(which == 1) return 1;
  return 0;
}

__DT_CLONE_TARGETS__
int process(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid)
{
  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  const dt_iop_vignette_data_t *data = (dt_iop_vignette_data_t *)piece->data;
  const dt_iop_roi_t *buf_in = &piece->buf_in;
  const size_t ch = piece->dsc_in.channels;
  const gboolean unbound = data->unbound;

  /* Center coordinates of buf_in, these should not consider buf_in->{x,y}! */
  const dt_iop_vector_2d_t buf_center = { buf_in->width * .5f, buf_in->height * .5f };
  /* Center coordinates of vignette center */
  const dt_iop_vector_2d_t vignette_center = { buf_center.x + data->center.x * buf_in->width / 2.0,
                                               buf_center.y + data->center.y * buf_in->height / 2.0 };
  /* Coordinates of vignette_center in terms of roi_in */
  const dt_iop_vector_2d_t roi_center
      = { vignette_center.x * roi_in->scale - roi_in->x, vignette_center.y * roi_in->scale - roi_in->y };
  float xscale;
  float yscale;

  /* w/h ratio follows piece dimensions */
  if(data->autoratio)
  {
    xscale = 2.0 / (buf_in->width * roi_out->scale);
    yscale = 2.0 / (buf_in->height * roi_out->scale);
  }
  else /* specified w/h ratio, scale proportional to longest side */
  {
    const float basis = 2.0 / (MAX(buf_in->height, buf_in->width) * roi_out->scale);
    // w/h ratio from 0-1 use as-is
    if(data->whratio <= 1.0)
    {
      yscale = basis;
      xscale = yscale / data->whratio;
    }
    // w/h ratio from 1-2 interpret as 1-inf
    // that is, the h/w ratio + 1
    else
    {
      xscale = basis;
      yscale = xscale / (2.0 - data->whratio);
    }
  }
  const float dscale = data->scale / 100.0;
  // A minimum falloff is used, based on the image size, to smooth out aliasing artifacts
  const float min_falloff = 100.0 / MIN(buf_in->width, buf_in->height);
  const float fscale = MAX(data->falloff_scale, min_falloff) / 100.0;
  const float shape = MAX(data->shape, 0.001);
  const float exp1 = 2.0 / shape;
  const float exp2 = shape / 2.0;
  // Pre-scale the center offset
  const dt_iop_vector_2d_t roi_center_scaled = { roi_center.x * xscale, roi_center.y * yscale };

  float dither = 0.0f;

  switch(data->dithering)
  {
    case DITHER_8BIT:
      dither = 1.0f / 256;
      break;
    case DITHER_16BIT:
      dither = 1.0f / 65536;
      break;
    case DITHER_OFF:
    default:
      dither = 0.0f;
  }

  unsigned int *const tea_states = alloc_tea_states(dt_get_num_openmp_threads());
  __OMP_PARALLEL_FOR__()
  for(int j = 0; j < roi_out->height; j++)
  {
    const size_t k = (size_t)ch * roi_out->width * j;
    const float *in = (const float *)ivoid + k;
    float *out = (float *)ovoid + k;
    unsigned int *tea_state = get_tea_state(tea_states,dt_get_thread_num());
    tea_state[0] = j * roi_out->height; /* + dt_get_thread_num() -- do not include, makes results unreproducible */
    for(int i = 0; i < roi_out->width; i++, in += ch, out += ch)
    {
      // current pixel coord translated to local coord
      const dt_iop_vector_2d_t pv
          = { fabsf(i * xscale - roi_center_scaled.x), fabsf(j * yscale - roi_center_scaled.y) };

      // Calculate the pixel weight in vignette
      const float cplen = powf(powf(pv.x, exp1) + powf(pv.y, exp1), exp2); // Length from center to pv
      float weight = 0.0;
      float dith = 0.0;

      if(cplen >= dscale) // pixel is outside the inner vignette circle, lets calculate weight of vignette
      {
        weight = ((cplen - dscale) / fscale);
        if(weight >= 1.0)
          weight = 1.0;
        else if(weight <= 0.0)
          weight = 0.0;
        else if(dither == 0.0f)
        {
          // don't bother computing the random number if dithering is disabled
          dith = 0.0f;
        }
        else
        {
          weight = 0.5 - cosf(M_PI * weight) / 2.0;
          encrypt_tea(tea_state);
          dith = dither * tpdf(tea_state[0]);
        }
      }

      // Let's apply weighted effect on brightness and desaturation
      float col0 = in[0], col1 = in[1], col2 = in[2], col3 = in[3];
      if(weight > 0)
      {
        // Then apply falloff vignette
        float falloff = (data->brightness < 0) ? (1.0f + (weight * data->brightness))
                                               : (weight * data->brightness);
        col0 = data->brightness < 0 ? col0 * falloff + dith : col0 + falloff + dith;
        col1 = data->brightness < 0 ? col1 * falloff + dith : col1 + falloff + dith;
        col2 = data->brightness < 0 ? col2 * falloff + dith : col2 + falloff + dith;

        col0 = unbound ? col0 : CLIP(col0);
        col1 = unbound ? col1 : CLIP(col1);
        col2 = unbound ? col2 : CLIP(col2);

        // apply saturation
        float mv = (col0 + col1 + col2) / 3.0f;
        float wss = weight * data->saturation;
        col0 = col0 - ((mv - col0) * wss);
        col1 = col1 - ((mv - col1) * wss);
        col2 = col2 - ((mv - col2) * wss);

        col0 = unbound ? col0 : CLIP(col0);
        col1 = unbound ? col1 : CLIP(col1);
        col2 = unbound ? col2 : CLIP(col2);
      }

      out[0] = col0;
      out[1] = col1;
      out[2] = col2;
      out[3] = col3;
    }
  }

  free_tea_states(tea_states);
  return 0;
}


#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out)
{
  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  dt_iop_vignette_data_t *data = (dt_iop_vignette_data_t *)piece->data;
  dt_iop_vignette_global_data_t *gd = (dt_iop_vignette_global_data_t *)self->global_data;

  cl_int err = -999;
  const int devid = pipe->devid;
  const int width = roi_out->width;
  const int height = roi_out->height;

  const dt_iop_roi_t *buf_in = &piece->buf_in;

  /* Center coordinates of buf_in, these should not consider buf_in->{x,y}! */
  const dt_iop_vector_2d_t buf_center = { buf_in->width * .5f, buf_in->height * .5f };
  /* Center coordinates of vignette center */
  const dt_iop_vector_2d_t vignette_center = { buf_center.x + data->center.x * buf_in->width / 2.0,
                                               buf_center.y + data->center.y * buf_in->height / 2.0 };
  /* Coordinates of vignette_center in terms of roi_in */
  const dt_iop_vector_2d_t roi_center
      = { vignette_center.x * roi_in->scale - roi_in->x, vignette_center.y * roi_in->scale - roi_in->y };
  float xscale;
  float yscale;

  /* w/h ratio follows piece dimensions */
  if(data->autoratio)
  {
    xscale = 2.0 / (buf_in->width * roi_out->scale);
    yscale = 2.0 / (buf_in->height * roi_out->scale);
  }
  else /* specified w/h ratio, scale proportional to longest side */
  {
    const float basis = 2.0 / (MAX(buf_in->height, buf_in->width) * roi_out->scale);
    // w/h ratio from 0-1 use as-is
    if(data->whratio <= 1.0)
    {
      yscale = basis;
      xscale = yscale / data->whratio;
    }
    // w/h ratio from 1-2 interpret as 1-inf
    // that is, the h/w ratio + 1
    else
    {
      xscale = basis;
      yscale = xscale / (2.0 - data->whratio);
    }
  }
  const float dscale = data->scale / 100.0;
  // A minimum falloff is used, based on the image size, to smooth out aliasing artifacts
  const float min_falloff = 100.0 / MIN(buf_in->width, buf_in->height);
  const float fscale = MAX(data->falloff_scale, min_falloff) / 100.0;
  const float shape = MAX(data->shape, 0.001);
  const float exp1 = 2.0 / shape;
  const float exp2 = shape / 2.0;
  // Pre-scale the center offset
  const dt_iop_vector_2d_t roi_center_scaled = { roi_center.x * xscale, roi_center.y * yscale };

  float dither = 0.0f;

  switch(data->dithering)
  {
    case DITHER_8BIT:
      dither = 1.0f / 256;
      break;
    case DITHER_16BIT:
      dither = 1.0f / 65536;
      break;
    case DITHER_OFF:
    default:
      dither = 0.0f;
  }

  float scale[2] = { xscale, yscale };
  float roi_center_scaled_f[2] = { roi_center_scaled.x, roi_center_scaled.y };
  float expt[2] = { exp1, exp2 };
  const float brightness = data->brightness;
  const float saturation = data->saturation;
  const int unbound = data->unbound;

  size_t sizes[2] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid) };

  dt_opencl_set_kernel_arg(devid, gd->kernel_vignette, 0, sizeof(cl_mem), &dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_vignette, 1, sizeof(cl_mem), &dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_vignette, 2, sizeof(int), &width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_vignette, 3, sizeof(int), &height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_vignette, 4, 2 * sizeof(float), &scale);
  dt_opencl_set_kernel_arg(devid, gd->kernel_vignette, 5, 2 * sizeof(float), &roi_center_scaled_f);
  dt_opencl_set_kernel_arg(devid, gd->kernel_vignette, 6, 2 * sizeof(float), &expt);
  dt_opencl_set_kernel_arg(devid, gd->kernel_vignette, 7, sizeof(float), &dscale);
  dt_opencl_set_kernel_arg(devid, gd->kernel_vignette, 8, sizeof(float), &fscale);
  dt_opencl_set_kernel_arg(devid, gd->kernel_vignette, 9, sizeof(float), &brightness);
  dt_opencl_set_kernel_arg(devid, gd->kernel_vignette, 10, sizeof(float), &saturation);
  dt_opencl_set_kernel_arg(devid, gd->kernel_vignette, 11, sizeof(float), &dither);
  dt_opencl_set_kernel_arg(devid, gd->kernel_vignette, 12, sizeof(int), &unbound);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_vignette, sizes);
  if(err != CL_SUCCESS) goto error;

  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_vignette] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif


void init_global(dt_iop_module_so_t *module)
{
  const int program = 8; // extended.cl from programs.conf
  dt_iop_vignette_global_data_t *gd
      = (dt_iop_vignette_global_data_t *)malloc(sizeof(dt_iop_vignette_global_data_t));
  module->data = gd;
  gd->kernel_vignette = dt_opencl_create_kernel(program, "vignette");
}


void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_vignette_global_data_t *gd = (dt_iop_vignette_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_vignette);
  dt_free(module->data);
}


void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_vignette_gui_data_t *g = (dt_iop_vignette_gui_data_t *)dt_iop_gui_data(self);
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  if(IS_NULL_PTR(g) || IS_NULL_PTR(p)) return;

  gtk_widget_set_sensitive(GTK_WIDGET(g->whratio), !p->autoratio);
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)p1;
  dt_iop_vignette_data_t *d = (dt_iop_vignette_data_t *)piece->data;
  d->scale = p->scale;
  d->falloff_scale = p->falloff_scale;
  d->brightness = p->brightness;
  d->saturation = p->saturation;
  d->center = p->center;
  d->autoratio = p->autoratio;
  d->whratio = p->whratio;
  d->shape = p->shape;
  d->dithering = p->dithering;
  d->unbound = p->unbound;
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_database_start_transaction();
  dt_iop_vignette_params_t p;
  p.scale = 40.0f;
  p.falloff_scale = 100.0f;
  p.brightness = -1.0f;
  p.saturation = 0.5f;
  p.center.x = 0.0f;
  p.center.y = 0.0f;
  p.autoratio = FALSE;
  p.whratio = 1.0f;
  p.shape = 1.0f;
  p.dithering = 0;
  p.unbound = TRUE;
  dt_gui_presets_add_generic(_("lomo"), self->op,
                             self->version(), &p, sizeof(p), 1);
  dt_database_release_transaction();
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc_align(sizeof(dt_iop_vignette_data_t));
  piece->data_size = sizeof(dt_iop_vignette_data_t);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_free_align(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_vignette_gui_data_t *g = (dt_iop_vignette_gui_data_t *)dt_iop_gui_data(self);
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  if(IS_NULL_PTR(g) || IS_NULL_PTR(p)) return;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->autoratio), p->autoratio);
  gtk_widget_set_sensitive(GTK_WIDGET(g->whratio), !p->autoratio);
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_vignette_gui_data_t *g = IOP_GUI_ALLOC(vignette);

  g->scale = dt_bauhaus_slider_from_params(self, N_("scale"));
  g->falloff_scale = dt_bauhaus_slider_from_params(self, "falloff_scale");
  g->brightness = dt_bauhaus_slider_from_params(self, N_("brightness"));
  g->saturation = dt_bauhaus_slider_from_params(self, N_("saturation"));

  gtk_box_pack_start(GTK_BOX(self->gui->widget),
                     dt_ui_section_label_new(_("position / form")), FALSE, FALSE, 0);

  g->center_x = dt_bauhaus_slider_from_params(self, "center.x");
  g->center_y = dt_bauhaus_slider_from_params(self, "center.y");
  g->shape = dt_bauhaus_slider_from_params(self, N_("shape"));
  g->autoratio = dt_bauhaus_toggle_from_params(self, "autoratio");
  g->whratio = dt_bauhaus_slider_from_params(self, "whratio");
  g->dithering = dt_bauhaus_combobox_from_params(self, N_("dithering"));

  dt_bauhaus_slider_set_digits(g->brightness, 3);
  dt_bauhaus_slider_set_digits(g->saturation, 3);
  dt_bauhaus_slider_set_digits(g->center_x, 3);
  dt_bauhaus_slider_set_digits(g->center_y, 3);
  dt_bauhaus_slider_set_digits(g->whratio, 3);

  dt_bauhaus_slider_set_format(g->scale, "%");
  dt_bauhaus_slider_set_format(g->falloff_scale, "%");

  gtk_widget_set_tooltip_text(g->scale, _("the radii scale of vignette for start of fall-off"));
  gtk_widget_set_tooltip_text(g->falloff_scale, _("the radii scale of vignette for end of fall-off"));
  gtk_widget_set_tooltip_text(g->brightness, _("strength of effect on brightness"));
  gtk_widget_set_tooltip_text(g->saturation, _("strength of effect on saturation"));
  gtk_widget_set_tooltip_text(g->center_x, _("horizontal offset of center of the effect"));
  gtk_widget_set_tooltip_text(g->center_y, _("vertical offset of center of the effect"));
  gtk_widget_set_tooltip_text(g->shape, _("shape factor\n0 produces a rectangle\n1 produces a circle or ellipse\n"
                                          "2 produces a diamond"));
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->autoratio), _("enable to have the ratio automatically follow the image size"));
  gtk_widget_set_tooltip_text(g->whratio, _("width-to-height ratio"));
  gtk_widget_set_tooltip_text(g->dithering, _("add some level of random noise to prevent banding"));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
