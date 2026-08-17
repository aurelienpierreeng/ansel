#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

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
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
