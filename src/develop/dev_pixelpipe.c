/*
    This file is part of the Ansel project.
    Copyright (C) 2026 Aurélien PIERRE.
    Copyright (C) 2026 Guillaume Stutin.
    
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

#include "common/debug.h"
#include "common/darktable.h"
#include "common/dtpthread.h"
#include "develop/imageop.h"
#include "develop/pixelpipe.h"
#include "develop/pixelpipe_cache.h"
#include "develop/blend.h"
#include "gui/color_picker_proxy.h"
#include "control/control.h"
#include "control/signal.h"
#include <stdint.h>
#include <stdlib.h>

// Keep a preview-like virtual pipe in sync with history without running pixels.
static void _sync_virtual_pipe(dt_develop_t *dev, dt_dev_pixelpipe_change_t flag);
static void _sync_pipe_nodes_from_history_from_node(dt_dev_pixelpipe_t *pipe,
                                                    const uint32_t history_end, GList *start_node,
                                                    const char *debug_label);
// dt_dev_pixelpipe_propagate_formats() is the authoritative forward buffer-format pass; it is
// declared in dev_pixelpipe.h. It must run before any dt_pixelpipe_get_global_hash() because the
// auto-disable it performs feeds the cumulative global hash (see its definition).
static void _dt_dev_pixelpipe_cache_wait_ready_callback(gpointer instance, const guint64 hash,
                                                        gpointer user_data);

typedef struct dt_dev_pixelpipe_cache_wait_record_t
{
  dt_dev_pixelpipe_cache_wait_t *wait;
  uint64_t request_id;
  int64_t queued_at_us;
} dt_dev_pixelpipe_cache_wait_record_t;

typedef struct dt_dev_pixelpipe_cache_wait_manager_t
{
  dt_pthread_mutex_t lock;
  GList *pending;
  gboolean connected;
  gboolean wait_cursor_active;
  uint64_t next_request_id;
  uint64_t queued_requests;
  uint64_t served_requests;
  uint64_t cancelled_requests;
  uint64_t immediate_hits;
  uint64_t misses;
} dt_dev_pixelpipe_cache_wait_manager_t;

static dt_dev_pixelpipe_cache_wait_manager_t _cache_wait_manager
    = { .lock = { PTHREAD_MUTEX_INITIALIZER }, .pending = NULL, .connected = FALSE,
        .wait_cursor_active = FALSE,
        .next_request_id = 1, .queued_requests = 0, .served_requests = 0,
        .cancelled_requests = 0, .immediate_hits = 0, .misses = 0 };

static gboolean _cache_wait_cursor_progress(gpointer user_data)
{
  if(IS_NULL_PTR(darktable.control) || IS_NULL_PTR(darktable.gui) || IS_NULL_PTR(darktable.gui->ui))
    return G_SOURCE_REMOVE;

  dt_control_change_cursor_by_name_and_flush("progress");
  return G_SOURCE_REMOVE;
}

static gboolean _cache_wait_cursor_restore(gpointer user_data)
{
  if(IS_NULL_PTR(darktable.control) || IS_NULL_PTR(darktable.gui) || IS_NULL_PTR(darktable.gui->ui))
    return G_SOURCE_REMOVE;

  dt_control_commit_cursor();
  dt_control_queue_redraw_center();
  return G_SOURCE_REMOVE;
}

static dt_dev_pixelpipe_cache_wait_record_t *_cache_wait_manager_remove_wait_locked(dt_dev_pixelpipe_cache_wait_t *wait)
{
  for(GList *iter = _cache_wait_manager.pending; iter; iter = g_list_next(iter))
  {
    dt_dev_pixelpipe_cache_wait_record_t *record = iter->data;
    if(!IS_NULL_PTR(record) && record->wait == wait)
    {
      _cache_wait_manager.pending = g_list_delete_link(_cache_wait_manager.pending, iter);
      return record;
    }
  }

  return NULL;
}

void dt_dev_pixelpipe_cache_wait_dump_pending(const char *reason)
{
  const char *context = !IS_NULL_PTR(reason) ? reason : "unspecified";
  const int64_t now_us = g_get_monotonic_time();

  // This is a diagnostic snapshot only. Skip it rather than stalling GUI teardown
  // behind a cache-ready callback that is already updating the same wait list.
  if(dt_pthread_mutex_trylock(&_cache_wait_manager.lock))
  {
    dt_print(DT_DEBUG_PIPECACHE, "[cache-wait] dump reason=%s skipped=lock-busy\n", context);
    return;
  }

  const guint pending_count = g_list_length(_cache_wait_manager.pending);
  dt_print(DT_DEBUG_PIPECACHE,
           "[cache-wait] dump reason=%s pending=%u queued=%" PRIu64 " served=%" PRIu64
           " cancelled=%" PRIu64 " immediate=%" PRIu64 " misses=%" PRIu64 "\n",
           context, pending_count, _cache_wait_manager.queued_requests,
           _cache_wait_manager.served_requests, _cache_wait_manager.cancelled_requests,
           _cache_wait_manager.immediate_hits, _cache_wait_manager.misses);

  for(GList *iter = _cache_wait_manager.pending; iter; iter = g_list_next(iter))
  {
    dt_dev_pixelpipe_cache_wait_record_t *record = iter->data;
    dt_dev_pixelpipe_cache_wait_t *wait = !IS_NULL_PTR(record) ? record->wait : NULL;
    if(IS_NULL_PTR(wait) || IS_NULL_PTR(record)) continue;

    const int64_t age_ms = MAX((now_us - record->queued_at_us) / 1000, 0);
    dt_print(DT_DEBUG_PIPECACHE,
             "[cache-wait] pending id=%" PRIu64 " owner=%s hash=%" PRIu64
             " target=%s connected=%d age_ms=%" PRId64 "\n",
             wait->request_id,
             !IS_NULL_PTR(wait->owner_tag) ? wait->owner_tag : "(unknown)",
             wait->hash,
             !IS_NULL_PTR(wait->module) ? wait->module->op : "backbuf",
             wait->connected,
             age_ms);
  }
  dt_pthread_mutex_unlock(&_cache_wait_manager.lock);
}

static gboolean _module_requires_global_histogram_output_cache(const dt_dev_pixelpipe_t *pipe,
                                                               const dt_iop_module_t *module)
{
  if(IS_NULL_PTR(pipe) || IS_NULL_PTR(module)) return FALSE;
  if(pipe->type != DT_DEV_PIXELPIPE_PREVIEW) return FALSE;
  if(dt_dev_pixelpipe_get_realtime(pipe)) return FALSE;
  if(!pipe->gui_observable_source) return FALSE;

  return !strcmp(module->op, "initialscale") || !strcmp(module->op, "colorout");
}

static gboolean _module_requires_global_histogram_input_cache(const dt_dev_pixelpipe_t *pipe,
                                                              const dt_iop_module_t *module)
{
  if(IS_NULL_PTR(pipe) || IS_NULL_PTR(module)) return FALSE;
  if(pipe->type != DT_DEV_PIXELPIPE_PREVIEW) return FALSE;
  if(dt_dev_pixelpipe_get_realtime(pipe)) return FALSE;
  if(!pipe->gui_observable_source) return FALSE;

  return !strcmp(module->op, "gamma");
}

static gchar *_get_debug_pipe_name(const dt_dev_pixelpipe_t *pipe, const dt_develop_t *dev)
{
  if(!IS_NULL_PTR(dev) && !IS_NULL_PTR(pipe) && dev->virtual_pipe == pipe)
    return g_strdup("virtual-preview");

  return g_strdup(dt_pixelpipe_get_pipe_name(!IS_NULL_PTR(pipe) ? pipe->type : DT_DEV_PIXELPIPE_NONE));
}

static GList *_find_detailmask_node(dt_dev_pixelpipe_t *pipe)
{
  for(GList *nodes = g_list_first(pipe->nodes); nodes; nodes = g_list_next(nodes))
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
    if(!IS_NULL_PTR(piece) && !strcmp(piece->module->op, "detailmask")) return nodes;
  }

  return NULL;
}

static void _refresh_pipe_detail_mask_state(dt_dev_pixelpipe_t *pipe)
{
  dt_dev_pixelpipe_iop_t *detailmask_piece = NULL;
  pipe->want_detail_mask = DT_DEV_DETAIL_MASK_NONE;

  for(GList *nodes = g_list_first(pipe->nodes); nodes; nodes = g_list_next(nodes))
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
    if(IS_NULL_PTR(piece)) continue;

    if(!strcmp(piece->module->op, "detailmask")) detailmask_piece = piece;
    if(piece->detail_mask)
    {
      pipe->want_detail_mask = DT_DEV_DETAIL_MASK_ENABLED;
      break;
    }
  }

  if(!IS_NULL_PTR(detailmask_piece))
  {
    const gboolean enabled = (pipe->want_detail_mask == DT_DEV_DETAIL_MASK_ENABLED)
                             && (detailmask_piece->dsc_in.channels == 4)
                             && (detailmask_piece->dsc_in.datatype == TYPE_FLOAT)
                             && (detailmask_piece->dsc_in.cst == IOP_CS_RGB);
    detailmask_piece->enabled = enabled;
    detailmask_piece->process_tiling_ready = !enabled;
    if(!IS_NULL_PTR(detailmask_piece->data)) *((int *)detailmask_piece->data) = enabled ? 1 : 0;
  }
}

static void _change_pipe(dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_change_t flag)
{
  if(IS_NULL_PTR(pipe)) return;
  dt_dev_pixelpipe_or_changed(pipe, flag);
  dt_atomic_set_int(&pipe->shutdown, TRUE);
}

/**
 * @brief Re-commit the focused module's piece in place from transient (or realtime) params.
 *
 * @details
 * Interactive editing (drawlayer realtime stroke, ashift/crop edit mode) keeps changing the focused
 * module's params while the pipe is already instantiated. Rather than replaying the generic
 * `synch_top()` walk, we refresh just that one piece and recompute the cumulative global hash, which
 * naturally re-keys the focused piece and every downstream piece in the pixelpipe cache.
 *
 * The params come from the thread-safe **transient** channel when the focused module has published one
 * (`dt_dev_transient_params_*`), otherwise from the module's history snapshot. We never read the
 * GUI-owned `module->params` from this (pipeline) thread. The module's enable/blend state is taken from
 * its history item, since transient edits change params only.
 *
 * This helper only replaces the history-tail replay. The rest of `dt_dev_pixelpipe_change()` still runs
 * afterwards so cache policy and ROI contracts remain sealed for the next processing pass.
 */
