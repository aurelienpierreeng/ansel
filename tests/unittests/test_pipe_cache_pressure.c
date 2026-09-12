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

/** How much the pixelpipe cache may hold while the machine stalls on memory.
 *
 * These tests exist because nothing else can see this policy: it changes no pixel and no hash,
 * only how much cache survives a stalling machine, and its failures were measured on live runs
 * that ended with systemd-oomd killing the process. Both are pinned below.
 */

#include "caches/pixelpipe_cache_pressure.h"

#include <stdarg.h>
#include <stddef.h>
// cmocka.h declares `extern jmp_buf global_expect_assert_env' at file scope without including
// <setjmp.h> itself. Same suppression as test_pipe_cache_policy.c, and for the same reason.
#include <setjmp.h>  // NOLINT(misc-include-cleaner)
#include <stdint.h>
#include <cmocka.h>

#define GIB ((size_t)1024 * 1024 * 1024)
#define MIB ((size_t)1024 * 1024)

static dt_pixelpipe_cache_pressure_t _plan_of(const size_t plan)
{
  dt_pixelpipe_cache_pressure_t p;
  dt_pixelpipe_cache_pressure_init(&p, plan);
  return p;
}

/** A fresh cache may use its whole plan, and reports it. */
static void _starts_at_the_plan(void **state)
{
  (void)state;
  dt_pixelpipe_cache_pressure_t p = _plan_of(16 * GIB);

  assert_int_equal(dt_pixelpipe_cache_pressure_budget(&p), 16 * GIB);
  assert_int_equal(dt_pixelpipe_cache_pressure_cap(&p), 16 * GIB);
  assert_int_equal(dt_pixelpipe_cache_pressure_reported(&p, 4 * GIB), 16 * GIB);
}

/** A stalled window sheds a quarter of what is held, half when the stall is severe. */
static void _sheds_a_share_of_what_is_held(void **state)
{
  (void)state;
  dt_pixelpipe_cache_pressure_t p = _plan_of(16 * GIB);

  assert_int_equal(dt_pixelpipe_cache_pressure_shed_target(&p, 0.12, 8 * GIB), 6 * GIB);
  assert_int_equal(dt_pixelpipe_cache_pressure_shed_target(&p, 0.40, 8 * GIB), 4 * GIB);

  // Under the threshold nothing is shed, however much is held.
  assert_int_equal(dt_pixelpipe_cache_pressure_shed_target(&p, 0.05, 8 * GIB),
                   DT_PIXELPIPE_CACHE_PRESSURE_KEEP);
}

/** THE STARTUP DEFECT: four kernel wake-ups in the first 12 s of a run, while the cache still
 *  held nothing, recorded a mark of 0 and pinned the budget at the floor for the next quarter
 *  of an hour. A cache holding less than the floor is not what the machine is short of. */
static void _an_empty_cache_is_not_the_cause(void **state)
{
  (void)state;
  dt_pixelpipe_cache_pressure_t p = _plan_of(16 * GIB);

  assert_int_equal(dt_pixelpipe_cache_pressure_shed_target(&p, 0.90, 0), DT_PIXELPIPE_CACHE_PRESSURE_KEEP);
  assert_int_equal(dt_pixelpipe_cache_pressure_shed_target(&p, 0.90, dt_pixelpipe_cache_pressure_floor(&p)),
                   DT_PIXELPIPE_CACHE_PRESSURE_KEEP);

  // Nothing was shed, so nothing lowered the budget either.
  assert_int_equal(dt_pixelpipe_cache_pressure_budget(&p), 16 * GIB);
  assert_int_equal(p.mark, dt_pixelpipe_cache_pressure_mark_top(&p));
}

/** A shed lowers the budget to what eviction actually left, and marks where pressure struck. */
static void _a_shed_lowers_the_budget_to_what_is_left(void **state)
{
  (void)state;
  dt_pixelpipe_cache_pressure_t p = _plan_of(16 * GIB);

  dt_pixelpipe_cache_pressure_after_shed(&p, 12 * GIB, 9 * GIB);
  assert_int_equal(p.mark, 12 * GIB);
  assert_int_equal(dt_pixelpipe_cache_pressure_budget(&p), 9 * GIB);

  // Entries in use can leave the footprint above the ceiling; the ceiling only falls.
  dt_pixelpipe_cache_pressure_after_shed(&p, 11 * GIB, 10 * GIB);
  assert_int_equal(dt_pixelpipe_cache_pressure_budget(&p), 9 * GIB);
}

