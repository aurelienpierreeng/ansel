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

#include "canvas/canvas_place.h"

#include "system/macros.h"
#include "system/mem_alloc.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/** The levels, from the strictest to the loosest; each one masks a subset of the one before. */
#define PLACE_LEVELS 3
/** How many columns are searched first, nearest the anchor; the others only when these hold nothing. */
#define PLACE_NEAREST_COLUMNS 64
/** An anchor that moved this far or less leaves the properties exactly where they are. */
#define PLACE_HYSTERESIS_PIXELS 2.0
/** The card is never given more than this fraction of the view's height. */
#define PLACE_CARD_VIEW_FRACTION 0.6
/** The steps a pixel is cut into for the anchor and the press: see `_snap_fine()`. */
#define PLACE_POINT_STEPS 256.0
/** The most pixels a view spans, across and down; see dt_canvas_place_solve(). */
#define PLACE_VIEW_MAX 65536.0
/** How far from the origin a view may start and still be a screen: every pixel is exact out there. */
#define PLACE_COORDINATE_MAX 1e9
/** How much more of the body a placement kept over it may cover: rounding, never a real growth. */
#define PLACE_BODY_SLACK 0.001

/* What each term of a candidate's cost weighs. The distance, the movement and the edge are
 * fractions of the view's diagonal, the soft and the body cover fractions of an area, so the
 * weights compare terms of the same order and the ranking holds at every window size. */
#define PLACE_COST_DISTANCE 100.0
#define PLACE_COST_SIDE 8.0
#define PLACE_COST_SOFT 40.0
#define PLACE_COST_MOVE 300.0
#define PLACE_COST_SCROLL 30.0
#define PLACE_COST_ROOM 20.0
#define PLACE_COST_BODY 2000.0
#define PLACE_COST_EDGE 100.0
#define PLACE_COST_PRESS 400.0

/** A rectangle by its edges, every one a whole pixel once the scene is snapped. */
typedef struct place_box_t
{
  double x0;
  double y0;
  double x1;
  double y1;
} place_box_t;

typedef struct place_soft_t
{
  place_box_t box;
  double weight;
} place_soft_t;

/** A column to search, with its distance from the anchor so the nearest ones go first. */
typedef struct place_column_t
{
  double key;
  double x;
  gboolean left_stop; ///< a column a placement slid left would stop at: the view's edge or a shape's
  double run;         ///< the longest free stretch of the view's height over the column, at its level
} place_column_t;

/** The best candidate found so far at one level of the ladder. */
typedef struct place_best_t
{
  gboolean found;
  double cost;
  double x;
  double y;
  dt_canvas_place_growth_t growth;
  double card;
} place_best_t;

/** One rung of the ladder: a level, and whether the card is asked for at it. */
typedef struct place_rung_t
{
  int level;
  gboolean with_card;
} place_rung_t;

/** The input, snapped to whole pixels and sorted for the search. */
typedef struct place_scene_t
{
  place_box_t view;
  double air;
  double width;
  double strip_height;
  gboolean card_open;
  double card_want;          ///< what the card asks for, capped; 0 while it is closed
  double card_min;
  double last_card_height;
  double diagonal;
  place_box_t object;        ///< the box around the edited object's own HARD and BODY shapes
  double anchor_x;
  double anchor_y;
  gboolean has_press;
  double press_x;
  double press_y;
  const dt_canvas_place_t *previous;
  double previous_shift_x;   ///< the anchor's motion since the previous placement, in whole pixels
  double previous_shift_y;
  // What each level must stay clear of, grown by the air and sorted by top edge.
  place_box_t *masked[PLACE_LEVELS];
  size_t masked_count[PLACE_LEVELS];
  place_column_t *columns[PLACE_LEVELS];
  size_t column_count[PLACE_LEVELS];
  gboolean columns_ready[PLACE_LEVELS];
  gboolean runs_ready[PLACE_LEVELS];
  double level_bound[PLACE_LEVELS]; ///< no column's free stretch is longer: what the boxes blocking every column leave
  size_t nearest_first[PLACE_LEVELS]; ///< the columns nearest the anchor, a window of the x-sorted columns
  size_t nearest_last[PLACE_LEVELS];
  double level_run[PLACE_LEVELS]; ///< the longest free stretch over any column: nothing taller fits the level
  place_box_t *body;
  size_t body_count;
  double body_total;         ///< the body's area inside the view: what a cover is a fraction of
  place_soft_t *soft;
  size_t soft_count;
  double *spans;             ///< scratch: the free stretches of one column, as pairs
} place_scene_t;

static double _overlap_area(const place_box_t *first, const place_box_t *second)
{
  const double width = MIN(first->x1, second->x1) - MAX(first->x0, second->x0);
  const double height = MIN(first->y1, second->y1) - MAX(first->y0, second->y0);
  if(width <= 0.0 || height <= 0.0) return 0.0;
  return width * height;
}

/** Half a pixel rounds up whatever the sign, so a scene moved by whole pixels rounds the same way. */
static double _round_half_up(const double value)
{
  return floor(value + 0.5);
}

/**
 * A point snapped to a 256th of a pixel, below anything a placement can show. Every distance the
 * cost measures from it to a whole-pixel edge is then exact, so a scene moved by whole pixels costs
 * the same to the last bit and two spots that nearly tie are ranked the same way wherever the scene
 * sits: measured from the raw point, the rounding of the subtraction changed with the position and
 * a pan of the whole scene could pick the other spot.
 */
static double _snap_fine(const double value)
{
  return floor(value * PLACE_POINT_STEPS + 0.5) / PLACE_POINT_STEPS;
}

/** A shape snapped outward to whole pixels: never smaller than the reach it was grown by. */
static gboolean _snap_outward(const dt_canvas_place_rect_t *rect, place_box_t *box)
{
  const double left = rect->x;
  const double right = rect->x + rect->width;
  const double top = rect->y;
  const double bottom = rect->y + rect->height;
  if(!isfinite(left) || !isfinite(right) || !isfinite(top) || !isfinite(bottom)) return FALSE;
  box->x0 = floor(MIN(left, right));
  box->x1 = ceil(MAX(left, right));
  box->y0 = floor(MIN(top, bottom));
  box->y1 = ceil(MAX(top, bottom));
  return TRUE;
}

/** Does the class keep the level's placements clear of it? */
static gboolean _level_masks(const int level, const dt_canvas_place_class_t shape_class)
{
  switch(shape_class)
  {
    case DT_CANVAS_PLACE_HARD:
      return TRUE;
    case DT_CANVAS_PLACE_PREDICTED:
      return level == DT_CANVAS_PLACE_LEVEL_HPB;
    case DT_CANVAS_PLACE_BODY:
      return level != DT_CANVAS_PLACE_LEVEL_H;
    default:
      return FALSE;
  }
}

/** The pixel row of the view a box's top edge is read at: `_spans()` clamps it into the view. */
static size_t _top_row(const place_scene_t *scene, const place_box_t *box)
{
  return (size_t)(MAX(box->y0, scene->view.y0) - scene->view.y0);
}

