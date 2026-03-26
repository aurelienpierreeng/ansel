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
 * Realtime painting rasterizes directly into the authoritative
 * full-resolution `base_patch`. The backend worker consumes GUI raw input,
 * interpolates dabs, updates the full-resolution layer cache, and publishes
 * damage for preview redraw/history invalidation.
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
  gboolean thread_started;                       /**< TRUE once `thread` is owned by this worker. */
  dt_drawlayer_paint_raw_input_t *ring;          /**< FIFO ring storage. */
  guint ring_capacity;                           /**< Max events in ring. */
  guint ring_head;                               /**< Pop index. */
  guint ring_tail;                               /**< Push index. */
  guint ring_count;                              /**< Current queued events. */
  dt_drawlayer_worker_state_t state;             /**< Consolidated worker lifecycle state. */
  gboolean stop;                                 /**< Stop-request flag. */
} drawlayer_rt_worker_t;

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
  guint live_publish_source_id;                 /**< Pending main-thread live publish callback id (0 if none). */
  GArray *backend_history;                      /**< Emitted dab history owned by worker. */
  GArray *stroke_raw_inputs;                    /**< Preserved raw input history for current stroke. */
  dt_drawlayer_paint_stroke_t *stroke;          /**< Worker-owned current stroke runtime. */
  dt_drawlayer_damaged_rect_t *backend_path;    /**< Worker-owned backend damage accumulator. */
  gint64 live_publish_ts;                       /**< Realtime publish pacing timestamp. */
  uint32_t live_publish_serial;                 /**< Monotonic live publish serial. */
  dt_drawlayer_damaged_rect_t live_publish_damage; /**< Worker-owned accumulated publish damage. */
};

typedef struct drawlayer_paint_backend_ctx_t
{
  dt_iop_module_t *self;
  dt_drawlayer_worker_t *worker;
  dt_drawlayer_paint_stroke_t *stroke;
} drawlayer_paint_backend_ctx_t;

static gboolean _paint_build_dab_cb(void *user_data, dt_drawlayer_paint_stroke_t *state,
                                    const dt_drawlayer_paint_raw_input_t *input, dt_drawlayer_brush_dab_t *out_dab);
static gboolean _paint_layer_to_widget_cb(void *user_data, float lx, float ly, float *wx, float *wy);
static void _paint_stroke_seed_cb(void *user_data, uint64_t stroke_seed);
static void _publish_backend_progress(drawlayer_paint_backend_ctx_t *ctx, gboolean flush_pending);
static gboolean _publish_backend_progress_idle(gpointer user_data);
static void _process_backend_input(dt_iop_module_t *self, const dt_drawlayer_paint_raw_input_t *input,
                                   dt_drawlayer_paint_stroke_t *stroke);
static void _process_backend_dab(dt_iop_module_t *self, const dt_drawlayer_brush_dab_t *dab,
                                 drawlayer_paint_backend_ctx_t *ctx);
static inline drawlayer_rt_worker_t *_backend_worker(dt_drawlayer_worker_t *rt);
static inline const drawlayer_rt_worker_t *_backend_worker_const(const dt_drawlayer_worker_t *rt);
static inline gint64 _live_publish_interval_us(void);
static inline gboolean _live_publish_deadline_reached(const dt_drawlayer_worker_t *rt, const gint64 input_ts,
                                                      const gint64 interval_us);
static inline drawlayer_paint_backend_ctx_t _make_backend_ctx(dt_iop_module_t *self, dt_drawlayer_worker_t *worker,
                                                              dt_drawlayer_paint_stroke_t *stroke);
static guint _rasterize_pending_dab_batch(drawlayer_paint_backend_ctx_t *ctx, gint64 budget_us);
static gboolean _rt_queue_pop_locked(dt_drawlayer_worker_t *rt,
                                     dt_drawlayer_paint_raw_input_t *event);
static guint _worker_batch_min_size(void);
static void _log_worker_batch_timing(const char *tag, guint processed_dabs, guint thread_count, double elapsed_ms,
                                     gboolean outer_loop);
#if defined(_OPENMP) && OUTER_LOOP
static guint _rasterize_dab_batch_outer_loop(const GArray *dabs, guint max_dabs, float distance_percent,
                                             dt_drawlayer_cache_patch_t *patch, float scale,
                                             dt_drawlayer_cache_patch_t *stroke_mask,
                                             dt_drawlayer_damaged_rect_t *batch_damage,
                                             const char *tag);
static gboolean _dab_batch_supports_outer_loop(const GArray *dabs, guint count);
#endif

/* GUI worker painting into uint8 buffers is deimplemented. Realtime preview
 * now relies on the regular pipeline/backbuffer path. */

#if defined(_OPENMP) && OUTER_LOOP
#include <omp.h>
#endif

#define DRAWLAYER_BATCH_TILE_SIZE 128
#define DRAWLAYER_OUTER_LIVE_BATCH_MULTIPLIER 2u

