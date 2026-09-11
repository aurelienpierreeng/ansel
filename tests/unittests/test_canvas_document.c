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
#include "canvas/canvas_format.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <setjmp.h>
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
 * A frame offers nine places to attach a connector: the four edge midpoints, the four
 * corners, and the centre. They are the frame's own points, so they turn with it.
 */
/**
 * The page list is ordered for reading and stored by code, so a size appended to the enum --
 * A0 and A1 were -- shows where it belongs without moving any saved document's page.
 */
static void _the_page_list_reads_in_order_and_stores_by_code(void **state)
{
  (void)state;
  // The list starts at "None" and then runs down the ISO A series from the largest.
  assert_int_equal(dt_canvas_paper_code(0), DT_CANVAS_PAPER_NONE);
  assert_int_equal(dt_canvas_paper_code(1), DT_CANVAS_PAPER_A0);
  assert_int_equal(dt_canvas_paper_code(2), DT_CANVAS_PAPER_A1);
  assert_int_equal(dt_canvas_paper_code(3), DT_CANVAS_PAPER_A2);
  assert_int_equal(dt_canvas_paper_code(7), DT_CANVAS_PAPER_A6);
  assert_string_equal(dt_canvas_paper_name(1), "A0");
  // Every code the list offers comes back to the row it was shown on, and every one of them
  // is a size -- a stored value with no size behind it would silently become no pages at all.
  for(int position = 1; position < dt_canvas_paper_count(); position++)
  {
    const uint32_t code = dt_canvas_paper_code(position);
    assert_int_equal(dt_canvas_paper_position(code), position);
    double width = 0.0;
    double height = 0.0;
    assert_true(dt_canvas_paper_points(code, &width, &height));
    assert_true(width > 0.0 && height > 0.0);
  }
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
 * A canvas unit is a display pixel and the canvas says how many go to the inch, so a sheet of
 * paper is scaled by that and a screen format is not. Read as points, as both were, an
 * Instagram story came out 1080 units against an A4's 595 -- nearly twice the sheet, for
 * something that fits in a hand.
 */
static void _a_sheet_of_paper_scales_with_the_resolution_and_a_screen_format_does_not(void **state)
{
  (void)state;
  assert_true(dt_canvas_paper_is_physical(DT_CANVAS_PAPER_A4));
  assert_false(dt_canvas_paper_is_physical(DT_CANVAS_PAPER_STORY));

  dt_canvas_t *canvas = dt_canvas_new();
  // A document from before the field holds zero and reads as 72, which is the geometry it was
  // laid out with: every page size is then its own number outright.
  canvas->resolution = 0.0f;
  assert_float_equal(dt_canvas_resolution(canvas), 72.0, 1e-9);
  double paper_width = 0.0;
  double screen_width = 0.0;
  canvas->paper_size = DT_CANVAS_PAPER_A4;
  assert_true(dt_canvas_paper_dimensions(canvas, &paper_width, NULL));
  canvas->paper_size = DT_CANVAS_PAPER_STORY;
  assert_true(dt_canvas_paper_dimensions(canvas, &screen_width, NULL));
  assert_float_equal(paper_width, 595.0, 1e-9);
  assert_float_equal(screen_width, 1080.0, 1e-9);
  assert_true(screen_width > paper_width); // the old reading, and why it had to change

  // At the resolution a new canvas carries, the sheet is the larger of the two and the screen
  // format has not moved: it was already in the plane's own unit.
  canvas->resolution = 300.0f;
  canvas->paper_size = DT_CANVAS_PAPER_A4;
  assert_true(dt_canvas_paper_dimensions(canvas, &paper_width, NULL));
  canvas->paper_size = DT_CANVAS_PAPER_STORY;
  assert_true(dt_canvas_paper_dimensions(canvas, &screen_width, NULL));
  assert_float_equal(paper_width, 595.0 * 300.0 / 72.0, 1e-6);
  assert_float_equal(screen_width, 1080.0, 1e-9);
  assert_true(paper_width > screen_width);
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
    cmocka_unit_test(_a_record_from_a_later_version_is_skipped_by_its_size),
    cmocka_unit_test(_a_newer_format_and_a_truncated_index_are_refused),
    cmocka_unit_test(_archive_round_trip_carries_jpegs_and_markdown),
    cmocka_unit_test(_removing_a_frame_takes_its_connectors_and_unlinks_sidecars),
    cmocka_unit_test(_snapshot_restore_round_trips_the_objects),
    cmocka_unit_test(_draw_order_edits_keep_the_list_sorted),
    cmocka_unit_test(_rotated_frames_answer_hit_tests_and_bounds),
    cmocka_unit_test(_connectors_route_between_cardinal_anchors),
    cmocka_unit_test(_the_page_list_reads_in_order_and_stores_by_code),
    cmocka_unit_test(_a_page_carries_a_margin_inside_it_and_a_bleed_outside),
    cmocka_unit_test(_a_frame_offers_its_corners_and_its_centre_as_anchors),
    cmocka_unit_test(_a_waypoint_bends_every_routing_through_it),
    cmocka_unit_test(_frames_snap_with_their_paddings_touching),
    cmocka_unit_test(_a_sheet_of_paper_scales_with_the_resolution_and_a_screen_format_does_not),
    cmocka_unit_test(_a_spread_keeps_its_pages_together_and_opens_between_sheets),
    cmocka_unit_test(_paper_tiles_the_plane_from_the_origin),
    cmocka_unit_test(_layouts_arrange_without_moving_the_group),
    cmocka_unit_test(_a_layout_sorts_images_by_a_key_and_keeps_the_rest_after),
    cmocka_unit_test(_colours_parse_and_format),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
