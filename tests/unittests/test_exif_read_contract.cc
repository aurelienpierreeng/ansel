#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <utime.h>

#include <exiv2/exiv2.hpp>
#include <glib/gstdio.h>

extern "C"
{
#include <cmocka.h>
}

#include "testdb.h"

#include "common/conf.h"
#include "common/file_location.h"
#include "common/image.h"
#include "common/datetime.h"
#include "common/variables.h"
#include "database/image_repository.h"
#include "database/tag_repository.h"
#include "darktable.h"
#include "metadata/exif.h"
#include "metadata/metadata.h"
#include "metadata/notify.h"

#include "exif_test_redirect.h"

extern "C" int dt_test_exif_read_persisted(dt_image_t *img, const char *path);
extern "C" bool dt_test_exif_apply_xmp_persistence(dt_image_t *img, Exiv2::XmpData &xmpData,
                                                     int version, bool replace_existing, bool *tags_changed);

#include "../../src/metadata/exif.cc"

#undef dt_database_start_transaction
#undef dt_database_release_transaction
#undef dt_database_rollback_transaction
#undef dt_datetime_unix_to_img

typedef enum dt_test_exif_transaction_outcome_t
{
  DT_TEST_EXIF_TRANSACTION_DELEGATE,
  DT_TEST_EXIF_TRANSACTION_FAIL
} dt_test_exif_transaction_outcome_t;

static dt_test_exif_transaction_outcome_t start_outcome = DT_TEST_EXIF_TRANSACTION_DELEGATE;
static dt_test_exif_transaction_outcome_t release_outcome = DT_TEST_EXIF_TRANSACTION_DELEGATE;
static int start_calls = 0;
static int release_calls = 0;
static int rollback_calls = 0;
static int notification_calls = 0;
static GMutex coordination_mutex;
static GCond coordination_condition;
static gboolean reader_reached_datetime = FALSE;
static gboolean writer_may_write = FALSE;
static gboolean writer_finished = FALSE;
static gboolean coordinate_transaction = FALSE;

typedef struct dt_test_exif_fixture_t
{
  char *user_config_dir;
  char *user_cache_dir;
  char *config_path;
  char *path;
  int32_t image_id;
  int32_t film_id;
} dt_test_exif_fixture_t;

static dt_test_exif_fixture_t fixture = { 0 };

static const char *jpeg_base64 =
    "/9j/4AAQSkZJRgABAQEAYABgAAD/2wBDAP//////////////////////////////////////////"
    "//////////////////////////////////////2wBDAf//////////////////////////////////////////"
    "//////////////////////////////////////wAARCAABAAEDASIAAhEBAxEB/8QAFQABAQAAAAAAAAAAAAAAAA"
    "AAAAX/xAAUEAEAAAAAAAAAAAAAAAAAAAAA/9oADAMBAAIQAxAAAAH/xAAUEAEAAAAAAAAAAAAAAAAAAAAA/9oA"
    "CAEAAgEFAqf/xAAUEQEAAAAAAAAAAAAAAAAAAAAA/9oACAEDAQE/Aaf/xAAUEQEAAAAAAAAAAAAAAAAAAAAA/9oA"
    "CAECAQE/Aaf/xAAUEAEAAAAAAAAAAAAAAAAAAAAA/9oACAEBAAY/Aqf/xAAUEAEAAAAAAAAAAAAAAAAAAAAA/9oA"
    "CAEBAAE/IX//2gAMAwEAAgADAAAAEP/EABQRAQAAAAAAAAAAAAAAAAAAABD/2gAIAQMBAT8QH//EABQRAQAAAAAA"
    "AAAAAAAAAAAAABD/2gAIAQIBAT8QH//EABQQAQAAAAAAAAAAAAAAAAAAABD/2gAIAQEAAT8QH//Z";

static void count_notification(void)
{
  notification_calls++;
}

