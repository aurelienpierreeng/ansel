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
#include "common/undo.h"
#include "common/history_snapshot.h"
#include "common/image_cache.h"
#include "develop/dev_history.h"
#include "develop/blend.h"
#include "develop/imageop.h"
#include "develop/masks.h"

#include "gui/presets.h"

#include <inttypes.h>

#include <glib.h>

static void _process_history_db_entry(dt_develop_t *dev, sqlite3_stmt *stmt, const int32_t imgid,
                                      int *legacy_params, gboolean presets);

static sqlite3_stmt *_dev_history_delete_history_stmt = NULL;
static sqlite3_stmt *_dev_history_delete_masks_stmt = NULL;
static sqlite3_stmt *_dev_history_update_history_end_stmt = NULL;
static sqlite3_stmt *_dev_history_select_num_stmt = NULL;
static sqlite3_stmt *_dev_history_insert_num_stmt = NULL;
static sqlite3_stmt *_dev_history_update_item_stmt = NULL;
static dt_pthread_mutex_t _dev_history_stmt_mutex;
static gsize _dev_history_stmt_mutex_inited = 0;

static inline void _dev_history_stmt_mutex_ensure(void)
{
  if(g_once_init_enter(&_dev_history_stmt_mutex_inited))
  {
    dt_pthread_mutex_init(&_dev_history_stmt_mutex, NULL);
    g_once_init_leave(&_dev_history_stmt_mutex_inited, 1);
  }
}

// returns the first history item with hist->module == module
dt_dev_history_item_t *dt_dev_history_get_first_item_by_module(dt_develop_t *dev, dt_iop_module_t *module)
{
  dt_dev_history_item_t *hist_mod = NULL;
  for(GList *history = g_list_first(dev->history); history; history = g_list_next(history))
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

// returns the first history item with corresponding module->op
static dt_dev_history_item_t *_search_history_by_op(dt_develop_t *dev, dt_iop_module_t *module)
{
  dt_dev_history_item_t *hist_mod = NULL;
  for(GList *history = g_list_first(dev->history); history; history = g_list_next(history))
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);

    if(strcmp(hist->module->op, module->op) == 0)
    {
      hist_mod = hist;
      break;
    }
  }
  return hist_mod;
}

const dt_dev_history_item_t *_get_last_history_item_for_module(dt_develop_t *dev, struct dt_iop_module_t *module)
{
  for(GList *l = g_list_last(dev->history); l; l = g_list_previous(l))
  {
    dt_dev_history_item_t *item = (dt_dev_history_item_t *)l->data;
    if(item->module == module)
      return item;
  }
  return NULL;
}

// fills used with formid, if it is a group it recurs and fill all sub-forms
static void _fill_used_forms(GList *forms_list, int formid, int *used, int nb)
{
  // first, we search for the formid in used table
  for(int i = 0; i < nb; i++)
  {
    if(used[i] == 0)
    {
      // we store the formid
      used[i] = formid;
      break;
    }
    if(used[i] == formid) break;
  }

  // if the form is a group, we iterate through the sub-forms
  dt_masks_form_t *form = dt_masks_get_from_id_ext(forms_list, formid);
  if(form && (form->type & DT_MASKS_GROUP))
  {
    for(GList *grpts = form->points; grpts; grpts = g_list_next(grpts))
    {
      dt_masks_form_group_t *grpt = (dt_masks_form_group_t *)grpts->data;
      _fill_used_forms(forms_list, grpt->formid, used, nb);
    }
  }
}

