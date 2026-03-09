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

#ifdef HAVE_CONFIG_H
#include "common/darktable.h"
#include "config.h"
#endif

#include "common/image.h"
#include "common/imagebuf.h"
#include "common/imageio.h"
#include "common/imageio_module.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/iop_profile.h"
#include "common/opencl.h"
#include "common/dtpthread.h"
#include "bauhaus/bauhaus.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/dev_history.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/imageop_math.h"
#include "develop/pixelpipe_cache.h"
#include "develop/noise_generator.h"
#include "gui/gtk.h"
#include "gui/color_picker_proxy.h"
#include "iop/drawlayer/brush.h"
#include "iop/drawlayer/cache.h"
#include "iop/drawlayer/io.h"
#include "iop/drawlayer/paint.h"
#include "iop/drawlayer/worker.h"
#include "iop/drawlayer/widgets.h"
#include "iop/iop_api.h"

#include <glib/gstdio.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>

DT_MODULE_INTROSPECTION(1, dt_iop_drawlayer_params_t)

/** @file
 *  @brief Drawlayer module entrypoints and runtime orchestration.
 */

/*
 * drawlayer architecture summary
 * ------------------------------
 *
 * This module stores a painted premultiplied RGBA layer in a half-float TIFF sidecar.
 * The persistent layer lives in full image coordinates (raw-sized canvas, but in the
 * module's current pipeline geometry, not in raw sensor geometry). The GUI keeps:
 *
 * 1. a full-resolution half-float cache of the selected TIFF layer (`base_patch`),
 * 2. (legacy) a widget-sized ARGB preview overlay (`live_surface`) for immediate feedback,
 * 3. a worker thread that consumes stroke samples while the module has focus.
 *
 * The current stroke model is intentionally conservative:
 * - `distance` defines a spatial metronome for resampling the path independently of
 *   `mouse_moved` dispatch cadence.
 * - smoothing is applied only to the incoming raw cursor point before that resampling.
 * - the kept implementation uses a simple linear extrapolation from two anchor samples
 *   separated by one brush radius. Several "cleverer" variants were tried here
 *   (kernel caching, fixed-N smoothing windows, higher-order extrapolation, inertial
 *   direction filters), but they either benchmarked slower, introduced sampling
 *   dependence again, or created visible cusps/overshoot. Those attempts were removed
 *   and are documented locally near the code they affected.
 *
 * The guiding rule throughout this file is: keep the authoritative geometry in layer
 * space, and derive widget-space feedback from it, so GUI preview and pipeline output
 * cannot drift apart due to duplicated math.
 */

#define DRAWLAYER_NAME_SIZE 64
#define DRAWLAYER_PROFILE_SIZE 256
#define DRAWLAYER_WORKER_RING_CAPACITY 65536
#define DRAWLAYER_COMPARE_ANALYTIC_TIMINGS 1

typedef enum drawlayer_mapping_profile_t
{
  DRAWLAYER_PROFILE_LINEAR = 0,
  DRAWLAYER_PROFILE_QUADRATIC = 1,
  DRAWLAYER_PROFILE_SQRT = 2,
  DRAWLAYER_PROFILE_INV_LINEAR = 3,
  DRAWLAYER_PROFILE_INV_SQRT = 4,
  DRAWLAYER_PROFILE_INV_QUADRATIC = 5,
} drawlayer_mapping_profile_t;

typedef enum drawlayer_input_map_flag_t
{
  DRAWLAYER_INPUT_MAP_PRESSURE_SIZE = 1u << 0,
  DRAWLAYER_INPUT_MAP_PRESSURE_OPACITY = 1u << 1,
  DRAWLAYER_INPUT_MAP_PRESSURE_FLOW = 1u << 2,
  DRAWLAYER_INPUT_MAP_PRESSURE_SOFTNESS = 1u << 3,
  DRAWLAYER_INPUT_MAP_TILT_SIZE = 1u << 4,
  DRAWLAYER_INPUT_MAP_TILT_OPACITY = 1u << 5,
  DRAWLAYER_INPUT_MAP_TILT_FLOW = 1u << 6,
  DRAWLAYER_INPUT_MAP_TILT_SOFTNESS = 1u << 7,
  DRAWLAYER_INPUT_MAP_ACCEL_SIZE = 1u << 8,
  DRAWLAYER_INPUT_MAP_ACCEL_OPACITY = 1u << 9,
  DRAWLAYER_INPUT_MAP_ACCEL_FLOW = 1u << 10,
  DRAWLAYER_INPUT_MAP_ACCEL_SOFTNESS = 1u << 11,
} drawlayer_input_map_flag_t;

typedef struct dt_iop_drawlayer_params_t
{
  unsigned int stroke_commit_hash; // $DEFAULT: 0
  char layer_name[DRAWLAYER_NAME_SIZE];
  char work_profile[DRAWLAYER_PROFILE_SIZE];
  int64_t sidecar_timestamp; // $DEFAULT: 0
  int layer_order; // $DEFAULT: -1
} dt_iop_drawlayer_params_t;

typedef enum drawlayer_preview_bg_mode_t
{
  DRAWLAYER_PREVIEW_BG_IMAGE = 0,
  DRAWLAYER_PREVIEW_BG_WHITE = 1,
  DRAWLAYER_PREVIEW_BG_GREY = 2,
  DRAWLAYER_PREVIEW_BG_BLACK = 3,
} drawlayer_preview_bg_mode_t;

typedef enum drawlayer_pick_source_t
{
  DRAWLAYER_PICK_SOURCE_INPUT = 0,
  DRAWLAYER_PICK_SOURCE_OUTPUT = 1,
} drawlayer_pick_source_t;

typedef struct drawlayer_paint_backend_ctx_t drawlayer_paint_backend_ctx_t;

typedef dt_drawlayer_cache_patch_t drawlayer_patch_t;

typedef struct drawlayer_view_patch_t
{
  int x;
  int y;
  int width;
  int height;
} drawlayer_view_patch_t;

typedef struct drawlayer_dir_info_t
{
  gboolean found;
  int index;
  int count;
  uint32_t width;
  uint32_t height;
  char name[DRAWLAYER_NAME_SIZE];
  char work_profile[DRAWLAYER_PROFILE_SIZE];
} drawlayer_dir_info_t;

typedef struct dt_iop_drawlayer_gui_data_t
{
  /* True between button-press and button-release while a stroke is being captured. */
  gboolean painting;
  /* Whether a pointer is currently active over the center view for cursor preview drawing. */
  gboolean pointer_valid;
  /* Last darkroom ROI state seen by the GUI; used to detect pan/zoom changes. */
  float last_view_x;
  float last_view_y;
  float last_view_scale;
  /* Visible layer-space rectangle currently mirrored by the backend process patch. */
  dt_drawlayer_damaged_rect_t live_view_rect;
  /* Extra layer-space padding around the visible view so strokes can extend off-screen. */
  float live_padding;
  /* Widget-space rectangle where the transient overlay cairo surface is painted. */
  dt_drawlayer_damaged_rect_t preview_rect;

  /* Damage state for backend and process raster paths. */
  dt_drawlayer_damaged_rect_t *backend_path;
  dt_drawlayer_damaged_rect_t *process_path;
  /* `base_patch` mirrors the selected TIFF page in memory at full resolution. */
  drawlayer_patch_t base_patch;
  /* `process_patch` is the current visible tile in process/output coordinates
   * (module ROI size) used as the backend write target. */
  drawlayer_patch_t process_patch;
  /* Front read-only snapshot used by process()/process_cl() blending. */
  drawlayer_patch_t process_read_patch;
  /* Serialize process-patch/stroke-mask mutation between the realtime worker
   * and GUI/cache rebuild paths. */
  dt_pthread_mutex_t process_patch_mutex;
  /* Single-level undo snapshot of the full-resolution cached layer, swapped by reference. */
  drawlayer_patch_t undo_patch;
  /* `stroke_mask` stores the current stroke's own accumulated alpha in full layer
   * coordinates. It is reset at stroke start and lets flow=0 reason about "no
   * self build-up" against the current stroke only, not against the pre-existing
   * destination alpha already on the canvas. */
  drawlayer_patch_t stroke_mask;
  /* Stroke-local alpha mask for `process_patch`, so flow=0 remains consistent
   * when the backend worker updates the visible source crop directly. */
  drawlayer_patch_t process_stroke_mask;
  /* Visible layer-space rectangle used to detect pan/zoom geometry changes. */
  drawlayer_view_patch_t live_patch;
  /* Whether `base_patch` currently mirrors the selected TIFF layer successfully. */
  gboolean cache_valid;
  /* Whether `base_patch` has local edits not yet flushed back to the sidecar TIFF. */
  gboolean cache_dirty;
  /* Availability flags for the single-level undo/redo swap buffers. */
  gboolean undo_available;
  gboolean redo_available;
  /* Image/layer identity cached with `base_patch`, so the cache can be invalidated on change. */
  int32_t cache_imgid;
  char cache_layer_name[DRAWLAYER_NAME_SIZE];
  int cache_layer_order;
  /* Extra retained references held on top of the base patch's ownership ref.
   * These make the authoritative cache line intentionally "sticky" in the
   * global pixelpipe cache:
   * - one extra ref when the base patch is first materialized from TIFF,
   * - one extra ref for each committed GUI stroke since the last explicit save.
   * Saving the sidecar releases those extra refs again. */
  gboolean base_patch_loaded_ref;
  uint32_t base_patch_stroke_refs;
  /* Private runtime revision of `base_patch` content. Unlike the serialized
   * history hash, this exists only to invalidate internal derived caches (such
   * as the post-zoom process tile cache) when the in-memory layer changes. */
  uint64_t cache_epoch;
  /* Geometry signature and affine mapping for `process_patch`. Unlike
   * `cache_epoch`, this tracks only scale/translation geometry changes; content
   * changes are applied directly into the visible source crop by the backend
   * worker. */
  gboolean process_patch_valid;
  /* True when `process_patch` contains edits not yet folded back into
   * `base_patch`. This lets flush paths skip expensive resampling when the
   * transformed tile only mirrors base content. */
  gboolean process_patch_dirty;
  /* Union of unsynced process-tile edits, expressed in base/layer coordinates.
   * Flushing only this rectangle avoids full-tile upsampling on every sync. */
  dt_drawlayer_damaged_rect_t process_dirty_rect;
  /* Extra padding around visible ROI in process/output pixels so edge dabs
   * are fully represented in the realtime process patch. */
  int process_patch_padding;
  uint64_t process_geom_hash;
  uint64_t process_cl_prewarm_hash;
  int process_cl_prewarm_devid;
#ifdef HAVE_OPENCL
  cl_mem process_read_clmem;
  int process_read_clmem_width;
  int process_read_clmem_height;
  int process_read_clmem_devid;
  gboolean process_read_clmem_dirty;
#endif
  dt_iop_roi_t process_combined_roi;
  /* Opaque realtime worker state owned by drawlayer/worker.c. This hides the worker
   * threads, ring buffers, synchronization primitives, and async callback ids
   * from the GUI/controller layer. */
  dt_drawlayer_worker_t *rt;
  /* Number of raw samples queued for the current stroke, used for history/hash bookkeeping. */
  guint stroke_sample_count;
  /* Monotonic event index within current stroke, attached to queued raw input events. */
  uint32_t stroke_event_index;
  /* Last backend-processed dab center in layer coordinates, used to seed the
   * stroke commit hash deterministically without wall-clock time. */
  gboolean last_dab_valid;
  float last_dab_x;
  float last_dab_y;
  /* A stroke has ended and history commit should happen once workers are drained. */
  gboolean finish_commit_pending;
  /* Monotonic stroke ids so overlapping queued strokes can be disambiguated safely. */
  uint32_t current_stroke_batch;
  /* GUI-only preview mode: image / solid white / solid grey / solid black. */
  int preview_bg_mode;
  /* Suppress repeated "missing layer" prompts while the same missing name stays selected.
   * Empty string means "no prompt currently tracked". */
  char missing_layer_prompt_name[DRAWLAYER_NAME_SIZE];
  /* Custom drawlayer widgets state (picker, swatches and their cairo caches). */
  dt_drawlayer_widgets_t *widgets;
  /* Cached cursor stamp preview painted in gui_post_expose(). It mirrors the
   * current brush fall-off, display color and opacity at the current on-screen
   * brush radius, so the cursor shows a filled stamp instead of only an outline. */
  cairo_surface_t *cursor_surface;
  int cursor_surface_size;
  double cursor_surface_ppd;
  float cursor_radius;
  float cursor_opacity;
  float cursor_hardness;
  int cursor_shape;
  float cursor_color[3];

  /* Brush controls (GUI-only; persisted through dt_conf, not module params). */
  GtkWidget *brush_shape;
  GtkWidget *brush_mode;
  GtkWidget *color;
  GtkWidget *color_row;
  GtkWidget *color_swatch;
  GtkWidget *image_colorpicker;
  GtkWidget *image_colorpicker_source;
  GtkWidget *size;
  GtkWidget *distance;
  GtkWidget *smoothing;
  GtkWidget *opacity;
  GtkWidget *flow;
  GtkWidget *sprinkles;
  GtkWidget *sprinkle_size;
  GtkWidget *sprinkle_coarseness;
  GtkWidget *softness;
  GtkWidget *hdr_exposure;
  /* Layer selection / layer actions. */
  GtkEntry *layer_name;
  GtkWidget *layer_select;
  GtkWidget *undo_button;
  GtkWidget *redo_button;
  GtkWidget *preview_bg_image;
  GtkWidget *preview_bg_white;
  GtkWidget *preview_bg_grey;
  GtkWidget *preview_bg_black;
  GtkWidget *delete_layer;
  GtkWidget *create_layer;
  GtkWidget *create_background;
  GtkWidget *save_layer;
  GtkWidget *fill_white;
  GtkWidget *fill_black;
  GtkWidget *fill_transparent;

  /* Optional stylus mappings for pressure. */
  GtkWidget *map_pressure_size;
  GtkWidget *map_pressure_opacity;
  GtkWidget *map_pressure_flow;
  GtkWidget *map_pressure_softness;

  /* Optional stylus mappings for tilt. */
  GtkWidget *map_tilt_size;
  GtkWidget *map_tilt_opacity;
  GtkWidget *map_tilt_flow;
  GtkWidget *map_tilt_softness;

  /* Optional stylus mappings for pointer acceleration. */
  GtkWidget *map_accel_size;
  GtkWidget *map_accel_opacity;
  GtkWidget *map_accel_flow;
  GtkWidget *map_accel_softness;

  GtkWidget *pressure_profile;
  GtkWidget *tilt_profile;
  GtkWidget *accel_profile;
  gboolean background_job_running;
} dt_iop_drawlayer_gui_data_t;

typedef struct dt_iop_drawlayer_data_t
{
  /* Keep serialized params as the first field so the pipe runtime can mirror the
   * module params while also carrying headless-only caches for non-GUI pipes. */
  dt_iop_drawlayer_params_t params;

  /* Non-GUI pipelines (thumbnail/export/headless) do not have `gui_data`, so
   * they need their own in-memory authoritative layer cache loaded outside of
   * `process()`. This is loaded in `commit_params()` and then reused read-only
   * by `process()`. */
  drawlayer_patch_t headless_base_patch;
  gboolean headless_cache_valid;
  int32_t headless_cache_imgid;
  char headless_cache_layer_name[DRAWLAYER_NAME_SIZE];
  int headless_cache_layer_order;
} dt_iop_drawlayer_data_t;

#ifdef HAVE_OPENCL
typedef struct dt_iop_drawlayer_global_data_t
{
  int kernel_premult_over;
} dt_iop_drawlayer_global_data_t;
#endif

static gboolean _commit_dabs(dt_iop_module_t *self, const gboolean record_history);
static gboolean _flush_layer_cache(dt_iop_module_t *self);
static gboolean _compute_view_patch(dt_iop_module_t *self, float padding, drawlayer_view_patch_t *patch,
                                    float *x0, float *y0, float *x1, float *y1);
static gboolean _widget_to_layer_coords(dt_iop_module_t *self, const double wx, const double wy,
                                        float *lx, float *ly);
static gboolean _layer_to_widget_coords(dt_iop_module_t *self, const float x, const float y,
                                        float *wx, float *wy);
static void _process_backend_dab(dt_iop_module_t *self, const dt_drawlayer_brush_dab_t *dab,
                                 drawlayer_paint_backend_ctx_t *ctx);
static void _touch_stroke_commit_hash(dt_iop_drawlayer_params_t *params, int dab_count,
                                      gboolean have_last_dab, float last_dab_x, float last_dab_y);
static void _flush_process_patch_to_base(dt_iop_module_t *self, dt_iop_drawlayer_gui_data_t *g);
static void _flush_process_patch_to_base_locked(dt_iop_module_t *self, dt_iop_drawlayer_gui_data_t *g);
static void _invalidate_undo_redo(dt_iop_module_t *self);
static void _sync_save_button(dt_iop_module_t *self);
static void _sync_mode_sensitive_widgets(dt_iop_module_t *self);
static gboolean _sync_temp_buffers(dt_iop_module_t *self, gboolean flush_pending, gboolean record_history);
static int _offer_missing_layer_recreation(dt_iop_module_t *self, const char *missing_name);
static void _set_drawlayer_pipeline_realtime_mode(dt_iop_module_t *self, gboolean state);
static gboolean _background_layer_job_done_idle(gpointer user_data);
static gboolean _replay_finished_stroke_to_base_patch(dt_iop_module_t *self, const GArray *history,
                                                      float distance_percent);
static gboolean _prime_live_process_patch_before_stroke(dt_iop_module_t *self);

typedef struct drawlayer_wait_dialog_t
{
  GtkWidget *dialog;
} drawlayer_wait_dialog_t;

static gboolean _is_drawlayer_display_pipe(const dt_iop_module_t *self, const dt_dev_pixelpipe_iop_t *piece)
{
  if(!self || !self->dev || !piece || !piece->pipe) return FALSE;
  if(self->dev->gui_module != self) return FALSE;
  /* Displayed darkroom rendering can come from either `dev->pipe` or
   * `dev->preview_pipe` depending on current scheduling/state. Both must use
   * the GUI-owned process patch fast path to avoid falling back to the
   * headless full-res clip+zoom path (slow and stale during live strokes). */
  return (piece->pipe == self->dev->pipe || piece->pipe == self->dev->preview_pipe);
}
static inline float _clamp01(const float value)
{
  return fminf(fmaxf(value, 0.0f), 1.0f);
}

static gboolean _ensure_external_patch_buffer(drawlayer_patch_t *patch, const int width, const int height,
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

static void _copy_patch_rect(const drawlayer_patch_t *src, drawlayer_patch_t *dst,
                             const dt_drawlayer_damaged_rect_t *rect)
{
  if(!src || !dst || !rect || !rect->valid || !src->pixels || !dst->pixels) return;

  const int x0 = CLAMP(rect->nw[0], 0, MIN(src->width, dst->width));
  const int y0 = CLAMP(rect->nw[1], 0, MIN(src->height, dst->height));
  const int x1 = CLAMP(rect->se[0], 0, MIN(src->width, dst->width));
  const int y1 = CLAMP(rect->se[1], 0, MIN(src->height, dst->height));
  if(x1 <= x0 || y1 <= y0) return;

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) \
  dt_omp_firstprivate(src, dst, x0, x1, y0, y1)
#endif
  for(int y = y0; y < y1; y++)
  {
    const float *src_row = src->pixels + 4 * ((size_t)y * src->width + x0);
    float *dst_row = dst->pixels + 4 * ((size_t)y * dst->width + x0);
    memcpy(dst_row, src_row, (size_t)(x1 - x0) * 4 * sizeof(float));
  }
}

static gboolean _publish_process_patch_locked(dt_iop_drawlayer_gui_data_t *g,
                                              const dt_drawlayer_damaged_rect_t *damage,
                                              const gboolean full_copy)
{
  if(!g || !g->process_patch.pixels || g->process_patch.width <= 0 || g->process_patch.height <= 0) return FALSE;
  if(!full_copy && (!damage || !damage->valid)) return FALSE;

  const int width = g->process_patch.width;
  const int height = g->process_patch.height;
  if(!_ensure_external_patch_buffer(&g->process_read_patch, width, height, "drawlayer process read tile"))
    return FALSE;

  const dt_drawlayer_damaged_rect_t full_rect = {
    .valid = TRUE,
    .nw = { 0, 0 },
    .se = { width, height },
  };
  if(full_copy)
  {
    _copy_patch_rect(&g->process_patch, &g->process_read_patch, &full_rect);
#ifdef HAVE_OPENCL
    /* `process_read_patch` is an external host buffer that may also be mirrored
     * through the generic pixelpipe pinned-image cache. Invalidate those cached
     * host-backed device objects whenever we rewrite the snapshot in place so a
     * later focused drawlayer render cannot reopen stale GPU views for the same
     * host pointer after pan/zoom or stroke updates. */
    dt_dev_pixelpipe_cache_flush_host_pinned_image(darktable.pixelpipe_cache, g->process_read_patch.pixels, NULL, -1);
    g->process_read_clmem_dirty = TRUE;
#endif
    return TRUE;
  }

  /* Incremental publish updates the single read snapshot in place while the
   * process-patch mutex is held, so readers never observe a partially copied
   * tile and stale snapshots are not reintroduced by buffer swapping. */
  _copy_patch_rect(&g->process_patch, &g->process_read_patch, damage);
#ifdef HAVE_OPENCL
  /* See full-copy case above: keep the generic host-pinned cache in sync with
   * the authoritative host snapshot stored in `process_read_patch`. */
  dt_dev_pixelpipe_cache_flush_host_pinned_image(darktable.pixelpipe_cache, g->process_read_patch.pixels, NULL, -1);
  g->process_read_clmem_dirty = TRUE;
#endif
  return TRUE;
}

static void _clear_cursor_stamp_surface(dt_iop_drawlayer_gui_data_t *g)
{
  if(!g) return;
  if(g->cursor_surface)
  {
    cairo_surface_destroy(g->cursor_surface);
    g->cursor_surface = NULL;
  }
  g->cursor_surface_size = 0;
  g->cursor_surface_ppd = 0.0;
  g->cursor_radius = 0.0f;
  g->cursor_opacity = 0.0f;
  g->cursor_hardness = 0.0f;
  g->cursor_shape = -1;
  g->cursor_color[0] = g->cursor_color[1] = g->cursor_color[2] = -1.0f;
}

#ifdef HAVE_OPENCL
static void _clear_process_read_clmem(dt_iop_drawlayer_gui_data_t *g)
{
  if(!g) return;
  if(g->process_read_clmem)
    dt_opencl_release_mem_object(g->process_read_clmem);
  g->process_read_clmem = NULL;
  g->process_read_clmem_width = 0;
  g->process_read_clmem_height = 0;
  g->process_read_clmem_devid = -1;
  g->process_read_clmem_dirty = TRUE;
}

static cl_mem _ensure_process_read_clmem_locked(dt_iop_drawlayer_gui_data_t *g, const int devid)
{
  if(!g || devid < 0 || !g->process_read_patch.pixels
     || g->process_read_patch.width <= 0 || g->process_read_patch.height <= 0)
    return NULL;

  const gboolean need_realloc = (!g->process_read_clmem
                                 || g->process_read_clmem_devid != devid
                                 || g->process_read_clmem_width != g->process_read_patch.width
                                 || g->process_read_clmem_height != g->process_read_patch.height);
  if(need_realloc)
  {
    _clear_process_read_clmem(g);
    g->process_read_clmem = dt_opencl_alloc_device(devid,
                                                   g->process_read_patch.width,
                                                   g->process_read_patch.height,
                                                   4 * sizeof(float));
    if(!g->process_read_clmem) return NULL;
    g->process_read_clmem_width = g->process_read_patch.width;
    g->process_read_clmem_height = g->process_read_patch.height;
    g->process_read_clmem_devid = devid;
    g->process_read_clmem_dirty = TRUE;
  }

  if(g->process_read_clmem_dirty)
  {
    if(dt_opencl_write_host_to_device(devid,
                                      g->process_read_patch.pixels,
                                      g->process_read_clmem,
                                      g->process_read_patch.width,
                                      g->process_read_patch.height,
                                      4 * sizeof(float)) != CL_SUCCESS)
    {
      _clear_process_read_clmem(g);
      return NULL;
    }
    g->process_read_clmem_dirty = FALSE;
  }

  return g->process_read_clmem;
}
#endif

#include "drawlayer/conf.c"

static void _get_brush_colors(dt_iop_module_t *self, float display_rgb[3], float pipeline_rgb[3])
{
  float raw_display_rgb[3] = { 0.0f };
  _conf_display_color(raw_display_rgb);
  pipeline_rgb[0] = raw_display_rgb[0];
  pipeline_rgb[1] = raw_display_rgb[1];
  pipeline_rgb[2] = raw_display_rgb[2];

  // The GUI overlay is painted through cairo as a display-referred feedback surface.
  display_rgb[0] = _clamp01(raw_display_rgb[0]);
  display_rgb[1] = _clamp01(raw_display_rgb[1]);
  display_rgb[2] = _clamp01(raw_display_rgb[2]);

  if(self && self->dev && self->dev->pipe)
  {
    const dt_iop_order_iccprofile_info_t *const display_profile = dt_ioppr_get_pipe_output_profile_info(self->dev->pipe);
    const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_iop_work_profile_info(self, self->dev->iop);
    if(display_profile && work_profile)
    {
      float in[4] = { raw_display_rgb[0], raw_display_rgb[1], raw_display_rgb[2], 0.0f };
      float out[4] = { raw_display_rgb[0], raw_display_rgb[1], raw_display_rgb[2], 0.0f };
      dt_ioppr_transform_image_colorspace_rgb(in, out, 1, 1, display_profile, work_profile, "drawlayer brush color");
      pipeline_rgb[0] = out[0];
      pipeline_rgb[1] = out[1];
      pipeline_rgb[2] = out[2];
    }
  }

  const float gain = exp2f(_conf_hdr_exposure());
  pipeline_rgb[0] *= gain;
  pipeline_rgb[1] *= gain;
  pipeline_rgb[2] *= gain;
}

static inline void _set_toggle_if_valid(GtkWidget *widget, const gboolean active)
{
  if(GTK_IS_TOGGLE_BUTTON(widget)) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), active);
}

static void _paint_reset_worker_paths(dt_iop_drawlayer_gui_data_t *g)
{
  /* Reset all active path generators together so controller code does not
   * duplicate the bookkeeping. */
  if(!g) return;
  dt_drawlayer_paint_runtime_state_reset(g->backend_path);
  dt_drawlayer_paint_runtime_state_reset(g->process_path);
}

static void _paint_reset_stroke_runtime(dt_iop_drawlayer_gui_data_t *g)
{
  /* Common stroke-side reset used by commits, clears, and initialization. */
  if(!g) return;
  _paint_reset_worker_paths(g);
}

static inline float _mapping_profile_value(const drawlayer_mapping_profile_t profile, const float x)
{
  const float v = _clamp01(x);
  switch(profile)
  {
    case DRAWLAYER_PROFILE_QUADRATIC:
      return 1.f + v * v;
    case DRAWLAYER_PROFILE_SQRT:
      return 1.f + sqrtf(v);
    case DRAWLAYER_PROFILE_INV_LINEAR:
      return 1.0f / (1.f + v);
    case DRAWLAYER_PROFILE_INV_SQRT:
      return 1.0f / (1.f + sqrtf(v));
    case DRAWLAYER_PROFILE_INV_QUADRATIC:
      return 1.0f / (1.f + v * v);
    case DRAWLAYER_PROFILE_LINEAR:
    default:
      return 1.f + v;
  }
}

static inline float _mapping_multiplier(const drawlayer_mapping_profile_t profile, const float normalized_input)
{
  return _mapping_profile_value(profile, normalized_input);
}

static void _fill_input_brush_settings(dt_iop_module_t *self, dt_drawlayer_paint_raw_input_t *input)
{
  if(!self || !input) return;

  uint32_t map_flags = 0u;
  if(dt_conf_get_bool(DRAWLAYER_CONF_MAP_PRESSURE_SIZE)) map_flags |= DRAWLAYER_INPUT_MAP_PRESSURE_SIZE;
  if(dt_conf_get_bool(DRAWLAYER_CONF_MAP_PRESSURE_OPACITY)) map_flags |= DRAWLAYER_INPUT_MAP_PRESSURE_OPACITY;
  if(dt_conf_get_bool(DRAWLAYER_CONF_MAP_PRESSURE_FLOW)) map_flags |= DRAWLAYER_INPUT_MAP_PRESSURE_FLOW;
  if(dt_conf_get_bool(DRAWLAYER_CONF_MAP_PRESSURE_SOFTNESS)) map_flags |= DRAWLAYER_INPUT_MAP_PRESSURE_SOFTNESS;
  if(dt_conf_get_bool(DRAWLAYER_CONF_MAP_TILT_SIZE)) map_flags |= DRAWLAYER_INPUT_MAP_TILT_SIZE;
  if(dt_conf_get_bool(DRAWLAYER_CONF_MAP_TILT_OPACITY)) map_flags |= DRAWLAYER_INPUT_MAP_TILT_OPACITY;
  if(dt_conf_get_bool(DRAWLAYER_CONF_MAP_TILT_FLOW)) map_flags |= DRAWLAYER_INPUT_MAP_TILT_FLOW;
  if(dt_conf_get_bool(DRAWLAYER_CONF_MAP_TILT_SOFTNESS)) map_flags |= DRAWLAYER_INPUT_MAP_TILT_SOFTNESS;
  if(dt_conf_get_bool(DRAWLAYER_CONF_MAP_ACCEL_SIZE)) map_flags |= DRAWLAYER_INPUT_MAP_ACCEL_SIZE;
  if(dt_conf_get_bool(DRAWLAYER_CONF_MAP_ACCEL_OPACITY)) map_flags |= DRAWLAYER_INPUT_MAP_ACCEL_OPACITY;
  if(dt_conf_get_bool(DRAWLAYER_CONF_MAP_ACCEL_FLOW)) map_flags |= DRAWLAYER_INPUT_MAP_ACCEL_FLOW;
  if(dt_conf_get_bool(DRAWLAYER_CONF_MAP_ACCEL_SOFTNESS)) map_flags |= DRAWLAYER_INPUT_MAP_ACCEL_SOFTNESS;

  float display_rgb[3] = { 0.0f };
  float pipeline_rgb[3] = { 0.0f };
  _get_brush_colors(self, display_rgb, pipeline_rgb);

  input->map_flags = map_flags;
  input->pressure_profile = (uint8_t)_conf_mapping_profile(DRAWLAYER_CONF_PRESSURE_PROFILE);
  input->tilt_profile = (uint8_t)_conf_mapping_profile(DRAWLAYER_CONF_TILT_PROFILE);
  input->accel_profile = (uint8_t)_conf_mapping_profile(DRAWLAYER_CONF_ACCEL_PROFILE);
  input->distance_percent = _conf_distance() / 100.0f;
  input->smoothing_percent = _conf_smoothing() / 100.0f;
  input->brush_radius = _conf_size();
  input->brush_opacity = _conf_opacity() / 100.0f;
  input->brush_flow = _conf_flow() / 100.0f;
  input->brush_hardness = _conf_hardness();
  input->brush_sprinkles = _conf_sprinkles() / 100.0f;
  input->brush_sprinkle_size = _conf_sprinkle_size();
  input->brush_sprinkle_coarseness = _conf_sprinkle_coarseness() / 100.0f;
  input->brush_shape = _conf_brush_shape();
  input->brush_mode = _conf_brush_mode();
  input->color[0] = pipeline_rgb[0];
  input->color[1] = pipeline_rgb[1];
  input->color[2] = pipeline_rgb[2];
  input->display_color[0] = display_rgb[0];
  input->display_color[1] = display_rgb[1];
  input->display_color[2] = display_rgb[2];
}

