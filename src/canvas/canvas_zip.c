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

#include "canvas/canvas_zip.h"

#include "system/macros.h"
#include "system/mem_alloc.h"

#include <glib/gstdio.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

#define ZIP_SIGNATURE_LOCAL 0x04034b50u
#define ZIP_SIGNATURE_CENTRAL 0x02014b50u
#define ZIP_SIGNATURE_END 0x06054b50u
#define ZIP_VERSION_NEEDED 20u
#define ZIP_FLAG_UTF8 0x0800u
#define ZIP_METHOD_STORE 0u
#define ZIP_METHOD_DEFLATE 8u
#define ZIP_MAX_ENTRY_SIZE 0xFFFFFFFFu
#define ZIP_END_RECORD_SIZE 22u
#define ZIP_MAX_COMMENT_SIZE 65535u
#define ZIP_LOCAL_HEADER_SIZE 30u
#define ZIP_CENTRAL_HEADER_SIZE 46u
#define ZIP_INFLATE_CHUNK (256u * 1024u)

typedef struct dt_zip_entry_t
{
  char *name;
  uint32_t crc;
  uint32_t compressed_size;
  uint32_t uncompressed_size;
  uint16_t method;
  uint16_t dos_time;
  uint16_t dos_date;
  uint32_t local_offset;
} dt_zip_entry_t;

struct dt_canvas_zip_writer_t
{
  FILE *file;
  char *path;
  char *temp_path;
  GPtrArray *entries;
  uint64_t offset;
  gboolean failed;
};

struct dt_canvas_zip_reader_t
{
  FILE *file;
  GPtrArray *entries;
  GHashTable *by_name;
};

/* --- byte helpers ---------------------------------------------------------- */

static void _put_u16(uint8_t *out, const uint16_t value)
{
  out[0] = (uint8_t)(value & 0xFF);
  out[1] = (uint8_t)((value >> 8) & 0xFF);
}

static void _put_u32(uint8_t *out, const uint32_t value)
{
  out[0] = (uint8_t)(value & 0xFF);
  out[1] = (uint8_t)((value >> 8) & 0xFF);
  out[2] = (uint8_t)((value >> 16) & 0xFF);
  out[3] = (uint8_t)((value >> 24) & 0xFF);
}

static uint16_t _get_u16(const uint8_t *in)
{
  return (uint16_t)(in[0] | (in[1] << 8));
}

