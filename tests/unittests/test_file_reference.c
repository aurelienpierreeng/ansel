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

/** Finding again a file the user chose for an image (common/file_reference.h).
 *
 * The relative paths are strings only. Everything else runs on a tree of real files, made afresh for
 * each test in a temporary folder: an image folder, and around it the files a search by name may or
 * may not reach. Each test has its own folder, so what one search keeps for the session never answers
 * another test.
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
#include <glib/gstdio.h>
#include <string.h>

#include "common/file_reference.h"
#include "system/macros.h"    // IS_NULL_PTR
#include "system/mem_alloc.h" // dt_free

static gchar *_root = NULL; // the temporary folder of the running test

static void _remove_tree(const char *path)
{
  GDir *dir = g_dir_open(path, 0, NULL);
  const gchar *name = NULL;
  while(!IS_NULL_PTR(dir) && !IS_NULL_PTR(name = g_dir_read_name(dir)))
  {
    gchar *child = g_build_filename(path, name, NULL);
    if(g_file_test(child, G_FILE_TEST_IS_DIR))
      _remove_tree(child);
    else
      g_remove(child);
    dt_free(child);
  }
  if(!IS_NULL_PTR(dir)) g_dir_close(dir);
  g_rmdir(path);
}

static int _setup(void **state G_GNUC_UNUSED)
{
  _root = g_dir_make_tmp("ansel-file-reference-XXXXXX", NULL);
  return IS_NULL_PTR(_root);
}

static int _teardown(void **state G_GNUC_UNUSED)
{
  _remove_tree(_root);
  dt_free(_root);
  return 0;
}

/* A folder under the test's root, made with its parents. */
static gchar *_folder(const char *relative)
{
  gchar *path = g_build_filename(_root, relative, NULL);
  g_mkdir_with_parents(path, 0700);
  return path;
}

/* A file under the test's root holding @p content, made with its folders. */
static gchar *_file(const char *relative, const char *content)
{
  gchar *path = g_build_filename(_root, relative, NULL);
  gchar *dir = g_path_get_dirname(path);
  g_mkdir_with_parents(dir, 0700);
  assert_true(g_file_set_contents(path, content, -1, NULL));
  dt_free(dir);
  return path;
}

/* What the file chosen as @p chosen was, read from a file holding @p content, removed after: the file
 * is gone from where it was chosen, as on another computer. */
static void _identity_of(const char *content, uint32_t *size, uint8_t sha256[DT_FILE_REFERENCE_SHA256_SIZE])
{
  gchar *probe = _file("probe", content);
  assert_int_equal(dt_file_reference_identify(probe, size, sha256), 0);
  g_remove(probe);
  dt_free(probe);
}

static void _expect_relative(const char *folder, const char *path, const char *expected)
{
  gchar *got = dt_file_reference_relative(folder, path);
  if(IS_NULL_PTR(expected))
    assert_null(got);
  else
    assert_string_equal(got, expected);
  dt_free(got);
}

static void _relative_paths(void **state G_GNUC_UNUSED)
{
  _expect_relative("/a/b/c", "/a/b/c/f.NEF", "f.NEF");
  _expect_relative("/a/b/c", "/a/b/x/f.NEF", "../x/f.NEF");
  _expect_relative("/a/b/c", "/a/f.NEF", "../../f.NEF");
  _expect_relative("/a/b", "/a/b/s/t/f.NEF", "s/t/f.NEF");
  // a doubled or a trailing separator names no folder
  _expect_relative("/a//b/", "/a/b/f.NEF", "f.NEF");
#ifdef _WIN32
  _expect_relative("D:\\Photos\\2025\\s1", "D:\\Photos\\flats\\f.NEF", "../../flats/f.NEF");
  _expect_relative("D:/Photos/2025", "D:\\Photos\\flats\\f.NEF", "../flats/f.NEF");
  // the file systems of Windows ignore case
  _expect_relative("d:\\PHOTOS\\s1", "D:\\photos\\flats\\f.NEF", "../flats/f.NEF");
  // nothing leads from one drive or one share to another
  _expect_relative("D:\\Photos", "E:\\Photos\\f.NEF", NULL);
  _expect_relative("\\\\server\\one\\Photos", "\\\\server\\two\\f.NEF", NULL);
  _expect_relative("D:\\Photos", "\\\\server\\one\\f.NEF", NULL);
  _expect_relative("\\\\server\\one\\Photos\\s1", "\\\\server\\one\\flats\\f.NEF", "../../flats/f.NEF");
#else
  _expect_relative("/a/b", "/x/f.NEF", "../../x/f.NEF");
  _expect_relative("/a/B", "/a/b/f.NEF", "../b/f.NEF");
#endif
}