gboolean dt_drawlayer_build_worker_input_dab(dt_iop_module_t *self, dt_drawlayer_paint_stroke_t *state,
                                             const dt_drawlayer_paint_raw_input_t *input,
                                             dt_drawlayer_brush_dab_t *dab)
{
  if(!self || !state || !input || !dab) return FALSE;

  float lx = input->lx;
  float ly = input->ly;
  if(!input->have_layer_coords && !dt_drawlayer_widget_to_layer_coords(self, input->wx, input->wy, &lx, &ly))
    return FALSE;

  const float pressure_norm = _clamp01(input->pressure);
  const float tilt_norm = _clamp01(input->tilt);
  const float accel_norm = _clamp01(input->acceleration);
  const gboolean map_pressure_size = (input->map_flags & DRAWLAYER_INPUT_MAP_PRESSURE_SIZE) != 0u;
  const gboolean map_pressure_opacity = (input->map_flags & DRAWLAYER_INPUT_MAP_PRESSURE_OPACITY) != 0u;
  const gboolean map_pressure_flow = (input->map_flags & DRAWLAYER_INPUT_MAP_PRESSURE_FLOW) != 0u;
  const gboolean map_pressure_softness = (input->map_flags & DRAWLAYER_INPUT_MAP_PRESSURE_SOFTNESS) != 0u;
  const gboolean map_tilt_size = (input->map_flags & DRAWLAYER_INPUT_MAP_TILT_SIZE) != 0u;
  const gboolean map_tilt_opacity = (input->map_flags & DRAWLAYER_INPUT_MAP_TILT_OPACITY) != 0u;
  const gboolean map_tilt_flow = (input->map_flags & DRAWLAYER_INPUT_MAP_TILT_FLOW) != 0u;
  const gboolean map_tilt_softness = (input->map_flags & DRAWLAYER_INPUT_MAP_TILT_SOFTNESS) != 0u;
  const gboolean map_accel_size = (input->map_flags & DRAWLAYER_INPUT_MAP_ACCEL_SIZE) != 0u;
  const gboolean map_accel_opacity = (input->map_flags & DRAWLAYER_INPUT_MAP_ACCEL_OPACITY) != 0u;
  const gboolean map_accel_flow = (input->map_flags & DRAWLAYER_INPUT_MAP_ACCEL_FLOW) != 0u;
  const gboolean map_accel_softness = (input->map_flags & DRAWLAYER_INPUT_MAP_ACCEL_SOFTNESS) != 0u;
  const drawlayer_mapping_profile_t pressure_profile = (drawlayer_mapping_profile_t)CLAMP(
      (int)input->pressure_profile, (int)DRAWLAYER_PROFILE_LINEAR, (int)DRAWLAYER_PROFILE_INV_QUADRATIC);
  const drawlayer_mapping_profile_t tilt_profile = (drawlayer_mapping_profile_t)CLAMP(
      (int)input->tilt_profile, (int)DRAWLAYER_PROFILE_LINEAR, (int)DRAWLAYER_PROFILE_INV_QUADRATIC);
  const drawlayer_mapping_profile_t accel_profile = (drawlayer_mapping_profile_t)CLAMP(
      (int)input->accel_profile, (int)DRAWLAYER_PROFILE_LINEAR, (int)DRAWLAYER_PROFILE_INV_QUADRATIC);
  const float pressure_coeff = _mapping_profile_value(pressure_profile, pressure_norm);
  const float tilt_coeff = _mapping_profile_value(tilt_profile, tilt_norm);
  const float accel_coeff = _mapping_profile_value(accel_profile, accel_norm);

  float radius = fmaxf(input->brush_radius, 0.5f);
  float opacity = _clamp01(input->brush_opacity);
  float flow = _clamp01(input->brush_flow);
  const float sprinkles = _clamp01(input->brush_sprinkles);
  float hardness = _clamp01(input->brush_hardness);
  const float base_radius = radius;
  const float base_opacity = opacity;
  const float base_flow = flow;
  const float base_hardness = hardness;

  if(map_pressure_size) radius *= pressure_coeff;
  if(map_pressure_opacity) opacity *= pressure_coeff;
  if(map_pressure_flow) flow *= pressure_coeff;
  if(map_pressure_softness) hardness *= pressure_coeff;

  if(map_tilt_size) radius *= tilt_coeff;
  if(map_tilt_opacity) opacity *= tilt_coeff;
  if(map_tilt_flow) flow *= tilt_coeff;
  if(map_tilt_softness) hardness *= tilt_coeff;

  if(map_accel_size) radius *= accel_coeff;
  if(map_accel_opacity) opacity *= accel_coeff;
  if(map_accel_flow) flow *= accel_coeff;
  if(map_accel_softness) hardness *= accel_coeff;

  radius = fmaxf(radius, 0.5f);
  hardness = _clamp01(hardness);
  float dir_x = 0.0f;
  float dir_y = 0.0f;
  if(state->have_last_input_dab)
  {
    const float dx = lx - state->last_input_dab.x;
    const float dy = ly - state->last_input_dab.y;
    const float dir_len = hypotf(dx, dy);
    if(dir_len > 1e-6f)
    {
      dir_x = dx / dir_len;
      dir_y = dy / dir_len;
    }
  }

  *dab = (dt_drawlayer_brush_dab_t){
    .x = lx,
    .y = ly,
    .wx = input->wx,
    .wy = input->wy,
    .radius = radius,
    .dir_x = dir_x,
    .dir_y = dir_y,
    .opacity = _clamp01(opacity),
    .flow = _clamp01(flow),
    .sprinkles = _clamp01(sprinkles),
    .sprinkle_size = input->brush_sprinkle_size,
    .sprinkle_coarseness = _clamp01(input->brush_sprinkle_coarseness),
    .hardness = hardness,
    .color = { input->color[0], input->color[1], input->color[2], 1.0f },
    .display_color = { input->display_color[0], input->display_color[1], input->display_color[2] },
    .shape = input->brush_shape,
    .mode = input->brush_mode,
    .stroke_batch = input->stroke_batch,
    .stroke_pos = input->stroke_pos,
  };

  if((map_pressure_size || map_pressure_opacity || map_pressure_flow || map_pressure_softness || map_tilt_size
      || map_tilt_opacity || map_tilt_flow || map_tilt_softness || map_accel_size || map_accel_opacity
      || map_accel_flow || map_accel_softness)
     && (input->stroke_pos != DT_DRAWLAYER_PAINT_STROKE_MIDDLE
         || ((state->history && (state->history->len & 15u) == 0u))))
  {
    dt_print(DT_DEBUG_INPUT,
             "[drawlayer] map p=%.4f t=%.4f a=%.4f coeff[p=%.4f t=%.4f a=%.4f] "
             "base[r=%.2f o=%.3f f=%.3f h=%.3f] out[r=%.2f o=%.3f f=%.3f h=%.3f] "
             "flags[p=%d%d%d%d t=%d%d%d%d a=%d%d%d%d]\n",
             pressure_norm, tilt_norm, accel_norm, pressure_coeff, tilt_coeff, accel_coeff, base_radius,
             base_opacity, base_flow, base_hardness, radius, _clamp01(opacity), _clamp01(flow), _clamp01(hardness),
             map_pressure_size ? 1 : 0, map_pressure_opacity ? 1 : 0, map_pressure_flow ? 1 : 0,
             map_pressure_softness ? 1 : 0, map_tilt_size ? 1 : 0, map_tilt_opacity ? 1 : 0, map_tilt_flow ? 1 : 0,
             map_tilt_softness ? 1 : 0, map_accel_size ? 1 : 0, map_accel_opacity ? 1 : 0, map_accel_flow ? 1 : 0,
             map_accel_softness ? 1 : 0);
  }

  return TRUE;
}

