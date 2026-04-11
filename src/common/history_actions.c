/*
    This file is part of Ansel,
    Copyright (C) 2026 Aurélien PIERRE.

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

#include "common/history_actions.h"

#include "common/darktable.h"
#include "common/debug.h"
#include "common/exif.h"
#include "common/history.h"
#include "common/history_snapshot.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "common/styles.h"
#include "common/tags.h"
#include "common/undo.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/dev_history.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "dtgtk/thumbtable.h"
#include "gui/hist_dialog.h"

static void _history_action_finalize_list(const GList *list, const gboolean changed)
{
  if(!changed) return;

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_TAG_CHANGED);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_IMAGE_INFO_CHANGED, g_list_copy((GList *)list));
}

typedef gboolean (*dt_history_action_fn)(const int32_t imgid, void *user_data);

static gboolean _history_action_on_list_with_undo(const GList *list, dt_history_action_fn action, void *user_data,
                                                  const gboolean use_undo)
{
  if(IS_NULL_PTR(list)) return FALSE;

  if(use_undo) dt_undo_start_group(darktable.undo, DT_UNDO_LT_HISTORY);
  gboolean changed = FALSE;
  for(const GList *l = list; l; l = g_list_next(l))
  {
    const int32_t imgid = GPOINTER_TO_INT(l->data);
    changed |= action(imgid, user_data);
  }
  if(use_undo) dt_undo_end_group(darktable.undo);

  _history_action_finalize_list(list, changed);
  return changed;
}

static gboolean _history_action_on_list(const GList *list, dt_history_action_fn action, void *user_data)
{
  return _history_action_on_list_with_undo(list, action, user_data, TRUE);
}

/**
 * @brief Build a module list to copy based on selected history indices.
 *
 * When @p ops is NULL, builds a list from the full history depending on @p copy_full.
 *
 * @param dev_src Source develop context.
 * @param ops Optional list of history indices (as GPOINTER_TO_UINT).
 * @param copy_full Whether to include disabled/default items.
 * @return Newly-allocated list of modules to copy.
 */
