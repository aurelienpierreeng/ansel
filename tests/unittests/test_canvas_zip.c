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

#include "canvas/canvas_zip.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <cmocka.h>

static char *_temp_path(const char *name)
{
  return g_build_filename(g_get_tmp_dir(), name, NULL);
}

static void _round_trip_preserves_stored_and_deflated_entries(void **state)
{
  (void)state;
  char *path = _temp_path("ansel-test-canvas-zip-roundtrip.zip");
  g_unlink(path);

  // A compressible text and an incompressible blob: one deflates, one is stored.
  GString *text = g_string_new("");
  for(int idx = 0; idx < 2000; idx++) g_string_append(text, "the quick brown fox jumps over the lazy dog\n");
  uint8_t noise[4096];
  uint32_t seed = 12345u;
  for(size_t idx = 0; idx < sizeof(noise); idx++)
  {
    seed = seed * 1103515245u + 12345u;
    noise[idx] = (uint8_t)(seed >> 16);
  }

  dt_canvas_zip_writer_t *writer = dt_canvas_zip_writer_open(path);
  assert_non_null(writer);
  assert_true(dt_canvas_zip_writer_add(writer, "mimetype", "application/x-test", 18, FALSE));
  assert_true(dt_canvas_zip_writer_add(writer, "texts/1.md", text->str, text->len, TRUE));
  assert_true(dt_canvas_zip_writer_add(writer, "images/1.jpg", noise, sizeof(noise), FALSE));
  assert_true(dt_canvas_zip_writer_add(writer, "empty", NULL, 0, TRUE));
  assert_true(dt_canvas_zip_writer_close(writer, TRUE));

  dt_canvas_zip_reader_t *reader = dt_canvas_zip_reader_open(path);
  assert_non_null(reader);
  assert_int_equal(dt_canvas_zip_reader_count(reader), 4);
  assert_string_equal(dt_canvas_zip_reader_name_at(reader, 0), "mimetype");
  assert_true(dt_canvas_zip_reader_has(reader, "images/1.jpg"));
  assert_false(dt_canvas_zip_reader_has(reader, "images/2.jpg"));

  GBytes *read_text = dt_canvas_zip_reader_get(reader, "texts/1.md");
  assert_non_null(read_text);
  gsize read_size = 0;
  const char *read_data = g_bytes_get_data(read_text, &read_size);
  assert_int_equal(read_size, text->len);
  assert_memory_equal(read_data, text->str, text->len);
  g_bytes_unref(read_text);

  GBytes *read_noise = dt_canvas_zip_reader_get(reader, "images/1.jpg");
  assert_non_null(read_noise);
  assert_int_equal(g_bytes_get_size(read_noise), sizeof(noise));
  assert_memory_equal(g_bytes_get_data(read_noise, NULL), noise, sizeof(noise));
  g_bytes_unref(read_noise);

  GBytes *read_empty = dt_canvas_zip_reader_get(reader, "empty");
  assert_non_null(read_empty);
  assert_int_equal(g_bytes_get_size(read_empty), 0);
  g_bytes_unref(read_empty);

  assert_null(dt_canvas_zip_reader_get(reader, "absent"));
  dt_canvas_zip_reader_close(reader);

  // Interoperability: a standard unzip must accept what we wrote, when one is installed.
  gchar *unzip = g_find_program_in_path("unzip");
  if(unzip != NULL)
  {
    gchar *argv[] = { unzip, "-t", path, NULL };
    gint exit_status = -1;
    assert_true(g_spawn_sync(NULL, argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
                             NULL, NULL, &exit_status, NULL));
    assert_int_equal(exit_status, 0);
    g_free(unzip);
  }

  g_string_free(text, TRUE);
  g_unlink(path);
  g_free(path);
}

static void _a_discarded_writer_leaves_no_file(void **state)
{
  (void)state;
  char *path = _temp_path("ansel-test-canvas-zip-discard.zip");
  g_unlink(path);
  dt_canvas_zip_writer_t *writer = dt_canvas_zip_writer_open(path);
  assert_non_null(writer);
  assert_true(dt_canvas_zip_writer_add(writer, "a", "x", 1, FALSE));
  assert_true(dt_canvas_zip_writer_close(writer, FALSE));
  assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
  char *part = g_strdup_printf("%s.part", path);
  assert_false(g_file_test(part, G_FILE_TEST_EXISTS));
  g_free(part);
  g_free(path);
}

static void _a_corrupted_entry_is_refused(void **state)
{
  (void)state;
  char *path = _temp_path("ansel-test-canvas-zip-corrupt.zip");
  g_unlink(path);
  dt_canvas_zip_writer_t *writer = dt_canvas_zip_writer_open(path);
  assert_non_null(writer);
  const char payload[] = "0123456789abcdef0123456789abcdef";
  assert_true(dt_canvas_zip_writer_add(writer, "stored", payload, sizeof(payload), FALSE));
  assert_true(dt_canvas_zip_writer_close(writer, TRUE));

  // Flip one payload byte: the local header is 30 bytes plus the 6-byte name.
  FILE *file = g_fopen(path, "r+b");
  assert_non_null(file);
  fseek(file, 30 + 6 + 4, SEEK_SET);
  fputc('X', file);
  fclose(file);

  dt_canvas_zip_reader_t *reader = dt_canvas_zip_reader_open(path);
  assert_non_null(reader);
  assert_null(dt_canvas_zip_reader_get(reader, "stored"));
  dt_canvas_zip_reader_close(reader);
  g_unlink(path);
  g_free(path);
}

static void _not_an_archive_does_not_open(void **state)
{
  (void)state;
  char *path = _temp_path("ansel-test-canvas-zip-notzip.bin");
  g_file_set_contents(path, "this is not a zip archive at all, just text", -1, NULL);
  assert_null(dt_canvas_zip_reader_open(path));
  g_unlink(path);
  g_free(path);
  assert_null(dt_canvas_zip_reader_open("/nonexistent/path/x.zip"));
  assert_null(dt_canvas_zip_writer_open(NULL));
}

static void _entry_names_are_validated(void **state)
{
  (void)state;
  char *path = _temp_path("ansel-test-canvas-zip-names.zip");
  dt_canvas_zip_writer_t *writer = dt_canvas_zip_writer_open(path);
  assert_non_null(writer);
  assert_false(dt_canvas_zip_writer_add(writer, "/absolute", "x", 1, FALSE));
  assert_false(dt_canvas_zip_writer_add(writer, "", "x", 1, FALSE));
  assert_true(dt_canvas_zip_writer_close(writer, FALSE));
  g_free(path);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_round_trip_preserves_stored_and_deflated_entries),
    cmocka_unit_test(_a_discarded_writer_leaves_no_file),
    cmocka_unit_test(_a_corrupted_entry_is_refused),
    cmocka_unit_test(_not_an_archive_does_not_open),
    cmocka_unit_test(_entry_names_are_validated),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