static gboolean _build_worker_input_dab(dt_iop_module_t *self, dt_drawlayer_paint_stroke_t *state,
                                        const dt_drawlayer_paint_raw_input_t *input,
                                        dt_drawlayer_brush_dab_t *dab)
{
  /* Convert one raw GUI input event into a fully parameterized dab in layer
   * coordinates.
   *
   * This is still not the final painted dab stream: the result will then be
   * smoothed and resampled at constant distance. This helper is responsible
   * only for:
   *   - widget -> layer coordinate conversion
   *   - dynamic brush settings (size/opacity/flow/hardness)
   *   - pressure / tilt / acceleration mapping
   *   - color preparation in both display and pipeline spaces */
  if(!self || !state || !input || !dab) return FALSE;

  float lx = 0.0f;
  float ly = 0.0f;
  if(!_widget_to_layer_coords(self, input->wx, input->wy, &lx, &ly)) return FALSE;

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
  const drawlayer_mapping_profile_t pressure_profile
      = (drawlayer_mapping_profile_t)CLAMP((int)input->pressure_profile, (int)DRAWLAYER_PROFILE_LINEAR,
                                           (int)DRAWLAYER_PROFILE_INV_QUADRATIC);
  const drawlayer_mapping_profile_t tilt_profile
      = (drawlayer_mapping_profile_t)CLAMP((int)input->tilt_profile, (int)DRAWLAYER_PROFILE_LINEAR,
                                           (int)DRAWLAYER_PROFILE_INV_QUADRATIC);
  const drawlayer_mapping_profile_t accel_profile
      = (drawlayer_mapping_profile_t)CLAMP((int)input->accel_profile, (int)DRAWLAYER_PROFILE_LINEAR,
                                           (int)DRAWLAYER_PROFILE_INV_QUADRATIC);
  const float pressure_coeff = _mapping_multiplier(pressure_profile, pressure_norm);
  const float tilt_coeff = _mapping_multiplier(tilt_profile, tilt_norm);
  const float accel_coeff = _mapping_multiplier(accel_profile, accel_norm);

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

  if((map_pressure_size || map_pressure_opacity || map_pressure_flow || map_pressure_softness
      || map_tilt_size || map_tilt_opacity || map_tilt_flow || map_tilt_softness
      || map_accel_size || map_accel_opacity || map_accel_flow || map_accel_softness)
     && (input->stroke_pos != DT_DRAWLAYER_PAINT_STROKE_MIDDLE
         || ((state->history && (state->history->len & 15u) == 0u))))
  {
    dt_print(DT_DEBUG_INPUT,
             "[drawlayer] map p=%.4f t=%.4f a=%.4f coeff[p=%.4f t=%.4f a=%.4f] "
             "base[r=%.2f o=%.3f f=%.3f h=%.3f] out[r=%.2f o=%.3f f=%.3f h=%.3f] "
             "flags[p=%d%d%d%d t=%d%d%d%d a=%d%d%d%d]\n",
             pressure_norm, tilt_norm, accel_norm,
             pressure_coeff, tilt_coeff, accel_coeff,
             base_radius, base_opacity, base_flow, base_hardness,
             radius, _clamp01(opacity), _clamp01(flow), _clamp01(hardness),
             map_pressure_size ? 1 : 0, map_pressure_opacity ? 1 : 0, map_pressure_flow ? 1 : 0, map_pressure_softness ? 1 : 0,
             map_tilt_size ? 1 : 0, map_tilt_opacity ? 1 : 0, map_tilt_flow ? 1 : 0, map_tilt_softness ? 1 : 0,
             map_accel_size ? 1 : 0, map_accel_opacity ? 1 : 0, map_accel_flow ? 1 : 0, map_accel_softness ? 1 : 0);
  }

  return TRUE;
}

/* GUI worker painting into uint8 buffers is deimplemented. Realtime preview
 * now relies on the regular pipeline/backbuffer path. */

struct drawlayer_paint_backend_ctx_t
{
  dt_iop_module_t *self;
  dt_drawlayer_paint_stroke_t *stroke;
  gboolean have_pending_publish;
  float last_dab_x;
  float last_dab_y;
};

static gboolean _paint_build_dab_cb(void *user_data, dt_drawlayer_paint_stroke_t *state,
                                    const dt_drawlayer_paint_raw_input_t *input,
                                    dt_drawlayer_brush_dab_t *out_dab)
{
  drawlayer_paint_backend_ctx_t *ctx = (drawlayer_paint_backend_ctx_t *)user_data;
  return (ctx && ctx->self)
             ? _build_worker_input_dab(ctx->self, state, (const dt_drawlayer_paint_raw_input_t *)input,
                                       (dt_drawlayer_brush_dab_t *)out_dab)
             : FALSE;
}

static gboolean _paint_layer_to_widget_cb(void *user_data, const float lx, const float ly, float *wx, float *wy)
{
  drawlayer_paint_backend_ctx_t *ctx = (drawlayer_paint_backend_ctx_t *)user_data;
  return (ctx && ctx->self) ? _layer_to_widget_coords(ctx->self, lx, ly, wx, wy) : FALSE;
}

static void _paint_emit_backend_dab_cb(void *user_data, const dt_drawlayer_brush_dab_t *dab)
{
  drawlayer_paint_backend_ctx_t *ctx = (drawlayer_paint_backend_ctx_t *)user_data;
  if(ctx && ctx->self)
    _process_backend_dab(ctx->self, (const dt_drawlayer_brush_dab_t *)dab, ctx);
}

static void _paint_stroke_seed_cb(void *user_data, const uint64_t stroke_seed)
{
  drawlayer_paint_backend_ctx_t *ctx = (drawlayer_paint_backend_ctx_t *)user_data;
  if(!ctx) return;
  if(ctx->stroke) dt_drawlayer_paint_runtime_set_stroke_seed(ctx->stroke, stroke_seed);
}

static void _publish_backend_progress(drawlayer_paint_backend_ctx_t *ctx)
{
  if(!ctx || !ctx->self || !ctx->have_pending_publish) return;

  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)ctx->self->gui_data;
  if(!g) return;

  dt_iop_drawlayer_params_t *params = (dt_iop_drawlayer_params_t *)ctx->self->params;
  const int sample_count = (int)g->stroke_sample_count;
  _touch_stroke_commit_hash(params, sample_count, TRUE, ctx->last_dab_x, ctx->last_dab_y);

  dt_develop_t *dev = ctx->self->dev;
  if(dev)
  {
    /* Publish once per drained raw-input batch. The first motion event can
     * backfill many dabs at once, so doing this per emitted sample adds a
     * start-of-stroke latency multiplier unrelated to rasterization itself. */
    dt_pthread_rwlock_wrlock(&dev->history_mutex);
    dt_dev_add_history_item_ext(dev, ctx->self, FALSE, FALSE);
    dev->history_hash = dt_dev_history_get_hash(dev);
    dt_pthread_rwlock_unlock(&dev->history_mutex);
  }

  ctx->have_pending_publish = FALSE;
}

static void _process_backend_input(dt_iop_module_t *self, const dt_drawlayer_paint_raw_input_t *input,
                                   dt_drawlayer_paint_stroke_t *stroke)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !input || !stroke) return;

  drawlayer_paint_backend_ctx_t ctx = {
    .self = self,
    .stroke = stroke,
  };
  const dt_drawlayer_paint_callbacks_t callbacks = {
    .build_dab = _paint_build_dab_cb,
    .layer_to_widget = _paint_layer_to_widget_cb,
    .emit_dab = _paint_emit_backend_dab_cb,
    .on_stroke_seed = _paint_stroke_seed_cb,
  };
  if(!dt_drawlayer_paint_queue_raw_input(stroke, input)) return;
  dt_drawlayer_paint_raster_path(stroke, &callbacks, &ctx);
  _publish_backend_progress(&ctx);
}

static void _flush_pending_backend_input(dt_iop_module_t *self,
                                         dt_drawlayer_paint_stroke_t *stroke)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !stroke) return;

  drawlayer_paint_backend_ctx_t ctx = {
    .self = self,
    .stroke = stroke,
  };
  const dt_drawlayer_paint_callbacks_t callbacks = {
    .build_dab = _paint_build_dab_cb,
    .layer_to_widget = _paint_layer_to_widget_cb,
    .emit_dab = _paint_emit_backend_dab_cb,
    .on_stroke_seed = _paint_stroke_seed_cb,
  };
  dt_drawlayer_paint_finalize_path(stroke, &callbacks, &ctx);
  _publish_backend_progress(&ctx);
}

static void _publish_backend_stroke_damage(dt_iop_module_t *self)
{
  /* Called by drawlayer commit path (main module layer), not by worker code.
   * Paint runtime owns stroke accumulation; drawlayer owns how that damage is
   * consumed. Now that the backend always writes the authoritative base patch
   * directly, process-tile damage no longer implies "unsynced process edits"
   * that need a later flush back to base. */
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g) return;

  dt_drawlayer_paint_runtime_state_reset(g->process_path);

  dt_drawlayer_damaged_rect_t backend_damage = { 0 };
  if(dt_drawlayer_paint_merge_runtime_stroke_damage(g->backend_path, &backend_damage))
    g->cache_dirty = TRUE;
}

typedef struct drawlayer_fullres_replay_scratch_t
{
  float *replay_pixels;
  size_t replay_pixels_capacity;
  float *stroke_mask;
  size_t stroke_mask_capacity;
  dt_drawlayer_paint_stroke_t *stroke;
} drawlayer_fullres_replay_scratch_t;

static void _destroy_fullres_replay_scratch(gpointer data)
{
  drawlayer_fullres_replay_scratch_t *scratch = (drawlayer_fullres_replay_scratch_t *)data;
  if(!scratch) return;
  dt_free_align(scratch->replay_pixels);
  dt_free_align(scratch->stroke_mask);
  if(scratch->stroke) dt_drawlayer_paint_runtime_private_destroy(&scratch->stroke);
  dt_free(scratch);
}

static GPrivate _drawlayer_fullres_replay_scratch_key = G_PRIVATE_INIT(_destroy_fullres_replay_scratch);

static drawlayer_fullres_replay_scratch_t *_get_fullres_replay_scratch(void)
{
  drawlayer_fullres_replay_scratch_t *scratch
      = (drawlayer_fullres_replay_scratch_t *)g_private_get(&_drawlayer_fullres_replay_scratch_key);
  if(scratch) return scratch;

  scratch = g_malloc0(sizeof(*scratch));
  if(!scratch) return NULL;

  scratch->stroke = dt_drawlayer_paint_runtime_private_create();
  if(!scratch->stroke)
  {
    dt_free(scratch);
    return NULL;
  }
  scratch->stroke->dab_window = g_array_new(FALSE, FALSE, sizeof(dt_drawlayer_brush_dab_t));
  if(!scratch->stroke->dab_window)
  {
    dt_drawlayer_paint_runtime_private_destroy(&scratch->stroke);
    dt_free(scratch);
    return NULL;
  }

  g_private_set(&_drawlayer_fullres_replay_scratch_key, scratch);
  return scratch;
}

static float *_ensure_fullres_replay_float_buffer(float **buffer, size_t *capacity_values, const size_t needed_values)
{
  if(!buffer || !capacity_values || needed_values == 0) return NULL;
  if(*capacity_values < needed_values)
  {
    dt_free_align(*buffer);
    *buffer = dt_alloc_align(needed_values * sizeof(float));
    if(!*buffer)
    {
      *capacity_values = 0;
      return NULL;
    }
    *capacity_values = needed_values;
  }
  return *buffer;
}

static gboolean _replay_finished_stroke_to_base_patch(dt_iop_module_t *self, const GArray *history,
                                                      const float distance_percent)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !history || history->len == 0) return FALSE;
  if(!g->base_patch.pixels || g->base_patch.width <= 0 || g->base_patch.height <= 0) return FALSE;

  dt_drawlayer_damaged_rect_t replay_bounds = { 0 };
  for(guint i = 0; i < history->len; i++)
  {
    const dt_drawlayer_brush_dab_t *dab = &g_array_index(history, dt_drawlayer_brush_dab_t, i);
    const int x0 = CLAMP((int)floorf(dab->x - dab->radius - 1.0f), 0, g->base_patch.width - 1);
    const int y0 = CLAMP((int)floorf(dab->y - dab->radius - 1.0f), 0, g->base_patch.height - 1);
    const int x1 = CLAMP((int)ceilf(dab->x + dab->radius + 1.0f), 0, g->base_patch.width - 1);
    const int y1 = CLAMP((int)ceilf(dab->y + dab->radius + 1.0f), 0, g->base_patch.height - 1);
    const dt_drawlayer_damaged_rect_t dab_bounds = {
      .valid = TRUE,
      .nw = { x0, y0 },
      .se = { x1, y1 },
    };
    dt_drawlayer_paint_runtime_note_dab_damage(&replay_bounds, &dab_bounds);
  }
  if(!replay_bounds.valid) return FALSE;

  const int replay_width = replay_bounds.se[0] - replay_bounds.nw[0] + 1;
  const int replay_height = replay_bounds.se[1] - replay_bounds.nw[1] + 1;
  if(replay_width <= 0 || replay_height <= 0) return FALSE;

  drawlayer_fullres_replay_scratch_t *scratch = _get_fullres_replay_scratch();
  if(!scratch || !scratch->stroke) return FALSE;

  dt_drawlayer_paint_stroke_t *stroke = scratch->stroke;
  stroke->distance_percent = distance_percent;
  dt_drawlayer_paint_runtime_private_reset(stroke);

  float *replay_pixels = _ensure_fullres_replay_float_buffer(&scratch->replay_pixels,
                                                             &scratch->replay_pixels_capacity,
                                                             (size_t)replay_width * replay_height * 4);
  if(!replay_pixels) return FALSE;

  float *stroke_mask = _ensure_fullres_replay_float_buffer(&scratch->stroke_mask,
                                                           &scratch->stroke_mask_capacity,
                                                           (size_t)replay_width * replay_height);
  if(!stroke_mask) return FALSE;
  memset(stroke_mask, 0, (size_t)replay_width * replay_height * sizeof(float));
  dt_drawlayer_damaged_rect_t replay_damage = { 0 };

  dt_drawlayer_cache_patch_rdlock(&g->base_patch);
  for(int yy = 0; yy < replay_height; yy++)
  {
    const float *src = g->base_patch.pixels
                       + ((size_t)(replay_bounds.nw[1] + yy) * g->base_patch.width + replay_bounds.nw[0]) * 4;
    float *dst = replay_pixels + (size_t)yy * replay_width * 4;
    memcpy(dst, src, (size_t)replay_width * 4 * sizeof(float));
  }
  dt_drawlayer_cache_patch_rdunlock(&g->base_patch);

  gboolean wrote_replay_tile = FALSE;
  for(guint i = 0; i < history->len; i++)
  {
    const dt_drawlayer_brush_dab_t *dab = &g_array_index(history, dt_drawlayer_brush_dab_t, i);
    wrote_replay_tile |= dt_drawlayer_paint_rasterize_segment_to_buffer(
      dab,
      _clamp01(stroke->distance_percent),
      replay_pixels,
      replay_width,
      replay_height,
      replay_bounds.nw[0], replay_bounds.nw[1], 1.0f,
      stroke_mask,
      replay_width,
      replay_height,
      &replay_damage,
      stroke);
    dt_iop_nap(5000);
  }
  if(wrote_replay_tile)
  {
    dt_drawlayer_cache_patch_wrlock(&g->base_patch);
    for(int yy = 0; yy < replay_height; yy++)
    {
      const float *src = replay_pixels + (size_t)yy * replay_width * 4;
      float *dst = g->base_patch.pixels
                   + ((size_t)(replay_bounds.nw[1] + yy) * g->base_patch.width + replay_bounds.nw[0]) * 4;
      memcpy(dst, src, (size_t)replay_width * 4 * sizeof(float));
    }
#ifdef HAVE_OPENCL
    dt_dev_pixelpipe_cache_flush_host_pinned_image(darktable.pixelpipe_cache, g->base_patch.pixels,
                                                   g->base_patch.cache_entry, -1);
#endif
    dt_drawlayer_cache_patch_wrunlock(&g->base_patch);
  }
  dt_iop_nap(200000);
  return wrote_replay_tile;
}

static void _process_backend_dab(dt_iop_module_t *self, const dt_drawlayer_brush_dab_t *dab,
                                 drawlayer_paint_backend_ctx_t *ctx)
{
  dt_drawlayer_paint_stroke_t *stroke = ctx ? ctx->stroke : NULL;
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  if(!g || !stroke || !stroke->dab_window || !dab) return;

  /* Realtime hot path stamps only the current-view process tile.
   * The full-resolution authoritative `base_patch` is replayed once from the
   * preserved dab history when the stroke commits. */
  gboolean have_process_damage = FALSE;
  dt_drawlayer_damaged_rect_t process_step_path = { 0 };
  dt_drawlayer_damaged_rect_t process_step_damage = { 0 };
  dt_drawlayer_paint_runtime_state_reset(&process_step_path);

  dt_pthread_mutex_lock(&g->process_patch_mutex);
  dt_drawlayer_cache_patch_wrlock(&g->process_patch);
  if(g->process_patch_valid && g->process_patch.pixels && g->process_patch.width > 0 && g->process_patch.height > 0
     && g->process_combined_roi.scale > 1e-6f)
  {
    dt_drawlayer_paint_rasterize_segment_to_buffer(
      dab,
      _clamp01(stroke->distance_percent),
      g->process_patch.pixels,
      g->process_patch.width,
      g->process_patch.height,
      g->process_combined_roi.x,
      g->process_combined_roi.y,
      g->process_combined_roi.scale,
      g->process_stroke_mask.pixels,
      g->process_stroke_mask.width,
      g->process_stroke_mask.height,
      &process_step_path,
      stroke);
    have_process_damage = dt_drawlayer_paint_runtime_get_stroke_damage(&process_step_path, &process_step_damage);
    if(have_process_damage)
    {
      g->process_patch_dirty = TRUE;
      dt_drawlayer_paint_runtime_note_dab_damage(&g->process_dirty_rect, &process_step_damage);
      _publish_process_patch_locked(g, &process_step_damage, FALSE);
    }
  }
  dt_drawlayer_cache_patch_wrunlock(&g->process_patch);
  dt_pthread_mutex_unlock(&g->process_patch_mutex);

  if(have_process_damage)
  {
    g->cache_dirty = TRUE;

    g->last_dab_valid = TRUE;
    g->last_dab_x = dab->x;
    g->last_dab_y = dab->y;
    if(ctx)
    {
      ctx->have_pending_publish = TRUE;
      ctx->last_dab_x = dab->x;
      ctx->last_dab_y = dab->y;
    }
  }

}

static gboolean _layer_bounds_to_widget_bounds(dt_iop_module_t *self, const float x0, const float y0,
                                               const float x1, const float y1,
                                               float *left, float *top, float *right, float *bottom)
{
  if(!self || !self->dev || !self->dev->virtual_pipe) return FALSE;

  float pts[8] = {
    x0, y0,
    x1, y0,
    x0, y1,
    x1, y1,
  };

  if(!dt_dev_distort_transform_plus(self->dev, self->dev->virtual_pipe, self->iop_order,
                                    DT_DEV_TRANSFORM_DIR_FORW_EXCL, pts, 4))
    return FALSE;

  dt_dev_coordinates_image_abs_to_image_norm(self->dev, pts, 4);
  dt_dev_coordinates_image_norm_to_widget(self->dev, pts, 4);

  float min_x = pts[0];
  float max_x = pts[0];
  float min_y = pts[1];
  float max_y = pts[1];
  for(int i = 1; i < 4; i++)
  {
    min_x = fminf(min_x, pts[2 * i]);
    max_x = fmaxf(max_x, pts[2 * i]);
    min_y = fminf(min_y, pts[2 * i + 1]);
    max_y = fmaxf(max_y, pts[2 * i + 1]);
  }

  if(left) *left = min_x;
  if(top) *top = min_y;
  if(right) *right = max_x;
  if(bottom) *bottom = max_y;
  return TRUE;
}

static void _paint_temp_buffer(dt_iop_module_t *self, cairo_t *cr, const int width, const int height)
{
  (void)self;
  (void)cr;
  (void)width;
  (void)height;
}


static void _default_layer_name(dt_iop_module_t *self, char *name, const size_t name_size)
{
  const char *suffix = "0";
  if(self && self->multi_name[0] != '\0') suffix = self->multi_name;
  else if(self)
  {
    static char fallback[16];
    g_snprintf(fallback, sizeof(fallback), "%d", MAX(self->multi_priority, 0) + 1);
    suffix = fallback;
  }
  g_snprintf(name, name_size, "layer %s", suffix);
}

static gboolean _layer_name_non_empty(const char *name)
{
  if(!name) return FALSE;
  char tmp[DRAWLAYER_NAME_SIZE] = { 0 };
  g_strlcpy(tmp, name, sizeof(tmp));
  g_strstrip(tmp);
  return tmp[0] != '\0';
}

static void _work_profile_key_from_info(const dt_iop_order_iccprofile_info_t *info, char *key, const size_t key_size)
{
  if(!key || key_size == 0) return;
  key[0] = '\0';
  if(!info) return;

  g_snprintf(key, key_size, "%d|%d|%s", (int)info->type, (int)info->intent, info->filename);
}

static gboolean _get_current_work_profile_key(dt_iop_module_t *self, GList *iop_list, dt_dev_pixelpipe_t *pipe,
                                              char *key, const size_t key_size)
{
  if(!key || key_size == 0) return FALSE;
  key[0] = '\0';
  if(!self || !pipe || !iop_list) return FALSE;

  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_iop_work_profile_info(self, iop_list);
  if(!work_profile) return FALSE;

  _work_profile_key_from_info(work_profile, key, key_size);
  return key[0] != '\0';
}

static void _ensure_layer_name(dt_iop_module_t *self, dt_iop_drawlayer_params_t *params)
{
  if(params->layer_name[0] != '\0') return;
  _default_layer_name(self, params->layer_name, sizeof(params->layer_name));
  params->layer_order = -1;
}

typedef struct drawlayer_process_scratch_t
{
  float *layerbuf;
  size_t layerbuf_pixels;
  float *cl_background_rgba;
  size_t cl_background_rgba_pixels;
  float *flush_update_rgba;
  size_t flush_update_rgba_pixels;
} drawlayer_process_scratch_t;

static void _destroy_process_scratch(gpointer data)
{
  drawlayer_process_scratch_t *scratch = (drawlayer_process_scratch_t *)data;
  if(!scratch) return;
  dt_drawlayer_cache_free_temp_buffer((void **)&scratch->layerbuf, "drawlayer process scratch");
  dt_drawlayer_cache_free_temp_buffer((void **)&scratch->cl_background_rgba, "drawlayer process scratch");
  dt_drawlayer_cache_free_temp_buffer((void **)&scratch->flush_update_rgba, "drawlayer process update scratch");
  dt_free(scratch);
}

static GPrivate _drawlayer_process_scratch_key = G_PRIVATE_INIT(_destroy_process_scratch);

static drawlayer_process_scratch_t *_get_process_scratch(void)
{
  drawlayer_process_scratch_t *scratch = (drawlayer_process_scratch_t *)g_private_get(&_drawlayer_process_scratch_key);
  if(scratch) return scratch;

  scratch = g_malloc0(sizeof(*scratch));
  if(!scratch) return NULL;
  g_private_set(&_drawlayer_process_scratch_key, scratch);
  return scratch;
}

static uint64_t _drawlayer_sidecar_cache_hash(const char *path, const char *layer_name, const char *purpose)
{
  uint64_t hash = 5381u;
  if(path) hash = dt_hash(hash, path, strlen(path));
  if(layer_name) hash = dt_hash(hash, layer_name, strlen(layer_name));
  if(purpose) hash = dt_hash(hash, purpose, strlen(purpose));
  return hash ? hash : 1u;
}

static uint64_t _drawlayer_params_cache_hash(const int32_t imgid, const dt_iop_drawlayer_params_t *params)
{
  /* Internal drawlayer base-cache identity must stay stable across transient
   * stroke/hash updates. Key only by image + layer identity + working profile,
   * not by volatile fields (stroke hash, sidecar timestamp...).
   *
   * This keeps the shared base patch line hot across interactive drawing ticks
   * and avoids expensive rekey conflicts/republishing during realtime updates. */
  uint64_t hash = 5381u;
  hash = dt_hash(hash, (const char *)&imgid, sizeof(imgid));
  if(params)
  {
    hash = dt_hash(hash, params->layer_name, sizeof(params->layer_name));
    hash = dt_hash(hash, (const char *)&params->layer_order, sizeof(params->layer_order));
    hash = dt_hash(hash, params->work_profile, sizeof(params->work_profile));
  }
  return hash ? hash : 1u;
}

static uint64_t _drawlayer_process_cache_hash(const dt_dev_pixelpipe_iop_t *piece,
                                              const dt_iop_roi_t *roi_in,
                                              const dt_iop_roi_t *roi_out)
{
  /* This hash identifies only the affine geometry of the transformed process
   * tile:
   * - current pipeline full-canvas size,
   * - propagated module input/output rectangles,
   * - current tile ROI.
   *
   * Content changes are intentionally *not* part of this key. The transformed
   * tile is updated incrementally by the backend worker as the stroke evolves,
   * so changing brush content should not force a full-tile resample. */
  uint64_t hash = 5381u;
  if(piece)
  {
    hash = dt_hash(hash, (const char *)&piece->pipe->image.id, sizeof(piece->pipe->image.id));
    hash = dt_hash(hash, (const char *)&piece->pipe->iwidth, sizeof(piece->pipe->iwidth));
    hash = dt_hash(hash, (const char *)&piece->pipe->iheight, sizeof(piece->pipe->iheight));
    hash = dt_hash(hash, (const char *)&piece->buf_in, sizeof(piece->buf_in));
    hash = dt_hash(hash, (const char *)&piece->buf_out, sizeof(piece->buf_out));
  }

  if(roi_in) hash = dt_hash(hash, (const char *)roi_in, sizeof(*roi_in));
  if(roi_out) hash = dt_hash(hash, (const char *)roi_out, sizeof(*roi_out));
  return hash ? hash : 1u;
}

static void _touch_layer_cache_epoch(dt_iop_drawlayer_gui_data_t *g)
{
  if(!g) return;
  g->cache_epoch++;
  if(g->cache_epoch == 0) g->cache_epoch++;
}

static void _clear_patch(drawlayer_patch_t *patch)
{
  dt_drawlayer_cache_patch_clear(patch, "drawlayer patch");
}

static gboolean _rekey_shared_base_patch(drawlayer_patch_t *patch, const int32_t imgid,
                                         const dt_iop_drawlayer_params_t *params)
{
  /* Rekeying lets the same pixelpipe cache line keep its allocated storage and
   * current in-memory pixels while the serialized module hash advances to a new
   * history snapshot. This is the central piece that lets other pipelines find
   * the newest base patch through the cache instead of through GUI internals. */
  if(!patch || !patch->cache_entry || !params) return FALSE;
  const uint64_t new_hash = _drawlayer_params_cache_hash(imgid, params);
  if(new_hash == patch->cache_hash) return TRUE;
  if(dt_dev_pixelpipe_cache_rekey(darktable.pixelpipe_cache, patch->cache_hash, new_hash, patch->cache_entry) == 0)
  {
    patch->cache_hash = new_hash;
    return TRUE;
  }

  /* Fallback path: another cache line already owns `new_hash` (or rekeying
   * failed for any other reason). Publish the current authoritative pixels into
   * that target key explicitly so parallel/headless pipelines resolving by the
   * latest params hash still see up-to-date content.
   *
   * We intentionally keep `patch` bound to its original cache entry/hash here,
   * because this module may hold additional explicit refs on that entry
   * (`base_patch_loaded_ref`, stroke refs). Rebinding `patch` would desynchronize
   * those ref counters. */
  if(!patch->pixels || patch->width <= 0 || patch->height <= 0) return FALSE;

  drawlayer_patch_t published = { 0 };
  int created = 0;
  if(!dt_drawlayer_cache_patch_alloc_shared(&published, new_hash, (size_t)patch->width * patch->height,
                                            patch->width, patch->height, "drawlayer sidecar cache", &created))
    return FALSE;

  dt_drawlayer_cache_patch_rdlock(patch);
  memcpy(published.pixels, patch->pixels, (size_t)patch->width * patch->height * 4 * sizeof(float));
  dt_drawlayer_cache_patch_rdunlock(patch);
#ifdef HAVE_OPENCL
  dt_dev_pixelpipe_cache_flush_host_pinned_image(darktable.pixelpipe_cache, published.pixels, published.cache_entry, -1);
#endif
  _clear_patch(&published);
  dt_print(DT_DEBUG_PERF,
           "[drawlayer] cache rekey conflict old=%" PRIu64 " new=%" PRIu64 " -> published snapshot instead\n",
           patch->cache_hash, new_hash);
  return TRUE;
}

static void _retain_base_patch_loaded_ref(dt_iop_drawlayer_gui_data_t *g)
{
  if(!g || !g->base_patch.cache_entry || g->base_patch_loaded_ref) return;
  dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, g->base_patch.cache_hash, TRUE,
                                         g->base_patch.cache_entry);
  g->base_patch_loaded_ref = TRUE;
}

static void _retain_base_patch_stroke_ref(dt_iop_drawlayer_gui_data_t *g)
{
  if(!g || !g->base_patch.cache_entry) return;
  dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, g->base_patch.cache_hash, TRUE,
                                         g->base_patch.cache_entry);
  g->base_patch_stroke_refs++;
}

static void _release_all_base_patch_extra_refs(dt_iop_drawlayer_gui_data_t *g)
{
  if(!g) return;
  if(!g->base_patch.cache_entry)
  {
    /* Keep refcount bookkeeping state coherent even if the cache entry has
     * already been detached/cleared. This prevents stale counters from being
     * applied to a future reused `g->base_patch` entry. */
    g->base_patch_loaded_ref = FALSE;
    g->base_patch_stroke_refs = 0;
    return;
  }

  if(g->base_patch_loaded_ref)
  {
    dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, g->base_patch.cache_hash, FALSE,
                                           g->base_patch.cache_entry);
    g->base_patch_loaded_ref = FALSE;
  }

  while(g->base_patch_stroke_refs > 0)
  {
    dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, g->base_patch.cache_hash, FALSE,
                                           g->base_patch.cache_entry);
    g->base_patch_stroke_refs--;
  }
}

static gboolean _acquire_shared_base_patch(drawlayer_patch_t *patch, const int32_t imgid,
                                           const dt_iop_drawlayer_params_t *params, const int width,
                                           const int height, const char *name, int *created_out)
{
  /* All authoritative base-patch users (GUI and headless pipes) attach through
   * one shared pixelpipe cache line keyed by stable layer identity.
   * This helper hides whether the caller hit a hot cache line from another pipe
   * or had to instantiate a new one that still needs to be initialized. */
  if(!params) return FALSE;
  const uint64_t hash = _drawlayer_params_cache_hash(imgid, params);
  return dt_drawlayer_cache_patch_alloc_shared(patch, hash, (size_t)width * height, width, height, name, created_out);
}

static inline void _to_io_patch(const drawlayer_patch_t *src, dt_drawlayer_io_patch_t *dst)
{
  if(!dst) return;
  if(!src)
  {
    memset(dst, 0, sizeof(*dst));
    return;
  }
  dst->x = src->x;
  dst->y = src->y;
  dst->width = src->width;
  dst->height = src->height;
  dst->pixels = src->pixels;
}

static inline void _from_io_info(const dt_drawlayer_io_layer_info_t *src, drawlayer_dir_info_t *dst)
{
  if(!dst) return;
  if(!src)
  {
    memset(dst, 0, sizeof(*dst));
    dst->index = -1;
    return;
  }
  dst->found = src->found;
  dst->index = src->index;
  dst->count = src->count;
  dst->width = src->width;
  dst->height = src->height;
  g_strlcpy(dst->name, src->name, sizeof(dst->name));
  g_strlcpy(dst->work_profile, src->work_profile, sizeof(dst->work_profile));
}

static void _clear_headless_cache(dt_iop_drawlayer_data_t *data)
{
  if(!data) return;
  _clear_patch(&data->headless_base_patch);
  data->headless_cache_valid = FALSE;
  data->headless_cache_imgid = -1;
  data->headless_cache_layer_name[0] = '\0';
  data->headless_cache_layer_order = -1;
}

