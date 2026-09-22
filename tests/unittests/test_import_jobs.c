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
  if(!IS_NULL_PTR(fixture->data.datetime)) g_date_time_unref(fixture->data.datetime);
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
  fixture->data.base_folder = test_case->target_is_file ? fixture->target : fixture->directory;
  fixture->data.target_subfolder_pattern = test_case->target_is_file ? "missing" : "read-only";
  fixture->data.target_file_pattern = "target.raw";
  fixture->data.datetime = g_date_time_new_now_local();
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
  import_result = _import_copy_file("source.raw", 1, &fixture->data,
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
    cmocka_unit_test(_test_image_import_extension_boundaries),
    cmocka_unit_test_prestate_setup_teardown(_test_copy_directory_failure_reports_only_its_cause,
      _directory_failure_setup, _directory_failure_teardown, &directory_fixtures[0]),
    cmocka_unit_test_prestate_setup_teardown(_test_copy_directory_failure_reports_only_its_cause,
      _directory_failure_setup, _directory_failure_teardown, &directory_fixtures[1]),
  };
  const int unit_result = cmocka_run_group_tests(tests, _setup, _teardown);
  if(unit_result) return unit_result;

  return 0;
}
