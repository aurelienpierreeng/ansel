/*
    This file is part of darktable,
    Copyright (C) 2025 Aurélien PIERRE.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

/** @file database/image_repository.h
 *
 * @brief Reading and writing one ::dt_image_t to and from the library database. The SQL half
 * of what used to be `common/image_cache.c`.
 *
 * @details The image cache was two things wearing one name: an LRU of `dt_image_t` structs
 * with refcounting and per-entry locking, and the only code in the tree that knows the shape
 * of the `main.images` row. The second is not a cache, it is a repository -- 107 of the file's
 * lines were SQL -- and keeping them together meant every consumer of the cache also had a
 * translation unit that could reach the database.
 *
 * They are separate now. `caches/image_cache.c` holds the LRU and calls in here whenever it
 * needs a row read or written; this file holds every `sqlite3_*` call and the prepared
 * statements behind them, and knows nothing about caching, refcounting or eviction.
 *
 * @warning The statement cache below is process-wide and guarded by one mutex, so the calls
 * here serialise against each other. That is inherited behaviour, not a new constraint: the
 * cache took the same mutex around the same statements before the split.
 */

#ifndef DT_DATABASE_IMAGE_REPOSITORY_H
#define DT_DATABASE_IMAGE_REPOSITORY_H

#include "common/image.h"

#include <sqlite3.h>
#include <stdint.h>

G_BEGIN_DECLS

/**
 * @brief Fill @p img from the `main.images` row for @p imgid.
 *
 * @param imgid image to read.
 * @param img destination. On failure its `id` is set to -1, which is what
 *        dt_image_invalid() tests, and the rest is left as the caller had it.
 * @return TRUE when a row was found and read.
 */
gboolean dt_image_repository_load(const int32_t imgid, dt_image_t *img);

/**
 * @brief Write @p img back to `main.images`, plus its colour labels and history hash.
 *
 * @details Three statements, because three tables carry one image's state: the row itself,
 * `main.color_labels` (through dt_colorlabels_set_labels()) and `main.history_hash`. A caller
 * that wrote only the first would leave an image whose labels and hash describe its previous
 * contents.
 *
 * @param img image to persist. Ignored when NULL or when its `id` is not positive.
 */
void dt_image_repository_store(const dt_image_t *img);

/**
 * @brief Fill @p img from a row of a query that selected the repository's own column list.
 *
 * @details Public because `gui/dtgtk/thumbtable.c` runs its own bulk query with the same
 * columns and maps each row with this rather than reloading images one at a time. Which means
 * the column list in this file and the one in that query are a contract: changing either
 * without the other silently shifts every field.
 *
 * @param img destination.
 * @param stmt a stepped statement positioned on a row.
 */
void dt_image_from_stmt(dt_image_t *img, sqlite3_stmt *stmt);

/**
 * @brief Finalise the prepared statements. Called at shutdown, after the last cache release
 * and before the database connection closes.
 */
void dt_image_repository_cleanup(void);

G_END_DECLS

#endif // DT_DATABASE_IMAGE_REPOSITORY_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