static void _names(void **state G_GNUC_UNUSED)
{
  // a path chosen on either system names its file on both
  assert_string_equal(dt_file_reference_name("D:\\Photos\\flat.NEF"), "flat.NEF");
  assert_string_equal(dt_file_reference_name("/home/me/flat.NEF"), "flat.NEF");
  assert_string_equal(dt_file_reference_name("flat.NEF"), "flat.NEF");
}

static void _identify(void **state G_GNUC_UNUSED)
{
  static const uint8_t abc[DT_FILE_REFERENCE_SHA256_SIZE]
      = { 0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
          0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad };
  gchar *path = _file("abc", "abc");
  uint32_t size = 0;
  uint8_t sha256[DT_FILE_REFERENCE_SHA256_SIZE] = { 0 };
  assert_int_equal(dt_file_reference_identify(path, &size, sha256), 0);
  assert_int_equal(size, 3);
  assert_memory_equal(sha256, abc, sizeof(abc));

  // a file that cannot be read leaves both as they were
  gchar *gone = g_build_filename(_root, "gone", NULL);
  size = 7;
  assert_int_equal(dt_file_reference_identify(gone, &size, sha256), 1);
  assert_int_equal(size, 7);
  assert_memory_equal(sha256, abc, sizeof(abc));
  dt_free(gone);
  dt_free(path);
}

static void _found_at_its_relative_path(void **state G_GNUC_UNUSED)
{
  gchar *folder = _folder("bank/s1");
  gchar *flat = _file("bank/flats/flat.NEF", "replaced in place");
  gchar *chosen = g_build_filename(_root, "elsewhere", "flat.NEF", NULL);
  uint32_t size = 0;
  uint8_t sha256[DT_FILE_REFERENCE_SHA256_SIZE];
  _identity_of("as chosen", &size, sha256);

  // what is at its relative path is taken as it is, whatever it has become
  const dt_file_reference_t ref = { chosen, "../flats/flat.NEF", size, sha256 };
  gchar *found = NULL;
  assert_int_equal(dt_file_reference_find(&ref, folder, &found, NULL), DT_FILE_REFERENCE_FOUND);
  gchar *expected = g_canonicalize_filename(flat, NULL);
  assert_string_equal(found, expected);

  dt_free(expected);
  dt_free(found);
  dt_free(chosen);
  dt_free(flat);
  dt_free(folder);
}

static void _found_at_its_absolute_path(void **state G_GNUC_UNUSED)
{
  gchar *folder = _folder("bank/s1");
  gchar *flat = _file("flats/flat.NEF", "flat");
  uint32_t size = 0;
  uint8_t sha256[DT_FILE_REFERENCE_SHA256_SIZE];
  _identity_of("flat", &size, sha256);

  // the image moved, not the file: its relative path leads nowhere now
  const dt_file_reference_t ref = { flat, "../../old/flats/flat.NEF", size, sha256 };
  gchar *found = NULL;
  assert_int_equal(dt_file_reference_find(&ref, folder, &found, NULL), DT_FILE_REFERENCE_FOUND);
  assert_string_equal(found, flat);

  dt_free(found);
  dt_free(flat);
  dt_free(folder);
}

static void _found_by_its_name_and_what_it_is(void **state G_GNUC_UNUSED)
{
  gchar *folder = _folder("bank/2025/s1");
  // nearer, of its name ignoring case and of its size, but another file: passed over
  gchar *nearer = _file("bank/2025/s1/sub/FLAT.nef", "the FLAT");
  // two folders up, in a folder directly in it: that one
  gchar *flat = _file("bank/flats/flat.NEF", "the flat");
  gchar *chosen = g_build_filename(_root, "old", "flat.NEF", NULL);
  uint32_t size = 0;
  uint8_t sha256[DT_FILE_REFERENCE_SHA256_SIZE];
  _identity_of("the flat", &size, sha256);

  const dt_file_reference_t ref = { chosen, "../../../old/flat.NEF", size, sha256 };
  gchar *found = NULL;
  GList *others = NULL;
  assert_int_equal(dt_file_reference_find(&ref, folder, &found, &others), DT_FILE_REFERENCE_FOUND);
  gchar *expected = g_canonicalize_filename(flat, NULL);
  assert_string_equal(found, expected);
  assert_null(others);

  dt_free(expected);
  dt_free(found);
  dt_free(chosen);
  dt_free(flat);
  dt_free(nearer);
  dt_free(folder);
}

