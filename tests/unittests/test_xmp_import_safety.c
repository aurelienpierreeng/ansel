/*
    This file is part of Ansel,
    Copyright (C) 2026 Paolo SANTUCCI.
    Copyright (C) 2026 Guillaume Stutin.

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "testdb.h"

#include "common/conf.h"
#include "common/datetime.h"
#include "common/history_actions.h"
#include "common/image.h"
#include "common/undo.h"
#include "common/xmp_sidecar.h"
#include "caches/image_cache.h"
#include "database/database.h"
#include "database/image_repository.h"
#include "darktable.h"
#include "develop/blend.h"
#include "develop/masks.h"
#include "metadata/exif.h"
#include "metadata/metadata.h"
#include "metadata/notify.h"

#include <glib/gstdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>

#ifdef __linux__
#include <dlfcn.h>
#include <pthread.h>
#endif

#ifdef _WIN32
#include <io.h>
#include "win/main_wrapper.h"
#define test_dup _dup
// _dup2() answers 0 on success where POSIX dup2() answers the target descriptor
#define test_dup2(fd, fd2) (_dup2((fd), (fd2)) == 0 ? (fd2) : -1)
#define test_close _close
#else
#include <unistd.h>
#define test_dup dup
#define test_dup2 dup2
#define test_close close
#endif

static char *config_path = NULL;
static char *xmp_path = NULL;
static int tags_changed_count = 0;

#ifdef __linux__
typedef enum xmp_transaction_event_t
{
  XMP_TRANSACTION_BEGIN,
  XMP_TRANSACTION_WRITER_READY,
  XMP_TRANSACTION_COORDINATOR_COMPLETE,
  XMP_TRANSACTION_WRITER_COMPLETE,
  XMP_TRANSACTION_EVENT_COUNT,
} xmp_transaction_event_t;

typedef struct xmp_transaction_barrier_t
{
  pthread_mutex_t mutex;
  pthread_cond_t condition;
  gboolean begin_complete;
  gboolean writer_ready;
  gboolean writer_complete;
  gboolean fail_commit;
  gboolean fail_write_timestamp;
  int rollback_count;
  xmp_transaction_event_t events[XMP_TRANSACTION_EVENT_COUNT];
  int event_count;
} xmp_transaction_barrier_t;

typedef int (*sqlite3_exec_t)(sqlite3 *, const char *, sqlite3_callback, void *, char **);
typedef int (*sqlite3_step_t)(sqlite3_stmt *);

static sqlite3_exec_t real_sqlite3_exec = NULL;
static sqlite3_step_t real_sqlite3_step = NULL;
static xmp_transaction_barrier_t *xmp_transaction_barrier = NULL;

static void record_xmp_transaction_event(xmp_transaction_barrier_t *barrier, const xmp_transaction_event_t event)
{
  if(barrier->event_count < 0 || barrier->event_count >= XMP_TRANSACTION_EVENT_COUNT)
  {
    fail_msg("too many XMP transaction events");
    return;
  }
  barrier->events[barrier->event_count++] = event;
}

int sqlite3_exec(sqlite3 *database, const char *sql, sqlite3_callback callback, void *data, char **error)
{
  xmp_transaction_barrier_t *barrier = xmp_transaction_barrier;
  if(IS_NULL_PTR(barrier)) return real_sqlite3_exec(database, sql, callback, data, error);

  if(!strcmp(sql, "COMMIT TRANSACTION") && barrier->fail_commit) return SQLITE_ERROR;

  const int result = real_sqlite3_exec(database, sql, callback, data, error);
  if(strcmp(sql, "BEGIN TRANSACTION") || result != SQLITE_OK)
  {
    if(!strcmp(sql, "ROLLBACK TRANSACTION") || (!strcmp(sql, "COMMIT TRANSACTION") && result == SQLITE_OK))
    {
      pthread_mutex_lock(&barrier->mutex);
      if(!strcmp(sql, "ROLLBACK TRANSACTION")) barrier->rollback_count++;
      record_xmp_transaction_event(barrier, XMP_TRANSACTION_COORDINATOR_COMPLETE);
      pthread_cond_broadcast(&barrier->condition);
      pthread_mutex_unlock(&barrier->mutex);
    }
    return result;
  }

  pthread_mutex_lock(&barrier->mutex);
  barrier->begin_complete = TRUE;
  record_xmp_transaction_event(barrier, XMP_TRANSACTION_BEGIN);
  pthread_cond_broadcast(&barrier->condition);
  while(!barrier->writer_ready) pthread_cond_wait(&barrier->condition, &barrier->mutex);
  pthread_mutex_unlock(&barrier->mutex);
  return result;
}

int sqlite3_step(sqlite3_stmt *statement)
{
  xmp_transaction_barrier_t *barrier = xmp_transaction_barrier;
  if(IS_NULL_PTR(barrier)
     || strcmp(sqlite3_sql(statement), "UPDATE main.images SET write_timestamp = ?2 WHERE id = ?1"))
    return real_sqlite3_step(statement);

  pthread_mutex_lock(&barrier->mutex);
  const gboolean fail_write_timestamp = barrier->fail_write_timestamp;
  barrier->fail_write_timestamp = FALSE;
  pthread_mutex_unlock(&barrier->mutex);
  if(fail_write_timestamp) return SQLITE_ERROR;
  return real_sqlite3_step(statement);
}
#endif

static void record_tags_changed(void)
{
  tags_changed_count++;
}

typedef struct { uint32_t mode; float opacity; uint32_t mask_id; } blend_params_v1_t;
typedef struct { uint32_t mode; float opacity; uint32_t mask_id; uint32_t blendif; float blendif_parameters[32]; } blend_params_v2_t;
typedef struct { uint32_t mode; float opacity; uint32_t mask_id; uint32_t blendif; float blendif_parameters[64]; } blend_params_v3_t;
typedef struct { uint32_t mode; float opacity; uint32_t mask_id; uint32_t blendif; float radius; float blendif_parameters[64]; } blend_params_v4_t;
typedef struct { uint32_t mask_mode; uint32_t blend_mode; float opacity; uint32_t mask_combine; uint32_t mask_id; uint32_t blendif; float radius; uint32_t reserved[4]; float blendif_parameters[64]; } blend_params_v5_t;
typedef struct { uint32_t mask_mode; uint32_t blend_mode; float opacity; uint32_t mask_combine; uint32_t mask_id; uint32_t blendif; float feathering_radius; uint32_t feathering_guide; float blur_radius; float contrast; float brightness; uint32_t reserved[4]; float blendif_parameters[64]; } blend_params_v8_t;
typedef struct { uint32_t mask_mode; uint32_t blend_mode; float opacity; uint32_t mask_combine; uint32_t mask_id; uint32_t blendif; float feathering_radius; uint32_t feathering_guide; float blur_radius; float contrast; float brightness; uint32_t reserved[4]; float blendif_parameters[64]; dt_dev_operation_t raster_mask_source; int raster_mask_instance; int raster_mask_id; gboolean raster_mask_invert; } blend_params_v9_t;
typedef struct { uint32_t mask_mode; int32_t blend_cst; uint32_t blend_mode; float blend_parameter; float opacity; uint32_t mask_combine; uint32_t mask_id; uint32_t blendif; float feathering_radius; uint32_t feathering_guide; float blur_radius; float contrast; float brightness; uint32_t reserved[4]; float blendif_parameters[64]; float blendif_boost_factors[16]; dt_dev_operation_t raster_mask_source; int raster_mask_instance; int raster_mask_id; gboolean raster_mask_invert; } blend_params_v10_t;

static int setup(void **state)
{
#ifdef __linux__
  real_sqlite3_exec = (sqlite3_exec_t)dlsym(RTLD_NEXT, "sqlite3_exec");
  real_sqlite3_step = (sqlite3_step_t)dlsym(RTLD_NEXT, "sqlite3_step");
  if(IS_NULL_PTR(real_sqlite3_exec) || IS_NULL_PTR(real_sqlite3_step)) return -1;
#endif
  darktable.configdir = (char *)g_get_tmp_dir();
  darktable.cachedir = (char *)g_get_tmp_dir();
  darktable.tmpdir = (char *)g_get_tmp_dir();
  darktable.datadir = (char *)g_get_tmp_dir();
  if(IS_NULL_PTR(dt_datetime_origin())) dt_datetime_init();
  if(testdb_setup(state)) return -1;

  const int config_fd = g_file_open_tmp("ansel-test-xmp-import-safety-XXXXXX.rc", &config_path, NULL);
  if(config_fd < 0) return -1;
  g_close(config_fd, NULL);
  g_remove(config_path);

  darktable.conf = calloc(1, sizeof(dt_conf_t));
  if(IS_NULL_PTR(darktable.conf)) return -1;

  darktable.undo = dt_undo_init();
  if(IS_NULL_PTR(darktable.undo)) return -1;

  dt_conf_init(darktable.conf, config_path, NULL);
  dt_conf_set_string("write_sidecar_files", "FALSE");
  dt_image_xmp_mode_refresh_from_conf();
  dt_exif_init();

  const int fd = g_file_open_tmp("ansel-test-xmp-import-safety-XXXXXX.xmp", &xmp_path, NULL);
  if(fd < 0) return -1;
  g_close(fd, NULL);
  dt_image_cache_init(FALSE);
  if(!dt_image_cache_is_ready()) return -1;
  return 0;
}

static int teardown(void **state)
{
  dt_metadata_set_tags_changed_handler(NULL);
  dt_exif_cleanup();
  dt_undo_cleanup(darktable.undo);
  darktable.undo = NULL;
  dt_conf_cleanup(darktable.conf);
  dt_free(darktable.conf);
  darktable.conf = NULL;
  g_remove(config_path);
  dt_free(config_path);
  config_path = NULL;
  g_remove(xmp_path);
  dt_free(xmp_path);
  xmp_path = NULL;
  dt_image_cache_cleanup();
  darktable.configdir = NULL;
  darktable.cachedir = NULL;
  darktable.tmpdir = NULL;
  darktable.datadir = NULL;
  return testdb_teardown(state);
}

static dt_image_t make_image(const char *filename)
{
  const int32_t film = testdb_make_film("/testdb/xmp-import-safety");
  const int32_t imgid = testdb_make_image(film, filename);
  assert_true(imgid > 0);

  dt_image_t image;
  dt_image_init(&image);
  image.id = imgid;
  image.film_id = film;
  image.group_id = imgid;
  const time_t unix_timestamp = time(NULL);
  assert_true(dt_datetime_unix_to_img(&image, &unix_timestamp));
  image.import_timestamp = image.exif_datetime_taken;
  image.change_timestamp = image.exif_datetime_taken;
  image.export_timestamp = image.exif_datetime_taken;
  image.print_timestamp = image.exif_datetime_taken;
  g_strlcpy(image.filename, filename, sizeof(image.filename));
  dt_image_repository_store(&image);
  assert_true(dt_image_repository_load(imgid, &image));
  return image;
}

static void write_xmp(const int version, const char *properties)
{
  char *packet = g_strdup_printf(
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">"
      "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
      "<rdf:Description rdf:about=\"\" xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\" "
      "xmlns:darktable=\"http://darktable.sf.net/\" "
      "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" darktable:xmp_version=\"%d\">%s"
      "</rdf:Description></rdf:RDF></x:xmpmeta>",
      version, properties);
  assert_true(g_file_set_contents(xmp_path, packet, -1, NULL));
  dt_free(packet);
}

static char *capture_xmp_diagnostic(dt_image_t *image, const dt_xmp_import_result_t expected_result)
{
  char *diagnostic_path = NULL;
  const int diagnostic_fd = g_file_open_tmp("ansel-test-xmp-diagnostic-XXXXXX", &diagnostic_path, NULL);
  if(diagnostic_fd < 0)
  {
    fail();
    return NULL;
  }
  const int stderr_fd = test_dup(fileno(stderr));
  if(stderr_fd < 0)
  {
    test_close(diagnostic_fd);
    g_remove(diagnostic_path);
    dt_free(diagnostic_path);
    fail();
    return NULL;
  }
  fflush(stderr);
  assert_int_equal(test_dup2(diagnostic_fd, fileno(stderr)), fileno(stderr));

  const dt_xmp_import_result_t result = dt_exif_xmp_read(image, xmp_path, FALSE, NULL);

  fflush(stderr);
  assert_int_equal(test_dup2(stderr_fd, fileno(stderr)), fileno(stderr));
  assert_int_equal(test_close(stderr_fd), 0);
  assert_int_equal(test_close(diagnostic_fd), 0);
  assert_int_equal(result, expected_result);
  char *diagnostic = NULL;
  assert_true(g_file_get_contents(diagnostic_path, &diagnostic, NULL, NULL));
  g_remove(diagnostic_path);
  dt_free(diagnostic_path);
  return diagnostic;
}

static void assert_stable_xmp_rejection(dt_image_t *image, const char *category)
{
  char *diagnostic = capture_xmp_diagnostic(image, DT_XMP_IMPORT_READ_ERROR);
  char *basename = g_path_get_basename(xmp_path);
  assert_non_null(strstr(diagnostic, basename));
  assert_non_null(strstr(diagnostic, category));
  assert_null(strstr(diagnostic, "<rdf:"));
  assert_null(strstr(strstr(diagnostic, basename) + strlen(basename), basename));
  dt_free(basename);
  dt_free(diagnostic);
}

static void seed_development(const int32_t imgid)
{
  static const unsigned char params[] = { 0, 1, 2, 3 };
  static const unsigned char points[] = { 4, 5, 6, 7 };
  static const unsigned char source[] = { 8, 9 };

  assert_true(dt_history_repository_write_item(imgid, 0, "exposure", params, sizeof(params), 1, TRUE,
                                                NULL, 0, 1, 0, ""));
  assert_true(dt_history_repository_write_mask_item(imgid, 0, 42, 1, "keep", 1, points, sizeof(points),
                                                     1, source, sizeof(source)));
}

static void assert_development_preserved(const int32_t imgid)
{
  assert_int_equal(dt_history_repository_count_items(imgid), 1);
  assert_int_equal(dt_history_repository_count_mask_items(imgid), 1);
}

typedef struct mask_history_rows_t
{
  int count;
  int nums[2];
  int formids[2];
  int types[2];
  int versions[2];
  int point_counts[2];
  char names[2][8];
  dt_masks_node_circle_t points[2];
  float sources[2][2];
} mask_history_rows_t;

static void collect_mask_history_rows(void *user_data, const dt_history_repository_mask_row_t *row)
{
  mask_history_rows_t *rows = (mask_history_rows_t *)user_data;
  if(row->form == 0 || row->form == DT_MASKS_GROUP) return;
  if(rows->count >= (int)G_N_ELEMENTS(rows->nums))
  {
    assert_true(FALSE);
    return;
  }
  rows->nums[rows->count] = row->num;
  rows->formids[rows->count] = row->mask_id;
  rows->types[rows->count] = row->form;
  rows->versions[rows->count] = row->version;
  rows->point_counts[rows->count] = row->points_count;
  g_strlcpy(rows->names[rows->count], row->name, sizeof(rows->names[rows->count]));
  assert_int_equal(row->points_len, (int)sizeof(rows->points[rows->count]));
  memcpy(&rows->points[rows->count], row->points, sizeof(rows->points[rows->count]));
  assert_int_equal(row->source_len, (int)sizeof(rows->sources[rows->count]));
  memcpy(rows->sources[rows->count], row->source, sizeof(rows->sources[rows->count]));
  rows->count++;
}

typedef struct group_mask_history_row_t
{
  int count;
  int mask_id;
  int points_len;
  dt_masks_form_group_t members[2];
} group_mask_history_row_t;

static void collect_group_mask_history_row(void *user_data, const dt_history_repository_mask_row_t *row)
{
  group_mask_history_row_t *group = user_data;
  if(row->form != DT_MASKS_GROUP) return;

  if(group->count != 0 || row->points_len < 0 || row->points_len > (int)sizeof(group->members))
  {
    fail();
    return;
  }
  group->mask_id = row->mask_id;
  group->points_len = row->points_len;
  memcpy(group->members, row->points, row->points_len);
  group->count++;
}

static char *make_v3_circle_mask(const char *attributes, const char *name,
                                 const dt_masks_node_circle_t *point, const float source[2])
{
  char *points = dt_exif_xmp_encode((const unsigned char *)point, sizeof(*point), NULL);
  char *source_position = dt_exif_xmp_encode((const unsigned char *)source, 2 * sizeof(float), NULL);
  char *mask = g_strdup_printf("<rdf:li %s darktable:mask_type=\"%d\" darktable:mask_name=\"%s\" "
                               "darktable:mask_version=\"%d\" darktable:mask_points=\"%s\" darktable:mask_nb=\"1\" "
                               "darktable:mask_src=\"%s\"/>",
                               attributes, DT_MASKS_CIRCLE, name, DEVELOP_MASKS_VERSION, points, source_position);
  dt_free(source_position);
  dt_free(points);
  return mask;
}

static char *make_v3_history_step(const int num)
{
  static const unsigned char params[] = { 0, 0 };
  char *encoded_params = dt_exif_xmp_encode(params, sizeof(params), NULL);
  char *history = g_strdup_printf("<rdf:li darktable:num=\"%d\" darktable:operation=\"exposure\" "
                                  "darktable:enabled=\"1\" darktable:modversion=\"1\" darktable:params=\"%s\"/>",
                                  num, encoded_params);
  dt_free(encoded_params);
  return history;
}

static char *make_v3_history_step_with_blend(const int num, const int blend_version,
                                              const unsigned char *blend, const int blend_len)
{
  static const unsigned char params[] = { 0, 0 };
  char *encoded_params = dt_exif_xmp_encode(params, sizeof(params), NULL);
  char *encoded_blend = dt_exif_xmp_encode(blend, blend_len, NULL);
  char *history = g_strdup_printf("<rdf:li darktable:num=\"%d\" darktable:operation=\"exposure\" "
                                  "darktable:enabled=\"1\" darktable:modversion=\"1\" darktable:params=\"%s\" "
                                  "darktable:blendop_version=\"%d\" darktable:blendop_params=\"%s\"/>",
                                  num, encoded_params, blend_version, encoded_blend);
  dt_free(encoded_blend);
  dt_free(encoded_params);
  return history;
}

static char *make_v3_group_mask(const int mask_num, const int mask_id,
                                 const dt_masks_form_group_t *members, const int member_count)
{
  const int points_len = member_count * (int)sizeof(*members);
  char *points = dt_exif_xmp_encode((const unsigned char *)members, points_len, NULL);
  char *mask = g_strdup_printf("<rdf:li darktable:mask_num=\"%d\" darktable:mask_id=\"%d\" "
                               "darktable:mask_type=\"%d\" darktable:mask_name=\"group\" "
                               "darktable:mask_version=\"%d\" darktable:mask_points=\"%s\" "
                               "darktable:mask_nb=\"%d\" darktable:mask_src=\"\"/>",
                               mask_num, mask_id, DT_MASKS_GROUP, DEVELOP_MASKS_VERSION, points, member_count);
  dt_free(points);
  return mask;
}

typedef struct history_blend_row_t
{
  int count;
  char operations[3][32];
  gboolean enabled[3];
  int versions[3];
  int lengths[3];
  unsigned char bytes[3][sizeof(dt_develop_blend_params_t)];
} history_blend_row_t;

static void collect_history_blend_row(void *user_data, const dt_history_repository_row_t *history)
{
  history_blend_row_t *row = (history_blend_row_t *)user_data;
  if(row->count < 0 || row->count >= (int)G_N_ELEMENTS(row->bytes) || history->blendop_params_len < 0
     || history->blendop_params_len > (int)sizeof(row->bytes[0]))
  {
    fail();
    return;
  }
  g_strlcpy(row->operations[row->count], history->operation, sizeof(row->operations[0]));
  row->enabled[row->count] = history->enabled;
  row->versions[row->count] = history->blendop_version;
  row->lengths[row->count] = history->blendop_params_len;
  memcpy(row->bytes[row->count], history->blendop_params, history->blendop_params_len);
  row->count++;
}

static void test_excluded_descriptive_metadata_survives_full_xmp_read(void **state)
{
  (void)state;
  dt_image_t image = make_image("metadata.raw");
  const char *key = dt_metadata_get_key(DT_METADATA_XMP_DC_TITLE);
  const char *name = dt_metadata_get_name(DT_METADATA_XMP_DC_TITLE);
  char *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", name);

  dt_metadata_set(image.id, key, "keep", FALSE);
  dt_conf_set_int(setting, 0);
  write_xmp(1, "<dc:title><rdf:Alt><rdf:li xml:lang=\"x-default\">replace</rdf:li></rdf:Alt></dc:title>");

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_OK);
  GList *values = dt_metadata_get(image.id, key, NULL);
  assert_non_null(values);
  assert_string_equal(values->data, "keep");

  g_list_free_full(values, dt_free_gpointer);
  dt_free(setting);
}

static void test_unsupported_xmp_schema_imports_metadata_without_replacing_development(void **state)
{
  (void)state;
  dt_image_t image = make_image("unsupported.raw");
  seed_development(image.id);
  const GTimeSpan timestamp = image.change_timestamp;
  const char *key = dt_metadata_get_key(DT_METADATA_XMP_DC_DESCRIPTION);
  const char *name = dt_metadata_get_name(DT_METADATA_XMP_DC_DESCRIPTION);
  char *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", name);
  dt_conf_set_int(setting, DT_METADATA_FLAG_IMPORTED);
  write_xmp(99, "<darktable:change_timestamp>1</darktable:change_timestamp>"
                "<dc:description><rdf:Alt><rdf:li xml:lang=\"x-default\">newer metadata</rdf:li>"
                "</rdf:Alt></dc:description>");
  gboolean tags_changed = FALSE;
  gboolean development_changed = TRUE;

  assert_int_equal(dt_exif_xmp_read_with_result(&image, xmp_path, FALSE, NULL, &tags_changed,
                                                 &development_changed), DT_XMP_IMPORT_PARTIAL);
  assert_false(development_changed);
  assert_development_preserved(image.id);
  assert_int_equal(image.change_timestamp, timestamp);
  GList *values = dt_metadata_get(image.id, key, NULL);
  assert_non_null(values);
  assert_string_equal(values->data, "newer metadata");
  g_list_free_full(values, dt_free_gpointer);
  dt_free(setting);
}

typedef struct mask_snapshot_row_t
{
  int count;
  int nums[4];
  int ids[4];
  int forms[4];
  int points_lengths[4];
} mask_snapshot_row_t;

static void collect_mask_snapshot_row(void *user_data, const dt_history_repository_mask_row_t *row)
{
  mask_snapshot_row_t *rows = user_data;
  if(rows->count < 0 || rows->count >= (int)G_N_ELEMENTS(rows->nums))
  {
    fail();
    return;
  }
  rows->nums[rows->count] = row->num;
  rows->ids[rows->count] = row->mask_id;
  rows->forms[rows->count] = row->form;
  rows->points_lengths[rows->count] = row->points_len;
  rows->count++;
}

static void assert_partial_mask_snapshot_recovery(dt_image_t *image)
{
  assert_int_equal(dt_exif_xmp_read(image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_PARTIAL);
  assert_int_equal(dt_history_repository_count_items(image->id), 1);
  mask_snapshot_row_t snapshots = { 0 };
  dt_history_repository_foreach_mask_item(image->id, collect_mask_snapshot_row, &snapshots);
  assert_int_equal(snapshots.count, 2);
  assert_int_equal(snapshots.nums[0], 0);
  assert_int_equal(snapshots.ids[0], 0);
  assert_int_equal(snapshots.forms[0], 0);
  assert_int_equal(snapshots.points_lengths[0], 0);
  assert_int_equal(snapshots.nums[1], 1);
  assert_int_equal(snapshots.ids[1], 10);
}

static void test_v3_damaged_snapshot_preserves_empty_replay_boundary_through_export(void **state)
{
  (void)state;
  const dt_masks_node_circle_t first_point = { .center = { 0.25f, 0.50f }, .radius = 0.10f, .border = 0.20f };
  const dt_masks_node_circle_t third_point = { .center = { 0.75f, 0.50f }, .radius = 0.15f, .border = 0.30f };
  const float first_source[] = { 0.10f, 0.20f };
  const float third_source[] = { 0.80f, 0.90f };
  dt_develop_blend_params_t first_blend = { 0 };
  dt_develop_blend_params_t second_blend = { 0 };
  dt_develop_blend_params_t third_blend = { 0 };
  first_blend.mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_SHAPE;
  second_blend.mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_SHAPE;
  third_blend.mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_SHAPE;
  first_blend.mask_id = 1;
  second_blend.mask_id = 1;
  third_blend.mask_id = 2;
  dt_image_t image = make_image("empty-snapshot-boundary.raw");
  char *first_history = make_v3_history_step_with_blend(0, DEVELOP_BLEND_VERSION,
                                                         (const unsigned char *)&first_blend, sizeof(first_blend));
  char *second_history = make_v3_history_step_with_blend(1, DEVELOP_BLEND_VERSION,
                                                          (const unsigned char *)&second_blend, sizeof(second_blend));
  char *third_history = make_v3_history_step_with_blend(2, DEVELOP_BLEND_VERSION,
                                                         (const unsigned char *)&third_blend, sizeof(third_blend));
  char *first_mask = make_v3_circle_mask("darktable:mask_num=\"0\" darktable:mask_id=\"1\"", "first",
                                          &first_point, first_source);
  char *damaged_mask = g_strdup("<rdf:li darktable:mask_num=\"1\" darktable:mask_id=\"1\" "
                                "darktable:mask_type=\"4\" darktable:mask_name=\"broken\" "
                                "darktable:mask_version=\"1\" darktable:mask_points=\"00\" "
                                "darktable:mask_nb=\"1\" darktable:mask_src=\"\"/>");
  char *third_mask = make_v3_circle_mask("darktable:mask_num=\"2\" darktable:mask_id=\"2\"", "third",
                                          &third_point, third_source);
  char *properties = g_strdup_printf("<darktable:history><rdf:Seq>%s%s%s</rdf:Seq></darktable:history>"
                                     "<darktable:masks_history><rdf:Seq>%s%s%s</rdf:Seq></darktable:masks_history>",
                                     first_history, second_history, third_history, first_mask, damaged_mask, third_mask);
  write_xmp(3, properties);
  gboolean development_changed = FALSE;

  assert_int_equal(dt_exif_xmp_read_with_result(&image, xmp_path, FALSE, NULL, NULL, &development_changed),
                   DT_XMP_IMPORT_PARTIAL);
  assert_true(development_changed);
  mask_snapshot_row_t imported = { 0 };
  dt_history_repository_foreach_mask_item(image.id, collect_mask_snapshot_row, &imported);
  assert_int_equal(imported.count, 3);
  assert_int_equal(imported.nums[0], 0);
  assert_int_equal(imported.ids[0], 1);
  assert_int_equal(imported.nums[1], 1);
  assert_int_equal(imported.ids[1], 0);
  assert_int_equal(imported.forms[1], 0);
  assert_int_equal(imported.points_lengths[1], 0);
  assert_int_equal(imported.nums[2], 2);
  assert_int_equal(imported.ids[2], 2);
  history_blend_row_t history = { 0 };
  dt_history_repository_foreach_row(image.id, collect_history_blend_row, &history);
  assert_true(history.enabled[0]);
  assert_false(history.enabled[1]);
  assert_true(history.enabled[2]);
  assert_memory_equal(history.bytes[0], &first_blend, sizeof(first_blend));
  assert_memory_equal(history.bytes[1], &second_blend, sizeof(second_blend));
  assert_memory_equal(history.bytes[2], &third_blend, sizeof(third_blend));

  char *packet = dt_exif_xmp_read_string(image.id);
  assert_non_null(packet);
  assert_true(g_file_set_contents(xmp_path, packet, -1, NULL));
  dt_free(packet);
  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_OK);
  mask_snapshot_row_t reimported = { 0 };
  dt_history_repository_foreach_mask_item(image.id, collect_mask_snapshot_row, &reimported);
  assert_int_equal(reimported.count, 3);
  assert_int_equal(reimported.nums[1], 1);
  assert_int_equal(reimported.ids[1], 0);
  assert_int_equal(reimported.forms[1], 0);

  dt_free(properties);
  dt_free(third_mask);
  dt_free(damaged_mask);
  dt_free(first_mask);
  dt_free(third_history);
  dt_free(second_history);
  dt_free(first_history);
}

static void test_malformed_v2_history_preserves_development(void **state)
{
  (void)state;
  dt_image_t image = make_image("malformed-history.raw");
  seed_development(image.id);
  write_xmp(2, "<darktable:history><rdf:Seq><rdf:li darktable:operation=\"exposure\" "
               "darktable:enabled=\"1\" darktable:modversion=\"1\"/>"
                "</rdf:Seq></darktable:history>");

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_READ_ERROR);
  assert_development_preserved(image.id);
}

static void test_short_history_operation_preserves_development(void **state)
{
  (void)state;
  dt_image_t image = make_image("short-operation.raw");
  seed_development(image.id);
  write_xmp(2, "<darktable:history><rdf:Seq><rdf:li darktable:num=\"0\" darktable:operation=\"x\" "
               "darktable:enabled=\"1\" darktable:modversion=\"1\" darktable:params=\"0000\"/>"
               "</rdf:Seq></darktable:history>");

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_READ_ERROR);
  assert_development_preserved(image.id);
}

static void test_v1_optional_priorities_preserve_history_instances(void **state G_GNUC_UNUSED)
{
  dt_image_t image = make_image("v1-priorities.raw");
  const struct
  {
    const char *priorities;
    int expected[2];
  } cases[] = {
    { "", { 0, 0 } },
    { "<darktable:multi_priority><rdf:Seq><rdf:li>2</rdf:li></rdf:Seq></darktable:multi_priority>",
      { 2, 0 } },
    { "<darktable:multi_priority><rdf:Seq><rdf:li>2</rdf:li><rdf:li>1</rdf:li>"
      "</rdf:Seq></darktable:multi_priority>", { 2, 1 } }
  };

  for(size_t i = 0; i < G_N_ELEMENTS(cases); i++)
  {
    char *properties = g_strconcat(
      "<darktable:history_operation><rdf:Seq><rdf:li>exposure</rdf:li><rdf:li>exposure</rdf:li>"
      "</rdf:Seq></darktable:history_operation>"
      "<darktable:history_modversion><rdf:Seq><rdf:li>1</rdf:li><rdf:li>1</rdf:li>"
      "</rdf:Seq></darktable:history_modversion>"
      "<darktable:history_enabled><rdf:Seq><rdf:li>1</rdf:li><rdf:li>1</rdf:li>"
      "</rdf:Seq></darktable:history_enabled>"
      "<darktable:history_params><rdf:Seq><rdf:li>0000</rdf:li><rdf:li>0100</rdf:li>"
      "</rdf:Seq></darktable:history_params>", cases[i].priorities, NULL);
    write_xmp(1, properties);
    dt_free(properties);

    assert_int_equal(dt_exif_xmp_read(&image, xmp_path, TRUE, NULL), DT_XMP_IMPORT_OK);
    sqlite3_stmt *stmt = NULL;
    assert_int_equal(sqlite3_prepare_v2(dt_database_get_sqlite3_global(),
                                       "SELECT multi_priority FROM main.history WHERE imgid = ?1 ORDER BY num",
                                       -1, &stmt, NULL), SQLITE_OK);
    assert_int_equal(sqlite3_bind_int(stmt, 1, image.id), SQLITE_OK);
    for(size_t row = 0; row < G_N_ELEMENTS(cases[i].expected); row++)
    {
      assert_int_equal(sqlite3_step(stmt), SQLITE_ROW);
      assert_int_equal(sqlite3_column_int(stmt, 0), cases[i].expected[row]);
    }
    assert_int_equal(sqlite3_step(stmt), SQLITE_DONE);
    assert_int_equal(sqlite3_finalize(stmt), SQLITE_OK);
  }
}

static void test_partial_v1_legacy_mask_properties_preserve_development(void **state)
{
  (void)state;
  dt_image_t image = make_image("partial-v1-mask.raw");
  seed_development(image.id);
  write_xmp(1, "<darktable:mask><rdf:Seq><rdf:li>AA==</rdf:li></rdf:Seq></darktable:mask>");

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_READ_ERROR);
  assert_development_preserved(image.id);
}

static void test_partial_v2_legacy_mask_properties_preserve_development(void **state)
{
  (void)state;
  dt_image_t image = make_image("partial-v2-mask.raw");
  seed_development(image.id);
  write_xmp(2, "<darktable:mask><rdf:Seq><rdf:li>AA==</rdf:li></rdf:Seq></darktable:mask>");

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_READ_ERROR);
  assert_development_preserved(image.id);
}

static void test_duplicate_legacy_mask_ids_preserve_development(void **state)
{
  (void)state;
  dt_image_t image = make_image("duplicate-mask-id.raw");
  seed_development(image.id);
  write_xmp(2, "<darktable:mask><rdf:Seq><rdf:li>AA==</rdf:li><rdf:li>AA==</rdf:li></rdf:Seq></darktable:mask>"
               "<darktable:mask_src><rdf:Seq><rdf:li>AA==</rdf:li><rdf:li>AA==</rdf:li></rdf:Seq></darktable:mask_src>"
               "<darktable:mask_name><rdf:Seq><rdf:li>one</rdf:li><rdf:li>two</rdf:li></rdf:Seq></darktable:mask_name>"
               "<darktable:mask_type><rdf:Seq><rdf:li>0</rdf:li><rdf:li>0</rdf:li></rdf:Seq></darktable:mask_type>"
               "<darktable:mask_version><rdf:Seq><rdf:li>1</rdf:li><rdf:li>1</rdf:li></rdf:Seq></darktable:mask_version>"
               "<darktable:mask_id><rdf:Seq><rdf:li>42</rdf:li><rdf:li>42</rdf:li></rdf:Seq></darktable:mask_id>"
               "<darktable:mask_nb><rdf:Seq><rdf:li>1</rdf:li><rdf:li>1</rdf:li></rdf:Seq></darktable:mask_nb>");

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_READ_ERROR);
  assert_development_preserved(image.id);
}

static void test_v3_masks_reuse_formid_across_history_steps(void **state)
{
  (void)state;
  dt_image_t image = make_image("v3-mask-formid-per-step.raw");
  const dt_masks_node_circle_t first_point = { .center = { 0.25f, 0.50f }, .radius = 0.10f, .border = 0.20f };
  const dt_masks_node_circle_t second_point = { .center = { 0.75f, 0.50f }, .radius = 0.15f, .border = 0.30f };
  const float first_source[] = { 0.10f, 0.20f };
  const float second_source[] = { 0.80f, 0.90f };
  char *first_history = make_v3_history_step(0);
  char *second_history = make_v3_history_step(1);
  char *first_mask = make_v3_circle_mask("darktable:mask_num=\"0\" darktable:mask_id=\"1\"", "first",
                                          &first_point, first_source);
  char *second_mask = make_v3_circle_mask("darktable:mask_num=\"1\" darktable:mask_id=\"1\"", "second",
                                           &second_point, second_source);
  char *properties = g_strdup_printf("<darktable:history><rdf:Seq>%s%s</rdf:Seq></darktable:history>"
                                     "<darktable:masks_history><rdf:Seq>%s%s</rdf:Seq></darktable:masks_history>",
                                     first_history, second_history, first_mask, second_mask);
  write_xmp(3, properties);

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_OK);
  assert_int_equal(dt_history_repository_count_items(image.id), 2);
  assert_int_equal(dt_history_repository_count_mask_items(image.id), 2);
  char *packet = dt_exif_xmp_read_string(image.id);
  assert_non_null(packet);
  assert_true(g_file_set_contents(xmp_path, packet, -1, NULL));
  dt_free(packet);
  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_OK);
  assert_int_equal(dt_history_repository_count_items(image.id), 2);
  mask_history_rows_t rows = { 0 };
  dt_history_repository_foreach_mask_item(image.id, collect_mask_history_rows, &rows);
  assert_int_equal(rows.count, 2);
  assert_int_equal(rows.nums[0], 0);
  assert_int_equal(rows.formids[0], 1);
  assert_int_equal(rows.types[0], DT_MASKS_CIRCLE);
  assert_int_equal(rows.versions[0], DEVELOP_MASKS_VERSION);
  assert_int_equal(rows.point_counts[0], 1);
  assert_int_equal(rows.nums[1], 1);
  assert_int_equal(rows.formids[1], 1);
  assert_int_equal(rows.types[1], DT_MASKS_CIRCLE);
  assert_int_equal(rows.versions[1], DEVELOP_MASKS_VERSION);
  assert_int_equal(rows.point_counts[1], 1);
  assert_string_equal(rows.names[0], "first");
  assert_memory_equal(&rows.points[0], &first_point, sizeof(first_point));
  assert_memory_equal(rows.sources[0], first_source, sizeof(first_source));
  assert_string_equal(rows.names[1], "second");
  assert_memory_equal(&rows.points[1], &second_point, sizeof(second_point));
  assert_memory_equal(rows.sources[1], second_source, sizeof(second_source));

  dt_free(properties);
  dt_free(second_mask);
  dt_free(first_mask);
  dt_free(second_history);
  dt_free(first_history);
}

static void test_invalid_v3_mask_properties_omit_only_affected_snapshot(void **state)
{
  (void)state;
  const struct
  {
    const char *attributes;
    int expected_num;
  } cases[] = { { "darktable:mask_num=\"-1\" darktable:mask_id=\"1\"", 0 },
                { "darktable:mask_num=\"2147483648\" darktable:mask_id=\"1\"", 0 },
                { "darktable:mask_num=\"3\" darktable:mask_id=\"-1\"", 3 },
                { "darktable:mask_num=\"3\" darktable:mask_id=\"0\"", 3 },
                { "darktable:mask_num=\"3\" darktable:mask_id=\"2147483648\"", 3 } };
  const dt_masks_node_circle_t point = { .center = { 0.25f, 0.50f }, .radius = 0.10f, .border = 0.20f };
  const float source[] = { 0.10f, 0.20f };

  for(size_t i = 0; i < G_N_ELEMENTS(cases); i++)
  {
    char *filename = g_strdup_printf("out-of-range-v3-mask-%" G_GSIZE_FORMAT ".raw", i);
    dt_image_t image = make_image(filename);
    char *history = make_v3_history_step(0);
    char *mask = make_v3_circle_mask(cases[i].attributes, "mask", &point, source);
    char *properties = g_strdup_printf("<darktable:history><rdf:Seq>%s</rdf:Seq></darktable:history>"
                                       "<darktable:masks_history><rdf:Seq>%s</rdf:Seq></darktable:masks_history>",
                                       history, mask);
    write_xmp(3, properties);

    char *diagnostic = capture_xmp_diagnostic(&image, DT_XMP_IMPORT_PARTIAL);
    assert_non_null(strstr(diagnostic, "recovered"));
    assert_null(strstr(diagnostic, "rejected"));
    dt_free(diagnostic);
    assert_int_equal(dt_history_repository_count_items(image.id), 1);
    mask_snapshot_row_t snapshots = { 0 };
    dt_history_repository_foreach_mask_item(image.id, collect_mask_snapshot_row, &snapshots);
    assert_int_equal(snapshots.count, 1);
    assert_int_equal(snapshots.nums[0], cases[i].expected_num);
    assert_int_equal(snapshots.ids[0], 0);
    assert_int_equal(snapshots.forms[0], 0);
    assert_int_equal(snapshots.points_lengths[0], 0);
    dt_free(properties);
    dt_free(mask);
    dt_free(history);
    dt_free(filename);
  }
}

static void test_duplicate_v3_mask_ids_omit_only_affected_snapshot(void **state)
{
  (void)state;
  dt_image_t image = make_image("duplicate-v3-mask-id.raw");
  const dt_masks_node_circle_t first_point = { .center = { 0.25f, 0.50f }, .radius = 0.10f, .border = 0.20f };
  const dt_masks_node_circle_t second_point = { .center = { 0.75f, 0.50f }, .radius = 0.15f, .border = 0.30f };
  const float first_source[] = { 0.10f, 0.20f };
  const float second_source[] = { 0.80f, 0.90f };
  char *history = make_v3_history_step(0);
  char *first_mask = make_v3_circle_mask("darktable:mask_num=\"0\" darktable:mask_id=\"1\"", "first",
                                          &first_point, first_source);
  char *second_mask = make_v3_circle_mask("darktable:mask_num=\"0\" darktable:mask_id=\"1\"", "second",
                                           &second_point, second_source);
  char *properties = g_strdup_printf("<darktable:history><rdf:Seq>%s</rdf:Seq></darktable:history>"
                                     "<darktable:masks_history><rdf:Seq>%s%s</rdf:Seq></darktable:masks_history>",
                                     history, first_mask, second_mask);
  write_xmp(3, properties);

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_PARTIAL);
  assert_int_equal(dt_history_repository_count_items(image.id), 1);
  mask_snapshot_row_t snapshots = { 0 };
  dt_history_repository_foreach_mask_item(image.id, collect_mask_snapshot_row, &snapshots);
  assert_int_equal(snapshots.count, 1);
  assert_int_equal(snapshots.nums[0], 0);
  assert_int_equal(snapshots.ids[0], 0);
  assert_int_equal(snapshots.forms[0], 0);
  assert_int_equal(snapshots.points_lengths[0], 0);

  dt_free(properties);
  dt_free(second_mask);
  dt_free(first_mask);
  dt_free(history);
}

static void test_malformed_history_restores_cached_raw_parameters_and_flags(void **state)
{
  (void)state;
  dt_image_t image = make_image("cached-image.raw");
  dt_image_t *cached = dt_image_cache_get(image.id, 'w');
  cached->legacy_flip.legacy = 0;
  cached->legacy_flip.user_flip = 3;
  cached->flags |= DT_IMAGE_REJECTED;
  const uint32_t flags = cached->flags;
  dt_image_cache_write_release(cached, DT_IMAGE_CACHE_SAFE);
  write_xmp(2, "<darktable:raw_params>117440512</darktable:raw_params>"
               "<darktable:auto_presets_applied>1</darktable:auto_presets_applied>"
               "<darktable:history><rdf:Seq><rdf:li darktable:operation=\"exposure\"/>"
               "</rdf:Seq></darktable:history>");

  assert_int_equal(dt_history_load_and_apply(image.id, xmp_path, TRUE), 1);
  cached = dt_image_cache_get(image.id, 'r');
  assert_int_equal(cached->legacy_flip.user_flip, 3);
  assert_int_equal(cached->flags, flags);
  dt_image_cache_read_release(cached);

  dt_image_t persisted;
  dt_image_init(&persisted);
  assert_true(dt_image_repository_load(image.id, &persisted));
  assert_int_equal(persisted.legacy_flip.user_flip, 3);
  assert_int_equal(persisted.flags, flags);
}

static void test_legacy_raw_parameters_decode_flip_byte(void **state G_GNUC_UNUSED)
{
  const char *serialized[] = { "16777215", "100663297", "-1" };
  const unsigned int expected[] = { 0, 6, 255 };
  dt_image_t image = make_image("legacy-flip.raw");

  for(guint i = 0; i < G_N_ELEMENTS(serialized); i++)
  {
    image.legacy_flip.legacy = 42;
    image.legacy_flip.user_flip = 3;
    char *properties = g_strdup_printf("<darktable:raw_params>%s</darktable:raw_params>"
                                        "<darktable:history><rdf:Seq/></darktable:history>", serialized[i]);
    write_xmp(2, properties);
    dt_free(properties);

    assert_int_equal(dt_exif_xmp_read(&image, xmp_path, TRUE, NULL), DT_XMP_IMPORT_OK);
    assert_int_equal(image.legacy_flip.user_flip, expected[i]);
    assert_int_equal(image.legacy_flip.legacy, 0);
  }
}

static void test_empty_valid_history_is_accepted(void **state)
{
  (void)state;
  dt_image_t image = make_image("empty-history.raw");
  seed_development(image.id);
  write_xmp(2, "<darktable:history><rdf:Seq/></darktable:history>");

  char *diagnostic = capture_xmp_diagnostic(&image, DT_XMP_IMPORT_OK);
  assert_null(strstr(diagnostic, "rejected"));
  dt_free(diagnostic);
  assert_int_equal(dt_history_repository_count_items(image.id), 0);
  assert_int_equal(dt_history_repository_count_mask_items(image.id), 0);
}

static void test_v1_sequence_history_is_accepted(void **state)
{
  (void)state;
  dt_image_t image = make_image("v1-sequence.raw");
  write_xmp(1, "<darktable:history_modversion><rdf:Seq><rdf:li>1</rdf:li></rdf:Seq></darktable:history_modversion>"
               "<darktable:history_enabled><rdf:Seq><rdf:li>1</rdf:li></rdf:Seq></darktable:history_enabled>"
               "<darktable:history_operation><rdf:Seq><rdf:li>exposure</rdf:li></rdf:Seq></darktable:history_operation>"
                "<darktable:history_params><rdf:Seq><rdf:li>00000000</rdf:li></rdf:Seq></darktable:history_params>");

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_OK);
  assert_int_equal(dt_history_repository_count_items(image.id), 1);
}

static void test_superold_bag_history_is_accepted(void **state)
{
  (void)state;
  dt_image_t image = make_image("superold-bag.raw");
  write_xmp(0, "<darktable:history_modversion><rdf:Bag><rdf:li>1</rdf:li></rdf:Bag></darktable:history_modversion>"
               "<darktable:history_enabled><rdf:Bag><rdf:li>1</rdf:li></rdf:Bag></darktable:history_enabled>"
               "<darktable:history_operation><rdf:Bag><rdf:li>exposure</rdf:li></rdf:Bag></darktable:history_operation>"
                "<darktable:history_params><rdf:Bag><rdf:li>00000000</rdf:li></rdf:Bag></darktable:history_params>");

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_OK);
  assert_int_equal(dt_history_repository_count_items(image.id), 1);
}

static void test_out_of_range_rating_preserves_existing_rating(void **state)
{
  (void)state;
  dt_image_t image = make_image("rating.raw");
  dt_image_set_xmp_rating(&image, 3);
  write_xmp(0, "<xmp:Rating>6</xmp:Rating>");

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_PARTIAL);
  assert_int_equal(dt_image_get_xmp_rating(&image), 3);
}

static void test_absent_rating_clears_xmp_rating_flags(void **state)
{
  (void)state;
  dt_image_t image = make_image("absent-rating.raw");
  dt_image_set_xmp_rating(&image, 3);
  write_xmp(0, "");

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_OK);
  assert_int_equal(dt_image_get_xmp_rating(&image), 0);
}

static void test_valid_rating_import_updates_rating(void **state)
{
  (void)state;
  dt_image_t image = make_image("valid-rating.raw");
  dt_image_set_xmp_rating(&image, 1);
  write_xmp(2, "<xmp:Rating>4</xmp:Rating>");

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_OK);
  assert_int_equal(dt_image_get_xmp_rating(&image), 4);
}

static void test_malformed_nonempty_history_blendop_blob_preserves_development(void **state)
{
  (void)state;
  dt_image_t image = make_image("malformed-blendop.raw");
  seed_development(image.id);
  write_xmp(2, "<darktable:history><rdf:Seq><rdf:li darktable:num=\"0\" darktable:operation=\"exposure\" "
               "darktable:enabled=\"1\" darktable:modversion=\"1\" darktable:params=\"00000000\" "
               "darktable:blendop_params=\"not-hex\"/></rdf:Seq></darktable:history>");

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_READ_ERROR);
  assert_development_preserved(image.id);
}

static void test_rejected_xmp_reports_one_stable_diagnostic(void **state)
{
  (void)state;
  dt_image_t history_image = make_image("diagnostic-history.raw");
  seed_development(history_image.id);
  write_xmp(3, "<darktable:history><rdf:Seq><rdf:li darktable:operation=\"exposure\" "
               "darktable:enabled=\"1\" darktable:modversion=\"1\" darktable:params=\"0000\"/>"
               "</rdf:Seq></darktable:history>");
  assert_stable_xmp_rejection(&history_image, "history-number");
  assert_development_preserved(history_image.id);

  dt_image_t blend_image = make_image("diagnostic-blend.raw");
  seed_development(blend_image.id);
  write_xmp(2, "<darktable:history><rdf:Seq><rdf:li darktable:num=\"0\" darktable:operation=\"exposure\" "
               "darktable:enabled=\"1\" darktable:modversion=\"1\" darktable:params=\"0000\" "
               "darktable:blendop_params=\"not-hex\"/></rdf:Seq></darktable:history>");
  assert_stable_xmp_rejection(&blend_image, "blend-payload");
  assert_development_preserved(blend_image.id);

  dt_image_t mask_image = make_image("diagnostic-mask.raw");
  seed_development(mask_image.id);
  write_xmp(2, "<darktable:mask><rdf:Seq><rdf:li>not-hex</rdf:li></rdf:Seq></darktable:mask>"
               "<darktable:mask_src><rdf:Seq><rdf:li/></rdf:Seq></darktable:mask_src>"
               "<darktable:mask_name><rdf:Seq><rdf:li>mask</rdf:li></rdf:Seq></darktable:mask_name>"
               "<darktable:mask_type><rdf:Seq><rdf:li>0</rdf:li></rdf:Seq></darktable:mask_type>"
               "<darktable:mask_version><rdf:Seq><rdf:li>1</rdf:li></rdf:Seq></darktable:mask_version>"
               "<darktable:mask_id><rdf:Seq><rdf:li>42</rdf:li></rdf:Seq></darktable:mask_id>"
               "<darktable:mask_nb><rdf:Seq><rdf:li>0</rdf:li></rdf:Seq></darktable:mask_nb>");
  assert_stable_xmp_rejection(&mask_image, "mask-payload");
  assert_development_preserved(mask_image.id);

  dt_image_t scalar_image = make_image("diagnostic-scalar.raw");
  seed_development(scalar_image.id);
  write_xmp(2, "<xmp:Rating>6</xmp:Rating>");
  char *diagnostic = capture_xmp_diagnostic(&scalar_image, DT_XMP_IMPORT_PARTIAL);
  assert_null(strstr(diagnostic, "rejected"));
  dt_free(diagnostic);
  assert_int_equal(dt_history_repository_count_items(scalar_image.id), 0);
  assert_int_equal(dt_history_repository_count_mask_items(scalar_image.id), 0);
}

static void test_metadata_only_import_does_not_notify_tags(void **state)
{
  (void)state;
  dt_image_t image = make_image("metadata-notification.raw");
  const char *key = dt_metadata_get_key(DT_METADATA_XMP_DC_DESCRIPTION);
  const char *name = dt_metadata_get_name(DT_METADATA_XMP_DC_DESCRIPTION);
  char *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", name);
  dt_conf_set_int(setting, DT_METADATA_FLAG_IMPORTED);
  dt_metadata_set(image.id, key, "before", FALSE);
  tags_changed_count = 0;
  dt_metadata_set_tags_changed_handler(record_tags_changed);
  write_xmp(2, "<dc:description><rdf:Alt><rdf:li xml:lang=\"x-default\">after</rdf:li>"
               "</rdf:Alt></dc:description>");
  gboolean tags_changed = TRUE;

  assert_int_equal(dt_exif_xmp_read_with_tags(&image, xmp_path, FALSE, NULL, &tags_changed), DT_XMP_IMPORT_OK);
  assert_false(tags_changed);
  assert_int_equal(tags_changed_count, 0);
  GList *values = dt_metadata_get(image.id, key, NULL);
  assert_non_null(values);
  assert_string_equal(values->data, "after");
  g_list_free_full(values, dt_free_gpointer);
  dt_metadata_set_tags_changed_handler(NULL);
  dt_free(setting);
}

static void assert_history_numbers(const int32_t imgid, const int *numbers, const size_t count)
{
  sqlite3_stmt *statement = NULL;
  assert_int_equal(sqlite3_prepare_v2(dt_database_get_sqlite3_global(),
                                      "SELECT num FROM main.history WHERE imgid = ?1 ORDER BY num", -1,
                                      &statement, NULL), SQLITE_OK);
  assert_int_equal(sqlite3_bind_int(statement, 1, imgid), SQLITE_OK);
  for(size_t index = 0; index < count; index++)
  {
    assert_int_equal(sqlite3_step(statement), SQLITE_ROW);
    assert_int_equal(sqlite3_column_int(statement, 0), numbers[index]);
  }
  assert_int_equal(sqlite3_step(statement), SQLITE_DONE);
  assert_int_equal(sqlite3_finalize(statement), SQLITE_OK);
}

static void test_v2_optional_history_numbers_use_sequence_order(void **state)
{
  (void)state;
  static const struct
  {
    const char *name;
    const char *first_number;
    const char *second_number;
  } cases[] = {
    { "absent", "", "" },
    { "duplicate", "darktable:num=\"0\"", "darktable:num=\"0\"" },
    { "negative", "darktable:num=\"-1\"", "darktable:num=\"-2\"" },
    { "overflow", "darktable:num=\"2147483648\"", "darktable:num=\"2147483649\"" },
    { "nonnumeric", "darktable:num=\"first\"", "darktable:num=\"second\"" },
  };
  static const int expected_numbers[] = { 0, 1 };

  for(size_t index = 0; index < G_N_ELEMENTS(cases); index++)
  {
    char *filename = g_strdup_printf("v2-%s-history-numbers.raw", cases[index].name);
    dt_image_t image = make_image(filename);
    seed_development(image.id);
    char *history = g_strdup_printf(
        "<darktable:history><rdf:Seq>"
        "<rdf:li %s darktable:operation=\"exposure\" darktable:enabled=\"1\" "
        "darktable:modversion=\"1\" darktable:params=\"0000\"/>"
        "<rdf:li %s darktable:operation=\"exposure\" darktable:enabled=\"1\" "
        "darktable:modversion=\"1\" darktable:params=\"0100\"/>"
        "</rdf:Seq></darktable:history>",
        cases[index].first_number, cases[index].second_number);
    write_xmp(2, history);

    assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_OK);
    assert_history_numbers(image.id, expected_numbers, G_N_ELEMENTS(expected_numbers));

    dt_free(history);
    dt_free(filename);
  }
}

static void assert_invalid_v3_history_number(const char *filename, const char *first_number,
                                             const char *second_number)
{
  dt_image_t image = make_image(filename);
  seed_development(image.id);
  char *history = g_strdup_printf(
      "<darktable:history><rdf:Seq>"
      "<rdf:li %s darktable:operation=\"exposure\" darktable:enabled=\"1\" "
      "darktable:modversion=\"1\" darktable:params=\"0000\"/>"
      "<rdf:li %s darktable:operation=\"exposure\" darktable:enabled=\"1\" "
      "darktable:modversion=\"1\" darktable:params=\"0100\"/>"
      "</rdf:Seq></darktable:history>",
      first_number, second_number);
  write_xmp(3, history);

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_READ_ERROR);
  assert_development_preserved(image.id);
  dt_free(history);
}

static void test_invalid_v3_history_numbers_preserve_development(void **state)
{
  (void)state;
  assert_invalid_v3_history_number("v3-missing-num.raw", "", "darktable:num=\"1\"");
  assert_invalid_v3_history_number("v3-duplicate-num.raw", "darktable:num=\"0\"", "darktable:num=\"0\"");
  assert_invalid_v3_history_number("v3-negative-num.raw", "darktable:num=\"-1\"", "darktable:num=\"1\"");
  assert_invalid_v3_history_number("v3-overflow-num.raw", "darktable:num=\"2147483648\"", "darktable:num=\"1\"");
  assert_invalid_v3_history_number("v3-nonnumeric-num.raw", "darktable:num=\"invalid\"", "darktable:num=\"1\"");
}

static void test_valid_v3_history_numbers_are_persisted(void **state)
{
  (void)state;
  static const int expected_numbers[] = { 0, 1 };
  dt_image_t image = make_image("v3-valid-history-numbers.raw");
  write_xmp(3, "<darktable:history><rdf:Seq>"
               "<rdf:li darktable:num=\"0\" darktable:operation=\"exposure\" darktable:enabled=\"1\" "
               "darktable:modversion=\"1\" darktable:params=\"0000\"/>"
               "<rdf:li darktable:num=\"1\" darktable:operation=\"exposure\" darktable:enabled=\"1\" "
               "darktable:modversion=\"1\" darktable:params=\"0100\"/>"
               "</rdf:Seq></darktable:history>");

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_OK);
  assert_history_numbers(image.id, expected_numbers, G_N_ELEMENTS(expected_numbers));
}

static void assert_v3_near_prefix_history_rejected(const char *filename, const char *history)
{
  dt_image_t image = make_image(filename);
  seed_development(image.id);
  write_xmp(3, history);

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_READ_ERROR);
  assert_development_preserved(image.id);
}

static void test_v3_near_prefix_history_properties_preserve_development(void **state)
{
  (void)state;
  assert_v3_near_prefix_history_rejected(
      "v3-near-num.raw", "<darktable:history><rdf:Seq>"
                         "<rdf:li darktable:number=\"0\" darktable:operation=\"exposure\" darktable:enabled=\"1\" "
                         "darktable:modversion=\"1\" darktable:params=\"0000\"/>"
                         "</rdf:Seq></darktable:history>");
  assert_v3_near_prefix_history_rejected(
      "v3-near-operation.raw", "<darktable:history><rdf:Seq>"
                               "<rdf:li darktable:num=\"0\" darktable:operation_suffix=\"exposure\" darktable:enabled=\"1\" "
                               "darktable:modversion=\"1\" darktable:params=\"0000\"/>"
                               "</rdf:Seq></darktable:history>");
  assert_v3_near_prefix_history_rejected(
      "v3-near-params.raw", "<darktable:history><rdf:Seq>"
                            "<rdf:li darktable:num=\"0\" darktable:operation=\"exposure\" darktable:enabled=\"1\" "
                            "darktable:modversion=\"1\" darktable:params_suffix=\"0000\"/>"
                            "</rdf:Seq></darktable:history>");
  assert_v3_near_prefix_history_rejected(
      "v3-near-modversion.raw", "<darktable:history><rdf:Seq>"
                                "<rdf:li darktable:num=\"0\" darktable:operation=\"exposure\" darktable:enabled=\"1\" "
                                "darktable:modversion_suffix=\"1\" darktable:params=\"0000\"/>"
                                "</rdf:Seq></darktable:history>");
}

static void test_short_v2_history_blendop_blob_is_preserved(void **state)
{
  (void)state;
  dt_image_t image = make_image("short-blendop.raw");
  write_xmp(2, "<darktable:history><rdf:Seq><rdf:li darktable:num=\"0\" darktable:operation=\"exposure\" "
               "darktable:enabled=\"1\" darktable:modversion=\"1\" darktable:params=\"0000\" "
               "darktable:blendop_params=\"0000\"/></rdf:Seq></darktable:history>");

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_OK);
  history_blend_row_t rows = { 0 };
  dt_history_repository_foreach_row(image.id, collect_history_blend_row, &rows);
  static const unsigned char expected[] = { 0, 0 };
  assert_int_equal(rows.count, 1);
  assert_memory_equal(rows.bytes[0], expected, sizeof(expected));
}

static void test_v1_history_blendop_blob_is_accepted(void **state)
{
  (void)state;
  dt_image_t image = make_image("v1-blendop.raw");
  write_xmp(2, "<darktable:history><rdf:Seq><rdf:li darktable:num=\"0\" darktable:operation=\"exposure\" "
               "darktable:enabled=\"1\" darktable:modversion=\"1\" darktable:params=\"0000\" "
               "darktable:blendop_params=\"000000000000000000000000\"/></rdf:Seq></darktable:history>");

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_OK);
  assert_int_equal(dt_history_repository_count_items(image.id), 1);
}

static void test_v7_history_blendop_blob_is_accepted(void **state)
{
  (void)state;
  dt_image_t image = make_image("v7-blendop.raw");
  unsigned char params[300] = { 0 };
  params[8] = 1;
  char *encoded = dt_exif_xmp_encode_internal(params, sizeof(params), NULL, FALSE);
  char *history = g_strdup_printf("<darktable:history><rdf:Seq><rdf:li darktable:num=\"0\" darktable:operation=\"exposure\" "
                                  "darktable:enabled=\"1\" darktable:modversion=\"1\" darktable:params=\"0000\" "
                                  "darktable:blendop_version=\"7\" darktable:blendop_params=\"%s\"/></rdf:Seq></darktable:history>",
                                  encoded);
  write_xmp(2, history);

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_OK);
  assert_int_equal(dt_history_repository_count_items(image.id), 1);
  dt_free(history);
  dt_free(encoded);
}

static void test_empty_xmp_decode_is_valid(void **state)
{
  (void)state;
  int length = -1;
  unsigned char *decoded = dt_exif_xmp_decode("", 0, &length);

  assert_non_null(decoded);
  assert_int_equal(length, 0);
  dt_free(decoded);
}

static void test_xmp_import_rejects_outer_transaction(void **state)
{
  (void)state;
  dt_image_t image = make_image("outer-transaction.raw");
  seed_development(image.id);
  write_xmp(2, "<darktable:history><rdf:Seq/></darktable:history>");

  assert_true(dt_database_start_transaction());
  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_PERSIST_ERROR);
  assert_true(dt_database_release_transaction());
  assert_development_preserved(image.id);
}

static void test_malformed_nonempty_legacy_mask_blob_preserves_development(void **state)
{
  (void)state;
  dt_image_t image = make_image("malformed-mask.raw");
  seed_development(image.id);
  write_xmp(2, "<darktable:mask><rdf:Seq><rdf:li>not-hex</rdf:li></rdf:Seq></darktable:mask>"
               "<darktable:mask_src><rdf:Seq><rdf:li/></rdf:Seq></darktable:mask_src>"
               "<darktable:mask_name><rdf:Seq><rdf:li>mask</rdf:li></rdf:Seq></darktable:mask_name>"
               "<darktable:mask_type><rdf:Seq><rdf:li>0</rdf:li></rdf:Seq></darktable:mask_type>"
               "<darktable:mask_version><rdf:Seq><rdf:li>1</rdf:li></rdf:Seq></darktable:mask_version>"
               "<darktable:mask_id><rdf:Seq><rdf:li>42</rdf:li></rdf:Seq></darktable:mask_id>"
               "<darktable:mask_nb><rdf:Seq><rdf:li>0</rdf:li></rdf:Seq></darktable:mask_nb>");

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_READ_ERROR);
  assert_development_preserved(image.id);
}

static void test_malformed_compressed_xmp_decode_returns_null(void **state)
{
  (void)state;
  const char *inputs[] = { "gz", "gz12eJ" };

  for(size_t i = 0; i < G_N_ELEMENTS(inputs); i++)
  {
    int decoded_len = 123;
    assert_null(dt_exif_xmp_decode(inputs[i], strlen(inputs[i]), &decoded_len));
    assert_int_equal(decoded_len, 123);
  }
}

static void test_failed_xmp_load_does_not_record_undo(void **state)
{
  (void)state;
  dt_image_t image = make_image("undo.raw");
  write_xmp(99, "");

  assert_int_equal(dt_history_load_and_apply(image.id, xmp_path, TRUE), 0);
  assert_false(dt_is_undo_list_populated(dt_undo_get_global(), DT_UNDO_LT_HISTORY));
}

static void test_missing_sidecar_returns_read_error_without_rejection_diagnostic(void **state)
{
  (void)state;
  dt_image_t image = make_image("missing-sidecar.raw");
  char *missing_path = g_strdup_printf("%s.missing", xmp_path);
  char *diagnostic_path = NULL;
  const int diagnostic_fd = g_file_open_tmp("ansel-test-xmp-missing-diagnostic-XXXXXX", &diagnostic_path, NULL);
  assert_true(diagnostic_fd >= 0);
  const int stderr_fd = test_dup(fileno(stderr));
  assert_true(stderr_fd >= 0);
  fflush(stderr);
  assert_int_equal(test_dup2(diagnostic_fd, fileno(stderr)), fileno(stderr));

  const dt_xmp_import_result_t result = dt_exif_xmp_read(&image, missing_path, FALSE, NULL);

  fflush(stderr);
  assert_int_equal(test_dup2(stderr_fd, fileno(stderr)), fileno(stderr));
  assert_int_equal(test_close(stderr_fd), 0);
  assert_int_equal(test_close(diagnostic_fd), 0);
  assert_int_equal(result, DT_XMP_IMPORT_READ_ERROR);
  char *diagnostic = NULL;
  assert_true(g_file_get_contents(diagnostic_path, &diagnostic, NULL, NULL));
  assert_string_equal(diagnostic, "");
  g_remove(diagnostic_path);
  dt_free(diagnostic);
  dt_free(diagnostic_path);
  dt_free(missing_path);
}

static const char full_development_xmp[] =
    "<darktable:history><rdf:Seq><rdf:li darktable:num=\"0\" darktable:operation=\"exposure\" "
    "darktable:enabled=\"1\" darktable:modversion=\"1\" darktable:params=\"00000000\"/>"
    "</rdf:Seq></darktable:history>"
    "<darktable:history_end>1</darktable:history_end>"
    "<darktable:auto_presets_applied>1</darktable:auto_presets_applied>"
    "<darktable:change_timestamp>1</darktable:change_timestamp>"
    "<darktable:export_timestamp>2</darktable:export_timestamp>"
    "<darktable:print_timestamp>3</darktable:print_timestamp>"
    "<darktable:history_current_hash>0102030405060708</darktable:history_current_hash>"
    "<xmp:Rating>4</xmp:Rating>"
    "<darktable:colorlabels><rdf:Seq><rdf:li>2</rdf:li></rdf:Seq></darktable:colorlabels>"
    "<dc:subject><rdf:Bag><rdf:li>imported</rdf:li></rdf:Bag></dc:subject>";

static void test_invalid_present_rating_and_timestamp_keep_previous_values_with_partial_history(void **state)
{
  (void)state;
  dt_image_t image = make_image("partial-scalars.raw");
  dt_image_set_xmp_rating(&image, 3);
  image.change_timestamp = 42;
  write_xmp(5, "<darktable:history><rdf:Seq><rdf:li darktable:num=\"0\" darktable:operation=\"exposure\" "
               "darktable:enabled=\"1\" darktable:modversion=\"1\" darktable:params=\"0000\"/>"
               "</rdf:Seq></darktable:history>"
               "<darktable:change_timestamp>not-a-timestamp</darktable:change_timestamp>"
               "<xmp:Rating>6</xmp:Rating>");

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_PARTIAL);
  assert_int_equal(dt_history_repository_count_items(image.id), 1);
  assert_int_equal(dt_image_get_xmp_rating(&image), 3);
  assert_int_equal(image.change_timestamp, 42);
}

#ifdef __linux__
typedef struct xmp_reader_t
{
  dt_image_t *image;
  const int64_t *write_timestamp;
  dt_xmp_import_result_t result;
} xmp_reader_t;

typedef struct xmp_writer_t
{
  xmp_transaction_barrier_t *barrier;
  int32_t imgid;
  int64_t timestamp;
  gboolean result;
} xmp_writer_t;

static void *run_xmp_reader(void *user_data)
{
  xmp_reader_t *reader = user_data;
  reader->result = dt_exif_xmp_read(reader->image, xmp_path, FALSE, reader->write_timestamp);
  return NULL;
}

static void *run_xmp_writer(void *user_data)
{
  xmp_writer_t *writer = user_data;
  pthread_mutex_lock(&writer->barrier->mutex);
  while(!writer->barrier->begin_complete) pthread_cond_wait(&writer->barrier->condition, &writer->barrier->mutex);
  writer->barrier->writer_ready = TRUE;
  record_xmp_transaction_event(writer->barrier, XMP_TRANSACTION_WRITER_READY);
  pthread_cond_broadcast(&writer->barrier->condition);
  pthread_mutex_unlock(&writer->barrier->mutex);

  writer->result = dt_image_repository_set_write_timestamp(writer->imgid, writer->timestamp);

  pthread_mutex_lock(&writer->barrier->mutex);
  writer->barrier->writer_complete = TRUE;
  record_xmp_transaction_event(writer->barrier, XMP_TRANSACTION_WRITER_COMPLETE);
  pthread_cond_broadcast(&writer->barrier->condition);
  pthread_mutex_unlock(&writer->barrier->mutex);
  return NULL;
}

static void assert_xmp_metadata(const int32_t imgid, const char *key, const char *value)
{
  GList *values = dt_metadata_get(imgid, key, NULL);
  assert_non_null(values);
  assert_string_equal(values->data, value);
  g_list_free_full(values, dt_free_gpointer);
}

typedef enum xmp_isolation_outcome_t
{
  XMP_ISOLATION_SUCCESS,
  XMP_ISOLATION_PARTIAL_FAILURE,
  XMP_ISOLATION_COMMIT_FAILURE,
} xmp_isolation_outcome_t;

static void run_xmp_isolation_case(const xmp_isolation_outcome_t outcome)
{
  const char *description_key = dt_metadata_get_key(DT_METADATA_XMP_DC_DESCRIPTION);
  const char *description_name = dt_metadata_get_name(DT_METADATA_XMP_DC_DESCRIPTION);
  char *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", description_name);
  char *properties = g_strconcat(full_development_xmp,
                                 "<dc:description><rdf:Alt><rdf:li xml:lang=\"x-default\">after</rdf:li>"
                                 "</rdf:Alt></dc:description>", NULL);
  char *filename = g_strdup_printf("xmp-isolation-%d.raw", outcome);
  char *writer_filename = g_strdup_printf("xmp-isolation-writer-%d.raw", outcome);
  dt_image_t image = make_image(filename);
  dt_image_t writer_image = make_image(writer_filename);
  const int64_t xmp_timestamp = 4102444800;
  const int64_t writer_timestamp = 4102444801;
  const int64_t original_timestamp = dt_image_repository_get_write_timestamp(image.id);
  const guint original_tag = dt_tag_repository_insert("xmp-isolation-original");
  const guint imported_tag = dt_tag_repository_insert("imported");
  dt_image_t original_image;
  assert_true(original_tag > 0);
  assert_true(imported_tag > 0);
  dt_conf_set_int(setting, DT_METADATA_FLAG_IMPORTED);
  dt_metadata_set(image.id, description_key, "before", FALSE);
  assert_true(dt_tag_repository_attach(original_tag, image.id));
  dt_image_set_xmp_rating(&image, 1);
  image.color_labels = 1;
  dt_colorlabel_repository_set(image.id, 0);
  seed_development(image.id);
  original_image = image;
  write_xmp(5, properties);

  xmp_transaction_barrier_t barrier = { .mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER,
                                        .fail_commit = outcome == XMP_ISOLATION_COMMIT_FAILURE,
                                        .fail_write_timestamp = outcome == XMP_ISOLATION_PARTIAL_FAILURE };
  xmp_reader_t reader = { .image = &image, .write_timestamp = &xmp_timestamp };
  xmp_writer_t writer = { .barrier = &barrier, .imgid = writer_image.id, .timestamp = writer_timestamp };
  pthread_t reader_thread;
  pthread_t writer_thread;
  char *diagnostic_path = NULL;
  int diagnostic_fd = -1;
  int stderr_fd = -1;
  if(outcome != XMP_ISOLATION_SUCCESS)
  {
    diagnostic_fd = g_file_open_tmp("ansel-test-xmp-isolation-diagnostic-XXXXXX", &diagnostic_path, NULL);
    assert_true(diagnostic_fd >= 0);
    if(diagnostic_fd < 0) return;
    stderr_fd = test_dup(fileno(stderr));
    assert_true(stderr_fd >= 0);
    if(stderr_fd < 0)
    {
      test_close(diagnostic_fd);
      g_remove(diagnostic_path);
      dt_free(diagnostic_path);
      return;
    }
    fflush(stderr);
    assert_int_equal(test_dup2(diagnostic_fd, fileno(stderr)), fileno(stderr));
  }
  xmp_transaction_barrier = &barrier;
  const int reader_create_result = dt_pthread_create(&reader_thread, run_xmp_reader, &reader, FALSE);
  int writer_create_result = reader_create_result;
  if(reader_create_result == 0)
    writer_create_result = dt_pthread_create(&writer_thread, run_xmp_writer, &writer, FALSE);
  if(reader_create_result == 0 && writer_create_result != 0)
  {
    pthread_mutex_lock(&barrier.mutex);
    barrier.writer_ready = TRUE;
    pthread_cond_broadcast(&barrier.condition);
    pthread_mutex_unlock(&barrier.mutex);
  }
  if(reader_create_result == 0) assert_int_equal(pthread_join(reader_thread, NULL), 0);
  if(writer_create_result == 0) assert_int_equal(pthread_join(writer_thread, NULL), 0);
  xmp_transaction_barrier = NULL;
  if(outcome != XMP_ISOLATION_SUCCESS)
  {
    fflush(stderr);
    assert_int_equal(test_dup2(stderr_fd, fileno(stderr)), fileno(stderr));
    assert_int_equal(test_close(stderr_fd), 0);
    assert_int_equal(test_close(diagnostic_fd), 0);
  }

  if(reader_create_result != 0 || writer_create_result != 0)
  {
    if(!IS_NULL_PTR(diagnostic_path))
    {
      g_remove(diagnostic_path);
      dt_free(diagnostic_path);
    }
    pthread_cond_destroy(&barrier.condition);
    pthread_mutex_destroy(&barrier.mutex);
    dt_free(writer_filename);
    dt_free(filename);
    dt_free(properties);
    dt_free(setting);
    fail_msg("could not create XMP isolation worker threads");
    return;
  }

  assert_true(writer.result);
  assert_int_equal(dt_image_repository_get_write_timestamp(writer_image.id), writer_timestamp);
  assert_int_equal(barrier.event_count, (int)G_N_ELEMENTS(barrier.events));
  assert_int_equal(barrier.events[0], XMP_TRANSACTION_BEGIN);
  assert_int_equal(barrier.events[1], XMP_TRANSACTION_WRITER_READY);
  assert_int_equal(barrier.events[2], XMP_TRANSACTION_COORDINATOR_COMPLETE);
  assert_int_equal(barrier.events[3], XMP_TRANSACTION_WRITER_COMPLETE);
  assert_false(dt_database_transaction_is_owned_by_current_thread());
  if(outcome == XMP_ISOLATION_PARTIAL_FAILURE) assert_false(barrier.fail_write_timestamp);

  if(outcome == XMP_ISOLATION_SUCCESS)
  {
    assert_int_equal(reader.result, DT_XMP_IMPORT_OK);
    assert_int_equal(barrier.rollback_count, 0);
    assert_int_equal(dt_history_repository_count_items(image.id), 1);
    assert_int_equal(dt_image_repository_get_write_timestamp(image.id), xmp_timestamp);
    assert_int_equal(dt_image_get_xmp_rating(&image), 4);
    assert_true(image.flags & DT_IMAGE_AUTO_PRESETS_APPLIED);
    assert_xmp_metadata(image.id, description_key, "after");
    assert_true(dt_tag_repository_is_attached(imported_tag, image.id));
    assert_false(dt_tag_repository_is_attached(original_tag, image.id));
  }
  else
  {
    assert_int_equal(reader.result, DT_XMP_IMPORT_PERSIST_ERROR);
    assert_int_equal(barrier.rollback_count, 1);
    assert_development_preserved(image.id);
    assert_int_equal(dt_image_repository_get_write_timestamp(image.id), original_timestamp);
    assert_int_equal(dt_image_get_xmp_rating(&image), 1);
    assert_xmp_metadata(image.id, description_key, "before");
    assert_true(dt_tag_repository_is_attached(original_tag, image.id));
    assert_false(dt_tag_repository_is_attached(imported_tag, image.id));
    assert_memory_equal(&image, &original_image, sizeof(image));
    char *diagnostic = NULL;
    char *basename = g_path_get_basename(xmp_path);
    assert_true(g_file_get_contents(diagnostic_path, &diagnostic, NULL, NULL));
    assert_non_null(strstr(diagnostic, basename));
    assert_non_null(strstr(diagnostic, outcome == XMP_ISOLATION_PARTIAL_FAILURE ? "write-failure" : "commit-failure"));
    assert_null(strstr(diagnostic, "<rdf:"));
    assert_null(strstr(strstr(diagnostic, basename) + strlen(basename), basename));
    dt_free(basename);
    dt_free(diagnostic);
    g_remove(diagnostic_path);
    dt_free(diagnostic_path);
  }

  pthread_cond_destroy(&barrier.condition);
  pthread_mutex_destroy(&barrier.mutex);
  dt_free(writer_filename);
  dt_free(filename);
  dt_free(properties);
  dt_free(setting);
}

static void test_persisted_xmp_commit_isolated_from_repository_writer(void **state G_GNUC_UNUSED)
{
  run_xmp_isolation_case(XMP_ISOLATION_SUCCESS);
}

static void test_persisted_xmp_partial_failure_isolated_from_repository_writer(void **state G_GNUC_UNUSED)
{
  run_xmp_isolation_case(XMP_ISOLATION_PARTIAL_FAILURE);
}

static void test_persisted_xmp_commit_failure_isolated_from_repository_writer(void **state G_GNUC_UNUSED)
{
  run_xmp_isolation_case(XMP_ISOLATION_COMMIT_FAILURE);
}
#endif

static void test_successful_import_publishes_development_for_both_metadata_modes(void **state G_GNUC_UNUSED)
{
  const char *filenames[] = { "full-development.raw", "history-only.raw" };
  const guint original_tag = dt_tag_repository_insert("original");
  const guint imported_tag = dt_tag_repository_insert("imported");
  assert_true(original_tag > 0);
  assert_true(imported_tag > 0);

  for(int history_only = 0; history_only < 2; history_only++)
  {
    dt_image_t image = make_image(filenames[history_only]);
    image.flags |= DT_IMAGE_REMOVE | DT_IMAGE_HAS_TXT;
    dt_image_set_xmp_rating(&image, 1);
    image.color_labels = 1;
    dt_colorlabel_repository_set(image.id, 0);
    assert_true(dt_tag_repository_attach(original_tag, image.id));
    write_xmp(5, full_development_xmp);

    assert_int_equal(dt_exif_xmp_read(&image, xmp_path, history_only, NULL), DT_XMP_IMPORT_OK);
    assert_int_equal(dt_history_repository_count_items(image.id), 1);
    assert_int_equal(dt_history_repository_get_end(image.id), 1);
    assert_false(image.flags & DT_IMAGE_REMOVE);
    assert_true(image.flags & DT_IMAGE_HAS_TXT);
    assert_true(image.flags & DT_IMAGE_AUTO_PRESETS_APPLIED);
    assert_true(image.flags & DT_IMAGE_NO_LEGACY_PRESETS);
    assert_int_equal(image.change_timestamp, 1);
    assert_int_equal(image.export_timestamp, 2);
    assert_int_equal(image.print_timestamp, 3);
    assert_int_equal(image.history_hash, UINT64_C(0x0102030405060708));
    assert_int_equal(dt_image_get_xmp_rating(&image), history_only ? 1 : 4);
    assert_int_equal(image.color_labels, history_only ? 1 : 4);
    dt_image_repository_store(&image);
    assert_int_equal(dt_colorlabel_repository_get(image.id), history_only ? 1 : 4);
    assert_int_equal(dt_tag_repository_is_attached(original_tag, image.id), history_only);
    assert_int_equal(dt_tag_repository_is_attached(imported_tag, image.id), !history_only);
    assert_false(dt_database_transaction_is_owned_by_current_thread());
  }
}

static void test_xmp_import_updates_write_timestamp_atomically(void **state)
{
  (void)state;
  dt_image_t image = make_image("timestamp-success.raw");
  const int64_t timestamp = 4102444800;
  write_xmp(2, "<darktable:history><rdf:Seq/></darktable:history>");

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, &timestamp), DT_XMP_IMPORT_OK);
  assert_int_equal(dt_image_repository_get_write_timestamp(image.id), timestamp);
}

static void test_failed_write_timestamp_rolls_back_xmp_import(void **state)
{
  (void)state;
  dt_image_t image = make_image("timestamp-failure.raw");
  const int64_t original_timestamp = dt_image_repository_get_write_timestamp(image.id);
  const int64_t timestamp = 4102444800;
  seed_development(image.id);
  dt_image_set_xmp_rating(&image, 1);
  image.color_labels = 1;
  dt_colorlabel_repository_set(image.id, 0);
  const guint original_tag = dt_tag_repository_insert("original");
  const guint imported_tag = dt_tag_repository_insert("imported");
  assert_true(dt_tag_repository_attach(original_tag, image.id));
  const dt_image_t original_image = image;
  write_xmp(5, full_development_xmp);

  assert_int_equal(sqlite3_exec(dt_database_get_sqlite3_global(),
                                "CREATE TRIGGER reject_write_timestamp BEFORE UPDATE OF write_timestamp ON images "
                                "BEGIN SELECT RAISE(ABORT, 'write timestamp rejected'); END",
                                NULL, NULL, NULL), SQLITE_OK);
  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, &timestamp), DT_XMP_IMPORT_PERSIST_ERROR);
  assert_int_equal(dt_image_repository_get_write_timestamp(image.id), original_timestamp);
  assert_development_preserved(image.id);
  assert_memory_equal(&image, &original_image, sizeof(image));
  assert_int_equal(dt_colorlabel_repository_get(image.id), 1);
  assert_true(dt_tag_repository_is_attached(original_tag, image.id));
  assert_false(dt_tag_repository_is_attached(imported_tag, image.id));
  assert_false(dt_database_transaction_is_owned_by_current_thread());
}

static void test_metadata_insert_failure_restores_deleted_metadata(void **state)
{
  (void)state;
  dt_image_t image = make_image("metadata-insert-failure.raw");
  const char *key = dt_metadata_get_key(DT_METADATA_XMP_DC_DESCRIPTION);
  const char *name = dt_metadata_get_name(DT_METADATA_XMP_DC_DESCRIPTION);
  char *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", name);
  dt_conf_set_int(setting, DT_METADATA_FLAG_IMPORTED);
  dt_metadata_set(image.id, key, "keep", FALSE);
  const dt_image_t original_image = image;
  write_xmp(2, "<dc:description><rdf:Alt><rdf:li xml:lang=\"x-default\">replace</rdf:li></rdf:Alt></dc:description>");

  assert_int_equal(sqlite3_exec(dt_database_get_sqlite3_global(),
                                "CREATE TRIGGER reject_metadata_insert BEFORE INSERT ON meta_data "
                                "BEGIN SELECT RAISE(ABORT, 'metadata insert rejected'); END",
                                NULL, NULL, NULL), SQLITE_OK);
  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_PERSIST_ERROR);
  GList *values = dt_metadata_get(image.id, key, NULL);
  assert_non_null(values);
  assert_string_equal(values->data, "keep");
  assert_memory_equal(&image, &original_image, sizeof(image));

  assert_int_equal(sqlite3_exec(dt_database_get_sqlite3_global(), "DROP TRIGGER reject_metadata_insert", NULL, NULL, NULL), SQLITE_OK);
  g_list_free_full(values, dt_free_gpointer);
  dt_free(setting);
}

static void test_v4_timestamp_is_converted_from_unix_seconds(void **state)
{
  (void)state;
  dt_image_t image = make_image("v4-timestamp.raw");
  GDateTime *datetime = g_date_time_new_from_unix_utc(1);
  const GTimeSpan expected = dt_datetime_gdatetime_to_gtimespan(datetime);
  g_date_time_unref(datetime);
  write_xmp(4, "<darktable:change_timestamp>1</darktable:change_timestamp>");

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_OK);
  assert_int_equal(image.change_timestamp, expected);
  assert_int_not_equal(image.change_timestamp, 1);
}

static void test_v5_timestamp_is_a_direct_gtimespan(void **state)
{
  (void)state;
  dt_image_t image = make_image("v5-timestamp.raw");
  write_xmp(5, "<darktable:change_timestamp>1</darktable:change_timestamp>");

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_OK);
  assert_int_equal(image.change_timestamp, 1);
}

static void test_malformed_v5_timestamp_keeps_previous_value_and_imports_history(void **state)
{
  (void)state;
  dt_image_t image = make_image("malformed-v5-timestamp.raw");
  seed_development(image.id);
  const GTimeSpan timestamp = image.change_timestamp;
  write_xmp(5, "<darktable:history><rdf:Seq><rdf:li darktable:num=\"0\" darktable:operation=\"exposure\" "
               "darktable:enabled=\"1\" darktable:modversion=\"1\" darktable:params=\"0000\"/>"
               "</rdf:Seq></darktable:history>"
               "<darktable:change_timestamp>not-a-timestamp</darktable:change_timestamp>");

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_PARTIAL);
  assert_int_equal(dt_history_repository_count_items(image.id), 1);
  assert_int_equal(image.change_timestamp, timestamp);
}

static void test_v3_invalid_snapshots_do_not_discard_independent_history(void **state)
{
  (void)state;
  const dt_masks_node_circle_t point = { .center = { 0.25f, 0.50f }, .radius = 0.10f, .border = 0.20f };
  const float source[] = { 0.10f, 0.20f };
  const dt_masks_form_group_t missing_child[] = { { .formid = 99, .parentid = 1, .state = 0, .opacity = 1.0f } };
  const dt_masks_form_group_t cycle_first[] = { { .formid = 2, .parentid = 1, .state = 0, .opacity = 1.0f } };
  const dt_masks_form_group_t cycle_second[] = { { .formid = 1, .parentid = 2, .state = 0, .opacity = 1.0f } };
  const struct
  {
    const char *name;
    int expected_id;
  } cases[] = {
    { "malformed", 0 },
    { "missing-child", 1 },
    { "cycle", 0 },
  };

  for(guint index = 0; index < G_N_ELEMENTS(cases); index++)
  {
    dt_image_t image = make_image(cases[index].name);
    char *history = make_v3_history_step(0);
    char *valid = make_v3_circle_mask("darktable:mask_num=\"1\" darktable:mask_id=\"10\"", "valid", &point, source);
    char *invalid = NULL;
    if(index == 0)
      invalid = g_strdup("<rdf:li darktable:mask_num=\"0\" darktable:mask_id=\"1\" darktable:mask_type=\"4\" "
                         "darktable:mask_name=\"broken\" darktable:mask_version=\"1\" darktable:mask_points=\"00\" "
                         "darktable:mask_nb=\"1\" darktable:mask_src=\"\"/>");
    else if(index == 1)
      invalid = make_v3_group_mask(0, 1, missing_child, G_N_ELEMENTS(missing_child));
    else
    {
      char *first = make_v3_group_mask(0, 1, cycle_first, G_N_ELEMENTS(cycle_first));
      char *second = make_v3_group_mask(0, 2, cycle_second, G_N_ELEMENTS(cycle_second));
      invalid = g_strconcat(first, second, NULL);
      dt_free(second);
      dt_free(first);
    }
    char *properties = g_strdup_printf("<darktable:history><rdf:Seq>%s</rdf:Seq></darktable:history>"
                                       "<darktable:masks_history><rdf:Seq>%s%s</rdf:Seq></darktable:masks_history>",
                                       history, invalid, valid);
    write_xmp(3, properties);

    assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_PARTIAL);
    assert_int_equal(dt_history_repository_count_items(image.id), 1);
    mask_snapshot_row_t snapshots = { 0 };
    dt_history_repository_foreach_mask_item(image.id, collect_mask_snapshot_row, &snapshots);
    assert_int_equal(snapshots.count, 2);
    assert_int_equal(snapshots.nums[0], 0);
    assert_int_equal(snapshots.ids[0], cases[index].expected_id);
    if(cases[index].expected_id == 0)
    {
      assert_int_equal(snapshots.forms[0], 0);
      assert_int_equal(snapshots.points_lengths[0], 0);
    }
    else
    {
      assert_int_equal(snapshots.forms[0], DT_MASKS_GROUP);
      assert_int_equal(snapshots.points_lengths[0], (int)sizeof(missing_child));
      group_mask_history_row_t persisted = { 0 };
      dt_history_repository_foreach_mask_item(image.id, collect_group_mask_history_row, &persisted);
      assert_int_equal(persisted.count, 1);
      assert_int_equal(persisted.mask_id, 1);
      assert_memory_equal(persisted.members, missing_child, sizeof(missing_child));
    }
    assert_int_equal(snapshots.nums[1], 1);
    assert_int_equal(snapshots.ids[1], 10);
    dt_free(properties);
    dt_free(invalid);
    dt_free(valid);
    dt_free(history);
  }
}

static void test_v3_partially_missing_group_keeps_its_independent_child_snapshot(void **state)
{
  (void)state;
  const dt_masks_node_circle_t point = { .center = { 0.25f, 0.50f }, .radius = 0.10f, .border = 0.20f };
  const float source[] = { 0.10f, 0.20f };
  const dt_masks_form_group_t members[] = {
    { .formid = 2, .parentid = 1, .state = 0, .opacity = 1.0f },
    { .formid = 99, .parentid = 1, .state = 0, .opacity = 1.0f },
  };
  dt_image_t image = make_image("partially-missing-group.raw");
  char *history = make_v3_history_step(0);
  char *group = make_v3_group_mask(0, 1, members, G_N_ELEMENTS(members));
  char *child = make_v3_circle_mask("darktable:mask_num=\"0\" darktable:mask_id=\"2\"", "child", &point, source);
  char *properties = g_strdup_printf("<darktable:history><rdf:Seq>%s</rdf:Seq></darktable:history>"
                                     "<darktable:masks_history><rdf:Seq>%s%s</rdf:Seq></darktable:masks_history>",
                                     history, group, child);
  write_xmp(3, properties);

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_PARTIAL);
  mask_snapshot_row_t snapshots = { 0 };
  dt_history_repository_foreach_mask_item(image.id, collect_mask_snapshot_row, &snapshots);
  assert_int_equal(snapshots.count, 2);
  assert_int_equal(snapshots.nums[0], 0);
  assert_int_equal(snapshots.ids[0], 1);
  assert_int_equal(snapshots.forms[0], DT_MASKS_GROUP);
  assert_int_equal(snapshots.points_lengths[0], (int)sizeof(members));
  assert_int_equal(snapshots.nums[1], 0);
  assert_int_equal(snapshots.ids[1], 2);
  assert_int_equal(snapshots.forms[1], DT_MASKS_CIRCLE);
  assert_int_equal(snapshots.points_lengths[1], (int)sizeof(point));
  group_mask_history_row_t persisted = { 0 };
  dt_history_repository_foreach_mask_item(image.id, collect_group_mask_history_row, &persisted);
  assert_int_equal(persisted.count, 1);
  assert_int_equal(persisted.mask_id, 1);
  assert_memory_equal(persisted.members, members, sizeof(members));
  mask_history_rows_t rows = { 0 };
  dt_history_repository_foreach_mask_item(image.id, collect_mask_history_rows, &rows);
  assert_int_equal(rows.count, 1);
  assert_int_equal(rows.formids[0], 2);
  assert_memory_equal(&rows.points[0], &point, sizeof(point));

  dt_free(properties);
  dt_free(child);
  dt_free(group);
  dt_free(history);
}

static void test_v3_repeated_group_members_are_preserved(void **state)
{
  (void)state;
  const dt_masks_node_circle_t point = { .center = { 0.25f, 0.50f }, .radius = 0.10f, .border = 0.20f };
  const float source[] = { 0.10f, 0.20f };
  const dt_masks_form_group_t members[] = {
    { .formid = 2, .parentid = 1, .state = DT_MASKS_STATE_USE, .opacity = 0.25f },
    { .formid = 2, .parentid = 1, .state = DT_MASKS_STATE_USE | DT_MASKS_STATE_INVERSE, .opacity = 0.75f },
  };
  dt_image_t image = make_image("repeated-group-members.raw");
  char *history = make_v3_history_step(0);
  char *group = make_v3_group_mask(0, 1, members, G_N_ELEMENTS(members));
  char *child = make_v3_circle_mask("darktable:mask_num=\"0\" darktable:mask_id=\"2\"", "child", &point, source);
  char *properties = g_strdup_printf("<darktable:history><rdf:Seq>%s</rdf:Seq></darktable:history>"
                                     "<darktable:masks_history><rdf:Seq>%s%s</rdf:Seq></darktable:masks_history>",
                                     history, group, child);
  write_xmp(3, properties);

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_OK);
  group_mask_history_row_t persisted = { 0 };
  dt_history_repository_foreach_mask_item(image.id, collect_group_mask_history_row, &persisted);
  assert_int_equal(persisted.count, 1);
  assert_memory_equal(persisted.members, members, sizeof(members));

  dt_free(properties);
  dt_free(child);
  dt_free(group);
  dt_free(history);
}

static void test_v3_unassignable_mask_does_not_discard_valid_snapshot(void **state)
{
  (void)state;
  const dt_masks_node_circle_t point = { .center = { 0.25f, 0.50f }, .radius = 0.10f, .border = 0.20f };
  const float source[] = { 0.10f, 0.20f };
  dt_image_t image = make_image("unassignable-mask.raw");
  char *history = make_v3_history_step(0);
  char *valid = make_v3_circle_mask("darktable:mask_num=\"1\" darktable:mask_id=\"10\"", "valid", &point, source);
  char *properties = g_strdup_printf("<darktable:history><rdf:Seq>%s</rdf:Seq></darktable:history>"
                                     "<darktable:masks_history><rdf:Seq>"
                                     "<rdf:li darktable:mask_id=\"1\" darktable:mask_type=\"1\" darktable:mask_name=\"lost\" "
                                     "darktable:mask_version=\"1\" darktable:mask_points=\"\" darktable:mask_nb=\"0\" darktable:mask_src=\"\"/>"
                                     "%s</rdf:Seq></darktable:masks_history>", history, valid);
  write_xmp(3, properties);

  assert_partial_mask_snapshot_recovery(&image);
  dt_free(properties);
  dt_free(valid);
  dt_free(history);
}

static void test_v3_mask_property_suffix_omits_only_affected_snapshot(void **state)
{
  (void)state;
  const dt_masks_node_circle_t point = { .center = { 0.25f, 0.50f }, .radius = 0.10f, .border = 0.20f };
  const float source[] = { 0.10f, 0.20f };
  const char *property_names[] = {
    "darktable:mask_num", "darktable:mask_id", "darktable:mask_type", "darktable:mask_name",
    "darktable:mask_version", "darktable:mask_points", "darktable:mask_nb", "darktable:mask_src",
  };

  for(guint index = 0; index < G_N_ELEMENTS(property_names); index++)
  {
    char *filename = g_strdup_printf("mask-property-suffix-%u.raw", index);
    dt_image_t image = make_image(filename);
    char *history = make_v3_history_step(0);
    char *valid = make_v3_circle_mask("darktable:mask_num=\"1\" darktable:mask_id=\"10\"", "valid", &point, source);
    char *bad = g_strdup_printf("%s%s=\"0\" %s%s=\"1\" %s%s=\"1\" %s%s=\"lost\" "
                                "%s%s=\"1\" %s%s=\"\" %s%s=\"0\" %s%s=\"\"",
                                property_names[0], index == 0 ? "_extra" : "",
                                property_names[1], index == 1 ? "_extra" : "",
                                property_names[2], index == 2 ? "_extra" : "",
                                property_names[3], index == 3 ? "_extra" : "",
                                property_names[4], index == 4 ? "_extra" : "",
                                property_names[5], index == 5 ? "_extra" : "",
                                property_names[6], index == 6 ? "_extra" : "",
                                property_names[7], index == 7 ? "_extra" : "");
    char *packet = g_strdup_printf(
        "<darktable:history><rdf:Seq>%s</rdf:Seq></darktable:history>"
        "<darktable:masks_history><rdf:Seq><rdf:li %s/>%s</rdf:Seq></darktable:masks_history>",
        history, bad, valid);
    write_xmp(3, packet);

    assert_partial_mask_snapshot_recovery(&image);
    dt_free(packet);
    dt_free(bad);
    dt_free(valid);
    dt_free(history);
    dt_free(filename);
  }
}

static void test_v3_overdepth_snapshot_does_not_discard_independent_snapshot(void **state)
{
  (void)state;
  const dt_masks_node_circle_t point = { .center = { 0.25f, 0.50f }, .radius = 0.10f, .border = 0.20f };
  const float source[] = { 0.10f, 0.20f };
  GString *deep_groups = g_string_new(NULL);
  for(int mask_id = 1; mask_id <= 65; mask_id++)
  {
    const dt_masks_form_group_t member = { .formid = mask_id == 65 ? 1 : mask_id + 1,
                                           .parentid = mask_id, .state = 0, .opacity = 1.0f };
    char *group = make_v3_group_mask(0, mask_id, &member, 1);
    g_string_append(deep_groups, group);
    dt_free(group);
  }
  dt_image_t image = make_image("overdepth-mask.raw");
  char *history = make_v3_history_step(0);
  char *valid = make_v3_circle_mask("darktable:mask_num=\"1\" darktable:mask_id=\"10\"", "valid", &point, source);
  char *properties = g_strdup_printf("<darktable:history><rdf:Seq>%s</rdf:Seq></darktable:history>"
                                     "<darktable:masks_history><rdf:Seq>%s%s</rdf:Seq></darktable:masks_history>",
                                     history, deep_groups->str, valid);
  write_xmp(3, properties);

  assert_partial_mask_snapshot_recovery(&image);
  dt_free(properties);
  dt_free(valid);
  dt_free(history);
  g_string_free(deep_groups, TRUE);
}

static void test_unresolved_group_keeps_readable_blend_and_preserves_undecodable_bytes(void **state)
{
  (void)state;
  const dt_masks_form_group_t missing_child[] = { { .formid = 99, .parentid = 1, .state = 0, .opacity = 1.0f } };
  dt_develop_blend_params_t readable = { 0 };
  readable.mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_SHAPE;
  readable.mask_id = 1;
  unsigned char unknown[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
  unsigned char wrong_length[] = { 9, 8, 7, 6 };
  dt_image_t image = make_image("blend-recovery.raw");
  char *first = make_v3_history_step_with_blend(0, DEVELOP_BLEND_VERSION,
                                                 (const unsigned char *)&readable, sizeof(readable));
  char *second = make_v3_history_step_with_blend(1, 99, unknown, sizeof(unknown));
  char *third = make_v3_history_step_with_blend(2, DEVELOP_BLEND_VERSION, wrong_length, sizeof(wrong_length));
  char *invalid = make_v3_group_mask(0, 1, missing_child, G_N_ELEMENTS(missing_child));
  char *properties = g_strdup_printf("<darktable:history><rdf:Seq>%s%s%s</rdf:Seq></darktable:history>"
                                     "<darktable:masks_history><rdf:Seq>%s</rdf:Seq></darktable:masks_history>",
                                     first, second, third, invalid);
  write_xmp(3, properties);

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_PARTIAL);
  assert_int_equal(dt_history_repository_count_items(image.id), 3);
  assert_int_equal(dt_history_repository_count_mask_items(image.id), 1);
  history_blend_row_t rows = { 0 };
  dt_history_repository_foreach_row(image.id, collect_history_blend_row, &rows);
  assert_int_equal(rows.count, 3);
  assert_false(rows.enabled[0]);
  uint32_t mask_id = 0;
  assert_int_equal(dt_develop_blend_read_mask_id(rows.bytes[0], rows.lengths[0], rows.versions[0], &mask_id),
                   DT_DEVELOP_BLEND_MASK_ID_READABLE);
  assert_int_equal(mask_id, 1);
  assert_memory_equal(rows.bytes[0], &readable, sizeof(readable));
  assert_memory_equal(rows.bytes[1], unknown, sizeof(unknown));
  assert_int_equal(rows.versions[1], 99);
  assert_memory_equal(rows.bytes[2], wrong_length, sizeof(wrong_length));
  assert_int_equal(rows.versions[2], DEVELOP_BLEND_VERSION);

  dt_free(properties);
  dt_free(invalid);
  dt_free(third);
  dt_free(second);
  dt_free(first);
}

static void test_unresolved_group_keeps_member_order_operator_and_opacity(void **state)
{
  (void)state;
  const dt_masks_form_group_t members[] = {
    { .formid = 99, .parentid = 1, .state = DT_MASKS_STATE_USE | DT_MASKS_STATE_DIFFERENCE, .opacity = 0.25f },
    { .formid = 100, .parentid = 1, .state = DT_MASKS_STATE_USE | DT_MASKS_STATE_EXCLUSION | DT_MASKS_STATE_INVERSE, .opacity = 0.75f },
  };
  dt_develop_blend_params_t blend = { 0 };
  blend.mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_SHAPE;
  blend.mask_id = 1;
  dt_image_t image = make_image("unresolved-group-members.raw");
  char *history = make_v3_history_step_with_blend(0, DEVELOP_BLEND_VERSION,
                                                   (const unsigned char *)&blend, sizeof(blend));
  char *group = make_v3_group_mask(0, 1, members, G_N_ELEMENTS(members));
  char *properties = g_strdup_printf("<darktable:history><rdf:Seq>%s</rdf:Seq></darktable:history>"
                                     "<darktable:masks_history><rdf:Seq>%s</rdf:Seq></darktable:masks_history>",
                                     history, group);
  write_xmp(3, properties);

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_PARTIAL);
  group_mask_history_row_t persisted = { 0 };
  dt_history_repository_foreach_mask_item(image.id, collect_group_mask_history_row, &persisted);
  assert_int_equal(persisted.count, 1);
  assert_int_equal(persisted.points_len, (int)sizeof(members));
  assert_memory_equal(persisted.members, members, sizeof(members));
  history_blend_row_t rows = { 0 };
  dt_history_repository_foreach_row(image.id, collect_history_blend_row, &rows);
  assert_false(rows.enabled[0]);

  dt_free(properties);
  dt_free(group);
  dt_free(history);
}

static void test_unresolved_group_keeps_uniform_operation_enabled(void **state)
{
  (void)state;
  const dt_masks_form_group_t member = { .formid = 99, .parentid = 1, .state = DT_MASKS_STATE_USE, .opacity = 1.0f };
  dt_develop_blend_params_t blend = { 0 };
  blend.mask_mode = DEVELOP_MASK_ENABLED;
  blend.mask_id = 1;
  dt_image_t image = make_image("unresolved-uniform-group.raw");
  char *history = make_v3_history_step_with_blend(0, DEVELOP_BLEND_VERSION,
                                                   (const unsigned char *)&blend, sizeof(blend));
  char *group = make_v3_group_mask(0, 1, &member, 1);
  char *properties = g_strdup_printf("<darktable:history><rdf:Seq>%s</rdf:Seq></darktable:history>"
                                     "<darktable:masks_history><rdf:Seq>%s</rdf:Seq></darktable:masks_history>",
                                     history, group);
  write_xmp(3, properties);

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_PARTIAL);
  history_blend_row_t rows = { 0 };
  dt_history_repository_foreach_row(image.id, collect_history_blend_row, &rows);
  assert_true(rows.enabled[0]);

  dt_free(properties);
  dt_free(group);
  dt_free(history);
}

static void test_inactive_shape_and_raster_dependencies_keep_operations_enabled(void **state)
{
  (void)state;
  const dt_masks_form_group_t missing_member = { .formid = 99, .parentid = 1, .state = 0, .opacity = 1.0f };
  const struct
  {
    const char *name;
    uint32_t mask_mode;
    gboolean has_group;
  } current_cases[] = {
    { "inactive-current-shape", DEVELOP_MASK_SHAPE, TRUE },
    { "inactive-current-raster", DEVELOP_MASK_RASTER, FALSE },
  };

  for(size_t index = 0; index < G_N_ELEMENTS(current_cases); index++)
  {
    dt_develop_blend_params_t blend = { 0 };
    blend.mask_mode = current_cases[index].mask_mode;
    blend.mask_id = 1;
    g_strlcpy(blend.raster_mask_source, "missing", sizeof(blend.raster_mask_source));
    dt_image_t image = make_image(current_cases[index].name);
    char *history = make_v3_history_step_with_blend(0, DEVELOP_BLEND_VERSION,
                                                    (const unsigned char *)&blend, sizeof(blend));
    char *group = current_cases[index].has_group ? make_v3_group_mask(0, 1, &missing_member, 1) : NULL;
    char *properties = g_strdup_printf("<darktable:history><rdf:Seq>%s</rdf:Seq></darktable:history>%s",
                                       history, group ? "<darktable:masks_history><rdf:Seq>" : "");
    if(group)
    {
      char *with_group = g_strconcat(properties, group, "</rdf:Seq></darktable:masks_history>", NULL);
      dt_free(properties);
      properties = with_group;
    }
    write_xmp(3, properties);

    assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL),
                     current_cases[index].has_group ? DT_XMP_IMPORT_PARTIAL : DT_XMP_IMPORT_OK);
    history_blend_row_t rows = { 0 };
    dt_history_repository_foreach_row(image.id, collect_history_blend_row, &rows);
    assert_int_equal(rows.count, 1);
    assert_true(rows.enabled[0]);

    dt_free(properties);
    dt_free(group);
    dt_free(history);
  }

  const struct
  {
    int version;
    int length;
    size_t mask_id_offset;
    uint32_t mask_mode;
  } legacy_cases[] = {
    { 5, sizeof(blend_params_v5_t), offsetof(blend_params_v5_t, mask_id), DEVELOP_MASK_SHAPE },
    { 9, sizeof(blend_params_v9_t), offsetof(blend_params_v9_t, mask_id), DEVELOP_MASK_RASTER },
  };
  char *group_points = dt_exif_xmp_encode((const unsigned char *)&missing_member, sizeof(missing_member), NULL);
  static const unsigned char group_source[] = { 0 };
  char *encoded_group_source = dt_exif_xmp_encode(group_source, sizeof(group_source), NULL);

  for(size_t index = 0; index < G_N_ELEMENTS(legacy_cases); index++)
  {
    unsigned char payload[sizeof(dt_develop_blend_params_t)] = { 0 };
    const uint32_t mask_id = 1;
    memcpy(payload, &legacy_cases[index].mask_mode, sizeof(legacy_cases[index].mask_mode));
    memcpy(payload + legacy_cases[index].mask_id_offset, &mask_id, sizeof(mask_id));
    char *filename = g_strdup_printf("inactive-legacy-v%d", legacy_cases[index].version);
    dt_image_t image = make_image(filename);
    char *history = make_v3_history_step_with_blend(0, legacy_cases[index].version, payload,
                                                    legacy_cases[index].length);
    char *properties = g_strdup_printf(
        "<darktable:history><rdf:Seq>%s</rdf:Seq></darktable:history>"
        "<darktable:mask><rdf:Seq><rdf:li>%s</rdf:li></rdf:Seq></darktable:mask>"
        "<darktable:mask_src><rdf:Seq><rdf:li>%s</rdf:li></rdf:Seq></darktable:mask_src>"
        "<darktable:mask_name><rdf:Seq><rdf:li>group</rdf:li></rdf:Seq></darktable:mask_name>"
        "<darktable:mask_type><rdf:Seq><rdf:li>%d</rdf:li></rdf:Seq></darktable:mask_type>"
        "<darktable:mask_version><rdf:Seq><rdf:li>1</rdf:li></rdf:Seq></darktable:mask_version>"
        "<darktable:mask_id><rdf:Seq><rdf:li>1</rdf:li></rdf:Seq></darktable:mask_id>"
        "<darktable:mask_nb><rdf:Seq><rdf:li>1</rdf:li></rdf:Seq></darktable:mask_nb>",
        history, group_points, encoded_group_source, DT_MASKS_GROUP);
    write_xmp(2, properties);

    assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_PARTIAL);
    history_blend_row_t rows = { 0 };
    dt_history_repository_foreach_row(image.id, collect_history_blend_row, &rows);
    assert_int_equal(rows.count, 2);
    assert_string_equal(rows.operations[0], "mask_manager");
    assert_false(rows.enabled[0]);
    assert_string_equal(rows.operations[1], "exposure");
    assert_true(rows.enabled[1]);

    dt_free(properties);
    dt_free(history);
    dt_free(filename);
  }
  dt_free(encoded_group_source);
  dt_free(group_points);
}

static void test_raster_consumer_before_unusable_provider_is_disabled(void **state)
{
  (void)state;
  dt_develop_blend_params_t consumer = { 0 };
  dt_develop_blend_params_t provider = { 0 };
  consumer.mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_RASTER;
  g_strlcpy(consumer.raster_mask_source, "exposure", sizeof(consumer.raster_mask_source));
  consumer.raster_mask_instance = 0;
  consumer.raster_mask_id = 0;
  provider.mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_SHAPE;
  provider.mask_id = 1;
  dt_image_t image = make_image("raster-provider-after-consumer.raw");
  char *first = make_v3_history_step_with_blend(0, DEVELOP_BLEND_VERSION,
                                                 (const unsigned char *)&consumer, sizeof(consumer));
  char *second = make_v3_history_step_with_blend(1, DEVELOP_BLEND_VERSION,
                                                  (const unsigned char *)&provider, sizeof(provider));
  char *properties = g_strdup_printf("<darktable:history><rdf:Seq>%s%s</rdf:Seq></darktable:history>", first, second);
  write_xmp(3, properties);

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_OK);
  history_blend_row_t rows = { 0 };
  dt_history_repository_foreach_row(image.id, collect_history_blend_row, &rows);
  assert_int_equal(rows.count, 2);
  assert_false(rows.enabled[0]);
  assert_false(rows.enabled[1]);
  assert_memory_equal(rows.bytes[0], &consumer, sizeof(consumer));
  assert_memory_equal(rows.bytes[1], &provider, sizeof(provider));

  dt_free(properties);
  dt_free(second);
  dt_free(first);
}

static void test_legacy_unresolved_root_keeps_each_referenced_mask(void **state)
{
  (void)state;
  const dt_masks_node_circle_t point = { .center = { 0.25f, 0.50f }, .radius = 0.10f, .border = 0.20f };
  const float source[] = { 0.10f, 0.20f };
  const dt_masks_form_group_t missing_child[] = { { .formid = 99, .parentid = 1, .state = 0, .opacity = 1.0f } };
  dt_develop_blend_params_t invalid_root_blend = { 0 };
  dt_develop_blend_params_t valid_root_blend = { 0 };
  invalid_root_blend.mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_SHAPE;
  valid_root_blend.mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_SHAPE;
  invalid_root_blend.mask_id = 1;
  valid_root_blend.mask_id = 2;
  static const unsigned char group_source[] = { 0 };
  char *group_points = dt_exif_xmp_encode((const unsigned char *)missing_child, sizeof(missing_child), NULL);
  char *encoded_group_source = dt_exif_xmp_encode(group_source, sizeof(group_source), NULL);
  char *circle_points = dt_exif_xmp_encode((const unsigned char *)&point, sizeof(point), NULL);
  char *circle_source = dt_exif_xmp_encode((const unsigned char *)source, sizeof(source), NULL);
  char *first = make_v3_history_step_with_blend(0, DEVELOP_BLEND_VERSION,
                                                 (const unsigned char *)&invalid_root_blend, sizeof(invalid_root_blend));
  char *second = make_v3_history_step_with_blend(1, DEVELOP_BLEND_VERSION,
                                                  (const unsigned char *)&valid_root_blend, sizeof(valid_root_blend));
  char *properties = g_strdup_printf(
      "<darktable:history><rdf:Seq>%s%s</rdf:Seq></darktable:history>"
      "<darktable:mask><rdf:Seq><rdf:li>%s</rdf:li><rdf:li>%s</rdf:li></rdf:Seq></darktable:mask>"
      "<darktable:mask_src><rdf:Seq><rdf:li>%s</rdf:li><rdf:li>%s</rdf:li></rdf:Seq></darktable:mask_src>"
      "<darktable:mask_name><rdf:Seq><rdf:li>bad</rdf:li><rdf:li>good</rdf:li></rdf:Seq></darktable:mask_name>"
      "<darktable:mask_type><rdf:Seq><rdf:li>%d</rdf:li><rdf:li>%d</rdf:li></rdf:Seq></darktable:mask_type>"
      "<darktable:mask_version><rdf:Seq><rdf:li>1</rdf:li><rdf:li>1</rdf:li></rdf:Seq></darktable:mask_version>"
      "<darktable:mask_id><rdf:Seq><rdf:li>1</rdf:li><rdf:li>2</rdf:li></rdf:Seq></darktable:mask_id>"
      "<darktable:mask_nb><rdf:Seq><rdf:li>1</rdf:li><rdf:li>1</rdf:li></rdf:Seq></darktable:mask_nb>",
      first, second, group_points, circle_points, encoded_group_source, circle_source, DT_MASKS_GROUP, DT_MASKS_CIRCLE);
  dt_image_t image = make_image("legacy-independent-root.raw");
  write_xmp(2, properties);

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_PARTIAL);
  assert_int_equal(dt_history_repository_count_items(image.id), 3);
  assert_int_equal(dt_history_repository_count_mask_items(image.id), 2);
  history_blend_row_t rows = { 0 };
  dt_history_repository_foreach_row(image.id, collect_history_blend_row, &rows);
  assert_int_equal(rows.count, 3);
  uint32_t mask_id = 0;
  assert_int_equal(dt_develop_blend_read_mask_id(rows.bytes[1], rows.lengths[1], rows.versions[1], &mask_id),
                   DT_DEVELOP_BLEND_MASK_ID_READABLE);
  assert_int_equal(mask_id, 1);
  assert_memory_equal(rows.bytes[1], &invalid_root_blend, sizeof(invalid_root_blend));
  assert_int_equal(dt_develop_blend_read_mask_id(rows.bytes[2], rows.lengths[2], rows.versions[2], &mask_id),
                   DT_DEVELOP_BLEND_MASK_ID_READABLE);
  assert_int_equal(mask_id, 2);

  dt_free(properties);
  dt_free(second);
  dt_free(first);
  dt_free(circle_source);
  dt_free(encoded_group_source);
  dt_free(circle_points);
  dt_free(group_points);
}

static void test_legacy_readable_blends_keep_unresolved_root(void **state)
{
  (void)state;
  const dt_masks_node_circle_t point = { .center = { 0.25f, 0.50f }, .radius = 0.10f, .border = 0.20f };
  const float source[] = { 0.10f, 0.20f };
  const dt_masks_form_group_t missing_child[] = { { .formid = 99, .parentid = 1, .state = 0, .opacity = 1.0f } };
  const struct
  {
    int version;
    int length;
    size_t mask_id_offset;
  } cases[] = {
    { 1, sizeof(blend_params_v1_t), offsetof(blend_params_v1_t, mask_id) },
    { 2, sizeof(blend_params_v2_t), offsetof(blend_params_v2_t, mask_id) },
    { 3, sizeof(blend_params_v3_t), offsetof(blend_params_v3_t, mask_id) },
    { 4, sizeof(blend_params_v4_t), offsetof(blend_params_v4_t, mask_id) },
    { 5, sizeof(blend_params_v5_t), offsetof(blend_params_v5_t, mask_id) },
    { 6, sizeof(blend_params_v5_t), offsetof(blend_params_v5_t, mask_id) },
    { 7, sizeof(blend_params_v5_t), offsetof(blend_params_v5_t, mask_id) },
    { 8, sizeof(blend_params_v8_t), offsetof(blend_params_v8_t, mask_id) },
    { 9, sizeof(blend_params_v9_t), offsetof(blend_params_v9_t, mask_id) },
    { 10, sizeof(blend_params_v10_t), offsetof(blend_params_v10_t, mask_id) },
    { 11, sizeof(dt_develop_blend_params_t), offsetof(dt_develop_blend_params_t, mask_id) },
  };
  static const unsigned char group_source[] = { 0 };
  char *group_points = dt_exif_xmp_encode((const unsigned char *)missing_child, sizeof(missing_child), NULL);
  char *encoded_group_source = dt_exif_xmp_encode(group_source, sizeof(group_source), NULL);
  char *circle_points = dt_exif_xmp_encode((const unsigned char *)&point, sizeof(point), NULL);
  char *circle_source = dt_exif_xmp_encode((const unsigned char *)source, sizeof(source), NULL);

  for(guint index = 0; index < G_N_ELEMENTS(cases); index++)
  {
    unsigned char payload[sizeof(dt_develop_blend_params_t)];
    memset(payload, 0xa5, cases[index].length);
    const uint32_t original_mask_id = 1;
    memcpy(payload + cases[index].mask_id_offset, &original_mask_id, sizeof(original_mask_id));
    char *filename = g_strdup_printf("legacy-v%d-unresolved.raw", cases[index].version);
    dt_image_t image = make_image(filename);
    char *history = make_v3_history_step_with_blend(0, cases[index].version, payload, cases[index].length);
    char *properties = g_strdup_printf(
        "<darktable:history><rdf:Seq>%s</rdf:Seq></darktable:history>"
        "<darktable:mask><rdf:Seq><rdf:li>%s</rdf:li><rdf:li>%s</rdf:li></rdf:Seq></darktable:mask>"
        "<darktable:mask_src><rdf:Seq><rdf:li>%s</rdf:li><rdf:li>%s</rdf:li></rdf:Seq></darktable:mask_src>"
        "<darktable:mask_name><rdf:Seq><rdf:li>bad</rdf:li><rdf:li>good</rdf:li></rdf:Seq></darktable:mask_name>"
        "<darktable:mask_type><rdf:Seq><rdf:li>%d</rdf:li><rdf:li>%d</rdf:li></rdf:Seq></darktable:mask_type>"
        "<darktable:mask_version><rdf:Seq><rdf:li>1</rdf:li><rdf:li>1</rdf:li></rdf:Seq></darktable:mask_version>"
        "<darktable:mask_id><rdf:Seq><rdf:li>1</rdf:li><rdf:li>2</rdf:li></rdf:Seq></darktable:mask_id>"
        "<darktable:mask_nb><rdf:Seq><rdf:li>1</rdf:li><rdf:li>1</rdf:li></rdf:Seq></darktable:mask_nb>",
        history, group_points, circle_points, encoded_group_source, circle_source, DT_MASKS_GROUP, DT_MASKS_CIRCLE);
    write_xmp(2, properties);

    assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), DT_XMP_IMPORT_PARTIAL);
    assert_int_equal(dt_history_repository_count_mask_items(image.id), 2);
    group_mask_history_row_t root = { 0 };
    dt_history_repository_foreach_mask_item(image.id, collect_group_mask_history_row, &root);
    assert_int_equal(root.count, 1);
    assert_int_equal(root.mask_id, 1);
    assert_memory_equal(root.members, missing_child, sizeof(missing_child));
    mask_history_rows_t masks = { 0 };
    dt_history_repository_foreach_mask_item(image.id, collect_mask_history_rows, &masks);
    assert_int_equal(masks.count, 1);
    assert_int_equal(masks.formids[0], 2);
    assert_memory_equal(&masks.points[0], &point, sizeof(point));
    history_blend_row_t rows = { 0 };
    dt_history_repository_foreach_row(image.id, collect_history_blend_row, &rows);
    assert_int_equal(rows.count, 2);
    assert_string_equal(rows.operations[0], "mask_manager");
    assert_false(rows.enabled[0]);
    assert_string_equal(rows.operations[1], "exposure");
    uint32_t mask_id = 0;
    assert_int_equal(dt_develop_blend_read_mask_id(rows.bytes[1], rows.lengths[1], rows.versions[1], &mask_id),
                      DT_DEVELOP_BLEND_MASK_ID_READABLE);
    assert_int_equal(mask_id, original_mask_id);
    assert_memory_equal(rows.bytes[1], payload, cases[index].length);
    dt_free(filename);
    dt_free(properties);
    dt_free(history);
  }
  dt_free(circle_source);
  dt_free(circle_points);
  dt_free(encoded_group_source);
  dt_free(group_points);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_excluded_descriptive_metadata_survives_full_xmp_read),
    cmocka_unit_test(test_unsupported_xmp_schema_imports_metadata_without_replacing_development),
    cmocka_unit_test(test_v3_damaged_snapshot_preserves_empty_replay_boundary_through_export),
    cmocka_unit_test(test_malformed_v2_history_preserves_development),
    cmocka_unit_test(test_short_history_operation_preserves_development),
    cmocka_unit_test(test_v1_optional_priorities_preserve_history_instances),
    cmocka_unit_test(test_partial_v1_legacy_mask_properties_preserve_development),
    cmocka_unit_test(test_partial_v2_legacy_mask_properties_preserve_development),
    cmocka_unit_test(test_duplicate_legacy_mask_ids_preserve_development),
    cmocka_unit_test(test_v3_masks_reuse_formid_across_history_steps),
    cmocka_unit_test(test_invalid_v3_mask_properties_omit_only_affected_snapshot),
    cmocka_unit_test(test_duplicate_v3_mask_ids_omit_only_affected_snapshot),
    cmocka_unit_test(test_malformed_history_restores_cached_raw_parameters_and_flags),
    cmocka_unit_test(test_legacy_raw_parameters_decode_flip_byte),
    cmocka_unit_test(test_empty_valid_history_is_accepted),
    cmocka_unit_test(test_v1_sequence_history_is_accepted),
    cmocka_unit_test(test_superold_bag_history_is_accepted),
    cmocka_unit_test(test_out_of_range_rating_preserves_existing_rating),
    cmocka_unit_test(test_absent_rating_clears_xmp_rating_flags),
    cmocka_unit_test(test_valid_rating_import_updates_rating),
    cmocka_unit_test(test_malformed_nonempty_history_blendop_blob_preserves_development),
    cmocka_unit_test(test_rejected_xmp_reports_one_stable_diagnostic),
    cmocka_unit_test(test_metadata_only_import_does_not_notify_tags),
    cmocka_unit_test(test_v2_optional_history_numbers_use_sequence_order),
    cmocka_unit_test(test_invalid_v3_history_numbers_preserve_development),
    cmocka_unit_test(test_valid_v3_history_numbers_are_persisted),
    cmocka_unit_test(test_v3_near_prefix_history_properties_preserve_development),
    cmocka_unit_test(test_short_v2_history_blendop_blob_is_preserved),
    cmocka_unit_test(test_v1_history_blendop_blob_is_accepted),
    cmocka_unit_test(test_v7_history_blendop_blob_is_accepted),
    cmocka_unit_test(test_empty_xmp_decode_is_valid),
    cmocka_unit_test(test_xmp_import_rejects_outer_transaction),
    cmocka_unit_test(test_malformed_nonempty_legacy_mask_blob_preserves_development),
    cmocka_unit_test(test_malformed_compressed_xmp_decode_returns_null),
    cmocka_unit_test(test_failed_xmp_load_does_not_record_undo),
    cmocka_unit_test(test_missing_sidecar_returns_read_error_without_rejection_diagnostic),
    cmocka_unit_test(test_invalid_present_rating_and_timestamp_keep_previous_values_with_partial_history),
#ifdef __linux__
    cmocka_unit_test(test_persisted_xmp_commit_isolated_from_repository_writer),
    cmocka_unit_test(test_persisted_xmp_partial_failure_isolated_from_repository_writer),
    cmocka_unit_test(test_persisted_xmp_commit_failure_isolated_from_repository_writer),
#endif
    cmocka_unit_test(test_xmp_import_updates_write_timestamp_atomically),
    cmocka_unit_test(test_successful_import_publishes_development_for_both_metadata_modes),
    cmocka_unit_test(test_failed_write_timestamp_rolls_back_xmp_import),
    cmocka_unit_test(test_metadata_insert_failure_restores_deleted_metadata),
    cmocka_unit_test(test_v4_timestamp_is_converted_from_unix_seconds),
    cmocka_unit_test(test_v5_timestamp_is_a_direct_gtimespan),
    cmocka_unit_test(test_malformed_v5_timestamp_keeps_previous_value_and_imports_history),
    cmocka_unit_test(test_v3_invalid_snapshots_do_not_discard_independent_history),
    cmocka_unit_test(test_v3_partially_missing_group_keeps_its_independent_child_snapshot),
    cmocka_unit_test(test_v3_repeated_group_members_are_preserved),
    cmocka_unit_test(test_v3_unassignable_mask_does_not_discard_valid_snapshot),
    cmocka_unit_test(test_v3_mask_property_suffix_omits_only_affected_snapshot),
    cmocka_unit_test(test_v3_overdepth_snapshot_does_not_discard_independent_snapshot),
    cmocka_unit_test(test_unresolved_group_keeps_readable_blend_and_preserves_undecodable_bytes),
    cmocka_unit_test(test_unresolved_group_keeps_member_order_operator_and_opacity),
    cmocka_unit_test(test_unresolved_group_keeps_uniform_operation_enabled),
    cmocka_unit_test(test_inactive_shape_and_raster_dependencies_keep_operations_enabled),
    cmocka_unit_test(test_raster_consumer_before_unusable_provider_is_disabled),
    cmocka_unit_test(test_legacy_unresolved_root_keeps_each_referenced_mask),
    cmocka_unit_test(test_legacy_readable_blends_keep_unresolved_root),
  };
  return cmocka_run_group_tests(tests, setup, teardown);
}
