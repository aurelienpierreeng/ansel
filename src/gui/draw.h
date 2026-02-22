/*
    This file is part of darktable,
    Copyright (C) 2009-2013, 2016 johannes hanika.
    Copyright (C) 2010-2011 Henrik Andersson.
    Copyright (C) 2010, 2012-2014, 2016, 2018 Tobias Ellinghaus.
    Copyright (C) 2011 Jochen Schroeder.
    Copyright (C) 2011 Robert Bieber.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2014, 2016 Roman Lebedev.
    Copyright (C) 2018-2020, 2025 Aurélien PIERRE.
    Copyright (C) 2019 Andreas Schneider.
    Copyright (C) 2019 Edgardo Hoszowski.
    Copyright (C) 2019 Heiko Bauke.
    Copyright (C) 2019 Jakub Filipowicz.
    Copyright (C) 2019-2020, 2022 Pascal Obry.
    Copyright (C) 2019 Philippe Weyland.
    Copyright (C) 2020, 2022 Chris Elston.
    Copyright (C) 2020-2021 Dan Torop.
    Copyright (C) 2020 Ralf Brown.
    Copyright (C) 2022 Aldric Renaudin.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2025 Alynx Zhou.
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

#pragma once

/** some common drawing routines. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common/curve_tools.h"
#include "common/darktable.h"
#include "common/splines.h"
#include "control/conf.h"
#include "develop/develop.h"
#include <cairo.h>
#include <glib.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <gui/gtk.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef M_PI
#define M_PI 3.141592654
#endif


/** line sizes for drawing */
#define DT_DRAW_SIZE_LINE                      DT_PIXEL_APPLY_DPI(1.5f)
#define DT_DRAW_SIZE_LINE_SELECTED             DT_PIXEL_APPLY_DPI(3.0f)
#define DT_DRAW_SIZE_LINE_HIGHLIGHT            (DT_PIXEL_APPLY_DPI(4.0f) + DT_DRAW_SIZE_LINE)
#define DT_DRAW_SIZE_LINE_HIGHLIGHT_SELECTED   (DT_PIXEL_APPLY_DPI(5.0f) + DT_DRAW_SIZE_LINE_SELECTED)
#define DT_DRAW_SIZE_CROSS                     DT_PIXEL_APPLY_DPI(7.0f)

/** stuff's scale */
#define DT_DRAW_SCALE_DASH          DT_PIXEL_APPLY_DPI(12.0f)
#define DT_DRAW_SCALE_ARROW         DT_PIXEL_APPLY_DPI(18.0f)

// radius/width of node (handles are set to be 3/4 of a node size)
#define DT_DRAW_RADIUS_NODE          DT_PIXEL_APPLY_DPI(5.0f)
#define DT_DRAW_RADIUS_NODE_SELECTED (1.25f * DT_DRAW_RADIUS_NODE)

// used to detect the area where rotation of a shape is possible
#define DT_DRAW_SELECTION_ROTATION_AREA           DT_PIXEL_APPLY_DPI(50.0f)
#define DT_DRAW_SELECTION_ROTATION_RADIUS(dev)   (DT_DRAW_SELECTION_ROTATION_AREA / dt_dev_get_zoom_level((dt_develop_t *)dev))  

/**dash type */
typedef enum dt_draw_dash_type_t
{
  DT_MASKS_NO_DASH = 0,
  DT_MASKS_DASH_STICK = 1,
  DT_MASKS_DASH_ROUND = 2
} dt_draw_dash_type_t;

/** wrapper around nikon curve. */
typedef struct dt_draw_curve_t
{
  CurveData c;
  CurveSample csample;
} dt_draw_curve_t;

/** set color based on gui overlay preference */
static inline void dt_draw_set_color_overlay(cairo_t *cr, gboolean bright, double alpha)
{
  double amt;

  if(bright)
    amt = 0.5 + darktable.gui->overlay_contrast * 0.5;
  else
    amt = (1.0 - darktable.gui->overlay_contrast) * 0.5;

  cairo_set_source_rgba(cr, darktable.gui->overlay_red * amt, darktable.gui->overlay_green * amt, darktable.gui->overlay_blue * amt, alpha);
}

/** draws a rating star
 */
static inline void dt_draw_star(cairo_t *cr, float x, float y, float r1, float r2)
{
  const float d = 2.0 * M_PI * 0.1f;
  const float dx[10] = { sinf(0.0),   sinf(d),     sinf(2 * d), sinf(3 * d), sinf(4 * d),
                         sinf(5 * d), sinf(6 * d), sinf(7 * d), sinf(8 * d), sinf(9 * d) };
  const float dy[10] = { cosf(0.0),   cosf(d),     cosf(2 * d), cosf(3 * d), cosf(4 * d),
                         cosf(5 * d), cosf(6 * d), cosf(7 * d), cosf(8 * d), cosf(9 * d) };

  cairo_move_to(cr, x + r1 * dx[0], y - r1 * dy[0]);
  for(int k = 1; k < 10; k++)
    if(k & 1)
      cairo_line_to(cr, x + r2 * dx[k], y - r2 * dy[k]);
    else
      cairo_line_to(cr, x + r1 * dx[k], y - r1 * dy[k]);
  cairo_close_path(cr);
}

