/*
    This file is part of darktable,
    Copyright (C) 2009-2021 darktable developers.

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
#include <assert.h>
#include <glib/gprintf.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "common/atomic.h"
#include "common/debug.h"
#include "common/history.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/mipmap_cache.h"
#include "common/opencl.h"
#include "common/tags.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/lightroom.h"
#include "develop/masks.h"
#include "develop/pixelpipe_cache.h"
#include "gui/gtk.h"
#include "gui/presets.h"

#define DT_DEV_AVERAGE_DELAY_START 250
#define DT_DEV_PREVIEW_AVERAGE_DELAY_START 50
#define DT_DEV_AVERAGE_DELAY_COUNT 5
#define DT_IOP_ORDER_INFO (darktable.unmuted & DT_DEBUG_IOPORDER)

void dt_dev_init(dt_develop_t *dev, int32_t gui_attached)
{
  memset(dev, 0, sizeof(dt_develop_t));
  dev->gui_module = NULL;
  dev->average_delay = DT_DEV_AVERAGE_DELAY_START;
  dev->preview_average_delay = DT_DEV_PREVIEW_AVERAGE_DELAY_START;
  dt_pthread_mutex_init(&dev->history_mutex, NULL);
  dev->history_end = 0;
  dev->history = NULL; // empty list
  dev->history_hash = 0;

  dev->gui_attached = gui_attached;
  dev->width = -1;
  dev->height = -1;
  dev->exit = 0;

  dt_image_init(&dev->image_storage);
  dev->image_invalid_cnt = 0;
  dev->pipe = dev->preview_pipe = NULL;
  dev->histogram_pre_tonecurve = NULL;
  dev->histogram_pre_levels = NULL;
  dev->forms = NULL;
  dev->form_visible = NULL;
  dev->form_gui = NULL;
  dev->allforms = NULL;
  dev->forms_hash = 0;
  dev->forms_changed = FALSE;

  if(dev->gui_attached)
  {
    dev->pipe = (dt_dev_pixelpipe_t *)malloc(sizeof(dt_dev_pixelpipe_t));
    dev->preview_pipe = (dt_dev_pixelpipe_t *)malloc(sizeof(dt_dev_pixelpipe_t));
    dt_dev_pixelpipe_init(dev->pipe);
    dt_dev_pixelpipe_init_preview(dev->preview_pipe);
    dev->histogram_pre_tonecurve = (uint32_t *)calloc(4 * 256, sizeof(uint32_t));
    dev->histogram_pre_levels = (uint32_t *)calloc(4 * 256, sizeof(uint32_t));

    // FIXME: these are uint32_t, setting to -1 is confusing
    dev->histogram_pre_tonecurve_max = -1;
    dev->histogram_pre_levels_max = -1;
  }

  dev->raw_histogram.buffer = NULL;
  dev->raw_histogram.op = "demosaic";
  dev->raw_histogram.height = 0;
  dev->raw_histogram.width = 0;
  dev->raw_histogram.hash = -1;
  dev->raw_histogram.bpp = 0;

  dev->output_histogram.buffer = NULL;
  dev->output_histogram.op = "colorout";
  dev->output_histogram.width = 0;
  dev->output_histogram.height = 0;
  dev->output_histogram.hash = -1;
  dev->output_histogram.bpp = 0;

  dev->display_histogram.buffer = NULL;
  dev->display_histogram.op = "gamma";
  dev->display_histogram.width = 0;
  dev->display_histogram.height = 0;
  dev->display_histogram.hash = -1;
  dev->display_histogram.bpp = 0;

  dev->auto_save_timeout = 0;
  dev->drawing_timeout = 0;

  dev->iop_instance = 0;
  dev->iop = NULL;
  dev->alliop = NULL;

  dev->allprofile_info = NULL;

  dev->iop_order_version = 0;
  dev->iop_order_list = NULL;

  dev->proxy.exposure.module = NULL;
  dev->proxy.chroma_adaptation = NULL;
  dev->proxy.wb_is_D65 = TRUE; // don't display error messages until we know for sure it's FALSE
  dev->proxy.wb_coeffs[0] = 0.f;

  dev->rawoverexposed.enabled = FALSE;
  dev->rawoverexposed.mode = dt_conf_get_int("darkroom/ui/rawoverexposed/mode");
  dev->rawoverexposed.colorscheme = dt_conf_get_int("darkroom/ui/rawoverexposed/colorscheme");
  dev->rawoverexposed.threshold = dt_conf_get_float("darkroom/ui/rawoverexposed/threshold");

  dev->overexposed.enabled = FALSE;
  dev->overexposed.mode = dt_conf_get_int("darkroom/ui/overexposed/mode");
  dev->overexposed.colorscheme = dt_conf_get_int("darkroom/ui/overexposed/colorscheme");
  dev->overexposed.lower = dt_conf_get_float("darkroom/ui/overexposed/lower");
  dev->overexposed.upper = dt_conf_get_float("darkroom/ui/overexposed/upper");

  dev->iso_12646.enabled = FALSE;

  // Init the mask lock state
  dev->mask_lock = 0;
  dev->darkroom_skip_mouse_events = 0;
}

void dt_dev_cleanup(dt_develop_t *dev)
{
  if(!dev) return;
  // image_cache does not have to be unref'd, this is done outside develop module.

  if(dev->raw_histogram.buffer) dt_free_align(dev->raw_histogram.buffer);
  if(dev->output_histogram.buffer) dt_free_align(dev->output_histogram.buffer);
  if(dev->display_histogram.buffer) dt_free_align(dev->display_histogram.buffer);

  // On dev cleanup, it is expected to force an history save
  if(dev->auto_save_timeout) g_source_remove(dev->auto_save_timeout);
  if(dev->drawing_timeout) g_source_remove(dev->drawing_timeout);

  dev->proxy.chroma_adaptation = NULL;
  dev->proxy.wb_coeffs[0] = 0.f;
  if(dev->pipe)
  {
    dt_dev_pixelpipe_cleanup(dev->pipe);
    free(dev->pipe);
  }
  if(dev->preview_pipe)
  {
    dt_dev_pixelpipe_cleanup(dev->preview_pipe);
    free(dev->preview_pipe);
  }
  while(dev->history)
  {
    dt_dev_free_history_item(((dt_dev_history_item_t *)dev->history->data));
    dev->history = g_list_delete_link(dev->history, dev->history);
  }
  while(dev->iop)
  {
    dt_iop_cleanup_module((dt_iop_module_t *)dev->iop->data);
    free(dev->iop->data);
    dev->iop = g_list_delete_link(dev->iop, dev->iop);
  }
  while(dev->alliop)
  {
    dt_iop_cleanup_module((dt_iop_module_t *)dev->alliop->data);
    free(dev->alliop->data);
    dev->alliop = g_list_delete_link(dev->alliop, dev->alliop);
  }
  g_list_free_full(dev->iop_order_list, free);
  while(dev->allprofile_info)
  {
    dt_ioppr_cleanup_profile_info((dt_iop_order_iccprofile_info_t *)dev->allprofile_info->data);
    dt_free_align(dev->allprofile_info->data);
    dev->allprofile_info = g_list_delete_link(dev->allprofile_info, dev->allprofile_info);
  }
  dt_pthread_mutex_destroy(&dev->history_mutex);

  free(dev->histogram_pre_tonecurve);
  free(dev->histogram_pre_levels);

  g_list_free_full(dev->forms, (void (*)(void *))dt_masks_free_form);
  g_list_free_full(dev->allforms, (void (*)(void *))dt_masks_free_form);

  dt_conf_set_int("darkroom/ui/rawoverexposed/mode", dev->rawoverexposed.mode);
  dt_conf_set_int("darkroom/ui/rawoverexposed/colorscheme", dev->rawoverexposed.colorscheme);
  dt_conf_set_float("darkroom/ui/rawoverexposed/threshold", dev->rawoverexposed.threshold);

  dt_conf_set_int("darkroom/ui/overexposed/mode", dev->overexposed.mode);
  dt_conf_set_int("darkroom/ui/overexposed/colorscheme", dev->overexposed.colorscheme);
  dt_conf_set_float("darkroom/ui/overexposed/lower", dev->overexposed.lower);
  dt_conf_set_float("darkroom/ui/overexposed/upper", dev->overexposed.upper);
}

void dt_dev_process_image(dt_develop_t *dev)
{
  if(!dev->gui_attached) return;
  const int err
      = dt_control_add_job_res(darktable.control, dt_dev_process_image_job_create(dev), DT_CTL_WORKER_DARKROOM_THUMB);
  if(err) fprintf(stderr, "[dev_process_image] job queue exceeded!\n");
}

void dt_dev_process_preview(dt_develop_t *dev)
{
  if(!dev->gui_attached) return;
  const int err
      = dt_control_add_job_res(darktable.control, dt_dev_process_preview_job_create(dev), DT_CTL_WORKER_DARKROOM_MAIN);
  if(err) fprintf(stderr, "[dev_process_preview] job queue exceeded!\n");
}

void dt_dev_refresh_ui_images_real(dt_develop_t *dev)
{
  // We need to get the shutdown atomic set to TRUE,
  // which is handled everytime history is changed,
  // including when initing a new pipeline (from scratch or from user history).
  // Benefit is atomics are de-facto thread-safe.
  if(dt_atomic_get_int(&dev->preview_pipe->shutdown) && !dev->preview_pipe->processing)
    dt_dev_process_preview(dev);
  // else : join current pipe

  // When entering darkroom, the GUI will size itself and call the
  // configure() method of the view, which calls dev_configure() below.
  // Problem is the GUI can be glitchy and reconfigure the pipe twice with different sizes,
  // Each one starting a recompute. The shutdown mechanism should take care of stopping
  // an ongoing pipe which output we don't care about anymore.
  // But just in case, always start with the preview pipe, hoping
  // the GUI will have figured out what size it really wants when we start
  // the main preview pipe.
  if(dt_atomic_get_int(&dev->pipe->shutdown) && !dev->pipe->processing)
    dt_dev_process_image(dev);
  // else : join current pipe
}

void _dev_pixelpipe_set_dirty(dt_dev_pixelpipe_t *pipe)
{
  pipe->status = DT_DEV_PIXELPIPE_DIRTY;
}

void dt_dev_pixelpipe_rebuild(dt_develop_t *dev)
{
  if(!dev || !dev->gui_attached || !dev->pipe || !dev->preview_pipe) return;

  dt_times_t start;
  dt_get_times(&start);

  _dev_pixelpipe_set_dirty(dev->pipe);
  _dev_pixelpipe_set_dirty(dev->preview_pipe);

  dev->pipe->changed |= DT_DEV_PIPE_REMOVE;
  dev->preview_pipe->changed |= DT_DEV_PIPE_REMOVE;

  dt_atomic_set_int(&dev->pipe->shutdown, TRUE);
  dt_atomic_set_int(&dev->preview_pipe->shutdown, TRUE);

  dt_show_times(&start, "[dt_dev_invalidate] sending killswitch signal on all pipelines");
}

void dt_dev_pixelpipe_resync_main(dt_develop_t *dev)
{
  if(!dev || !dev->gui_attached || !dev->pipe) return;

  _dev_pixelpipe_set_dirty(dev->pipe);
  dev->pipe->changed |= DT_DEV_PIPE_SYNCH;
  dt_atomic_set_int(&dev->pipe->shutdown, TRUE);
}

void dt_dev_pixelpipe_resync_preview(dt_develop_t *dev)
{
  if(!dev || !dev->gui_attached || !dev->preview_pipe) return;

  _dev_pixelpipe_set_dirty(dev->preview_pipe);
  dev->preview_pipe->changed |= DT_DEV_PIPE_SYNCH;
  dt_atomic_set_int(&dev->preview_pipe->shutdown, TRUE);
}

void dt_dev_pixelpipe_resync_all(dt_develop_t *dev)
{
  if(!dev || !dev->gui_attached || !dev->pipe || !dev->preview_pipe) return;

  dt_dev_pixelpipe_resync_preview(dev);
  dt_dev_pixelpipe_resync_main(dev);
}

void dt_dev_invalidate_real(dt_develop_t *dev)
{
  if(!dev || !dev->gui_attached || !dev->pipe) return;

  dt_times_t start;
  dt_get_times(&start);

  _dev_pixelpipe_set_dirty(dev->pipe);
  dev->pipe->changed |= DT_DEV_PIPE_TOP_CHANGED;
  dt_atomic_set_int(&dev->pipe->shutdown, TRUE);

  dt_show_times(&start, "[dt_dev_invalidate] sending killswitch signal on main image pipeline");
}

void dt_dev_invalidate_zoom_real(dt_develop_t *dev)
{
  if(!dev || !dev->gui_attached || !dev->pipe) return;

  dt_times_t start;
  dt_get_times(&start);

  _dev_pixelpipe_set_dirty(dev->pipe);
  dev->pipe->changed |= DT_DEV_PIPE_ZOOMED;
  dt_atomic_set_int(&dev->pipe->shutdown, TRUE);

  dt_show_times(&start, "[dt_dev_invalidate_zoom] sending killswitch signal on main image pipeline");
}

void dt_dev_invalidate_zoom_preview(dt_develop_t *dev)
{
  if(!dev || !dev->gui_attached || !dev->pipe) return;

  dt_times_t start;
  dt_get_times(&start);

  _dev_pixelpipe_set_dirty(dev->preview_pipe);
  dev->preview_pipe->changed |= DT_DEV_PIPE_ZOOMED;
  dt_atomic_set_int(&dev->preview_pipe->shutdown, TRUE);

  dt_show_times(&start, "[dt_dev_invalidate_zoom] sending killswitch signal on preview image pipeline");
}

void dt_dev_invalidate_preview_real(dt_develop_t *dev)
{
  if(!dev || !dev->gui_attached || !dev->preview_pipe) return;

  dt_times_t start;
  dt_get_times(&start);

  _dev_pixelpipe_set_dirty(dev->preview_pipe);
  dev->preview_pipe->changed |= DT_DEV_PIPE_TOP_CHANGED;
  dt_atomic_set_int(&dev->preview_pipe->shutdown, TRUE);

  dt_show_times(&start, "[dt_dev_invalidate_preview] sending killswitch signal on preview pipeline");
}

void dt_dev_invalidate_all_real(dt_develop_t *dev)
{
  if(!dev || !dev->gui_attached || !dev->pipe || !dev->preview_pipe) return;

  dt_dev_invalidate(dev);
  dt_dev_invalidate_preview(dev);
}


static void _flag_pipe(dt_dev_pixelpipe_t *pipe, gboolean error)
{
  // If dt_dev_pixelpipe_process() returned with a state int == 1
  // and the shutdown flag is on, it means history commit activated the kill-switch.
  // Any other circomstance returning 1 is a runtime error, flag it invalid.
  if(error && !dt_atomic_get_int(&pipe->shutdown))
    pipe->status = DT_DEV_PIXELPIPE_INVALID;

  // Before calling dt_dev_pixelpipe_process(), we set the status to DT_DEV_PIXELPIPE_UNDEF.
  // If it's still set to this value and we have a backbuf, everything went well.
  else if(pipe->backbuf && pipe->status == DT_DEV_PIXELPIPE_UNDEF)
    pipe->status = DT_DEV_PIXELPIPE_VALID;

  // Otherwise, the main thread will have reset the status to DT_DEV_PIXELPIPE_DIRTY
  // and the pipe->shutdown to TRUE because history has changed in the middle of a process.
  // In that case, do nothing and do another loop
}


static void _update_gui_backbuf(dt_dev_pixelpipe_t *pipe)
{
  // The pipeline backbuffer belongs to the pixelpipe cache, so we have to communicate with it
  struct dt_pixel_cache_entry_t *cache_entry = dt_dev_pixelpipe_cache_get_entry_from_data(darktable.pixelpipe_cache, pipe->backbuf);

  // NOTE: dt_dev_pixelpipe_cache_get_entry_from_data internally puts a read lock on the cache_entry
  // so everything following is guaranteed to be safe:

  if(pipe->status != DT_DEV_PIXELPIPE_VALID || cache_entry == NULL)
  {
    // invalid pipeline either means error during processing or killswitch triggered before completion.
    // either way, the backbuf is unusable.
    if(cache_entry)
    {
      // Unref and attempt deletion on a useless cache entry
      dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, 0, FALSE, cache_entry);
      dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, 0, FALSE, cache_entry);
      dt_dev_pixelpipe_cache_remove(darktable.pixelpipe_cache, 0, FALSE, cache_entry);
    }
    return;
  }

  dt_pthread_mutex_lock(&pipe->backbuf_mutex);

  if(pipe->output_backbuf == NULL ||
      pipe->output_backbuf_width != pipe->backbuf_width ||
      pipe->output_backbuf_height != pipe->backbuf_height)
  {
    g_free(pipe->output_backbuf);
    pipe->output_backbuf_width = pipe->backbuf_width;
    pipe->output_backbuf_height = pipe->backbuf_height;
    pipe->output_backbuf = malloc(sizeof(uint8_t) * 4 * pipe->output_backbuf_width * pipe->output_backbuf_height);
  }

  if(pipe->output_backbuf)
    memcpy(pipe->output_backbuf, pipe->backbuf, sizeof(uint8_t) * 4 * pipe->output_backbuf_width * pipe->output_backbuf_height);

  pipe->output_imgid = pipe->image.id;

  dt_pthread_mutex_unlock(&pipe->backbuf_mutex);

  // We are done with pipe->backbuf, the pipe cache can now delete it, unlock it.
  dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, 0, FALSE, cache_entry);
  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, 0, FALSE, cache_entry);
}


// Return TRUE if ROI changed since previous computation
static gboolean _update_preview_scale(dt_develop_t *dev, dt_dev_pixelpipe_t *pipe, int *wd, int *ht, float *scale)
{
  // Store previous values
  int wd_old = *wd;
  int ht_old = *ht;

  *scale = fminf((float)dev->width / (float)pipe->processed_width, (float)dev->height / (float)pipe->processed_height) * darktable.gui->ppd;
  int window_width = dev->width * darktable.gui->ppd;
  int window_height = dev->height * darktable.gui->ppd;
  *wd = MIN(window_width, roundf((float)pipe->processed_width * *scale));
  *ht = MIN(window_height, roundf((float)pipe->processed_height * *scale));

  return wd_old != *wd || ht_old != *ht;
}

// Return TRUE if ROI changed since previous computation
static gboolean _update_darkroom_roi(dt_develop_t *dev, dt_dev_pixelpipe_t *pipe, int *x, int *y, int *wd, int *ht,
                                     float *scale, float *zoom_x, float *zoom_y)
{
  // Store previous values
  int x_old = *x;
  int y_old = *y;
  int wd_old = *wd;
  int ht_old = *ht;

  // determine scale according to new dimensions
  dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  int closeup = dt_control_get_dev_closeup();
  *zoom_x = dt_control_get_dev_zoom_x();
  *zoom_y = dt_control_get_dev_zoom_y();

  // if just changed to an image with a different aspect ratio or
  // altered image orientation, the prior zoom xy could now be beyond
  // the image boundary
  // FIXME: That belongs to darkroom GUI code if it's even needed

  /*
  dt_dev_pixelpipe_change_t pipe_changed = pipe->changed;

  if(pipe_changed != DT_DEV_PIPE_UNCHANGED)
  {
    dt_dev_check_zoom_bounds(dev, &zoom_x, &zoom_y, zoom, closeup, NULL, NULL);
    dt_control_set_dev_zoom_x(zoom_x);
    dt_control_set_dev_zoom_y(zoom_y);
  }
  */

  *scale = dt_dev_get_zoom_scale(dev, zoom, 1.0f, 0) * darktable.gui->ppd;
  int window_width = dev->width * darktable.gui->ppd;
  int window_height = dev->height * darktable.gui->ppd;
  if(closeup)
  {
    window_width /= 1 << closeup;
    window_height /= 1 << closeup;
  }
  *wd = MIN(window_width, roundf((float)pipe->processed_width * *scale));
  *ht = MIN(window_height, roundf((float)pipe->processed_height * *scale));
  *x = MAX(0, roundf(*scale * (float)pipe->processed_width  * (.5f + *zoom_x) - (float)*wd / 2.f));
  *y = MAX(0, roundf(*scale * (float)pipe->processed_height * (.5f + *zoom_y) - (float)*ht / 2.f));

  return x_old != *x || y_old != *y || wd_old != *wd || ht_old != *ht;
}

