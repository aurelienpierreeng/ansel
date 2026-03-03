/*
 * drawlayer realtime worker subsystem
 *
 * This file acts like a small standalone worker library inside the drawlayer module:
 * it knows about threads, queues, waiting, pausing and completion, but it does not know
 * what "drawing" means. All drawlayer-specific behavior is routed through callbacks:
 * - preview worker callback processes a sample into the GUI overlay and requests redraws,
 * - backend worker callback processes a sample into the full-resolution cache and, on a
 *   stroke-end event, requests a history commit once both workers are idle.
 *
 * Both workers use the same topology:
 * - each owns its own FIFO queue,
 * - each sleeps until new events arrive,
 * - each processes events serially in realtime priority,
 * - each reports idleness through the shared condition variable.
 */

/* The preview worker is optional. Keeping this switch local to the realtime
 * subsystem makes it easy to compare "backend-only" behavior while preserving
 * the same public control API. Disabled workers are simply skipped by the
 * generic worker loops below.
 */
#ifndef DRAWLAYER_ENABLE_PREVIEW_WORKER
#define DRAWLAYER_ENABLE_PREVIEW_WORKER 0
#endif

#define DRAWLAYER_HISTORY_TICK_USEC 50000

typedef enum drawlayer_rt_worker_kind_t
{
  DRAWLAYER_RT_WORKER_PREVIEW = 0,
  DRAWLAYER_RT_WORKER_BACKEND = 1,
  DRAWLAYER_RT_WORKER_COUNT = 2,
} drawlayer_rt_worker_kind_t;

typedef void (*drawlayer_rt_sample_cb)(dt_iop_module_t *self, const drawlayer_raw_input_t *input);
typedef void (*drawlayer_rt_stroke_end_cb)(dt_iop_module_t *self);
typedef void (*drawlayer_rt_idle_cb)(dt_iop_module_t *self, dt_iop_drawlayer_gui_data_t *g);

typedef struct drawlayer_rt_callbacks_t
{
  const char *thread_name;
  drawlayer_rt_sample_cb process_sample;
  drawlayer_rt_stroke_end_cb process_stroke_end;
  drawlayer_rt_idle_cb on_idle;
} drawlayer_rt_callbacks_t;

typedef struct drawlayer_rt_worker_t
{
  drawlayer_rt_worker_kind_t kind;
  const drawlayer_rt_callbacks_t *callbacks;
  pthread_t thread;
  drawlayer_rt_event_t *ring;
  guint ring_capacity;
  guint ring_head;
  guint ring_tail;
  guint ring_count;
  gboolean started;
  gboolean stop;
  gboolean paused;
  gboolean busy;
} drawlayer_rt_worker_t;

static const drawlayer_rt_callbacks_t _rt_callbacks[DRAWLAYER_RT_WORKER_COUNT];
static gboolean _async_history_tick_idle(gpointer user_data);
static void _cancel_async_history_tick(dt_iop_module_t *self);

struct drawlayer_rt_state_t
{
  GMutex worker_mutex;
  GCond worker_cond;
  drawlayer_rt_worker_t workers[DRAWLAYER_RT_WORKER_COUNT];

  guint finish_commit_source_id;
  guint history_tick_source_id;
  guint redraw_source_id;
  gboolean backend_status_active;
  guint backend_status_source_id;
  guint undo_ui_source_id;
  gint64 last_history_tick_us;
  gint64 last_input_sample_us;
  gint64 last_queue_overflow_log_us;
  uint64_t dropped_samples;
};

#define RT(g) ((g)->rt)

static drawlayer_rt_worker_t *_rt_worker(dt_iop_drawlayer_gui_data_t *g, const drawlayer_rt_worker_kind_t kind)
{
  if(!g || !RT(g) || kind < 0 || kind >= DRAWLAYER_RT_WORKER_COUNT) return NULL;
  return &RT(g)->workers[kind];
}

static const drawlayer_rt_worker_t *_rt_worker_const(const dt_iop_drawlayer_gui_data_t *g,
                                                     const drawlayer_rt_worker_kind_t kind)
{
  if(!g || !RT(g) || kind < 0 || kind >= DRAWLAYER_RT_WORKER_COUNT) return NULL;
  return &RT(g)->workers[kind];
}

static gboolean _rt_worker_enabled(const drawlayer_rt_worker_kind_t kind)
{
  if(kind == DRAWLAYER_RT_WORKER_PREVIEW) return DRAWLAYER_ENABLE_PREVIEW_WORKER;
  return TRUE;
}

