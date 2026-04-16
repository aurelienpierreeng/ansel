/*
    This file is part of darktable,
    Copyright (C) 2009-2015, 2018 johannes hanika.
    Copyright (C) 2010 Alexandre Prokoudine.
    Copyright (C) 2010-2011 Bruce Guenter.
    Copyright (C) 2010-2012 Henrik Andersson.
    Copyright (C) 2011 Karl Mikaelsson.
    Copyright (C) 2011 Mikko Ruohola.
    Copyright (C) 2011 Omari Stephens.
    Copyright (C) 2011 Robert Bieber.
    Copyright (C) 2011 Rostyslav Pidgornyi.
    Copyright (C) 2011-2019 Tobias Ellinghaus.
    Copyright (C) 2012-2014, 2016, 2020-2021 Aldric Renaudin.
    Copyright (C) 2012 Antony Dovgal.
    Copyright (C) 2012 Moritz Lipp.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012-2014, 2016-2017 Ulrich Pegelow.
    Copyright (C) 2013-2022 Pascal Obry.
    Copyright (C) 2014, 2020 Dan Torop.
    Copyright (C) 2014 parafin.
    Copyright (C) 2014-2015 Pedro Côrte-Real.
    Copyright (C) 2014-2017 Roman Lebedev.
    Copyright (C) 2016 Alexander V. Smal.
    Copyright (C) 2017, 2021 luzpaz.
    Copyright (C) 2018-2019 Edgardo Hoszowski.
    Copyright (C) 2019 Alexander Blinne.
    Copyright (C) 2019-2020, 2022-2026 Aurélien PIERRE.
    Copyright (C) 2019-2021 Diederik Ter Rahe.
    Copyright (C) 2019-2022 Hanno Schwalm.
    Copyright (C) 2019 Heiko Bauke.
    Copyright (C) 2019-2020 Philippe Weyland.
    Copyright (C) 2020-2021 Chris Elston.
    Copyright (C) 2020 GrahamByrnes.
    Copyright (C) 2020 Harold le Clément de Saint-Marcq.
    Copyright (C) 2020 Hubert Kowalski.
    Copyright (C) 2020 JP Verrue.
    Copyright (C) 2020-2021 Ralf Brown.
    Copyright (C) 2021 paolodepetrillo.
    Copyright (C) 2021 Sakari Kapanen.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2023 Alynx Zhou.
    Copyright (C) 2023 lologor.
    Copyright (C) 2023 Luca Zulberti.
    Copyright (C) 2023 Ricky Moon.
    Copyright (C) 2025-2026 Guillaume Stutin.
    Copyright (C) 2025 Miguel Moquillon.
    
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
#include "common/darktable.h"
#include "common/history.h"
#include "common/undo.h"
#include "common/history_snapshot.h"
#include "common/image_cache.h"
#include "common/history_merge.h"
#include "common/iop_order.h"
#include "develop/dev_history.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/masks.h"

#include "gui/presets.h"

#include <inttypes.h>

#include <glib.h>

static void _process_history_db_entry(dt_develop_t *dev, const int32_t imgid, const int id, const int num,
                                      const int modversion, const char *operation, const void *module_params,
                                      const int param_length, const gboolean enabled, const void *blendop_params,
                                      const int bl_length, const int blendop_version, const int multi_priority,
                                      const char *multi_name, const char *preset_name, int *legacy_params,
                                      const gboolean presets);

typedef struct dt_dev_history_db_ctx_t
{
  dt_develop_t *dev;
  int32_t imgid;
  int *legacy_params;
  gboolean presets;
} dt_dev_history_db_ctx_t;

gboolean dt_dev_init_default_history(dt_develop_t *dev, const int32_t imgid, gboolean apply_auto_presets);

/**
 * @brief Adapter callback for history DB rows.
 *
 * Converts DB row fields into a history item and appends it to dev->history.
 *
 * @param user_data Context pointer (dt_dev_history_db_ctx_t).
 * @param id Image id from DB row.
 * @param num History index.
 * @param modversion Module version stored in DB.
 * @param operation Operation name.
 * @param module_params Module params blob.
 * @param param_length Params blob length.
 * @param enabled Enabled flag from DB.
 * @param blendop_params Blend params blob.
 * @param bl_length Blend blob length.
 * @param blendop_version Blend params version.
 * @param multi_priority Instance priority.
 * @param multi_name Instance name.
 * @param preset_name Optional preset name (for auto presets).
 */
static void _dev_history_db_row_cb(void *user_data, const int32_t id, const int num, const int modversion,
                                   const char *operation, const void *module_params, const int param_length,
                                   const gboolean enabled, const void *blendop_params, const int bl_length,
                                   const int blendop_version, const int multi_priority, const char *multi_name,
                                   const char *preset_name)
{
  dt_dev_history_db_ctx_t *ctx = (dt_dev_history_db_ctx_t *)user_data;
  _process_history_db_entry(ctx->dev, ctx->imgid, id, num, modversion, operation, module_params, param_length, enabled,
                            blendop_params, bl_length, blendop_version, multi_priority, multi_name, preset_name,
                            ctx->legacy_params, ctx->presets);
}

// returns the first history item with hist->module == module
dt_dev_history_item_t *dt_dev_history_get_first_item_by_module(GList *history_list, dt_iop_module_t *module)
{
  dt_dev_history_item_t *hist_mod = NULL;
  for(GList *history = g_list_first(history_list); history; history = g_list_next(history))
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);

    if(hist->module == module)
    {
      hist_mod = hist;
      break;
    }
  }
  return hist_mod;
}

dt_dev_history_item_t *dt_dev_history_get_last_item_by_module(GList *history_list, dt_iop_module_t *module, int history_end)
{
  dt_dev_history_item_t *hist_mod = NULL;
  for(GList *history = g_list_nth(history_list, history_end -1); history; history = g_list_previous(history))
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);

    if(hist->module == module)
    {
      hist_mod = hist;
      break;
    }
  }
  return hist_mod;
}

gboolean dt_dev_history_item_update_from_params(dt_develop_t *dev, dt_dev_history_item_t *hist, dt_iop_module_t *module,
                                               const gboolean enabled, const void *params, const int32_t params_size,
                                               const dt_develop_blend_params_t *blend_params, GList *forms)
{
  if(!hist || IS_NULL_PTR(module)) return FALSE;

  if(!hist->params)
  {
    hist->params = g_malloc0(module->params_size);
    if(IS_NULL_PTR(hist->params)) return FALSE;
  }

  if(!hist->blend_params)
  {
    hist->blend_params = g_malloc0(sizeof(dt_develop_blend_params_t));
    if(IS_NULL_PTR(hist->blend_params)) return FALSE;
  }

  if(hist->forms)
  {
    g_list_free_full(hist->forms, (void (*)(void *))dt_masks_free_form);
    hist->forms = NULL;
  }
  hist->forms = forms;

  module->enabled = enabled;
  hist->enabled = enabled;
  hist->module = module;
  hist->params_size = module->params_size;
  hist->module_version = module->version();
  hist->blendop_params_size = sizeof(dt_develop_blend_params_t);
  hist->blendop_version = dt_develop_blend_version();
  hist->iop_order = module->iop_order;
  hist->multi_priority = module->multi_priority;
  g_strlcpy(hist->op_name, module->op, sizeof(hist->op_name));
  g_strlcpy(hist->multi_name, module->multi_name, sizeof(hist->multi_name));

  const void *src_params = params ? params : module->params;
  const int32_t src_size = params ? params_size : module->params_size;
  const int32_t sz = MIN(module->params_size, src_size);
  if(!IS_NULL_PTR(src_params) && hist->params && sz > 0) memcpy(hist->params, src_params, sz);

  const dt_develop_blend_params_t *src_blend = blend_params ? blend_params : module->blend_params;
  if(src_blend && hist->blend_params) memcpy(hist->blend_params, src_blend, sizeof(dt_develop_blend_params_t));

  // Apply to module to keep the hash in sync.
  if(hist->params && module->params && module->params_size > 0)
    memcpy(module->params, hist->params, module->params_size);
  dt_iop_commit_blend_params(module, hist->blend_params);

  dt_iop_compute_module_hash(module, hist->forms);
  hist->hash = module->hash;

  return TRUE;
}

int dt_dev_next_multi_priority_for_op(dt_develop_t *dev, const char *op)
{
  int max_priority = 0;
  for(const GList *l = g_list_first(dev->iop); l; l = g_list_next(l))
  {
    const dt_iop_module_t *m = (const dt_iop_module_t *)l->data;
    if(strcmp(m->op, op)) continue;
    max_priority = MAX(max_priority, m->multi_priority);
  }
  return max_priority + 1;
}

dt_iop_module_t *dt_dev_get_module_instance(dt_develop_t *dev, const char *op, const char *multi_name,
                                            const int multi_priority)
{
  const char *name = (multi_name && *multi_name) ? multi_name : NULL;
  dt_iop_module_t *module = dt_iop_get_module_by_instance_name(dev->iop, op, name);
  if(IS_NULL_PTR(module) && (IS_NULL_PTR(name) || *name == '\0'))
    module = dt_iop_get_module_by_op_priority(dev->iop, op, multi_priority);
  if(IS_NULL_PTR(module) && (IS_NULL_PTR(name) || *name == '\0'))
    module = dt_iop_get_module_by_op_priority(dev->iop, op, 0);
  return module;
}

dt_iop_module_t *dt_dev_create_module_instance(dt_develop_t *dev, const char *op, const char *multi_name,
                                               const int multi_priority, gboolean use_next_priority)
{
  dt_iop_module_t *base = dt_iop_get_module_by_op_priority(dev->iop, op, 0);
  if(IS_NULL_PTR(base)) base = dt_iop_get_module_by_op_priority(dev->iop, op, -1);
  if(IS_NULL_PTR(base)) return NULL;

  if((base->flags() & IOP_FLAGS_ONE_INSTANCE) == IOP_FLAGS_ONE_INSTANCE)
    return base;

  dt_iop_module_t *module = (dt_iop_module_t *)calloc(1, sizeof(dt_iop_module_t));
  if(IS_NULL_PTR(module)) return NULL;

  if(dt_iop_load_module(module, base->so, dev))
  {
    fprintf(stderr, "[dt_dev_create_module_instance] can't load module %s\n", op);
    dt_free(module);
    return NULL;
  }

  module->instance = base->instance;
  module->enabled = FALSE;
  module->multi_priority = use_next_priority ? dt_dev_next_multi_priority_for_op(dev, op) : multi_priority;
  g_strlcpy(module->multi_name, multi_name ? multi_name : "", sizeof(module->multi_name));

  dev->iop = g_list_append(dev->iop, module);
  return module;
}

int dt_dev_copy_module_contents(dt_develop_t *dev_dest, dt_develop_t *dev_src, dt_iop_module_t *mod_dest,
                                const dt_iop_module_t *mod_src)
{
  mod_dest->enabled = mod_src->enabled;

  const int32_t sz_dest = mod_dest->params_size;
  const int32_t sz_src = mod_src->params_size;

  // If both param sizes don't match, we have a serious problem
  assert(sz_dest == sz_src);
  if(sz_dest != sz_src) return 1;

  const int32_t sz = MIN(sz_dest, sz_src);
  if(sz > 0) memcpy(mod_dest->params, mod_src->params, sz);

  if(mod_src->blend_params) dt_iop_commit_blend_params(mod_dest, mod_src->blend_params);

  if(dev_src && dt_masks_copy_used_forms_for_module(dev_dest, dev_src, mod_src)) return 1;
  return 0;
}