// dev_src is used only to copy masks, if no mask will be copied it can be null
int dt_history_merge_module_into_history(dt_develop_t *dev_dest, dt_develop_t *dev_src, dt_iop_module_t *mod_src, GList **_modules_used)
{
  int module_added = 1;
  GList *modules_used = *_modules_used;
  dt_iop_module_t *module = NULL;
  dt_iop_module_t *mod_replace = NULL;

  // one-instance modules always replace the existing one
  if(mod_src->flags() & IOP_FLAGS_ONE_INSTANCE)
  {
    mod_replace = dt_iop_get_module_by_op_priority(dev_dest->iop, mod_src->op, -1);
    if(mod_replace == NULL)
    {
      fprintf(stderr, "[dt_history_merge_module_into_history] can't find single instance module %s\n",
              mod_src->op);
      module_added = 0;
    }
    else
    {
      dt_print(DT_DEBUG_HISTORY, "[dt_history_merge_module_into_history] %s (%s) will be overriden in target history by parameters from source history\n", mod_src->name(), mod_src->multi_name);
    }
  }

  if(module_added && mod_replace == NULL)
  {
    // we haven't found a module to replace, so we will create a new instance
    // but if there's an un-used instance on dev->iop we will use that

    if(_search_history_by_op(dev_dest, mod_src) == NULL)
    {
      // there should be only one instance of this iop (since is un-used)
      mod_replace = dt_iop_get_module_by_op_priority(dev_dest->iop, mod_src->op, -1);
      if(mod_replace == NULL)
      {
        fprintf(stderr, "[dt_history_merge_module_into_history] can't find base instance module %s\n", mod_src->op);
        module_added = 0;
      }
      else
      {
        dt_print(DT_DEBUG_HISTORY, "[dt_history_merge_module_into_history] %s (%s) will be enabled in target history with parameters from source history\n", mod_src->name(), mod_src->multi_name);
      }
    }
  }

  if(module_added)
  {
    // if we are creating a new instance, create a new module
    if(mod_replace == NULL)
    {
      dt_iop_module_t *base = dt_iop_get_module_by_op_priority(dev_dest->iop, mod_src->op, -1);
      module = (dt_iop_module_t *)calloc(1, sizeof(dt_iop_module_t));
      if(dt_iop_load_module(module, base->so, dev_dest))
      {
        fprintf(stderr, "[dt_history_merge_module_into_history] can't load module %s\n", mod_src->op);
        module_added = 0;
      }
      else
      {
        module->instance = mod_src->instance;
        module->multi_priority = mod_src->multi_priority;
        module->iop_order = dt_ioppr_get_iop_order(dev_dest->iop_order_list, module->op, module->multi_priority);
        dt_print(DT_DEBUG_HISTORY, "[dt_history_merge_module_into_history] %s (%s) will be inserted as a new instance in target history\n", mod_src->name(), mod_src->multi_name);
      }
    }
    else
    {
      module = mod_replace;
    }

    module->enabled = mod_src->enabled;
    g_strlcpy(module->multi_name, mod_src->multi_name, sizeof(module->multi_name));

    memcpy(module->params, mod_src->params, module->params_size);
    if(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
    {
      memcpy(module->blend_params, mod_src->blend_params, sizeof(dt_develop_blend_params_t));
      module->blend_params->mask_id = mod_src->blend_params->mask_id;
    }
  }

  // we have the module, we will use the source module iop_order unless there's already
  // a module with that order
  if(module_added)
  {
    dt_iop_module_t *module_duplicate = NULL;
    // check if there's a module with the same iop_order
    for( GList *modules_dest = g_list_first(dev_dest->iop); modules_dest; modules_dest = g_list_next(modules_dest))
    {
      dt_iop_module_t *mod = (dt_iop_module_t *)(modules_dest->data);

      if(module_duplicate != NULL)
      {
        module_duplicate = mod;
        break;
      }
      if(mod->iop_order == mod_src->iop_order && mod != module)
      {
        module_duplicate = mod;
      }
    }

    // do some checking...
    if(mod_src->iop_order <= 0.0 || mod_src->iop_order == INT_MAX)
      fprintf(stderr, "[dt_history_merge_module_into_history] invalid source module %s %s(%d)(%i)\n",
          mod_src->op, mod_src->multi_name, mod_src->iop_order, mod_src->multi_priority);
    if(module_duplicate && (module_duplicate->iop_order <= 0.0 || module_duplicate->iop_order == INT_MAX))
      fprintf(stderr, "[dt_history_merge_module_into_history] invalid duplicate module module %s %s(%d)(%i)\n",
          module_duplicate->op, module_duplicate->multi_name, module_duplicate->iop_order, module_duplicate->multi_priority);
    if(module->iop_order <= 0.0 || module->iop_order == INT_MAX)
      fprintf(stderr, "[dt_history_merge_module_into_history] invalid iop_order for module %s %s(%d)(%i)\n",
          module->op, module->multi_name, module->iop_order, module->multi_priority);

    // if this is a new module just add it to the list
    if(mod_replace == NULL)
      dev_dest->iop = g_list_insert_sorted(dev_dest->iop, module, dt_sort_iop_by_order);
    else
      dev_dest->iop = g_list_sort(dev_dest->iop, dt_sort_iop_by_order);
  }

  // and we add it to history
  if(module_added)
  {
    dt_print(DT_DEBUG_HISTORY, "[dt_history_merge_module_into_history] %s (%s) was at position %i in source pipeline, now is at position %i\n", mod_src->name(), mod_src->multi_name, mod_src->iop_order, module->iop_order);

    // copy masks
    guint nbf = 0;
    int *forms_used_replace = NULL;

    if(dev_src)
    {
      // we will copy only used forms
      // record the masks used by this module
      if(mod_src->flags() & IOP_FLAGS_SUPPORTS_BLENDING && mod_src->blend_params->mask_id > 0)
      {
        nbf = g_list_length(dev_src->forms);
        forms_used_replace = calloc(nbf, sizeof(int));

        _fill_used_forms(dev_src->forms, mod_src->blend_params->mask_id, forms_used_replace, nbf);

        // now copy masks
        for(int i = 0; i < nbf && forms_used_replace[i] > 0; i++)
        {
          dt_masks_form_t *form = dt_masks_get_from_id(dev_src, forms_used_replace[i]);
          if(form)
          {
            // check if the form already exists in dest image
            // if so we'll remove it, so it is replaced
            dt_masks_form_t *form_dest = dt_masks_get_from_id_ext(dev_dest->forms, forms_used_replace[i]);
            if(form_dest)
            {
              dev_dest->forms = g_list_remove(dev_dest->forms, form_dest);
              // and add it to allforms to cleanup
              dev_dest->allforms = g_list_append(dev_dest->allforms, form_dest);
            }

            // and add it to dest image
            dt_masks_form_t *form_new = dt_masks_dup_masks_form(form);
            dev_dest->forms = g_list_append(dev_dest->forms, form_new);
          }
          else
            fprintf(stderr, "[dt_history_merge_module_into_history] form %i not found in source image\n", forms_used_replace[i]);
        }
      }
    }

    dt_dev_add_history_item_ext(dev_dest, module, FALSE, FALSE, TRUE, TRUE);

    dt_ioppr_resync_modules_order(dev_dest);

    dt_ioppr_check_iop_order(dev_dest, 0, "dt_history_merge_module_into_history");

    dt_dev_pop_history_items_ext(dev_dest);

    if(forms_used_replace) free(forms_used_replace);
  }

  *_modules_used = modules_used;

  return module_added;
}

static int _history_copy_and_paste_on_image_merge(int32_t imgid, int32_t dest_imgid, GList *ops, const gboolean copy_full)
{
  GList *modules_used = NULL;

  dt_develop_t _dev_src = { 0 };
  dt_develop_t _dev_dest = { 0 };

  dt_develop_t *dev_src = &_dev_src;
  dt_develop_t *dev_dest = &_dev_dest;

  // we will do the copy/paste on memory so we can deal with masks
  dt_dev_init(dev_src, FALSE);
  dt_dev_init(dev_dest, FALSE);

  dt_dev_read_history_ext(dev_src, imgid, TRUE);

  // This prepends the default modules and converts just in case it's an empty history
  dt_dev_read_history_ext(dev_dest, dest_imgid, TRUE);

  dt_ioppr_check_iop_order(dev_src, imgid, "_history_copy_and_paste_on_image_merge ");
  dt_ioppr_check_iop_order(dev_dest, dest_imgid, "_history_copy_and_paste_on_image_merge ");

  dt_dev_pop_history_items_ext(dev_src);
  dt_dev_pop_history_items_ext(dev_dest);

  dt_ioppr_check_iop_order(dev_src, imgid, "_history_copy_and_paste_on_image_merge 1");
  dt_ioppr_check_iop_order(dev_dest, dest_imgid, "_history_copy_and_paste_on_image_merge 1");

  GList *mod_list = NULL;

  if(ops)
  {
    dt_print(DT_DEBUG_PARAMS, "[_history_copy_and_paste_on_image_merge] pasting selected IOP\n");

    // copy only selected history entries
    for(const GList *l = g_list_last(ops); l; l = g_list_previous(l))
    {
      const unsigned int num = GPOINTER_TO_UINT(l->data);
      const dt_dev_history_item_t *hist = g_list_nth_data(dev_src->history, num);

      if(hist)
      {
        if(!dt_iop_is_hidden(hist->module))
        {
          dt_print(DT_DEBUG_IOPORDER, "\n  module %20s, multiprio %i", hist->module->op,
                   hist->module->multi_priority);

          mod_list = g_list_prepend(mod_list, hist->module);
        }
      }
    }
  }
  else
  {
    dt_print(DT_DEBUG_PARAMS, "[_history_copy_and_paste_on_image_merge] pasting all IOP\n");

    // we will copy all modules
    for(GList *modules_src = g_list_first(dev_src->iop); modules_src; modules_src = g_list_next(modules_src))
    {
      dt_iop_module_t *mod_src = (dt_iop_module_t *)(modules_src->data);

      // copy from history only if
      if((dt_dev_history_get_first_item_by_module(dev_src, mod_src) != NULL) // module is in history of source image
         && !dt_iop_is_hidden(mod_src) // hidden modules are technical and special
         && (copy_full || !dt_history_module_skip_copy(mod_src->flags()))
        )
      {
        // Note: we prepend to GList because it's more efficient
        mod_list = g_list_prepend(mod_list, mod_src);
      }
    }
  }

  mod_list = g_list_reverse(mod_list);   // list was built in reverse order, so un-reverse it

  // update iop-order list to have entries for the new modules
  dt_ioppr_update_for_modules(dev_dest, mod_list, FALSE);

  for(GList *l = g_list_first(mod_list); l; l = g_list_next(l))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)l->data;
    dt_history_merge_module_into_history(dev_dest, dev_src, mod, &modules_used);
  }

  // update iop-order list to have entries for the new modules
  dt_ioppr_update_for_modules(dev_dest, mod_list, FALSE);

  dt_ioppr_check_iop_order(dev_dest, dest_imgid, "_history_copy_and_paste_on_image_merge 2");

  // write history and forms to db
  dt_dev_write_history_ext(dev_dest, dest_imgid);

  dt_dev_cleanup(dev_src);
  dt_dev_cleanup(dev_dest);

  g_list_free(modules_used);

  return 0;
}