static void _rt_init_state(dt_iop_drawlayer_gui_data_t *g)
{
  /* Centralize worker/queue storage lifetime in the realtime subsystem so the
   * controller layer does not manipulate ring-buffer bookkeeping directly. */
  if(!g) return;
  g_free(RT(g));
  RT(g) = g_malloc0(sizeof(*RT(g)));
  if(!RT(g)) return;

  g_mutex_init(&RT(g)->worker_mutex);
  g_cond_init(&RT(g)->worker_cond);

  for(int i = 0; i < DRAWLAYER_RT_WORKER_COUNT; i++)
  {
    drawlayer_rt_worker_t *worker = &RT(g)->workers[i];
    worker->kind = (drawlayer_rt_worker_kind_t)i;
    worker->callbacks = &_rt_callbacks[i];
    if(!_rt_worker_enabled(worker->kind)) continue;
    worker->ring_capacity = DRAWLAYER_WORKER_RING_CAPACITY;
    worker->ring = g_malloc_n(worker->ring_capacity, sizeof(drawlayer_rt_event_t));
  }
}

static void _rt_cleanup_state(dt_iop_drawlayer_gui_data_t *g)
{
  if(!g || !RT(g)) return;

  for(int i = 0; i < DRAWLAYER_RT_WORKER_COUNT; i++) g_free(RT(g)->workers[i].ring);
  g_cond_clear(&RT(g)->worker_cond);
  g_mutex_clear(&RT(g)->worker_mutex);
  g_free(RT(g));
  RT(g) = NULL;
}

static gboolean _rt_queue_empty(const dt_iop_drawlayer_gui_data_t *g, const drawlayer_rt_worker_kind_t kind)
{
  const drawlayer_rt_worker_t *worker = _rt_worker_const(g, kind);
  return !worker || worker->ring_count == 0;
}

static gboolean _rt_queue_full(const dt_iop_drawlayer_gui_data_t *g, const drawlayer_rt_worker_kind_t kind)
{
  const drawlayer_rt_worker_t *worker = _rt_worker_const(g, kind);
  if(!worker) return TRUE;
  return worker->ring_capacity > 0 && worker->ring_count >= worker->ring_capacity;
}

static gboolean _rt_queue_push_locked(dt_iop_drawlayer_gui_data_t *g, const drawlayer_rt_worker_kind_t kind,
                                      const drawlayer_rt_event_t *event)
{
  if(!g || !event) return FALSE;
  drawlayer_rt_worker_t *worker = _rt_worker(g, kind);
  if(!worker) return FALSE;
  if(_rt_queue_full(g, kind)) return FALSE;
  if(!worker->ring || worker->ring_capacity == 0) return FALSE;
  worker->ring[worker->ring_tail] = *event;
  worker->ring_tail = (worker->ring_tail + 1) % worker->ring_capacity;
  worker->ring_count++;
  return TRUE;
}

static gboolean _rt_queue_overwrite_latest_sample_locked(dt_iop_drawlayer_gui_data_t *g,
                                                          const drawlayer_rt_worker_kind_t kind,
                                                          const drawlayer_rt_event_t *event)
{
  if(!g || !event || event->type != DRAWLAYER_RT_EVENT_SAMPLE) return FALSE;
  drawlayer_rt_worker_t *worker = _rt_worker(g, kind);
  if(!worker || !worker->ring || worker->ring_capacity == 0 || worker->ring_count == 0) return FALSE;

  const guint last_index = (worker->ring_tail + worker->ring_capacity - 1) % worker->ring_capacity;
  drawlayer_rt_event_t *last = &worker->ring[last_index];
  if(last->type != DRAWLAYER_RT_EVENT_SAMPLE) return FALSE;

  /* Never rewrite first/end semantics. Coalescing only applies to middle dabs
   * from the same stroke batch. */
  if(event->input.stroke_pos != DRAWLAYER_STROKE_MIDDLE || last->input.stroke_pos != DRAWLAYER_STROKE_MIDDLE)
    return FALSE;
  if(event->input.stroke_batch == 0u || last->input.stroke_batch != event->input.stroke_batch)
    return FALSE;

  *last = *event;
  return TRUE;
}

static gboolean _rt_queue_pop_locked(dt_iop_drawlayer_gui_data_t *g, const drawlayer_rt_worker_kind_t kind,
                                     drawlayer_rt_event_t *event)
{
  if(!g || !event || _rt_queue_empty(g, kind)) return FALSE;
  drawlayer_rt_worker_t *worker = _rt_worker(g, kind);
  if(!worker || !worker->ring || worker->ring_capacity == 0) return FALSE;
  *event = worker->ring[worker->ring_head];
  worker->ring_head = (worker->ring_head + 1) % worker->ring_capacity;
  worker->ring_count--;
  return TRUE;
}