void dt_dev_darkroom_pipeline(dt_develop_t *dev, dt_dev_pixelpipe_t *pipe)
{
  // -1×-1 px means the dimensions of the main preview in darkroom were not inited yet.
  // 0×0 px is not feasible.
  // Anything lower than 32 px might cause segfaults with blurs and local contrast.
  // When the window size get inited, we will get a new order to recompute with a "zoom_changed" flag.
  // Until then, don't bother computing garbage that will not be reused later.
  if(dev->width < 32 || dev->height < 32) return;

  pipe->running = 1;

  dt_pthread_mutex_lock(&pipe->busy_mutex);

  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_t *cache = darktable.mipmap_cache;
  dt_mipmap_cache_get(cache, &buf, dev->image_storage.id, DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING, 'r');

  gboolean finish_on_error = (!buf.buf || buf.width == 0 || buf.height == 0);

  // Take a local copy of the buffer so we can release the mipmap cache lock immediately
  const size_t buf_width = buf.width;
  const size_t buf_height = buf.height;
  dt_mipmap_cache_release(cache, &buf);

  if(!finish_on_error)
  {
    dt_dev_pixelpipe_set_input(pipe, dev, dev->image_storage.id, buf_width, buf_height, DT_MIPMAP_FULL);
    dt_print(DT_DEBUG_DEV, "[pixelpipe] Started darkroom pipe %i recompute at %i×%i px\n", pipe->type, dev->width, dev->height);
  }

  pipe->processing = 1;

  // Count the number of pipe re-entries and limit it to 2 to avoid infinite loops
  int reentries = 0;

  // Keep track of ROI changes out of the loop
  float scale = 1.f, zoom_x = 1.f, zoom_y = 1.f;
  int x = 0, y = 0, wd = 0, ht = 0;

  while(!dev->exit && !finish_on_error && (pipe->status == DT_DEV_PIXELPIPE_DIRTY) && reentries < 2)
  {
    dt_times_t thread_start;
    dt_get_times(&thread_start);

    // We are starting fresh, reset the killswitch signal
    dt_atomic_set_int(&pipe->shutdown, FALSE);

    // In case of re-entry, we will rerun the whole pipe, so we need
    // too resynch it in full too before.
    // Need to be before dt_dev_pixelpipe_change()
    if(dt_dev_pixelpipe_has_reentry(pipe))
    {
      pipe->changed |= DT_DEV_PIPE_REMOVE;
      dt_dev_pixelpipe_cache_flush(darktable.pixelpipe_cache, pipe->type);
    }

    // this locks dev->history_mutex
    dt_dev_pixelpipe_change(pipe, dev);

    dt_control_log_busy_enter();
    dt_control_toast_busy_enter();

    // If user zoomed/panned in darkroom during the previous loop of recomputation,
    // the kill-switch event was sent, which terminated the pipeline before completion in the previous run,
    // but the coordinates of the ROI changed since then, and we will handle the new coordinates right away,
    // without exiting the thread to avoid the overhead of restarting a new one.
    // However, if the pipe re-entry flag was set, now the hash ID of the object (mask or module)
    // that captured it has changed too (because all hashes depend on ROI size & position too).
    // Since only the object that locked the re-entry flag can unlock it, and we now lost its reference,
    // nothing will unset it anymore, so we simply hard-reset it.
    gboolean reset_size = FALSE;
    if(pipe->type == DT_DEV_PIXELPIPE_FULL)
      reset_size = _update_darkroom_roi(dev, pipe, &x, &y, &wd, &ht, &scale, &zoom_x, &zoom_y);
    else if(pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
      reset_size = _update_preview_scale(dev, pipe, &wd, &ht, &scale);

    if(reset_size)
      dt_dev_pixelpipe_reset_reentry(pipe);

    // Signal that we are starting
    pipe->status = DT_DEV_PIXELPIPE_UNDEF;

    int ret = dt_dev_pixelpipe_process(pipe, dev, x, y, wd, ht, scale);

    dt_control_log_busy_leave();
    dt_control_toast_busy_leave();

    if(pipe->type == DT_DEV_PIXELPIPE_FULL)
      dt_dev_average_delay_update(&thread_start, &dev->average_delay);
    else if(pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
      dt_dev_average_delay_update(&thread_start, &dev->preview_average_delay);

    // If pipe is flagged for re-entry, we need to restart it right away
    if(dt_dev_pixelpipe_has_reentry(pipe))
    {
      reentries++;
      pipe->status = DT_DEV_PIXELPIPE_DIRTY;
    }
    else
    {
      _flag_pipe(pipe, ret);
      _update_gui_backbuf(pipe);
    }

    // cool, we got a new image!
    if(pipe->status == DT_DEV_PIXELPIPE_VALID)
    {
      pipe->backbuf_scale = scale;
      pipe->backbuf_zoom_x = zoom_x;
      pipe->backbuf_zoom_y = zoom_y;
      dev->image_invalid_cnt = 0;
    }

    if(pipe->status == DT_DEV_PIXELPIPE_VALID)
    {
      if(pipe->type == DT_DEV_PIXELPIPE_FULL)
        DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED);
      else if(pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
        DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED);
    }

    dt_iop_nap(200);
  }
  pipe->processing = 0;

  dt_pthread_mutex_unlock(&pipe->busy_mutex);

  pipe->running = 0;

  if(pipe->type == DT_DEV_PIXELPIPE_FULL)
    dt_control_queue_redraw_center();
  else if(pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
    dt_control_queue_redraw();
}


void dt_dev_process_preview_job(dt_develop_t *dev)
{
  dt_dev_darkroom_pipeline(dev, dev->preview_pipe);
}


void dt_dev_process_image_job(dt_develop_t *dev)
{
  dt_dev_darkroom_pipeline(dev, dev->pipe);
}


// load the raw and get the new image struct, blocking in gui thread
static inline int _dt_dev_load_raw(dt_develop_t *dev, const int32_t imgid)
{
  // first load the raw, to make sure dt_image_t will contain all and correct data.
  dt_times_t start;
  dt_get_times(&start);

  // Test we got images. Also that populates the cache for later.
  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_get(darktable.mipmap_cache, &buf, imgid, DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING, 'r');
  gboolean no_valid_image = (buf.buf == NULL) || buf.width == 0 || buf.height == 0;
  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);

  dt_show_times_f(&start, "[dev]", "to load the image.");

  const dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  dev->image_storage = *image;
  dt_image_cache_read_release(darktable.image_cache, image);

  return (no_valid_image);
}

float dt_dev_get_zoom_scale(dt_develop_t *dev, dt_dev_zoom_t zoom, int closeup_factor, int preview)
{
  float zoom_scale;

  const float w = preview ? dev->preview_pipe->processed_width : dev->pipe->processed_width;
  const float h = preview ? dev->preview_pipe->processed_height : dev->pipe->processed_height;
  const float ps = (float)dev->preview_pipe->processed_width / (float)dev->preview_pipe->backbuf_width;

  switch(zoom)
  {
    case DT_ZOOM_FIT:
      zoom_scale = fminf(dev->width / w, dev->height / h);
      break;
    case DT_ZOOM_FILL:
      zoom_scale = fmaxf(dev->width / w, dev->height / h);
      break;
    case DT_ZOOM_1:
      zoom_scale = closeup_factor;
      if(preview) zoom_scale *= ps;
      break;
    default: // DT_ZOOM_FREE
      zoom_scale = dt_control_get_dev_zoom_scale();
      if(preview) zoom_scale *= ps;
      break;
  }

  return zoom_scale;
}

int dt_dev_load_image(dt_develop_t *dev, const int32_t imgid)
{
  if(_dt_dev_load_raw(dev, imgid)) return 1;

  // we need a global lock as the dev->iop set must not be changed until read history is terminated
  dt_pthread_mutex_lock(&dev->history_mutex);
  dev->iop = dt_iop_load_modules(dev);

  dt_dev_read_history_ext(dev, dev->image_storage.id, FALSE);

  if(dev->pipe)
  {
    dev->pipe->processed_width = 0;
    dev->pipe->processed_height = 0;
  }
  if(dev->preview_pipe)
  {
    dev->preview_pipe->processed_width = 0;
    dev->preview_pipe->processed_height = 0;
  }
  dt_pthread_mutex_unlock(&dev->history_mutex);

  dt_dev_pixelpipe_rebuild(dev);

  return 0;
}

void dt_dev_configure_real(dt_develop_t *dev, int wd, int ht)
{
  // Called only from Darkroom to init and update drawing size
  // depending on sidebars and main window resizing.
  if(dev->width != wd || dev->height != ht || !dev->pipe->output_backbuf)
  {
    // If dimensions didn't change or we don't have a valid output image to display

    dev->width = wd;
    dev->height = ht;

    dt_print(DT_DEBUG_DEV, "[pixelpipe] Darkroom requested a %i×%i px main preview\n", wd, ht);
    dt_dev_invalidate_zoom(dev);
    dt_dev_invalidate_zoom_preview(dev);

    if(dev->image_storage.id > -1 && darktable.mipmap_cache)
    {
      // Only if it's not our initial configure call, aka if we already have an image
      dt_control_queue_redraw_center();
      dt_dev_refresh_ui_images(dev);
    }
  }
}

void dt_dev_reprocess_all(dt_develop_t *dev)
{
  dt_pthread_mutex_lock(&darktable.pipeline_threadsafe);
  dt_dev_pixelpipe_cache_flush(darktable.pixelpipe_cache, -1);
  dt_pthread_mutex_unlock(&darktable.pipeline_threadsafe);

  if(darktable.gui->reset || !dev || !dev->gui_attached) return;
  dt_dev_pixelpipe_rebuild(dev);
}

void dt_dev_check_zoom_bounds(dt_develop_t *dev, float *zoom_x, float *zoom_y, dt_dev_zoom_t zoom,
                              int closeup, float *boxww, float *boxhh)
{
  int procw = 0, proch = 0;
  dt_dev_get_processed_size(dev, &procw, &proch);
  float boxw = 1.0f, boxh = 1.0f; // viewport in normalised space
                            //   if(zoom == DT_ZOOM_1)
                            //   {
                            //     const float imgw = (closeup ? 2 : 1)*procw;
                            //     const float imgh = (closeup ? 2 : 1)*proch;
                            //     const float devw = MIN(imgw, dev->width);
                            //     const float devh = MIN(imgh, dev->height);
                            //     boxw = fminf(1.0, devw/imgw);
                            //     boxh = fminf(1.0, devh/imgh);
                            //   }
  if(zoom == DT_ZOOM_FIT)
  {
    *zoom_x = *zoom_y = 0.0f;
    boxw = boxh = 1.0f;
  }
  else
  {
    const float scale = dt_dev_get_zoom_scale(dev, zoom, 1<<closeup, 0);
    const float imgw = procw;
    const float imgh = proch;
    const float devw = dev->width;
    const float devh = dev->height;
    boxw = devw / (imgw * scale);
    boxh = devh / (imgh * scale);
  }

  if(*zoom_x < boxw / 2 - .5) *zoom_x = boxw / 2 - .5;
  if(*zoom_x > .5 - boxw / 2) *zoom_x = .5 - boxw / 2;
  if(*zoom_y < boxh / 2 - .5) *zoom_y = boxh / 2 - .5;
  if(*zoom_y > .5 - boxh / 2) *zoom_y = .5 - boxh / 2;
  if(boxw > 1.0) *zoom_x = 0.0f;
  if(boxh > 1.0) *zoom_y = 0.0f;

  if(boxww) *boxww = boxw;
  if(boxhh) *boxhh = boxh;
}

void dt_dev_get_processed_size(const dt_develop_t *dev, int *procw, int *proch)
{
  if(!dev) return;

  // if pipe is processed, lets return its size
  if(dev->pipe && dev->pipe->processed_width)
  {
    *procw = dev->pipe->processed_width;
    *proch = dev->pipe->processed_height;
    return;
  }

  // fallback on preview pipe
  if(dev->preview_pipe && dev->preview_pipe->processed_width)
  {
    *procw = dev->preview_pipe->processed_width;
    *proch = dev->preview_pipe->processed_height;
    return;
  }

  // no processed pipes, lets return 0 size
  *procw = *proch = 0;
  return;
}

void dt_dev_get_pointer_zoom_pos(dt_develop_t *dev, const float px, const float py, float *zoom_x,
                                 float *zoom_y)
{
  dt_dev_zoom_t zoom;
  int closeup = 0, procw = 0, proch = 0;
  float zoom2_x = 0.0f, zoom2_y = 0.0f;
  zoom = dt_control_get_dev_zoom();
  closeup = dt_control_get_dev_closeup();
  zoom2_x = dt_control_get_dev_zoom_x();
  zoom2_y = dt_control_get_dev_zoom_y();
  dt_dev_get_processed_size(dev, &procw, &proch);
  const float scale = dt_dev_get_zoom_scale(dev, zoom, 1<<closeup, 0);
  // offset from center now (current zoom_{x,y} points there)
  const float mouse_off_x = px - .5 * dev->width, mouse_off_y = py - .5 * dev->height;
  zoom2_x += mouse_off_x / (procw * scale);
  zoom2_y += mouse_off_y / (proch * scale);
  *zoom_x = zoom2_x;
  *zoom_y = zoom2_y;
}

int dt_dev_is_current_image(dt_develop_t *dev, int32_t imgid)
{
  return (dev->image_storage.id == imgid) ? 1 : 0;
}

static dt_dev_proxy_exposure_t *find_last_exposure_instance(dt_develop_t *dev)
{
  if(!dev->proxy.exposure.module) return NULL;

  dt_dev_proxy_exposure_t *instance = &dev->proxy.exposure;

  return instance;
};

float dt_dev_exposure_get_exposure(dt_develop_t *dev)
{
  dt_dev_proxy_exposure_t *instance = find_last_exposure_instance(dev);

  if(instance && instance->module && instance->get_exposure) return instance->get_exposure(instance->module);

  return 0.0;
}


float dt_dev_exposure_get_black(dt_develop_t *dev)
{
  dt_dev_proxy_exposure_t *instance = find_last_exposure_instance(dev);

  if(instance && instance->module && instance->get_black) return instance->get_black(instance->module);

  return 0.0;
}

void dt_dev_modulegroups_set(dt_develop_t *dev, uint32_t group)
{
  if(dev->proxy.modulegroups.module && dev->proxy.modulegroups.set)
    dev->proxy.modulegroups.set(dev->proxy.modulegroups.module, group);
}

uint32_t dt_dev_modulegroups_get(dt_develop_t *dev)
{
  if(dev->proxy.modulegroups.module && dev->proxy.modulegroups.get)
    return dev->proxy.modulegroups.get(dev->proxy.modulegroups.module);

  return 0;
}

void dt_dev_modulegroups_switch(dt_develop_t *dev, dt_iop_module_t *module)
{
  if(dev->proxy.modulegroups.module && dev->proxy.modulegroups.switch_group)
    dev->proxy.modulegroups.switch_group(dev->proxy.modulegroups.module, module);
}

void dt_dev_modulegroups_update_visibility(dt_develop_t *dev)
{
  if(dev->proxy.modulegroups.module && dev->proxy.modulegroups.switch_group)
    dev->proxy.modulegroups.update_visibility(dev->proxy.modulegroups.module);
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

void dt_dev_average_delay_update(const dt_times_t *start, uint32_t *average_delay)
{
  dt_times_t end;
  dt_get_times(&end);

  *average_delay += ((end.clock - start->clock) * 1000 / DT_DEV_AVERAGE_DELAY_COUNT
                     - *average_delay / DT_DEV_AVERAGE_DELAY_COUNT);
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
  dt_pthread_mutex_lock(&dev->history_mutex);
  int del = 0;

  if(dev->gui_attached)
  {
    dt_dev_undo_start_record(dev);

    GList *elem = dev->history;
    while(elem != NULL)
    {
      GList *next = g_list_next(elem);
      dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(elem->data);

      if(module == hist->module)
      {
        dt_print(DT_DEBUG_HISTORY, "[dt_module_remode] removing obsoleted history item: %s %s %p %p\n", hist->module->op, hist->module->multi_name, module, hist->module);
        dt_dev_free_history_item(hist);
        dev->history = g_list_delete_link(dev->history, elem);
        dt_dev_set_history_end(dev, dt_dev_get_history_end(dev) - 1);
        del = 1;
      }
      elem = next;
    }
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

  dt_pthread_mutex_unlock(&dev->history_mutex);

  if(dev->gui_attached && del)
  {
    /* signal that history has changed */
    dt_dev_undo_end_record(dev);

    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_MODULE_REMOVE, module);
  }
}

void _dev_module_update_multishow(dt_develop_t *dev, struct dt_iop_module_t *module)
{
  // We count the number of other instances
  int nb_instances = 0;
  for(GList *modules = g_list_first(dev->iop); modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;

    if(mod->instance == module->instance) nb_instances++;
  }

  dt_iop_module_t *mod_prev = dt_iop_gui_get_previous_visible_module(module);
  dt_iop_module_t *mod_next = dt_iop_gui_get_next_visible_module(module);

  const gboolean move_next = (mod_next && mod_next->iop_order != INT_MAX) ? dt_ioppr_check_can_move_after_iop(dev->iop, module, mod_next) : -1.0;
  const gboolean move_prev = (mod_prev && mod_prev->iop_order != INT_MAX) ? dt_ioppr_check_can_move_before_iop(dev->iop, module, mod_prev) : -1.0;

  module->multi_show_new = !(module->flags() & IOP_FLAGS_ONE_INSTANCE);
  module->multi_show_close = (nb_instances > 1);
  if(mod_next)
    module->multi_show_up = move_next;
  else
    module->multi_show_up = 0;
  if(mod_prev)
    module->multi_show_down = move_prev;
  else
    module->multi_show_down = 0;
}

void dt_dev_modules_update_multishow(dt_develop_t *dev)
{
  dt_ioppr_check_iop_order(dev, 0, "dt_dev_modules_update_multishow");

  for(GList *modules = dev->iop; modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;

    // only for visible modules
    GtkWidget *expander = mod->expander;
    if(expander && gtk_widget_is_visible(expander))
    {
      _dev_module_update_multishow(dev, mod);
    }
  }
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
    g_free(clean_name);
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
  g_free(clean_name);
  return label;
}

int dt_dev_distort_transform(dt_develop_t *dev, float *points, size_t points_count)
{
  return dt_dev_distort_transform_plus(dev, dev->preview_pipe, 0.0f, DT_DEV_TRANSFORM_DIR_ALL, points, points_count);
}
int dt_dev_distort_backtransform(dt_develop_t *dev, float *points, size_t points_count)
{
  return dt_dev_distort_backtransform_plus(dev, dev->preview_pipe, 0.0f, DT_DEV_TRANSFORM_DIR_ALL, points, points_count);
}

// only call directly or indirectly from dt_dev_distort_transform_plus, so that it runs with the history locked
int dt_dev_distort_transform_locked(dt_develop_t *dev, dt_dev_pixelpipe_t *pipe, const double iop_order,
                                    const int transf_direction, float *points, size_t points_count)
{
  GList *modules = pipe->iop;
  GList *pieces = pipe->nodes;
  while(modules)
  {
    if(!pieces)
    {
      return 0;
    }
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)(pieces->data);
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
    modules = g_list_next(modules);
    pieces = g_list_next(pieces);
  }
  return 1;
}

int dt_dev_distort_transform_plus(dt_develop_t *dev, dt_dev_pixelpipe_t *pipe, const double iop_order, const int transf_direction,
                                  float *points, size_t points_count)
{
  dt_pthread_mutex_lock(&dev->history_mutex);
  dt_dev_distort_transform_locked(dev, pipe, iop_order, transf_direction, points, points_count);
  dt_pthread_mutex_unlock(&dev->history_mutex);
  return 1;
}

// only call directly or indirectly from dt_dev_distort_transform_plus, so that it runs with the history locked
int dt_dev_distort_backtransform_locked(dt_develop_t *dev, dt_dev_pixelpipe_t *pipe, const double iop_order,
                                        const int transf_direction, float *points, size_t points_count)
{
  GList *modules = g_list_last(pipe->iop);
  GList *pieces = g_list_last(pipe->nodes);
  while(modules)
  {
    if(!pieces)
    {
      return 0;
    }
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)(pieces->data);
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
    modules = g_list_previous(modules);
    pieces = g_list_previous(pieces);
  }
  return 1;
}