static gboolean _refresh_headless_cache(dt_iop_module_t *self, dt_iop_drawlayer_data_t *data,
                                        const dt_iop_drawlayer_params_t *params, dt_dev_pixelpipe_iop_t *piece)
{
  if(!self || !data || !params || !piece) return FALSE;
  if(params->layer_name[0] == '\0')
  {
    _clear_headless_cache(data);
    return TRUE;
  }

  const uint64_t base_hash = _drawlayer_params_cache_hash(piece->pipe->image.id, params);
  const int known_width = data->headless_base_patch.width;
  const int known_height = data->headless_base_patch.height;
  if(data->headless_cache_valid && data->headless_base_patch.cache_entry
     && data->headless_base_patch.cache_hash == base_hash
     && data->headless_cache_imgid == piece->pipe->image.id
     && !g_strcmp0(data->headless_cache_layer_name, params->layer_name))
    return TRUE;

  _clear_headless_cache(data);

  if(known_width > 0 && known_height > 0)
  {
    int created = 0;
    if(!_acquire_shared_base_patch(&data->headless_base_patch, piece->pipe->image.id, params,
                                   known_width, known_height, "drawlayer headless sidecar cache", &created))
      return FALSE;

    if(!created)
    {
      data->headless_cache_valid = TRUE;
      data->headless_cache_imgid = piece->pipe->image.id;
      g_strlcpy(data->headless_cache_layer_name, params->layer_name, sizeof(data->headless_cache_layer_name));
      data->headless_cache_layer_order = params->layer_order;
      return TRUE;
    }

    char warm_path[PATH_MAX] = { 0 };
    gboolean warm_loaded = FALSE;
    if(dt_drawlayer_io_sidecar_path(piece->pipe->image.id, warm_path, sizeof(warm_path))
       && g_file_test(warm_path, G_FILE_TEST_EXISTS))
    {
      dt_drawlayer_io_patch_t warm_patch = { 0 };
      dt_drawlayer_cache_patch_wrlock(&data->headless_base_patch);
      _to_io_patch(&data->headless_base_patch, &warm_patch);
      warm_loaded = dt_drawlayer_io_load_layer(warm_path, params->layer_name, params->layer_order,
                                               known_width, known_height, &warm_patch);
      dt_drawlayer_cache_patch_wrunlock(&data->headless_base_patch);
    }

    if(warm_loaded)
    {
      data->headless_cache_valid = TRUE;
      data->headless_cache_imgid = piece->pipe->image.id;
      g_strlcpy(data->headless_cache_layer_name, params->layer_name, sizeof(data->headless_cache_layer_name));
      data->headless_cache_layer_order = params->layer_order;
      return TRUE;
    }

    _clear_headless_cache(data);
  }

  char path[PATH_MAX] = { 0 };
  if(!dt_drawlayer_io_sidecar_path(piece->pipe->image.id, path, sizeof(path))) return FALSE;
  if(!g_file_test(path, G_FILE_TEST_EXISTS)) return FALSE;

  dt_drawlayer_io_layer_info_t io_info = { 0 };
  drawlayer_dir_info_t info = { 0 };
  if(!dt_drawlayer_io_find_layer(path, params->layer_name, params->layer_order, &io_info))
    return FALSE;
  _from_io_info(&io_info, &info);
  if(info.width == 0 || info.height == 0)
    return FALSE;

  int created = 0;
  if(!_acquire_shared_base_patch(&data->headless_base_patch, piece->pipe->image.id, params,
                                 (int)info.width, (int)info.height, "drawlayer headless sidecar cache",
                                 &created))
    return FALSE;

  if(created)
  {
    dt_drawlayer_io_patch_t io_patch = { 0 };
    dt_drawlayer_cache_patch_wrlock(&data->headless_base_patch);
    dt_drawlayer_cache_clear_transparent_float(data->headless_base_patch.pixels, (size_t)info.width * info.height);
    _to_io_patch(&data->headless_base_patch, &io_patch);
    const gboolean loaded = dt_drawlayer_io_load_layer(path, params->layer_name, info.index,
                                                       (int)info.width, (int)info.height, &io_patch);
    dt_drawlayer_cache_patch_wrunlock(&data->headless_base_patch);
    if(!loaded)
    {
      _clear_headless_cache(data);
      return FALSE;
    }
  }

  data->headless_cache_valid = TRUE;
  data->headless_cache_imgid = piece->pipe->image.id;
  g_strlcpy(data->headless_cache_layer_name, params->layer_name, sizeof(data->headless_cache_layer_name));
  data->headless_cache_layer_order = info.index;
  return TRUE;
}

static gboolean _build_process_patch_from_base(dt_iop_module_t *self,
                                               dt_iop_drawlayer_gui_data_t *g,
                                               const dt_dev_pixelpipe_iop_t *piece,
                                               const dt_iop_roi_t *roi_in,
                                               const dt_iop_roi_t *roi_out)
{
  const gint64 t0 = g_get_monotonic_time();
  gint64 t = t0;
  /* Materialize (or refresh) a display-sized process tile from the raw-sized
   * authoritative cache. This keeps `process()`/`process_cl()` on a direct
   * blend path during live drawing: backend dabs update this tile in place and
   * pipeline blending no longer has to resample the full raw patch every run.
   *
   * When geometry changes invalidate this tile, `_flush_process_patch_to_base()`
   * upsamples it back into `base_patch` first so no in-flight stroke edit is
   * lost before rebuilding the tile for the new ROI. */
  if(!self || !g || !piece || !roi_out || !g->base_patch.pixels) return FALSE;
  const int current_full_w = piece->pipe->iwidth;
  const int current_full_h = piece->pipe->iheight;
  if(current_full_w <= 0 || current_full_h <= 0) return FALSE;

  const dt_iop_roi_t process_roi = roi_in ? *roi_in : *roi_out;
  dt_iop_roi_t combined_roi = { 0 };
  dt_drawlayer_cache_build_combined_process_roi_for_piece(piece, &process_roi, current_full_w, current_full_h,
                                                          g->base_patch.width, g->base_patch.height, &combined_roi);
  {
    const gint64 now = g_get_monotonic_time();
    dt_print(DT_DEBUG_PERF, "[drawlayer] process step=build-process-roi ms=%.3f",
             (now - t) / 1000.0);
    t = now;
  }

  if(combined_roi.scale <= 1e-6f
     || roi_out->width <= 0 || roi_out->height <= 0) return FALSE;

  /* Keep one brush-radius margin around the visible process ROI so dabs near
   * view edges are not clipped in realtime preview/process rendering. */
  const float brush_radius_src = fmaxf(_conf_size(), 0.5f);
  const int process_pad = MAX(0, (int)ceilf(brush_radius_src * combined_roi.scale));
  dt_iop_roi_t padded_roi = combined_roi;
  padded_roi.x -= process_pad;
  padded_roi.y -= process_pad;
  padded_roi.width += 2 * process_pad;
  padded_roi.height += 2 * process_pad;
  const int patch_width = roi_out->width + 2 * process_pad;
  const int patch_height = roi_out->height + 2 * process_pad;

  gboolean have_process_patch = FALSE;
  gboolean same_geometry = FALSE;
  gboolean keep_live_geometry = FALSE;
  gboolean need_flush = FALSE;
  dt_iop_roi_t previous_process_roi = { 0 };
  int previous_process_width = 0;
  int previous_process_height = 0;
  dt_pthread_mutex_lock(&g->process_patch_mutex);
  have_process_patch = (g->process_patch_valid && g->process_patch.pixels);
  if(have_process_patch)
  {
    previous_process_roi = g->process_combined_roi;
    previous_process_width = g->process_patch.width;
    previous_process_height = g->process_patch.height;
    same_geometry = (g->process_patch_padding == process_pad
                     && g->process_patch.width == patch_width
                     && g->process_patch.height == patch_height
                     && g->process_read_patch.pixels
                     && g->process_read_patch.width == patch_width
                     && g->process_read_patch.height == patch_height
                     && abs(g->process_combined_roi.x - padded_roi.x) <= 2
                     && abs(g->process_combined_roi.y - padded_roi.y) <= 2
                     && g->process_combined_roi.width == padded_roi.width
                     && g->process_combined_roi.height == padded_roi.height
                     && fabs(g->process_combined_roi.scale - combined_roi.scale) <= 1e-2);
    if(!same_geometry && (g->painting || dt_drawlayer_worker_any_active(g->rt)))
      keep_live_geometry = (g->process_patch_padding == process_pad
                            && g->process_patch.width == patch_width
                            && g->process_patch.height == patch_height
                            && g->process_read_patch.pixels);
    need_flush = g->process_patch_dirty;
  }
  dt_pthread_mutex_unlock(&g->process_patch_mutex);
  if(same_geometry)
  {
    const gint64 now = g_get_monotonic_time();
    dt_print(DT_DEBUG_PERF, "[drawlayer] process step=process-patch-hit ms=%.3f total=%.3f",
             (now - t) / 1000.0, (now - t0) / 1000.0);
    return TRUE;
  }

  /* During active drawing, keep serving the current display-sized process tile
   * even if upstream ROI metadata jitters slightly between recomputes.
   * Rebuilding this tile costs hundreds of ms and kills interaction latency. */
  if(keep_live_geometry)
  {
    const gint64 now = g_get_monotonic_time();
    dt_print(DT_DEBUG_PERF,
             "[drawlayer] process step=process-patch-keep-live ms=%.3f total=%.3f painting=%d workers=%d",
             (now - t) / 1000.0, (now - t0) / 1000.0, g->painting ? 1 : 0, dt_drawlayer_worker_any_active(g->rt) ? 1 : 0);
    return TRUE;
  }

  if(have_process_patch)
  {
    dt_print(DT_DEBUG_PERF,
             "[drawlayer] process step=process-patch-miss old=(x=%d y=%d w=%d h=%d s=%.6f pw=%d ph=%d) "
             "new=(x=%d y=%d w=%d h=%d s=%.6f pw=%d ph=%d)\n",
             previous_process_roi.x, previous_process_roi.y, previous_process_roi.width,
             previous_process_roi.height, previous_process_roi.scale,
             previous_process_width, previous_process_height,
             padded_roi.x, padded_roi.y, padded_roi.width, padded_roi.height, padded_roi.scale,
             patch_width, patch_height);
  }

  if(need_flush)
  {
    /* Geometry changed: fold incremental process-tile edits back into base
     * only if this transformed tile actually diverged from base content. */
    _flush_process_patch_to_base(self, g);
    _rekey_shared_base_patch(&g->base_patch, self->dev->image_storage.id,
                             (const dt_iop_drawlayer_params_t *)self->params);
    {
      const gint64 now = g_get_monotonic_time();
      dt_print(DT_DEBUG_PERF, "[drawlayer] process step=flush-old-process-patch ms=%.3f",
               (now - t) / 1000.0);
      t = now;
    }
  }

  gboolean populated = FALSE;
  dt_pthread_mutex_lock(&g->process_patch_mutex);
  populated = dt_drawlayer_cache_populate_process_patch_from_base(&g->base_patch, &g->stroke_mask,
                                                                  &g->process_patch,
                                                                  &g->process_stroke_mask,
                                                                  &padded_roi, process_pad,
                                                                  patch_width, patch_height,
                                                                  &g->process_patch_valid,
                                                                  &g->process_patch_dirty,
                                                                  &g->process_dirty_rect,
                                                                  &g->process_patch_padding,
                                                                  &g->process_combined_roi,
                                                                  "drawlayer process tile",
                                                                  "drawlayer process stroke mask");
  if(populated)
  {
    _publish_process_patch_locked(g, NULL, TRUE);
    g->process_geom_hash = _drawlayer_process_cache_hash(piece, roi_in, roi_out);
    dt_drawlayer_paint_runtime_state_reset(g->process_path);
  }
  dt_pthread_mutex_unlock(&g->process_patch_mutex);
  if(!populated) return FALSE;
  {
    const gint64 now = g_get_monotonic_time();
    dt_print(DT_DEBUG_PERF, "[drawlayer] process step=build-process-patch-clipzoom ms=%.3f",
             (now - t) / 1000.0);
    t = now;
  }
  {
    const gint64 now = g_get_monotonic_time();
    dt_print(DT_DEBUG_PERF, "[drawlayer] process step=build-process-patch-finalize ms=%.3f total=%.3f",
             (now - t) / 1000.0, (now - t0) / 1000.0);
  }
  return TRUE;
}

static void _flush_process_patch_to_base_locked(dt_iop_module_t *self, dt_iop_drawlayer_gui_data_t *g)
{
  /* `process_patch` lives in current process/output coordinates.
   * Before invalidating it (ROI change, module unfocus, explicit save), rescale
   * it back to source/layer coordinates and fold it into the authoritative
   * `base_patch`. */
  if(!g) return;
  drawlayer_process_scratch_t *scratch = _get_process_scratch();
  if(!scratch) return;
  const gboolean was_dirty = g->process_patch_dirty;
  if(!dt_drawlayer_cache_flush_process_patch_to_base(&g->base_patch, &g->stroke_mask,
                                                     &g->process_combined_roi, &g->process_patch,
                                                     &g->process_stroke_mask,
                                                     &scratch->flush_update_rgba,
                                                     &scratch->flush_update_rgba_pixels,
                                                     &g->cache_dirty, &g->process_patch_dirty,
                                                     &g->process_dirty_rect,
                                                     "drawlayer process update tile"))
    return;

  if(was_dirty && !g->process_patch_dirty && self && self->params && self->dev)
    _rekey_shared_base_patch(&g->base_patch, self->dev->image_storage.id,
                             (const dt_iop_drawlayer_params_t *)self->params);
}

static void _flush_process_patch_to_base(dt_iop_module_t *self, dt_iop_drawlayer_gui_data_t *g)
{
  if(!g) return;
  if(!g->painting && dt_drawlayer_worker_any_active(g->rt))
    dt_drawlayer_worker_flush_finished_strokes(g->rt);
  dt_pthread_mutex_lock(&g->process_patch_mutex);
  _flush_process_patch_to_base_locked(self, g);
  dt_pthread_mutex_unlock(&g->process_patch_mutex);
}

static void _copy_input_to_output(const float *input, float *output, const int width, const int height)
{
  if(!input || !output || width <= 0 || height <= 0) return;
  dt_iop_image_copy_by_size(output, input, width, height, 4);
}

static void _blend_layer_over_input(float *output, const float *input, const float *layerbuf, const size_t pixels,
                                    const gboolean use_preview_bg, const float preview_bg)
{
  if(!output || !input || !layerbuf || pixels == 0) return;

  const dt_aligned_pixel_simd_t preview_base = { preview_bg, preview_bg, preview_bg, 1.0f };

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) \
  dt_omp_firstprivate(input, output, layerbuf, pixels, use_preview_bg, preview_base)
#endif
  for(size_t kk = 0; kk < pixels; kk++)
  {
    const float *base = input + 4 * kk;
    const float *layer = layerbuf + 4 * kk;
    float *pixel = output + 4 * kk;
    const float src_alpha = _clamp01(layer[3]);
    if(src_alpha > 1e-8f)
    {
      const dt_aligned_pixel_simd_t base_v = use_preview_bg ? preview_base : dt_load_simd_aligned(base);
      dt_aligned_pixel_simd_t src_v = dt_load_simd_aligned(layer);
      const float inv_alpha = 1.0f - src_alpha;
      const dt_aligned_pixel_simd_t inv_alpha_v = { inv_alpha, inv_alpha, inv_alpha, inv_alpha };
      src_v[3] = src_alpha;
      dt_store_simd_aligned(pixel, src_v + base_v * inv_alpha_v);
    }
    else
    {
      const dt_aligned_pixel_simd_t base_v = use_preview_bg ? preview_base : dt_load_simd_aligned(base);
      dt_store_simd_aligned(pixel, base_v);
    }
  }
}

#ifdef HAVE_OPENCL
typedef struct drawlayer_cl_image_handle_t
{
  cl_mem mem;
  gboolean is_pinned;
  gboolean is_cached_device;
} drawlayer_cl_image_handle_t;

static gboolean _drawlayer_sync_host_image_to_device(const int devid, cl_mem device_image,
                                                     void *host_pixels, const int width, const int height,
                                                     const int bpp)
{
  if(!device_image || !host_pixels || width <= 0 || height <= 0 || bpp <= 0) return FALSE;

  const cl_mem_flags flags = dt_opencl_get_mem_flags(device_image);
  if(flags & CL_MEM_USE_HOST_PTR)
  {
    void *mapped = dt_opencl_map_image(devid, device_image, TRUE, CL_MAP_WRITE, width, height, bpp);
    if(mapped)
    {
      if(mapped != host_pixels)
        memcpy(mapped, host_pixels, (size_t)width * height * bpp);
      if(dt_opencl_unmap_mem_object(devid, device_image, mapped) == CL_SUCCESS)
      {
        dt_opencl_finish(devid);
        return TRUE;
      }
    }
  }

  return dt_opencl_write_host_to_device(devid, host_pixels, device_image, width, height, bpp) == CL_SUCCESS;
}

static gboolean _drawlayer_acquire_source_image(const int devid, const float *layer_pixels,
                                                dt_pixel_cache_entry_t *resolved_entry,
                                                const gboolean force_device_copy,
                                                const gboolean realtime_reuse,
                                                const int source_w, const int source_h,
                                                drawlayer_cl_image_handle_t *source)
{
  if(!source || !layer_pixels || source_w <= 0 || source_h <= 0) return FALSE;
  *source = (drawlayer_cl_image_handle_t){ 0 };

  if(force_device_copy)
  {
    source->mem = dt_opencl_copy_host_to_device(devid, (void *)layer_pixels, source_w, source_h, 4 * sizeof(float));
    return source->mem != NULL;
  }

  source->mem = dt_dev_pixelpipe_cache_get_pinned_image(darktable.pixelpipe_cache, (void *)layer_pixels,
                                                        resolved_entry, devid, source_w, source_h,
                                                        4 * sizeof(float),
                                                        CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR,
                                                        NULL, NULL);
  if(source->mem)
  {
    source->is_pinned = TRUE;
    return TRUE;
  }

  if(realtime_reuse && resolved_entry)
  {
    source->mem = dt_pixel_cache_clmem_get(resolved_entry, NULL, devid, source_w, source_h,
                                           4 * (int)sizeof(float), CL_MEM_READ_WRITE, NULL);
    if(!source->mem)
    {
      dt_dev_pixelpipe_cache_flush_clmem(darktable.pixelpipe_cache, devid);
      source->mem = dt_pixel_cache_clmem_get(resolved_entry, NULL, devid, source_w, source_h,
                                             4 * (int)sizeof(float), CL_MEM_READ_WRITE, NULL);
    }

    if(source->mem)
    {
      if(_drawlayer_sync_host_image_to_device(devid, source->mem, (void *)layer_pixels,
                                              source_w, source_h, 4 * sizeof(float)))
      {
        source->is_cached_device = TRUE;
        return TRUE;
      }

      dt_pixel_cache_clmem_remove(resolved_entry, source->mem);
      dt_opencl_release_mem_object(source->mem);
      source->mem = NULL;
    }
  }

  source->mem = dt_opencl_alloc_device_use_host_pointer(devid, source_w, source_h, 4 * sizeof(float),
                                                         (void *)layer_pixels,
                                                         CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR);
  if(source->mem)
  {
    if(_drawlayer_sync_host_image_to_device(devid, source->mem, (void *)layer_pixels,
                                            source_w, source_h, 4 * sizeof(float)))
    {
      source->is_pinned = TRUE;
      return TRUE;
    }

    dt_opencl_release_mem_object(source->mem);
    source->mem = NULL;
  }

  source->mem = dt_opencl_copy_host_to_device(devid, (void *)layer_pixels, source_w, source_h, 4 * sizeof(float));
  return source->mem != NULL;
}

static int _drawlayer_copy_or_resample_layer_roi(const int devid, cl_mem dev_source_rgba, cl_mem dev_layer_rgba,
                                                 const int source_w, const int source_h,
                                                 const dt_iop_roi_t *const target_roi,
                                                 const dt_iop_roi_t *const source_roi)
{
  const gboolean can_copy_crop = (fabs(target_roi->scale - 1.0) <= 1e-6
                                  && fabs(source_roi->scale - 1.0) <= 1e-6
                                  && source_roi->x >= 0 && source_roi->y >= 0
                                  && source_roi->x + target_roi->width <= source_w
                                  && source_roi->y + target_roi->height <= source_h
                                  && source_roi->width == target_roi->width
                                  && source_roi->height == target_roi->height);
  if(can_copy_crop)
  {
    size_t src_origin[3] = { (size_t)source_roi->x, (size_t)source_roi->y, 0 };
    size_t dst_origin[3] = { 0, 0, 0 };
    size_t region[3] = { (size_t)target_roi->width, (size_t)target_roi->height, 1 };
    const int copy_err = dt_opencl_enqueue_copy_image(devid, dev_source_rgba, dev_layer_rgba,
                                                      src_origin, dst_origin, region);
    if(copy_err == CL_SUCCESS) return CL_SUCCESS;
  }

  return dt_iop_clip_and_zoom_roi_cl(devid, dev_layer_rgba, dev_source_rgba, target_roi, source_roi);
}

static gboolean _drawlayer_acquire_layer_image(const int devid, dt_pixel_cache_entry_t *resolved_entry,
                                               const gboolean realtime_reuse, const gboolean direct_copy,
                                               cl_mem dev_source_rgba, const int source_w, const int source_h,
                                               const dt_iop_roi_t *const target_roi,
                                               const dt_iop_roi_t *const source_roi,
                                               drawlayer_cl_image_handle_t *layer, int *err)
{
  if(!layer || !err || !target_roi || !source_roi) return FALSE;
  *layer = (drawlayer_cl_image_handle_t){ 0 };

  if(direct_copy)
  {
    layer->mem = dev_source_rgba;
    return TRUE;
  }

  if(realtime_reuse && resolved_entry)
  {
    layer->mem = dt_pixel_cache_clmem_get(resolved_entry, NULL, devid, target_roi->width, target_roi->height,
                                          4 * (int)sizeof(float), CL_MEM_READ_WRITE, NULL);
    if(!layer->mem)
    {
      dt_dev_pixelpipe_cache_flush_clmem(darktable.pixelpipe_cache, devid);
      layer->mem = dt_pixel_cache_clmem_get(resolved_entry, NULL, devid, target_roi->width, target_roi->height,
                                            4 * (int)sizeof(float), CL_MEM_READ_WRITE, NULL);
    }
    layer->is_cached_device = (layer->mem != NULL);
  }

  if(!layer->mem)
  {
    layer->mem = dt_opencl_alloc_device(devid, target_roi->width, target_roi->height, 4 * sizeof(float));
    if(!layer->mem)
    {
      dt_dev_pixelpipe_cache_flush_clmem(darktable.pixelpipe_cache, devid);
      layer->mem = dt_opencl_alloc_device(devid, target_roi->width, target_roi->height, 4 * sizeof(float));
    }
  }

  if(!layer->mem)
  {
    *err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    return FALSE;
  }

  *err = _drawlayer_copy_or_resample_layer_roi(devid, dev_source_rgba, layer->mem, source_w, source_h,
                                               target_roi, source_roi);
  return *err == CL_SUCCESS;
}

static int _drawlayer_run_premult_over_kernel(const int devid, const int kernel_premult_over,
                                              cl_mem dev_background, cl_mem dev_layer_rgba, cl_mem dev_out,
                                              const int width, const int height,
                                              const int background_offset_x, const int background_offset_y)
{
  const int offs[2] = { background_offset_x, background_offset_y };
  const size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };

  int err = dt_opencl_set_kernel_arg(devid, kernel_premult_over, 0, sizeof(cl_mem), &dev_background);
  err |= dt_opencl_set_kernel_arg(devid, kernel_premult_over, 1, sizeof(cl_mem), &dev_layer_rgba);
  err |= dt_opencl_set_kernel_arg(devid, kernel_premult_over, 2, sizeof(cl_mem), &dev_out);
  err |= dt_opencl_set_kernel_arg(devid, kernel_premult_over, 3, sizeof(int), &width);
  err |= dt_opencl_set_kernel_arg(devid, kernel_premult_over, 4, sizeof(int), &height);
  err |= dt_opencl_set_kernel_arg(devid, kernel_premult_over, 5, sizeof(offs), offs);
  if(err != CL_SUCCESS) return err;

  return dt_opencl_enqueue_kernel_2d(devid, kernel_premult_over, sizes);
}

static int _blend_layer_over_input_cl(const int devid, const int kernel_premult_over, cl_mem dev_out, cl_mem dev_in,
                                      drawlayer_process_scratch_t *scratch, const float *layer_pixels,
                                      dt_pixel_cache_entry_t *source_entry,
                                      cl_mem source_mem_override,
                                      const int source_w, const int source_h,
                                      const dt_iop_roi_t *const target_roi,
                                      const dt_iop_roi_t *const source_roi,
                                      const gboolean direct_copy,
                                      const gboolean use_preview_bg, const float preview_bg,
                                      const gboolean realtime_reuse,
                                      const gboolean force_device_copy)
{
  if(devid < 0 || !dev_out || !dev_in || !scratch || (!layer_pixels && !source_mem_override) || source_w <= 0 || source_h <= 0
     || !target_roi || target_roi->width <= 0 || target_roi->height <= 0)
    return FALSE;
  if(kernel_premult_over < 0) return FALSE;

  dt_pixel_cache_entry_t *resolved_entry = source_entry;
  gboolean resolved_entry_ref = FALSE;
  if(realtime_reuse && !resolved_entry)
  {
    resolved_entry = dt_dev_pixelpipe_cache_ref_entry_for_host_ptr(darktable.pixelpipe_cache, (void *)layer_pixels);
    resolved_entry_ref = (resolved_entry != NULL);
  }

  drawlayer_cl_image_handle_t source = { 0 };
  drawlayer_cl_image_handle_t layer = { 0 };
  cl_mem dev_background = NULL;
  int err = CL_SUCCESS;
  int result = FALSE;
  if(source_mem_override)
    source.mem = source_mem_override;
  else if(!_drawlayer_acquire_source_image(devid, layer_pixels, resolved_entry, force_device_copy, realtime_reuse,
                                           source_w, source_h, &source))
    goto cleanup;

  if(!_drawlayer_acquire_layer_image(devid, resolved_entry, realtime_reuse, direct_copy, source.mem,
                                     source_w, source_h, target_roi, source_roi, &layer, &err))
    goto cleanup;

  if(use_preview_bg)
  {
    const size_t out_pixels = (size_t)target_roi->width * target_roi->height;
    float *background = dt_drawlayer_cache_ensure_scratch_buffer(&scratch->cl_background_rgba,
                                                                 &scratch->cl_background_rgba_pixels, out_pixels,
                                                                 "drawlayer process scratch");
    if(!background) goto cleanup;
    for(size_t kk = 0; kk < out_pixels; kk++)
    {
      float *pixel = background + 4 * kk;
      pixel[0] = preview_bg;
      pixel[1] = preview_bg;
      pixel[2] = preview_bg;
      pixel[3] = 1.0f;
    }
    dev_background = dt_dev_pixelpipe_cache_get_pinned_image(darktable.pixelpipe_cache, background, NULL, devid,
                                                             target_roi->width, target_roi->height,
                                                             4 * sizeof(float),
                                                             CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR, NULL, NULL);
    if(!dev_background) goto cleanup;
  }
  else
    dev_background = dev_in;

  err = _drawlayer_run_premult_over_kernel(devid, kernel_premult_over, dev_background,
                                           layer.mem, dev_out,
                                           target_roi->width, target_roi->height,
                                           0, 0);
  if(err != CL_SUCCESS) goto cleanup;

  /* The realtime display source (`process_read_patch`) is a host-side snapshot.
   * When imported as CL_MEM_USE_HOST_PTR, queued GPU reads may still touch that
   * host memory after process_cl() returns. The backend worker would then be
   * free to publish newer patch contents into the same buffer, racing the GPU
   * and making in-stroke updates appear stale or only become visible at stroke
   * end. Finish the queue before releasing the snapshot whenever the layer
   * source is host-backed. */
  if(source.is_pinned)
  {
    if(!dt_opencl_finish(devid))
    {
      err = -1;
      goto cleanup;
    }
  }

  result = TRUE;

cleanup:
  if(use_preview_bg) dt_dev_pixelpipe_cache_put_pinned_image(darktable.pixelpipe_cache, scratch->cl_background_rgba,
                                                             NULL, -1, (void **)&dev_background);
  if(layer.mem && layer.mem != source.mem)
  {
    if(layer.is_cached_device && resolved_entry)
      dt_pixel_cache_clmem_put(resolved_entry, NULL, devid, target_roi->width, target_roi->height,
                               4 * (int)sizeof(float), CL_MEM_READ_WRITE, IOP_CS_RGB, layer.mem);
    else
      dt_opencl_release_mem_object(layer.mem);
  }
  if(!source_mem_override && source.is_pinned)
    dt_dev_pixelpipe_cache_put_pinned_image(darktable.pixelpipe_cache, (void *)layer_pixels, resolved_entry, -1,
                                            (void **)&source.mem);
  else if(!source_mem_override && source.is_cached_device && resolved_entry)
    dt_pixel_cache_clmem_put(resolved_entry, NULL, devid, source_w, source_h, 4 * (int)sizeof(float),
                             CL_MEM_READ_WRITE, IOP_CS_RGB, source.mem);
  else if(!source_mem_override && source.mem)
    dt_opencl_release_mem_object(source.mem);
  if(resolved_entry_ref)
    dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, resolved_entry->hash, FALSE, resolved_entry);

  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL, "[drawlayer] process_cl blend path failed: %d\n", err);

  return result;
}
#endif

static void _virtual_piece_input_offset(dt_iop_module_t *self, int *x, int *y)
{
  int ox = 0;
  int oy = 0;

  if(self && self->dev && self->dev->virtual_pipe)
  {
    dt_dev_pixelpipe_iop_t *piece = dt_dev_distort_get_iop_pipe(self->dev, self->dev->virtual_pipe, self);
    if(piece)
      dt_drawlayer_cache_resolve_piece_input_origin(piece, piece->pipe->iwidth, piece->pipe->iheight, &ox, &oy);
  }

  if(x) *x = ox;
  if(y) *y = oy;
}

static gboolean _string_field_is_sane(const char *value, const size_t value_size)
{
  if(!value || value_size == 0) return FALSE;
  return memchr(value, '\0', value_size) != NULL;
}

static gboolean _profile_key_is_sane(const char *value)
{
  if(!value || value[0] == '\0') return FALSE;

  int separators = 0;
  for(const unsigned char *c = (const unsigned char *)value; *c; c++)
  {
    if(*c == '|') separators++;
    else if(!g_ascii_isprint(*c)) return FALSE;
  }

  return separators >= 2;
}

static int64_t _sidecar_timestamp_from_path(const char *path)
{
  if(!path || path[0] == '\0' || !g_file_test(path, G_FILE_TEST_EXISTS)) return 0;

  GStatBuf st = { 0 };
  if(g_stat(path, &st) != 0) return 0;
  return (int64_t)st.st_mtime;
}

static void _ensure_cursor_stamp_surface(dt_iop_module_t *self, const float widget_radius,
                                         const float opacity, const float hardness)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || widget_radius <= 0.0f) return;

  const double ppd = (darktable.gui && darktable.gui->ppd > 0.0) ? darktable.gui->ppd : 1.0;
  float display_rgb[3] = { 0.0f };
  _conf_display_color(display_rgb);
  const int shape = _conf_brush_shape();
  const int size_px = MAX(2, (int)ceil((2.0f * widget_radius + 2.0f) * ppd));

  const gboolean needs_rebuild
      = !g->cursor_surface || g->cursor_surface_size != size_px || fabs(g->cursor_surface_ppd - ppd) > 1e-9
        || fabsf(g->cursor_radius - widget_radius) > 1e-3f || fabsf(g->cursor_opacity - opacity) > 1e-6f
        || fabsf(g->cursor_hardness - hardness) > 1e-6f || g->cursor_shape != shape
        || fabsf(g->cursor_color[0] - display_rgb[0]) > 1e-6f || fabsf(g->cursor_color[1] - display_rgb[1]) > 1e-6f
        || fabsf(g->cursor_color[2] - display_rgb[2]) > 1e-6f;
  if(!needs_rebuild) return;

  _clear_cursor_stamp_surface(g);
  g->cursor_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size_px, size_px);
  if(cairo_surface_status(g->cursor_surface) != CAIRO_STATUS_SUCCESS)
  {
    _clear_cursor_stamp_surface(g);
    return;
  }
  cairo_surface_set_device_scale(g->cursor_surface, ppd, ppd);

  unsigned char *data = cairo_image_surface_get_data(g->cursor_surface);
  const int stride = cairo_image_surface_get_stride(g->cursor_surface);
  memset(data, 0, (size_t)stride * size_px);

  dt_drawlayer_brush_dab_t dab = {
    .radius = fmaxf(widget_radius * (float)ppd, 0.5f),
    .shape = shape,
    .hardness = hardness,
    .opacity = opacity,
    .display_color = { display_rgb[0], display_rgb[1], display_rgb[2] },
  };

  const float half = 0.5f * (float)size_px;
  dt_drawlayer_brush_rasterize_dab_argb8(&dab, data, size_px, size_px, stride,
                                         half, half, 1.0f);
  cairo_surface_mark_dirty(g->cursor_surface);

  g->cursor_surface_size = size_px;
  g->cursor_surface_ppd = ppd;
  g->cursor_radius = widget_radius;
  g->cursor_opacity = opacity;
  g->cursor_hardness = hardness;
  g->cursor_shape = shape;
  g->cursor_color[0] = display_rgb[0];
  g->cursor_color[1] = display_rgb[1];
  g->cursor_color[2] = display_rgb[2];
}

