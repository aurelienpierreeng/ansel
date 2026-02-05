
#include "common/debug.h"
#include "common/darktable.h"
#include "common/dtpthread.h"
#include "develop/pixelpipe.h"
#include "develop/blend.h"
#include "control/control.h"

static void _change_pipe(dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_change_t flag)
{
  if(!pipe) return;
  pipe->status = DT_DEV_PIXELPIPE_DIRTY;
  pipe->changed |= flag;
  dt_atomic_set_int(&pipe->shutdown, TRUE);
}

void dt_dev_pixelpipe_rebuild_all(dt_develop_t *dev)
{
  if(!dev || !dev->gui_attached) return;
  _change_pipe(dev->preview_pipe, DT_DEV_PIPE_REMOVE);
  _change_pipe(dev->pipe, DT_DEV_PIPE_REMOVE);
}

void dt_dev_pixelpipe_resync_history_main(dt_develop_t *dev)
{
  if(!dev || !dev->gui_attached) return;
  _change_pipe(dev->pipe, DT_DEV_PIPE_SYNCH);
}

void dt_dev_pixelpipe_resync_history_preview(dt_develop_t *dev)
{
  if(!dev || !dev->gui_attached) return;
  _change_pipe(dev->preview_pipe, DT_DEV_PIPE_SYNCH);
}

void dt_dev_pixelpipe_resync_history_all(dt_develop_t *dev)
{
  if(!dev || !dev->gui_attached) return;
  dt_dev_pixelpipe_resync_history_preview(dev);
  dt_dev_pixelpipe_resync_history_main(dev);
}

void dt_dev_pixelpipe_update_history_main_real(dt_develop_t *dev)
{
  if(!dev || !dev->gui_attached) return;
  _change_pipe(dev->pipe, DT_DEV_PIPE_TOP_CHANGED);
}

void dt_dev_pixelpipe_update_history_preview_real(dt_develop_t *dev)
{
  if(!dev || !dev->gui_attached) return;
  _change_pipe(dev->preview_pipe, DT_DEV_PIPE_TOP_CHANGED);
}

void dt_dev_pixelpipe_update_history_all_real(dt_develop_t *dev)
{
  if(!dev || !dev->gui_attached) return;
  dt_dev_pixelpipe_update_preview(dev);
  dt_dev_pixelpipe_update_history_main(dev);
}

void dt_dev_pixelpipe_update_zoom_preview(dt_develop_t *dev)
{
  if(!dev || !dev->gui_attached) return;
  _change_pipe(dev->preview_pipe, DT_DEV_PIPE_ZOOMED);
}

void dt_dev_pixelpipe_update_zoom_main_real(dt_develop_t *dev)
{
  if(!dev || !dev->gui_attached) return;
  _change_pipe(dev->pipe, DT_DEV_PIPE_ZOOMED);
}

void dt_dev_pixelpipe_reset_all(dt_develop_t *dev)
{
  dt_pthread_mutex_lock(&darktable.pipeline_threadsafe);
  dt_dev_pixelpipe_cache_flush(darktable.pixelpipe_cache, -1);
  dt_pthread_mutex_unlock(&darktable.pipeline_threadsafe);

  if(darktable.gui->reset || !dev || !dev->gui_attached) return;
  dt_dev_pixelpipe_rebuild_all(dev);
}

void dt_dev_pixelpipe_refresh_main(dt_develop_t *dev, gboolean full)
{
  if (!dev || !dev->gui_attached) return;

  if(full)
    dt_dev_pixelpipe_resync_history_main(dev);
  else
    dt_dev_pixelpipe_update_history_main(dev);

  dt_dev_process(dev, dev->pipe);
}

void dt_dev_pixelpipe_refresh_preview(dt_develop_t *dev, gboolean full)
{
  if (!dev || !dev->gui_attached) return;

  if(full)
    dt_dev_pixelpipe_resync_history_preview(dev);
  else
    dt_dev_pixelpipe_update_preview(dev);

  dt_dev_process(dev, dev->preview_pipe);
}

