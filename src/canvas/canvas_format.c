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

#include "canvas/canvas_format.h"

#include "system/macros.h"
#include "system/mem_alloc.h"

#include <string.h>

#define CANVAS_MAGIC "ANSELCNV"
#define CANVAS_MAGIC_LEN 8

G_DEFINE_QUARK(dt - canvas - error - quark, dt_canvas_error)

/* --- writer ----------------------------------------------------------------- */

static void _w_u32(GByteArray *out, const uint32_t value)
{
  const uint8_t bytes[4] = { (uint8_t)(value & 0xFF), (uint8_t)((value >> 8) & 0xFF), (uint8_t)((value >> 16) & 0xFF),
                             (uint8_t)((value >> 24) & 0xFF) };
  g_byte_array_append(out, bytes, 4);
}

static void _w_i32(GByteArray *out, const int32_t value)
{
  _w_u32(out, (uint32_t)value);
}

static void _w_u64(GByteArray *out, const uint64_t value)
{
  _w_u32(out, (uint32_t)(value & 0xFFFFFFFFu));
  _w_u32(out, (uint32_t)(value >> 32));
}

static void _w_i64(GByteArray *out, const int64_t value)
{
  _w_u64(out, (uint64_t)value);
}

static void _w_f32(GByteArray *out, const float value)
{
  uint32_t bits = 0;
  memcpy(&bits, &value, sizeof(bits));
  _w_u32(out, bits);
}

static void _w_f64(GByteArray *out, const double value)
{
  uint64_t bits = 0;
  memcpy(&bits, &value, sizeof(bits));
  _w_u64(out, bits);
}

static void _w_string(GByteArray *out, const char *text, const size_t field_len)
{
  uint8_t *field = g_malloc0(field_len);
  if(!IS_NULL_PTR(text)) g_strlcpy((char *)field, text, field_len);
  g_byte_array_append(out, field, field_len);
  dt_free(field);
}

static void _w_bytes(GByteArray *out, const uint8_t *bytes, const size_t len)
{
  g_byte_array_append(out, bytes, len);
}

static void _w_color(GByteArray *out, const dt_canvas_color_t *color)
{
  _w_f32(out, color->red);
  _w_f32(out, color->green);
  _w_f32(out, color->blue);
  _w_f32(out, color->alpha);
}

static void _write_image(GByteArray *out, const dt_canvas_image_t *image)
{
  _w_i32(out, image->imgid);
  _w_i32(out, image->version);
  _w_i32(out, image->film_id);
  _w_u64(out, image->history_hash);
  _w_i64(out, image->rendered_at);
  _w_i32(out, image->pixel_width);
  _w_i32(out, image->pixel_height);
  _w_i32(out, image->source_width);
  _w_i32(out, image->source_height);
  _w_i32(out, image->orientation);
  _w_string(out, image->folder, sizeof(image->folder));
  _w_string(out, image->filename, sizeof(image->filename));
  _w_string(out, image->exif_maker, sizeof(image->exif_maker));
  _w_string(out, image->exif_model, sizeof(image->exif_model));
  _w_string(out, image->exif_lens, sizeof(image->exif_lens));
  _w_f32(out, image->exif_exposure);
  _w_f32(out, image->exif_aperture);
  _w_f32(out, image->exif_iso);
  _w_f32(out, image->exif_focal_length);
  _w_f32(out, image->exif_exposure_bias);
  _w_i64(out, image->exif_datetime_taken);
  _w_bytes(out, image->reserved, sizeof(image->reserved));
}

static void _write_text(GByteArray *out, const dt_canvas_text_t *text)
{
  _w_string(out, text->font, sizeof(text->font));
  _w_color(out, &text->text_color);
  _w_color(out, &text->background);
  _w_u32(out, text->source);
  _w_u32(out, text->linked_object);
  _w_f32(out, text->padding);
  _w_bytes(out, text->reserved, sizeof(text->reserved));
}

static void _write_connector(GByteArray *out, const dt_canvas_connector_t *connector)
{
  _w_u32(out, connector->from_id);
  _w_u32(out, connector->to_id);
  _w_u32(out, connector->style);
  _w_color(out, &connector->color);
  _w_f32(out, connector->line_width);
  _w_u32(out, connector->from_anchor);
  _w_u32(out, connector->to_anchor);
  _w_u32(out, connector->routing);
  _w_bytes(out, connector->reserved, sizeof(connector->reserved));
}

