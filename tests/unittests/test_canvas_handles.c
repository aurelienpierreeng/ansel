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

/* The handle sites the atelier's hit tests read: where each one is, how far it catches -- against
 * the numbers the view caught with before the list existed, written out here rather than read
 * back -- in what order, and that the band a connector is picked in and the ink an arrowhead lays
 * down are the pick's and the painter's own numbers, not a second guess at them. */

#include "canvas/canvas.h"
#include "canvas/canvas_handles.h"
#include "canvas/canvas_paint.h"
#include "system/mem_alloc.h"

#include <cairo.h>
#include <glib.h>
#include <math.h>
// cmocka.h declares `extern jmp_buf global_expect_assert_env' without including <setjmp.h>.
#include <setjmp.h>  // NOLINT(misc-include-cleaner)
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <cmocka.h>

/** A reproducible sequence: the same scenes on every run and every machine. */
static uint64_t _random_state = 0x9E3779B97F4A7C15ull;

static double _random_unit(void)
{
  _random_state ^= _random_state << 13;
  _random_state ^= _random_state >> 7;
  _random_state ^= _random_state << 17;
  return (double)(_random_state >> 11) / (double)(1ull << 53);
}

static double _random_range(const double low, const double high)
{
  return low + (high - low) * _random_unit();
}

/** A zoom spread evenly in magnitude from 0.05 to 8, the atelier's working range and a little past it. */
static double _random_zoom(void)
{
  return exp(_random_range(log(0.05), log(8.0)));
}

static size_t _sites(const dt_canvas_t *canvas, const dt_canvas_object_t *object, const uint32_t what,
                     dt_canvas_handle_site_t **sites)
{
  const size_t count = dt_canvas_handle_sites(canvas, object, what, NULL, 0);
  *sites = count > 0 ? g_new0(dt_canvas_handle_site_t, count) : NULL;
  const size_t written = dt_canvas_handle_sites(canvas, object, what, *sites, count);
  assert_int_equal(written, count);
  return count;
}

static size_t _count_role(const dt_canvas_handle_site_t *sites, const size_t count, const uint32_t role)
{
  size_t found = 0;
  for(size_t idx = 0; idx < count; idx++)
    if(sites[idx].role == role) found++;
  return found;
}

/** The i-th site of a role, in list order. */
static const dt_canvas_handle_site_t *_nth_role(const dt_canvas_handle_site_t *sites, const size_t count,
                                                const uint32_t role, const size_t nth)
{
  size_t found = 0;
  for(size_t idx = 0; idx < count; idx++)
  {
    if(sites[idx].role != role) continue;
    if(found == nth) return &sites[idx];
    found++;
  }
  return NULL;
}

/** A polygon of `count` nodes on an ellipse, of every node kind, some with a fall-off of their own. */
static void _random_polygon(dt_canvas_t *canvas, dt_canvas_object_t *frame, const uint32_t count)
{
  float *nodes = g_new0(float, (size_t)count * DT_CANVAS_MASK_NODE_FLOATS);
  for(uint32_t idx = 0; idx < count; idx++)
  {
    float *node = nodes + (size_t)idx * DT_CANVAS_MASK_NODE_FLOATS;
    const double angle = 2.0 * M_PI * idx / count + _random_range(-0.2, 0.2);
    const float x = (float)(0.5 + 0.4 * cos(angle));
    const float y = (float)(0.5 + 0.4 * sin(angle));
    node[DT_CANVAS_MASK_NODE_X] = x;
    node[DT_CANVAS_MASK_NODE_Y] = y;
    node[DT_CANVAS_MASK_NODE_CTRL1_X] = x + (float)_random_range(-0.1, 0.1);
    node[DT_CANVAS_MASK_NODE_CTRL1_Y] = y + (float)_random_range(-0.1, 0.1);
    node[DT_CANVAS_MASK_NODE_CTRL2_X] = x + (float)_random_range(-0.1, 0.1);
    node[DT_CANVAS_MASK_NODE_CTRL2_Y] = y + (float)_random_range(-0.1, 0.1);
    node[DT_CANVAS_MASK_NODE_SMOOTH] = (float)(idx % 3);
    const float own = _random_unit() < 0.5 ? 0.0f : (float)_random_range(0.01, 0.3);
    node[DT_CANVAS_MASK_NODE_BORDER1] = own;
    node[DT_CANVAS_MASK_NODE_BORDER2] = own;
  }
  dt_canvas_mask_set_shape(canvas, frame, DT_CANVAS_MASK_POLYGON);
  dt_canvas_mask_set_nodes(canvas, frame, nodes, count);
  dt_free(nodes);
}

/**
 * Four cusp nodes at the quarter points of the unit square, their control points on them: a
 * polygon whose every handle position can be worked out on paper.
 */
static void _square_cusp_nodes(float nodes[4 * DT_CANVAS_MASK_NODE_FLOATS])
{
  const float corners[8] = { 0.25f, 0.25f, 0.75f, 0.25f, 0.75f, 0.75f, 0.25f, 0.75f };
  for(int idx = 0; idx < 4; idx++)
  {
    float *node = nodes + idx * DT_CANVAS_MASK_NODE_FLOATS;
    node[DT_CANVAS_MASK_NODE_X] = corners[2 * idx];
    node[DT_CANVAS_MASK_NODE_Y] = corners[2 * idx + 1];
    node[DT_CANVAS_MASK_NODE_CTRL1_X] = corners[2 * idx];
    node[DT_CANVAS_MASK_NODE_CTRL1_Y] = corners[2 * idx + 1];
    node[DT_CANVAS_MASK_NODE_CTRL2_X] = corners[2 * idx];
    node[DT_CANVAS_MASK_NODE_CTRL2_Y] = corners[2 * idx + 1];
    node[DT_CANVAS_MASK_NODE_SMOOTH] = (float)DT_CANVAS_MASK_NODE_CUSP;
  }
}

/**
 * Two frames of random size, turn and lock, the first one cut, joined by a random connector; and a
 * random line, free at both ends or anchored to the second frame at its end, sometimes locked.
 */
typedef struct handles_scene_t
{
  dt_canvas_t *canvas;
  dt_canvas_object_t *first;
  dt_canvas_object_t *second;
  dt_canvas_object_t *connector;
  dt_canvas_object_t *line;
} handles_scene_t;

static void _random_frame(dt_canvas_object_t *frame)
{
  frame->x = _random_range(-2000.0, 2000.0);
  frame->y = _random_range(-2000.0, 2000.0);
  frame->width = _random_range(20.0, 1500.0);
  frame->height = _random_range(20.0, 1500.0);
  frame->rotation = _random_range(-M_PI, M_PI);
  if(_random_unit() < 0.25) frame->flags |= DT_CANVAS_OBJECT_FLAG_LOCKED;
}

static void _scene_build(handles_scene_t *scene)
{
  scene->canvas = dt_canvas_new();
  scene->first = dt_canvas_add_image(scene->canvas, 0.0, 0.0, 6000, 4000);
  scene->second = dt_canvas_add_image(scene->canvas, 0.0, 0.0, 4000, 6000);
  _random_frame(scene->first);
  _random_frame(scene->second);
  const uint32_t shape = (uint32_t)(_random_unit() * 5.0);
  if(shape == DT_CANVAS_MASK_POLYGON)
    _random_polygon(scene->canvas, scene->first, 3 + (uint32_t)(_random_unit() * 8.0));
  else
    dt_canvas_mask_set_shape(scene->canvas, scene->first, shape);
  scene->first->mask.center_x = (float)_random_range(0.2, 0.8);
  scene->first->mask.center_y = (float)_random_range(0.2, 0.8);
  scene->first->mask.rotation = (float)_random_range(-180.0, 180.0);
  scene->connector = dt_canvas_add_connector(scene->canvas, scene->first->id, scene->second->id);
  scene->connector->connector.routing = (uint32_t)(_random_unit() * 3.0);
  scene->connector->connector.line_width = (float)_random_range(0.0, 16.0);
  scene->connector->connector.style = (uint32_t)(_random_unit() * 4.0);
  scene->connector->connector.from_anchor = (uint32_t)(_random_unit() * DT_CANVAS_ANCHOR_LAST);
  scene->connector->connector.to_anchor = (uint32_t)(_random_unit() * DT_CANVAS_ANCHOR_LAST);
  if(_random_unit() < 0.5)
  {
    dt_canvas_connector_add_via(scene->canvas, scene->connector);
    scene->connector->connector.via_x += _random_range(-300.0, 300.0);
    scene->connector->connector.via_y += _random_range(-300.0, 300.0);
  }
  const dt_canvas_routing_t line_routing = (dt_canvas_routing_t)(uint32_t)(_random_unit() * 3.0);
  scene->line = dt_canvas_add_line(scene->canvas, _random_range(-2000.0, 2000.0), _random_range(-2000.0, 2000.0),
                                   _random_range(-2000.0, 2000.0), _random_range(-2000.0, 2000.0), line_routing,
                                   NULL);
  scene->line->connector.line_width = (float)_random_range(0.0, 16.0);
  scene->line->connector.style = (uint32_t)(_random_unit() * 4.0);
  if(_random_unit() < 0.25)
  {
    scene->line->connector.to_id = scene->second->id;
    scene->line->connector.to_anchor = (uint32_t)(_random_unit() * DT_CANVAS_ANCHOR_LAST);
  }
  if(_random_unit() < 0.5)
  {
    dt_canvas_connector_add_via(scene->canvas, scene->line);
    scene->line->connector.via_x += _random_range(-300.0, 300.0);
    scene->line->connector.via_y += _random_range(-300.0, 300.0);
  }
  if(_random_unit() < 0.25) scene->line->flags |= DT_CANVAS_OBJECT_FLAG_LOCKED;
}