static void _set_drawlayer_os_cursor_hidden(const gboolean hidden)
{
  if(!darktable.gui || !darktable.gui->ui) return;

  GtkWidget *center = dt_ui_center(darktable.gui->ui);
  GtkWidget *main = dt_ui_main_window(darktable.gui->ui);
  GdkDisplay *display = gdk_display_get_default();
  if(!display) return;

  const dt_cursor_t cursor_id = hidden ? GDK_BLANK_CURSOR : GDK_LEFT_PTR;
  GdkCursor *cursor = gdk_cursor_new_for_display(display, cursor_id);
  if(!cursor) return;

  if(center && gtk_widget_get_window(center))
    gdk_window_set_cursor(gtk_widget_get_window(center), cursor);
  if(main && gtk_widget_get_window(main))
    gdk_window_set_cursor(gtk_widget_get_window(main), cursor);

  dt_control_set_cursor(cursor_id);
  g_object_unref(cursor);
}

static gboolean _should_show_leave_wait_dialog(const dt_iop_drawlayer_gui_data_t *g)
{
  return g && (g->painting || g->cache_dirty || g->process_patch_dirty || g->stroke_sample_count > 0
               || g->finish_commit_pending || dt_drawlayer_worker_any_active(g->rt));
}

static drawlayer_wait_dialog_t _show_leave_wait_dialog(void)
{
  drawlayer_wait_dialog_t wait = { 0 };
  if(!darktable.gui || !darktable.gui->ui) return wait;

  GtkWidget *dialog = gtk_dialog_new();
  GtkWidget *main = dt_ui_main_window(darktable.gui->ui);
  if(main) gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(main));
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_deletable(GTK_WINDOW(dialog), FALSE);
  gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
  gtk_window_set_title(GTK_WINDOW(dialog), _("Saving layer"));

  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(12));
  GtkWidget *spinner = gtk_spinner_new();
  GtkWidget *label = gtk_label_new(_("Waiting for the layer to be saved..."));
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_box_pack_start(GTK_BOX(box), spinner, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), label, TRUE, TRUE, 0);
  gtk_container_set_border_width(GTK_CONTAINER(box), DT_PIXEL_APPLY_DPI(12));
  gtk_box_pack_start(GTK_BOX(content), box, TRUE, TRUE, 0);
  gtk_spinner_start(GTK_SPINNER(spinner));
  gtk_widget_show_all(dialog);
  gtk_widget_show_now(dialog);
  gtk_window_present(GTK_WINDOW(dialog));
  GdkDisplay *display = gtk_widget_get_display(dialog);
  if(display) gdk_display_flush(display);
  wait.dialog = dialog;
  return wait;
}

static void _hide_leave_wait_dialog(drawlayer_wait_dialog_t *wait)
{
  if(!wait || !wait->dialog) return;
  gtk_widget_destroy(wait->dialog);
  wait->dialog = NULL;
}

static gboolean _color_picker_set_from_position(dt_iop_module_t *self, const float x, const float y)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !g->widgets || !g->color) return FALSE;

  float display_rgb[3] = { 0.0f };
  if(!dt_drawlayer_widgets_update_from_picker_position(g->widgets, g->color, x, y, display_rgb)) return FALSE;
  _apply_display_brush_color(self, display_rgb, FALSE);
  return TRUE;
}

static gboolean _color_picker_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !g->widgets) return FALSE;
  const double ppd = (darktable.gui && darktable.gui->ppd > 0.0) ? darktable.gui->ppd : 1.0;
  return dt_drawlayer_widgets_draw_picker(g->widgets, widget, cr, ppd);
}

static gboolean _color_swatch_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !g->widgets) return FALSE;
  return dt_drawlayer_widgets_draw_swatch(g->widgets, widget, cr);
}

static gboolean _color_swatch_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !g->widgets || !widget || !event || event->button != 1) return FALSE;

  float display_rgb[3] = { 0.0f };
  if(!dt_drawlayer_widgets_pick_history_color(g->widgets, widget, event->x, event->y, display_rgb)) return FALSE;
  _apply_display_brush_color(self, display_rgb, FALSE);
  return TRUE;
}

static void _sync_brush_profile_preview_widget(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !g->widgets || !g->brush_shape) return;

  dt_drawlayer_widgets_set_brush_profile_preview(g->widgets,
                                                 _conf_opacity() / 100.0f,
                                                 _conf_hardness(),
                                                 _conf_sprinkles() / 100.0f,
                                                 _conf_sprinkle_size(),
                                                 _conf_sprinkle_coarseness() / 100.0f,
                                                 _conf_brush_shape());
  gtk_widget_queue_draw(g->brush_shape);
}

static gboolean _brush_profile_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !g->widgets) return FALSE;
  const double ppd = (darktable.gui && darktable.gui->ppd > 0.0) ? darktable.gui->ppd : 1.0;
  return dt_drawlayer_widgets_draw_brush_profiles(g->widgets, widget, cr, ppd);
}

static gboolean _brush_profile_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !g->widgets || !widget || !event || event->button != 1) return FALSE;

  int shape = DT_DRAWLAYER_BRUSH_SHAPE_LINEAR;
  if(!dt_drawlayer_widgets_pick_brush_profile(g->widgets, widget, event->x, event->y, &shape)) return FALSE;

  _sync_params_from_gui(self, FALSE);
  _sync_mode_sensitive_widgets(self);
  _sync_temp_buffers(self, TRUE, FALSE);
  _sync_brush_profile_preview_widget(self);
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean _working_rgb_to_display_rgb(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                            const float working_rgb[3], float display_rgb[3])
{
  if(!working_rgb || !display_rgb) return FALSE;

  display_rgb[0] = _clamp01(working_rgb[0]);
  display_rgb[1] = _clamp01(working_rgb[1]);
  display_rgb[2] = _clamp01(working_rgb[2]);

  dt_dev_pixelpipe_t *pipe = piece ? piece->pipe : (self && self->dev ? self->dev->pipe : NULL);
  if(!self || !pipe) return TRUE;

  const dt_iop_order_iccprofile_info_t *const work_profile = self->dev
                                                                 ? dt_ioppr_get_iop_work_profile_info(self, self->dev->iop)
                                                                 : NULL;
  const dt_iop_order_iccprofile_info_t *const display_profile = dt_ioppr_get_pipe_output_profile_info(pipe);
  if(!work_profile || !display_profile) return TRUE;

  float in[4] = { working_rgb[0], working_rgb[1], working_rgb[2], 0.0f };
  float out[4] = { working_rgb[0], working_rgb[1], working_rgb[2], 0.0f };
  dt_ioppr_transform_image_colorspace_rgb(in, out, 1, 1, work_profile, display_profile,
                                          "drawlayer picked color");
  display_rgb[0] = _clamp01(out[0]);
  display_rgb[1] = _clamp01(out[1]);
  display_rgb[2] = _clamp01(out[2]);
  return TRUE;
}

/** @brief Apply selected picker color to drawlayer brush color. */
void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_iop_t *piece)
{
  (void)picker;
  if(!self || darktable.gui->reset) return;
  const drawlayer_pick_source_t source = _conf_pick_source();
  const float *picked = (source == DRAWLAYER_PICK_SOURCE_OUTPUT) ? self->picked_output_color : self->picked_color;
  const float *picked_min = (source == DRAWLAYER_PICK_SOURCE_OUTPUT) ? self->picked_output_color_min : self->picked_color_min;
  const float *picked_max = (source == DRAWLAYER_PICK_SOURCE_OUTPUT) ? self->picked_output_color_max : self->picked_color_max;
  if(picked_max[0] < picked_min[0]) return;

  float display_rgb[3] = { 0.0f };
  if(!_working_rgb_to_display_rgb(self, piece, picked, display_rgb)) return;
  _apply_display_brush_color(self, display_rgb, TRUE);
}

static gboolean _color_picker_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(!event || event->button != 1) return FALSE;
  (void)widget;
  return _color_picker_set_from_position(self, event->x, event->y);
}

static gboolean _color_picker_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  (void)widget;
  (void)event;
  if(g && g->widgets)
  {
    float display_rgb[3] = { 0.0f };
    if(dt_drawlayer_widgets_finish_picker_drag(g->widgets, display_rgb)) _remember_display_color(self, display_rgb);
  }
  return FALSE;
}

static gboolean _color_picker_motion(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  (void)widget;
  if(!g || !g->widgets || !event || !dt_drawlayer_widgets_is_picker_dragging(g->widgets)) return FALSE;
  return _color_picker_set_from_position(self, event->x, event->y);
}

static void _sanitize_params(dt_iop_module_t *self, dt_iop_drawlayer_params_t *params)
{
  if(!params) return;

  if(!_string_field_is_sane(params->layer_name, sizeof(params->layer_name)))
    memset(params->layer_name, 0, sizeof(params->layer_name));
  else
    params->layer_name[sizeof(params->layer_name) - 1] = '\0';

  if(!_string_field_is_sane(params->work_profile, sizeof(params->work_profile)))
    memset(params->work_profile, 0, sizeof(params->work_profile));
  else
    params->work_profile[sizeof(params->work_profile) - 1] = '\0';

  if(params->layer_order < -1) params->layer_order = -1;
  if(params->work_profile[0] != '\0' && !_profile_key_is_sane(params->work_profile))
    memset(params->work_profile, 0, sizeof(params->work_profile));

  // Freshly enabled or migrated instances without a concrete sidecar page should adopt the current profile.
  if(params->layer_order < 0 && params->stroke_commit_hash == 0u)
    memset(params->work_profile, 0, sizeof(params->work_profile));

  _ensure_layer_name(self, params);

  if(params->work_profile[0] == '\0' && self && self->dev)
  {
    char current_profile[DRAWLAYER_PROFILE_SIZE] = { 0 };
    if(_get_current_work_profile_key(self, self->dev->iop, self->dev->pipe, current_profile, sizeof(current_profile)))
      g_strlcpy(params->work_profile, current_profile, sizeof(params->work_profile));
  }
}

static gboolean _widget_to_layer_coords(dt_iop_module_t *self, const double wx, const double wy, float *lx, float *ly)
{
  if(!self || !self->dev || !self->dev->virtual_pipe) return FALSE;

  float pt[2] = { (float)wx, (float)wy };
  dt_dev_coordinates_widget_to_image_norm(self->dev, pt, 1);
  dt_dev_coordinates_image_norm_to_image_abs(self->dev, pt, 1);

  if(!dt_dev_distort_backtransform_plus(self->dev, self->dev->virtual_pipe, self->iop_order,
                                        DT_DEV_TRANSFORM_DIR_FORW_EXCL, pt, 1))
    return FALSE;

  int offset_x = 0;
  int offset_y = 0;
  _virtual_piece_input_offset(self, &offset_x, &offset_y);
  pt[0] += offset_x;
  pt[1] += offset_y;

  *lx = pt[0];
  *ly = pt[1];
  return TRUE;
}

static void _sanitize_requested_layer_name(dt_iop_module_t *self, const char *requested, char *name,
                                           const size_t name_size)
{
  if(!name || name_size == 0) return;
  name[0] = '\0';
  if(requested && requested[0]) g_strlcpy(name, requested, name_size);
  g_strstrip(name);
  if(name[0] == '\0') _default_layer_name(self, name, name_size);
}

static void _layerio_append_error(GString *errors, const char *message)
{
  if(!errors || !message || message[0] == '\0') return;
  if(errors->len > 0) g_string_append(errors, "; ");
  g_string_append(errors, message);
}

static void _layerio_log_errors(GString *errors)
{
  if(!errors) return;
  if(errors->len > 0) dt_control_log("%s", errors->str);
}

static void _populate_layer_list(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  dt_iop_drawlayer_params_t *params = (dt_iop_drawlayer_params_t *)self->params;
  if(!g) return;
  if(darktable.gui) ++darktable.gui->reset;
  _ensure_layer_name(self, params);

  while(dt_bauhaus_combobox_length(g->layer_select) > 0)
    dt_bauhaus_combobox_remove_at(g->layer_select, dt_bauhaus_combobox_length(g->layer_select) - 1);

  if(!self->dev)
  {
    dt_bauhaus_combobox_add(g->layer_select, params->layer_name);
    dt_bauhaus_combobox_set(g->layer_select, 0);
    if(darktable.gui) --darktable.gui->reset;
    return;
  }

  char path[PATH_MAX] = { 0 };
  char **names = NULL;
  int count = 0;
  if(!dt_drawlayer_io_sidecar_path(self->dev->image_storage.id, path, sizeof(path))
     || !dt_drawlayer_io_list_layer_names(path, &names, &count))
  {
    dt_bauhaus_combobox_add(g->layer_select, params->layer_name);
    dt_bauhaus_combobox_set(g->layer_select, 0);
    if(darktable.gui) --darktable.gui->reset;
    return;
  }

  int active = -1;
  for(int i = 0; i < count; i++)
  {
    const char *page_name = names[i] ? names[i] : "";
    dt_bauhaus_combobox_add(g->layer_select, page_name);
    if(params->layer_order == i
       || (params->layer_name[0] && !g_strcmp0(page_name, params->layer_name)))
      active = i;
  }

  dt_drawlayer_io_free_layer_names(&names, &count);
  if(count == 0)
  {
    dt_bauhaus_combobox_add(g->layer_select, params->layer_name);
    dt_bauhaus_combobox_set(g->layer_select, 0);
  }
  else if(active >= 0)
    dt_bauhaus_combobox_set(g->layer_select, active);
  else
    dt_bauhaus_combobox_set(g->layer_select, 0);
  if(darktable.gui) --darktable.gui->reset;
}

/* Realtime worker/queue implementation lives in its own implementation include. */
#include "drawlayer/worker.c"

static void _reset_stroke_session(dt_iop_drawlayer_gui_data_t *g)
{
  if(!g) return;
  g->stroke_sample_count = 0;
  g->stroke_event_index = 0;
  g->last_dab_valid = FALSE;
  dt_pthread_mutex_lock(&g->process_patch_mutex);
  _paint_reset_stroke_runtime(g);
  dt_drawlayer_paint_runtime_state_reset(&g->process_dirty_rect);
  if(g->stroke_mask.pixels)
    memset(g->stroke_mask.pixels, 0,
           (size_t)g->stroke_mask.width * g->stroke_mask.height * sizeof(float));
  if(g->process_stroke_mask.pixels)
    memset(g->process_stroke_mask.pixels, 0,
           (size_t)g->process_stroke_mask.width * g->process_stroke_mask.height * sizeof(float));
  dt_pthread_mutex_unlock(&g->process_patch_mutex);
}

static void _invalidate_process_patch(dt_iop_drawlayer_gui_data_t *g)
{
  /* Mark the transformed process-tile cache as stale while keeping any
   * allocated storage around for reuse. Geometry changes or out-of-band layer
   * edits use this path; in-stroke backend dabs update the transformed tile
   * incrementally and therefore do not need to invalidate it. */
  if(!g) return;
  dt_pthread_mutex_lock(&g->process_patch_mutex);
  dt_drawlayer_cache_invalidate_process_patch_state(&g->process_patch_valid, &g->process_patch_dirty,
                                                    &g->process_dirty_rect,
                                                    &g->process_patch_padding, &g->process_combined_roi);
  g->process_geom_hash = 0;
  g->process_cl_prewarm_hash = 0;
  g->process_cl_prewarm_devid = -1;
  dt_drawlayer_paint_runtime_state_reset(g->process_path);
  dt_pthread_mutex_unlock(&g->process_patch_mutex);
  /* Do not clear `process_stroke_mask` here: it is either reset on stroke
   * start or reinitialized when rebuilding the process patch. */
}

static gboolean _layer_cache_matches(const dt_iop_drawlayer_gui_data_t *g, const int32_t imgid, const int raw_width,
                                     const int raw_height, const char *layer_name, const int layer_order)
{
  /* Cache identity belongs to the module orchestration layer because it
   * combines sidecar identity with GUI/module state (active image, selected
   * layer, in-memory patch ownership). The low-level TIFF primitives stay in
   * drawlayer/io.c. */
  if(!g || !g->cache_valid || !g->base_patch.pixels) return FALSE;
  if(g->cache_imgid != imgid || g->base_patch.width != raw_width || g->base_patch.height != raw_height) return FALSE;

  /* Once a layer is cached in memory, `process()` should treat that payload as
   * authoritative until we explicitly reload or flush it. The page index in the
   * params is only a sidecar lookup hint and may legitimately drift after sidecar
   * rewrites or page reordering. Layer names are unique and stable by design, so
   * prefer them for cache identity. Falling back to the page index here causes
   * `process()` to re-open the TIFF and re-scan directories even though the
   * correct pixels are already in memory. */
  const char *target_name = layer_name ? layer_name : "";
  if(target_name[0] != '\0' || g->cache_layer_name[0] != '\0')
    return !g_strcmp0(g->cache_layer_name, target_name);

  if(layer_order >= 0 && g->cache_layer_order >= 0) return g->cache_layer_order == layer_order;
  return TRUE;
}

static gboolean _ensure_layer_cache(dt_iop_module_t *self)
{
  /* Make sure `base_patch` mirrors the currently selected sidecar layer.
   * The low-level TIFF read/write primitives live in drawlayer/io.c, while this
   * function stays here because it orchestrates cache lifetime, widget state,
   * prompting, and history/UI side effects around those I/O operations. */
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  dt_iop_drawlayer_params_t *params = (dt_iop_drawlayer_params_t *)self->params;
  if(!g || !self->dev) return FALSE;

  _sanitize_params(self, params);
  const int raw_width = self->dev->roi.raw_width;
  const int raw_height = self->dev->roi.raw_height;
  const int32_t imgid = self->dev->image_storage.id;
  GMainContext *const ui_ctx = g_main_context_default();
  const gboolean ui_thread = ui_ctx && g_main_context_is_owner(ui_ctx);
  if(imgid <= 0 || raw_width <= 0 || raw_height <= 0) return FALSE;
  const gboolean bootstrap = (params->sidecar_timestamp == 0 && params->layer_order < 0);

  GString *errors = g_string_new(NULL);

  char current_profile[DRAWLAYER_PROFILE_SIZE] = { 0 };
  const gboolean have_current_profile = _get_current_work_profile_key(self, self->dev->iop, self->dev->pipe,
                                                                      current_profile, sizeof(current_profile));
  if(bootstrap && have_current_profile)
    g_strlcpy(params->work_profile, current_profile, sizeof(params->work_profile));
  else if(!have_current_profile)
    _layerio_append_error(errors, _("failed to resolve drawlayer working profile"));
  else if(params->work_profile[0] == '\0')
    g_strlcpy(params->work_profile, current_profile, sizeof(params->work_profile));
  else if(g_strcmp0(params->work_profile, current_profile))
    _layerio_append_error(errors, _("drawlayer working profile mismatch"));

  if(_layer_cache_matches(g, imgid, raw_width, raw_height, params->layer_name, params->layer_order))
  {
    _layerio_log_errors(errors);
    g_string_free(errors, TRUE);
    return TRUE;
  }

  if(!_flush_layer_cache(self))
  {
    _layerio_append_error(errors, _("failed to write drawing layer sidecar"));
    _layerio_log_errors(errors);
    g_string_free(errors, TRUE);
    return FALSE;
  }
  _invalidate_undo_redo(self);
  /* We are about to replace/rebind `g->base_patch`; drop all explicit extra
   * refs from the previous entry first so counters never leak across entries. */
  _release_all_base_patch_extra_refs(g);

  int created = 0;
  if(!_acquire_shared_base_patch(&g->base_patch, imgid, params, raw_width, raw_height,
                                 "drawlayer sidecar cache", &created))
  {
    _release_all_base_patch_extra_refs(g);
    _clear_patch(&g->base_patch);
    g->cache_valid = FALSE;
    _layerio_log_errors(errors);
    g_string_free(errors, TRUE);
    return FALSE;
  }
  if(created)
  {
    dt_drawlayer_cache_patch_wrlock(&g->base_patch);
    dt_drawlayer_cache_clear_transparent_float(g->base_patch.pixels, (size_t)raw_width * raw_height);
    dt_drawlayer_cache_patch_wrunlock(&g->base_patch);
  }

  gboolean ok = TRUE;
  gboolean cache_loaded = FALSE;
  gboolean file_exists = FALSE;
  char path[PATH_MAX] = { 0 };

  if(!created)
  {
    cache_loaded = TRUE;
  }
  else if(!dt_drawlayer_io_sidecar_path(imgid, path, sizeof(path)))
  {
    _release_all_base_patch_extra_refs(g);
    _clear_patch(&g->base_patch);
    g->cache_valid = FALSE;
    _layerio_append_error(errors, _("failed to resolve drawlayer sidecar path"));
    _layerio_log_errors(errors);
    g_string_free(errors, TRUE);
    return TRUE;
  }

  if(created) file_exists = g_file_test(path, G_FILE_TEST_EXISTS);

  if(created && bootstrap)
  {
    if(!have_current_profile)
    {
      ok = FALSE;
    }
    else
    {
      gboolean initialized = FALSE;

      /* Bootstrap should not rename/create layers blindly when reopening an
       * existing image: first try to resolve the layer by its serialized name. */
      if(file_exists)
      {
        drawlayer_dir_info_t info = { 0 };
        dt_drawlayer_io_layer_info_t io_info = { 0 };
        if(dt_drawlayer_io_find_layer(path, params->layer_name, -1, &io_info))
        {
          dt_drawlayer_io_patch_t io_patch = { 0 };
          _from_io_info(&io_info, &info);
          _to_io_patch(&g->base_patch, &io_patch);
          if(!dt_drawlayer_io_load_layer(path, params->layer_name, info.index, raw_width, raw_height, &io_patch))
          {
            _layerio_append_error(errors, _("failed to read drawing layer sidecar"));
            ok = FALSE;
          }
          else
          {
            params->layer_order = info.index;
            params->sidecar_timestamp = _sidecar_timestamp_from_path(path);
            cache_loaded = TRUE;
            initialized = TRUE;
          }
        }
      }

      if(ok && !initialized)
      {
        char unique_name[DRAWLAYER_NAME_SIZE] = { 0 };
        char fallback[DRAWLAYER_NAME_SIZE] = { 0 };
        _default_layer_name(self, fallback, sizeof(fallback));
        dt_drawlayer_io_make_unique_name(path, params->layer_name, fallback, unique_name, sizeof(unique_name));
        g_strlcpy(params->layer_name, unique_name, sizeof(params->layer_name));

        int final_order = -1;
        dt_drawlayer_io_patch_t io_patch = { 0 };
        dt_drawlayer_cache_patch_rdlock(&g->base_patch);
        _to_io_patch(&g->base_patch, &io_patch);
        const gboolean stored = dt_drawlayer_io_store_layer(path, params->layer_name, -1, params->work_profile,
                                                            &io_patch, raw_width, raw_height, FALSE, &final_order);
        dt_drawlayer_cache_patch_rdunlock(&g->base_patch);
        if(!stored)
        {
          _layerio_append_error(errors, _("failed to initialize drawing layer sidecar"));
          ok = FALSE;
        }
        else
        {
          params->layer_order = final_order;
          params->sidecar_timestamp = _sidecar_timestamp_from_path(path);
          _touch_layer_cache_epoch(g);
          _invalidate_process_patch(g);
          cache_loaded = TRUE;
        }
      }
    }
  }
  else if(created && !file_exists)
  {
    _layerio_append_error(errors, _("drawlayer sidecar TIFF is missing"));
  }
  else if(created)
  {
    drawlayer_dir_info_t info = { 0 };
    dt_drawlayer_io_layer_info_t io_info = { 0 };
    if(!dt_drawlayer_io_find_layer(path, params->layer_name, -1, &io_info))
    {
      int missing_action = 0;
      if(ui_thread && self->dev && self->dev->gui_module == self)
        missing_action = _offer_missing_layer_recreation(self, params->layer_name);

      if(missing_action == 2)
      {
        if(!have_current_profile)
        {
          _layerio_append_error(errors, _("failed to resolve drawlayer working profile"));
          ok = FALSE;
        }
        else
        {
          g_string_truncate(errors, 0);
          g_strlcpy(params->work_profile, current_profile, sizeof(params->work_profile));
          int final_order = -1;
          dt_drawlayer_io_patch_t io_patch = { 0 };
          dt_drawlayer_cache_patch_rdlock(&g->base_patch);
          _to_io_patch(&g->base_patch, &io_patch);
          const gboolean stored = dt_drawlayer_io_store_layer(path, params->layer_name, -1, params->work_profile,
                                                              &io_patch, raw_width, raw_height, FALSE,
                                                              &final_order);
          dt_drawlayer_cache_patch_rdunlock(&g->base_patch);
          if(!stored)
          {
            _layerio_append_error(errors, _("failed to initialize drawing layer sidecar"));
            ok = FALSE;
          }
          else
          {
            if(g)
            {
              g->missing_layer_prompt_name[0] = '\0';
            }
            params->layer_order = final_order;
            params->sidecar_timestamp = _sidecar_timestamp_from_path(path);
            _touch_layer_cache_epoch(g);
            _invalidate_process_patch(g);
            cache_loaded = TRUE;
          }
        }
      }
      else if(missing_action == 0)
      {
        _layerio_append_error(errors, _("drawlayer layer not found in sidecar TIFF"));
      }
    }
    else
    {
      _from_io_info(&io_info, &info);
      if(g)
      {
        g->missing_layer_prompt_name[0] = '\0';
      }
      params->layer_order = info.index;
      if(info.work_profile[0] != '\0' && params->work_profile[0] == '\0')
        g_strlcpy(params->work_profile, info.work_profile, sizeof(params->work_profile));

      if(have_current_profile && info.work_profile[0] != '\0' && g_strcmp0(info.work_profile, current_profile))
        _layerio_append_error(errors, _("drawlayer sidecar profile mismatch"));

      dt_drawlayer_io_patch_t io_patch = { 0 };
      dt_drawlayer_cache_patch_wrlock(&g->base_patch);
      _to_io_patch(&g->base_patch, &io_patch);
      const gboolean loaded = dt_drawlayer_io_load_layer(path, params->layer_name, info.index, raw_width,
                                                         raw_height, &io_patch);
      dt_drawlayer_cache_patch_wrunlock(&g->base_patch);
      if(!loaded)
      {
        _layerio_append_error(errors, _("failed to read drawing layer sidecar"));
        ok = FALSE;
      }
      else
      {
        params->sidecar_timestamp = _sidecar_timestamp_from_path(path);
        _touch_layer_cache_epoch(g);
        _invalidate_process_patch(g);
        cache_loaded = TRUE;
      }
    }
  }

  if(cache_loaded)
  {
    g->cache_valid = TRUE;
    g->cache_dirty = FALSE;
    g->cache_imgid = imgid;
    g_strlcpy(g->cache_layer_name, params->layer_name, sizeof(g->cache_layer_name));
    g->cache_layer_order = params->layer_order;

    if(ui_thread && g->layer_name && g_strcmp0(gtk_entry_get_text(g->layer_name), params->layer_name))
    {
      gtk_entry_set_text(g->layer_name, params->layer_name);
    }
    if(ui_thread && g->layer_select) _populate_layer_list(self);
    if(created) _retain_base_patch_loaded_ref(g);
  }
  else
  {
    _release_all_base_patch_extra_refs(g);
    _clear_patch(&g->base_patch);
    g->cache_valid = FALSE;
    g->cache_dirty = FALSE;
  }

  if(ui_thread) _sync_save_button(self);

  _layerio_log_errors(errors);
  g_string_free(errors, TRUE);
  return ok || !cache_loaded;
}

static gboolean _flush_layer_cache(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  if(!g || !self->dev || !g->cache_valid || !g->cache_dirty || !g->base_patch.pixels) return TRUE;
  if(!_layer_name_non_empty(g->cache_layer_name)) return FALSE;
  if(dt_drawlayer_worker_any_active(g->rt))
    dt_drawlayer_worker_flush_finished_strokes(g->rt);

  /* If the visible transformed tile has been updated incrementally for the
   * current ROI, fold it back into the authoritative raw-sized cache before the
   * sidecar write. That keeps the final flushed layer consistent with what the
   * user actually saw, even if the transformed tile was carrying the most recent
   * stroke-stateful updates. */
  _flush_process_patch_to_base(self, g);

  char path[PATH_MAX] = { 0 };
  const int32_t flush_imgid = (g->cache_imgid > 0) ? g->cache_imgid : self->dev->image_storage.id;
  if(flush_imgid <= 0) return TRUE;
  if(!dt_drawlayer_io_sidecar_path(flush_imgid, path, sizeof(path))) return FALSE;

  int final_order = g->cache_layer_order;
  const dt_iop_drawlayer_params_t *params = (const dt_iop_drawlayer_params_t *)self->params;
  const char *work_profile = params ? params->work_profile : "";
  dt_drawlayer_io_patch_t io_patch = { 0 };
  dt_drawlayer_cache_patch_rdlock(&g->base_patch);
  _to_io_patch(&g->base_patch, &io_patch);
  const gboolean ok = dt_drawlayer_io_store_layer(path, g->cache_layer_name, g->cache_layer_order, work_profile,
                                                  &io_patch, g->base_patch.width, g->base_patch.height, FALSE,
                                                  &final_order);
  dt_drawlayer_cache_patch_rdunlock(&g->base_patch);
  if(!ok) return FALSE;

  g->cache_layer_order = final_order;
  g->cache_dirty = FALSE;

  dt_iop_drawlayer_params_t *mutable_params = (dt_iop_drawlayer_params_t *)self->params;
  if(mutable_params)
  {
    if(!g_strcmp0(mutable_params->layer_name, g->cache_layer_name)) mutable_params->layer_order = final_order;
    mutable_params->sidecar_timestamp = _sidecar_timestamp_from_path(path);
    _rekey_shared_base_patch(&g->base_patch, flush_imgid, mutable_params);
  }
  _release_all_base_patch_extra_refs(g);
  return TRUE;
}

static gboolean _ensure_widget_cache(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  if(!g || !self->dev) return FALSE;

  drawlayer_view_patch_t visible = { 0 };
  float view_x0 = 0.0f, view_y0 = 0.0f, view_x1 = 0.0f, view_y1 = 0.0f;
  if(!_compute_view_patch(self, 0.0f, &visible, &view_x0, &view_y0, &view_x1, &view_y1)) return FALSE;

  float wx0 = 0.0f, wy0 = 0.0f, wx1 = 0.0f, wy1 = 0.0f;
  if(!_layer_bounds_to_widget_bounds(self, view_x0, view_y0, view_x1, view_y1, &wx0, &wy0, &wx1, &wy1)) return FALSE;

  const dt_drawlayer_damaged_rect_t live_view_rect = {
    .valid = TRUE,
    .nw = { (int)floorf(view_x0), (int)floorf(view_y0) },
    .se = { (int)ceilf(view_x1), (int)ceilf(view_y1) },
  };
  const dt_drawlayer_damaged_rect_t preview_rect = {
    .valid = TRUE,
    .nw = { (int)floorf(wx0), (int)floorf(wy0) },
    .se = { (int)ceilf(wx1), (int)ceilf(wy1) },
  };

  const gboolean same_view = (g->live_patch.width == visible.width && g->live_patch.height == visible.height
                              && g->live_patch.x == visible.x && g->live_patch.y == visible.y
                              && g->live_view_rect.valid && g->preview_rect.valid
                              && g->live_view_rect.nw[0] == live_view_rect.nw[0]
                              && g->live_view_rect.nw[1] == live_view_rect.nw[1]
                              && g->live_view_rect.se[0] == live_view_rect.se[0]
                              && g->live_view_rect.se[1] == live_view_rect.se[1]
                              && g->preview_rect.nw[0] == preview_rect.nw[0]
                              && g->preview_rect.nw[1] == preview_rect.nw[1]
                              && g->preview_rect.se[0] == preview_rect.se[0]
                              && g->preview_rect.se[1] == preview_rect.se[1]);

  if(same_view)
  {
    g->last_view_x = self->dev->roi.x;
    g->last_view_y = self->dev->roi.y;
    g->last_view_scale = self->dev->roi.scaling;
    return TRUE;
  }

  g->live_patch = visible;
  g->live_view_rect = live_view_rect;
  g->preview_rect = preview_rect;

  dt_drawlayer_paint_runtime_state_reset(g->backend_path);
  g->last_view_x = self->dev->roi.x;
  g->last_view_y = self->dev->roi.y;
  g->last_view_scale = self->dev->roi.scaling;
  return TRUE;
}

static float _current_live_padding(dt_iop_module_t *self)
{
  dt_drawlayer_brush_dab_t dab = {
    .radius = fmaxf(_conf_size(), 0.5f),
    .hardness = _conf_hardness(),
    .shape = _conf_brush_shape(),
  };
  return ceilf(dab.radius + 1.0f);
}

