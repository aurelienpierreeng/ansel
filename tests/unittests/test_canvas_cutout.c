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
#include "canvas/canvas_format.h"
#include "canvas/canvas_paint.h"
#include "canvas/canvas_render.h"
#include "develop/masks_cutout.h"

#include <cairo.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <unistd.h>
#include <jpeglib.h>
#include <math.h>
// cmocka.h declares `extern jmp_buf global_expect_assert_env' without including <setjmp.h>.
#include <setjmp.h>  // NOLINT(misc-include-cleaner)
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cmocka.h>

#define SIZE 100

/**
 * The code an sRGB colour lands on in the canvas's layer encoding, Adobe RGB (1998), the way
 * the painter's own path would not compute it: the sRGB curve out, the standard matrix, the
 * 563/256 gamma in, then cairo's quantisation (sixteen bits, rounded, then the high byte).
 */
static uint32_t _layer_code(const double red, const double green, const double blue)
{
  const double srgb[3] = { red, green, blue };
  double linear[3];
  for(int channel = 0; channel < 3; channel++)
    linear[channel] = srgb[channel] <= 0.04045 ? srgb[channel] / 12.92 : pow((srgb[channel] + 0.055) / 1.055, 2.4);
  const double adobe[3] = { 0.7151658 * linear[0] + 0.2848342 * linear[1], linear[1],
                            0.0411705 * linear[1] + 0.9588295 * linear[2] };
  uint32_t codes[3];
  for(int channel = 0; channel < 3; channel++)
  {
    const double encoded = pow(fmin(fmax(adobe[channel], 0.0), 1.0), 256.0 / 563.0);
    codes[channel] = (uint32_t)floor(encoded * 65535.0 + 0.5) >> 8;
  }
  return (codes[0] << 16) | (codes[1] << 8) | codes[2];
}

static gboolean _within(const uint32_t pixel, const uint32_t expected, const int tolerance)
{
  for(int shift = 0; shift <= 16; shift += 8)
  {
    const int got = (int)((pixel >> shift) & 0xFF);
    const int wanted = (int)((expected >> shift) & 0xFF);
    if(abs(got - wanted) > tolerance) return FALSE;
  }
  return TRUE;
}

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
  // By index, not as a flat run: the record may gain a field, and a fixture that assumed its
  // width would then feed every node the next one's numbers.
  const float corners[4][2] = { { 0.2f, 0.2f }, { 0.8f, 0.2f }, { 0.8f, 0.8f }, { 0.2f, 0.8f } };
  float nodes[4 * DT_MASKS_CUTOUT_NODE_FLOATS];
  memset(nodes, 0, sizeof(nodes));
  for(int node = 0; node < 4; node++)
  {
    float *record = nodes + (size_t)node * DT_MASKS_CUTOUT_NODE_FLOATS;
    record[DT_MASKS_CUTOUT_NODE_X] = corners[node][0];
    record[DT_MASKS_CUTOUT_NODE_Y] = corners[node][1];
    record[DT_MASKS_CUTOUT_NODE_CTRL1_X] = corners[node][0];
    record[DT_MASKS_CUTOUT_NODE_CTRL1_Y] = corners[node][1];
    record[DT_MASKS_CUTOUT_NODE_CTRL2_X] = corners[node][0];
    record[DT_MASKS_CUTOUT_NODE_CTRL2_Y] = corners[node][1];
  }
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

/**
 * A node's record says where its curve comes from, and there are THREE answers, not two: its
 * own control points (a cusp when they coincide, a smooth node when the caller keeps them
 * collinear), or a tangent computed from its neighbours. The entry used to read the field as
 * "non-zero means computed", which threw away every tangent a user had steered -- the handles
 * moved the outline on screen and the cut did not follow.
 */
static void _a_polygon_node_steers_its_own_curve(void **state)
{
  (void)state;
  const float corners[4][2] = { { 0.2f, 0.2f }, { 0.8f, 0.2f }, { 0.8f, 0.8f }, { 0.2f, 0.8f } };
  float nodes[4 * DT_MASKS_CUTOUT_NODE_FLOATS];
  dt_masks_cutout_t cutout;

  // A square whose nodes carry control points ON themselves: every edge is straight, so a
  // probe just outside the right edge is empty.
  for(int kind = 0; kind < 3; kind++)
  {
    const float kinds[3] = { 0.0f, 1.0f, 2.0f };
    memset(nodes, 0, sizeof(nodes));
    for(int node = 0; node < 4; node++)
    {
      float *record = nodes + (size_t)node * DT_MASKS_CUTOUT_NODE_FLOATS;
      record[DT_MASKS_CUTOUT_NODE_X] = corners[node][0];
      record[DT_MASKS_CUTOUT_NODE_Y] = corners[node][1];
      record[DT_MASKS_CUTOUT_NODE_CTRL1_X] = corners[node][0];
      record[DT_MASKS_CUTOUT_NODE_CTRL1_Y] = corners[node][1];
      record[DT_MASKS_CUTOUT_NODE_CTRL2_X] = corners[node][0];
      record[DT_MASKS_CUTOUT_NODE_CTRL2_Y] = corners[node][1];
      record[DT_MASKS_CUTOUT_NODE_SMOOTH] = kinds[kind];
    }
    memset(&cutout, 0, sizeof(cutout));
    cutout.shape = DT_MASKS_CUTOUT_POLYGON;
    cutout.node_count = 4;
    cutout.node_stride = DT_MASKS_CUTOUT_NODE_FLOATS;
    cutout.nodes = nodes;
    cutout.feather = 0.02f;
    float *raster = dt_masks_cutout_rasterise(&cutout, SIZE, SIZE);
    assert_non_null(raster);
    // The computed tangent of a square points along its diagonal, so a smoothed square bulges
    // past the middle of every edge; a square carrying its own coincident controls does not.
    // A STEERED node carries its own too, so it must read as the cusp and not as the computed
    // one -- that is the whole regression.
    if(kinds[kind] == 1.0f)
      assert_true(_at(raster, 86, 50) > 0.5f);
    else
      assert_float_equal(_at(raster, 86, 50), 0.0f, 1e-3);
    dt_masks_cutout_free(raster);
  }

  // And a steered node's own control points are what the cut follows: pulling the two
  // controls of the right edge outward bulges it, with the kind still saying "smooth".
  memset(nodes, 0, sizeof(nodes));
  for(int node = 0; node < 4; node++)
  {
    float *record = nodes + (size_t)node * DT_MASKS_CUTOUT_NODE_FLOATS;
    record[DT_MASKS_CUTOUT_NODE_X] = corners[node][0];
    record[DT_MASKS_CUTOUT_NODE_Y] = corners[node][1];
    record[DT_MASKS_CUTOUT_NODE_CTRL1_X] = corners[node][0];
    record[DT_MASKS_CUTOUT_NODE_CTRL1_Y] = corners[node][1];
    record[DT_MASKS_CUTOUT_NODE_CTRL2_X] = corners[node][0];
    record[DT_MASKS_CUTOUT_NODE_CTRL2_Y] = corners[node][1];
    record[DT_MASKS_CUTOUT_NODE_SMOOTH] = 2.0f;
  }
  // Node 1 leaves towards node 2 and node 2 arrives from node 1: both controls out to the right.
  nodes[1 * DT_MASKS_CUTOUT_NODE_FLOATS + DT_MASKS_CUTOUT_NODE_CTRL2_X] = 0.95f;
  nodes[1 * DT_MASKS_CUTOUT_NODE_FLOATS + DT_MASKS_CUTOUT_NODE_CTRL2_Y] = 0.2f;
  nodes[2 * DT_MASKS_CUTOUT_NODE_FLOATS + DT_MASKS_CUTOUT_NODE_CTRL1_X] = 0.95f;
  nodes[2 * DT_MASKS_CUTOUT_NODE_FLOATS + DT_MASKS_CUTOUT_NODE_CTRL1_Y] = 0.8f;
  memset(&cutout, 0, sizeof(cutout));
  cutout.shape = DT_MASKS_CUTOUT_POLYGON;
  cutout.node_count = 4;
  cutout.node_stride = DT_MASKS_CUTOUT_NODE_FLOATS;
  cutout.nodes = nodes;
  cutout.feather = 0.02f;
  float *steered = dt_masks_cutout_rasterise(&cutout, SIZE, SIZE);
  assert_non_null(steered);
  assert_true(_at(steered, 86, 50) > 0.5f);
  dt_masks_cutout_free(steered);
}

/**
 * A polygon node carries its own fall-off radius, and a node without one takes the shape's.
 * That is the darkroom's own per-node border, which the cutout entry now passes through.
 */
static void _a_polygon_node_carries_its_own_fall_off(void **state)
{
  (void)state;
  const float corners[4][2] = { { 0.25f, 0.25f }, { 0.75f, 0.25f }, { 0.75f, 0.75f }, { 0.25f, 0.75f } };
  float nodes[4 * DT_MASKS_CUTOUT_NODE_FLOATS];
  memset(nodes, 0, sizeof(nodes));
  for(int node = 0; node < 4; node++)
  {
    float *record = nodes + (size_t)node * DT_MASKS_CUTOUT_NODE_FLOATS;
    record[DT_MASKS_CUTOUT_NODE_X] = corners[node][0];
    record[DT_MASKS_CUTOUT_NODE_Y] = corners[node][1];
    record[DT_MASKS_CUTOUT_NODE_CTRL1_X] = corners[node][0];
    record[DT_MASKS_CUTOUT_NODE_CTRL1_Y] = corners[node][1];
    record[DT_MASKS_CUTOUT_NODE_CTRL2_X] = corners[node][0];
    record[DT_MASKS_CUTOUT_NODE_CTRL2_Y] = corners[node][1];
  }
  dt_masks_cutout_t cutout;
  memset(&cutout, 0, sizeof(cutout));
  cutout.shape = DT_MASKS_CUTOUT_POLYGON;
  cutout.node_count = 4;
  cutout.node_stride = DT_MASKS_CUTOUT_NODE_FLOATS;
  cutout.nodes = nodes;
  cutout.feather = 0.02f;

  // A tight fall-off everywhere: eight pixels out from the top edge is well past it.
  float *raster = dt_masks_cutout_rasterise(&cutout, SIZE, SIZE);
  assert_non_null(raster);
  const float tight = _at(raster, 30, 17);
  assert_float_equal(tight, 0.0f, 1e-3);
  dt_masks_cutout_free(raster);

  // The same shape with a wide fall-off on the two nodes of that edge reaches the same point.
  nodes[0 * DT_MASKS_CUTOUT_NODE_FLOATS + DT_MASKS_CUTOUT_NODE_BORDER1] = 0.2f;
  nodes[0 * DT_MASKS_CUTOUT_NODE_FLOATS + DT_MASKS_CUTOUT_NODE_BORDER2] = 0.2f;
  nodes[1 * DT_MASKS_CUTOUT_NODE_FLOATS + DT_MASKS_CUTOUT_NODE_BORDER1] = 0.2f;
  nodes[1 * DT_MASKS_CUTOUT_NODE_FLOATS + DT_MASKS_CUTOUT_NODE_BORDER2] = 0.2f;
  raster = dt_masks_cutout_rasterise(&cutout, SIZE, SIZE);
  assert_non_null(raster);
  const float wide = _at(raster, 30, 17);
  if(!(wide > tight + 0.05f))
  {
    print_error("a node's own fall-off changed nothing: %f against %f\n", (double)wide, (double)tight);
    fail();
  }
  // The shape is untouched where it is solid, and a node with no fall-off of its own still
  // takes the shape's: the bottom edge, whose nodes were left alone, is as tight as before.
  assert_float_equal(_at(raster, 50, 50), 1.0f, 1e-3);
  assert_float_equal(_at(raster, 50, 83), 0.0f, 1e-3);
  dt_masks_cutout_free(raster);
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
  cairo_surface_t *surface = dt_canvas_render_mask(frame, 200, 100, 0, 0);
  assert_non_null(surface);
  assert_int_equal(cairo_image_surface_get_format(surface), CAIRO_FORMAT_A8);
  const uint8_t *pixels = cairo_image_surface_get_data(surface);
  const int stride = cairo_image_surface_get_stride(surface);
  // Radius 0.45 of the shorter side (100): the centre is in, 60 px to the right is out, and the
  // edge itself, 45 px out, is part-covered: the alpha is anti-aliased, not a step.
  assert_int_equal(pixels[50 * stride + 100], 255);
  assert_int_equal(pixels[50 * stride + 175], 0);
  const int edge = pixels[50 * stride + 145];
  assert_true(edge > 0 && edge < 255);
  cairo_surface_destroy(surface);
  // An inset keeps the shape clear of the frame's edges by that much: the circle reaches
  // y = 5 on its own, and with ten pixels of inset its top is gone.
  cairo_surface_t *kept_in = dt_canvas_render_mask(frame, 200, 100, 10, 0);
  assert_non_null(kept_in);
  assert_int_equal(cairo_image_surface_get_width(kept_in), 200);
  const int kept_stride = cairo_image_surface_get_stride(kept_in);
  const uint8_t *kept_pixels = cairo_image_surface_get_data(kept_in);
  assert_int_equal(kept_pixels[7 * kept_stride + 100], 0);
  assert_int_equal(kept_pixels[15 * kept_stride + 100], 255);
  assert_int_equal(kept_pixels[50 * kept_stride + 100], 255);
  cairo_surface_destroy(kept_in);
  // A hash keyed cache answers the same surface for the same mask and size, another after an edit.
  dt_canvas_surface_cache_t *cache = dt_canvas_surface_cache_new(FALSE, 64 * 1024 * 1024);
  cairo_surface_t *first = dt_canvas_surface_cache_get_mask(cache, frame, 200, 100, 0, 0);
  assert_ptr_equal(first, dt_canvas_surface_cache_get_mask(cache, frame, 200, 100, 0, 0));
  frame->mask.radius_x = 0.2f;
  assert_ptr_not_equal(first, dt_canvas_surface_cache_get_mask(cache, frame, 200, 100, 0, 0));
  dt_canvas_surface_cache_free(cache);
  dt_canvas_free(canvas);
}

/**
 * Paint a canvas at one pixel per unit into a square surface of `size` device units, and read
 * a pixel back -- in the surface's own pixels, which a device scale of 2 doubles.
 */
static uint32_t _painted_pixel_scaled(const dt_canvas_t *canvas, const int size, const double device_scale,
                                      const int x, const int y)
{
  const int side = (int)(size * device_scale);
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, side, side);
  cairo_surface_set_device_scale(surface, device_scale, device_scale);
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

static uint32_t _painted_pixel(const dt_canvas_t *canvas, const int size, const int x, const int y)
{
  return _painted_pixel_scaled(canvas, size, 1.0, x, y);
}

static void _the_compositor_paints_the_surfaces_own_pixels_on_a_scaled_surface(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  canvas->background = dt_canvas_color(0.0f, 0.0f, 0.0f, 1.0f);
  canvas->grid_flags = 0;
  canvas->paper_size = DT_CANVAS_PAPER_NONE;
  // A white 100-unit frame centred on a 200-unit surface at device scale 2: its edge is at
  // pixel 100 and 300, sharp, not at 50 and 150 blown up.
  dt_canvas_object_t *frame = dt_canvas_add_text(canvas, 0.0, 0.0, 100.0, 100.0, "");
  frame->text.background = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
  frame->border_width = 0.0f;
  frame->flags |= DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE;
  assert_int_equal(_painted_pixel_scaled(canvas, 200, 2.0, 200, 200), 0xFFFFFFu);
  assert_int_equal(_painted_pixel_scaled(canvas, 200, 2.0, 101, 200), 0xFFFFFFu);
  assert_int_equal(_painted_pixel_scaled(canvas, 200, 2.0, 98, 200), 0x000000u);
  assert_int_equal(_painted_pixel_scaled(canvas, 200, 2.0, 298, 200), 0xFFFFFFu);
  assert_int_equal(_painted_pixel_scaled(canvas, 200, 2.0, 301, 200), 0x000000u);
  dt_canvas_free(canvas);
}

/**
 * Paint at `zoom` pixels per canvas unit through a surface cache the caller keeps, and read a
 * pixel back. The caches -- the cutout rasters, the pictures' sprites -- only engage when a
 * paint carries one, and only a caller that paints the same document more than once through
 * the same cache can catch one answering with the frame before.
 */
static uint32_t _painted_pixel_quality(const dt_canvas_t *canvas, dt_canvas_surface_cache_t *cache, const int size,
                                       const double zoom, const double quality, const int x, const int y)
{
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, size, size);
  cairo_t *cr = cairo_create(surface);
  cairo_translate(cr, size * 0.5, size * 0.5);
  cairo_scale(cr, zoom, zoom);
  const dt_canvas_rect_t whole = { -size * 0.5 / zoom, -size * 0.5 / zoom, size / zoom, size / zoom };
  dt_canvas_paint_options_t options = dt_canvas_paint_options_export(cache, 1.0 / zoom, whole);
  options.quality = quality;
  dt_canvas_paint(cr, canvas, &options);
  cairo_destroy(cr);
  cairo_surface_flush(surface);
  const uint8_t *pixels = cairo_image_surface_get_data(surface);
  const int stride = cairo_image_surface_get_stride(surface);
  const uint32_t pixel = *(const uint32_t *)(pixels + (size_t)y * stride + (size_t)x * 4) & 0xFFFFFFu;
  cairo_surface_destroy(surface);
  return pixel;
}

static uint32_t _painted_pixel_cached(const dt_canvas_t *canvas, dt_canvas_surface_cache_t *cache, const int size,
                                      const double zoom, const int x, const int y)
{
  return _painted_pixel_quality(canvas, cache, size, zoom, 1.0, x, y);
}

/** A flat sRGB JPEG of this size and colour, to hang on an image frame. */
static GBytes *_flat_jpeg(const int width, const int height, const uint8_t red, const uint8_t green,
                          const uint8_t blue)
{
  struct jpeg_compress_struct compress;
  struct jpeg_error_mgr error;
  compress.err = jpeg_std_error(&error);
  jpeg_create_compress(&compress);
  unsigned char *buffer = NULL;
  unsigned long size = 0;
  jpeg_mem_dest(&compress, &buffer, &size);
  compress.image_width = width;
  compress.image_height = height;
  compress.input_components = 3;
  compress.in_color_space = JCS_RGB;
  jpeg_set_defaults(&compress);
  jpeg_set_quality(&compress, 100, TRUE);
  jpeg_start_compress(&compress, TRUE);
  uint8_t *row = g_malloc((size_t)width * 3);
  for(int col = 0; col < width; col++)
  {
    row[3 * col + 0] = red;
    row[3 * col + 1] = green;
    row[3 * col + 2] = blue;
  }
  while(compress.next_scanline < compress.image_height)
  {
    JSAMPROW pointer = row;
    jpeg_write_scanlines(&compress, &pointer, 1);
  }
  jpeg_finish_compress(&compress);
  jpeg_destroy_compress(&compress);
  g_free(row);
  GBytes *jpeg = g_bytes_new(buffer, size);
  free(buffer);
  return jpeg;
}

/**
 * A picture fills its frame at every zoom. The painter blits a picture from a sprite scaled
 * once per size the screen shows, under an identity matrix, inside the frame's clip: a sprite
 * that does not reach the clip leaves a line of canvas along the frame's edge, and it comes
 * and goes with the sub-pixel position, which is what a pan or a zoom changes.
 */
static void _a_picture_reaches_its_frames_every_edge(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  canvas->background = dt_canvas_color(0.05f, 0.05f, 0.2f, 1.0f);
  canvas->grid_flags = 0;
  canvas->paper_size = DT_CANVAS_PAPER_NONE;
  canvas->border_width = 0.0f;
  dt_canvas_object_t *frame = dt_canvas_add_image(canvas, 0.0, 0.0, 64, 64);
  frame->width = 100.0;
  frame->height = 100.0;
  frame->border_width = 0.0f;
  frame->flags |= DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE;
  GBytes *jpeg = _flat_jpeg(64, 64, 0, 0, 0);
  dt_canvas_image_set_render(canvas, frame, jpeg, 64, 64, 0, 0, DT_CANVAS_COLORSPACE_SRGB);
  g_bytes_unref(jpeg);
  dt_canvas_surface_cache_t *cache = dt_canvas_surface_cache_new(FALSE, 64u * 1024u * 1024u);
  // The picture is black, the canvas white: any bright pixel well inside the frame is canvas
  // showing through where the picture should be.
  const double zooms[] = { 1.0, 1.5, 2.0, 0.75, 1.0, 2.5 };
  for(size_t idx = 0; idx < sizeof(zooms) / sizeof(zooms[0]); idx++)
  {
    for(int step = 0; step <= 4; step++)
    {
      frame->x = step * 0.2;
      dt_canvas_touch(canvas);
      const int size = 300;
      const double zoom = zooms[idx];
      // Two pixels inside each edge of the frame, in the surface's own pixels.
      const double half = 50.0 * zoom;
      const int left = (int)ceil(size * 0.5 + (frame->x - 50.0) * zoom) + 2;
      const int right = (int)floor(size * 0.5 + (frame->x + 50.0) * zoom) - 2;
      const int top = (int)ceil(size * 0.5 - half) + 2;
      const int bottom = (int)floor(size * 0.5 + half) - 2;
      const int probes[4][2] = { { left, size / 2 }, { right, size / 2 }, { size / 2, top }, { size / 2, bottom } };
      for(int probe = 0; probe < 4; probe++)
      {
        const uint32_t pixel = _painted_pixel_cached(canvas, cache, size, zoom, probes[probe][0], probes[probe][1]);
        if(((pixel >> 16) & 0xFF) > 96)
        {
          print_error("zoom %.2f offset %.1f: (%d, %d) is %06x, canvas showing through the picture\n", zoom,
                      frame->x, probes[probe][0], probes[probe][1], pixel);
          fail();
        }
      }
      // And no rim anywhere along the frame's edge: every pixel of the whole frame and the
      // ring of canvas around it is one of the two, or a blend, never brighter than both.
      const int span = (int)ceil(half) + 4;
      for(int row = size / 2 - span; row <= size / 2 + span; row++)
      {
        for(int col = size / 2 - span; col <= size / 2 + span; col++)
        {
          if(row < 0 || col < 0 || row >= size || col >= size) continue;
          const uint32_t pixel = _painted_pixel_cached(canvas, cache, size, zoom, col, row);
          for(int channel = 0; channel < 3; channel++)
          {
            const int shade = (pixel >> (8 * channel)) & 0xFF;
            if(shade > 0x40)
            {
              print_error("zoom %.2f offset %.1f: rim at (%d, %d) is %06x, brighter than the picture and the canvas\n",
                          zoom, frame->x, col, row, pixel);
              fail();
            }
          }
        }
      }
    }
  }
  dt_canvas_surface_cache_free(cache);
  dt_canvas_free(canvas);
}

/**
 * A cutout's edge is anti-aliased, at full resolution and at the reduced one a gesture paints
 * at. The raster is sized for the FULL frame -- a gesture reuses it rather than rasterising
 * every cutout again -- so at half quality it is four times finer than the pixels being
 * painted, and reading it at each pixel's centre alone lands the edge wherever the sample
 * happens to fall: whole rows step straight from one colour to the other, which is the
 * stair-stepped edge. The compositor averages the raster over each pixel's footprint instead.
 */