static gboolean _sync_focused_in_place(dt_dev_pixelpipe_t *pipe)
{
  if(IS_NULL_PTR(pipe) || IS_NULL_PTR(pipe->dev) || IS_NULL_PTR(pipe->dev->gui_module)) return FALSE;
  dt_develop_t *dev = pipe->dev;
  dt_iop_module_t *focus = dev->gui_module;

  const gboolean transient = dt_dev_transient_params_active(dev, focus);
  const gboolean realtime = dt_dev_pixelpipe_get_realtime(pipe);
  if(!transient && !realtime) return FALSE;

  const uint32_t history_end = dt_dev_get_history_end_ext(dev);
  if(history_end == 0) return FALSE;

  // Enable/blend state comes from the focused module's history item (transient edits are params-only).
  dt_dev_history_item_t *hist = dt_dev_history_get_last_item_by_module(dev->history, focus, history_end);
  if(IS_NULL_PTR(hist) || IS_NULL_PTR(hist->module) || IS_NULL_PTR(hist->params)) return FALSE;

  dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)dt_dev_pixelpipe_get_module_piece(pipe, focus);
  if(IS_NULL_PTR(piece)) return FALSE;

  // Resolve params: thread-safe transient snapshot when published, else the history snapshot. A copy is
  // taken so the slot lock is not held across commit_params(); never the live GUI module->params.
  dt_iop_params_t *params = hist->params;
  void *tbuf = NULL;
  if(transient && focus->params_size > 0)
  {
    tbuf = g_malloc0(focus->params_size);
    if(!IS_NULL_PTR(tbuf)
       && dt_dev_transient_params_get(dev, focus, tbuf, (size_t)focus->params_size, NULL, 0, NULL))
      params = (dt_iop_params_t *)tbuf;
    else
      dt_free(tbuf);
  }

  const gboolean previous_want_detail_mask = (pipe->want_detail_mask != DT_DEV_DETAIL_MASK_NONE);
  piece->enabled = hist->enabled;
  piece->detail_mask = !IS_NULL_PTR(hist->blend_params) && hist->blend_params->details != 0.0f;
  dt_iop_commit_params(focus, params, hist->blend_params, pipe, piece);
  dt_free(tbuf);
  _refresh_pipe_detail_mask_state(pipe);
  if(previous_want_detail_mask != (pipe->want_detail_mask != DT_DEV_DETAIL_MASK_NONE)) return FALSE;

  // The global hash is cumulative and skips disabled nodes, so the format contract (which may
  // disable a node on an incompatible input) must be settled first.
  dt_dev_pixelpipe_propagate_formats(pipe);
  dt_pixelpipe_get_global_hash(pipe);

  // Only advance the synch_top fence when the focused module is the newest history item (the realtime
  // drawlayer case, where each heartbeat appended a top item). For a transient edit of a non-top module
  // no history item changed, so the fence must stay where the last real history sync left it.
  GList *last_item = g_list_nth(dev->history, history_end - 1);
  if(last_item && (dt_dev_history_item_t *)last_item->data == hist)
  {
    pipe->last_history_hash = hist->hash;
    pipe->last_history_item = hist;
  }
  return TRUE;
}

/**
 * @brief Seal host-cache retention policy on synchronized pieces before processing starts.
 *
 * @details
 * `piece->cache_output_on_ram` is part of the sealed runtime contract: the
 * processing recursion only consumes it and must not infer GUI/runtime
 * heuristics while running. This pass therefore authors all host-cache
 * retention requests immediately after history synchronization, while the live
 * pipe graph and current GUI state are both known.
 *
 * The reverse walk carries one upstream requirement:
 * "the previous enabled module must keep its output on host because the current
 * enabled module will read from RAM instead of OpenCL".
 *
 * Keep that carry explicit: we only cache module outputs, so any consumer that
 * needs the current module input must author a requirement on the previous
 * enabled module, not on the current one.
 *
 * We add to that:
 * - the module's own authored `cache_output_on_ram`,
 * - user cache preferences,
 * - active color-picker sampling,
 * - global histogram stages sampling their output,
 * - active GUI editing on the module itself.
 *
 * GUI cache requests are intentionally not turned into eager host retention
 * here. `dt_dev_pixelpipe_cache_peek_gui()` can already reopen a device-only
 * cacheline and materialize RAM on demand, so keeping the one-shot request out
 * of this policy avoids forcing stale module-output cachelines to be published
 * to host just because a transient GUI reader missed once.
 */
