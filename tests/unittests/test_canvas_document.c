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

#include "canvas/canvas.h"
#include "math/polygon_envelope.h"
#include "system/mem_alloc.h"
#include "canvas/canvas_format.h"
#include "canvas/canvas_paint.h"

#include <cairo.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <setjmp.h>  // NOLINT(misc-include-cleaner)
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <cmocka.h>

static dt_canvas_t *_populated_canvas(void)
{
  dt_canvas_t *canvas = dt_canvas_new();
  g_strlcpy(canvas->title, "Exhibition 2026", sizeof(canvas->title));
  canvas->grid_size = 25.0f;
  canvas->grid_flags = DT_CANVAS_GRID_VISIBLE | DT_CANVAS_GRID_SNAP;
  canvas->border_width = 3.0f;
  canvas->border_color = dt_canvas_color(1.0f, 0.5f, 0.25f, 1.0f);
  canvas->padding = 35.0f;
  canvas->background_style = DT_CANVAS_BACKGROUND_WATERCOLOUR;
  canvas->grid_color = dt_canvas_color(0.1f, 0.2f, 0.3f, 0.4f);
  canvas->paper_size = DT_CANVAS_PAPER_A4;
  canvas->paper_landscape = 1;
  canvas->reserved[7] = 0xAB;

  dt_canvas_object_t *image = dt_canvas_add_image(canvas, 100.0, 200.0, 6000, 4000);
  image->image.imgid = 42;
  image->image.version = 1;
  image->image.film_id = 7;
  g_strlcpy(image->image.folder, "/home/someone/Pictures/2026", sizeof(image->image.folder));
  g_strlcpy(image->image.filename, "DSC_0001.NEF", sizeof(image->image.filename));
  g_strlcpy(image->image.exif_maker, "Nikon", sizeof(image->image.exif_maker));
  image->image.exif_iso = 400.0f;
  image->image.exif_datetime_taken = 1700000000000000LL;
  image->rotation = 0.25;
  image->flags = DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE;
  image->border_width = 9.0f;
  const char jpeg_stand_in[] = "\xff\xd8not really a jpeg\xff\xd9";
  GBytes *jpeg = g_bytes_new(jpeg_stand_in, sizeof(jpeg_stand_in));
  dt_canvas_image_set_render(canvas, image, jpeg, 2048, 1365, 0x1234567890ABCDEFULL, 1725000000LL,
                             DT_CANVAS_COLORSPACE_ADOBERGB);
  g_bytes_unref(jpeg);

  dt_canvas_object_t *text = dt_canvas_add_text(canvas, -300.0, 50.0, 400.0, 150.0, "# Title\n\nSome *emphasis*.");
  g_strlcpy(text->text.font, "Serif Bold 14", sizeof(text->text.font));
  text->text.align_h = DT_CANVAS_ALIGN_JUSTIFY;
  text->text.align_v = DT_CANVAS_ALIGN_END;
  text->text.background.alpha = 0.0f;
  text->text.first_line_indent = -18.0f;  // a hanging indent, so the sign survives too
  text->text.paragraph_spacing = 24.0f;
  text->text.shadow.color = dt_canvas_color(0.0f, 0.0f, 0.0f, 0.7f);
  text->text.shadow.blur = 1.5f;
  text->text.shadow.extent = 1.75f;

  dt_canvas_object_t *connector = dt_canvas_add_connector(canvas, text->id, image->id);
  assert_non_null(connector);
  connector->connector.from_anchor = DT_CANVAS_ANCHOR_SOUTH;
  connector->connector.to_anchor = DT_CANVAS_ANCHOR_WEST;
  connector->connector.routing = DT_CANVAS_ROUTING_CUBIC;
  connector->connector.via_count = 1;
  connector->connector.via_x = -120.0;
  connector->connector.via_y = 300.0;
  connector->connector.from_reach = 77.0f;
  connector->connector.via_tangent_x = 40.0;
  connector->connector.via_tangent_y = -10.0;
  canvas->page_color = dt_canvas_color(0.5f, 0.6f, 0.7f, 0.8f);
  canvas->shadow.color = dt_canvas_color(0.1f, 0.2f, 0.3f, 0.6f);
  canvas->shadow.offset_x = 11.0f;
  canvas->shadow.offset_y = -7.0f;
  canvas->shadow.blur = 5.5f;
  canvas->shadow.extent = 6.5f;
  canvas->padding_color = dt_canvas_color(0.9f, 0.8f, 0.7f, 0.6f);
  canvas->texture_contrast = 1.5f;
  canvas->texture_detail = 0.5f;
  canvas->texture_scale = 2.0f;
  canvas->texture_grain = 0.0f;
  canvas->corner_radius = 14.0f;
  image->corner_radius = 7.0f;
  image->flags |= DT_CANVAS_OBJECT_FLAG_CORNER_OVERRIDE;
  canvas->grid_flags |= DT_CANVAS_PADDING_VISIBLE;

  // The image gets a shadow of its own, some transparency and a polygon cutout with four nodes.
  image->shadow.color = dt_canvas_color(0.0f, 0.0f, 0.0f, 0.4f);
  image->shadow.offset_x = 3.0f;
  image->shadow.offset_y = 4.0f;
  image->shadow.blur = 2.0f;
  image->shadow.extent = 3.25f;
  image->flags |= DT_CANVAS_OBJECT_FLAG_SHADOW_OVERRIDE;
  image->transparency = 0.25f;
  image->background = dt_canvas_color(0.2f, 0.4f, 0.6f, 0.8f);
  dt_canvas_mask_set_shape(canvas, image, DT_CANVAS_MASK_POLYGON);
  // Written by index rather than as a flat run, so the record may gain a field without every
  // node in this fixture silently shifting into the next one's.
  const float corners[4][2] = { { 0.1f, 0.1f }, { 0.9f, 0.1f }, { 0.9f, 0.9f }, { 0.1f, 0.9f } };
  float nodes[4 * DT_CANVAS_MASK_NODE_FLOATS];
  memset(nodes, 0, sizeof(nodes));
  for(int node = 0; node < 4; node++)
  {
    float *record = nodes + (size_t)node * DT_CANVAS_MASK_NODE_FLOATS;
    record[DT_CANVAS_MASK_NODE_X] = corners[node][0];
    record[DT_CANVAS_MASK_NODE_Y] = corners[node][1];
    record[DT_CANVAS_MASK_NODE_CTRL1_X] = corners[node][0];
    record[DT_CANVAS_MASK_NODE_CTRL1_Y] = corners[node][1];
    record[DT_CANVAS_MASK_NODE_CTRL2_X] = corners[node][0];
    record[DT_CANVAS_MASK_NODE_CTRL2_Y] = corners[node][1];
  }
  nodes[2 * DT_CANVAS_MASK_NODE_FLOATS + DT_CANVAS_MASK_NODE_SMOOTH] = 1.0f;
  nodes[3 * DT_CANVAS_MASK_NODE_FLOATS + DT_CANVAS_MASK_NODE_BORDER1] = 0.07f;
  nodes[3 * DT_CANVAS_MASK_NODE_FLOATS + DT_CANVAS_MASK_NODE_BORDER2] = 0.04f;
  dt_canvas_mask_set_nodes(canvas, image, nodes, 4);
  image->mask.feather = 0.12f;
  image->mask.flags = DT_CANVAS_MASK_INVERT;
  canvas->page_margin = 18.0f;
  canvas->page_bleed = 9.0f;
  canvas->margin_color = dt_canvas_color(0.1f, 0.2f, 0.25f, 1.0f);
  canvas->bleed_color = dt_canvas_color(0.75f, 0.3f, 0.3f, 1.0f);
  // The text gets an ellipse: no nodes, only the fixed fields.
  dt_canvas_mask_set_shape(canvas, text, DT_CANVAS_MASK_ELLIPSE);
  text->mask.center_x = 0.4f;
  text->mask.rotation = 30.0f;

  dt_canvas_object_t *map = dt_canvas_add_map(canvas, 700.0, 700.0, 45.1885, 5.7245, 13, 1);
  const char map_stand_in[] = "\xff\xd8map\xff\xd9";
  GBytes *map_jpeg = g_bytes_new(map_stand_in, sizeof(map_stand_in));
  dt_canvas_map_set_render(canvas, map, map_jpeg, 600, 450, 1725000001LL);
  g_bytes_unref(map_jpeg);
  return canvas;
}

