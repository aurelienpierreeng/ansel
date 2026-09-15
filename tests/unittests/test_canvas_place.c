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

/* Where the floating properties go. The solver is checked against a brute force that knows
 * nothing of spans, columns, runs or ladders: it marks every pixel each shape keeps clear and asks,
 * for every position in the view, whether a rectangle fits there. What the solver claims -- that a
 * placement covers no handle, that it went over the body only because nothing clear existed, that
 * the card was given up only because it fitted nowhere -- is then a statement about every pixel,
 * not about the candidates the solver chose to look at.
 *
 * The brute force snaps the scene the way the solver documents it does -- the view inward, every
 * shape outward, the air and the widget up -- since "nothing fits" is a statement about that
 * snapped scene. What a placement promises about the real one, clearance and containment, is
 * checked against the raw numbers. */

#include "canvas/canvas.h"
#include "canvas/canvas_handles.h"
#include "canvas/canvas_place.h"
#include "canvas/canvas_place_shapes.h"

#include <glib.h>
#include <math.h>
// cmocka.h declares `extern jmp_buf global_expect_assert_env' without including <setjmp.h>.
#include <setjmp.h>  // NOLINT(misc-include-cleaner)
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <cmocka.h>

#define FUZZ_SCENES 10000
#define MAX_SHAPES 256

/** A reproducible sequence: the same scenes on every run and every machine. */
static uint64_t _random_state = 0x2545F4914F6CDD1Dull;

static double _random_unit(void)
{
  _random_state ^= _random_state << 13;
  _random_state ^= _random_state >> 7;
  _random_state ^= _random_state << 17;
  return (double)(_random_state >> 11) / (double)(1ull << 53);
}

/** A whole number in [low, high]. */
static int _random_int(const int low, const int high)
{
  return low + (int)floor(_random_unit() * (high - low + 1));
}

typedef struct scene_t
{
  dt_canvas_place_input_t input;
  dt_canvas_place_shape_t shapes[MAX_SHAPES];
} scene_t;

static void _shape_add(scene_t *scene, const dt_canvas_place_class_t shape_class, const double x, const double y,
                       const double width, const double height, const gboolean target)
{
  assert_true(scene->input.shape_count < MAX_SHAPES);
  dt_canvas_place_shape_t *shape = &scene->shapes[scene->input.shape_count];
  shape->rect.x = x;
  shape->rect.y = y;
  shape->rect.width = width;
  shape->rect.height = height;
  shape->shape_class = shape_class;
  shape->weight = shape_class == DT_CANVAS_PLACE_SOFT ? DT_CANVAS_PLACE_WEIGHT_FRAME : 0.0;
  shape->target = target;
  scene->input.shape_count++;
  scene->input.shapes = scene->shapes;
}

static void _scene_defaults(scene_t *scene)
{
  memset(scene, 0, sizeof(*scene));
  scene->input.shapes = scene->shapes;
  scene->input.card_min = 120.0;
  scene->input.reason = DT_CANVAS_PLACE_OPEN;
}

/** Copy a scene, keeping the copy's shapes pointer on its own array. */
static void _scene_copy(scene_t *copy, const scene_t *scene)
{
  memcpy(copy, scene, sizeof(*copy));
  copy->input.shapes = copy->shapes;
}

/** Up to one pixel more, a quarter of the time: the fractions the view's DPI scaling brings. */
static double _maybe_fraction(const gboolean fractional)
{
  return fractional ? _random_unit() : 0.0;
}

/** A random scene small enough to brute force. */
static void _scene_random(scene_t *scene)
{
  _scene_defaults(scene);
  const gboolean fractional = _random_unit() < 0.25;
  const int view_x = _random_int(0, 40);
  const int view_y = _random_int(0, 40);
  const int view_width = _random_int(60, 200);
  const int view_height = _random_int(50, 150);
  scene->input.view = (dt_canvas_place_rect_t){ view_x + _maybe_fraction(fractional),
                                                view_y + _maybe_fraction(fractional),
                                                view_width - _maybe_fraction(fractional),
                                                view_height - _maybe_fraction(fractional) };
  scene->input.air = _random_int(0, 6) + _maybe_fraction(fractional);
  scene->input.width = _random_int(10, 120) - _maybe_fraction(fractional);
  scene->input.strip_height = _random_int(6, 36) - _maybe_fraction(fractional);
  // The object: a box its body slabs tile, its handles around and on it.
  const int object_x = view_x + _random_int(-30, view_width);
  const int object_y = view_y + _random_int(-30, view_height);
  const int object_width = _random_int(4, 120);
  const int object_height = _random_int(4, 100);
  const gboolean has_body = _random_unit() < 0.8;
  if(has_body)
  {
    const int slabs = _random_int(1, 8);
    for(int idx = 0; idx < slabs; idx++)
    {
      const int slab_y = object_y + idx * object_height / slabs;
      const int slab_height = MAX(object_height / slabs, 1);
      const int inset = _random_int(0, object_width / 4);
      _shape_add(scene, DT_CANVAS_PLACE_BODY, object_x + inset + _maybe_fraction(fractional), slab_y,
                 MAX(object_width - 2 * inset, 1), slab_height, TRUE);
    }
  }
  const int hard = _random_int(0, 12);
  for(int idx = 0; idx < hard; idx++)
    _shape_add(scene, DT_CANVAS_PLACE_HARD, object_x + _random_int(-40, object_width + 40) + _maybe_fraction(fractional),
               object_y + _random_int(-40, object_height + 40) - _maybe_fraction(fractional), _random_int(1, 24),
               _random_int(1, 24) + _maybe_fraction(fractional), _random_unit() < 0.7);
  const int predicted = _random_int(0, 8);
  for(int idx = 0; idx < predicted; idx++)
    _shape_add(scene, DT_CANVAS_PLACE_PREDICTED, view_x + _random_int(-10, view_width),
               view_y + _random_int(-10, view_height) + _maybe_fraction(fractional), _random_int(1, 40),
               _random_int(1, 40), FALSE);
  const int soft = _random_int(0, 5);
  for(int idx = 0; idx < soft; idx++)
    _shape_add(scene, DT_CANVAS_PLACE_SOFT, view_x + _random_int(-20, view_width),
               view_y + _random_int(-20, view_height), _random_int(4, 80), _random_int(4, 80), FALSE);
  // A few scenes are crowded with handles, so every rung of the ladder gets exercised.
  if(_random_unit() < 0.15)
  {
    const int crowd = _random_int(8, 16);
    for(int idx = 0; idx < crowd && scene->input.shape_count < MAX_SHAPES; idx++)
      _shape_add(scene, DT_CANVAS_PLACE_HARD, view_x + _random_int(-10, view_width),
                 view_y + _random_int(-10, view_height), _random_int(2, 30), _random_int(2, 30), FALSE);
  }
  scene->input.anchor_x = object_x + _random_unit() * object_width;
  scene->input.anchor_y = object_y + _random_unit() * object_height;
  scene->input.has_press = _random_unit() < 0.5;
  scene->input.press_x = view_x + _random_unit() * view_width;
  scene->input.press_y = view_y + _random_unit() * view_height;
  scene->input.card_open = _random_unit() < 0.3;
  scene->input.card_content_height = _random_int(0, 160) + _maybe_fraction(fractional);
  scene->input.card_min = _random_int(4, 60) - _maybe_fraction(fractional);
  scene->input.last_card_height = _random_unit() < 0.5 ? 0.0 : _random_int(0, 120);
}

/**
 * A scene with far more columns than the solver searches first: a narrow strip on a wide view,
 * and a crowd of thin handles around the anchor that usually fills the nearest columns, so the
 * columns further out -- and the runs that decide which of them are walked -- are what finds room.
 */
static void _scene_random_crowded(scene_t *scene)
{
  _scene_defaults(scene);
  const int view_width = _random_int(300, 600);
  const int view_height = _random_int(60, 140);
  scene->input.view = (dt_canvas_place_rect_t){ 0.0, 0.0, view_width, view_height };
  scene->input.air = _random_int(0, 2);
  scene->input.width = _random_int(6, 30);
  scene->input.strip_height = _random_int(6, 24);
  const int anchor_x = _random_int(0, view_width);
  const int anchor_y = _random_int(0, view_height);
  const int spread = _random_int(40, view_width);
  const int crowd = _random_int(120, 230);
  for(int idx = 0; idx < crowd; idx++)
  {
    const double pick = _random_unit();
    const dt_canvas_place_class_t shape_class
        = pick < 0.6 ? DT_CANVAS_PLACE_HARD : (pick < 0.8 ? DT_CANVAS_PLACE_PREDICTED : DT_CANVAS_PLACE_BODY);
    _shape_add(scene, shape_class, anchor_x + _random_int(-spread / 2, spread / 2), _random_int(-10, view_height),
               _random_int(1, 5), _random_int(8, view_height), _random_unit() < 0.5);
  }
  // Bands across the whole view, half the time: what a body wider than the view is, and what lets
  // a level with no room anywhere be given up without walking a column.
  const int bands = _random_unit() < 0.5 ? _random_int(1, 4) : 0;
  for(int idx = 0; idx < bands; idx++)
  {
    const double pick = _random_unit();
    const dt_canvas_place_class_t shape_class
        = pick < 0.3 ? DT_CANVAS_PLACE_HARD : (pick < 0.6 ? DT_CANVAS_PLACE_PREDICTED : DT_CANVAS_PLACE_BODY);
    _shape_add(scene, shape_class, _random_int(-40, 0), _random_int(-10, view_height),
               view_width + _random_int(40, 80), _random_int(4, 60), TRUE);
  }
  scene->input.anchor_x = anchor_x;
  scene->input.anchor_y = anchor_y;
  scene->input.card_open = _random_unit() < 0.3;
  scene->input.card_content_height = _random_int(0, 80);
  scene->input.card_min = _random_int(2, 30);
}

/* --- the oracle ---------------------------------------------------------------------------- */

/** The scene the way the solver snaps it, and the card heights it derives from that. */
typedef struct snapped_t
{
  int view_x0;
  int view_y0;
  int view_x1;
  int view_y1;
  int air;
  int width;
  int strip_height;
  int card;                ///< the card a card rung places: all of it, or all the view has
  gboolean card_placeable; ///< the card is open and the view shows at least its least
} snapped_t;

static snapped_t _snap(const scene_t *scene)
{
  const dt_canvas_place_input_t *input = &scene->input;
  snapped_t snapped;
  snapped.view_x0 = (int)ceil(input->view.x);
  snapped.view_y0 = (int)ceil(input->view.y);
  snapped.view_x1 = (int)floor(input->view.x + input->view.width);
  snapped.view_y1 = (int)floor(input->view.y + input->view.height);
  snapped.air = (int)ceil(MAX(input->air, 0.0));
  snapped.width = (int)ceil(MAX(input->width, 0.0));
  snapped.strip_height = (int)ceil(MAX(input->strip_height, 0.0));
  const double view_height = MAX(snapped.view_y1 - snapped.view_y0, 0);
  const double whole = ceil(MAX(input->card_content_height, 0.0));
  const double want = input->card_open ? MIN(whole, MAX(view_height - snapped.strip_height, 0.0)) : 0.0;
  snapped.card = (int)want;
  snapped.card_placeable = input->card_open && want >= MIN(ceil(MAX(input->card_min, 0.0)), whole);
  return snapped;
}

/** Does a rectangle overlap a shape once the shape is grown by the air? Touching is not overlapping. */
static gboolean _hits(const dt_canvas_place_rect_t *rect, const dt_canvas_place_shape_t *shape, const double air)
{
  return rect->x < shape->rect.x + shape->rect.width + air && rect->x + rect->width > shape->rect.x - air
         && rect->y < shape->rect.y + shape->rect.height + air && rect->y + rect->height > shape->rect.y - air;
}

