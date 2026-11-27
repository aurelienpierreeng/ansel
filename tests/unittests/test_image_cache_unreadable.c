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

/** What the callers of dt_image_cache_get() do with an image they cannot read.
 *
 * The cache answers NULL for an image whose first database read failed, for as long as the
 * entry stays cached, although the row is there and other queries list it: group members,
 * duplicates. That is the state these tests put one image in, the way another program does --
 * a second connection holds the library's EXCLUSIVE lock while the image is asked for. Each
 * caller below dereferenced that NULL.
 */

#include "testdb.h"

#include "caches/image_cache.h"
#include "common/conf.h"
#include "common/grouping.h"
#include "common/image.h"
#include "darktable.h"

#include <glib/gstdio.h>
#include <math.h>
#include <stdlib.h>  // calloc/free, used directly below

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static gchar *_dir = NULL;
static gchar *_library = NULL;
static gchar *_rcfile = NULL;

/* A library on disk, opened anew for each test: in memory, every connection has a database of
 * its own and no other connection can lock it; and one successful read holds a read
 * transaction that the lock cannot pass, so each test needs a connection that has read
 * nothing yet. */
static int _setup(void **state)
{
  _dir = g_dir_make_tmp("ansel_test_image_cache_unreadable_XXXXXX", NULL);
  if(IS_NULL_PTR(_dir)) return -1;
  _library = g_build_filename(_dir, "library.db", NULL);

  testdb_paths_t *paths = g_new0(testdb_paths_t, 1);
  paths->configdir = g_dir_make_tmp("ansel-test-config-XXXXXX", NULL);
  paths->cachedir = g_dir_make_tmp("ansel-test-cache-XXXXXX", NULL);
  if(IS_NULL_PTR(paths->configdir) || IS_NULL_PTR(paths->cachedir))
  {
    if(!IS_NULL_PTR(paths->configdir)) g_rmdir(paths->configdir);
    if(!IS_NULL_PTR(paths->cachedir)) g_rmdir(paths->cachedir);
    g_free(paths->configdir);
    g_free(paths->cachedir);
    g_free(paths);
    return -1;
  }
  dt_loc_init_user_config_dir(paths->configdir);
  dt_loc_init_user_cache_dir(paths->cachedir);
  *state = paths;

  const dt_database_params_t params = { .alternative = _library,
                                        .library = NULL,
                                        .load_data = FALSE,
                                        .has_gui = FALSE,
                                        .verbose = FALSE };
  if(dt_database_open(&params) != DT_DATABASE_OPEN_OK) return -1;

  /* Populating a cache entry reads conf (dt_image_film_roll_name()); and a sidecar write is a
   * job, with no control to run it here. */
  _rcfile = g_build_filename(_dir, "anselrc", NULL);
  darktable.conf = (dt_conf_t *)calloc(1, sizeof(dt_conf_t));
  dt_conf_init(darktable.conf, _rcfile, NULL);
  dt_conf_set_string("write_sidecar_files", "FALSE");
  dt_image_xmp_mode_refresh_from_conf();

  dt_image_cache_init(FALSE);
  return 0;
}

static void _remove_tree(const gchar *dir)
{
  GDir *d = g_dir_open(dir, 0, NULL);
  if(IS_NULL_PTR(d)) return;
  const char *name = NULL;
  while((name = g_dir_read_name(d)))
  {
    gchar *path = g_build_filename(dir, name, NULL);
    // the empty backup marker dt_database_open() leaves is read-only, which Windows will not delete
    g_chmod(path, 0700);
    if(g_file_test(path, G_FILE_TEST_IS_DIR))
      _remove_tree(path);
    else
      g_remove(path);
    dt_free(path);
  }
  g_dir_close(d);
  g_rmdir(dir);
}

static int _teardown(void **state)
{
  dt_image_cache_cleanup();
  dt_conf_cleanup(darktable.conf);
  free(darktable.conf);
  darktable.conf = NULL;
  testdb_teardown(state);
  _remove_tree(_dir);
  dt_free(_rcfile);
  dt_free(_library);
  dt_free(_dir);
  return 0;
}

/** Ask for @p imgid while another connection holds the library: the read fails, and the
 * cache keeps answering NULL for it once the lock is gone. Call it before anything else of
 * the test reads an image. */
static void _make_unreadable(const int32_t imgid)
{
  sqlite3 *other = NULL;
  assert_int_equal(sqlite3_open(_library, &other), SQLITE_OK);
  assert_int_equal(sqlite3_exec(other, "BEGIN EXCLUSIVE", NULL, NULL, NULL), SQLITE_OK);
  const dt_image_t *img = dt_image_cache_get(imgid, 'r');
  // released before asserting, so that a failure does not leave the file locked for the teardown
  sqlite3_exec(other, "ROLLBACK", NULL, NULL, NULL);
  sqlite3_close(other);

  assert_null(img);
  assert_null(dt_image_cache_get(imgid, 'r'));
}

static gboolean _group_is(const int32_t group_id, const int32_t *members, const int count)
{
  GList *got = dt_image_repository_get_group_members(group_id, -1);
  gboolean same = (g_list_length(got) == (guint)count);
  for(int k = 0; k < count; k++)
    same = same && !IS_NULL_PTR(g_list_find(got, GINT_TO_POINTER(members[k])));
  g_list_free(got);
  return same;
}