static void _rt_queue_clear_locked(dt_iop_drawlayer_gui_data_t *g, const drawlayer_rt_worker_kind_t kind)
{
  drawlayer_rt_worker_t *worker = _rt_worker(g, kind);
  if(!worker) return;
  worker->ring_head = 0;
  worker->ring_tail = 0;
  worker->ring_count = 0;
}

static gboolean _rt_worker_stop_requested(const dt_iop_drawlayer_gui_data_t *g, const drawlayer_rt_worker_kind_t kind)
{
  const drawlayer_rt_worker_t *worker = _rt_worker_const(g, kind);
  return !worker || worker->stop;
}

static gboolean _rt_worker_paused(const dt_iop_drawlayer_gui_data_t *g, const drawlayer_rt_worker_kind_t kind)
{
  const drawlayer_rt_worker_t *worker = _rt_worker_const(g, kind);
  return !worker || worker->paused;
}

static void _rt_set_worker_busy(dt_iop_drawlayer_gui_data_t *g, const drawlayer_rt_worker_kind_t kind, const gboolean busy)
{
  drawlayer_rt_worker_t *worker = _rt_worker(g, kind);
  if(!worker) return;
  worker->busy = busy;
}

static void _set_current_thread_realtime_best_effort(void)
{
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
  struct sched_param param = { 0 };
  const int max_prio = sched_get_priority_max(SCHED_FIFO);
  if(max_prio > 0)
  {
    param.sched_priority = MIN(max_prio, 80);
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
  }
#endif
}

static gboolean _workers_active_locked(const dt_iop_drawlayer_gui_data_t *g)
{
  if(!g) return FALSE;
  for(int i = 0; i < DRAWLAYER_RT_WORKER_COUNT; i++)
  {
    if(!_rt_worker_enabled((drawlayer_rt_worker_kind_t)i)) continue;
    const drawlayer_rt_worker_t *worker = _rt_worker_const(g, (drawlayer_rt_worker_kind_t)i);
    if(worker && (worker->busy || worker->ring_count > 0)) return TRUE;
  }
  return g->finish_commit_pending;
}

static gboolean _rt_workers_active(dt_iop_drawlayer_gui_data_t *g)
{
  gboolean active = FALSE;
  if(!g) return FALSE;

  g_mutex_lock(&RT(g)->worker_mutex);
  active = _workers_active_locked(g);
  g_mutex_unlock(&RT(g)->worker_mutex);
  return active;
}

static gboolean _workers_ready_for_commit_locked(const dt_iop_drawlayer_gui_data_t *g)
{
  if(!g) return FALSE;
  if(!g->finish_commit_pending || g->painting) return FALSE;
  for(int i = 0; i < DRAWLAYER_RT_WORKER_COUNT; i++)
  {
    if(!_rt_worker_enabled((drawlayer_rt_worker_kind_t)i)) continue;
    const drawlayer_rt_worker_t *worker = _rt_worker_const(g, (drawlayer_rt_worker_kind_t)i);
    if(worker && (worker->busy || worker->ring_count > 0)) return FALSE;
  }
  return TRUE;
}

static void _schedule_async_commit_if_ready_locked(dt_iop_module_t *self, dt_iop_drawlayer_gui_data_t *g)
{
  if(!g || !self || !self->dev) return;

  if(_workers_ready_for_commit_locked(g) && RT(g)->finish_commit_source_id == 0)
    RT(g)->finish_commit_source_id = g_idle_add(_async_commit_idle, self);
}

static void _schedule_async_history_tick_locked(dt_iop_module_t *self, dt_iop_drawlayer_gui_data_t *g)
{
  if(!g || !self || !self->dev) return;
  if(!g->painting || g->finish_commit_pending) return;
  if(RT(g)->history_tick_source_id != 0) return;

  const gint64 now = g_get_monotonic_time();
  if(RT(g)->last_history_tick_us > 0 && now - RT(g)->last_history_tick_us < DRAWLAYER_HISTORY_TICK_USEC) return;

  RT(g)->last_history_tick_us = now;
  RT(g)->history_tick_source_id = g_idle_add(_async_history_tick_idle, self);
}

static void _preview_worker_on_idle(dt_iop_module_t *self, dt_iop_drawlayer_gui_data_t *g)
{
  _schedule_async_commit_if_ready_locked(self, g);
}

static void _backend_worker_on_idle(dt_iop_module_t *self, dt_iop_drawlayer_gui_data_t *g)
{
  _schedule_async_commit_if_ready_locked(self, g);
}

