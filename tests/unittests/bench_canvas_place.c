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

/* What one placement of the floating properties costs on a busy screen: 2048 shapes, 64 of them
 * soft, on a 2560 x 1440 view -- the pieces of a long connector's band, the slabs of a large
 * frame, the handles of a many-node cutout, the frames around. The solve runs in idles and toggle
 * handlers, so the number to watch is the time a click on the card's toggle waits. Run:
 *
 *   ./tests/unittests/bench_canvas_place
 *
 * Two cases are the worst a solve gets. An object filling the view leaves no room clear of it, so
 * both levels clear of the body are searched before it is given up; and a view crowded with handles
 * everywhere but far from the anchor fails every column near the anchor, so the columns further out
 * are searched. Both used to grow with the square of the shapes -- 9.2 ms for the first, measured,
 * where the budget is 2 ms -- because every one of a number of columns that grows with the shapes
 * walked every shape.
 *
 * Every case is held to a bound five times the 2 ms budget: generous, since a machine under load
 * must not turn the suite red for a timing, and still below what walking every column costs on the
 * crowded case, so the search growing with the square again is what fails it. That case is itself
 * over the budget, at 3 ms: 2048 handles one can grab, scattered over the whole view with room left
 * only in a corner, is not a screen the atelier draws, and it is kept for what it catches.
 */

#include "canvas/canvas_place.h"

#include <glib.h>
#include <math.h>
// cmocka.h declares `extern jmp_buf global_expect_assert_env' without including <setjmp.h>.
#include <setjmp.h>  // NOLINT(misc-include-cleaner)
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <cmocka.h>

#define BENCH_SHAPES 2048
#define BENCH_SOFT 64
#define BENCH_ROUNDS 200
/* Five times the 2 ms a solve is budgeted, in an optimised build. An unoptimised one runs the solver
 * about two and a half times slower -- the crowded case, measured: 7.5 ms against 3.0 -- and is
 * given twice the bound, which the search walking every column still exceeds twice over there
 * (42.5 ms unoptimised, 15.7 ms optimised). */
#ifdef __OPTIMIZE__
#define BENCH_BOUND_MS 10.0
#else
#define BENCH_BOUND_MS 20.0
#endif

static uint64_t _random_state = 0x9E3779B97F4A7C15ull;

static double _random_unit(void)
{
  _random_state ^= _random_state << 13;
  _random_state ^= _random_state >> 7;
  _random_state ^= _random_state << 17;
  return (double)(_random_state >> 11) / (double)(1ull << 53);
}

static void _rect(dt_canvas_place_shape_t *shape, const dt_canvas_place_class_t shape_class, const double x,
                  const double y, const double width, const double height, const gboolean target)
{
  shape->rect.x = x;
  shape->rect.y = y;
  shape->rect.width = width;
  shape->rect.height = height;
  shape->shape_class = shape_class;
  shape->weight = shape_class == DT_CANVAS_PLACE_SOFT ? DT_CANVAS_PLACE_WEIGHT_FRAME : 0.0;
  shape->target = target;
}

/** Milliseconds per solve, over a number of rounds. */
static double _time_solve(const dt_canvas_place_input_t *input, dt_canvas_place_t *result)
{
  const gint64 start = g_get_monotonic_time();
  for(int round = 0; round < BENCH_ROUNDS; round++) dt_canvas_place_solve(input, result);
  return (double)(g_get_monotonic_time() - start) / 1000.0 / BENCH_ROUNDS;
}

