#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>

#include <glib.h>
#include <cmocka.h>

#include "common/dng_opcode.h"
#include "common/logging.h"

static const char *last_diagnostic;

void __wrap_dt_print(dt_debug_thread_t, const char *message, ...)
{
  last_diagnostic = message;
}

static void put_u32(uint8_t *buffer, size_t offset, uint32_t value)
{
  const uint32_t big_endian = GUINT32_TO_BE(value);
  memcpy(buffer + offset, &big_endian, sizeof(big_endian));
}

static void put_double(uint8_t *buffer, size_t offset, double value)
{
  uint64_t bits;
  memcpy(&bits, &value, sizeof(bits));
  bits = GUINT64_TO_BE(bits);
  memcpy(buffer + offset, &bits, sizeof(bits));
}

static void put_warp_rectilinear(uint8_t *buffer, size_t offset, double center_x)
{
  put_u32(buffer, offset, 1);
  put_u32(buffer, offset + 12, 68);
  put_u32(buffer, offset + 16, 1);
  put_double(buffer, offset + 20, center_x);
  put_double(buffer, offset + 68, center_x);
  put_double(buffer, offset + 76, center_x + 1.0);
}

static void put_vignette_radial(uint8_t *buffer, size_t offset, double center_x)
{
  put_u32(buffer, offset, 3);
  put_u32(buffer, offset + 12, 56);
  put_double(buffer, offset + 16, center_x);
  put_double(buffer, offset + 56, center_x);
  put_double(buffer, offset + 64, center_x + 1.0);
}

static void put_gain_map(uint8_t *buffer, size_t offset, uint32_t top)
{
  put_u32(buffer, offset, 9);
  put_u32(buffer, offset + 12, 80);
  put_u32(buffer, offset + 16, top);
  put_u32(buffer, offset + 48, 1);
  put_u32(buffer, offset + 52, 1);
  put_u32(buffer, offset + 88, 1);
  put_u32(buffer, offset + 92, 0x3f800000);
}

static void free_gain_maps(dt_image_t *image)
{
  g_list_free_full(image->dng_gain_maps, dt_free_gpointer);
  image->dng_gain_maps = NULL;
}

static void test_opcode_list_2_short_counts_reset_gain_maps(void **state)
{
  const uint8_t buffer[] = { 0, 0, 0 };
  for(size_t size = 0; size < sizeof(buffer) + 1; size++)
  {
    dt_image_t image = { 0 };
    image.dng_gain_maps = g_list_append(NULL, g_malloc0(sizeof(dt_dng_gain_map_t)));
    last_diagnostic = NULL;

    dt_dng_opcode_process_opcode_list_2((uint8_t *)buffer, size, &image);

    assert_null(image.dng_gain_maps);
    assert_string_equal(last_diagnostic, "[dng_opcode] OpcodeList2 buffer too small for opcode count\n");
    free_gain_maps(&image);
  }
}

static void test_opcode_list_2_zero_count_resets_gain_maps(void **state)
{
  dt_image_t image = { 0 };
  image.dng_gain_maps = g_list_append(NULL, g_malloc0(sizeof(dt_dng_gain_map_t)));
  uint8_t buffer[4] = { 0 };
  last_diagnostic = NULL;

  dt_dng_opcode_process_opcode_list_2(buffer, sizeof(buffer), &image);

  assert_null(image.dng_gain_maps);
  assert_null(last_diagnostic);
  free_gain_maps(&image);
}

static void test_opcode_list_2_partial_header_does_not_append_gain_map(void **state)
{
  dt_image_t image = { 0 };
  uint8_t buffer[19] = { 0 };
  put_u32(buffer, 0, 1);
  last_diagnostic = NULL;

  dt_dng_opcode_process_opcode_list_2(buffer, sizeof(buffer), &image);

  assert_null(image.dng_gain_maps);
  assert_string_equal(last_diagnostic, "[dng_opcode] Truncated opcode header in OpcodeList2\n");
  free_gain_maps(&image);
}

static void test_opcode_list_2_payload_overflow_does_not_append_gain_map(void **state)
{
  dt_image_t image = { 0 };
  uint8_t buffer[20] = { 0 };
  put_u32(buffer, 0, 1);
  put_u32(buffer, 4, 9);
  put_u32(buffer, 16, UINT32_MAX);
  last_diagnostic = NULL;

  dt_dng_opcode_process_opcode_list_2(buffer, sizeof(buffer), &image);

  assert_null(image.dng_gain_maps);
  assert_string_equal(last_diagnostic, "[dng_opcode] Invalid opcode size in OpcodeList2\n");
  free_gain_maps(&image);
}

