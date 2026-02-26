/*
    This file is part of the Ansel project.
    Copyright (C) 2025 Alynx Zhou.
    Copyright (C) 2025-2026 Aurélien PIERRE.
    
    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    
    Ansel is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    along with Ansel.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "common/history.h"
#include "common/history_merge.h"

#include <glib.h>

#pragma once

/**
 * @file develop/dev_history.h
 *
 * The `common/history.h` defines methods to handle histories from/to database.
 * They work out of any GUI or development stack, so they don't care about modules .so.
 * This file defines binders between that and the GUI/dev objects.
 *
 */

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DT_IOP_PARAMS_T
#define DT_IOP_PARAMS_T
typedef void dt_iop_params_t;
#endif

struct dt_iop_module_t;
struct dt_develop_blend_params_t;
struct dt_develop_t;

typedef struct dt_dev_history_item_t
{
  struct dt_iop_module_t *module; // pointer to image operation module
  gboolean enabled;               // switched respective module on/off
  dt_iop_params_t *params;        // parameters for this operation
  struct dt_develop_blend_params_t *blend_params;
  char op_name[32];
  int iop_order;
  int multi_priority;
  char multi_name[128];
  GList *forms; // snapshot of dt_develop_t->forms
  int num; // num of history on database

  uint64_t hash; // module params hash.
} dt_dev_history_item_t;


/**
 * @brief Free the whole history list attached to dev->history.
 *
 * Frees each history item and clears the list pointer.
 *
 * @param dev Develop context.
 */
void dt_dev_history_free_history(struct dt_develop_t *dev);

/**
 * @brief Free a single history item (used as GList free callback).
 *
 * @param data Pointer to @ref dt_dev_history_item_t.
 */
void dt_dev_free_history_item(gpointer data);

/**
 * @brief Fill/refresh a history item from explicit params and apply them to the module.
 *
 * This helper exists to share code between regular history edits and history merge logic.
 * It will:
 * - allocate `hist->params` and `hist->blend_params` if needed,
 * - free and replace `hist->forms` with `forms` (ownership transferred),
 * - sync history metadata from `module` (op_name, multi_name, iop_order, multi_priority),
 * - copy params/blend_params into the history buffers (params size is clamped),
 * - apply params/blend_params to the module and recompute the hash.
 *
 * @param dev          develop context (currently unused, reserved for future).
 * @param hist         history item to update (must be non-NULL).
 * @param module       destination module instance (must be non-NULL).
 * @param enabled      enabled state to store/apply.
 * @param params       params buffer (if NULL, uses `module->params`).
 * @param params_size  size of @p params in bytes (ignored if params is NULL).
 * @param blend_params blend params buffer (if NULL, uses `module->blend_params`).
 * @param forms        mask forms snapshot for this history item (ownership transferred; may be NULL).
 * @return TRUE on success, FALSE on allocation failure/invalid args.
 */
/**
 * @brief Populate a history item from module parameters and recompute hashes.
 *
 * This allocates the history buffers if needed, copies params/blend params,
 * assigns module metadata (op name, instance data), applies the values back
 * to the module to keep hashes in sync, and computes the history hash using
 * the provided mask snapshot.
 *
 * @param dev Develop context (currently unused, reserved for future use).
 * @param hist History item to update.
 * @param module Destination module instance.
 * @param enabled Enabled state to store.
 * @param params Optional params buffer (NULL uses module->params).
 * @param params_size Size of @p params in bytes (ignored if params is NULL).
 * @param blend_params Optional blend params buffer (NULL uses module->blend_params).
 * @param forms Mask snapshot to attach (ownership transferred, may be NULL).
 * @return TRUE on success, FALSE on allocation/argument failure.
 */
gboolean dt_dev_history_item_update_from_params(struct dt_develop_t *dev, dt_dev_history_item_t *hist,
                                                struct dt_iop_module_t *module, gboolean enabled,
                                                const void *params, const int32_t params_size,
                                                const struct dt_develop_blend_params_t *blend_params, GList *forms);

/**
 * @brief Return the next available multi_priority for an operation.
 *
 * @param dev Develop context.
 * @param op Operation name.
 * @return Next available instance priority (>= 1).
 */
int dt_dev_next_multi_priority_for_op(struct dt_develop_t *dev, const char *op);

/**
 * @brief Find a module instance by op name and instance metadata.
 *
 * Tries multi_name first, then falls back to matching multi_priority.
 *
 * @param dev Develop context.
 * @param op Operation name.
 * @param multi_name Instance name (may be NULL/empty).
 * @param multi_priority Instance priority.
 * @return Matching module instance or NULL.
 */
