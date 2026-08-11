/*
    This file is part of darktable,
    Copyright (C) 2010-2011 Henrik Andersson.
    Copyright (C) 2011-2016 Tobias Ellinghaus.
    Copyright (C) 2012, 2019-2022 Pascal Obry.
    Copyright (C) 2018 Edgardo Hoszowski.
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

#ifndef DT_DATABASE_STYLE_REPOSITORY_H
#define DT_DATABASE_STYLE_REPOSITORY_H

#include <glib.h>
#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/** `data.styles` and `data.style_items`, plus the reads and writes of `main.history` that copy a
 *  development into a style and back.
 *
 *  Nothing here returns a `dt_style_t` or a `dt_style_item_t`. Those live in `common/styles.h`,
 *  which includes three `develop/` headers to declare `dt_iop_params_t` and
 *  `dt_develop_blend_params_t` -- four layers above this one. Rows are handed to a callback as
 *  scalars and `common/styles.c` builds the structs, which is also where a module's localized
 *  name has to come from (`dt_iop_get_localized_name()`, same layer problem).
 *
 *  A row callback runs while this module holds its statement mutex, so a callback must not call
 *  back into this repository. In practice they only build a list.
 *
 *  The `iop_list` column crosses the same boundary: this module stores and returns it as the TEXT
 *  it is, and styles.c serialises it through `dt_ioppr_*`.
 */

/* ---------------------------------------------------------------------------------------------
 * data.styles -- the header row
 * ------------------------------------------------------------------------------------------ */

/** the id of the newest style with this name, or 0 if there is none */
int32_t dt_style_repository_get_id_by_name(const char *name);

/** the style's description, or NULL. Caller owns it. NULL means either "no such style" or "the
 *  description column is NULL" -- the two are not distinguishable here, and never were. */
char *dt_style_repository_get_description(const int32_t styleid);

/** the style's `iop_list` column as stored, or NULL when the style has none (or does not exist).
 *  That is also the answer to "does this style carry a module order?". Caller owns it. */
char *dt_style_repository_get_iop_list(const char *name);

/** insert a header row, taking the next free id. `iop_list_txt` may be NULL. */
gboolean dt_style_repository_insert_header(const char *name, const char *description,
                                           const char *iop_list_txt);

/** replace a style's module order; a NULL `iop_list_txt` clears the column */
gboolean dt_style_repository_set_iop_list(const int32_t styleid, const char *iop_list_txt);

/** rename and re-describe an existing style */
gboolean dt_style_repository_update_header(const int32_t styleid, const char *newname,
                                           const char *description);

/** delete the header row and every item under it */
gboolean dt_style_repository_delete(const int32_t styleid);

/** one `data.styles` row */
typedef void (*dt_style_repository_style_cb)(void *user_data, const char *name,
                                             const char *description);

/** every style whose name or description matches `filter` (a bare substring -- this module wraps
 *  it in the LIKE wildcards), in name order */
void dt_style_repository_foreach_style(const char *filter, dt_style_repository_style_cb cb,
                                       void *user_data);

/** the one style with this exact name; FALSE if there is none */
gboolean dt_style_repository_get_style(const char *name, dt_style_repository_style_cb cb,
                                       void *user_data);

/* ---------------------------------------------------------------------------------------------
 * data.style_items
 * ------------------------------------------------------------------------------------------ */

/** one item row.
 *
 *  `num` is -1 when the column is SQL NULL. `selimg_num` is -1 unless the query was asked to
 *  match the style against an image, in which case it carries that image's own history num for
 *  the same module, or -1 where the image does not have it.
 *
 *  `params` / `blendop_params` are NULL for the listing queries that do not select them. Where
 *  they are not NULL they point INTO the statement and die with the row -- copy them in the
 *  callback.
 *
 *  `iop_order` is read from the same column as `blendop_version`, because that is what the
 *  original query did; see dt_style_repository_foreach_item_with_params(). */
