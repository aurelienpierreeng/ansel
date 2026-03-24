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
#include <assert.h>
#include <stddef.h>
#include <glib/gprintf.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "common/atomic.h"
#include "common/datetime.h"
#include "common/debug.h"
#include "common/history.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/mipmap_cache.h"
#include "common/opencl.h"
#include "common/tags.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/signal.h"
#include "control/jobs.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/lightroom.h"
#include "develop/masks.h"
#include "develop/pixelpipe_cache.h"
#include "develop/pixelpipe_gui.h"
#include "gui/gtk.h"
#include "gui/gui_throttle.h"
#include "gui/presets.h"
#include "libs/colorpicker.h"

#define DT_IOP_ORDER_INFO (darktable.unmuted & DT_DEBUG_IOPORDER)

GList *dt_dev_load_modules(dt_develop_t *dev)
{
  GList *res = NULL;
  dt_iop_module_t *module;
  dt_iop_module_so_t *module_so;
  GList *iop = g_list_first(darktable.iop);
  while(iop)
  {
    module_so = (dt_iop_module_so_t *)iop->data;
    module = (dt_iop_module_t *)calloc(1, sizeof(dt_iop_module_t));
    if(dt_iop_load_module_by_so(module, module_so, dev))
    {
      dt_free(module);
      continue;
    }
    res = g_list_insert_sorted(res, module, dt_sort_iop_by_order);
    module->global_data = module_so->data;
    module->so = module_so;
    iop = g_list_next(iop);
  }

  GList *it = res;
  while(it)
  {
    module = (dt_iop_module_t *)it->data;
    it = g_list_next(it);
  }
  return res;
}

void dt_dev_init(dt_develop_t *dev, int32_t gui_attached)
{
  memset(dev, 0, sizeof(dt_develop_t));
  dt_dev_set_history_hash(dev, DT_PIXELPIPE_CACHE_HASH_INVALID);
  dt_pthread_rwlock_init(&dev->history_mutex, NULL);
  dt_pthread_rwlock_init(&dev->masks_mutex, NULL);

  dev->gui_attached = gui_attached;
  dev->roi.width = -1;
  dev->roi.height = -1;

  dt_image_init(&dev->image_storage);

  if(dev->gui_attached)
  {
    dev->pipe = (dt_dev_pixelpipe_t *)malloc(sizeof(dt_dev_pixelpipe_t));
    dev->preview_pipe = (dt_dev_pixelpipe_t *)malloc(sizeof(dt_dev_pixelpipe_t));
    // Virtual pipe mirrors preview_pipe for geometry, but is never processed.
    dev->virtual_pipe = (dt_dev_pixelpipe_t *)malloc(sizeof(dt_dev_pixelpipe_t));
    dt_dev_pixelpipe_init(dev->pipe);
    dt_dev_pixelpipe_init_preview(dev->preview_pipe);
    dt_dev_pixelpipe_init_preview(dev->virtual_pipe);
    dev->histogram_pre_tonecurve = (uint32_t *)calloc(4 * 256, sizeof(uint32_t));
    dev->histogram_pre_levels = (uint32_t *)calloc(4 * 256, sizeof(uint32_t));

    // FIXME: these are uint32_t, setting to -1 is confusing
    dev->histogram_pre_tonecurve_max = -1;
    dev->histogram_pre_levels_max = -1;
  }

  dt_dev_set_backbuf(&dev->raw_histogram, 0, 0, 0, -1, -1);
  dt_dev_set_backbuf(&dev->output_histogram, 0, 0, 0, -1, -1);
  dt_dev_set_backbuf(&dev->display_histogram, 0, 0, 0, -1, -1);

  dev->proxy.wb_is_D65 = TRUE; // don't display error messages until we know for sure it's FALSE
  dev->proxy.wb_coeffs[0] = 0.f;

  dev->rawoverexposed.mode = dt_conf_get_int("darkroom/ui/rawoverexposed/mode");
  dev->rawoverexposed.colorscheme = dt_conf_get_int("darkroom/ui/rawoverexposed/colorscheme");
  dev->rawoverexposed.threshold = dt_conf_get_float("darkroom/ui/rawoverexposed/threshold");

  dev->overexposed.mode = dt_conf_get_int("darkroom/ui/overexposed/mode");
  dev->overexposed.colorscheme = dt_conf_get_int("darkroom/ui/overexposed/colorscheme");
  dev->overexposed.lower = dt_conf_get_float("darkroom/ui/overexposed/lower");
  dev->overexposed.upper = dt_conf_get_float("darkroom/ui/overexposed/upper");

  if(dev->gui_attached)
  {
    dev->color_picker.primary_sample = g_malloc0(sizeof(dt_colorpicker_sample_t));
    dev->color_picker.display_samples = dt_conf_get_bool("ui_last/colorpicker_display_samples");
    dev->color_picker.live_samples_enabled = TRUE;
    dev->color_picker.restrict_histogram = dt_conf_get_bool("ui_last/colorpicker_restrict_histogram");
    dt_dev_pixelpipe_gui_init();
  }

  dt_dev_reset_roi(dev);

  dev->iop = dt_dev_load_modules(dev);
}

void dt_dev_cleanup(dt_develop_t *dev)
{
  if(!dev) return;
  // image_cache does not have to be unref'd, this is done outside develop module.

  dt_gui_throttle_cancel(dev);

  dev->proxy.chroma_adaptation = NULL;
  dev->proxy.wb_coeffs[0] = 0.f;
  if(dev->pipe)
  {
    dt_dev_pixelpipe_cleanup(dev->pipe);
    dt_free(dev->pipe);
  }
  if(dev->preview_pipe)
  {
    dt_dev_pixelpipe_cleanup(dev->preview_pipe);
    dt_free(dev->preview_pipe);
  }
  if(dev->virtual_pipe)
  {
    // Virtual pipe has nodes and committed params but no pixel buffers.
    dt_dev_pixelpipe_cleanup(dev->virtual_pipe);
    dt_free(dev->virtual_pipe);
  }

  if(dev->gui_attached)
    dt_dev_pixelpipe_gui_cleanup();

  dt_pthread_rwlock_wrlock(&dev->history_mutex);
  while(dev->history)
  {
    dt_dev_free_history_item(((dt_dev_history_item_t *)dev->history->data));
    dev->history = g_list_delete_link(dev->history, dev->history);
  }
  dt_pthread_rwlock_unlock(&dev->history_mutex);
  dt_pthread_rwlock_destroy(&dev->history_mutex);

  // free pending "before" snapshots for history undo
  dev->undo_history_depth = 0;
  g_list_free_full(dev->undo_history_before_snapshot, dt_dev_free_history_item);
  dev->undo_history_before_snapshot = NULL;
  g_list_free_full(dev->undo_history_before_iop_order_list, dt_free_gpointer);
  dev->undo_history_before_iop_order_list = NULL;
  dev->undo_history_before_end = 0;

  while(dev->iop)
  {
    dt_iop_cleanup_module((dt_iop_module_t *)dev->iop->data);
    dt_free(dev->iop->data);
    dev->iop = g_list_delete_link(dev->iop, dev->iop);
  }
  while(dev->alliop)
  {
    dt_iop_cleanup_module((dt_iop_module_t *)dev->alliop->data);
    dt_free(dev->alliop->data);
    dev->alliop = g_list_delete_link(dev->alliop, dev->alliop);
  }
  g_list_free_full(dev->iop_order_list, dt_free_gpointer);
  dev->iop_order_list = NULL;
  while(dev->allprofile_info)
  {
    dt_ioppr_cleanup_profile_info((dt_iop_order_iccprofile_info_t *)dev->allprofile_info->data);
    dt_free_align(dev->allprofile_info->data);
    dev->allprofile_info->data = NULL;
    dev->allprofile_info = g_list_delete_link(dev->allprofile_info, dev->allprofile_info);
  }

  dt_free(dev->histogram_pre_tonecurve);
  dt_free(dev->histogram_pre_levels);

  if(dev->color_picker.primary_sample)
  {
    dt_free(dev->color_picker.primary_sample);
    dev->color_picker.primary_sample = NULL;
  }

  dt_pthread_rwlock_wrlock(&dev->masks_mutex);
  g_list_free_full(dev->forms, (void (*)(void *))dt_masks_free_form);
  dev->forms = NULL;
  g_list_free_full(dev->allforms, (void (*)(void *))dt_masks_free_form);
  dev->allforms = NULL;
  dt_pthread_rwlock_unlock(&dev->masks_mutex);

  dt_pthread_rwlock_destroy(&dev->masks_mutex);

  dt_conf_set_int("darkroom/ui/rawoverexposed/mode", dev->rawoverexposed.mode);
  dt_conf_set_int("darkroom/ui/rawoverexposed/colorscheme", dev->rawoverexposed.colorscheme);
  dt_conf_set_float("darkroom/ui/rawoverexposed/threshold", dev->rawoverexposed.threshold);

  dt_conf_set_int("darkroom/ui/overexposed/mode", dev->overexposed.mode);
  dt_conf_set_int("darkroom/ui/overexposed/colorscheme", dev->overexposed.colorscheme);
  dt_conf_set_float("darkroom/ui/overexposed/lower", dev->overexposed.lower);
  dt_conf_set_float("darkroom/ui/overexposed/upper", dev->overexposed.upper);
}

