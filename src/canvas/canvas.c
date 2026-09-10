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
#include "canvas/canvas_zip.h"
#include "system/macros.h"
#include "system/mem_alloc.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define CANVAS_DEFAULT_GRID_SIZE 50.0f
#define CANVAS_DEFAULT_LONG_EDGE 2048
#define CANVAS_DEFAULT_JPEG_QUALITY 92
#define CANVAS_DEFAULT_FONT "Sans 12"
#define CANVAS_DEFAULT_TEXT_WIDTH 400.0
#define CANVAS_DEFAULT_TEXT_HEIGHT 200.0
#define CANVAS_DEFAULT_TEXT_PADDING 12.0f
#define CANVAS_DEFAULT_IMAGE_LONG_EDGE_UNITS 600.0
#define CANVAS_DUPLICATE_OFFSET 40.0
#define CANVAS_CONNECTOR_LINE_WIDTH 2.0f

/* --- objects: allocation ---------------------------------------------------- */

static void _object_free(gpointer data)
{
  dt_canvas_object_t *object = (dt_canvas_object_t *)data;
  if(IS_NULL_PTR(object)) return;
  if(object->kind == DT_CANVAS_OBJECT_IMAGE && !IS_NULL_PTR(object->image.jpeg))
  {
    g_bytes_unref(object->image.jpeg);
    object->image.jpeg = NULL;
  }
  if(object->kind == DT_CANVAS_OBJECT_TEXT)
  {
    dt_free(object->text.markdown);
  }
  dt_free(object);
}

static dt_canvas_object_t *_object_copy(const dt_canvas_object_t *source)
{
  dt_canvas_object_t *copy = g_new(dt_canvas_object_t, 1);
  memcpy(copy, source, sizeof(dt_canvas_object_t));
  if(copy->kind == DT_CANVAS_OBJECT_IMAGE && !IS_NULL_PTR(copy->image.jpeg))
  {
    copy->image.jpeg = g_bytes_ref(copy->image.jpeg);
  }
  if(copy->kind == DT_CANVAS_OBJECT_TEXT)
  {
    copy->text.markdown = g_strdup(IS_NULL_PTR(source->text.markdown) ? "" : source->text.markdown);
  }
  return copy;
}

static gint _object_compare_z(gconstpointer first, gconstpointer second)
{
  const dt_canvas_object_t *object_a = *(const dt_canvas_object_t *const *)first;
  const dt_canvas_object_t *object_b = *(const dt_canvas_object_t *const *)second;
  if(object_a->z != object_b->z) return object_a->z < object_b->z ? -1 : 1;
  if(object_a->id != object_b->id) return object_a->id < object_b->id ? -1 : 1;
  return 0;
}

static void _sort_objects(dt_canvas_t *canvas)
{
  g_ptr_array_sort(canvas->objects, _object_compare_z);
}

static int32_t _top_z(const dt_canvas_t *canvas)
{
  int32_t top = 0;
  for(guint idx = 0; idx < canvas->objects->len; idx++)
  {
    const dt_canvas_object_t *object = g_ptr_array_index(canvas->objects, idx);
    if(object->z > top) top = object->z;
  }
  return top;
}

static int32_t _bottom_z(const dt_canvas_t *canvas)
{
  int32_t bottom = 0;
  for(guint idx = 0; idx < canvas->objects->len; idx++)
  {
    const dt_canvas_object_t *object = g_ptr_array_index(canvas->objects, idx);
    if(object->z < bottom) bottom = object->z;
  }
  return bottom;
}

static dt_canvas_object_t *_object_new(dt_canvas_t *canvas, const dt_canvas_object_kind_t kind, const double x,
                                       const double y, const double width, const double height)
{
  dt_canvas_object_t *object = g_new0(dt_canvas_object_t, 1);
  object->id = canvas->next_id++;
  object->kind = kind;
  object->x = x;
  object->y = y;
  object->width = width;
  object->height = height;
  object->rotation = 0.0;
  object->z = _top_z(canvas) + 1;
  object->flags = DT_CANVAS_OBJECT_FLAG_NONE;
  object->border_color = canvas->border_color;
  object->border_width = canvas->border_width;
  g_ptr_array_add(canvas->objects, object);
  _sort_objects(canvas);
  dt_canvas_touch(canvas);
  return object;
}

/* --- lifecycle -------------------------------------------------------------- */

dt_canvas_t *dt_canvas_new(void)
{
  dt_canvas_t *canvas = g_new0(dt_canvas_t, 1);
  canvas->format_version = DT_CANVAS_FORMAT_VERSION;
  canvas->background = dt_canvas_color(0.18f, 0.18f, 0.18f, 1.0f);
  canvas->border_color = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
  canvas->border_width = 0.0f;
  canvas->grid_size = CANVAS_DEFAULT_GRID_SIZE;
  canvas->grid_flags = DT_CANVAS_GRID_VISIBLE;
  canvas->view_zoom = 1.0;
  canvas->view_x = 0.0;
  canvas->view_y = 0.0;
  g_strlcpy(canvas->default_font, CANVAS_DEFAULT_FONT, sizeof(canvas->default_font));
  canvas->image_long_edge = CANVAS_DEFAULT_LONG_EDGE;
  canvas->jpeg_quality = CANVAS_DEFAULT_JPEG_QUALITY;
  canvas->next_id = 1;
  canvas->objects = g_ptr_array_new_with_free_func(_object_free);
  canvas->path = NULL;
  canvas->dirty = FALSE;
  canvas->generation = 1;
  return canvas;
}

dt_canvas_t *dt_canvas_free(dt_canvas_t *canvas)
{
  if(IS_NULL_PTR(canvas)) return NULL;
  g_ptr_array_free(canvas->objects, TRUE);
  dt_free(canvas->path);
  dt_free(canvas);
  return NULL;
}

