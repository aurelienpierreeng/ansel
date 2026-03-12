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

/*
 * drawlayer realtime worker subsystem
 *
 * The drawlayer module uses two worker roles:
 * - a realtime backend worker consuming GUI raw input into the live process tile,
 * - a deferred full-resolution worker replaying finished strokes into `base_patch`.
 */

/** @file
 *  @brief Drawlayer realtime worker thread and FIFO event queue.
 */

/** @brief Internal worker slot kinds (currently backend only). */
typedef enum drawlayer_rt_worker_kind_t
{
  DRAWLAYER_RT_WORKER_BACKEND = 0,
  DRAWLAYER_RT_WORKER_COUNT = 1,
} drawlayer_rt_worker_kind_t;

/** @brief Callback signature for one raw-input event processing. */
typedef void (*drawlayer_rt_sample_cb)(dt_iop_module_t *self, dt_drawlayer_worker_t *rt,
                                       const dt_drawlayer_paint_raw_input_t *input);
/** @brief Callback signature for stroke-end event processing. */
typedef void (*drawlayer_rt_stroke_end_cb)(dt_iop_module_t *self, dt_drawlayer_worker_t *rt);
/** @brief Callback signature for idle transitions inside worker loop. */
typedef void (*drawlayer_rt_idle_cb)(dt_iop_module_t *self, dt_drawlayer_worker_t *rt);

/** @brief Per-worker callback vtable. */
typedef struct drawlayer_rt_callbacks_t
{
  const char *thread_name;                     /**< Thread name for diagnostics/profiling. */
  drawlayer_rt_sample_cb process_sample;       /**< Raw input event handler. */
  drawlayer_rt_stroke_end_cb process_stroke_end;/**< Stroke-end handler. */
  drawlayer_rt_idle_cb on_idle;                /**< Idle hook (commit scheduling). */
} drawlayer_rt_callbacks_t;

/** @brief One worker thread runtime including event ring buffer. */
typedef struct drawlayer_rt_worker_t
{
  pthread_t thread;                              /**< POSIX worker thread handle. */
  dt_drawlayer_paint_raw_input_t *ring;          /**< FIFO ring storage. */
  guint ring_capacity;                           /**< Max events in ring. */
  guint ring_head;                               /**< Pop index. */
  guint ring_tail;                               /**< Push index. */
  guint ring_count;                              /**< Current queued events. */
  dt_drawlayer_worker_state_t state;             /**< Consolidated worker lifecycle state. */
  gboolean stop;                                 /**< Stop-request flag. */
} drawlayer_rt_worker_t;

/** @brief One finished stroke queued for deferred full-resolution replay. */
typedef struct drawlayer_finished_stroke_job_t
{
  GArray *raw_inputs;                            /**< Deep-copied raw input history for one stroke. */
} drawlayer_finished_stroke_job_t;

/** @brief Drawlayer worker global state shared with drawlayer module. */
struct dt_drawlayer_worker_t
{
  dt_iop_module_t *self;                        /**< Parent module instance. */
  gboolean *painting;                           /**< External painting-state mirror. */
  gboolean *finish_commit_pending;              /**< External commit-request flag mirror. */
  guint *stroke_sample_count;                   /**< External per-stroke sample counter mirror. */
  uint32_t *current_stroke_batch;               /**< External stroke-batch counter mirror. */
  dt_pthread_mutex_t worker_mutex;              /**< Mutex guarding queue/flags. */
  pthread_cond_t worker_cond;                   /**< Condition variable for worker wakeups. */
  drawlayer_rt_worker_t workers[DRAWLAYER_RT_WORKER_COUNT]; /**< Worker slots. */
  guint finish_commit_source_id;                /**< Pending idle callback id (0 if none). */
  GArray *backend_history;                      /**< Emitted dab history owned by worker. */
  GArray *stroke_raw_inputs;                    /**< Preserved raw input history for current stroke. */
  dt_drawlayer_paint_stroke_t *stroke;          /**< Worker-owned current stroke runtime. */
  pthread_t fullres_thread;                     /**< Deferred full-resolution replay worker. */
  GQueue *finished_stroke_queue;                /**< Queue of finished stroke replay jobs. */
  dt_drawlayer_worker_state_t fullres_state;    /**< Full-resolution worker lifecycle state. */
  gboolean fullres_stop;                        /**< Full-resolution worker stop flag. */
  gboolean finished_stroke_queued;              /**< Current preserved stroke already handed off. */
  dt_drawlayer_worker_finished_stroke_cb finished_stroke_cb; /**< Replay callback owned by drawlayer. */
};

static const drawlayer_rt_callbacks_t _rt_callbacks[DRAWLAYER_RT_WORKER_COUNT];
static void _stop_worker(dt_iop_module_t *self, dt_drawlayer_worker_t *rt);
static gboolean _enqueue_finished_stroke(dt_drawlayer_worker_t *rt);

/** @brief Destroy one finished-stroke replay job and owned history. */
static void _finished_stroke_job_destroy(drawlayer_finished_stroke_job_t *job)
{
  if(!job) return;
  if(job->raw_inputs) g_array_free(job->raw_inputs, TRUE);
  dt_free(job);
}

/** @brief Deep-copy preserved stroke history into one deferred replay job. */
/** @brief Destroy stroke runtime and owned dab window. */
static void _stroke_destroy(dt_drawlayer_worker_t *rt)
{
  if(!rt) return;
  if(rt->stroke && rt->stroke->pending_dabs)
  {
    g_array_free(rt->stroke->pending_dabs, TRUE);
    rt->stroke->pending_dabs = NULL;
  }
  if(rt->stroke && rt->stroke->dab_window)
  {
    g_array_free(rt->stroke->dab_window, TRUE);
    rt->stroke->dab_window = NULL;
  }
  dt_drawlayer_paint_runtime_private_destroy(&rt->stroke);
}