static gboolean _update_darkroom_roi(dt_develop_t *dev, dt_dev_pixelpipe_t *pipe, int *x, int *y, int *wd, int *ht,
                                     float *scale);

static gboolean _darkroom_pipeline_inputs_ready(const dt_develop_t *dev)
{
  if(!dev) return FALSE;

  return dev->image_storage.id > 0
         && dev->roi.width >= 32
         && dev->roi.height >= 32
         && dev->roi.raw_width >= 32
         && dev->roi.raw_height >= 32
         && dev->roi.processed_width >= 32
         && dev->roi.processed_height >= 32
         && dev->roi.preview_width >= 32
         && dev->roi.preview_height >= 32;
}

int dt_dev_get_thumbnail_size(dt_develop_t *dev)
{
  if(!dev->roi.raw_inited || !dev->roi.gui_inited) return 1;

  // Keep the virtual pipe synced so ROI computations on the GUI thread
  // always use up-to-date history and input sizes.
  if(dev->virtual_pipe->imgid != dev->image_storage.id
      || dev->virtual_pipe->iwidth != dev->roi.raw_width
      || dev->virtual_pipe->iheight != dev->roi.raw_height
      || dev->virtual_pipe->image.id != dev->image_storage.id)
    dt_dev_pixelpipe_set_input(dev->virtual_pipe, dev, dev->image_storage.id,
                                dev->roi.raw_width, dev->roi.raw_height, DT_MIPMAP_FULL);

  if(!dev->virtual_pipe->nodes)
    dt_dev_pixelpipe_or_changed(dev->virtual_pipe, DT_DEV_PIPE_REMOVE);
  else if(dt_dev_pixelpipe_get_history_hash(dev->virtual_pipe) != dt_dev_get_history_hash(dev))
    dt_dev_pixelpipe_or_changed(dev->virtual_pipe, DT_DEV_PIPE_SYNCH);

  if(dt_dev_pixelpipe_get_changed(dev->virtual_pipe) != DT_DEV_PIPE_UNCHANGED)
    dt_dev_pixelpipe_change(dev->virtual_pipe, dev);

  // Compute the virtual full-res output. This needs an inited history
  dt_dev_pixelpipe_get_roi_out(dev->virtual_pipe, dev, dev->roi.raw_width, dev->roi.raw_height, 
                               &dev->roi.processed_width, &dev->roi.processed_height);

  // Compute the scaling factor that makes full-res output fit within widget
  dev->roi.natural_scale = dt_dev_get_natural_scale(dev);

  // The preview backbuffer and the pipeline ROI both live in raster pixels.
  // `natural_scale` therefore directly maps the processed image size to the
  // raster backbuffer size, without any GUI-density factor mixed in.
  dev->roi.preview_width = dev->roi.natural_scale * dev->roi.processed_width;
  dev->roi.preview_height = dev->roi.natural_scale * dev->roi.processed_height;
  dev->roi.output_inited = TRUE;

  dt_dev_update_mouse_effect_radius(dev); 

  dt_print(DT_DEBUG_DEV,
            "[pixelpipe] thumbnail sizes raw %dx%d -> processed %dx%d -> preview %dx%d (scale %.5f)\n",
            dev->roi.raw_width, dev->roi.raw_height, dev->roi.processed_width, dev->roi.processed_height,
            dev->roi.preview_width, dev->roi.preview_height, dev->roi.natural_scale);
  
  return 0;
}

gboolean dt_dev_pixelpipe_has_preview_output(const dt_develop_t *dev, const dt_dev_pixelpipe_t *pipe,
                                             const dt_iop_roi_t *roi)
{
  if(!dev || !pipe || !dev->gui_attached || !dev->roi.output_inited) return FALSE;

  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  float scale = dev->roi.natural_scale;

  if(roi)
  {
    x = roi->x;
    y = roi->y;
    width = roi->width;
    height = roi->height;
    scale = roi->scale;
  }
  else
  {
    // Recompute the current darkroom output geometry so callers that run ahead of process()
    // still classify the pipe from the image they are about to produce, not the last backbuffer.
    _update_darkroom_roi((dt_develop_t *)dev, (dt_dev_pixelpipe_t *)pipe, &x, &y, &width, &height, &scale);
  }

  if(width != dev->roi.preview_width || height != dev->roi.preview_height) return FALSE;
  return x == 0 && y == 0 && fabsf(scale - dev->roi.natural_scale) < 1e-4f;
}


// Return TRUE if ROI changed since previous computation
static gboolean _update_darkroom_roi(dt_develop_t *dev, dt_dev_pixelpipe_t *pipe, int *x, int *y, int *wd, int *ht,
                                     float *scale)
{  
  if(!dev->roi.output_inited) return 1;

  // Store previous values
  int x_old = *x;
  int y_old = *y;
  int wd_old = *wd;
  int ht_old = *ht;
  float old_scale = *scale;

  // roi->scale is the pipeline sampling ratio against the processed image and
  // therefore excludes the GUI backing-store density.
  *scale = dev->roi.natural_scale;
  const gboolean preview_pipe = (pipe == dev->preview_pipe);
  if(!preview_pipe) *scale *= dev->roi.scaling;

  // Width, height, x and y are already expressed in raster pixels, so they
  // must follow the same raster-space sampling ratio as roi->scale.
  int roi_width = roundf(*scale * dev->roi.processed_width);
  int roi_height = roundf(*scale * dev->roi.processed_height);
  int widget_wd = dev->roi.width;
  int widget_ht = dev->roi.height;

  *wd = roundf(fminf(roi_width, widget_wd));
  *ht = roundf(fminf(roi_height, widget_ht));

  // dev->roi.x,y are the relative coordinates of the ROI center.
  // in preview pipe, we always render a full image, so x,y = 0,0 
  // otherwise, x,y here are the top-left corner. Translate:
  *x = preview_pipe ? 0 : roundf(dev->roi.x * roi_width - *wd * .5f);
  *y = preview_pipe ? 0 : roundf(dev->roi.y * roi_height - *ht * .5f);

/*  fprintf (stderr, "_update_darkroom_roi: dev %.2f %.2f  type %s  xy %d %d  dim %d %d"
                   "   ppd:%.4f scale:%.4f nat_scale:%.4f * scaling:%.4f\n",
            dev->roi.x, dev->roi.y, dt_pipe_type_to_str(pipe->type), *x, *y, *wd, *ht, darktable.gui->ppd, *scale, dev->roi.natural_scale, dev->roi.scaling);
*/
  return x_old != *x || y_old != *y || wd_old != *wd || ht_old != *ht || old_scale != *scale;
}

gboolean dt_dev_pipelines_share_preview_output(dt_develop_t *dev)
{
  if(!dev || !dev->gui_attached || !dev->pipe || !dev->preview_pipe || !dev->roi.output_inited) return FALSE;

  float preview_scale = 1.0f;
  float main_scale = 1.0f;
  int preview_x = 0, preview_y = 0, preview_wd = 0, preview_ht = 0;
  int main_x = 0, main_y = 0, main_wd = 0, main_ht = 0;

  _update_darkroom_roi(dev, dev->preview_pipe, &preview_x, &preview_y, &preview_wd, &preview_ht, &preview_scale);
  _update_darkroom_roi(dev, dev->pipe, &main_x, &main_y, &main_wd, &main_ht, &main_scale);

  return preview_x == main_x && preview_y == main_y && preview_wd == main_wd && preview_ht == main_ht
         && fabsf(preview_scale - main_scale) < 1e-4f;
}