dt_canvas_t *dt_canvas_copy(const dt_canvas_t *canvas)
{
  if(IS_NULL_PTR(canvas)) return NULL;
  dt_canvas_t *copy = g_new(dt_canvas_t, 1);
  memcpy(copy, canvas, sizeof(dt_canvas_t));
  copy->objects = g_ptr_array_new_with_free_func(_object_free);
  for(guint idx = 0; idx < canvas->objects->len; idx++)
  {
    g_ptr_array_add(copy->objects, _object_copy(g_ptr_array_index(canvas->objects, idx)));
  }
  copy->path = g_strdup(canvas->path);
  return copy;
}

void dt_canvas_restore(dt_canvas_t *canvas, const dt_canvas_t *snapshot)
{
  if(IS_NULL_PTR(canvas) || IS_NULL_PTR(snapshot)) return;
  GPtrArray *old_objects = canvas->objects;
  char *path = canvas->path;
  const uint64_t generation = canvas->generation;
  memcpy(canvas, snapshot, sizeof(dt_canvas_t));
  canvas->path = path;
  canvas->objects = g_ptr_array_new_with_free_func(_object_free);
  for(guint idx = 0; idx < snapshot->objects->len; idx++)
  {
    g_ptr_array_add(canvas->objects, _object_copy(g_ptr_array_index(snapshot->objects, idx)));
  }
  g_ptr_array_free(old_objects, TRUE);
  canvas->generation = generation;
  dt_canvas_touch(canvas);
}

void dt_canvas_touch(dt_canvas_t *canvas)
{
  if(IS_NULL_PTR(canvas)) return;
  canvas->generation++;
  canvas->dirty = TRUE;
}

/* --- persistence ----------------------------------------------------------- */

dt_canvas_t *dt_canvas_load(const char *path, GError **error)
{
  dt_canvas_zip_reader_t *reader = dt_canvas_zip_reader_open(path);
  if(IS_NULL_PTR(reader))
  {
    g_set_error(error, DT_CANVAS_ERROR, DT_CANVAS_ERROR_NOT_A_CANVAS, "`%s' is not a canvas archive", path);
    return NULL;
  }
  GBytes *index = dt_canvas_zip_reader_get(reader, DT_CANVAS_ENTRY_INDEX);
  if(IS_NULL_PTR(index))
  {
    dt_canvas_zip_reader_close(reader);
    g_set_error(error, DT_CANVAS_ERROR, DT_CANVAS_ERROR_NOT_A_CANVAS, "`%s' has no canvas index", path);
    return NULL;
  }

  dt_canvas_t *canvas = dt_canvas_new();
  const gboolean parsed = dt_canvas_format_read_index(canvas, index, error);
  g_bytes_unref(index);
  if(!parsed)
  {
    dt_canvas_zip_reader_close(reader);
    return dt_canvas_free(canvas);
  }

  for(guint idx = 0; idx < canvas->objects->len; idx++)
  {
    dt_canvas_object_t *object = g_ptr_array_index(canvas->objects, idx);
    char entry[64];
    if(object->kind == DT_CANVAS_OBJECT_IMAGE)
    {
      snprintf(entry, sizeof(entry), DT_CANVAS_ENTRY_IMAGE_PATTERN, object->id);
      object->image.jpeg = dt_canvas_zip_reader_get(reader, entry);
      object->image.sync_status = DT_CANVAS_SYNC_UNKNOWN;
    }
    else if(object->kind == DT_CANVAS_OBJECT_TEXT)
    {
      snprintf(entry, sizeof(entry), DT_CANVAS_ENTRY_TEXT_PATTERN, object->id);
      GBytes *markdown = dt_canvas_zip_reader_get(reader, entry);
      if(!IS_NULL_PTR(markdown))
      {
        gsize markdown_size = 0;
        const char *text = g_bytes_get_data(markdown, &markdown_size);
        object->text.markdown = g_strndup(text, markdown_size);
        g_bytes_unref(markdown);
      }
      else
      {
        object->text.markdown = g_strdup("");
      }
    }
  }
  dt_canvas_zip_reader_close(reader);
  _sort_objects(canvas);
  canvas->path = g_strdup(path);
  canvas->dirty = FALSE;
  return canvas;
}

gboolean dt_canvas_save(dt_canvas_t *canvas, const char *path, GError **error)
{
  if(IS_NULL_PTR(canvas) || IS_NULL_PTR(path) || path[0] == '\0')
  {
    g_set_error(error, DT_CANVAS_ERROR, DT_CANVAS_ERROR_IO, "no path to save to");
    return FALSE;
  }
  dt_canvas_zip_writer_t *writer = dt_canvas_zip_writer_open(path);
  if(IS_NULL_PTR(writer))
  {
    g_set_error(error, DT_CANVAS_ERROR, DT_CANVAS_ERROR_IO, "cannot write `%s'", path);
    return FALSE;
  }

  gboolean ok = dt_canvas_zip_writer_add(writer, DT_CANVAS_ENTRY_MIMETYPE, DT_CANVAS_MIMETYPE,
                                         strlen(DT_CANVAS_MIMETYPE), FALSE);
  GBytes *index = dt_canvas_format_write_index(canvas);
  if(ok && !IS_NULL_PTR(index))
  {
    gsize index_size = 0;
    const void *index_data = g_bytes_get_data(index, &index_size);
    ok = dt_canvas_zip_writer_add(writer, DT_CANVAS_ENTRY_INDEX, index_data, index_size, TRUE);
  }
  else
  {
    ok = FALSE;
  }
  if(!IS_NULL_PTR(index)) g_bytes_unref(index);

  for(guint idx = 0; idx < canvas->objects->len && ok; idx++)
  {
    const dt_canvas_object_t *object = g_ptr_array_index(canvas->objects, idx);
    char entry[64];
    if(object->kind == DT_CANVAS_OBJECT_IMAGE && !IS_NULL_PTR(object->image.jpeg))
    {
      snprintf(entry, sizeof(entry), DT_CANVAS_ENTRY_IMAGE_PATTERN, object->id);
      gsize jpeg_size = 0;
      const void *jpeg_data = g_bytes_get_data(object->image.jpeg, &jpeg_size);
      ok = dt_canvas_zip_writer_add(writer, entry, jpeg_data, jpeg_size, FALSE);
    }
    else if(object->kind == DT_CANVAS_OBJECT_TEXT)
    {
      snprintf(entry, sizeof(entry), DT_CANVAS_ENTRY_TEXT_PATTERN, object->id);
      const char *markdown = dt_canvas_text_get_markdown(object);
      ok = dt_canvas_zip_writer_add(writer, entry, markdown, strlen(markdown), TRUE);
    }
  }

  const gboolean committed = dt_canvas_zip_writer_close(writer, ok);
  if(!ok || !committed)
  {
    g_set_error(error, DT_CANVAS_ERROR, DT_CANVAS_ERROR_IO, "writing `%s' failed", path);
    return FALSE;
  }
  if(canvas->path != path)
  {
    char *new_path = g_strdup(path);
    dt_free(canvas->path);
    canvas->path = new_path;
  }
  canvas->dirty = FALSE;
  return TRUE;
}