static void _others_of_its_name_nearest_first(void **state G_GNUC_UNUSED)
{
  gchar *folder = _folder("bank/2025/s1");
  gchar *further = _file("bank/flats/flat.NEF", "a third flat");
  gchar *closer = _file("bank/2025/s1/flat.NEF", "a second flat");
  gchar *chosen = g_build_filename(_root, "old", "flat.NEF", NULL);
  uint32_t size = 0;
  uint8_t sha256[DT_FILE_REFERENCE_SHA256_SIZE];
  _identity_of("the flat", &size, sha256);

  const dt_file_reference_t ref = { chosen, "", size, sha256 };
  gchar *found = NULL;
  GList *others = NULL;
  assert_int_equal(dt_file_reference_find(&ref, folder, &found, &others), DT_FILE_REFERENCE_OTHERS);
  assert_null(found);
  assert_int_equal(g_list_length(others), 2);
  gchar *first = g_canonicalize_filename(closer, NULL);
  gchar *second = g_canonicalize_filename(further, NULL);
  assert_string_equal((const char *)others->data, first);
  assert_string_equal((const char *)others->next->data, second);

  // asked again, the same answer, without the caller having to want the list
  assert_int_equal(dt_file_reference_find(&ref, folder, &found, NULL), DT_FILE_REFERENCE_OTHERS);

  g_list_free_full(others, g_free);
  dt_free(second);
  dt_free(first);
  dt_free(chosen);
  dt_free(closer);
  dt_free(further);
  dt_free(folder);
}

static void _found_as_far_as_it_is_looked_for(void **state G_GNUC_UNUSED)
{
  gchar *folder = _folder("a/b/c/d/s1");
  // three folders up, in a folder directly in it
  gchar *flat = _file("a/b/x/flat.NEF", "the flat");
  gchar *chosen = g_build_filename(_root, "old", "flat.NEF", NULL);
  uint32_t size = 0;
  uint8_t sha256[DT_FILE_REFERENCE_SHA256_SIZE];
  _identity_of("the flat", &size, sha256);

  const dt_file_reference_t ref = { chosen, "", size, sha256 };
  gchar *found = NULL;
  assert_int_equal(dt_file_reference_find(&ref, folder, &found, NULL), DT_FILE_REFERENCE_FOUND);
  gchar *expected = g_canonicalize_filename(flat, NULL);
  assert_string_equal(found, expected);

  dt_free(expected);
  dt_free(found);
  dt_free(chosen);
  dt_free(flat);
  dt_free(folder);
}

static void _missing_beyond_where_it_is_looked_for(void **state G_GNUC_UNUSED)
{
  gchar *folder = _folder("a/b/c/d/s1");
  // four folders up: one too many
  gchar *too_high = _file("a/flat.NEF", "the flat");
  // three folders up, but two folders down from there: one too deep
  gchar *too_deep = _file("a/b/x/y/flat.NEF", "the flat");
  gchar *chosen = g_build_filename(_root, "old", "flat.NEF", NULL);
  uint32_t size = 0;
  uint8_t sha256[DT_FILE_REFERENCE_SHA256_SIZE];
  _identity_of("the flat", &size, sha256);

  const dt_file_reference_t ref = { chosen, "", size, sha256 };
  gchar *found = NULL;
  GList *others = NULL;
  assert_int_equal(dt_file_reference_find(&ref, folder, &found, &others), DT_FILE_REFERENCE_MISSING);
  assert_null(found);
  assert_null(others);

  dt_free(chosen);
  dt_free(too_deep);
  dt_free(too_high);
  dt_free(folder);
}

static void _found_again_once_it_moved(void **state G_GNUC_UNUSED)
{
  gchar *folder = _folder("bank/s1");
  gchar *first = _file("bank/flats/flat.NEF", "the flat");
  gchar *chosen = g_build_filename(_root, "old", "flat.NEF", NULL);
  uint32_t size = 0;
  uint8_t sha256[DT_FILE_REFERENCE_SHA256_SIZE];
  _identity_of("the flat", &size, sha256);

  const dt_file_reference_t ref = { chosen, "", size, sha256 };
  gchar *found = NULL;
  assert_int_equal(dt_file_reference_find(&ref, folder, &found, NULL), DT_FILE_REFERENCE_FOUND);
  dt_free(found);

  // what the session kept is not believed once the file is gone from there
  gchar *second = g_build_filename(_root, "bank", "s1", "flat.NEF", NULL);
  assert_int_equal(g_rename(first, second), 0);
  assert_int_equal(dt_file_reference_find(&ref, folder, &found, NULL), DT_FILE_REFERENCE_FOUND);
  gchar *expected = g_canonicalize_filename(second, NULL);
  assert_string_equal(found, expected);

  dt_free(expected);
  dt_free(found);
  dt_free(second);
  dt_free(chosen);
  dt_free(first);
  dt_free(folder);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_relative_paths),
    cmocka_unit_test(_names),
    cmocka_unit_test_setup_teardown(_identify, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_found_at_its_relative_path, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_found_at_its_absolute_path, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_found_by_its_name_and_what_it_is, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_others_of_its_name_nearest_first, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_found_as_far_as_it_is_looked_for, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_missing_beyond_where_it_is_looked_for, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_found_again_once_it_moved, _setup, _teardown),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
