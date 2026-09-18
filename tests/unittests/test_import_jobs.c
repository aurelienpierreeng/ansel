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

extern int _import_copy_file(const char *filename, int index, dt_control_import_t *data,
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
static int _setup(void **state)
{
  _rcfile = g_build_filename(g_get_tmp_dir(), "ansel-test-import-jobs.rc", NULL);
  g_remove(_rcfile);
  darktable.conf = calloc(1, sizeof(dt_conf_t));
  dt_conf_init(darktable.conf, _rcfile, NULL);
  return 0;
}

static int _teardown(void **state)
{
  dt_conf_cleanup(darktable.conf);
  free(darktable.conf);
  darktable.conf = NULL;
  g_remove(_rcfile);
  dt_free(_rcfile);
  _rcfile = NULL;
  return 0;
}

static void _test_extension_contract(void **state)
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

static void _test_extension_utility_rejections(void **state)
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

static void _test_image_import_extension_boundaries(void **state)
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

static void _test_copy_creation_failure_reports_only_creation(void **state)
{
  char *directory = g_dir_make_tmp("ansel-test-import-copy-XXXXXX", NULL);
  assert_non_null(directory);
  char *blocker = g_build_filename(directory, "blocker", NULL);
  assert_true(g_file_set_contents(blocker, "x", 1, NULL));

  dt_control_t control = { 0 };
  darktable.control = &control;
  dt_pthread_mutex_init(&control.log_mutex, NULL);

  dt_control_import_t data = { 0 };
  data.base_folder = blocker;
  data.target_subfolder_pattern = "missing";
  data.target_file_pattern = "target.raw";
  data.target_dir = g_build_filename(blocker, "missing", NULL);
  data.datetime = g_date_time_new_now_local();
  GList *discarded = NULL;
  gchar image_path[DT_PATH_MAX] = { 0 };
  char *diagnostics = NULL;
  const int diagnostic_fd = g_file_open_tmp("ansel-test-import-jobs-XXXXXX", &diagnostics, NULL);
  assert_true(diagnostic_fd >= 0);
  fflush(stdout);
  fflush(stderr);
  const int stdout_fd = test_dup(fileno(stdout));
  const int stderr_fd = test_dup(fileno(stderr));
  assert_true(stdout_fd >= 0);
  assert_true(stderr_fd >= 0);
  assert_int_equal(test_dup2(diagnostic_fd, fileno(stdout)), fileno(stdout));
  assert_int_equal(test_dup2(diagnostic_fd, fileno(stderr)), fileno(stderr));
  test_close(diagnostic_fd);

  assert_int_equal(_import_copy_file("source.raw", 1, &data, image_path, sizeof(image_path), &discarded), -1);

  fflush(stdout);
  fflush(stderr);
  assert_int_equal(test_dup2(stdout_fd, fileno(stdout)), fileno(stdout));
  assert_int_equal(test_dup2(stderr_fd, fileno(stderr)), fileno(stderr));
  test_close(stdout_fd);
  test_close(stderr_fd);
  gchar *output = NULL;
  assert_true(g_file_get_contents(diagnostics, &output, NULL, NULL));
  assert_int_equal(control.log_pos, 1);
  assert_non_null(strstr(control.log_message[0], "Impossible to create directory"));
  assert_non_null(strstr(output, "Unable to create the target folder"));
  assert_null(strstr(output, "Not allowed to write"));
  assert_null(strstr(output, "Unable to copy the file"));
  assert_int_equal(image_path[0], '\0');
  assert_null(discarded);

  dt_free(output);
  g_remove(diagnostics);
  dt_free(diagnostics);
  dt_free(data.target_dir);
  g_date_time_unref(data.datetime);
  dt_pthread_mutex_destroy(&control.log_mutex);
  darktable.control = NULL;
  g_remove(blocker);
  g_rmdir(directory);
  dt_free(blocker);
  dt_free(directory);
}

static void _test_copy_non_writable_directory_reports_only_writability(void **state)
{
  char *directory = g_dir_make_tmp("ansel-test-import-copy-XXXXXX", NULL);
  assert_non_null(directory);
  char *target_dir = g_build_filename(directory, "read-only", NULL);
  assert_int_equal(g_mkdir(target_dir, 0700), 0);
  assert_int_equal(g_chmod(target_dir, 0500), 0);
  if(g_access(target_dir, W_OK | X_OK) == 0)
  {
    assert_int_equal(g_chmod(target_dir, 0700), 0);
    g_rmdir(target_dir);
    g_rmdir(directory);
    dt_free(target_dir);
    dt_free(directory);
    skip();
  }

  dt_control_import_t data = { 0 };
  data.base_folder = directory;
  data.target_subfolder_pattern = "read-only";
  data.target_file_pattern = "target.raw";
  data.target_dir = g_strdup(target_dir);
  data.datetime = g_date_time_new_now_local();
  GList *discarded = NULL;
  gchar image_path[DT_PATH_MAX] = { 0 };
  char *diagnostics = NULL;
  const int diagnostic_fd = g_file_open_tmp("ansel-test-import-jobs-XXXXXX", &diagnostics, NULL);
  assert_true(diagnostic_fd >= 0);
  fflush(stdout);
  fflush(stderr);
  const int stdout_fd = test_dup(fileno(stdout));
  const int stderr_fd = test_dup(fileno(stderr));
  assert_true(stdout_fd >= 0);
  assert_true(stderr_fd >= 0);
  assert_int_equal(test_dup2(diagnostic_fd, fileno(stdout)), fileno(stdout));
  assert_int_equal(test_dup2(diagnostic_fd, fileno(stderr)), fileno(stderr));
  test_close(diagnostic_fd);

  assert_int_equal(_import_copy_file("source.raw", 1, &data, image_path, sizeof(image_path), &discarded), -1);

  fflush(stdout);
  fflush(stderr);
  assert_int_equal(test_dup2(stdout_fd, fileno(stdout)), fileno(stdout));
  assert_int_equal(test_dup2(stderr_fd, fileno(stderr)), fileno(stderr));
  test_close(stdout_fd);
  test_close(stderr_fd);
  gchar *output = NULL;
  assert_true(g_file_get_contents(diagnostics, &output, NULL, NULL));
  assert_non_null(strstr(output, "Not allowed to write"));
  assert_null(strstr(output, "Unable to create the target folder"));
  assert_null(strstr(output, "Unable to copy the file"));
  assert_int_equal(image_path[0], '\0');
  assert_null(discarded);

  dt_free(output);
  g_remove(diagnostics);
  dt_free(diagnostics);
  dt_free(data.target_dir);
  g_date_time_unref(data.datetime);
  assert_int_equal(g_chmod(target_dir, 0700), 0);
  g_rmdir(target_dir);
  g_rmdir(directory);
  dt_free(target_dir);
  dt_free(directory);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_test_extension_contract),
    cmocka_unit_test(_test_extension_utility_rejections),
    cmocka_unit_test(_test_image_import_extension_boundaries),
    cmocka_unit_test(_test_copy_creation_failure_reports_only_creation),
    cmocka_unit_test(_test_copy_non_writable_directory_reports_only_writability),
  };
  const int unit_result = cmocka_run_group_tests(tests, _setup, _teardown);
  if(unit_result) return unit_result;
  return 0;
}