static void _seal_opencl_cache_policy(dt_dev_pixelpipe_t *pipe)
{
  if(IS_NULL_PTR(pipe) || IS_NULL_PTR(pipe->nodes)) return;

  gboolean current_output_must_cache_host = TRUE;

  for(GList *pieces = g_list_last(pipe->nodes); pieces; pieces = g_list_previous(pieces))
  {
    dt_dev_pixelpipe_iop_t *piece = pieces->data;
    dt_iop_module_t *module = !IS_NULL_PTR(piece) ? piece->module : NULL;
    if(IS_NULL_PTR(piece) || IS_NULL_PTR(module) || !piece->enabled) continue;

    gboolean supports_opencl = FALSE;
#ifdef HAVE_OPENCL
    supports_opencl = dt_opencl_is_inited() && piece->process_cl_ready && module->process_cl;
#endif

    gchar *string = g_strdup_printf("/plugins/%s/cache", module->op);
    if(!dt_conf_key_exists(string) || !dt_conf_key_not_empty(string))
      dt_conf_set_bool(string, piece->cache_output_on_ram);

    const gboolean authored_cache = piece->cache_output_on_ram;
    const gboolean user_requested_cache = dt_conf_get_bool(string);
    dt_free(string);

    const gboolean color_picker_on = dt_iop_color_picker_force_cache(pipe, module);
    const gboolean global_hist_output_on = _module_requires_global_histogram_output_cache(pipe, module);
    const gboolean global_hist_input_on = _module_requires_global_histogram_input_cache(pipe, module);
    const gboolean module_hist_on
        = (pipe->type == DT_DEV_PIXELPIPE_PREVIEW
           && pipe->gui_observable_source
           && (pipe->dev->gui_attached || !(piece->request_histogram & DT_REQUEST_ONLY_IN_GUI))
           && (piece->request_histogram & DT_REQUEST_ON));
    const gboolean active_in_gui
        = (pipe->type == DT_DEV_PIXELPIPE_FULL || pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
           && pipe->dev->gui_module == module;

    const gboolean has_autoset = pipe->autoset && !IS_NULL_PTR(module->autoset);

    const gboolean previous_output_must_cache_host
        = !supports_opencl || active_in_gui || module_hist_on || global_hist_input_on || has_autoset;

    piece->cache_output_on_ram
        = authored_cache || user_requested_cache || color_picker_on
          || global_hist_output_on
          || current_output_must_cache_host;

    current_output_must_cache_host = previous_output_must_cache_host;
  }
}

void dt_dev_pixelpipe_rebuild_all_real(dt_develop_t *dev)
{
  if(IS_NULL_PTR(dev) || !dev->gui_attached) return;
  _change_pipe(dev->preview_pipe, DT_DEV_PIPE_REMOVE);
  _change_pipe(dev->pipe, DT_DEV_PIPE_REMOVE);
  _sync_virtual_pipe(dev, DT_DEV_PIPE_REMOVE);
}

void dt_dev_pixelpipe_resync_history_main_real(dt_develop_t *dev)
{
  if(IS_NULL_PTR(dev) || !dev->gui_attached) return;
  _change_pipe(dev->pipe, DT_DEV_PIPE_SYNCH);
}

void dt_dev_pixelpipe_resync_history_preview_real(dt_develop_t *dev)
{
  if(IS_NULL_PTR(dev) || !dev->gui_attached) return;
  _change_pipe(dev->preview_pipe, DT_DEV_PIPE_SYNCH);
  // Virtual pipe mirrors preview history for GUI coordinate transforms.
  _sync_virtual_pipe(dev, DT_DEV_PIPE_SYNCH);
}

void dt_dev_pixelpipe_resync_history_all_real(dt_develop_t *dev)
{
  if(IS_NULL_PTR(dev) || !dev->gui_attached) return;
  dt_dev_pixelpipe_resync_history_preview(dev);
  dt_dev_pixelpipe_resync_history_main(dev);
}

void dt_dev_pixelpipe_update_history_main_real(dt_develop_t *dev)
{
  if(IS_NULL_PTR(dev) || !dev->gui_attached) return;
  _change_pipe(dev->pipe, DT_DEV_PIPE_TOP_CHANGED);
}

void dt_dev_pixelpipe_update_history_preview_real(dt_develop_t *dev)
{
  if(IS_NULL_PTR(dev) || !dev->gui_attached) return;
  _change_pipe(dev->preview_pipe, DT_DEV_PIPE_TOP_CHANGED);
  // Virtual pipe mirrors preview history for GUI coordinate transforms.
  _sync_virtual_pipe(dev, DT_DEV_PIPE_TOP_CHANGED);
}

void dt_dev_pixelpipe_update_history_all_real(dt_develop_t *dev)
{
  if(IS_NULL_PTR(dev) || !dev->gui_attached) return;
  dt_dev_pixelpipe_update_history_preview(dev);
  dt_dev_pixelpipe_update_history_main(dev);
}

void dt_dev_pixelpipe_update_zoom_preview_real(dt_develop_t *dev)
{
  if(IS_NULL_PTR(dev) || !dev->gui_attached) return;
  _change_pipe(dev->preview_pipe, DT_DEV_PIPE_ZOOMED);
  // Keep the virtual pipe aligned with preview ROI/zoom changes for GUI transforms.
  _sync_virtual_pipe(dev, DT_DEV_PIPE_ZOOMED);
}

void dt_dev_pixelpipe_update_zoom_main_real(dt_develop_t *dev)
{
  if(IS_NULL_PTR(dev) || !dev->gui_attached) return;
  /* Zoom/pan updates must run through the normal validity/ROI flow.
   * If a module left the main pipe in realtime mode, force it back to
   * standard mode here so zoom changes cannot get visually stuck on a
   * best-effort backbuffer policy. */
  dt_dev_pixelpipe_set_realtime(dev->pipe, FALSE);
  _change_pipe(dev->pipe, DT_DEV_PIPE_ZOOMED);
  _sync_virtual_pipe(dev, DT_DEV_PIPE_ZOOMED);
}

void dt_dev_pixelpipe_reset_all(dt_develop_t *dev)
{
  dt_dev_pixelpipe_cache_flush(darktable.pixelpipe_cache, -1);
  if(darktable.gui->reset || IS_NULL_PTR(dev) || !dev->gui_attached) return;
  dt_dev_pixelpipe_rebuild_all(dev);
}

void dt_dev_pixelpipe_change_zoom_main(dt_develop_t *dev)
{
  if (IS_NULL_PTR(dev) || !dev->gui_attached) return;
  /* Entering a zoom/pan change always exits realtime rendering policy.
   * Realtime is stroke-scoped and should never control darkroom navigation. */
  dt_dev_pixelpipe_set_realtime(dev->pipe, FALSE);
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


void dt_dev_pixelpipe_get_roi_out(dt_dev_pixelpipe_t *pipe,
                                  const int width_in, const int height_in,
                                  int *width, int *height)
{
  dt_iop_roi_t roi_in = (dt_iop_roi_t){ 0, 0, width_in, height_in, 1.0 };
  dt_iop_roi_t roi_out = roi_in;
  gchar *pipe_name = NULL;
  if(darktable.unmuted & DT_DEBUG_PIPE)
    pipe_name = _get_debug_pipe_name(pipe, pipe->dev);

  for(GList *nodes = g_list_first(pipe->nodes); nodes; nodes = g_list_next(nodes))
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
    dt_iop_module_t *module = piece->module;

    piece->buf_in = roi_in;

    // If in GUI and using a module that needs a full, undistorterted image,
    // we need to shutdown temporarily any module distorting the image.
    if(dt_dev_pixelpipe_activemodule_disables_currentmodule(pipe->dev, module))
      piece->enabled = FALSE;

    // If module is disabled, modify_roi_out() is a no-op
    if(piece->enabled)
      module->modify_roi_out(module, pipe, piece, &roi_out, &roi_in);
    else
      roi_out = roi_in;

    // Forward ROI planning answers "what output rectangle does this module
    // produce from the previous one ?". Logging the tuple here makes each
    // module-local geometry change visible on `-d pipe`.
    if(piece->enabled && (darktable.unmuted & DT_DEBUG_PIPE))
      dt_print(DT_DEBUG_PIPE,
               "[roi-out] pipe=%-15s module=%-18s enabled=%d in =(x=%5d y=%5d w=%5d h=%5d scale=%2.2f)"
               " out=(x=%5d y=%5d w=%5d h=%5d scale=%2.2f)\n",
               pipe_name, module->op, piece->enabled,
               roi_in.x, roi_in.y, roi_in.width, roi_in.height, roi_in.scale,
               roi_out.x, roi_out.y, roi_out.width, roi_out.height, roi_out.scale);

    piece->buf_out = roi_out;
    roi_in = roi_out;
  }

  if(pipe_name) dt_free(pipe_name);
  *width = roi_out.width;
  *height = roi_out.height;
}

void dt_dev_pixelpipe_get_roi_in(dt_dev_pixelpipe_t *pipe, const struct dt_iop_roi_t roi_out)
{
  // while module->modify_roi_out describes how the current module will change the size of
  // the output buffer depending on its parameters (pretty intuitive),
  // module->modify_roi_in describes "how much material" the current module needs from the previous one,
  // because some modules (lens correction) need a padding on their input.
  // The tricky part is therefore that the effect of the current module->modify_roi_in() needs to be repercuted
  // upstream in the pipeline for proper pipeline cache invalidation, so we need to browse the pipeline
  // backwards.

  // The virtual pipe is expected to be ready before calling this.
  // This function no longer supports NULL pipes or ad-hoc temp nodes.

  dt_iop_roi_t roi_out_temp = roi_out;
  dt_iop_roi_t roi_in;
  gchar *pipe_name = NULL;
  if(darktable.unmuted & DT_DEBUG_PIPE)
    pipe_name = _get_debug_pipe_name(pipe, pipe->dev);
  for(GList *nodes = g_list_last(pipe->nodes); nodes; nodes = g_list_previous(nodes))
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
    dt_iop_module_t *module = piece->module;

    piece->roi_out = roi_out_temp;

    // If in GUI and using a module that needs a full, undistorterted image,
    // we need to shutdown temporarily any module distorting the image.
    if(dt_dev_pixelpipe_activemodule_disables_currentmodule(pipe->dev, module))
      piece->enabled = FALSE;

    // If module is disabled, modify_roi_in() is a no-op
    if(piece->enabled)
      module->modify_roi_in(module, pipe, piece, &roi_out_temp, &roi_in);
    else
      roi_in = roi_out_temp;

    // Backward ROI planning answers "how much input rectangle does this
    // module need from upstream to deliver the requested downstream output ?".
    // Logging that request before and after modify_roi_in() makes ROI growth
    // and padding traceable module-by-module on `-d pipe`.
    if(piece->enabled && (darktable.unmuted & DT_DEBUG_PIPE))
      dt_print(DT_DEBUG_PIPE,
               "[roi-in ] pipe=%-15s module=%-18s enabled=%d out=(x=%5d y=%5d w=%5d h=%5d scale=%2.2f)"
               " in=(x=%5d y=%5d w=%5d h=%5d scale=%2.2f)\n",
               pipe_name, module->op, piece->enabled,
               roi_out_temp.x, roi_out_temp.y, roi_out_temp.width, roi_out_temp.height, roi_out_temp.scale,
               roi_in.x, roi_in.y, roi_in.width, roi_in.height, roi_in.scale);

    piece->roi_in = roi_in;
    roi_out_temp = roi_in;
  }

  if(pipe_name) dt_free(pipe_name);

  /* ROI planning runs backwards, but rawprepare seals the effective Bayer/X-Trans phase only once
   * the real input crop is known. Forward that authored RAW descriptor now so the downstream RAW
   * modules process with the same CFA layout that rawprepare just computed for this run. */
  dt_iop_buffer_dsc_t upstream_dsc = pipe->dev->image_storage.dsc;
  for(GList *nodes = g_list_first(pipe->nodes); nodes; nodes = g_list_next(nodes))
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;

    if(piece->dsc_in.cst == IOP_CS_RAW && piece->dsc_in.channels == 1)
    {
      piece->dsc_in.filters = upstream_dsc.filters;
      memcpy(piece->dsc_in.xtrans, upstream_dsc.xtrans, sizeof(piece->dsc_in.xtrans));
    }

    if(piece->dsc_out.cst == IOP_CS_RAW && piece->dsc_out.channels == 1
       && (!piece->enabled || strcmp(piece->module->op, "rawprepare")))
    {
      piece->dsc_out.filters = piece->dsc_in.filters;
      memcpy(piece->dsc_out.xtrans, piece->dsc_in.xtrans, sizeof(piece->dsc_out.xtrans));
    }

    upstream_dsc = piece->dsc_out;
  }

}

