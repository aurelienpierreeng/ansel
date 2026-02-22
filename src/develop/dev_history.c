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
                                      const int param_length, const int enabled, const void *blendop_params,
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

static void _dev_history_db_row_cb(void *user_data, const int32_t id, const int num, const int modversion,
                                   const char *operation, const void *module_params, const int param_length,
                                   const int enabled, const void *blendop_params, const int bl_length,
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

int dt_history_merge_module_list_into_image(dt_develop_t *dev_dest, dt_develop_t *dev_src, const int32_t dest_imgid,
                                            const GList *mod_list)
{
  if(!dev_dest || dest_imgid <= 0) return 1;
  if(!mod_list) return 0;

  // update iop-order list to have entries for the new modules
  dt_ioppr_update_for_modules(dev_dest, (GList *)mod_list, FALSE);

  GList *modules_used = NULL;
  for(const GList *l = g_list_first((GList *)mod_list); l; l = g_list_next(l))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)l->data;
    dt_history_merge_module_into_history(dev_dest, dev_src, mod, &modules_used);
  }

  // update iop-order list to have entries for the new modules
  dt_ioppr_update_for_modules(dev_dest, (GList *)mod_list, FALSE);

  dt_dev_write_history_ext(dev_dest, dest_imgid);

  g_list_free(modules_used);
  return 0;
}