/** @brief Create stroke runtime if missing. */
static gboolean _stroke_create(dt_drawlayer_worker_t *rt)
{
  if(!rt) return FALSE;
  if(rt->stroke) return TRUE;
  rt->stroke = dt_drawlayer_paint_runtime_private_create();
  if(!rt->stroke) return FALSE;
  rt->stroke->history = rt->backend_history;
  rt->stroke->pending_dabs = g_array_new(FALSE, FALSE, sizeof(dt_drawlayer_brush_dab_t));
  if(!rt->stroke->pending_dabs)
  {
    dt_drawlayer_paint_runtime_private_destroy(&rt->stroke);
    return FALSE;
  }
  rt->stroke->dab_window = g_array_new(FALSE, FALSE, sizeof(dt_drawlayer_brush_dab_t));
  if(!rt->stroke->dab_window)
  {
    g_array_free(rt->stroke->pending_dabs, TRUE);
    rt->stroke->pending_dabs = NULL;
    dt_drawlayer_paint_runtime_private_destroy(&rt->stroke);
    return FALSE;
  }
  return TRUE;
}

/** @brief Start new stroke runtime and reset history/path state. */
static gboolean _stroke_begin(dt_drawlayer_worker_t *rt)
{
  if(!rt) return FALSE;
  if(!rt->backend_history)
    rt->backend_history = g_array_new(FALSE, FALSE, sizeof(dt_drawlayer_brush_dab_t));
  if(!rt->backend_history) return FALSE;
  if(!rt->stroke_raw_inputs)
    rt->stroke_raw_inputs = g_array_new(FALSE, FALSE, sizeof(dt_drawlayer_paint_raw_input_t));
  if(!rt->stroke_raw_inputs) return FALSE;
  if(!_stroke_create(rt)) return FALSE;
  rt->stroke->history = rt->backend_history;
  dt_drawlayer_paint_path_state_reset(rt->stroke);
  dt_drawlayer_paint_runtime_private_reset(rt->stroke);
  g_array_set_size(rt->backend_history, 0);
  g_array_set_size(rt->stroke_raw_inputs, 0);
  rt->finished_stroke_queued = FALSE;
  return TRUE;
}

/** @brief Clear current stroke state while preserving allocations. */
static void _stroke_clear(dt_drawlayer_worker_t *rt)
{
  if(!rt) return;
  if(rt->backend_history) g_array_set_size(rt->backend_history, 0);
  if(rt->stroke_raw_inputs) g_array_set_size(rt->stroke_raw_inputs, 0);
  if(rt->stroke)
  {
    dt_drawlayer_paint_path_state_reset(rt->stroke);
    dt_drawlayer_paint_runtime_private_reset(rt->stroke);
  }
  rt->finished_stroke_queued = FALSE;
}

/** @brief Clear queued events (lock must be held). */
static void _rt_queue_clear_locked(dt_drawlayer_worker_t *rt)
{
  drawlayer_rt_worker_t *worker = rt ? &rt->workers[DRAWLAYER_RT_WORKER_BACKEND] : NULL;
  if(!worker) return;
  worker->ring_head = 0;
  worker->ring_tail = 0;
  worker->ring_count = 0;
}

/** @brief Test whether event queue is empty. */
static gboolean _rt_queue_empty(const dt_drawlayer_worker_t *rt)
{
  const drawlayer_rt_worker_t *worker = rt ? &rt->workers[DRAWLAYER_RT_WORKER_BACKEND] : NULL;
  return !worker || worker->ring_count == 0;
}

/** @brief Test whether event queue is full. */
static gboolean _rt_queue_full(const dt_drawlayer_worker_t *rt)
{
  const drawlayer_rt_worker_t *worker = rt ? &rt->workers[DRAWLAYER_RT_WORKER_BACKEND] : NULL;
  return !worker || (worker->ring_capacity > 0 && worker->ring_count >= worker->ring_capacity);
}

/** @brief Push one event in ring queue (lock must be held). */
static gboolean _rt_queue_push_locked(dt_drawlayer_worker_t *rt,
                                      const dt_drawlayer_paint_raw_input_t *event)
{
  drawlayer_rt_worker_t *worker = rt ? &rt->workers[DRAWLAYER_RT_WORKER_BACKEND] : NULL;
  if(!worker || !event || !worker->ring || worker->ring_capacity == 0 || _rt_queue_full(rt)) return FALSE;
  worker->ring[worker->ring_tail] = *event;
  worker->ring_tail = (worker->ring_tail + 1) % worker->ring_capacity;
  worker->ring_count++;
  return TRUE;
}

/** @brief Pop one event from ring queue (lock must be held). */
static gboolean _rt_queue_pop_locked(dt_drawlayer_worker_t *rt,
                                     dt_drawlayer_paint_raw_input_t *event)
{
  drawlayer_rt_worker_t *worker = rt ? &rt->workers[DRAWLAYER_RT_WORKER_BACKEND] : NULL;
  if(!worker || !event || !worker->ring || worker->ring_capacity == 0 || _rt_queue_empty(rt)) return FALSE;
  *event = worker->ring[worker->ring_head];
  worker->ring_head = (worker->ring_head + 1) % worker->ring_capacity;
  worker->ring_count--;
  return TRUE;
}

static inline gboolean _worker_is_started(const drawlayer_rt_worker_t *worker)
{
  return worker && worker->state != DT_DRAWLAYER_WORKER_STATE_STOPPED;
}

static inline gboolean _worker_is_busy(const drawlayer_rt_worker_t *worker)
{
  return worker && worker->state == DT_DRAWLAYER_WORKER_STATE_BUSY;
}

static inline gboolean _worker_pause_requested(const drawlayer_rt_worker_t *worker)
{
  return worker
         && (worker->state == DT_DRAWLAYER_WORKER_STATE_PAUSING
             || worker->state == DT_DRAWLAYER_WORKER_STATE_PAUSED);
}

static inline gboolean _backend_pending_dabs_locked(const dt_drawlayer_worker_t *rt)
{
  return rt && rt->stroke && rt->stroke->pending_dabs && rt->stroke->pending_dabs->len > 0;
}

static inline gboolean _fullres_worker_started(const dt_drawlayer_worker_t *rt)
{
  return rt && rt->fullres_state != DT_DRAWLAYER_WORKER_STATE_STOPPED;
}

static inline gboolean _fullres_worker_busy(const dt_drawlayer_worker_t *rt)
{
  return rt && rt->fullres_state == DT_DRAWLAYER_WORKER_STATE_BUSY;
}

/** @brief Set worker state atomically under caller synchronization. */
static void _rt_set_worker_state(dt_drawlayer_worker_t *rt, const dt_drawlayer_worker_state_t state)
{
  drawlayer_rt_worker_t *worker = rt ? &rt->workers[DRAWLAYER_RT_WORKER_BACKEND] : NULL;
  if(worker) worker->state = state;
}