static gboolean _paint_build_dab_cb(void *user_data, dt_drawlayer_paint_stroke_t *state,
                                    const dt_drawlayer_paint_raw_input_t *input, dt_drawlayer_brush_dab_t *out_dab)
{
  drawlayer_paint_backend_ctx_t *ctx = (drawlayer_paint_backend_ctx_t *)user_data;
  return (ctx && ctx->self)
             ? dt_drawlayer_build_worker_input_dab(ctx->self, state, (const dt_drawlayer_paint_raw_input_t *)input,
                                                   (dt_drawlayer_brush_dab_t *)out_dab)
             : FALSE;
}

static gboolean _paint_layer_to_widget_cb(void *user_data, float lx, float ly, float *wx, float *wy)
{
  drawlayer_paint_backend_ctx_t *ctx = (drawlayer_paint_backend_ctx_t *)user_data;
  return (ctx && ctx->self) ? dt_drawlayer_layer_to_widget_coords(ctx->self, lx, ly, wx, wy) : FALSE;
}

static void _paint_stroke_seed_cb(void *user_data, uint64_t stroke_seed)
{
  drawlayer_paint_backend_ctx_t *ctx = (drawlayer_paint_backend_ctx_t *)user_data;
  if(!ctx) return;
  if(ctx->stroke) dt_drawlayer_paint_runtime_set_stroke_seed(ctx->stroke, stroke_seed);
}

static void _publish_backend_progress(drawlayer_paint_backend_ctx_t *ctx, gboolean flush_pending)
{
  if(!ctx || !ctx->self || !flush_pending) return;

  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)ctx->self->gui_data;
  if(!ctx->worker || !g || !ctx->worker->live_publish_damage.valid) return;

  if(g->process.base_patch.pixels)
    dt_dev_pixelpipe_cache_flush_host_pinned_image(darktable.pixelpipe_cache, g->process.base_patch.pixels,
                                                   g->process.base_patch.cache_entry, -1);
  ctx->worker->live_publish_serial++;
  dt_drawlayer_paint_runtime_state_reset(&ctx->worker->live_publish_damage);
  ctx->worker->live_publish_ts = g_get_monotonic_time();

  dt_pthread_mutex_lock(&ctx->worker->worker_mutex);
  if(ctx->worker->live_publish_source_id == 0)
    ctx->worker->live_publish_source_id = g_idle_add(_publish_backend_progress_idle, ctx->worker);
  dt_pthread_mutex_unlock(&ctx->worker->worker_mutex);
}

/** @brief Refresh history/display from the latest full-resolution live paint on the GTK thread.
 *
 *  The worker now rasterizes directly into `base_patch`, so the GUI preview
 *  needs a regular pipeline refresh to sample that updated cache line.
 *  Performing history synchronization here keeps GTK/history ownership on the
 *  main thread and avoids mutating the history stack from the realtime worker.
 */
static gboolean _publish_backend_progress_idle(gpointer user_data)
{
  dt_drawlayer_worker_t *rt = (dt_drawlayer_worker_t *)user_data;
  if(!rt || !rt->self || !rt->self->dev) return G_SOURCE_REMOVE;

  dt_iop_module_t *self = rt->self;
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  dt_iop_drawlayer_params_t *params = (dt_iop_drawlayer_params_t *)self->params;
  dt_develop_t *dev = self->dev;
  uint32_t publish_serial = 0u;

  dt_pthread_mutex_lock(&rt->worker_mutex);
  publish_serial = rt->live_publish_serial;
  rt->live_publish_source_id = 0;
  dt_pthread_mutex_unlock(&rt->worker_mutex);

  if(!g || !params || !dev) return G_SOURCE_REMOVE;

  const int sample_count = (int)g->stroke.stroke_sample_count;
  dt_drawlayer_touch_stroke_commit_hash(params, sample_count, g->stroke.last_dab_valid, g->stroke.last_dab_x,
                                        g->stroke.last_dab_y, publish_serial);

  dt_pthread_rwlock_wrlock(&dev->history_mutex);
  dt_dev_add_history_item_ext(dev, self, FALSE, FALSE);
  dt_dev_set_history_hash(dev, dt_dev_history_compute_hash(dev));
  dt_pthread_rwlock_unlock(&dev->history_mutex);

  dt_dev_pixelpipe_update_history_all(dev);
  dt_control_queue_redraw_center();
  return G_SOURCE_REMOVE;
}

