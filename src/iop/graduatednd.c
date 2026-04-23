/*
    This file is part of darktable,
    Copyright (C) 2010-2011 Bruce Guenter.
    Copyright (C) 2010-2012 Henrik Andersson.
    Copyright (C) 2010-2014, 2016, 2018 johannes hanika.
    Copyright (C) 2010 Milan Knížek.
    Copyright (C) 2011 Antony Dovgal.
    Copyright (C) 2011 Brian Teague.
    Copyright (C) 2011 Olivier Tribout.
    Copyright (C) 2011 Robert Bieber.
    Copyright (C) 2011 Rostyslav Pidgornyi.
    Copyright (C) 2011-2014, 2016, 2019 Tobias Ellinghaus.
    Copyright (C) 2012-2013, 2019-2020 Aldric Renaudin.
    Copyright (C) 2012 Pascal de Bruijn.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012-2014, 2017 Ulrich Pegelow.
    Copyright (C) 2013 Dennis Gnad.
    Copyright (C) 2013-2016 Roman Lebedev.
    Copyright (C) 2013 Simon Spannagel.
    Copyright (C) 2013 Thomas Pryds.
    Copyright (C) 2015 Pedro Côrte-Real.
    Copyright (C) 2017, 2019-2020 Heiko Bauke.
    Copyright (C) 2018, 2020, 2022-2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2018-2019 Edgardo Hoszowski.
    Copyright (C) 2018 Maurizio Paglia.
    Copyright (C) 2018-2022 Pascal Obry.
    Copyright (C) 2018 rawfiner.
    Copyright (C) 2019 Andreas Schneider.
    Copyright (C) 2019-2020, 2022 Diederik Ter Rahe.
    Copyright (C) 2019 Diederik ter Rahe.
    Copyright (C) 2020, 2022 Chris Elston.
    Copyright (C) 2020 Nicolas Auffray.
    Copyright (C) 2020-2021 Ralf Brown.
    Copyright (C) 2021 Hubert Kowalski.
    Copyright (C) 2022 Hanno Schwalm.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Philipp Lutz.
    Copyright (C) 2023 Luca Zulberti.
    Copyright (C) 2025, 2026 Guillaume Stutin.
    
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
#include "common/colorspaces.h"
#include "common/debug.h"
#include "common/math.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "develop/tiling.h"
#include "dtgtk/gradientslider.h"

#include "gui/color_picker_proxy.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"

DT_MODULE_INTROSPECTION(1, dt_iop_graduatednd_params_t)

typedef struct dt_iop_graduatednd_params_t
{
  float density;     // $MIN: -8.0 $MAX: 8.0 $DEFAULT: 1.0 $DESCRIPTION: "density" The density of filter 0-8 EV
  float hardness;    // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "hardness" 0% = soft and 100% = hard
  float rotation;    // $MIN: -180.0 $MAX: 180.0 $DEFAULT: 0.0 $DESCRIPTION: "rotation" 2*PI -180 - +180
  float offset;      // $DEFAULT: 50.0 $DESCRIPTION: "offset" centered, can be offsetted...
  float hue;         // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "hue"
  float saturation;  // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "saturation"
} dt_iop_graduatednd_params_t;

typedef struct dt_iop_graduatednd_global_data_t
{
  int kernel_graduatedndp;
  int kernel_graduatedndm;
} dt_iop_graduatednd_global_data_t;


void init_presets(dt_iop_module_so_t *self)
{
  dt_database_start_transaction(darktable.db);

  dt_gui_presets_add_generic(_("neutral gray ND2 (soft)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 1, 0, 0, 50, 0, 0 },
                             sizeof(dt_iop_graduatednd_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);
  dt_gui_presets_add_generic(_("neutral gray ND4 (soft)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 2, 0, 0, 50, 0, 0 },
                             sizeof(dt_iop_graduatednd_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);
  dt_gui_presets_add_generic(_("neutral gray ND8 (soft)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 3, 0, 0, 50, 0, 0 },
                             sizeof(dt_iop_graduatednd_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);
  dt_gui_presets_add_generic(_("neutral gray ND2 (hard)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 1, 75, 0, 50, 0, 0 },
                             sizeof(dt_iop_graduatednd_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  dt_gui_presets_add_generic(_("neutral gray ND4 (hard)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 2, 75, 0, 50, 0, 0 },
                             sizeof(dt_iop_graduatednd_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);
  dt_gui_presets_add_generic(_("neutral gray ND8 (hard)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 3, 75, 0, 50, 0, 0 },
                             sizeof(dt_iop_graduatednd_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  dt_gui_presets_add_generic(_("orange ND2 (soft)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 1, 0, 0, 50, 0.102439, 0.8 },
                             sizeof(dt_iop_graduatednd_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);
  dt_gui_presets_add_generic(_("yellow ND2 (soft)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 1, 0, 0, 50, 0.151220, 0.5 },
                             sizeof(dt_iop_graduatednd_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  dt_gui_presets_add_generic(_("purple ND2 (soft)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 1, 0, 0, 50, 0.824390, 0.5 },
                             sizeof(dt_iop_graduatednd_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  dt_gui_presets_add_generic(_("green ND2 (soft)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 1, 0, 0, 50, 0.302439, 0.5 },
                             sizeof(dt_iop_graduatednd_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  dt_gui_presets_add_generic(_("red ND2 (soft)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 1, 0, 0, 50, 0, 0.5 },
                             sizeof(dt_iop_graduatednd_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);
  dt_gui_presets_add_generic(_("blue ND2 (soft)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 1, 0, 0, 50, 0.663415, 0.5 },
                             sizeof(dt_iop_graduatednd_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  dt_gui_presets_add_generic(_("brown ND4 (soft)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 2, 0, 0, 50, 0.082927, 0.25 },
                             sizeof(dt_iop_graduatednd_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  dt_database_release_transaction(darktable.db);
}

typedef struct grad_point_t
{
  float x;
  float y;
} grad_point_t;
typedef struct dt_iop_graduatednd_gui_data_t
{
  GtkWidget *density, *hardness, *rotation, *hue, *saturation;

  int selected;
  int dragging;

  gboolean define;
  float oldx;
  float oldy;
  grad_point_t a;
  grad_point_t b;
} dt_iop_graduatednd_gui_data_t;

typedef struct dt_iop_graduatednd_data_t
{
  float density;     // The density of filter 0-8 EV
  float hardness; // Default 0% = soft and 100% = hard
  float rotation;    // 2*PI -180 - +180
  float offset;      // Default 50%, centered, can be offsetted...
  float color[4];    // RGB color of gradient
  float color1[4];   // inverted color (1 - c)
} dt_iop_graduatednd_data_t;


const char *name()
{
  return _("graduated density");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("simulate an optical graduated neutral density filter"),
                                      _("corrective and creative"),
                                      _("linear or non-linear, RGB, scene-referred"),
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

static inline float f(const float t, const float c, const float x)
{
  return (t / (1.0f + powf(c, -x * 6.0f)) + (1.0f - t) * (x * .5f + .5f));
}

typedef struct dt_iop_vector_2d_t
{
  double x;
  double y;
} dt_iop_vector_2d_t;

// determine the distance between the segment [(ax,ay)(bx,by)] and the point (xc,yc)
static float _dist_seg(grad_point_t a, grad_point_t b, grad_point_t c)
{
  grad_point_t s = { b.x - a.x, b.y - a.y };
  grad_point_t u = { c.x - a.x, c.y - a.y };
  const float sn2 = s.x * s.x + s.y * s.y;
  if(sn2 <= 0.0f) return u.x * u.x + u.y * u.y;

  const float t = CLAMP((s.x * u.x + s.y * u.y) / sn2, 0.0f, 1.0f);
  const float dx = u.x - t * s.x;
  const float dy = u.y - t * s.y;
  return dx * dx + dy * dy;
}

/**
 * @brief Draw one triangular endpoint marker on the graduated line.
 *
 * The marker geometry is built from the active endpoint and the opposite endpoint, then mirrored with
 * @p normal_sign so the two markers keep opposite winding while sharing the same drawing path.
 */