/** A point `distance` from (x, y) along the direction `angle`. */
static void _step(const double x, const double y, const double angle, const double distance, double *out_x,
                  double *out_y)
{
  *out_x = x + cos(angle) * distance;
  *out_y = y + sin(angle) * distance;
}

/** The painter's arrowhead, spelled out here: a stored width of zero is drawn two units wide. */
static double _expected_arrow_reach(const float line_width)
{
  const double painted_width = line_width > 0.0f ? line_width : 2.0;
  return painted_width + 14.0 * fmax(painted_width / 2.0, 1.0);
}

/** What a site must be. */
typedef struct handles_expected_t
{
  uint32_t shape;
  double reach_px;
  double reach_units;
  gboolean turned;    ///< the angle decides something: a square's axes, or where a pixel offset points
  double angle;
  double offset_px_y;
} handles_expected_t;

/**
 * Each role's shape, reach and turn, written out as the numbers the atelier's hit tests caught
 * with before there was a list of sites -- eight screen pixels for a corner, the knob, a node, a
 * cutout handle and an edge, ten for a tangent and the waypoint, four around a line plus its
 * width -- and NOT read back from the enumerator or its constants, so a reach one pixel off, a
 * square turned into a disc or a turn dropped shows here.
 */
static handles_expected_t _expected_site(const dt_canvas_object_t *owner, const dt_canvas_handle_site_t *site)
{
  handles_expected_t expected = { DT_CANVAS_HANDLE_DISC, 0.0, 0.0, FALSE, 0.0, 0.0 };
  switch(site->role)
  {
    case DT_CANVAS_HANDLE_CORNER:
    case DT_CANVAS_HANDLE_MASK_NODE:
      expected.shape = DT_CANVAS_HANDLE_SQUARE;
      expected.reach_px = 8.0;
      expected.turned = TRUE;
      expected.angle = owner->rotation;
      break;
    case DT_CANVAS_HANDLE_ROTATE:
      expected.shape = DT_CANVAS_HANDLE_SQUARE;
      expected.reach_px = 8.0;
      expected.turned = TRUE;
      expected.angle = owner->rotation;
      expected.offset_px_y = -28.0;
      break;
    case DT_CANVAS_HANDLE_ROTATE_STEM:
      expected.shape = DT_CANVAS_HANDLE_SEGMENT;
      expected.reach_px = 2.0;
      expected.turned = TRUE;
      expected.angle = owner->rotation;
      expected.offset_px_y = -28.0;
      break;
    case DT_CANVAS_HANDLE_MASK_POINT:
    case DT_CANVAS_HANDLE_MASK_NODE_OWN:
      expected.shape = DT_CANVAS_HANDLE_DISC;
      expected.reach_px = 8.0;
      break;
    case DT_CANVAS_HANDLE_MASK_EDGE:
      expected.shape = DT_CANVAS_HANDLE_SEGMENT;
      expected.reach_px = 8.0;
      break;
    case DT_CANVAS_HANDLE_TANGENT:
      expected.shape = DT_CANVAS_HANDLE_DISC;
      expected.reach_px = 10.0;
      break;
    case DT_CANVAS_HANDLE_TETHER:
      expected.shape = DT_CANVAS_HANDLE_SEGMENT;
      expected.reach_px = 2.0;
      break;
    case DT_CANVAS_HANDLE_VIA:
      // A square that does not turn: the waypoint is caught along the canvas's own axes.
      expected.shape = DT_CANVAS_HANDLE_SQUARE;
      expected.reach_px = 10.0;
      expected.turned = TRUE;
      expected.angle = 0.0;
      break;
    case DT_CANVAS_HANDLE_CURVE:
      expected.shape = DT_CANVAS_HANDLE_SEGMENT;
      expected.reach_px = 4.0;
      expected.reach_units = owner->connector.line_width;
      break;
    case DT_CANVAS_HANDLE_ARROW:
      expected.shape = DT_CANVAS_HANDLE_DISC;
      expected.reach_units = _expected_arrow_reach(owner->connector.line_width);
      break;
    case DT_CANVAS_HANDLE_ENDPOINT:
      // The waypoint's square, the same ten pixels along the canvas's own axes.
      expected.shape = DT_CANVAS_HANDLE_SQUARE;
      expected.reach_px = 10.0;
      expected.turned = TRUE;
      expected.angle = 0.0;
      break;
    default:
      fail_msg("unknown role %u", site->role);
  }
  return expected;
}

/** A site's fields against what its role must be, then the pointer against its reach at `zoom`. */
static void _assert_site_catches_as_expected(const dt_canvas_object_t *owner, const dt_canvas_handle_site_t *site,
                                             const double zoom)
{
  const handles_expected_t expected = _expected_site(owner, site);
  assert_int_equal(site->shape, expected.shape);
  assert_true(site->reach_px == expected.reach_px);
  assert_double_equal(site->reach_units, expected.reach_units, 1e-12);
  assert_true(site->offset_px_x == 0.0);
  assert_true(site->offset_px_y == expected.offset_px_y);
  if(expected.turned) assert_true(site->angle == expected.angle);

  const double half_pixel = 0.5 / zoom;
  const double reach = expected.reach_px / zoom + expected.reach_units;
  double x0 = 0.0;
  double y0 = 0.0;
  double x1 = 0.0;
  double y1 = 0.0;
  double resolved = 0.0;
  dt_canvas_handle_site_resolve(site, zoom, &x0, &y0, &x1, &y1, &resolved);
  assert_double_equal(resolved, reach, 1e-9 * fmax(reach, 1.0));
  double probe_x = 0.0;
  double probe_y = 0.0;
  if(expected.shape == DT_CANVAS_HANDLE_SQUARE)
  {
    assert_true(dt_canvas_handle_site_hit(site, x0, y0, zoom));
    for(int side = 0; side < 4; side++)
    {
      const double axis = expected.angle + side * M_PI_2;
      _step(x0, y0, axis, reach + half_pixel, &probe_x, &probe_y);
      assert_false(dt_canvas_handle_site_hit(site, probe_x, probe_y, zoom));
      _step(x0, y0, axis, reach - half_pixel, &probe_x, &probe_y);
      assert_true(dt_canvas_handle_site_hit(site, probe_x, probe_y, zoom));
    }
    // A square is a square: its corner catches where a disc of the same reach would not.
    _step(x0, y0, expected.angle + M_PI_4, 0.9 * reach * M_SQRT2, &probe_x, &probe_y);
    assert_true(dt_canvas_handle_site_hit(site, probe_x, probe_y, zoom));
  }
  else if(expected.shape == DT_CANVAS_HANDLE_DISC)
  {
    assert_true(dt_canvas_handle_site_hit(site, x0, y0, zoom));
    for(int side = 0; side < 8; side++)
    {
      const double direction = side * M_PI_4;
      _step(x0, y0, direction, reach + half_pixel, &probe_x, &probe_y);
      assert_false(dt_canvas_handle_site_hit(site, probe_x, probe_y, zoom));
      if(reach > 2.0 * half_pixel)
      {
        _step(x0, y0, direction, reach - half_pixel, &probe_x, &probe_y);
        assert_true(dt_canvas_handle_site_hit(site, probe_x, probe_y, zoom));
      }
    }
    // And a disc is a disc: the corner of the square around it lets go.
    if(reach > 6.0 * half_pixel)
      assert_false(dt_canvas_handle_site_hit(site, x0 + reach - half_pixel, y0 + reach - half_pixel, zoom));
  }
  else
  {
    const double middle_x = 0.5 * (x0 + x1);
    const double middle_y = 0.5 * (y0 + y1);
    assert_true(dt_canvas_handle_site_hit(site, middle_x, middle_y, zoom));
    const double length = hypot(x1 - x0, y1 - y0);
    if(!(length > 4.0 * half_pixel)) return;
    const double along = atan2(y1 - y0, x1 - x0);
    for(int side = -1; side <= 1; side += 2)
    {
      _step(middle_x, middle_y, along + side * M_PI_2, reach + half_pixel, &probe_x, &probe_y);
      assert_false(dt_canvas_handle_site_hit(site, probe_x, probe_y, zoom));
      _step(middle_x, middle_y, along + side * M_PI_2, reach - half_pixel, &probe_x, &probe_y);
      assert_true(dt_canvas_handle_site_hit(site, probe_x, probe_y, zoom));
    }
    _step(x1, y1, along, reach + half_pixel, &probe_x, &probe_y);
    assert_false(dt_canvas_handle_site_hit(site, probe_x, probe_y, zoom));
  }
}