static void write_keyword_fixture(const char *path)
{
  gsize jpeg_size = 0;
  g_autofree guchar *jpeg = g_base64_decode(jpeg_base64, &jpeg_size);
  assert_non_null(jpeg);
  assert_true(g_file_set_contents(path, (const gchar *)jpeg, jpeg_size, NULL));

  std::unique_ptr<Exiv2::Image> image(Exiv2::ImageFactory::open(path));
  assert_non_null(image.get());
  image->readMetadata();
  image->exifData()["Exif.Image.Make"] = "AnselFixture";
  image->exifData()["Exif.Image.Model"] = "Scratch";
  image->exifData()["Exif.Photo.DateTimeOriginal"] = "2012:03:04 05:06:07";
  image->xmpData()["Xmp.dc.subject"] = "contract-keyword";
  image->writeMetadata();
}

extern "C" gboolean dt_test_exif_start_transaction(void)
{
  start_calls++;
  if(start_outcome == DT_TEST_EXIF_TRANSACTION_FAIL) return FALSE;
  const gboolean started = dt_database_start_transaction_debug();
  if(!started) return FALSE;
  if(coordinate_transaction)
  {
    g_mutex_lock(&coordination_mutex);
    writer_may_write = TRUE;
    g_cond_broadcast(&coordination_condition);
    while(!writer_finished) g_cond_wait(&coordination_condition, &coordination_mutex);
    g_mutex_unlock(&coordination_mutex);
  }
  return TRUE;
}

extern "C" gboolean dt_test_exif_release_transaction(void)
{
  release_calls++;
  if(release_outcome == DT_TEST_EXIF_TRANSACTION_FAIL) return FALSE;
  return dt_database_release_transaction_debug();
}

extern "C" void dt_test_exif_rollback_transaction(void)
{
  rollback_calls++;
  dt_database_rollback_transaction();
}

extern "C" void dt_test_exif_unix_to_img(dt_image_t *img, const time_t *datetime)
{
  dt_datetime_unix_to_img(img, datetime);
  g_mutex_lock(&coordination_mutex);
  reader_reached_datetime = TRUE;
  g_cond_broadcast(&coordination_condition);
  g_mutex_unlock(&coordination_mutex);
}

static gpointer write_unrelated_image(gpointer user_data)
{
  const int iteration = GPOINTER_TO_INT(user_data);
  g_mutex_lock(&coordination_mutex);
  while(!writer_may_write) g_cond_wait(&coordination_condition, &coordination_mutex);
  g_mutex_unlock(&coordination_mutex);

  char *filename = g_strdup_printf("unrelated-%d.jpg", iteration);
  assert_true(dt_image_repository_insert_import(fixture.film_id, filename, DT_IMAGE_LDR, 0));
  dt_free(filename);

  g_mutex_lock(&coordination_mutex);
  writer_finished = TRUE;
  g_cond_broadcast(&coordination_condition);
  g_mutex_unlock(&coordination_mutex);
  return NULL;
}

static gpointer read_scratch_image(gpointer user_data)
{
  dt_image_t *image = (dt_image_t *)user_data;
  assert_int_equal(dt_test_exif_read(image, fixture.path), 0);
  return NULL;
}