/**
 * Every level's boxes, sorted by the top edge `_spans()` reads, clamped into the view. Every edge is
 * a whole pixel and a box that reaches the view has its top above the view's bottom, so they are
 * bucketed per pixel row rather than sorted: sorting them three times, and the columns once per
 * level, was half the time of a solve. One pass orders all three levels, each keeping a subset in
 * the same order. Boxes with the same top may come in any order; the spans merge them the same way.
 */
static void _masked_sort(place_scene_t *scene, const place_box_t *boxes, const dt_canvas_place_class_t *classes,
                         const size_t count)
{
  const size_t rows = (size_t)MAX(scene->view.y1 - scene->view.y0, 0.0) + 1;
  size_t *row_offset = g_new0(size_t, rows + 1);
  for(size_t idx = 0; idx < count; idx++) row_offset[_top_row(scene, &boxes[idx]) + 1]++;
  for(size_t row = 0; row < rows; row++) row_offset[row + 1] += row_offset[row];
  size_t *order = g_new(size_t, count + 1);
  for(size_t idx = 0; idx < count; idx++)
  {
    const size_t row = _top_row(scene, &boxes[idx]);
    order[row_offset[row]] = idx;
    row_offset[row]++;
  }
  for(size_t rank = 0; rank < count; rank++)
  {
    const size_t idx = order[rank];
    for(int level = 0; level < PLACE_LEVELS; level++)
    {
      if(!_level_masks(level, classes[idx])) continue;
      scene->masked[level][scene->masked_count[level]] = boxes[idx];
      scene->masked_count[level]++;
    }
  }
  dt_free(order);
  dt_free(row_offset);
}

static void _scene_init(place_scene_t *scene, const dt_canvas_place_input_t *input)
{
  memset(scene, 0, sizeof(*scene));
  // The view snaps inward and everything that must stay clear snaps outward, so a whole-pixel
  // placement inside the snapped scene is inside the real one.
  scene->view.x0 = ceil(input->view.x);
  scene->view.y0 = ceil(input->view.y);
  // A view is a screen: past PLACE_VIEW_MAX pixels it is cut off at its right and bottom, which is
  // what lets every search below bucket its positions by pixel instead of sorting them.
  scene->view.x1 = MIN(floor(input->view.x + input->view.width), scene->view.x0 + PLACE_VIEW_MAX);
  scene->view.y1 = MIN(floor(input->view.y + input->view.height), scene->view.y0 + PLACE_VIEW_MAX);
  scene->air = ceil(MAX(input->air, 0.0));
  scene->width = ceil(MAX(input->width, 0.0));
  scene->strip_height = ceil(MAX(input->strip_height, 0.0));
  scene->card_open = input->card_open;
  const double view_height = MAX(scene->view.y1 - scene->view.y0, 0.0);
  const double card_cap = MIN(input->card_max, PLACE_CARD_VIEW_FRACTION * view_height);
  scene->card_want = input->card_open ? floor(MAX(MIN(input->card_content_height, card_cap), 0.0)) : 0.0;
  scene->card_min = ceil(MAX(input->card_min, 0.0));
  scene->last_card_height = MAX(input->last_card_height, 0.0);
  scene->diagonal = MAX(hypot(scene->view.x1 - scene->view.x0, view_height), 1.0);
  scene->anchor_x = _snap_fine(input->anchor_x);
  scene->anchor_y = _snap_fine(input->anchor_y);
  scene->has_press = input->has_press && isfinite(input->press_x) && isfinite(input->press_y);
  scene->press_x = scene->has_press ? _snap_fine(input->press_x) : 0.0;
  scene->press_y = scene->has_press ? _snap_fine(input->press_y) : 0.0;
  // A placement that found no room was never on screen: there is nothing to keep and nowhere to stay
  // near. Its strip is all zeros, and measuring a movement from it pulled the next placement toward
  // the view's top-left corner instead of the anchor.
  scene->previous = NULL;
  if(input->reason != DT_CANVAS_PLACE_OPEN && !IS_NULL_PTR(input->previous) && input->previous->visible)
  {
    scene->previous = input->previous;
    // Whole pixels, half a pixel rounding up whatever the sign: a fraction left over is carried to
    // the next motion by the anchor a kept placement records, instead of lost.
    scene->previous_shift_x = _round_half_up(scene->anchor_x - _snap_fine(input->previous->anchor_x));
    scene->previous_shift_y = _round_half_up(scene->anchor_y - _snap_fine(input->previous->anchor_y));
  }

  const size_t count = IS_NULL_PTR(input->shapes) ? 0 : input->shape_count;
  for(int level = 0; level < PLACE_LEVELS; level++)
  {
    scene->masked[level] = g_new(place_box_t, count + 1);
    scene->columns[level] = NULL;
  }
  scene->body = g_new(place_box_t, count + 1);
  scene->soft = g_new(place_soft_t, count + 1);
  scene->spans = g_new(double, 2 * (count + 2));
  place_box_t *grown_boxes = g_new(place_box_t, count + 1);
  dt_canvas_place_class_t *grown_classes = g_new(dt_canvas_place_class_t, count + 1);
  size_t grown_count = 0;

  gboolean object_found = FALSE;
  for(size_t idx = 0; idx < count; idx++)
  {
    const dt_canvas_place_shape_t *shape = &input->shapes[idx];
    place_box_t box;
    if(!_snap_outward(&shape->rect, &box)) continue;
    if(shape->target && (shape->shape_class == DT_CANVAS_PLACE_HARD || shape->shape_class == DT_CANVAS_PLACE_BODY))
    {
      if(!object_found)
        scene->object = box;
      else
      {
        scene->object.x0 = MIN(scene->object.x0, box.x0);
        scene->object.y0 = MIN(scene->object.y0, box.y0);
        scene->object.x1 = MAX(scene->object.x1, box.x1);
        scene->object.y1 = MAX(scene->object.y1, box.y1);
      }
      object_found = TRUE;
    }
    if(shape->shape_class == DT_CANVAS_PLACE_SOFT)
    {
      scene->soft[scene->soft_count].box = box;
      scene->soft[scene->soft_count].weight = shape->weight;
      scene->soft_count++;
      continue;
    }
    if(shape->shape_class == DT_CANVAS_PLACE_BODY)
    {
      scene->body[scene->body_count] = box;
      scene->body_count++;
      scene->body_total += _overlap_area(&box, &scene->view);
    }
    // Grown by the air once, here, so every later test is a plain open overlap. A shape that
    // cannot reach the view once grown can never block a placement inside it.
    const place_box_t grown = { box.x0 - scene->air, box.y0 - scene->air, box.x1 + scene->air, box.y1 + scene->air };
    if(grown.x0 >= scene->view.x1 || grown.x1 <= scene->view.x0 || grown.y0 >= scene->view.y1
       || grown.y1 <= scene->view.y0)
      continue;
    grown_boxes[grown_count] = grown;
    grown_classes[grown_count] = shape->shape_class;
    grown_count++;
  }
  if(!object_found)
  {
    const double anchor_x = _round_half_up(scene->anchor_x);
    const double anchor_y = _round_half_up(scene->anchor_y);
    scene->object.x0 = anchor_x;
    scene->object.x1 = anchor_x;
    scene->object.y0 = anchor_y;
    scene->object.y1 = anchor_y;
  }
  _masked_sort(scene, grown_boxes, grown_classes, grown_count);
  dt_free(grown_classes);
  dt_free(grown_boxes);
}

