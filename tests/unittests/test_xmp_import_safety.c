/*
    This file is part of Ansel,
    Copyright (C) 2026 Paolo SANTUCCI.

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
#include "metadata/exif.h"
#include "metadata/metadata.h"

#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>

static char *config_path = NULL;
static char *xmp_path = NULL;

static int setup(void **state)
{
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

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), 0);
  GList *values = dt_metadata_get(image.id, key, NULL);
  assert_non_null(values);
  assert_string_equal(values->data, "keep");

  g_list_free_full(values, dt_free_gpointer);
  dt_free(setting);
}

static void test_unsupported_xmp_schema_preserves_development(void **state)
{
  (void)state;
  dt_image_t image = make_image("unsupported.raw");
  seed_development(image.id);
  write_xmp(99, "");

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), 1);
  assert_development_preserved(image.id);
}

static void test_malformed_v2_history_preserves_development(void **state)
{
  (void)state;
  dt_image_t image = make_image("malformed-history.raw");
  seed_development(image.id);
  write_xmp(2, "<darktable:history><rdf:Seq><rdf:li darktable:operation=\"exposure\"/>"
               "</rdf:Seq></darktable:history>");

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), 1);
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

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), 1);
  assert_development_preserved(image.id);
}

static void test_partial_v1_legacy_mask_properties_preserve_development(void **state)
{
  (void)state;
  dt_image_t image = make_image("partial-v1-mask.raw");
  seed_development(image.id);
  write_xmp(1, "<darktable:mask><rdf:Seq><rdf:li>AA==</rdf:li></rdf:Seq></darktable:mask>");

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), 1);
  assert_development_preserved(image.id);
}

static void test_partial_v2_legacy_mask_properties_preserve_development(void **state)
{
  (void)state;
  dt_image_t image = make_image("partial-v2-mask.raw");
  seed_development(image.id);
  write_xmp(2, "<darktable:mask><rdf:Seq><rdf:li>AA==</rdf:li></rdf:Seq></darktable:mask>");

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), 1);
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

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), 1);
  assert_development_preserved(image.id);
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

static void test_empty_valid_history_is_accepted(void **state)
{
  (void)state;
  dt_image_t image = make_image("empty-history.raw");
  seed_development(image.id);
  write_xmp(2, "<darktable:history><rdf:Seq/></darktable:history>");

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), 0);
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

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), 0);
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

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), 0);
  assert_int_equal(dt_history_repository_count_items(image.id), 1);
}

static void test_out_of_range_rating_preserves_existing_rating(void **state)
{
  (void)state;
  dt_image_t image = make_image("rating.raw");
  dt_image_set_xmp_rating(&image, 3);
  write_xmp(0, "<xmp:Rating>6</xmp:Rating>");

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), 1);
  assert_int_equal(dt_image_get_xmp_rating(&image), 3);
}

static void test_valid_rating_import_updates_rating(void **state)
{
  (void)state;
  dt_image_t image = make_image("valid-rating.raw");
  dt_image_set_xmp_rating(&image, 1);
  write_xmp(2, "<xmp:Rating>4</xmp:Rating>");

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), 0);
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

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), 1);
  assert_development_preserved(image.id);
}

static void assert_invalid_v2_history(const char *filename, const char *history)
{
  dt_image_t image = make_image(filename);
  seed_development(image.id);
  write_xmp(2, history);

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), 1);
  assert_development_preserved(image.id);
}

static void test_missing_v2_history_num_preserves_development(void **state)
{
  (void)state;
  assert_invalid_v2_history("missing-num.raw",
                            "<darktable:history><rdf:Seq><rdf:li darktable:operation=\"exposure\" "
                            "darktable:enabled=\"1\" darktable:modversion=\"1\" darktable:params=\"0000\"/>"
                            "</rdf:Seq></darktable:history>");
}

static void test_duplicate_v2_history_num_preserves_development(void **state)
{
  (void)state;
  assert_invalid_v2_history("duplicate-num.raw",
                            "<darktable:history><rdf:Seq>"
                            "<rdf:li darktable:num=\"0\" darktable:operation=\"exposure\" darktable:enabled=\"1\" "
                            "darktable:modversion=\"1\" darktable:params=\"0000\"/>"
                            "<rdf:li darktable:num=\"0\" darktable:operation=\"exposure\" darktable:enabled=\"1\" "
                            "darktable:modversion=\"1\" darktable:params=\"0000\"/>"
                            "</rdf:Seq></darktable:history>");
}

static void test_negative_v2_history_num_preserves_development(void **state)
{
  (void)state;
  assert_invalid_v2_history("negative-num.raw",
                            "<darktable:history><rdf:Seq><rdf:li darktable:num=\"-1\" darktable:operation=\"exposure\" "
                            "darktable:enabled=\"1\" darktable:modversion=\"1\" darktable:params=\"0000\"/>"
                            "</rdf:Seq></darktable:history>");
}

static void test_short_v2_history_blendop_blob_preserves_development(void **state)
{
  (void)state;
  assert_invalid_v2_history("short-blendop.raw",
                            "<darktable:history><rdf:Seq><rdf:li darktable:num=\"0\" darktable:operation=\"exposure\" "
                            "darktable:enabled=\"1\" darktable:modversion=\"1\" darktable:params=\"0000\" "
                            "darktable:blendop_params=\"0000\"/></rdf:Seq></darktable:history>");
}

static void test_v1_history_blendop_blob_is_accepted(void **state)
{
  (void)state;
  dt_image_t image = make_image("v1-blendop.raw");
  write_xmp(2, "<darktable:history><rdf:Seq><rdf:li darktable:num=\"0\" darktable:operation=\"exposure\" "
               "darktable:enabled=\"1\" darktable:modversion=\"1\" darktable:params=\"0000\" "
               "darktable:blendop_params=\"000000000000000000000000\"/></rdf:Seq></darktable:history>");

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), 0);
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

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), 0);
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
  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), 1);
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

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), 1);
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

  assert_int_equal(dt_history_load_and_apply(image.id, xmp_path, TRUE), 1);
  assert_false(dt_is_undo_list_populated(dt_undo_get_global(), DT_UNDO_LT_HISTORY));
}

static void test_xmp_import_updates_write_timestamp_atomically(void **state)
{
  (void)state;
  dt_image_t image = make_image("timestamp-success.raw");
  const int64_t timestamp = 4102444800;
  write_xmp(2, "<darktable:history><rdf:Seq/></darktable:history>");

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, &timestamp), 0);
  assert_int_equal(dt_image_repository_get_write_timestamp(image.id), timestamp);
}

static void test_failed_write_timestamp_rolls_back_xmp_import(void **state)
{
  (void)state;
  dt_image_t image = make_image("timestamp-failure.raw");
  const int64_t original_timestamp = dt_image_repository_get_write_timestamp(image.id);
  const int64_t timestamp = 4102444800;
  seed_development(image.id);
  write_xmp(2, "<darktable:history><rdf:Seq/></darktable:history>");

  assert_int_equal(sqlite3_exec(dt_database_get_sqlite3_global(),
                                "CREATE TRIGGER reject_write_timestamp BEFORE UPDATE OF write_timestamp ON images "
                                "BEGIN SELECT RAISE(ABORT, 'write timestamp rejected'); END",
                                NULL, NULL, NULL), SQLITE_OK);
  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, &timestamp), 1);
  assert_int_equal(dt_image_repository_get_write_timestamp(image.id), original_timestamp);
  assert_development_preserved(image.id);
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
  write_xmp(2, "<dc:description><rdf:Alt><rdf:li xml:lang=\"x-default\">replace</rdf:li></rdf:Alt></dc:description>");

  assert_int_equal(sqlite3_exec(dt_database_get_sqlite3_global(),
                                "CREATE TRIGGER reject_metadata_insert BEFORE INSERT ON meta_data "
                                "BEGIN SELECT RAISE(ABORT, 'metadata insert rejected'); END",
                                NULL, NULL, NULL), SQLITE_OK);
  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), 1);
  GList *values = dt_metadata_get(image.id, key, NULL);
  assert_non_null(values);
  assert_string_equal(values->data, "keep");

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

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), 0);
  assert_int_equal(image.change_timestamp, expected);
  assert_int_not_equal(image.change_timestamp, 1);
}

static void test_v5_timestamp_is_a_direct_gtimespan(void **state)
{
  (void)state;
  dt_image_t image = make_image("v5-timestamp.raw");
  write_xmp(5, "<darktable:change_timestamp>1</darktable:change_timestamp>");

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), 0);
  assert_int_equal(image.change_timestamp, 1);
}

static void test_malformed_v5_timestamp_preserves_development(void **state)
{
  (void)state;
  dt_image_t image = make_image("malformed-v5-timestamp.raw");
  seed_development(image.id);
  const GTimeSpan timestamp = image.change_timestamp;
  write_xmp(5, "<darktable:change_timestamp>not-a-timestamp</darktable:change_timestamp>");

  assert_int_equal(dt_exif_xmp_read(&image, xmp_path, FALSE, NULL), 1);
  assert_development_preserved(image.id);
  assert_int_equal(image.change_timestamp, timestamp);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_excluded_descriptive_metadata_survives_full_xmp_read),
    cmocka_unit_test(test_unsupported_xmp_schema_preserves_development),
    cmocka_unit_test(test_malformed_v2_history_preserves_development),
    cmocka_unit_test(test_short_history_operation_preserves_development),
    cmocka_unit_test(test_partial_v1_legacy_mask_properties_preserve_development),
    cmocka_unit_test(test_partial_v2_legacy_mask_properties_preserve_development),
    cmocka_unit_test(test_duplicate_legacy_mask_ids_preserve_development),
    cmocka_unit_test(test_malformed_history_restores_cached_raw_parameters_and_flags),
    cmocka_unit_test(test_empty_valid_history_is_accepted),
    cmocka_unit_test(test_v1_sequence_history_is_accepted),
    cmocka_unit_test(test_superold_bag_history_is_accepted),
    cmocka_unit_test(test_out_of_range_rating_preserves_existing_rating),
    cmocka_unit_test(test_valid_rating_import_updates_rating),
    cmocka_unit_test(test_malformed_nonempty_history_blendop_blob_preserves_development),
    cmocka_unit_test(test_missing_v2_history_num_preserves_development),
    cmocka_unit_test(test_duplicate_v2_history_num_preserves_development),
    cmocka_unit_test(test_negative_v2_history_num_preserves_development),
    cmocka_unit_test(test_short_v2_history_blendop_blob_preserves_development),
    cmocka_unit_test(test_v1_history_blendop_blob_is_accepted),
    cmocka_unit_test(test_v7_history_blendop_blob_is_accepted),
    cmocka_unit_test(test_empty_xmp_decode_is_valid),
    cmocka_unit_test(test_xmp_import_rejects_outer_transaction),
    cmocka_unit_test(test_malformed_nonempty_legacy_mask_blob_preserves_development),
    cmocka_unit_test(test_malformed_compressed_xmp_decode_returns_null),
    cmocka_unit_test(test_failed_xmp_load_does_not_record_undo),
    cmocka_unit_test(test_xmp_import_updates_write_timestamp_atomically),
    cmocka_unit_test(test_failed_write_timestamp_rolls_back_xmp_import),
    cmocka_unit_test(test_metadata_insert_failure_restores_deleted_metadata),
    cmocka_unit_test(test_v4_timestamp_is_converted_from_unix_seconds),
    cmocka_unit_test(test_v5_timestamp_is_a_direct_gtimespan),
    cmocka_unit_test(test_malformed_v5_timestamp_preserves_development),
  };
  return cmocka_run_group_tests(tests, setup, teardown);
}
