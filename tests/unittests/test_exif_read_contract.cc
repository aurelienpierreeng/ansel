#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <utime.h>
#include <array>

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

extern "C" gboolean dt_test_exif_start_transaction(void);
extern "C" gboolean dt_test_exif_release_transaction(void);
extern "C" void dt_test_exif_rollback_transaction(void);
extern "C" void dt_test_exif_unix_to_img(dt_image_t *img, const time_t *datetime);
extern "C" int dt_test_exif_read_persisted(dt_image_t *img, const char *path);
extern "C" bool dt_test_exif_apply_xmp_persistence(dt_image_t *img, Exiv2::XmpData &xmpData,
                                                     int version, bool replace_existing, bool *tags_changed);

#define dt_database_start_transaction_debug dt_test_exif_start_transaction
#define dt_database_release_transaction_debug dt_test_exif_release_transaction
#define dt_database_rollback_transaction dt_test_exif_rollback_transaction
#define dt_datetime_unix_to_img dt_test_exif_unix_to_img
#define dt_exif_set_exiv2_taglist dt_test_exif_set_exiv2_taglist
#define dt_exif_get_exiv2_taglist dt_test_exif_get_exiv2_taglist
#define dt_remove_exif_keys dt_test_remove_exif_keys
#define dt_exif_decode_xmp_data dt_test_exif_decode_xmp_data
#define dt_exif_apply_xmp_persistence dt_test_exif_apply_xmp_persistence
#define dt_exif_read_exif_tag dt_test_exif_read_exif_tag
#define dt_exif_read_usercrop dt_test_exif_read_usercrop
#define dt_exif_img_check_additional_tags dt_test_exif_img_check_additional_tags
#define dt_exif_read_from_blob dt_test_exif_read_from_blob
#define dt_exif_get_thumbnail dt_test_exif_get_thumbnail
#define dt_exif_read dt_test_exif_read
#define dt_exif_read_persisted dt_test_exif_read_persisted
#define dt_exif_write_blob dt_test_exif_write_blob
#define dt_exif_get_color_space dt_test_exif_get_color_space
#define dt_exif_get_datetime_taken dt_test_exif_get_datetime_taken
#define dt_exif_init dt_test_exif_init
#define dt_exif_cleanup dt_test_exif_cleanup

#include "../../src/metadata/exif.cc"

#undef dt_database_start_transaction_debug
#undef dt_database_release_transaction_debug
#undef dt_database_rollback_transaction
#undef dt_datetime_unix_to_img

enum class dt_test_exif_transaction_outcome_t
{
  DT_TEST_EXIF_TRANSACTION_DELEGATE,
  DT_TEST_EXIF_TRANSACTION_FAIL
};

struct dt_test_exif_fixture_t
{
  char *user_config_dir;
  char *user_cache_dir;
  char *config_path;
  char *path;
  int32_t image_id;
  int32_t film_id;
  dt_test_exif_transaction_outcome_t start_outcome;
  dt_test_exif_transaction_outcome_t release_outcome;
  int start_calls;
  int release_calls;
  int rollback_calls;
  int notification_calls;
  GMutex coordination_mutex;
  GCond coordination_condition;
  gboolean reader_reached_datetime;
  gboolean writer_may_write;
  gboolean writer_finished;
  gboolean coordinate_transaction;
  const char *unlink_after_datetime_path;
  char *rawspeed_datadir;
};

/** Share the serial CMocka fixture with the C-linkage injection and GLib thread callbacks. */
static dt_test_exif_fixture_t &test_context()
{
  static dt_test_exif_fixture_t fixture = {};
  return fixture;
}

static const char *const jpeg_base64 =
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
  test_context().notification_calls++;
}