static void _a_cutouts_edge_is_anti_aliased_at_every_quality(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  canvas->background = dt_canvas_color(0.0f, 0.0f, 0.0f, 1.0f);
  canvas->grid_flags = 0;
  canvas->paper_size = DT_CANVAS_PAPER_NONE;
  dt_canvas_object_t *frame = dt_canvas_add_text(canvas, 0.0, 0.0, 100.0, 100.0, "");
  frame->text.background = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
  frame->border_width = 0.0f;
  frame->flags |= DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE;
  dt_canvas_mask_set_shape(canvas, frame, DT_CANVAS_MASK_CIRCLE);
  frame->mask.radius_x = 0.4f;
  frame->mask.feather = 0.0f;
  dt_canvas_surface_cache_t *cache = dt_canvas_surface_cache_new(FALSE, 64u * 1024u * 1024u);
  const double qualities[] = { 1.0, 0.5 };
  const double zooms[] = { 1.3, 2.1, 3.1 };
  for(size_t quality = 0; quality < sizeof(qualities) / sizeof(qualities[0]); quality++)
  {
    for(size_t idx = 0; idx < sizeof(zooms) / sizeof(zooms[0]); idx++)
    {
      const double zoom = zooms[idx];
      const int size = (int)lround(240.0 * zoom);
      const int centre = size / 2;
      // Down the circle's right flank: every row crosses the edge, so every row owes a pixel
      // that is neither the frame nor the canvas.
      int rows = 0;
      int stepped = 0;
      for(int row = centre - (int)lround(20.0 * zoom); row <= centre + (int)lround(20.0 * zoom); row++)
      {
        int intermediate = 0;
        for(int col = centre + (int)lround(30.0 * zoom); col <= centre + (int)lround(44.0 * zoom); col++)
        {
          const uint32_t shade = _painted_pixel_quality(canvas, cache, size, zoom, qualities[quality], col, row) & 0xFFu;
          if(shade > 8 && shade < 247) intermediate++;
        }
        rows++;
        if(intermediate == 0) stepped++;
      }
      if(stepped * 10 > rows)
      {
        print_error("quality %.2f zoom %.2f: %d of %d rows step straight across the cutout's edge\n",
                    qualities[quality], zoom, stepped, rows);
        fail();
      }
    }
  }
  dt_canvas_surface_cache_free(cache);
  dt_canvas_free(canvas);
}

/**
 * An edge between two colours is a blend of the two and can be neither brighter nor darker
 * than both. Cairo hands the compositor a partly covered pixel PREMULTIPLIED in eight bits,
 * so the decode divides the colour back out by a coverage that may be as low as 1/255 --
 * where a rounded 1 becomes a full 255. That is a bright rim one pixel wide along every frame
 * whose edge does not land on the pixel grid, which is why a pan or a zoom makes it come and go.
 */
static void _an_edge_pixel_stays_between_the_colours_it_blends(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  canvas->background = dt_canvas_color(0.8f, 0.1f, 0.1f, 1.0f);
  canvas->grid_flags = 0;
  canvas->paper_size = DT_CANVAS_PAPER_NONE;
  // Dark on bright: any rim brighter than the background is the decode's, not the picture's.
  dt_canvas_object_t *frame = dt_canvas_add_text(canvas, 0.0, 0.0, 100.0, 100.0, "");
  frame->text.background = dt_canvas_color(0.02f, 0.02f, 0.06f, 1.0f);
  frame->border_width = 0.0f;
  frame->flags |= DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE;
  dt_canvas_surface_cache_t *cache = dt_canvas_surface_cache_new(FALSE, 64u * 1024u * 1024u);
  const uint32_t ground = _painted_pixel_cached(canvas, cache, 200, 1.0, 5, 5);
  // Walk the frame's edge across the pixel grid: at some offset it covers a pixel partly.
  for(int step = 0; step <= 8; step++)
  {
    frame->x = step * 0.125;
    dt_canvas_touch(canvas);
    for(int y = 40; y < 160; y++)
    {
      for(int x = 40; x < 62; x++)
      {
        const uint32_t pixel = _painted_pixel_cached(canvas, cache, 200, 1.0, x, y);
        for(int channel = 0; channel < 3; channel++)
        {
          const int shade = (pixel >> (8 * channel)) & 0xFF;
          const int high = (ground >> (8 * channel)) & 0xFF;
          if(shade > high + 1)
          {
            print_error("edge at (%d, %d) offset %.3f: channel %d is %d, above the %d it blends into\n", x, y,
                        frame->x, channel, shade, high);
            fail();
          }
        }
      }
    }
  }
  dt_canvas_surface_cache_free(cache);
  dt_canvas_free(canvas);
}

/**
 * The painter keeps the frame it last composited and blits it again for a key it has already
 * seen. The document's generation is in that key, so an edit that bumps it is drawn at once.
 * This is what makes a drag follow the pointer: every motion that changes the document has to
 * touch it, or the gesture paints its first frame over and over.
 */
static void _an_edited_document_is_not_served_from_the_last_frame(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  canvas->background = dt_canvas_color(0.0f, 0.0f, 0.0f, 1.0f);
  canvas->grid_flags = 0;
  canvas->paper_size = DT_CANVAS_PAPER_NONE;
  dt_canvas_object_t *frame = dt_canvas_add_text(canvas, 0.0, 0.0, 60.0, 60.0, "");
  frame->text.background = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
  frame->border_width = 0.0f;
  frame->flags |= DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE;
  dt_canvas_surface_cache_t *cache = dt_canvas_surface_cache_new(FALSE, 16u * 1024u * 1024u);

  assert_int_equal(_painted_pixel_cached(canvas, cache, 200, 1.0, 100, 100), 0xFFFFFFu);
  // The same view, the same cache: only the document moved, and only its generation says so.
  frame->x = 70.0;
  dt_canvas_touch(canvas);
  assert_int_equal(_painted_pixel_cached(canvas, cache, 200, 1.0, 100, 100), 0x000000u);
  assert_int_equal(_painted_pixel_cached(canvas, cache, 200, 1.0, 170, 100), 0xFFFFFFu);
  // And back: a generation it has seen before is still a new one, never the frame it held.
  frame->x = 0.0;
  dt_canvas_touch(canvas);
  assert_int_equal(_painted_pixel_cached(canvas, cache, 200, 1.0, 100, 100), 0xFFFFFFu);
  dt_canvas_surface_cache_free(cache);
  dt_canvas_free(canvas);
}

/**
 * A cutout keeps the side it was told to keep at every zoom. The raster cache holds two
 * sizes per frame, so a zoom out and back reads the raster built for the first zoom: it must
 * be that frame's raster and not another's, and an inverted shape must not come back
 * right side out.
 */
static void _a_cutout_keeps_its_side_through_the_raster_cache(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  canvas->background = dt_canvas_color(0.0f, 0.0f, 0.0f, 1.0f);
  canvas->grid_flags = 0;
  canvas->paper_size = DT_CANVAS_PAPER_NONE;
  dt_canvas_object_t *frame = dt_canvas_add_text(canvas, 0.0, 0.0, 100.0, 100.0, "");
  frame->text.background = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
  frame->border_width = 0.0f;
  frame->flags |= DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE;
  dt_canvas_mask_set_shape(canvas, frame, DT_CANVAS_MASK_CIRCLE);
  frame->mask.radius_x = 0.25f;
  frame->mask.feather = 0.0f;
  dt_canvas_surface_cache_t *cache = dt_canvas_surface_cache_new(FALSE, 64u * 1024u * 1024u);

  // Kept inside: the centre is the frame, a point outside the circle is the canvas.
  const double zooms[] = { 1.0, 2.0, 1.0, 3.0, 1.0, 2.0 };
  for(size_t idx = 0; idx < sizeof(zooms) / sizeof(zooms[0]); idx++)
  {
    const int size = (int)lround(200.0 * zooms[idx]);
    const int centre = size / 2;
    const int outside = centre + (int)lround(40.0 * zooms[idx]);
    assert_int_equal(_painted_pixel_cached(canvas, cache, size, zooms[idx], centre, centre), 0xFFFFFFu);
    assert_int_equal(_painted_pixel_cached(canvas, cache, size, zooms[idx], outside, centre), 0x000000u);
  }
  // Inverted: the same points swap, and stay swapped at every zoom the cache has seen.
  frame->mask.flags |= DT_CANVAS_MASK_INVERT;
  dt_canvas_touch(canvas);
  for(size_t idx = 0; idx < sizeof(zooms) / sizeof(zooms[0]); idx++)
  {
    const int size = (int)lround(200.0 * zooms[idx]);
    const int centre = size / 2;
    const int outside = centre + (int)lround(40.0 * zooms[idx]);
    assert_int_equal(_painted_pixel_cached(canvas, cache, size, zooms[idx], centre, centre), 0x000000u);
    assert_int_equal(_painted_pixel_cached(canvas, cache, size, zooms[idx], outside, centre), 0xFFFFFFu);
  }
  dt_canvas_surface_cache_free(cache);
  dt_canvas_free(canvas);
}

static void _a_cut_frames_border_follows_the_cutout_outward(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  canvas->background = dt_canvas_color(0.0f, 0.0f, 0.0f, 1.0f);
  canvas->grid_flags = 0;
  canvas->paper_size = DT_CANVAS_PAPER_NONE;
  dt_canvas_object_t *frame = dt_canvas_add_text(canvas, 0.0, 0.0, 100.0, 100.0, "");
  frame->text.background = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
  frame->border_width = 10.0f;
  frame->border_color = dt_canvas_color(1.0f, 0.0f, 0.0f, 1.0f);
  frame->flags |= DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE;
  const uint32_t red = _layer_code(1.0, 0.0, 0.0);
  const uint32_t blue = _layer_code(0.0, 0.0, 1.0);
  // Rectangular: the border sits inside the edge, the content within it.
  assert_int_equal(_painted_pixel(canvas, 200, 100, 100), 0xFFFFFFu);
  assert_true(_within(_painted_pixel(canvas, 200, 53, 100), red, 1));
  assert_int_equal(_painted_pixel(canvas, 200, 47, 100), 0x000000u);
  // Cut to a circle of radius 25: the content fills the circle, the border is the ring
  // 25 to 35 out from the centre, and past the ring is the background.
  dt_canvas_mask_set_shape(canvas, frame, DT_CANVAS_MASK_CIRCLE);
  frame->mask.radius_x = 0.25f;
  frame->mask.feather = 0.0f;
  assert_int_equal(_painted_pixel(canvas, 200, 100, 100), 0xFFFFFFu);
  assert_int_equal(_painted_pixel(canvas, 200, 120, 100), 0xFFFFFFu);
  assert_true(_within(_painted_pixel(canvas, 200, 130, 100), red, 1));
  assert_true(_within(_painted_pixel(canvas, 200, 100, 130), red, 1));
  assert_true(_within(_painted_pixel(canvas, 200, 122, 122), red, 1)); // 31 out along the diagonal: a disc, not a square
  assert_int_equal(_painted_pixel(canvas, 200, 138, 100), 0x000000u);
  assert_int_equal(_painted_pixel(canvas, 200, 53, 53), 0x000000u);
  // Feathered by 10: the border starts where the feather ends, at 35, not where the shape's
  // edge is, and the background fills the shape's whole support, feather included, solid.
  frame->mask.feather = 0.1f;
  frame->text.background = dt_canvas_color(0.0f, 0.0f, 1.0f, 1.0f);
  assert_true(_within(_painted_pixel(canvas, 200, 100, 100), blue, 1));
  assert_true(_within(_painted_pixel(canvas, 200, 130, 100), blue, 1));
  assert_true(_within(_painted_pixel(canvas, 200, 140, 100), red, 1));
  assert_int_equal(_painted_pixel(canvas, 200, 148, 100), 0x000000u);
  // Without a background the feather dissolves into nothing; the border still starts past it.
  frame->text.background.alpha = 0.0f;
  assert_int_equal(_painted_pixel(canvas, 200, 130, 100), 0x000000u);
  assert_true(_within(_painted_pixel(canvas, 200, 140, 100), red, 1));
  // A gradient covers the whole frame: its border is the frame's own ring, square corners
  // and all -- the band is dilated from the shape and stopped at the frame, not rounded.
  dt_canvas_mask_set_shape(canvas, frame, DT_CANVAS_MASK_GRADIENT);
  frame->text.background = dt_canvas_color(0.0f, 0.0f, 1.0f, 1.0f);
  assert_true(_within(_painted_pixel(canvas, 200, 53, 53), red, 1));
  assert_true(_within(_painted_pixel(canvas, 200, 146, 53), red, 1));
  assert_int_equal(_painted_pixel(canvas, 200, 47, 47), 0x000000u);
  dt_canvas_free(canvas);
}

static void _rounded_corners_round_the_frame_and_its_border(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  canvas->background = dt_canvas_color(0.0f, 0.0f, 0.0f, 1.0f);
  canvas->grid_flags = 0;
  canvas->paper_size = DT_CANVAS_PAPER_NONE;
  canvas->corner_radius = 20.0f;
  dt_canvas_object_t *frame = dt_canvas_add_text(canvas, 0.0, 0.0, 100.0, 100.0, "");
  frame->text.background = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
  frame->border_width = 0.0f;
  frame->flags |= DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE;
  // The frame spans 50..150; the corner's arc is centred 20 in: (52, 52) is outside it, (60, 60) inside.
  assert_int_equal(_painted_pixel(canvas, 200, 52, 52), 0x000000u);
  assert_int_equal(_painted_pixel(canvas, 200, 60, 60), 0xFFFFFFu);
  assert_int_equal(_painted_pixel(canvas, 200, 100, 52), 0xFFFFFFu);
  // The object's own radius overrides the canvas's: square again.
  frame->corner_radius = 0.0f;
  frame->flags |= DT_CANVAS_OBJECT_FLAG_CORNER_OVERRIDE;
  assert_int_equal(_painted_pixel(canvas, 200, 52, 52), 0xFFFFFFu);
  // A cut frame's border follows the rounded frame too: a gradient cutout with a border.
  frame->flags &= ~DT_CANVAS_OBJECT_FLAG_CORNER_OVERRIDE;
  frame->border_width = 10.0f;
  frame->border_color = dt_canvas_color(1.0f, 0.0f, 0.0f, 1.0f);
  dt_canvas_mask_set_shape(canvas, frame, DT_CANVAS_MASK_GRADIENT);
  assert_int_equal(_painted_pixel(canvas, 200, 52, 52), 0x000000u);
  assert_true(_within(_painted_pixel(canvas, 200, 100, 53), _layer_code(1.0, 0.0, 0.0), 1));
  dt_canvas_free(canvas);
}

