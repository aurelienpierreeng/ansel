/*
    This file is part of darktable,
    Copyright (C) 2009-2011 johannes hanika.
    Copyright (C) 2010-2011 Henrik Andersson.
    Copyright (C) 2011-2016 Tobias Ellinghaus.
    Copyright (C) 2012, 2019-2022 Pascal Obry.
    Copyright (C) 2025-2026 Aurelien PIERRE.

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

#ifndef DT_DATABASE_COLLECTION_QUERY_H
#define DT_DATABASE_COLLECTION_QUERY_H

#include <glib.h>
#include <inttypes.h>

#include "common/collection.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The collection query: which images the lighttable is currently showing, and in what order.
 *
 *  This is the one place in the tree that COMPOSES SQL rather than merely running it, and that
 *  is why it lives here. Rules arrive as rules -- a `dt_collection_params_t` and an array of
 *  `dt_collection_rule_t` -- never as SQL. Turning each rule into a WHERE, joining them with
 *  their conjunctions and assembling the query is this module's job, and none of that text
 *  leaves the file. Callers ask for ids, counts and offsets.
 *
 *  Reading the user's rules out of conf stays in `common/collection.c`: this module reads no
 *  configuration, which is what lets it be reused against a different database. For the same
 *  reason anything presentational -- translating a module-order name, deciding whether a
 *  metadata field is hidden -- is resolved by the caller and handed in.
 *
 *  There is no handle. `dt_collection_new()` has a single call site, so the collection is a
 *  singleton in practice and an argument no caller chooses is not a parameter.
 */

/** Resolve a user-visible module-order name to the version id stored in `main.module_order`,
 *  or -1 when it names none.
 *
 *  Matching a LOCALISED string is presentation, and this module has no business doing it -- but
 *  the "module order" collection rule is expressed that way. The caller installs a resolver. */
typedef int (*dt_collection_query_order_resolver_t)(const char *text);
void dt_collection_query_set_order_resolver(dt_collection_query_order_resolver_t fn);

/** Supply the user-visible module-order names, index-aligned with the versions stored in
 *  `main.module_order`. The value list for that property interpolates them into its query, so
 *  the module needs the strings themselves -- but not the ability to translate. Borrowed. */
void dt_collection_query_set_order_names(const char *const *names, const int count);

/** Replace the rules and recompose.
 *
 *  The rules arrive as rules -- property, mode, text -- not as SQL. Turning them into WHERE
 *  fragments and assembling the query is this module's whole job. Everything is copied, so the
 *  caller keeps ownership of what it passed. */
int dt_collection_query_set_rules(const dt_collection_params_t *params,
                                  const dt_collection_rule_t *rules, const int n_rules,
                                  const uint32_t tagid);

/** Recompose from the rules already held -- what a caller does after changing something the
 *  query text depends on without changing the rules themselves. */
int dt_collection_query_recompose(void);

/** Rebuild `memory.collected_images` from the current query. */
void dt_collection_query_refresh_memory_table(void);

/** How many images the collection currently holds. */
uint32_t dt_collection_query_count(void);

/** A number that advances on every recomposition.
 *
 *  Callers that need to notice "the collection changed" compare this. It replaces hashing the
 *  query string, which required the text to leave the module. */
uint64_t dt_collection_query_get_generation(void);

/** @brief One module's identity, for the rule that searches by module name. */
typedef struct dt_iop_name_row_t
{
  const char *operation;  /**< borrowed for the duration of the call */
  const char *name;       /**< its localised display name */
} dt_iop_name_row_t;

/**
 * @brief Fill `memory.darktable_iop_names`, the table the module-name rules join against.
 *
 * @details Must run before any collection query naming a module. The table lives here
 * because these queries are its only readers; the localised names come from the module
 * objects, which the caller owns.
 */
void dt_collection_query_set_iop_names(const dt_iop_name_row_t *rows, const size_t count);

/** The first `limit` image ids of the collection, in collection order (-1 for all of them). */
GList *dt_collection_query_get_images(const uint32_t limit);

/** The id at position `nth`, or -1. */
int32_t dt_collection_query_get_nth(const int nth);

/** The position of `imgid` within the collection, or 0 if it is not in it -- the original's
 *  convention, which callers use to land at the start rather than error out. */
int dt_collection_query_image_offset(const int32_t imgid);

/** Save / restore `memory.collected_images`, for code that needs to collect something else for
 *  a moment and put the user's collection back afterwards. */
void dt_collection_query_push(void);
void dt_collection_query_pop(void);

/** Drop from `memory.collected_images` everything that is not in `main.selected_images`. */
void dt_collection_query_restrict_to_selection(void);

/** Members of image group @p group_id that are IN the current collection, @p exclude_imgid left
 *  out. Lives here rather than in image_repository because it is scoped by the collection
 *  query, and that text does not leave this file. */
GList *dt_collection_query_get_group_members(const int32_t group_id, const int32_t exclude_imgid);

/** What to list, and every preference the answer depends on, resolved by the caller.
 *
 *  Each of these was a conf read inside the query builder. They are display decisions -- which
 *  rule excludes itself, whether a metadata field is hidden, how film rolls are ordered -- and
 *  this module reads no configuration. */
typedef struct dt_collection_values_request_t
{
  dt_collection_properties_t property;
  int exclude_rule;              /**< the rule being edited, so it does not hide its own choices */
  gboolean apply_exclude;        /**< FALSE for an OR rule, which does not limit the collection */
  gboolean metadata_hidden;      /**< for the metadata properties: is this field hidden? */
  const char *filmroll_order_by; /**< "film_rolls_id DESC" | "folder" | "folder DESC" */
} dt_collection_values_request_t;

/** The distinct values of the requested property across the collection, with counts. */
GList *dt_collection_query_get_property_values(const dt_collection_values_request_t *req);

/** Camera maker/model pairs present in the library, sanitised and as EXIF recorded them. */
void dt_collection_query_get_makermodels(const gchar *filter, GList **sanitized, GList **exif);

/** Image ids matching ONE rule, independent of the active collection -- what batch operations
 *  enumerate over. */
GList *dt_collection_query_get_images_for_rule(const dt_collection_properties_t property,
                                               const char *text, gboolean recursive);

/** The first collected image that is not in @p imgids, looked for after the list and then
 *  before it; -1 if there is none. */
int32_t dt_collection_query_find_neighbour(GList *imgids);

/** Finalise the cached statements and release the stored rules. Must run before the connection
 *  closes. */
void dt_collection_query_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif // DT_DATABASE_COLLECTION_QUERY_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