void dt_dev_pixelpipe_refresh_all(dt_develop_t *dev, gboolean full)
{
  if (!dev || !dev->gui_attached) return;

  // Always start reprocessing thumbnail first,
  // because it's needed for final GUI sizes,
  // histograms, color pickers, etc. and is used
  // as placeholder pending a main image recompute.
  if(full)
  {
    dt_dev_pixelpipe_resync_history_preview(dev);
    dt_dev_pixelpipe_resync_history_main(dev);
  }
  else
  {
    dt_dev_pixelpipe_update_preview(dev);
    dt_dev_pixelpipe_update_history_main(dev);
  }

  dt_dev_process_all(dev);
}

void dt_dev_pixelpipe_change_zoom_main(dt_develop_t *dev)
{
  if (!dev || !dev->gui_attached) return;
  // Slightly different logic: killswitch ASAP,
  // then redraw UI ASAP for feedback,
  // finally flag the pipe as dirty for later recompute.
  // Remember GUI responsiveness is paramount, since a laggy UI
  // will make user repeat their order for lack of feedback, 
  // meaning relaunching a pipe recompute, meaning working more
  // for the same contract.
  dt_atomic_set_int(&dev->pipe->shutdown, TRUE);
  dt_control_navigation_redraw();
  gtk_widget_queue_draw(dt_ui_center(darktable.gui->ui));
  dt_dev_pixelpipe_update_zoom_main(dev);
  dt_dev_update_mouse_effect_radius(dev);
  dt_dev_process(dev, dev->pipe);
}

gboolean dt_dev_pixelpipe_activemodule_disables_currentmodule(struct dt_develop_t *dev, struct dt_iop_module_t *current_module)
{
  return (dev                  // don't segfault
          && dev->gui_attached // don't run on background/export pipes
          && dev->gui_module   // don't segfault
          && dev->gui_module != current_module 
          // current_module is not the active one (capturing edit mode)
          && dev->gui_module->operation_tags_filter() & current_module->operation_tags())
          // current_module does operation(s) that active module doesn't want
          && dt_iop_get_cache_bypass(dev->gui_module); 
          // cache bypass is our hint that the active module is in "editing" mode
}

void dt_dev_pixelpipe_get_roi_out(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev,
                                  const int width_in, const int height_in,
                                  int *width, int *height)
{
  dt_iop_roi_t roi_in = (dt_iop_roi_t){ 0, 0, width_in, height_in, 1.0 };
  dt_iop_roi_t roi_out;
  GList *modules = g_list_first(pipe->iop);
  GList *pieces = g_list_first(pipe->nodes);
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)pieces->data;

    piece->buf_in = roi_in;

    // If in GUI and using a module that needs a full, undistorterted image,
    // we need to shutdown temporarily any module distorting the image.
    if(dt_dev_pixelpipe_activemodule_disables_currentmodule(dev, module))
      piece->enabled = FALSE;

    // If module is disabled, modify_roi_in() is a no-op
    if(piece->enabled)
      module->modify_roi_out(module, piece, &roi_out, &roi_in);
    else
      roi_out = roi_in;

    piece->buf_out = roi_out;
    roi_in = roi_out;

    modules = g_list_next(modules);
    pieces = g_list_next(pieces);
  }
  *width = roi_out.width;
  *height = roi_out.height;
}

void dt_dev_pixelpipe_get_roi_in(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev, const struct dt_iop_roi_t roi_out)
{
  // while module->modify_roi_out describes how the current module will change the size of
  // the output buffer depending on its parameters (pretty intuitive),
  // module->modify_roi_in describes "how much material" the current module needs from the previous one,
  // because some modules (lens correction) need a padding on their input.
  // The tricky part is therefore that the effect of the current module->modify_roi_in() needs to be repercuted
  // upstream in the pipeline for proper pipeline cache invalidation, so we need to browse the pipeline
  // backwards.

  dt_iop_roi_t roi_out_temp = roi_out;
  dt_iop_roi_t roi_in;
  GList *modules = g_list_last(pipe->iop);
  GList *pieces = g_list_last(pipe->nodes);
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)pieces->data;

    piece->planned_roi_out = roi_out_temp;

    // If in GUI and using a module that needs a full, undistorterted image,
    // we need to shutdown temporarily any module distorting the image.
    if(dt_dev_pixelpipe_activemodule_disables_currentmodule(dev, module))
      piece->enabled = FALSE;

    // If module is disabled, modify_roi_in() is a no-op
    if(piece->enabled)
      module->modify_roi_in(module, piece, &roi_out_temp, &roi_in);
    else
      roi_in = roi_out_temp;

    /*
    if(piece->enabled)
    {
      fprintf(stdout, "%s : scale : in %f out %f - (x, y) : (%i, %i) to (%i, %i)\n",
        module->op, 
        roi_in.scale, roi_out.scale, 
        roi_in.x, roi_in.y,
        roi_out.x, roi_out.y);
    }
    */

    piece->planned_roi_in = roi_in;
    roi_out_temp = roi_in;

    modules = g_list_previous(modules);
    pieces = g_list_previous(pieces);
  }
}

