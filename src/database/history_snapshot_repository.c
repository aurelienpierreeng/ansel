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

#include "database/history_snapshot_repository.h"

#include "database/database.h"
#include "database/sql_debug.h"
#include "system/macros.h"

#include <sqlite3.h>

/* Run one `?1 = snap_id, ?2 = imgid` statement to completion. Every query in this file has
 * that shape, which is what makes the file short. */
static gboolean _run_snap_imgid(const char *query, const int snap_id, const int32_t imgid,
                                const int expected)
{
  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, snap_id);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
  const gboolean ok = (sqlite3_step(stmt) == expected);
  sqlite3_finalize(stmt);
  return ok;
}

int dt_history_snapshot_repository_next_id(const int32_t imgid)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT MAX(id) FROM memory.undo_history WHERE imgid=?1", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  int snap_id = 0;
  if(sqlite3_step(stmt) == SQLITE_ROW) snap_id = sqlite3_column_int(stmt, 0) + 1;
  sqlite3_finalize(stmt);
  return snap_id;
}

gboolean dt_history_snapshot_repository_create(const int snap_id, const int32_t imgid,
                                               const gboolean empty_history)
{
  gboolean all_ok = TRUE;

  dt_database_start_transaction();

  if(empty_history)
  {
    // insert a dummy undo_histroy to ensure proper snap_id later
    // clang-format off
    all_ok = _run_snap_imgid("INSERT INTO memory.undo_history"
                             "  VALUES (?1, ?2, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL)",
                             snap_id, imgid, SQLITE_DONE);
    // clang-format on
  }
  else
  {
    // copy current state into undo_history
    // clang-format off
    all_ok = _run_snap_imgid("INSERT INTO memory.undo_history"
                             "  SELECT ?1, imgid, num, module, operation, op_params, enabled, "
                             "         blendop_params, blendop_version, multi_priority, multi_name "
                             "  FROM main.history"
                             "  WHERE imgid=?2",
                             snap_id, imgid, SQLITE_DONE);

    // copy current state into undo_masks_history
    /* `&&` in this order, not the other: the original short-circuits, so once one copy
     * has failed the rest are not attempted. The transaction rolls back either way, but
     * this keeps the statement count identical. */
    all_ok = all_ok && _run_snap_imgid("INSERT INTO memory.undo_masks_history"
                                       "  SELECT ?1, imgid, num, formid, form, name, version,"
                                       "         points, points_count, source"
                                       "  FROM main.masks_history"
                                       "  WHERE imgid=?2",
                                       snap_id, imgid, SQLITE_DONE);

    // copy the module order
    all_ok = all_ok && _run_snap_imgid("INSERT INTO memory.undo_module_order"
                                       "  SELECT ?1, imgid, version, iop_list"
                                       "  FROM main.module_order"
                                       "  WHERE imgid=?2",
                                       snap_id, imgid, SQLITE_DONE);
    // clang-format on
  }

  if(all_ok)
    dt_database_release_transaction();
  else
    dt_database_rollback_transaction();

  return all_ok;
}

gboolean dt_history_snapshot_repository_restore(const int snap_id, const int32_t imgid)
{
  // clang-format off
  gboolean all_ok = _run_snap_imgid("INSERT INTO main.history"
                                    "  SELECT imgid, num, module, operation, op_params, enabled, "
                                    "         blendop_params, blendop_version, multi_priority, multi_name "
                                    "  FROM memory.undo_history"
                                    "  WHERE imgid=?2 AND id=?1",
                                    snap_id, imgid, SQLITE_DONE);

  /* `&= ` in the original, which does NOT short-circuit: all three copies are attempted
   * even after one fails. Kept. */
  all_ok = _run_snap_imgid("INSERT INTO main.masks_history"
                           "  SELECT imgid, num, formid, form, name, version, "
                           "         points, points_count, source"
                           "  FROM memory.undo_masks_history"
                           "  WHERE imgid=?2 AND id=?1",
                           snap_id, imgid, SQLITE_DONE) && all_ok;

  all_ok = _run_snap_imgid("INSERT INTO main.module_order"
                           "  SELECT imgid, version, iop_list"
                           "  FROM memory.undo_module_order"
                           "  WHERE imgid=?2 AND id=?1",
                           snap_id, imgid, SQLITE_DONE) && all_ok;
  // clang-format on

  return all_ok;
}

void dt_history_snapshot_repository_clear(const int snap_id, const int32_t imgid)
{
  _run_snap_imgid("DELETE FROM memory.undo_history WHERE id=?1 AND imgid=?2",
                  snap_id, imgid, SQLITE_DONE);
  _run_snap_imgid("DELETE FROM memory.undo_masks_history WHERE id=?1 AND imgid=?2",
                  snap_id, imgid, SQLITE_DONE);
  _run_snap_imgid("DELETE FROM memory.undo_module_order WHERE id=?1 AND imgid=?2",
                  snap_id, imgid, SQLITE_DONE);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