/**
 * @brief Run darkroom preview and main pipelines from one background loop.
 *
 * @details
 * Preview must be serviced before the main pipe so both darkroom pipelines can share freshly-published cachelines
 * without cross-thread timeout heuristics. Pause state, dirty detection, history resync, ROI updates, and re-entry
 * handling stay local to each pipe, but their execution order is now explicit and deterministic.
 *
 * GUI sampling and picker notifications are intentionally delayed until both ordered pipe runs completed so GUI
 * observers consume the preview-first, main-second cache state from the same loop.
 */
void dt_dev_darkroom_pipeline(dt_develop_t *dev)
{
  dt_dev_pixelpipe_t *const pipes[] = { dev->preview_pipe, dev->pipe };

  for(size_t i = 0; i < G_N_ELEMENTS(pipes); i++)
    pipes[i]->running = 1;

  // Infinite loop: run for as long as the worker thread is running.
  while(!dev->exit && dt_control_running())
  {
    if(!_darkroom_pipeline_inputs_ready(dev))
    {
      dt_iop_nap(50000); // wait 50 ms until GUI/image sizes are initialized
      continue;
    }

    // Always service preview first, then the main pipe, so the main pipe can reuse the cache state
    // just published by preview instead of trying to race it from another thread.
    for(size_t i = 0; i < G_N_ELEMENTS(pipes); i++)
    {
      dt_dev_pixelpipe_t *pipe = pipes[i];

      // We recompute if history hash changed or ROI has changed.
      // If we know history changed, ensure at least the last step is resynced.
      if(dt_dev_pixelpipe_get_history_hash(pipe) != dt_dev_get_history_hash(dev))
        dt_dev_pixelpipe_or_changed(pipe, DT_DEV_PIPE_TOP_CHANGED);

      // pipe->changed is set by zoom/pan otherwise.
      // So at this point, if nothing changed, skip.
      gboolean needs_update = (dt_dev_pixelpipe_get_changed(pipe) != DT_DEV_PIPE_UNCHANGED) && !pipe->pause;
      if(!needs_update) continue;

      // This is cheap to run, keep it in sync always.
      dt_dev_pixelpipe_set_input(pipe, dev, dev->image_storage.id, dev->roi.raw_width, dev->roi.raw_height,
                                 DT_MIPMAP_FULL);

      float scale = 1.f;
      int x = 0, y = 0, wd = 0, ht = 0;

      // Re-entries are designed for raster masks when we lose their reference, so we rerun the full
      // pipeline immediately but cap retries to avoid infinite loops.
      int reentries = 0;
      dt_dev_pixelpipe_reset_reentry(pipe);

      int runs = 0;

      // Updating loop: run while output is invalid/unavailable, or while realtime mode
      // has a new history state to consume.
      while(!dev->exit && dt_control_running() && reentries < 2 && runs < 2 && needs_update)
      {
        dt_print(DT_DEBUG_PIPE | DT_DEBUG_DEV, "PIPE %s needs update\n", pipe->type == DT_DEV_PIXELPIPE_FULL ? "full" : "preview");

        dt_pthread_mutex_lock(&pipe->busy_mutex);
        pipe->processing = 1;

        // We are starting fresh, reset the killswitch signal.
        dt_atomic_set_int(&pipe->shutdown, FALSE);

        // In case of re-entry, we will rerun the whole pipe, so we need
        // to resynch it in full too before.
        // Need to be before dt_dev_pixelpipe_change()
        if(dt_dev_pixelpipe_has_reentry(pipe))
        {
          dt_dev_pixelpipe_or_changed(pipe, DT_DEV_PIPE_REMOVE);
          dt_dev_pixelpipe_cache_flush(darktable.pixelpipe_cache, pipe->type);
        }

        // Resynch history with pipeline. NB: this locks dev->history_mutex.
        while(dt_dev_pixelpipe_get_changed(pipe) != DT_DEV_PIPE_UNCHANGED)
          dt_dev_pixelpipe_change(pipe, dev);

        // If user zoomed/panned in darkroom during the previous loop of recomputation,
        // the kill-switch event was sent, which terminated the pipeline before completion in the previous run,
        // but the coordinates of the ROI changed since then, and we will handle the new coordinates right away
        // without restarting the worker. If re-entry was still armed, the object that owned it may have changed
        // hash too, so we hard-reset the flag here.
        if(_update_darkroom_roi(dev, pipe, &x, &y, &wd, &ht, &scale))
          dt_dev_pixelpipe_reset_reentry(pipe);

        // Catch early killswitch. dt_dev_pixelpipe_change() can be lengthy with huge masks stacks.
        if(dt_atomic_get_int(&pipe->shutdown))
        {
          pipe->processing = 0;
          dt_pthread_mutex_unlock(&pipe->busy_mutex);
          break;
        }

        dt_iop_roi_t roi = (dt_iop_roi_t){ x, y, wd, ht, scale };

        dt_control_log_busy_enter();
        dt_control_toast_busy_enter();

        dt_times_t thread_start;
        dt_get_times(&thread_start);
        const gint64 process_start_us = g_get_monotonic_time();

        dev->progress.completed = 0;
        dev->progress.total = 0;
        const int ret = dt_dev_pixelpipe_process(pipe, dev, roi);
        dev->progress.completed = 0;
        dev->progress.total = 0;
        const gint64 process_runtime_us = g_get_monotonic_time() - process_start_us;

        gchar *msg = g_strdup_printf("[dev_process_%s] pipeline processing thread", dt_pixelpipe_get_pipe_name(pipe->type));
        dt_show_times(&thread_start, msg);
        dt_free(msg);

        dt_control_log_busy_leave();
        dt_control_toast_busy_leave();

        // If pipe is flagged for re-entry, we need to restart it right away.
        if(dt_dev_pixelpipe_has_reentry(pipe))
        {
          reentries++;
        }
        else if(!dt_atomic_get_int(&pipe->shutdown))
        {
          runs++;
        }

        pipe->processing = 0;
        dt_pthread_mutex_unlock(&pipe->busy_mutex);

        if(!ret && !dt_atomic_get_int(&pipe->shutdown))
        {
          dt_gui_throttle_record_runtime(pipe, process_runtime_us);
          needs_update = FALSE;
        }

        if(pipe->type == DT_DEV_PIXELPIPE_FULL)
        {
          DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED);
          dt_control_queue_redraw_center();
        }
        else if(pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
        {
          DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED);
          dt_control_queue_redraw();
        }

        // Allow some breathing room to the OS and GPU
        dt_iop_nap(10000); // 10 ms
      }
    }

    if(dt_dev_pixelpipe_get_realtime(pipes[0]) || dt_dev_pixelpipe_get_realtime(pipes[1]))
      dt_iop_nap(10000);
    else
      dt_iop_nap(50000);
  }

  for(size_t i = 0; i < G_N_ELEMENTS(pipes); i++)
    pipes[i]->running = 0;
}

static int32_t dt_dev_process_job_run(dt_job_t *job)
{
  dt_develop_t *dev = dt_control_job_get_params(job);
  dt_dev_darkroom_pipeline(dev);
  return 0;
}

dt_job_t *dt_dev_process_job_create(dt_develop_t *dev)
{
  dt_job_t *job = dt_control_job_create(&dt_dev_process_job_run, "develop process image");
  if(!job) return NULL;
  dt_control_job_set_params(job, dev, NULL);
  return job;
}

void dt_dev_start_all_pipelines(dt_develop_t *dev)
{
  if(dev->pipelines_started) return;
  dt_control_add_job_res(darktable.control, dt_dev_process_job_create(dev), DT_CTL_WORKER_DARKROOM);
  dev->pipelines_started = TRUE;
}