/** @brief Try elevating current thread scheduling policy for lower-latency input. */
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

/** @brief Check whether workers still have pending activity (lock must be held). */
static gboolean _workers_active_locked(const dt_drawlayer_worker_t *rt)
{
  const drawlayer_rt_worker_t *worker = rt ? &rt->workers[DRAWLAYER_RT_WORKER_BACKEND] : NULL;
  return worker && (_worker_is_busy(worker) || worker->ring_count > 0
                    || (rt->finish_commit_pending && *rt->finish_commit_pending));
}

/** @brief Check whether deferred full-resolution replay still has pending activity (lock must be held). */
static gboolean _fullres_active_locked(const dt_drawlayer_worker_t *rt)
{
  return rt && (_fullres_worker_busy(rt) || rt->fullres_stop
                || (rt->finished_stroke_queue && !g_queue_is_empty(rt->finished_stroke_queue)));
}

/** @brief Check whether any worker activity remains (backend or deferred replay). */
static gboolean _workers_any_active_locked(const dt_drawlayer_worker_t *rt)
{
  return _workers_active_locked(rt) || _fullres_active_locked(rt);
}

/** @brief Check if workers are idle and commit can be safely scheduled. */
static gboolean _workers_ready_for_commit_locked(const dt_drawlayer_worker_t *rt)
{
  const drawlayer_rt_worker_t *worker = rt ? &rt->workers[DRAWLAYER_RT_WORKER_BACKEND] : NULL;
  return worker && rt->finish_commit_pending && rt->painting
         && *rt->finish_commit_pending && !*rt->painting
         && !_worker_is_busy(worker) && worker->ring_count == 0;
}

/** @brief Thread-safe wrapper for active-workers status. */
static gboolean _rt_workers_active(dt_drawlayer_worker_t *rt)
{
  gboolean active = FALSE;
  if(!rt) return FALSE;
  dt_pthread_mutex_lock(&rt->worker_mutex);
  active = _workers_active_locked(rt);
  dt_pthread_mutex_unlock(&rt->worker_mutex);
  return active;
}

/** @brief Thread-safe wrapper for any worker activity, including deferred replay. */
static gboolean _rt_workers_any_active(dt_drawlayer_worker_t *rt)
{
  gboolean active = FALSE;
  if(!rt) return FALSE;
  dt_pthread_mutex_lock(&rt->worker_mutex);
  active = _workers_any_active_locked(rt);
  dt_pthread_mutex_unlock(&rt->worker_mutex);
  return active;
}

/** @brief Idle callback committing pending stroke once workers are fully idle. */
static gboolean _async_commit_idle(gpointer user_data)
{
  dt_drawlayer_worker_t *rt = (dt_drawlayer_worker_t *)user_data;
  dt_iop_module_t *self = rt ? rt->self : NULL;
  if(!rt || !self || !self->dev) return G_SOURCE_REMOVE;

  gboolean should_commit = FALSE;
  dt_pthread_mutex_lock(&rt->worker_mutex);
  rt->finish_commit_source_id = 0;
  should_commit = _workers_ready_for_commit_locked(rt);
  dt_pthread_mutex_unlock(&rt->worker_mutex);

  if(should_commit) _commit_dabs(self, TRUE);
  return G_SOURCE_REMOVE;
}

/** @brief Schedule async commit when lock-state indicates readiness. */
static void _schedule_async_commit_if_ready_locked(dt_drawlayer_worker_t *rt)
{
  if(!rt || !rt->self || !rt->self->dev) return;
  if(_workers_ready_for_commit_locked(rt) && rt->finish_commit_source_id == 0)
    rt->finish_commit_source_id = g_idle_add(_async_commit_idle, rt);
}

/** @brief Backend-worker idle hook. */
static void _backend_worker_on_idle(dt_iop_module_t *self, dt_drawlayer_worker_t *rt)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  drawlayer_paint_backend_ctx_t ctx = {
    .self = self,
    .stroke = rt ? rt->stroke : NULL,
  };
  if(self && g && rt && rt->stroke && rt->stroke->pending_dabs && rt->stroke->pending_dabs->len > 0)
  {
    const gint64 live_publish_interval_us
        = MAX((gint64)dt_gui_throttle_get_pipe_runtime_us(DT_DEV_PIXELPIPE_FULL), (gint64)20000);

    dt_drawlayer_cache_patch_wrlock(&g->process.process_patch);
    guint processed_dabs = 0;
    const double batch_t0 = dt_get_wtime();
    while(processed_dabs < rt->stroke->pending_dabs->len)
    {
      const dt_drawlayer_brush_dab_t *dab
          = &g_array_index(rt->stroke->pending_dabs, dt_drawlayer_brush_dab_t, processed_dabs);
      _process_backend_dab(self, dab, &ctx);
      processed_dabs++;
      const gint64 elapsed_us = (gint64)(1000000.0 * (dt_get_wtime() - batch_t0));
      if(elapsed_us >= live_publish_interval_us) break;
    }
    if(processed_dabs > 0) g_array_remove_range(rt->stroke->pending_dabs, 0, processed_dabs);
    dt_drawlayer_cache_patch_wrunlock(&g->process.process_patch);
    if(processed_dabs > 0) _publish_backend_progress(&ctx, TRUE);
    return;
  }

  if(!rt) return;
  dt_pthread_mutex_lock(&rt->worker_mutex);
  _schedule_async_commit_if_ready_locked(rt);
  dt_pthread_mutex_unlock(&rt->worker_mutex);
}

/** @brief Process one backend raw input event. */
static void _backend_worker_process_sample(dt_iop_module_t *self, dt_drawlayer_worker_t *rt,
                                           const dt_drawlayer_paint_raw_input_t *input)
{
  if(!rt || !input) return;
  if(rt && input && input->stroke_pos == DT_DRAWLAYER_PAINT_STROKE_FIRST)
  {
    _stroke_begin(rt);
  }

  if(!rt->stroke && !_stroke_begin(rt)) return;
  if(rt->stroke_raw_inputs) g_array_append_val(rt->stroke_raw_inputs, *input);
  _process_backend_input(self, input, rt->stroke);
}

