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

/** The cache counts what it refuses for lack of memory, per thread.
 *
 * A pixelpipe run that fails returns the same error whether a module failed or the cache refused
 * it a buffer. An export reads dt_pixelpipe_cache_get_alloc_refusals() around the run to tell the
 * two apart, and shows "Out of RAM" on the thumbnail instead of "Processing error". That holds
 * only if every refusal is counted once, nothing else is, and a refusal on another thread is not.
 *
 * These tests drive the cache alone, without a pipe: the contract is the cache's.
 */

#include "caches/pixelpipe_cache.h"
#include "system/macros.h"

#include <glib.h>
#include <stdarg.h>
#include <stddef.h>
// cmocka.h declares `extern jmp_buf global_expect_assert_env' at file scope without including
// <setjmp.h> itself. Same suppression as test_metadata_notify.c, and for the same reason.
#include <setjmp.h>  // NOLINT(misc-include-cleaner)
#include <stdint.h>
#include <cmocka.h>

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

#define MIB ((size_t)1024 * 1024)
#define CACHE_BYTES (64 * MIB)
#define HASH_HELD 0x0A0A0A0A0A0A0A0Aull

static int _setup(void **state __attribute__((unused)))
{
  return dt_dev_pixelpipe_cache_init(CACHE_BYTES, FALSE, FALSE) ? 0 : -1;
}

static int _teardown(void **state __attribute__((unused)))
{
  dt_dev_pixelpipe_cache_cleanup();
  return 0;
}

/** A buffer the cache can give is not a refusal. */
static void _granted_allocation_is_not_counted(void **state __attribute__((unused)))
{
  const uint32_t before = dt_pixelpipe_cache_get_alloc_refusals();

  void *buffer = dt_pixelpipe_cache_alloc_align_cache_impl(MIB, 0, "granted");
  assert_non_null(buffer);
  assert_int_equal(dt_pixelpipe_cache_get_alloc_refusals(), before);

  dt_pixelpipe_cache_free_align_cache(&buffer, "granted");
}

/** More than the whole cache: the arena has no run that long. */
static void _request_larger_than_the_cache_is_counted_once(void **state __attribute__((unused)))
{
  const uint32_t before = dt_pixelpipe_cache_get_alloc_refusals();

  assert_null(dt_pixelpipe_cache_alloc_align_cache_impl(2 * CACHE_BYTES, 0, "too large"));
  assert_int_equal(dt_pixelpipe_cache_get_alloc_refusals(), before + 1);
}

/** The budget is spent by a line still in use, so nothing can be evicted to make room. */
static void _budget_spent_by_a_line_in_use_is_counted_once(void **state __attribute__((unused)))
{
  void *data = NULL;
  dt_pixel_cache_entry_t *held = NULL;
  // Created referenced and write-locked, the way a module holds its output while it writes it.
  assert_int_equal(dt_dev_pixelpipe_cache_get_writable(HASH_HELD, 48 * MIB, "held line", 0, TRUE, FALSE, NULL,
                                                       NULL, &data, &held),
                   DT_DEV_PIXELPIPE_CACHE_WRITABLE_CREATED);
  assert_non_null(data);

  const uint32_t before = dt_pixelpipe_cache_get_alloc_refusals();
  assert_null(dt_pixelpipe_cache_alloc_align_cache_impl(32 * MIB, 0, "no room"));
  assert_int_equal(dt_pixelpipe_cache_get_alloc_refusals(), before + 1);

  dt_dev_pixelpipe_cache_wrlock_entry(FALSE, held);
  dt_dev_pixelpipe_cache_ref_count_entry(FALSE, held);
}

static gpointer _refuse_on_this_thread(gpointer unused __attribute__((unused)))
{
  const uint32_t before = dt_pixelpipe_cache_get_alloc_refusals();
  void *buffer = dt_pixelpipe_cache_alloc_align_cache_impl(2 * CACHE_BYTES, 0, "too large");
  const uint32_t counted = dt_pixelpipe_cache_get_alloc_refusals() - before;
  return GUINT_TO_POINTER(IS_NULL_PTR(buffer) ? counted : G_MAXUINT);
}

/** A refusal is counted on the thread it was refused to, and read there only: an export reading
 * its own run must not see another pipe's. */
static void _refusal_on_another_thread_is_not_counted_here(void **state __attribute__((unused)))
{
  const uint32_t before = dt_pixelpipe_cache_get_alloc_refusals();

  GThread *other = g_thread_new("refused elsewhere", _refuse_on_this_thread, NULL);
  const guint counted_there = GPOINTER_TO_UINT(g_thread_join(other));

  assert_int_equal(counted_there, 1);
  assert_int_equal(dt_pixelpipe_cache_get_alloc_refusals(), before);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(_granted_allocation_is_not_counted, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_request_larger_than_the_cache_is_counted_once, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_budget_spent_by_a_line_in_use_is_counted_once, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_refusal_on_another_thread_is_not_counted_here, _setup, _teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