int dt_dev_distort_backtransform_plus(dt_develop_t *dev, dt_dev_pixelpipe_t *pipe, const double iop_order, const int transf_direction,
                                      float *points, size_t points_count)
{
  dt_pthread_mutex_lock(&dev->history_mutex);
  const int success = dt_dev_distort_backtransform_locked(dev, pipe, iop_order, transf_direction, points, points_count);
  dt_pthread_mutex_unlock(&dev->history_mutex);
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


int dt_dev_wait_hash(dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe, const double iop_order, const int transf_direction, dt_pthread_mutex_t *lock,
                     const volatile uint64_t *const hash)
{
  const int usec = 5000;
  int nloop;

#ifdef HAVE_OPENCL
  if(pipe->devid >= 0)
    nloop = darktable.opencl->opencl_synchronization_timeout;
  else
    nloop = dt_conf_get_int("pixelpipe_synchronization_timeout");
#else
  nloop = dt_conf_get_int("pixelpipe_synchronization_timeout");
#endif

  if(nloop <= 0) return TRUE;  // non-positive values omit pixelpipe synchronization

  for(int n = 0; n < nloop; n++)
  {
    if(dt_atomic_get_int(&pipe->shutdown))
      return TRUE;  // stop waiting if pipe shuts down

    uint64_t probehash;

    if(lock)
    {
      dt_pthread_mutex_lock(lock);
      probehash = *hash;
      dt_pthread_mutex_unlock(lock);
    }
    else
      probehash = *hash;

    if(probehash == dt_dev_hash(dev, pipe))
      return TRUE;

    dt_iop_nap(usec);
  }

  return FALSE;
}

int dt_dev_sync_pixelpipe_hash(dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe, const double iop_order, const int transf_direction, dt_pthread_mutex_t *lock,
                               const volatile uint64_t *const hash)
{
  // first wait for matching hash values
  if(dt_dev_wait_hash(dev, pipe, iop_order, transf_direction, lock, hash))
    return TRUE;

  // timed out. let's see if history stack has changed
  if(pipe->changed & (DT_DEV_PIPE_TOP_CHANGED | DT_DEV_PIPE_REMOVE | DT_DEV_PIPE_SYNCH))
  {
    dt_dev_invalidate(dev);
    // pretend that everything is fine
    return TRUE;
  }

  // no way to get pixelpipes in sync
  return FALSE;
}


uint64_t dt_dev_hash(dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe)
{
  uint64_t hash = 0;
  // FIXME: this should have its own hash
  // since it's available before pipeline computation
  // but after dev->history reading and pipe nodes init
  GList *pieces = g_list_last(pipe->nodes);
  if(pieces)
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)(pieces->data);
    hash = piece->global_hash;
  }
  return hash;
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
    DT_DEBUG_CONTROL_SIGNAL_RAISE
      (darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_WILL_CHANGE,
       dt_history_duplicate(dev->history),
       dt_dev_get_history_end(dev),
       dt_ioppr_iop_order_copy_deep(dev->iop_order_list));
  }
}

