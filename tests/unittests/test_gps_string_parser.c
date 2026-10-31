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

#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

#include "common/utility.h"

static void _parses_degrees_minutes_with_ascii_fractional_decimals(void **state G_GNUC_UNUSED)
{
  assert_double_equal(dt_util_gps_string_to_number("12.5,30.25,N"), 13.004166666666666, 0.000000000001);
}

static void _preserves_cardinal_direction_signs(void **state G_GNUC_UNUSED)
{
  assert_double_equal(dt_util_gps_string_to_number("45,0,N"), 45.0, 0.0);
  assert_double_equal(dt_util_gps_string_to_number("45,0,S"), -45.0, 0.0);
  assert_double_equal(dt_util_gps_string_to_number("120,0,E"), 120.0, 0.0);
  assert_double_equal(dt_util_gps_string_to_number("120,0,W"), -120.0, 0.0);
}

static void _accepts_direction_with_or_without_a_separator(void **state G_GNUC_UNUSED)
{
  assert_double_equal(dt_util_gps_string_to_number("45,30,N"), 45.5, 0.0);
  assert_double_equal(dt_util_gps_string_to_number("45,30N"), 45.5, 0.0);
  assert_double_equal(dt_util_gps_string_to_number("45,30, N"), 45.5, 0.0);
}

static void _parses_degrees_minutes_seconds(void **state G_GNUC_UNUSED)
{
  assert_double_equal(dt_util_gps_string_to_number("179,59,59.5,W"), -179.9998611111111,
                      0.000000000001);
}

static void _accepts_latitude_and_longitude_endpoints(void **state G_GNUC_UNUSED)
{
  assert_double_equal(dt_util_gps_string_to_number("90,0,N"), 90.0, 0.0);
  assert_double_equal(dt_util_gps_string_to_number("180,0,0,E"), 180.0, 0.0);
}

static void _accepts_fractional_minutes_and_seconds_below_sixty(void **state G_GNUC_UNUSED)
{
  assert_double_equal(dt_util_gps_string_to_number("1,59.999,N"), 1.9999833333333332, 0.000000000001);
  assert_double_equal(dt_util_gps_string_to_number("1,0,59.999,N"), 1.016666388888889,
                      0.000000000001);
}

static void _rejects_coordinates_past_cardinal_endpoints(void **state G_GNUC_UNUSED)
{
  const char *const input[] = { "90,0.001,N", "180,0,0.001,E" };

  for(size_t index = 0; index < G_N_ELEMENTS(input); index++)
    assert_true(isnan(dt_util_gps_string_to_number(input[index])));
}

static void _rejects_minutes_or_seconds_of_sixty(void **state G_GNUC_UNUSED)
{
  const char *const input[] = { "1,60,N", "1,0,60,N" };

  for(size_t index = 0; index < G_N_ELEMENTS(input); index++)
    assert_true(isnan(dt_util_gps_string_to_number(input[index])));
}

static void _rejects_incomplete_and_extra_coordinate_tokens(void **state G_GNUC_UNUSED)
{
  const char *const input[] = { "x", "", "1,N", "1,2", "1,2,,N", "1,2,3,4,N" };

  for(size_t index = 0; index < G_N_ELEMENTS(input); index++)
    assert_true(isnan(dt_util_gps_string_to_number(input[index])));
}

static void _rejects_nonfinite_and_trailing_numeric_tokens(void **state G_GNUC_UNUSED)
{
  const char *const input[] = { "NaN,1,N", "Inf,1,N", "1,2.5x,N", "1,2,Nx" };

  for(size_t index = 0; index < G_N_ELEMENTS(input); index++)
    assert_true(isnan(dt_util_gps_string_to_number(input[index])));
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_parses_degrees_minutes_with_ascii_fractional_decimals),
    cmocka_unit_test(_preserves_cardinal_direction_signs),
    cmocka_unit_test(_accepts_direction_with_or_without_a_separator),
    cmocka_unit_test(_parses_degrees_minutes_seconds),
    cmocka_unit_test(_accepts_latitude_and_longitude_endpoints),
    cmocka_unit_test(_accepts_fractional_minutes_and_seconds_below_sixty),
    cmocka_unit_test(_rejects_coordinates_past_cardinal_endpoints),
    cmocka_unit_test(_rejects_minutes_or_seconds_of_sixty),
    cmocka_unit_test(_rejects_incomplete_and_extra_coordinate_tokens),
    cmocka_unit_test(_rejects_nonfinite_and_trailing_numeric_tokens),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