static uint64_t _default_pipe_hash(dt_dev_pixelpipe_t *pipe)
{
  // Start with a hash that is unique, image-wise.
  return dt_hash(5381, (const char *)&pipe->dev->image_storage.filename, DT_MAX_FILENAME_LEN);
}

uint64_t dt_dev_pixelpipe_node_hash(dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece, 
                                    const dt_iop_roi_t roi_out, const int pos)
{
  // to be called at runtime, not at pipe init.

  // Only at the first step of pipe, we don't have a module because we init the base buffer.
  if(!IS_NULL_PTR(piece))
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

const dt_dev_pixelpipe_iop_t *dt_dev_pixelpipe_get_module_piece(const dt_dev_pixelpipe_t *pipe,
                                                                const dt_iop_module_t *module)
{
  if(IS_NULL_PTR(pipe) || IS_NULL_PTR(module)) return NULL;

  for(GList *node = g_list_first(pipe->nodes); node; node = g_list_next(node))
  {
    dt_dev_pixelpipe_iop_t *const piece = node->data;
    if(piece && piece->enabled && piece->module == module)
      return piece;
  }

  return NULL;
}

const dt_dev_pixelpipe_iop_t *dt_dev_pixelpipe_get_prev_enabled_piece(const dt_dev_pixelpipe_t *pipe,
                                                                      const dt_dev_pixelpipe_iop_t *piece)
{
  if(IS_NULL_PTR(pipe) || IS_NULL_PTR(piece)) return NULL;

  GList *node = g_list_find(pipe->nodes, (gpointer)piece);
  if(IS_NULL_PTR(node)) return NULL;

  for(node = g_list_previous(node); node; node = g_list_previous(node))
  {
    dt_dev_pixelpipe_iop_t *const previous = node->data;
    if(previous && previous->enabled)
      return previous;
  }

  return NULL;
}

gboolean dt_dev_pixelpipe_cache_peek_gui(dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece,
                                         void **data, dt_pixel_cache_entry_t **cache_entry,
                                         dt_dev_pixelpipe_cache_wait_t *wait,
                                         dt_dev_pixelpipe_cache_ready_callback_t restart,
                                         gpointer restart_data)
{
  if(!IS_NULL_PTR(data)) *data = NULL;
  if(!IS_NULL_PTR(cache_entry)) *cache_entry = NULL;
  if(IS_NULL_PTR(pipe)) return FALSE;

  // Module-output cache requests are not satisfiable when the target piece
  // itself bypasses cache retention. Re-queueing those from GUI redraws would
  // keep transient edit modes in a self-feeding recompute loop.
  // Upstream targets (for example color-picker input sampling) stay allowed even if a
  // downstream piece enabled bypass mode.
  if(!IS_NULL_PTR(piece)
     && (dt_dev_pixelpipe_get_realtime(pipe) || pipe->no_cache || piece->bypass_cache))
    return FALSE;

  const uint64_t hash = !IS_NULL_PTR(piece) ? piece->global_hash : dt_dev_pixelpipe_get_hash(pipe);

  // For the final backbuffer (piece == NULL), GUI consumers display the last *published* frame,
  // identified by pipe->backbuf.hash. The pipeline plans the next frame ahead of publishing it, so
  // pipe->hash (the planned final hash) runs ahead of the backbuffer while a recompute is in flight.
  // This is the normal steady state of realtime drawing: every heartbeat plans a new frame. Peeking
  // the planned hash would then miss the perfectly valid published frame, the darkroom main-surface
  // lock would fail, and the view would drop to the paused preview pipe (which lacks the in-progress
  // stroke) — the flicker between "drawing applied" and "original". So look up the published backbuf
  // hash for display, and keep the planned hash only to drive recompute on a genuine miss below.
  uint64_t display_hash = hash;
  if(IS_NULL_PTR(piece))
  {
    const uint64_t backbuf_hash = dt_dev_backbuf_get_hash(&pipe->backbuf);
    if(backbuf_hash != DT_PIXELPIPE_CACHE_HASH_INVALID) display_hash = backbuf_hash;
  }

  void *buffer = NULL;
  dt_pixel_cache_entry_t *entry = NULL;
  // GUI consumers run on the CPU and never own the OpenCL device lock, so we pass preferred_devid = -1:
  // dt_dev_pixelpipe_cache_peek() then returns host-resident cachelines directly but refuses to
  // materialize device-only ones from the GPU (which would enqueue hidden GPU work from the GUI thread,
  // racing whichever pipeline thread currently owns the device — the clReleaseEvent crash). A device-only
  // entry is reported as a miss, so the request below waits for the pipeline to publish a host copy
  // (modules whose output the GUI samples, e.g. initialscale, already cache to RAM).
  if(display_hash != DT_PIXELPIPE_CACHE_HASH_INVALID
     && dt_dev_pixelpipe_cache_peek(darktable.pixelpipe_cache, display_hash, &buffer, &entry, -1, NULL)
     &&  !IS_NULL_PTR(buffer) && !IS_NULL_PTR(entry))
  {
    // These counters are only diagnostic; cache consumers should not wait for
    // queue lifecycle updates before reopening an already available cacheline.
    if(!dt_pthread_mutex_trylock(&_cache_wait_manager.lock))
    {
      _cache_wait_manager.immediate_hits++;
      dt_pthread_mutex_unlock(&_cache_wait_manager.lock);
    }
    dt_dev_pixelpipe_cache_wait_cleanup(wait, "peek-gui-immediate-hit");
    if(!IS_NULL_PTR(data)) *data = buffer;
    if(!IS_NULL_PTR(cache_entry)) *cache_entry = entry;
    return TRUE;
  }

  // A missed GUI peek already requests cache publication below. The miss counter
  // is diagnostic, so it must not block redraw/picker paths under contention.
  if(!dt_pthread_mutex_trylock(&_cache_wait_manager.lock))
  {
    _cache_wait_manager.misses++;
    dt_pthread_mutex_unlock(&_cache_wait_manager.lock);
  }

  gboolean request_cacheline = TRUE;
  if(!IS_NULL_PTR(wait) && !IS_NULL_PTR(restart) && hash != DT_PIXELPIPE_CACHE_HASH_INVALID)
  {
    const gboolean changed_target = !wait->connected
                                    || wait->pipe != pipe
                                    || wait->module != (!IS_NULL_PTR(piece) ? piece->module : NULL)
                                    || wait->hash != hash
                                    || wait->restart != restart;
    if(changed_target)
    {
      gboolean activate_wait_cursor = FALSE;
      dt_dev_pixelpipe_cache_wait_cleanup(wait, "peek-gui-target-changed");
      wait->pipe = pipe;
      wait->module = !IS_NULL_PTR(piece) ? piece->module : NULL;
      wait->hash = hash;
      wait->restart = restart;
      wait->user_data = restart_data;
      if(IS_NULL_PTR(wait->owner_tag))
        wait->owner_tag = !IS_NULL_PTR(piece) && !IS_NULL_PTR(piece->module) ? piece->module->op : "gui-backbuf";
      if(IS_NULL_PTR(wait->owner_object))
        wait->owner_object = restart_data;
      wait->connected = TRUE;

      dt_pthread_mutex_lock(&_cache_wait_manager.lock);
      wait->request_id = _cache_wait_manager.next_request_id++;
      _cache_wait_manager.queued_requests++;
      dt_dev_pixelpipe_cache_wait_record_t *record = calloc(1, sizeof(*record));
      record->wait = wait;
      record->request_id = wait->request_id;
      record->queued_at_us = g_get_monotonic_time();
      _cache_wait_manager.pending = g_list_prepend(_cache_wait_manager.pending, record);
      if(!_cache_wait_manager.connected)
      {
        DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_CACHELINE_READY,
                                        G_CALLBACK(_dt_dev_pixelpipe_cache_wait_ready_callback), NULL);
        _cache_wait_manager.connected = TRUE;
      }
      if(!_cache_wait_manager.wait_cursor_active)
      {
        _cache_wait_manager.wait_cursor_active = TRUE;
        activate_wait_cursor = TRUE;
      }
      dt_pthread_mutex_unlock(&_cache_wait_manager.lock);
      if(activate_wait_cursor)
        g_main_context_invoke(NULL, _cache_wait_cursor_progress, NULL);

      dt_print(DT_DEBUG_PIPECACHE,
               "[cache-wait] queued id=%" PRIu64 " owner=%s hash=%" PRIu64 " target=%s\n",
               wait->request_id,
               !IS_NULL_PTR(wait->owner_tag) ? wait->owner_tag : "(unknown)",
               wait->hash,
               !IS_NULL_PTR(wait->module) ? wait->module->op : "backbuf");
    }
    else
    {
      /* The GUI already waits for this exact cacheline. Re-emitting CACHE_REQUEST from
       * each expose keeps retrying an unsatisfied target forever when the pipeline cannot
       * publish it, for example after an OpenCL memory pre-check failure on very large images. */
      request_cacheline = FALSE;
    }
  }

  if(request_cacheline)
  {
    dt_dev_pixelpipe_set_cache_request(pipe,
                                       !IS_NULL_PTR(piece) ? DT_DEV_PIXELPIPE_CACHE_REQUEST_MODULE
                                                           : DT_DEV_PIXELPIPE_CACHE_REQUEST_BACKBUF,
                                       !IS_NULL_PTR(piece) ? piece->module : NULL);
    dt_dev_pixelpipe_or_changed(pipe, DT_DEV_PIPE_CACHE_REQUEST);

    dt_print(DT_DEBUG_DEV, "[pixelpipe/gui] request host cache pipe=%s target=%s hash=%" PRIu64 "\n",
             dt_pixelpipe_get_pipe_name(pipe->type),
             !IS_NULL_PTR(piece) && !IS_NULL_PTR(piece->module) ? piece->module->op : "backbuf", hash);
  }

  return FALSE;
}

