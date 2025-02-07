#include "common/darktable.h"
#include "common/undo.h"
#include "common/history_snapshot.h"
#include "common/image_cache.h"
#include "develop/dev_history.h"
#include "develop/blend.h"
#include "develop/imageop.h"
#include "develop/masks.h"

#include "gui/presets.h"

#include <glib.h>


// returns the first history item with hist->module == module
static dt_dev_history_item_t *_search_history_by_module(dt_develop_t *dev, dt_iop_module_t *module)
{
  dt_dev_history_item_t *hist_mod = NULL;
  for(GList *history = dev->history; history; history = g_list_next(history))
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
  for(GList *history = dev->history; history; history = g_list_next(history))
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
      dt_masks_point_group_t *grpt = (dt_masks_point_group_t *)grpts->data;
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
    for( GList *modules_dest = dev_dest->iop; modules_dest; modules_dest = g_list_next(modules_dest))
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
          dt_masks_form_t *form = dt_masks_get_from_id_ext(dev_src->forms, forms_used_replace[i]);
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

    dt_dev_pop_history_items_ext(dev_dest, dt_dev_get_history_end(dev_dest));

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

  dev_src->iop = dt_iop_load_modules_ext(dev_src, TRUE);
  dev_dest->iop = dt_iop_load_modules_ext(dev_dest, TRUE);

  dt_dev_read_history_ext(dev_src, imgid, TRUE);

  // This prepends the default modules and converts just in case it's an empty history
  dt_dev_read_history_ext(dev_dest, dest_imgid, TRUE);

  dt_ioppr_check_iop_order(dev_src, imgid, "_history_copy_and_paste_on_image_merge ");
  dt_ioppr_check_iop_order(dev_dest, dest_imgid, "_history_copy_and_paste_on_image_merge ");

  dt_dev_pop_history_items_ext(dev_src, dt_dev_get_history_end(dev_src));
  dt_dev_pop_history_items_ext(dev_dest, dt_dev_get_history_end(dev_dest));

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
    for(GList *modules_src = dev_src->iop; modules_src; modules_src = g_list_next(modules_src))
    {
      dt_iop_module_t *mod_src = (dt_iop_module_t *)(modules_src->data);

      // copy from history only if
      if((_search_history_by_module(dev_src, mod_src) != NULL) // module is in history of source image
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

  for(GList *l = mod_list; l; l = g_list_next(l))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)l->data;
    dt_history_merge_module_into_history(dev_dest, dev_src, mod, &modules_used);
  }

  // update iop-order list to have entries for the new modules
  dt_ioppr_update_for_modules(dev_dest, mod_list, FALSE);

  dt_ioppr_check_iop_order(dev_dest, dest_imgid, "_history_copy_and_paste_on_image_merge 2");

  // write history and forms to db
  dt_dev_write_history_ext(dev_dest->history, dev_dest->iop_order_list, dest_imgid);
  dt_dev_write_history_end_ext(dt_dev_get_history_end(dev_dest), dest_imgid);

  dt_dev_cleanup(dev_src);
  dt_dev_cleanup(dev_dest);

  g_list_free(modules_used);

  return 0;
}

gboolean dt_history_copy_and_paste_on_image(const int32_t imgid, const int32_t dest_imgid, GList *ops,
                                       const gboolean copy_iop_order, const gboolean copy_full)
{
  if(imgid == dest_imgid) return 1;

  if(imgid == -1)
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

  /* attach changed tag reflecting actual change */
  dt_dev_append_changed_tag(dest_imgid);

  /* update xmp file */
  dt_control_save_xmp(dest_imgid);

  dt_mipmap_cache_remove(darktable.mipmap_cache, dest_imgid);

  /* update the aspect ratio. recompute only if really needed for performance reasons */
  dt_image_reset_aspect_ratio(dest_imgid, FALSE);

  // signal that the mipmap need to be updated
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_MIPMAP_UPDATED, dest_imgid);

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

    int32_t params_size = 0;
    if(old->module)
    {
      params_size = old->module->params_size;
    }
    else
    {
      dt_iop_module_t *base = dt_iop_get_module(old->op_name);
      if(base)
      {
        params_size = base->params_size;
      }
      else
      {
        // nothing else to do
        fprintf(stderr, "[_duplicate_history] can't find base module for %s\n", old->op_name);
      }
    }

    if(params_size > 0)
    {
      new->params = malloc(params_size);
      memcpy(new->params, old->params, params_size);
    }

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
    // module = NULL means a mask was changed from the mask manager
    // and that's where this function is called.
    // Find it now, even though it is not enabled and won't be.
    module = _find_mask_manager(dev);
    if(module)
    {
      // Mask manager is an IOP that never processes pixel
      // aka it's an ugly hack to record mask history
      force_new_item = FALSE;
      enable = FALSE;
    }
    else
    {
      return add_new_pipe_node;
    }
  }

  dt_iop_compute_blendop_hash(module);
  dt_iop_compute_module_hash(module);

  // look for leaks on top of history
  _remove_history_leaks(dev);

  // Check if the current module to append to history is actually the same as the last one in history,
  GList *last = g_list_last(dev->history);
  gboolean new_is_old = FALSE;
  if(last && last->data && !force_new_item)
  {
    dt_dev_history_item_t *last_item = (dt_dev_history_item_t *)last->data;
    dt_iop_module_t *last_module = last_item->module;
    // fprintf(stdout, "history has hash %lu, new module %s has %lu\n", last_item->hash, module->op, module->hash);
    new_is_old = dt_iop_check_modules_equal(module, last_module);
    // add_new_pipe_node = FALSE
  }
  else
  {
    const dt_dev_history_item_t *previous_item = dt_dev_get_history_item(dev, module);
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
    hist->forms = dt_masks_dup_forms_deep(dev->forms, NULL);
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

  if(enable) module->enabled = TRUE;
  hist->enabled = module->enabled;
  hist->hash = module->hash;

  // It is assumed that the last-added history entry is always on top
  // so its cursor index is always equal to the number of elements,
  // keeping in mind that history_end = 0 is the raw image, aka not a dev->history GList entry.
  // So dev->history_end = index of last history entry + 1 = length of history
  dt_dev_set_history_end(dev, g_list_length(dev->history));

  return add_new_pipe_node;
}