static int setup(void **state)
{
  int fd = -1;
  int32_t film_id = -1;

  if(IS_NULL_PTR(dt_datetime_origin())) dt_datetime_init();

  start_outcome = DT_TEST_EXIF_TRANSACTION_DELEGATE;
  release_outcome = DT_TEST_EXIF_TRANSACTION_DELEGATE;
  start_calls = 0;
  release_calls = 0;
  rollback_calls = 0;
  notification_calls = 0;
  coordinate_transaction = FALSE;
  g_mutex_init(&coordination_mutex);
  g_cond_init(&coordination_condition);
  fixture.user_config_dir = NULL;
  fixture.user_cache_dir = NULL;
  fixture.user_config_dir = g_dir_make_tmp("ansel-test-exif-config-XXXXXX", NULL);
  if(IS_NULL_PTR(fixture.user_config_dir)) return -1;
  dt_loc_init_user_config_dir(fixture.user_config_dir);
  fixture.user_cache_dir = g_dir_make_tmp("ansel-test-exif-cache-XXXXXX", NULL);
  if(IS_NULL_PTR(fixture.user_cache_dir)) goto cleanup_location;
  dt_loc_init_user_cache_dir(fixture.user_cache_dir);
  if(testdb_setup(state)) goto cleanup_location;

  fixture.config_path = NULL;
  fd = g_file_open_tmp("ansel-test-exif-contract-XXXXXX.rc", &fixture.config_path, NULL);
  if(fd < 0) goto cleanup_database;
  g_close(fd, NULL);
  g_remove(fixture.config_path);

  darktable.conf = (dt_conf_t *)calloc(1, sizeof(dt_conf_t));
  if(IS_NULL_PTR(darktable.conf)) goto cleanup_config;
  dt_conf_init(darktable.conf, fixture.config_path, NULL);
  dt_conf_set_string("write_sidecar_files", "FALSE");
  {
    const dt_metadata_t imported_keys[] = {
      DT_METADATA_XMP_DC_CREATOR,
      DT_METADATA_XMP_DC_DESCRIPTION,
      DT_METADATA_XMP_DC_RIGHTS,
    };
    for(size_t i = 0; i < G_N_ELEMENTS(imported_keys); i++)
    {
      const char *name = dt_metadata_get_name(imported_keys[i]);
      char *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", name);
      dt_conf_set_int(setting, DT_METADATA_FLAG_IMPORTED);
      dt_free(setting);
    }
  }
  dt_image_xmp_mode_refresh_from_conf();
  dt_exif_init();
  fixture.path = NULL;
  fd = g_file_open_tmp("ansel-test-exif-contract-XXXXXX.jpg", &fixture.path, NULL);
  if(fd < 0) goto cleanup_exif;
  g_close(fd, NULL);
  write_keyword_fixture(fixture.path);

  film_id = testdb_make_film("/testdb/exif-contract");
  fixture.film_id = film_id;
  fixture.image_id = testdb_make_image(film_id, "embedded.jpg");
  if(fixture.image_id <= 0) goto cleanup_file;
  dt_metadata_set_tags_changed_handler(count_notification);
  return 0;

cleanup_file:
  g_remove(fixture.path);
  dt_free(fixture.path);
  fixture.path = NULL;
cleanup_exif:
  dt_exif_cleanup();
  dt_conf_cleanup(darktable.conf);
  dt_free(darktable.conf);
  darktable.conf = NULL;
cleanup_config:
  g_remove(fixture.config_path);
  dt_free(fixture.config_path);
  fixture.config_path = NULL;
cleanup_database:
  testdb_teardown(state);
cleanup_location:
  if(!IS_NULL_PTR(fixture.user_cache_dir)) g_rmdir(fixture.user_cache_dir);
  dt_free(fixture.user_cache_dir);
  fixture.user_cache_dir = NULL;
  g_rmdir(fixture.user_config_dir);
  dt_free(fixture.user_config_dir);
  fixture.user_config_dir = NULL;
  return -1;
}

static int teardown(void **state)
{
  dt_metadata_set_tags_changed_handler(NULL);
  g_cond_clear(&coordination_condition);
  g_mutex_clear(&coordination_mutex);
  dt_exif_cleanup();
  dt_conf_cleanup(darktable.conf);
  dt_free(darktable.conf);
  darktable.conf = NULL;
  g_remove(fixture.config_path);
  dt_free(fixture.config_path);
  fixture.config_path = NULL;
  g_remove(fixture.path);
  dt_free(fixture.path);
  fixture.path = NULL;
  const int result = testdb_teardown(state);
  g_rmdir(fixture.user_cache_dir);
  dt_free(fixture.user_cache_dir);
  fixture.user_cache_dir = NULL;
  g_rmdir(fixture.user_config_dir);
  dt_free(fixture.user_config_dir);
  fixture.user_config_dir = NULL;
  return result;
}

static dt_image_t persisted_image(void)
{
  dt_image_t image;
  dt_image_init(&image);
  image.id = fixture.image_id;
  return image;
}