void dt_dev_undo_end_record(dt_develop_t *dev)
{
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);

  /* record current history state : after change (needed for undo) */
  if(dev->gui_attached && cv->view((dt_view_t *)cv) == DT_VIEW_DARKROOM)
  {
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

int32_t dt_dev_get_history_end(dt_develop_t *dev)
{
  const int num_items = g_list_length(dev->history);
  return CLAMP(dev->history_end, 0, num_items);
}

void dt_dev_set_history_end(dt_develop_t *dev, const uint32_t index)
{
  const int num_items = g_list_length(dev->history);
  dev->history_end = CLAMP(index, 0, num_items);
}

void dt_dev_append_changed_tag(const int32_t imgid)
{
  /* attach changed tag reflecting actual change */
  guint tagid = 0;
  dt_tag_new("darktable|changed", &tagid);
  const gboolean tag_change = dt_tag_attach(tagid, imgid, FALSE, FALSE);

  /* register last change timestamp in cache */
  dt_image_cache_set_change_timestamp(darktable.image_cache, imgid);

  if(tag_change) DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_TAG_CHANGED);
}

void dt_dev_masks_update_hash(dt_develop_t *dev)
{
  dt_times_t start;
  dt_get_times(&start);

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

  dt_show_times(&start, "[masks_update_hash] computing forms hash");
}