static void _draw_end_marker(cairo_t *cr, const grad_point_t endpoint, const grad_point_t opposite, const float zoom_scale,
                                  const float normal_sign, const gboolean active)
{
  grad_point_t e_1 = { 0.0f, 0.0f };
  grad_point_t e_2 = { 0.0f, 0.0f };
  const float dx = opposite.x - endpoint.x;
  const float dy = opposite.y - endpoint.y;
  const float length = dt_fast_hypotf(dx, dy);
  const float x = DT_PIXEL_APPLY_DPI_DPP(15.0f) / zoom_scale;
  const float inv_len = 1.0f / length;
  const float ux = dx * inv_len;
  const float uy = dy * inv_len;
  const float px = -uy;
  const float py = ux;

  // e_1 is at distance x from endpoint along [endpoint, opposite].
  e_1.x = endpoint.x + ux * x;
  e_1.y = endpoint.y + uy * x;
  // e_2 is the midpoint of [endpoint, e_1], offset by x * normal_sign on the perpendicular.
  const float mx = (endpoint.x + e_1.x) * 0.5f;
  const float my = (endpoint.y + e_1.y) * 0.5f;
  e_2.x = mx + px * (x * normal_sign);
  e_2.y = my + py * (x * normal_sign);

  cairo_move_to(cr, endpoint.x, endpoint.y);
  cairo_line_to(cr, e_1.x, e_1.y);
  cairo_line_to(cr, e_2.x, e_2.y);
  cairo_close_path(cr);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.0f) / zoom_scale);

  dt_draw_set_color_overlay(cr, TRUE, active ? 1.0f : 0.5f);
  cairo_fill_preserve(cr);
  dt_draw_set_color_overlay(cr, FALSE, active ? 1.0f : 0.5f);
  cairo_stroke(cr);

  dt_draw_node(cr, TRUE, active, FALSE, zoom_scale, endpoint.x, endpoint.y);
}