static gboolean _hits_class(const scene_t *scene, const dt_canvas_place_rect_t *rect,
                            const dt_canvas_place_class_t shape_class)
{
  for(size_t idx = 0; idx < scene->input.shape_count; idx++)
  {
    if(scene->shapes[idx].shape_class != shape_class) continue;
    if(_hits(rect, &scene->shapes[idx], scene->input.air)) return TRUE;
  }
  return FALSE;
}

static gboolean _level_keeps_clear(const int level, const dt_canvas_place_class_t shape_class)
{
  if(shape_class == DT_CANVAS_PLACE_HARD) return TRUE;
  if(shape_class == DT_CANVAS_PLACE_BODY) return level <= DT_CANVAS_PLACE_LEVEL_HB;
  if(shape_class == DT_CANVAS_PLACE_PREDICTED) return level == DT_CANVAS_PLACE_LEVEL_HPB;
  return FALSE;
}

/**
 * Is there ANY whole-pixel position in the snapped view where a width x height rectangle is clear
 * of what a level keeps clear? Every pixel a snapped, grown shape overlaps is marked through a
 * difference table, and a summed-area table answers each position.
 */
static gboolean _exists(const scene_t *scene, const int level, const int width, const int height)
{
  const snapped_t snapped = _snap(scene);
  const int columns = snapped.view_x1 - snapped.view_x0;
  const int rows = snapped.view_y1 - snapped.view_y0;
  if(width > columns || height > rows || columns <= 0 || rows <= 0) return FALSE;
  const int stride = columns + 1;
  int *marks = g_new0(int, (size_t)stride * (rows + 1));
  for(size_t idx = 0; idx < scene->input.shape_count; idx++)
  {
    const dt_canvas_place_shape_t *shape = &scene->shapes[idx];
    if(!_level_keeps_clear(level, shape->shape_class)) continue;
    const double left = shape->rect.x;
    const double right = shape->rect.x + shape->rect.width;
    const double top = shape->rect.y;
    const double bottom = shape->rect.y + shape->rect.height;
    if(!isfinite(left) || !isfinite(right) || !isfinite(top) || !isfinite(bottom)) continue;
    // A pixel [c, c + 1) overlaps the grown shape (x0 - air, x1 + air) for c from x0 - air to x1 + air - 1.
    const int column_first = MAX((int)floor(MIN(left, right)) - snapped.air - snapped.view_x0, 0);
    const int column_last = MIN((int)ceil(MAX(left, right)) + snapped.air - snapped.view_x0, columns);
    const int row_first = MAX((int)floor(MIN(top, bottom)) - snapped.air - snapped.view_y0, 0);
    const int row_last = MIN((int)ceil(MAX(top, bottom)) + snapped.air - snapped.view_y0, rows);
    if(column_first >= column_last || row_first >= row_last) continue;
    marks[row_first * stride + column_first]++;
    marks[row_first * stride + column_last]--;
    marks[row_last * stride + column_first]--;
    marks[row_last * stride + column_last]++;
  }
  // The difference table summed is how many shapes cover each pixel; its own summed-area table of
  // "covered or not" is then read once per position.
  int *sums = g_new0(int, (size_t)stride * (rows + 1));
  int *covered = g_new0(int, (size_t)stride);
  for(int row = 0; row < rows; row++)
  {
    int along = 0;
    for(int column = 0; column < columns; column++)
    {
      along += marks[row * stride + column];
      covered[column] += along;
      const int blocked = covered[column] > 0 ? 1 : 0;
      sums[(row + 1) * stride + column + 1]
          = blocked + sums[row * stride + column + 1] + sums[(row + 1) * stride + column] - sums[row * stride + column];
    }
  }
  gboolean found = FALSE;
  for(int row = 0; row + height <= rows && !found; row++)
  {
    for(int column = 0; column + width <= columns && !found; column++)
    {
      const int bottom = row + height;
      const int right = column + width;
      const int blocked = sums[bottom * stride + right] - sums[row * stride + right] - sums[bottom * stride + column]
                          + sums[row * stride + column];
      found = blocked == 0;
    }
  }
  g_free(covered);
  g_free(sums);
  g_free(marks);
  return found;
}

static gboolean _same(const dt_canvas_place_t *first, const dt_canvas_place_t *second)
{
  return first->visible == second->visible && first->strip.x == second->strip.x && first->strip.y == second->strip.y
         && first->strip.width == second->strip.width && first->strip.height == second->strip.height
         && first->growth == second->growth && first->card_height == second->card_height
         && first->card_shown == second->card_shown && first->clipped == second->clipped
         && first->level == second->level && first->body == second->body && first->anchor_x == second->anchor_x
         && first->anchor_y == second->anchor_y;
}

/**
 * Everything a placement promises, against the brute force. `previous` is what a RESOLVE or a GROW
 * started from. `ladder` is TRUE when the ladder's own order is owed: an OPEN, or a GROW whose
 * previous placement was solved on this same scene. A RESOLVE may keep a placement at the level it
 * was found at, so only what it may never do is checked.
 */
static void _check(const scene_t *scene, const dt_canvas_place_t *place, const dt_canvas_place_t *previous,
                   const gboolean ladder)
{
  const dt_canvas_place_input_t *input = &scene->input;
  const snapped_t snapped = _snap(scene);
  const int width = snapped.width;
  const int height = snapped.strip_height;
  const int card_whole = snapped.card;
  if(!place->visible)
  {
    // Nothing fits means nothing fits: not even over the body.
    assert_false(_exists(scene, DT_CANVAS_PLACE_LEVEL_H, width, height));
    assert_false(place->card_shown);
    return;
  }
  const dt_canvas_place_rect_t *view = &input->view;
  assert_true(place->strip.width == width && place->strip.width >= input->width);
  assert_true(place->strip.height == height && place->strip.height >= input->strip_height);
  assert_true(place->strip.x == floor(place->strip.x) && place->strip.y == floor(place->strip.y));
  assert_int_equal(place->clipped, input->card_open && !place->card_shown);
  // The footprint, built from what the placement says, never taken from the function under test.
  const double card = place->card_shown ? place->card_height : 0.0;
  if(!place->card_shown) assert_true(place->card_height == 0.0);
  const dt_canvas_place_rect_t footprint = { place->strip.x,
                                             place->growth == DT_CANVAS_PLACE_UP ? place->strip.y - card
                                                                                 : place->strip.y,
                                             place->strip.width, place->strip.height + card };
  const dt_canvas_place_rect_t reported = dt_canvas_place_footprint(place);
  assert_true(reported.x == footprint.x && reported.y == footprint.y && reported.width == footprint.width
              && reported.height == footprint.height);
  assert_true(footprint.x >= view->x && footprint.y >= view->y);
  assert_true(footprint.x + footprint.width <= view->x + view->width);
  assert_true(footprint.y + footprint.height <= view->y + view->height);
  // Never a handle, at any level.
  assert_false(_hits_class(scene, &footprint, DT_CANVAS_PLACE_HARD));
  if(_hits_class(scene, &footprint, DT_CANVAS_PLACE_PREDICTED)) assert_true(place->level >= DT_CANVAS_PLACE_LEVEL_HB);
  if(_hits_class(scene, &footprint, DT_CANVAS_PLACE_BODY)) assert_int_equal(place->level, DT_CANVAS_PLACE_LEVEL_H);
  if(place->card_shown)
  {
    assert_true(input->card_open);
    assert_true(snapped.card_placeable);
    // All of it, snapped up to a whole pixel, or all the view has: never cut down to the room beside
    // the strip, never less than its least.
    assert_true(place->card_height == card_whole);
    // Independently of how the solver snaps: shorter than its content only when it fills the view.
    assert_true(place->card_height >= input->card_content_height
                || footprint.height == snapped.view_y1 - snapped.view_y0);
    assert_true(place->card_height >= MIN(ceil(MAX(input->card_min, 0.0)), ceil(MAX(input->card_content_height, 0.0))));
    const dt_canvas_place_rect_t card_rect
        = { footprint.x, place->growth == DT_CANVAS_PLACE_UP ? footprint.y : place->strip.y + place->strip.height,
            footprint.width, place->card_height };
    // Only a card the user just opened goes over the body, or one kept where such a GROW put it.
    if(_hits_class(scene, &card_rect, DT_CANVAS_PLACE_BODY))
    {
      const gboolean grown = input->reason == DT_CANVAS_PLACE_GROW;
      const gboolean kept = input->reason == DT_CANVAS_PLACE_RESOLVE && previous != NULL
                            && previous->level == DT_CANVAS_PLACE_LEVEL_H && previous->card_shown;
      assert_true(grown || kept);
    }
  }
  if(place->level == DT_CANVAS_PLACE_LEVEL_H)
  {
    // Over the body only once nothing clear of it holds what is shown -- whatever the reason.
    if(place->card_shown)
      assert_false(_exists(scene, DT_CANVAS_PLACE_LEVEL_HB, width, height + card_whole));
    else
      assert_false(_exists(scene, DT_CANVAS_PLACE_LEVEL_HB, width, height));
  }
  if(!ladder) return;
  // The ladder: a card at HB only once none fits at HPB; the card given up only once it fits
  // nowhere clear of the body -- nowhere at all, on a GROW; a looser level for the strip only once
  // the stricter one has nowhere.
  if(place->card_shown && place->level == DT_CANVAS_PLACE_LEVEL_HB)
    assert_false(_exists(scene, DT_CANVAS_PLACE_LEVEL_HPB, width, height + card_whole));
  if(!place->card_shown && snapped.card_placeable)
  {
    assert_false(_exists(scene, DT_CANVAS_PLACE_LEVEL_HB, width, height + card_whole));
    if(input->reason == DT_CANVAS_PLACE_GROW)
      assert_false(_exists(scene, DT_CANVAS_PLACE_LEVEL_H, width, height + card_whole));
  }
  if(!place->card_shown && place->level >= DT_CANVAS_PLACE_LEVEL_HB)
    assert_false(_exists(scene, DT_CANVAS_PLACE_LEVEL_HPB, width, height));
}

/**
 * Solve, and solve again: on the same input, and on a copy whose shapes come in another order.
 * Both must give exactly the same placement -- the order the view happens to list shapes in is
 * not an input.
 */
static void _solve(const scene_t *scene, dt_canvas_place_t *place)
{
  dt_canvas_place_solve(&scene->input, place);
  dt_canvas_place_t again;
  dt_canvas_place_solve(&scene->input, &again);
  assert_true(_same(place, &again));
  scene_t shuffled;
  _scene_copy(&shuffled, scene);
  for(size_t idx = shuffled.input.shape_count; idx > 1; idx--)
  {
    const size_t other = (size_t)_random_int(0, (int)idx - 1);
    const dt_canvas_place_shape_t swap = shuffled.shapes[idx - 1];
    shuffled.shapes[idx - 1] = shuffled.shapes[other];
    shuffled.shapes[other] = swap;
  }
  dt_canvas_place_t reordered;
  dt_canvas_place_solve(&shuffled.input, &reordered);
  assert_true(_same(place, &reordered));
}

/** Move what a pan moves -- everything but the view and the soft bands -- by a few pixels, a fraction sometimes. */
static void _pan(scene_t *scene)
{
  const double fraction = _random_unit() < 0.3 ? _random_unit() : 0.0;
  const double shift_x = _random_int(-6, 6) + fraction;
  const double shift_y = _random_int(-6, 6) - fraction;
  for(size_t idx = 0; idx < scene->input.shape_count; idx++)
  {
    if(scene->shapes[idx].shape_class == DT_CANVAS_PLACE_SOFT) continue;
    scene->shapes[idx].rect.x += shift_x;
    scene->shapes[idx].rect.y += shift_y;
  }
  scene->input.anchor_x += shift_x;
  scene->input.anchor_y += shift_y;
}