static void test_persisted_read_rolls_back_begin_failure(void **state)
{
  (void)state;
  dt_image_t image = persisted_image();
  const dt_image_t original = image;
  start_outcome = DT_TEST_EXIF_TRANSACTION_FAIL;

  assert_int_equal(dt_test_exif_read_persisted(&image, fixture.path), 1);
  assert_memory_equal(&image, &original, sizeof(image));
  assert_int_equal(dt_tag_repository_find_by_name("contract-keyword"), 0);
  assert_int_equal(notification_calls, 0);
  assert_int_equal(rollback_calls, 0);
}

static void test_persisted_read_rejects_stale_image_before_transaction(void **state)
{
  (void)state;
  dt_image_t image = persisted_image();
  image.id += 1000;
  const dt_image_t original = image;

  assert_int_equal(dt_test_exif_read_persisted(&image, fixture.path), 1);
  assert_memory_equal(&image, &original, sizeof(image));
  assert_int_equal(start_calls, 0);
  assert_int_equal(release_calls, 0);
  assert_int_equal(rollback_calls, 0);
  assert_int_equal(notification_calls, 0);
}

static void test_persisted_read_rolls_back_commit_failure(void **state)
{
  (void)state;
  dt_image_t image = persisted_image();
  const dt_image_t original = image;
  release_outcome = DT_TEST_EXIF_TRANSACTION_FAIL;

  assert_int_equal(dt_test_exif_read_persisted(&image, fixture.path), 1);
  assert_memory_equal(&image, &original, sizeof(image));
  assert_int_equal(dt_tag_repository_find_by_name("contract-keyword"), 0);
  assert_int_equal(notification_calls, 0);
  assert_int_equal(rollback_calls, 1);
  assert_false(dt_database_transaction_is_owned_by_current_thread());
}

static void test_persisted_read_rolls_back_tag_persistence_failure(void **state)
{
  (void)state;
  std::unique_ptr<Exiv2::Image> fixture_image(Exiv2::ImageFactory::open(fixture.path));
  assert_non_null(fixture_image.get());
  fixture_image->readMetadata();
  fixture_image->xmpData()["Xmp.dc.subject"] = std::string(1024, 'x');
  fixture_image->writeMetadata();

  dt_image_t image = persisted_image();
  const dt_image_t original = image;

  assert_int_equal(dt_test_exif_read_persisted(&image, fixture.path), 1);
  assert_memory_equal(&image, &original, sizeof(image));
  assert_int_equal(dt_tag_repository_find_by_name("contract-keyword"), 0);
  assert_int_equal(notification_calls, 0);
  assert_int_equal(rollback_calls, 1);
  assert_false(dt_database_transaction_is_owned_by_current_thread());
}

static void test_persisted_read_commits_keyword(void **state)
{
  (void)state;
  dt_image_t image = persisted_image();

  assert_int_equal(dt_test_exif_read_persisted(&image, fixture.path), 0);
  const guint tagid = dt_tag_repository_find_by_name("contract-keyword");
  assert_true(tagid > 0);
  assert_true(dt_tag_repository_is_attached(tagid, image.id));
  assert_int_equal(notification_calls, 1);
}

static void test_persisted_read_imports_exif_only_metadata(void **state)
{
  (void)state;
  std::unique_ptr<Exiv2::Image> fixture_image(Exiv2::ImageFactory::open(fixture.path));
  assert_non_null(fixture_image.get());
  fixture_image->readMetadata();
  fixture_image->xmpData().clear();
  fixture_image->exifData()["Exif.Image.Artist"] = "Ansel Author";
  fixture_image->exifData()["Exif.Image.ImageDescription"] = "Ansel Description";
  fixture_image->exifData()["Exif.Image.Copyright"] = "Ansel Copyright";
  fixture_image->writeMetadata();

  dt_image_t image = persisted_image();
  assert_int_equal(dt_test_exif_read_persisted(&image, fixture.path), 0);

  GList *creator = dt_metadata_get(image.id, "Xmp.dc.creator", NULL);
  GList *description = dt_metadata_get(image.id, "Xmp.dc.description", NULL);
  GList *rights = dt_metadata_get(image.id, "Xmp.dc.rights", NULL);
  assert_non_null(creator);
  assert_non_null(description);
  assert_non_null(rights);
  assert_string_equal((const char *)creator->data, "Ansel Author");
  assert_string_equal((const char *)description->data, "Ansel Description");
  assert_string_equal((const char *)rights->data, "Ansel Copyright");
  g_list_free_full(rights, dt_free_gpointer);
  g_list_free_full(description, dt_free_gpointer);
  g_list_free_full(creator, dt_free_gpointer);
}