static int set_grad_from_points(struct dt_iop_module_t *self, const grad_point_t *a, const grad_point_t *b,
                                float *rotation, float *offset)
{
  // we want absolute preview positions
  float pts[4] = { a->x, a->y, b->x, b->y };
  dt_dev_coordinates_image_norm_to_image_abs(self->dev, pts, 2);
  dt_dev_distort_backtransform_plus(self->dev->virtual_pipe, self->iop_order, DT_DEV_TRANSFORM_DIR_FORW_EXCL, pts, 2);
  dt_dev_pixelpipe_iop_t *piece = dt_dev_distort_get_iop_pipe(self->dev->virtual_pipe, self);
  pts[0] /= (float)piece->buf_out.width;
  pts[2] /= (float)piece->buf_out.width;
  pts[1] /= (float)piece->buf_out.height;
  pts[3] /= (float)piece->buf_out.height;

  // directly compute the line angle from segment AB and keep one representative modulo PI
  const float eps = .0001f;
  const float diff_x = pts[2] - pts[0];
  const float diff_y = pts[3] - pts[1];
  if(fabsf(diff_x) <= eps && fabsf(diff_y) <= eps) return 9;
  float v = atan2f(diff_y, diff_x);
  *rotation = -v * 180.0f / M_PI;

  // and now we go for the offset (more easy)
  const float sinv = sinf(v);
  const float cosv = cosf(v);
  const float ofs = (-2.0f * sinv * pts[0]) + sinv - cosv + 1.0f + (2.0f * cosv * pts[1]);

  *offset = ofs * 50.0f;

  return 1;
}