static uint64_t _default_pipe_hash(dt_dev_pixelpipe_t *pipe)
{
  // Start with a hash that is unique, image-wise.
  return dt_hash(5381, (const char *)&pipe->image.filename, DT_MAX_FILENAME_LEN);
}

uint64_t dt_dev_pixelpipe_node_hash(dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t roi_out, const int pos)
{
  // to be called at runtime, not at pipe init.

  // Only at the first step of pipe, we don't have a module because we init the base buffer.
  if(piece)
    return piece->global_hash;
  else
  {
    // This is used for the first step of the pipe, before modules, when initing base buffer
    // We need to take care of the ROI manually
    uint64_t hash = _default_pipe_hash(pipe);
    hash = dt_hash(hash, (const char *)&roi_out, sizeof(dt_iop_roi_t));
    return dt_hash(hash, (const char *)&pos, sizeof(int));
  }
}

void dt_pixelpipe_get_global_hash(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev)
{
  /* Traverse the pipeline node by node and compute the cumulative (global) hash of each module.
  *  This hash takes into account the hashes of the previous modules and the size of the current ROI.
  *  It is used to map pipeline cache states to current parameters.
  *  It represents the state of internal modules params as well as their position in the pipe and their output size.
  *  It is to be called at pipe init, not at runtime.
  */

  // bernstein hash (djb2)
  uint64_t hash = _default_pipe_hash(pipe);

  // Bypassing cache contaminates downstream modules, starting at the module requesting it.
  // Usecase : crop, clip, ashift, etc. that need the uncropped image ;
  // mask displays ; overexposed/clipping alerts and all other transient previews.
  gboolean bypass_cache = FALSE;

  for(GList *node = g_list_first(pipe->nodes); node; node = g_list_next(node))
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)node->data;
    if(!piece->enabled) continue;

    // Combine with the previous bypass states
    bypass_cache |= piece->module->bypass_cache;
    piece->bypass_cache = bypass_cache;

    // Combine with the previous modules hashes
    uint64_t local_hash = piece->hash;

    // Panning and zooming change the ROI. Some GUI modes (crop in editing mode) too.
    // dt_dev_get_roi_in() should have run before
    local_hash = dt_hash(local_hash, (const char *)&piece->planned_roi_in, sizeof(dt_iop_roi_t));
    local_hash = dt_hash(local_hash, (const char *)&piece->planned_roi_out, sizeof(dt_iop_roi_t));