static void _a_placement_keeps_every_promise_in_ten_thousand_scenes(void **state)
{
  (void)state;
  int opened = 0;
  int visible = 0;
  int levels[3] = { 0, 0, 0 };
  int cards = 0;
  int clipped = 0;
  int kept_over_body = 0;
  for(int scene_index = 0; scene_index < FUZZ_SCENES; scene_index++)
  {
    scene_t scene;
    if(_random_unit() < 0.06)
      _scene_random_crowded(&scene);
    else
      _scene_random(&scene);
    const double reason_pick = _random_unit();
    if(reason_pick < 0.4)
    {
      dt_canvas_place_t place;
      _solve(&scene, &place);
      _check(&scene, &place, NULL, TRUE);
      opened++;
      visible += place.visible ? 1 : 0;
      if(place.visible) levels[place.level]++;
      cards += place.card_shown ? 1 : 0;
      clipped += place.clipped ? 1 : 0;
      continue;
    }
    // The card is opened over properties opened with it closed, on the same scene.
    scene_t card_closed;
    _scene_copy(&card_closed, &scene);
    card_closed.input.card_open = FALSE;
    dt_canvas_place_t opening;
    _solve(&card_closed, &opening);
    _check(&card_closed, &opening, NULL, TRUE);
    if(reason_pick < 0.6)
    {
      scene.input.card_open = TRUE;
      scene.input.reason = DT_CANVAS_PLACE_GROW;
      scene.input.previous = &opening;
      dt_canvas_place_t place;
      _solve(&scene, &place);
      _check(&scene, &place, &opening, TRUE);
      cards += place.card_shown ? 1 : 0;
      clipped += place.clipped ? 1 : 0;
      continue;
    }
    // A pan, over the properties as they were opened, or as the card was grown over them.
    dt_canvas_place_t grown = opening;
    if(reason_pick >= 0.8)
    {
      scene_t growing;
      _scene_copy(&growing, &scene);
      growing.input.card_open = TRUE;
      growing.input.reason = DT_CANVAS_PLACE_GROW;
      growing.input.previous = &opening;
      _solve(&growing, &grown);
      _check(&growing, &grown, &opening, TRUE);
      scene.input.card_open = _random_unit() < 0.7;
    }
    else
    {
      // Now and then the card is open, as the properties were placed without it: it is clipped, or
      // shown where it fits, but never grown over the body by a pan.
      scene.input.card_open = _random_unit() < 0.3;
    }
    _pan(&scene);
    scene.input.reason = DT_CANVAS_PLACE_RESOLVE;
    scene.input.previous = &grown;
    dt_canvas_place_t place;
    _solve(&scene, &place);
    _check(&scene, &place, &grown, FALSE);
    if(place.visible && place.card_shown && place.level == DT_CANVAS_PLACE_LEVEL_H) kept_over_body++;
  }
  // The fuzz is only worth its time if it reaches every outcome.
  assert_true(visible > opened / 2);
  assert_true(visible < opened);
  assert_true(levels[DT_CANVAS_PLACE_LEVEL_HPB] > 0);
  assert_true(levels[DT_CANVAS_PLACE_LEVEL_HB] > 0);
  assert_true(levels[DT_CANVAS_PLACE_LEVEL_H] > 0);
  assert_true(cards > 0);
  assert_true(clipped > 0);
  assert_true(kept_over_body > 0);
}

/* --- real objects -------------------------------------------------------------------------- */

/** A view showing canvas units one to one, its top-left corner at the canvas origin. */
static dt_canvas_place_view_t _unit_view(const double width, const double height)
{
  const dt_canvas_place_view_t projection = { .center_x = width * 0.5,
                                              .center_y = height * 0.5,
                                              .zoom = 1.0,
                                              .width = width,
                                              .height = height,
                                              .margin = 7.0 };
  return projection;
}

/** A scene from an object of a real document, the way the view builds it: its shapes, the other frames soft. */
static void _scene_from_object(scene_t *scene, const dt_canvas_place_view_t *projection, const dt_canvas_t *canvas,
                               const dt_canvas_object_t *object)
{
  _scene_defaults(scene);
  scene->input.view = (dt_canvas_place_rect_t){ 6.0, 6.0, projection->width - 12.0, projection->height - 12.0 };
  scene->input.air = 6.0;
  GArray *shapes = g_array_new(FALSE, FALSE, sizeof(dt_canvas_place_shape_t));
  dt_canvas_place_object_shapes(shapes, projection, canvas, object, FALSE);
  for(guint idx = 0; idx < dt_canvas_object_count(canvas); idx++)
  {
    const dt_canvas_object_t *other = dt_canvas_object_at(canvas, idx);
    if(other == object || !dt_canvas_object_is_frame(other)) continue;
    const dt_canvas_rect_t bounds = dt_canvas_object_bounds(other);
    dt_canvas_place_shape_add(shapes, DT_CANVAS_PLACE_SOFT, bounds.x, bounds.y, bounds.x + bounds.width,
                              bounds.y + bounds.height, DT_CANVAS_PLACE_WEIGHT_FRAME, FALSE);
  }
  assert_true(shapes->len <= MAX_SHAPES);
  for(guint idx = 0; idx < shapes->len; idx++)
    scene->shapes[idx] = g_array_index(shapes, dt_canvas_place_shape_t, idx);
  scene->input.shape_count = shapes->len;
  g_array_free(shapes, TRUE);
}

/** The box of an object's own HARD and BODY shapes: what the solver calls the object. */
static void _object_box(const scene_t *scene, double *x0, double *y0, double *x1, double *y1)
{
  *x0 = INFINITY;
  *y0 = INFINITY;
  *x1 = -INFINITY;
  *y1 = -INFINITY;
  for(size_t idx = 0; idx < scene->input.shape_count; idx++)
  {
    const dt_canvas_place_shape_t *shape = &scene->shapes[idx];
    if(!shape->target || (shape->shape_class != DT_CANVAS_PLACE_HARD && shape->shape_class != DT_CANVAS_PLACE_BODY))
      continue;
    *x0 = MIN(*x0, floor(shape->rect.x));
    *y0 = MIN(*y0, floor(shape->rect.y));
    *x1 = MAX(*x1, ceil(shape->rect.x + shape->rect.width));
    *y1 = MAX(*y1, ceil(shape->rect.y + shape->rect.height));
  }
}

static size_t _count_class(const scene_t *scene, const dt_canvas_place_class_t shape_class)
{
  size_t count = 0;
  for(size_t idx = 0; idx < scene->input.shape_count; idx++)
    if(scene->shapes[idx].shape_class == shape_class) count++;
  return count;
}

/**
 * Two frames side by side, a cubic connector from the bottom of one to the bottom of the other,
 * given a waypoint pulled down under them: the control points hang under the frames and the route's
 * lowest point is the waypoint. The properties are asked for on the waypoint.
 */
static dt_canvas_t *_cubic_connector_document(dt_canvas_object_t **connector_out)
{
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *left = dt_canvas_add_image(canvas, 0.0, 0.0, 1000, 1000);
  left->x = 250.0;
  left->y = 200.0;
  left->width = 200.0;
  left->height = 150.0;
  dt_canvas_object_t *right = dt_canvas_add_image(canvas, 0.0, 0.0, 1000, 1000);
  right->x = 750.0;
  right->y = 200.0;
  right->width = 200.0;
  right->height = 150.0;
  dt_canvas_object_t *connector = dt_canvas_add_connector(canvas, left->id, right->id);
  connector->connector.routing = DT_CANVAS_ROUTING_CUBIC;
  connector->connector.from_anchor = DT_CANVAS_ANCHOR_SOUTH;
  connector->connector.to_anchor = DT_CANVAS_ANCHOR_SOUTH;
  connector->connector.line_width = 2.0f;
  dt_canvas_connector_add_via(canvas, connector);
  connector->connector.via_x = 500.0;
  connector->connector.via_y = 420.0;
  *connector_out = connector;
  return canvas;
}

static void _a_cubic_connector_keeps_its_tangents_and_waypoint_clear(void **state)
{
  (void)state;
  dt_canvas_object_t *connector = NULL;
  dt_canvas_t *canvas = _cubic_connector_document(&connector);
  dt_canvas_place_view_t projection = _unit_view(1000.0, 800.0);
  scene_t scene;
  _scene_from_object(&scene, &projection, canvas, connector);
  scene.input.width = 200.0;
  scene.input.strip_height = 32.0;
  scene.input.anchor_x = connector->connector.via_x;
  scene.input.anchor_y = connector->connector.via_y;
  // A cubic route through a waypoint: four tangents with their tethers, the waypoint, the legs.
  assert_true(_count_class(&scene, DT_CANVAS_PLACE_HARD) > 9);
  dt_canvas_place_t place;
  _solve(&scene, &place);
  assert_true(place.visible);
  assert_int_equal(place.level, DT_CANVAS_PLACE_LEVEL_HPB);
  double object_x0 = 0.0;
  double object_y0 = 0.0;
  double object_x1 = 0.0;
  double object_y1 = 0.0;
  _object_box(&scene, &object_x0, &object_y0, &object_x1, &object_y1);
  // Under the connector's handles, the lowest things it has.
  assert_true(place.strip.y >= object_y1 + scene.input.air);
  _check(&scene, &place, NULL, TRUE);

  // The view's bottom edge just under the lowest handle: no room below, so beside or above the
  // handles, never over one.
  projection = _unit_view(1000.0, object_y1 + 6.0 + 6.0 + 20.0);
  _scene_from_object(&scene, &projection, canvas, connector);
  scene.input.width = 200.0;
  scene.input.strip_height = 32.0;
  scene.input.anchor_x = connector->connector.via_x;
  scene.input.anchor_y = connector->connector.via_y;
  _solve(&scene, &place);
  assert_true(place.visible);
  assert_true(place.strip.y + place.strip.height <= object_y1 + scene.input.air);
  _check(&scene, &place, NULL, TRUE);

  // The card, opened there beside the handles, cannot grow down past the view's edge: it goes up,
  // and the strip stays.
  dt_canvas_place_t previous = place;
  previous.strip.x = 20.0;
  previous.strip.y = object_y1 - 32.0;
  previous.level = DT_CANVAS_PLACE_LEVEL_HPB;
  scene.input.previous = &previous;
  scene.input.card_open = TRUE;
  scene.input.card_content_height = 150.0;
  scene.input.card_min = 60.0;
  scene.input.reason = DT_CANVAS_PLACE_GROW;
  const dt_canvas_place_rect_t strip = { previous.strip.x, previous.strip.y, 200.0, 32.0 };
  assert_false(_hits_class(&scene, &strip, DT_CANVAS_PLACE_HARD));
  assert_false(_hits_class(&scene, &strip, DT_CANVAS_PLACE_BODY));
  assert_false(_hits_class(&scene, &strip, DT_CANVAS_PLACE_PREDICTED));
  _solve(&scene, &place);
  assert_true(place.visible);
  assert_true(place.card_shown);
  assert_true(place.strip.x == previous.strip.x && place.strip.y == previous.strip.y);
  assert_int_equal(place.growth, DT_CANVAS_PLACE_UP);
  _check(&scene, &place, &previous, FALSE);
  dt_canvas_free(canvas);
}