static int set_points_from_grad(struct dt_iop_module_t *self, grad_point_t *a, grad_point_t *b,
                                const float rotation, const float offset)
{
  // we get the extremities of the line
  const float v = (-rotation / 180) * M_PI;
  const float sinv = sinf(v);
  const float cosv = cosf(v);
  const float eps = 1e-6f;
  dt_boundingbox_t pts;

  dt_dev_pixelpipe_iop_t *piece = dt_dev_distort_get_iop_pipe(self->dev->virtual_pipe, self);
  if(IS_NULL_PTR(piece)) return 0;
  float wp = piece->buf_out.width, hp = piece->buf_out.height;

  const float off = offset / 100.0f;

  if(fabsf(sinv) <= eps) // horizontal: swap x ends and y offset sign depending on rotation direction
  {
    const int fwd = (cosv > 0.0f);
    pts[0] = wp * (fwd ? 0.1f : 0.9f);
    pts[2] = wp * (fwd ? 0.9f : 0.1f);
    pts[1] = pts[3] = hp * (fwd ? off : (1.0f - off));
  }
  else if(fabsf(fabsf(sinv) - 1.0f) <= eps) // vertical: swap y ends and x offset sign depending on rotation direction
  {
    const int fwd = (sinv < 0.0f);
    pts[0] = pts[2] = wp * (fwd ? off : (1.0f - off));
    pts[1] = hp * (fwd ? 0.9f : 0.1f);
    pts[3] = hp * (fwd ? 0.1f : 0.9f);
  }
  else
  {
    // otherwise we determine the extremities
    float xx1 = (sinv - cosv + 1.0f - offset / 50.0f) * wp * 0.5f / sinv;
    float xx2 = (sinv + cosv + 1.0f - offset / 50.0f) * wp * 0.5f / sinv;
    float yy1 = 0.0f;
    float yy2 = hp;
    const float aa = hp / (xx2 - xx1);
    const float bb = -xx1 * aa;

    // clamp extremities to image width and recompute y from the line equation y = a*x + b
    xx1 = CLAMP(xx1, 0.0f, wp);
    yy1 = aa * xx1 + bb;
    xx2 = CLAMP(xx2, 0.0f, wp);
    yy2 = aa * xx2 + bb;

    // inset extremities away from image border by 10%
    const float dx = xx2 - xx1;
    const float dy = yy2 - yy1;
    xx1 += dx * 0.1f;
    yy1 += dy * 0.1f;
    xx2 -= dx * 0.1f;
    yy2 -= dy * 0.1f;

    // near rotation: ax < bx; far rotation: bx < ax — in both cases just pick which end goes first
    const int first_is_xx1 = (rotation < 90.0f && rotation > -90.0f) ? (xx1 < xx2) : (xx1 > xx2);
    if(first_is_xx1)
    {
      pts[0] = xx1;
      pts[1] = yy1;
      pts[2] = xx2;
      pts[3] = yy2;
    }
    else
    {
      pts[0] = xx2;
      pts[1] = yy2;
      pts[2] = xx1;
      pts[3] = yy1;
    }
  }
  // now we want that points to take care of distort modules

  if(!dt_dev_distort_transform_plus(self->dev->virtual_pipe, self->iop_order, DT_DEV_TRANSFORM_DIR_FORW_EXCL, pts, 2))
    return 0;
  dt_dev_coordinates_image_abs_to_image_norm(self->dev, pts, 2);
  a->x = pts[0];
  a->y = pts[1];
  b->x = pts[2];
  b->y = pts[3];
  return 1;
}

static inline void update_saturation_slider_end_color(GtkWidget *slider, float hue)
{
  dt_aligned_pixel_t rgb;
  hsl2rgb(rgb, hue, 1.0, 0.5);
  dt_bauhaus_slider_set_stop(slider, 1.0, rgb[0], rgb[1], rgb[2]);
}

void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_graduatednd_gui_data_t *g = (dt_iop_graduatednd_gui_data_t *)self->gui_data;
  dt_iop_graduatednd_params_t *p = (dt_iop_graduatednd_params_t *)self->params;

  // convert picker RGB 2 HSL
  float H = .0f, S = .0f, L = .0f;
  rgb2hsl(self->picked_color, &H, &S, &L);

  if(fabsf(p->hue - H) < 0.0001f && fabsf(p->saturation - S) < 0.0001f)
  {
    // interrupt infinite loops
    return;
  }

  p->hue        = H;
  p->saturation = S;

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->hue, p->hue);
  dt_bauhaus_slider_set(g->saturation, p->saturation);
  update_saturation_slider_end_color(g->saturation, p->hue);
  --darktable.gui->reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE);
}

void gui_reset(struct dt_iop_module_t *self)
{
  dt_iop_color_picker_reset(self, TRUE);
}

void gui_post_expose(struct dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height,
                     int32_t pointerx, int32_t pointery)
{
  dt_develop_t *dev = self->dev;
  dt_iop_graduatednd_gui_data_t *g = (dt_iop_graduatednd_gui_data_t *)self->gui_data;
  dt_iop_graduatednd_params_t *p = (dt_iop_graduatednd_params_t *)self->params;

  const float zoom_scale = dev->roi.scaling;
  dt_dev_rescale_roi(dev, cr, width, height);

  // we get the extremities of the line
  if(g->define == 0)
  {
    if(!set_points_from_grad(self, &g->a, &g->b, p->rotation, p->offset))
      return;
    g->define = 1;
  }

  float line[4] = { g->a.x, g->a.y, g->b.x, g->b.y };
  dt_dev_coordinates_image_norm_to_preview_abs(dev, line, 2);
  grad_point_t a = { line[0], line[1] };
  grad_point_t b = { line[2], line[3] };

  // the lines
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  if(g->selected == 3 || g->dragging == 3)
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(5.0) / zoom_scale);
  else
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(3.0) / zoom_scale);
  dt_draw_set_color_overlay(cr, FALSE, 0.8);

  cairo_move_to(cr, a.x, a.y);
  cairo_line_to(cr, b.x, b.y);
  cairo_stroke(cr);

  if(g->selected == 3 || g->dragging == 3)
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.0) / zoom_scale);
  else
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.0) / zoom_scale);
  dt_draw_set_color_overlay(cr, TRUE, 0.8);
  cairo_move_to(cr, a.x, a.y);
  cairo_line_to(cr, b.x, b.y);
  cairo_stroke(cr);

  // the extremities
  _draw_end_marker(cr, a, b, zoom_scale, 1.0f, g->selected == 1 || g->dragging == 1);
  _draw_end_marker(cr, b, a, zoom_scale, -1.0f, g->selected == 2 || g->dragging == 2);
}

