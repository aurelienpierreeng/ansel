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

/* The colour well's arithmetic, which needs no display: the hexadecimal numbers it reads and writes,
 * the list of recent colours it keeps, and the hue, saturation and value it holds a colour in. */

#include "widgets/color_well.h"

#include <glib.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <cmocka.h>

static void _assert_rgba(const GdkRGBA *color, const double red, const double green, const double blue,
                         const double alpha)
{
  assert_true(fabs(color->red - red) < 1e-9);
  assert_true(fabs(color->green - green) < 1e-9);
  assert_true(fabs(color->blue - blue) < 1e-9);
  assert_true(fabs(color->alpha - alpha) < 1e-9);
}

static void _hex_reads_every_form_css_does(void **state)
{
  (void)state;
  GdkRGBA color = { 0.1, 0.2, 0.3, 0.4 };
  gboolean has_alpha = TRUE;

  // A short form repeats each digit, and a text with no alpha digits leaves the opacity alone.
  assert_true(dt_color_well_parse_hex("#f80", &color, &has_alpha));
  _assert_rgba(&color, 1.0, 0x88 / 255.0, 0.0, 0.4);
  assert_false(has_alpha);

  assert_true(dt_color_well_parse_hex("  #FF8800 ", &color, &has_alpha));
  _assert_rgba(&color, 1.0, 0x88 / 255.0, 0.0, 0.4);
  assert_false(has_alpha);

  assert_true(dt_color_well_parse_hex("#ff880080", &color, &has_alpha));
  _assert_rgba(&color, 1.0, 0x88 / 255.0, 0.0, 128 / 255.0);
  assert_true(has_alpha);

  assert_true(dt_color_well_parse_hex("#abcd", &color, &has_alpha));
  _assert_rgba(&color, 0xaa / 255.0, 0xbb / 255.0, 0xcc / 255.0, 0xdd / 255.0);
  assert_true(has_alpha);

  assert_true(dt_color_well_parse_hex("0a0b0c", &color, NULL));
  _assert_rgba(&color, 10 / 255.0, 11 / 255.0, 12 / 255.0, 0xdd / 255.0);
}

static void _hex_refuses_what_is_not_a_colour_and_touches_nothing(void **state)
{
  (void)state;
  const char *const invalid[]
      = { "", "#", "#12", "#12345", "#1234567", "#123456789", "#ggg", "#ff88zz", "red", "# f80" };
  for(size_t idx = 0; idx < G_N_ELEMENTS(invalid); idx++)
  {
    GdkRGBA color = { 0.5, 0.5, 0.5, 0.5 };
    assert_false(dt_color_well_parse_hex(invalid[idx], &color, NULL));
    _assert_rgba(&color, 0.5, 0.5, 0.5, 0.5);
  }
  GdkRGBA color = { 0.5, 0.5, 0.5, 0.5 };
  assert_false(dt_color_well_parse_hex(NULL, &color, NULL));
}

static void _hex_writes_the_alpha_only_when_there_is_one(void **state)
{
  (void)state;
  char text[DT_COLOR_WELL_HEX_SIZE];
  const GdkRGBA opaque = { 1.0, 0.5, 0.0, 1.0 };
  dt_color_well_format_hex(&opaque, TRUE, text, sizeof(text));
  assert_string_equal(text, "#FF8000");
  const GdkRGBA half = { 1.0, 0.5, 0.0, 0.5 };
  dt_color_well_format_hex(&half, TRUE, text, sizeof(text));
  assert_string_equal(text, "#FF800080");
  dt_color_well_format_hex(&half, FALSE, text, sizeof(text));
  assert_string_equal(text, "#FF8000");
  // Out of range clamps rather than wrapping a byte.
  const GdkRGBA outside = { 1.5, -0.2, 0.0, 1.0 };
  dt_color_well_format_hex(&outside, TRUE, text, sizeof(text));
  assert_string_equal(text, "#FF0000");
}