static inline void dt_draw_line(cairo_t *cr, float left, float top, float right, float bottom)
{
  cairo_move_to(cr, left, top);
  cairo_line_to(cr, right, bottom);
}

static inline void dt_draw_grid(cairo_t *cr, const int num, const int left, const int top, const int right,
                                const int bottom)
{
  float width = right - left;
  float height = bottom - top;

  for(int k = 1; k < num; k++)
  {
    dt_draw_line(cr, left + k / (float)num * width, top, left + k / (float)num * width, bottom);
    cairo_stroke(cr);
    dt_draw_line(cr, left, top + k / (float)num * height, right, top + k / (float)num * height);
    cairo_stroke(cr);
  }
}

static inline float dt_curve_to_mouse(const float x, const float zoom_factor, const float offset)
{
  return (x - offset) * zoom_factor;
}

/* left, right, top, bottom are in curve coordinates [0..1] */
static inline void dt_draw_grid_zoomed(cairo_t *cr, const int num, const float left, const float top,
                                       const float right, const float bottom, const float width,
                                       const float height, const float zoom_factor, const float zoom_offset_x,
                                       const float zoom_offset_y)
{
  for(int k = 1; k < num; k++)
  {
    dt_draw_line(cr, dt_curve_to_mouse(left + k / (float)num, zoom_factor, zoom_offset_x) * width,
                 dt_curve_to_mouse(top, zoom_factor, zoom_offset_y) * -height,
                 dt_curve_to_mouse(left + k / (float)num, zoom_factor, zoom_offset_x) * width,
                 dt_curve_to_mouse(bottom, zoom_factor, zoom_offset_y) * -height);
    cairo_stroke(cr);

    dt_draw_line(cr, dt_curve_to_mouse(left, zoom_factor, zoom_offset_x) * width,
                 dt_curve_to_mouse(top + k / (float)num, zoom_factor, zoom_offset_y) * -height,
                 dt_curve_to_mouse(right, zoom_factor, zoom_offset_x) * width,
                 dt_curve_to_mouse(top + k / (float)num, zoom_factor, zoom_offset_y) * -height);
    cairo_stroke(cr);
  }
}

#ifdef _OPENMP
#pragma omp declare simd uniform(base)
#endif
static inline float dt_log_scale_axis(const float x, const float base)
{
  return logf(x * (base - 1.0f) + 1.f) / logf(base);
}

static inline void dt_draw_loglog_grid(cairo_t *cr, const int num, const int left, const int top, const int right,
                                       const int bottom, const float base)
{
  float width = right - left;
  float height = bottom - top;

  for(int k = 1; k < num; k++)
  {
    const float x = dt_log_scale_axis(k / (float)num, base);
    dt_draw_line(cr, left + x * width, top, left + x * width, bottom);
    cairo_stroke(cr);
    dt_draw_line(cr, left, top + x * height, right, top + x * height);
    cairo_stroke(cr);
  }
}

static inline void dt_draw_semilog_x_grid(cairo_t *cr, const int num, const int left, const int top,
                                          const int right, const int bottom, const float base)
{
  float width = right - left;
  float height = bottom - top;

  for(int k = 1; k < num; k++)
  {
    const float x = dt_log_scale_axis(k / (float)num, base);
    dt_draw_line(cr, left + x * width, top, left + x * width, bottom);
    cairo_stroke(cr);
    dt_draw_line(cr, left, top + k / (float)num * height, right, top + k / (float)num * height);
    cairo_stroke(cr);
  }
}

static inline void dt_draw_semilog_y_grid(cairo_t *cr, const int num, const int left, const int top,
                                          const int right, const int bottom, const float base)
{
  float width = right - left;
  float height = bottom - top;

  for(int k = 1; k < num; k++)
  {
    const float x = dt_log_scale_axis(k / (float)num, base);
    dt_draw_line(cr, left + k / (float)num * width, top, left + k / (float)num * width, bottom);
    cairo_stroke(cr);
    dt_draw_line(cr, left, top + x * height, right, top + x * height);
    cairo_stroke(cr);
  }
}


static inline void dt_draw_vertical_lines(cairo_t *cr, const int num, const int left, const int top,
                                          const int right, const int bottom)
{
  float width = right - left;

  for(int k = 1; k < num; k++)
  {
    cairo_move_to(cr, left + k / (float)num * width, top);
    cairo_line_to(cr, left + k / (float)num * width, bottom);
    cairo_stroke(cr);
  }
}

static inline void dt_draw_horizontal_lines(cairo_t *cr, const int num, const int left, const int top,
                                            const int right, const int bottom)
{
  float height = bottom - top;

  for(int k = 1; k < num; k++)
  {
    cairo_move_to(cr, left, top + k / (float)num * height);
    cairo_line_to(cr, right, top + k / (float)num * height);
    cairo_stroke(cr);
  }
}