const dt_dev_history_item_t *dt_dev_get_history_item(dt_develop_t *dev, struct dt_iop_module_t *module)
{
  for(GList *l = g_list_last(dev->history); l; l = g_list_previous(l))
  {
    dt_dev_history_item_t *item = (dt_dev_history_item_t *)l->data;
    if(item->module == module)
      return item;
  }
  return NULL;
}


#define AUTO_SAVE_TIMEOUT 30000

static int _auto_save_edit(gpointer data)
{
  // TODO: put that in a parallel job to not freeze GUI mainthread
  // when writing XMP on remote storage ? But that will still lock history mutex...
  dt_develop_t *dev = (dt_develop_t *)data;
  dev->auto_save_timeout = 0;

  dt_times_t start;
  dt_get_times(&start);
  dt_toast_log(_("autosaving changes..."));

  dt_pthread_mutex_lock(&dev->history_mutex);
  dt_dev_write_history_ext(dev->history, dev->iop_order_list, dev->image_storage.id);
  dt_dev_write_history_end_ext(dt_dev_get_history_end(dev), dev->image_storage.id);
  dt_pthread_mutex_unlock(&dev->history_mutex);

  dt_control_save_xmp(dev->image_storage.id);

  dt_show_times(&start, "[_auto_save_edit] auto-saving history upon last change");

  dt_times_t end;
  dt_get_times(&end);
  dt_toast_log("autosaving completed in %.3f s", end.clock - start.clock);
  return G_SOURCE_REMOVE;
}


// The next 2 functions are always called from GUI controls setting parameters
// This is why they directly start a pipeline recompute.
// Otherwise, please keep GUI and pipeline fully separated.

void dt_dev_add_history_item_real(dt_develop_t *dev, dt_iop_module_t *module, gboolean enable)
{
  dt_atomic_set_int(&dev->pipe->shutdown, TRUE);
  dt_atomic_set_int(&dev->preview_pipe->shutdown, TRUE);

  dt_dev_undo_start_record(dev);

  // Run the delayed post-commit actions if implemented
  if(module && module->post_history_commit) module->post_history_commit(module);

  dt_pthread_mutex_lock(&dev->history_mutex);
  dt_dev_add_history_item_ext(dev, module, enable, FALSE, FALSE, FALSE);
  dt_pthread_mutex_unlock(&dev->history_mutex);

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
  if(module && !has_forms)
  {
    // If we have a module and it doesn't have masks, we only need to resync the top-most history item with pipeline
    dt_dev_invalidate_all(dev);
  }
  else
  {
    // We either don't have a module, meaning we have the mask manager, or
    // we have a module and it has masks. Both ways, masks can affect several modules anywhere.
    // We need a full resync of all pipeline with history.
    dt_dev_pixelpipe_resync_all(dev);
  }

  dt_dev_masks_list_update(dev);
  dt_dev_refresh_ui_images(dev);

  if(darktable.gui && dev->gui_attached)
  {
    if(module) dt_iop_gui_set_enable_button(module);

    // Auto-save N s after the last change.
    // If another change is made during that delay,
    // reset the timer and restart Ns
    if(dev->auto_save_timeout)
    {
      g_source_remove(dev->auto_save_timeout);
      dev->auto_save_timeout = 0;
    }
    dev->auto_save_timeout = g_timeout_add(AUTO_SAVE_TIMEOUT, _auto_save_edit, dev);
  }
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
  dt_pthread_mutex_lock(&dev->history_mutex);
  dt_dev_history_free_history(dev);
  dt_dev_read_history_ext(dev, dev->image_storage.id, FALSE);
  dt_pthread_mutex_unlock(&dev->history_mutex);

  // we have to add new module instances first
  for(GList *modules = g_list_first(dev->iop); modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    if(module->multi_priority > 0)
    {
      if(!dt_iop_is_hidden(module) && !module->expander)
      {
        dt_iop_gui_init(module);

        /* add module to right panel */
        dt_iop_gui_set_expander(module);
        dt_iop_gui_set_expanded(module, TRUE, FALSE);

        dt_iop_reload_defaults(module);
        dt_iop_gui_update_blending(module);
      }
    }
    else if(!dt_iop_is_hidden(module) && module->expander)
    {
      // we have to ensure that the name of the widget is correct
      dt_iop_gui_update_header(module);
    }
  }

  dt_dev_pop_history_items(dev, dt_dev_get_history_end(dev));

  dt_ioppr_resync_iop_list(dev);

  // set the module list order
  dt_dev_reorder_gui_module_list(dev);

  // we update show params for multi-instances for each other instances
  dt_dev_modules_update_multishow(dev);

  dt_dev_pixelpipe_rebuild(dev);
}