static gboolean _dt_dev_mipmap_prefetch_full(dt_develop_t *dev, const int32_t imgid)
{
  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_get(darktable.mipmap_cache, &buf, imgid, DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING, 'r');

  const gboolean ok = (buf.buf != NULL) && buf.width != 0 && buf.height != 0;

  if(dev->gui_attached)
  {
    dev->roi.raw_height = buf.height;
    dev->roi.raw_width = buf.width;
    dev->roi.raw_inited = TRUE;
  }

  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);

  return ok;
}

static gboolean _dt_dev_refresh_image_storage(dt_develop_t *dev, const int32_t imgid)
{
  const dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  if(!image) return FALSE;
  dev->image_storage = *image;
  dt_image_cache_read_release(darktable.image_cache, image);
  dt_iop_buffer_dsc_update_bpp(&dev->image_storage.dsc);
  return TRUE;
}

dt_dev_image_storage_t dt_dev_ensure_image_storage(dt_develop_t *dev, const int32_t imgid)
{
  if(!dev || imgid <= 0 || !darktable.image_cache)
    return DT_DEV_IMAGE_STORAGE_DB_NOT_READ;

  if(!_dt_dev_mipmap_prefetch_full(dev, imgid))
    return DT_DEV_IMAGE_STORAGE_MIPMAP_NOT_FOUND;

  if(!_dt_dev_refresh_image_storage(dev, imgid)) 
    return DT_DEV_IMAGE_STORAGE_DB_NOT_READ;

  return DT_DEV_IMAGE_STORAGE_OK;
}

// load the raw and get the new image struct, blocking in gui thread
static inline dt_dev_image_storage_t _dt_dev_load_raw(dt_develop_t *dev, const int32_t imgid)
{
  // then load the raw
  dt_times_t start;
  dt_get_times(&start);

  // Test we got images. Also that populates the cache for later.
  // Refresh our private copy in case raw loading updated image metadata
  const dt_dev_image_storage_t storage_status = dt_dev_ensure_image_storage(dev, imgid);
  if(storage_status) 
    return storage_status;
  
  dt_show_times_f(&start, "[dev_pixelpipe]", "to load the image.");

  return storage_status;
}

// return the zoom scale to fit into the viewport
float dt_dev_get_zoom_scale(const dt_develop_t *dev, const gboolean preview)
{
  const float w = preview ? dev->roi.processed_width : dev->pipe->processed_width;
  const float h = preview ? dev->roi.processed_height : dev->pipe->processed_height;
  return fminf(dev->roi.width / w, dev->roi.height / h);
}

dt_dev_image_storage_t dt_dev_load_image(dt_develop_t *dev, const int32_t imgid)
{
  const dt_dev_image_storage_t ret = _dt_dev_load_raw(dev, imgid);
  if(ret) return ret;

  // we need a global lock as the dev->iop set must not be changed until read history is terminated
  dt_pthread_rwlock_wrlock(&dev->history_mutex);

  const gboolean first_run = dt_dev_read_history_ext(dev, imgid);

  if(first_run && dev == darktable.develop)
  {
    // Resync our private copy of image image with DB,
    // mostly for DT_IMAGE_AUTO_PRESETS_APPLIED flag.
    dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'w');
    if(image)
    {
      *image = dev->image_storage;
      dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_SAFE);
    }

    dt_dev_write_history_ext(dev, imgid);
  }

  dt_pthread_rwlock_unlock(&dev->history_mutex);

  if(first_run && dev == darktable.develop)
  {
    dt_dev_append_changed_tag(imgid);
    dt_dev_history_notify_change(dev, imgid);
  }

  return ret;
}

void dt_dev_configure_real(dt_develop_t *dev, int wd, int ht)
{
  // Called only from Darkroom to convert the widget allocation into the
  // raster ROI contract consumed by the pipeline. Everything stored in
  // dev->roi below is expressed in real buffer pixels.
  const dt_iop_roi_t gui_roi = { .x = 0, .y = 0, .width = wd, .height = ht, .scale = 1.0f };
  dt_iop_roi_t pipe_roi = { 0 };
  dt_dev_convert_roi(dev, &gui_roi, &pipe_roi, DT_DEV_ROI_GUI_LOGICAL, DT_DEV_ROI_PIPELINE);
  dev->roi.width = pipe_roi.width;
  dev->roi.height = pipe_roi.height;
  dev->roi.gui_inited = TRUE;

  dt_print(DT_DEBUG_DEV,
           "[pixelpipe] Darkroom requested a %i×%i px widget -> %i×%i px raster preview\n",
           wd, ht, dev->roi.width, dev->roi.height);

  dt_dev_get_thumbnail_size(dev);
  dt_dev_pixelpipe_update_zoom_main(dev);
  dt_dev_pixelpipe_update_zoom_preview(dev);
  dt_control_queue_redraw();
}

void dt_dev_check_zoom_pos_bounds(dt_develop_t *dev, float *dev_x, float *dev_y, float *box_w, float *box_h)
{
  // for the debug strings lower
  //float old_x = *dev_x;
  //float old_y = *dev_y;
  int proc_w = 0;
  int proc_h = 0;
  dt_dev_get_processed_size(dev, &proc_w, &proc_h);
  const float scale = dt_dev_get_zoom_level(dev);

  // find the box size
  const float bw = dev->roi.width / (proc_w * scale);
  const float bh = dev->roi.height / (proc_h * scale);

  // calculate half-dimensions once
  const float half_bw = bw * 0.5f;
  const float half_bh = bh * 0.5f;

  // clamp position using pre-calculated values
  *dev_x = (bw > 1.0f || dev->roi.scaling <= 1.0f) ? 0.5f : CLAMPF(*dev_x, half_bw, 1.0f - half_bw);
  *dev_y = (bh > 1.0f || dev->roi.scaling <= 1.0f) ? 0.5f : CLAMPF(*dev_y, half_bh, 1.0f - half_bh);
  // return box size
  if(box_w) *box_w = bw;
  if(box_h) *box_h = bh;

  /*
  fprintf(stdout, "BOUNDS: box size: %2.2f x %2.2f\n", bw, bh);
  fprintf(stdout, "BOUNDS: half box size: %2.2f x %2.2f\n", half_bw, half_bh);
  fprintf(stdout, "BOUNDS: X pos: %2.2f -> %2.2f [%2.2f %2.2f]\n",
    old_x, *dev_x, half_bw, 1.0f - half_bw);
  fprintf(stdout, "BOUNDS: Y pos: %2.2f -> %2.2f [%2.2f %2.2f]\n",
    old_y, *dev_y, half_bh, 1.0f - half_bh);
*/
}

void dt_dev_get_processed_size(const dt_develop_t *dev, int *procw, int *proch)
{
  if(!dev) return;
  *procw = dev->roi.processed_width;
  *proch = dev->roi.processed_height;
 }

void dt_dev_coordinates_widget_delta_to_image_delta(dt_develop_t *dev, float *points, size_t num_points)
{
  if(!dev || !points || num_points == 0) return;

  const float scale = dt_dev_get_zoom_level(dev) / darktable.gui->ppd;
  if(scale == 0.0f) return;

  // Widget deltas are measured in Gtk logical pixels. Convert them to processed-image
  // pixels here so dragging thresholds and keyboard pans share the same zoom math.
  for(size_t i = 0; i < num_points; ++i)
  {
    const size_t idx = i * 2;
    points[idx + 0] /= scale;
    points[idx + 1] /= scale;
  }
}

void dt_dev_coordinates_widget_to_image_norm(dt_develop_t *dev, float *points, size_t num_points)
{
  if(!dev || !points || num_points == 0) return;
  const float processed_width = dev->roi.processed_width;
  const float processed_height = dev->roi.processed_height;
  if(processed_width == 0.0f || processed_height == 0.0f) return;

  // Widget events are expressed in GUI logical coordinates, while the pipeline
  // zoom lives in raster pixels. Convert back to the same GUI-space zoom used
  // by dt_dev_rescale_roi() so event hit-testing and overlay drawing stay aligned.
  const float scale = dt_dev_get_zoom_level(dev) / darktable.gui->ppd;
  const float roi_x = (float)dev->roi.x;
  const float roi_y = (float)dev->roi.y;
  const float center_x = 0.5f * (float)dev->roi.orig_width;
  const float center_y = 0.5f * (float)dev->roi.orig_height;
  const float inv_scaled_width = 1.0f / (processed_width * scale);
  const float inv_scaled_height = 1.0f / (processed_height * scale);

  for(size_t i = 0; i < num_points; ++i)
  {
    const size_t idx = i * 2;
    const float px = points[idx + 0];
    const float py = points[idx + 1];
    points[idx + 0] = roi_x + (px - center_x) * inv_scaled_width;
    points[idx + 1] = roi_y + (py - center_y) * inv_scaled_height;
  }
}

