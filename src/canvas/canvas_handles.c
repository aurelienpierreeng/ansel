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

#include "canvas/canvas_handles.h"

#include "canvas/canvas_paint.h"
#include "system/macros.h"

#include <math.h>
#include <string.h>

/** The waypoint and the tangent handles are drawn 7 pixels across and caught this much wider. */
#define HANDLES_VIA_GRIP_PIXELS 3.0
/**
 * A tether is stroked three pixels wide with its halo, the knob's stem one. Nothing grabs either,
 * but a control laid over one hides which handle belongs to what, so each is a site all the same.
 */
#define HANDLES_LINE_REACH_PIXELS 2.0

/* --- the cutout's geometry ------------------------------------------------------ */

double dt_canvas_mask_side(const dt_canvas_object_t *object)
{
  return fmax(fmin(object->width, object->height), 1.0);
}

void dt_canvas_mask_to_local(const dt_canvas_object_t *object, const double u, const double v, double *local_x,
                             double *local_y)
{
  *local_x = (u - 0.5) * object->width;
  *local_y = (v - 0.5) * object->height;
}

double dt_canvas_mask_node_border(const dt_canvas_mask_t *mask, const uint32_t index)
{
  // A node is born with no fall-off of its own, so a polygon reads as it always did until the
  // user pulls one node's feather out.
  const float *node = mask->nodes + (size_t)index * DT_CANVAS_MASK_NODE_FLOATS;
  const double own = node[DT_CANVAS_MASK_NODE_BORDER1];
  return own > 0.0 ? own : mask->feather;
}

/** The average of a polygon's nodes, in local units: inside any polygon the user can draw here. */
static void _mask_node_centre(const dt_canvas_object_t *object, double *centre_x, double *centre_y)
{
  const dt_canvas_mask_t *mask = &object->mask;
  const uint32_t count = mask->node_count;
  *centre_x = 0.0;
  *centre_y = 0.0;
  if(count == 0) return;
  for(uint32_t idx = 0; idx < count; idx++)
  {
    const float *other = mask->nodes + (size_t)idx * DT_CANVAS_MASK_NODE_FLOATS;
    double other_x = 0.0;
    double other_y = 0.0;
    dt_canvas_mask_to_local(object, other[0], other[1], &other_x, &other_y);
    *centre_x += other_x;
    *centre_y += other_y;
  }
  *centre_x /= count;
  *centre_y /= count;
}

/**
 * The outward unit normal at a node, in local units: perpendicular to the line through its
 * neighbours, turned away from a centre the caller already has. The node's feather handle is hung
 * along it, so the handle leaves the shape rather than lying along it. The centre is the caller's
 * so that listing every node's handles averages the nodes once rather than once per node: a
 * 512-node polygon would otherwise cost a quarter of a million additions per pointer motion.
 */
static void _mask_node_normal_about(const dt_canvas_object_t *object, const uint32_t index, const double centre_x,
                                    const double centre_y, double *normal_x, double *normal_y)
{
  const dt_canvas_mask_t *mask = &object->mask;
  const uint32_t count = mask->node_count;
  *normal_x = 1.0;
  *normal_y = 0.0;
  if(count < 3) return;
  const float *previous = mask->nodes + (size_t)((index + count - 1) % count) * DT_CANVAS_MASK_NODE_FLOATS;
  const float *node = mask->nodes + (size_t)index * DT_CANVAS_MASK_NODE_FLOATS;
  const float *next = mask->nodes + (size_t)((index + 1) % count) * DT_CANVAS_MASK_NODE_FLOATS;
  double previous_x = 0.0;
  double previous_y = 0.0;
  double node_x = 0.0;
  double node_y = 0.0;
  double next_x = 0.0;
  double next_y = 0.0;
  dt_canvas_mask_to_local(object, previous[0], previous[1], &previous_x, &previous_y);
  dt_canvas_mask_to_local(object, node[0], node[1], &node_x, &node_y);
  dt_canvas_mask_to_local(object, next[0], next[1], &next_x, &next_y);
  double tangent_x = next_x - previous_x;
  double tangent_y = next_y - previous_y;
  const double length = hypot(tangent_x, tangent_y);
  if(!(length > 0.0)) return;
  tangent_x /= length;
  tangent_y /= length;
  *normal_x = tangent_y;
  *normal_y = -tangent_x;
  if((node_x - centre_x) * *normal_x + (node_y - centre_y) * *normal_y < 0.0)
  {
    *normal_x = -*normal_x;
    *normal_y = -*normal_y;
  }
}

