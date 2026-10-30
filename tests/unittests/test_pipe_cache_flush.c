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

/** A flush frees nothing somebody holds.
 *
 * A backbuffer keepalive, or a module keeping its preview across runs, holds a reference and no
 * lock. The flush used to spare locked lines only: it freed a held one, and the holder's release
 * then decremented freed memory (issue #1546).
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

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

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

/* A written line, still referenced once by its creator and no longer locked: what a keepalive holds. */
static dt_pixel_cache_entry_t *_create_held(void)
{
  void *data = NULL;
  dt_pixel_cache_entry_t *entry = NULL;
  assert_int_equal(dt_dev_pixelpipe_cache_get(HASH_A, LINE_BYTES, "test line", 0, TRUE, &data, &entry), 1);
  assert_non_null(entry);
  assert_non_null(data);
  dt_dev_pixelpipe_cache_wrlock_entry(FALSE, entry);
  return entry;
}

/** Held through the flush: the line stays, with its reference, and its holder releases it safely. */
static void _held_line_survives_the_flush(void **state __attribute__((unused)))
{
  dt_pixel_cache_entry_t *entry = _create_held();

  dt_dev_pixelpipe_cache_flush(-1);
  assert_ptr_equal(dt_dev_pixelpipe_cache_get_entry(HASH_A), entry);
  assert_int_equal(dt_atomic_get_int(&entry->refcount), 1);

  dt_dev_pixelpipe_cache_unref_entry(entry);
}

/** Released, the same line goes with the next flush: the guard spares holders, not lines. */
static void _released_line_goes_with_the_flush(void **state __attribute__((unused)))
{
  dt_pixel_cache_entry_t *entry = _create_held();
  dt_dev_pixelpipe_cache_unref_entry(entry);

  dt_dev_pixelpipe_cache_flush(-1);
  assert_null(dt_dev_pixelpipe_cache_get_entry(HASH_A));
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(_held_line_survives_the_flush, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_released_line_goes_with_the_flush, _setup, _teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