static void _process_backend_input(dt_iop_module_t *self, const dt_drawlayer_paint_raw_input_t *input,
                                   dt_drawlayer_paint_stroke_t *stroke)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !input || !stroke) return;

  drawlayer_paint_backend_ctx_t ctx = _make_backend_ctx(self, g->stroke.worker, stroke);
  const dt_drawlayer_paint_callbacks_t callbacks = {
    .build_dab = _paint_build_dab_cb,
    .layer_to_widget = _paint_layer_to_widget_cb,
    .on_stroke_seed = _paint_stroke_seed_cb,
  };
  if(!dt_drawlayer_paint_queue_raw_input(stroke, input)) return;
  dt_drawlayer_paint_interpolate_path(stroke, &callbacks, &ctx);

  if(stroke->pending_dabs && stroke->pending_dabs->len > 0)
  {
    const gint64 live_publish_interval_us = _live_publish_interval_us();
    const gint64 input_ts = input->event_ts ? input->event_ts : g_get_monotonic_time();
    if(ctx.worker && ctx.worker->live_publish_ts == 0)
      ctx.worker->live_publish_ts = input_ts - live_publish_interval_us;

    if(_live_publish_deadline_reached(ctx.worker, input_ts, live_publish_interval_us))
    {
      const guint processed_dabs = _rasterize_pending_dab_batch(&ctx, live_publish_interval_us);
      if(processed_dabs > 0) _publish_backend_progress(&ctx, TRUE);
    }
  }
}

void dt_drawlayer_worker_publish_backend_stroke_damage(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g) return;

  dt_drawlayer_damaged_rect_t backend_damage = { 0 };
  if(g->stroke.worker && dt_drawlayer_paint_merge_runtime_stroke_damage(g->stroke.worker->backend_path, &backend_damage))
    g->process.cache_dirty = TRUE;
}

static void _process_backend_dab(dt_iop_module_t *self, const dt_drawlayer_brush_dab_t *dab,
                                 drawlayer_paint_backend_ctx_t *ctx)
{
  dt_drawlayer_paint_stroke_t *stroke = ctx ? ctx->stroke : NULL;
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  if(!g || !stroke || !stroke->dab_window || !dab) return;

  gboolean have_base_damage = FALSE;
  dt_drawlayer_damaged_rect_t base_step_path = { 0 };
  dt_drawlayer_damaged_rect_t base_step_damage = { 0 };
  dt_drawlayer_paint_runtime_state_reset(&base_step_path);

  if(g->process.base_patch.pixels && g->process.base_patch.width > 0 && g->process.base_patch.height > 0
     && g->process.stroke_mask.pixels)
  {
    dt_drawlayer_paint_rasterize_segment_to_buffer(
        dab, _clamp01(stroke->distance_percent), &g->process.base_patch, 1.0f, &g->process.stroke_mask,
        &base_step_path, stroke);
    have_base_damage = dt_drawlayer_paint_runtime_get_stroke_damage(&base_step_path, &base_step_damage);

  }

  if(have_base_damage)
  {
    g->process.cache_dirty = TRUE;
    if(ctx && ctx->worker)
      dt_drawlayer_paint_runtime_note_dab_damage(&ctx->worker->live_publish_damage, &base_step_damage);
    g->stroke.last_dab_valid = TRUE;
    g->stroke.last_dab_x = dab->x;
    g->stroke.last_dab_y = dab->y;
  }
}

static const drawlayer_rt_callbacks_t _rt_callbacks[DRAWLAYER_RT_WORKER_COUNT];
static void _stop_worker(dt_iop_module_t *self, dt_drawlayer_worker_t *rt);

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
}

static void _reset_backend_path(dt_drawlayer_worker_t *rt)
{
  if(rt && rt->backend_path) dt_drawlayer_paint_runtime_state_reset(rt->backend_path);
}

static void _reset_live_publish(dt_drawlayer_worker_t *rt)
{
  if(!rt) return;
  rt->live_publish_ts = 0;
  rt->live_publish_serial = 0;
  dt_drawlayer_paint_runtime_state_reset(&rt->live_publish_damage);
}

static inline drawlayer_rt_worker_t *_backend_worker(dt_drawlayer_worker_t *rt)
{
  return rt ? &rt->workers[DRAWLAYER_RT_WORKER_BACKEND] : NULL;
}

static inline const drawlayer_rt_worker_t *_backend_worker_const(const dt_drawlayer_worker_t *rt)
{
  return rt ? &rt->workers[DRAWLAYER_RT_WORKER_BACKEND] : NULL;
}

static inline gint64 _live_publish_interval_us(void)
{
  return MAX((gint64)dt_gui_throttle_get_pipe_runtime_us(DT_DEV_PIXELPIPE_FULL), (gint64)20000);
}

static guint _worker_batch_min_size(void)
{
#if defined(_OPENMP) && OUTER_LOOP
  return MAX(1, omp_get_max_threads());
#else
  return 1;
#endif
}

#if defined(_OPENMP) && OUTER_LOOP
static gboolean _dab_batch_supports_outer_loop(const GArray *dabs, const guint count)
{
  if(!dabs || count == 0) return FALSE;
  for(guint i = 0; i < count; i++)
  {
    const dt_drawlayer_brush_dab_t *dab = &g_array_index(dabs, dt_drawlayer_brush_dab_t, i);
    if(dab->mode == DT_DRAWLAYER_BRUSH_MODE_SMUDGE) return FALSE;
  }
  return TRUE;
}
#endif

static void _log_worker_batch_timing(const char *tag, const guint processed_dabs, const guint thread_count,
                                     const double elapsed_ms, const gboolean outer_loop)
{
  if(!(darktable.unmuted & DT_DEBUG_PERF)) return;
  dt_print(DT_DEBUG_PERF, "[drawlayer] batch worker=%s dabs=%u threads=%u outer=%d ms=%.3f\n",
           tag ? tag : "unknown", processed_dabs, thread_count, outer_loop ? 1 : 0, elapsed_ms);
}

static inline gboolean _live_publish_deadline_reached(const dt_drawlayer_worker_t *rt, const gint64 input_ts,
                                                      const gint64 interval_us)
{
  return rt && input_ts - rt->live_publish_ts >= interval_us;
}

static inline drawlayer_paint_backend_ctx_t _make_backend_ctx(dt_iop_module_t *self, dt_drawlayer_worker_t *worker,
                                                              dt_drawlayer_paint_stroke_t *stroke)
{
  return (drawlayer_paint_backend_ctx_t){
    .self = self,
    .worker = worker,
    .stroke = stroke,
  };
}