static inline void _dt_dev_modules_reload_defaults(dt_develop_t *dev)
{
  // The reason for this is modules mandatorily ON don't leave history.
  // We therefore need to init modules containers to the defaults.
  // This is of course shit because it relies on the fact that defaults will never change in the future.
  // As a result, changing defaults needs to be handled on a case-by-case basis, by first adding the affected
  // modules to history, then setting said history to the previous defaults.
  // Worse, some modules (temperature.c) grabbed their params at runtime (WB as shot in camera),
  // meaning the defaults were not even static values.
  for(GList *modules = g_list_first(dev->iop); modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    memcpy(module->params, module->default_params, module->params_size);
    dt_iop_commit_blend_params(module, module->default_blendop_params);
    module->enabled = module->default_enabled;

    if(module->multi_priority == 0)
      module->iop_order = dt_ioppr_get_iop_order(dev->iop_order_list, module->op, module->multi_priority);
    else
      module->iop_order = INT_MAX;
  }
}


void dt_dev_pop_history_items_ext(dt_develop_t *dev, int32_t cnt)
{
  dt_print(DT_DEBUG_HISTORY, "[dt_dev_pop_history_items_ext] loading history entries into modules...\n");

  dt_dev_set_history_end(dev, cnt);

  // reset gui params for all modules
  _dt_dev_modules_reload_defaults(dev);

  // go through history and set modules params
  GList *forms = NULL;
  GList *history = g_list_first(dev->history);
  for(int i = 0; i < cnt && history; i++)
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);
    memcpy(hist->module->params, hist->params, hist->module->params_size);
    dt_iop_commit_blend_params(hist->module, hist->blend_params);

    hist->module->iop_order = hist->iop_order;
    hist->module->enabled = hist->enabled;
    hist->module->multi_priority = hist->multi_priority;
    g_strlcpy(hist->module->multi_name, hist->multi_name, sizeof(hist->module->multi_name));

    // This needs to run after dt_iop_compute_blendop_hash()
    // which is called in dt_iop_commit_blend_params
    dt_iop_compute_module_hash(hist->module);
    hist->hash = hist->module->hash;

    if(hist->forms) forms = hist->forms;

    history = g_list_next(history);
  }

  dt_ioppr_resync_modules_order(dev);

  dt_ioppr_check_duplicate_iop_order(&dev->iop, dev->history);

  dt_ioppr_check_iop_order(dev, 0, "dt_dev_pop_history_items_ext end");

  dt_masks_replace_current_forms(dev, forms);
}

void dt_dev_pop_history_items(dt_develop_t *dev, int32_t cnt)
{
  ++darktable.gui->reset;

  dt_pthread_mutex_lock(&dev->history_mutex);
  dt_ioppr_check_iop_order(dev, 0, "dt_dev_pop_history_items");
  dt_dev_pop_history_items_ext(dev, cnt);
  dt_pthread_mutex_unlock(&dev->history_mutex);

  // update all gui modules
  for(GList *module = g_list_first(dev->iop); module; module = g_list_next(module))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)(module->data);
    dt_iop_gui_update(mod);
  }
  --darktable.gui->reset;

  dt_dev_masks_list_change(dev);
  dt_dev_pixelpipe_rebuild(dev);
  dt_dev_refresh_ui_images(dev);
}

static void _cleanup_history(const int imgid)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "DELETE FROM main.history WHERE imgid = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "DELETE FROM main.masks_history WHERE imgid = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
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

