/*
    This file is part of Ansel,
    Copyright (C) 2026 Paolo SANTUCCI.

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "common/utility.h"
#include "common/variables.h"
#include "common/conf.h"
#include "common/image.h"
#include "control/control.h"
#include "control/jobs/import_jobs.h"
#include "darktable.h"
#include "imageio/imageio_jpeg.h"
#include "system/macros.h"
#include "system/mem_alloc.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcntl.h>
#include <cmocka.h>
#include <glib/gstdio.h>

#ifdef _WIN32
#include <io.h>
#define test_dup _dup
#define test_dup2 _dup2
#define test_close _close
#else
#include <unistd.h>
#define test_dup dup
#define test_dup2 dup2
#define test_close close
#endif

extern int _import_copy_file(const char *filename, const char *destination, dt_control_import_t *data,
                               gchar *img_path_to_db, size_t pathname_len, GList **discarded);

typedef struct extension_case_t
{
  const char *path;
  const char *extension;
  const char *variables;
  const char *replacement;
} extension_case_t;

static const extension_case_t _extension_cases[] = {
  { "capture.raw", "raw", "capture|capture|raw", "capture.jpg" },
  { ".profile.jpg", "jpg", ".profile|.profile|jpg", ".profile.jpg" },
  { "/a.b/capture", NULL, "capture|capture|", NULL },
  { "C:\\a.b\\capture", NULL, "capture|capture|", NULL },
  { ".profile", NULL, ".profile|.profile|", NULL },
  { "capture.", NULL, "capture.|capture.|", NULL },
  { "capture", NULL, "capture|capture|", NULL },
  { "", NULL, "||", NULL },
  { "/", NULL, "||", NULL },
  { "\\", NULL, "||", NULL },
};

static char *_rcfile = NULL;
static char *_integration_config = NULL;
static char *_integration_cache = NULL;
static char *_integration_tmp = NULL;
static char *_integration_datadir = NULL;

typedef struct import_completion_t
{
  int expected;
  int completed;
  int succeeded;
  gboolean job_disposed;
  GHashTable *sources;
} import_completion_t;

static void _import_completed(const char *source, gboolean success, gpointer user_data)
{
  import_completion_t *completion = user_data;
  g_hash_table_insert(completion->sources, g_strdup(source), GINT_TO_POINTER(success));
  completion->completed++;
  completion->succeeded += success;
}

static void _import_job_disposed(gpointer user_data)
{
  import_completion_t *completion = user_data;
  completion->job_disposed = TRUE;
}

static void _completion_init(import_completion_t *completion, const int expected)
{
  completion->expected = expected;
  completion->sources = g_hash_table_new_full(g_str_hash, g_str_equal, dt_free_gpointer, NULL);
}

static void _completion_cleanup(import_completion_t *completion)
{
  g_hash_table_destroy(completion->sources);
}

static void _remove_tree(const char *path)
{
  GDir *directory = g_dir_open(path, 0, NULL);
  if(IS_NULL_PTR(directory))
  {
    g_remove(path);
    return;
  }

  const char *name = NULL;
  while((name = g_dir_read_name(directory)))
  {
    char *child = g_build_filename(path, name, NULL);
    _remove_tree(child);
    dt_free(child);
  }
  g_dir_close(directory);
  g_rmdir(path);
}

static char *_make_jpeg(const char *directory, const char *name, const uint8_t value)
{
  char *path = g_build_filename(directory, name, NULL);
  const uint8_t pixels[] = { value, value, value, 255, value, value, value, 255,
                             value, value, value, 255, value, value, value, 255 };
  assert_int_equal(dt_imageio_jpeg_write(path, pixels, 2, 2, 90, NULL, 0), 0);
  return path;
}

static void _assert_file_bytes(const char *path, const gchar *expected, const gsize expected_size)
{
  gchar *actual = NULL;
  gsize actual_size = 0;
  assert_true(g_file_get_contents(path, &actual, &actual_size, NULL));
  assert_int_equal(actual_size, expected_size);
  assert_int_equal(memcmp(actual, expected, expected_size), 0);
  dt_free(actual);
}

static void _assert_different_bytes(const gchar *first, const gsize first_size, const gchar *second,
                                    const gsize second_size)
{
  assert_true(first_size != second_size || memcmp(first, second, first_size) != 0);
}

static void _assert_import(import_completion_t *completion, const int expected_successes)
{
  assert_int_equal(completion->completed, completion->expected);
  assert_int_equal(completion->succeeded, expected_successes);
  assert_int_equal(g_hash_table_size(completion->sources), completion->expected);
  assert_true(completion->job_disposed);
}

static dt_control_import_t _copy_import(GList *sources, const int elements, const char *destination,
                                        const char *pattern, const dt_import_onconflict_t policy,
                                        import_completion_t *completion)
{
  dt_control_import_t data = { .imgs = sources, .elements = elements };
  data.datetime = g_date_time_new_now_local();
  data.copy = TRUE;
  data.on_conflict = policy;
  data.base_folder = g_strdup(destination);
  data.target_subfolder_pattern = g_strdup("");
  data.target_file_pattern = g_strdup(pattern);
  data.file_imported = _import_completed;
  data.callback_data = completion;
  data.callback_data_free = _import_job_disposed;
  return data;
}

static void _test_public_siblings_keep_sequence_for_distinct_destinations(void **state G_GNUC_UNUSED)
{
  char *root = g_dir_make_tmp("ansel-import-siblings-XXXXXX", NULL);
  char *source = g_build_filename(root, "source", NULL);
  char *destination = g_build_filename(root, "destination", NULL);
  assert_int_equal(g_mkdir(source, 0700), 0);
  char *jpg = _make_jpeg(source, "capture.jpg", 20);
  char *jpeg = _make_jpeg(source, "capture.jpeg", 21);
  gchar *jpg_bytes = NULL;
  gchar *jpeg_bytes = NULL;
  gsize jpg_size = 0;
  gsize jpeg_size = 0;
  assert_true(g_file_get_contents(jpg, &jpg_bytes, &jpg_size, NULL));
  assert_true(g_file_get_contents(jpeg, &jpeg_bytes, &jpeg_size, NULL));
  _assert_different_bytes(jpg_bytes, jpg_size, jpeg_bytes, jpeg_size);
  dt_free(jpeg_bytes);
  dt_free(jpg_bytes);

  import_completion_t completion = { 0 };
  _completion_init(&completion, 2);
  GList *sources = g_list_append(g_list_append(NULL, jpg), jpeg);
  assert_int_equal(dt_control_import(_copy_import(sources, 2, destination,
                                                  "$(FILE.NAME)_$(FILE.EXTENSION)_$(SEQUENCE).jpg",
                                                  DT_IMPORT_ONCONFLICT_SKIP, &completion)), 0);
  _assert_import(&completion, 2);
  char *first = g_build_filename(destination, "capture_jpg_0001.jpg", NULL);
  char *second = g_build_filename(destination, "capture_jpeg_0001.jpg", NULL);
  assert_true(g_file_test(first, G_FILE_TEST_IS_REGULAR));
  assert_true(g_file_test(second, G_FILE_TEST_IS_REGULAR));
  assert_true(dt_image_get_id_full_path(first) > UNKNOWN_IMAGE);
  assert_true(dt_image_get_id_full_path(second) > UNKNOWN_IMAGE);
  dt_free(first);
  dt_free(second);
  _completion_cleanup(&completion);
  _remove_tree(root);
  dt_free(destination);
  dt_free(source);
  dt_free(root);
}

static void _test_public_in_place_import_preserves_source(void **state G_GNUC_UNUSED)
{
  char *root = g_dir_make_tmp("ansel-import-in-place-XXXXXX", NULL);
  assert_non_null(root);
  char *source = _make_jpeg(root, "capture.jpg", 25);
  char *source_path = g_strdup(source);
  char *unused_destination = g_build_filename(root, "unused", NULL);
  import_completion_t completion = { 0 };
  _completion_init(&completion, 1);
  dt_control_import_t data = _copy_import(g_list_append(NULL, source), 1, unused_destination,
                                         "fixed.jpg", DT_IMPORT_ONCONFLICT_SKIP, &completion);
  data.copy = FALSE;
  data.delete_source = TRUE;
  assert_int_equal(dt_control_import(data), 0);
  _assert_import(&completion, 1);
  assert_true(g_file_test(source_path, G_FILE_TEST_IS_REGULAR));
  assert_true(dt_image_get_id_full_path(source_path) > UNKNOWN_IMAGE);
  assert_false(g_file_test(unused_destination, G_FILE_TEST_EXISTS));
  _completion_cleanup(&completion);
  _remove_tree(root);
  dt_free(unused_destination);
  dt_free(source_path);
  dt_free(root);
}

static void _test_public_delete_source_requires_successful_copy(void **state G_GNUC_UNUSED)
{
  char *root = g_dir_make_tmp("ansel-import-delete-XXXXXX", NULL);
  assert_non_null(root);
  char *destination = g_build_filename(root, "destination", NULL);
  char *source = _make_jpeg(root, "first.jpg", 35);
  char *source_path = g_strdup(source);
  gchar *bytes = NULL;
  gsize size = 0;
  assert_true(g_file_get_contents(source, &bytes, &size, NULL));
  import_completion_t completion = { 0 };
  _completion_init(&completion, 1);
  dt_control_import_t data = _copy_import(g_list_append(NULL, source), 1, destination,
                                         "fixed.jpg", DT_IMPORT_ONCONFLICT_SKIP, &completion);
  data.delete_source = TRUE;
  assert_int_equal(dt_control_import(data), 0);
  _assert_import(&completion, 1);
  assert_false(g_file_test(source_path, G_FILE_TEST_EXISTS));
  char *copied = g_build_filename(destination, "fixed.jpg", NULL);
  _assert_file_bytes(copied, bytes, size);
  _completion_cleanup(&completion);
  dt_free(source_path);

  source = _make_jpeg(root, "second.jpg", 45);
  source_path = g_strdup(source);
  completion = (import_completion_t){ 0 };
  _completion_init(&completion, 1);
  dt_control_import_t skipped = _copy_import(g_list_append(NULL, source), 1, destination,
                                            "fixed.jpg", DT_IMPORT_ONCONFLICT_SKIP, &completion);
  skipped.delete_source = TRUE;
  assert_int_equal(dt_control_import(skipped), 0);
  _assert_import(&completion, 1);
  assert_true(g_file_test(source_path, G_FILE_TEST_IS_REGULAR));
  _assert_file_bytes(copied, bytes, size);
  _completion_cleanup(&completion);
  dt_free(source_path);
  dt_free(copied);
  dt_free(bytes);
  _remove_tree(root);
  dt_free(destination);
  dt_free(root);
}

static void _test_stream_comparison_boundaries(void **state G_GNUC_UNUSED)
{
  char *directory = g_dir_make_tmp("ansel-stream-compare-XXXXXX", NULL);
  assert_non_null(directory);
  char *first_path = g_build_filename(directory, "first", NULL);
  char *second_path = g_build_filename(directory, "second", NULL);
  const size_t sizes[] = { 0, 1, 65535, 65536, 65537, 131073 };
  unsigned char *bytes = g_malloc0(sizes[G_N_ELEMENTS(sizes) - 1]);
  gboolean streams_opened = TRUE;
  size_t i = 0;
  while(i < G_N_ELEMENTS(sizes) && streams_opened)
  {
    assert_true(g_file_set_contents(first_path, (const char *)bytes, sizes[i], NULL));
    assert_true(g_file_set_contents(second_path, (const char *)bytes, sizes[i], NULL));
    FILE *first = g_fopen(first_path, "rb");
    FILE *second = g_fopen(second_path, "r+b");
    streams_opened = !IS_NULL_PTR(first) && !IS_NULL_PTR(second);
    if(!streams_opened)
    {
      if(!IS_NULL_PTR(first)) fclose(first);
      if(!IS_NULL_PTR(second)) fclose(second);
      goto cleanup;
    }
    const int equal = dt_util_streams_equal(first, second);
    int mismatch = 0;
    if(sizes[i] > 0)
    {
      assert_int_equal(fseek(second, -1, SEEK_END), 0);
      assert_int_equal(fputc(1, second), 1);
      rewind(first);
      rewind(second);
      mismatch = dt_util_streams_equal(first, second);
      assert_int_equal(fseek(second, -1, SEEK_END), 0);
      assert_int_equal(fputc(0, second), 0);
    }
    assert_int_equal(fseek(second, 0, SEEK_END), 0);
    assert_int_equal(fputc(2, second), 2);
    rewind(first);
    rewind(second);
    const int longer = dt_util_streams_equal(first, second);
    fclose(second);
    second = g_fopen(second_path, "wb");
    streams_opened = !IS_NULL_PTR(second);
    int read_error = 0;
    if(streams_opened)
    {
      rewind(first);
      read_error = dt_util_streams_equal(first, second);
      fclose(second);
    }
    fclose(first);
    assert_int_equal(equal, 1);
    assert_int_equal(mismatch, 0);
    assert_int_equal(longer, 0);
    if(streams_opened) assert_int_equal(read_error, -1);
    i++;
  }
cleanup:
  dt_free(bytes)
  g_remove(first_path);
  g_remove(second_path);
  g_rmdir(directory);
  dt_free(first_path);
  dt_free(second_path);
  dt_free(directory);
  assert_true(streams_opened);
}

static void _test_public_sibling_collision_reexpands_once(void **state G_GNUC_UNUSED)
{
  char *root = g_dir_make_tmp("ansel-import-fallback-XXXXXX", NULL);
  char *source = g_build_filename(root, "source", NULL);
  char *destination = g_build_filename(root, "destination", NULL);
  assert_int_equal(g_mkdir(source, 0700), 0);
  char *jpg = _make_jpeg(source, "capture.jpg", 30);
  char *jpeg = _make_jpeg(source, "capture.jpeg", 31);
  gchar *first_bytes = NULL;
  gchar *fallback_bytes = NULL;
  gsize first_size = 0;
  gsize fallback_size = 0;
  assert_true(g_file_get_contents(jpg, &first_bytes, &first_size, NULL));
  assert_true(g_file_get_contents(jpeg, &fallback_bytes, &fallback_size, NULL));
  _assert_different_bytes(first_bytes, first_size, fallback_bytes, fallback_size);

  import_completion_t completion = { 0 };
  _completion_init(&completion, 2);
  GList *sources = g_list_append(g_list_append(NULL, jpg), jpeg);
  assert_int_equal(dt_control_import(_copy_import(sources, 2, destination, "$(FILE.NAME)_$(SEQUENCE).jpg",
                                                  DT_IMPORT_ONCONFLICT_SKIP, &completion)), 0);
  _assert_import(&completion, 2);
  char *first = g_build_filename(destination, "capture_0001.jpg", NULL);
  char *fallback = g_build_filename(destination, "capture_0002.jpg", NULL);
  assert_true(g_file_test(first, G_FILE_TEST_IS_REGULAR));
  assert_true(g_file_test(fallback, G_FILE_TEST_IS_REGULAR));
  assert_true(dt_image_get_id_full_path(first) > UNKNOWN_IMAGE);
  assert_true(dt_image_get_id_full_path(fallback) > UNKNOWN_IMAGE);
  _assert_file_bytes(first, first_bytes, first_size);
  _assert_file_bytes(fallback, fallback_bytes, fallback_size);
  dt_free(fallback_bytes);
  dt_free(first_bytes);
  dt_free(first);
  dt_free(fallback);
  _completion_cleanup(&completion);
  _remove_tree(root);
  dt_free(destination);
  dt_free(source);
  dt_free(root);
}

static void _test_public_fixed_destination_follows_all_policies(void **state G_GNUC_UNUSED)
{
  const dt_import_onconflict_t policies[] = { DT_IMPORT_ONCONFLICT_SKIP, DT_IMPORT_ONCONFLICT_OVERWRITE,
                                               DT_IMPORT_ONCONFLICT_UNIQUE };
  for(size_t policy = 0; policy < G_N_ELEMENTS(policies); policy++)
  {
    char *root = g_dir_make_tmp("ansel-import-fixed-XXXXXX", NULL);
    char *source = g_build_filename(root, "source", NULL);
    char *destination = g_build_filename(root, "destination", NULL);
    assert_int_equal(g_mkdir(source, 0700), 0);
    char *jpg = _make_jpeg(source, "capture.jpg", 40);
    char *jpeg = _make_jpeg(source, "capture.jpeg", 41);
    gchar *first_bytes = NULL;
    gchar *second_bytes = NULL;
    gsize first_size = 0;
    gsize second_size = 0;
    assert_true(g_file_get_contents(jpg, &first_bytes, &first_size, NULL));
    assert_true(g_file_get_contents(jpeg, &second_bytes, &second_size, NULL));
    _assert_different_bytes(first_bytes, first_size, second_bytes, second_size);
    import_completion_t completion = { 0 };
    _completion_init(&completion, 2);
    GList *sources = g_list_append(g_list_append(NULL, jpg), jpeg);
    assert_int_equal(dt_control_import(_copy_import(sources, 2, destination, "fixed.jpg", policies[policy],
                                                    &completion)), 0);
    _assert_import(&completion, 2);
    char *fixed = g_build_filename(destination, "fixed.jpg", NULL);
    char *unique = g_build_filename(destination, "fixed_01.jpg", NULL);
    assert_true(g_file_test(fixed, G_FILE_TEST_IS_REGULAR));
    assert_true(dt_image_get_id_full_path(fixed) > UNKNOWN_IMAGE);
    _assert_file_bytes(fixed, policies[policy] == DT_IMPORT_ONCONFLICT_OVERWRITE ? second_bytes : first_bytes,
                       policies[policy] == DT_IMPORT_ONCONFLICT_OVERWRITE ? second_size : first_size);
    assert_int_equal(g_file_test(unique, G_FILE_TEST_IS_REGULAR), policies[policy] == DT_IMPORT_ONCONFLICT_UNIQUE);
    if(policies[policy] == DT_IMPORT_ONCONFLICT_UNIQUE)
    {
      assert_true(dt_image_get_id_full_path(unique) > UNKNOWN_IMAGE);
      _assert_file_bytes(unique, second_bytes, second_size);
    }
    dt_free(second_bytes);
    dt_free(first_bytes);
    dt_free(unique);
    dt_free(fixed);
    _completion_cleanup(&completion);
    _remove_tree(root);
    dt_free(destination);
    dt_free(source);
    dt_free(root);
  }
}

static void _test_public_unrelated_collision_has_no_sibling_fallback(void **state G_GNUC_UNUSED)
{
  const dt_import_onconflict_t policies[] = { DT_IMPORT_ONCONFLICT_SKIP, DT_IMPORT_ONCONFLICT_OVERWRITE,
                                               DT_IMPORT_ONCONFLICT_UNIQUE };
  for(size_t policy = 0; policy < G_N_ELEMENTS(policies); policy++)
  {
    char *root = g_dir_make_tmp("ansel-import-unrelated-XXXXXX", NULL);
    char *source = g_build_filename(root, "source", NULL);
    char *destination = g_build_filename(root, "destination", NULL);
    assert_int_equal(g_mkdir(source, 0700), 0);
    char *first_source = _make_jpeg(source, "first.jpg", 50);
    char *second_source = _make_jpeg(source, "second.jpg", 60);
    gchar *first_bytes = NULL;
    gchar *second_bytes = NULL;
    gsize first_size = 0;
    gsize second_size = 0;
    assert_true(g_file_get_contents(first_source, &first_bytes, &first_size, NULL));
    assert_true(g_file_get_contents(second_source, &second_bytes, &second_size, NULL));
    _assert_different_bytes(first_bytes, first_size, second_bytes, second_size);
    import_completion_t completion = { 0 };
    _completion_init(&completion, 2);
    GList *sources = g_list_append(g_list_append(NULL, first_source), second_source);
    assert_int_equal(dt_control_import(_copy_import(sources, 2, destination, "fixed.jpg", policies[policy],
                                                    &completion)), 0);
    _assert_import(&completion, 2);
    char *fixed = g_build_filename(destination, "fixed.jpg", NULL);
    char *fallback = g_build_filename(destination, "fixed_2.jpg", NULL);
    char *unique = g_build_filename(destination, "fixed_01.jpg", NULL);
    assert_true(g_file_test(fixed, G_FILE_TEST_IS_REGULAR));
    assert_false(g_file_test(fallback, G_FILE_TEST_EXISTS));
    _assert_file_bytes(fixed, policies[policy] == DT_IMPORT_ONCONFLICT_OVERWRITE ? second_bytes : first_bytes,
                       policies[policy] == DT_IMPORT_ONCONFLICT_OVERWRITE ? second_size : first_size);
    if(policies[policy] == DT_IMPORT_ONCONFLICT_UNIQUE)
    {
      assert_true(g_file_test(unique, G_FILE_TEST_IS_REGULAR));
      _assert_file_bytes(unique, second_bytes, second_size);
    }
    dt_free(second_bytes);
    dt_free(first_bytes);
    dt_free(unique);
    dt_free(fallback);
    dt_free(fixed);
    _completion_cleanup(&completion);
    _remove_tree(root);
    dt_free(destination);
    dt_free(source);
    dt_free(root);
  }
}

static void _test_public_failures_consume_sequences_and_complete_once(void **state G_GNUC_UNUSED)
{
  char *root = g_dir_make_tmp("ansel-import-failures-XXXXXX", NULL);
  char *source = g_build_filename(root, "source", NULL);
  char *destination = g_build_filename(root, "destination", NULL);
  assert_int_equal(g_mkdir(source, 0700), 0);
  char *missing = g_build_filename(source, "missing.jpg", NULL);
  char *invalid = g_build_filename(source, "invalid.dt", NULL);
  char *valid = _make_jpeg(source, "valid.jpg", 70);
  char *missing_key = g_strdup(missing);
  char *invalid_key = g_strdup(invalid);
  char *valid_key = g_strdup(valid);
  assert_true(g_file_set_contents(invalid, "not a jpeg", -1, NULL));
  import_completion_t completion = { 0 };
  _completion_init(&completion, 3);
  GList *sources = g_list_append(g_list_append(g_list_append(NULL, missing), invalid), valid);
  assert_int_equal(dt_control_import(_copy_import(sources, 3, destination, "$(FILE.NAME)_$(SEQUENCE).$(FILE.EXTENSION)",
                                                  DT_IMPORT_ONCONFLICT_SKIP, &completion)), 0);
  _assert_import(&completion, 1);
  assert_false(GPOINTER_TO_INT(g_hash_table_lookup(completion.sources, missing_key)));
  assert_false(GPOINTER_TO_INT(g_hash_table_lookup(completion.sources, invalid_key)));
  assert_true(GPOINTER_TO_INT(g_hash_table_lookup(completion.sources, valid_key)));
  char *missing_destination = g_build_filename(destination, "missing_0001.jpg", NULL);
  char *invalid_destination = g_build_filename(destination, "invalid_0002.dt", NULL);
  char *valid_destination = g_build_filename(destination, "valid_0003.jpg", NULL);
  assert_false(g_file_test(missing_destination, G_FILE_TEST_EXISTS));
  assert_true(g_file_test(invalid_destination, G_FILE_TEST_IS_REGULAR));
  assert_true(g_file_test(valid_destination, G_FILE_TEST_IS_REGULAR));
  assert_int_equal(dt_image_get_id_full_path(invalid_destination), UNKNOWN_IMAGE);
  assert_true(dt_image_get_id_full_path(valid_destination) > UNKNOWN_IMAGE);
  dt_free(missing_destination);
  dt_free(invalid_destination);
  dt_free(valid_destination);
  dt_free(valid_key);
  dt_free(invalid_key);
  dt_free(missing_key);
  _completion_cleanup(&completion);
  _remove_tree(root);
  dt_free(destination);
  dt_free(source);
  dt_free(root);
}

static void _test_public_copy_failure_reservation_forces_sibling_fallback(void **state G_GNUC_UNUSED)
{
  char *root = g_dir_make_tmp("ansel-import-copy-reservation-XXXXXX", NULL);
  char *source = g_build_filename(root, "source", NULL);
  char *destination = g_build_filename(root, "destination", NULL);
  assert_int_equal(g_mkdir(source, 0700), 0);
  char *missing = g_build_filename(source, "capture.jpg", NULL);
  char *sibling = _make_jpeg(source, "capture.jpeg", 80);
  char *missing_key = g_strdup(missing);
  char *sibling_key = g_strdup(sibling);
  import_completion_t completion = { 0 };
  _completion_init(&completion, 2);
  GList *sources = g_list_append(g_list_append(NULL, missing), sibling);
  assert_int_equal(dt_control_import(_copy_import(sources, 2, destination, "$(FILE.NAME)_$(SEQUENCE).jpg",
                                                  DT_IMPORT_ONCONFLICT_SKIP, &completion)), 0);
  _assert_import(&completion, 1);
  assert_false(GPOINTER_TO_INT(g_hash_table_lookup(completion.sources, missing_key)));
  assert_true(GPOINTER_TO_INT(g_hash_table_lookup(completion.sources, sibling_key)));
  char *failed = g_build_filename(destination, "capture_0001.jpg", NULL);
  char *fallback = g_build_filename(destination, "capture_0002.jpg", NULL);
  assert_false(g_file_test(failed, G_FILE_TEST_EXISTS));
  assert_true(g_file_test(fallback, G_FILE_TEST_IS_REGULAR));
  assert_true(dt_image_get_id_full_path(fallback) > UNKNOWN_IMAGE);
  dt_free(fallback);
  dt_free(failed);
  dt_free(sibling_key);
  dt_free(missing_key);
  _completion_cleanup(&completion);
  _remove_tree(root);
  dt_free(destination);
  dt_free(source);
  dt_free(root);
}

static void _test_public_database_failure_reservation_forces_sibling_fallback(void **state G_GNUC_UNUSED)
{
  char *root = g_dir_make_tmp("ansel-import-db-reservation-XXXXXX", NULL);
  char *source = g_build_filename(root, "source", NULL);
  char *destination = g_build_filename(root, "destination", NULL);
  assert_int_equal(g_mkdir(source, 0700), 0);
  char *first = _make_jpeg(source, "capture.jpg", 90);
  char *sibling = _make_jpeg(source, "capture.jpeg", 91);
  gchar *first_bytes = NULL;
  gchar *sibling_bytes = NULL;
  gsize first_size = 0;
  gsize sibling_size = 0;
  assert_true(g_file_get_contents(first, &first_bytes, &first_size, NULL));
  assert_true(g_file_get_contents(sibling, &sibling_bytes, &sibling_size, NULL));
  _assert_different_bytes(first_bytes, first_size, sibling_bytes, sibling_size);
  dt_free(sibling_bytes);
  dt_free(first_bytes);
  char *first_key = g_strdup(first);
  char *sibling_key = g_strdup(sibling);
  import_completion_t completion = { 0 };
  _completion_init(&completion, 2);
  GList *sources = g_list_append(g_list_append(NULL, first), sibling);
  assert_int_equal(dt_control_import(_copy_import(sources, 2, destination, "$(FILE.NAME)_$(SEQUENCE).dt",
                                                  DT_IMPORT_ONCONFLICT_SKIP, &completion)), 0);
  _assert_import(&completion, 0);
  assert_false(GPOINTER_TO_INT(g_hash_table_lookup(completion.sources, first_key)));
  assert_false(GPOINTER_TO_INT(g_hash_table_lookup(completion.sources, sibling_key)));
  char *first_destination = g_build_filename(destination, "capture_0001.dt", NULL);
  char *fallback = g_build_filename(destination, "capture_0002.dt", NULL);
  assert_true(g_file_test(first_destination, G_FILE_TEST_IS_REGULAR));
  assert_true(g_file_test(fallback, G_FILE_TEST_IS_REGULAR));
  assert_int_equal(dt_image_get_id_full_path(first_destination), UNKNOWN_IMAGE);
  assert_int_equal(dt_image_get_id_full_path(fallback), UNKNOWN_IMAGE);
  dt_free(fallback);
  dt_free(first_destination);
  dt_free(sibling_key);
  dt_free(first_key);
  _completion_cleanup(&completion);
  _remove_tree(root);
  dt_free(destination);
  dt_free(source);
  dt_free(root);
}

static void _test_public_reserved_fallback_reaches_policy_without_third_expansion(void **state G_GNUC_UNUSED)
{
  const dt_import_onconflict_t policies[] = { DT_IMPORT_ONCONFLICT_SKIP, DT_IMPORT_ONCONFLICT_OVERWRITE,
                                               DT_IMPORT_ONCONFLICT_UNIQUE };
  for(size_t policy = 0; policy < G_N_ELEMENTS(policies); policy++)
  {
    char *root = g_dir_make_tmp("ansel-import-reserved-fallback-XXXXXX", NULL);
    char *source = g_build_filename(root, "source", NULL);
    char *destination = g_build_filename(root, "destination", NULL);
    assert_int_equal(g_mkdir(source, 0700), 0);
    char *capture = _make_jpeg(source, "capture.jpg", 100);
    char *fallback_owner = _make_jpeg(source, "capture1.jpg", 101);
    char *capture_key = g_strdup(capture);
    char *owner_key = g_strdup(fallback_owner);
    gchar *capture_bytes = NULL;
    gchar *owner_bytes = NULL;
    gchar *sibling_bytes = NULL;
    gsize capture_size = 0;
    gsize owner_size = 0;
    gsize sibling_size = 0;
    assert_true(g_file_get_contents(capture, &capture_bytes, &capture_size, NULL));
    assert_true(g_file_get_contents(fallback_owner, &owner_bytes, &owner_size, NULL));
    GList *sources = g_list_append(g_list_append(NULL, capture), fallback_owner);
    for(int sequence = 3; sequence < 12; sequence++)
    {
      char *name = g_strdup_printf("other%d.jpg", sequence);
      sources = g_list_append(sources, _make_jpeg(source, name, (uint8_t)(100 + sequence)));
      dt_free(name);
    }
    char *sibling = _make_jpeg(source, "capture.jpeg", 120);
    char *sibling_key = g_strdup(sibling);
    assert_true(g_file_get_contents(sibling, &sibling_bytes, &sibling_size, NULL));
    _assert_different_bytes(capture_bytes, capture_size, owner_bytes, owner_size);
    _assert_different_bytes(owner_bytes, owner_size, sibling_bytes, sibling_size);
    sources = g_list_append(sources, sibling);
    import_completion_t completion = { 0 };
    _completion_init(&completion, 12);
    assert_int_equal(dt_control_import(_copy_import(sources, 12, destination, "$(FILE.NAME)$(SEQUENCE1).jpg",
                                                    policies[policy], &completion)), 0);
    _assert_import(&completion, 12);
    assert_true(GPOINTER_TO_INT(g_hash_table_lookup(completion.sources, capture_key)));
    assert_true(GPOINTER_TO_INT(g_hash_table_lookup(completion.sources, owner_key)));
    assert_true(GPOINTER_TO_INT(g_hash_table_lookup(completion.sources, sibling_key)));
    char *first_destination = g_build_filename(destination, "capture1.jpg", NULL);
    char *reserved_fallback = g_build_filename(destination, "capture12.jpg", NULL);
    char *third_retry = g_build_filename(destination, "capture13.jpg", NULL);
    char *unique = g_build_filename(destination, "capture12_01.jpg", NULL);
    assert_true(g_file_test(first_destination, G_FILE_TEST_IS_REGULAR));
    assert_true(g_file_test(reserved_fallback, G_FILE_TEST_IS_REGULAR));
    assert_false(g_file_test(third_retry, G_FILE_TEST_EXISTS));
    _assert_file_bytes(first_destination, capture_bytes, capture_size);
    _assert_file_bytes(reserved_fallback, policies[policy] == DT_IMPORT_ONCONFLICT_OVERWRITE ? sibling_bytes : owner_bytes,
                       policies[policy] == DT_IMPORT_ONCONFLICT_OVERWRITE ? sibling_size : owner_size);
    assert_int_equal(g_file_test(unique, G_FILE_TEST_IS_REGULAR), policies[policy] == DT_IMPORT_ONCONFLICT_UNIQUE);
    if(policies[policy] == DT_IMPORT_ONCONFLICT_UNIQUE)
      _assert_file_bytes(unique, sibling_bytes, sibling_size);
    dt_free(unique);
    dt_free(third_retry);
    dt_free(reserved_fallback);
    dt_free(first_destination);
    dt_free(sibling_key);
    dt_free(sibling_bytes);
    dt_free(owner_bytes);
    dt_free(capture_bytes);
    dt_free(owner_key);
    dt_free(capture_key);
    _completion_cleanup(&completion);
    _remove_tree(root);
    dt_free(destination);
    dt_free(source);
    dt_free(root);
  }
}

static char *_prepare_test_datadir(const char *tmp_dir)
{
  char *datadir = g_build_filename(tmp_dir, "data", NULL);
  char *rawspeed_dir = g_build_filename(datadir, "rawspeed", NULL);
  char *rawspeed_xml = g_build_filename(rawspeed_dir, "cameras.xml", NULL);
  char *source_xml = g_build_filename(ANSEL_TEST_SOURCE_DIR, "src", "external", "rawspeed", "data", "cameras.xml", NULL);
  assert_int_equal(g_mkdir(datadir, 0700), 0);
  assert_int_equal(g_mkdir(rawspeed_dir, 0700), 0);
  GFile *source_file = g_file_new_for_path(source_xml);
  GFile *destination_file = g_file_new_for_path(rawspeed_xml);
  assert_true(g_file_copy(source_file, destination_file, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, NULL));
  g_object_unref(destination_file);
  g_object_unref(source_file);
  dt_free(source_xml);
  dt_free(rawspeed_xml);
  dt_free(rawspeed_dir);
  return datadir;
}

static int _setup(void **state G_GNUC_UNUSED)
{
  _rcfile = g_build_filename(g_get_tmp_dir(), "ansel-test-import-jobs.rc", NULL);
  g_remove(_rcfile);
  darktable.conf = calloc(1, sizeof(dt_conf_t));
  dt_conf_init(darktable.conf, _rcfile, NULL);
  return 0;
}

static int _teardown(void **state G_GNUC_UNUSED)
{
  dt_conf_cleanup(darktable.conf);
  free(darktable.conf);
  darktable.conf = NULL;
  g_remove(_rcfile);
  dt_free(_rcfile);
  _rcfile = NULL;
  return 0;
}

static void _test_extension_contract(void **state G_GNUC_UNUSED)
{
  dt_variables_params_t *params = NULL;
  dt_variables_params_init(&params);

  for(size_t i = 0; i < G_N_ELEMENTS(_extension_cases); i++)
  {
    const extension_case_t *test_case = &_extension_cases[i];
    char *path = g_strdup(test_case->path);
    const char *extension = dt_util_path_get_extension(path);

    if(test_case->extension)
      assert_ptr_equal(extension, path + strlen(path) - strlen(test_case->extension));
    else
      assert_null(extension);
    assert_string_equal(path, test_case->path);

    params->filename = path;
    char source[] = "$(FILE.NAME)|$(IMAGE.BASENAME)|$(FILE.EXTENSION)";
    char *variables = dt_variables_expand(params, source, FALSE);
    assert_string_equal(variables, test_case->variables);
    assert_string_equal(path, test_case->path);
    dt_free(variables);

    if(test_case->replacement)
    {
      char *replacement = dt_copy_filename_extension(path, "capture.jpg");
      assert_string_equal(replacement, test_case->replacement);
      assert_true(dt_has_same_path_basename(path, replacement));
      assert_string_equal(path, test_case->path);
      dt_free(replacement);
    }
    else
    {
      assert_null(dt_copy_filename_extension(path, "capture.jpg"));
      assert_false(dt_has_same_path_basename(path, "capture.jpg"));
    }

    dt_free(path);
  }

  dt_variables_params_destroy(params);
}

static void _test_extension_utility_rejections(void **state G_GNUC_UNUSED)
{
  assert_false(dt_has_same_path_basename("capture.raw", "other.jpg"));
  assert_false(dt_has_same_path_basename("/a.b/capture.raw", "/a/capture.jpg"));
  assert_false(dt_has_same_path_basename(NULL, "capture.jpg"));
  assert_false(dt_has_same_path_basename("capture.raw", NULL));

  assert_null(dt_copy_filename_extension(NULL, "capture.jpg"));
  assert_null(dt_copy_filename_extension("capture.raw", NULL));
  assert_null(dt_copy_filename_extension("capture.raw", ".profile"));
  assert_null(dt_copy_filename_extension("capture.raw", "capture."));
}

static void _test_pattern_expansion_rejects_missing_patterns(void **state G_GNUC_UNUSED)
{
  dt_control_import_t data = { 0 };
  dt_image_t image;
  char *directory = g_dir_make_tmp("ansel-test-pattern-expansion-XXXXXX", NULL);
  assert_non_null(directory);
  data.base_folder = directory;
  data.datetime = g_date_time_new_now_local();
  dt_image_init(&image);

  assert_null(dt_build_filename_from_pattern("source.raw", 1, &image, &data));
  assert_null(data.target_dir);

  data.target_file_pattern = "target.raw";
  data.target_subfolder_pattern = "";
  data.base_folder = NULL;
  assert_null(dt_build_filename_from_pattern("source.raw", 1, &image, &data));
  assert_null(data.target_dir);

  g_date_time_unref(data.datetime);
  _remove_tree(directory);
  dt_free(directory);
}

static void _test_image_import_extension_boundaries(void **state G_GNUC_UNUSED)
{
  char *directory = g_dir_make_tmp("ansel-test-image-import-XXXXXX", NULL);
  assert_non_null(directory);
  char *dotted_directory = g_build_filename(directory, "a.b", NULL);
  assert_int_equal(g_mkdir(dotted_directory, 0700), 0);

  char *extensionless = g_build_filename(dotted_directory, "capture", NULL);
  char *dotfile = g_build_filename(directory, ".profile", NULL);
  char *trailing_dot = g_build_filename(directory, "capture.", NULL);
  assert_true(g_file_set_contents(extensionless, "x", 1, NULL));
  assert_true(g_file_set_contents(dotfile, "x", 1, NULL));
  assert_true(g_file_set_contents(trailing_dot, "x", 1, NULL));

  assert_int_equal(dt_image_import(0, extensionless, FALSE), 0);
  assert_int_equal(dt_image_import(0, dotfile, FALSE), 0);
  assert_int_equal(dt_image_import(0, trailing_dot, FALSE), 0);

  assert_true(dt_image_flags_from_extension(".cr2") & DT_IMAGE_RAW);
  assert_true(dt_image_flags_from_extension(".jpg") & DT_IMAGE_LDR);

  g_remove(extensionless);
  g_remove(dotfile);
  g_remove(trailing_dot);
  g_rmdir(dotted_directory);
  g_rmdir(directory);
  dt_free(extensionless);
  dt_free(dotfile);
  dt_free(trailing_dot);
  dt_free(dotted_directory);
  dt_free(directory);
}

typedef struct directory_failure_case_t
{
  const char *target_name;
  const char *expected_diagnostic;
  const char *unexpected_diagnostic;
  gboolean target_is_file;
} directory_failure_case_t;

static const directory_failure_case_t _directory_failure_cases[] = {
  { "blocker", "Unable to create the target folder", "Not allowed to write", TRUE },
  { "read-only", "Not allowed to write", "Unable to create the target folder", FALSE },
};

typedef struct directory_failure_fixture_t
{
  const directory_failure_case_t *test_case;
  char *directory;
  char *target;
  char *destination;
  char *diagnostics;
  char *output;
  int diagnostic_fd;
  int stdout_fd;
  int stderr_fd;
  gboolean target_created;
  gboolean mutex_initialized;
  gboolean writable_after_chmod;
  dt_control_t control;
  dt_control_t *saved_control;
  dt_control_import_t data;
  GList *discarded;
} directory_failure_fixture_t;

/** @brief Release fixture resources after setup failure, assertion failure, or skip. */
static int _directory_failure_teardown(void **state)
{
  directory_failure_fixture_t *fixture = *state;
  int result = 0;
  if(fixture->stdout_fd >= 0) test_close(fixture->stdout_fd);
  if(fixture->stderr_fd >= 0) test_close(fixture->stderr_fd);
  if(fixture->diagnostic_fd >= 0) test_close(fixture->diagnostic_fd);
  g_list_free_full(fixture->discarded, dt_free_gpointer);
  dt_free(fixture->output);
  dt_free(fixture->destination);
  if(!IS_NULL_PTR(fixture->diagnostics)) g_remove(fixture->diagnostics);
  dt_free(fixture->diagnostics);
  dt_free(fixture->data.target_dir);
  if(fixture->mutex_initialized) dt_pthread_mutex_destroy(&fixture->control.log_mutex);
  darktable.control = fixture->saved_control;
  if(fixture->target_created)
  {
    if(fixture->test_case->target_is_file)
      result = g_remove(fixture->target);
    else
    {
      result = g_chmod(fixture->target, 0700);
      if(g_rmdir(fixture->target) != 0) result = -1;
    }
  }
  if(!IS_NULL_PTR(fixture->directory) && g_rmdir(fixture->directory) != 0) result = -1;
  dt_free(fixture->target);
  dt_free(fixture->directory);
  return result;
}

