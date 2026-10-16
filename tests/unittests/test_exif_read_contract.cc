#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

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
#include "darktable.h"
#include "metadata/exif.h"
#include "metadata/notify.h"

#include "exif_test_redirect.h"

extern "C" int dt_test_exif_read_persisted(dt_image_t *img, const char *path);

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

typedef struct dt_test_exif_fixture_t
{
  char *user_config_dir;
  char *user_cache_dir;
  char *config_path;
  char *path;
  int32_t image_id;
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
  image->xmpData()["Xmp.dc.subject"] = "contract-keyword";
  image->writeMetadata();
}

extern "C" gboolean dt_test_exif_start_transaction(void)
{
  start_calls++;
  if(start_outcome == DT_TEST_EXIF_TRANSACTION_FAIL) return FALSE;
  return dt_database_start_transaction_debug();
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
}

static int setup(void **state)
{
  int fd = -1;
  int32_t film_id = -1;

  start_outcome = DT_TEST_EXIF_TRANSACTION_DELEGATE;
  release_outcome = DT_TEST_EXIF_TRANSACTION_DELEGATE;
  start_calls = 0;
  release_calls = 0;
  rollback_calls = 0;
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
  dt_image_xmp_mode_refresh_from_conf();
  dt_exif_init();
  fixture.path = NULL;
  fd = g_file_open_tmp("ansel-test-exif-contract-XXXXXX.jpg", &fixture.path, NULL);
  if(fd < 0) goto cleanup_exif;
  g_close(fd, NULL);
  write_keyword_fixture(fixture.path);

  film_id = testdb_make_film("/testdb/exif-contract");
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

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(test_persisted_read_rolls_back_begin_failure, setup, teardown),
    cmocka_unit_test_setup_teardown(test_persisted_read_rejects_stale_image_before_transaction, setup, teardown),
    cmocka_unit_test_setup_teardown(test_persisted_read_rolls_back_commit_failure, setup, teardown),
    cmocka_unit_test_setup_teardown(test_persisted_read_rolls_back_tag_persistence_failure, setup, teardown),
    cmocka_unit_test_setup_teardown(test_persisted_read_commits_keyword, setup, teardown),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