static void write_keyword_fixture(const char *path)
{
  gsize jpeg_size = 0;
  g_autofree guchar *jpeg = g_base64_decode(jpeg_base64, &jpeg_size);
  assert_non_null(jpeg);
  assert_true(g_file_set_contents(path, (const gchar *)jpeg, jpeg_size, nullptr));

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
  auto &fixture = test_context();
  fixture.start_calls++;
  if(fixture.start_outcome == dt_test_exif_transaction_outcome_t::DT_TEST_EXIF_TRANSACTION_FAIL) return FALSE;
  if(const gboolean started = dt_database_start_transaction_debug(); !started) return FALSE;
  if(fixture.coordinate_transaction)
  {
    g_mutex_lock(&fixture.coordination_mutex);
    fixture.writer_may_write = TRUE;
    g_cond_broadcast(&fixture.coordination_condition);
    while(!fixture.writer_finished) g_cond_wait(&fixture.coordination_condition, &fixture.coordination_mutex);
    g_mutex_unlock(&fixture.coordination_mutex);
  }
  return TRUE;
}

extern "C" gboolean dt_test_exif_release_transaction(void)
{
  auto &fixture = test_context();
  fixture.release_calls++;
  if(fixture.release_outcome == dt_test_exif_transaction_outcome_t::DT_TEST_EXIF_TRANSACTION_FAIL) return FALSE;
  return dt_database_release_transaction_debug();
}

extern "C" void dt_test_exif_rollback_transaction(void)
{
  test_context().rollback_calls++;
  dt_database_rollback_transaction();
}

extern "C" void dt_test_exif_unix_to_img(dt_image_t *img, const time_t *datetime)
{
  auto &fixture = test_context();
  dt_datetime_unix_to_img(img, datetime);
  if(!IS_NULL_PTR(fixture.unlink_after_datetime_path))
  {
    g_remove(fixture.unlink_after_datetime_path);
    fixture.unlink_after_datetime_path = nullptr;
  }
  g_mutex_lock(&fixture.coordination_mutex);
  fixture.reader_reached_datetime = TRUE;
  g_cond_broadcast(&fixture.coordination_condition);
  g_mutex_unlock(&fixture.coordination_mutex);
}

static gpointer write_unrelated_image(gpointer user_data)
{
  auto &fixture = test_context();
  const auto iteration = GPOINTER_TO_INT(user_data);
  g_mutex_lock(&fixture.coordination_mutex);
  while(!fixture.writer_may_write) g_cond_wait(&fixture.coordination_condition, &fixture.coordination_mutex);
  g_mutex_unlock(&fixture.coordination_mutex);

  char *filename = g_strdup_printf("unrelated-%d.jpg", iteration);
  assert_true(dt_image_repository_insert_import(fixture.film_id, filename, DT_IMAGE_LDR, 0));
  dt_free(filename);

  g_mutex_lock(&fixture.coordination_mutex);
  fixture.writer_finished = TRUE;
  g_cond_broadcast(&fixture.coordination_condition);
  g_mutex_unlock(&fixture.coordination_mutex);
  return nullptr;
}

static gpointer read_scratch_image(gpointer user_data)
{
  auto *image = (dt_image_t *)user_data;
  assert_int_equal(dt_test_exif_read(image, test_context().path), 0);
  return nullptr;
}