/** @brief Prepare both diagnostic cases without redirecting process-wide output. */
static int _directory_failure_setup(void **state)
{
  directory_failure_fixture_t *fixture = *state;
  const directory_failure_case_t *test_case = fixture->test_case;
  fixture->stdout_fd = -1;
  fixture->stderr_fd = -1;
  fixture->diagnostic_fd = -1;
  fixture->saved_control = darktable.control;
  fixture->directory = g_dir_make_tmp("ansel-test-import-copy-XXXXXX", NULL);
  if(IS_NULL_PTR(fixture->directory)) goto failed;
  fixture->target = g_build_filename(fixture->directory, test_case->target_name, NULL);
  if(test_case->target_is_file)
    fixture->target_created = g_file_set_contents(fixture->target, "x", 1, NULL);
  else
    fixture->target_created = g_mkdir(fixture->target, 0700) == 0;
  if(!fixture->target_created) goto failed;
  if(!test_case->target_is_file)
  {
    if(g_chmod(fixture->target, 0500) != 0) goto failed;
    fixture->writable_after_chmod = dt_util_test_writable_dir(fixture->target);
  }
  if(dt_pthread_mutex_init(&fixture->control.log_mutex, NULL) != 0) goto failed;
  fixture->mutex_initialized = TRUE;
  darktable.control = &fixture->control;
  fixture->data.target_dir = test_case->target_is_file
                              ? g_build_filename(fixture->target, "missing", NULL) : g_strdup(fixture->target);
  fixture->destination = g_build_filename(fixture->data.target_dir, "target.raw", NULL);
  fixture->diagnostics = g_build_filename(fixture->directory, "diagnostics", NULL);
  fixture->diagnostic_fd = g_open(fixture->diagnostics, O_CREAT | O_EXCL | O_RDWR, 0600);
  if(fixture->diagnostic_fd < 0) goto failed;
  fixture->stdout_fd = test_dup(fileno(stdout));
  if(fixture->stdout_fd < 0) goto failed;
  fixture->stderr_fd = test_dup(fileno(stderr));
  if(fixture->stderr_fd < 0) goto failed;
  return 0;

failed:
  _directory_failure_teardown(state);
  return -1;
}