static inline dt_draw_curve_t *dt_draw_curve_new(const float min, const float max, unsigned int type)
{
  dt_draw_curve_t *c = (dt_draw_curve_t *)malloc(sizeof(dt_draw_curve_t));
  c->csample.m_samplingRes = 0x10000;
  c->csample.m_outputRes = 0x10000;
  c->csample.m_Samples = (uint16_t *)malloc(sizeof(uint16_t) * 0x10000);

  c->c.m_spline_type = type;
  c->c.m_numAnchors = 0;
  c->c.m_min_x = 0.0;
  c->c.m_max_x = 1.0;
  c->c.m_min_y = 0.0;
  c->c.m_max_y = 1.0;
  return c;
}

static inline void dt_draw_curve_destroy(dt_draw_curve_t *c)
{
  free(c->csample.m_Samples);
  free(c);
}

static inline void dt_draw_curve_set_point(dt_draw_curve_t *c, const int num, const float x, const float y)
{
  c->c.m_anchors[num].x = x;
  c->c.m_anchors[num].y = y;
}

static inline void dt_draw_curve_smaple_values(dt_draw_curve_t *c, const float min, const float max, const int res,
                                               float *x, float *y)
{
  if(x)
  {
#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) dt_omp_firstprivate(res) shared(x) schedule(static)
#endif
    for(int k = 0; k < res; k++) x[k] = k * (1.0f / res);
  }
  if(y)
  {
#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) dt_omp_firstprivate(min, max, res) shared(y, c) schedule(static)
#endif
    for(int k = 0; k < res; k++) y[k] = min + (max - min) * c->csample.m_Samples[k] * (1.0f / 0x10000);
  }
}

static inline void dt_draw_curve_calc_values(dt_draw_curve_t *c, const float min, const float max, const int res,
                                             float *x, float *y)
{
  c->csample.m_samplingRes = res;
  c->csample.m_outputRes = 0x10000;
  CurveDataSample(&c->c, &c->csample);
  dt_draw_curve_smaple_values(c, min, max, res, x, y);
}

static inline void dt_draw_curve_calc_values_V2_nonperiodic(dt_draw_curve_t *c, const float min, const float max,
                                                            const int res, float *x, float *y)
{
  c->csample.m_samplingRes = res;
  c->csample.m_outputRes = 0x10000;
  CurveDataSampleV2(&c->c, &c->csample);
  dt_draw_curve_smaple_values(c, min, max, res, x, y);
}

static inline void dt_draw_curve_calc_values_V2_periodic(dt_draw_curve_t *c, const float min, const float max,
                                                         const int res, float *x, float *y)
{
  c->csample.m_samplingRes = res;
  c->csample.m_outputRes = 0x10000;
  CurveDataSampleV2Periodic(&c->c, &c->csample);
  dt_draw_curve_smaple_values(c, min, max, res, x, y);
}

static inline void dt_draw_curve_calc_values_V2(dt_draw_curve_t *c, const float min, const float max,
                                                const int res, float *x, float *y, const gboolean periodic)
{
  if(periodic)
    dt_draw_curve_calc_values_V2_periodic(c, min, max, res, x, y);
  else
    dt_draw_curve_calc_values_V2_nonperiodic(c, min, max, res, x, y);
 }

static inline float dt_draw_curve_calc_value(dt_draw_curve_t *c, const float x)
{
  float xa[20], ya[20];
  float val = 0.f;
  float *ypp = NULL;
  for(int i = 0; i < c->c.m_numAnchors; i++)
  {
    xa[i] = c->c.m_anchors[i].x;
    ya[i] = c->c.m_anchors[i].y;
  }
  ypp = interpolate_set(c->c.m_numAnchors, xa, ya, c->c.m_spline_type);
  if(ypp)
  {
    val = interpolate_val(c->c.m_numAnchors, xa, x, ya, ypp, c->c.m_spline_type);
    free(ypp);
  }
  return MIN(MAX(val, c->c.m_min_y), c->c.m_max_y);
}

static inline int dt_draw_curve_add_point(dt_draw_curve_t *c, const float x, const float y)
{
  c->c.m_anchors[c->c.m_numAnchors].x = x;
  c->c.m_anchors[c->c.m_numAnchors].y = y;
  c->c.m_numAnchors++;
  return 0;
}

// linear x linear y
static inline void dt_draw_histogram_8_linxliny(cairo_t *cr, const uint32_t *hist, int32_t channels,
                                                int32_t channel)
{
  cairo_move_to(cr, 0, 0);
  for(int k = 0; k < 256; k++) cairo_line_to(cr, k, hist[channels * k + channel]);
  cairo_line_to(cr, 255, 0);
  cairo_close_path(cr);
  cairo_fill(cr);
}