static gboolean _compute_view_patch(dt_iop_module_t *self, const float padding, drawlayer_view_patch_t *patch,
                                    float *x0, float *y0, float *x1, float *y1)
{
  if(!self || !self->dev || !patch) return FALSE;

  const int raw_width = self->dev->roi.raw_width;
  const int raw_height = self->dev->roi.raw_height;
  if(raw_width <= 0 || raw_height <= 0) return FALSE;

  const float widget_w = (float)self->dev->roi.orig_width;
  const float widget_h = (float)self->dev->roi.orig_height;
  const float preview_w = self->dev->roi.preview_width;
  const float preview_h = self->dev->roi.preview_height;
  if(widget_w <= 0.0f || widget_h <= 0.0f || preview_w <= 0.0f || preview_h <= 0.0f) return FALSE;

  const float zoom_scale = dt_dev_get_overlay_scale(self->dev);
  const float border = (float)self->dev->roi.border_size;
  const float roi_w = fminf(widget_w, preview_w * zoom_scale);
  const float roi_h = fminf(widget_h, preview_h * zoom_scale);
  const float rec_x = fmaxf(border, 0.5f * (widget_w - roi_w));
  const float rec_y = fmaxf(border, 0.5f * (widget_h - roi_h));
  const float rec_w = fminf(widget_w - 2.0f * border, roi_w);
  const float rec_h = fminf(widget_h - 2.0f * border, roi_h);
  if(rec_w <= 0.0f || rec_h <= 0.0f) return FALSE;

  float pts[8] = { rec_x, rec_y, rec_x + rec_w, rec_y, rec_x, rec_y + rec_h, rec_x + rec_w, rec_y + rec_h };
  for(int i = 0; i < 4; i++)
  {
    if(!_widget_to_layer_coords(self, pts[2 * i], pts[2 * i + 1], &pts[2 * i], &pts[2 * i + 1])) return FALSE;
  }

  float min_x = pts[0];
  float max_x = pts[0];
  float min_y = pts[1];
  float max_y = pts[1];
  for(int i = 1; i < 4; i++)
  {
    min_x = fminf(min_x, pts[2 * i]);
    max_x = fmaxf(max_x, pts[2 * i]);
    min_y = fminf(min_y, pts[2 * i + 1]);
    max_y = fmaxf(max_y, pts[2 * i + 1]);
  }

  if(x0) *x0 = min_x;
  if(y0) *y0 = min_y;
  if(x1) *x1 = max_x;
  if(y1) *y1 = max_y;

  patch->x = MAX(0, (int)floorf(min_x - padding));
  patch->y = MAX(0, (int)floorf(min_y - padding));
  const int right = MIN(raw_width, (int)ceilf(max_x + padding));
  const int bottom = MIN(raw_height, (int)ceilf(max_y + padding));
  patch->width = MAX(0, right - patch->x);
  patch->height = MAX(0, bottom - patch->y);
  return patch->width > 0 && patch->height > 0;
}

static gboolean _sync_temp_buffers(dt_iop_module_t *self, const gboolean flush_pending, const gboolean record_history)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  if(!g || !self->dev) return FALSE;

  if(flush_pending && (g->painting || g->stroke_sample_count > 0) && !_commit_dabs(self, record_history)) return FALSE;
  _pause_worker(self, g->rt);
  if(!_ensure_layer_cache(self))
  {
    _resume_worker(self, g->rt);
    return FALSE;
  }
  if(!_ensure_widget_cache(self))
  {
    _resume_worker(self, g->rt);
    return FALSE;
  }

  g->live_padding = _current_live_padding(self);
  _resume_worker(self, g->rt);
  return TRUE;
}

static void _prime_gui_process_patch_from_backbuffer(dt_iop_module_t *self,
                                                      const dt_dev_pixelpipe_iop_t *piece,
                                                      const dt_iop_roi_t *roi_in,
                                                      const dt_iop_roi_t *roi_out,
                                                      const dt_iop_drawlayer_params_t *runtime_params)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!self || !self->dev || !self->dev->gui_attached || !g || !piece || !roi_out || !runtime_params) return;
  if(!_is_drawlayer_display_pipe(self, piece)) return;

  /* process()/process_cl() necessarily compute a backbuffer. In GUI mode, cache
   * the ROI-scaled preview tile from that same run immediately. */
  if(!g->cache_valid) _ensure_layer_cache(self);
  if(!g->cache_valid) return;

  const int cache_ref_w = g->base_patch.width > 0 ? g->base_patch.width : piece->pipe->iwidth;
  const int cache_ref_h = g->base_patch.height > 0 ? g->base_patch.height : piece->pipe->iheight;
  if(!_layer_cache_matches(g, piece->pipe->image.id, cache_ref_w, cache_ref_h,
                           runtime_params->layer_name, runtime_params->layer_order))
    return;

  gboolean need_build = FALSE;
    dt_pthread_mutex_lock(&g->process_patch_mutex);
  dt_drawlayer_cache_patch_rdlock(&g->process_read_patch);
  need_build = !g->process_patch_valid || !g->process_read_patch.pixels;
    dt_drawlayer_cache_patch_rdunlock(&g->process_read_patch);
  dt_pthread_mutex_unlock(&g->process_patch_mutex);
  if(need_build)
    _build_process_patch_from_base(self, g, piece, roi_in, roi_out);
}

static gboolean _prime_live_process_patch_before_stroke(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!self || !self->dev || !self->dev->gui_attached || !g) return FALSE;

  gboolean need_build = FALSE;
  dt_pthread_mutex_lock(&g->process_patch_mutex);
  dt_drawlayer_cache_patch_rdlock(&g->process_read_patch);
  need_build = !g->process_patch_valid || !g->process_read_patch.pixels;
  dt_drawlayer_cache_patch_rdunlock(&g->process_read_patch);
  dt_pthread_mutex_unlock(&g->process_patch_mutex);
  if(!need_build) return TRUE;

  dt_dev_pixelpipe_t *pipes[] = { self->dev->pipe, self->dev->preview_pipe };
  for(size_t k = 0; k < G_N_ELEMENTS(pipes); k++)
  {
    dt_dev_pixelpipe_t *pipe = pipes[k];
    if(!pipe) continue;

    dt_dev_pixelpipe_iop_t *piece = dt_dev_distort_get_iop_pipe(self->dev, pipe, self);
    if(!piece || !_is_drawlayer_display_pipe(self, piece)) continue;

    if(_build_process_patch_from_base(self, g, piece, &piece->planned_roi_in, &piece->planned_roi_out))
      return TRUE;
  }

  return FALSE;
}

#ifdef HAVE_OPENCL
static void _prewarm_gui_process_patch_cl(dt_iop_module_t *self,
                                          const dt_dev_pixelpipe_iop_t *piece,
                                          const dt_iop_roi_t *roi_out,
                                          const dt_iop_drawlayer_global_data_t *gd,
                                          const gboolean use_preview_bg, const float preview_bg)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!self || !piece || !piece->pipe || !roi_out || !gd || !g) return;
  if(!self->dev || !self->dev->gui_attached) return;
  if(!_is_drawlayer_display_pipe(self, piece)) return;
  if(piece->pipe->devid < 0 || gd->kernel_premult_over < 0) return;
  if(roi_out->width <= 0 || roi_out->height <= 0) return;
    dt_pthread_mutex_lock(&g->process_patch_mutex);
  dt_drawlayer_cache_patch_rdlock(&g->process_read_patch);
  if(!g->process_patch_valid || !g->process_read_patch.pixels)
  {
      dt_drawlayer_cache_patch_rdunlock(&g->process_read_patch);
  dt_pthread_mutex_unlock(&g->process_patch_mutex);
    return;
  }
  if(g->process_cl_prewarm_hash == g->process_geom_hash
     && g->process_cl_prewarm_devid == piece->pipe->devid)
  {
      dt_drawlayer_cache_patch_rdunlock(&g->process_read_patch);
  dt_pthread_mutex_unlock(&g->process_patch_mutex);
    return;
  }

  drawlayer_process_scratch_t *scratch = _get_process_scratch();
  if(!scratch)
  {
      dt_drawlayer_cache_patch_rdunlock(&g->process_read_patch);
  dt_pthread_mutex_unlock(&g->process_patch_mutex);
    return;
  }

  cl_mem dev_in = dt_opencl_alloc_device(piece->pipe->devid, roi_out->width, roi_out->height, 4 * sizeof(float));
  cl_mem dev_out = dt_opencl_alloc_device(piece->pipe->devid, roi_out->width, roi_out->height, 4 * sizeof(float));
  if(!dev_in || !dev_out)
  {
    if(dev_in) dt_opencl_release_mem_object(dev_in);
    if(dev_out) dt_opencl_release_mem_object(dev_out);
      dt_drawlayer_cache_patch_rdunlock(&g->process_read_patch);
  dt_pthread_mutex_unlock(&g->process_patch_mutex);
    return;
  }

  dt_iop_roi_t source_process_roi = { 0 };
  dt_iop_roi_t blend_target_roi = { 0 };
  gboolean direct_copy = FALSE;
  if(!dt_drawlayer_cache_build_process_blend_rois(&g->process_read_patch, g->process_patch_padding, roi_out,
                                                  &blend_target_roi, &source_process_roi, &direct_copy))
  {
    dt_opencl_release_mem_object(dev_in);
    dt_opencl_release_mem_object(dev_out);
      dt_drawlayer_cache_patch_rdunlock(&g->process_read_patch);
  dt_pthread_mutex_unlock(&g->process_patch_mutex);
    return;
  }

  const gboolean warmed = _blend_layer_over_input_cl(piece->pipe->devid, gd->kernel_premult_over, dev_out, dev_in,
                                                     scratch, g->process_read_patch.pixels, g->process_read_patch.cache_entry, NULL,
                                                     g->process_read_patch.width, g->process_read_patch.height,
                                                     &blend_target_roi, &source_process_roi, direct_copy,
                                                     use_preview_bg, preview_bg,
                                                     dt_dev_pixelpipe_get_realtime(piece->pipe), FALSE);
  dt_opencl_release_mem_object(dev_in);
  dt_opencl_release_mem_object(dev_out);
    dt_drawlayer_cache_patch_rdunlock(&g->process_read_patch);
  dt_pthread_mutex_unlock(&g->process_patch_mutex);

  if(warmed)
  {
    g->process_cl_prewarm_hash = g->process_geom_hash;
    g->process_cl_prewarm_devid = piece->pipe->devid;
  }
}
#endif

static float _widget_brush_radius(dt_iop_module_t *self, const dt_drawlayer_brush_dab_t *dab,
                                  const float fallback)
{
  if(!self || !self->dev || !self->dev->virtual_pipe || !dab) return fallback;

  float pts[6] = {
    dab->x, dab->y,
    dab->x + dab->radius, dab->y,
    dab->x, dab->y + dab->radius,
  };

  if(!dt_dev_distort_transform_plus(self->dev, self->dev->virtual_pipe, self->iop_order,
                                    DT_DEV_TRANSFORM_DIR_FORW_EXCL, pts, 3))
    return fallback;

  dt_dev_coordinates_image_abs_to_image_norm(self->dev, pts, 3);
  dt_dev_coordinates_image_norm_to_widget(self->dev, pts, 3);

  const float rx = hypotf(pts[2] - pts[0], pts[3] - pts[1]);
  const float ry = hypotf(pts[4] - pts[0], pts[5] - pts[1]);
  const float radius = 0.5f * (rx + ry);
  return fmaxf(0.5f, isfinite(radius) ? radius : fallback);
}

static gboolean _layer_to_widget_coords(dt_iop_module_t *self, const float x, const float y, float *wx, float *wy)
{
  if(!self || !self->dev || !self->dev->virtual_pipe || !wx || !wy) return FALSE;

  int offset_x = 0;
  int offset_y = 0;
  _virtual_piece_input_offset(self, &offset_x, &offset_y);

  float pt[2] = { x - offset_x, y - offset_y };
  if(!dt_dev_distort_transform_plus(self->dev, self->dev->virtual_pipe, self->iop_order,
                                    DT_DEV_TRANSFORM_DIR_FORW_EXCL, pt, 1))
    return FALSE;

  dt_dev_coordinates_image_abs_to_image_norm(self->dev, pt, 1);
  dt_dev_coordinates_image_norm_to_widget(self->dev, pt, 1);
  *wx = pt[0];
  *wy = pt[1];
  return TRUE;
}

static void _touch_stroke_commit_hash(dt_iop_drawlayer_params_t *params, const int dab_count,
                                      const gboolean have_last_dab, const float last_dab_x,
                                      const float last_dab_y)
{
  if(!params) return;

  uint32_t x_bits = 0u;
  uint32_t y_bits = 0u;
  if(have_last_dab)
  {
    memcpy(&x_bits, &last_dab_x, sizeof(x_bits));
    memcpy(&y_bits, &last_dab_y, sizeof(y_bits));
  }

  const uint32_t seed[4] = {
    (uint32_t)dab_count,
    have_last_dab ? 1u : 0u,
    x_bits,
    y_bits
  };

  uint64_t hash = params->stroke_commit_hash ? params->stroke_commit_hash : 5381u;
  hash = dt_hash(hash, (const char *)seed, sizeof(seed));

  /* Keep the serialized field non-zero so "uninitialized" remains distinguishable
   * from "updated at least once" in legacy parameter blobs. */
  params->stroke_commit_hash = (uint32_t)(hash ? hash : 1u);
}

static void _sync_undo_redo_buttons(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  GMainContext *const ui_ctx = g_main_context_default();
  if(!g || !(ui_ctx && g_main_context_is_owner(ui_ctx))) return;

  if(g->undo_button) gtk_widget_set_sensitive(g->undo_button, g->undo_available && !g->painting);
  if(g->redo_button) gtk_widget_set_sensitive(g->redo_button, g->redo_available && !g->painting);
}

static void _sync_save_button(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  GMainContext *const ui_ctx = g_main_context_default();
  if(!g || !g->save_layer || !(ui_ctx && g_main_context_is_owner(ui_ctx))) return;

  /* The sidecar save action should stay available whenever we have an
   * authoritative layer cache in memory. Unlike undo/redo, it is useful while
   * the module is focused because it is the explicit persistence escape hatch
   * for users who do not want to rely on focus-loss or shutdown hooks. */
  gtk_widget_set_sensitive(g->save_layer, g->cache_valid && g->base_patch.pixels);
}

static void _refresh_layer_widgets(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  dt_iop_drawlayer_params_t *params = self ? (dt_iop_drawlayer_params_t *)self->params : NULL;
  GMainContext *const ui_ctx = g_main_context_default();
  if(!g || !params || !(ui_ctx && g_main_context_is_owner(ui_ctx))) return;

  if(g->layer_name)
  {
    gtk_entry_set_text(g->layer_name, params->layer_name);
  }

  if(g->layer_select) _populate_layer_list(self);
  if(g->create_background) gtk_widget_set_sensitive(g->create_background, !g->background_job_running);
  _sync_save_button(self);
}

static void _invalidate_undo_redo(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g) return;

  _clear_patch(&g->undo_patch);
  _clear_patch(&g->stroke_mask);
  g->undo_available = FALSE;
  g->redo_available = FALSE;
  _sync_undo_redo_buttons(self);
}

static gboolean _prepare_undo_snapshot(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !g->cache_valid || !g->base_patch.pixels) return FALSE;

  const size_t count = (size_t)g->base_patch.width * g->base_patch.height * 4;
  if(count == 0) return FALSE;

  if(g->undo_patch.width != g->base_patch.width || g->undo_patch.height != g->base_patch.height || !g->undo_patch.pixels)
  {
    const uint64_t undo_hash = g->base_patch.cache_hash
                                   ? dt_hash(g->base_patch.cache_hash, "undo", strlen("undo"))
                                   : _drawlayer_sidecar_cache_hash(g->cache_layer_name, g->cache_layer_name, "undo");
    if(!dt_drawlayer_cache_patch_alloc_shared(&g->undo_patch, undo_hash,
                                              (size_t)g->base_patch.width * g->base_patch.height,
                                              g->base_patch.width, g->base_patch.height,
                                              "drawlayer undo cache", NULL))
    {
      _clear_patch(&g->undo_patch);
      g->undo_available = FALSE;
      g->redo_available = FALSE;
      return FALSE;
    }
    g->undo_patch.x = g->base_patch.x;
    g->undo_patch.y = g->base_patch.y;
  }
  else
  {
    g->undo_patch.x = g->base_patch.x;
    g->undo_patch.y = g->base_patch.y;
  }

  dt_drawlayer_cache_patch_rdlock(&g->base_patch);
  memcpy(g->undo_patch.pixels, g->base_patch.pixels, count * sizeof(float));
  dt_drawlayer_cache_patch_rdunlock(&g->base_patch);

  const size_t mask_count = (size_t)g->base_patch.width * g->base_patch.height;
  if(g->stroke_mask.width != g->base_patch.width || g->stroke_mask.height != g->base_patch.height
     || !g->stroke_mask.pixels)
  {
    _clear_patch(&g->stroke_mask);
    g->stroke_mask.width = g->base_patch.width;
    g->stroke_mask.height = g->base_patch.height;
    g->stroke_mask.x = 0;
    g->stroke_mask.y = 0;
    g->stroke_mask.pixels = dt_drawlayer_cache_alloc_temp_buffer(mask_count * sizeof(float),
                                                                 "drawlayer stroke mask");
    g->stroke_mask.external_alloc = TRUE;
    if(!g->stroke_mask.pixels)
    {
      g->stroke_mask.width = 0;
      g->stroke_mask.height = 0;
      g->undo_available = TRUE;
      g->redo_available = FALSE;
      return TRUE;
    }
  }
  memset(g->stroke_mask.pixels, 0, mask_count * sizeof(float));

  g->undo_available = TRUE;
  g->redo_available = FALSE;

  _sync_undo_redo_buttons(self);
  return TRUE;
}

static gboolean _swap_undo_redo(dt_iop_module_t *self, const gboolean undo)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  dt_iop_drawlayer_params_t *params = self ? (dt_iop_drawlayer_params_t *)self->params : NULL;
  if(!self || !self->dev || !g || !params) return FALSE;

  if((undo && !g->undo_available) || (!undo && !g->redo_available)) return FALSE;
  if(!_commit_dabs(self, FALSE)) return FALSE;
  if(!_sync_temp_buffers(self, FALSE, FALSE)) return FALSE;
  if(!g->base_patch.pixels || !g->undo_patch.pixels) return FALSE;

  _release_all_base_patch_extra_refs(g);
  drawlayer_patch_t tmp = g->base_patch;
  g->base_patch = g->undo_patch;
  g->undo_patch = tmp;
  g->cache_valid = TRUE;
  g->cache_dirty = TRUE;
  _touch_layer_cache_epoch(g);
  _invalidate_process_patch(g);

  if(undo)
  {
    g->undo_available = FALSE;
    g->redo_available = TRUE;
  }
  else
  {
    g->undo_available = TRUE;
    g->redo_available = FALSE;
  }

  _touch_stroke_commit_hash(params, 0, FALSE, 0.0f, 0.0f);
  _sync_undo_redo_buttons(self);
  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
  return TRUE;
}

static void _sync_preview_bg_buttons(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g) return;

  if(GTK_IS_TOGGLE_BUTTON(g->preview_bg_image))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->preview_bg_image), g->preview_bg_mode == DRAWLAYER_PREVIEW_BG_IMAGE);
  if(GTK_IS_TOGGLE_BUTTON(g->preview_bg_white))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->preview_bg_white), g->preview_bg_mode == DRAWLAYER_PREVIEW_BG_WHITE);
  if(GTK_IS_TOGGLE_BUTTON(g->preview_bg_grey))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->preview_bg_grey), g->preview_bg_mode == DRAWLAYER_PREVIEW_BG_GREY);
  if(GTK_IS_TOGGLE_BUTTON(g->preview_bg_black))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->preview_bg_black), g->preview_bg_mode == DRAWLAYER_PREVIEW_BG_BLACK);
}

static gboolean _commit_dabs(dt_iop_module_t *self, const gboolean record_history)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  dt_iop_drawlayer_params_t *params = (dt_iop_drawlayer_params_t *)self->params;
  if(!g || !self->dev) return TRUE;
  if(record_history && g->painting)
  {
    dt_drawlayer_worker_request_commit(g->rt);
    return TRUE;
  }

  _set_drawlayer_pipeline_realtime_mode(self, FALSE);
  _cancel_async_commit(g->rt);

  /* Commit ordering is strict:
   * - the preview queue and backend queue workers must be idle,
   * - the layer cache must already contain the stroke,
   * - only then do we mutate params/history so the pipeline invalidation sees a coherent state. */
  _wait_worker_idle(self, g->rt);
  dt_drawlayer_worker_flush_pending(g->rt);
  if(!dt_drawlayer_worker_finished_stroke_queued(g->rt))
  {
    const dt_drawlayer_paint_stroke_t *stroke = dt_drawlayer_worker_stroke(g->rt);
    const GArray *history = dt_drawlayer_worker_history(g->rt);
    if(stroke && history)
      _replay_finished_stroke_to_base_patch(self, history, stroke->distance_percent);
  }
  /* Damage-rectangle ownership stays in drawlayer:
   * paint accumulates per-dab bounds into a stroke rectangle, and on commit the
   * module consumes that rectangle to update process/base dirty regions. */
  _publish_backend_stroke_damage(self);
  /* Do not resample the display-sized process tile back into `base_patch` at
   * stroke end. If the view geometry did not change, the current process tile
   * remains the highest-fidelity representation of the just-painted stroke.
   * Upsampling it into base and then immediately reusing base to rebuild the
   * same view adds avoidable memory traffic and visibly destroys high-frequency
   * brush detail (noise/sprinkles). Base synchronization is deferred
   * to geometry changes and explicit persistence points. */

  int sample_count = 0;
  gboolean had_stroke = FALSE;
  dt_iop_gui_enter_critical_section(self);
  g->finish_commit_pending = FALSE;
  sample_count = (int)g->stroke_sample_count;
  had_stroke = (sample_count > 0);
  _reset_stroke_session(g);
  dt_iop_gui_leave_critical_section(self);
  dt_drawlayer_worker_reset_stroke(g->rt);

  if(had_stroke)
  {
    /* Keep stroke commit lightweight for interactive rendering:
     * - do not flush process tile back to base patch here,
     * - do not invalidate the process tile here.
     *
     * Base-patch synchronization is handled at explicit persistence points
     * (save/focus-out/mouse-leave/geometry rebuild) so recomputes can keep
     * blending the already-updated process tile directly. */
    _touch_stroke_commit_hash(params, sample_count, g->last_dab_valid, g->last_dab_x, g->last_dab_y);
    _retain_base_patch_stroke_ref(g);
    g->last_dab_valid = FALSE;
    if(record_history && self->dev)
    {
      dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
      /* Stroke-end commits must request an actual pipe refresh, not only a
       * center redraw. Otherwise a late-finishing worker commit can leave the
       * display showing the previous processed state until some unrelated UI
       * event happens to trigger a recompute. */
      dt_dev_pixelpipe_refresh_all(self->dev, FALSE);
    }

    /* The final stroke-end raster batches may land after the button-release
     * event already requested one redraw. Queue one more redraw now that the
     * worker is idle and the last published process snapshot is coherent, so
     * late-finishing strokes become visible even without further UI activity. */
    if(self->dev && self->dev->gui_attached)
      dt_control_queue_redraw_center();
  }

  return TRUE;
}

static void _develop_ui_pipe_finished_callback(gpointer instance, gpointer user_data)
{
  (void)instance;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !self->dev) return;

  const gboolean view_changed = (fabsf(g->last_view_x - self->dev->roi.x) > 1e-6f
                                 || fabsf(g->last_view_y - self->dev->roi.y) > 1e-6f
                                 || fabsf(g->last_view_scale - self->dev->roi.scaling) > 1e-6f);
  const gboolean padding_changed = fabsf(g->live_padding - _current_live_padding(self)) > 1e-6f;
  if(!view_changed && !padding_changed) return;

  const gboolean worker_active = dt_drawlayer_worker_any_active(g->rt);

  /* Never flush or rebuild buffers from the pipe-finished callback while a stroke is active
   * or while the worker still has pending stroke work. Doing so races the asynchronous stroke
   * commit path and can block forever waiting for the worker from inside the callback. */
  if(g->painting || worker_active) return;

  if(!_sync_temp_buffers(self, FALSE, FALSE))
  {
    /* This callback performs a best-effort cache resync after a completed UI-pipe render.
     * A transient failure here does not mean the stroke failed; it usually means the
     * transformed view rectangle was temporarily unavailable during the pipe transition.
     * Consume the current ROI state so we do not spam the same warning after every stroke. */
    g->last_view_x = self->dev->roi.x;
    g->last_view_y = self->dev->roi.y;
    g->last_view_scale = self->dev->roi.scaling;
    g->live_padding = _current_live_padding(self);
  }
}

static void _sync_mode_sensitive_widgets(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !g->color || !g->softness) return;

  const gboolean paint_mode = (_conf_brush_mode() == DT_DRAWLAYER_BRUSH_MODE_PAINT);
  const gboolean show_hardness = (_conf_brush_shape() != DT_DRAWLAYER_BRUSH_SHAPE_GAUSSIAN);
  gtk_widget_set_visible(GTK_WIDGET(g->color), paint_mode);
  if(g->color_row) gtk_widget_set_visible(g->color_row, paint_mode);
  if(g->color_swatch) gtk_widget_set_visible(g->color_swatch, paint_mode);
  if(g->image_colorpicker) gtk_widget_set_visible(g->image_colorpicker, paint_mode);
  if(g->image_colorpicker_source) gtk_widget_set_visible(g->image_colorpicker_source, paint_mode);
  gtk_widget_set_visible(g->softness, show_hardness);
}

static gboolean _delete_current_layer(dt_iop_module_t *self)
{
  if(!self->dev) return FALSE;

  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  dt_iop_drawlayer_params_t *params = (dt_iop_drawlayer_params_t *)self->params;
  _ensure_layer_name(self, params);

  GString *errors = g_string_new(NULL);
  gboolean deleted = FALSE;

  char path[PATH_MAX] = { 0 };
  if(!dt_drawlayer_io_sidecar_path(self->dev->image_storage.id, path, sizeof(path)))
  {
    _layerio_append_error(errors, _("failed to resolve drawlayer sidecar path"));
  }
  else if(!g_file_test(path, G_FILE_TEST_EXISTS))
  {
    _layerio_append_error(errors, _("drawlayer sidecar TIFF is missing"));
  }
  else
  {
    drawlayer_dir_info_t info = { 0 };
    dt_drawlayer_io_layer_info_t io_info = { 0 };
    if(!dt_drawlayer_io_find_layer(path, params->layer_name, -1, &io_info))
    {
      _layerio_append_error(errors, _("drawlayer layer not found in sidecar TIFF"));
    }
    else
    {
      _from_io_info(&io_info, &info);
      if(!dt_drawlayer_io_store_layer(path, params->layer_name, info.index, NULL, NULL,
                                      self->dev->roi.raw_width, self->dev->roi.raw_height, TRUE, NULL))
      {
        _layerio_append_error(errors, _("failed to delete drawing layer from sidecar"));
      }
      else
      {
        deleted = TRUE;
        _default_layer_name(self, params->layer_name, sizeof(params->layer_name));
        params->layer_order = -1;
        params->sidecar_timestamp = 0;
        memset(params->work_profile, 0, sizeof(params->work_profile));
        if(g)
        {
          _release_all_base_patch_extra_refs(g);
          _clear_patch(&g->base_patch);
          g->cache_valid = FALSE;
          g->cache_dirty = FALSE;
          g->cache_layer_name[0] = '\0';
          g->cache_layer_order = -1;
          _reset_stroke_session(g);
        }
        _refresh_layer_widgets(self);
      }
    }
  }

  _layerio_log_errors(errors);
  g_string_free(errors, TRUE);
  return deleted;
}

static gboolean _confirm_delete_layer(dt_iop_module_t *self, const gboolean removing_module)
{
  if(!self->dev) return FALSE;

  dt_iop_drawlayer_params_t *params = (dt_iop_drawlayer_params_t *)self->params;
  _ensure_layer_name(self, params);

  GtkWidget *dialog = gtk_message_dialog_new(
      GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)),
      GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
      GTK_MESSAGE_QUESTION,
      GTK_BUTTONS_NONE,
      "%s",
      removing_module
          ? _("Delete the linked drawing layer from the sidecar TIFF before removing this module instance?")
          : _("Delete the linked drawing layer from the sidecar TIFF?"));
  if(removing_module)
  {
    gtk_dialog_add_button(GTK_DIALOG(dialog), _("Keep layer"), GTK_RESPONSE_NO);
    gtk_dialog_add_button(GTK_DIALOG(dialog), _("Delete layer"), GTK_RESPONSE_YES);
  }
  else
  {
    gtk_dialog_add_button(GTK_DIALOG(dialog), _("Cancel"), GTK_RESPONSE_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(dialog), _("Delete"), GTK_RESPONSE_ACCEPT);
  }

  const int response = gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);

  if(removing_module)
  {
    if(response == GTK_RESPONSE_YES) _delete_current_layer(self);
    /* Module removal must proceed regardless of the answer or any layer-delete failure. */
    return TRUE;
  }

  if(response != GTK_RESPONSE_ACCEPT) return FALSE;
  return _delete_current_layer(self);
}

static gboolean _rename_current_layer_from_gui(dt_iop_module_t *self, const char *requested_name)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  dt_iop_drawlayer_params_t *params = self ? (dt_iop_drawlayer_params_t *)self->params : NULL;
  if(!self || !self->dev || !g || !params) return FALSE;

  char new_name[DRAWLAYER_NAME_SIZE] = { 0 };
  char stripped_requested[DRAWLAYER_NAME_SIZE] = { 0 };
  if(requested_name) g_strlcpy(stripped_requested, requested_name, sizeof(stripped_requested));
  g_strstrip(stripped_requested);
  if(stripped_requested[0] == '\0') return FALSE;
  _sanitize_requested_layer_name(self, requested_name, new_name, sizeof(new_name));
  if(new_name[0] == '\0') return FALSE;

  if(!g_strcmp0(new_name, params->layer_name))
  {
    if(g_strcmp0(gtk_entry_get_text(g->layer_name), new_name))
    {
      gtk_entry_set_text(g->layer_name, new_name);
    }
    return TRUE;
  }

  GString *errors = g_string_new(NULL);
  gboolean renamed = FALSE;

  if(!_commit_dabs(self, FALSE))
    _layerio_append_error(errors, _("failed to commit drawing stroke before renaming"));
  else if(!_flush_layer_cache(self))
    _layerio_append_error(errors, _("failed to write drawing layer sidecar"));
  else
  {
    char path[PATH_MAX] = { 0 };
    if(!dt_drawlayer_io_sidecar_path(self->dev->image_storage.id, path, sizeof(path)))
      _layerio_append_error(errors, _("failed to resolve drawlayer sidecar path"));
    else if(!g_file_test(path, G_FILE_TEST_EXISTS))
      _layerio_append_error(errors, _("drawlayer sidecar TIFF is missing"));
    else
    {
      drawlayer_dir_info_t info = { 0 };
      dt_drawlayer_io_layer_info_t io_info = { 0 };
      if(!dt_drawlayer_io_find_layer(path, params->layer_name, -1, &io_info))
      {
        _layerio_append_error(errors, _("drawlayer layer not found in sidecar TIFF"));
      }
      else
      {
        _from_io_info(&io_info, &info);
        if(dt_drawlayer_io_layer_name_exists(path, new_name, info.index))
        {
          _layerio_append_error(errors, _("drawlayer layer name already exists"));
        }
        else
        {
          int final_order = info.index;
          if(!dt_drawlayer_io_store_layer(path, new_name, info.index, params->work_profile, NULL,
                                          self->dev->roi.raw_width, self->dev->roi.raw_height, FALSE,
                                          &final_order))
          {
            _layerio_append_error(errors, _("failed to rename drawing layer in sidecar"));
          }
          else
          {
            g_strlcpy(params->layer_name, new_name, sizeof(params->layer_name));
            params->layer_order = final_order;
            params->sidecar_timestamp = _sidecar_timestamp_from_path(path);
            g_strlcpy(g->cache_layer_name, params->layer_name, sizeof(g->cache_layer_name));
            g->cache_layer_order = params->layer_order;
            renamed = TRUE;
          }
        }
      }
    }
  }

  if(renamed)
  {
    g->missing_layer_prompt_name[0] = '\0';
    _refresh_layer_widgets(self);
    if(self->dev) dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
  }
  else
  {
    _refresh_layer_widgets(self);
  }

  _layerio_log_errors(errors);
  g_string_free(errors, TRUE);
  return renamed;
}