static void _test_copy_directory_failure_reports_only_its_cause(void **state)
{
  directory_failure_fixture_t *fixture = *state;
  const directory_failure_case_t *test_case = fixture->test_case;
  if(fixture->writable_after_chmod) skip();
  gchar image_path[DT_PATH_MAX] = { 0 };
  int import_result = 0;
  int stderr_redirect_result = -1;
  fflush(stdout);
  fflush(stderr);
  const int stdout_redirect_result = test_dup2(fixture->diagnostic_fd, fileno(stdout));
  if(stdout_redirect_result != fileno(stdout)) goto restore;
  stderr_redirect_result = test_dup2(fixture->diagnostic_fd, fileno(stderr));
  if(stderr_redirect_result != fileno(stderr)) goto restore;
  import_result = _import_copy_file("source.raw", fixture->destination, &fixture->data,
                                    image_path, sizeof(image_path), &fixture->discarded);

restore:
  fflush(stdout);
  fflush(stderr);
  const int stdout_restore_result = test_dup2(fixture->stdout_fd, fileno(stdout));
  const int stderr_restore_result = test_dup2(fixture->stderr_fd, fileno(stderr));
  assert_int_equal(stdout_restore_result, fileno(stdout));
  assert_int_equal(stderr_restore_result, fileno(stderr));
  assert_int_equal(stdout_redirect_result, fileno(stdout));
  assert_int_equal(stderr_redirect_result, fileno(stderr));
  assert_int_equal(import_result, -1);
  assert_true(g_file_get_contents(fixture->diagnostics, &fixture->output, NULL, NULL));
  assert_non_null(strstr(fixture->output, test_case->expected_diagnostic));
  assert_null(strstr(fixture->output, test_case->unexpected_diagnostic));
  assert_null(strstr(fixture->output, "Unable to copy the file"));
  assert_string_equal(image_path, "");
  assert_null(fixture->discarded);
  if(test_case->target_is_file)
  {
    assert_int_equal(fixture->control.log_pos, 1);
    assert_non_null(strstr(fixture->control.log_message[0], "Impossible to create directory"));
  }
}