static void _preview_worker_process_sample(dt_iop_module_t *self, const drawlayer_raw_input_t *input)
{
  _process_live_input(self, input);
}

static void _backend_worker_process_sample(dt_iop_module_t *self, const drawlayer_raw_input_t *input)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(g && input && input->stroke_pos == DRAWLAYER_STROKE_FIRST)
  {
    _cancel_async_history_tick(self);
    g_mutex_lock(&RT(g)->worker_mutex);
    RT(g)->last_history_tick_us = input->event_ts;
    RT(g)->dropped_samples = 0;
    RT(g)->last_queue_overflow_log_us = 0;
    RT(g)->backend_status_active = TRUE;
    if(RT(g)->backend_status_source_id == 0)
      RT(g)->backend_status_source_id = g_idle_add(_async_backend_status_idle, self);
    g_mutex_unlock(&RT(g)->worker_mutex);
  }

  _process_backend_input(self, input);
  /* Redraw after each backend-processed dab so the darkroom center reflects
   * realtime stroke progress even when the optional preview worker is disabled.
   */
  _rt_schedule_async_redraw(self);
  if(g)
  {
    g_mutex_lock(&RT(g)->worker_mutex);
    _schedule_async_history_tick_locked(self, g);
    g_mutex_unlock(&RT(g)->worker_mutex);
  }
}

static void _preview_worker_process_stroke_end(dt_iop_module_t *self)
{
  _flush_pending_live_input(self);
}

static void _backend_worker_process_stroke_end(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g) return;

  _flush_pending_backend_input(self);
  g_mutex_lock(&RT(g)->worker_mutex);
  if(RT(g)->history_tick_source_id != 0)
  {
    const guint source_id = RT(g)->history_tick_source_id;
    RT(g)->history_tick_source_id = 0;
    g_mutex_unlock(&RT(g)->worker_mutex);
    g_source_remove(source_id);
    g_mutex_lock(&RT(g)->worker_mutex);
  }
  g->finish_commit_pending = TRUE;
  _schedule_async_commit_if_ready_locked(self, g);
  g_cond_broadcast(&RT(g)->worker_cond);
  g_mutex_unlock(&RT(g)->worker_mutex);
}

static const drawlayer_rt_callbacks_t _rt_callbacks[] = {
  [DRAWLAYER_RT_WORKER_PREVIEW] = {
    .thread_name = "draw-prev",
    .process_sample = _preview_worker_process_sample,
    .process_stroke_end = _preview_worker_process_stroke_end,
    .on_idle = _preview_worker_on_idle,
  },
  [DRAWLAYER_RT_WORKER_BACKEND] = {
    .thread_name = "draw-back",
    .process_sample = _backend_worker_process_sample,
    .process_stroke_end = _backend_worker_process_stroke_end,
    .on_idle = _backend_worker_on_idle,
  },
};

static gboolean _async_commit_idle(gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !self->dev) return G_SOURCE_REMOVE;

  gboolean should_commit = FALSE;
  g_mutex_lock(&RT(g)->worker_mutex);
  RT(g)->finish_commit_source_id = 0;
  should_commit = _workers_ready_for_commit_locked(g);
  if(should_commit) RT(g)->backend_status_active = FALSE;
  g_mutex_unlock(&RT(g)->worker_mutex);

  if(should_commit) _commit_dabs(self, TRUE);
  return G_SOURCE_REMOVE;
}

static gboolean _async_history_tick_idle(gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !self->dev) return G_SOURCE_REMOVE;

  g_mutex_lock(&RT(g)->worker_mutex);
  RT(g)->history_tick_source_id = 0;
  g_mutex_unlock(&RT(g)->worker_mutex);

  /* This checkpoint path must stay strictly non-blocking with respect to the
   * backend FIFO worker:
   *   - it does not wait for worker idleness,
   *   - it does not set `finish_commit_pending`,
   *   - it does not reset any stroke runtime.
   *
   * The backend worker keeps mutating the cached layer in parallel while the
   * main thread only bumps the serialized hash/history to trigger a new pipe
   * render against whatever cache state exists at that moment. */
  _checkpoint_stroke_history(self);
  return G_SOURCE_REMOVE;
}

static gboolean _async_redraw_idle(gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g) return G_SOURCE_REMOVE;

  g_mutex_lock(&RT(g)->worker_mutex);
  RT(g)->redraw_source_id = 0;
  g_mutex_unlock(&RT(g)->worker_mutex);

  dt_control_queue_redraw_center();
  return G_SOURCE_REMOVE;
}