static void _scene_cleanup(place_scene_t *scene)
{
  for(int level = 0; level < PLACE_LEVELS; level++)
  {
    dt_free(scene->masked[level]);
    dt_free(scene->columns[level]);
  }
  dt_free(scene->body);
  dt_free(scene->soft);
  dt_free(scene->spans);
}

/**
 * The free stretches of the view's height over the column [x, x + width), at a level: the view
 * less every blocked stretch of a masked shape that overlaps the column. Exact, so a strip fits
 * a column if and only if one of these holds its height, and nothing needs a search along y.
 * Only the stretches tall enough for the strip are kept.
 * @return how many pairs `scene->spans` holds
 */
static size_t _spans(place_scene_t *scene, const double x, const int level)
{
  // The boxes are sorted by their top edge once per solve, so the blocked stretches of a column
  // come out in order and are merged in the same walk: sorting them per column was nine tenths of
  // a solve's time.
  const place_box_t *boxes = scene->masked[level];
  const size_t count = scene->masked_count[level];
  const double right = x + scene->width;
  size_t span_count = 0;
  double cursor = scene->view.y0;
  for(size_t idx = 0; idx < count; idx++)
  {
    const place_box_t *box = &boxes[idx];
    if(box->x0 >= right || box->x1 <= x) continue;
    const double low = MAX(box->y0, scene->view.y0);
    const double high = MIN(box->y1, scene->view.y1);
    if(high <= low) continue;
    if(low - cursor >= scene->strip_height)
    {
      scene->spans[2 * span_count] = cursor;
      scene->spans[2 * span_count + 1] = low;
      span_count++;
    }
    cursor = MAX(cursor, high);
  }
  if(scene->view.y1 - cursor >= scene->strip_height)
  {
    scene->spans[2 * span_count] = cursor;
    scene->spans[2 * span_count + 1] = scene->view.y1;
    span_count++;
  }
  return span_count;
}

/** Is a footprint inside the view and clear of everything the level masks? */
static gboolean _valid(const place_scene_t *scene, const place_box_t *footprint, const int level)
{
  if(footprint->x0 < scene->view.x0 || footprint->x1 > scene->view.x1 || footprint->y0 < scene->view.y0
     || footprint->y1 > scene->view.y1)
    return FALSE;
  const place_box_t *boxes = scene->masked[level];
  const size_t count = scene->masked_count[level];
  for(size_t idx = 0; idx < count; idx++)
  {
    const place_box_t *box = &boxes[idx];
    if(box->y0 >= footprint->y1) break;
    if(box->x0 < footprint->x1 && box->x1 > footprint->x0 && box->y1 > footprint->y0) return FALSE;
  }
  return TRUE;
}

/** The free stretch a strip sits in at a level, if it sits in one. */
static gboolean _span_holding(place_scene_t *scene, const place_box_t *strip, const int level, double *low,
                              double *high)
{
  if(strip->x0 < scene->view.x0 || strip->x1 > scene->view.x1) return FALSE;
  const size_t span_count = _spans(scene, strip->x0, level);
  for(size_t idx = 0; idx < span_count; idx++)
  {
    const double span_low = scene->spans[2 * idx];
    const double span_high = scene->spans[2 * idx + 1];
    if(span_low > strip->y0 || strip->y1 > span_high) continue;
    *low = span_low;
    *high = span_high;
    return TRUE;
  }
  return FALSE;
}

static place_box_t _strip_box(const place_scene_t *scene, const double x, const double y)
{
  const place_box_t strip = { x, y, x + scene->width, y + scene->strip_height };
  return strip;
}

static place_box_t _footprint_box(const place_box_t *strip, const dt_canvas_place_growth_t growth, const double card)
{
  place_box_t footprint = *strip;
  if(growth == DT_CANVAS_PLACE_UP)
    footprint.y0 -= card;
  else
    footprint.y1 += card;
  return footprint;
}

static double _body_cover(const place_scene_t *scene, const place_box_t *footprint)
{
  if(scene->body_total <= 0.0) return 0.0;
  double covered = 0.0;
  for(size_t idx = 0; idx < scene->body_count; idx++) covered += _overlap_area(footprint, &scene->body[idx]);
  return covered / scene->body_total;
}

static double _soft_cover(const place_scene_t *scene, const place_box_t *footprint)
{
  const double area = (footprint->x1 - footprint->x0) * (footprint->y1 - footprint->y0);
  if(area <= 0.0) return 0.0;
  double covered = 0.0;
  for(size_t idx = 0; idx < scene->soft_count; idx++)
    covered += scene->soft[idx].weight * _overlap_area(footprint, &scene->soft[idx].box);
  return covered / area;
}

static double _distance_to_box(const place_box_t *box, const double x, const double y)
{
  const double distance_x = MAX(MAX(box->x0 - x, 0.0), x - box->x1);
  const double distance_y = MAX(MAX(box->y0 - y, 0.0), y - box->y1);
  return hypot(distance_x, distance_y);
}

/** Which side of the object a strip is on, in the order they are preferred: below, right, above, left. */
static double _side_rank(const place_box_t *strip, const place_box_t *object)
{
  if(strip->y0 >= object->y1) return 0.0;
  if(strip->x0 >= object->x1) return 1.0;
  if(strip->y1 <= object->y0) return 2.0;
  if(strip->x1 <= object->x0) return 3.0;
  return 4.0;
}

static double _cost(const place_scene_t *scene, const place_box_t *strip, const place_box_t *footprint,
                    const int level, const double card, const double free_down, const double free_up)
{
  double cost = PLACE_COST_DISTANCE * _distance_to_box(footprint, scene->anchor_x, scene->anchor_y) / scene->diagonal;
  cost += PLACE_COST_SIDE * _side_rank(strip, &scene->object);
  cost += PLACE_COST_SOFT * _soft_cover(scene, footprint);
  if(!IS_NULL_PTR(scene->previous))
  {
    const double moved = hypot(strip->x0 - scene->previous->strip.x, strip->y0 - scene->previous->strip.y);
    cost += PLACE_COST_MOVE * moved / scene->diagonal;
  }
  if(scene->card_open && card < scene->card_want) cost += PLACE_COST_SCROLL;
  // A closed card still has a height it will want: prefer a strip that leaves room for it.
  if(!scene->card_open && MAX(free_down, free_up) < scene->last_card_height) cost += PLACE_COST_ROOM;
  if(level == DT_CANVAS_PLACE_LEVEL_H)
  {
    // Over the body, cover as little of it as there is, keep to an edge of the view, and never
    // sit where the user just pressed.
    const double edge = MIN(MIN(footprint->x0 - scene->view.x0, scene->view.x1 - footprint->x1),
                            MIN(footprint->y0 - scene->view.y0, scene->view.y1 - footprint->y1));
    cost += PLACE_COST_BODY * _body_cover(scene, footprint);
    cost += PLACE_COST_EDGE * MAX(edge, 0.0) / scene->diagonal;
    if(scene->has_press && scene->press_x >= footprint->x0 && scene->press_x < footprint->x1
       && scene->press_y >= footprint->y0 && scene->press_y < footprint->y1)
      cost += PLACE_COST_PRESS;
  }
  return cost;
}