/**
 * @brief Serve queued GUI cache waiters matching one published cacheline hash.
 *
 * `DT_SIGNAL_CACHELINE_READY` may wake several GUI consumers at once (pickers,
 * histograms, darkroom surfaces, autoset). We therefore:
 * 1. iterate the pending wait list under the manager lock,
 * 2. extract only waiters whose requested hash matches @p hash,
 * 3. disconnect the global signal and clear the busy cursor when the queue
 *    becomes empty,
 * 4. release the lock before running restart callbacks.
 *
 * Keeping callbacks out of the critical section is intentional: restart
 * handlers can queue redraws or request new cachelines, so running them under
 * lock would serialize unrelated GUI wakeups and risks re-entrant lock
 * contention.
 */
static void _dt_dev_pixelpipe_cache_wait_ready_callback(gpointer instance, const guint64 hash,
                                                        gpointer user_data)
{
  GList *to_restart = NULL;
  gboolean restore_wait_cursor = FALSE;

  dt_pthread_mutex_lock(&_cache_wait_manager.lock);
  for(GList *iter = _cache_wait_manager.pending; iter; )
  {
    GList *next = g_list_next(iter);
    dt_dev_pixelpipe_cache_wait_record_t *record = iter->data;
    dt_dev_pixelpipe_cache_wait_t *wait = !IS_NULL_PTR(record) ? record->wait : NULL;
    if(!IS_NULL_PTR(wait) && wait->connected && wait->hash == hash)
    {
      _cache_wait_manager.pending = g_list_delete_link(_cache_wait_manager.pending, iter);
      _cache_wait_manager.served_requests++;
      wait->connected = FALSE;
      to_restart = g_list_prepend(to_restart, wait);
      dt_free(record);
    }
    iter = next;
  }

  if(IS_NULL_PTR(_cache_wait_manager.pending) && _cache_wait_manager.connected)
  {
    DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals,
                                       G_CALLBACK(_dt_dev_pixelpipe_cache_wait_ready_callback), NULL);
    _cache_wait_manager.connected = FALSE;
  }
  if(IS_NULL_PTR(_cache_wait_manager.pending) && _cache_wait_manager.wait_cursor_active)
  {
    _cache_wait_manager.wait_cursor_active = FALSE;
    restore_wait_cursor = TRUE;
  }
  dt_pthread_mutex_unlock(&_cache_wait_manager.lock);
  if(restore_wait_cursor)
    g_main_context_invoke(NULL, _cache_wait_cursor_restore, NULL);

  for(GList *iter = to_restart; iter; iter = g_list_next(iter))
  {
    dt_dev_pixelpipe_cache_wait_t *wait = iter->data;
    const dt_dev_pixelpipe_cache_ready_callback_t restart = wait->restart;
    gpointer restart_data = wait->user_data;

    dt_print(DT_DEBUG_PIPECACHE,
             "[cache-wait] served id=%" PRIu64 " owner=%s hash=%" PRIu64 " target=%s\n",
             wait->request_id,
             !IS_NULL_PTR(wait->owner_tag) ? wait->owner_tag : "(unknown)",
             hash,
             !IS_NULL_PTR(wait->module) ? wait->module->op : "backbuf");

    wait->pipe = NULL;
    wait->module = NULL;
    wait->hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
    wait->restart = NULL;
    wait->user_data = NULL;
    wait->request_id = 0;

    if(!IS_NULL_PTR(restart)) restart(restart_data);
  }
  g_list_free(to_restart);
}

void dt_dev_pixelpipe_cache_wait_cleanup(dt_dev_pixelpipe_cache_wait_t *wait, const char *reason)
{
  if(IS_NULL_PTR(wait) || !wait->connected) return;
  gboolean restore_wait_cursor = FALSE;
  int64_t queued_at_us = 0;
  const uint64_t request_id = wait->request_id;
  const uint64_t hash = wait->hash;
  const char *owner_tag = wait->owner_tag;
  const dt_iop_module_t *module = wait->module;
  const char *cancel_reason = !IS_NULL_PTR(reason) ? reason : "unspecified";

  dt_pthread_mutex_lock(&_cache_wait_manager.lock);
  dt_dev_pixelpipe_cache_wait_record_t *record = _cache_wait_manager_remove_wait_locked(wait);
  if(!IS_NULL_PTR(record))
  {
    queued_at_us = record->queued_at_us;
    dt_free(record);
  }
  _cache_wait_manager.cancelled_requests++;
  if(IS_NULL_PTR(_cache_wait_manager.pending) && _cache_wait_manager.connected)
  {
    DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals,
                                       G_CALLBACK(_dt_dev_pixelpipe_cache_wait_ready_callback), NULL);
    _cache_wait_manager.connected = FALSE;
  }
  if(IS_NULL_PTR(_cache_wait_manager.pending) && _cache_wait_manager.wait_cursor_active)
  {
    _cache_wait_manager.wait_cursor_active = FALSE;
    restore_wait_cursor = TRUE;
  }
  dt_pthread_mutex_unlock(&_cache_wait_manager.lock);
  if(restore_wait_cursor)
    g_main_context_invoke(NULL, _cache_wait_cursor_restore, NULL);

  const int64_t age_ms = queued_at_us > 0 ? MAX((g_get_monotonic_time() - queued_at_us) / 1000, 0) : -1;
  dt_print(DT_DEBUG_PIPECACHE,
           "[cache-wait] cancelled id=%" PRIu64 " owner=%s hash=%" PRIu64
           " target=%s age_ms=%" PRId64 " reason=%s\n",
           request_id,
           !IS_NULL_PTR(owner_tag) ? owner_tag : "(unknown)",
           hash,
           !IS_NULL_PTR(module) ? module->op : "backbuf",
           age_ms,
           cancel_reason);

  wait->pipe = NULL;
  wait->module = NULL;
  wait->hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
  wait->restart = NULL;
  wait->user_data = NULL;
  wait->request_id = 0;
  wait->connected = FALSE;
}

void dt_dev_pixelpipe_cache_wait_set_owner(dt_dev_pixelpipe_cache_wait_t *wait,
                                           const char *owner_tag,
                                           gpointer owner_object)
{
  if(IS_NULL_PTR(wait)) return;
  wait->owner_tag = owner_tag;
  wait->owner_object = owner_object;
}

/* Establish the *input* side of a piece's buffer contract from the upstream descriptor by
 * asking the module's input_format(). This does NOT decide compatibility or finalize the
 * output: the single authoritative pass dt_dev_pixelpipe_propagate_formats() owns the mismatch/auto-disable
 * decision so it is taken once, on the fully-committed and consistently-threaded chain, never
 * on the stale partial state a synch_top / realtime edit can leave behind (issue #733).
 *
 * It is split out from that pass because commit_params() of a few modules (exposure, highlights,
 * invert, temperature) reads piece->dsc_in (filters, processed_maximum), so the commit loop must
 * set dsc_in before committing. */
// Short colorspace name for the [dsc] format-propagation debug log (-d pipe). Knowing the cst of
// each node's input/output is essential to spot when the pixelpipe's automatic RGB<->Lab conversion
// should fire: a Lab-domain module whose dsc_in.cst is not "lab" will silently consume RGB.
static const char *_dsc_cst_name(const int cst)
{
  switch(cst)
  {
    case IOP_CS_RAW:         return "raw";
    case IOP_CS_LAB:         return "lab";
    case IOP_CS_RGB:         return "rgb";
    case IOP_CS_RGB_DISPLAY: return "display-rgb";
    case IOP_CS_LCH:         return "lch";
    case IOP_CS_HSL:         return "hsl";
    case IOP_CS_JZCZHZ:      return "jzczhz";
    case IOP_CS_NONE:        return "none";
    default:                 return "?";
  }
}

static void _prepare_piece_input_contract(dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                                          const dt_iop_buffer_dsc_t *upstream_dsc)
{
  piece->dsc_in = *upstream_dsc;
  piece->dsc_out = *upstream_dsc;
  dt_iop_buffer_dsc_update_bpp(&piece->dsc_in);
  dt_iop_buffer_dsc_update_bpp(&piece->dsc_out);

  /* Disabled modules are strict pass-through stages. Their advertised contracts must
   * not rewrite the upstream descriptor, otherwise a disabled RGB-only module can make
   * downstream RAW stages such as demosaic believe the stream was already converted. */
  if(!piece->enabled) return;

  piece->module->input_format(piece->module, pipe, piece, &piece->dsc_in);
  dt_iop_buffer_dsc_update_bpp(&piece->dsc_in);
  piece->dsc_out = piece->dsc_in;
  dt_iop_buffer_dsc_update_bpp(&piece->dsc_out);
}