static gboolean _create_new_layer(dt_iop_module_t *self, const char *requested_name)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  dt_iop_drawlayer_params_t *params = self ? (dt_iop_drawlayer_params_t *)self->params : NULL;
  if(!self || !self->dev || !g || !params) return FALSE;

  char new_name[DRAWLAYER_NAME_SIZE] = { 0 };
  char stripped_requested[DRAWLAYER_NAME_SIZE] = { 0 };
  if(requested_name) g_strlcpy(stripped_requested, requested_name, sizeof(stripped_requested));
  g_strstrip(stripped_requested);
  if(stripped_requested[0] == '\0') return FALSE;
  _sanitize_requested_layer_name(self, requested_name, new_name, sizeof(new_name));
  if(new_name[0] == '\0') return FALSE;

  const dt_iop_drawlayer_params_t previous = *params;

  if(!_commit_dabs(self, FALSE)) return FALSE;
  if(!_flush_layer_cache(self)) return FALSE;

  g_strlcpy(params->layer_name, new_name, sizeof(params->layer_name));
  params->layer_order = -1;
  params->sidecar_timestamp = 0;
  memset(params->work_profile, 0, sizeof(params->work_profile));

  if(!_sync_temp_buffers(self, FALSE, FALSE))
  {
    *params = previous;
    gui_update(self);
    return FALSE;
  }

  g->missing_layer_prompt_name[0] = '\0';
  _touch_stroke_commit_hash(params, 0, FALSE, 0.0f, 0.0f);
  if(self->dev)
  {
    dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
    dt_dev_pixelpipe_refresh_all(self->dev, FALSE);
  }
  _refresh_layer_widgets(self);
  gui_update(self);
  return TRUE;
}

typedef struct drawlayer_tiff_export_params_t
{
  dt_imageio_module_data_t global;
  int bpp;
  int compress;
  int compresslevel;
  int shortfile;
} drawlayer_tiff_export_params_t;

typedef struct drawlayer_background_job_params_t
{
  int32_t imgid;
  int raw_width;
  int raw_height;
  int dst_x;
  int dst_y;
  int insert_after_order;
  char sidecar_path[PATH_MAX];
  char work_profile[DRAWLAYER_PROFILE_SIZE];
  char requested_bg_name[DRAWLAYER_NAME_SIZE];
  char filter[64];
  char initiator_layer_name[DRAWLAYER_NAME_SIZE];
  int initiator_layer_order;
} drawlayer_background_job_params_t;

typedef struct drawlayer_background_job_result_t
{
  gboolean success;
  int32_t imgid;
  int64_t sidecar_timestamp;
  char created_bg_name[DRAWLAYER_NAME_SIZE];
  char initiator_layer_name[DRAWLAYER_NAME_SIZE];
  int initiator_layer_order;
  char message[DT_CONTROL_DESCRIPTION_LEN];
} drawlayer_background_job_result_t;

static void _build_pre_module_filter_string(dt_iop_module_t *self, char *filter, const size_t filter_size)
{
  if(!filter || filter_size == 0)
    return;
  filter[0] = '\0';
  if(!self || !self->dev || !self->dev->pipe) return;

  dt_dev_pixelpipe_iop_t *self_piece = dt_dev_distort_get_iop_pipe(self->dev, self->dev->pipe, self);
  const char *prev_op = NULL;
  if(self_piece && self->dev->pipe)
  {
    for(GList *nodes = self->dev->pipe->nodes; nodes; nodes = g_list_next(nodes))
    {
      dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
      if(piece == self_piece) break;
      if(piece->enabled && piece->module && piece->module->op[0]) prev_op = piece->module->op;
    }
  }
  if(prev_op && prev_op[0]) g_snprintf(filter, filter_size, "pre:%s", prev_op);
}

static gboolean _export_pre_module_fullres_to_tiff(const int32_t imgid, const char *filter, const char *path)
{
  if(imgid <= 0 || !path || path[0] == '\0') return FALSE;

  dt_imageio_module_format_t *format = dt_imageio_get_format_by_name("tiff");
  if(!format) return FALSE;

  dt_imageio_module_data_t *format_params = format->get_params(format);
  if(!format_params) return FALSE;

  format_params->max_width = 0;
  format_params->max_height = 0;
  format_params->width = 0;
  format_params->height = 0;
  format_params->style[0] = '\0';

  if(format->params_size(format) >= sizeof(drawlayer_tiff_export_params_t))
  {
    drawlayer_tiff_export_params_t *const tiff_params = (drawlayer_tiff_export_params_t *)format_params;
    tiff_params->bpp = 32;
    tiff_params->compress = 0;
    tiff_params->compresslevel = 0;
    tiff_params->shortfile = 0;
  }

  const int rc = dt_imageio_export_with_flags(imgid,
                                              path,
                                              format,
                                              format_params,
                                              TRUE,
                                              FALSE,
                                              TRUE,
                                              FALSE,
                                              FALSE,
                                              (filter && filter[0]) ? filter : NULL,
                                              FALSE,
                                              FALSE,
                                              DT_COLORSPACE_NONE,
                                              NULL,
                                              DT_INTENT_PERCEPTUAL,
                                              NULL,
                                              NULL,
                                              0,
                                              0,
                                              NULL);

  format->free_params(format, format_params);
  return rc == 0;
}

static int32_t _background_layer_job_run(dt_job_t *job)
{
  const drawlayer_background_job_params_t *params = (const drawlayer_background_job_params_t *)dt_control_job_get_params(job);
  if(!params) return 0;

  drawlayer_background_job_result_t *result = g_new0(drawlayer_background_job_result_t, 1);
  result->imgid = params->imgid;
  g_strlcpy(result->initiator_layer_name, params->initiator_layer_name, sizeof(result->initiator_layer_name));
  result->initiator_layer_order = params->initiator_layer_order;
  g_strlcpy(result->message, _("failed to create background layer from input"), sizeof(result->message));

  gchar *tmp_path = NULL;
  const int tmp_fd = g_file_open_tmp("ansel-drawlayer-bg-XXXXXX.tiff", &tmp_path, NULL);
  if(tmp_fd >= 0) g_close(tmp_fd, NULL);

  float *export_pixels = NULL;
  int export_w = 0;
  int export_h = 0;
  drawlayer_patch_t bg_patch = { 0 };

  do
  {
    if(tmp_fd < 0 || !tmp_path) break;
    if(!_export_pre_module_fullres_to_tiff(params->imgid, params->filter, tmp_path)) break;
    if(!dt_drawlayer_io_load_flat_rgba(tmp_path, &export_pixels, &export_w, &export_h)) break;
    if(!export_pixels || export_w <= 0 || export_h <= 0) break;

    bg_patch.width = params->raw_width;
    bg_patch.height = params->raw_height;
    bg_patch.x = 0;
    bg_patch.y = 0;
    bg_patch.pixels = dt_drawlayer_cache_alloc_temp_buffer((size_t)params->raw_width * params->raw_height * 4
                                                           * sizeof(float),
                                                           "drawlayer bg layer");
    if(!bg_patch.pixels) break;
    dt_drawlayer_cache_clear_transparent_float(bg_patch.pixels, (size_t)params->raw_width * params->raw_height);

    const int clip_x0 = MAX(params->dst_x, 0);
    const int clip_y0 = MAX(params->dst_y, 0);
    const int clip_x1 = MIN(params->dst_x + export_w, params->raw_width);
    const int clip_y1 = MIN(params->dst_y + export_h, params->raw_height);
    if(clip_x1 <= clip_x0 || clip_y1 <= clip_y0) break;

    const int copy_w = clip_x1 - clip_x0;
    const int copy_h = clip_y1 - clip_y0;
    const int src_x0 = clip_x0 - params->dst_x;
    const int src_y0 = clip_y0 - params->dst_y;
    for(int y = 0; y < copy_h; y++)
    {
      const float *src = export_pixels + 4 * ((size_t)(src_y0 + y) * export_w + src_x0);
      float *dst = bg_patch.pixels + 4 * ((size_t)(clip_y0 + y) * params->raw_width + clip_x0);
      memcpy(dst, src, (size_t)copy_w * 4 * sizeof(float));
    }

    char bg_name[DRAWLAYER_NAME_SIZE] = { 0 };
    dt_drawlayer_io_make_unique_name_plain(params->sidecar_path, params->requested_bg_name, bg_name, sizeof(bg_name));
    if(!_layer_name_non_empty(bg_name)) break;

    int final_order = -1;
    dt_drawlayer_io_patch_t io_patch = { 0 };
    _to_io_patch(&bg_patch, &io_patch);
    if(!dt_drawlayer_io_insert_layer(params->sidecar_path, bg_name, params->insert_after_order,
                                     params->work_profile, &io_patch,
                                     params->raw_width, params->raw_height, &final_order))
      break;

    drawlayer_dir_info_t created_info = { 0 };
    dt_drawlayer_io_layer_info_t io_info = { 0 };
    if(!dt_drawlayer_io_find_layer(params->sidecar_path, bg_name, final_order, &io_info)) break;
    _from_io_info(&io_info, &created_info);

    result->success = TRUE;
    result->sidecar_timestamp = _sidecar_timestamp_from_path(params->sidecar_path);
    g_strlcpy(result->created_bg_name, bg_name, sizeof(result->created_bg_name));
    g_snprintf(result->message, sizeof(result->message), _("created background layer `%s'"), bg_name);
  } while(0);

  if(export_pixels)
  {
    dt_free(export_pixels);
  }
  if(bg_patch.pixels) dt_drawlayer_cache_free_temp_buffer((void **)&bg_patch.pixels, "drawlayer bg layer");
  if(tmp_path)
  {
    g_unlink(tmp_path);
    dt_free(tmp_path);
  }

  g_main_context_invoke(NULL, _background_layer_job_done_idle, result);
  return 0;
}

static gboolean _background_layer_job_done_idle(gpointer user_data)
{
  drawlayer_background_job_result_t *result = (drawlayer_background_job_result_t *)user_data;
  if(!result) return G_SOURCE_REMOVE;

  dt_control_log("%s", result->message[0] ? result->message : _("background layer job finished"));

  gboolean cleared_initiator = FALSE;
  if(darktable.develop && darktable.develop->image_storage.id == result->imgid)
  {
    for(GList *modules = darktable.develop->iop; modules; modules = g_list_next(modules))
    {
      dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
      if(!module || g_strcmp0(module->op, "drawlayer")) continue;

      dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)module->gui_data;
      dt_iop_drawlayer_params_t *params = (dt_iop_drawlayer_params_t *)module->params;
      if(g && params
         && !g_strcmp0(params->layer_name, result->initiator_layer_name)
         && params->layer_order == result->initiator_layer_order)
      {
        g->background_job_running = FALSE;
        if(g->create_background) gtk_widget_set_sensitive(g->create_background, TRUE);
        cleared_initiator = TRUE;
      }

      if(result->success && params)
      {
        params->sidecar_timestamp = result->sidecar_timestamp;
        _refresh_layer_widgets(module);
      }
    }
  }

  if(!cleared_initiator && darktable.develop)
  {
    for(GList *modules = darktable.develop->iop; modules; modules = g_list_next(modules))
    {
      dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
      if(!module || g_strcmp0(module->op, "drawlayer")) continue;
      dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)module->gui_data;
      if(!g || !g->background_job_running) continue;
      g->background_job_running = FALSE;
      if(g->create_background) gtk_widget_set_sensitive(g->create_background, TRUE);
    }
  }

  dt_free(result);
  return G_SOURCE_REMOVE;
}

static gboolean _create_background_layer_from_input(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  dt_iop_drawlayer_params_t *params = self ? (dt_iop_drawlayer_params_t *)self->params : NULL;
  if(!self || !self->dev || !g || !params) return FALSE;
  if(g->background_job_running) return FALSE;
  _ensure_layer_name(self, params);
  if(!_layer_name_non_empty(params->layer_name)) return FALSE;

  if(!_commit_dabs(self, FALSE)) return FALSE;
  if(!_ensure_layer_cache(self)) return FALSE;
  if(!_flush_layer_cache(self)) return FALSE;

  const int raw_width = self->dev->roi.raw_width;
  const int raw_height = self->dev->roi.raw_height;
  if(raw_width <= 0 || raw_height <= 0) return FALSE;

  char sidecar_path[PATH_MAX] = { 0 };
  if(!dt_drawlayer_io_sidecar_path(self->dev->image_storage.id, sidecar_path, sizeof(sidecar_path))) return FALSE;

  drawlayer_dir_info_t current_info = { 0 };
  dt_drawlayer_io_layer_info_t io_info = { 0 };
  if(!dt_drawlayer_io_find_layer(sidecar_path, params->layer_name, params->layer_order, &io_info))
    return FALSE;
  _from_io_info(&io_info, &current_info);

  int dst_x = 0;
  int dst_y = 0;
  dt_dev_pixelpipe_iop_t *piece = dt_dev_distort_get_iop_pipe(self->dev, self->dev->pipe, self);
  if(piece)
  {
    dst_x = piece->buf_in.x;
    dst_y = piece->buf_in.y;
  }

  drawlayer_background_job_params_t *job_params = g_new0(drawlayer_background_job_params_t, 1);
  job_params->imgid = self->dev->image_storage.id;
  job_params->raw_width = raw_width;
  job_params->raw_height = raw_height;
  job_params->dst_x = dst_x;
  job_params->dst_y = dst_y;
  job_params->insert_after_order = current_info.index;
  g_strlcpy(job_params->sidecar_path, sidecar_path, sizeof(job_params->sidecar_path));
  g_strlcpy(job_params->work_profile, params->work_profile, sizeof(job_params->work_profile));
  g_snprintf(job_params->requested_bg_name, sizeof(job_params->requested_bg_name), "%s-bg", params->layer_name);
  g_strlcpy(job_params->initiator_layer_name, params->layer_name, sizeof(job_params->initiator_layer_name));
  job_params->initiator_layer_order = params->layer_order;
  _build_pre_module_filter_string(self, job_params->filter, sizeof(job_params->filter));

  dt_job_t *job = dt_control_job_create(_background_layer_job_run, "drawlayer create background layer");
  if(!job)
  {
    dt_free(job_params);
    return FALSE;
  }

  dt_control_job_set_params(job, job_params, g_free);
  dt_control_job_add_progress(job, _("creating background layer"), TRUE);
  g->background_job_running = TRUE;
  if(g->create_background) gtk_widget_set_sensitive(g->create_background, FALSE);
  if(dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_BG, job) != 0)
  {
    g->background_job_running = FALSE;
    if(g->create_background) gtk_widget_set_sensitive(g->create_background, TRUE);
    return FALSE;
  }
  return TRUE;
}

static int _offer_missing_layer_recreation(dt_iop_module_t *self, const char *missing_name)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!self || !self->dev || !g) return FALSE;
  if(self->dev->gui_module != self) return 0;

  if(g->missing_layer_prompt_name[0] != '\0'
     && !g_strcmp0(g->missing_layer_prompt_name, missing_name ? missing_name : ""))
    return 1;

  g_strlcpy(g->missing_layer_prompt_name, missing_name ? missing_name : "", sizeof(g->missing_layer_prompt_name));

  GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)),
                                             GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
                                             GTK_MESSAGE_WARNING,
                                             GTK_BUTTONS_NONE,
                                             "%s",
                                             _("The linked drawing layer was not found in the sidecar TIFF."));
  gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
                                           "%s",
                                           _("Do you want to create a new layer for this module now?"));
  gtk_dialog_add_button(GTK_DIALOG(dialog), _("Keep module as no-op"), GTK_RESPONSE_NO);
  gtk_dialog_add_button(GTK_DIALOG(dialog), _("Create new layer"), GTK_RESPONSE_YES);

  const int response = gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);

  if(response == GTK_RESPONSE_YES) return 2;
  return 1;
}

static gboolean _current_layer_missing_in_sidecar(dt_iop_module_t *self)
{
  dt_iop_drawlayer_params_t *params = self ? (dt_iop_drawlayer_params_t *)self->params : NULL;
  if(!self || !self->dev || !params || params->layer_name[0] == '\0') return FALSE;

  char path[PATH_MAX] = { 0 };
  if(!dt_drawlayer_io_sidecar_path(self->dev->image_storage.id, path, sizeof(path))) return FALSE;
  if(!g_file_test(path, G_FILE_TEST_EXISTS)) return FALSE;

  dt_drawlayer_io_layer_info_t info = { 0 };
  return !dt_drawlayer_io_find_layer(path, params->layer_name, -1, &info);
}

static void _widget_changed(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  if(!g || (darktable.gui && darktable.gui->reset)) return;

  if(widget == GTK_WIDGET(g->layer_name))
  {
    _rename_current_layer_from_gui(self, gtk_entry_get_text(g->layer_name));
    return;
  }

  _sync_params_from_gui(self, FALSE);

  if(widget == g->brush_mode || widget == g->brush_shape) _sync_mode_sensitive_widgets(self);

  if(widget == g->size || widget == g->softness || widget == g->brush_shape)
    _sync_temp_buffers(self, TRUE, FALSE);

  if(widget == g->brush_shape || widget == g->opacity || widget == g->softness || widget == g->sprinkles
     || widget == g->sprinkle_size || widget == g->sprinkle_coarseness)
    _sync_brush_profile_preview_widget(self);
}

static void _layer_selected(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  dt_iop_drawlayer_params_t *params = (dt_iop_drawlayer_params_t *)self->params;
  if(!g ||  (darktable.gui && darktable.gui->reset)) return;

  const int active = dt_bauhaus_combobox_get(widget);
  if(active < 0) return;

  const char *text = dt_bauhaus_combobox_get_text(g->layer_select);
  if(!text) return;

  gtk_entry_set_text(g->layer_name, text);

  char previous_name[DRAWLAYER_NAME_SIZE] = { 0 };
  g_strlcpy(previous_name, params->layer_name, sizeof(previous_name));
  const int previous_order = params->layer_order;
  g_strlcpy(params->layer_name, text, sizeof(params->layer_name));
  params->layer_order = active;
  if(!_sync_temp_buffers(self, TRUE, FALSE))
  {
    g_strlcpy(params->layer_name, previous_name, sizeof(params->layer_name));
    params->layer_order = previous_order;
    gui_update(self);
    return;
  }

  g->missing_layer_prompt_name[0] = '\0';
  _touch_stroke_commit_hash(params, 0, FALSE, 0.0f, 0.0f);
  if(self->dev)
  {
    dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
    dt_dev_pixelpipe_refresh_all(self->dev, FALSE);
  }
  _refresh_layer_widgets(self);
}

static void _delete_layer_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(!self->dev) return;
  if(!_commit_dabs(self, FALSE)) return;
  if(!_confirm_delete_layer(self, FALSE)) return;

  dt_iop_drawlayer_params_t *params = (dt_iop_drawlayer_params_t *)self->params;
  _invalidate_undo_redo(self);
  _touch_stroke_commit_hash(params, 0, FALSE, 0.0f, 0.0f);
  self->enabled = FALSE;
  dt_iop_gui_set_enable_button(self);
  if(self->dev) dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
  _refresh_layer_widgets(self);
  gui_update(self);
}

static gboolean _fill_current_layer(dt_iop_module_t *self, const float value)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  dt_iop_drawlayer_params_t *params = self ? (dt_iop_drawlayer_params_t *)self->params : NULL;
  if(!self || !self->dev || !g || !params) return FALSE;

  if(!_commit_dabs(self, FALSE)) return FALSE;
  if(!_sync_temp_buffers(self, FALSE, FALSE)) return FALSE;
  if(!g->base_patch.pixels) return FALSE;

  const size_t count = (size_t)g->base_patch.width * g->base_patch.height;
  dt_drawlayer_cache_patch_wrlock(&g->base_patch);
  for(size_t k = 0; k < count; k++)
  {
    float *pixel = g->base_patch.pixels + 4 * k;
    pixel[0] = _clamp01(value);
    pixel[1] = _clamp01(value);
    pixel[2] = _clamp01(value);
    pixel[3] = 1.0f;
  }
  dt_drawlayer_cache_patch_wrunlock(&g->base_patch);

  g->cache_dirty = TRUE;
  _touch_layer_cache_epoch(g);
  _invalidate_process_patch(g);
  _invalidate_undo_redo(self);
  _touch_stroke_commit_hash(params, 0, FALSE, 0.0f, 0.0f);
  _reset_stroke_session(g);

  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
  return TRUE;
}

static gboolean _clear_current_layer(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  dt_iop_drawlayer_params_t *params = self ? (dt_iop_drawlayer_params_t *)self->params : NULL;
  if(!self || !self->dev || !g || !params) return FALSE;

  if(!_commit_dabs(self, FALSE)) return FALSE;
  if(!_sync_temp_buffers(self, FALSE, FALSE)) return FALSE;
  if(!g->base_patch.pixels) return FALSE;

  dt_drawlayer_cache_patch_wrlock(&g->base_patch);
  dt_drawlayer_cache_clear_transparent_float(g->base_patch.pixels, (size_t)g->base_patch.width * g->base_patch.height);
  dt_drawlayer_cache_patch_wrunlock(&g->base_patch);

  g->cache_dirty = TRUE;
  _touch_layer_cache_epoch(g);
  _invalidate_process_patch(g);
  _invalidate_undo_redo(self);
  _touch_stroke_commit_hash(params, 0, FALSE, 0.0f, 0.0f);
  _reset_stroke_session(g);

  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
  return TRUE;
}

static void _fill_white_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(!_fill_current_layer(self, 1.0f)) dt_control_log(_("failed to fill drawing layer"));
}

static void _fill_black_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(!_fill_current_layer(self, 0.0f)) dt_control_log(_("failed to fill drawing layer"));
}

static void _fill_transparent_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(!_clear_current_layer(self)) dt_control_log(_("failed to clear drawing layer"));
}

static void _save_layer_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!self || !self->dev || !g) return;

  if(!g->cache_valid || !g->base_patch.pixels)
  {
    dt_control_log(_("no drawing layer is loaded in memory"));
    _sync_save_button(self);
    return;
  }
  if(!_layer_name_non_empty(g->cache_layer_name))
  {
    dt_control_log(_("layer name is empty, sidecar save aborted"));
    _refresh_layer_widgets(self);
    return;
  }

  GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)),
                                             GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
                                             GTK_MESSAGE_QUESTION,
                                             GTK_BUTTONS_NONE,
                                             "%s",
                                             _("Save the drawing sidecar now?"));
  gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
                                           "%s",
                                           _("This writes the current in-memory drawing layer to the sidecar TIFF immediately."));
  gtk_dialog_add_button(GTK_DIALOG(dialog), _("Cancel"), GTK_RESPONSE_CANCEL);
  gtk_dialog_add_button(GTK_DIALOG(dialog), _("Save"), GTK_RESPONSE_ACCEPT);

  const int response = gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
  if(response != GTK_RESPONSE_ACCEPT)
  {
    _sync_save_button(self);
    return;
  }

  /* Saving the sidecar is explicit persistence, not a history edit.
   * We still finalize any pending stroke first so the flush sees the latest
   * authoritative cache state, then write that cache to the TIFF immediately. */
  if(!_commit_dabs(self, FALSE))
  {
    dt_control_log(_("failed to finalize drawing stroke before saving sidecar"));
    _sync_save_button(self);
    return;
  }

  _flush_process_patch_to_base(self, g);
  _rekey_shared_base_patch(&g->base_patch, self->dev->image_storage.id,
                           (const dt_iop_drawlayer_params_t *)self->params);

  if(!_flush_layer_cache(self))
  {
    dt_control_log(_("failed to write drawing layer sidecar"));
    _sync_save_button(self);
    return;
  }

  _release_all_base_patch_extra_refs(g);
  _refresh_layer_widgets(self);

  dt_control_log(_("drawing layer sidecar saved"));

  GtkWidget *done = gtk_message_dialog_new(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)),
                                           GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
                                           GTK_MESSAGE_INFO,
                                           GTK_BUTTONS_OK,
                                           "%s",
                                           _("Drawing sidecar saved."));
  gtk_dialog_run(GTK_DIALOG(done));
  gtk_widget_destroy(done);
}

static void _create_layer_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!self || !g) return;
  if(!_create_new_layer(self, gtk_entry_get_text(g->layer_name)))
    dt_control_log(_("failed to create drawing layer"));
}

static void _create_background_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(!_create_background_layer_from_input(self))
    dt_control_log(_("failed to create background layer from input"));
}

static void _undo_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(!_swap_undo_redo(self, TRUE)) dt_control_log(_("failed to undo drawing stroke"));
}

static void _redo_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(!_swap_undo_redo(self, FALSE)) dt_control_log(_("failed to redo drawing stroke"));
}

static void _preview_bg_toggled(GtkToggleButton *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  dt_iop_drawlayer_params_t *params = self ? (dt_iop_drawlayer_params_t *)self->params : NULL;
  if(!self || !self->dev || !g || (darktable.gui && darktable.gui->reset)
     || !gtk_toggle_button_get_active(button)) return;

  if(GTK_WIDGET(button) == g->preview_bg_white)
    g->preview_bg_mode = DRAWLAYER_PREVIEW_BG_WHITE;
  else if(GTK_WIDGET(button) == g->preview_bg_grey)
    g->preview_bg_mode = DRAWLAYER_PREVIEW_BG_GREY;
  else if(GTK_WIDGET(button) == g->preview_bg_black)
    g->preview_bg_mode = DRAWLAYER_PREVIEW_BG_BLACK;
  else
    g->preview_bg_mode = DRAWLAYER_PREVIEW_BG_IMAGE;

  _sync_preview_bg_buttons(self);
  if(params) _touch_stroke_commit_hash(params, 0, FALSE, 0.0f, 0.0f);
  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
  dt_dev_pixelpipe_refresh_all(self->dev, FALSE);
}

static gboolean _append_dab_sample(dt_iop_module_t *self, const double wx, const double wy, const double pressure)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  if(!g) return FALSE;
  dt_control_pointer_input_t pointer_input = { 0 };
  dt_control_get_pointer_input(&pointer_input);
  const float input_wx = isfinite(pointer_input.x) ? (float)pointer_input.x : (float)wx;
  const float input_wy = isfinite(pointer_input.y) ? (float)pointer_input.y : (float)wy;
  const float pressure_norm = pointer_input.has_pressure ? _clamp01(pointer_input.pressure) : _clamp01(pressure);
  dt_drawlayer_paint_raw_input_t input = {
    .wx = input_wx,
    .wy = input_wy,
    .pressure = pressure_norm,
    .tilt = (float)_clamp01(pointer_input.tilt),
    .acceleration = (float)_clamp01(pointer_input.acceleration),
    .event_ts = g_get_monotonic_time(),
    .stroke_batch = g->current_stroke_batch,
    .event_index = ++g->stroke_event_index,
    .stroke_pos = DT_DRAWLAYER_PAINT_STROKE_MIDDLE,
  };
  _fill_input_brush_settings(self, &input);

  /* Middle-dab enqueue failures are expected under transient pressure and are
   * already tracked/throttled in drawlayer/worker.c. Avoid per-sample UI logging here. */
  return dt_drawlayer_worker_enqueue_input(g->rt, &input);
}

/** @brief Module display name. */
const char *name()
{
  return C_("modulename", "drawing");
}

/** @brief Module description strings used by UI/help. */
const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("paint premultiplied RGB layers in a TIFF sidecar"),
                                      _("creative"),
                                      _("linear, RGB, scene-referred"),
                                      _("geometric, RGB"),
                                      _("linear, RGB, scene-referred"));
}

#ifdef HAVE_OPENCL
/** @brief Initialize global OpenCL resources for drawlayer kernels. */
void init_global(dt_iop_module_so_t *module)
{
  const int program = 3; // blendop.cl, from programs.conf
  dt_iop_drawlayer_global_data_t *gd = calloc(1, sizeof(*gd));
  module->data = gd;
  if(!gd) return;
  gd->kernel_premult_over = -1;

  /* Reuse the existing blendop OpenCL program and add one drawlayer-specific
   * kernel to handle premultiplied "over" directly. This avoids the costly
   * de-premultiply + mask split that the stock blend kernels would require. */
  gd->kernel_premult_over = dt_opencl_create_kernel(program, "blendop_premult_over");
}

/** @brief Release global OpenCL resources for drawlayer. */
void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_drawlayer_global_data_t *gd = (dt_iop_drawlayer_global_data_t *)module->data;
  if(!gd) return;
  dt_opencl_free_kernel(gd->kernel_premult_over);
  dt_free(gd);
  module->data = NULL;
}
#endif

/** @brief Return default iop group for drawlayer module. */
int default_group()
{
  return IOP_GROUP_EFFECTS;
}

/** @brief Return module capability flags. */
int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING;
}

/** @brief Return default colorspace expected by drawlayer process paths. */
int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

/** @brief Allocate and initialize module parameter blocks. */
void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_drawlayer_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_drawlayer_params_t));
  module->params_size = sizeof(dt_iop_drawlayer_params_t);
  module->gui_data = NULL;

  if(module->params) ((dt_iop_drawlayer_params_t *)module->params)->layer_order = -1;
  if(module->default_params) ((dt_iop_drawlayer_params_t *)module->default_params)->layer_order = -1;
}

/** @brief Initialize per-pipe runtime data. */
void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  (void)self;
  (void)pipe;
  piece->data = calloc(1, sizeof(dt_iop_drawlayer_data_t));
  piece->data_size = sizeof(dt_iop_drawlayer_params_t);
  if(piece->data)
  {
    dt_iop_drawlayer_data_t *data = (dt_iop_drawlayer_data_t *)piece->data;
    data->params.layer_order = -1;
    data->headless_cache_layer_order = -1;
    data->headless_cache_imgid = -1;
  }
}

/** @brief Cleanup per-pipe runtime data. */
void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  (void)self;
  (void)pipe;
  if(!piece || !piece->data) return;
  dt_iop_drawlayer_data_t *data = (dt_iop_drawlayer_data_t *)piece->data;
  _clear_headless_cache(data);
  dt_free(piece->data);
  piece->data_size = 0;
}

/** @brief Commit params to runtime piece and refresh base cache state. */
void commit_params(dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_drawlayer_data_t *data = (dt_iop_drawlayer_data_t *)piece->data;
  memcpy(&data->params, params, sizeof(dt_iop_drawlayer_params_t));
  _sanitize_params(self, &data->params);

  /* Every pipe now warms the same authoritative base-patch snapshot through
   * the pixelpipe cache during `commit_params()`. GUI pipes still keep their
   * own transformed ROI cache on top, but they attach to the same shared base
   * line as headless pipes instead of carrying a private sidecar mirror. */
  _refresh_headless_cache(self, data, &data->params, piece);
}

/** @brief Reset GUI/session state for current drawlayer instance. */
void gui_reset(dt_iop_module_t *self)
{
  if(!self || !self->dev) return;
  if(!_commit_dabs(self, FALSE)) return;
  if(!_confirm_delete_layer(self, FALSE)) return;

  dt_iop_drawlayer_params_t *params = (dt_iop_drawlayer_params_t *)self->params;
  _default_layer_name(self, params->layer_name, sizeof(params->layer_name));
  params->layer_order = -1;
  _invalidate_undo_redo(self);
  _touch_stroke_commit_hash(params, 0, FALSE, 0.0f, 0.0f);
  _sync_temp_buffers(self, FALSE, FALSE);
}

/** @brief Hook called before module removal from history stack. */
gboolean module_will_remove(dt_iop_module_t *self)
{
  if(!self->dev) return TRUE;
  if(!_commit_dabs(self, FALSE)) return FALSE;
  _flush_layer_cache(self);
  return _confirm_delete_layer(self, TRUE);
}

