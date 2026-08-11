/*
    This file is part of darktable,
    Copyright (C) 2009-2011 johannes hanika.
    Copyright (C) 2010-2011 Henrik Andersson.
    Copyright (C) 2011-2016 Tobias Ellinghaus.
    Copyright (C) 2012, 2019-2022 Pascal Obry.
    Copyright (C) 2025-2026 Aurélien PIERRE.

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

#ifndef DT_DATABASE_FILM_REPOSITORY_H
#define DT_DATABASE_FILM_REPOSITORY_H

#include <glib.h>
#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/** `main.film_rolls` -- one row per imported folder -- and the `memory.film_folder` scratch table
 *  that records, for this session, which of those folders still exist on disk.
 *
 *  Deciding what to do about a film roll is `common/film.c`'s: whether an empty one should have
 *  its directory removed, whether the user is asked first, what a relocation means. This module
 *  answers only what the table says and writes back what it is told.
 */

/** the id of the roll whose folder is @p folder, or -1.
 *
 *  Matched case-insensitively on Windows and exactly everywhere else, which is why the caller
 *  cannot express this as a plain equality itself. */
int32_t dt_film_repository_find_by_folder(const char *folder);

/** the folder of roll @p id, or NULL. Caller owns it. */
char *dt_film_repository_get_folder(const int32_t id);

/** insert a roll for @p folder, stamped as accessed now */
gboolean dt_film_repository_insert(const char *folder);

/** stamp roll @p id as accessed now */
gboolean dt_film_repository_touch_access(const int32_t id);

/** move roll @p id to @p folder */
gboolean dt_film_repository_set_folder(const int32_t id, const char *folder);

/** delete roll @p id.
 *
 *  Foreign keys cascade this to every image of the roll and to everything referencing those
 *  images, so the caller must have released them from its caches first. */
gboolean dt_film_repository_delete(const int32_t id);

/** TRUE when roll @p id has at least one image */
gboolean dt_film_repository_has_images(const int32_t id);

/** every image id of roll @p filmid, in row order */
GList *dt_film_repository_get_image_ids(const int32_t filmid);

/** one `main.film_rolls` row */
typedef void (*dt_film_repository_row_cb)(void *user_data, const int32_t id, const char *folder);

/** every roll, in row order */
void dt_film_repository_foreach(dt_film_repository_row_cb cb, void *user_data);

/** every roll that has no images left */
void dt_film_repository_foreach_empty(dt_film_repository_row_cb cb, void *user_data);

/** every roll whose folder is @p path or below it */
void dt_film_repository_foreach_under(const char *path, dt_film_repository_row_cb cb,
                                      void *user_data);

/* ---------------------------------------------------------------------------------------------
 * memory.film_folder -- does this roll's folder still exist?
 * ------------------------------------------------------------------------------------------ */

/** forget every recorded status */
void dt_film_repository_folder_status_clear(void);

/** record whether roll @p id's folder is currently present */
void dt_film_repository_folder_status_set(const int32_t id, const gboolean present);

#ifdef __cplusplus
}
#endif

#endif // DT_DATABASE_FILM_REPOSITORY_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
