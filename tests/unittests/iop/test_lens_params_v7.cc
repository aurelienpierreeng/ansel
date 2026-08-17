/*
    This file is part of Ansel,
    Copyright (C) 2026 Aurélien PIERRE.

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Ansel is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Ansel.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#include "../../../src/iop/lens_legacy_params.hh"

static dt_iop_lensfun_params_t defaults()
{
  dt_iop_lensfun_params_t result = {};
  result.scale = 1.0f;
  result.tca_r = 1.0f;
  result.tca_b = 1.0f;
  result.has_been_set = 1;
  result.vignetting_method = dt_iop_lens_correction_source_t::EMBEDDED;
  result.distortion_method = dt_iop_lens_correction_source_t::OFF;
  result.tca_method = dt_iop_lens_tca_source_t::MANUAL;
  return result;
}

template<typename legacy_params_t>
static void populate(legacy_params_t *legacy)
{
  memset(legacy, 0, sizeof(*legacy));
  legacy->modify_flags = 0x55;
  legacy->scale = 1.2f;
  legacy->crop = 0.05f;
  legacy->focal = 50.0f;
  legacy->aperture = 2.8f;
  legacy->distance = 5.0f;
  legacy->target_geom = LF_RECTILINEAR;
  legacy->tca_r = 0.999f;
  legacy->tca_b = 1.001f;
  memset(legacy->camera, 'c', sizeof(legacy->camera));
  memset(legacy->lens, 'l', sizeof(legacy->lens));
}

template<typename legacy_params_t>
static void assert_common_conversion(const legacy_params_t &legacy,
                                      const dt_iop_lensfun_params_t &output)
{
  assert_int_equal(output.modify_flags, legacy.modify_flags);
  assert_float_equal(output.scale, legacy.scale, 1e-6f);
  assert_float_equal(output.crop, legacy.crop, 1e-6f);
  assert_float_equal(output.focal, legacy.focal, 1e-6f);
  assert_float_equal(output.aperture, legacy.aperture, 1e-6f);
  assert_float_equal(output.distance, legacy.distance, 1e-6f);
  assert_int_equal(output.target_geom, legacy.target_geom);
  const size_t camera_length = sizeof(legacy.camera) < sizeof(output.camera) - 1
      ? sizeof(legacy.camera) : sizeof(output.camera) - 1;
  const size_t lens_length = sizeof(legacy.lens) < sizeof(output.lens) - 1
      ? sizeof(legacy.lens) : sizeof(output.lens) - 1;
  assert_memory_equal(output.camera, legacy.camera, camera_length);
  assert_memory_equal(output.lens, legacy.lens, lens_length);
  assert_int_equal(output.camera[camera_length], '\0');
  assert_int_equal(output.lens[lens_length], '\0');
  assert_float_equal(output.tca_r, legacy.tca_b, 1e-6f);
  assert_float_equal(output.tca_b, legacy.tca_r, 1e-6f);
}

static void assert_default_methods(const dt_iop_lensfun_params_t &output)
{
  assert_int_equal(output.vignetting_method, dt_iop_lens_correction_source_t::EMBEDDED);
  assert_int_equal(output.distortion_method, dt_iop_lens_correction_source_t::OFF);
  assert_int_equal(output.tca_method, dt_iop_lens_tca_source_t::MANUAL);
}

static void test_v2_conversion(void **)
{
  dt_iop_lensfun_params_v2_t legacy;
  populate(&legacy);
  dt_iop_lensfun_params_t output = {};
  const dt_iop_lensfun_params_t default_params = defaults();

  assert_int_equal(dt_iop_lensfun_convert_legacy_params(&legacy, 2, &default_params, &output, 7), 0);
  assert_common_conversion(legacy, output);
  assert_int_equal(output.has_been_set, 0);
  assert_default_methods(output);
}

static void test_v3_conversion(void **)
{
  dt_iop_lensfun_params_v3_t legacy;
  populate(&legacy);
  dt_iop_lensfun_params_t output = {};
  const dt_iop_lensfun_params_t default_params = defaults();

  assert_int_equal(dt_iop_lensfun_convert_legacy_params(&legacy, 3, &default_params, &output, 7), 0);
  assert_common_conversion(legacy, output);
  assert_int_equal(output.has_been_set, 0);
  assert_default_methods(output);
}

static void test_v4_modified_conversion(void **)
{
  dt_iop_lensfun_params_v4_t legacy;
  populate(&legacy);
  legacy.modified = 1;
  dt_iop_lensfun_params_t output = {};
  const dt_iop_lensfun_params_t default_params = defaults();

  assert_int_equal(dt_iop_lensfun_convert_legacy_params(&legacy, 4, &default_params, &output, 7), 0);
  assert_common_conversion(legacy, output);
  assert_int_equal(output.has_been_set, 0);
  assert_default_methods(output);
}

static void test_v4_unmodified_conversion(void **)
{
  dt_iop_lensfun_params_v4_t legacy;
  populate(&legacy);
  legacy.modified = 0;
  dt_iop_lensfun_params_t output = {};
  const dt_iop_lensfun_params_t default_params = defaults();

  assert_int_equal(dt_iop_lensfun_convert_legacy_params(&legacy, 4, &default_params, &output, 7), 0);
  assert_common_conversion(legacy, output);
  assert_int_equal(output.has_been_set, 1);
  assert_default_methods(output);
}

static void test_v5_modified_conversion(void **)
{
  dt_iop_lensfun_params_v5_t legacy;
  populate(&legacy);
  legacy.modified = 1;
  dt_iop_lensfun_params_t output = {};
  const dt_iop_lensfun_params_t default_params = defaults();

  assert_int_equal(dt_iop_lensfun_convert_legacy_params(&legacy, 5, &default_params, &output, 7), 0);
  assert_common_conversion(legacy, output);
  assert_int_equal(output.has_been_set, 0);
  assert_default_methods(output);
}

static void test_v5_unmodified_conversion(void **)
{
  dt_iop_lensfun_params_v5_t legacy;
  populate(&legacy);
  legacy.modified = 0;
  dt_iop_lensfun_params_t output = {};
  const dt_iop_lensfun_params_t default_params = defaults();

  assert_int_equal(dt_iop_lensfun_convert_legacy_params(&legacy, 5, &default_params, &output, 7), 0);
  assert_common_conversion(legacy, output);
  assert_int_equal(output.has_been_set, 1);
  assert_default_methods(output);
}

static void test_v6_embedded_conversion(void **)
{
  dt_iop_lensfun_params_v6_t legacy;
  populate(&legacy);
  legacy.has_been_set = 0;
  legacy.method = dt_iop_lens_method_t::EMBEDDED_METADATA;
  dt_iop_lensfun_params_t output = {};
  const dt_iop_lensfun_params_t default_params = defaults();

  assert_int_equal(dt_iop_lensfun_convert_legacy_params(&legacy, 6, &default_params, &output, 7), 0);
  assert_common_conversion(legacy, output);
  assert_int_equal(output.has_been_set, 0);
  assert_int_equal(output.vignetting_method, dt_iop_lens_correction_source_t::EMBEDDED);
  assert_int_equal(output.distortion_method, dt_iop_lens_correction_source_t::EMBEDDED);
  assert_int_equal(output.tca_method, dt_iop_lens_tca_source_t::EMBEDDED);
}

static void test_v6_lensfun_conversion(void **)
{
  dt_iop_lensfun_params_v6_t legacy;
  populate(&legacy);
  legacy.has_been_set = 1;
  legacy.method = dt_iop_lens_method_t::LENSFUN;
  dt_iop_lensfun_params_t output = {};
  const dt_iop_lensfun_params_t default_params = defaults();

  assert_int_equal(dt_iop_lensfun_convert_legacy_params(&legacy, 6, &default_params, &output, 7), 0);
  assert_common_conversion(legacy, output);
  assert_int_equal(output.has_been_set, 1);
  assert_int_equal(output.vignetting_method, dt_iop_lens_correction_source_t::LENSFUN_DB);
  assert_int_equal(output.distortion_method, dt_iop_lens_correction_source_t::LENSFUN_DB);
  assert_int_equal(output.tca_method, dt_iop_lens_tca_source_t::LENSFUN_DB);
}

static void test_unsupported_pairs_do_not_write(void **)
{
  dt_iop_lensfun_params_v2_t legacy = {};
  dt_iop_lensfun_params_t output;
  dt_iop_lensfun_params_t expected;
  const dt_iop_lensfun_params_t default_params = defaults();
  memset(&output, 0xcd, sizeof(output));
  expected = output;

  assert_int_equal(dt_iop_lensfun_convert_legacy_params(&legacy, 1, &default_params, &output, 7), 1);
  assert_memory_equal(&output, &expected, sizeof(output));
  assert_int_equal(dt_iop_lensfun_convert_legacy_params(&legacy, 2, &default_params, &output, 6), 1);
  assert_memory_equal(&output, &expected, sizeof(output));
}

int main()
{
  const CMUnitTest tests[] = {
    cmocka_unit_test(test_v2_conversion),
    cmocka_unit_test(test_v3_conversion),
    cmocka_unit_test(test_v4_modified_conversion),
    cmocka_unit_test(test_v4_unmodified_conversion),
    cmocka_unit_test(test_v5_modified_conversion),
    cmocka_unit_test(test_v5_unmodified_conversion),
    cmocka_unit_test(test_v6_embedded_conversion),
    cmocka_unit_test(test_v6_lensfun_conversion),
    cmocka_unit_test(test_unsupported_pairs_do_not_write),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