/** Mark a column to search, rounded to a whole pixel and clamped into the positions a strip can take. */
static void _column_mark(const place_scene_t *scene, guint8 *marks, const double x, const guint8 flags)
{
  const double first = scene->view.x0;
  const double last = scene->view.x1 - scene->width;
  const double snapped = CLAMP(_round_half_up(x), first, last);
  marks[(size_t)(snapped - first)] |= flags;
}

/**
 * A node of the tree the column sweep keeps over the view's height: how much of its stretch is
 * free, as runs. `cover` counts the blocked stretches that cover the whole node and are not
 * counted in its children, which is what lets a stretch be added and taken away again in log time.
 */
typedef struct place_run_node_t
{
  int32_t cover;
  int32_t length; ///< in whole pixels, which every edge is, and no view has more than PLACE_VIEW_MAX of
  int32_t prefix; ///< the free run starting at the node's top
  int32_t suffix; ///< the free run ending at its bottom
  int32_t best;   ///< the longest free run inside it
} place_run_node_t;

/**
 * The tree, complete: `leaves` is a power of two, leaf `i` is node `leaves + i`, node `i` has the
 * children `2i` and `2i + 1`, node 1 is the root. The leaves past the view's last stretch have no
 * length, and a stretch of no length is free and changes no run.
 */
typedef struct place_run_tree_t
{
  place_run_node_t *nodes;
  size_t leaves;
} place_run_tree_t;

static void _run_pull(place_run_tree_t *tree, const size_t node)
{
  place_run_node_t *self = &tree->nodes[node];
  if(self->cover > 0)
  {
    self->prefix = 0;
    self->suffix = 0;
    self->best = 0;
    return;
  }
  if(node >= tree->leaves)
  {
    self->prefix = self->length;
    self->suffix = self->length;
    self->best = self->length;
    return;
  }
  const place_run_node_t *left = &tree->nodes[2 * node];
  const place_run_node_t *right = &tree->nodes[2 * node + 1];
  self->prefix = left->prefix == left->length ? left->length + right->prefix : left->prefix;
  self->suffix = right->suffix == right->length ? right->length + left->suffix : right->suffix;
  self->best = MAX(MAX(left->best, right->best), left->suffix + right->prefix);
}

/** A tree over `stretch_count` stretches between the sorted `heights`, nothing covered. */
static void _run_build(place_run_tree_t *tree, const double *heights, const size_t stretch_count)
{
  size_t leaves = 1;
  while(leaves < MAX(stretch_count, (size_t)1)) leaves *= 2;
  tree->leaves = leaves;
  tree->nodes = g_new0(place_run_node_t, 2 * leaves);
  for(size_t leaf = 0; leaf < leaves; leaf++)
  {
    place_run_node_t *node = &tree->nodes[leaves + leaf];
    node->length = leaf < stretch_count ? (int32_t)(heights[leaf + 1] - heights[leaf]) : 0;
    _run_pull(tree, leaves + leaf);
  }
  for(size_t node = leaves - 1; node > 0; node--)
  {
    tree->nodes[node].length = tree->nodes[2 * node].length + tree->nodes[2 * node + 1].length;
    _run_pull(tree, node);
  }
}

/**
 * Cover, or uncover, the stretches `from` to `to` (excluded). Bottom-up: the nodes whose whole range
 * lies inside take the count, and only the two paths from the ends to the root are recomputed. The
 * recursive walk it replaced was the sweep's main cost.
 */
static void _run_update(place_run_tree_t *tree, const size_t from, const size_t to, const int delta)
{
  if(from >= to) return;
  size_t left = from + tree->leaves;
  size_t right = to + tree->leaves;
  const size_t first = left;
  const size_t last = right - 1;
  while(left < right)
  {
    if(left & 1)
    {
      tree->nodes[left].cover += delta;
      _run_pull(tree, left);
      left++;
    }
    if(right & 1)
    {
      right--;
      tree->nodes[right].cover += delta;
      _run_pull(tree, right);
    }
    left >>= 1;
    right >>= 1;
  }
  for(size_t node = first >> 1; node > 0; node >>= 1) _run_pull(tree, node);
  for(size_t node = last >> 1; node > 0; node >>= 1) _run_pull(tree, node);
}

/**
 * The longest free stretch of the view's height over every column of a level, in one sweep from
 * left to right. A masked box blocks the column at x exactly when x + width > box.x0 and x < box.x1,
 * so with every edge a whole pixel it starts blocking at box.x0 - width + 1 and stops at box.x1;
 * a tree over the view's height keeps the longest free run as boxes come and go.
 *
 * This is what bounds a search. Asking a column its free stretches walks every masked box, and the
 * search used to ask that of every left stop whenever the nearest columns held nothing -- a number
 * of columns that grows with the boxes, so the solve grew with their square: 9.2 ms for 2048 shapes
 * over an object filling the view, where neither level clear of the body has room anywhere and
 * every column was walked to learn it. With the runs known, a level with no room is given up at
 * once and a column too short for what is asked is never walked.
 *
 * Every position is a whole pixel of a view no larger than a screen, so the starts, the stops and
 * the heights are bucketed by pixel rather than sorted.
 */