/*
    fprintf(stdout, "start->end : %-17s | ROI in: %4ix%-4i @%2.4f | ROI out: %4ix%-4i @%2.4f\n", piece->module->op,
            piece->buf_in.width, piece->buf_in.height, piece->buf_in.scale, piece->buf_out.width,
            piece->buf_out.height, piece->buf_out.scale);
    fprintf(stdout, "end->start : %-17s | ROI in: %4ix%-4i @%2.4f | ROI out: %4ix%-4i @%2.4f\n", piece->module->op,
            piece->planned_roi_in.width, piece->planned_roi_in.height, piece->planned_roi_in.scale,
            piece->planned_roi_out.width, piece->planned_roi_out.height, piece->planned_roi_out.scale);
*/
    // Mask preview display doesn't re-commit params, so we need to keep that of it here
    // Too much GUI stuff interleaved with pipeline stuff...
    // Mask display applies only to main preview in darkroom.
    if(pipe->type == DT_DEV_PIXELPIPE_FULL)
    {
      local_hash = dt_hash(local_hash, (const char *)&piece->module->request_mask_display, sizeof(int));
    }
    else 
    {
      const int zero = 0;
      local_hash = dt_hash(local_hash, (const char *)&zero, sizeof(int));
    }

    // Keep track of distortion bypass in GUI. That may affect upstream modules in the stack,
    // while bypass_cache only affects downstream ones.
    // In theory, distortion bypass should already affect planned ROI in/out, but it depends whether
    // internal params are committed. Anyway, make it more reliable.
    int bypass_distort = dt_dev_pixelpipe_activemodule_disables_currentmodule(dev, piece->module);
    local_hash = dt_hash(local_hash, (const char *)&bypass_distort, sizeof(int));

    // If the cache bypass is on, the corresponding cache lines will be freed immediately after use,
    // we need to track that. It somewhat overlaps module->request_mask_display, but...
    local_hash = dt_hash(local_hash, (const char *)&piece->bypass_cache, sizeof(gboolean));

    // Update global hash for this stage
    hash = dt_hash(hash, (const char *)&local_hash, sizeof(uint64_t));

    gchar *type = dt_pixelpipe_get_pipe_name(pipe->type);
    dt_print(DT_DEBUG_PIPE, "[pixelpipe] global hash for %20s (%s) in pipe %s with hash %lu\n", piece->module->op, piece->module->multi_name, type, (long unsigned int)hash);

    // In case of drawn masks, we would need to account only for the distortions of previous modules.
    // Aka conditional to: if((piece->module->operation_tags() & IOP_TAG_DISTORT) == IOP_TAG_DISTORT)
    // But in case of parametric masks, they depend on previous modules parameters.
    // So, all in all, (parametric | drawn | raster) masking depends on everything :
    // - if masking on output, internal params + blendop params + all previous modules internal params + ROI size,
    // - if masking on input, blendop params + all previous modules internal params + ROI size
    // So we use all that ot once : 
    piece->global_mask_hash = dt_hash(hash, (const char *)&piece->blendop_hash, sizeof(uint64_t));

    // Finally, the output of the module also depends on the mask:
    hash = dt_hash(hash, (const char *)&piece->global_mask_hash, sizeof(uint64_t));
    piece->global_hash = hash;
  }

  // The pipe hash is the hash of its last module.
  pipe->hash = hash;
  pipe->bypass_cache = bypass_cache;
}

gboolean _commit_history_to_node(dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece, dt_dev_history_item_t *hist)
{
  if(piece->module == hist->module)
  {
    piece->enabled = hist->enabled;
    dt_iop_commit_params(hist->module, hist->params, hist->blend_params, pipe, piece);

    if(piece->blendop_data)
    {
      const dt_develop_blend_params_t *const bp = (const dt_develop_blend_params_t *)piece->blendop_data;
      if(bp->details != 0.0f)
        pipe->want_detail_mask |= DT_DEV_DETAIL_MASK_REQUIRED;
    }
    return TRUE;
  }
  return FALSE;
}

// helper
void dt_dev_pixelpipe_synch(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, GList *history)
{
  dt_dev_history_item_t *hist = (dt_dev_history_item_t *)history->data;
  dt_dev_pixelpipe_iop_t *piece = NULL;

  // Traverse the list of pipe nodes until we found the one matching our history item.
  // We begin by the end, because it's expected that users will follow an editing history
  // roughly similar to node order, so as history is growing, we shall have an higher
  // probability of finding the last history item node at the end of the pipeline.
  for(GList *nodes = g_list_last(pipe->nodes); nodes; nodes = g_list_previous(nodes))
  {
    piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
    if(_commit_history_to_node(pipe, piece, hist))
      break;
  }
}

/**
 * @brief Find the last history item matching each pipeline node (module), in the order of pipeline execution.
 * This is super important because modules providing raster masks need to be inited before modules using them,
 * in the order of pipeline nodes. But history holds no guaranty that raster masks providers will be older
 * than raster masks users, especially after history compression. So reading in history order is not an option.
 *
 * @param pipe
 * @param dev
 * @param caller_func
 */