static void _a_recent_colour_moves_to_the_head_rather_than_repeating(void **state)
{
  (void)state;
  GdkRGBA history[DT_COLOR_WELL_HISTORY_MAX];
  const GdkRGBA red = { 1.0, 0.0, 0.0, 1.0 };
  const GdkRGBA green = { 0.0, 1.0, 0.0, 1.0 };
  const GdkRGBA blue = { 0.0, 0.0, 1.0, 1.0 };
  int count = 0;
  count = dt_color_well_history_push(history, count, &red);
  count = dt_color_well_history_push(history, count, &green);
  count = dt_color_well_history_push(history, count, &blue);
  count = dt_color_well_history_push(history, count, &green);
  assert_int_equal(count, 3);
  _assert_rgba(&history[0], 0.0, 1.0, 0.0, 1.0);
  _assert_rgba(&history[1], 0.0, 0.0, 1.0, 1.0);
  _assert_rgba(&history[2], 1.0, 0.0, 0.0, 1.0);

  // Equal to the byte is equal: the list is stored at that precision and would read back as a repeat.
  const GdkRGBA almost_green = { 0.001, 1.0, 0.0, 1.0 };
  count = dt_color_well_history_push(history, count, &almost_green);
  assert_int_equal(count, 3);

  // Transparency is part of the colour.
  const GdkRGBA see_through_green = { 0.0, 1.0, 0.0, 0.5 };
  count = dt_color_well_history_push(history, count, &see_through_green);
  assert_int_equal(count, 4);
}

static void _a_change_is_one_the_byte_shows(void **state)
{
  (void)state;
  // A document's float colour against the recent colour naming it: the same "#89" for 0.537 and 137/255.
  const GdkRGBA document = { 0.537, 0.2, 1.0, 1.0 };
  const GdkRGBA recent = { 137 / 255.0, 51 / 255.0, 1.0, 1.0 };
  assert_true(dt_color_well_same_color(&document, &recent));
  const GdkRGBA next_code = { 138 / 255.0, 51 / 255.0, 1.0, 1.0 };
  assert_false(dt_color_well_same_color(&document, &next_code));
  // The opacity is part of the colour, and a byte of it is a change.
  const GdkRGBA lighter = { 0.537, 0.2, 1.0, 254 / 255.0 };
  assert_false(dt_color_well_same_color(&document, &lighter));
  assert_false(dt_color_well_same_color(&document, NULL));
}

static void _the_list_is_cut_at_its_length_most_recent_first(void **state)
{
  (void)state;
  GdkRGBA history[DT_COLOR_WELL_HISTORY_MAX];
  int count = 0;
  for(int idx = 0; idx < DT_COLOR_WELL_HISTORY_MAX + 5; idx++)
  {
    const GdkRGBA color = { idx / 20.0, 0.3, 0.6, 1.0 };
    count = dt_color_well_history_push(history, count, &color);
  }
  assert_int_equal(count, DT_COLOR_WELL_HISTORY_MAX);
  assert_true(fabs(history[0].red - (DT_COLOR_WELL_HISTORY_MAX + 4) / 20.0) < 1e-9);
  assert_true(fabs(history[DT_COLOR_WELL_HISTORY_MAX - 1].red - 5 / 20.0) < 1e-9);
}

static void _the_stored_list_reads_back_and_skips_what_it_cannot_read(void **state)
{
  (void)state;
  const GdkRGBA history[DT_COLOR_WELL_HISTORY_MAX] = { { 1.0, 0.0, 0.0, 1.0 }, { 0.0, 0.5, 1.0, 0.25 } };
  gchar *text = dt_color_well_history_format(history, 2);
  // Always eight digits, so an opaque colour and a transparent one read back the same way.
  assert_string_equal(text, "#FF0000FF,#0080FF40");
  GdkRGBA back[DT_COLOR_WELL_HISTORY_MAX];
  assert_int_equal(dt_color_well_history_parse(text, back), 2);
  _assert_rgba(&back[0], 1.0, 0.0, 0.0, 1.0);
  assert_true(fabs(back[1].green - 128 / 255.0) < 1e-9);
  assert_true(fabs(back[1].alpha - 64 / 255.0) < 1e-9);
  g_free(text);

  assert_int_equal(dt_color_well_history_parse("#ff0000ff,garbage,,#00ff00", back), 2);
  _assert_rgba(&back[1], 0.0, 1.0, 0.0, 1.0);
  assert_int_equal(dt_color_well_history_parse("", back), 0);
  assert_int_equal(dt_color_well_history_parse(NULL, back), 0);
}