static void _columns_runs(place_scene_t *scene, const int level)
{
  if(scene->runs_ready[level]) return;
  scene->runs_ready[level] = TRUE;
  place_column_t *columns = scene->columns[level];
  const size_t column_count = scene->column_count[level];
  const place_box_t *boxes = scene->masked[level];
  const size_t box_count = scene->masked_count[level];
  const double first_column = scene->view.x0;
  const double last_column = scene->view.x1 - scene->width;
  const size_t positions = (size_t)(last_column - first_column) + 1;
  const size_t rows = (size_t)(scene->view.y1 - scene->view.y0) + 1;

  // The view's height, cut at every edge a box brings into it: the tree's leaves.
  int *height_rank = g_new0(int, rows);
  height_rank[0] = 1;
  height_rank[rows - 1] = 1;
  for(size_t idx = 0; idx < box_count; idx++)
  {
    const double low = MAX(boxes[idx].y0, scene->view.y0);
    const double high = MIN(boxes[idx].y1, scene->view.y1);
    if(high <= low) continue;
    height_rank[(size_t)(low - scene->view.y0)] = 1;
    height_rank[(size_t)(high - scene->view.y0)] = 1;
  }
  double *heights = g_new(double, rows);
  size_t distinct = 0;
  for(size_t row = 0; row < rows; row++)
  {
    if(!height_rank[row]) continue;
    heights[distinct] = scene->view.y0 + (double)row;
    height_rank[row] = (int)distinct;
    distinct++;
  }
  const size_t leaves = distinct - 1;

  // Where each box starts and stops blocking, bucketed by column position: a start left of the
  // first position applies before it, and a stop right of the last never applies.
  size_t *event_offset = g_new0(size_t, positions + 1);
  for(size_t idx = 0; idx < box_count; idx++)
  {
    const double low = MAX(boxes[idx].y0, scene->view.y0);
    const double high = MIN(boxes[idx].y1, scene->view.y1);
    const double start = boxes[idx].x0 - scene->width + 1.0;
    const double stop = boxes[idx].x1;
    // A box blocking no position, or no height of the view, changes no run: `_spans()` skips it too.
    if(high <= low || start >= stop || start > last_column || stop <= first_column) continue;
    event_offset[(size_t)(MAX(start, first_column) - first_column) + 1]++;
    if(stop <= last_column) event_offset[(size_t)(stop - first_column) + 1]++;
  }
  for(size_t position = 0; position < positions; position++) event_offset[position + 1] += event_offset[position];
  const size_t event_count = event_offset[positions];
  // A start is the box's index plus one, a stop its negation: one signed number per event.
  ptrdiff_t *events = g_new(ptrdiff_t, event_count + 1);
  size_t *event_fill = g_new(size_t, positions + 1);
  memcpy(event_fill, event_offset, (positions + 1) * sizeof(size_t));
  for(size_t idx = 0; idx < box_count; idx++)
  {
    const double low = MAX(boxes[idx].y0, scene->view.y0);
    const double high = MIN(boxes[idx].y1, scene->view.y1);
    const double start = boxes[idx].x0 - scene->width + 1.0;
    const double stop = boxes[idx].x1;
    if(high <= low || start >= stop || start > last_column || stop <= first_column) continue;
    const size_t start_position = (size_t)(MAX(start, first_column) - first_column);
    events[event_fill[start_position]] = (ptrdiff_t)idx + 1;
    event_fill[start_position]++;
    if(stop > last_column) continue;
    const size_t stop_position = (size_t)(stop - first_column);
    events[event_fill[stop_position]] = -((ptrdiff_t)idx + 1);
    event_fill[stop_position]++;
  }

  place_run_tree_t tree;
  _run_build(&tree, heights, leaves);
  double level_run = 0.0;
  size_t column = 0;
  for(size_t position = 0; position < positions && column < column_count; position++)
  {
    // Every start and stop at or before the position is applied before a column there is read.
    for(size_t event = event_offset[position]; event < event_offset[position + 1]; event++)
    {
      const int delta = events[event] > 0 ? 1 : -1;
      const size_t box = (size_t)(events[event] > 0 ? events[event] : -events[event]) - 1;
      const double low = MAX(boxes[box].y0, scene->view.y0);
      const double high = MIN(boxes[box].y1, scene->view.y1);
      const size_t from = (size_t)height_rank[(size_t)(low - scene->view.y0)];
      const size_t to = (size_t)height_rank[(size_t)(high - scene->view.y0)];
      _run_update(&tree, from, to, delta);
    }
    // The columns are sorted by position, one per position at most.
    if((size_t)(columns[column].x - first_column) != position) continue;
    columns[column].run = (double)tree.nodes[1].best;
    level_run = MAX(level_run, columns[column].run);
    column++;
  }
  scene->level_run[level] = level_run;

  dt_free(tree.nodes);
  dt_free(event_fill);
  dt_free(events);
  dt_free(event_offset);
  dt_free(heights);
  dt_free(height_rank);
}

/**
 * A bound on every column's longest free stretch, from the boxes that block every column alike:
 * what they leave free, no column has more of. An object filling the view is exactly that -- its
 * body is as wide as the view -- and the bound then gives up both levels clear of it without the
 * sweep, which is the one case the nearest columns always fail and the sweep would always run.
 */
static double _columns_bound(const place_scene_t *scene, const int level)
{
  const place_box_t *boxes = scene->masked[level];
  const size_t box_count = scene->masked_count[level];
  const double first_column = scene->view.x0;
  const double last_column = scene->view.x1 - scene->width;
  double cursor = scene->view.y0;
  double longest = 0.0;
  // In top order, so the blocked stretches merge in one walk as they do in `_spans()`.
  for(size_t idx = 0; idx < box_count; idx++)
  {
    const double start = boxes[idx].x0 - scene->width + 1.0;
    if(start > first_column || boxes[idx].x1 <= last_column) continue;
    const double low = MAX(boxes[idx].y0, scene->view.y0);
    const double high = MIN(boxes[idx].y1, scene->view.y1);
    if(high <= low) continue;
    longest = MAX(longest, low - cursor);
    cursor = MAX(cursor, high);
  }
  return MAX(longest, scene->view.y1 - cursor);
}

/** A column position is worth searching. */
#define PLACE_COLUMN_MARKED 1
/** And a placement slid left stops there. */
#define PLACE_COLUMN_LEFT_STOP 2

/**
 * The columns worth searching at a level, sorted by position. A free placement can always be slid
 * left until it touches the view's edge or the air around a shape it would otherwise run into, so
 * the view's left edge and every masked shape's right edge plus the air are enough to find one if
 * one exists at all: those are the LEFT STOPS. The other columns -- the anchor's own, the object's
 * edges, a shape's left edge less the air and the width, where the properties already are and
 * where following the object would take them -- are there for the cost, since the best placement
 * is usually lined up with one of them.
 *
 * The columns nearest the anchor are a window of the sorted list, grown one column at a time
 * toward whichever side is nearer; at equal distance the one on the left, the smaller position,
 * goes first.
 */
