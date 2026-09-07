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

/* The stroke rasteriser, pixel by pixel.
 *
 * Every expectation here is a pixel value read back from an ARGB32 surface, because the
 * rasteriser's whole contract is what it puts in pixels: a band of the right width at the
 * right place, a round or flat end, a gap where a dash pattern says so, the bright pass over
 * the dark one, and -- the one that bites -- the same place cairo would put it when drawing
 * inside a pushed group, whose surface is offset from the target's device space. */

#include "widgets/stroke_raster.h"

#include <cairo.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>

static uint32_t _pixel(cairo_surface_t *surface, const int x, const int y)
{
  cairo_surface_flush(surface);
  const uint8_t *data = cairo_image_surface_get_data(surface);
  const int stride = cairo_image_surface_get_stride(surface);
  return *(const uint32_t *)(data + (size_t)y * stride + (size_t)x * 4);
}

static int _alpha(cairo_surface_t *surface, const int x, const int y)
{
  return (int)(_pixel(surface, x, y) >> 24);
}

static dt_stroke_style_t _solid(const double width, const double r, const double g, const double b)
{
  dt_stroke_style_t style = { 0 };
  style.dark = (dt_stroke_pass_t){ .width = width, .red = r, .green = g, .blue = b, .alpha = 1.0 };
  style.round_caps = TRUE;
  return style;
}

static void _a_horizontal_line_paints_a_band_of_its_width(void **state)
{
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 100, 20);
  const double xy[] = { 10.5, 10.5, 90.5, 10.5 };   /* pixel centres */
  const dt_stroke_style_t style = _solid(4.0, 0.0, 0.0, 1.0);
  assert_true(dt_stroke_raster_polyline(s, xy, 2, &style));

  /* on the centreline: opaque blue, premultiplied */
  assert_int_equal(_pixel(s, 50, 10), 0xff0000ffu);
  /* one pixel off the centreline is still inside a 4-wide band (distance 1 < 2) */
  assert_int_equal(_alpha(s, 50, 11), 255);
  /* the ramp: distance 2 sits on the edge, half covered */
  const int edge = _alpha(s, 50, 12);
  assert_in_range(edge, 100, 155);
  /* distance 3 is beyond the ramp */
  assert_int_equal(_alpha(s, 50, 13), 0);
  /* a round cap: one pixel past the end is inside (distance 1), the pixel at distance 2 sits on
   * the cap's edge, and distance 3 is beyond it */
  assert_int_equal(_alpha(s, 9, 10), 255);
  assert_in_range(_alpha(s, 8, 10), 100, 155);
  assert_int_equal(_alpha(s, 7, 10), 0);

  cairo_rectangle_int_t touched;
  assert_true(dt_stroke_raster_touched(s, &touched));
  assert_true(touched.x <= 8 && touched.x + touched.width >= 93);
  assert_true(touched.y <= 8 && touched.y + touched.height >= 13);
  cairo_surface_destroy(s);
}

static void _flat_caps_stop_at_the_ends(void **state)
{
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 100, 20);
  const double xy[] = { 10.5, 10.5, 90.5, 10.5 };
  dt_stroke_style_t style = _solid(4.0, 0.0, 0.0, 1.0);
  style.round_caps = FALSE;
  assert_true(dt_stroke_raster_polyline(s, xy, 2, &style));
  assert_int_equal(_alpha(s, 50, 10), 255);
  assert_int_equal(_alpha(s, 8, 10), 0);    /* nothing past the end */
  assert_int_equal(_alpha(s, 11, 10), 255); /* the line itself is intact */
  cairo_surface_destroy(s);
}

static void _dashes_leave_gaps_along_the_line(void **state)
{
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 120, 20);
  const double xy[] = { 0.5, 10.5, 100.5, 10.5 };
  dt_stroke_style_t style = _solid(2.0, 1.0, 1.0, 1.0);
  style.dash_on = 10.0;
  style.dash_off = 10.0;
  style.round_caps = FALSE;
  assert_true(dt_stroke_raster_polyline(s, xy, 2, &style));
  assert_int_equal(_alpha(s, 5, 10), 255);    /* first dash, 0..10 */
  assert_int_equal(_alpha(s, 15, 10), 0);     /* first gap, 10..20 */
  assert_int_equal(_alpha(s, 25, 10), 255);   /* second dash */
  assert_int_equal(_alpha(s, 35, 10), 0);
  assert_int_equal(_alpha(s, 95, 10), 0);     /* 90..100 is a gap */
  cairo_surface_destroy(s);
}

