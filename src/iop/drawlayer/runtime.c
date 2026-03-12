/*
    This file is part of the Ansel project.
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

#include "develop/pixelpipe_cache.h"
#include "control/control.h"
#include "iop/drawlayer/runtime.h"

#include <string.h>

static gboolean _ensure_external_patch_buffer(dt_drawlayer_cache_patch_t *patch, const int width, const int height,
                                              const char *name)
{
  if(!patch || width <= 0 || height <= 0) return FALSE;
  if(patch->pixels && patch->width == width && patch->height == height) return TRUE;

  dt_drawlayer_cache_patch_clear(patch, name);
  patch->x = 0;
  patch->y = 0;
  patch->width = width;
  patch->height = height;
  patch->pixels = dt_drawlayer_cache_alloc_temp_buffer((size_t)width * height * 4 * sizeof(float), name);
  patch->external_alloc = TRUE;
  if(!patch->pixels) return FALSE;
  patch->cache_entry = dt_dev_pixelpipe_cache_ref_entry_for_host_ptr(darktable.pixelpipe_cache, patch->pixels);
  patch->cache_hash = patch->cache_entry ? patch->cache_entry->hash : DT_PIXELPIPE_CACHE_HASH_INVALID;
  return patch->cache_entry != NULL;
}

static void _copy_patch_rect(const dt_drawlayer_cache_patch_t *src, dt_drawlayer_cache_patch_t *dst,
                             const dt_drawlayer_damaged_rect_t *rect)
{
  if(!src || !dst || !rect || !rect->valid || !src->pixels || !dst->pixels) return;

  const int x0 = CLAMP(rect->nw[0], 0, MIN(src->width, dst->width));
  const int y0 = CLAMP(rect->nw[1], 0, MIN(src->height, dst->height));
  const int x1 = CLAMP(rect->se[0], 0, MIN(src->width, dst->width));
  const int y1 = CLAMP(rect->se[1], 0, MIN(src->height, dst->height));
  if(x1 <= x0 || y1 <= y0) return;

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) dt_omp_firstprivate(src, dst, x0, x1, y0, y1)
#endif
  for(int y = y0; y < y1; y++)
  {
    const float *src_row = src->pixels + 4 * ((size_t)y * src->width + x0);
    float *dst_row = dst->pixels + 4 * ((size_t)y * dst->width + x0);
    memcpy(dst_row, src_row, (size_t)(x1 - x0) * 4 * sizeof(float));
  }
}

static void _sync_buffer_state(dt_drawlayer_runtime_manager_t *state, const dt_drawlayer_runtime_buffer_t buffer,
                               const gboolean resident, const gboolean valid, const gboolean dirty)
{
  if(!state || buffer >= DT_DRAWLAYER_RUNTIME_BUFFER_COUNT) return;
  state->buffers[buffer].resident = resident;
  state->buffers[buffer].valid = valid;
  state->buffers[buffer].dirty = dirty;
}

void dt_drawlayer_runtime_manager_init(dt_drawlayer_runtime_manager_t *state)
{
  if(!state) return;
  memset(state, 0, sizeof(*state));
  dt_pthread_mutex_init(&state->mutex, NULL);
}

void dt_drawlayer_runtime_manager_cleanup(dt_drawlayer_runtime_manager_t *state)
{
  if(!state) return;
  dt_pthread_mutex_destroy(&state->mutex);
  memset(state, 0, sizeof(*state));
}

void dt_drawlayer_runtime_manager_note_buffer_lock(dt_drawlayer_runtime_manager_t *state,
                                                   const dt_drawlayer_runtime_buffer_t buffer,
                                                   const dt_drawlayer_runtime_actor_t actor,
                                                   const gboolean write_lock,
                                                   const gboolean acquire)
{
  if(!state || buffer >= DT_DRAWLAYER_RUNTIME_BUFFER_COUNT) return;
  dt_pthread_mutex_lock(&state->mutex);
  dt_drawlayer_runtime_buffer_state_t *entry = &state->buffers[buffer];

  if(write_lock)
  {
    entry->write_locked = acquire;
    entry->writer = acquire ? actor : DT_DRAWLAYER_RUNTIME_ACTOR_NONE;
  }
  else if(acquire)
  {
    entry->read_locks++;
    entry->last_reader = actor;
  }
  else if(entry->read_locks > 0)
  {
    entry->read_locks--;
    if(entry->read_locks == 0) entry->last_reader = DT_DRAWLAYER_RUNTIME_ACTOR_NONE;
  }
  dt_pthread_mutex_unlock(&state->mutex);
}

void dt_drawlayer_runtime_manager_note_sidecar_io(dt_drawlayer_runtime_manager_t *state, const gboolean active)
{
  if(!state) return;
  dt_pthread_mutex_lock(&state->mutex);
  state->sidecar_io_active = active;
  state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_TIFF_IO].active = active;
  state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_TIFF_IO].waiting = FALSE;
  dt_pthread_mutex_unlock(&state->mutex);
}

void dt_drawlayer_runtime_manager_note_thread(dt_drawlayer_runtime_manager_t *state,
                                              const dt_drawlayer_runtime_actor_t actor,
                                              const gboolean active,
                                              const gboolean waiting,
                                              const guint queued)
{
  if(!state || actor <= DT_DRAWLAYER_RUNTIME_ACTOR_NONE || actor >= DT_DRAWLAYER_RUNTIME_ACTOR_COUNT) return;
  dt_pthread_mutex_lock(&state->mutex);
  state->threads[actor].active = active;
  state->threads[actor].waiting = waiting;
  state->threads[actor].queued = queued;
  dt_pthread_mutex_unlock(&state->mutex);
}

static void _fill_runtime_inputs(const dt_drawlayer_runtime_context_t *runtime,
                                 const dt_drawlayer_worker_snapshot_t *worker_snapshot,
                                 dt_drawlayer_runtime_inputs_t *inputs)
{
  if(inputs) *inputs = (dt_drawlayer_runtime_inputs_t){ 0 };
  if(!runtime || !inputs) return;

  const dt_drawlayer_runtime_request_t *const request = &runtime->runtime;
  dt_iop_module_t *const self = request->self;
  dt_iop_drawlayer_gui_data_t *const g = request->gui;
  dt_drawlayer_process_state_t *const process = request->process_state ? request->process_state : (g ? &g->process : NULL);
  const dt_iop_drawlayer_params_t *const runtime_params
      = request->runtime_params ? request->runtime_params : (const dt_iop_drawlayer_params_t *)self->params;

  *inputs = (dt_drawlayer_runtime_inputs_t){
    .session = g ? &g->session : NULL,
    .process = process,
    .stroke = g ? &g->stroke : NULL,
    .worker = worker_snapshot,
    .base_patch = NULL,
    .base_patch_valid = FALSE,
    .base_patch_dirty = FALSE,
    .painting_active = g && g->manager.painting_active,
    .gui_attached = self && self->dev && self->dev->gui_attached && g,
    .module_focused = self && self->dev && self->dev->gui_module == self,
    .display_pipe = request->display_pipe,
    .have_layer_selection = runtime_params && runtime_params->layer_name[0] != '\0',
    .have_valid_output_roi = request->roi_out && request->roi_out->width > 0 && request->roi_out->height > 0,
    .use_opencl = request->use_opencl,
    .view_changed = g && self && self->dev
                    && (fabsf(g->session.last_view_x - self->dev->roi.x) > 1e-6f
                        || fabsf(g->session.last_view_y - self->dev->roi.y) > 1e-6f
                        || fabsf(g->session.last_view_scale - self->dev->roi.scaling) > 1e-6f),
    .padding_changed = g && self && fabsf(g->session.live_padding - dt_drawlayer_current_live_padding(self)) > 1e-6f,
  };
}

static void _collect_runtime_inputs(const dt_drawlayer_runtime_update_request_t *request,
                                    const dt_drawlayer_runtime_context_t *context,
                                    dt_drawlayer_runtime_inputs_t *inputs,
                                    dt_drawlayer_worker_snapshot_t *worker_snapshot)
{
  if(inputs) *inputs = (dt_drawlayer_runtime_inputs_t){ 0 };
  if(worker_snapshot) *worker_snapshot = (dt_drawlayer_worker_snapshot_t){ 0 };
  if(request && request->inputs)
  {
    if(inputs) *inputs = *request->inputs;
    return;
  }
  if(context && context->runtime.gui && context->runtime.gui->stroke.worker && worker_snapshot)
    dt_drawlayer_worker_get_snapshot(context->runtime.gui->stroke.worker, worker_snapshot);
  _fill_runtime_inputs(context, worker_snapshot, inputs);
}

static void _sync_runtime_state_from_inputs(dt_drawlayer_runtime_manager_t *state,
                                            const dt_drawlayer_runtime_inputs_t *inputs)
{
  const dt_drawlayer_session_state_t *session = inputs ? inputs->session : NULL;
  const dt_drawlayer_process_state_t *process = inputs ? inputs->process : NULL;
  const dt_drawlayer_worker_snapshot_t *worker = inputs ? inputs->worker : NULL;
  const dt_drawlayer_cache_patch_t *base_patch = inputs ? inputs->base_patch : NULL;

  state->painting_active = inputs && inputs->painting_active;
  state->background_job_running = session && session->background_job_running;

  if(process)
  {
    state->layer_cache_valid = process->cache_valid;
    state->process_patch_dirty = process->process_patch_dirty;
    state->process_snapshot_valid = process->process_patch_valid && process->process_snapshot_valid
                                    && process->process_read_patch.pixels
                                    && process->process_read_patch.width > 0
                                    && process->process_read_patch.height > 0;
    state->process_cl_valid
#ifdef HAVE_OPENCL
        = process->process_read_clmem != NULL;
#else
        = FALSE;
#endif
    _sync_buffer_state(state, DT_DRAWLAYER_RUNTIME_BUFFER_BASE_PATCH, process->base_patch.pixels != NULL,
                       process->cache_valid, process->cache_dirty);
    _sync_buffer_state(state, DT_DRAWLAYER_RUNTIME_BUFFER_PROCESS_PATCH, process->process_patch.pixels != NULL,
                       process->process_patch_valid, process->process_patch_dirty);
    _sync_buffer_state(state, DT_DRAWLAYER_RUNTIME_BUFFER_PROCESS_SNAPSHOT,
                       process->process_read_patch.pixels != NULL, state->process_snapshot_valid,
                       process->process_patch_dirty);
    _sync_buffer_state(state, DT_DRAWLAYER_RUNTIME_BUFFER_PROCESS_CL, state->process_cl_valid,
                       state->process_cl_valid, FALSE);
    _sync_buffer_state(state, DT_DRAWLAYER_RUNTIME_BUFFER_STROKE_MASK, process->stroke_mask.pixels != NULL,
                       process->stroke_mask.pixels != NULL, FALSE);
    _sync_buffer_state(state, DT_DRAWLAYER_RUNTIME_BUFFER_PROCESS_STROKE_MASK,
                       process->process_stroke_mask.pixels != NULL, process->process_stroke_mask.pixels != NULL,
                       FALSE);
  }
  else if(base_patch)
  {
    state->layer_cache_valid = inputs->base_patch_valid;
    _sync_buffer_state(state, DT_DRAWLAYER_RUNTIME_BUFFER_BASE_PATCH, base_patch->pixels != NULL,
                       inputs->base_patch_valid, inputs->base_patch_dirty);
  }

  if(worker)
  {
    const gboolean backend_started = worker->backend_state != DT_DRAWLAYER_WORKER_STATE_STOPPED;
    const gboolean backend_busy = worker->backend_state == DT_DRAWLAYER_WORKER_STATE_BUSY;
    const gboolean backend_waiting = (worker->backend_state == DT_DRAWLAYER_WORKER_STATE_PAUSING
                                      || worker->backend_state == DT_DRAWLAYER_WORKER_STATE_PAUSED);
    const gboolean fullres_started = worker->fullres_state != DT_DRAWLAYER_WORKER_STATE_STOPPED;
    const gboolean fullres_busy = worker->fullres_state == DT_DRAWLAYER_WORKER_STATE_BUSY;
    const gboolean backend_active = backend_started
                                    && (backend_busy || worker->backend_queue_count > 0
                                        || worker->commit_pending || state->painting_active);
    const gboolean fullres_active = fullres_started
                                    && (fullres_busy || worker->fullres_queue_count > 0);
    state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_RASTER_BACKEND].active = backend_active;
    state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_RASTER_BACKEND].waiting = backend_waiting;
    state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_RASTER_BACKEND].queued = worker->backend_queue_count;
    state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_RASTER_FULLRES].active = fullres_active;
    state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_RASTER_FULLRES].waiting = FALSE;
    state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_RASTER_FULLRES].queued = worker->fullres_queue_count;
  }
}

static void _apply_runtime_event(dt_drawlayer_runtime_manager_t *state,
                                 const dt_drawlayer_runtime_update_request_t *request,
                                 const dt_drawlayer_runtime_inputs_t *inputs)
{
  if(!state || !request) return;

  state->last_event = request->event;
  state->last_raw_input_kind = request->raw_input_kind;

  switch(request->event)
  {
    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_FOCUS_GAIN:
      state->gui_focused = TRUE;
      state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_GUI].active = TRUE;
      state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_GUI].waiting = FALSE;
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_FOCUS_LOSS:
      state->gui_focused = FALSE;
      state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_GUI].active = FALSE;
      state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_GUI].waiting = FALSE;
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_MOUSE_ENTER:
      state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_GUI].active = TRUE;
      state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_GUI].waiting = FALSE;
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_MOUSE_LEAVE:
    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_SCROLL:
    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_CHANGE_IMAGE:
    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_RESYNC:
    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_PIPE_FINISHED:
    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_STROKE_ABORT:
      state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_GUI].active = inputs && inputs->gui_attached;
      state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_GUI].waiting = FALSE;
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_RAW_INPUT:
      state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_GUI].active
          = state->gui_focused && inputs && inputs->gui_attached;
      state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_GUI].waiting = FALSE;
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_PROCESS_CPU_BEFORE:
      state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_PIPELINE_CPU].active = TRUE;
      state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_PIPELINE_CPU].waiting = FALSE;
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_PROCESS_CPU_AFTER:
      state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_PIPELINE_CPU].active = FALSE;
      state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_PIPELINE_CPU].waiting = FALSE;
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_PROCESS_CL_BEFORE:
      state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_PIPELINE_CL].active = TRUE;
      state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_PIPELINE_CL].waiting = FALSE;
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_PROCESS_CL_AFTER:
      state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_PIPELINE_CL].active = FALSE;
      state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_PIPELINE_CL].waiting = FALSE;
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_SIDECAR_LOAD_BEGIN:
    case DT_DRAWLAYER_RUNTIME_EVENT_SIDECAR_SAVE_BEGIN:
      state->sidecar_io_active = TRUE;
      state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_TIFF_IO].active = TRUE;
      state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_TIFF_IO].waiting = FALSE;
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_SIDECAR_LOAD_END:
    case DT_DRAWLAYER_RUNTIME_EVENT_SIDECAR_SAVE_END:
      state->sidecar_io_active = FALSE;
      state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_TIFF_IO].active = FALSE;
      state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_TIFF_IO].waiting = FALSE;
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_COMMIT_BEGIN:
      state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_GUI].active = TRUE;
      state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_GUI].waiting = FALSE;
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_COMMIT_END:
      state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_GUI].active
          = state->gui_focused && inputs && inputs->gui_attached;
      state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_GUI].waiting = FALSE;
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_NONE:
    default:
      break;
  }

  if(request->event == DT_DRAWLAYER_RUNTIME_EVENT_GUI_RAW_INPUT)
  {
    if(request->raw_input_kind == DT_DRAWLAYER_RUNTIME_RAW_INPUT_STROKE_BEGIN)
      state->painting_active = TRUE;
    else if(request->raw_input_kind == DT_DRAWLAYER_RUNTIME_RAW_INPUT_STROKE_END)
      state->painting_active = FALSE;
  }
  else if(request->event == DT_DRAWLAYER_RUNTIME_EVENT_GUI_FOCUS_LOSS
          || request->event == DT_DRAWLAYER_RUNTIME_EVENT_GUI_CHANGE_IMAGE
          || request->event == DT_DRAWLAYER_RUNTIME_EVENT_GUI_STROKE_ABORT)
  {
    state->painting_active = FALSE;
  }
}

static void _update_realtime_state(dt_drawlayer_runtime_manager_t *state,
                                   const dt_drawlayer_runtime_update_request_t *request,
                                   const dt_drawlayer_runtime_inputs_t *inputs)
{
  if(!state) return;

  gboolean realtime_active = state->gui_focused && inputs && inputs->gui_attached
                             && state->painting_active;

  if(request)
  {
    switch(request->event)
    {
      case DT_DRAWLAYER_RUNTIME_EVENT_GUI_FOCUS_LOSS:
      case DT_DRAWLAYER_RUNTIME_EVENT_COMMIT_BEGIN:
      case DT_DRAWLAYER_RUNTIME_EVENT_GUI_CHANGE_IMAGE:
      case DT_DRAWLAYER_RUNTIME_EVENT_GUI_STROKE_ABORT:
        realtime_active = FALSE;
        break;

      case DT_DRAWLAYER_RUNTIME_EVENT_GUI_RAW_INPUT:
        realtime_active = state->gui_focused && inputs && inputs->gui_attached
                          && (request->raw_input_kind == DT_DRAWLAYER_RUNTIME_RAW_INPUT_STROKE_BEGIN
                              || request->raw_input_kind == DT_DRAWLAYER_RUNTIME_RAW_INPUT_SAMPLE);
        break;

      case DT_DRAWLAYER_RUNTIME_EVENT_NONE:
      case DT_DRAWLAYER_RUNTIME_EVENT_GUI_FOCUS_GAIN:
      case DT_DRAWLAYER_RUNTIME_EVENT_GUI_MOUSE_ENTER:
      case DT_DRAWLAYER_RUNTIME_EVENT_GUI_MOUSE_LEAVE:
      case DT_DRAWLAYER_RUNTIME_EVENT_GUI_SCROLL:
      case DT_DRAWLAYER_RUNTIME_EVENT_GUI_SYNC_TEMP_BUFFERS:
      case DT_DRAWLAYER_RUNTIME_EVENT_GUI_RESYNC:
      case DT_DRAWLAYER_RUNTIME_EVENT_PROCESS_CPU_BEFORE:
      case DT_DRAWLAYER_RUNTIME_EVENT_PROCESS_CPU_AFTER:
      case DT_DRAWLAYER_RUNTIME_EVENT_PROCESS_CL_BEFORE:
      case DT_DRAWLAYER_RUNTIME_EVENT_PROCESS_CL_AFTER:
      case DT_DRAWLAYER_RUNTIME_EVENT_SIDECAR_LOAD_BEGIN:
      case DT_DRAWLAYER_RUNTIME_EVENT_SIDECAR_LOAD_END:
      case DT_DRAWLAYER_RUNTIME_EVENT_SIDECAR_SAVE_BEGIN:
      case DT_DRAWLAYER_RUNTIME_EVENT_SIDECAR_SAVE_END:
      case DT_DRAWLAYER_RUNTIME_EVENT_COMMIT_END:
      default:
        break;
    }
  }

  state->realtime_active = realtime_active;
}

typedef struct dt_drawlayer_runtime_schedule_t
{
  dt_drawlayer_runtime_commit_mode_t commit_mode;
  gboolean sync_realtime_mode;
  dt_drawlayer_runtime_feedback_t feedback;
  gboolean ensure_worker_running;
  gboolean stop_worker;
  gboolean sync_widget_cache;
  gboolean prime_live_process_patch;
  gboolean queue_raw_input;
  gboolean request_commit;
  gboolean ensure_layer_cache;
  gboolean build_process_patch;
  gboolean flush_process_patch;
  gboolean wait_fullres_worker;
  gboolean release_process_clmem;
  gboolean flush_sidecar;
  gboolean set_pointer_state;
  gboolean pointer_valid;
  gboolean pointer_hide_cursor;
  gboolean queue_redraw_center;
  gboolean refresh_gui;
  gboolean invalidate_layer_cache;
  gboolean rasterization_busy;
} dt_drawlayer_runtime_schedule_t;

static void _update_manager_information(dt_drawlayer_runtime_manager_t *state,
                                        const dt_drawlayer_runtime_update_request_t *request,
                                        const dt_drawlayer_runtime_host_t *host,
                                        dt_drawlayer_runtime_schedule_t *schedule);

static gboolean _perform_runtime_commit_sequence(dt_drawlayer_runtime_manager_t *state,
                                                 const dt_drawlayer_runtime_update_request_t *request,
                                                 const dt_drawlayer_runtime_host_t *host,
                                                 const dt_drawlayer_runtime_commit_mode_t commit_mode,
                                                 dt_drawlayer_runtime_result_t *result)
{
  if(commit_mode == DT_DRAWLAYER_RUNTIME_COMMIT_NONE) return TRUE;
  const dt_drawlayer_runtime_context_t *const context
      = host ? (const dt_drawlayer_runtime_context_t *)host->user_data : NULL;
  if(!context || !context->runtime.self || !context->runtime.gui) return FALSE;

  const dt_drawlayer_runtime_update_request_t begin = {
    .event = DT_DRAWLAYER_RUNTIME_EVENT_COMMIT_BEGIN,
    .raw_input_kind = DT_DRAWLAYER_RUNTIME_RAW_INPUT_NONE,
    .inputs = request ? request->inputs : NULL,
  };
  _update_manager_information(state, &begin, host, NULL);
  dt_drawlayer_set_pipeline_realtime_mode(context->runtime.self, context->runtime.gui->manager.realtime_active);
  if(!dt_drawlayer_commit_dabs(context->runtime.self, commit_mode == DT_DRAWLAYER_RUNTIME_COMMIT_HISTORY)) return FALSE;

  const dt_drawlayer_runtime_update_request_t end = {
    .event = DT_DRAWLAYER_RUNTIME_EVENT_COMMIT_END,
    .raw_input_kind = DT_DRAWLAYER_RUNTIME_RAW_INPUT_NONE,
    .inputs = request ? request->inputs : NULL,
  };
  _update_manager_information(state, &end, host, NULL);
  dt_drawlayer_set_pipeline_realtime_mode(context->runtime.self, context->runtime.gui->manager.realtime_active);
  return TRUE;
}

static gboolean _perform_runtime_widget_cache_sync(const dt_drawlayer_runtime_host_t *host,
                                                   dt_drawlayer_runtime_result_t *result)
{
  const dt_drawlayer_runtime_context_t *const context
      = host ? (const dt_drawlayer_runtime_context_t *)host->user_data : NULL;
  (void)result;
  if(!context || !context->runtime.self) return FALSE;
  if(!dt_drawlayer_ensure_layer_cache(context->runtime.self)) return FALSE;
  return dt_drawlayer_sync_widget_cache(context->runtime.self);
}

static void _release_runtime_source(dt_drawlayer_runtime_manager_t *state,
                                    dt_drawlayer_process_state_t *process,
                                    dt_drawlayer_runtime_source_t *source);

static void _build_runtime_schedule(dt_drawlayer_runtime_manager_t *state,
                                    const dt_drawlayer_runtime_update_request_t *request,
                                    const dt_drawlayer_runtime_inputs_t *inputs,
                                    dt_drawlayer_runtime_schedule_t *schedule)
{
  if(schedule)
    *schedule = (dt_drawlayer_runtime_schedule_t){
      .commit_mode = DT_DRAWLAYER_RUNTIME_COMMIT_NONE,
      .feedback = DT_DRAWLAYER_RUNTIME_FEEDBACK_NONE,
    };
  const dt_drawlayer_process_state_t *process = inputs ? inputs->process : NULL;
  const dt_drawlayer_stroke_state_t *stroke = inputs ? inputs->stroke : NULL;
  if(!schedule || !inputs || !request) return;

  schedule->rasterization_busy
      = state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_RASTER_BACKEND].active
        || state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_RASTER_FULLRES].active;

  const gboolean backend_busy = state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_RASTER_BACKEND].active;
  const gboolean fullres_busy = state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_RASTER_FULLRES].active;
  const gboolean have_pending_stroke_work
      = (state->painting_active || (stroke && (stroke->finish_commit_pending || stroke->stroke_sample_count > 0)))
        || backend_busy || fullres_busy;
  const gboolean have_pending_gui_stroke_work
      = state->painting_active || (stroke && stroke->stroke_sample_count > 0);
  const gboolean have_pending_cache_writes
      = process && (process->cache_dirty || process->process_patch_dirty);

  switch(request->event)
  {
    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_FOCUS_GAIN:
      schedule->sync_realtime_mode = TRUE;
      schedule->ensure_worker_running = inputs->module_focused;
      schedule->sync_widget_cache = inputs->gui_attached && inputs->have_layer_selection;
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_FOCUS_LOSS:
      schedule->sync_realtime_mode = TRUE;
      schedule->feedback = schedule->rasterization_busy
                               ? DT_DRAWLAYER_RUNTIME_FEEDBACK_FOCUS_LOSS_WAIT
                               : DT_DRAWLAYER_RUNTIME_FEEDBACK_NONE;
      schedule->commit_mode = have_pending_stroke_work
                                  ? DT_DRAWLAYER_RUNTIME_COMMIT_QUIET
                                  : DT_DRAWLAYER_RUNTIME_COMMIT_NONE;
      schedule->wait_fullres_worker = schedule->rasterization_busy;
      schedule->flush_process_patch = process && process->process_patch_dirty;
      schedule->flush_sidecar = have_pending_cache_writes && process && process->cache_valid;
      schedule->stop_worker = TRUE;
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_MOUSE_ENTER:
      schedule->set_pointer_state = TRUE;
      schedule->pointer_valid = TRUE;
      schedule->pointer_hide_cursor = TRUE;
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_MOUSE_LEAVE:
      schedule->sync_realtime_mode = TRUE;
      schedule->set_pointer_state = TRUE;
      schedule->pointer_valid = FALSE;
      schedule->pointer_hide_cursor = FALSE;
      schedule->queue_redraw_center = TRUE;
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_SCROLL:
      schedule->commit_mode = have_pending_gui_stroke_work ? DT_DRAWLAYER_RUNTIME_COMMIT_QUIET
                                                           : DT_DRAWLAYER_RUNTIME_COMMIT_NONE;
      schedule->sync_widget_cache = TRUE;
      schedule->queue_redraw_center = TRUE;
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_SYNC_TEMP_BUFFERS:
      schedule->commit_mode = (request->flush_pending && have_pending_gui_stroke_work)
                                  ? DT_DRAWLAYER_RUNTIME_COMMIT_QUIET
                                  : DT_DRAWLAYER_RUNTIME_COMMIT_NONE;
      schedule->sync_widget_cache = inputs->gui_attached && inputs->have_layer_selection;
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_CHANGE_IMAGE:
      schedule->sync_realtime_mode = TRUE;
      schedule->commit_mode = have_pending_stroke_work
                                  ? DT_DRAWLAYER_RUNTIME_COMMIT_QUIET
                                  : DT_DRAWLAYER_RUNTIME_COMMIT_NONE;
      schedule->flush_process_patch = process && process->process_patch_dirty;
      schedule->flush_sidecar = process && process->cache_valid;
      schedule->stop_worker = TRUE;
      schedule->release_process_clmem = state->process_cl_valid;
      schedule->invalidate_layer_cache = TRUE;
      schedule->refresh_gui = TRUE;
      schedule->sync_widget_cache = TRUE;
      schedule->ensure_worker_running = inputs->module_focused;
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_RESYNC:
      schedule->sync_widget_cache = inputs->gui_attached && inputs->have_layer_selection;
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_PIPE_FINISHED:
      schedule->sync_widget_cache = (inputs->view_changed || inputs->padding_changed)
                                    && !state->painting_active && !schedule->rasterization_busy;
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_STROKE_ABORT:
      schedule->sync_realtime_mode = TRUE;
      schedule->request_commit = TRUE;
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_RAW_INPUT:
      switch(request->raw_input_kind)
      {
        case DT_DRAWLAYER_RUNTIME_RAW_INPUT_STROKE_BEGIN:
          schedule->sync_realtime_mode = TRUE;
          schedule->commit_mode = (!state->painting_active
                                   && ((stroke && (stroke->finish_commit_pending || stroke->stroke_sample_count > 0))
                                       || backend_busy))
                                      ? DT_DRAWLAYER_RUNTIME_COMMIT_HISTORY
                                      : DT_DRAWLAYER_RUNTIME_COMMIT_NONE;
          schedule->sync_widget_cache = TRUE;
          schedule->prime_live_process_patch = TRUE;
          schedule->ensure_worker_running = TRUE;
          schedule->queue_raw_input = TRUE;
          break;

        case DT_DRAWLAYER_RUNTIME_RAW_INPUT_SAMPLE:
          schedule->sync_realtime_mode = TRUE;
          schedule->ensure_worker_running = state->painting_active;
          schedule->queue_raw_input = state->painting_active;
          break;

        case DT_DRAWLAYER_RUNTIME_RAW_INPUT_STROKE_END:
          schedule->sync_realtime_mode = FALSE;
          schedule->ensure_worker_running = TRUE;
          schedule->request_commit = TRUE;
          schedule->queue_raw_input = TRUE;
          break;

        case DT_DRAWLAYER_RUNTIME_RAW_INPUT_NONE:
        default:
          schedule->sync_realtime_mode = TRUE;
          break;
      }
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_PROCESS_CPU_BEFORE:
      schedule->ensure_layer_cache = inputs->display_pipe && inputs->have_layer_selection
                                     && !state->layer_cache_valid;
      schedule->build_process_patch = inputs->display_pipe && inputs->have_layer_selection
                                      && inputs->have_valid_output_roi;
      schedule->release_process_clmem = state->process_cl_valid;
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_PROCESS_CL_BEFORE:
      schedule->ensure_layer_cache = inputs->display_pipe && inputs->have_layer_selection
                                     && !state->layer_cache_valid;
      schedule->build_process_patch = inputs->display_pipe && inputs->have_layer_selection
                                      && inputs->have_valid_output_roi;
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_SIDECAR_LOAD_BEGIN:
    case DT_DRAWLAYER_RUNTIME_EVENT_SIDECAR_LOAD_END:
    case DT_DRAWLAYER_RUNTIME_EVENT_SIDECAR_SAVE_BEGIN:
    case DT_DRAWLAYER_RUNTIME_EVENT_SIDECAR_SAVE_END:
    case DT_DRAWLAYER_RUNTIME_EVENT_COMMIT_BEGIN:
    case DT_DRAWLAYER_RUNTIME_EVENT_COMMIT_END:
    case DT_DRAWLAYER_RUNTIME_EVENT_PROCESS_CPU_AFTER:
    case DT_DRAWLAYER_RUNTIME_EVENT_PROCESS_CL_AFTER:
    case DT_DRAWLAYER_RUNTIME_EVENT_NONE:
    default:
      break;
  }
}

static void _update_manager_information(dt_drawlayer_runtime_manager_t *state,
                                        const dt_drawlayer_runtime_update_request_t *request,
                                        const dt_drawlayer_runtime_host_t *host,
                                        dt_drawlayer_runtime_schedule_t *schedule)
{
  if(schedule) *schedule = (dt_drawlayer_runtime_schedule_t){ 0 };
  if(!state) return;

  dt_drawlayer_runtime_inputs_t inputs = { 0 };
  dt_drawlayer_worker_snapshot_t worker_snapshot = { 0 };
  _collect_runtime_inputs(request, host ? (const dt_drawlayer_runtime_context_t *)host->user_data : NULL,
                          &inputs, &worker_snapshot);

  dt_pthread_mutex_lock(&state->mutex);
  _sync_runtime_state_from_inputs(state, &inputs);
  if(request) _apply_runtime_event(state, request, &inputs);
  _update_realtime_state(state, request, &inputs);
  _build_runtime_schedule(state, request, &inputs, schedule);
  dt_pthread_mutex_unlock(&state->mutex);
}

dt_drawlayer_runtime_result_t dt_drawlayer_runtime_manager_update(dt_drawlayer_runtime_manager_t *state,
                                                                  const dt_drawlayer_runtime_update_request_t *request,
                                                                  const dt_drawlayer_runtime_host_t *host)
{
  dt_drawlayer_runtime_result_t result = {
    .ok = TRUE,
    .raw_input_ok = TRUE,
  };
  if(!state || !request || !host) return result;
  const dt_drawlayer_runtime_context_t *const context
      = (const dt_drawlayer_runtime_context_t *)host->user_data;
  if(!context) return result;

  dt_iop_module_t *const self = context->runtime.self;
  dt_iop_drawlayer_gui_data_t *const g = context->runtime.gui;
  dt_drawlayer_process_state_t *const process = context->runtime.process_state;

  dt_drawlayer_runtime_schedule_t schedule = { 0 };
  _update_manager_information(state, request, host, &schedule);

  if(schedule.sync_realtime_mode && self && g)
    dt_drawlayer_set_pipeline_realtime_mode(self, g->manager.realtime_active);

  switch(request->event)
  {
    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_FOCUS_GAIN:
      if(schedule.ensure_worker_running && self && g
         && !dt_drawlayer_worker_ensure_running(self, g->stroke.worker))
      {
        dt_control_log(_("failed to start drawing worker"));
        result.ok = FALSE;
      }
      if(schedule.sync_widget_cache && result.ok
         && !_perform_runtime_widget_cache_sync(host, &result))
        result.ok = FALSE;
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_MOUSE_ENTER:
    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_MOUSE_LEAVE:
      if(schedule.set_pointer_state && g)
      {
        g->session.pointer_valid = schedule.pointer_valid;
        dt_drawlayer_set_os_cursor_hidden(schedule.pointer_hide_cursor);
      }
      if(schedule.queue_redraw_center) dt_control_queue_redraw_center();
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_SCROLL:
    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_SYNC_TEMP_BUFFERS:
    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_RESYNC:
      if(result.ok
         && !_perform_runtime_commit_sequence(state, request, host, schedule.commit_mode, &result))
        result.ok = FALSE;
      if(result.ok && schedule.sync_widget_cache
         && !_perform_runtime_widget_cache_sync(host, &result))
        result.ok = FALSE;
      if(schedule.queue_redraw_center) dt_control_queue_redraw_center();
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_PIPE_FINISHED:
      if(schedule.sync_widget_cache && !_perform_runtime_widget_cache_sync(host, &result)) result.ok = FALSE;
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_STROKE_ABORT:
      if(self) dt_drawlayer_end_gui_stroke_capture(self);
      if(schedule.request_commit && g) dt_drawlayer_worker_request_commit(g->stroke.worker);
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_FOCUS_LOSS:
    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_CHANGE_IMAGE:
      if(result.ok
         && !_perform_runtime_commit_sequence(state, request, host, schedule.commit_mode, &result))
        result.ok = FALSE;
      if(result.ok && schedule.feedback != DT_DRAWLAYER_RUNTIME_FEEDBACK_NONE && g)
        dt_drawlayer_show_runtime_feedback(g, schedule.feedback);
      if(schedule.wait_fullres_worker && g)
        dt_drawlayer_worker_flush_finished_strokes(g->stroke.worker);
      if(schedule.flush_process_patch && self && g)
        dt_drawlayer_flush_process_patch_to_base(self, g);
      if(schedule.flush_sidecar)
      {
        const dt_drawlayer_runtime_update_request_t begin = {
          .event = DT_DRAWLAYER_RUNTIME_EVENT_SIDECAR_SAVE_BEGIN,
          .raw_input_kind = DT_DRAWLAYER_RUNTIME_RAW_INPUT_NONE,
          .inputs = request->inputs,
        };
        _update_manager_information(state, &begin, host, NULL);
        if(self && !dt_drawlayer_flush_layer_cache(self))
        {
          dt_control_log(_("failed to write drawing layer sidecar"));
          result.ok = FALSE;
        }
        const dt_drawlayer_runtime_update_request_t end = {
          .event = DT_DRAWLAYER_RUNTIME_EVENT_SIDECAR_SAVE_END,
          .raw_input_kind = DT_DRAWLAYER_RUNTIME_RAW_INPUT_NONE,
          .inputs = request->inputs,
        };
        _update_manager_information(state, &end, host, NULL);
      }
      if(schedule.stop_worker && self && g) dt_drawlayer_worker_stop(self, g->stroke.worker);
#ifdef HAVE_OPENCL
      if(schedule.release_process_clmem && process)
      {
        dt_drawlayer_cache_patch_wrlock(&process->process_read_patch);
        dt_drawlayer_process_state_clear_clmem(process);
        dt_drawlayer_cache_patch_wrunlock(&process->process_read_patch);
      }
#endif
      if(schedule.invalidate_layer_cache && g)
      {
        dt_drawlayer_release_all_base_patch_extra_refs(g);
        dt_drawlayer_cache_patch_clear(&g->process.base_patch, "drawlayer patch");
        g->process.cache_valid = FALSE;
        g->process.cache_dirty = FALSE;
        dt_drawlayer_process_state_invalidate(&g->process);
      }
      if(schedule.refresh_gui && self) gui_update(self);
      if(schedule.sync_widget_cache && result.ok
         && !_perform_runtime_widget_cache_sync(host, &result))
        result.ok = FALSE;
      if(schedule.ensure_worker_running && self && g
         && !dt_drawlayer_worker_ensure_running(self, g->stroke.worker))
      {
        dt_control_log(_("failed to start drawing worker"));
        result.ok = FALSE;
      }
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_RAW_INPUT:
      if(request->raw_input_kind == DT_DRAWLAYER_RUNTIME_RAW_INPUT_STROKE_BEGIN)
      {
        if(result.ok
           && !_perform_runtime_commit_sequence(state, request, host, schedule.commit_mode, &result))
          result.ok = FALSE;
        if(result.ok && schedule.sync_widget_cache
           && !_perform_runtime_widget_cache_sync(host, &result))
          result.ok = FALSE;
        if(result.ok && schedule.prime_live_process_patch && self)
          dt_drawlayer_prime_live_process_patch_before_stroke(self);
        if(result.ok && schedule.ensure_worker_running && self && g)
        {
          if(!dt_drawlayer_worker_ensure_running(self, g->stroke.worker))
          {
            dt_control_log(_("failed to start drawing worker"));
            result.ok = FALSE;
          }
        }
        if(result.ok && self) dt_drawlayer_begin_gui_stroke_capture(self, context->raw_input);
      }
      else if(request->raw_input_kind == DT_DRAWLAYER_RUNTIME_RAW_INPUT_STROKE_END)
        if(self) dt_drawlayer_end_gui_stroke_capture(self);

      if(result.ok && schedule.ensure_worker_running && self && g
         && request->raw_input_kind != DT_DRAWLAYER_RUNTIME_RAW_INPUT_STROKE_BEGIN)
      {
        if(!dt_drawlayer_worker_ensure_running(self, g->stroke.worker)
           && request->raw_input_kind != DT_DRAWLAYER_RUNTIME_RAW_INPUT_SAMPLE)
        {
          dt_control_log(_("failed to start drawing worker"));
          result.ok = FALSE;
        }
      }

      if(schedule.request_commit && g) dt_drawlayer_worker_request_commit(g->stroke.worker);

      if(schedule.queue_raw_input && g)
      {
        gboolean ok = TRUE;
        if(!context->raw_input)
          ok = FALSE;
        else if(request->raw_input_kind == DT_DRAWLAYER_RUNTIME_RAW_INPUT_STROKE_END)
          ok = dt_drawlayer_worker_enqueue_stroke_end(g->stroke.worker, context->raw_input);
        else if(request->raw_input_kind == DT_DRAWLAYER_RUNTIME_RAW_INPUT_STROKE_BEGIN
                || request->raw_input_kind == DT_DRAWLAYER_RUNTIME_RAW_INPUT_SAMPLE)
          ok = dt_drawlayer_worker_enqueue_input(g->stroke.worker, context->raw_input);
        result.raw_input_ok = ok;
      }
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_PROCESS_CPU_BEFORE:
    case DT_DRAWLAYER_RUNTIME_EVENT_PROCESS_CL_BEFORE:
#ifdef HAVE_OPENCL
      if(schedule.release_process_clmem && process)
      {
        dt_drawlayer_cache_patch_wrlock(&process->process_read_patch);
        dt_drawlayer_process_state_clear_clmem(process);
        dt_drawlayer_cache_patch_wrunlock(&process->process_read_patch);
      }
#endif
      if(schedule.ensure_layer_cache)
      {
        const dt_drawlayer_runtime_update_request_t begin = {
          .event = DT_DRAWLAYER_RUNTIME_EVENT_SIDECAR_LOAD_BEGIN,
          .raw_input_kind = DT_DRAWLAYER_RUNTIME_RAW_INPUT_NONE,
          .inputs = request->inputs,
        };
        _update_manager_information(state, &begin, host, NULL);
        if(self && !dt_drawlayer_ensure_layer_cache(self)) result.ok = FALSE;
        const dt_drawlayer_runtime_update_request_t end = {
          .event = DT_DRAWLAYER_RUNTIME_EVENT_SIDECAR_LOAD_END,
          .raw_input_kind = DT_DRAWLAYER_RUNTIME_RAW_INPUT_NONE,
          .inputs = request->inputs,
        };
        _update_manager_information(state, &end, host, NULL);
      }
      if(schedule.build_process_patch && self && g && process && process->cache_valid && context->runtime.piece
         && context->runtime.roi_out)
        dt_drawlayer_build_process_patch_from_base(self, g, context->runtime.piece, context->runtime.roi_in,
                                                   context->runtime.roi_out);
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_PROCESS_CPU_AFTER:
    case DT_DRAWLAYER_RUNTIME_EVENT_PROCESS_CL_AFTER:
    case DT_DRAWLAYER_RUNTIME_EVENT_SIDECAR_LOAD_BEGIN:
    case DT_DRAWLAYER_RUNTIME_EVENT_SIDECAR_LOAD_END:
    case DT_DRAWLAYER_RUNTIME_EVENT_SIDECAR_SAVE_BEGIN:
    case DT_DRAWLAYER_RUNTIME_EVENT_SIDECAR_SAVE_END:
    case DT_DRAWLAYER_RUNTIME_EVENT_COMMIT_BEGIN:
    case DT_DRAWLAYER_RUNTIME_EVENT_COMMIT_END:
    case DT_DRAWLAYER_RUNTIME_EVENT_NONE:
    default:
      break;
  }

  if(request->release.source)
    _release_runtime_source(state, request->release.process, request->release.source);

  _update_manager_information(state, request->inputs ? request : NULL, host, NULL);
  if(schedule.sync_realtime_mode && self && g)
    dt_drawlayer_set_pipeline_realtime_mode(self, g->manager.realtime_active);

  return result;
}

void dt_drawlayer_runtime_manager_bind_piece(dt_drawlayer_runtime_manager_t *headless_manager,
                                             dt_drawlayer_process_state_t *headless_process,
                                             dt_drawlayer_runtime_manager_t *gui_manager,
                                             dt_drawlayer_process_state_t *gui_process,
                                             const gboolean display_pipe,
                                             dt_drawlayer_runtime_manager_t **runtime_manager,
                                             dt_drawlayer_process_state_t **runtime_process,
                                             gboolean *runtime_display_pipe)
{
  if(runtime_manager) *runtime_manager = display_pipe ? gui_manager : headless_manager;
  if(runtime_process) *runtime_process = display_pipe ? gui_process : headless_process;
  if(runtime_display_pipe) *runtime_display_pipe = display_pipe;
}

void dt_drawlayer_process_state_init(dt_drawlayer_process_state_t *state)
{
  if(!state) return;
  memset(state, 0, sizeof(*state));
  state->cache_imgid = -1;
  state->cache_layer_order = -1;
#ifdef HAVE_OPENCL
  state->process_read_clmem_devid = -1;
  state->process_read_clmem_dirty = TRUE;
  dt_drawlayer_paint_runtime_state_reset(&state->process_read_clmem_dirty_rect);
#endif
}

void dt_drawlayer_process_state_cleanup(dt_drawlayer_process_state_t *state)
{
  if(!state) return;
#ifdef HAVE_OPENCL
  dt_drawlayer_process_state_clear_clmem(state);
#endif
  dt_drawlayer_cache_patch_clear(&state->base_patch, "drawlayer patch");
  dt_drawlayer_cache_patch_clear(&state->process_patch, "drawlayer patch");
  dt_drawlayer_cache_patch_clear(&state->process_read_patch, "drawlayer patch");
  dt_drawlayer_cache_patch_clear(&state->stroke_mask, "drawlayer patch");
  dt_drawlayer_cache_patch_clear(&state->process_stroke_mask, "drawlayer patch");
  memset(state, 0, sizeof(*state));
  state->cache_imgid = -1;
  state->cache_layer_order = -1;
#ifdef HAVE_OPENCL
  state->process_read_clmem_devid = -1;
  state->process_read_clmem_dirty = TRUE;
  dt_drawlayer_paint_runtime_state_reset(&state->process_read_clmem_dirty_rect);
#endif
}

void dt_drawlayer_process_state_reset_stroke(dt_drawlayer_process_state_t *state)
{
  if(!state) return;
  dt_drawlayer_cache_patch_wrlock(&state->process_patch);
  dt_drawlayer_paint_runtime_state_reset(&state->process_dirty_rect);
  if(state->stroke_mask.pixels)
    memset(state->stroke_mask.pixels, 0, (size_t)state->stroke_mask.width * state->stroke_mask.height * sizeof(float));
  if(state->process_stroke_mask.pixels)
    memset(state->process_stroke_mask.pixels, 0,
           (size_t)state->process_stroke_mask.width * state->process_stroke_mask.height * sizeof(float));
  dt_drawlayer_cache_patch_wrunlock(&state->process_patch);
}

void dt_drawlayer_process_state_invalidate(dt_drawlayer_process_state_t *state)
{
  if(!state) return;
  dt_drawlayer_cache_patch_wrlock(&state->process_patch);
  dt_drawlayer_cache_invalidate_process_patch_state(&state->process_patch_valid, &state->process_patch_dirty,
                                                    &state->process_dirty_rect, &state->process_patch_padding,
                                                    &state->process_combined_roi);
  state->process_snapshot_valid = FALSE;
#ifdef HAVE_OPENCL
  state->process_read_clmem_dirty = TRUE;
  dt_drawlayer_paint_runtime_state_reset(&state->process_read_clmem_dirty_rect);
#endif
  dt_drawlayer_cache_patch_wrunlock(&state->process_patch);
}

/* Publish a stable, host-side snapshot of the mutable `process_patch` into
 * `process_read_patch` for readers (GUI, pipeline, GPU). This copies either
 * the full tile or the provided damage rect into the external buffer while
 * holding `process_patch` read-locked and `process_read_patch` write-locked,
 * then flushes any host-pinned memory for safe GPU access. */
