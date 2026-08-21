/*
    This file is part of darktable,
    Copyright (C) 2009-2011 johannes hanika.
    Copyright (C) 2010 Henrik Andersson.
    Copyright (C) 2011, 2014-2016 Tobias Ellinghaus.
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

#ifndef DT_DATABASE_HISTORY_REPOSITORY_H
#define DT_DATABASE_HISTORY_REPOSITORY_H

#include <glib.h>
#include <inttypes.h>

#include "common/image.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The `main.history`, `main.masks_history`, `main.module_order` and `main.history_hash` tables,
 *  plus the two auto-apply queries that read `data.presets` / `main.legacy_presets` to seed a
 *  new image's history.
 *
 *  The hot statements (the per-image read/write cycle) are prepared once, cached, and used
 *  only under this file's statement mutex; `dt_history_repository_cleanup()` finalises them,
 *  which the connection must be able to do before it closes. The rest prepare and finalise
 *  per call. Rows never leave as a `sqlite3_stmt` -- the foreach families decode each row
 *  into scalars and hand those to a callback.
 *
 *  @warning dt_history_repository_foreach_row() and
 *  dt_history_repository_foreach_auto_preset_row() step a CACHED statement and therefore
 *  hold the statement mutex across every callback invocation. The mutex is not recursive:
 *  a callback that calls back into this repository deadlocks its own thread. Today's
 *  callbacks only decode; keep it that way, or move those two walks to per-call statements
 *  first.
 */

/* ---------------------------------------------------------------------------------------------
 * history_end (main.images.history_end)
 * ------------------------------------------------------------------------------------------ */

/** read history_end for an image; 0 for an invalid imgid or a NULL column */
int32_t dt_history_repository_get_end(const int32_t imgid);

/** write history_end for an image */
gboolean dt_history_repository_set_end(const int32_t imgid, const int32_t history_end);

/* ---------------------------------------------------------------------------------------------
 * writing history items
 * ------------------------------------------------------------------------------------------ */

/** the num a new item should take: MAX(num) + 1, or 0 on an empty history */
int32_t dt_history_repository_get_next_num(const int32_t imgid);

/** shift every item's num by delta (used when inserting ahead of an existing history) */
gboolean dt_history_repository_shift_nums(const int32_t imgid, const int delta);

/** insert-or-update one history item, keyed on (imgid, num) */
gboolean dt_history_repository_write_item(const int32_t imgid, const int num, const char *operation,
                                          const void *op_params, const int op_params_size,
                                          const int module_version, const gboolean enabled,
                                          const void *blendop_params, const int blendop_params_size,
                                          const int blendop_version, const int multi_priority,
                                          const char *multi_name);

/* ---------------------------------------------------------------------------------------------
 * deleting
 * ------------------------------------------------------------------------------------------ */

gboolean dt_history_repository_delete_history(const int32_t imgid);
gboolean dt_history_repository_delete_masks_history(const int32_t imgid);

/** history + masks_history, the pair a development is made of */
gboolean dt_history_repository_delete_dev_history(const int32_t imgid);

/** everything an image's development is stored in: history, module_order, masks_history,
 *  history_hash, and the history_end / aspect_ratio reset on the image row itself. */
gboolean dt_history_repository_delete_all_for_image(const int32_t imgid);

/* ---------------------------------------------------------------------------------------------
 * reading history rows
 * ------------------------------------------------------------------------------------------ */

/** one stored history item, or one auto-apply preset presented as if it were one.
 *  `preset_name` is "" for a real history row and the preset's name for an auto-apply row. */
typedef void (*dt_history_repository_row_cb)(void *user_data,
                                             const int32_t imgid,
                                             const int num,
                                             const int module_version,
                                             const char *operation,
                                             const void *op_params,
                                             const int op_params_len,
                                             const gboolean enabled,
                                             const void *blendop_params,
                                             const int blendop_params_len,
                                             const int blendop_version,
                                             const int multi_priority,
                                             const char *multi_name,
                                             const char *preset_name);

