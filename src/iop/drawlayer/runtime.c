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
#include "iop/drawlayer/runtime.h"

#include <string.h>

#if defined(__clang__)
#define DRAWLAYER_NO_THREAD_SAFETY_ANALYSIS __attribute__((no_thread_safety_analysis))
#else
#define DRAWLAYER_NO_THREAD_SAFETY_ANALYSIS
#endif

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
  return patch->pixels != NULL;
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

static void _collect_runtime_inputs(const dt_drawlayer_runtime_update_request_t *request,
                                    const dt_drawlayer_runtime_host_t *host,
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
  if(!host || !host->collect_inputs) return;
  host->collect_inputs(host->user_data, inputs, worker_snapshot);
}

static gboolean _perform_runtime_action(const dt_drawlayer_runtime_host_t *host,
                                        const dt_drawlayer_runtime_action_request_t *action,
                                        dt_drawlayer_runtime_result_t *result)
{
  if(!host || !host->perform_action || !action) return FALSE;
  return host->perform_action(host->user_data, action, result);
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
    state->undo_available = process->undo_available;
    state->redo_available = process->redo_available;
    state->process_snapshot_valid = process->process_patch_valid && process->process_read_patch.pixels
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
                             && (((state->painting_active
                                   || (inputs && inputs->stroke && inputs->stroke->finish_commit_pending)))
                                 || state->threads[DT_DRAWLAYER_RUNTIME_ACTOR_RASTER_BACKEND].active);

  if(request)
  {
    switch(request->event)
    {
      case DT_DRAWLAYER_RUNTIME_EVENT_GUI_FOCUS_LOSS:
      case DT_DRAWLAYER_RUNTIME_EVENT_COMMIT_BEGIN:
      case DT_DRAWLAYER_RUNTIME_EVENT_GUI_CHANGE_IMAGE:
      case DT_DRAWLAYER_RUNTIME_EVENT_GUI_PIPE_FINISHED:
      case DT_DRAWLAYER_RUNTIME_EVENT_GUI_STROKE_ABORT:
        realtime_active = FALSE;
        break;

      case DT_DRAWLAYER_RUNTIME_EVENT_GUI_RAW_INPUT:
        if(request->raw_input_kind == DT_DRAWLAYER_RUNTIME_RAW_INPUT_STROKE_BEGIN
           || request->raw_input_kind == DT_DRAWLAYER_RUNTIME_RAW_INPUT_STROKE_END)
          realtime_active = state->gui_focused && inputs && inputs->gui_attached;
        break;

      case DT_DRAWLAYER_RUNTIME_EVENT_NONE:
      case DT_DRAWLAYER_RUNTIME_EVENT_GUI_FOCUS_GAIN:
      case DT_DRAWLAYER_RUNTIME_EVENT_GUI_MOUSE_ENTER:
      case DT_DRAWLAYER_RUNTIME_EVENT_GUI_MOUSE_LEAVE:
      case DT_DRAWLAYER_RUNTIME_EVENT_GUI_SCROLL:
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
  gboolean sync_temp_buffers;
  gboolean sync_temp_buffers_flush_pending;
  gboolean prepare_undo_snapshot;
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
  gboolean sync_save_button;
  gboolean refresh_gui;
  gboolean invalidate_layer_cache;
  gboolean rasterization_busy;
} dt_drawlayer_runtime_schedule_t;

static void _release_runtime_source(dt_drawlayer_runtime_manager_t *state,
                                    dt_drawlayer_process_state_t *process,
                                    const dt_drawlayer_cache_patch_t *headless_base_patch,
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
  const gboolean have_pending_cache_writes
      = process && (process->cache_dirty || process->process_patch_dirty);

  switch(request->event)
  {
    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_FOCUS_GAIN:
      schedule->sync_realtime_mode = TRUE;
      schedule->ensure_worker_running = inputs->module_focused;
      schedule->sync_temp_buffers = inputs->gui_attached && inputs->have_layer_selection;
      schedule->sync_temp_buffers_flush_pending = FALSE;
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
      schedule->sync_temp_buffers = TRUE;
      schedule->sync_temp_buffers_flush_pending = TRUE;
      schedule->queue_redraw_center = TRUE;
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
      schedule->sync_save_button = TRUE;
      schedule->refresh_gui = TRUE;
      schedule->sync_temp_buffers = TRUE;
      schedule->sync_temp_buffers_flush_pending = TRUE;
      schedule->ensure_worker_running = inputs->module_focused;
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_RESYNC:
      schedule->sync_temp_buffers = inputs->gui_attached && inputs->have_layer_selection;
      schedule->sync_temp_buffers_flush_pending = FALSE;
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_PIPE_FINISHED:
      schedule->sync_temp_buffers = (inputs->view_changed || inputs->padding_changed)
                                    && !state->painting_active && !schedule->rasterization_busy;
      schedule->sync_temp_buffers_flush_pending = FALSE;
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
          schedule->sync_temp_buffers = TRUE;
          schedule->sync_temp_buffers_flush_pending = TRUE;
          schedule->commit_mode = (!state->painting_active
                                   && ((stroke && (stroke->finish_commit_pending || stroke->stroke_sample_count > 0))
                                       || backend_busy))
                                      ? DT_DRAWLAYER_RUNTIME_COMMIT_HISTORY
                                      : DT_DRAWLAYER_RUNTIME_COMMIT_NONE;
          schedule->prepare_undo_snapshot = TRUE;
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
          schedule->sync_realtime_mode = TRUE;
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
  _collect_runtime_inputs(request, host, &inputs, &worker_snapshot);

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

  dt_drawlayer_runtime_schedule_t schedule = { 0 };
  _update_manager_information(state, request, host, &schedule);

  if(schedule.sync_realtime_mode)
  {
    const dt_drawlayer_runtime_action_request_t action = {
      .action = DT_DRAWLAYER_RUNTIME_ACTION_SYNC_REALTIME_MODE,
    };
    _perform_runtime_action(host, &action, &result);
  }

  switch(request->event)
  {
    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_FOCUS_GAIN:
      if(schedule.ensure_worker_running)
      {
        const dt_drawlayer_runtime_action_request_t action = {
          .action = DT_DRAWLAYER_RUNTIME_ACTION_ENSURE_WORKER_RUNNING,
        };
        _perform_runtime_action(host, &action, &result);
      }
      if(schedule.sync_temp_buffers)
      {
        const dt_drawlayer_runtime_action_request_t action = {
          .action = DT_DRAWLAYER_RUNTIME_ACTION_SYNC_TEMP_BUFFERS,
          .data.sync_temp_buffers = {
            .flush_pending = schedule.sync_temp_buffers_flush_pending,
          },
        };
        _perform_runtime_action(host, &action, &result);
      }
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_MOUSE_ENTER:
    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_MOUSE_LEAVE:
      if(schedule.set_pointer_state)
      {
        const dt_drawlayer_runtime_action_request_t action = {
          .action = DT_DRAWLAYER_RUNTIME_ACTION_SET_POINTER_STATE,
          .data.pointer = {
            .valid = schedule.pointer_valid,
            .hide_cursor = schedule.pointer_hide_cursor,
          },
        };
        _perform_runtime_action(host, &action, &result);
      }
      if(schedule.queue_redraw_center)
      {
        const dt_drawlayer_runtime_action_request_t action = {
          .action = DT_DRAWLAYER_RUNTIME_ACTION_QUEUE_REDRAW_CENTER,
        };
        _perform_runtime_action(host, &action, &result);
      }
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_SCROLL:
    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_RESYNC:
      if(schedule.sync_temp_buffers)
      {
        const dt_drawlayer_runtime_action_request_t action = {
          .action = DT_DRAWLAYER_RUNTIME_ACTION_SYNC_TEMP_BUFFERS,
          .data.sync_temp_buffers = {
            .flush_pending = schedule.sync_temp_buffers_flush_pending,
          },
        };
        if(!_perform_runtime_action(host, &action, &result)) result.ok = FALSE;
      }
      if(schedule.queue_redraw_center)
      {
        const dt_drawlayer_runtime_action_request_t action = {
          .action = DT_DRAWLAYER_RUNTIME_ACTION_QUEUE_REDRAW_CENTER,
        };
        _perform_runtime_action(host, &action, &result);
      }
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_PIPE_FINISHED:
      if(schedule.sync_temp_buffers)
      {
        const dt_drawlayer_runtime_action_request_t action = {
          .action = DT_DRAWLAYER_RUNTIME_ACTION_SYNC_TEMP_BUFFERS,
          .data.sync_temp_buffers = {
            .flush_pending = schedule.sync_temp_buffers_flush_pending,
          },
        };
        if(!_perform_runtime_action(host, &action, &result)) result.ok = FALSE;
      }
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_STROKE_ABORT:
    {
      const dt_drawlayer_runtime_action_request_t end_capture = {
        .action = DT_DRAWLAYER_RUNTIME_ACTION_END_STROKE_CAPTURE,
      };
      _perform_runtime_action(host, &end_capture, &result);
      if(schedule.request_commit)
      {
        const dt_drawlayer_runtime_action_request_t action = {
          .action = DT_DRAWLAYER_RUNTIME_ACTION_REQUEST_COMMIT,
        };
        _perform_runtime_action(host, &action, &result);
      }
      break;
    }

    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_FOCUS_LOSS:
    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_CHANGE_IMAGE:
      if(schedule.feedback != DT_DRAWLAYER_RUNTIME_FEEDBACK_NONE)
      {
        const dt_drawlayer_runtime_action_request_t action = {
          .action = DT_DRAWLAYER_RUNTIME_ACTION_SHOW_FEEDBACK,
          .data.feedback = schedule.feedback,
        };
        _perform_runtime_action(host, &action, &result);
      }
      if(schedule.commit_mode != DT_DRAWLAYER_RUNTIME_COMMIT_NONE)
      {
        const dt_drawlayer_runtime_update_request_t begin = {
          .event = DT_DRAWLAYER_RUNTIME_EVENT_COMMIT_BEGIN,
          .raw_input_kind = DT_DRAWLAYER_RUNTIME_RAW_INPUT_NONE,
          .inputs = request->inputs,
        };
        const dt_drawlayer_runtime_action_request_t action = {
          .action = DT_DRAWLAYER_RUNTIME_ACTION_COMMIT,
          .data.commit_mode = schedule.commit_mode,
        };
        _update_manager_information(state, &begin, host, NULL);
        _perform_runtime_action(host,
                                &(dt_drawlayer_runtime_action_request_t){
                                  .action = DT_DRAWLAYER_RUNTIME_ACTION_SYNC_REALTIME_MODE,
                                },
                                &result);
        if(!_perform_runtime_action(host, &action, &result)) result.ok = FALSE;
        const dt_drawlayer_runtime_update_request_t end = {
          .event = DT_DRAWLAYER_RUNTIME_EVENT_COMMIT_END,
          .raw_input_kind = DT_DRAWLAYER_RUNTIME_RAW_INPUT_NONE,
          .inputs = request->inputs,
        };
        _update_manager_information(state, &end, host, NULL);
        _perform_runtime_action(host,
                                &(dt_drawlayer_runtime_action_request_t){
                                  .action = DT_DRAWLAYER_RUNTIME_ACTION_SYNC_REALTIME_MODE,
                                },
                                &result);
      }
      if(schedule.wait_fullres_worker)
      {
        const dt_drawlayer_runtime_action_request_t action = {
          .action = DT_DRAWLAYER_RUNTIME_ACTION_WAIT_FULLRES_WORKER,
        };
        _perform_runtime_action(host, &action, &result);
      }
      if(schedule.flush_process_patch)
      {
        const dt_drawlayer_runtime_action_request_t action = {
          .action = DT_DRAWLAYER_RUNTIME_ACTION_FLUSH_PROCESS_PATCH,
        };
        _perform_runtime_action(host, &action, &result);
      }
      if(schedule.flush_sidecar)
      {
        const dt_drawlayer_runtime_update_request_t begin = {
          .event = DT_DRAWLAYER_RUNTIME_EVENT_SIDECAR_SAVE_BEGIN,
          .raw_input_kind = DT_DRAWLAYER_RUNTIME_RAW_INPUT_NONE,
          .inputs = request->inputs,
        };
        const dt_drawlayer_runtime_action_request_t action = {
          .action = DT_DRAWLAYER_RUNTIME_ACTION_FLUSH_SIDECAR,
        };
        _update_manager_information(state, &begin, host, NULL);
        if(!_perform_runtime_action(host, &action, &result)) result.ok = FALSE;
        const dt_drawlayer_runtime_update_request_t end = {
          .event = DT_DRAWLAYER_RUNTIME_EVENT_SIDECAR_SAVE_END,
          .raw_input_kind = DT_DRAWLAYER_RUNTIME_RAW_INPUT_NONE,
          .inputs = request->inputs,
        };
        _update_manager_information(state, &end, host, NULL);
      }
      if(schedule.stop_worker)
      {
        const dt_drawlayer_runtime_action_request_t action = {
          .action = DT_DRAWLAYER_RUNTIME_ACTION_STOP_WORKER,
        };
        _perform_runtime_action(host, &action, &result);
      }
#ifdef HAVE_OPENCL
      if(schedule.release_process_clmem)
      {
        const dt_drawlayer_runtime_action_request_t action = {
          .action = DT_DRAWLAYER_RUNTIME_ACTION_RELEASE_PROCESS_CLMEM,
        };
        _perform_runtime_action(host, &action, &result);
      }
#endif
      if(schedule.invalidate_layer_cache)
      {
        const dt_drawlayer_runtime_action_request_t action = {
          .action = DT_DRAWLAYER_RUNTIME_ACTION_INVALIDATE_LAYER_CACHE,
        };
        _perform_runtime_action(host, &action, &result);
      }
      if(schedule.sync_save_button)
      {
        const dt_drawlayer_runtime_action_request_t action = {
          .action = DT_DRAWLAYER_RUNTIME_ACTION_SYNC_SAVE_BUTTON,
        };
        _perform_runtime_action(host, &action, &result);
      }
      if(schedule.refresh_gui)
      {
        const dt_drawlayer_runtime_action_request_t action = {
          .action = DT_DRAWLAYER_RUNTIME_ACTION_REFRESH_GUI,
        };
        _perform_runtime_action(host, &action, &result);
      }
      if(schedule.sync_temp_buffers)
      {
        const dt_drawlayer_runtime_action_request_t action = {
          .action = DT_DRAWLAYER_RUNTIME_ACTION_SYNC_TEMP_BUFFERS,
          .data.sync_temp_buffers = {
            .flush_pending = schedule.sync_temp_buffers_flush_pending,
          },
        };
        _perform_runtime_action(host, &action, &result);
      }
      if(schedule.ensure_worker_running)
      {
        const dt_drawlayer_runtime_action_request_t action = {
          .action = DT_DRAWLAYER_RUNTIME_ACTION_ENSURE_WORKER_RUNNING,
        };
        _perform_runtime_action(host, &action, &result);
      }
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_GUI_RAW_INPUT:
      if(request->raw_input_kind == DT_DRAWLAYER_RUNTIME_RAW_INPUT_STROKE_BEGIN)
      {
        if(schedule.sync_temp_buffers)
        {
          const dt_drawlayer_runtime_action_request_t action = {
            .action = DT_DRAWLAYER_RUNTIME_ACTION_SYNC_TEMP_BUFFERS,
            .data.sync_temp_buffers = {
              .flush_pending = schedule.sync_temp_buffers_flush_pending,
            },
          };
          if(!_perform_runtime_action(host, &action, &result)) result.ok = FALSE;
        }
        if(result.ok && schedule.commit_mode != DT_DRAWLAYER_RUNTIME_COMMIT_NONE)
        {
          const dt_drawlayer_runtime_update_request_t begin = {
            .event = DT_DRAWLAYER_RUNTIME_EVENT_COMMIT_BEGIN,
            .raw_input_kind = DT_DRAWLAYER_RUNTIME_RAW_INPUT_NONE,
            .inputs = request->inputs,
          };
          const dt_drawlayer_runtime_action_request_t action = {
            .action = DT_DRAWLAYER_RUNTIME_ACTION_COMMIT,
            .data.commit_mode = schedule.commit_mode,
          };
          _update_manager_information(state, &begin, host, NULL);
          _perform_runtime_action(host,
                                  &(dt_drawlayer_runtime_action_request_t){
                                    .action = DT_DRAWLAYER_RUNTIME_ACTION_SYNC_REALTIME_MODE,
                                  },
                                  &result);
          if(!_perform_runtime_action(host, &action, &result)) result.ok = FALSE;
          const dt_drawlayer_runtime_update_request_t end = {
            .event = DT_DRAWLAYER_RUNTIME_EVENT_COMMIT_END,
            .raw_input_kind = DT_DRAWLAYER_RUNTIME_RAW_INPUT_NONE,
            .inputs = request->inputs,
          };
          _update_manager_information(state, &end, host, NULL);
          _perform_runtime_action(host,
                                  &(dt_drawlayer_runtime_action_request_t){
                                    .action = DT_DRAWLAYER_RUNTIME_ACTION_SYNC_REALTIME_MODE,
                                  },
                                  &result);
        }
        if(result.ok && schedule.prepare_undo_snapshot)
        {
          const dt_drawlayer_runtime_action_request_t action = {
            .action = DT_DRAWLAYER_RUNTIME_ACTION_PREPARE_UNDO_SNAPSHOT,
          };
          if(!_perform_runtime_action(host, &action, &result)) result.ok = FALSE;
        }
        if(result.ok && schedule.prime_live_process_patch)
        {
          const dt_drawlayer_runtime_action_request_t action = {
            .action = DT_DRAWLAYER_RUNTIME_ACTION_PRIME_LIVE_PROCESS_PATCH,
          };
          _perform_runtime_action(host, &action, &result);
        }
        if(result.ok && schedule.ensure_worker_running)
        {
          const dt_drawlayer_runtime_action_request_t action = {
            .action = DT_DRAWLAYER_RUNTIME_ACTION_ENSURE_WORKER_RUNNING,
          };
          if(!_perform_runtime_action(host, &action, &result)) result.ok = FALSE;
        }
        if(result.ok)
        {
          const dt_drawlayer_runtime_action_request_t action = {
            .action = DT_DRAWLAYER_RUNTIME_ACTION_BEGIN_STROKE_CAPTURE,
          };
          _perform_runtime_action(host, &action, &result);
        }
      }
      else if(request->raw_input_kind == DT_DRAWLAYER_RUNTIME_RAW_INPUT_STROKE_END)
      {
        const dt_drawlayer_runtime_action_request_t end_capture = {
          .action = DT_DRAWLAYER_RUNTIME_ACTION_END_STROKE_CAPTURE,
        };
        _perform_runtime_action(host, &end_capture, &result);
      }

      if(result.ok && schedule.ensure_worker_running
         && request->raw_input_kind != DT_DRAWLAYER_RUNTIME_RAW_INPUT_STROKE_BEGIN)
      {
        const dt_drawlayer_runtime_action_request_t action = {
          .action = DT_DRAWLAYER_RUNTIME_ACTION_ENSURE_WORKER_RUNNING,
        };
        if(!_perform_runtime_action(host, &action, &result)
           && request->raw_input_kind != DT_DRAWLAYER_RUNTIME_RAW_INPUT_SAMPLE)
          result.ok = FALSE;
      }

      if(schedule.request_commit)
      {
        const dt_drawlayer_runtime_action_request_t action = {
          .action = DT_DRAWLAYER_RUNTIME_ACTION_REQUEST_COMMIT,
        };
        _perform_runtime_action(host, &action, &result);
      }

      if(schedule.queue_raw_input)
      {
        const dt_drawlayer_runtime_action_request_t action = {
          .action = DT_DRAWLAYER_RUNTIME_ACTION_QUEUE_RAW_INPUT,
          .data.raw_input = {
            .kind = request->raw_input_kind,
          },
        };
        _perform_runtime_action(host, &action, &result);
      }
      break;

    case DT_DRAWLAYER_RUNTIME_EVENT_PROCESS_CPU_BEFORE:
    case DT_DRAWLAYER_RUNTIME_EVENT_PROCESS_CL_BEFORE:
#ifdef HAVE_OPENCL
      if(schedule.release_process_clmem)
      {
        const dt_drawlayer_runtime_action_request_t action = {
          .action = DT_DRAWLAYER_RUNTIME_ACTION_RELEASE_PROCESS_CLMEM,
        };
        _perform_runtime_action(host, &action, &result);
      }
#endif
      if(schedule.ensure_layer_cache)
      {
        const dt_drawlayer_runtime_update_request_t begin = {
          .event = DT_DRAWLAYER_RUNTIME_EVENT_SIDECAR_LOAD_BEGIN,
          .raw_input_kind = DT_DRAWLAYER_RUNTIME_RAW_INPUT_NONE,
          .inputs = request->inputs,
        };
        const dt_drawlayer_runtime_action_request_t action = {
          .action = DT_DRAWLAYER_RUNTIME_ACTION_ENSURE_LAYER_CACHE,
        };
        _update_manager_information(state, &begin, host, NULL);
        if(!_perform_runtime_action(host, &action, &result)) result.ok = FALSE;
        const dt_drawlayer_runtime_update_request_t end = {
          .event = DT_DRAWLAYER_RUNTIME_EVENT_SIDECAR_LOAD_END,
          .raw_input_kind = DT_DRAWLAYER_RUNTIME_RAW_INPUT_NONE,
          .inputs = request->inputs,
        };
        _update_manager_information(state, &end, host, NULL);
      }
      if(schedule.build_process_patch)
      {
        const dt_drawlayer_runtime_action_request_t action = {
          .action = DT_DRAWLAYER_RUNTIME_ACTION_BUILD_PROCESS_PATCH,
        };
        _perform_runtime_action(host, &action, &result);
      }
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
    _release_runtime_source(state, request->release.process, request->release.headless_base_patch,
                            request->release.source);

  _update_manager_information(state, request->inputs ? request : NULL, host, NULL);
  if(schedule.sync_realtime_mode)
  {
    const dt_drawlayer_runtime_action_request_t action = {
      .action = DT_DRAWLAYER_RUNTIME_ACTION_SYNC_REALTIME_MODE,
    };
    _perform_runtime_action(host, &action, &result);
  }

  return result;
}

void dt_drawlayer_runtime_manager_bind_piece(dt_drawlayer_runtime_manager_t *headless_manager,
                                             dt_drawlayer_runtime_manager_t *gui_manager,
                                             dt_drawlayer_process_state_t *gui_process,
                                             const gboolean display_pipe,
                                             dt_drawlayer_runtime_manager_t **runtime_manager,
                                             dt_drawlayer_process_state_t **runtime_process,
                                             gboolean *runtime_display_pipe)
{
  if(runtime_manager) *runtime_manager = display_pipe ? gui_manager : headless_manager;
  if(runtime_process) *runtime_process = display_pipe ? gui_process : NULL;
  if(runtime_display_pipe) *runtime_display_pipe = display_pipe;
}

void dt_drawlayer_process_state_init(dt_drawlayer_process_state_t *state)
{
  if(!state) return;
  memset(state, 0, sizeof(*state));
  dt_pthread_mutex_init(&state->process_patch_mutex, NULL);
  state->backend_path = dt_drawlayer_paint_runtime_state_create();
  state->cache_imgid = -1;
  state->cache_layer_order = -1;
#ifdef HAVE_OPENCL
  state->process_read_clmem_devid = -1;
  state->process_read_clmem_dirty = TRUE;
#endif
}

void dt_drawlayer_process_state_cleanup(dt_drawlayer_process_state_t *state)
{
  if(!state) return;
#ifdef HAVE_OPENCL
  dt_drawlayer_process_state_clear_clmem(state);
#endif
  dt_drawlayer_paint_runtime_state_destroy(&state->backend_path);
  dt_drawlayer_cache_patch_clear(&state->base_patch, "drawlayer patch");
  dt_drawlayer_cache_patch_clear(&state->process_patch, "drawlayer patch");
  dt_drawlayer_cache_patch_clear(&state->process_read_patch, "drawlayer patch");
  dt_drawlayer_cache_patch_clear(&state->undo_patch, "drawlayer patch");
  dt_drawlayer_cache_patch_clear(&state->stroke_mask, "drawlayer patch");
  dt_drawlayer_cache_patch_clear(&state->process_stroke_mask, "drawlayer patch");
  dt_pthread_mutex_destroy(&state->process_patch_mutex);
  memset(state, 0, sizeof(*state));
  state->cache_imgid = -1;
  state->cache_layer_order = -1;
#ifdef HAVE_OPENCL
  state->process_read_clmem_devid = -1;
  state->process_read_clmem_dirty = TRUE;
#endif
}

void dt_drawlayer_process_state_reset_stroke(dt_drawlayer_process_state_t *state)
{
  if(!state) return;
  dt_pthread_mutex_lock(&state->process_patch_mutex);
  dt_drawlayer_paint_runtime_state_reset(state->backend_path);
  dt_drawlayer_paint_runtime_state_reset(&state->process_dirty_rect);
  if(state->stroke_mask.pixels)
    memset(state->stroke_mask.pixels, 0, (size_t)state->stroke_mask.width * state->stroke_mask.height * sizeof(float));
  if(state->process_stroke_mask.pixels)
    memset(state->process_stroke_mask.pixels, 0,
           (size_t)state->process_stroke_mask.width * state->process_stroke_mask.height * sizeof(float));
  dt_pthread_mutex_unlock(&state->process_patch_mutex);
}

void dt_drawlayer_process_state_invalidate(dt_drawlayer_process_state_t *state)
{
  if(!state) return;
  dt_pthread_mutex_lock(&state->process_patch_mutex);
  dt_drawlayer_cache_invalidate_process_patch_state(&state->process_patch_valid, &state->process_patch_dirty,
                                                    &state->process_dirty_rect, &state->process_patch_padding,
                                                    &state->process_combined_roi);
  dt_pthread_mutex_unlock(&state->process_patch_mutex);
}

gboolean dt_drawlayer_process_state_publish_locked(dt_drawlayer_process_state_t *state,
                                                   const dt_drawlayer_damaged_rect_t *damage,
                                                   const gboolean full_copy)
{
  if(!state || !state->process_patch.pixels || state->process_patch.width <= 0 || state->process_patch.height <= 0)
    return FALSE;
  if(!full_copy && (!damage || !damage->valid)) return FALSE;

  const int width = state->process_patch.width;
  const int height = state->process_patch.height;
  if(!_ensure_external_patch_buffer(&state->process_read_patch, width, height, "drawlayer process read tile"))
    return FALSE;

  const dt_drawlayer_damaged_rect_t full_rect = {
    .valid = TRUE,
    .nw = { 0, 0 },
    .se = { width, height },
  };
  _copy_patch_rect(&state->process_patch, &state->process_read_patch, full_copy ? &full_rect : damage);
#ifdef HAVE_OPENCL
  dt_dev_pixelpipe_cache_flush_host_pinned_image(darktable.pixelpipe_cache, state->process_read_patch.pixels, NULL,
                                                 -1);
  state->process_read_clmem_dirty = TRUE;
#endif
  return TRUE;
}

DRAWLAYER_NO_THREAD_SAFETY_ANALYSIS
gboolean dt_drawlayer_process_state_lock_view(dt_drawlayer_process_state_t *state, const dt_iop_roi_t *roi_out,
                                              dt_drawlayer_process_view_t *view)
{
  if(view) *view = (dt_drawlayer_process_view_t){ 0 };
  if(!state || !roi_out || !view) return FALSE;

  dt_pthread_mutex_lock(&state->process_patch_mutex);
  dt_drawlayer_cache_patch_rdlock(&state->process_read_patch);
  if(!state->process_patch_valid || !state->process_read_patch.pixels || state->process_read_patch.width <= 0
     || state->process_read_patch.height <= 0
     || !dt_drawlayer_cache_build_process_blend_rois(&state->process_read_patch, state->process_patch_padding,
                                                     roi_out, &view->blend_target_roi, &view->source_process_roi,
                                                     &view->direct_copy))
  {
    dt_drawlayer_cache_patch_rdunlock(&state->process_read_patch);
    dt_pthread_mutex_unlock(&state->process_patch_mutex);
    return FALSE;
  }

  view->patch = &state->process_read_patch;
  return TRUE;
}

DRAWLAYER_NO_THREAD_SAFETY_ANALYSIS
void dt_drawlayer_process_state_unlock_view(dt_drawlayer_process_state_t *state)
{
  if(!state) return;
  dt_drawlayer_cache_patch_rdunlock(&state->process_read_patch);
  dt_pthread_mutex_unlock(&state->process_patch_mutex);
}

static void _release_runtime_source(dt_drawlayer_runtime_manager_t *state,
                                    dt_drawlayer_process_state_t *process,
                                    const dt_drawlayer_cache_patch_t *headless_base_patch,
                                    dt_drawlayer_runtime_source_t *source)
{
  if(!source) return;

  if(state && source->tracked_read_lock)
    dt_drawlayer_runtime_manager_note_buffer_lock(state, source->tracked_buffer, source->tracked_actor, FALSE,
                                                  FALSE);

  switch(source->kind)
  {
    case DT_DRAWLAYER_SOURCE_GUI_PROCESS:
      if(process) dt_drawlayer_process_state_unlock_view(process);
      break;

    case DT_DRAWLAYER_SOURCE_HEADLESS_BASE:
      if(headless_base_patch) dt_drawlayer_cache_patch_rdunlock(headless_base_patch);
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
  }

  if(state->process_read_clmem_dirty)
  {
    if(dt_opencl_write_host_to_device(devid, state->process_read_patch.pixels, state->process_read_clmem,
                                      state->process_read_patch.width, state->process_read_patch.height,
                                      4 * sizeof(float))
       != CL_SUCCESS)
    {
      dt_drawlayer_process_state_clear_clmem(state);
      return NULL;
    }
    state->process_read_clmem_dirty = FALSE;
  }

  return state->process_read_clmem;
}
#endif
