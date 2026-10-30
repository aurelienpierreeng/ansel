/*
    This file is part of Ansel,
    Copyright (C) 2026 Aurélien PIERRE.

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Ansel is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Ansel.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef DT_WIDGETS_STROKE_RASTER_H
#define DT_WIDGETS_STROKE_RASTER_H

/* A stroke rasteriser for GUI overlays.
 *
 * An overlay line -- a mask's outline, its feather border -- arrives as a polyline already
 * sampled at the resolution it is shown at: one vertex per device pixel or so. Handing that to
 * cairo means turning pixels back into a vector path and asking a general-purpose stroker to
 * tessellate and rasterise it, twice per line (a wide dark pass under a narrow bright one). What
 * the overlays actually use of a stroker is small and fixed: two widths, two colours with alpha,
 * two dash patterns, round or butt caps, and a one-pixel antialiasing ramp. This is that, and
 * nothing more, painting straight into the ARGB32 pixels of the surface the overlay is drawn on.
 *
 * It knows nothing about masks, nodes or handles. Everything drawn with a cairo primitive that
 * is NOT a long polyline -- discs, arcs, arrows, the CLEAR-operator punch under a node -- stays
 * with cairo, on the same surface, before or after these strokes as the caller likes.
 *
 * The coverage model is a distance field: for every pixel within reach of a polyline, the
 * distance to the nearest point of it (a union of capsules, one per segment, so joins and caps
 * come out round with no seams). Coverage of a pass of half-width R is clamp(R + 1/2 - d, 0, 1),
 * the same one-pixel ramp CAIRO_ANTIALIAS_FAST draws. Dashes are cut along the polyline's arc
 * length before the capsules are stamped, so a dash end is a round cap exactly as cairo's
 * CAIRO_LINE_CAP_ROUND makes it. Compositing is premultiplied OVER, dark pass then bright pass,
 * which is what the two cairo_stroke() calls it replaces composed. */

#include <cairo.h>
#include <glib.h>

G_BEGIN_DECLS

/** One pass of a stroke: a width and a straight (not premultiplied) colour with alpha. */
typedef struct dt_stroke_pass_t
{
  double width;
  double red;
  double green;
  double blue;
  double alpha;
} dt_stroke_pass_t;

/** How a polyline is stroked. Widths and dash lengths are in whatever space the entry point
 * says: device pixels for dt_stroke_raster_polyline(), user units for dt_stroke_raster_path(). */
typedef struct dt_stroke_style_t
{
  dt_stroke_pass_t dark;     /**< painted first, the wider of the two */
  dt_stroke_pass_t bright;   /**< painted over it; width <= 0 means no second pass */
  double dash_on;            /**< length of a dash; <= 0 means solid */
  double dash_off;           /**< length of the gap after a dash */
  gboolean round_caps;       /**< round ends on the polyline and on every dash; else flat ends */
} dt_stroke_style_t;

/** Is @p surface something this rasteriser can paint into: an ARGB32 image surface. */
gboolean dt_stroke_raster_can_paint(cairo_surface_t *surface);

/** Stroke @p count device-space vertices (x, y pairs, in the pixel grid of @p surface) as one
 * open polyline into @p surface. The style's widths and dashes are device pixels. Returns FALSE,
 * having painted nothing, when the surface is not one this can paint into (see
 * dt_stroke_raster_can_paint()) or when there is nothing to draw. */
gboolean dt_stroke_raster_polyline(cairo_surface_t *surface, const double *xy, int count,
                                   const dt_stroke_style_t *style);

/** Stroke the current path of @p cr with @p style, then consume the path, as a pair of
 * cairo_stroke() calls would. The path is flattened and mapped through cr's matrix into the
 * pixels of the surface cr is drawing on -- the current group's, if one is pushed -- and the
 * style's widths and dashes, given in USER units as the cairo calls receive them, are mapped
 * through the same matrix. Returns FALSE, leaving the path in place, when that surface is not
 * one this can paint into; the caller then strokes it with cairo instead. */
gboolean dt_stroke_raster_path(cairo_t *cr, const dt_stroke_style_t *style);

/** The pixel rectangle everything painted into @p surface through this file has touched since
 * the last reset, so a caller can composite or clear that much and no more. Returns FALSE when
 * nothing was painted. */
gboolean dt_stroke_raster_touched(cairo_surface_t *surface, cairo_rectangle_int_t *touched);

/** Forget the touched rectangle of @p surface. */
void dt_stroke_raster_touched_reset(cairo_surface_t *surface);

G_END_DECLS

#endif // DT_WIDGETS_STROKE_RASTER_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