#if defined(_OPENMP) && OUTER_LOOP
static gboolean _dab_bounds_in_patch(const dt_drawlayer_cache_patch_t *patch, const float scale,
                                     const dt_drawlayer_brush_dab_t *dab, dt_drawlayer_damaged_rect_t *bounds)
{
  if(bounds) *bounds = (dt_drawlayer_damaged_rect_t){ 0 };
  if(!patch || !dab || !bounds || !patch->pixels || patch->width <= 0 || patch->height <= 0
     || dab->radius <= 0.0f || dab->opacity <= 0.0f || scale <= 0.0f)
    return FALSE;

  const float support_radius = dab->radius;
  bounds->valid = TRUE;
  bounds->nw[0] = MAX(0, (int)floorf((dab->x - support_radius) * scale) - patch->x);
  bounds->nw[1] = MAX(0, (int)floorf((dab->y - support_radius) * scale) - patch->y);
  bounds->se[0] = MIN(patch->width, (int)ceilf((dab->x + support_radius) * scale) - patch->x + 1);
  bounds->se[1] = MIN(patch->height, (int)ceilf((dab->y + support_radius) * scale) - patch->y + 1);
  return bounds->se[0] > bounds->nw[0] && bounds->se[1] > bounds->nw[1];
}

static dt_drawlayer_paint_stroke_t *_create_batch_runtime(void)
{
  dt_drawlayer_paint_stroke_t *runtime = dt_drawlayer_paint_runtime_private_create();
  if(!runtime) return NULL;
  runtime->dab_window = g_array_new(FALSE, FALSE, sizeof(dt_drawlayer_brush_dab_t));
  if(!runtime->dab_window)
  {
    dt_drawlayer_paint_runtime_private_destroy(&runtime);
    return NULL;
  }
  return runtime;
}

static void _destroy_batch_runtime(dt_drawlayer_paint_stroke_t **runtime)
{
  if(!runtime || !*runtime) return;
  if((*runtime)->dab_window) g_array_free((*runtime)->dab_window, TRUE);
  (*runtime)->dab_window = NULL;
  dt_drawlayer_paint_runtime_private_destroy(runtime);
}

static void _lock_batch_tiles(omp_lock_t *locks, const int tile_cols, const int tile_origin_x,
                              const int tile_origin_y, const dt_drawlayer_damaged_rect_t *bounds)
{
  if(!locks || tile_cols <= 0 || !bounds || !bounds->valid) return;
  const int tx0 = bounds->nw[0] / DRAWLAYER_BATCH_TILE_SIZE - tile_origin_x;
  const int ty0 = bounds->nw[1] / DRAWLAYER_BATCH_TILE_SIZE - tile_origin_y;
  const int tx1 = MAX(tx0, (bounds->se[0] - 1) / DRAWLAYER_BATCH_TILE_SIZE - tile_origin_x);
  const int ty1 = MAX(ty0, (bounds->se[1] - 1) / DRAWLAYER_BATCH_TILE_SIZE - tile_origin_y);
  for(int ty = ty0; ty <= ty1; ty++)
    for(int tx = tx0; tx <= tx1; tx++)
      omp_set_lock(&locks[ty * tile_cols + tx]);
}

static void _unlock_batch_tiles(omp_lock_t *locks, const int tile_cols, const int tile_origin_x,
                                const int tile_origin_y, const dt_drawlayer_damaged_rect_t *bounds)
{
  if(!locks || tile_cols <= 0 || !bounds || !bounds->valid) return;
  const int tx0 = bounds->nw[0] / DRAWLAYER_BATCH_TILE_SIZE - tile_origin_x;
  const int ty0 = bounds->nw[1] / DRAWLAYER_BATCH_TILE_SIZE - tile_origin_y;
  const int tx1 = MAX(tx0, (bounds->se[0] - 1) / DRAWLAYER_BATCH_TILE_SIZE - tile_origin_x);
  const int ty1 = MAX(ty0, (bounds->se[1] - 1) / DRAWLAYER_BATCH_TILE_SIZE - tile_origin_y);
  for(int ty = ty1; ty >= ty0; ty--)
    for(int tx = tx1; tx >= tx0; tx--)
      omp_unset_lock(&locks[ty * tile_cols + tx]);
}

