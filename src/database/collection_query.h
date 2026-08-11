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
 *  is why it lives here. The rules come in as a `dt_collection_params_t` plus the rule-derived
 *  WHERE fragments; the SQL built from them never leaves this file. Callers ask for ids, counts
 *  and offsets.
 *
 *  Reading the user's rules out of conf stays in `common/collection.c`: this module reads no
 *  configuration, which is what lets it be reused against a different database.
 *
 *  There is no handle. `dt_collection_new()` has a single call site, so the collection is a
 *  singleton in practice and an argument no caller chooses is not a parameter.
 */

/** Replace the rules and recompose. `where_ext` is the NULL-terminated array of WHERE fragments
 *  the rules produced; both it and `params` are copied, so the caller keeps ownership. */
int dt_collection_query_set_rules(const dt_collection_params_t *params, gchar **where_ext,
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

/** The first `limit` image ids of the collection, in collection order (-1 for all of them). */
GList *dt_collection_query_get_images(const uint32_t limit);

/** The id at position `nth`, or -1. */
int32_t dt_collection_query_get_nth(const int nth);

/** The position of `imgid` within the collection, or -1 if it is not in it. */
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