/** @brief Handle backend stroke end: flush, reset, and request commit. */
static void _backend_worker_process_stroke_end(dt_iop_module_t *self, dt_drawlayer_worker_t *rt)
{
  if(!rt) return;

  /* Worker only drains per-stroke pending samples. Stroke-damage rectangles are
   * consumed/applied by drawlayer during commit, after worker idle is reached. */
  if(rt->stroke)
  {
    dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
    drawlayer_paint_backend_ctx_t ctx = {
      .self = self,
      .stroke = rt->stroke,
    };
    const dt_drawlayer_paint_callbacks_t callbacks = {
      .build_dab = _paint_build_dab_cb,
      .layer_to_widget = _paint_layer_to_widget_cb,
      .emit_dab = _paint_emit_backend_dab_cb,
      .on_stroke_seed = _paint_stroke_seed_cb,
    };
    if(g)
    {
      dt_drawlayer_paint_finalize_path(rt->stroke, &callbacks, &ctx);
      dt_drawlayer_cache_patch_wrlock(&g->process.process_patch);
      if(rt->stroke->pending_dabs)
      {
        for(guint i = 0; i < rt->stroke->pending_dabs->len; i++)
        {
          const dt_drawlayer_brush_dab_t *dab
              = &g_array_index(rt->stroke->pending_dabs, dt_drawlayer_brush_dab_t, i);
          _process_backend_dab(self, dab, &ctx);
        }
        g_array_set_size(rt->stroke->pending_dabs, 0);
      }
      dt_drawlayer_cache_patch_wrunlock(&g->process.process_patch);
    }
    _publish_backend_progress(&ctx, TRUE);
  }
  _enqueue_finished_stroke(rt);
  dt_pthread_mutex_lock(&rt->worker_mutex);
  if(rt->finish_commit_pending) *rt->finish_commit_pending = TRUE;
  _schedule_async_commit_if_ready_locked(rt);
  pthread_cond_broadcast(&rt->worker_cond);
  dt_pthread_mutex_unlock(&rt->worker_mutex);
}

static const drawlayer_rt_callbacks_t _rt_callbacks[] = {
  [DRAWLAYER_RT_WORKER_BACKEND] = {
    .thread_name = "draw-back",
    .process_sample = _backend_worker_process_sample,
    .process_stroke_end = _backend_worker_process_stroke_end,
    .on_idle = _backend_worker_on_idle,
  },
};

/** @brief Stop and free an existing worker state object. */
static void _rt_destroy_state(dt_iop_module_t *self, dt_drawlayer_worker_t **rt_out)
{
  if(!rt_out || !*rt_out) return;
  dt_drawlayer_worker_t *rt = *rt_out;
  drawlayer_rt_worker_t *worker = &rt->workers[DRAWLAYER_RT_WORKER_BACKEND];
  _stop_worker(self ? self : rt->self, rt);
  _stroke_destroy(rt);
  if(rt->backend_history) g_array_free(rt->backend_history, TRUE);
  if(rt->stroke_raw_inputs) g_array_free(rt->stroke_raw_inputs, TRUE);
  if(rt->finished_stroke_queue)
  {
    while(!g_queue_is_empty(rt->finished_stroke_queue))
      _finished_stroke_job_destroy((drawlayer_finished_stroke_job_t *)g_queue_pop_head(rt->finished_stroke_queue));
    g_queue_free(rt->finished_stroke_queue);
  }
  dt_free(worker->ring);
  pthread_cond_destroy(&rt->worker_cond);
  dt_pthread_mutex_destroy(&rt->worker_mutex);
  dt_free(rt);
  *rt_out = NULL;
}

/** @brief Allocate and initialize worker state object and buffers. */
static void _rt_init_state(dt_iop_module_t *self, dt_drawlayer_worker_t **rt_out,
                           gboolean *painting, gboolean *finish_commit_pending,
                           guint *stroke_sample_count, uint32_t *current_stroke_batch,
                           dt_drawlayer_worker_finished_stroke_cb finished_stroke_cb)
{
  if(!rt_out) return;
  _rt_destroy_state(self, rt_out);
  dt_drawlayer_worker_t *rt = NULL;
  rt = g_malloc0(sizeof(*rt));
  *rt_out = rt;
  if(!rt) return;

  rt->self = self;
  rt->painting = painting;
  rt->finish_commit_pending = finish_commit_pending;
  rt->stroke_sample_count = stroke_sample_count;
  rt->current_stroke_batch = current_stroke_batch;
  rt->finished_stroke_cb = finished_stroke_cb;
  dt_pthread_mutex_init(&rt->worker_mutex, NULL);
  pthread_cond_init(&rt->worker_cond, NULL);

  drawlayer_rt_worker_t *worker = rt ? &rt->workers[DRAWLAYER_RT_WORKER_BACKEND] : NULL;
  worker->ring_capacity = DRAWLAYER_WORKER_RING_CAPACITY;
  worker->ring = g_malloc_n(worker->ring_capacity, sizeof(dt_drawlayer_paint_raw_input_t));
  rt->backend_history = g_array_new(FALSE, FALSE, sizeof(dt_drawlayer_brush_dab_t));
  rt->stroke_raw_inputs = g_array_new(FALSE, FALSE, sizeof(dt_drawlayer_paint_raw_input_t));
  rt->finished_stroke_queue = g_queue_new();
  _stroke_create(rt);
}

/** @brief Destroy worker state object and all owned resources. */
static void _rt_cleanup_state(dt_drawlayer_worker_t **rt_out)
{
  _rt_destroy_state(NULL, rt_out);
}

/** @brief Wait until worker queue is drained and not busy. */
static gboolean _wait_worker_idle(dt_iop_module_t *self, dt_drawlayer_worker_t *rt)
{
  (void)self;
  const drawlayer_rt_worker_t *worker = rt ? &rt->workers[DRAWLAYER_RT_WORKER_BACKEND] : NULL;
  if(!rt || !worker || !_worker_is_started(worker)) return TRUE;

  dt_pthread_mutex_lock(&rt->worker_mutex);
  while(_worker_is_busy(worker) || worker->ring_count > 0)
  {
    if(worker->stop) break;
    dt_pthread_cond_wait(&rt->worker_cond, &rt->worker_mutex);
  }
  dt_pthread_mutex_unlock(&rt->worker_mutex);
  return TRUE;
}

