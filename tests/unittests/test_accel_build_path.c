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

/** The contract of dt_accels_build_path() (widgets/accelerators.c): `feature` is one level.
 *
 * Accel paths are built from GUI labels, most of them translated, and "/" is the only structure
 * a path has. The French catalogue translated the geotagging module "Date/heure et GPS", which
 * tripped the assertion that used to forbid a "/" in a module name -- in a Debug build only, while
 * a release build silently registered the module one level too deep. A user-typed instance name
 * has no catalogue to review at all. The function now writes a "/" from a label as "-", and these
 * tests pin that, together with the nesting that callers build one level at a time.
 */

#include <glib.h>

#include <stdarg.h>
#include <stddef.h>
// cmocka.h declares `extern jmp_buf global_expect_assert_env' at file scope without including
// <setjmp.h> itself. Same suppression as the other tests here, and for the same reason.
#include <setjmp.h>  // NOLINT(misc-include-cleaner)
#include <stdint.h>
#include <cmocka.h>

#include "system/mem_alloc.h"  // dt_free
#include "widgets/accelerators.h"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

/** A "/" in the label stays inside its level. */
static void _a_slash_in_the_feature_stays_one_level(void **state __attribute__((unused)))
{
  gchar *path = dt_accels_build_path("Lighttable/Toolboxes", "Date/heure et GPS");
  assert_string_equal(path, "<Ansel>/Lighttable/Toolboxes/Date-heure et GPS");
  dt_free(path);
}

/** The scope is a hierarchy: its "/" are levels and are kept. */
static void _the_scope_keeps_its_levels(void **state __attribute__((unused)))
{
  gchar *path = dt_accels_build_path("Darkroom/Modules/Instances", "Exposure");
  assert_string_equal(path, "<Ansel>/Darkroom/Modules/Instances/Exposure");
  dt_free(path);
}

/** A path built so far is a scope: the root is not repeated, and each level is cleaned on its
 * own -- the way a module instance and a module control are nested. */
static void _nested_levels_are_built_one_at_a_time(void **state __attribute__((unused)))
{
  gchar *module_path = dt_accels_build_path("Darkroom/Modules/Instances", "Tone/curve");
  gchar *path = dt_accels_build_path(module_path, "sky/ground");
  assert_string_equal(module_path, "<Ansel>/Darkroom/Modules/Instances/Tone-curve");
  assert_string_equal(path, "<Ansel>/Darkroom/Modules/Instances/Tone-curve/sky-ground");
  dt_free(path);
  dt_free(module_path);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_a_slash_in_the_feature_stays_one_level),
    cmocka_unit_test(_the_scope_keeps_its_levels),
    cmocka_unit_test(_nested_levels_are_built_one_at_a_time),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