/** However hard the pressure, the budget stays usable: a floor of an eighth of the plan. */
static void _the_budget_never_falls_under_the_floor(void **state)
{
  (void)state;
  dt_pixelpipe_cache_pressure_t p = _plan_of(16 * GIB);

  dt_pixelpipe_cache_pressure_after_shed(&p, 12 * GIB, 0);
  assert_int_equal(dt_pixelpipe_cache_pressure_budget(&p), dt_pixelpipe_cache_pressure_floor(&p));
  assert_int_equal(dt_pixelpipe_cache_pressure_budget(&p), 2 * GIB);
}

/** THE RECOVERY DEFECT: restoring the whole plan in ~80 s brought the stall back within half a
 *  minute of every recovery, five times in five minutes, once at 49% -- one point under
 *  systemd-oomd's limit. The ceiling must stay under the footprint that caused the stall until
 *  the mark itself has recovered, which takes minutes. */
static void _recovery_waits_under_the_mark(void **state)
{
  (void)state;
  dt_pixelpipe_cache_pressure_t p = _plan_of(16 * GIB);
  dt_pixelpipe_cache_pressure_after_shed(&p, 12 * GIB, 8 * GIB);

  // The ceiling climbs, but never past 7/8 of the mark ...
  for(int window = 0; window < 16; window++)
  {
    dt_pixelpipe_cache_pressure_relax(&p);
    assert_true(p.ceiling <= dt_pixelpipe_cache_pressure_cap(&p));
  }
  assert_true(dt_pixelpipe_cache_pressure_budget(&p) < 12 * GIB);

  // ... and the mark rises far more slowly than the ceiling: the 1/1024 of a plan per 2 s
  // window is minutes of calm before the size that stalled is offered again.
  assert_true(p.mark < 13 * GIB);
}

/** And it gets there in the end, continuously: the ceiling reaches the plan by climbing, never
 *  by a jump when the mark tops out. */
static void _recovery_reaches_the_plan_without_a_jump(void **state)
{
  (void)state;
  dt_pixelpipe_cache_pressure_t p = _plan_of(16 * GIB);
  dt_pixelpipe_cache_pressure_after_shed(&p, 12 * GIB, 8 * GIB);

  size_t previous = dt_pixelpipe_cache_pressure_budget(&p);
  const size_t step = p.plan / 32;
  for(int window = 0; window < 4096; window++)
  {
    if(!dt_pixelpipe_cache_pressure_relax(&p)) continue;
    const size_t now = dt_pixelpipe_cache_pressure_budget(&p);
    assert_true(now >= previous);
    assert_true(now - previous <= step);
    previous = now;
  }
  assert_int_equal(dt_pixelpipe_cache_pressure_budget(&p), 16 * GIB);
}

/** The reported budget never drops under what is held: both readers compute `max - current`
 *  unsigned, and a lowered ceiling would wrap it into a huge number. */
static void _reported_budget_never_underflows_its_readers(void **state)
{
  (void)state;
  dt_pixelpipe_cache_pressure_t p = _plan_of(16 * GIB);
  dt_pixelpipe_cache_pressure_after_shed(&p, 12 * GIB, 3 * GIB);

  const size_t held = 11 * GIB; // still pinned by a running pipe
  assert_true(dt_pixelpipe_cache_pressure_budget(&p) < held);
  assert_int_equal(dt_pixelpipe_cache_pressure_reported(&p, held), held);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_starts_at_the_plan),
    cmocka_unit_test(_sheds_a_share_of_what_is_held),
    cmocka_unit_test(_an_empty_cache_is_not_the_cause),
    cmocka_unit_test(_a_shed_lowers_the_budget_to_what_is_left),
    cmocka_unit_test(_the_budget_never_falls_under_the_floor),
    cmocka_unit_test(_recovery_waits_under_the_mark),
    cmocka_unit_test(_recovery_reaches_the_plan_without_a_jump),
    cmocka_unit_test(_reported_budget_never_underflows_its_readers),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
