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

#include <float.h>
#include <glib/gi18n.h>
#include <librsvg/rsvg.h>
#include <math.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>

#define CANVAS_DEFAULT_GRID_SIZE 50.0f
#define CANVAS_DEFAULT_PADDING 20.0f
/**
 * Canvas units to the inch on a new canvas. The plane is measured in display pixels, so this
 * is what turns a sheet of paper into a size on it: 300 is the print standard and puts an A4
 * at 2480 units against an Instagram reel's 1080, which is the right way round. A document
 * from before the field reads as 72 and keeps the geometry it was laid out with.
 */
#define CANVAS_DEFAULT_RESOLUTION 300.0f
#define CANVAS_DEFAULT_LONG_EDGE 2048
#define CANVAS_DEFAULT_JPEG_QUALITY 92
#define CANVAS_DEFAULT_FONT "Sans 12"
#define CANVAS_DEFAULT_TEXT_WIDTH 400.0
#define CANVAS_DEFAULT_TEXT_HEIGHT 200.0
/**
 * A new picture's long edge, in points: two inches, about a third of A4's width. It was 600
 * when a unit was a three-hundredth of an inch, which is the same two inches -- the number
 * changed with the unit and the picture did not.
 */
#define CANVAS_DEFAULT_IMAGE_LONG_EDGE_UNITS 144.0
#define CANVAS_DUPLICATE_OFFSET 40.0
/**
 * How far out a free end's automatic control point sits, as a fraction of the distance to where
 * the route heads next, and how long a seeded arc's tangents are against its chord. It stands in
 * for the anchored ends' 40-unit floor, which a free end must not take: a line a few units long
 * would otherwise loop out past both of its ends.
 */