/* Three images in a group led by `a`; `c`, the first in database order, cannot be read. */
static void _group_of_three(const char *folder, int32_t *a, int32_t *b, int32_t *c)
{
  const int32_t film = testdb_make_film(folder);
  assert_true(film > 0);
  *c = testdb_make_image(film, "c.raw");
  *a = testdb_make_image(film, "a.raw");
  *b = testdb_make_image(film, "b.raw");
  assert_true(*a > 0 && *b > 0 && *c > 0);
  assert_true(dt_image_repository_set_group(*a, *a));
  assert_true(dt_image_repository_set_group(*b, *a));
  assert_true(dt_image_repository_set_group(*c, *a));
  _make_unreadable(*c);
}

/* The member that cannot be read follows the new representative with the others: its row moves,
 * and no group is left named after an image that is no longer in it. */
static void test_change_representative_moves_an_unreadable_member(void **state __attribute__((unused)))
{
  int32_t a = 0;
  int32_t b = 0;
  int32_t c = 0;
  _group_of_three("/testdb/representative", &a, &b, &c);

  assert_int_equal(dt_grouping_change_representative(b), b);

  assert_true(_group_is(b, (int32_t[]){ a, b, c }, 3));
  assert_true(_group_is(a, NULL, 0));

  // and an image that cannot be read is not made a representative
  assert_int_equal(dt_grouping_change_representative(c), -1);
  assert_true(_group_is(b, (int32_t[]){ a, b, c }, 3));
}

/* The representative leaves: the group takes the name of the first member that can be read, not
 * of the first member, and the update of the rows still moves the unreadable one into it. */
static void test_remove_from_group_names_the_group_after_a_readable_member(void **state __attribute__((unused)))
{
  int32_t a = 0;
  int32_t b = 0;
  int32_t c = 0;
  _group_of_three("/testdb/leaving", &a, &b, &c);

  assert_int_equal(dt_grouping_remove_from_group(a), b);

  assert_true(_group_is(b, (int32_t[]){ b, c }, 2));
  assert_true(_group_is(a, (int32_t[]){ a }, 1));
}

/* The setters leave an image they cannot read as it is, and say nothing changed. */
static void test_setters_leave_an_unreadable_image_alone(void **state __attribute__((unused)))
{
  const int32_t film = testdb_make_film("/testdb/setters");
  assert_true(film > 0);
  const int32_t a = testdb_make_image(film, "a.raw");
  const int32_t c = testdb_make_image(film, "c.raw");
  assert_true(a > 0 && c > 0);
  assert_true(dt_image_repository_set_group(a, a));
  assert_true(dt_image_repository_set_group(c, c));
  _make_unreadable(c);

  dt_grouping_add_to_group(a, c);
  assert_true(_group_is(c, (int32_t[]){ c }, 1));

  const dt_image_geoloc_t here = { 2.35, 48.85, 35.0 };
  dt_image_set_location(c, &here, FALSE, FALSE);
  dt_image_geoloc_t got = { 0.0, 0.0, 0.0 };
  dt_image_get_location(c, &got);
  assert_true(isnan(got.longitude) && isnan(got.latitude) && isnan(got.elevation));

  GList *one = g_list_prepend(NULL, GINT_TO_POINTER(c));
  dt_image_set_datetime(one, "2026:10:28 12:00:00.000", FALSE);

  dt_image_set_monochrome_flag(c, TRUE);
  GList *flagged = dt_image_repository_get_ids_with_flag_among(one, DT_IMAGE_MONOCHROME_PREVIEW);
  assert_null(flagged);
  g_list_free(one);
}

/* Moving a file whose duplicate cannot be read: the file moves, the readable row follows it, and
 * the duplicate's row stays where it was -- with an alert, on stderr here, saying so. */
static void test_rename_leaves_an_unreadable_duplicate_behind(void **state __attribute__((unused)))
{
  gchar *src = g_build_filename(_dir, "src", NULL);
  gchar *dst = g_build_filename(_dir, "dst", NULL);
  assert_int_equal(g_mkdir(src, 0700), 0);
  assert_int_equal(g_mkdir(dst, 0700), 0);
  gchar *file = g_build_filename(src, "x.raw", NULL);
  gchar *moved = g_build_filename(dst, "x.raw", NULL);
  assert_true(g_file_set_contents(file, "raw", -1, NULL));

  const int32_t from = testdb_make_film(src);
  const int32_t to = testdb_make_film(dst);
  assert_true(from > 0 && to > 0);
  const int32_t a = testdb_make_image(from, "x.raw");
  assert_true(a > 0);
  assert_true(dt_image_repository_set_group(a, a));
  const int32_t dup = dt_image_repository_duplicate(a, 1);
  assert_true(dup > 0);
  _make_unreadable(dup);

  assert_int_equal(dt_image_move(a, to), 0);

  assert_true(g_file_test(moved, G_FILE_TEST_EXISTS));
  assert_int_equal(dt_image_repository_find_by_film_and_filename(to, "x.raw"), a);
  assert_int_equal(dt_image_repository_find_by_film_and_filename(from, "x.raw"), dup);

  dt_free(moved);
  dt_free(file);
  dt_free(dst);
  dt_free(src);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(test_change_representative_moves_an_unreadable_member, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_remove_from_group_names_the_group_after_a_readable_member, _setup,
                                    _teardown),
    cmocka_unit_test_setup_teardown(test_setters_leave_an_unreadable_image_alone, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_rename_leaves_an_unreadable_duplicate_behind, _setup, _teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
