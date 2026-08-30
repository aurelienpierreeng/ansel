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

/** The self-intersection cut mechanism, pinned as a producer/consumer pair.
 *
 * A polygon's border folds over itself at concave runs tighter than the feathering radius;
 * the folds are cut out of every crossing-count walk. The cuts used to travel IN-BAND, as NaN
 * jump sentinels encoded into the border buffer, and both bugs the mechanism ever shipped
 * lived in that encoding while the geometry was right: overlapping ranges once trapped the
 * reader in a cycle it exited silently with a garbage crossing count, and issue #1313 encoded
 * a fold straddling the buffer seam as its own complement, swallowing 99.8% of the contour
 * into one straight chord. These tests pin the out-of-band replacement against exactly those
 * two histories: the builder must refuse to emit what broke, and the walk must refuse to
 * follow it if handed anyway.
 */

#include "develop/masks_types.h"             // dt_masks_skip_range_t
#include "develop/masks/masks_functions.h"    // the builder and the walk

#include <stdarg.h>
#include <stddef.h>
// cmocka.h declares `extern jmp_buf global_expect_assert_env' at file scope without including
// <setjmp.h> itself, so this include is FOR cmocka.h and clang-tidy cannot attribute it.
#include <setjmp.h>  // IWYU pragma: keep
#include <stdint.h>
#include <cmocka.h>

/* ---------------------------------------------------------------------------------------
 * dt_masks_skip_ranges_build(): the producer half.
 * ------------------------------------------------------------------------------------- */

static void _build_normalizes_discovery_order(void **state)
{
  (void)state;
  // Discovery walks from a shape extremum, so either index of a pair can come first.
  const float pairs[] = { 20.f, 10.f };
  dt_masks_skip_range_t out[1];
  int dropped = -1;

  assert_int_equal(dt_masks_skip_ranges_build(pairs, 1, 100, out, &dropped), 1);
  assert_int_equal(out[0].jump_from, 10);
  assert_int_equal(out[0].resume_at, 20);
  assert_int_equal(dropped, 0);
}

static void _build_drops_a_fold_that_straddles_the_seam(void **state)
{
  (void)state;
  /* The literal pairs of issue #1313, on its literal border size. Each fold spans 330-454
   * points the short way round the closed contour; [min, max] would name the ~147000-point
   * complement, and merging would then swallow every other cut into one range covering 99.8%
   * of the shape -- the reported straight chord. */
  const float pairs[] = { 147487.f, 271.f,
                          147457.f, 301.f,
                          147503.f, 411.f,
                          95500.f, 95703.f };
  dt_masks_skip_range_t out[4];
  int dropped = -1;

  assert_int_equal(dt_masks_skip_ranges_build(pairs, 4, 147546, out, &dropped), 1);
  assert_int_equal(dropped, 3);
  // the one genuine forward fold survives untouched
  assert_int_equal(out[0].jump_from, 95500);
  assert_int_equal(out[0].resume_at, 95703);
}

static void _build_merges_overlapping_and_nested_ranges(void **state)
{
  (void)state;
  /* Overlapping ranges written independently are the cycle bug: two cuts pointing into each
   * other trapped the read walk until its visit cap fired, silently. Merged and disjoint,
   * every skip moves strictly forward. The (22, 18) pair is deliberately reversed AND nested. */
  const float pairs[] = { 10.f, 20.f, 15.f, 25.f, 30.f, 35.f, 22.f, 18.f };
  dt_masks_skip_range_t out[4];

  assert_int_equal(dt_masks_skip_ranges_build(pairs, 4, 100, out, NULL), 2);
  assert_int_equal(out[0].jump_from, 10);
  assert_int_equal(out[0].resume_at, 25);
  assert_int_equal(out[1].jump_from, 30);
  assert_int_equal(out[1].resume_at, 35);
  // disjoint and sorted IS the forward-only guarantee
  assert_true(out[0].resume_at < out[1].jump_from);
}

static void _build_rejects_garbage(void **state)
{
  (void)state;
  const float pairs[] = { -1.f, 5.f, 5.f, 5.f, 50.f, 150.f };
  dt_masks_skip_range_t out[3];
  int dropped = -1;

  // out-of-bounds and zero-length pairs vanish without becoming ranges OR "wrapping" stats
  assert_int_equal(dt_masks_skip_ranges_build(pairs, 3, 100, out, &dropped), 0);
  assert_int_equal(dropped, 0);

  assert_int_equal(dt_masks_skip_ranges_build(NULL, 3, 100, out, NULL), 0);
  assert_int_equal(dt_masks_skip_ranges_build(pairs, 0, 100, out, NULL), 0);
}

