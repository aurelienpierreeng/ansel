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

/** Contracts of database/image_repository.h that the migration promised to keep.
 *
 * The first test is a regression test for a real bug: the callers this repository absorbed
 * used to bind a comma-joined id string to a single `IN (?)` parameter, which SQLite reads
 * as ONE value -- it matched a lone id and matched NOTHING for two or more. The repository
 * builds the set into the statement as integers; these tests hold it there.
 */

#include "testdb.h"


// an arbitrary flag bit with no side meaning in these tests
#define TEST_FLAG 2048

static void test_flag_among_multi_image(void **state)
{
  (void)state;
  const int32_t film = testdb_make_film("/testdb/filmroll");
  assert_true(film > 0);
  const int32_t a = testdb_make_image(film, "a.raw");
  const int32_t b = testdb_make_image(film, "b.raw");
  const int32_t c = testdb_make_image(film, "c.raw");
  assert_true(a > 0 && b > 0 && c > 0);

  GList *two = g_list_append(g_list_append(NULL, GINT_TO_POINTER(a)), GINT_TO_POINTER(b));
  GList *all = g_list_append(g_list_copy(two), GINT_TO_POINTER(c));

  // set the flag on TWO images at once -- the case the old IN (?) binding silently dropped
  assert_true(dt_image_repository_set_flag_among(two, TEST_FLAG));

  GList *flagged = dt_image_repository_get_ids_with_flag_among(all, TEST_FLAG);
  assert_int_equal(g_list_length(flagged), 2);
  assert_int_equal(GPOINTER_TO_INT(flagged->data), a);
  assert_int_equal(GPOINTER_TO_INT(flagged->next->data), b);
  g_list_free(flagged);

  // a single-element list keeps working (the only case the old binding handled)
  GList *one = g_list_append(NULL, GINT_TO_POINTER(a));
  flagged = dt_image_repository_get_ids_with_flag_among(one, TEST_FLAG);
  assert_int_equal(g_list_length(flagged), 1);
  assert_int_equal(GPOINTER_TO_INT(flagged->data), a);
  g_list_free(flagged);

  // an empty set asks about nothing
  assert_null(dt_image_repository_get_ids_with_flag_among(NULL, TEST_FLAG));

  g_list_free(one);
  g_list_free(two);
  g_list_free(all);
}

static void test_full_paths(void **state)
{
  (void)state;
  const int32_t film = testdb_make_film("/testdb/paths");
  const int32_t a = testdb_make_image(film, "a.raw");
  const int32_t b = testdb_make_image(film, "b.raw");
  assert_true(a > 0 && b > 0);

  GList *ids = g_list_append(g_list_append(NULL, GINT_TO_POINTER(a)), GINT_TO_POINTER(b));
  GList *paths = dt_image_repository_get_full_paths(ids);
  assert_int_equal(g_list_length(paths), 2);
  assert_string_equal((const char *)paths->data, "/testdb/paths/a.raw");
  assert_string_equal((const char *)paths->next->data, "/testdb/paths/b.raw");
  g_list_free_full(paths, g_free);
  g_list_free(ids);
}

static void test_id_range(void **state)
{
  (void)state;
  const int32_t film = testdb_make_film("/testdb/range");
  const int32_t a = testdb_make_image(film, "a.raw");
  const int32_t b = testdb_make_image(film, "b.raw");
  const int32_t c = testdb_make_image(film, "c.raw");
  assert_true(a > 0 && c > a);

  assert_int_equal(dt_image_repository_count_in_id_range(a, c), 3);
  assert_int_equal(dt_image_repository_count_in_id_range(b, b), 1);
  assert_int_equal(dt_image_repository_count_in_id_range(c + 1, c + 100), 0);
}

static void test_write_timestamp_is_64bit(void **state)
{
  (void)state;
  const int32_t film = testdb_make_film("/testdb/ts");
  const int32_t a = testdb_make_image(film, "a.raw");
  assert_true(a > 0);

  // 2100-01-01: overflows a 32-bit time_t. The caller this setter replaced bound the
  // timestamp with sqlite3_bind_int(), which truncates in 2038.
  const int64_t year2100 = 4102444800LL;
  assert_true(dt_image_repository_set_write_timestamp(a, year2100));
  assert_true(dt_image_repository_get_write_timestamp(a) == year2100);
}

static void test_group_member_rows(void **state)
{
  (void)state;
  const int32_t film = testdb_make_film("/testdb/group");
  const int32_t a = testdb_make_image(film, "a.raw");
  const int32_t b = testdb_make_image(film, "b.raw");
  assert_true(a > 0 && b > 0);
  assert_true(dt_image_repository_set_group(a, a));
  assert_true(dt_image_repository_set_group(b, a));

  GList *members = dt_image_repository_get_group_member_rows(a);
  assert_int_equal(g_list_length(members), 2);
  const dt_image_group_member_t *first = (const dt_image_group_member_t *)members->data;
  const dt_image_group_member_t *second = (const dt_image_group_member_t *)members->next->data;
  assert_int_equal(first->imgid, a);
  assert_string_equal(first->filename, "a.raw");
  assert_int_equal(second->imgid, b);
  assert_string_equal(second->filename, "b.raw");
  g_list_free_full(members, dt_image_group_member_free);
}

static void test_count_distinct_fields(void **state)
{
  (void)state;
  const int32_t film = testdb_make_film("/testdb/distinct");
  const int32_t a = testdb_make_image(film, "a.raw");
  const int32_t b = testdb_make_image(film, "b.raw");
  assert_true(a > 0 && b > 0);

  GList *ids = g_list_append(g_list_append(NULL, GINT_TO_POINTER(a)), GINT_TO_POINTER(b));
  int *counts = dt_image_repository_count_distinct_fields(ids);
  assert_non_null(counts);

  assert_int_equal(counts[DT_IMAGE_FIELD_FILM_ROLL], 1); // same film -> agree
  assert_int_equal(counts[DT_IMAGE_FIELD_FILENAME], 2);  // different names -> disagree
  assert_int_equal(counts[DT_IMAGE_FIELD_IMGID], 2);     // ids differ by definition
  assert_int_equal(counts[DT_IMAGE_FIELD_VERSION], 1);   // both imported at version 0

  free(counts);
  g_list_free(ids);

  // the empty set answers nothing rather than something
  assert_null(dt_image_repository_count_distinct_fields(NULL));
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_flag_among_multi_image),
    cmocka_unit_test(test_full_paths),
    cmocka_unit_test(test_id_range),
    cmocka_unit_test(test_write_timestamp_is_64bit),
    cmocka_unit_test(test_group_member_rows),
    cmocka_unit_test(test_count_distinct_fields),
  };
  return cmocka_run_group_tests(tests, testdb_setup, testdb_teardown);
}
