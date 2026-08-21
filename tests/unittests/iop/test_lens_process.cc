#include <cstring>
#include <cmath>
#include <glib.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>
#include <lensfun.h>
#include "darktable.h"
#include "test_develop_context.h"

extern "C" dt_pthread_mutex_t *dt_plugin_threadsafe_mutex(void)
{
  return &darktable.plugin_threadsafe;
}

extern "C" dt_dev_pixelpipe_cache_t *dt_pixelpipe_cache_get_global(void)
{
  return darktable.pixelpipe_cache;
}

extern "C" int dt_get_num_openmp_threads(void)
{
  return darktable.num_openmp_threads;
}

enum test_lens_process_fixture_t { TEST_LENS_PROCESS_EMBEDDED_ONLY, TEST_LENS_PROCESS_EMBEDDED_VIGNETTE_ONLY, TEST_LENS_PROCESS_LENSFUN_ONLY, TEST_LENS_PROCESS_IDENTITY, TEST_LENS_PROCESS_MIXED, TEST_LENS_PROCESS_MIXED_TCA_SUPPRESSED, TEST_LENS_PROCESS_MIXED_NO_TCA, TEST_LENS_PROCESS_MIXED_TCA_COORDINATE_ALLOCATION_FAILURE, TEST_LENS_PROCESS_MIXED_TCA_OUTPUT_ALLOCATION_FAILURE };
typedef struct { float pixels[32 * 32 * 4]; size_t pixels_count; int roi_in[4]; int roi_out[4]; int tca_roi[4]; char trace[128]; gboolean normal_alpha_initialized; gboolean alpha_copy_contained; gboolean fallback_used; } test_lens_process_result_t;
extern "C" int test_lens_process_characterize(test_lens_process_fixture_t, dt_develop_t *, dt_dev_pixelpipe_t *, lfDatabase *, const dt_iop_roi_t *, int, gboolean, int, test_lens_process_result_t *);
extern "C" size_t test_lens_process_modifier_deletions(void);
extern "C" size_t test_lens_process_tca_modifier_deletions(void);

typedef struct { dt_test_develop_context_t context; lfDatabase database; lfDatabase no_tca_database; const lfLens *lens; char *fixture; char *no_tca_fixture; } fixture_t;

static int setup(void **state)
{
  fixture_t *fixture = new fixture_t();
  char *directory = NULL;
  const lfCamera **camera = NULL;
  const lfLens **lenses = NULL;
  if(dt_test_develop_context_init(&fixture->context)) goto error;
  directory = g_path_get_dirname(__FILE__);
  fixture->fixture = g_build_filename(directory, "fixtures", "lensfun-test.xml", NULL);
  fixture->no_tca_fixture = g_build_filename(directory, "fixtures", "lensfun-no-tca-test.xml", NULL);
  g_free(directory);
  directory = NULL;
  if(fixture->database.Load(fixture->fixture) != LF_NO_ERROR) goto error;
  if(fixture->no_tca_database.Load(fixture->no_tca_fixture) != LF_NO_ERROR) goto error;
  camera = fixture->database.FindCamerasExt("Ansel test", "Camera", 0);
  lenses = camera ? fixture->database.FindLenses(camera[0], "Ansel test", "Lens", 0) : NULL;
  fixture->lens = lenses ? lenses[0] : NULL;
  if(!fixture->lens) goto error;
  lf_free(lenses);
  lenses = NULL;
  lf_free(camera);
  camera = NULL;
  *state = fixture;
  return 0;

error:
  lf_free(lenses);
  lf_free(camera);
  g_free(directory);
  g_free(fixture->no_tca_fixture);
  g_free(fixture->fixture);
  dt_test_develop_context_cleanup(&fixture->context);
  delete fixture;
  return -1;
}

static int teardown(void **state)
{
  fixture_t *fixture = static_cast<fixture_t *>(*state);
  if(IS_NULL_PTR(fixture)) return 0;
  g_free(fixture->no_tca_fixture);
  g_free(fixture->fixture);
  dt_test_develop_context_cleanup(&fixture->context);
  delete fixture;
  return 0;
}