/* --- objects: CRUD ---------------------------------------------------------- */

dt_canvas_object_t *dt_canvas_add_image(dt_canvas_t *canvas, double x, double y, int32_t source_width,
                                        int32_t source_height)
{
  if(IS_NULL_PTR(canvas)) return NULL;
  double width = CANVAS_DEFAULT_IMAGE_LONG_EDGE_UNITS;
  double height = CANVAS_DEFAULT_IMAGE_LONG_EDGE_UNITS;
  if(source_width > 0 && source_height > 0)
  {
    if(source_width >= source_height)
      height = CANVAS_DEFAULT_IMAGE_LONG_EDGE_UNITS * (double)source_height / (double)source_width;
    else
      width = CANVAS_DEFAULT_IMAGE_LONG_EDGE_UNITS * (double)source_width / (double)source_height;
  }
  dt_canvas_object_t *object = _object_new(canvas, DT_CANVAS_OBJECT_IMAGE, x, y, width, height);
  object->image.imgid = -1;
  object->image.version = 0;
  object->image.film_id = -1;
  object->image.history_hash = 0;
  object->image.source_width = source_width;
  object->image.source_height = source_height;
  object->image.jpeg = NULL;
  object->image.sync_status = DT_CANVAS_SYNC_UNKNOWN;
  return object;
}

dt_canvas_object_t *dt_canvas_add_text(dt_canvas_t *canvas, double x, double y, double width, double height,
                                       const char *markdown)
{
  if(IS_NULL_PTR(canvas)) return NULL;
  if(width <= 0.0) width = CANVAS_DEFAULT_TEXT_WIDTH;
  if(height <= 0.0) height = CANVAS_DEFAULT_TEXT_HEIGHT;
  dt_canvas_object_t *object = _object_new(canvas, DT_CANVAS_OBJECT_TEXT, x, y, width, height);
  object->text.font[0] = '\0';
  object->text.text_color = dt_canvas_color(0.92f, 0.92f, 0.92f, 1.0f);
  object->text.background = dt_canvas_color(0.12f, 0.12f, 0.12f, 1.0f);
  object->text.source = DT_CANVAS_TEXT_SOURCE_MARKDOWN;
  object->text.linked_object = 0;
  object->text.padding = CANVAS_DEFAULT_TEXT_PADDING;
  object->text.markdown = g_strdup(IS_NULL_PTR(markdown) ? "" : markdown);
  return object;
}

dt_canvas_object_t *dt_canvas_add_connector(dt_canvas_t *canvas, uint32_t from_id, uint32_t to_id)
{
  if(IS_NULL_PTR(canvas) || from_id == to_id) return NULL;
  const dt_canvas_object_t *from = dt_canvas_find_object(canvas, from_id);
  const dt_canvas_object_t *to = dt_canvas_find_object(canvas, to_id);
  if(!dt_canvas_object_is_frame(from) || !dt_canvas_object_is_frame(to)) return NULL;
  dt_canvas_object_t *object = _object_new(canvas, DT_CANVAS_OBJECT_CONNECTOR, 0.0, 0.0, 0.0, 0.0);
  object->connector.from_id = from_id;
  object->connector.to_id = to_id;
  object->connector.style = DT_CANVAS_CONNECTOR_ARROW_END;
  object->connector.color = dt_canvas_color(0.85f, 0.85f, 0.85f, 1.0f);
  object->connector.line_width = CANVAS_CONNECTOR_LINE_WIDTH;
  object->connector.from_anchor = DT_CANVAS_ANCHOR_AUTO;
  object->connector.to_anchor = DT_CANVAS_ANCHOR_AUTO;
  object->connector.routing = DT_CANVAS_ROUTING_STRAIGHT;
  return object;
}

static gint _index_of(const dt_canvas_t *canvas, const uint32_t id)
{
  for(guint idx = 0; idx < canvas->objects->len; idx++)
  {
    const dt_canvas_object_t *object = g_ptr_array_index(canvas->objects, idx);
    if(object->id == id) return (gint)idx;
  }
  return -1;
}

gboolean dt_canvas_remove_object(dt_canvas_t *canvas, uint32_t id)
{
  if(IS_NULL_PTR(canvas)) return FALSE;
  const gint index = _index_of(canvas, id);
  if(index < 0) return FALSE;
  g_ptr_array_remove_index(canvas->objects, (guint)index);

  // Connectors attached to the departed frame go with it; a sidecar text frame stays but forgets its link.
  for(guint idx = canvas->objects->len; idx > 0; idx--)
  {
    dt_canvas_object_t *object = g_ptr_array_index(canvas->objects, idx - 1);
    if(object->kind == DT_CANVAS_OBJECT_CONNECTOR
       && (object->connector.from_id == id || object->connector.to_id == id))
    {
      g_ptr_array_remove_index(canvas->objects, idx - 1);
    }
    else if(object->kind == DT_CANVAS_OBJECT_TEXT && object->text.linked_object == id)
    {
      object->text.linked_object = 0;
      object->text.source = DT_CANVAS_TEXT_SOURCE_MARKDOWN;
    }
  }
  dt_canvas_touch(canvas);
  return TRUE;
}

