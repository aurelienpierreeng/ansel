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

/* The cutout masks and the linear compositor: the drawn-mask shapes rasterised in a frame's
 * unit square with no darkroom behind them, and the painter blending in linear light. */

#include "caches/pixelpipe_cache.h"
#include "canvas/canvas.h"
#include "canvas/canvas_paint.h"
#include "canvas/canvas_render.h"
#include "develop/masks_cutout.h"

#include <cairo.h>
#include <glib.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <cmocka.h>

#define SIZE 100

static float _at(const float *raster, const int x, const int y)
{
  return raster[y * SIZE + x];
}

static void _a_circle_is_full_inside_empty_outside_and_feathers_between(void **state)
{
  (void)state;
  dt_masks_cutout_t cutout;
  memset(&cutout, 0, sizeof(cutout));
  cutout.shape = DT_MASKS_CUTOUT_CIRCLE;
  cutout.center[0] = 0.5f;
  cutout.center[1] = 0.5f;
  cutout.radius[0] = 0.25f;
  cutout.feather = 0.0f;
  float *raster = dt_masks_cutout_rasterise(&cutout, SIZE, SIZE);
  assert_non_null(raster);
  assert_float_equal(_at(raster, 50, 50), 1.0f, 1e-3);
  assert_float_equal(_at(raster, 2, 2), 0.0f, 1e-3);
  assert_float_equal(_at(raster, 50, 90), 0.0f, 1e-3); // 40 px out, radius 25
  dt_masks_cutout_free(raster);

  // A feather of a quarter side: half-way through it the mask is neither full nor empty.
  cutout.feather = 0.25f;
  raster = dt_masks_cutout_rasterise(&cutout, SIZE, SIZE);
  assert_non_null(raster);
  assert_float_equal(_at(raster, 50, 50), 1.0f, 1e-3);
  const float mid = _at(raster, 50, 50 + 37);
  assert_true(mid > 0.05f && mid < 0.95f);
  assert_true(_at(raster, 50, 50 + 20) > mid);
  dt_masks_cutout_free(raster);

  // Inverted: the centre is what goes.
  cutout.invert = TRUE;
  raster = dt_masks_cutout_rasterise(&cutout, SIZE, SIZE);
  assert_non_null(raster);
  assert_float_equal(_at(raster, 50, 50), 0.0f, 1e-3);
  assert_float_equal(_at(raster, 2, 2), 1.0f, 1e-3);
  dt_masks_cutout_free(raster);
}

static void _a_polygon_fills_its_interior(void **state)
{
  (void)state;
  const float nodes[4 * DT_MASKS_CUTOUT_NODE_FLOATS] = { 0.2f, 0.2f, 0.2f, 0.2f, 0.2f, 0.2f, 0.0f,
                                                         0.8f, 0.2f, 0.8f, 0.2f, 0.8f, 0.2f, 0.0f,
                                                         0.8f, 0.8f, 0.8f, 0.8f, 0.8f, 0.8f, 0.0f,
                                                         0.2f, 0.8f, 0.2f, 0.8f, 0.2f, 0.8f, 0.0f };
  dt_masks_cutout_t cutout;
  memset(&cutout, 0, sizeof(cutout));
  cutout.shape = DT_MASKS_CUTOUT_POLYGON;
  cutout.node_count = 4;
  cutout.node_stride = DT_MASKS_CUTOUT_NODE_FLOATS;
  cutout.nodes = nodes;
  cutout.feather = 0.05f;
  float *raster = dt_masks_cutout_rasterise(&cutout, SIZE, SIZE);
  assert_non_null(raster);
  assert_float_equal(_at(raster, 50, 50), 1.0f, 1e-3);
  assert_float_equal(_at(raster, 30, 70), 1.0f, 1e-3);
  assert_float_equal(_at(raster, 5, 5), 0.0f, 1e-3);
  assert_float_equal(_at(raster, 95, 50), 0.0f, 1e-3);
  dt_masks_cutout_free(raster);

  // Fewer than three nodes is not a shape.
  cutout.node_count = 2;
  assert_null(dt_masks_cutout_rasterise(&cutout, SIZE, SIZE));
}