/* ---------------------------------------------------------------------------------------
 * dt_masks_point_in_form_exact(): the consumer half.
 *
 * The contour is a square with a bump on its top edge:
 *
 *        5 ----- 4
 *        |       |          bump: indices 3..6
 *   7 -- 6       3 -- 2
 *   |                 |
 *   0 --------------- 1
 *
 * Cutting the bump with the range {3 -> 7} closes the contour with the chord (10,10)-(0,10):
 * a point inside the bump is inside the full contour and OUTSIDE the cut one. That chord is
 * exactly what a skipped self-intersection fold renders as.
 * ------------------------------------------------------------------------------------- */

static const float _bumped_square[] = {
  0.f, 0.f,   10.f, 0.f,   10.f, 10.f,   6.f, 10.f,
  6.f, 14.f,  4.f, 14.f,   4.f, 10.f,    0.f, 10.f,
};
#define BUMPED_COUNT 8

static void _walk_answers_the_plain_contour(void **state)
{
  (void)state;
  const float inside[2] = { 5.f, 5.f };
  const float in_bump[2] = { 5.f, 12.f };
  const float outside[2] = { 20.f, 5.f };

  assert_int_equal(dt_masks_point_in_form_exact(inside, 1, _bumped_square, 0, BUMPED_COUNT, NULL, 0), 0);
  assert_int_equal(dt_masks_point_in_form_exact(in_bump, 1, _bumped_square, 0, BUMPED_COUNT, NULL, 0), 0);
  assert_int_equal(dt_masks_point_in_form_exact(outside, 1, _bumped_square, 0, BUMPED_COUNT, NULL, 0), -1);
}

static void _a_cut_closes_the_contour_with_a_chord(void **state)
{
  (void)state;
  const dt_masks_skip_range_t cut = { .jump_from = 3, .resume_at = 7 };
  const float in_bump[2] = { 5.f, 12.f };
  const float inside[2] = { 5.f, 5.f };

  // the bump is walked out of existence...
  assert_int_equal(dt_masks_point_in_form_exact(in_bump, 1, _bumped_square, 0, BUMPED_COUNT, &cut, 1), -1);
  // ...and the rest of the shape is untouched
  assert_int_equal(dt_masks_point_in_form_exact(inside, 1, _bumped_square, 0, BUMPED_COUNT, &cut, 1), 0);
}

static void _a_backward_cut_is_refused_not_followed(void **state)
{
  (void)state;
  /* The cycle bug, replayed at the consumer: a skip that moves the walk backwards would
   * re-walk the span it just left until the visit cap fires, and the crossing count would be
   * garbage served as an answer. The walk must IGNORE such a range -- same answers as with no
   * cuts at all -- and, above all, terminate. */
  const dt_masks_skip_range_t backward = { .jump_from = 3, .resume_at = 2 };
  const dt_masks_skip_range_t out_of_bounds = { .jump_from = 3, .resume_at = 99 };
  const float in_bump[2] = { 5.f, 12.f };

  assert_int_equal(dt_masks_point_in_form_exact(in_bump, 1, _bumped_square, 0, BUMPED_COUNT, &backward, 1), 0);
  assert_int_equal(dt_masks_point_in_form_exact(in_bump, 1, _bumped_square, 0, BUMPED_COUNT, &out_of_bounds, 1), 0);
}

static void _the_seam_bug_end_to_end(void **state)
{
  (void)state;
  /* Issue #1313 in miniature, producer to consumer: the detector reports the bump fold as the
   * pair (7, 3) -- discovery met the larger index first, and the fold happens to sit against
   * the walk's wrap. Encoded as [min, max] = [3, 7] it cuts the bump (correct, forward, short
   * arc). But a pair like (7, 1) whose short arc wraps the seam CANNOT be a forward range: the
   * builder must drop it, and the walk must then still see the bump. */
  const float wrapping_pair[] = { 7.f, 1.f };
  dt_masks_skip_range_t out[1];
  int dropped = 0;

  const int n = dt_masks_skip_ranges_build(wrapping_pair, 1, BUMPED_COUNT, out, &dropped);
  assert_int_equal(n, 0);
  assert_int_equal(dropped, 1);

  const float in_bump[2] = { 5.f, 12.f };
  assert_int_equal(dt_masks_point_in_form_exact(in_bump, 1, _bumped_square, 0, BUMPED_COUNT, out, n), 0);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_build_normalizes_discovery_order),
    cmocka_unit_test(_build_drops_a_fold_that_straddles_the_seam),
    cmocka_unit_test(_build_merges_overlapping_and_nested_ranges),
    cmocka_unit_test(_build_rejects_garbage),
    cmocka_unit_test(_walk_answers_the_plain_contour),
    cmocka_unit_test(_a_cut_closes_the_contour_with_a_chord),
    cmocka_unit_test(_a_backward_cut_is_refused_not_followed),
    cmocka_unit_test(_the_seam_bug_end_to_end),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