dt_canvas_object_t *dt_canvas_duplicate_object(dt_canvas_t *canvas, uint32_t id)
{
  if(IS_NULL_PTR(canvas)) return NULL;
  const dt_canvas_object_t *source = dt_canvas_find_object(canvas, id);
  if(!dt_canvas_object_is_frame(source)) return NULL;
  dt_canvas_object_t *copy = _object_copy(source);
  copy->id = canvas->next_id++;
  copy->x += CANVAS_DUPLICATE_OFFSET;
  copy->y += CANVAS_DUPLICATE_OFFSET;
  copy->z = _top_z(canvas) + 1;
  g_ptr_array_add(canvas->objects, copy);
  _sort_objects(canvas);
  dt_canvas_touch(canvas);
  return copy;
}

dt_canvas_object_t *dt_canvas_find_object(const dt_canvas_t *canvas, uint32_t id)
{
  if(IS_NULL_PTR(canvas) || id == 0) return NULL;
  const gint index = _index_of(canvas, id);
  if(index < 0) return NULL;
  return g_ptr_array_index(canvas->objects, (guint)index);
}

guint dt_canvas_object_count(const dt_canvas_t *canvas)
{
  if(IS_NULL_PTR(canvas)) return 0;
  return canvas->objects->len;
}

dt_canvas_object_t *dt_canvas_object_at(const dt_canvas_t *canvas, guint index)
{
  if(IS_NULL_PTR(canvas) || index >= canvas->objects->len) return NULL;
  return g_ptr_array_index(canvas->objects, index);
}

void dt_canvas_object_to_front(dt_canvas_t *canvas, uint32_t id)
{
  dt_canvas_object_t *object = dt_canvas_find_object(canvas, id);
  if(IS_NULL_PTR(object)) return;
  object->z = _top_z(canvas) + 1;
  _sort_objects(canvas);
  dt_canvas_touch(canvas);
}

void dt_canvas_object_to_back(dt_canvas_t *canvas, uint32_t id)
{
  dt_canvas_object_t *object = dt_canvas_find_object(canvas, id);
  if(IS_NULL_PTR(object)) return;
  object->z = _bottom_z(canvas) - 1;
  _sort_objects(canvas);
  dt_canvas_touch(canvas);
}

static void _swap_z_with_neighbour(dt_canvas_t *canvas, const uint32_t id, const int direction)
{
  const gint index = _index_of(canvas, id);
  if(index < 0) return;
  const gint neighbour_index = index + direction;
  if(neighbour_index < 0 || neighbour_index >= (gint)canvas->objects->len) return;
  dt_canvas_object_t *object = g_ptr_array_index(canvas->objects, (guint)index);
  dt_canvas_object_t *neighbour = g_ptr_array_index(canvas->objects, (guint)neighbour_index);
  // Equal z values sort by id, so a plain swap of z would not move the object: renumber both.
  const int32_t object_z = object->z;
  const int32_t neighbour_z = neighbour->z;
  if(object_z == neighbour_z)
  {
    object->z = neighbour_z + direction;
  }
  else
  {
    object->z = neighbour_z;
    neighbour->z = object_z;
  }
  _sort_objects(canvas);
  dt_canvas_touch(canvas);
}

void dt_canvas_object_raise(dt_canvas_t *canvas, uint32_t id)
{
  if(IS_NULL_PTR(canvas)) return;
  _swap_z_with_neighbour(canvas, id, +1);
}

void dt_canvas_object_lower(dt_canvas_t *canvas, uint32_t id)
{
  if(IS_NULL_PTR(canvas)) return;
  _swap_z_with_neighbour(canvas, id, -1);
}

void dt_canvas_image_set_render(dt_canvas_t *canvas, dt_canvas_object_t *object, GBytes *jpeg, int32_t pixel_width,
                                int32_t pixel_height, uint64_t history_hash, int64_t rendered_at)
{
  if(IS_NULL_PTR(canvas) || IS_NULL_PTR(object) || object->kind != DT_CANVAS_OBJECT_IMAGE) return;
  GBytes *previous = object->image.jpeg;
  object->image.jpeg = IS_NULL_PTR(jpeg) ? NULL : g_bytes_ref(jpeg);
  if(!IS_NULL_PTR(previous)) g_bytes_unref(previous);
  object->image.pixel_width = pixel_width;
  object->image.pixel_height = pixel_height;
  object->image.history_hash = history_hash;
  object->image.rendered_at = rendered_at;
  object->image.sync_status = DT_CANVAS_SYNC_CURRENT;
  // The frame keeps its area and takes the render's aspect ratio, so a re-render after a crop reflows nothing else.
  if(pixel_width > 0 && pixel_height > 0)
  {
    const double area = object->width * object->height;
    const double ratio = (double)pixel_width / (double)pixel_height;
    object->width = sqrt(area * ratio);
    object->height = object->width / ratio;
  }
  dt_canvas_touch(canvas);
}

void dt_canvas_text_set_markdown(dt_canvas_t *canvas, dt_canvas_object_t *object, const char *markdown)
{
  if(IS_NULL_PTR(canvas) || IS_NULL_PTR(object) || object->kind != DT_CANVAS_OBJECT_TEXT) return;
  char *copy = g_strdup(IS_NULL_PTR(markdown) ? "" : markdown);
  dt_free(object->text.markdown);
  object->text.markdown = copy;
  dt_canvas_touch(canvas);
}

const char *dt_canvas_text_get_markdown(const dt_canvas_object_t *object)
{
  if(IS_NULL_PTR(object) || object->kind != DT_CANVAS_OBJECT_TEXT || IS_NULL_PTR(object->text.markdown)) return "";
  return object->text.markdown;
}

const char *dt_canvas_text_effective_font(const dt_canvas_t *canvas, const dt_canvas_object_t *object)
{
  if(!IS_NULL_PTR(object) && object->kind == DT_CANVAS_OBJECT_TEXT && object->text.font[0] != '\0')
    return object->text.font;
  if(!IS_NULL_PTR(canvas) && canvas->default_font[0] != '\0') return canvas->default_font;
  return CANVAS_DEFAULT_FONT;
}