/**
 * Every site of every object has the shape, reach and turn its role has always caught with, and
 * catches half a screen pixel inside that reach and lets go half a pixel past it, in every
 * direction that measures it: a square along its two turned axes, a disc all round, a segment
 * across its middle and past its far end. At a quarter, at four times and at a random zoom.
 */
static void _every_site_catches_its_centre_and_lets_go_past_its_reach(void **state)
{
  (void)state;
  size_t checked_sites = 0;
  size_t roles_seen[DT_CANVAS_HANDLE_ROLE_COUNT] = { 0 };
  for(int scene_idx = 0; scene_idx < 300; scene_idx++)
  {
    handles_scene_t scene;
    _scene_build(&scene);
    const double zooms[3] = { 0.25, 4.0, _random_zoom() };
    const dt_canvas_object_t *objects[4] = { scene.first, scene.second, scene.connector, scene.line };
    for(int object_idx = 0; object_idx < 4; object_idx++)
    {
      dt_canvas_handle_site_t *sites = NULL;
      const size_t count = _sites(scene.canvas, objects[object_idx], DT_CANVAS_HANDLES_ALL, &sites);
      // A free end is a handle unless its line is locked; an anchored end never is, frames' included.
      const dt_canvas_object_t *owner = objects[object_idx];
      size_t free_ends = 0;
      if(owner->kind == DT_CANVAS_OBJECT_CONNECTOR && !(owner->flags & DT_CANVAS_OBJECT_FLAG_LOCKED))
        free_ends = (owner->connector.from_id == 0 ? 1u : 0u) + (owner->connector.to_id == 0 ? 1u : 0u);
      assert_int_equal(_count_role(sites, count, DT_CANVAS_HANDLE_ENDPOINT), free_ends);
      for(size_t idx = 0; idx < count; idx++)
      {
        roles_seen[sites[idx].role]++;
        checked_sites++;
        for(int zoom_idx = 0; zoom_idx < 3; zoom_idx++)
          _assert_site_catches_as_expected(objects[object_idx], &sites[idx], zooms[zoom_idx]);
      }
      dt_free(sites);
    }
    dt_canvas_free(scene.canvas);
  }
  // The scenes must actually have exercised every kind of site, or the loop above proves nothing.
  for(uint32_t role = DT_CANVAS_HANDLE_CORNER; role < DT_CANVAS_HANDLE_ROLE_COUNT; role++)
    assert_true(roles_seen[role] > 0);
  assert_true(checked_sites > 5000);
}

/**
 * A handle whose position is not a number catches nothing, wherever the pointer is: a file reads
 * the cutout's floats as they stand, and a hit test asked the other way round would hand every
 * press to such a handle.
 */
static void _a_handle_that_is_not_a_number_catches_nothing(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *frame = dt_canvas_add_image(canvas, 1000.0, 500.0, 2000, 2000);
  frame->width = 400.0;
  frame->height = 400.0;
  frame->rotation = 0.0;
  float nodes[4 * DT_CANVAS_MASK_NODE_FLOATS] = { 0.0f };
  _square_cusp_nodes(nodes);
  nodes[DT_CANVAS_MASK_NODE_CTRL1_X] = NAN;
  dt_canvas_mask_set_shape(canvas, frame, DT_CANVAS_MASK_POLYGON);
  dt_canvas_mask_set_nodes(canvas, frame, nodes, 4);
  dt_canvas_handle_site_t *sites = NULL;
  const size_t count = _sites(canvas, frame, DT_CANVAS_HANDLES_MASK_NODE_OWN, &sites);
  const dt_canvas_handle_site_t *incoming = _nth_role(sites, count, DT_CANVAS_HANDLE_MASK_NODE_OWN, 1);
  assert_non_null(incoming);
  assert_int_equal(incoming->part, DT_CANVAS_HANDLE_PART_INCOMING);
  assert_true(isnan(incoming->x0));
  const double probes[8] = { 900.0, 400.0, 1000.0, 500.0, 0.0, 0.0, -1e6, 1e6 };
  for(int idx = 0; idx < 4; idx++)
    assert_false(dt_canvas_handle_site_hit(incoming, probes[2 * idx], probes[2 * idx + 1], 1.0));
  dt_free(sites);
  dt_canvas_free(canvas);
}

/** A frame's local point in canvas units, turned by the frame: spelled out here, not borrowed. */
static void _turn(const dt_canvas_object_t *frame, const double local_x, const double local_y, double *x, double *y)
{
  *x = frame->x + local_x * cos(frame->rotation) - local_y * sin(frame->rotation);
  *y = frame->y + local_x * sin(frame->rotation) + local_y * cos(frame->rotation);
}

/**
 * The knob hangs a fixed SCREEN distance above the middle of the top edge, along the frame's own
 * up, so it moves closer to the frame in canvas units as the zoom grows -- and it turns with the
 * frame. The corners are listed in dt_canvas_object_corners()'s order, which is the index the
 * scale gesture reads, and all four come before the knob: the hit test takes the first that
 * catches, so a corner under the knob of a small frame is still the corner.
 */
static void _the_knob_floats_a_screen_distance_above_a_turned_frame(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *frame = dt_canvas_add_image(canvas, 100.0, 50.0, 2000, 1000);
  frame->width = 200.0;
  frame->height = 100.0;
  frame->rotation = 30.0 * M_PI / 180.0;
  dt_canvas_handle_site_t sites[16];
  const size_t count = dt_canvas_handle_sites(canvas, frame, DT_CANVAS_HANDLES_FRAME, sites, G_N_ELEMENTS(sites));
  assert_int_equal(count, 6);
  double corners[8];
  dt_canvas_object_corners(frame, corners);
  for(int idx = 0; idx < 4; idx++)
  {
    assert_int_equal(sites[idx].role, DT_CANVAS_HANDLE_CORNER);
    assert_int_equal(sites[idx].index, idx);
    assert_double_equal(sites[idx].x0, corners[2 * idx], 1e-9);
    assert_double_equal(sites[idx].y0, corners[2 * idx + 1], 1e-9);
  }
  assert_int_equal(sites[4].role, DT_CANVAS_HANDLE_ROTATE);
  assert_int_equal(sites[5].role, DT_CANVAS_HANDLE_ROTATE_STEM);
  const double zooms[3] = { 0.25, 1.0, 4.0 };
  for(int zoom_idx = 0; zoom_idx < 3; zoom_idx++)
  {
    const double zoom = zooms[zoom_idx];
    const double lift = frame->height * 0.5 + 28.0 / zoom;
    // (0, -lift) in the frame's local units, turned clockwise by 30 degrees.
    const double expected_x = 100.0 + lift * sin(frame->rotation);
    const double expected_y = 50.0 - lift * cos(frame->rotation);
    const dt_canvas_handle_site_t *knob = &sites[4];
    const dt_canvas_handle_site_t *stem = &sites[5];
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = 0.0;
    double y1 = 0.0;
    double reach = 0.0;
    dt_canvas_handle_site_resolve(knob, zoom, &x0, &y0, &x1, &y1, &reach);
    assert_double_equal(x0, expected_x, 1e-9);
    assert_double_equal(y0, expected_y, 1e-9);
    assert_double_equal(reach, 8.0 / zoom, 1e-12);
    dt_canvas_handle_site_resolve(stem, zoom, &x0, &y0, &x1, &y1, &reach);
    assert_double_equal(x0, 100.0 + 50.0 * sin(frame->rotation), 1e-9);
    assert_double_equal(y0, 50.0 - 50.0 * cos(frame->rotation), 1e-9);
    assert_double_equal(x1, expected_x, 1e-9);
    assert_double_equal(y1, expected_y, 1e-9);
  }
  dt_canvas_free(canvas);
}

/** A locked frame cannot be scaled or turned, so it offers neither -- but its cutout is still its own. */
static void _a_locked_frame_offers_no_frame_handles_but_keeps_its_cutout(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *frame = dt_canvas_add_image(canvas, 0.0, 0.0, 3000, 2000);
  dt_canvas_mask_set_shape(canvas, frame, DT_CANVAS_MASK_CIRCLE);
  assert_int_equal(dt_canvas_handle_sites(canvas, frame, DT_CANVAS_HANDLES_FRAME, NULL, 0), 6);
  frame->flags |= DT_CANVAS_OBJECT_FLAG_LOCKED;
  assert_int_equal(dt_canvas_handle_sites(canvas, frame, DT_CANVAS_HANDLES_FRAME, NULL, 0), 0);
  assert_int_equal(dt_canvas_handle_sites(canvas, frame, DT_CANVAS_HANDLES_MASK, NULL, 0), 4);
  dt_canvas_free(canvas);
}

