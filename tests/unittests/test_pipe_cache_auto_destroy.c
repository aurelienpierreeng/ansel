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

/** A disposable cache line goes with its last release, in the same hold of the cache lock.
 *
 * To drop a line it holds, a caller flags it, then releases it. Releasing first and naming the line
 * afterwards, to remove or to flag it, leaves a window between two holds of the lock in which another
 * thread's eviction can free it, and the second call then works on freed memory.
 *
 * These tests drive the cache alone, without a pipe: the contract is the cache's.
 */

#include "caches/pixelpipe_cache.h"
#include "system/atomic.h"

#include <stdarg.h>
#include <stddef.h>
// cmocka.h declares `extern jmp_buf global_expect_assert_env' at file scope without including
// <setjmp.h> itself. Same suppression as test_metadata_notify.c, and for the same reason.
#include <setjmp.h>  // NOLINT(misc-include-cleaner)
#include <stdint.h>
#include <cmocka.h>

#define LINE_BYTES (256 * 1024)
#define HASH_A 0x0A0A0A0A0A0A0A0Aull

static int _setup(void **state __attribute__((unused)))
{
  return dt_dev_pixelpipe_cache_init((size_t)64 * 1024 * 1024, FALSE, FALSE) ? 0 : -1;
}

static int _teardown(void **state __attribute__((unused)))
{
  dt_dev_pixelpipe_cache_cleanup();
  return 0;
}

/* Create a line the way a module creates a side-band output: referenced once and write-locked. */
static dt_pixel_cache_entry_t *_create(void)
{
  void *data = NULL;
  dt_pixel_cache_entry_t *entry = NULL;
  assert_int_equal(dt_dev_pixelpipe_cache_get(HASH_A, LINE_BYTES, "test line", 0, TRUE, &data, &entry), 1);
  assert_non_null(entry);
  assert_non_null(data);
  return entry;
}

/** Flagged while held, then released: the release removes it. */
static void _flagged_line_goes_with_its_release(void **state __attribute__((unused)))
{
  dt_pixel_cache_entry_t *entry = _create();
  dt_dev_pixelpipe_cache_flag_auto_destroy(entry);
  dt_dev_pixelpipe_cache_wrlock_entry(FALSE, entry);
  dt_dev_pixelpipe_cache_ref_count_entry(FALSE, entry);

  assert_null(dt_dev_pixelpipe_cache_get_entry(HASH_A));
}

/** Another holder keeps it, out of reach of any retained lookup, and the last release removes it. */
static void _flagged_line_goes_with_its_last_holder(void **state __attribute__((unused)))
{
  dt_pixel_cache_entry_t *entry = _create();
  dt_dev_pixelpipe_cache_wrlock_entry(FALSE, entry);
  dt_dev_pixelpipe_cache_ref_count_entry(TRUE, entry);

  dt_dev_pixelpipe_cache_flag_auto_destroy(entry);
  dt_dev_pixelpipe_cache_ref_count_entry(FALSE, entry);
  assert_ptr_equal(dt_dev_pixelpipe_cache_get_entry(HASH_A), entry);
  assert_int_equal(dt_atomic_get_int(&entry->refcount), 1);

  dt_pixel_cache_entry_t *held = NULL;
  assert_false(dt_dev_pixelpipe_cache_ref_entry_by_hash(HASH_A, NULL, &held));
  assert_null(held);

  dt_dev_pixelpipe_cache_ref_count_entry(FALSE, entry);
  assert_null(dt_dev_pixelpipe_cache_get_entry(HASH_A));
}

/** A line nobody flagged stays after its last release: it is kept for reuse. */
static void _unflagged_line_outlives_its_release(void **state __attribute__((unused)))
{
  dt_pixel_cache_entry_t *entry = _create();
  dt_dev_pixelpipe_cache_wrlock_entry(FALSE, entry);
  dt_dev_pixelpipe_cache_ref_count_entry(FALSE, entry);

  dt_pixel_cache_entry_t *held = NULL;
  assert_true(dt_dev_pixelpipe_cache_ref_entry_by_hash(HASH_A, NULL, &held));
  assert_ptr_equal(held, entry);
  dt_dev_pixelpipe_cache_ref_count_entry(FALSE, held);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(_flagged_line_goes_with_its_release, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_flagged_line_goes_with_its_last_holder, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_unflagged_line_outlives_its_release, _setup, _teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