static void _write_object(GByteArray *out, const dt_canvas_object_t *object)
{
  const guint start = out->len;
  _w_u32(out, object->kind);
  _w_u32(out, 0); // record_size, patched below
  _w_u32(out, object->id);
  _w_f64(out, object->x);
  _w_f64(out, object->y);
  _w_f64(out, object->width);
  _w_f64(out, object->height);
  _w_f64(out, object->rotation);
  _w_i32(out, object->z);
  _w_u32(out, object->flags);
  _w_color(out, &object->border_color);
  _w_f32(out, object->border_width);
  _w_bytes(out, object->reserved, sizeof(object->reserved));
  switch(object->kind)
  {
    case DT_CANVAS_OBJECT_IMAGE:
      _write_image(out, &object->image);
      break;
    case DT_CANVAS_OBJECT_TEXT:
      _write_text(out, &object->text);
      break;
    case DT_CANVAS_OBJECT_CONNECTOR:
      _write_connector(out, &object->connector);
      break;
    default:
      break;
  }
  const uint32_t record_size = out->len - start;
  uint8_t *size_field = out->data + start + 4;
  size_field[0] = (uint8_t)(record_size & 0xFF);
  size_field[1] = (uint8_t)((record_size >> 8) & 0xFF);
  size_field[2] = (uint8_t)((record_size >> 16) & 0xFF);
  size_field[3] = (uint8_t)((record_size >> 24) & 0xFF);
}

GBytes *dt_canvas_format_write_index(const dt_canvas_t *canvas)
{
  if(IS_NULL_PTR(canvas)) return NULL;
  GByteArray *out = g_byte_array_new();

  _w_bytes(out, (const uint8_t *)CANVAS_MAGIC, CANVAS_MAGIC_LEN);
  _w_u32(out, DT_CANVAS_FORMAT_VERSION);
  _w_u32(out, 0); // header_size, patched below
  _w_u32(out, dt_canvas_object_count(canvas));
  _w_string(out, canvas->title, sizeof(canvas->title));
  _w_color(out, &canvas->background);
  _w_color(out, &canvas->border_color);
  _w_f32(out, canvas->border_width);
  _w_f32(out, canvas->grid_size);
  _w_u32(out, canvas->grid_flags);
  _w_f64(out, canvas->view_zoom);
  _w_f64(out, canvas->view_x);
  _w_f64(out, canvas->view_y);
  _w_string(out, canvas->default_font, sizeof(canvas->default_font));
  _w_i32(out, canvas->image_long_edge);
  _w_i32(out, canvas->jpeg_quality);
  _w_u32(out, canvas->next_id);
  _w_bytes(out, canvas->reserved, sizeof(canvas->reserved));
  const uint32_t header_size = out->len;
  uint8_t *size_field = out->data + CANVAS_MAGIC_LEN + 4;
  size_field[0] = (uint8_t)(header_size & 0xFF);
  size_field[1] = (uint8_t)((header_size >> 8) & 0xFF);
  size_field[2] = (uint8_t)((header_size >> 16) & 0xFF);
  size_field[3] = (uint8_t)((header_size >> 24) & 0xFF);

  for(guint idx = 0; idx < dt_canvas_object_count(canvas); idx++)
  {
    _write_object(out, dt_canvas_object_at(canvas, idx));
  }
  return g_byte_array_free_to_bytes(out);
}

/* --- reader ----------------------------------------------------------------- */

typedef struct dt_canvas_cursor_t
{
  const uint8_t *data;
  size_t size;
  size_t pos;
  size_t limit;    ///< end of the current record; reads past it yield zeros
  gboolean overrun; ///< a read went past `size`, which is corruption
} dt_canvas_cursor_t;

static gboolean _r_take(dt_canvas_cursor_t *cursor, uint8_t *out, const size_t len)
{
  // Inside the record's declared size but past what this version wrote: unknown
  // future fields, read as zeros. Past the buffer itself: corrupt.
  if(cursor->pos + len > cursor->size)
  {
    cursor->overrun = TRUE;
    memset(out, 0, len);
    return FALSE;
  }
  if(cursor->pos + len > cursor->limit)
  {
    memset(out, 0, len);
    cursor->pos = cursor->limit;
    return FALSE;
  }
  memcpy(out, cursor->data + cursor->pos, len);
  cursor->pos += len;
  return TRUE;
}

