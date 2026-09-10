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
  canvas->gutter = 35.0f;
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
  dt_canvas_image_set_render(canvas, image, jpeg, 2048, 1365, 0x1234567890ABCDEFULL, 1725000000LL);
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
  canvas->gutter_color = dt_canvas_color(0.9f, 0.8f, 0.7f, 0.6f);
  canvas->grid_flags |= DT_CANVAS_GUTTER_VISIBLE;

  // The image gets a shadow of its own, some transparency and a polygon cutout with four nodes.
  image->shadow.color = dt_canvas_color(0.0f, 0.0f, 0.0f, 0.4f);
  image->shadow.offset_x = 3.0f;
  image->shadow.offset_y = 4.0f;
  image->shadow.blur = 2.0f;
  image->flags |= DT_CANVAS_OBJECT_FLAG_SHADOW_OVERRIDE;
  image->transparency = 0.25f;
  image->background = dt_canvas_color(0.2f, 0.4f, 0.6f, 0.8f);
  dt_canvas_mask_set_shape(canvas, image, DT_CANVAS_MASK_POLYGON);
  const float nodes[4 * DT_CANVAS_MASK_NODE_FLOATS] = { 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.0f, 0.0f,
                                                        0.9f, 0.1f, 0.9f, 0.1f, 0.9f, 0.1f, 0.0f, 0.0f,
                                                        0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 1.0f, 0.0f,
                                                        0.1f, 0.9f, 0.1f, 0.9f, 0.1f, 0.9f, 0.0f, 0.0f };
  dt_canvas_mask_set_nodes(canvas, image, nodes, 4);
  image->mask.feather = 0.12f;
  image->mask.flags = DT_CANVAS_MASK_INVERT;
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
  assert_int_equal(restored->grid_flags, DT_CANVAS_GRID_VISIBLE | DT_CANVAS_GRID_SNAP | DT_CANVAS_GUTTER_VISIBLE);
  assert_float_equal(restored->border_color.green, 0.5f, 1e-6);
  assert_float_equal(restored->gutter, 35.0f, 1e-6);
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
  assert_float_equal(image->rotation, 0.25, 1e-12);
  assert_int_equal(image->flags, DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE | DT_CANVAS_OBJECT_FLAG_SHADOW_OVERRIDE);
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
  assert_float_equal(image->mask.nodes[1 * DT_CANVAS_MASK_NODE_FLOATS + 0], 0.9f, 1e-6);
  assert_float_equal(image->mask.nodes[2 * DT_CANVAS_MASK_NODE_FLOATS + 1], 0.9f, 1e-6);
  assert_float_equal(image->mask.nodes[2 * DT_CANVAS_MASK_NODE_FLOATS + 6], 1.0f, 1e-6);
  assert_float_equal(restored->shadow.color.alpha, 0.6f, 1e-6);
  assert_float_equal(restored->shadow.offset_x, 11.0f, 1e-6);
  assert_float_equal(restored->shadow.offset_y, -7.0f, 1e-6);
  assert_float_equal(restored->shadow.blur, 5.5f, 1e-6);
  assert_float_equal(restored->gutter_color.red, 0.9f, 1e-6);
  assert_true((restored->grid_flags & DT_CANVAS_GUTTER_VISIBLE) != 0);

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

static void _frames_snap_next_to_their_neighbours_one_gutter_apart(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  canvas->gutter = 30.0f;
  dt_canvas_object_t *fixed = dt_canvas_add_text(canvas, 100.0, 100.0, 200.0, 100.0, ""); // spans x 0..200, y 50..150
  dt_canvas_object_t *moving = dt_canvas_add_text(canvas, 400.0, 400.0, 100.0, 100.0, "");
  GArray *exclude = g_array_new(FALSE, FALSE, sizeof(uint32_t));
  g_array_append_val(exclude, moving->id);
  double delta_x = 0.0;
  double delta_y = 0.0;

  // Its left edge 6 units short of one gutter past the fixed frame's right edge: pulled there.
  dt_canvas_rect_t box = { 224.0, 300.0, 100.0, 100.0 };
  assert_true(dt_canvas_snap_to_neighbours(canvas, &box, exclude, 8.0, DT_CANVAS_EDGE_ALL, &delta_x, &delta_y));
  assert_float_equal(delta_x, 6.0, 1e-9);
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

  // Masonry: two frames stacked one gutter apart offer their combined height.
  dt_canvas_object_t *below = dt_canvas_add_text(canvas, 100.0, 150.0 + 30.0 + 40.0, 200.0, 80.0, ""); // y 180..260
  height = 205.0;
  assert_true(dt_canvas_snap_size(canvas, exclude, 8.0, NULL, &height, NULL, &height_reference));
  assert_float_equal(height, 210.0, 1e-9); // 50..260
  assert_float_equal(height_reference.y, 50.0, 1e-9);
  assert_float_equal(height_reference.height, 210.0, 1e-9);
  (void)below;
  (void)fixed;
  g_array_free(exclude, TRUE);
  dt_canvas_free(canvas);
}