gboolean dt_history_copy_and_paste_on_image(const int32_t imgid, const int32_t dest_imgid, GList *ops,
                                       const gboolean copy_iop_order, const gboolean copy_full)
{
  if(imgid == dest_imgid) return 1;

  if(imgid == UNKNOWN_IMAGE)
  {
    dt_control_log(_("you need to copy history from an image before you paste it onto another"));
    return 1;
  }

  dt_undo_lt_history_t *hist = dt_history_snapshot_item_init();
  hist->imgid = dest_imgid;
  dt_history_snapshot_undo_create(hist->imgid, &hist->before, &hist->before_history_end);

  if(copy_iop_order)
  {
    GList *iop_list = dt_ioppr_get_iop_order_list(imgid, FALSE);
    dt_ioppr_write_iop_order_list(iop_list, dest_imgid);
    g_list_free_full(iop_list, g_free);
  }

  int ret_val = _history_copy_and_paste_on_image_merge(imgid, dest_imgid, ops, copy_full);

  dt_history_snapshot_undo_create(hist->imgid, &hist->after, &hist->after_history_end);
  dt_undo_start_group(darktable.undo, DT_UNDO_LT_HISTORY);
  dt_undo_record(darktable.undo, NULL, DT_UNDO_LT_HISTORY, (dt_undo_data_t)hist,
                 dt_history_snapshot_undo_pop, dt_history_snapshot_undo_lt_history_data_free);
  dt_undo_end_group(darktable.undo);

  /* update xmp file */
  dt_control_save_xmp(dest_imgid);

  // signal that the mipmap need to be updated
  dt_thumbtable_refresh_thumbnail(darktable.gui->ui->thumbtable_lighttable, dest_imgid, TRUE);

  return ret_val;
}

GList *dt_history_duplicate(GList *hist)
{
  GList *result = NULL;
  for(GList *h = g_list_first(hist); h; h = g_list_next(h))
  {
    const dt_dev_history_item_t *old = (dt_dev_history_item_t *)(h->data);
    dt_dev_history_item_t *new = (dt_dev_history_item_t *)malloc(sizeof(dt_dev_history_item_t));

    memcpy(new, old, sizeof(dt_dev_history_item_t));

    dt_iop_module_t *module = (old->module) ? old->module : dt_iop_get_module(old->op_name);

    if(module && module->params_size > 0)
    {
      new->params = malloc(module->params_size);
      memcpy(new->params, old->params, module->params_size);
    }

    if(!module)
      fprintf(stderr, "[_duplicate_history] can't find base module for %s\n", old->op_name);

    new->blend_params = malloc(sizeof(dt_develop_blend_params_t));
    memcpy(new->blend_params, old->blend_params, sizeof(dt_develop_blend_params_t));

    if(old->forms) new->forms = dt_masks_dup_forms_deep(old->forms, NULL);

    result = g_list_prepend(result, new);
  }

  return g_list_reverse(result);  // list was built in reverse order, so un-reverse it
}


static dt_iop_module_t * _find_mask_manager(dt_develop_t *dev)
{
  for(GList *module = g_list_first(dev->iop); module; module = g_list_next(module))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)(module->data);
    if(strcmp(mod->op, "mask_manager") == 0)
      return mod;
  }
  return NULL;
}

static void _remove_history_leaks(dt_develop_t *dev)
{
  GList *history = g_list_nth(dev->history, dt_dev_get_history_end(dev));
  while(history)
  {
    // We need to use a while because we are going to dynamically remove entries at the end
    // of the list, so we can't know the number of iterations
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);
    dt_print(DT_DEBUG_HISTORY, "[dt_dev_add_history_item_ext] history item %s at %i is past history limit (%i)\n", hist->module->op, g_list_index(dev->history, hist), dt_dev_get_history_end(dev) - 1);

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
                                     gboolean force_new_item, gboolean no_image, gboolean include_masks)
{
  // If this history item is the first for this module,
  // we need to notify the pipeline that its topology may change (aka insert a new node).
  // Since changing topology is expensive, we want to do it only when needed.
  gboolean add_new_pipe_node = FALSE;

  if(!module)
  {
    // module = NULL means a mask was changed from the mask manager and that's where this function is called.
    // Find it now, even though it is not enabled and won't be.
    module = _find_mask_manager(dev);
    if(module)
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
    // add_new_pipe_node = FALSE
  }
  else
  {
    const dt_dev_history_item_t *previous_item = _get_last_history_item_for_module(dev, module);
    // check if NULL first or prevous_item->module will segfault
    // We need to add a new pipeline node if:
    add_new_pipe_node = (previous_item == NULL)                         // it's the first history entry for this module
                        || (previous_item->enabled != module->enabled); // the previous history entry is disabled
    // if previous history entry is disabled and we don't have any other entry,
    // it is possible the pipeline will not have this node.
  }

  dt_dev_history_item_t *hist;
  if(force_new_item || !new_is_old)
  {
    // Create a new history entry
    hist = (dt_dev_history_item_t *)calloc(1, sizeof(dt_dev_history_item_t));
    hist->params = malloc(module->params_size);
    hist->blend_params = malloc(sizeof(dt_develop_blend_params_t));

    dev->history = g_list_append(dev->history, hist);

    hist->num = g_list_index(dev->history, hist);

    dt_print(DT_DEBUG_HISTORY, "[dt_dev_add_history_item_ext] new history entry added for %s at position %i\n",
            module->name(), hist->num);
  }
  else
  {
    // Reuse previous history entry
    hist = (dt_dev_history_item_t *)last->data;

    // Drawn masks are forced-resync later, free them now
    if(hist->forms) g_list_free_full(hist->forms, (void (*)(void *))dt_masks_free_form);

    dt_print(DT_DEBUG_HISTORY, "[dt_dev_add_history_item_ext] history entry reused for %s at position %i\n",
             module->name(), hist->num);
  }

  // Always resync history with all module internals
  if(enable) module->enabled = TRUE;
  hist->enabled = module->enabled;
  hist->module = module;
  hist->iop_order = module->iop_order;
  hist->multi_priority = module->multi_priority;
  g_strlcpy(hist->op_name, module->op, sizeof(hist->op_name));
  g_strlcpy(hist->multi_name, module->multi_name, sizeof(hist->multi_name));
  memcpy(hist->params, module->params, module->params_size);
  memcpy(hist->blend_params, module->blend_params, sizeof(dt_develop_blend_params_t));

  // Include masks if module supports blending and blending is on or if it's the mask manager
  include_masks = ((module->flags() & IOP_FLAGS_SUPPORTS_BLENDING) == IOP_FLAGS_SUPPORTS_BLENDING
                   && module->blend_params->mask_mode > DEVELOP_MASK_ENABLED)
                  || (module->flags() & IOP_FLAGS_INTERNAL_MASKS) == IOP_FLAGS_INTERNAL_MASKS;

  if(include_masks)
  {
    dt_print(DT_DEBUG_HISTORY, "[dt_dev_add_history_item_ext] committing masks for module %s at history position %i\n", module->name(), hist->num);
    // FIXME: this copies ALL drawn masks AND masks groups used by all modules to any module history using masks.
    // Kudos to the idiots who thought it would be reasonable. Expect database bloating and perf penalty.
    dt_pthread_rwlock_rdlock(&dev->masks_mutex);
    hist->forms = dt_masks_dup_forms_deep(dev->forms, NULL);
    dt_pthread_rwlock_unlock(&dev->masks_mutex);

    dev->forms_changed = FALSE; // reset
  }
  else
  {
    hist->forms = NULL;
  }

  if(include_masks && hist->forms)
    dt_print(DT_DEBUG_HISTORY, "[dt_dev_add_history_item_ext] masks committed for module %s at history position %i\n", module->name(), hist->num);
  else if(include_masks)
    dt_print(DT_DEBUG_HISTORY, "[dt_dev_add_history_item_ext] masks NOT committed for module %s at history position %i\n", module->name(), hist->num);

  // Refresh hashes now because they use enabled state and masks
  dt_iop_compute_module_hash(module, hist->forms);
  hist->hash = module->hash;

  // It is assumed that the last-added history entry is always on top
  // so its cursor index is always equal to the number of elements,
  // keeping in mind that history_end = 0 is the raw image, aka not a dev->history GList entry.
  // So dev->history_end = index of last history entry + 1 = length of history
  dt_dev_set_history_end(dev, g_list_length(dev->history));

  return add_new_pipe_node;
}