/**
 * Asked with room for only `room` sites, the enumerator answers the true count, writes the first
 * `room` exactly as the full list has them, and leaves every byte after them alone -- for every
 * room from none to the whole list, so each of the fields written after a site is added is
 * caught writing where it has no room.
 */
static void _assert_truncates_cleanly(const dt_canvas_t *canvas, const dt_canvas_object_t *object, const uint32_t what)
{
  dt_canvas_handle_site_t *full = NULL;
  const size_t count = _sites(canvas, object, what, &full);
  assert_true(count > 0);
  dt_canvas_handle_site_t *partial = g_new(dt_canvas_handle_site_t, count);
  for(size_t room = 0; room <= count; room++)
  {
    memset(partial, 0xA5, sizeof(dt_canvas_handle_site_t) * count);
    assert_int_equal(dt_canvas_handle_sites(canvas, object, what, partial, room), count);
    for(size_t idx = 0; idx < room; idx++)
      assert_memory_equal(&partial[idx], &full[idx], sizeof(dt_canvas_handle_site_t));
    const unsigned char *tail = (const unsigned char *)(partial + room);
    const size_t tail_bytes = sizeof(dt_canvas_handle_site_t) * (count - room);
    for(size_t byte = 0; byte < tail_bytes; byte++)
      assert_int_equal(tail[byte], 0xA5);
  }
  dt_free(partial);
  dt_free(full);
}

/**
 * A cubic connector offers one tangent handle per control point, each tied back to the end it
 * steers and followed by its tether: two on a plain curve, four through a waypoint, where the
 * middle two turn the waypoint's one tangent from either side. The waypoint is a site of its own
 * whatever the routing, listed after the tangents; a straight or square route has no tangents.
 */
static void _a_cubic_connector_offers_a_tangent_per_control_point(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *left = dt_canvas_add_image(canvas, 0.0, 0.0, 1000, 1000);
  dt_canvas_object_t *right = dt_canvas_add_image(canvas, 900.0, 300.0, 1000, 1000);
  dt_canvas_object_t *connector = dt_canvas_add_connector(canvas, left->id, right->id);
  connector->connector.routing = DT_CANVAS_ROUTING_CUBIC;

  dt_canvas_handle_site_t *sites = NULL;
  size_t count = _sites(canvas, connector, DT_CANVAS_HANDLES_CONNECTOR, &sites);
  dt_canvas_route_t route;
  assert_true(dt_canvas_connector_route(canvas, connector, &route));
  assert_int_equal(route.segment_count, 1);
  assert_int_equal(_count_role(sites, count, DT_CANVAS_HANDLE_TANGENT), 2);
  assert_int_equal(_count_role(sites, count, DT_CANVAS_HANDLE_TETHER), 2);
  assert_int_equal(_count_role(sites, count, DT_CANVAS_HANDLE_VIA), 0);
  const dt_canvas_handle_site_t *start = _nth_role(sites, count, DT_CANVAS_HANDLE_TANGENT, 0);
  const dt_canvas_handle_site_t *end = _nth_role(sites, count, DT_CANVAS_HANDLE_TANGENT, 1);
  assert_int_equal(start->part, DT_CANVAS_HANDLE_PART_FROM);
  assert_int_equal(end->part, DT_CANVAS_HANDLE_PART_TO);
  assert_double_equal(start->x0, route.control1_x, 1e-12);
  assert_double_equal(start->y0, route.control1_y, 1e-12);
  assert_double_equal(end->x0, route.control2_x, 1e-12);
  assert_double_equal(end->y0, route.control2_y, 1e-12);
  const dt_canvas_handle_site_t *tether = _nth_role(sites, count, DT_CANVAS_HANDLE_TETHER, 1);
  assert_double_equal(tether->x0, route.to_x, 1e-12);
  assert_double_equal(tether->y0, route.to_y, 1e-12);
  assert_double_equal(tether->x1, route.control2_x, 1e-12);
  assert_double_equal(tether->y1, route.control2_y, 1e-12);
  dt_free(sites);

  dt_canvas_connector_add_via(canvas, connector);
  connector->connector.via_y -= 200.0;
  count = _sites(canvas, connector, DT_CANVAS_HANDLES_CONNECTOR, &sites);
  assert_true(dt_canvas_connector_route(canvas, connector, &route));
  assert_int_equal(route.segment_count, 2);
  assert_int_equal(count, 9);
  for(size_t idx = 0; idx < 8; idx++)
    assert_int_equal(sites[idx].role, idx % 2 == 0 ? DT_CANVAS_HANDLE_TANGENT : DT_CANVAS_HANDLE_TETHER);
  assert_int_equal(sites[8].role, DT_CANVAS_HANDLE_VIA);
  const double controls[8] = { route.control1_x, route.control1_y, route.control2_x, route.control2_y,
                               route.control3_x, route.control3_y, route.control4_x, route.control4_y };
  const uint32_t parts[4] = { DT_CANVAS_HANDLE_PART_FROM, DT_CANVAS_HANDLE_PART_VIA, DT_CANVAS_HANDLE_PART_VIA,
                              DT_CANVAS_HANDLE_PART_TO };
  const int signs[4] = { 0, -1, 1, 0 };
  const double probe_zooms[2] = { 0.5, 3.0 };
  for(int idx = 0; idx < 4; idx++)
  {
    const dt_canvas_handle_site_t *tangent = _nth_role(sites, count, DT_CANVAS_HANDLE_TANGENT, (size_t)idx);
    assert_int_equal(tangent->index, idx + 1);
    assert_int_equal(tangent->part, parts[idx]);
    assert_int_equal(tangent->sign, signs[idx]);
    assert_double_equal(tangent->x0, controls[2 * idx], 1e-12);
    assert_double_equal(tangent->y0, controls[2 * idx + 1], 1e-12);
    // Ten screen pixels, whatever the zoom: caught at nine and a half, let go at ten and a half.
    for(int zoom_idx = 0; zoom_idx < 2; zoom_idx++)
    {
      const double zoom = probe_zooms[zoom_idx];
      assert_true(dt_canvas_handle_site_hit(tangent, controls[2 * idx] + 9.5 / zoom, controls[2 * idx + 1], zoom));
      assert_false(dt_canvas_handle_site_hit(tangent, controls[2 * idx] + 10.5 / zoom, controls[2 * idx + 1], zoom));
      assert_false(dt_canvas_handle_site_hit(tangent, controls[2 * idx] + 9.5 / zoom,
                                             controls[2 * idx + 1] + 9.5 / zoom, zoom));
    }
  }
  const dt_canvas_handle_site_t *via = &sites[8];
  assert_double_equal(via->x0, connector->connector.via_x, 1e-12);
  assert_double_equal(via->y0, connector->connector.via_y, 1e-12);
  for(int zoom_idx = 0; zoom_idx < 2; zoom_idx++)
  {
    // The waypoint is a square of the same ten pixels, along the canvas's own axes.
    const double zoom = probe_zooms[zoom_idx];
    assert_true(dt_canvas_handle_site_hit(via, via->x0 + 9.5 / zoom, via->y0 - 9.5 / zoom, zoom));
    assert_false(dt_canvas_handle_site_hit(via, via->x0 + 10.5 / zoom, via->y0, zoom));
    assert_false(dt_canvas_handle_site_hit(via, via->x0, via->y0 - 10.5 / zoom, zoom));
  }
  dt_free(sites);
  connector->connector.style = DT_CANVAS_CONNECTOR_ARROW_START | DT_CANVAS_CONNECTOR_ARROW_END;
  _assert_truncates_cleanly(canvas, connector, DT_CANVAS_HANDLES_ALL);

  connector->connector.routing = DT_CANVAS_ROUTING_STRAIGHT;
  count = _sites(canvas, connector, DT_CANVAS_HANDLES_CONNECTOR, &sites);
  assert_int_equal(_count_role(sites, count, DT_CANVAS_HANDLE_TANGENT), 0);
  assert_int_equal(_count_role(sites, count, DT_CANVAS_HANDLE_TETHER), 0);
  assert_int_equal(_count_role(sites, count, DT_CANVAS_HANDLE_VIA), 1);
  dt_free(sites);
  dt_canvas_free(canvas);
}

/**
 * A line's free ends are handles, where its route starts and ends, the start's before the end's
 * and both before anything else the line offers: a press near an end and its control point takes
 * the end. An end anchored to a frame is the frame's to move and offers nothing, so a half-free
 * connector offers one and a connector between two frames none -- and a locked line keeps both of
 * its ends, as a locked frame keeps its corners.
 */
