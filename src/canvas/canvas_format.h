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

#ifndef DT_CANVAS_CANVAS_FORMAT_H
#define DT_CANVAS_CANVAS_FORMAT_H

/**
 * @file canvas_format.h
 * @brief The binary index of a canvas archive: the records, without the archive.
 *
 * @details Split from canvas.c so the byte layout can be unit-tested on buffers. The
 * archive entry names are here too, so the writer and the reader cannot drift apart.
 *
 * Layout, all little-endian, no alignment padding:
 *
 *   header:  magic[8] "ANSELCNV", u32 format_version, u32 header_size, u32 object_count,
 *            then the canvas fields in declaration order, then the reserved bytes.
 *   object:  u32 kind, u32 record_size, then the common fields, the kind's fields and the
 *            two reserved blocks. `record_size` counts everything from `kind` on, so a
 *            reader positions the next record without knowing the fields of this one.
 *
 * Strings are fixed-size, zero-padded, and truncated on write if longer. Floats are IEEE
 * binary32 / binary64 in little-endian byte order.
 */

#include "canvas/canvas.h"

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The archive entry that declares the file type, stored first and uncompressed. */
#define DT_CANVAS_ENTRY_MIMETYPE "mimetype"
#define DT_CANVAS_MIMETYPE "application/x-ansel-canvas"
/** The index. */
#define DT_CANVAS_ENTRY_INDEX "canvas.bin"
/** printf patterns taking the object id. */
#define DT_CANVAS_ENTRY_IMAGE_PATTERN "images/%u.jpg"
#define DT_CANVAS_ENTRY_TEXT_PATTERN "texts/%u.md"
#define DT_CANVAS_ENTRY_MAP_PATTERN "maps/%u.jpg"

#define DT_CANVAS_ERROR (dt_canvas_error_quark())
GQuark dt_canvas_error_quark(void);

typedef enum dt_canvas_error_t
{
  DT_CANVAS_ERROR_NOT_A_CANVAS,   ///< not an archive, or no index, or a bad magic
  DT_CANVAS_ERROR_VERSION,        ///< the index is from a newer format
  DT_CANVAS_ERROR_CORRUPT,        ///< a record does not parse
  DT_CANVAS_ERROR_IO,             ///< the file could not be read or written
} dt_canvas_error_t;

/** @brief Serialise the header and every object record. */
GBytes *dt_canvas_format_write_index(const dt_canvas_t *canvas);

/**
 * @brief Parse an index into an EMPTY canvas: its header fields and its objects.
 * @details Objects arrive without their JPEG or Markdown; the caller attaches those from
 * the archive entries named by the patterns above.
 */
gboolean dt_canvas_format_read_index(dt_canvas_t *canvas, GBytes *index, GError **error);

#ifdef __cplusplus
}
#endif

#endif // DT_CANVAS_CANVAS_FORMAT_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