uint64_t dt_dev_history_get_hash(dt_develop_t *dev)
{
  uint64_t hash = 5381;
  for(GList *hist = g_list_nth(dev->history, dt_dev_get_history_end(dev) - 1);
      hist;
      hist = g_list_previous(hist))
  {
    dt_dev_history_item_t *item = (dt_dev_history_item_t *)hist->data;
    hash = dt_hash(hash, (const char *)&item->hash, sizeof(uint64_t));
  }
  dt_print(DT_DEBUG_HISTORY, "[dt_dev_history_get_hash] history hash: %" PRIu64 ", history end: %i, items %i\n", hash, dt_dev_get_history_end(dev), g_list_length(dev->history));
  return hash;
}


// The next 2 functions are always called from GUI controls setting parameters
// This is why they directly start a pipeline recompute.
// Otherwise, please keep GUI and pipeline fully separated.

void dt_dev_add_history_item_real(dt_develop_t *dev, dt_iop_module_t *module, gboolean enable, gboolean redraw)
{
  dt_atomic_set_int(&dev->pipe->shutdown, TRUE);
  dt_atomic_set_int(&dev->preview_pipe->shutdown, TRUE);

  dt_dev_undo_start_record(dev);

  // Run the delayed post-commit actions if implemented
  if(module && module->post_history_commit) module->post_history_commit(module);

  dt_pthread_rwlock_wrlock(&dev->history_mutex);
  dt_dev_add_history_item_ext(dev, module, enable, FALSE, FALSE, FALSE);
  dt_pthread_rwlock_unlock(&dev->history_mutex);

  /* signal that history has changed */
  dt_dev_undo_end_record(dev);

  // Figure out if the current history item includes masks/forms
  GList *last_history = g_list_nth(dev->history, dt_dev_get_history_end(dev) - 1);
  dt_dev_history_item_t *hist = NULL;
  gboolean has_forms = FALSE;
  if(last_history)
  {
    hist = (dt_dev_history_item_t *)last_history->data;
    has_forms = (hist->forms != NULL);
  }

  // Recompute pipeline last
  if(module && !(has_forms || (module->blend_params->blend_mode & DEVELOP_MASK_RASTER)))
  {
    // If we have a module and it doesn't use drawn or raster masks,
    // we only need to resync the top-most history item with pipeline
    dt_dev_pixelpipe_update_history_all(dev);
  }
  else
  {
    // We either don't have a module, meaning we have the mask manager, or
    // we have a module and it uses masks (drawn or raster).
    // Because masks can affect several modules anywhere, not necessarily sequentially,
    // we need a full resync of all pipeline with history.
    // Note that the blendop params (thus their hash) references the raster mask provider
    // in its consumer, and the consumer in its provider. So updating the whole pipe
    // resyncs the cumulative hashes too, and triggers a new recompute from the provider on update.
    dt_dev_pixelpipe_resync_history_all(dev);
  }

  dt_dev_masks_list_update(dev);

  if(darktable.gui && dev->gui_attached)
  {
    if(redraw) dt_dev_process_all(dev);
    
    if(module) 
    { 
      ++darktable.gui->reset; // don't run GUI callbacks when setting GUI state
      dt_iop_gui_set_enable_button(module);
      --darktable.gui->reset;
    }
  }
  
  // Save history straight away
  dt_dev_write_history_ext(dev, dev->image_storage.id);
}

void dt_dev_free_history_item(gpointer data)
{
  dt_dev_history_item_t *item = (dt_dev_history_item_t *)data;
  if(!item) return; // nothing to free

  g_free(item->params);
  item->params = NULL;
  g_free(item->blend_params);
  item->blend_params = NULL;
  g_list_free_full(item->forms, (void (*)(void *))dt_masks_free_form);
  item->forms = NULL;
  g_free(item);
  item = NULL;
}

void dt_dev_history_free_history(dt_develop_t *dev)
{
  g_list_free_full(g_steal_pointer(&dev->history), dt_dev_free_history_item);
  dev->history = NULL;
}

void dt_dev_reload_history_items(dt_develop_t *dev)
{
  // Recreate the whole history from scratch
  dt_pthread_rwlock_wrlock(&dev->history_mutex);
  dt_dev_history_free_history(dev);
  dt_dev_read_history_ext(dev, dev->image_storage.id, FALSE);
  dt_pthread_rwlock_unlock(&dev->history_mutex);
  dt_dev_pop_history_items(dev);
}


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

    dt_iop_compute_module_hash(module, dev->forms);
  }
}

// Dump the content of an history entry into its associated module params, blendops, etc.
static inline void _history_to_module(const dt_dev_history_item_t *const hist, dt_iop_module_t *module)
{
  module->enabled = hist->enabled;

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

  // Shitty design ahead:
  // some modules (temperature.c, colorin.c) init their GUI comboboxes
  // in/from reload_defaults. Though we already loaded them once at
  // _read_history_ext() when initing history, and history is now sanitized
  // such that all used module will have at least an entry,
  // it's not enough and we need to reload defaults here.
  // But anyway, if user truncated history before mandatory modules,
  // and we reload it here, it's good to ensure defaults are re-inited.
  _dt_dev_modules_reload_defaults(dev);

  // go through history and set modules params
  GList *history = g_list_first(dev->history);
  GList *forms = NULL;
  for(int i = 0; i < dt_dev_get_history_end(dev) && history; i++)
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);
    dt_iop_module_t *module = hist->module;
    _history_to_module(hist, module);

    if(hist->hash != module->hash)
      fprintf(stderr, "[dt_dev_pop_history_items] module hash is not consistent with history hash for %s : %" PRIu64 " != %" PRIu64 " \n",
              module->op, module->hash, hist->hash);

    if(hist->forms) forms = hist->forms;

    history = g_list_next(history);
  }

  dt_masks_replace_current_forms(dev, forms);
  dt_ioppr_resync_modules_order(dev);
  dt_ioppr_check_duplicate_iop_order(&dev->iop, dev->history);
  dt_ioppr_check_iop_order(dev, 0, "dt_dev_pop_history_items_ext end");
}