static gboolean _async_backend_status_idle(gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g) return G_SOURCE_REMOVE;

  g_mutex_lock(&RT(g)->worker_mutex);
  RT(g)->backend_status_source_id = 0;
  g_mutex_unlock(&RT(g)->worker_mutex);

  return G_SOURCE_REMOVE;
}

static gboolean _async_undo_ui_idle(gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g) return G_SOURCE_REMOVE;

  g_mutex_lock(&RT(g)->worker_mutex);
  RT(g)->undo_ui_source_id = 0;
  g_mutex_unlock(&RT(g)->worker_mutex);

  _sync_undo_redo_buttons(self);
  return G_SOURCE_REMOVE;
}

static void _cancel_async_redraw(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g) return;

  guint source_id = 0;
  g_mutex_lock(&RT(g)->worker_mutex);
  source_id = RT(g)->redraw_source_id;
  RT(g)->redraw_source_id = 0;
  g_mutex_unlock(&RT(g)->worker_mutex);

  if(source_id != 0) g_source_remove(source_id);
}

static void _cancel_async_backend_status(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g) return;

  guint source_id = 0;
  g_mutex_lock(&RT(g)->worker_mutex);
  RT(g)->backend_status_active = FALSE;
  source_id = RT(g)->backend_status_source_id;
  RT(g)->backend_status_source_id = 0;
  g_mutex_unlock(&RT(g)->worker_mutex);

  if(source_id != 0) g_source_remove(source_id);
}

static void _cancel_async_history_tick(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !RT(g)) return;

  guint source_id = 0;
  g_mutex_lock(&RT(g)->worker_mutex);
  source_id = RT(g)->history_tick_source_id;
  RT(g)->history_tick_source_id = 0;
  g_mutex_unlock(&RT(g)->worker_mutex);

  if(source_id != 0) g_source_remove(source_id);
}

static void _rt_schedule_async_undo_ui(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g) return;

  g_mutex_lock(&RT(g)->worker_mutex);
  if(RT(g)->undo_ui_source_id == 0) RT(g)->undo_ui_source_id = g_idle_add(_async_undo_ui_idle, self);
  g_mutex_unlock(&RT(g)->worker_mutex);
}

static void _rt_cancel_async_undo_ui(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g) return;

  guint source_id = 0;
  g_mutex_lock(&RT(g)->worker_mutex);
  source_id = RT(g)->undo_ui_source_id;
  RT(g)->undo_ui_source_id = 0;
  g_mutex_unlock(&RT(g)->worker_mutex);

  if(source_id != 0) g_source_remove(source_id);
}

static void _rt_schedule_async_redraw(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !RT(g)) return;

  g_mutex_lock(&RT(g)->worker_mutex);
  if(RT(g)->redraw_source_id == 0) RT(g)->redraw_source_id = g_idle_add(_async_redraw_idle, self);
  g_mutex_unlock(&RT(g)->worker_mutex);
}

static void _rt_clear_backend_status_active(dt_iop_drawlayer_gui_data_t *g)
{
  if(!g || !RT(g)) return;
  g_mutex_lock(&RT(g)->worker_mutex);
  RT(g)->backend_status_active = FALSE;
  g_mutex_unlock(&RT(g)->worker_mutex);
}

static gboolean _wait_worker_idle(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !RT(g)) return TRUE;

  gboolean have_started_worker = FALSE;
  for(int i = 0; i < DRAWLAYER_RT_WORKER_COUNT; i++)
  {
    if(!_rt_worker_enabled((drawlayer_rt_worker_kind_t)i)) continue;
    if(RT(g)->workers[i].started)
    {
      have_started_worker = TRUE;
      break;
    }
  }
  if(!have_started_worker) return TRUE;

  g_mutex_lock(&RT(g)->worker_mutex);
  while(TRUE)
  {
    gboolean workers_busy = FALSE;
    gboolean workers_running = FALSE;
    for(int i = 0; i < DRAWLAYER_RT_WORKER_COUNT; i++)
    {
      if(!_rt_worker_enabled((drawlayer_rt_worker_kind_t)i)) continue;
      const drawlayer_rt_worker_t *worker = &RT(g)->workers[i];
      workers_busy |= worker->busy || worker->ring_count > 0;
      workers_running |= !worker->stop;
    }
    if(!workers_busy || !workers_running) break;
    g_cond_wait(&RT(g)->worker_cond, &RT(g)->worker_mutex);
  }
  g_mutex_unlock(&RT(g)->worker_mutex);
  return TRUE;
}

typedef struct drawlayer_rt_thread_ctx_t
{
  dt_iop_module_t *self;
  drawlayer_rt_worker_kind_t kind;
} drawlayer_rt_thread_ctx_t;

