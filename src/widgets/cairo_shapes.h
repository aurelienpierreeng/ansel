/*
 *    This file is part of darktable,
 *    Copyright (C) 2016 johannes hanika.
 *    Copyright (C) 2016, 2020 Tobias Ellinghaus.
 *    Copyright (C) 2020 Pascal Obry.
 *    Copyright (C) 2021 Sakari Kapanen.
 *    Copyright (C) 2022 Martin Bařinka.
 *    
 *    darktable is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *    
 *    darktable is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *    
 *    You should have received a copy of the GNU General Public License
 *    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */


#ifndef DT_WIDGETS_CAIRO_SHAPES_H
#define DT_WIDGETS_CAIRO_SHAPES_H

#include <cairo.h>
#include <gdk/gdk.h>
#include <math.h>

G_BEGIN_DECLS

/* Cairo path primitives with no knowledge of anything. Split out of gui/draw.h, which also
 * holds curve/histogram drawing that reaches into the application; widgets/ needs only the
 * shapes. gui/draw.h includes this, so its existing consumers are unaffected. */

/** Draw a rating star: `r1` outer radius, `r2` inner. */
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

/** A straight segment from (left, top) to (right, bottom). */
static inline void dt_draw_line(cairo_t *cr, float left, float top, float right, float bottom)
{
  cairo_move_to(cr, left, top);
  cairo_line_to(cr, right, bottom);
}

/** Set the cairo source to a GdkRGBA, alpha included. */
static inline void set_color(cairo_t *cr, GdkRGBA color)
{
  cairo_set_source_rgba(cr, color.red, color.green, color.blue, color.alpha);
}

G_END_DECLS

#endif // DT_WIDGETS_CAIRO_SHAPES_H