void dt_dev_pop_history_items(dt_develop_t *dev)
{
  dt_pthread_rwlock_wrlock(&dev->history_mutex);
  dt_dev_pop_history_items_ext(dev);
  dt_pthread_rwlock_unlock(&dev->history_mutex);

  ++darktable.gui->reset;

  // update all gui modules
  for(GList *module = g_list_first(dev->iop); module; module = g_list_next(module))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)(module->data);
    dt_iop_gui_update(mod);
  }

  dt_dev_reorder_gui_module_list(dev);
  dt_dev_modules_update_multishow(dev);
  dt_dev_modulegroups_update_visibility(dev);
  dt_dev_masks_list_change(dev);

  --darktable.gui->reset;

  dt_dev_pixelpipe_rebuild_all(dev);
  dt_dev_process_all(dev);
}

static void _cleanup_history(const int32_t imgid)
{
  _dev_history_stmt_mutex_ensure();
  dt_pthread_mutex_lock(&_dev_history_stmt_mutex);

  if(!_dev_history_delete_history_stmt)
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "DELETE FROM main.history WHERE imgid = ?1", -1,
                                &_dev_history_delete_history_stmt, NULL);
  sqlite3_stmt *stmt = _dev_history_delete_history_stmt;
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);

  if(!_dev_history_delete_masks_stmt)
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "DELETE FROM main.masks_history WHERE imgid = ?1", -1,
                                &_dev_history_delete_masks_stmt, NULL);
  stmt = _dev_history_delete_masks_stmt;
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);

  dt_pthread_mutex_unlock(&_dev_history_stmt_mutex);
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

static void _warn_about_history_overuse(GList *dev_history, int32_t imgid)
{
  /* History stores one entry per module, everytime a parameter is changed.
  *  For modules using masks, we also store a full snapshot of masks states.
  *  All that is saved into database and XMP. When history entries x number of mask > 250,
  *  we get a really bad performance penalty.
  */
  guint states = dt_dev_mask_history_overload(dev_history, 250);

  if(states > 250)
    dt_toast_log(_("Image #%i history is storing %d mask states. n"
                   "Consider compressing history and removing unused masks to keep reads/writes manageable."),
                   imgid, states);
}


void dt_dev_write_history_end_ext(const int history_end, const int32_t imgid)
{
  // update history end
  _dev_history_stmt_mutex_ensure();
  dt_pthread_mutex_lock(&_dev_history_stmt_mutex);
  if(!_dev_history_update_history_end_stmt)
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "UPDATE main.images SET history_end = ?1 WHERE id = ?2", -1,
                                &_dev_history_update_history_end_stmt, NULL);
  sqlite3_stmt *stmt = _dev_history_update_history_end_stmt;
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, history_end);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
  sqlite3_step(stmt);
  dt_pthread_mutex_unlock(&_dev_history_stmt_mutex);
}

// helper used to synch a single history item with db
int dt_dev_write_history_item(const int32_t imgid, dt_dev_history_item_t *h, int32_t num)
{
  dt_print(DT_DEBUG_HISTORY, "[dt_dev_write_history_item] writing history for module %s (%s) at pipe position %i for image %i...\n", h->op_name, h->multi_name, h->iop_order, imgid);

  _dev_history_stmt_mutex_ensure();
  dt_pthread_mutex_lock(&_dev_history_stmt_mutex);

  if(!_dev_history_select_num_stmt)
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT num FROM main.history WHERE imgid = ?1 AND num = ?2", -1,
                                &_dev_history_select_num_stmt, NULL);
  sqlite3_stmt *stmt = _dev_history_select_num_stmt;
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, num);
  if(sqlite3_step(stmt) != SQLITE_ROW)
  {
    if(!_dev_history_insert_num_stmt)
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "INSERT INTO main.history (imgid, num) VALUES (?1, ?2)", -1,
                                  &_dev_history_insert_num_stmt, NULL);
    stmt = _dev_history_insert_num_stmt;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, num);
    sqlite3_step(stmt);
  }

  if(!_dev_history_update_item_stmt)
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "UPDATE main.history"
                                " SET operation = ?1, op_params = ?2, module = ?3, enabled = ?4, "
                                "     blendop_params = ?7, blendop_version = ?8, multi_priority = ?9, multi_name = ?10"
                                " WHERE imgid = ?5 AND num = ?6",
                                -1, &_dev_history_update_item_stmt, NULL);
    // clang-format on
  stmt = _dev_history_update_item_stmt;
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, h->module->op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 2, h->params, h->module->params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, h->module->version());
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, h->enabled);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 5, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 6, num);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 7, h->blend_params, sizeof(dt_develop_blend_params_t), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 8, dt_develop_blend_version());
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 9, h->multi_priority);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 10, h->multi_name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);

  dt_pthread_mutex_unlock(&_dev_history_stmt_mutex);

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
  _dev_history_stmt_mutex_ensure();
  dt_pthread_mutex_lock(&_dev_history_stmt_mutex);

  if(_dev_history_delete_history_stmt)
  {
    sqlite3_finalize(_dev_history_delete_history_stmt);
    _dev_history_delete_history_stmt = NULL;
  }
  if(_dev_history_delete_masks_stmt)
  {
    sqlite3_finalize(_dev_history_delete_masks_stmt);
    _dev_history_delete_masks_stmt = NULL;
  }
  if(_dev_history_update_history_end_stmt)
  {
    sqlite3_finalize(_dev_history_update_history_end_stmt);
    _dev_history_update_history_end_stmt = NULL;
  }
  if(_dev_history_select_num_stmt)
  {
    sqlite3_finalize(_dev_history_select_num_stmt);
    _dev_history_select_num_stmt = NULL;
  }
  if(_dev_history_insert_num_stmt)
  {
    sqlite3_finalize(_dev_history_insert_num_stmt);
    _dev_history_insert_num_stmt = NULL;
  }
  if(_dev_history_update_item_stmt)
  {
    sqlite3_finalize(_dev_history_update_item_stmt);
    _dev_history_update_item_stmt = NULL;
  }

  dt_pthread_mutex_unlock(&_dev_history_stmt_mutex);

  if(_dev_history_stmt_mutex_inited)
  {
    dt_pthread_mutex_destroy(&_dev_history_stmt_mutex);
    _dev_history_stmt_mutex_inited = 0;
  }
}



