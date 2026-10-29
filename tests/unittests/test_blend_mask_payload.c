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
#include <Windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
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

typedef struct
{
  int version;
  size_t length;
  size_t mask_id_offset;
  size_t raster_mask_source_offset;
  size_t raster_mask_instance_offset;
  size_t raster_mask_id_offset;
} _raster_layout_t;

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

static const _raster_layout_t _raster_layouts[] = {
  { 9, sizeof(_blend_params9_t), offsetof(_blend_params9_t, mask_id),
    offsetof(_blend_params9_t, raster_mask_source), offsetof(_blend_params9_t, raster_mask_instance),
    offsetof(_blend_params9_t, raster_mask_id) },
  { 10, sizeof(_blend_params10_t), offsetof(_blend_params10_t, mask_id),
    offsetof(_blend_params10_t, raster_mask_source), offsetof(_blend_params10_t, raster_mask_instance),
    offsetof(_blend_params10_t, raster_mask_id) },
  { 11, sizeof(dt_develop_blend_params_t), offsetof(dt_develop_blend_params_t, mask_id),
    offsetof(dt_develop_blend_params_t, raster_mask_source),
    offsetof(dt_develop_blend_params_t, raster_mask_instance),
    offsetof(dt_develop_blend_params_t, raster_mask_id) },
};

static void _assert_undecodable_reference(const uint8_t *payload, const size_t length, const int version)
{
  uint8_t expected_payload[sizeof(dt_develop_blend_params_t) + 1];
  dt_develop_blend_mask_reference_t reference;
  dt_develop_blend_mask_reference_t expected_reference;

  if(length > sizeof(expected_payload))
  {
    fail();
    return;
  }
  memcpy(expected_payload, payload, length);
  memset(&reference, 0x5a, sizeof(reference));
  memcpy(&expected_reference, &reference, sizeof(reference));

  assert_int_equal(dt_develop_blend_read_mask_reference(payload, length, version, &reference),
                   DT_DEVELOP_BLEND_MASK_ID_UNDECODABLE);
  assert_memory_equal(payload, expected_payload, length);
  assert_memory_equal(&reference, &expected_reference, sizeof(reference));

  assert_int_equal(dt_develop_blend_read_mask_reference(payload, length, version, &reference),
                   DT_DEVELOP_BLEND_MASK_ID_UNDECODABLE);
  assert_memory_equal(&reference, &expected_reference, sizeof(reference));
}

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

static void _read_exact_raster_layout_references(void **state)
{
  (void)state;

  for(size_t layout_index = 0; layout_index < sizeof(_raster_layouts) / sizeof(_raster_layouts[0]); layout_index++)
  {
    const _raster_layout_t *layout = &_raster_layouts[layout_index];
    uint8_t payload[sizeof(dt_develop_blend_params_t)];
    uint8_t expected_payload[sizeof(payload)];
    const uint32_t mask_mode = DEVELOP_MASK_RASTER | DEVELOP_MASK_SHAPE;
    const uint32_t mask_id = 913;
    const int raster_mask_instance = 17;
    const int raster_mask_id = 29;
    dt_develop_blend_mask_reference_t reference;
    dt_develop_blend_mask_reference_t expected_reference = { 0 };

    memset(payload, 0xa5, layout->length);
    memcpy(payload, &mask_mode, sizeof(mask_mode));
    memcpy(payload + layout->mask_id_offset, &mask_id, sizeof(mask_id));
    memcpy(payload + layout->raster_mask_source_offset, "rawprepare", sizeof("rawprepare"));
    memcpy(payload + layout->raster_mask_instance_offset, &raster_mask_instance, sizeof(raster_mask_instance));
    memcpy(payload + layout->raster_mask_id_offset, &raster_mask_id, sizeof(raster_mask_id));
    memcpy(expected_payload, payload, layout->length);

    expected_reference.mask_mode = mask_mode;
    expected_reference.mask_id = mask_id;
    memcpy(expected_reference.raster_mask_source, payload + layout->raster_mask_source_offset,
           sizeof(expected_reference.raster_mask_source));
    expected_reference.raster_mask_instance = raster_mask_instance;
    expected_reference.raster_mask_id = raster_mask_id;

    assert_int_equal(dt_develop_blend_read_mask_reference(payload, layout->length, layout->version, &reference),
                     DT_DEVELOP_BLEND_MASK_ID_READABLE);
    assert_memory_equal(&reference, &expected_reference, sizeof(reference));
    assert_memory_equal(payload, expected_payload, layout->length);
  }
}