static void *_drawlayer_worker_main(void *user_data)
{
  drawlayer_rt_thread_ctx_t *ctx = (drawlayer_rt_thread_ctx_t *)user_data;
  dt_iop_module_t *self = ctx ? ctx->self : NULL;
  const drawlayer_rt_worker_kind_t kind = ctx ? ctx->kind : DRAWLAYER_RT_WORKER_PREVIEW;
  g_free(ctx);
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  drawlayer_rt_worker_t *worker = _rt_worker(g, kind);
  const drawlayer_rt_callbacks_t *cb = worker ? worker->callbacks : NULL;
  if(!self || !g || !worker || !cb || !_rt_worker_enabled(kind)) return NULL;

  dt_pthread_setname(cb->thread_name);
  _set_current_thread_realtime_best_effort();

  uint32_t open_batch = 0u;
  gboolean have_open_batch = FALSE;

  while(TRUE)
  {
    drawlayer_rt_event_t event = { 0 };
    gboolean have_event = FALSE;

    g_mutex_lock(&RT(g)->worker_mutex);
    while(!_rt_worker_stop_requested(g, kind) && (_rt_worker_paused(g, kind) || _rt_queue_empty(g, kind)))
    {
      _rt_set_worker_busy(g, kind, FALSE);
      if(cb->on_idle) cb->on_idle(self, g);
      g_cond_broadcast(&RT(g)->worker_cond);
      g_cond_wait(&RT(g)->worker_cond, &RT(g)->worker_mutex);
    }

    if(_rt_worker_stop_requested(g, kind))
    {
      _rt_set_worker_busy(g, kind, FALSE);
      g_cond_broadcast(&RT(g)->worker_cond);
      g_mutex_unlock(&RT(g)->worker_mutex);
      break;
    }

    have_event = _rt_queue_pop_locked(g, kind, &event);
    _rt_set_worker_busy(g, kind, have_event);
    g_mutex_unlock(&RT(g)->worker_mutex);

    if(!have_event) continue;

    const uint32_t event_batch = event.input.stroke_batch;
    if(have_open_batch && event_batch != 0u && event_batch != open_batch)
    {
      if(cb->process_stroke_end) cb->process_stroke_end(self);
      have_open_batch = FALSE;
      open_batch = 0u;
    }

    switch(event.type)
    {
      case DRAWLAYER_RT_EVENT_STROKE_END:
        if(event_batch != 0u)
        {
          have_open_batch = FALSE;
          open_batch = 0u;
        }
        if(cb->process_stroke_end) cb->process_stroke_end(self);
        break;
      case DRAWLAYER_RT_EVENT_SAMPLE:
      default:
        if(event_batch != 0u)
        {
          have_open_batch = TRUE;
          open_batch = event_batch;
        }
        if(cb->process_sample) cb->process_sample(self, &event.input);
        break;
    }

    g_mutex_lock(&RT(g)->worker_mutex);
    _rt_set_worker_busy(g, kind, FALSE);
    if(cb->on_idle) cb->on_idle(self, g);
    g_cond_broadcast(&RT(g)->worker_cond);
    g_mutex_unlock(&RT(g)->worker_mutex);
  }

  return NULL;
}

static gboolean _start_worker(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !RT(g)) return FALSE;

  int started_this_call = 0;
  for(int i = 0; i < DRAWLAYER_RT_WORKER_COUNT; i++)
  {
    drawlayer_rt_worker_t *worker = &RT(g)->workers[i];
    if(!_rt_worker_enabled(worker->kind)) continue;
    if(worker->started) continue;

    worker->stop = FALSE;
    worker->paused = FALSE;
    worker->busy = FALSE;

    drawlayer_rt_thread_ctx_t *ctx = g_malloc(sizeof(*ctx));
    if(!ctx) return FALSE;
    ctx->self = self;
    ctx->kind = worker->kind;

    const int err = dt_pthread_create(&worker->thread, _drawlayer_worker_main, ctx, TRUE);
    if(err != 0)
    {
      g_free(ctx);
      g_mutex_lock(&RT(g)->worker_mutex);
      for(int j = 0; j < DRAWLAYER_RT_WORKER_COUNT; j++)
      {
        if(!_rt_worker_enabled((drawlayer_rt_worker_kind_t)j)) continue;
        if(RT(g)->workers[j].started) RT(g)->workers[j].stop = TRUE;
      }
      g_cond_broadcast(&RT(g)->worker_cond);
      g_mutex_unlock(&RT(g)->worker_mutex);

      for(int j = 0; j < DRAWLAYER_RT_WORKER_COUNT; j++)
      {
        if(!_rt_worker_enabled((drawlayer_rt_worker_kind_t)j)) continue;
        drawlayer_rt_worker_t *started_worker = &RT(g)->workers[j];
        if(started_worker->started)
        {
          pthread_join(started_worker->thread, NULL);
          started_worker->started = FALSE;
          started_worker->stop = FALSE;
          started_worker->paused = FALSE;
          started_worker->busy = FALSE;
        }
      }
      return FALSE;
    }

    worker->started = TRUE;
    started_this_call++;
  }

  return TRUE;
}