static void _a_line_offers_its_free_ends_and_nothing_at_an_anchored_one(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *line = dt_canvas_add_line(canvas, 100.0, 50.0, 400.0, 250.0, DT_CANVAS_ROUTING_CUBIC, NULL);
  dt_canvas_route_t route;
  assert_true(dt_canvas_connector_route(canvas, line, &route));
  dt_canvas_handle_site_t *sites = NULL;
  size_t count = _sites(canvas, line, DT_CANVAS_HANDLES_ALL, &sites);
  assert_true(count > 6);
  const uint32_t leading_roles[6] = { DT_CANVAS_HANDLE_ENDPOINT, DT_CANVAS_HANDLE_ENDPOINT, DT_CANVAS_HANDLE_TANGENT,
                                      DT_CANVAS_HANDLE_TETHER,   DT_CANVAS_HANDLE_TANGENT,  DT_CANVAS_HANDLE_TETHER };
  for(int idx = 0; idx < 6; idx++)
    assert_int_equal(sites[idx].role, leading_roles[idx]);
  assert_int_equal(_count_role(sites, count, DT_CANVAS_HANDLE_ENDPOINT), 2);
  assert_int_equal(sites[0].part, DT_CANVAS_HANDLE_PART_FROM);
  assert_int_equal(sites[0].index, 0);
  assert_int_equal(sites[1].part, DT_CANVAS_HANDLE_PART_TO);
  assert_int_equal(sites[1].index, 1);
  assert_true(sites[0].x0 == route.from_x && sites[0].y0 == route.from_y);
  assert_true(sites[1].x0 == route.to_x && sites[1].y0 == route.to_y);
  assert_true(route.from_x == 100.0 && route.from_y == 50.0);
  assert_true(route.to_x == 400.0 && route.to_y == 250.0);
  const double probe_zooms[2] = { 0.5, 3.0 };
  for(int end = 0; end < 2; end++)
  {
    const dt_canvas_handle_site_t *site = &sites[end];
    for(int zoom_idx = 0; zoom_idx < 2; zoom_idx++)
    {
      // Ten screen pixels either way along the canvas's axes, corners included: a square.
      const double zoom = probe_zooms[zoom_idx];
      assert_true(dt_canvas_handle_site_hit(site, site->x0 + 9.5 / zoom, site->y0 - 9.5 / zoom, zoom));
      assert_true(dt_canvas_handle_site_hit(site, site->x0 - 9.5 / zoom, site->y0 + 9.5 / zoom, zoom));
      assert_false(dt_canvas_handle_site_hit(site, site->x0 + 10.5 / zoom, site->y0, zoom));
      assert_false(dt_canvas_handle_site_hit(site, site->x0, site->y0 - 10.5 / zoom, zoom));
    }
  }
  dt_free(sites);
  line->connector.style = DT_CANVAS_CONNECTOR_ARROW_START | DT_CANVAS_CONNECTOR_ARROW_END;
  dt_canvas_connector_add_via(canvas, line);
  _assert_truncates_cleanly(canvas, line, DT_CANVAS_HANDLES_ALL);

  // Asked for the ends alone, the ends alone; locked, none.
  assert_int_equal(dt_canvas_handle_sites(canvas, line, DT_CANVAS_HANDLES_ENDPOINTS, NULL, 0), 2);
  assert_int_equal(dt_canvas_handle_sites(canvas, line, DT_CANVAS_HANDLES_TANGENTS | DT_CANVAS_HANDLES_VIA, NULL, 0),
                   dt_canvas_handle_sites(canvas, line, DT_CANVAS_HANDLES_CONNECTOR, NULL, 0) - 2);
  line->flags |= DT_CANVAS_OBJECT_FLAG_LOCKED;
  assert_int_equal(dt_canvas_handle_sites(canvas, line, DT_CANVAS_HANDLES_ENDPOINTS, NULL, 0), 0);
  line->flags &= ~DT_CANVAS_OBJECT_FLAG_LOCKED;

  // Its end anchored to a frame: the start alone, still where the line starts.
  dt_canvas_object_t *frame = dt_canvas_add_image(canvas, 900.0, 400.0, 1000, 1000);
  line->connector.to_id = frame->id;
  line->connector.to_anchor = DT_CANVAS_ANCHOR_WEST;
  count = _sites(canvas, line, DT_CANVAS_HANDLES_ENDPOINTS, &sites);
  assert_int_equal(count, 1);
  assert_int_equal(sites[0].part, DT_CANVAS_HANDLE_PART_FROM);
  assert_true(sites[0].x0 == 100.0 && sites[0].y0 == 50.0);
  dt_free(sites);
  line->connector.from_id = frame->id;
  line->connector.to_id = 0;
  count = _sites(canvas, line, DT_CANVAS_HANDLES_ENDPOINTS, &sites);
  assert_int_equal(count, 1);
  assert_int_equal(sites[0].part, DT_CANVAS_HANDLE_PART_TO);
  assert_int_equal(sites[0].index, 1);
  assert_true(sites[0].x0 == 400.0 && sites[0].y0 == 250.0);
  dt_free(sites);

  // Between two frames, no end of its own.
  dt_canvas_object_t *other = dt_canvas_add_image(canvas, -900.0, 400.0, 1000, 1000);
  dt_canvas_object_t *connector = dt_canvas_add_connector(canvas, frame->id, other->id);
  assert_int_equal(dt_canvas_handle_sites(canvas, connector, DT_CANVAS_HANDLES_ENDPOINTS, NULL, 0), 0);
  dt_canvas_free(canvas);
}

/**
 * A polygon of N nodes offers N nodes where its stored points put them, every node's three own
 * handles (latent: only the node the pointer works near shows them, but any node can become that
 * one without a click), and N straight edges from each node to the next -- in that order, after
 * its one centre dot. A list too short gets the first sites and the true count, and nothing past
 * its end is written.
 */
static void _a_polygon_offers_its_nodes_their_own_handles_and_its_edges(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *frame = dt_canvas_add_image(canvas, 320.0, -140.0, 3000, 2000);
  frame->width = 480.0;
  frame->height = 270.0;
  frame->rotation = 0.7;
  const uint32_t nodes = 7;
  _random_polygon(canvas, frame, nodes);
  assert_int_equal(frame->mask.node_count, nodes);

  dt_canvas_handle_site_t *sites = NULL;
  const size_t count = _sites(canvas, frame, DT_CANVAS_HANDLES_MASK, &sites);
  assert_int_equal(count, 1 + 5 * nodes);
  const uint32_t order[4]
      = { DT_CANVAS_HANDLE_MASK_POINT, DT_CANVAS_HANDLE_MASK_NODE, DT_CANVAS_HANDLE_MASK_NODE_OWN, DT_CANVAS_HANDLE_MASK_EDGE };
  const size_t firsts[4] = { 0, 1, 1 + nodes, 1 + 4 * nodes };
  const size_t lengths[4] = { 1, nodes, 3 * nodes, nodes };
  for(int group = 0; group < 4; group++)
    for(size_t idx = firsts[group]; idx < firsts[group] + lengths[group]; idx++)
      assert_int_equal(sites[idx].role, order[group]);
  for(uint32_t node = 0; node < nodes; node++)
  {
    double border[2];
    double incoming[2];
    double outgoing[2];
    dt_canvas_mask_node_handles(frame, node, border, incoming, outgoing);
    const double *own[3] = { border, incoming, outgoing };
    const uint32_t parts[3]
        = { DT_CANVAS_HANDLE_PART_BORDER, DT_CANVAS_HANDLE_PART_INCOMING, DT_CANVAS_HANDLE_PART_OUTGOING };
    for(int which = 0; which < 3; which++)
    {
      const dt_canvas_handle_site_t *site
          = _nth_role(sites, count, DT_CANVAS_HANDLE_MASK_NODE_OWN, (size_t)node * 3 + (size_t)which);
      assert_int_equal(site->index, (int)node);
      assert_int_equal(site->part, parts[which]);
      assert_true(site->latent);
      double expected_x = 0.0;
      double expected_y = 0.0;
      _turn(frame, own[which][0], own[which][1], &expected_x, &expected_y);
      assert_double_equal(site->x0, expected_x, 1e-9);
      assert_double_equal(site->y0, expected_y, 1e-9);
    }
    // The node itself from its stored unit-square point, across the frame's own width and height.
    const float *stored = frame->mask.nodes + (size_t)node * DT_CANVAS_MASK_NODE_FLOATS;
    double node_x = 0.0;
    double node_y = 0.0;
    _turn(frame, (stored[DT_CANVAS_MASK_NODE_X] - 0.5) * 480.0, (stored[DT_CANVAS_MASK_NODE_Y] - 0.5) * 270.0, &node_x,
          &node_y);
    const dt_canvas_handle_site_t *node_site = _nth_role(sites, count, DT_CANVAS_HANDLE_MASK_NODE, node);
    const dt_canvas_handle_site_t *edge = _nth_role(sites, count, DT_CANVAS_HANDLE_MASK_EDGE, node);
    const dt_canvas_handle_site_t *next = _nth_role(sites, count, DT_CANVAS_HANDLE_MASK_NODE, (node + 1) % nodes);
    assert_int_equal(node_site->index, (int)node);
    assert_false(node_site->latent);
    assert_double_equal(node_site->x0, node_x, 1e-9);
    assert_double_equal(node_site->y0, node_y, 1e-9);
    assert_int_equal(edge->index, (int)node);
    assert_double_equal(edge->x0, node_site->x0, 1e-9);
    assert_double_equal(edge->y0, node_site->y0, 1e-9);
    assert_double_equal(edge->x1, next->x0, 1e-9);
    assert_double_equal(edge->y1, next->y0, 1e-9);
  }
  dt_free(sites);
  _assert_truncates_cleanly(canvas, frame, DT_CANVAS_HANDLES_ALL);

  // The other shapes answer with the handles dt_canvas_mask_handle_points() lists, and none cut, with none.
  const uint32_t shapes[4] = { DT_CANVAS_MASK_CIRCLE, DT_CANVAS_MASK_ELLIPSE, DT_CANVAS_MASK_GRADIENT, DT_CANVAS_MASK_NONE };
  const size_t points[4] = { 4, 4, 2, 0 };
  for(int shape = 0; shape < 4; shape++)
  {
    dt_canvas_mask_set_shape(canvas, frame, shapes[shape]);
    assert_int_equal(dt_canvas_handle_sites(canvas, frame, DT_CANVAS_HANDLES_MASK, NULL, 0), points[shape]);
  }
  dt_canvas_free(canvas);
}