static GKeyFile *load_golden(void)
{
  char *directory = g_path_get_dirname(__FILE__);
  char *path = g_build_filename(directory, "golden", "lens_process.sha256", NULL);
  GKeyFile *golden = g_key_file_new();
  GError *error = NULL;
  const gboolean loaded = g_key_file_load_from_file(golden, path, G_KEY_FILE_NONE, &error);
  g_free(path);
  g_free(directory);
  assert_true(loaded);
  assert_null(error);
  return golden;
}

static void characterize(fixture_t *fixture, test_lens_process_fixture_t kind, const dt_iop_roi_t *roi,
                         const int channels, const gboolean monochrome, const int mask_display,
                         test_lens_process_result_t *result)
{
  lfDatabase *database = kind == TEST_LENS_PROCESS_MIXED_NO_TCA ? &fixture->no_tca_database
                                                                  : &fixture->database;
  assert_int_equal(test_lens_process_characterize(kind, &fixture->context.develop, &fixture->context.pipe,
                                                    database, roi, channels, monochrome,
                                                    mask_display, result), 0);
}

#ifndef TEST_LENS_PROCESS_EXPECT_MUTATION
static void run_fixture(void **state, const char *name, test_lens_process_fixture_t kind)
{
  test_lens_process_result_t result = {};
  fixture_t *fixture = static_cast<fixture_t *>(*state);
  const dt_iop_roi_t roi = { 5, 7, 11, 9, 1.0f };
   characterize(fixture, kind, &roi, 4, FALSE, 0, &result);
  assert_int_equal(result.pixels_count, 11 * 9 * 4);
  gchar *digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256, reinterpret_cast<const guchar *>(result.pixels), result.pixels_count * sizeof(*result.pixels));
  assert_non_null(digest);
  GKeyFile *golden = load_golden();
  gchar *expected_digest = g_key_file_get_string(golden, name, "sha256", NULL);
  gchar *expected_trace = g_key_file_get_string(golden, name, "trace", NULL);
  gint *expected_roi_in = g_key_file_get_integer_list(golden, name, "roi_in", NULL, NULL);
  gint *expected_roi_out = g_key_file_get_integer_list(golden, name, "roi_out", NULL, NULL);
  gint *expected_tca_roi = g_key_file_get_integer_list(golden, name, "tca_roi", NULL, NULL);
  assert_string_equal(digest, expected_digest);
  assert_string_equal(result.trace, expected_trace);
  assert_memory_equal(result.roi_in, expected_roi_in, sizeof(result.roi_in));
  assert_memory_equal(result.roi_out, expected_roi_out, sizeof(result.roi_out));
  if(expected_tca_roi) assert_memory_equal(result.tca_roi, expected_tca_roi, sizeof(result.tca_roi));
  assert_false(result.fallback_used);
  if(strcmp(name, "mixed") == 0)
  {
    const int expected_output[] = { 5, 7, 11, 9 };
    const int expected_tca[] = { 4, 6, 14, 12 };
    const int expected_input[] = { 2, 4, 18, 16 };
    assert_memory_equal(result.roi_out, expected_output, sizeof(expected_output));
    assert_memory_equal(result.tca_roi, expected_tca, sizeof(expected_tca));
    assert_memory_equal(result.roi_in, expected_input, sizeof(expected_input));
  }
  g_free(expected_tca_roi);
  g_free(expected_roi_out);
  g_free(expected_roi_in);
  g_free(expected_trace);
  g_free(expected_digest);
  g_key_file_free(golden);
  g_free(digest);
}

static void test_identity(void **state) { run_fixture(state, "identity", TEST_LENS_PROCESS_IDENTITY); }
static void test_embedded(void **state) { run_fixture(state, "embedded", TEST_LENS_PROCESS_EMBEDDED_ONLY); }
static void test_lensfun(void **state) { run_fixture(state, "lensfun", TEST_LENS_PROCESS_LENSFUN_ONLY); }
static void test_mixed(void **state) { run_fixture(state, "mixed", TEST_LENS_PROCESS_MIXED); }

