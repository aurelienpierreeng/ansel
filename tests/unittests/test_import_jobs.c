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

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_test_extension_contract),
    cmocka_unit_test(_test_extension_utility_rejections),
    cmocka_unit_test(_test_image_import_extension_boundaries),
  };
  const int unit_result = cmocka_run_group_tests(tests, _setup, _teardown);
  if(unit_result) return unit_result;
  return 0;
}