int dt_dev_history_item_from_source_history_item(dt_develop_t *dev_dest, dt_develop_t *dev_src,
                                                 const dt_dev_history_item_t *hist_src, dt_iop_module_t *mod_dest,
                                                 dt_dev_history_item_t **out_hist)
{
  if(IS_NULL_PTR(hist_src) || IS_NULL_PTR(hist_src->module) || IS_NULL_PTR(mod_dest) || IS_NULL_PTR(out_hist)) return 1;

  dt_dev_history_item_t *hist = (dt_dev_history_item_t *)calloc(1, sizeof(dt_dev_history_item_t));
  if(IS_NULL_PTR(hist)) return 1;

  if(dt_masks_copy_used_forms_for_module(dev_dest, dev_src, hist_src->module))
  {
    dt_dev_free_history_item(hist);
    return 1;
  }
  GList *forms_snapshot = NULL;
  if(dt_iop_module_needs_mask_history(hist_src->module))
  {
    forms_snapshot = dt_masks_snapshot_current_forms(dev_dest, FALSE);
    if(IS_NULL_PTR(forms_snapshot))
    {
      dt_dev_free_history_item(hist);
      return 1;
    }
  }

  if(!dt_dev_history_item_update_from_params(dev_dest, hist, mod_dest, hist_src->enabled, hist_src->params,
                                             hist_src->module->params_size, hist_src->blend_params, forms_snapshot))
  {
    dt_dev_free_history_item(hist);
    return 1;
  }

  *out_hist = hist;
  return 0;
}

int dt_dev_merge_history_into_image(dt_develop_t *dev_src, int32_t dest_imgid, const GList *mod_list,
                                    gboolean merge_iop_order, const dt_history_merge_strategy_t mode,
                                    const gboolean paste_instances)
{
  if(dest_imgid <= 0) return 1;
  if(IS_NULL_PTR(mod_list)) return 0;

  dt_develop_t dev_dest = { 0 };
  dt_dev_init(&dev_dest, FALSE);
  dt_dev_reload_history_items(&dev_dest, dest_imgid);

  const int ret_val = dt_history_merge(&dev_dest, dev_src, dest_imgid, mod_list, merge_iop_order, mode, paste_instances);

  if(ret_val == 0)
  {
    dt_dev_pop_history_items_ext(&dev_dest);
    dt_dev_write_history(&dev_dest, FALSE);
  }

  dt_dev_cleanup(&dev_dest);
  return ret_val;
}

/**
 * @brief Find the first history item matching a module operation name.
 *
 * @param dev Develop context.
 * @param module Module instance to match by op name.
 * @return First matching history item or NULL.
 */
static dt_dev_history_item_t *_search_history_by_op(dt_develop_t *dev, const dt_iop_module_t *module)
{
  dt_dev_history_item_t *hist_mod = NULL;
  for(GList *history = g_list_first(dev->history); history; history = g_list_next(history))
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);
    if(!hist || !hist->module) continue;

    if(strcmp(hist->module->op, module->op) == 0)
    {
      hist_mod = hist;
      break;
    }
  }
  return hist_mod;
}

/**
 * @brief Resolve or create the destination module instance for history merge.
 *
 * Attempts to reuse existing instances when possible, otherwise creates a new instance.
 *
 * @param dev_dest Destination develop context.
 * @param mod_src Source module instance.
 * @param created Output flag set TRUE if a new instance is created.
 * @param reused_base Output flag set TRUE if an existing base instance is reused.
 * @return Destination module instance or NULL on failure.
 */
static dt_iop_module_t *_history_merge_resolve_dest_instance(dt_develop_t *dev_dest,
                                                             const dt_iop_module_t *mod_src,
                                                             gboolean *created,
                                                             gboolean *reused_base)
{
  *created = FALSE;
  *reused_base = FALSE;

  if(mod_src->flags() & IOP_FLAGS_ONE_INSTANCE)
    return dt_iop_get_module_by_op_priority(dev_dest->iop, mod_src->op, -1);

  dt_iop_module_t *module
      = dt_dev_get_module_instance(dev_dest, mod_src->op, mod_src->multi_name, mod_src->multi_priority);
  if(module) return module;

  if(_search_history_by_op(dev_dest, mod_src) == NULL)
  {
    module = dt_iop_get_module_by_op_priority(dev_dest->iop, mod_src->op, -1);
    if(!IS_NULL_PTR(module))
    {
      *reused_base = TRUE;
      return module;
    }
  }

  module = dt_dev_create_module_instance(dev_dest, mod_src->op, mod_src->multi_name, mod_src->multi_priority, FALSE);
  if(module) *created = TRUE;

  return module;
}

// dev_src is used only to copy masks, if no mask will be copied it can be null
int dt_history_merge_module_into_history(dt_develop_t *dev_dest, dt_develop_t *dev_src, dt_iop_module_t *mod_src)
{
  gboolean created = FALSE;
  gboolean reused_base = FALSE;
  dt_iop_module_t *module = _history_merge_resolve_dest_instance(dev_dest, mod_src, &created, &reused_base);
  if(IS_NULL_PTR(module)) return 0;

  if(mod_src->flags() & IOP_FLAGS_ONE_INSTANCE)
  {
    dt_print(DT_DEBUG_HISTORY, "[dt_history_merge_module_into_history] %s (%s) will be overriden in target history by parameters from source history\n",
             mod_src->name(), mod_src->multi_name);
  }
  else if(created)
  {
    dt_print(DT_DEBUG_HISTORY, "[dt_history_merge_module_into_history] %s (%s) will be inserted as a new instance in target history\n",
             mod_src->name(), mod_src->multi_name);
  }
  else if(reused_base)
  {
    dt_print(DT_DEBUG_HISTORY, "[dt_history_merge_module_into_history] %s (%s) will be enabled in target history with parameters from source history\n",
             mod_src->name(), mod_src->multi_name);
  }

  g_strlcpy(module->multi_name, mod_src->multi_name, sizeof(module->multi_name));

  if(dt_dev_copy_module_contents(dev_dest, dev_src, module, mod_src)) return 0;

  dt_dev_add_history_item_ext(dev_dest, module, FALSE, FALSE);

  return 1;
}
/**
 * @brief Deep-copy a history list.
 *
 * Duplicates params, blend params and masks for each history item.
 *
 * @param hist Source history list.
 * @return Newly-allocated history list.
 */
GList *dt_history_duplicate(GList *hist)
{
  GList *result = NULL;
  for(GList *h = g_list_first(hist); h; h = g_list_next(h))
  {
    const dt_dev_history_item_t *old = (dt_dev_history_item_t *)(h->data);
    dt_dev_history_item_t *new = (dt_dev_history_item_t *)malloc(sizeof(dt_dev_history_item_t));

    memcpy(new, old, sizeof(dt_dev_history_item_t));

    dt_iop_module_t *module = (old->module) ? old->module : dt_iop_get_module(old->op_name);

    if(old->params && old->params_size > 0)
    {
      new->params = malloc(old->params_size);
      memcpy(new->params, old->params, old->params_size);
    }

    if(IS_NULL_PTR(module))
      fprintf(stderr, "[_duplicate_history] can't find base module for %s\n", old->op_name);

    if(old->blend_params && old->blendop_params_size > 0)
    {
      new->blend_params = malloc(old->blendop_params_size);
      memcpy(new->blend_params, old->blend_params, old->blendop_params_size);
    }

    if(old->forms) new->forms = dt_masks_dup_forms_deep(old->forms, NULL);

    result = g_list_prepend(result, new);
  }

  return g_list_reverse(result);  // list was built in reverse order, so un-reverse it
}

typedef struct dt_undo_history_t
{
  GList *before_snapshot, *after_snapshot;
  int before_end, after_end;
  GList *before_iop_order_list, *after_iop_order_list;
  dt_masks_edit_mode_t mask_edit_mode;
  dt_dev_pixelpipe_display_mask_t request_mask_display;
} dt_undo_history_t;

struct _cb_data
{
  dt_iop_module_t *module;
  int multi_priority;
};

/**
 * @brief Undo iterator callback to invalidate module pointers in snapshots.
 *
 * @param user_data Module pointer to invalidate.
 * @param type Undo record type.
 * @param item Undo data payload.
 */
static void _history_invalidate_cb(gpointer user_data, dt_undo_type_t type, dt_undo_data_t item)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;
  dt_undo_history_t *hist = (dt_undo_history_t *)item;
  dt_dev_invalidate_history_module(hist->before_snapshot, module);
  dt_dev_invalidate_history_module(hist->after_snapshot, module);
}

void dt_dev_history_undo_invalidate_module(dt_iop_module_t *module)
{
  if(IS_NULL_PTR(module)) return;
  dt_undo_iterate_internal(darktable.undo, DT_UNDO_HISTORY, module, &_history_invalidate_cb);
}

/**
 * @brief Free an undo history snapshot structure.
 *
 * @param data Pointer to dt_undo_history_t.
 */
static void _history_undo_data_free(gpointer data)
{
  dt_undo_history_t *hist = (dt_undo_history_t *)data;
  if(IS_NULL_PTR(hist)) return;
  g_list_free_full(hist->before_snapshot, dt_dev_free_history_item);
  hist->before_snapshot = NULL;
  g_list_free_full(hist->after_snapshot, dt_dev_free_history_item);
  hist->after_snapshot = NULL;
  g_list_free_full(hist->before_iop_order_list, dt_free_gpointer);
  hist->before_iop_order_list = NULL;
  g_list_free_full(hist->after_iop_order_list, dt_free_gpointer);
  hist->after_iop_order_list = NULL;
  dt_free(hist);
}

/**
 * @brief Apply an undo/redo history snapshot to a develop context.
 *
 * Restores history list, history_end, and iop_order_list, then re-populates modules.
 *
 * @param user_data Develop context.
 * @param type Undo record type.
 * @param data Undo record payload.
 * @param action Undo/redo action.
 * @param imgs Unused.
 */