int mouse_moved(struct dt_iop_module_t *self, double x, double y, double pressure, int which)
{
  dt_iop_graduatednd_gui_data_t *g = (dt_iop_graduatednd_gui_data_t *)self->gui_data;
  float pzxpy[2] = { (float)x, (float)y };
  dt_dev_coordinates_widget_to_image_norm(self->dev, pzxpy, 1);
  float pzx = pzxpy[0];
  float pzy = pzxpy[1];

  // are we dragging something ?
  if(g->dragging > 0)
  {
    if(g->dragging == 1)
    {
      // we are dragging a
      g->a.x = pzx;
      g->a.y = pzy;
    }
    else if(g->dragging == 2)
    {
      // we are dragging b
      g->b.x = pzx;
      g->b.y = pzy;
    }
    else if(g->dragging == 3)
    {
      // we are dragging the entire line
      g->a.x += pzx - g->oldx;
      g->b.x += pzx - g->oldx;
      g->a.y += pzy - g->oldy;
      g->b.y += pzy - g->oldy;
      g->oldx = pzx;
      g->oldy = pzy;
    }
  }
  else
  {
    g->selected = 0;
    float ext[2] = { DT_GUI_MOUSE_EFFECT_RADIUS_SCALED, 0 };
    dt_dev_coordinates_image_abs_to_image_norm(self->dev, ext, 1);

    const float ext2 = ext[0] * ext[0];

    const grad_point_t pz = { pzx, pzy };
    const float da_x = pz.x - g->a.x;
    const float da_y = pz.y - g->a.y;
    const float db_x = pz.x - g->b.x;
    const float db_y = pz.y - g->b.y;

    // are we near extermity ?
    if(da_x * da_x + da_y * da_y < ext2)
    {
      g->selected = 1;
    }
    else if(db_x * db_x + db_y * db_y < ext2)
    {
      g->selected = 2;
    }
    else if(_dist_seg(g->a, g->b, pz) < ext2 * 0.5f)
      g->selected = 3;
  }

  if(g->selected > 0 || g->dragging > 0)
    dt_control_queue_redraw_center();

  return 1;
}

int button_pressed(struct dt_iop_module_t *self, double x, double y, double pressure, int which, int type,
                   uint32_t state)
{
  dt_iop_graduatednd_gui_data_t *g = (dt_iop_graduatednd_gui_data_t *)self->gui_data;
  float pzxpy[2] = { (float)x, (float)y };
  dt_dev_coordinates_widget_to_image_norm(self->dev, pzxpy, 1);
  float pzx = pzxpy[0];
  float pzy = pzxpy[1];

  if(which == 3)
  {
    // creating a line with right click
    g->dragging = 2;
    g->a.x = pzx;
    g->a.y = pzy;
    g->b.x = pzx;
    g->b.y = pzy;
    g->oldx = pzx;
    g->oldy = pzy;
    return 1;
  }
  else if(g->selected > 0 && which == 1)
  {
    g->dragging = g->selected;
    g->oldx = pzx;
    g->oldy = pzy;
    return 1;
  }
  g->dragging = 0;
  return 0;
}