static void _a_background_fills_the_frame_under_a_missing_render(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  canvas->background = dt_canvas_color(0.0f, 0.0f, 0.0f, 1.0f);
  canvas->grid_flags = 0;
  canvas->paper_size = DT_CANVAS_PAPER_NONE;
  dt_canvas_object_t *image = dt_canvas_add_image(canvas, 0.0, 0.0, 100, 100);
  image->width = 100.0;
  image->height = 100.0;
  image->border_width = 0.0f;
  image->flags |= DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE;
  // No render and no background: nothing but the canvas.
  assert_int_equal(_painted_pixel(canvas, 200, 100, 100), 0x000000u);
  image->background = dt_canvas_color(0.0f, 1.0f, 0.0f, 1.0f);
  assert_true(_within(_painted_pixel(canvas, 200, 100, 100), _layer_code(0.0, 1.0, 0.0), 1));
  assert_int_equal(_painted_pixel(canvas, 200, 5, 5), 0x000000u);
  dt_canvas_free(canvas);
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
  // Half the light, re-encoded with the 563/256 gamma: 0.5 ^ (256/563) is 0.7297, code 186.
  const uint32_t inside = _painted_pixel(canvas, 200, 100, 100);
  for(int shift = 0; shift <= 16; shift += 8)
  {
    const int code = (int)((inside >> shift) & 0xFF);
    assert_true(code >= 185 && code <= 187);
  }
  // Outside the frame the background comes back as the exact code it was given.
  assert_int_equal(_painted_pixel(canvas, 200, 5, 5), 0x000000u);
  // An opaque colour comes back as the code cairo painted it in the layer encoding: the
  // decode and the encode are exact inverses on every code.
  canvas->background = dt_canvas_color(0.2f, 0.6f, 0.8f, 1.0f);
  const uint32_t background = _painted_pixel(canvas, 200, 5, 5);
  assert_true(_within(background, _layer_code(0.2, 0.6, 0.8), 1));
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
  // No blur and no extent is no shadow at all, whatever the offset says.
  assert_int_equal(_painted_pixel(canvas, 200, 160, 160), background);
  // Hardly blurred, the shadow is where the frame's silhouette lands, offset.
  frame->shadow.blur = 0.4f;
  assert_int_equal(_painted_pixel(canvas, 200, 160, 160), 0x000000u);
  assert_int_equal(_painted_pixel(canvas, 200, 175, 175), background);
  // Blurred, the same spot is a shade between the shadow and the background.
  frame->shadow.blur = 8.0f;
  const uint32_t soft = _painted_pixel(canvas, 200, 168, 168);
  assert_true((soft & 0xFF) > 0 && (soft & 0xFF) < (background & 0xFF));
  // Cast inside, the shadow falls along the frame's own edges: the frame's corner opposite the
  // offset darkens, its middle does not, and nothing lands outside.
  frame->shadow.inset = TRUE;
  assert_int_equal(_painted_pixel(canvas, 200, 168, 168), background);
  const uint32_t inner = _painted_pixel(canvas, 200, 54, 54);
  assert_true((inner & 0xFF) < 0xFF);
  assert_int_equal(_painted_pixel(canvas, 200, 100, 100), 0xFFFFFFu);
  // With no offset, the shadow along an edge is as deep at the frame's own edge as it is on
  // a cutout's edge well inside the frame: the world past the frame is uncovered, and the
  // blur must not read it as covered. A pixel two in from the left edge, mid-height...
  frame->shadow.offset_x = 0.0f;
  frame->shadow.offset_y = 0.0f;
  const uint32_t at_frame_edge = _painted_pixel(canvas, 200, 52, 100);
  // ...against the same distance inside a square cutout's straight edge, 25 in from the frame
  // (a curved edge would legitimately differ: more uncovered world around it).
  const float square_corners[4][2] = { { 0.25f, 0.25f }, { 0.75f, 0.25f }, { 0.75f, 0.75f }, { 0.25f, 0.75f } };
  float square[4 * DT_CANVAS_MASK_NODE_FLOATS];
  memset(square, 0, sizeof(square));
  for(int node = 0; node < 4; node++)
  {
    float *record = square + (size_t)node * DT_CANVAS_MASK_NODE_FLOATS;
    record[DT_CANVAS_MASK_NODE_X] = square_corners[node][0];
    record[DT_CANVAS_MASK_NODE_Y] = square_corners[node][1];
    record[DT_CANVAS_MASK_NODE_CTRL1_X] = square_corners[node][0];
    record[DT_CANVAS_MASK_NODE_CTRL1_Y] = square_corners[node][1];
    record[DT_CANVAS_MASK_NODE_CTRL2_X] = square_corners[node][0];
    record[DT_CANVAS_MASK_NODE_CTRL2_Y] = square_corners[node][1];
  }
  dt_canvas_mask_set_shape(canvas, frame, DT_CANVAS_MASK_POLYGON);
  dt_canvas_mask_set_nodes(canvas, frame, square, 4);
  frame->mask.feather = 0.0f;
  const uint32_t at_shape_edge = _painted_pixel(canvas, 200, 77, 100);
  assert_true(abs((int)(at_frame_edge & 0xFF) - (int)(at_shape_edge & 0xFF)) <= 3);
  dt_canvas_mask_set_shape(canvas, frame, DT_CANVAS_MASK_NONE);
  frame->shadow.offset_x = 20.0f;
  frame->shadow.offset_y = 20.0f;
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

/**
 * Text told to flow around what is laid over it lays its lines in the clear run beside the
 * object, so the same words take more lines and the frame needs to be taller. Without the
 * flag the object is ignored and the text runs straight under it.
 */
/** A text frame with one obstacle laid over it, whose top edge sits `offset` below the text's. */
static double _flowed_height_with_obstacle_below_the_top(const double offset)
{
  dt_canvas_t *canvas = dt_canvas_new();
  if(IS_NULL_PTR(canvas)) return -1.0;
  dt_canvas_object_t *text = dt_canvas_add_text(
      canvas, 200.0, 150.0, 360.0, 260.0,
      "Typography on an infinite plane demands that a paragraph break its lines the same way whatever the "
      "zoom, because the page is the thing being designed and the screen is only a window onto it.");
  if(IS_NULL_PTR(text))
  {
    dt_canvas_free(canvas);
    return -1.0;
  }
  g_strlcpy(text->text.font, "Sans 24", sizeof(text->text.font));
  text->text.text_flags |= DT_CANVAS_TEXT_WRAP_AROUND;
  text->text.wrap_standoff = 0.0f;

  // The left half of the column, from `offset` below the text's top down past its bottom. Later
  // in draw order, so it is laid OVER the text.
  const double top = 20.0 + offset;
  const double bottom = 400.0;
  dt_canvas_object_t *over
      = dt_canvas_add_text(canvas, 120.0, (top + bottom) * 0.5, 200.0, bottom - top, "");
  if(IS_NULL_PTR(over))
  {
    dt_canvas_free(canvas);
    return -1.0;
  }

  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
  cairo_t *cr = cairo_create(surface);
  const double flowed = dt_canvas_paint_text_natural_height(cr, canvas, text);
  cairo_destroy(cr);
  cairo_surface_destroy(surface);
  dt_canvas_free(canvas);
  return flowed;
}


static void _a_font_offers_the_features_it_actually_ships(void **state)
{
  (void)state;
  /*
   * Asked of the FACE through HarfBuzz, never assumed from a table. A font carries whatever
   * tags its designer cut: measured on this machine, FreeSerif answers with 45 of them --
   * historical ligatures and forms, small capitals, four stylistic sets -- DejaVu Serif with
   * 11, Liberation Serif with 6, and the bare default with none. A fixed list would offer the
   * last of those everything and the first of them a fraction of what it has.
   *
   * Which fonts are installed is not this test's business, so it asks several and judges
   * whatever answers: the shape of the list, not its contents.
   */
  dt_canvas_t *canvas = dt_canvas_new();
  assert_non_null(canvas);
  dt_canvas_object_t *text = dt_canvas_add_text(canvas, 0.0, 0.0, 400.0, 200.0, "Typography.");
  assert_non_null(text);

  static const char *const probes[] = { "DejaVu Serif 12", "DejaVu Sans 12", "FreeSerif 12",
                                        "Liberation Serif 12", "Bitstream Vera Sans 12" };
  uint32_t counts[G_N_ELEMENTS(probes)];
  uint32_t answered = 0;
  for(guint probe = 0; probe < G_N_ELEMENTS(probes); probe++)
  {
    g_strlcpy(text->text.font, probes[probe], sizeof(text->text.font));
    char list[64][DT_CANVAS_FONT_FEATURE_TAG_LEN];
    counts[probe] = dt_canvas_paint_text_font_features(canvas, text, list, 64);
    if(counts[probe] > 0) answered++;
    for(uint32_t idx = 0; idx < counts[probe]; idx++)
    {
      // Four characters, sorted, and never the same tag twice -- so the panel reads the same
      // way every time it is opened and no feature is offered to the user in duplicate.
      assert_int_equal((int)strlen(list[idx]), 4);
      if(idx > 0) assert_true(strcmp(list[idx - 1], list[idx]) < 0);
    }
  }
  // Something on this machine has to ship a feature, or nothing is being read from a face.
  assert_true(answered > 0);
  // And the lists are not one list: two faces that both answer must not answer identically in
  // COUNT for every pair, which a fixed table would.
  gboolean differ = FALSE;
  for(guint left = 0; left < G_N_ELEMENTS(probes) && !differ; left++)
    for(guint right = left + 1; right < G_N_ELEMENTS(probes) && !differ; right++)
      differ = counts[left] != counts[right];
  assert_true(differ);

  // A tag the build has a name for is offered by name; one it does not is offered by its tag,
  // which is what makes a font's own stylistic sets reachable at all.
  gchar *historical = dt_canvas_text_feature_label("hlig");
  gchar *small_caps = dt_canvas_text_feature_label("smcp");
  gchar *set = dt_canvas_text_feature_label("ss01");
  gchar *nothing = dt_canvas_text_feature_label("zzzz");
  assert_non_null(historical);
  assert_non_null(small_caps);
  assert_non_null(set);
  assert_null(nothing);
  dt_free(historical);
  dt_free(small_caps);
  dt_free(set);
  dt_canvas_free(canvas);
}

/** A column with one obstacle over it; `centred` puts it in the middle, else against the right. */
static double _flowed_past_obstacle(const gboolean centred)
{
  dt_canvas_t *canvas = dt_canvas_new();
  if(IS_NULL_PTR(canvas)) return -1.0;
  dt_canvas_object_t *text = dt_canvas_add_text(
      canvas, 0.0, 0.0, 600.0, 4000.0,
      "Typography on an infinite plane demands that a paragraph break its lines the same way whatever the "
      "zoom, because the page is the thing being designed and the screen is only a window onto it, and a "
      "column set beside a picture must keep clear of it line by line.");
  if(IS_NULL_PTR(text))
  {
    dt_canvas_free(canvas);
    return -1.0;
  }
  text->text.padding = 0.0f;
  text->text.wrap_standoff = 0.0f;
  text->text.text_flags |= DT_CANVAS_TEXT_WRAP_AROUND | DT_CANVAS_TEXT_OPTICAL_MARGINS;
  // 200 units wide either way, over the whole height the text can reach. Centred it leaves 200
  // clear on each side; against the right edge it leaves 400 on the left and nothing beyond.
  dt_canvas_object_t *over
      = dt_canvas_add_text(canvas, centred ? 0.0 : 200.0, 0.0, centred ? 200.0 : 200.0, 4000.0, "");
  if(IS_NULL_PTR(over))
  {
    dt_canvas_free(canvas);
    return -1.0;
  }
  over->border_width = 0.0f;
  over->flags |= DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE;

  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
  cairo_t *cr = cairo_create(surface);
  const double height = dt_canvas_paint_text_natural_height(cr, canvas, text);
  cairo_destroy(cr);
  cairo_surface_destroy(surface);
  dt_canvas_free(canvas);
  return height;
}

static void _a_line_carries_on_past_a_picture_standing_in_the_column(void **state)
{
  (void)state;
  /*
   * A picture standing in the MIDDLE of a column leaves clear space on both sides of it, and a
   * reader expects the line to carry on past it. Setting each line in the single widest
   * stretch -- "the largest area", which is one of the choices a page-layout application
   * offers -- abandons the far side, which is what "the text is flowing only on one side"
   * reports.
   *
   * Centred, the obstacle leaves 200 units clear either side; against the right edge it leaves
   * 400 on the left and nothing past it. The centred one has HALF the measure per stretch and
   * must still take fewer lines, because it has two of them.
   */
  const double centred = _flowed_past_obstacle(TRUE);
  const double against_the_edge = _flowed_past_obstacle(FALSE);
  assert_true(centred > 0.0 && against_the_edge > 0.0);
  // Measured: 1.000 setting the line across both stretches, 1.833 taking the widest of them.
  assert_true(centred < against_the_edge * 1.3);
}

/** Three paragraphs in a column with a picture standing in the middle of it. */
static double _three_paragraphs_past_a_picture(const float spacing)
{
  dt_canvas_t *canvas = dt_canvas_new();
  if(IS_NULL_PTR(canvas)) return -1.0;
  dt_canvas_object_t *text = dt_canvas_add_text(
      canvas, 0.0, 0.0, 600.0, 4000.0,
      "Typography on an infinite plane demands that a paragraph break its lines the same way "
      "whatever the zoom.\n\nThe page is the thing being designed and the screen is only a window "
      "onto it.\n\nA column set beside a picture must keep clear of it line by line.");
  if(IS_NULL_PTR(text))
  {
    dt_canvas_free(canvas);
    return -1.0;
  }
  text->text.padding = 0.0f;
  text->text.wrap_standoff = 0.0f;
  text->text.paragraph_spacing = spacing;
  text->text.text_flags |= DT_CANVAS_TEXT_WRAP_AROUND | DT_CANVAS_TEXT_OPTICAL_MARGINS;
  /*
   * OFF CENTRE, so the two clear stretches are 250 and 150 units wide rather than equal. Each
   * line is then set from a layout built for a width the last piece did not use, and the
   * cached one is rebuilt on every piece -- which is the state the paragraph break was lost
   * in: the rebuild starts after the break and the empty line carrying it is never reached.
   * Equal stretches let the layout survive from one line to the next and never exercise it; a
   * slanted obstacle exercises it but moves every line to a new width, so no two heights are
   * comparable. This shape does the one without the other.
   */
  dt_canvas_object_t *over = dt_canvas_add_text(canvas, 50.0, 0.0, 200.0, 4000.0, "");
  if(IS_NULL_PTR(over))
  {
    dt_canvas_free(canvas);
    return -1.0;
  }
  over->border_width = 0.0f;
  over->flags |= DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE;

  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
  cairo_t *cr = cairo_create(surface);
  const double height = dt_canvas_paint_text_natural_height(cr, canvas, text);
  cairo_destroy(cr);
  cairo_surface_destroy(surface);
  dt_canvas_free(canvas);
  return height;
}

static void _a_paragraph_break_survives_the_layout_being_rebuilt(void **state)
{
  (void)state;
  /*
   * Which line opens a paragraph is a question about the TEXT, and asking it of the cached
   * Pango layout answered correctly only while that layout survived from line to line. A line
   * set across two stretches rebuilds it almost every line, and the rebuild starts after the
   * break: the empty line Pango draws for the blank one was never reached, the paragraph after
   * it was never asked about, and neither the space nor the indent arrived -- on a real
   * document with both set to 50 units, not once in twenty-three lines.
   *
   * Three paragraphs are two gaps, whatever the picture in the middle does to the lines.
   */
  const double tight = _three_paragraphs_past_a_picture(0.0f);
  const double spaced = _three_paragraphs_past_a_picture(40.0f);
  assert_true(tight > 0.0);
  assert_float_equal(spaced, tight + 80.0, 1.0);
}

static void _paragraphs_take_their_indent_and_their_space(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  assert_non_null(canvas);
  dt_canvas_object_t *text = dt_canvas_add_text(
      canvas, 0.0, 0.0, 400.0, 4000.0,
      "Typography on an infinite plane demands that a paragraph break its lines the same way "
      "whatever the zoom.\n\nThe page is the thing being designed and the screen is only a window "
      "onto it.\n\nA column set beside a picture must keep clear of it line by line.");
  assert_non_null(text);
  text->text.padding = 0.0f;

  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
  cairo_t *cr = cairo_create(surface);
  const double plain = dt_canvas_paint_text_natural_height(cr, canvas, text);

  // Three paragraphs, so two gaps between them. The first one gets none: the space is BETWEEN
  // paragraphs, not above every one of them.
  text->text.paragraph_spacing = 40.0f;
  const double spaced = dt_canvas_paint_text_natural_height(cr, canvas, text);

  // An indent shortens the first line of each paragraph. Half the measure, over three
  // paragraphs, has to cost lines.
  text->text.paragraph_spacing = 0.0f;
  text->text.first_line_indent = 200.0f;
  const double indented = dt_canvas_paint_text_natural_height(cr, canvas, text);
  cairo_destroy(cr);
  cairo_surface_destroy(surface);

  assert_true(plain > 0.0);
  assert_float_equal(spaced, plain + 80.0, 1.0);
  assert_true(indented > plain);
  dt_canvas_free(canvas);
}

/** A column beside one obstacle; returns the height its text needs at that leading. */
static double _paragraph_height(const gboolean flowing, const float leading, const gboolean one_line)
{
  dt_canvas_t *canvas = dt_canvas_new();
  if(IS_NULL_PTR(canvas)) return -1.0;
  dt_canvas_object_t *text = dt_canvas_add_text(
      canvas, 0.0, 0.0, 500.0, 4000.0,
      one_line ? "Typography."
               : "Typography on an infinite plane demands that a paragraph break its lines the same way "
                 "whatever the zoom, because the page is the thing being designed and the screen is only a "
                 "window onto it, and a column set beside a picture must keep clear of it line by line.");
  if(IS_NULL_PTR(text))
  {
    dt_canvas_free(canvas);
    return -1.0;
  }
  text->text.padding = 0.0f;
  text->text.wrap_standoff = 0.0f;
  text->text.line_height = leading;
  if(flowing) text->text.text_flags |= DT_CANVAS_TEXT_WRAP_AROUND | DT_CANVAS_TEXT_OPTICAL_MARGINS;
  /*
   * A rectangle beside the first lines, spanning further than the paragraph reaches at either
   * leading. Its edges are VERTICAL on purpose: every line then loses the same width wherever
   * the leading puts it, so the flowing line count cannot change between the two leadings. A
   * slanted obstacle would not do -- opening the leading moves every line to a new height,
   * where it legitimately meets a different width of the shape.
   */
  dt_canvas_object_t *over = dt_canvas_add_text(canvas, -180.0, -1600.0, 300.0, 1000.0, "");
  if(IS_NULL_PTR(over))
  {
    dt_canvas_free(canvas);
    return -1.0;
  }
  over->border_width = 0.0f;
  over->flags |= DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE;

  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
  cairo_t *cr = cairo_create(surface);
  const double height = dt_canvas_paint_text_natural_height(cr, canvas, text);
  cairo_destroy(cr);
  cairo_surface_destroy(surface);
  dt_canvas_free(canvas);
  return height;
}

static void _the_leading_reaches_a_flowing_paragraph_once_per_gap(void **state)
{
  (void)state;
  /*
   * Pango's spacing is the space BETWEEN two lines of one layout, and every line of a flowing
   * paragraph is line zero of a layout of its own -- so no line's extents ever carry it, and a
   * line height did exactly nothing to a frame that wrapped around something or hung its
   * punctuation, silently, while the plain paragraph beside it honoured it.
   *
   * It is applied once per GAP and not once per line: reading "is there more text" before the
   * line's own text is accounted for leaves a trailing gap Pango would not have left.
   *
   * Line counts differ between the two paragraphs -- only the flowing one is narrowed by the
   * obstacle -- so the plain one is here to say what a gap costs, and the flowing one is
   * checked against its own line count. What the ink band buys on a SLANTED edge (a band `h`
   * tall narrows the run by `h * tan(theta)`, the gutter reading wider along a slant than
   * along a straight) is measured in doc/canvas.md rather than pinned here: moving a line
   * changes which width of a slanted shape it meets, so no two leadings are comparable there.
   */
  const double one_line = _paragraph_height(FALSE, 1.0f, TRUE);
  const double plain_tight = _paragraph_height(FALSE, 1.0f, FALSE);
  const double plain_open = _paragraph_height(FALSE, 3.0f, FALSE);
  const double flowing_tight = _paragraph_height(TRUE, 1.0f, FALSE);
  const double flowing_open = _paragraph_height(TRUE, 3.0f, FALSE);
  assert_true(one_line > 0.0 && plain_tight > 0.0 && flowing_tight > 0.0);

  const double plain_lines = round(plain_tight / one_line);
  const double flowing_lines = round(flowing_tight / one_line);
  assert_true(plain_lines >= 2.0 && flowing_lines > plain_lines);
  // What one gap costs, from the paragraph that is laid out plainly.
  const double gap = (plain_open - plain_tight) / (plain_lines - 1.0);
  assert_true(gap > 0.0);
  assert_float_equal(flowing_open - flowing_tight, gap * (flowing_lines - 1.0), 1.0);
}

static void _a_frame_standing_just_outside_a_column_still_pushes_its_text(void **state)
{
  (void)state;
  /*
   * Coverage is only ever sampled at a cell of the occupancy grid, so a grid stopping at the
   * text area cannot know about a frame standing just beyond it: no cell is covered, the
   * dilation of nothing is nothing, and the frame pushed the text not at all however wide a
   * gap was asked for. The grid is grown by the furthest anything can reach into it.
   */
  double heights[2] = { 0.0, 0.0 };
  const float gaps[2] = { 100.0f, 300.0f };
  for(int pass = 0; pass < 2; pass++)
  {
    dt_canvas_t *canvas = dt_canvas_new();
    assert_non_null(canvas);
    dt_canvas_object_t *text = dt_canvas_add_text(
        canvas, 0.0, 0.0, 400.0, 900.0,
        "Typography on an infinite plane demands that a paragraph break its lines the same way whatever "
        "the zoom, because the page is the thing being designed and the screen is only a window onto it.");
    assert_non_null(text);
    text->text.padding = 0.0f;
    text->text.text_flags |= DT_CANVAS_TEXT_WRAP_AROUND | DT_CANVAS_TEXT_OPTICAL_MARGINS;
    text->text.wrap_standoff = gaps[pass];
    // Spanning the column's whole height, its right edge 50 units clear of the text area's
    // left edge at -200. It touches no cell of the column; only the gap reaches in.
    dt_canvas_object_t *beside = dt_canvas_add_text(canvas, -325.0, 0.0, 150.0, 900.0, "");
    assert_non_null(beside);
    beside->border_width = 0.0f;
  beside->flags |= DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE;

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
    cairo_t *cr = cairo_create(surface);
    heights[pass] = dt_canvas_paint_text_natural_height(cr, canvas, text);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    dt_canvas_free(canvas);
  }
  // 100 units of gap clears the 50 that separate them and takes 50 off the measure; 300 takes
  // 250, so the same text needs more lines.
  assert_true(heights[0] > 0.0);
  assert_true(heights[1] > heights[0]);
}

static void _the_gap_around_an_obstacle_is_a_disc_not_a_square(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  assert_non_null(canvas);
  dt_canvas_object_t *text = dt_canvas_add_text(
      canvas, 0.0, 0.0, 400.0, 600.0,
      "Typography on an infinite plane demands that a paragraph break its lines the same way whatever the "
      "zoom, because the page is the thing being designed and the screen is only a window onto it.");
  assert_non_null(text);
  text->text.padding = 0.0f;
  // Optical margins keep BOTH measurements on the line-by-line engine: without an obstacle the
  // wrap flag alone falls back to the plain paragraph layout, and the two engines do not agree
  // on a height to the last hundredth.
  text->text.text_flags |= DT_CANVAS_TEXT_WRAP_AROUND | DT_CANVAS_TEXT_OPTICAL_MARGINS;
  text->text.wrap_standoff = 200.0f;

  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
  cairo_t *cr = cairo_create(surface);
  const double alone = dt_canvas_paint_text_natural_height(cr, canvas, text);

  /*
   * A small frame off the text area's top-left CORNER, 150 units clear on each axis. Its
   * diagonal distance to the area is 212, beyond the 200 the gap allows, so a disc of that
   * radius does not touch the column and the text must lay out exactly as it did alone. A
   * separable max filter is a SQUARE instead: it reaches 200 along each axis independently,
   * its corner covers everything within 283, and it would take a bite out of the first lines.
   * That is the same square that made the clear space widest where a shape's edge slants and
   * tightest where it runs straight -- a gutter that would not hold still along a cut.
   */
  dt_canvas_object_t *corner = dt_canvas_add_text(canvas, -370.0, -470.0, 40.0, 40.0, "");
  assert_non_null(corner);
  corner->border_width = 0.0f;
  corner->flags |= DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE;
  const double with_corner = dt_canvas_paint_text_natural_height(cr, canvas, text);
  cairo_destroy(cr);
  cairo_surface_destroy(surface);

  assert_true(alone > 0.0);
  assert_float_equal(alone, with_corner, 0.01);
  dt_canvas_free(canvas);
}

/** A column with one circular cut frame over it, of the given cutout radius and fall-off. */
static double _flowed_past_a_cut_frame(const float radius, const float feather)
{
  dt_canvas_t *canvas = dt_canvas_new();
  if(IS_NULL_PTR(canvas)) return -1.0;
  dt_canvas_object_t *text = dt_canvas_add_text(
      canvas, 0.0, 0.0, 600.0, 4000.0,
      "Typography on an infinite plane demands that a paragraph break its lines the same way whatever the "
      "zoom, because the page is the thing being designed and the screen is only a window onto it, and a "
      "column set beside a picture must keep clear of it line by line.");
  if(IS_NULL_PTR(text))
  {
    dt_canvas_free(canvas);
    return -1.0;
  }
  text->text.padding = 0.0f;
  text->text.wrap_standoff = 0.0f;
  text->text.text_flags |= DT_CANVAS_TEXT_WRAP_AROUND | DT_CANVAS_TEXT_OPTICAL_MARGINS;
  // Over the first lines: the cutout is a circle in the middle of its frame, so the frame has
  // to straddle the text rather than merely overlap it.
  dt_canvas_object_t *over = dt_canvas_add_text(canvas, -300.0, -1940.0, 600.0, 600.0, "");
  if(IS_NULL_PTR(over))
  {
    dt_canvas_free(canvas);
    return -1.0;
  }
  over->border_width = 0.0f;
  over->flags |= DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE;
  dt_canvas_mask_set_shape(canvas, over, DT_CANVAS_MASK_CIRCLE);
  over->mask.radius_x = radius;
  over->mask.feather = feather;

  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
  cairo_t *cr = cairo_create(surface);
  const double height = dt_canvas_paint_text_natural_height(cr, canvas, text);
  cairo_destroy(cr);
  cairo_surface_destroy(surface);
  dt_canvas_free(canvas);
  return height;
}


static void _a_point_of_type_is_a_unit_on_the_plane(void **state)
{
  (void)state;
  /*
   * A canvas unit is a POINT, and a font's size is in points already -- but Pango turns those
   * into its context's units at the context's density, which is 96 unless it is told
   * otherwise. Untold, "12" arrived on the plane as sixteen units: a type size meant nothing
   * anyone could measure against a page, and it did not change when the export density did
   * while the page did.
   *
   * Measured with the context pinned at 72: a line of N-point type is 1.1667 N units at every
   * size, which is the font's own line height and nothing else. At 96 it would be 1.5556 N.
   */
  static const int sizes[] = { 12, 24, 72 };
  static const double densities[] = { 72.0, 300.0 };
  double first = 0.0;
  for(guint density = 0; density < G_N_ELEMENTS(densities); density++)
    for(guint size = 0; size < G_N_ELEMENTS(sizes); size++)
    {
      dt_canvas_t *canvas = dt_canvas_new();
      canvas->resolution = (float)densities[density];
      dt_canvas_object_t *text = dt_canvas_add_text(canvas, 0.0, 0.0, 4000.0, 400.0, "Typography");
      assert_non_null(text);
      text->text.padding = 0.0f;
      gchar *description = g_strdup_printf("Sans %d", sizes[size]);
      g_strlcpy(text->text.font, description, sizeof(text->text.font));
      g_free(description);

      cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
      cairo_t *cr = cairo_create(surface);
      const double line = dt_canvas_paint_text_natural_height(cr, canvas, text);
      cairo_destroy(cr);
      cairo_surface_destroy(surface);
      dt_canvas_free(canvas);

      const double per_point = line / sizes[size];
      // The font's leading and no scale factor hiding behind it -- 96/72 would put this at 1.56.
      assert_true(per_point > 1.0 && per_point < 1.3);
      // The same at every size, and the export density does not enter into it at all.
      if(first == 0.0) first = per_point;
      assert_float_equal(per_point, first, 1e-6);
    }
}

/** Write a drawing to a temporary file and give back its path; the caller frees it. */
static gchar *_write_svg(const char *body)
{
  gchar *path = NULL;
  const int handle = g_file_open_tmp("canvas-XXXXXX.svg", &path, NULL);
  assert_true(handle >= 0);
  close(handle);
  assert_true(g_file_set_contents(path, body, -1, NULL));
  return path;
}

static void _a_drawing_arrives_at_the_size_its_file_states(void **state)
{
  (void)state;
  /*
   * A canvas unit is a point and so is an SVG's own user unit once the handle is told there
   * are 72 to the inch, so a drawing that says it is 144 by 72 points arrives as a frame of
   * exactly that -- two inches by one, on the page, with no scale factor anywhere between the
   * file and the paper.
   */
  gchar *path = _write_svg("<svg xmlns='http://www.w3.org/2000/svg' width='2in' height='1in' "
                           "viewBox='0 0 144 72'><rect width='144' height='72' fill='#ff0000'/></svg>");
  dt_canvas_t *canvas = dt_canvas_new();
  GError *error = NULL;
  dt_canvas_object_t *drawing = dt_canvas_add_svg(canvas, 10.0, 20.0, path, &error);
  assert_null(error);
  assert_non_null(drawing);
  assert_int_equal(drawing->kind, DT_CANVAS_OBJECT_SVG);
  assert_float_equal(drawing->width, 144.0, 1e-6);
  assert_float_equal(drawing->height, 72.0, 1e-6);
  assert_non_null(drawing->svg.svg);
  // Where it came from, so it can be read again.
  char remembered[DT_PATH_MAX];
  dt_canvas_svg_path(drawing, remembered, sizeof(remembered));
  assert_string_equal(remembered, path);

  // A file stating only a viewBox has no physical size to honour and takes the viewBox, which
  // is what every browser does.
  gchar *boxed = _write_svg("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 300 150'>"
                            "<rect width='300' height='150' fill='#00ff00'/></svg>");
  dt_canvas_object_t *second = dt_canvas_add_svg(canvas, 0.0, 0.0, boxed, &error);
  assert_null(error);
  assert_non_null(second);
  assert_float_equal(second->width, 300.0, 1e-6);
  assert_float_equal(second->height, 150.0, 1e-6);

  g_remove(path);
  g_remove(boxed);
  g_free(path);
  g_free(boxed);
  dt_canvas_free(canvas);
}

static void _a_drawing_is_rasterised_whole_and_then_brought_into_the_layer(void **state)
{
  (void)state;
  /*
   * The specification composites an SVG in sRGB with the transfer function applied; this
   * canvas composites in linear Adobe RGB. So the drawing is rendered WHOLE, exactly as its
   * author saw it, and only the finished image is converted -- and the conversion has to
   * divide cairo's premultiplied alpha back out first, or every anti-aliased edge in the
   * drawing comes out at the wrong lightness.
   */
  gchar *path = _write_svg("<svg xmlns='http://www.w3.org/2000/svg' width='64' height='64' "
                           "viewBox='0 0 64 64'>"
                           "<rect x='0' y='0' width='64' height='32' fill='#ff0000'/>"
                           "<rect x='0' y='32' width='64' height='32' fill='#ff0000' opacity='0.5'/>"
                           "</svg>");
  GBytes *bytes = NULL;
  gchar *contents = NULL;
  gsize length = 0;
  assert_true(g_file_get_contents(path, &contents, &length, NULL));
  bytes = g_bytes_new_take(contents, length);
  cairo_surface_t *surface = dt_canvas_render_decode(bytes, DT_CANVAS_COLORSPACE_SRGB);
  assert_non_null(surface);
  assert_int_equal(cairo_image_surface_get_format(surface), CAIRO_FORMAT_ARGB32);
  const int width = cairo_image_surface_get_width(surface);
  const int height = cairo_image_surface_get_height(surface);
  assert_true(width >= 64 && height >= 64);
  const uint8_t *pixels = cairo_image_surface_get_data(surface);
  const int stride = cairo_image_surface_get_stride(surface);

  // Opaque sRGB red, in the layer's own encoding. Cairo's ARGB32 is B, G, R, A in memory.
  const uint8_t *opaque = pixels + (size_t)(height / 4) * stride + (size_t)(width / 2) * 4;
  assert_int_equal(opaque[3], 255);
  /*
   * sRGB red is (1, 0, 0) in linear light, which is (0.715166, 0, 0) in Adobe RGB, which the
   * layer's own encoding writes as 219. Left in sRGB it would still read 255, so this number
   * is the whole of the conversion in one byte.
   */
  assert_int_equal(opaque[2], 219);
  assert_int_equal(opaque[1], 0);
  assert_int_equal(opaque[0], 0);

  /*
   * Half-transparent red: the same colour at half the alpha, still PREMULTIPLIED, 219 * 128 /
   * 255. The alpha is divided back out before the transfer function and folded in after
   * because that is what the two spaces mean -- but measured, converting the premultiplied
   * bytes directly gives 109 against this 110, and that is not luck: sRGB's transfer function
   * and Adobe RGB's are both near a gamma of 2.2, and for a pure gamma the alpha factors
   * straight out of `encode(k * eotf(a * c)) = a * encode(k * eotf(c))`. So the careful path
   * costs nothing and buys a code here; it is kept because the day either curve is not that
   * gamma -- a linear layer, a PQ one -- it is the only version that stays right, and because
   * a reader should not have to rediscover that the two agree by accident.
   */
  const uint8_t *half = pixels + (size_t)(height * 3 / 4) * stride + (size_t)(width / 2) * 4;
  assert_int_equal(half[3], 128);
  assert_int_equal(half[2], (int)lround(219.0 * 128.0 / 255.0));

  cairo_surface_destroy(surface);
  g_bytes_unref(bytes);
  g_remove(path);
  g_free(path);
}

static void _text_flows_around_what_a_drawing_draws_not_its_box(void **state)
{
  (void)state;
  /*
   * A drawing is not a rectangle. A file whose ink fills only the left half of its viewBox
   * must push the text out of that half and no further -- treated as its box, it would take
   * the whole column and the text would keep clear of empty paper.
   */
  gchar *half_full = _write_svg("<svg xmlns='http://www.w3.org/2000/svg' width='400' height='400' "
                                "viewBox='0 0 400 400'>"
                                "<rect x='0' y='0' width='200' height='400' fill='#000000'/></svg>");
  gchar *filled = _write_svg("<svg xmlns='http://www.w3.org/2000/svg' width='400' height='400' "
                             "viewBox='0 0 400 400'>"
                             "<rect x='0' y='0' width='400' height='400' fill='#000000'/></svg>");
  double heights[2] = { 0.0, 0.0 };
  const gchar *files[2] = { half_full, filled };
  for(int which = 0; which < 2; which++)
  {
    dt_canvas_t *canvas = dt_canvas_new();
    dt_canvas_object_t *text = dt_canvas_add_text(
        canvas, 0.0, 0.0, 600.0, 4000.0,
        "Typography on an infinite plane demands that a paragraph break its lines the same way whatever the "
        "zoom, because the page is the thing being designed and the screen is only a window onto it.");
    text->text.padding = 0.0f;
    text->text.wrap_standoff = 0.0f;
    text->text.text_flags |= DT_CANVAS_TEXT_WRAP_AROUND | DT_CANVAS_TEXT_OPTICAL_MARGINS;
    GError *error = NULL;
    // Over the first lines, its box spanning the column's left 400 of 600 points.
    dt_canvas_object_t *drawing = dt_canvas_add_svg(canvas, -100.0, -1800.0, files[which], &error);
    assert_null(error);
    assert_non_null(drawing);
    drawing->border_width = 0.0f;
    drawing->flags |= DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE;

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
    cairo_t *cr = cairo_create(surface);
    heights[which] = dt_canvas_paint_text_natural_height(cr, canvas, text);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    dt_canvas_free(canvas);
  }
  assert_true(heights[0] > 0.0 && heights[1] > 0.0);
  // Measured: 41.91 against 97.78. Read as a rectangle the two are identical, because the
  // rectangle is the same rectangle.
  assert_true(heights[0] < heights[1] * 0.75);
  // Ink in half the box costs the column less than ink in all of it. Read as a rectangle, the
  // two would be identical.
  assert_true(heights[0] < heights[1]);

  g_remove(half_full);
  g_remove(filled);
  g_free(half_full);
  g_free(filled);
}

static void _a_drawing_travels_in_the_document(void **state)
{
  (void)state;
  /*
   * The file's own bytes are an archive entry, so a document carries the drawing rather than a
   * reference to one and opens on a machine that has never seen the file. The path travels
   * beside them so it can be read again when the drawing changes.
   */
  gchar *path = _write_svg("<svg xmlns='http://www.w3.org/2000/svg' width='120' height='60' "
                           "viewBox='0 0 120 60'><circle cx='60' cy='30' r='30' fill='#3366cc'/></svg>");
  dt_canvas_t *canvas = dt_canvas_new();
  GError *error = NULL;
  dt_canvas_object_t *drawing = dt_canvas_add_svg(canvas, 5.0, 6.0, path, &error);
  assert_non_null(drawing);
  const uint32_t id = drawing->id;

  gchar *document = g_build_filename(g_get_tmp_dir(), "canvas-svg-round-trip.anselcanvas", NULL);
  assert_true(dt_canvas_save(canvas, document, &error));
  assert_null(error);
  dt_canvas_free(canvas);

  dt_canvas_t *restored = dt_canvas_load(document, &error);
  assert_non_null(restored);
  assert_null(error);
  dt_canvas_object_t *back = dt_canvas_find_object(restored, id);
  assert_non_null(back);
  assert_int_equal(back->kind, DT_CANVAS_OBJECT_SVG);
  assert_non_null(back->svg.svg);
  assert_float_equal(back->width, 120.0, 1e-6);
  assert_float_equal(back->svg.source_height, 60.0f, 1e-6);
  char remembered[DT_PATH_MAX];
  dt_canvas_svg_path(back, remembered, sizeof(remembered));
  assert_string_equal(remembered, path);
  // And it draws: the bytes that came back are the drawing, not a husk.
  cairo_surface_t *raster = dt_canvas_render_decode(back->svg.svg, DT_CANVAS_COLORSPACE_SRGB);
  assert_non_null(raster);
  cairo_surface_destroy(raster);

  // Reading the file again reports whether anything changed, and says nothing changed here.
  assert_false(dt_canvas_svg_reload(restored, back, &error));
  assert_null(error);
  // Changed on disk, it comes back changed and the frame stays where the user put it.
  const double kept_width = back->width;
  assert_true(g_file_set_contents(path, "<svg xmlns='http://www.w3.org/2000/svg' width='120' height='60' "
                                        "viewBox='0 0 120 60'><rect width='120' height='60'/></svg>",
                                  -1, NULL));
  assert_true(dt_canvas_svg_reload(restored, back, &error));
  assert_null(error);
  assert_float_equal(back->width, kept_width, 1e-6);

  g_remove(document);
  g_remove(path);
  g_free(document);
  g_free(path);
  dt_canvas_free(restored);
}

static void _a_drawing_keeps_the_paper_where_it_draws_nothing(void **state)
{
  (void)state;
  /*
   * An SVG is mostly holes: a logo, a diagram, an arrow are ink on nothing, and the nothing
   * has to stay nothing. A photograph has no alpha at all, so anything that assumes a frame's
   * picture covers its frame turns every drawing into a rectangle of whatever colour sits
   * under it.
   */
  gchar *path = _write_svg("<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100' "
                           "viewBox='0 0 100 100'><circle cx='50' cy='50' r='20' fill='#ffffff'/></svg>");
  dt_canvas_t *canvas = dt_canvas_new();
  canvas->background = dt_canvas_color(1.0f, 0.0f, 0.0f, 1.0f); // a red plane, to see through to
  canvas->background_style = DT_CANVAS_BACKGROUND_PLAIN;
  canvas->grid_flags = 0;
  GError *error = NULL;
  dt_canvas_object_t *drawing = dt_canvas_add_svg(canvas, 0.0, 0.0, path, &error);
  assert_non_null(drawing);
  drawing->border_width = 0.0f;
  drawing->flags |= DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE | DT_CANVAS_OBJECT_FLAG_SHADOW_OVERRIDE;
  drawing->shadow.blur = 0.0f;

  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 200, 200);
  cairo_t *cr = cairo_create(surface);
  cairo_translate(cr, 100.0, 100.0);
  const dt_canvas_rect_t whole = { -100.0, -100.0, 200.0, 200.0 };
  dt_canvas_paint_options_t options = dt_canvas_paint_options_export(NULL, 1.0, whole);
  dt_canvas_paint(cr, canvas, &options);
  cairo_destroy(cr);
  cairo_surface_flush(surface);

  const uint8_t *pixels = cairo_image_surface_get_data(surface);
  const int stride = cairo_image_surface_get_stride(surface);
  // The middle is the circle's white; a corner of the drawing's own frame is where it drew
  // nothing, and the plane must still be red there.
  const uint8_t *middle = pixels + (size_t)100 * stride + (size_t)100 * 4;
  const uint8_t *corner = pixels + (size_t)62 * stride + (size_t)62 * 4;
  print_message("middle B %3u G %3u R %3u | inside the frame, outside the ink: B %3u G %3u R %3u\n",
                middle[0], middle[1], middle[2], corner[0], corner[1], corner[2]);

  cairo_surface_destroy(surface);
  g_remove(path);
  g_free(path);
  dt_canvas_free(canvas);
}