void dt_dev_coordinates_image_norm_to_widget(dt_develop_t *dev, float *points, size_t num_points)
{
  if(!dev || !points || num_points == 0) return;
  const float processed_width = dev->roi.processed_width;
  const float processed_height = dev->roi.processed_height;
  if(processed_width == 0.0f || processed_height == 0.0f) return;

  // GUI overlays are drawn in logical widget coordinates, so use the same
  // GUI-space zoom that the Cairo darkroom transform applies.
  const float scale = dt_dev_get_zoom_level(dev) / darktable.gui->ppd;
  const float roi_x = (float)dev->roi.x;
  const float roi_y = (float)dev->roi.y;
  const float scaled_width = processed_width * scale;
  const float scaled_height = processed_height * scale;
  const float center_x = 0.5f * (float)dev->roi.orig_width;
  const float center_y = 0.5f * (float)dev->roi.orig_height;

  for(size_t i = 0; i < num_points; ++i)
  {
    const size_t idx = i * 2;
    const float px = points[idx + 0];
    const float py = points[idx + 1];
    const float dx = (px - roi_x) * scaled_width;
    const float dy = (py - roi_y) * scaled_height;
    points[idx + 0] = dx + center_x;
    points[idx + 1] = dy + center_y;
  }
}

void dt_dev_coordinates_image_norm_to_image_abs(dt_develop_t *dev, float *points, size_t num_points)
{
  if(!dev || !points || num_points == 0) return;
  const float processed_width = dev->roi.processed_width;
  const float processed_height = dev->roi.processed_height;
  if(processed_width == 0.0f || processed_height == 0.0f) return;

  for(size_t i = 0; i < num_points; ++i)
  {
    const size_t idx = i * 2;
    points[idx + 0] *= processed_width;
    points[idx + 1] *= processed_height;
  }
}

void dt_dev_coordinates_image_abs_to_image_norm(dt_develop_t *dev, float *points, size_t num_points)
{
  if(!dev || !points || num_points == 0) return;
  const float processed_width = dev->roi.processed_width;
  const float processed_height = dev->roi.processed_height;
  if(processed_width == 0.0f || processed_height == 0.0f) return;

  const float inv_width = 1.0f / processed_width;
  const float inv_height = 1.0f / processed_height;
  for(size_t i = 0; i < num_points; ++i)
  {
    const size_t idx = i * 2;
    points[idx + 0] *= inv_width;
    points[idx + 1] *= inv_height;
  }
}

void dt_dev_coordinates_raw_abs_to_raw_norm(dt_develop_t *dev, float *points, size_t num_points)
{
  if(!dev || !points || num_points == 0) return;
  const float raw_width = dev->roi.raw_width;
  const float raw_height = dev->roi.raw_height;
  if(raw_width == 0.0f || raw_height == 0.0f) return;
  
  const float inv_width = 1.f / raw_width;
  const float inv_height = 1.f / raw_height;
  for(size_t i = 0; i < num_points; i++)
  {
    const size_t idx = i * 2;
    points[idx + 0] *= inv_width;
    points[idx + 1] *= inv_height;
  }
}

void dt_dev_coordinates_raw_norm_to_raw_abs(dt_develop_t *dev, float *points, size_t num_points)
{
  if(!dev || !points || num_points == 0) return;
  const float raw_width = dev->roi.raw_width;
  const float raw_height = dev->roi.raw_height;
  if(raw_width == 0.0f || raw_height == 0.0f) return;
  
  for(size_t i = 0; i < num_points; i++)
  {
    const size_t idx = i * 2;
    points[idx + 0] *= raw_width;
    points[idx + 1] *= raw_height;
  }
}

void dt_dev_coordinates_image_abs_to_raw_norm(dt_develop_t *dev, float *points, size_t num_points)
{
  dt_dev_coordinates_image_abs_to_raw_abs(dev, points, num_points);
  dt_dev_coordinates_raw_abs_to_raw_norm(dev, points, num_points);
}

void dt_dev_coordinates_image_norm_to_preview_abs(dt_develop_t *dev, float *points, size_t num_points)
{
  if(!dev || !points || num_points == 0) return;
  const float preview_width = dev->roi.preview_width;
  const float preview_height = dev->roi.preview_height;
  if(preview_width == 0.0f || preview_height == 0.0f) return;
  
  for(size_t i = 0; i < num_points; i++)
  {
    const size_t idx = i * 2;
    points[idx + 0] *= preview_width;
    points[idx + 1] *= preview_height;
  }
}

void dt_dev_coordinates_preview_abs_to_image_norm(dt_develop_t *dev, float *points, size_t num_points)
{
  if(!dev || !points || num_points == 0) return;
  const float preview_width = dev->roi.preview_width;
  const float preview_height = dev->roi.preview_height;
  if(preview_width == 0.0f || preview_height == 0.0f) return;

  const float inv_width = 1.f / preview_width;
  const float inv_height = 1.f / preview_height;
  for(size_t i = 0; i < num_points; i++)
  {
    const size_t idx = i * 2;
    points[idx + 0] *= inv_width;
    points[idx + 1] *= inv_height;
  }
}

int dt_dev_is_current_image(dt_develop_t *dev, int32_t imgid)
{
  return (dev->image_storage.id == imgid) ? 1 : 0;
}

void dt_dev_modulegroups_switch(dt_develop_t *dev, dt_iop_module_t *module)
{
  if(!dev || !module) return;
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_MODULEGROUPS_SET, module);
}

void dt_dev_modulegroups_update_visibility(dt_develop_t *dev)
{
  if(!dev) return;
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_MODULEGROUPS_SET, NULL);
}

void dt_dev_masks_list_change(dt_develop_t *dev)
{
  if(dev->proxy.masks.module && dev->proxy.masks.list_change)
    dev->proxy.masks.list_change(dev->proxy.masks.module);
}
void dt_dev_masks_list_update(dt_develop_t *dev)
{
  if(dev->proxy.masks.module && dev->proxy.masks.list_update)
    dev->proxy.masks.list_update(dev->proxy.masks.module);
}
void dt_dev_masks_list_remove(dt_develop_t *dev, int formid, int parentid)
{
  if(dev->proxy.masks.module && dev->proxy.masks.list_remove)
    dev->proxy.masks.list_remove(dev->proxy.masks.module, formid, parentid);
}
void dt_dev_masks_selection_change(dt_develop_t *dev, struct dt_iop_module_t *module,
                                   const int selectid, const int throw_event)
{
  if(dev->proxy.masks.module && dev->proxy.masks.selection_change)
    dev->proxy.masks.selection_change(dev->proxy.masks.module, module, selectid, throw_event);
}

void dt_dev_snapshot_request(dt_develop_t *dev, const char *filename)
{
  dev->proxy.snapshot.filename = filename;
  dev->proxy.snapshot.request = TRUE;
  dt_control_queue_redraw_center();
}