gboolean dt_drawlayer_process_state_publish_locked(dt_drawlayer_process_state_t *state,
                                                   const dt_drawlayer_damaged_rect_t *damage,
                                                   const gboolean full_copy)
{
  if(!state) return FALSE;
  if(!full_copy && (!damage || !damage->valid)) return FALSE;
  if(state->process_patch.width <= 0 || state->process_patch.height <= 0) return FALSE;
  const gboolean need_realloc = (!state->process_read_patch.pixels
                                 || state->process_read_patch.width != state->process_patch.width
                                 || state->process_read_patch.height != state->process_patch.height);
  if(!_ensure_external_patch_buffer(&state->process_read_patch, state->process_patch.width,
                                    state->process_patch.height, "drawlayer process read tile"))
    return FALSE;

  dt_drawlayer_cache_patch_rdlock(&state->process_patch);
  if(!state->process_patch.pixels || state->process_patch.width <= 0 || state->process_patch.height <= 0)
  {
    dt_drawlayer_cache_patch_rdunlock(&state->process_patch);
    return FALSE;
  }

  const gboolean publish_full_copy = (full_copy || need_realloc || !state->process_snapshot_valid);
  const int width = state->process_patch.width;
  const int height = state->process_patch.height;
  dt_drawlayer_cache_patch_wrlock(&state->process_read_patch);
  const dt_drawlayer_damaged_rect_t full_rect = {
    .valid = TRUE,
    .nw = { 0, 0 },
    .se = { width, height },
  };
  const dt_drawlayer_damaged_rect_t *publish_rect = publish_full_copy ? &full_rect : damage;
  _copy_patch_rect(&state->process_patch, &state->process_read_patch, publish_rect);
  state->process_snapshot_valid = TRUE;
#ifdef HAVE_OPENCL
  state->process_read_clmem_dirty = TRUE;
  if(publish_full_copy)
  {
    state->process_read_clmem_dirty_rect = full_rect;
  }
  else
  {
    dt_drawlayer_paint_runtime_note_dab_damage(&state->process_read_clmem_dirty_rect, publish_rect);
  }
#endif
  dt_drawlayer_cache_patch_wrunlock(&state->process_read_patch);
  dt_drawlayer_cache_patch_rdunlock(&state->process_patch);
#ifdef HAVE_OPENCL
  dt_dev_pixelpipe_cache_flush_host_pinned_image(darktable.pixelpipe_cache, state->process_read_patch.pixels, NULL,
                                                 -1);
#endif
  return TRUE;
}