static void _a_straight_connector_leaves_room_for_the_tangents_a_click_would_add(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *left = dt_canvas_add_image(canvas, 0.0, 0.0, 1000, 1000);
  left->x = 150.0;
  left->y = 300.0;
  left->width = 100.0;
  left->height = 100.0;
  dt_canvas_object_t *right = dt_canvas_add_image(canvas, 0.0, 0.0, 1000, 1000);
  right->x = 850.0;
  right->y = 300.0;
  right->width = 100.0;
  right->height = 100.0;
  dt_canvas_object_t *connector = dt_canvas_add_connector(canvas, left->id, right->id);
  connector->connector.routing = DT_CANVAS_ROUTING_STRAIGHT;
  connector->connector.from_anchor = DT_CANVAS_ANCHOR_EAST;
  connector->connector.to_anchor = DT_CANVAS_ANCHOR_WEST;
  const dt_canvas_place_view_t projection = _unit_view(1000.0, 700.0);
  scene_t scene;
  _scene_from_object(&scene, &projection, canvas, connector);
  scene.input.width = 200.0;
  scene.input.strip_height = 32.0;
  scene.input.anchor_x = 500.0;
  scene.input.anchor_y = 300.0;
  // What the route made cubic, or given a waypoint, would put on screen.
  assert_true(_count_class(&scene, DT_CANVAS_PLACE_PREDICTED) > 0);
  // Right under the middle of the line, where the strip would go without them, is one of them.
  double line_bottom = -INFINITY;
  for(size_t idx = 0; idx < scene.input.shape_count; idx++)
  {
    const dt_canvas_place_shape_t *shape = &scene.shapes[idx];
    if(shape->shape_class != DT_CANVAS_PLACE_HARD || shape->rect.x >= 600.0 || shape->rect.x + shape->rect.width <= 400.0)
      continue;
    line_bottom = MAX(line_bottom, shape->rect.y + shape->rect.height);
  }
  const dt_canvas_place_rect_t under = { 400.0, line_bottom + scene.input.air, 200.0, 32.0 };
  assert_false(_hits_class(&scene, &under, DT_CANVAS_PLACE_HARD));
  assert_true(_hits_class(&scene, &under, DT_CANVAS_PLACE_PREDICTED));
  dt_canvas_place_t place;
  _solve(&scene, &place);
  assert_true(place.visible);
  assert_int_equal(place.level, DT_CANVAS_PLACE_LEVEL_HPB);
  _check(&scene, &place, NULL, TRUE);
  dt_canvas_free(canvas);

  // A view the predicted handles fill leaves only the level that covers them.
  scene_t crowded;
  _scene_defaults(&crowded);
  crowded.input.view = (dt_canvas_place_rect_t){ 0.0, 0.0, 300.0, 200.0 };
  crowded.input.width = 100.0;
  crowded.input.strip_height = 20.0;
  _shape_add(&crowded, DT_CANVAS_PLACE_PREDICTED, 0.0, 0.0, 300.0, 200.0, FALSE);
  _shape_add(&crowded, DT_CANVAS_PLACE_HARD, 140.0, 90.0, 20.0, 20.0, TRUE);
  crowded.input.anchor_x = 150.0;
  crowded.input.anchor_y = 100.0;
  _solve(&crowded, &place);
  assert_true(place.visible);
  assert_int_equal(place.level, DT_CANVAS_PLACE_LEVEL_HB);
  _check(&crowded, &place, NULL, TRUE);
}

/** Is a screen point inside a shape's rectangle? Closed, give or take rounding. */
static gboolean _shape_holds_point(const dt_canvas_place_shape_t *shape, const double x, const double y)
{
  return x >= shape->rect.x - 1e-6 && x <= shape->rect.x + shape->rect.width + 1e-6 && y >= shape->rect.y - 1e-6
         && y <= shape->rect.y + shape->rect.height + 1e-6;
}

/** Is a screen point inside one of the rectangles of a class? */
static gboolean _covered_by_class(const GArray *shapes, const dt_canvas_place_class_t shape_class, const double x,
                                  const double y)
{
  for(guint idx = 0; idx < shapes->len; idx++)
  {
    const dt_canvas_place_shape_t *shape = &g_array_index(shapes, dt_canvas_place_shape_t, idx);
    if(shape->shape_class != shape_class) continue;
    if(_shape_holds_point(shape, x, y)) return TRUE;
  }
  return FALSE;
}

/**
 * A line free at both ends keeps its two end handles clear, out to their corners, as surely as its
 * band; an end anchored to a frame is no handle and keeps nothing clear past the band. And what the
 * Route control would add to a straight line is the ARC it bends the line into, so the tangents it
 * keeps clear are that arc's control points -- worked out on paper here, 30 degrees off a 600-unit
 * chord at 0.4 of its length -- and not points along the chord.
 */
static void _a_free_line_keeps_its_ends_and_the_arc_a_click_would_bend_clear(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  // No arrowhead: a head's ink is kept clear in a disc round the tip that holds the corners too.
  dt_canvas_line_style_t style = dt_canvas_line_style_default();
  style.arrow_start = FALSE;
  style.arrow_end = FALSE;
  dt_canvas_object_t *line
      = dt_canvas_add_line(canvas, 200.0, 300.0, 800.0, 300.0, DT_CANVAS_ROUTING_STRAIGHT, &style);
  const dt_canvas_place_view_t projection = _unit_view(1000.0, 700.0);
  GArray *shapes = g_array_new(FALSE, FALSE, sizeof(dt_canvas_place_shape_t));
  dt_canvas_place_object_shapes(shapes, &projection, canvas, line, FALSE);
  const double ends[4] = { 200.0, 300.0, 800.0, 300.0 };
  const double corners[8] = { -9.5, -9.5, 9.5, -9.5, 9.5, 9.5, -9.5, 9.5 };
  for(int end = 0; end < 2; end++)
  {
    for(int corner = 0; corner < 4; corner++)
    {
      const double x = ends[2 * end] + corners[2 * corner];
      const double y = ends[2 * end + 1] + corners[2 * corner + 1];
      assert_true(_covered_by_class(shapes, DT_CANVAS_PLACE_HARD, x, y));
    }
  }
  const double arc_reach = 0.4 * 600.0;
  const double seeded[4] = { 200.0 + arc_reach * cos(M_PI / 6.0), 300.0 - arc_reach * sin(M_PI / 6.0),
                             800.0 - arc_reach * cos(M_PI / 6.0), 300.0 - arc_reach * sin(M_PI / 6.0) };
  for(int control = 0; control < 2; control++)
    assert_true(_covered_by_class(shapes, DT_CANVAS_PLACE_PREDICTED, seeded[2 * control], seeded[2 * control + 1]));
  // Nothing the click would add sits where an unbent cubic would have put its control points.
  assert_false(_covered_by_class(shapes, DT_CANVAS_PLACE_PREDICTED, 440.0, 300.0 + 9.0));
  g_array_set_size(shapes, 0);

  // The same line between two frames: its ends are theirs, and only the band is kept clear there.
  dt_canvas_object_t *left = dt_canvas_add_image(canvas, 0.0, 0.0, 1000, 1000);
  left->x = 150.0;
  left->y = 300.0;
  left->width = 100.0;
  left->height = 100.0;
  dt_canvas_object_t *right = dt_canvas_add_image(canvas, 0.0, 0.0, 1000, 1000);
  right->x = 850.0;
  right->y = 300.0;
  right->width = 100.0;
  right->height = 100.0;
  line->connector.from_id = left->id;
  line->connector.from_anchor = DT_CANVAS_ANCHOR_EAST;
  line->connector.to_id = right->id;
  line->connector.to_anchor = DT_CANVAS_ANCHOR_WEST;
  dt_canvas_place_object_shapes(shapes, &projection, canvas, line, FALSE);
  for(int end = 0; end < 2; end++)
  {
    for(int corner = 0; corner < 4; corner++)
    {
      const double x = ends[2 * end] + corners[2 * corner];
      const double y = ends[2 * end + 1] + corners[2 * corner + 1];
      assert_false(_covered_by_class(shapes, DT_CANVAS_PLACE_HARD, x, y));
    }
  }
  g_array_free(shapes, TRUE);
  dt_canvas_free(canvas);
}

/** Is a screen point inside one of the rectangles appended from `first` on? Closed, give or take rounding. */
static gboolean _covered(const GArray *shapes, const guint first, const double x, const double y)
{
  for(guint idx = first; idx < shapes->len; idx++)
  {
    const dt_canvas_place_shape_t *shape = &g_array_index(shapes, dt_canvas_place_shape_t, idx);
    if(_shape_holds_point(shape, x, y)) return TRUE;
  }
  return FALSE;
}

/**
 * Every point a site catches, that the view shows, lies inside the rectangles the site becomes;
 * and every point inside a frame lies inside its body's slabs. Checked against the hit test itself,
 * on turned frames, cutouts, every routing, at zooms from far out to close in: a box one pixel too
 * narrow is a handle the properties may be laid over.
 */
static void _sites_become_rectangles_holding_every_point_they_catch(void **state)
{
  (void)state;
  int caught = 0;
  int inside = 0;
  for(int trial = 0; trial < 60; trial++)
  {
    dt_canvas_t *canvas = dt_canvas_new();
    dt_canvas_object_t *first = dt_canvas_add_image(canvas, 0.0, 0.0, 6000, 4000);
    dt_canvas_object_t *second = dt_canvas_add_image(canvas, 0.0, 0.0, 4000, 6000);
    dt_canvas_object_t *frames[2] = { first, second };
    for(int idx = 0; idx < 2; idx++)
    {
      frames[idx]->x = -400.0 + 800.0 * _random_unit();
      frames[idx]->y = -300.0 + 600.0 * _random_unit();
      frames[idx]->width = 40.0 + 400.0 * _random_unit();
      frames[idx]->height = 40.0 + 400.0 * _random_unit();
      frames[idx]->rotation = -G_PI + 2.0 * G_PI * _random_unit();
    }
    dt_canvas_mask_set_shape(canvas, first, (uint32_t)_random_int(0, 4));
    first->mask.rotation = (float)(-180.0 + 360.0 * _random_unit());
    dt_canvas_object_t *connector = dt_canvas_add_connector(canvas, first->id, second->id);
    connector->connector.routing = (uint32_t)_random_int(0, 2);
    connector->connector.line_width = (float)(16.0 * _random_unit());
    connector->connector.style = (uint32_t)_random_int(0, 3);
    connector->connector.from_anchor = (uint32_t)_random_int(0, DT_CANVAS_ANCHOR_LAST - 1);
    connector->connector.to_anchor = (uint32_t)_random_int(0, DT_CANVAS_ANCHOR_LAST - 1);
    if(_random_unit() < 0.5) dt_canvas_connector_add_via(canvas, connector);
    const dt_canvas_place_view_t projection = { .center_x = -100.0 + 200.0 * _random_unit(),
                                                .center_y = -100.0 + 200.0 * _random_unit(),
                                                .zoom = exp(log(0.1) + (log(5.0) - log(0.1)) * _random_unit()),
                                                .width = 900.0,
                                                .height = 700.0,
                                                .margin = 7.0 };
    const dt_canvas_object_t *objects[3] = { first, second, connector };
    GArray *shapes = g_array_new(FALSE, FALSE, sizeof(dt_canvas_place_shape_t));
    for(int object_idx = 0; object_idx < 3; object_idx++)
    {
      const dt_canvas_object_t *object = objects[object_idx];
      const size_t count = dt_canvas_handle_sites(canvas, object, DT_CANVAS_HANDLES_ALL, NULL, 0);
      dt_canvas_handle_site_t *sites = g_new(dt_canvas_handle_site_t, count + 1);
      dt_canvas_handle_sites(canvas, object, DT_CANVAS_HANDLES_ALL, sites, count);
      for(size_t site_idx = 0; site_idx < count; site_idx++)
      {
        const dt_canvas_handle_site_t *site = &sites[site_idx];
        const guint first_shape = shapes->len;
        dt_canvas_place_sites_add(shapes, &projection, site, 1, DT_CANVAS_PLACE_HARD);
        double x0 = 0.0;
        double y0 = 0.0;
        double x1 = 0.0;
        double y1 = 0.0;
        double reach = 0.0;
        dt_canvas_handle_site_resolve(site, projection.zoom, &x0, &y0, &x1, &y1, &reach);
        for(int sample = 0; sample < 200; sample++)
        {
          // Around the site's shape, a little past its reach, so the edge of the band is sampled too.
          const double along = _random_unit();
          const double base_x = x0 + (x1 - x0) * along;
          const double base_y = y0 + (y1 - y0) * along;
          const double canvas_x = base_x + (-1.2 + 2.4 * _random_unit()) * reach;
          const double canvas_y = base_y + (-1.2 + 2.4 * _random_unit()) * reach;
          if(!dt_canvas_handle_site_hit(site, canvas_x, canvas_y, projection.zoom)) continue;
          double screen_x = 0.0;
          double screen_y = 0.0;
          dt_canvas_place_to_screen(&projection, canvas_x, canvas_y, &screen_x, &screen_y);
          if(screen_x < 0.0 || screen_y < 0.0 || screen_x > projection.width || screen_y > projection.height) continue;
          assert_true(_covered(shapes, first_shape, screen_x, screen_y));
          caught++;
        }
      }
      g_free(sites);
      if(!dt_canvas_object_is_frame(object)) continue;
      const guint first_slab = shapes->len;
      dt_canvas_place_body_add(shapes, &projection, object);
      for(int sample = 0; sample < 400; sample++)
      {
        const double local_x = (-0.5 + _random_unit()) * object->width;
        const double local_y = (-0.5 + _random_unit()) * object->height;
        const double canvas_x = object->x + local_x * cos(object->rotation) - local_y * sin(object->rotation);
        const double canvas_y = object->y + local_x * sin(object->rotation) + local_y * cos(object->rotation);
        double screen_x = 0.0;
        double screen_y = 0.0;
        dt_canvas_place_to_screen(&projection, canvas_x, canvas_y, &screen_x, &screen_y);
        if(screen_x < 0.0 || screen_y < 0.0 || screen_x > projection.width || screen_y > projection.height) continue;
        assert_true(_covered(shapes, first_slab, screen_x, screen_y));
        inside++;
      }
    }
    g_array_free(shapes, TRUE);
    dt_canvas_free(canvas);
  }
  // Worth its time only if many points were caught on screen.
  assert_true(caught > 2000);
  assert_true(inside > 2000);
}