static int _history_copy_and_paste_on_image_merge(int32_t imgid, int32_t dest_imgid, GList *ops, const gboolean copy_full)
{
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
      if((dt_dev_history_get_first_item_by_module(dev_src->history, mod_src) != NULL) // module is in history of source image
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

  dt_ioppr_check_iop_order(dev_dest, dest_imgid, "_history_copy_and_paste_on_image_merge 2 pre");
  const int ret_val = dt_history_merge_module_list_into_image(dev_dest, dev_src, dest_imgid, mod_list);
  dt_ioppr_check_iop_order(dev_dest, dest_imgid, "_history_copy_and_paste_on_image_merge 2 post");

  dt_dev_cleanup(dev_src);
  dt_dev_cleanup(dev_dest);

  g_list_free(mod_list);

  return ret_val;
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

static void _history_invalidate_cb(gpointer user_data, dt_undo_type_t type, dt_undo_data_t item)
{
  dt_iop_module_t *module = (dt_iop_module_t *)user_data;
  dt_undo_history_t *hist = (dt_undo_history_t *)item;
  dt_dev_invalidate_history_module(hist->before_snapshot, module);
  dt_dev_invalidate_history_module(hist->after_snapshot, module);
}

void dt_dev_history_undo_invalidate_module(dt_iop_module_t *module)
{
  if(!module) return;
  dt_undo_iterate_internal(darktable.undo, DT_UNDO_HISTORY, module, &_history_invalidate_cb);
}

static void _history_undo_data_free(gpointer data)
{
  dt_undo_history_t *hist = (dt_undo_history_t *)data;
  if(!hist) return;
  g_list_free_full(hist->before_snapshot, dt_dev_free_history_item);
  g_list_free_full(hist->after_snapshot, dt_dev_free_history_item);
  g_list_free_full(hist->before_iop_order_list, free);
  g_list_free_full(hist->after_iop_order_list, free);
  free(hist);
}

static void _pop_undo(gpointer user_data, dt_undo_type_t type, dt_undo_data_t data, dt_undo_action_t action, GList **imgs)
{
  if(type != DT_UNDO_HISTORY) return;

  dt_develop_t *dev = (dt_develop_t *)user_data;
  dt_undo_history_t *hist = (dt_undo_history_t *)data;
  if(!dev || !hist) return;

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
  g_list_free_full(dev->iop_order_list, free);
  dev->iop_order_list = iop_order_temp;
  dt_pthread_rwlock_unlock(&dev->history_mutex);

  dt_dev_write_history(dev);
  dt_dev_reload_history_items(dev, dev->image_storage.id);

  int pipe_remove = dt_dev_history_refresh_nodes(dev, dev->iop, dev->history);
  dt_dev_history_gui_update(dev);
  dt_dev_history_pixelpipe_update(dev, pipe_remove);
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

  (void)imgs;
}

void dt_dev_history_undo_start_record(dt_develop_t *dev)
{
  if(!dev) return;

  if(dev->undo_history_depth == 0)
  {
    g_list_free_full(dev->undo_history_before_snapshot, dt_dev_free_history_item);
    dev->undo_history_before_snapshot = NULL;
    g_list_free_full(dev->undo_history_before_iop_order_list, free);
    dev->undo_history_before_iop_order_list = NULL;
    dev->undo_history_before_end = 0;

    dt_pthread_rwlock_rdlock(&dev->history_mutex);
    dev->undo_history_before_snapshot = dt_history_duplicate(dev->history);
    dev->undo_history_before_end = dt_dev_get_history_end_ext(dev);
    dev->undo_history_before_iop_order_list = dt_ioppr_iop_order_copy_deep(dev->iop_order_list);
    dt_pthread_rwlock_unlock(&dev->history_mutex);
  }

  dev->undo_history_depth++;
}

void dt_dev_history_undo_end_record(dt_develop_t *dev)
{
  if(!dev || dev->undo_history_depth <= 0) return;

  dev->undo_history_depth--;
  if(dev->undo_history_depth != 0) return;

  if(!dev->undo_history_before_snapshot) return;

  dt_undo_history_t *hist = malloc(sizeof(dt_undo_history_t));
  hist->before_snapshot = dev->undo_history_before_snapshot;
  hist->before_end = dev->undo_history_before_end;
  hist->before_iop_order_list = dev->undo_history_before_iop_order_list;
  dev->undo_history_before_snapshot = NULL;
  dev->undo_history_before_end = 0;
  dev->undo_history_before_iop_order_list = NULL;

  dt_pthread_rwlock_rdlock(&dev->history_mutex);
  hist->after_snapshot = dt_history_duplicate(dev->history);
  hist->after_end = dt_dev_get_history_end_ext(dev);
  hist->after_iop_order_list = dt_ioppr_iop_order_copy_deep(dev->iop_order_list);
  dt_pthread_rwlock_unlock(&dev->history_mutex);

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
  GList *history = g_list_nth(dev->history, dt_dev_get_history_end_ext(dev));
  while(history)
  {
    // We need to use a while because we are going to dynamically remove entries at the end
    // of the list, so we can't know the number of iterations
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);
    dt_print(DT_DEBUG_HISTORY, "[dt_dev_add_history_item_ext] history item %s at %i is past history limit (%i)\n", hist->module->op, g_list_index(dev->history, hist), dt_dev_get_history_end_ext(dev) - 1);

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
  dt_dev_set_history_end_ext(dev, g_list_length(dev->history));

  return add_new_pipe_node;
}

uint64_t dt_dev_history_get_hash(dt_develop_t *dev)
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
  dt_atomic_set_int(&dev->pipe->shutdown, TRUE);
  dt_atomic_set_int(&dev->preview_pipe->shutdown, TRUE);

  dt_dev_undo_start_record(dev);
  dt_pthread_rwlock_wrlock(&dev->history_mutex);
  dt_dev_add_history_item_ext(dev, module, enable, FALSE, FALSE, FALSE);
  dt_pthread_rwlock_unlock(&dev->history_mutex);
  dt_dev_undo_end_record(dev);

  // Run the delayed post-commit actions if implemented
  if(module && module->post_history_commit) module->post_history_commit(module);

  // Figure out if the current history item includes masks/forms
  GList *last_history = g_list_nth(dev->history, dt_dev_get_history_end_ext(dev) - 1);
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
  dt_dev_write_history(dev);
  dt_dev_history_notify_change(dev, dev->image_storage.id);
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
  if(!dev->history) return;
  g_list_free_full(g_steal_pointer(&dev->history), dt_dev_free_history_item);
  dev->history = NULL;
}

void dt_dev_reload_history_items(dt_develop_t *dev, const int32_t imgid)
{
  // Recreate the whole history from scratch.
  // Backend only: GUI updates and pixelpipe rebuilds need to be triggered by callers.
  if(darktable.gui && dev->gui_attached) ++darktable.gui->reset;
  dt_pthread_rwlock_wrlock(&dev->history_mutex);
  dt_dev_read_history_ext(dev, imgid, !dev->gui_attached);
  dt_dev_pop_history_items_ext(dev);
  dt_pthread_rwlock_unlock(&dev->history_mutex);
  if(darktable.gui && dev->gui_attached) --darktable.gui->reset;
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

  const int history_end = dt_dev_get_history_end_ext(dev);

  // Modules after history_end need to be reset to default in case they were previously enabled
  // They will get a chance to be re-enabled next
  for(GList *history = g_list_nth(dev->history, history_end); history; history = g_list_next(history))
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);
    dt_iop_module_t *module = hist->module;
    module->enabled = module->default_enabled;
    dt_iop_compute_module_hash(module, hist->forms);
    hist->hash = module->hash;
  }

  // go through history up to history_end and set modules params
  GList *history = g_list_first(dev->history);
  GList *forms = NULL;
  for(int i = 0; i < history_end && history; i++)
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);
    dt_iop_module_t *module = hist->module;
    _history_to_module(hist, module);
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
  // Ensure `dev->image_storage` is up-to-date before modules reload their defaults.
  // This avoids using incomplete RAW metadata (WB coeffs, matrices) on newly-inited images.
  dt_dev_ensure_image_storage(dev, dev->image_storage.id);

  dt_pthread_rwlock_wrlock(&dev->history_mutex);
  dt_dev_pop_history_items_ext(dev);
  dt_pthread_rwlock_unlock(&dev->history_mutex);
}