void dt_dev_pixelpipe_synch_all_real(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, const char *caller_func)
{
  gchar *type = dt_pixelpipe_get_pipe_name(pipe->type);
  dt_print(DT_DEBUG_DEV, "[pixelpipe] synch all modules with history for pipe %s called from %s\n", type, caller_func);

  // go through all history items and adjust params
  // note that we don't necessarily process the whole history, history_end is an user param.
  const uint32_t history_end = dt_dev_get_history_end(dev);

  for(GList *nodes = g_list_first(pipe->nodes); nodes; nodes = g_list_next(nodes))
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
    piece->hash = 0;
    piece->global_hash = 0;
    piece->enabled = piece->module->default_enabled;
    gboolean found_history = FALSE;

    // now browse all history items from the end. Since each history item is a full snapshot of parameters,
    // the latest history entry matching current node is the one we want, and we don't need to look for the previous.
    for(GList *history = g_list_nth(dev->history, history_end - 1);
        history;
        history = g_list_previous(history))
    {
      dt_dev_history_item_t *hist = (dt_dev_history_item_t *)history->data;
      if(_commit_history_to_node(pipe, piece, hist))
      {
        found_history = TRUE;
        break;
      }
    }

    // No history found, commit default params even if module is disabled because some
    // may self-enable conditionnaly there
    if(!found_history)
    {
      dt_iop_commit_params(piece->module, piece->module->default_params, piece->module->default_blendop_params,
                           pipe, piece);
    
      dt_print(DT_DEBUG_PARAMS, "[pixelpipe] info: committed default params for %s (%s) in pipe %s \n", piece->module->op, piece->module->multi_name, type);
    }
  }

  // Keep track of the last history item to have been synced
  GList *last_item = g_list_nth(dev->history, history_end - 1);
  if(last_item)
  {
    dt_dev_history_item_t *last_hist = (dt_dev_history_item_t *)last_item->data;
    pipe->last_history_hash = last_hist->hash;
  }

  pipe->resync_timestamp = time(NULL);
  pipe->history_hash = dt_dev_history_get_hash(dev);
}

void dt_dev_pixelpipe_synch_top(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev)
{
  // We can't be sure that there is only one history item to resync
  // since the last history -> pipe nodes resync: on slow systems,
  // user may have added more than one during a single pipe recompute.
  // Note however that the sync_top method is only used when adding new history items
  // on top. So we need to resync every history item from end to start, until
  // we find the previously synchronized one. This uses history hashes.
  gchar *type = dt_pixelpipe_get_pipe_name(pipe->type);

  dt_print(DT_DEBUG_DEV, "[pixelpipe] synch top modules with history for pipe %s\n", type);

  GList *last_item = g_list_nth(dev->history, dt_dev_get_history_end(dev) - 1);
  if(last_item)
  {
    GList *first_item = NULL;
    for(GList *history = last_item; history; history = g_list_previous(history))
    {
      dt_dev_history_item_t *hist = (dt_dev_history_item_t *)history->data;
      first_item = history;

      if(hist->hash == pipe->last_history_hash)
      {
        // Note that this also takes care of the case where the
        // last-known history item reference hasn't changed, but its internal
        // parameters have.
        break;
      }
      // if we don't find the hash again, we will just iterate over the whole history.
    }

    // We also need to care about the case where the history_end is not at the actual end of the history
    // aka stop looping before we overflow the desired range of history.
    GList *fence_item = g_list_nth(dev->history, dt_dev_get_history_end(dev));
    // if the history end cursor is at the actual end of the history, dt_dev_get_history_end()
    // returns an index that is outside of the range (equal to number of elements),
    // so fence_item = NULL but the code works as expected since we check history != NULL
    // first.
    for(GList *history = first_item; history && history != fence_item; history = g_list_next(history))
    {
      dt_dev_history_item_t *hist = (dt_dev_history_item_t *)history->data;
      dt_print(DT_DEBUG_PARAMS, "[pixelpipe] synch top history module `%s` (%s) for pipe %s\n", hist->module->op, hist->module->multi_name, type);
      dt_dev_pixelpipe_synch(pipe, dev, history);
    }

    // Keep track of the last history item to have been synced
    dt_dev_history_item_t *last_hist = (dt_dev_history_item_t *)last_item->data;
    pipe->last_history_hash = last_hist->hash;
  }
  else
  {
    dt_print(DT_DEBUG_DEV, "[pixelpipe] synch top history module missing error for pipe %s\n", type);
  }

  pipe->resync_timestamp = time(NULL);
  pipe->history_hash = dt_dev_history_get_hash(dev);
}