static void _rescaling_a_sprite_keeps_the_alpha_it_had(void **state)
{
  (void)state;
  /*
   * The sprite cache rescales a picture once per size and blits it 1:1 after. It used to build
   * every sprite as RGB24 and force the top byte opaque, which is invisible for a photograph
   * -- a JPEG has no alpha to lose -- and turns a drawing into a rectangle of whatever the
   * top byte then means. A premultiplied mean is the mean of the covered colour, so the alpha
   * rescales alongside the colour with no un-premultiplying anywhere.
   */
  cairo_surface_t *source = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 64, 64);
  cairo_t *cr = cairo_create(source);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_paint(cr);
  // An opaque white square in the middle of nothing at all.
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
  cairo_rectangle(cr, 16.0, 16.0, 32.0, 32.0);
  cairo_fill(cr);
  cairo_destroy(cr);

  for(int size = 0; size < 2; size++)
  {
    const int edge = size == 0 ? 32 : 128; // shrinking and growing take different paths
    cairo_surface_t *sprite = dt_canvas_render_rescale(source, edge, edge);
    assert_non_null(sprite);
    assert_int_equal(cairo_image_surface_get_format(sprite), CAIRO_FORMAT_ARGB32);
    const uint8_t *pixels = cairo_image_surface_get_data(sprite);
    const int stride = cairo_image_surface_get_stride(sprite);
    const uint8_t *middle = pixels + (size_t)(edge / 2) * stride + (size_t)(edge / 2) * 4;
    const uint8_t *outside = pixels + (size_t)(edge / 16) * stride + (size_t)(edge / 16) * 4;
    assert_int_equal(middle[3], 255);
    assert_int_equal(outside[3], 0); // where the drawing drew nothing, it still draws nothing
    cairo_surface_destroy(sprite);
  }

  // And a picture with no alpha to keep is still built without one, so nothing else moves.
  cairo_surface_t *opaque = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 64, 64);
  cairo_surface_t *sprite = dt_canvas_render_rescale(opaque, 32, 32);
  assert_non_null(sprite);
  assert_int_equal(cairo_image_surface_get_format(sprite), CAIRO_FORMAT_RGB24);
  cairo_surface_destroy(sprite);
  cairo_surface_destroy(opaque);
  cairo_surface_destroy(source);
}

/** The sharpest single-pixel step anywhere in a surface: 255 for a hard edge, less for a soft one. */
static int _sharpest_step(cairo_surface_t *surface)
{
  cairo_surface_flush(surface);
  const uint8_t *pixels = cairo_image_surface_get_data(surface);
  const int stride = cairo_image_surface_get_stride(surface);
  const int width = cairo_image_surface_get_width(surface);
  const int height = cairo_image_surface_get_height(surface);
  int sharpest = 0;
  for(int row = 0; row < height; row++)
    for(int col = 1; col < width; col++)
    {
      const uint8_t *here = pixels + (size_t)row * stride + (size_t)col * 4;
      const uint8_t *before = here - 4;
      const int step = abs((int)here[3] - (int)before[3]);
      if(step > sharpest) sharpest = step;
    }
  return sharpest;
}

static void _a_drawing_is_drawn_at_the_size_it_is_shown_at(void **state)
{
  (void)state;
  /*
   * An SVG has no resolution of its own -- that is the whole point of one -- so the size to
   * draw it at is whatever it is about to be shown at. Rasterising it once and rescaling that
   * throws away the only thing it had over a photograph: a small drawing enlarged on the page,
   * or any drawing at a zoom past whatever factor the raster was made with, comes back soft.
   */
  gchar *path = _write_svg("<svg xmlns='http://www.w3.org/2000/svg' width='32' height='32' "
                           "viewBox='0 0 32 32'><rect x='8' y='8' width='16' height='16' fill='#000000'/></svg>");
  gchar *contents = NULL;
  gsize length = 0;
  assert_true(g_file_get_contents(path, &contents, &length, NULL));
  GBytes *bytes = g_bytes_new_take(contents, length);

  // Drawn at the size asked for: the square's edge is a hard one, whatever that size is.
  cairo_surface_t *large = dt_canvas_render_svg(bytes, 512, 512, 0, 0, 0.0, 0.0);
  assert_non_null(large);
  assert_int_equal(cairo_image_surface_get_width(large), 512);
  assert_true(_sharpest_step(large) > 200);

  // Against the alternative: the file's own 32 points, blown up to the same 512 by resampling.
  cairo_surface_t *small = dt_canvas_render_svg(bytes, 0, 0, 0, 0, 0.0, 0.0);
  assert_non_null(small);
  assert_int_equal(cairo_image_surface_get_width(small), 32);
  cairo_surface_t *stretched = dt_canvas_render_rescale(small, 512, 512);
  assert_non_null(stretched);
  // Sixteen pixels of ramp where there should be none: this is the blur, in one number.
  assert_true(_sharpest_step(stretched) < 60);

  cairo_surface_destroy(large);
  cairo_surface_destroy(small);
  cairo_surface_destroy(stretched);
  g_bytes_unref(bytes);
  g_remove(path);
  g_free(path);
}

static void _a_picture_and_a_drawing_keep_their_shape_unless_told_not_to(void **state)
{
  (void)state;
  /*
   * Stated the free way round so that ZERO is the careful answer: a frame with proportions to
   * keep keeps them until it is told otherwise. A photograph always did; a drawing needs it
   * more, since a stretched logo is almost always a mistake -- and the one time it is not,
   * the flag says so.
   */
  gchar *path = _write_svg("<svg xmlns='http://www.w3.org/2000/svg' width='120' height='60' "
                           "viewBox='0 0 120 60'><rect width='120' height='60'/></svg>");
  dt_canvas_t *canvas = dt_canvas_new();
  GError *error = NULL;
  dt_canvas_object_t *drawing = dt_canvas_add_svg(canvas, 0.0, 0.0, path, &error);
  assert_non_null(drawing);
  dt_canvas_object_t *text = dt_canvas_add_text(canvas, 0.0, 0.0, 100.0, 100.0, "");
  assert_non_null(text);

  // A drawing keeps its shape out of the box, and says so through one predicate the drag and
  // the properties' two size spin buttons all read.
  assert_true(dt_canvas_object_keeps_ratio(drawing));
  drawing->flags |= DT_CANVAS_OBJECT_FLAG_FREE_RATIO;
  assert_false(dt_canvas_object_keeps_ratio(drawing));
  drawing->flags &= ~DT_CANVAS_OBJECT_FLAG_FREE_RATIO;
  assert_true(dt_canvas_object_keeps_ratio(drawing));
  // A text frame has no proportions to keep, flag or no flag.
  assert_false(dt_canvas_object_keeps_ratio(text));
  text->flags |= DT_CANVAS_OBJECT_FLAG_FREE_RATIO;
  assert_false(dt_canvas_object_keeps_ratio(text));
  assert_false(dt_canvas_object_keeps_ratio(NULL));

  // And it survives the file, since it rides in the object's own flags.
  const uint32_t id = drawing->id;
  drawing->flags |= DT_CANVAS_OBJECT_FLAG_FREE_RATIO;
  GBytes *index = dt_canvas_format_write_index(canvas);
  assert_non_null(index);
  dt_canvas_t *restored = dt_canvas_new();
  assert_true(dt_canvas_format_read_index(restored, index, NULL));
  g_bytes_unref(index);
  assert_false(dt_canvas_object_keeps_ratio(dt_canvas_find_object(restored, id)));

  dt_canvas_free(restored);
  dt_canvas_free(canvas);
  g_remove(path);
  g_free(path);
}


static void _a_drawings_sprite_is_exactly_the_size_it_was_asked_for(void **state)
{
  (void)state;
  /*
   * The painter blits a sprite ONE PIXEL TO ONE at a corner it worked out itself, so a surface
   * of any other size lands small in the corner of where it belongs -- reported as a drawing
   * that vanishes or jumps as the zoom passes some threshold, which is where an internal
   * ceiling on the raster used to change the size underneath the caller. A ceiling is still
   * needed, so that a drawing across a wall-sized page cannot ask for a raster nobody has the
   * memory for; it is applied to what is RENDERED and the result is brought back to the size
   * that was asked for.
   */
  gchar *path = _write_svg("<svg xmlns='http://www.w3.org/2000/svg' width='300' height='200' "
                           "viewBox='0 0 300 200'><rect width='300' height='200' fill='#123456'/></svg>");
  gchar *contents = NULL;
  gsize length = 0;
  assert_true(g_file_get_contents(path, &contents, &length, NULL));
  GBytes *bytes = g_bytes_new_take(contents, length);
  static const int wanted[][2] = { { 300, 200 }, { 301, 201 }, { 4000, 2667 }, { 6000, 4000 }, { 9000, 6000 } };
  for(guint idx = 0; idx < G_N_ELEMENTS(wanted); idx++)
  {
    cairo_surface_t *sprite = dt_canvas_render_svg(bytes, wanted[idx][0], wanted[idx][1], 0, 0, 0.0, 0.0);
    assert_non_null(sprite);
    assert_int_equal(cairo_image_surface_get_width(sprite), wanted[idx][0]);
    assert_int_equal(cairo_image_surface_get_height(sprite), wanted[idx][1]);
    cairo_surface_destroy(sprite);
  }
  g_bytes_unref(bytes);
  g_remove(path);
  g_free(path);
}

