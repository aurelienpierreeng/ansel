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

/** The monitor profile built from a windowing system's numbers.
 *
 * dt_colorspaces_build_display_profile() turns the chromaticities and tone curve a Wayland
 * compositor reports into the ICC profile every pixel sent to the screen goes through. Two
 * things about it were got wrong once and are invisible until a user sees them:
 *
 *  - lcms2 accepts an exponent of 0 and builds a profile that maps black, mid-grey and white
 *    alike to white. A compositor that names a tone curve the caller does not know left the
 *    exponent at 0, and the whole interface turned white.
 *
 *  - lcms2 stamps the creation time into the profile. The display profile is replaced when
 *    its bytes differ, so a profile rebuilt on every window event was a new monitor every
 *    second, with the transforms rebuilt each time.
 */

#include "colorprofiles/colorspaces.h"
#include "system/mem_alloc.h"

#include <glib.h>
#include <lcms2.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
// cmocka.h declares `extern jmp_buf global_expect_assert_env' at file scope without including
// <setjmp.h> itself. Same suppression as the other tests here, and for the same reason.
#include <setjmp.h>  // NOLINT(misc-include-cleaner)
#include <stdint.h>
#include <string.h>
#include <cmocka.h>

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static dt_colorspaces_display_description_t _srgb_monitor(void)
{
  const dt_colorspaces_display_description_t description = {
    .red = { 0.64, 0.33 },
    .green = { 0.30, 0.60 },
    .blue = { 0.15, 0.06 },
    .white = { 0.3127, 0.3290 },
    .gamma = 2.2,
    .srgb_curve = FALSE,
  };
  return description;
}

/** What 8-bit sRGB black, mid-grey and white become on the display the profile describes. */
static void _srgb_through(const uint8_t *data, const size_t size, uint8_t out[9])
{
  cmsHPROFILE display = cmsOpenProfileFromMem(data, (cmsUInt32Number)size);
  assert_non_null(display);
  cmsHPROFILE srgb = cmsCreate_sRGBProfile();
  cmsHTRANSFORM transform = cmsCreateTransform(srgb, TYPE_RGB_8, display, TYPE_RGB_8, INTENT_PERCEPTUAL, 0);
  assert_non_null(transform);

  const uint8_t in[9] = { 0, 0, 0, 128, 128, 128, 255, 255, 255 };
  cmsDoTransform(transform, in, out, 3);

  cmsDeleteTransform(transform);
  cmsCloseProfile(srgb);
  cmsCloseProfile(display);
}

static void _an_srgb_monitor_keeps_grey_grey(void **state)
{
  const dt_colorspaces_display_description_t description = _srgb_monitor();
  size_t size = 0;
  uint8_t *data = dt_colorspaces_build_display_profile(&description, &size);
  assert_non_null(data);
  assert_true(size > 128);

  uint8_t out[9] = { 0 };
  _srgb_through(data, size, out);

  for(int c = 0; c < 3; c++)
  {
    assert_int_equal(out[c], 0);
    // the sRGB curve and a 2.2 power law are one code value apart at mid-grey
    assert_in_range(out[3 + c], 126, 130);
    assert_int_equal(out[6 + c], 255);
  }

  dt_free(data);
}

static void _the_same_description_gives_the_same_bytes(void **state)
{
  const dt_colorspaces_display_description_t description = _srgb_monitor();

  size_t first_size = 0;
  uint8_t *first = dt_colorspaces_build_display_profile(&description, &first_size);
  assert_non_null(first);

  // lcms2 stamps seconds: the clock has to tick between the two builds for this to prove anything
  g_usleep(1100 * 1000);

  size_t second_size = 0;
  uint8_t *second = dt_colorspaces_build_display_profile(&description, &second_size);
  assert_non_null(second);

  assert_int_equal(first_size, second_size);
  assert_memory_equal(first, second, first_size);

  dt_free(first);
  dt_free(second);
}

static void _an_exponent_lcms2_would_turn_white_is_refused(void **state)
{
  const double exponents[] = { 0.0, -1.0, 0.5, 10.5, NAN, INFINITY };

  for(size_t i = 0; i < G_N_ELEMENTS(exponents); i++)
  {
    dt_colorspaces_display_description_t description = _srgb_monitor();
    description.gamma = exponents[i];

    size_t size = 1;
    assert_null(dt_colorspaces_build_display_profile(&description, &size));
    assert_int_equal(size, 0);
  }
}

static void _primaries_that_make_no_space_are_refused(void **state)
{
  size_t size = 1;

  // CIE XYZ as "primaries": y = 0 twice, and lcms2 divides by it
  dt_colorspaces_display_description_t description = _srgb_monitor();
  description.red[0] = 1.0;
  description.red[1] = 0.0;
  description.blue[0] = 0.0;
  description.blue[1] = 0.0;
  assert_null(dt_colorspaces_build_display_profile(&description, &size));

  // three primaries on one line
  description = _srgb_monitor();
  description.green[0] = 0.395;
  description.green[1] = 0.195;
  assert_null(dt_colorspaces_build_display_profile(&description, &size));

  // what a compositor sends before it knows anything
  memset(&description, 0, sizeof(description));
  description.gamma = 2.2;
  assert_null(dt_colorspaces_build_display_profile(&description, &size));

  assert_null(dt_colorspaces_build_display_profile(NULL, &size));
  assert_int_equal(size, 0);
}

static void _the_srgb_curve_is_the_piecewise_one(void **state)
{
  dt_colorspaces_display_description_t description = _srgb_monitor();
  description.srgb_curve = TRUE;
  // ignored when the piecewise curve is asked for, including a value that would be refused
  description.gamma = 0.0;

  size_t size = 0;
  uint8_t *data = dt_colorspaces_build_display_profile(&description, &size);
  assert_non_null(data);

  // through the sRGB curve on both sides, sRGB is the identity
  uint8_t out[9] = { 0 };
  _srgb_through(data, size, out);
  for(int c = 0; c < 3; c++)
  {
    assert_int_equal(out[c], 0);
    assert_int_equal(out[3 + c], 128);
    assert_int_equal(out[6 + c], 255);
  }

  dt_free(data);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_an_srgb_monitor_keeps_grey_grey),
    cmocka_unit_test(_the_same_description_gives_the_same_bytes),
    cmocka_unit_test(_an_exponent_lcms2_would_turn_white_is_refused),
    cmocka_unit_test(_primaries_that_make_no_space_are_refused),
    cmocka_unit_test(_the_srgb_curve_is_the_piecewise_one),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