static void test_opcode_list_2_skips_unknown_between_gain_maps(void **state)
{
  dt_image_t image = { 0 };
  uint8_t buffer[4 + 96 + 16 + 96] = { 0 };
  put_u32(buffer, 0, 3);
  put_gain_map(buffer, 4, 11);
  put_u32(buffer, 100, 99);
  put_gain_map(buffer, 116, 22);
  last_diagnostic = NULL;

  dt_dng_opcode_process_opcode_list_2(buffer, sizeof(buffer), &image);

  assert_int_equal(g_list_length(image.dng_gain_maps), 2);
  assert_int_equal(((dt_dng_gain_map_t *)g_list_nth_data(image.dng_gain_maps, 0))->top, 11);
  assert_int_equal(((dt_dng_gain_map_t *)g_list_nth_data(image.dng_gain_maps, 1))->top, 22);
  assert_string_equal(last_diagnostic, "[dng_opcode] OpcodeList2 has unsupported %s opcode %d\n");
  free_gain_maps(&image);
}

static void test_opcode_list_2_appends_two_gain_maps_in_order(void **state)
{
  dt_image_t image = { 0 };
  uint8_t buffer[4 + 96 + 96] = { 0 };
  put_u32(buffer, 0, 2);
  put_gain_map(buffer, 4, 55);
  put_gain_map(buffer, 100, 66);
  last_diagnostic = NULL;

  dt_dng_opcode_process_opcode_list_2(buffer, sizeof(buffer), &image);

  assert_int_equal(g_list_length(image.dng_gain_maps), 2);
  assert_int_equal(((dt_dng_gain_map_t *)g_list_nth_data(image.dng_gain_maps, 0))->top, 55);
  assert_int_equal(((dt_dng_gain_map_t *)g_list_nth_data(image.dng_gain_maps, 1))->top, 66);
  assert_null(last_diagnostic);
  free_gain_maps(&image);
}

static void test_opcode_list_2_skips_undersized_gain_map(void **state)
{
  dt_image_t image = { 0 };
  uint8_t buffer[4 + 16 + 4 + 96] = { 0 };
  put_u32(buffer, 0, 2);
  put_u32(buffer, 4, 9);
  put_u32(buffer, 16, 4);
  put_gain_map(buffer, 24, 33);
  last_diagnostic = NULL;

  dt_dng_opcode_process_opcode_list_2(buffer, sizeof(buffer), &image);

  assert_int_equal(g_list_length(image.dng_gain_maps), 1);
  assert_int_equal(((dt_dng_gain_map_t *)image.dng_gain_maps->data)->top, 33);
  assert_string_equal(last_diagnostic, "[dng_opcode] Undersized GainMap opcode in OpcodeList2\n");
  free_gain_maps(&image);
}

static void test_opcode_list_2_skips_gain_map_with_missing_samples(void **state)
{
  dt_image_t image = { 0 };
  uint8_t buffer[4 + 16 + 80] = { 0 };
  put_u32(buffer, 0, 1);
  put_gain_map(buffer, 4, 44);
  put_u32(buffer, 52, 2);
  put_u32(buffer, 56, 2);
  last_diagnostic = NULL;

  dt_dng_opcode_process_opcode_list_2(buffer, sizeof(buffer), &image);

  assert_null(image.dng_gain_maps);
  assert_string_equal(last_diagnostic, "[dng_opcode] Undersized GainMap opcode in OpcodeList2\n");
  free_gain_maps(&image);
}

static void test_opcode_list_2_retains_gain_map_before_malformed_envelope(void **state)
{
  dt_image_t image = { 0 };
  uint8_t buffer[4 + 96 + 16] = { 0 };
  put_u32(buffer, 0, 2);
  put_gain_map(buffer, 4, 44);
  put_u32(buffer, 100, 9);
  put_u32(buffer, 112, UINT32_MAX);
  last_diagnostic = NULL;

  dt_dng_opcode_process_opcode_list_2(buffer, sizeof(buffer), &image);

  assert_int_equal(g_list_length(image.dng_gain_maps), 1);
  assert_int_equal(((dt_dng_gain_map_t *)image.dng_gain_maps->data)->top, 44);
  assert_string_equal(last_diagnostic, "[dng_opcode] Invalid opcode size in OpcodeList2\n");
  free_gain_maps(&image);
}