static void _bench(void **state)
{
  (void)state;
  dt_canvas_place_shape_t *shapes = g_new0(dt_canvas_place_shape_t, BENCH_SHAPES);
  size_t count = 0;
  // A cubic connector's band laid across the view as 16-pixel pieces, winding.
  for(int piece = 0; piece < 1200; piece++)
  {
    const double along = piece / 1200.0;
    const double x = 80.0 + along * 2400.0;
    const double y = 720.0 + 420.0 * sin(along * 6.0 * G_PI);
    _rect(&shapes[count], DT_CANVAS_PLACE_HARD, x - 5.0, y - 5.0, 12.0, 12.0, TRUE);
    count++;
  }
  // A large frame's body as slabs, and a cutout's handles scattered over it, predicted.
  for(int slab = 0; slab < 16; slab++)
  {
    _rect(&shapes[count], DT_CANVAS_PLACE_BODY, 900.0, 300.0 + slab * 40.0, 700.0, 40.0, TRUE);
    count++;
  }
  while(count < (size_t)(BENCH_SHAPES - BENCH_SOFT))
  {
    const gboolean hard = _random_unit() < 0.5;
    _rect(&shapes[count], hard ? DT_CANVAS_PLACE_HARD : DT_CANVAS_PLACE_PREDICTED, 900.0 + 700.0 * _random_unit(),
          300.0 + 640.0 * _random_unit(), 16.0, 16.0, hard);
    count++;
  }
  // The frames around, better left uncovered.
  while(count < BENCH_SHAPES)
  {
    _rect(&shapes[count], DT_CANVAS_PLACE_SOFT, 2560.0 * _random_unit(), 1440.0 * _random_unit(),
          80.0 + 400.0 * _random_unit(), 80.0 + 300.0 * _random_unit(), FALSE);
    count++;
  }

  dt_canvas_place_input_t input;
  memset(&input, 0, sizeof(input));
  input.view = (dt_canvas_place_rect_t){ 6.0, 6.0, 2548.0, 1428.0 };
  input.air = 6.0;
  input.shapes = shapes;
  input.shape_count = count;
  input.anchor_x = 1250.0;
  input.anchor_y = 940.0;
  input.width = 460.0;
  input.strip_height = 32.0;
  input.card_max = 420.0;
  input.card_min = 120.0;
  input.reason = DT_CANVAS_PLACE_OPEN;
  dt_canvas_place_t opened;
  const double open_ms = _time_solve(&input, &opened);

  input.card_open = TRUE;
  input.card_content_height = 380.0;
  input.reason = DT_CANVAS_PLACE_GROW;
  input.previous = &opened;
  dt_canvas_place_t grown;
  const double grow_ms = _time_solve(&input, &grown);

  input.card_open = FALSE;
  input.reason = DT_CANVAS_PLACE_RESOLVE;
  input.anchor_x += 40.0;
  dt_canvas_place_t kept;
  const double resolve_ms = _time_solve(&input, &kept);

  // The object fills the view: both levels clear of the body have no room anywhere.
  input.anchor_x = 1250.0;
  input.reason = DT_CANVAS_PLACE_OPEN;
  input.previous = NULL;
  for(int slab = 0; slab < 16; slab++)
    _rect(&shapes[1200 + slab], DT_CANVAS_PLACE_BODY, 0.0, slab * 90.0, 2560.0, 90.0, TRUE);
  dt_canvas_place_t covered;
  const double covered_ms = _time_solve(&input, &covered);

  // Handles everywhere left of the last 480 pixels, each at a column of its own, and thin tall
  // pieces of a body in those 480: no column holds the strip clear of the body, so both levels clear
  // of it are searched through every column, and the only room is over the body at the far right.
  count = 0;
  for(int piece = 0; piece < 5; piece++)
  {
    _rect(&shapes[count], DT_CANVAS_PLACE_BODY, 2100.0 + piece * 100.0, 0.0, 4.0, 1440.0, TRUE);
    count++;
  }
  while(count < BENCH_SHAPES)
  {
    _rect(&shapes[count], DT_CANVAS_PLACE_HARD, 2080.0 * _random_unit(), 1440.0 * _random_unit(), 12.0, 12.0, FALSE);
    count++;
  }
  input.width = 400.0;
  // The card opened there: its ladder asks every level twice, with the card and without.
  dt_canvas_place_t crowded_strip;
  dt_canvas_place_solve(&input, &crowded_strip);
  input.card_open = TRUE;
  input.card_content_height = 380.0;
  input.reason = DT_CANVAS_PLACE_GROW;
  input.previous = &crowded_strip;
  dt_canvas_place_t crowded;
  const double crowded_ms = _time_solve(&input, &crowded);

  fprintf(stderr, "[bench_canvas_place] %d shapes (%d soft): open %.3f ms (level %d), grow %.3f ms, "
                  "resolve %.3f ms, over the body %.3f ms (level %d), crowded %.3f ms (at x %.0f level %d)\n",
          BENCH_SHAPES, BENCH_SOFT, open_ms, (int)opened.level, grow_ms, resolve_ms, covered_ms, (int)covered.level,
          crowded_ms, crowded.strip.x, (int)crowded.level);
  assert_true(covered.visible && covered.level == DT_CANVAS_PLACE_LEVEL_H);
  assert_true(crowded_strip.visible && crowded_strip.level == DT_CANVAS_PLACE_LEVEL_H && crowded_strip.strip.x >= 1900.0);
  assert_true(crowded.visible && crowded.level == DT_CANVAS_PLACE_LEVEL_H);
  assert_true(open_ms < BENCH_BOUND_MS);
  assert_true(grow_ms < BENCH_BOUND_MS);
  assert_true(resolve_ms < BENCH_BOUND_MS);
  assert_true(covered_ms < BENCH_BOUND_MS);
  assert_true(crowded_ms < BENCH_BOUND_MS);
  g_free(shapes);
}

int main(void)
{
  const struct CMUnitTest tests[] = { cmocka_unit_test(_bench) };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