/**
 * On a square polygon, where the outward direction at a corner is plain to see, a node's own
 * handles sit where they are drawn: its fall-off out along the diagonal by its own border times
 * the frame's side, and a cusp's two control points exactly where it stores them.
 */
static void _a_square_polygon_hangs_a_node_handles_where_they_are_drawn(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *frame = dt_canvas_add_image(canvas, 1000.0, 500.0, 2000, 2000);
  frame->width = 400.0;
  frame->height = 400.0;
  frame->rotation = 0.0;
  float nodes[4 * DT_CANVAS_MASK_NODE_FLOATS] = { 0.0f };
  _square_cusp_nodes(nodes);
  nodes[DT_CANVAS_MASK_NODE_CTRL1_X] = 0.2f;
  nodes[DT_CANVAS_MASK_NODE_CTRL1_Y] = 0.3f;
  nodes[DT_CANVAS_MASK_NODE_CTRL2_X] = 0.3f;
  nodes[DT_CANVAS_MASK_NODE_CTRL2_Y] = 0.2f;
  nodes[DT_CANVAS_MASK_NODE_BORDER1] = 0.1f;
  nodes[DT_CANVAS_MASK_NODE_BORDER2] = 0.1f;
  dt_canvas_mask_set_shape(canvas, frame, DT_CANVAS_MASK_POLYGON);
  dt_canvas_mask_set_nodes(canvas, frame, nodes, 4);
  dt_canvas_handle_site_t sites[32];
  const size_t count = dt_canvas_handle_sites(canvas, frame, DT_CANVAS_HANDLES_MASK_NODES | DT_CANVAS_HANDLES_MASK_NODE_OWN,
                                              sites, G_N_ELEMENTS(sites));
  assert_int_equal(count, 4 + 12);
  // The top-left node, 100 units up and left of the frame's centre.
  assert_double_equal(sites[0].x0, 900.0, 1e-9);
  assert_double_equal(sites[0].y0, 400.0, 1e-9);
  // Its fall-off: its own 0.1 of the 400-unit side, straight out along the diagonal, away from the square.
  const double border_reach = 0.1f * 400.0 * M_SQRT1_2;
  assert_int_equal(sites[4].part, DT_CANVAS_HANDLE_PART_BORDER);
  assert_double_equal(sites[4].x0, 900.0 - border_reach, 1e-9);
  assert_double_equal(sites[4].y0, 400.0 - border_reach, 1e-9);
  // Its control points: (0.2, 0.3) and (0.3, 0.2) of the frame, as stored.
  assert_int_equal(sites[5].part, DT_CANVAS_HANDLE_PART_INCOMING);
  assert_double_equal(sites[5].x0, 1000.0 + (0.2f - 0.5) * 400.0, 1e-9);
  assert_double_equal(sites[5].y0, 500.0 + (0.3f - 0.5) * 400.0, 1e-9);
  assert_int_equal(sites[6].part, DT_CANVAS_HANDLE_PART_OUTGOING);
  assert_double_equal(sites[6].x0, 1000.0 + (0.3f - 0.5) * 400.0, 1e-9);
  assert_double_equal(sites[6].y0, 500.0 + (0.2f - 0.5) * 400.0, 1e-9);
  dt_canvas_free(canvas);
}

/**
 * A circle's, an ellipse's and a gradient's handles sit where the shape puts them on a frame that
 * is neither square nor upright, and carry the index the view maps to a gesture: the centre, the
 * first radius or the reach, the second radius, the feather.
 */