static void _a_drawing_fills_its_box_and_sits_in_the_callers_air(void **state)
{
  (void)state;
  /*
   * A drawing is FITTED to its frame, so a frame whose proportions differ from the document's
   * is filled along one axis and letterboxed along the other -- and a frame proportionally
   * taller than its drawing has the ink running edge to edge DOWN it. An author who drew to
   * the edge of the page, which is most of them, then has the last line of type sitting
   * exactly on the frame's boundary, anti-aliased against whatever is behind it and reading as
   * shaved off: that is what was reported as text clipped on a drawing, with two pixels of
   * headroom asked for by name.
   *
   * The air that answers it is the CALLER's, added around the sprite and paid back at the
   * blit, and this is the renderer's half of that bargain: a drawing is drawn at exactly the
   * box it was given and centred in whatever is left over. Subtracting the air from the
   * drawing instead -- what this did until the zoom glitch was measured -- charges a fixed
   * number of screen pixels to a box whose size is the zoom's, so a drawing shrinks inside its
   * own frame as the page is zoomed out.
   */
  gchar *path = _write_svg("<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100' "
                           "viewBox='0 0 100 100'><rect width='100' height='100' fill='#000000'/></svg>");
  gchar *contents = NULL;
  gsize length = 0;
  assert_true(g_file_get_contents(path, &contents, &length, NULL));
  GBytes *bytes = g_bytes_new_take(contents, length);

  // Whatever air the caller leaves, the ink is a hundred pixels of it, starting where the air
  // ends: the drawing's size is the BOX's and owes the padding nothing.
  static const int pads[] = { 0, 1, 2, 4 };
  for(guint idx = 0; idx < G_N_ELEMENTS(pads); idx++)
  {
    const int pad = pads[idx];
    const int edge = 100 + 2 * pad;
    cairo_surface_t *sprite = dt_canvas_render_svg(bytes, edge, edge, 100, 100, 0.0, 0.0);
    assert_non_null(sprite);
    assert_int_equal(cairo_image_surface_get_width(sprite), edge);
    const uint8_t *pixels = cairo_image_surface_get_data(sprite);
    const int stride = cairo_image_surface_get_stride(sprite);
    int first = -1;
    int last = -1;
    for(int row = 0; row < edge; row++)
    {
      if(pixels[(size_t)row * stride + (edge / 2) * 4 + 3] == 0) continue;
      if(first < 0) first = row;
      last = row;
    }
    assert_int_equal(first, pad);
    assert_int_equal(last, edge - 1 - pad);
    cairo_surface_destroy(sprite);
  }

  /*
   * And past the renderer's own ceiling, where the document is drawn into a smaller raster and
   * scaled back to the size that was asked for. The box, the padding and the fit inside it are
   * all arithmetic in the size ASKED for, with the ceiling applied as a transform, so the
   * answer is the same one: derived a second time in the smaller raster's own integers, as this
   * was first written, the three truncations did not cancel and the guard came back anywhere
   * between nothing and two and a half pixels, while the raster's own truncated aspect -- not
   * quite the one asked for -- made rsvg letterbox the drawing inside its own box, measured at
   * 15 pixels either side of a 5000-pixel box with nothing to letterbox at all.
   *
   * A 25:1 drawing in a 25:1 box: no frame/document mismatch, so every pixel of the box is the
   * drawing's and anything short of it is the defect. The guard survives the ceiling while it
   * is still worth half a pixel of the smaller raster, which is a sprite up to 16384 px;
   * past that it is sub-pixel there and the scale back smears the ink into it, which is a
   * property of the ceiling and not of the placement.
   */
  gchar *wide = _write_svg("<svg xmlns='http://www.w3.org/2000/svg' width='250' height='10' "
                           "viewBox='0 0 250 10'><rect width='250' height='10' fill='#000000'/></svg>");
  gchar *wide_contents = NULL;
  gsize wide_length = 0;
  assert_true(g_file_get_contents(wide, &wide_contents, &wide_length, NULL));
  GBytes *wide_bytes = g_bytes_new_take(wide_contents, wide_length);
  static const int capped[][2] = { { 2000, 80 }, { 5000, 200 }, { 8000, 320 } };
  for(guint idx = 0; idx < G_N_ELEMENTS(capped); idx++)
  {
    const int pad = DT_CANVAS_SVG_GUARD;
    const int content_x = capped[idx][0];
    const int content_y = capped[idx][1];
    cairo_surface_t *sprite
        = dt_canvas_render_svg(wide_bytes, content_x + 2 * pad, content_y + 2 * pad, content_x, content_y, 0.0, 0.0);
    assert_non_null(sprite);
    assert_int_equal(cairo_image_surface_get_width(sprite), content_x + 2 * pad);
    const uint8_t *pixels = cairo_image_surface_get_data(sprite);
    const int stride = cairo_image_surface_get_stride(sprite);
    const int middle_row = (content_y + 2 * pad) / 2;
    const int middle_col = (content_x + 2 * pad) / 2;
    int first_row = -1;
    int last_row = -1;
    int first_col = -1;
    int last_col = -1;
    // Half coverage, so what is found is where the edge IS and not how far its fade reaches.
    for(int row = 0; row < content_y + 2 * pad; row++)
      if(pixels[(size_t)row * stride + middle_col * 4 + 3] >= 128)
      {
        if(first_row < 0) first_row = row;
        last_row = row;
      }
    for(int col = 0; col < content_x + 2 * pad; col++)
      if(pixels[(size_t)middle_row * stride + col * 4 + 3] >= 128)
      {
        if(first_col < 0) first_col = col;
        last_col = col;
      }
    if(first_row != pad || first_col != pad || last_row != content_y + pad - 1 || last_col != content_x + pad - 1)
      print_error("a %d x %d drawing sits at rows [%d..%d] cols [%d..%d], wanted [%d..%d] and [%d..%d]\n",
                  content_x, content_y, first_row, last_row, first_col, last_col, pad, content_y + pad - 1, pad,
                  content_x + pad - 1);
    assert_int_equal(first_row, pad);
    assert_int_equal(first_col, pad);
    assert_int_equal(last_row, content_y + pad - 1);
    assert_int_equal(last_col, content_x + pad - 1);
    cairo_surface_destroy(sprite);
  }
  g_bytes_unref(wide_bytes);
  g_remove(wide);
  g_free(wide);

  // Asked to fill the sprite, it fills the sprite: a photograph's bargain, and the path an
  // export takes, where there is no fractional box and so nothing to guard against.
  cairo_surface_t *filled = dt_canvas_render_svg(bytes, 104, 104, 0, 0, 0.0, 0.0);
  assert_non_null(filled);
  const uint8_t *full = cairo_image_surface_get_data(filled);
  const int full_stride = cairo_image_surface_get_stride(filled);
  assert_int_equal(full[(size_t)0 * full_stride + 52 * 4 + 3], 255);
  assert_int_equal(full[(size_t)103 * full_stride + 52 * 4 + 3], 255);
  cairo_surface_destroy(filled);

  g_bytes_unref(bytes);
  g_remove(path);
  g_free(path);
}

static void _a_feathered_cutout_covers_all_of_its_fade(void **state)
{
  (void)state;
  /*
   * A cut frame's edge FADES rather than stopping, and what the text has to clear is wherever
   * the picture paints something the eye can see -- not the contour where it happens to be
   * half opaque. Sampling the cutout at half took the text to the middle of the fade, where it
   * sat under the visible half of the picture's own soft rim: reported as the text
   * intersecting the border of a hexagonal cutout.
   *
   * A shape of radius r with a fall-off of f paints out to r + f, so it must push the text
   * exactly as far as a hard-edged shape of radius r + f does.
   */
  const double feathered = _flowed_past_a_cut_frame(0.10f, 0.30f);
  const double hard = _flowed_past_a_cut_frame(0.40f, 0.0f);
  const double small_and_hard = _flowed_past_a_cut_frame(0.10f, 0.0f);
  assert_true(feathered > 0.0 && hard > 0.0);
  // The fall-off is part of the picture: it costs the column what the same reach of hard edge
  // costs, and more than the shape without it.
  /*
   * Measured, a shape of 0.10 with a fall-off of 0.30 against hard shapes of 0.40 and 0.10:
   * 111.75, 130.38 and 74.50. The fade is worth most of its own width -- its faintest tail
   * stops a little short of the nominal reach, which is right, since there is nothing there to
   * see -- so the column pays for well over half of it. Sampling at half opacity instead put
   * the feathered shape at the bare one's 74.50 and the text under the visible half of the
   * picture's rim.
   */
  assert_true(feathered > small_and_hard);
  assert_true(feathered > (small_and_hard + hard) * 0.5);
  assert_true(feathered <= hard + 1.0);
}

static void _text_keeps_off_what_an_obstacle_paints_not_just_its_silhouette(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  assert_non_null(canvas);
  dt_canvas_object_t *text = dt_canvas_add_text(
      canvas, 200.0, 150.0, 360.0, 260.0,
      "Typography on an infinite plane demands that a paragraph break its lines the same way whatever the "
      "zoom, because the page is the thing being designed and the screen is only a window onto it.");
  assert_non_null(text);
  text->text.text_flags |= DT_CANVAS_TEXT_WRAP_AROUND;
  text->text.wrap_standoff = 0.0f;
  dt_canvas_object_t *over = dt_canvas_add_text(canvas, 300.0, 150.0, 140.0, 120.0, "");
  assert_non_null(over);
  over->border_width = 0.0f;
  over->flags |= DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE;

  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
  cairo_t *cr = cairo_create(surface);
  const double bare = dt_canvas_paint_text_natural_height(cr, canvas, text);
  // A border is painted OUTSIDE the silhouette the occupancy map is built from, so text set
  // flush against that silhouette lands under it. Measured on a cut picture over a column: the
  // run started exactly on the cutout edge -- the layout was right to 0.0 units -- and the
  // first word of five lines still vanished, into a 75-unit white border band.
  // The OVERRIDE flag with it: a frame without it takes the canvas's border, not its own, and
  // that is the whole of what this test is about.
  over->border_width = 40.0f;
  over->flags |= DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE;
  const double bordered = dt_canvas_paint_text_natural_height(cr, canvas, text);
  cairo_destroy(cr);
  cairo_surface_destroy(surface);

  assert_true(bare > 0.0);
  assert_true(bordered > bare);
  dt_canvas_free(canvas);
}

static void _an_auto_height_frame_grows_downward_and_settles(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  assert_non_null(canvas);
  dt_canvas_object_t *text = dt_canvas_add_text(
      canvas, 200.0, 150.0, 360.0, 120.0,
      "Typography on an infinite plane demands that a paragraph break its lines the same way whatever the "
      "zoom, because the page is the thing being designed and the screen is only a window onto it, and a "
      "column set beside a picture must keep clear of it line by line.");
  assert_non_null(text);
  text->text.text_flags |= DT_CANVAS_TEXT_WRAP_AROUND | DT_CANVAS_TEXT_AUTO_HEIGHT;
  text->text.wrap_standoff = 8.0f;
  // Over the frame's upper half, so what the first lines must avoid depends on where the top
  // edge is -- which is the whole point: growing the frame must not move that edge.
  dt_canvas_object_t *over = dt_canvas_add_text(canvas, 120.0, 120.0, 200.0, 120.0, "");
  assert_non_null(over);

  const double top_before = text->y - text->height * 0.5;
  assert_true(dt_canvas_paint_text_fit_height(canvas, text));
  const double top_after = text->y - text->height * 0.5;
  // The frame grew into the room below it; the edge the user placed did not move.
  assert_true(text->height > 120.0);
  assert_float_equal(top_before, top_after, 0.01);
  // And it settled: asking again changes nothing. Growing about the CENTRE instead moved the
  // top, changed the obstacles above the first lines, and gave a two-cycle the frame flipped
  // between on every repaint.
  assert_false(dt_canvas_paint_text_fit_height(canvas, text));
  dt_canvas_free(canvas);
}

/**
 * The frame the zoom glitch was reported on: a flowing, auto-height column with a star laid
 * over its upper half, so its height depends on what stands above each line and its lines
 * depend on the height. Shared by the four checks below, which differ only in what they ask
 * of it.
 */
static dt_canvas_object_t *_zoom_invariant_column(dt_canvas_t *canvas)
{
  dt_canvas_object_t *text = dt_canvas_add_text(
      canvas, 0.0, 0.0, 520.0, 420.0,
      "Typography on an infinite plane demands that a paragraph break its lines the same way "
      "whatever the zoom, because the page is the thing being designed and the screen is only a "
      "window onto it, and the measure a line is set to belongs to the page rather than to the "
      "window looking at it. A line is set across every clear stretch of its band, not the widest "
      "one, so a picture in the middle of a column leaves space either side, and the line carries "
      "on past it; what has to clear a picture is the glyphs, and a logical box carries the "
      "font's full ascent above the tallest of them.");
  if(IS_NULL_PTR(text)) return NULL;
  g_strlcpy(text->text.font, "DejaVu Serif 12", DT_CANVAS_FONT_LEN);
  text->text.padding = 10.0f;
  text->text.wrap_standoff = 12.0f;
  text->text.text_flags |= DT_CANVAS_TEXT_WRAP_AROUND | DT_CANVAS_TEXT_AUTO_HEIGHT;
  /*
   * Black type on a white ground, where a frame ships light type on a dark one, and a white
   * star: the painted check below reads a glyph as a pixel darker than the page, and none of
   * these colours reaches the layout -- an obstacle covers wherever it paints anything,
   * whatever colour it paints it in, so the star's geometry is the one that was reported. The
   * frame's own shadow goes for the same reason, a soft grey at its edge being indistinguishable
   * from type; the star keeps the canvas's, so what the text must flow around is untouched.
   */
  text->text.text_color = dt_canvas_color(0.0f, 0.0f, 0.0f, 1.0f);
  text->text.background = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
  memset(&text->shadow, 0, sizeof(text->shadow));
  text->flags |= DT_CANVAS_OBJECT_FLAG_SHADOW_OVERRIDE;
  canvas->grid_flags = 0;
  canvas->background = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
  dt_canvas_shape_style_t style = dt_canvas_shape_style_default();
  style.fill = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
  style.sides = 5;
  style.depth = DT_CANVAS_SHAPE_STAR_DEPTH;
  const dt_canvas_rect_t box = { -40.0, -200.0, 200.0, 140.0 };
  if(IS_NULL_PTR(dt_canvas_add_shape(canvas, DT_CANVAS_SHAPE_POLYGON, &box, &style))) return NULL;
  return text;
}

/** What the text frame comes to, measured on a scratch context carrying this matrix. */
static double _natural_height_at(const dt_canvas_t *canvas, const dt_canvas_object_t *text, const double ctm)
{
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
  cairo_t *cr = cairo_create(surface);
  cairo_scale(cr, ctm, ctm);
  const double height = dt_canvas_paint_text_natural_height(cr, canvas, text);
  cairo_destroy(cr);
  cairo_surface_destroy(surface);
  return height;
}

/** The ten viewports the glitch was measured over, from a quarter zoom to five times. */
static const double _glitch_ctms[] = { 0.2, 0.37, 0.5, 0.618, 0.8, 1.0, 1.37, 2.0, 3.1, 5.0 };

static void _a_text_frames_natural_height_is_the_same_at_every_zoom(void **state)
{
  (void)state;
  /*
   * `pango_cairo_create_layout()` copies the cairo CTM into the Pango context, so a font's
   * metrics come back rounded to a Pango unit AT DEVICE SCALE: DejaVu Serif 12's line box is
   * 13.968750 units at every viewport but 0.370, where it is 13.969727 -- a 1024th of a unit,
   * accumulating to 0.0238 down this frame. A height is a property of the DOCUMENT, so the
   * viewport that happens to ask must not be able to move it by any amount at all; the flow
   * engine works in 256ths of a unit for exactly that reason. Measured before the quantum:
   * 173.666992 at 0.2 and 0.37, 173.656250 at 0.5, 173.645508 at 0.8 -- three answers to one
   * question.
   */
  dt_canvas_t *canvas = dt_canvas_new();
  assert_non_null(canvas);
  dt_canvas_object_t *text = _zoom_invariant_column(canvas);
  assert_non_null(text);
  const double reference = _natural_height_at(canvas, text, _glitch_ctms[0]);
  assert_true(reference > 0.0);
  for(size_t idx = 1; idx < sizeof(_glitch_ctms) / sizeof(_glitch_ctms[0]); idx++)
  {
    const double height = _natural_height_at(canvas, text, _glitch_ctms[idx]);
    if(height != reference)
    {
      print_error("natural height %.6f at a CTM of %.3f against %.6f at %.3f\n", height, _glitch_ctms[idx],
                  reference, _glitch_ctms[0]);
      fail();
    }
  }
  dt_canvas_free(canvas);
}

static void _a_leaded_text_frames_natural_height_is_the_same_at_every_zoom(void **state)
{
  (void)state;
  /*
   * The same question asked of a frame whose lines are led apart. A leading is a context
   * metric like the line box, and drifts with the viewport the same way: measured before the
   * quantum, this frame came to 222.565430 at two viewports, 222.546875 at seven and
   * 222.537109 at one. It needs its own check because the flow engine reads the leading from
   * `pango_layout_get_spacing()`, which is only ever non-zero when a frame asks for a line
   * height of its own -- so a frame that leaves the leading alone never exercises it, and the
   * quantum there could be removed with every other check still green.
   */
  dt_canvas_t *canvas = dt_canvas_new();
  assert_non_null(canvas);
  dt_canvas_object_t *text = _zoom_invariant_column(canvas);
  assert_non_null(text);
  text->text.line_height = 1.5f;
  dt_canvas_touch(canvas);
  const double reference = _natural_height_at(canvas, text, _glitch_ctms[0]);
  assert_true(reference > 0.0);
  for(size_t idx = 1; idx < sizeof(_glitch_ctms) / sizeof(_glitch_ctms[0]); idx++)
  {
    const double height = _natural_height_at(canvas, text, _glitch_ctms[idx]);
    if(height != reference)
    {
      print_error("led natural height %.6f at a CTM of %.3f against %.6f at %.3f\n", height, _glitch_ctms[idx],
                  reference, _glitch_ctms[0]);
      fail();
    }
  }
  dt_canvas_free(canvas);
}

static void _a_fitted_text_frame_is_exactly_its_natural_height(void **state)
{
  (void)state;
  /*
   * `_flow_text()`'s cut is exact -- deliberately, since a tolerance there would let a line
   * overflow a frame the user sized by hand -- so the fit must leave the frame exactly as tall
   * as its text came to, not within a hundredth of a unit of it. Breaking before the
   * assignment left it short, and the last line then fell the wrong side of the cut.
   *
   * The sweep covers the products the painter's own layer matrix produces, zoom times device
   * scale times the quality a gesture drops to. The painter folds all three into ONE scalar
   * (`cairo_matrix_init_scale()` then `cairo_matrix_multiply()` in `_paint_canvas()`, applied
   * by `_layer_context()`), so a product is the whole of what the metrics are rounded at and
   * the twenty triples below are sixteen distinct matrices -- the duplicates cost nothing and
   * are kept because the triple is how a reader thinks of a viewport. A frame shorter than the
   * natural height at any one of them drops its last line there.
   */
  dt_canvas_t *canvas = dt_canvas_new();
  assert_non_null(canvas);
  dt_canvas_object_t *text = _zoom_invariant_column(canvas);
  assert_non_null(text);
  assert_true(dt_canvas_paint_text_fit_height(canvas, text));
  // Settled: asking again changes nothing, and reports that nothing changed.
  assert_false(dt_canvas_paint_text_fit_height(canvas, text));

  const double zooms[] = { 0.2, 0.37, 0.5, 1.0, 2.0 };
  for(size_t idx = 0; idx < sizeof(zooms) / sizeof(zooms[0]); idx++)
  {
    for(int device_scale = 1; device_scale <= 2; device_scale++)
    {
      for(int reduced = 0; reduced <= 1; reduced++)
      {
        const double ctm = zooms[idx] * (double)device_scale * (reduced ? 0.5 : 1.0);
        const double height = _natural_height_at(canvas, text, ctm);
        if(height != text->height)
        {
          print_error("zoom %.3f x device scale %d x quality %.1f: the frame stands at %.6f and its text "
                      "comes to %.6f\n",
                      zooms[idx], device_scale, reduced ? 0.5 : 1.0, text->height, height);
          fail();
        }
      }
    }
  }
  for(size_t idx = 0; idx < sizeof(_glitch_ctms) / sizeof(_glitch_ctms[0]); idx++)
  {
    const double height = _natural_height_at(canvas, text, _glitch_ctms[idx]);
    if(height != text->height)
    {
      print_error("CTM %.3f: the frame stands at %.6f and its text comes to %.6f\n", _glitch_ctms[idx],
                  text->height, height);
      fail();
    }
  }
  dt_canvas_free(canvas);
}

/**
 * Paint the whole canvas at this viewport and report the lowest inked row WITHIN the text
 * frame's own box, in CANVAS units measured down from the plane's origin. Black type on a
 * white page and a white star, so anything darker than the page inside the box is a glyph and
 * nothing else. The row is converted back through the viewport, so viewports that rasterise at
 * wildly different densities give comparable answers.
 * @return FALSE when no type was painted inside the frame at all.
 */
static gboolean _painted_ink_bottom(const dt_canvas_t *canvas, const dt_canvas_object_t *text, const double zoom,
                                    const double device_scale, const double quality, double *bottom)
{
  const int size = (int)(760.0 * device_scale);
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, size, size);
  cairo_surface_set_device_scale(surface, device_scale, device_scale);
  cairo_t *cr = cairo_create(surface);
  const double half = 0.5 * size / device_scale;
  cairo_translate(cr, half, half);
  cairo_scale(cr, zoom, zoom);
  const dt_canvas_rect_t whole = { -half / zoom, -half / zoom, 2.0 * half / zoom, 2.0 * half / zoom };
  dt_canvas_surface_cache_t *cache = dt_canvas_surface_cache_new(FALSE, 64u * 1024u * 1024u);
  dt_canvas_paint_options_t options = dt_canvas_paint_options_export(cache, 1.0 / zoom, whole);
  options.quality = quality;
  dt_canvas_paint(cr, canvas, &options);
  cairo_destroy(cr);
  cairo_surface_flush(surface);
  const uint8_t *pixels = cairo_image_surface_get_data(surface);
  const int stride = cairo_image_surface_get_stride(surface);
  /* the frame's box in the surface's own pixels, so nothing outside the column is read */
  const double top_edge = (text->y - text->height * 0.5) * zoom + half;
  const double bottom_edge = (text->y + text->height * 0.5) * zoom + half;
  const double left_edge = (text->x - text->width * 0.5) * zoom + half;
  const double right_edge = (text->x + text->width * 0.5) * zoom + half;
  const int first_row = CLAMP((int)floor(top_edge * device_scale), 0, size - 1);
  const int last_row = CLAMP((int)ceil(bottom_edge * device_scale), 0, size - 1);
  const int first_col = CLAMP((int)floor(left_edge * device_scale), 0, size - 1);
  const int last_col = CLAMP((int)ceil(right_edge * device_scale), 0, size - 1);
  int lowest = -1;
  for(int row = last_row; row >= first_row && lowest < 0; row--)
  {
    for(int col = first_col; col <= last_col; col++)
    {
      const uint32_t pixel = *(const uint32_t *)(pixels + (size_t)row * stride + (size_t)col * 4) & 0xFFFFFFu;
      const int luminance = (int)(((pixel >> 16) & 0xFF) + ((pixel >> 8) & 0xFF) + (pixel & 0xFF)) / 3;
      // Generous, because the coarsest viewport here rasterises a stem across a fifth of a
      // pixel: a threshold tight enough to want a solid black found no type at all there.
      if(luminance < 215)
      {
        lowest = row;
        break;
      }
    }
  }
  dt_canvas_surface_cache_free(cache);
  cairo_surface_destroy(surface);
  if(lowest < 0) return FALSE;
  *bottom = ((double)lowest / device_scale - half) / zoom;
  return TRUE;
}

/**
 * A gesture drops the quality, and that must reach the pictures and NOTHING else. Painted twice
 * over the same canvas at quality 1 and quality 0.5, every pixel must be the same byte as long
 * as no picture or drawing stands on it: the quality used to scale the layer matrix, so the
 * whole page was composited at half the resolution and scaled back up, and every line of type
 * lost its stems for the 180 ms after any drag anywhere -- measured at zoom 1, 672 fully dark
 * glyph pixels at rest against 0 mid-gesture.
 */
/**
 * The black square of a drawing, in the surface's own pixels, painted through the whole page.
 *
 * `phase` shifts the whole page by that many device pixels before the zoom, which is what a pan
 * does between two frames: the sprite is blitted at a WHOLE pixel and the box it stands for is
 * not, so every sub-pixel alignment has to give the same answer or a drawing breathes as the
 * page is dragged.
 */