int button_released(struct dt_iop_module_t *self, double x, double y, int which, uint32_t state)
{
  dt_iop_graduatednd_gui_data_t *g = (dt_iop_graduatednd_gui_data_t *)self->gui_data;
  dt_iop_graduatednd_params_t *p = (dt_iop_graduatednd_params_t *)self->params;
  if(g->dragging > 0)
  {
    float rotation = 0.0;
    float offset = 0.0;
    set_grad_from_points(self, &g->a, &g->b, &rotation, &offset);

    // if this is a "line dragging, we reset extremities, to be sure they are not outside the image
    if(g->dragging == 3)
    {
      // whole line dragging should not change rotation, so we should reuse
      // old rotation to avoid rounding issues

      rotation = p->rotation;
      set_points_from_grad(self, &g->a, &g->b, rotation, offset);
    }
    ++darktable.gui->reset;
    dt_bauhaus_slider_set(g->rotation, rotation);
    --darktable.gui->reset;
    p->rotation = rotation;
    p->offset = offset;
    g->dragging = 0;
    dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE);
  }

  g->dragging = 0;
  return 0;
}

int scrolled(dt_iop_module_t *self, double x, double y, int up, uint32_t state)
{
  dt_iop_graduatednd_gui_data_t *g = (dt_iop_graduatednd_gui_data_t *)self->gui_data;
  dt_iop_graduatednd_params_t *p = (dt_iop_graduatednd_params_t *)self->params;
  if(dt_modifier_is(state, GDK_CONTROL_MASK))
  {
    float dens;
    if(up)
      dens = fminf(8.0, p->density + 0.1);
    else
      dens = fmaxf(-8.0, p->density - 0.1);
    if(dens != p->density)
    {
      dt_bauhaus_slider_set(g->density, dens);
    }
    return 1;
  }
  if(dt_modifier_is(state, GDK_SHIFT_MASK))
  {
    float comp;
    if(up)
      comp = fminf(100.0, p->hardness + 1.0);
    else
      comp = fmaxf(0.0, p->hardness - 1.0);
    if(comp != p->hardness)
    {
      dt_bauhaus_slider_set(g->hardness, comp);
    }
    return 1;
  }
  return 0;
}

__OMP_DECLARE_SIMD__(simdlen(4))
static inline float density_times_length(const float dens, const float length)
{
//  return (dens * CLIP(0.5f + length) / 8.0f);
  return (dens * CLAMP(0.5f + length, 0.0f, 1.0f) / 8.0f);
}

__OMP_DECLARE_SIMD__(simdlen(4))
static inline float compute_density(const float dens, const float length)
{
#if 1
  // !!! approximation is ok only when highest density is 8
  // for input x = (data->density * CLIP( 0.5+length ), calculate 2^x as (e^(ln2*x/8))^8
  // use exp2f approximation to calculate e^(ln2*x/8)
  // in worst case - density==8,CLIP(0.5-length) == 1.0 it gives 0.6% of error
  const float t = DT_M_LN2f * density_times_length(dens,length);
  const float d1 = t * t * 0.5f;
  const float d2 = d1 * t * 0.333333333f;
  const float d3 = d2 * t * 0.25f;
  const float d = 1 + t + d1 + d2 + d3; /* taylor series for e^x till x^4 */
  // printf("%d %d  %f\n",y,x,d);
  float density = d * d;
  density = density * density;
  density = density * density;
#else
  // use fair exp2f
  const float density = exp2f(dens * CLIP(0.5f + length));
#endif
  return density;
}