static void _pop_undo(gpointer user_data, dt_undo_type_t type, dt_undo_data_t data, dt_undo_action_t action, GList **imgs)
{
  if(type != DT_UNDO_HISTORY) return;

  dt_develop_t *dev = (dt_develop_t *)user_data;
  dt_undo_history_t *hist = (dt_undo_history_t *)data;
  if(IS_NULL_PTR(dev) || IS_NULL_PTR(hist)) return;

  GList *snapshot = (action == DT_ACTION_UNDO) ? hist->before_snapshot : hist->after_snapshot;
  const int history_end = (action == DT_ACTION_UNDO) ? hist->before_end : hist->after_end;
  GList *iop_order_list
      = (action == DT_ACTION_UNDO) ? hist->before_iop_order_list : hist->after_iop_order_list;

  GList *history_temp = dt_history_duplicate(snapshot);
  GList *iop_order_temp = dt_ioppr_iop_order_copy_deep(iop_order_list);

  dt_pthread_rwlock_wrlock(&dev->history_mutex);
  dt_dev_history_free_history(dev);
  dev->history = history_temp;
  dt_dev_set_history_end_ext(dev, history_end);
  g_list_free_full(dev->iop_order_list, dt_free_gpointer);
  dev->iop_order_list = iop_order_temp;
  dt_dev_pop_history_items_ext(dev);
  dt_dev_write_history_ext(dev, dev->image_storage.id);
  dt_pthread_rwlock_unlock(&dev->history_mutex);

  dt_dev_history_gui_update(dev);
  // TODO: check if we need to rebuild the full pipeline and do it only if needed
  dt_dev_history_pixelpipe_update(dev, TRUE);
  dt_dev_history_notify_change(dev, dev->image_storage.id);

  if(dev->gui_module)
  {
    dt_masks_set_edit_mode(dev->gui_module, hist->mask_edit_mode);
    dev->gui_module->request_mask_display = hist->request_mask_display;
    dt_iop_gui_update_blendif(dev->gui_module);
    dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)(dev->gui_module->blend_data);
    if(bd)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->showmask),
                                   hist->request_mask_display == DT_DEV_PIXELPIPE_DISPLAY_MASK);
  }

  // Ensure all UI pieces (history treeview, iop order, etc.) resync after undo/redo.
  // Undo callbacks bypass dt_dev_undo_end_record(), so we need to raise the change signal here.
  if(darktable.gui && dev->gui_attached && dev == darktable.develop)
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_CHANGE);
}

void dt_dev_history_undo_start_record(dt_develop_t *dev)
{
  if(IS_NULL_PTR(dev)) return;
  dt_pthread_rwlock_rdlock(&dev->history_mutex);
  dt_dev_history_undo_start_record_locked(dev);
  dt_pthread_rwlock_unlock(&dev->history_mutex);
}

void dt_dev_history_undo_start_record_locked(dt_develop_t *dev)
{
  if(IS_NULL_PTR(dev)) return;

  if(dev->undo_history_depth == 0)
  {
    g_list_free_full(dev->undo_history_before_snapshot, dt_dev_free_history_item);
    dev->undo_history_before_snapshot = NULL;
    g_list_free_full(dev->undo_history_before_iop_order_list, dt_free_gpointer);
    dev->undo_history_before_iop_order_list = NULL;
    dev->undo_history_before_end = 0;

    dev->undo_history_before_snapshot = dt_history_duplicate(dev->history);
    dev->undo_history_before_end = dt_dev_get_history_end_ext(dev);
    dev->undo_history_before_iop_order_list = dt_ioppr_iop_order_copy_deep(dev->iop_order_list);
  }

  dev->undo_history_depth++;
}

void dt_dev_history_undo_end_record(dt_develop_t *dev)
{
  if(IS_NULL_PTR(dev)) return;
  dt_pthread_rwlock_rdlock(&dev->history_mutex);
  dt_dev_history_undo_end_record_locked(dev);
  dt_pthread_rwlock_unlock(&dev->history_mutex);
}

void dt_dev_history_undo_end_record_locked(dt_develop_t *dev)
{
  if(IS_NULL_PTR(dev) || dev->undo_history_depth <= 0) return;

  dev->undo_history_depth--;
  if(dev->undo_history_depth != 0) return;

  if(IS_NULL_PTR(dev->undo_history_before_snapshot)) return;

  dt_undo_history_t *hist = malloc(sizeof(dt_undo_history_t));
  hist->before_snapshot = dev->undo_history_before_snapshot;
  hist->before_end = dev->undo_history_before_end;
  hist->before_iop_order_list = dev->undo_history_before_iop_order_list;
  dev->undo_history_before_snapshot = NULL;
  dev->undo_history_before_end = 0;
  dev->undo_history_before_iop_order_list = NULL;

  hist->after_snapshot = dt_history_duplicate(dev->history);
  hist->after_end = dt_dev_get_history_end_ext(dev);
  hist->after_iop_order_list = dt_ioppr_iop_order_copy_deep(dev->iop_order_list);

  if(dev->gui_module)
  {
    hist->mask_edit_mode = dt_masks_get_edit_mode(dev->gui_module);
    hist->request_mask_display = dev->gui_module->request_mask_display;
  }
  else
  {
    hist->mask_edit_mode = DT_MASKS_EDIT_OFF;
    hist->request_mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
  }

  dt_undo_record(darktable.undo, dev, DT_UNDO_HISTORY, (dt_undo_data_t)hist, _pop_undo, _history_undo_data_free);
}


/**
 * @brief Remove history items past history_end when allowed.
 *
 * Filters out obsolete entries while preserving mandatory modules.
 *
 * @param dev Develop context.
 */
static void _remove_history_leaks(dt_develop_t *dev)
{
  GList *history = g_list_nth(dev->history, dt_dev_get_history_end_ext(dev));
  while(history)
  {
    // We need to use a while because we are going to dynamically remove entries at the end
    // of the list, so we can't know the number of iterations
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);
    if(IS_NULL_PTR(hist) || IS_NULL_PTR(hist->module))
    {
      dt_print(DT_DEBUG_HISTORY,
               "[dt_dev_add_history_item_ext] archival history item %s at %i is past history limit (%i) and will be kept\n",
               hist ? hist->op_name : "(null)", g_list_index(dev->history, hist), dt_dev_get_history_end_ext(dev) - 1);
      history = g_list_next(history);
      continue;
    }

    dt_print(DT_DEBUG_HISTORY, "[dt_dev_add_history_item_ext] history item %s at %i is past history limit (%i)\n",
             hist->module->op, g_list_index(dev->history, hist), dt_dev_get_history_end_ext(dev) - 1);

    // In case user wants to insert new history items before auto-enabled or mandatory modules,
    // we forbid it, unless we already have at least one lower history entry.

    // Check if an earlier instance of mandatory module exists
    gboolean earlier_entry = FALSE;
    if((hist->module->hide_enable_button || hist->module->default_enabled))
    {
      for(GList *prior_history = g_list_previous(history); prior_history;
          prior_history = g_list_previous(prior_history))
      {
        dt_dev_history_item_t *prior_hist = (dt_dev_history_item_t *)(prior_history->data);
        if(!prior_hist || !prior_hist->module) continue;
        if(prior_hist->module->so == hist->module->so)
        {
          earlier_entry = TRUE;
          break;
        }
      }
    }

    // In case we delete the current link, we need to update the incrementer now
    // to not loose the reference
    GList *link = history;
    history = g_list_next(history);

    // Finally: attempt removing the obsoleted entry
    if((!hist->module->hide_enable_button && !hist->module->default_enabled)
        || earlier_entry)
    {
      dt_print(DT_DEBUG_HISTORY, "[dt_dev_add_history_item_ext] removing obsoleted history item: %s at %i\n", hist->module->op, g_list_index(dev->history, hist));
      dt_dev_free_history_item(hist);
      dev->history = g_list_delete_link(dev->history, link);
    }
    else
    {
      dt_print(DT_DEBUG_HISTORY, "[dt_dev_add_history_item_ext] obsoleted history item will be kept: %s at %i\n", hist->module->op, g_list_index(dev->history, hist));
    }
  }
}

gboolean dt_dev_add_history_item_ext(dt_develop_t *dev, struct dt_iop_module_t *module, gboolean enable,
                                     gboolean force_new_item)
{
  // If this history item is the first for this module,
  // we need to notify the pipeline that its topology may change (aka insert a new node).
  // Since changing topology is expensive, we want to do it only when needed.
  gboolean add_new_pipe_node = FALSE;

  if(IS_NULL_PTR(module))
  {
    // module = NULL means a mask was changed from the mask manager and that's where this function is called.
    // Find it now, even though it is not enabled and won't be.
    module = dt_masks_get_mask_manager(dev);
    if(!IS_NULL_PTR(module))
    {
      // Mask manager is an IOP that never processes pixel aka it's an ugly hack to record mask history
      force_new_item = FALSE;
      enable = FALSE;
    }
    else
    {
      return add_new_pipe_node;
    }
  }

  // look for leaks on top of history
  _remove_history_leaks(dev);

  // Check if the current module to append to history is actually the same as the last one in history,
  GList *last = g_list_last(dev->history);
  gboolean new_is_old = FALSE;
  if(last && last->data && !force_new_item)
  {
    dt_dev_history_item_t *last_item = (dt_dev_history_item_t *)last->data;
    dt_iop_module_t *last_module = last_item->module;
    new_is_old = dt_iop_check_modules_equal(module, last_module);
    add_new_pipe_node = FALSE;
  }
  else
  {
    const dt_dev_history_item_t *previous_item =
      dt_dev_history_get_last_item_by_module(dev->history, module, g_list_length(dev->history));
    // check if NULL first or prevous_item->module will segfault
    // We need to add a new pipeline node if:
    add_new_pipe_node = (IS_NULL_PTR(previous_item))                         // it's the first history entry for this module
                        || (previous_item->enabled != module->enabled); // the previous history entry is disabled
    // if previous history entry is disabled and we don't have any other entry,
    // it is possible the pipeline will not have this node.
  }

  dt_dev_history_item_t *hist;
  if(force_new_item || !new_is_old)
  {
    // Create a new history entry
    hist = (dt_dev_history_item_t *)calloc(1, sizeof(dt_dev_history_item_t));

    dev->history = g_list_append(dev->history, hist);

    hist->num = g_list_index(dev->history, hist);

    dt_print(DT_DEBUG_HISTORY, "[dt_dev_add_history_item_ext] new history entry added for %s at position %i\n",
            module->name(), hist->num);
  }
  else
  {
    // Reuse previous history entry
    hist = (dt_dev_history_item_t *)last->data;

    dt_print(DT_DEBUG_HISTORY, "[dt_dev_add_history_item_ext] history entry reused for %s at position %i\n",
             module->name(), hist->num);
  }

  // Always resync history with all module internals
  if(enable) module->enabled = TRUE;

  // Include masks if module supports blending and masks are in use, or if it's the mask manager.
  const gboolean include_masks = dt_iop_module_needs_mask_history(module);

  GList *forms_snapshot = NULL;
  if(include_masks)
  {
    dt_print(DT_DEBUG_HISTORY, "[dt_dev_add_history_item_ext] committing masks for module %s at history position %i\n", module->name(), hist->num);
    // FIXME: this copies ALL drawn masks AND masks groups used by all modules to any module history using masks.
    // Kudos to the idiots who thought it would be reasonable. Expect database bloating and perf penalty.
    forms_snapshot = dt_masks_snapshot_current_forms(dev, TRUE);
  }
  // else forms_snapshot stays NULL

  // Fill history item and recompute hash (also applies params/blend_params to module to keep hash consistent).
  dt_dev_history_item_update_from_params(dev, hist, module, module->enabled, module->params, module->params_size,
                                         module->blend_params, forms_snapshot);

  if(include_masks && hist->forms)
    dt_print(DT_DEBUG_HISTORY, "[dt_dev_add_history_item_ext] masks committed for module %s at history position %i\n", module->name(), hist->num);
  else if(include_masks)
    dt_print(DT_DEBUG_HISTORY, "[dt_dev_add_history_item_ext] masks NOT committed for module %s at history position %i\n", module->name(), hist->num);

  // It is assumed that the last-added history entry is always on top
  // so its cursor index is always equal to the number of elements,
  // keeping in mind that history_end = 0 is the raw image, aka not a dev->history GList entry.
  // So dev->history_end = index of last history entry + 1 = length of history
  dt_dev_set_history_end_ext(dev, g_list_length(dev->history));

  return add_new_pipe_node;
}

