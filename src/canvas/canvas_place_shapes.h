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

#ifndef DT_CANVAS_CANVAS_PLACE_SHAPES_H
#define DT_CANVAS_CANVAS_PLACE_SHAPES_H

/**
 * @file canvas_place_shapes.h
 * @brief What an object puts on screen that its floating properties must keep clear of.
 *
 * @details canvas_place.h searches rectangles and knows nothing of a document; this is where an
 * object becomes those rectangles. Its handle sites (canvas_handles.h) are projected on screen and
 * grown by how far each one catches the pointer, its body is cut into slabs, and what one click in
 * its properties would add is asked of a copy of it. Kept out of the view so that the geometry the
 * placement relies on is tested against the hit tests it stands for: a box one pixel too narrow is
 * a handle the properties may be laid over.
 *
 * GTK-free: plain numbers for the view, a GArray of dt_canvas_place_shape_t out.
 */

#include "canvas/canvas.h"
#include "canvas/canvas_handles.h"
#include "canvas/canvas_place.h"

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

/** How the canvas is shown: what the view is centred on, at what zoom, and how large it is. */
typedef struct dt_canvas_place_view_t
{
  double center_x; ///< the canvas point at the middle of the view
  double center_y;
  double zoom;     ///< screen pixels per canvas unit, strictly positive
  double width;    ///< the view's size in logical pixels
  double height;
  double margin;   ///< how far outside the view a shape still counts: the air the placement keeps, and more
} dt_canvas_place_view_t;

/** @brief A canvas point on screen, in the view's logical pixels. */
void dt_canvas_place_to_screen(const dt_canvas_place_view_t *view, double canvas_x, double canvas_y,
                               double *screen_x, double *screen_y);

/** @brief Append one rectangle given by two corners in any order, snapped outward to whole pixels. */
void dt_canvas_place_shape_add(GArray *shapes, dt_canvas_place_class_t shape_class, double x0, double y0, double x1,
                               double y1, double weight, gboolean target);

/**
 * @brief Append handle sites as rectangles on screen.
 * @details A site's reach is its pixel part plus its document part at this zoom, rounded up. A
 * square catches along its own turned axes, so its box is wider by the turn; a line is cut into
 * pieces short enough that the box of each is no wider than the band itself, and only the part the
 * view shows, give or take the margin, is cut. Every rectangle belongs to the object edited.
 */
void dt_canvas_place_sites_add(GArray *shapes, const dt_canvas_place_view_t *view,
                               const dt_canvas_handle_site_t *sites, size_t count,
                               dt_canvas_place_class_t shape_class);

/**
 * @brief Append a frame's body: its turned quadrilateral as horizontal slabs, each the width the
 * frame spans at those heights and grown by how far outside a frame a press still picks it.
 */
void dt_canvas_place_body_add(GArray *shapes, const dt_canvas_place_view_t *view, const dt_canvas_object_t *frame);

/**
 * @brief Append everything an object puts on screen that its properties must keep clear of.
 * @details A frame: its corners and knob, a cutout's handles -- HARD while `mask_editing`, PREDICTED
 * otherwise, since one click on Edit shows them -- and its body. A connector: its tangents, waypoint,
 * line and arrowheads, and as PREDICTED the tangents and waypoint one click on the route or the
 * waypoint would add, asked of copies of it that never reach the document.
 * @param canvas the document, for a connector's route
 */
void dt_canvas_place_object_shapes(GArray *shapes, const dt_canvas_place_view_t *view, const dt_canvas_t *canvas,
                                   const dt_canvas_object_t *object, gboolean mask_editing);

#ifdef __cplusplus
}
#endif

#endif // DT_CANVAS_CANVAS_PLACE_SHAPES_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
