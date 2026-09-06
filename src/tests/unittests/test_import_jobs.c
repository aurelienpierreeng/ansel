/*
    This file is part of Ansel,
    Copyright (C) 2026 Paolo SANTUCCI.

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

/** Regression coverage for the capture-sequence grouping used during copy-import.
 *
 * Files that share the same capture (same path up to the final extension) must receive
 * the same 1-based $(SEQUENCE) number, so a RAW+JPEG pair lands under one shared name.
 * Different captures advance the counter. Dots in parent directory names must not be
 * mistaken for extensions, and paths without extensions must still be treated as distinct
 * captures.
 */

#include "control/jobs/import_jobs.h"
#include "system/mem_alloc.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>
#include <glib.h>

static GHashTable *_new_capture_table(void)
{
  return g_hash_table_new_full(g_str_hash, g_str_equal, dt_free_gpointer, NULL);
}

static void test_same_capture_raw_jpeg_share_sequence(void **state)
{
  (void)state;
  GHashTable *captures = _new_capture_table();
  int sequence = 0;

  assert_int_equal(dt_control_import_capture_sequence(captures, "/import/capture.raw", &sequence), 1);
  assert_int_equal(dt_control_import_capture_sequence(captures, "/import/capture.jpg", &sequence), 1);
  assert_int_equal(sequence, 1);

  g_hash_table_destroy(captures);
}

static void test_different_capture_gets_next_sequence(void **state)
{
  (void)state;
  GHashTable *captures = _new_capture_table();
  int sequence = 0;

  assert_int_equal(dt_control_import_capture_sequence(captures, "/import/capture_a.raw", &sequence), 1);
  assert_int_equal(dt_control_import_capture_sequence(captures, "/import/capture_b.raw", &sequence), 2);
  assert_int_equal(sequence, 2);

  g_hash_table_destroy(captures);
}

static void test_dot_in_parent_directory_ignored(void **state)
{
  (void)state;
  GHashTable *captures = _new_capture_table();
  int sequence = 0;

  gchar *dir = g_build_filename("import", "2026.09.05", NULL);
  gchar *capture_a = g_build_filename(dir, "capture_a", NULL);
  gchar *capture_b = g_build_filename(dir, "capture_b", NULL);
  const gchar *alternate_separator = G_DIR_SEPARATOR == '/' ? "\\" : "/";
  gchar *alternate_capture_a = g_strjoin(alternate_separator, "import", "2026.09.05", "capture_a", NULL);
  gchar *alternate_capture_b = g_strjoin(alternate_separator, "import", "2026.09.05", "capture_b", NULL);

  assert_int_equal(dt_control_import_capture_sequence(captures, capture_a, &sequence), 1);
  assert_int_equal(dt_control_import_capture_sequence(captures, capture_b, &sequence), 2);
  assert_int_equal(dt_control_import_capture_sequence(captures, alternate_capture_a, &sequence), 3);
  assert_int_equal(dt_control_import_capture_sequence(captures, alternate_capture_b, &sequence), 4);
  assert_int_equal(sequence, 4);

  dt_free(dir);
  dt_free(capture_a);
  dt_free(capture_b);
  dt_free(alternate_capture_a);
  dt_free(alternate_capture_b);
  g_hash_table_destroy(captures);
}

static void test_no_extension_separate_paths(void **state)
{
  (void)state;
  GHashTable *captures = _new_capture_table();
  int sequence = 0;

  assert_int_equal(dt_control_import_capture_sequence(captures, "/import/capture_a", &sequence), 1);
  assert_int_equal(dt_control_import_capture_sequence(captures, "/import/capture_b", &sequence), 2);
  assert_int_equal(sequence, 2);

  g_hash_table_destroy(captures);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_same_capture_raw_jpeg_share_sequence),
    cmocka_unit_test(test_different_capture_gets_next_sequence),
    cmocka_unit_test(test_dot_in_parent_directory_ignored),
    cmocka_unit_test(test_no_extension_separate_paths),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