static void _stop_worker(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  _cancel_async_redraw(self);
  _cancel_async_backend_status(self);
  _cancel_async_history_tick(self);
  if(!g || !RT(g)) return;

  gboolean have_started_worker = FALSE;
  for(int i = 0; i < DRAWLAYER_RT_WORKER_COUNT; i++)
  {
    if(!_rt_worker_enabled((drawlayer_rt_worker_kind_t)i)) continue;
    if(RT(g)->workers[i].started)
    {
      have_started_worker = TRUE;
      break;
    }
  }
  if(!have_started_worker) return;
  _wait_worker_idle(self);

  g_mutex_lock(&RT(g)->worker_mutex);
  for(int i = 0; i < DRAWLAYER_RT_WORKER_COUNT; i++)
  {
    if(!_rt_worker_enabled((drawlayer_rt_worker_kind_t)i)) continue;
    RT(g)->workers[i].stop = TRUE;
  }
  g_cond_broadcast(&RT(g)->worker_cond);
  g_mutex_unlock(&RT(g)->worker_mutex);

  for(int i = 0; i < DRAWLAYER_RT_WORKER_COUNT; i++)
  {
    if(!_rt_worker_enabled((drawlayer_rt_worker_kind_t)i)) continue;
    drawlayer_rt_worker_t *worker = &RT(g)->workers[i];
    if(worker->started) pthread_join(worker->thread, NULL);
    worker->started = FALSE;
    worker->stop = FALSE;
    worker->paused = FALSE;
    worker->busy = FALSE;
    _rt_queue_clear_locked(g, worker->kind);
  }
}

static void _cancel_async_commit(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g) return;

  guint source_id = 0;
  g_mutex_lock(&RT(g)->worker_mutex);
  g->finish_commit_pending = FALSE;
  source_id = RT(g)->finish_commit_source_id;
  RT(g)->finish_commit_source_id = 0;
  g_mutex_unlock(&RT(g)->worker_mutex);

  if(source_id != 0) g_source_remove(source_id);
}

static void _cancel_async_resync(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g) return;

  guint source_id = 0;
  g_mutex_lock(&RT(g)->worker_mutex);
  g->resync_pending = FALSE;
  g->resync_record_history = FALSE;
  source_id = g->resync_source_id;
  g->resync_source_id = 0;
  g_mutex_unlock(&RT(g)->worker_mutex);

  if(source_id != 0) g_source_remove(source_id);
}

static void _pause_worker(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !RT(g)) return;

  gboolean have_started_worker = FALSE;
  for(int i = 0; i < DRAWLAYER_RT_WORKER_COUNT; i++)
  {
    if(!_rt_worker_enabled((drawlayer_rt_worker_kind_t)i)) continue;
    if(RT(g)->workers[i].started)
    {
      have_started_worker = TRUE;
      break;
    }
  }
  if(!have_started_worker) return;

  g_mutex_lock(&RT(g)->worker_mutex);
  for(int i = 0; i < DRAWLAYER_RT_WORKER_COUNT; i++)
  {
    if(!_rt_worker_enabled((drawlayer_rt_worker_kind_t)i)) continue;
    RT(g)->workers[i].paused = TRUE;
  }
  while(TRUE)
  {
    gboolean worker_busy = FALSE;
    gboolean workers_running = FALSE;
    for(int i = 0; i < DRAWLAYER_RT_WORKER_COUNT; i++)
    {
      if(!_rt_worker_enabled((drawlayer_rt_worker_kind_t)i)) continue;
      const drawlayer_rt_worker_t *worker = &RT(g)->workers[i];
      worker_busy |= worker->busy;
      workers_running |= !worker->stop;
    }
    if(!worker_busy || !workers_running) break;
    g_cond_wait(&RT(g)->worker_cond, &RT(g)->worker_mutex);
  }
  g_mutex_unlock(&RT(g)->worker_mutex);
}