void dt_dev_history_gui_update(dt_develop_t *dev)
{
  if(!dev->gui_attached) return;

  // Ensure the set of module instances shown in the right panel matches the current history:
  // hide/remove instances that are no longer referenced by any history item.
  // Note: this may also reorder modules in the GUI if needed.
  dt_pthread_rwlock_wrlock(&dev->history_mutex);
  (void)dt_dev_history_refresh_nodes(dev, dev->iop, dev->history);
  dt_pthread_rwlock_unlock(&dev->history_mutex);

  ++darktable.gui->reset;

  for(GList *module = g_list_first(dev->iop); module; module = g_list_next(module))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)(module->data);
    dt_iop_gui_update(mod);
  }

  dt_dev_reorder_gui_module_list(dev);
  dt_dev_modules_update_multishow(dev);
  dt_dev_modulegroups_update_visibility(dev);
  dt_dev_masks_list_change(dev);
  dt_dev_modulegroups_set(darktable.develop, dt_dev_modulegroups_get(darktable.develop));

  --darktable.gui->reset;
}

void dt_dev_history_pixelpipe_update(dt_develop_t *dev, gboolean rebuild)
{
  if(!dev->gui_attached) return;

  if(rebuild)
    dt_dev_pixelpipe_rebuild_all(dev);
  else
    dt_dev_pixelpipe_resync_history_all(dev);

  dt_dev_process_all(dev);
}

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
  if(!dev || imgid <= 0) return;

  if(darktable.gui && dev->gui_attached)
  {
    const guint states = dt_dev_mask_history_overload(dev->history, 250);
    if(states > 250)
      dt_toast_log(_("Image #%i history is storing %d mask states. n"
                     "Consider compressing history and removing unused masks to keep reads/writes manageable."),
                     imgid, states);
  }

  // Don't refresh the thumbnail if we are in darkroom
  // Spawning another export thread will likely slow-down the current one.
  if(darktable.gui && dev != darktable.develop)
    dt_thumbtable_refresh_thumbnail(darktable.gui->ui->thumbtable_lighttable, imgid, TRUE);

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_IMAGE_INFO_CHANGED,
                               g_list_append(NULL, GINT_TO_POINTER(imgid)));
}