void dt_canvas_mask_polygon_controls(const dt_canvas_mask_t *mask, const uint32_t index, float control1[2],
                                     float control2[2])
{
  const uint32_t count = mask->node_count;
  const float *previous = mask->nodes + (size_t)((index + count - 1) % count) * DT_CANVAS_MASK_NODE_FLOATS;
  const float *from = mask->nodes + (size_t)index * DT_CANVAS_MASK_NODE_FLOATS;
  const float *to = mask->nodes + (size_t)((index + 1) % count) * DT_CANVAS_MASK_NODE_FLOATS;
  const float *after = mask->nodes + (size_t)((index + 2) % count) * DT_CANVAS_MASK_NODE_FLOATS;
  // Only an AUTO node has its tangent computed: a cusp and a steered node both carry theirs,
  // and the difference between those two is what a DRAG does to them, not what is read here.
  if(from[DT_CANVAS_MASK_NODE_SMOOTH] == (float)DT_CANVAS_MASK_NODE_AUTO)
  {
    control1[0] = (-previous[0] + 6.0f * from[0] + to[0]) / 6.0f;
    control1[1] = (-previous[1] + 6.0f * from[1] + to[1]) / 6.0f;
  }
  else
  {
    control1[0] = from[DT_CANVAS_MASK_NODE_CTRL2_X];
    control1[1] = from[DT_CANVAS_MASK_NODE_CTRL2_Y];
  }
  if(to[DT_CANVAS_MASK_NODE_SMOOTH] == (float)DT_CANVAS_MASK_NODE_AUTO)
  {
    control2[0] = (from[0] + 6.0f * to[0] - after[0]) / 6.0f;
    control2[1] = (from[1] + 6.0f * to[1] - after[1]) / 6.0f;
  }
  else
  {
    control2[0] = to[DT_CANVAS_MASK_NODE_CTRL1_X];
    control2[1] = to[DT_CANVAS_MASK_NODE_CTRL1_Y];
  }
}

void dt_canvas_mask_node_controls(const dt_canvas_mask_t *mask, const uint32_t index, float incoming[2],
                                  float outgoing[2])
{
  const uint32_t count = mask->node_count;
  float control1[2];
  float control2[2];
  dt_canvas_mask_polygon_controls(mask, index, control1, control2);
  outgoing[0] = control1[0];
  outgoing[1] = control1[1];
  dt_canvas_mask_polygon_controls(mask, (index + count - 1) % count, control1, control2);
  incoming[0] = control2[0];
  incoming[1] = control2[1];
}

static void _mask_node_handles_about(const dt_canvas_object_t *object, const uint32_t index, const double centre_x,
                                     const double centre_y, double border[2], double incoming[2],
                                     double outgoing[2])
{
  const dt_canvas_mask_t *mask = &object->mask;
  const float *node = mask->nodes + (size_t)index * DT_CANVAS_MASK_NODE_FLOATS;
  double node_x = 0.0;
  double node_y = 0.0;
  dt_canvas_mask_to_local(object, node[0], node[1], &node_x, &node_y);
  double normal_x = 0.0;
  double normal_y = 0.0;
  _mask_node_normal_about(object, index, centre_x, centre_y, &normal_x, &normal_y);
  const double reach = dt_canvas_mask_node_border(mask, index) * dt_canvas_mask_side(object);
  border[0] = node_x + normal_x * reach;
  border[1] = node_y + normal_y * reach;
  float in_point[2];
  float out_point[2];
  dt_canvas_mask_node_controls(mask, index, in_point, out_point);
  dt_canvas_mask_to_local(object, in_point[0], in_point[1], &incoming[0], &incoming[1]);
  dt_canvas_mask_to_local(object, out_point[0], out_point[1], &outgoing[0], &outgoing[1]);
}

void dt_canvas_mask_node_handles(const dt_canvas_object_t *object, const uint32_t index, double border[2],
                                 double incoming[2], double outgoing[2])
{
  double centre_x = 0.0;
  double centre_y = 0.0;
  _mask_node_centre(object, &centre_x, &centre_y);
  _mask_node_handles_about(object, index, centre_x, centre_y, border, incoming, outgoing);
}