/** @brief Build GUI widgets and initialize worker/caches. */
void gui_init(dt_iop_module_t *self)
{
  IOP_GUI_ALLOC(drawlayer);
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  dt_iop_drawlayer_params_t *params = (dt_iop_drawlayer_params_t *)self->params;
  _ensure_gui_conf_defaults();
  g->widgets = dt_drawlayer_widgets_init();
  dt_pthread_mutex_init(&g->process_patch_mutex, NULL);
  _load_color_history(g);
  _ensure_layer_name(self, params);
  _sanitize_params(self, params);

  g->backend_path = dt_drawlayer_paint_runtime_state_create();
  g->process_path = dt_drawlayer_paint_runtime_state_create();
  _paint_reset_stroke_runtime(g);
#ifdef HAVE_OPENCL
  g->process_read_clmem_devid = -1;
  g->process_read_clmem_dirty = TRUE;
#endif
  dt_drawlayer_worker_init(self, &g->rt, &g->painting, &g->finish_commit_pending,
                           &g->stroke_sample_count, &g->current_stroke_batch,
                           _replay_finished_stroke_to_base_patch);
  g->cache_valid = FALSE;
  g->cache_dirty = FALSE;
  g->background_job_running = FALSE;
  g->last_view_x = 0.0f;
  g->last_view_y = 0.0f;
  g->last_view_scale = 1.0f;
  if(self->dev)
  {
    g->last_view_x = self->dev->roi.x;
    g->last_view_y = self->dev->roi.y;
    g->last_view_scale = self->dev->roi.scaling;
  }

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_IOP_MODULE_CONTROL_SPACING);
  if(self->reset_button) gtk_widget_hide(self->reset_button);

  GtkWidget *history_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_IOP_MODULE_CONTROL_SPACING);
  g->undo_button = gtk_button_new_with_label(_("undo"));
  g->redo_button = gtk_button_new_with_label(_("redo"));
  g->save_layer = gtk_button_new_with_label(_("save sidecar"));
  gtk_box_pack_start(GTK_BOX(history_box), g->undo_button, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(history_box), g->redo_button, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(history_box), g->save_layer, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), history_box, FALSE, FALSE, 0);

  GtkWidget *notebook = gtk_notebook_new();
  gtk_widget_set_hexpand(notebook, TRUE);
  gtk_box_pack_start(GTK_BOX(self->widget), notebook, FALSE, FALSE, 0);

  GtkWidget *brush_tab = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_IOP_MODULE_CONTROL_SPACING);
  GtkWidget *layer_tab = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_IOP_MODULE_CONTROL_SPACING);
  GtkWidget *input_tab = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_IOP_MODULE_CONTROL_SPACING);

  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), brush_tab, gtk_label_new(_("Brush")));
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), layer_tab, gtk_label_new(_("Layer")));
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), input_tab, gtk_label_new(_("Input")));
  gtk_container_child_set(GTK_CONTAINER(notebook), brush_tab, "tab-expand", TRUE, "tab-fill", TRUE, NULL);
  gtk_container_child_set(GTK_CONTAINER(notebook), layer_tab, "tab-expand", TRUE, "tab-fill", TRUE, NULL);
  gtk_container_child_set(GTK_CONTAINER(notebook), input_tab, "tab-expand", TRUE, "tab-fill", TRUE, NULL);

  GtkWidget *preview_title = gtk_label_new(_("Background"));
  gtk_widget_set_halign(preview_title, GTK_ALIGN_START);
  GtkWidget *preview_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_IOP_MODULE_CONTROL_SPACING);
  GSList *preview_group = NULL;
  g->preview_bg_image = gtk_radio_button_new_with_label(preview_group, _("image"));
  preview_group = gtk_radio_button_get_group(GTK_RADIO_BUTTON(g->preview_bg_image));
  g->preview_bg_white = gtk_radio_button_new_with_label(preview_group, _("white"));
  preview_group = gtk_radio_button_get_group(GTK_RADIO_BUTTON(g->preview_bg_white));
  g->preview_bg_grey = gtk_radio_button_new_with_label(preview_group, _("grey"));
  preview_group = gtk_radio_button_get_group(GTK_RADIO_BUTTON(g->preview_bg_grey));
  g->preview_bg_black = gtk_radio_button_new_with_label(preview_group, _("black"));
  gtk_box_pack_start(GTK_BOX(preview_box), g->preview_bg_image, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(preview_box), g->preview_bg_white, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(preview_box), g->preview_bg_grey, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(preview_box), g->preview_bg_black, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(layer_tab), preview_title, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(layer_tab), preview_box, FALSE, FALSE, 0);

  g->brush_mode = dt_bauhaus_combobox_new(darktable.bauhaus, DT_GUI_MODULE(self));
  dt_bauhaus_combobox_add(g->brush_mode, _("paint"));
  dt_bauhaus_combobox_add(g->brush_mode, _("erase"));
  dt_bauhaus_combobox_add(g->brush_mode, _("blur"));
  dt_bauhaus_combobox_add(g->brush_mode, _("smudge"));
  dt_bauhaus_widget_set_label(g->brush_mode, _("Paint mode"));
  gtk_box_pack_start(GTK_BOX(brush_tab), g->brush_mode, TRUE, TRUE, 0);

  GtkWidget *color_title = gtk_label_new(_("Color"));
  gtk_label_set_xalign(GTK_LABEL(color_title), 0.0f);
  dt_gui_add_class(color_title, "dt_section_label");
  gtk_box_pack_start(GTK_BOX(brush_tab), color_title, TRUE, TRUE, 0);
  g->color = gtk_drawing_area_new();
  gtk_widget_set_size_request(g->color, -1, DT_DRAWLAYER_COLOR_PICKER_HEIGHT);
  gtk_widget_add_events(g->color, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
  gtk_box_pack_start(GTK_BOX(brush_tab), g->color, TRUE, TRUE, 0);
  g->color_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(16));
  g->color_swatch = gtk_drawing_area_new();
  gtk_widget_set_size_request(g->color_swatch, -1,
                              DT_PIXEL_APPLY_DPI(DT_DRAWLAYER_COLOR_HISTORY_HEIGHT));
  gtk_widget_add_events(g->color_swatch, GDK_BUTTON_PRESS_MASK);
  gtk_box_pack_start(GTK_BOX(g->color_row), g->color_swatch, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(brush_tab), g->color_row, TRUE, TRUE, 0);
  GtkWidget *picker_controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(6));
  g->image_colorpicker = dt_color_picker_new_with_cst(self, DT_COLOR_PICKER_POINT_AREA, NULL, IOP_CS_NONE);
  g->image_colorpicker_source = dt_bauhaus_combobox_new(darktable.bauhaus, DT_GUI_MODULE(self));
  dt_bauhaus_combobox_add(g->image_colorpicker_source, _("input"));
  dt_bauhaus_combobox_add(g->image_colorpicker_source, _("output"));
  dt_bauhaus_widget_set_label(g->image_colorpicker_source, _("Pick from"));
  gtk_box_pack_start(GTK_BOX(picker_controls), g->image_colorpicker, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(picker_controls), g->image_colorpicker_source, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(brush_tab), picker_controls, TRUE, TRUE, 0);

  g->hdr_exposure = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 0.0f, 4.0f, 0.1f, 0.0f, 2);
  dt_bauhaus_widget_set_label(g->hdr_exposure, _("HDR exposure"));
  dt_bauhaus_slider_set_format(g->hdr_exposure, _(" EV"));
  gtk_box_pack_start(GTK_BOX(brush_tab), g->hdr_exposure, TRUE, TRUE, 0);

  GtkWidget *geometry_title = gtk_label_new(_("Geometry"));
  gtk_label_set_xalign(GTK_LABEL(geometry_title), 0.0f);
  dt_gui_add_class(geometry_title, "dt_section_label");
  gtk_box_pack_start(GTK_BOX(brush_tab), geometry_title, TRUE, TRUE, 0);
  GtkWidget *brush_shape_title = gtk_label_new(_("Fall-off"));
  gtk_label_set_xalign(GTK_LABEL(brush_shape_title), 0.0f);
  gtk_box_pack_start(GTK_BOX(brush_tab), brush_shape_title, TRUE, TRUE, 0);
  g->brush_shape = gtk_drawing_area_new();
  gtk_widget_set_size_request(g->brush_shape, -1, DT_PIXEL_APPLY_DPI(72));
  gtk_widget_add_events(g->brush_shape, GDK_BUTTON_PRESS_MASK);
  gtk_box_pack_start(GTK_BOX(brush_tab), g->brush_shape, TRUE, TRUE, 0);

  g->size = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 1.0f, 2048.0f, 1.0f, 64.0f, 0);
  dt_bauhaus_widget_set_label(g->size, _("Size"));
  dt_bauhaus_slider_set_format(g->size, _(" px"));
  gtk_box_pack_start(GTK_BOX(brush_tab), g->size, TRUE, TRUE, 0);
  g->distance = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 0.0f, 100.0f, 1.0f, 0.0f, 2);
  dt_bauhaus_widget_set_label(g->distance, _("Sampling distance"));
  dt_bauhaus_slider_set_format(g->distance, "%");
  gtk_box_pack_start(GTK_BOX(brush_tab), g->distance, TRUE, TRUE, 0);
  g->smoothing = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 0.0f, 100.0f, 1.0f, 0.0f, 2);
  dt_bauhaus_widget_set_label(g->smoothing, _("Smoothing"));
  dt_bauhaus_slider_set_format(g->smoothing, "%");
  gtk_box_pack_start(GTK_BOX(brush_tab), g->smoothing, TRUE, TRUE, 0);
  g->softness = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 0.0f, 1.0f, 0.01f, 0.5f, 2);
  dt_bauhaus_widget_set_label(g->softness, _("Hardness"));
  dt_bauhaus_slider_set_factor(g->softness, 100.0f);
  dt_bauhaus_slider_set_format(g->softness, "%");
  gtk_box_pack_start(GTK_BOX(brush_tab), g->softness, TRUE, TRUE, 0);

  GtkWidget *thickness_title = gtk_label_new(_("Thickness"));
  gtk_label_set_xalign(GTK_LABEL(thickness_title), 0.0f);
  dt_gui_add_class(thickness_title, "dt_section_label");
  gtk_box_pack_start(GTK_BOX(brush_tab), thickness_title, TRUE, TRUE, 0);
  g->opacity = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 0.0f, 100.0f, 1.0f, 100.0f, 2);
  dt_bauhaus_widget_set_label(g->opacity, _("Opacity"));
  dt_bauhaus_slider_set_format(g->opacity, "%");
  gtk_box_pack_start(GTK_BOX(brush_tab), g->opacity, TRUE, TRUE, 0);
  g->flow = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 0.0f, 100.0f, 1.0f, 100.0f, 2);
  dt_bauhaus_widget_set_label(g->flow, _("Flow"));
  dt_bauhaus_slider_set_format(g->flow, "%");
  gtk_box_pack_start(GTK_BOX(brush_tab), g->flow, TRUE, TRUE, 0);

  GtkWidget *texture_title = gtk_label_new(_("Texture"));
  gtk_label_set_xalign(GTK_LABEL(texture_title), 0.0f);
  dt_gui_add_class(texture_title, "dt_section_label");
  gtk_box_pack_start(GTK_BOX(brush_tab), texture_title, TRUE, TRUE, 0);
  g->sprinkles = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 0.0f, 100.0f, 1.0f, 0.0f, 2);
  dt_bauhaus_widget_set_label(g->sprinkles, _("Sprinkles"));
  dt_bauhaus_slider_set_format(g->sprinkles, "%");
  gtk_box_pack_start(GTK_BOX(brush_tab), g->sprinkles, TRUE, TRUE, 0);
  g->sprinkle_size = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 1.0f, 256.0f, 1.0f, 3.0f, 0);
  dt_bauhaus_widget_set_label(g->sprinkle_size, _("Sprinkle size"));
  dt_bauhaus_slider_set_format(g->sprinkle_size, _(" px"));
  gtk_box_pack_start(GTK_BOX(brush_tab), g->sprinkle_size, TRUE, TRUE, 0);
  g->sprinkle_coarseness = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 0.0f, 100.0f, 1.0f, 50.0f, 2);
  dt_bauhaus_widget_set_label(g->sprinkle_coarseness, _("Coarseness"));
  dt_bauhaus_slider_set_format(g->sprinkle_coarseness, "%");
  gtk_box_pack_start(GTK_BOX(brush_tab), g->sprinkle_coarseness, TRUE, TRUE, 0);

  GtkWidget *layer_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_IOP_MODULE_CONTROL_SPACING);
  GtkWidget *layer_name_title = gtk_label_new(_("Layer name"));
  gtk_widget_set_halign(layer_name_title, GTK_ALIGN_START);
  GtkWidget *layer_fill_title = gtk_label_new(_("Fill"));
  gtk_widget_set_halign(layer_fill_title, GTK_ALIGN_START);
  GtkWidget *layer_action_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_IOP_MODULE_CONTROL_SPACING);
  GtkWidget *layer_fill_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_IOP_MODULE_CONTROL_SPACING);
  g->layer_name = GTK_ENTRY(gtk_entry_new());
  dt_accels_disconnect_on_text_input(GTK_WIDGET(g->layer_name));
  g->layer_select = dt_bauhaus_combobox_new(darktable.bauhaus, DT_GUI_MODULE(self));
  dt_bauhaus_widget_set_label(g->layer_select, _("Source layer"));
  g->delete_layer = gtk_button_new_with_label(_("delete layer"));
  g->create_layer = gtk_button_new_with_label(_("create new layer"));
  g->create_background = gtk_button_new_with_label(_("create background from input"));
  g->fill_white = gtk_button_new_with_label(_("white"));
  g->fill_black = gtk_button_new_with_label(_("black"));
  g->fill_transparent = gtk_button_new_with_label(_("transparency"));
  gtk_box_pack_start(GTK_BOX(layer_box), layer_name_title, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(layer_box), GTK_WIDGET(g->layer_name), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(layer_box), g->layer_select, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(layer_action_row), g->create_layer, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(layer_action_row), g->delete_layer, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(layer_box), layer_action_row, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(layer_box), layer_fill_title, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(layer_fill_row), g->fill_white, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(layer_fill_row), g->fill_black, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(layer_fill_row), g->fill_transparent, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(layer_box), layer_fill_row, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(layer_box), g->create_background, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(layer_tab), layer_box, FALSE, FALSE, 0);

  GtkWidget *mapping_title = gtk_label_new(_("tablet mapping"));
  gtk_widget_set_halign(mapping_title, GTK_ALIGN_START);
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 6);

  const char *labels[4] = { _("size"), _("opacity"), _("flow"), _("hardness") };
  const char *rows[3] = { _("pressure"), _("tilt"), _("acceleration") };
  GtkWidget **targets[3][4] = {
    { &g->map_pressure_size, &g->map_pressure_opacity, &g->map_pressure_flow, &g->map_pressure_softness },
    { &g->map_tilt_size, &g->map_tilt_opacity, &g->map_tilt_flow, &g->map_tilt_softness },
    { &g->map_accel_size, &g->map_accel_opacity, &g->map_accel_flow, &g->map_accel_softness },
  };
  GtkWidget **profiles[3] = { &g->pressure_profile, &g->tilt_profile, &g->accel_profile };

  for(int c = 0; c < 4; c++)
  {
    GtkWidget *label = gtk_label_new(labels[c]);
    gtk_label_set_angle(GTK_LABEL(label), 90.0);
    gtk_grid_attach(GTK_GRID(grid), label, c + 1, 0, 1, 1);
  }
  gtk_grid_attach(GTK_GRID(grid), gtk_label_new(_("profile")), 5, 0, 1, 1);

  for(int r = 0; r < 3; r++)
  {
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new(rows[r]), 0, r + 1, 1, 1);
    for(int c = 0; c < 4; c++)
    {
      *targets[r][c] = gtk_check_button_new();
      gtk_grid_attach(GTK_GRID(grid), *targets[r][c], c + 1, r + 1, 1, 1);
    }
    *profiles[r] = dt_bauhaus_combobox_new(darktable.bauhaus, DT_GUI_MODULE(self));
    dt_bauhaus_combobox_add(*profiles[r], _("linear"));
    dt_bauhaus_combobox_add(*profiles[r], _("quadratic"));
    dt_bauhaus_combobox_add(*profiles[r], _("square root"));
    dt_bauhaus_combobox_add(*profiles[r], _("inverse linear"));
    dt_bauhaus_combobox_add(*profiles[r], _("inverse square root"));
    dt_bauhaus_combobox_add(*profiles[r], _("inverse quadratic"));
    gtk_grid_attach(GTK_GRID(grid), *profiles[r], 5, r + 1, 1, 1);
  }

  gtk_box_pack_start(GTK_BOX(input_tab), mapping_title, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(input_tab), grid, FALSE, FALSE, 0);

  g_signal_connect(g->brush_shape, "draw", G_CALLBACK(_brush_profile_draw), self);
  g_signal_connect(g->brush_shape, "button-press-event", G_CALLBACK(_brush_profile_button_press), self);
  g_signal_connect(G_OBJECT(g->brush_mode), "value-changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(g->color, "draw", G_CALLBACK(_color_picker_draw), self);
  g_signal_connect(g->color_swatch, "draw", G_CALLBACK(_color_swatch_draw), self);
  g_signal_connect(g->color_swatch, "button-press-event", G_CALLBACK(_color_swatch_button_press), self);
  g_signal_connect(g->color, "button-press-event", G_CALLBACK(_color_picker_button_press), self);
  g_signal_connect(g->color, "button-release-event", G_CALLBACK(_color_picker_button_release), self);
  g_signal_connect(g->color, "motion-notify-event", G_CALLBACK(_color_picker_motion), self);
  g_signal_connect(G_OBJECT(g->image_colorpicker_source), "value-changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(G_OBJECT(g->size), "value-changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(G_OBJECT(g->distance), "value-changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(G_OBJECT(g->smoothing), "value-changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(G_OBJECT(g->opacity), "value-changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(G_OBJECT(g->flow), "value-changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(G_OBJECT(g->sprinkles), "value-changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(G_OBJECT(g->sprinkle_size), "value-changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(G_OBJECT(g->sprinkle_coarseness), "value-changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(G_OBJECT(g->softness), "value-changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(G_OBJECT(g->hdr_exposure), "value-changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(g->layer_name, "changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(G_OBJECT(g->layer_select), "value-changed", G_CALLBACK(_layer_selected), self);
  g_signal_connect(g->undo_button, "clicked", G_CALLBACK(_undo_clicked), self);
  g_signal_connect(g->redo_button, "clicked", G_CALLBACK(_redo_clicked), self);
  g_signal_connect(g->preview_bg_image, "toggled", G_CALLBACK(_preview_bg_toggled), self);
  g_signal_connect(g->preview_bg_white, "toggled", G_CALLBACK(_preview_bg_toggled), self);
  g_signal_connect(g->preview_bg_grey, "toggled", G_CALLBACK(_preview_bg_toggled), self);
  g_signal_connect(g->preview_bg_black, "toggled", G_CALLBACK(_preview_bg_toggled), self);
  g_signal_connect(g->create_layer, "clicked", G_CALLBACK(_create_layer_clicked), self);
  g_signal_connect(g->create_background, "clicked", G_CALLBACK(_create_background_clicked), self);
  g_signal_connect(g->save_layer, "clicked", G_CALLBACK(_save_layer_clicked), self);
  g_signal_connect(g->delete_layer, "clicked", G_CALLBACK(_delete_layer_clicked), self);
  g_signal_connect(g->fill_white, "clicked", G_CALLBACK(_fill_white_clicked), self);
  g_signal_connect(g->fill_black, "clicked", G_CALLBACK(_fill_black_clicked), self);
  g_signal_connect(g->fill_transparent, "clicked", G_CALLBACK(_fill_transparent_clicked), self);

  for(int r = 0; r < 3; r++)
  {
    for(int c = 0; c < 4; c++)
      g_signal_connect(*targets[r][c], "toggled", G_CALLBACK(_widget_changed), self);
    g_signal_connect(G_OBJECT(*profiles[r]), "value-changed", G_CALLBACK(_widget_changed), self);
  }

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED,
                                  G_CALLBACK(_develop_ui_pipe_finished_callback), self);

  if(self->dev) _sync_temp_buffers(self, FALSE, FALSE);
}

/** @brief Refresh GUI controls from current params and configuration. */
void gui_update(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  dt_iop_drawlayer_params_t *params = (dt_iop_drawlayer_params_t *)self->params;
  if(!g) return;

  _ensure_layer_name(self, params);
  _sanitize_params(self, params);

  dt_bauhaus_combobox_set(g->brush_mode, _conf_brush_mode());
  dt_bauhaus_slider_set(g->size, _conf_size());
  dt_bauhaus_slider_set(g->distance, _conf_distance());
  dt_bauhaus_slider_set(g->smoothing, _conf_smoothing());
  dt_bauhaus_slider_set(g->opacity, _conf_opacity());
  dt_bauhaus_slider_set(g->flow, _conf_flow());
  dt_bauhaus_slider_set(g->sprinkles, _conf_sprinkles());
  dt_bauhaus_slider_set(g->sprinkle_size, _conf_sprinkle_size());
  dt_bauhaus_slider_set(g->sprinkle_coarseness, _conf_sprinkle_coarseness());
  dt_bauhaus_slider_set(g->softness, _conf_hardness());
  if(g->image_colorpicker_source) dt_bauhaus_combobox_set(g->image_colorpicker_source, _conf_pick_source());
  dt_bauhaus_slider_set(g->hdr_exposure, _conf_hdr_exposure());

  _sync_color_picker_from_conf(self);
  _sync_brush_profile_preview_widget(self);
  if(g->color) gtk_widget_queue_draw(g->color);
  gtk_entry_set_text(g->layer_name, params->layer_name);

  _set_toggle_if_valid(g->map_pressure_size, dt_conf_get_bool(DRAWLAYER_CONF_MAP_PRESSURE_SIZE));
  _set_toggle_if_valid(g->map_pressure_opacity, dt_conf_get_bool(DRAWLAYER_CONF_MAP_PRESSURE_OPACITY));
  _set_toggle_if_valid(g->map_pressure_flow, dt_conf_get_bool(DRAWLAYER_CONF_MAP_PRESSURE_FLOW));
  _set_toggle_if_valid(g->map_pressure_softness, dt_conf_get_bool(DRAWLAYER_CONF_MAP_PRESSURE_SOFTNESS));

  _set_toggle_if_valid(g->map_tilt_size, dt_conf_get_bool(DRAWLAYER_CONF_MAP_TILT_SIZE));
  _set_toggle_if_valid(g->map_tilt_opacity, dt_conf_get_bool(DRAWLAYER_CONF_MAP_TILT_OPACITY));
  _set_toggle_if_valid(g->map_tilt_flow, dt_conf_get_bool(DRAWLAYER_CONF_MAP_TILT_FLOW));
  _set_toggle_if_valid(g->map_tilt_softness, dt_conf_get_bool(DRAWLAYER_CONF_MAP_TILT_SOFTNESS));

  _set_toggle_if_valid(g->map_accel_size, dt_conf_get_bool(DRAWLAYER_CONF_MAP_ACCEL_SIZE));
  _set_toggle_if_valid(g->map_accel_opacity, dt_conf_get_bool(DRAWLAYER_CONF_MAP_ACCEL_OPACITY));
  _set_toggle_if_valid(g->map_accel_flow, dt_conf_get_bool(DRAWLAYER_CONF_MAP_ACCEL_FLOW));
  _set_toggle_if_valid(g->map_accel_softness, dt_conf_get_bool(DRAWLAYER_CONF_MAP_ACCEL_SOFTNESS));

  if(g->pressure_profile) dt_bauhaus_combobox_set(g->pressure_profile, _conf_mapping_profile(DRAWLAYER_CONF_PRESSURE_PROFILE));
  if(g->tilt_profile) dt_bauhaus_combobox_set(g->tilt_profile, _conf_mapping_profile(DRAWLAYER_CONF_TILT_PROFILE));
  if(g->accel_profile) dt_bauhaus_combobox_set(g->accel_profile, _conf_mapping_profile(DRAWLAYER_CONF_ACCEL_PROFILE));

  _sync_mode_sensitive_widgets(self);
  _sync_undo_redo_buttons(self);
  _sync_preview_bg_buttons(self);
  _sync_save_button(self);
  _populate_layer_list(self);
}

/* Realtime pipe mode is tied to active strokes only. It bypasses strict history
 * resync checks/kill-switch behavior and must not stay enabled while idle.
 *
 * Important: drawlayer does NOT toggle module cache-bypass here.
 * - `bypass_cache` means disposable output cachelines (auto-destroy),
 * - `realtime` means best-effort processing where cachelines can still be reused.
 */
static void _set_drawlayer_pipeline_realtime_mode(dt_iop_module_t *self, gboolean state)
{
  if(!self || !self->dev || !self->dev->pipe) return;
  dt_dev_pixelpipe_set_realtime(self->dev->pipe, state);
}

static gboolean _drawlayer_pipeline_realtime_active(const dt_iop_drawlayer_gui_data_t *g)
{
  return g && (g->painting || g->finish_commit_pending || dt_drawlayer_worker_active(g->rt));
}

static void _sync_drawlayer_pipeline_realtime_mode(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  _set_drawlayer_pipeline_realtime_mode(self, _drawlayer_pipeline_realtime_active(g));
}

/** @brief Invalidate module state when active image changes. */
void change_image(dt_iop_module_t *self)
{
  if(self->gui_data)
  {
    dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
    _set_drawlayer_pipeline_realtime_mode(self, FALSE);
  _commit_dabs(self, FALSE);
  if(g) _flush_process_patch_to_base(self, g);
  _flush_layer_cache(self);
  _stop_worker(self, g->rt);
#ifdef HAVE_OPENCL
  _clear_process_read_clmem(g);
#endif
    _release_all_base_patch_extra_refs(g);
    _clear_patch(&g->base_patch);
    g->cache_valid = FALSE;
    g->cache_dirty = FALSE;
    _sync_save_button(self);
    gui_update(self);
    _sync_temp_buffers(self, TRUE, FALSE);
    if(self->dev && self->dev->gui_module == self && !_start_worker(self, g->rt))
      dt_control_log(_("failed to restart drawing worker"));
  }
}

/** @brief Focus transition hook (enter/leave) for drawlayer GUI mode. */
void gui_focus(dt_iop_module_t *self, gboolean in)
{
  if(!in)
  {
    dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
    dt_iop_drawlayer_params_t *params = (dt_iop_drawlayer_params_t *)self->params;
    const int pending_samples = g ? (int)g->stroke_sample_count : 0;
    const gboolean had_pending_edits
        = (g && (g->cache_dirty || g->process_patch_dirty
                 || g->stroke_sample_count > 0));
    drawlayer_wait_dialog_t wait = { 0 };
    if(_should_show_leave_wait_dialog(g))
      wait = _show_leave_wait_dialog();
    _set_drawlayer_os_cursor_hidden(FALSE);
    _set_drawlayer_pipeline_realtime_mode(self, FALSE);
    /* On focus loss we want exactly one final coherent state pushed to history:
     * 1) drain workers/queues without creating intermediate history entries,
     * 2) fold process tile into authoritative base,
     * 3) flush sidecar,
     * 4) bump hash + history once if anything changed. */
    _commit_dabs(self, FALSE);
    if(had_pending_edits && params)
      _touch_stroke_commit_hash(params, pending_samples, g->last_dab_valid, g->last_dab_x, g->last_dab_y);
    if(g) _flush_process_patch_to_base(self, g);
    if(!_flush_layer_cache(self)) dt_control_log(_("failed to write drawing layer sidecar"));
    if(had_pending_edits && self->dev && params)
      dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
    _stop_worker(self, g->rt);
    _hide_leave_wait_dialog(&wait);
  }
  else if(self->gui_data)
  {
    dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
    dt_iop_drawlayer_params_t *params = (dt_iop_drawlayer_params_t *)self->params;
    _set_drawlayer_pipeline_realtime_mode(self, FALSE);
    if(g)
    {
      g->missing_layer_prompt_name[0] = '\0';
    }

    if(!_start_worker(self, g->rt)) dt_control_log(_("failed to start drawing worker"));
    _sync_temp_buffers(self, FALSE, FALSE);
    _sync_save_button(self);

    if(g && params && !g->cache_valid && _current_layer_missing_in_sidecar(self))
    {
      const int action = _offer_missing_layer_recreation(self, params->layer_name);
      if(action == 2 && !_create_new_layer(self, params->layer_name))
        dt_control_log(_("failed to create drawing layer"));
    }
  }
}

/** @brief Destroy GUI resources and stop background worker. */
void gui_cleanup(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  drawlayer_wait_dialog_t wait = { 0 };
  if(_should_show_leave_wait_dialog(g))
    wait = _show_leave_wait_dialog();
  _set_drawlayer_os_cursor_hidden(FALSE);
  _set_drawlayer_pipeline_realtime_mode(self, FALSE);
  _commit_dabs(self, FALSE);
  if(g) _flush_process_patch_to_base(self, g);
  _flush_layer_cache(self);
  _stop_worker(self, g->rt);
  _hide_leave_wait_dialog(&wait);

  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_develop_ui_pipe_finished_callback), self);

  if(g)
  {
#ifdef HAVE_OPENCL
    _clear_process_read_clmem(g);
#endif
    dt_drawlayer_paint_runtime_state_destroy(&g->backend_path);
    dt_drawlayer_paint_runtime_state_destroy(&g->process_path);
    _release_all_base_patch_extra_refs(g);
    _clear_patch(&g->base_patch);
    _clear_patch(&g->process_patch);
    _clear_patch(&g->process_read_patch);
    _clear_patch(&g->undo_patch);
    _clear_patch(&g->stroke_mask);
    _clear_patch(&g->process_stroke_mask);
    dt_pthread_mutex_destroy(&g->process_patch_mutex);
    memset(&g->live_patch, 0, sizeof(g->live_patch));
    dt_drawlayer_widgets_cleanup(&g->widgets);
    _clear_cursor_stamp_surface(g);
    g->cache_valid = FALSE;
    g->cache_dirty = FALSE;
    g->process_patch_valid = FALSE;
    g->process_geom_hash = 0;
    dt_drawlayer_worker_cleanup(&g->rt);
  }

  IOP_GUI_FREE;
}

typedef struct drawlayer_hud_brush_state_t
{
  float pressure;
  float tilt;
  float acceleration;
  float radius;
  float opacity;
  float flow;
  float hardness;
} drawlayer_hud_brush_state_t;

static void _compute_hud_brush_state(const dt_control_pointer_input_t *pointer_input,
                                     drawlayer_hud_brush_state_t *state)
{
  if(!state) return;

  const float pressure_norm = _clamp01((pointer_input && pointer_input->has_pressure) ? pointer_input->pressure : 1.0f);
  const float tilt_norm = _clamp01((pointer_input && pointer_input->has_tilt) ? pointer_input->tilt : 0.0f);
  const float accel_norm = _clamp01(pointer_input ? pointer_input->acceleration : 0.0f);
  const drawlayer_mapping_profile_t pressure_profile = _conf_mapping_profile(DRAWLAYER_CONF_PRESSURE_PROFILE);
  const drawlayer_mapping_profile_t tilt_profile = _conf_mapping_profile(DRAWLAYER_CONF_TILT_PROFILE);
  const drawlayer_mapping_profile_t accel_profile = _conf_mapping_profile(DRAWLAYER_CONF_ACCEL_PROFILE);
  const float pressure_coeff = _mapping_multiplier(pressure_profile, pressure_norm);
  const float tilt_coeff = _mapping_multiplier(tilt_profile, tilt_norm);
  const float accel_coeff = _mapping_multiplier(accel_profile, accel_norm);

  float radius = _conf_size();
  float opacity = _conf_opacity() / 100.0f;
  float flow = _conf_flow() / 100.0f;
  float hardness = _conf_hardness();

  if(dt_conf_get_bool(DRAWLAYER_CONF_MAP_PRESSURE_SIZE)) radius *= pressure_coeff;
  if(dt_conf_get_bool(DRAWLAYER_CONF_MAP_PRESSURE_OPACITY)) opacity *= pressure_coeff;
  if(dt_conf_get_bool(DRAWLAYER_CONF_MAP_PRESSURE_FLOW)) flow *= pressure_coeff;
  if(dt_conf_get_bool(DRAWLAYER_CONF_MAP_PRESSURE_SOFTNESS)) hardness *= pressure_coeff;

  if(dt_conf_get_bool(DRAWLAYER_CONF_MAP_TILT_SIZE)) radius *= tilt_coeff;
  if(dt_conf_get_bool(DRAWLAYER_CONF_MAP_TILT_OPACITY)) opacity *= tilt_coeff;
  if(dt_conf_get_bool(DRAWLAYER_CONF_MAP_TILT_FLOW)) flow *= tilt_coeff;
  if(dt_conf_get_bool(DRAWLAYER_CONF_MAP_TILT_SOFTNESS)) hardness *= tilt_coeff;

  if(dt_conf_get_bool(DRAWLAYER_CONF_MAP_ACCEL_SIZE)) radius *= accel_coeff;
  if(dt_conf_get_bool(DRAWLAYER_CONF_MAP_ACCEL_OPACITY)) opacity *= accel_coeff;
  if(dt_conf_get_bool(DRAWLAYER_CONF_MAP_ACCEL_FLOW)) flow *= accel_coeff;
  if(dt_conf_get_bool(DRAWLAYER_CONF_MAP_ACCEL_SOFTNESS)) hardness *= accel_coeff;

  state->pressure = pressure_norm;
  state->tilt = tilt_norm;
  state->acceleration = accel_norm;
  state->radius = fmaxf(0.5f, radius);
  state->opacity = _clamp01(opacity);
  state->flow = _clamp01(flow);
  state->hardness = _clamp01(hardness);
}

static void _draw_brush_hud(cairo_t *cr, const drawlayer_hud_brush_state_t *state)
{
  if(!cr || !state) return;

  char lines[3][128] = { { 0 } };
  g_snprintf(lines[0], sizeof(lines[0]), _("size %.1f px  hardness %.2f%%"),
             state->radius * 2.0f, state->hardness * 100.0f);
  g_snprintf(lines[1], sizeof(lines[1]), _("opacity %.2f%%  flow %.2f%%"),
             state->opacity * 100.0f, state->flow * 100.0f);
  g_snprintf(lines[2], sizeof(lines[2]), _("pressure %.2f%%  tilt %.2f%%  acceleration %.2f%%"),
             state->pressure * 100.0f, state->tilt * 100.0f, state->acceleration * 100.0f);

  const double pad = DT_PIXEL_APPLY_DPI(6.0);
  const double line_h = DT_PIXEL_APPLY_DPI(13.0);
  const double fs = DT_PIXEL_APPLY_DPI(12.0);
  double max_w = 0.0;

  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, fs);
  for(int i = 0; i < 3; i++)
  {
    cairo_text_extents_t ext = { 0 };
    cairo_text_extents(cr, lines[i], &ext);
    max_w = fmax(max_w, ext.x_advance);
  }

  const double box_w = max_w + 2.0 * pad;
  const double box_h = 3.0 * line_h + 2.0 * pad;
  const double x = DT_PIXEL_APPLY_DPI(10.0);
  const double y = DT_PIXEL_APPLY_DPI(10.0);

  cairo_save(cr);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.55);
  cairo_rectangle(cr, x, y, box_w, box_h);
  cairo_fill(cr);

  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.92);
  for(int i = 0; i < 3; i++)
  {
    cairo_move_to(cr, x + pad, y + pad + (i + 1) * line_h - DT_PIXEL_APPLY_DPI(2.0));
    cairo_show_text(cr, lines[i]);
  }
  cairo_restore(cr);
}

