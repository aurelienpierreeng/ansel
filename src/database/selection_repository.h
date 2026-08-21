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

/** @file database/selection_repository.h
 *
 * @brief `main.selected_images`, and the `memory.selected_backup` it is pushed onto.
 *
 * @details The selection is in the database rather than in memory because the collection
 * query joins against it. `common/selection.c` keeps the in-memory mirror, the signals and
 * the range/toggle semantics; the two tables live here.
 */

#ifndef DT_DATABASE_SELECTION_REPOSITORY_H
#define DT_DATABASE_SELECTION_REPOSITORY_H

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

/** @brief Add @p imgid to the selection. Selecting an already-selected image does nothing. */
void dt_selection_repository_select(const int32_t imgid);

/** @brief Remove @p imgid from the selection. */
void dt_selection_repository_deselect(const int32_t imgid);

/**
 * @brief Add every id in @p ids to the selection.
 *
 * @param ids a comma-separated list of parenthesised single-column tuples, `"(1),(2),(3)"`
 *        -- the VALUES clause of the insert. Built by the caller because it is the caller
 *        that knows how to walk its own list.
 */
void dt_selection_repository_select_list(const char *ids);

/**
 * @brief Remove every id in @p ids from the selection.
 *
 * @param ids a comma-separated list of bare ids, `"1,2,3"`. Note this is NOT the same
 *        spelling as dt_selection_repository_select_list() takes -- one lands in a VALUES
 *        clause, the other in an `IN (...)`. That asymmetry is inherited; both callers are
 *        in `common/selection.c`, a dozen lines apart.
 */
void dt_selection_repository_deselect_list(const char *ids);

/** @brief Empty the selection. */
void dt_selection_repository_clear(void);

/** @brief Every selected image id, `GINT_TO_POINTER`, ASCENDING. Free with g_list_free().
 *
 *  The query orders DESC and the list is built by prepending, which flips it -- that is the
 *  order the selection's in-memory mirror is kept in and what every caller expects. */
GList *dt_selection_repository_get_all(void);

/** @brief Drop selected images that are no longer in the current collection. */
void dt_selection_repository_drop_uncollected(void);

/** @brief Copy the selection into `memory.selected_backup`, replacing what was there. */
void dt_selection_repository_push(void);

/** @brief Copy `memory.selected_backup` back over the selection. */
void dt_selection_repository_pop(void);

/**
 * @brief The lowest selected image id, or UNKNOWN_IMAGE when nothing is selected.
 *
 * @details The caller this replaced took the first row of an unordered
 * `SELECT imgid FROM main.selected_images`. That is not arbitrary: `imgid` is that table's
 * INTEGER PRIMARY KEY, so a full scan yields ascending ids and the first row is always the
 * lowest. Same query, and the name now says which image you get.
 */
int32_t dt_selection_repository_get_lowest_id(void);

/** @brief Finalise whatever this repository still caches -- today, nothing. See
 *  dt_colorlabel_repository_cleanup() for why the hook stays. */
void dt_selection_repository_cleanup(void);

G_END_DECLS

#endif // DT_DATABASE_SELECTION_REPOSITORY_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
