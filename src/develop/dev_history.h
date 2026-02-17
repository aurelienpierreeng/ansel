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


/** Free the whole GList of `dt_dev_history_item_t` attached to dev->history  */
void dt_dev_history_free_history(struct dt_develop_t *dev);

/** Free a single GList *link containing a `dt_dev_history_item_t` */
void dt_dev_free_history_item(gpointer data);

/**
 * @brief Append a new history item on dev->history, at dev->history_end position.
 * If history items exist after dev->history_end, they will be removed under certain conditions.
 *
 * @param dev
 * @param module
 * @param enable
 * @param force_new_item
 * @param no_image
 * @param include_masks
 * @return gboolean TRUE if the pipeline topology may need to be updated, aka new module node inserted
 */
gboolean dt_dev_add_history_item_ext(struct dt_develop_t *dev, struct dt_iop_module_t *module, gboolean enable, gboolean force_new_item,
                                     gboolean no_image, gboolean include_masks);

// Locks dev->history_mutex, calls `dt_dev_add_history_item_ext()`, invalidates darkroom pipelines,
// triggers pipe recomputation and queue an history auto-save for the next 15 seconds.
void dt_dev_add_history_item_real(struct dt_develop_t *dev, struct dt_iop_module_t *module, gboolean enable, gboolean redraw);

// Debug helper to follow calls to `dt_dev_add_history_item_real()`, but mostly to follow useless pipe recomputations.
#define dt_dev_add_history_item(dev, module, enable, redraw) DT_DEBUG_TRACE_WRAPPER(DT_DEBUG_DEV, dt_dev_add_history_item_real, (dev), (module), (enable), (redraw))


// Locks darktable.database_threadsafe in write mode,
// write dev->history GList into DB and XMP
void dt_dev_write_history_ext(struct dt_develop_t *dev, const int32_t imgid);

// Locks dev->history_mutex and calls dt_dev_write_history_ext()
void dt_dev_write_history(struct dt_develop_t *dev);

// Locks darktable.database_threadsafe in read mode,
// get history (module params) and masks from DB,
// apply default modules, auto-presets and mandatory modules,
// then populate dev->history GList.
void dt_dev_read_history_ext(struct dt_develop_t *dev, const int32_t imgid, gboolean no_image);;

// Read dev->history state, up to dev->history_end,
// and write it into the params/blendops of modules from dev->iop.
// dev->history_end should be set before, see `dt_dev_set_history_end()`.
// This doesn't update GUI. See `dt_dev_pop_history_items()`
void dt_dev_pop_history_items_ext(struct dt_develop_t *dev);

// Locks dev->history_mutex and calls `dt_dev_pop_history_items_ext()`
// Then update module GUI
void dt_dev_pop_history_items(struct dt_develop_t *dev);


// Free exisiting history, re-read it from database, update GUI and rebuild darkroom pipeline nodes.
// Locks dev->history_mutex
void dt_dev_reload_history_items(struct dt_develop_t *dev);


// Removes the reference to `*module` from history entries
// FIXME: why is that needed ?
void dt_dev_invalidate_history_module(GList *list, struct dt_iop_module_t *module);

/**
 * @brief Get the integrity checksum of the whole history stack
 *
 * @param dev
 * @return uint64_t
 */
uint64_t dt_dev_history_get_hash(struct dt_develop_t *dev);

/**
 * @brief Write history to DB and XMP only if the integrety hash has changed since
 * first reading history, or since prior saving point.
 * Callback function meant to be used with g_timeout, or standalone.
 *
 * @param dev
 * @return int
 */
int dt_dev_history_auto_save(struct dt_develop_t *dev);


// We allow pipelines to run partial histories, up to a certain index
// stored privately in dev->history_end. Use these getter/setters
// that will check validity, instead of directly reading/writing the private data.

// Get the index of the last active history element from a GUI perspective.
// It means that dev->history_end is shifted by a +1 offset, so index 0 is the raw image,
// therefore outside of the actual dev->history list, then dev->history_end = 1 is
// actually the first element of history, and dev->history_end = length(dev->history) is the last.
// Note: the value is sanitized with the actual history size.
// It needs to run after dev->history is fully populated
int32_t dt_dev_get_history_end(struct dt_develop_t *dev);

// Set the index of the last active history element from a GUI perspective.
// It means that dev->history_end is shifted by a +1 offset, so index 0 is the raw image,
// therefore outside of the actual dev->history list, then dev->history_end = 1 is
// actually the first element of history, and dev->history_end = length(dev->history) is the last.
// Note: the value is sanitized with the actual history size.
// It needs to run after dev->history is fully populated
void dt_dev_set_history_end(struct dt_develop_t *dev, const uint32_t index);

gboolean dt_history_module_skip_copy(const int flags);

/** adds to dev_dest module mod_src */
int dt_history_merge_module_into_history(struct dt_develop_t *dev_dest, struct dt_develop_t *dev_src, struct dt_iop_module_t *mod_src, GList **_modules_used);


/** copy history from imgid and pasts on dest_imgid, merge or overwrite... */
int dt_history_copy_and_paste_on_image(int32_t imgid, int32_t dest_imgid, GList *ops, gboolean copy_iop_order, const gboolean copy_full);


/**
 * @brief Compress an history from a loaded pipeline,
 * aka simply take a snapshot of all modules parameters.
 * This assumes the history end is properly set, which always happens
 * after calling _pop_history_item.
 * @param dev
 */
void dt_dev_history_compress(struct dt_develop_t *dev);

/**
 * @brief Find the first history entry on dev->history attached to a module
 * 
 * @param dev 
 * @param module 
 * @return dt_dev_history_item_t* 
 */
dt_dev_history_item_t *dt_dev_history_get_first_item_by_module(struct dt_develop_t *dev, struct dt_iop_module_t *module);


#ifdef __cplusplus
}
#endif