static void _columns_prepare(place_scene_t *scene, const int level)
{
  if(scene->columns_ready[level]) return;
  scene->columns_ready[level] = TRUE;
  const size_t count = scene->masked_count[level];
  const double width = scene->width;
  const double air = scene->air;
  const double first = scene->view.x0;
  const size_t positions = (size_t)(scene->view.x1 - width - first) + 1;
  guint8 *marks = g_new0(guint8, positions);
  _column_mark(scene, marks, scene->anchor_x - width * 0.5, PLACE_COLUMN_MARKED);
  _column_mark(scene, marks, scene->object.x0, PLACE_COLUMN_MARKED);
  _column_mark(scene, marks, scene->object.x1 - width, PLACE_COLUMN_MARKED);
  _column_mark(scene, marks, scene->object.x1 + air, PLACE_COLUMN_MARKED);
  _column_mark(scene, marks, scene->object.x0 - air - width, PLACE_COLUMN_MARKED);
  _column_mark(scene, marks, scene->view.x0, PLACE_COLUMN_MARKED | PLACE_COLUMN_LEFT_STOP);
  _column_mark(scene, marks, scene->view.x1 - width, PLACE_COLUMN_MARKED);
  if(!IS_NULL_PTR(scene->previous))
  {
    // A placement searched again can stay exactly where it was, or follow the object exactly, and
    // pay nothing for it: without these two columns neither spot was on the list, so a motion of a
    // pixel moved the properties by a pixel and a scene that had not changed at all could move them.
    _column_mark(scene, marks, scene->previous->strip.x, PLACE_COLUMN_MARKED);
    _column_mark(scene, marks, scene->previous->strip.x + scene->previous_shift_x, PLACE_COLUMN_MARKED);
  }
  for(size_t idx = 0; idx < count; idx++)
  {
    // The masked boxes are grown by the air already.
    _column_mark(scene, marks, scene->masked[level][idx].x1, PLACE_COLUMN_MARKED | PLACE_COLUMN_LEFT_STOP);
    _column_mark(scene, marks, scene->masked[level][idx].x0 - width, PLACE_COLUMN_MARKED);
  }
  size_t column_count = 0;
  for(size_t position = 0; position < positions; position++)
    if(marks[position]) column_count++;
  place_column_t *columns = g_new(place_column_t, column_count + 1);
  size_t column = 0;
  // The first column right of the anchor's own: left of it the distances fall, right of it they grow.
  size_t split = column_count;
  for(size_t position = 0; position < positions; position++)
  {
    if(!marks[position]) continue;
    const double x = first + (double)position;
    const double offset = x + width * 0.5 - scene->anchor_x;
    columns[column].x = x;
    columns[column].key = fabs(offset);
    columns[column].left_stop = (marks[position] & PLACE_COLUMN_LEFT_STOP) != 0;
    columns[column].run = 0.0;
    if(split == column_count && offset > 0.0) split = column;
    column++;
  }
  size_t nearest_first = split;
  size_t nearest_last = split;
  while(nearest_last - nearest_first < PLACE_NEAREST_COLUMNS && (nearest_first > 0 || nearest_last < column_count))
  {
    if(nearest_first == 0)
      nearest_last++;
    else if(nearest_last == column_count)
      nearest_first--;
    else if(columns[nearest_first - 1].key <= columns[nearest_last].key)
      nearest_first--;
    else
      nearest_last++;
  }
  dt_free(marks);
  scene->level_bound[level] = _columns_bound(scene, level);
  scene->columns[level] = columns;
  scene->column_count[level] = column_count;
  scene->nearest_first[level] = nearest_first;
  scene->nearest_last[level] = nearest_last;
}

static gboolean _better(const place_best_t *best, const double cost, const double y, const double x,
                        const dt_canvas_place_growth_t growth)
{
  if(!best->found) return TRUE;
  if(cost != best->cost) return cost < best->cost;
  if(y != best->y) return y < best->y;
  if(x != best->x) return x < best->x;
  return growth < best->growth;
}

/** The least free height a column must have for a rung: the strip, and the card's least when it is asked for. */
static double _rung_height(const place_scene_t *scene, const gboolean with_card)
{
  return scene->strip_height + (with_card ? MIN(scene->card_min, scene->card_want) : 0.0);
}

/** Every top worth trying in one free stretch, moved into it: never out of it, never onto the object. */
static void _evaluate_column(place_scene_t *scene, const double x, const int level, const gboolean with_card,
                             place_best_t *best)
{
  const size_t span_count = _spans(scene, x, level);
  const double strip_height = scene->strip_height;
  const double air = scene->air;
  double wanted[8] = { scene->object.y1 + air,
                       scene->object.y0 - air - strip_height,
                       scene->anchor_y + air,
                       scene->anchor_y - air - strip_height,
                       0.0,
                       0.0,
                       0.0,
                       0.0 };
  int wanted_count = 4;
  if(!IS_NULL_PTR(scene->previous))
  {
    // Where the properties are, and where following the object takes them: see the columns.
    wanted[4] = scene->previous->strip.y;
    wanted[5] = scene->previous->strip.y + scene->previous_shift_y;
    wanted_count = 6;
  }
  for(size_t span = 0; span < span_count; span++)
  {
    const double low = scene->spans[2 * span];
    const double high = scene->spans[2 * span + 1];
    double tops[10];
    int top_count = 0;
    for(int idx = 0; idx < wanted_count + 2; idx++)
    {
      // The span's own two ends come last, after what the scene asks for.
      double wanted_top = high - strip_height;
      if(idx < wanted_count)
        wanted_top = wanted[idx];
      else if(idx == wanted_count)
        wanted_top = low;
      const double top = CLAMP(_round_half_up(wanted_top), low, high - strip_height);
      gboolean seen = FALSE;
      for(int other = 0; other < top_count && !seen; other++) seen = tops[other] == top;
      if(seen) continue;
      tops[top_count] = top;
      top_count++;
    }
    for(int idx = 0; idx < top_count; idx++)
    {
      const double y = tops[idx];
      const place_box_t strip = _strip_box(scene, x, y);
      const double free_down = high - strip.y1;
      const double free_up = strip.y0 - low;
      for(int growth = DT_CANVAS_PLACE_DOWN; growth <= DT_CANVAS_PLACE_UP; growth++)
      {
        const double room = growth == DT_CANVAS_PLACE_DOWN ? free_down : free_up;
        const double card = MIN(scene->card_want, room);
        if(with_card && card < MIN(scene->card_min, scene->card_want)) continue;
        const double shown = with_card ? card : 0.0;
        const place_box_t footprint = _footprint_box(&strip, (dt_canvas_place_growth_t)growth, shown);
        const double cost = _cost(scene, &strip, &footprint, level, card, free_down, free_up);
        if(!_better(best, cost, y, x, (dt_canvas_place_growth_t)growth)) continue;
        best->found = TRUE;
        best->cost = cost;
        best->x = x;
        best->y = y;
        best->growth = (dt_canvas_place_growth_t)growth;
        best->card = shown;
      }
    }
  }
}

/**
 * Search one rung of the ladder. The columns nearest the anchor go first and usually settle it;
 * the left stops among the rest are searched only when those hold nothing, so a rung is given up
 * only once no placement clear at its level exists anywhere -- which is what lets a looser rung
 * stand for "nothing clear exists". The stop a clear placement slides to is never right of where
 * it started, so it is inside the view's range and clamping the stops cannot have moved it. A
 * column whose longest free stretch is shorter than the rung asks for holds nothing, and is not
 * walked.
 */
static gboolean _search(place_scene_t *scene, const int level, const gboolean with_card, place_best_t *best)
{
  memset(best, 0, sizeof(*best));
  // The columns are clamped into the view on the promise that the strip fits across it.
  if(scene->width > scene->view.x1 - scene->view.x0) return FALSE;
  if(scene->strip_height > scene->view.y1 - scene->view.y0) return FALSE;
  _columns_prepare(scene, level);
  const place_column_t *columns = scene->columns[level];
  const size_t count = scene->column_count[level];
  const size_t nearest_first = scene->nearest_first[level];
  const size_t nearest_last = scene->nearest_last[level];
  const double need = _rung_height(scene, with_card);
  if(scene->level_bound[level] < need) return FALSE;
  // A level whose runs an earlier rung swept has nothing taller than its longest run, and no column
  // shorter than a rung asks for holds anything: the rungs sharing a level skip what the first learnt.
  const gboolean runs_known = scene->runs_ready[level];
  if(runs_known && scene->level_run[level] < need) return FALSE;
  // The nearest columns first, walked outright: they usually hold the answer, and sweeping the runs
  // of every column to skip a few of these costs more than walking them.
  for(size_t idx = nearest_first; idx < nearest_last; idx++)
  {
    if(runs_known && columns[idx].run < need) continue;
    _evaluate_column(scene, columns[idx].x, level, with_card, best);
  }
  if(best->found) return TRUE;
  if(nearest_last - nearest_first == count) return FALSE;
  _columns_runs(scene, level);
  if(scene->level_run[level] < need) return FALSE;
  for(size_t idx = 0; idx < count; idx++)
  {
    if(idx >= nearest_first && idx < nearest_last) continue;
    if(!columns[idx].left_stop || columns[idx].run < need) continue;
    _evaluate_column(scene, columns[idx].x, level, with_card, best);
  }
  return best->found;
}