int dt_canvas_mask_handle_points(const dt_canvas_object_t *object, double points[8])
{
  const dt_canvas_mask_t *mask = &object->mask;
  const double side = dt_canvas_mask_side(object);
  double center_x = 0.0;
  double center_y = 0.0;
  dt_canvas_mask_to_local(object, mask->center_x, mask->center_y, &center_x, &center_y);
  points[0] = center_x;
  points[1] = center_y;
  const double angle = mask->rotation * M_PI / 180.0;
  switch(mask->shape)
  {
    case DT_CANVAS_MASK_CIRCLE:
      points[2] = center_x + mask->radius_x * side;
      points[3] = center_y;
      // The feather handle sits on the dashed ring, off the radius handle's axis.
      points[6] = center_x + M_SQRT1_2 * (mask->radius_x + mask->feather) * side;
      points[7] = center_y + M_SQRT1_2 * (mask->radius_x + mask->feather) * side;
      return 4;
    case DT_CANVAS_MASK_ELLIPSE:
      points[2] = center_x + cos(angle) * mask->radius_x * side;
      points[3] = center_y + sin(angle) * mask->radius_x * side;
      points[4] = center_x - sin(angle) * mask->radius_y * side;
      points[5] = center_y + cos(angle) * mask->radius_y * side;
      points[6] = center_x - cos(angle) * (mask->radius_x + mask->feather) * side;
      points[7] = center_y - sin(angle) * (mask->radius_x + mask->feather) * side;
      return 4;
    case DT_CANVAS_MASK_GRADIENT:
      // The reach handle sits across the line, on the side the fall-off goes.
      points[2] = center_x - sin(angle) * mask->radius_x * side;
      points[3] = center_y + cos(angle) * mask->radius_x * side;
      return 2;
    default:
      return mask->shape == DT_CANVAS_MASK_NONE ? 0 : 1;
  }
}

/* --- the sites -------------------------------------------------------------------- */

/** The list being filled: sites are counted whether or not there is room to write them. */
typedef struct handles_list_t
{
  dt_canvas_handle_site_t *out;
  size_t max;
  size_t count;
} handles_list_t;

static dt_canvas_handle_site_t *_site_add(handles_list_t *list, const uint32_t role, const uint32_t shape)
{
  const size_t slot = list->count;
  list->count++;
  if(IS_NULL_PTR(list->out) || slot >= list->max) return NULL;
  dt_canvas_handle_site_t *site = &list->out[slot];
  site->role = role;
  site->shape = shape;
  site->part = DT_CANVAS_HANDLE_PART_NONE;
  site->index = 0;
  site->sign = 0;
  site->latent = FALSE;
  site->x0 = 0.0;
  site->y0 = 0.0;
  site->x1 = 0.0;
  site->y1 = 0.0;
  site->angle = 0.0;
  site->offset_px_x = 0.0;
  site->offset_px_y = 0.0;
  site->reach_px = 0.0;
  site->reach_units = 0.0;
  return site;
}

/** A point, a square or a disc, in canvas units. */
static void _site_point(handles_list_t *list, const uint32_t role, const uint32_t shape, const double x,
                        const double y, const double angle, const double reach_px, const int index)
{
  dt_canvas_handle_site_t *site = _site_add(list, role, shape);
  if(IS_NULL_PTR(site)) return;
  site->x0 = x;
  site->y0 = y;
  site->x1 = x;
  site->y1 = y;
  site->angle = angle;
  site->reach_px = reach_px;
  site->index = index;
}

static void _site_segment(handles_list_t *list, const uint32_t role, const double from_x, const double from_y,
                          const double to_x, const double to_y, const double reach_px, const int index)
{
  dt_canvas_handle_site_t *site = _site_add(list, role, DT_CANVAS_HANDLE_SEGMENT);
  if(IS_NULL_PTR(site)) return;
  site->x0 = from_x;
  site->y0 = from_y;
  site->x1 = to_x;
  site->y1 = to_y;
  site->reach_px = reach_px;
  site->index = index;
}

/** A frame's local point in canvas units, turned the way dt_canvas_object_corners() turns them. */
static void _local_to_canvas(const dt_canvas_object_t *object, const double local_x, const double local_y,
                             double *x, double *y)
{
  const double cos_r = cos(object->rotation);
  const double sin_r = sin(object->rotation);
  *x = object->x + local_x * cos_r - local_y * sin_r;
  *y = object->y + local_x * sin_r + local_y * cos_r;
}

