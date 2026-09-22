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
  double quality;                    ///< 1 composites every pixel; 0.5 composites at half the resolution and scales up, for a frame mid-gesture
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
  int64_t pixels;            ///< composited, over every band; 0 when the last frame was served from the cache
  gboolean cached;           ///< the frame was the previous one, blitted
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

/**
 * @brief How far from an arrowed end of a connector the painter lays ink, in canvas units.
 *
 * The head is sized to the line, so this grows with the width. A stored width of zero is
 * painted two units wide and is answered for as such. The painter grows an object's box by it,
 * and anything that must not cover an arrowhead asks the same question rather than guessing.
 * Below a width of one unit it runs short of the head's own back corners, which stay about
 * 14.9 units from the tip however thin the line: measured, 14.98 units of ink against 14.25 at a
 * quarter of a unit. From one unit up it covers the head.
 */
double dt_canvas_paint_arrow_reach(double line_width);

/**
 * @brief Everything this object paints, in canvas units.
 *
 * Its frame, turned, or its route with the arrowheads and every control point; grown by the reach
 * of its shadow and, for a text, of its letters' own shadow; and by the padding drawn around a
 * frame while the paddings are shown. The canvas-unit twin of the box the painter sizes each
 * object's layer to, so what it answers is what a repaint of that object can touch.
 *
 * @return FALSE when the object paints nothing that can be measured -- an unrouted connector.
 */
gboolean dt_canvas_paint_object_extent(const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                                       dt_canvas_rect_t *out);

/**
 * @brief The part of the plane that can change when these objects move, in canvas units.
 *
 * What they paint, what every connector attached to one of them paints -- it follows its frame --
 * and the whole of every text frame flowing around frames whose own paint meets a moved one's,
 * since its lines can re-wrap anywhere inside it. Asked once before a move and once after, the
 * union of the two is all a repaint has to cover: everything outside it is pixel for pixel what it
 * was. `test` in `bench_canvas_paint` holds that against a full repaint on real canvases.
 *
 * @param ids the objects being moved; objects not in the canvas are skipped.
 * @return FALSE when nothing measurable moves, in which case nothing need be repainted either.
 */
gboolean dt_canvas_paint_move_damage(const dt_canvas_t *canvas, const uint32_t *ids, size_t count,
                                     dt_canvas_rect_t *out);

/** @brief Paint one object. */
void dt_canvas_paint_object(cairo_t *cr, const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                            const dt_canvas_paint_options_t *options);

/**
 * @brief Lay a text frame out, for the atelier's fit-to-content action.
 * @return the height in canvas units the frame needs to show all of its text at its width.
 */
double dt_canvas_paint_text_natural_height(cairo_t *cr, const dt_canvas_t *canvas, const dt_canvas_object_t *object);

/**
 * @brief Resize a text frame to the text it holds, ONCE, at edit time.
 *
 * Never per frame: a text frame that flows around what is laid over it has a height that
 * depends on the obstacles above it, and those depend on where its top edge is, so measuring
 * on every repaint let the two chase each other. The frame grows DOWNWARD -- its top edge is
 * where the user put it -- and the height is iterated to a fixed point here rather than one
 * step per paint. The caller decides whether the frame wants this; the auto-height flag is
 * not read here, so the atelier's explicit "fit height" action shares the entry.
 *
 * @return TRUE when the object's geometry was changed, so the caller may touch the document.
 */
gboolean dt_canvas_paint_text_fit_height(const dt_canvas_t *canvas, dt_canvas_object_t *object);

#define DT_CANVAS_FONT_FEATURE_TAG_LEN 5 ///< four characters and a terminator
#define DT_CANVAS_TEXT_FEATURE_LIST_MAX 128 ///< a rich face ships tens of them; FreeSerif, 45

/**
 * @brief The OpenType features the object's own font actually ships.
 *
 * Asked of the face through HarfBuzz rather than assumed from a table: a font carries whatever
 * tags its designer cut, so Linux Libertine's historical ligatures are there to be offered and
 * a face without small capitals must not be. The tags come back sorted and without repeats,
 * from the substitution and the positioning tables both.
 *
 * @param tags filled with NUL-terminated four-character tags
 * @param max how many the caller has room for
 * @return how many were written, 0 when the face cannot be asked
 */
uint32_t dt_canvas_paint_text_font_features(const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                                            char tags[][DT_CANVAS_FONT_FEATURE_TAG_LEN], const uint32_t max);

#ifdef __cplusplus
}
#endif

#endif // DT_CANVAS_CANVAS_PAINT_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