static void _release_runtime_source(dt_drawlayer_runtime_manager_t *state,
                                    dt_drawlayer_process_state_t *process,
                                    dt_drawlayer_runtime_source_t *source)
{
  if(!source) return;

  if(state && source->tracked_read_lock)
    dt_drawlayer_runtime_manager_note_buffer_lock(state, source->tracked_buffer, source->tracked_actor, FALSE,
                                                  FALSE);

  switch(source->kind)
  {
    case DT_DRAWLAYER_SOURCE_GUI_PROCESS:
      break;

    case DT_DRAWLAYER_SOURCE_HEADLESS_BASE:
      if(process) dt_drawlayer_cache_patch_rdunlock(&process->base_patch);
      break;

    case DT_DRAWLAYER_SOURCE_NONE:
    default:
      break;
  }

  *source = (dt_drawlayer_runtime_source_t){ 0 };
}

void dt_drawlayer_ui_cursor_clear(dt_drawlayer_ui_state_t *state)
{
  if(!state) return;
  if(state->cursor_surface)
  {
    cairo_surface_destroy(state->cursor_surface);
    state->cursor_surface = NULL;
  }
  state->cursor_surface_size = 0;
  state->cursor_surface_ppd = 0.0;
  state->cursor_radius = 0.0f;
  state->cursor_opacity = 0.0f;
  state->cursor_hardness = 0.0f;
  state->cursor_shape = -1;
  state->cursor_color[0] = state->cursor_color[1] = state->cursor_color[2] = -1.0f;
}