void dt_dev_write_history_ext(dt_develop_t *dev, const int32_t imgid)
{
  dt_image_t *cache_img = dt_image_cache_get(darktable.image_cache, imgid, 'w');
  if(!cache_img) return;

  _warn_about_history_overuse(dev->history, imgid);

  dt_print(DT_DEBUG_HISTORY, "[dt_dev_write_history_ext] writing history for image %i...\n", imgid);

  _cleanup_history(imgid);

  // write history entries
  int i = 0;
  for(GList *history = g_list_first(dev->history); history; history = g_list_next(history))
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);
    dt_dev_write_history_item(imgid, hist, i);
    i++;
  }

  dt_dev_write_history_end_ext(dt_dev_get_history_end(dev), dev->image_storage.id);

  // write the current iop-order-list for this image
  dt_ioppr_write_iop_order_list(dev->iop_order_list, imgid);

  cache_img->history_hash = dev->history_hash;

  dt_image_cache_write_release(darktable.image_cache, cache_img, DT_IMAGE_CACHE_SAFE);
  cache_img = NULL;

  dt_mipmap_cache_remove(darktable.mipmap_cache, imgid, TRUE);

  // Don't refresh the thumbnail if we are in darkroom
  // Spawning another export thread will likely slow-down the current one.
  if(darktable.gui && dev != darktable.develop)
    dt_thumbtable_refresh_thumbnail(darktable.gui->ui->thumbtable_lighttable, imgid, TRUE);

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_IMAGE_INFO_CHANGED, g_list_append(NULL, GINT_TO_POINTER(imgid)));
}

// Write TO XMP, so from the dev perspective, it's a read
void dt_dev_write_history(dt_develop_t *dev)
{
  dt_pthread_rwlock_rdlock(&dev->history_mutex);
  dt_dev_write_history_ext(dev, dev->image_storage.id);
  dt_pthread_rwlock_unlock(&dev->history_mutex);
}

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

  sqlite3_stmt *stmt;
  // clang-format off
  char *query = g_strdup_printf(
  " SELECT ?1, 0, op_version, operation, op_params," // 0 is num in main.history, we just need uniform params binding
  "       enabled, blendop_params, blendop_version, multi_priority, multi_name, name"
  " FROM %s" // SQLite doesn't like the table name to be bound with variables...
  " WHERE ( (autoapply=1"
  "          AND ((?2 LIKE model AND ?3 LIKE maker) OR (?4 LIKE model AND ?5 LIKE maker))"
  "          AND ?6 LIKE lens AND ?7 BETWEEN iso_min AND iso_max"
  "          AND ?8 BETWEEN exposure_min AND exposure_max"
  "          AND ?9 BETWEEN aperture_min AND aperture_max"
  "          AND ?10 BETWEEN focal_length_min AND focal_length_max"
  "          AND (format = 0 OR (format & ?11 != 0 AND ~format & ?12 != 0)))"
  "        OR (name = ?13))"
  "   AND operation NOT IN"
  "        ('ioporder', 'metadata', 'modulegroups', 'export', 'tagging', 'collect', 'basecurve')"
  " ORDER BY writeprotect DESC, LENGTH(model), LENGTH(maker), LENGTH(lens)",
  (image->flags & DT_IMAGE_NO_LEGACY_PRESETS) ? "data.presets" : "main.legacy_presets");
  // clang-format on
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, image->exif_model, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, image->exif_maker, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, image->camera_alias, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, image->camera_maker, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 6, image->exif_lens, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 7, fmaxf(0.0f, fminf(FLT_MAX, image->exif_iso)));
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 8, fmaxf(0.0f, fminf(1000000, image->exif_exposure)));
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 9, fmaxf(0.0f, fminf(1000000, image->exif_aperture)));
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 10, fmaxf(0.0f, fminf(1000000, image->exif_focal_length)));
  // 0: dontcare, 1: ldr, 2: raw plus monochrome & color
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 11, iformat);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 12, excluded);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 13, workflow_preset, -1, SQLITE_TRANSIENT);

  int legacy_params = 0;
  while(sqlite3_step(stmt) == SQLITE_ROW)
    _process_history_db_entry(dev, stmt, imgid, &legacy_params, TRUE);

  sqlite3_finalize(stmt);
  g_free(query);

  // now we want to auto-apply the iop-order list if one corresponds and none are
  // still applied. Note that we can already have an iop-order list set when
  // copying an history or applying a style to a not yet developed image.

  if(!dt_ioppr_has_iop_order_list(imgid))
  {
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT op_params"
                                " FROM data.presets"
                                " WHERE autoapply=1"
                                "       AND ((?2 LIKE model AND ?3 LIKE maker) OR (?4 LIKE model AND ?5 LIKE maker))"
                                "       AND ?6 LIKE lens AND ?7 BETWEEN iso_min AND iso_max"
                                "       AND ?8 BETWEEN exposure_min AND exposure_max"
                                "       AND ?9 BETWEEN aperture_min AND aperture_max"
                                "       AND ?10 BETWEEN focal_length_min AND focal_length_max"
                                "       AND (format = 0 OR (format & ?11 != 0 AND ~format & ?12 != 0))"
                                "       AND operation = 'ioporder'"
                                " ORDER BY writeprotect DESC, LENGTH(model), LENGTH(maker), LENGTH(lens)",
                                -1, &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, image->exif_model, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, image->exif_maker, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, image->camera_alias, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, image->camera_maker, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 6, image->exif_lens, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 7, fmaxf(0.0f, fminf(FLT_MAX, image->exif_iso)));
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 8, fmaxf(0.0f, fminf(1000000, image->exif_exposure)));
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 9, fmaxf(0.0f, fminf(1000000, image->exif_aperture)));
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 10, fmaxf(0.0f, fminf(1000000, image->exif_focal_length)));
    // 0: dontcare, 1: ldr, 2: raw plus monochrome & color
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 11, iformat);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 12, excluded);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      const char *params = (char *)sqlite3_column_blob(stmt, 0);
      const int32_t params_len = sqlite3_column_bytes(stmt, 0);
      GList *iop_list = dt_ioppr_deserialize_iop_order_list(params, params_len);
      dt_ioppr_write_iop_order_list(iop_list, imgid);
      g_list_free_full(iop_list, free);
      dt_ioppr_set_default_iop_order(dev, imgid);
    }
    else
    {
      // we have no auto-apply order, so apply iop order, depending of the workflow
      GList *iop_list = dt_ioppr_get_iop_order_list_version(DT_IOP_ORDER_V30);
      dt_ioppr_write_iop_order_list(iop_list, imgid);
      g_list_free_full(iop_list, free);
      dt_ioppr_set_default_iop_order(dev, imgid);
    }
    sqlite3_finalize(stmt);
  }

  // Notify our private image copy that auto-presets got applied
  dev->image_storage.flags |= DT_IMAGE_AUTO_PRESETS_APPLIED | DT_IMAGE_NO_LEGACY_PRESETS;

  return TRUE;
}

// helper function for debug strings
char * _print_validity(gboolean state)
{
  if(state)
    return "ok";
  else
    return "WRONG";
}