// helper used to synch a single history item with db
int dt_dev_write_history_item(const int32_t imgid, dt_dev_history_item_t *h, int32_t num)
{
  dt_print(DT_DEBUG_HISTORY, "[dt_dev_write_history_item] writing history for module %s (%s) at pipe position %i for image %i...\n", h->op_name, h->multi_name, h->iop_order, imgid);

  dt_history_db_write_history_item(imgid, num, h->module->op, h->params, h->module->params_size, h->module->version(),
                                   h->enabled, h->blend_params, sizeof(dt_develop_blend_params_t),
                                   dt_develop_blend_version(), h->multi_priority, h->multi_name);

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
  if(!cache_img) return;

  dt_print(DT_DEBUG_HISTORY, "[dt_dev_write_history_ext] writing history for image %i...\n", imgid);

  dev->history_hash = dt_dev_history_get_hash(dev);

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

  cache_img->history_hash = dev->history_hash;

  dt_image_cache_write_release(darktable.image_cache, cache_img, DT_IMAGE_CACHE_SAFE);
  dt_mipmap_cache_remove(darktable.mipmap_cache, imgid, TRUE);
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
      g_list_free_full(iop_list, free);
      dt_ioppr_set_default_iop_order(dev, imgid);
      g_free(params);
    }
    else
    {
      // we have no auto-apply order, so apply iop order, depending of the workflow
      GList *iop_list = dt_ioppr_get_iop_order_list_version(DT_IOP_ORDER_V30);
      dt_ioppr_write_iop_order_list(iop_list, imgid);
      g_list_free_full(iop_list, free);
      dt_ioppr_set_default_iop_order(dev, imgid);
    }
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
static void _process_history_db_entry(dt_develop_t *dev, const int32_t imgid, const int id, const int num,
                                      const int modversion, const char *operation, const void *module_params,
                                      const int param_length, const int enabled, const void *blendop_params,
                                      const int bl_length, const int blendop_version, const int multi_priority,
                                      const char *multi_name, const char *preset_name, int *legacy_params,
                                      const gboolean presets)
{
  // Sanity checks
  const gboolean is_valid_id = (id == imgid);
  const gboolean has_operation = (operation != NULL);

  if(!(has_operation && is_valid_id))
  {
    fprintf(stderr, "[dev_read_history] database history for image `%s' seems to be corrupted!\n",
            dev->image_storage.filename);
    return;
  }

  const int iop_order = dt_ioppr_get_iop_order(dev->iop_order_list, operation, multi_priority);

  // Init a bare minimal history entry
  dt_dev_history_item_t *hist = (dt_dev_history_item_t *)calloc(1, sizeof(dt_dev_history_item_t));
  hist->module = NULL;
  hist->num = num;
  hist->iop_order = iop_order;
  hist->multi_priority = multi_priority;
  hist->enabled = enabled;
  g_strlcpy(hist->op_name, operation, sizeof(hist->op_name));
  g_strlcpy(hist->multi_name, multi_name ? multi_name : "", sizeof(hist->multi_name));

  // Find a .so file that matches our history entry, aka a module to run the params stored in DB
  _find_so_for_history_entry(dev, hist);

  if(!hist->module)
  {
    // History will be lost forever for this module
    fprintf(
        stderr,
        "[dev_read_history] the module `%s' requested by image `%s' is not installed on this computer!\n",
        operation, dev->image_storage.filename);
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
  dt_dev_set_history_end_ext(dev, g_list_length(dev->history));

  dt_print(DT_DEBUG_HISTORY, "[history entry] read %s at pipe position %i (enabled %i) from %s %s\n", hist->op_name,
    hist->iop_order, hist->enabled, (presets) ? "preset" : "database", (presets) ? preset_name : "");
}


gboolean dt_dev_read_history_ext(dt_develop_t *dev, const int32_t imgid, gboolean no_image)
{
  if(imgid == UNKNOWN_IMAGE) return FALSE;

  if(!dev->iop)
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

  gboolean first_run = _init_default_history(dev, imgid);

  // Protect history DB reads with a cache read lock.
  // Release it before applying history to modules to avoid deadlocks.
  dt_image_t *read_lock_img = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  if(!read_lock_img) return FALSE;

  // Find the new history end from DB now, if defined.
  // Note: dt_dev_set_history_end_ext sanitizes the value with the actual history size.
  // It needs to run after dev->history is fully populated.
  const int32_t history_end = dt_history_get_end(imgid);

  dt_dev_history_db_ctx_t ctx = { .dev = dev, .imgid = imgid, .legacy_params = &legacy_params, .presets = FALSE };
  dt_history_db_foreach_history_row(imgid, _dev_history_db_row_cb, &ctx);

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

  // Unless it's a new editing and history end is the length of the history stack,
  // we need to grab it from DB because it's user-defined.
  if(history_end > 0) dt_dev_set_history_end_ext(dev, history_end);

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

gboolean _module_leaves_no_history(dt_iop_module_t *module)
{
  return (module->flags() & IOP_FLAGS_NO_HISTORY_STACK);
}

void dt_dev_history_compress(dt_develop_t *dev)
{
  const int32_t imgid = dev->image_storage.id;
  if(darktable.gui && dev->gui_attached) ++darktable.gui->reset;
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
  // TODO: write a fast path sanitizing without intermediate DB write
  dt_dev_write_history_ext(dev, imgid);

  // Reload to sanitize mandatory/incompatible modules.
  dt_dev_read_history_ext(dev, imgid, !dev->gui_attached);
  dt_dev_set_history_end_ext(dev, g_list_length(dev->history));
  dt_dev_pop_history_items_ext(dev);

  // Write again after sanitization.
  dt_dev_write_history_ext(dev, imgid);

  dt_pthread_rwlock_unlock(&dev->history_mutex);
  if(darktable.gui && dev->gui_attached) --darktable.gui->reset;
}


static int _check_deleted_instances(dt_develop_t *dev, GList **_iop_list, GList *history_list)
{
  GList *iop_list = *_iop_list;
  int deleted_module_found = 0;

  // we will check on dev->iop if there's a module that is not in history
  GList *modules = iop_list;
  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    if(mod == NULL) continue;

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

static void _reorder_gui_module_list(dt_develop_t *dev)
{
  int pos_module = 0;
  for(const GList *modules = g_list_last(dev->iop); modules; modules = g_list_previous(modules))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);

    GtkWidget *expander = module->expander;
    if(expander)
    {
      gtk_box_reorder_child(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER), expander,
                            pos_module++);
    }
  }
}

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