static void test_persisted_read_rolls_back_exif_only_metadata(void **state)
{
  (void)state;
  std::unique_ptr<Exiv2::Image> fixture_image(Exiv2::ImageFactory::open(fixture.path));
  assert_non_null(fixture_image.get());
  fixture_image->readMetadata();
  fixture_image->xmpData().clear();
  fixture_image->exifData()["Exif.Image.Artist"] = "Ansel Author";
  fixture_image->writeMetadata();

  dt_image_t image = persisted_image();
  release_outcome = DT_TEST_EXIF_TRANSACTION_FAIL;
  assert_int_equal(dt_test_exif_read_persisted(&image, fixture.path), 1);
  GList *creator = dt_metadata_get(image.id, "Xmp.dc.creator", NULL);
  assert_null(creator);
  assert_int_equal(rollback_calls, 1);
  assert_int_equal(notification_calls, 0);
}

static void test_persisted_read_notifies_for_metadata_only_xmp(void **state)
{
  (void)state;
  std::unique_ptr<Exiv2::Image> fixture_image(Exiv2::ImageFactory::open(fixture.path));
  assert_non_null(fixture_image.get());
  fixture_image->readMetadata();
  fixture_image->xmpData().clear();
  fixture_image->xmpData()["Xmp.dc.description"] = "metadata-only";
  fixture_image->writeMetadata();

  dt_image_t image = persisted_image();
  assert_int_equal(dt_test_exif_read_persisted(&image, fixture.path), 0);
  GList *description = dt_metadata_get(image.id, "Xmp.dc.description", NULL);
  assert_non_null(description);
  assert_string_equal((const char *)description->data, "metadata-only");
  assert_int_equal(notification_calls, 1);
  g_list_free_full(description, dt_free_gpointer);
}

static void test_persisted_read_notifies_for_iptc_caption_only(void **state)
{
  (void)state;
  std::unique_ptr<Exiv2::Image> fixture_image(Exiv2::ImageFactory::open(fixture.path));
  assert_non_null(fixture_image.get());
  fixture_image->readMetadata();
  fixture_image->xmpData().clear();
  fixture_image->iptcData().clear();
  fixture_image->iptcData()["Iptc.Application2.Caption"] = "iptc-caption-only";
  fixture_image->writeMetadata();

  dt_image_t image = persisted_image();
  assert_int_equal(dt_test_exif_read_persisted(&image, fixture.path), 0);
  GList *description = dt_metadata_get(image.id, "Xmp.dc.description", NULL);
  assert_non_null(description);
  assert_string_equal((const char *)description->data, "iptc-caption-only");
  assert_int_equal(notification_calls, 1);
  g_list_free_full(description, dt_free_gpointer);
}

static void test_xmp_persistence_marks_metadata_replacement(void **state)
{
  (void)state;
  dt_image_t image = persisted_image();
  assert_true(dt_metadata_set_import(image.id, "Xmp.dc.description", "existing"));
  Exiv2::XmpData xmp_data;
  bool tags_changed = false;

  assert_true(dt_test_exif_start_transaction());
  assert_true(dt_test_exif_apply_xmp_persistence(&image, xmp_data, -1, true, &tags_changed));
  assert_true(tags_changed);
  assert_true(dt_test_exif_release_transaction());
  GList *description = dt_metadata_get(image.id, "Xmp.dc.description", NULL);
  assert_null(description);
  assert_int_equal(notification_calls, 0);
}