static int setup(void **state)
{
  auto &fixture = test_context();
  int fd = -1;
  int32_t film_id = -1;

  if(IS_NULL_PTR(dt_datetime_origin())) dt_datetime_init();
  fixture.start_outcome = dt_test_exif_transaction_outcome_t::DT_TEST_EXIF_TRANSACTION_DELEGATE;
  fixture.release_outcome = dt_test_exif_transaction_outcome_t::DT_TEST_EXIF_TRANSACTION_DELEGATE;
  fixture.start_calls = 0;
  fixture.release_calls = 0;
  fixture.rollback_calls = 0;
  fixture.notification_calls = 0;
  fixture.coordinate_transaction = FALSE;
  fixture.unlink_after_datetime_path = nullptr;
  g_mutex_init(&fixture.coordination_mutex);
  g_cond_init(&fixture.coordination_condition);
  fixture.user_config_dir = nullptr;
  fixture.user_cache_dir = nullptr;
  fixture.user_config_dir = g_dir_make_tmp("ansel-test-exif-config-XXXXXX", nullptr);
  if(IS_NULL_PTR(fixture.user_config_dir)) return -1;
  dt_loc_init_user_config_dir(fixture.user_config_dir);
  fixture.user_cache_dir = g_dir_make_tmp("ansel-test-exif-cache-XXXXXX", nullptr);
  if(IS_NULL_PTR(fixture.user_cache_dir)) goto cleanup_location;
  dt_loc_init_user_cache_dir(fixture.user_cache_dir);
  if(testdb_setup(state)) goto cleanup_location;

  fixture.config_path = nullptr;
  fd = g_file_open_tmp("ansel-test-exif-contract-XXXXXX.rc", &fixture.config_path, nullptr);
  if(fd < 0) goto cleanup_database;
  g_close(fd, nullptr);
  g_remove(fixture.config_path);

  darktable.conf = g_try_new0(dt_conf_t, 1);
  if(IS_NULL_PTR(darktable.conf)) goto cleanup_config;
  dt_conf_init(darktable.conf, fixture.config_path, nullptr);
  dt_conf_set_string("write_sidecar_files", "FALSE");
  {
    const std::array imported_keys = {
      DT_METADATA_XMP_DC_CREATOR,
      DT_METADATA_XMP_DC_DESCRIPTION,
      DT_METADATA_XMP_DC_RIGHTS,
    };
    for(const dt_metadata_t key : imported_keys)
    {
      const char *name = dt_metadata_get_name(key);
      char *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", name);
      dt_conf_set_int(setting, DT_METADATA_FLAG_IMPORTED);
      dt_free(setting);
    }
  }
  dt_image_xmp_mode_refresh_from_conf();
  dt_exif_init();
  fixture.path = nullptr;
  fd = g_file_open_tmp("ansel-test-exif-contract-XXXXXX.jpg", &fixture.path, nullptr);
  if(fd < 0) goto cleanup_exif;
  g_close(fd, nullptr);
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
  fixture.path = nullptr;
cleanup_exif:
  dt_exif_cleanup();
  dt_conf_cleanup(darktable.conf);
  dt_free(darktable.conf);
  darktable.conf = nullptr;
cleanup_config:
  g_remove(fixture.config_path);
  dt_free(fixture.config_path);
  fixture.config_path = nullptr;
cleanup_database:
  testdb_teardown(state);
cleanup_location:
  if(!IS_NULL_PTR(fixture.user_cache_dir)) g_rmdir(fixture.user_cache_dir);
  dt_free(fixture.user_cache_dir);
  fixture.user_cache_dir = nullptr;
  g_rmdir(fixture.user_config_dir);
  dt_free(fixture.user_config_dir);
  fixture.user_config_dir = nullptr;
  return -1;
}

static int teardown(void **state)
{
  auto &fixture = test_context();
  dt_metadata_set_tags_changed_handler(nullptr);
  g_cond_clear(&fixture.coordination_condition);
  g_mutex_clear(&fixture.coordination_mutex);
  dt_exif_cleanup();
  dt_conf_cleanup(darktable.conf);
  dt_free(darktable.conf);
  darktable.conf = nullptr;
  g_remove(fixture.config_path);
  dt_free(fixture.config_path);
  fixture.config_path = nullptr;
  g_remove(fixture.path);
  dt_free(fixture.path);
  fixture.path = nullptr;
  const int result = testdb_teardown(state);
  g_rmdir(fixture.user_cache_dir);
  dt_free(fixture.user_cache_dir);
  fixture.user_cache_dir = nullptr;
  g_rmdir(fixture.user_config_dir);
  dt_free(fixture.user_config_dir);
  fixture.user_config_dir = nullptr;
  return result;
}

static dt_image_t persisted_image(void)
{
  dt_image_t image;
  dt_image_init(&image);
  image.id = test_context().image_id;
  return image;
}

static void test_persisted_read_rolls_back_begin_failure(void **)
{
  auto &fixture = test_context();
  dt_image_t image = persisted_image();
  const dt_image_t original = image;
  fixture.start_outcome = dt_test_exif_transaction_outcome_t::DT_TEST_EXIF_TRANSACTION_FAIL;

  assert_int_equal(dt_test_exif_read_persisted(&image, fixture.path), 1);
  assert_memory_equal(&image, &original, sizeof(image));
  assert_int_equal(dt_tag_repository_find_by_name("contract-keyword"), 0);
  assert_int_equal(fixture.notification_calls, 0);
  assert_int_equal(fixture.rollback_calls, 0);
}

