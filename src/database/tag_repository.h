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

#ifdef __cplusplus
extern "C" {
#endif

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

/** @brief Attach @p tagid to @p imgid, at the end of the tag order. */
gboolean dt_tag_repository_attach(const guint tagid, const int32_t imgid);

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

/** Which tags a listing should include. Its own enum rather than `common/tags.c`'s
 *  `dt_tag_type_t`, which is private to that file -- a repository borrowing a caller's
 *  private type would make the caller impossible to move. */
typedef enum dt_tag_kind_t
{
  DT_TAG_KIND_ANY = 0,  /**< every tag */
  DT_TAG_KIND_INTERNAL, /**< only `darktable|…`, i.e. those in `memory.darktable_tags` */
  DT_TAG_KIND_USER      /**< everything that is not internal */
} dt_tag_kind_t;

/**
 * @brief Ids of the tags on a set of images, optionally restricted by kind.
 *
 * @param imgid_list a comma-separated list of decimal image ids, composed by the caller --
 *        one id for a single image, or the selection's, which `common/selection.c` already
 *        knows how to spell.
 * @param kind all tags, only the internal `darktable|…` ones, or only the others.
 * @return `GList` of tag ids as `GINT_TO_POINTER`. Free with g_list_free().
 */
GList *dt_tag_repository_get_ids_for_images(const char *imgid_list, const dt_tag_kind_t kind);

/**
 * @brief Every user tag with how often it is used and how much of the selection carries it.
 *
 * @param nb_selected how many images are selected, which `common/selection.c` knows and
 *        the database does not.
 * @return `GList` of `dt_tag_t *` ordered by name, with `count` = total attachments and
 *         `select` already folded into the tri-state. `leave` is left to the caller.
 *
 * @note This one fills `select` itself, where dt_tag_repository_get_attached() leaves it.
 *       There, `count` IS the per-image number, so the caller can fold it afterwards;
 *       here `count` is the tag's global usage and the per-selection number appears only
 *       inside the query. Rather than hand back a second parallel list for it, or park it
 *       in a field whose units then lie, the fold happens where the number exists.
 */
GList *dt_tag_repository_get_with_usage(const uint32_t nb_selected);

/**
 * @brief User tags attached to at least one image of the current collection, by name.
 * @return `GList` of `dt_tag_t *` with `tag` and `id` set only -- the caller shows names.
 */
GList *dt_tag_repository_get_collection_tags(void);

/**
 * @brief Names of the tags on @p imgid that start with @p category.
 * @return `GList` of newly allocated full tag names, in query order. The caller slices
 *         them into path components; that is string work, not storage.
 */
GList *dt_tag_repository_get_names_under(const int32_t imgid, const char *category);

/* ---------------------------------------------------------------------------------------
 *  Keyword search -- a tag and everything under it
 *
 *  Both of these first fill `memory.similar_tags` with the tag whose name equals the
 *  keyword plus every tag whose name starts with `keyword|`, then read from that. The
 *  scratch table is emptied on the way out, as it always was.
 * ------------------------------------------------------------------------------------- */

/** @brief How many tags match @p keyword, and how many images carry any of them. */
void dt_tag_repository_count_similar(const char *keyword, int *tag_count, int *img_count);

/**
 * @brief The tags matching @p keyword and the images carrying any of them.
 *
 * @param tags receives `dt_tag_t *` with `id` and `tag` set; appended, not replaced.
 * @param imgids receives image ids as `GINT_TO_POINTER`; appended, not replaced.
 */
void dt_tag_repository_get_similar(const char *keyword, GList **tags, GList **imgids);

/**
 * @brief Tags worth suggesting for the current selection.
 *
 * @details Two sources unioned: tags that co-occur with the selection's tags often enough
 * to clear @p confidence, and the user's recent-tag list. Tags already on every selected
 * image are rejected from both -- suggesting one would be a no-op.
 *
 * @param nb_selected how many images are selected. Also used to fold `select`, for the
 *        same reason as dt_tag_repository_get_with_usage().
 * @param confidence percentage, 0..100. At 100 the co-occurrence half is skipped entirely
 *        and only the recent list is returned -- a different query, not a stricter filter.
 * @param recent_tags the recent-tag names, already quoted and comma-separated by the
 *        caller, dropped into an `IN (…)`. It comes from the user's own configuration.
 * @param nb_recent how many of them to keep.
 * @return `GList` of `dt_tag_t *`; `leave` is left to the caller.
 */
GList *dt_tag_repository_get_suggestions(const uint32_t nb_selected, const int confidence,
                                         const char *recent_tags, const int nb_recent);

/**
 * @brief Do all of @p imgids carry exactly the same tags? The same categories?
 *
 * @details A tag is shared when every image in the set carries it, so the test is per tag:
 * count the distinct images attached to it and compare against the size of the set. Any tag
 * short of that means the images disagree. Tags flagged `DT_TF_CATEGORY` are answered
 * separately because the panel shows them on their own row. Darktable's internal `darktable|*`
 * namespace is excluded, as it always was.
 *
 * Both out-parameters are set to TRUE for an empty or unreadable set -- vacuously, nothing
 * disagrees. Either may be NULL.
 */
void dt_tag_repository_get_agreement(GList *imgids, gboolean *same_tags,
                                     gboolean *same_categories);

/** @brief Finalise whatever this repository still caches -- today, nothing. See
 *  dt_colorlabel_repository_cleanup() for why the hook stays. */
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


#ifdef __cplusplus
}
#endif

#endif // DT_DATABASE_TAG_REPOSITORY_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