static void _frame_sites(handles_list_t *list, const dt_canvas_object_t *object)
{
  if(object->flags & DT_CANVAS_OBJECT_FLAG_LOCKED) return;
  double corners[8];
  dt_canvas_object_corners(object, corners);
  for(int idx = 0; idx < 4; idx++)
    _site_point(list, DT_CANVAS_HANDLE_CORNER, DT_CANVAS_HANDLE_SQUARE, corners[2 * idx], corners[2 * idx + 1],
                object->rotation, DT_CANVAS_HANDLE_PIXELS, idx);
  // The knob floats a fixed distance on SCREEN above the top edge, so its centre is the edge's
  // middle plus a pixel offset along the frame's own up.
  double top_x = 0.0;
  double top_y = 0.0;
  _local_to_canvas(object, 0.0, -object->height * 0.5, &top_x, &top_y);
  dt_canvas_handle_site_t *knob = _site_add(list, DT_CANVAS_HANDLE_ROTATE, DT_CANVAS_HANDLE_SQUARE);
  if(!IS_NULL_PTR(knob))
  {
    knob->x0 = top_x;
    knob->y0 = top_y;
    knob->x1 = top_x;
    knob->y1 = top_y;
    knob->angle = object->rotation;
    knob->offset_px_y = -DT_CANVAS_ROTATE_HANDLE_OFFSET_PIXELS;
    knob->reach_px = DT_CANVAS_HANDLE_PIXELS;
  }
  dt_canvas_handle_site_t *stem = _site_add(list, DT_CANVAS_HANDLE_ROTATE_STEM, DT_CANVAS_HANDLE_SEGMENT);
  if(!IS_NULL_PTR(stem))
  {
    stem->x0 = top_x;
    stem->y0 = top_y;
    stem->x1 = top_x;
    stem->y1 = top_y;
    stem->angle = object->rotation;
    stem->offset_px_y = -DT_CANVAS_ROTATE_HANDLE_OFFSET_PIXELS;
    stem->reach_px = HANDLES_LINE_REACH_PIXELS;
  }
}