struct dt_iop_module_t *dt_dev_get_module_instance(struct dt_develop_t *dev, const char *op, const char *multi_name,
                                                   const int multi_priority);

/**
 * @brief Create a new module instance from an existing base .so.
 *
 * @param dev Develop context.
 * @param op Operation name.
 * @param multi_name Instance name (may be NULL/empty).
 * @param multi_priority Instance priority.
 * @param use_next_priority If TRUE, auto-pick the next priority for this op.
 * @return New module instance or NULL on failure.
 */
struct dt_iop_module_t *dt_dev_create_module_instance(struct dt_develop_t *dev, const char *op, const char *multi_name,
                                                      const int multi_priority, gboolean use_next_priority);
/**
 * @brief Copy params/blend params from one module instance to another.
 *
 * Optionally copies the drawn masks used by @p mod_src from @p dev_src into
 * @p dev_dest (if @p dev_src is non-NULL).
 *
 * @param dev_dest Destination develop context.
 * @param dev_src Source develop context (may be NULL to skip mask copy).
 * @param mod_dest Destination module instance.
 * @param mod_src Source module instance.
 * @return 0 on success, non-zero on allocation failure.
 */
int dt_dev_copy_module_contents(struct dt_develop_t *dev_dest, struct dt_develop_t *dev_src,
                                struct dt_iop_module_t *mod_dest, const struct dt_iop_module_t *mod_src);

/**
 * @brief Create a history item from another history item, using a destination module.
 *
 * Copies params/blend params, updates module ordering metadata, and
 * snapshots masks if needed by the source module.
 *
 * @param dev_dest Destination develop context (receives masks).
 * @param dev_src Source develop context (provides masks).
 * @param hist_src Source history item.
 * @param mod_dest Destination module instance.
 * @param out_hist Output history item (allocated on success).
 * @return 0 on success, non-zero on allocation failure.
 */
int dt_dev_history_item_from_source_history_item(struct dt_develop_t *dev_dest, struct dt_develop_t *dev_src,
                                                 const struct dt_dev_history_item_t *hist_src,
                                                 struct dt_iop_module_t *mod_dest,
                                                 struct dt_dev_history_item_t **out_hist);

/**
 * @brief Merge a list of modules into a destination image history via dt_history_merge().
 *
 * @param dev_src Source develop context (provides module params).
 * @param dest_imgid Destination image id.
 * @param mod_list List of module instances to merge.
 * @param merge_iop_order Whether to merge pipeline order (TRUE) or preserve destination (FALSE).
 * @param mode Merge strategy for history entries.
 * @param paste_instances Whether to paste module instances.
 * @return 0 on success, non-zero on failure.
 */
int dt_dev_merge_history_into_image(struct dt_develop_t *dev_src, int32_t dest_imgid, const GList *mod_list,
                                    gboolean merge_iop_order, const dt_history_merge_strategy_t mode,
                                    const gboolean paste_instances);
/**
 * @brief Replace an image history with the content of @p dev_src.
 *
 * Optionally reloads default modules before writing to DB. This is used
 * by history replace and style replace paths.
 *
 * @param dev_src Source develop context.
 * @param dest_imgid Destination image id.
 * @param reload_defaults Whether to reload default modules before writing.
 * @param msg Optional debug message.
 * @return 0 on success, non-zero on failure.
 */
int dt_dev_replace_history_on_image(struct dt_develop_t *dev_src, const int32_t dest_imgid,
                                    const gboolean reload_defaults, const char *msg);

/**
 * @brief Append or update a history item for a module.
 *
 * If the last history item matches the module and @p force_new_item is FALSE,
 * the existing item is reused. Otherwise a new entry is appended.
 * If history items exist after dev->history_end, they may be removed depending
 * on module rules (see dev_history.c).
 *
 * @param dev
 * @param module
 * @param enable
 * @param force_new_item
 * @return TRUE if the pipeline topology may need to be updated (new module node).
 */
gboolean dt_dev_add_history_item_ext(struct dt_develop_t *dev, struct dt_iop_module_t *module, gboolean enable,
                                     gboolean force_new_item);

/**
 * @brief Thread-safe wrapper around dt_dev_add_history_item_ext().
 *
 * Locks history mutex, invalidates pipelines, triggers recomputation and
 * schedules history auto-save. This is the typical entry point for GUI actions.
 *
 * @param dev Develop context.
 * @param module Module instance.
 * @param enable Enable state.
 * @param redraw Whether to force a GUI redraw.
 */
