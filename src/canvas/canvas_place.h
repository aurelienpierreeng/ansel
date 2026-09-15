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

#ifndef DT_CANVAS_CANVAS_PLACE_H
#define DT_CANVAS_CANVAS_PLACE_H

/**
 * @file canvas_place.h
 * @brief Where an object's floating properties go on screen.
 *
 * @details The properties float next to the object they edit, and everything around that object
 * is something the user is about to grab: its corners and knob, a connector's tangents and
 * waypoint, the band its line is picked in, a cutout's nodes, the navigation flower. A floating
 * control laid over one of those takes the press the handle was owed, so the question "where do
 * they go" is answered here once, as a search over rectangles, and the answer is a rectangle that
 * covers none of them.
 *
 * What is on screen reaches the solver as SHAPES, axis-aligned rectangles in logical pixels that
 * the caller has already grown by how far each one catches the pointer, each with a class:
 * - HARD is never covered, whatever else fails: a handle, a line, the flower, the pointer;
 * - PREDICTED is what one click in the properties would create -- a straight connector's cubic
 *   tangents, a cutout's handles before it is edited -- and is covered only when nothing else fits;
 * - BODY is the object itself, and is covered only after every spot clear of it has failed;
 * - SOFT is anything better left uncovered -- other frames, the status line -- and only weighs in
 *   the cost.
 * Every shape except a SOFT one is kept `air` pixels clear of on every side.
 *
 * The widget is a STRIP, one row, that can grow a CARD below or above it. A placement is the
 * strip's rectangle plus the side and the height the card takes, and three reasons ask for one:
 * - OPEN searches the whole view, level by level, from the strictest (clear of HARD, PREDICTED
 *   and BODY) to the loosest (clear of HARD alone);
 * - RESOLVE keeps the previous placement when the object moved by two pixels or less, and
 *   translates it rigidly with the object otherwise, as long as the result is still clear at the
 *   level it was found at -- so a pan, a zoom or a refill carries the properties along instead of
 *   re-deciding where they go. A placement that had to cover the object is kept only while no
 *   spot clear of it exists and it covers no more of it than before, since the room a pan or a zoom
 *   opens is exactly what it was missing. A search that follows still offers the spot the
 *   properties are in, so staying costs nothing. A placement that was not visible is no previous
 *   placement at all;
 * - GROW keeps the strip where it is and fits the card on whichever side has the room, scrolling it
 *   when neither side holds all of it: opening the card never moves the strip under the pointer
 *   unless there is no room for even the card's smallest height.
 *
 * A candidate top is only ever moved WITHIN a stretch of the view proven free, never clamped onto
 * the object, so a placement that covers something it must not cannot be produced by rounding
 * one into the view. The search is deterministic: ties go to the smaller top, then the smaller
 * left, then the card growing down.
 *
 * GTK-free and document-free: plain rectangles in, a rectangle out.
 */

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

/** What a shape is to the properties: see the file's description. */
typedef enum dt_canvas_place_class_t
{
  DT_CANVAS_PLACE_HARD = 0,
  DT_CANVAS_PLACE_PREDICTED = 1,
  DT_CANVAS_PLACE_BODY = 2,
  DT_CANVAS_PLACE_SOFT = 3,
} dt_canvas_place_class_t;

/** What a SOFT shape weighs: another frame, and the status line or the toast. */
#define DT_CANVAS_PLACE_WEIGHT_FRAME 1.0
#define DT_CANVAS_PLACE_WEIGHT_BAND 2.0

/** What a placement had to give up, from nothing to everything but the HARD shapes. */
typedef enum dt_canvas_place_level_t
{
  DT_CANVAS_PLACE_LEVEL_HPB = 0, ///< clear of HARD, PREDICTED and BODY
  DT_CANVAS_PLACE_LEVEL_HB = 1,  ///< clear of HARD and BODY: may cover what a click would create
  DT_CANVAS_PLACE_LEVEL_H = 2,   ///< clear of HARD only: may cover the object itself
} dt_canvas_place_level_t;

