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

#include "canvas/canvas_place_shapes.h"

#include "system/macros.h"
#include "system/mem_alloc.h"

#include <math.h>

/** The longest piece a line is kept clear in: its box is then no wider than the band plus this. */
#define PLACE_SHAPES_PIECE_PIXELS 16.0
/** The slabs a frame's body is cut into, so a turned frame's corners are left to the placement. */
#define PLACE_SHAPES_BODY_SLABS 16

void dt_canvas_place_to_screen(const dt_canvas_place_view_t *view, const double canvas_x, const double canvas_y,
                               double *screen_x, double *screen_y)
{
  *screen_x = (canvas_x - view->center_x) * view->zoom + view->width * 0.5;
  *screen_y = (canvas_y - view->center_y) * view->zoom + view->height * 0.5;
}

void dt_canvas_place_shape_add(GArray *shapes, const dt_canvas_place_class_t shape_class, const double x0,
                               const double y0, const double x1, const double y1, const double weight,
                               const gboolean target)
{
  dt_canvas_place_shape_t shape;
  const double left = floor(MIN(x0, x1));
  const double top = floor(MIN(y0, y1));
  shape.rect.x = left;
  shape.rect.y = top;
  shape.rect.width = ceil(MAX(x0, x1)) - left;
  shape.rect.height = ceil(MAX(y0, y1)) - top;
  shape.shape_class = shape_class;
  shape.weight = weight;
  shape.target = target;
  g_array_append_val(shapes, shape);
}

/** Clip a segment to a box, in place. FALSE when none of it is inside, or it is not a number. */
static gboolean _clip_segment(double *x0, double *y0, double *x1, double *y1, const double left, const double top,
                              const double right, const double bottom)
{
  if(!isfinite(*x0) || !isfinite(*y0) || !isfinite(*x1) || !isfinite(*y1)) return FALSE;
  const double delta_x = *x1 - *x0;
  const double delta_y = *y1 - *y0;
  const double directions[4] = { -delta_x, delta_x, -delta_y, delta_y };
  const double distances[4] = { *x0 - left, right - *x0, *y0 - top, bottom - *y0 };
  double enter = 0.0;
  double leave = 1.0;
  for(int edge = 0; edge < 4; edge++)
  {
    if(directions[edge] == 0.0)
    {
      if(distances[edge] < 0.0) return FALSE;
      continue;
    }
    const double along = distances[edge] / directions[edge];
    if(directions[edge] < 0.0)
      enter = MAX(enter, along);
    else
      leave = MIN(leave, along);
    if(enter > leave) return FALSE;
  }
  const double start_x = *x0;
  const double start_y = *y0;
  *x0 = start_x + delta_x * enter;
  *y0 = start_y + delta_y * enter;
  *x1 = start_x + delta_x * leave;
  *y1 = start_y + delta_y * leave;
  return TRUE;
}

void dt_canvas_place_sites_add(GArray *shapes, const dt_canvas_place_view_t *view,
                               const dt_canvas_handle_site_t *sites, const size_t count,
                               const dt_canvas_place_class_t shape_class)
{
  if(IS_NULL_PTR(shapes) || IS_NULL_PTR(view) || IS_NULL_PTR(sites) || !(view->zoom > 0.0)) return;
  const double margin = view->margin;
  for(size_t idx = 0; idx < count; idx++)
  {
    const dt_canvas_handle_site_t *site = &sites[idx];
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = 0.0;
    double y1 = 0.0;
    double reach = 0.0;
    dt_canvas_handle_site_resolve(site, view->zoom, &x0, &y0, &x1, &y1, &reach);
    const double reach_pixels = ceil(reach * view->zoom);
    // Asked as "within reach", never as "not beyond it": a site that is not a number keeps nothing clear,
    // as it catches nothing.
    if(!isfinite(reach_pixels)) continue;
    double start_x = 0.0;
    double start_y = 0.0;
    double end_x = 0.0;
    double end_y = 0.0;
    dt_canvas_place_to_screen(view, x0, y0, &start_x, &start_y);
    dt_canvas_place_to_screen(view, x1, y1, &end_x, &end_y);
    if(site->shape == DT_CANVAS_HANDLE_SEGMENT)
    {
      const double grow = reach_pixels + margin;
      if(!_clip_segment(&start_x, &start_y, &end_x, &end_y, -grow, -grow, view->width + grow, view->height + grow))
        continue;
      const double length = hypot(end_x - start_x, end_y - start_y);
      const int pieces = MAX((int)ceil(length / PLACE_SHAPES_PIECE_PIXELS), 1);
      for(int piece = 0; piece < pieces; piece++)
      {
        const double from = (double)piece / pieces;
        const double to = (double)(piece + 1) / pieces;
        const double from_x = start_x + (end_x - start_x) * from;
        const double from_y = start_y + (end_y - start_y) * from;
        const double to_x = start_x + (end_x - start_x) * to;
        const double to_y = start_y + (end_y - start_y) * to;
        dt_canvas_place_shape_add(shapes, shape_class, MIN(from_x, to_x) - reach_pixels,
                                  MIN(from_y, to_y) - reach_pixels, MAX(from_x, to_x) + reach_pixels,
                                  MAX(from_y, to_y) + reach_pixels, 0.0, TRUE);
      }
      continue;
    }
    if(!isfinite(start_x) || !isfinite(start_y) || !isfinite(site->angle)) continue;
    const double half = site->shape == DT_CANVAS_HANDLE_SQUARE
                            ? reach_pixels * (fabs(cos(site->angle)) + fabs(sin(site->angle)))
                            : reach_pixels;
    if(start_x + half < -margin || start_x - half > view->width + margin || start_y + half < -margin
       || start_y - half > view->height + margin)
      continue;
    dt_canvas_place_shape_add(shapes, shape_class, start_x - half, start_y - half, start_x + half, start_y + half,
                              0.0, TRUE);
  }
}