#ifdef HAVE_OPENCL
void dt_drawlayer_process_state_clear_clmem(dt_drawlayer_process_state_t *state)
{
  if(!state) return;
  if(state->process_read_clmem) dt_opencl_release_mem_object(state->process_read_clmem);
  state->process_read_clmem = NULL;
  state->process_read_clmem_width = 0;
  state->process_read_clmem_height = 0;
  state->process_read_clmem_devid = -1;
  state->process_read_clmem_dirty = TRUE;
  dt_drawlayer_paint_runtime_state_reset(&state->process_read_clmem_dirty_rect);
}

cl_mem dt_drawlayer_process_state_ensure_read_clmem_locked(dt_drawlayer_process_state_t *state, const int devid)
{
  if(!state || devid < 0 || !state->process_read_patch.pixels || state->process_read_patch.width <= 0
     || state->process_read_patch.height <= 0)
    return NULL;

  const gboolean need_realloc = (!state->process_read_clmem || state->process_read_clmem_devid != devid
                                 || state->process_read_clmem_width != state->process_read_patch.width
                                 || state->process_read_clmem_height != state->process_read_patch.height);
  if(need_realloc)
  {
    dt_drawlayer_process_state_clear_clmem(state);
    state->process_read_clmem
        = dt_opencl_alloc_device(devid, state->process_read_patch.width, state->process_read_patch.height,
                                 4 * sizeof(float));
    if(!state->process_read_clmem) return NULL;
    state->process_read_clmem_width = state->process_read_patch.width;
    state->process_read_clmem_height = state->process_read_patch.height;
    state->process_read_clmem_devid = devid;
    state->process_read_clmem_dirty = TRUE;
    state->process_read_clmem_dirty_rect = (dt_drawlayer_damaged_rect_t){
      .valid = TRUE,
      .nw = { 0, 0 },
      .se = { state->process_read_patch.width, state->process_read_patch.height },
    };
  }

  if(state->process_read_clmem_dirty)
  {
    dt_drawlayer_damaged_rect_t upload_rect = state->process_read_clmem_dirty_rect;
    if(!upload_rect.valid)
    {
      upload_rect = (dt_drawlayer_damaged_rect_t){
        .valid = TRUE,
        .nw = { 0, 0 },
        .se = { state->process_read_patch.width, state->process_read_patch.height },
      };
    }

    const size_t origin[] = { (size_t)CLAMP(upload_rect.nw[0], 0, state->process_read_patch.width),
                              (size_t)CLAMP(upload_rect.nw[1], 0, state->process_read_patch.height), 0 };
    const size_t region[] = { (size_t)CLAMP(upload_rect.se[0], 0, state->process_read_patch.width) - origin[0],
                              (size_t)CLAMP(upload_rect.se[1], 0, state->process_read_patch.height) - origin[1], 1 };
    float *host_ptr = state->process_read_patch.pixels
                      + 4 * ((size_t)origin[1] * state->process_read_patch.width + origin[0]);
    if(region[0] == 0 || region[1] == 0
       || dt_opencl_write_host_to_device_raw(devid, host_ptr, state->process_read_clmem, origin, region,
                                             state->process_read_patch.width * 4 * (int)sizeof(float), CL_TRUE)
       != CL_SUCCESS)
    {
      dt_drawlayer_process_state_clear_clmem(state);
      return NULL;
    }
    state->process_read_clmem_dirty = FALSE;
    dt_drawlayer_paint_runtime_state_reset(&state->process_read_clmem_dirty_rect);
  }

  return state->process_read_clmem;
}
#endif