static gboolean _painted_drawing_span(const dt_canvas_t *canvas, const double zoom, const double device_scale,
                                      const double quality, const double phase, double *span_x, double *span_y)
{
  const int size = (int)lround(1024.0 * device_scale);
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, size, size);
  cairo_surface_set_device_scale(surface, device_scale, device_scale);
  cairo_t *cr = cairo_create(surface);
  const double half = 0.5 * size / device_scale;
  cairo_translate(cr, half + phase / device_scale, half + phase / device_scale);
  cairo_scale(cr, zoom, zoom);
  const dt_canvas_rect_t whole = { -half / zoom, -half / zoom, 2.0 * half / zoom, 2.0 * half / zoom };
  dt_canvas_surface_cache_t *cache = dt_canvas_surface_cache_new(FALSE, 64u * 1024u * 1024u);
  dt_canvas_paint_options_t options = dt_canvas_paint_options_export(cache, 1.0 / zoom, whole);
  options.quality = quality;
  dt_canvas_paint(cr, canvas, &options);
  cairo_destroy(cr);
  cairo_surface_flush(surface);
  const uint8_t *pixels = cairo_image_surface_get_data(surface);
  const int stride = cairo_image_surface_get_stride(surface);
  int first_row = -1;
  int last_row = -1;
  int first_col = size;
  int last_col = -1;
  for(int row = 0; row < size; row++)
  {
    for(int col = 0; col < size; col++)
    {
      const uint32_t pixel = *(const uint32_t *)(pixels + (size_t)row * stride + (size_t)col * 4) & 0xFFFFFFu;
      const int luminance = (int)(((pixel >> 16) & 0xFF) + ((pixel >> 8) & 0xFF) + (pixel & 0xFF)) / 3;
      // Half way up the ink's own ramp, so an edge is found where it actually lies whether the
      // sprite was blitted one pixel to one or scaled back up from a gesture's raster.
      if(luminance >= 128) continue;
      if(first_row < 0) first_row = row;
      last_row = row;
      if(col < first_col) first_col = col;
      if(col > last_col) last_col = col;
    }
  }
  dt_canvas_surface_cache_free(cache);
  cairo_surface_destroy(surface);
  if(first_row < 0 || last_col < first_col) return FALSE;
  *span_x = (double)(last_col - first_col + 1);
  *span_y = (double)(last_row - first_row + 1);
  return TRUE;
}

/**
 * A drawing fills the frame it was given, at every zoom, quality, screen and sub-pixel phase.
 *
 * The air a drawing wants around it is the CALLER's, added around the sprite and paid back at
 * the blit. Taken out of the DRAWING, which is what the guard did until this was measured, it
 * is a fixed number of SCREEN pixels charged to a box whose size is the zoom's: the same
 * drawing filled 84.0% of its frame at a quarter zoom and 98.7% at three times, so it breathed
 * against its own border on every wheel click. Subtracting it from both axes changed the drawn
 * box's proportions as well, and rsvg's default `xMidYMid meet` then letterboxed the document
 * inside it -- a second inset, also the zoom's, that moved the drawing off its own corner.
 *
 * Measured in absolute device pixels rather than as a fraction, because that is what the defect
 * is: a fixed cost. The drawing here fills its document, and its frame carries the document's
 * proportions, so the ink's span IS the frame's, to within the pixel the box's own fractional
 * position costs and the half a threshold costs at each edge.
 */
/**
 * Where a hard vertical edge inside a drawing stands, to a fraction of a device pixel.
 *
 * The first column that is not the page's white holds the edge: what is left of it is the
 * white the edge did not cover, and the layer's own 563/256 gamma turns that code back into
 * the fraction. Spelled here rather than borrowed, so the check does not depend on the
 * encoder it is checking.
 */
static gboolean _painted_edge_position(const dt_canvas_t *canvas, dt_canvas_surface_cache_t *cache,
                                       const double device_scale, const double phase, const int from_column,
                                       double *position)
{
  const int size = (int)lround(1024.0 * device_scale);
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, size, size);
  cairo_surface_set_device_scale(surface, device_scale, device_scale);
  cairo_t *cr = cairo_create(surface);
  const double half = 0.5 * size / device_scale;
  cairo_translate(cr, half + phase / device_scale, half + phase / device_scale);
  const dt_canvas_rect_t whole = { -half, -half, 2.0 * half, 2.0 * half };
  dt_canvas_paint_options_t options = dt_canvas_paint_options_export(cache, 1.0, whole);
  dt_canvas_paint(cr, canvas, &options);
  cairo_destroy(cr);
  cairo_surface_flush(surface);
  const uint8_t *pixels = cairo_image_surface_get_data(surface);
  const int stride = cairo_image_surface_get_stride(surface);
  const int row = size / 2;
  gboolean found = FALSE;
  for(int col = from_column; col < size && !found; col++)
  {
    const int green = (int)((*(const uint32_t *)(pixels + (size_t)row * stride + (size_t)col * 4) >> 8) & 0xFF);
    if(green >= 255) continue;
    *position = (double)col + pow((double)green / 255.0, 563.0 / 256.0);
    found = TRUE;
  }
  cairo_surface_destroy(surface);
  return found;
}

/**
 * A drawing slides with the page instead of crabbing against it.
 *
 * A sprite is blitted one pixel to one at a WHOLE pixel -- that is what makes the sprite path
 * cheap -- so a box that begins at a fractional pixel had its content quantised to the grid:
 * measured on a drawing panned in eighth-pixel steps, an edge inside it stood at the same
 * column for EIGHT frames and then jumped a whole one, hard-edged, while the frame's own
 * border and every glyph beside it slid smoothly by an eighth each time. Against a border the
 * canvas draws at its true position, that is a picture that will not sit still.
 *
 * The drawing is rendered at the fraction instead of blitted there, snapped to
 * DT_CANVAS_SVG_PHASE_STEPS so the sprite cache stays small: the edge then advances in halves
 * with real anti-aliasing at each one, never more than a quarter of a pixel from where it
 * belongs, and never backwards. The edge measured here is INSIDE the document, so it is the
 * sprite's own placement being read and not the frame's clip, which was always at its true
 * sub-pixel position and hid the defect at both edges of the frame.
 */
/**
 * A picture keeps fewer sprites than a drawing, because it has fewer to keep.
 *
 * The slots exist for a DRAWING's sub-pixel phases -- four of them, at the gesture's quality
 * beside the one at rest -- and a picture has no phase at all, its sprite being a resample. So
 * the eight the drawing needs would let one picture hold four times the sprites it can use, at
 * 14 MB apiece for a full-screen one, and `_cache_evict_to_budget()` sheds whole entries and
 * never a cold slot of the object it is painting: the budget cannot take it back.
 *
 * Read by identity, with a reference held on the sprite under test so that a freed one cannot
 * be handed back at the same address and read as a hit.
 */
static void _a_picture_keeps_fewer_sprites_than_a_drawing(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  canvas->grid_flags = 0;
  dt_canvas_object_t *picture = dt_canvas_add_image(canvas, 0.0, 0.0, 64, 64);
  assert_non_null(picture);
  GBytes *jpeg = _flat_jpeg(64, 64, 10, 20, 30);
  dt_canvas_image_set_render(canvas, picture, jpeg, 64, 64, 0, 0, DT_CANVAS_COLORSPACE_SRGB);
  g_bytes_unref(jpeg);

  gchar *path = _write_svg("<svg xmlns='http://www.w3.org/2000/svg' width='64' height='64' "
                           "viewBox='0 0 64 64'><rect width='64' height='64' fill='#000000'/></svg>");
  GError *error = NULL;
  dt_canvas_object_t *drawing = dt_canvas_add_svg(canvas, 0.0, 0.0, path, &error);
  assert_non_null(drawing);

  dt_canvas_surface_cache_t *cache = dt_canvas_surface_cache_new(FALSE, 256u * 1024u * 1024u);

  // Three sizes of the picture, and the first is gone: two is what it may keep.
  cairo_surface_t *first = dt_canvas_surface_cache_get_scaled(cache, picture, 100, 100, 0, 0, 0.0, 0.0);
  assert_non_null(first);
  cairo_surface_reference(first);
  assert_non_null(dt_canvas_surface_cache_get_scaled(cache, picture, 200, 200, 0, 0, 0.0, 0.0));
  assert_non_null(dt_canvas_surface_cache_get_scaled(cache, picture, 300, 300, 0, 0, 0.0, 0.0));
  cairo_surface_t *again = dt_canvas_surface_cache_get_scaled(cache, picture, 100, 100, 0, 0, 0.0, 0.0);
  assert_non_null(again);
  assert_ptr_not_equal(again, first);
  cairo_surface_destroy(first);

  /*
   * The drawing's four phases of ONE size all survive, which is the whole reason the slots were
   * raised: a diagonal pan visits every one of them and would otherwise be an rsvg render per
   * frame. The padding is the caller's, so the sprite is the box plus two guards either way.
   */
  const int pad = DT_CANVAS_SVG_GUARD;
  const int sprite = 100 + 2 * pad;
  cairo_surface_t *phases[4] = { NULL, NULL, NULL, NULL };
  for(int at = 0; at < 4; at++)
  {
    phases[at] = dt_canvas_surface_cache_get_scaled(cache, drawing, sprite, sprite, 100, 100,
                                                    0.5 * (at & 1), 0.5 * ((at >> 1) & 1));
    assert_non_null(phases[at]);
    for(int earlier = 0; earlier < at; earlier++) assert_ptr_not_equal(phases[at], phases[earlier]);
  }
  for(int at = 0; at < 4; at++)
  {
    cairo_surface_t *kept = dt_canvas_surface_cache_get_scaled(cache, drawing, sprite, sprite, 100, 100,
                                                               0.5 * (at & 1), 0.5 * ((at >> 1) & 1));
    if(kept != phases[at]) print_error("the drawing lost the sprite for phase %d\n", at);
    assert_ptr_equal(kept, phases[at]);
  }

  dt_canvas_surface_cache_free(cache);
  dt_canvas_free(canvas);
  g_remove(path);
  g_free(path);
}

/**
 * A text casts a shadow from its GLYPHS, and it is a new thing only for a frame with a ground.
 *
 * A shadow's silhouette is the layer's alpha, so a text frame with no background and no border
 * already shadowed its letters through the frame's own shadow. What this adds is the same for a
 * frame that HAS a ground, where the frame's shadow is cast by the ground and the letters
 * cast nothing -- and a shadow with an offset and NO blur, which the frame's own predicate
 * refuses because an unblurred rectangle behind a rectangle is a rectangle.
 */
static void _a_text_casts_a_shadow_from_its_glyphs(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  canvas->grid_flags = 0;
  canvas->paper_size = DT_CANVAS_PAPER_NONE;
  canvas->background = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
  dt_canvas_object_t *text = dt_canvas_add_text(canvas, 0.0, 0.0, 300.0, 120.0, "Hg");
  assert_non_null(text);
  g_strlcpy(text->text.font, "DejaVu Serif 64", DT_CANVAS_FONT_LEN);
  text->text.text_color = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
  // A ground under the letters, so the frame's own shadow could only ever be the ground's.
  text->text.background = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
  text->border_width = 0.0f;
  text->flags |= DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE;
  memset(&text->shadow, 0, sizeof(text->shadow));
  text->flags |= DT_CANVAS_OBJECT_FLAG_SHADOW_OVERRIDE;

  // White letters on a white ground: nothing to see at all.
  const uint32_t plain = _painted_pixel(canvas, 400, 200, 200);

  /* A hard drop shadow: an offset and no blur, which is what a typesetter draws and what the
   * frame's own shadow predicate refuses. Something dark must now appear inside the frame. */
  text->text.shadow.color = dt_canvas_color(0.0f, 0.0f, 0.0f, 1.0f);
  text->text.shadow.offset_x = 3.0f;
  text->text.shadow.offset_y = 3.0f;
  text->text.shadow.blur = 0.0f;
  assert_true(dt_canvas_text_shadow_visible(&text->text.shadow));
  assert_false(dt_canvas_shadow_visible(&text->text.shadow));

  int dark = 0;
  for(int at = 0; at < 300; at++)
  {
    const uint32_t pixel = _painted_pixel(canvas, 400, 150 + at % 120, 140 + at / 3);
    if((int)(((pixel >> 16) & 0xFF) + ((pixel >> 8) & 0xFF) + (pixel & 0xFF)) / 3 < 128) dark++;
  }
  if(dark == 0) print_error("a hard drop shadow painted nothing inside the frame\n");
  assert_true(dark > 0);
  assert_int_equal(plain, 0xFFFFFFu);

  // And with no opacity there is no shadow, whatever the offsets say.
  text->text.shadow.color.alpha = 0.0f;
  assert_false(dt_canvas_text_shadow_visible(&text->text.shadow));

  dt_canvas_free(canvas);
}

/*
 * A shadow's extent grows the silhouette BEFORE the blur, so a wide blur fades a solid shadow
 * rather than spreading the same thin ink ever thinner. Judged where it was reported: beside a
 * thin bar under a wide blur, where the shadow had all but vanished, and inside a frame under a
 * wide inset blur, where the same held for the uncovered world.
 */
static void _a_shadows_extent_carries_its_strength_past_a_wide_blur(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  canvas->grid_flags = 0;
  canvas->paper_size = DT_CANVAS_PAPER_NONE;
  canvas->background = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
  // A bar four units wide, white on white: only its shadow can show.
  dt_canvas_object_t *bar = dt_canvas_add_text(canvas, 0.0, 0.0, 4.0, 120.0, "");
  bar->text.background = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
  bar->border_width = 0.0f;
  bar->flags |= DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE | DT_CANVAS_OBJECT_FLAG_SHADOW_OVERRIDE;
  bar->shadow.color = dt_canvas_color(0.0f, 0.0f, 0.0f, 1.0f);
  bar->shadow.offset_x = 0.0f;
  bar->shadow.offset_y = 0.0f;
  bar->shadow.blur = 20.0f;
  bar->shadow.extent = 0.0f;
  // Ten units past the bar's edge, halfway down it.
  const uint32_t washed = _painted_pixel(canvas, 200, 112, 100) & 0xFF;
  dt_canvas_rect_t narrow;
  assert_true(dt_canvas_paint_object_extent(canvas, bar, &narrow));
  bar->shadow.extent = 15.0f;
  const uint32_t carried = _painted_pixel(canvas, 200, 112, 100) & 0xFF;
  printf("beside a 4-unit bar under a 20-unit blur: code %u, %u with an extent of 15\n", washed, carried);
  // Four units of ink under a sigma of twenty leave about 7 % of the colour ten units out...
  assert_true(washed >= 235);
  // ...where thirty-four units of it leave about half.
  assert_true(carried <= 205);
  // What a move repaints reaches exactly as much further, on both sides.
  dt_canvas_rect_t wide;
  assert_true(dt_canvas_paint_object_extent(canvas, bar, &wide));
  assert_float_equal(wide.width - narrow.width, 30.0, 1e-6);

  // Inset: thirty units inside a frame's left edge, halfway down it.
  dt_canvas_remove_object(canvas, bar->id);
  dt_canvas_object_t *frame = dt_canvas_add_text(canvas, 0.0, 0.0, 120.0, 120.0, "");
  frame->text.background = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
  frame->border_width = 0.0f;
  frame->flags |= DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE | DT_CANVAS_OBJECT_FLAG_SHADOW_OVERRIDE;
  frame->shadow.color = dt_canvas_color(0.0f, 0.0f, 0.0f, 1.0f);
  frame->shadow.offset_x = 0.0f;
  frame->shadow.offset_y = 0.0f;
  frame->shadow.blur = 20.0f;
  frame->shadow.inset = TRUE;
  frame->shadow.extent = 0.0f;
  const uint32_t faint = _painted_pixel(canvas, 200, 70, 100) & 0xFF;
  frame->shadow.extent = 25.0f;
  const uint32_t deep = _painted_pixel(canvas, 200, 70, 100) & 0xFF;
  printf("30 units inside a frame under a 20-unit inset blur: code %u, %u with an extent of 25\n", faint, deep);
  assert_true(faint >= 235);
  assert_true(deep <= 215);
  // And the grow stays inside the frame: nothing lands on the canvas past its edge.
  assert_int_equal(_painted_pixel(canvas, 200, 30, 100), 0xFFFFFFu);
  /* An extent with NO blur, cast inside: a hard band along the frame's edges, the extent wide. A
   * signed radius could not say this at all -- a radius of nothing was no shadow -- which is what
   * the shadow's own switch is for. The frame's left edge is pixel 40; the segments reach within a
   * pixel of 10 here. */
  frame->shadow.blur = 0.0f;
  frame->shadow.extent = 10.0f;
  assert_true(dt_canvas_shadow_visible(&frame->shadow));
  assert_int_equal(_painted_pixel(canvas, 200, 40 + 7, 100), 0x000000u);
  assert_int_equal(_painted_pixel(canvas, 200, 40 + 13, 100), 0xFFFFFFu);
  assert_int_equal(_painted_pixel(canvas, 200, 100, 100), 0xFFFFFFu);
  assert_int_equal(_painted_pixel(canvas, 200, 40 - 3, 100), 0xFFFFFFu);

  /* With next to no blur the grown silhouette IS the shadow, so it must reach the whole extent past
   * the frame at full strength, both ways. This is the case the plane's padding exists for: the
   * layer leaves no room beyond the grown silhouette, and the band the grow cannot compute would
   * otherwise cut the shadow short of its extent. The segments reach within a pixel of 40 here. */
  dt_canvas_remove_object(canvas, frame->id);
  dt_canvas_object_t *tile = dt_canvas_add_text(canvas, 0.0, 0.0, 40.0, 40.0, "");
  tile->text.background = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
  tile->border_width = 0.0f;
  tile->flags |= DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE | DT_CANVAS_OBJECT_FLAG_SHADOW_OVERRIDE;
  tile->shadow.color = dt_canvas_color(0.0f, 0.0f, 0.0f, 1.0f);
  tile->shadow.offset_x = 0.0f;
  tile->shadow.offset_y = 0.0f;
  tile->shadow.blur = 0.1f;
  tile->shadow.extent = 40.0f;
  // The tile spans pixels 80 to 119; its shadow 40 either side of that.
  assert_int_equal(_painted_pixel(canvas, 200, 120 + 38, 100), 0x000000u);
  assert_int_equal(_painted_pixel(canvas, 200, 79 - 38, 100), 0x000000u);
  assert_int_equal(_painted_pixel(canvas, 200, 100, 120 + 38), 0x000000u);
  assert_int_equal(_painted_pixel(canvas, 200, 120 + 42, 100), 0xFFFFFFu);
  assert_int_equal(_painted_pixel(canvas, 200, 79 - 42, 100), 0xFFFFFFu);
  dt_canvas_free(canvas);
}

/* Grown with no blur and no offset, the glyphs' own shadow is an outline round the letters. */
static void _a_grown_glyph_shadow_outlines_the_letters(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  canvas->grid_flags = 0;
  canvas->paper_size = DT_CANVAS_PAPER_NONE;
  canvas->background = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
  dt_canvas_object_t *text = dt_canvas_add_text(canvas, 0.0, 0.0, 300.0, 120.0, "Hg");
  assert_non_null(text);
  g_strlcpy(text->text.font, "DejaVu Serif 64", DT_CANVAS_FONT_LEN);
  // White letters on nothing, over white: invisible until something is drawn round them.
  text->text.text_color = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
  text->text.background = dt_canvas_color(1.0f, 1.0f, 1.0f, 0.0f);
  text->border_width = 0.0f;
  text->flags |= DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE | DT_CANVAS_OBJECT_FLAG_SHADOW_OVERRIDE;
  memset(&text->shadow, 0, sizeof(text->shadow));
  text->text.shadow.color = dt_canvas_color(0.0f, 0.0f, 0.0f, 1.0f);
  int dark[2] = { 0, 0 };
  for(int pass = 0; pass < 2; pass++)
  {
    text->text.shadow.extent = pass == 0 ? 0.0f : 3.0f;
    for(int at = 0; at < 600; at++)
    {
      const uint32_t pixel = _painted_pixel(canvas, 400, 60 + at % 280, 150 + at / 6);
      if((int)(((pixel >> 16) & 0xFF) + ((pixel >> 8) & 0xFF) + (pixel & 0xFF)) / 3 < 128) dark[pass]++;
    }
  }
  printf("dark pixels round the letters: %d with no extent, %d with an extent of 3\n", dark[0], dark[1]);
  assert_int_equal(dark[0], 0);
  assert_true(dark[1] > 0);
  dt_canvas_free(canvas);
}

/*
 * Cast inside the letters, the glyphs' shadow falls ON them, within their own coverage, and never on
 * the ground around them: dark letters out of white ones, and not a dark pixel anywhere the letters
 * are not. Judged against the letters' own mask, taken from the same text set in black.
 */
static void _a_glyph_shadow_cast_inside_stays_on_the_letters(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  canvas->grid_flags = 0;
  canvas->paper_size = DT_CANVAS_PAPER_NONE;
  canvas->background = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
  dt_canvas_object_t *text = dt_canvas_add_text(canvas, 0.0, 0.0, 300.0, 120.0, "Hg");
  assert_non_null(text);
  g_strlcpy(text->text.font, "DejaVu Sans Bold 72", DT_CANVAS_FONT_LEN);
  text->text.background = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
  text->border_width = 0.0f;
  text->flags |= DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE | DT_CANVAS_OBJECT_FLAG_SHADOW_OVERRIDE;
  memset(&text->shadow, 0, sizeof(text->shadow));

  // The letters' mask: the same text in black, no shadow.
  text->text.text_color = dt_canvas_color(0.0f, 0.0f, 0.0f, 1.0f);
  memset(&text->text.shadow, 0, sizeof(text->text.shadow));
  const int size = 400;
  gboolean *letter = g_new0(gboolean, (size_t)size * size);
  int letters = 0;
  for(int at = 0; at < size * size; at += 7)
  {
    const uint32_t pixel = _painted_pixel(canvas, size, at % size, at / size);
    letter[at] = (pixel & 0xFF) < 250;
    if(letter[at]) letters++;
  }
  assert_true(letters > 0);

  // White letters, their shadow cast inside them, down and right, hard.
  text->text.text_color = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
  text->text.shadow.color = dt_canvas_color(0.0f, 0.0f, 0.0f, 1.0f);
  text->text.shadow.offset_x = 3.0f;
  text->text.shadow.offset_y = 3.0f;
  text->text.shadow.inset = TRUE;
  assert_true(dt_canvas_text_shadow_visible(&text->text.shadow));
  int dark = 0;
  int astray = 0;
  for(int at = 0; at < size * size; at += 7)
  {
    const uint32_t pixel = _painted_pixel(canvas, size, at % size, at / size);
    if((pixel & 0xFF) >= 128) continue;
    dark++;
    if(!letter[at]) astray++;
  }
  printf("inset glyph shadow: %d dark samples, %d of them off the letters\n", dark, astray);
  assert_true(dark > 0);
  assert_int_equal(astray, 0);
  // It reaches nothing past the frame, so what a move repaints is the frame's alone.
  dt_canvas_rect_t with_shadow;
  assert_true(dt_canvas_paint_object_extent(canvas, text, &with_shadow));
  text->text.shadow.color.alpha = 0.0f;
  dt_canvas_rect_t without;
  assert_true(dt_canvas_paint_object_extent(canvas, text, &without));
  assert_float_equal(with_shadow.width, without.width, 1e-6);
  g_free(letter);
  dt_canvas_free(canvas);
}