static void _a_gradient_fades_across_its_line(void **state)
{
  (void)state;
  dt_masks_cutout_t cutout;
  memset(&cutout, 0, sizeof(cutout));
  cutout.shape = DT_MASKS_CUTOUT_GRADIENT;
  cutout.center[0] = 0.5f;
  cutout.center[1] = 0.5f;
  cutout.radius[0] = 0.25f; // the extent
  cutout.rotation = 0.0f;
  float *raster = dt_masks_cutout_rasterise(&cutout, SIZE, SIZE);
  assert_non_null(raster);
  // One side of the line is kept whole, the other fades out; both edges are the extremes.
  const float top = _at(raster, 50, 2);
  const float bottom = _at(raster, 50, 97);
  assert_true(fabsf(top - bottom) > 0.9f);
  assert_true(fmaxf(top, bottom) > 0.95f);
  assert_true(fminf(top, bottom) < 0.05f);
  // Along the line nothing changes.
  assert_float_equal(_at(raster, 5, 30), _at(raster, 95, 30), 1e-3);
  dt_masks_cutout_free(raster);
}

static void _the_object_mask_surface_matches_the_raster(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *frame = dt_canvas_add_text(canvas, 0.0, 0.0, 200.0, 100.0, "");
  dt_canvas_mask_set_shape(canvas, frame, DT_CANVAS_MASK_CIRCLE);
  frame->mask.feather = 0.0f;
  cairo_surface_t *surface = dt_canvas_render_mask(frame, 200, 100);
  assert_non_null(surface);
  assert_int_equal(cairo_image_surface_get_format(surface), CAIRO_FORMAT_A8);
  const uint8_t *pixels = cairo_image_surface_get_data(surface);
  const int stride = cairo_image_surface_get_stride(surface);
  // Radius 0.45 of the shorter side (100): the centre is in, 60 px to the right is out.
  assert_int_equal(pixels[50 * stride + 100], 255);
  assert_int_equal(pixels[50 * stride + 175], 0);
  cairo_surface_destroy(surface);
  // A hash keyed cache answers the same surface for the same mask and size, another after an edit.
  dt_canvas_surface_cache_t *cache = dt_canvas_surface_cache_new(FALSE, 64 * 1024 * 1024);
  cairo_surface_t *first = dt_canvas_surface_cache_get_mask(cache, frame, 200, 100);
  assert_ptr_equal(first, dt_canvas_surface_cache_get_mask(cache, frame, 200, 100));
  frame->mask.radius_x = 0.2f;
  assert_ptr_not_equal(first, dt_canvas_surface_cache_get_mask(cache, frame, 200, 100));
  dt_canvas_surface_cache_free(cache);
  dt_canvas_free(canvas);
}

/** Paint a canvas at one pixel per unit into a square surface, and read a pixel back. */
static uint32_t _painted_pixel(const dt_canvas_t *canvas, const int size, const int x, const int y)
{
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, size, size);
  cairo_t *cr = cairo_create(surface);
  // Canvas (0, 0) at the middle of the surface.
  cairo_translate(cr, size * 0.5, size * 0.5);
  const dt_canvas_rect_t whole = { -size * 0.5, -size * 0.5, (double)size, (double)size };
  dt_canvas_paint_options_t options = dt_canvas_paint_options_export(NULL, 1.0, whole);
  dt_canvas_paint(cr, canvas, &options);
  cairo_destroy(cr);
  cairo_surface_flush(surface);
  const uint8_t *pixels = cairo_image_surface_get_data(surface);
  const int stride = cairo_image_surface_get_stride(surface);
  const uint32_t pixel = *(const uint32_t *)(pixels + (size_t)y * stride + (size_t)x * 4) & 0xFFFFFFu;
  cairo_surface_destroy(surface);
  return pixel;
}