static void test_generic_read_keeps_scratch_fields_without_persistence(void **state)
{
  (void)state;
  dt_image_t image;
  dt_image_init(&image);

  assert_int_equal(dt_test_exif_read(&image, fixture.path), 0);
  assert_string_equal(image.exif_maker, "AnselFixture");
  assert_string_equal(image.exif_model, "Scratch");
  char datetime[DT_DATETIME_EXIF_LENGTH] = { 0 };
  assert_true(dt_datetime_img_to_exif(datetime, sizeof(datetime), &image));
  assert_memory_equal(datetime, "2012", 4);
  dt_variables_params_t *params = NULL;
  dt_variables_params_init(&params);
  params->img = &image;
  char pattern[] = "$(EXIF.YEAR)";
  char *year = dt_variables_expand(params, pattern, FALSE);
  assert_string_equal(year, "2012");
  dt_free(year);
  dt_variables_params_destroy(params);
  assert_int_equal(start_calls, 0);
  assert_int_equal(release_calls, 0);
  assert_int_equal(rollback_calls, 0);
  assert_int_equal(dt_tag_repository_find_by_name("contract-keyword"), 0);
  assert_int_equal(notification_calls, 0);
}

static void test_generic_read_keeps_persisted_image_database_free(void **state)
{
  (void)state;
  dt_image_t image = persisted_image();

  assert_int_equal(dt_test_exif_read(&image, fixture.path), 0);
  assert_string_equal(image.exif_maker, "AnselFixture");
  assert_int_equal(start_calls, 0);
  assert_int_equal(release_calls, 0);
  assert_int_equal(rollback_calls, 0);
  assert_int_equal(dt_tag_repository_find_by_name("contract-keyword"), 0);
  assert_int_equal(notification_calls, 0);
}

static void test_generic_read_keeps_iptc_and_xmp_image_fields(void **state)
{
  (void)state;
  std::unique_ptr<Exiv2::Image> fixture_image(Exiv2::ImageFactory::open(fixture.path));
  assert_non_null(fixture_image.get());
  fixture_image->readMetadata();
  fixture_image->iptcData()["Iptc.Application2.DateCreated"] = "2011-02-03";
  fixture_image->iptcData()["Iptc.Application2.TimeCreated"] = "04:05:06";
  fixture_image->xmpData()["Xmp.exif.GPSAltitude"] = "123/1";
  fixture_image->xmpData()["Xmp.exif.GPSAltitudeRef"] = "0";
  fixture_image->xmpData()["Xmp.exifEX.LensModel"] = "lang=x-default Lens Fixture";
  fixture_image->writeMetadata();

  dt_image_t image;
  dt_image_init(&image);
  assert_int_equal(dt_test_exif_read(&image, fixture.path), 0);
  char datetime[DT_DATETIME_EXIF_LENGTH] = { 0 };
  assert_true(dt_datetime_img_to_exif(datetime, sizeof(datetime), &image));
  assert_memory_equal(datetime, "2011", 4);
  assert_float_equal(image.geoloc.elevation, 123.0, 1e-6);
  assert_string_equal(image.exif_lens, "Lens Fixture");
  assert_int_equal(start_calls, 0);
  assert_int_equal(release_calls, 0);
  assert_int_equal(rollback_calls, 0);
  assert_int_equal(notification_calls, 0);
}