void dt_canvas_object_effective_border(const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                                       dt_canvas_color_t *color, float *width)
{
  dt_canvas_color_t effective_color = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
  float effective_width = 0.0f;
  if(!IS_NULL_PTR(canvas))
  {
    effective_color = canvas->border_color;
    effective_width = canvas->border_width;
  }
  if(!IS_NULL_PTR(object) && (object->flags & DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE))
  {
    effective_color = object->border_color;
    effective_width = object->border_width;
  }
  if(!IS_NULL_PTR(color)) *color = effective_color;
  if(!IS_NULL_PTR(width)) *width = effective_width;
}

/* --- geometry --------------------------------------------------------------- */

gboolean dt_canvas_object_is_frame(const dt_canvas_object_t *object)
{
  if(IS_NULL_PTR(object)) return FALSE;
  return object->kind == DT_CANVAS_OBJECT_IMAGE || object->kind == DT_CANVAS_OBJECT_TEXT;
}

void dt_canvas_object_corners(const dt_canvas_object_t *object, double corners[8])
{
  const double half_width = object->width * 0.5;
  const double half_height = object->height * 0.5;
  const double cos_r = cos(object->rotation);
  const double sin_r = sin(object->rotation);
  const double local[8] = { -half_width, -half_height, half_width, -half_height,
                            half_width,  half_height,  -half_width, half_height };
  for(int idx = 0; idx < 4; idx++)
  {
    const double local_x = local[2 * idx];
    const double local_y = local[2 * idx + 1];
    corners[2 * idx] = object->x + local_x * cos_r - local_y * sin_r;
    corners[2 * idx + 1] = object->y + local_x * sin_r + local_y * cos_r;
  }
}

dt_canvas_rect_t dt_canvas_object_bounds(const dt_canvas_object_t *object)
{
  dt_canvas_rect_t bounds = { 0.0, 0.0, 0.0, 0.0 };
  if(IS_NULL_PTR(object)) return bounds;
  double corners[8];
  dt_canvas_object_corners(object, corners);
  double min_x = corners[0];
  double max_x = corners[0];
  double min_y = corners[1];
  double max_y = corners[1];
  for(int idx = 1; idx < 4; idx++)
  {
    min_x = fmin(min_x, corners[2 * idx]);
    max_x = fmax(max_x, corners[2 * idx]);
    min_y = fmin(min_y, corners[2 * idx + 1]);
    max_y = fmax(max_y, corners[2 * idx + 1]);
  }
  bounds.x = min_x;
  bounds.y = min_y;
  bounds.width = max_x - min_x;
  bounds.height = max_y - min_y;
  return bounds;
}

void dt_canvas_object_to_local(const dt_canvas_object_t *object, double x, double y, double *local_x,
                               double *local_y)
{
  const double offset_x = x - object->x;
  const double offset_y = y - object->y;
  const double cos_r = cos(-object->rotation);
  const double sin_r = sin(-object->rotation);
  *local_x = offset_x * cos_r - offset_y * sin_r;
  *local_y = offset_x * sin_r + offset_y * cos_r;
}

static double _segment_distance(const double px, const double py, const double ax, const double ay, const double bx,
                                const double by)
{
  const double segment_x = bx - ax;
  const double segment_y = by - ay;
  const double length_squared = segment_x * segment_x + segment_y * segment_y;
  double parameter = 0.0;
  if(length_squared > 0.0)
  {
    parameter = ((px - ax) * segment_x + (py - ay) * segment_y) / length_squared;
    parameter = CLAMP(parameter, 0.0, 1.0);
  }
  const double closest_x = ax + parameter * segment_x;
  const double closest_y = ay + parameter * segment_y;
  return hypot(px - closest_x, py - closest_y);
}

gboolean dt_canvas_object_contains(const dt_canvas_t *canvas, const dt_canvas_object_t *object, double x, double y,
                                   double tolerance)
{
  if(IS_NULL_PTR(object)) return FALSE;
  if(object->kind == DT_CANVAS_OBJECT_CONNECTOR)
  {
    dt_canvas_route_t route;
    if(!dt_canvas_connector_route(canvas, object, &route)) return FALSE;
    const double reach = tolerance + object->connector.line_width;
    for(int idx = 0; idx + 1 < route.point_count; idx++)
    {
      if(_segment_distance(x, y, route.points[2 * idx], route.points[2 * idx + 1], route.points[2 * idx + 2],
                           route.points[2 * idx + 3])
         <= reach)
        return TRUE;
    }
    return FALSE;
  }
  double local_x = 0.0;
  double local_y = 0.0;
  dt_canvas_object_to_local(object, x, y, &local_x, &local_y);
  return fabs(local_x) <= object->width * 0.5 + tolerance && fabs(local_y) <= object->height * 0.5 + tolerance;
}

/* --- connectors: anchors and routes -------------------------------------------- */

static void _anchor_local(const dt_canvas_anchor_t anchor, const double width, const double height, double *local_x,
                          double *local_y, double *normal_x, double *normal_y)
{
  *local_x = 0.0;
  *local_y = 0.0;
  *normal_x = 0.0;
  *normal_y = 0.0;
  switch(anchor)
  {
    case DT_CANVAS_ANCHOR_EAST:
      *local_x = width * 0.5;
      *normal_x = 1.0;
      break;
    case DT_CANVAS_ANCHOR_SOUTH:
      *local_y = height * 0.5;
      *normal_y = 1.0;
      break;
    case DT_CANVAS_ANCHOR_WEST:
      *local_x = -width * 0.5;
      *normal_x = -1.0;
      break;
    case DT_CANVAS_ANCHOR_NORTH:
    default:
      *local_y = -height * 0.5;
      *normal_y = -1.0;
      break;
  }
}

static void _anchor_world(const dt_canvas_object_t *frame, const dt_canvas_anchor_t anchor, double *x, double *y,
                          double *normal_x, double *normal_y)
{
  double local_x = 0.0;
  double local_y = 0.0;
  double local_normal_x = 0.0;
  double local_normal_y = 0.0;
  _anchor_local(anchor, frame->width, frame->height, &local_x, &local_y, &local_normal_x, &local_normal_y);
  const double cos_r = cos(frame->rotation);
  const double sin_r = sin(frame->rotation);
  *x = frame->x + local_x * cos_r - local_y * sin_r;
  *y = frame->y + local_x * sin_r + local_y * cos_r;
  *normal_x = local_normal_x * cos_r - local_normal_y * sin_r;
  *normal_y = local_normal_x * sin_r + local_normal_y * cos_r;
}