static void test_persisted_read_rejects_stale_image_before_transaction(void **)
{
  auto &fixture = test_context();
  dt_image_t image = persisted_image();
  image.id += 1000;
  const dt_image_t original = image;

  assert_int_equal(dt_test_exif_read_persisted(&image, fixture.path), 1);
  assert_memory_equal(&image, &original, sizeof(image));
  assert_int_equal(fixture.start_calls, 0);
  assert_int_equal(fixture.release_calls, 0);
  assert_int_equal(fixture.rollback_calls, 0);
  assert_int_equal(fixture.notification_calls, 0);
}

static void test_persisted_read_rejects_outer_transaction(void **)
{
  auto &fixture = test_context();
  for(int invalid_keyword = 0; invalid_keyword < 2; invalid_keyword++)
  {
    std::unique_ptr<Exiv2::Image> source(Exiv2::ImageFactory::open(fixture.path));
    source->readMetadata();
    source->xmpData()["Xmp.dc.subject"] = invalid_keyword ? std::string(1024, 'x') : "contract-keyword";
    source->writeMetadata();
    dt_image_t image = persisted_image();
    const dt_image_t original = image;
    fixture.start_calls = 0;
    fixture.release_calls = 0;
    fixture.rollback_calls = 0;
    fixture.notification_calls = 0;

    assert_true(dt_database_start_transaction_debug());
    assert_true(dt_database_start_transaction_debug());
    assert_true(dt_metadata_set_import(image.id, "Xmp.dc.description", "outer-sentinel"));
    const int result = dt_test_exif_read_persisted(&image, fixture.path);
    const gboolean owned_after_read = dt_database_transaction_is_owned_by_current_thread();
    gboolean nested_release = FALSE;
    gboolean owned_after_release = FALSE;
    gboolean committed = FALSE;
    if(owned_after_read)
    {
      nested_release = dt_database_release_transaction_debug();
      owned_after_release = dt_database_transaction_is_owned_by_current_thread();
      if(owned_after_release) committed = dt_database_release_transaction_debug();
    }
    if(dt_database_transaction_is_owned_by_current_thread()) dt_database_rollback_transaction();

    GList *description = dt_metadata_get(image.id, "Xmp.dc.description", nullptr);
    const gboolean sentinel_preserved = !IS_NULL_PTR(description)
                                       && !g_strcmp0((const char *)description->data, "outer-sentinel");
    g_list_free_full(description, dt_free_gpointer);
    assert_int_equal(result, 1);
    assert_memory_equal(&image, &original, sizeof(image));
    assert_true(owned_after_read);
    assert_true(nested_release);
    assert_true(owned_after_release);
    assert_true(committed);
    assert_true(sentinel_preserved);
    assert_int_equal(fixture.start_calls, 0);
    assert_int_equal(fixture.release_calls, 0);
    assert_int_equal(fixture.rollback_calls, 0);
    assert_int_equal(fixture.notification_calls, 0);
    assert_int_equal(dt_tag_repository_find_by_name("contract-keyword"), 0);
  }
}

static void test_persisted_read_rolls_back_commit_failure(void **)
{
  auto &fixture = test_context();
  dt_image_t image = persisted_image();
  const dt_image_t original = image;
  fixture.release_outcome = dt_test_exif_transaction_outcome_t::DT_TEST_EXIF_TRANSACTION_FAIL;

  assert_int_equal(dt_test_exif_read_persisted(&image, fixture.path), 1);
  assert_memory_equal(&image, &original, sizeof(image));
  assert_int_equal(dt_tag_repository_find_by_name("contract-keyword"), 0);
  assert_int_equal(fixture.notification_calls, 0);
  assert_int_equal(fixture.rollback_calls, 1);
  assert_false(dt_database_transaction_is_owned_by_current_thread());
}

static void test_persisted_read_rolls_back_tag_persistence_failure(void **)
{
  auto &fixture = test_context();
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
  assert_int_equal(fixture.notification_calls, 0);
  assert_int_equal(fixture.rollback_calls, 1);
  assert_false(dt_database_transaction_is_owned_by_current_thread());
}

static void test_persisted_read_commits_keyword(void **)
{
  auto &fixture = test_context();
  dt_image_t image = persisted_image();

  assert_int_equal(dt_test_exif_read_persisted(&image, fixture.path), 0);
  const guint tagid = dt_tag_repository_find_by_name("contract-keyword");
  assert_true(tagid > 0);
  assert_true(dt_tag_repository_is_attached(tagid, image.id));
  assert_int_equal(fixture.notification_calls, 1);
}