typedef struct drawlayer_rt_thread_ctx_t
{
  dt_drawlayer_worker_t *rt;
} drawlayer_rt_thread_ctx_t;

/** @brief Deferred full-resolution replay worker main loop. */
static void *_drawlayer_fullres_worker_main(void *user_data)
{
  drawlayer_rt_thread_ctx_t *ctx = (drawlayer_rt_thread_ctx_t *)user_data;
  dt_drawlayer_worker_t *rt = ctx ? ctx->rt : NULL;
  dt_free(ctx);

  dt_iop_module_t *self = rt ? rt->self : NULL;
  if(!self || !rt) return NULL;

  dt_pthread_setname("draw-base");

  while(TRUE)
  {
    drawlayer_finished_stroke_job_t *job = NULL;

    dt_pthread_mutex_lock(&rt->worker_mutex);
    while(!rt->fullres_stop && (!rt->finished_stroke_queue || g_queue_is_empty(rt->finished_stroke_queue)))
    {
      rt->fullres_state = DT_DRAWLAYER_WORKER_STATE_IDLE;
      pthread_cond_broadcast(&rt->worker_cond);
      dt_pthread_cond_wait(&rt->worker_cond, &rt->worker_mutex);
    }

    if(rt->fullres_stop)
    {
      rt->fullres_state = DT_DRAWLAYER_WORKER_STATE_IDLE;
      pthread_cond_broadcast(&rt->worker_cond);
      dt_pthread_mutex_unlock(&rt->worker_mutex);
      break;
    }

    job = (drawlayer_finished_stroke_job_t *)g_queue_pop_head(rt->finished_stroke_queue);
    rt->fullres_state = job ? DT_DRAWLAYER_WORKER_STATE_BUSY : DT_DRAWLAYER_WORKER_STATE_IDLE;
    dt_pthread_mutex_unlock(&rt->worker_mutex);

    if(job && rt->finished_stroke_cb)
      rt->finished_stroke_cb(self, job->raw_inputs);

    _finished_stroke_job_destroy(job);

    dt_pthread_mutex_lock(&rt->worker_mutex);
    rt->fullres_state = DT_DRAWLAYER_WORKER_STATE_IDLE;
    pthread_cond_broadcast(&rt->worker_cond);
    dt_pthread_mutex_unlock(&rt->worker_mutex);
  }

  return NULL;
}

/** @brief Start deferred full-resolution replay worker if not running. */
static gboolean _start_fullres_worker(dt_drawlayer_worker_t *rt)
{
  if(!rt) return FALSE;
  if(_fullres_worker_started(rt)) return TRUE;

  rt->fullres_stop = FALSE;
  rt->fullres_state = DT_DRAWLAYER_WORKER_STATE_IDLE;

  drawlayer_rt_thread_ctx_t *ctx = g_malloc(sizeof(*ctx));
  if(!ctx) return FALSE;
  ctx->rt = rt;

  const int err = dt_pthread_create(&rt->fullres_thread, _drawlayer_fullres_worker_main, ctx, TRUE);
  if(err != 0)
  {
    dt_free(ctx);
    return FALSE;
  }

  rt->fullres_state = DT_DRAWLAYER_WORKER_STATE_IDLE;
  return TRUE;
}

/** @brief Wait until deferred full-resolution replay queue is drained and idle. */
static void _wait_fullres_idle(dt_drawlayer_worker_t *rt)
{
  if(!rt || !_fullres_worker_started(rt)) return;

  dt_pthread_mutex_lock(&rt->worker_mutex);
  while(_fullres_worker_busy(rt) || (rt->finished_stroke_queue && !g_queue_is_empty(rt->finished_stroke_queue)))
  {
    if(rt->fullres_stop) break;
    dt_pthread_cond_wait(&rt->worker_cond, &rt->worker_mutex);
  }
  dt_pthread_mutex_unlock(&rt->worker_mutex);
}

/** @brief Queue preserved finished stroke for deferred full-resolution replay (lock must be held). */
static gboolean _enqueue_finished_stroke(dt_drawlayer_worker_t *rt)
{
  if(!rt || rt->finished_stroke_queued) return TRUE;
  if(!rt->finished_stroke_cb || !rt->stroke_raw_inputs || rt->stroke_raw_inputs->len == 0) return FALSE;

  if(!_start_fullres_worker(rt))
  {
    return FALSE;
  }

  GQueue *new_queue = NULL;
  if(!rt->finished_stroke_queue)
  {
    new_queue = g_queue_new();
    if(!new_queue)
      return FALSE;
  }

  GArray *next_raw_inputs = g_array_new(FALSE, FALSE, sizeof(dt_drawlayer_paint_raw_input_t));
  if(!next_raw_inputs)
  {
    if(new_queue) g_queue_free(new_queue);
    return FALSE;
  }

  drawlayer_finished_stroke_job_t *job = g_malloc0(sizeof(*job));
  if(!job)
  {
    if(new_queue) g_queue_free(new_queue);
    g_array_free(next_raw_inputs, TRUE);
    return FALSE;
  }

  gboolean queued = FALSE;
  dt_pthread_mutex_lock(&rt->worker_mutex);
  if(!rt->finished_stroke_queue && new_queue)
  {
    rt->finished_stroke_queue = new_queue;
    new_queue = NULL;
  }
  if(rt->finished_stroke_queued)
    queued = TRUE;
  else if(rt->finished_stroke_queue)
  {
    job->raw_inputs = rt->stroke_raw_inputs;
    g_queue_push_tail(rt->finished_stroke_queue, job);
    rt->stroke_raw_inputs = next_raw_inputs;
    rt->finished_stroke_queued = TRUE;
    queued = TRUE;
    next_raw_inputs = NULL;
    job = NULL;
    pthread_cond_broadcast(&rt->worker_cond);
  }
  dt_pthread_mutex_unlock(&rt->worker_mutex);

  if(new_queue) g_queue_free(new_queue);
  if(next_raw_inputs) g_array_free(next_raw_inputs, TRUE);
  _finished_stroke_job_destroy(job);
  return queued;
}

