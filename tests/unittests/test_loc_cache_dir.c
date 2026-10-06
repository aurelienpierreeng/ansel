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

/** Where the default user cache directory is, per platform.
 *
 * `dt_loc_default_user_cache_dir()` is the one definition of that path: `dt_loc_init_user_cache_dir()`
 * takes its default from it, and the two callers that run BEFORE dt_loc_init() -- main()'s Windows log
 * redirection and the usage text naming that log -- resolve it through the same function so they cannot
 * name different directories.
 *
 * The regression these tests exist for is Windows-specific and invisible on this machine: GLib's
 * `g_get_user_cache_dir()` returns FOLDERID_InternetCache there -- the shell folder still labelled
 * "Temporary Internet Files" -- which Storage Sense clears by default on Windows 11. A thumbnail cache
 * written there is deleted by the OS behind the user's back and a log file written there is unfindable
 * (#1473). The last test below is the guard, and it asserts on the base directory rather than on the
 * string, so it says something true on every platform and fails on the one that regresses.
 *
 * The XDG tests are the part that runs meaningfully here: `XDG_CACHE_HOME` must win on every platform,
 * because a caller who sets it means it, and GLib honours it on Windows too.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

#include <glib.h>
#include <string.h>

#include "common/file_location.h"
#include "system/macros.h"     // IS_NULL_PTR
#include "system/mem_alloc.h"  // dt_free

#define XDG_KEY "XDG_CACHE_HOME"

/* An absolute path that is never created and never touched: these tests exercise string
 * resolution only, so the directory does not have to exist. Deliberately NOT under /tmp --
 * naming a publicly writable directory in a test invites a symlink-attack finding (SonarCloud
 * c:S5443) for a path nothing ever opens. */
#define FAKE_ABSOLUTE_DIR G_DIR_SEPARATOR_S "nonexistent-ansel-test" G_DIR_SEPARATOR_S "xdg-cache-home"

/* Restored by the teardown: the variable is process-wide and the other tests in this binary must not
 * inherit whatever one of these set. A NULL saved value means it was not set at all. */
static gchar *_saved_xdg = NULL;
static gboolean _had_xdg = FALSE;

static int _setup(void **state)
{
  const gchar *current = g_getenv(XDG_KEY);
  _had_xdg = !IS_NULL_PTR(current);
  _saved_xdg = _had_xdg ? g_strdup(current) : NULL;
  return 0;
}

static int _teardown(void **state)
{
  if(_had_xdg)
    g_setenv(XDG_KEY, _saved_xdg, TRUE);
  else
    g_unsetenv(XDG_KEY);

  dt_free(_saved_xdg);
  _had_xdg = FALSE;
  return 0;
}

static void _xdg_cache_home_wins(void **state)
{
  g_setenv(XDG_KEY, FAKE_ABSOLUTE_DIR, TRUE);

  gchar *expected = g_build_filename(FAKE_ABSOLUTE_DIR, "ansel", NULL);
  gchar *got = dt_loc_default_user_cache_dir();

  assert_non_null(got);
  assert_string_equal(got, expected);

  dt_free(got);
  dt_free(expected);
}

static void _an_empty_xdg_is_ignored(void **state)
{
  /* An exported-but-empty variable is how a shell spells "not set"; taking it literally would put
   * the cache at the filesystem root. */
  g_setenv(XDG_KEY, "", TRUE);

  gchar *got = dt_loc_default_user_cache_dir();

  assert_non_null(got);
  assert_true(g_path_is_absolute(got));
  assert_string_not_equal(got, G_DIR_SEPARATOR_S "ansel");

  dt_free(got);
}

static void _a_relative_xdg_is_ignored(void **state)
{
  /* The XDG specification calls a relative path in one of these variables invalid, and honouring one
   * would make the cache location depend on the working directory. */
  g_setenv(XDG_KEY, "relative-cache", TRUE);

  gchar *got = dt_loc_default_user_cache_dir();

  assert_non_null(got);
  assert_true(g_path_is_absolute(got));
  assert_null(strstr(got, "relative-cache"));

  dt_free(got);
}

static void _the_default_is_not_a_shell_managed_temporary_folder(void **state)
{
  g_unsetenv(XDG_KEY);

  gchar *got = dt_loc_default_user_cache_dir();
  assert_non_null(got);
  assert_true(g_path_is_absolute(got));

  /* The base the platform must resolve against. On Windows this is deliberately NOT
   * g_get_user_cache_dir(): see the file comment. Same resolution order as the function under
   * test, so this says the right thing on a Windows box whose %LOCALAPPDATA% is unset. */
#ifdef _WIN32
  const gchar *base = g_getenv("LOCALAPPDATA");
  if(IS_NULL_PTR(base) || !base[0]) base = g_get_user_data_dir();
#else
  const gchar *base = g_get_user_cache_dir();
#endif
  assert_non_null(base);
  assert_true(g_str_has_prefix(got, base));

  /* Named explicitly as well as structurally: g_get_user_data_dir() and g_get_user_cache_dir() are
   * the same directory on some platforms, so the prefix test alone would not catch a return to
   * INetCache everywhere. */
  assert_null(strstr(got, "INetCache"));
  assert_null(strstr(got, "Temporary Internet Files"));

  dt_free(got);
}

static void _every_call_answers_the_same(void **state)
{
  /* Two callers resolve this independently -- main() before dt_loc_init(), and
   * dt_loc_init_user_cache_dir() during it -- and they must land on the same directory. */
  g_unsetenv(XDG_KEY);

  gchar *first = dt_loc_default_user_cache_dir();
  gchar *second = dt_loc_default_user_cache_dir();

  assert_non_null(first);
  assert_non_null(second);
  assert_ptr_not_equal(first, second);
  assert_string_equal(first, second);

  dt_free(second);
  dt_free(first);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_xdg_cache_home_wins),
    cmocka_unit_test(_an_empty_xdg_is_ignored),
    cmocka_unit_test(_a_relative_xdg_is_ignored),
    cmocka_unit_test(_the_default_is_not_a_shell_managed_temporary_folder),
    cmocka_unit_test(_every_call_answers_the_same),
  };

  return cmocka_run_group_tests(tests, _setup, _teardown);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
