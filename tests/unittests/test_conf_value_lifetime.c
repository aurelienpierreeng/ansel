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

/** Lifetime of the string dt_conf_get_string_const() hands out.
 *
 * That function returns a pointer INTO the conf hash table, and dt_conf_get_var() releases
 * the lock before returning it. Replacing the value used to free it on the spot -- the
 * table's value_destroy_func is a free -- so any reader still holding the pointer was
 * reading freed memory. With 98 call sites and no way to tell from a signature which of
 * them could race a writer, the fix was made in the writer: displaced values are retired
 * and released only at cleanup.
 *
 * These tests pin that. The first one is the bug: read, overwrite, read again through the
 * ORIGINAL pointer.
 *
 * Checked against the defect rather than only against the fix. Reverting the writer to its
 * free-on-replace form and running under MALLOC_PERTURB_=170 -- which fills freed memory,
 * so a use-after-free stops being silent without needing a full ASAN build -- fails three
 * of the four, reporting the fill pattern where the value should be:
 *
 *     [  ERROR   ] --- "\357\277\275\357\277\275" != "11"
 *
 * The fourth, _rewriting_the_same_value_retires_nothing(), passes EITHER WAY and is not a
 * regression detector: it asserts pointer identity, and the old code frees then immediately
 * reallocates the same size, so the allocator hands back the same address. It is here to
 * pin the dedup property -- that an unchanged write retires nothing -- not the lifetime.
 */

#include "common/conf.h"
#include "darktable.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>  // calloc/free, used directly below
// cmocka.h declares `extern jmp_buf global_expect_assert_env' at file scope without including
// <setjmp.h> itself. Same suppression as test_pipe_cache_policy.c, and for the same reason.
#include <setjmp.h>  // NOLINT(misc-include-cleaner)
#include <stdint.h>
#include <cmocka.h>

/** A key with no confgen declaration, so nothing clamps or sanitises what we store. */
#define TEST_KEY "plugins/test/conf_value_lifetime"

static char *_rcfile = NULL;

static int _setup(void **state)
{
  (void)state;
  // dt_conf_init() writes through the darktable.conf global, so it must point at the
  // instance being initialised before the call -- exactly as dt_init() does it.
  _rcfile = g_build_filename(g_get_tmp_dir(), "ansel_test_conf_lifetime.rc", NULL);
  g_remove(_rcfile);
  darktable.conf = (dt_conf_t *)calloc(1, sizeof(dt_conf_t));
  dt_conf_init(darktable.conf, _rcfile, NULL);
  return 0;
}

static int _teardown(void **state)
{
  (void)state;
  dt_conf_cleanup(darktable.conf);
  free(darktable.conf);
  darktable.conf = NULL;
  g_remove(_rcfile);
  g_free(_rcfile);
  _rcfile = NULL;
  return 0;
}

/** The bug, pinned: a borrowed pointer survives the write that displaces it. */
static void _borrowed_value_survives_being_replaced(void **state)
{
  (void)state;
  dt_conf_set_string(TEST_KEY, "first");

  const char *borrowed = dt_conf_get_string_const(TEST_KEY);
  assert_string_equal(borrowed, "first");

  // The write that used to free `borrowed` out from under us.
  dt_conf_set_string(TEST_KEY, "second");

  // Still readable, and still the value it was read as. Under ASAN, a regression here is a
  // heap-use-after-free rather than a failed assertion.
  assert_string_equal(borrowed, "first");

  // ... while a fresh read observes the new value. Stale, not wrong.
  assert_string_equal(dt_conf_get_string_const(TEST_KEY), "second");
}

/** Many successive writes, all of them retired rather than freed. */
static void _every_displaced_value_stays_readable(void **state)
{
  (void)state;
  const char *held[8];

  for(int i = 0; i < 8; i++)
  {
    char *value = g_strdup_printf("value-%d", i);
    dt_conf_set_string(TEST_KEY, value);
    g_free(value);
    held[i] = dt_conf_get_string_const(TEST_KEY);
  }

  // Every pointer taken along the way still reads as what it was when taken.
  for(int i = 0; i < 8; i++)
  {
    char *expected = g_strdup_printf("value-%d", i);
    assert_string_equal(held[i], expected);
    g_free(expected);
  }
}

/** Rewriting a key with the value it already holds must change nothing at all.
 *
 * This is what keeps the retired list proportional to real edits: GUI state is written back
 * constantly with values that did not change, and retiring a string for each of those would
 * make the list grow with idle churn instead. Pointer IDENTITY is the observable. */
static void _rewriting_the_same_value_retires_nothing(void **state)
{
  (void)state;
  dt_conf_set_string(TEST_KEY, "unchanged");
  const char *before = dt_conf_get_string_const(TEST_KEY);

  for(int i = 0; i < 16; i++) dt_conf_set_string(TEST_KEY, "unchanged");

  // Same allocation, so nothing was displaced and nothing was retired.
  assert_ptr_equal(before, dt_conf_get_string_const(TEST_KEY));
  assert_string_equal(before, "unchanged");
}

/** The typed setters go through the same writer, so they inherit the same guarantee. */
static void _typed_setters_retire_too(void **state)
{
  (void)state;
  dt_conf_set_int(TEST_KEY, 11);
  const char *borrowed = dt_conf_get_string_const(TEST_KEY);
  assert_string_equal(borrowed, "11");

  dt_conf_set_int(TEST_KEY, 22);

  assert_string_equal(borrowed, "11");
  assert_int_equal(dt_conf_get_int_fast(TEST_KEY), 22);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_borrowed_value_survives_being_replaced),
    cmocka_unit_test(_every_displaced_value_stays_readable),
    cmocka_unit_test(_rewriting_the_same_value_retires_nothing),
    cmocka_unit_test(_typed_setters_retire_too),
  };

  return cmocka_run_group_tests(tests, _setup, _teardown);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