static void _index_round_trip_keeps_every_field(void **state)
{
  (void)state;
  dt_canvas_t *canvas = _populated_canvas();
  GBytes *index = dt_canvas_format_write_index(canvas);
  assert_non_null(index);

  dt_canvas_t *restored = dt_canvas_new();
  GError *error = NULL;
  assert_true(dt_canvas_format_read_index(restored, index, &error));
  assert_null(error);
  g_bytes_unref(index);

  assert_string_equal(restored->title, "Exhibition 2026");
  assert_float_equal(restored->grid_size, 25.0f, 1e-6);
  assert_int_equal(restored->grid_flags, DT_CANVAS_GRID_VISIBLE | DT_CANVAS_GRID_SNAP | DT_CANVAS_PADDING_VISIBLE);
  assert_float_equal(restored->border_color.green, 0.5f, 1e-6);
  assert_float_equal(restored->padding, 35.0f, 1e-6);
  assert_int_equal(restored->background_style, DT_CANVAS_BACKGROUND_WATERCOLOUR);
  assert_float_equal(restored->grid_color.alpha, 0.4f, 1e-6);
  assert_int_equal(restored->paper_size, DT_CANVAS_PAPER_A4);
  assert_int_equal(restored->paper_landscape, 1);
  assert_int_equal(restored->reserved[7], 0xAB);
  assert_int_equal(dt_canvas_object_count(restored), 4);
  assert_int_equal(restored->next_id, canvas->next_id);
  const dt_canvas_object_t *map = dt_canvas_find_object(restored, 4);
  assert_non_null(map);
  assert_int_equal(map->kind, DT_CANVAS_OBJECT_MAP);
  assert_float_equal(map->map.latitude, 45.1885, 1e-9);
  assert_float_equal(map->map.longitude, 5.7245, 1e-9);
  assert_int_equal(map->map.zoom, 13);
  assert_int_equal(map->map.source, 1);
  assert_int_equal(map->map.pixel_width, 600);
  assert_null(map->map.jpeg); // its raster is an archive entry of its own

  const dt_canvas_object_t *image = dt_canvas_find_object(restored, 1);
  assert_non_null(image);
  assert_int_equal(image->kind, DT_CANVAS_OBJECT_IMAGE);
  assert_int_equal(image->image.imgid, 42);
  assert_int_equal(image->image.film_id, 7);
  assert_string_equal(image->image.folder, "/home/someone/Pictures/2026");
  assert_string_equal(image->image.filename, "DSC_0001.NEF");
  assert_string_equal(image->image.exif_maker, "Nikon");
  assert_float_equal(image->image.exif_iso, 400.0f, 1e-6);
  assert_true(image->image.exif_datetime_taken == 1700000000000000LL);
  assert_true(image->image.history_hash == 0x1234567890ABCDEFULL);
  assert_int_equal(image->image.pixel_width, 2048);
  assert_int_equal(image->image.colorspace, DT_CANVAS_COLORSPACE_ADOBERGB);
  assert_float_equal(image->rotation, 0.25, 1e-12);
  assert_int_equal(image->flags, DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE | DT_CANVAS_OBJECT_FLAG_SHADOW_OVERRIDE
                                     | DT_CANVAS_OBJECT_FLAG_CORNER_OVERRIDE);
  assert_float_equal(image->border_width, 9.0f, 1e-6);
  // The JPEG is a separate archive entry, not an index field.
  assert_null(image->image.jpeg);
  // The shadow, the transparency and the cutout: fixed fields taken from the reserved bytes,
  // and the polygon's nodes from a chunk after the record.
  assert_float_equal(image->shadow.color.alpha, 0.4f, 1e-6);
  assert_float_equal(image->shadow.offset_x, 3.0f, 1e-6);
  assert_float_equal(image->shadow.offset_y, 4.0f, 1e-6);
  assert_float_equal(image->shadow.blur, 2.0f, 1e-6);
  assert_float_equal(image->shadow.extent, 3.25f, 1e-6);
  assert_float_equal(image->transparency, 0.25f, 1e-6);
  assert_float_equal(image->background.green, 0.4f, 1e-6);
  assert_float_equal(image->background.alpha, 0.8f, 1e-6);
  assert_int_equal(image->mask.shape, DT_CANVAS_MASK_POLYGON);
  assert_int_equal(image->mask.flags, DT_CANVAS_MASK_INVERT);
  assert_float_equal(image->mask.feather, 0.12f, 1e-6);
  assert_int_equal(image->mask.node_count, 4);
  assert_non_null(image->mask.nodes);
  assert_float_equal(image->mask.nodes[1 * DT_CANVAS_MASK_NODE_FLOATS + DT_CANVAS_MASK_NODE_X], 0.9f, 1e-6);
  assert_float_equal(image->mask.nodes[2 * DT_CANVAS_MASK_NODE_FLOATS + DT_CANVAS_MASK_NODE_Y], 0.9f, 1e-6);
  assert_float_equal(image->mask.nodes[2 * DT_CANVAS_MASK_NODE_FLOATS + DT_CANVAS_MASK_NODE_SMOOTH], 1.0f, 1e-6);
  // A node's own fall-off radii, either side of it, survive the round trip.
  assert_float_equal(image->mask.nodes[3 * DT_CANVAS_MASK_NODE_FLOATS + DT_CANVAS_MASK_NODE_BORDER1], 0.07f, 1e-6);
  assert_float_equal(image->mask.nodes[3 * DT_CANVAS_MASK_NODE_FLOATS + DT_CANVAS_MASK_NODE_BORDER2], 0.04f, 1e-6);
  assert_float_equal(image->mask.nodes[0 * DT_CANVAS_MASK_NODE_FLOATS + DT_CANVAS_MASK_NODE_BORDER1], 0.0f, 1e-6);
  assert_float_equal(restored->shadow.color.alpha, 0.6f, 1e-6);
  assert_float_equal(restored->shadow.offset_x, 11.0f, 1e-6);
  assert_float_equal(restored->shadow.offset_y, -7.0f, 1e-6);
  assert_float_equal(restored->shadow.blur, 5.5f, 1e-6);
  assert_float_equal(restored->shadow.extent, 6.5f, 1e-6);
  assert_float_equal(restored->page_margin, 18.0f, 1e-6);
  assert_float_equal(restored->page_bleed, 9.0f, 1e-6);
  assert_float_equal(restored->margin_color.blue, 0.25f, 1e-6);
  assert_float_equal(restored->bleed_color.red, 0.75f, 1e-6);
  assert_float_equal(restored->padding_color.red, 0.9f, 1e-6);
  assert_float_equal(restored->texture_contrast, 1.5f, 1e-6);
  assert_float_equal(restored->texture_scale, 2.0f, 1e-6);
  assert_float_equal(restored->corner_radius, 14.0f, 1e-6);
  assert_float_equal(image->corner_radius, 7.0f, 1e-6);
  assert_true((image->flags & DT_CANVAS_OBJECT_FLAG_CORNER_OVERRIDE) != 0);
  // A zero among non-zero siblings is a weight the user turned off, and reads as off. Only
  // all FOUR at zero is a file from before the fields, and that one reads as the paper as
  // designed -- the fixture sets three weights and leaves the grain at zero.
  float detail = -1.0f;
  float grain = -1.0f;
  dt_canvas_texture_get(restored, NULL, &detail, NULL, &grain);
  assert_float_equal(detail, 0.5f, 1e-6);
  assert_float_equal(grain, 0.0f, 1e-6);

  dt_canvas_t *old_file = dt_canvas_new();
  assert_non_null(old_file);
  old_file->texture_contrast = 0.0f;
  old_file->texture_detail = 0.0f;
  old_file->texture_scale = 0.0f;
  old_file->texture_grain = 0.0f;
  float weights[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
  dt_canvas_texture_get(old_file, &weights[0], &weights[1], &weights[2], &weights[3]);
  for(int idx = 0; idx < 4; idx++) assert_float_equal(weights[idx], 1.0f, 1e-6);
  dt_canvas_free(old_file);
  assert_true((restored->grid_flags & DT_CANVAS_PADDING_VISIBLE) != 0);

  const dt_canvas_object_t *text = dt_canvas_find_object(restored, 2);
  assert_non_null(text);
  assert_string_equal(text->text.font, "Serif Bold 14");
  assert_int_equal(text->text.align_h, DT_CANVAS_ALIGN_JUSTIFY);
  assert_int_equal(text->text.align_v, DT_CANVAS_ALIGN_END);
  assert_float_equal(text->text.background.alpha, 0.0f, 1e-6);
  assert_float_equal(text->text.first_line_indent, -18.0f, 1e-6);
  assert_float_equal(text->text.paragraph_spacing, 24.0f, 1e-6);
  assert_float_equal(text->text.shadow.blur, 1.5f, 1e-6);
  assert_float_equal(text->text.shadow.extent, 1.75f, 1e-6);
  assert_int_equal(text->mask.shape, DT_CANVAS_MASK_ELLIPSE);
  assert_float_equal(text->mask.center_x, 0.4f, 1e-6);
  assert_float_equal(text->mask.rotation, 30.0f, 1e-6);
  assert_int_equal(text->mask.node_count, 0);
  assert_null(text->mask.nodes);

  const dt_canvas_object_t *connector = dt_canvas_find_object(restored, 3);
  assert_non_null(connector);
  assert_int_equal(connector->connector.from_id, 2);
  assert_int_equal(connector->connector.to_id, 1);
  assert_int_equal(connector->connector.from_anchor, DT_CANVAS_ANCHOR_SOUTH);
  assert_int_equal(connector->connector.to_anchor, DT_CANVAS_ANCHOR_WEST);
  assert_int_equal(connector->connector.routing, DT_CANVAS_ROUTING_CUBIC);
  assert_int_equal(connector->connector.via_count, 1);
  assert_float_equal(connector->connector.via_x, -120.0, 1e-9);
  assert_float_equal(connector->connector.via_y, 300.0, 1e-9);
  assert_float_equal(connector->connector.from_reach, 77.0f, 1e-6);
  assert_float_equal(connector->connector.via_tangent_x, 40.0, 1e-9);
  assert_float_equal(restored->page_color.blue, 0.7f, 1e-6);

  dt_canvas_free(restored);
  dt_canvas_free(canvas);
}

/*
 * A shadow's extent sizes a raster, so a stored one nobody can read is no grow at all rather than
 * the largest the range allows, and none is ever negative -- held once, as the record is read, for
 * the canvas's shadow, an object's and a text's glyphs' alike.
 */
static void _a_stored_extent_is_held_to_its_range(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  assert_non_null(canvas);
  canvas->shadow.extent = NAN;
  dt_canvas_object_t *image = dt_canvas_add_image(canvas, 0.0, 0.0, 600, 400);
  assert_non_null(image);
  image->shadow.extent = -4.0f;
  dt_canvas_object_t *text = dt_canvas_add_text(canvas, 400.0, 0.0, 200.0, 100.0, "Caption");
  assert_non_null(text);
  text->text.shadow.extent = 1.0e9f;
  const uint32_t image_id = image->id;
  const uint32_t text_id = text->id;
  GBytes *index = dt_canvas_format_write_index(canvas);
  assert_non_null(index);
  dt_canvas_t *restored = dt_canvas_new();
  GError *error = NULL;
  assert_true(dt_canvas_format_read_index(restored, index, &error));
  assert_null(error);
  g_bytes_unref(index);
  assert_float_equal(restored->shadow.extent, 0.0f, 1e-6);
  assert_float_equal(dt_canvas_find_object(restored, image_id)->shadow.extent, 0.0f, 1e-6);
  assert_float_equal(dt_canvas_find_object(restored, text_id)->text.shadow.extent, DT_CANVAS_SHADOW_EXTENT_MAX, 1e-6);
  dt_canvas_free(restored);
  dt_canvas_free(canvas);
}

static void _a_record_from_a_later_version_is_skipped_by_its_size(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_add_text(canvas, 0.0, 0.0, 100.0, 100.0, "a");
  dt_canvas_add_text(canvas, 10.0, 10.0, 100.0, 100.0, "b");
  GBytes *index = dt_canvas_format_write_index(canvas);
  gsize size = 0;
  const uint8_t *data = g_bytes_get_data(index, &size);

  // Grow the first record by 16 bytes of "future fields" and patch its record_size.
  const uint32_t header_size = data[8 + 4] | (data[8 + 5] << 8) | (data[8 + 6] << 16) | ((uint32_t)data[8 + 7] << 24);
  const uint8_t *first = data + header_size;
  const uint32_t first_size = first[4] | (first[5] << 8) | (first[6] << 16) | ((uint32_t)first[7] << 24);
  GByteArray *grown = g_byte_array_new();
  g_byte_array_append(grown, data, header_size + first_size);
  const uint8_t future[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
  g_byte_array_append(grown, future, sizeof(future));
  g_byte_array_append(grown, data + header_size + first_size, size - header_size - first_size);
  const uint32_t new_size = first_size + sizeof(future);
  grown->data[header_size + 4] = (uint8_t)(new_size & 0xFF);
  grown->data[header_size + 5] = (uint8_t)((new_size >> 8) & 0xFF);
  grown->data[header_size + 6] = (uint8_t)((new_size >> 16) & 0xFF);
  grown->data[header_size + 7] = (uint8_t)((new_size >> 24) & 0xFF);
  g_bytes_unref(index);
  GBytes *grown_bytes = g_byte_array_free_to_bytes(grown);

  dt_canvas_t *restored = dt_canvas_new();
  assert_true(dt_canvas_format_read_index(restored, grown_bytes, NULL));
  assert_int_equal(dt_canvas_object_count(restored), 2);
  assert_non_null(dt_canvas_find_object(restored, 1));
  assert_non_null(dt_canvas_find_object(restored, 2));
  assert_float_equal(dt_canvas_find_object(restored, 2)->x, 10.0, 1e-12);
  g_bytes_unref(grown_bytes);
  dt_canvas_free(restored);
  dt_canvas_free(canvas);
}

static void _a_newer_format_and_a_truncated_index_are_refused(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_add_text(canvas, 0.0, 0.0, 100.0, 100.0, "a");
  GBytes *index = dt_canvas_format_write_index(canvas);
  gsize size = 0;
  const uint8_t *data = g_bytes_get_data(index, &size);

  uint8_t *newer = g_memdup2(data, size);
  newer[8] = 99;
  GBytes *newer_bytes = g_bytes_new_take(newer, size);
  dt_canvas_t *restored = dt_canvas_new();
  GError *error = NULL;
  assert_false(dt_canvas_format_read_index(restored, newer_bytes, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_CANVAS_ERROR_VERSION);
  g_clear_error(&error);
  g_bytes_unref(newer_bytes);
  dt_canvas_free(restored);

  GBytes *truncated = g_bytes_new(data, size - 40);
  restored = dt_canvas_new();
  assert_false(dt_canvas_format_read_index(restored, truncated, &error));
  assert_int_equal(error->code, DT_CANVAS_ERROR_CORRUPT);
  g_clear_error(&error);
  g_bytes_unref(truncated);
  dt_canvas_free(restored);

  GBytes *garbage = g_bytes_new_static("PK garbage that is not an index", 31);
  restored = dt_canvas_new();
  assert_false(dt_canvas_format_read_index(restored, garbage, &error));
  assert_int_equal(error->code, DT_CANVAS_ERROR_NOT_A_CANVAS);
  g_clear_error(&error);
  g_bytes_unref(garbage);
  dt_canvas_free(restored);

  g_bytes_unref(index);
  dt_canvas_free(canvas);
}

static void _archive_round_trip_carries_jpegs_and_markdown(void **state)
{
  (void)state;
  char *path = g_build_filename(g_get_tmp_dir(), "ansel-test-canvas-document" DT_CANVAS_FILE_EXTENSION, NULL);
  g_unlink(path);
  dt_canvas_t *canvas = _populated_canvas();
  GError *error = NULL;
  assert_true(dt_canvas_save(canvas, path, &error));
  assert_null(error);
  assert_false(canvas->dirty);
  assert_string_equal(canvas->path, path);

  dt_canvas_t *loaded = dt_canvas_load(path, &error);
  assert_non_null(loaded);
  assert_null(error);
  assert_int_equal(dt_canvas_object_count(loaded), 4);
  assert_non_null(dt_canvas_object_raster(dt_canvas_find_object(loaded, 4)));
  assert_int_equal(g_bytes_get_size(dt_canvas_object_raster(dt_canvas_find_object(loaded, 4))),
                   g_bytes_get_size(dt_canvas_object_raster(dt_canvas_find_object(canvas, 4))));
  const dt_canvas_object_t *image = dt_canvas_find_object(loaded, 1);
  assert_non_null(image->image.jpeg);
  assert_int_equal(g_bytes_get_size(image->image.jpeg), g_bytes_get_size(dt_canvas_find_object(canvas, 1)->image.jpeg));
  const dt_canvas_object_t *text = dt_canvas_find_object(loaded, 2);
  assert_string_equal(dt_canvas_text_get_markdown(text), "# Title\n\nSome *emphasis*.");
  assert_false(loaded->dirty);
  dt_canvas_free(loaded);

  assert_null(dt_canvas_load("/nonexistent/x" DT_CANVAS_FILE_EXTENSION, &error));
  assert_non_null(error);
  g_clear_error(&error);

  g_unlink(path);
  g_free(path);
  dt_canvas_free(canvas);
}

static void _removing_a_frame_takes_its_connectors_and_unlinks_sidecars(void **state)
{
  (void)state;
  dt_canvas_t *canvas = _populated_canvas();
  dt_canvas_object_t *sidecar = dt_canvas_add_text(canvas, 0.0, 0.0, 100.0, 100.0, "note");
  sidecar->text.source = DT_CANVAS_TEXT_SOURCE_SIDECAR;
  sidecar->text.linked_object = 1;
  assert_int_equal(dt_canvas_object_count(canvas), 5);
  assert_true(dt_canvas_remove_object(canvas, 1));
  assert_int_equal(dt_canvas_object_count(canvas), 3);
  assert_null(dt_canvas_find_object(canvas, 3));
  assert_int_equal(sidecar->text.linked_object, 0);
  assert_int_equal(sidecar->text.source, DT_CANVAS_TEXT_SOURCE_MARKDOWN);
  assert_false(dt_canvas_remove_object(canvas, 1));
  dt_canvas_free(canvas);
}

static void _snapshot_restore_round_trips_the_objects(void **state)
{
  (void)state;
  dt_canvas_t *canvas = _populated_canvas();
  dt_canvas_t *snapshot = dt_canvas_copy(canvas);
  assert_true(dt_canvas_remove_object(canvas, 2));
  assert_int_equal(dt_canvas_object_count(canvas), 2);
  dt_canvas_restore(canvas, snapshot);
  assert_int_equal(dt_canvas_object_count(canvas), 4);
  assert_string_equal(dt_canvas_text_get_markdown(dt_canvas_find_object(canvas, 2)), "# Title\n\nSome *emphasis*.");
  // The snapshot is untouched by the restore and by the later edit.
  dt_canvas_text_set_markdown(canvas, dt_canvas_find_object(canvas, 2), "changed");
  assert_string_equal(dt_canvas_text_get_markdown(dt_canvas_find_object(snapshot, 2)), "# Title\n\nSome *emphasis*.");
  dt_canvas_free(snapshot);
  dt_canvas_free(canvas);
}

static void _abandoning_a_gesture_keeps_the_renders_and_the_saved_state(void **state)
{
  (void)state;
  // A colour window left open while a picture renders and a map fetches its tiles, then closed with Escape.
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *text = dt_canvas_add_text(canvas, 0.0, 0.0, 300.0, 100.0, "A caption");
  const uint32_t text_id = text->id;
  text->text.text_color = dt_canvas_color(0.0f, 0.0f, 1.0f, 1.0f);
  dt_canvas_object_t *image = dt_canvas_add_image(canvas, 500.0, 0.0, 6000, 4000);
  const uint32_t image_id = image->id;
  image->image.sync_status = DT_CANVAS_SYNC_RENDERING;
  const double area = image->width * image->height;
  dt_canvas_object_t *map = dt_canvas_add_map(canvas, 0.0, 500.0, 48.85, 2.35, 12, 0u);
  const uint32_t map_id = map->id;
  map->map.sync_status = DT_CANVAS_SYNC_RENDERING;
  canvas->dirty = FALSE;
  dt_canvas_t *snapshot = dt_canvas_copy(canvas);

  text = dt_canvas_find_object(canvas, text_id);
  text->text.text_color = dt_canvas_color(1.0f, 0.0f, 0.0f, 0.5f);
  dt_canvas_touch(canvas);
  const char picture_bytes[] = "\xff\xd8 picture\xff\xd9";
  GBytes *picture = g_bytes_new(picture_bytes, sizeof(picture_bytes));
  dt_canvas_image_set_render(canvas, dt_canvas_find_object(canvas, image_id), picture, 3000, 1000, 77u, 1234, 0u);
  const char tiles_bytes[] = "\xff\xd8some tiles\xff\xd9";
  GBytes *tiles = g_bytes_new(tiles_bytes, sizeof(tiles_bytes));
  map = dt_canvas_find_object(canvas, map_id);
  dt_canvas_map_set_render(canvas, map, tiles, 512, 256, 5678);
  const uint64_t generation = canvas->generation;

  dt_canvas_abandon(canvas, snapshot);
  // What the gesture did goes back.
  text = dt_canvas_find_object(canvas, text_id);
  assert_float_equal(text->text.text_color.blue, 1.0f, 0.0f);
  assert_float_equal(text->text.text_color.alpha, 1.0f, 0.0f);
  // What landed meanwhile stays: without it the picture reads RENDERING with no job left to finish it.
  image = dt_canvas_find_object(canvas, image_id);
  assert_int_equal(image->image.sync_status, DT_CANVAS_SYNC_CURRENT);
  assert_ptr_equal(image->image.jpeg, picture);
  assert_int_equal(image->image.pixel_width, 3000);
  assert_true(image->image.history_hash == 77u);
  // The frame takes the render's proportions on the area it had, as a render landing does.
  assert_float_equal(image->width / image->height, 3.0, 1e-9);
  assert_float_equal(image->width * image->height, area, 1e-6);
  map = dt_canvas_find_object(canvas, map_id);
  assert_int_equal(map->map.sync_status, DT_CANVAS_SYNC_CURRENT);
  assert_ptr_equal(map->map.jpeg, tiles);
  assert_int_equal(map->map.pixel_height, 256);
  assert_float_equal(map->map.latitude, 48.85, 1e-9);
  // A render is written with the document: something to save, although the gesture was not.
  assert_true(canvas->dirty);
  assert_true(canvas->generation > generation);
  // The snapshot keeps its own.
  assert_int_equal(dt_canvas_find_object(snapshot, image_id)->image.sync_status, DT_CANVAS_SYNC_RENDERING);
  g_bytes_unref(picture);
  g_bytes_unref(tiles);
  dt_canvas_free(snapshot);

  // Nothing landed: a document saved before the gesture is saved after it, and still repaints.
  canvas->dirty = FALSE;
  snapshot = dt_canvas_copy(canvas);
  text = dt_canvas_find_object(canvas, text_id);
  text->text.text_color = dt_canvas_color(0.0f, 1.0f, 0.0f, 1.0f);
  dt_canvas_touch(canvas);
  const uint64_t edited = canvas->generation;
  dt_canvas_abandon(canvas, snapshot);
  assert_false(canvas->dirty);
  assert_true(canvas->generation > edited);
  assert_float_equal(dt_canvas_find_object(canvas, text_id)->text.text_color.green, 0.0f, 0.0f);
  dt_canvas_free(snapshot);
  dt_canvas_free(canvas);
}

static void _draw_order_edits_keep_the_list_sorted(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *first = dt_canvas_add_text(canvas, 0.0, 0.0, 10.0, 10.0, "1");
  dt_canvas_object_t *second = dt_canvas_add_text(canvas, 0.0, 0.0, 10.0, 10.0, "2");
  dt_canvas_object_t *third = dt_canvas_add_text(canvas, 0.0, 0.0, 10.0, 10.0, "3");
  assert_ptr_equal(dt_canvas_object_at(canvas, 2), third);
  dt_canvas_object_to_front(canvas, first->id);
  assert_ptr_equal(dt_canvas_object_at(canvas, 2), first);
  dt_canvas_object_to_back(canvas, first->id);
  assert_ptr_equal(dt_canvas_object_at(canvas, 0), first);
  dt_canvas_object_raise(canvas, first->id);
  assert_ptr_equal(dt_canvas_object_at(canvas, 1), first);
  assert_ptr_equal(dt_canvas_object_at(canvas, 0), second);
  dt_canvas_object_lower(canvas, first->id);
  assert_ptr_equal(dt_canvas_object_at(canvas, 0), first);
  // The frontmost object under a point wins the pick.
  assert_ptr_equal(dt_canvas_pick(canvas, 0.0, 0.0, 0.0), third);
  dt_canvas_free(canvas);
}

static void _rotated_frames_answer_hit_tests_and_bounds(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *frame = dt_canvas_add_text(canvas, 100.0, 100.0, 200.0, 100.0, "");
  frame->rotation = M_PI / 2.0; // now 100 wide and 200 tall on screen
  assert_true(dt_canvas_object_contains(canvas, frame, 100.0, 190.0, 0.0));
  assert_false(dt_canvas_object_contains(canvas, frame, 190.0, 100.0, 0.0));
  const dt_canvas_rect_t bounds = dt_canvas_object_bounds(frame);
  assert_float_equal(bounds.width, 100.0, 1e-9);
  assert_float_equal(bounds.height, 200.0, 1e-9);
  assert_float_equal(bounds.x, 50.0, 1e-9);

  dt_canvas_object_t *other = dt_canvas_add_text(canvas, 500.0, 100.0, 100.0, 100.0, "");
  dt_canvas_object_t *connector = dt_canvas_add_connector(canvas, frame->id, other->id);
  double from_x = 0.0;
  double from_y = 0.0;
  double to_x = 0.0;
  double to_y = 0.0;
  assert_true(dt_canvas_connector_endpoints(canvas, connector, &from_x, &from_y, &to_x, &to_y));
  assert_float_equal(from_x, 150.0, 1e-9); // leaves the rotated frame's right edge
  assert_float_equal(to_x, 450.0, 1e-9);   // enters the other frame's left edge
  assert_true(dt_canvas_object_contains(canvas, connector, 300.0, 100.0, 1.0));
  assert_false(dt_canvas_object_contains(canvas, connector, 300.0, 140.0, 1.0));

  // Self links and links to connectors are refused.
  assert_null(dt_canvas_add_connector(canvas, frame->id, frame->id));
  assert_null(dt_canvas_add_connector(canvas, frame->id, connector->id));
  dt_canvas_free(canvas);
}

static void _connectors_route_between_cardinal_anchors(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *left = dt_canvas_add_text(canvas, 0.0, 0.0, 200.0, 100.0, "");
  dt_canvas_object_t *right = dt_canvas_add_text(canvas, 600.0, 400.0, 200.0, 100.0, "");
  dt_canvas_object_t *connector = dt_canvas_add_connector(canvas, left->id, right->id);
  dt_canvas_route_t route;

  // A new connector is a cubic spline; its handles are automatic until dragged.
  assert_int_equal(connector->connector.routing, DT_CANVAS_ROUTING_CUBIC);
  assert_float_equal(connector->connector.from_reach, 0.0f, 1e-6);
  connector->connector.routing = DT_CANVAS_ROUTING_STRAIGHT;

  // Automatic anchors face each other: the left frame's right edge, the right frame's left edge.
  assert_true(dt_canvas_connector_route(canvas, connector, &route));
  assert_int_equal(route.routing, DT_CANVAS_ROUTING_STRAIGHT);
  assert_int_equal(route.point_count, 2);
  assert_float_equal(route.from_x, 100.0, 1e-9);
  assert_float_equal(route.from_y, 0.0, 1e-9);
  assert_float_equal(route.from_normal_x, 1.0, 1e-9);
  assert_float_equal(route.to_x, 500.0, 1e-9);
  assert_float_equal(route.to_normal_x, -1.0, 1e-9);

  // Explicit anchors are the frame's own cardinal points, and rotate with it.
  connector->connector.from_anchor = DT_CANVAS_ANCHOR_SOUTH;
  connector->connector.to_anchor = DT_CANVAS_ANCHOR_NORTH;
  assert_true(dt_canvas_connector_route(canvas, connector, &route));
  assert_float_equal(route.from_y, 50.0, 1e-9);
  assert_float_equal(route.from_normal_y, 1.0, 1e-9);
  assert_float_equal(route.to_y, 350.0, 1e-9);
  left->rotation = M_PI / 2.0;
  assert_true(dt_canvas_connector_route(canvas, connector, &route));
  assert_float_equal(route.from_x, -50.0, 1e-9); // south, turned a quarter clockwise, now faces west
  assert_float_equal(route.from_normal_x, -1.0, 1e-9);
  left->rotation = 0.0;

  // Square routing leaves each anchor along its normal and travels in horizontal and vertical legs.
  connector->connector.routing = DT_CANVAS_ROUTING_SQUARE;
  assert_true(dt_canvas_connector_route(canvas, connector, &route));
  assert_true(route.point_count >= 4);
  for(int idx = 0; idx + 1 < route.point_count; idx++)
  {
    const double delta_x = fabs(route.points[2 * idx + 2] - route.points[2 * idx]);
    const double delta_y = fabs(route.points[2 * idx + 3] - route.points[2 * idx + 1]);
    assert_true(delta_x < 1e-9 || delta_y < 1e-9);
  }
  assert_float_equal(route.points[2], route.from_x, 1e-9); // first leg goes straight down from the south anchor
  assert_true(route.points[3] > route.from_y);

  // Cubic routing: control points along the normals, ends on the anchors, flattened for hit tests.
  connector->connector.routing = DT_CANVAS_ROUTING_CUBIC;
  assert_true(dt_canvas_connector_route(canvas, connector, &route));
  assert_int_equal(route.point_count, DT_CANVAS_ROUTE_MAX_POINTS);
  assert_float_equal(route.control1_x, route.from_x, 1e-9);
  assert_true(route.control1_y > route.from_y);
  assert_true(route.control2_y < route.to_y);
  // A dragged handle sets the control point's distance along the normal, and nothing else.
  connector->connector.from_reach = 123.0f;
  assert_true(dt_canvas_connector_route(canvas, connector, &route));
  assert_float_equal(route.control1_x, route.from_x, 1e-9);
  assert_float_equal(route.control1_y, route.from_y + 123.0, 1e-6);
  connector->connector.from_reach = 0.0f;
  assert_true(dt_canvas_connector_route(canvas, connector, &route));
  assert_float_equal(route.points[0], route.from_x, 1e-9);
  assert_float_equal(route.points[2 * route.point_count - 1], route.to_y, 1e-9);
  // The curve's middle is where a hit test finds it, well off the straight chord.
  const double middle_x = route.points[DT_CANVAS_ROUTE_MAX_POINTS];
  const double middle_y = route.points[DT_CANVAS_ROUTE_MAX_POINTS + 1];
  assert_true(dt_canvas_object_contains(canvas, connector, middle_x, middle_y, 1.0));
  assert_false(dt_canvas_object_contains(canvas, connector, 100.0, 300.0, 1.0));
  dt_canvas_free(canvas);
}

/**
 * cmocka's float and double comparisons both pass anything within FLT_EPSILON of the larger
 * value, whatever tolerance they are handed -- 3.6e-5 at 300 units, measured -- so a check that
 * means a tighter tolerance than that has to say so itself.
 */
#define assert_near(actual, expected, tolerance)                                                                 \
  do                                                                                                            \
  {                                                                                                             \
    const double near_actual = (double)(actual);                                                                \
    const double near_expected = (double)(expected);                                                            \
    const double near_tolerance = (double)(tolerance);                                                          \
    if(!(fabs(near_actual - near_expected) <= near_tolerance))                                                  \
      fail_msg("%s is %.17g, expected %.17g within %g", #actual, near_actual, near_expected, near_tolerance);    \
  } while(0)

/*
 * The anchored routes, as they were before free ends. Free ends were added to the one resolver
 * every route goes through, and a document laid out before them must come back to the lines it
 * had. These hashes were taken from the resolver as it stood BEFORE free ends existed, over three
 * frames (one rotated, one cut to an ellipse, one plain), every pairing, three anchor pairs
 * covering AUTO, a cardinal, a corner and the centre, every routing, with and without a
 * waypoint, and with the handles automatic and dragged.
 *
 * Every length is hashed ROUNDED to 1/65536 of a unit, never by its bit pattern. The library is
 * built with -ffast-math and -ffp-contract=fast in some configurations and without either in
 * others, so the last bit of a sum is the compiler's to choose and the resolver's source does not
 * define it: the old resolver itself gave 44 of these 108 routes different bits in a gcc Debug
 * build than in RelWithDebInfo, one ulp each. A quantum of 2^-16 sits far above that and far
 * below any change in what a route IS -- measured, the value closest to a rounding boundary lies
 * 7e-9 units from it, half a million ulps of that value -- and the same 108 hashes came out of
 * the old resolver built as gcc Debug and as clang Debug, and out of the new one in gcc
 * RelWithDebInfo with LTO, whose bits were the old resolver's in that build. Which fields a free
 * end reads is pinned EXACTLY below, where both sides of the comparison come out of one build.
 */
#define GOLDEN_ROUTE_PAIRS 3
#define GOLDEN_ROUTE_ANCHOR_SETS 3
#define GOLDEN_ROUTE_ROUTINGS 3
#define GOLDEN_ROUTE_VIAS 2
#define GOLDEN_ROUTE_HANDLES 2
#define GOLDEN_ROUTE_CASES                                                                                      \
  (GOLDEN_ROUTE_PAIRS * GOLDEN_ROUTE_ANCHOR_SETS * GOLDEN_ROUTE_ROUTINGS * GOLDEN_ROUTE_VIAS * GOLDEN_ROUTE_HANDLES)
#define GOLDEN_ROUTE_QUANTA_PER_UNIT 65536.0

static uint64_t _golden_hash_bits(uint64_t hash, const uint64_t bits)
{
  for(int byte = 0; byte < 8; byte++)
  {
    hash ^= (bits >> (8 * byte)) & 0xFFu;
    hash *= 1099511628211ULL;
  }
  return hash;
}

static uint64_t _golden_hash_length(const uint64_t hash, const double value)
{
  return _golden_hash_bits(hash, (uint64_t)llround(value * GOLDEN_ROUTE_QUANTA_PER_UNIT));
}

/** Every field a route resolves to, in a fixed order: the counts as they are, the lengths rounded. */
static uint64_t _golden_route_hash(const dt_canvas_route_t *route)
{
  uint64_t hash = 14695981039346656037ULL;
  hash = _golden_hash_bits(hash, (uint64_t)route->routing);
  hash = _golden_hash_length(hash, route->from_x);
  hash = _golden_hash_length(hash, route->from_y);
  hash = _golden_hash_length(hash, route->to_x);
  hash = _golden_hash_length(hash, route->to_y);
  hash = _golden_hash_length(hash, route->from_normal_x);
  hash = _golden_hash_length(hash, route->from_normal_y);
  hash = _golden_hash_length(hash, route->to_normal_x);
  hash = _golden_hash_length(hash, route->to_normal_y);
  hash = _golden_hash_bits(hash, (uint64_t)route->segment_count);
  hash = _golden_hash_length(hash, route->via_x);
  hash = _golden_hash_length(hash, route->via_y);
  hash = _golden_hash_length(hash, route->control1_x);
  hash = _golden_hash_length(hash, route->control1_y);
  hash = _golden_hash_length(hash, route->control2_x);
  hash = _golden_hash_length(hash, route->control2_y);
  hash = _golden_hash_length(hash, route->control3_x);
  hash = _golden_hash_length(hash, route->control3_y);
  hash = _golden_hash_length(hash, route->control4_x);
  hash = _golden_hash_length(hash, route->control4_y);
  hash = _golden_hash_bits(hash, (uint64_t)route->point_count);
  for(int idx = 0; idx < 2 * route->point_count; idx++) hash = _golden_hash_length(hash, route->points[idx]);
  return hash;
}

/** The scene and the connector of one case, which the caller frees. */
static dt_canvas_t *_golden_route_scene(const int index, dt_canvas_object_t **connector)
{
  static const uint32_t anchor_sets[GOLDEN_ROUTE_ANCHOR_SETS][2]
      = { { DT_CANVAS_ANCHOR_AUTO, DT_CANVAS_ANCHOR_AUTO },
          { DT_CANVAS_ANCHOR_SOUTH, DT_CANVAS_ANCHOR_NORTH_WEST },
          { DT_CANVAS_ANCHOR_CENTRE, DT_CANVAS_ANCHOR_EAST } };
  static const uint32_t pairs[GOLDEN_ROUTE_PAIRS][2] = { { 1, 2 }, { 2, 3 }, { 3, 1 } };
  int rest = index;
  const int handles = rest % GOLDEN_ROUTE_HANDLES;
  rest /= GOLDEN_ROUTE_HANDLES;
  const int via = rest % GOLDEN_ROUTE_VIAS;
  rest /= GOLDEN_ROUTE_VIAS;
  const int routing = rest % GOLDEN_ROUTE_ROUTINGS;
  rest /= GOLDEN_ROUTE_ROUTINGS;
  const int anchor_set = rest % GOLDEN_ROUTE_ANCHOR_SETS;
  rest /= GOLDEN_ROUTE_ANCHOR_SETS;
  const int pair = rest % GOLDEN_ROUTE_PAIRS;

  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *rotated = dt_canvas_add_text(canvas, -250.0, 40.0, 300.0, 180.0, "rotated");
  rotated->rotation = 0.3;
  dt_canvas_object_t *plain = dt_canvas_add_text(canvas, 420.0, 310.0, 220.0, 260.0, "plain");
  plain->corner_radius = 24.0f;
  plain->flags |= DT_CANVAS_OBJECT_FLAG_CORNER_OVERRIDE;
  dt_canvas_object_t *cut = dt_canvas_add_text(canvas, 60.0, -260.0, 200.0, 140.0, "cut");
  dt_canvas_mask_set_shape(canvas, cut, DT_CANVAS_MASK_ELLIPSE);
  *connector = dt_canvas_add_connector(canvas, pairs[pair][0], pairs[pair][1]);
  (*connector)->connector.from_anchor = anchor_sets[anchor_set][0];
  (*connector)->connector.to_anchor = anchor_sets[anchor_set][1];
  (*connector)->connector.routing = (uint32_t)routing;
  if(via)
  {
    (*connector)->connector.via_count = 1;
    (*connector)->connector.via_x = 150.0;
    (*connector)->connector.via_y = 420.0;
  }
  if(handles)
  {
    (*connector)->connector.from_reach = 77.0f;
    (*connector)->connector.to_reach = 33.5f;
    (*connector)->connector.via_tangent_x = 40.0;
    (*connector)->connector.via_tangent_y = -10.0;
  }
  return canvas;
}

static const uint64_t _golden_route_hashes[GOLDEN_ROUTE_CASES] = {
  0xb3dc146adf18eed7ULL, 0xb3dc146adf18eed7ULL, 0x28b3a1bb880d7b31ULL,
  0x28b3a1bb880d7b31ULL, 0x5f5ed5321036290dULL, 0x5f5ed5321036290dULL,
  0x6c4213d676583067ULL, 0x6c4213d676583067ULL, 0x64333667bea7f904ULL,
  0xbea66eb3e68cde85ULL, 0x3e8b2dc82d6e0fd6ULL, 0x527264aed6af16a9ULL,
  0xf0df0fad1db3455cULL, 0xf0df0fad1db3455cULL, 0x30e0ca73cd086fa2ULL,
  0x30e0ca73cd086fa2ULL, 0xb0b2e4ecb1acf330ULL, 0xb0b2e4ecb1acf330ULL,
  0x7b42c3d516be478cULL, 0x7b42c3d516be478cULL, 0xc4bbb7b27d71fd1fULL,
  0x1d636ed46452179bULL, 0x234f0501c49e017aULL, 0x8d00ef80c162e272ULL,
  0xd7c2c26259c66b7bULL, 0xd7c2c26259c66b7bULL, 0x330bb41c6f3d3b19ULL,
  0x330bb41c6f3d3b19ULL, 0xc5287a100a482d86ULL, 0xc5287a100a482d86ULL,
  0x2c2acd5b0fcc925cULL, 0x2c2acd5b0fcc925cULL, 0x8d5cc27ba89d6879ULL,
  0xb5c11b5f66c76cf8ULL, 0x8956df32123a1483ULL, 0x0f2f88d2518a9caaULL,
  0xa01551fa040eec3dULL, 0xa01551fa040eec3dULL, 0x43914548ba780e97ULL,
  0x43914548ba780e97ULL, 0x6b2f530ff1788fd7ULL, 0x6b2f530ff1788fd7ULL,
  0xfbb090af752cd0c9ULL, 0xfbb090af752cd0c9ULL, 0xb00f6206a3221835ULL,
  0xcbe1d9284c5ff338ULL, 0xce95264420ca3f49ULL, 0xcb2d83fc73343383ULL,
  0xb7a716723182227fULL, 0xb7a716723182227fULL, 0xc308f5e42fcac5fdULL,
  0xc308f5e42fcac5fdULL, 0x7462db0c3e12546bULL, 0x7462db0c3e12546bULL,
  0x6c10237b17543ab7ULL, 0x6c10237b17543ab7ULL, 0xd96b3dfcd58a93f1ULL,
  0xf52f7c92c2df9340ULL, 0x1f3148fc84ef527dULL, 0x9e2e623f767b78b8ULL,
  0xdf4f8a4173274505ULL, 0xdf4f8a4173274505ULL, 0x387e6c87020f142bULL,
  0x387e6c87020f142bULL, 0xea730a2e560949bfULL, 0xea730a2e560949bfULL,
  0xd702d3b386efdbf7ULL, 0xd702d3b386efdbf7ULL, 0xf21112c6eced5514ULL,
  0x3a735b1205b0ef29ULL, 0xffd90adf31de19eeULL, 0x9a01674bb5c9246cULL,
  0xfaca37641e403306ULL, 0xfaca37641e403306ULL, 0xbdcc51b92d06c53cULL,
  0xbdcc51b92d06c53cULL, 0xfe7234c8a3b56245ULL, 0xfe7234c8a3b56245ULL,
  0x5a7fbf967e5d4cc5ULL, 0x5a7fbf967e5d4cc5ULL, 0x8bce0be85eb37f17ULL,
  0x4997e56ff6446046ULL, 0xf2f5310fdb9a78d0ULL, 0x05fce947dd601f1cULL,
  0x7a808d1b3dc6a24dULL, 0x7a808d1b3dc6a24dULL, 0xc78fce96459c2123ULL,
  0xc78fce96459c2123ULL, 0x624bdadddf537b4aULL, 0x624bdadddf537b4aULL,
  0x129c92c89dc92530ULL, 0x129c92c89dc92530ULL, 0xe98879b8a972580fULL,
  0xa31e7d02009c8984ULL, 0x96e5bab40d88315bULL, 0xe28745f95cc85de6ULL,
  0x1b69cb09e45eae04ULL, 0x1b69cb09e45eae04ULL, 0xf15d4d03d1a97196ULL,
  0xf15d4d03d1a97196ULL, 0x887681e2643da18bULL, 0x887681e2643da18bULL,
  0x9c74eb1cdb21bde1ULL, 0x9c74eb1cdb21bde1ULL, 0xf6f8834dbab154a2ULL,
  0x01df8662baecd09aULL, 0x07d011b6607c9327ULL, 0xcde832154a25ededULL
};

static void _anchored_routes_are_unchanged_by_free_ends(void **state)
{
  (void)state;
  for(int index = 0; index < GOLDEN_ROUTE_CASES; index++)
  {
    dt_canvas_object_t *connector = NULL;
    dt_canvas_t *canvas = _golden_route_scene(index, &connector);
    dt_canvas_route_t route;
    assert_true(dt_canvas_connector_route(canvas, connector, &route));
    const uint64_t hash = _golden_route_hash(&route);
    if(hash != _golden_route_hashes[index])
      fail_msg("anchored route %d (pair %d, anchors %d, routing %u, via %u) moved: 0x%016llx, was 0x%016llx", index,
               index / 36, (index / 12) % 3, route.routing, connector->connector.via_count, (unsigned long long)hash,
               (unsigned long long)_golden_route_hashes[index]);

    // Each anchored end still aims at the other frame's centre, as it did before free ends.
    const dt_canvas_object_t *from = dt_canvas_find_object(canvas, connector->connector.from_id);
    const dt_canvas_object_t *to = dt_canvas_find_object(canvas, connector->connector.to_id);
    double anchor_x = 0.0;
    double anchor_y = 0.0;
    double normal_x = 0.0;
    double normal_y = 0.0;
    dt_canvas_object_anchor_point(canvas, from, (dt_canvas_anchor_t)connector->connector.from_anchor, to->x, to->y,
                                  &anchor_x, &anchor_y, &normal_x, &normal_y);
    assert_near(route.from_x, anchor_x, 1e-9);
    assert_near(route.from_y, anchor_y, 1e-9);
    assert_near(route.from_normal_x, normal_x, 1e-12);
    assert_near(route.from_normal_y, normal_y, 1e-12);
    dt_canvas_object_anchor_point(canvas, to, (dt_canvas_anchor_t)connector->connector.to_anchor, from->x, from->y,
                                  &anchor_x, &anchor_y, &normal_x, &normal_y);
    assert_near(route.to_x, anchor_x, 1e-9);
    assert_near(route.to_y, anchor_y, 1e-9);
    assert_near(route.to_normal_x, normal_x, 1e-12);
    assert_near(route.to_normal_y, normal_y, 1e-12);

    // A free end's point and tangent are read only while its id is 0: on a connector anchored at
    // both ends, whatever those fields hold routes to the very same bits.
    connector->connector.from_x = 1234.5;
    connector->connector.from_y = -987.25;
    connector->connector.to_x = -55.5;
    connector->connector.to_y = 77.75;
    connector->connector.from_tangent_x = 13.0f;
    connector->connector.from_tangent_y = -7.0f;
    connector->connector.to_tangent_x = 21.0f;
    connector->connector.to_tangent_y = 3.0f;
    dt_canvas_route_t with_junk;
    assert_true(dt_canvas_connector_route(canvas, connector, &with_junk));
    if(memcmp(&route, &with_junk, sizeof(route)) != 0)
      fail_msg("anchored route %d reads the free-end fields", index);
    dt_canvas_free(canvas);
  }
}

/** A line: a connector with both ends at their own points, routed through the same resolver. */
static void _a_free_line_routes_between_its_own_points(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *line = dt_canvas_add_line(canvas, -40.0, 25.0, 160.0, 75.0, DT_CANVAS_ROUTING_STRAIGHT, NULL);
  assert_non_null(line);
  assert_int_equal(line->kind, DT_CANVAS_OBJECT_CONNECTOR);
  assert_int_equal(line->connector.from_id, 0);
  assert_int_equal(line->connector.to_id, 0);
  assert_true(dt_canvas_connector_has_free_end(line));
  dt_canvas_route_t route;
  assert_true(dt_canvas_connector_route(canvas, line, &route));
  assert_int_equal(route.point_count, 2);
  assert_near(route.points[0], -40.0, 1e-12);
  assert_near(route.points[1], 25.0, 1e-12);
  assert_near(route.points[2], 160.0, 1e-12);
  assert_near(route.points[3], 75.0, 1e-12);
  // Each free end leaves toward the other.
  const double length = hypot(200.0, 50.0);
  assert_near(route.from_normal_x, 200.0 / length, 1e-12);
  assert_near(route.from_normal_y, 50.0 / length, 1e-12);
  assert_near(route.to_normal_x, -200.0 / length, 1e-12);
  assert_near(route.to_normal_y, -50.0 / length, 1e-12);
  assert_true(dt_canvas_object_contains(canvas, line, 60.0, 50.0, 1.0));
  assert_false(dt_canvas_object_contains(canvas, line, 60.0, 80.0, 1.0));

  // A new line takes the style it is handed, and a connector anchored at both ends is not free.
  dt_canvas_line_style_t style = dt_canvas_line_style_default();
  style.line_width = 6.5f;
  style.color = dt_canvas_color(0.1f, 0.2f, 0.3f, 0.4f);
  style.dashed = TRUE;
  style.arrow_start = TRUE;
  style.arrow_end = FALSE;
  dt_canvas_object_t *styled = dt_canvas_add_line(canvas, 0.0, 0.0, 10.0, 0.0, DT_CANVAS_ROUTING_SQUARE, &style);
  assert_int_equal(styled->connector.routing, DT_CANVAS_ROUTING_SQUARE);
  assert_int_equal(styled->connector.style, DT_CANVAS_CONNECTOR_DASHED | DT_CANVAS_CONNECTOR_ARROW_START);
  dt_canvas_line_style_t read_back;
  assert_true(dt_canvas_line_style_get(styled, &read_back));
  assert_true(read_back.line_width == 6.5f);
  assert_true(read_back.color.blue == 0.3f);
  assert_true(read_back.dashed);
  assert_true(read_back.arrow_start);
  assert_false(read_back.arrow_end);
  dt_canvas_object_t *left = dt_canvas_add_text(canvas, 0.0, 400.0, 100.0, 100.0, "");
  dt_canvas_object_t *right = dt_canvas_add_text(canvas, 400.0, 400.0, 100.0, 100.0, "");
  dt_canvas_object_t *anchored = dt_canvas_add_connector(canvas, left->id, right->id);
  assert_false(dt_canvas_connector_has_free_end(anchored));
  assert_false(dt_canvas_connector_has_free_end(left));
  // The connector and the line nobody styled are born alike, so the two cannot drift apart.
  const dt_canvas_line_style_t defaults = dt_canvas_line_style_default();
  assert_true(dt_canvas_line_style_get(anchored, &read_back));
  assert_true(memcmp(&read_back, &defaults, sizeof(defaults)) == 0);
  dt_canvas_free(canvas);
}

/**
 * A style comes back from the atelier's memory of the last line, which is a configuration file
 * anything may have written: it is made into a style a line can be born with before it is used, and
 * a line born from a wild one holds the sound values rather than the wild ones.
 */
static void _a_line_style_from_outside_is_made_sound(void **state)
{
  (void)state;
  dt_canvas_line_style_t style = dt_canvas_line_style_default();
  const dt_canvas_line_style_t untouched = style;
  // What the atelier itself writes is already sound, and sanitising it changes nothing at all.
  assert_true(dt_canvas_line_style_sanitize(&style));
  assert_true(memcmp(&style, &untouched, sizeof(style)) == 0);

  /* A width that is not a number is NONE of it, which for a line means the canvas's -- the
   * careful answer, and the same one an empty remembered colour gives. Held to the range
   * otherwise. */
  style.line_width = NAN;
  assert_false(dt_canvas_line_style_sanitize(&style));
  assert_true(style.line_width == 0.0f);
  style.line_width = -3.0f;
  assert_false(dt_canvas_line_style_sanitize(&style));
  assert_true(style.line_width == 0.0f);
  style.line_width = 500.0f;
  assert_false(dt_canvas_line_style_sanitize(&style));
  assert_true(style.line_width == DT_CANVAS_LINE_WIDTH_MAX);
  // Zero is a width: the painter draws it two units wide, as a connector's stored zero is drawn.
  style.line_width = 0.0f;
  assert_true(dt_canvas_line_style_sanitize(&style));
  assert_true(style.line_width == 0.0f);

  // A damaged colour stays a colour: each channel is held to what a channel can be.
  style.color = dt_canvas_color(1.5f, -0.2f, 0.5f, 2.0f);
  style.color.green = -0.2f;
  assert_false(dt_canvas_line_style_sanitize(&style));
  assert_true(style.color.red == 1.0f);
  assert_true(style.color.green == 0.0f);
  assert_true(style.color.blue == 0.5f);
  assert_true(style.color.alpha == 1.0f);
  style.color.blue = NAN;
  assert_false(dt_canvas_line_style_sanitize(&style));
  assert_true(style.color.blue == 0.0f);

  // A switch read back as some other non-zero number answers TRUE, which is what it is compared to.
  style.dashed = 2;
  style.arrow_start = -1;
  assert_false(dt_canvas_line_style_sanitize(&style));
  assert_int_equal(style.dashed, TRUE);
  assert_int_equal(style.arrow_start, TRUE);
  assert_false(dt_canvas_line_style_sanitize(NULL));

  // A line born from a wild style holds the sound one, so nothing downstream sees the wild values.
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_line_style_t wild = dt_canvas_line_style_default();
  wild.line_width = 4000.0f;
  wild.color = dt_canvas_color(-1.0f, 0.25f, 0.5f, 1.0f);
  dt_canvas_object_t *line = dt_canvas_add_line(canvas, 0.0, 0.0, 50.0, 0.0, DT_CANVAS_ROUTING_STRAIGHT, &wild);
  assert_non_null(line);
  assert_true(line->connector.line_width == DT_CANVAS_LINE_WIDTH_MAX);
  assert_true(line->connector.color.red == 0.0f);
  assert_true(line->connector.color.green == 0.25f);
  dt_canvas_free(canvas);
}

/**
 * Two free ends in the same place: every routing gives the axis normals -- the start leaving
 * rightward, the end arriving from the left -- and a polyline that stays on the point, where the
 * anchored ends' floors would have drawn a 40-unit dash out of a line of no length.
 */
static void _coincident_free_ends_route_to_a_finite_point(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  const dt_canvas_routing_t routings[3]
      = { DT_CANVAS_ROUTING_STRAIGHT, DT_CANVAS_ROUTING_SQUARE, DT_CANVAS_ROUTING_CUBIC };
  for(int idx = 0; idx < 3; idx++)
  {
    dt_canvas_object_t *line = dt_canvas_add_line(canvas, 12.0, -7.0, 12.0, -7.0, routings[idx], NULL);
    dt_canvas_route_t route;
    assert_true(dt_canvas_connector_route(canvas, line, &route));
    assert_true(route.point_count >= 2);
    assert_true(route.from_normal_x == 1.0 && route.from_normal_y == 0.0);
    assert_true(route.to_normal_x == -1.0 && route.to_normal_y == 0.0);
    for(int point = 0; point < route.point_count; point++)
    {
      assert_near(route.points[2 * point], 12.0, 1e-9);
      assert_near(route.points[2 * point + 1], -7.0, 1e-9);
    }
    if(routings[idx] == DT_CANVAS_ROUTING_CUBIC)
    {
      // Nothing to seed from.
      assert_true(line->connector.from_tangent_x == 0.0f && line->connector.to_tangent_y == 0.0f);
      assert_near(route.control1_x, 12.0, 1e-9);
      assert_near(route.control2_y, -7.0, 1e-9);
    }
  }
  dt_canvas_free(canvas);
}

/** One end on a frame, one free: the anchored end aims at the free point, not at a frame. */
static void _a_half_free_connector_aims_its_anchor_at_the_free_point(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *frame = dt_canvas_add_text(canvas, 0.0, 0.0, 200.0, 200.0, "");
  dt_canvas_object_t *connector = dt_canvas_add_line(canvas, 0.0, 0.0, 0.0, 0.0, DT_CANVAS_ROUTING_STRAIGHT, NULL);
  connector->connector.from_id = frame->id;
  connector->connector.from_anchor = DT_CANVAS_ANCHOR_CENTRE;
  connector->connector.to_x = 0.0;
  connector->connector.to_y = 500.0;
  // It owns its far point, so it has a free end -- but it is not a LINE: the frame it holds is what
  // it is for. The two questions are asked of different callers and must not answer alike.
  assert_true(dt_canvas_connector_has_free_end(connector));
  assert_false(dt_canvas_connector_is_line(connector));
  assert_true(dt_canvas_connector_is_line(dt_canvas_add_line(canvas, 0.0, 0.0, 10.0, 10.0,
                                                             DT_CANVAS_ROUTING_STRAIGHT, NULL)));
  assert_false(dt_canvas_connector_is_line(frame));
  assert_false(dt_canvas_connector_is_line(NULL));
  dt_canvas_route_t route;
  assert_true(dt_canvas_connector_route(canvas, connector, &route));
  // Straight down from the centre, out through the bottom edge.
  assert_near(route.from_x, 0.0, 1e-9);
  assert_near(route.from_y, 100.0, 1e-9);
  assert_near(route.from_normal_y, 1.0, 1e-12);
  assert_near(route.to_y, 500.0, 1e-12);
  connector->connector.to_x = -500.0;
  connector->connector.to_y = 0.0;
  assert_true(dt_canvas_connector_route(canvas, connector, &route));
  assert_near(route.from_x, -100.0, 1e-9);
  assert_near(route.from_y, 0.0, 1e-9);
  // AUTO takes the cardinal nearest the free point too.
  connector->connector.from_anchor = DT_CANVAS_ANCHOR_AUTO;
  connector->connector.to_x = 20.0;
  connector->connector.to_y = -900.0;
  assert_true(dt_canvas_connector_route(canvas, connector, &route));
  assert_near(route.from_y, -100.0, 1e-9);
  // The anchored end still refuses a frame that is gone.
  connector->connector.from_id = 999;
  assert_false(dt_canvas_connector_route(canvas, connector, &route));
  dt_canvas_free(canvas);
}

/** No point of a route runs past the box of its own two ends, grown by `margin` across the chord. */
static void _assert_route_between_its_ends(const dt_canvas_route_t *route, const double margin)
{
  const double low_x = fmin(route->from_x, route->to_x);
  const double high_x = fmax(route->from_x, route->to_x);
  const double low_y = fmin(route->from_y, route->to_y) - margin;
  const double high_y = fmax(route->from_y, route->to_y) + margin;
  for(int point = 0; point < route->point_count; point++)
  {
    const double x = route->points[2 * point];
    const double y = route->points[2 * point + 1];
    if(x < low_x - 1e-9 || x > high_x + 1e-9 || y < low_y - 1e-9 || y > high_y + 1e-9)
      fail_msg("route point %d (%g, %g) runs past its ends' box [%g, %g] x [%g, %g]", point, x, y, low_x, high_x,
               low_y, high_y);
  }
}

/** A free end's own tangent is its control point, less the float it is stored in. */
static void _a_free_tangent_places_the_control_point(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *line = dt_canvas_add_line(canvas, 10.0, 20.0, 310.0, 20.0, DT_CANVAS_ROUTING_CUBIC, NULL);
  line->connector.from_tangent_x = 30.0f;
  line->connector.from_tangent_y = 80.0f;
  line->connector.to_tangent_x = -45.5f;
  line->connector.to_tangent_y = -12.25f;
  dt_canvas_route_t route;
  assert_true(dt_canvas_connector_route(canvas, line, &route));
  assert_near(route.control1_x, 10.0 + 30.0, 1e-4);
  assert_near(route.control1_y, 20.0 + 80.0, 1e-4);
  assert_near(route.control2_x, 310.0 - 45.5, 1e-4);
  assert_near(route.control2_y, 20.0 - 12.25, 1e-4);
  // An automatic tangent heads for the other end at 0.4 of the chord, however short the line,
  // where an anchored end's floor would have put it 40 units out.
  dt_canvas_object_t *short_line = dt_canvas_add_line(canvas, 0.0, 0.0, 10.0, 0.0, DT_CANVAS_ROUTING_CUBIC, NULL);
  short_line->connector.from_tangent_x = 0.0f;
  short_line->connector.from_tangent_y = 0.0f;
  short_line->connector.to_tangent_x = 0.0f;
  short_line->connector.to_tangent_y = 0.0f;
  assert_true(dt_canvas_connector_route(canvas, short_line, &route));
  assert_near(route.control1_x, 4.0, 1e-5);
  assert_near(route.control1_y, 0.0, 1e-12);
  assert_near(route.control2_x, 6.0, 1e-5);
  // Through a waypoint, each free end heads for the waypoint instead.
  short_line->connector.via_count = 1;
  short_line->connector.via_x = 5.0;
  short_line->connector.via_y = 50.0;
  assert_true(dt_canvas_connector_route(canvas, short_line, &route));
  const double to_via = hypot(5.0, 50.0);
  assert_near(route.from_normal_x, 5.0 / to_via, 1e-12);
  assert_near(route.from_normal_y, 50.0 / to_via, 1e-12);
  assert_near(route.to_normal_x, -5.0 / to_via, 1e-12);
  assert_near(route.control1_y, 0.4 * 50.0, 1e-4);

  // The waypoint's own automatic tangent takes no floor either. Added where adding puts it, on
  // the middle of the route, a short line's curve stays between its ends; before, the tangent
  // was 40 units and threw both halves out past them.
  short_line->connector.via_count = 0;
  dt_canvas_connector_add_via(canvas, short_line);
  assert_near(short_line->connector.via_x, 5.0, 1e-9);
  assert_true(dt_canvas_connector_route(canvas, short_line, &route));
  assert_near(route.control2_x, 5.0 - 2.0, 1e-9);
  assert_near(route.control3_x, 5.0 + 2.0, 1e-9);
  _assert_route_between_its_ends(&route, 0.0);
  // Near one end, the tangent is the shorter leg's share, or the curve runs past that end.
  short_line->connector.via_x = 9.0;
  assert_true(dt_canvas_connector_route(canvas, short_line, &route));
  assert_near(route.control3_x, 9.0 + 0.4, 1e-9);
  _assert_route_between_its_ends(&route, 0.0);
  // A seeded arc keeps its steered tangents through a waypoint added on its middle.
  dt_canvas_object_t *arc = dt_canvas_add_line(canvas, 0.0, 0.0, 10.0, 0.0, DT_CANVAS_ROUTING_CUBIC, NULL);
  dt_canvas_connector_add_via(canvas, arc);
  assert_true(dt_canvas_connector_route(canvas, arc, &route));
  _assert_route_between_its_ends(&route, 4.0);

  // A short free SQUARE line: its stubs meet in the middle instead of each running 20 units past
  // the other end.
  dt_canvas_object_t *square = dt_canvas_add_line(canvas, 0.0, 0.0, 10.0, 0.0, DT_CANVAS_ROUTING_SQUARE, NULL);
  assert_true(dt_canvas_connector_route(canvas, square, &route));
  _assert_route_between_its_ends(&route, 0.0);
  // A long one keeps the anchored stubs, which never reach halfway.
  square->connector.to_x = 1000.0;
  assert_true(dt_canvas_connector_route(canvas, square, &route));
  assert_near(route.points[2], 60.0, 1e-9);
  // Through a waypoint, a stub stops halfway to it.
  square->connector.to_x = 100.0;
  square->connector.via_count = 1;
  square->connector.via_x = 0.0;
  square->connector.via_y = 16.0;
  assert_true(dt_canvas_connector_route(canvas, square, &route));
  assert_near(route.points[2], 0.0, 1e-9);
  assert_near(route.points[3], 8.0, 1e-9);
  // A connector anchored at one end keeps that end's floor: the stub clears the frame.
  dt_canvas_object_t *frame = dt_canvas_add_text(canvas, 0.0, 0.0, 100.0, 100.0, "");
  dt_canvas_object_t *half = dt_canvas_add_line(canvas, 0.0, 0.0, 0.0, 60.0, DT_CANVAS_ROUTING_SQUARE, NULL);
  half->connector.from_id = frame->id;
  half->connector.from_anchor = DT_CANVAS_ANCHOR_SOUTH;
  assert_true(dt_canvas_connector_route(canvas, half, &route));
  assert_near(route.from_y, 50.0, 1e-9);
  assert_near(route.points[3], 50.0 + 20.0, 1e-9);
  assert_near(route.points[2 * (route.point_count - 2) + 1], 55.0, 1e-9);
  dt_canvas_free(canvas);
}

/** A cubic line is born an arc: symmetric control points either side, its middle off the chord. */
static void _a_seeded_curve_is_a_symmetric_arc(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *curve = dt_canvas_add_line(canvas, 0.0, 0.0, 100.0, 0.0, DT_CANVAS_ROUTING_CUBIC, NULL);
  dt_canvas_route_t route;
  assert_true(dt_canvas_connector_route(canvas, curve, &route));
  // 0.4 of the chord at 30 degrees: (34.64, -20) and (65.36, -20), bulging up on screen.
  const double along = 40.0 * cos(M_PI / 6.0);
  assert_near(route.control1_x, along, 1e-4);
  assert_near(route.control1_y, -20.0, 1e-4);
  assert_near(route.control2_x, 100.0 - along, 1e-4);
  assert_near(route.control2_y, -20.0, 1e-4);
  const int middle = (route.point_count - 1) / 2;
  const double middle_x = 0.5 * (route.points[2 * middle] + route.points[2 * (route.point_count - 1 - middle)]);
  const double middle_y = 0.5 * (route.points[2 * middle + 1] + route.points[2 * (route.point_count - 1 - middle) + 1]);
  assert_near(middle_x, 50.0, 1e-3);
  assert_true(middle_y < -10.0);
  const float seeded_to_x = curve->connector.to_tangent_x;
  const float seeded_to_y = curve->connector.to_tangent_y;

  // Nothing left to seed: both tangents steered.
  curve->connector.from_tangent_x = 3.0f;
  curve->connector.from_tangent_y = 4.0f;
  assert_false(dt_canvas_connector_seed_curve(curve, &route));
  assert_true(curve->connector.from_tangent_x == 3.0f && curve->connector.from_tangent_y == 4.0f);
  assert_true(curve->connector.to_tangent_x == seeded_to_x && curve->connector.to_tangent_y == seeded_to_y);
  // One steered, one automatic: only the automatic one is seeded, and the steered one is kept.
  curve->connector.to_tangent_x = 0.0f;
  curve->connector.to_tangent_y = 0.0f;
  assert_true(dt_canvas_connector_seed_curve(curve, &route));
  assert_true(curve->connector.from_tangent_x == 3.0f && curve->connector.from_tangent_y == 4.0f);
  assert_true(curve->connector.to_tangent_x == seeded_to_x && curve->connector.to_tangent_y == seeded_to_y);
  // An anchored end is its frame's: a half-free cubic seeds its free end alone.
  dt_canvas_object_t *frame = dt_canvas_add_text(canvas, -300.0, 0.0, 100.0, 100.0, "");
  dt_canvas_object_t *half = dt_canvas_add_line(canvas, 0.0, 0.0, 100.0, 0.0, DT_CANVAS_ROUTING_STRAIGHT, NULL);
  half->connector.from_id = frame->id;
  half->connector.routing = DT_CANVAS_ROUTING_CUBIC;
  assert_true(dt_canvas_connector_route(canvas, half, &route));
  assert_true(dt_canvas_connector_seed_curve(half, &route));
  assert_true(half->connector.from_tangent_x == 0.0f && half->connector.from_tangent_y == 0.0f);
  const double chord_x = route.to_x - route.from_x;
  assert_near(half->connector.to_tangent_x, -0.4 * cos(M_PI / 6.0) * chord_x, 1e-4);
  assert_near(half->connector.to_tangent_y, -0.4 * sin(M_PI / 6.0) * chord_x, 1e-4);
  // Neither a straight line nor a curve through a waypoint is seeded.
  dt_canvas_object_t *straight = dt_canvas_add_line(canvas, 0.0, 0.0, 100.0, 0.0, DT_CANVAS_ROUTING_STRAIGHT, NULL);
  assert_true(dt_canvas_connector_route(canvas, straight, &route));
  assert_false(dt_canvas_connector_seed_curve(straight, &route));
  straight->connector.routing = DT_CANVAS_ROUTING_CUBIC;
  straight->connector.via_count = 1;
  assert_false(dt_canvas_connector_seed_curve(straight, &route));
  straight->connector.via_count = 0;
  assert_true(dt_canvas_connector_seed_curve(straight, &route));
  assert_near(straight->connector.to_tangent_y, -20.0, 1e-4);
  dt_canvas_free(canvas);
}

/** The free ends reach the disk and come back, beside every connector field that was there before. */
static void _free_ends_round_trip_through_the_index(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *frame = dt_canvas_add_text(canvas, 0.0, 0.0, 100.0, 100.0, "");
  dt_canvas_object_t *line = dt_canvas_add_line(canvas, -12.5, 7.25, 1234.5, -987.125, DT_CANVAS_ROUTING_CUBIC, NULL);
  line->connector.from_tangent_x = 11.5f;
  line->connector.from_tangent_y = -3.25f;
  line->connector.to_tangent_x = -0.5f;
  line->connector.to_tangent_y = 99.0f;
  line->connector.via_count = 1;
  line->connector.via_x = 400.0;
  line->connector.via_y = -300.0;
  line->connector.reserved[DT_CANVAS_CONNECTOR_RESERVED - 1] = 0x5A;
  dt_canvas_object_t *half = dt_canvas_add_line(canvas, 0.0, 0.0, 800.0, 90.0, DT_CANVAS_ROUTING_SQUARE, NULL);
  half->connector.from_id = frame->id;
  GBytes *index = dt_canvas_format_write_index(canvas);
  dt_canvas_t *restored = dt_canvas_new();
  assert_true(dt_canvas_format_read_index(restored, index, NULL));
  g_bytes_unref(index);
  const dt_canvas_object_t *line_back = dt_canvas_find_object(restored, line->id);
  assert_non_null(line_back);
  assert_int_equal(line_back->connector.from_id, 0);
  assert_int_equal(line_back->connector.to_id, 0);
  assert_true(line_back->connector.from_x == -12.5);
  assert_true(line_back->connector.from_y == 7.25);
  assert_true(line_back->connector.to_x == 1234.5);
  assert_true(line_back->connector.to_y == -987.125);
  assert_true(line_back->connector.from_tangent_x == 11.5f);
  assert_true(line_back->connector.from_tangent_y == -3.25f);
  assert_true(line_back->connector.to_tangent_x == -0.5f);
  assert_true(line_back->connector.to_tangent_y == 99.0f);
  assert_int_equal(line_back->connector.via_count, 1);
  assert_int_equal(line_back->connector.reserved[DT_CANVAS_CONNECTOR_RESERVED - 1], 0x5A);
  const dt_canvas_object_t *half_back = dt_canvas_find_object(restored, half->id);
  assert_int_equal(half_back->connector.from_id, frame->id);
  assert_int_equal(half_back->connector.to_id, 0);
  assert_true(half_back->connector.to_x == 800.0);
  assert_int_equal(half_back->connector.routing, DT_CANVAS_ROUTING_SQUARE);
  // Routed from the file as from memory.
  dt_canvas_route_t before;
  dt_canvas_route_t after;
  assert_true(dt_canvas_connector_route(canvas, line, &before));
  assert_true(dt_canvas_connector_route(restored, line_back, &after));
  assert_true(memcmp(&before, &after, sizeof(before)) == 0);
  assert_true(dt_canvas_connector_route(canvas, half, &before));
  assert_true(dt_canvas_connector_route(restored, half_back, &after));
  assert_true(memcmp(&before, &after, sizeof(before)) == 0);
  dt_canvas_free(restored);
  dt_canvas_free(canvas);
}

static uint32_t _index_u32(const uint8_t *bytes)
{
  return bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/** Where the record of the object with `id` starts in an index, or 0 when there is none. */
static gsize _index_record_of(const uint8_t *data, const gsize size, const uint32_t id)
{
  gsize position = _index_u32(data + 12);
  while(position + 12 <= size)
  {
    const uint32_t record_size = _index_u32(data + position + 4);
    if(_index_u32(data + position + 8) == id) return position;
    if(record_size < 12) return 0;
    position += record_size;
  }
  return 0;
}

/**
 * A connector anchored at both ends is written byte for byte as it was before free ends: the
 * record keeps its size -- 496 bytes, measured on the writer as it stood -- and the 72 bytes that
 * were reserved then, which now start with the free ends, are still zeros. A file from before free
 * ends therefore holds exactly what this writer writes, and reads back to the same route.
 */
static void _an_anchored_connector_is_written_as_before_free_ends(void **state)
{
  (void)state;
  assert_int_equal(sizeof(dt_canvas_connector_t), 160);
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *left = dt_canvas_add_text(canvas, 0.0, 0.0, 100.0, 100.0, "a");
  dt_canvas_object_t *right = dt_canvas_add_text(canvas, 400.0, 0.0, 100.0, 100.0, "b");
  dt_canvas_object_t *connector = dt_canvas_add_connector(canvas, left->id, right->id);
  connector->connector.via_count = 1;
  connector->connector.via_x = 200.0;
  connector->connector.via_y = 300.0;
  connector->connector.from_reach = 50.0f;
  connector->connector.via_tangent_x = 20.0;
  GBytes *index = dt_canvas_format_write_index(canvas);
  gsize size = 0;
  const uint8_t *data = g_bytes_get_data(index, &size);
  const gsize record = _index_record_of(data, size, connector->id);
  assert_true(record > 0);
  assert_int_equal(_index_u32(data + record), DT_CANVAS_OBJECT_CONNECTOR);
  const uint32_t record_size = _index_u32(data + record + 4);
  assert_int_equal(record_size, 496);
  for(uint32_t byte = record_size - 72; byte < record_size; byte++)
  {
    if(data[record + byte] != 0) fail_msg("byte %u of the connector record is %u, not 0", byte, data[record + byte]);
  }
  dt_canvas_t *restored = dt_canvas_new();
  assert_true(dt_canvas_format_read_index(restored, index, NULL));
  dt_canvas_route_t before;
  dt_canvas_route_t after;
  assert_true(dt_canvas_connector_route(canvas, connector, &before));
  assert_true(dt_canvas_connector_route(restored, dt_canvas_find_object(restored, connector->id), &after));
  assert_true(memcmp(&before, &after, sizeof(before)) == 0);
  g_bytes_unref(index);
  dt_canvas_free(restored);
  dt_canvas_free(canvas);
}

/**
 * No object may carry the id 0, which is what a free end holds: a file that gives one an object is
 * refused that object on load, and asking to remove id 0 removes nothing -- not every line whose
 * end is free.
 */
static void _no_object_loads_with_the_id_a_free_end_holds(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *frame = dt_canvas_add_text(canvas, 0.0, 0.0, 100.0, 100.0, "");
  dt_canvas_object_t *line = dt_canvas_add_line(canvas, 0.0, 200.0, 300.0, 200.0, DT_CANVAS_ROUTING_STRAIGHT, NULL);
  const uint32_t frame_id = frame->id;
  const uint32_t line_id = line->id;
  GBytes *index = dt_canvas_format_write_index(canvas);
  gsize size = 0;
  uint8_t *data = g_bytes_unref_to_data(index, &size);
  const gsize record = _index_record_of(data, size, frame_id);
  assert_true(record > 0);
  memset(data + record + 8, 0, 4);
  GBytes *patched = g_bytes_new_take(data, size);
  dt_canvas_t *restored = dt_canvas_new();
  assert_true(dt_canvas_format_read_index(restored, patched, NULL));
  g_bytes_unref(patched);
  assert_int_equal(dt_canvas_object_count(restored), 1);
  assert_non_null(dt_canvas_find_object(restored, line_id));
  assert_false(dt_canvas_remove_object(restored, 0));
  assert_int_equal(dt_canvas_object_count(restored), 1);
  // An id-0 object put in memory by hand is not a way around it either.
  dt_canvas_object_t *stray = dt_canvas_add_text(restored, 0.0, 0.0, 10.0, 10.0, "");
  stray->id = 0;
  assert_false(dt_canvas_remove_object(restored, 0));
  assert_non_null(dt_canvas_find_object(restored, line_id));
  dt_canvas_free(restored);
  dt_canvas_free(canvas);
}

/** A line is copied whole and offset; a connector anchored to frames is not the copy's to make. */
static void _duplicating_a_line_offsets_its_points_and_waypoint(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *line = dt_canvas_add_line(canvas, 10.0, 20.0, 110.0, 70.0, DT_CANVAS_ROUTING_CUBIC, NULL);
  line->connector.via_count = 1;
  line->connector.via_x = 60.0;
  line->connector.via_y = -30.0;
  const uint32_t line_id = line->id;
  dt_canvas_object_t *copy = dt_canvas_duplicate_object(canvas, line_id);
  assert_non_null(copy);
  line = dt_canvas_find_object(canvas, line_id);
  assert_int_not_equal(copy->id, line_id);
  assert_int_equal(copy->connector.from_id, 0);
  assert_int_equal(copy->connector.to_id, 0);
  const double offset = copy->connector.from_x - line->connector.from_x;
  assert_true(offset > 0.0);
  assert_near(copy->connector.from_y - line->connector.from_y, offset, 1e-12);
  assert_near(copy->connector.to_x - line->connector.to_x, offset, 1e-12);
  assert_near(copy->connector.to_y - line->connector.to_y, offset, 1e-12);
  assert_near(copy->connector.via_x - line->connector.via_x, offset, 1e-12);
  assert_near(copy->connector.via_y - line->connector.via_y, offset, 1e-12);
  assert_true(copy->connector.from_tangent_x == line->connector.from_tangent_x);
  assert_true(copy->z > line->z);

  // Translation moves what a connector owns, each axis by its own offset: both points of a line...
  dt_canvas_connector_translate(copy, -5.0, 2.5);
  assert_near(copy->connector.from_x, 10.0 + offset - 5.0, 1e-12);
  assert_near(copy->connector.from_y, 20.0 + offset + 2.5, 1e-12);
  assert_near(copy->connector.to_x, 110.0 + offset - 5.0, 1e-12);
  assert_near(copy->connector.to_y, 70.0 + offset + 2.5, 1e-12);
  assert_near(copy->connector.via_x, 60.0 + offset - 5.0, 1e-12);
  assert_near(copy->connector.via_y, -30.0 + offset + 2.5, 1e-12);
  // ...nothing of a connector anchored at both ends, whose frames move it...
  dt_canvas_object_t *left = dt_canvas_add_text(canvas, 0.0, 400.0, 100.0, 100.0, "");
  dt_canvas_object_t *right = dt_canvas_add_text(canvas, 400.0, 400.0, 100.0, 100.0, "");
  dt_canvas_object_t *anchored = dt_canvas_add_connector(canvas, left->id, right->id);
  anchored->connector.via_count = 1;
  anchored->connector.via_x = 200.0;
  dt_canvas_connector_translate(anchored, 50.0, 50.0);
  assert_true(anchored->connector.via_x == 200.0);
  assert_null(dt_canvas_duplicate_object(canvas, anchored->id));
  // ...and only the free end of a half line, with the waypoint, leaving the anchored end's fields be.
  anchored->connector.to_id = 0;
  anchored->connector.to_x = 700.0;
  anchored->connector.to_y = -3.0;
  anchored->connector.from_x = 11.0;
  anchored->connector.from_y = 13.0;
  dt_canvas_connector_translate(anchored, 50.0, 7.0);
  assert_true(anchored->connector.to_x == 750.0);
  assert_true(anchored->connector.to_y == 4.0);
  assert_true(anchored->connector.via_x == 250.0);
  assert_true(anchored->connector.from_x == 11.0);
  assert_true(anchored->connector.from_y == 13.0);
  assert_null(dt_canvas_duplicate_object(canvas, anchored->id));

  // Removing frames never takes a line with them: no frame has the id a free end holds.
  const guint count = dt_canvas_object_count(canvas);
  assert_true(dt_canvas_remove_object(canvas, right->id));
  assert_int_equal(dt_canvas_object_count(canvas), count - 1);
  assert_non_null(dt_canvas_find_object(canvas, line_id));
  assert_non_null(dt_canvas_find_object(canvas, anchored->id));
  assert_true(dt_canvas_remove_object(canvas, left->id));
  assert_int_equal(dt_canvas_object_count(canvas), count - 3);
  assert_non_null(dt_canvas_find_object(canvas, line_id));
  dt_canvas_free(canvas);
}

/**
 * A dragged end snaps to the grid on both axes when nothing is held. Held to an angle step it lies
 * where the pointer projects onto the nearest step's direction from the other end: exactly level or
 * plumb on an axis, where it still snaps along the axis it moves on, and off the grid on a
 * diagonal, whose angle is kept to the bit. A pointer on the other end is left there.
 */
static void _a_dragged_line_end_snaps_to_the_grid_or_holds_its_angle(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  canvas->grid_size = 20.0f;
  canvas->grid_flags &= ~DT_CANVAS_GRID_SNAP;
  double x = 133.0;
  double y = 47.0;
  dt_canvas_constrain_line_end(canvas, 3.0, 7.0, 0, &x, &y);
  assert_true(x == 133.0 && y == 47.0);
  canvas->grid_flags |= DT_CANVAS_GRID_SNAP;
  dt_canvas_constrain_line_end(canvas, 3.0, 7.0, 0, &x, &y);
  assert_true(x == 140.0 && y == 40.0);

  // 16.7 degrees off level from (3, 7): 45-degree steps make it level, at the origin's own height,
  // the pointer's x snapped to the grid.
  x = 133.0;
  y = 7.0 + 130.0 * tan(16.7 * M_PI / 180.0);
  dt_canvas_constrain_line_end(canvas, 3.0, 7.0, 45, &x, &y);
  assert_true(x == 140.0);
  assert_true(y == 7.0);
  // Straight up on screen, from beside it, and leftward.
  x = 10.0;
  y = -191.0;
  dt_canvas_constrain_line_end(canvas, 3.0, 7.0, 45, &x, &y);
  assert_true(x == 3.0);
  assert_true(y == -200.0);
  x = -251.0;
  y = 30.0;
  dt_canvas_constrain_line_end(canvas, 3.0, 7.0, 15, &x, &y);
  assert_true(x == -260.0);
  assert_true(y == 7.0);

  // 40 degrees below level: 45-degree steps put it on the diagonal, the pointer's projection onto
  // it, and the grid is left alone.
  const double pointer_angle = 40.0 * M_PI / 180.0;
  x = 3.0 + 200.0 * cos(pointer_angle);
  y = 7.0 + 200.0 * sin(pointer_angle);
  dt_canvas_constrain_line_end(canvas, 3.0, 7.0, 45, &x, &y);
  const double along = 200.0 * cos(5.0 * M_PI / 180.0);
  assert_near(x, 3.0 + along * M_SQRT1_2, 1e-9);
  assert_near(y, 7.0 + along * M_SQRT1_2, 1e-9);
  assert_near(atan2(y - 7.0, x - 3.0), M_PI_4, 1e-12);
  // 15-degree steps take the same pointer to 45 as well, and one at 22 degrees to 15.
  x = 3.0 + 200.0 * cos(22.0 * M_PI / 180.0);
  y = 7.0 + 200.0 * sin(22.0 * M_PI / 180.0);
  dt_canvas_constrain_line_end(canvas, 3.0, 7.0, 15, &x, &y);
  assert_near(atan2(y - 7.0, x - 3.0), 15.0 * M_PI / 180.0, 1e-12);
  assert_near(hypot(x - 3.0, y - 7.0), 200.0 * cos(7.0 * M_PI / 180.0), 1e-9);
  // And at 22 degrees 45-degree steps return to level.
  x = 3.0 + 200.0 * cos(22.0 * M_PI / 180.0);
  y = 7.0 + 200.0 * sin(22.0 * M_PI / 180.0);
  dt_canvas_constrain_line_end(canvas, 3.0, 7.0, 45, &x, &y);
  assert_true(y == 7.0);

  // On the other end itself there is no direction to hold.
  x = 3.0;
  y = 7.0;
  dt_canvas_constrain_line_end(canvas, 3.0, 7.0, 45, &x, &y);
  assert_true(x == 3.0 && y == 7.0);

  // The fact a tool drawing a line has to be written against: from an origin ON the grid, a pointer
  // that has travelled several times the drag threshold -- three screen pixels, three canvas units
  // at 1:1 -- comes back AT the origin, the cell being wider than the travel. A drawing that decided
  // "the pointer really moved" on the screen delta alone would make a line with no length at all.
  x = 44.0;
  y = 33.0;
  dt_canvas_constrain_line_end(canvas, 40.0, 40.0, 0, &x, &y);
  assert_true(x == 40.0 && y == 40.0);
  // The axis locks land on it too: level takes the origin's own height and snaps the pointer's x.
  x = 47.0;
  y = 41.0;
  dt_canvas_constrain_line_end(canvas, 40.0, 40.0, 45, &x, &y);
  assert_true(x == 40.0 && y == 40.0);
  dt_canvas_free(canvas);
}

/** The largest and smallest value one axis of a cubic takes, sampled finely enough to stand for the curve. */
static void _sampled_cubic_range(const double p0, const double p1, const double p2, const double p3, double *low,
                                 double *high)
{
  const int samples = 100000;
  for(int idx = 0; idx <= samples; idx++)
  {
    const double t = (double)idx / (double)samples;
    const double u = 1.0 - t;
    const double value = u * u * u * p0 + 3.0 * u * u * t * p1 + 3.0 * u * t * t * p2 + t * t * t * p3;
    *low = fmin(*low, value);
    *high = fmax(*high, value);
  }
}

/**
 * The canvas's bounds hold a line's ink and no more: the stroke grown by half its width, and each
 * arrowhead's own triangle where it has one; a connector between frames adds nothing.
 */
static void _bounds_hold_a_free_line_and_its_arrowhead(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_line_style_t style = dt_canvas_line_style_default();
  style.line_width = 4.0f;
  style.arrow_end = TRUE;
  style.arrow_start = FALSE;
  dt_canvas_object_t *line = dt_canvas_add_line(canvas, 0.0, 0.0, 300.0, 0.0, DT_CANVAS_ROUTING_STRAIGHT, &style);
  // The painter's boxes take one reach for the whole route, whichever end carries the head.
  const double arrow = dt_canvas_stroke_reach(4.0, DT_CANVAS_CONNECTOR_ARROW_END);
  assert_near(arrow, 4.0 + 14.0 * 2.0, 1e-12);
  assert_near(dt_canvas_stroke_reach(4.0, DT_CANVAS_CONNECTOR_ARROW_START), arrow, 1e-12);
  assert_near(dt_canvas_stroke_reach(4.0, DT_CANVAS_CONNECTOR_DASHED), 2.0, 1e-12);
  assert_near(dt_canvas_stroke_reach(0.0, DT_CANVAS_CONNECTOR_PLAIN), DT_CANVAS_CONNECTOR_LINE_WIDTH / 2.0, 1e-12);
  assert_true(dt_canvas_paint_arrow_reach(4.0) == arrow);
  // A head at the end, pointing right: its tip is the end, its base 28 units back and 10 either side.
  dt_canvas_rect_t bounds = dt_canvas_bounds(canvas);
  assert_near(bounds.x, -2.0, 1e-12);
  assert_near(bounds.y, -10.0, 1e-12);
  assert_near(bounds.width, 304.0, 1e-12);
  assert_near(bounds.height, 20.0, 1e-12);
  // At the start instead, the head lies at the start; the far end is a round cap.
  line->connector.style = DT_CANVAS_CONNECTOR_ARROW_START;
  bounds = dt_canvas_bounds(canvas);
  assert_near(bounds.x, -2.0, 1e-12);
  assert_near(bounds.y, -10.0, 1e-12);
  assert_near(bounds.width, 304.0, 1e-12);
  dt_canvas_route_t route;
  assert_true(dt_canvas_connector_route(canvas, line, &route));
  double triangle[6];
  dt_canvas_route_arrow_head(&route, FALSE, 4.0, triangle);
  assert_near(triangle[0], 0.0, 1e-12);
  assert_near(triangle[2], 28.0, 1e-12);
  assert_near(fabs(triangle[3]), 10.0, 1e-12);
  assert_near(triangle[4], 28.0, 1e-12);
  assert_near(triangle[3] + triangle[5], 0.0, 1e-12);
  // Without a head the ink reaches half the width past the round caps.
  line->connector.style = DT_CANVAS_CONNECTOR_DASHED;
  bounds = dt_canvas_bounds(canvas);
  assert_near(bounds.x, -2.0, 1e-12);
  assert_near(bounds.height, 4.0, 1e-12);
  // A cubic line's bounds hold the curve, which reaches three quarters of the way to its controls.
  line->connector.routing = DT_CANVAS_ROUTING_CUBIC;
  assert_true(dt_canvas_connector_route(canvas, line, &route));
  assert_true(dt_canvas_connector_seed_curve(line, &route));
  assert_true(dt_canvas_connector_route(canvas, line, &route));
  bounds = dt_canvas_bounds(canvas);
  assert_near(bounds.y, 0.75 * route.control1_y - 2.0, 1e-9);
  assert_near(bounds.height, -0.75 * route.control1_y + 4.0, 1e-9);

  // Through a waypoint, away from the origin, with tangents that throw the first half up and the
  // second out past the end: both halves' true extremes, sampled, are the box, and neither is a
  // control point.
  line->connector.from_tangent_x = 0.0f;
  line->connector.from_tangent_y = -150.0f;
  line->connector.to_tangent_x = 150.0f;
  line->connector.to_tangent_y = 0.0f;
  line->connector.from_x = 500.0;
  line->connector.from_y = 500.0;
  line->connector.to_x = 800.0;
  line->connector.to_y = 500.0;
  line->connector.via_count = 1;
  line->connector.via_x = 650.0;
  line->connector.via_y = 700.0;
  line->connector.via_tangent_x = 200.0;
  line->connector.via_tangent_y = 0.0;
  assert_true(dt_canvas_connector_route(canvas, line, &route));
  double low_x = route.from_x;
  double high_x = route.from_x;
  double low_y = route.from_y;
  double high_y = route.from_y;
  _sampled_cubic_range(route.from_x, route.control1_x, route.control2_x, route.via_x, &low_x, &high_x);
  _sampled_cubic_range(route.from_y, route.control1_y, route.control2_y, route.via_y, &low_y, &high_y);
  _sampled_cubic_range(route.via_x, route.control3_x, route.control4_x, route.to_x, &low_x, &high_x);
  _sampled_cubic_range(route.via_y, route.control3_y, route.control4_y, route.to_y, &low_y, &high_y);
  assert_true(high_x > route.to_x + 10.0);
  assert_true(high_x < route.control4_x - 10.0);
  assert_true(low_y < route.from_y - 10.0);
  assert_true(low_y > route.control1_y + 10.0);
  dt_canvas_rect_t extent;
  assert_true(dt_canvas_object_extent(canvas, line, &extent));
  assert_near(extent.x, low_x - 2.0, 1e-6);
  assert_near(extent.y, low_y - 2.0, 1e-6);
  assert_near(extent.x + extent.width, high_x + 2.0, 1e-6);
  assert_near(extent.y + extent.height, high_y + 2.0, 1e-6);

  // A hidden line is left out, and frames with a connector between them are framed as before.
  line->flags |= DT_CANVAS_OBJECT_FLAG_HIDDEN;
  dt_canvas_object_t *left = dt_canvas_add_text(canvas, 0.0, 400.0, 100.0, 100.0, "");
  dt_canvas_object_t *right = dt_canvas_add_text(canvas, 400.0, 400.0, 100.0, 100.0, "");
  dt_canvas_object_t *anchored = dt_canvas_add_connector(canvas, left->id, right->id);
  anchored->connector.routing = DT_CANVAS_ROUTING_SQUARE;
  anchored->connector.via_count = 1;
  anchored->connector.via_y = 2000.0;
  bounds = dt_canvas_bounds(canvas);
  assert_near(bounds.x, -50.0, 1e-12);
  assert_near(bounds.y, 350.0, 1e-12);
  assert_near(bounds.width, 500.0, 1e-12);
  assert_near(bounds.height, 100.0, 1e-12);
  // The extent of a frame is its bounds; of an unroutable connector, nothing.
  assert_true(dt_canvas_object_extent(canvas, left, &extent));
  assert_near(extent.width, 100.0, 1e-12);
  anchored->connector.to_id = 999;
  assert_false(dt_canvas_object_extent(canvas, anchored, &extent));
  dt_canvas_free(canvas);
}

/**
 * A frame offers nine places to attach a connector: the four edge midpoints, the four
 * corners, and the centre. They are the frame's own points, so they turn with it.
 */
/**
 * The page list is ordered for reading and stored by code, and the two now agree: the enum was
 * renumbered once, deliberately, while the format was R&D and local, and is append-only again.
 */
static void _a_custom_page_is_the_canvass_own_size(void **state)
{
  (void)state;
  /*
   * The one size the table cannot hold, because it is the document's. `dt_canvas_paper_points()`
   * must REFUSE it -- it has no canvas and would otherwise hand back a 0 x 0 page -- and
   * `dt_canvas_paper_dimensions()`, which does have one, answers it.
   */
  dt_canvas_t *canvas = dt_canvas_new();
  canvas->paper_size = DT_CANVAS_PAPER_CUSTOM;
  double width = 0.0;
  double height = 0.0;

  // Never sized, it is no page at all -- which every consumer already handles, so there is
  // nothing to migrate: zeros in an old file read exactly this way.
  assert_false(dt_canvas_paper_custom(canvas, &width, &height));
  assert_false(dt_canvas_paper_dimensions(canvas, &width, &height));

  dt_canvas_paper_custom_set(canvas, 210.0 * 72.0 / 25.4, 297.0 * 72.0 / 25.4);
  assert_true(dt_canvas_paper_custom(canvas, &width, &height));
  assert_true(dt_canvas_paper_dimensions(canvas, &width, &height));
  assert_float_equal(width, 595.28, 0.05);
  assert_float_equal(height, 841.89, 0.05);
  // It turns with the orientation exactly as a named size does.
  canvas->paper_landscape = TRUE;
  assert_true(dt_canvas_paper_dimensions(canvas, &width, &height));
  assert_float_equal(width, 841.89, 0.05);
  assert_float_equal(height, 595.28, 0.05);

  // A size that is not a number is none of it, which reads back as a page never given.
  dt_canvas_paper_custom_set(canvas, NAN, 500.0);
  assert_false(dt_canvas_paper_custom(canvas, &width, &height));

  // And it survives the file.
  dt_canvas_paper_custom_set(canvas, 333.0, 444.0);
  canvas->paper_landscape = FALSE;
  gchar *path = g_build_filename(g_get_tmp_dir(), "canvas-custom-page.anselcanvas", NULL);
  GError *error = NULL;
  assert_true(dt_canvas_save(canvas, path, &error));
  dt_canvas_t *back = dt_canvas_load(path, &error);
  assert_non_null(back);
  assert_int_equal(back->paper_size, DT_CANVAS_PAPER_CUSTOM);
  assert_true(dt_canvas_paper_custom(back, &width, &height));
  assert_float_equal(width, 333.0, 0.01);
  assert_float_equal(height, 444.0, 0.01);
  dt_canvas_free(back);
  g_unlink(path);
  g_free(path);
  dt_canvas_free(canvas);
}

static void _the_page_list_reads_in_order_and_stores_by_code(void **state)
{
  (void)state;
  // None, then the one size the canvas carries itself, then down the ISO A series.
  assert_int_equal(dt_canvas_paper_code(0), DT_CANVAS_PAPER_NONE);
  assert_int_equal(dt_canvas_paper_code(1), DT_CANVAS_PAPER_CUSTOM);
  assert_int_equal(dt_canvas_paper_code(2), DT_CANVAS_PAPER_A0);
  assert_int_equal(dt_canvas_paper_code(3), DT_CANVAS_PAPER_A1);
  assert_int_equal(dt_canvas_paper_code(8), DT_CANVAS_PAPER_A6);
  assert_string_equal(dt_canvas_paper_name(2), "A0");
  // The stored code IS the position now, which is what the renumbering bought.
  for(int position = 0; position < dt_canvas_paper_count(); position++)
    assert_int_equal((int)dt_canvas_paper_code(position), position);
  /* Every code the list offers comes back to the row it was shown on, and every one of them is
   * a size -- except CUSTOM, whose size is the canvas's and which `dt_canvas_paper_points()`
   * must REFUSE, or a custom page would silently be 0 x 0. */
  for(int position = 1; position < dt_canvas_paper_count(); position++)
  {
    const uint32_t code = dt_canvas_paper_code(position);
    assert_int_equal(dt_canvas_paper_position(code), position);
    double width = 0.0;
    double height = 0.0;
    if(code == DT_CANVAS_PAPER_CUSTOM)
    {
      assert_false(dt_canvas_paper_points(code, &width, &height));
      continue;
    }
    assert_true(dt_canvas_paper_points(code, &width, &height));
    assert_true(width > 0.0 && height > 0.0);
  }
  // And a size this build has never heard of is no page, never the last row of the table.
  assert_true(dt_canvas_paper_known(DT_CANVAS_PAPER_A4));
  assert_true(dt_canvas_paper_known(DT_CANVAS_PAPER_CUSTOM));
  assert_false(dt_canvas_paper_known(DT_CANVAS_PAPER_LAST));
  assert_false(dt_canvas_paper_known(9999u));
  // A0 is 841 by 1189 mm, which is what a print shop will ask for.
  double width = 0.0;
  double height = 0.0;
  assert_true(dt_canvas_paper_points(DT_CANVAS_PAPER_A0, &width, &height));
  assert_float_equal(width, 2384.0, 1.0);
  assert_float_equal(height, 3370.0, 1.0);
  // And each step down the series halves the sheet: A1's long edge is A0's short one.
  double next_width = 0.0;
  double next_height = 0.0;
  assert_true(dt_canvas_paper_points(DT_CANVAS_PAPER_A1, &next_width, &next_height));
  assert_float_equal(next_height, width, 1.0);
}

/**
 * The page's inner margin and the sheet's bleed are the same rectangle moved in and out, and
 * both are the document's rather than the export's.
 */
static void _a_page_carries_a_margin_inside_it_and_a_bleed_outside(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  canvas->paper_size = DT_CANVAS_PAPER_A6; // 298 x 420 points
  canvas->resolution = 72.0f;              // one unit to the point, so the numbers below are the points
  canvas->paper_landscape = 0;
  canvas->page_margin = 20.0f;
  canvas->page_bleed = 10.0f;
  dt_canvas_rect_t rect;

  assert_true(dt_canvas_page_guide_rect(canvas, 0, 0, 0.0, &rect));
  assert_float_equal(rect.width, 298.0, 1e-6);
  assert_float_equal(rect.height, 420.0, 1e-6);
  // Inside, by the margin, on all four sides.
  assert_true(dt_canvas_page_guide_rect(canvas, 0, 0, -canvas->page_margin, &rect));
  assert_float_equal(rect.x, 20.0, 1e-6);
  assert_float_equal(rect.y, 20.0, 1e-6);
  assert_float_equal(rect.width, 298.0 - 40.0, 1e-6);
  assert_float_equal(rect.height, 420.0 - 40.0, 1e-6);
  // Outside, by the bleed, on all four sides -- and on the page next door too.
  assert_true(dt_canvas_page_guide_rect(canvas, 1, 0, canvas->page_bleed, &rect));
  assert_float_equal(rect.x, 298.0 - 10.0, 1e-6);
  assert_float_equal(rect.width, 298.0 + 20.0, 1e-6);
  // A margin that would meet itself is no guide at all.
  assert_false(dt_canvas_page_guide_rect(canvas, 0, 0, -200.0, &rect));
  // And none of it exists without pages.
  canvas->paper_size = DT_CANVAS_PAPER_NONE;
  assert_false(dt_canvas_page_guide_rect(canvas, 0, 0, 0.0, &rect));
  dt_canvas_free(canvas);
}

static void _a_frame_offers_its_corners_and_its_centre_as_anchors(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *frame = dt_canvas_add_text(canvas, 100.0, 50.0, 200.0, 100.0, "");
  double x = 0.0;
  double y = 0.0;
  double normal_x = 0.0;
  double normal_y = 0.0;

  // The corners are the frame's corners, and their normals point out along the diagonal.
  dt_canvas_object_anchor_point(canvas, frame, DT_CANVAS_ANCHOR_SOUTH_EAST, 0.0, 0.0, &x, &y, &normal_x, &normal_y);
  assert_float_equal(x, 200.0, 1e-9);
  assert_float_equal(y, 100.0, 1e-9);
  assert_float_equal(normal_x, normal_y, 1e-9);
  assert_true(normal_x > 0.0);
  assert_float_equal(hypot(normal_x, normal_y), 1.0, 1e-9);
  dt_canvas_object_anchor_point(canvas, frame, DT_CANVAS_ANCHOR_NORTH_WEST, 0.0, 0.0, &x, &y, &normal_x, &normal_y);
  assert_float_equal(x, 0.0, 1e-9);
  assert_float_equal(y, 0.0, 1e-9);

  // A quarter turn carries them round with the frame: the corner at (+100, +50) in the
  // frame's own axes swings to (-50, +100) of its centre.
  frame->rotation = M_PI / 2.0;
  dt_canvas_object_anchor_point(canvas, frame, DT_CANVAS_ANCHOR_SOUTH_EAST, 0.0, 0.0, &x, &y, &normal_x, &normal_y);
  assert_float_equal(x, 50.0, 1e-6);
  assert_float_equal(y, 150.0, 1e-6);
  frame->rotation = 0.0;

  // The centre's handle is the centre; what it attaches is out on the edge facing the other
  // end, so it slides around the frame as that end moves.
  dt_canvas_object_anchor_handle(canvas, frame, DT_CANVAS_ANCHOR_CENTRE, &x, &y);
  assert_float_equal(x, 100.0, 1e-9);
  assert_float_equal(y, 50.0, 1e-9);
  dt_canvas_object_anchor_point(canvas, frame, DT_CANVAS_ANCHOR_CENTRE, 1000.0, 50.0, &x, &y, &normal_x, &normal_y);
  assert_float_equal(x, 200.0, 1e-9); // the right edge, dead level with the centre
  assert_float_equal(y, 50.0, 1e-9);
  assert_float_equal(normal_x, 1.0, 1e-9);
  dt_canvas_object_anchor_point(canvas, frame, DT_CANVAS_ANCHOR_CENTRE, 100.0, -1000.0, &x, &y, &normal_x, &normal_y);
  assert_float_equal(x, 100.0, 1e-9); // straight above: the top edge
  assert_float_equal(y, 0.0, 1e-9);
  assert_float_equal(normal_y, -1.0, 1e-9);
  // Every direction leaves it on the frame's own edge, never inside and never past it.
  for(int step = 0; step < 16; step++)
  {
    const double angle = step * M_PI / 8.0;
    dt_canvas_object_anchor_point(canvas, frame, DT_CANVAS_ANCHOR_CENTRE, 100.0 + cos(angle) * 900.0,
                                  50.0 + sin(angle) * 900.0, &x, &y, &normal_x, &normal_y);
    const double on_side = fabs(fabs(x - 100.0) - 100.0) < 1e-9;
    const double on_edge = fabs(fabs(y - 50.0) - 50.0) < 1e-9;
    assert_true(on_side || on_edge);
    assert_true(fabs(x - 100.0) <= 100.0 + 1e-9 && fabs(y - 50.0) <= 50.0 + 1e-9);
  }

  // The centre leaves by what the object DRAWS. Cut it to a circle well inside the frame and
  // the line stops on the circle's own edge, not out on the rectangle.
  dt_canvas_mask_set_shape(canvas, frame, DT_CANVAS_MASK_CIRCLE);
  frame->mask.center_x = 0.5f;
  frame->mask.center_y = 0.5f;
  frame->mask.radius_x = 0.25f; // a quarter of the shorter side: 25 units
  frame->mask.feather = 0.0f;
  frame->border_width = 0.0f;
  frame->flags |= DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE;
  dt_canvas_object_anchor_point(canvas, frame, DT_CANVAS_ANCHOR_CENTRE, 1000.0, 50.0, &x, &y, &normal_x, &normal_y);
  assert_float_equal(x, 125.0, 1e-6); // the frame's centre plus the circle's radius
  assert_float_equal(y, 50.0, 1e-6);
  // The fall-off and the border reach past the shape, and the line ends past them too.
  frame->mask.feather = 0.1f; // ten more units
  dt_canvas_object_anchor_point(canvas, frame, DT_CANVAS_ANCHOR_CENTRE, 1000.0, 50.0, &x, &y, &normal_x, &normal_y);
  assert_float_equal(x, 135.0, 1e-6);
  frame->border_width = 5.0f;
  frame->border_color = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
  dt_canvas_object_anchor_point(canvas, frame, DT_CANVAS_ANCHOR_CENTRE, 1000.0, 50.0, &x, &y, &normal_x, &normal_y);
  assert_float_equal(x, 140.0, 1e-6);
  // Never past the frame: a cutout is confined to it, and so is what leaves by the centre.
  frame->mask.radius_x = 5.0f;
  dt_canvas_object_anchor_point(canvas, frame, DT_CANVAS_ANCHOR_CENTRE, 1000.0, 50.0, &x, &y, &normal_x, &normal_y);
  assert_float_equal(x, 200.0, 1e-6);
  // Inverted, the shape is a hole and the frame's own edge is what shows.
  frame->mask.radius_x = 0.25f;
  frame->mask.flags |= DT_CANVAS_MASK_INVERT;
  dt_canvas_object_anchor_point(canvas, frame, DT_CANVAS_ANCHOR_CENTRE, 1000.0, 50.0, &x, &y, &normal_x, &normal_y);
  assert_float_equal(x, 200.0, 1e-6);
  frame->mask.flags = 0;

  // An ellipse answers the same way, through its own axes.
  dt_canvas_mask_set_shape(canvas, frame, DT_CANVAS_MASK_ELLIPSE);
  frame->mask.center_x = 0.5f;
  frame->mask.center_y = 0.5f;
  frame->mask.radius_x = 0.25f;
  frame->mask.radius_y = 0.4f;
  frame->mask.rotation = 0.0f;
  frame->mask.feather = 0.0f;
  frame->border_width = 0.0f;
  dt_canvas_object_anchor_point(canvas, frame, DT_CANVAS_ANCHOR_CENTRE, 1000.0, 50.0, &x, &y, &normal_x, &normal_y);
  assert_float_equal(x, 125.0, 1e-6);
  dt_canvas_object_anchor_point(canvas, frame, DT_CANVAS_ANCHOR_CENTRE, 100.0, 1000.0, &x, &y, &normal_x, &normal_y);
  assert_float_equal(y, 90.0, 1e-6); // 0.4 of the shorter side, downward

  // And a polygon, against the straight run of its nodes.
  dt_canvas_mask_set_shape(canvas, frame, DT_CANVAS_MASK_POLYGON);
  frame->mask.feather = 0.0f;
  const float square_corners[4][2] = { { 0.25f, 0.25f }, { 0.75f, 0.25f }, { 0.75f, 0.75f }, { 0.25f, 0.75f } };
  float square_nodes[4 * DT_CANVAS_MASK_NODE_FLOATS];
  memset(square_nodes, 0, sizeof(square_nodes));
  for(int node = 0; node < 4; node++)
  {
    float *record = square_nodes + (size_t)node * DT_CANVAS_MASK_NODE_FLOATS;
    record[DT_CANVAS_MASK_NODE_X] = square_corners[node][0];
    record[DT_CANVAS_MASK_NODE_Y] = square_corners[node][1];
    record[DT_CANVAS_MASK_NODE_CTRL1_X] = square_corners[node][0];
    record[DT_CANVAS_MASK_NODE_CTRL1_Y] = square_corners[node][1];
    record[DT_CANVAS_MASK_NODE_CTRL2_X] = square_corners[node][0];
    record[DT_CANVAS_MASK_NODE_CTRL2_Y] = square_corners[node][1];
  }
  dt_canvas_mask_set_nodes(canvas, frame, square_nodes, 4);
  dt_canvas_object_anchor_point(canvas, frame, DT_CANVAS_ANCHOR_CENTRE, 1000.0, 50.0, &x, &y, &normal_x, &normal_y);
  assert_float_equal(x, 150.0, 1e-6); // a quarter of the frame's width in from its right edge

  dt_canvas_mask_set_shape(canvas, frame, DT_CANVAS_MASK_NONE);
  frame->border_width = 0.0f;

  // Rounded corners round the silhouette with them: straight out to a corner, the line stops
  // on the arc rather than on the point the rectangle would have had.
  frame->corner_radius = 30.0f;
  frame->flags |= DT_CANVAS_OBJECT_FLAG_CORNER_OVERRIDE;
  const double square = dt_canvas_object_silhouette_reach(canvas, frame, 1.0, 0.0);
  assert_float_equal(square, 100.0, 1e-6); // a flat side is untouched by the radius
  const double diagonal = dt_canvas_object_silhouette_reach(canvas, frame, 100.0, 50.0);
  assert_true(diagonal < hypot(100.0, 50.0) - 1e-6);
  frame->flags &= ~DT_CANVAS_OBJECT_FLAG_CORNER_OVERRIDE;

  // The anchors are stored as they stand, so a new one has to be appended, never inserted.
  assert_int_equal(DT_CANVAS_ANCHOR_NORTH, 1);
  assert_int_equal(DT_CANVAS_ANCHOR_WEST, 4);
  assert_int_equal(DT_CANVAS_ANCHOR_CENTRE, 9);
  dt_canvas_free(canvas);
}

static void _a_waypoint_bends_every_routing_through_it(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *left = dt_canvas_add_text(canvas, 0.0, 0.0, 200.0, 100.0, "");
  dt_canvas_object_t *right = dt_canvas_add_text(canvas, 600.0, 0.0, 200.0, 100.0, "");
  dt_canvas_object_t *connector = dt_canvas_add_connector(canvas, left->id, right->id);
  dt_canvas_route_t route;

  // Added at the middle of the current route, the waypoint changes nothing yet.
  dt_canvas_connector_add_via(canvas, connector);
  assert_int_equal(connector->connector.via_count, 1);
  assert_float_equal(connector->connector.via_x, 300.0, 1e-9);
  assert_float_equal(connector->connector.via_y, 0.0, 1e-9);

  // Moved, every routing passes through it.
  connector->connector.via_x = 300.0;
  connector->connector.via_y = 250.0;
  static const dt_canvas_routing_t routings[]
      = { DT_CANVAS_ROUTING_STRAIGHT, DT_CANVAS_ROUTING_SQUARE, DT_CANVAS_ROUTING_CUBIC };
  for(size_t idx = 0; idx < G_N_ELEMENTS(routings); idx++)
  {
    connector->connector.routing = routings[idx];
    assert_true(dt_canvas_connector_route(canvas, connector, &route));
    assert_int_equal(route.segment_count, 2);
    assert_true(dt_canvas_object_contains(canvas, connector, 300.0, 250.0, 1.0));
    assert_float_equal(route.points[0], 100.0, 1e-9);
    assert_float_equal(route.points[2 * route.point_count - 2], 500.0, 1e-9);
  }
  // Straight: two segments, the chord's middle is no longer on the line.
  connector->connector.routing = DT_CANVAS_ROUTING_STRAIGHT;
  assert_false(dt_canvas_object_contains(canvas, connector, 300.0, 0.0, 1.0));
  dt_canvas_connector_remove_via(canvas, connector);
  assert_true(dt_canvas_object_contains(canvas, connector, 300.0, 0.0, 1.0));
  dt_canvas_free(canvas);
}

static void _frames_snap_with_their_paddings_touching(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  canvas->padding = 30.0f;
  dt_canvas_object_t *fixed = dt_canvas_add_text(canvas, 100.0, 100.0, 200.0, 100.0, ""); // spans x 0..200, y 50..150
  dt_canvas_object_t *moving = dt_canvas_add_text(canvas, 400.0, 400.0, 100.0, 100.0, "");
  GArray *exclude = g_array_new(FALSE, FALSE, sizeof(uint32_t));
  g_array_append_val(exclude, moving->id);
  double delta_x = 0.0;
  double delta_y = 0.0;

  // The padding is a margin around EACH frame, so two of them side by side are two paddings
  // apart and their margin boxes meet on one line. The fixed frame's right edge is at 200,
  // so the moving frame's left edge belongs at 260: from 254 it is pulled 6 units.
  dt_canvas_rect_t box = { 254.0, 300.0, 100.0, 100.0 };
  assert_true(dt_canvas_snap_to_neighbours(canvas, &box, exclude, 8.0, DT_CANVAS_EDGE_ALL, &delta_x, &delta_y));
  assert_float_equal(delta_x, 6.0, 1e-9);
  // And one padding apart is no longer a resting place: nothing pulls it there.
  dt_canvas_rect_t single = { 232.0, 300.0, 100.0, 100.0 };
  double single_x = 0.0;
  dt_canvas_snap_to_neighbours(canvas, &single, exclude, 8.0, DT_CANVAS_EDGE_ALL, &single_x, &delta_y);
  assert_float_equal(single_x, 0.0, 1e-9);
  assert_float_equal(delta_y, 0.0, 1e-9); // nothing within reach on y
  // Only the dragged edges may snap: with the left edge held still, nothing pulls on x.
  assert_false(dt_canvas_snap_to_neighbours(canvas, &box, exclude, 8.0, DT_CANVAS_EDGE_RIGHT | DT_CANVAS_EDGE_BOTTOM,
                                            &delta_x, &delta_y));
  // Its top edge close to the fixed frame's top: aligned.
  box.y = 53.0;
  assert_true(dt_canvas_snap_to_neighbours(canvas, &box, exclude, 8.0, DT_CANVAS_EDGE_ALL, &delta_x, &delta_y));
  assert_float_equal(delta_y, -3.0, 1e-9);
  // Far from everything: nothing.
  box.x = 1000.0;
  box.y = 1000.0;
  assert_false(dt_canvas_snap_to_neighbours(canvas, &box, exclude, 8.0, DT_CANVAS_EDGE_ALL, &delta_x, &delta_y));
  // The excluded frame never attracts itself.
  const dt_canvas_rect_t self_box = dt_canvas_object_bounds(moving);
  dt_canvas_rect_t nudged = self_box;
  nudged.x += 2.0;
  assert_false(dt_canvas_snap_to_neighbours(canvas, &nudged, exclude, 8.0, DT_CANVAS_EDGE_ALL, &delta_x, &delta_y));

  // Same size: a width within reach of the fixed frame's takes it, a height far off is left alone,
  // and the reference names the frame the width came from.
  double width = 196.0;
  double height = 300.0;
  dt_canvas_rect_t width_reference = { 0.0, 0.0, 0.0, 0.0 };
  dt_canvas_rect_t height_reference = { 0.0, 0.0, 0.0, 0.0 };
  assert_true(dt_canvas_snap_size(canvas, exclude, 8.0, &width, &height, &width_reference, &height_reference));
  assert_float_equal(width, 200.0, 1e-9);
  assert_float_equal(height, 300.0, 1e-9);
  assert_float_equal(width_reference.x, 0.0, 1e-9);
  assert_float_equal(width_reference.width, 200.0, 1e-9);
  width = 150.0;
  assert_false(dt_canvas_snap_size(canvas, exclude, 8.0, &width, &height, NULL, NULL));

  // Masonry: two frames stacked with their paddings touching -- two paddings of clear space --
  // offer their combined height.
  dt_canvas_object_t *below = dt_canvas_add_text(canvas, 100.0, 150.0 + 60.0 + 40.0, 200.0, 80.0, ""); // y 210..290
  height = 235.0;
  assert_true(dt_canvas_snap_size(canvas, exclude, 8.0, NULL, &height, NULL, &height_reference));
  assert_float_equal(height, 240.0, 1e-9); // 50..290
  assert_float_equal(height_reference.y, 50.0, 1e-9);
  assert_float_equal(height_reference.height, 240.0, 1e-9);
  (void)below;
  (void)fixed;
  g_array_free(exclude, TRUE);
  dt_canvas_free(canvas);
}

/**
 * A canvas unit is a POINT, so a page is its own size whatever the export is rasterised at,
 * and a format named in pixels is a physical size too -- at the W3C's reference density, the
 * only number that makes "1080 px" mean a length. That is what lets a sheet and a story be
 * the same kind of thing.
 *
 * It used to scale a sheet of paper by the export density and leave everything ON the sheet
 * where it was, so raising the density shrank the whole layout against its own paper: measured
 * on A4, a twelve-point line went from 7.0% of the page's height at 72 dpi to 1.7% at 300.
 */
static void _a_page_is_measured_in_points_whatever_it_is_rasterised_at(void **state)
{
  (void)state;
  // The flag says how a format is WRITTEN DOWN, not how it reaches the plane.
  assert_true(dt_canvas_paper_is_physical(DT_CANVAS_PAPER_A4));
  assert_false(dt_canvas_paper_is_physical(DT_CANVAS_PAPER_STORY));

  dt_canvas_t *canvas = dt_canvas_new();
  double a4_width = 0.0;
  double a4_height = 0.0;
  double story_width = 0.0;
  double story_height = 0.0;
  static const double densities[] = { 72.0, 96.0, 300.0, 1200.0 };
  for(guint density = 0; density < G_N_ELEMENTS(densities); density++)
  {
    canvas->resolution = (float)densities[density];
    canvas->paper_size = DT_CANVAS_PAPER_A4;
    assert_true(dt_canvas_paper_dimensions(canvas, &a4_width, &a4_height));
    canvas->paper_size = DT_CANVAS_PAPER_STORY;
    assert_true(dt_canvas_paper_dimensions(canvas, &story_width, &story_height));
    // A4 is A4 and a story is a story, at every density there is.
    assert_float_equal(a4_width, 595.0, 1e-9);
    assert_float_equal(a4_height, 842.0, 1e-9);
    assert_float_equal(story_width, 1080.0 * 72.0 / DT_CANVAS_REFERENCE_PIXEL_DPI, 1e-9);
    assert_float_equal(story_height, 1920.0 * 72.0 / DT_CANVAS_REFERENCE_PIXEL_DPI, 1e-9);
  }
  // And the story is what it is named: rasterised at the reference density it gives back
  // exactly the pixel count on the tin.
  assert_float_equal(story_width * DT_CANVAS_REFERENCE_PIXEL_DPI / 72.0, 1080.0, 1e-9);
  assert_float_equal(story_height * DT_CANVAS_REFERENCE_PIXEL_DPI / 72.0, 1920.0, 1e-9);

  // A canvas with no answer takes the default rather than the point: 72 dpi is a preview, not
  // a print, and the number no longer decides any geometry that could be got wrong by it.
  canvas->resolution = 0.0f;
  assert_float_equal(dt_canvas_resolution(canvas), 300.0, 1e-9);
  dt_canvas_free(canvas);
}

/**
 * A spread is the block of pages that stays on one sheet. Inside it they are contiguous and
 * the borders between them are folds; between two spreads the plane opens by twice the bleed,
 * so each sheet carries its own all round and no two bleeds overlap. Zero is a plane tiled
 * uniformly, which is what every document written before the field holds.
 */
static void _a_spread_keeps_its_pages_together_and_opens_between_sheets(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  canvas->resolution = 72.0f;
  canvas->paper_size = DT_CANVAS_PAPER_A6; // 298 x 420 points
  canvas->page_bleed = 9.0f;
  canvas->page_margin = 20.0f;

  // No spread: the plane tiles evenly, every page is its own sheet, and nothing is a fold.
  const dt_canvas_rect_t plain = dt_canvas_page_rect(canvas, 2, 0);
  assert_float_equal(plain.x, 2.0 * 298.0, 1e-6);
  dt_canvas_rect_t sheet;
  int cols = 0;
  assert_true(dt_canvas_spread_rect(canvas, 2, 0, &sheet, &cols, NULL));
  assert_int_equal(cols, 1);
  assert_float_equal(sheet.width, 298.0, 1e-6);

  // Two pages to a sheet: page 1 abuts page 0, and page 2 opens the next sheet by two bleeds.
  canvas->spread_cols = 2;
  assert_float_equal(dt_canvas_page_rect(canvas, 0, 0).x, 0.0, 1e-6);
  assert_float_equal(dt_canvas_page_rect(canvas, 1, 0).x, 298.0, 1e-6);
  assert_float_equal(dt_canvas_page_rect(canvas, 2, 0).x, 2.0 * 298.0 + 18.0, 1e-6);
  assert_float_equal(dt_canvas_page_rect(canvas, 3, 0).x, 3.0 * 298.0 + 18.0, 1e-6);
  // And to the left of the origin the gaps are owed on that side too.
  assert_float_equal(dt_canvas_page_rect(canvas, -1, 0).x, -298.0 - 18.0, 1e-6);
  assert_float_equal(dt_canvas_page_rect(canvas, -2, 0).x, -2.0 * 298.0 - 18.0, 1e-6);

  assert_true(dt_canvas_spread_rect(canvas, 1, 0, &sheet, &cols, NULL));
  assert_int_equal(cols, 2);
  assert_float_equal(sheet.x, 0.0, 1e-6);
  assert_float_equal(sheet.width, 2.0 * 298.0, 1e-6);

  // Where a page sits in its sheet is what tells a fold from a trim.
  int across = 0;
  dt_canvas_page_in_spread(canvas, 0, 0, &across, NULL, NULL, NULL);
  assert_int_equal(across, 0);
  dt_canvas_page_in_spread(canvas, 1, 0, &across, NULL, NULL, NULL);
  assert_int_equal(across, 1);
  dt_canvas_page_in_spread(canvas, -1, 0, &across, NULL, NULL, NULL);
  assert_int_equal(across, 1); // the right-hand page of the sheet before the origin

  // The binding's allowance is owed by the fold side and by no other.
  canvas->bind_gutter = 30.0f;
  dt_canvas_rect_t margin;
  assert_true(dt_canvas_page_margin_rect(canvas, 0, 0, &margin));
  assert_float_equal(margin.x, 20.0, 1e-6);                        // outer edge: the margin alone
  assert_float_equal(margin.width, 298.0 - 20.0 - 50.0, 1e-6);     // fold edge: margin plus bind
  assert_true(dt_canvas_page_margin_rect(canvas, 1, 0, &margin));
  assert_float_equal(margin.x, 298.0 + 50.0, 1e-6);                // this one folds on its left
  assert_float_equal(margin.width, 298.0 - 50.0 - 20.0, 1e-6);
  // Nothing folds vertically here, so both horizontal edges take the margin alone.
  assert_float_equal(margin.y, 20.0, 1e-6);
  assert_float_equal(margin.height, 420.0 - 40.0, 1e-6);

  // A point in the gap between two sheets belongs to the page on its left.
  int col = 0;
  dt_canvas_page_at(canvas, 2.0 * 298.0 + 4.0, 0.0, &col, NULL);
  assert_int_equal(col, 1);
  dt_canvas_page_at(canvas, 2.0 * 298.0 + 20.0, 0.0, &col, NULL);
  assert_int_equal(col, 2);
  dt_canvas_free(canvas);
}

/**
 * The OpenType features are stored as the string Pango reads and shown as a list of boxes to
 * tick, so the two have to agree: what setting a tag writes, asking for it must read back.
 * They are keyed on the TAG rather than on a position in a table, because the list a font
 * offers is the font's own and no two faces agree on it.
 */
static void _opentype_features_round_trip_through_their_pango_spelling(void **state)
{
  (void)state;
  char features[DT_CANVAS_TEXT_FEATURES_LEN] = { 0 };

  // Nothing ticked is an empty string, which is the font's own behaviour and not "all off".
  assert_string_equal(features, "");
  assert_false(dt_canvas_text_feature_is_on(features, "liga"));

  dt_canvas_text_feature_set(features, sizeof(features), "liga", TRUE);
  dt_canvas_text_feature_set(features, sizeof(features), "hlig", TRUE);
  dt_canvas_text_feature_set(features, sizeof(features), "onum", TRUE);
  assert_true(dt_canvas_text_feature_is_on(features, "liga"));
  assert_true(dt_canvas_text_feature_is_on(features, "hlig"));
  assert_true(dt_canvas_text_feature_is_on(features, "onum"));
  assert_false(dt_canvas_text_feature_is_on(features, "smcp"));
  // Pango's own spelling, so the renderer reads back exactly what the panel wrote.
  assert_non_null(strstr(features, "liga 1"));
  assert_non_null(strstr(features, "hlig 1"));

  // Switching one off closes the gap it leaves rather than stranding a separator.
  dt_canvas_text_feature_set(features, sizeof(features), "hlig", FALSE);
  assert_false(dt_canvas_text_feature_is_on(features, "hlig"));
  assert_true(dt_canvas_text_feature_is_on(features, "liga"));
  assert_true(dt_canvas_text_feature_is_on(features, "onum"));
  assert_null(strstr(features, ",,"));
  assert_true(features[0] != ',' && features[strlen(features) - 1] != ',');

  // Setting what is already set, and clearing what was never set, both change nothing.
  char before[DT_CANVAS_TEXT_FEATURES_LEN];
  g_strlcpy(before, features, sizeof(before));
  dt_canvas_text_feature_set(features, sizeof(features), "liga", TRUE);
  dt_canvas_text_feature_set(features, sizeof(features), "swsh", FALSE);
  assert_string_equal(features, before);

  // A tag that will not fit whole is not written at all: half a tag is not a feature.
  char tight[12] = { 0 };
  dt_canvas_text_feature_set(tight, sizeof(tight), "liga", TRUE);
  assert_string_equal(tight, "liga 1");
  dt_canvas_text_feature_set(tight, sizeof(tight), "onum", TRUE);
  assert_string_equal(tight, "liga 1");

  /*
   * More features than the record's fixed 64-byte field holds must survive the file. A rich
   * face ships tens of them -- Linux Libertine, 32 -- and a document with seven set silently
   * refused the eighth and every one after it, which read as the checkboxes having stopped
   * working. The whole string travels as a chunk beside the record; the fixed field keeps as
   * many WHOLE tags as fit, for a reader that predates the chunk.
   */
  dt_canvas_t *canvas = dt_canvas_new();
  assert_non_null(canvas);
  dt_canvas_object_t *text = dt_canvas_add_text(canvas, 0.0, 0.0, 100.0, 100.0, "Typography.");
  assert_non_null(text);
  static const char *const many[] = { "liga", "dlig", "hlig", "onum", "smcp", "c2sc", "frac",
                                      "zero", "salt", "case", "cpsp", "sups" };
  for(guint idx = 0; idx < G_N_ELEMENTS(many); idx++)
    dt_canvas_text_feature_set(text->text.features, sizeof(text->text.features), many[idx], TRUE);
  assert_true(strlen(text->text.features) > DT_CANVAS_TEXT_FEATURES_FIELD);
  for(guint idx = 0; idx < G_N_ELEMENTS(many); idx++)
    assert_true(dt_canvas_text_feature_is_on(text->text.features, many[idx]));

  GBytes *index = dt_canvas_format_write_index(canvas);
  assert_non_null(index);
  dt_canvas_t *restored = dt_canvas_new();
  assert_true(dt_canvas_format_read_index(restored, index, NULL));
  g_bytes_unref(index);
  const dt_canvas_object_t *back = dt_canvas_object_at(restored, 0);
  assert_non_null(back);
  assert_string_equal(back->text.features, text->text.features);
  dt_canvas_free(restored);
  dt_canvas_free(canvas);

  // A tag the build has a name for is offered by name, and so is a numbered set, of which a
  // font may ship twenty and only the font knows what each draws.
  gchar *historical = dt_canvas_text_feature_label("hlig");
  gchar *historical_hint = dt_canvas_text_feature_hint("hlig");
  gchar *set = dt_canvas_text_feature_label("ss04");
  gchar *variant = dt_canvas_text_feature_label("cv12");
  gchar *nothing = dt_canvas_text_feature_label("zzzz");
  assert_non_null(historical);
  assert_non_null(historical_hint);
  assert_non_null(strstr(set, "4"));
  assert_non_null(strstr(variant, "12"));
  assert_null(nothing);
  dt_free(historical);
  dt_free(historical_hint);
  dt_free(set);
  dt_free(variant);

  // And the ones the layout engine owns are not offered at all: a checkbox on those breaks the
  // shaping rather than styling it.
  assert_true(dt_canvas_text_feature_offered("smcp"));
  assert_true(dt_canvas_text_feature_offered("ss04"));
  assert_false(dt_canvas_text_feature_offered("ccmp"));
  assert_false(dt_canvas_text_feature_offered("mark"));
  assert_false(dt_canvas_text_feature_offered("locl"));
}

/**
 * What a frame covers is its SILHOUETTE, not the box around it: a circular cutout covers its
 * middle and leaves the corner beside it empty. That is what text flowing around an object has
 * to ask, or it would keep clear of empty corners.
 */
static void _a_frame_covers_its_silhouette_and_not_its_corners(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *frame = dt_canvas_add_text(canvas, 0.0, 0.0, 200.0, 200.0, "");
  // Uncut: the whole rectangle is covered, and just outside it is not.
  assert_true(dt_canvas_object_covers(canvas, frame, 0.0, 0.0, 0.0));
  assert_true(dt_canvas_object_covers(canvas, frame, 95.0, 95.0, 0.0));
  assert_false(dt_canvas_object_covers(canvas, frame, 130.0, 0.0, 0.0));
  // The standoff grows it, which is what keeps the text off it.
  assert_true(dt_canvas_object_covers(canvas, frame, 130.0, 0.0, 40.0));

  // Cut to a circle: the centre is still covered, the corner it no longer fills is not.
  dt_canvas_mask_set_shape(canvas, frame, DT_CANVAS_MASK_CIRCLE);
  assert_true(dt_canvas_object_covers(canvas, frame, 0.0, 0.0, 0.0));
  assert_false(dt_canvas_object_covers(canvas, frame, 95.0, 95.0, 0.0));
  dt_canvas_free(canvas);
}

static void _paper_tiles_the_plane_from_the_origin(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  double width = 0.0;
  double height = 0.0;
  assert_false(dt_canvas_paper_dimensions(canvas, &width, &height));
  canvas->resolution = 72.0f; // one unit to the point, so the numbers below are the points
  canvas->paper_size = DT_CANVAS_PAPER_A4;
  assert_true(dt_canvas_paper_dimensions(canvas, &width, &height));
  assert_float_equal(width, 595.0, 1e-9);
  assert_float_equal(height, 842.0, 1e-9);
  canvas->paper_landscape = 1;
  assert_true(dt_canvas_paper_dimensions(canvas, &width, &height));
  assert_float_equal(width, 842.0, 1e-9);
  const dt_canvas_rect_t page = dt_canvas_page_rect(canvas, -1, 2);
  assert_float_equal(page.x, -842.0, 1e-9);
  assert_float_equal(page.y, 1190.0, 1e-9);
  assert_float_equal(page.width, 842.0, 1e-9);
  // Edges snap onto the nearest page border within reach.
  dt_canvas_rect_t box = { 836.0, 100.0, 100.0, 100.0 };
  double delta_x = 0.0;
  double delta_y = 0.0;
  assert_true(dt_canvas_snap_to_pages(canvas, &box, 8.0, DT_CANVAS_EDGE_ALL, &delta_x, &delta_y));
  assert_float_equal(delta_x, 6.0, 1e-9);
  assert_float_equal(delta_y, 0.0, 1e-9);
  assert_false(dt_canvas_snap_to_pages(canvas, &box, 8.0, DT_CANVAS_EDGE_RIGHT, &delta_x, &delta_y));
  dt_canvas_free(canvas);
}

static void _layouts_arrange_without_moving_the_group(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *frames[4];
  for(int idx = 0; idx < 4; idx++)
  {
    frames[idx] = dt_canvas_add_image(canvas, 1000.0 + idx * 7.0, 2000.0 - idx * 3.0, 3000, 2000);
  }
  const dt_canvas_rect_t before = dt_canvas_bounds(canvas);
  canvas->grid_size = 50.0f;
  canvas->padding = 30.0f;
  dt_canvas_layout_apply(canvas, NULL, DT_CANVAS_LAYOUT_GRID, 0, DT_CANVAS_SORT_CANVAS);
  const dt_canvas_rect_t after = dt_canvas_bounds(canvas);
  assert_float_equal(after.x, before.x, 1e-9);
  assert_float_equal(after.y, before.y, 1e-9);
  // Every frame keeps a padding around itself, so the second column starts one cell plus TWO
  // paddings after the first -- the same arithmetic the snapping uses, or an arranged layout
  // would not be one the snapping can reproduce by hand.
  assert_float_equal(frames[1]->x - frames[0]->x, frames[0]->width + 60.0, 1e-9);
  // Two columns of two: the second frame sits to the right of the first, the third below it.
  assert_true(frames[1]->x > frames[0]->x);
  assert_float_equal(frames[1]->y, frames[0]->y, 1e-9);
  assert_float_equal(frames[2]->x, frames[0]->x, 1e-9);
  assert_true(frames[2]->y > frames[0]->y);

  dt_canvas_layout_apply(canvas, NULL, DT_CANVAS_LAYOUT_ROW, 0, DT_CANVAS_SORT_CANVAS);
  for(int idx = 1; idx < 4; idx++)
  {
    assert_float_equal(frames[idx]->y, frames[0]->y, 1e-9);
    assert_true(frames[idx]->x > frames[idx - 1]->x);
  }

  dt_canvas_layout_apply(canvas, NULL, DT_CANVAS_LAYOUT_MASONRY, 3, DT_CANVAS_SORT_CANVAS);
  assert_float_equal(frames[0]->width, frames[3]->width, 1e-9);
  assert_true(frames[3]->y > frames[0]->y); // the fourth wraps under the first column

  // Snapping rounds to the grid only when enabled.
  assert_float_equal(dt_canvas_snap(canvas, 74.0), 74.0, 1e-9);
  canvas->grid_flags |= DT_CANVAS_GRID_SNAP;
  assert_float_equal(dt_canvas_snap(canvas, 74.0), 50.0, 1e-9);
  assert_float_equal(dt_canvas_snap(canvas, 76.0), 100.0, 1e-9);

  // With snapping on and a padding that is a grid multiple, every layout puts every frame's
  // top-left corner on the grid.
  canvas->padding = 50.0f;
  static const dt_canvas_layout_t layouts[]
      = { DT_CANVAS_LAYOUT_GRID, DT_CANVAS_LAYOUT_MASONRY, DT_CANVAS_LAYOUT_ROW, DT_CANVAS_LAYOUT_COLUMN };
  for(size_t layout = 0; layout < G_N_ELEMENTS(layouts); layout++)
  {
    dt_canvas_layout_apply(canvas, NULL, layouts[layout], 2, DT_CANVAS_SORT_CANVAS);
    for(int idx = 0; idx < 4; idx++)
    {
      const dt_canvas_rect_t bounds = dt_canvas_object_bounds(frames[idx]);
      assert_float_equal(fmod(bounds.x, 50.0), 0.0, 1e-6);
      assert_float_equal(fmod(bounds.y, 50.0), 0.0, 1e-6);
    }
  }
  dt_canvas_free(canvas);
}

static void _a_layout_sorts_images_by_a_key_and_keeps_the_rest_after(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  canvas->grid_flags = 0;
  // Three images in draw order c, a, b by file name, and a text frame first of all.
  dt_canvas_object_t *text = dt_canvas_add_text(canvas, 0.0, 0.0, 100.0, 100.0, "");
  dt_canvas_object_t *image_c = dt_canvas_add_image(canvas, 0.0, 0.0, 100, 100);
  dt_canvas_object_t *image_a = dt_canvas_add_image(canvas, 0.0, 0.0, 100, 100);
  dt_canvas_object_t *image_b = dt_canvas_add_image(canvas, 0.0, 0.0, 100, 100);
  g_strlcpy(image_c->image.filename, "c.nef", sizeof(image_c->image.filename));
  g_strlcpy(image_a->image.filename, "a.nef", sizeof(image_a->image.filename));
  g_strlcpy(image_b->image.filename, "b.nef", sizeof(image_b->image.filename));
  image_c->image.exif_datetime_taken = 30;
  image_a->image.exif_datetime_taken = 20;
  image_b->image.exif_datetime_taken = 10;
  dt_canvas_layout_apply(canvas, NULL, DT_CANVAS_LAYOUT_ROW, 0, DT_CANVAS_SORT_FILENAME);
  // A row lays frames out left to right: a, b, c, then the text frame.
  assert_true(image_a->x < image_b->x);
  assert_true(image_b->x < image_c->x);
  assert_true(image_c->x < text->x);
  dt_canvas_layout_apply(canvas, NULL, DT_CANVAS_LAYOUT_ROW, 0, DT_CANVAS_SORT_DATETIME);
  assert_true(image_b->x < image_a->x);
  assert_true(image_a->x < image_c->x);
  assert_true(image_c->x < text->x);
  // The canvas's own order is the draw order: the text frame first.
  dt_canvas_layout_apply(canvas, NULL, DT_CANVAS_LAYOUT_ROW, 0, DT_CANVAS_SORT_CANVAS);
  assert_true(text->x < image_c->x);
  assert_true(image_c->x < image_a->x);
  dt_canvas_free(canvas);
}

/* --- drawn shapes -------------------------------------------------------------------------- */

/** A rectangle, styled and cut, comes back from the index exactly as it went in. */
static void _a_shape_round_trips_with_its_reserved_bytes(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_shape_style_t style = dt_canvas_shape_style_default();
  style.fill = dt_canvas_color(0.2f, 0.4f, 0.6f, 0.8f);
  style.border_override = TRUE;
  style.border_width = 7.5f;
  style.border_color = dt_canvas_color(1.0f, 0.0f, 0.5f, 1.0f);
  style.corner_override = TRUE;
  style.corner_radius = 12.0f;
  style.sides = 9;
  style.depth = 0.3f;
  style.roundness = 0.6f;
  const dt_canvas_rect_t box = { -50.0, -30.0, 200.0, 120.0 };
  dt_canvas_object_t *shape = dt_canvas_add_shape(canvas, DT_CANVAS_SHAPE_RECTANGLE, &box, &style);
  assert_non_null(shape);
  assert_int_equal(shape->kind, DT_CANVAS_OBJECT_SHAPE);
  // The box is a rectangle on the plane and the object's own x/y is its middle.
  assert_float_equal(shape->x, 50.0, 1e-9);
  assert_float_equal(shape->y, 30.0, 1e-9);
  assert_float_equal(shape->width, 200.0, 1e-9);
  assert_float_equal(shape->height, 120.0, 1e-9);
  shape->shape.reserved[3] = 0x5A;
  const uint32_t shape_id = shape->id;

  GBytes *index = dt_canvas_format_write_index(canvas);
  assert_non_null(index);
  dt_canvas_t *restored = dt_canvas_new();
  assert_true(dt_canvas_format_read_index(restored, index, NULL));
  g_bytes_unref(index);
  const dt_canvas_object_t *back = dt_canvas_find_object(restored, shape_id);
  assert_non_null(back);
  assert_int_equal(back->kind, DT_CANVAS_OBJECT_SHAPE);
  assert_int_equal(back->shape.geometry, DT_CANVAS_SHAPE_RECTANGLE);
  assert_int_equal(back->shape.sides, 9);
  assert_float_equal(back->shape.depth, 0.3f, 1e-6);
  assert_float_equal(back->shape.roundness, 0.6f, 1e-6);
  assert_int_equal(back->shape.reserved[3], 0x5A);
  assert_float_equal(back->background.alpha, 0.8f, 1e-6);
  assert_float_equal(back->border_width, 7.5f, 1e-6);
  assert_float_equal(back->corner_radius, 12.0f, 1e-6);
  assert_true((back->flags & DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE) != 0);
  assert_true((back->flags & DT_CANVAS_OBJECT_FLAG_CORNER_OVERRIDE) != 0);
  assert_false((back->flags & DT_CANVAS_OBJECT_FLAG_SHADOW_OVERRIDE) != 0);
  // A shape is a frame: it snaps, it is bounded, it takes handles and it is exported.
  assert_true(dt_canvas_object_is_frame(back));
  dt_canvas_free(restored);
  dt_canvas_free(canvas);
}

/** The cutout's node chunk follows a shape's record as it follows any other frame's. */
static void _a_cut_rectangle_keeps_its_cutout_nodes(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  const dt_canvas_rect_t box = { 0.0, 0.0, 300.0, 200.0 };
  dt_canvas_object_t *shape = dt_canvas_add_shape(canvas, DT_CANVAS_SHAPE_RECTANGLE, &box, NULL);
  assert_non_null(shape);
  dt_canvas_mask_set_shape(canvas, shape, DT_CANVAS_MASK_POLYGON);
  assert_int_equal(shape->mask.shape, DT_CANVAS_MASK_POLYGON);
  assert_true(shape->mask.node_count >= 3);
  const uint32_t nodes = shape->mask.node_count;
  const float first_x = shape->mask.nodes[DT_CANVAS_MASK_NODE_X];
  const uint32_t shape_id = shape->id;

  GBytes *index = dt_canvas_format_write_index(canvas);
  assert_non_null(index);
  dt_canvas_t *restored = dt_canvas_new();
  assert_true(dt_canvas_format_read_index(restored, index, NULL));
  g_bytes_unref(index);
  const dt_canvas_object_t *back = dt_canvas_find_object(restored, shape_id);
  assert_non_null(back);
  assert_int_equal(back->mask.shape, DT_CANVAS_MASK_POLYGON);
  assert_int_equal(back->mask.node_count, nodes);
  assert_non_null(back->mask.nodes);
  assert_float_equal(back->mask.nodes[DT_CANVAS_MASK_NODE_X], first_x, 1e-6);
  dt_canvas_free(restored);
  dt_canvas_free(canvas);
}

/**
 * A kind from a build this one has never heard of: the record is stepped over by its own size, the
 * object keeps its place in the document, and nothing that walks the objects trips over it.
 */
static void _an_unknown_kind_is_kept_and_draws_nothing(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  const dt_canvas_rect_t box = { 0.0, 0.0, 200.0, 100.0 };
  dt_canvas_object_t *shape = dt_canvas_add_shape(canvas, DT_CANVAS_SHAPE_RECTANGLE, &box, NULL);
  assert_non_null(shape);
  dt_canvas_object_t *text = dt_canvas_add_text(canvas, 400.0, 0.0, 200.0, 100.0, "beside it");
  assert_non_null(text);
  const uint32_t unknown_id = shape->id;
  GBytes *index = dt_canvas_format_write_index(canvas);
  assert_non_null(index);
  gsize length = 0;
  const uint8_t *bytes = g_bytes_get_data(index, &length);
  uint8_t *edited = g_memdup2(bytes, length);
  // The first object record starts where the header ends, and its first field is the kind. The
  // header is the magic (8), the format version (4), then the header's own size.
  const uint32_t header_size = (uint32_t)edited[12] | ((uint32_t)edited[13] << 8) | ((uint32_t)edited[14] << 16)
                               | ((uint32_t)edited[15] << 24);
  assert_true(header_size + 8 <= length);
  edited[header_size] = 99;
  edited[header_size + 1] = 0;
  edited[header_size + 2] = 0;
  edited[header_size + 3] = 0;
  GBytes *patched = g_bytes_new_take(edited, length);
  g_bytes_unref(index);

  dt_canvas_t *restored = dt_canvas_new();
  assert_true(dt_canvas_format_read_index(restored, patched, NULL));
  g_bytes_unref(patched);
  const dt_canvas_object_t *kept = dt_canvas_find_object(restored, unknown_id);
  assert_non_null(kept);
  assert_int_equal(kept->kind, 99);
  // The record after it still parses: the unknown one was stepped over whole, not read into.
  assert_int_equal(dt_canvas_object_count(restored), 2);
  assert_non_null(dt_canvas_find_object(restored, text->id));
  // It is no frame, so nothing asks it for a raster, a route or an outline.
  assert_false(dt_canvas_object_is_frame(kept));
  dt_canvas_pick(restored, 0.0, 0.0, 4.0);
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 64, 64);
  cairo_t *cr = cairo_create(surface);
  const dt_canvas_rect_t whole = { -500.0, -500.0, 1000.0, 1000.0 };
  dt_canvas_paint_options_t options = dt_canvas_paint_options_export(NULL, 1.0, whole);
  dt_canvas_paint(cr, restored, &options);
  cairo_destroy(cr);
  cairo_surface_destroy(surface);
  GBytes *again = dt_canvas_format_write_index(restored);
  assert_non_null(again);
  g_bytes_unref(again);
  dt_canvas_free(restored);
  dt_canvas_free(canvas);
}

/** What comes back from the configuration was written by whatever wrote it; a shape is born sound. */
static void _a_shape_style_from_outside_is_made_sound(void **state)
{
  (void)state;
  dt_canvas_shape_style_t style = dt_canvas_shape_style_default();
  assert_true(dt_canvas_shape_style_sanitize(&style));
  assert_float_equal(style.fill.alpha, 1.0f, 1e-6);
  assert_false(style.border_override);
  assert_int_equal(style.sides, DT_CANVAS_SHAPE_DEFAULT_SIDES);
  assert_float_equal(style.depth, 0.0f, 1e-6);

  dt_canvas_shape_style_t damaged = style;
  damaged.fill.red = NAN;
  damaged.fill.alpha = 4.0f;
  damaged.border_width = -3.0f;
  damaged.corner_radius = INFINITY;
  damaged.shadow.blur = NAN;
  damaged.shadow.offset_x = -INFINITY;
  damaged.sides = 40;
  damaged.depth = 2.0f;
  damaged.roundness = -1.0f;
  damaged.border_override = 7;
  assert_false(dt_canvas_shape_style_sanitize(&damaged));
  assert_float_equal(damaged.fill.red, 0.0f, 1e-6);
  assert_float_equal(damaged.fill.alpha, 1.0f, 1e-6);
  assert_float_equal(damaged.border_width, 0.0f, 1e-6);
  assert_true(isfinite(damaged.corner_radius));
  // A number nobody can read is NONE of the length, not the far end of what the range allows:
  // the end of a signed range is the most extreme value there is, and a shadow thrown five
  // hundred units off the shape is the last thing a mistyped configuration key should produce.
  assert_float_equal(damaged.shadow.blur, 0.0f, 1e-6);
  assert_float_equal(damaged.shadow.offset_x, 0.0f, 1e-6);
  assert_int_equal(damaged.sides, DT_CANVAS_SHAPE_MAX_SIDES);
  assert_float_equal(damaged.depth, DT_CANVAS_SHAPE_MAX_DEPTH, 1e-6);
  assert_float_equal(damaged.roundness, 0.0f, 1e-6);
  assert_int_equal(damaged.border_override, TRUE);

  // A style nobody trusts still makes a shape, and the shape it makes is sound.
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_shape_style_t wild = style;
  wild.sides = 1;
  const dt_canvas_rect_t sliver = { 0.0, 0.0, 0.5, 0.0 };
  const dt_canvas_object_t *shape = dt_canvas_add_shape(canvas, (dt_canvas_shape_geometry_t)77, &sliver, &wild);
  assert_non_null(shape);
  assert_int_equal(shape->shape.geometry, DT_CANVAS_SHAPE_RECTANGLE);
  assert_int_equal(shape->shape.sides, DT_CANVAS_SHAPE_MIN_SIDES);
  // Neither side is left too small to take hold of.
  assert_true(shape->width >= 4.0);
  assert_true(shape->height >= 4.0);
  dt_canvas_free(canvas);
}

/**
 * A whole-canvas arrangement gathers the pictures. A shape is placed against something else --
 * a panel behind a caption -- so sweeping it into the grid would move it away from what it was
 * drawn for; named in a selection, it is arranged like anything else.
 */
static void _a_whole_canvas_arrangement_leaves_shapes_where_they_are(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *image = dt_canvas_add_image(canvas, 800.0, 500.0, 6000, 4000);
  const dt_canvas_rect_t box = { 1000.0, 1000.0, 200.0, 120.0 };
  dt_canvas_object_t *shape = dt_canvas_add_shape(canvas, DT_CANVAS_SHAPE_RECTANGLE, &box, NULL);
  assert_non_null(image);
  assert_non_null(shape);
  const double shape_x = shape->x;
  const double shape_y = shape->y;
  dt_canvas_layout_apply(canvas, NULL, DT_CANVAS_LAYOUT_GRID, 0, DT_CANVAS_SORT_CANVAS);
  assert_float_equal(shape->x, shape_x, 1e-9);
  assert_float_equal(shape->y, shape_y, 1e-9);

  GArray *ids = g_array_new(FALSE, FALSE, sizeof(uint32_t));
  g_array_append_val(ids, image->id);
  g_array_append_val(ids, shape->id);
  dt_canvas_layout_apply(canvas, ids, DT_CANVAS_LAYOUT_ROW, 0, DT_CANVAS_SORT_CANVAS);
  g_array_free(ids, TRUE);
  // Named in a selection it is arranged like anything else: the row anchors on the leftmost of the
  // two and lays the shape beside the picture, a long way from where it stood.
  assert_true(shape->x != shape_x || shape->y != shape_y);
  assert_true(shape->x > image->x);
  dt_canvas_free(canvas);
}

/* --- polygons and stars ---------------------------------------------------------------------- */

/** A shape of the given geometry, filling `width` x `height` at the origin, with no corner radius. */
static dt_canvas_object_t *_polygon(dt_canvas_t *canvas, const uint32_t sides, const float depth,
                                    const float roundness, const double width, const double height)
{
  dt_canvas_shape_style_t style = dt_canvas_shape_style_default();
  style.sides = sides;
  style.depth = depth;
  style.roundness = roundness;
  const dt_canvas_rect_t box = { -0.5 * width, -0.5 * height, width, height };
  return dt_canvas_add_shape(canvas, DT_CANVAS_SHAPE_POLYGON, &box, &style);
}

/** The box around an outline, as four numbers. */
static void _outline_box(const double *outline, const size_t points, double *left, double *right, double *top,
                         double *bottom)
{
  *left = HUGE_VAL;
  *right = -HUGE_VAL;
  *top = HUGE_VAL;
  *bottom = -HUGE_VAL;
  for(size_t idx = 0; idx < points; idx++)
  {
    *left = fmin(*left, outline[2 * idx]);
    *right = fmax(*right, outline[2 * idx]);
    *top = fmin(*top, outline[2 * idx + 1]);
    *bottom = fmax(*bottom, outline[2 * idx + 1]);
  }
}

/**
 * The shape touches all four edges of the frame it stands in, whatever it is: that is what makes
 * the frame's handles, the snapping, the layout and the body hug the shape rather than a box drawn
 * loosely around it. It holds for the convex polygon, the star and the rounded forms of both,
 * and in a frame of any proportions -- the fit stretches each axis on its own.
 */
static void _a_fitted_outline_fills_the_frame_it_stands_in(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  static const struct
  {
    uint32_t sides;
    float depth;
    float roundness;
  } cases[] = {
    { 3, 0.0f, 0.0f },  { 5, 0.0f, 0.0f },  { 6, 0.0f, 0.0f },  { 12, 0.0f, 0.0f },
    { 5, DT_CANVAS_SHAPE_STAR_DEPTH, 0.0f }, { 7, 0.9f, 0.0f },  { 3, 0.0f, 1.0f },
    { 6, 0.5f, 0.35f }, { 8, 0.0f, 0.5f },  { 12, 0.95f, 0.02f },
  };
  static const double boxes[][2] = { { 200.0, 140.0 }, { 97.0, 313.0 }, { 640.0, 640.0 } };
  for(size_t idx = 0; idx < G_N_ELEMENTS(cases); idx++)
  {
    for(size_t box = 0; box < G_N_ELEMENTS(boxes); box++)
    {
      dt_canvas_object_t *shape
          = _polygon(canvas, cases[idx].sides, cases[idx].depth, cases[idx].roundness, boxes[box][0], boxes[box][1]);
      assert_non_null(shape);
      // Built by hand rather than by dt_canvas_add_shape(), which holds a polygon at its own ratio.
      shape->flags |= DT_CANVAS_OBJECT_FLAG_FREE_RATIO;
      shape->width = boxes[box][0];
      shape->height = boxes[box][1];
      double outline[2 * DT_CANVAS_SHAPE_OUTLINE_MAX];
      const size_t points = dt_canvas_shape_outline(canvas, shape, outline, DT_CANVAS_SHAPE_OUTLINE_MAX);
      assert_true(points >= 3);
      double left = 0.0;
      double right = 0.0;
      double top = 0.0;
      double bottom = 0.0;
      _outline_box(outline, points, &left, &right, &top, &bottom);
      assert_float_equal(right - left, boxes[box][0], 1e-9);
      assert_float_equal(bottom - top, boxes[box][1], 1e-9);
      // Centred on the frame's own centre, which is where the object sits.
      assert_float_equal(left + right, 0.0, 1e-9);
      assert_float_equal(top + bottom, 0.0, 1e-9);
      dt_canvas_remove_object(canvas, shape->id);
    }
  }
  dt_canvas_free(canvas);
}

/**
 * The four numbers a shape is bounded by are the polygon envelope's, written again in canvas.h
 * because a document holds them as its own types and `src/canvas` does not take a dependency on
 * `src/math` for a constant. Written again is written twice, so this is what keeps them one
 * number: the toolbar's star glyph draws from the envelope's side of the pair, and a drift would
 * show as an icon that is not the shape the tool makes.
 */
static void _the_shape_bounds_are_the_polygon_envelope_s(void **state)
{
  (void)state;
  assert_float_equal(DT_CANVAS_SHAPE_STAR_DEPTH, (float)DT_POLYGON_PENTAGRAM_DEPTH, 0.0f);
  assert_float_equal(DT_CANVAS_SHAPE_MAX_DEPTH, (float)DT_POLYGON_MAX_DEPTH, 0.0f);
  assert_int_equal((int)DT_CANVAS_SHAPE_MIN_SIDES, DT_POLYGON_MIN_SIDES);
  assert_int_equal((int)DT_CANVAS_SHAPE_MAX_SIDES, DT_POLYGON_MAX_SIDES);
}

/**
 * A polygon and a star are regular, and the proportion they keep is their own outline's: a hexagon
 * is `sqrt(3) / 2` as wide as it is tall and stays so however its sides, its notches or its
 * roundness are edited. A rectangle has no such shape to keep, and neither has a polygon told to
 * keep none.
 */
static void _a_polygon_is_born_and_stays_at_its_own_ratio(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *hexagon = _polygon(canvas, 6, 0.0f, 0.0f, 240.0, 240.0);
  assert_non_null(hexagon);
  assert_true(dt_canvas_object_keeps_ratio(hexagon));
  assert_true(dt_canvas_shape_refit_height(hexagon));
  const double hexagon_aspect = dt_canvas_shape_unit_aspect(6, 0.0f, 0.0f);
  assert_float_equal(hexagon_aspect, sqrt(3.0) / 2.0, 1e-9);
  assert_float_equal(hexagon->width / hexagon->height, hexagon_aspect, 1e-9);
  // Once fitted it is fitted: asking again moves nothing.
  assert_false(dt_canvas_shape_refit_height(hexagon));

  // The triangle's is 2 / sqrt(3), and a five-pointed star's is its own: every one of them comes
  // from the same points the outline draws, so the frame and the shape cannot disagree.
  assert_float_equal(dt_canvas_shape_unit_aspect(3, 0.0f, 0.0f), 2.0 / sqrt(3.0), 1e-9);
  hexagon->shape.sides = 5;
  hexagon->shape.depth = DT_CANVAS_SHAPE_STAR_DEPTH;
  assert_true(dt_canvas_shape_refit_height(hexagon));
  assert_float_equal(hexagon->width / hexagon->height,
                     dt_canvas_shape_unit_aspect(5, DT_CANVAS_SHAPE_STAR_DEPTH, 0.0f), 1e-9);

  // Told to keep none, it is left stretched as it was stretched.
  hexagon->flags |= DT_CANVAS_OBJECT_FLAG_FREE_RATIO;
  hexagon->height = 1000.0;
  assert_false(dt_canvas_object_keeps_ratio(hexagon));
  assert_false(dt_canvas_shape_refit_height(hexagon));
  assert_float_equal(hexagon->height, 1000.0, 1e-9);

  // A rectangle has no ratio of its own, whatever numbers it carries for a geometry switch.
  const dt_canvas_rect_t box = { 0.0, 0.0, 300.0, 100.0 };
  dt_canvas_object_t *rectangle = dt_canvas_add_shape(canvas, DT_CANVAS_SHAPE_RECTANGLE, &box, NULL);
  assert_non_null(rectangle);
  assert_false(dt_canvas_object_keeps_ratio(rectangle));
  assert_false(dt_canvas_shape_refit_height(rectangle));

  // A shape too small to be one is held up to the smallest a shape may have, and that hold keeps
  // its proportions: lifting each side on its own puts a hexagon dragged out three units across
  // into a SQUARE box, and since a polygon keeps whatever ratio it is given, that square is then
  // what every later resize preserves.
  const dt_canvas_rect_t speck = { 0.0, 0.0, 3.0, 3.0 / (sqrt(3.0) / 2.0) };
  dt_canvas_shape_style_t tiny_style = dt_canvas_shape_style_default();
  tiny_style.sides = 6;
  dt_canvas_object_t *tiny = dt_canvas_add_shape(canvas, DT_CANVAS_SHAPE_POLYGON, &speck, &tiny_style);
  assert_non_null(tiny);
  fprintf(stderr, "a hexagon dragged out three units across is born %.3f by %.3f\n", tiny->width, tiny->height);
  assert_true(tiny->width >= 4.0 - 1e-9);
  assert_true(tiny->height >= 4.0 - 1e-9);
  assert_float_equal(tiny->width / tiny->height, hexagon_aspect, 1e-9);
  // And the refit agrees with the birth on the same box: a shape narrower than the minimum grows
  // both ways rather than coming out square.
  tiny->width = 3.0;
  tiny->height = 3.0;
  assert_true(dt_canvas_shape_refit_height(tiny));
  assert_true(tiny->width >= 4.0 - 1e-9);
  assert_true(tiny->height >= 4.0 - 1e-9);
  assert_float_equal(tiny->width / tiny->height, hexagon_aspect, 1e-9);
  dt_canvas_free(canvas);
}

/** How far a point is from the nearest point of an outline. */
static double _outline_nearest(const double *outline, const size_t points, const double x, const double y)
{
  double nearest = HUGE_VAL;
  for(size_t idx = 0; idx < points; idx++)
    nearest = fmin(nearest, hypot(outline[2 * idx] - x, outline[2 * idx + 1] - y));
  return nearest;
}

/** How far a point is from the nearest SEGMENT of a closed outline, the closing one included. */
static double _outline_edge_distance(const double *outline, const size_t points, const double x, const double y)
{
  double nearest = HUGE_VAL;
  for(size_t idx = 0; idx < points; idx++)
  {
    const size_t next = (idx + 1) % points;
    nearest = fmin(nearest, dt_canvas_segment_distance(x, y, outline[2 * idx], outline[2 * idx + 1],
                                                       outline[2 * next], outline[2 * next + 1]));
  }
  return nearest;
}

/** Whether two segments cross each other anywhere but at a shared end. */
static gboolean _segments_cross(const double ax, const double ay, const double bx, const double by,
                                const double cx, const double cy, const double dx, const double dy)
{
  const double first_x = bx - ax;
  const double first_y = by - ay;
  const double second_x = dx - cx;
  const double second_y = dy - cy;
  const double denominator = first_x * second_y - first_y * second_x;
  if(fabs(denominator) < 1e-12) return FALSE;
  const double along = ((cx - ax) * second_y - (cy - ay) * second_x) / denominator;
  const double across = ((cx - ax) * first_y - (cy - ay) * first_x) / denominator;
  // Strictly inside both, so two segments meeting end to end are not a crossing.
  return along > 1e-9 && along < 1.0 - 1e-9 && across > 1e-9 && across < 1.0 - 1e-9;
}

/** How many pairs of an outline's segments cross, the adjacent ones excepted: a simple outline has none. */
static int _outline_self_crossings(const double *outline, const size_t points)
{
  int crossings = 0;
  for(size_t first = 0; first < points; first++)
  {
    const size_t first_next = (first + 1) % points;
    for(size_t second = first + 2; second < points; second++)
    {
      const size_t second_next = (second + 1) % points;
      if(second_next == first) continue;
      if(_segments_cross(outline[2 * first], outline[2 * first + 1], outline[2 * first_next],
                         outline[2 * first_next + 1], outline[2 * second], outline[2 * second + 1],
                         outline[2 * second_next], outline[2 * second_next + 1]))
        crossings++;
    }
  }
  return crossings;
}

/**
 * A fillet stops where the edge it shares with its neighbour runs out, and the two share that edge
 * in proportion to what each asked for -- NOT half each. `math/polygon_envelope.h` warns this caller
 * by name against the half-edge cap, and the second half of this test is why: a star's segment is
 * half of the convex polygon's side, so a cap at half of it would halve the largest fillet a tip can
 * take the instant the notches gained a depth, while the shape itself has barely moved. Sharing in
 * proportion is continuous there, because a notch that has barely left the straight edge asks for
 * almost nothing and leaves the whole segment to the tip.
 *
 * What the sharing owes is that no fillet passes the corner beside it -- an arc that did would run
 * back along the edge it shares and cross both its neighbour and the straight piece between them. A
 * twelve-pointed star at the deepest notch, with a radius larger than the whole shape, is the
 * hardest case there is: every one of its twenty-four corners asks for more than it can have.
 */
static void _a_fillet_stops_where_its_neighbour_starts(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *star = _polygon(canvas, 12, (float)DT_CANVAS_SHAPE_MAX_DEPTH, 0.0f, 400.0, 400.0);
  assert_non_null(star);
  double corners[2 * DT_CANVAS_SHAPE_OUTLINE_MAX];
  const size_t corner_count = dt_canvas_shape_outline(canvas, star, corners, DT_CANVAS_SHAPE_OUTLINE_MAX);
  assert_int_equal(corner_count, 24);
  assert_int_equal(_outline_self_crossings(corners, corner_count), 0);

  star->flags |= DT_CANVAS_OBJECT_FLAG_CORNER_OVERRIDE;
  star->corner_radius = 10000.0f;
  double rounded[2 * DT_CANVAS_SHAPE_OUTLINE_MAX];
  const size_t rounded_count = dt_canvas_shape_outline(canvas, star, rounded, DT_CANVAS_SHAPE_OUTLINE_MAX);
  assert_true(rounded_count > corner_count);
  // The worst case the buffer is sized for, and it fits.
  assert_true(rounded_count <= DT_CANVAS_SHAPE_OUTLINE_MAX);
  const int crossings = _outline_self_crossings(rounded, rounded_count);
  fprintf(stderr, "fillets on a 12-point star: %zu corners -> %zu points, %d self-crossings\n", corner_count,
          rounded_count, crossings);
  assert_int_equal(crossings, 0);
  // And inside the frame the corners stood in: rounding a corner never grows a shape past its box.
  double left = 0.0;
  double right = 0.0;
  double top = 0.0;
  double bottom = 0.0;
  _outline_box(rounded, rounded_count, &left, &right, &top, &bottom);
  assert_true(left >= -0.5 * star->width - 1e-9);
  assert_true(right <= 0.5 * star->width + 1e-9);
  assert_true(top >= -0.5 * star->height - 1e-9);
  assert_true(bottom <= 0.5 * star->height + 1e-9);

  // The continuity the half-edge cap would have cost: a hexagon whose notches have barely left the
  // straight edge rounds its tips by what the convex hexagon rounds them by.
  double reach_at[2] = { 0.0, 0.0 };
  const float depths[2] = { 0.0f, 1e-3f };
  for(int which = 0; which < 2; which++)
  {
    dt_canvas_object_t *hexagon = _polygon(canvas, 6, depths[which], 0.0f, 400.0, 400.0);
    assert_non_null(hexagon);
    hexagon->flags |= DT_CANVAS_OBJECT_FLAG_CORNER_OVERRIDE;
    hexagon->corner_radius = 10000.0f;
    const size_t points = dt_canvas_shape_outline(canvas, hexagon, rounded, DT_CANVAS_SHAPE_OUTLINE_MAX);
    assert_true(points > 6);
    assert_int_equal(_outline_self_crossings(rounded, points), 0);
    // How far the rounded outline stands off the tip it rounds: the size of that tip's fillet.
    reach_at[which] = _outline_nearest(rounded, points, 0.0, -0.5 * hexagon->height);
    dt_canvas_remove_object(canvas, hexagon->id);
  }
  fprintf(stderr, "tip fillet across depth zero: %.4f units at depth 0, %.4f at depth 1e-3\n", reach_at[0],
          reach_at[1]);
  assert_true(reach_at[0] > 1.0);
  assert_true(fabs(reach_at[1] - reach_at[0]) < 0.02 * reach_at[0]);

  // And every corner of a regular shape is rounded by the SAME arc as every other corner of its
  // kind, because the shape's own symmetry says they are the same corner over and over. A pass that
  // shared each edge as it walked broke exactly this: the reach it cut down at one end of an edge
  // was the reach the next edge then shared against, and the edge that wraps was never revisited.
  const float symmetry_depths[2] = { 0.0f, (float)DT_CANVAS_SHAPE_STAR_DEPTH };
  for(int which = 0; which < 2; which++)
  {
    dt_canvas_object_t *regular = _polygon(canvas, 5, symmetry_depths[which], 0.0f, 400.0, 400.0);
    assert_non_null(regular);
    assert_true(dt_canvas_shape_refit_height(regular));
    double corner_points[2 * DT_CANVAS_SHAPE_OUTLINE_MAX];
    const size_t corners_here
        = dt_canvas_shape_outline(canvas, regular, corner_points, DT_CANVAS_SHAPE_OUTLINE_MAX);
    assert_int_equal(corners_here, symmetry_depths[which] > 0.0f ? 10u : 5u);
    regular->flags |= DT_CANVAS_OBJECT_FLAG_CORNER_OVERRIDE;
    // Big enough that the neighbours really do have to share their edge: at a radius no edge is
    // short for, every reach is the one it asked for and the sharing is never reached at all.
    regular->corner_radius = 200.0f;
    double filleted[2 * DT_CANVAS_SHAPE_OUTLINE_MAX];
    const size_t filleted_points
        = dt_canvas_shape_outline(canvas, regular, filleted, DT_CANVAS_SHAPE_OUTLINE_MAX);
    assert_true(filleted_points > corners_here);
    // How far each corner was pulled in by its own fillet, corner by corner around the shape.
    double pulled_in[2] = { -1.0, -1.0 };
    for(size_t idx = 0; idx < corners_here; idx++)
    {
      const double pull = _outline_edge_distance(filleted, filleted_points, corner_points[2 * idx],
                                                 corner_points[2 * idx + 1]);
      // The tips are the even corners and the notches the odd ones, the outline starting at a tip.
      const size_t kind = (symmetry_depths[which] > 0.0f) ? idx % 2 : 0;
      if(pulled_in[kind] < 0.0)
      {
        pulled_in[kind] = pull;
        assert_true(pull > 1.0);
        continue;
      }
      fprintf(stderr, "fillet symmetry at depth %.3f, corner %zu: %.6f against %.6f\n",
              (double)symmetry_depths[which], idx, pull, pulled_in[kind]);
      assert_float_equal(pull, pulled_in[kind], 1e-9);
    }
    dt_canvas_remove_object(canvas, regular->id);
  }

  // A shape with no corners left has nothing for a radius to take: a circle is a circle.
  star->shape.roundness = 1.0f;
  const size_t circle_count = dt_canvas_shape_outline(canvas, star, rounded, DT_CANVAS_SHAPE_OUTLINE_MAX);
  assert_int_equal(circle_count, 2 * 12 * 16);
  dt_canvas_free(canvas);
}

/**
 * A star's notches are holes in the shape, and the hit test knows it: a point out in a notch is not
 * the star, even filled and even well inside the box the star stands in. That is what stops an
 * ornament laid over a photograph from taking the clicks meant for the picture between its points.
 */
static void _a_filled_star_is_not_picked_in_its_notches(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *star = _polygon(canvas, 5, DT_CANVAS_SHAPE_STAR_DEPTH, 0.0f, 400.0, 400.0);
  assert_non_null(star);
  assert_true(dt_canvas_shape_refit_height(star));
  assert_true(dt_canvas_object_contains(canvas, star, star->x, star->y, 0.0));

  // Straight up is the first point; a tenth of a turn round is the notch between it and the next.
  const double tip_angle = 0.0;
  const double notch_angle = M_PI / 5.0;
  double tip_reach = 0.0;
  double notch_reach = 0.0;
  for(int step = 1; step <= 400; step++)
  {
    const double radius = (double)step;
    if(dt_canvas_object_contains(canvas, star, star->x + radius * sin(tip_angle),
                                 star->y - radius * cos(tip_angle), 0.0))
      tip_reach = radius;
    if(dt_canvas_object_contains(canvas, star, star->x + radius * sin(notch_angle),
                                 star->y - radius * cos(notch_angle), 0.0))
      notch_reach = radius;
  }
  fprintf(stderr, "star hit test: %.0f units toward a point, %.0f into a notch\n", tip_reach, notch_reach);
  // The point sits on the frame's top edge, half the fitted height out; the probes step by the unit.
  assert_true(tip_reach >= 0.5 * star->height - 1.0);
  // The notch's own radius is (1 - depth) cos(pi / 5) of the tip's, scaled by the fit: a hair over
  // a third. Anything past it is between the points and is not the star.
  assert_true(notch_reach < 0.45 * tip_reach);
  assert_false(dt_canvas_object_contains(canvas, star, star->x + 0.9 * tip_reach * sin(notch_angle),
                                         star->y - 0.9 * tip_reach * cos(notch_angle), 0.0));
  assert_true(dt_canvas_object_contains(canvas, star, star->x + 0.9 * tip_reach * sin(tip_angle),
                                        star->y - 0.9 * tip_reach * cos(tip_angle), 0.0));
  dt_canvas_free(canvas);
}

/**
 * What a connector anchored on a polygon's centre touches, and what text flowing round it clears, is
 * the polygon's own edge: a ray out of a triangle meets its slanted side long before it would meet
 * the corner of the box the triangle stands in.
 */
static void _a_polygons_silhouette_is_its_outline_not_its_box(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *triangle = _polygon(canvas, 3, 0.0f, 0.0f, 200.0, 200.0);
  assert_non_null(triangle);
  assert_true(dt_canvas_shape_refit_height(triangle));
  const double half_width = 0.5 * triangle->width;
  const double half_height = 0.5 * triangle->height;

  // The apex sits on the middle of the frame's top edge, and the reach that way is its distance.
  assert_float_equal(dt_canvas_object_silhouette_reach(canvas, triangle, 0.0, -1.0), half_height, 1e-9);
  // The two other vertices are the frame's own bottom corners.
  assert_float_equal(dt_canvas_object_silhouette_reach(canvas, triangle, half_width, half_height),
                     hypot(half_width, half_height), 1e-9);

  // Up and to the right, the ray leaves by the slanted side between the apex (0, -h/2) and the
  // bottom-right corner (w/2, h/2), a long way inside the corner of the box.
  const double apex_x = 0.0;
  const double apex_y = -half_height;
  const double corner_x = half_width;
  const double corner_y = half_height;
  const double unit = 1.0 / sqrt(2.0);
  // Solve apex + s (corner - apex) = t (unit, -unit) for t, the crossing's distance.
  const double edge_x = corner_x - apex_x;
  const double edge_y = corner_y - apex_y;
  const double denominator = unit * edge_y - (-unit) * edge_x;
  const double along = (apex_x * edge_y - apex_y * edge_x) / denominator;
  const double diagonal = dt_canvas_object_silhouette_reach(canvas, triangle, 1.0, -1.0);
  fprintf(stderr, "triangle silhouette: %.3f up the diagonal against %.3f to the frame's corner\n", diagonal,
          hypot(half_width, half_height));
  assert_float_equal(diagonal, along, 1e-9);
  assert_true(diagonal < 0.5 * hypot(half_width, half_height));
  // It never reads past what the frame draws: the fit puts the whole outline inside the box.
  for(int step = 0; step < 72; step++)
  {
    const double angle = step * M_PI / 36.0;
    const double reach = dt_canvas_object_silhouette_reach(canvas, triangle, cos(angle), sin(angle));
    assert_true(reach <= hypot(half_width, half_height) + 1e-9);
    assert_true(reach > 0.0);
  }

  // A ROUNDED shape takes no corner radius -- it has no corner left for one -- so the reach may not
  // be held back by one either. Capped against the frame rounded by a radius the outline never
  // used, the silhouette stopped short of a shape the painter fills to the frame's own edges, by up
  // to a quarter of it.
  dt_canvas_object_t *rounded = _polygon(canvas, 5, (float)DT_CANVAS_SHAPE_STAR_DEPTH, 0.5f, 400.0, 400.0);
  assert_non_null(rounded);
  assert_true(dt_canvas_shape_refit_height(rounded));
  rounded->flags |= DT_CANVAS_OBJECT_FLAG_CORNER_OVERRIDE;
  rounded->corner_radius = 0.4f * (float)fmin(rounded->width, rounded->height);
  double soft[2 * DT_CANVAS_SHAPE_OUTLINE_MAX];
  const size_t soft_points = dt_canvas_shape_outline(canvas, rounded, soft, DT_CANVAS_SHAPE_OUTLINE_MAX);
  assert_true(soft_points > 3);
  double worst_short = 0.0;
  for(size_t idx = 0; idx < soft_points; idx++)
  {
    const double sample_x = soft[2 * idx];
    const double sample_y = soft[2 * idx + 1];
    const double distance = hypot(sample_x, sample_y);
    if(!(distance > 1.0)) continue;
    const double reach = dt_canvas_object_silhouette_reach(canvas, rounded, sample_x, sample_y);
    worst_short = fmax(worst_short, (distance - reach) / distance);
  }
  fprintf(stderr, "a rounded star's silhouette falls short of its own outline by at most %.4f%%\n",
          100.0 * worst_short);
  // Every sample of the outline IS the silhouette that way, to the tolerance a ray crossing costs.
  assert_true(worst_short < 1e-9);
  dt_canvas_free(canvas);
}

/**
 * Nothing in the atelier cuts a polygon, so a mask left on one by a hand-edited file or by a
 * geometry it has since left is ignored -- by the painter, by the hit test, by the silhouette and by
 * the handles alike. One predicate answers for all of them, which is what stops any two of them from
 * drawing different shapes.
 */
static void _a_polygon_is_never_cut_whatever_mask_it_carries(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  dt_canvas_object_t *polygon = _polygon(canvas, 6, 0.0f, 0.0f, 300.0, 300.0);
  assert_non_null(polygon);
  assert_true(dt_canvas_shape_refit_height(polygon));
  double before[2 * DT_CANVAS_SHAPE_OUTLINE_MAX];
  const size_t before_points = dt_canvas_shape_outline(canvas, polygon, before, DT_CANVAS_SHAPE_OUTLINE_MAX);
  const double reach_before = dt_canvas_object_silhouette_reach(canvas, polygon, 1.0, 0.0);

  dt_canvas_mask_set_shape(canvas, polygon, DT_CANVAS_MASK_CIRCLE);
  assert_int_equal(polygon->mask.shape, DT_CANVAS_MASK_CIRCLE);
  assert_false(dt_canvas_object_is_cut(polygon));
  assert_true(dt_canvas_shape_needs_coverage(polygon));
  double after[2 * DT_CANVAS_SHAPE_OUTLINE_MAX];
  const size_t after_points = dt_canvas_shape_outline(canvas, polygon, after, DT_CANVAS_SHAPE_OUTLINE_MAX);
  assert_int_equal(after_points, before_points);
  for(size_t idx = 0; idx < 2 * before_points; idx++) assert_float_equal(after[idx], before[idx], 1e-12);
  assert_float_equal(dt_canvas_object_silhouette_reach(canvas, polygon, 1.0, 0.0), reach_before, 1e-9);

  // A RECTANGLE with the same mask is cut, which is what makes the answer above a decision and not
  // an accident of the shape kind.
  const dt_canvas_rect_t box = { 1000.0, 0.0, 300.0, 300.0 };
  dt_canvas_object_t *rectangle = dt_canvas_add_shape(canvas, DT_CANVAS_SHAPE_RECTANGLE, &box, NULL);
  assert_non_null(rectangle);
  dt_canvas_mask_set_shape(canvas, rectangle, DT_CANVAS_MASK_CIRCLE);
  assert_true(dt_canvas_object_is_cut(rectangle));
  assert_false(dt_canvas_shape_needs_coverage(rectangle));
  dt_canvas_free(canvas);
}

static void _colours_parse_and_format(void **state)
{
  (void)state;
  dt_canvas_color_t color = dt_canvas_color(0.0f, 0.0f, 0.0f, 0.0f);
  assert_true(dt_canvas_color_parse("#ff8040", &color));
  assert_float_equal(color.red, 1.0f, 1e-6);
  assert_float_equal(color.green, 128.0f / 255.0f, 1e-6);
  assert_float_equal(color.alpha, 1.0f, 1e-6);
  assert_true(dt_canvas_color_parse("#00000080", &color));
  assert_float_equal(color.alpha, 128.0f / 255.0f, 1e-6);
  assert_false(dt_canvas_color_parse("ff8040", &color));
  assert_false(dt_canvas_color_parse("#zz8040", &color));
  char text[16];
  dt_canvas_color_format(&color, text, sizeof(text));
  assert_string_equal(text, "#00000080");
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_index_round_trip_keeps_every_field),
    cmocka_unit_test(_a_stored_extent_is_held_to_its_range),
    cmocka_unit_test(_a_record_from_a_later_version_is_skipped_by_its_size),
    cmocka_unit_test(_a_newer_format_and_a_truncated_index_are_refused),
    cmocka_unit_test(_archive_round_trip_carries_jpegs_and_markdown),
    cmocka_unit_test(_removing_a_frame_takes_its_connectors_and_unlinks_sidecars),
    cmocka_unit_test(_snapshot_restore_round_trips_the_objects),
    cmocka_unit_test(_abandoning_a_gesture_keeps_the_renders_and_the_saved_state),
    cmocka_unit_test(_draw_order_edits_keep_the_list_sorted),
    cmocka_unit_test(_rotated_frames_answer_hit_tests_and_bounds),
    cmocka_unit_test(_connectors_route_between_cardinal_anchors),
    cmocka_unit_test(_anchored_routes_are_unchanged_by_free_ends),
    cmocka_unit_test(_a_free_line_routes_between_its_own_points),
    cmocka_unit_test(_coincident_free_ends_route_to_a_finite_point),
    cmocka_unit_test(_a_half_free_connector_aims_its_anchor_at_the_free_point),
    cmocka_unit_test(_a_free_tangent_places_the_control_point),
    cmocka_unit_test(_a_line_style_from_outside_is_made_sound),
    cmocka_unit_test(_a_seeded_curve_is_a_symmetric_arc),
    cmocka_unit_test(_free_ends_round_trip_through_the_index),
    cmocka_unit_test(_an_anchored_connector_is_written_as_before_free_ends),
    cmocka_unit_test(_no_object_loads_with_the_id_a_free_end_holds),
    cmocka_unit_test(_duplicating_a_line_offsets_its_points_and_waypoint),
    cmocka_unit_test(_a_dragged_line_end_snaps_to_the_grid_or_holds_its_angle),
    cmocka_unit_test(_bounds_hold_a_free_line_and_its_arrowhead),
    cmocka_unit_test(_the_page_list_reads_in_order_and_stores_by_code),
    cmocka_unit_test(_a_custom_page_is_the_canvass_own_size),
    cmocka_unit_test(_a_page_carries_a_margin_inside_it_and_a_bleed_outside),
    cmocka_unit_test(_a_frame_offers_its_corners_and_its_centre_as_anchors),
    cmocka_unit_test(_a_waypoint_bends_every_routing_through_it),
    cmocka_unit_test(_frames_snap_with_their_paddings_touching),
    cmocka_unit_test(_a_page_is_measured_in_points_whatever_it_is_rasterised_at),
    cmocka_unit_test(_a_spread_keeps_its_pages_together_and_opens_between_sheets),
    cmocka_unit_test(_opentype_features_round_trip_through_their_pango_spelling),
    cmocka_unit_test(_a_frame_covers_its_silhouette_and_not_its_corners),
    cmocka_unit_test(_paper_tiles_the_plane_from_the_origin),
    cmocka_unit_test(_layouts_arrange_without_moving_the_group),
    cmocka_unit_test(_a_layout_sorts_images_by_a_key_and_keeps_the_rest_after),
    cmocka_unit_test(_a_shape_round_trips_with_its_reserved_bytes),
    cmocka_unit_test(_a_cut_rectangle_keeps_its_cutout_nodes),
    cmocka_unit_test(_an_unknown_kind_is_kept_and_draws_nothing),
    cmocka_unit_test(_a_shape_style_from_outside_is_made_sound),
    cmocka_unit_test(_a_whole_canvas_arrangement_leaves_shapes_where_they_are),
    cmocka_unit_test(_a_fitted_outline_fills_the_frame_it_stands_in),
    cmocka_unit_test(_the_shape_bounds_are_the_polygon_envelope_s),
    cmocka_unit_test(_a_polygon_is_born_and_stays_at_its_own_ratio),
    cmocka_unit_test(_a_fillet_stops_where_its_neighbour_starts),
    cmocka_unit_test(_a_filled_star_is_not_picked_in_its_notches),
    cmocka_unit_test(_a_polygons_silhouette_is_its_outline_not_its_box),
    cmocka_unit_test(_a_polygon_is_never_cut_whatever_mask_it_carries),
    cmocka_unit_test(_colours_parse_and_format),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