static void test_nonzero_tile_matches_full_frame(void **state)
{
  fixture_t *fixture = static_cast<fixture_t *>(*state);
  const int full_image[] = { 0, 0, 32, 32 };
  const dt_iop_roi_t bounded_roi = { 11, 5, 9, 8, 1.0f };
  test_lens_process_result_t bounded = {};
  characterize(fixture, TEST_LENS_PROCESS_MIXED, &bounded_roi, 4, FALSE, 0, &bounded);
   assert_memory_not_equal(bounded.roi_in, full_image, sizeof(full_image));
   assert_memory_not_equal(bounded.tca_roi, full_image, sizeof(full_image));
   assert_true(bounded.alpha_copy_contained);
   const float scales[] = { 1.0f, 0.5f };
   for(size_t scale_index = 0; scale_index < G_N_ELEMENTS(scales); scale_index++)
   {
    const int image_size = (int)(32 * scales[scale_index]);
    const dt_iop_roi_t full_roi = { 0, 0, image_size, image_size, scales[scale_index] };
    test_lens_process_result_t full = {};
    characterize(fixture, TEST_LENS_PROCESS_MIXED, &full_roi, 4, FALSE, 0, &full);
    const dt_iop_roi_t tiles[] = { { image_size / 3, image_size / 3, 6, 6, scales[scale_index] },
                                   { image_size / 3, 0, 6, 5, scales[scale_index] },
                                   { image_size / 3, image_size - 5, 6, 5, scales[scale_index] },
                                   { 0, image_size / 3, 5, 6, scales[scale_index] },
                                   { image_size - 5, image_size / 3, 5, 6, scales[scale_index] } };
    for(size_t i = 0; i < G_N_ELEMENTS(tiles); i++)
    {
      const dt_iop_roi_t &tile_roi = tiles[i];
      test_lens_process_result_t tile = {};
      test_lens_process_result_t repeat = {};
       characterize(fixture, TEST_LENS_PROCESS_MIXED, &tile_roi, 4, FALSE, 0, &tile);
       characterize(fixture, TEST_LENS_PROCESS_MIXED, &tile_roi, 4, FALSE, 0, &repeat);
      assert_int_equal(tile.pixels_count, (size_t)tile_roi.width * tile_roi.height * 4);
      assert_memory_equal(tile.roi_out, &tile_roi.x, sizeof(tile.roi_out));
       assert_string_equal(tile.trace, "copy -> Lensfun vignette -> TCA-only remap -> embedded remap");
       assert_true(tile.alpha_copy_contained);
       assert_false(tile.fallback_used);
       assert_memory_equal(&tile, &repeat, sizeof(tile));
      for(int y = 0; y < tile_roi.height; y++)
        for(int x = 0; x < tile_roi.width; x++)
        {
          const float tiled_alpha = tile.pixels[((size_t)y * tile_roi.width + x) * 4 + 3];
          const float global_alpha = full.pixels[((size_t)(y + tile_roi.y) * full_roi.width + tile_roi.x + x) * 4 + 3];
          assert_true(isnan(tiled_alpha) == isnan(global_alpha));
          assert_true(isinf(tiled_alpha) == isinf(global_alpha));
          assert_true(signbit(tiled_alpha) == signbit(global_alpha));
          assert_memory_equal(&tiled_alpha, &global_alpha, sizeof(tiled_alpha));
          for(int c = 0; c < 3; c++)
          {
            const float tiled = tile.pixels[((size_t)y * tile_roi.width + x) * 4 + c];
            const float global = full.pixels[((size_t)(y + tile_roi.y) * full_roi.width + tile_roi.x + x) * 4 + c];
            assert_true(isnan(tiled) == isnan(global));
            assert_true(isinf(tiled) == isinf(global));
            assert_true(signbit(tiled) == signbit(global));
             if(isfinite(tiled) && isfinite(global))
            {
              uint32_t tiled_bits;
              uint32_t global_bits;
              memcpy(&tiled_bits, &tiled, sizeof(tiled_bits));
              memcpy(&global_bits, &global, sizeof(global_bits));
              tiled_bits = tiled_bits & 0x80000000U ? ~tiled_bits + 1U : tiled_bits | 0x80000000U;
              global_bits = global_bits & 0x80000000U ? ~global_bits + 1U : global_bits | 0x80000000U;
                const uint32_t limit = scales[scale_index] == 1.0f ? 5U : 16U;
                assert_true(tiled_bits > global_bits ? tiled_bits - global_bits <= limit : global_bits - tiled_bits <= limit);
            }
            else assert_memory_equal(&tiled, &global, sizeof(tiled));
         }
    }
   }
 }
}