static void _the_cutout_handles_sit_where_the_shape_puts_them(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *frame = dt_canvas_add_image(canvas, 250.0, -80.0, 3000, 2000);
  frame->width = 600.0;
  frame->height = 360.0;
  frame->rotation = -0.6;
  const double side = 360.0;
  const uint32_t shapes[3] = { DT_CANVAS_MASK_CIRCLE, DT_CANVAS_MASK_ELLIPSE, DT_CANVAS_MASK_GRADIENT };
  const size_t expected_counts[3] = { 4, 4, 2 };
  for(int shape_idx = 0; shape_idx < 3; shape_idx++)
  {
    dt_canvas_mask_set_shape(canvas, frame, shapes[shape_idx]);
    frame->mask.center_x = 0.3f;
    frame->mask.center_y = 0.6f;
    frame->mask.radius_x = 0.2f;
    frame->mask.radius_y = 0.35f;
    frame->mask.feather = 0.05f;
    frame->mask.rotation = 25.0f;
    const double center_x = (frame->mask.center_x - 0.5) * 600.0;
    const double center_y = (frame->mask.center_y - 0.5) * 360.0;
    const double radius_x = frame->mask.radius_x * side;
    const double radius_y = frame->mask.radius_y * side;
    const double outer = (frame->mask.radius_x + frame->mask.feather) * side;
    const double turn = frame->mask.rotation * M_PI / 180.0;
    double expected[8] = { center_x, center_y, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    gboolean placed[4] = { TRUE, TRUE, FALSE, FALSE };
    if(shapes[shape_idx] == DT_CANVAS_MASK_CIRCLE)
    {
      // A circle lists four so its feather keeps index 3, but has no second radius: its [2] is
      // not a place this test can name, and is left out rather than pinned.
      expected[2] = center_x + radius_x;
      expected[3] = center_y;
      expected[6] = center_x + M_SQRT1_2 * outer;
      expected[7] = center_y + M_SQRT1_2 * outer;
      placed[3] = TRUE;
    }
    else if(shapes[shape_idx] == DT_CANVAS_MASK_ELLIPSE)
    {
      expected[2] = center_x + cos(turn) * radius_x;
      expected[3] = center_y + sin(turn) * radius_x;
      expected[4] = center_x - sin(turn) * radius_y;
      expected[5] = center_y + cos(turn) * radius_y;
      expected[6] = center_x - cos(turn) * outer;
      expected[7] = center_y - sin(turn) * outer;
      placed[2] = TRUE;
      placed[3] = TRUE;
    }
    else
    {
      // The reach handle stands across the line, on the side the fall-off goes.
      expected[2] = center_x - sin(turn) * radius_x;
      expected[3] = center_y + cos(turn) * radius_x;
    }
    dt_canvas_handle_site_t sites[8];
    const size_t count = dt_canvas_handle_sites(canvas, frame, DT_CANVAS_HANDLES_MASK, sites, G_N_ELEMENTS(sites));
    assert_int_equal(count, expected_counts[shape_idx]);
    for(size_t nth = 0; nth < count; nth++)
    {
      assert_int_equal(sites[nth].role, DT_CANVAS_HANDLE_MASK_POINT);
      assert_int_equal(sites[nth].index, (int)nth);
      if(!placed[nth]) continue;
      double expected_x = 0.0;
      double expected_y = 0.0;
      _turn(frame, expected[2 * nth], expected[2 * nth + 1], &expected_x, &expected_y);
      assert_double_equal(sites[nth].x0, expected_x, 1e-9);
      assert_double_equal(sites[nth].y0, expected_y, 1e-9);
    }
  }
  dt_canvas_free(canvas);
}

/**
 * The band a connector is picked in, asked of its CURVE sites, agrees with
 * dt_canvas_object_contains() at the atelier's tolerance at every point sampled: uniformly over
 * the route's neighbourhood, a hair either side of the band's edge, and -- on a level leg at a
 * zoom where the band is a whole number -- exactly on the edge, where only the comparison itself
 * decides.
 */
static void _the_curve_band_is_exactly_the_pick(void **state)
{
  (void)state;
  size_t inside = 0;
  size_t outside = 0;
  for(int scene_idx = 0; scene_idx < 60; scene_idx++)
  {
    handles_scene_t scene;
    _scene_build(&scene);
    const double zoom = _random_zoom();
    const double tolerance = 4.0 / zoom;
    dt_canvas_route_t route;
    if(!dt_canvas_connector_route(scene.canvas, scene.connector, &route))
    {
      dt_canvas_free(scene.canvas);
      continue;
    }
    dt_canvas_handle_site_t *sites = NULL;
    const size_t count = _sites(scene.canvas, scene.connector, DT_CANVAS_HANDLES_CURVE, &sites);
    assert_int_equal(_count_role(sites, count, DT_CANVAS_HANDLE_CURVE), (size_t)(route.point_count - 1));
    double min_x = route.points[0];
    double max_x = route.points[0];
    double min_y = route.points[1];
    double max_y = route.points[1];
    for(int idx = 1; idx < route.point_count; idx++)
    {
      min_x = fmin(min_x, route.points[2 * idx]);
      max_x = fmax(max_x, route.points[2 * idx]);
      min_y = fmin(min_y, route.points[2 * idx + 1]);
      max_y = fmax(max_y, route.points[2 * idx + 1]);
    }
    const double band = tolerance + scene.connector->connector.line_width;
    for(int sample = 0; sample < 1000; sample++)
    {
      double x = 0.0;
      double y = 0.0;
      if(sample % 2 == 0)
      {
        x = _random_range(min_x - 3.0 * band, max_x + 3.0 * band);
        y = _random_range(min_y - 3.0 * band, max_y + 3.0 * band);
      }
      else
      {
        // On a random leg, across it at the band's edge give or take a hair.
        const int leg = (int)(_random_unit() * (route.point_count - 1));
        const double t = _random_unit();
        const double ax = route.points[2 * leg];
        const double ay = route.points[2 * leg + 1];
        const double bx = route.points[2 * leg + 2];
        const double by = route.points[2 * leg + 3];
        const double along = atan2(by - ay, bx - ax);
        const double offset = band * (1.0 + _random_range(-1e-3, 1e-3)) * (_random_unit() < 0.5 ? -1.0 : 1.0);
        _step(ax + (bx - ax) * t, ay + (by - ay) * t, along + M_PI_2, offset, &x, &y);
      }
      gboolean caught = FALSE;
      for(size_t idx = 0; idx < count && !caught; idx++)
      {
        if(sites[idx].role != DT_CANVAS_HANDLE_CURVE) continue;
        caught = dt_canvas_handle_site_hit(&sites[idx], x, y, zoom);
      }
      const gboolean picked = dt_canvas_object_contains(scene.canvas, scene.connector, x, y, tolerance);
      assert_int_equal(caught, picked);
      if(picked)
        inside++;
      else
        outside++;
    }
    dt_free(sites);
    dt_canvas_free(scene.canvas);
  }
  assert_true(inside > 5000);
  assert_true(outside > 5000);

  // Exactly on the edge: a level leg, four pixels at zoom 1 plus a line two units wide is a band
  // of exactly six, and a point exactly six units off the leg is inside it, both ways of asking.
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *left = dt_canvas_add_text(canvas, 0.0, 0.0, 200.0, 100.0, "");
  dt_canvas_object_t *right = dt_canvas_add_text(canvas, 600.0, 0.0, 200.0, 100.0, "");
  dt_canvas_object_t *connector = dt_canvas_add_connector(canvas, left->id, right->id);
  connector->connector.routing = DT_CANVAS_ROUTING_STRAIGHT;
  connector->connector.style = 0;
  connector->connector.line_width = 2.0f;
  dt_canvas_route_t level;
  assert_true(dt_canvas_connector_route(canvas, connector, &level));
  assert_true(level.points[1] == level.points[3]);
  assert_true(level.points[0] == floor(level.points[0]));
  assert_true(level.points[2] == floor(level.points[2]));
  const double edge_x = level.points[0] + 0.25 * (level.points[2] - level.points[0]);
  const double edge_y = level.points[1] + 6.0;
  const double beyond_y = nextafter(edge_y, INFINITY);
  dt_canvas_handle_site_t curve_sites[16];
  const size_t curve_count
      = dt_canvas_handle_sites(canvas, connector, DT_CANVAS_HANDLES_CURVE, curve_sites, G_N_ELEMENTS(curve_sites));
  assert_true(curve_count >= 1);
  assert_int_equal(curve_sites[0].role, DT_CANVAS_HANDLE_CURVE);
  assert_true(dt_canvas_object_contains(canvas, connector, edge_x, edge_y, 4.0));
  assert_true(dt_canvas_handle_site_hit(&curve_sites[0], edge_x, edge_y, 1.0));
  assert_false(dt_canvas_object_contains(canvas, connector, edge_x, beyond_y, 4.0));
  assert_false(dt_canvas_handle_site_hit(&curve_sites[0], edge_x, beyond_y, 1.0));
  dt_canvas_free(canvas);
}

/** The largest distance from an arrowed tip of ink that is not the line's own stroke, in canvas units. */
static double _arrowhead_ink_reach(const dt_canvas_t *canvas, const dt_canvas_object_t *connector)
{
  dt_canvas_route_t route;
  assert_true(dt_canvas_connector_route(canvas, connector, &route));
  const int margin = 200;
  const int origin_x = (int)floor(fmin(route.from_x, route.to_x)) - margin;
  const int origin_y = (int)floor(fmin(route.from_y, route.to_y)) - margin;
  const int width = (int)ceil(fabs(route.to_x - route.from_x)) + 2 * margin;
  const int height = (int)ceil(fabs(route.to_y - route.from_y)) + 2 * margin;
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(surface);
  cairo_translate(cr, -origin_x, -origin_y);
  const dt_canvas_rect_t whole = { origin_x, origin_y, width, height };
  const dt_canvas_paint_options_t options = dt_canvas_paint_options_export(NULL, 1.0, whole);
  dt_canvas_paint_object(cr, canvas, connector, &options);
  cairo_destroy(cr);
  cairo_surface_flush(surface);
  const uint8_t *pixels = cairo_image_surface_get_data(surface);
  const int stride = cairo_image_surface_get_stride(surface);
  const double half_line = connector->connector.line_width * 0.5;
  double farthest = 0.0;
  for(int row = 0; row < height; row++)
  {
    for(int col = 0; col < width; col++)
    {
      const uint32_t pixel = *(const uint32_t *)(pixels + (size_t)row * stride + (size_t)col * 4);
      if((pixel >> 24) == 0) continue;
      const double x = origin_x + col + 0.5;
      const double y = origin_y + row + 0.5;
      // The stroke's own pixels say nothing about the heads: a head is the ink wider than the line.
      double to_line = INFINITY;
      for(int idx = 0; idx + 1 < route.point_count; idx++)
      {
        const double ax = route.points[2 * idx];
        const double ay = route.points[2 * idx + 1];
        const double bx = route.points[2 * idx + 2];
        const double by = route.points[2 * idx + 3];
        const double length2 = (bx - ax) * (bx - ax) + (by - ay) * (by - ay);
        const double t = length2 > 0.0 ? CLAMP(((x - ax) * (bx - ax) + (y - ay) * (by - ay)) / length2, 0.0, 1.0) : 0.0;
        to_line = fmin(to_line, hypot(x - (ax + t * (bx - ax)), y - (ay + t * (by - ay))));
      }
      if(to_line <= half_line + 1.5) continue;
      double to_tip = INFINITY;
      if(connector->connector.style & DT_CANVAS_CONNECTOR_ARROW_START) to_tip = fmin(to_tip, hypot(x - route.from_x, y - route.from_y));
      if(connector->connector.style & DT_CANVAS_CONNECTOR_ARROW_END) to_tip = fmin(to_tip, hypot(x - route.to_x, y - route.to_y));
      farthest = fmax(farthest, to_tip);
    }
  }
  cairo_surface_destroy(surface);
  return farthest;
}

/**
 * An arrowhead's site reaches exactly as far as the painter says it draws, which is the number
 * the painter grows a connector's box by -- and, painted, the head's ink ends inside that reach
 * and fills most of it, so the number is neither short nor idly generous.
 */
static void _an_arrowhead_reaches_as_far_as_the_painter_draws(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *left = dt_canvas_add_image(canvas, 0.0, 0.0, 1000, 1000);
  dt_canvas_object_t *right = dt_canvas_add_image(canvas, 1000.0, 180.0, 1000, 1000);
  dt_canvas_object_t *connector = dt_canvas_add_connector(canvas, left->id, right->id);
  connector->connector.routing = DT_CANVAS_ROUTING_STRAIGHT;
  connector->connector.style = DT_CANVAS_CONNECTOR_ARROW_START | DT_CANVAS_CONNECTOR_ARROW_END;
  const float widths[7] = { 0.0f, 0.5f, 1.0f, 2.0f, 3.0f, 8.0f, 20.0f };
  for(int idx = 0; idx < 7; idx++)
  {
    connector->connector.line_width = widths[idx];
    const double expected = _expected_arrow_reach(widths[idx]);
    assert_double_equal(dt_canvas_paint_arrow_reach(widths[idx]), expected, 1e-12);
    dt_canvas_handle_site_t sites[64];
    const size_t count = dt_canvas_handle_sites(canvas, connector, DT_CANVAS_HANDLES_CURVE, sites, G_N_ELEMENTS(sites));
    assert_true(count <= G_N_ELEMENTS(sites));
    assert_true(count >= 3);
    // The legs from start to end, then the start's head before the end's.
    for(size_t leg = 0; leg + 2 < count; leg++)
    {
      assert_int_equal(sites[leg].role, DT_CANVAS_HANDLE_CURVE);
      assert_int_equal(sites[leg].index, (int)leg);
    }
    assert_int_equal(_count_role(sites, count, DT_CANVAS_HANDLE_ARROW), 2);
    assert_int_equal(sites[count - 2].part, DT_CANVAS_HANDLE_PART_FROM);
    assert_int_equal(sites[count - 1].part, DT_CANVAS_HANDLE_PART_TO);
    for(size_t nth = 0; nth < 2; nth++)
    {
      const dt_canvas_handle_site_t *arrow = &sites[count - 2 + nth];
      assert_int_equal(arrow->role, DT_CANVAS_HANDLE_ARROW);
      assert_true(arrow->reach_px == 0.0);
      assert_double_equal(arrow->reach_units, expected, 1e-12);
    }
  }
  const float measured_widths[2] = { 2.0f, 8.0f };
  for(int idx = 0; idx < 2; idx++)
  {
    connector->connector.line_width = measured_widths[idx];
    const double reach = dt_canvas_paint_arrow_reach(measured_widths[idx]);
    const double ink = _arrowhead_ink_reach(canvas, connector);
    assert_true(ink <= reach + 1.5);
    assert_true(ink >= 0.85 * reach);
  }
  // No arrowheads, no arrowhead sites: the line alone.
  connector->connector.style = 0;
  dt_canvas_handle_site_t plain[64];
  const size_t plain_count = dt_canvas_handle_sites(canvas, connector, DT_CANVAS_HANDLES_CURVE, plain, G_N_ELEMENTS(plain));
  assert_int_equal(_count_role(plain, plain_count, DT_CANVAS_HANDLE_ARROW), 0);
  dt_canvas_free(canvas);
}

/**
 * The midpoint is the middle of the route's LENGTH, not of its chord: on a route that runs 100
 * units along and then 300 down, half of its 400 is a third of the way down the long leg. And it
 * is where a waypoint is added, to the bit, on every routing.
 */
static void _a_waypoint_is_added_at_the_route_midpoint(void **state)
{
  (void)state;
  dt_canvas_route_t bent;
  memset(&bent, 0, sizeof(bent));
  bent.from_x = 0.0;
  bent.from_y = 0.0;
  bent.to_x = 100.0;
  bent.to_y = 300.0;
  bent.point_count = 3;
  const double bent_points[6] = { 0.0, 0.0, 100.0, 0.0, 100.0, 300.0 };
  memcpy(bent.points, bent_points, sizeof(bent_points));
  double middle_x = 0.0;
  double middle_y = 0.0;
  dt_canvas_route_midpoint(&bent, &middle_x, &middle_y);
  assert_double_equal(middle_x, 100.0, 1e-12);
  assert_double_equal(middle_y, 100.0, 1e-12);

  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *left = dt_canvas_add_image(canvas, 0.0, 0.0, 1000, 1000);
  dt_canvas_object_t *right = dt_canvas_add_image(canvas, 1300.0, 700.0, 1000, 1000);
  dt_canvas_object_t *connector = dt_canvas_add_connector(canvas, left->id, right->id);
  const uint32_t routings[3] = { DT_CANVAS_ROUTING_STRAIGHT, DT_CANVAS_ROUTING_SQUARE, DT_CANVAS_ROUTING_CUBIC };
  for(int idx = 0; idx < 3; idx++)
  {
    connector->connector.routing = routings[idx];
    connector->connector.via_count = 0;
    dt_canvas_route_t route;
    assert_true(dt_canvas_connector_route(canvas, connector, &route));
    double x = 0.0;
    double y = 0.0;
    dt_canvas_route_midpoint(&route, &x, &y);
    dt_canvas_connector_add_via(canvas, connector);
    assert_int_equal(connector->connector.via_count, 1);
    assert_true(x == connector->connector.via_x);
    assert_true(y == connector->connector.via_y);
  }
  dt_canvas_free(canvas);
}

/**
 * A place picked on a connector is kept as a fraction of its route's length, and found again
 * from it. On the bent route (100 along, 300 down) the corner is a quarter of the way; a point
 * off the route counts where the route passes closest to it -- the corner itself for a point
 * out beyond the bend, not a place on either leg's extension; and on every routing the fraction
 * of a point taken at a fraction is that fraction.
 */
static void _a_place_on_a_route_is_a_fraction_of_its_length(void **state)
{
  (void)state;
  dt_canvas_route_t bent;
  memset(&bent, 0, sizeof(bent));
  bent.to_x = 100.0;
  bent.to_y = 300.0;
  bent.point_count = 3;
  const double bent_points[6] = { 0.0, 0.0, 100.0, 0.0, 100.0, 300.0 };
  memcpy(bent.points, bent_points, sizeof(bent_points));
  double corner_x = 0.0;
  double corner_y = 0.0;
  dt_canvas_route_point_at(&bent, 0.25, &corner_x, &corner_y);
  assert_double_equal(corner_x, 100.0, 1e-12);
  assert_double_equal(corner_y, 0.0, 1e-12);
  assert_double_equal(dt_canvas_route_fraction_at(&bent, 100.0, 0.0), 0.25, 1e-12);
  assert_double_equal(dt_canvas_route_fraction_at(&bent, 160.0, 200.0), 0.75, 1e-12);
  assert_double_equal(dt_canvas_route_fraction_at(&bent, 130.0, -40.0), 0.25, 1e-12);
  assert_double_equal(dt_canvas_route_fraction_at(&bent, -50.0, -40.0), 0.0, 1e-12);
  assert_double_equal(dt_canvas_route_fraction_at(&bent, 90.0, 900.0), 1.0, 1e-12);

  dt_canvas_route_t still;
  memset(&still, 0, sizeof(still));
  still.point_count = 2;
  assert_double_equal(dt_canvas_route_fraction_at(&still, 10.0, 10.0), 0.5, 1e-12);

  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *left = dt_canvas_add_image(canvas, 0.0, 0.0, 1000, 1000);
  dt_canvas_object_t *right = dt_canvas_add_image(canvas, 1300.0, 700.0, 1000, 1000);
  dt_canvas_object_t *connector = dt_canvas_add_connector(canvas, left->id, right->id);
  const uint32_t routings[3] = { DT_CANVAS_ROUTING_STRAIGHT, DT_CANVAS_ROUTING_SQUARE, DT_CANVAS_ROUTING_CUBIC };
  for(int routing = 0; routing < 3; routing++)
  {
    connector->connector.routing = routings[routing];
    dt_canvas_route_t route;
    assert_true(dt_canvas_connector_route(canvas, connector, &route));
    for(int step = 0; step <= 20; step++)
    {
      const double fraction = step / 20.0;
      double x = 0.0;
      double y = 0.0;
      dt_canvas_route_point_at(&route, fraction, &x, &y);
      assert_double_equal(dt_canvas_route_fraction_at(&route, x, y), fraction, 1e-9);
    }
  }
  dt_canvas_free(canvas);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_a_place_on_a_route_is_a_fraction_of_its_length),
    cmocka_unit_test(_every_site_catches_its_centre_and_lets_go_past_its_reach),
    cmocka_unit_test(_a_handle_that_is_not_a_number_catches_nothing),
    cmocka_unit_test(_the_knob_floats_a_screen_distance_above_a_turned_frame),
    cmocka_unit_test(_a_locked_frame_offers_no_frame_handles_but_keeps_its_cutout),
    cmocka_unit_test(_a_cubic_connector_offers_a_tangent_per_control_point),
    cmocka_unit_test(_a_line_offers_its_free_ends_and_nothing_at_an_anchored_one),
    cmocka_unit_test(_a_polygon_offers_its_nodes_their_own_handles_and_its_edges),
    cmocka_unit_test(_a_square_polygon_hangs_a_node_handles_where_they_are_drawn),
    cmocka_unit_test(_the_cutout_handles_sit_where_the_shape_puts_them),
    cmocka_unit_test(_the_curve_band_is_exactly_the_pick),
    cmocka_unit_test(_an_arrowhead_reaches_as_far_as_the_painter_draws),
    cmocka_unit_test(_a_waypoint_is_added_at_the_route_midpoint),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