uint64_t dt_dev_history_compute_hash(dt_develop_t *dev)
{
  uint64_t hash = 5381;
  for(GList *hist = g_list_nth(dev->history, dt_dev_get_history_end_ext(dev) - 1);
      hist;
      hist = g_list_previous(hist))
  {
    dt_dev_history_item_t *item = (dt_dev_history_item_t *)hist->data;
    hash = dt_hash(hash, (const char *)&item->hash, sizeof(uint64_t));
  }
  dt_print(DT_DEBUG_HISTORY, "[dt_dev_history_get_hash] history hash: %" PRIu64 ", history end: %i, items %i\n", hash, dt_dev_get_history_end_ext(dev), g_list_length(dev->history));
  return hash;
}


// The next 2 functions are always called from GUI controls setting parameters
// This is why they directly start a pipeline recompute.
// Otherwise, please keep GUI and pipeline fully separated.

void dt_dev_add_history_item_real(dt_develop_t *dev, dt_iop_module_t *module, gboolean enable, gboolean redraw)
{
  gboolean add_new_pipe_node = FALSE;

  dt_atomic_set_int(&dev->pipe->shutdown, TRUE);
  dt_atomic_set_int(&dev->preview_pipe->shutdown, TRUE);

  dt_dev_undo_start_record(dev);
  dt_pthread_rwlock_wrlock(&dev->history_mutex);
  add_new_pipe_node = dt_dev_add_history_item_ext(dev, module, enable, FALSE);
  dt_pthread_rwlock_unlock(&dev->history_mutex);
  dt_dev_undo_end_record(dev);

  // Run the delayed post-commit actions if implemented
  if(!IS_NULL_PTR(module) && !IS_NULL_PTR(module->post_history_commit)) module->post_history_commit(module);

  // Figure out if the current history item includes masks/forms
  GList *last_history = g_list_nth(dev->history, dt_dev_get_history_end_ext(dev) - 1);
  dt_dev_history_item_t *hist = NULL;
  gboolean has_forms = FALSE;
  if(last_history)
  {
    hist = (dt_dev_history_item_t *)last_history->data;
    has_forms = (!IS_NULL_PTR(hist->forms));
  }

  // We don't update history hash in dt_dev_add_history_item_ext
  // because it can be called within loops, so that can be expensive.
  dt_dev_set_history_hash(dev, dt_dev_history_compute_hash(dev));
  if(dev->image_storage.id > 0)
  {
    dt_image_t *cache_img = dt_image_cache_get(darktable.image_cache, dev->image_storage.id, 'w');
    if(cache_img)
    {
      cache_img->history_hash = dt_dev_get_history_hash(dev);
      cache_img->mipmap_hash = 0;
      dt_print(DT_DEBUG_IMAGEIO,
               "[history/hash] imgid=%d staged current=%" PRIu64 " mipmap=%" PRIu64
               " after history edit\n",
               dev->image_storage.id, cache_img->history_hash, cache_img->mipmap_hash);
      dt_image_cache_write_release(darktable.image_cache, cache_img, DT_IMAGE_CACHE_RELAXED);
    }
  }

  // Recompute pipeline last
  const gboolean has_raster = module && dt_iop_module_has_raster_mask(module);
  if(!IS_NULL_PTR(module) && !(has_forms || has_raster) && !add_new_pipe_node)
  {
    // If we have a module and it doesn't use drawn or raster masks,
    // we only need to resync the top-most history item with pipeline
    dt_dev_pixelpipe_update_history_all(dev);
  }
  else
  {
    // We either don't have a module, meaning we have the mask manager, or
    // we have a module and it uses masks (drawn or raster), or the current
    // history commit changes the pipeline topology. The latter happens for
    // example when enabling/disabling a module or when a module gets its first
    // history entry. In all these cases, updating only the top-most synced
    // history tail is insufficient because the set of active pipe nodes itself
    // may differ from the previous run.
    // Because masks can affect several modules anywhere, not necessarily sequentially,
    // we need a full resync of all pipeline with history.
    // Note that the blendop params (thus their hash) references the raster mask provider
    // in its consumer, and the consumer in its provider. So updating the whole pipe
    // resyncs the cumulative hashes too, and triggers a new recompute from the provider on update.
    dt_dev_pixelpipe_resync_history_all(dev);
  }

  dt_dev_masks_list_update(dev);

  if(!IS_NULL_PTR(darktable.gui) && dev->gui_attached && !IS_NULL_PTR(module))
  {
    // If module params change the geometry of the ROI,
    // update immediately so we avoid drawing glitches.
    if(module->modify_roi_in || module->modify_roi_out)
      dt_dev_get_thumbnail_size(dev);
    

    // Changing a parameter of a disabled module enables it,
    // so update the GUI toggle state to reflect it.
    ++darktable.gui->reset; // don't run GUI callbacks when setting GUI state
    dt_iop_gui_set_enable_button(module);
    --darktable.gui->reset;
  }
  
  // Save history straight away. Regular GUI edits are the only place where
  // we accept an asynchronous write because `dev` is the long-lived darkroom
  // context and there is no immediate DB read on the same control path.
  dt_dev_write_history(dev, TRUE);
  dt_dev_history_notify_change(dev, dev->image_storage.id);
}

void dt_dev_free_history_item(gpointer data)
{
  dt_dev_history_item_t *item = (dt_dev_history_item_t *)data;
  if(IS_NULL_PTR(item)) return; // nothing to free

  dt_free(item->params);
  dt_free(item->blend_params);
  g_list_free_full(item->forms, (void (*)(void *))dt_masks_free_form);
  item->forms = NULL;
  dt_free(item);
}

void dt_dev_history_free_history(dt_develop_t *dev)
{
  if(IS_NULL_PTR(dev->history)) return;
  g_list_free_full(g_steal_pointer(&dev->history), dt_dev_free_history_item);
  dev->history = NULL;
}

void dt_dev_reload_history_items(dt_develop_t *dev, const int32_t imgid)
{
  // Recreate the whole history from scratch.
  // Backend only: GUI updates and pixelpipe rebuilds need to be triggered by callers.
  if(darktable.gui && dev->gui_attached) ++darktable.gui->reset;
  dt_pthread_rwlock_wrlock(&dev->history_mutex);
  dt_dev_read_history_ext(dev, imgid);
  dt_dev_pop_history_items_ext(dev);
  dt_pthread_rwlock_unlock(&dev->history_mutex);
  if(darktable.gui && dev->gui_attached) --darktable.gui->reset;
}


/**
 * @brief Reload defaults for all modules in dev->iop.
 *
 * Some modules depend on defaults to initialize GUI state or internal structures.
 *
 * @param dev Develop context.
 */
static inline void _dt_dev_modules_reload_defaults(dt_develop_t *dev)
{
  for(GList *modules = g_list_first(dev->iop); modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    dt_iop_reload_defaults(module);

    if(module->multi_priority == 0)
      module->iop_order = dt_ioppr_get_iop_order(dev->iop_order_list, module->op, module->multi_priority);
    else
      module->iop_order = INT_MAX;

    // Last chance & desperate attempt at enabling/disabling critical modules
    // when history is garbled - This might prevent segfaults on invalid data
    if(module->force_enable)
      module->enabled = (module->force_enable(module, module->enabled) != 0);

    // make sure that always-on modules are always on. duh.
    if(module->default_enabled == 1 && module->hide_enable_button == 1)
      module->enabled = TRUE;

    dt_iop_compute_module_hash(module, dev->forms);
  }
}

// Dump the content of an history entry into its associated module params, blendops, etc.
/**
 * @brief Apply a history item to a module instance.
 *
 * Copies params/blend params, updates enabled state, and recomputes hashes.
 *
 * @param hist History item.
 * @param module Module instance.
 */
static inline void _history_to_module(const dt_dev_history_item_t *const hist, dt_iop_module_t *module)
{
  module->enabled = (hist->enabled != 0);

  // Update IOP order stuff, that applies to all modules regardless of their internals
  module->iop_order = hist->iop_order;
  dt_iop_update_multi_priority(module, hist->multi_priority);

  // Copy instance name
  g_strlcpy(module->multi_name, hist->multi_name, sizeof(module->multi_name));

  // Copy params from history entry to module internals
  memcpy(module->params, hist->params, module->params_size);
  dt_iop_commit_blend_params(module, hist->blend_params);

  // Get the module hash
  dt_iop_compute_module_hash(module, hist->forms);
}


void dt_dev_pop_history_items_ext(dt_develop_t *dev)
{
  dt_print(DT_DEBUG_HISTORY, "[dt_dev_pop_history_items_ext] loading history entries into modules...\n");

  // Ensure `dev->image_storage` is up-to-date before modules reload their defaults.
  // This avoids using incomplete RAW metadata (WB coeffs, matrices) on newly-inited images.
  dt_dev_ensure_image_storage(dev, dev->image_storage.id);

  // Shitty design ahead:
  // some modules (temperature.c, colorin.c) init their GUI comboboxes
  // in/from reload_defaults. Though we already loaded them once at
  // _read_history_ext() when initing history, and history is now sanitized
  // such that all used module will have at least an entry,
  // it's not enough and we need to reload defaults here.
  // But anyway, if user truncated history before mandatory modules,
  // and we reload it here, it's good to ensure defaults are re-inited.
  _dt_dev_modules_reload_defaults(dev);

  const int history_end = dt_dev_get_history_end_ext(dev);

  // go through history up to history_end and set modules params
  GList *history = g_list_first(dev->history);
  GList *forms = NULL;
  for(int i = 0; i < history_end && history; i++)
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);
    dt_iop_module_t *module = hist->module;
    if(module) _history_to_module(hist, module);

    // Update the reference to the form snapshot that doesn't belong
    // conceptually to history items
    if(hist->forms) forms = hist->forms;

    history = g_list_next(history);
  }

  // Nuke dev->forms and replace it with the last hist->forms in history.
  dt_masks_replace_current_forms(dev, forms);

  dt_ioppr_resync_pipeline(dev, 0, "dt_dev_pop_history_items_ext end", TRUE);

  // Reloading defaults might have changed the global history hash
  dt_dev_set_history_hash(dev, dt_dev_history_compute_hash(dev));

  // Update darkroom sizes in case clipping & distortion changed.
  // This is now handled by the wrapper after releasing the history lock.
}

void dt_dev_pop_history_items(dt_develop_t *dev)
{
  if(darktable.gui && dev->gui_attached) ++darktable.gui->reset;
  dt_pthread_rwlock_wrlock(&dev->history_mutex);
  dt_dev_pop_history_items_ext(dev);
  dt_pthread_rwlock_unlock(&dev->history_mutex);
  // Update darkroom sizes after releasing the history lock to avoid deadlocks.
  if(dev->gui_attached) dt_dev_get_thumbnail_size(dev);
  if(darktable.gui && dev->gui_attached) --darktable.gui->reset;
}