int main(void)
{
  directory_failure_fixture_t directory_fixtures[] = {
    { .test_case = &_directory_failure_cases[0] },
    { .test_case = &_directory_failure_cases[1] },
  };
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_test_extension_contract),
    cmocka_unit_test(_test_extension_utility_rejections),
    cmocka_unit_test(_test_pattern_expansion_rejects_missing_patterns),
    cmocka_unit_test(_test_image_import_extension_boundaries),
    cmocka_unit_test(_test_stream_comparison_boundaries),
    cmocka_unit_test_prestate_setup_teardown(_test_copy_directory_failure_reports_only_its_cause,
      _directory_failure_setup, _directory_failure_teardown, &directory_fixtures[0]),
    cmocka_unit_test_prestate_setup_teardown(_test_copy_directory_failure_reports_only_its_cause,
      _directory_failure_setup, _directory_failure_teardown, &directory_fixtures[1]),
  };
  const int unit_result = cmocka_run_group_tests(tests, _setup, _teardown);
  if(unit_result) return unit_result;

  _integration_config = g_strdup_printf("%s/ansel-test-import-config-XXXXXX", g_get_tmp_dir());
  _integration_cache = g_strdup_printf("%s/ansel-test-import-cache-XXXXXX", g_get_tmp_dir());
  _integration_tmp = g_strdup_printf("%s/ansel-test-import-tmp-XXXXXX", g_get_tmp_dir());
  assert_non_null(g_mkdtemp(_integration_config));
  assert_non_null(g_mkdtemp(_integration_cache));
  assert_non_null(g_mkdtemp(_integration_tmp));
  _integration_datadir = _prepare_test_datadir(_integration_tmp);
  char *noiseprofiles = g_build_filename(ANSEL_TEST_SOURCE_DIR, "data", "noiseprofiles.json", NULL);
  char *argv[] = { "ansel-test-import-jobs", "--library", ":memory:", "--datadir", _integration_datadir,
                   "--noiseprofiles", noiseprofiles, "--moduledir", ANSEL_TEST_BINARY_DIR "/src",
                   "--configdir", _integration_config, "--cachedir", _integration_cache, "--tmpdir", _integration_tmp,
                   "--disable-opencl", "--conf", "write_sidecar_files=FALSE", "-t", "1", NULL };
  assert_int_equal(dt_init(G_N_ELEMENTS(argv) - 1, argv, FALSE, FALSE), 0);
  dt_pthread_mutex_init(&darktable.control->progress_system.mutex, NULL);
  dt_free(noiseprofiles);

  const struct CMUnitTest integration_tests[] = {
    cmocka_unit_test(_test_public_siblings_keep_sequence_for_distinct_destinations),
    cmocka_unit_test(_test_public_in_place_import_preserves_source),
    cmocka_unit_test(_test_public_delete_source_requires_successful_copy),
    cmocka_unit_test(_test_public_sibling_collision_reexpands_once),
    cmocka_unit_test(_test_public_fixed_destination_follows_all_policies),
    cmocka_unit_test(_test_public_unrelated_collision_has_no_sibling_fallback),
    cmocka_unit_test(_test_public_failures_consume_sequences_and_complete_once),
    cmocka_unit_test(_test_public_copy_failure_reservation_forces_sibling_fallback),
    cmocka_unit_test(_test_public_database_failure_reservation_forces_sibling_fallback),
    cmocka_unit_test(_test_public_reserved_fallback_reaches_policy_without_third_expansion),
  };
  const int integration_result = cmocka_run_group_tests(integration_tests, NULL, NULL);
  dt_pthread_mutex_destroy(&darktable.control->progress_system.mutex);
  dt_cleanup();
  _remove_tree(_integration_config);
  _remove_tree(_integration_cache);
  _remove_tree(_integration_tmp);
  dt_free(_integration_datadir);
  dt_free(_integration_tmp);
  dt_free(_integration_cache);
  dt_free(_integration_config);
  return integration_result;
}