/** duplicate a existent module */
dt_iop_module_t *dt_dev_module_duplicate(dt_develop_t *dev, dt_iop_module_t *base)
{
  // we create the new module
  dt_iop_module_t *module = (dt_iop_module_t *)calloc(1, sizeof(dt_iop_module_t));
  if(dt_iop_load_module(module, base->so, base->dev)) return NULL;
  module->instance = base->instance;

  // we set the multi-instance priority and the iop order
  int pmax = 0;
  for(GList *modules = base->dev->iop; modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    if(mod->instance == base->instance)
    {
      if(pmax < mod->multi_priority) pmax = mod->multi_priority;
    }
  }
  // create a unique multi-priority
  pmax += 1;
  dt_iop_update_multi_priority(module, pmax);

  // add this new module position into the iop-order-list
  dt_ioppr_insert_module_instance(dev, module);

  // since we do not rename the module we need to check that an old module does not have the same name. Indeed
  // the multi_priority
  // are always rebased to start from 0, to it may be the case that the same multi_name be generated when
  // duplicating a module.
  int pname = module->multi_priority;
  char mname[128];

  do
  {
    snprintf(mname, sizeof(mname), "%d", pname);
    gboolean dup = FALSE;

    for(GList *modules = base->dev->iop; modules; modules = g_list_next(modules))
    {
      dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
      if(mod->instance == base->instance)
      {
        if(strcmp(mname, mod->multi_name) == 0)
        {
          dup = TRUE;
          break;
        }
      }
    }

    if(dup)
      pname++;
    else
      break;
  } while(1);

  // the multi instance name
  g_strlcpy(module->multi_name, mname, sizeof(module->multi_name));
  // we insert this module into dev->iop
  base->dev->iop = g_list_insert_sorted(base->dev->iop, module, dt_sort_iop_by_order);

  // always place the new instance after the base one
  if(!dt_ioppr_move_iop_after(base->dev, module, base))
  {
    fprintf(stderr, "[dt_dev_module_duplicate] can't move new instance after the base one\n");
  }

  // that's all. rest of insertion is gui work !
  return module;
}



void dt_dev_module_remove(dt_develop_t *dev, dt_iop_module_t *module)
{
  // if(darktable.gui->reset) return;
  int del = 0;

  if(dev->gui_attached)
  {
    dt_pthread_rwlock_wrlock(&dev->history_mutex);
    dt_dev_history_undo_start_record_locked(dev);

    const int history_end = dt_dev_get_history_end_ext(dev);
    int removed_before_end = 0;
    int history_pos = 0;
    GList *elem = dev->history;
    while(elem != NULL)
    {
      GList *next = g_list_next(elem);
      dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(elem->data);

      if(module == hist->module)
      {
        dt_print(DT_DEBUG_HISTORY, "[dt_module_remode] removing obsoleted history item: %s %s %p %p\n",
                 hist->module->op, hist->module->multi_name, module, hist->module);
        dt_dev_free_history_item(hist);
        dev->history = g_list_delete_link(dev->history, elem);
        if(history_pos < history_end) removed_before_end++;
        del = 1;
      }
      history_pos++;
      elem = next;
    }

    if(removed_before_end > 0)
      dt_dev_set_history_end_ext(dev, MAX(0, history_end - removed_before_end));

    dt_dev_history_undo_end_record_locked(dev);
    dt_pthread_rwlock_unlock(&dev->history_mutex);
    if(del) dt_dev_history_undo_invalidate_module(module);
  }

  // and we remove it from the list
  for(GList *modules = dev->iop; modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    if(mod == module)
    {
      dev->iop = g_list_remove_link(dev->iop, modules);
      break;
    }
  }
}

typedef struct dt_dev_multishow_state_t
{
  GHashTable *instance_counts;
  GHashTable *modules_in_history;
  GHashTable *prev_visible;
  GHashTable *next_visible;
} dt_dev_multishow_state_t;

void _dev_module_update_multishow(dt_develop_t *dev, struct dt_iop_module_t *module,
                                  const dt_dev_multishow_state_t *state)
{
  const int nb_instances = GPOINTER_TO_INT(
      g_hash_table_lookup(state->instance_counts, GINT_TO_POINTER(module->instance)));

  dt_iop_module_t *mod_prev = (dt_iop_module_t *)g_hash_table_lookup(state->prev_visible, module);
  dt_iop_module_t *mod_next = (dt_iop_module_t *)g_hash_table_lookup(state->next_visible, module);

  const gboolean move_next = (mod_next && mod_next->iop_order != INT_MAX)
                                 ? dt_ioppr_check_can_move_after_iop(dev->iop, module, mod_next)
                                 : -1.0;
  const gboolean move_prev = (mod_prev && mod_prev->iop_order != INT_MAX)
                                 ? dt_ioppr_check_can_move_before_iop(dev->iop, module, mod_prev)
                                 : -1.0;

  module->multi_show_new = !(module->flags() & IOP_FLAGS_ONE_INSTANCE);
  // Never allow deleting the base instance (multi_priority == 0) nor modules limited to one instance.
  module->multi_show_close =
      (nb_instances > 1 && module->multi_priority > 0 && !(module->flags() & IOP_FLAGS_ONE_INSTANCE));
  if(mod_next)
    module->multi_show_up = move_next;
  else
    module->multi_show_up = 0;
  if(mod_prev)
    module->multi_show_down = move_prev;
  else
    module->multi_show_down = 0;

  // If it's an additional instance supposed to be added by an history item after
  // the current history_end cursor, conceptually it doesn't exist yet,
  // even though it's dangling there on the pipe. So hide it from GUI.
  if(nb_instances > 1
     && module->multi_priority > 0
     && !g_hash_table_contains(state->modules_in_history, module))
    gtk_widget_hide(module->expander);
}

void dt_dev_modules_update_multishow(dt_develop_t *dev)
{
  dt_ioppr_check_iop_order(dev, 0, "dt_dev_modules_update_multishow");
  const int history_end = dt_dev_get_history_end_ext(dev);

  dt_dev_multishow_state_t state = { 0 };
  state.instance_counts = g_hash_table_new(g_direct_hash, g_direct_equal);
  state.modules_in_history = g_hash_table_new(g_direct_hash, g_direct_equal);
  state.prev_visible = g_hash_table_new(g_direct_hash, g_direct_equal);
  state.next_visible = g_hash_table_new(g_direct_hash, g_direct_equal);

  // Precompute how many instances exist for each base module.
  for(GList *modules = dev->iop; modules; modules = g_list_next(modules))
  {
    const dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    gpointer key = GINT_TO_POINTER(mod->instance);
    int count = GPOINTER_TO_INT(g_hash_table_lookup(state.instance_counts, key));
    g_hash_table_replace(state.instance_counts, key, GINT_TO_POINTER(count + 1));
  }

  // Precompute which modules exist in history up to history_end.
  int history_pos = 0;
  for(GList *history = g_list_first(dev->history);
      history && history_pos < history_end;
      history = g_list_next(history), history_pos++)
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)history->data;
    if(hist->module) g_hash_table_add(state.modules_in_history, hist->module);
  }

  // Precompute previous visible module in pipeline order.
  dt_iop_module_t *last_visible = NULL;
  for(GList *modules = g_list_first(dev->iop); modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    if(dt_iop_gui_module_is_visible(mod))
    {
      g_hash_table_insert(state.prev_visible, mod, last_visible);
      last_visible = mod;
    }
  }

  // Precompute next visible module in GUI order (reverse pipeline).
  last_visible = NULL;
  for(GList *modules = g_list_last(dev->iop); modules; modules = g_list_previous(modules))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    if(dt_iop_gui_module_is_visible(mod))
    {
      g_hash_table_insert(state.next_visible, mod, last_visible);
      last_visible = mod;
    }
  }

  for(GList *modules = dev->iop; modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;

    if(dt_iop_gui_module_is_visible(mod))
      _dev_module_update_multishow(dev, mod, &state);
  }

  g_hash_table_destroy(state.instance_counts);
  g_hash_table_destroy(state.modules_in_history);
  g_hash_table_destroy(state.prev_visible);
  g_hash_table_destroy(state.next_visible);
}

gchar *dt_history_item_get_label(const struct dt_iop_module_t *module)
{
  gchar *label;
  /* create a history button and add to box */
  if(!module->multi_name[0] || strcmp(module->multi_name, "0") == 0)
    label = g_strdup(module->name());
  else
  {
    label = g_strdup_printf("%s %s", module->name(), module->multi_name);
  }
  return label;
}

gchar *dt_dev_get_multi_name(const struct dt_iop_module_t *module)
{
  gboolean has_multi_name = g_strcmp0(module->multi_name, "0") != 0 && g_strcmp0(module->multi_name, "") != 0;
  gchar *label = has_multi_name ? g_strdup(module->multi_name) : g_strdup("");

  return label;
}

