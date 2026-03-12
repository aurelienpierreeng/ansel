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
#include "gui/gtk.h"
#include "gui/gui_throttle.h"
#include "gui/presets.h"

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
  dev->gui_module = NULL;
  dt_pthread_rwlock_init(&dev->history_mutex, NULL);
  dt_pthread_rwlock_init(&dev->masks_mutex, NULL);
  dev->history = NULL; // empty list

  dev->gui_attached = gui_attached;
  dev->roi.width = -1;
  dev->roi.height = -1;
  dev->image_surface = NULL;

  dt_image_init(&dev->image_storage);
  dev->pipe = dev->preview_pipe = NULL;
  dev->virtual_pipe = NULL;
  dev->histogram_pre_tonecurve = NULL;
  dev->histogram_pre_levels = NULL;
  dev->forms = NULL;
  dev->form_gui = NULL;
  dev->allforms = NULL;

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

  dev->iop = NULL;
  dev->alliop = NULL;
  dev->allprofile_info = NULL;
  dev->iop_order_list = NULL;

  dev->proxy.chroma_adaptation = NULL;
  dev->proxy.wb_is_D65 = TRUE; // don't display error messages until we know for sure it's FALSE
  dev->proxy.wb_coeffs[0] = 0.f;

  dev->rawoverexposed.mode = dt_conf_get_int("darkroom/ui/rawoverexposed/mode");
  dev->rawoverexposed.colorscheme = dt_conf_get_int("darkroom/ui/rawoverexposed/colorscheme");
  dev->rawoverexposed.threshold = dt_conf_get_float("darkroom/ui/rawoverexposed/threshold");

  dev->overexposed.mode = dt_conf_get_int("darkroom/ui/overexposed/mode");
  dev->overexposed.colorscheme = dt_conf_get_int("darkroom/ui/overexposed/colorscheme");
  dev->overexposed.lower = dt_conf_get_float("darkroom/ui/overexposed/lower");
  dev->overexposed.upper = dt_conf_get_float("darkroom/ui/overexposed/upper");

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

static void _dev_start_pipeline(dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe)
{
  if(!pipe->running)
  {
    switch(pipe->type)
    {
      case DT_DEV_PIXELPIPE_PREVIEW:
        dt_control_add_job_res(darktable.control, dt_dev_process_preview_job_create(dev), DT_CTL_WORKER_DARKROOM_THUMB);
        break;
      case DT_DEV_PIXELPIPE_FULL:
        dt_control_add_job_res(darktable.control, dt_dev_process_image_job_create(dev), DT_CTL_WORKER_DARKROOM_MAIN);
        break;
      default:
        break;
    }
  }
  // else : join currently-running threads
}

void dt_dev_start_all_pipelines(dt_develop_t *dev)
{
  const gboolean preview_running = dev && dev->preview_pipe && dev->preview_pipe->running;
  const gboolean main_running = dev && dev->pipe && dev->pipe->running;
  if(dev->pipelines_started && preview_running && main_running) return;

  // Try to make the preview pipe runs first, we need it for many output sizes computations
  // aka give a timeout to main pipe. No guaranty though, we don't control threads.
  dev->pipe->timeout = 150000; // 150 ms
  _dev_start_pipeline(dev, dev->preview_pipe);
  _dev_start_pipeline(dev, dev->pipe);

  dev->pipelines_started = TRUE;
}