static inline void dt_draw_histogram_8_zoomed(cairo_t *cr, const uint32_t *hist, int32_t channels, int32_t channel,
                                              const float zoom_factor, const float zoom_offset_x,
                                              const float zoom_offset_y, gboolean linear)
{
  cairo_move_to(cr, -zoom_offset_x, -zoom_offset_y);
  for(int k = 0; k < 256; k++)
  {
    const float value = ((float)hist[channels * k + channel] - zoom_offset_y) * zoom_factor;
    const float hist_value = value < 0 ? 0.f : value;
    cairo_line_to(cr, ((float)k - zoom_offset_x) * zoom_factor, linear ? hist_value : logf(1.0f + hist_value));
  }
  cairo_line_to(cr, (255.f - zoom_offset_x), -zoom_offset_y * zoom_factor);
  cairo_close_path(cr);
  cairo_fill(cr);
}

// log x (scalable) & linear y
static inline void dt_draw_histogram_8_logxliny(cairo_t *cr, const uint32_t *hist, int32_t channels,
                                                int32_t channel, float base_log)
{
  cairo_move_to(cr, 0, 0);
  for(int k = 0; k < 256; k++)
  {
    const float x = logf((float)k / 255.0f * (base_log - 1.0f) + 1.0f) / logf(base_log) * 255.0f;
    const float y = hist[channels * k + channel];
    cairo_line_to(cr, x, y);
  }
  cairo_line_to(cr, 255, 0);
  cairo_close_path(cr);
  cairo_fill(cr);
}

// log x (scalable) & log y
static inline void dt_draw_histogram_8_logxlogy(cairo_t *cr, const uint32_t *hist, int32_t channels,
                                                int32_t channel, float base_log)
{
  cairo_move_to(cr, 0, 0);
  for(int k = 0; k < 256; k++)
  {
    const float x = logf((float)k / 255.0f * (base_log - 1.0f) + 1.0f) / logf(base_log) * 255.0f;
    const float y = logf(1.0 + hist[channels * k + channel]);
    cairo_line_to(cr, x, y);
  }
  cairo_line_to(cr, 255, 0);
  cairo_close_path(cr);
  cairo_fill(cr);
}

// linear x log y
static inline void dt_draw_histogram_8_linxlogy(cairo_t *cr, const uint32_t *hist, int32_t channels,
                                                int32_t channel)
{
  cairo_move_to(cr, 0, 0);
  for(int k = 0; k < 256; k++) cairo_line_to(cr, k, logf(1.0 + hist[channels * k + channel]));
  cairo_line_to(cr, 255, 0);
  cairo_close_path(cr);
  cairo_fill(cr);
}

// log x (scalable)
static inline void dt_draw_histogram_8_log_base(cairo_t *cr, const uint32_t *hist, int32_t channels,
                                                int32_t channel, const gboolean linear, float base_log)
{

  if(linear) // linear y
    dt_draw_histogram_8_logxliny(cr, hist, channels, channel, base_log);
  else // log y
    dt_draw_histogram_8_logxlogy(cr, hist, channels, channel, base_log);
}

// linear x
static inline void dt_draw_histogram_8(cairo_t *cr, const uint32_t *hist, int32_t channels, int32_t channel,
                                       const gboolean linear)
{
  if(linear) // linear y
    dt_draw_histogram_8_linxliny(cr, hist, channels, channel);
  else // log y
    dt_draw_histogram_8_linxlogy(cr, hist, channels, channel);
}

/** transform a data blob from cairo's premultiplied rgba/bgra to GdkPixbuf's un-premultiplied bgra/rgba */
static inline void dt_draw_cairo_to_gdk_pixbuf(uint8_t *data, unsigned int width, unsigned int height)
{
  for(uint32_t y = 0; y < height; y++)
    for(uint32_t x = 0; x < width; x++)
    {
      uint8_t *r, *g, *b, *a, tmp;
      r = &data[(y * width + x) * 4 + 0];
      g = &data[(y * width + x) * 4 + 1];
      b = &data[(y * width + x) * 4 + 2];
      a = &data[(y * width + x) * 4 + 3];

      // switch r and b
      tmp = *r;
      *r = *b;
      *b = tmp;

      // cairo uses premultiplied alpha, reverse that
      if(*a != 0)
      {
        float inv_a = 255.0 / *a;
        *r *= inv_a;
        *g *= inv_a;
        *b *= inv_a;
      }
    }
}

static inline void dt_cairo_perceptual_gradient(cairo_pattern_t *grad, double alpha)
{
  // Create a linear gradient from black to white
  cairo_pattern_add_color_stop_rgba(grad, 0.0, 0.0, 0.0, 0.0, alpha);
  cairo_pattern_add_color_stop_rgba(grad, 1.0, 1.0, 1.0, 1.0, alpha);
}