static void test_opcode_list_3_short_counts_reset_corrections(void **state)
{
  const uint8_t buffer[] = { 0, 0, 0 };
  for(size_t size = 0; size < sizeof(buffer) + 1; size++)
  {
    dt_image_t image = { 0 };
    image.exif_correction_data.dng.has_warp = TRUE;
    last_diagnostic = NULL;

    dt_dng_opcode_process_opcode_list_3((uint8_t *)buffer, size, &image);

    assert_false(image.exif_correction_data.dng.has_warp);
    assert_string_equal(last_diagnostic, "[dng_opcode] OpcodeList3 buffer too small for opcode count\n");
  }
}

static void test_opcode_list_3_zero_count_resets_corrections(void **state)
{
  dt_image_t image = { 0 };
  uint8_t buffer[4] = { 0 };
  image.exif_correction_data.dng.has_warp = TRUE;
  last_diagnostic = NULL;

  dt_dng_opcode_process_opcode_list_3(buffer, sizeof(buffer), &image);

  assert_false(image.exif_correction_data.dng.has_warp);
  assert_null(last_diagnostic);
}

static void test_opcode_list_3_partial_header_preserves_reset(void **state)
{
  dt_image_t image = { 0 };
  uint8_t buffer[19] = { 0 };
  put_u32(buffer, 0, 1);
  last_diagnostic = NULL;

  dt_dng_opcode_process_opcode_list_3(buffer, sizeof(buffer), &image);

  assert_false(image.exif_correction_data.dng.has_warp);
  assert_string_equal(last_diagnostic, "[dng_opcode] Truncated opcode header in OpcodeList3\n");
}

static void test_opcode_list_3_uint32_max_payload_preserves_reset(void **state)
{
  dt_image_t image = { 0 };
  uint8_t buffer[20] = { 0 };
  put_u32(buffer, 0, 1);
  put_u32(buffer, 4, 1);
  put_u32(buffer, 16, UINT32_MAX);
  last_diagnostic = NULL;

  dt_dng_opcode_process_opcode_list_3(buffer, sizeof(buffer), &image);

  assert_false(image.exif_correction_data.dng.has_warp);
  assert_string_equal(last_diagnostic, "[dng_opcode] Invalid opcode size in OpcodeList3\n");
}

static void test_opcode_list_3_skips_unknown_between_valid_opcodes(void **state)
{
  dt_image_t image = { 0 };
  uint8_t buffer[4 + 84 + 16 + 72] = { 0 };
  put_u32(buffer, 0, 3);
  put_warp_rectilinear(buffer, 4, 10.0);
  put_u32(buffer, 88, 99);
  put_vignette_radial(buffer, 104, 20.0);
  last_diagnostic = NULL;

  dt_dng_opcode_process_opcode_list_3(buffer, sizeof(buffer), &image);

  assert_true(image.exif_correction_data.dng.has_warp);
  assert_true(image.exif_correction_data.dng.has_vignette);
  assert_float_equal(image.exif_correction_data.dng.warp_cx, 10.0, 0.0);
  assert_float_equal(image.exif_correction_data.dng.vig_cx, 20.0, 0.0);
  assert_string_equal(last_diagnostic, "[dng_opcode] OpcodeList3 has unsupported %s opcode %d\n");
}

static void test_opcode_list_3_uses_last_valid_warp(void **state)
{
  dt_image_t image = { 0 };
  uint8_t buffer[4 + 84 + 84] = { 0 };
  put_u32(buffer, 0, 2);
  put_warp_rectilinear(buffer, 4, 10.0);
  put_warp_rectilinear(buffer, 88, 20.0);
  last_diagnostic = NULL;

  dt_dng_opcode_process_opcode_list_3(buffer, sizeof(buffer), &image);

  assert_true(image.exif_correction_data.dng.has_warp);
  assert_float_equal(image.exif_correction_data.dng.warp_cx, 20.0, 0.0);
  assert_null(last_diagnostic);
}

static void test_opcode_list_3_retains_valid_correction_before_malformed_envelope(void **state)
{
  dt_image_t image = { 0 };
  uint8_t buffer[4 + 84 + 16] = { 0 };
  put_u32(buffer, 0, 2);
  put_warp_rectilinear(buffer, 4, 10.0);
  put_u32(buffer, 88, 3);
  put_u32(buffer, 100, UINT32_MAX);
  last_diagnostic = NULL;

  dt_dng_opcode_process_opcode_list_3(buffer, sizeof(buffer), &image);

  assert_true(image.exif_correction_data.dng.has_warp);
  assert_float_equal(image.exif_correction_data.dng.warp_cx, 10.0, 0.0);
  assert_string_equal(last_diagnostic, "[dng_opcode] Invalid opcode size in OpcodeList3\n");
}