/** @brief Worker main loop: FIFO dequeue, process, and idle scheduling. */
static void *_drawlayer_worker_main(void *user_data)
{
  drawlayer_rt_thread_ctx_t *ctx = (drawlayer_rt_thread_ctx_t *)user_data;
  dt_drawlayer_worker_t *rt = ctx ? ctx->rt : NULL;
  dt_free(ctx);

  dt_iop_module_t *self = rt ? rt->self : NULL;
  drawlayer_rt_worker_t *worker = rt ? &rt->workers[DRAWLAYER_RT_WORKER_BACKEND] : NULL;
  const drawlayer_rt_callbacks_t *cb = &_rt_callbacks[DRAWLAYER_RT_WORKER_BACKEND];
  if(!self || !rt || !worker || !cb) return NULL;

  dt_pthread_setname(cb->thread_name);
  _set_current_thread_realtime_best_effort();

  while(TRUE)
  {
    dt_drawlayer_paint_raw_input_t event = { 0 };
    gboolean have_event = FALSE;
    gboolean have_backlog = FALSE;

    dt_pthread_mutex_lock(&rt->worker_mutex);
    while(!worker->stop && (_worker_pause_requested(worker)
                            || (worker->ring_count == 0 && !_backend_pending_dabs_locked(rt))))
    {
      _rt_set_worker_state(rt, _worker_pause_requested(worker) ? DT_DRAWLAYER_WORKER_STATE_PAUSED
                                                               : DT_DRAWLAYER_WORKER_STATE_IDLE);
      pthread_cond_broadcast(&rt->worker_cond);
      dt_pthread_mutex_unlock(&rt->worker_mutex);
      if(cb->on_idle) cb->on_idle(self, rt);
      dt_pthread_mutex_lock(&rt->worker_mutex);
      if(!worker->stop && (_worker_pause_requested(worker)
                           || (worker->ring_count == 0 && !_backend_pending_dabs_locked(rt))))
        dt_pthread_cond_wait(&rt->worker_cond, &rt->worker_mutex);
    }

    if(worker->stop)
    {
      _rt_set_worker_state(rt, DT_DRAWLAYER_WORKER_STATE_STOPPED);
      pthread_cond_broadcast(&rt->worker_cond);
      dt_pthread_mutex_unlock(&rt->worker_mutex);
      break;
    }

    have_backlog = _backend_pending_dabs_locked(rt);
    have_event = _rt_queue_pop_locked(rt, &event);
    _rt_set_worker_state(rt, (have_event || have_backlog) ? DT_DRAWLAYER_WORKER_STATE_BUSY
                                                          : DT_DRAWLAYER_WORKER_STATE_IDLE);
    dt_pthread_mutex_unlock(&rt->worker_mutex);

    if(!have_event)
    {
      if(cb->on_idle) cb->on_idle(self, rt);
      continue;
    }

    if(event.stroke_pos == DT_DRAWLAYER_PAINT_STROKE_END)
    {
      /* If stroke-end carries a real raw sample (button release), process it
       * before closing the stroke so the final segment is not lost. */
      if(cb->process_sample && event.event_ts != 0)
        cb->process_sample(self, rt, &event);
      if(cb->process_stroke_end) cb->process_stroke_end(self, rt);
    }
    else
    {
      if(cb->process_sample) cb->process_sample(self, rt, &event);
    }

    dt_pthread_mutex_lock(&rt->worker_mutex);
    have_backlog = _backend_pending_dabs_locked(rt);
    _rt_set_worker_state(rt, _worker_pause_requested(worker) ? DT_DRAWLAYER_WORKER_STATE_PAUSED
                                                             : (have_backlog ? DT_DRAWLAYER_WORKER_STATE_BUSY
                                                                             : DT_DRAWLAYER_WORKER_STATE_IDLE));
    pthread_cond_broadcast(&rt->worker_cond);
    dt_pthread_mutex_unlock(&rt->worker_mutex);
    if(cb->on_idle) cb->on_idle(self, rt);
  }

  return NULL;
}

/** @brief Start backend worker thread if not running. */
static gboolean _start_worker(dt_iop_module_t *self, dt_drawlayer_worker_t *rt)
{
  drawlayer_rt_worker_t *worker = rt ? &rt->workers[DRAWLAYER_RT_WORKER_BACKEND] : NULL;
  if(!self || !rt || !worker) return FALSE;
  if(_worker_is_started(worker))
  {
    if(!_fullres_worker_started(rt)) _start_fullres_worker(rt);
    return TRUE;
  }

  worker->stop = FALSE;
  worker->state = DT_DRAWLAYER_WORKER_STATE_IDLE;

  drawlayer_rt_thread_ctx_t *ctx = g_malloc(sizeof(*ctx));
  if(!ctx) return FALSE;
  ctx->rt = rt;

  const int err = dt_pthread_create(&worker->thread, _drawlayer_worker_main, ctx, TRUE);
  if(err != 0)
  {
    dt_free(ctx);
    return FALSE;
  }

  worker->state = DT_DRAWLAYER_WORKER_STATE_IDLE;
  if(!_fullres_worker_started(rt)) _start_fullres_worker(rt);
  return TRUE;
}

/** @brief Cancel pending async commit idle callback if any. */
static void _cancel_async_commit(dt_drawlayer_worker_t *rt)
{
  if(!rt) return;

  guint source_id = 0;
  dt_pthread_mutex_lock(&rt->worker_mutex);
  if(rt->finish_commit_pending) *rt->finish_commit_pending = FALSE;
  source_id = rt->finish_commit_source_id;
  rt->finish_commit_source_id = 0;
  dt_pthread_mutex_unlock(&rt->worker_mutex);

  if(source_id != 0) g_source_remove(source_id);
}

/** @brief Stop worker thread and clear transient state. */
static void _stop_worker(dt_iop_module_t *self, dt_drawlayer_worker_t *rt)
{
  drawlayer_rt_worker_t *worker = rt ? &rt->workers[DRAWLAYER_RT_WORKER_BACKEND] : NULL;
  _cancel_async_commit(rt);
  if(!rt) return;

  if(worker && _worker_is_started(worker))
  {
    _wait_worker_idle(self, rt);

    dt_pthread_mutex_lock(&rt->worker_mutex);
    worker->stop = TRUE;
    pthread_cond_broadcast(&rt->worker_cond);
    dt_pthread_mutex_unlock(&rt->worker_mutex);

    pthread_join(worker->thread, NULL);
    worker->state = DT_DRAWLAYER_WORKER_STATE_STOPPED;
    worker->stop = FALSE;
    _rt_queue_clear_locked(rt);
    _stroke_clear(rt);
  }

  if(_fullres_worker_started(rt))
  {
    _wait_fullres_idle(rt);

    dt_pthread_mutex_lock(&rt->worker_mutex);
    rt->fullres_stop = TRUE;
    pthread_cond_broadcast(&rt->worker_cond);
    dt_pthread_mutex_unlock(&rt->worker_mutex);

    pthread_join(rt->fullres_thread, NULL);
    rt->fullres_state = DT_DRAWLAYER_WORKER_STATE_STOPPED;
    rt->fullres_stop = FALSE;
  }
}