static void _mask_sites(handles_list_t *list, const dt_canvas_object_t *object, const uint32_t what)
{
  const dt_canvas_mask_t *mask = &object->mask;
  if(mask->shape == DT_CANVAS_MASK_NONE) return;
  if(what & DT_CANVAS_HANDLES_MASK_POINTS)
  {
    double points[8] = { 0.0 };
    const int count = dt_canvas_mask_handle_points(object, points);
    for(int idx = 0; idx < count; idx++)
    {
      double x = 0.0;
      double y = 0.0;
      _local_to_canvas(object, points[2 * idx], points[2 * idx + 1], &x, &y);
      _site_point(list, DT_CANVAS_HANDLE_MASK_POINT, DT_CANVAS_HANDLE_DISC, x, y, object->rotation,
                  DT_CANVAS_HANDLE_PIXELS, idx);
    }
  }
  if(mask->shape != DT_CANVAS_MASK_POLYGON) return;
  const uint32_t node_count = mask->node_count;
  if(what & DT_CANVAS_HANDLES_MASK_NODES)
  {
    for(uint32_t idx = 0; idx < node_count; idx++)
    {
      const float *node = mask->nodes + (size_t)idx * DT_CANVAS_MASK_NODE_FLOATS;
      double local_x = 0.0;
      double local_y = 0.0;
      dt_canvas_mask_to_local(object, node[0], node[1], &local_x, &local_y);
      double x = 0.0;
      double y = 0.0;
      _local_to_canvas(object, local_x, local_y, &x, &y);
      _site_point(list, DT_CANVAS_HANDLE_MASK_NODE, DT_CANVAS_HANDLE_SQUARE, x, y, object->rotation,
                  DT_CANVAS_HANDLE_PIXELS, (int)idx);
    }
  }
  if(what & DT_CANVAS_HANDLES_MASK_NODE_OWN)
  {
    // Every node's, not only the one showing: hovering picks whichever node is nearest, so any
    // of them can appear under the pointer without a click.
    double centre_x = 0.0;
    double centre_y = 0.0;
    _mask_node_centre(object, &centre_x, &centre_y);
    for(uint32_t idx = 0; idx < node_count; idx++)
    {
      double border[2] = { 0.0, 0.0 };
      double incoming[2] = { 0.0, 0.0 };
      double outgoing[2] = { 0.0, 0.0 };
      _mask_node_handles_about(object, idx, centre_x, centre_y, border, incoming, outgoing);
      // In the order the hit test tries them: the fall-off, then the control points.
      const double *own[3] = { border, incoming, outgoing };
      const uint32_t parts[3]
          = { DT_CANVAS_HANDLE_PART_BORDER, DT_CANVAS_HANDLE_PART_INCOMING, DT_CANVAS_HANDLE_PART_OUTGOING };
      for(int which = 0; which < 3; which++)
      {
        double x = 0.0;
        double y = 0.0;
        _local_to_canvas(object, own[which][0], own[which][1], &x, &y);
        const size_t slot = list->count;
        _site_point(list, DT_CANVAS_HANDLE_MASK_NODE_OWN, DT_CANVAS_HANDLE_DISC, x, y, object->rotation,
                    DT_CANVAS_HANDLE_PIXELS, (int)idx);
        if(IS_NULL_PTR(list->out) || slot >= list->max) continue;
        list->out[slot].part = parts[which];
        list->out[slot].latent = TRUE;
      }
    }
  }
  if(what & DT_CANVAS_HANDLES_MASK_EDGES)
  {
    // Straight, node to node, whatever the curve does between them: the edge Ctrl+click
    // inserts a node on.
    for(uint32_t idx = 0; idx < node_count; idx++)
    {
      const float *from = mask->nodes + (size_t)idx * DT_CANVAS_MASK_NODE_FLOATS;
      const float *to = mask->nodes + (size_t)((idx + 1) % node_count) * DT_CANVAS_MASK_NODE_FLOATS;
      double from_local_x = 0.0;
      double from_local_y = 0.0;
      double to_local_x = 0.0;
      double to_local_y = 0.0;
      dt_canvas_mask_to_local(object, from[0], from[1], &from_local_x, &from_local_y);
      dt_canvas_mask_to_local(object, to[0], to[1], &to_local_x, &to_local_y);
      double from_x = 0.0;
      double from_y = 0.0;
      double to_x = 0.0;
      double to_y = 0.0;
      _local_to_canvas(object, from_local_x, from_local_y, &from_x, &from_y);
      _local_to_canvas(object, to_local_x, to_local_y, &to_x, &to_y);
      _site_segment(list, DT_CANVAS_HANDLE_MASK_EDGE, from_x, from_y, to_x, to_y, DT_CANVAS_HANDLE_PIXELS, (int)idx);
    }
  }
}

/** One tangent handle and its tether, the handle first. */
static void _tangent_sites(handles_list_t *list, const double anchor_x, const double anchor_y, const double handle_x,
                           const double handle_y, const int index, const uint32_t part, const int sign)
{
  const size_t slot = list->count;
  _site_point(list, DT_CANVAS_HANDLE_TANGENT, DT_CANVAS_HANDLE_DISC, handle_x, handle_y, 0.0,
              DT_CANVAS_VIA_HANDLE_PIXELS + HANDLES_VIA_GRIP_PIXELS, index);
  if(!IS_NULL_PTR(list->out) && slot < list->max)
  {
    list->out[slot].part = part;
    list->out[slot].sign = sign;
  }
  const size_t tether = list->count;
  _site_segment(list, DT_CANVAS_HANDLE_TETHER, anchor_x, anchor_y, handle_x, handle_y, HANDLES_LINE_REACH_PIXELS,
                index);
  if(!IS_NULL_PTR(list->out) && tether < list->max)
  {
    list->out[tether].part = part;
    list->out[tether].sign = sign;
  }
}