static void test_rgb_rgba_mask_and_alpha(void **state)
{
  fixture_t *fixture = static_cast<fixture_t *>(*state);
  const dt_iop_roi_t roi = { 5, 7, 11, 9, 1.0f };
  test_lens_process_result_t rgb = {};
  test_lens_process_result_t rgba = {};
  characterize(fixture, TEST_LENS_PROCESS_EMBEDDED_ONLY, &roi, 3, FALSE, 0, &rgb);
  characterize(fixture, TEST_LENS_PROCESS_EMBEDDED_ONLY, &roi, 4, FALSE, DT_DEV_PIXELPIPE_DISPLAY_MASK, &rgba);
  assert_int_equal(rgb.pixels_count, 11 * 9 * 3);
  assert_int_equal(rgba.pixels_count, 11 * 9 * 4);
  for(size_t k = 0; k < rgba.pixels_count / 4; k++) assert_true(isfinite(rgba.pixels[4 * k + 3]));
}

static void test_embedded_vignette_preserves_alpha(void **state)
{
  fixture_t *fixture = static_cast<fixture_t *>(*state);
  const dt_iop_roi_t roi = { 5, 7, 11, 9, 1.0f };
  test_lens_process_result_t result = {};
  characterize(fixture, TEST_LENS_PROCESS_EMBEDDED_VIGNETTE_ONLY, &roi, 4, FALSE, 0, &result);
  for(int y = 0; y < roi.height; y++)
    for(int x = 0; x < roi.width; x++)
    {
      const float expected = (float)(((roi.y + y) * 32 + roi.x + x + 3 * 13) % 251) / 251.0f;
      assert_memory_equal(&result.pixels[((size_t)y * roi.width + x) * 4 + 3], &expected, sizeof(expected));
    }
}

static void test_tca_recovery_paths(void **state);

static void test_mixed_normal_alpha_is_initialized(void **state)
{
  fixture_t *fixture = static_cast<fixture_t *>(*state);
  const dt_iop_roi_t roi = { 5, 7, 11, 9, 1.0f };
  test_lens_process_result_t mixed = {};
  test_lens_process_result_t mask = {};
  test_lens_process_result_t tca_suppressed = {};
  characterize(fixture, TEST_LENS_PROCESS_MIXED, &roi, 4, FALSE, 0, &mixed);
  characterize(fixture, TEST_LENS_PROCESS_MIXED, &roi, 4, FALSE, DT_DEV_PIXELPIPE_DISPLAY_MASK, &mask);
  characterize(fixture, TEST_LENS_PROCESS_MIXED_TCA_SUPPRESSED, &roi, 4, FALSE, 0, &tca_suppressed);
  assert_string_equal(mixed.trace, "copy -> Lensfun vignette -> TCA-only remap -> embedded remap");
  assert_string_equal(mask.trace, "copy -> Lensfun vignette -> TCA-only remap -> embedded remap");
  assert_string_equal(tca_suppressed.trace, "copy -> Lensfun vignette -> embedded remap");
  assert_true(mixed.normal_alpha_initialized);
  GKeyFile *golden = load_golden();
  gchar *normal_digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                                      reinterpret_cast<const guchar *>(mixed.pixels),
                                                      mixed.pixels_count * sizeof(*mixed.pixels));
  gchar *mask_digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                                    reinterpret_cast<const guchar *>(mask.pixels),
                                                    mask.pixels_count * sizeof(*mask.pixels));
  gchar *expected_normal = g_key_file_get_string(golden, "mixed", "sha256", NULL);
  gchar *expected_mask = g_key_file_get_string(golden, "mixed_mask", "sha256", NULL);
  assert_string_equal(normal_digest, expected_normal);
  assert_string_equal(mask_digest, expected_mask);
  g_free(expected_mask);
  g_free(expected_normal);
  g_free(mask_digest);
  g_free(normal_digest);
  g_key_file_free(golden);
  test_tca_recovery_paths(state);
}

