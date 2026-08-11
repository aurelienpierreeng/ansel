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

/** @file database/history_snapshot_repository.h
 *
 * @brief `memory.undo_history`, `memory.undo_masks_history`, `memory.undo_module_order`.
 *
 * @details The lighttable's undo for history operations does not keep an in-memory record
 * of what changed -- it copies the three history tables into three `memory.` twins before
 * the operation and copies them back to undo it. So every function here is a bulk
 * table-to-table copy, which is also why they are here rather than expressed as reads and
 * writes of `dt_dev_history_item_t`: the rows never become C structs at all.
 *
 * `common/history_snapshot.c` keeps the undo bookkeeping -- the snapshot ids on
 * `dt_undo_lt_history_t`, the history-end handling, the image-cache invalidation.
 */

#ifndef DT_DATABASE_HISTORY_SNAPSHOT_REPOSITORY_H
#define DT_DATABASE_HISTORY_SNAPSHOT_REPOSITORY_H

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

/** @brief The id the next snapshot of @p imgid should use: one past the highest taken. */
int dt_history_snapshot_repository_next_id(const int32_t imgid);

/**
 * @brief Copy @p imgid's history, masks history and module order into the undo tables.
 *
 * @param empty_history TRUE when the image has no history at all. A placeholder row goes
 *        into `memory.undo_history` instead of the three copies, so that the next
 *        dt_history_snapshot_repository_next_id() still counts this snapshot.
 * @return TRUE when every statement succeeded. The whole thing is one transaction and
 *         rolls back otherwise.
 */
gboolean dt_history_snapshot_repository_create(const int snap_id, const int32_t imgid,
                                               const gboolean empty_history);

/**
 * @brief Copy snapshot @p snap_id of @p imgid back over the live history tables.
 *
 * @warning The caller must have cleared the live history first, and must be inside its own
 * transaction: restoring is only half of an operation whose other half
 * (dt_history_delete_on_image_ext(), dt_history_repository_set_end()) is domain code this module must
 * not call. `common/history_snapshot.c` opens the transaction around both.
 */
gboolean dt_history_snapshot_repository_restore(const int snap_id, const int32_t imgid);

/** @brief Drop snapshot @p snap_id of @p imgid from all three undo tables. */
void dt_history_snapshot_repository_clear(const int snap_id, const int32_t imgid);

G_END_DECLS

#endif // DT_DATABASE_HISTORY_SNAPSHOT_REPOSITORY_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