static void _hue_saturation_and_value_round_trip_and_name_the_primaries(void **state)
{
  (void)state;
  double red = 0.0;
  double green = 0.0;
  double blue = 0.0;
  dt_color_well_hsv_to_rgb(0.0, 1.0, 1.0, &red, &green, &blue);
  assert_true(red == 1.0 && green == 0.0 && blue == 0.0);
  dt_color_well_hsv_to_rgb(1.0 / 3.0, 1.0, 1.0, &red, &green, &blue);
  assert_true(fabs(red) < 1e-12 && fabs(green - 1.0) < 1e-12 && fabs(blue) < 1e-12);
  dt_color_well_hsv_to_rgb(2.0 / 3.0, 1.0, 1.0, &red, &green, &blue);
  assert_true(fabs(red) < 1e-12 && fabs(green) < 1e-12 && fabs(blue - 1.0) < 1e-12);
  // One turn of the hue is red again.
  dt_color_well_hsv_to_rgb(1.0, 1.0, 1.0, &red, &green, &blue);
  assert_true(red == 1.0 && green == 0.0 && blue == 0.0);
  // The field's corners: saturation across, value down.
  dt_color_well_hsv_to_rgb(0.25, 0.0, 1.0, &red, &green, &blue);
  assert_true(red == 1.0 && green == 1.0 && blue == 1.0);
  dt_color_well_hsv_to_rgb(0.25, 1.0, 0.0, &red, &green, &blue);
  assert_true(red == 0.0 && green == 0.0 && blue == 0.0);

  // Red with blue a hair above green: the hue is a hair below zero, which rounds to one once wrapped.
  double edge_hue = 0.0;
  double edge_saturation = 0.0;
  double edge_value = 0.0;
  dt_color_well_rgb_to_hsv(1.0, 0.5, nextafter(0.5, 1.0), &edge_hue, &edge_saturation, &edge_value);
  assert_true(edge_hue >= 0.0 && edge_hue < 1.0);

  GRand *random = g_rand_new_with_seed(4242u);
  double worst = 0.0;
  for(int idx = 0; idx < 10000; idx++)
  {
    const double source_red = g_rand_double(random);
    const double source_green = g_rand_double(random);
    const double source_blue = g_rand_double(random);
    double hue = 0.0;
    double saturation = 0.0;
    double value = 0.0;
    dt_color_well_rgb_to_hsv(source_red, source_green, source_blue, &hue, &saturation, &value);
    assert_true(hue >= 0.0 && hue < 1.0);
    dt_color_well_hsv_to_rgb(hue, saturation, value, &red, &green, &blue);
    worst = MAX(worst, fabs(red - source_red));
    worst = MAX(worst, fabs(green - source_green));
    worst = MAX(worst, fabs(blue - source_blue));
  }
  g_rand_free(random);
  assert_true(worst < 1e-12);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_hex_reads_every_form_css_does),
    cmocka_unit_test(_hex_refuses_what_is_not_a_colour_and_touches_nothing),
    cmocka_unit_test(_hex_writes_the_alpha_only_when_there_is_one),
    cmocka_unit_test(_a_recent_colour_moves_to_the_head_rather_than_repeating),
    cmocka_unit_test(_a_change_is_one_the_byte_shows),
    cmocka_unit_test(_the_list_is_cut_at_its_length_most_recent_first),
    cmocka_unit_test(_the_stored_list_reads_back_and_skips_what_it_cannot_read),
    cmocka_unit_test(_hue_saturation_and_value_round_trip_and_name_the_primaries),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