/** Which side of the strip the card grows on. */
typedef enum dt_canvas_place_growth_t
{
  DT_CANVAS_PLACE_DOWN = 0,
  DT_CANVAS_PLACE_UP = 1,
} dt_canvas_place_growth_t;

/** Why a placement is asked for. */
typedef enum dt_canvas_place_reason_t
{
  DT_CANVAS_PLACE_OPEN = 0,    ///< the properties are shown for the first time: search everything
  DT_CANVAS_PLACE_RESOLVE = 1, ///< the view, the object or the widget changed: keep, else search
  DT_CANVAS_PLACE_GROW = 2,    ///< the card was opened or grew taller: keep the strip, else search
} dt_canvas_place_reason_t;

/** A rectangle in logical pixels. */
typedef struct dt_canvas_place_rect_t
{
  double x;
  double y;
  double width;
  double height;
} dt_canvas_place_rect_t;

typedef struct dt_canvas_place_shape_t
{
  dt_canvas_place_rect_t rect;         ///< already grown by how far it catches the pointer, NOT by the air
  dt_canvas_place_class_t shape_class;
  double weight;                       ///< a SOFT shape's weight; ignored for the others
  gboolean target;                     ///< belongs to the object edited: its HARD and BODY shapes bound it
} dt_canvas_place_shape_t;

/** A placement: the strip, and the card attached to it. */
typedef struct dt_canvas_place_t
{
  gboolean visible;                 ///< FALSE when nothing fits: the properties stay hidden
  dt_canvas_place_rect_t strip;
  dt_canvas_place_growth_t growth;
  double card_height;               ///< the height the card is given, which may be less than it wants: it scrolls
  gboolean card_shown;
  gboolean clipped;                 ///< the card is open but there was no room for it here
  dt_canvas_place_level_t level;    ///< the level the placement is clear at
  double body;                      ///< the fraction of the object's on-screen body it covers
  double anchor_x;                  ///< the anchor it is attached to: what a later RESOLVE measures motion from
  double anchor_y;
} dt_canvas_place_t;

typedef struct dt_canvas_place_input_t
{
  dt_canvas_place_rect_t view;      ///< where the properties may go: the view, already inset from its edges
  double air;                       ///< the gap kept around every HARD, PREDICTED and BODY shape
  const dt_canvas_place_shape_t *shapes;
  size_t shape_count;
  double anchor_x;                  ///< the place on the object they were asked for, on screen
  double anchor_y;
  gboolean has_press;               ///< the last press point is known: a placement over the body avoids it
  double press_x;
  double press_y;
  double width;                     ///< the widget's width, strip and card alike
  double strip_height;
  gboolean card_open;
  double card_content_height;       ///< the card's natural height
  double card_max;                  ///< the most a card is given before it scrolls, before 60 % of the view caps it
  double card_min;                  ///< the least a card is shown at; below this it is clipped instead
  double last_card_height;          ///< a hint: the card this kind last opened, so the strip leaves room for it
  dt_canvas_place_reason_t reason;
  const dt_canvas_place_t *previous; ///< the placement shown so far, or NULL; ignored when OPENING
} dt_canvas_place_input_t;

/**
 * @brief Place the properties.
 * @details Inputs are snapped to whole pixels first -- the view inward, every shape outward, the
 * air and the widget's size up -- so that every rectangle the search compares has whole-pixel
 * edges and the answer does not depend on where a fraction happened to fall; the anchor and the
 * press are snapped to a 256th of a pixel. A view is a screen: past 65536 pixels across or down it
 * is cut off, and one that is not a number, or starts further than 1e9 pixels from the origin,
 * places nothing -- as does an anchor or a widget size that is not a number.
 */
void dt_canvas_place_solve(const dt_canvas_place_input_t *input, dt_canvas_place_t *result);

/** @brief The rectangle a placement covers: the strip and the card it shows, if any. */
dt_canvas_place_rect_t dt_canvas_place_footprint(const dt_canvas_place_t *place);

#ifdef __cplusplus
}
#endif

#endif // DT_CANVAS_CANVAS_PLACE_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