static void _resume_worker(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !RT(g)) return;

  gboolean have_started_worker = FALSE;
  for(int i = 0; i < DRAWLAYER_RT_WORKER_COUNT; i++)
  {
    if(!_rt_worker_enabled((drawlayer_rt_worker_kind_t)i)) continue;
    if(RT(g)->workers[i].started)
    {
      have_started_worker = TRUE;
      break;
    }
  }
  if(!have_started_worker) return;

  g_mutex_lock(&RT(g)->worker_mutex);
  for(int i = 0; i < DRAWLAYER_RT_WORKER_COUNT; i++)
  {
    if(!_rt_worker_enabled((drawlayer_rt_worker_kind_t)i)) continue;
    RT(g)->workers[i].paused = FALSE;
  }
  g_cond_broadcast(&RT(g)->worker_cond);
  g_mutex_unlock(&RT(g)->worker_mutex);
}

static gboolean _enqueue_input(dt_iop_module_t *self, const drawlayer_raw_input_t *input)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !input) return FALSE;
  if(!_start_worker(self)) return FALSE;

  const drawlayer_rt_event_t event = {
    .type = DRAWLAYER_RT_EVENT_SAMPLE,
    .input = *input,
  };

  g_mutex_lock(&RT(g)->worker_mutex);
  gboolean ok = TRUE;
  gboolean coalesced = FALSE;
  for(int i = 0; i < DRAWLAYER_RT_WORKER_COUNT; i++)
  {
    if(!_rt_worker_enabled((drawlayer_rt_worker_kind_t)i)) continue;
    ok &= !_rt_queue_full(g, (drawlayer_rt_worker_kind_t)i);
  }
  if(ok)
  {
    for(int i = 0; i < DRAWLAYER_RT_WORKER_COUNT; i++)
    {
      if(!_rt_worker_enabled((drawlayer_rt_worker_kind_t)i)) continue;
      ok &= _rt_queue_push_locked(g, (drawlayer_rt_worker_kind_t)i, &event);
    }
  }
  if(!ok && input->stroke_pos == DRAWLAYER_STROKE_MIDDLE)
  {
    ok = TRUE;
    coalesced = TRUE;
    for(int i = 0; i < DRAWLAYER_RT_WORKER_COUNT; i++)
    {
      if(!_rt_worker_enabled((drawlayer_rt_worker_kind_t)i)) continue;
      if(!_rt_queue_overwrite_latest_sample_locked(g, (drawlayer_rt_worker_kind_t)i, &event))
      {
        ok = FALSE;
        coalesced = FALSE;
        break;
      }
    }
  }

  if(ok && !coalesced)
    g->stroke_sample_count++;
  if(ok)
    RT(g)->last_input_sample_us = input->event_ts > 0 ? input->event_ts : g_get_monotonic_time();
  else if(!ok)
  {
    RT(g)->dropped_samples++;
    const gint64 now = g_get_monotonic_time();
    if(now - RT(g)->last_queue_overflow_log_us > G_USEC_PER_SEC)
    {
      RT(g)->last_queue_overflow_log_us = now;
      dt_print(DT_DEBUG_PERF, "[drawlayer] worker queue saturated, dropped=%" PRIu64 "\n", RT(g)->dropped_samples);
    }
  }
  g_cond_broadcast(&RT(g)->worker_cond);
  g_mutex_unlock(&RT(g)->worker_mutex);
  return ok;
}

static gboolean _enqueue_stroke_end(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g) return FALSE;
  if(!_start_worker(self)) return FALSE;

  const drawlayer_rt_event_t event = {
    .type = DRAWLAYER_RT_EVENT_STROKE_END,
    .input = {
      .stroke_batch = g->current_stroke_batch,
      .stroke_pos = DRAWLAYER_STROKE_END,
    },
  };

  g_mutex_lock(&RT(g)->worker_mutex);
  gboolean ok = TRUE;
  for(int i = 0; i < DRAWLAYER_RT_WORKER_COUNT; i++)
  {
    if(!_rt_worker_enabled((drawlayer_rt_worker_kind_t)i)) continue;
    ok &= !_rt_queue_full(g, (drawlayer_rt_worker_kind_t)i);
  }
  if(ok)
  {
    for(int i = 0; i < DRAWLAYER_RT_WORKER_COUNT; i++)
    {
      if(!_rt_worker_enabled((drawlayer_rt_worker_kind_t)i)) continue;
      ok &= _rt_queue_push_locked(g, (drawlayer_rt_worker_kind_t)i, &event);
    }
  }
  if(!ok) dt_control_log(_("drawing worker queue is full"));
  g_cond_broadcast(&RT(g)->worker_cond);
  g_mutex_unlock(&RT(g)->worker_mutex);
  return ok;
}