/** every row of an image's history, in num order */
void dt_history_repository_foreach_row(const int32_t imgid, dt_history_repository_row_cb cb,
                                       void *user_data);

/** every auto-apply preset matching this image, plus the named workflow preset, in the order
 *  they must be applied. Reads `data.presets`, or `main.legacy_presets` when the image predates
 *  DT_IMAGE_NO_LEGACY_PRESETS -- a different table, hence a second cached statement. */
void dt_history_repository_foreach_auto_preset_row(const int32_t imgid, const struct dt_image_t *image,
                                                   const char *workflow_preset, const int iformat,
                                                   const int excluded, dt_history_repository_row_cb cb,
                                                   void *user_data);

/** the auto-applied 'ioporder' preset's params, if any. Caller owns *params. */
gboolean dt_history_repository_get_autoapply_ioporder_params(const int32_t imgid,
                                                             const struct dt_image_t *image,
                                                             const int iformat, const int excluded,
                                                             void **params, int32_t *params_len);

/** how many history items an image has -- 0 means "never developed", which is what the
 *  "altered" indicator and the embedded-JPEG-vs-raw thumbnail decision both read */
int dt_history_repository_count_items(const int32_t imgid);

/** the params blob and enabled flag of the LAST history item for `operation`, or FALSE if the
 *  image has none. `*params` is a copy the caller owns; only written when the item exists AND
 *  is enabled, because that is the only case its one consumer acts on. */
gboolean dt_history_repository_get_last_enabled_params(const int32_t imgid, const char *operation,
                                                       void **params, int32_t *params_len);

/** is this module present in the image's stored history at all? */
gboolean dt_history_repository_module_exists(const int32_t imgid, const char *operation);

/* ---------------------------------------------------------------------------------------------
 * listing history items for display
 * ------------------------------------------------------------------------------------------ */

/** one row of the two listing queries below. The repository does not name modules -- turning an
 *  `operation` into something a user reads is develop/'s job, several layers up. */
typedef void (*dt_history_repository_item_cb)(void *user_data, const int num, const char *operation,
                                              const gboolean enabled, const char *multi_name);

/** the latest item per (operation, multi_priority) -- i.e. one row per module instance,
 *  carrying its final state. `enabled` FALSE also returns disabled items. */
void dt_history_repository_foreach_last_item(const int32_t imgid, const gboolean enabled,
                                             dt_history_repository_item_cb cb, void *user_data);

/** every stored item, newest first, duplicates included */
void dt_history_repository_foreach_item(const int32_t imgid, dt_history_repository_item_cb cb,
                                        void *user_data);

/* ---------------------------------------------------------------------------------------------
 * main.masks_history
 *
 * The drawn shapes a development refers to, one row per (history step, form). Persisted beside
 * main.history and read back with it, so it lives in the same repository.
 * ------------------------------------------------------------------------------------------ */

/** append one mask row. TRUE when the INSERT ran to completion.
 *
 *  Both callers act on this: common/exif.cc's add_mask_entry_to_db() marks the entry
 *  already-added on success (its "insert once" guard used to test sqlite3_step() against a
 *  value an INSERT cannot return, and never fired -- fixed alongside the extraction), and
 *  develop/masks/masks.c writes each form through it. */
gboolean dt_history_repository_write_mask_item(const int32_t imgid, const int num, const int formid,
                                               const int form, const char *name, const int version,
                                               const void *points, const int points_len,
                                               const int points_count, const void *source,
                                               const int source_len);

/** how many mask rows an image has */
int dt_history_repository_count_mask_items(const int32_t imgid);

/** one main.masks_history row. The two blobs point INTO the statement -- copy them here. */
typedef void (*dt_history_repository_mask_cb)(void *user_data, const int num, const int formid,
                                              const int form, const char *name, const int version,
                                              const void *points, const int points_len,
                                              const int points_count, const void *source,
                                              const int source_len);

