/*
    This file is part of darktable,
    Copyright (C) 2026 Guillaume Stutin.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/darktable.h"
#include "common/database.h"
#include "common/film.h"
#include "common/history_actions.h"
#include "common/history_merge.h"
#include "common/image.h"
#include "common/styles.h"

#include <assert.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

#ifndef ANSEL_TEST_SOURCE_DIR
#define ANSEL_TEST_SOURCE_DIR "."
#endif

#ifndef ANSEL_TEST_BINARY_DIR
#define ANSEL_TEST_BINARY_DIR "."
#endif

/**
 * @brief Run style-application scenarios from `tests/styles`.
 *
 * Each `*.dtstyle` file defines one scenario. The basename of the style file is
 * used to find `start_<style>.xmp` and `end_<style>.xmp`. The `start` sidecar is
 * optional: if it is missing, the runner keeps the raw import history. The
 * `end` sidecar is required and provides the expected history after applying
 * the style.
 *
 * The comparison is intentionally limited to the resulting pipe order and final
 * enabled states. The history stack can legitimately differ after a merge, so
 * the enabled-state check only looks at the last active history item for each
 * module instance.
 */
static char *test_image_dir = NULL;

/**
 * @brief One-line scenario result printed after every style has run.
 *
 * The full diagnostics are emitted where the failure is detected. This summary
 * keeps only the first failure reason so the final output stays readable when
 * several fixtures are executed in a single run.
 */
typedef struct dt_style_scenario_result_t
{
  char *style_file;
  int result;
  char *reason;
} dt_style_scenario_result_t;

static int test_fail(const char *scenario, const char *message, char **failure_reason)
{
  fprintf(stderr, "[FAIL] %s: %s\n", scenario, message);
  if(!IS_NULL_PTR(failure_reason) && IS_NULL_PTR(*failure_reason))
    *failure_reason = g_strdup(message);
  return 1;
}

static gboolean is_style_file(const char *filename)
{
  const char *extension = strrchr(filename, '.');
  return !IS_NULL_PTR(extension) && !g_ascii_strcasecmp(extension, ".dtstyle");
}

static char *scenario_name_from_style_file(const char *filename)
{
  char *scenario = g_strdup(filename);
  char *extension = strrchr(scenario, '.');
  if(!IS_NULL_PTR(extension)) *extension = '\0';
  return scenario;
}

static int sql_int_for_bound_images(const char *query, const int32_t imgid_a, const int32_t imgid_b)
{
  sqlite3_stmt *stmt = NULL;
  sqlite3_prepare_v2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  sqlite3_bind_int(stmt, 1, imgid_a);
  sqlite3_bind_int(stmt, 2, imgid_b);
  const int value = (sqlite3_step(stmt) == SQLITE_ROW) ? sqlite3_column_int(stmt, 0) : -1;
  sqlite3_finalize(stmt);
  return value;
}

static int max_style_id(void)
{
  sqlite3_stmt *stmt = NULL;
  sqlite3_prepare_v2(dt_database_get(darktable.db), "SELECT COALESCE(MAX(id), 0) FROM data.styles", -1,
                     &stmt, NULL);
  const int value = (sqlite3_step(stmt) == SQLITE_ROW) ? sqlite3_column_int(stmt, 0) : 0;
  sqlite3_finalize(stmt);
  return value;
}

static char *imported_style_name(const int before_import_style_id)
{
  sqlite3_stmt *stmt = NULL;
  sqlite3_prepare_v2(dt_database_get(darktable.db),
                     "SELECT name FROM data.styles WHERE id > ?1 ORDER BY id DESC LIMIT 1", -1, &stmt,
                     NULL);
  sqlite3_bind_int(stmt, 1, before_import_style_id);

  char *name = NULL;
  if(sqlite3_step(stmt) == SQLITE_ROW)
    name = g_strdup((const char *)sqlite3_column_text(stmt, 0));

  sqlite3_finalize(stmt);
  return name;
}

