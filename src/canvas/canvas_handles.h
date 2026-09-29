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

#ifndef DT_CANVAS_CANVAS_HANDLES_H
#define DT_CANVAS_CANVAS_HANDLES_H

/**
 * @file canvas_handles.h
 * @brief Where an object can be taken hold of, as one list of sites.
 *
 * @details A frame's corners and its rotation knob, a line's free ends, a cubic connector's
 * tangents, its waypoint, the band its line is picked in, a cutout's points and a polygon's nodes:
 * every place the pointer grabs something, and the lines drawn between them that a floating
 * control must not cover, are described here ONCE, as a site with a shape and a reach. The
 * atelier's hit tests are loops over these sites, so they cannot disagree about where a handle is
 * or how far it catches.
 * The painters do not read the list: the frame handles, a line's free ends, the tangents and the
 * waypoint are drawn from the same constants and routes, and the cutout's handles from the geometry
 * below, so a painted handle and its site agree through those and not through one list.
 *
 * A reach has two parts because a handle has two: the part that stays the same size on screen
 * whatever the zoom (a handle is eight pixels at any magnification) and the part that belongs
 * to the document and zooms with it (a line's width, an arrowhead). Sites are in canvas units
 * and the zoom is supplied when one is asked about.
 *
 * The cutout's geometry lives here too, for the same reason: the handles are where that
 * geometry is, and the view paints and drags them from the same functions. Everything is read
 * from canvas types; nothing here names the darkroom's masks.
 *
 * GTK-free: nothing here paints, raises a signal or touches the document.
 */

#include "canvas/canvas.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A frame corner's and a cutout handle's size on screen, and how far either catches the pointer. */
#define DT_CANVAS_HANDLE_PIXELS 8.0
/** How far above a frame's top edge its rotation knob floats, on screen. */
#define DT_CANVAS_ROTATE_HANDLE_OFFSET_PIXELS 28.0
/** How far outside an object a press still picks it, on screen. */
#define DT_CANVAS_PICK_TOLERANCE_PIXELS 4.0
/** A connector's waypoint, tangent handles and free-end marks as drawn, on screen; they catch a little wider. */
#define DT_CANVAS_VIA_HANDLE_PIXELS 7.0

/** What a site is. The view maps each to the gesture it starts; see the fields each one uses. */
typedef enum dt_canvas_handle_role_t
{
  DT_CANVAS_HANDLE_CORNER = 0,    ///< a frame's corner; `index` 0..3, from the top-left clockwise
  DT_CANVAS_HANDLE_ROTATE,        ///< the knob above the frame's top edge
  DT_CANVAS_HANDLE_ROTATE_STEM,   ///< the line from the top edge up to the knob; grabs nothing
  DT_CANVAS_HANDLE_TANGENT,       ///< a cubic connector's control point; `index` 1..4, `part` its end
  DT_CANVAS_HANDLE_TETHER,        ///< the line from that end to its control point; grabs nothing
  DT_CANVAS_HANDLE_VIA,           ///< a connector's waypoint
  DT_CANVAS_HANDLE_CURVE,         ///< one leg of the route's polyline, `index` its first point
  DT_CANVAS_HANDLE_ARROW,         ///< an arrowhead; `part` FROM or TO; grabs nothing of its own
  DT_CANVAS_HANDLE_MASK_POINT,    ///< a cutout handle; `index` as dt_canvas_mask_handle_points() orders them
  DT_CANVAS_HANDLE_MASK_NODE,     ///< a polygon node; `index` the node
  DT_CANVAS_HANDLE_MASK_NODE_OWN, ///< a node's own fall-off or control point, latent; `index` the node
  DT_CANVAS_HANDLE_MASK_EDGE,     ///< the straight edge from node `index` to the next
  DT_CANVAS_HANDLE_ENDPOINT,      ///< a connector's free end; `part` FROM or TO, `index` 0 or 1
  DT_CANVAS_HANDLE_ROLE_COUNT,    ///< how many roles there are; never a site's, never stored
} dt_canvas_handle_role_t;