static void _commit_piece_contract(dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                                   dt_iop_params_t *params, dt_develop_blend_params_t *blend_params,
                                   dt_iop_buffer_dsc_t *upstream_dsc)
{
  const dt_iop_buffer_dsc_t actual_input_dsc = *upstream_dsc;

  // Set dsc_in before committing (commit_params() of some modules reads it). Compatibility and
  // the final output descriptor are settled later by dt_dev_pixelpipe_propagate_formats(), so this loop
  // never auto-disables a module on a possibly-stale upstream descriptor.
  _prepare_piece_input_contract(pipe, piece, upstream_dsc);

  // This should run even if the module is disabled because
  // some modules with no history self-enable based on runtime context
  dt_iop_commit_params(piece->module, params, blend_params, pipe, piece);

  if(piece->enabled)
  {
    piece->module->output_format(piece->module, pipe, piece, &piece->dsc_out);
    dt_iop_buffer_dsc_update_bpp(&piece->dsc_out);
  }
  else
  {
    piece->dsc_in = actual_input_dsc;
    piece->dsc_out = actual_input_dsc;
    dt_iop_buffer_dsc_update_bpp(&piece->dsc_in);
    dt_iop_buffer_dsc_update_bpp(&piece->dsc_out);
  }

  *upstream_dsc = piece->dsc_out;
}

/* Direction-independent forward format pass.
 *
 * The buffer-format contract (channels / datatype / colorspace / CFA presence flowing from one
 * module to the next) is conceptually independent of ROI planning, which runs in both
 * directions (end->start for a target size, start->end for the native size). Deriving the
 * contract inside the history-sync commit loop made it sensitive to partial syncs
 * (synch_top / realtime top edits): a downstream module could read a stale upstream descriptor
 * and get wrongly disabled for an "unexpected input buffer format" (issue #733). Worse, the
 * auto-disable is one-way, so a later pass could not undo a false positive.
 *
 * This single forward pass re-threads the whole chain from the input image descriptor after
 * params have been committed, and is the *only* place that disables a module for a genuinely
 * incompatible input. Disabling here is safe because the chain is consistent and fully threaded
 * from pipe->dev->image_storage.dsc. Only the first stage (basebuffer) reads the input image
 * type; every later node derives its contract solely from its upstream piece. The lone
 * ROI-dependent refinement, rawprepare's CFA phase shift, is finalized later in modify_roi_*().
 *
 * It is deliberately NON-destructive for compatible enabled modules: it verifies the input
 * contract and re-threads dsc_in, but keeps the dsc_out that commit_params() produced. dsc_out
 * carries runtime fields (processed_maximum, rawprepare black/white, temperature coeffs, CFA
 * phase) that input/output_format cannot reconstruct, so recomputing them here would wipe the
 * tonal scaling and render images black/white. */
void dt_dev_pixelpipe_propagate_formats(dt_dev_pixelpipe_t *pipe)
{
  if(IS_NULL_PTR(pipe)) return;

  dt_iop_buffer_dsc_t upstream_dsc = pipe->dev->image_storage.dsc;
  gchar *pipe_name = (darktable.unmuted & DT_DEBUG_PIPE) ? _get_debug_pipe_name(pipe, NULL) : NULL;

  for(GList *nodes = g_list_first(pipe->nodes); nodes; nodes = g_list_next(nodes))
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
    if(IS_NULL_PTR(piece) || IS_NULL_PTR(piece->module)) continue;

    const dt_iop_buffer_dsc_t actual_input_dsc = upstream_dsc;

    if(!piece->enabled)
    {
      // Disabled modules are strict pass-through stages.
      piece->dsc_in = actual_input_dsc;
      piece->dsc_out = actual_input_dsc;
      dt_iop_buffer_dsc_update_bpp(&piece->dsc_in);
      dt_iop_buffer_dsc_update_bpp(&piece->dsc_out);
    }
    else
    {
      // Ask the module what input it expects, into a scratch descriptor. We must NOT recompute
      // piece->dsc_out from input/output_format here: commit_params() already produced dsc_out
      // with runtime fields that those methods cannot reconstruct (processed_maximum, rawprepare
      // black/white, temperature coeffs, CFA phase). Recomputing would reset processed_maximum to
      // the input image's value and blow up downstream tone handling (black/white renders).
      dt_iop_buffer_dsc_t declared_in = actual_input_dsc;
      dt_iop_buffer_dsc_update_bpp(&declared_in);
      piece->module->input_format(piece->module, pipe, piece, &declared_in);
      dt_iop_buffer_dsc_update_bpp(&declared_in);

      // A module whose declared input does not match what the previous stage actually publishes
      // cannot run: disable it (in the pipe only; history is untouched) and turn it into a
      // pass-through so the contract keeps flowing to the next stage.
      const gboolean input_mismatch
          = (declared_in.bpp != actual_input_dsc.bpp
             || declared_in.channels != actual_input_dsc.channels
             || declared_in.filters != actual_input_dsc.filters);

      if(input_mismatch)
      {
        dt_control_log(_("disabled module `%s`: unexpected input buffer format"),
                       piece->module->name());
        dt_print(DT_DEBUG_PIPE,
                 "[pixelpipe] disabling module %s because input format expects %" G_GSIZE_FORMAT
                 " B/px, %u channels, filters %u but upstream publishes %" G_GSIZE_FORMAT
                 " B/px, %u channels, filters %u\n",
                 piece->module->op, declared_in.bpp, declared_in.channels, declared_in.filters,
                 actual_input_dsc.bpp, actual_input_dsc.channels, actual_input_dsc.filters);

        piece->enabled = FALSE;
        piece->hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
        piece->blendop_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
        piece->global_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
        piece->global_mask_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
        piece->dsc_in = actual_input_dsc;
        piece->dsc_out = actual_input_dsc;
        dt_iop_buffer_dsc_update_bpp(&piece->dsc_in);
        dt_iop_buffer_dsc_update_bpp(&piece->dsc_out);
      }
      else
      {
        // Input is compatible: publish the module's DECLARED input descriptor. `declared_in` was
        // seeded from the upstream descriptor and then run through input_format(), so it already
        // carries the upstream runtime fields (processed_maximum, rawprepare, temperature, CFA
        // phase) AND the colorspace/channel layout the module actually consumes.
        //
        // Critically, we must NOT fall back to `actual_input_dsc` here: that would reset dsc_in.cst
        // to the upstream colorspace and make it equal to the buffer's cst, so the pixelpipe's
        // automatic colorspace conversion (pixelpipe_cpu.c / pixelpipe_gpu.c) would believe no
        // conversion is needed. Lab-domain modules (old color balance, color checker, atrous, ...)
        // inserted after an RGB stage would then receive RGB data interpreted as Lab and render
        // garbled/solid colors. The full-resync path keeps dsc_in from input_format() for the same
        // reason; this incremental (synch_top) path must match it.
        piece->dsc_in = declared_in;
        dt_iop_buffer_dsc_update_bpp(&piece->dsc_in);
      }
    }

    if(pipe_name)
      dt_print(DT_DEBUG_PIPE,
               "[dsc] pipe=%s module=%s enabled=%d in=(cst=%s ch=%i bpp=%" G_GSIZE_FORMAT
               " filters=%u) out=(cst=%s ch=%i bpp=%" G_GSIZE_FORMAT " filters=%u)\n",
               pipe_name, piece->module->op, piece->enabled,
               _dsc_cst_name(piece->dsc_in.cst), piece->dsc_in.channels, piece->dsc_in.bpp, piece->dsc_in.filters,
               _dsc_cst_name(piece->dsc_out.cst), piece->dsc_out.channels, piece->dsc_out.bpp, piece->dsc_out.filters);

    upstream_dsc = piece->dsc_out;
  }

  dt_free(pipe_name);
}

static void _sync_pipe_nodes_from_history(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, const uint32_t history_end,
                                          const char *debug_label)
{
  dt_iop_buffer_dsc_t upstream_dsc = pipe->dev->image_storage.dsc;
  const gboolean previous_want_detail_mask = (pipe->want_detail_mask != DT_DEV_DETAIL_MASK_NONE);

  for(GList *nodes = g_list_first(pipe->nodes); nodes; nodes = g_list_next(nodes))
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
    if(IS_NULL_PTR(piece)) continue;

    piece->hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
    piece->blendop_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
    piece->global_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
    piece->global_mask_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
    piece->enabled = piece->module->default_enabled;
    piece->detail_mask = FALSE;

    dt_iop_params_t *params = piece->module->default_params;
    dt_develop_blend_params_t *blend_params = piece->module->default_blendop_params;
    gboolean found_history = FALSE;

    for(GList *history = g_list_nth(dev->history, history_end - 1);
        history;
        history = g_list_previous(history))
    {
      dt_dev_history_item_t *hist = (dt_dev_history_item_t *)history->data;
      if(piece->module == hist->module)
      {
        piece->enabled = hist->enabled;
        params = hist->params;
        blend_params = hist->blend_params;
        found_history = TRUE;
        break;
      }
    }

    piece->detail_mask = blend_params && blend_params->details != 0.0f;
    if(!strcmp(piece->module->op, "detailmask"))
      piece->enabled = (pipe->want_detail_mask == DT_DEV_DETAIL_MASK_ENABLED);
    _commit_piece_contract(pipe, piece, params, blend_params, &upstream_dsc);

    if(!found_history)
      dt_print(DT_DEBUG_PARAMS, "[pixelpipe] info: committed default params for %s (%s) in pipe %s\n",
               piece->module->op, piece->module->multi_name, debug_label);
  }

  _refresh_pipe_detail_mask_state(pipe);
  if(previous_want_detail_mask != (pipe->want_detail_mask != DT_DEV_DETAIL_MASK_NONE))
  {
    GList *detailmask_node = _find_detailmask_node(pipe);
    if(detailmask_node)
      _sync_pipe_nodes_from_history_from_node(pipe, history_end, detailmask_node, debug_label);
  }
}