static void _the_compositor_blends_in_linear_light_and_round_trips_opaque_codes(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  canvas->background = dt_canvas_color(0.0f, 0.0f, 0.0f, 1.0f);
  canvas->grid_flags = 0;
  canvas->paper_size = DT_CANVAS_PAPER_NONE;
  // A white frame at half opacity over black: half the light, which sRGB encodes as 188, not 128.
  dt_canvas_object_t *frame = dt_canvas_add_text(canvas, 0.0, 0.0, 100.0, 100.0, "");
  frame->text.background = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
  frame->border_width = 0.0f;
  frame->flags |= DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE;
  frame->transparency = 0.5f;
  const uint32_t inside = _painted_pixel(canvas, 200, 100, 100);
  const int code = (int)(inside & 0xFF);
  assert_true(code >= 186 && code <= 189);
  assert_int_equal((inside >> 8) & 0xFF, code);
  // Outside the frame the background comes back as the exact code it was given.
  assert_int_equal(_painted_pixel(canvas, 200, 5, 5), 0x000000u);
  // Codes cairo lands on exactly (multiples of 1/255), so the round trip is judged and not cairo's rounding.
  canvas->background = dt_canvas_color(0.2f, 0.6f, 0.8f, 1.0f);
  const uint32_t background = _painted_pixel(canvas, 200, 5, 5);
  assert_int_equal((background >> 16) & 0xFF, 51);
  assert_int_equal((background >> 8) & 0xFF, 153);
  assert_int_equal(background & 0xFF, 204);
  // Fully opaque, the frame's white comes back as white.
  frame->transparency = 0.0f;
  assert_int_equal(_painted_pixel(canvas, 200, 100, 100), 0xFFFFFFu);

  // A circle cutout: the frame's corner is inside its rectangle and outside its shape.
  dt_canvas_mask_set_shape(canvas, frame, DT_CANVAS_MASK_CIRCLE);
  frame->mask.feather = 0.0f;
  assert_int_equal(_painted_pixel(canvas, 200, 100, 100), 0xFFFFFFu);
  assert_int_equal(_painted_pixel(canvas, 200, 53, 53), background);
  dt_canvas_mask_set_shape(canvas, frame, DT_CANVAS_MASK_NONE);

  // A shadow: offset down and right, with no blur, it darkens what lies past the frame's corner.
  frame->shadow.color = dt_canvas_color(0.0f, 0.0f, 0.0f, 1.0f);
  frame->shadow.offset_x = 20.0f;
  frame->shadow.offset_y = 20.0f;
  frame->shadow.blur = 0.0f;
  frame->flags |= DT_CANVAS_OBJECT_FLAG_SHADOW_OVERRIDE;
  assert_int_equal(_painted_pixel(canvas, 200, 160, 160), 0x000000u);
  assert_int_equal(_painted_pixel(canvas, 200, 175, 175), background);
  // Blurred, the same spot is a shade between the shadow and the background.
  frame->shadow.blur = 8.0f;
  const uint32_t soft = _painted_pixel(canvas, 200, 168, 168);
  assert_true((soft & 0xFF) > 0 && (soft & 0xFF) < (background & 0xFF));
  dt_canvas_free(canvas);
}

/* The shapes take their working buffers from the pixelpipe cache's arena, the one the
 * darkroom hands them; a headless consumer needs that arena too, and nothing else of dt_init(). */
static int _group_setup(void **state)
{
  (void)state;
  return dt_dev_pixelpipe_cache_init(64u * 1024u * 1024u, FALSE, FALSE) ? 0 : 1;
}

static int _group_teardown(void **state)
{
  (void)state;
  dt_dev_pixelpipe_cache_cleanup();
  return 0;
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_a_circle_is_full_inside_empty_outside_and_feathers_between),
    cmocka_unit_test(_a_polygon_fills_its_interior),
    cmocka_unit_test(_a_gradient_fades_across_its_line),
    cmocka_unit_test(_the_object_mask_surface_matches_the_raster),
    cmocka_unit_test(_the_compositor_blends_in_linear_light_and_round_trips_opaque_codes),
  };
  return cmocka_run_group_tests(tests, _group_setup, _group_teardown);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