__DT_CLONE_TARGETS__
int process(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid)
{
  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  const dt_iop_graduatednd_data_t *const data = (const dt_iop_graduatednd_data_t *const)piece->data;
  const int ch = piece->dsc_in.channels;

  const int ix = (roi_in->x);
  const int iy = (roi_in->y);
  const float iw = piece->buf_in.width * roi_out->scale;
  const float ih = piece->buf_in.height * roi_out->scale;
  const float hw = iw / 2.0f;
  const float hh = ih / 2.0f;
  const float hw_inv = 1.0f / hw;
  const float hh_inv = 1.0f / hh;
  const float v = (-data->rotation / 180) * M_PI;
  const float sinv = sinf(v);
  const float cosv = cosf(v);
  const float filter_radie = sqrtf((hh * hh) + (hw * hw)) / hh;
  const float offset = data->offset / 100.0f * 2;

  const float filter_hardness = 1.0 / filter_radie / (1.0 - (0.5 + (data->hardness / 100.0) * 0.9 / 2.0)) * 0.5;

  const int width = roi_out->width;
  const int height = roi_out->height;
  if(data->density > 0)
  {
    __OMP_PARALLEL_FOR__()
    for(int y = 0; y < height; y++)
    {
      const size_t k = (size_t)width * y * ch;
      const float *const restrict in = (float *)ivoid + k;
      float *const restrict out = (float *)ovoid + k;

      float length = (sinv * (-1.0 + ix * hw_inv) - cosv * (-1.0 + (iy + y) * hh_inv) - 1.0 + offset)
                     * filter_hardness;
      const float length_inc = sinv * hw_inv * filter_hardness;

      for(int x = 0; x < width; x++)
      {
        const float density = compute_density(data->density, length);
        __OMP_SIMD__(aligned(in, out : 16))
        for(int l = 0; l < 4; l++)
        {
          out[ch*x+l] = MAX(0.0f, (in[ch*x+l] / (data->color[l] + data->color1[l] * density)));
        }
        length += length_inc;
      }
    }
  }
  else
  {
    __OMP_PARALLEL_FOR__()
    for(int y = 0; y < height; y++)
    {
      const size_t k = (size_t)width * y * ch;
      const float *const restrict in = (float *)ivoid + k;
      float *const restrict out = (float *)ovoid + k;

      float length = (sinv * (-1.0f + ix * hw_inv) - cosv * (-1.0f + (iy + y) * hh_inv) - 1.0f + offset)
                     * filter_hardness;
      const float length_inc = sinv * hw_inv * filter_hardness;

      for(int x = 0; x < width; x++)
      {
        const float density = compute_density(-data->density, -length);
        __OMP_SIMD__(aligned(in, out : 16))
        for(int l = 0; l < 4; l++)
        {
          out[ch*x+l] = MAX(0.0f, (in[ch*x+l] * (data->color[l] + data->color1[l] * density)));
        }
        length += length_inc;
      }
    }
  }

  if(pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
    dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
  return 0;
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out)
{
  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  dt_iop_graduatednd_data_t *data = (dt_iop_graduatednd_data_t *)piece->data;
  dt_iop_graduatednd_global_data_t *gd = (dt_iop_graduatednd_global_data_t *)self->global_data;

  cl_int err = -999;
  const int devid = pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  const int ix = (roi_in->x);
  const int iy = (roi_in->y);
  const float iw = piece->buf_in.width * roi_out->scale;
  const float ih = piece->buf_in.height * roi_out->scale;
  const float hw = iw / 2.0f;
  const float hh = ih / 2.0f;
  const float hw_inv = 1.0f / hw;
  const float hh_inv = 1.0f / hh;
  const float v = (-data->rotation / 180) * M_PI;
  const float sinv = sinf(v);
  const float cosv = cosf(v);
  const float filter_radie = sqrtf((hh * hh) + (hw * hw)) / hh;
  const float offset = data->offset / 100.0f * 2;
  const float density = data->density;

#if 1
  const float filter_hardness = 1.0 / filter_radie
                                   / (1.0 - (0.5 + (data->hardness / 100.0) * 0.9 / 2.0)) * 0.5;
#else
  const float hardness = data->hardness / 100.0f;
  const float t = 1.0f - .8f / (.8f + hardness);
  const float c = 1.0f + 1000.0f * powf(4.0, hardness);
#endif

  const float length_base = (sinv * (-1.0 + ix * hw_inv) - cosv * (-1.0 + iy * hh_inv) - 1.0 + offset)
                            * filter_hardness;
  const float length_inc_y = -cosv * hh_inv * filter_hardness;
  const float length_inc_x = sinv * hw_inv * filter_hardness;

  size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };

  int kernel = density > 0 ? gd->kernel_graduatedndp : gd->kernel_graduatedndm;

  dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, kernel, 4, 4 * sizeof(float), (void *)data->color);
  dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(float), (void *)&density);
  dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(float), (void *)&length_base);
  dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(float), (void *)&length_inc_x);
  dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(float), (void *)&length_inc_y);
  err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
  if(err != CL_SUCCESS) goto error;
  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_graduatednd] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void init_global(dt_iop_module_so_t *module)
{
  const int program = 8; // extended.cl, from programs.conf
  dt_iop_graduatednd_global_data_t *gd
      = (dt_iop_graduatednd_global_data_t *)malloc(sizeof(dt_iop_graduatednd_global_data_t));
  module->data = gd;
  gd->kernel_graduatedndp = dt_opencl_create_kernel(program, "graduatedndp");
  gd->kernel_graduatedndm = dt_opencl_create_kernel(program, "graduatedndm");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_graduatednd_global_data_t *gd = (dt_iop_graduatednd_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_graduatedndp);
  dt_opencl_free_kernel(gd->kernel_graduatedndm);
  dt_free(module->data);
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_graduatednd_params_t *p = (dt_iop_graduatednd_params_t *)self->params;
  dt_iop_graduatednd_gui_data_t *g = (dt_iop_graduatednd_gui_data_t *)self->gui_data;
  if(w == g->rotation)
  {
    set_points_from_grad(self, &g->a, &g->b, p->rotation, p->offset);
  }
  else if(w == g->hue)
  {
    update_saturation_slider_end_color(g->saturation, p->hue);
    gtk_widget_queue_draw(g->saturation);
  }
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_graduatednd_params_t *p = (dt_iop_graduatednd_params_t *)p1;
  dt_iop_graduatednd_data_t *d = (dt_iop_graduatednd_data_t *)piece->data;

  d->density = p->density;
  d->hardness = p->hardness;
  d->rotation = p->rotation;
  d->offset = p->offset;

  hsl2rgb(d->color, p->hue, p->saturation, 0.5);
  d->color[3] = 0.0f;

  if(d->density < 0)
    for(int l = 0; l < 4; l++) d->color[l] = 1.0 - d->color[l];

  for(int l = 0; l < 4; l++) d->color1[l] = 1.0 - d->color[l];
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc_align(sizeof(dt_iop_graduatednd_data_t));
  piece->data_size = sizeof(dt_iop_graduatednd_data_t);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_free_align(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_graduatednd_gui_data_t *g = (dt_iop_graduatednd_gui_data_t *)self->gui_data;
  dt_iop_graduatednd_params_t *p = (dt_iop_graduatednd_params_t *)self->params;

  dt_iop_color_picker_reset(self, TRUE);

  g->define = 0;
  update_saturation_slider_end_color(g->saturation, p->hue);
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_graduatednd_gui_data_t *g = IOP_GUI_ALLOC(graduatednd);

  g->density = dt_bauhaus_slider_from_params(self, "density");
  dt_bauhaus_slider_set_format(g->density, _(" EV"));
  gtk_widget_set_tooltip_text(g->density, _("the density in EV for the filter"));

  g->hardness = dt_bauhaus_slider_from_params(self, "hardness");
  dt_bauhaus_slider_set_format(g->hardness, "%");
  /* xgettext:no-c-format */
  gtk_widget_set_tooltip_text(g->hardness, _("hardness of graduation:\n0% = soft, 100% = hard"));

  g->rotation = dt_bauhaus_slider_from_params(self, "rotation");
  dt_bauhaus_slider_set_format(g->rotation, "\302\260");
  gtk_widget_set_tooltip_text(g->rotation, _("rotation of filter -180 to 180 degrees"));

  g->hue = dt_color_picker_new(self, DT_COLOR_PICKER_POINT, dt_bauhaus_slider_from_params(self, "hue"));
  dt_bauhaus_slider_set_feedback(g->hue, 0);
  dt_bauhaus_slider_set_factor(g->hue, 360.0f);
  dt_bauhaus_slider_set_format(g->hue, "\302\260");
  dt_bauhaus_slider_set_stop(g->hue, 0.0f, 1.0f, 0.0f, 0.0f);
  dt_bauhaus_slider_set_stop(g->hue, 0.166f, 1.0f, 1.0f, 0.0f);
  dt_bauhaus_slider_set_stop(g->hue, 0.322f, 0.0f, 1.0f, 0.0f);
  dt_bauhaus_slider_set_stop(g->hue, 0.498f, 0.0f, 1.0f, 1.0f);
  dt_bauhaus_slider_set_stop(g->hue, 0.664f, 0.0f, 0.0f, 1.0f);
  dt_bauhaus_slider_set_stop(g->hue, 0.830f, 1.0f, 0.0f, 1.0f);
  dt_bauhaus_slider_set_stop(g->hue, 1.0f, 1.0f, 0.0f, 0.0f);
  gtk_widget_set_tooltip_text(g->hue, _("select the hue tone of filter"));

  g->saturation = dt_bauhaus_slider_from_params(self, "saturation");
  dt_bauhaus_slider_set_format(g->saturation, "%");
  dt_bauhaus_slider_set_stop(g->saturation, 0.0f, 0.2f, 0.2f, 0.2f);
  dt_bauhaus_slider_set_stop(g->saturation, 1.0f, 1.0f, 1.0f, 1.0f);
  gtk_widget_set_tooltip_text(g->saturation, _("select the saturation of filter"));

  g->selected = 0;
  g->dragging = 0;
  g->define = 0;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