#define CANVAS_FREE_REACH 0.4
#define CANVAS_SEED_ANGLE (M_PI / 6.0) ///< how far to one side of the chord a seeded arc leaves and arrives
#define CANVAS_DEFAULT_SHADOW_OFFSET 8.0f
#define CANVAS_MASK_MAX_NODES 512u

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
  if(object->kind == DT_CANVAS_OBJECT_MAP && !IS_NULL_PTR(object->map.jpeg))
  {
    g_bytes_unref(object->map.jpeg);
    object->map.jpeg = NULL;
  }
  if(object->kind == DT_CANVAS_OBJECT_SVG && !IS_NULL_PTR(object->svg.svg))
  {
    g_bytes_unref(object->svg.svg);
    object->svg.svg = NULL;
  }
  dt_canvas_mask_clear(object);
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
  if(copy->kind == DT_CANVAS_OBJECT_MAP && !IS_NULL_PTR(copy->map.jpeg))
  {
    copy->map.jpeg = g_bytes_ref(copy->map.jpeg);
  }
  if(copy->kind == DT_CANVAS_OBJECT_SVG && !IS_NULL_PTR(copy->svg.svg))
  {
    copy->svg.svg = g_bytes_ref(copy->svg.svg);
  }
  // The nodes are the copy's own: the source keeps its array.
  copy->mask.nodes = NULL;
  copy->mask.node_count = 0;
  if(source->mask.node_count > 0 && !IS_NULL_PTR(source->mask.nodes))
  {
    const size_t floats = (size_t)source->mask.node_count * DT_CANVAS_MASK_NODE_FLOATS;
    copy->mask.nodes = g_new(float, floats);
    memcpy(copy->mask.nodes, source->mask.nodes, floats * sizeof(float));
    copy->mask.node_count = source->mask.node_count;
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

static uint64_t _next_serial(void)
{
  static gint serial = 0;
  return (uint64_t)g_atomic_int_add(&serial, 1) + 1u;
}

dt_canvas_t *dt_canvas_new(void)
{
  dt_canvas_t *canvas = g_new0(dt_canvas_t, 1);
  canvas->serial = _next_serial();
  canvas->format_version = DT_CANVAS_FORMAT_VERSION;
  canvas->background = dt_canvas_color(0.18f, 0.18f, 0.18f, 1.0f);
  canvas->border_color = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
  canvas->border_width = 0.0f;
  canvas->grid_size = CANVAS_DEFAULT_GRID_SIZE;
  canvas->grid_flags = DT_CANVAS_GRID_VISIBLE;
  canvas->padding = CANVAS_DEFAULT_PADDING;
  canvas->background_style = DT_CANVAS_BACKGROUND_PLAIN;
  canvas->grid_color = dt_canvas_color(0.5f, 0.5f, 0.5f, 1.0f);
  canvas->paper_size = DT_CANVAS_PAPER_NONE;
  canvas->paper_landscape = 0;
  // The prepress palette, as every print shop's template draws it: the trim black, the
  // bleed red, the margin violet. A guide is stroked under a white keyline so a black trim
  // still reads on a charcoal plane, which is the one thing InDesign never has to solve.
  canvas->page_color = dt_canvas_color(0.0f, 0.0f, 0.0f, 1.0f);
  canvas->grid_flags |= DT_CANVAS_PAGE_VISIBLE;
  canvas->shadow.color = dt_canvas_color(0.0f, 0.0f, 0.0f, 0.5f);
  canvas->shadow.offset_x = CANVAS_DEFAULT_SHADOW_OFFSET;
  canvas->shadow.offset_y = CANVAS_DEFAULT_SHADOW_OFFSET;
  canvas->shadow.blur = 0.0f; // off until asked for
  // A padding is no prepress object at all -- it is a layout aid -- so it takes the one
  // family the convention leaves free here, the blue of the slug.
  canvas->padding_color = dt_canvas_color(0.235f, 0.471f, 0.784f, 1.0f);
  canvas->texture_contrast = 1.0f;
  canvas->texture_detail = 1.0f;
  canvas->texture_scale = 1.0f;
  canvas->texture_grain = 1.0f;
  canvas->resolution = CANVAS_DEFAULT_RESOLUTION;
  canvas->page_margin = 0.0f;
  canvas->margin_color = dt_canvas_color(0.557f, 0.267f, 0.816f, 1.0f);
  canvas->page_bleed = 0.0f;
  canvas->bleed_color = dt_canvas_color(0.882f, 0.149f, 0.110f, 1.0f);
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
  copy->serial = _next_serial();
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
  const uint64_t serial = canvas->serial;
  memcpy(canvas, snapshot, sizeof(dt_canvas_t));
  canvas->path = path;
  canvas->serial = serial;
  canvas->objects = g_ptr_array_new_with_free_func(_object_free);
  for(guint idx = 0; idx < snapshot->objects->len; idx++)
  {
    g_ptr_array_add(canvas->objects, _object_copy(g_ptr_array_index(snapshot->objects, idx)));
  }
  g_ptr_array_free(old_objects, TRUE);
  canvas->generation = generation;
  dt_canvas_touch(canvas);
}

/** Whether a render landed on, or started for, `live` since `snapshot` was taken of the same object. */
static gboolean _render_moved(const dt_canvas_object_t *snapshot, const dt_canvas_object_t *live)
{
  if(snapshot->kind != live->kind) return FALSE;
  if(live->kind == DT_CANVAS_OBJECT_IMAGE)
    return snapshot->image.jpeg != live->image.jpeg || snapshot->image.rendered_at != live->image.rendered_at
           || snapshot->image.sync_status != live->image.sync_status;
  if(live->kind == DT_CANVAS_OBJECT_MAP)
    return snapshot->map.jpeg != live->map.jpeg || snapshot->map.rendered_at != live->map.rendered_at
           || snapshot->map.sync_status != live->map.sync_status;
  return FALSE;
}

/**
 * Hand `live`'s render to `restored`, which holds the snapshot's copy of the same object. A picture's
 * whole record goes -- it holds nothing but what the render and its source said -- and its frame takes
 * the render's proportions on the snapshot's area, as dt_canvas_image_set_render() does, rather than the
 * live frame's size, which the abandoned gesture may have changed. A map keeps its place, its zoom and its
 * provider from the snapshot: only the tiles it fetched are the render's.
 */
static void _render_carry(dt_canvas_object_t *restored, const dt_canvas_object_t *live)
{
  if(live->kind == DT_CANVAS_OBJECT_IMAGE)
  {
    GBytes *previous = restored->image.jpeg;
    restored->image = live->image;
    if(!IS_NULL_PTR(restored->image.jpeg)) g_bytes_ref(restored->image.jpeg);
    if(!IS_NULL_PTR(previous)) g_bytes_unref(previous);
    const int32_t pixel_width = restored->image.pixel_width;
    const int32_t pixel_height = restored->image.pixel_height;
    if(pixel_width > 0 && pixel_height > 0)
    {
      const double area = restored->width * restored->height;
      const double ratio = (double)pixel_width / (double)pixel_height;
      restored->width = sqrt(area * ratio);
      restored->height = restored->width / ratio;
    }
    return;
  }
  GBytes *previous = restored->map.jpeg;
  restored->map.jpeg = IS_NULL_PTR(live->map.jpeg) ? NULL : g_bytes_ref(live->map.jpeg);
  if(!IS_NULL_PTR(previous)) g_bytes_unref(previous);
  restored->map.pixel_width = live->map.pixel_width;
  restored->map.pixel_height = live->map.pixel_height;
  restored->map.rendered_at = live->map.rendered_at;
  restored->map.sync_status = live->map.sync_status;
}

void dt_canvas_abandon(dt_canvas_t *canvas, const dt_canvas_t *snapshot)
{
  if(IS_NULL_PTR(canvas) || IS_NULL_PTR(snapshot)) return;
  // The live objects outlive the restore by a moment, for the renders that landed on them meanwhile.
  GPtrArray *live_objects = g_ptr_array_new_with_free_func(_object_free);
  for(guint idx = 0; idx < canvas->objects->len; idx++)
  {
    g_ptr_array_add(live_objects, _object_copy(g_ptr_array_index(canvas->objects, idx)));
  }
  dt_canvas_restore(canvas, snapshot);
  gboolean render_carried = FALSE;
  for(guint idx = 0; idx < canvas->objects->len; idx++)
  {
    dt_canvas_object_t *restored = g_ptr_array_index(canvas->objects, idx);
    for(guint live_idx = 0; live_idx < live_objects->len; live_idx++)
    {
      const dt_canvas_object_t *live = g_ptr_array_index(live_objects, live_idx);
      if(live->id != restored->id) continue;
      if(_render_moved(restored, live))
      {
        _render_carry(restored, live);
        render_carried = TRUE;
      }
      break;
    }
  }
  g_ptr_array_free(live_objects, TRUE);
  // A render is written with the document, so one that landed is a change to save; the gesture was not.
  canvas->dirty = snapshot->dirty || render_carried;
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
    else if(object->kind == DT_CANVAS_OBJECT_MAP)
    {
      snprintf(entry, sizeof(entry), DT_CANVAS_ENTRY_MAP_PATTERN, object->id);
      object->map.jpeg = dt_canvas_zip_reader_get(reader, entry);
      object->map.sync_status = IS_NULL_PTR(object->map.jpeg) ? DT_CANVAS_SYNC_STALE : DT_CANVAS_SYNC_CURRENT;
    }
    else if(object->kind == DT_CANVAS_OBJECT_SVG)
    {
      snprintf(entry, sizeof(entry), DT_CANVAS_ENTRY_SVG_PATTERN, object->id);
      object->svg.svg = dt_canvas_zip_reader_get(reader, entry);
      // Whether the file on disk still says the same thing is a question for whoever asks it
      // to reload: the document carries the drawing and opens without the file being there.
      object->svg.sync_status = IS_NULL_PTR(object->svg.svg) ? DT_CANVAS_SYNC_MISSING : DT_CANVAS_SYNC_UNKNOWN;
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
    else if(object->kind == DT_CANVAS_OBJECT_MAP && !IS_NULL_PTR(object->map.jpeg))
    {
      snprintf(entry, sizeof(entry), DT_CANVAS_ENTRY_MAP_PATTERN, object->id);
      gsize jpeg_size = 0;
      const void *jpeg_data = g_bytes_get_data(object->map.jpeg, &jpeg_size);
      ok = dt_canvas_zip_writer_add(writer, entry, jpeg_data, jpeg_size, FALSE);
    }
    else if(object->kind == DT_CANVAS_OBJECT_SVG && !IS_NULL_PTR(object->svg.svg))
    {
      // The drawing's own source, so a document carries the picture and not a reference to
      // one. Deflated: it is text, and text is what deflate is for.
      snprintf(entry, sizeof(entry), DT_CANVAS_ENTRY_SVG_PATTERN, object->id);
      gsize svg_size = 0;
      const void *svg_data = g_bytes_get_data(object->svg.svg, &svg_size);
      ok = dt_canvas_zip_writer_add(writer, entry, svg_data, svg_size, TRUE);
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
  object->text.padding = DT_CANVAS_TEXT_DEFAULT_PADDING;
  object->text.align_h = DT_CANVAS_ALIGN_START;
  object->text.align_v = DT_CANVAS_ALIGN_START;
  object->text.markdown = g_strdup(IS_NULL_PTR(markdown) ? "" : markdown);
  return object;
}

dt_canvas_object_t *dt_canvas_add_map(dt_canvas_t *canvas, double x, double y, double latitude, double longitude,
                                      int32_t zoom, uint32_t source)
{
  if(IS_NULL_PTR(canvas)) return NULL;
  dt_canvas_object_t *object = _object_new(canvas, DT_CANVAS_OBJECT_MAP, x, y, CANVAS_DEFAULT_IMAGE_LONG_EDGE_UNITS,
                                           CANVAS_DEFAULT_IMAGE_LONG_EDGE_UNITS * 0.75);
  object->map.latitude = CLAMP(latitude, -85.0, 85.0);
  object->map.longitude = CLAMP(longitude, -180.0, 180.0);
  object->map.zoom = CLAMP(zoom, 1, 19);
  object->map.source = source;
  object->map.jpeg = NULL;
  object->map.sync_status = DT_CANVAS_SYNC_UNKNOWN;
  return object;
}

void dt_canvas_map_set_render(dt_canvas_t *canvas, dt_canvas_object_t *object, GBytes *jpeg, int32_t pixel_width,
                              int32_t pixel_height, int64_t rendered_at)
{
  if(IS_NULL_PTR(canvas) || IS_NULL_PTR(object) || object->kind != DT_CANVAS_OBJECT_MAP) return;
  GBytes *previous = object->map.jpeg;
  object->map.jpeg = IS_NULL_PTR(jpeg) ? NULL : g_bytes_ref(jpeg);
  if(!IS_NULL_PTR(previous)) g_bytes_unref(previous);
  object->map.pixel_width = pixel_width;
  object->map.pixel_height = pixel_height;
  object->map.rendered_at = rendered_at;
  object->map.sync_status = IS_NULL_PTR(jpeg) ? DT_CANVAS_SYNC_MISSING : DT_CANVAS_SYNC_CURRENT;
  dt_canvas_touch(canvas);
}

GBytes *dt_canvas_object_raster(const dt_canvas_object_t *object)
{
  if(IS_NULL_PTR(object)) return NULL;
  if(object->kind == DT_CANVAS_OBJECT_IMAGE) return object->image.jpeg;
  if(object->kind == DT_CANVAS_OBJECT_MAP) return object->map.jpeg;
  // An SVG's "raster" is its own source: the decoder rasterises it, and everything downstream
  // -- the surface cache's key, the blit, the mask, the export -- works on the result exactly
  // as it does for a photograph, without knowing the difference.
  if(object->kind == DT_CANVAS_OBJECT_SVG) return object->svg.svg;
  return NULL;
}

/**
 * The drawing's own size in points, from the file. An SVG states a physical width and height,
 * or only a viewBox, or neither; a canvas unit is a point and so is the SVG user unit, so a
 * file that says how big it is arrives at that size with no scale factor in between.
 */
static gboolean _svg_intrinsic_size(GBytes *bytes, double *width, double *height, GError **error)
{
  gsize length = 0;
  const void *data = g_bytes_get_data(bytes, &length);
  RsvgHandle *handle = rsvg_handle_new_from_data(data, length, error);
  if(IS_NULL_PTR(handle)) return FALSE;
  // Points per inch: the handle turns the file's physical units (mm, in, pt) into user units
  // with this, so telling it 72 makes one user unit one point, which is one canvas unit.
  rsvg_handle_set_dpi(handle, 72.0);
  gdouble intrinsic_width = 0.0;
  gdouble intrinsic_height = 0.0;
  gboolean has_width = FALSE;
  gboolean has_height = FALSE;
  gboolean has_viewbox = FALSE;
  RsvgLength width_length;
  RsvgLength height_length;
  RsvgRectangle viewbox;
  memset(&width_length, 0, sizeof(width_length));
  memset(&height_length, 0, sizeof(height_length));
  memset(&viewbox, 0, sizeof(viewbox));
  rsvg_handle_get_intrinsic_dimensions(handle, &has_width, &width_length, &has_height, &height_length,
                                       &has_viewbox, &viewbox);
  if(!rsvg_handle_get_intrinsic_size_in_pixels(handle, &intrinsic_width, &intrinsic_height))
  {
    // No physical size of its own: the viewBox is what every browser falls back to.
    intrinsic_width = has_viewbox ? viewbox.width : 0.0;
    intrinsic_height = has_viewbox ? viewbox.height : 0.0;
  }
  g_object_unref(handle);
  if(!(intrinsic_width > 0.0) || !(intrinsic_height > 0.0))
  {
    g_set_error(error, DT_CANVAS_ERROR, DT_CANVAS_ERROR_CORRUPT, "the drawing states no size of its own");
    return FALSE;
  }
  if(!IS_NULL_PTR(width)) *width = intrinsic_width;
  if(!IS_NULL_PTR(height)) *height = intrinsic_height;
  return TRUE;
}

/** Read a file and check it parses as SVG before anything is built from it. */
static GBytes *_svg_read(const char *path, double *width, double *height, GError **error)
{
  gchar *contents = NULL;
  gsize length = 0;
  if(!g_file_get_contents(path, &contents, &length, error)) return NULL;
  GBytes *bytes = g_bytes_new_take(contents, length);
  if(!_svg_intrinsic_size(bytes, width, height, error))
  {
    g_bytes_unref(bytes);
    return NULL;
  }
  return bytes;
}

dt_canvas_object_t *dt_canvas_add_svg(dt_canvas_t *canvas, const double x, const double y, const char *path,
                                      GError **error)
{
  if(IS_NULL_PTR(canvas) || IS_NULL_PTR(path) || path[0] == '\0')
  {
    g_set_error(error, DT_CANVAS_ERROR, DT_CANVAS_ERROR_IO, "no drawing to read");
    return NULL;
  }
  double width = 0.0;
  double height = 0.0;
  GBytes *bytes = _svg_read(path, &width, &height, error);
  if(IS_NULL_PTR(bytes)) return NULL;

  dt_canvas_object_t *object = _object_new(canvas, DT_CANVAS_OBJECT_SVG, x, y, width, height);
  /*
   * No border and no shadow, whatever the canvas gives a photograph. A drawing is ink on
   * nothing -- a logo, a diagram, an arrow -- and its holes are the point of it: a card behind
   * it and a rule around it turn it into a rectangle, which is the one thing it is not. Both
   * are the user's to switch on afterwards, and the override flags are what say the canvas's
   * defaults do not apply here.
   */
  object->flags |= DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE | DT_CANVAS_OBJECT_FLAG_SHADOW_OVERRIDE;
  object->border_width = 0.0f;
  memset(&object->shadow, 0, sizeof(object->shadow));
  object->svg.svg = bytes;
  object->svg.source_width = (float)width;
  object->svg.source_height = (float)height;
  object->svg.loaded_at = (int64_t)g_get_real_time() / G_USEC_PER_SEC;
  object->svg.sync_status = DT_CANVAS_SYNC_CURRENT;
  gchar *folder = g_path_get_dirname(path);
  gchar *filename = g_path_get_basename(path);
  g_strlcpy(object->svg.folder, folder, sizeof(object->svg.folder));
  g_strlcpy(object->svg.filename, filename, sizeof(object->svg.filename));
  dt_free(folder);
  dt_free(filename);
  dt_canvas_touch(canvas);
  return object;
}

void dt_canvas_svg_path(const dt_canvas_object_t *object, char *path, const size_t length)
{
  if(IS_NULL_PTR(path) || length == 0) return;
  path[0] = '\0';
  if(IS_NULL_PTR(object) || object->kind != DT_CANVAS_OBJECT_SVG) return;
  if(object->svg.folder[0] == '\0' || object->svg.filename[0] == '\0') return;
  gchar *joined = g_build_filename(object->svg.folder, object->svg.filename, NULL);
  g_strlcpy(path, joined, length);
  dt_free(joined);
}

gboolean dt_canvas_svg_reload(dt_canvas_t *canvas, dt_canvas_object_t *object, GError **error)
{
  if(IS_NULL_PTR(canvas) || IS_NULL_PTR(object) || object->kind != DT_CANVAS_OBJECT_SVG) return FALSE;
  char path[DT_PATH_MAX];
  dt_canvas_svg_path(object, path, sizeof(path));
  if(path[0] == '\0')
  {
    g_set_error(error, DT_CANVAS_ERROR, DT_CANVAS_ERROR_IO, "the drawing remembers no file to read");
    object->svg.sync_status = DT_CANVAS_SYNC_MISSING;
    return FALSE;
  }
  double width = 0.0;
  double height = 0.0;
  GBytes *bytes = _svg_read(path, &width, &height, error);
  if(IS_NULL_PTR(bytes))
  {
    object->svg.sync_status = DT_CANVAS_SYNC_MISSING;
    return FALSE;
  }
  const gboolean changed = IS_NULL_PTR(object->svg.svg) || !g_bytes_equal(bytes, object->svg.svg);
  if(!IS_NULL_PTR(object->svg.svg)) g_bytes_unref(object->svg.svg);
  object->svg.svg = bytes;
  object->svg.loaded_at = (int64_t)g_get_real_time() / G_USEC_PER_SEC;
  object->svg.sync_status = DT_CANVAS_SYNC_CURRENT;
  /*
   * The FRAME is not resized. Where a drawing sits and how big it is on the page are the
   * user's, and a file that has been edited since is still the same drawing in the same box --
   * what changes is only its own idea of its size, which is kept so a later "fit" can use it.
   */
  object->svg.source_width = (float)width;
  object->svg.source_height = (float)height;
  if(changed) dt_canvas_touch(canvas);
  return changed;
}

dt_canvas_line_style_t dt_canvas_line_style_default(void)
{
  dt_canvas_line_style_t style;
  memset(&style, 0, sizeof(style));
  style.line_width = DT_CANVAS_CONNECTOR_LINE_WIDTH;
  style.color = dt_canvas_color(0.85f, 0.85f, 0.85f, 1.0f);
  style.dashed = FALSE;
  style.arrow_start = FALSE;
  style.arrow_end = TRUE;
  return style;
}

gboolean dt_canvas_line_style_get(const dt_canvas_object_t *object, dt_canvas_line_style_t *style)
{
  if(IS_NULL_PTR(object) || IS_NULL_PTR(style) || object->kind != DT_CANVAS_OBJECT_CONNECTOR) return FALSE;
  const uint32_t bits = object->connector.style;
  style->line_width = object->connector.line_width;
  style->color = object->connector.color;
  style->dashed = (bits & DT_CANVAS_CONNECTOR_DASHED) != 0;
  style->arrow_start = (bits & DT_CANVAS_CONNECTOR_ARROW_START) != 0;
  style->arrow_end = (bits & DT_CANVAS_CONNECTOR_ARROW_END) != 0;
  return TRUE;
}

/** A colour channel held to what a colour can be; a channel that is not a number is none of it. */
static float _unit_channel(const float value, gboolean *sound)
{
  if(!isfinite(value))
  {
    *sound = FALSE;
    return 0.0f;
  }
  if(value < 0.0f || value > 1.0f) *sound = FALSE;
  return CLAMP(value, 0.0f, 1.0f);
}

gboolean dt_canvas_line_style_sanitize(dt_canvas_line_style_t *style)
{
  if(IS_NULL_PTR(style)) return FALSE;
  gboolean sound = TRUE;
  if(!isfinite(style->line_width))
  {
    style->line_width = DT_CANVAS_CONNECTOR_LINE_WIDTH;
    sound = FALSE;
  }
  else if(style->line_width < 0.0f || style->line_width > DT_CANVAS_LINE_WIDTH_MAX)
  {
    style->line_width = CLAMP(style->line_width, 0.0f, DT_CANVAS_LINE_WIDTH_MAX);
    sound = FALSE;
  }
  style->color.red = _unit_channel(style->color.red, &sound);
  style->color.green = _unit_channel(style->color.green, &sound);
  style->color.blue = _unit_channel(style->color.blue, &sound);
  style->color.alpha = _unit_channel(style->color.alpha, &sound);
  // A switch compared against TRUE elsewhere must hold TRUE itself, not merely something that is not 0.
  const gboolean dashed = style->dashed != FALSE;
  const gboolean arrow_start = style->arrow_start != FALSE;
  const gboolean arrow_end = style->arrow_end != FALSE;
  if(dashed != style->dashed || arrow_start != style->arrow_start || arrow_end != style->arrow_end) sound = FALSE;
  style->dashed = dashed;
  style->arrow_start = arrow_start;
  style->arrow_end = arrow_end;
  return sound;
}

/** Write a style into a connector: the one spelling both a connector and a line are born through. */
static void _connector_apply_style(dt_canvas_connector_t *connector, const dt_canvas_line_style_t *style)
{
  // A style handed down from somewhere else is not trusted as it comes; a zero width is, and paints two units.
  dt_canvas_line_style_t sound = *style;
  dt_canvas_line_style_sanitize(&sound);
  uint32_t bits = DT_CANVAS_CONNECTOR_PLAIN;
  if(sound.arrow_end) bits |= DT_CANVAS_CONNECTOR_ARROW_END;
  if(sound.arrow_start) bits |= DT_CANVAS_CONNECTOR_ARROW_START;
  if(sound.dashed) bits |= DT_CANVAS_CONNECTOR_DASHED;
  connector->style = bits;
  connector->color = sound.color;
  connector->line_width = sound.line_width;
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
  const dt_canvas_line_style_t style = dt_canvas_line_style_default();
  _connector_apply_style(&object->connector, &style);
  object->connector.from_anchor = DT_CANVAS_ANCHOR_AUTO;
  object->connector.to_anchor = DT_CANVAS_ANCHOR_AUTO;
  object->connector.routing = DT_CANVAS_ROUTING_CUBIC;
  object->connector.via_count = 0;
  object->connector.from_reach = 0.0f;
  object->connector.to_reach = 0.0f;
  object->connector.via_tangent_x = 0.0;
  object->connector.via_tangent_y = 0.0;
  return object;
}

dt_canvas_object_t *dt_canvas_add_line(dt_canvas_t *canvas, const double x0, const double y0, const double x1,
                                       const double y1, const dt_canvas_routing_t routing,
                                       const dt_canvas_line_style_t *style)
{
  if(IS_NULL_PTR(canvas)) return NULL;
  const dt_canvas_line_style_t fallback = dt_canvas_line_style_default();
  const dt_canvas_line_style_t *applied = IS_NULL_PTR(style) ? &fallback : style;
  dt_canvas_object_t *object = _object_new(canvas, DT_CANVAS_OBJECT_CONNECTOR, 0.0, 0.0, 0.0, 0.0);
  // Both ids stay 0, which is what makes both ends free: no frame has that id.
  object->connector.from_id = 0;
  object->connector.to_id = 0;
  _connector_apply_style(&object->connector, applied);
  object->connector.from_anchor = DT_CANVAS_ANCHOR_AUTO;
  object->connector.to_anchor = DT_CANVAS_ANCHOR_AUTO;
  const gboolean known_routing = routing == DT_CANVAS_ROUTING_STRAIGHT || routing == DT_CANVAS_ROUTING_SQUARE
                                 || routing == DT_CANVAS_ROUTING_CUBIC;
  object->connector.routing = known_routing ? (uint32_t)routing : (uint32_t)DT_CANVAS_ROUTING_STRAIGHT;
  object->connector.via_count = 0;
  object->connector.from_x = x0;
  object->connector.from_y = y0;
  object->connector.to_x = x1;
  object->connector.to_y = y1;
  dt_canvas_route_t route;
  if(object->connector.routing == DT_CANVAS_ROUTING_CUBIC && dt_canvas_connector_route(canvas, object, &route))
    dt_canvas_connector_seed_curve(object, &route);
  return object;
}

gboolean dt_canvas_connector_has_free_end(const dt_canvas_object_t *object)
{
  if(IS_NULL_PTR(object) || object->kind != DT_CANVAS_OBJECT_CONNECTOR) return FALSE;
  return object->connector.from_id == 0 || object->connector.to_id == 0;
}

gboolean dt_canvas_connector_is_line(const dt_canvas_object_t *object)
{
  if(IS_NULL_PTR(object) || object->kind != DT_CANVAS_OBJECT_CONNECTOR) return FALSE;
  return object->connector.from_id == 0 && object->connector.to_id == 0;
}

void dt_canvas_connector_translate(dt_canvas_object_t *object, const double dx, const double dy)
{
  if(!dt_canvas_connector_has_free_end(object)) return;
  if(object->connector.from_id == 0)
  {
    object->connector.from_x += dx;
    object->connector.from_y += dy;
  }
  if(object->connector.to_id == 0)
  {
    object->connector.to_x += dx;
    object->connector.to_y += dy;
  }
  // The waypoint goes with whatever of the line moves: left behind, it would bend a moved line
  // back to where it was. A line anchored at both ends never gets here, since its frames move it.
  object->connector.via_x += dx;
  object->connector.via_y += dy;
}

/* --- shapes ------------------------------------------------------------------------- */

/** The smallest side a shape is born with: below this there is nothing left to take hold of. */
#define CANVAS_SHAPE_MIN_SIDE 4.0
/*
 * What a remembered style may hold, the same numbers the property rows allow: a style comes back
 * from wherever the atelier kept it, and a width no row could have produced is not a width.
 */
#define CANVAS_SHAPE_BORDER_MAX 500.0f
#define CANVAS_SHAPE_CORNER_MAX 5000.0f
#define CANVAS_SHAPE_SHADOW_MAX 500.0f

dt_canvas_shape_style_t dt_canvas_shape_style_default(void)
{
  dt_canvas_shape_style_t style;
  memset(&style, 0, sizeof(style));
  // Filled, in a grey that belongs to no palette: a shape nobody has styled yet must be VISIBLE,
  // and a neutral is the one fill that reads as a placeholder rather than as a choice.
  style.fill = dt_canvas_color(0.5f, 0.5f, 0.5f, 1.0f);
  style.border_override = FALSE;
  style.border_width = 0.0f;
  style.border_color = dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
  style.corner_override = FALSE;
  style.corner_radius = 0.0f;
  style.shadow_override = FALSE;
  style.sides = DT_CANVAS_SHAPE_DEFAULT_SIDES;
  // A convex polygon, not a star: the star's own depth is DT_CANVAS_SHAPE_STAR_DEPTH and is the
  // star TOOL's to ask for, so that a polygon born from this style is the polygon it was asked for.
  style.depth = 0.0f;
  style.roundness = 0.0f;
  return style;
}

gboolean dt_canvas_shape_style_get(const dt_canvas_object_t *object, dt_canvas_shape_style_t *style)
{
  if(IS_NULL_PTR(object) || IS_NULL_PTR(style) || object->kind != DT_CANVAS_OBJECT_SHAPE) return FALSE;
  memset(style, 0, sizeof(*style));
  style->fill = object->background;
  style->border_override = (object->flags & DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE) != 0;
  style->border_width = object->border_width;
  style->border_color = object->border_color;
  style->corner_override = (object->flags & DT_CANVAS_OBJECT_FLAG_CORNER_OVERRIDE) != 0;
  style->corner_radius = object->corner_radius;
  style->shadow_override = (object->flags & DT_CANVAS_OBJECT_FLAG_SHADOW_OVERRIDE) != 0;
  style->shadow = object->shadow;
  style->sides = object->shape.sides;
  style->depth = object->shape.depth;
  style->roundness = object->shape.roundness;
  return TRUE;
}

/** A length held to a range, saying whether it had to be: one that is not a number is none of it. */
static float _sound_length(const float value, const float lowest, const float highest, gboolean *sound)
{
  if(!isfinite(value))
  {
    *sound = FALSE;
    // None of it, held to the range: nothing for a length that only grows, and the neutral middle
    // for an offset that goes either way. The range's own end would be the most extreme value it
    // allows, which is the last thing a number nobody can read should become -- a shadow thrown
    // five hundred units off the shape, from a configuration key somebody mistyped.
    return CLAMP(0.0f, lowest, highest);
  }
  if(value < lowest || value > highest) *sound = FALSE;
  return CLAMP(value, lowest, highest);
}

/** A colour whose every channel is a colour's, saying whether it already was. */
static dt_canvas_color_t _sound_color(const dt_canvas_color_t color, gboolean *sound)
{
  dt_canvas_color_t held;
  held.red = _unit_channel(color.red, sound);
  held.green = _unit_channel(color.green, sound);
  held.blue = _unit_channel(color.blue, sound);
  held.alpha = _unit_channel(color.alpha, sound);
  return held;
}

gboolean dt_canvas_shape_style_sanitize(dt_canvas_shape_style_t *style)
{
  if(IS_NULL_PTR(style)) return FALSE;
  gboolean sound = TRUE;
  style->fill = _sound_color(style->fill, &sound);
  style->border_color = _sound_color(style->border_color, &sound);
  style->border_width = _sound_length(style->border_width, 0.0f, CANVAS_SHAPE_BORDER_MAX, &sound);
  style->corner_radius = _sound_length(style->corner_radius, 0.0f, CANVAS_SHAPE_CORNER_MAX, &sound);
  style->shadow.color = _sound_color(style->shadow.color, &sound);
  style->shadow.offset_x
      = _sound_length(style->shadow.offset_x, -CANVAS_SHAPE_SHADOW_MAX, CANVAS_SHAPE_SHADOW_MAX, &sound);
  style->shadow.offset_y
      = _sound_length(style->shadow.offset_y, -CANVAS_SHAPE_SHADOW_MAX, CANVAS_SHAPE_SHADOW_MAX, &sound);
  style->shadow.blur = _sound_length(style->shadow.blur, -CANVAS_SHAPE_SHADOW_MAX, CANVAS_SHAPE_SHADOW_MAX, &sound);
  if(style->sides < DT_CANVAS_SHAPE_MIN_SIDES || style->sides > DT_CANVAS_SHAPE_MAX_SIDES)
  {
    style->sides = CLAMP(style->sides, DT_CANVAS_SHAPE_MIN_SIDES, DT_CANVAS_SHAPE_MAX_SIDES);
    sound = FALSE;
  }
  style->depth = _sound_length(style->depth, 0.0f, DT_CANVAS_SHAPE_MAX_DEPTH, &sound);
  style->roundness = _sound_length(style->roundness, 0.0f, 1.0f, &sound);
  // A switch compared against TRUE elsewhere must hold TRUE itself, not merely something that is not 0.
  const gboolean border_override = style->border_override != FALSE;
  const gboolean corner_override = style->corner_override != FALSE;
  const gboolean shadow_override = style->shadow_override != FALSE;
  if(border_override != style->border_override || corner_override != style->corner_override
     || shadow_override != style->shadow_override)
    sound = FALSE;
  style->border_override = border_override;
  style->corner_override = corner_override;
  style->shadow_override = shadow_override;
  return sound;
}

dt_canvas_object_t *dt_canvas_add_shape(dt_canvas_t *canvas, const dt_canvas_shape_geometry_t geometry,
                                        const dt_canvas_rect_t *box, const dt_canvas_shape_style_t *style)
{
  if(IS_NULL_PTR(canvas) || IS_NULL_PTR(box)) return NULL;
  const dt_canvas_shape_style_t fallback = dt_canvas_shape_style_default();
  dt_canvas_shape_style_t applied = IS_NULL_PTR(style) ? fallback : *style;
  dt_canvas_shape_style_sanitize(&applied);
  const double width = fmax(box->width, CANVAS_SHAPE_MIN_SIDE);
  const double height = fmax(box->height, CANVAS_SHAPE_MIN_SIDE);
  dt_canvas_object_t *object
      = _object_new(canvas, DT_CANVAS_OBJECT_SHAPE, box->x + box->width * 0.5, box->y + box->height * 0.5, width,
                    height);
  const gboolean known_geometry = geometry >= DT_CANVAS_SHAPE_RECTANGLE && geometry < DT_CANVAS_SHAPE_LAST;
  object->shape.geometry = known_geometry ? (uint32_t)geometry : (uint32_t)DT_CANVAS_SHAPE_RECTANGLE;
  object->shape.sides = applied.sides;
  object->shape.depth = applied.depth;
  object->shape.roundness = applied.roundness;
  // A shape IS its fill and its border: both come from the style, and each override flag says
  // whether the canvas's own value still applies -- which is what makes a shape drawn with no
  // border of its own follow a canvas whose border is changed afterwards.
  object->background = applied.fill;
  if(applied.border_override)
  {
    object->flags |= DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE;
    object->border_width = applied.border_width;
    object->border_color = applied.border_color;
  }
  if(applied.corner_override)
  {
    object->flags |= DT_CANVAS_OBJECT_FLAG_CORNER_OVERRIDE;
    object->corner_radius = applied.corner_radius;
  }
  if(applied.shadow_override)
  {
    object->flags |= DT_CANVAS_OBJECT_FLAG_SHADOW_OVERRIDE;
    object->shadow = applied.shadow;
  }
  return object;
}

/** One point into the caller's buffer, if there is room for it. */
static void _outline_point(double *xy, const size_t max, size_t *count, const double x, const double y)
{
  if(*count >= max) return;
  xy[2 * *count] = x;
  xy[2 * *count + 1] = y;
  (*count)++;
}

size_t dt_canvas_shape_outline(const dt_canvas_t *canvas, const dt_canvas_object_t *object, double *xy,
                               const size_t max)
{
  if(IS_NULL_PTR(object) || IS_NULL_PTR(xy) || max < 4) return 0;
  if(object->kind != DT_CANVAS_OBJECT_SHAPE) return 0;
  const double half_width = object->width * 0.5;
  const double half_height = object->height * 0.5;
  const double radius
      = CLAMP(dt_canvas_object_effective_corner_radius(canvas, object), 0.0, fmin(half_width, half_height));
  size_t count = 0;
  if(!(radius > 0.0))
  {
    _outline_point(xy, max, &count, -half_width, -half_height);
    _outline_point(xy, max, &count, half_width, -half_height);
    _outline_point(xy, max, &count, half_width, half_height);
    _outline_point(xy, max, &count, -half_width, half_height);
    return count;
  }
  // The same four arcs _frame_path() draws, in the same order, sampled finely enough that no
  // chord strays more than a fraction of a unit from the arc it stands in for.
  static const int steps = 12;
  const double corners[4][2] = { { half_width - radius, -half_height + radius },
                                 { half_width - radius, half_height - radius },
                                 { -half_width + radius, half_height - radius },
                                 { -half_width + radius, -half_height + radius } };
  for(int corner = 0; corner < 4; corner++)
  {
    const double start = -M_PI / 2.0 + corner * M_PI / 2.0;
    for(int step = 0; step <= steps; step++)
    {
      const double angle = start + (M_PI / 2.0) * (double)step / (double)steps;
      _outline_point(xy, max, &count, corners[corner][0] + radius * cos(angle),
                     corners[corner][1] + radius * sin(angle));
    }
  }
  return count;
}

gboolean dt_canvas_shape_needs_coverage(const dt_canvas_object_t *object)
{
  if(IS_NULL_PTR(object) || object->kind != DT_CANVAS_OBJECT_SHAPE) return FALSE;
  // A cut shape is asked for its cutout, like every other cut frame; that path knows nothing of
  // the outline and must not be diverted here.
  if(object->mask.shape != DT_CANVAS_MASK_NONE) return FALSE;
  return object->shape.geometry != DT_CANVAS_SHAPE_RECTANGLE || !(object->background.alpha > 0.0f);
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
  // 0 names no object -- it is what a free end holds -- and the cascade below would read it as
  // every free end in the document.
  if(IS_NULL_PTR(canvas) || id == 0) return FALSE;
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
  // A line owns everything it is made of; a connector with an anchored end belongs to its frames,
  // and a copy of it would lie exactly over the original.
  const gboolean is_line = dt_canvas_connector_is_line(source);
  if(!dt_canvas_object_is_frame(source) && !is_line) return NULL;
  dt_canvas_object_t *copy = _object_copy(source);
  copy->id = canvas->next_id++;
  if(is_line)
  {
    dt_canvas_connector_translate(copy, CANVAS_DUPLICATE_OFFSET, CANVAS_DUPLICATE_OFFSET);
  }
  else
  {
    copy->x += CANVAS_DUPLICATE_OFFSET;
    copy->y += CANVAS_DUPLICATE_OFFSET;
  }
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
                                int32_t pixel_height, uint64_t history_hash, int64_t rendered_at, uint32_t colorspace)
{
  if(IS_NULL_PTR(canvas) || IS_NULL_PTR(object) || object->kind != DT_CANVAS_OBJECT_IMAGE) return;
  object->image.colorspace = colorspace;
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

void dt_canvas_object_effective_shadow(const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                                       dt_canvas_shadow_t *shadow)
{
  dt_canvas_shadow_t effective;
  memset(&effective, 0, sizeof(effective));
  if(!IS_NULL_PTR(canvas)) effective = canvas->shadow;
  if(!IS_NULL_PTR(object) && (object->flags & DT_CANVAS_OBJECT_FLAG_SHADOW_OVERRIDE)) effective = object->shadow;
  if(!IS_NULL_PTR(shadow)) *shadow = effective;
}

void dt_canvas_texture_get(const dt_canvas_t *canvas, float *contrast, float *detail, float *scale, float *grain)
{
  const float values[4] = { IS_NULL_PTR(canvas) ? 1.0f : canvas->texture_contrast,
                            IS_NULL_PTR(canvas) ? 1.0f : canvas->texture_detail,
                            IS_NULL_PTR(canvas) ? 1.0f : canvas->texture_scale,
                            IS_NULL_PTR(canvas) ? 1.0f : canvas->texture_grain };
  float *targets[4] = { contrast, detail, scale, grain };
  // A file from before these fields holds FOUR zeros, and that is the only thing a zero may
  // stand for: all four unset is an old file and reads as the paper as designed. Read the
  // rule per field instead -- as it was -- and a weight a user deliberately turned down to
  // nothing comes back as 1, which is how the grain slider came to do nothing at its own
  // zero, and the detail slider with it. A canvas whose four weights are all zero is a plain
  // colour by another name, so nothing is lost by spending that one combination here.
  const gboolean unset = values[0] <= 0.0f && values[1] <= 0.0f && values[2] <= 0.0f && values[3] <= 0.0f;
  // The scale divides every knee, so it alone may never reach zero.
  const float floors[4] = { 0.0f, 0.0f, 0.05f, 0.0f };
  for(int idx = 0; idx < 4; idx++)
  {
    if(IS_NULL_PTR(targets[idx])) continue;
    *targets[idx] = unset ? 1.0f : fmaxf(values[idx], floors[idx]);
  }
}

/** Every background, in the order the list shows them; the code is what is stored. */
static const struct
{
  uint32_t code;
  const char *name;
} _backgrounds[] = {
  { DT_CANVAS_BACKGROUND_TRANSPARENT, N_("Transparent") },
  { DT_CANVAS_BACKGROUND_PLAIN, N_("Plain colour") },
  { DT_CANVAS_BACKGROUND_MOLESKINE, N_("Moleskine paper") },
  { DT_CANVAS_BACKGROUND_WATERCOLOUR, N_("Watercolour paper") },
  { DT_CANVAS_BACKGROUND_LAID, N_("Laid paper") },
  { DT_CANVAS_BACKGROUND_EMBOSSED, N_("Embossed paper") },
  { DT_CANVAS_BACKGROUND_JAPANESE, N_("Japanese paper") },
  { DT_CANVAS_BACKGROUND_PSYCHEDELIC, N_("Psychedelic washi") },
  { DT_CANVAS_BACKGROUND_KRAFT, N_("Kraft paper") },
  { DT_CANVAS_BACKGROUND_CHARCOAL, N_("Charcoal card") },
};

int dt_canvas_background_count(void)
{
  return (int)(sizeof(_backgrounds) / sizeof(_backgrounds[0]));
}

const char *dt_canvas_background_name(const int position)
{
  if(position < 0 || position >= dt_canvas_background_count()) return NULL;
  return _(_backgrounds[position].name);
}

uint32_t dt_canvas_background_code(const int position)
{
  if(position < 0 || position >= dt_canvas_background_count()) return DT_CANVAS_BACKGROUND_PLAIN;
  return _backgrounds[position].code;
}

int dt_canvas_background_position(const uint32_t style)
{
  int plain = 0;
  for(int position = 0; position < dt_canvas_background_count(); position++)
  {
    if(_backgrounds[position].code == style) return position;
    if(_backgrounds[position].code == DT_CANVAS_BACKGROUND_PLAIN) plain = position;
  }
  // A value this version does not know reads as the plain colour, never as a hole.
  return plain;
}

gboolean dt_canvas_background_is_transparent(const uint32_t style)
{
  return style == DT_CANVAS_BACKGROUND_TRANSPARENT;
}

dt_canvas_color_t dt_canvas_background_tint(const uint32_t style)
{
  switch(style)
  {
    case DT_CANVAS_BACKGROUND_MOLESKINE:
      return dt_canvas_color(0.961f, 0.941f, 0.886f, 1.0f); // a pale cream
    case DT_CANVAS_BACKGROUND_WATERCOLOUR:
      return dt_canvas_color(1.0f, 1.0f, 1.0f, 1.0f);
    case DT_CANVAS_BACKGROUND_EMBOSSED:
      return dt_canvas_color(0.965f, 0.962f, 0.950f, 1.0f);
    case DT_CANVAS_BACKGROUND_JAPANESE:
      return dt_canvas_color(0.972f, 0.962f, 0.935f, 1.0f);
    case DT_CANVAS_BACKGROUND_PSYCHEDELIC:
      // Washi's own colour: it is the same sheet, and only its wrinkles are dyed.
      return dt_canvas_color(0.972f, 0.962f, 0.935f, 1.0f);
    case DT_CANVAS_BACKGROUND_LAID:
      // Antique laid is a warm cream: rag pulp, aged.
      return dt_canvas_color(0.945f, 0.930f, 0.895f, 1.0f);
    case DT_CANVAS_BACKGROUND_KRAFT:
      // Unbleached softwood pulp, the colour its own lignin leaves it.
      return dt_canvas_color(0.640f, 0.490f, 0.340f, 1.0f);
    case DT_CANVAS_BACKGROUND_CHARCOAL:
      // Not black: the tooth has to be able to catch a light, and it can only lift what is
      // there. A sheet at zero would stay at zero however deep its relief.
      return dt_canvas_color(0.130f, 0.128f, 0.138f, 1.0f);
    default:
      return dt_canvas_color(0.18f, 0.18f, 0.18f, 1.0f);
  }
}

gboolean dt_canvas_shadow_visible(const dt_canvas_shadow_t *shadow)
{
  return !IS_NULL_PTR(shadow) && shadow->color.alpha > 0.0f && shadow->blur != 0.0f;
}

double dt_canvas_object_effective_corner_radius(const dt_canvas_t *canvas, const dt_canvas_object_t *object)
{
  if(IS_NULL_PTR(object)) return 0.0;
  double radius = IS_NULL_PTR(canvas) ? 0.0 : canvas->corner_radius;
  if(object->flags & DT_CANVAS_OBJECT_FLAG_CORNER_OVERRIDE) radius = object->corner_radius;
  const double limit = fmin(object->width, object->height) * 0.5;
  return CLAMP(radius, 0.0, fmax(limit, 0.0));
}

dt_canvas_color_t dt_canvas_object_background(const dt_canvas_object_t *object)
{
  if(IS_NULL_PTR(object)) return dt_canvas_color(0.0f, 0.0f, 0.0f, 0.0f);
  if(object->kind == DT_CANVAS_OBJECT_TEXT) return object->text.background;
  return object->background;
}

/* --- cutout masks ------------------------------------------------------------------- */

void dt_canvas_mask_clear(dt_canvas_object_t *object)
{
  if(IS_NULL_PTR(object)) return;
  dt_free(object->mask.nodes);
  object->mask.nodes = NULL;
  object->mask.node_count = 0;
}

void dt_canvas_mask_set_nodes(dt_canvas_t *canvas, dt_canvas_object_t *object, const float *nodes, uint32_t count)
{
  if(IS_NULL_PTR(object)) return;
  dt_canvas_mask_clear(object);
  if(count > CANVAS_MASK_MAX_NODES) count = CANVAS_MASK_MAX_NODES;
  if(count > 0 && !IS_NULL_PTR(nodes))
  {
    const size_t floats = (size_t)count * DT_CANVAS_MASK_NODE_FLOATS;
    object->mask.nodes = g_new(float, floats);
    memcpy(object->mask.nodes, nodes, floats * sizeof(float));
    object->mask.node_count = count;
  }
  dt_canvas_touch(canvas);
}

/** A corner node at a unit-square point: control points on the node, not smoothed. */
static void _mask_node_init(float *node, const float x, const float y)
{
  for(int idx = 0; idx < DT_CANVAS_MASK_NODE_FLOATS; idx++) node[idx] = 0.0f;
  node[DT_CANVAS_MASK_NODE_X] = x;
  node[DT_CANVAS_MASK_NODE_Y] = y;
  node[DT_CANVAS_MASK_NODE_CTRL1_X] = x;
  node[DT_CANVAS_MASK_NODE_CTRL1_Y] = y;
  node[DT_CANVAS_MASK_NODE_CTRL2_X] = x;
  node[DT_CANVAS_MASK_NODE_CTRL2_Y] = y;
  // The borders stay 0: the node takes the shape's fall-off until the user gives it its own.
}

void dt_canvas_mask_set_shape(dt_canvas_t *canvas, dt_canvas_object_t *object, uint32_t shape)
{
  if(IS_NULL_PTR(object)) return;
  if(shape > DT_CANVAS_MASK_GRADIENT) shape = DT_CANVAS_MASK_NONE;
  if(object->mask.shape == shape) return;
  dt_canvas_mask_clear(object);
  object->mask.shape = shape;
  object->mask.center_x = 0.5f;
  object->mask.center_y = 0.5f;
  object->mask.rotation = 0.0f;
  object->mask.spare = 0.0f;
  if(object->mask.feather <= 0.0f) object->mask.feather = 0.05f;
  // Radii are fractions of the shorter side, so the default shape fits every aspect ratio.
  const double longer = fmax(object->width, object->height);
  const double shorter = fmax(fmin(object->width, object->height), 1.0);
  const float wide = (float)(longer / shorter);
  switch(shape)
  {
    case DT_CANVAS_MASK_CIRCLE:
      object->mask.radius_x = 0.45f;
      object->mask.radius_y = 0.45f;
      break;
    case DT_CANVAS_MASK_ELLIPSE:
      object->mask.radius_x = object->width >= object->height ? 0.45f * wide : 0.45f;
      object->mask.radius_y = object->width >= object->height ? 0.45f : 0.45f * wide;
      break;
    case DT_CANVAS_MASK_GRADIENT:
      object->mask.radius_x = 0.25f; // the extent
      object->mask.radius_y = 0.0f;  // the curvature
      object->mask.rotation = 0.0f;
      object->mask.center_y = 0.6f;
      break;
    case DT_CANVAS_MASK_POLYGON:
    {
      // A hexagon, inset a little, corners at the frame's edges.
      float nodes[6 * DT_CANVAS_MASK_NODE_FLOATS];
      const float inset = 0.06f;
      _mask_node_init(nodes + 0 * DT_CANVAS_MASK_NODE_FLOATS, 0.25f, inset);
      _mask_node_init(nodes + 1 * DT_CANVAS_MASK_NODE_FLOATS, 0.75f, inset);
      _mask_node_init(nodes + 2 * DT_CANVAS_MASK_NODE_FLOATS, 1.0f - inset, 0.5f);
      _mask_node_init(nodes + 3 * DT_CANVAS_MASK_NODE_FLOATS, 0.75f, 1.0f - inset);
      _mask_node_init(nodes + 4 * DT_CANVAS_MASK_NODE_FLOATS, 0.25f, 1.0f - inset);
      _mask_node_init(nodes + 5 * DT_CANVAS_MASK_NODE_FLOATS, inset, 0.5f);
      dt_canvas_mask_set_nodes(canvas, object, nodes, 6);
      break;
    }
    default:
      object->mask.shape = DT_CANVAS_MASK_NONE;
      object->mask.flags = 0;
      break;
  }
  dt_canvas_touch(canvas);
}

gboolean dt_canvas_mask_insert_node(dt_canvas_t *canvas, dt_canvas_object_t *object, uint32_t index, float x, float y)
{
  if(IS_NULL_PTR(object) || object->mask.shape != DT_CANVAS_MASK_POLYGON) return FALSE;
  if(object->mask.node_count >= CANVAS_MASK_MAX_NODES) return FALSE;
  if(index > object->mask.node_count) index = object->mask.node_count;
  const uint32_t count = object->mask.node_count + 1;
  float *nodes = g_new(float, (size_t)count * DT_CANVAS_MASK_NODE_FLOATS);
  const size_t node_bytes = DT_CANVAS_MASK_NODE_FLOATS * sizeof(float);
  if(index > 0) memcpy(nodes, object->mask.nodes, index * node_bytes);
  _mask_node_init(nodes + (size_t)index * DT_CANVAS_MASK_NODE_FLOATS, x, y);
  if(index < object->mask.node_count)
    memcpy(nodes + (size_t)(index + 1) * DT_CANVAS_MASK_NODE_FLOATS,
           object->mask.nodes + (size_t)index * DT_CANVAS_MASK_NODE_FLOATS,
           (object->mask.node_count - index) * node_bytes);
  dt_free(object->mask.nodes);
  object->mask.nodes = nodes;
  object->mask.node_count = count;
  dt_canvas_touch(canvas);
  return TRUE;
}

gboolean dt_canvas_mask_remove_node(dt_canvas_t *canvas, dt_canvas_object_t *object, uint32_t index)
{
  if(IS_NULL_PTR(object) || object->mask.shape != DT_CANVAS_MASK_POLYGON) return FALSE;
  if(object->mask.node_count <= 3 || index >= object->mask.node_count) return FALSE;
  const size_t node_bytes = DT_CANVAS_MASK_NODE_FLOATS * sizeof(float);
  memmove(object->mask.nodes + (size_t)index * DT_CANVAS_MASK_NODE_FLOATS,
          object->mask.nodes + (size_t)(index + 1) * DT_CANVAS_MASK_NODE_FLOATS,
          (object->mask.node_count - index - 1) * node_bytes);
  object->mask.node_count--;
  dt_canvas_touch(canvas);
  return TRUE;
}

uint64_t dt_canvas_mask_hash(const dt_canvas_mask_t *mask)
{
  if(IS_NULL_PTR(mask) || mask->shape == DT_CANVAS_MASK_NONE) return 0;
  // FNV-1a over the fixed fields then the nodes: cheap, and any bit moved changes the raster.
  uint64_t hash = 1469598103934665603ULL;
  const uint8_t *bytes = (const uint8_t *)mask;
  const size_t fixed = offsetof(dt_canvas_mask_t, node_count);
  for(size_t idx = 0; idx < fixed; idx++)
  {
    hash ^= bytes[idx];
    hash *= 1099511628211ULL;
  }
  const size_t node_bytes = (size_t)mask->node_count * DT_CANVAS_MASK_NODE_FLOATS * sizeof(float);
  const uint8_t *node_data = (const uint8_t *)mask->nodes;
  for(size_t idx = 0; idx < node_bytes && !IS_NULL_PTR(node_data); idx++)
  {
    hash ^= node_data[idx];
    hash *= 1099511628211ULL;
  }
  return hash == 0 ? 1 : hash;
}

gboolean dt_canvas_object_is_frame(const dt_canvas_object_t *object)
{
  if(IS_NULL_PTR(object)) return FALSE;
  return object->kind == DT_CANVAS_OBJECT_IMAGE || object->kind == DT_CANVAS_OBJECT_TEXT
         || object->kind == DT_CANVAS_OBJECT_MAP || object->kind == DT_CANVAS_OBJECT_SVG
         || object->kind == DT_CANVAS_OBJECT_SHAPE;
}

gboolean dt_canvas_object_keeps_ratio(const dt_canvas_object_t *object)
{
  if(IS_NULL_PTR(object)) return FALSE;
  if(object->flags & DT_CANVAS_OBJECT_FLAG_FREE_RATIO) return FALSE;
  return object->kind == DT_CANVAS_OBJECT_IMAGE || object->kind == DT_CANVAS_OBJECT_SVG;
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

double dt_canvas_segment_distance(const double px, const double py, const double ax, const double ay, const double bx,
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

/** How far a point is from the nearest segment of a closed polyline; the closing segment counts. */
static double _outline_distance(const double *xy, const size_t points, const double x, const double y)
{
  double nearest = INFINITY;
  for(size_t idx = 0; idx < points; idx++)
  {
    const size_t next = (idx + 1) % points;
    nearest = fmin(nearest, dt_canvas_segment_distance(x, y, xy[2 * idx], xy[2 * idx + 1], xy[2 * next],
                                                       xy[2 * next + 1]));
  }
  return nearest;
}

/** Whether a closed polyline encloses a point: the crossing count of a ray, which is the nonzero
 * winding rule for the simple outlines a shape draws. */
static gboolean _outline_encloses(const double *xy, const size_t points, const double x, const double y)
{
  if(points < 3) return FALSE;
  gboolean inside = FALSE;
  for(size_t idx = 0, previous = points - 1; idx < points; previous = idx++)
  {
    const double this_x = xy[2 * idx];
    const double this_y = xy[2 * idx + 1];
    const double last_x = xy[2 * previous];
    const double last_y = xy[2 * previous + 1];
    if((this_y > y) == (last_y > y)) continue;
    if(x < (last_x - this_x) * (y - this_y) / (last_y - this_y) + this_x) inside = !inside;
  }
  return inside;
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
      if(dt_canvas_segment_distance(x, y, route.points[2 * idx], route.points[2 * idx + 1], route.points[2 * idx + 2],
                                    route.points[2 * idx + 3])
         <= reach)
        return TRUE;
    }
    return FALSE;
  }
  double local_x = 0.0;
  double local_y = 0.0;
  dt_canvas_object_to_local(object, x, y, &local_x, &local_y);
  if(object->kind == DT_CANVAS_OBJECT_SHAPE && object->mask.shape == DT_CANVAS_MASK_NONE)
  {
    double outline[2 * DT_CANVAS_SHAPE_OUTLINE_MAX];
    const size_t points = dt_canvas_shape_outline(canvas, object, outline, DT_CANVAS_SHAPE_OUTLINE_MAX);
    if(points >= 3)
    {
      const double to_edge = _outline_distance(outline, points, local_x, local_y);
      if(to_edge <= tolerance) return TRUE;
      if(!_outline_encloses(outline, points, local_x, local_y)) return FALSE;
      // A shape with no fill is picked by the BAND it actually paints, not by the box it stands
      // in: an outline box drawn over a photograph would otherwise take every click meant for
      // the picture inside it, and there is nothing of the shape there to click on.
      if(object->background.alpha > 0.0f) return TRUE;
      dt_canvas_color_t border_color;
      float border_width = 0.0f;
      dt_canvas_object_effective_border(canvas, object, &border_color, &border_width);
      // The band is what the border PAINTS, and a border painted in nothing paints none of it:
      // the painter and the coverage the text reads both ask its strength, so a hit test that did
      // not would hand the picture's clicks to a shape with nothing of itself on screen. Its
      // outline still answers above, so such a shape is never left with no way to take hold of it.
      return border_color.alpha > 0.0f && to_edge <= (double)border_width + tolerance;
    }
  }
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
  // A corner's normal is the diagonal, so a route leaves it at 45 degrees rather than along an
  // edge it would then run beside.
  static const double diagonal = 0.70710678118654752;
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
    case DT_CANVAS_ANCHOR_NORTH_EAST:
      *local_x = width * 0.5;
      *local_y = -height * 0.5;
      *normal_x = diagonal;
      *normal_y = -diagonal;
      break;
    case DT_CANVAS_ANCHOR_SOUTH_EAST:
      *local_x = width * 0.5;
      *local_y = height * 0.5;
      *normal_x = diagonal;
      *normal_y = diagonal;
      break;
    case DT_CANVAS_ANCHOR_SOUTH_WEST:
      *local_x = -width * 0.5;
      *local_y = height * 0.5;
      *normal_x = -diagonal;
      *normal_y = diagonal;
      break;
    case DT_CANVAS_ANCHOR_NORTH_WEST:
      *local_x = -width * 0.5;
      *local_y = -height * 0.5;
      *normal_x = -diagonal;
      *normal_y = -diagonal;
      break;
    case DT_CANVAS_ANCHOR_CENTRE:
      // Resolved against the other end by the caller; the centre is only where its handle is.
      break;
    case DT_CANVAS_ANCHOR_NORTH:
    default:
      *local_y = -height * 0.5;
      *normal_y = -1.0;
      break;
  }
}

/* --- what an object actually draws ---------------------------------------------- */

static double _cross(const double a_x, const double a_y, const double b_x, const double b_y)
{
  return a_x * b_y - a_y * b_x;
}

/** Where a ray from the centre leaves a rounded rectangle of half extents `half`, radius `corner`. */
static double _rounded_rect_reach(const double half_x, const double half_y, const double corner, const double dir_x,
                                  const double dir_y)
{
  const double to_side = fabs(dir_x) > 1e-9 ? half_x / fabs(dir_x) : INFINITY;
  const double to_edge = fabs(dir_y) > 1e-9 ? half_y / fabs(dir_y) : INFINITY;
  const double straight = fmin(to_side, to_edge);
  if(!(corner > 0.0)) return straight;
  const double at_x = dir_x * straight;
  const double at_y = dir_y * straight;
  // Only a ray leaving through a corner's square meets the arc; the rest leaves a flat side.
  if(fabs(at_x) <= half_x - corner || fabs(at_y) <= half_y - corner) return straight;
  const double centre_x = (at_x < 0.0 ? -1.0 : 1.0) * (half_x - corner);
  const double centre_y = (at_y < 0.0 ? -1.0 : 1.0) * (half_y - corner);
  const double along = dir_x * centre_x + dir_y * centre_y;
  const double discriminant = along * along - (centre_x * centre_x + centre_y * centre_y - corner * corner);
  if(discriminant < 0.0) return straight;
  return fmax(along + sqrt(discriminant), 0.0);
}

/** Where a ray from the centre leaves a disc of radius `radius` centred at `centre`. */
static double _disc_reach(const double centre_x, const double centre_y, const double radius, const double dir_x,
                          const double dir_y)
{
  const double along = dir_x * centre_x + dir_y * centre_y;
  const double discriminant = along * along - (centre_x * centre_x + centre_y * centre_y - radius * radius);
  if(discriminant < 0.0) return -1.0;
  return along + sqrt(discriminant);
}

gboolean dt_canvas_object_covers(const dt_canvas_t *canvas, const dt_canvas_object_t *frame, const double x,
                                 const double y, const double standoff)
{
  if(IS_NULL_PTR(frame) || !dt_canvas_object_is_frame(frame)) return FALSE;
  if(frame->flags & DT_CANVAS_OBJECT_FLAG_HIDDEN) return FALSE;
  double local_x = 0.0;
  double local_y = 0.0;
  dt_canvas_object_to_local(frame, x, y, &local_x, &local_y);
  const double distance = hypot(local_x, local_y);
  // The centre is inside whatever the frame draws, and has no direction to ask about.
  if(!(distance > 0.0)) return TRUE;
  const double reach = dt_canvas_object_silhouette_reach(canvas, frame, local_x, local_y);
  return distance <= reach + fmax(standoff, 0.0);
}

double dt_canvas_object_silhouette_reach(const dt_canvas_t *canvas, const dt_canvas_object_t *frame,
                                         const double dir_x, const double dir_y)
{
  if(IS_NULL_PTR(frame)) return 0.0;
  const double length = hypot(dir_x, dir_y);
  if(!(length > 0.0)) return 0.0;
  const double unit_x = dir_x / length;
  const double unit_y = dir_y / length;
  const double half_x = frame->width * 0.5;
  const double half_y = frame->height * 0.5;
  const double corner = dt_canvas_object_effective_corner_radius(canvas, frame);
  const double outer = _rounded_rect_reach(half_x, half_y, corner, unit_x, unit_y);

  const dt_canvas_mask_t *mask = &frame->mask;
  const gboolean cut = dt_canvas_object_is_frame(frame) && mask->shape != DT_CANVAS_MASK_NONE;
  // An inverted cutout keeps what is outside the shape, so the frame's own edge is the
  // silhouette; a gradient covers the frame and comes to the same thing.
  if(!cut || (mask->flags & DT_CANVAS_MASK_INVERT) || mask->shape == DT_CANVAS_MASK_GRADIENT) return outer;

  const double side = fmax(fmin(frame->width, frame->height), 1.0);
  const double centre_x = (mask->center_x - 0.5) * frame->width;
  const double centre_y = (mask->center_y - 0.5) * frame->height;
  // The fall-off and the border band both reach past the shape's own edge, and the band is
  // dilated outward from it, so what shows ends that much further out.
  dt_canvas_color_t border_color;
  float border_width = 0.0f;
  dt_canvas_object_effective_border(canvas, frame, &border_color, &border_width);
  const double grown = fmax(mask->feather, 0.0f) * side
                       + (border_color.alpha > 0.0f ? fmax(border_width, 0.0f) : 0.0);
  double reach = -1.0;
  switch(mask->shape)
  {
    case DT_CANVAS_MASK_CIRCLE:
      reach = _disc_reach(centre_x, centre_y, fmax(mask->radius_x, 0.0f) * side + grown, unit_x, unit_y);
      break;
    case DT_CANVAS_MASK_ELLIPSE:
    {
      // In the ellipse's own axes the shape is the unit circle, so the ray is solved there.
      const double angle = mask->rotation * M_PI / 180.0;
      const double cos_r = cos(angle);
      const double sin_r = sin(angle);
      const double radius_x = fmax(fmax(mask->radius_x, 0.0f) * side + grown, 1e-6);
      const double radius_y = fmax(fmax(mask->radius_y, 0.0f) * side + grown, 1e-6);
      const double along_x = (unit_x * cos_r + unit_y * sin_r) / radius_x;
      const double along_y = (-unit_x * sin_r + unit_y * cos_r) / radius_y;
      const double offset_x = (centre_x * cos_r + centre_y * sin_r) / radius_x;
      const double offset_y = (-centre_x * sin_r + centre_y * cos_r) / radius_y;
      const double square = along_x * along_x + along_y * along_y;
      if(square > 1e-12)
      {
        const double along = along_x * offset_x + along_y * offset_y;
        const double discriminant = along * along - square * (offset_x * offset_x + offset_y * offset_y - 1.0);
        if(discriminant >= 0.0) reach = (along + sqrt(discriminant)) / square;
      }
      break;
    }
    case DT_CANVAS_MASK_POLYGON:
    {
      // The straight polygon through the nodes, which a curve through them leaves by at most
      // a fraction of a segment: near enough for where a line should stop.
      if(mask->node_count < 3 || IS_NULL_PTR(mask->nodes)) break;
      for(uint32_t idx = 0; idx < mask->node_count; idx++)
      {
        const float *from = mask->nodes + (size_t)idx * DT_CANVAS_MASK_NODE_FLOATS;
        const float *to = mask->nodes + (size_t)((idx + 1) % mask->node_count) * DT_CANVAS_MASK_NODE_FLOATS;
        const double from_x = (from[DT_CANVAS_MASK_NODE_X] - 0.5) * frame->width;
        const double from_y = (from[DT_CANVAS_MASK_NODE_Y] - 0.5) * frame->height;
        const double edge_x = (to[DT_CANVAS_MASK_NODE_X] - 0.5) * frame->width - from_x;
        const double edge_y = (to[DT_CANVAS_MASK_NODE_Y] - 0.5) * frame->height - from_y;
        const double denominator = _cross(unit_x, unit_y, edge_x, edge_y);
        if(fabs(denominator) < 1e-12) continue;
        const double along = _cross(from_x, from_y, edge_x, edge_y) / denominator;
        const double across = _cross(from_x, from_y, unit_x, unit_y) / denominator;
        if(along <= 0.0 || across < 0.0 || across > 1.0) continue;
        // The farthest crossing: a ray out of a dented shape leaves by its outermost edge.
        if(along > reach) reach = along;
      }
      if(reach > 0.0) reach += grown;
      break;
    }
    default:
      break;
  }
  // Never past what the frame itself draws: a cut shape is confined to it.
  if(!(reach > 0.0)) return outer;
  return fmin(reach, outer);
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

void dt_canvas_object_anchor_point(const dt_canvas_t *canvas, const dt_canvas_object_t *frame,
                                   dt_canvas_anchor_t anchor, double target_x, double target_y, double *x, double *y,
                                   double *normal_x, double *normal_y)
{
  if(IS_NULL_PTR(frame)) return;
  if(anchor == DT_CANVAS_ANCHOR_CENTRE)
  {
    // The centre anchor aims at the centre and touches the edge on the way: the point slides
    // around the frame as the other end moves, which is the anchor to reach for when which
    // side the route leaves by is the layout's business rather than the user's.
    double local_x = 0.0;
    double local_y = 0.0;
    dt_canvas_object_to_local(frame, target_x, target_y, &local_x, &local_y);
    double length = hypot(local_x, local_y);
    if(!(length > 0.0))
    {
      local_x = 1.0;
      local_y = 0.0;
      length = 1.0;
    }
    local_x /= length;
    local_y /= length;
    // Where that direction leaves what the object DRAWS, which is its rounded rectangle or,
    // where a cutout replaces it, the cut shape with its fall-off and its border: a line meets
    // the picture rather than the empty corner of a bounding box.
    const double reach = dt_canvas_object_silhouette_reach(canvas, frame, local_x, local_y);
    const double cos_r = cos(frame->rotation);
    const double sin_r = sin(frame->rotation);
    const double out_x = local_x * reach;
    const double out_y = local_y * reach;
    *x = frame->x + out_x * cos_r - out_y * sin_r;
    *y = frame->y + out_x * sin_r + out_y * cos_r;
    *normal_x = local_x * cos_r - local_y * sin_r;
    *normal_y = local_x * sin_r + local_y * cos_r;
    return;
  }
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

void dt_canvas_object_anchor_handle(const dt_canvas_t *canvas, const dt_canvas_object_t *frame,
                                    const dt_canvas_anchor_t anchor, double *x, double *y)
{
  if(IS_NULL_PTR(frame)) return;
  if(anchor == DT_CANVAS_ANCHOR_CENTRE)
  {
    *x = frame->x;
    *y = frame->y;
    return;
  }
  double normal_x = 0.0;
  double normal_y = 0.0;
  dt_canvas_object_anchor_point(canvas, frame, anchor, 0.0, 0.0, x, y, &normal_x, &normal_y);
}

static void _route_add_point(dt_canvas_route_t *route, const double x, const double y)
{
  if(route->point_count >= DT_CANVAS_ROUTE_MAX_POINTS) return;
  route->points[2 * route->point_count] = x;
  route->points[2 * route->point_count + 1] = y;
  route->point_count++;
}

/**
 * Square routing: a stub along each normal, then legs that are horizontal or vertical. The stubs
 * are the caller's, since only it knows which ends are free.
 */
static void _route_square(dt_canvas_route_t *route, const double from_stub, const double to_stub)
{
  const double start_x = route->from_x + route->from_normal_x * from_stub;
  const double start_y = route->from_y + route->from_normal_y * from_stub;
  const double end_x = route->to_x + route->to_normal_x * to_stub;
  const double end_y = route->to_y + route->to_normal_y * to_stub;
  const gboolean from_horizontal = fabs(route->from_normal_x) >= fabs(route->from_normal_y);
  const gboolean to_horizontal = fabs(route->to_normal_x) >= fabs(route->to_normal_y);
  _route_add_point(route, route->from_x, route->from_y);
  _route_add_point(route, start_x, start_y);
  if(route->segment_count == 2)
  {
    // Through the waypoint: one elbow to reach it along the start's axis, one to leave it along the end's.
    if(from_horizontal)
      _route_add_point(route, route->via_x, start_y);
    else
      _route_add_point(route, start_x, route->via_y);
    _route_add_point(route, route->via_x, route->via_y);
    if(to_horizontal)
      _route_add_point(route, route->via_x, end_y);
    else
      _route_add_point(route, end_x, route->via_y);
  }
  else if(from_horizontal && to_horizontal)
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

static void _route_flatten_cubic(dt_canvas_route_t *route, const double start_x, const double start_y,
                                 const double control1_x, const double control1_y, const double control2_x,
                                 const double control2_y, const double end_x, const double end_y,
                                 const int segments, const gboolean skip_start)
{
  for(int idx = skip_start ? 1 : 0; idx <= segments; idx++)
  {
    const double parameter = (double)idx / (double)segments;
    const double remaining = 1.0 - parameter;
    const double weight0 = remaining * remaining * remaining;
    const double weight1 = 3.0 * remaining * remaining * parameter;
    const double weight2 = 3.0 * remaining * parameter * parameter;
    const double weight3 = parameter * parameter * parameter;
    _route_add_point(route, weight0 * start_x + weight1 * control1_x + weight2 * control2_x + weight3 * end_x,
                     weight0 * start_y + weight1 * control1_y + weight2 * control2_y + weight3 * end_y);
  }
}

/**
 * Cubic routing: control points along the normals, flattened for hit tests. `floored` keeps the
 * automatic lengths at least 40 units, which gives a curve room to leave a frame; a line with a
 * free end has no frame to leave there, and a short one would loop out past its own ends.
 */
static void _route_cubic(dt_canvas_route_t *route, const dt_canvas_connector_t *connector, const gboolean floored)
{
  if(route->segment_count == 2)
  {
    // Two curves meeting at the waypoint with one tangent: the waypoint's own handle, or
    // the direction from start to end.
    double tangent_x = connector->via_tangent_x;
    double tangent_y = connector->via_tangent_y;
    const double reach1_measured = hypot(route->via_x - route->from_x, route->via_y - route->from_y) * 0.4;
    const double reach2_measured = hypot(route->to_x - route->via_x, route->to_y - route->via_y) * 0.4;
    const double reach1_auto = fmax(40.0, reach1_measured);
    const double reach2_auto = fmax(40.0, reach2_measured);
    const double reach1 = connector->from_reach > 0.0f ? connector->from_reach : reach1_auto;
    const double reach2 = connector->to_reach > 0.0f ? connector->to_reach : reach2_auto;
    // A free end brings its own reach, so the floor above only reaches a line through the
    // waypoint's automatic tangent. Unfloored, that tangent spans the shorter leg's share: it is
    // laid along the chord on both sides of the waypoint, and a waypoint near one end would
    // otherwise throw the curve out past that end. Floored, it is the start leg's share alone, which
    // is what every route between two frames through a waypoint has always drawn and still draws; it
    // is also why such a curve depends on which end is the start, so a reversed connector walks the
    // same curve back only once that tangent has been dragged or both legs ask for one length.
    const double tangent_reach = floored ? reach1_auto : fmin(reach1_measured, reach2_measured);
    if(hypot(tangent_x, tangent_y) < 1e-9)
    {
      tangent_x = route->to_x - route->from_x;
      tangent_y = route->to_y - route->from_y;
      const double tangent_length = hypot(tangent_x, tangent_y);
      if(tangent_length > 1e-9)
      {
        tangent_x *= tangent_reach / tangent_length;
        tangent_y *= tangent_reach / tangent_length;
      }
    }
    route->control1_x = route->from_x + route->from_normal_x * reach1;
    route->control1_y = route->from_y + route->from_normal_y * reach1;
    route->control2_x = route->via_x - tangent_x;
    route->control2_y = route->via_y - tangent_y;
    route->control3_x = route->via_x + tangent_x;
    route->control3_y = route->via_y + tangent_y;
    route->control4_x = route->to_x + route->to_normal_x * reach2;
    route->control4_y = route->to_y + route->to_normal_y * reach2;
    const int segments = (DT_CANVAS_ROUTE_MAX_POINTS - 1) / 2;
    _route_flatten_cubic(route, route->from_x, route->from_y, route->control1_x, route->control1_y,
                         route->control2_x, route->control2_y, route->via_x, route->via_y, segments, FALSE);
    _route_flatten_cubic(route, route->via_x, route->via_y, route->control3_x, route->control3_y,
                         route->control4_x, route->control4_y, route->to_x, route->to_y, segments, TRUE);
    return;
  }
  const double distance = hypot(route->to_x - route->from_x, route->to_y - route->from_y);
  const double reach_auto = fmax(40.0, distance * 0.4);
  const double reach1 = connector->from_reach > 0.0f ? connector->from_reach : reach_auto;
  const double reach2 = connector->to_reach > 0.0f ? connector->to_reach : reach_auto;
  route->control1_x = route->from_x + route->from_normal_x * reach1;
  route->control1_y = route->from_y + route->from_normal_y * reach1;
  route->control2_x = route->to_x + route->to_normal_x * reach2;
  route->control2_y = route->to_y + route->to_normal_y * reach2;
  _route_flatten_cubic(route, route->from_x, route->from_y, route->control1_x, route->control1_y, route->control2_x,
                       route->control2_y, route->to_x, route->to_y, DT_CANVAS_ROUTE_MAX_POINTS - 1, FALSE);
}

/**
 * A free end's normal and the reach its control point sits at. Its own tangent gives both when it
 * has one. Otherwise it leaves toward where the route heads next, at a fraction of that distance
 * rather than the anchored ends' floor, so a short line does not overshoot itself; and ends that
 * coincide still leave along an axis, which keeps every normal finite and gives square routing's
 * horizontal-or-vertical test a side to take.
 * @return the distance to where the route heads next, which bounds a square stub the same way.
 */
static double _route_free_normal(const double end_x, const double end_y, const float tangent_x,
                                 const float tangent_y, const double aim_x, const double aim_y,
                                 const double fallback_x, double *normal_x, double *normal_y, float *reach)
{
  const double delta_x = aim_x - end_x;
  const double delta_y = aim_y - end_y;
  const double distance = hypot(delta_x, delta_y);
  const double tangent_length = hypot((double)tangent_x, (double)tangent_y);
  if(tangent_length > 0.0)
  {
    *normal_x = (double)tangent_x / tangent_length;
    *normal_y = (double)tangent_y / tangent_length;
    *reach = (float)tangent_length;
    return distance;
  }
  if(distance > 1e-9)
  {
    *normal_x = delta_x / distance;
    *normal_y = delta_y / distance;
  }
  else
  {
    *normal_x = fallback_x;
    *normal_y = 0.0;
  }
  // Never zero: a zero reach means "automatic" to the cubic, which would put the 40-unit floor back.
  *reach = fmaxf((float)(CANVAS_FREE_REACH * distance), FLT_MIN);
  return distance;
}

/**
 * A square stub. An anchored end's clears its frame, so it keeps its 20-unit floor. A free end's
 * leaves toward where the route heads next and is held to half that distance, so a short line's
 * stubs meet rather than cross and run past both of its ends.
 */
static double _route_square_stub(const double chord, const gboolean free_end, const double aim_distance)
{
  const double anchored_stub = CLAMP(chord * 0.25, 20.0, 60.0);
  if(!free_end) return anchored_stub;
  return MIN(anchored_stub, aim_distance * 0.5);
}

gboolean dt_canvas_connector_route(const dt_canvas_t *canvas, const dt_canvas_object_t *connector,
                                   dt_canvas_route_t *route)
{
  if(IS_NULL_PTR(connector) || IS_NULL_PTR(route) || connector->kind != DT_CANVAS_OBJECT_CONNECTOR) return FALSE;
  const dt_canvas_connector_t *line = &connector->connector;
  // The four steps run in this order so nothing is circular: which ends are free, where the free
  // ends are, where the anchored ends are (aiming at what step two placed), and only then which
  // way the free ends leave (toward what steps two and three placed).
  //
  // 1. An anchored end must be on a frame; an id of 0 is free.
  const gboolean from_free = line->from_id == 0;
  const gboolean to_free = line->to_id == 0;
  const dt_canvas_object_t *from = from_free ? NULL : dt_canvas_find_object(canvas, line->from_id);
  const dt_canvas_object_t *to = to_free ? NULL : dt_canvas_find_object(canvas, line->to_id);
  if(!from_free && !dt_canvas_object_is_frame(from)) return FALSE;
  if(!to_free && !dt_canvas_object_is_frame(to)) return FALSE;
  memset(route, 0, sizeof(*route));
  route->routing = line->routing;
  route->segment_count = line->via_count > 0 ? 2 : 1;
  route->via_x = line->via_x;
  route->via_y = line->via_y;
  // 2. The free points.
  if(from_free)
  {
    route->from_x = line->from_x;
    route->from_y = line->from_y;
  }
  if(to_free)
  {
    route->to_x = line->to_x;
    route->to_y = line->to_y;
  }
  // 3. An anchored end aims at the other frame's centre, exactly as it always has, so a document
  //    from before free ends routes to the same bits; at the other end's point when that is free.
  if(!from_free)
  {
    const double target_x = to_free ? route->to_x : to->x;
    const double target_y = to_free ? route->to_y : to->y;
    dt_canvas_object_anchor_point(canvas, from, (dt_canvas_anchor_t)line->from_anchor, target_x, target_y,
                                  &route->from_x, &route->from_y, &route->from_normal_x, &route->from_normal_y);
  }
  if(!to_free)
  {
    const double target_x = from_free ? route->from_x : from->x;
    const double target_y = from_free ? route->from_y : from->y;
    dt_canvas_object_anchor_point(canvas, to, (dt_canvas_anchor_t)line->to_anchor, target_x, target_y,
                                  &route->to_x, &route->to_y, &route->to_normal_x, &route->to_normal_y);
  }
  // 4. The free normals, and their reaches written into a copy: the routings below read a reach
  //    from the connector, and the document's own record is not this function's to change.
  dt_canvas_connector_t effective = *line;
  double from_aim_distance = 0.0;
  double to_aim_distance = 0.0;
  if(from_free)
  {
    const double aim_x = route->segment_count == 2 ? route->via_x : route->to_x;
    const double aim_y = route->segment_count == 2 ? route->via_y : route->to_y;
    from_aim_distance = _route_free_normal(route->from_x, route->from_y, line->from_tangent_x, line->from_tangent_y,
                                           aim_x, aim_y, 1.0, &route->from_normal_x, &route->from_normal_y,
                                           &effective.from_reach);
  }
  if(to_free)
  {
    const double aim_x = route->segment_count == 2 ? route->via_x : route->from_x;
    const double aim_y = route->segment_count == 2 ? route->via_y : route->from_y;
    to_aim_distance = _route_free_normal(route->to_x, route->to_y, line->to_tangent_x, line->to_tangent_y, aim_x,
                                         aim_y, -1.0, &route->to_normal_x, &route->to_normal_y,
                                         &effective.to_reach);
  }
  switch(route->routing)
  {
    case DT_CANVAS_ROUTING_SQUARE:
    {
      const double chord = hypot(route->to_x - route->from_x, route->to_y - route->from_y);
      const double from_stub = _route_square_stub(chord, from_free, from_aim_distance);
      const double to_stub = _route_square_stub(chord, to_free, to_aim_distance);
      _route_square(route, from_stub, to_stub);
      break;
    }
    case DT_CANVAS_ROUTING_CUBIC:
      _route_cubic(route, &effective, !from_free && !to_free);
      break;
    case DT_CANVAS_ROUTING_STRAIGHT:
    default:
      route->routing = DT_CANVAS_ROUTING_STRAIGHT;
      _route_add_point(route, route->from_x, route->from_y);
      if(route->segment_count == 2) _route_add_point(route, route->via_x, route->via_y);
      _route_add_point(route, route->to_x, route->to_y);
      break;
  }
  return route->point_count >= 2;
}

gboolean dt_canvas_connector_seed_curve(dt_canvas_object_t *object, const dt_canvas_route_t *route)
{
  if(IS_NULL_PTR(object) || IS_NULL_PTR(route) || object->kind != DT_CANVAS_OBJECT_CONNECTOR) return FALSE;
  dt_canvas_connector_t *line = &object->connector;
  // A waypoint already bends the curve, and an anchored end leaves along its frame's normal.
  if(line->routing != DT_CANVAS_ROUTING_CUBIC || line->via_count > 0) return FALSE;
  const gboolean seed_from = line->from_id == 0 && line->from_tangent_x == 0.0f && line->from_tangent_y == 0.0f;
  const gboolean seed_to = line->to_id == 0 && line->to_tangent_x == 0.0f && line->to_tangent_y == 0.0f;
  if(!seed_from && !seed_to) return FALSE;
  const double chord_x = route->to_x - route->from_x;
  const double chord_y = route->to_y - route->from_y;
  if(!(hypot(chord_x, chord_y) > 1e-9)) return FALSE;
  // R(phi) = [cos -sin; sin cos] turns clockwise on screen, y being down. The start leaves along
  // R(-30 degrees) of the chord and the end arrives along R(+30 degrees) of it, so both control
  // points fall on the same side and the arc is symmetric about the chord's perpendicular.
  const double cos_angle = cos(CANVAS_SEED_ANGLE);
  const double sin_angle = sin(CANVAS_SEED_ANGLE);
  if(seed_from)
  {
    line->from_tangent_x = (float)(CANVAS_FREE_REACH * (cos_angle * chord_x + sin_angle * chord_y));
    line->from_tangent_y = (float)(CANVAS_FREE_REACH * (-sin_angle * chord_x + cos_angle * chord_y));
  }
  if(seed_to)
  {
    line->to_tangent_x = (float)(-CANVAS_FREE_REACH * (cos_angle * chord_x - sin_angle * chord_y));
    line->to_tangent_y = (float)(-CANVAS_FREE_REACH * (sin_angle * chord_x + cos_angle * chord_y));
  }
  return TRUE;
}

void dt_canvas_route_midpoint(const dt_canvas_route_t *route, double *x, double *y)
{
  dt_canvas_route_point_at(route, 0.5, x, y);
}

/** The whole length of a route's polyline. */
static double _route_length(const dt_canvas_route_t *route)
{
  double total = 0.0;
  for(int idx = 0; idx + 1 < route->point_count; idx++)
    total += hypot(route->points[2 * idx + 2] - route->points[2 * idx], route->points[2 * idx + 3] - route->points[2 * idx + 1]);
  return total;
}

void dt_canvas_route_point_at(const dt_canvas_route_t *route, const double fraction, double *x, double *y)
{
  // By arc length, whatever the routing and however many points it was flattened to: the
  // chord's point only when the route has no length to walk.
  const double wanted = CLAMP(fraction, 0.0, 1.0);
  const double total = _route_length(route);
  double walked = 0.0;
  *x = route->from_x + (route->to_x - route->from_x) * wanted;
  *y = route->from_y + (route->to_y - route->from_y) * wanted;
  for(int idx = 0; idx + 1 < route->point_count; idx++)
  {
    const double segment = hypot(route->points[2 * idx + 2] - route->points[2 * idx], route->points[2 * idx + 3] - route->points[2 * idx + 1]);
    if(walked + segment >= total * wanted && segment > 0.0)
    {
      const double along = (total * wanted - walked) / segment;
      *x = route->points[2 * idx] + (route->points[2 * idx + 2] - route->points[2 * idx]) * along;
      *y = route->points[2 * idx + 1] + (route->points[2 * idx + 3] - route->points[2 * idx + 1]) * along;
      break;
    }
    walked += segment;
  }
}

double dt_canvas_route_fraction_at(const dt_canvas_route_t *route, const double x, const double y)
{
  if(IS_NULL_PTR(route)) return 0.5;
  const double total = _route_length(route);
  if(!(total > 0.0)) return 0.5;
  double closest_distance = INFINITY;
  double closest_walk = 0.0;
  double walked = 0.0;
  for(int idx = 0; idx + 1 < route->point_count; idx++)
  {
    const double start_x = route->points[2 * idx];
    const double start_y = route->points[2 * idx + 1];
    const double delta_x = route->points[2 * idx + 2] - start_x;
    const double delta_y = route->points[2 * idx + 3] - start_y;
    const double segment = hypot(delta_x, delta_y);
    double along = 0.0;
    if(segment > 0.0)
      along = CLAMP(((x - start_x) * delta_x + (y - start_y) * delta_y) / (segment * segment), 0.0, 1.0);
    const double distance = hypot(start_x + delta_x * along - x, start_y + delta_y * along - y);
    // Strictly closer only: where two legs meet at the same distance, the earlier one keeps it.
    if(distance < closest_distance)
    {
      closest_distance = distance;
      closest_walk = walked + segment * along;
    }
    walked += segment;
  }
  return CLAMP(closest_walk / total, 0.0, 1.0);
}

void dt_canvas_connector_add_via(dt_canvas_t *canvas, dt_canvas_object_t *connector)
{
  if(IS_NULL_PTR(canvas) || IS_NULL_PTR(connector) || connector->kind != DT_CANVAS_OBJECT_CONNECTOR) return;
  dt_canvas_route_t route;
  connector->connector.via_count = 0;
  if(!dt_canvas_connector_route(canvas, connector, &route)) return;
  // The middle of the current route, so adding a waypoint changes nothing until it is moved.
  dt_canvas_route_midpoint(&route, &connector->connector.via_x, &connector->connector.via_y);
  connector->connector.via_count = 1;
  dt_canvas_touch(canvas);
}

void dt_canvas_connector_remove_via(dt_canvas_t *canvas, dt_canvas_object_t *connector)
{
  if(IS_NULL_PTR(canvas) || IS_NULL_PTR(connector) || connector->kind != DT_CANVAS_OBJECT_CONNECTOR) return;
  connector->connector.via_count = 0;
  dt_canvas_touch(canvas);
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

double dt_canvas_stroke_reach(const double line_width, const uint32_t style)
{
  const double width = line_width > 0.0 ? line_width : DT_CANVAS_CONNECTOR_LINE_WIDTH;
  if(style & (DT_CANVAS_CONNECTOR_ARROW_END | DT_CANVAS_CONNECTOR_ARROW_START))
    return width + DT_CANVAS_ARROW_LENGTH * fmax(width / 2.0, 1.0);
  return width / 2.0;
}

void dt_canvas_route_arrow_head(const dt_canvas_route_t *route, const gboolean at_end, const double line_width,
                                double xy[6])
{
  const double width = line_width > 0.0 ? line_width : DT_CANVAS_CONNECTOR_LINE_WIDTH;
  // A head is sized to its line, so a thick connector gets a proportionate one.
  const double scale = fmax(width / 2.0, 1.0);
  const int last = route->point_count - 1;
  const double tip_x = at_end ? route->to_x : route->from_x;
  const double tip_y = at_end ? route->to_y : route->from_y;
  // It points along the leg it ends: for a straight line the chord, for the others the stub or
  // the last flattened step of the curve.
  const double behind_x = at_end ? route->points[2 * last - 2] : route->points[2];
  const double behind_y = at_end ? route->points[2 * last - 1] : route->points[3];
  const double angle = atan2(tip_y - behind_y, tip_x - behind_x);
  const double length = DT_CANVAS_ARROW_LENGTH * scale;
  const double half_width = DT_CANVAS_ARROW_HALF_WIDTH * scale;
  const double base_x = tip_x - cos(angle) * length;
  const double base_y = tip_y - sin(angle) * length;
  const double normal_x = -sin(angle) * half_width;
  const double normal_y = cos(angle) * half_width;
  xy[0] = tip_x;
  xy[1] = tip_y;
  xy[2] = base_x + normal_x;
  xy[3] = base_y + normal_y;
  xy[4] = base_x - normal_x;
  xy[5] = base_y - normal_y;
}

/**
 * Grow [low, high] to hold one axis of a cubic Bezier. Its extremes are its ends and wherever the
 * derivative, a quadratic, crosses zero inside the curve -- which the control points bound but
 * rarely reach: a seeded arc peaks at three quarters of its controls' height.
 */
static void _cubic_axis_range(const double p0, const double p1, const double p2, const double p3, double *low,
                              double *high)
{
  *low = fmin(*low, fmin(p0, p3));
  *high = fmax(*high, fmax(p0, p3));
  // B'(t) / 3 = a t^2 + b t + c.
  const double a = -p0 + 3.0 * p1 - 3.0 * p2 + p3;
  const double b = 2.0 * (p0 - 2.0 * p1 + p2);
  const double c = p1 - p0;
  double roots[2] = { -1.0, -1.0 };
  const double scale = fabs(a) + fabs(b) + fabs(c);
  if(!(scale > 0.0)) return;
  if(fabs(a) <= 1e-12 * scale)
  {
    if(fabs(b) > 1e-12 * scale) roots[0] = -c / b;
  }
  else
  {
    const double discriminant = b * b - 4.0 * a * c;
    if(discriminant >= 0.0)
    {
      const double root = sqrt(discriminant);
      roots[0] = (-b + root) / (2.0 * a);
      roots[1] = (-b - root) / (2.0 * a);
    }
  }
  for(int idx = 0; idx < 2; idx++)
  {
    const double parameter = roots[idx];
    if(!(parameter > 0.0 && parameter < 1.0)) continue;
    const double remaining = 1.0 - parameter;
    const double value = remaining * remaining * remaining * p0 + 3.0 * remaining * remaining * parameter * p1
                         + 3.0 * remaining * parameter * parameter * p2 + parameter * parameter * parameter * p3;
    *low = fmin(*low, value);
    *high = fmax(*high, value);
  }
}

gboolean dt_canvas_object_extent(const dt_canvas_t *canvas, const dt_canvas_object_t *object, dt_canvas_rect_t *out)
{
  if(IS_NULL_PTR(object) || IS_NULL_PTR(out)) return FALSE;
  if(object->kind != DT_CANVAS_OBJECT_CONNECTOR)
  {
    *out = dt_canvas_object_bounds(object);
    return TRUE;
  }
  dt_canvas_route_t route;
  if(!dt_canvas_connector_route(canvas, object, &route) || route.point_count < 2) return FALSE;
  double min_x = route.from_x;
  double min_y = route.from_y;
  double max_x = route.from_x;
  double max_y = route.from_y;
  if(route.routing == DT_CANVAS_ROUTING_CUBIC)
  {
    // The painter strokes the curve itself, not the polyline it is flattened to for hit tests.
    const gboolean through_via = route.segment_count == 2;
    const double first_end_x = through_via ? route.via_x : route.to_x;
    const double first_end_y = through_via ? route.via_y : route.to_y;
    _cubic_axis_range(route.from_x, route.control1_x, route.control2_x, first_end_x, &min_x, &max_x);
    _cubic_axis_range(route.from_y, route.control1_y, route.control2_y, first_end_y, &min_y, &max_y);
    if(through_via)
    {
      _cubic_axis_range(route.via_x, route.control3_x, route.control4_x, route.to_x, &min_x, &max_x);
      _cubic_axis_range(route.via_y, route.control3_y, route.control4_y, route.to_y, &min_y, &max_y);
    }
  }
  else
  {
    for(int idx = 0; idx < route.point_count; idx++)
    {
      min_x = fmin(min_x, route.points[2 * idx]);
      max_x = fmax(max_x, route.points[2 * idx]);
      min_y = fmin(min_y, route.points[2 * idx + 1]);
      max_y = fmax(max_y, route.points[2 * idx + 1]);
    }
  }
  // Round caps and joins keep the stroke within half its width of the path it follows.
  const double half_width = dt_canvas_stroke_reach(object->connector.line_width, DT_CANVAS_CONNECTOR_PLAIN);
  min_x -= half_width;
  min_y -= half_width;
  max_x += half_width;
  max_y += half_width;
  // A head is filled, not stroked, and reaches past the line only as far as its own triangle.
  const uint32_t heads[2] = { DT_CANVAS_CONNECTOR_ARROW_START, DT_CANVAS_CONNECTOR_ARROW_END };
  for(int end = 0; end < 2; end++)
  {
    if(!(object->connector.style & heads[end])) continue;
    double triangle[6];
    dt_canvas_route_arrow_head(&route, end == 1, object->connector.line_width, triangle);
    for(int corner = 0; corner < 3; corner++)
    {
      min_x = fmin(min_x, triangle[2 * corner]);
      max_x = fmax(max_x, triangle[2 * corner]);
      min_y = fmin(min_y, triangle[2 * corner + 1]);
      max_y = fmax(max_y, triangle[2 * corner + 1]);
    }
  }
  out->x = min_x;
  out->y = min_y;
  out->width = max_x - min_x;
  out->height = max_y - min_y;
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
    // A line is content of its own; a connector anchored at both ends lies between frames that
    // are counted already, and leaving it out keeps an older document framed as it was.
    const gboolean counted = dt_canvas_object_is_frame(object) || dt_canvas_connector_has_free_end(object);
    if(!counted || (object->flags & DT_CANVAS_OBJECT_FLAG_HIDDEN)) continue;
    dt_canvas_rect_t object_bounds;
    if(!dt_canvas_object_extent(canvas, object, &object_bounds)) continue;
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

void dt_canvas_constrain_line_end(const dt_canvas_t *canvas, const double origin_x, const double origin_y,
                                  const int step_degrees, double *x, double *y)
{
  if(IS_NULL_PTR(x) || IS_NULL_PTR(y)) return;
  if(step_degrees <= 0)
  {
    *x = dt_canvas_snap(canvas, *x);
    *y = dt_canvas_snap(canvas, *y);
    return;
  }
  const double delta_x = *x - origin_x;
  const double delta_y = *y - origin_y;
  if(!(hypot(delta_x, delta_y) > 0.0)) return;
  const long steps = lround(atan2(delta_y, delta_x) * 180.0 / M_PI / step_degrees);
  // The step count is kept whole so the four axes are recognised exactly rather than by a cosine
  // that only comes near zero: an end locked level must have its other end's height to the bit.
  const long degrees = ((steps * step_degrees) % 360 + 360) % 360;
  // On an axis the pointer's projection is simply its coordinate along that axis.
  if(degrees == 0 || degrees == 180)
  {
    *x = dt_canvas_snap(canvas, *x);
    *y = origin_y;
    return;
  }
  if(degrees == 90 || degrees == 270)
  {
    *x = origin_x;
    *y = dt_canvas_snap(canvas, *y);
    return;
  }
  // A diagonal crosses the grid's points only at whole multiples of a cell on both axes, so
  // snapping either coordinate would bend it off the angle that was asked for.
  const double angle = degrees * M_PI / 180.0;
  const double direction_x = cos(angle);
  const double direction_y = sin(angle);
  const double along = delta_x * direction_x + delta_y * direction_y;
  *x = origin_x + along * direction_x;
  *y = origin_y + along * direction_y;
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

/* --- snapping to neighbours ----------------------------------------------------- */

static gboolean _excluded(const GArray *exclude, const uint32_t id)
{
  if(IS_NULL_PTR(exclude)) return FALSE;
  for(guint idx = 0; idx < exclude->len; idx++)
  {
    if(g_array_index(exclude, uint32_t, idx) == id) return TRUE;
  }
  return FALSE;
}

/** Keep `candidate` as the axis's snap when it beats the current best within `threshold`. */
static void _snap_axis(const double current, const double candidate, const double threshold, double *best_delta,
                       gboolean *found)
{
  const double delta = candidate - current;
  if(fabs(delta) > threshold) return;
  if(*found && fabs(delta) >= fabs(*best_delta)) return;
  *best_delta = delta;
  *found = TRUE;
}

gboolean dt_canvas_snap_to_neighbours(const dt_canvas_t *canvas, const dt_canvas_rect_t *moving,
                                      const GArray *exclude, double threshold, uint32_t edges, double *delta_x,
                                      double *delta_y)
{
  if(IS_NULL_PTR(canvas) || IS_NULL_PTR(moving)) return FALSE;
  const gboolean snap_left = (edges & DT_CANVAS_EDGE_LEFT) != 0;
  const gboolean snap_right = (edges & DT_CANVAS_EDGE_RIGHT) != 0;
  const gboolean snap_top = (edges & DT_CANVAS_EDGE_TOP) != 0;
  const gboolean snap_bottom = (edges & DT_CANVAS_EDGE_BOTTOM) != 0;
  const double padding = canvas->padding > 0.0f ? canvas->padding : 0.0;
  const double left = moving->x;
  const double right = moving->x + moving->width;
  const double top = moving->y;
  const double bottom = moving->y + moving->height;
  gboolean found_x = FALSE;
  gboolean found_y = FALSE;
  double best_x = 0.0;
  double best_y = 0.0;
  for(guint idx = 0; idx < canvas->objects->len; idx++)
  {
    const dt_canvas_object_t *other = g_ptr_array_index(canvas->objects, idx);
    if(!dt_canvas_object_is_frame(other) || (other->flags & DT_CANVAS_OBJECT_FLAG_HIDDEN)) continue;
    if(_excluded(exclude, other->id)) continue;
    const dt_canvas_rect_t bounds = dt_canvas_object_bounds(other);
    const double other_left = bounds.x;
    const double other_right = bounds.x + bounds.width;
    const double other_top = bounds.y;
    const double other_bottom = bounds.y + bounds.height;
    // The padding is a MARGIN AROUND each frame, not a gap between two, so two frames sit
    // side by side when their margins TOUCH and the clear space between them is two paddings.
    // Keyed on one padding, a frame's own margin box landed exactly on its neighbour's edge
    // and the two boxes overlapped across the whole gap, each drawing its line on top of the
    // other frame's border -- box against frame, which is what read as odd and crossing.
    // Box against box, they share one line.
    if(snap_left)
    {
      _snap_axis(left, other_right + 2.0 * padding, threshold, &best_x, &found_x);
      _snap_axis(left, other_left, threshold, &best_x, &found_x);
    }
    if(snap_right)
    {
      _snap_axis(right, other_left - 2.0 * padding, threshold, &best_x, &found_x);
      _snap_axis(right, other_right, threshold, &best_x, &found_x);
    }
    if(snap_top)
    {
      _snap_axis(top, other_bottom + 2.0 * padding, threshold, &best_y, &found_y);
      _snap_axis(top, other_top, threshold, &best_y, &found_y);
    }
    if(snap_bottom)
    {
      _snap_axis(bottom, other_top - 2.0 * padding, threshold, &best_y, &found_y);
      _snap_axis(bottom, other_bottom, threshold, &best_y, &found_y);
    }
  }
  if(!IS_NULL_PTR(delta_x)) *delta_x = found_x ? best_x : 0.0;
  if(!IS_NULL_PTR(delta_y)) *delta_y = found_y ? best_y : 0.0;
  return found_x || found_y;
}

typedef struct dt_canvas_size_candidate_t
{
  double *best_delta;
  gboolean *found;
  dt_canvas_rect_t *reference;
} dt_canvas_size_candidate_t;

static void _snap_size_axis(const double current, const double candidate, const double threshold,
                            const dt_canvas_rect_t *box, dt_canvas_size_candidate_t *axis)
{
  const gboolean was_found = *axis->found;
  const double previous = *axis->best_delta;
  _snap_axis(current, candidate, threshold, axis->best_delta, axis->found);
  if(*axis->found && (!was_found || *axis->best_delta != previous) && !IS_NULL_PTR(axis->reference))
    *axis->reference = *box;
}

#define CANVAS_RUN_MAX 8
#define CANVAS_RUN_TOLERANCE 2.0

static gboolean _ranges_overlap(const double start_a, const double end_a, const double start_b, const double end_b)
{
  return start_a < end_b && start_b < end_a;
}

/**
 * Every run of frames stacked one padding apart, starting at `start`, as a growing box: the
 * masonry candidates. `vertical` walks downwards, else rightwards.
 */
static void _snap_size_runs(const dt_canvas_t *canvas, const GArray *exclude, const dt_canvas_rect_t *start,
                            const gboolean vertical, const double current, const double threshold,
                            dt_canvas_size_candidate_t *axis)
{
  const double padding = canvas->padding > 0.0f ? canvas->padding : 0.0;
  dt_canvas_rect_t span = *start;
  for(int step = 0; step < CANVAS_RUN_MAX; step++)
  {
    gboolean extended = FALSE;
    for(guint idx = 0; idx < canvas->objects->len && !extended; idx++)
    {
      const dt_canvas_object_t *other = g_ptr_array_index(canvas->objects, idx);
      if(!dt_canvas_object_is_frame(other) || (other->flags & DT_CANVAS_OBJECT_FLAG_HIDDEN)) continue;
      if(_excluded(exclude, other->id)) continue;
      const dt_canvas_rect_t bounds = dt_canvas_object_bounds(other);
      if(vertical)
      {
        if(fabs(bounds.y - (span.y + span.height + 2.0 * padding)) > CANVAS_RUN_TOLERANCE) continue;
        if(!_ranges_overlap(span.x, span.x + span.width, bounds.x, bounds.x + bounds.width)) continue;
        const double left = fmin(span.x, bounds.x);
        const double right = fmax(span.x + span.width, bounds.x + bounds.width);
        span.x = left;
        span.width = right - left;
        span.height = bounds.y + bounds.height - span.y;
      }
      else
      {
        if(fabs(bounds.x - (span.x + span.width + 2.0 * padding)) > CANVAS_RUN_TOLERANCE) continue;
        if(!_ranges_overlap(span.y, span.y + span.height, bounds.y, bounds.y + bounds.height)) continue;
        const double top = fmin(span.y, bounds.y);
        const double bottom = fmax(span.y + span.height, bounds.y + bounds.height);
        span.y = top;
        span.height = bottom - top;
        span.width = bounds.x + bounds.width - span.x;
      }
      extended = TRUE;
    }
    if(!extended) return;
    _snap_size_axis(current, vertical ? span.height : span.width, threshold, &span, axis);
  }
}

gboolean dt_canvas_snap_size(const dt_canvas_t *canvas, const GArray *exclude, double threshold, double *width,
                             double *height, dt_canvas_rect_t *width_reference, dt_canvas_rect_t *height_reference)
{
  if(IS_NULL_PTR(canvas)) return FALSE;
  gboolean found_width = FALSE;
  gboolean found_height = FALSE;
  double best_width = 0.0;
  double best_height = 0.0;
  dt_canvas_size_candidate_t width_axis = { &best_width, &found_width, width_reference };
  dt_canvas_size_candidate_t height_axis = { &best_height, &found_height, height_reference };
  for(guint idx = 0; idx < canvas->objects->len; idx++)
  {
    const dt_canvas_object_t *other = g_ptr_array_index(canvas->objects, idx);
    if(!dt_canvas_object_is_frame(other) || (other->flags & DT_CANVAS_OBJECT_FLAG_HIDDEN)) continue;
    if(_excluded(exclude, other->id)) continue;
    const dt_canvas_rect_t bounds = dt_canvas_object_bounds(other);
    if(!IS_NULL_PTR(width))
    {
      _snap_size_axis(*width, bounds.width, threshold, &bounds, &width_axis);
      _snap_size_runs(canvas, exclude, &bounds, FALSE, *width, threshold, &width_axis);
    }
    if(!IS_NULL_PTR(height))
    {
      _snap_size_axis(*height, bounds.height, threshold, &bounds, &height_axis);
      _snap_size_runs(canvas, exclude, &bounds, TRUE, *height, threshold, &height_axis);
    }
  }
  if(found_width) *width += best_width;
  if(found_height) *height += best_height;
  return found_width || found_height;
}

/* --- paper ------------------------------------------------------------------- */

/**
 * Every page size, portrait. The order IS the stored value, so entries are only ever appended.
 *
 * A sheet of paper is given in POINTS and a screen format in PIXELS, and the two reach the
 * plane differently: a canvas unit is a display pixel, so a screen format is its pixel size
 * outright while a sheet is its size in points scaled by the canvas's resolution. Read both
 * as points, as they were, and an Instagram reel comes out 1080 units against an A4's 595 --
 * nearly twice the sheet, for something that fits in a hand.
 */
static const struct
{
  uint32_t code; ///< what is stored; the row's position is only where it is shown
  const char *name;
  double width;
  double height;
  gboolean physical; ///< the size is in points and scales with the resolution; else it is pixels
} _paper_sizes[] = {
  { DT_CANVAS_PAPER_NONE, N_("None"), 0.0, 0.0, FALSE },
  { DT_CANVAS_PAPER_A0, "A0", 2384.0, 3370.0, TRUE },
  { DT_CANVAS_PAPER_A1, "A1", 1684.0, 2384.0, TRUE },
  { DT_CANVAS_PAPER_A2, "A2", 1191.0, 1684.0, TRUE },
  { DT_CANVAS_PAPER_A3, "A3", 842.0, 1191.0, TRUE },
  { DT_CANVAS_PAPER_A4, "A4", 595.0, 842.0, TRUE },
  { DT_CANVAS_PAPER_A5, "A5", 420.0, 595.0, TRUE },
  { DT_CANVAS_PAPER_A6, "A6", 298.0, 420.0, TRUE },
  { DT_CANVAS_PAPER_LETTER, N_("US Letter"), 612.0, 792.0, TRUE },
  { DT_CANVAS_PAPER_INSTAGRAM_SQUARE, N_("Instagram square"), 1080.0, 1080.0, FALSE },
  { DT_CANVAS_PAPER_INSTAGRAM_PORTRAIT, N_("Instagram portrait"), 1080.0, 1350.0, FALSE },
  { DT_CANVAS_PAPER_STORY, N_("Story, reel, Short"), 1080.0, 1920.0, FALSE },
  { DT_CANVAS_PAPER_FACEBOOK_POST, N_("Facebook post"), 1200.0, 630.0, FALSE },
  { DT_CANVAS_PAPER_FACEBOOK_COVER, N_("Facebook cover"), 851.0, 315.0, FALSE },
  { DT_CANVAS_PAPER_YOUTUBE_THUMBNAIL, N_("YouTube thumbnail"), 1280.0, 720.0, FALSE },
  { DT_CANVAS_PAPER_YOUTUBE_BANNER, N_("YouTube banner"), 2560.0, 1440.0, FALSE },
};

gboolean dt_canvas_paper_is_physical(const uint32_t paper)
{
  for(int position = 0; position < dt_canvas_paper_count(); position++)
    if(_paper_sizes[position].code == paper) return _paper_sizes[position].physical;
  return FALSE;
}

/**
 * The OpenType features worth a name in a menu, each with the four-letter tag Pango reads.
 * Appended only: the stored form is the tag string, so the order here is nobody's business
 * but the menu's.
 */
static const struct
{
  const char *tag;
  const char *name;
  const char *tooltip;
} _text_features[] = {
  { "liga", N_("Standard ligatures"), N_("The joined pairs a font draws for fi, fl and the like") },
  { "dlig", N_("Discretionary ligatures"), N_("The decorative joins a font offers but does not use by default") },
  { "calt", N_("Contextual alternates"), N_("Letters the font swaps depending on their neighbours") },
  { "smcp", N_("Small capitals"), N_("Lower case drawn as capitals at lower-case height") },
  { "c2sc", N_("Capitals to small capitals"), N_("Capitals drawn at small-capital height too") },
  { "onum", N_("Old-style figures"), N_("Figures with ascenders and descenders, which sit in running text") },
  { "lnum", N_("Lining figures"), N_("Figures all of cap height, which sit in tables and headings") },
  { "tnum", N_("Tabular figures"), N_("Every figure the same width, so columns line up") },
  { "pnum", N_("Proportional figures"), N_("Every figure its own width, which reads better in a sentence") },
  { "zero", N_("Slashed zero"), N_("A zero told apart from a capital O") },
  { "frac", N_("Fractions"), N_("Figures around a slash drawn as a proper fraction") },
  { "sups", N_("Superscript"), N_("Figures and letters raised and reduced") },
  { "subs", N_("Subscript"), N_("Figures and letters lowered and reduced") },
  { "swsh", N_("Swashes"), N_("The flourished forms a font keeps for display") },
  { "kern", N_("Kerning"), N_("The pair adjustments the font asks for; on by default in every font that has them") },
  { "hlig", N_("Historical ligatures"), N_("The joins an old face used and a modern reader does not expect, the long s among them") },
  { "hist", N_("Historical forms"), N_("The letterforms an old face used in place of today's") },
  { "titl", N_("Titling"), N_("Capitals cut for a line set large, lighter than the text ones") },
  { "ordn", N_("Ordinals"), N_("The raised letters of 1st and 2nd, drawn for the purpose") },
  { "salt", N_("Stylistic alternates"), N_("The font's own alternative shapes, wherever it offers them") },
  { "case", N_("Case-sensitive forms"), N_("Punctuation and figures raised to suit a line set all in capitals") },
  { "cpsp", N_("Capital spacing"), N_("A little air added between capitals, which are drawn to sit closer") },
  { "unic", N_("Unicase"), N_("One height for capitals and lower case together") },
  { "aalt", N_("All alternates"), N_("Every alternative shape the font holds for a character, at once") },
  { "sinf", N_("Scientific inferiors"), N_("The lowered figures of a chemical formula, drawn for the purpose") },
  { "nalt", N_("Annotation forms"), N_("Circled and boxed forms, where the font draws them") },
  { "lfbd", N_("Optical left bound"), N_("The font's own idea of where its left edge should hang") },
  { "rtbd", N_("Optical right bound"), N_("The font's own idea of where its right edge should hang") },
  { "falt", N_("Final glyph alternates"), N_("The shapes a font keeps for the last glyph of a line") },
  { "expt", N_("Expert forms"), N_("The expert set of a Japanese face") },
  { "jalt", N_("Justification alternates"), N_("Shapes the font offers to help a line justify") },
  { "rand", N_("Randomise"), N_("A different shape each time a character repeats, where the font has them") },
};

/**
 * The features the LAYOUT ENGINE owns, which are never the user's to switch.
 *
 * A font ships these so that text can be shaped at all -- glyph composition, mark placement,
 * the joining forms of a cursive script, the language's own substitutions. HarfBuzz turns them
 * on and off as the script and the language require, and a checkbox that overrides that breaks
 * the rendering rather than styling it. Linux Libertine offers five of them among its 32, so
 * without this the panel would invite exactly that.
 */
static const char *const _shaping_features[] = {
  "abvf", "abvm", "abvs", "akhn", "blwf", "blwm", "blws", "ccmp", "cfar", "cjct", "curs", "dist",
  "dtls", "fin2", "fin3", "fina", "flac", "half", "haln", "init", "isol", "ljmo", "locl", "ltra",
  "ltrm", "mark", "med2", "medi", "mkmk", "mset", "nukt", "pref", "pres", "pstf", "psts", "rclt",
  "rkrf", "rlig", "rphf", "rtla", "rtlm", "rvrn", "ssty", "stch", "tjmo", "vjmo",
};


/** A numbered family: "ss01" is stylistic set 1, "cv01" character variant 1. 0 for anything else. */
static int _numbered_feature(const char *tag, const char *family)
{
  if(IS_NULL_PTR(tag) || strlen(tag) != 4 || strncmp(tag, family, 2) != 0) return 0;
  if(!g_ascii_isdigit(tag[2]) || !g_ascii_isdigit(tag[3])) return 0;
  return (tag[2] - '0') * 10 + (tag[3] - '0');
}

gboolean dt_canvas_text_feature_offered(const char *tag)
{
  if(IS_NULL_PTR(tag) || strlen(tag) != 4) return FALSE;
  for(size_t idx = 0; idx < sizeof(_shaping_features) / sizeof(_shaping_features[0]); idx++)
    if(g_strcmp0(_shaping_features[idx], tag) == 0) return FALSE;
  return TRUE;
}

gchar *dt_canvas_text_feature_label(const char *tag)
{
  if(IS_NULL_PTR(tag)) return NULL;
  for(size_t feature = 0; feature < sizeof(_text_features) / sizeof(_text_features[0]); feature++)
    if(g_strcmp0(_text_features[feature].tag, tag) == 0) return g_strdup(_(_text_features[feature].name));
  // A font's own numbered sets: there are up to twenty of each and only the font knows what
  // they draw, so they are named by their number rather than left as four characters.
  const int set = _numbered_feature(tag, "ss");
  if(set > 0) return g_strdup_printf(_("Stylistic set %d"), set);
  const int variant = _numbered_feature(tag, "cv");
  if(variant > 0) return g_strdup_printf(_("Character variant %d"), variant);
  return NULL;
}

gchar *dt_canvas_text_feature_hint(const char *tag)
{
  if(IS_NULL_PTR(tag)) return NULL;
  for(size_t feature = 0; feature < sizeof(_text_features) / sizeof(_text_features[0]); feature++)
    if(g_strcmp0(_text_features[feature].tag, tag) == 0) return g_strdup(_(_text_features[feature].tooltip));
  if(_numbered_feature(tag, "ss") > 0)
    return g_strdup(_("One of the font's own sets of alternative shapes; what it draws is the font's business"));
  if(_numbered_feature(tag, "cv") > 0)
    return g_strdup(_("One of the font's own alternative shapes for a single character"));
  return NULL;
}

gboolean dt_canvas_text_feature_is_on(const char *features, const char *tag)
{
  if(IS_NULL_PTR(features) || IS_NULL_PTR(tag) || features[0] == '\0') return FALSE;
  // The composed form is always "<tag> 1", so this reads back exactly what was written and
  // ignores anything else the string may hold rather than guessing at it.
  gchar *token = g_strdup_printf("%s 1", tag);
  const gboolean on = strstr(features, token) != NULL;
  dt_free(token);
  return on;
}

void dt_canvas_text_feature_set(char *features, const size_t length, const char *tag, const gboolean on)
{
  if(IS_NULL_PTR(features) || length == 0 || IS_NULL_PTR(tag) || tag[0] == '\0') return;
  if(dt_canvas_text_feature_is_on(features, tag) == on) return;
  if(on)
  {
    // A tag that will not fit whole is not written at all: half a tag is not a feature, and
    // the string is a fixed field in the file rather than something that can grow.
    const size_t wanted = strlen(features) + (features[0] == '\0' ? 0 : 2) + strlen(tag) + 2;
    if(wanted >= length) return;
    if(features[0] != '\0') g_strlcat(features, ", ", length);
    g_strlcat(features, tag, length);
    g_strlcat(features, " 1", length);
    return;
  }
  // Off: rebuild without it, which is the only way to close the gap it leaves behind.
  gchar **parts = g_strsplit(features, ",", -1);
  features[0] = '\0';
  for(int part = 0; !IS_NULL_PTR(parts[part]); part++)
  {
    gchar *trimmed = g_strstrip(g_strdup(parts[part]));
    if(trimmed[0] != '\0' && !g_str_has_prefix(trimmed, tag))
    {
      if(features[0] != '\0') g_strlcat(features, ", ", length);
      g_strlcat(features, trimmed, length);
    }
    dt_free(trimmed);
  }
  g_strfreev(parts);
}

void dt_canvas_text_margins(const dt_canvas_object_t *object, double margins[4])
{
  const double uniform = IS_NULL_PTR(object) ? 0.0 : fmax((double)object->text.padding, 0.0);
  for(int side = 0; side < 4; side++) margins[side] = uniform;
  if(IS_NULL_PTR(object)) return;
  // All four unset is a document from before the per-side margins, and takes the uniform one.
  // Any one of them set makes all four literal, so a side really can be zero -- the same rule
  // the canvas's texture weights follow, for the same reason.
  gboolean any = FALSE;
  for(int side = 0; side < 4; side++) any = any || object->text.margins[side] > 0.0f;
  if(!any) return;
  for(int side = 0; side < 4; side++) margins[side] = fmax((double)object->text.margins[side], 0.0);
}

double dt_canvas_resolution(const dt_canvas_t *canvas)
{
  // The density the page is RASTERISED at, and nothing else: the plane is measured in points
  // whatever this says, so changing it moves nothing on the page and only decides how many
  // pixels the export carries. A document with no answer takes the default rather than the
  // point, since 72 dpi is a preview and not a print.
  if(IS_NULL_PTR(canvas) || !(canvas->resolution > 0.0f)) return CANVAS_DEFAULT_RESOLUTION;
  return (double)canvas->resolution;
}

int dt_canvas_paper_count(void)
{
  return (int)(sizeof(_paper_sizes) / sizeof(_paper_sizes[0]));
}

const char *dt_canvas_paper_name(const int position)
{
  if(position < 0 || position >= dt_canvas_paper_count()) return NULL;
  // A0 through A6 are the same word in every language and are not in the catalogue.
  const uint32_t code = _paper_sizes[position].code;
  const gboolean iso = code == DT_CANVAS_PAPER_A0 || code == DT_CANVAS_PAPER_A1
                       || (code >= DT_CANVAS_PAPER_A2 && code <= DT_CANVAS_PAPER_A6);
  return iso ? _paper_sizes[position].name : _(_paper_sizes[position].name);
}

uint32_t dt_canvas_paper_code(const int position)
{
  if(position < 0 || position >= dt_canvas_paper_count()) return DT_CANVAS_PAPER_NONE;
  return _paper_sizes[position].code;
}

int dt_canvas_paper_position(const uint32_t paper)
{
  for(int position = 0; position < dt_canvas_paper_count(); position++)
    if(_paper_sizes[position].code == paper) return position;
  return 0;
}

gboolean dt_canvas_paper_points(const uint32_t paper, double *width, double *height)
{
  if(paper == DT_CANVAS_PAPER_NONE) return FALSE;
  for(int position = 0; position < dt_canvas_paper_count(); position++)
  {
    if(_paper_sizes[position].code != paper) continue;
    /*
     * Points, whichever way the format is named. A sheet of paper is stated in points already;
     * a screen format is stated in PIXELS, and a pixel is a physical length as soon as a
     * density is named for it -- the W3C's reference pixel, 96 to the inch, which is what every
     * browser and toolkit means by one. So a 1080 x 1920 story is 810 x 1440 points, and
     * exporting it at 96 dpi gives back exactly the 1080 x 1920 it is named for.
     *
     * That is what lets the two kinds of page be the same kind of thing: twelve points is
     * twelve points on A4 and on a story, and neither moves when the export density changes.
     */
    const double to_points = _paper_sizes[position].physical ? 1.0 : 72.0 / DT_CANVAS_REFERENCE_PIXEL_DPI;
    if(!IS_NULL_PTR(width)) *width = _paper_sizes[position].width * to_points;
    if(!IS_NULL_PTR(height)) *height = _paper_sizes[position].height * to_points;
    return TRUE;
  }
  return FALSE;
}

gboolean dt_canvas_paper_dimensions(const dt_canvas_t *canvas, double *width, double *height)
{
  if(IS_NULL_PTR(canvas)) return FALSE;
  double portrait_width = 0.0;
  double portrait_height = 0.0;
  if(!dt_canvas_paper_points(canvas->paper_size, &portrait_width, &portrait_height)) return FALSE;
  // A canvas unit IS a point, so a page is its own size and the export density does not enter
  // into it. Scaling the page by the density and leaving everything on it where it was is what
  // made raising the DPI shrink the whole layout against its own paper.
  if(!IS_NULL_PTR(width)) *width = canvas->paper_landscape ? portrait_height : portrait_width;
  if(!IS_NULL_PTR(height)) *height = canvas->paper_landscape ? portrait_width : portrait_height;
  return TRUE;
}

/** Pages to a spread along one axis; 0 is a plane tiled uniformly, and the cap keeps the walk finite. */
static int _spread_span(const uint32_t pages)
{
  return pages == 0u ? 0 : (int)MIN(pages, 64u);
}

/** The gap the plane opens between two spreads: one bleed for each of the sheets that meet there. */
static double _spread_gap(const dt_canvas_t *canvas)
{
  return 2.0 * fmax((double)canvas->page_bleed, 0.0);
}

/** How far a page index is pushed along by the gaps between all the spreads before it. */
static double _spread_offset(const int index, const int span, const double gap)
{
  if(span <= 0 || !(gap > 0.0)) return 0.0;
  // Floored, not truncated: a page left of the origin owes the gaps on its own side.
  const int spread = (int)floor((double)index / (double)span);
  return (double)spread * gap;
}

/** The page index whose extent covers `position`, or the nearest one when it falls in a gap. */
static int _page_index_at(const double position, const double extent, const int span, const double gap)
{
  if(!(extent > 0.0)) return 0;
  if(span <= 0 || !(gap > 0.0)) return (int)floor(position / extent);
  const double pitch = (double)span * extent + gap;
  const int spread = (int)floor(position / pitch);
  const double within = position - (double)spread * pitch;
  int page = (int)floor(within / extent);
  // Inside the gap between two sheets: the last page of the one on the left owns it.
  page = CLAMP(page, 0, span - 1);
  return spread * span + page;
}

dt_canvas_rect_t dt_canvas_page_rect(const dt_canvas_t *canvas, int col, int row)
{
  dt_canvas_rect_t rect = { 0.0, 0.0, 0.0, 0.0 };
  double width = 0.0;
  double height = 0.0;
  if(!dt_canvas_paper_dimensions(canvas, &width, &height)) return rect;
  const double gap = _spread_gap(canvas);
  rect.x = col * width + _spread_offset(col, _spread_span(canvas->spread_cols), gap);
  rect.y = row * height + _spread_offset(row, _spread_span(canvas->spread_rows), gap);
  rect.width = width;
  rect.height = height;
  return rect;
}

void dt_canvas_page_in_spread(const dt_canvas_t *canvas, const int col, const int row, int *across, int *down,
                              int *cols, int *rows)
{
  const int span_x = IS_NULL_PTR(canvas) ? 0 : _spread_span(canvas->spread_cols);
  const int span_y = IS_NULL_PTR(canvas) ? 0 : _spread_span(canvas->spread_rows);
  // With no spread every page is its own sheet: it sits at 0 of 1, and every edge is a trim.
  if(!IS_NULL_PTR(cols)) *cols = span_x > 0 ? span_x : 1;
  if(!IS_NULL_PTR(rows)) *rows = span_y > 0 ? span_y : 1;
  if(!IS_NULL_PTR(across)) *across = span_x > 0 ? col - (int)floor((double)col / span_x) * span_x : 0;
  if(!IS_NULL_PTR(down)) *down = span_y > 0 ? row - (int)floor((double)row / span_y) * span_y : 0;
}

gboolean dt_canvas_spread_rect(const dt_canvas_t *canvas, const int col, const int row, dt_canvas_rect_t *rect,
                               int *cols, int *rows)
{
  if(IS_NULL_PTR(canvas) || IS_NULL_PTR(rect)) return FALSE;
  int across = 0;
  int down = 0;
  int span_x = 1;
  int span_y = 1;
  dt_canvas_page_in_spread(canvas, col, row, &across, &down, &span_x, &span_y);
  const dt_canvas_rect_t first = dt_canvas_page_rect(canvas, col - across, row - down);
  if(!(first.width > 0.0) || !(first.height > 0.0)) return FALSE;
  // Pages inside a spread are contiguous, so the sheet is simply as many of them across.
  rect->x = first.x;
  rect->y = first.y;
  rect->width = first.width * span_x;
  rect->height = first.height * span_y;
  if(!IS_NULL_PTR(cols)) *cols = span_x;
  if(!IS_NULL_PTR(rows)) *rows = span_y;
  return TRUE;
}

gboolean dt_canvas_page_margin_rect(const dt_canvas_t *canvas, const int col, const int row, dt_canvas_rect_t *rect)
{
  if(IS_NULL_PTR(canvas) || IS_NULL_PTR(rect)) return FALSE;
  *rect = dt_canvas_page_rect(canvas, col, row);
  if(!(rect->width > 0.0) || !(rect->height > 0.0)) return FALSE;
  const double margin = fmax((double)canvas->page_margin, 0.0);
  const double bind = fmax((double)canvas->bind_gutter, 0.0);
  int across = 0;
  int down = 0;
  int span_x = 1;
  int span_y = 1;
  dt_canvas_page_in_spread(canvas, col, row, &across, &down, &span_x, &span_y);
  // The binding's allowance is owed by the sides that ARE a fold, and by no others: that is
  // the whole difference between it and the page margin, which is uniform all round.
  const double left = margin + (across > 0 ? bind : 0.0);
  const double right = margin + (across < span_x - 1 ? bind : 0.0);
  const double top = margin + (down > 0 ? bind : 0.0);
  const double bottom = margin + (down < span_y - 1 ? bind : 0.0);
  if(left + right >= rect->width || top + bottom >= rect->height) return FALSE;
  rect->x += left;
  rect->y += top;
  rect->width -= left + right;
  rect->height -= top + bottom;
  return TRUE;
}

void dt_canvas_page_at(const dt_canvas_t *canvas, const double x, const double y, int *col, int *row)
{
  if(!IS_NULL_PTR(col)) *col = 0;
  if(!IS_NULL_PTR(row)) *row = 0;
  double width = 0.0;
  double height = 0.0;
  if(!dt_canvas_paper_dimensions(canvas, &width, &height)) return;
  const double gap = _spread_gap(canvas);
  if(!IS_NULL_PTR(col)) *col = _page_index_at(x, width, _spread_span(canvas->spread_cols), gap);
  if(!IS_NULL_PTR(row)) *row = _page_index_at(y, height, _spread_span(canvas->spread_rows), gap);
}

gboolean dt_canvas_page_guide_rect(const dt_canvas_t *canvas, const int col, const int row, const double outset,
                                   dt_canvas_rect_t *rect)
{
  if(IS_NULL_PTR(rect)) return FALSE;
  *rect = dt_canvas_page_rect(canvas, col, row);
  if(!(rect->width > 0.0) || !(rect->height > 0.0)) return FALSE;
  // A margin that would meet itself, or a bleed on no page at all, is no guide.
  if(2.0 * outset <= -rect->width || 2.0 * outset <= -rect->height) return FALSE;
  rect->x -= outset;
  rect->y -= outset;
  rect->width += 2.0 * outset;
  rect->height += 2.0 * outset;
  return TRUE;
}

/**
 * The vertical lines one page column offers: its own two borders, its margin's -- which the
 * binding's allowance moves on a fold side -- and its SHEET's bleed, since a page in the
 * middle of a spread has no bleed at its folds. At most six.
 */
static int _page_lines_x(const dt_canvas_t *canvas, const int col, const int row, double *lines)
{
  int count = 0;
  const dt_canvas_rect_t page = dt_canvas_page_rect(canvas, col, row);
  if(!(page.width > 0.0)) return 0;
  lines[count++] = page.x;
  lines[count++] = page.x + page.width;
  if(canvas->grid_flags & DT_CANVAS_SNAP_MARGIN)
  {
    dt_canvas_rect_t margin;
    if(dt_canvas_page_margin_rect(canvas, col, row, &margin))
    {
      lines[count++] = margin.x;
      lines[count++] = margin.x + margin.width;
    }
  }
  if((canvas->grid_flags & DT_CANVAS_SNAP_BLEED) && canvas->page_bleed > 0.0f)
  {
    dt_canvas_rect_t sheet;
    if(dt_canvas_spread_rect(canvas, col, row, &sheet, NULL, NULL))
    {
      lines[count++] = sheet.x - canvas->page_bleed;
      lines[count++] = sheet.x + sheet.width + canvas->page_bleed;
    }
  }
  return count;
}

/** The same, horizontally. */
static int _page_lines_y(const dt_canvas_t *canvas, const int col, const int row, double *lines)
{
  int count = 0;
  const dt_canvas_rect_t page = dt_canvas_page_rect(canvas, col, row);
  if(!(page.height > 0.0)) return 0;
  lines[count++] = page.y;
  lines[count++] = page.y + page.height;
  if(canvas->grid_flags & DT_CANVAS_SNAP_MARGIN)
  {
    dt_canvas_rect_t margin;
    if(dt_canvas_page_margin_rect(canvas, col, row, &margin))
    {
      lines[count++] = margin.y;
      lines[count++] = margin.y + margin.height;
    }
  }
  if((canvas->grid_flags & DT_CANVAS_SNAP_BLEED) && canvas->page_bleed > 0.0f)
  {
    dt_canvas_rect_t sheet;
    if(dt_canvas_spread_rect(canvas, col, row, &sheet, NULL, NULL))
    {
      lines[count++] = sheet.y - canvas->page_bleed;
      lines[count++] = sheet.y + sheet.height + canvas->page_bleed;
    }
  }
  return count;
}

gboolean dt_canvas_snap_to_pages(const dt_canvas_t *canvas, const dt_canvas_rect_t *moving, double threshold,
                                 uint32_t edges, double *delta_x, double *delta_y)
{
  if(!IS_NULL_PTR(delta_x)) *delta_x = 0.0;
  if(!IS_NULL_PTR(delta_y)) *delta_y = 0.0;
  double page_width = 0.0;
  double page_height = 0.0;
  if(IS_NULL_PTR(canvas) || IS_NULL_PTR(moving) || !dt_canvas_paper_dimensions(canvas, &page_width, &page_height))
    return FALSE;
  gboolean found_x = FALSE;
  gboolean found_y = FALSE;
  double best_x = 0.0;
  double best_y = 0.0;
  const double xs[2] = { moving->x, moving->x + moving->width };
  const uint32_t x_edges[2] = { DT_CANVAS_EDGE_LEFT, DT_CANVAS_EDGE_RIGHT };
  const double ys[2] = { moving->y, moving->y + moving->height };
  const uint32_t y_edges[2] = { DT_CANVAS_EDGE_TOP, DT_CANVAS_EDGE_BOTTOM };
  // The plane no longer tiles evenly -- it opens by two bleeds between spreads, and a fold
  // side's margin sits further in than the others -- so the lines are asked of the pages
  // around each edge rather than computed from a period. The neighbours either side are
  // included because an edge lying in the gap between two sheets belongs to neither.
  for(int idx = 0; idx < 2; idx++)
  {
    int col = 0;
    int row = 0;
    dt_canvas_page_at(canvas, xs[idx], ys[idx], &col, &row);
    for(int step = -1; step <= 1; step++)
    {
      double lines[6];
      if(edges & x_edges[idx])
      {
        const int count = _page_lines_x(canvas, col + step, row, lines);
        for(int line = 0; line < count; line++) _snap_axis(xs[idx], lines[line], threshold, &best_x, &found_x);
      }
      if(edges & y_edges[idx])
      {
        const int count = _page_lines_y(canvas, col, row + step, lines);
        for(int line = 0; line < count; line++) _snap_axis(ys[idx], lines[line], threshold, &best_y, &found_y);
      }
    }
  }
  if(!IS_NULL_PTR(delta_x)) *delta_x = found_x ? best_x : 0.0;
  if(!IS_NULL_PTR(delta_y)) *delta_y = found_y ? best_y : 0.0;
  return found_x || found_y;
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
      // A whole-canvas arrangement gathers the PICTURES and what goes with them. A shape is
      // decoration -- a rule under a title, a panel behind a caption -- placed where it is
      // against something else, so sweeping it into the grid with the photographs would move it
      // away from the thing it was drawn for. A shape named in a selection is still arranged.
      if(object->kind == DT_CANVAS_OBJECT_SHAPE) continue;
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

/** Round a size UP to the grid when snapping is on, so a cell holds its frame and stays on the grid. */
static double _layout_ceil(const dt_canvas_t *canvas, const double value)
{
  if(!(canvas->grid_flags & DT_CANVAS_GRID_SNAP) || canvas->grid_size <= 0.0f) return value;
  return ceil(value / canvas->grid_size - 1e-9) * canvas->grid_size;
}

/** Frames sit top-left in their cell: with the cell on the grid, so is the frame's corner. */
static void _layout_place(dt_canvas_object_t *frame, const double cell_x, const double cell_y)
{
  frame->rotation = 0.0;
  frame->x = cell_x + frame->width * 0.5;
  frame->y = cell_y + frame->height * 0.5;
}

static void _layout_grid(const dt_canvas_t *canvas, GPtrArray *frames, const double anchor_x, const double anchor_y,
                         const double gap)
{
  double cell_width = 0.0;
  double cell_height = 0.0;
  for(guint idx = 0; idx < frames->len; idx++)
  {
    const dt_canvas_object_t *frame = g_ptr_array_index(frames, idx);
    cell_width = fmax(cell_width, frame->width);
    cell_height = fmax(cell_height, frame->height);
  }
  cell_width = _layout_ceil(canvas, cell_width);
  cell_height = _layout_ceil(canvas, cell_height);
  const guint columns = (guint)ceil(sqrt((double)frames->len));
  for(guint idx = 0; idx < frames->len; idx++)
  {
    dt_canvas_object_t *frame = g_ptr_array_index(frames, idx);
    const guint col = idx % columns;
    const guint row = idx / columns;
    _layout_place(frame, anchor_x + col * (cell_width + gap), anchor_y + row * (cell_height + gap));
  }
}

static void _layout_masonry(const dt_canvas_t *canvas, GPtrArray *frames, const double anchor_x,
                            const double anchor_y, const double gap, const int column_count)
{
  const int columns = column_count < 1 ? 1 : column_count;
  double column_width = 0.0;
  for(guint idx = 0; idx < frames->len; idx++)
  {
    const dt_canvas_object_t *frame = g_ptr_array_index(frames, idx);
    column_width = fmax(column_width, frame->width);
  }
  column_width = _layout_ceil(canvas, column_width);
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
    frame->width = column_width;
    frame->height = column_width / ratio;
    _layout_place(frame, anchor_x + shortest * (column_width + gap), anchor_y + column_heights[shortest]);
    // The next frame in this column starts on the grid, whatever height this one scaled to.
    column_heights[shortest] = _layout_ceil(canvas, column_heights[shortest] + frame->height) + gap;
  }
  dt_free(column_heights);
}

static void _layout_row(const dt_canvas_t *canvas, GPtrArray *frames, const double anchor_x, const double anchor_y,
                        const double gap)
{
  double row_height = 0.0;
  for(guint idx = 0; idx < frames->len; idx++)
  {
    const dt_canvas_object_t *frame = g_ptr_array_index(frames, idx);
    row_height = fmax(row_height, frame->height);
  }
  row_height = _layout_ceil(canvas, row_height);
  double cursor_x = anchor_x;
  for(guint idx = 0; idx < frames->len; idx++)
  {
    dt_canvas_object_t *frame = g_ptr_array_index(frames, idx);
    const double ratio = frame->height > 0.0 ? frame->width / frame->height : 1.0;
    frame->height = row_height;
    frame->width = row_height * ratio;
    _layout_place(frame, cursor_x, anchor_y);
    cursor_x = _layout_ceil(canvas, cursor_x + frame->width) + gap;
  }
}

static void _layout_column(const dt_canvas_t *canvas, GPtrArray *frames, const double anchor_x,
                           const double anchor_y, const double gap)
{
  double column_width = 0.0;
  for(guint idx = 0; idx < frames->len; idx++)
  {
    const dt_canvas_object_t *frame = g_ptr_array_index(frames, idx);
    column_width = fmax(column_width, frame->width);
  }
  column_width = _layout_ceil(canvas, column_width);
  double cursor_y = anchor_y;
  for(guint idx = 0; idx < frames->len; idx++)
  {
    dt_canvas_object_t *frame = g_ptr_array_index(frames, idx);
    const double ratio = frame->height > 0.0 ? frame->width / frame->height : 1.0;
    frame->width = column_width;
    frame->height = column_width / ratio;
    _layout_place(frame, anchor_x, cursor_y);
    cursor_y = _layout_ceil(canvas, cursor_y + frame->height) + gap;
  }
}

/* Sorting frames for a layout. Images compare on the chosen key, then on their draw order so
 * the sort is stable; a frame that is not an image comes after every image, in draw order. */
static gint _layout_compare(gconstpointer first, gconstpointer second, gpointer user_data)
{
  const dt_canvas_object_t *object_a = *(const dt_canvas_object_t *const *)first;
  const dt_canvas_object_t *object_b = *(const dt_canvas_object_t *const *)second;
  const dt_canvas_sort_t sort = (dt_canvas_sort_t)GPOINTER_TO_INT(user_data);
  const gboolean image_a = object_a->kind == DT_CANVAS_OBJECT_IMAGE;
  const gboolean image_b = object_b->kind == DT_CANVAS_OBJECT_IMAGE;
  if(image_a != image_b) return image_a ? -1 : 1;
  int order = 0;
  if(image_a && image_b)
  {
    switch(sort)
    {
      case DT_CANVAS_SORT_FILENAME:
        order = g_utf8_collate(object_a->image.filename, object_b->image.filename);
        break;
      case DT_CANVAS_SORT_DATETIME:
        order = object_a->image.exif_datetime_taken < object_b->image.exif_datetime_taken
                    ? -1
                    : (object_a->image.exif_datetime_taken > object_b->image.exif_datetime_taken ? 1 : 0);
        break;
      case DT_CANVAS_SORT_ID:
        order = object_a->image.imgid < object_b->image.imgid ? -1 : (object_a->image.imgid > object_b->image.imgid ? 1 : 0);
        break;
      case DT_CANVAS_SORT_PATH:
        order = g_utf8_collate(object_a->image.folder, object_b->image.folder);
        if(order == 0) order = g_utf8_collate(object_a->image.filename, object_b->image.filename);
        break;
      default:
        break;
    }
  }
  if(order != 0) return order;
  return _object_compare_z(first, second);
}

void dt_canvas_layout_apply(dt_canvas_t *canvas, const GArray *ids, dt_canvas_layout_t layout, int columns,
                            dt_canvas_sort_t sort)
{
  if(IS_NULL_PTR(canvas)) return;
  GPtrArray *frames = _layout_frames(canvas, ids);
  if(frames->len == 0)
  {
    g_ptr_array_free(frames, TRUE);
    return;
  }
  if(sort != DT_CANVAS_SORT_CANVAS) g_ptr_array_sort_with_data(frames, _layout_compare, GINT_TO_POINTER(sort));
  double anchor_x = 0.0;
  double anchor_y = 0.0;
  _layout_anchor(frames, &anchor_x, &anchor_y);
  // Every frame keeps a padding of clear space around itself, so two of them side by side are
  // two paddings apart -- the same arithmetic the snapping uses, or an arranged layout would
  // not be one the snapping can reproduce by hand.
  const double gap = canvas->padding > 0.0f ? 2.0 * canvas->padding : 0.0;
  anchor_x = dt_canvas_snap(canvas, anchor_x);
  anchor_y = dt_canvas_snap(canvas, anchor_y);
  switch(layout)
  {
    case DT_CANVAS_LAYOUT_MASONRY:
      _layout_masonry(canvas, frames, anchor_x, anchor_y, gap, columns);
      break;
    case DT_CANVAS_LAYOUT_ROW:
      _layout_row(canvas, frames, anchor_x, anchor_y, gap);
      break;
    case DT_CANVAS_LAYOUT_COLUMN:
      _layout_column(canvas, frames, anchor_x, anchor_y, gap);
      break;
    case DT_CANVAS_LAYOUT_GRID:
    default:
      _layout_grid(canvas, frames, anchor_x, anchor_y, gap);
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