void dt_canvas_object_anchor_point(const dt_canvas_object_t *frame, dt_canvas_anchor_t anchor, double target_x,
                                   double target_y, double *x, double *y, double *normal_x, double *normal_y)
{
  if(IS_NULL_PTR(frame)) return;
  if(anchor != DT_CANVAS_ANCHOR_AUTO)
  {
    _anchor_world(frame, anchor, x, y, normal_x, normal_y);
    return;
  }
  // AUTO: the cardinal point nearest the other end's centre.
  static const dt_canvas_anchor_t candidates[4]
      = { DT_CANVAS_ANCHOR_NORTH, DT_CANVAS_ANCHOR_EAST, DT_CANVAS_ANCHOR_SOUTH, DT_CANVAS_ANCHOR_WEST };
  double best_distance = INFINITY;
  for(int idx = 0; idx < 4; idx++)
  {
    double candidate_x = 0.0;
    double candidate_y = 0.0;
    double candidate_normal_x = 0.0;
    double candidate_normal_y = 0.0;
    _anchor_world(frame, candidates[idx], &candidate_x, &candidate_y, &candidate_normal_x, &candidate_normal_y);
    const double distance = hypot(candidate_x - target_x, candidate_y - target_y);
    if(distance < best_distance)
    {
      best_distance = distance;
      *x = candidate_x;
      *y = candidate_y;
      *normal_x = candidate_normal_x;
      *normal_y = candidate_normal_y;
    }
  }
}

static void _route_add_point(dt_canvas_route_t *route, const double x, const double y)
{
  if(route->point_count >= DT_CANVAS_ROUTE_MAX_POINTS) return;
  route->points[2 * route->point_count] = x;
  route->points[2 * route->point_count + 1] = y;
  route->point_count++;
}

/** Square routing: a stub along each normal, then legs that are horizontal or vertical. */
static void _route_square(dt_canvas_route_t *route)
{
  const double distance = hypot(route->to_x - route->from_x, route->to_y - route->from_y);
  const double stub = CLAMP(distance * 0.25, 20.0, 60.0);
  const double start_x = route->from_x + route->from_normal_x * stub;
  const double start_y = route->from_y + route->from_normal_y * stub;
  const double end_x = route->to_x + route->to_normal_x * stub;
  const double end_y = route->to_y + route->to_normal_y * stub;
  const gboolean from_horizontal = fabs(route->from_normal_x) >= fabs(route->from_normal_y);
  const gboolean to_horizontal = fabs(route->to_normal_x) >= fabs(route->to_normal_y);
  _route_add_point(route, route->from_x, route->from_y);
  _route_add_point(route, start_x, start_y);
  if(from_horizontal && to_horizontal)
  {
    const double mid_x = (start_x + end_x) * 0.5;
    _route_add_point(route, mid_x, start_y);
    _route_add_point(route, mid_x, end_y);
  }
  else if(!from_horizontal && !to_horizontal)
  {
    const double mid_y = (start_y + end_y) * 0.5;
    _route_add_point(route, start_x, mid_y);
    _route_add_point(route, end_x, mid_y);
  }
  else if(from_horizontal)
  {
    _route_add_point(route, end_x, start_y);
  }
  else
  {
    _route_add_point(route, start_x, end_y);
  }
  _route_add_point(route, end_x, end_y);
  _route_add_point(route, route->to_x, route->to_y);
}

/** Cubic routing: control points along the normals, flattened for hit tests. */
static void _route_cubic(dt_canvas_route_t *route)
{
  const double distance = hypot(route->to_x - route->from_x, route->to_y - route->from_y);
  const double reach = fmax(40.0, distance * 0.4);
  route->control1_x = route->from_x + route->from_normal_x * reach;
  route->control1_y = route->from_y + route->from_normal_y * reach;
  route->control2_x = route->to_x + route->to_normal_x * reach;
  route->control2_y = route->to_y + route->to_normal_y * reach;
  const int segments = DT_CANVAS_ROUTE_MAX_POINTS - 1;
  for(int idx = 0; idx <= segments; idx++)
  {
    const double parameter = (double)idx / (double)segments;
    const double remaining = 1.0 - parameter;
    const double weight0 = remaining * remaining * remaining;
    const double weight1 = 3.0 * remaining * remaining * parameter;
    const double weight2 = 3.0 * remaining * parameter * parameter;
    const double weight3 = parameter * parameter * parameter;
    _route_add_point(route,
                     weight0 * route->from_x + weight1 * route->control1_x + weight2 * route->control2_x
                         + weight3 * route->to_x,
                     weight0 * route->from_y + weight1 * route->control1_y + weight2 * route->control2_y
                         + weight3 * route->to_y);
  }
}

gboolean dt_canvas_connector_route(const dt_canvas_t *canvas, const dt_canvas_object_t *connector,
                                   dt_canvas_route_t *route)
{
  if(IS_NULL_PTR(connector) || IS_NULL_PTR(route) || connector->kind != DT_CANVAS_OBJECT_CONNECTOR) return FALSE;
  const dt_canvas_object_t *from = dt_canvas_find_object(canvas, connector->connector.from_id);
  const dt_canvas_object_t *to = dt_canvas_find_object(canvas, connector->connector.to_id);
  if(!dt_canvas_object_is_frame(from) || !dt_canvas_object_is_frame(to)) return FALSE;
  memset(route, 0, sizeof(*route));
  route->routing = connector->connector.routing;
  dt_canvas_object_anchor_point(from, (dt_canvas_anchor_t)connector->connector.from_anchor, to->x, to->y,
                                &route->from_x, &route->from_y, &route->from_normal_x, &route->from_normal_y);
  dt_canvas_object_anchor_point(to, (dt_canvas_anchor_t)connector->connector.to_anchor, from->x, from->y,
                                &route->to_x, &route->to_y, &route->to_normal_x, &route->to_normal_y);
  switch(route->routing)
  {
    case DT_CANVAS_ROUTING_SQUARE:
      _route_square(route);
      break;
    case DT_CANVAS_ROUTING_CUBIC:
      _route_cubic(route);
      break;
    case DT_CANVAS_ROUTING_STRAIGHT:
    default:
      route->routing = DT_CANVAS_ROUTING_STRAIGHT;
      _route_add_point(route, route->from_x, route->from_y);
      _route_add_point(route, route->to_x, route->to_y);
      break;
  }
  return route->point_count >= 2;
}