/** every mask row of an image, in num order */
void dt_history_repository_foreach_mask_item(const int32_t imgid,
                                             dt_history_repository_mask_cb cb, void *user_data);

/**
 * @brief The module version some image's history recorded for this exact parameter blob.
 *
 * @details A preset with no version of its own can be dated by finding a history entry
 * anywhere in the library that carries byte-identical parameters -- that entry knows which
 * version wrote them. Searches every image, so the first match wins arbitrarily; the blobs
 * are equal, which is what makes that acceptable.
 *
 * @return the version, or 0 when no history entry matches.
 */
int dt_history_repository_find_version_for_params(const char *operation, const void *op_params,
                                                  const int op_params_size);

/** one enabled module of an image, as a reader wants to see it listed */
typedef void (*dt_history_repository_active_module_cb)(void *user_data, const int num,
                                                       const char *operation, const char *multi_name);

/**
 * @brief Every module an image has enabled, once each, in the order they run.
 *
 * @details Grouped by (operation, multi_name) and ordered by the lowest `num` of each group,
 * so a module edited ten times appears once, at the position of its first appearance.
 */
void dt_history_repository_foreach_active_module(const int32_t imgid,
                                                 dt_history_repository_active_module_cb cb,
                                                 void *user_data);

/* ------------------------------------------------------------------------------------------
 * main.module_order
 *
 * Which order the modules run in for one image: a version from ::dt_iop_order_t, plus -- when
 * that version is CUSTOM, or when the image carries multiple instances of a module -- the
 * serialized list itself. One row per image, or no row at all, which is a meaningful state:
 * it means nobody has chosen, and the caller picks a built-in default from the image's kind.
 * ------------------------------------------------------------------------------------------ */

/** @brief @p imgid's stored order version.
 *
 *  @return FALSE when the image has no row, leaving @p version untouched. That is the caller's
 *  cue to derive a default rather than trust a zero. */
gboolean dt_history_repository_get_module_order_version(const int32_t imgid, int *version);

/** @brief Whether @p imgid has a `main.module_order` row at all, whatever it says.
 *
 *  @details Same table and same predicate as
 *  dt_history_repository_get_module_order_version(); it just discards the value. */
gboolean dt_history_repository_has_module_order(const int32_t imgid);

/** @brief Whether @p imgid's row stores a serialized list, i.e. `iop_list IS NOT NULL`. */
gboolean dt_history_repository_has_custom_module_order(const int32_t imgid);

/** @brief One `main.module_order` row. */
typedef struct dt_module_order_row_t
{
  int version;      /**< a ::dt_iop_order_t value */
  char *iop_list;   /**< the serialized list, or NULL when the column is NULL. Caller owns it. */
} dt_module_order_row_t;

/** @brief Read @p imgid's row. FALSE when there is none, and @p row is then left zeroed. */
gboolean dt_history_repository_get_module_order(const int32_t imgid, dt_module_order_row_t *row);

/** @brief Release what dt_history_repository_get_module_order() allocated. */
void dt_module_order_row_cleanup(dt_module_order_row_t *row);

/** @brief Store @p imgid's order: @p version, and @p iop_list serialized or NULL.
 *
 *  @details Two statements, as the caller's own two-step was: INSERT OR REPLACE a bare row so
 *  that one exists, then UPDATE it. Kept that shape rather than collapsed into a single
 *  INSERT OR REPLACE carrying the values, because the row this replaces is deleted either way
 *  and the cascade that follows is worth leaving exactly where it was. */
gboolean dt_history_repository_set_module_order(const int32_t imgid, const int version,
                                                const char *iop_list);

/** finalise every cached statement. Must run before the connection closes. */
void dt_history_repository_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif // DT_DATABASE_HISTORY_REPOSITORY_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
