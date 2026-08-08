/*
    This file is part of ansel,
    Copyright (C) 2025-2026 Guillaume STUTIN.

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

#include "develop/dev_snapshot.h"

#include "common/iop_order.h"
#include "common/mipmap_cache.h"
#include "control/control.h"
#include "control/jobs.h"
#include "develop/dev_history.h"
#include "develop/develop.h"
#include "develop/pixelpipe_hb.h"
#include "views/dev_backbuf.h"

#include <math.h>

// Real state behind a dt_dev_snapshot_t handle. Heap-allocated and refcounted so that copying a
// dt_dev_snapshot_t (e.g. libs/snapshots.c shuffling its fixed-size slot array) only ever copies a
// stable pointer -- never this struct itself, which a background recompute job can still be
// touching. `pipe`'s accurate ("main") reprocess runs on that job (control/jobs.h, mirroring
// dtgtk/thumbnail.c's own refcounted-job pattern for the exact same problem); `preview_pipe` is
// cheap enough to always run inline on the GUI thread.
//
// A new job for `pipe` is requested every time dt_dev_snapshot_draw() sees dev's
// viewport has moved, exactly like dev->pipe's own darkroom worker loop, which replans and
// reprocesses on every zoom/pan tick with no artificial delay either. Responsiveness is instead
// naturally paced by "at most one job in flight at a time" (see _schedule_main_recompute()): while
// one is running, newer requests are simply dropped and re-derived from dev's then-current
// viewport once that job's completion triggers the next redraw -- the same effect a delay would
// have bought, without adding latency once a job slot is actually free.
//
// `job`, `pending_roi`, `last_roi`, `roi_valid` are shared between the GUI thread and the job's
// worker thread and guarded by `lock`. Everything else here is only ever touched from the GUI
// thread. `pipe`/`preview_pipe` themselves need no extra locking beyond that: dt_dev_lock_pipe_surface()
// (views/dev_backbuf.c) and the pixelpipe cache are already built for exactly one writer
// (whichever thread calls dt_dev_pixelpipe_process()) concurrent with the GUI thread reading the
// published backbuf -- the same guarantee that already makes dev->pipe safe between the darkroom
// worker thread and the GUI thread.
typedef struct dt_dev_snapshot_engine_t
{
  dt_atomic_int ref_count;
  dt_atomic_int destroying;

  dt_develop_t *frozen;
  dt_dev_pixelpipe_t *pipe;
  dt_dev_pixelpipe_t *preview_pipe;
  int32_t raw_width, raw_height;
  float raw_iscale;

  dt_dev_locked_surface_t locked;
  dt_dev_pixelpipe_cache_wait_t wait;

  dt_dev_locked_surface_t preview_locked;
  dt_dev_pixelpipe_cache_wait_t preview_wait;
  dt_iop_roi_t preview_last_roi;
  gboolean preview_roi_valid;

  dt_pthread_mutex_t lock;    // guards the four fields below only.
  dt_job_t *job;
  dt_iop_roi_t pending_roi;
  dt_iop_roi_t last_roi;
  gboolean roi_valid;

  gboolean captured; // GUI-thread only, set once at the end of dt_dev_snapshot_capture().
} dt_dev_snapshot_engine_t;

static void _engine_free(dt_dev_snapshot_engine_t *engine)
{
  dt_dev_release_locked_surface(&engine->locked);
  dt_dev_release_locked_surface(&engine->preview_locked);

  if(!IS_NULL_PTR(engine->pipe))
  {
    dt_dev_pixelpipe_cleanup(engine->pipe);
    dt_free(engine->pipe);
  }
  if(!IS_NULL_PTR(engine->preview_pipe))
  {
    dt_dev_pixelpipe_cleanup(engine->preview_pipe);
    dt_free(engine->preview_pipe);
  }
  if(!IS_NULL_PTR(engine->frozen))
  {
    dt_dev_cleanup(engine->frozen);
    dt_free(engine->frozen);
  }

  dt_pthread_mutex_destroy(&engine->lock);
  dt_free(engine);
}

// Drops one reference; the last one frees the engine. May run on the GUI thread (dt_dev_snapshot_clear())
// or on the recompute job's own worker thread (its params-destroy callback) -- whichever happens
// last, exactly like dtgtk/thumbnail.c's _thumbnail_release()/_thumbnail_free().
static void _engine_unref(dt_dev_snapshot_engine_t *engine)
{
  if(IS_NULL_PTR(engine)) return;
  if(dt_atomic_sub_int(&engine->ref_count, 1) == 1) _engine_free(engine);
}

static void _recompute_job_cleanup(void *params)
{
  _engine_unref((dt_dev_snapshot_engine_t *)params);
}

void dt_dev_snapshot_clear(dt_dev_snapshot_t *snap)
{
  if(IS_NULL_PTR(snap) || IS_NULL_PTR(snap->engine)) return;

  dt_dev_snapshot_engine_t *engine = snap->engine;
  snap->engine = NULL;

  dt_atomic_set_int(&engine->destroying, TRUE);

  dt_pthread_mutex_lock(&engine->lock);
  dt_job_t *job = engine->job;
  dt_pthread_mutex_unlock(&engine->lock);
  // Best-effort: a queued-but-not-yet-started job is skipped outright; one already running is
  // never preempted mid-process() (exactly like dev->pipe is never preempted mid-process()), it
  // just finds `destroying` set and skips publishing/redrawing once it does finish.
  if(!IS_NULL_PTR(job)) dt_control_job_cancel(job);

  _engine_unref(engine); // drop our own reference; the job (if any) still holds its own.
}

gboolean dt_dev_snapshot_is_valid(const dt_dev_snapshot_t *snap)
{
  return !IS_NULL_PTR(snap) && !IS_NULL_PTR(snap->engine) && snap->engine->captured;
}

// Mirrors _update_darkroom_roi()'s main-pipe branch (develop/develop.c), substituting `pipe`'s
// own processed size for dev->roi.processed_width/height -- the snapshot's own image may have
// different dimensions than the one currently open in darkroom. Deliberately ignores the caller's
// clip rect: only dev's pan/zoom (dev->roi) drives what gets processed, so resizing/dragging a
// compare split line never triggers a reprocess.
static gboolean _compute_main_roi(const dt_develop_t *dev, const dt_dev_pixelpipe_t *pipe, dt_iop_roi_t *roi)
{
  if(IS_NULL_PTR(dev) || IS_NULL_PTR(pipe) || IS_NULL_PTR(roi)) return FALSE;
  if(!dev->roi.output_inited || dev->roi.width <= 0 || dev->roi.height <= 0) return FALSE;
  if(pipe->processed_width <= 0 || pipe->processed_height <= 0) return FALSE;

  const float scale = dev->roi.natural_scale * dev->roi.scaling;
  const int roi_width = (int)roundf(scale * pipe->processed_width);
  const int roi_height = (int)roundf(scale * pipe->processed_height);

  roi->width = MAX(1, MIN(roi_width, dev->roi.width));
  roi->height = MAX(1, MIN(roi_height, dev->roi.height));
  roi->x = (int)roundf(dev->roi.x * roi_width - roi->width * .5f);
  roi->y = (int)roundf(dev->roi.y * roi_height - roi->height * .5f);
  roi->scale = scale;
  return TRUE;
}

// Mirrors _update_darkroom_roi()'s preview-pipe branch: the whole image, fit to the widget, no
// user zoom factor and no pan offset.
static gboolean _compute_preview_roi(const dt_develop_t *dev, const dt_dev_pixelpipe_t *pipe, dt_iop_roi_t *roi)
{
  if(IS_NULL_PTR(dev) || IS_NULL_PTR(pipe) || IS_NULL_PTR(roi)) return FALSE;
  if(!dev->roi.output_inited) return FALSE;
  if(pipe->processed_width <= 0 || pipe->processed_height <= 0) return FALSE;

  const float scale = dev->roi.natural_scale;
  roi->width = MAX(1, (int)roundf(scale * pipe->processed_width));
  roi->height = MAX(1, (int)roundf(scale * pipe->processed_height));
  roi->x = 0;
  roi->y = 0;
  roi->scale = scale;
  return TRUE;
}

static gboolean _roi_equal(const dt_iop_roi_t *a, const dt_iop_roi_t *b)
{
  return a->x == b->x 
      && a->y == b->y
      && a->width == b->width
      && a->height == b->height
      && fabs(a->scale - b->scale) < 1e-6;
}

// Runs `pipe` at `roi` and publishes a new backbuf. Mirrors what the darkroom worker loop does
// for dev->pipe on every zoom/pan tick (_resync_pipe_with_history() -> dt_dev_pixelpipe_change()
// with DT_DEV_PIPE_ZOOMED): re-committing no-history-stack modules (finalscale self-enables/
// disables depending on scale) and re-settling piece->buf_in/out and processed_width/height
// through dt_dev_pixelpipe_change() before every process() call at a *different* roi/scale than
// the previous one. Skipping this leaves finalscale's state stale from whichever roi was last
// committed, and the next process() at a different size reads/writes with the wrong geometry --
// each row starting from the wrong offset (a diagonal shear), because the piece never learned its
// target size actually changed.
static gboolean _process_at_roi(dt_dev_snapshot_engine_t *engine, dt_dev_pixelpipe_t *pipe, const dt_iop_roi_t *roi)
{
  dt_dev_pixelpipe_set_input(pipe, engine->frozen->image_storage.id, engine->raw_width, engine->raw_height,
                             engine->raw_iscale, DT_MIPMAP_FULL);
  dt_dev_pixelpipe_or_changed(pipe, DT_DEV_PIPE_ZOOMED);
  dt_dev_pixelpipe_change(pipe);
  return dt_dev_pixelpipe_process(pipe, *roi) == 0;
}

// Immediate GUI-thread only: (re)runs the cheap preview tier if its target roi
// changed. Called on every draw -- cheap because it only changes when the widget is resized.
static void _sync_preview_now(dt_dev_snapshot_engine_t *engine, dt_develop_t *dev)
{
  dt_iop_roi_t roi = { 0 };
  if(!_compute_preview_roi(dev, engine->preview_pipe, &roi)) return;
  if(engine->preview_roi_valid && _roi_equal(&engine->preview_last_roi, &roi)) return;

  engine->preview_roi_valid = _process_at_roi(engine, engine->preview_pipe, &roi);
  if(engine->preview_roi_valid) engine->preview_last_roi = roi;
}

// Runs the accurate main tier at `roi` right now and publishes the result under `lock`. Used
// directly (no job) for the capture-time smoke test, and by the recompute job otherwise.
static gboolean _sync_main_now(dt_dev_snapshot_engine_t *engine, const dt_iop_roi_t *roi)
{
  const gboolean ok = _process_at_roi(engine, engine->pipe, roi);
  dt_pthread_mutex_lock(&engine->lock);
  engine->roi_valid = ok;
  if(ok) engine->last_roi = *roi;
  dt_pthread_mutex_unlock(&engine->lock);
  return ok;
}

// Runs on a control/jobs.h worker thread (DT_JOB_QUEUE_USER_FG), never on the GUI thread.
static int32_t _recompute_job_run(dt_job_t *job)
{
  dt_dev_snapshot_engine_t *engine = (dt_dev_snapshot_engine_t *)dt_control_job_get_params(job);
  if(IS_NULL_PTR(engine)) return 1;
  if(dt_atomic_get_int(&engine->destroying)) return 1;

  dt_pthread_mutex_lock(&engine->lock);
  // A cancelled or superseded job (dt_dev_snapshot_clear() ran, or a newer job replaced this one
  // in the narrow window between scheduling and running) must not touch the pipe or publish a
  // result -- same guard as dtgtk/thumbnail.c's _get_image_buffer().
  const gboolean stale = engine->job != job || dt_control_job_get_state(job) == DT_JOB_STATE_CANCELLED;
  const dt_iop_roi_t roi = engine->pending_roi;
  dt_pthread_mutex_unlock(&engine->lock);
  if(stale) return 1;

  const gboolean ok = _process_at_roi(engine, engine->pipe, &roi);

  dt_pthread_mutex_lock(&engine->lock);
  if(engine->job == job)
  {
    engine->roi_valid = ok;
    if(ok) engine->last_roi = roi;
    engine->job = NULL;
  }
  dt_pthread_mutex_unlock(&engine->lock);

  // dt_control_queue_redraw_center() is already called from worker threads elsewhere in the
  // pixelpipe itself (develop/pixelpipe_hb.c's tiling progress messages, run from the darkroom
  // worker thread), so this is a proven-safe cross-thread call, no marshaling needed.
  if(!dt_atomic_get_int(&engine->destroying)) dt_control_queue_redraw_center();

  return 0;
}

// Requests a main-tier reprocess at `roi` right now, matching dev->pipe's own
// darkroom worker loop, which replans and reprocesses on every zoom/pan tick with no artificial
// delay either. The only throttle is "never run two jobs concurrently on the same `pipe`" (unlike
// the GUI-reads/job-writes pair, which is safe by construction -- dt_dev_lock_pipe_surface()'s own
// contract -- two writers are not): if one is already in flight, this just records the latest
// target and returns; the running job's completion triggers a redraw, dt_dev_snapshot_draw() sees
// the still-stale roi on that next call, and calls back in here to start a fresh job for whatever
// dev's viewport has become by then -- so responsiveness is bounded by "how fast one job finishes",
// not by a fixed delay.
static void _schedule_main_recompute(dt_dev_snapshot_engine_t *engine, const dt_iop_roi_t *roi)
{
  dt_pthread_mutex_lock(&engine->lock);
  const gboolean job_active = !IS_NULL_PTR(engine->job);
  if(!job_active) engine->pending_roi = *roi;
  dt_pthread_mutex_unlock(&engine->lock);
  if(job_active) return;

  dt_job_t *job = dt_control_job_create(&_recompute_job_run, "snapshot recompute");
  if(IS_NULL_PTR(job)) return;

  dt_atomic_add_int(&engine->ref_count, 1); // the job's own reference
  dt_control_job_set_params(job, engine, _recompute_job_cleanup);

  dt_pthread_mutex_lock(&engine->lock);
  engine->job = job;
  dt_pthread_mutex_unlock(&engine->lock);

  if(dt_control_add_job(dt_control_get_global(), DT_JOB_QUEUE_USER_FG, job) != 0)
  {
    dt_pthread_mutex_lock(&engine->lock);
    if(engine->job == job) engine->job = NULL;
    dt_pthread_mutex_unlock(&engine->lock);
    dt_control_job_dispose(job); // triggers _recompute_job_cleanup(), dropping the ref taken above
  }
}

// Approximates the current viewport from the cheap, fit-scale preview tier: cairo-translate/scale
// the already-rendered fit image to roughly match dev's current pan/zoom, same technique as
// darkroom.c's own _build_preview_fallback_surface() for dev->preview_pipe.
static void _draw_preview_fallback(dt_dev_snapshot_engine_t *engine, dt_develop_t *dev, cairo_t *cr, int width,
                                   int height)
{
  if(!engine->preview_roi_valid) return;
  if(!dt_dev_lock_pipe_surface(dev, engine->preview_pipe, &engine->preview_locked, &engine->preview_wait,
                               "snapshot-preview", TRUE))
    return;
  if(IS_NULL_PTR(engine->preview_locked.surface) || IS_NULL_PTR(engine->preview_locked.entry)) return;

  const float ppd = dt_gui_get_global()->ppd;
  const float preview_wd = engine->preview_locked.width / ppd;
  const float preview_ht = engine->preview_locked.height / ppd;
  const float preview_scale = dev->roi.scaling;
  const float tx = 0.5f * width - dev->roi.x * preview_wd * preview_scale;
  const float ty = 0.5f * height - dev->roi.y * preview_ht * preview_scale;

  dt_dev_pixelpipe_cache_rdlock_entry(dt_pixelpipe_cache_get_global(), TRUE, engine->preview_locked.entry);
  cairo_surface_set_device_scale(engine->preview_locked.surface, ppd, ppd);
  cairo_save(cr);
  cairo_translate(cr, tx, ty);
  cairo_scale(cr, preview_scale, preview_scale);
  cairo_rectangle(cr, 0, 0, preview_wd, preview_ht);
  cairo_set_source_surface(cr, engine->preview_locked.surface, 0, 0);
  cairo_fill(cr);
  cairo_restore(cr);
  dt_dev_pixelpipe_cache_rdlock_entry(dt_pixelpipe_cache_get_global(), FALSE, engine->preview_locked.entry);
}

gboolean dt_dev_snapshot_capture(dt_dev_snapshot_t *snap, dt_develop_t *dev, int32_t imgid,
                                  GList *history_override, GList *iop_order_override,
                                  int32_t history_end_override)
{
  dt_develop_t *frozen = NULL;
  dt_dev_snapshot_engine_t *engine = NULL;
  dt_mipmap_buffer_t buf = { 0 };
  const dt_dev_pixelpipe_t *live_preview = NULL;

  dt_dev_snapshot_clear(snap);
  if(IS_NULL_PTR(snap) || IS_NULL_PTR(dev) || imgid <= 0) goto fail;

  frozen = (dt_develop_t *)calloc(1, sizeof(dt_develop_t));
  if(IS_NULL_PTR(frozen)) goto fail;
  dt_dev_init(frozen, 0);

  if(dt_dev_load_image(frozen, imgid))
  {
    dt_print(DT_DEBUG_DEV, "[dev_snapshot] capture failed: dt_dev_load_image failed for imgid=%d\n", imgid);
    dt_dev_cleanup(frozen);
    dt_free(frozen);
    goto fail;
  }

  if(history_override)
  {
    dt_dev_history_free_history(frozen);
    frozen->history = history_override;
    history_override = NULL; // ownership transferred to frozen; do not free again below
    g_list_free_full(frozen->iop_order_list, dt_free_gpointer);
    frozen->iop_order_list = iop_order_override;
    iop_order_override = NULL;

    for(GList *history = g_list_first(frozen->history); history; history = g_list_next(history))
    {
      dt_dev_history_item_t *hist = (dt_dev_history_item_t *)history->data;
      if(IS_NULL_PTR(hist)) continue;
      hist->module = dt_dev_get_module_instance(frozen, hist->op_name, hist->multi_name, hist->multi_priority);
      if(IS_NULL_PTR(hist->module))
        hist->module = dt_dev_create_module_instance(frozen, hist->op_name, hist->multi_name, hist->multi_priority, FALSE);
      if(IS_NULL_PTR(hist->module))
        hist->module = dt_iop_get_module_by_op_priority(frozen->iop, hist->op_name, -1);
      if(IS_NULL_PTR(hist->module))
      {
        dt_print(DT_DEBUG_DEV,
                 "[dev_snapshot] capture failed: unresolved module op=%s multi=%s priority=%d for imgid=%d\n",
                 hist->op_name, hist->multi_name, hist->multi_priority, imgid);
        dt_dev_cleanup(frozen);
        dt_free(frozen);
        goto fail;
      }
    }

    dt_dev_set_history_end_ext(frozen, history_end_override);
    dt_dev_set_history_hash(frozen, dt_dev_history_compute_hash(frozen));
  }

  dt_mipmap_cache_get(dt_mipmap_cache_get_global(), &buf, frozen->image_storage.id, DT_MIPMAP_FULL,
                      DT_MIPMAP_BLOCKING, 'r');
  if(IS_NULL_PTR(buf.buf) || buf.width <= 0 || buf.height <= 0)
  {
    dt_print(DT_DEBUG_DEV, "[dev_snapshot] capture failed: mipmap full unavailable for imgid=%d\n", imgid);
    dt_mipmap_cache_release(dt_mipmap_cache_get_global(), &buf);
    dt_dev_cleanup(frozen);
    dt_free(frozen);
    goto fail;
  }

  engine = (dt_dev_snapshot_engine_t *)calloc(1, sizeof(dt_dev_snapshot_engine_t));
  if(IS_NULL_PTR(engine))
  {
    dt_mipmap_cache_release(dt_mipmap_cache_get_global(), &buf);
    dt_dev_cleanup(frozen);
    dt_free(frozen);
    goto fail;
  }
  dt_pthread_mutex_init(&engine->lock, NULL);
  dt_atomic_set_int(&engine->ref_count, 1);
  dt_atomic_set_int(&engine->destroying, FALSE);

  engine->pipe = (dt_dev_pixelpipe_t *)calloc(1, sizeof(dt_dev_pixelpipe_t));
  engine->preview_pipe = (dt_dev_pixelpipe_t *)calloc(1, sizeof(dt_dev_pixelpipe_t));
  if(IS_NULL_PTR(engine->pipe) || IS_NULL_PTR(engine->preview_pipe))
  {
    dt_mipmap_cache_release(dt_mipmap_cache_get_global(), &buf);
    if(engine->pipe) dt_free(engine->pipe);
    if(engine->preview_pipe) dt_free(engine->preview_pipe);
    dt_pthread_mutex_destroy(&engine->lock);
    dt_free(engine);
    dt_dev_cleanup(frozen);
    dt_free(frozen);
    goto fail;
  }

  // Not combined with the calloc check above via `||`: both init calls must run unconditionally
  // (short-circuiting would leave the second pipe raw calloc'd memory that never went through
  // dt_dev_pixelpipe_init_cached(), which dt_dev_pixelpipe_cleanup() cannot safely be called on
  // below -- its mutex would never have been initialized).
  const gboolean pipe_inited = dt_dev_pixelpipe_init(engine->pipe, frozen);
  const gboolean preview_inited = dt_dev_pixelpipe_init(engine->preview_pipe, frozen);
  if(!pipe_inited || !preview_inited)
  {
    dt_print(DT_DEBUG_DEV, "[dev_snapshot] capture failed: pixelpipe init failed for imgid=%d\n", imgid);
    dt_mipmap_cache_release(dt_mipmap_cache_get_global(), &buf);
    if(pipe_inited) dt_dev_pixelpipe_cleanup(engine->pipe);
    dt_free(engine->pipe);
    if(preview_inited) dt_dev_pixelpipe_cleanup(engine->preview_pipe);
    dt_free(engine->preview_pipe);
    dt_pthread_mutex_destroy(&engine->lock);
    dt_free(engine);
    dt_dev_cleanup(frozen);
    dt_free(frozen);
    goto fail;
  }

  engine->raw_width = buf.width;
  engine->raw_height = buf.height;
  engine->raw_iscale = buf.iscale;

  // Reuse the live darkroom's ICC settings if any image is currently open in darkroom, so a
  // captured snapshot/preview soft-proofs the same way the live pipe does. Harmless when frozen
  // is the same image dev->preview_pipe already reflects; still correct when it's a different
  // one, since ICC intent/profile are a display-wide GUI setting, not per-image state.
  dt_develop_t *const live_dev = dt_dev_get_global();
  live_preview = live_dev ? live_dev->preview_pipe : NULL;

  for(int i = 0; i < 2; i++)
  {
    dt_dev_pixelpipe_t *p = i == 0 ? engine->pipe : engine->preview_pipe;
    dt_dev_pixelpipe_set_input(p, frozen->image_storage.id, buf.width, buf.height, buf.iscale, DT_MIPMAP_FULL);
    dt_dev_pixelpipe_create_nodes(p);
    if(!IS_NULL_PTR(live_preview))
      dt_dev_pixelpipe_set_icc(p, live_preview->icc_type, live_preview->icc_filename, live_preview->icc_intent);
    dt_dev_pixelpipe_synch_all(p);
    dt_dev_pixelpipe_propagate_formats(p);
    dt_dev_pixelpipe_get_roi_out(p, p->iwidth, p->iheight, &p->processed_width, &p->processed_height);
  }

  dt_mipmap_cache_release(dt_mipmap_cache_get_global(), &buf);

  engine->frozen = frozen; // ownership transferred: pipe nodes reference frozen->iop instances.

  // Smoke-test render at dev's current viewport, synchronously and inline (no job -- there is
  // nothing else that could hold a reference to `engine` yet): validates the capture the same way
  // a full-resolution render used to (a failure here aborts the capture, same contract callers
  // already rely on), and primes the first draw so it is never a blank frame.
  dt_iop_roi_t roi = { 0 };
  gboolean ok = FALSE;
  if(_compute_main_roi(dev, engine->pipe, &roi))
    ok = _sync_main_now(engine, &roi);
  _sync_preview_now(engine, dev);

  engine->captured = ok;
  if(!ok)
  {
    _engine_unref(engine);
    goto fail;
  }

  snap->engine = engine;
  return TRUE;

fail:
  if(history_override) g_list_free_full(history_override, dt_free_gpointer);
  if(iop_order_override) g_list_free_full(iop_order_override, dt_free_gpointer);
  return FALSE;
}

void dt_dev_snapshot_draw(dt_dev_snapshot_t *snap, cairo_t *cri, struct dt_develop_t *dev,
                           int32_t width, int32_t height,
                           double clip_x, double clip_y, double clip_w, double clip_h)
{
  if(IS_NULL_PTR(snap) || IS_NULL_PTR(snap->engine) || IS_NULL_PTR(dev) || IS_NULL_PTR(cri)) return;
  if(clip_w <= 0.0 || clip_h <= 0.0) return;

  dt_dev_snapshot_engine_t *engine = snap->engine;

  dt_iop_roi_t want_roi = { 0 };
  const gboolean want_ok = _compute_main_roi(dev, engine->pipe, &want_roi);
  _sync_preview_now(engine, dev);

  dt_pthread_mutex_lock(&engine->lock);
  const gboolean roi_valid = engine->roi_valid;
  const dt_iop_roi_t last_roi = engine->last_roi;
  dt_pthread_mutex_unlock(&engine->lock);

  const gboolean main_ready = want_ok && roi_valid && _roi_equal(&last_roi, &want_roi);
  if(want_ok && !main_ready) _schedule_main_recompute(engine, &want_roi);

  if(!main_ready && !engine->preview_roi_valid) return;

  dt_aligned_pixel_t bg_color = { 0.0f };
  dt_dev_get_background_color(dev, bg_color);

  cairo_save(cri);
  cairo_rectangle(cri, clip_x, clip_y, clip_w, clip_h);
  cairo_clip(cri);

  if(main_ready)
  {
    if(dt_dev_lock_pipe_surface(dev, engine->pipe, &engine->locked, &engine->wait, "snapshot", FALSE)
       && !IS_NULL_PTR(engine->locked.surface))
      dt_dev_render_locked_surface(cri, dev, &engine->locked, width, height, dev->roi.border_size, bg_color);
  }
  else
  {
    cairo_set_source_rgb(cri, bg_color[0], bg_color[1], bg_color[2]);
    cairo_paint(cri);
    _draw_preview_fallback(engine, dev, cri, width, height);
  }

  cairo_restore(cri);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