/* --- motion ------------------------------------------------------------------------------ */

static void _translate(scene_t *scene, const double shift_x, const double shift_y)
{
  scene->input.view.x += shift_x;
  scene->input.view.y += shift_y;
  for(size_t idx = 0; idx < scene->input.shape_count; idx++)
  {
    scene->shapes[idx].rect.x += shift_x;
    scene->shapes[idx].rect.y += shift_y;
  }
  scene->input.anchor_x += shift_x;
  scene->input.anchor_y += shift_y;
  scene->input.press_x += shift_x;
  scene->input.press_y += shift_y;
}

/** A pan: the object and its handles move, the view does not. */
static void _move_object(scene_t *scene, const double shift_x, const double shift_y)
{
  for(size_t idx = 0; idx < scene->input.shape_count; idx++)
  {
    if(scene->shapes[idx].shape_class == DT_CANVAS_PLACE_SOFT) continue;
    scene->shapes[idx].rect.x += shift_x;
    scene->shapes[idx].rect.y += shift_y;
  }
  scene->input.anchor_x += shift_x;
  scene->input.anchor_y += shift_y;
}

static void _a_scene_moved_by_whole_pixels_moves_its_placement_by_as_much(void **state)
{
  (void)state;
  int compared = 0;
  int followed = 0;
  for(int trial = 0; trial < 500; trial++)
  {
    scene_t scene;
    _scene_random(&scene);
    scene.input.card_open = FALSE;
    // Half the time on whole and half pixels, where rounding has a direction to pick and must pick
    // the same one on both sides of zero.
    if(trial % 2 == 0)
    {
      scene.input.anchor_x = floor(scene.input.anchor_x) + 0.5 * (trial % 4 == 0);
      scene.input.anchor_y = floor(scene.input.anchor_y) + 0.5 * (trial % 4 == 0);
    }
    dt_canvas_place_t place;
    _solve(&scene, &place);
    const int shift_x = _random_int(-300, 300);
    const int shift_y = _random_int(-300, 300);
    scene_t moved;
    _scene_copy(&moved, &scene);
    _translate(&moved, shift_x, shift_y);
    dt_canvas_place_t moved_place;
    _solve(&moved, &moved_place);
    assert_int_equal(place.visible, moved_place.visible);
    if(!place.visible) continue;
    assert_true(moved_place.strip.x == place.strip.x + shift_x);
    assert_true(moved_place.strip.y == place.strip.y + shift_y);
    assert_int_equal(moved_place.level, place.level);
    assert_int_equal(moved_place.growth, place.growth);
    assert_true(moved_place.card_height == place.card_height);

    // And a placement resolved after a pan past the hysteresis -- by a fraction, so the rounding of
    // the motion is exercised -- resolves the same way wherever the whole scene sits. The fractions
    // stay clear of a half so the motion measured on either side cannot round differently.
    const double motions[4] = { 3.3, -3.7, 5.2, -4.4 };
    const double motion_x = motions[trial % 4];
    const double motion_y = motions[(trial + 1) % 4];
    scene_t kept;
    _scene_copy(&kept, &scene);
    _move_object(&kept, motion_x, motion_y);
    kept.input.reason = DT_CANVAS_PLACE_RESOLVE;
    kept.input.previous = &place;
    dt_canvas_place_t kept_place;
    _solve(&kept, &kept_place);
    _check(&kept, &kept_place, &place, FALSE);
    dt_canvas_place_t moved_previous = place;
    moved_previous.strip.x += shift_x;
    moved_previous.strip.y += shift_y;
    moved_previous.anchor_x += shift_x;
    moved_previous.anchor_y += shift_y;
    scene_t resolved;
    _scene_copy(&resolved, &moved);
    _move_object(&resolved, motion_x, motion_y);
    resolved.input.reason = DT_CANVAS_PLACE_RESOLVE;
    resolved.input.previous = &moved_previous;
    dt_canvas_place_t resolved_place;
    _solve(&resolved, &resolved_place);
    assert_int_equal(kept_place.visible, resolved_place.visible);
    if(!kept_place.visible) continue;
    assert_true(resolved_place.strip.x == kept_place.strip.x + shift_x);
    assert_true(resolved_place.strip.y == kept_place.strip.y + shift_y);
    assert_int_equal(resolved_place.level, kept_place.level);
    compared++;
    // Either searched afresh, and attached to the anchor as it now is, or carried along rigidly: by
    // the motion rounded to whole pixels, and the anchor by exactly as much.
    const gboolean carried = kept_place.anchor_x == place.anchor_x + floor(motion_x + 0.5)
                             && kept_place.anchor_y == place.anchor_y + floor(motion_y + 0.5);
    const gboolean searched = kept_place.anchor_x == floor(kept.input.anchor_x * 256.0 + 0.5) / 256.0
                              && kept_place.anchor_y == floor(kept.input.anchor_y * 256.0 + 0.5) / 256.0;
    assert_true(carried || searched);
    if(carried)
    {
      assert_true(kept_place.strip.x == place.strip.x + floor(motion_x + 0.5));
      assert_true(kept_place.strip.y == place.strip.y + floor(motion_y + 0.5));
      followed++;
    }
  }
  assert_true(compared > 50);
  assert_true(followed > 20);
}

/**
 * A scene the fuzz once found, kept as it was: after a pan, staying and following cost the same to
 * within the rounding of a subtraction from an anchor that is not a binary fraction, and moving the
 * whole scene by whole pixels moved that rounding, so the same pan resolved to two different spots.
 * The anchor is snapped to a 256th of a pixel before anything is measured from it.
 */
static void _a_near_tie_resolves_the_same_way_wherever_the_scene_sits(void **state)
{
  (void)state;
  scene_t scene;
  _scene_defaults(&scene);
  scene.input.view = (dt_canvas_place_rect_t){ 19.0, 34.0, 123.0, 63.0 };
  scene.input.air = 1.0;
  scene.input.width = 12.0;
  scene.input.strip_height = 33.0;
  scene.input.anchor_x = 100.5;
  scene.input.anchor_y = 47.5;
  scene.input.last_card_height = 102.0;
  scene.input.has_press = TRUE;
  scene.input.press_x = 34.491015083461761;
  scene.input.press_y = 84.640041332721154;
  scene.input.card_content_height = 38.0;
  scene.input.card_min = 44.0;
  const double shapes[22][6] = {
    { DT_CANVAS_PLACE_BODY, 20, 21, 63, 31, 1 },       { DT_CANVAS_PLACE_BODY, 9, 52, 85, 31, 1 },
    { DT_CANVAS_PLACE_BODY, 13, 83, 77, 31, 1 },       { DT_CANVAS_PLACE_HARD, 86, 138, 17, 7, 1 },
    { DT_CANVAS_PLACE_HARD, 110, 15, 24, 16, 1 },      { DT_CANVAS_PLACE_HARD, 38, 10, 23, 13, 0 },
    { DT_CANVAS_PLACE_HARD, 16, 46, 19, 17, 1 },       { DT_CANVAS_PLACE_HARD, 28, 78, 22, 9, 1 },
    { DT_CANVAS_PLACE_HARD, 116, 136, 15, 18, 1 },     { DT_CANVAS_PLACE_HARD, 103, 1, 14, 12, 1 },
    { DT_CANVAS_PLACE_HARD, -43, 0, 23, 9, 1 },        { DT_CANVAS_PLACE_HARD, 99, 119, 4, 5, 0 },
    { DT_CANVAS_PLACE_PREDICTED, 22, 82, 29, 14, 0 },  { DT_CANVAS_PLACE_PREDICTED, 68, 56, 8, 7, 0 },
    { DT_CANVAS_PLACE_PREDICTED, 11, 55, 5, 22, 0 },   { DT_CANVAS_PLACE_PREDICTED, 126, 79, 38, 38, 0 },
    { DT_CANVAS_PLACE_PREDICTED, 103, 39, 35, 21, 0 }, { DT_CANVAS_PLACE_PREDICTED, 27, 88, 40, 3, 0 },
    { DT_CANVAS_PLACE_PREDICTED, 122, 61, 4, 40, 0 },  { DT_CANVAS_PLACE_PREDICTED, 60, 38, 27, 16, 0 },
    { DT_CANVAS_PLACE_SOFT, 12, 29, 7, 4, 0 },         { DT_CANVAS_PLACE_SOFT, 117, 55, 17, 40, 0 },
  };
  for(int idx = 0; idx < 22; idx++)
    _shape_add(&scene, (dt_canvas_place_class_t)shapes[idx][0], shapes[idx][1], shapes[idx][2], shapes[idx][3],
               shapes[idx][4], shapes[idx][5] != 0.0);
  dt_canvas_place_t place;
  _solve(&scene, &place);
  assert_true(place.visible);

  scene_t kept;
  _scene_copy(&kept, &scene);
  _move_object(&kept, 3.3, -3.7);
  kept.input.reason = DT_CANVAS_PLACE_RESOLVE;
  kept.input.previous = &place;
  dt_canvas_place_t kept_place;
  _solve(&kept, &kept_place);
  _check(&kept, &kept_place, &place, FALSE);

  scene_t resolved;
  _scene_copy(&resolved, &scene);
  _translate(&resolved, -20.0, 98.0);
  _move_object(&resolved, 3.3, -3.7);
  dt_canvas_place_t moved_previous = place;
  moved_previous.strip.x -= 20.0;
  moved_previous.strip.y += 98.0;
  moved_previous.anchor_x -= 20.0;
  moved_previous.anchor_y += 98.0;
  resolved.input.reason = DT_CANVAS_PLACE_RESOLVE;
  resolved.input.previous = &moved_previous;
  dt_canvas_place_t resolved_place;
  _solve(&resolved, &resolved_place);
  assert_true(kept_place.visible && resolved_place.visible);
  assert_true(resolved_place.strip.x == kept_place.strip.x - 20.0);
  assert_true(resolved_place.strip.y == kept_place.strip.y + 98.0);
}