static void test_persisted_read_imports_exif_only_metadata(void **)
{
  auto &fixture = test_context();
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

  GList *creator = dt_metadata_get(image.id, "Xmp.dc.creator", nullptr);
  GList *description = dt_metadata_get(image.id, "Xmp.dc.description", nullptr);
  GList *rights = dt_metadata_get(image.id, "Xmp.dc.rights", nullptr);
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

static void test_persisted_read_rolls_back_exif_only_metadata(void **)
{
  auto &fixture = test_context();
  std::unique_ptr<Exiv2::Image> fixture_image(Exiv2::ImageFactory::open(fixture.path));
  assert_non_null(fixture_image.get());
  fixture_image->readMetadata();
  fixture_image->xmpData().clear();
  fixture_image->exifData()["Exif.Image.Artist"] = "Ansel Author";
  fixture_image->writeMetadata();

  dt_image_t image = persisted_image();
  fixture.release_outcome = dt_test_exif_transaction_outcome_t::DT_TEST_EXIF_TRANSACTION_FAIL;
  assert_int_equal(dt_test_exif_read_persisted(&image, fixture.path), 1);
  GList *creator = dt_metadata_get(image.id, "Xmp.dc.creator", nullptr);
  assert_null(creator);
  assert_int_equal(fixture.rollback_calls, 1);
  assert_int_equal(fixture.notification_calls, 0);
}

static void test_persisted_read_notifies_for_metadata_only_xmp(void **)
{
  auto &fixture = test_context();
  std::unique_ptr<Exiv2::Image> fixture_image(Exiv2::ImageFactory::open(fixture.path));
  assert_non_null(fixture_image.get());
  fixture_image->readMetadata();
  fixture_image->xmpData().clear();
  fixture_image->xmpData()["Xmp.dc.description"] = "metadata-only";
  fixture_image->writeMetadata();

  dt_image_t image = persisted_image();
  assert_int_equal(dt_test_exif_read_persisted(&image, fixture.path), 0);
  GList *description = dt_metadata_get(image.id, "Xmp.dc.description", nullptr);
  assert_non_null(description);
  assert_string_equal((const char *)description->data, "metadata-only");
  assert_int_equal(fixture.notification_calls, 1);
  g_list_free_full(description, dt_free_gpointer);
}

static void test_persisted_read_notifies_for_iptc_caption_only(void **)
{
  auto &fixture = test_context();
  std::unique_ptr<Exiv2::Image> fixture_image(Exiv2::ImageFactory::open(fixture.path));
  assert_non_null(fixture_image.get());
  fixture_image->readMetadata();
  fixture_image->xmpData().clear();
  fixture_image->iptcData().clear();
  fixture_image->iptcData()["Iptc.Application2.Caption"] = "iptc-caption-only";
  fixture_image->writeMetadata();

  dt_image_t image = persisted_image();
  assert_int_equal(dt_test_exif_read_persisted(&image, fixture.path), 0);
  GList *description = dt_metadata_get(image.id, "Xmp.dc.description", nullptr);
  assert_non_null(description);
  assert_string_equal((const char *)description->data, "iptc-caption-only");
  assert_int_equal(fixture.notification_calls, 1);
  g_list_free_full(description, dt_free_gpointer);
}

static void test_xmp_persistence_marks_metadata_replacement(void **)
{
  auto &fixture = test_context();
  dt_image_t image = persisted_image();
  assert_true(dt_metadata_set_import(image.id, "Xmp.dc.description", "existing"));
  Exiv2::XmpData xmp_data;
  bool tags_changed = false;

  assert_true(dt_test_exif_start_transaction());
  assert_true(dt_test_exif_apply_xmp_persistence(&image, xmp_data, -1, true, &tags_changed));
  assert_true(tags_changed);
  assert_true(dt_test_exif_release_transaction());
  GList *description = dt_metadata_get(image.id, "Xmp.dc.description", nullptr);
  assert_null(description);
  assert_int_equal(fixture.notification_calls, 0);
}

static void test_generic_read_keeps_scratch_fields_without_persistence(void **)
{
  auto &fixture = test_context();
  dt_image_t image;
  dt_image_init(&image);

  assert_int_equal(dt_test_exif_read(&image, fixture.path), 0);
  assert_string_equal(image.exif_maker, "AnselFixture");
  assert_string_equal(image.exif_model, "Scratch");
  std::array<char, DT_DATETIME_EXIF_LENGTH> datetime = {};
  assert_true(dt_datetime_img_to_exif(datetime.data(), datetime.size(), &image));
  assert_memory_equal(datetime.data(), "2012", 4);
  dt_variables_params_t *params = nullptr;
  dt_variables_params_init(&params);
  params->img = &image;
  std::string pattern = "$(EXIF.YEAR)";
  char *year = dt_variables_expand(params, pattern.data(), FALSE);
  assert_string_equal(year, "2012");
  dt_free(year);
  dt_variables_params_destroy(params);
  assert_int_equal(fixture.start_calls, 0);
  assert_int_equal(fixture.release_calls, 0);
  assert_int_equal(fixture.rollback_calls, 0);
  assert_int_equal(dt_tag_repository_find_by_name("contract-keyword"), 0);
  assert_int_equal(fixture.notification_calls, 0);
}

static void test_generic_read_keeps_persisted_image_database_free(void **)
{
  auto &fixture = test_context();
  dt_image_t image = persisted_image();

  assert_int_equal(dt_test_exif_read(&image, fixture.path), 0);
  assert_string_equal(image.exif_maker, "AnselFixture");
  assert_int_equal(fixture.start_calls, 0);
  assert_int_equal(fixture.release_calls, 0);
  assert_int_equal(fixture.rollback_calls, 0);
  assert_int_equal(dt_tag_repository_find_by_name("contract-keyword"), 0);
  assert_int_equal(fixture.notification_calls, 0);
}

static void test_generic_read_keeps_iptc_and_xmp_image_fields(void **)
{
  auto &fixture = test_context();
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
  std::array<char, DT_DATETIME_EXIF_LENGTH> datetime = {};
  assert_true(dt_datetime_img_to_exif(datetime.data(), datetime.size(), &image));
  assert_memory_equal(datetime.data(), "2011", 4);
  assert_float_equal(image.geoloc.elevation, 123.0, 1e-6);
  assert_string_equal(image.exif_lens, "Lens Fixture");
  assert_int_equal(fixture.start_calls, 0);
  assert_int_equal(fixture.release_calls, 0);
  assert_int_equal(fixture.rollback_calls, 0);
  assert_int_equal(fixture.notification_calls, 0);
}

static void test_generic_read_does_not_rollback_unrelated_write(void **)
{
  auto &fixture = test_context();
  fixture.coordinate_transaction = TRUE;
  for(int iteration = 0; iteration < 100; iteration++)
  {
    fixture.reader_reached_datetime = FALSE;
    fixture.writer_may_write = FALSE;
    fixture.writer_finished = FALSE;
    dt_image_t image;
    dt_image_init(&image);
    GThread *writer = g_thread_new("exif-writer", write_unrelated_image, GINT_TO_POINTER(iteration));
    GThread *reader = g_thread_new("exif-reader", read_scratch_image, &image);

    g_mutex_lock(&fixture.coordination_mutex);
    while(!fixture.reader_reached_datetime) g_cond_wait(&fixture.coordination_condition, &fixture.coordination_mutex);
    g_mutex_unlock(&fixture.coordination_mutex);
    g_thread_join(reader);

    g_mutex_lock(&fixture.coordination_mutex);
    fixture.writer_may_write = TRUE;
    g_cond_broadcast(&fixture.coordination_condition);
    while(!fixture.writer_finished) g_cond_wait(&fixture.coordination_condition, &fixture.coordination_mutex);
    g_mutex_unlock(&fixture.coordination_mutex);
    g_thread_join(writer);

    char *filename = g_strdup_printf("unrelated-%d.jpg", iteration);
    assert_true(dt_image_repository_find_by_film_and_filename(fixture.film_id, filename) > UNKNOWN_IMAGE);
    dt_free(filename);
  }
  assert_int_equal(fixture.start_calls, 0);
  assert_int_equal(fixture.release_calls, 0);
  assert_int_equal(fixture.rollback_calls, 0);
  assert_int_equal(fixture.notification_calls, 0);
  fixture.coordinate_transaction = FALSE;
}

static void test_generic_read_retains_pfm_fallbacks(void **)
{
  auto &fixture = test_context();
  char *path = nullptr;
  const int fd = g_file_open_tmp("ansel-test-exif-fallback-XXXXXX.pfm", &path, nullptr);
  assert_true(fd >= 0);
  g_close(fd, nullptr);
  assert_true(g_file_set_contents(path, "PF\n1 1\n-1.0\n\0\0\0\0", 16, nullptr));
  const time_t timestamp = 981173106;
  struct utimbuf times = { .actime = timestamp, .modtime = timestamp };
  assert_int_equal(g_utime(path, &times), 0);

  dt_image_t image;
  dt_image_t expected;
  dt_image_init(&image);
  dt_image_init(&expected);
  dt_datetime_unix_to_img(&expected, &timestamp);
  assert_int_equal(dt_test_exif_read(&image, path), 1);
  assert_true(image.flags & DT_IMAGE_HDR);
  assert_int_equal(image.exif_datetime_taken, expected.exif_datetime_taken);
  assert_int_equal(fixture.start_calls, 0);
  assert_int_equal(fixture.release_calls, 0);
  assert_int_equal(fixture.rollback_calls, 0);
  g_remove(path);
  dt_free(path);
}

static void test_generic_read_retains_fallbacks_when_file_disappears_after_stat(void **)
{
  auto &fixture = test_context();
  char *path = nullptr;
  const int fd = g_file_open_tmp("ansel-test-exif-unlink-XXXXXX.pfm", &path, nullptr);
  assert_true(fd >= 0);
  g_close(fd, nullptr);
  assert_true(g_file_set_contents(path, "PF\n1 1\n-1.0\n\0\0\0\0", 16, nullptr));
  const time_t timestamp = 981173106;
  struct utimbuf times = { .actime = timestamp, .modtime = timestamp };
  assert_int_equal(g_utime(path, &times), 0);

  dt_image_t image;
  dt_image_t expected;
  dt_image_init(&image);
  dt_image_init(&expected);
  dt_datetime_unix_to_img(&expected, &timestamp);
  fixture.unlink_after_datetime_path = path;
  assert_int_equal(dt_test_exif_read(&image, path), 1);
  assert_null(fixture.unlink_after_datetime_path);
  assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
  assert_true(image.flags & DT_IMAGE_HDR);
  assert_int_equal(image.exif_datetime_taken, expected.exif_datetime_taken);
  assert_int_equal(fixture.start_calls, 0);
  assert_int_equal(fixture.release_calls, 0);
  assert_int_equal(fixture.rollback_calls, 0);
  dt_free(path);
}

static void test_scratch_exif_preserves_camera_fields(void **)
{
  dt_image_t image;
  dt_image_init(&image);
  Exiv2::ExifData data;
  data["Exif.Image.Make"] = "Canon";
  data["Exif.Image.Model"] = "Fixture";
  data["Exif.Photo.ExposureTime"] = Exiv2::URational(1, 125);
  data["Exif.Photo.FNumber"] = Exiv2::URational(4, 1);
  data["Exif.Photo.ISOSpeedRatings"] = uint16_t(200);
  data["Exif.Photo.FocalLength"] = Exiv2::URational(50, 1);
  data["Exif.Photo.FocalLengthIn35mmFilm"] = uint16_t(75);
  data["Exif.CanonFi.FocusDistanceUpper"] = uint16_t(200);
  data["Exif.CanonFi.FocusDistanceLower"] = uint16_t(100);
  data["Exif.Image.Orientation"] = uint16_t(6);
  data["Exif.GPSInfo.GPSLatitude"] = "45/1 30/1 0/1";
  data["Exif.GPSInfo.GPSLatitudeRef"] = "N";
  data["Exif.GPSInfo.GPSLongitude"] = "10/1 0/1 0/1";
  data["Exif.GPSInfo.GPSLongitudeRef"] = "E";
  data["Exif.GPSInfo.GPSAltitude"] = Exiv2::URational(12, 1);
  data["Exif.GPSInfo.GPSAltitudeRef"] = uint8_t(0);
  data["Exif.Photo.LensModel"] = "Fixture lens";
  data["Exif.Image.ColorMatrix1"] = "1/1 0/1 0/1 0/1 1/1 0/1 0/1 0/1 1/1";
  data["Exif.Image.CalibrationIlluminant1"] = uint16_t(DT_LS_D65);

  assert_true(_exif_decode_exif_data(&image, data, false));
  assert_float_equal(image.exif_exposure, 1.0 / 125.0, 1e-6);
  assert_float_equal(image.exif_aperture, 4.0, 1e-6);
  assert_float_equal(image.exif_iso, 200.0, 1e-6);
  assert_float_equal(image.exif_focal_length, 50.0, 1e-6);
  assert_float_equal(image.exif_crop, 1.5, 1e-6);
  assert_float_equal(image.exif_focus_distance, 1.5, 1e-6);
  assert_int_equal(image.orientation, dt_image_orientation_to_flip_bits(6));
  assert_float_equal(image.geoloc.latitude, 45.5, 1e-6);
  assert_float_equal(image.geoloc.longitude, 10.0, 1e-6);
  assert_float_equal(image.geoloc.elevation, 12.0, 1e-6);
  assert_string_equal(image.exif_lens, "Fixture lens");
  for(int i = 0; i < 9; i++)
  {
    const float expected = i % 4 == 0 ? 1.0f : 0.0f;
    assert_float_equal(image.d65_color_matrix[i], expected, 1e-6);
  }
  assert_int_equal(test_context().start_calls, 0);
}

int main(void)
{
  auto &fixture = test_context();
  fixture.rawspeed_datadir = g_dir_make_tmp("ansel-test-exif-data-XXXXXX", nullptr);
  if(IS_NULL_PTR(fixture.rawspeed_datadir)) return 1;
  char *rawspeed_dir = g_build_filename(fixture.rawspeed_datadir, "rawspeed", nullptr);
  char *cameras
      = g_build_filename(ANSEL_TEST_SOURCE_DIR, "src", "external", "rawspeed", "data", "cameras.xml", nullptr);
  const gboolean prepared = g_mkdir(rawspeed_dir, 0700) == 0 && g_file_test(cameras, G_FILE_TEST_IS_REGULAR);
  GFile *source = prepared ? g_file_new_for_path(cameras) : nullptr;
  char *destination_path = g_build_filename(rawspeed_dir, "cameras.xml", nullptr);
  GFile *destination = prepared ? g_file_new_for_path(destination_path) : nullptr;
  const gboolean copied
      = prepared && g_file_copy(source, destination, G_FILE_COPY_OVERWRITE, nullptr, nullptr, nullptr, nullptr);
  if(!IS_NULL_PTR(destination)) g_object_unref(destination);
  if(!IS_NULL_PTR(source)) g_object_unref(source);
  dt_free(cameras);
  if(copied) dt_loc_init_datadir(nullptr, fixture.rawspeed_datadir);
  const std::array<CMUnitTest, 18> tests = { {
      cmocka_unit_test_setup_teardown(test_scratch_exif_preserves_camera_fields, setup, teardown),
      cmocka_unit_test_setup_teardown(test_persisted_read_rolls_back_begin_failure, setup, teardown),
      cmocka_unit_test_setup_teardown(test_persisted_read_rejects_stale_image_before_transaction, setup, teardown),
      cmocka_unit_test_setup_teardown(test_persisted_read_rejects_outer_transaction, setup, teardown),
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
      cmocka_unit_test_setup_teardown(test_generic_read_retains_fallbacks_when_file_disappears_after_stat, setup,
                                      teardown),
  } };

  const int result
      = copied ? _cmocka_run_group_tests("exif_read_contract", tests.data(), tests.size(), nullptr, nullptr) : 1;
  g_remove(destination_path);
  g_rmdir(rawspeed_dir);
  g_rmdir(fixture.rawspeed_datadir);
  dt_free(destination_path);
  dt_free(rawspeed_dir);
  dt_free(fixture.rawspeed_datadir);
  fixture.rawspeed_datadir = nullptr;
  return result;
}