static int32_t create_test_image(const char *source_image_path)
{
  static int image_index = 0;
  const char *extension = strrchr(source_image_path, '.');
  char *filename = g_strdup_printf("style-test-%d%s", image_index++, extension ? extension : ".raw");
  char *image_path = g_build_filename(test_image_dir, filename, NULL);

  GFile *source_file = g_file_new_for_path(source_image_path);
  GFile *image_file = g_file_new_for_path(image_path);
  GError *error = NULL;
  const gboolean copied = g_file_copy(source_file, image_file, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &error);
  g_object_unref(source_file);
  g_object_unref(image_file);

  if(!copied)
  {
    fprintf(stderr, "[FAIL] copy test image: %s\n", error ? error->message : "unknown error");
    g_clear_error(&error);
    dt_free(filename);
    dt_free(image_path);
    return 0;
  }

  dt_film_t film;
  dt_film_init(&film);
  if(dt_film_new(&film, test_image_dir) <= 0)
  {
    dt_film_cleanup(&film);
    dt_free(filename);
    dt_free(image_path);
    return 0;
  }

  const int32_t imgid = dt_image_import(film.id, image_path, FALSE);

  dt_film_cleanup(&film);
  dt_free(filename);
  dt_free(image_path);
  return imgid;
}

static int load_xmp_on_image(const char *scenario, const int32_t imgid, const char *xmp_path,
                             char **failure_reason)
{
  if(!g_file_test(xmp_path, G_FILE_TEST_IS_REGULAR))
    return test_fail(scenario, "missing XMP fixture", failure_reason);

  return dt_history_load_and_apply(imgid, (gchar *)xmp_path, TRUE)
             ? test_fail(scenario, "could not load XMP fixture", failure_reason)
             : 0;
}

static int load_start_history(const char *scenario, const int32_t imgid, const char *start_xmp_path,
                              const char *start_xmp_name, char **failure_reason)
{
  if(g_file_test(start_xmp_path, G_FILE_TEST_IS_REGULAR))
    return load_xmp_on_image(scenario, imgid, start_xmp_path, failure_reason);

  printf("[NOTE] %s: no %s fixture, using imported base history\n", scenario, start_xmp_name);
  return 0;
}

static void print_module_order_summary(const char *label, const int32_t imgid)
{
  sqlite3_stmt *stmt = NULL;
  sqlite3_prepare_v2(dt_database_get(darktable.db),
                     "SELECT version, iop_list FROM main.module_order WHERE imgid=?1", -1, &stmt, NULL);
  sqlite3_bind_int(stmt, 1, imgid);

  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const unsigned char *iop_list = sqlite3_column_text(stmt, 1);
    fprintf(stderr, "  %s pipe order for image %d:\n", label, imgid);
    fprintf(stderr, "    version=%d\n", sqlite3_column_int(stmt, 0));
    fprintf(stderr, "    iop_list=%s\n", iop_list ? (const char *)iop_list : "(null)");
  }
  else
  {
    fprintf(stderr, "  %s pipe order for image %d: no module_order row\n", label, imgid);
  }

  sqlite3_finalize(stmt);
}

static int compare_pipe_order(const char *scenario, const int32_t actual_imgid, const int32_t expected_imgid,
                              char **failure_reason)
{
  const int diff = sql_int_for_bound_images(
      "SELECT"
      "  (SELECT COUNT(*) FROM"
      "     (SELECT version, iop_list FROM main.module_order WHERE imgid=?1"
      "      EXCEPT"
      "      SELECT version, iop_list FROM main.module_order WHERE imgid=?2))"
      "  +"
      "  (SELECT COUNT(*) FROM"
      "     (SELECT version, iop_list FROM main.module_order WHERE imgid=?2"
      "      EXCEPT"
      "      SELECT version, iop_list FROM main.module_order WHERE imgid=?1))",
      actual_imgid, expected_imgid);

  if(!diff) return 0;

  test_fail(scenario, "module order differs from expected XMP", failure_reason);
  print_module_order_summary("actual", actual_imgid);
  print_module_order_summary("expected", expected_imgid);
  return 1;
}