static void test_opcode_list_3_skips_invalid_plane_and_continues(void **state)
{
  dt_image_t image = { 0 };
  uint8_t buffer[4 + 84 + 72] = { 0 };
  put_u32(buffer, 0, 2);
  put_warp_rectilinear(buffer, 4, 10.0);
  put_u32(buffer, 20, 0);
  put_vignette_radial(buffer, 88, 20.0);
  last_diagnostic = NULL;

  dt_dng_opcode_process_opcode_list_3(buffer, sizeof(buffer), &image);

  assert_false(image.exif_correction_data.dng.has_warp);
  assert_true(image.exif_correction_data.dng.has_vignette);
  assert_string_equal(last_diagnostic,
                      "[dng_opcode] Invalid or undersized WarpRectilinear planes in OpcodeList3\n");
}

typedef struct opcode_list_3_thread_input_t
{
  uint8_t *buffer;
  uint32_t size;
  dt_image_t *image;
} opcode_list_3_thread_input_t;

static void *process_opcode_list_3(void *user_data)
{
  opcode_list_3_thread_input_t *input = user_data;
  dt_dng_opcode_process_opcode_list_3(input->buffer, input->size, input->image);
  return NULL;
}

static void test_opcode_list_3_concurrent_images_remain_isolated(void **state)
{
  dt_image_t warp_image = { 0 };
  dt_image_t vignette_image = { 0 };
  uint8_t warp_buffer[4 + 84] = { 0 };
  uint8_t vignette_buffer[4 + 72] = { 0 };
  put_u32(warp_buffer, 0, 1);
  put_warp_rectilinear(warp_buffer, 4, 10.0);
  put_u32(vignette_buffer, 0, 1);
  put_vignette_radial(vignette_buffer, 4, 20.0);
  const opcode_list_3_thread_input_t warp_input = { warp_buffer, sizeof(warp_buffer), &warp_image };
  const opcode_list_3_thread_input_t vignette_input = { vignette_buffer, sizeof(vignette_buffer), &vignette_image };
  pthread_t warp_thread;
  pthread_t vignette_thread;

  assert_int_equal(pthread_create(&warp_thread, NULL, process_opcode_list_3, (void *)&warp_input), 0);
  assert_int_equal(pthread_create(&vignette_thread, NULL, process_opcode_list_3, (void *)&vignette_input), 0);
  assert_int_equal(pthread_join(warp_thread, NULL), 0);
  assert_int_equal(pthread_join(vignette_thread, NULL), 0);

  assert_true(warp_image.exif_correction_data.dng.has_warp);
  assert_false(warp_image.exif_correction_data.dng.has_vignette);
  assert_false(vignette_image.exif_correction_data.dng.has_warp);
  assert_true(vignette_image.exif_correction_data.dng.has_vignette);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_opcode_list_2_short_counts_reset_gain_maps),
    cmocka_unit_test(test_opcode_list_2_zero_count_resets_gain_maps),
    cmocka_unit_test(test_opcode_list_2_partial_header_does_not_append_gain_map),
    cmocka_unit_test(test_opcode_list_2_payload_overflow_does_not_append_gain_map),
    cmocka_unit_test(test_opcode_list_2_skips_unknown_between_gain_maps),
    cmocka_unit_test(test_opcode_list_2_appends_two_gain_maps_in_order),
    cmocka_unit_test(test_opcode_list_2_skips_undersized_gain_map),
    cmocka_unit_test(test_opcode_list_2_skips_gain_map_with_missing_samples),
    cmocka_unit_test(test_opcode_list_2_retains_gain_map_before_malformed_envelope),
    cmocka_unit_test(test_opcode_list_3_short_counts_reset_corrections),
    cmocka_unit_test(test_opcode_list_3_zero_count_resets_corrections),
    cmocka_unit_test(test_opcode_list_3_partial_header_preserves_reset),
    cmocka_unit_test(test_opcode_list_3_uint32_max_payload_preserves_reset),
    cmocka_unit_test(test_opcode_list_3_skips_unknown_between_valid_opcodes),
    cmocka_unit_test(test_opcode_list_3_uses_last_valid_warp),
    cmocka_unit_test(test_opcode_list_3_retains_valid_correction_before_malformed_envelope),
    cmocka_unit_test(test_opcode_list_3_skips_invalid_plane_and_continues),
    cmocka_unit_test(test_opcode_list_3_concurrent_images_remain_isolated),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