/** @brief Pause worker processing after current callback returns. */
static void _pause_worker(dt_iop_module_t *self, dt_drawlayer_worker_t *rt)
{
  (void)self;
  drawlayer_rt_worker_t *worker = rt ? &rt->workers[DRAWLAYER_RT_WORKER_BACKEND] : NULL;
  if(!rt || !worker || !_worker_is_started(worker)) return;

  dt_pthread_mutex_lock(&rt->worker_mutex);
  worker->state = _worker_is_busy(worker) ? DT_DRAWLAYER_WORKER_STATE_PAUSING
                                          : DT_DRAWLAYER_WORKER_STATE_PAUSED;
  while(_worker_is_busy(worker) && !worker->stop)
    dt_pthread_cond_wait(&rt->worker_cond, &rt->worker_mutex);
  dt_pthread_mutex_unlock(&rt->worker_mutex);
}

/** @brief Resume worker processing and wake sleeping thread. */
static void _resume_worker(dt_iop_module_t *self, dt_drawlayer_worker_t *rt)
{
  (void)self;
  drawlayer_rt_worker_t *worker = rt ? &rt->workers[DRAWLAYER_RT_WORKER_BACKEND] : NULL;
  if(!rt || !worker || !_worker_is_started(worker)) return;

  dt_pthread_mutex_lock(&rt->worker_mutex);
  worker->state = DT_DRAWLAYER_WORKER_STATE_IDLE;
  pthread_cond_broadcast(&rt->worker_cond);
  dt_pthread_mutex_unlock(&rt->worker_mutex);
}

/** @brief Generic enqueue helper ensuring worker startup. */
static gboolean _enqueue_event(dt_iop_module_t *self, dt_drawlayer_worker_t *rt,
                               const dt_drawlayer_paint_raw_input_t *event)
{
  if(!rt || !event) return FALSE;
  if(!_start_worker(self, rt)) return FALSE;

  dt_pthread_mutex_lock(&rt->worker_mutex);
  const gboolean ok = _rt_queue_push_locked(rt, event);
  pthread_cond_broadcast(&rt->worker_cond);
  dt_pthread_mutex_unlock(&rt->worker_mutex);
  return ok;
}

/** @brief Enqueue raw input with saturation policy and stroke-abort fallback. */
static gboolean _enqueue_input(dt_iop_module_t *self, dt_drawlayer_worker_t *rt,
                               const dt_drawlayer_paint_raw_input_t *input)
{
  if(!rt || !input) return FALSE;
  if(!_start_worker(self, rt)) return FALSE;

  const dt_drawlayer_paint_raw_input_t event = *input;

  gboolean ok = FALSE;
  dt_pthread_mutex_lock(&rt->worker_mutex);
  if(!_rt_queue_full(rt))
  {
    ok = _rt_queue_push_locked(rt, &event);
    if(ok && rt->stroke_sample_count) (*rt->stroke_sample_count)++;
  }
  if(!ok)
  {
    /* Saturated queue policy: abort current stroke deterministically instead of
     * silently dropping or coalescing raw input events. Keep FIFO backlog and
     * force a stroke end at the newest queued slot. */
    drawlayer_rt_worker_t *worker = &rt->workers[DRAWLAYER_RT_WORKER_BACKEND];
    dt_drawlayer_paint_raw_input_t end_input = *input;
    end_input.stroke_pos = DT_DRAWLAYER_PAINT_STROKE_END;
    end_input.event_index = input->event_index + 1u;
    const dt_drawlayer_paint_raw_input_t end_event = end_input;
    if(worker->ring && worker->ring_count > 0)
    {
      const guint last_index = (worker->ring_tail + worker->ring_capacity - 1) % worker->ring_capacity;
      worker->ring[last_index] = end_event;
      ok = TRUE;
    }
    else
      ok = _rt_queue_push_locked(rt, &end_event);

    if(rt->finish_commit_pending) *rt->finish_commit_pending = TRUE;
    dt_control_log(_("drawing worker queue is full, stroke aborted"));
  }
  pthread_cond_broadcast(&rt->worker_cond);
  dt_pthread_mutex_unlock(&rt->worker_mutex);
  return ok;
}

/** @brief Enqueue explicit stroke-end event (with optional raw release sample). */
static gboolean _enqueue_stroke_end(dt_iop_module_t *self, dt_drawlayer_worker_t *rt,
                                    const dt_drawlayer_paint_raw_input_t *input)
{
  if(!rt) return FALSE;

  dt_drawlayer_paint_raw_input_t end_input = { 0 };
  if(input) end_input = *input;
  if(end_input.stroke_batch == 0u && rt->current_stroke_batch)
    end_input.stroke_batch = *rt->current_stroke_batch;
  end_input.stroke_pos = DT_DRAWLAYER_PAINT_STROKE_END;

  if(_enqueue_event(self, rt, &end_input)) return TRUE;
  dt_control_log(_("drawing worker queue is full"));
  return FALSE;
}

/** @brief Public worker initialization entry point. */
void dt_drawlayer_worker_init(dt_iop_module_t *self, dt_drawlayer_worker_t **worker,
                              gboolean *painting, gboolean *finish_commit_pending,
                              guint *stroke_sample_count, uint32_t *current_stroke_batch,
                              dt_drawlayer_worker_finished_stroke_cb finished_stroke_cb)
{
  _rt_init_state(self, worker, painting, finish_commit_pending, stroke_sample_count, current_stroke_batch,
                 finished_stroke_cb);
}

/** @brief Public worker cleanup entry point. */
void dt_drawlayer_worker_cleanup(dt_drawlayer_worker_t **worker)
{
  _rt_cleanup_state(worker);
}