static void print_enabled_state_summary(const char *label, const int32_t imgid)
{
  fprintf(stderr, "  %s enabled state for image %d:\n", label, imgid);

  sqlite3_stmt *stmt = NULL;
  sqlite3_prepare_v2(
      dt_database_get(darktable.db),
      "SELECT h.operation, h.multi_priority, IFNULL(h.multi_name, ''), h.enabled"
      " FROM main.history h"
      " WHERE h.imgid=?1"
      "   AND h.num < (SELECT history_end FROM main.images WHERE id=?1)"
      "   AND h.num = (SELECT MAX(h2.num)"
      "                FROM main.history h2"
      "                WHERE h2.imgid=?1"
      "                  AND h2.operation=h.operation"
      "                  AND h2.multi_priority=h.multi_priority"
      "                  AND h2.num < (SELECT history_end FROM main.images WHERE id=?1))"
      " ORDER BY h.operation, h.multi_priority",
      -1, &stmt, NULL);
  sqlite3_bind_int(stmt, 1, imgid);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const unsigned char *operation = sqlite3_column_text(stmt, 0);
    const unsigned char *multi_name = sqlite3_column_text(stmt, 2);
    fprintf(stderr, "    %-24s priority=%d name='%s' enabled=%d\n",
            operation ? (const char *)operation : "", sqlite3_column_int(stmt, 1),
            multi_name ? (const char *)multi_name : "", sqlite3_column_int(stmt, 3));
  }

  sqlite3_finalize(stmt);
}

static int compare_enabled_state(const char *scenario, const int32_t actual_imgid,
                                 const int32_t expected_imgid, char **failure_reason)
{
  const int diff = sql_int_for_bound_images(
      "WITH actual AS ("
      "  SELECT h.operation, h.multi_priority, IFNULL(h.multi_name, '') AS multi_name, h.enabled"
      "  FROM main.history h"
      "  WHERE h.imgid=?1"
      "    AND h.num < (SELECT history_end FROM main.images WHERE id=?1)"
      "    AND h.num = (SELECT MAX(h2.num)"
      "                 FROM main.history h2"
      "                 WHERE h2.imgid=?1"
      "                   AND h2.operation=h.operation"
      "                   AND h2.multi_priority=h.multi_priority"
      "                   AND h2.num < (SELECT history_end FROM main.images WHERE id=?1))"
      "), expected AS ("
      "  SELECT h.operation, h.multi_priority, IFNULL(h.multi_name, '') AS multi_name, h.enabled"
      "  FROM main.history h"
      "  WHERE h.imgid=?2"
      "    AND h.num < (SELECT history_end FROM main.images WHERE id=?2)"
      "    AND h.num = (SELECT MAX(h2.num)"
      "                 FROM main.history h2"
      "                 WHERE h2.imgid=?2"
      "                   AND h2.operation=h.operation"
      "                   AND h2.multi_priority=h.multi_priority"
      "                   AND h2.num < (SELECT history_end FROM main.images WHERE id=?2))"
      ")"
      "SELECT"
      "  (SELECT COUNT(*) FROM (SELECT * FROM actual EXCEPT SELECT * FROM expected))"
      "  +"
      "  (SELECT COUNT(*) FROM (SELECT * FROM expected EXCEPT SELECT * FROM actual))",
      actual_imgid, expected_imgid);

  if(!diff) return 0;

  test_fail(scenario, "final enabled states differ from expected XMP", failure_reason);
  print_enabled_state_summary("actual", actual_imgid);
  print_enabled_state_summary("expected", expected_imgid);
  return 1;
}

static int apply_style_to_image(const char *scenario, const char *style_name, const int32_t imgid,
                                char **failure_reason)
{
  const int style_id = dt_styles_get_id_by_name(style_name);
  if(style_id == 0)
    return test_fail(scenario, "imported style is missing from the database", failure_reason);

  dt_conf_set_bool("history/copy_iop_order", FALSE);
  dt_conf_set_bool("history/paste_instances", TRUE);

  return dt_styles_apply_to_image_merge(style_name, style_id, imgid, DT_HISTORY_MERGE_APPEND, NULL)
             ? test_fail(scenario, "style application failed", failure_reason)
             : 0;
}