/** How a site catches the pointer. */
typedef enum dt_canvas_handle_shape_t
{
  DT_CANVAS_HANDLE_SQUARE = 0,  ///< within reach of the centre along both of its own axes, turned by `angle`
  DT_CANVAS_HANDLE_DISC,        ///< within reach of the centre
  DT_CANVAS_HANDLE_SEGMENT,     ///< within reach of the nearest point of the segment
} dt_canvas_handle_shape_t;

/** Which end, or which of a node's own handles, a site belongs to. */
typedef enum dt_canvas_handle_part_t
{
  DT_CANVAS_HANDLE_PART_NONE = 0,
  DT_CANVAS_HANDLE_PART_FROM,     ///< a connector's start
  DT_CANVAS_HANDLE_PART_VIA,      ///< its waypoint
  DT_CANVAS_HANDLE_PART_TO,       ///< its end
  DT_CANVAS_HANDLE_PART_BORDER,   ///< a polygon node's own fall-off
  DT_CANVAS_HANDLE_PART_INCOMING, ///< its control point on the previous node's side
  DT_CANVAS_HANDLE_PART_OUTGOING, ///< and on the next node's side
} dt_canvas_handle_part_t;

/** Which sites to list: any combination. */
typedef enum dt_canvas_handle_set_t
{
  DT_CANVAS_HANDLES_FRAME = 1 << 0,          ///< corners, knob and stem; none on a locked frame
  DT_CANVAS_HANDLES_MASK_POINTS = 1 << 1,    ///< a cutout's handles
  DT_CANVAS_HANDLES_MASK_NODES = 1 << 2,     ///< a polygon's nodes
  DT_CANVAS_HANDLES_MASK_NODE_OWN = 1 << 3,  ///< every node's own three handles
  DT_CANVAS_HANDLES_MASK_EDGES = 1 << 4,     ///< a polygon's straight node-to-node edges
  DT_CANVAS_HANDLES_TANGENTS = 1 << 5,       ///< a cubic connector's control points and their tethers
  DT_CANVAS_HANDLES_VIA = 1 << 6,            ///< a connector's waypoint
  DT_CANVAS_HANDLES_CURVE = 1 << 7,          ///< the band a connector is picked in, and its arrowheads
  DT_CANVAS_HANDLES_ENDPOINTS = 1 << 8,      ///< a connector's free ends; none on an anchored end or a locked line
  DT_CANVAS_HANDLES_MASK = DT_CANVAS_HANDLES_MASK_POINTS | DT_CANVAS_HANDLES_MASK_NODES
                           | DT_CANVAS_HANDLES_MASK_NODE_OWN | DT_CANVAS_HANDLES_MASK_EDGES,
  DT_CANVAS_HANDLES_CONNECTOR = DT_CANVAS_HANDLES_ENDPOINTS | DT_CANVAS_HANDLES_TANGENTS | DT_CANVAS_HANDLES_VIA,
  DT_CANVAS_HANDLES_ALL = DT_CANVAS_HANDLES_FRAME | DT_CANVAS_HANDLES_MASK | DT_CANVAS_HANDLES_CONNECTOR
                          | DT_CANVAS_HANDLES_CURVE,
} dt_canvas_handle_set_t;

/**
 * One place an object is taken hold of, or drawn over. Canvas units, except for what is
 * explicitly screen pixels: those are divided by the zoom when the site is asked about.
 */
typedef struct dt_canvas_handle_site_t
{
  uint32_t role;       ///< dt_canvas_handle_role_t
  uint32_t shape;      ///< dt_canvas_handle_shape_t
  uint32_t part;       ///< dt_canvas_handle_part_t
  int index;           ///< which corner, control point, leg, node or cutout handle; see the role
  int sign;            ///< a waypoint tangent's side: -1 before the waypoint, +1 after it; 0 otherwise
  gboolean latent;     ///< drawn and caught only for the node the pointer is working near
  double x0;           ///< the centre, or where a segment starts
  double y0;
  double x1;           ///< where a segment ends; the centre again for a square or a disc
  double y1;
  double angle;        ///< radians: the axes of a square, and of the pixel offset
  double offset_px_x;  ///< screen pixels along the site's own axes, added to the centre -- or to a
  double offset_px_y;  ///< segment's end: the knob floats the same distance above a frame at any zoom
  double reach_px;     ///< how far from its shape it catches, on screen: the part that does not zoom
  double reach_units;  ///< and in canvas units: the part that does, a line's width or an arrowhead
} dt_canvas_handle_site_t;