void dt_dev_add_history_item_real(struct dt_develop_t *dev, struct dt_iop_module_t *module, gboolean enable, gboolean redraw);

// Debug helper to follow calls to `dt_dev_add_history_item_real()`, but mostly to follow useless pipe recomputations.
#define dt_dev_add_history_item(dev, module, enable, redraw) DT_DEBUG_TRACE_WRAPPER(DT_DEBUG_DEV, dt_dev_add_history_item_real, (dev), (module), (enable), (redraw))


/**
 * @brief Write dev->history to DB and XMP for a given image id.
 *
 * This acquires the database lock in write mode.
 *
 * @param dev Develop context.
 * @param imgid Image id.
 */
void dt_dev_write_history_ext(struct dt_develop_t *dev, const int32_t imgid);

/**
 * @brief Thread-safe wrapper around dt_dev_write_history_ext() for dev->image_storage.id.
 *
 * @param dev Develop context.
 */
void dt_dev_write_history(struct dt_develop_t *dev);

/**
 * @brief Apply history-loaded params to module GUIs.
 *
 * Ensures module instances shown in the GUI match the history state.
 *
 * @param dev Develop context.
 */
void dt_dev_history_gui_update(struct dt_develop_t *dev);

/**
 * @brief Rebuild or resync pixelpipes after backend history changes.
 *
 * @param dev Develop context.
 * @param rebuild TRUE to rebuild pipeline topology, FALSE to resync only.
 */
void dt_dev_history_pixelpipe_update(struct dt_develop_t *dev, gboolean rebuild);

/**
 * @brief Notify the rest of the app that history changes were written.
 *
 * Updates thumbnails and emits user-visible notices when needed.
 *
 * @param dev Develop context.
 * @param imgid Image id.
 */
void dt_dev_history_notify_change(struct dt_develop_t *dev, const int32_t imgid);

/**
 * @brief Start an undo record for history changes.
 *
 * Called by the develop undo framework.
 *
 * @param dev Develop context.
 */
void dt_dev_history_undo_start_record(struct dt_develop_t *dev);
/**
 * @brief Finish an undo record for history changes.
 *
 * @param dev Develop context.
 */
void dt_dev_history_undo_end_record(struct dt_develop_t *dev);
/**
 * @brief Start an undo record with history_mutex already locked.
 *
 * Caller must hold dev->history_mutex (read or write).
 *
 * @param dev Develop context.
 */
void dt_dev_history_undo_start_record_locked(struct dt_develop_t *dev);
/**
 * @brief Finish an undo record with history_mutex already locked.
 *
 * Caller must hold dev->history_mutex (read or write).
 *
 * @param dev Develop context.
 */
void dt_dev_history_undo_end_record_locked(struct dt_develop_t *dev);
/**
 * @brief Invalidate a module pointer inside undo snapshots.
 *
 * Used when module instances are destroyed or replaced.
 *
 * @param module Module to invalidate.
 */
void dt_dev_history_undo_invalidate_module(struct dt_iop_module_t *module);

/**
 * @brief Read history and masks from DB and populate dev->history.
 *
 * Also loads default modules and auto-presets when needed. This initializes
 * module internals with the full history and does not honor history_end.
 * Call dt_dev_pop_history_items_ext() afterwards to apply history_end.
 *
 * @param dev Develop context.
 * @param imgid Image id.
 * @return TRUE if this is a newly-initialized history, FALSE otherwise.
 */
gboolean dt_dev_read_history_ext(struct dt_develop_t *dev, const int32_t imgid);

/**
 * @brief Apply history items to module params up to dev->history_end.
 *
 * Does not update the GUI; see dt_dev_pop_history_items() for GUI-aware calls.
 *
 * @param dev Develop context.
 */
void dt_dev_pop_history_items_ext(struct dt_develop_t *dev);

/**
 * @brief Thread-safe wrapper around dt_dev_pop_history_items_ext(), then update GUI.
 *
 * @param dev Develop context.
 */
void dt_dev_pop_history_items(struct dt_develop_t *dev);


/**
 * @brief Reload history from DB and rebuild pipelines/GUI state.
 *
 * Frees existing history, re-reads from DB, applies to modules,
 * and updates GUI and pipelines. Locks history mutex.
 *
 * @param dev Develop context.
 * @param imgid Image id.
 */
void dt_dev_reload_history_items(struct dt_develop_t *dev, const int32_t imgid);


/**
 * @brief Remove a module pointer from a history list.
 *
 * Used when modules are deleted or re-instantiated.
 *
 * @param list History list.
 * @param module Module to invalidate.
 */