static void test_generic_read_does_not_rollback_unrelated_write(void **state)
{
  (void)state;
  coordinate_transaction = TRUE;
  for(int iteration = 0; iteration < 100; iteration++)
  {
    reader_reached_datetime = FALSE;
    writer_may_write = FALSE;
    writer_finished = FALSE;
    dt_image_t image;
    dt_image_init(&image);
    GThread *writer = g_thread_new("exif-writer", write_unrelated_image, GINT_TO_POINTER(iteration));
    GThread *reader = g_thread_new("exif-reader", read_scratch_image, &image);

    g_mutex_lock(&coordination_mutex);
    while(!reader_reached_datetime) g_cond_wait(&coordination_condition, &coordination_mutex);
    g_mutex_unlock(&coordination_mutex);
    g_thread_join(reader);

    g_mutex_lock(&coordination_mutex);
    writer_may_write = TRUE;
    g_cond_broadcast(&coordination_condition);
    while(!writer_finished) g_cond_wait(&coordination_condition, &coordination_mutex);
    g_mutex_unlock(&coordination_mutex);
    g_thread_join(writer);

    char *filename = g_strdup_printf("unrelated-%d.jpg", iteration);
    assert_true(dt_image_repository_find_by_film_and_filename(fixture.film_id, filename) > UNKNOWN_IMAGE);
    dt_free(filename);
  }
  assert_int_equal(start_calls, 0);
  assert_int_equal(release_calls, 0);
  assert_int_equal(rollback_calls, 0);
  assert_int_equal(notification_calls, 0);
  coordinate_transaction = FALSE;
}

static void test_generic_read_retains_pfm_fallbacks(void **state)
{
  (void)state;
  char *path = NULL;
  const int fd = g_file_open_tmp("ansel-test-exif-fallback-XXXXXX.pfm", &path, NULL);
  assert_true(fd >= 0);
  g_close(fd, NULL);
  assert_true(g_file_set_contents(path, "PF\n1 1\n-1.0\n\0\0\0\0", 16, NULL));
  const time_t timestamp = 981173106;
  const struct utimbuf times = { .actime = timestamp, .modtime = timestamp };
  assert_int_equal(g_utime(path, &times), 0);

  dt_image_t image;
  dt_image_t expected;
  dt_image_init(&image);
  dt_image_init(&expected);
  dt_datetime_unix_to_img(&expected, &timestamp);
  assert_int_equal(dt_test_exif_read(&image, path), 1);
  assert_true(image.flags & DT_IMAGE_HDR);
  assert_int_equal(image.exif_datetime_taken, expected.exif_datetime_taken);
  assert_int_equal(start_calls, 0);
  assert_int_equal(release_calls, 0);
  assert_int_equal(rollback_calls, 0);
  g_remove(path);
  dt_free(path);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(test_persisted_read_rolls_back_begin_failure, setup, teardown),
    cmocka_unit_test_setup_teardown(test_persisted_read_rejects_stale_image_before_transaction, setup, teardown),
    cmocka_unit_test_setup_teardown(test_persisted_read_rolls_back_commit_failure, setup, teardown),
    cmocka_unit_test_setup_teardown(test_persisted_read_rolls_back_tag_persistence_failure, setup, teardown),
    cmocka_unit_test_setup_teardown(test_persisted_read_commits_keyword, setup, teardown),
    cmocka_unit_test_setup_teardown(test_persisted_read_imports_exif_only_metadata, setup, teardown),
    cmocka_unit_test_setup_teardown(test_persisted_read_rolls_back_exif_only_metadata, setup, teardown),
    cmocka_unit_test_setup_teardown(test_persisted_read_notifies_for_metadata_only_xmp, setup, teardown),
    cmocka_unit_test_setup_teardown(test_persisted_read_notifies_for_iptc_caption_only, setup, teardown),
    cmocka_unit_test_setup_teardown(test_xmp_persistence_marks_metadata_replacement, setup, teardown),
    cmocka_unit_test_setup_teardown(test_generic_read_keeps_scratch_fields_without_persistence, setup, teardown),
    cmocka_unit_test_setup_teardown(test_generic_read_keeps_persisted_image_database_free, setup, teardown),
    cmocka_unit_test_setup_teardown(test_generic_read_keeps_iptc_and_xmp_image_fields, setup, teardown),
    cmocka_unit_test_setup_teardown(test_generic_read_does_not_rollback_unrelated_write, setup, teardown),
    cmocka_unit_test_setup_teardown(test_generic_read_retains_pfm_fallbacks, setup, teardown),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
