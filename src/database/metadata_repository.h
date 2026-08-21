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

/** @file database/metadata_repository.h
 *
 * @brief `main.meta_data`: the free-text image metadata (title, description, creator,
 * publisher, rights, notes, version name).
 *
 * @details One row per (image, key, value), `key` being a `dt_metadata_t` index rather
 * than a name -- the names live in `common/metadata.c`, which owns the mapping, the XMP
 * spellings and the visibility flags.
 *
 * @note Several functions here take an `imgid` where **a negative value means "every
 * selected image"** rather than "no image". That is the convention `dt_metadata_get()`
 * has always used (`id == -1` from the GUI meaning "the selection", any other value
 * meaning the image under the cursor), and it is preserved rather than corrected because
 * the callers pass it straight through from the GUI.
 */

#ifndef DT_DATABASE_METADATA_REPOSITORY_H
#define DT_DATABASE_METADATA_REPOSITORY_H

#include <glib.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

G_BEGIN_DECLS

/**
 * @brief Values stored under @p keyid, ordered by value.
 *
 * @param imgid the image, or a negative value for every selected image.
 * @return a `GList` of newly allocated strings, never containing NULL (a NULL column
 *         comes back as ""). Free with `g_list_free_full(l, g_free)`.
 */
GList *dt_metadata_repository_get_values(const int32_t imgid, const int keyid);

/**
 * @brief Every (key, value) pair of @p imgid, as a flat list.
 *
 * @return alternating entries: a newly allocated **decimal string of the keyid**, then its
 *         newly allocated value. That shape is what `dt_metadata_get_list_id()` has always
 *         returned and what the XMP writer consumes.
 */
GList *dt_metadata_repository_get_all(const int32_t imgid);

/** @brief Delete the rows of @p imgid whose key is in @p keyid_list, a comma-separated
 *  list of decimal key ids. Does nothing when @p keyid_list is NULL. */
void dt_metadata_repository_remove(const int32_t imgid, const char *keyid_list);

/** One (image, key, value) row to insert. */
typedef struct dt_metadata_row_t
{
  int32_t imgid;
  int keyid;
  const char *value; /**< borrowed for the duration of the call; quoted by the repository */
} dt_metadata_row_t;

/**
 * @brief Insert @p count rows as a single multi-VALUES statement.
 *
 * @details Quoting is done here, with `sqlite3_mprintf("%q", …)`. It used to be done by
 * the caller, which is why `common/metadata.c` linked against sqlite for one function
 * call: a module that has to escape its own strings for SQL is still writing SQL. Nothing
 * happens when @p count is 0.
 */
void dt_metadata_repository_add(const dt_metadata_row_t *rows, const size_t count);

/**
 * @brief The first image carrying @p value under any key, or -1.
 *
 * @details Used to recognise an already-imported file by the `filename-datetime` marker
 * the importer stores. Matches on value alone, across every key.
 */
int32_t dt_metadata_repository_find_image_by_value(const char *value);

/** @brief One `main.meta_data` row: its key id and its value. */
typedef void (*dt_metadata_repository_row_cb)(void *user_data, const int keyid, const char *value);

/** @brief Every metadata row of @p imgid, in row order. */
void dt_metadata_repository_foreach(const int32_t imgid, dt_metadata_repository_row_cb cb,
                                    void *user_data);

/** @brief One distinct (key, value) across the selection, and how many selected images
 *  carry it. */
typedef void (*dt_metadata_repository_selected_cb)(void *user_data, const int keyid,
                                                   const char *value, const uint32_t count);

/**
 * @brief Every distinct (key, value) pair across `main.selected_images`, ordered by value.
 *
 * @details One pass instead of one dt_metadata_repository_get_values() call per key, which
 * is what the panel needs: for each field it must know both the values present and whether
 * every selected image agrees on one -- hence the count travelling with the pair.
 */
void dt_metadata_repository_foreach_selected(dt_metadata_repository_selected_cb cb,
                                             void *user_data);

/** @brief Finalise the prepared statements. See dt_colorlabel_repository_cleanup(). */
void dt_metadata_repository_cleanup(void);

G_END_DECLS


#ifdef __cplusplus
}
#endif

#endif // DT_DATABASE_METADATA_REPOSITORY_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