static void _flag_pipe(dt_dev_pixelpipe_t *pipe, gboolean error)
{
  const gboolean shutdown = dt_atomic_get_int(&pipe->shutdown);
  const gboolean pending_change = (pipe->changed != DT_DEV_PIPE_UNCHANGED);

  // If dt_dev_pixelpipe_process() returned with a state int == 1
  // and the shutdown flag is on, it means history commit activated the kill-switch.
  // Any other circomstance returning 1 is a runtime error, flag it invalid.
  if(error && !shutdown)
    pipe->status = DT_DEV_PIXELPIPE_INVALID;

  /* If a new change request landed while this run was executing, keep the
   * loop in DIRTY state regardless of intermediate VALID/UNDEF transitions.
   * This decouples "needs another run" from GUI writes to pipe->status. */
  else if(shutdown || pending_change)
    pipe->status = DT_DEV_PIXELPIPE_DIRTY;

  // Before calling dt_dev_pixelpipe_process(), we set the status to DT_DEV_PIXELPIPE_UNDEF.
  // If it's still set to this value and we have a backbuf, everything went well.
  else if(pipe->status == DT_DEV_PIXELPIPE_UNDEF)
    pipe->status = DT_DEV_PIXELPIPE_VALID;

  // Otherwise keep the status as-is and let the outer loop decide.
}

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
  if(!dev->roi.raw_inited || !dev->roi.gui_inited || !dev->history || !dev->iop || !dev->virtual_pipe) return 1;
  
  // Keep the virtual pipe synced so ROI computations on the GUI thread
  // always use up-to-date history and input sizes.
  if(dev->virtual_pipe->imgid != dev->image_storage.id
      || dev->virtual_pipe->iwidth != dev->roi.raw_width
      || dev->virtual_pipe->iheight != dev->roi.raw_height
      || dev->virtual_pipe->image.id != dev->image_storage.id)
    dt_dev_pixelpipe_set_input(dev->virtual_pipe, dev, dev->image_storage.id,
                                dev->roi.raw_width, dev->roi.raw_height, DT_MIPMAP_FULL);

  if(!dev->virtual_pipe->nodes)
    dev->virtual_pipe->changed |= DT_DEV_PIPE_REMOVE;
  else if(dt_dev_pixelpipe_get_history_hash(dev->virtual_pipe) != dt_dev_get_history_hash(dev))
    dev->virtual_pipe->changed |= DT_DEV_PIPE_SYNCH;

  if(dev->virtual_pipe->changed != DT_DEV_PIPE_UNCHANGED)
    dt_dev_pixelpipe_change(dev->virtual_pipe, dev);

  // Compute the virtual full-res output. This needs an inited history
  int processed_width = dev->roi.processed_width;
  int processed_height = dev->roi.processed_height;
  dt_dev_pixelpipe_get_roi_out(dev->virtual_pipe, dev, dev->roi.raw_width, dev->roi.raw_height, 
                               &processed_width, &processed_height);

  // Compute the scaling factor that makes full-res output fit within widget
  const float natural_scale = fminf(fminf((float)dev->roi.width / (float)processed_width,
                                          (float)dev->roi.height / (float)processed_height),
                                    1.f)
                              * darktable.gui->ppd;

  // Get the final size of the preview backbuffer
  const int preview_width = roundf(natural_scale * processed_width);
  const int preview_height = roundf(natural_scale * processed_height);

  dev->roi.processed_width = processed_width;
  dev->roi.processed_height = processed_height;
  dev->roi.natural_scale = natural_scale;
  dev->roi.preview_width = preview_width;
  dev->roi.preview_height = preview_height;
  dev->roi.output_inited = TRUE;

  dt_print(DT_DEBUG_DEV,
            "[pixelpipe] thumbnail sizes raw %dx%d -> processed %dx%d -> preview %dx%d (scale %.5f)\n",
            dev->roi.raw_width, dev->roi.raw_height, dev->roi.processed_width, dev->roi.processed_height,
            dev->roi.preview_width, dev->roi.preview_height, dev->roi.natural_scale);
  
  return 0;
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

  // Update theoritical final scale based on distorting modules
  // This also writes piece->buf_in/out for each pipe->nodes piece,
  // so it's not nearly a matter of getting processed_width/height
  if(pipe->processed_height != dev->roi.processed_height 
    || pipe->processed_width != dev->roi.processed_height)
    dt_dev_pixelpipe_get_roi_out(pipe, dev, pipe->iwidth, pipe->iheight, &pipe->processed_width,
                                 &pipe->processed_height);

  // Scale is inited to the value that would fit our full-res raw to GUI viewport size
  *scale = dev->roi.natural_scale;
  // The full pipeline shows only the ROI, which may be zoomed in/out
  if(pipe->type == DT_DEV_PIXELPIPE_FULL) *scale *= dev->roi.scaling;

  // Backbuf size depends on GUI window size only
  int roi_width = roundf(*scale * dev->roi.processed_width);
  int roi_height = roundf(*scale * dev->roi.processed_height);
  int widget_wd = dev->roi.width * darktable.gui->ppd;
  int widget_ht = dev->roi.height * darktable.gui->ppd;

  *wd = roundf(fminf(roi_width, widget_wd));
  *ht = roundf(fminf(roi_height, widget_ht));

  // dev->roi.x,y are the relative coordinates of the ROI center.
  // in preview pipe, we always render a full image, so x,y = 0,0 
  // otherwise, x,y here are the top-left corner. Translate:
  *x = (pipe->type == DT_DEV_PIXELPIPE_PREVIEW) ? 0 : roundf(dev->roi.x * roi_width - *wd * .5f);
  *y = (pipe->type == DT_DEV_PIXELPIPE_PREVIEW) ? 0 : roundf(dev->roi.y * roi_height - *ht * .5f);