void dt_dev_history_gui_update(dt_develop_t *dev)
{
  if(!dev->gui_attached) return;

  // Ensure the set of module instances shown in the right panel matches the current history:
  // hide/remove instances that are no longer referenced by any history item.
  // Note: this may also reorder modules in the GUI if needed.
  dt_pthread_rwlock_wrlock(&dev->history_mutex);
  dt_dev_history_refresh_nodes_ext(dev, &dev->iop, dev->history);
  dt_pthread_rwlock_unlock(&dev->history_mutex);

  ++darktable.gui->reset;

  for(GList *module = g_list_first(dev->iop); module; module = g_list_next(module))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)(module->data);
    dt_iop_gui_update(mod);
  }

  dt_dev_masks_list_change(dev);
  --darktable.gui->reset;

  dt_dev_signal_modules_moved(dev);
}

void dt_dev_history_pixelpipe_update(dt_develop_t *dev, gboolean rebuild)
{
  if(!dev->gui_attached) return;

  if(rebuild)
    dt_dev_pixelpipe_rebuild_all(dev);
  else
    dt_dev_pixelpipe_resync_history_all(dev);
}

/**
 * @brief Delete all history entries for an image from the DB.
 *
 * @param imgid Image id.
 */
static void _cleanup_history(const int32_t imgid)
{
  dt_history_db_delete_dev_history(imgid);
}

guint dt_dev_mask_history_overload(GList *dev_history, guint threshold)
{
  // Count all the mask forms used x history entries, up to a certain threshold.
  // Stop counting when the threshold is reached, for performance.
  guint states = 0;
  for(GList *history = g_list_first(dev_history); history; history = g_list_next(history))
  {
    dt_dev_history_item_t *hist_item = (dt_dev_history_item_t *)(history->data);
    states += g_list_length(hist_item->forms);
    if(states > threshold) break;
  }
  return states;
}

void dt_dev_history_notify_change(dt_develop_t *dev, const int32_t imgid)
{
  if(IS_NULL_PTR(dev) || imgid <= 0) return;

  if(darktable.gui && dev->gui_attached)
  {
    const guint states = dt_dev_mask_history_overload(dev->history, 250);
    if(states > 250)
      dt_toast_log(_("Image #%i history is storing %d mask states. n"
                     "Consider compressing history and removing unused masks to keep reads/writes manageable."),
                     imgid, states);
  }

  // Remove all old images
  dt_mipmap_cache_remove(darktable.mipmap_cache, imgid, TRUE);

  // Don't refresh the thumbnail if we are in darkroom
  // Spawning another export thread will likely slow-down the current one.
  if(darktable.gui && dev != darktable.develop)
    dt_thumbtable_refresh_thumbnail(darktable.gui->ui->thumbtable_lighttable, imgid, TRUE);
}


// helper used to synch a single history item with db
int dt_dev_write_history_item(const int32_t imgid, dt_dev_history_item_t *h, int32_t num)
{
  if(IS_NULL_PTR(h)) return 1;

  dt_print(DT_DEBUG_HISTORY, "[dt_dev_write_history_item] writing history for module %s (%s) (enabled %i) at pipe position %i for image %i\n", 
                                                    h->op_name, h->multi_name, h->enabled, h->iop_order, imgid);

  const char *operation = h->module ? h->module->op : h->op_name;
  const int params_size = h->module ? h->module->params_size : h->params_size;
  const int module_version = h->module ? h->module->version() : h->module_version;
  const int blendop_params_size
      = h->blend_params ? (h->blendop_params_size > 0 ? h->blendop_params_size : (int)sizeof(dt_develop_blend_params_t)) : 0;
  const int blendop_version
      = h->blend_params ? (h->blendop_version > 0 ? h->blendop_version : dt_develop_blend_version()) : 0;

  dt_history_db_write_history_item(imgid, num, operation, h->params, params_size, module_version, h->enabled != 0,
                                   h->blend_params, blendop_params_size, blendop_version, h->multi_priority, h->multi_name);

  // write masks (if any)
  if(h->forms)
    dt_print(DT_DEBUG_HISTORY, "[dt_dev_write_history_item] drawn mask found for module %s (%s) for image %i\n", h->op_name, h->multi_name, imgid);

  for(GList *forms = g_list_first(h->forms); forms; forms = g_list_next(forms))
  {
    dt_masks_form_t *form = (dt_masks_form_t *)forms->data;
    if (form)
      dt_masks_write_masks_history_item(imgid, num, form);
  }

  return 0;
}

void dt_dev_history_cleanup(void)
{
  // No-op: SQL statement caching/cleanup for history lives in common/history.c (dt_history_cleanup()).
}



void dt_dev_write_history_ext(dt_develop_t *dev, const int32_t imgid)
{
  dt_image_t *cache_img = dt_image_cache_get(darktable.image_cache, imgid, 'w');
  if(IS_NULL_PTR(cache_img)) return;

  dt_print(DT_DEBUG_HISTORY, "[dt_dev_write_history_ext] writing history for image %i...\n", imgid);

  dt_dev_set_history_hash(dev, dt_dev_history_compute_hash(dev));

  _cleanup_history(imgid);

  // write history entries
  int i = 0;
  for(GList *history = g_list_first(dev->history); history; history = g_list_next(history))
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);
    dt_dev_write_history_item(imgid, hist, i);
    i++;
  }

  dt_history_set_end(imgid, dt_dev_get_history_end_ext(dev));

  // write the current iop-order-list for this image
  dt_ioppr_write_iop_order_list(dev->iop_order_list, imgid);

  cache_img->history_hash = dt_dev_get_history_hash(dev);

  dt_image_cache_write_release(darktable.image_cache, cache_img, DT_IMAGE_CACHE_SAFE);
}

// Schedule history write as a background job to avoid blocking the GUI.
// If scheduling fails, fall back to synchronous write to preserve behaviour.
static int _dt_dev_write_history_job_run(dt_job_t *job)
{
  dt_develop_t *d = dt_control_job_get_params(job);
  if(IS_NULL_PTR(d)) return 1;
  dt_pthread_rwlock_rdlock(&d->history_mutex);
  dt_dev_write_history_ext(d, d->image_storage.id);
  dt_pthread_rwlock_unlock(&d->history_mutex);
  return 0;
}

// Write TO XMP, so from the dev perspective, it's a read
void dt_dev_write_history(dt_develop_t *dev, gboolean async)
{
  if(!async)
  {
    dt_pthread_rwlock_rdlock(&dev->history_mutex);
    dt_dev_write_history_ext(dev, dev->image_storage.id);
    dt_pthread_rwlock_unlock(&dev->history_mutex);
    return;
  }

  dt_job_t *job = dt_control_job_create(&_dt_dev_write_history_job_run, "write history %d",
                                        dev->image_storage.id);
  dt_control_job_set_params(job, dev, NULL);

  if(dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_BG, job) != 0)
  {
    // scheduling failed: dispose job and run synchronously
    dt_control_job_dispose(job);
    dt_dev_write_history(dev, FALSE);
  }
}

/**
 * @brief Apply auto-presets and default iop order for a fresh history.
 *
 * @param dev Develop context.
 * @param imgid Image id.
 * @return TRUE if presets were applied.
 */
static gboolean _dev_auto_apply_presets(dt_develop_t *dev, int32_t imgid)
{
  dt_image_t *image = &dev->image_storage;
  const gboolean has_matrix = dt_image_is_matrix_correction_supported(image);
  const char *workflow_preset = has_matrix ? _("scene-referred default") : "\t\n";

  int iformat = 0;
  if(dt_image_is_rawprepare_supported(image))
    iformat |= FOR_RAW;
  else
    iformat |= FOR_LDR;

  if(dt_image_is_hdr(image))
    iformat |= FOR_HDR;

  int excluded = 0;
  if(dt_image_monochrome_flags(image))
    excluded |= FOR_NOT_MONO;
  else
    excluded |= FOR_NOT_COLOR;

  int legacy_params = 0;
  dt_dev_history_db_ctx_t ctx = { .dev = dev, .imgid = imgid, .legacy_params = &legacy_params, .presets = TRUE };
  dt_history_db_foreach_auto_preset_row(imgid, image, workflow_preset, iformat, excluded, _dev_history_db_row_cb, &ctx);

  // now we want to auto-apply the iop-order list if one corresponds and none are
  // still applied. Note that we can already have an iop-order list set when
  // copying an history or applying a style to a not yet developed image.

  if(!dt_ioppr_has_iop_order_list(imgid))
  {
    void *params = NULL;
    int32_t params_len = 0;
    if(dt_history_db_get_autoapply_ioporder_params(imgid, image, iformat, excluded, &params, &params_len))
    {
      GList *iop_list = dt_ioppr_deserialize_iop_order_list(params, params_len);
      dt_ioppr_write_iop_order_list(iop_list, imgid);
      g_list_free_full(iop_list, dt_free_gpointer);
      iop_list = NULL;
      dt_ioppr_set_default_iop_order(dev, imgid);
      dt_free(params);
    }
    else
    {
      // we have no auto-apply order, so apply iop order, depending of the workflow
      GList *iop_list = dt_ioppr_get_iop_order_list_version(DT_IOP_ORDER_ANSEL_RAW);
      dt_ioppr_write_iop_order_list(iop_list, imgid);
      g_list_free_full(iop_list, dt_free_gpointer);
      iop_list = NULL;
      dt_ioppr_set_default_iop_order(dev, imgid);
    }
  }

  // Notify our private image copy that auto-presets got applied
  dev->image_storage.flags |= DT_IMAGE_AUTO_PRESETS_APPLIED | DT_IMAGE_NO_LEGACY_PRESETS;

  return TRUE;
}

/**
 * @brief Insert default modules into history when needed.
 *
 * Ensures mandatory/default-enabled modules are represented in history,
 * including legacy handling for older histories.
 *
 * @param dev Develop context.
 * @param module Module instance to consider.
 * @param is_inited TRUE if auto-presets were already applied.
 */
static void _insert_default_modules(dt_develop_t *dev, dt_iop_module_t *module, gboolean is_inited)
{
  // Module already in history: don't prepend extra entries
  if(dt_history_check_module_exists(dev->image_storage.id, module->op, FALSE))
    return;

  // Module has no user params: no history: don't prepend either
  if((module->flags() & IOP_FLAGS_NO_HISTORY_STACK)
     && (module->default_enabled || (module->force_enable && module->force_enable(module, FALSE))))
  {
    module->enabled = TRUE;
    return;
  }

  dt_image_t *image = &dev->image_storage;

  // Prior to Darktable 3.0, modules enabled by default which still had
  // default params (no user change) were not inserted into history/DB.
  // We need to insert them here with default params.
  // But defaults have changed since then for some modules, so we need to ensure
  // we insert them with OLD defaults.
  if(module->default_enabled || (module->force_enable && module->force_enable(module, FALSE)))
  {
    module->enabled = TRUE;
    const gboolean has_matrix = dt_image_is_matrix_correction_supported(image);
    const gboolean is_raw = dt_image_is_raw(image);

    if(!strcmp(module->op, "temperature")
       && (image->change_timestamp == -1) // change_timestamp is not defined for old pics
       && is_raw && is_inited && has_matrix)
    {
      dt_print(DT_DEBUG_HISTORY, "[history] Image history seems older than Darktable 3.0, we will insert white balance.\n");

      // Temp revert to legacy defaults
      dt_conf_set_string("plugins/darkroom/chromatic-adaptation", "legacy");
      dt_iop_reload_defaults(module);

      dt_dev_add_history_item_ext(dev, module, TRUE, TRUE);

      // Go back to current defaults
      dt_conf_set_string("plugins/darkroom/chromatic-adaptation", "modern");
      dt_iop_reload_defaults(module);
    }
    else
    {
      dt_dev_add_history_item_ext(dev, module, TRUE, TRUE);
    }
  }
  else if(module->workflow_enabled && !is_inited)
  {
    module->enabled = TRUE;
    dt_dev_add_history_item_ext(dev, module, TRUE, TRUE);
  }

  if(module->enabled)
    dt_print(DT_DEBUG_HISTORY, "[history] %s was inserted into history by default (enabled %i)\n", module->op, module->enabled);
}