static int run_style_scenario(const char *scenario_dir, const char *source_image_path, const char *style_file,
                              char **failure_reason)
{
  char *scenario = scenario_name_from_style_file(style_file);
  char *style_path = g_build_filename(scenario_dir, style_file, NULL);
  char *start_xmp_name = g_strdup_printf("start_%s.xmp", scenario);
  char *end_xmp_name = g_strdup_printf("end_%s.xmp", scenario);
  char *start_xmp_path = g_build_filename(scenario_dir, start_xmp_name, NULL);
  char *end_xmp_path = g_build_filename(scenario_dir, end_xmp_name, NULL);

  printf("\n[STEP] %s\n", scenario);

  int result = 1;
  const int before_import_style_id = max_style_id();
  dt_styles_import_from_file(style_path);
  char *style_name = imported_style_name(before_import_style_id);
  if(IS_NULL_PTR(style_name)) style_name = g_strdup(scenario);

  const int32_t actual_imgid = create_test_image(source_image_path);
  const int32_t expected_imgid = create_test_image(source_image_path);

  if(actual_imgid <= 0 || expected_imgid <= 0)
  {
    test_fail(scenario, "could not import test image", failure_reason);
    goto end;
  }

  if(load_start_history(scenario, actual_imgid, start_xmp_path, start_xmp_name, failure_reason)) goto end;
  if(apply_style_to_image(scenario, style_name, actual_imgid, failure_reason)) goto end;
  if(load_xmp_on_image(scenario, expected_imgid, end_xmp_path, failure_reason)) goto end;
  if(compare_pipe_order(scenario, actual_imgid, expected_imgid, failure_reason)) goto end;
  if(compare_enabled_state(scenario, actual_imgid, expected_imgid, failure_reason)) goto end;

  printf("[OK] %s\n", scenario);
  result = 0;

end:
  dt_free(style_name);
  dt_free(scenario);
  dt_free(style_path);
  dt_free(start_xmp_name);
  dt_free(end_xmp_name);
  dt_free(start_xmp_path);
  dt_free(end_xmp_path);
  return result;
}

static int run_style_scenarios(const char *scenario_dir, const char *source_image_path)
{
  GError *error = NULL;
  GDir *dir = g_dir_open(scenario_dir, 0, &error);
  if(IS_NULL_PTR(dir))
  {
    fprintf(stderr, "[FAIL] cannot open style scenario folder `%s`: %s\n", scenario_dir,
            error ? error->message : "unknown error");
    g_clear_error(&error);
    return 1;
  }

  GList *style_files = NULL;
  const char *entry = NULL;
  while((entry = g_dir_read_name(dir)) != NULL)
  {
    if(is_style_file(entry)) style_files = g_list_prepend(style_files, g_strdup(entry));
  }
  g_dir_close(dir);

  style_files = g_list_sort(style_files, (GCompareFunc)g_strcmp0);
  if(IS_NULL_PTR(style_files))
  {
    printf("[SKIP] no .dtstyle file found in %s\n", scenario_dir);
    return 0;
  }

  int result = 0;
  GList *summaries = NULL;
  for(GList *l = style_files; l; l = g_list_next(l))
  {
    const char *style_file = (const char *)l->data;
    dt_style_scenario_result_t *summary = g_malloc0(sizeof(*summary));
    summary->style_file = g_strdup(style_file);
    summary->result = run_style_scenario(scenario_dir, source_image_path, style_file, &summary->reason);
    if(summary->result && IS_NULL_PTR(summary->reason))
      summary->reason = g_strdup("unknown failure");

    summaries = g_list_prepend(summaries, summary);
    result |= summary->result;
  }

  summaries = g_list_reverse(summaries);
  printf("\n[SUMMARY]\n");
  for(GList *l = summaries; l; l = g_list_next(l))
  {
    dt_style_scenario_result_t *summary = (dt_style_scenario_result_t *)l->data;
    printf("%s: %s", summary->style_file, summary->result ? "FAILED" : "PASSED");
    if(summary->result)
      printf(" - %s", summary->reason);
    printf("\n");
  }

  for(GList *l = summaries; l; l = g_list_next(l))
  {
    dt_style_scenario_result_t *summary = (dt_style_scenario_result_t *)l->data;
    dt_free(summary->style_file);
    dt_free(summary->reason);
    dt_free(summary);
  }
  g_list_free(summaries);
  g_list_free_full(style_files, dt_free_gpointer);
  return result;
}