/** @brief Public status query: TRUE when worker has pending activity. */
gboolean dt_drawlayer_worker_active(const dt_drawlayer_worker_t *worker)
{
  return _rt_workers_active((dt_drawlayer_worker_t *)worker);
}

/** @brief Public status query: TRUE when any worker still has pending activity. */
gboolean dt_drawlayer_worker_any_active(const dt_drawlayer_worker_t *worker)
{
  return _rt_workers_any_active((dt_drawlayer_worker_t *)worker);
}

void dt_drawlayer_worker_get_snapshot(const dt_drawlayer_worker_t *worker,
                                      dt_drawlayer_worker_snapshot_t *snapshot)
{
  if(snapshot) *snapshot = (dt_drawlayer_worker_snapshot_t){ 0 };
  dt_drawlayer_worker_t *rt = (dt_drawlayer_worker_t *)worker;
  if(!rt || !snapshot) return;

  dt_pthread_mutex_lock(&rt->worker_mutex);
  const drawlayer_rt_worker_t *backend = &rt->workers[DRAWLAYER_RT_WORKER_BACKEND];
  snapshot->backend_state = backend->state;
  snapshot->backend_queue_count = backend->ring_count;
  snapshot->fullres_state = rt->fullres_state;
  snapshot->fullres_queue_count = (rt->finished_stroke_queue) ? (guint)rt->finished_stroke_queue->length : 0u;
  snapshot->commit_pending = rt->finish_commit_pending && *rt->finish_commit_pending;
  dt_pthread_mutex_unlock(&rt->worker_mutex);
}

/** @brief Public commit request helper. */
void dt_drawlayer_worker_request_commit(dt_drawlayer_worker_t *worker)
{
  if(!worker) return;
  dt_pthread_mutex_lock(&worker->worker_mutex);
  if(worker->finish_commit_pending) *worker->finish_commit_pending = TRUE;
  _schedule_async_commit_if_ready_locked(worker);
  pthread_cond_broadcast(&worker->worker_cond);
  dt_pthread_mutex_unlock(&worker->worker_mutex);
}

/** @brief Flush pending backend stroke inputs synchronously. */
void dt_drawlayer_worker_flush_pending(dt_drawlayer_worker_t *worker)
{
  if(!worker || !worker->self) return;
  _wait_worker_idle(worker->self, worker);
}

void dt_drawlayer_worker_seal_for_commit(dt_drawlayer_worker_t *worker)
{
  if(!worker || !worker->self) return;

  /* Quiet commit paths do not need the live raster worker to finish stamping
   * every display-sized dab. The authoritative stroke result is replayed later
   * from preserved raw inputs, so we pause after the current callback, fold any
   * queued raw inputs into that preserved history, and discard unreplayed live
   * dab backlog. */
  _pause_worker(worker->self, worker);

  dt_pthread_mutex_lock(&worker->worker_mutex);
  if(!worker->stroke_raw_inputs)
    worker->stroke_raw_inputs = g_array_new(FALSE, FALSE, sizeof(dt_drawlayer_paint_raw_input_t));

  dt_drawlayer_paint_raw_input_t event = { 0 };
  while(worker->stroke_raw_inputs && _rt_queue_pop_locked(worker, &event))
    g_array_append_val(worker->stroke_raw_inputs, event);

  if(worker->stroke && worker->stroke->pending_dabs)
    g_array_set_size(worker->stroke->pending_dabs, 0);
  if(worker->stroke && worker->stroke->dab_window)
    g_array_set_size(worker->stroke->dab_window, 0);

  worker->workers[DRAWLAYER_RT_WORKER_BACKEND].state = DT_DRAWLAYER_WORKER_STATE_IDLE;
  pthread_cond_broadcast(&worker->worker_cond);
  dt_pthread_mutex_unlock(&worker->worker_mutex);
}

/** @brief Wait until deferred full-resolution replay queue becomes idle. */
void dt_drawlayer_worker_flush_finished_strokes(dt_drawlayer_worker_t *worker)
{
  _wait_fullres_idle(worker);
}

/** @brief Clear preserved stroke runtime/history after commit completed. */
void dt_drawlayer_worker_reset_stroke(dt_drawlayer_worker_t *worker)
{
  if(!worker) return;
  dt_pthread_mutex_lock(&worker->worker_mutex);
  _stroke_clear(worker);
  dt_pthread_mutex_unlock(&worker->worker_mutex);
}

/** @brief Read-only access to preserved raw input history. */
GArray *dt_drawlayer_worker_raw_inputs(dt_drawlayer_worker_t *worker)
{
  return worker ? worker->stroke_raw_inputs : NULL;
}

/** @brief Read-only access to preserved stroke runtime. */
dt_drawlayer_paint_stroke_t *dt_drawlayer_worker_stroke(dt_drawlayer_worker_t *worker)
{
  return worker ? worker->stroke : NULL;
}

guint dt_drawlayer_worker_pending_dab_count(const dt_drawlayer_worker_t *worker)
{
  dt_drawlayer_worker_t *rt = (dt_drawlayer_worker_t *)worker;
  if(!rt) return 0;

  guint len = 0;
  dt_pthread_mutex_lock(&rt->worker_mutex);
  if(rt->stroke && rt->stroke->pending_dabs) len = rt->stroke->pending_dabs->len;
  dt_pthread_mutex_unlock(&rt->worker_mutex);
  return len;
}

/** @brief Report whether current preserved stroke was already queued for deferred replay. */
gboolean dt_drawlayer_worker_finished_stroke_queued(const dt_drawlayer_worker_t *worker)
{
  return worker ? worker->finished_stroke_queued : FALSE;
}

/** @brief Public FIFO enqueue for one raw input event. */
gboolean dt_drawlayer_worker_enqueue_input(dt_drawlayer_worker_t *worker,
                                           const dt_drawlayer_paint_raw_input_t *input)
{
  return _enqueue_input(worker ? worker->self : NULL, worker, (const dt_drawlayer_paint_raw_input_t *)input);
}

/** @brief Public FIFO enqueue for stroke-end event. */
gboolean dt_drawlayer_worker_enqueue_stroke_end(dt_drawlayer_worker_t *worker,
                                                const dt_drawlayer_paint_raw_input_t *input)
{
  return _enqueue_stroke_end(worker ? worker->self : NULL, worker, input);
}
