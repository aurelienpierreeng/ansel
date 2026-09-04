/*
    This file is part of darktable,
    Copyright (C) 2026 Aurélien PIERRE.

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

/** @file database/removed_image_repository.h
 *
 * @brief `memory.removed_*`: the staging area behind "undo remove from library".
 *
 * @details Removing an image from the library deletes its `main.images` row, and the
 * foreign keys take its history, masks, tags, colour labels, metadata and module order
 * down with it. Nothing about that is recoverable from the file on disk -- the raw is
 * still there, but the edit is not -- so the rows are copied into `memory.` twins first
 * and copied back to undo the removal. Every function here is a bulk table-to-table copy,
 * which is why the rows never become C structs: the same reason
 * database/history_snapshot_repository.h gives.
 *
 * The twins are declared in database.c by `CREATE TABLE ... AS SELECT` over the live
 * tables, so their columns follow whatever `main` holds and a schema migration needs no
 * edit here. Each carries two extra leading columns, `snap_id` and `undo_imgid`, which is
 * the key every function below takes.
 *
 * `common/image.c` keeps the undo bookkeeping -- when a snapshot is taken, what the undo
 * and redo halves do with it, and when it is dropped.
 */

#ifndef DT_DATABASE_REMOVED_IMAGE_REPOSITORY_H
#define DT_DATABASE_REMOVED_IMAGE_REPOSITORY_H

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

/** @brief The id the next snapshot of @p imgid should use: one past the highest taken. */
int dt_removed_image_repository_next_id(const int32_t imgid);

/**
 * @brief Copy every row @p imgid owns out of `main`, so the removal about to happen can be undone.
 *
 * @details Call this BEFORE anything touches the image: the group membership of the images
 * staying behind is part of the snapshot, and removing a group's leader rewrites it.
 *
 * @return TRUE when every statement succeeded. The whole thing is one transaction and rolls
 *         back otherwise, in which case the caller must remove the image without an undo
 *         record rather than record one that cannot restore anything.
 */
gboolean dt_removed_image_repository_create(const int snap_id, const int32_t imgid);

/**
 * @brief Copy snapshot @p snap_id of @p imgid back into `main`, film roll included.
 *
 * @details Foreign keys are deferred to the commit, because an image is restored before the
 * group leader it points at may be: a group removed in one go comes back one image per undo
 * record, in whatever order the undo list holds. Any group_id still dangling at the end of
 * the restore is repointed at the image itself rather than left to fail the commit.
 *
 * @return TRUE when every statement succeeded; the transaction rolls back otherwise.
 */
gboolean dt_removed_image_repository_restore(const int snap_id, const int32_t imgid);

/** @brief Drop snapshot @p snap_id of @p imgid, making the removal permanent. */
void dt_removed_image_repository_clear(const int snap_id, const int32_t imgid);

G_END_DECLS

#endif // DT_DATABASE_REMOVED_IMAGE_REPOSITORY_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