static void _connector_sites(handles_list_t *list, const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                             const uint32_t what)
{
  const dt_canvas_connector_t *connector = &object->connector;
  dt_canvas_route_t route;
  memset(&route, 0, sizeof(route));
  const uint32_t routed_sets = DT_CANVAS_HANDLES_ENDPOINTS | DT_CANVAS_HANDLES_TANGENTS | DT_CANVAS_HANDLES_CURVE;
  const gboolean routed = (what & routed_sets) && dt_canvas_connector_route(canvas, object, &route);
  /*
   * BOTH ends are handles now. A free end is the line's own to move; an ANCHORED end is where
   * its frame puts it, and dragging it is how the attachment point is chosen -- dropped on one
   * of the frame's anchor dots it re-attaches there, and dropped anywhere else it stays where it
   * was. The part (FROM or TO) already tells the two apart, so no new role is needed.
   * A locked line keeps its ends where they are, as a locked frame keeps its corners.
   */
  const gboolean ends_movable = (what & DT_CANVAS_HANDLES_ENDPOINTS) && routed
                                && !(object->flags & DT_CANVAS_OBJECT_FLAG_LOCKED);
  if(ends_movable)
  {
    const double end_x[2] = { route.from_x, route.to_x };
    const double end_y[2] = { route.from_y, route.to_y };
    const uint32_t end_part[2] = { DT_CANVAS_HANDLE_PART_FROM, DT_CANVAS_HANDLE_PART_TO };
    for(int end = 0; end < 2; end++)
    {
      // Along the canvas's own axes, as the waypoint is: a line has no turn of its own to follow.
      const size_t slot = list->count;
      _site_point(list, DT_CANVAS_HANDLE_ENDPOINT, DT_CANVAS_HANDLE_SQUARE, end_x[end], end_y[end], 0.0,
                  DT_CANVAS_VIA_HANDLE_PIXELS + HANDLES_VIA_GRIP_PIXELS, end);
      if(!IS_NULL_PTR(list->out) && slot < list->max) list->out[slot].part = end_part[end];
    }
  }
  if((what & DT_CANVAS_HANDLES_TANGENTS) && routed && connector->routing == DT_CANVAS_ROUTING_CUBIC)
  {
    // Control points in the order the hit test tries them, each with the end it steers: on a
    // route through a waypoint the middle two turn the waypoint's one tangent, either side.
    _tangent_sites(list, route.from_x, route.from_y, route.control1_x, route.control1_y, 1,
                   DT_CANVAS_HANDLE_PART_FROM, 0);
    if(route.segment_count == 2)
    {
      _tangent_sites(list, route.via_x, route.via_y, route.control2_x, route.control2_y, 2,
                     DT_CANVAS_HANDLE_PART_VIA, -1);
      _tangent_sites(list, route.via_x, route.via_y, route.control3_x, route.control3_y, 3,
                     DT_CANVAS_HANDLE_PART_VIA, 1);
      _tangent_sites(list, route.to_x, route.to_y, route.control4_x, route.control4_y, 4, DT_CANVAS_HANDLE_PART_TO,
                     0);
    }
    else
    {
      _tangent_sites(list, route.to_x, route.to_y, route.control2_x, route.control2_y, 2, DT_CANVAS_HANDLE_PART_TO,
                     0);
    }
  }
  if((what & DT_CANVAS_HANDLES_VIA) && connector->via_count > 0)
  {
    const size_t slot = list->count;
    _site_point(list, DT_CANVAS_HANDLE_VIA, DT_CANVAS_HANDLE_SQUARE, connector->via_x, connector->via_y, 0.0,
                DT_CANVAS_VIA_HANDLE_PIXELS + HANDLES_VIA_GRIP_PIXELS, 0);
    if(!IS_NULL_PTR(list->out) && slot < list->max) list->out[slot].part = DT_CANVAS_HANDLE_PART_VIA;
  }
  if((what & DT_CANVAS_HANDLES_CURVE) && routed)
  {
    // Its own line, or the canvas's when it carries none: what the pick measures against has to
    // be the line that is actually drawn, or a connector inheriting a thick one is hard to hit.
    float effective_line = 0.0f;
    dt_canvas_object_effective_line(canvas, object, NULL, &effective_line);
    // The pick's band exactly: the tolerance on screen plus the line's whole width in units,
    // as dt_canvas_object_contains() measures it, leg by leg of the flattened route.
    for(int idx = 0; idx + 1 < route.point_count; idx++)
    {
      const size_t slot = list->count;
      _site_segment(list, DT_CANVAS_HANDLE_CURVE, route.points[2 * idx], route.points[2 * idx + 1],
                    route.points[2 * idx + 2], route.points[2 * idx + 3], DT_CANVAS_PICK_TOLERANCE_PIXELS, idx);
      if(!IS_NULL_PTR(list->out) && slot < list->max) list->out[slot].reach_units = effective_line;
    }
    const double arrow_reach = dt_canvas_paint_arrow_reach(effective_line);
    const uint32_t ends[2] = { DT_CANVAS_CONNECTOR_ARROW_START, DT_CANVAS_CONNECTOR_ARROW_END };
    for(int end = 0; end < 2; end++)
    {
      if(!(connector->style & ends[end])) continue;
      const size_t slot = list->count;
      const double tip_x = end == 0 ? route.from_x : route.to_x;
      const double tip_y = end == 0 ? route.from_y : route.to_y;
      _site_point(list, DT_CANVAS_HANDLE_ARROW, DT_CANVAS_HANDLE_DISC, tip_x, tip_y, 0.0, 0.0, end);
      if(IS_NULL_PTR(list->out) || slot >= list->max) continue;
      list->out[slot].part = end == 0 ? DT_CANVAS_HANDLE_PART_FROM : DT_CANVAS_HANDLE_PART_TO;
      list->out[slot].reach_units = arrow_reach;
    }
  }
}