/** A roomy scene: a frame in the middle of a large view, its four corners and its knob. */
static void _frame_scene(scene_t *scene)
{
  _scene_defaults(scene);
  scene->input.view = (dt_canvas_place_rect_t){ 6.0, 6.0, 1188.0, 788.0 };
  scene->input.air = 6.0;
  scene->input.width = 300.0;
  scene->input.strip_height = 32.0;
  for(int slab = 0; slab < 16; slab++)
    _shape_add(scene, DT_CANVAS_PLACE_BODY, 396.0, 196.0 + slab * 13.0, 208.0, 13.0, TRUE);
  const double corners[4][2] = { { 400.0, 200.0 }, { 600.0, 200.0 }, { 600.0, 400.0 }, { 400.0, 400.0 } };
  for(int corner = 0; corner < 4; corner++)
    _shape_add(scene, DT_CANVAS_PLACE_HARD, corners[corner][0] - 8.0, corners[corner][1] - 8.0, 16.0, 16.0, TRUE);
  _shape_add(scene, DT_CANVAS_PLACE_HARD, 492.0, 164.0, 16.0, 16.0, TRUE);
  scene->input.anchor_x = 500.0;
  scene->input.anchor_y = 400.0;
}

static void _a_small_motion_leaves_the_properties_where_they_are(void **state)
{
  (void)state;
  scene_t scene;
  _frame_scene(&scene);
  dt_canvas_place_t opened;
  _solve(&scene, &opened);
  assert_true(opened.visible);
  assert_int_equal(opened.level, DT_CANVAS_PLACE_LEVEL_HPB);

  // One pixel, then another: two pixels in all, and the strip has not moved.
  _move_object(&scene, 1.0, -1.0);
  scene.input.reason = DT_CANVAS_PLACE_RESOLVE;
  scene.input.previous = &opened;
  dt_canvas_place_t kept;
  _solve(&scene, &kept);
  assert_true(kept.strip.x == opened.strip.x && kept.strip.y == opened.strip.y);
  assert_true(kept.anchor_x == opened.anchor_x && kept.anchor_y == opened.anchor_y);
  _move_object(&scene, 1.0, -1.0);
  dt_canvas_place_t previous = kept;
  scene.input.previous = &previous;
  _solve(&scene, &kept);
  assert_true(kept.strip.x == opened.strip.x && kept.strip.y == opened.strip.y);

  // A third pixel crosses the threshold: the strip follows the object by all three, rigidly.
  _move_object(&scene, 1.0, -1.0);
  previous = kept;
  _solve(&scene, &kept);
  assert_true(kept.strip.x == opened.strip.x + 3.0);
  assert_true(kept.strip.y == opened.strip.y - 3.0);
  assert_true(kept.anchor_x == opened.anchor_x + 3.0);
  assert_int_equal(kept.level, DT_CANVAS_PLACE_LEVEL_HPB);

  // A pan across the view: still where it was relative to the object.
  _move_object(&scene, -250.0, 120.0);
  previous = kept;
  _solve(&scene, &kept);
  assert_true(kept.strip.x == opened.strip.x + 3.0 - 250.0);
  assert_true(kept.strip.y == opened.strip.y - 3.0 + 120.0);
}

/**
 * Half a pixel rounds up whatever the sign, and what rounding leaves over is carried by the anchor:
 * 3.5 moves the strip by 4, -3.5 by 3; after 3.5 the anchor stands at 4, so a further 2.5 -- 6 in
 * all -- is 2 from it and moves nothing, where measuring from the real anchor would move it again.
 */
static void _a_fraction_of_a_pixel_is_carried_to_the_next_motion(void **state)
{
  (void)state;
  scene_t scene;
  _frame_scene(&scene);
  dt_canvas_place_t opened;
  _solve(&scene, &opened);
  assert_true(opened.visible);

  scene_t right;
  _scene_copy(&right, &scene);
  _move_object(&right, 3.5, 0.0);
  right.input.reason = DT_CANVAS_PLACE_RESOLVE;
  right.input.previous = &opened;
  dt_canvas_place_t first;
  _solve(&right, &first);
  assert_true(first.strip.x == opened.strip.x + 4.0);
  assert_true(first.anchor_x == opened.anchor_x + 4.0);
  _check(&right, &first, &opened, FALSE);

  _move_object(&right, 2.5, 0.0);
  right.input.previous = &first;
  dt_canvas_place_t second;
  _solve(&right, &second);
  assert_true(second.strip.x == opened.strip.x + 4.0);
  assert_true(second.anchor_x == opened.anchor_x + 4.0);

  _move_object(&right, 1.0, 0.0);
  right.input.previous = &second;
  dt_canvas_place_t third;
  _solve(&right, &third);
  assert_true(third.strip.x == opened.strip.x + 7.0);
  assert_true(third.anchor_x == opened.anchor_x + 7.0);

  scene_t left;
  _scene_copy(&left, &scene);
  _move_object(&left, -3.5, 0.0);
  left.input.reason = DT_CANVAS_PLACE_RESOLVE;
  left.input.previous = &opened;
  dt_canvas_place_t back;
  _solve(&left, &back);
  assert_true(back.strip.x == opened.strip.x - 3.0);
  assert_true(back.anchor_x == opened.anchor_x - 3.0);
}

/* --- the card ------------------------------------------------------------------------------ */

static void _growing_the_card_keeps_the_strip_and_collapsing_it_too(void **state)
{
  (void)state;
  scene_t scene;
  _frame_scene(&scene);
  dt_canvas_place_t strip_only;
  _solve(&scene, &strip_only);
  assert_true(strip_only.visible);
  assert_false(strip_only.card_shown);

  // Room below: the card grows down, whole, and the strip stays.
  scene_t grow;
  _scene_copy(&grow, &scene);
  grow.input.card_open = TRUE;
  grow.input.card_content_height = 260.0;
  grow.input.reason = DT_CANVAS_PLACE_GROW;
  grow.input.previous = &strip_only;
  dt_canvas_place_t grown;
  _solve(&grow, &grown);
  assert_true(grown.card_shown);
  assert_true(grown.strip.x == strip_only.strip.x && grown.strip.y == strip_only.strip.y);
  assert_int_equal(grown.growth, DT_CANVAS_PLACE_DOWN);
  assert_true(grown.card_height == 260.0);
  _check(&grow, &grown, &strip_only, TRUE);

  // Collapsing it moves nothing.
  scene_t collapse;
  _scene_copy(&collapse, &scene);
  collapse.input.reason = DT_CANVAS_PLACE_RESOLVE;
  collapse.input.previous = &grown;
  dt_canvas_place_t collapsed;
  _solve(&collapse, &collapsed);
  assert_true(collapsed.strip.x == strip_only.strip.x && collapsed.strip.y == strip_only.strip.y);
  assert_false(collapsed.card_shown);
  assert_false(collapsed.clipped);

  // A strip beside the frame with the view's edge close under it: the card goes up instead, and
  // the strip still stays.
  dt_canvas_place_t beside = strip_only;
  beside.strip.x = 700.0;
  beside.strip.y = 500.0;
  beside.growth = DT_CANVAS_PLACE_DOWN;
  scene_t up;
  _scene_copy(&up, &grow);
  up.input.previous = &beside;
  up.input.view.height = 532.0 + 40.0 - up.input.view.y;
  _solve(&up, &grown);
  assert_true(grown.card_shown);
  assert_true(grown.strip.x == beside.strip.x && grown.strip.y == beside.strip.y);
  assert_int_equal(grown.growth, DT_CANVAS_PLACE_UP);
  assert_true(grown.card_height == 260.0);
  _check(&up, &grown, &beside, TRUE);
  const double strip_bottom = strip_only.strip.y + strip_only.strip.height;

  // Too little on both sides of the strip for all of it: the strip moves to where the whole card fits.
  // Cut down to the room beside the strip, the card would hide its last rows behind a scrollbar in a
  // view with room for every one of them.
  scene_t tight;
  _scene_copy(&tight, &scene);
  tight.input.view.height = strip_bottom + 150.0 - tight.input.view.y;
  tight.input.card_open = TRUE;
  tight.input.card_content_height = 400.0;
  tight.input.card_min = 120.0;
  tight.input.reason = DT_CANVAS_PLACE_GROW;
  tight.input.previous = &strip_only;
  _solve(&tight, &grown);
  assert_true(grown.visible);
  assert_true(grown.card_shown);
  assert_true(grown.card_height == 400.0);
  assert_false(grown.strip.x == strip_only.strip.x && grown.strip.y == strip_only.strip.y);
  _check(&tight, &grown, &strip_only, TRUE);

  // A view shorter than the strip and the whole card: the card is given all the height the strip
  // leaves -- the one card that scrolls -- wherever a column is clear from the view's top to its bottom.
  scene_t short_view;
  _scene_copy(&short_view, &tight);
  short_view.input.view = (dt_canvas_place_rect_t){ 6.0, 6.0, 1188.0, 300.0 };
  _solve(&short_view, &grown);
  assert_true(grown.visible);
  assert_true(grown.card_shown);
  assert_true(grown.card_height == 300.0 - 32.0);
  const dt_canvas_place_rect_t filling = dt_canvas_place_footprint(&grown);
  assert_true(filling.y == 6.0 && filling.height == 300.0);
  _check(&short_view, &grown, &strip_only, TRUE);

  // And below the card's least even that is not shown: the strip alone, the card clipped.
  short_view.input.card_min = 300.0;
  _solve(&short_view, &grown);
  assert_true(grown.visible);
  assert_false(grown.card_shown);
  assert_true(grown.clipped);
  _check(&short_view, &grown, &strip_only, TRUE);
}

/**
 * An open card is part of what the properties are: opened with it, it is shown where it fits; a
 * pan carries it along; and when it no longer fits beside the strip a pan carried along, the card
 * is searched a place for rather than silently clipped.
 */
static void _an_open_card_is_shown_wherever_it_fits(void **state)
{
  (void)state;
  scene_t scene;
  _frame_scene(&scene);
  scene.input.card_open = TRUE;
  scene.input.card_content_height = 260.0;
  dt_canvas_place_t opened;
  _solve(&scene, &opened);
  assert_true(opened.visible);
  assert_true(opened.card_shown);
  assert_true(opened.card_height == 260.0);
  _check(&scene, &opened, NULL, TRUE);

  _move_object(&scene, 3.0, 4.0);
  scene.input.reason = DT_CANVAS_PLACE_RESOLVE;
  scene.input.previous = &opened;
  dt_canvas_place_t panned;
  _solve(&scene, &panned);
  assert_true(panned.card_shown);
  assert_true(panned.strip.x == opened.strip.x + 3.0 && panned.strip.y == opened.strip.y + 4.0);
  _check(&scene, &panned, &opened, FALSE);

  // Pan the frame down until the strip carried along has neither the view's bottom below it nor
  // room between it and the frame above it: the card goes elsewhere, whole.
  const double drop = (scene.input.view.y + scene.input.view.height) - (panned.strip.y + panned.strip.height) - 40.0;
  _move_object(&scene, 0.0, drop);
  const dt_canvas_place_t before_drop = panned;
  scene.input.previous = &before_drop;
  _solve(&scene, &panned);
  assert_true(panned.visible);
  assert_true(panned.card_shown);
  assert_true(panned.card_height == 260.0);
  _check(&scene, &panned, &before_drop, FALSE);
}