gboolean dt_canvas_connector_endpoints(const dt_canvas_t *canvas, const dt_canvas_object_t *connector,
                                       double *from_x, double *from_y, double *to_x, double *to_y)
{
  dt_canvas_route_t route;
  if(!dt_canvas_connector_route(canvas, connector, &route)) return FALSE;
  *from_x = route.from_x;
  *from_y = route.from_y;
  *to_x = route.to_x;
  *to_y = route.to_y;
  return TRUE;
}

dt_canvas_rect_t dt_canvas_bounds(const dt_canvas_t *canvas)
{
  dt_canvas_rect_t bounds = { 0.0, 0.0, 0.0, 0.0 };
  if(IS_NULL_PTR(canvas)) return bounds;
  gboolean first = TRUE;
  double min_x = 0.0;
  double min_y = 0.0;
  double max_x = 0.0;
  double max_y = 0.0;
  for(guint idx = 0; idx < canvas->objects->len; idx++)
  {
    const dt_canvas_object_t *object = g_ptr_array_index(canvas->objects, idx);
    if(!dt_canvas_object_is_frame(object) || (object->flags & DT_CANVAS_OBJECT_FLAG_HIDDEN)) continue;
    const dt_canvas_rect_t object_bounds = dt_canvas_object_bounds(object);
    if(first)
    {
      min_x = object_bounds.x;
      min_y = object_bounds.y;
      max_x = object_bounds.x + object_bounds.width;
      max_y = object_bounds.y + object_bounds.height;
      first = FALSE;
    }
    else
    {
      min_x = fmin(min_x, object_bounds.x);
      min_y = fmin(min_y, object_bounds.y);
      max_x = fmax(max_x, object_bounds.x + object_bounds.width);
      max_y = fmax(max_y, object_bounds.y + object_bounds.height);
    }
  }
  if(first) return bounds;
  bounds.x = min_x;
  bounds.y = min_y;
  bounds.width = max_x - min_x;
  bounds.height = max_y - min_y;
  return bounds;
}

double dt_canvas_snap(const dt_canvas_t *canvas, double value)
{
  if(IS_NULL_PTR(canvas) || !(canvas->grid_flags & DT_CANVAS_GRID_SNAP) || canvas->grid_size <= 0.0f) return value;
  return round(value / canvas->grid_size) * canvas->grid_size;
}

dt_canvas_object_t *dt_canvas_pick(const dt_canvas_t *canvas, double x, double y, double tolerance)
{
  if(IS_NULL_PTR(canvas)) return NULL;
  for(guint idx = canvas->objects->len; idx > 0; idx--)
  {
    dt_canvas_object_t *object = g_ptr_array_index(canvas->objects, idx - 1);
    if(object->flags & DT_CANVAS_OBJECT_FLAG_HIDDEN) continue;
    if(dt_canvas_object_contains(canvas, object, x, y, tolerance)) return object;
  }
  return NULL;
}

/* --- layout ----------------------------------------------------------------- */

static GPtrArray *_layout_frames(const dt_canvas_t *canvas, const GArray *ids)
{
  GPtrArray *frames = g_ptr_array_new();
  if(IS_NULL_PTR(ids))
  {
    for(guint idx = 0; idx < canvas->objects->len; idx++)
    {
      dt_canvas_object_t *object = g_ptr_array_index(canvas->objects, idx);
      if(dt_canvas_object_is_frame(object) && !(object->flags & DT_CANVAS_OBJECT_FLAG_HIDDEN))
        g_ptr_array_add(frames, object);
    }
  }
  else
  {
    for(guint idx = 0; idx < ids->len; idx++)
    {
      dt_canvas_object_t *object = dt_canvas_find_object(canvas, g_array_index(ids, uint32_t, idx));
      if(dt_canvas_object_is_frame(object)) g_ptr_array_add(frames, object);
    }
  }
  return frames;
}

static void _layout_anchor(GPtrArray *frames, double *anchor_x, double *anchor_y)
{
  *anchor_x = INFINITY;
  *anchor_y = INFINITY;
  for(guint idx = 0; idx < frames->len; idx++)
  {
    const dt_canvas_rect_t bounds = dt_canvas_object_bounds(g_ptr_array_index(frames, idx));
    *anchor_x = fmin(*anchor_x, bounds.x);
    *anchor_y = fmin(*anchor_y, bounds.y);
  }
}

static void _layout_grid(GPtrArray *frames, const double anchor_x, const double anchor_y, const double gap)
{
  double cell_width = 0.0;
  double cell_height = 0.0;
  for(guint idx = 0; idx < frames->len; idx++)
  {
    const dt_canvas_object_t *frame = g_ptr_array_index(frames, idx);
    cell_width = fmax(cell_width, frame->width);
    cell_height = fmax(cell_height, frame->height);
  }
  const guint columns = (guint)ceil(sqrt((double)frames->len));
  for(guint idx = 0; idx < frames->len; idx++)
  {
    dt_canvas_object_t *frame = g_ptr_array_index(frames, idx);
    const guint col = idx % columns;
    const guint row = idx / columns;
    frame->rotation = 0.0;
    frame->x = anchor_x + col * (cell_width + gap) + cell_width * 0.5;
    frame->y = anchor_y + row * (cell_height + gap) + cell_height * 0.5;
  }
}