static inline GdkPixbuf *dt_draw_paint_to_pixbuf
 (GtkWidget *widget, const guint pixbuf_size, const int flags,
  void (*dtgtk_cairo_paint_fct)(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data))
{
  GdkRGBA fg_color;
  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  GtkStateFlags state = gtk_widget_get_state_flags(widget);
  gtk_style_context_get_color(context, state, &fg_color);

  const int dim = DT_PIXEL_APPLY_DPI(pixbuf_size);
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, dim, dim);
  cairo_t *cr = cairo_create(cst);
  gdk_cairo_set_source_rgba(cr, &fg_color);
  (*dtgtk_cairo_paint_fct)(cr, 0, 0, dim, dim, flags, NULL);
  cairo_destroy(cr);
  uint8_t *data = cairo_image_surface_get_data(cst);
  dt_draw_cairo_to_gdk_pixbuf(data, dim, dim);
  const size_t size = (size_t)dim * dim * 4;
  uint8_t *buf = (uint8_t *)malloc(size);
  memcpy(buf, data, size);
  GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data(buf, GDK_COLORSPACE_RGB, TRUE, 8, dim, dim, dim * 4,
                                               (GdkPixbufDestroyNotify)free, NULL);
  cairo_surface_destroy(cst);
  return pixbuf;
}

/***** SHAPES */