static char *prepare_test_datadir(const char *tmp_dir)
{
  char *datadir = g_build_filename(tmp_dir, "data", NULL);
  char *rawspeed_dir = g_build_filename(datadir, "rawspeed", NULL);
  char *rawspeed_xml = g_build_filename(rawspeed_dir, "cameras.xml", NULL);
  char *rawspeed_source_xml = g_build_filename(ANSEL_TEST_SOURCE_DIR, "src", "external", "rawspeed", "data",
                                               "cameras.xml", NULL);

  g_mkdir(datadir, 0700);
  g_mkdir(rawspeed_dir, 0700);

  GError *error = NULL;
  GFile *source_file = g_file_new_for_path(rawspeed_source_xml);
  GFile *dest_file = g_file_new_for_path(rawspeed_xml);
  const gboolean copied = g_file_copy(source_file, dest_file, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &error);
  g_object_unref(source_file);
  g_object_unref(dest_file);

  if(!copied)
  {
    fprintf(stderr, "[FAIL] copy rawspeed cameras.xml: %s\n", error ? error->message : "unknown error");
    g_clear_error(&error);
    dt_free(datadir);
    datadir = NULL;
  }

  dt_free(rawspeed_dir);
  dt_free(rawspeed_xml);
  dt_free(rawspeed_source_xml);
  return datadir;
}

int main(int argc, char *argv[])
{
  const gboolean explicit_scenario_dir = argc > 1;
  char *default_scenario_dir = g_build_filename(ANSEL_TEST_SOURCE_DIR, "tests", "styles", NULL);
  char *default_source_image = g_build_filename(ANSEL_TEST_SOURCE_DIR, "tests", "integration", "images",
                                               "mire1.cr2", NULL);
  const char *scenario_dir = explicit_scenario_dir ? argv[1] : default_scenario_dir;
  const char *source_image_path = (argc > 2) ? argv[2] : default_source_image;

  if(!g_file_test(scenario_dir, G_FILE_TEST_IS_DIR))
  {
    fprintf(stderr, explicit_scenario_dir ? "[FAIL] style scenario folder does not exist: %s\n"
                                          : "[SKIP] style scenario folder does not exist: %s\n",
            scenario_dir);
    dt_free(default_scenario_dir);
    dt_free(default_source_image);
    return explicit_scenario_dir ? 1 : 0;
  }

  if(!g_file_test(source_image_path, G_FILE_TEST_IS_REGULAR))
  {
    fprintf(stderr, "[FAIL] test image does not exist: %s\n", source_image_path);
    dt_free(default_scenario_dir);
    dt_free(default_source_image);
    return 1;
  }

  char *config_dir = g_strdup_printf("%s/ansel-test-styles-config-XXXXXX", g_get_tmp_dir());
  char *cache_dir = g_strdup_printf("%s/ansel-test-styles-cache-XXXXXX", g_get_tmp_dir());
  char *tmp_dir = g_strdup_printf("%s/ansel-test-styles-tmp-XXXXXX", g_get_tmp_dir());

  assert(!IS_NULL_PTR(g_mkdtemp(config_dir)));
  assert(!IS_NULL_PTR(g_mkdtemp(cache_dir)));
  assert(!IS_NULL_PTR(g_mkdtemp(tmp_dir)));
  test_image_dir = tmp_dir;

  char *test_datadir = prepare_test_datadir(tmp_dir);
  if(IS_NULL_PTR(test_datadir))
  {
    dt_free(default_scenario_dir);
    dt_free(default_source_image);
    dt_free(config_dir);
    dt_free(cache_dir);
    dt_free(tmp_dir);
    return 1;
  }

  char *noiseprofiles = g_build_filename(ANSEL_TEST_SOURCE_DIR, "data", "noiseprofiles.json", NULL);
  char *argv_override[] = {
    "ansel-test-styles",
    "--library", ":memory:",
    "--datadir", test_datadir,
    "--noiseprofiles", noiseprofiles,
    "--moduledir", ANSEL_TEST_BINARY_DIR "/lib/ansel",
    "--configdir", config_dir,
    "--cachedir", cache_dir,
    "--tmpdir", tmp_dir,
    "--disable-opencl",
    "--conf", "write_sidecar_files=FALSE",
    "--conf", "plugins/lighttable/export/force_lcms2=FALSE",
    "-t", "1",
    NULL
  };
  int argc_override = sizeof(argv_override) / sizeof(*argv_override) - 1;

  if(dt_init(argc_override, argv_override, FALSE, FALSE)) exit(1);

  const int result = run_style_scenarios(scenario_dir, source_image_path);

  dt_cleanup();
  dt_free(noiseprofiles);
  dt_free(test_datadir);
  dt_free(default_scenario_dir);
  dt_free(default_source_image);
  dt_free(config_dir);
  dt_free(cache_dir);
  dt_free(tmp_dir);

  return result;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
