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

/** @file database/tag_repository.h
 *
 * @brief `data.tags` and `main.tagged_images`.
 *
 * @details A tag is a row in `data.tags` (id, name, flags, synonyms) and an attachment is
 * a row in `main.tagged_images` (imgid, tagid, position). `memory.darktable_tags` caches
 * which tags are internal (`darktable|…`), so the collection query can exclude them
 * cheaply.
 *
 * @warning **Partial.** `common/tags.c` still holds the tag *listing* machinery -- the
 * suggestion, usage-count and similar-tag queries, several of which are multi-level
 * SELECTs assembled from conditional fragments. Those belong here too. Extend this file;
 * do not start a second tag repository.
 */

#ifndef DT_DATABASE_TAG_REPOSITORY_H
#define DT_DATABASE_TAG_REPOSITORY_H

#include "common/tags.h"

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

/**
 * @brief Names of the tags attached to an image.
 *
 * @param imgid the image, or a negative value for every selected image -- the convention
 *        dt_metadata_get() uses, which is this function's only caller. Across a selection
 *        the same name appears once per image carrying it, and that is deliberate: the
 *        caller counts occurrences.
 * @return a `GList` of newly allocated names. Free with `g_list_free_full(l, g_free)`.
 */
GList *dt_tag_repository_get_attached_names(const int32_t imgid);

/* ---------------------------------------------------------------------------------------
 *  Identity and lifecycle -- `data.tags`
 * ------------------------------------------------------------------------------------- */

/** @brief The id of the tag named @p name, or 0 when there is none. */
guint dt_tag_repository_find_by_name(const char *name);

/** @brief The id of the tag whose name matches @p name case-INSENSITIVELY, or 0. */
guint dt_tag_repository_find_by_name_nocase(const char *name);

/** @brief Insert a tag named @p name and return its new id, or 0 on failure. */
guint dt_tag_repository_insert(const char *name);

/** @brief The name of @p tagid, newly allocated, or NULL. */
gchar *dt_tag_repository_get_name(const guint tagid);

/** @brief Rename @p tagid. The caller checks first that the new name is free. */
void dt_tag_repository_rename(const guint tagid, const char *new_name);

/** @brief How many attachment ROWS @p tagid has, or -1 if the count could not be read.
 *  See dt_tag_repository_count_distinct_images() for the other question. */
int dt_tag_repository_count_attachments(const guint tagid);

/** @brief Delete @p tagid, its attachments, and its `memory.darktable_tags` entry. */
void dt_tag_repository_delete(const guint tagid);

/** @brief Delete every tag in @p id_list and its attachments.
 *  @param id_list a comma-separated list of decimal tag ids, composed by the caller. */
void dt_tag_repository_delete_batch(const char *id_list);

/* ---------------------------------------------------------------------------------------
 *  `memory.darktable_tags` -- the cache of which tags are internal
 * ------------------------------------------------------------------------------------- */

/** @brief Record @p tagid as an internal tag. */
void dt_tag_repository_mark_internal(const guint tagid);

/** @brief Rebuild the whole internal-tag cache from `data.tags`. */
void dt_tag_repository_rebuild_internal(void);

/* ---------------------------------------------------------------------------------------
 *  Flags and synonyms
 * ------------------------------------------------------------------------------------- */

/** @brief The flags word of @p tagid, or 0. */
gint dt_tag_repository_get_flags(const guint tagid);

/** @brief Replace the flags word of @p tagid. */
void dt_tag_repository_set_flags(const guint tagid, const gint flags);

/** @brief Set the bits in @p set and clear those absent from @p keep_mask:
 *  `flags = (IFNULL(flags,0) & keep_mask) | set`. */
void dt_tag_repository_update_flags(const guint tagid, const gint set, const gint keep_mask);

/** @brief The synonyms of @p tagid, newly allocated, or NULL. */
gchar *dt_tag_repository_get_synonyms(const guint tagid);

/** @brief Replace the synonyms of @p tagid. */
void dt_tag_repository_set_synonyms(const guint tagid, const char *synonyms);