static void _warn_about_history_overuse(GList *dev_history)
{
  /* History stores one entry per module, everytime a parameter is changed.
  *  For modules using masks, we also store a full snapshot of masks states.
  *  All that is saved into database and XMP. When history entries x number of mask > 250,
  *  we get a really bad performance penalty.
  */
  guint states = dt_dev_mask_history_overload(dev_history, 250);

  if(states > 250)
    dt_toast_log(_("Your history is storing %d mask states. To ensure smooth operation, consider compressing "
                   "history and removing unused masks."),
                 states);
}


void dt_dev_write_history_end_ext(const int history_end, const int imgid)
{
  // update history end
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE main.images SET history_end = ?1 WHERE id = ?2", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, history_end);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

// helper used to synch a single history item with db
int dt_dev_write_history_item(const int imgid, dt_dev_history_item_t *h, int32_t num)
{
  dt_print(DT_DEBUG_HISTORY, "[dt_dev_write_history_item] writing history for module %s (%s) at pipe position %i for image %i...\n", h->op_name, h->multi_name, h->iop_order, imgid);

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT num FROM main.history WHERE imgid = ?1 AND num = ?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, num);
  if(sqlite3_step(stmt) != SQLITE_ROW)
  {
    sqlite3_finalize(stmt);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "INSERT INTO main.history (imgid, num) VALUES (?1, ?2)", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, num);
    sqlite3_step(stmt);
  }
  // printf("[dev write history item] writing %d - %s params %f %f\n", h->module->instance, h->module->op,
  // *(float *)h->params, *(((float *)h->params)+1));
  sqlite3_finalize(stmt);
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE main.history"
                              " SET operation = ?1, op_params = ?2, module = ?3, enabled = ?4, "
                              "     blendop_params = ?7, blendop_version = ?8, multi_priority = ?9, multi_name = ?10"
                              " WHERE imgid = ?5 AND num = ?6",
                              -1, &stmt, NULL);
  // clang-format on
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
  sqlite3_finalize(stmt);

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



void dt_dev_write_history_ext(GList *dev_history, GList *iop_order_list, const int imgid)
{
  _cleanup_history(imgid);
  _warn_about_history_overuse(dev_history);

  dt_print(DT_DEBUG_HISTORY, "[dt_dev_write_history_ext] writing history for image %i...\n", imgid);

  // write history entries
  int i = 0;
  for(GList *history = g_list_first(dev_history); history; history = g_list_next(history))
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);
    dt_dev_write_history_item(imgid, hist, i);
    i++;
  }

  // write the current iop-order-list for this image
  dt_ioppr_write_iop_order_list(iop_order_list, imgid);
  dt_history_hash_write_from_history(imgid, DT_HISTORY_HASH_CURRENT);
}

void dt_dev_write_history(dt_develop_t *dev)
{
  // FIXME: [CRITICAL] should lock the image history at the app level
  dt_pthread_mutex_lock(&dev->history_mutex);
  dt_dev_write_history_ext(dev->history, dev->iop_order_list, dev->image_storage.id);
  dt_dev_write_history_end_ext(dt_dev_get_history_end(dev), dev->image_storage.id);
  dt_pthread_mutex_unlock(&dev->history_mutex);
}

static int _dev_get_module_nb_records()
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT count (*) FROM  memory.history",
                              -1, &stmt, NULL);
  sqlite3_step(stmt);
  const int cnt = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return cnt;
}

void _dev_insert_module(dt_develop_t *dev, dt_iop_module_t *module, const int imgid)
{
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(
    dt_database_get(darktable.db),
    "INSERT INTO memory.history VALUES (?1, 0, ?2, ?3, ?4, 1, NULL, 0, 0, '')",
    -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, module->version());
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, module->op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 4, module->default_params, module->params_size, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  dt_print(DT_DEBUG_PARAMS, "[history] module %s inserted to history\n", module->op);
}

