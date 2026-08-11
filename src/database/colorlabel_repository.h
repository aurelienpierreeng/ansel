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

#ifdef __cplusplus
extern "C" {
#endif

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
 * @brief The colours set on @p imgid, as a list rather than a bitmask.
 *
 * @param imgid the image, or a negative value for every selected image -- the convention
 *        dt_metadata_get() uses, which is this function's only caller.
 * @return a `GList` of colour indices as `GINT_TO_POINTER`. Sorted for a single image,
 *         in table order across a selection (where duplicates are expected and kept:
 *         the caller counts them). Free with g_list_free().
 */
GList *dt_colorlabel_repository_get_list(const int32_t imgid);

/** @brief Every colour label of @p imgid, one call per row, in the order the rows come back.
 *
 *  @details dt_colorlabel_repository_get() folds the same rows into a bitmask, which loses that
 *  order. The sidecar writer needs it, because the XMP sequence it builds is compared byte for
 *  byte against the file already on disk.
 */
void dt_colorlabel_repository_foreach(const int32_t imgid, void (*cb)(void *, const int),
                                      void *user_data);

/**
 * @brief Finalise whatever this repository still caches -- today, nothing.
 *
 * @details Every repository has one of these, and the hook is not optional even when it is
 * empty: dt_database_close() runs every repository's cleanup, because a connection cannot
 * be closed out from under a live `sqlite3_stmt` -- that contract is what makes a workspace
 * swap possible, and the repositories that do cache (image, history, style) rely on it.
 */
void dt_colorlabel_repository_cleanup(void);

G_END_DECLS


#ifdef __cplusplus
}
#endif

#endif // DT_DATABASE_COLORLABEL_REPOSITORY_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