gchar *dt_dev_get_masks_group_name(const struct dt_iop_module_t *module)
{
  gchar *module_multi_name = dt_dev_get_multi_name(module);
  gboolean has_multi_name = g_strcmp0(module_multi_name, "") != 0;
  // fallback to module name if no multi name
  gchar *module_label = has_multi_name ?  g_strdup(module_multi_name) : dt_history_item_get_name(module);
  gchar *group_name = g_strdup_printf("Mask %s", module_label);
  dt_free(module_label);
  dt_free(module_multi_name);

  return group_name;
}


gchar *dt_history_item_get_name(const struct dt_iop_module_t *module)
{
  gchar *label;
  /* create a history button and add to box */
  if(!module->multi_name[0] || strcmp(module->multi_name, "0") == 0)
    label = delete_underscore(module->name());
  else
  {
    gchar *clean_name = delete_underscore(module->name());
    label = g_strdup_printf("%s %s", clean_name, module->multi_name);
    dt_free(clean_name);
  }
  return label;
}

gchar *dt_history_item_get_name_html(const struct dt_iop_module_t *module)
{
  gchar *clean_name = delete_underscore(module->name());
  gchar *label;
  /* create a history button and add to box */
  if(!module->multi_name[0] || strcmp(module->multi_name, "0") == 0)
    label = g_markup_escape_text(clean_name, -1);
  else
    label = g_markup_printf_escaped("%s <span size=\"smaller\">%s</span>", clean_name, module->multi_name);
  dt_free(clean_name);
  return label;
}

static int dt_dev_distort_backtransform_locked(dt_develop_t *dev, dt_dev_pixelpipe_t *pipe, const double iop_order,
                                               const int transf_direction, float *points, size_t points_count);

int dt_dev_coordinates_raw_abs_to_image_abs(dt_develop_t *dev, float *points, size_t points_count)
{
  return dt_dev_distort_transform_plus(dev, dev->virtual_pipe, 0.0f, DT_DEV_TRANSFORM_DIR_ALL, points, points_count);
}

int dt_dev_coordinates_image_abs_to_raw_abs(dt_develop_t *dev, float *points, size_t points_count)
{
  return dt_dev_distort_backtransform_locked(dev, dev->virtual_pipe, 0.0f, DT_DEV_TRANSFORM_DIR_ALL, points, points_count);
}

// only call directly or indirectly from dt_dev_distort_transform_plus, so that it runs with the history locked
int dt_dev_distort_transform_locked(dt_develop_t *dev, const dt_dev_pixelpipe_t *pipe, const double iop_order,
                                    const int transf_direction, float *points, size_t points_count)
{
  for(GList *pieces = g_list_first(pipe->nodes); pieces; pieces = g_list_next(pieces))
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)(pieces->data);
    dt_iop_module_t *module = piece->module;
    if(piece->enabled
       && ((transf_direction == DT_DEV_TRANSFORM_DIR_ALL)
           || (transf_direction == DT_DEV_TRANSFORM_DIR_FORW_INCL && module->iop_order >= iop_order)
           || (transf_direction == DT_DEV_TRANSFORM_DIR_FORW_EXCL && module->iop_order > iop_order)
           || (transf_direction == DT_DEV_TRANSFORM_DIR_BACK_INCL && module->iop_order <= iop_order)
           || (transf_direction == DT_DEV_TRANSFORM_DIR_BACK_EXCL && module->iop_order < iop_order))
       && !dt_dev_pixelpipe_activemodule_disables_currentmodule(dev, module))
    {
      module->distort_transform(module, pipe, piece, points, points_count);
    }
  }
  return 1;
}

int dt_dev_distort_transform_plus(dt_develop_t *dev, const dt_dev_pixelpipe_t *pipe, const double iop_order, const int transf_direction,
                                  float *points, size_t points_count)
{
  dt_dev_distort_transform_locked(dev, pipe, iop_order, transf_direction, points, points_count);
  return 1;
}

// Internal backtransform loop. Keep this file-local so callers use the public wrappers.
static int dt_dev_distort_backtransform_locked(dt_develop_t *dev, dt_dev_pixelpipe_t *pipe, const double iop_order,
                                               const int transf_direction, float *points, size_t points_count)
{
  for(GList *pieces = g_list_last(pipe->nodes); pieces; pieces = g_list_previous(pieces))
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)(pieces->data);
    dt_iop_module_t *module = piece->module;
    if(piece->enabled
       && ((transf_direction == DT_DEV_TRANSFORM_DIR_ALL)
           || (transf_direction == DT_DEV_TRANSFORM_DIR_FORW_INCL && module->iop_order >= iop_order)
           || (transf_direction == DT_DEV_TRANSFORM_DIR_FORW_EXCL && module->iop_order > iop_order)
           || (transf_direction == DT_DEV_TRANSFORM_DIR_BACK_INCL && module->iop_order <= iop_order)
           || (transf_direction == DT_DEV_TRANSFORM_DIR_BACK_EXCL && module->iop_order < iop_order))
       && !dt_dev_pixelpipe_activemodule_disables_currentmodule(dev, module))
    {
      module->distort_backtransform(module, pipe, piece, points, points_count);
    }
  }
  return 1;
}

int dt_dev_distort_backtransform_plus(dt_develop_t *dev, dt_dev_pixelpipe_t *pipe, const double iop_order, const int transf_direction,
                                      float *points, size_t points_count)
{
  const int success = dt_dev_distort_backtransform_locked(dev, pipe, iop_order, transf_direction, points, points_count);
  return success;
}

dt_dev_pixelpipe_iop_t *dt_dev_distort_get_iop_pipe(dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe,
                                                    struct dt_iop_module_t *module)
{
  for(const GList *pieces = g_list_last(pipe->nodes); pieces; pieces = g_list_previous(pieces))
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)(pieces->data);
    if(piece->module == module)
    {
      return piece;
    }
  }
  return NULL;
}

// set the module list order
void dt_dev_reorder_gui_module_list(dt_develop_t *dev)
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

void dt_dev_undo_start_record(dt_develop_t *dev)
{
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);

  /* record current history state : before change (needed for undo) */
  if(dev->gui_attached && cv->view((dt_view_t *)cv) == DT_VIEW_DARKROOM)
  {
    dt_dev_history_undo_start_record(dev);
  }
}

void dt_dev_undo_end_record(dt_develop_t *dev)
{
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);

  /* record current history state : after change (needed for undo) */
  if(dev->gui_attached && cv->view((dt_view_t *)cv) == DT_VIEW_DARKROOM)
  {
    dt_dev_history_undo_end_record(dev);
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_CHANGE);
  }
}

gboolean dt_masks_get_lock_mode(dt_develop_t *dev)
{
  if(dev->gui_attached)
  {
    dt_pthread_mutex_lock(&darktable.gui->mutex);
    const gboolean state = dev->mask_lock;
    dt_pthread_mutex_unlock(&darktable.gui->mutex);
    return state;
  }
  return FALSE;
}

void dt_masks_set_lock_mode(dt_develop_t *dev, gboolean mode)
{
  if(dev->gui_attached)
  {
    dt_pthread_mutex_lock(&darktable.gui->mutex);
    dev->mask_lock = mode;
    dt_pthread_mutex_unlock(&darktable.gui->mutex);
  }
}

int32_t dt_dev_get_history_end_ext(dt_develop_t *dev)
{
  const int num_items = g_list_length(dev->history);
  return CLAMP(dev->history_end, 0, num_items);
}

void dt_dev_set_history_end_ext(dt_develop_t *dev, const uint32_t index)
{
  const int num_items = g_list_length(dev->history);
  dev->history_end = CLAMP(index, 0, num_items);
  dt_dev_set_history_hash(dev, dt_dev_history_compute_hash(dev));
}

void dt_dev_append_changed_tag(const int32_t imgid)
{
  /* attach changed tag reflecting actual change */
  guint tagid = 0;
  dt_tag_new("darktable|changed", &tagid);
  const gboolean tag_change = dt_tag_attach(tagid, imgid, FALSE, FALSE);
  if(tag_change) DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_TAG_CHANGED);
}