static gboolean _dev_auto_apply_presets(dt_develop_t *dev)
{
  // NOTE: the presets/default iops will be *prepended* into the history.

  const int imgid = dev->image_storage.id;

  if(imgid <= 0) return FALSE;

  gboolean run = FALSE;
  dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'w');
  if(!(image->flags & DT_IMAGE_AUTO_PRESETS_APPLIED)) run = TRUE;

  const gboolean is_raw = dt_image_is_raw(image);

  // Force-reload modern chromatic adaptation
  // Will be overriden below if we have no history for temperature
  dt_conf_set_string("plugins/darkroom/chromatic-adaptation", "modern");

  // flag was already set? only apply presets once in the lifetime of a history stack.
  // (the flag will be cleared when removing it).
  if(!run || image->id <= 0)
  {
    // Next section is to recover old edits where all modules with default parameters were not
    // recorded in the db nor in the .XMP.
    //
    // One crucial point is the white-balance which has automatic default based on the camera
    // and depends on the chroma-adaptation. In modern mode the default won't be the same used
    // in legacy mode and if the white-balance is not found on the history one will be added by
    // default using current defaults. But if we are in modern chromatic adaptation the default
    // will not be equivalent to the one used to develop this old edit.

    // So if the current mode is the modern chromatic-adaptation, do check the history.

    if(is_raw)
    {
      // loop over all modules and display a message for default-enabled modules that
      // are not found on the history.

      for(GList *modules = g_list_first(dev->iop); modules; modules = g_list_next(modules))
      {
        dt_iop_module_t *module = (dt_iop_module_t *)modules->data;

        if(module->default_enabled
           && !(module->flags() & IOP_FLAGS_NO_HISTORY_STACK)
           && !dt_history_check_module_exists(imgid, module->op, FALSE))
        {
          fprintf(stderr,
                  "[_dev_auto_apply_presets] missing mandatory module %s for image %d\n",
                  module->op, imgid);

          // If the module is white-balance and we are dealing with a raw file we need to add
          // one now with the default legacy parameters. And we want to do this only for
          // old edits.
          //
          // For new edits the temperature will be added back depending on the chromatic
          // adaptation the standard way.

          if(!strcmp(module->op, "temperature")
             && (image->change_timestamp == -1))
          {
            // it is important to recover temperature in this case (modern chroma and
            // not module present as we need to have the pre 3.0 default parameters used.

            dt_conf_set_string("plugins/darkroom/chromatic-adaptation", "legacy");
            dt_iop_reload_defaults(module);
            _dev_insert_module(dev, module, imgid);
            dt_conf_set_string("plugins/darkroom/chromatic-adaptation", "modern");
            dt_iop_reload_defaults(module);
          }
        }
      }
    }

    dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_RELAXED);
    return FALSE;
  }

  //  Add scene-referred workflow
  //  Note that we cannot use a preset for FilmicRGB as the default values are
  //  dynamically computed depending on the actual exposure compensation
  //  (see reload_default routine in filmicrgb.c)

  const gboolean has_matrix = dt_image_is_matrix_correction_supported(image);

  if(is_raw)
  {
    for(GList *modules = dev->iop; modules; modules = g_list_next(modules))
    {
      dt_iop_module_t *module = (dt_iop_module_t *)modules->data;

      if((   (strcmp(module->op, "filmicrgb") == 0)
          || (strcmp(module->op, "colorbalancergb") == 0)
          || (strcmp(module->op, "lens") == 0)
          || (has_matrix && strcmp(module->op, "channelmixerrgb") == 0) )
         && !dt_history_check_module_exists(imgid, module->op, FALSE)
         && !(module->flags() & IOP_FLAGS_NO_HISTORY_STACK))
      {
        _dev_insert_module(dev, module, imgid);
      }
    }
  }

  // FIXME : the following query seems duplicated from gui/presets.c/dt_gui_presets_autoapply_for_module()

  // select all presets from one of the following table and add them into memory.history. Note that
  // this is appended to possibly already present default modules.
  const char *preset_table[2] = { "data.presets", "main.legacy_presets" };
  const int legacy = (image->flags & DT_IMAGE_NO_LEGACY_PRESETS) ? 0 : 1;
  char query[1024];
  // clang-format off
  snprintf(query, sizeof(query),
           "INSERT INTO memory.history"
           " SELECT ?1, 0, op_version, operation, op_params,"
           "       enabled, blendop_params, blendop_version, multi_priority, multi_name"
           " FROM %s"
           " WHERE ( (autoapply=1"
           "          AND ((?2 LIKE model AND ?3 LIKE maker) OR (?4 LIKE model AND ?5 LIKE maker))"
           "          AND ?6 LIKE lens AND ?7 BETWEEN iso_min AND iso_max"
           "          AND ?8 BETWEEN exposure_min AND exposure_max"
           "          AND ?9 BETWEEN aperture_min AND aperture_max"
           "          AND ?10 BETWEEN focal_length_min AND focal_length_max"
           "          AND (format = 0 OR (format&?11 != 0 AND ~format&?12 != 0)))"
           "        OR (name = ?13))"
           "   AND operation NOT IN"
           "        ('ioporder', 'metadata', 'modulegroups', 'export', 'tagging', 'collect', 'basecurve')"
           " ORDER BY writeprotect DESC, LENGTH(model), LENGTH(maker), LENGTH(lens)",
           preset_table[legacy]);
  // clang-format on
  // query for all modules at once:
  sqlite3_stmt *stmt;
  const char *workflow_preset = has_matrix ? _("scene-referred default") : "\t\n";
  int iformat = 0;
  if(dt_image_is_rawprepare_supported(image)) iformat |= FOR_RAW;
  else iformat |= FOR_LDR;
  if(dt_image_is_hdr(image)) iformat |= FOR_HDR;

  int excluded = 0;
  if(dt_image_monochrome_flags(image)) excluded |= FOR_NOT_MONO;
  else excluded |= FOR_NOT_COLOR;

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
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

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
                                "       AND (format = 0 OR (format&?11 != 0 AND ~format&?12 != 0))"
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

  image->flags |= DT_IMAGE_AUTO_PRESETS_APPLIED | DT_IMAGE_NO_LEGACY_PRESETS;

  // make sure these end up in the image_cache; as the history is not correct right now
  // we don't write the sidecar here but later in dt_dev_read_history_ext
  dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_RELAXED);

  return TRUE;
}