void dt_dev_pixelpipe_change(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev)
{
  dt_times_t start;
  dt_get_times(&start);

  // Read and write immediately to ensure cross-thread consistency of the value
  // in case the GUI overwrites that while we are syncing history and nodes
  const dt_dev_pixelpipe_change_t status = pipe->changed;
  pipe->changed = DT_DEV_PIPE_UNCHANGED;

  gchar *type = dt_pixelpipe_get_pipe_name(pipe->type);
  char *status_str = g_strdup_printf("%s%s%s%s%s",
                                  (status & DT_DEV_PIPE_UNCHANGED) ? "UNCHANGED " : "",
                                  (status & DT_DEV_PIPE_REMOVE) ? "REMOVE " : "",
                                  (status & DT_DEV_PIPE_TOP_CHANGED) ? "TOP_CHANGED " : "",
                                  (status & DT_DEV_PIPE_SYNCH) ? "SYNCH " : "",
                                  (status & DT_DEV_PIPE_ZOOMED) ? "ZOOMED " : "");

  dt_print(DT_DEBUG_DEV, "[dt_dev_pixelpipe_change] pipeline state changing for pipe %s, flag %s\n",
     type, status_str);

  g_free(status_str);

  // mask display off as a starting point
  pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
  // and blendif active
  pipe->bypass_blendif = 0;

  // Init fucking details masks
  const dt_image_t *img = &pipe->image;
  pipe->want_detail_mask &= DT_DEV_DETAIL_MASK_REQUIRED;
  if(dt_image_is_raw(img))
    pipe->want_detail_mask |= DT_DEV_DETAIL_MASK_DEMOSAIC;
  else if(dt_image_is_rawprepare_supported(img))
    pipe->want_detail_mask |= DT_DEV_DETAIL_MASK_RAWPREPARE;

  dt_pthread_rwlock_rdlock(&dev->history_mutex);

  // case DT_DEV_PIPE_UNCHANGED: case DT_DEV_PIPE_ZOOMED:
  if(status & DT_DEV_PIPE_REMOVE)
  {
    // modules have been added in between or removed. need to rebuild the whole pipeline.
    dt_dev_pixelpipe_cleanup_nodes(pipe);
    dt_dev_pixelpipe_create_nodes(pipe, dev);
    dt_dev_pixelpipe_synch_all(pipe, dev);
  }
  else if(status & DT_DEV_PIPE_SYNCH)
  {
    // pipeline topology remains intact, only change all params.
    dt_dev_pixelpipe_synch_all(pipe, dev);
  }
  else if(status & DT_DEV_PIPE_TOP_CHANGED)
  {
    // only top history item(s) changed.
    dt_dev_pixelpipe_synch_top(pipe, dev);
  }
  dt_pthread_rwlock_unlock(&dev->history_mutex);

  dt_show_times_f(&start, "[dev_pixelpipe] pipeline resync with history", "for pipe %s", type);
}

gboolean dt_dev_pixelpipe_is_backbufer_valid(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev)
{
  return dt_dev_history_get_hash(dev) == pipe->backbuf_hist_hash && pipe->backbuf != NULL;
}

gboolean dt_dev_pixelpipe_is_pipeline_valid(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev)
{
  return dt_dev_history_get_hash(dev) == pipe->history_hash;
}

void *dt_dev_pixelpipe_get_backbuf(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev)
{
  void *out = NULL;

  if(pipe->backbuf 
     && dt_dev_pixelpipe_cache_get_existing(darktable.pixelpipe_cache, pipe->backbuf_pipe_hash, &out, NULL, NULL))
    return out;
  else
    dt_dev_process(dev, pipe);

  return out;
}