static void _result_fill(const place_scene_t *scene, dt_canvas_place_t *result, const place_box_t *strip,
                         const dt_canvas_place_growth_t growth, const double card, const gboolean card_shown,
                         const int level, const double anchor_x, const double anchor_y)
{
  const place_box_t footprint = _footprint_box(strip, growth, card_shown ? card : 0.0);
  result->visible = TRUE;
  result->strip.x = strip->x0;
  result->strip.y = strip->y0;
  result->strip.width = strip->x1 - strip->x0;
  result->strip.height = strip->y1 - strip->y0;
  result->growth = growth;
  result->card_height = card_shown ? card : 0.0;
  result->card_shown = card_shown;
  result->clipped = scene->card_open && !card_shown;
  result->level = (dt_canvas_place_level_t)level;
  result->body = _body_cover(scene, &footprint);
  result->anchor_x = anchor_x;
  result->anchor_y = anchor_y;
}

static gboolean _full(place_scene_t *scene, const dt_canvas_place_reason_t reason, dt_canvas_place_t *result)
{
  if(scene->width > scene->view.x1 - scene->view.x0) return FALSE;
  if(scene->strip_height > scene->view.y1 - scene->view.y0) return FALSE;
  // An open card is kept for as long as a level clear of the body holds it, and only then given up
  // for the strip alone. It goes over the body only when the user has just asked for it: after a
  // pan or a refill it is clipped instead.
  static const place_rung_t grow_ladder[6]
      = { { DT_CANVAS_PLACE_LEVEL_HPB, TRUE },  { DT_CANVAS_PLACE_LEVEL_HB, TRUE },
          { DT_CANVAS_PLACE_LEVEL_H, TRUE },    { DT_CANVAS_PLACE_LEVEL_HPB, FALSE },
          { DT_CANVAS_PLACE_LEVEL_HB, FALSE },  { DT_CANVAS_PLACE_LEVEL_H, FALSE } };
  static const place_rung_t card_ladder[5]
      = { { DT_CANVAS_PLACE_LEVEL_HPB, TRUE },  { DT_CANVAS_PLACE_LEVEL_HB, TRUE },
          { DT_CANVAS_PLACE_LEVEL_HPB, FALSE }, { DT_CANVAS_PLACE_LEVEL_HB, FALSE },
          { DT_CANVAS_PLACE_LEVEL_H, FALSE } };
  static const place_rung_t strip_ladder[3]
      = { { DT_CANVAS_PLACE_LEVEL_HPB, FALSE }, { DT_CANVAS_PLACE_LEVEL_HB, FALSE },
          { DT_CANVAS_PLACE_LEVEL_H, FALSE } };
  const place_rung_t *ladder = strip_ladder;
  int rungs = 3;
  if(scene->card_open && reason == DT_CANVAS_PLACE_GROW)
  {
    ladder = grow_ladder;
    rungs = 6;
  }
  else if(scene->card_open)
  {
    ladder = card_ladder;
    rungs = 5;
  }
  for(int rung = 0; rung < rungs; rung++)
  {
    place_best_t best;
    if(!_search(scene, ladder[rung].level, ladder[rung].with_card, &best)) continue;
    const place_box_t strip = _strip_box(scene, best.x, best.y);
    _result_fill(scene, result, &strip, best.growth, best.card, ladder[rung].with_card, ladder[rung].level,
                 scene->anchor_x, scene->anchor_y);
    return TRUE;
  }
  return FALSE;
}

/** The previous strip, at the widget's current size. */
static place_box_t _previous_strip(const place_scene_t *scene, const double shift_x, const double shift_y)
{
  const dt_canvas_place_t *previous = scene->previous;
  return _strip_box(scene, previous->strip.x + shift_x, previous->strip.y + shift_y);
}

/** Fit the card on either side of a strip that stays where it is: its side first, then the other. */
static gboolean _fit_card(const place_scene_t *scene, const place_box_t *strip, const double low, const double high,
                          const dt_canvas_place_growth_t preferred, dt_canvas_place_growth_t *growth, double *card)
{
  const double free_down = high - strip->y1;
  const double free_up = strip->y0 - low;
  const dt_canvas_place_growth_t other = preferred == DT_CANVAS_PLACE_DOWN ? DT_CANVAS_PLACE_UP : DT_CANVAS_PLACE_DOWN;
  const dt_canvas_place_growth_t order[2] = { preferred, other };
  for(int idx = 0; idx < 2; idx++)
  {
    const double room = order[idx] == DT_CANVAS_PLACE_DOWN ? free_down : free_up;
    if(room < scene->card_want) continue;
    *growth = order[idx];
    *card = scene->card_want;
    return TRUE;
  }
  // Neither side holds all of it: the roomier side, scrolling, if it holds the card's least.
  dt_canvas_place_growth_t roomier = preferred;
  if(free_down > free_up) roomier = DT_CANVAS_PLACE_DOWN;
  if(free_up > free_down) roomier = DT_CANVAS_PLACE_UP;
  const double room = roomier == DT_CANVAS_PLACE_DOWN ? free_down : free_up;
  if(room < scene->card_min) return FALSE;
  *growth = roomier;
  *card = room;
  return TRUE;
}

/**
 * Is there anywhere clear of the body at all, for the strip or for the strip and the card's least?
 * A placement over the body is only ever right for as long as the answer is no: the ladder covers
 * the body only after every level clear of it has failed, and keeping or growing one that covers
 * it must not skip what the ladder would have found. HB is enough to ask: HPB masks more.
 */
static gboolean _body_free_exists(place_scene_t *scene, const gboolean with_card)
{
  place_best_t probe;
  return _search(scene, DT_CANVAS_PLACE_LEVEL_HB, with_card, &probe);
}

/** GROW: the strip stays exactly where it is and the card finds its side. */
static gboolean _grow(place_scene_t *scene, dt_canvas_place_t *result)
{
  const dt_canvas_place_t *previous = scene->previous;
  if(IS_NULL_PTR(previous) || !scene->card_open) return FALSE;
  if(previous->level == DT_CANVAS_PLACE_LEVEL_H && _body_free_exists(scene, TRUE)) return FALSE;
  const place_box_t strip = _previous_strip(scene, 0.0, 0.0);
  double low = 0.0;
  double high = 0.0;
  if(!_span_holding(scene, &strip, previous->level, &low, &high)) return FALSE;
  dt_canvas_place_growth_t growth = previous->growth;
  double card = 0.0;
  if(!_fit_card(scene, &strip, low, high, previous->growth, &growth, &card)) return FALSE;
  _result_fill(scene, result, &strip, growth, card, TRUE, previous->level, previous->anchor_x, previous->anchor_y);
  return TRUE;
}