static void _dev_add_default_modules(dt_develop_t *dev, const int imgid)
{
  // modules that cannot be disabled
  // or modules that can be disabled but are auto-on
  for(GList *modules = dev->iop; modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)modules->data;

    if(!dt_history_check_module_exists(imgid, module->op, FALSE)
       && module->default_enabled
       && !(module->flags() & IOP_FLAGS_NO_HISTORY_STACK))
    {
      _dev_insert_module(dev, module, imgid);
    }
  }
}

static void _dev_merge_history(dt_develop_t *dev, const int imgid)
{
  sqlite3_stmt *stmt;

  // count what we found:
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT COUNT(*) FROM memory.history", -1,
                              &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    // if there is anything..
    const int cnt = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    // workaround a sqlite3 "feature". The above statement to insert
    // items into memory.history is complex and in this case sqlite
    // does not give rowid a linear increment. But the following code
    // really expect that the rowid in this table starts from 0 and
    // increment one by one. So in the following code we rewrite the
    // "num" values from 0 to cnt-1.

    if(cnt > 0)
    {
      // get all rowids
      GList *rowids = NULL;

      // get the rowids in descending order since building the list will reverse the order
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "SELECT rowid FROM memory.history ORDER BY rowid DESC",
                                  -1, &stmt, NULL);
      while(sqlite3_step(stmt) == SQLITE_ROW)
        rowids = g_list_prepend(rowids, GINT_TO_POINTER(sqlite3_column_int(stmt, 0)));
      sqlite3_finalize(stmt);

      // update num accordingly
      int v = 0;

      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "UPDATE memory.history SET num=?1 WHERE rowid=?2",
                                  -1, &stmt, NULL);

      // let's wrap this into a transaction, it might make it a little faster.
      dt_database_start_transaction(darktable.db);

      for(GList *r = rowids; r; r = g_list_next(r))
      {
        DT_DEBUG_SQLITE3_CLEAR_BINDINGS(stmt);
        DT_DEBUG_SQLITE3_RESET(stmt);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, v);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, GPOINTER_TO_INT(r->data));

        if(sqlite3_step(stmt) != SQLITE_DONE) break;

        v++;
      }

      dt_database_release_transaction(darktable.db);

      g_list_free(rowids);

      // advance the current history by cnt amount, that is, make space
      // for the preset/default iops that will be *prepended* into the
      // history.
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "UPDATE main.history SET num=num+?1 WHERE imgid=?2",
                                  -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, cnt);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);

      if(sqlite3_step(stmt) == SQLITE_DONE)
      {
        sqlite3_finalize(stmt);
        // clang-format off
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "UPDATE main.images"
                                    " SET history_end=history_end+?1"
                                    " WHERE id=?2",
                                    -1, &stmt, NULL);
        // clang-format on
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, cnt);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);

        if(sqlite3_step(stmt) == SQLITE_DONE)
        {
          // and finally prepend the rest with increasing numbers (starting at 0)
          sqlite3_finalize(stmt);
          // clang-format off
          DT_DEBUG_SQLITE3_PREPARE_V2(
            dt_database_get(darktable.db),
            "INSERT INTO main.history"
            " SELECT imgid, num, module, operation, op_params, enabled, "
            "        blendop_params, blendop_version, multi_priority,"
            "        multi_name"
            " FROM memory.history",
            -1, &stmt, NULL);
          // clang-format on
          sqlite3_step(stmt);
          sqlite3_finalize(stmt);
        }
      }
    }
  }
}

// helper function for debug strings
char * _print_validity(gboolean state)
{
  if(state)
    return "ok";
  else
    return "WRONG";
}


static inline void _dt_dev_load_pipeline_defaults(dt_develop_t *dev)
{
  for(const GList *modules = g_list_first(dev->iop); modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    dt_iop_reload_defaults(module);
  }
}