/** An object's sites of some kinds, as rectangles. */
static void _object_sites_add(GArray *shapes, const dt_canvas_place_view_t *view, const dt_canvas_t *canvas,
                              const dt_canvas_object_t *object, const uint32_t what,
                              const dt_canvas_place_class_t shape_class)
{
  const size_t count = dt_canvas_handle_sites(canvas, object, what, NULL, 0);
  if(count == 0) return;
  dt_canvas_handle_site_t *sites = g_new(dt_canvas_handle_site_t, count);
  const size_t written = dt_canvas_handle_sites(canvas, object, what, sites, count);
  dt_canvas_place_sites_add(shapes, view, sites, MIN(written, count), shape_class);
  dt_free(sites);
}

/** The lowest and highest x a quadrilateral reaches between two heights. FALSE when it does not reach them. */
static gboolean _quad_band_extent(const double points[8], const double low, const double high, double *min_x,
                                  double *max_x)
{
  gboolean found = FALSE;
  *min_x = INFINITY;
  *max_x = -INFINITY;
  for(int corner = 0; corner < 4; corner++)
  {
    const double x = points[2 * corner];
    const double y = points[2 * corner + 1];
    if(y >= low && y <= high)
    {
      *min_x = MIN(*min_x, x);
      *max_x = MAX(*max_x, x);
      found = TRUE;
    }
    const double next_x = points[2 * ((corner + 1) % 4)];
    const double next_y = points[2 * ((corner + 1) % 4) + 1];
    if(next_y == y) continue;
    const double levels[2] = { low, high };
    for(int level = 0; level < 2; level++)
    {
      const double along = (levels[level] - y) / (next_y - y);
      if(!(along >= 0.0 && along <= 1.0)) continue;
      const double crossing = x + (next_x - x) * along;
      *min_x = MIN(*min_x, crossing);
      *max_x = MAX(*max_x, crossing);
      found = TRUE;
    }
  }
  return found;
}

void dt_canvas_place_body_add(GArray *shapes, const dt_canvas_place_view_t *view, const dt_canvas_object_t *frame)
{
  if(IS_NULL_PTR(shapes) || IS_NULL_PTR(view) || IS_NULL_PTR(frame)) return;
  double corners[8];
  dt_canvas_object_corners(frame, corners);
  double points[8];
  double top = INFINITY;
  double bottom = -INFINITY;
  for(int corner = 0; corner < 4; corner++)
  {
    dt_canvas_place_to_screen(view, corners[2 * corner], corners[2 * corner + 1], &points[2 * corner],
                              &points[2 * corner + 1]);
    top = MIN(top, points[2 * corner + 1]);
    bottom = MAX(bottom, points[2 * corner + 1]);
  }
  if(!isfinite(top) || !isfinite(bottom)) return;
  const double slab_height = (bottom - top) / PLACE_SHAPES_BODY_SLABS;
  const double tolerance = DT_CANVAS_PICK_TOLERANCE_PIXELS;
  for(int slab = 0; slab < PLACE_SHAPES_BODY_SLABS; slab++)
  {
    const double low = top + slab_height * slab;
    const double high = slab == PLACE_SHAPES_BODY_SLABS - 1 ? bottom : low + slab_height;
    if(high < -tolerance || low > view->height + tolerance) continue;
    double min_x = 0.0;
    double max_x = 0.0;
    if(!_quad_band_extent(points, low, high, &min_x, &max_x)) continue;
    dt_canvas_place_shape_add(shapes, DT_CANVAS_PLACE_BODY, min_x - tolerance, low - tolerance, max_x + tolerance,
                              high + tolerance, 0.0, TRUE);
  }
}