static void _read_legacy_drawn_mask_references_without_raster_data(void **state)
{
  (void)state;

  for(size_t layout_index = 0; layout_index < 8; layout_index++)
  {
    const _serialized_layout_t *layout = &_serialized_layouts[layout_index];
    uint8_t payload[sizeof(dt_develop_blend_params_t)];
    uint8_t expected_payload[sizeof(payload)];
    const uint32_t mask_mode = layout->version < 5 ? 0u : DEVELOP_MASK_ENABLED | DEVELOP_MASK_SHAPE;
    const uint32_t mask_id = 700u + (uint32_t)layout->version;
    dt_develop_blend_mask_reference_t reference;
    dt_develop_blend_mask_reference_t expected_reference = { 0 };

    memset(payload, 0xa5, layout->length);
    memcpy(payload, &mask_mode, sizeof(mask_mode));
    memcpy(payload + layout->mask_id_offset, &mask_id, sizeof(mask_id));
    memcpy(expected_payload, payload, layout->length);

    expected_reference.mask_mode = layout->version < 5 ? DEVELOP_MASK_ENABLED | DEVELOP_MASK_SHAPE : mask_mode;
    expected_reference.mask_id = mask_id;

    assert_int_equal(dt_develop_blend_read_mask_reference(payload, layout->length, layout->version, &reference),
                     DT_DEVELOP_BLEND_MASK_ID_READABLE);
    assert_memory_equal(&reference, &expected_reference, sizeof(reference));
    assert_memory_equal(payload, expected_payload, layout->length);
  }
}

static void _reject_malformed_mask_reference_payloads_without_changing_input_or_output(void **state)
{
  (void)state;
  uint8_t payload[sizeof(dt_develop_blend_params_t) + 1];

  memset(payload, 0xa5, sizeof(payload));
  _assert_undecodable_reference(payload, sizeof(dt_develop_blend_params_t) - 1, 11);
  _assert_undecodable_reference(payload, sizeof(dt_develop_blend_params_t) + 1, 11);
  _assert_undecodable_reference(payload, sizeof(dt_develop_blend_params_t), 12);
}

static void _reject_raster_source_without_a_terminator_at_a_guard_page(void **state)
{
  (void)state;
  const size_t payload_length = sizeof(dt_develop_blend_params_t);
  const size_t source_offset = offsetof(dt_develop_blend_params_t, raster_mask_source);
#ifdef _WIN32
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  const size_t page_size = info.dwPageSize;
#else
  const size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
#endif
  const uint32_t mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_RASTER;
  uint8_t expected_payload[sizeof(dt_develop_blend_params_t)];
  dt_develop_blend_mask_reference_t reference;
  dt_develop_blend_mask_reference_t expected_reference;

  assert_true(page_size > payload_length);
#ifdef _WIN32
  uint8_t *mapping = VirtualAlloc(NULL, 2 * page_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  DWORD old_protection;
  assert_non_null(mapping);
  assert_true(VirtualProtect(mapping + page_size, page_size, PAGE_NOACCESS, &old_protection));
#else
  uint8_t *mapping = mmap(NULL, 2 * page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert_ptr_not_equal(mapping, MAP_FAILED);
  assert_int_equal(mprotect(mapping + page_size, page_size, PROT_NONE), 0);
#endif
  uint8_t *payload = mapping + page_size - payload_length;
  memset(payload, 0xa5, payload_length);
  memcpy(payload, &mask_mode, sizeof(mask_mode));
  memset(payload + source_offset, 'x', sizeof(dt_dev_operation_t));
  memcpy(expected_payload, payload, payload_length);
  memset(&reference, 0x5a, sizeof(reference));
  memcpy(&expected_reference, &reference, sizeof(reference));

  assert_int_equal(dt_develop_blend_read_mask_reference(payload, payload_length, 11, &reference),
                   DT_DEVELOP_BLEND_MASK_ID_UNDECODABLE);
  assert_memory_equal(payload, expected_payload, payload_length);
  assert_memory_equal(&reference, &expected_reference, sizeof(reference));

#ifdef _WIN32
  assert_true(VirtualFree(mapping, 0, MEM_RELEASE));
#else
  assert_int_equal(munmap(mapping, 2 * page_size), 0);
#endif
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_read_and_clear_only_the_serialized_mask_id),
    cmocka_unit_test(_empty_payloads_are_absent),
    cmocka_unit_test(_undecodable_payloads_remain_unchanged),
    cmocka_unit_test(_read_exact_raster_layout_references),
    cmocka_unit_test(_read_legacy_drawn_mask_references_without_raster_data),
    cmocka_unit_test(_reject_malformed_mask_reference_payloads_without_changing_input_or_output),
    cmocka_unit_test(_reject_raster_source_without_a_terminator_at_a_guard_page),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