/**
 * TODO: this is a big pile of bullshit
 *
 * We insert modules into a temporary history SQL table in memory.history
 * Then perform all kinds of silly SQL operations.
 * Then merge into where we keep the real histories, aka main.history in dev_merge_history function.
 *
 * First of all, that merge_history function needs to re-index all entries sequentially through C
 * because SQLite doesn't do it.
 *
 * Then, when loading large numbers of small files (PNG, JPEG) for the first time in lighttable,
 * sooner or later, we get the error:
 * `function dt_database_start_transaction_debug(), query "BEGIN": cannot start a transaction within a transaction`,
 * coming from _merge_history. When using a DEBUG build, which checks asserts, that makes the app crash.
 * Otherwise, the app doesn't crash and there is no telling what's going on in histories.
 *
 * But then, I couldn't find where we nest transactions here.
 *
 * Or perhaps, due to DEBUG builds being slow due to -O0 optimization, the race condition shows, and doesn't otherwise.
 *
 * So, anyway… history init should be done in C, so modules are inserted with defaults params inited
 * and sanitized directly with a pipe order. Then, we save to history or keep building the pipeline,
 * because anyway, read_history_ext() init defaults only if it's the first time we open the image,
 * and then reloads everything from main.history table from database.
 *
 * None of that is thread-safe.
 *
 **/

static void _init_default_history(dt_develop_t *dev, const int imgid, gboolean *first_run, gboolean *auto_apply_modules)
{
  // cleanup DB
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM memory.history", NULL, NULL, NULL);
  dt_print(DT_DEBUG_HISTORY, "[history] temporary history deleted\n");

  // make sure all modules default params are loaded to init history
  _dt_dev_load_pipeline_defaults(dev);

  // prepend all default modules to memory.history
  _dev_add_default_modules(dev, imgid);
  const int default_modules = _dev_get_module_nb_records();

  // maybe add auto-presets to memory.history
  *first_run = _dev_auto_apply_presets(dev);
  *auto_apply_modules = _dev_get_module_nb_records() - default_modules;
  dt_print(DT_DEBUG_HISTORY, "[history] temporary history initialised with default params and presets\n");

  // now merge memory.history into main.history
  _dev_merge_history(dev, imgid);
  dt_print(DT_DEBUG_HISTORY, "[history] temporary history merged with image history\n");
}


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
                          const int modversion, int *legacy_params)
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
      fprintf(stderr, "[dev_read_history] module `%s' version mismatch: history is %d, dt %d.\n",
              hist->module->op, modversion, hist->module->version());

      dt_control_log(_("module `%s' version mismatch: %d != %d"), hist->module->op,
                      hist->module->version(), modversion);
      dt_dev_free_history_item(hist);
      return 1;
    }
    else
    {
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
      */
    if(!strcmp(hist->module->op, "flip") && hist->enabled == 0 && labs(modversion) == 1)
    {
      memcpy(hist->params, hist->module->default_params, hist->module->params_size);
      hist->enabled = 1;
    }
  }

  // Copy params from history entry to module internals
  memcpy(hist->module->params, hist->params, hist->module->params_size);

  return 0;
}

static int _process_history_db_entry(dt_develop_t *dev, sqlite3_stmt *stmt, const int imgid, int *legacy_params)
{
  // Unpack the DB blobs
  const int id = sqlite3_column_int(stmt, 0);
  const int num = sqlite3_column_int(stmt, 1);
  const int modversion = sqlite3_column_int(stmt, 2);
  const char *module_name = (const char *)sqlite3_column_text(stmt, 3);
  const void *module_params = sqlite3_column_blob(stmt, 4);
  int enabled = sqlite3_column_int(stmt, 5);
  const void *blendop_params = sqlite3_column_blob(stmt, 6);
  const int blendop_version = sqlite3_column_int(stmt, 7);
  const int multi_priority = sqlite3_column_int(stmt, 8);
  const char *multi_name = (const char *)sqlite3_column_text(stmt, 9);

  const int param_length = sqlite3_column_bytes(stmt, 4);
  const int bl_length = sqlite3_column_bytes(stmt, 6);

  // Sanity checks
  const gboolean is_valid_id = (id == imgid);
  const gboolean has_module_name = (module_name != NULL);

  if(!(has_module_name && is_valid_id))
  {
    fprintf(stderr, "[dev_read_history] database history for image `%s' seems to be corrupted!\n",
            dev->image_storage.filename);
    return 1;
  }

  const int iop_order = dt_ioppr_get_iop_order(dev->iop_order_list, module_name, multi_priority);

  // Init a bare minimal history entry
  dt_dev_history_item_t *hist = (dt_dev_history_item_t *)calloc(1, sizeof(dt_dev_history_item_t));
  hist->module = NULL;
  hist->enabled = (enabled != 0); // "cast" int into a clean gboolean
  hist->num = num;
  hist->iop_order = iop_order;
  hist->multi_priority = multi_priority;
  g_strlcpy(hist->op_name, module_name, sizeof(hist->op_name));
  g_strlcpy(hist->multi_name, multi_name, sizeof(hist->multi_name));

  // Find a .so file that matches our history entry, aka a module to run the params stored in DB
  _find_so_for_history_entry(dev, hist);

  if(!hist->module)
  {
    fprintf(
        stderr,
        "[dev_read_history] the module `%s' requested by image `%s' is not installed on this computer!\n",
        module_name, dev->image_storage.filename);
    free(hist);
    return 1;
  }

  // Update IOP order stuff, that applies to all modules regardless of their internals
  hist->module->iop_order = hist->iop_order;
  dt_iop_update_multi_priority(hist->module, hist->multi_priority);

  // module has no user params and won't bother us in GUI - exit early, we are done
  if(hist->module->flags() & IOP_FLAGS_NO_HISTORY_STACK)
  {
    free(hist);
    return 1;
  }

  // Last chance & desperate attempt at enabling/disabling critical modules
  // when history is garbled - This might prevent segfaults on invalid data
  if(hist->module->force_enable) enabled = hist->module->force_enable(hist->module, enabled);

  dt_print(DT_DEBUG_HISTORY, "[history] successfully loaded module %s history (enabled: %i)\n", hist->module->op, enabled);

  // Copy instance name
  g_strlcpy(hist->module->multi_name, hist->multi_name, sizeof(hist->module->multi_name));

  // Copy blending params if valid, else try to convert legacy params
  _sync_blendop_params(hist, blendop_params, bl_length, blendop_version, legacy_params);

  // Copy module params if valid, else try to convert legacy params
  if(_sync_params(hist, module_params, param_length, modversion, legacy_params))
    return 1;

  // make sure that always-on modules are always on. duh.
  if(hist->module->default_enabled == 1 && hist->module->hide_enable_button == 1)
    hist->enabled = hist->module->enabled = TRUE;

  dev->history = g_list_append(dev->history, hist);
  dt_dev_set_history_end(dev, dt_dev_get_history_end(dev) + 1);

  return 0;
}