static void test_tca_recovery_paths(void **state)
{
  fixture_t *fixture = static_cast<fixture_t *>(*state);
  const dt_iop_roi_t roi = { 5, 7, 11, 9, 1.0f };
  const test_lens_process_fixture_t kinds[] = {
    TEST_LENS_PROCESS_MIXED_NO_TCA,
    TEST_LENS_PROCESS_MIXED_TCA_COORDINATE_ALLOCATION_FAILURE,
    TEST_LENS_PROCESS_MIXED_TCA_OUTPUT_ALLOCATION_FAILURE
  };
  const char *sections[] = { "recovery_no_tca", "recovery_coordinate_failure", "recovery_output_failure" };
  const char *traces[] = {
    "copy -> Lensfun vignette -> embedded remap",
    "copy -> Lensfun vignette -> embedded remap -> remaining Lensfun remap",
    "copy -> Lensfun vignette -> embedded remap -> remaining Lensfun remap"
  };
  const gboolean fallback_used[] = { TRUE, FALSE, FALSE };
  GKeyFile *golden = load_golden();
  for(size_t i = 0; i < G_N_ELEMENTS(kinds); i++)
  {
    test_lens_process_result_t result = {};
    characterize(fixture, kinds[i], &roi, 4, FALSE, 0, &result);
    gchar *digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                                reinterpret_cast<const guchar *>(result.pixels),
                                                result.pixels_count * sizeof(*result.pixels));
    gchar *expected = g_key_file_get_string(golden, sections[i], "sha256", NULL);
    assert_string_equal(result.trace, traces[i]);
    assert_string_equal(digest, expected);
    assert_int_equal(result.fallback_used, fallback_used[i]);
    g_free(expected);
    g_free(digest);
  }
  g_key_file_free(golden);
}

static void test_modifier_lifetime_exactly_once(void **state)
{
  fixture_t *fixture = static_cast<fixture_t *>(*state);
  const dt_iop_roi_t roi = { 5, 7, 11, 9, 1.0f };
  const test_lens_process_fixture_t kinds[] = {
    TEST_LENS_PROCESS_IDENTITY,
    TEST_LENS_PROCESS_EMBEDDED_ONLY,
    TEST_LENS_PROCESS_EMBEDDED_VIGNETTE_ONLY,
    TEST_LENS_PROCESS_LENSFUN_ONLY,
    TEST_LENS_PROCESS_MIXED,
    TEST_LENS_PROCESS_MIXED_TCA_SUPPRESSED,
    TEST_LENS_PROCESS_MIXED_NO_TCA,
    TEST_LENS_PROCESS_MIXED_TCA_COORDINATE_ALLOCATION_FAILURE,
    TEST_LENS_PROCESS_MIXED_TCA_OUTPUT_ALLOCATION_FAILURE
  };
  const size_t expected_modifier_deletions[] = { 0, 0, 0, 1, 1, 1, 1, 1, 1 };
  const size_t expected_tca_deletions[] = { 0, 0, 0, 0, 1, 0, 1, 1, 1 };
  for(size_t i = 0; i < G_N_ELEMENTS(kinds); i++)
  {
    test_lens_process_result_t result = {};
    characterize(fixture, kinds[i], &roi, 4, FALSE, 0, &result);
    assert_int_equal(test_lens_process_modifier_deletions(), expected_modifier_deletions[i]);
    assert_int_equal(test_lens_process_tca_modifier_deletions(), expected_tca_deletions[i]);
  }
}

static void test_monochrome_skips_lensfun_tca(void **state)
{
  fixture_t *fixture = static_cast<fixture_t *>(*state);
  const dt_iop_roi_t roi = { 5, 7, 11, 9, 1.0f };
  test_lens_process_result_t color = {};
  test_lens_process_result_t mono = {};
  characterize(fixture, TEST_LENS_PROCESS_LENSFUN_ONLY, &roi, 4, FALSE, 0, &color);
  characterize(fixture, TEST_LENS_PROCESS_LENSFUN_ONLY, &roi, 4, TRUE, 0, &mono);
  assert_int_not_equal(memcmp(color.pixels, mono.pixels, color.pixels_count * sizeof(float)), 0);
  assert_string_equal(mono.trace, "Lensfun vignette -> remaining Lensfun remap");
  for(size_t k = 0; k < mono.pixels_count / 4; k++)
  {
    assert_memory_equal(&mono.pixels[4 * k], &mono.pixels[4 * k + 1], sizeof(float));
    assert_memory_equal(&mono.pixels[4 * k + 1], &mono.pixels[4 * k + 2], sizeof(float));
  }
}