// Returns TRUE if this is a freshly-inited history on which we just applied auto presets and defaults,
// FALSE if we had an earlier history
gboolean dt_dev_init_default_history(dt_develop_t *dev, const int32_t imgid, gboolean apply_auto_presets)
{
  const gboolean is_inited = (dev->image_storage.flags & DT_IMAGE_AUTO_PRESETS_APPLIED);

  // Make sure this is set
  dt_conf_set_string("plugins/darkroom/chromatic-adaptation", "modern");

  // make sure all modules default params are loaded to init history
  for(GList *iop = g_list_first(dev->iop); iop; iop = g_list_next(iop))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(iop->data);
    dt_iop_reload_defaults(module);
    _insert_default_modules(dev, module, is_inited);
  }

  // On virgin history image, apply auto stuff (ours and user's)
  if(apply_auto_presets && !is_inited) _dev_auto_apply_presets(dev, imgid);
  dt_print(DT_DEBUG_HISTORY, "[history] temporary history initialised with default params and presets\n");

  return !is_inited;
}

int dt_dev_replace_history_on_image(dt_develop_t *dev_src, const int32_t dest_imgid,
                                    const gboolean reload_defaults, const char *msg)
{
  if(dest_imgid <= 0) return 1;

  dt_dev_ensure_image_storage(dev_src, dest_imgid);

  if(reload_defaults)
  {
    dt_dev_init_default_history(dev_src, dest_imgid, FALSE);
    dt_ioppr_resync_pipeline(dev_src, dest_imgid, msg, FALSE);
  }

  dt_dev_pop_history_items_ext(dev_src);
  dt_dev_write_history(dev_src, FALSE);

  return 0;
}

// populate hist->module
/**
 * @brief Bind a history entry to a module instance (.so) in dev->iop.
 *
 * Creates a new instance if a matching one is required but missing.
 *
 * @param dev Develop context.
 * @param hist History item to bind.
 */
static void _find_so_for_history_entry(dt_develop_t *dev, dt_dev_history_item_t *hist)
{
  dt_iop_module_t *match = NULL;

  for(GList *modules = g_list_first(dev->iop); modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
    if(!strcmp(module->op, hist->op_name))
    {
      if(module->multi_priority == hist->multi_priority)
      {
        // Found exact match at required priority: we are done
        hist->module = module;
        break;
      }
      else if(hist->multi_priority > 0)
      {
        // Found the right kind of module but the wrong instance.
        // Current history entry is targeting an instance that may exist later in the pipe, so keep looping/looking.
        match = module;
      }
    }
  }

  if(!hist->module && match)
  {
    // We found a module having the required name but not the required instance number:
    // add a new instance of this module by using its ->so property
    dt_iop_module_t *new_module = (dt_iop_module_t *)calloc(1, sizeof(dt_iop_module_t));
    if(!dt_iop_load_module(new_module, match->so, dev))
    {
      dev->iop = g_list_append(dev->iop, new_module);
      // Just init, it will get rewritten later by resync IOP order methods:
      new_module->instance = match->instance;
      hist->module = new_module;
    }
  }
  // else we found an already-existing instance and it's in hist->module already

  if(hist->module) hist->module->enabled = (hist->enabled != 0);
}


/**
 * @brief Load or convert blendop params into a history item.
 *
 * Handles version mismatch and legacy conversion.
 *
 * @param hist History item to populate.
 * @param blendop_params Raw blend params blob.
 * @param bl_length Blob size.
 * @param blendop_version Stored version.
 * @param legacy_params Output flag set when legacy conversion occurs.
 */
static void _sync_blendop_params(dt_dev_history_item_t *hist, const void *blendop_params, const int bl_length,
                                 const int blendop_version, int *legacy_params)
{
  const gboolean is_valid_blendop_version = (blendop_version == dt_develop_blend_version());
  const gboolean is_valid_blendop_size = (bl_length == sizeof(dt_develop_blend_params_t));

  hist->blend_params = malloc(sizeof(dt_develop_blend_params_t));
  hist->blendop_params_size = sizeof(dt_develop_blend_params_t);
  hist->blendop_version = dt_develop_blend_version();

  if(!IS_NULL_PTR(blendop_params) && is_valid_blendop_version && is_valid_blendop_size)
  {
    memcpy(hist->blend_params, blendop_params, sizeof(dt_develop_blend_params_t));
  }
  else if(blendop_params
          && dt_develop_blend_legacy_params(hist->module, blendop_params, blendop_version, hist->blend_params,
                                            dt_develop_blend_version(), bl_length)
                 == 0)
  {
    *legacy_params = TRUE;
  }
  else
  {
    memcpy(hist->blend_params, hist->module->default_blendop_params, sizeof(dt_develop_blend_params_t));
  }
}

/**
 * @brief Load or convert module params into a history item.
 *
 * Handles version mismatch, legacy conversion and special cases.
 *
 * @param hist History item to populate.
 * @param module_params Raw params blob.
 * @param param_length Blob size.
 * @param modversion Stored module version.
 * @param legacy_params Output flag set when legacy conversion occurs.
 * @param preset_name Optional preset name (for logging).
 * @return 0 on success, non-zero on conversion failure.
 */
static int _sync_params(dt_dev_history_item_t *hist, const void *module_params, const int param_length,
                        const int modversion, int *legacy_params, const char *preset_name)
{
  hist->params_size = hist->module->params_size;
  hist->module_version = hist->module->version();
  const gboolean is_valid_module_version = (modversion == hist->module->version());
  const gboolean is_valid_params_size = (param_length == hist->module->params_size);

  hist->params = malloc(hist->module->params_size);
  if(is_valid_module_version && is_valid_params_size)
  {
    memcpy(hist->params, module_params, hist->module->params_size);
  }
  else
  {
    if(!hist->module->legacy_params
        || hist->module->legacy_params(hist->module, module_params, labs(modversion),
                                       hist->params, labs(hist->module->version())))
    {
      gchar *preset = (preset_name) ? g_strdup_printf(_("from preset %s"), preset_name)
                                    : g_strdup("");

      fprintf(stderr, "[dev_read_history] module `%s' %s version mismatch: history is %d, dt %d.\n", hist->module->op,
              preset, modversion, hist->module->version());

      dt_control_log(_("module `%s' %s version mismatch: %d != %d"), hist->module->op,
                      preset, hist->module->version(), modversion);

      dt_free(preset);
      return 1;
    }
    else
    {
      // NOTE: spots version was bumped from 1 to 2 in 2013.
      // This handles edits made prior to Darktable 1.4.
      // Then spots was deprecated in 2021 in favour of retouch.
      // How many edits out there still need the legacy conversion in 2025 ?
      if(!strcmp(hist->module->op, "spots") && modversion == 1)
      {
        // quick and dirty hack to handle spot removal legacy_params
        memcpy(hist->blend_params, hist->module->blend_params, sizeof(dt_develop_blend_params_t));
      }
      *legacy_params = TRUE;
    }

    /*
      * Fix for flip iop: previously it was not always needed, but it might be
      * in history stack as "orientation (off)", but now we always want it
      * by default, so if it is disabled, enable it, and replace params with
      * default_params. if user want to, he can disable it.
      * NOTE: Flip version was bumped from 1 to 2 in 2014.
      * This handles edits made prior to Darktable 1.6.
      * How many edits out there still need the legacy conversion in 2025 ?
      */
    if(!strcmp(hist->module->op, "flip") && hist->enabled == 0 && labs(modversion) == 1)
    {
      memcpy(hist->params, hist->module->default_params, hist->module->params_size);
      hist->enabled = TRUE;
    }
  }

  return 0;
}

// WARNING: this does not set hist->forms
/**
 * @brief Build a history item from DB row data and append it to dev->history.
 *
 * Resolves module instance, loads params/blend params, and updates ordering metadata.
 *
 * @param dev Develop context.
 * @param imgid Image id.
 * @param id Image id from DB row (sanity-checked).
 * @param num History index.
 * @param modversion Module version stored in DB.
 * @param operation Operation name.
 * @param module_params Module params blob.
 * @param param_length Module params size.
 * @param enabled Enabled flag from DB.
 * @param blendop_params Blend params blob.
 * @param bl_length Blend params size.
 * @param blendop_version Blend params version.
 * @param multi_priority Instance priority.
 * @param multi_name Instance name.
 * @param preset_name Optional preset name (for logging).
 * @param legacy_params Output flag set when legacy conversion occurs.
 * @param presets TRUE if reading from presets instead of DB history.
 */
static void _process_history_db_entry(dt_develop_t *dev, const int32_t imgid, const int id, const int num,
                                      const int modversion, const char *operation, const void *module_params,
                                      const int param_length, const gboolean enabled, const void *blendop_params,
                                      const int bl_length, const int blendop_version, const int multi_priority,
                                      const char *multi_name, const char *preset_name, int *legacy_params,
                                      const gboolean presets)
{
  // Sanity checks
  const gboolean is_valid_id = (id == imgid);
  const gboolean has_operation = (!IS_NULL_PTR(operation));

  if(!(has_operation && is_valid_id))
  {
    fprintf(stderr, "[dev_read_history] database history for image `%s' seems to be corrupted!\n",
            dev->image_storage.filename);
    return;
  }

  /**
   * History rows may outlive modules that were removed or renamed between
   * releases. If the operation no longer exists in the current module list,
   * drop that row quietly instead of treating it as a broken install.
   */
  if(!dt_iop_get_module_from_list(dev->iop, operation)) return;

  const int iop_order = dt_ioppr_get_iop_order(dev->iop_order_list, operation, multi_priority);

  // Init a bare minimal history entry
  dt_dev_history_item_t *hist = (dt_dev_history_item_t *)calloc(1, sizeof(dt_dev_history_item_t));
  hist->module = NULL;
  hist->num = num;
  hist->iop_order = iop_order;
  hist->multi_priority = multi_priority;
  hist->enabled = (enabled != 0); // cast to gboolean
  g_strlcpy(hist->op_name, operation, sizeof(hist->op_name));
  g_strlcpy(hist->multi_name, multi_name ? multi_name : "", sizeof(hist->multi_name));

  // Find a .so file that matches our history entry, aka a module to run the params stored in DB
  _find_so_for_history_entry(dev, hist);

  if(!hist->module)
  {
    // Keep the serialized history entry even though no live module can consume it.
    hist->params_size = MAX(param_length, 0);
    hist->module_version = modversion;
    hist->blendop_params_size = MAX(bl_length, 0);
    hist->blendop_version = blendop_version;
    if(!IS_NULL_PTR(module_params) && param_length > 0)
    {
      hist->params = malloc(param_length);
      memcpy(hist->params, module_params, param_length);
    }
    if(!IS_NULL_PTR(blendop_params) && bl_length > 0)
    {
      hist->blend_params = malloc(bl_length);
      memcpy(hist->blend_params, blendop_params, bl_length);
    }

    fprintf(
        stderr,
        "[dev_read_history] the module `%s' requested by image `%s' is not installed on this computer!\n",
        operation, dev->image_storage.filename);
    dev->history = g_list_append(dev->history, hist);
    return;
  }

  // Update IOP order stuff, that applies to all modules regardless of their internals
  // Needed now to de-entangle multi-instances
  hist->module->iop_order = hist->iop_order;
  dt_iop_update_multi_priority(hist->module, hist->multi_priority);

  // module has no user params and won't bother us in GUI - exit early, we are done
  if(hist->module->flags() & IOP_FLAGS_NO_HISTORY_STACK)
  {
    // Since it's the last we hear from this module as far as history is concerned,
    // compute its hash here.
    dt_iop_compute_module_hash(hist->module, NULL);

    // Done. We don't add to history
    dt_free(hist);
    return;
  }

  // Copy module params if valid version, else try to convert legacy params
  if(_sync_params(hist, module_params, param_length, modversion, legacy_params, preset_name))
  {
    dt_free(hist);
    return;
  }

  // So far, on error we haven't allocated any buffer, so we just freed the hist structure

  // Last chance & desperate attempt at enabling/disabling critical modules
  // when history is garbled - This might prevent segfaults on invalid data
  if(hist->module->force_enable)
    hist->enabled = (hist->module->force_enable(hist->module, hist->enabled) != 0);

  // make sure that always-on modules are always on. duh.
  if(hist->module->default_enabled == TRUE && hist->module->hide_enable_button == TRUE)
    hist->enabled = TRUE;

  // Copy blending params if valid, else try to convert legacy params
  _sync_blendop_params(hist, blendop_params, bl_length, blendop_version, legacy_params);

  dev->history = g_list_append(dev->history, hist);

  dt_print(DT_DEBUG_HISTORY, "[history entry] read %s at pipe position %i (enabled %i) from %s %s\n", hist->op_name,
    hist->iop_order, hist->enabled, (presets) ? "preset" : "database", (presets) ? preset_name : "");
}