/**
 * The handles one click in the connector's properties would create -- the tangents of a cubic
 * route, a waypoint -- asked of a COPY of the connector with that click applied. The copy never
 * reaches the document: the route and the site list only read it, where the real edit touches the
 * canvas. Both orders of the two clicks are asked, since a waypoint lands halfway along whichever
 * route it is added to.
 */
static void _connector_predicted_add(GArray *shapes, const dt_canvas_place_view_t *view, const dt_canvas_t *canvas,
                                     const dt_canvas_object_t *connector)
{
  const gboolean straight = connector->connector.routing != DT_CANVAS_ROUTING_CUBIC;
  const gboolean without_via = connector->connector.via_count == 0;
  if(!straight && !without_via) return;
  dt_canvas_object_t *probes = g_new(dt_canvas_object_t, 4);
  int probe_count = 0;
  dt_canvas_route_t route;
  if(straight)
  {
    probes[probe_count] = *connector;
    probes[probe_count].connector.routing = DT_CANVAS_ROUTING_CUBIC;
    probe_count++;
  }
  if(without_via && dt_canvas_connector_route(canvas, connector, &route))
  {
    probes[probe_count] = *connector;
    dt_canvas_route_midpoint(&route, &probes[probe_count].connector.via_x, &probes[probe_count].connector.via_y);
    probes[probe_count].connector.via_count = 1;
    probe_count++;
  }
  if(straight && without_via && probe_count == 2)
  {
    // The route made cubic, then given a waypoint halfway along it...
    probes[probe_count] = probes[0];
    if(dt_canvas_connector_route(canvas, &probes[0], &route))
    {
      dt_canvas_route_midpoint(&route, &probes[probe_count].connector.via_x, &probes[probe_count].connector.via_y);
      probes[probe_count].connector.via_count = 1;
      probe_count++;
    }
    // ...and given the waypoint first, then made cubic.
    probes[probe_count] = probes[1];
    probes[probe_count].connector.routing = DT_CANVAS_ROUTING_CUBIC;
    probe_count++;
  }
  for(int probe = 0; probe < probe_count; probe++)
    _object_sites_add(shapes, view, canvas, &probes[probe], DT_CANVAS_HANDLES_CONNECTOR, DT_CANVAS_PLACE_PREDICTED);
  dt_free(probes);
}

void dt_canvas_place_object_shapes(GArray *shapes, const dt_canvas_place_view_t *view, const dt_canvas_t *canvas,
                                   const dt_canvas_object_t *object, const gboolean mask_editing)
{
  if(IS_NULL_PTR(shapes) || IS_NULL_PTR(view) || IS_NULL_PTR(object) || !(view->zoom > 0.0)) return;
  if(dt_canvas_object_is_frame(object))
  {
    _object_sites_add(shapes, view, canvas, object, DT_CANVAS_HANDLES_FRAME, DT_CANVAS_PLACE_HARD);
    // A cutout's handles are there to grab while it is edited, and one click on Edit away when it
    // is not. A frame without a cutout needs nothing: its default shapes lie inside it, within the
    // body's own tolerance.
    if(object->mask.shape != DT_CANVAS_MASK_NONE)
      _object_sites_add(shapes, view, canvas, object, DT_CANVAS_HANDLES_MASK,
                        mask_editing ? DT_CANVAS_PLACE_HARD : DT_CANVAS_PLACE_PREDICTED);
    dt_canvas_place_body_add(shapes, view, object);
    return;
  }
  if(object->kind != DT_CANVAS_OBJECT_CONNECTOR || IS_NULL_PTR(canvas)) return;
  _object_sites_add(shapes, view, canvas, object, DT_CANVAS_HANDLES_CONNECTOR | DT_CANVAS_HANDLES_CURVE,
                    DT_CANVAS_PLACE_HARD);
  _connector_predicted_add(shapes, view, canvas, object);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