static void _sync_pipe_nodes_from_history_from_node(dt_dev_pixelpipe_t *pipe,
                                                    const uint32_t history_end, GList *start_node,
                                                    const char *debug_label)
{
  if(IS_NULL_PTR(pipe) || IS_NULL_PTR(start_node)) return;

  dt_iop_buffer_dsc_t upstream_dsc = pipe->dev->image_storage.dsc;
  const gboolean previous_want_detail_mask = (pipe->want_detail_mask != DT_DEV_DETAIL_MASK_NONE);
  for(GList *node = g_list_first(pipe->nodes); node && node != start_node; node = g_list_next(node))
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)node->data;
    if(IS_NULL_PTR(piece)) continue;

    upstream_dsc = piece->dsc_out;
  }

  for(GList *nodes = start_node; nodes; nodes = g_list_next(nodes))
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
    if(IS_NULL_PTR(piece)) continue;

    piece->hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
    piece->blendop_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
    piece->global_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
    piece->global_mask_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;

    piece->enabled = piece->module->default_enabled;
    piece->detail_mask = FALSE;

    dt_iop_params_t *params = piece->module->default_params;
    dt_develop_blend_params_t *blend_params = piece->module->default_blendop_params;
    gboolean found_history = FALSE;

    for(GList *history = g_list_nth(pipe->dev->history, history_end - 1);
        history;
        history = g_list_previous(history))
    {
      dt_dev_history_item_t *hist = (dt_dev_history_item_t *)history->data;
      if(piece->module == hist->module)
      {
        piece->enabled = hist->enabled;
        params = hist->params;
        blend_params = hist->blend_params;
        found_history = TRUE;
        break;
      }
    }

    piece->detail_mask = blend_params && blend_params->details != 0.0f;
    if(!strcmp(piece->module->op, "detailmask"))
      piece->enabled = (pipe->want_detail_mask == DT_DEV_DETAIL_MASK_ENABLED);
    _commit_piece_contract(pipe, piece, params, blend_params, &upstream_dsc);

    if(!found_history)
      dt_print(DT_DEBUG_PARAMS, "[pixelpipe] info: committed default params for %s (%s) in pipe %s\n",
               piece->module->op, piece->module->multi_name, debug_label);
  }

  _refresh_pipe_detail_mask_state(pipe);
  if(previous_want_detail_mask != (pipe->want_detail_mask != DT_DEV_DETAIL_MASK_NONE))
  {
    GList *detailmask_node = _find_detailmask_node(pipe);
    if(detailmask_node && detailmask_node != start_node)
      _sync_pipe_nodes_from_history_from_node(pipe, history_end, detailmask_node, debug_label);
  }
}