gboolean dt_dev_read_history_ext(dt_develop_t *dev, const int32_t imgid)
{
  if(imgid == UNKNOWN_IMAGE) return FALSE;

  // This should be inited already when creating a pipeline or entering darkroom
  // but some pathes don't handle it, so make sure we have modules loaded.
  if(IS_NULL_PTR(dev->iop))
    dev->iop = dt_dev_load_modules(dev);

  // Ensure raw metadata (WB coeffs, matrices, etc.) is available for modules that
  // query it while (re)loading defaults (e.g. temperature/colorin).
  // This is redundant with `_dt_dev_load_raw()` called from `dt_dev_load_image()`,
  // but some call sites reload history without guaranteeing a prior FULL open.
  if(dt_dev_ensure_image_storage(dev, imgid)) 
    return FALSE;

  // Start fresh
  dt_dev_history_free_history(dev);
  int legacy_params = 0;
  dt_ioppr_set_default_iop_order(dev, imgid);

  // Find the new history end from DB now, if defined.
  // Note: dt_dev_set_history_end_ext sanitizes the value with the actual history size.
  // It needs to run after dev->history is fully populated.
  int32_t history_end = dt_history_get_end(imgid);

  // Find out if we already have an history, and how many items  
  const int32_t db_items = dt_history_db_get_next_history_num(imgid);

  // Load all the modules that may be required by the image format,
  // plus auto-presets if it's a new edit
  gboolean first_run = dt_dev_init_default_history(dev, imgid, (db_items == 0));

  // Protect history DB reads with a cache read lock.
  // Release it before applying history to modules to avoid deadlocks.
  dt_image_t *read_lock_img = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  if(IS_NULL_PTR(read_lock_img)) return FALSE;

  // Load DB history into dev->history
  dt_dev_history_db_ctx_t ctx = { .dev = dev, .imgid = imgid, .legacy_params = &legacy_params, .presets = FALSE };
  dt_history_db_foreach_history_row(imgid, _dev_history_db_row_cb, &ctx);
  const int32_t history_length = g_list_length(dev->history);

  if(history_length > db_items)
  {
    // We have now more history entries than when reading DB,
    // meaning the sanitization step added required default modules.
    // We need to shift history end by the same amount to still point to the same entry.
    // This is valid whether history_end was 0 or not
    history_end += history_length - db_items;
  }

  // Set a provisional history_end so mask history can resolve current forms.
  // We will recompute hashes once all history items are committed below.
  dt_dev_set_history_end_ext(dev, history_end);

  // Sanitize and flatten module order
  dt_ioppr_resync_pipeline(dev, imgid, "dt_dev_read_history_no_image end", FALSE);

  // Update "masks history"
  // This design is stupid because `dt_dev_history_item_t *hist->forms` is not read
  // from the DB history items (`main.history`), but from the `main.masks_history`,
  // and later on, `dev->forms` is restored at the `history_end` index from dumping
  // the content of the `dt_dev_history_item_t *hist->forms` snapshot.
  // This only means that `hist->forms` doesn't belong to the `dt_dev_history_item_t *`
  // but should live in its own branch. See `dt_dev_pop_history_items_ext()`
  dt_masks_read_masks_history(dev, imgid);

  dt_image_cache_read_release(darktable.image_cache, read_lock_img);
  read_lock_img = NULL;

  // Now we have fully-populated history items:
  // Commit params to modules and publish the masks on the raster stack for other modules to find
  for(GList *history = g_list_first(dev->history); history; history = g_list_next(history))
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)history->data;
    if(!hist)
    {
      fprintf(stderr, "[dt_dev_read_history_ext] we have no history item. This is not normal.\n");
      continue;
    }
    else if(!hist->module)
    {
      dt_print(DT_DEBUG_HISTORY, "[history] keeping archival history item %s (%s) without live module binding\n",
               hist->op_name, hist->multi_name);
      continue;
    }

    dt_iop_module_t *module = hist->module;
    _history_to_module(hist, module);
    hist->hash = hist->module->hash;

    dt_print(DT_DEBUG_HISTORY, "[history] successfully loaded module %s history (enabled: %i)\n", hist->module->op, hist->enabled);
  }

  dt_dev_masks_list_change(dev);
  dt_dev_masks_update_hash(dev);

  dt_dev_set_history_end_ext(dev, history_end);
  // Note: dt_dev_set_history_end already updates dev->history_hash

  dt_print(DT_DEBUG_HISTORY, "[history] dt_dev_read_history_ext completed\n");
  return first_run;
}


void dt_dev_invalidate_history_module(GList *list, dt_iop_module_t *module)
{
  for(; list; list = g_list_next(list))
  {
    dt_dev_history_item_t *hitem = (dt_dev_history_item_t *)list->data;
    if (hitem->module == module)
    {
      hitem->module = NULL;
    }
  }
}

gboolean dt_history_module_skip_copy(const int flags)
{
  return flags & (IOP_FLAGS_DEPRECATED | IOP_FLAGS_UNSAFE_COPY | IOP_FLAGS_HIDDEN);
}

/**
 * @brief Return whether a module never writes history entries.
 *
 * @param module Module instance.
 * @return TRUE if module leaves no history, FALSE otherwise.
 */
gboolean _module_leaves_no_history(dt_iop_module_t *module)
{
  return (module->flags() & IOP_FLAGS_NO_HISTORY_STACK);
}

/**
 * @brief Check if a module is enabled by default or force-enabled.
 *
 * @param module Module instance.
 * @return TRUE if default/forced enabled, FALSE otherwise.
 */
static gboolean _module_is_default_or_forced_enabled(dt_iop_module_t *module)
{
  return module->default_enabled || (module->force_enable && module->force_enable(module, module->enabled));
}

/**
 * @brief Check if module params match defaults.
 *
 * @param module Module instance.
 * @return TRUE if params are default, FALSE otherwise.
 */
static gboolean _module_params_are_default(dt_iop_module_t *module)
{
  return module->has_defaults ? module->has_defaults(module) : TRUE;
}

/**
 * @brief Check if blend params match defaults.
 *
 * @param module Module instance.
 * @return TRUE if blend params are default, FALSE otherwise.
 */
static gboolean _module_blend_params_are_default(dt_iop_module_t *module)
{
  if(!module->blend_params || !module->default_blendop_params) return TRUE;

  return memcmp(module->blend_params, module->default_blendop_params,
                sizeof(dt_develop_blend_params_t)) == 0;
}

/**
 * @brief Check if any module params (including blend params) are non-default.
 *
 * @param module Module instance.
 * @return TRUE if any params are non-default, FALSE otherwise.
 */
static gboolean _module_has_nondefault_internal_params(dt_iop_module_t *module)
{
  return !_module_params_are_default(module) || !_module_blend_params_are_default(module);
}

typedef gboolean (*dt_iop_module_filter_t)(dt_iop_module_t *module);

/**
 * @brief Append history items for modules that pass a filter.
 *
 * @param dev Develop context.
 * @param filter Predicate deciding whether a module should be added.
 */
static void _dev_history_add_filtered(dt_develop_t *dev, dt_iop_module_filter_t filter)
{
  for(GList *item = g_list_first(dev->iop); item; item = g_list_next(item))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(item->data);
    if(!_module_leaves_no_history(module) && filter(module))
      dt_dev_add_history_item_ext(dev, module, FALSE, TRUE);
  }
}

/**
 * @brief Filter: enabled modules that are default/forced enabled.
 */
static gboolean _compress_enabled_default_or_forced(dt_iop_module_t *module)
{
  return module->enabled && _module_is_default_or_forced_enabled(module);
}

/**
 * @brief Filter: enabled modules with default params (user enabled).
 */
static gboolean _compress_enabled_user_default_params(dt_iop_module_t *module)
{
  return module->enabled && !_module_is_default_or_forced_enabled(module)
         && _module_params_are_default(module);
}

/**
 * @brief Filter: enabled modules with non-default params (user edits).
 */
static gboolean _compress_enabled_user_nondefault_params(dt_iop_module_t *module)
{
  return module->enabled && !_module_is_default_or_forced_enabled(module)
         && !_module_params_are_default(module);
}

/**
 * @brief Filter: disabled modules that still need history entries.
 */
static gboolean _compress_disabled_with_history(dt_iop_module_t *module)
{
  return !module->enabled
         && (module->default_enabled || module->workflow_enabled || _module_has_nondefault_internal_params(module));
}

/**
 * @brief Rebuild history from current pipeline state.
 *
 * Creates a compact history snapshot and optionally writes it to DB.
 *
 * @param dev Develop context.
 * @param write_history Whether to write to DB/XMP after compression.
 */