static void _a_drawing_slides_with_the_page_instead_of_crabbing_against_it(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  assert_non_null(canvas);
  canvas->grid_flags = 0;
  canvas->background = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
  // A stripe a hundred units in, so neither of its edges is the frame's own.
  gchar *path = _write_svg("<svg xmlns='http://www.w3.org/2000/svg' width='240' height='240' "
                           "viewBox='0 0 240 240'><rect x='100' y='0' width='40' height='240' "
                           "fill='#000000'/></svg>");
  GError *error = NULL;
  dt_canvas_object_t *drawing = dt_canvas_add_svg(canvas, 0.0, 0.0, path, &error);
  assert_non_null(drawing);
  drawing->x = 0.0;
  drawing->y = 0.0;

  for(int device_scale = 1; device_scale <= 2; device_scale++)
  {
    /*
     * ONE cache across the whole pan, which is what the atelier does -- the view builds a
     * surface cache for its lifetime and clears it only when the document or the profile
     * changes. A cache built and freed per frame, which is what this checked at first, hands
     * every call an empty entry: the slot loop short-circuits on the NULL sprite before it ever
     * compares a phase, so the key this test exists for was never once exercised. Measured
     * under gdb over the whole suite: 203 calls into the sprite cache, 66 of them a drawing,
     * not one of them meeting a filled slot.
     */
    dt_canvas_surface_cache_t *cache = dt_canvas_surface_cache_new(FALSE, 64u * 1024u * 1024u);
    double previous = -1.0;
    for(int step = 0; step <= 16; step++)
    {
      const double phase = 0.125 * step;
      const double frame_left = 0.5 * 1024.0 * device_scale + phase - 0.5 * drawing->width * device_scale;
      const double wanted = frame_left + 100.0 * device_scale;
      double edge = 0.0;
      assert_true(_painted_edge_position(canvas, cache, (double)device_scale, phase,
                                         (int)floor(frame_left) + 1, &edge));
      if(fabs(edge - wanted) > 0.3 || edge < previous)
        print_error("scale %d pan %5.3f: the edge stands at %8.3f, wanted %8.3f, after %8.3f\n", device_scale,
                    phase, edge, wanted, previous);
      // A quarter of a pixel is the most a half-pixel phase can be out by; the rest is the
      // renderer's own rounding of the coverage into a code.
      assert_true(fabs(edge - wanted) <= 0.3);
      // And it never goes backwards while the page goes forwards.
      assert_true(edge >= previous);
      previous = edge;
    }
    dt_canvas_surface_cache_free(cache);
  }

  dt_canvas_free(canvas);
  g_remove(path);
  g_free(path);
}

static void _a_drawing_fills_its_frame_at_every_zoom(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  assert_non_null(canvas);
  canvas->grid_flags = 0;
  canvas->background = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
  gchar *path = _write_svg("<svg xmlns='http://www.w3.org/2000/svg' width='240' height='240' "
                           "viewBox='0 0 240 240'><rect width='240' height='240' fill='#000000'/></svg>");
  GError *error = NULL;
  dt_canvas_object_t *drawing = dt_canvas_add_svg(canvas, 0.0, 0.0, path, &error);
  assert_non_null(drawing);
  drawing->x = 0.0;
  drawing->y = 0.0;
  const double frame_width = drawing->width;
  const double frame_height = drawing->height;
  assert_true(frame_width > 0.0 && frame_height > 0.0);

  static const double zooms[] = { 0.25, 0.5, 1.0, 3.1 };
  static const double qualities[] = { 0.5, 1.0 };
  static const double phases[] = { 0.0, 0.5 };
  for(guint zoom_idx = 0; zoom_idx < G_N_ELEMENTS(zooms); zoom_idx++)
    for(guint quality_idx = 0; quality_idx < G_N_ELEMENTS(qualities); quality_idx++)
      for(int device_scale = 1; device_scale <= 2; device_scale++)
        for(guint phase_idx = 0; phase_idx < G_N_ELEMENTS(phases); phase_idx++)
        {
          const double zoom = zooms[zoom_idx];
          const double quality = qualities[quality_idx];
          double span_x = 0.0;
          double span_y = 0.0;
          assert_true(_painted_drawing_span(canvas, zoom, (double)device_scale, quality, phases[phase_idx],
                                            &span_x, &span_y));
          const double wanted_x = frame_width * zoom * device_scale;
          const double wanted_y = frame_height * zoom * device_scale;
          if(fabs(span_x - wanted_x) > 2.0 || fabs(span_y - wanted_y) > 2.0)
            print_error("zoom %.2f quality %.2f scale %d phase %.3f: %.0f x %.0f painted of %.1f x %.1f "
                        "(%.1f%% x %.1f%%)\n",
                        zoom, quality, device_scale, phases[phase_idx], span_x, span_y, wanted_x, wanted_y,
                        100.0 * span_x / wanted_x, 100.0 * span_y / wanted_y);
          assert_true(fabs(span_x - wanted_x) <= 2.0);
          assert_true(fabs(span_y - wanted_y) <= 2.0);
        }

  dt_canvas_free(canvas);
  g_remove(path);
  g_free(path);
}

static void _a_gesture_costs_the_page_nothing_but_its_pictures(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  assert_non_null(canvas);
  dt_canvas_object_t *text = _zoom_invariant_column(canvas);
  assert_non_null(text);
  assert_true(dt_canvas_paint_text_fit_height(canvas, text));

  const int width = 480;
  const int height = 420;
  cairo_surface_t *sharp = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
  cairo_surface_t *reduced = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
  for(int pass = 0; pass < 2; pass++)
  {
    cairo_surface_t *surface = pass == 0 ? sharp : reduced;
    cairo_t *cr = cairo_create(surface);
    cairo_translate(cr, width * 0.5, height * 0.5);
    cairo_scale(cr, 0.618, 0.618);
    const dt_canvas_rect_t visible = { -400.0, -400.0, 800.0, 800.0 };
    // A cache of its own per pass, so neither the composite nor a sprite can carry over.
    dt_canvas_surface_cache_t *cache = dt_canvas_surface_cache_new(FALSE, 64u * 1024u * 1024u);
    dt_canvas_paint_options_t options = dt_canvas_paint_options_export(cache, 1.0 / 0.618, visible);
    options.quality = pass == 0 ? 1.0 : 0.5;
    dt_canvas_paint(cr, canvas, &options);
    cairo_destroy(cr);
    cairo_surface_flush(surface);
    dt_canvas_surface_cache_free(cache);
  }
  const uint8_t *a = cairo_image_surface_get_data(sharp);
  const uint8_t *b = cairo_image_surface_get_data(reduced);
  const int stride = cairo_image_surface_get_stride(sharp);
  int differing = 0;
  for(int row = 0; row < height && differing == 0; row++)
    for(int column = 0; column < width; column++)
      if(memcmp(a + (size_t)row * stride + (size_t)column * 4, b + (size_t)row * stride + (size_t)column * 4, 3) != 0)
      {
        print_error("row %d column %d differs between quality 1.0 and 0.5 on a canvas holding no picture\n", row,
                    column);
        differing++;
        break;
      }
  cairo_surface_destroy(sharp);
  cairo_surface_destroy(reduced);
  dt_canvas_free(canvas);
  assert_int_equal(differing, 0);
}

static void _a_text_frame_paints_the_same_lines_at_every_zoom(void **state)
{
  (void)state;
  /*
   * The visible half of the same defect: of these twenty viewports, five painted twenty-one
   * lines and fifteen painted twenty-two, the last line -- "full ascent above the tallest of
   * them." -- present at rest and gone for the duration of any drag, because a gesture drops
   * the quality and so changes the matrix the metrics were rounded at. The lowest inked row is
   * what says which happened: a dropped line lifts it by a whole line, 13.97 units, where the
   * viewports that agree land it within one of their own pixels of each other.
   */
  dt_canvas_t *canvas = dt_canvas_new();
  assert_non_null(canvas);
  dt_canvas_object_t *text = _zoom_invariant_column(canvas);
  assert_non_null(text);
  assert_true(dt_canvas_paint_text_fit_height(canvas, text));

  const double zooms[] = { 0.37, 0.5, 1.0, 2.0, 3.1 };
  double reference = 0.0;
  gboolean have_reference = FALSE;
  for(size_t idx = 0; idx < sizeof(zooms) / sizeof(zooms[0]); idx++)
  {
    for(int device_scale = 1; device_scale <= 2; device_scale++)
    {
      for(int reduced = 0; reduced <= 1; reduced++)
      {
        const double quality = reduced ? 0.5 : 1.0;
        double bottom = 0.0;
        assert_true(_painted_ink_bottom(canvas, text, zooms[idx], (double)device_scale, quality, &bottom));
        if(!have_reference)
        {
          reference = bottom;
          have_reference = TRUE;
        }
        /*
         * Measured with the last line dropped at some viewports and not at others: the twenty-
         * one-line readings came to -62.16 and -60.81 units, the twenty-two-line ones spread
         * from -51.35 to -49.03 -- 9.5 units between the two groups against 2.3 within the
         * larger one, the spread being what a stem rasterised across a fifth of a pixel does to
         * the threshold above rather than anything the flow engine decided. Five units sits
         * between the two with room either side.
         */
        /*
         * And an absolute anchor, or a change that dropped the last line at EVERY viewport
         * would pass: the lowest type must sit within two lines of the frame's own inner
         * bottom edge, which is where a frame fitted to its text puts it.
         */
        const double inner_bottom = text->y + text->height * 0.5 - (double)text->text.padding;
        if(bottom < inner_bottom - 2.0 * 13.97)
        {
          print_error("zoom %.2f x device scale %d x quality %.1f: the lowest type sits at %.3f units, more "
                      "than two lines above the frame's inner bottom edge at %.3f\n",
                      zooms[idx], device_scale, quality, bottom, inner_bottom);
          fail();
        }
        if(fabs(bottom - reference) > 5.0)
        {
          print_error("zoom %.2f x device scale %d x quality %.1f: the lowest type sits at %.3f units "
                      "against %.3f at the first viewport -- %.3f units apart, most of a line\n",
                      zooms[idx], device_scale, quality, bottom, reference, fabs(bottom - reference));
          fail();
        }
      }
    }
  }
  dt_canvas_free(canvas);
}

static void _a_text_frame_short_of_its_text_is_brought_up_to_it(void **state)
{
  (void)state;
  /*
   * The fit is asked to make a frame as tall as its text, and the cut that decides which lines
   * fit is EXACT -- deliberately, since a tolerance there would let a line overflow a frame the
   * user sized by hand. So a frame five THOUSANDTHS of a unit short of its text already paints
   * one line fewer, and the fit, finding itself inside its own settle tolerance, used to break
   * before assigning and report that there was nothing to do. Measured: 22 lines at the natural
   * height, 21 at five thousandths under it, and the fit left it there.
   *
   * A frame arrives in that state whenever a height is written by something other than this
   * loop -- typed into the card, read back from a document written before the metrics were
   * quantised, or scaled by a drag.
   */
  dt_canvas_t *canvas = dt_canvas_new();
  assert_non_null(canvas);
  dt_canvas_object_t *text = _zoom_invariant_column(canvas);
  assert_non_null(text);
  assert_true(dt_canvas_paint_text_fit_height(canvas, text));
  const double settled = text->height;
  const double top = text->y - text->height * 0.5;
  double whole = 0.0;
  assert_true(_painted_ink_bottom(canvas, text, 1.0, 1.0, 1.0, &whole));

  text->height = settled - 0.005;
  text->y -= 0.0025;
  dt_canvas_touch(canvas);
  double clipped = 0.0;
  assert_true(_painted_ink_bottom(canvas, text, 1.0, 1.0, 1.0, &clipped));
  // The shortfall really does cost a line: a tenth of a line apart would prove nothing.
  assert_true(whole - clipped > 5.0);

  assert_true(dt_canvas_paint_text_fit_height(canvas, text));
  if(text->height != settled)
  {
    print_error("the fit left the frame at %.6f where its text comes to %.6f\n", text->height, settled);
    fail();
  }
  /*
   * Grown downward, as it always is: the edge the user placed is not the fit's to move. The
   * tolerance has to be far below what the correction moves, or the check passes whether or
   * not the correction is there: this shortfall is five thousandths of a unit, so the line
   * that moves the top edge contributes half of that, and a hundredth of a unit would be four
   * times the whole quantity under test.
   */
  if(fabs(top - (text->y - text->height * 0.5)) > 1.0e-9)
  {
    print_error("the fit moved the top edge from %.9f to %.9f\n", top, text->y - text->height * 0.5);
    fail();
  }
  dt_canvas_touch(canvas);
  double restored = 0.0;
  assert_true(_painted_ink_bottom(canvas, text, 1.0, 1.0, 1.0, &restored));
  assert_float_equal(whole, restored, 0.01);
  dt_canvas_free(canvas);
}

static void _the_first_line_is_placed_against_a_whole_line_of_the_obstacle_map(void **state)
{
  (void)state;
  // The band a line is placed against must be a line tall. Before there is a line to measure
  // it came out as nothing, so the first line was placed against a sliver of the map: an
  // obstacle whose top edge sits just below the text's own was invisible to it, and the line
  // was set at the full measure and drawn straight under the shape. Both offsets are well
  // inside one line's height, so the two must lay out identically.
  const double flush = _flowed_height_with_obstacle_below_the_top(0.0);
  const double lowered = _flowed_height_with_obstacle_below_the_top(20.0);
  assert_true(flush > 0.0);
  assert_true(lowered > 0.0);
  assert_float_equal(flush, lowered, 0.01);
}

static void _a_word_too_long_for_every_stretch_is_still_set(void **state)
{
  (void)state;
  /*
   * The flowing walk may REFUSE a stretch a piece does not fit -- that is what stops a word from
   * being drawn across the picture beside it. A word wider than the widest stretch there is has
   * nowhere better to go, so refusing it everywhere would drop it, and with it every word after
   * it: the walk consumes nothing and the frame comes out empty. It must be set instead, and
   * overflow, which is what PANGO_WRAP_WORD means.
   */
  dt_canvas_t *canvas = dt_canvas_new();
  assert_non_null(canvas);
  dt_canvas_object_t *text = dt_canvas_add_text(canvas, 200.0, 150.0, 360.0, 260.0,
                                                "Court mot. "
                                                "Anticonstitutionnellementgrandissimeissimementissimearchisuperlongissimement "
                                                "et voici la suite du paragraphe qui doit rester.");
  assert_non_null(text);
  dt_canvas_object_t *over = dt_canvas_add_text(canvas, 300.0, 150.0, 140.0, 120.0, "");
  assert_non_null(over);
  text->text.text_flags |= DT_CANVAS_TEXT_WRAP_AROUND;
  text->text.wrap_standoff = 8.0f;

  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
  cairo_t *cr = cairo_create(surface);
  const double height = dt_canvas_paint_text_natural_height(cr, canvas, text);
  cairo_destroy(cr);
  cairo_surface_destroy(surface);
  const double line = 12.0 * 1.1667;

  /*
   * Judged against the SAME paragraph with the long word taken out: the word is one line, so a
   * frame that set it owes about one line more. A frame that dropped it instead consumes
   * nothing, asks the same question one line lower and burns the 4096-line cap -- measured at
   * 4133 units against the 66 owed, which is what the upper bound catches.
   */
  dt_canvas_t *control = dt_canvas_new();
  assert_non_null(control);
  dt_canvas_object_t *short_text
      = dt_canvas_add_text(control, 200.0, 150.0, 360.0, 260.0,
                           "Court mot. et voici la suite du paragraphe qui doit rester.");
  assert_non_null(short_text);
  dt_canvas_object_t *control_over = dt_canvas_add_text(control, 300.0, 150.0, 140.0, 120.0, "");
  assert_non_null(control_over);
  short_text->text.text_flags |= DT_CANVAS_TEXT_WRAP_AROUND;
  short_text->text.wrap_standoff = 8.0f;
  surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
  cr = cairo_create(surface);
  const double without = dt_canvas_paint_text_natural_height(cr, control, short_text);
  cairo_destroy(cr);
  cairo_surface_destroy(surface);
  dt_canvas_free(control);

  if(height > without + 4.0 * line)
    print_error("the long word cost %.1f units over the %.1f the rest of the paragraph owes\n", height - without,
                without);
  assert_true(height > without);
  assert_true(height <= without + 4.0 * line);
  dt_canvas_free(canvas);
}

static void _text_flows_around_what_is_laid_over_it(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  assert_non_null(canvas);
  dt_canvas_object_t *text = dt_canvas_add_text(
      canvas, 200.0, 150.0, 360.0, 260.0,
      "Typography on an infinite plane demands that a paragraph break its lines the same way whatever the "
      "zoom, because the page is the thing being designed and the screen is only a window onto it.");
  assert_non_null(text);
  // Later in draw order, so it is laid OVER the text and pushes it.
  dt_canvas_object_t *over = dt_canvas_add_text(canvas, 300.0, 150.0, 140.0, 120.0, "");
  assert_non_null(over);

  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
  cairo_t *cr = cairo_create(surface);
  const double plain = dt_canvas_paint_text_natural_height(cr, canvas, text);
  text->text.text_flags |= DT_CANVAS_TEXT_WRAP_AROUND;
  text->text.wrap_standoff = 8.0f;
  const double flowed = dt_canvas_paint_text_natural_height(cr, canvas, text);
  // The gap the text leaves around what it avoids is the user's to set: widening it takes more
  // width off every line beside the obstacle, so the paragraph grows.
  text->text.wrap_standoff = 48.0f;
  const double roomier = dt_canvas_paint_text_natural_height(cr, canvas, text);
  cairo_destroy(cr);
  cairo_surface_destroy(surface);

  assert_true(plain > 0.0);
  assert_true(flowed > plain);
  assert_true(roomier > flowed);
  dt_canvas_free(canvas);
}

/* --- drawn shapes -------------------------------------------------------------------------- */

/** The whole canvas painted into a square surface at one pixel per unit; the caller destroys it. */
static cairo_surface_t *_painted_surface(const dt_canvas_t *canvas, const int size, const gboolean placeholders)
{
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, size, size);
  cairo_t *cr = cairo_create(surface);
  cairo_translate(cr, size * 0.5, size * 0.5);
  const dt_canvas_rect_t whole = { -size * 0.5, -size * 0.5, (double)size, (double)size };
  dt_canvas_paint_options_t options = dt_canvas_paint_options_export(NULL, 1.0, whole);
  options.draw_placeholders = placeholders;
  dt_canvas_paint(cr, canvas, &options);
  cairo_destroy(cr);
  cairo_surface_flush(surface);
  return surface;
}

/** How many pixels of two equally sized surfaces differ at all. */
static int _pixels_differing(cairo_surface_t *first, cairo_surface_t *second, const int size)
{
  const uint8_t *left = cairo_image_surface_get_data(first);
  const uint8_t *right = cairo_image_surface_get_data(second);
  const int left_stride = cairo_image_surface_get_stride(first);
  const int right_stride = cairo_image_surface_get_stride(second);
  int differing = 0;
  for(int row = 0; row < size; row++)
    for(int column = 0; column < size; column++)
    {
      const uint32_t a = *(const uint32_t *)(left + (size_t)row * left_stride + (size_t)column * 4) & 0xFFFFFFu;
      const uint32_t b = *(const uint32_t *)(right + (size_t)row * right_stride + (size_t)column * 4) & 0xFFFFFFu;
      if(a != b) differing++;
    }
  return differing;
}

/** A canvas with nothing on it but the plane: what every shape test below starts from. */
static dt_canvas_t *_bare_canvas(void)
{
  dt_canvas_t *canvas = dt_canvas_new();
  canvas->background = dt_canvas_color(0.0f, 0.0f, 0.0f, 1.0f);
  canvas->grid_flags = 0;
  canvas->paper_size = DT_CANVAS_PAPER_NONE;
  canvas->border_width = 0.0f;
  return canvas;
}

/**
 * A filled rectangle is exactly what a picture frame with no picture paints, to the pixel: the
 * border and the colour inside it come from ONE function, so a rectangle can never drift from the
 * ground every frame stands on. And no placeholder: a rectangle is waiting for nothing.
 */
static void _a_filled_rectangle_paints_a_frames_own_ground(void **state)
{
  (void)state;
  dt_canvas_t *with_image = _bare_canvas();
  dt_canvas_object_t *image = dt_canvas_add_image(with_image, 0.0, 0.0, 100, 100);
  image->width = 120.0;
  image->height = 80.0;
  image->background = dt_canvas_color(0.2f, 0.6f, 0.9f, 1.0f);
  image->border_width = 8.0f;
  image->border_color = dt_canvas_color(1.0f, 0.4f, 0.0f, 1.0f);
  image->flags |= DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE;
  image->corner_radius = 14.0f;
  image->flags |= DT_CANVAS_OBJECT_FLAG_CORNER_OVERRIDE;

  dt_canvas_t *with_shape = _bare_canvas();
  dt_canvas_shape_style_t style = dt_canvas_shape_style_default();
  style.fill = image->background;
  style.border_override = TRUE;
  style.border_width = image->border_width;
  style.border_color = image->border_color;
  style.corner_override = TRUE;
  style.corner_radius = image->corner_radius;
  const dt_canvas_rect_t box = { -60.0, -40.0, 120.0, 80.0 };
  dt_canvas_object_t *shape = dt_canvas_add_shape(with_shape, DT_CANVAS_SHAPE_RECTANGLE, &box, &style);
  assert_non_null(shape);

  cairo_surface_t *painted_image = _painted_surface(with_image, 200, FALSE);
  cairo_surface_t *painted_shape = _painted_surface(with_shape, 200, FALSE);
  assert_int_equal(_pixels_differing(painted_image, painted_shape, 200), 0);
  cairo_surface_destroy(painted_image);
  cairo_surface_destroy(painted_shape);

  // With placeholders on, the picture waiting for its render grows the grey cross; the shape does
  // not move a pixel, because it is not waiting for anything.
  cairo_surface_t *plain_shape = _painted_surface(with_shape, 200, FALSE);
  cairo_surface_t *marked_shape = _painted_surface(with_shape, 200, TRUE);
  cairo_surface_t *marked_image = _painted_surface(with_image, 200, TRUE);
  assert_int_equal(_pixels_differing(plain_shape, marked_shape, 200), 0);
  assert_true(_pixels_differing(marked_image, marked_shape, 200) > 100);
  cairo_surface_destroy(plain_shape);
  cairo_surface_destroy(marked_shape);
  cairo_surface_destroy(marked_image);
  dt_canvas_free(with_image);
  dt_canvas_free(with_shape);
}

/** A shape is a frame, so its cutout and the band around it are the frame machinery's, unchanged. */
static void _a_cut_rectangle_paints_its_cutout_and_its_band(void **state)
{
  (void)state;
  dt_canvas_t *canvas = _bare_canvas();
  const dt_canvas_rect_t box = { -50.0, -50.0, 100.0, 100.0 };
  dt_canvas_shape_style_t style = dt_canvas_shape_style_default();
  style.fill = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
  style.border_override = TRUE;
  style.border_width = 10.0f;
  style.border_color = dt_canvas_color(1.0f, 0.0f, 0.0f, 1.0f);
  dt_canvas_object_t *shape = dt_canvas_add_shape(canvas, DT_CANVAS_SHAPE_RECTANGLE, &box, &style);
  assert_non_null(shape);
  const uint32_t red = _layer_code(1.0, 0.0, 0.0);
  // Rectangular: the border sits inside the edge, the fill within it.
  assert_int_equal(_painted_pixel(canvas, 200, 100, 100), 0xFFFFFFu);
  assert_true(_within(_painted_pixel(canvas, 200, 53, 100), red, 1));
  assert_int_equal(_painted_pixel(canvas, 200, 47, 100), 0x000000u);
  // Cut to an ellipse: the fill fills the ellipse, the band follows its edge outward, and past
  // the band there is nothing but the plane -- the corners of the frame included.
  dt_canvas_mask_set_shape(canvas, shape, DT_CANVAS_MASK_ELLIPSE);
  shape->mask.radius_x = 0.4f;
  shape->mask.radius_y = 0.2f;
  shape->mask.feather = 0.0f;
  assert_int_equal(_painted_pixel(canvas, 200, 100, 100), 0xFFFFFFu);
  assert_int_equal(_painted_pixel(canvas, 200, 130, 100), 0xFFFFFFu);
  assert_true(_within(_painted_pixel(canvas, 200, 143, 100), red, 1));
  assert_true(_within(_painted_pixel(canvas, 200, 100, 124), red, 1));
  assert_int_equal(_painted_pixel(canvas, 200, 55, 55), 0x000000u);
  dt_canvas_free(canvas);
}