/**
 * A search that runs after a pan still offers the spot the properties are in, and the one following
 * the object takes them to: an open card that fits nowhere sends a pan to the full search, and the
 * strip alone is what it comes back with -- where it stood, since ten pixels of movement cost more
 * than the few pixels of distance following would save, and not at the spot the anchor would pick
 * afresh, which the pointer that asked for the properties had kept them away from.
 */
static void _a_search_after_a_pan_can_stay_where_the_properties_are(void **state)
{
  (void)state;
  scene_t scene;
  _frame_scene(&scene);
  // Too narrow for the strip to stand beside the frame, too short for the card above or below it.
  scene.input.view = (dt_canvas_place_rect_t){ 6.0, 6.0, 988.0, 500.0 };
  scene.input.width = 400.0;
  scene_t pointer;
  _scene_copy(&pointer, &scene);
  _shape_add(&pointer, DT_CANVAS_PLACE_HARD, 488.0, 414.0, 24.0, 24.0, FALSE);
  dt_canvas_place_t opened;
  _solve(&pointer, &opened);
  assert_true(opened.visible);
  assert_int_equal(opened.level, DT_CANVAS_PLACE_LEVEL_HPB);
  _check(&pointer, &opened, NULL, TRUE);
  // Without the pointer, the anchor's own column is free: the properties would open elsewhere.
  dt_canvas_place_t unhindered;
  _solve(&scene, &unhindered);
  assert_false(unhindered.strip.x == opened.strip.x);

  scene_t panned;
  _scene_copy(&panned, &scene);
  _move_object(&panned, 10.0, 0.0);
  panned.input.card_open = TRUE;
  panned.input.card_content_height = 400.0;
  panned.input.card_min = 300.0;
  panned.input.reason = DT_CANVAS_PLACE_RESOLVE;
  panned.input.previous = &opened;
  dt_canvas_place_t followed;
  _solve(&panned, &followed);
  assert_true(followed.visible);
  assert_false(followed.card_shown);
  assert_true(followed.strip.x == opened.strip.x && followed.strip.y == opened.strip.y);
  _check(&panned, &followed, &opened, FALSE);
}

/**
 * Over the body, the properties still stay put for a small motion, still stay put when the pointer
 * that avoided them at the opening is gone, still keep a card the user grew there, and still collapse
 * without moving -- as long as nothing clear of the body has opened. Once something has, they go there.
 */
static void _an_object_filling_the_view_is_covered_as_little_as_possible(void **state)
{
  (void)state;
  scene_t scene;
  _scene_defaults(&scene);
  scene.input.view = (dt_canvas_place_rect_t){ 6.0, 6.0, 788.0, 588.0 };
  scene.input.air = 6.0;
  scene.input.width = 300.0;
  scene.input.strip_height = 32.0;
  for(int slab = 0; slab < 16; slab++)
    _shape_add(&scene, DT_CANVAS_PLACE_BODY, -100.0, -100.0 + slab * 50.0, 1000.0, 50.0, TRUE);
  scene.input.anchor_x = 400.0;
  scene.input.anchor_y = 300.0;
  scene.input.has_press = TRUE;
  scene.input.press_x = 400.0;
  scene.input.press_y = 300.0;
  dt_canvas_place_t place;
  _solve(&scene, &place);
  assert_true(place.visible);
  assert_int_equal(place.level, DT_CANVAS_PLACE_LEVEL_H);
  const dt_canvas_place_rect_t footprint = dt_canvas_place_footprint(&place);
  const gboolean at_edge = footprint.x == 6.0 || footprint.y == 6.0 || footprint.x + footprint.width == 794.0
                           || footprint.y + footprint.height == 594.0;
  assert_true(at_edge);
  assert_false(scene.input.press_x >= footprint.x && scene.input.press_x < footprint.x + footprint.width
               && scene.input.press_y >= footprint.y && scene.input.press_y < footprint.y + footprint.height);
  _check(&scene, &place, NULL, TRUE);

  // Pressed right where they would otherwise go -- at the left edge, level with the anchor -- they
  // go somewhere else rather than sit under the press.
  scene_t pressed;
  _scene_copy(&pressed, &scene);
  pressed.input.press_x = 16.0;
  pressed.input.press_y = 272.0;
  dt_canvas_place_t moved;
  _solve(&pressed, &moved);
  assert_true(moved.visible);
  const dt_canvas_place_rect_t moved_footprint = dt_canvas_place_footprint(&moved);
  assert_false(pressed.input.press_x >= moved_footprint.x && pressed.input.press_x < moved_footprint.x + moved_footprint.width
               && pressed.input.press_y >= moved_footprint.y
               && pressed.input.press_y < moved_footprint.y + moved_footprint.height);
  _check(&pressed, &moved, NULL, TRUE);

  // A pixel of motion over the body leaves them exactly where they are.
  scene_t nudged;
  _scene_copy(&nudged, &scene);
  _move_object(&nudged, 1.0, 1.0);
  nudged.input.reason = DT_CANVAS_PLACE_RESOLVE;
  nudged.input.previous = &place;
  dt_canvas_place_t nudged_place;
  _solve(&nudged, &nudged_place);
  assert_true(nudged_place.strip.x == place.strip.x && nudged_place.strip.y == place.strip.y);
  assert_int_equal(nudged_place.level, DT_CANVAS_PLACE_LEVEL_H);
  _check(&nudged, &nudged_place, &place, FALSE);

  // Opened away from the pointer that asked for them, where they would otherwise have gone: once
  // the pointer is no longer a shape, nothing has moved and neither do they.
  scene_t pointer;
  _scene_copy(&pointer, &scene);
  _shape_add(&pointer, DT_CANVAS_PLACE_HARD, footprint.x + 20.0, footprint.y + 4.0, 24.0, 24.0, FALSE);
  dt_canvas_place_t avoided;
  _solve(&pointer, &avoided);
  assert_true(avoided.visible);
  assert_false(avoided.strip.x == place.strip.x && avoided.strip.y == place.strip.y);
  scene_t pointer_gone;
  _scene_copy(&pointer_gone, &scene);
  pointer_gone.input.reason = DT_CANVAS_PLACE_RESOLVE;
  pointer_gone.input.previous = &avoided;
  dt_canvas_place_t stayed;
  _solve(&pointer_gone, &stayed);
  assert_true(stayed.strip.x == avoided.strip.x && stayed.strip.y == avoided.strip.y);
  _check(&pointer_gone, &stayed, &avoided, FALSE);

  // The card grown over the body -- nothing clear of it exists -- is kept by a refill, and a
  // collapse leaves the strip where it is.
  scene_t grow;
  _scene_copy(&grow, &scene);
  grow.input.card_open = TRUE;
  grow.input.card_content_height = 200.0;
  grow.input.reason = DT_CANVAS_PLACE_GROW;
  grow.input.previous = &place;
  dt_canvas_place_t grown;
  _solve(&grow, &grown);
  assert_true(grown.card_shown);
  assert_true(grown.strip.x == place.strip.x && grown.strip.y == place.strip.y);
  assert_int_equal(grown.level, DT_CANVAS_PLACE_LEVEL_H);
  _check(&grow, &grown, &place, TRUE);
  scene_t refill;
  _scene_copy(&refill, &grow);
  refill.input.reason = DT_CANVAS_PLACE_RESOLVE;
  refill.input.previous = &grown;
  dt_canvas_place_t refilled;
  _solve(&refill, &refilled);
  assert_true(refilled.strip.x == grown.strip.x && refilled.strip.y == grown.strip.y);
  assert_true(refilled.card_shown);
  assert_true(refilled.card_height == grown.card_height);
  _check(&refill, &refilled, &grown, FALSE);
  scene_t collapse;
  _scene_copy(&collapse, &scene);
  collapse.input.reason = DT_CANVAS_PLACE_RESOLVE;
  collapse.input.previous = &grown;
  dt_canvas_place_t collapsed;
  _solve(&collapse, &collapsed);
  assert_true(collapsed.strip.x == grown.strip.x && collapsed.strip.y == grown.strip.y);
  assert_false(collapsed.card_shown);
  _check(&collapse, &collapsed, &grown, FALSE);

  // A card open when the properties went over the body, without a GROW -- a refill, a pan -- is not
  // grown over it: it stays clipped, and the strip stays where it is.
  scene_t card_refill;
  _scene_copy(&card_refill, &grow);
  card_refill.input.reason = DT_CANVAS_PLACE_RESOLVE;
  card_refill.input.previous = &place;
  dt_canvas_place_t clipped;
  _solve(&card_refill, &clipped);
  assert_true(clipped.strip.x == place.strip.x && clipped.strip.y == place.strip.y);
  assert_false(clipped.card_shown);
  assert_true(clipped.clipped);
  _check(&card_refill, &clipped, &place, FALSE);

  // The frame panned half out of the view opens room clear of it: the properties go there.
  scene_t panned;
  _scene_copy(&panned, &scene);
  _move_object(&panned, 0.0, 400.0);
  panned.input.reason = DT_CANVAS_PLACE_RESOLVE;
  panned.input.previous = &place;
  dt_canvas_place_t freed;
  _solve(&panned, &freed);
  assert_true(freed.visible);
  assert_true(freed.level < DT_CANVAS_PLACE_LEVEL_H);
  _check(&panned, &freed, &place, FALSE);

  // And the card opened over properties that went over the body before that room opened -- a
  // placement held back while the user was in them -- goes to the room, not over the body.
  scene_t stale;
  _scene_copy(&stale, &panned);
  stale.input.card_open = TRUE;
  stale.input.card_content_height = 150.0;
  stale.input.reason = DT_CANVAS_PLACE_GROW;
  stale.input.previous = &place;
  dt_canvas_place_t regrown;
  _solve(&stale, &regrown);
  assert_true(regrown.visible);
  assert_true(regrown.card_shown);
  assert_true(regrown.level < DT_CANVAS_PLACE_LEVEL_H);
  _check(&stale, &regrown, &place, FALSE);
}

/**
 * A placement over the body is kept only while it covers no more of it than it did, and grows no
 * card there that the user did not grow. Thin slabs of
 * body every forty pixels leave nowhere clear of it, and the properties open between two of them,
 * covering none; the object then grows a thick part right under them, without the anchor moving.
 * Kept, they would sit on it; searched again, they go to another gap between thin slabs.
 */
static void _over_the_body_a_placement_covers_no_more_and_grows_no_card(void **state)
{
  (void)state;
  scene_t scene;
  _scene_defaults(&scene);
  scene.input.view = (dt_canvas_place_rect_t){ 0.0, 0.0, 800.0, 600.0 };
  scene.input.air = 6.0;
  scene.input.width = 200.0;
  scene.input.strip_height = 32.0;
  for(int slab = 0; slab < 15; slab++) _shape_add(&scene, DT_CANVAS_PLACE_BODY, 0.0, 20.0 + slab * 40.0, 800.0, 1.0, TRUE);
  scene.input.anchor_x = 400.0;
  scene.input.anchor_y = 300.0;
  dt_canvas_place_t opened;
  _solve(&scene, &opened);
  assert_true(opened.visible);
  assert_int_equal(opened.level, DT_CANVAS_PLACE_LEVEL_H);
  assert_true(opened.body == 0.0);
  _check(&scene, &opened, NULL, TRUE);

  scene_t thick;
  _scene_copy(&thick, &scene);
  _shape_add(&thick, DT_CANVAS_PLACE_BODY, opened.strip.x - 10.0, opened.strip.y - 10.0, opened.strip.width + 20.0,
             opened.strip.height + 20.0, TRUE);
  thick.input.reason = DT_CANVAS_PLACE_RESOLVE;
  thick.input.previous = &opened;
  dt_canvas_place_t resolved;
  _solve(&thick, &resolved);
  assert_true(resolved.visible);
  assert_false(resolved.strip.x == opened.strip.x && resolved.strip.y == opened.strip.y);
  assert_true(resolved.body < 0.01);
  _check(&thick, &resolved, &opened, FALSE);

  // A card opened without a GROW is not shown over the body, not even where it happens to fall
  // between two slabs and would cover no more of the body than the strip does: over the body, only
  // a card the user grew there is kept. Here the strip rests on a slab and the seven pixels below it
  // are clear.
  dt_canvas_place_t resting = opened;
  resting.strip.x = 300.0;
  resting.strip.y = 21.0;
  resting.growth = DT_CANVAS_PLACE_DOWN;
  resting.card_shown = FALSE;
  resting.card_height = 0.0;
  resting.anchor_x = scene.input.anchor_x;
  resting.anchor_y = scene.input.anchor_y;
  resting.body = 0.0;
  scene_t card_open;
  _scene_copy(&card_open, &scene);
  card_open.input.card_open = TRUE;
  card_open.input.card_content_height = 7.0;
  card_open.input.card_min = 5.0;
  card_open.input.reason = DT_CANVAS_PLACE_RESOLVE;
  card_open.input.previous = &resting;
  dt_canvas_place_t kept;
  _solve(&card_open, &kept);
  assert_true(kept.strip.x == resting.strip.x && kept.strip.y == resting.strip.y);
  assert_false(kept.card_shown);
  assert_true(kept.clipped);
  _check(&card_open, &kept, &resting, FALSE);
}

