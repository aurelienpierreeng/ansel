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

/** What dt_conf_flush() leaves on disk.
 *
 * The settings file is replaced, not rewritten in place: a process that dies during a save must
 * leave the previous file whole. The observable, without killing anything, is a handle opened on
 * the old file before the save: a replacement leaves it reading the old content, a rewrite in
 * place ("wb") truncates it under the reader.
 *
 * The tests share a directory of their own, so that what is left beside the settings file --
 * the temporary file of the replacement -- can be counted.
 *
 * Checked against the defect: with the save put back to g_fopen(filename, "wb"),
 * _flush_replaces_the_file_rather_than_rewriting_it() fails on `old_intact`, the old handle
 * reading the rewritten file. The other two pass either way: they pin what is written and what a
 * failure returns, not how the file is replaced.
 */

#include "common/conf.h"
#include "darktable.h"
#include "system/macros.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>  // calloc/free, used directly below
#include <string.h>
// cmocka.h declares `extern jmp_buf global_expect_assert_env' at file scope without including
// <setjmp.h> itself. Same suppression as test_pipe_cache_policy.c, and for the same reason.
#include <setjmp.h>  // NOLINT(misc-include-cleaner)
#include <stdint.h>
#include <cmocka.h>

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

/** A key with no confgen declaration, so nothing clamps or sanitises what we store. */
#define TEST_KEY "plugins/test/conf_save"

static char *_dir = NULL;
static char *_rcfile = NULL;

static int _count_entries(const char *dir)
{
  GDir *d = g_dir_open(dir, 0, NULL);
  assert_non_null(d);
  int count = 0;
  while(g_dir_read_name(d)) count++;
  g_dir_close(d);
  return count;
}

static int _setup(void **state __attribute__((unused)))
{
  _dir = g_dir_make_tmp("ansel_test_conf_save_XXXXXX", NULL);
  if(IS_NULL_PTR(_dir)) return -1;
  _rcfile = g_build_filename(_dir, "anselrc", NULL);
  // dt_conf_init() writes through the darktable.conf global, so it must point at the
  // instance being initialised before the call -- exactly as dt_init() does it.
  darktable.conf = (dt_conf_t *)calloc(1, sizeof(dt_conf_t));
  dt_conf_init(darktable.conf, _rcfile, NULL);
  return 0;
}

static int _teardown(void **state __attribute__((unused)))
{
  dt_conf_cleanup(darktable.conf);
  free(darktable.conf);
  darktable.conf = NULL;

  // Everything the tests left in the directory: the settings file and the blocker of
  // _a_failed_flush_returns_1_and_writes_nothing().
  GDir *d = g_dir_open(_dir, 0, NULL);
  if(!IS_NULL_PTR(d))
  {
    const char *name = NULL;
    while((name = g_dir_read_name(d)))
    {
      char *path = g_build_filename(_dir, name, NULL);
      g_remove(path);
      g_free(path);
    }
    g_dir_close(d);
  }
  g_rmdir(_dir);
  g_free(_rcfile);
  g_free(_dir);
  _rcfile = NULL;
  _dir = NULL;
  return 0;
}

/** A value set in memory is in the file once the flush returns, and the temporary file the
 * replacement went through is gone. */
static void _flush_writes_what_was_set(void **state __attribute__((unused)))
{
  dt_conf_set_string(TEST_KEY, "saved");
  assert_int_equal(dt_conf_flush(), 0);

  char *contents = NULL;
  assert_true(g_file_get_contents(_rcfile, &contents, NULL, NULL));
  const gboolean written = !IS_NULL_PTR(strstr(contents, TEST_KEY "=saved\n"));
  g_free(contents);
  assert_true(written);

  assert_int_equal(_count_entries(_dir), 1);
}

/** A handle opened on the old file still reads the old content after the flush: the file was
 * replaced. A rewrite in place would have truncated it under the reader. */
static void _flush_replaces_the_file_rather_than_rewriting_it(void **state __attribute__((unused)))
{
#ifdef _WIN32
  // Windows refuses to rename over a file that is open without FILE_SHARE_DELETE, which a
  // stdio handle is not: the observable this test relies on does not exist there.
  skip();
#endif
  dt_conf_set_string(TEST_KEY, "before");
  assert_int_equal(dt_conf_flush(), 0);

  FILE *old = g_fopen(_rcfile, "rb");
  assert_non_null(old);

  dt_conf_set_string(TEST_KEY, "after");
  assert_int_equal(dt_conf_flush(), 0);

  // Read the whole old file through the handle opened before the second flush.
  GString *seen = g_string_new(NULL);
  char buffer[4096];
  size_t n = 0;
  while((n = fread(buffer, 1, sizeof(buffer), old)) > 0) g_string_append_len(seen, buffer, n);
  fclose(old);
  const gboolean old_intact = !IS_NULL_PTR(strstr(seen->str, TEST_KEY "=before\n"));
  g_string_free(seen, TRUE);
  assert_true(old_intact);

  char *contents = NULL;
  assert_true(g_file_get_contents(_rcfile, &contents, NULL, NULL));
  const gboolean new_written = !IS_NULL_PTR(strstr(contents, TEST_KEY "=after\n"));
  g_free(contents);
  assert_true(new_written);
}

/** A flush that cannot write returns 1 and leaves nothing behind. The settings file is pointed
 * below a regular file, which no platform and no user, root included, can create a file in. */
static void _a_failed_flush_returns_1_and_writes_nothing(void **state __attribute__((unused)))
{
  char *blocker = g_build_filename(_dir, "not-a-directory", NULL);
  assert_true(g_file_set_contents(blocker, "x", 1, NULL));
  char *unwritable = g_build_filename(blocker, "anselrc", NULL);
  const int entries = _count_entries(_dir);

  char saved[sizeof(darktable.conf->filename)];
  g_strlcpy(saved, darktable.conf->filename, sizeof(saved));
  g_strlcpy(darktable.conf->filename, unwritable, sizeof(darktable.conf->filename));

  dt_conf_set_string(TEST_KEY, "unwritable");
  const int result = dt_conf_flush();

  g_strlcpy(darktable.conf->filename, saved, sizeof(darktable.conf->filename));
  g_free(unwritable);

  char *contents = NULL;
  assert_true(g_file_get_contents(blocker, &contents, NULL, NULL));
  const gboolean blocker_intact = !g_strcmp0(contents, "x");
  g_free(contents);
  g_free(blocker);

  assert_int_equal(result, 1);
  assert_true(blocker_intact);
  assert_int_equal(_count_entries(_dir), entries);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_flush_writes_what_was_set),
    cmocka_unit_test(_flush_replaces_the_file_rather_than_rewriting_it),
    cmocka_unit_test(_a_failed_flush_returns_1_and_writes_nothing),
  };

  return cmocka_run_group_tests(tests, _setup, _teardown);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