/* ---------------------------------------------------------------------------------------
 *  Attachments -- `main.tagged_images`
 * ------------------------------------------------------------------------------------- */

/** @brief TRUE when @p tagid is attached to @p imgid. */
gboolean dt_tag_repository_is_attached(const guint tagid, const int32_t imgid);

/** @brief Images carrying @p tagid, `GINT_TO_POINTER`. Free with g_list_free(). */
GList *dt_tag_repository_get_images(const guint tagid);

/** @brief Images carrying @p tagid, restricted to @p imgid_list -- a comma-separated list
 *  of decimal image ids composed by the caller. */
GList *dt_tag_repository_get_images_in_list(const guint tagid, const char *imgid_list);

/**
 * @brief Distinct images carrying @p tagid.
 *
 * @warning Not the same question as dt_tag_repository_count_attachments(), which counts
 * ROWS. They differ if an image ever gets the same tag twice, and the two callers ask for
 * different reasons -- one for "is this tag still in use", one to show a number to the
 * user.
 */
uint32_t dt_tag_repository_count_distinct_images(const guint tagid);

/** @brief Detach every tag in @p tagid_list from @p imgid.
 *  @param tagid_list comma-separated decimal tag ids. Does nothing when NULL. */
void dt_tag_repository_detach_batch(const int32_t imgid, const char *tagid_list);

/** @brief Attach rows given as the VALUES clause of the insert -- `"(imgid,tagid,pos),…"`.
 *  The position expression is the caller's, which is why this takes text. */
void dt_tag_repository_attach_batch(const char *values);

/* ---------------------------------------------------------------------------------------
 *  Attached-tag listings
 *
 *  These return `dt_tag_t` with `id`, `tag`, `flags`, `synonym` and `count` filled.
 *  `leave` (the last path component) and `select` (how much of the selection carries the
 *  tag) are left to the caller: the first is string handling and the second needs the
 *  selection size, which is `common/selection.c`'s to know, not the database's.
 * ------------------------------------------------------------------------------------- */

/**
 * @brief Tags attached to one image or to the current selection, ordered by name.
 *
 * @param imgid a positive image id, or <= 0 to read the selection (joining
 *        `main.selected_images` rather than binding an id).
 * @param ignore_internal exclude tags in `memory.darktable_tags`.
 * @return `GList` of `dt_tag_t *`; `count` is the number of DISTINCT images each tag is on.
 *         Free with `dt_tag_free_result()`.
 */
GList *dt_tag_repository_get_attached(const int32_t imgid, const gboolean ignore_internal);

/**
 * @brief Tags attached to @p imgid for export, plus every ancestor on their paths.
 *
 * @details The ancestors are what lets the caller check whether a node in the path is a
 * category, so a hierarchical tag exports the right way. Internal tags are always excluded
 * and `count` is not filled -- the export does not use it.
 */
GList *dt_tag_repository_get_attached_for_export(const int32_t imgid);

/** @brief Finalise the cached statements. See dt_colorlabel_repository_cleanup(). */
void dt_tag_repository_cleanup(void);

/** One row of dt_tag_repository_get_by_path_with_counts(). */
typedef struct dt_tag_count_t
{
  guint id;
  gchar *name; /**< the FULL name, root included; trimming it is the caller's business */
  guint count;
} dt_tag_count_t;

/** @brief Release one ::dt_tag_count_t, name included. Suits `g_list_free_full()`. */
void dt_tag_count_free(gpointer data);

/**
 * @brief Tags at @p path or below it, each with the number of distinct images carrying it.
 *
 * @param path exact name to match.
 * @param path_prefix names starting with this also match; normally `path` plus a `|`.
 * @return a `GList` of newly allocated ::dt_tag_count_t. Free with
 *         `g_list_free_full(l, dt_tag_count_free)`.
 */
GList *dt_tag_repository_get_by_path_with_counts(const char *path, const char *path_prefix);


G_END_DECLS

#endif // DT_DATABASE_TAG_REPOSITORY_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