static uint32_t _r_u32(dt_canvas_cursor_t *cursor)
{
  uint8_t bytes[4] = { 0 };
  _r_take(cursor, bytes, 4);
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static int32_t _r_i32(dt_canvas_cursor_t *cursor)
{
  return (int32_t)_r_u32(cursor);
}

static uint64_t _r_u64(dt_canvas_cursor_t *cursor)
{
  const uint64_t low = _r_u32(cursor);
  const uint64_t high = _r_u32(cursor);
  return low | (high << 32);
}

static int64_t _r_i64(dt_canvas_cursor_t *cursor)
{
  return (int64_t)_r_u64(cursor);
}

static float _r_f32(dt_canvas_cursor_t *cursor)
{
  const uint32_t bits = _r_u32(cursor);
  float value = 0.0f;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

static double _r_f64(dt_canvas_cursor_t *cursor)
{
  const uint64_t bits = _r_u64(cursor);
  double value = 0.0;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

static void _r_string(dt_canvas_cursor_t *cursor, char *field, const size_t field_len)
{
  _r_take(cursor, (uint8_t *)field, field_len);
  field[field_len - 1] = '\0';
}

static void _r_bytes(dt_canvas_cursor_t *cursor, uint8_t *bytes, const size_t len)
{
  _r_take(cursor, bytes, len);
}

static dt_canvas_color_t _r_color(dt_canvas_cursor_t *cursor)
{
  dt_canvas_color_t color;
  color.red = _r_f32(cursor);
  color.green = _r_f32(cursor);
  color.blue = _r_f32(cursor);
  color.alpha = _r_f32(cursor);
  return color;
}

static void _read_image(dt_canvas_cursor_t *cursor, dt_canvas_image_t *image)
{
  image->imgid = _r_i32(cursor);
  image->version = _r_i32(cursor);
  image->film_id = _r_i32(cursor);
  image->history_hash = _r_u64(cursor);
  image->rendered_at = _r_i64(cursor);
  image->pixel_width = _r_i32(cursor);
  image->pixel_height = _r_i32(cursor);
  image->source_width = _r_i32(cursor);
  image->source_height = _r_i32(cursor);
  image->orientation = _r_i32(cursor);
  _r_string(cursor, image->folder, sizeof(image->folder));
  _r_string(cursor, image->filename, sizeof(image->filename));
  _r_string(cursor, image->exif_maker, sizeof(image->exif_maker));
  _r_string(cursor, image->exif_model, sizeof(image->exif_model));
  _r_string(cursor, image->exif_lens, sizeof(image->exif_lens));
  image->exif_exposure = _r_f32(cursor);
  image->exif_aperture = _r_f32(cursor);
  image->exif_iso = _r_f32(cursor);
  image->exif_focal_length = _r_f32(cursor);
  image->exif_exposure_bias = _r_f32(cursor);
  image->exif_datetime_taken = _r_i64(cursor);
  _r_bytes(cursor, image->reserved, sizeof(image->reserved));
  image->jpeg = NULL;
  image->sync_status = DT_CANVAS_SYNC_UNKNOWN;
}

static void _read_text(dt_canvas_cursor_t *cursor, dt_canvas_text_t *text)
{
  _r_string(cursor, text->font, sizeof(text->font));
  text->text_color = _r_color(cursor);
  text->background = _r_color(cursor);
  text->source = _r_u32(cursor);
  text->linked_object = _r_u32(cursor);
  text->padding = _r_f32(cursor);
  _r_bytes(cursor, text->reserved, sizeof(text->reserved));
  text->markdown = NULL;
}

static void _read_connector(dt_canvas_cursor_t *cursor, dt_canvas_connector_t *connector)
{
  connector->from_id = _r_u32(cursor);
  connector->to_id = _r_u32(cursor);
  connector->style = _r_u32(cursor);
  connector->color = _r_color(cursor);
  connector->line_width = _r_f32(cursor);
  connector->from_anchor = _r_u32(cursor);
  connector->to_anchor = _r_u32(cursor);
  connector->routing = _r_u32(cursor);
  _r_bytes(cursor, connector->reserved, sizeof(connector->reserved));
}

static gboolean _read_object(dt_canvas_cursor_t *cursor, dt_canvas_object_t *object)
{
  const size_t record_start = cursor->pos;
  cursor->limit = cursor->size;
  object->kind = _r_u32(cursor);
  const uint32_t record_size = _r_u32(cursor);
  if(cursor->overrun) return FALSE;
  if(record_size < 8 || record_start + record_size > cursor->size) return FALSE;
  cursor->limit = record_start + record_size;

  object->id = _r_u32(cursor);
  object->x = _r_f64(cursor);
  object->y = _r_f64(cursor);
  object->width = _r_f64(cursor);
  object->height = _r_f64(cursor);
  object->rotation = _r_f64(cursor);
  object->z = _r_i32(cursor);
  object->flags = _r_u32(cursor);
  object->border_color = _r_color(cursor);
  object->border_width = _r_f32(cursor);
  _r_bytes(cursor, object->reserved, sizeof(object->reserved));
  switch(object->kind)
  {
    case DT_CANVAS_OBJECT_IMAGE:
      _read_image(cursor, &object->image);
      break;
    case DT_CANVAS_OBJECT_TEXT:
      _read_text(cursor, &object->text);
      break;
    case DT_CANVAS_OBJECT_CONNECTOR:
      _read_connector(cursor, &object->connector);
      break;
    default:
      // A kind this version does not know: keep its place in the file, draw nothing.
      break;
  }
  // Whatever this version did not read of the record is a later version's fields: skip them.
  cursor->pos = record_start + record_size;
  cursor->limit = cursor->size;
  return !cursor->overrun;
}

gboolean dt_canvas_format_read_index(dt_canvas_t *canvas, GBytes *index, GError **error)
{
  if(IS_NULL_PTR(canvas) || IS_NULL_PTR(index))
  {
    g_set_error(error, DT_CANVAS_ERROR, DT_CANVAS_ERROR_NOT_A_CANVAS, "no index");
    return FALSE;
  }
  dt_canvas_cursor_t cursor;
  cursor.data = g_bytes_get_data(index, &cursor.size);
  cursor.pos = 0;
  cursor.limit = cursor.size;
  cursor.overrun = FALSE;

  uint8_t magic[CANVAS_MAGIC_LEN];
  _r_bytes(&cursor, magic, sizeof(magic));
  if(cursor.overrun || memcmp(magic, CANVAS_MAGIC, CANVAS_MAGIC_LEN) != 0)
  {
    g_set_error(error, DT_CANVAS_ERROR, DT_CANVAS_ERROR_NOT_A_CANVAS, "not a canvas index");
    return FALSE;
  }
  const uint32_t version = _r_u32(&cursor);
  if(version > DT_CANVAS_FORMAT_VERSION)
  {
    g_set_error(error, DT_CANVAS_ERROR, DT_CANVAS_ERROR_VERSION, "canvas format %u is newer than this build (%u)",
                version, DT_CANVAS_FORMAT_VERSION);
    return FALSE;
  }
  const uint32_t header_size = _r_u32(&cursor);
  if(header_size < 16 || header_size > cursor.size)
  {
    g_set_error(error, DT_CANVAS_ERROR, DT_CANVAS_ERROR_CORRUPT, "bad header size");
    return FALSE;
  }
  cursor.limit = header_size;
  const uint32_t object_count = _r_u32(&cursor);
  canvas->format_version = version;
  _r_string(&cursor, canvas->title, sizeof(canvas->title));
  canvas->background = _r_color(&cursor);
  canvas->border_color = _r_color(&cursor);
  canvas->border_width = _r_f32(&cursor);
  canvas->grid_size = _r_f32(&cursor);
  canvas->grid_flags = _r_u32(&cursor);
  canvas->view_zoom = _r_f64(&cursor);
  canvas->view_x = _r_f64(&cursor);
  canvas->view_y = _r_f64(&cursor);
  _r_string(&cursor, canvas->default_font, sizeof(canvas->default_font));
  canvas->image_long_edge = _r_i32(&cursor);
  canvas->jpeg_quality = _r_i32(&cursor);
  canvas->next_id = _r_u32(&cursor);
  _r_bytes(&cursor, canvas->reserved, sizeof(canvas->reserved));
  cursor.pos = header_size;
  cursor.limit = cursor.size;

  uint32_t highest_id = 0;
  for(uint32_t idx = 0; idx < object_count; idx++)
  {
    dt_canvas_object_t *object = g_new0(dt_canvas_object_t, 1);
    if(!_read_object(&cursor, object))
    {
      dt_free(object);
      g_set_error(error, DT_CANVAS_ERROR, DT_CANVAS_ERROR_CORRUPT, "object record %u does not parse", idx);
      return FALSE;
    }
    if(object->kind == DT_CANVAS_OBJECT_NONE || dt_canvas_find_object(canvas, object->id) != NULL)
    {
      // Unknown kind or a duplicated id: drop it rather than draw garbage.
      dt_free(object);
      continue;
    }
    if(object->id > highest_id) highest_id = object->id;
    g_ptr_array_add(canvas->objects, object);
  }
  if(canvas->next_id <= highest_id) canvas->next_id = highest_id + 1;
  if(canvas->next_id == 0) canvas->next_id = 1;
  if(canvas->view_zoom <= 0.0) canvas->view_zoom = 1.0;
  return TRUE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