void dt_dev_masks_update_hash(dt_develop_t *dev)
{
  uint64_t hash = 5381;
  for(GList *form = g_list_first(dev->forms); form; form = g_list_next(form))
  {
    dt_masks_form_t *shape = (dt_masks_form_t *)form->data;
    hash = dt_masks_group_get_hash(hash, shape);
  }

  // Keep on accumulating "changed" states until something saves the new stack
  // and resets that to 0
  uint64_t old_hash = dev->forms_hash;
  dev->forms_changed |= (old_hash != hash);
  dev->forms_hash = hash;
}

float dt_dev_get_natural_scale(dt_develop_t *dev)
{
  if(!dev->roi.gui_inited || !dev->roi.raw_inited) return -1.f;

  return fminf(fminf((float)dev->roi.width / (float)dev->roi.processed_width,
                      (float)dev->roi.height / (float)dev->roi.processed_height),
                1.f);
}

float dt_dev_get_fit_scale(dt_develop_t *dev)
{
  if(!dev) return 1.0f;
  return dev->roi.scaling / darktable.gui->ppd;
}

float dt_dev_get_overlay_scale(dt_develop_t *dev)
{
  return dt_dev_get_fit_scale(dev);
}

float dt_dev_get_widget_zoom_scale(const dt_develop_t *dev, const float scaling)
{
  if(!dev) return 1.0f;
  return scaling * dev->roi.natural_scale / darktable.gui->ppd;
}

void dt_dev_get_widget_center(const dt_develop_t *dev, float *point)
{
  if(!dev || !point) return;
  point[0] = 0.5f * dev->roi.orig_width;
  point[1] = 0.5f * dev->roi.orig_height;
}

void dt_dev_get_image_box_in_widget(const dt_develop_t *dev, const int32_t width, const int32_t height, float *box)
{
  if(!dev || !box) return;

  const float scale = dev->roi.scaling / darktable.gui->ppd;
  const float roi_width = fminf(width, dev->roi.preview_width * scale);
  const float roi_height = fminf(height, dev->roi.preview_height * scale);
  const float border = dev->roi.border_size;

  box[0] = fmaxf(border, 0.5f * (width - roi_width));
  box[1] = fmaxf(border, 0.5f * (height - roi_height));
  box[2] = fminf(width - 2 * border, roi_width);
  box[3] = fminf(height - 2 * border, roi_height);
}

float dt_dev_get_zoom_level(const dt_develop_t *dev)
{
  if(!dev) return 1.f;
  return dev->roi.scaling * dev->roi.natural_scale;
}

void dt_dev_reset_roi(dt_develop_t *dev)
{
  dev->roi.natural_scale = -1.f;
  dev->roi.scaling = 1.f;
  dev->roi.x = 0.5f;
  dev->roi.y = 0.5f;
}

void dt_dev_convert_roi(const dt_develop_t *dev, const dt_iop_roi_t *roi_in, dt_iop_roi_t *roi_out,
                        const dt_dev_roi_space_t from, const dt_dev_roi_space_t to)
{
  if(!dev || !roi_in || !roi_out) return;

  *roi_out = *roi_in;
  if(from == to) return;

  const float factor = (from == DT_DEV_ROI_GUI_LOGICAL && to == DT_DEV_ROI_PIPELINE)
                           ? darktable.gui->ppd
                           : 1.0f / darktable.gui->ppd;

  // x/y/width/height belong to the GUI/pipeline geometry boundary and therefore
  // follow the ppd factor. roi->scale stays unchanged because it expresses the
  // image-space sampling ratio, which must not depend on GUI density.
  roi_out->x = lroundf(roi_in->x * factor);
  roi_out->y = lroundf(roi_in->y * factor);
  roi_out->width = lroundf(roi_in->width * factor);
  roi_out->height = lroundf(roi_in->height * factor);
  roi_out->scale = roi_in->scale * factor;
}

gboolean dt_dev_clip_roi(dt_develop_t *dev, cairo_t *cr, int32_t width, int32_t height)
{
  // DO NOT MODIFIY !! //

  const float wd = dev->roi.preview_width;
  const float ht = dev->roi.preview_height;
  if(wd == 0.f || ht == 0.f) return TRUE;

  const float zoom_scale = dt_dev_get_overlay_scale(dev);
  const int32_t border = dev->roi.border_size;
  const float roi_width = fminf(width, wd * zoom_scale);
  const float roi_height = fminf(height, ht * zoom_scale);

  const float rec_x = fmaxf(border, (width - roi_width) * 0.5f);
  const float rec_y = fmaxf(border, (height - roi_height) * 0.5f);
  const float rec_w = fminf(width - 2 * border, roi_width);
  const float rec_h = fminf(height - 2 * border, roi_height);

  cairo_rectangle(cr, rec_x, rec_y, rec_w, rec_h);
  cairo_clip(cr);

  return FALSE;
}

static gboolean _dev_translate_roi(dt_develop_t *dev, cairo_t *cr, int32_t width, int32_t height)
{
  // DO NOT MODIFIY !! //
  // used by preview image scalling, guide and modules //
  int proc_wd = 0;
  int proc_ht = 0;
  dt_dev_get_processed_size(dev, &proc_wd, &proc_ht);
  if(proc_wd == 0.f || proc_ht == 0.f) return TRUE;

  // Get image's origin position and scale
  const float zoom_scale = dt_dev_get_zoom_level(dev) / darktable.gui->ppd;
  const float tx = 0.5f * width - dev->roi.x * proc_wd * zoom_scale;
  const float ty = 0.5f * height - dev->roi.y * proc_ht * zoom_scale;

  cairo_translate(cr, tx, ty);
  
  return FALSE;
}

gboolean dt_dev_rescale_roi(dt_develop_t *dev, cairo_t *cr, int32_t width, int32_t height)
{
  if(_dev_translate_roi(dev, cr, width, height))
    return TRUE;
  const float scale = dt_dev_get_fit_scale(dev);
  cairo_scale(cr, scale, scale);
  
  return FALSE;
}

gboolean dt_dev_rescale_roi_to_input(dt_develop_t *dev, cairo_t *cr, int32_t width, int32_t height)
{
  if(_dev_translate_roi(dev, cr, width, height))
    return TRUE;
  const float scale = dt_dev_get_zoom_level(dev) / darktable.gui->ppd;
  cairo_scale(cr, scale, scale);
  
  return FALSE;
}

gboolean dt_dev_check_zoom_scale_bounds(dt_develop_t *dev)
{
  const float natural_scale = dev->roi.natural_scale;

  // Limit zoom in to 16x the size of an apparent pixel on screen
  const float pixel_actual_size = natural_scale * dev->roi.scaling;
  const float pixel_max_size = 16.f;
  
  if(pixel_actual_size >= pixel_max_size)
  {
    // Restore old scaling (caller should handle this)
    dev->roi.scaling = pixel_max_size / natural_scale;
    return TRUE;
  }
  
  // Limit zoom out to 1/3rd of the fit-to-window size
  const float min_scaling = 0.33f;
  if(dev->roi.scaling < min_scaling)
  {
    dev->roi.scaling = min_scaling;
    return TRUE;
  }
  return FALSE;
}

void dt_dev_update_mouse_effect_radius(dt_develop_t *dev)
{
  float zoom_level = dt_dev_get_zoom_level(dev);
  if(zoom_level <= 0.f) zoom_level = 1.0f;

  // Constant 10 device-pixel safety margin for mask selection, independent of zoom and PPD.
  const float radius = DT_PIXEL_APPLY_DPI(15.0f);
  darktable.gui->mouse.effect_radius = radius / zoom_level;
  darktable.gui->mouse.effect_radius_scaled = darktable.gui->mouse.effect_radius * darktable.gui->ppd;

  dt_print(DT_DEBUG_MASKS,
           "[mouse] effect_radius=%0.2f effect_radius_scaled=%0.2f zoom_level=%0.4f ppd=%0.4f\n",
           darktable.gui->mouse.effect_radius, darktable.gui->mouse.effect_radius_scaled,
           zoom_level, darktable.gui->ppd);
}

void dt_dev_set_backbuf(dt_backbuf_t *backbuf, const int width, const int height, const size_t bpp, 
                        const int64_t hash, const int64_t history_hash)
{
  backbuf->height = height;
  backbuf->width = width;
  dt_dev_backbuf_set_hash(backbuf, hash);
  backbuf->bpp = bpp;
  dt_dev_backbuf_set_history_hash(backbuf, history_hash);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