static void _layout_masonry(GPtrArray *frames, const double anchor_x, const double anchor_y, const double gap,
                            const int column_count)
{
  const int columns = column_count < 1 ? 1 : column_count;
  double column_width = 0.0;
  for(guint idx = 0; idx < frames->len; idx++)
  {
    const dt_canvas_object_t *frame = g_ptr_array_index(frames, idx);
    column_width = fmax(column_width, frame->width);
  }
  double *column_heights = g_new0(double, columns);
  for(guint idx = 0; idx < frames->len; idx++)
  {
    dt_canvas_object_t *frame = g_ptr_array_index(frames, idx);
    int shortest = 0;
    for(int col = 1; col < columns; col++)
    {
      if(column_heights[col] < column_heights[shortest]) shortest = col;
    }
    const double ratio = frame->height > 0.0 ? frame->width / frame->height : 1.0;
    frame->rotation = 0.0;
    frame->width = column_width;
    frame->height = column_width / ratio;
    frame->x = anchor_x + shortest * (column_width + gap) + column_width * 0.5;
    frame->y = anchor_y + column_heights[shortest] + frame->height * 0.5;
    column_heights[shortest] += frame->height + gap;
  }
  dt_free(column_heights);
}

static void _layout_row(GPtrArray *frames, const double anchor_x, const double anchor_y, const double gap)
{
  double row_height = 0.0;
  for(guint idx = 0; idx < frames->len; idx++)
  {
    const dt_canvas_object_t *frame = g_ptr_array_index(frames, idx);
    row_height = fmax(row_height, frame->height);
  }
  double cursor_x = anchor_x;
  for(guint idx = 0; idx < frames->len; idx++)
  {
    dt_canvas_object_t *frame = g_ptr_array_index(frames, idx);
    const double ratio = frame->height > 0.0 ? frame->width / frame->height : 1.0;
    frame->rotation = 0.0;
    frame->height = row_height;
    frame->width = row_height * ratio;
    frame->x = cursor_x + frame->width * 0.5;
    frame->y = anchor_y + row_height * 0.5;
    cursor_x += frame->width + gap;
  }
}

static void _layout_column(GPtrArray *frames, const double anchor_x, const double anchor_y, const double gap)
{
  double column_width = 0.0;
  for(guint idx = 0; idx < frames->len; idx++)
  {
    const dt_canvas_object_t *frame = g_ptr_array_index(frames, idx);
    column_width = fmax(column_width, frame->width);
  }
  double cursor_y = anchor_y;
  for(guint idx = 0; idx < frames->len; idx++)
  {
    dt_canvas_object_t *frame = g_ptr_array_index(frames, idx);
    const double ratio = frame->height > 0.0 ? frame->width / frame->height : 1.0;
    frame->rotation = 0.0;
    frame->width = column_width;
    frame->height = column_width / ratio;
    frame->x = anchor_x + column_width * 0.5;
    frame->y = cursor_y + frame->height * 0.5;
    cursor_y += frame->height + gap;
  }
}

void dt_canvas_layout_apply(dt_canvas_t *canvas, const GArray *ids, dt_canvas_layout_t layout, int columns,
                            double gap)
{
  if(IS_NULL_PTR(canvas)) return;
  GPtrArray *frames = _layout_frames(canvas, ids);
  if(frames->len == 0)
  {
    g_ptr_array_free(frames, TRUE);
    return;
  }
  double anchor_x = 0.0;
  double anchor_y = 0.0;
  _layout_anchor(frames, &anchor_x, &anchor_y);
  if(gap < 0.0) gap = 0.0;
  switch(layout)
  {
    case DT_CANVAS_LAYOUT_MASONRY:
      _layout_masonry(frames, anchor_x, anchor_y, gap, columns);
      break;
    case DT_CANVAS_LAYOUT_ROW:
      _layout_row(frames, anchor_x, anchor_y, gap);
      break;
    case DT_CANVAS_LAYOUT_COLUMN:
      _layout_column(frames, anchor_x, anchor_y, gap);
      break;
    case DT_CANVAS_LAYOUT_GRID:
    default:
      _layout_grid(frames, anchor_x, anchor_y, gap);
      break;
  }
  g_ptr_array_free(frames, TRUE);
  dt_canvas_touch(canvas);
}

/* --- colours ---------------------------------------------------------------- */

dt_canvas_color_t dt_canvas_color(float red, float green, float blue, float alpha)
{
  dt_canvas_color_t color;
  color.red = red;
  color.green = green;
  color.blue = blue;
  color.alpha = alpha;
  return color;
}

static int _hex_value(const char digit)
{
  if(digit >= '0' && digit <= '9') return digit - '0';
  if(digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
  if(digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
  return -1;
}

gboolean dt_canvas_color_parse(const char *text, dt_canvas_color_t *color)
{
  if(IS_NULL_PTR(text) || IS_NULL_PTR(color) || text[0] != '#') return FALSE;
  const size_t length = strlen(text + 1);
  if(length != 6 && length != 8) return FALSE;
  int channels[4] = { 0, 0, 0, 255 };
  for(size_t idx = 0; idx < length; idx += 2)
  {
    const int high = _hex_value(text[1 + idx]);
    const int low = _hex_value(text[2 + idx]);
    if(high < 0 || low < 0) return FALSE;
    channels[idx / 2] = high * 16 + low;
  }
  *color = dt_canvas_color(channels[0] / 255.0f, channels[1] / 255.0f, channels[2] / 255.0f, channels[3] / 255.0f);
  return TRUE;
}

void dt_canvas_color_format(const dt_canvas_color_t *color, char *out, size_t out_len)
{
  if(IS_NULL_PTR(out) || out_len == 0) return;
  if(IS_NULL_PTR(color))
  {
    out[0] = '\0';
    return;
  }
  const int red = (int)lroundf(CLAMP(color->red, 0.0f, 1.0f) * 255.0f);
  const int green = (int)lroundf(CLAMP(color->green, 0.0f, 1.0f) * 255.0f);
  const int blue = (int)lroundf(CLAMP(color->blue, 0.0f, 1.0f) * 255.0f);
  const int alpha = (int)lroundf(CLAMP(color->alpha, 0.0f, 1.0f) * 255.0f);
  snprintf(out, out_len, "#%02x%02x%02x%02x", red, green, blue, alpha);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