static void _paper_tiles_the_plane_from_the_origin(void **state)
{
  (void)state;
  dt_canvas_t *canvas = dt_canvas_new();
  double width = 0.0;
  double height = 0.0;
  assert_false(dt_canvas_paper_dimensions(canvas, &width, &height));
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
  canvas->gutter = 30.0f;
  dt_canvas_layout_apply(canvas, NULL, DT_CANVAS_LAYOUT_GRID, 0);
  const dt_canvas_rect_t after = dt_canvas_bounds(canvas);
  assert_float_equal(after.x, before.x, 1e-9);
  assert_float_equal(after.y, before.y, 1e-9);
  // The gap is the gutter: the second column starts one cell plus one gutter after the first.
  assert_float_equal(frames[1]->x - frames[0]->x, frames[0]->width + 30.0, 1e-9);
  // Two columns of two: the second frame sits to the right of the first, the third below it.
  assert_true(frames[1]->x > frames[0]->x);
  assert_float_equal(frames[1]->y, frames[0]->y, 1e-9);
  assert_float_equal(frames[2]->x, frames[0]->x, 1e-9);
  assert_true(frames[2]->y > frames[0]->y);

  dt_canvas_layout_apply(canvas, NULL, DT_CANVAS_LAYOUT_ROW, 0);
  for(int idx = 1; idx < 4; idx++)
  {
    assert_float_equal(frames[idx]->y, frames[0]->y, 1e-9);
    assert_true(frames[idx]->x > frames[idx - 1]->x);
  }

  dt_canvas_layout_apply(canvas, NULL, DT_CANVAS_LAYOUT_MASONRY, 3);
  assert_float_equal(frames[0]->width, frames[3]->width, 1e-9);
  assert_true(frames[3]->y > frames[0]->y); // the fourth wraps under the first column

  // Snapping rounds to the grid only when enabled.
  assert_float_equal(dt_canvas_snap(canvas, 74.0), 74.0, 1e-9);
  canvas->grid_flags |= DT_CANVAS_GRID_SNAP;
  assert_float_equal(dt_canvas_snap(canvas, 74.0), 50.0, 1e-9);
  assert_float_equal(dt_canvas_snap(canvas, 76.0), 100.0, 1e-9);

  // With snapping on and a gutter that is a grid multiple, every layout puts every frame's
  // top-left corner on the grid.
  canvas->gutter = 50.0f;
  static const dt_canvas_layout_t layouts[]
      = { DT_CANVAS_LAYOUT_GRID, DT_CANVAS_LAYOUT_MASONRY, DT_CANVAS_LAYOUT_ROW, DT_CANVAS_LAYOUT_COLUMN };
  for(size_t layout = 0; layout < G_N_ELEMENTS(layouts); layout++)
  {
    dt_canvas_layout_apply(canvas, NULL, layouts[layout], 2);
    for(int idx = 0; idx < 4; idx++)
    {
      const dt_canvas_rect_t bounds = dt_canvas_object_bounds(frames[idx]);
      assert_float_equal(fmod(bounds.x, 50.0), 0.0, 1e-6);
      assert_float_equal(fmod(bounds.y, 50.0), 0.0, 1e-6);
    }
  }
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
    cmocka_unit_test(_a_waypoint_bends_every_routing_through_it),
    cmocka_unit_test(_frames_snap_next_to_their_neighbours_one_gutter_apart),
    cmocka_unit_test(_paper_tiles_the_plane_from_the_origin),
    cmocka_unit_test(_layouts_arrange_without_moving_the_group),
    cmocka_unit_test(_colours_parse_and_format),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