static GList *_get_user_mod_list(dt_develop_t *dev_src, GList *ops, gboolean copy_full)
{
  GList *mod_list = NULL;

  if(ops)
  {
    dt_print(DT_DEBUG_PARAMS, "[_history_copy_and_paste_on_image_merge] pasting selected IOP\n");

    // copy only selected history entries
    for(const GList *l = g_list_last(ops); l; l = g_list_previous(l))
    {
      const unsigned int num = GPOINTER_TO_UINT(l->data);
      const dt_dev_history_item_t *hist = NULL;
      for(GList *h = g_list_last(dev_src->history); h; h = g_list_previous(h))
      {
        const dt_dev_history_item_t *item = (dt_dev_history_item_t *)h->data;
        if(item && item->num == (int)num)
        {
          hist = item;
          break;
        }
      }

      if(hist)
      {
        dt_iop_module_t *mod = hist->module;
        if(mod && !dt_iop_is_hidden(mod))
        {
          dt_print(DT_DEBUG_HISTORY, "selected for copy/pasting : module %20s, multiprio %i", mod->op, mod->multi_priority);

          mod_list = g_list_prepend(mod_list, mod);
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

  return g_list_reverse(mod_list);
}

/**
 * @brief Copy/merge history between images using the merge pipeline.
 *
 * Builds a source dev, resolves the module list, and dispatches to dt_history_merge().
 *
 * @param imgid Source image id.
 * @param dest_imgid Destination image id.
 * @param ops Optional list of history indices to copy.
 * @param copy_full Whether to copy the full history.
 * @param mode Merge strategy.
 * @return 0 on success, non-zero on failure.
 */
static int _history_copy_and_paste_on_image_merge(int32_t imgid, int32_t dest_imgid, GList *ops,
                                                  const gboolean copy_full, const dt_history_merge_strategy_t mode)
{
  // Init source history + pipeline
  dt_develop_t _dev_src = { 0 };
  dt_develop_t *dev_src = &_dev_src;
  dt_dev_init(dev_src, FALSE);
  dt_dev_reload_history_items(dev_src, imgid);

  int ret_val = 0;

  if(mode == DT_HISTORY_MERGE_REPLACE)
  {
    // Dumb mode : keep dev_src intact but swap destination imgid and image info into it.
    //
    // NOTE: when pasting from LDR/JPEG onto RAW images, the source iop-order list might not be compatible
    // with the destination pipeline. We need to re-init the iop-order list for the destination image and
    // reload module defaults for the destination image, but we must not apply auto-presets.
    ret_val = dt_dev_replace_history_on_image(dev_src, dest_imgid, TRUE, "_history_copy_and_paste_on_image_merge");
  }
  else
  {
    // Merge
    GList *mod_list = _get_user_mod_list(dev_src, ops, copy_full);
    ret_val = dt_dev_merge_history_into_image(dev_src, dest_imgid, mod_list,
                                              dt_conf_get_bool("history/copy_iop_order"), mode,
                                              dt_conf_get_bool("history/paste_instances"));
    g_list_free(mod_list);
    mod_list = NULL;
  }
  dt_dev_cleanup(dev_src);

  return ret_val;
}

gboolean dt_history_copy_and_paste_on_image(const int32_t imgid, const int32_t dest_imgid, GList *ops,
                                            const gboolean copy_full, const dt_history_merge_strategy_t mode)
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

  int ret_val = _history_copy_and_paste_on_image_merge(imgid, dest_imgid, ops, copy_full, mode);

  dt_history_snapshot_undo_create(hist->imgid, &hist->after, &hist->after_history_end);
  dt_undo_record(darktable.undo, NULL, DT_UNDO_LT_HISTORY, (dt_undo_data_t)hist,
                 dt_history_snapshot_undo_pop, dt_history_snapshot_undo_lt_history_data_free);

  return ret_val;
}

gboolean dt_history_copy(int32_t imgid)
{
  // note that this routine does not copy anything, it just setup the copy_paste proxy
  // with the needed information that will be used while pasting.

  if(imgid <= 0) return FALSE;

  darktable.view_manager->copy_paste.copied_imageid = imgid;

  return TRUE;
}

gboolean dt_history_copy_parts(int32_t imgid)
{
  if(dt_history_copy(imgid))
  {
    // run dialog, it will insert into selops the selected moduel

    if(dt_gui_hist_dialog_new(&(darktable.view_manager->copy_paste), imgid, TRUE) == GTK_RESPONSE_CANCEL)
      return FALSE;
    return TRUE;
  }
  else
    return FALSE;
}

static gboolean _history_paste_apply(const int32_t imgid, void *user_data)
{
  (void)user_data;
  if(darktable.view_manager->copy_paste.copied_imageid <= 0) return FALSE;
  if(imgid <= 0) return FALSE;

  const gboolean pasted = dt_history_copy_and_paste_on_image(darktable.view_manager->copy_paste.copied_imageid,
                                                             imgid,
                                                             darktable.view_manager->copy_paste.selops,
                                                             FALSE,
                                                             dt_conf_get_int("history/mode")) == 0;
  return pasted;
}

gboolean dt_history_paste_on_image(const int32_t imgid)
{
  return _history_paste_apply(imgid, NULL);
}

gboolean dt_history_paste_on_list(const GList *list)
{
  if(darktable.view_manager->copy_paste.copied_imageid <= 0) return FALSE;
  return _history_action_on_list(list, _history_paste_apply, NULL);
}

gboolean dt_history_paste_parts_prepare(void)
{
  if(darktable.view_manager->copy_paste.copied_imageid <= 0) return FALSE;

  // we launch the dialog
  const int res = dt_gui_hist_dialog_new(&(darktable.view_manager->copy_paste),
                                         darktable.view_manager->copy_paste.copied_imageid, FALSE);

  if(res != GTK_RESPONSE_OK)
  {
    return FALSE;
  }

  return TRUE;
}

static gboolean _history_paste_parts_apply(const int32_t imgid, void *user_data)
{
  (void)user_data;
  if(darktable.view_manager->copy_paste.copied_imageid <= 0) return FALSE;
  if(IS_NULL_PTR(darktable.view_manager->copy_paste.selops)) return FALSE;
  if(imgid <= 0) return FALSE;

  const gboolean pasted = dt_history_copy_and_paste_on_image(darktable.view_manager->copy_paste.copied_imageid,
                                                             imgid,
                                                             darktable.view_manager->copy_paste.selops,
                                                             FALSE,
                                                             dt_conf_get_int("history/mode")) == 0;
  return pasted;
}

gboolean dt_history_paste_parts_on_image(const int32_t imgid)
{
  return _history_paste_parts_apply(imgid, NULL);
}

gboolean dt_history_paste_parts_on_list(const GList *list)
{
  if(darktable.view_manager->copy_paste.copied_imageid <= 0) return FALSE;
  if(IS_NULL_PTR(darktable.view_manager->copy_paste.selops))
    return FALSE;
  return _history_action_on_list(list, _history_paste_parts_apply, NULL);
}

static gboolean _history_compress_apply(const int32_t imgid, void *user_data)
{
  (void)user_data;
  dt_print(DT_DEBUG_HISTORY, "[dt_history_compress_on_image] compressing history for image %i\n", imgid);
  if(imgid <= 0) return FALSE;

  dt_develop_t dev;
  dt_dev_init(&dev, FALSE);
  dt_dev_reload_history_items(&dev, imgid);
  dt_dev_history_compress_or_truncate(&dev);
  dt_dev_write_history_ext(&dev, imgid);
  dt_dev_cleanup(&dev);
  return TRUE;
}

void dt_history_compress_on_image(const int32_t imgid)
{
  _history_compress_apply(imgid, NULL);
}

int dt_history_compress_on_list(const GList *imgs)
{
  _history_action_on_list(imgs, _history_compress_apply, NULL);
  return 0;
}

typedef struct dt_history_load_params_t
{
  gchar *filename;
  int history_only;
} dt_history_load_params_t;

static gboolean _history_load_and_apply_apply(const int32_t imgid, void *user_data)
{
  dt_history_load_params_t *params = (dt_history_load_params_t *)user_data;
  dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'w');
  if(IS_NULL_PTR(img)) return FALSE;

  dt_undo_lt_history_t *hist = dt_history_snapshot_item_init();
  hist->imgid = imgid;
  dt_history_snapshot_undo_create(hist->imgid, &hist->before, &hist->before_history_end);

  if(dt_exif_xmp_read(img, params->filename, params->history_only))
  {
    dt_image_cache_write_release(darktable.image_cache, img,
                                 // ugly but if not history_only => called from crawler - do not write the xmp
                                 params->history_only ? DT_IMAGE_CACHE_SAFE : DT_IMAGE_CACHE_RELAXED);
    return FALSE;
  }

  dt_history_snapshot_undo_create(hist->imgid, &hist->after, &hist->after_history_end);
  dt_undo_record(darktable.undo, NULL, DT_UNDO_LT_HISTORY, (dt_undo_data_t)hist,
                 dt_history_snapshot_undo_pop, dt_history_snapshot_undo_lt_history_data_free);

  dt_image_cache_write_release(darktable.image_cache, img,
                               // ugly but if not history_only => called from crawler - do not write the xmp
                               params->history_only ? DT_IMAGE_CACHE_SAFE : DT_IMAGE_CACHE_RELAXED);

  return TRUE;
}

int dt_history_load_and_apply(const int32_t imgid, gchar *filename, int history_only)
{
  dt_history_load_params_t params = { .filename = filename, .history_only = history_only };
  return _history_load_and_apply_apply(imgid, &params) ? 0 : 1;
}

int dt_history_load_and_apply_on_image(int32_t imgid, gchar *filename, int history_only)
{
  return dt_history_load_and_apply(imgid, filename, history_only);
}

int dt_history_load_and_apply_on_list(gchar *filename, const GList *list)
{
  dt_history_load_params_t params = { .filename = filename, .history_only = 1 };
  const gboolean changed = _history_action_on_list(list, _history_load_and_apply_apply, &params);
  return changed ? 0 : 1;
}

typedef struct dt_history_delete_params_t
{
  gboolean undo;
} dt_history_delete_params_t;

static gboolean _history_delete_apply(const int32_t imgid, void *user_data)
{
  if(imgid <= 0) return FALSE;

  const dt_history_delete_params_t *params = (dt_history_delete_params_t *)user_data;
  const gboolean undo = params ? params->undo : TRUE;

  dt_undo_lt_history_t *hist = NULL;
  if(undo)
  {
    hist = dt_history_snapshot_item_init();
    hist->imgid = imgid;
    dt_history_snapshot_undo_create(hist->imgid, &hist->before, &hist->before_history_end);
  }

  dt_history_delete_on_image_ext(imgid, FALSE);

  if(undo)
  {
    dt_history_snapshot_undo_create(hist->imgid, &hist->after, &hist->after_history_end);
    dt_undo_record(darktable.undo, NULL, DT_UNDO_LT_HISTORY, (dt_undo_data_t)hist, dt_history_snapshot_undo_pop,
                   dt_history_snapshot_undo_lt_history_data_free);
  }

  return TRUE;
}

gboolean dt_history_delete_on_list(const GList *list, gboolean undo)
{
  dt_history_delete_params_t params = { .undo = undo };
  return _history_action_on_list_with_undo(list, _history_delete_apply, &params, undo);
}

typedef struct dt_history_style_params_t
{
  const char *name;
  int style_id;
  gboolean duplicate;
  dt_history_merge_strategy_t mode;
} dt_history_style_params_t;

static gboolean _history_style_apply(const int32_t imgid, void *user_data)
{
  const dt_history_style_params_t *params = (dt_history_style_params_t *)user_data;
  if(IS_NULL_PTR(params) || params->style_id == 0 || IS_NULL_PTR(params->name) || !*params->name) return FALSE;

  int32_t newimgid = imgid;
  if(params->duplicate)
  {
    newimgid = dt_image_duplicate(imgid);
    if(newimgid == UNKNOWN_IMAGE) return FALSE;

    const gboolean pasted = dt_history_copy_and_paste_on_image(imgid, newimgid, NULL, TRUE, params->mode) == 0;
    return pasted;
  }

  dt_undo_lt_history_t *hist = dt_history_snapshot_item_init();
  hist->imgid = newimgid;
  dt_history_snapshot_undo_create(hist->imgid, &hist->before, &hist->before_history_end);

  const int ret_val = dt_styles_apply_to_image_merge(params->name, params->style_id, newimgid, params->mode);

  dt_history_snapshot_undo_create(hist->imgid, &hist->after, &hist->after_history_end);
  dt_undo_record(darktable.undo, NULL, DT_UNDO_LT_HISTORY, (dt_undo_data_t)hist,
                 dt_history_snapshot_undo_pop, dt_history_snapshot_undo_lt_history_data_free);

  const gboolean changed = (ret_val == 0);
  return changed;
}

gboolean dt_history_style_on_image(const int32_t imgid, const char *name, const gboolean duplicate)
{
  if(IS_NULL_PTR(name) || !*name) return FALSE;

  dt_history_style_params_t params = {
    .name = name,
    .style_id = dt_styles_get_id_by_name(name),
    .duplicate = duplicate,
    .mode = dt_conf_get_int("history/mode"),
  };
  if(params.style_id == 0) return FALSE;

  return _history_style_apply(imgid, &params);
}

gboolean dt_history_style_on_list(const GList *list, const char *name, const gboolean duplicate)
{
  if(IS_NULL_PTR(name) || !*name) return FALSE;

  dt_history_style_params_t params = {
    .name = name,
    .style_id = dt_styles_get_id_by_name(name),
    .duplicate = duplicate,
    .mode = dt_conf_get_int("history/mode"),
  };
  if(params.style_id == 0) return FALSE;

  return _history_action_on_list(list, _history_style_apply, &params);
}