static guint _rasterize_dab_batch_outer_loop(const GArray *dabs, const guint max_dabs, const float distance_percent,
                                             dt_drawlayer_cache_patch_t *patch, const float scale,
                                             dt_drawlayer_cache_patch_t *stroke_mask,
                                             dt_drawlayer_damaged_rect_t *batch_damage,
                                             const char *tag)
{
  if(!dabs || max_dabs == 0 || !patch || !patch->pixels || patch->width <= 0 || patch->height <= 0) return 0;

  const guint thread_count = _worker_batch_min_size();
  dt_drawlayer_damaged_rect_t batch_bounds = { 0 };
  for(guint i = 0; i < max_dabs; i++)
  {
    dt_drawlayer_damaged_rect_t dab_bounds = { 0 };
    const dt_drawlayer_brush_dab_t *dab = &g_array_index(dabs, dt_drawlayer_brush_dab_t, i);
    if(_dab_bounds_in_patch(patch, scale, dab, &dab_bounds))
      dt_drawlayer_paint_runtime_note_dab_damage(&batch_bounds, &dab_bounds);
  }
  if(!batch_bounds.valid) return 0;

  const int tile_origin_x = batch_bounds.nw[0] / DRAWLAYER_BATCH_TILE_SIZE;
  const int tile_origin_y = batch_bounds.nw[1] / DRAWLAYER_BATCH_TILE_SIZE;
  const int tile_cols = MAX(1, (batch_bounds.se[0] + DRAWLAYER_BATCH_TILE_SIZE - 1) / DRAWLAYER_BATCH_TILE_SIZE - tile_origin_x);
  const int tile_rows = MAX(1, (batch_bounds.se[1] + DRAWLAYER_BATCH_TILE_SIZE - 1) / DRAWLAYER_BATCH_TILE_SIZE - tile_origin_y);
  dt_drawlayer_paint_stroke_t **thread_runtime = g_malloc0((size_t)thread_count * sizeof(*thread_runtime));
  dt_drawlayer_damaged_rect_t *thread_damage = g_malloc0((size_t)thread_count * sizeof(*thread_damage));
  omp_lock_t *tile_locks = g_malloc0((size_t)tile_cols * tile_rows * sizeof(*tile_locks));
  if(!thread_runtime || !thread_damage || !tile_locks)
  {
    g_free(thread_runtime);
    g_free(thread_damage);
    g_free(tile_locks);
    return 0;
  }

  for(guint i = 0; i < thread_count; i++)
  {
    thread_runtime[i] = _create_batch_runtime();
    if(!thread_runtime[i])
    {
      for(guint k = 0; k < i; k++) _destroy_batch_runtime(&thread_runtime[k]);
      g_free(thread_runtime);
      g_free(thread_damage);
      g_free(tile_locks);
      return 0;
    }
  }

  for(int i = 0; i < tile_cols * tile_rows; i++)
    omp_init_lock(&tile_locks[i]);

  const double t0 = dt_get_wtime();
#pragma omp parallel for schedule(static) default(none) \
  shared(dabs, patch, stroke_mask, thread_runtime, thread_damage, tile_locks, tile_cols, tile_origin_x, tile_origin_y, max_dabs, distance_percent, scale)
  for(guint i = 0; i < max_dabs; i++)
  {
    const int tid = omp_get_thread_num();
    dt_drawlayer_paint_stroke_t *runtime = thread_runtime[tid];
    dt_drawlayer_damaged_rect_t runtime_damage = { 0 };
    dt_drawlayer_damaged_rect_t dab_damage = { 0 };
    dt_drawlayer_damaged_rect_t bounds = { 0 };
    const dt_drawlayer_brush_dab_t *dab = &g_array_index(dabs, dt_drawlayer_brush_dab_t, i);

    dt_drawlayer_paint_path_state_reset(runtime);
    dt_drawlayer_paint_runtime_private_reset(runtime);
    if(runtime->dab_window) g_array_set_size(runtime->dab_window, 0);
    if(!_dab_bounds_in_patch(patch, scale, dab, &bounds))
      continue;

    _lock_batch_tiles(tile_locks, tile_cols, tile_origin_x, tile_origin_y, &bounds);
    dt_drawlayer_paint_rasterize_segment_to_buffer(dab, distance_percent, patch, scale, stroke_mask,
                                                   &runtime_damage, runtime);
    _unlock_batch_tiles(tile_locks, tile_cols, tile_origin_x, tile_origin_y, &bounds);

    if(dt_drawlayer_paint_runtime_get_stroke_damage(&runtime_damage, &dab_damage))
      dt_drawlayer_paint_runtime_note_dab_damage(&thread_damage[tid], &dab_damage);
  }
  const double t1 = dt_get_wtime();

  if(batch_damage) dt_drawlayer_paint_runtime_state_reset(batch_damage);
  for(guint i = 0; i < thread_count; i++)
  {
    if(batch_damage) dt_drawlayer_paint_runtime_note_dab_damage(batch_damage, &thread_damage[i]);
    _destroy_batch_runtime(&thread_runtime[i]);
  }
  for(int i = 0; i < tile_cols * tile_rows; i++)
    omp_destroy_lock(&tile_locks[i]);

  g_free(thread_runtime);
  g_free(thread_damage);
  g_free(tile_locks);

  _log_worker_batch_timing(tag, max_dabs, thread_count, 1000.0 * (t1 - t0), TRUE);
  return max_dabs;
}
#endif

static guint _rasterize_pending_dab_batch(drawlayer_paint_backend_ctx_t *ctx, gint64 budget_us)
{
  dt_iop_drawlayer_gui_data_t *g = (ctx && ctx->self) ? (dt_iop_drawlayer_gui_data_t *)ctx->self->gui_data : NULL;
  dt_drawlayer_paint_stroke_t *stroke = ctx ? ctx->stroke : NULL;
  if(!g || !stroke || !stroke->pending_dabs || stroke->pending_dabs->len == 0) return 0;

  const guint min_batch = _worker_batch_min_size();

#if defined(_OPENMP) && OUTER_LOOP
  const guint remaining_dabs = stroke->pending_dabs->len;

  if(remaining_dabs >= min_batch && _dab_batch_supports_outer_loop(stroke->pending_dabs, remaining_dabs))
  {
    const guint batch_dabs = (budget_us > 0)
                                 ? MIN(remaining_dabs, min_batch * DRAWLAYER_OUTER_LIVE_BATCH_MULTIPLIER)
                                 : remaining_dabs;
    dt_drawlayer_damaged_rect_t batch_damage = { 0 };
    dt_drawlayer_cache_patch_wrlock(&g->process.base_patch);
    const guint processed_dabs = _rasterize_dab_batch_outer_loop(
        stroke->pending_dabs, batch_dabs, _clamp01(stroke->distance_percent), &g->process.base_patch, 1.0f,
        &g->process.stroke_mask, &batch_damage, "realtime");
    dt_drawlayer_cache_patch_wrunlock(&g->process.base_patch);

    if(processed_dabs > 0)
    {
      const dt_drawlayer_brush_dab_t *last_dab
          = &g_array_index(stroke->pending_dabs, dt_drawlayer_brush_dab_t, processed_dabs - 1);
      g_array_remove_range(stroke->pending_dabs, 0, processed_dabs);
      if(batch_damage.valid)
      {
        g->process.cache_dirty = TRUE;
        if(ctx->worker)
          dt_drawlayer_paint_runtime_note_dab_damage(&ctx->worker->live_publish_damage, &batch_damage);
        g->stroke.last_dab_valid = TRUE;
        g->stroke.last_dab_x = last_dab->x;
        g->stroke.last_dab_y = last_dab->y;
      }
    }
    return processed_dabs;
  }
#endif

  dt_drawlayer_cache_patch_wrlock(&g->process.base_patch);
  guint processed_dabs = 0;
  const double batch_t0 = dt_get_wtime();
  while(processed_dabs < stroke->pending_dabs->len)
  {
    const dt_drawlayer_brush_dab_t *dab
        = &g_array_index(stroke->pending_dabs, dt_drawlayer_brush_dab_t, processed_dabs);
    _process_backend_dab(ctx->self, dab, ctx);
    processed_dabs++;

    if(budget_us > 0)
    {
      const gint64 elapsed_us = (gint64)(1000000.0 * (dt_get_wtime() - batch_t0));
      if(processed_dabs >= min_batch && elapsed_us >= budget_us) break;
    }
  }
  if(processed_dabs > 0) g_array_remove_range(stroke->pending_dabs, 0, processed_dabs);
  dt_drawlayer_cache_patch_wrunlock(&g->process.base_patch);
  if(processed_dabs > 0)
    _log_worker_batch_timing("realtime", processed_dabs, 1, 1000.0 * (dt_get_wtime() - batch_t0), FALSE);
  return processed_dabs;
}