static void _the_bright_pass_paints_over_the_dark_one(void **state)
{
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 100, 20);
  const double xy[] = { 10.5, 10.5, 90.5, 10.5 };
  dt_stroke_style_t style = _solid(6.0, 1.0, 0.0, 0.0);
  style.bright = (dt_stroke_pass_t){ .width = 2.0, .red = 0.0, .green = 1.0, .blue = 0.0, .alpha = 1.0 };
  assert_true(dt_stroke_raster_polyline(s, xy, 2, &style));
  /* centre: green won */
  assert_int_equal(_pixel(s, 50, 10), 0xff00ff00u);
  /* two pixels out: only the dark, red pass */
  assert_int_equal(_pixel(s, 50, 12), 0xffff0000u);
  cairo_surface_destroy(s);
}

static void _a_surface_it_cannot_write_is_refused_and_the_path_kept(void **state)
{
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 100, 20);
  cairo_t *cr = cairo_create(s);
  cairo_move_to(cr, 10, 10);
  cairo_line_to(cr, 90, 10);
  const dt_stroke_style_t style = _solid(4.0, 0.0, 0.0, 1.0);
  assert_false(dt_stroke_raster_path(cr, &style));
  /* the path is still there for cairo to stroke */
  assert_true(cairo_has_current_point(cr));
  cairo_path_t *path = cairo_copy_path(cr);
  assert_true(path->num_data > 0);
  cairo_path_destroy(path);
  cairo_destroy(cr);
  cairo_surface_destroy(s);
}

static void _a_path_in_a_pushed_group_lands_where_cairo_puts_it(void **state)
{
  /* The group's surface is sized to the clip and offset to it; a stroke painted into that
   * surface must still appear at the path's device position once the group is composited. */
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 100, 100);
  cairo_t *cr = cairo_create(s);
  cairo_rectangle(cr, 20, 20, 60, 60);
  cairo_clip(cr);
  cairo_push_group(cr);
  cairo_move_to(cr, 30, 50);
  cairo_line_to(cr, 70, 50);
  const dt_stroke_style_t style = _solid(4.0, 0.0, 0.0, 1.0);
  assert_true(dt_stroke_raster_path(cr, &style));
  assert_false(cairo_has_current_point(cr));   /* consumed, as a stroke would */
  cairo_pop_group_to_source(cr);
  cairo_paint(cr);
  cairo_destroy(cr);

  assert_int_equal(_alpha(s, 50, 50), 255);   /* on the line */
  assert_int_equal(_alpha(s, 50, 45), 0);     /* well off it */
  assert_int_equal(_alpha(s, 10, 50), 0);     /* outside the clip */
  assert_int_equal(_alpha(s, 30, 50), 255);   /* the start, inside the clip */
  cairo_surface_destroy(s);
}

static void _the_matrix_scales_widths_and_positions(void **state)
{
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 120, 60);
  cairo_t *cr = cairo_create(s);
  cairo_scale(cr, 2.0, 2.0);
  cairo_move_to(cr, 10.25, 10.25);   /* device (20.5, 20.5): a pixel centre */
  cairo_line_to(cr, 40.25, 10.25);
  dt_stroke_style_t style = _solid(2.0, 0.0, 0.0, 1.0);   /* 2 user units = 4 device pixels */
  assert_true(dt_stroke_raster_path(cr, &style));
  cairo_destroy(cr);
  assert_int_equal(_alpha(s, 50, 20), 255);
  assert_int_equal(_alpha(s, 50, 21), 255);   /* still inside a 4-wide band */
  assert_int_equal(_alpha(s, 50, 23), 0);     /* three pixels out: beyond it */
  assert_int_equal(_alpha(s, 50, 10), 0);     /* the user-space y would have been wrong here */
  cairo_surface_destroy(s);
}

static void _the_touched_record_resets(void **state)
{
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 100, 20);
  cairo_rectangle_int_t touched;
  assert_false(dt_stroke_raster_touched(s, &touched));
  const double xy[] = { 10.5, 10.5, 20.5, 10.5 };
  const dt_stroke_style_t style = _solid(2.0, 1.0, 1.0, 1.0);
  assert_true(dt_stroke_raster_polyline(s, xy, 2, &style));
  assert_true(dt_stroke_raster_touched(s, &touched));
  dt_stroke_raster_touched_reset(s);
  assert_false(dt_stroke_raster_touched(s, &touched));
  cairo_surface_destroy(s);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_a_horizontal_line_paints_a_band_of_its_width),
    cmocka_unit_test(_flat_caps_stop_at_the_ends),
    cmocka_unit_test(_dashes_leave_gaps_along_the_line),
    cmocka_unit_test(_the_bright_pass_paints_over_the_dark_one),
    cmocka_unit_test(_a_surface_it_cannot_write_is_refused_and_the_path_kept),
    cmocka_unit_test(_a_path_in_a_pushed_group_lands_where_cairo_puts_it),
    cmocka_unit_test(_the_matrix_scales_widths_and_positions),
    cmocka_unit_test(_the_touched_record_resets),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