void dt_dev_get_final_size(dt_develop_t *dev, dt_dev_pixelpipe_t *pipe, const int32_t imgid, const int input_width, const int input_height, int *processed_width, int *processed_height)
{
  dt_times_t start;
  dt_get_times(&start);

  gboolean clean_dev = FALSE;
  gboolean clean_pipe = FALSE;
  dt_develop_t temp_dev;
  dt_dev_pixelpipe_t temp_pipe;

  if(dev == NULL)
  {
    clean_dev = TRUE;
    dev = &temp_dev;

    dt_dev_init(dev, 0);

    // Needed for some module's default params init/reload
    const dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'r');
    dev->image_storage = *image;
    dt_image_cache_read_release(darktable.image_cache, image);

    dev->iop = dt_iop_load_modules(dev);
    dt_dev_read_history_ext(dev, imgid, FALSE);
  }

  if(pipe == NULL)
  {
    clean_pipe = TRUE;
    pipe = &temp_pipe;
    dt_dev_pixelpipe_init_dummy(pipe, input_width, input_height);
    dt_dev_pixelpipe_set_input(pipe, dev, imgid, input_width, input_height, DT_MIPMAP_NONE);
    dt_dev_pixelpipe_create_nodes(pipe, dev);
    dt_dev_pixelpipe_synch_all(pipe, dev);
  }

  dt_dev_pixelpipe_get_roi_out(pipe, dev, input_width, input_height, processed_width, processed_height);

  if(clean_pipe) dt_dev_pixelpipe_cleanup(&temp_pipe);
  if(clean_dev) dt_dev_cleanup(&temp_dev);

  dt_show_times(&start, "[dt_dev_get_final_size] computing test final size");
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