static void _dt_dev_history_compress_internal(dt_develop_t *dev, const gboolean write_history)
{
  const int32_t imgid = dev->image_storage.id;

  dt_pthread_rwlock_wrlock(&dev->history_mutex);

  // Cleanup old history
  dt_dev_history_free_history(dev);

  // Rebuild an history from current pipeline.
  // First: modules enabled by default or forced enabled for technical reasons
  _dev_history_add_filtered(dev, _compress_enabled_default_or_forced);

  // Second: modules enabled by user
  // 2.1 : start with modules that still have default params,
  _dev_history_add_filtered(dev, _compress_enabled_user_default_params);

  // 2.2 : then modules that are set to non-default
  _dev_history_add_filtered(dev, _compress_enabled_user_nondefault_params);

  // Third: disabled modules that have history because they were enabled by default or
  // because their internal params (including blendops) differ from defaults. Maybe users
  // want to re-enable them later, or it's modules enabled by default that were manually disabled.
  // Put them the end of the history, so user can truncate it after the last enabled item
  // to get rid of disabled history if needed.
  _dev_history_add_filtered(dev, _compress_disabled_with_history);

  dt_dev_set_history_end_ext(dev, g_list_length(dev->history));
  dt_dev_pop_history_items_ext(dev);
  if(write_history) dt_dev_write_history_ext(dev, imgid);

  dt_pthread_rwlock_unlock(&dev->history_mutex);
}

void dt_dev_history_compress_ext(dt_develop_t *dev, gboolean write_history)
{
  _dt_dev_history_compress_internal(dev, write_history);
}

void dt_dev_history_compress(dt_develop_t *dev)
{
  _dt_dev_history_compress_internal(dev, TRUE);
}

void dt_dev_history_truncate(dt_develop_t *dev, const int32_t imgid)
{
  // Remove tail entries (num >= end).
  // history_end is a cursor expressed in "number of applied items" terms:
  // - keep items [0..history_end-1]
  // - remove items [history_end..]
  GList *link = g_list_nth(dev->history, dt_dev_get_history_end_ext(dev));
  while(link)
  {
    GList *next = g_list_next(link);
    dt_dev_free_history_item(link->data);
    dev->history = g_list_delete_link(dev->history, link);
    link = next;
  }

  // Re-apply history and resync iop order from the truncated stack.
  dt_dev_pop_history_items_ext(dev);
  dt_dev_write_history_ext(dev, imgid);
}

void dt_dev_history_compress_or_truncate(dt_develop_t *dev)
{
  if(dt_dev_get_history_end_ext(dev) == g_list_length(dev->history))
    dt_dev_history_compress(dev);
  else
    dt_dev_history_truncate(dev, dev->image_storage.id);
}


/**
 * @brief Detect and handle module instances that exist in iop list but not in history.
 *
 * @param dev Develop context.
 * @param _iop_list Pointer to module list to mutate.
 * @param history_list History list.
 * @return 0 on success, non-zero on error.
 */
static int _check_deleted_instances(dt_develop_t *dev, GList **_iop_list, GList *history_list)
{
  GList *iop_list = *_iop_list;
  int deleted_module_found = 0;

  // we will check on dev->iop if there's a module that is not in history
  GList *modules = iop_list;
  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    if(IS_NULL_PTR(mod)) continue;

    int delete_module = 0;

    // base modules are a special case
    // most base modules won't be in history and must not be deleted
    // but the user may have deleted a base instance of a multi-instance module
    // and then undo and redo, so we will end up with two entries in dev->iop
    // with multi_priority == 0, this can't happen and the extra one must be deleted
    // dev->iop is sorted by (priority, multi_priority DESC), so if the next one is
    // a base instance too, one must be deleted
    if(mod->multi_priority == 0)
    {
      GList *modules_next = g_list_next(modules);
      if(modules_next)
      {
        dt_iop_module_t *mod_next = (dt_iop_module_t *)modules_next->data;
        if(strcmp(mod_next->op, mod->op) == 0 && mod_next->multi_priority == 0)
        {
          // is the same one, check which one must be deleted
          const int mod_in_history = (dt_dev_history_get_first_item_by_module(history_list, mod) != NULL);
          const int mod_next_in_history = (dt_dev_history_get_first_item_by_module(history_list, mod_next) != NULL);

          // current is in history and next is not, delete next
          if(mod_in_history && !mod_next_in_history)
          {
            mod = mod_next;
            modules = modules_next;
            delete_module = 1;
          }
          // current is not in history and next is, delete current
          else if(!mod_in_history && mod_next_in_history)
          {
            delete_module = 1;
          }
          else
          {
            if(mod_in_history && mod_next_in_history)
              fprintf(
                  stderr,
                  "[_check_deleted_instances] found duplicate module %s %s (%i) and %s %s (%i) both in history\n",
                  mod->op, mod->multi_name, mod->multi_priority, mod_next->op, mod_next->multi_name,
                  mod_next->multi_priority);
            else
              fprintf(
                  stderr,
                  "[_check_deleted_instances] found duplicate module %s %s (%i) and %s %s (%i) none in history\n",
                  mod->op, mod->multi_name, mod->multi_priority, mod_next->op, mod_next->multi_name,
                  mod_next->multi_priority);
          }
        }
      }
    }
    // this is a regular multi-instance and must be in history
    else
    {
      delete_module = (dt_dev_history_get_first_item_by_module(history_list, mod) == NULL);
    }

    // if module is not in history we delete it
    if(delete_module && mod)
    {
      deleted_module_found = 1;

      if(darktable.develop->gui_module == mod) dt_iop_request_focus(NULL);

      ++darktable.gui->reset;

      // we remove the plugin effectively
      if(!dt_iop_is_hidden(mod))
      {
        // we just hide the module to avoid lots of gtk critical warnings
        gtk_widget_hide(mod->expander);

        // this is copied from dt_iop_gui_delete_callback(), not sure why the above sentence...
        dt_iop_gui_cleanup_module(mod);
        gtk_widget_destroy(mod->widget);
      }

      iop_list = g_list_remove_link(iop_list, modules);

      // remove the module reference from all snapshots
      dt_undo_iterate_internal(darktable.undo, DT_UNDO_HISTORY, mod, &_history_invalidate_cb);

      // don't delete the module, a pipe may still need it
      dev->alliop = g_list_append(dev->alliop, mod);

      --darktable.gui->reset;

      // and reset the list
      modules = iop_list;
      continue;
    }

    modules = g_list_next(modules);
  }
  if(deleted_module_found) iop_list = g_list_sort(iop_list, dt_sort_iop_by_order);

  *_iop_list = iop_list;

  return deleted_module_found;
}

/**
 * @brief Resync module multi_priority values from history.
 *
 * @param history_list History list.
 * @return 1 if changes were made, 0 otherwise.
 */
static int _rebuild_multi_priority(GList *history_list)
{
  int changed = 0;
  for(const GList *history = history_list; history; history = g_list_next(history))
  {
    dt_dev_history_item_t *hitem = (dt_dev_history_item_t *)history->data;

    // if multi_priority is different in history and dev->iop
    // we keep the history version
    if(hitem->module && hitem->module->multi_priority != hitem->multi_priority)
    {
      dt_iop_update_multi_priority(hitem->module, hitem->multi_priority);
      changed = 1;
    }
  }
  return changed;
}

/**
 * @brief Rebind history items to a module instance after recreation.
 *
 * @param hist History list.
 * @param module Module instance.
 * @param multi_priority Instance priority to match.
 */
static void _reset_module_instance(GList *hist, dt_iop_module_t *module, int multi_priority)
{
  for(; hist; hist = g_list_next(hist))
  {
    dt_dev_history_item_t *hit = (dt_dev_history_item_t *)hist->data;

    if(IS_NULL_PTR(hit->module) && strcmp(hit->op_name, module->op) == 0 && hit->multi_priority == multi_priority)
    {
      hit->module = module;
    }
  }
}

/**
 * @brief Undo iterator callback to fix module pointers in snapshots.
 *
 * @param user_data Callback data (struct _cb_data).
 * @param type Undo record type.
 * @param data Undo record payload.
 */
static void _undo_items_cb(gpointer user_data, dt_undo_type_t type, dt_undo_data_t data)
{
  struct _cb_data *udata = (struct _cb_data *)user_data;
  dt_undo_history_t *hdata = (dt_undo_history_t *)data;
  _reset_module_instance(hdata->after_snapshot, udata->module, udata->multi_priority);
}

/**
 * @brief Recreate missing module instances referenced by history.
 *
 * This is used during undo/redo when history refers to modules that
 * were deleted from the live module list.
 *
 * @param _iop_list Pointer to module list to mutate.
 * @param history_list History list.
 * @return 1 if changes were made, 0 otherwise.
 */
static int _create_deleted_modules(GList **_iop_list, GList *history_list)
{
  GList *iop_list = *_iop_list;
  int changed = 0;
  gboolean done = FALSE;

  GList *l = history_list;
  while(l)
  {
    GList *next = g_list_next(l);
    dt_dev_history_item_t *hitem = (dt_dev_history_item_t *)l->data;

    // this fixes the duplicate module when undo: hitem->multi_priority = 0;
    if(IS_NULL_PTR(hitem->module))
    {
      changed = 1;

      const dt_iop_module_t *base_module = dt_iop_get_module_from_list(iop_list, hitem->op_name);
      if(IS_NULL_PTR(base_module))
      {
        fprintf(stderr, "[_create_deleted_modules] can't find base module for %s\n", hitem->op_name);
        return changed;
      }

      // from there we create a new module for this base instance. The goal is to do a very minimal setup of the
      // new module to be able to write the history items. From there we reload the whole history back and this
      // will recreate the proper module instances.
      dt_iop_module_t *module = (dt_iop_module_t *)calloc(1, sizeof(dt_iop_module_t));
      if(dt_iop_load_module(module, base_module->so, base_module->dev))
      {
        return changed;
      }
      module->instance = base_module->instance;

      if(!dt_iop_is_hidden(module))
      {
        ++darktable.gui->reset;
        module->gui_init(module);
        --darktable.gui->reset;
      }

      // adjust the multi_name of the new module
      g_strlcpy(module->multi_name, hitem->multi_name, sizeof(module->multi_name));
      dt_iop_update_multi_priority(module, hitem->multi_priority);
      module->iop_order = hitem->iop_order;

      // we insert this module into dev->iop
      iop_list = g_list_insert_sorted(iop_list, module, dt_sort_iop_by_order);

      // if not already done, set the module to all others same instance
      if(!done)
      {
        _reset_module_instance(history_list, module, hitem->multi_priority);

        // and do that also in the undo/redo lists
        struct _cb_data udata = { module, hitem->multi_priority };
        dt_undo_iterate_internal(darktable.undo, DT_UNDO_HISTORY, &udata, &_undo_items_cb);
        done = TRUE;
      }

      hitem->module = module;
    }
    l = next;
  }

  *_iop_list = iop_list;

  return changed;
}


// returns 1 if the topology of the pipe has changed, aka it needs a full rebuild
// 0 means only internal parameters of pipe nodes have change, so it's a mere resync
int dt_dev_history_refresh_nodes_ext(dt_develop_t *dev, GList **iop, GList *history)
{
  GList *iop_list = *iop;

  // topology has changed?
  int pipe_remove = 0;

  // we have to check if multi_priority has changed since history was saved
  // we will adjust it here
  if(_rebuild_multi_priority(history))
  {
    pipe_remove = 1;
    iop_list = g_list_sort(iop_list, dt_sort_iop_by_order);
  }

  // check if this undo a delete module and re-create it
  if(_create_deleted_modules(&iop_list, history))
    pipe_remove = 1;

  // check if this is a redo of a delete module or an undo of an add module
  if(_check_deleted_instances(dev, &iop_list, history))
    pipe_remove = 1;

  *iop = iop_list;

  // if topology has changed, we need to reorder modules in GUI
  if(pipe_remove) dt_dev_signal_modules_moved(dev);

  return pipe_remove;
}