/** @brief Clear queued events (lock must be held). */
static void _rt_queue_clear_locked(dt_drawlayer_worker_t *rt)
{
  drawlayer_rt_worker_t *worker = _backend_worker(rt);
  if(!worker) return;
  worker->ring_head = 0;
  worker->ring_tail = 0;
  worker->ring_count = 0;
}

/** @brief Test whether event queue is empty. */
static gboolean _rt_queue_empty(const dt_drawlayer_worker_t *rt)
{
  const drawlayer_rt_worker_t *worker = _backend_worker_const(rt);
  return !worker || worker->ring_count == 0;
}

/** @brief Test whether event queue is full. */
static gboolean _rt_queue_full(const dt_drawlayer_worker_t *rt)
{
  const drawlayer_rt_worker_t *worker = _backend_worker_const(rt);
  return !worker || (worker->ring_capacity > 0 && worker->ring_count >= worker->ring_capacity);
}

/** @brief Push one event in ring queue (lock must be held). */
static gboolean _rt_queue_push_locked(dt_drawlayer_worker_t *rt,
                                      const dt_drawlayer_paint_raw_input_t *event)
{
  drawlayer_rt_worker_t *worker = _backend_worker(rt);
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
  drawlayer_rt_worker_t *worker = _backend_worker(rt);
  if(!worker || !event || !worker->ring || worker->ring_capacity == 0 || _rt_queue_empty(rt)) return FALSE;
  *event = worker->ring[worker->ring_head];
  worker->ring_head = (worker->ring_head + 1) % worker->ring_capacity;
  worker->ring_count--;
  return TRUE;
}