void dt_dev_read_history_ext(dt_develop_t *dev, const int imgid, gboolean no_image)
{
  if(imgid <= 0) return;
  if(!dev->iop) return;

  int auto_apply_modules = 0;
  gboolean first_run = FALSE;
  gboolean legacy_params = FALSE;

  dt_ioppr_set_default_iop_order(dev, imgid);

  if(!no_image) _init_default_history(dev, imgid, &first_run, &auto_apply_modules);

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
  {
    if(_process_history_db_entry(dev, stmt, imgid, &legacy_params))
      continue;
  }
  sqlite3_finalize(stmt);

  // find the new history end
  // Note: dt_dev_set_history_end sanitizes the value with the actual history size.
  // It needs to run after dev->history is fully populated
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT history_end FROM main.images WHERE id = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW) // seriously, this should never fail
    if(sqlite3_column_type(stmt, 0) != SQLITE_NULL)
      dt_dev_set_history_end(dev, sqlite3_column_int(stmt, 0));
  sqlite3_finalize(stmt);

  // Sanitize and flatten module order
  dt_ioppr_resync_modules_order(dev);
  dt_ioppr_check_iop_order(dev, imgid, "dt_dev_read_history_no_image end");

  // Update masks history
  // Note: until there, we had only blendops. No masks
  dt_masks_read_masks_history(dev, imgid);

  // Copy and publish the masks on the raster stack for other modules to find
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

    dt_iop_commit_blend_params(hist->module, hist->blend_params);

    // Compute the history params hash.
    // This needs to run after dt_iop_compute_blendop_hash(),
    // which is called in dt_iop_commit_blend_params,
    // which needs to run after dt_masks_read_masks_history()
    // TL;DR: don't move this higher, it needs blendop AND mask shapes
    dt_iop_compute_module_hash(hist->module);
    hist->hash = hist->module->hash;
  }

  dt_dev_masks_list_change(dev);
  dt_dev_masks_update_hash(dev);

  dt_print(DT_DEBUG_HISTORY, "[history] dt_dev_read_history_ext completed\n");
}

void dt_dev_read_history(dt_develop_t *dev)
{
  dt_pthread_mutex_lock(&dev->history_mutex);
  dt_dev_read_history_ext(dev, dev->image_storage.id, FALSE);
  dt_pthread_mutex_unlock(&dev->history_mutex);
}

void dt_dev_get_history_item_label(dt_dev_history_item_t *hist, char *label, const int cnt)
{
  gchar *module_label = dt_history_item_get_name(hist->module);
  g_snprintf(label, cnt, "%s (%s)", module_label, hist->enabled ? _("on") : _("off"));
  g_free(module_label);
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
  if(!dev->iop) return;

  dt_pthread_mutex_lock(&dev->history_mutex);

  // Cleanup old history
  dt_dev_history_free_history(dev);

  // Rebuild an history from current pipeline
  for(GList *item = g_list_first(dev->iop); item; item = g_list_next(item))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(item->data);
    if(module->enabled && !_module_leaves_no_history(module))
      dt_dev_add_history_item_ext(dev, module, FALSE, TRUE, TRUE, TRUE);
  }

  dt_pthread_mutex_unlock(&dev->history_mutex);

  // Commit to DB
  dt_dev_write_history(dev);

  // Reload to sanitize mandatory/incompatible modules
  dt_dev_reload_history_items(dev);
}