static void _reset_module_instance(GList *hist, dt_iop_module_t *module, int multi_priority)
{
  for(; hist; hist = g_list_next(hist))
  {
    dt_dev_history_item_t *hit = (dt_dev_history_item_t *)hist->data;

    if(!hit->module && strcmp(hit->op_name, module->op) == 0 && hit->multi_priority == multi_priority)
    {
      hit->module = module;
    }
  }
}

static void _undo_items_cb(gpointer user_data, dt_undo_type_t type, dt_undo_data_t data)
{
  struct _cb_data *udata = (struct _cb_data *)user_data;
  dt_undo_history_t *hdata = (dt_undo_history_t *)data;
  _reset_module_instance(hdata->after_snapshot, udata->module, udata->multi_priority);
}

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
    if(hitem->module == NULL)
    {
      changed = 1;

      const dt_iop_module_t *base_module = dt_iop_get_module_from_list(iop_list, hitem->op_name);
      if(base_module == NULL)
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
int dt_dev_history_refresh_nodes(dt_develop_t *dev, GList *iop, GList *history)
{
  // topology has changed?
  int pipe_remove = 0;

  // we have to check if multi_priority has changed since history was saved
  // we will adjust it here
  if(_rebuild_multi_priority(history))
  {
    pipe_remove = 1;
    iop = g_list_sort(iop, dt_sort_iop_by_order);
  }

  // check if this undo a delete module and re-create it
  if(_create_deleted_modules(&iop, history))
    pipe_remove = 1;

  // check if this is a redo of a delete module or an undo of an add module
  if(_check_deleted_instances(dev, &iop, history))
    pipe_remove = 1;

  // if topology has changed, we need to reorder modules in GUI
  if(pipe_remove) _reorder_gui_module_list(dev);

  return pipe_remove;
}