/**
 * An outline box laid over a photograph must not take the clicks meant for the picture: a shape
 * with no fill is picked by the BAND it paints, and there is nothing of it in the middle.
 */
static void _an_unfilled_rectangle_is_picked_by_its_band(void **state)
{
  (void)state;
  dt_canvas_t *canvas = _bare_canvas();
  dt_canvas_shape_style_t style = dt_canvas_shape_style_default();
  style.fill.alpha = 0.0f;
  style.border_override = TRUE;
  style.border_width = 10.0f;
  style.border_color = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
  const dt_canvas_rect_t box = { -100.0, -100.0, 200.0, 200.0 };
  dt_canvas_object_t *shape = dt_canvas_add_shape(canvas, DT_CANVAS_SHAPE_RECTANGLE, &box, &style);
  assert_non_null(shape);
  assert_false(dt_canvas_object_contains(canvas, shape, 0.0, 0.0, 0.0));
  assert_true(dt_canvas_object_contains(canvas, shape, 95.0, 0.0, 0.0));
  assert_true(dt_canvas_object_contains(canvas, shape, 0.0, -95.0, 0.0));
  // The outline itself answers whatever the fill is, and a tolerance reaches over the edge.
  assert_true(dt_canvas_object_contains(canvas, shape, 100.0, 0.0, 0.0));
  assert_true(dt_canvas_object_contains(canvas, shape, 104.0, 0.0, 5.0));
  assert_false(dt_canvas_object_contains(canvas, shape, 120.0, 0.0, 5.0));
  // Just inside the band's inner edge is no longer the shape: that is the picture's own click.
  assert_false(dt_canvas_object_contains(canvas, shape, 80.0, 0.0, 0.0));

  // A border painted in nothing paints no band, so it takes no click either: the painter and the
  // coverage the text reads both ask the colour's strength, and the hit test must ask the same
  // question or a shape with nothing at all on screen swallows the picture's clicks.
  shape->border_color.alpha = 0.0f;
  assert_false(dt_canvas_object_contains(canvas, shape, 95.0, 0.0, 0.0));
  assert_false(dt_canvas_object_contains(canvas, shape, 0.0, 0.0, 0.0));
  // Its outline still answers, so it is never left with nothing to take hold of.
  assert_true(dt_canvas_object_contains(canvas, shape, 100.0, 0.0, 0.0));
  shape->border_color.alpha = 1.0f;

  // Filled, the whole box is the shape's again.
  shape->background.alpha = 1.0f;
  assert_true(dt_canvas_object_contains(canvas, shape, 0.0, 0.0, 0.0));
  assert_true(dt_canvas_object_contains(canvas, shape, 80.0, 0.0, 0.0));
  dt_canvas_free(canvas);
}

/** The natural height of a text frame with `shape` laid over its middle, or none. */
static double _flowed_height_under_a_shape(const gboolean with_shape, const gboolean filled)
{
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *text = dt_canvas_add_text(
      canvas, 0.0, 0.0, 400.0, 400.0,
      "Typography on an infinite plane demands that a paragraph break its lines the same way whatever the "
      "zoom, because the page is the thing being designed and the screen is only a window onto it, and "
      "the measure a line is set to belongs to the page rather than to the window looking at it.");
  text->text.text_flags |= DT_CANVAS_TEXT_WRAP_AROUND;
  text->text.wrap_standoff = 0.0f;
  if(with_shape)
  {
    dt_canvas_shape_style_t style = dt_canvas_shape_style_default();
    style.fill.alpha = filled ? 1.0f : 0.0f;
    style.border_override = TRUE;
    style.border_width = 4.0f;
    style.border_color = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
    // Over the text from its very first line: an obstacle the column has not reached yet says
    // nothing about whether it would have to go round it.
    const dt_canvas_rect_t box = { -110.0, -190.0, 220.0, 220.0 };
    dt_canvas_add_shape(canvas, DT_CANVAS_SHAPE_RECTANGLE, &box, &style);
  }
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
  cairo_t *cr = cairo_create(surface);
  const double height = dt_canvas_paint_text_natural_height(cr, canvas, text);
  cairo_destroy(cr);
  cairo_surface_destroy(surface);
  dt_canvas_free(canvas);
  return height;
}

/**
 * Text set under an outline box runs THROUGH it: an obstacle covers where it puts ink, and an
 * unfilled shape puts none in its middle. Filled, the same box is a wall and the column has to go
 * round it, which is what tells the two apart rather than the box they share.
 */
static void _text_flows_inside_an_outline_box(void **state)
{
  (void)state;
  const double plain = _flowed_height_under_a_shape(FALSE, FALSE);
  const double outline = _flowed_height_under_a_shape(TRUE, FALSE);
  const double filled = _flowed_height_under_a_shape(TRUE, TRUE);
  assert_true(plain > 0.0);
  assert_true(filled > plain);
  assert_true(outline < filled);
  // The band is still an obstacle: the outline costs the column something, just not the hole.
  assert_true(outline >= plain);
}

/** A polygon of `sides` at `depth`, `width` across and at its own ratio, on a bare canvas. */
static dt_canvas_object_t *_painted_polygon(dt_canvas_t *canvas, const uint32_t sides, const float depth,
                                            const double width, const dt_canvas_shape_style_t *style)
{
  const double height = width / dt_canvas_shape_unit_aspect(sides, depth, 0.0f);
  dt_canvas_shape_style_t applied = IS_NULL_PTR(style) ? dt_canvas_shape_style_default() : *style;
  applied.sides = sides;
  applied.depth = depth;
  applied.roundness = 0.0f;
  const dt_canvas_rect_t box = { -0.5 * width, -0.5 * height, width, height };
  return dt_canvas_add_shape(canvas, DT_CANVAS_SHAPE_POLYGON, &box, &applied);
}

/**
 * A polygon's border is a BAND that REPLACES the fill it covers, not a colour laid over it: a
 * translucent border shows the plane through it, exactly as a rectangular frame's border does by
 * being stroked with the fill inset inside it. The proof is that the band reads the same over a
 * filled shape as over an empty one -- and reads nothing like the colour it would have been over
 * the fill, which is what says the two cases are telling us something.
 */
static void _a_polygons_band_replaces_its_fill_rather_than_lying_over_it(void **state)
{
  (void)state;
  dt_canvas_shape_style_t style = dt_canvas_shape_style_default();
  style.fill = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
  style.border_override = TRUE;
  style.border_width = 20.0f;
  style.border_color = dt_canvas_color(1.0f, 0.0f, 0.0f, 0.5f);

  dt_canvas_t *filled = _bare_canvas();
  dt_canvas_object_t *hexagon = _painted_polygon(filled, 6, 0.0f, 160.0, &style);
  assert_non_null(hexagon);
  // The right side of a hexagon this wide is a vertical edge at 80 units out; the band runs from
  // there 20 units in, so its middle is at 70 and the fill's own ground is at the centre.
  const uint32_t in_band = _painted_pixel(filled, 200, 170, 100);
  const uint32_t at_centre = _painted_pixel(filled, 200, 100, 100);
  assert_int_equal(at_centre, 0xFFFFFFu);
  // Outside the outline, inside the frame's box: the plane, and nothing of the shape.
  assert_int_equal(_painted_pixel(filled, 200, 178, 190), 0x000000u);

  dt_canvas_t *empty = _bare_canvas();
  style.fill.alpha = 0.0f;
  dt_canvas_object_t *outline = _painted_polygon(empty, 6, 0.0f, 160.0, &style);
  assert_non_null(outline);
  const uint32_t in_band_unfilled = _painted_pixel(empty, 200, 170, 100);
  assert_int_equal(_painted_pixel(empty, 200, 100, 100), 0x000000u);
  fprintf(stderr, "polygon band: %06x over a filled shape, %06x over an empty one, %06x had it lain over\n",
          in_band, in_band_unfilled, _layer_code(1.0, 0.5, 0.5));
  // The band is the border over the PAPER either way, to a code.
  assert_true(_within(in_band, in_band_unfilled, 1));
  // And not the border over the white fill, which is what an OVER would have given.
  assert_false(_within(in_band, _layer_code(1.0, 0.5, 0.5), 8));
  dt_canvas_free(filled);
  dt_canvas_free(empty);
}

/**
 * The band is painted with SOURCE, which REPLACES whatever it is laid on rather than blending into
 * it -- so it is painted inside a group of its own, bounded by the frame. `dt_canvas_paint_object()`
 * hands the painter the caller's own surface, and without the group a translucent border would take
 * the plane, the paper and every object under it away with the fill it was only meant to cover.
 *
 * The group is pushed for a BAND and for nothing else: a fill is laid with OVER and isolating it
 * changes not one byte (measured, on a transparent destination and on a filled one, at three fill
 * opacities), while costing a frame-sized allocation, clear and composite -- 0.05 ms on a small
 * shape's layer and 0.77 ms on a large one. The fill-only shape is the common one, so this test
 * gives the star a border and no fill, which is the case that needs the group.
 */
static void _a_polygons_band_clears_nothing_outside_itself(void **state)
{
  (void)state;
  dt_canvas_t *canvas = _bare_canvas();
  dt_canvas_shape_style_t style = dt_canvas_shape_style_default();
  style.fill.alpha = 0.0f;
  style.border_override = TRUE;
  style.border_width = 12.0f;
  // TRANSLUCENT, which is what tells the two apart: an opaque band writes the same opaque pixels
  // whichever way it is composited, and says nothing about what it did to the surface beneath.
  style.border_color = dt_canvas_color(1.0f, 0.0f, 0.0f, 0.5f);
  dt_canvas_object_t *star = _painted_polygon(canvas, 5, DT_CANVAS_SHAPE_STAR_DEPTH, 160.0, &style);
  assert_non_null(star);

  // A surface already carrying something of its own, the way the compositor's does.
  const int size = 200;
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
  cairo_t *cr = cairo_create(surface);
  cairo_set_source_rgba(cr, 0.0, 0.0, 1.0, 1.0);
  cairo_paint(cr);
  cairo_translate(cr, size * 0.5, size * 0.5);
  const dt_canvas_rect_t whole = { -size * 0.5, -size * 0.5, (double)size, (double)size };
  const dt_canvas_paint_options_t options = dt_canvas_paint_options_export(NULL, 1.0, whole);
  dt_canvas_paint_object(cr, canvas, star, &options);
  cairo_destroy(cr);
  cairo_surface_flush(surface);
  const uint8_t *pixels = cairo_image_surface_get_data(surface);
  const int stride = cairo_image_surface_get_stride(surface);

  int cleared = 0;
  int painted = 0;
  for(int row = 0; row < size; row++)
  {
    for(int col = 0; col < size; col++)
    {
      const uint32_t pixel = *(const uint32_t *)(pixels + (size_t)row * stride + (size_t)col * 4);
      const uint32_t alpha = pixel >> 24;
      if(alpha < 250)
        cleared++;
      else if((pixel & 0xFFFFFFu) != 0x0000FFu)
        painted++;
    }
  }
  fprintf(stderr, "a star painted onto a filled surface: %d pixels painted, %d cleared\n", painted, cleared);
  // The band painted plenty, and took nothing away: every pixel is still opaque.
  assert_true(painted > 1000);
  assert_int_equal(cleared, 0);
  cairo_surface_destroy(surface);
  dt_canvas_free(canvas);
}

/**
 * Nothing in the atelier cuts a polygon, so a mask left on one by a hand-edited file is ignored by
 * the painter too: the shape it draws is the shape every other reader of it reports.
 */
static void _a_polygon_with_a_stale_mask_paints_uncut(void **state)
{
  (void)state;
  dt_canvas_shape_style_t style = dt_canvas_shape_style_default();
  style.fill = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);

  dt_canvas_t *plain = _bare_canvas();
  assert_non_null(_painted_polygon(plain, 6, 0.0f, 160.0, &style));
  dt_canvas_t *masked = _bare_canvas();
  dt_canvas_object_t *stale = _painted_polygon(masked, 6, 0.0f, 160.0, &style);
  assert_non_null(stale);
  dt_canvas_mask_set_shape(masked, stale, DT_CANVAS_MASK_CIRCLE);
  stale->mask.radius_x = 0.2f;
  stale->mask.radius_y = 0.2f;

  cairo_surface_t *without = _painted_surface(plain, 200, FALSE);
  cairo_surface_t *with = _painted_surface(masked, 200, FALSE);
  assert_int_equal(_pixels_differing(without, with, 200), 0);
  cairo_surface_destroy(without);
  cairo_surface_destroy(with);
  dt_canvas_free(plain);
  dt_canvas_free(masked);
}

/** A column of text, added FIRST so that anything put on the canvas after it is laid OVER it. */
static dt_canvas_object_t *_flowing_column(dt_canvas_t *canvas)
{
  dt_canvas_object_t *text = dt_canvas_add_text(
      canvas, 0.0, 0.0, 400.0, 400.0,
      "Typography on an infinite plane demands that a paragraph break its lines the same way whatever the "
      "zoom, because the page is the thing being designed and the screen is only a window onto it, and "
      "the measure a line is set to belongs to the page rather than to the window looking at it.");
  text->text.text_flags |= DT_CANVAS_TEXT_WRAP_AROUND;
  text->text.wrap_standoff = 0.0f;
  return text;
}

/** The height that column needs, once everything on the canvas is where it is going to be. */
static double _flowed_height(dt_canvas_t *canvas, const dt_canvas_object_t *text)
{
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
  cairo_t *cr = cairo_create(surface);
  const double height = dt_canvas_paint_text_natural_height(cr, canvas, text);
  cairo_destroy(cr);
  cairo_surface_destroy(surface);
  return height;
}

/**
 * Text flows INTO a star's notches. An obstacle covers where it puts ink, and the ink of a star is
 * the star -- so a column set under one costs less height than the same column under the rectangle
 * the star stands in, which is the box a silhouette taken as a reach would have handed the text.
 */
static void _text_flows_into_a_stars_notches(void **state)
{
  (void)state;
  dt_canvas_shape_style_t style = dt_canvas_shape_style_default();
  style.fill = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
  const double width = 240.0;
  const double height = width / dt_canvas_shape_unit_aspect(5, DT_CANVAS_SHAPE_STAR_DEPTH, 0.0f);
  // Over the column from its very first line: an obstacle the text has not reached yet says nothing
  // about whether it would have had to go round it.
  const dt_canvas_rect_t box = { -0.5 * width, -190.0, width, height };

  dt_canvas_t *plain = dt_canvas_new();
  const double bare = _flowed_height(plain, _flowing_column(plain));

  dt_canvas_t *with_star = dt_canvas_new();
  const dt_canvas_object_t *star_column = _flowing_column(with_star);
  dt_canvas_shape_style_t star_style = style;
  star_style.sides = 5;
  star_style.depth = DT_CANVAS_SHAPE_STAR_DEPTH;
  assert_non_null(dt_canvas_add_shape(with_star, DT_CANVAS_SHAPE_POLYGON, &box, &star_style));
  const double starred = _flowed_height(with_star, star_column);

  dt_canvas_t *with_box = dt_canvas_new();
  const dt_canvas_object_t *box_column = _flowing_column(with_box);
  assert_non_null(dt_canvas_add_shape(with_box, DT_CANVAS_SHAPE_RECTANGLE, &box, &style));
  const double boxed = _flowed_height(with_box, box_column);

  // An UNFILLED star is a hole with a rule round it: its middle is free, and a line set across it
  // is broken into the runs either side of the band rather than turned away altogether. Only a
  // raster of what the shape PAINTS can say that -- a ray out of the frame's centre, which is all a
  // silhouette reach is, reports the same solid star for both. So it is this reading, and not the
  // filled star above, that says the shape's own ink is what the text was given.
  dt_canvas_t *with_outline = dt_canvas_new();
  const dt_canvas_object_t *outline_column = _flowing_column(with_outline);
  dt_canvas_shape_style_t outline_style = star_style;
  outline_style.fill.alpha = 0.0f;
  outline_style.border_override = TRUE;
  outline_style.border_width = 4.0f;
  outline_style.border_color = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
  assert_non_null(dt_canvas_add_shape(with_outline, DT_CANVAS_SHAPE_POLYGON, &box, &outline_style));
  const double outlined = _flowed_height(with_outline, outline_column);

  fprintf(stderr, "text under a star: %.1f bare, %.1f under the star, %.1f under its outline, %.1f under its box\n",
          bare, starred, outlined, boxed);
  assert_true(bare > 0.0);
  assert_true(starred > bare);
  assert_true(starred < boxed);
  assert_true(outlined < boxed);
  assert_true(outlined > bare);
  assert_true(fabs(outlined - starred) > 2.0);
  dt_canvas_free(plain);
  dt_canvas_free(with_star);
  dt_canvas_free(with_box);
  dt_canvas_free(with_outline);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_a_filled_rectangle_paints_a_frames_own_ground),
    cmocka_unit_test(_a_cut_rectangle_paints_its_cutout_and_its_band),
    cmocka_unit_test(_an_unfilled_rectangle_is_picked_by_its_band),
    cmocka_unit_test(_text_flows_inside_an_outline_box),
    cmocka_unit_test(_a_polygons_band_replaces_its_fill_rather_than_lying_over_it),
    cmocka_unit_test(_a_polygons_band_clears_nothing_outside_itself),
    cmocka_unit_test(_a_polygon_with_a_stale_mask_paints_uncut),
    cmocka_unit_test(_text_flows_into_a_stars_notches),
    cmocka_unit_test(_a_circle_is_full_inside_empty_outside_and_feathers_between),
    cmocka_unit_test(_a_polygon_fills_its_interior),
    cmocka_unit_test(_a_polygon_node_carries_its_own_fall_off),
    cmocka_unit_test(_a_polygon_node_steers_its_own_curve),
    cmocka_unit_test(_text_flows_around_what_is_laid_over_it),
    cmocka_unit_test(_a_word_too_long_for_every_stretch_is_still_set),
    cmocka_unit_test(_the_first_line_is_placed_against_a_whole_line_of_the_obstacle_map),
    cmocka_unit_test(_an_auto_height_frame_grows_downward_and_settles),
    cmocka_unit_test(_a_text_frames_natural_height_is_the_same_at_every_zoom),
    cmocka_unit_test(_a_leaded_text_frames_natural_height_is_the_same_at_every_zoom),
    cmocka_unit_test(_a_fitted_text_frame_is_exactly_its_natural_height),
    cmocka_unit_test(_a_gesture_costs_the_page_nothing_but_its_pictures),
    cmocka_unit_test(_a_drawing_fills_its_frame_at_every_zoom),
    cmocka_unit_test(_a_drawing_slides_with_the_page_instead_of_crabbing_against_it),
    cmocka_unit_test(_a_text_casts_a_shadow_from_its_glyphs),
    cmocka_unit_test(_a_picture_keeps_fewer_sprites_than_a_drawing),
    cmocka_unit_test(_a_text_frame_paints_the_same_lines_at_every_zoom),
    cmocka_unit_test(_a_text_frame_short_of_its_text_is_brought_up_to_it),
    cmocka_unit_test(_text_keeps_off_what_an_obstacle_paints_not_just_its_silhouette),
    cmocka_unit_test(_a_feathered_cutout_covers_all_of_its_fade),
    cmocka_unit_test(_a_drawings_sprite_is_exactly_the_size_it_was_asked_for),
    cmocka_unit_test(_a_drawing_fills_its_box_and_sits_in_the_callers_air),
    cmocka_unit_test(_a_drawing_arrives_at_the_size_its_file_states),
    cmocka_unit_test(_a_drawing_is_rasterised_whole_and_then_brought_into_the_layer),
    cmocka_unit_test(_text_flows_around_what_a_drawing_draws_not_its_box),
    cmocka_unit_test(_a_drawing_travels_in_the_document),
    cmocka_unit_test(_a_drawing_keeps_the_paper_where_it_draws_nothing),
    cmocka_unit_test(_rescaling_a_sprite_keeps_the_alpha_it_had),
    cmocka_unit_test(_a_drawing_is_drawn_at_the_size_it_is_shown_at),
    cmocka_unit_test(_a_picture_and_a_drawing_keep_their_shape_unless_told_not_to),
    cmocka_unit_test(_a_point_of_type_is_a_unit_on_the_plane),
    cmocka_unit_test(_the_gap_around_an_obstacle_is_a_disc_not_a_square),
    cmocka_unit_test(_a_frame_standing_just_outside_a_column_still_pushes_its_text),
    cmocka_unit_test(_the_leading_reaches_a_flowing_paragraph_once_per_gap),
    cmocka_unit_test(_paragraphs_take_their_indent_and_their_space),
    cmocka_unit_test(_a_paragraph_break_survives_the_layout_being_rebuilt),
    cmocka_unit_test(_a_line_carries_on_past_a_picture_standing_in_the_column),
    cmocka_unit_test(_a_font_offers_the_features_it_actually_ships),
    cmocka_unit_test(_a_gradient_fades_across_its_line),
    cmocka_unit_test(_the_object_mask_surface_matches_the_raster),
    cmocka_unit_test(_the_compositor_blends_in_linear_light_and_round_trips_opaque_codes),
    cmocka_unit_test(_a_shadows_extent_carries_its_strength_past_a_wide_blur),
    cmocka_unit_test(_a_grown_glyph_shadow_outlines_the_letters),
    cmocka_unit_test(_a_glyph_shadow_cast_inside_stays_on_the_letters),
    cmocka_unit_test(_the_compositor_paints_the_surfaces_own_pixels_on_a_scaled_surface),
    cmocka_unit_test(_a_cut_frames_border_follows_the_cutout_outward),
    cmocka_unit_test(_an_edited_document_is_not_served_from_the_last_frame),
    cmocka_unit_test(_a_cutout_keeps_its_side_through_the_raster_cache),
    cmocka_unit_test(_an_edge_pixel_stays_between_the_colours_it_blends),
    cmocka_unit_test(_a_picture_reaches_its_frames_every_edge),
    cmocka_unit_test(_a_cutouts_edge_is_anti_aliased_at_every_quality),
    cmocka_unit_test(_a_background_fills_the_frame_under_a_missing_render),
    cmocka_unit_test(_rounded_corners_round_the_frame_and_its_border),
  };
  return cmocka_run_group_tests(tests, _group_setup, _group_teardown);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
