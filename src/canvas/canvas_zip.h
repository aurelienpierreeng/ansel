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

#ifndef DT_CANVAS_CANVAS_ZIP_H
#define DT_CANVAS_CANVAS_ZIP_H

/**
 * @file canvas_zip.h
 * @brief A minimal ZIP container: write a set of named byte buffers, read them back.
 *
 * @details A canvas is a ZIP archive, so it can be opened by anyone with a file manager:
 * the JPEG renders and the Markdown texts are ordinary files inside it. Nothing links an
 * archive library, and zlib is already a hard dependency through libpng, so this is the
 * subset of PKWARE's APPNOTE that a self-produced archive needs: `store` and `deflate`
 * entries, one central directory, no ZIP64 (an entry or an archive above 4 GB is refused
 * rather than silently truncated), no encryption, no multi-disk, UTF-8 names.
 *
 * The writer produces the archive in a sibling temporary file and renames it over the
 * destination on commit, so an interrupted save never leaves a half-written canvas
 * where a whole one used to be. The reader keeps the file open and reads each entry on
 * demand, so opening a canvas costs the central directory, not the JPEGs.
 *
 * Neither object is thread-safe: one thread owns a writer or a reader at a time.
 */

#include <glib.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dt_canvas_zip_writer_t dt_canvas_zip_writer_t;
typedef struct dt_canvas_zip_reader_t dt_canvas_zip_reader_t;

/**
 * @brief Start writing an archive that will become `path` on commit.
 * @param path the destination. A temporary file is created next to it.
 * @return the writer, or NULL when the temporary file could not be created.
 */
dt_canvas_zip_writer_t *dt_canvas_zip_writer_open(const char *path);

/**
 * @brief Append one entry.
 * @param name the entry name, UTF-8, `/`-separated. Must not start with `/`.
 * @param data the bytes. May be NULL when `size` is 0.
 * @param size the byte count. Above 4 GB the entry is refused.
 * @param compress TRUE to deflate, FALSE to store verbatim (JPEGs do not shrink).
 * @return TRUE when the entry was written.
 */
gboolean dt_canvas_zip_writer_add(dt_canvas_zip_writer_t *writer, const char *name, const void *data, size_t size,
                                  gboolean compress);

/**
 * @brief Finish the archive.
 * @param commit TRUE writes the central directory and renames the temporary file over
 * the destination; FALSE discards the temporary file.
 * @return TRUE when the destination now holds the archive (or when a discard succeeded).
 * @note The writer is freed either way.
 */
gboolean dt_canvas_zip_writer_close(dt_canvas_zip_writer_t *writer, gboolean commit);

/**
 * @brief Open an archive for reading and parse its central directory.
 * @return the reader, or NULL when the file is not a ZIP archive this reader accepts.
 */
dt_canvas_zip_reader_t *dt_canvas_zip_reader_open(const char *path);

/** @brief Does the archive hold an entry of that name? */
gboolean dt_canvas_zip_reader_has(const dt_canvas_zip_reader_t *reader, const char *name);

/**
 * @brief Read one entry, decompressing it and checking its CRC.
 * @return the bytes, or NULL when the entry is absent, corrupt or uses a method this
 * reader does not implement. The caller owns the reference.
 */
GBytes *dt_canvas_zip_reader_get(dt_canvas_zip_reader_t *reader, const char *name);

/** @brief Number of entries in the archive. */
guint dt_canvas_zip_reader_count(const dt_canvas_zip_reader_t *reader);

/** @brief Name of the entry at that index, in central-directory order. NULL past the end. */
const char *dt_canvas_zip_reader_name_at(const dt_canvas_zip_reader_t *reader, guint index);

/** @brief Close the file and free the reader. NULL-safe. */
void dt_canvas_zip_reader_close(dt_canvas_zip_reader_t *reader);

#ifdef __cplusplus
}
#endif

#endif // DT_CANVAS_CANVAS_ZIP_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
