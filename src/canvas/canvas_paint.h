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

#ifndef DT_CANVAS_CANVAS_PAINT_H
#define DT_CANVAS_CANVAS_PAINT_H

/**
 * @file canvas_paint.h
 * @brief Draw a canvas into a cairo context, in canvas units.
 *
 * @details One painter serves the atelier's centre view and the PDF export, so what is
 * printed is what was shown, apart from the colour management target: the atelier asks
 * for display colours, the export keeps sRGB and converts the whole page afterwards.
 *
 * The caller sets the transform: user space must already be canvas units, with the
 * origin and scale of its choosing. The painter draws the background, the grid dots, then
 * every object in draw order. Selection handles are the atelier's business and are not
 * drawn here.
 */

#include "canvas/canvas.h"
#include "canvas/canvas_render.h"

#include <cairo.h>
#include <glib.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dt_canvas_paint_options_t
{
  gboolean for_display;              ///< colour-manage for the display, else keep sRGB
  dt_canvas_surface_cache_t *cache;  ///< decoded frames; NULL decodes on the spot, every time
  gboolean draw_background;          ///< fill `clip` with the canvas background
  gboolean draw_grid;                ///< dots at the grid crossings, when the canvas shows its grid
  gboolean draw_placeholders;        ///< a frame with no render yet gets a placeholder box
  double units_per_pixel;            ///< canvas units per device pixel, for hairlines and dots
  dt_canvas_rect_t clip;             ///< the canvas area being painted; width 0 means everything
} dt_canvas_paint_options_t;

/** @brief Options suited to the atelier: display colours, grid, placeholders. */
dt_canvas_paint_options_t dt_canvas_paint_options_display(dt_canvas_surface_cache_t *cache, double units_per_pixel,
                                                          dt_canvas_rect_t clip);

/** @brief Options suited to an export: sRGB, no grid, no placeholders. */
dt_canvas_paint_options_t dt_canvas_paint_options_export(dt_canvas_surface_cache_t *cache, double units_per_pixel,
                                                         dt_canvas_rect_t clip);

/** @brief Paint the whole canvas. */
void dt_canvas_paint(cairo_t *cr, const dt_canvas_t *canvas, const dt_canvas_paint_options_t *options);

/** What the last dt_canvas_paint() cost, phase by phase: printed under `-d perf`, and there for tuning. */
typedef struct dt_canvas_paint_stats_t
{
  int64_t pixels;            ///< composited, over every band
  int objects;               ///< drawn
  int layers;                ///< cairo layers painted (a cut frame is three)
  int shadows;
  double background_seconds; ///< the base layer, painted and decoded
  double objects_seconds;    ///< every object, from its cairo layer to its composite
  double paint_seconds;      ///< of which cairo painting and decoding the main layer
  double shadow_seconds;     ///< of which the shadows' blur and composite
  double encode_seconds;     ///< the finished canvas to 8 bits and to the display
  double total_seconds;
} dt_canvas_paint_stats_t;

dt_canvas_paint_stats_t dt_canvas_paint_last_stats(void);

/** @brief Paint one object. */
void dt_canvas_paint_object(cairo_t *cr, const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                            const dt_canvas_paint_options_t *options);

/**
 * @brief Lay a text frame out, for the atelier's fit-to-content action.
 * @return the height in canvas units the frame needs to show all of its text at its width.
 */
double dt_canvas_paint_text_natural_height(cairo_t *cr, const dt_canvas_t *canvas, const dt_canvas_object_t *object);

#ifdef __cplusplus
}
#endif

#endif // DT_CANVAS_CANVAS_PAINT_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