static uint32_t _get_u32(const uint8_t *in)
{
  return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

static void _entry_free(gpointer data)
{
  dt_zip_entry_t *entry = (dt_zip_entry_t *)data;
  if(IS_NULL_PTR(entry)) return;
  dt_free(entry->name);
  dt_free(entry);
}

static void _dos_datetime_now(uint16_t *dos_time, uint16_t *dos_date)
{
  const time_t now = time(NULL);
  struct tm local_time;
#ifdef _WIN32
  localtime_s(&local_time, &now);
#else
  localtime_r(&now, &local_time);
#endif
  const int year = local_time.tm_year + 1900;
  const int clamped_year = year < 1980 ? 1980 : year;
  *dos_time = (uint16_t)(((local_time.tm_hour & 0x1F) << 11) | ((local_time.tm_min & 0x3F) << 5)
                         | ((local_time.tm_sec / 2) & 0x1F));
  *dos_date = (uint16_t)((((clamped_year - 1980) & 0x7F) << 9) | (((local_time.tm_mon + 1) & 0x0F) << 5)
                         | (local_time.tm_mday & 0x1F));
}

static gboolean _write_all(FILE *file, const void *data, const size_t size)
{
  if(size == 0) return TRUE;
  return fwrite(data, 1, size, file) == size;
}

/* --- deflate ---------------------------------------------------------------- */

static uint8_t *_deflate_buffer(const uint8_t *data, const size_t size, size_t *out_size)
{
  *out_size = 0;
  z_stream stream;
  memset(&stream, 0, sizeof(stream));
  // Negative window bits: raw deflate, which is what ZIP entries carry.
  if(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) return NULL;

  const uLong bound = deflateBound(&stream, (uLong)size);
  uint8_t *out = g_try_malloc(bound);
  if(IS_NULL_PTR(out))
  {
    deflateEnd(&stream);
    return NULL;
  }

  stream.next_in = (Bytef *)data;
  stream.avail_in = (uInt)size;
  stream.next_out = out;
  stream.avail_out = (uInt)bound;
  const int status = deflate(&stream, Z_FINISH);
  if(status != Z_STREAM_END)
  {
    deflateEnd(&stream);
    dt_free(out);
    return NULL;
  }
  *out_size = stream.total_out;
  deflateEnd(&stream);
  return out;
}

static uint8_t *_inflate_buffer(const uint8_t *data, const size_t compressed_size, const size_t expected_size)
{
  uint8_t *out = g_try_malloc(expected_size == 0 ? 1 : expected_size);
  if(IS_NULL_PTR(out)) return NULL;

  z_stream stream;
  memset(&stream, 0, sizeof(stream));
  if(inflateInit2(&stream, -15) != Z_OK)
  {
    dt_free(out);
    return NULL;
  }
  stream.next_in = (Bytef *)data;
  stream.avail_in = (uInt)compressed_size;
  stream.next_out = out;
  stream.avail_out = (uInt)expected_size;
  const int status = inflate(&stream, Z_FINISH);
  const gboolean complete = (status == Z_STREAM_END) && (stream.total_out == expected_size);
  inflateEnd(&stream);
  if(!complete)
  {
    dt_free(out);
    return NULL;
  }
  return out;
}

/* --- writer ----------------------------------------------------------------- */

dt_canvas_zip_writer_t *dt_canvas_zip_writer_open(const char *path)
{
  if(IS_NULL_PTR(path) || path[0] == '\0') return NULL;

  dt_canvas_zip_writer_t *writer = g_new0(dt_canvas_zip_writer_t, 1);
  writer->path = g_strdup(path);
  writer->temp_path = g_strdup_printf("%s.part", path);
  writer->file = g_fopen(writer->temp_path, "wb");
  if(IS_NULL_PTR(writer->file))
  {
    dt_free(writer->temp_path);
    dt_free(writer->path);
    dt_free(writer);
    return NULL;
  }
  writer->entries = g_ptr_array_new_with_free_func(_entry_free);
  writer->offset = 0;
  writer->failed = FALSE;
  return writer;
}

gboolean dt_canvas_zip_writer_add(dt_canvas_zip_writer_t *writer, const char *name, const void *data, size_t size,
                                  gboolean compress)
{
  if(IS_NULL_PTR(writer) || writer->failed) return FALSE;
  if(IS_NULL_PTR(name) || name[0] == '\0' || name[0] == '/') return FALSE;
  if(size > ZIP_MAX_ENTRY_SIZE) return FALSE;
  const size_t name_length = strlen(name);
  if(name_length > 0xFFFF) return FALSE;
  if(writer->offset > ZIP_MAX_ENTRY_SIZE) return FALSE;

  const uint8_t *payload = (const uint8_t *)data;
  size_t payload_size = size;
  uint8_t *deflated = NULL;
  uint16_t method = ZIP_METHOD_STORE;
  if(compress && size > 0)
  {
    size_t deflated_size = 0;
    deflated = _deflate_buffer(payload, size, &deflated_size);
    // Deflate only pays when it shrinks; a JPEG or an already-compressed blob stays stored.
    if(!IS_NULL_PTR(deflated) && deflated_size < size)
    {
      payload = deflated;
      payload_size = deflated_size;
      method = ZIP_METHOD_DEFLATE;
    }
  }

  dt_zip_entry_t *entry = g_new0(dt_zip_entry_t, 1);
  entry->name = g_strdup(name);
  entry->crc = size > 0 ? (uint32_t)crc32(crc32(0L, Z_NULL, 0), (const Bytef *)data, (uInt)size) : 0;
  entry->compressed_size = (uint32_t)payload_size;
  entry->uncompressed_size = (uint32_t)size;
  entry->method = method;
  entry->local_offset = (uint32_t)writer->offset;
  _dos_datetime_now(&entry->dos_time, &entry->dos_date);

  uint8_t header[ZIP_LOCAL_HEADER_SIZE];
  _put_u32(header + 0, ZIP_SIGNATURE_LOCAL);
  _put_u16(header + 4, ZIP_VERSION_NEEDED);
  _put_u16(header + 6, ZIP_FLAG_UTF8);
  _put_u16(header + 8, entry->method);
  _put_u16(header + 10, entry->dos_time);
  _put_u16(header + 12, entry->dos_date);
  _put_u32(header + 14, entry->crc);
  _put_u32(header + 18, entry->compressed_size);
  _put_u32(header + 22, entry->uncompressed_size);
  _put_u16(header + 26, (uint16_t)name_length);
  _put_u16(header + 28, 0);

  gboolean ok = _write_all(writer->file, header, sizeof(header));
  if(ok) ok = _write_all(writer->file, name, name_length);
  if(ok) ok = _write_all(writer->file, payload, payload_size);
  dt_free(deflated);

  if(!ok)
  {
    _entry_free(entry);
    writer->failed = TRUE;
    return FALSE;
  }

  writer->offset += sizeof(header) + name_length + payload_size;
  g_ptr_array_add(writer->entries, entry);
  return TRUE;
}

static gboolean _writer_write_central_directory(dt_canvas_zip_writer_t *writer)
{
  const uint64_t central_offset = writer->offset;
  uint64_t central_size = 0;
  for(guint idx = 0; idx < writer->entries->len; idx++)
  {
    const dt_zip_entry_t *entry = g_ptr_array_index(writer->entries, idx);
    const size_t name_length = strlen(entry->name);
    uint8_t header[ZIP_CENTRAL_HEADER_SIZE];
    _put_u32(header + 0, ZIP_SIGNATURE_CENTRAL);
    _put_u16(header + 4, ZIP_VERSION_NEEDED);
    _put_u16(header + 6, ZIP_VERSION_NEEDED);
    _put_u16(header + 8, ZIP_FLAG_UTF8);
    _put_u16(header + 10, entry->method);
    _put_u16(header + 12, entry->dos_time);
    _put_u16(header + 14, entry->dos_date);
    _put_u32(header + 16, entry->crc);
    _put_u32(header + 20, entry->compressed_size);
    _put_u32(header + 24, entry->uncompressed_size);
    _put_u16(header + 28, (uint16_t)name_length);
    _put_u16(header + 30, 0);
    _put_u16(header + 32, 0);
    _put_u16(header + 34, 0);
    _put_u16(header + 36, 0);
    _put_u32(header + 38, 0);
    _put_u32(header + 42, entry->local_offset);
    if(!_write_all(writer->file, header, sizeof(header))) return FALSE;
    if(!_write_all(writer->file, entry->name, name_length)) return FALSE;
    central_size += sizeof(header) + name_length;
  }
  if(central_offset + central_size > ZIP_MAX_ENTRY_SIZE) return FALSE;
  if(writer->entries->len > 0xFFFF) return FALSE;

  uint8_t end[ZIP_END_RECORD_SIZE];
  _put_u32(end + 0, ZIP_SIGNATURE_END);
  _put_u16(end + 4, 0);
  _put_u16(end + 6, 0);
  _put_u16(end + 8, (uint16_t)writer->entries->len);
  _put_u16(end + 10, (uint16_t)writer->entries->len);
  _put_u32(end + 12, (uint32_t)central_size);
  _put_u32(end + 16, (uint32_t)central_offset);
  _put_u16(end + 20, 0);
  return _write_all(writer->file, end, sizeof(end));
}

gboolean dt_canvas_zip_writer_close(dt_canvas_zip_writer_t *writer, gboolean commit)
{
  if(IS_NULL_PTR(writer)) return FALSE;

  gboolean ok = commit && !writer->failed;
  if(ok) ok = _writer_write_central_directory(writer);
  if(ok) ok = fflush(writer->file) == 0;
  const int close_status = fclose(writer->file);
  writer->file = NULL;
  if(close_status != 0) ok = FALSE;

  if(ok)
  {
    ok = g_rename(writer->temp_path, writer->path) == 0;
  }
  if(!ok)
  {
    g_unlink(writer->temp_path);
  }

  g_ptr_array_free(writer->entries, TRUE);
  dt_free(writer->temp_path);
  dt_free(writer->path);
  dt_free(writer);
  return ok || !commit;
}

/* --- reader ----------------------------------------------------------------- */

// Windows has no fseeko(); its 64-bit seek pair is the underscored one.
static int _file_seek(FILE *file, const int64_t offset, const int whence)
{
#ifdef _WIN32
  return _fseeki64(file, offset, whence);
#else
  return fseeko(file, (off_t)offset, whence);
#endif
}

static int64_t _file_tell(FILE *file)
{
#ifdef _WIN32
  return _ftelli64(file);
#else
  return (int64_t)ftello(file);
#endif
}

static gboolean _read_at(FILE *file, const uint64_t offset, void *out, const size_t size)
{
  if(_file_seek(file, (int64_t)offset, SEEK_SET) != 0) return FALSE;
  return fread(out, 1, size, file) == size;
}

static gboolean _reader_find_end_record(FILE *file, uint64_t *central_offset, uint32_t *central_size,
                                        uint16_t *entry_count)
{
  if(_file_seek(file, 0, SEEK_END) != 0) return FALSE;
  const int64_t file_size = _file_tell(file);
  if(file_size < (int64_t)ZIP_END_RECORD_SIZE) return FALSE;

  const uint64_t max_tail = ZIP_END_RECORD_SIZE + ZIP_MAX_COMMENT_SIZE;
  const uint64_t tail_size = (uint64_t)file_size < max_tail ? (uint64_t)file_size : max_tail;
  uint8_t *tail = g_try_malloc(tail_size);
  if(IS_NULL_PTR(tail)) return FALSE;
  const uint64_t tail_offset = (uint64_t)file_size - tail_size;
  if(!_read_at(file, tail_offset, tail, tail_size))
  {
    dt_free(tail);
    return FALSE;
  }

  gboolean found = FALSE;
  for(int64_t pos = (int64_t)tail_size - (int64_t)ZIP_END_RECORD_SIZE; pos >= 0 && !found; pos--)
  {
    if(_get_u32(tail + pos) != ZIP_SIGNATURE_END) continue;
    const uint8_t *record = tail + pos;
    const uint16_t comment_length = _get_u16(record + 20);
    if(pos + ZIP_END_RECORD_SIZE + comment_length != tail_size) continue;
    *entry_count = _get_u16(record + 10);
    *central_size = _get_u32(record + 12);
    *central_offset = _get_u32(record + 16);
    found = TRUE;
  }
  dt_free(tail);
  return found;
}

dt_canvas_zip_reader_t *dt_canvas_zip_reader_open(const char *path)
{
  if(IS_NULL_PTR(path) || path[0] == '\0') return NULL;
  FILE *file = g_fopen(path, "rb");
  if(IS_NULL_PTR(file)) return NULL;

  uint64_t central_offset = 0;
  uint32_t central_size = 0;
  uint16_t entry_count = 0;
  if(!_reader_find_end_record(file, &central_offset, &central_size, &entry_count))
  {
    fclose(file);
    return NULL;
  }

  uint8_t *central = g_try_malloc(central_size == 0 ? 1 : central_size);
  if(IS_NULL_PTR(central) || !_read_at(file, central_offset, central, central_size))
  {
    dt_free(central);
    fclose(file);
    return NULL;
  }

  dt_canvas_zip_reader_t *reader = g_new0(dt_canvas_zip_reader_t, 1);
  reader->file = file;
  reader->entries = g_ptr_array_new_with_free_func(_entry_free);
  reader->by_name = g_hash_table_new(g_str_hash, g_str_equal);

  uint64_t pos = 0;
  gboolean ok = TRUE;
  for(uint16_t idx = 0; idx < entry_count && ok; idx++)
  {
    if(pos + ZIP_CENTRAL_HEADER_SIZE > central_size || _get_u32(central + pos) != ZIP_SIGNATURE_CENTRAL)
    {
      ok = FALSE;
      break;
    }
    const uint8_t *header = central + pos;
    const uint16_t name_length = _get_u16(header + 28);
    const uint16_t extra_length = _get_u16(header + 30);
    const uint16_t comment_length = _get_u16(header + 32);
    if(pos + ZIP_CENTRAL_HEADER_SIZE + name_length > central_size)
    {
      ok = FALSE;
      break;
    }
    dt_zip_entry_t *entry = g_new0(dt_zip_entry_t, 1);
    entry->name = g_strndup((const char *)(header + ZIP_CENTRAL_HEADER_SIZE), name_length);
    entry->method = _get_u16(header + 10);
    entry->dos_time = _get_u16(header + 12);
    entry->dos_date = _get_u16(header + 14);
    entry->crc = _get_u32(header + 16);
    entry->compressed_size = _get_u32(header + 20);
    entry->uncompressed_size = _get_u32(header + 24);
    entry->local_offset = _get_u32(header + 42);
    g_ptr_array_add(reader->entries, entry);
    // A duplicated name keeps its first entry, which is what a writer producing one per name never creates.
    if(!g_hash_table_contains(reader->by_name, entry->name)) g_hash_table_insert(reader->by_name, entry->name, entry);
    pos += ZIP_CENTRAL_HEADER_SIZE + name_length + extra_length + comment_length;
  }
  dt_free(central);

  if(!ok)
  {
    dt_canvas_zip_reader_close(reader);
    return NULL;
  }
  return reader;
}

gboolean dt_canvas_zip_reader_has(const dt_canvas_zip_reader_t *reader, const char *name)
{
  if(IS_NULL_PTR(reader) || IS_NULL_PTR(name)) return FALSE;
  return g_hash_table_contains(reader->by_name, name);
}

GBytes *dt_canvas_zip_reader_get(dt_canvas_zip_reader_t *reader, const char *name)
{
  if(IS_NULL_PTR(reader) || IS_NULL_PTR(name)) return NULL;
  const dt_zip_entry_t *entry = g_hash_table_lookup(reader->by_name, name);
  if(IS_NULL_PTR(entry)) return NULL;
  if(entry->method != ZIP_METHOD_STORE && entry->method != ZIP_METHOD_DEFLATE) return NULL;

  uint8_t local[ZIP_LOCAL_HEADER_SIZE];
  if(!_read_at(reader->file, entry->local_offset, local, sizeof(local))) return NULL;
  if(_get_u32(local + 0) != ZIP_SIGNATURE_LOCAL) return NULL;
  const uint16_t name_length = _get_u16(local + 26);
  const uint16_t extra_length = _get_u16(local + 28);
  const uint64_t data_offset = (uint64_t)entry->local_offset + ZIP_LOCAL_HEADER_SIZE + name_length + extra_length;

  uint8_t *compressed = g_try_malloc(entry->compressed_size == 0 ? 1 : entry->compressed_size);
  if(IS_NULL_PTR(compressed)) return NULL;
  if(!_read_at(reader->file, data_offset, compressed, entry->compressed_size))
  {
    dt_free(compressed);
    return NULL;
  }

  uint8_t *data = NULL;
  if(entry->method == ZIP_METHOD_STORE)
  {
    if(entry->compressed_size != entry->uncompressed_size)
    {
      dt_free(compressed);
      return NULL;
    }
    data = compressed;
    compressed = NULL;
  }
  else
  {
    data = _inflate_buffer(compressed, entry->compressed_size, entry->uncompressed_size);
    dt_free(compressed);
    if(IS_NULL_PTR(data)) return NULL;
  }

  const uint32_t crc = entry->uncompressed_size > 0
                           ? (uint32_t)crc32(crc32(0L, Z_NULL, 0), data, (uInt)entry->uncompressed_size)
                           : 0;
  if(crc != entry->crc)
  {
    dt_free(data);
    return NULL;
  }
  return g_bytes_new_take(data, entry->uncompressed_size);
}

guint dt_canvas_zip_reader_count(const dt_canvas_zip_reader_t *reader)
{
  if(IS_NULL_PTR(reader)) return 0;
  return reader->entries->len;
}

const char *dt_canvas_zip_reader_name_at(const dt_canvas_zip_reader_t *reader, guint index)
{
  if(IS_NULL_PTR(reader) || index >= reader->entries->len) return NULL;
  const dt_zip_entry_t *entry = g_ptr_array_index(reader->entries, index);
  return entry->name;
}

void dt_canvas_zip_reader_close(dt_canvas_zip_reader_t *reader)
{
  if(IS_NULL_PTR(reader)) return;
  if(!IS_NULL_PTR(reader->file)) fclose(reader->file);
  if(!IS_NULL_PTR(reader->by_name)) g_hash_table_destroy(reader->by_name);
  if(!IS_NULL_PTR(reader->entries)) g_ptr_array_free(reader->entries, TRUE);
  dt_free(reader);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