void dt_dev_invalidate_history_module(GList *list, struct dt_iop_module_t *module);

/**
 * @brief Get the integrity checksum of the whole history stack.
 * This should be done ONLY when history is changed, read or written.
 *
 * @param dev
 * @return uint64_t
 */
uint64_t dt_dev_history_get_hash(struct dt_develop_t *dev);

/**
 * @brief Get the current history end index (GUI perspective).
 *
 * The index is 1-based with 0 representing the raw input image. The value is
 * sanitized against the actual history length.
 *
 * @param dev Develop context.
 * @return History end index.
 */
int32_t dt_dev_get_history_end_ext(struct dt_develop_t *dev);

/**
 * @brief Set the history end index (GUI perspective).
 *
 * The index is 1-based with 0 representing the raw input image. The value is
 * sanitized against the actual history length.
 *
 * @param dev Develop context.
 * @param index New history end index.
 */
void dt_dev_set_history_end_ext(struct dt_develop_t *dev, const uint32_t index);

/**
 * @brief Determine whether a module should be skipped during history copy.
 *
 * Evaluates module flags such as deprecated/unsafe/hidden.
 *
 * @param flags Module flags.
 * @return TRUE if module should be skipped, FALSE otherwise.
 */
gboolean dt_history_module_skip_copy(const int flags);

/**
 * @brief Merge a single module instance into a destination history.
 *
 * Creates or reuses a destination module instance and copies its parameters.
 * This does not resync the pipeline or pop history; callers should batch
 * multiple merges and resync once.
 *
 * @param dev_dest Destination develop context.
 * @param dev_src Source develop context (may be NULL to skip mask copy).
 * @param mod_src Source module instance.
 * @return 1 on success, 0 on failure.
 */
int dt_history_merge_module_into_history(struct dt_develop_t *dev_dest, struct dt_develop_t *dev_src,
                                         struct dt_iop_module_t *mod_src);

/**
 * @brief Compress an history from a loaded pipeline,
 * aka simply take a snapshot of all modules parameters.
 * This assumes the history end is properly set, which always happens
 * after calling _pop_history_item.
 * @param dev
 */
void dt_dev_history_compress(struct dt_develop_t *dev);
/**
 * @brief Variant of history compression that optionally skips DB writeback.
 *
 * @param dev Develop context.
 * @param write_history If TRUE, write history to DB/XMP after compression.
 */
void dt_dev_history_compress_ext(struct dt_develop_t *dev, gboolean write_history);
/**
 * @brief Compress history if history_end is at top, otherwise truncate.
 *
 * @param dev Develop context.
 */
void dt_dev_history_compress_or_truncate(struct dt_develop_t *dev);
/**
 * @brief Cleanup cached statements or state used by history I/O.
 */
void dt_dev_history_cleanup(void);

/**
 * @brief Initialize module defaults and insert required default modules.
 *
 * This does not read the database history. It only loads defaults and
 * optionally applies auto-presets, mirroring the internal init path used
 * by dt_dev_read_history_ext().
 *
 * @param dev Develop context.
 * @param imgid Image id.
 * @param apply_auto_presets Whether to apply auto-presets.
 * @return TRUE if this was the first initialization for the image.
 */
gboolean dt_dev_init_default_history(struct dt_develop_t *dev, const int32_t imgid, gboolean apply_auto_presets);

/**
 * @brief Find the first history item referencing a module.
 *
 * @param history_list History list.
 * @param module Module instance.
 * @return First matching history item or NULL.
 */
dt_dev_history_item_t *dt_dev_history_get_first_item_by_module(GList *history_list, struct dt_iop_module_t *module);

/**
 * @brief Find the last history item referencing a module up to history_end.
 *
 * @param history_list History list.
 * @param module Module instance.
 * @param history_end Upper bound index (GUI perspective).
 * @return Last matching history item or NULL.
 */
dt_dev_history_item_t *dt_dev_history_get_last_item_by_module(GList *history_list, struct dt_iop_module_t *module, int history_end);

/**
 * @brief Refresh GUI module nodes to match history state.
 *
 * Removes modules without history, creates missing instances, and reorders
 * the GUI list according to history/pipeline ordering.
 *
 * @param dev Develop context.
 * @param iop Module list.
 * @param history History list.
 * @return 0 on success, non-zero on error.
 */
int dt_dev_history_refresh_nodes_ext(struct dt_develop_t *dev, GList *iop, GList *history);

/** truncate history stack */
void dt_dev_history_truncate(struct dt_develop_t *dev, const int32_t imgid);

#ifdef __cplusplus
}
#endif
