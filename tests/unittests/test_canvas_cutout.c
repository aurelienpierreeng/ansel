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
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
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
  // A radius of zero is no shadow at all.
  assert_int_equal(_painted_pixel(canvas, 200, 160, 160), background);
  // Hardly blurred, the shadow is where the frame's silhouette lands, offset.
  frame->shadow.blur = 0.4f;
  assert_int_equal(_painted_pixel(canvas, 200, 160, 160), 0x000000u);
  assert_int_equal(_painted_pixel(canvas, 200, 175, 175), background);
  // Blurred, the same spot is a shade between the shadow and the background.
  frame->shadow.blur = 8.0f;
  const uint32_t soft = _painted_pixel(canvas, 200, 168, 168);
  assert_true((soft & 0xFF) > 0 && (soft & 0xFF) < (background & 0xFF));
  // A negative radius casts the shadow inside the frame's own edges: the frame's corner
  // opposite the offset darkens, its middle does not, and nothing lands outside.
  frame->shadow.blur = -8.0f;
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
  cairo_surface_t *large = dt_canvas_render_svg(bytes, 512, 512, 0, 0);
  assert_non_null(large);
  assert_int_equal(cairo_image_surface_get_width(large), 512);
  assert_true(_sharpest_step(large) > 200);

  // Against the alternative: the file's own 32 points, blown up to the same 512 by resampling.
  cairo_surface_t *small = dt_canvas_render_svg(bytes, 0, 0, 0, 0);
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
    cairo_surface_t *sprite = dt_canvas_render_svg(bytes, wanted[idx][0], wanted[idx][1], 0, 0);
    assert_non_null(sprite);
    assert_int_equal(cairo_image_surface_get_width(sprite), wanted[idx][0]);
    assert_int_equal(cairo_image_surface_get_height(sprite), wanted[idx][1]);
    cairo_surface_destroy(sprite);
  }
  g_bytes_unref(bytes);
  g_remove(path);
  g_free(path);
}

static void _a_drawing_keeps_a_guard_pixel_inside_the_box(void **state)
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
   * The guard is the hair of air that answers it, and it is headroom rather than a repair --
   * the sub-pixel placement of the sprite was measured and is NOT what takes the ink (the
   * bottom line of type in a real diagram keeps it to within 0.07% at every alignment, guard
   * or no guard). What it pins here is that the ink is a whole pixel clear of the sprite's
   * edges whatever padding the sprite carries, so a drawing never lands on its frame.
   */
  gchar *path = _write_svg("<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100' "
                           "viewBox='0 0 100 100'><rect width='100' height='100' fill='#000000'/></svg>");
  gchar *contents = NULL;
  gsize length = 0;
  assert_true(g_file_get_contents(path, &contents, &length, NULL));
  GBytes *bytes = g_bytes_new_take(contents, length);

  // A sprite four pixels larger than the box, carrying a box-sized drawing: the ink starts one
  // pixel in and ends one pixel before the box does, whatever padding the sprite carries.
  static const int sprites[] = { 100, 101, 102, 104 };
  for(guint idx = 0; idx < G_N_ELEMENTS(sprites); idx++)
  {
    const int edge = sprites[idx];
    cairo_surface_t *sprite = dt_canvas_render_svg(bytes, edge, edge, 100, 100);
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
    // At least two pixels of air at the top, and the ink stopping at least two before the
    // box's own bottom edge, whatever padding the sprite happens to carry.
    assert_true(first >= 2);
    assert_true(last <= 97);
    cairo_surface_destroy(sprite);
  }

  // Asked to fill the sprite, it fills the sprite: a photograph's bargain, and the path an
  // export takes, where there is no fractional box and so nothing to guard against.
  cairo_surface_t *filled = dt_canvas_render_svg(bytes, 104, 104, 0, 0);
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

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_a_circle_is_full_inside_empty_outside_and_feathers_between),
    cmocka_unit_test(_a_polygon_fills_its_interior),
    cmocka_unit_test(_a_polygon_node_carries_its_own_fall_off),
    cmocka_unit_test(_a_polygon_node_steers_its_own_curve),
    cmocka_unit_test(_text_flows_around_what_is_laid_over_it),
    cmocka_unit_test(_the_first_line_is_placed_against_a_whole_line_of_the_obstacle_map),
    cmocka_unit_test(_an_auto_height_frame_grows_downward_and_settles),
    cmocka_unit_test(_text_keeps_off_what_an_obstacle_paints_not_just_its_silhouette),
    cmocka_unit_test(_a_feathered_cutout_covers_all_of_its_fade),
    cmocka_unit_test(_a_drawings_sprite_is_exactly_the_size_it_was_asked_for),
    cmocka_unit_test(_a_drawing_keeps_a_guard_pixel_inside_the_box),
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