size_t dt_canvas_handle_sites(const dt_canvas_t *canvas, const dt_canvas_object_t *object, const uint32_t what,
                              dt_canvas_handle_site_t *out, const size_t max)
{
  handles_list_t list = { out, max, 0 };
  if(IS_NULL_PTR(object)) return 0;
  if(dt_canvas_object_is_frame(object))
  {
    if(what & DT_CANVAS_HANDLES_FRAME) _frame_sites(&list, object);
    if(what & DT_CANVAS_HANDLES_MASK) _mask_sites(&list, object, what);
  }
  else if(object->kind == DT_CANVAS_OBJECT_CONNECTOR)
  {
    _connector_sites(&list, canvas, object, what);
  }
  return list.count;
}

void dt_canvas_handle_site_resolve(const dt_canvas_handle_site_t *site, const double zoom, double *x0, double *y0,
                                   double *x1, double *y1, double *reach)
{
  const double cos_a = cos(site->angle);
  const double sin_a = sin(site->angle);
  const double shift_x = (site->offset_px_x * cos_a - site->offset_px_y * sin_a) / zoom;
  const double shift_y = (site->offset_px_x * sin_a + site->offset_px_y * cos_a) / zoom;
  *reach = site->reach_px / zoom + site->reach_units;
  if(site->shape == DT_CANVAS_HANDLE_SEGMENT)
  {
    // Only the far end moves: the knob's stem starts on the frame's edge and ends at the knob.
    *x0 = site->x0;
    *y0 = site->y0;
    *x1 = site->x1 + shift_x;
    *y1 = site->y1 + shift_y;
    return;
  }
  *x0 = site->x0 + shift_x;
  *y0 = site->y0 + shift_y;
  *x1 = *x0;
  *y1 = *y0;
}

gboolean dt_canvas_handle_site_hit(const dt_canvas_handle_site_t *site, const double x, const double y,
                                   const double zoom)
{
  if(IS_NULL_PTR(site) || !(zoom > 0.0)) return FALSE;
  double x0 = 0.0;
  double y0 = 0.0;
  double x1 = 0.0;
  double y1 = 0.0;
  double reach = 0.0;
  dt_canvas_handle_site_resolve(site, zoom, &x0, &y0, &x1, &y1, &reach);
  // Every test is asked as "within reach", never as "not beyond it": a site whose position or
  // turn is not a number -- a corrupt file reads straight into the cutout's floats -- then catches
  // nothing. The view asked the cutout's handles the other way round, and such a handle caught
  // every press made anywhere while it was showing.
  switch(site->shape)
  {
    case DT_CANVAS_HANDLE_SQUARE:
    {
      // Into the square's own axes the way dt_canvas_object_to_local() turns a point into a frame's.
      const double offset_x = x - x0;
      const double offset_y = y - y0;
      const double cos_r = cos(-site->angle);
      const double sin_r = sin(-site->angle);
      const double local_x = offset_x * cos_r - offset_y * sin_r;
      const double local_y = offset_x * sin_r + offset_y * cos_r;
      return fabs(local_x) <= reach && fabs(local_y) <= reach;
    }
    case DT_CANVAS_HANDLE_DISC:
      return hypot(x0 - x, y0 - y) <= reach;
    case DT_CANVAS_HANDLE_SEGMENT:
      // The pick's own measure, so the band a CURVE site answers for is the pick's.
      return dt_canvas_segment_distance(x, y, x0, y0, x1, y1) <= reach;
    default:
      return FALSE;
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