/**
 * A strip kept at the previous placement's level: clear at that level, with the card fitted to it
 * when `with_card` asks for one, and -- over the body -- covering no more of it than before.
 */
static gboolean _keep_strip(place_scene_t *scene, const place_box_t *strip, const gboolean with_card,
                            dt_canvas_place_growth_t *growth, double *card)
{
  const dt_canvas_place_t *previous = scene->previous;
  double low = 0.0;
  double high = 0.0;
  if(!_span_holding(scene, strip, previous->level, &low, &high)) return FALSE;
  *growth = previous->growth;
  *card = 0.0;
  if(with_card && !_fit_card(scene, strip, low, high, previous->growth, growth, card)) return FALSE;
  if(previous->level != DT_CANVAS_PLACE_LEVEL_H) return TRUE;
  const place_box_t footprint = _footprint_box(strip, *growth, *card);
  return _body_cover(scene, &footprint) <= previous->body + PLACE_BODY_SLACK;
}

/**
 * KEEP: the properties stay put for a small motion and follow the object rigidly for a larger
 * one, as long as they are still clear at the level they were found at.
 *
 * A placement over the body is kept like any other, but only while nothing clear of the body has
 * opened -- the room a pan, a zoom or a refill makes is exactly what it was missing -- and only
 * while it covers no more of the body than it did. Refusing it outright looked safer and was not:
 * the search it fell back to had no candidate where the properties already were, so a motion of a
 * pixel moved them by a pixel, and a scene that had not moved at all moved them too, whenever the
 * opening had avoided the pointer that asked for them.
 *
 * The card goes over the body only where the user put it there, with an explicit GROW: kept, it
 * stays; closed when the properties went over the body, it is not grown there by a pan.
 */
static gboolean _keep(place_scene_t *scene, dt_canvas_place_t *result)
{
  const dt_canvas_place_t *previous = scene->previous;
  if(IS_NULL_PTR(previous)) return FALSE;
  const gboolean over_body = previous->level == DT_CANVAS_PLACE_LEVEL_H;
  const gboolean with_card = scene->card_open && (!over_body || previous->card_shown);
  if(over_body && _body_free_exists(scene, with_card)) return FALSE;
  const double motion_x = scene->anchor_x - _snap_fine(previous->anchor_x);
  const double motion_y = scene->anchor_y - _snap_fine(previous->anchor_y);
  dt_canvas_place_growth_t growth = previous->growth;
  double card = 0.0;
  if(fabs(motion_x) <= PLACE_HYSTERESIS_PIXELS && fabs(motion_y) <= PLACE_HYSTERESIS_PIXELS)
  {
    // The anchor stays the one the strip was put at, so motions too small to move it add up.
    const place_box_t strip = _previous_strip(scene, 0.0, 0.0);
    // The card is fitted again first, so a card that was scrolling or clipped grows back the moment
    // there is room, whether or not the object happened to move past the threshold.
    if(_keep_strip(scene, &strip, with_card, &growth, &card))
    {
      _result_fill(scene, result, &strip, growth, card, with_card, previous->level, previous->anchor_x,
                   previous->anchor_y);
      return TRUE;
    }
    // Where it no longer fits, the card as it was shown -- capped, or clipped -- stays if it is still clear.
    const gboolean shown = with_card && previous->card_shown;
    const double kept_card = shown ? MIN(previous->card_height, scene->card_want) : 0.0;
    const place_box_t footprint = _footprint_box(&strip, previous->growth, kept_card);
    const gboolean body_kept = !over_body || _body_cover(scene, &footprint) <= previous->body + PLACE_BODY_SLACK;
    if(_valid(scene, &footprint, previous->level) && body_kept)
    {
      _result_fill(scene, result, &strip, previous->growth, kept_card, shown, previous->level, previous->anchor_x,
                   previous->anchor_y);
      return TRUE;
    }
  }
  // Whole pixels, and the anchor moves by exactly as much, so a fraction left over is carried to
  // the next motion instead of lost.
  const double anchor_x = previous->anchor_x + scene->previous_shift_x;
  const double anchor_y = previous->anchor_y + scene->previous_shift_y;
  const place_box_t strip = _previous_strip(scene, scene->previous_shift_x, scene->previous_shift_y);
  // An open card that no longer fits beside the strip is searched for: the card ladder finds it a
  // spot clear of the body before it gives the card up, and the movement cost keeps that spot near.
  if(!_keep_strip(scene, &strip, with_card, &growth, &card)) return FALSE;
  _result_fill(scene, result, &strip, growth, card, with_card, previous->level, anchor_x, anchor_y);
  return TRUE;
}

/**
 * A scene with a coordinate that is not a number has no pixel to snap to, and a view starting further
 * out than PLACE_COORDINATE_MAX has pixels a double no longer tells apart, so the positions bucketed
 * by pixel could land outside their buckets: nothing is placed in either.
 */
static gboolean _input_finite(const dt_canvas_place_input_t *input)
{
  const gboolean finite = isfinite(input->view.x) && isfinite(input->view.y) && isfinite(input->view.width)
                          && isfinite(input->view.height) && isfinite(input->air) && isfinite(input->width)
                          && isfinite(input->strip_height) && isfinite(input->anchor_x) && isfinite(input->anchor_y);
  return finite && fabs(input->view.x) <= PLACE_COORDINATE_MAX && fabs(input->view.y) <= PLACE_COORDINATE_MAX;
}

void dt_canvas_place_solve(const dt_canvas_place_input_t *input, dt_canvas_place_t *result)
{
  if(IS_NULL_PTR(result)) return;
  memset(result, 0, sizeof(*result));
  if(IS_NULL_PTR(input)) return;
  result->anchor_x = input->anchor_x;
  result->anchor_y = input->anchor_y;
  // An anchor read from a corrupt object is NaN: every clamp lets it through, every comparison
  // with it is false, and the first candidate -- costing NaN -- could never be replaced, so a
  // NaN strip was returned as visible and cast to an int by the view.
  if(!_input_finite(input)) return;
  place_scene_t scene;
  _scene_init(&scene, input);
  gboolean placed = FALSE;
  if(input->reason == DT_CANVAS_PLACE_GROW) placed = _grow(&scene, result);
  if(input->reason == DT_CANVAS_PLACE_RESOLVE) placed = _keep(&scene, result);
  if(!placed) placed = _full(&scene, input->reason, result);
  if(!placed)
  {
    memset(result, 0, sizeof(*result));
    result->anchor_x = input->anchor_x;
    result->anchor_y = input->anchor_y;
  }
  _scene_cleanup(&scene);
}

dt_canvas_place_rect_t dt_canvas_place_footprint(const dt_canvas_place_t *place)
{
  dt_canvas_place_rect_t footprint = { 0.0, 0.0, 0.0, 0.0 };
  if(IS_NULL_PTR(place)) return footprint;
  footprint = place->strip;
  if(!place->card_shown) return footprint;
  footprint.height += place->card_height;
  if(place->growth == DT_CANVAS_PLACE_UP) footprint.y -= place->card_height;
  return footprint;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