// Helper that fills the current path with CAIRO_OPERATOR_CLEAR,
// effectively erasing all drawings below,
// but preserving the path for further drawing.
static void _draw_fill_clear(cairo_t *cr, gboolean preserve)
{
  cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
  if(preserve)
    cairo_fill_preserve(cr);
  else
    cairo_fill(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
}

static void _fill_clear(cairo_t *cr)
{
  _draw_fill_clear(cr, FALSE);
}

static void _fill_clear_preserve(cairo_t *cr)
{
  _draw_fill_clear(cr, TRUE);
}

static inline void dt_draw_set_dash_style(cairo_t *cr, dt_draw_dash_type_t type, float zoom_scale)
{
  // return early if no dash is needed
  if(type == DT_MASKS_NO_DASH)
  {
    cairo_set_dash(cr, NULL, 0, 0);
    return;
  }

  double pattern[2];

  switch(type)
  {
    case DT_MASKS_NO_DASH:
      pattern[0] = 0.0f;
      pattern[1] = 0.0f;
      break;

    case DT_MASKS_DASH_STICK:
      pattern[0] = DT_DRAW_SCALE_DASH / zoom_scale;
      pattern[1] = DT_DRAW_SCALE_DASH / zoom_scale;
      break;

    case DT_MASKS_DASH_ROUND:
      pattern[0] = (DT_DRAW_SCALE_DASH * 0.25f) / zoom_scale;
      pattern[1] = DT_DRAW_SCALE_DASH / zoom_scale;
      break;

    default:
      cairo_set_dash(cr, NULL, 0, 0);
      return;
      
  }
  const int pattern_len = 2;
  cairo_set_dash(cr, pattern, pattern_len, 0);
}

/**
 * @brief Draw an node point of a mask.
 * 
 * @param cr the cairo context to draw into
 * @param square TRUE to draw a square node, FALSE to draw a round node
 * @param point_action TRUE if the point is selected or dragged
 * @param selected TRUE if the shape is selected
 * @param zoom_scale the current zoom scale of the image
 * @param x the center x position of the anchor
 * @param y the center y position of the anchor
 */
static inline void dt_draw_node(cairo_t *cr, const gboolean square, const gboolean point_action, const gboolean selected, const float zoom_scale, const float x, const float y)
{
  cairo_save(cr);

  const float node_width = (selected || point_action) ? DT_DRAW_RADIUS_NODE_SELECTED / zoom_scale : DT_DRAW_RADIUS_NODE / zoom_scale;
  // square for corner nodes, circle for others (curve)
  if(square)
  {
    const float pos = node_width * 0.7071f; // radius * sin(45°) to have the same diagonal as the circle
    cairo_rectangle(cr, x - pos, y - pos, node_width * 2.f, node_width * 2.f);
  }
  else
    cairo_arc(cr, x, y, node_width * 1.2f, 0.0, 2.0 * M_PI);

  // Erase all drawings below
  _fill_clear_preserve(cr);

  const float line_width = (point_action && selected) ? DT_DRAW_SIZE_LINE_SELECTED / zoom_scale
                                          : DT_DRAW_SIZE_LINE / zoom_scale;

  cairo_set_line_width(cr, line_width);
  dt_draw_set_color_overlay(cr, TRUE, selected || point_action ? 1 : 0.8);
  cairo_fill_preserve(cr);

  // draw dark border
  cairo_set_line_width(cr, (selected && !point_action) ? line_width * 2. : line_width);
  dt_draw_set_color_overlay(cr, FALSE, 0.8);
  cairo_stroke(cr);

  cairo_restore(cr);
}

/**
 * @brief Draw a control handle attached to a point with a tail between the node and the handle.
 *
 * @param cr the cairo context to draw into
 * @param pt_x the x position of the node point (-1 if no tail should be drawn)
 * @param pt_y the y position of the node point (-1 if no tail should be drawn)
 * @param zoom_scale the current zoom scale of the image
 * @param handle_x the x position of the handle point
 * @param handle_y the y position of the handle point
 * @param selected TRUE if the shape is selected
 * @param square TRUE to draw a square handle, FALSE to draw a round handle
 */
static inline void dt_draw_handle(cairo_t *cr, const float pt_x, const float pt_y, const float zoom_scale, 
                                          const float handle_x, const float handle_y, const gboolean selected, const gboolean square)
{
  // draw handle's tail if a size is specified
  if(pt_x >= 0 && pt_y >= 0)
  {
    cairo_save(cr);

    float delta_x = handle_x - pt_x;
    float delta_y = handle_y - pt_y;
    float tail_len = sqrtf(delta_x * delta_x + delta_y * delta_y);
    // Draw only if the line is long enough
    // and shorten the line by the size of the nodes so it does not overlap with them
    float shorten = (DT_DRAW_RADIUS_NODE / zoom_scale) * 0.5f;
    if(tail_len > (2 * shorten))
    {
      float start_x = pt_x + delta_x * (shorten / tail_len);
      float start_y = pt_y + delta_y * (shorten / tail_len);
      float end_x = handle_x - delta_x * (shorten / tail_len);
      float end_y = handle_y - delta_y * (shorten / tail_len);
      cairo_move_to(cr, start_x, start_y);
      cairo_line_to(cr, end_x, end_y);
    }

    cairo_set_line_width(cr, DT_DRAW_SIZE_LINE_HIGHLIGHT * 0.6 / zoom_scale);
    dt_draw_set_color_overlay(cr, FALSE, 0.6);
    cairo_stroke_preserve(cr);
    cairo_set_line_width(cr, DT_DRAW_SIZE_LINE * 0.6 / zoom_scale);
    dt_draw_set_color_overlay(cr, TRUE, 0.8);
    cairo_stroke(cr);

    cairo_restore(cr);
  }

  if(handle_x < 0 || handle_y < 0)
    return;

  cairo_save(cr);

  // Draw the control handle (1/4 smaller than a node)
  const float handle_radius = 0.75 * (selected ? DT_DRAW_RADIUS_NODE_SELECTED / zoom_scale
                                       : DT_DRAW_RADIUS_NODE / zoom_scale);

  if(square)
  {
    const float square_width = handle_radius * 0.7071f; // handle_radius * sin(45°) to have the same diagonal as the circle
    cairo_rectangle(cr, handle_x - square_width, handle_y - square_width, square_width * 2.f, square_width * 2.f);
  }
  else
    cairo_arc(cr, handle_x, handle_y, handle_radius, 0, 2.0 * M_PI);

  const float line_width_dark = selected
                  ? (DT_DRAW_SIZE_LINE_HIGHLIGHT_SELECTED / zoom_scale)
                  : (DT_DRAW_SIZE_LINE_HIGHLIGHT / zoom_scale);
  const float line_width_bright = selected
                  ? (DT_DRAW_SIZE_LINE_SELECTED / zoom_scale)
                  : (DT_DRAW_SIZE_LINE / zoom_scale);

  // OUTLINE (dark)
  cairo_set_line_width(cr, line_width_dark * 1.125);
  dt_draw_set_color_overlay(cr, FALSE, 0.5);
  cairo_stroke_preserve(cr);
  // NORMAL (bright)
  cairo_set_line_width(cr, line_width_bright * 1.5);
  dt_draw_set_color_overlay(cr, TRUE, 0.8);
  cairo_stroke_preserve(cr);
  // Erase all drawings below
  _fill_clear(cr);


  // uncomment this part if you want to see "real" control points
  /*cairo_move_to(cr, gpt->points[n*6+2],gpt->points[n*6+3]);
  cairo_line_to(cr, gpt->points[n*6],gpt->points[n*6+1]);
  cairo_stroke(cr);
  cairo_move_to(cr, gpt->points[n*6+2],gpt->points[n*6+3]);
  cairo_line_to(cr, gpt->points[n*6+4],gpt->points[n*6+5]);
  cairo_stroke(cr);*/

  cairo_restore(cr);
}

typedef void (*shape_draw_function_t)(cairo_t *cr, const float *points, const int points_count, const int nb, const gboolean border, const gboolean source);

/**
 * @brief Draw the lines of a mask shape.
 * 
 * @param dash_type the dash type to use
 * @param source TRUE if we draw the source shape (clone mask)
 * @param cr the cairo context to draw into
 * @param nb the number of coord by node
 * @param selected TRUE if the shape is selected
 * @param zoom_scale the current zoom scale of the image
 * @param points the points of the shape to draw
 * @param points_count the number of points in the shape
 * @param functions the functions table of the shape
 */
static inline void dt_draw_shape_lines(const dt_draw_dash_type_t dash_type, const gboolean source, cairo_t *cr, const int nb, const gboolean selected,
                const float zoom_scale, const float *points, const int points_count, const shape_draw_function_t *draw_shape_func, const cairo_line_cap_t line_cap)
{
  cairo_save(cr);
  
  cairo_set_line_cap(cr, line_cap);
  // Are we drawing a border ?
  const gboolean border = (dash_type != DT_MASKS_NO_DASH);  

  // Draw the shape from the integrated function if any
  if(points && points_count >= 2 && draw_shape_func)
    (*draw_shape_func)(cr, points, points_count, nb, border, FALSE);

  const dt_draw_dash_type_t dash = (dash_type && !source)
                                  ? dash_type : DT_MASKS_NO_DASH;

  dt_draw_set_dash_style(cr, dash, zoom_scale);

  const float line_width_dark = selected
                  ? DT_DRAW_SIZE_LINE_HIGHLIGHT_SELECTED / zoom_scale
                  : DT_DRAW_SIZE_LINE_HIGHLIGHT / zoom_scale;
  const float line_width_bright = selected
                  ? DT_DRAW_SIZE_LINE_SELECTED / zoom_scale
                  : DT_DRAW_SIZE_LINE / zoom_scale;
  
  // OUTLINE (dark)
  cairo_set_line_width(cr, line_width_dark);
  dt_draw_set_color_overlay(cr, FALSE, dash_type ? 0.3f : 0.9f);
  cairo_stroke_preserve(cr);

  // NORMAL (bright)
  cairo_set_line_width(cr, line_width_bright);
  dt_draw_set_color_overlay(cr, TRUE, 0.8);
  cairo_stroke(cr);
  
  cairo_restore(cr);
}

/**
 * @brief Stroke a line with style.
 * 
 * @param dash_type the dash type to use
 * @param source TRUE if we draw the source shape (clone mask)
 * @param cr the cairo context to draw into
 * @param selected TRUE if the shape is selected
 * @param zoom_scale the current zoom scale of the image
 */
static inline void dt_draw_stroke_line(const dt_draw_dash_type_t dash_type, const gboolean source, cairo_t *cr,
                          const gboolean selected, const float zoom_scale, const cairo_line_cap_t line_cap)
{
  dt_draw_shape_lines(dash_type, source, cr, 0, selected, zoom_scale, NULL, 0, NULL, line_cap);
}

static void _draw_arrow_head(cairo_t *cr, const float arrow[2], const float arrow_x_a, const float arrow_y_a,
                             const float arrow_x_b, const float arrow_y_b)
{
  //draw the arrow head
  cairo_move_to(cr, arrow_x_a, arrow_y_a);
  cairo_line_to(cr, arrow[0], arrow[1]);
  cairo_line_to(cr, arrow_x_b, arrow_y_b);
  // close the arrow head
  cairo_close_path(cr);
}

static void _draw_arrow_tail(cairo_t *cr, const float arrow_bud_x, const float arrow_bud_y,
                             const float tail[2], const gboolean draw_tail)
{
  if(draw_tail) dt_draw_line(cr, arrow_bud_x, arrow_bud_y, tail[0], tail[1]);
}

/**
 * @brief Draw an arrow with head and, if needed, tail.
 * The length of the arrow head is defined by DT_DRAW_SCALE_ARROW.
 * This is used for the clone mask source indicator.
 * 
 * @param cr the cairo context to draw into
 * @param zoom_scale the current zoom scale of the image
 * @param selected TRUE if the shape needs to be highlighted
 * @param draw_tail TRUE to draw the tail line
 * @param dash_style the dash style to use for the tail line
 * @param arrow the position of the arrow tip
 * @param tail the position of the tail point
 */
static inline void dt_draw_arrow(cairo_t *cr, const float zoom_scale,const gboolean selected, const gboolean draw_tail,
              const dt_draw_dash_type_t dash_style, const float arrow[2], const float tail[2], const float angle)
{
  // calculate the coordinates of the two base points of the arrow head
  const float arrow_x_a = arrow[0] + (DT_DRAW_SCALE_ARROW / zoom_scale) * cosf(angle + (0.4f));
  const float arrow_y_a = arrow[1] + (DT_DRAW_SCALE_ARROW / zoom_scale) * sinf(angle + (0.4f));
  const float arrow_x_b = arrow[0] + (DT_DRAW_SCALE_ARROW / zoom_scale) * cosf(angle - (0.4f));
  const float arrow_y_b = arrow[1] + (DT_DRAW_SCALE_ARROW / zoom_scale) * sinf(angle - (0.4f));
  // Calculate the coordinates of the arrow base's midpoint
  const float arrow_bud_x = (arrow_x_a + arrow_x_b) * 0.5f;
  const float arrow_bud_y = (arrow_y_a + arrow_y_b) * 0.5f;

  cairo_save(cr);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

  // we need to draw the arrow head and tail in two passes to get the dark and bright effect correctly
  
  // dark
  {
    // arrow head
    _draw_arrow_head(cr, arrow, arrow_x_a, arrow_y_a, arrow_x_b, arrow_y_b);
    // Erase all drawings below
    _fill_clear_preserve(cr);

    dt_draw_set_dash_style(cr, DT_MASKS_NO_DASH, zoom_scale);
    dt_draw_set_color_overlay(cr, FALSE, 0.6);

    if(selected)
      cairo_set_line_width(cr, 0.8f * DT_DRAW_SIZE_LINE_HIGHLIGHT_SELECTED / zoom_scale);
    else
      cairo_set_line_width(cr, 0.8f * DT_DRAW_SIZE_LINE_HIGHLIGHT / zoom_scale);
    cairo_stroke(cr);

    // arrow tail
    _draw_arrow_tail(cr, arrow_bud_x, arrow_bud_y, tail, draw_tail);
    dt_draw_set_dash_style(cr, dash_style, zoom_scale);
    dt_draw_set_color_overlay(cr, FALSE, 0.6);
    if(selected)
      cairo_set_line_width(cr, DT_DRAW_SIZE_LINE_HIGHLIGHT_SELECTED / zoom_scale);
    else
      cairo_set_line_width(cr, DT_DRAW_SIZE_LINE_HIGHLIGHT / zoom_scale);
    cairo_stroke(cr);
  }

  // bright
  {
    // arrow head
    _draw_arrow_head(cr, arrow, arrow_x_a, arrow_y_a, arrow_x_b, arrow_y_b);
    // erase all drawings below
    cairo_set_source_rgba(cr, 0., 0., 0., 0.);
    cairo_fill_preserve(cr);

    dt_draw_set_color_overlay(cr, TRUE, 0.8);
    dt_draw_set_dash_style(cr, DT_MASKS_NO_DASH, zoom_scale);
    if(selected)
      cairo_set_line_width(cr, (2 * DT_DRAW_SIZE_LINE) / zoom_scale);
    else
      cairo_set_line_width(cr, (DT_DRAW_SIZE_LINE) / zoom_scale);
    cairo_stroke(cr);

    // arrow tail
    _draw_arrow_tail(cr, arrow_bud_x, arrow_bud_y, tail, draw_tail);
    dt_draw_set_dash_style(cr, dash_style, zoom_scale);
    dt_draw_set_color_overlay(cr, TRUE, 0.8);
    if(selected)
      cairo_set_line_width(cr, (3 * DT_DRAW_SIZE_LINE) / zoom_scale);
    else
      cairo_set_line_width(cr, (2 * DT_DRAW_SIZE_LINE) / zoom_scale);
    cairo_stroke(cr);
  }
  cairo_restore(cr);
}

static inline void dt_draw_cross(cairo_t *cr, const float zoom_scale, const float x, const float y)
{
  const float dx = DT_DRAW_SIZE_CROSS / zoom_scale;
  const float dy = DT_DRAW_SIZE_CROSS / zoom_scale;
  cairo_save(cr);

  cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE);
  dt_draw_set_dash_style(cr, DT_MASKS_NO_DASH, zoom_scale);
  cairo_set_line_width(cr, DT_DRAW_SIZE_LINE_HIGHLIGHT / zoom_scale);
  dt_draw_set_color_overlay(cr, FALSE, 0.8);

  cairo_move_to(cr, x + dx, y);
  cairo_line_to(cr, x - dx, y);
  cairo_move_to(cr, x, y + dy);
  cairo_line_to(cr, x, y - dy);
  cairo_stroke_preserve(cr);

  cairo_set_line_width(cr, DT_DRAW_SIZE_LINE / zoom_scale);
  dt_draw_set_color_overlay(cr, TRUE, 0.8);
  cairo_stroke(cr);

  cairo_restore(cr);
}