/** @brief Draw post-expose overlay (cursor, HUD, temp preview). */
void gui_post_expose(dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  if(!g || !self->dev) return;

  cairo_save(cr);
  cairo_set_line_width(cr, 1.0);
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.8);

  _paint_temp_buffer(self, cr, width, height);

  if(g->pointer_valid)
  {
    dt_control_pointer_input_t pointer_input = { 0 };
    dt_control_get_pointer_input(&pointer_input);
    const float widget_x = isfinite(pointer_input.x)
                               ? (float)pointer_input.x
                               : ((pointerx >= 0 && pointery >= 0) ? (float)pointerx : -1.0f);
    const float widget_y = isfinite(pointer_input.y)
                               ? (float)pointer_input.y
                               : ((pointerx >= 0 && pointery >= 0) ? (float)pointery : -1.0f);
    if(widget_x < 0.0f || widget_y < 0.0f)
    {
      cairo_restore(cr);
      return;
    }
    drawlayer_hud_brush_state_t hud = { 0 };
    _compute_hud_brush_state(&pointer_input, &hud);

    float radius = hud.radius;
    const int brush_mode = _conf_brush_mode();
    const gboolean show_paint_fill = (brush_mode == DT_DRAWLAYER_BRUSH_MODE_PAINT);

    float draw_x = widget_x;
    float draw_y = widget_y;
    float lx = 0.0f;
    float ly = 0.0f;
    float widget_radius = radius * dt_dev_get_overlay_scale(self->dev);
    if(_widget_to_layer_coords(self, widget_x, widget_y, &lx, &ly))
    {
      dt_drawlayer_brush_dab_t dab = {
        .x = lx,
        .y = ly,
        .radius = radius,
      };
      if(!_layer_to_widget_coords(self, lx, ly, &draw_x, &draw_y))
      {
        draw_x = widget_x;
        draw_y = widget_y;
      }
      widget_radius = _widget_brush_radius(self, &dab, widget_radius);
    }

    // Draw the brush mipmap
    const float draw_radius = fmaxf(0.5f, widget_radius);
    if(show_paint_fill)
    {
      _ensure_cursor_stamp_surface(self, draw_radius, hud.opacity, hud.hardness);
      if(g->cursor_surface)
      {
        const float surface_half_extent
            = 0.5f * (float)g->cursor_surface_size / (float)g->cursor_surface_ppd;
        cairo_set_source_surface(cr, g->cursor_surface,
                                 draw_x - surface_half_extent, draw_y - surface_half_extent);
        cairo_paint(cr);
      }
    }

    // Draw the outer circle in case we are lost
    cairo_set_source_rgba(cr, 0., 0., 0., 0.5);
    cairo_set_line_width(cr, 2.5);
    cairo_arc(cr, draw_x, draw_y, draw_radius + 1.0f, 0.0, 2.0 * M_PI);
    cairo_stroke(cr);

    cairo_set_source_rgba(cr, 1., 1., 1., 0.5);
    cairo_set_line_width(cr, 1.0);
    cairo_arc(cr, draw_x, draw_y, draw_radius, 0.0, 2.0 * M_PI);
    cairo_stroke(cr);

    if(self->dev->gui_module == self)
    {
      _draw_brush_hud(cr, &hud);
    }
  }

  cairo_restore(cr);
}

/** @brief Mouse leave handler. */
int mouse_leave(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  if(!g) return 0;
  _sync_drawlayer_pipeline_realtime_mode(self);
  g->pointer_valid = FALSE;
  _set_drawlayer_os_cursor_hidden(FALSE);
  dt_control_queue_redraw_center();
  return 0;
}

/** @brief Mouse motion handler. */
int mouse_moved(dt_iop_module_t *self, double x, double y, double pressure, int which)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  if(!g || !self->dev) return 0;

  if(self->request_color_pick != DT_REQUEST_COLORPICK_OFF)
  {
    /* When the standard image color picker is active, drawlayer must stop
     * capturing the pointer entirely so darkroom can drive the picker overlay
     * and sampling path without competing cursor state from the brush tool. */
    const gboolean had_realtime = _drawlayer_pipeline_realtime_active(g);
    g->pointer_valid = FALSE;
    g->painting = FALSE;
    if(had_realtime) dt_drawlayer_worker_request_commit(g->rt);
    _sync_drawlayer_pipeline_realtime_mode(self);
    _sync_undo_redo_buttons(self);
    _set_drawlayer_os_cursor_hidden(FALSE);
    return 0;
  }

  if(!g->pointer_valid) _set_drawlayer_os_cursor_hidden(TRUE);
  g->pointer_valid = TRUE;

  if(g->painting)
  {
    if(!_append_dab_sample(self, x, y, pressure))
    {
      /* Queue overflow or enqueue failure aborts the current stroke so GUI and
       * worker stay in sync on stroke boundaries. */
      const gboolean had_realtime = _drawlayer_pipeline_realtime_active(g);
      g->painting = FALSE;
      if(had_realtime) dt_drawlayer_worker_request_commit(g->rt);
      _sync_drawlayer_pipeline_realtime_mode(self);
      _sync_undo_redo_buttons(self);
    }
  }
  else
  {
    _sync_drawlayer_pipeline_realtime_mode(self);
  }

  gtk_widget_queue_draw(dt_ui_center(darktable.gui->ui));
  return g->painting ? 1 : 0;
}

/** @brief Button press handler (starts stroke capture on left button). */
int button_pressed(dt_iop_module_t *self, double x, double y, double pressure, int which, int type, uint32_t state)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  if(!g || !self->dev || which != 1) return 0;

  if(self->dev->gui_module != self)
  {
    dt_iop_request_focus(self);
    return 1;
  }

  /* Latch realtime mode before any sync/start side effects so the very first
   * recompute triggered by this stroke observes realtime policy. */
  _set_drawlayer_pipeline_realtime_mode(self, TRUE);
  if(!_sync_temp_buffers(self, TRUE, FALSE))
  {
    _set_drawlayer_pipeline_realtime_mode(self, FALSE);
    return 0;
  }
  if(!g->painting && (g->finish_commit_pending || g->stroke_sample_count > 0 || dt_drawlayer_worker_active(g->rt)))
    _commit_dabs(self, TRUE);
  /* Prepare undo snapshot on stroke start in the UI thread (once per stroke),
   * not in the backend worker per first-sample callback. This keeps backend
   * sample processing focused on dab rasterization only.
   */
  _prepare_undo_snapshot(self);
  /* The first backend dab must find a live frontend process tile already
   * materialized, otherwise `_process_backend_dab()` has nowhere to stamp it
   * and the initial sample of the stroke is lost from the displayed preview. */
  _prime_live_process_patch_before_stroke(self);
  if(!_start_worker(self, g->rt))
  {
    _set_drawlayer_pipeline_realtime_mode(self, FALSE);
    return 0;
  }

  dt_iop_gui_enter_critical_section(self);
  g->painting = TRUE;
  g->pointer_valid = TRUE;
  g->current_stroke_batch++;
  if(g->current_stroke_batch == 0) g->current_stroke_batch++;
  if(!dt_drawlayer_worker_active(g->rt))
  {
    dt_drawlayer_paint_runtime_state_reset(g->backend_path);
  }
  g->finish_commit_pending = FALSE;
  g->stroke_sample_count = 0;
  g->stroke_event_index = 0;
  g->last_dab_valid = FALSE;
  _sync_undo_redo_buttons(self);
  dt_iop_gui_leave_critical_section(self);
  dt_control_pointer_input_t pointer_input = { 0 };
  dt_control_get_pointer_input(&pointer_input);
  const float input_wx = isfinite(pointer_input.x) ? (float)pointer_input.x : (float)x;
  const float input_wy = isfinite(pointer_input.y) ? (float)pointer_input.y : (float)y;
  const float pressure_norm = pointer_input.has_pressure ? _clamp01(pointer_input.pressure) : _clamp01(pressure);
  dt_drawlayer_paint_raw_input_t first = {
    .wx = input_wx,
    .wy = input_wy,
    .pressure = pressure_norm,
    .tilt = (float)_clamp01(pointer_input.tilt),
    .acceleration = (float)_clamp01(pointer_input.acceleration),
    .event_ts = g_get_monotonic_time(),
    .stroke_batch = g->current_stroke_batch,
    .event_index = ++g->stroke_event_index,
    .stroke_pos = DT_DRAWLAYER_PAINT_STROKE_FIRST,
  };
  _fill_input_brush_settings(self, &first);
  if(!dt_drawlayer_worker_enqueue_input(g->rt, &first)) dt_control_log(_("failed to queue live drawing stroke"));
  dt_control_queue_redraw_center();
  return 1;
}

/** @brief Button release handler (ends current stroke). */
int button_released(dt_iop_module_t *self, double x, double y, int which, uint32_t state)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  if(!g || !self->dev || which != 1) return 0;

  if(g->painting)
  {
    g->painting = FALSE;
    _sync_undo_redo_buttons(self);

    dt_control_pointer_input_t pointer_input = { 0 };
    dt_control_get_pointer_input(&pointer_input);
    const float input_wx = isfinite(pointer_input.x) ? (float)pointer_input.x : (float)x;
    const float input_wy = isfinite(pointer_input.y) ? (float)pointer_input.y : (float)y;
    dt_drawlayer_paint_raw_input_t end = {
      .wx = input_wx,
      .wy = input_wy,
      .pressure = pointer_input.has_pressure ? _clamp01(pointer_input.pressure) : 1.0f,
      .tilt = (float)_clamp01(pointer_input.tilt),
      .acceleration = (float)_clamp01(pointer_input.acceleration),
      .event_ts = g_get_monotonic_time(),
      .stroke_batch = g->current_stroke_batch,
      .event_index = ++g->stroke_event_index,
      .stroke_pos = DT_DRAWLAYER_PAINT_STROKE_END,
    };
    _fill_input_brush_settings(self, &end);

    /* Always request one final commit when the stroke ends, even if the queued
     * STROKE_END marker gets dropped under transient queue pressure. The async
     * idle callback will commit only when workers are truly idle. */
    dt_drawlayer_worker_request_commit(g->rt);
    if(!dt_drawlayer_worker_enqueue_stroke_end(g->rt, &end))
      dt_control_log(_("failed to queue drawing stroke end"));
    _sync_drawlayer_pipeline_realtime_mode(self);
    dt_control_queue_redraw_center();
    return 1;
  }

  return 0;
}

/** @brief Scroll handler used for interactive brush-size changes. */
int scrolled(dt_iop_module_t *self, double x, double y, int up, uint32_t state)
{
  if(!self->dev || self->dev->gui_module != self) return 0;

  const gboolean increase = dt_mask_scroll_increases(up);
  const float factor = increase ? 1.1f : 0.9f;
  const float new_size = CLAMP(_conf_size() * factor, 1.0f, 2048.0f);
  dt_conf_set_float(DRAWLAYER_CONF_SIZE, new_size);

  if(self->gui_data)
  {
    dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
    dt_bauhaus_slider_set(g->size, new_size);
  }

  _sync_temp_buffers(self, TRUE, FALSE);
  dt_control_queue_redraw_center();
  return 1;
}

#ifdef HAVE_OPENCL
/** @brief OpenCL processing path for layer-over-input compositing. */
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const gint64 process_t0 = g_get_monotonic_time();
  gint64 process_t = process_t0;
  const dt_iop_drawlayer_global_data_t *gd = (const dt_iop_drawlayer_global_data_t *)self->global_data;
  const dt_iop_drawlayer_data_t *data = (const dt_iop_drawlayer_data_t *)piece->data;
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  const dt_iop_drawlayer_params_t *runtime_params = data ? &data->params : (const dt_iop_drawlayer_params_t *)self->params;
  _prime_gui_process_patch_from_backbuffer(self, piece, roi_in, roi_out, runtime_params);
  if(!gd || gd->kernel_premult_over < 0) return FALSE;
  if(!runtime_params || runtime_params->layer_name[0] == '\0')
    return dt_iop_clip_and_zoom_roi_cl(piece->pipe->devid, dev_out, dev_in, roi_out, roi_in) == CL_SUCCESS;

  int preview_bg_mode = DRAWLAYER_PREVIEW_BG_IMAGE;
  if(g && self->dev && self->dev->gui_module == self) preview_bg_mode = g->preview_bg_mode;
  const gboolean use_preview_bg = (preview_bg_mode != DRAWLAYER_PREVIEW_BG_IMAGE);
  const float preview_bg = (preview_bg_mode == DRAWLAYER_PREVIEW_BG_WHITE)
                               ? 1.0f
                               : (preview_bg_mode == DRAWLAYER_PREVIEW_BG_GREY) ? 0.5f : 0.0f;
  const int current_full_w = piece->pipe->iwidth;
  const int current_full_h = piece->pipe->iheight;
  if(current_full_w <= 0 || current_full_h <= 0)
    return dt_iop_clip_and_zoom_roi_cl(piece->pipe->devid, dev_out, dev_in, roi_out, roi_in) == CL_SUCCESS;

  _prewarm_gui_process_patch_cl(self, piece, roi_out, gd, use_preview_bg, preview_bg);

  drawlayer_process_scratch_t *scratch = _get_process_scratch();
  if(!scratch) return FALSE;

  if(g && _is_drawlayer_display_pipe(self, piece))
  {
    const int cache_ref_w = g->base_patch.width > 0 ? g->base_patch.width : piece->pipe->iwidth;
    const int cache_ref_h = g->base_patch.height > 0 ? g->base_patch.height : piece->pipe->iheight;
    const gboolean have_cache = _layer_cache_matches(g, piece->pipe->image.id, cache_ref_w, cache_ref_h,
                                                     runtime_params->layer_name, runtime_params->layer_order);
    {
      const gint64 now = g_get_monotonic_time();
      dt_print(DT_DEBUG_PERF, "[drawlayer] process_cl step=raw-cache-check ms=%.3f hit=%d",
               (now - process_t) / 1000.0, have_cache ? 1 : 0);
      process_t = now;
    }
    if(have_cache && _build_process_patch_from_base(self, g, piece, roi_in, roi_out))
    {
      {
        const gint64 now = g_get_monotonic_time();
        dt_print(DT_DEBUG_PERF, "[drawlayer] process_cl step=prepare-process-patch ms=%.3f",
                 (now - process_t) / 1000.0);
        process_t = now;
      }
        dt_pthread_mutex_lock(&g->process_patch_mutex);
  dt_drawlayer_cache_patch_rdlock(&g->process_read_patch);
      if(!g->process_read_patch.pixels || g->process_read_patch.width <= 0 || g->process_read_patch.height <= 0)
      {
          dt_drawlayer_cache_patch_rdunlock(&g->process_read_patch);
  dt_pthread_mutex_unlock(&g->process_patch_mutex);
        return dt_iop_clip_and_zoom_roi_cl(piece->pipe->devid, dev_out, dev_in, roi_out, roi_in) == CL_SUCCESS;
      }

      dt_iop_roi_t source_process_roi = { 0 };
      dt_iop_roi_t blend_target_roi = { 0 };
      gboolean direct_copy = FALSE;
      if(!dt_drawlayer_cache_build_process_blend_rois(&g->process_read_patch, g->process_patch_padding, roi_out,
                                                      &blend_target_roi, &source_process_roi, &direct_copy))
      {
          dt_drawlayer_cache_patch_rdunlock(&g->process_read_patch);
  dt_pthread_mutex_unlock(&g->process_patch_mutex);
        return dt_iop_clip_and_zoom_roi_cl(piece->pipe->devid, dev_out, dev_in, roi_out, roi_in) == CL_SUCCESS;
      }
      cl_mem process_source = NULL;
      if(dt_dev_pixelpipe_get_realtime(piece->pipe))
        process_source = _ensure_process_read_clmem_locked(g, piece->pipe->devid);
      const gboolean ok = _blend_layer_over_input_cl(piece->pipe->devid, gd->kernel_premult_over, dev_out, dev_in,
                                                     scratch, g->process_read_patch.pixels, g->process_read_patch.cache_entry,
                                                     process_source,
                                                     g->process_read_patch.width, g->process_read_patch.height,
                                                     &blend_target_roi, &source_process_roi, direct_copy,
                                                     use_preview_bg, preview_bg,
                                                     dt_dev_pixelpipe_get_realtime(piece->pipe),
                                                     dt_dev_pixelpipe_get_realtime(piece->pipe) && !process_source);
        dt_drawlayer_cache_patch_rdunlock(&g->process_read_patch);
  dt_pthread_mutex_unlock(&g->process_patch_mutex);
      {
        const gint64 now = g_get_monotonic_time();
        dt_print(DT_DEBUG_PERF, "[drawlayer] process_cl step=blend-process-patch ms=%.3f total=%.3f ok=%d",
                 (now - process_t) / 1000.0, (now - process_t0) / 1000.0, ok ? 1 : 0);
      }
      return ok;
    }
  }

  if(data && data->headless_cache_valid && data->headless_base_patch.pixels
     && data->headless_cache_imgid == piece->pipe->image.id)
  {
    const dt_iop_roi_t process_roi = roi_in ? *roi_in : *roi_out;
    const dt_iop_roi_t source_full_roi = {
      .x = 0,
      .y = 0,
      .width = data->headless_base_patch.width,
      .height = data->headless_base_patch.height,
      .scale = 1.0f,
    };
    dt_iop_roi_t combined_roi = { 0 };
    dt_drawlayer_cache_build_combined_process_roi_for_piece(piece, &process_roi,
                                                            current_full_w, current_full_h,
                                                            data->headless_base_patch.width,
                                                            data->headless_base_patch.height, &combined_roi);
    {
      const gint64 now = g_get_monotonic_time();
      dt_print(DT_DEBUG_PERF, "[drawlayer] process_cl step=build-combined-roi-headless ms=%.3f",
               (now - process_t) / 1000.0);
      process_t = now;
    }
    dt_drawlayer_cache_patch_rdlock(&data->headless_base_patch);
    const gboolean ok = _blend_layer_over_input_cl(piece->pipe->devid, gd->kernel_premult_over, dev_out, dev_in,
                                                    scratch, data->headless_base_patch.pixels,
                                                    data->headless_base_patch.cache_entry, NULL,
                                                    data->headless_base_patch.width,
                                                    data->headless_base_patch.height, &combined_roi,
                                                    &source_full_roi, FALSE, use_preview_bg, preview_bg,
                                                    dt_dev_pixelpipe_get_realtime(piece->pipe), FALSE);
    dt_drawlayer_cache_patch_rdunlock(&data->headless_base_patch);
    {
      const gint64 now = g_get_monotonic_time();
      dt_print(DT_DEBUG_PERF, "[drawlayer] process_cl step=blend-headless ms=%.3f total=%.3f ok=%d",
               (now - process_t) / 1000.0, (now - process_t0) / 1000.0, ok ? 1 : 0);
    }
    return ok;
  }

  {
    const gint64 now = g_get_monotonic_time();
    dt_print(DT_DEBUG_PERF, "[drawlayer] process_cl step=no-cache-pass-through ms=%.3f total=%.3f",
             (now - process_t) / 1000.0, (now - process_t0) / 1000.0);
  }
  return dt_iop_clip_and_zoom_roi_cl(piece->pipe->devid, dev_out, dev_in, roi_out, roi_in) == CL_SUCCESS;
}
#endif

/** @brief CPU processing path for layer-over-input compositing. */
int process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
            const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_drawlayer_data_t *data = (const dt_iop_drawlayer_data_t *)piece->data;
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  const dt_iop_drawlayer_params_t *runtime_params = data ? &data->params : (const dt_iop_drawlayer_params_t *)self->params;
  const float *input = (const float *)ivoid;
  float *output = (float *)ovoid;
  const size_t pixels = (size_t)roi_out->width * roi_out->height;
  const gint64 process_t0 = g_get_monotonic_time();
  gint64 process_t = process_t0;
  _prime_gui_process_patch_from_backbuffer(self, piece, roi_in, roi_out, runtime_params);

#ifdef HAVE_OPENCL
  /* `process_read_clmem` is only useful while drawlayer itself runs on the GPU.
   * Keeping that mirror alive across CPU passes needlessly pins VRAM and can
   * make the next OpenCL module fail when it tries to import drawlayer's CPU
   * output back to the device. */
  if(g && g->process_read_clmem)
  {
    dt_pthread_mutex_lock(&g->process_patch_mutex);
    _clear_process_read_clmem(g);
    dt_pthread_mutex_unlock(&g->process_patch_mutex);
    g->process_cl_prewarm_hash = 0;
    g->process_cl_prewarm_devid = -1;
  }
#endif

#ifndef DRAWLAYER_PROCESS_TRACE
#define DRAWLAYER_PROCESS_TRACE 0
#endif
#if DRAWLAYER_PROCESS_TRACE
  dt_print(DT_DEBUG_DEV,
           "[drawlayer] process pipe_in=%dx%d piece_buf_in=(x=%d y=%d w=%d h=%d scale=%.6f) "
           "piece_buf_out=(x=%d y=%d w=%d h=%d scale=%.6f) "
           "roi_in=(x=%d y=%d w=%d h=%d scale=%.6f) "
           "roi_out=(x=%d y=%d w=%d h=%d scale=%.6f)\n",
           piece->pipe->iwidth, piece->pipe->iheight, piece->buf_in.x, piece->buf_in.y, piece->buf_in.width,
           piece->buf_in.height, piece->buf_in.scale, piece->buf_out.x, piece->buf_out.y, piece->buf_out.width,
           piece->buf_out.height, piece->buf_out.scale, roi_in ? roi_in->x : 0, roi_in ? roi_in->y : 0,
           roi_in ? roi_in->width : 0, roi_in ? roi_in->height : 0, roi_in ? roi_in->scale : 0.0f,
           roi_out ? roi_out->x : 0, roi_out ? roi_out->y : 0, roi_out ? roi_out->width : 0,
           roi_out ? roi_out->height : 0, roi_out ? roi_out->scale : 0.0f);
#endif

  /* `process()` keeps the pipeline contract simple:
   * - the in-memory cache and stroke math stay in float32,
   * - TIFF I/O converts to/from half-float only at the file boundary. */
  if(!runtime_params || runtime_params->layer_name[0] == '\0')
  {
    _copy_input_to_output(input, output, roi_out->width, roi_out->height);
    return 0;
  }

  int preview_bg_mode = DRAWLAYER_PREVIEW_BG_IMAGE;
  if(g && self->dev && self->dev->gui_module == self) preview_bg_mode = g->preview_bg_mode;
  const gboolean use_preview_bg = (preview_bg_mode != DRAWLAYER_PREVIEW_BG_IMAGE);
  const float preview_bg = (preview_bg_mode == DRAWLAYER_PREVIEW_BG_WHITE)
                               ? 1.0f
                               : (preview_bg_mode == DRAWLAYER_PREVIEW_BG_GREY) ? 0.5f : 0.0f;
  const int current_full_w = piece->pipe->iwidth;
  const int current_full_h = piece->pipe->iheight;
  if(current_full_w <= 0 || current_full_h <= 0)
  {
    _copy_input_to_output(input, output, roi_out->width, roi_out->height);
    return 0;
  }
  if(g && _is_drawlayer_display_pipe(self, piece))
  {
    /* Fastest steady-state path:
     * - raw/full-resolution layer cache is already resident,
     * - transformed process tile for the current geometry is either already
     *   valid or can be rebuilt once from that raw cache,
     * - then `process()` only blends the prepared tile.
     *
     * This intentionally bypasses the older one-shot post-zoom pixelpipe cache.
     * That cache still helps as a fallback, but it cannot remove the dominant
     * cost because it is repopulated once per snapshot. `process_patch` is the
     * persistent transformed tile that the backend worker can update in place. */
    const int cache_ref_w = g->base_patch.width > 0 ? g->base_patch.width : piece->pipe->iwidth;
    const int cache_ref_h = g->base_patch.height > 0 ? g->base_patch.height : piece->pipe->iheight;
    const gboolean have_cache = _layer_cache_matches(g, piece->pipe->image.id, cache_ref_w, cache_ref_h,
                                                     runtime_params->layer_name, runtime_params->layer_order);
    {
      const gint64 now = g_get_monotonic_time();
      dt_print(DT_DEBUG_PERF, "[drawlayer] process step=raw-cache-check ms=%.3f hit=%d",
               (now - process_t) / 1000.0, have_cache ? 1 : 0);
      process_t = now;
    }
    if(have_cache && _build_process_patch_from_base(self, g, piece, roi_in, roi_out))
    {
      {
        const gint64 now = g_get_monotonic_time();
        dt_print(DT_DEBUG_PERF, "[drawlayer] process step=build-process-patch ms=%.3f",
                 (now - process_t) / 1000.0);
        process_t = now;
      }
        dt_pthread_mutex_lock(&g->process_patch_mutex);
  dt_drawlayer_cache_patch_rdlock(&g->process_read_patch);
      gboolean direct_copy = FALSE;
      if(!dt_drawlayer_cache_build_process_blend_rois(&g->process_read_patch, g->process_patch_padding, roi_out,
                                                      NULL, NULL, &direct_copy))
      {
          dt_drawlayer_cache_patch_rdunlock(&g->process_read_patch);
  dt_pthread_mutex_unlock(&g->process_patch_mutex);
        _copy_input_to_output(input, output, roi_out->width, roi_out->height);
        return 0;
      }
      const float *layer_pixels = g->process_read_patch.pixels;
      drawlayer_process_scratch_t *scratch = NULL;
      if(!direct_copy)
      {
        const gint64 resample_t0 = g_get_monotonic_time();
        scratch = _get_process_scratch();
        if(!scratch)
        {
            dt_drawlayer_cache_patch_rdunlock(&g->process_read_patch);
  dt_pthread_mutex_unlock(&g->process_patch_mutex);
          _copy_input_to_output(input, output, roi_out->width, roi_out->height);
          return 0;
        }

        float *layerbuf = dt_drawlayer_cache_ensure_scratch_buffer(&scratch->layerbuf,
                                                                   &scratch->layerbuf_pixels, pixels,
                                                                   "drawlayer process scratch");
        if(!layerbuf)
        {
            dt_drawlayer_cache_patch_rdunlock(&g->process_read_patch);
  dt_pthread_mutex_unlock(&g->process_patch_mutex);
          _copy_input_to_output(input, output, roi_out->width, roi_out->height);
          return 0;
        }
        if(!dt_drawlayer_cache_resample_process_patch_to_output(&g->process_read_patch, g->process_patch_padding,
                                                                roi_out, layerbuf, roi_out->width))
        {
            dt_drawlayer_cache_patch_rdunlock(&g->process_read_patch);
  dt_pthread_mutex_unlock(&g->process_patch_mutex);
          _copy_input_to_output(input, output, roi_out->width, roi_out->height);
          return 0;
        }
        layer_pixels = layerbuf;
        {
          const gint64 now = g_get_monotonic_time();
          dt_print(DT_DEBUG_PERF, "[drawlayer] process step=resample-process-patch ms=%.3f",
                   (now - resample_t0) / 1000.0);
        }
      }

      _blend_layer_over_input(output, input, layer_pixels, pixels, use_preview_bg, preview_bg);
        dt_drawlayer_cache_patch_rdunlock(&g->process_read_patch);
  dt_pthread_mutex_unlock(&g->process_patch_mutex);
      {
        const gint64 now = g_get_monotonic_time();
        dt_print(DT_DEBUG_PERF, "[drawlayer] process step=blend-process-patch ms=%.3f total=%.3f",
                 (now - process_t) / 1000.0, (now - process_t0) / 1000.0);
      }
      return 0;
    }
  }

  if(data && data->headless_cache_valid && data->headless_base_patch.pixels
     && data->headless_cache_imgid == piece->pipe->image.id)
  {
    /* Headless / non-GUI pipelines keep their own authoritative cache loaded in
     * `commit_params()`, outside of `process()`. They still use the same affine
     * transform logic, but without any GUI-owned transformed cache. */
    const dt_iop_roi_t process_roi = roi_in ? *roi_in : *roi_out;
    const dt_iop_roi_t source_full_roi = {
      .x = 0,
      .y = 0,
      .width = data->headless_base_patch.width,
      .height = data->headless_base_patch.height,
      .scale = 1.0f,
    };
    drawlayer_process_scratch_t scratch = { 0 };
    float *layerbuf = dt_drawlayer_cache_ensure_scratch_buffer(&scratch.layerbuf, &scratch.layerbuf_pixels,
                                                               pixels, "drawlayer process scratch");
    if(layerbuf)
    {
      dt_iop_roi_t combined_roi = { 0 };
      dt_drawlayer_cache_build_combined_process_roi_for_piece(piece, &process_roi,
                                                              current_full_w, current_full_h,
                                                              data->headless_base_patch.width,
                                                              data->headless_base_patch.height, &combined_roi);
      {
        const gint64 now = g_get_monotonic_time();
        dt_print(DT_DEBUG_PERF, "[drawlayer] process step=build-combined-roi-headless ms=%.3f",
                 (now - process_t) / 1000.0);
        process_t = now;
      }
      dt_drawlayer_cache_patch_rdlock(&data->headless_base_patch);
      dt_iop_clip_and_zoom(layerbuf, data->headless_base_patch.pixels, &combined_roi, &source_full_roi,
                           roi_out->width, data->headless_base_patch.width);
      dt_drawlayer_cache_patch_rdunlock(&data->headless_base_patch);
      {
        const gint64 now = g_get_monotonic_time();
        dt_print(DT_DEBUG_PERF, "[drawlayer] process step=clip-and-zoom-headless ms=%.3f",
                 (now - process_t) / 1000.0);
        process_t = now;
      }
      _blend_layer_over_input(output, input, layerbuf, pixels, use_preview_bg, preview_bg);
      {
        const gint64 now = g_get_monotonic_time();
        dt_print(DT_DEBUG_PERF, "[drawlayer] process step=blend-headless ms=%.3f total=%.3f",
                 (now - process_t) / 1000.0, (now - process_t0) / 1000.0);
      }
      dt_drawlayer_cache_free_temp_buffer((void **)&scratch.layerbuf, "drawlayer process scratch");
      return 0;
    }
  }

  /* The sidecar is intentionally managed outside of `process()`. Once a layer
   * is loaded, the in-memory caches are authoritative until module-level flush
   * points write them back. If we do not have a usable in-memory cache here,
   * the correct backend behavior is therefore a no-op pass-through rather than
   * reopening/scanning/loading the TIFF in the hot process path. */
  {
    const gint64 now = g_get_monotonic_time();
    dt_print(DT_DEBUG_PERF, "[drawlayer] process step=no-cache-pass-through ms=%.3f total=%.3f",
             (now - process_t) / 1000.0, (now - process_t0) / 1000.0);
  }
  _copy_input_to_output(input, output, roi_out->width, roi_out->height);
  return 0;
}