static void _insert_default_modules(dt_develop_t *dev, dt_iop_module_t *module, const int32_t imgid, gboolean is_inited)
{
  // Module already in history: don't prepend extra entries
  if(dt_history_check_module_exists(imgid, module->op, FALSE))
    return;

  // Module has no user params: no history: don't prepend either
  if((module->flags() & IOP_FLAGS_NO_HISTORY_STACK)
     && (module->default_enabled || (module->force_enable && module->force_enable(module, FALSE))))
  {
    module->enabled = TRUE;
    return;
  }

  dt_image_t *image = &dev->image_storage;
  const gboolean has_matrix = dt_image_is_matrix_correction_supported(image);
  const gboolean is_raw = dt_image_is_raw(image);

  // Prior to Darktable 3.0, modules enabled by default which still had
  // default params (no user change) were not inserted into history/DB.
  // We need to insert them here with default params.
  // But defaults have changed since then for some modules, so we need to ensure
  // we insert them with OLD defaults.
  if(module->default_enabled || (module->force_enable && module->force_enable(module, FALSE)))
  {
    module->enabled = TRUE;
    if(!strcmp(module->op, "temperature")
       && (image->change_timestamp == -1) // change_timestamp is not defined for old pics
       && is_raw && is_inited && has_matrix)
    {
      dt_print(DT_DEBUG_HISTORY, "[history] Image history seems older than Darktable 3.0, we will insert white balance.\n");

      // Temp revert to legacy defaults
      dt_conf_set_string("plugins/darkroom/chromatic-adaptation", "legacy");
      dt_iop_reload_defaults(module);

      dt_dev_add_history_item_ext(dev, module, TRUE, TRUE, TRUE, FALSE);

      // Go back to current defaults
      dt_conf_set_string("plugins/darkroom/chromatic-adaptation", "modern");
      dt_iop_reload_defaults(module);
    }
    else
    {
      dt_dev_add_history_item_ext(dev, module, TRUE, TRUE, TRUE, FALSE);
    }
  }
  else if(module->workflow_enabled && !is_inited)
  {
    module->enabled = TRUE;
    dt_dev_add_history_item_ext(dev, module, TRUE, TRUE, TRUE, FALSE);
  }
}

// Returns TRUE if this is a freshly-inited history on which we just applied auto presets and defaults,
// FALSE if we had an earlier history
static gboolean _init_default_history(dt_develop_t *dev, const int32_t imgid)
{
  const gboolean is_inited = (dev->image_storage.flags & DT_IMAGE_AUTO_PRESETS_APPLIED);

  // Make sure this is set
  dt_conf_set_string("plugins/darkroom/chromatic-adaptation", "modern");

  // make sure all modules default params are loaded to init history
  for(GList *iop = g_list_first(dev->iop); iop; iop = g_list_next(iop))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(iop->data);
    dt_iop_reload_defaults(module);
    _insert_default_modules(dev, module, imgid, is_inited);
  }

  // On virgin history image, apply auto stuff (ours and user's)
  if(!is_inited) _dev_auto_apply_presets(dev, imgid);
  dt_print(DT_DEBUG_HISTORY, "[history] temporary history initialised with default params and presets\n");

  return !is_inited;
}

// populate hist->module
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

  if(hist->module) hist->module->enabled = hist->enabled;
}


static void _sync_blendop_params(dt_dev_history_item_t *hist, const void *blendop_params, const int bl_length,
                          const int blendop_version, int *legacy_params)
{
  const gboolean is_valid_blendop_version = (blendop_version == dt_develop_blend_version());
  const gboolean is_valid_blendop_size = (bl_length == sizeof(dt_develop_blend_params_t));

  hist->blend_params = malloc(sizeof(dt_develop_blend_params_t));

  if(blendop_params && is_valid_blendop_version && is_valid_blendop_size)
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

static int _sync_params(dt_dev_history_item_t *hist, const void *module_params, const int param_length,
                          const int modversion, int *legacy_params, const char *preset_name)
{
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

      g_free(preset);
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
      hist->enabled = 1;
    }
  }

  return 0;
}

// WARNING: this does not set hist->forms
static void _process_history_db_entry(dt_develop_t *dev, sqlite3_stmt *stmt, const int32_t imgid, int *legacy_params, gboolean presets)
{
  // Unpack the DB blobs
  const int id = sqlite3_column_int(stmt, 0);
  const int num = sqlite3_column_int(stmt, 1);
  const int modversion = sqlite3_column_int(stmt, 2);
  const char *module_name = (const char *)sqlite3_column_text(stmt, 3);
  const void *module_params = sqlite3_column_blob(stmt, 4);
  const int enabled = sqlite3_column_int(stmt, 5);
  const void *blendop_params = sqlite3_column_blob(stmt, 6);
  const int blendop_version = sqlite3_column_int(stmt, 7);
  const int multi_priority = sqlite3_column_int(stmt, 8);
  const char *multi_name = (const char *)sqlite3_column_text(stmt, 9);
  const char *preset_name = (presets) ? (const char *)sqlite3_column_text(stmt, 10) : "";

  const int param_length = sqlite3_column_bytes(stmt, 4);
  const int bl_length = sqlite3_column_bytes(stmt, 6);

  // Sanity checks
  const gboolean is_valid_id = (id == imgid);
  const gboolean has_module_name = (module_name != NULL);

  if(!(has_module_name && is_valid_id))
  {
    fprintf(stderr, "[dev_read_history] database history for image `%s' seems to be corrupted!\n",
            dev->image_storage.filename);
    return;
  }

  const int iop_order = dt_ioppr_get_iop_order(dev->iop_order_list, module_name, multi_priority);

  // Init a bare minimal history entry
  dt_dev_history_item_t *hist = (dt_dev_history_item_t *)calloc(1, sizeof(dt_dev_history_item_t));
  hist->module = NULL;
  hist->num = num;
  hist->iop_order = iop_order;
  hist->multi_priority = multi_priority;
  hist->enabled = enabled;
  g_strlcpy(hist->op_name, module_name, sizeof(hist->op_name));
  g_strlcpy(hist->multi_name, multi_name, sizeof(hist->multi_name));

  // Find a .so file that matches our history entry, aka a module to run the params stored in DB
  _find_so_for_history_entry(dev, hist);

  if(!hist->module)
  {
    // History will be lost forever for this module
    fprintf(
        stderr,
        "[dev_read_history] the module `%s' requested by image `%s' is not installed on this computer!\n",
        module_name, dev->image_storage.filename);
    free(hist);
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
    free(hist);
    return;
  }

  // Copy module params if valid version, else try to convert legacy params
  if(_sync_params(hist, module_params, param_length, modversion, legacy_params, preset_name))
  {
    free(hist);
    return;
  }

  // So far, on error we haven't allocated any buffer, so we just freed the hist structure

  // Last chance & desperate attempt at enabling/disabling critical modules
  // when history is garbled - This might prevent segfaults on invalid data
  if(hist->module->force_enable)
    hist->enabled = hist->module->force_enable(hist->module, hist->enabled);

  // make sure that always-on modules are always on. duh.
  if(hist->module->default_enabled == 1 && hist->module->hide_enable_button == 1)
    hist->enabled = TRUE;

  // Copy blending params if valid, else try to convert legacy params
  _sync_blendop_params(hist, blendop_params, bl_length, blendop_version, legacy_params);

  dev->history = g_list_append(dev->history, hist);

  // Update the history end cursor. Note that this is useful only if it's a fresh, empty history,
  // otherwise the value will get overriden by the DB value
  // when we are done adding entries from defaults & auto-presets.
  dt_dev_set_history_end(dev, g_list_length(dev->history));

  dt_print(DT_DEBUG_HISTORY, "[history entry] read %s at pipe position %i (enabled %i) from %s %s\n", hist->op_name,
    hist->iop_order, hist->enabled, (presets) ? "preset" : "database", (presets) ? preset_name : "");
}