typedef void (*dt_style_repository_item_cb)(void *user_data,
                                            const int num,
                                            const int multi_priority,
                                            const int module_version,
                                            const char *operation,
                                            const int enabled,
                                            const void *params,
                                            const int32_t params_size,
                                            const void *blendop_params,
                                            const int32_t blendop_params_size,
                                            const int blendop_version,
                                            const char *multi_name,
                                            const int selimg_num,
                                            const double iop_order);

/** every item of the style with its parameter blobs, ordered (num, operation, multi_priority) --
 *  the order applying a style walks them in */
void dt_style_repository_foreach_apply_item(const int32_t styleid,
                                            dt_style_repository_item_cb cb, void *user_data);

/** every item of the style with its parameter blobs, in whatever order SQLite returns them --
 *  writing a style to its .dtstyle file, which restores by num and does not depend on the order.
 *  Deliberately a separate query from foreach_apply_item(): same columns, no ORDER BY. */
void dt_style_repository_foreach_item_for_export(const int32_t styleid,
                                                 dt_style_repository_item_cb cb, void *user_data);

/** every item of the style with its parameter blobs, highest num first.
 *
 *  NOTE this query selects `blendop_version` as its last column and the original read that same
 *  column BOTH as blendop_version (int) and as iop_order (double). Preserved verbatim: the
 *  callback receives it twice, once each way. */
void dt_style_repository_foreach_item_with_params(const int32_t styleid,
                                                  dt_style_repository_item_cb cb, void *user_data);

/** every item of the style, no blobs, highest num first -- for listing it in the GUI.
 *
 *  NOTE this query selects only 8 columns, so there is no `blendop_version`/`iop_order` column at
 *  all; both come back 0, which is what the original read out of range. Preserved verbatim. */
void dt_style_repository_foreach_item(const int32_t styleid, dt_style_repository_item_cb cb,
                                      void *user_data);

/** the style's items UNION the image's own enabled history items that the style does not cover,
 *  so the GUI can show what applying it would replace and what it would add. No blobs; each row
 *  carries `selimg_num`. */
void dt_style_repository_foreach_item_against_image(const int32_t styleid, const int32_t imgid,
                                                    dt_style_repository_item_cb cb,
                                                    void *user_data);

/** copy an image's history into a style as items. `nums` is a GList of history nums to include
 *  (as GINT_TO_POINTER); NULL takes the whole history. */
gboolean dt_style_repository_copy_items_from_history(const int32_t styleid, const int32_t imgid,
                                                     GList *nums);

/** copy another style's items. `nums` selects which, as above; NULL takes all of them. */
gboolean dt_style_repository_copy_items_from_style(const int32_t dest_styleid,
                                                   const int32_t source_styleid, GList *nums);

/** drop every item whose num is NOT in `nums` */
gboolean dt_style_repository_delete_items_except(const int32_t styleid, GList *nums);

/** overwrite one style item from one of an image's history items.
 *
 *  These two are the only writes here that interpolate their integers into the query text rather
 *  than binding them, because that is what the originals did and a move is the wrong place to
 *  rewrite a query. They go through sqlite3_exec(), which reports nothing back, hence void.
 *  Every interpolated value is an int, so there is nothing to escape. */
void dt_style_repository_update_item_from_history(const int32_t styleid, const int item_num,
                                                  const int32_t imgid, const int history_num);

/** append one of an image's history items to the style, numbered after its current last item */
void dt_style_repository_append_item_from_history(const int32_t styleid, const int32_t imgid,
                                                  const int history_num);

/** insert one fully specified item -- what reading a style back from its XML file does */
gboolean dt_style_repository_insert_item(const int32_t styleid, const int num,
                                         const int module_version, const char *operation,
                                         const void *params, const int32_t params_size,
                                         const int enabled, const void *blendop_params,
                                         const int32_t blendop_params_size,
                                         const int blendop_version, const int multi_priority,
                                         const char *multi_name);

/** give every item a unique multi_priority per operation, numbered from 0 in multi_priority
 *  order. SQLite has no ROW_NUMBER, so this reads the rows and writes them back one by one. */
gboolean dt_style_repository_normalize_multi_priority(const int32_t styleid);

/** finalise every cached statement. Must run before the connection closes. */
void dt_style_repository_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif // DT_DATABASE_STYLE_REPOSITORY_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