/**
 * A placement that found no room is no placement to keep, and no place to measure a movement from:
 * the next one is exactly the one an opening would have found.
 */
static void _a_hidden_placement_is_not_where_the_next_one_starts(void **state)
{
  (void)state;
  scene_t scene;
  _frame_scene(&scene);
  scene_t narrow;
  _scene_copy(&narrow, &scene);
  narrow.input.view.width = 200.0;
  dt_canvas_place_t hidden;
  _solve(&narrow, &hidden);
  assert_false(hidden.visible);

  scene_t fresh;
  _scene_copy(&fresh, &scene);
  dt_canvas_place_t opened;
  _solve(&fresh, &opened);
  assert_true(opened.visible);
  scene_t resolved;
  _scene_copy(&resolved, &scene);
  resolved.input.reason = DT_CANVAS_PLACE_RESOLVE;
  resolved.input.previous = &hidden;
  dt_canvas_place_t again;
  _solve(&resolved, &again);
  assert_true(_same(&opened, &again));
  resolved.input.reason = DT_CANVAS_PLACE_GROW;
  resolved.input.card_open = TRUE;
  resolved.input.card_content_height = 200.0;
  _solve(&resolved, &again);
  assert_true(again.visible);
  _check(&resolved, &again, &hidden, TRUE);
}

/** A coordinate that is not a number, or not a pixel, places nothing, rather than a strip that is not a number. */
static void _a_scene_that_is_not_a_number_places_nothing(void **state)
{
  (void)state;
  const double nan_value = NAN;
  for(int field = 0; field < 6; field++)
  {
    scene_t scene;
    _frame_scene(&scene);
    if(field == 0) scene.input.anchor_x = nan_value;
    if(field == 1) scene.input.anchor_y = INFINITY;
    if(field == 2) scene.input.view.width = nan_value;
    if(field == 3) scene.input.width = nan_value;
    if(field == 4) scene.input.air = nan_value;
    // So far out that neighbouring pixels are the same double: not a screen either.
    if(field == 5) _translate(&scene, 1e17, 0.0);
    dt_canvas_place_t place;
    dt_canvas_place_solve(&scene.input, &place);
    assert_false(place.visible);
    assert_false(place.card_shown);
  }
}

/**
 * Two spots below the anchor that cost exactly the same -- both reach under it, at the same height --
 * go to the one on the left, as documented, whichever the search happens to look at first: the one
 * nearer the anchor's own column is the one on the right.
 */
static void _equal_costs_go_to_the_smaller_top_then_the_smaller_left(void **state)
{
  (void)state;
  scene_t scene;
  _scene_defaults(&scene);
  scene.input.view = (dt_canvas_place_rect_t){ 0.0, 0.0, 200.0, 200.0 };
  scene.input.air = 2.0;
  scene.input.width = 40.0;
  scene.input.strip_height = 20.0;
  _shape_add(&scene, DT_CANVAS_PLACE_HARD, 112.0, 40.0, 1.0, 60.0, FALSE);
  scene.input.anchor_x = 100.0;
  scene.input.anchor_y = 50.0;
  dt_canvas_place_t place;
  _solve(&scene, &place);
  assert_true(place.visible);
  assert_true(place.strip.y == 52.0);
  assert_true(place.strip.x == 60.0);
  assert_int_equal(place.growth, DT_CANVAS_PLACE_DOWN);
  _check(&scene, &place, NULL, TRUE);

  // Two tops in one free stretch, both holding the anchor -- no distance, the same side -- tie: the
  // smaller wins, though the search tries the larger first, the object's bottom edge clamped into
  // the stretch.
  scene_t stretch;
  _scene_defaults(&stretch);
  stretch.input.view = (dt_canvas_place_rect_t){ 0.0, 0.0, 200.0, 200.0 };
  stretch.input.width = 40.0;
  stretch.input.strip_height = 20.0;
  _shape_add(&stretch, DT_CANVAS_PLACE_HARD, -10.0, 80.0, 220.0, 15.0, FALSE);
  _shape_add(&stretch, DT_CANVAS_PLACE_HARD, -10.0, 117.0, 220.0, 13.0, FALSE);
  stretch.input.anchor_x = 100.0;
  stretch.input.anchor_y = 100.0;
  _solve(&stretch, &place);
  assert_true(place.visible);
  assert_true(place.strip.x == 100.0);
  assert_true(place.strip.y == 95.0);
  _check(&stretch, &place, NULL, TRUE);
}

static void _a_clear_spot_far_from_the_anchor_is_still_found(void **state)
{
  (void)state;
  scene_t scene;
  _scene_defaults(&scene);
  scene.input.view = (dt_canvas_place_rect_t){ 0.0, 0.0, 1000.0, 200.0 };
  scene.input.air = 2.0;
  scene.input.width = 50.0;
  scene.input.strip_height = 20.0;
  // Bars the full height of the view every ten pixels up to 800, too close for the strip to pass
  // between: well over the columns searched first, all of them near the anchor and all blocked.
  for(int bar = 0; bar <= 80; bar++)
    _shape_add(&scene, DT_CANVAS_PLACE_HARD, bar * 10.0, 0.0, 2.0, 200.0, bar < 4);
  scene.input.anchor_x = 100.0;
  scene.input.anchor_y = 100.0;
  dt_canvas_place_t place;
  _solve(&scene, &place);
  assert_true(place.visible);
  assert_int_equal(place.level, DT_CANVAS_PLACE_LEVEL_HPB);
  assert_true(place.strip.x >= 804.0);
  _check(&scene, &place, NULL, TRUE);
}

/**
 * A free stretch exactly as tall as the strip holds it, however the search got there. Bars fill the
 * view but for one opening by the anchor, where two bands leave exactly the strip's height: the card
 * fits nowhere, so both card rungs sweep the columns' runs before the strip alone is asked for at a
 * level already swept -- and the one column that holds it is among those searched first.
 */
static void _a_stretch_exactly_as_tall_as_the_strip_holds_it(void **state)
{
  (void)state;
  scene_t scene;
  _scene_defaults(&scene);
  scene.input.view = (dt_canvas_place_rect_t){ 0.0, 0.0, 1000.0, 200.0 };
  scene.input.air = 2.0;
  scene.input.width = 50.0;
  scene.input.strip_height = 20.0;
  for(int bar = 0; bar <= 100; bar++)
  {
    if(bar >= 45 && bar <= 50) continue;
    _shape_add(&scene, DT_CANVAS_PLACE_HARD, bar * 10.0, 0.0, 2.0, 200.0, FALSE);
  }
  _shape_add(&scene, DT_CANVAS_PLACE_HARD, 440.0, 0.0, 72.0, 88.0, FALSE);
  _shape_add(&scene, DT_CANVAS_PLACE_HARD, 440.0, 112.0, 72.0, 88.0, FALSE);
  scene.input.anchor_x = 475.0;
  scene.input.anchor_y = 100.0;
  scene.input.card_open = TRUE;
  scene.input.card_content_height = 50.0;
  scene.input.card_min = 10.0;
  dt_canvas_place_t place;
  _solve(&scene, &place);
  assert_true(place.visible);
  assert_int_equal(place.level, DT_CANVAS_PLACE_LEVEL_HPB);
  assert_false(place.card_shown);
  assert_true(place.strip.y == 90.0);
  _check(&scene, &place, NULL, TRUE);
}

static void _handles_everywhere_hide_the_properties(void **state)
{
  (void)state;
  scene_t scene;
  _scene_defaults(&scene);
  scene.input.view = (dt_canvas_place_rect_t){ 0.0, 0.0, 400.0, 300.0 };
  scene.input.width = 100.0;
  scene.input.strip_height = 20.0;
  // Handles 30 pixels across every 40: no gap wider than 10 pixels anywhere.
  for(int row = 0; row < 8; row++)
    for(int column = 0; column < 10; column++)
      _shape_add(&scene, DT_CANVAS_PLACE_HARD, column * 40.0 + 5.0, row * 40.0 - 5.0, 30.0, 30.0, FALSE);
  scene.input.anchor_x = 200.0;
  scene.input.anchor_y = 150.0;
  dt_canvas_place_t place;
  _solve(&scene, &place);
  assert_false(place.visible);
  _check(&scene, &place, NULL, TRUE);
  // A strip that fits a gap gets in.
  scene.input.width = 10.0;
  scene.input.strip_height = 10.0;
  _solve(&scene, &place);
  assert_true(place.visible);
  _check(&scene, &place, NULL, TRUE);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_a_placement_keeps_every_promise_in_ten_thousand_scenes),
    cmocka_unit_test(_a_cubic_connector_keeps_its_tangents_and_waypoint_clear),
    cmocka_unit_test(_a_straight_connector_leaves_room_for_the_tangents_a_click_would_add),
    cmocka_unit_test(_a_free_line_keeps_its_ends_and_the_arc_a_click_would_bend_clear),
    cmocka_unit_test(_sites_become_rectangles_holding_every_point_they_catch),
    cmocka_unit_test(_a_scene_moved_by_whole_pixels_moves_its_placement_by_as_much),
    cmocka_unit_test(_a_small_motion_leaves_the_properties_where_they_are),
    cmocka_unit_test(_a_fraction_of_a_pixel_is_carried_to_the_next_motion),
    cmocka_unit_test(_growing_the_card_keeps_the_strip_and_collapsing_it_too),
    cmocka_unit_test(_an_open_card_is_shown_wherever_it_fits),
    cmocka_unit_test(_a_search_after_a_pan_can_stay_where_the_properties_are),
    cmocka_unit_test(_an_object_filling_the_view_is_covered_as_little_as_possible),
    cmocka_unit_test(_over_the_body_a_placement_covers_no_more_and_grows_no_card),
    cmocka_unit_test(_a_near_tie_resolves_the_same_way_wherever_the_scene_sits),
    cmocka_unit_test(_a_hidden_placement_is_not_where_the_next_one_starts),
    cmocka_unit_test(_a_scene_that_is_not_a_number_places_nothing),
    cmocka_unit_test(_equal_costs_go_to_the_smaller_top_then_the_smaller_left),
    cmocka_unit_test(_a_clear_spot_far_from_the_anchor_is_still_found),
    cmocka_unit_test(_a_stretch_exactly_as_tall_as_the_strip_holds_it),
    cmocka_unit_test(_handles_everywhere_hide_the_properties),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