#endif

#if defined(TEST_LENS_PROCESS_EXPECT_MUTATION) && defined(LENS_PROCESS_MUTATE_TCA_GATE)
static void test_tca_gate_mutation_removes_tca_remap(void **state)
{
  fixture_t *fixture = static_cast<fixture_t *>(*state);
  const dt_iop_roi_t roi = { 5, 7, 11, 9, 1.0f };
  test_lens_process_result_t result = {};
  characterize(fixture, TEST_LENS_PROCESS_MIXED, &roi, 4, FALSE, 0, &result);
  gchar *digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256, reinterpret_cast<const guchar *>(result.pixels), result.pixels_count * sizeof(*result.pixels));
  GKeyFile *golden = load_golden();
  gchar *expected_digest = g_key_file_get_string(golden, "mixed", "sha256", NULL);
  gchar *expected_trace = g_key_file_get_string(golden, "mixed", "trace", NULL);
  assert_string_not_equal(result.trace, expected_trace);
  assert_string_equal(result.trace,
                      "copy -> Lensfun vignette -> embedded remap -> remaining Lensfun remap");
  assert_string_not_equal(digest, expected_digest);
  g_free(expected_trace);
  g_free(expected_digest);
  g_key_file_free(golden);
  g_free(digest);
}
#endif

#if defined(TEST_LENS_PROCESS_EXPECT_MUTATION) && defined(LENS_PROCESS_MUTATE_RB_ROUTE)
static void test_rb_route_mutation_changes_pixels_without_reordering(void **state)
{
  fixture_t *fixture = static_cast<fixture_t *>(*state);
  const dt_iop_roi_t roi = { 5, 7, 11, 9, 1.0f };
  test_lens_process_result_t result = {};
  characterize(fixture, TEST_LENS_PROCESS_MIXED, &roi, 4, FALSE, 0, &result);
  gchar *digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                              reinterpret_cast<const guchar *>(result.pixels),
                                              result.pixels_count * sizeof(*result.pixels));
  GKeyFile *golden = load_golden();
  gchar *expected_digest = g_key_file_get_string(golden, "mixed", "sha256", NULL);
  gchar *expected_trace = g_key_file_get_string(golden, "mixed", "trace", NULL);
  assert_string_equal(result.trace, expected_trace);
  assert_string_not_equal(digest, expected_digest);
  g_free(expected_trace);
  g_free(expected_digest);
  g_key_file_free(golden);
  g_free(digest);
}
#endif

int main(void)
{
#ifdef TEST_LENS_PROCESS_EXPECT_MUTATION
#ifdef LENS_PROCESS_MUTATE_TCA_GATE
  const CMUnitTest tests[] = { cmocka_unit_test(test_tca_gate_mutation_removes_tca_remap) };
#else
  const CMUnitTest tests[] = { cmocka_unit_test(test_rb_route_mutation_changes_pixels_without_reordering) };
#endif
#else
  const CMUnitTest tests[] = { cmocka_unit_test(test_identity), cmocka_unit_test(test_embedded), cmocka_unit_test(test_lensfun), cmocka_unit_test(test_mixed), cmocka_unit_test(test_nonzero_tile_matches_full_frame), cmocka_unit_test(test_rgb_rgba_mask_and_alpha), cmocka_unit_test(test_embedded_vignette_preserves_alpha), cmocka_unit_test(test_mixed_normal_alpha_is_initialized), cmocka_unit_test(test_monochrome_skips_lensfun_tca), cmocka_unit_test(test_modifier_lifetime_exactly_once) };
#endif
  return cmocka_run_group_tests(tests, setup, teardown);
}