void dt_dev_read_history_ext(dt_develop_t *dev, const int32_t imgid, gboolean no_image)
{
  if(imgid == UNKNOWN_IMAGE) return;

  if(!dev->iop)
    dev->iop = dt_dev_load_modules(dev);

  // Ensure we have our own fresh copy of the image structure.
  // Need to do it here, some modules rely on it to update their default params.
  // This is redundant with `_dt_dev_load_raw()` called from `dt_dev_load_image()`,
  // but we don't always manipulate an history when/after loading an image.
  if(dev->image_storage.id != imgid)
  {
    dt_image_t *cache_img = dt_image_cache_get(darktable.image_cache, imgid, 'r');
    if(!cache_img) return;
    dev->image_storage = *cache_img;
    dt_image_cache_read_release(darktable.image_cache, cache_img);
  }

  gboolean legacy_params = FALSE;

  dt_ioppr_set_default_iop_order(dev, imgid);

  // Lock database
  //dt_pthread_rwlock_rdlock(&darktable.database_threadsafe);

  gboolean first_run = _init_default_history(dev, imgid);

  // Protect history DB reads with a cache read lock.
  // Release it before applying history to modules to avoid deadlocks.
  dt_image_t *read_lock_img = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  if(!read_lock_img) return;

  sqlite3_stmt *stmt;

  // Load current image history from DB
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT imgid, num, module, operation,"
                              "       op_params, enabled, blendop_params,"
                              "       blendop_version, multi_priority, multi_name"
                              " FROM main.history"
                              " WHERE imgid = ?1"
                              " ORDER BY num",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  // Strip rows from DB lookup. One row == One module in history
  while(sqlite3_step(stmt) == SQLITE_ROW)
    _process_history_db_entry(dev, stmt, imgid, &legacy_params, FALSE);

  sqlite3_finalize(stmt);

  // find the new history end
  // Note: dt_dev_set_history_end sanitizes the value with the actual history size.
  // It needs to run after dev->history is fully populated
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT history_end FROM main.images WHERE id = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  if(sqlite3_step(stmt) == SQLITE_ROW) // seriously, this should never fail
    if(sqlite3_column_type(stmt, 0) != SQLITE_NULL
       && sqlite3_column_int(stmt, 0) > 0)
      dt_dev_set_history_end(dev, sqlite3_column_int(stmt, 0));

  sqlite3_finalize(stmt);

  // Unlock database
  //dt_pthread_rwlock_unlock(&darktable.database_threadsafe);

  // Sanitize and flatten module order
  dt_ioppr_resync_modules_order(dev);
  dt_ioppr_resync_iop_list(dev);
  dt_ioppr_check_iop_order(dev, imgid, "dt_dev_read_history_no_image end");

  // Update masks history
  // Note: until there, we had only blendops. No masks
  // writes hist->forms for each history entry, from DB
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
      fprintf(stderr, "[dt_dev_read_history_ext] we have no module for history item %s. This is not normal.\n", hist->op_name);
      continue;
    }

    dt_iop_module_t *module = hist->module;
    _history_to_module(hist, module);
    hist->hash = hist->module->hash;

    dt_print(DT_DEBUG_HISTORY, "[history] successfully loaded module %s history (enabled: %i)\n", hist->module->op, hist->enabled);
  }

  dt_dev_masks_list_change(dev);
  dt_dev_masks_update_hash(dev);

  // Init global history hash to track changes during runtime
  dev->history_hash = dt_dev_history_get_hash(dev);

  // Write it straight away if we just inited the history
  // NOTE: if the embedded_jpg mode is set to "never" (= 0),
  // browsing the lighttable will render the thumbnails from scratch
  // from the raw input, which will init an history to init a pipeline.
  // In that case, we don't want to write an history that would make the images
  // look like they have been edited.
  // So we auto-write here only if we are in darkroom.
  if(first_run && dev == darktable.develop)
  {
    // Resync our private copy of image image with DB,
    // mostly for DT_IMAGE_AUTO_PRESETS_APPLIED flag
    dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'w');
    *image = dev->image_storage;
    dt_dev_append_changed_tag(imgid);
    dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_SAFE);
    
    dt_dev_write_history_ext(dev, imgid);
  }
  //else if(legacy_params)
  //  TODO: ask user for confirmation before saving updated history
  //  because that will made it incompatible with earlier app versions

  dt_print(DT_DEBUG_HISTORY, "[history] dt_dev_read_history_ext completed\n");
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

gboolean _module_leaves_no_history(dt_iop_module_t *module)
{
  return (module->flags() & IOP_FLAGS_NO_HISTORY_STACK);
}

void dt_dev_history_compress(dt_develop_t *dev)
{
  dt_pthread_rwlock_wrlock(&dev->history_mutex);

  // Cleanup old history
  dt_dev_history_free_history(dev);

  // Rebuild an history from current pipeline.
  // First: modules enabled by default or forced enabled for technical reasons
  for(GList *item = g_list_first(dev->iop); item; item = g_list_next(item))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(item->data);
    if(module->enabled
       && (module->default_enabled || (module->force_enable && module->force_enable(module, module->enabled)))
       && !_module_leaves_no_history(module))
      dt_dev_add_history_item_ext(dev, module, FALSE, TRUE, TRUE, TRUE);
  }

  // Second: modules enabled by user
  // 2.1 : start with modules that still have default params,
  for(GList *item = g_list_first(dev->iop); item; item = g_list_next(item))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(item->data);
    if(module->enabled
      && !(module->default_enabled || (module->force_enable && module->force_enable(module, module->enabled)))
      && module->has_defaults(module)
      && !_module_leaves_no_history(module))
      dt_dev_add_history_item_ext(dev, module, FALSE, TRUE, TRUE, TRUE);
  }

  // 2.2 : then modules that are set to non-default
  for(GList *item = g_list_first(dev->iop); item; item = g_list_next(item))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(item->data);
    if(module->enabled
      && !(module->default_enabled || (module->force_enable && module->force_enable(module, module->enabled)))
      && !module->has_defaults(module)
      && !_module_leaves_no_history(module))
      dt_dev_add_history_item_ext(dev, module, FALSE, TRUE, TRUE, TRUE);
  }

  // Third: disabled modules that have an history. Maybe users want to re-enable them later,
  // or it's modules enabled by default that were manually disabled.
  // Put them the end of the history, so user can truncate it after the last enabled item
  // to get rid of disabled history if needed.
  for(GList *item = g_list_first(dev->iop); item; item = g_list_next(item))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(item->data);
    if(!module->enabled
       && (module->default_enabled || !module->has_defaults(module))
       && !_module_leaves_no_history(module))
      dt_dev_add_history_item_ext(dev, module, FALSE, TRUE, TRUE, TRUE);
  }

  // Commit to DB
  dt_dev_write_history_ext(dev, dev->image_storage.id);

  dt_pthread_rwlock_unlock(&dev->history_mutex);

  // Reload to sanitize mandatory/incompatible modules
  dt_dev_reload_history_items(dev);

  // Write again after sanitizatio
  dt_dev_write_history(dev);
}
