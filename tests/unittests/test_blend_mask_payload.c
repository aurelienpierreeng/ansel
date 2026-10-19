/*
    This file is part of Ansel,
    Copyright (C) 2026 Aurélien PIERRE.

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "develop/blend.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

typedef struct { uint32_t mode; float opacity; uint32_t mask_id; } _blend_params1_t;
typedef struct { uint32_t mode; float opacity; uint32_t mask_id; uint32_t blendif; float blendif_parameters[32]; } _blend_params2_t;
typedef struct { uint32_t mode; float opacity; uint32_t mask_id; uint32_t blendif; float blendif_parameters[64]; } _blend_params3_t;
typedef struct { uint32_t mode; float opacity; uint32_t mask_id; uint32_t blendif; float radius; float blendif_parameters[64]; } _blend_params4_t;
typedef struct { uint32_t mask_mode; uint32_t blend_mode; float opacity; uint32_t mask_combine; uint32_t mask_id; uint32_t blendif; float radius; uint32_t reserved[4]; float blendif_parameters[64]; } _blend_params5_t;
typedef struct { uint32_t mask_mode; uint32_t blend_mode; float opacity; uint32_t mask_combine; uint32_t mask_id; uint32_t blendif; float feathering_radius; uint32_t feathering_guide; float blur_radius; float contrast; float brightness; uint32_t reserved[4]; float blendif_parameters[64]; } _blend_params8_t;
typedef struct { uint32_t mask_mode; uint32_t blend_mode; float opacity; uint32_t mask_combine; uint32_t mask_id; uint32_t blendif; float feathering_radius; uint32_t feathering_guide; float blur_radius; float contrast; float brightness; uint32_t reserved[4]; float blendif_parameters[64]; dt_dev_operation_t raster_mask_source; int raster_mask_instance; int raster_mask_id; gboolean raster_mask_invert; } _blend_params9_t;
typedef struct { uint32_t mask_mode; int32_t blend_cst; uint32_t blend_mode; float blend_parameter; float opacity; uint32_t mask_combine; uint32_t mask_id; uint32_t blendif; float feathering_radius; uint32_t feathering_guide; float blur_radius; float contrast; float brightness; uint32_t reserved[4]; float blendif_parameters[64]; float blendif_boost_factors[16]; dt_dev_operation_t raster_mask_source; int raster_mask_instance; int raster_mask_id; gboolean raster_mask_invert; } _blend_params10_t;

typedef struct
{
  int version;
  size_t length;
  size_t mask_id_offset;
} _serialized_layout_t;

static const _serialized_layout_t _serialized_layouts[] = {
  { 1, sizeof(_blend_params1_t), offsetof(_blend_params1_t, mask_id) },
  { 2, sizeof(_blend_params2_t), offsetof(_blend_params2_t, mask_id) },
  { 3, sizeof(_blend_params3_t), offsetof(_blend_params3_t, mask_id) },
  { 4, sizeof(_blend_params4_t), offsetof(_blend_params4_t, mask_id) },
  { 5, sizeof(_blend_params5_t), offsetof(_blend_params5_t, mask_id) },
  { 6, sizeof(_blend_params5_t), offsetof(_blend_params5_t, mask_id) },
  { 7, sizeof(_blend_params5_t), offsetof(_blend_params5_t, mask_id) },
  { 8, sizeof(_blend_params8_t), offsetof(_blend_params8_t, mask_id) },
  { 9, sizeof(_blend_params9_t), offsetof(_blend_params9_t, mask_id) },
  { 10, sizeof(_blend_params10_t), offsetof(_blend_params10_t, mask_id) },
  { 11, sizeof(dt_develop_blend_params_t), offsetof(dt_develop_blend_params_t, mask_id) },
};

static void _read_and_clear_only_the_serialized_mask_id(void **state)
{
  (void)state;

  for(size_t layout_index = 0; layout_index < sizeof(_serialized_layouts) / sizeof(_serialized_layouts[0]); layout_index++)
  {
    const _serialized_layout_t *layout = &_serialized_layouts[layout_index];
    uint8_t payload[sizeof(dt_develop_blend_params_t)];
    uint8_t expected[sizeof(payload)];
    const uint32_t original_mask_id = 913;
    uint32_t mask_id = 0;

    if(layout->length > sizeof(payload))
    {
      fail();
      return;
    }
    memset(payload, 0xa5, layout->length);
    memcpy(payload + layout->mask_id_offset, &original_mask_id, sizeof(original_mask_id));
    memcpy(expected, payload, layout->length);
    memset(expected + layout->mask_id_offset, 0, sizeof(original_mask_id));

    assert_int_equal(dt_develop_blend_read_mask_id(payload, layout->length, layout->version, &mask_id),
                     DT_DEVELOP_BLEND_MASK_ID_READABLE);
    assert_int_equal(mask_id, original_mask_id);
    assert_true(dt_develop_blend_clear_mask_id(payload, layout->length, layout->version));
    assert_memory_equal(payload, expected, layout->length);
  }
}

static void _empty_payloads_are_absent(void **state)
{
  (void)state;
  uint8_t payload[sizeof(dt_develop_blend_params_t)] = { 0 };
  uint32_t mask_id = 0;

  assert_int_equal(dt_develop_blend_read_mask_id(NULL, 0, 11, &mask_id), DT_DEVELOP_BLEND_MASK_ID_ABSENT);
  assert_int_equal(dt_develop_blend_read_mask_id(NULL, sizeof(payload), 11, &mask_id),
                   DT_DEVELOP_BLEND_MASK_ID_ABSENT);
  assert_int_equal(dt_develop_blend_read_mask_id(payload, 0, 11, &mask_id), DT_DEVELOP_BLEND_MASK_ID_ABSENT);
  assert_int_equal(dt_develop_blend_read_mask_id(payload, sizeof(payload), 11, &mask_id), DT_DEVELOP_BLEND_MASK_ID_ABSENT);
}

static void _undecodable_payloads_remain_unchanged(void **state)
{
  (void)state;
  uint8_t payload[sizeof(dt_develop_blend_params_t)];
  uint8_t expected[sizeof(payload)];
  uint32_t mask_id = 0;

  memset(payload, 0xa5, sizeof(payload));
  memcpy(expected, payload, sizeof(payload));
  assert_int_equal(dt_develop_blend_read_mask_id(payload, sizeof(payload), 12, &mask_id),
                   DT_DEVELOP_BLEND_MASK_ID_UNDECODABLE);
  assert_false(dt_develop_blend_clear_mask_id(payload, sizeof(payload), 12));
  assert_memory_equal(payload, expected, sizeof(payload));

  assert_int_equal(dt_develop_blend_read_mask_id(payload, sizeof(payload) - 1, 11, &mask_id),
                   DT_DEVELOP_BLEND_MASK_ID_UNDECODABLE);
  assert_false(dt_develop_blend_clear_mask_id(payload, sizeof(payload) - 1, 11));
  assert_memory_equal(payload, expected, sizeof(payload));
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_read_and_clear_only_the_serialized_mask_id),
    cmocka_unit_test(_empty_payloads_are_absent),
    cmocka_unit_test(_undecodable_payloads_remain_unchanged),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