static inline gboolean _worker_is_started(const drawlayer_rt_worker_t *worker)
{
  return worker && worker->thread_started;
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

/** @brief Set worker state atomically under caller synchronization. */
static void _rt_set_worker_state(dt_drawlayer_worker_t *rt, const dt_drawlayer_worker_state_t state)
{
  drawlayer_rt_worker_t *worker = _backend_worker(rt);
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
  const drawlayer_rt_worker_t *worker = _backend_worker_const(rt);
  return worker && (_worker_is_busy(worker) || worker->ring_count > 0
                    || (rt->finish_commit_pending && *rt->finish_commit_pending));
}

/** @brief Check whether any worker activity remains. */
static gboolean _workers_any_active_locked(const dt_drawlayer_worker_t *rt)
{
  return _workers_active_locked(rt);
}

/** @brief Check if workers are idle and commit can be safely scheduled. */
static gboolean _workers_ready_for_commit_locked(const dt_drawlayer_worker_t *rt)
{
  const drawlayer_rt_worker_t *worker = _backend_worker_const(rt);
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

/** @brief Thread-safe wrapper for any worker activity. */
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
  drawlayer_paint_backend_ctx_t ctx = _make_backend_ctx(self, rt, rt ? rt->stroke : NULL);
  if(self && g && rt && rt->stroke && rt->stroke->pending_dabs && rt->stroke->pending_dabs->len > 0)
  {
    const guint processed_dabs = _rasterize_pending_dab_batch(&ctx, _live_publish_interval_us());
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
    drawlayer_paint_backend_ctx_t ctx = _make_backend_ctx(self, rt, rt->stroke);
    if(g)
    {
      dt_drawlayer_paint_finalize_path(rt->stroke);
      _rasterize_pending_dab_batch(&ctx, 0);
      _publish_backend_progress(&ctx, TRUE);
    }
  }
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
  drawlayer_rt_worker_t *worker = _backend_worker(rt);
  guint live_publish_source_id = 0;
  dt_pthread_mutex_lock(&rt->worker_mutex);
  live_publish_source_id = rt->live_publish_source_id;
  rt->live_publish_source_id = 0;
  dt_pthread_mutex_unlock(&rt->worker_mutex);
  if(live_publish_source_id != 0) g_source_remove(live_publish_source_id);
  _stop_worker(self ? self : rt->self, rt);
  _stroke_destroy(rt);
  if(rt->backend_history) g_array_free(rt->backend_history, TRUE);
  if(rt->stroke_raw_inputs) g_array_free(rt->stroke_raw_inputs, TRUE);
  dt_drawlayer_paint_runtime_state_destroy(&rt->backend_path);
  dt_free(worker->ring);
  pthread_cond_destroy(&rt->worker_cond);
  dt_pthread_mutex_destroy(&rt->worker_mutex);
  dt_free(rt);
  *rt_out = NULL;
}

/** @brief Allocate and initialize worker state object and buffers. */
static void _rt_init_state(dt_iop_module_t *self, dt_drawlayer_worker_t **rt_out,
                           gboolean *painting, gboolean *finish_commit_pending,
                           guint *stroke_sample_count, uint32_t *current_stroke_batch)
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
  rt->backend_path = dt_drawlayer_paint_runtime_state_create();
  dt_pthread_mutex_init(&rt->worker_mutex, NULL);
  pthread_cond_init(&rt->worker_cond, NULL);

  drawlayer_rt_worker_t *worker = _backend_worker(rt);
  worker->ring_capacity = DRAWLAYER_WORKER_RING_CAPACITY;
  worker->ring = g_malloc_n(worker->ring_capacity, sizeof(dt_drawlayer_paint_raw_input_t));
  rt->backend_history = g_array_new(FALSE, FALSE, sizeof(dt_drawlayer_brush_dab_t));
  rt->stroke_raw_inputs = g_array_new(FALSE, FALSE, sizeof(dt_drawlayer_paint_raw_input_t));
  _reset_live_publish(rt);
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
  const drawlayer_rt_worker_t *worker = _backend_worker_const(rt);
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

/** @brief Worker main loop: FIFO dequeue, process, and idle scheduling. */
static void *_drawlayer_worker_main(void *user_data)
{
  drawlayer_rt_thread_ctx_t *ctx = (drawlayer_rt_thread_ctx_t *)user_data;
  dt_drawlayer_worker_t *rt = ctx ? ctx->rt : NULL;
  dt_free(ctx);

  dt_iop_module_t *self = rt ? rt->self : NULL;
  drawlayer_rt_worker_t *worker = _backend_worker(rt);
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
  drawlayer_rt_worker_t *worker = _backend_worker(rt);
  if(!self || !rt || !worker) return FALSE;
  if(_worker_is_started(worker))
    return TRUE;

  worker->stop = FALSE;
  worker->state = DT_DRAWLAYER_WORKER_STATE_STOPPED;

  drawlayer_rt_thread_ctx_t *ctx = g_malloc(sizeof(*ctx));
  if(!ctx) return FALSE;
  ctx->rt = rt;

  const int err = dt_pthread_create(&worker->thread, _drawlayer_worker_main, ctx, TRUE);
  if(err != 0)
  {
    dt_free(ctx);
    worker->thread_started = FALSE;
    worker->state = DT_DRAWLAYER_WORKER_STATE_STOPPED;
    return FALSE;
  }

  worker->thread_started = TRUE;
  worker->state = DT_DRAWLAYER_WORKER_STATE_IDLE;
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
  if(!rt) return;
  drawlayer_rt_worker_t *worker = _backend_worker(rt);
  _cancel_async_commit(rt);

  if(worker && _worker_is_started(worker))
  {
    _wait_worker_idle(self, rt);

    dt_pthread_mutex_lock(&rt->worker_mutex);
    worker->stop = TRUE;
    pthread_cond_broadcast(&rt->worker_cond);
    dt_pthread_mutex_unlock(&rt->worker_mutex);

    pthread_join(worker->thread, NULL);
    memset(&worker->thread, 0, sizeof(worker->thread));
    worker->thread_started = FALSE;
    worker->state = DT_DRAWLAYER_WORKER_STATE_STOPPED;
    worker->stop = FALSE;
    _rt_queue_clear_locked(rt);
    _stroke_clear(rt);
  }
}

/** @brief Pause worker processing after current callback returns. */
static void _pause_worker(dt_iop_module_t *self, dt_drawlayer_worker_t *rt)
{
  (void)self;
  drawlayer_rt_worker_t *worker = _backend_worker(rt);
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
  drawlayer_rt_worker_t *worker = _backend_worker(rt);
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
    drawlayer_rt_worker_t *worker = _backend_worker(rt);
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
                              guint *stroke_sample_count, uint32_t *current_stroke_batch)
{
  _rt_init_state(self, worker, painting, finish_commit_pending, stroke_sample_count, current_stroke_batch);
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

gboolean dt_drawlayer_worker_ensure_running(dt_iop_module_t *self, dt_drawlayer_worker_t *rt)
{
  return _start_worker(self, rt);
}

void dt_drawlayer_worker_stop(dt_iop_module_t *self, dt_drawlayer_worker_t *rt)
{
  _stop_worker(self, rt);
}

void dt_drawlayer_worker_get_snapshot(const dt_drawlayer_worker_t *worker,
                                      dt_drawlayer_worker_snapshot_t *snapshot)
{
  if(snapshot) *snapshot = (dt_drawlayer_worker_snapshot_t){ 0 };
  dt_drawlayer_worker_t *rt = (dt_drawlayer_worker_t *)worker;
  if(!rt || !snapshot) return;

  dt_pthread_mutex_lock(&rt->worker_mutex);
  const drawlayer_rt_worker_t *backend = _backend_worker_const(rt);
  snapshot->backend_state = backend->state;
  snapshot->backend_queue_count = backend->ring_count;
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
  (void)worker;
}

void dt_drawlayer_worker_reset_backend_path(dt_drawlayer_worker_t *worker)
{
  if(!worker) return;
  dt_pthread_mutex_lock(&worker->worker_mutex);
  _reset_backend_path(worker);
  dt_pthread_mutex_unlock(&worker->worker_mutex);
}

void dt_drawlayer_worker_reset_live_publish(dt_drawlayer_worker_t *worker)
{
  if(!worker) return;
  dt_pthread_mutex_lock(&worker->worker_mutex);
  _reset_live_publish(worker);
  dt_pthread_mutex_unlock(&worker->worker_mutex);
}

/** @brief Clear preserved stroke runtime/history after commit completed. */
void dt_drawlayer_worker_reset_stroke(dt_drawlayer_worker_t *worker)
{
  if(!worker) return;
  dt_pthread_mutex_lock(&worker->worker_mutex);
  _stroke_clear(worker);
  _reset_live_publish(worker);
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