/*  fprintf (stderr, "_update_darkroom_roi: dev %.2f %.2f  type %s  xy %d %d  dim %d %d"
                   "   ppd:%.4f scale:%.4f nat_scale:%.4f * scaling:%.4f\n",
            dev->roi.x, dev->roi.y, dt_pipe_type_to_str(pipe->type), *x, *y, *wd, *ht, darktable.gui->ppd, *scale, dev->roi.natural_scale, dev->roi.scaling);
*/
  return x_old != *x || y_old != *y || wd_old != *wd || ht_old != *ht || old_scale != *scale;
}

void dt_dev_darkroom_pipeline(dt_develop_t *dev, dt_dev_pixelpipe_t *pipe)
{
  pipe->running = 1;

  // Infinite loop: run for as long as the thread is running
  while(!dev->exit && dt_control_running())
  {
    if(!_darkroom_pipeline_inputs_ready(dev))
    {
      pipe->status = DT_DEV_PIXELPIPE_DIRTY;
      dt_iop_nap(50000); // wait 50 ms until GUI/image sizes are initialized
      continue;
    }

    if(pipe->imgid != dev->image_storage.id
       || pipe->iwidth != dev->roi.raw_width
       || pipe->iheight != dev->roi.raw_height
       || pipe->image.id != dev->image_storage.id)
      dt_dev_pixelpipe_set_input(pipe, dev, dev->image_storage.id, dev->roi.raw_width, dev->roi.raw_height,
                                 DT_MIPMAP_FULL);

    // Keep track of ROI changes out of the loop
    float scale = 1.f;
    int x = 0, y = 0, wd = 0, ht = 0;

    // Count the number of pipe re-entries and limit it to 2 to avoid infinite loops
    int reentries = 0;

    if(pipe->timeout)
    {
      g_usleep(pipe->timeout);
      pipe->timeout = 0;
    }

    // Updating loop: run while output is invalid/unavailable, or while realtime mode
    // has a new history state to consume.
    while(!dev->exit && dt_control_running() && !pipe->pause)
    {
      const gboolean needs_regular_update
          = (((pipe->status == DT_DEV_PIXELPIPE_DIRTY) || (pipe->changed != DT_DEV_PIPE_UNCHANGED))
             && reentries < 2);
      const gboolean needs_realtime_update
          = (dt_dev_get_history_hash(dev) != dt_dev_pixelpipe_get_history_hash(pipe));

      if(!(needs_regular_update || needs_realtime_update)) break;

      // If we know history changed, ensure at least the last step is resynced
      if(needs_realtime_update)
        pipe->changed |= DT_DEV_PIPE_TOP_CHANGED;

      dt_pthread_mutex_lock(&pipe->busy_mutex);
      pipe->processing = 1;

      // We are starting fresh, reset the killswitch signal
      dt_atomic_set_int(&pipe->shutdown, FALSE);

      // In case of re-entry, we will rerun the whole pipe, so we need
      // to resynch it in full too before.
      // Need to be before dt_dev_pixelpipe_change()
      if(dt_dev_pixelpipe_has_reentry(pipe))
      {
        pipe->changed |= DT_DEV_PIPE_REMOVE;
        dt_dev_pixelpipe_cache_flush(darktable.pixelpipe_cache, pipe->type);
        // Keep the virtual pipe aligned with preview re-entry rebuilds.
        if(pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
          dt_dev_pixelpipe_sync_virtual(dev, DT_DEV_PIPE_REMOVE);
      }

      // Resynch history with pipeline. NB: this locks dev->history_mutex
      while(pipe->changed != DT_DEV_PIPE_UNCHANGED)
        dt_dev_pixelpipe_change(pipe, dev);

      // If user zoomed/panned in darkroom during the previous loop of recomputation,
      // the kill-switch event was sent, which terminated the pipeline before completion in the previous run,
      // but the coordinates of the ROI changed since then, and we will handle the new coordinates right away,
      // without exiting the thread to avoid the overhead of restarting a new one.
      // However, if the pipe re-entry flag was set, now the hash ID of the object (mask or module)
      // that captured it has changed too (because all hashes depend on ROI size & position too).
      // Since only the object that locked the re-entry flag can unlock it, and we now lost its reference,
      // nothing will unset it anymore, so we simply hard-reset it.
      if(_update_darkroom_roi(dev, pipe, &x, &y, &wd, &ht, &scale))
        dt_dev_pixelpipe_reset_reentry(pipe);

      // Catch early killswitch. dt_dev_pixelpipe_change() can be lengthy with huge masks stacks
      if(dt_atomic_get_int(&pipe->shutdown))
      {
        pipe->processing = 0;
        dt_pthread_mutex_unlock(&pipe->busy_mutex);
        pipe->status = DT_DEV_PIXELPIPE_DIRTY;
        break;
      }

      // Signal that we are starting
      pipe->status = DT_DEV_PIXELPIPE_UNDEF;

      dt_iop_roi_t roi = (dt_iop_roi_t){ x, y, wd, ht, scale };

      dt_control_log_busy_enter();
      dt_control_toast_busy_enter();

      dt_times_t thread_start;
      dt_get_times(&thread_start);
      const gint64 process_start_us = g_get_monotonic_time();

      dev->progress.completed = 0;
      dev->progress.total = 0;
      int ret = dt_dev_pixelpipe_process(pipe, dev, roi);
      dev->progress.completed = 0;
      dev->progress.total = 0;
      const gint64 process_runtime_us = g_get_monotonic_time() - process_start_us;

      gchar *msg = g_strdup_printf("[dev_process_%s] pipeline processing thread", dt_pixelpipe_get_pipe_name(pipe->type));
      dt_show_times(&thread_start, msg);
      dt_free(msg);

      dt_control_log_busy_leave();
      dt_control_toast_busy_leave();

      // If pipe is flagged for re-entry, we need to restart it right away
      if(dt_dev_pixelpipe_has_reentry(pipe))
      {
        reentries++;
        pipe->status = DT_DEV_PIXELPIPE_DIRTY;
      }
      else
      {
        _flag_pipe(pipe, ret);
      }

      if(pipe->status == DT_DEV_PIXELPIPE_VALID) dt_gui_throttle_record_runtime(pipe, process_runtime_us);

      pipe->processing = 0;

      dt_pthread_mutex_unlock(&pipe->busy_mutex);

      if(pipe->status == DT_DEV_PIXELPIPE_VALID)
      {
        if(pipe->type == DT_DEV_PIXELPIPE_FULL)
          DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED);
        else if(pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
          DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED);

        if(pipe->type == DT_DEV_PIXELPIPE_FULL)
          dt_control_queue_redraw_center();
        else if(pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
          dt_control_queue_redraw();
      }

      if(!dt_dev_pixelpipe_get_realtime(pipe))
        dt_iop_nap(50000); // wait 50 ms
      else
        dt_iop_nap(10000);
    }
    dt_iop_nap(50000); // wait 50 ms
  }

  pipe->running = 0;
}

void dt_dev_process_preview_job(dt_develop_t *dev)
{
  dt_dev_darkroom_pipeline(dev, dev->preview_pipe);
}

void dt_dev_process_image_job(dt_develop_t *dev)
{
  dt_dev_darkroom_pipeline(dev, dev->pipe);
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
static inline int _dt_dev_load_raw(dt_develop_t *dev, const int32_t imgid)
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

  return 0;
}

// return the zoom scale to fit into the viewport
float dt_dev_get_zoom_scale(const dt_develop_t *dev, const gboolean preview)
{
  const float w = preview ? dev->roi.processed_width : dev->pipe->processed_width;
  const float h = preview ? dev->roi.processed_height : dev->pipe->processed_height;
  return fminf(dev->roi.width / w, dev->roi.height / h);
}

int dt_dev_load_image(dt_develop_t *dev, const int32_t imgid)
{
  const int ret = _dt_dev_load_raw(dev, imgid);
  if(ret) return ret;

  return dt_dev_load_image_finish(dev, imgid);
}

int dt_dev_load_image_finish(dt_develop_t *dev, const int32_t imgid)
{
  if(!dev) return DT_DEV_IMAGE_STORAGE_DB_NOT_READ;

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

  if(dev->gui_attached && !dev->pixelpipe_init_batching) 
  {
    dt_dev_get_thumbnail_size(dev);
  }

  return 0;
}

void dt_dev_configure_real(dt_develop_t *dev, int wd, int ht)
{
  // Called only from Darkroom to init and update drawing size
  // depending on sidebars and main window resizing.
  if(dev->roi.width != wd || dev->roi.height != ht)
  {
    // If dimensions didn't change or we don't have a valid output image to display

    dev->roi.width = wd;
    dev->roi.height = ht;
    dev->roi.gui_inited = TRUE;

    dt_print(DT_DEBUG_DEV, "[pixelpipe] Darkroom requested a %i×%i px main preview\n", wd, ht);

    if(dev->image_storage.id != UNKNOWN_IMAGE)
    {
      dt_dev_get_thumbnail_size(dev);
      dt_dev_update_mouse_effect_radius(dev); 
      dt_dev_pixelpipe_update_zoom_main(dev);
      dt_dev_pixelpipe_update_zoom_preview(dev);
    }
  }
}

void dt_dev_check_zoom_pos_bounds(dt_develop_t *dev, float *dev_x, float *dev_y, float *box_w, float *box_h)
{
  // for the debug strings lower
  //float old_x = *dev_x;
  //float old_y = *dev_y;
  int proc_w = 0;
  int proc_h = 0;
  dt_dev_get_processed_size(dev, &proc_w, &proc_h);
  const float scale = dt_dev_get_zoom_level(dev)/ darktable.gui->ppd;

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

void dt_dev_coordinates_widget_delta_to_image_delta(dt_develop_t *dev, const int delta_in, float *delta_out)
{
  
}

void dt_dev_coordinates_widget_to_image_norm(dt_develop_t *dev, float *points, size_t num_points)
{
  if(!dev || !points || num_points == 0) return;
  const float processed_width = dev->roi.processed_width;
  const float processed_height = dev->roi.processed_height;
  if(processed_width == 0.0f || processed_height == 0.0f) return;

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
int dt_dev_distort_transform_locked(dt_develop_t *dev, dt_dev_pixelpipe_t *pipe, const double iop_order,
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
      module->distort_transform(module, piece, points, points_count);
    }
  }
  return 1;
}

int dt_dev_distort_transform_plus(dt_develop_t *dev, dt_dev_pixelpipe_t *pipe, const double iop_order, const int transf_direction,
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
      module->distort_backtransform(module, piece, points, points_count);
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
  if(!dev->roi.gui_inited) return -1.f;

  return fminf(fminf((float)dev->roi.width / (float)dev->roi.processed_width,
                      (float)dev->roi.height / (float)dev->roi.processed_height),
                1.f)
          * darktable.gui->ppd;
}

float dt_dev_get_fit_scale(dt_develop_t *dev)
{
  const float nat_scale = fminf(fminf((float)dev->roi.width / (float)dev->roi.preview_width,
                         (float)dev->roi.height / (float)dev->roi.preview_height),
                          1.f);
  return dev->roi.scaling * nat_scale;
}

float dt_dev_get_overlay_scale(dt_develop_t *dev)
{
  return dt_dev_get_fit_scale(dev) * darktable.gui->ppd;
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
  const float ppd = darktable.gui->ppd;

  // Limit zoom in to 16x the size of an apparent pixel on screen
  const float pixel_actual_size = natural_scale * dev->roi.scaling;
  const float pixel_max_size = 16.f * ppd;
  
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