static inline void dt_draw_source_shape(cairo_t *cr, const float zoom_scale, const gboolean selected, 
  const float *source_pts, const int source_pts_count, const int nodes_nb, const shape_draw_function_t *draw_shape_func)
{
  cairo_save(cr);

  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  dt_draw_set_dash_style(cr, DT_MASKS_NO_DASH, zoom_scale);
  
  if(draw_shape_func)
    (*draw_shape_func)(cr, source_pts, source_pts_count, nodes_nb, FALSE, TRUE);

  //dark line
  if(selected)
    cairo_set_line_width(cr, DT_DRAW_SIZE_LINE_HIGHLIGHT_SELECTED / zoom_scale);
  else
    cairo_set_line_width(cr, DT_DRAW_SIZE_LINE_HIGHLIGHT / zoom_scale);
  dt_draw_set_color_overlay(cr, FALSE, 0.6);
  cairo_stroke_preserve(cr);

  //bright line
  if(selected)
    cairo_set_line_width(cr, DT_DRAW_SIZE_LINE_SELECTED / zoom_scale);
  else
    cairo_set_line_width(cr, (1.5f * DT_DRAW_SIZE_LINE) / zoom_scale);
  dt_draw_set_color_overlay(cr, TRUE, 0.8);
  cairo_stroke(cr);
  
  cairo_restore(cr);
}

#ifdef __cplusplus
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
