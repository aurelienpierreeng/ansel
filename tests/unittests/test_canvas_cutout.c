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

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_a_circle_is_full_inside_empty_outside_and_feathers_between),
    cmocka_unit_test(_a_polygon_fills_its_interior),
    cmocka_unit_test(_a_polygon_node_carries_its_own_fall_off),
    cmocka_unit_test(_a_polygon_node_steers_its_own_curve),
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