void dt_pixelpipe_get_global_hash(dt_dev_pixelpipe_t *pipe)
{
  /* Traverse the pipeline node by node and compute the cumulative (global) hash of each module.
  *  This hash takes into account the hashes of the previous modules and the size of the current ROI.
  *  It is used to map pipeline cache states to current parameters.
  *  It represents the state of internal modules params as well as their position in the pipe and their output size.
  *  It is to be called at pipe init, not at runtime.
  */

  // bernstein hash (djb2)
  uint64_t hash = _default_pipe_hash(pipe);
  gboolean passthrough_preview = FALSE;

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

    // Some GUI previews author their final display inside the active module,
    // then runtime forwards that exact buffer through later pass-through stages.
    // Keep the planned hash contract aligned with the published cacheline so the
    // GUI does not keep requesting a downstream output hash that will never exist.
    if(passthrough_preview
       && !(piece->module->operation_tags() & IOP_TAG_DISTORT)
       && (piece->dsc_in.bpp == piece->dsc_out.bpp)
       && !memcmp(&piece->roi_in, &piece->roi_out, sizeof(dt_iop_roi_t)))
    {
      piece->global_mask_hash = hash;
      piece->global_hash = hash;
      continue;
    }

    // Panning and zooming change the ROI. Some GUI modes (crop in editing mode) too.
    // dt_dev_get_roi_in() should have run before
    local_hash = dt_hash(local_hash, (const char *)&piece->roi_in, sizeof(dt_iop_roi_t));
    local_hash = dt_hash(local_hash, (const char *)&piece->roi_out, sizeof(dt_iop_roi_t));

    local_hash = dt_hash(local_hash, (const char *)&piece->dsc_in, sizeof(dt_iop_buffer_dsc_t));
    local_hash = dt_hash(local_hash, (const char *)&piece->dsc_out, sizeof(dt_iop_buffer_dsc_t));

/*
    fprintf(stdout, "start->end : %-17s | ROI in: %4ix%-4i @%2.4f | ROI out: %4ix%-4i @%2.4f\n", piece->module->op,
            piece->buf_in.width, piece->buf_in.height, piece->buf_in.scale, piece->buf_out.width,
            piece->buf_out.height, piece->buf_out.scale);
    fprintf(stdout, "end->start : %-17s | ROI in: %4ix%-4i @%2.4f | ROI out: %4ix%-4i @%2.4f\n", piece->module->op,
            piece->roi_in.width, piece->roi_in.height, piece->planned_roi_in.scale,
            piece->roi_out.width, piece->roi_out.height, piece->roi_out.scale);
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
    int bypass_distort = dt_dev_pixelpipe_activemodule_disables_currentmodule(pipe->dev, piece->module);
    local_hash = dt_hash(local_hash, (const char *)&bypass_distort, sizeof(int));

    // If the cache bypass is on, the corresponding cache lines will be freed immediately after use,
    // we need to track that. It somewhat overlaps module->request_mask_display, but...
    local_hash = dt_hash(local_hash, (const char *)&piece->bypass_cache, sizeof(gboolean));

    // Update global hash for this stage
    hash = dt_hash(hash, (const char *)&local_hash, sizeof(uint64_t));

    if(darktable.unmuted & DT_DEBUG_VERBOSE)
    {
      gchar *type = _get_debug_pipe_name(pipe, pipe->dev);
      dt_print(DT_DEBUG_PIPE, "[pixelpipe] global hash for %20s (%s) in pipe %s with hash %lu\n",
               piece->module->op, piece->module->multi_name, type, (long unsigned int)hash);
      dt_free(type);
    }
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

    if(pipe->type == DT_DEV_PIXELPIPE_FULL
       && pipe->dev->gui_attached
       && (piece->module == pipe->dev->gui_module)
       && (piece->module->request_mask_display != DT_DEV_PIXELPIPE_DISPLAY_NONE))
    {
      passthrough_preview = TRUE;
    }
  }

  // The pipe hash is the hash of its last module.
  dt_dev_pixelpipe_set_hash(pipe, hash);
  pipe->bypass_cache = bypass_cache;
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
void dt_dev_pixelpipe_synch_all_real(dt_dev_pixelpipe_t *pipe, const char *caller_func)
{
  gchar *type = _get_debug_pipe_name(pipe, pipe->dev);
  dt_print(DT_DEBUG_DEV, "[pixelpipe] synch all modules with history for pipe %s called from %s\n", type, caller_func);

  const uint32_t history_end = dt_dev_get_history_end_ext(pipe->dev);
  _sync_pipe_nodes_from_history(pipe, pipe->dev, history_end, type);

  // Keep track of the last history item to have been synced
  GList *last_item = g_list_nth(pipe->dev->history, history_end - 1);
  if(last_item)
  {
    dt_dev_history_item_t *last_hist = (dt_dev_history_item_t *)last_item->data;
    pipe->last_history_hash = last_hist->hash;
    pipe->last_history_item = last_hist;
  }
  else
  {
    pipe->last_history_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
    pipe->last_history_item = NULL;
  }

  dt_free(type);
}

void dt_dev_pixelpipe_synch_top(dt_dev_pixelpipe_t *pipe)
{
  gchar *type = _get_debug_pipe_name(pipe, pipe->dev);

  dt_print(DT_DEBUG_DEV, "[pixelpipe] synch top modules with history for pipe %s\n", type);

  const uint32_t history_end = dt_dev_get_history_end_ext(pipe->dev);
  GList *last_item = g_list_nth(pipe->dev->history, history_end - 1);
  if(last_item)
  {
    GList *first_item = NULL;
    for(GList *history = last_item; history; history = g_list_previous(history))
    {
      dt_dev_history_item_t *hist = (dt_dev_history_item_t *)history->data;
      first_item = history;

      if(hist->hash == pipe->last_history_hash || hist == pipe->last_history_item)
        break;
    }

    GList *fence_item = g_list_nth(pipe->dev->history, history_end);
    for(GList *history = first_item; history && history != fence_item; history = g_list_next(history))
    {
      dt_dev_history_item_t *hist = (dt_dev_history_item_t *)history->data;
      if(!hist || !hist->module) continue;
      dt_print(DT_DEBUG_PARAMS, "[pixelpipe] synch top history module `%s` (%s) for pipe %s\n",
               hist->module->op, hist->module->multi_name, type);
      for(GList *nodes = g_list_last(pipe->nodes); nodes; nodes = g_list_previous(nodes))
      {
        dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
        if(IS_NULL_PTR(piece) || piece->module != hist->module) continue;

        _sync_pipe_nodes_from_history_from_node(pipe, history_end, nodes, type);
        break;
      }
    }

    dt_dev_history_item_t *last_hist = (dt_dev_history_item_t *)last_item->data;
    pipe->last_history_hash = last_hist->hash;
    pipe->last_history_item = last_hist;
  }
  else
  {
    dt_print(DT_DEBUG_DEV, "[pixelpipe] synch top history module missing error for pipe %s\n", type);
    pipe->last_history_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
    pipe->last_history_item = NULL;
  }

  dt_free(type);
}

// Modules without history need to be resynced unconditionnally with their internal params
// because some of them are self-enabled/disabled from commit_params() methods
void dt_dev_pixelpipe_sync_no_history(dt_dev_pixelpipe_t *pipe)
{
  for(GList *nodes = g_list_first(pipe->nodes); nodes; nodes = g_list_next(nodes))
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
    if(IS_NULL_PTR(piece) || IS_NULL_PTR(piece->module)) continue;
    dt_iop_module_t *module = piece->module;
    if(module->flags() & IOP_FLAGS_NO_HISTORY_STACK)
      dt_iop_commit_params(module, module->default_params, module->default_blendop_params, pipe, piece);
  }
}

void dt_dev_pixelpipe_change(dt_dev_pixelpipe_t *pipe)
{
  dt_times_t start;
  dt_get_times(&start);

  /**
   * Consume and clear pending change flags in one atomic step.
   *
   * The previous get-then-store sequence could lose updates when another
   * thread OR-ed a new flag between the load and the reset. That made history
   * resync requests randomly disappear, especially under fast GUI actions such
   * as toggles or consecutive parameter commits.
   */
  dt_dev_pixelpipe_change_t status
      = (dt_dev_pixelpipe_change_t)dt_atomic_exch_int((dt_atomic_int *)&pipe->changed, DT_DEV_PIPE_UNCHANGED);

  gchar *type = _get_debug_pipe_name(pipe, pipe->dev);
  char *status_str = g_strdup_printf("%s%s%s%s%s%s%s",
                                  (status & DT_DEV_PIPE_UNCHANGED) ? "UNCHANGED " : "",
                                  (status & DT_DEV_PIPE_REMOVE) ? "REMOVE " : "",
                                  (status & DT_DEV_PIPE_TOP_CHANGED) ? "TOP_CHANGED " : "",
                                  (status & DT_DEV_PIPE_SYNCH) ? "SYNCH " : "",
                                  (status & DT_DEV_PIPE_ZOOMED) ? "ZOOMED " : "",
                                  (status & DT_DEV_PIPE_CACHE_REQUEST) ? "CACHE_REQUEST " : "",
                                  (status & DT_DEV_PIPE_REENTRY) ? "REENTRY " : "");

  dt_print(DT_DEBUG_DEV, "[dt_dev_pixelpipe_change] pipeline state changing for pipe %s, flag %s\n",
     type, status_str);

  dt_free(status_str);

  // mask display off as a starting point
  pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
  pipe->bypass_blendif = 0;

  /* Zoom/pan only replans ROI and hashes, it does not replay history sync.
   * Rebuild the aggregate detail-mask demand from the already synchronized
   * pieces in that case, otherwise the pipe forgets that the hidden
   * `detailmask` stage is needed before the next processing run starts. */
  if(status & (DT_DEV_PIPE_REMOVE | DT_DEV_PIPE_SYNCH | DT_DEV_PIPE_TOP_CHANGED))
    pipe->want_detail_mask = DT_DEV_DETAIL_MASK_NONE;
  else
    _refresh_pipe_detail_mask_state(pipe);

  dt_pthread_rwlock_rdlock(&pipe->dev->history_mutex);

  // The realtime in-place path settles the format contract and the global hash itself (it must,
  // since that hash is cumulative over enabled nodes); the other paths defer it to the
  // unconditional pass below.
  gboolean formats_propagated = FALSE;

  // case DT_DEV_PIPE_UNCHANGED: case DT_DEV_PIPE_ZOOMED:
  if(status & DT_DEV_PIPE_REMOVE)
  {
    // modules have been added in between or removed. need to rebuild the whole pipeline.
    if(pipe->nodes) dt_dev_pixelpipe_cleanup_nodes(pipe);
    dt_dev_pixelpipe_create_nodes(pipe);
    dt_dev_pixelpipe_sync_no_history(pipe);
    dt_dev_pixelpipe_synch_all(pipe);
  }
  else if(status & DT_DEV_PIPE_SYNCH)
  {
    // pipeline topology remains intact, only change all params.
    dt_dev_pixelpipe_sync_no_history(pipe);
    dt_dev_pixelpipe_synch_all(pipe);
  }
  else if(status & DT_DEV_PIPE_TOP_CHANGED)
  {
    // only top history item(s) changed, or a focused module published transient/realtime params
    if(_sync_focused_in_place(pipe))
    {
      formats_propagated = TRUE;
    }
    else
    {
      dt_dev_pixelpipe_sync_no_history(pipe);
      dt_dev_pixelpipe_synch_top(pipe);
    }
  }
  else // DT_DEV_PIPE_ZOOMED DT_DEV_PIPE_CACHE_REQUEST DT_DEV_PIPE_REENTRY
  {
    // Finalscale will need to self-enable/disable depending on zoom level
    dt_dev_pixelpipe_sync_no_history(pipe);
  }
  dt_dev_pixelpipe_set_history_hash(pipe, dt_dev_get_history_hash(pipe->dev));
  dt_pthread_rwlock_unlock(&pipe->dev->history_mutex);

  // Re-establish the buffer-format contract of the whole chain once, after whichever sync path
  // (full, top-only or realtime-in-place) committed params. This is the single authoritative,
  // direction-independent place where per-node dsc_in/dsc_out are settled and incompatible
  // modules are disabled (issue #733), so the OpenCL cache policy and ROI planning below see a
  // consistent contract. The realtime path already did this before computing its global hash.
  if(!formats_propagated)
    dt_dev_pixelpipe_propagate_formats(pipe);

  _seal_opencl_cache_policy(pipe);
  
  // Update theoritical final scale based on distorting modules
  // This also writes piece->buf_in/out for each pipe->nodes piece,
  // so it's not nearly a matter of getting processed_width/height
  dt_dev_pixelpipe_get_roi_out(pipe, pipe->iwidth, pipe->iheight, &pipe->processed_width,
                               &pipe->processed_height);

  dt_show_times_f(&start, "[dev_pixelpipe] pipeline resync with history", "for pipe %s", type);
  dt_free(type);
}

static void _sync_virtual_pipe(dt_develop_t *dev, dt_dev_pixelpipe_change_t flag)
{
  // Virtual pipe exists only for GUI geometry (ROI/mask transforms) and never processes pixels.
  if(IS_NULL_PTR(dev) || !dev->gui_attached || IS_NULL_PTR(dev->virtual_pipe)) return;
  if(!dev->roi.raw_inited || dev->image_storage.id <= 0) return;

  // Ensure its input image metadata matches the current dev state.
  if(dev->virtual_pipe->imgid != dev->image_storage.id
     || dev->virtual_pipe->iwidth != dev->roi.raw_width
     || dev->virtual_pipe->iheight != dev->roi.raw_height
     || dev->virtual_pipe->dev->image_storage.id != dev->image_storage.id)
  {
    dt_dev_pixelpipe_set_input(dev->virtual_pipe, dev->image_storage.id,
                               dev->roi.raw_width, dev->roi.raw_height, 1.0f, DT_MIPMAP_FULL);
  }

  // Mirror the preview-pipe change flags and commit immediately.
  _change_pipe(dev->virtual_pipe, flag);
  dt_dev_pixelpipe_change(dev->virtual_pipe);
}

void dt_dev_pixelpipe_sync_virtual(dt_develop_t *dev, dt_dev_pixelpipe_change_t flag)
{
  _sync_virtual_pipe(dev, flag);
}

gboolean dt_dev_pixelpipe_is_backbufer_valid(dt_dev_pixelpipe_t *pipe)
{
  return dt_dev_backbuf_get_hash(&pipe->backbuf) != DT_PIXELPIPE_CACHE_HASH_INVALID
         && dt_dev_pixelpipe_get_hash(pipe) == dt_dev_backbuf_get_hash(&pipe->backbuf)
         && dt_dev_get_history_hash(pipe->dev) == dt_dev_backbuf_get_history_hash(&pipe->backbuf)
         && dt_dev_backbuf_get_history_hash(&pipe->backbuf) == dt_dev_get_history_hash(pipe->dev);
}

gboolean dt_dev_pixelpipe_is_pipeline_valid(dt_dev_pixelpipe_t *pipe)
{
  return dt_dev_get_history_hash(pipe->dev) == dt_dev_pixelpipe_get_history_hash(pipe);
}