/**
 * @brief List an object's sites.
 *
 * The order is fixed, and the hit tests that walk the list forwards grab the first site that
 * catches, so it is their priority:
 * - a frame: its four corners from the top-left clockwise, then the knob, then the knob's stem;
 * - a cutout: its points by index, then a polygon's nodes by index, then every node's own
 *   handles grouped by node, each node's fall-off before its incoming and its outgoing control
 *   point, then the edges by index;
 * - a connector: its free ends, the start's before the end's, then its control points from the
 *   start to the end, each followed by its tether, then its waypoint, then the legs of its route
 *   from start to end, then the start's arrowhead before the end's. A free end comes before the
 *   control point it leaves by: the point a line is drawn between is what a press near both means,
 *   and a control point pulled back under its end is still reached by zooming in, the end's reach
 *   being a screen distance and the control point's offset a length of the document.
 * A caller wanting another priority -- the cutout's outer points before its centre -- walks the
 * list backwards or filters it by role.
 *
 * @param canvas the document, for a connector's route; may be NULL when `object` is a frame
 * @param what   dt_canvas_handle_set_t bits
 * @param out    where to write the sites; may be NULL when `max` is 0
 * @param max    how many `out` holds
 * @return how many sites the object has, which may be more than `max`: only the first `max` are
 *         written, so a caller can ask once with nothing to learn how much to allocate.
 */
size_t dt_canvas_handle_sites(const dt_canvas_t *canvas, const dt_canvas_object_t *object, uint32_t what,
                              dt_canvas_handle_site_t *out, size_t max);

/**
 * @brief A site's shape at a zoom, in canvas units.
 * @param zoom screen pixels per canvas unit, strictly positive
 * @param reach how far the site catches, in canvas units
 */
void dt_canvas_handle_site_resolve(const dt_canvas_handle_site_t *site, double zoom, double *x0, double *y0,
                                   double *x1, double *y1, double *reach);

/** @brief Does a site catch a canvas point, at a zoom? */
gboolean dt_canvas_handle_site_hit(const dt_canvas_handle_site_t *site, double x, double y, double zoom);

/* --- the cutout's geometry ------------------------------------------------------ */

/** @brief The frame's shorter side, the unit of a cutout's radii and feather. */
double dt_canvas_mask_side(const dt_canvas_object_t *object);

/** @brief A cutout's unit-square point in the frame's local units, origin at its centre. */
void dt_canvas_mask_to_local(const dt_canvas_object_t *object, double u, double v, double *local_x,
                             double *local_y);

/**
 * @brief A polygon node's own fall-off radius, in the shape's units: its own where it has one,
 * the shape's where it does not.
 */
double dt_canvas_mask_node_border(const dt_canvas_mask_t *mask, uint32_t index);

/**
 * @brief The polygon's control points for the segment leaving node `index`, in the shape's
 * units, as the masks module computes them: a Catmull-Rom tangent through an automatic node,
 * the stored points otherwise.
 */
void dt_canvas_mask_polygon_controls(const dt_canvas_mask_t *mask, uint32_t index, float control1[2],
                                     float control2[2]);

/**
 * @brief A node's two control points, in the shape's units: the stored ones, or the tangent the
 * curve actually takes through a smooth node, so a handle always sits on the curve it steers.
 */
void dt_canvas_mask_node_controls(const dt_canvas_mask_t *mask, uint32_t index, float incoming[2],
                                  float outgoing[2]);

/** @brief Where a node's three own handles sit, in the frame's local units. */
void dt_canvas_mask_node_handles(const dt_canvas_object_t *object, uint32_t index, double border[2],
                                 double incoming[2], double outgoing[2]);

/**
 * @brief The cutout's handles in local units: [0] the centre or anchor, [1] the radius (circle),
 * the first radius (ellipse) or the reach (gradient), [2] the ellipse's second radius, [3] the
 * feather of a circle or an ellipse, on its dashed ring.
 * @return how many there are; a polygon has only its centre, its nodes being its own handles.
 */
int dt_canvas_mask_handle_points(const dt_canvas_object_t *object, double points[8]);

#ifdef __cplusplus
}
#endif

#endif // DT_CANVAS_CANVAS_HANDLES_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
