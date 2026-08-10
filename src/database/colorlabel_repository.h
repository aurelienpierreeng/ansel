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

/** @file database/colorlabel_repository.h
 *
 * @brief `main.color_labels`: which of the five colour labels are set on an image.
 *
 * @details One row per (image, colour). `common/colorlabels.c` owns everything else about
 * colour labels -- their names, the undo records, the toggle semantics, the toast -- and
 * reaches the table only through here.
 */

#ifndef DT_DATABASE_COLORLABEL_REPOSITORY_H
#define DT_DATABASE_COLORLABEL_REPOSITORY_H

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

/**
 * @brief The colours set on @p imgid, as a bitmask: bit `c` is set when colour `c` is.
 *
 * @details Returns 0 for an image with no labels, which is also what an unknown image
 * returns -- the table simply has no rows for it.
 */
int dt_colorlabel_repository_get(const int32_t imgid);

/** @brief Set colour @p color on @p imgid. Setting one that is already set does nothing. */
void dt_colorlabel_repository_set(const int32_t imgid, const int color);

/** @brief Clear colour @p color on @p imgid. Clearing one that is not set does nothing. */
void dt_colorlabel_repository_remove(const int32_t imgid, const int color);

/** @brief Clear every colour on @p imgid. */
void dt_colorlabel_repository_remove_all(const int32_t imgid);

/** @brief TRUE when colour @p color is set on @p imgid. */
gboolean dt_colorlabel_repository_has(const int32_t imgid, const int color);

/**
 * @brief Finalise the prepared statements.
 *
 * @details Every repository has one of these, and they are not optional: a connection
 * cannot be closed out from under a live `sqlite3_stmt`, so this is what has to run before
 * dt_database_close() for a workspace swap to become possible.
 */
void dt_colorlabel_repository_cleanup(void);

G_END_DECLS

#endif // DT_DATABASE_COLORLABEL_REPOSITORY_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
