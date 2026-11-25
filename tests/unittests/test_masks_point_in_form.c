/*
    This file is part of Ansel,
    Copyright (C) 2026 Guillaume Stutin.

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

/** The hit-test walks an outline with the skip ranges its producer cut out of it.
 *
 * The outline below is a square followed by a spike that comes back to the square's first
 * corner: the shape of a closed border whose last samples fold or come back over the first ones,
 * which dt_masks_outline_boundary_skips() hides with a range that resumes at the outline's end.
 * Every polygon the retouch dust brush made on a scan had one. Walked, the spike flips the parity
 * of what lies beside it; skipped, the contour closes on the first corner. */

#include "develop/masks/masks_functions.h"
#include "develop/masks_types.h"

#include <stdarg.h>
#include <stddef.h>
// cmocka.h declares `extern jmp_buf global_expect_assert_env' at file scope without including
// <setjmp.h> itself. Same suppression as the other tests here, and for the same reason.
#include <setjmp.h>  // NOLINT(misc-include-cleaner)
#include <stdint.h>
#include <cmocka.h>

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

/* One header point, then the square's corners (indices 1 to 4) and the spike (5 and 6). */
static const float _outline[] = {
  0.0f, 0.0f,                                       // header, not walked
  0.0f, 0.0f, 10.0f, 0.0f, 10.0f, 10.0f, 0.0f, 10.0f, // the square
  20.0f, 8.0f, 20.0f, 2.0f,                         // the spike, hidden
};
#define OUTLINE_START 1
#define OUTLINE_COUNT 7

static const float _in_square[2] = { 5.0f, 5.0f };
static const float _beside_square[2] = { 15.0f, 5.0f };

static int _hit(const float point[2], const dt_masks_skip_range_t *skips, const int skip_count)
{
  return dt_masks_point_in_form_exact(point, 1, _outline, OUTLINE_START, OUTLINE_COUNT, skips, skip_count);
}

static void _the_spike_flips_the_parity_when_walked(void **state)
{
  (void)state;
  assert_int_equal(_hit(_in_square, NULL, 0), -1);
  assert_int_equal(_hit(_beside_square, NULL, 0), 0);
}

static void _a_range_that_resumes_at_the_end_closes_on_the_first_sample(void **state)
{
  (void)state;
  const dt_masks_skip_range_t tail[] = { { .jump_from = 5, .resume_at = OUTLINE_COUNT } };
  assert_int_equal(_hit(_in_square, tail, 1), 0);
  assert_int_equal(_hit(_beside_square, tail, 1), -1);
}

static void _a_range_that_resumes_at_the_end_follows_no_other(void **state)
{
  (void)state;
  /* a range from the first sample, as a fold across the seam would make: the walk starts past
   * it, and closing on the first sample must not jump on through it and walk the square again */
  const dt_masks_skip_range_t skips[] = { { .jump_from = OUTLINE_START, .resume_at = 2 },
                                          { .jump_from = 5, .resume_at = OUTLINE_COUNT } };
  assert_int_equal(_hit(_in_square, skips, 2), 0);
  assert_int_equal(_hit(_beside_square, skips, 2), -1);
}

static void _a_range_that_does_not_move_forward_is_ignored(void **state)
{
  (void)state;
  const dt_masks_skip_range_t backward[] = { { .jump_from = 5, .resume_at = 3 } };
  assert_int_equal(_hit(_in_square, backward, 1), -1);
  const dt_masks_skip_range_t past_the_end[] = { { .jump_from = 5, .resume_at = OUTLINE_COUNT + 1 } };
  assert_int_equal(_hit(_in_square, past_the_end, 1), -1);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_the_spike_flips_the_parity_when_walked),
    cmocka_unit_test(_a_range_that_resumes_at_the_end_closes_on_the_first_sample),
    cmocka_unit_test(_a_range_that_resumes_at_the_end_follows_no_other),
    cmocka_unit_test(_a_range_that_does_not_move_forward_is_ignored),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
