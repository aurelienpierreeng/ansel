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

#include "bauhaus/bauhaus.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/dtpthread.h"
#include "common/image.h"
#include "common/imagebuf.h"
#include "common/imageio.h"
#include "common/imageio_module.h"
#include "common/iop_profile.h"
#include "common/opencl.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "develop/blend.h"
#include "develop/dev_history.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/imageop_math.h"
#include "develop/noise_generator.h"
#include "develop/pixelpipe_cache.h"
#include "gui/color_picker_proxy.h"
#include "gui/gtk.h"
#include "gui/gui_throttle.h"
#include "iop/drawlayer/brush.h"
#include "iop/drawlayer/cache.h"
#include "iop/drawlayer/common.h"
#include "iop/drawlayer/io.h"
#include "iop/drawlayer/paint.h"
#include "iop/drawlayer/runtime.h"
#include "iop/drawlayer/widgets.h"
#include "iop/drawlayer/worker.h"
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

typedef struct dt_iop_drawlayer_data_t
{
  /* Keep serialized params as the first field so the pipe runtime can mirror the
   * module params while also carrying per-piece cache/process state. */
  dt_iop_drawlayer_params_t params;

  /* Non-display pipelines still need the same authoritative base-layer cache as
   * the GUI, just without GUI-only transformed-preview state. Reuse the normal
   * process-state container so both paths speak the same cache model. */
  dt_drawlayer_process_state_t process;
  dt_drawlayer_runtime_manager_t headless_manager;
  dt_drawlayer_runtime_manager_t *runtime_manager;
  dt_drawlayer_process_state_t *runtime_process;
  gboolean runtime_display_pipe;
} dt_iop_drawlayer_data_t;

#ifdef HAVE_OPENCL
typedef struct dt_iop_drawlayer_global_data_t
{
  int kernel_premult_over;
} dt_iop_drawlayer_global_data_t;
#endif

typedef struct drawlayer_view_patch_info_t
{
  drawlayer_view_patch_t patch;
  float layer_x0;
  float layer_y0;
  float layer_x1;
  float layer_y1;
} drawlayer_view_patch_info_t;

typedef struct drawlayer_preview_background_t
{
  gboolean enabled;
  float value;
} drawlayer_preview_background_t;

typedef struct drawlayer_layer_cache_key_t
{
  int32_t imgid;
  int raw_width;
  int raw_height;
  const char *layer_name;
  int layer_order;
} drawlayer_layer_cache_key_t;

typedef dt_drawlayer_runtime_request_t drawlayer_runtime_request_t;
typedef dt_drawlayer_runtime_context_t drawlayer_runtime_host_context_t;

#define _commit_dabs dt_drawlayer_commit_dabs
#define _flush_layer_cache dt_drawlayer_flush_layer_cache
#define _flush_process_patch_to_base dt_drawlayer_flush_process_patch_to_base
#define _sync_widget_cache dt_drawlayer_sync_widget_cache
#define _set_drawlayer_pipeline_realtime_mode dt_drawlayer_set_pipeline_realtime_mode
#define _prime_live_process_patch_before_stroke dt_drawlayer_prime_live_process_patch_before_stroke
#define _ensure_layer_cache dt_drawlayer_ensure_layer_cache
#define _build_process_patch_from_base dt_drawlayer_build_process_patch_from_base
#define _release_all_base_patch_extra_refs dt_drawlayer_release_all_base_patch_extra_refs
#define _drawlayer_wait_for_rasterization_modal dt_drawlayer_wait_for_rasterization_modal
#define _set_drawlayer_os_cursor_hidden dt_drawlayer_set_os_cursor_hidden
#define _current_live_padding dt_drawlayer_current_live_padding
#define _drawlayer_runtime_collect_inputs NULL
#define _drawlayer_runtime_perform_action NULL

gboolean dt_drawlayer_commit_dabs(dt_iop_module_t *self, gboolean record_history);
gboolean dt_drawlayer_flush_layer_cache(dt_iop_module_t *self);
static gboolean _compute_view_patch(dt_iop_module_t *self, float padding, drawlayer_view_patch_info_t *view);
static gboolean _widget_to_layer_coords(dt_iop_module_t *self, const double wx, const double wy, float *lx,
                                        float *ly);
static gboolean _layer_to_widget_coords(dt_iop_module_t *self, const float x, const float y, float *wx, float *wy);
static gboolean _widget_points_to_layer_coords(dt_iop_module_t *self, float *pts, int count);
static gboolean _layer_points_to_widget_coords(dt_iop_module_t *self, float *pts, int count);
static void _process_backend_dab(dt_iop_module_t *self, const dt_drawlayer_brush_dab_t *dab,
                                 drawlayer_paint_backend_ctx_t *ctx);
static void _touch_stroke_commit_hash(dt_iop_drawlayer_params_t *params, int dab_count, gboolean have_last_dab,
                                      float last_dab_x, float last_dab_y, uint32_t publish_serial);
void dt_drawlayer_flush_process_patch_to_base(dt_iop_module_t *self, dt_iop_drawlayer_gui_data_t *g);
static void _flush_process_patch_to_base_locked(dt_iop_module_t *self, dt_iop_drawlayer_gui_data_t *g);
static void _sync_mode_sensitive_widgets(dt_iop_module_t *self);
gboolean dt_drawlayer_sync_widget_cache(dt_iop_module_t *self);
void dt_drawlayer_set_pipeline_realtime_mode(dt_iop_module_t *self, gboolean state);
static int _offer_missing_layer_recreation(dt_iop_module_t *self, const char *missing_name);
static gboolean _background_layer_job_done_idle(gpointer user_data);
static gboolean _replay_finished_stroke_to_base_patch(dt_iop_module_t *self, const GArray *raw_inputs);
gboolean dt_drawlayer_prime_live_process_patch_before_stroke(dt_iop_module_t *self);
float dt_drawlayer_current_live_padding(dt_iop_module_t *self);
static dt_drawlayer_runtime_result_t _update_gui_runtime_manager(dt_iop_module_t *self,
                                                                 dt_iop_drawlayer_gui_data_t *g,
                                                                 dt_drawlayer_runtime_event_t event,
                                                                 gboolean flush_pending);

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
    const dt_iop_order_iccprofile_info_t *const display_profile
        = dt_ioppr_get_pipe_output_profile_info(self->dev->pipe);
    const dt_iop_order_iccprofile_info_t *const work_profile
        = dt_ioppr_get_iop_work_profile_info(self, self->dev->iop);
    if(display_profile && work_profile)
    {
      float in[4] = { raw_display_rgb[0], raw_display_rgb[1], raw_display_rgb[2], 0.0f };
      float out[4] = { raw_display_rgb[0], raw_display_rgb[1], raw_display_rgb[2], 0.0f };
      dt_ioppr_transform_image_colorspace_rgb(in, out, 1, 1, display_profile, work_profile,
                                              "drawlayer brush color");
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

static void _fill_input_layer_coords(dt_iop_module_t *self, dt_drawlayer_paint_raw_input_t *input)
{
  if(!input) return;
  input->have_layer_coords = FALSE;
  input->lx = 0.0f;
  input->ly = 0.0f;
  if(!self) return;

  float lx = 0.0f;
  float ly = 0.0f;
  if(_widget_to_layer_coords(self, input->wx, input->wy, &lx, &ly))
  {
    input->lx = lx;
    input->ly = ly;
    input->have_layer_coords = TRUE;
  }
}

static gboolean _build_worker_input_dab(dt_iop_module_t *self, dt_drawlayer_paint_stroke_t *state,
                                        const dt_drawlayer_paint_raw_input_t *input, dt_drawlayer_brush_dab_t *dab)
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

  float lx = input->lx;
  float ly = input->ly;
  if(!input->have_layer_coords && !_widget_to_layer_coords(self, input->wx, input->wy, &lx, &ly)) return FALSE;

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

/* GUI worker painting into uint8 buffers is deimplemented. Realtime preview
 * now relies on the regular pipeline/backbuffer path. */

struct drawlayer_paint_backend_ctx_t
{
  dt_iop_module_t *self;
  dt_drawlayer_paint_stroke_t *stroke;
};

static gboolean _paint_build_dab_cb(void *user_data, dt_drawlayer_paint_stroke_t *state,
                                    const dt_drawlayer_paint_raw_input_t *input, dt_drawlayer_brush_dab_t *out_dab)
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
  if(ctx && ctx->self) _process_backend_dab(ctx->self, (const dt_drawlayer_brush_dab_t *)dab, ctx);
}

static void _paint_emit_noop_cb(void *user_data, const dt_drawlayer_brush_dab_t *dab)
{
  (void)user_data;
  (void)dab;
}

static void _paint_stroke_seed_cb(void *user_data, const uint64_t stroke_seed)
{
  drawlayer_paint_backend_ctx_t *ctx = (drawlayer_paint_backend_ctx_t *)user_data;
  if(!ctx) return;
  if(ctx->stroke) dt_drawlayer_paint_runtime_set_stroke_seed(ctx->stroke, stroke_seed);
}

static void _publish_backend_progress(drawlayer_paint_backend_ctx_t *ctx, const gboolean flush_pending)
{
  if(!ctx || !ctx->self || !flush_pending) return;

  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)ctx->self->gui_data;
  if(!g || !g->stroke.live_publish_damage.valid) return;

  dt_drawlayer_process_state_publish_locked(&g->process, &g->stroke.live_publish_damage, FALSE);
  g->stroke.live_publish_serial++;

  dt_develop_t *dev = ctx->self->dev;
  if(!dev) return;

  dt_iop_drawlayer_params_t *params = (dt_iop_drawlayer_params_t *)ctx->self->params;
  const int sample_count = (int)g->stroke.stroke_sample_count;
  _touch_stroke_commit_hash(params, sample_count, g->stroke.last_dab_valid, g->stroke.last_dab_x,
                            g->stroke.last_dab_y, g->stroke.live_publish_serial);

  dt_pthread_rwlock_wrlock(&dev->history_mutex);
  dt_dev_add_history_item_ext(dev, ctx->self, FALSE, FALSE);
  dt_dev_set_history_hash(dev, dt_dev_history_compute_hash(dev));
  dt_pthread_rwlock_unlock(&dev->history_mutex);
  dt_drawlayer_paint_runtime_state_reset(&g->stroke.live_publish_damage);
  g->stroke.live_publish_ts = g_get_monotonic_time();
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
  dt_drawlayer_paint_interpolate_path(stroke, &callbacks, &ctx);

  if(stroke->pending_dabs && stroke->pending_dabs->len > 0)
  {
    const gint64 live_publish_interval_us
        = MAX((gint64)dt_gui_throttle_get_pipe_runtime_us(DT_DEV_PIXELPIPE_FULL), (gint64)20000);
    const gint64 input_ts = input->event_ts ? input->event_ts : g_get_monotonic_time();
    if(g->stroke.live_publish_ts == 0)
      g->stroke.live_publish_ts = input_ts - live_publish_interval_us;

    if(input_ts - g->stroke.live_publish_ts >= live_publish_interval_us)
    {
      dt_drawlayer_cache_patch_wrlock(&g->process.process_patch);
      guint processed_dabs = 0;
      const double batch_t0 = dt_get_wtime();
      while(processed_dabs < stroke->pending_dabs->len)
      {
        const dt_drawlayer_brush_dab_t *dab
            = &g_array_index(stroke->pending_dabs, dt_drawlayer_brush_dab_t, processed_dabs);
        _process_backend_dab(self, dab, &ctx);
        processed_dabs++;
        const gint64 elapsed_us = (gint64)(1000000.0 * (dt_get_wtime() - batch_t0));
        if(elapsed_us >= live_publish_interval_us) break;
      }
      if(processed_dabs > 0) g_array_remove_range(stroke->pending_dabs, 0, processed_dabs);
      dt_drawlayer_cache_patch_wrunlock(&g->process.process_patch);
      if(processed_dabs > 0) _publish_backend_progress(&ctx, TRUE);
    }
  }
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

  dt_drawlayer_damaged_rect_t backend_damage = { 0 };
  if(dt_drawlayer_paint_merge_runtime_stroke_damage(g->process.backend_path, &backend_damage)) g->process.cache_dirty = TRUE;
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
  if(scratch->stroke)
  {
    if(scratch->stroke->history) g_array_free(scratch->stroke->history, TRUE);
    if(scratch->stroke->pending_dabs) g_array_free(scratch->stroke->pending_dabs, TRUE);
    if(scratch->stroke->dab_window) g_array_free(scratch->stroke->dab_window, TRUE);
    dt_drawlayer_paint_runtime_private_destroy(&scratch->stroke);
  }
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
  scratch->stroke->history = g_array_new(FALSE, FALSE, sizeof(dt_drawlayer_brush_dab_t));
  if(!scratch->stroke->history)
  {
    dt_drawlayer_paint_runtime_private_destroy(&scratch->stroke);
    dt_free(scratch);
    return NULL;
  }
  scratch->stroke->pending_dabs = g_array_new(FALSE, FALSE, sizeof(dt_drawlayer_brush_dab_t));
  if(!scratch->stroke->pending_dabs)
  {
    g_array_free(scratch->stroke->history, TRUE);
    scratch->stroke->history = NULL;
    dt_drawlayer_paint_runtime_private_destroy(&scratch->stroke);
    dt_free(scratch);
    return NULL;
  }
  scratch->stroke->dab_window = g_array_new(FALSE, FALSE, sizeof(dt_drawlayer_brush_dab_t));
  if(!scratch->stroke->dab_window)
  {
    g_array_free(scratch->stroke->pending_dabs, TRUE);
    scratch->stroke->pending_dabs = NULL;
    g_array_free(scratch->stroke->history, TRUE);
    scratch->stroke->history = NULL;
    dt_drawlayer_paint_runtime_private_destroy(&scratch->stroke);
    dt_free(scratch);
    return NULL;
  }

  g_private_set(&_drawlayer_fullres_replay_scratch_key, scratch);
  return scratch;
}

static float *_ensure_fullres_replay_float_buffer(float **buffer, size_t *capacity_values,
                                                  const size_t needed_values)
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

static gboolean _replay_finished_stroke_to_base_patch(dt_iop_module_t *self, const GArray *raw_inputs)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !raw_inputs || raw_inputs->len == 0) return FALSE;
  if(!g->process.base_patch.pixels || g->process.base_patch.width <= 0 || g->process.base_patch.height <= 0) return FALSE;

  drawlayer_fullres_replay_scratch_t *scratch = _get_fullres_replay_scratch();
  if(!scratch || !scratch->stroke) return FALSE;

  dt_drawlayer_paint_stroke_t *stroke = scratch->stroke;
  dt_drawlayer_paint_path_state_reset(stroke);
  dt_drawlayer_paint_runtime_private_reset(stroke);
  if(!stroke->history)
  {
    stroke->history = g_array_new(FALSE, FALSE, sizeof(dt_drawlayer_brush_dab_t));
    if(!stroke->history) return FALSE;
  }
  g_array_set_size(stroke->history, 0);
  if(!stroke->pending_dabs)
  {
    stroke->pending_dabs = g_array_new(FALSE, FALSE, sizeof(dt_drawlayer_brush_dab_t));
    if(!stroke->pending_dabs) return FALSE;
  }
  g_array_set_size(stroke->pending_dabs, 0);
  if(stroke->dab_window) g_array_set_size(stroke->dab_window, 0);

  drawlayer_paint_backend_ctx_t replay_ctx = {
    .self = self,
    .stroke = stroke,
  };
  const dt_drawlayer_paint_callbacks_t callbacks = {
    .build_dab = _paint_build_dab_cb,
    .layer_to_widget = _paint_layer_to_widget_cb,
    .emit_dab = _paint_emit_noop_cb,
    .on_stroke_seed = _paint_stroke_seed_cb,
  };
  for(guint i = 0; i < raw_inputs->len; i++)
  {
    const dt_drawlayer_paint_raw_input_t *input = &g_array_index(raw_inputs, dt_drawlayer_paint_raw_input_t, i);
    if(!dt_drawlayer_paint_queue_raw_input(stroke, input)) return FALSE;
  }
  dt_drawlayer_paint_interpolate_path(stroke, &callbacks, &replay_ctx);
  dt_drawlayer_paint_finalize_path(stroke, &callbacks, &replay_ctx);
  if(!stroke->history || stroke->history->len == 0 || !stroke->pending_dabs || stroke->pending_dabs->len == 0)
    return FALSE;

  dt_drawlayer_damaged_rect_t replay_bounds = { 0 };
  for(guint i = 0; i < stroke->history->len; i++)
  {
    const dt_drawlayer_brush_dab_t *dab = &g_array_index(stroke->history, dt_drawlayer_brush_dab_t, i);
    const int x0 = CLAMP((int)floorf(dab->x - dab->radius - 1.0f), 0, g->process.base_patch.width - 1);
    const int y0 = CLAMP((int)floorf(dab->y - dab->radius - 1.0f), 0, g->process.base_patch.height - 1);
    const int x1 = CLAMP((int)ceilf(dab->x + dab->radius + 1.0f), 0, g->process.base_patch.width);
    const int y1 = CLAMP((int)ceilf(dab->y + dab->radius + 1.0f), 0, g->process.base_patch.height);
    const dt_drawlayer_damaged_rect_t dab_bounds = {
      .valid = TRUE,
      .nw = { x0, y0 },
      .se = { x1, y1 },
    };
    dt_drawlayer_paint_runtime_note_dab_damage(&replay_bounds, &dab_bounds);
  }
  if(!replay_bounds.valid) return FALSE;

  const int replay_width = replay_bounds.se[0] - replay_bounds.nw[0];
  const int replay_height = replay_bounds.se[1] - replay_bounds.nw[1];
  if(replay_width <= 0 || replay_height <= 0) return FALSE;

  float *replay_pixels = _ensure_fullres_replay_float_buffer(
      &scratch->replay_pixels, &scratch->replay_pixels_capacity, (size_t)replay_width * replay_height * 4);
  if(!replay_pixels) return FALSE;

  float *stroke_mask = _ensure_fullres_replay_float_buffer(&scratch->stroke_mask, &scratch->stroke_mask_capacity,
                                                           (size_t)replay_width * replay_height);
  if(!stroke_mask) return FALSE;
  memset(stroke_mask, 0, (size_t)replay_width * replay_height * sizeof(float));
  dt_drawlayer_damaged_rect_t replay_damage = { 0 };
  dt_drawlayer_cache_patch_t replay_patch = {
    .x = replay_bounds.nw[0],
    .y = replay_bounds.nw[1],
    .width = replay_width,
    .height = replay_height,
    .pixels = replay_pixels,
    .external_alloc = TRUE,
  };
  dt_drawlayer_cache_patch_t replay_stroke_mask = {
    .x = replay_bounds.nw[0],
    .y = replay_bounds.nw[1],
    .width = replay_width,
    .height = replay_height,
    .pixels = stroke_mask,
    .external_alloc = TRUE,
  };

  dt_drawlayer_cache_patch_rdlock(&g->process.base_patch);
  for(int yy = 0; yy < replay_height; yy++)
  {
    const float *src = g->process.base_patch.pixels
                       + ((size_t)(replay_bounds.nw[1] + yy) * g->process.base_patch.width + replay_bounds.nw[0]) * 4;
    float *dst = replay_pixels + (size_t)yy * replay_width * 4;
    memcpy(dst, src, (size_t)replay_width * 4 * sizeof(float));
  }
  dt_drawlayer_cache_patch_rdunlock(&g->process.base_patch);

  const gboolean wrote_replay_tile
      = dt_drawlayer_paint_raster_path(stroke->pending_dabs, _clamp01(stroke->distance_percent), &replay_patch, 1.0f,
                                       &replay_stroke_mask, &replay_damage, stroke);
  if(wrote_replay_tile)
  {
    dt_drawlayer_cache_patch_wrlock(&g->process.base_patch);
    for(int yy = 0; yy < replay_height; yy++)
    {
      const float *src = replay_pixels + (size_t)yy * replay_width * 4;
      float *dst = g->process.base_patch.pixels
                   + ((size_t)(replay_bounds.nw[1] + yy) * g->process.base_patch.width + replay_bounds.nw[0]) * 4;
      memcpy(dst, src, (size_t)replay_width * 4 * sizeof(float));
    }
#ifdef HAVE_OPENCL
    dt_dev_pixelpipe_cache_flush_host_pinned_image(darktable.pixelpipe_cache, g->process.base_patch.pixels,
                                                   g->process.base_patch.cache_entry, -1);
#endif
    dt_drawlayer_cache_patch_wrunlock(&g->process.base_patch);
  }
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
   * preserved raw input history when the stroke commits. */
  gboolean have_process_damage = FALSE;
  dt_drawlayer_damaged_rect_t process_step_path = { 0 };
  dt_drawlayer_damaged_rect_t process_step_damage = { 0 };
  dt_drawlayer_paint_runtime_state_reset(&process_step_path);

  /* Caller keeps `process_patch` write-locked for the whole raw-input batch so
   * dense strokes don't thrash the rwlock once per dab. */
  if(g->process.process_patch_valid && g->process.process_patch.pixels && g->process.process_patch.width > 0 && g->process.process_patch.height > 0
     && g->process.process_combined_roi.scale > 1e-6f)
  {
    dt_drawlayer_cache_patch_t process_patch = g->process.process_patch;
    process_patch.x = g->process.process_combined_roi.x;
    process_patch.y = g->process.process_combined_roi.y;
    dt_drawlayer_cache_patch_t process_stroke_mask = g->process.process_stroke_mask;
    process_stroke_mask.x = process_patch.x;
    process_stroke_mask.y = process_patch.y;
    dt_drawlayer_paint_rasterize_segment_to_buffer(
        dab, _clamp01(stroke->distance_percent), &process_patch, g->process.process_combined_roi.scale,
        &process_stroke_mask, &process_step_path, stroke);
    have_process_damage = dt_drawlayer_paint_runtime_get_stroke_damage(&process_step_path, &process_step_damage);
    if(have_process_damage)
    {
      g->process.process_patch_dirty = TRUE;
      dt_drawlayer_paint_runtime_note_dab_damage(&g->process.process_dirty_rect, &process_step_damage);
    }
  }

  if(have_process_damage)
  {
    g->process.cache_dirty = TRUE;
    dt_drawlayer_paint_runtime_note_dab_damage(&g->stroke.live_publish_damage, &process_step_damage);

    g->stroke.last_dab_valid = TRUE;
    g->stroke.last_dab_x = dab->x;
    g->stroke.last_dab_y = dab->y;
  }
}

static gboolean _layer_bounds_to_widget_bounds(dt_iop_module_t *self, const float x0, const float y0,
                                               const float x1, const float y1, float *left, float *top,
                                               float *right, float *bottom)
{
  if(!self || !self->dev || !self->dev->virtual_pipe) return FALSE;

  float pts[8] = {
    x0, y0, x1, y0, x0, y1, x1, y1,
  };

  if(!_layer_points_to_widget_coords(self, pts, 4)) return FALSE;

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

static void _default_layer_name(dt_iop_module_t *self, char *name, const size_t name_size)
{
  const char *suffix = "0";
  if(self && self->multi_name[0] != '\0')
    suffix = self->multi_name;
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

static gboolean _get_current_work_profile_key(dt_iop_module_t *self, GList *iop_list, dt_dev_pixelpipe_t *pipe,
                                              char *key, const size_t key_size)
{
  if(!key || key_size == 0) return FALSE;
  key[0] = '\0';
  if(!self || !pipe || !iop_list) return FALSE;

  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_iop_work_profile_info(self, iop_list);
  if(!work_profile) return FALSE;

  g_snprintf(key, key_size, "%d|%d|%s", (int)work_profile->type, (int)work_profile->intent,
             work_profile->filename);
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
  drawlayer_process_scratch_t *scratch
      = (drawlayer_process_scratch_t *)g_private_get(&_drawlayer_process_scratch_key);
  if(scratch) return scratch;

  scratch = g_malloc0(sizeof(*scratch));
  if(!scratch) return NULL;
  g_private_set(&_drawlayer_process_scratch_key, scratch);
  return scratch;
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
  dt_dev_pixelpipe_cache_flush_host_pinned_image(darktable.pixelpipe_cache, published.pixels,
                                                 published.cache_entry, -1);
#endif
  dt_drawlayer_cache_patch_clear(&published, "drawlayer patch");
  if(darktable.unmuted & DT_DEBUG_VERBOSE)
    dt_print(DT_DEBUG_PERF,
             "[drawlayer] cache rekey conflict old=%" PRIu64 " new=%" PRIu64 " -> published snapshot instead\n",
             patch->cache_hash, new_hash);
  return TRUE;
}

static void _retain_base_patch_loaded_ref(dt_iop_drawlayer_gui_data_t *g)
{
  if(!g || !g->process.base_patch.cache_entry || g->process.base_patch_loaded_ref) return;
  dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, g->process.base_patch.cache_hash, TRUE,
                                         g->process.base_patch.cache_entry);
  g->process.base_patch_loaded_ref = TRUE;
}

static void _retain_base_patch_stroke_ref(dt_iop_drawlayer_gui_data_t *g)
{
  if(!g || !g->process.base_patch.cache_entry) return;
  dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, g->process.base_patch.cache_hash, TRUE,
                                         g->process.base_patch.cache_entry);
  g->process.base_patch_stroke_refs++;
}

void dt_drawlayer_release_all_base_patch_extra_refs(dt_iop_drawlayer_gui_data_t *g)
{
  if(!g) return;
  if(!g->process.base_patch.cache_entry)
  {
    /* Keep refcount bookkeeping state coherent even if the cache entry has
     * already been detached/cleared. This prevents stale counters from being
     * applied to a future reused `g->process.base_patch` entry. */
    g->process.base_patch_loaded_ref = FALSE;
    g->process.base_patch_stroke_refs = 0;
    return;
  }

  if(g->process.base_patch_loaded_ref)
  {
    dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, g->process.base_patch.cache_hash, FALSE,
                                           g->process.base_patch.cache_entry);
    g->process.base_patch_loaded_ref = FALSE;
  }

  while(g->process.base_patch_stroke_refs > 0)
  {
    dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, g->process.base_patch.cache_hash, FALSE,
                                           g->process.base_patch.cache_entry);
    g->process.base_patch_stroke_refs--;
  }
}

static gboolean _refresh_piece_base_cache(dt_iop_module_t *self, dt_iop_drawlayer_data_t *data,
                                          const dt_iop_drawlayer_params_t *params, dt_dev_pixelpipe_iop_t *piece)
{
  if(!self || !data || !params || !piece) return FALSE;
  if(params->layer_name[0] == '\0')
  {
    dt_drawlayer_cache_patch_clear(&data->process.base_patch, "drawlayer patch");
    data->process.cache_valid = FALSE;
    data->process.cache_dirty = FALSE;
    data->process.cache_imgid = -1;
    data->process.cache_layer_name[0] = '\0';
    data->process.cache_layer_order = -1;
    dt_drawlayer_process_state_invalidate(&data->process);
    return TRUE;
  }

  const uint64_t base_hash = _drawlayer_params_cache_hash(piece->pipe->image.id, params);
  const int known_width = data->process.base_patch.width;
  const int known_height = data->process.base_patch.height;
  if(data->process.cache_valid && data->process.base_patch.cache_entry
     && data->process.base_patch.cache_hash == base_hash && data->process.cache_imgid == piece->pipe->image.id
     && !g_strcmp0(data->process.cache_layer_name, params->layer_name))
    return TRUE;

  dt_drawlayer_cache_patch_clear(&data->process.base_patch, "drawlayer patch");
  data->process.cache_valid = FALSE;
  data->process.cache_dirty = FALSE;
  data->process.cache_imgid = -1;
  data->process.cache_layer_name[0] = '\0';
  data->process.cache_layer_order = -1;
  dt_drawlayer_process_state_invalidate(&data->process);

  if(known_width > 0 && known_height > 0)
  {
    int created = 0;
    if(!dt_drawlayer_cache_patch_alloc_shared(&data->process.base_patch,
                                              _drawlayer_params_cache_hash(piece->pipe->image.id, params),
                                              (size_t)known_width * known_height, known_width, known_height,
                                              "drawlayer sidecar cache", &created))
      return FALSE;

    if(!created)
    {
      data->process.cache_valid = TRUE;
      data->process.cache_imgid = piece->pipe->image.id;
      g_strlcpy(data->process.cache_layer_name, params->layer_name, sizeof(data->process.cache_layer_name));
      data->process.cache_layer_order = params->layer_order;
      return TRUE;
    }

    char warm_path[PATH_MAX] = { 0 };
    gboolean warm_loaded = FALSE;
    if(dt_drawlayer_io_sidecar_path(piece->pipe->image.id, warm_path, sizeof(warm_path))
       && g_file_test(warm_path, G_FILE_TEST_EXISTS))
    {
      dt_drawlayer_io_patch_t warm_patch = { 0 };
      dt_drawlayer_cache_patch_wrlock(&data->process.base_patch);
      warm_patch.x = data->process.base_patch.x;
      warm_patch.y = data->process.base_patch.y;
      warm_patch.width = data->process.base_patch.width;
      warm_patch.height = data->process.base_patch.height;
      warm_patch.pixels = data->process.base_patch.pixels;
      warm_loaded = dt_drawlayer_io_load_layer(warm_path, params->layer_name, params->layer_order, known_width,
                                               known_height, &warm_patch);
      dt_drawlayer_cache_patch_wrunlock(&data->process.base_patch);
    }

    if(warm_loaded)
    {
      data->process.cache_valid = TRUE;
      data->process.cache_imgid = piece->pipe->image.id;
      g_strlcpy(data->process.cache_layer_name, params->layer_name, sizeof(data->process.cache_layer_name));
      data->process.cache_layer_order = params->layer_order;
      return TRUE;
    }

    dt_drawlayer_cache_patch_clear(&data->process.base_patch, "drawlayer patch");
    data->process.cache_valid = FALSE;
    data->process.cache_dirty = FALSE;
    data->process.cache_imgid = -1;
    data->process.cache_layer_name[0] = '\0';
    data->process.cache_layer_order = -1;
    dt_drawlayer_process_state_invalidate(&data->process);
  }

  char path[PATH_MAX] = { 0 };
  if(!dt_drawlayer_io_sidecar_path(piece->pipe->image.id, path, sizeof(path))) return FALSE;
  if(!g_file_test(path, G_FILE_TEST_EXISTS)) return FALSE;

  dt_drawlayer_io_layer_info_t io_info = { 0 };
  drawlayer_dir_info_t info = { 0 };
  if(!dt_drawlayer_io_find_layer(path, params->layer_name, params->layer_order, &io_info)) return FALSE;
  info.found = io_info.found;
  info.index = io_info.index;
  info.count = io_info.count;
  info.width = io_info.width;
  info.height = io_info.height;
  g_strlcpy(info.name, io_info.name, sizeof(info.name));
  g_strlcpy(info.work_profile, io_info.work_profile, sizeof(info.work_profile));
  if(info.width == 0 || info.height == 0) return FALSE;

  int created = 0;
  if(!dt_drawlayer_cache_patch_alloc_shared(&data->process.base_patch,
                                            _drawlayer_params_cache_hash(piece->pipe->image.id, params),
                                            (size_t)info.width * info.height, (int)info.width, (int)info.height,
                                            "drawlayer sidecar cache", &created))
    return FALSE;

  if(created)
  {
    dt_drawlayer_io_patch_t io_patch = { 0 };
    dt_drawlayer_cache_patch_wrlock(&data->process.base_patch);
    dt_drawlayer_cache_clear_transparent_float(data->process.base_patch.pixels, (size_t)info.width * info.height);
    io_patch.x = data->process.base_patch.x;
    io_patch.y = data->process.base_patch.y;
    io_patch.width = data->process.base_patch.width;
    io_patch.height = data->process.base_patch.height;
    io_patch.pixels = data->process.base_patch.pixels;
    const gboolean loaded = dt_drawlayer_io_load_layer(path, params->layer_name, info.index, (int)info.width,
                                                       (int)info.height, &io_patch);
    dt_drawlayer_cache_patch_wrunlock(&data->process.base_patch);
    if(!loaded)
    {
      dt_drawlayer_cache_patch_clear(&data->process.base_patch, "drawlayer patch");
      data->process.cache_valid = FALSE;
      data->process.cache_dirty = FALSE;
      data->process.cache_imgid = -1;
      data->process.cache_layer_name[0] = '\0';
      data->process.cache_layer_order = -1;
      dt_drawlayer_process_state_invalidate(&data->process);
      return FALSE;
    }
  }

  data->process.cache_valid = TRUE;
  data->process.cache_imgid = piece->pipe->image.id;
  g_strlcpy(data->process.cache_layer_name, params->layer_name, sizeof(data->process.cache_layer_name));
  data->process.cache_layer_order = info.index;
  return TRUE;
}

gboolean dt_drawlayer_build_process_patch_from_base(dt_iop_module_t *self, dt_iop_drawlayer_gui_data_t *g,
                                                    const dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_in,
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
  if(!self || !g || !piece || !roi_out || !g->process.base_patch.pixels) return FALSE;
  const int current_full_w = piece->pipe->iwidth;
  const int current_full_h = piece->pipe->iheight;
  if(current_full_w <= 0 || current_full_h <= 0) return FALSE;

  const dt_iop_roi_t process_roi = roi_in ? *roi_in : *roi_out;
  dt_iop_roi_t combined_roi = { 0 };
  dt_drawlayer_cache_build_combined_process_roi_for_piece(piece, &process_roi, current_full_w, current_full_h,
                                                          g->process.base_patch.width, g->process.base_patch.height,
                                                          &combined_roi);
  {
    const gint64 now = g_get_monotonic_time();
    if(darktable.unmuted & DT_DEBUG_VERBOSE)
      dt_print(DT_DEBUG_PERF, "[drawlayer] process step=build-process-roi ms=%.3f\n", (now - t) / 1000.0);
    t = now;
  }

  if(combined_roi.scale <= 1e-6f || roi_out->width <= 0 || roi_out->height <= 0) return FALSE;

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
  const gboolean have_pending_live_batch = dt_drawlayer_worker_pending_dab_count(g->stroke.worker) > 0;
  gboolean need_flush = FALSE;
  dt_iop_roi_t previous_process_roi = { 0 };
  int previous_process_width = 0;
  int previous_process_height = 0;
  dt_drawlayer_cache_patch_rdlock(&g->process.process_patch);
  have_process_patch = (g->process.process_patch_valid && g->process.process_patch.pixels);
  if(have_process_patch)
  {
    previous_process_roi = g->process.process_combined_roi;
    previous_process_width = g->process.process_patch.width;
    previous_process_height = g->process.process_patch.height;
    same_geometry = (g->process.process_patch_padding == process_pad && g->process.process_patch.width == patch_width
                     && g->process.process_patch.height == patch_height && g->process.process_snapshot_valid
                     && g->process.process_read_patch.pixels
                     && g->process.process_read_patch.width == patch_width && g->process.process_read_patch.height == patch_height
                     && abs(g->process.process_combined_roi.x - padded_roi.x) <= 2
                     && abs(g->process.process_combined_roi.y - padded_roi.y) <= 2
                     && g->process.process_combined_roi.width == padded_roi.width
                     && g->process.process_combined_roi.height == padded_roi.height
                     && fabs(g->process.process_combined_roi.scale - combined_roi.scale) <= 1e-2);
    if(!same_geometry
       && ((g->manager.painting_active || dt_drawlayer_worker_any_active(g->stroke.worker))
           || have_pending_live_batch))
      keep_live_geometry = (g->process.process_patch_padding == process_pad && g->process.process_patch.width == patch_width
                            && g->process.process_patch.height == patch_height && g->process.process_snapshot_valid
                            && g->process.process_read_patch.pixels);
    need_flush = g->process.process_patch_dirty;
  }
  dt_drawlayer_cache_patch_rdunlock(&g->process.process_patch);
  if(same_geometry)
  {
    const gint64 now = g_get_monotonic_time();
    if(darktable.unmuted & DT_DEBUG_VERBOSE)
      dt_print(DT_DEBUG_PERF, "[drawlayer] process step=process-patch-hit ms=%.3f total=%.3f\n",
               (now - t) / 1000.0, (now - t0) / 1000.0);
    return TRUE;
  }

  /* During active drawing, keep serving the current display-sized process tile
   * even if upstream ROI metadata jitters slightly between recomputes.
   * Rebuilding this tile costs hundreds of ms and kills interaction latency. */
  if(keep_live_geometry)
  {
    const gint64 now = g_get_monotonic_time();
    if(darktable.unmuted & DT_DEBUG_VERBOSE)
      dt_print(DT_DEBUG_PERF,
               "[drawlayer] process step=process-patch-keep-live ms=%.3f total=%.3f painting=%d workers=%d pending=%d\n",
               (now - t) / 1000.0, (now - t0) / 1000.0, g->manager.painting_active ? 1 : 0,
               dt_drawlayer_worker_any_active(g->stroke.worker) ? 1 : 0, have_pending_live_batch ? 1 : 0);
    return TRUE;
  }

  if(have_process_patch)
  {
    if(darktable.unmuted & DT_DEBUG_VERBOSE)
      dt_print(DT_DEBUG_PERF,
               "[drawlayer] process step=process-patch-miss old=(x=%d y=%d w=%d h=%d s=%.6f pw=%d ph=%d) "
               "new=(x=%d y=%d w=%d h=%d s=%.6f pw=%d ph=%d)\n",
               previous_process_roi.x, previous_process_roi.y, previous_process_roi.width,
               previous_process_roi.height, previous_process_roi.scale, previous_process_width,
               previous_process_height, padded_roi.x, padded_roi.y, padded_roi.width, padded_roi.height,
               padded_roi.scale, patch_width, patch_height);
  }

  if(need_flush)
  {
    /* Geometry changed: fold incremental process-tile edits back into base
     * only if this transformed tile actually diverged from base content. */
    _flush_process_patch_to_base(self, g);
    _rekey_shared_base_patch(&g->process.base_patch, self->dev->image_storage.id,
                             (const dt_iop_drawlayer_params_t *)self->params);
    {
      const gint64 now = g_get_monotonic_time();
      if(darktable.unmuted & DT_DEBUG_VERBOSE)
        dt_print(DT_DEBUG_PERF, "[drawlayer] process step=flush-old-process-patch ms=%.3f\n",
                 (now - t) / 1000.0);
      t = now;
    }
  }

  gboolean populated = FALSE;
  if(!dt_drawlayer_cache_ensure_process_patch_buffer(&g->process.process_patch, &g->process.process_stroke_mask,
                                                     patch_width, patch_height, "drawlayer process tile",
                                                     "drawlayer process stroke mask"))
    return FALSE;
  dt_drawlayer_cache_patch_wrlock(&g->process.process_patch);
    g->process.process_snapshot_valid = FALSE;
    /* Populate a display-sized `process_patch` and its transformed
     * `process_stroke_mask` from the authoritative full-resolution
     * `base_patch` and `stroke_mask`. `stroke_mask` lives in base/source
     * coordinates and represents accumulated stroke alpha at full res; the
     * helper resamples it into `process_stroke_mask` aligned with the
     * `process_patch` so realtime blending and previews can use a display-
     * sized mask without resampling on-the-fly. */
    populated = dt_drawlayer_cache_populate_process_patch_from_base(
      &g->process.base_patch, &g->process.stroke_mask, &g->process.process_patch, &g->process.process_stroke_mask, &padded_roi, process_pad,
      patch_width, patch_height, &g->process.process_patch_valid, &g->process.process_patch_dirty, &g->process.process_dirty_rect,
      &g->process.process_patch_padding, &g->process.process_combined_roi, "drawlayer process tile",
      "drawlayer process stroke mask");
  dt_drawlayer_cache_patch_wrunlock(&g->process.process_patch);
  if(!populated) return FALSE;
  dt_drawlayer_process_state_publish_locked(&g->process, NULL, TRUE);
  {
    const gint64 now = g_get_monotonic_time();
    if(darktable.unmuted & DT_DEBUG_VERBOSE)
      dt_print(DT_DEBUG_PERF, "[drawlayer] process step=build-process-patch-clipzoom ms=%.3f\n",
               (now - t) / 1000.0);
    t = now;
  }
  {
    const gint64 now = g_get_monotonic_time();
    if(darktable.unmuted & DT_DEBUG_VERBOSE)
      dt_print(DT_DEBUG_PERF, "[drawlayer] process step=build-process-patch-finalize ms=%.3f total=%.3f\n",
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
  const gboolean was_dirty = g->process.process_patch_dirty;
  if(!dt_drawlayer_cache_flush_process_patch_to_base(
         &g->process.base_patch, &g->process.stroke_mask, &g->process.process_combined_roi, &g->process.process_patch, &g->process.process_stroke_mask,
         &scratch->flush_update_rgba, &scratch->flush_update_rgba_pixels, &g->process.cache_dirty, &g->process.process_patch_dirty,
         &g->process.process_dirty_rect, "drawlayer process update tile"))
    return;

  if(was_dirty && !g->process.process_patch_dirty && self && self->params && self->dev)
    _rekey_shared_base_patch(&g->process.base_patch, self->dev->image_storage.id,
                             (const dt_iop_drawlayer_params_t *)self->params);
}

void dt_drawlayer_flush_process_patch_to_base(dt_iop_module_t *self, dt_iop_drawlayer_gui_data_t *g)
{
  if(!g) return;
  if(!g->manager.painting_active && dt_drawlayer_worker_any_active(g->stroke.worker))
    dt_drawlayer_worker_flush_finished_strokes(g->stroke.worker);
  dt_drawlayer_cache_patch_wrlock(&g->process.process_patch);
  _flush_process_patch_to_base_locked(self, g);
  dt_drawlayer_cache_patch_wrunlock(&g->process.process_patch);
}

static void _blend_layer_over_input(float *output, const float *input, const float *layerbuf, const size_t pixels,
                                    const gboolean use_preview_bg, const float preview_bg)
{
  if(!output || !input || !layerbuf || pixels == 0) return;

  const dt_aligned_pixel_simd_t preview_base = { preview_bg, preview_bg, preview_bg, 1.0f };

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)                                                           \
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

static gboolean _drawlayer_sync_host_image_to_device(const int devid, cl_mem device_image, void *host_pixels,
                                                     const int width, const int height, const int bpp)
{
  if(!device_image || !host_pixels || width <= 0 || height <= 0 || bpp <= 0) return FALSE;

  const cl_mem_flags flags = dt_opencl_get_mem_flags(device_image);
  if(flags & CL_MEM_USE_HOST_PTR)
  {
    void *mapped = dt_opencl_map_image(devid, device_image, TRUE, CL_MAP_WRITE, width, height, bpp);
    if(mapped)
    {
      if(mapped != host_pixels) memcpy(mapped, host_pixels, (size_t)width * height * bpp);
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
                                                const gboolean force_device_copy, const gboolean realtime_reuse,
                                                const int source_w, const int source_h,
                                                drawlayer_cl_image_handle_t *source)
{
  if(!source || !layer_pixels || source_w <= 0 || source_h <= 0) return FALSE;
  *source = (drawlayer_cl_image_handle_t){ 0 };

  if(force_device_copy)
  {
    source->mem
        = dt_opencl_copy_host_to_device(devid, (void *)layer_pixels, source_w, source_h, 4 * sizeof(float));
    return source->mem != NULL;
  }

  source->mem = dt_dev_pixelpipe_cache_get_pinned_image(
      darktable.pixelpipe_cache, (void *)layer_pixels, resolved_entry, devid, source_w, source_h,
      4 * sizeof(float), CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR, NULL, NULL);
  if(source->mem)
  {
    source->is_pinned = TRUE;
    return TRUE;
  }

  if(realtime_reuse && resolved_entry)
  {
    source->mem = dt_pixel_cache_clmem_get(resolved_entry, NULL, devid, source_w, source_h, 4 * (int)sizeof(float),
                                           CL_MEM_READ_WRITE, NULL);
    if(source->mem)
    {
      if(_drawlayer_sync_host_image_to_device(devid, source->mem, (void *)layer_pixels, source_w, source_h,
                                              4 * sizeof(float)))
      {
        source->is_cached_device = TRUE;
        return TRUE;
      }

      dt_pixel_cache_clmem_remove(resolved_entry, source->mem);
      dt_opencl_release_mem_object(source->mem);
      source->mem = NULL;
    }
  }

  source->mem = dt_opencl_alloc_device_use_host_pointer(
      devid, source_w, source_h, 4 * sizeof(float), (void *)layer_pixels, CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR);
  if(source->mem)
  {
    if(_drawlayer_sync_host_image_to_device(devid, source->mem, (void *)layer_pixels, source_w, source_h,
                                            4 * sizeof(float)))
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
  const gboolean can_copy_crop
      = (fabs(target_roi->scale - 1.0) <= 1e-6 && fabs(source_roi->scale - 1.0) <= 1e-6 && source_roi->x >= 0
         && source_roi->y >= 0 && source_roi->x + target_roi->width <= source_w
         && source_roi->y + target_roi->height <= source_h && source_roi->width == target_roi->width
         && source_roi->height == target_roi->height);
  if(can_copy_crop)
  {
    size_t src_origin[3] = { (size_t)source_roi->x, (size_t)source_roi->y, 0 };
    size_t dst_origin[3] = { 0, 0, 0 };
    size_t region[3] = { (size_t)target_roi->width, (size_t)target_roi->height, 1 };
    const int copy_err
        = dt_opencl_enqueue_copy_image(devid, dev_source_rgba, dev_layer_rgba, src_origin, dst_origin, region);
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

  *err = _drawlayer_copy_or_resample_layer_roi(devid, dev_source_rgba, layer->mem, source_w, source_h, target_roi,
                                               source_roi);
  return *err == CL_SUCCESS;
}

static int _drawlayer_run_premult_over_kernel(const int devid, const int kernel_premult_over,
                                              cl_mem dev_background, cl_mem dev_layer_rgba, cl_mem dev_out,
                                              const int width, const int height, const int background_offset_x,
                                              const int background_offset_y)
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

static int _blend_layer_over_input_cl(const int devid, const int kernel_premult_over, cl_mem dev_out,
                                      cl_mem dev_in, drawlayer_process_scratch_t *scratch,
                                      const float *layer_pixels, dt_pixel_cache_entry_t *source_entry,
                                      cl_mem source_mem_override, const int source_w, const int source_h,
                                      const dt_iop_roi_t *const target_roi, const dt_iop_roi_t *const source_roi,
                                      const gboolean direct_copy, const gboolean use_preview_bg,
                                      const float preview_bg, const gboolean realtime_reuse,
                                      const gboolean force_device_copy)
{
  if(devid < 0 || !dev_out || !dev_in || !scratch || (!layer_pixels && !source_mem_override) || source_w <= 0
     || source_h <= 0 || !target_roi || target_roi->width <= 0 || target_roi->height <= 0)
    return FALSE;
  if(kernel_premult_over < 0) return FALSE;

  dt_pixel_cache_entry_t *resolved_entry = source_entry;
  gboolean resolved_entry_ref = FALSE;
  if(realtime_reuse && !resolved_entry)
  {
    resolved_entry
        = dt_dev_pixelpipe_cache_ref_entry_for_host_ptr(darktable.pixelpipe_cache, (void *)layer_pixels);
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

  if(!_drawlayer_acquire_layer_image(devid, resolved_entry, realtime_reuse, direct_copy, source.mem, source_w,
                                     source_h, target_roi, source_roi, &layer, &err))
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
    dev_background = dt_dev_pixelpipe_cache_get_pinned_image(
        darktable.pixelpipe_cache, background, NULL, devid, target_roi->width, target_roi->height,
        4 * sizeof(float), CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR, NULL, NULL);
    if(!dev_background) goto cleanup;
  }
  else
    dev_background = dev_in;

  err = _drawlayer_run_premult_over_kernel(devid, kernel_premult_over, dev_background, layer.mem, dev_out,
                                           target_roi->width, target_roi->height, 0, 0);
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
  if(use_preview_bg)
    dt_dev_pixelpipe_cache_put_pinned_image(darktable.pixelpipe_cache, scratch->cl_background_rgba, NULL, -1,
                                            (void **)&dev_background);
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
    dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, DT_PIXELPIPE_CACHE_HASH_INVALID, FALSE, resolved_entry);

  if(err != CL_SUCCESS) dt_print(DT_DEBUG_OPENCL, "[drawlayer] process_cl blend path failed: %d\n\n", err);

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

static gboolean _widget_points_to_layer_coords(dt_iop_module_t *self, float *pts, const int count)
{
  if(!self || !self->dev || !self->dev->virtual_pipe || !pts || count <= 0) return FALSE;

  dt_dev_coordinates_widget_to_image_norm(self->dev, pts, count);
  dt_dev_coordinates_image_norm_to_image_abs(self->dev, pts, count);

  if(!dt_dev_distort_backtransform_plus(self->dev, self->dev->virtual_pipe, self->iop_order,
                                        DT_DEV_TRANSFORM_DIR_FORW_EXCL, pts, count))
    return FALSE;

  int offset_x = 0;
  int offset_y = 0;
  _virtual_piece_input_offset(self, &offset_x, &offset_y);
  for(int i = 0; i < count; i++)
  {
    pts[2 * i] += offset_x;
    pts[2 * i + 1] += offset_y;
  }

  return TRUE;
}

static gboolean _layer_points_to_widget_coords(dt_iop_module_t *self, float *pts, const int count)
{
  if(!self || !self->dev || !self->dev->virtual_pipe || !pts || count <= 0) return FALSE;

  int offset_x = 0;
  int offset_y = 0;
  _virtual_piece_input_offset(self, &offset_x, &offset_y);
  for(int i = 0; i < count; i++)
  {
    pts[2 * i] -= offset_x;
    pts[2 * i + 1] -= offset_y;
  }

  if(!dt_dev_distort_transform_plus(self->dev, self->dev->virtual_pipe, self->iop_order,
                                    DT_DEV_TRANSFORM_DIR_FORW_EXCL, pts, count))
    return FALSE;

  dt_dev_coordinates_image_abs_to_image_norm(self->dev, pts, count);
  dt_dev_coordinates_image_norm_to_widget(self->dev, pts, count);
  return TRUE;
}

static gboolean _profile_key_is_sane(const char *value)
{
  if(!value || value[0] == '\0') return FALSE;

  int separators = 0;
  for(const unsigned char *c = (const unsigned char *)value; *c; c++)
  {
    if(*c == '|')
      separators++;
    else if(!g_ascii_isprint(*c))
      return FALSE;
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

static void _ensure_cursor_stamp_surface(dt_iop_module_t *self, const float widget_radius, const float opacity,
                                         const float hardness)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || widget_radius <= 0.0f) return;

  const double ppd = (darktable.gui && darktable.gui->ppd > 0.0) ? darktable.gui->ppd : 1.0;
  float display_rgb[3] = { 0.0f };
  _conf_display_color(display_rgb);
  const int shape = _conf_brush_shape();
  const int size_px = MAX(2, (int)ceil((2.0f * widget_radius + 2.0f) * ppd));

  const gboolean needs_rebuild
      = !g->ui.cursor_surface || g->ui.cursor_surface_size != size_px || fabs(g->ui.cursor_surface_ppd - ppd) > 1e-9
        || fabsf(g->ui.cursor_radius - widget_radius) > 1e-3f || fabsf(g->ui.cursor_opacity - opacity) > 1e-6f
        || fabsf(g->ui.cursor_hardness - hardness) > 1e-6f || g->ui.cursor_shape != shape
        || fabsf(g->ui.cursor_color[0] - display_rgb[0]) > 1e-6f || fabsf(g->ui.cursor_color[1] - display_rgb[1]) > 1e-6f
        || fabsf(g->ui.cursor_color[2] - display_rgb[2]) > 1e-6f;
  if(!needs_rebuild) return;

  dt_drawlayer_ui_cursor_clear(&g->ui);
  g->ui.cursor_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size_px, size_px);
  if(cairo_surface_status(g->ui.cursor_surface) != CAIRO_STATUS_SUCCESS)
  {
    dt_drawlayer_ui_cursor_clear(&g->ui);
    return;
  }
  cairo_surface_set_device_scale(g->ui.cursor_surface, ppd, ppd);

  unsigned char *data = cairo_image_surface_get_data(g->ui.cursor_surface);
  const int stride = cairo_image_surface_get_stride(g->ui.cursor_surface);
  memset(data, 0, (size_t)stride * size_px);

  dt_drawlayer_brush_dab_t dab = {
    .radius = fmaxf(widget_radius * (float)ppd, 0.5f),
    .shape = shape,
    .hardness = hardness,
    .opacity = opacity,
    .display_color = { display_rgb[0], display_rgb[1], display_rgb[2] },
  };

  const float half = 0.5f * (float)size_px;
  dt_drawlayer_brush_rasterize_dab_argb8(&dab, data, size_px, size_px, stride, half, half, 1.0f);
  cairo_surface_mark_dirty(g->ui.cursor_surface);

  g->ui.cursor_surface_size = size_px;
  g->ui.cursor_surface_ppd = ppd;
  g->ui.cursor_radius = widget_radius;
  g->ui.cursor_opacity = opacity;
  g->ui.cursor_hardness = hardness;
  g->ui.cursor_shape = shape;
  g->ui.cursor_color[0] = display_rgb[0];
  g->ui.cursor_color[1] = display_rgb[1];
  g->ui.cursor_color[2] = display_rgb[2];
}

void dt_drawlayer_set_os_cursor_hidden(const gboolean hidden)
{
  if(!darktable.gui || !darktable.gui->ui) return;

  GtkWidget *center = dt_ui_center(darktable.gui->ui);
  GtkWidget *main = dt_ui_main_window(darktable.gui->ui);
  GdkDisplay *display = gdk_display_get_default();
  if(!display) return;

  const dt_cursor_t cursor_id = hidden ? GDK_BLANK_CURSOR : GDK_LEFT_PTR;
  GdkCursor *cursor = gdk_cursor_new_for_display(display, cursor_id);
  if(!cursor) return;

  if(center && gtk_widget_get_window(center)) gdk_window_set_cursor(gtk_widget_get_window(center), cursor);
  if(main && gtk_widget_get_window(main)) gdk_window_set_cursor(gtk_widget_get_window(main), cursor);

  dt_control_set_cursor(cursor_id);
  g_object_unref(cursor);
}

static drawlayer_wait_dialog_t _show_drawlayer_wait_dialog(const char *title, const char *message)
{
  drawlayer_wait_dialog_t wait = { 0 };
  if(!darktable.gui || !darktable.gui->ui || !title || !title[0] || !message || !message[0]) return wait;

  GtkWidget *dialog = gtk_dialog_new();
  GtkWidget *main = dt_ui_main_window(darktable.gui->ui);
  if(main) gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(main));
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_deletable(GTK_WINDOW(dialog), FALSE);
  gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
  gtk_window_set_title(GTK_WINDOW(dialog), title);
  gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ON_PARENT);

  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(12));
  GtkWidget *spinner = gtk_spinner_new();
  GtkWidget *label = gtk_label_new(message);
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
  /* `gui_focus()` / explicit sidecar save show this dialog and then immediately
   * enter synchronous worker-drain / TIFF-write code on the UI thread. Give GTK
   * a couple of non-blocking iterations so the modal maps and paints before that
   * blocking section starts, otherwise it may never become visible at all. */
  for(int k = 0; k < 2; k++)
    gtk_main_iteration_do(FALSE);
  wait.dialog = dialog;
  return wait;
}

typedef struct drawlayer_modal_wait_state_t
{
  GMainLoop *loop;
  const dt_iop_drawlayer_gui_data_t *g;
} drawlayer_modal_wait_state_t;

static gboolean _drawlayer_modal_wait_tick(gpointer user_data)
{
  drawlayer_modal_wait_state_t *state = (drawlayer_modal_wait_state_t *)user_data;
  if(!state || !state->loop) return G_SOURCE_REMOVE;
  if(!(state->g && dt_drawlayer_worker_any_active(state->g->stroke.worker)))
  {
    g_main_loop_quit(state->loop);
    return G_SOURCE_REMOVE;
  }

  return G_SOURCE_CONTINUE;
}

void dt_drawlayer_wait_for_rasterization_modal(const dt_iop_drawlayer_gui_data_t *g,
                                               const char *title, const char *message)
{
  if(!(g && dt_drawlayer_worker_any_active(g->stroke.worker))) return;

  drawlayer_wait_dialog_t wait = _show_drawlayer_wait_dialog(title, message);
  GMainLoop *loop = g_main_loop_new(NULL, FALSE);
  drawlayer_modal_wait_state_t state = {
    .loop = loop,
    .g = g,
  };
  const guint source_id = g_timeout_add(16, _drawlayer_modal_wait_tick, &state);

  if(g && dt_drawlayer_worker_any_active(g->stroke.worker))
    g_main_loop_run(loop);

  if(source_id) g_source_remove(source_id);
  if(wait.dialog)
  {
    gtk_widget_destroy(wait.dialog);
    wait.dialog = NULL;
  }
  g_main_loop_unref(loop);
}

static void _show_drawlayer_modal_message(const GtkMessageType type, const char *primary, const char *secondary)
{
  if(!darktable.gui || !darktable.gui->ui || !primary || primary[0] == '\0') return;

  GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)),
                                             GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
                                             type, GTK_BUTTONS_OK, "%s", primary);
  if(secondary && secondary[0] != '\0')
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", secondary);
  gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
}

static gboolean _color_picker_set_from_position(dt_iop_module_t *self, const float x, const float y)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !g->ui.widgets || !g->controls.color) return FALSE;

  float display_rgb[3] = { 0.0f };
  if(!dt_drawlayer_widgets_update_from_picker_position(g->ui.widgets, g->controls.color, x, y, display_rgb)) return FALSE;
  _apply_display_brush_color(self, display_rgb, FALSE);
  return TRUE;
}

static gboolean _color_picker_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !g->ui.widgets) return FALSE;
  const double ppd = (darktable.gui && darktable.gui->ppd > 0.0) ? darktable.gui->ppd : 1.0;
  return dt_drawlayer_widgets_draw_picker(g->ui.widgets, widget, cr, ppd);
}

static gboolean _color_swatch_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !g->ui.widgets) return FALSE;
  return dt_drawlayer_widgets_draw_swatch(g->ui.widgets, widget, cr);
}

static gboolean _color_swatch_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !g->ui.widgets || !widget || !event || event->button != 1) return FALSE;

  float display_rgb[3] = { 0.0f };
  if(!dt_drawlayer_widgets_pick_history_color(g->ui.widgets, widget, event->x, event->y, display_rgb)) return FALSE;
  _apply_display_brush_color(self, display_rgb, FALSE);
  return TRUE;
}

static void _sync_brush_profile_preview_widget(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !g->ui.widgets || !g->controls.brush_shape) return;

  dt_drawlayer_widgets_set_brush_profile_preview(g->ui.widgets, _conf_opacity() / 100.0f, _conf_hardness(),
                                                 _conf_sprinkles() / 100.0f, _conf_sprinkle_size(),
                                                 _conf_sprinkle_coarseness() / 100.0f, _conf_brush_shape());
  gtk_widget_queue_draw(g->controls.brush_shape);
}

static gboolean _brush_profile_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !g->ui.widgets) return FALSE;
  const double ppd = (darktable.gui && darktable.gui->ppd > 0.0) ? darktable.gui->ppd : 1.0;
  return dt_drawlayer_widgets_draw_brush_profiles(g->ui.widgets, widget, cr, ppd);
}

static gboolean _brush_profile_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !g->ui.widgets || !widget || !event || event->button != 1) return FALSE;

  int shape = DT_DRAWLAYER_BRUSH_SHAPE_LINEAR;
  if(!dt_drawlayer_widgets_pick_brush_profile(g->ui.widgets, widget, event->x, event->y, &shape)) return FALSE;

  _sync_params_from_gui(self, FALSE);
  _sync_mode_sensitive_widgets(self);
  if(!_update_gui_runtime_manager(self, g, DT_DRAWLAYER_RUNTIME_EVENT_GUI_SYNC_TEMP_BUFFERS, TRUE).ok) return FALSE;
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

  const dt_iop_order_iccprofile_info_t *const work_profile
      = self->dev ? dt_ioppr_get_iop_work_profile_info(self, self->dev->iop) : NULL;
  const dt_iop_order_iccprofile_info_t *const display_profile = dt_ioppr_get_pipe_output_profile_info(pipe);
  if(!work_profile || !display_profile) return TRUE;

  float in[4] = { working_rgb[0], working_rgb[1], working_rgb[2], 0.0f };
  float out[4] = { working_rgb[0], working_rgb[1], working_rgb[2], 0.0f };
  dt_ioppr_transform_image_colorspace_rgb(in, out, 1, 1, work_profile, display_profile, "drawlayer picked color");
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
  const float *picked_min
      = (source == DRAWLAYER_PICK_SOURCE_OUTPUT) ? self->picked_output_color_min : self->picked_color_min;
  const float *picked_max
      = (source == DRAWLAYER_PICK_SOURCE_OUTPUT) ? self->picked_output_color_max : self->picked_color_max;
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
  if(g && g->ui.widgets)
  {
    float display_rgb[3] = { 0.0f };
    if(dt_drawlayer_widgets_finish_picker_drag(g->ui.widgets, display_rgb))
      _remember_display_color(self, display_rgb);
  }
  return FALSE;
}

static gboolean _color_picker_motion(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  (void)widget;
  if(!g || !g->ui.widgets || !event || !dt_drawlayer_widgets_is_picker_dragging(g->ui.widgets)) return FALSE;
  return _color_picker_set_from_position(self, event->x, event->y);
}

static void _sanitize_params(dt_iop_module_t *self, dt_iop_drawlayer_params_t *params)
{
  if(!params) return;

  if(memchr(params->layer_name, '\0', sizeof(params->layer_name)) == NULL)
    memset(params->layer_name, 0, sizeof(params->layer_name));
  else
    params->layer_name[sizeof(params->layer_name) - 1] = '\0';

  if(memchr(params->work_profile, '\0', sizeof(params->work_profile)) == NULL)
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
    if(_get_current_work_profile_key(self, self->dev->iop, self->dev->pipe, current_profile,
                                     sizeof(current_profile)))
      g_strlcpy(params->work_profile, current_profile, sizeof(params->work_profile));
  }
}

static gboolean _widget_to_layer_coords(dt_iop_module_t *self, const double wx, const double wy, float *lx,
                                        float *ly)
{
  if(!self || !self->dev || !self->dev->virtual_pipe || !lx || !ly) return FALSE;

  float pt[2] = { (float)wx, (float)wy };
  if(!_widget_points_to_layer_coords(self, pt, 1)) return FALSE;

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

  while(dt_bauhaus_combobox_length(g->controls.layer_select) > 0)
    dt_bauhaus_combobox_remove_at(g->controls.layer_select, dt_bauhaus_combobox_length(g->controls.layer_select) - 1);

  if(!self->dev)
  {
    dt_bauhaus_combobox_add(g->controls.layer_select, params->layer_name);
    dt_bauhaus_combobox_set(g->controls.layer_select, 0);
    if(darktable.gui) --darktable.gui->reset;
    return;
  }

  char path[PATH_MAX] = { 0 };
  char **names = NULL;
  int count = 0;
  if(!dt_drawlayer_io_sidecar_path(self->dev->image_storage.id, path, sizeof(path))
     || !dt_drawlayer_io_list_layer_names(path, &names, &count))
  {
    dt_bauhaus_combobox_add(g->controls.layer_select, params->layer_name);
    dt_bauhaus_combobox_set(g->controls.layer_select, 0);
    if(darktable.gui) --darktable.gui->reset;
    return;
  }

  int active = -1;
  for(int i = 0; i < count; i++)
  {
    const char *page_name = names[i] ? names[i] : "";
    dt_bauhaus_combobox_add(g->controls.layer_select, page_name);
    if(params->layer_order == i || (params->layer_name[0] && !g_strcmp0(page_name, params->layer_name)))
      active = i;
  }

  dt_drawlayer_io_free_layer_names(&names, &count);
  if(count == 0)
  {
    dt_bauhaus_combobox_add(g->controls.layer_select, params->layer_name);
    dt_bauhaus_combobox_set(g->controls.layer_select, 0);
  }
  else if(active >= 0)
    dt_bauhaus_combobox_set(g->controls.layer_select, active);
  else
    dt_bauhaus_combobox_set(g->controls.layer_select, 0);
  if(darktable.gui) --darktable.gui->reset;
}

/* Realtime worker/queue implementation lives in its own implementation include. */
#include "drawlayer/worker.c"

static void _reset_stroke_session(dt_iop_drawlayer_gui_data_t *g)
{
  if(!g) return;
  g->stroke.stroke_sample_count = 0;
  g->stroke.stroke_event_index = 0;
  g->stroke.last_dab_valid = FALSE;
  g->stroke.live_publish_ts = 0;
  g->stroke.live_publish_serial = 0;
  dt_drawlayer_paint_runtime_state_reset(&g->stroke.live_publish_damage);
  dt_drawlayer_process_state_reset_stroke(&g->process);
}

static void _invalidate_process_patch(dt_iop_drawlayer_gui_data_t *g)
{
  /* Mark the transformed process-tile cache as stale while keeping any
   * allocated storage around for reuse. Geometry changes or out-of-band layer
   * edits use this path; in-stroke backend dabs update the transformed tile
   * incrementally and therefore do not need to invalidate it. */
  if(g) dt_drawlayer_process_state_invalidate(&g->process);
}

static gboolean _layer_cache_matches(const dt_iop_drawlayer_gui_data_t *g, const drawlayer_layer_cache_key_t *key)
{
  /* Cache identity belongs to the module orchestration layer because it
   * combines sidecar identity with GUI/module state (active image, selected
   * layer, in-memory patch ownership). The low-level TIFF primitives stay in
   * drawlayer/io.c. */
  if(!g || !key || !g->process.cache_valid || !g->process.base_patch.pixels) return FALSE;
  if(g->process.cache_imgid != key->imgid || g->process.base_patch.width != key->raw_width
     || g->process.base_patch.height != key->raw_height)
    return FALSE;

  /* Once a layer is cached in memory, `process()` should treat that payload as
   * authoritative until we explicitly reload or flush it. The page index in the
   * params is only a sidecar lookup hint and may legitimately drift after sidecar
   * rewrites or page reordering. Layer names are unique and stable by design, so
   * prefer them for cache identity. Falling back to the page index here causes
   * `process()` to re-open the TIFF and re-scan directories even though the
   * correct pixels are already in memory. */
  const char *target_name = key->layer_name ? key->layer_name : "";
  if(target_name[0] != '\0' || g->process.cache_layer_name[0] != '\0') return !g_strcmp0(g->process.cache_layer_name, target_name);

  if(key->layer_order >= 0 && g->process.cache_layer_order >= 0)
    return g->process.cache_layer_order == key->layer_order;
  return TRUE;
}

gboolean dt_drawlayer_ensure_layer_cache(dt_iop_module_t *self)
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
  const drawlayer_layer_cache_key_t cache_key = {
    .imgid = imgid,
    .raw_width = raw_width,
    .raw_height = raw_height,
    .layer_name = params->layer_name,
    .layer_order = params->layer_order,
  };

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

  if(_layer_cache_matches(g, &cache_key))
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
  /* We are about to replace/rebind `g->process.base_patch`; drop all explicit extra
   * refs from the previous entry first so counters never leak across entries. */
  _release_all_base_patch_extra_refs(g);

  int created = 0;
  if(!dt_drawlayer_cache_patch_alloc_shared(&g->process.base_patch, _drawlayer_params_cache_hash(imgid, params),
                                            (size_t)raw_width * raw_height, raw_width, raw_height,
                                            "drawlayer sidecar cache", &created))
  {
    _release_all_base_patch_extra_refs(g);
    dt_drawlayer_cache_patch_clear(&g->process.base_patch, "drawlayer patch");
    g->process.cache_valid = FALSE;
    _layerio_log_errors(errors);
    g_string_free(errors, TRUE);
    return FALSE;
  }
  if(created)
  {
    dt_drawlayer_cache_patch_wrlock(&g->process.base_patch);
    dt_drawlayer_cache_clear_transparent_float(g->process.base_patch.pixels, (size_t)raw_width * raw_height);
    dt_drawlayer_cache_patch_wrunlock(&g->process.base_patch);
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
    dt_drawlayer_cache_patch_clear(&g->process.base_patch, "drawlayer patch");
    g->process.cache_valid = FALSE;
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
          info.found = io_info.found;
          info.index = io_info.index;
          info.count = io_info.count;
          info.width = io_info.width;
          info.height = io_info.height;
          g_strlcpy(info.name, io_info.name, sizeof(info.name));
          g_strlcpy(info.work_profile, io_info.work_profile, sizeof(info.work_profile));
          dt_drawlayer_io_patch_t io_patch = {
            .x = g->process.base_patch.x,
            .y = g->process.base_patch.y,
            .width = g->process.base_patch.width,
            .height = g->process.base_patch.height,
            .pixels = g->process.base_patch.pixels,
          };
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
        dt_drawlayer_cache_patch_rdlock(&g->process.base_patch);
        dt_drawlayer_io_patch_t io_patch = {
          .x = g->process.base_patch.x,
          .y = g->process.base_patch.y,
          .width = g->process.base_patch.width,
          .height = g->process.base_patch.height,
          .pixels = g->process.base_patch.pixels,
        };
        const gboolean stored = dt_drawlayer_io_store_layer(path, params->layer_name, -1, params->work_profile,
                                                            &io_patch, raw_width, raw_height, FALSE, &final_order);
        dt_drawlayer_cache_patch_rdunlock(&g->process.base_patch);
        if(!stored)
        {
          _layerio_append_error(errors, _("failed to initialize drawing layer sidecar"));
          ok = FALSE;
        }
        else
        {
          params->layer_order = final_order;
          params->sidecar_timestamp = _sidecar_timestamp_from_path(path);
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
          dt_drawlayer_cache_patch_rdlock(&g->process.base_patch);
          dt_drawlayer_io_patch_t io_patch = {
            .x = g->process.base_patch.x,
            .y = g->process.base_patch.y,
            .width = g->process.base_patch.width,
            .height = g->process.base_patch.height,
            .pixels = g->process.base_patch.pixels,
          };
          const gboolean stored
              = dt_drawlayer_io_store_layer(path, params->layer_name, -1, params->work_profile, &io_patch,
                                            raw_width, raw_height, FALSE, &final_order);
          dt_drawlayer_cache_patch_rdunlock(&g->process.base_patch);
          if(!stored)
          {
            _layerio_append_error(errors, _("failed to initialize drawing layer sidecar"));
            ok = FALSE;
          }
          else
          {
            if(g)
            {
              g->session.missing_layer_prompt_name[0] = '\0';
            }
            params->layer_order = final_order;
            params->sidecar_timestamp = _sidecar_timestamp_from_path(path);
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
      info.found = io_info.found;
      info.index = io_info.index;
      info.count = io_info.count;
      info.width = io_info.width;
      info.height = io_info.height;
      g_strlcpy(info.name, io_info.name, sizeof(info.name));
      g_strlcpy(info.work_profile, io_info.work_profile, sizeof(info.work_profile));
      if(g)
      {
        g->session.missing_layer_prompt_name[0] = '\0';
      }
      params->layer_order = info.index;
      if(info.work_profile[0] != '\0' && params->work_profile[0] == '\0')
        g_strlcpy(params->work_profile, info.work_profile, sizeof(params->work_profile));

      if(have_current_profile && info.work_profile[0] != '\0' && g_strcmp0(info.work_profile, current_profile))
        _layerio_append_error(errors, _("drawlayer sidecar profile mismatch"));

      dt_drawlayer_cache_patch_wrlock(&g->process.base_patch);
      dt_drawlayer_io_patch_t io_patch = {
        .x = g->process.base_patch.x,
        .y = g->process.base_patch.y,
        .width = g->process.base_patch.width,
        .height = g->process.base_patch.height,
        .pixels = g->process.base_patch.pixels,
      };
      const gboolean loaded
          = dt_drawlayer_io_load_layer(path, params->layer_name, info.index, raw_width, raw_height, &io_patch);
      dt_drawlayer_cache_patch_wrunlock(&g->process.base_patch);
      if(!loaded)
      {
        _layerio_append_error(errors, _("failed to read drawing layer sidecar"));
        ok = FALSE;
      }
      else
      {
        params->sidecar_timestamp = _sidecar_timestamp_from_path(path);
        _invalidate_process_patch(g);
        cache_loaded = TRUE;
      }
    }
  }

  if(cache_loaded)
  {
    g->process.cache_valid = TRUE;
    g->process.cache_dirty = FALSE;
    g->process.cache_imgid = imgid;
    g_strlcpy(g->process.cache_layer_name, params->layer_name, sizeof(g->process.cache_layer_name));
    g->process.cache_layer_order = params->layer_order;

    if(ui_thread && g->controls.layer_name && g_strcmp0(gtk_entry_get_text(g->controls.layer_name), params->layer_name))
    {
      gtk_entry_set_text(g->controls.layer_name, params->layer_name);
    }
    if(ui_thread && g->controls.layer_select) _populate_layer_list(self);
    if(created) _retain_base_patch_loaded_ref(g);
  }
  else
  {
    _release_all_base_patch_extra_refs(g);
    dt_drawlayer_cache_patch_clear(&g->process.base_patch, "drawlayer patch");
    g->process.cache_valid = FALSE;
    g->process.cache_dirty = FALSE;
  }

  _layerio_log_errors(errors);
  g_string_free(errors, TRUE);
  return ok || !cache_loaded;
}

gboolean dt_drawlayer_flush_layer_cache(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  if(!g || !self->dev || !g->process.cache_valid || !g->process.cache_dirty || !g->process.base_patch.pixels) return TRUE;
  if(!_layer_name_non_empty(g->process.cache_layer_name)) return FALSE;
  if(dt_drawlayer_worker_any_active(g->stroke.worker)) dt_drawlayer_worker_flush_finished_strokes(g->stroke.worker);

  /* If the visible transformed tile has been updated incrementally for the
   * current ROI, fold it back into the authoritative raw-sized cache before the
   * sidecar write. That keeps the final flushed layer consistent with what the
   * user actually saw, even if the transformed tile was carrying the most recent
   * stroke-stateful updates. */
  _flush_process_patch_to_base(self, g);

  char path[PATH_MAX] = { 0 };
  const int32_t flush_imgid = (g->process.cache_imgid > 0) ? g->process.cache_imgid : self->dev->image_storage.id;
  if(flush_imgid <= 0) return TRUE;
  if(!dt_drawlayer_io_sidecar_path(flush_imgid, path, sizeof(path))) return FALSE;

  int final_order = g->process.cache_layer_order;
  const dt_iop_drawlayer_params_t *params = (const dt_iop_drawlayer_params_t *)self->params;
  const char *work_profile = params ? params->work_profile : "";
  dt_drawlayer_cache_patch_rdlock(&g->process.base_patch);
  dt_drawlayer_io_patch_t io_patch = {
    .x = g->process.base_patch.x,
    .y = g->process.base_patch.y,
    .width = g->process.base_patch.width,
    .height = g->process.base_patch.height,
    .pixels = g->process.base_patch.pixels,
  };
  const gboolean ok
      = dt_drawlayer_io_store_layer(path, g->process.cache_layer_name, g->process.cache_layer_order, work_profile, &io_patch,
                                    g->process.base_patch.width, g->process.base_patch.height, FALSE, &final_order);
  dt_drawlayer_cache_patch_rdunlock(&g->process.base_patch);
  if(!ok) return FALSE;

  g->process.cache_layer_order = final_order;
  g->process.cache_dirty = FALSE;

  dt_iop_drawlayer_params_t *mutable_params = (dt_iop_drawlayer_params_t *)self->params;
  if(mutable_params)
  {
    if(!g_strcmp0(mutable_params->layer_name, g->process.cache_layer_name)) mutable_params->layer_order = final_order;
    mutable_params->sidecar_timestamp = _sidecar_timestamp_from_path(path);
    _rekey_shared_base_patch(&g->process.base_patch, flush_imgid, mutable_params);
  }
  _release_all_base_patch_extra_refs(g);
  return TRUE;
}

static gboolean _ensure_widget_cache(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  if(!g || !self->dev) return FALSE;

  drawlayer_view_patch_info_t view = { 0 };
  if(!_compute_view_patch(self, 0.0f, &view)) return FALSE;

  float wx0 = 0.0f, wy0 = 0.0f, wx1 = 0.0f, wy1 = 0.0f;
  if(!_layer_bounds_to_widget_bounds(self, view.layer_x0, view.layer_y0, view.layer_x1, view.layer_y1, &wx0, &wy0,
                                     &wx1, &wy1))
    return FALSE;

  const dt_drawlayer_damaged_rect_t live_view_rect = {
    .valid = TRUE,
    .nw = { (int)floorf(view.layer_x0), (int)floorf(view.layer_y0) },
    .se = { (int)ceilf(view.layer_x1), (int)ceilf(view.layer_y1) },
  };
  const dt_drawlayer_damaged_rect_t preview_rect = {
    .valid = TRUE,
    .nw = { (int)floorf(wx0), (int)floorf(wy0) },
    .se = { (int)ceilf(wx1), (int)ceilf(wy1) },
  };

  const gboolean same_view = (memcmp(&g->session.live_patch, &view.patch, sizeof(view.patch)) == 0
                              && memcmp(&g->session.live_view_rect, &live_view_rect, sizeof(live_view_rect)) == 0
                              && memcmp(&g->session.preview_rect, &preview_rect, sizeof(preview_rect)) == 0);

  if(same_view)
  {
    g->session.last_view_x = self->dev->roi.x;
    g->session.last_view_y = self->dev->roi.y;
    g->session.last_view_scale = self->dev->roi.scaling;
    return TRUE;
  }

  g->session.live_patch = view.patch;
  g->session.live_view_rect = live_view_rect;
  g->session.preview_rect = preview_rect;

  dt_drawlayer_paint_runtime_state_reset(g->process.backend_path);
  g->session.last_view_x = self->dev->roi.x;
  g->session.last_view_y = self->dev->roi.y;
  g->session.last_view_scale = self->dev->roi.scaling;
  return TRUE;
}

float dt_drawlayer_current_live_padding(dt_iop_module_t *self)
{
  dt_drawlayer_brush_dab_t dab = {
    .radius = fmaxf(_conf_size(), 0.5f),
    .hardness = _conf_hardness(),
    .shape = _conf_brush_shape(),
  };
  return ceilf(dab.radius + 1.0f);
}

static gboolean _compute_view_patch(dt_iop_module_t *self, const float padding, drawlayer_view_patch_info_t *view)
{
  if(!self || !self->dev || !view) return FALSE;

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
  if(!_widget_points_to_layer_coords(self, pts, 4)) return FALSE;

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

  view->layer_x0 = min_x;
  view->layer_y0 = min_y;
  view->layer_x1 = max_x;
  view->layer_y1 = max_y;

  view->patch.x = MAX(0, (int)floorf(min_x - padding));
  view->patch.y = MAX(0, (int)floorf(min_y - padding));
  const int right = MIN(raw_width, (int)ceilf(max_x + padding));
  const int bottom = MIN(raw_height, (int)ceilf(max_y + padding));
  view->patch.width = MAX(0, right - view->patch.x);
  view->patch.height = MAX(0, bottom - view->patch.y);
  return view->patch.width > 0 && view->patch.height > 0;
}

void dt_drawlayer_set_pipeline_realtime_mode(dt_iop_module_t *self, const gboolean state)
{
  if(!self || !self->dev || !self->dev->pipe) return;
  const gboolean was_realtime = dt_dev_pixelpipe_get_realtime(self->dev->pipe);
  dt_dev_pixelpipe_set_realtime(self->dev->pipe, state);
  if(was_realtime != state)
    dt_dev_pixelpipe_resync_history_main(self->dev);
  if(!self->dev->preview_pipe) return;

  self->dev->preview_pipe->pause = state;
  if(state)
    dt_atomic_set_int(&self->dev->preview_pipe->shutdown, TRUE);
}

gboolean dt_drawlayer_sync_widget_cache(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  if(!g || !self->dev) return FALSE;

  _pause_worker(self, g->stroke.worker);
  if(!_ensure_widget_cache(self))
  {
    _resume_worker(self, g->stroke.worker);
    return FALSE;
  }

  g->session.live_padding = _current_live_padding(self);
  _resume_worker(self, g->stroke.worker);
  return TRUE;
}

gboolean dt_drawlayer_prime_live_process_patch_before_stroke(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!self || !self->dev || !self->dev->gui_attached || !g) return FALSE;

  gboolean need_build = FALSE;
  dt_drawlayer_cache_patch_rdlock(&g->process.process_patch);
  need_build = !g->process.process_patch_valid || !g->process.process_snapshot_valid
               || !g->process.process_read_patch.pixels;
  dt_drawlayer_cache_patch_rdunlock(&g->process.process_patch);
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


// Find the cached working layer if any
static gboolean _update_runtime_state(const drawlayer_runtime_request_t *request,
                                      dt_drawlayer_runtime_source_t *source)
{
  if(source) *source = (dt_drawlayer_runtime_source_t){ 0 };
  if(!request || !request->self || !request->piece || !request->piece->pipe || !request->roi_out
     || !request->runtime_params || !source)
    return FALSE;

  dt_dev_pixelpipe_iop_t *const piece = request->piece;
  const dt_iop_drawlayer_params_t *const runtime_params = request->runtime_params;
  dt_iop_drawlayer_gui_data_t *const gui = request->gui;
  dt_drawlayer_runtime_manager_t *const manager = request->manager;
  dt_drawlayer_process_state_t *const process = request->process_state;

  // Darkroom pipelines : fetch a previously downsampled layer if any.
  if(process && request->display_pipe)
  {
    const drawlayer_layer_cache_key_t cache_key = {
      .imgid = piece->pipe->image.id,
      .raw_width = process->base_patch.width > 0 ? process->base_patch.width : piece->pipe->iwidth,
      .raw_height = process->base_patch.height > 0 ? process->base_patch.height : piece->pipe->iheight,
      .layer_name = runtime_params->layer_name,
      .layer_order = runtime_params->layer_order,
    };
    if(_layer_cache_matches(gui, &cache_key) && process->process_patch_valid && process->process_snapshot_valid)
    {
      source->process_view.patch = &process->process_read_patch;
      source->kind = DT_DRAWLAYER_SOURCE_GUI_PROCESS;
      source->tracked_buffer = DT_DRAWLAYER_RUNTIME_BUFFER_PROCESS_SNAPSHOT;
      source->tracked_actor = request->use_opencl ? DT_DRAWLAYER_RUNTIME_ACTOR_PIPELINE_CL
                                                  : DT_DRAWLAYER_RUNTIME_ACTOR_PIPELINE_CPU;
      return TRUE;
    }
  }

  // Export and thumbnail pipelines : leech an existing preview pipe cache if any.
  if(process && !request->display_pipe && process->cache_valid && process->base_patch.pixels
     && process->cache_imgid == piece->pipe->image.id)
  {
    const dt_iop_roi_t process_roi = request->roi_in ? *request->roi_in : *request->roi_out;
    source->kind = DT_DRAWLAYER_SOURCE_HEADLESS_BASE;
    source->pixels = process->base_patch.pixels;
    source->cache_entry = process->base_patch.cache_entry;
    source->width = process->base_patch.width;
    source->height = process->base_patch.height;
    source->direct_copy = FALSE;
    source->source_roi = (dt_iop_roi_t){
      .x = 0,
      .y = 0,
      .width = process->base_patch.width,
      .height = process->base_patch.height,
      .scale = 1.0f,
    };
    dt_drawlayer_cache_build_combined_process_roi_for_piece(
        piece, &process_roi, piece->pipe->iwidth, piece->pipe->iheight, process->base_patch.width,
        process->base_patch.height, &source->target_roi);
    dt_drawlayer_cache_patch_rdlock(&process->base_patch);
    if(manager)
    {
      source->tracked_buffer = DT_DRAWLAYER_RUNTIME_BUFFER_BASE_PATCH;
      source->tracked_actor = request->use_opencl ? DT_DRAWLAYER_RUNTIME_ACTOR_PIPELINE_CL
                                                  : DT_DRAWLAYER_RUNTIME_ACTOR_PIPELINE_CPU;
      source->tracked_read_lock = TRUE;
      dt_drawlayer_runtime_manager_note_buffer_lock(manager, source->tracked_buffer, source->tracked_actor, FALSE,
                                                    TRUE);
    }
    return TRUE;
  }

  return FALSE;
}

static drawlayer_preview_background_t _resolve_preview_background(const dt_iop_module_t *self,
                                                                  const dt_iop_drawlayer_gui_data_t *gui)
{
  const int preview_mode = (gui && self && self->dev && self->dev->gui_module == self)
                               ? gui->session.preview_bg_mode
                               : DRAWLAYER_PREVIEW_BG_IMAGE;
  return (drawlayer_preview_background_t){
    .enabled = (preview_mode != DRAWLAYER_PREVIEW_BG_IMAGE),
    .value = (preview_mode == DRAWLAYER_PREVIEW_BG_WHITE)  ? 1.0f
           : (preview_mode == DRAWLAYER_PREVIEW_BG_GREY) ? 0.5f
                                                         : 0.0f,
  };
}

static float _widget_brush_radius(dt_iop_module_t *self, const dt_drawlayer_brush_dab_t *dab, const float fallback)
{
  if(!self || !self->dev || !self->dev->virtual_pipe || !dab) return fallback;

  float pts[6] = {
    dab->x, dab->y, dab->x + dab->radius, dab->y, dab->x, dab->y + dab->radius,
  };

  if(!_layer_points_to_widget_coords(self, pts, 3)) return fallback;

  const float rx = hypotf(pts[2] - pts[0], pts[3] - pts[1]);
  const float ry = hypotf(pts[4] - pts[0], pts[5] - pts[1]);
  const float radius = 0.5f * (rx + ry);
  return fmaxf(0.5f, isfinite(radius) ? radius : fallback);
}

static gboolean _layer_to_widget_coords(dt_iop_module_t *self, const float x, const float y, float *wx, float *wy)
{
  if(!self || !self->dev || !self->dev->virtual_pipe || !wx || !wy) return FALSE;

  float pt[2] = { x, y };
  if(!_layer_points_to_widget_coords(self, pt, 1)) return FALSE;
  *wx = pt[0];
  *wy = pt[1];
  return TRUE;
}

static void _touch_stroke_commit_hash(dt_iop_drawlayer_params_t *params, const int dab_count,
                                      const gboolean have_last_dab, const float last_dab_x,
                                      const float last_dab_y, const uint32_t publish_serial)
{
  if(!params) return;

  uint32_t x_bits = 0u;
  uint32_t y_bits = 0u;
  if(have_last_dab)
  {
    memcpy(&x_bits, &last_dab_x, sizeof(x_bits));
    memcpy(&y_bits, &last_dab_y, sizeof(y_bits));
  }

  const uint32_t seed[5] = { (uint32_t)dab_count, have_last_dab ? 1u : 0u, x_bits, y_bits, publish_serial };

  uint64_t hash = params->stroke_commit_hash ? params->stroke_commit_hash : 5381u;
  hash = dt_hash(hash, (const char *)seed, sizeof(seed));

  /* Keep the serialized field non-zero so "uninitialized" remains distinguishable
   * from "updated at least once" in legacy parameter blobs. */
  params->stroke_commit_hash = (uint32_t)(hash ? hash : 1u);
}

static void _refresh_layer_widgets(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  dt_iop_drawlayer_params_t *params = self ? (dt_iop_drawlayer_params_t *)self->params : NULL;
  GMainContext *const ui_ctx = g_main_context_default();
  if(!g || !params || !(ui_ctx && g_main_context_is_owner(ui_ctx))) return;

  g->manager.background_job_running = g->session.background_job_running;

  if(g->controls.layer_name)
  {
    gtk_entry_set_text(g->controls.layer_name, params->layer_name);
  }

  if(g->controls.layer_select) _populate_layer_list(self);
  if(g->controls.create_background) gtk_widget_set_sensitive(g->controls.create_background, !g->session.background_job_running);
}

static void _sync_preview_bg_buttons(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g) return;

  if(GTK_IS_TOGGLE_BUTTON(g->controls.preview_bg_image))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->controls.preview_bg_image),
                                 g->session.preview_bg_mode == DRAWLAYER_PREVIEW_BG_IMAGE);
  if(GTK_IS_TOGGLE_BUTTON(g->controls.preview_bg_white))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->controls.preview_bg_white),
                                 g->session.preview_bg_mode == DRAWLAYER_PREVIEW_BG_WHITE);
  if(GTK_IS_TOGGLE_BUTTON(g->controls.preview_bg_grey))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->controls.preview_bg_grey),
                                 g->session.preview_bg_mode == DRAWLAYER_PREVIEW_BG_GREY);
  if(GTK_IS_TOGGLE_BUTTON(g->controls.preview_bg_black))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->controls.preview_bg_black),
                                 g->session.preview_bg_mode == DRAWLAYER_PREVIEW_BG_BLACK);
}

gboolean dt_drawlayer_commit_dabs(dt_iop_module_t *self, const gboolean record_history)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  dt_iop_drawlayer_params_t *params = (dt_iop_drawlayer_params_t *)self->params;
  if(!g || !self->dev) return TRUE;
  if(record_history && g->manager.painting_active)
  {
    dt_drawlayer_worker_request_commit(g->stroke.worker);
    return TRUE;
  }

  _cancel_async_commit(g->stroke.worker);

  if(!record_history)
    dt_drawlayer_worker_seal_for_commit(g->stroke.worker);

  /* Commit ordering is strict:
   * - the preview queue and backend queue workers must be idle,
   * - the deferred full-resolution replay queue must be drained too,
   * - the layer cache must already contain the stroke,
   * - only then do we mutate params/history so the pipeline invalidation sees a coherent state. */
  _wait_worker_idle(self, g->stroke.worker);
  dt_drawlayer_worker_flush_finished_strokes(g->stroke.worker);
  gboolean replayed_to_base = FALSE;
  if(!dt_drawlayer_worker_finished_stroke_queued(g->stroke.worker))
  {
    const dt_drawlayer_paint_stroke_t *stroke = dt_drawlayer_worker_stroke(g->stroke.worker);
    const GArray *raw_inputs = dt_drawlayer_worker_raw_inputs(g->stroke.worker);
    if(stroke && raw_inputs) replayed_to_base = _replay_finished_stroke_to_base_patch(self, raw_inputs);
  }
  else
    replayed_to_base = TRUE;
  /* Damage-rectangle ownership stays in drawlayer:
   * paint accumulates per-dab bounds into a stroke rectangle, and on commit the
   * module consumes that rectangle to update process/base dirty regions. */
  _publish_backend_stroke_damage(self);
  if(replayed_to_base)
  {
    _invalidate_process_patch(g);
    _prime_live_process_patch_before_stroke(self);
  }

  int sample_count = 0;
  gboolean had_stroke = FALSE;
  gboolean had_last_dab = FALSE;
  float last_dab_x = 0.0f;
  float last_dab_y = 0.0f;
  dt_iop_gui_enter_critical_section(self);
  g->stroke.finish_commit_pending = FALSE;
  sample_count = (int)g->stroke.stroke_sample_count;
  had_stroke = (sample_count > 0);
  had_last_dab = g->stroke.last_dab_valid;
  last_dab_x = g->stroke.last_dab_x;
  last_dab_y = g->stroke.last_dab_y;
  _reset_stroke_session(g);
  dt_iop_gui_leave_critical_section(self);
  dt_drawlayer_worker_reset_stroke(g->stroke.worker);

  if(had_stroke)
  {
    /* Keep stroke commit lightweight for interactive rendering:
     * - do not flush process tile back to base patch here,
     * - do not invalidate the process tile here.
     *
     * Base-patch synchronization is handled at explicit persistence points
     * (save/focus-out/mouse-leave/geometry rebuild) so recomputes can keep
     * blending the already-updated process tile directly. */
    _touch_stroke_commit_hash(params, sample_count, had_last_dab, last_dab_x, last_dab_y, 0u);
    _retain_base_patch_stroke_ref(g);
    if(record_history && self->dev)
    {
      dt_dev_undo_start_record(self->dev);
      dt_pthread_rwlock_wrlock(&self->dev->history_mutex);
      dt_dev_add_history_item_ext(self->dev, self, TRUE, FALSE);
      dt_dev_set_history_hash(self->dev, dt_dev_history_compute_hash(self->dev));
      dt_pthread_rwlock_unlock(&self->dev->history_mutex);
      dt_dev_undo_end_record(self->dev);
      if(self->post_history_commit) self->post_history_commit(self);

      g->manager.realtime_active = FALSE;
      _set_drawlayer_pipeline_realtime_mode(self, FALSE);
      dt_dev_pixelpipe_update_history_all(self->dev);
      dt_dev_write_history(self->dev);
      dt_dev_history_notify_change(self->dev, self->dev->image_storage.id);
    }

    /* The final stroke-end raster batches may land after the button-release
     * event already requested one redraw. Queue one more redraw now that the
     * worker is idle and the last published process snapshot is coherent, so
     * late-finishing strokes become visible even without further UI activity. */
    if(self->dev && self->dev->gui_attached) dt_control_queue_redraw_center();
  }

  if(!(had_stroke && record_history))
  {
    g->manager.realtime_active = FALSE;
    _set_drawlayer_pipeline_realtime_mode(self, FALSE);
  }

  return TRUE;
}

static void _develop_ui_pipe_finished_callback(gpointer instance, gpointer user_data)
{
  (void)instance;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !self->dev) return;
  drawlayer_runtime_host_context_t runtime_host = {
    .runtime = {
      .self = self,
      .runtime_params = (const dt_iop_drawlayer_params_t *)self->params,
      .gui = g,
      .manager = &g->manager,
      .process_state = &g->process,
    },
  };
  const dt_drawlayer_runtime_host_t runtime_manager = {
    .user_data = &runtime_host,
    .collect_inputs = _drawlayer_runtime_collect_inputs,
    .perform_action = _drawlayer_runtime_perform_action,
  };
  dt_drawlayer_runtime_manager_update(
      &g->manager,
      &(dt_drawlayer_runtime_update_request_t){
        .event = DT_DRAWLAYER_RUNTIME_EVENT_GUI_PIPE_FINISHED,
        .raw_input_kind = DT_DRAWLAYER_RUNTIME_RAW_INPUT_NONE,
      },
      &runtime_manager);
}

static void _sync_mode_sensitive_widgets(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !g->controls.color || !g->controls.softness) return;

  const gboolean paint_mode = (_conf_brush_mode() == DT_DRAWLAYER_BRUSH_MODE_PAINT);
  const gboolean show_hardness = (_conf_brush_shape() != DT_DRAWLAYER_BRUSH_SHAPE_GAUSSIAN);
  gtk_widget_set_visible(GTK_WIDGET(g->controls.color), paint_mode);
  if(g->controls.color_row) gtk_widget_set_visible(g->controls.color_row, paint_mode);
  if(g->controls.color_swatch) gtk_widget_set_visible(g->controls.color_swatch, paint_mode);
  if(g->controls.image_colorpicker) gtk_widget_set_visible(g->controls.image_colorpicker, paint_mode);
  if(g->controls.image_colorpicker_source) gtk_widget_set_visible(g->controls.image_colorpicker_source, paint_mode);
  gtk_widget_set_visible(g->controls.softness, show_hardness);
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
      info.found = io_info.found;
      info.index = io_info.index;
      info.count = io_info.count;
      info.width = io_info.width;
      info.height = io_info.height;
      g_strlcpy(info.name, io_info.name, sizeof(info.name));
      g_strlcpy(info.work_profile, io_info.work_profile, sizeof(info.work_profile));
      if(!dt_drawlayer_io_store_layer(path, params->layer_name, info.index, NULL, NULL, self->dev->roi.raw_width,
                                      self->dev->roi.raw_height, TRUE, NULL))
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
          dt_drawlayer_cache_patch_clear(&g->process.base_patch, "drawlayer patch");
          g->process.cache_valid = FALSE;
          g->process.cache_dirty = FALSE;
          g->process.cache_layer_name[0] = '\0';
          g->process.cache_layer_order = -1;
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
      GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
      GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s",
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
    if(g_strcmp0(gtk_entry_get_text(g->controls.layer_name), new_name))
    {
      gtk_entry_set_text(g->controls.layer_name, new_name);
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
        info.found = io_info.found;
        info.index = io_info.index;
        info.count = io_info.count;
        info.width = io_info.width;
        info.height = io_info.height;
        g_strlcpy(info.name, io_info.name, sizeof(info.name));
        g_strlcpy(info.work_profile, io_info.work_profile, sizeof(info.work_profile));
        if(dt_drawlayer_io_layer_name_exists(path, new_name, info.index))
        {
          _layerio_append_error(errors, _("drawlayer layer name already exists"));
        }
        else
        {
          int final_order = info.index;
          if(!dt_drawlayer_io_store_layer(path, new_name, info.index, params->work_profile, NULL,
                                          self->dev->roi.raw_width, self->dev->roi.raw_height, FALSE, &final_order))
          {
            _layerio_append_error(errors, _("failed to rename drawing layer in sidecar"));
          }
          else
          {
            g_strlcpy(params->layer_name, new_name, sizeof(params->layer_name));
            params->layer_order = final_order;
            params->sidecar_timestamp = _sidecar_timestamp_from_path(path);
            g_strlcpy(g->process.cache_layer_name, params->layer_name, sizeof(g->process.cache_layer_name));
            g->process.cache_layer_order = params->layer_order;
            renamed = TRUE;
          }
        }
      }
    }
  }

  if(renamed)
  {
    g->session.missing_layer_prompt_name[0] = '\0';
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

  if(!_update_gui_runtime_manager(self, g, DT_DRAWLAYER_RUNTIME_EVENT_GUI_SYNC_TEMP_BUFFERS, FALSE).ok)
  {
    *params = previous;
    gui_update(self);
    return FALSE;
  }

  g->session.missing_layer_prompt_name[0] = '\0';
  _touch_stroke_commit_hash(params, 0, FALSE, 0.0f, 0.0f, 0u);
  if(self->dev)
  {
    dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
    dt_dev_pixelpipe_update_history_all(self->dev);
  }
  _refresh_layer_widgets(self);
  gui_update(self);
  return TRUE;
}

static void _build_pre_module_filter_string(dt_iop_module_t *self, char *filter, const size_t filter_size)
{
  if(!filter || filter_size == 0) return;
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

static gboolean _background_layer_job_done_idle(gpointer user_data)
{
  dt_drawlayer_io_background_job_result_t *result = (dt_drawlayer_io_background_job_result_t *)user_data;
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
      if(g && params && !g_strcmp0(params->layer_name, result->initiator_layer_name)
         && params->layer_order == result->initiator_layer_order)
      {
        g->session.background_job_running = FALSE;
        if(g->controls.create_background) gtk_widget_set_sensitive(g->controls.create_background, TRUE);
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
      if(!g || !g->session.background_job_running) continue;
      g->session.background_job_running = FALSE;
      if(g->controls.create_background) gtk_widget_set_sensitive(g->controls.create_background, TRUE);
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
  if(g->session.background_job_running) return FALSE;
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
  if(!dt_drawlayer_io_find_layer(sidecar_path, params->layer_name, params->layer_order, &io_info)) return FALSE;
  current_info.found = io_info.found;
  current_info.index = io_info.index;
  current_info.count = io_info.count;
  current_info.width = io_info.width;
  current_info.height = io_info.height;
  g_strlcpy(current_info.name, io_info.name, sizeof(current_info.name));
  g_strlcpy(current_info.work_profile, io_info.work_profile, sizeof(current_info.work_profile));

  int dst_x = 0;
  int dst_y = 0;
  dt_dev_pixelpipe_iop_t *piece = dt_dev_distort_get_iop_pipe(self->dev, self->dev->pipe, self);
  if(piece)
  {
    dst_x = piece->buf_in.x;
    dst_y = piece->buf_in.y;
  }

  dt_drawlayer_io_background_job_params_t *job_params = g_new0(dt_drawlayer_io_background_job_params_t, 1);
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
  job_params->done_idle = _background_layer_job_done_idle;
  _build_pre_module_filter_string(self, job_params->filter, sizeof(job_params->filter));

  dt_job_t *job = dt_control_job_create(dt_drawlayer_io_background_layer_job_run,
                                        "drawlayer create background layer");
  if(!job)
  {
    dt_free(job_params);
    return FALSE;
  }

  dt_control_job_set_params(job, job_params, g_free);
  dt_control_job_add_progress(job, _("creating background layer"), TRUE);
  g->session.background_job_running = TRUE;
  if(g->controls.create_background) gtk_widget_set_sensitive(g->controls.create_background, FALSE);
  if(dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_BG, job) != 0)
  {
    g->session.background_job_running = FALSE;
    if(g->controls.create_background) gtk_widget_set_sensitive(g->controls.create_background, TRUE);
    return FALSE;
  }
  return TRUE;
}

static int _offer_missing_layer_recreation(dt_iop_module_t *self, const char *missing_name)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!self || !self->dev || !g) return FALSE;
  if(self->dev->gui_module != self) return 0;

  if(g->session.missing_layer_prompt_name[0] != '\0'
     && !g_strcmp0(g->session.missing_layer_prompt_name, missing_name ? missing_name : ""))
    return 1;

  g_strlcpy(g->session.missing_layer_prompt_name, missing_name ? missing_name : "", sizeof(g->session.missing_layer_prompt_name));

  GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)),
                                             GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
                                             GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, "%s",
                                             _("The linked drawing layer was not found in the sidecar TIFF."));
  gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s",
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

  if(widget == GTK_WIDGET(g->controls.layer_name))
  {
    _rename_current_layer_from_gui(self, gtk_entry_get_text(g->controls.layer_name));
    return;
  }

  _sync_params_from_gui(self, FALSE);

  if(widget == g->controls.brush_mode || widget == g->controls.brush_shape) _sync_mode_sensitive_widgets(self);

  if(widget == g->controls.size || widget == g->controls.softness || widget == g->controls.brush_shape)
    _update_gui_runtime_manager(self, g, DT_DRAWLAYER_RUNTIME_EVENT_GUI_SYNC_TEMP_BUFFERS, TRUE);

  if(widget == g->controls.brush_shape || widget == g->controls.opacity || widget == g->controls.softness || widget == g->controls.sprinkles
     || widget == g->controls.sprinkle_size || widget == g->controls.sprinkle_coarseness)
    _sync_brush_profile_preview_widget(self);
}

static void _layer_selected(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  dt_iop_drawlayer_params_t *params = (dt_iop_drawlayer_params_t *)self->params;
  if(!g || (darktable.gui && darktable.gui->reset)) return;

  const int active = dt_bauhaus_combobox_get(widget);
  if(active < 0) return;

  const char *text = dt_bauhaus_combobox_get_text(g->controls.layer_select);
  if(!text) return;

  gtk_entry_set_text(g->controls.layer_name, text);

  char previous_name[DRAWLAYER_NAME_SIZE] = { 0 };
  g_strlcpy(previous_name, params->layer_name, sizeof(previous_name));
  const int previous_order = params->layer_order;
  g_strlcpy(params->layer_name, text, sizeof(params->layer_name));
  params->layer_order = active;
  if(!_update_gui_runtime_manager(self, g, DT_DRAWLAYER_RUNTIME_EVENT_GUI_SYNC_TEMP_BUFFERS, TRUE).ok)
  {
    g_strlcpy(params->layer_name, previous_name, sizeof(params->layer_name));
    params->layer_order = previous_order;
    gui_update(self);
    return;
  }

  g->session.missing_layer_prompt_name[0] = '\0';
  _touch_stroke_commit_hash(params, 0, FALSE, 0.0f, 0.0f, 0u);
  if(self->dev)
  {
    dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
    dt_dev_pixelpipe_update_history_all(self->dev);
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
  _touch_stroke_commit_hash(params, 0, FALSE, 0.0f, 0.0f, 0u);
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
  if(!_update_gui_runtime_manager(self, g, DT_DRAWLAYER_RUNTIME_EVENT_GUI_SYNC_TEMP_BUFFERS, FALSE).ok) return FALSE;
  if(!g->process.base_patch.pixels) return FALSE;

  const size_t count = (size_t)g->process.base_patch.width * g->process.base_patch.height;
  dt_drawlayer_cache_patch_wrlock(&g->process.base_patch);
  for(size_t k = 0; k < count; k++)
  {
    float *pixel = g->process.base_patch.pixels + 4 * k;
    pixel[0] = _clamp01(value);
    pixel[1] = _clamp01(value);
    pixel[2] = _clamp01(value);
    pixel[3] = 1.0f;
  }
  dt_drawlayer_cache_patch_wrunlock(&g->process.base_patch);

  g->process.cache_dirty = TRUE;
  _invalidate_process_patch(g);
  _touch_stroke_commit_hash(params, 0, FALSE, 0.0f, 0.0f, 0u);
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
  if(!_update_gui_runtime_manager(self, g, DT_DRAWLAYER_RUNTIME_EVENT_GUI_SYNC_TEMP_BUFFERS, FALSE).ok) return FALSE;
  if(!g->process.base_patch.pixels) return FALSE;

  dt_drawlayer_cache_patch_wrlock(&g->process.base_patch);
  dt_drawlayer_cache_clear_transparent_float(g->process.base_patch.pixels,
                                             (size_t)g->process.base_patch.width * g->process.base_patch.height);
  dt_drawlayer_cache_patch_wrunlock(&g->process.base_patch);

  g->process.cache_dirty = TRUE;
  _invalidate_process_patch(g);
  _touch_stroke_commit_hash(params, 0, FALSE, 0.0f, 0.0f, 0u);
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

  if(!g->process.cache_valid || !g->process.base_patch.pixels)
  {
    _show_drawlayer_modal_message(GTK_MESSAGE_ERROR,
                                  _("No drawing layer is loaded in memory."),
                                  _("The sidecar save was aborted."));
    return;
  }
  if(!_layer_name_non_empty(g->process.cache_layer_name))
  {
    _show_drawlayer_modal_message(GTK_MESSAGE_ERROR,
                                  _("Layer name is empty."),
                                  _("The sidecar save was aborted."));
    _refresh_layer_widgets(self);
    return;
  }

  GtkWidget *dialog = gtk_message_dialog_new(
      GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
      GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s", _("Save the drawing sidecar now?"));
  gtk_message_dialog_format_secondary_text(
      GTK_MESSAGE_DIALOG(dialog), "%s",
      _("This writes the current in-memory drawing layer to the sidecar TIFF immediately."));
  gtk_dialog_add_button(GTK_DIALOG(dialog), _("Cancel"), GTK_RESPONSE_CANCEL);
  gtk_dialog_add_button(GTK_DIALOG(dialog), _("Save"), GTK_RESPONSE_ACCEPT);

  const int response = gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
  if(response != GTK_RESPONSE_ACCEPT)
  {
    return;
  }

  _drawlayer_wait_for_rasterization_modal(g, _("Saving layer"),
                                          _("Waiting for the layer rasterization to finish..."));

  /* Saving the sidecar is explicit persistence, not a history edit.
   * We still finalize any pending stroke first so the flush sees the latest
   * authoritative cache state, then write that cache to the TIFF immediately. */
  if(!_commit_dabs(self, FALSE))
  {
    _show_drawlayer_modal_message(GTK_MESSAGE_ERROR,
                                  _("Failed to finalize the drawing stroke."),
                                  _("The sidecar was not saved."));
    return;
  }

  _flush_process_patch_to_base(self, g);
  _rekey_shared_base_patch(&g->process.base_patch, self->dev->image_storage.id,
                           (const dt_iop_drawlayer_params_t *)self->params);

  if(!_flush_layer_cache(self))
  {
    _show_drawlayer_modal_message(GTK_MESSAGE_ERROR,
                                  _("Failed to write the drawing layer sidecar."),
                                  _("The sidecar TIFF could not be updated."));
    return;
  }

  _release_all_base_patch_extra_refs(g);
  _refresh_layer_widgets(self);

  _show_drawlayer_modal_message(GTK_MESSAGE_INFO,
                                _("Drawing sidecar saved."),
                                _("The current in-memory layer has been written to the sidecar TIFF."));
}

static void _create_layer_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!self || !g) return;
  if(!_create_new_layer(self, gtk_entry_get_text(g->controls.layer_name)))
    dt_control_log(_("failed to create drawing layer"));
}

static void _create_background_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(!_create_background_layer_from_input(self)) dt_control_log(_("failed to create background layer from input"));
}

static void _preview_bg_toggled(GtkToggleButton *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  dt_iop_drawlayer_params_t *params = self ? (dt_iop_drawlayer_params_t *)self->params : NULL;
  if(!self || !self->dev || !g || (darktable.gui && darktable.gui->reset) || !gtk_toggle_button_get_active(button))
    return;

  if(GTK_WIDGET(button) == g->controls.preview_bg_white)
    g->session.preview_bg_mode = DRAWLAYER_PREVIEW_BG_WHITE;
  else if(GTK_WIDGET(button) == g->controls.preview_bg_grey)
    g->session.preview_bg_mode = DRAWLAYER_PREVIEW_BG_GREY;
  else if(GTK_WIDGET(button) == g->controls.preview_bg_black)
    g->session.preview_bg_mode = DRAWLAYER_PREVIEW_BG_BLACK;
  else
    g->session.preview_bg_mode = DRAWLAYER_PREVIEW_BG_IMAGE;

  _sync_preview_bg_buttons(self);
  if(params) _touch_stroke_commit_hash(params, 0, FALSE, 0.0f, 0.0f, 0u);
  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
  dt_dev_pixelpipe_update_history_all(self->dev);
}

static gboolean _build_raw_input_event(dt_iop_module_t *self, const double wx, const double wy, const double pressure,
                                       const dt_drawlayer_paint_stroke_pos_t stroke_pos,
                                       dt_drawlayer_paint_raw_input_t *input)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g || !input) return FALSE;

  dt_control_pointer_input_t pointer_input = { 0 };
  dt_control_get_pointer_input(&pointer_input);
  const float input_wx = isfinite(pointer_input.x) ? (float)pointer_input.x : (float)wx;
  const float input_wy = isfinite(pointer_input.y) ? (float)pointer_input.y : (float)wy;
  const float pressure_norm = pointer_input.has_pressure ? _clamp01(pointer_input.pressure) : _clamp01(pressure);
  *input = (dt_drawlayer_paint_raw_input_t){
    .wx = input_wx,
    .wy = input_wy,
    .pressure = pressure_norm,
    .tilt = (float)_clamp01(pointer_input.tilt),
    .acceleration = (float)_clamp01(pointer_input.acceleration),
    .event_ts = g_get_monotonic_time(),
    .stroke_batch = g->stroke.current_stroke_batch,
    .event_index = ++g->stroke.stroke_event_index,
    .stroke_pos = stroke_pos,
  };
  _fill_input_layer_coords(self, input);
  _fill_input_brush_settings(self, input);
  return TRUE;
}

void dt_drawlayer_begin_gui_stroke_capture(dt_iop_module_t *self, const dt_drawlayer_paint_raw_input_t *first_input)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!self || !g) return;

  uint32_t stroke_batch = g->stroke.current_stroke_batch + 1u;
  if(stroke_batch == 0u) stroke_batch++;
  const uint32_t event_index = first_input ? first_input->event_index : 0u;
  if(first_input && first_input->stroke_batch != 0u) stroke_batch = first_input->stroke_batch;

  dt_iop_gui_enter_critical_section(self);
  g->session.pointer_valid = TRUE;
  g->stroke.current_stroke_batch = stroke_batch;
  if(!dt_drawlayer_worker_active(g->stroke.worker))
    dt_drawlayer_paint_runtime_state_reset(g->process.backend_path);
  g->stroke.finish_commit_pending = FALSE;
  g->stroke.stroke_sample_count = 0;
  g->stroke.stroke_event_index = event_index;
  g->stroke.last_dab_valid = FALSE;
  g->stroke.live_publish_ts = 0;
  g->stroke.live_publish_serial = 0;
  dt_drawlayer_paint_runtime_state_reset(&g->stroke.live_publish_damage);
  dt_iop_gui_leave_critical_section(self);
}

void dt_drawlayer_end_gui_stroke_capture(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g) return;
}

static dt_drawlayer_runtime_result_t _update_gui_runtime_manager(dt_iop_module_t *self,
                                                                 dt_iop_drawlayer_gui_data_t *g,
                                                                 const dt_drawlayer_runtime_event_t event,
                                                                 const gboolean flush_pending)
{
  dt_drawlayer_runtime_result_t result = {
    .ok = TRUE,
    .raw_input_ok = TRUE,
  };
  if(!self || !g) return result;

  const drawlayer_runtime_host_context_t runtime_host = {
    .runtime = {
      .self = self,
      .runtime_params = (const dt_iop_drawlayer_params_t *)self->params,
      .gui = g,
      .manager = &g->manager,
      .process_state = &g->process,
    },
  };
  const dt_drawlayer_runtime_host_t runtime_manager = {
    .user_data = (void *)&runtime_host,
    .collect_inputs = _drawlayer_runtime_collect_inputs,
    .perform_action = _drawlayer_runtime_perform_action,
  };
  return dt_drawlayer_runtime_manager_update(
      &g->manager,
      &(dt_drawlayer_runtime_update_request_t){
        .event = event,
        .raw_input_kind = DT_DRAWLAYER_RUNTIME_RAW_INPUT_NONE,
        .flush_pending = flush_pending,
      },
      &runtime_manager);
}

void dt_drawlayer_show_runtime_feedback(const dt_iop_drawlayer_gui_data_t *g,
                                        const dt_drawlayer_runtime_feedback_t feedback)
{
  if(!g) return;

  switch(feedback)
  {
    case DT_DRAWLAYER_RUNTIME_FEEDBACK_FOCUS_LOSS_WAIT:
      _drawlayer_wait_for_rasterization_modal(g, _("Finishing drawing"),
                                              _("Waiting for the drawing rasterization to finish..."));
      break;

    case DT_DRAWLAYER_RUNTIME_FEEDBACK_SAVE_WAIT:
      _drawlayer_wait_for_rasterization_modal(g, _("Saving layer"),
                                              _("Waiting for the layer rasterization to finish..."));
      break;

    case DT_DRAWLAYER_RUNTIME_FEEDBACK_NONE:
    default:
      break;
  }
}

/** @brief Module display name. */
const char *name()
{
  return C_("modulename", "drawing");
}

/** @brief Module description strings used by UI/help. */
const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("paint premultiplied RGB layers in a TIFF sidecar"), _("creative"),
                                _("linear, RGB, scene-referred"), _("geometric, RGB"),
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

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_drawlayer_data_t));
  piece->data_size = sizeof(dt_iop_drawlayer_params_t);
  if(piece->data)
  {
    dt_iop_drawlayer_data_t *data = (dt_iop_drawlayer_data_t *)piece->data;
    data->params.layer_order = -1;
    dt_drawlayer_process_state_init(&data->process);
    dt_drawlayer_runtime_manager_init(&data->headless_manager);
    dt_drawlayer_runtime_manager_bind_piece(&data->headless_manager, &data->process, NULL, NULL, FALSE,
                                            &data->runtime_manager, &data->runtime_process,
                                            &data->runtime_display_pipe);
  }
}

/** @brief Cleanup per-pipe runtime data. */
void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  (void)self;
  (void)pipe;
  if(!piece || !piece->data) return;
  dt_iop_drawlayer_data_t *data = (dt_iop_drawlayer_data_t *)piece->data;
  dt_drawlayer_process_state_cleanup(&data->process);
  dt_drawlayer_runtime_manager_cleanup(&data->headless_manager);
  dt_free(piece->data);
  piece->data_size = 0;
}

/** @brief Commit params to runtime piece and refresh base cache state. */
void commit_params(dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_drawlayer_data_t *data = (dt_iop_drawlayer_data_t *)piece->data;
  const gboolean display_pipe = self && self->dev && self->gui_data && pipe
                                && (pipe == self->dev->pipe || pipe == self->dev->preview_pipe);
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  dt_drawlayer_runtime_manager_bind_piece(&data->headless_manager, &data->process, g ? &g->manager : NULL,
                                          g ? &g->process : NULL, display_pipe, &data->runtime_manager,
                                          &data->runtime_process, &data->runtime_display_pipe);
  memcpy(&data->params, params, sizeof(dt_iop_drawlayer_params_t));
  _sanitize_params(self, &data->params);

  /* Every pipe now warms the same authoritative base-patch snapshot through
   * the pixelpipe cache during `commit_params()`. GUI pipes still keep their
   * own transformed ROI cache on top, but they attach to the same shared base
   * line as headless pipes instead of carrying a private sidecar mirror. */
  _refresh_piece_base_cache(self, data, &data->params, piece);
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
  _touch_stroke_commit_hash(params, 0, FALSE, 0.0f, 0.0f, 0u);
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  if(g)
  {
    drawlayer_runtime_host_context_t runtime_host = {
      .runtime = {
        .self = self,
        .runtime_params = (const dt_iop_drawlayer_params_t *)self->params,
        .gui = g,
        .manager = &g->manager,
        .process_state = &g->process,
      },
    };
    const dt_drawlayer_runtime_host_t runtime_manager = {
      .user_data = &runtime_host,
      .collect_inputs = _drawlayer_runtime_collect_inputs,
      .perform_action = _drawlayer_runtime_perform_action,
    };
    dt_drawlayer_runtime_manager_update(
        &g->manager,
        &(dt_drawlayer_runtime_update_request_t){
          .event = DT_DRAWLAYER_RUNTIME_EVENT_GUI_RESYNC,
          .raw_input_kind = DT_DRAWLAYER_RUNTIME_RAW_INPUT_NONE,
        },
        &runtime_manager);
  }
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
  g->ui.widgets = dt_drawlayer_widgets_init();
  dt_drawlayer_process_state_init(&g->process);
  _load_color_history(g);
  _ensure_layer_name(self, params);
  _sanitize_params(self, params);

  dt_drawlayer_worker_init(self, &g->stroke.worker, &g->manager.painting_active,
                           &g->stroke.finish_commit_pending, &g->stroke.stroke_sample_count,
                           &g->stroke.current_stroke_batch, _replay_finished_stroke_to_base_patch);
  g->session.background_job_running = FALSE;
  g->session.last_view_x = 0.0f;
  g->session.last_view_y = 0.0f;
  g->session.last_view_scale = 1.0f;
  if(self->dev)
  {
    g->session.last_view_x = self->dev->roi.x;
    g->session.last_view_y = self->dev->roi.y;
    g->session.last_view_scale = self->dev->roi.scaling;
  }

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_IOP_MODULE_CONTROL_SPACING);
  if(self->reset_button) gtk_widget_hide(self->reset_button);

  GtkWidget *history_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_IOP_MODULE_CONTROL_SPACING);
  g->controls.save_layer = gtk_button_new_with_label(_("save sidecar"));
  gtk_box_pack_start(GTK_BOX(history_box), g->controls.save_layer, TRUE, TRUE, 0);
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
  g->controls.preview_bg_image = gtk_radio_button_new_with_label(preview_group, _("image"));
  preview_group = gtk_radio_button_get_group(GTK_RADIO_BUTTON(g->controls.preview_bg_image));
  g->controls.preview_bg_white = gtk_radio_button_new_with_label(preview_group, _("white"));
  preview_group = gtk_radio_button_get_group(GTK_RADIO_BUTTON(g->controls.preview_bg_white));
  g->controls.preview_bg_grey = gtk_radio_button_new_with_label(preview_group, _("grey"));
  preview_group = gtk_radio_button_get_group(GTK_RADIO_BUTTON(g->controls.preview_bg_grey));
  g->controls.preview_bg_black = gtk_radio_button_new_with_label(preview_group, _("black"));
  gtk_box_pack_start(GTK_BOX(preview_box), g->controls.preview_bg_image, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(preview_box), g->controls.preview_bg_white, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(preview_box), g->controls.preview_bg_grey, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(preview_box), g->controls.preview_bg_black, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(layer_tab), preview_title, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(layer_tab), preview_box, FALSE, FALSE, 0);

  g->controls.brush_mode = dt_bauhaus_combobox_new(darktable.bauhaus, DT_GUI_MODULE(self));
  dt_bauhaus_combobox_add(g->controls.brush_mode, _("paint"));
  dt_bauhaus_combobox_add(g->controls.brush_mode, _("erase"));
  dt_bauhaus_combobox_add(g->controls.brush_mode, _("blur"));
  dt_bauhaus_combobox_add(g->controls.brush_mode, _("smudge"));
  dt_bauhaus_widget_set_label(g->controls.brush_mode, _("Paint mode"));
  gtk_box_pack_start(GTK_BOX(brush_tab), g->controls.brush_mode, TRUE, TRUE, 0);

  GtkWidget *color_title = gtk_label_new(_("Color"));
  gtk_label_set_xalign(GTK_LABEL(color_title), 0.0f);
  dt_gui_add_class(color_title, "dt_section_label");
  gtk_box_pack_start(GTK_BOX(brush_tab), color_title, TRUE, TRUE, 0);
  g->controls.color = gtk_drawing_area_new();
  gtk_widget_set_size_request(g->controls.color, -1, DT_DRAWLAYER_COLOR_PICKER_HEIGHT);
  gtk_widget_add_events(g->controls.color, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
  gtk_box_pack_start(GTK_BOX(brush_tab), g->controls.color, TRUE, TRUE, 0);
  g->controls.color_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(16));
  g->controls.color_swatch = gtk_drawing_area_new();
  gtk_widget_set_size_request(g->controls.color_swatch, -1, DT_PIXEL_APPLY_DPI(DT_DRAWLAYER_COLOR_HISTORY_HEIGHT));
  gtk_widget_add_events(g->controls.color_swatch, GDK_BUTTON_PRESS_MASK);
  gtk_box_pack_start(GTK_BOX(g->controls.color_row), g->controls.color_swatch, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(brush_tab), g->controls.color_row, TRUE, TRUE, 0);
  GtkWidget *picker_controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(6));
  g->controls.image_colorpicker = dt_color_picker_new_with_cst(self, DT_COLOR_PICKER_POINT_AREA, NULL, IOP_CS_NONE);
  g->controls.image_colorpicker_source = dt_bauhaus_combobox_new(darktable.bauhaus, DT_GUI_MODULE(self));
  dt_bauhaus_combobox_add(g->controls.image_colorpicker_source, _("input"));
  dt_bauhaus_combobox_add(g->controls.image_colorpicker_source, _("output"));
  dt_bauhaus_widget_set_label(g->controls.image_colorpicker_source, _("Pick from"));
  gtk_box_pack_start(GTK_BOX(picker_controls), g->controls.image_colorpicker, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(picker_controls), g->controls.image_colorpicker_source, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(brush_tab), picker_controls, TRUE, TRUE, 0);

  g->controls.hdr_exposure
      = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 0.0f, 4.0f, 0.1f, 0.0f, 2);
  dt_bauhaus_widget_set_label(g->controls.hdr_exposure, _("HDR exposure"));
  dt_bauhaus_slider_set_format(g->controls.hdr_exposure, _(" EV"));
  gtk_box_pack_start(GTK_BOX(brush_tab), g->controls.hdr_exposure, TRUE, TRUE, 0);

  GtkWidget *geometry_title = gtk_label_new(_("Geometry"));
  gtk_label_set_xalign(GTK_LABEL(geometry_title), 0.0f);
  dt_gui_add_class(geometry_title, "dt_section_label");
  gtk_box_pack_start(GTK_BOX(brush_tab), geometry_title, TRUE, TRUE, 0);
  GtkWidget *brush_shape_title = gtk_label_new(_("Fall-off"));
  gtk_label_set_xalign(GTK_LABEL(brush_shape_title), 0.0f);
  gtk_box_pack_start(GTK_BOX(brush_tab), brush_shape_title, TRUE, TRUE, 0);
  g->controls.brush_shape = gtk_drawing_area_new();
  gtk_widget_set_size_request(g->controls.brush_shape, -1, DT_PIXEL_APPLY_DPI(72));
  gtk_widget_add_events(g->controls.brush_shape, GDK_BUTTON_PRESS_MASK);
  gtk_box_pack_start(GTK_BOX(brush_tab), g->controls.brush_shape, TRUE, TRUE, 0);

  g->controls.size
      = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 1.0f, 2048.0f, 1.0f, 64.0f, 0);
  dt_bauhaus_widget_set_label(g->controls.size, _("Size"));
  dt_bauhaus_slider_set_format(g->controls.size, _(" px"));
  gtk_box_pack_start(GTK_BOX(brush_tab), g->controls.size, TRUE, TRUE, 0);
  g->controls.distance
      = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 0.0f, 100.0f, 1.0f, 0.0f, 2);
  dt_bauhaus_widget_set_label(g->controls.distance, _("Sampling distance"));
  dt_bauhaus_slider_set_format(g->controls.distance, "%");
  gtk_box_pack_start(GTK_BOX(brush_tab), g->controls.distance, TRUE, TRUE, 0);
  g->controls.smoothing
      = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 0.0f, 100.0f, 1.0f, 0.0f, 2);
  dt_bauhaus_widget_set_label(g->controls.smoothing, _("Smoothing"));
  dt_bauhaus_slider_set_format(g->controls.smoothing, "%");
  gtk_box_pack_start(GTK_BOX(brush_tab), g->controls.smoothing, TRUE, TRUE, 0);
  g->controls.softness
      = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 0.0f, 1.0f, 0.01f, 0.5f, 2);
  dt_bauhaus_widget_set_label(g->controls.softness, _("Hardness"));
  dt_bauhaus_slider_set_factor(g->controls.softness, 100.0f);
  dt_bauhaus_slider_set_format(g->controls.softness, "%");
  gtk_box_pack_start(GTK_BOX(brush_tab), g->controls.softness, TRUE, TRUE, 0);

  GtkWidget *thickness_title = gtk_label_new(_("Thickness"));
  gtk_label_set_xalign(GTK_LABEL(thickness_title), 0.0f);
  dt_gui_add_class(thickness_title, "dt_section_label");
  gtk_box_pack_start(GTK_BOX(brush_tab), thickness_title, TRUE, TRUE, 0);
  g->controls.opacity
      = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 0.0f, 100.0f, 1.0f, 100.0f, 2);
  dt_bauhaus_widget_set_label(g->controls.opacity, _("Opacity"));
  dt_bauhaus_slider_set_format(g->controls.opacity, "%");
  gtk_box_pack_start(GTK_BOX(brush_tab), g->controls.opacity, TRUE, TRUE, 0);
  g->controls.flow
      = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 0.0f, 100.0f, 1.0f, 100.0f, 2);
  dt_bauhaus_widget_set_label(g->controls.flow, _("Flow"));
  dt_bauhaus_slider_set_format(g->controls.flow, "%");
  gtk_box_pack_start(GTK_BOX(brush_tab), g->controls.flow, TRUE, TRUE, 0);

  GtkWidget *texture_title = gtk_label_new(_("Texture"));
  gtk_label_set_xalign(GTK_LABEL(texture_title), 0.0f);
  dt_gui_add_class(texture_title, "dt_section_label");
  gtk_box_pack_start(GTK_BOX(brush_tab), texture_title, TRUE, TRUE, 0);
  g->controls.sprinkles
      = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 0.0f, 100.0f, 1.0f, 0.0f, 2);
  dt_bauhaus_widget_set_label(g->controls.sprinkles, _("Sprinkles"));
  dt_bauhaus_slider_set_format(g->controls.sprinkles, "%");
  gtk_box_pack_start(GTK_BOX(brush_tab), g->controls.sprinkles, TRUE, TRUE, 0);
  g->controls.sprinkle_size
      = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 1.0f, 256.0f, 1.0f, 3.0f, 0);
  dt_bauhaus_widget_set_label(g->controls.sprinkle_size, _("Sprinkle size"));
  dt_bauhaus_slider_set_format(g->controls.sprinkle_size, _(" px"));
  gtk_box_pack_start(GTK_BOX(brush_tab), g->controls.sprinkle_size, TRUE, TRUE, 0);
  g->controls.sprinkle_coarseness
      = dt_bauhaus_slider_new_with_range(darktable.bauhaus, DT_GUI_MODULE(self), 0.0f, 100.0f, 1.0f, 50.0f, 2);
  dt_bauhaus_widget_set_label(g->controls.sprinkle_coarseness, _("Coarseness"));
  dt_bauhaus_slider_set_format(g->controls.sprinkle_coarseness, "%");
  gtk_box_pack_start(GTK_BOX(brush_tab), g->controls.sprinkle_coarseness, TRUE, TRUE, 0);

  GtkWidget *layer_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_IOP_MODULE_CONTROL_SPACING);
  GtkWidget *layer_name_title = gtk_label_new(_("Layer name"));
  gtk_widget_set_halign(layer_name_title, GTK_ALIGN_START);
  GtkWidget *layer_fill_title = gtk_label_new(_("Fill"));
  gtk_widget_set_halign(layer_fill_title, GTK_ALIGN_START);
  GtkWidget *layer_action_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_IOP_MODULE_CONTROL_SPACING);
  GtkWidget *layer_fill_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_IOP_MODULE_CONTROL_SPACING);
  g->controls.layer_name = GTK_ENTRY(gtk_entry_new());
  dt_accels_disconnect_on_text_input(GTK_WIDGET(g->controls.layer_name));
  g->controls.layer_select = dt_bauhaus_combobox_new(darktable.bauhaus, DT_GUI_MODULE(self));
  dt_bauhaus_widget_set_label(g->controls.layer_select, _("Source layer"));
  g->controls.delete_layer = gtk_button_new_with_label(_("delete layer"));
  g->controls.create_layer = gtk_button_new_with_label(_("create new layer"));
  g->controls.create_background = gtk_button_new_with_label(_("create background from input"));
  g->controls.fill_white = gtk_button_new_with_label(_("white"));
  g->controls.fill_black = gtk_button_new_with_label(_("black"));
  g->controls.fill_transparent = gtk_button_new_with_label(_("transparency"));
  gtk_box_pack_start(GTK_BOX(layer_box), layer_name_title, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(layer_box), GTK_WIDGET(g->controls.layer_name), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(layer_box), g->controls.layer_select, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(layer_action_row), g->controls.create_layer, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(layer_action_row), g->controls.delete_layer, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(layer_box), layer_action_row, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(layer_box), layer_fill_title, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(layer_fill_row), g->controls.fill_white, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(layer_fill_row), g->controls.fill_black, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(layer_fill_row), g->controls.fill_transparent, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(layer_box), layer_fill_row, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(layer_box), g->controls.create_background, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(layer_tab), layer_box, FALSE, FALSE, 0);

  GtkWidget *mapping_title = gtk_label_new(_("tablet mapping"));
  gtk_widget_set_halign(mapping_title, GTK_ALIGN_START);
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 6);

  const char *labels[4] = { _("size"), _("opacity"), _("flow"), _("hardness") };
  const char *rows[3] = { _("pressure"), _("tilt"), _("acceleration") };
  GtkWidget **targets[3][4] = {
    { &g->controls.map_pressure_size, &g->controls.map_pressure_opacity, &g->controls.map_pressure_flow, &g->controls.map_pressure_softness },
    { &g->controls.map_tilt_size, &g->controls.map_tilt_opacity, &g->controls.map_tilt_flow, &g->controls.map_tilt_softness },
    { &g->controls.map_accel_size, &g->controls.map_accel_opacity, &g->controls.map_accel_flow, &g->controls.map_accel_softness },
  };
  GtkWidget **profiles[3] = { &g->controls.pressure_profile, &g->controls.tilt_profile, &g->controls.accel_profile };

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

  g_signal_connect(g->controls.brush_shape, "draw", G_CALLBACK(_brush_profile_draw), self);
  g_signal_connect(g->controls.brush_shape, "button-press-event", G_CALLBACK(_brush_profile_button_press), self);
  g_signal_connect(G_OBJECT(g->controls.brush_mode), "value-changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(g->controls.color, "draw", G_CALLBACK(_color_picker_draw), self);
  g_signal_connect(g->controls.color_swatch, "draw", G_CALLBACK(_color_swatch_draw), self);
  g_signal_connect(g->controls.color_swatch, "button-press-event", G_CALLBACK(_color_swatch_button_press), self);
  g_signal_connect(g->controls.color, "button-press-event", G_CALLBACK(_color_picker_button_press), self);
  g_signal_connect(g->controls.color, "button-release-event", G_CALLBACK(_color_picker_button_release), self);
  g_signal_connect(g->controls.color, "motion-notify-event", G_CALLBACK(_color_picker_motion), self);
  g_signal_connect(G_OBJECT(g->controls.image_colorpicker_source), "value-changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(G_OBJECT(g->controls.size), "value-changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(G_OBJECT(g->controls.distance), "value-changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(G_OBJECT(g->controls.smoothing), "value-changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(G_OBJECT(g->controls.opacity), "value-changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(G_OBJECT(g->controls.flow), "value-changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(G_OBJECT(g->controls.sprinkles), "value-changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(G_OBJECT(g->controls.sprinkle_size), "value-changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(G_OBJECT(g->controls.sprinkle_coarseness), "value-changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(G_OBJECT(g->controls.softness), "value-changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(G_OBJECT(g->controls.hdr_exposure), "value-changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(g->controls.layer_name, "changed", G_CALLBACK(_widget_changed), self);
  g_signal_connect(G_OBJECT(g->controls.layer_select), "value-changed", G_CALLBACK(_layer_selected), self);
  g_signal_connect(g->controls.preview_bg_image, "toggled", G_CALLBACK(_preview_bg_toggled), self);
  g_signal_connect(g->controls.preview_bg_white, "toggled", G_CALLBACK(_preview_bg_toggled), self);
  g_signal_connect(g->controls.preview_bg_grey, "toggled", G_CALLBACK(_preview_bg_toggled), self);
  g_signal_connect(g->controls.preview_bg_black, "toggled", G_CALLBACK(_preview_bg_toggled), self);
  g_signal_connect(g->controls.create_layer, "clicked", G_CALLBACK(_create_layer_clicked), self);
  g_signal_connect(g->controls.create_background, "clicked", G_CALLBACK(_create_background_clicked), self);
  g_signal_connect(g->controls.save_layer, "clicked", G_CALLBACK(_save_layer_clicked), self);
  g_signal_connect(g->controls.delete_layer, "clicked", G_CALLBACK(_delete_layer_clicked), self);
  g_signal_connect(g->controls.fill_white, "clicked", G_CALLBACK(_fill_white_clicked), self);
  g_signal_connect(g->controls.fill_black, "clicked", G_CALLBACK(_fill_black_clicked), self);
  g_signal_connect(g->controls.fill_transparent, "clicked", G_CALLBACK(_fill_transparent_clicked), self);

  for(int r = 0; r < 3; r++)
  {
    for(int c = 0; c < 4; c++) g_signal_connect(*targets[r][c], "toggled", G_CALLBACK(_widget_changed), self);
    g_signal_connect(G_OBJECT(*profiles[r]), "value-changed", G_CALLBACK(_widget_changed), self);
  }

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED,
                                  G_CALLBACK(_develop_ui_pipe_finished_callback), self);

  if(self->dev)
  {
    drawlayer_runtime_host_context_t runtime_host = {
      .runtime = {
        .self = self,
        .runtime_params = (const dt_iop_drawlayer_params_t *)self->params,
        .gui = g,
        .manager = &g->manager,
        .process_state = &g->process,
      },
    };
    const dt_drawlayer_runtime_host_t runtime_manager = {
      .user_data = &runtime_host,
      .collect_inputs = _drawlayer_runtime_collect_inputs,
      .perform_action = _drawlayer_runtime_perform_action,
    };
    dt_drawlayer_runtime_manager_update(
        &g->manager,
        &(dt_drawlayer_runtime_update_request_t){
          .event = DT_DRAWLAYER_RUNTIME_EVENT_GUI_RESYNC,
          .raw_input_kind = DT_DRAWLAYER_RUNTIME_RAW_INPUT_NONE,
        },
        &runtime_manager);
  }
}

/** @brief Refresh GUI controls from current params and configuration. */
void gui_update(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  dt_iop_drawlayer_params_t *params = (dt_iop_drawlayer_params_t *)self->params;
  if(!g) return;

  _ensure_layer_name(self, params);
  _sanitize_params(self, params);

  dt_bauhaus_combobox_set(g->controls.brush_mode, _conf_brush_mode());
  dt_bauhaus_slider_set(g->controls.size, _conf_size());
  dt_bauhaus_slider_set(g->controls.distance, _conf_distance());
  dt_bauhaus_slider_set(g->controls.smoothing, _conf_smoothing());
  dt_bauhaus_slider_set(g->controls.opacity, _conf_opacity());
  dt_bauhaus_slider_set(g->controls.flow, _conf_flow());
  dt_bauhaus_slider_set(g->controls.sprinkles, _conf_sprinkles());
  dt_bauhaus_slider_set(g->controls.sprinkle_size, _conf_sprinkle_size());
  dt_bauhaus_slider_set(g->controls.sprinkle_coarseness, _conf_sprinkle_coarseness());
  dt_bauhaus_slider_set(g->controls.softness, _conf_hardness());
  if(g->controls.image_colorpicker_source) dt_bauhaus_combobox_set(g->controls.image_colorpicker_source, _conf_pick_source());
  dt_bauhaus_slider_set(g->controls.hdr_exposure, _conf_hdr_exposure());

  _sync_color_picker_from_conf(self);
  _sync_brush_profile_preview_widget(self);
  if(g->controls.color) gtk_widget_queue_draw(g->controls.color);
  gtk_entry_set_text(g->controls.layer_name, params->layer_name);

  if(GTK_IS_TOGGLE_BUTTON(g->controls.map_pressure_size))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->controls.map_pressure_size),
                                 dt_conf_get_bool(DRAWLAYER_CONF_MAP_PRESSURE_SIZE));
  if(GTK_IS_TOGGLE_BUTTON(g->controls.map_pressure_opacity))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->controls.map_pressure_opacity),
                                 dt_conf_get_bool(DRAWLAYER_CONF_MAP_PRESSURE_OPACITY));
  if(GTK_IS_TOGGLE_BUTTON(g->controls.map_pressure_flow))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->controls.map_pressure_flow),
                                 dt_conf_get_bool(DRAWLAYER_CONF_MAP_PRESSURE_FLOW));
  if(GTK_IS_TOGGLE_BUTTON(g->controls.map_pressure_softness))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->controls.map_pressure_softness),
                                 dt_conf_get_bool(DRAWLAYER_CONF_MAP_PRESSURE_SOFTNESS));

  if(GTK_IS_TOGGLE_BUTTON(g->controls.map_tilt_size))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->controls.map_tilt_size),
                                 dt_conf_get_bool(DRAWLAYER_CONF_MAP_TILT_SIZE));
  if(GTK_IS_TOGGLE_BUTTON(g->controls.map_tilt_opacity))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->controls.map_tilt_opacity),
                                 dt_conf_get_bool(DRAWLAYER_CONF_MAP_TILT_OPACITY));
  if(GTK_IS_TOGGLE_BUTTON(g->controls.map_tilt_flow))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->controls.map_tilt_flow),
                                 dt_conf_get_bool(DRAWLAYER_CONF_MAP_TILT_FLOW));
  if(GTK_IS_TOGGLE_BUTTON(g->controls.map_tilt_softness))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->controls.map_tilt_softness),
                                 dt_conf_get_bool(DRAWLAYER_CONF_MAP_TILT_SOFTNESS));

  if(GTK_IS_TOGGLE_BUTTON(g->controls.map_accel_size))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->controls.map_accel_size),
                                 dt_conf_get_bool(DRAWLAYER_CONF_MAP_ACCEL_SIZE));
  if(GTK_IS_TOGGLE_BUTTON(g->controls.map_accel_opacity))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->controls.map_accel_opacity),
                                 dt_conf_get_bool(DRAWLAYER_CONF_MAP_ACCEL_OPACITY));
  if(GTK_IS_TOGGLE_BUTTON(g->controls.map_accel_flow))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->controls.map_accel_flow),
                                 dt_conf_get_bool(DRAWLAYER_CONF_MAP_ACCEL_FLOW));
  if(GTK_IS_TOGGLE_BUTTON(g->controls.map_accel_softness))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->controls.map_accel_softness),
                                 dt_conf_get_bool(DRAWLAYER_CONF_MAP_ACCEL_SOFTNESS));

  if(g->controls.pressure_profile)
    dt_bauhaus_combobox_set(g->controls.pressure_profile, _conf_mapping_profile(DRAWLAYER_CONF_PRESSURE_PROFILE));
  if(g->controls.tilt_profile) dt_bauhaus_combobox_set(g->controls.tilt_profile, _conf_mapping_profile(DRAWLAYER_CONF_TILT_PROFILE));
  if(g->controls.accel_profile)
    dt_bauhaus_combobox_set(g->controls.accel_profile, _conf_mapping_profile(DRAWLAYER_CONF_ACCEL_PROFILE));

  _sync_mode_sensitive_widgets(self);
  _sync_preview_bg_buttons(self);
  _populate_layer_list(self);

  if(self->dev)
  {
    drawlayer_runtime_host_context_t runtime_host = {
      .runtime = {
        .self = self,
        .runtime_params = (const dt_iop_drawlayer_params_t *)self->params,
        .gui = g,
        .manager = &g->manager,
        .process_state = &g->process,
      },
    };
    const dt_drawlayer_runtime_host_t runtime_manager = {
      .user_data = &runtime_host,
      .collect_inputs = _drawlayer_runtime_collect_inputs,
      .perform_action = _drawlayer_runtime_perform_action,
    };
    dt_drawlayer_runtime_manager_update(
        &g->manager,
        &(dt_drawlayer_runtime_update_request_t){
          .event = DT_DRAWLAYER_RUNTIME_EVENT_GUI_RESYNC,
          .raw_input_kind = DT_DRAWLAYER_RUNTIME_RAW_INPUT_NONE,
        },
        &runtime_manager);
  }
}

/** @brief Invalidate module state when active image changes. */
void change_image(dt_iop_module_t *self)
{
  if(self->gui_data)
  {
    dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
    drawlayer_runtime_host_context_t runtime_host = {
      .runtime = {
        .self = self,
        .runtime_params = (const dt_iop_drawlayer_params_t *)self->params,
        .gui = g,
        .manager = &g->manager,
        .process_state = &g->process,
      },
    };
    const dt_drawlayer_runtime_host_t runtime_manager = {
      .user_data = &runtime_host,
      .collect_inputs = _drawlayer_runtime_collect_inputs,
      .perform_action = _drawlayer_runtime_perform_action,
    };
    const dt_drawlayer_runtime_update_request_t update = {
      .event = DT_DRAWLAYER_RUNTIME_EVENT_GUI_CHANGE_IMAGE,
      .raw_input_kind = DT_DRAWLAYER_RUNTIME_RAW_INPUT_NONE,
    };
    dt_drawlayer_runtime_manager_update(&g->manager, &update, &runtime_manager);
  }
}

/** @brief Focus transition hook (enter/leave) for drawlayer GUI mode. */
void gui_focus(dt_iop_module_t *self, gboolean in)
{
  if(!in)
  {
    dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
    dt_iop_drawlayer_params_t *params = (dt_iop_drawlayer_params_t *)self->params;
    const int pending_samples = g ? (int)g->stroke.stroke_sample_count : 0;
    const gboolean had_pending_edits
        = (g && (g->process.cache_dirty || g->process.process_patch_dirty || g->stroke.stroke_sample_count > 0));
    drawlayer_runtime_host_context_t runtime_host = {
      .runtime = {
        .self = self,
        .runtime_params = (const dt_iop_drawlayer_params_t *)self->params,
        .gui = g,
        .manager = &g->manager,
        .process_state = &g->process,
      },
    };
    const dt_drawlayer_runtime_host_t runtime_manager = {
      .user_data = &runtime_host,
      .collect_inputs = _drawlayer_runtime_collect_inputs,
      .perform_action = _drawlayer_runtime_perform_action,
    };
    const dt_drawlayer_runtime_update_request_t update = {
      .event = DT_DRAWLAYER_RUNTIME_EVENT_GUI_FOCUS_LOSS,
      .raw_input_kind = DT_DRAWLAYER_RUNTIME_RAW_INPUT_NONE,
    };
    _set_drawlayer_os_cursor_hidden(FALSE);
    if(g) dt_drawlayer_runtime_manager_update(&g->manager, &update, &runtime_manager);
    if(had_pending_edits && params)
      _touch_stroke_commit_hash(params, pending_samples, g->stroke.last_dab_valid, g->stroke.last_dab_x,
                                g->stroke.last_dab_y, 0u);
    if(had_pending_edits && self->dev && params)
    {
      dt_dev_undo_start_record(self->dev);
      dt_pthread_rwlock_wrlock(&self->dev->history_mutex);
      dt_dev_add_history_item_ext(self->dev, self, TRUE, FALSE);
      dt_dev_set_history_hash(self->dev, dt_dev_history_compute_hash(self->dev));
      dt_pthread_rwlock_unlock(&self->dev->history_mutex);
      dt_dev_undo_end_record(self->dev);
      if(self->post_history_commit) self->post_history_commit(self);
      dt_dev_write_history(self->dev);
      dt_dev_history_notify_change(self->dev, self->dev->image_storage.id);
    }
  }
  else if(self->gui_data)
  {
    dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
    dt_iop_drawlayer_params_t *params = (dt_iop_drawlayer_params_t *)self->params;
    drawlayer_runtime_host_context_t runtime_host = {
      .runtime = {
        .self = self,
        .runtime_params = (const dt_iop_drawlayer_params_t *)self->params,
        .gui = g,
        .manager = &g->manager,
        .process_state = &g->process,
      },
    };
    const dt_drawlayer_runtime_host_t runtime_manager = {
      .user_data = &runtime_host,
      .collect_inputs = _drawlayer_runtime_collect_inputs,
      .perform_action = _drawlayer_runtime_perform_action,
    };
    const dt_drawlayer_runtime_update_request_t update = {
      .event = DT_DRAWLAYER_RUNTIME_EVENT_GUI_FOCUS_GAIN,
      .raw_input_kind = DT_DRAWLAYER_RUNTIME_RAW_INPUT_NONE,
    };
    if(g)
    {
      g->session.missing_layer_prompt_name[0] = '\0';
    }

    dt_drawlayer_runtime_manager_update(&g->manager, &update, &runtime_manager);

    if(g && params && !g->process.cache_valid && _current_layer_missing_in_sidecar(self))
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
  if(g == NULL) return;

  _set_drawlayer_os_cursor_hidden(FALSE);
  _update_gui_runtime_manager(self, g, DT_DRAWLAYER_RUNTIME_EVENT_GUI_FOCUS_LOSS, FALSE);

  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_develop_ui_pipe_finished_callback), self);

  _release_all_base_patch_extra_refs(g);
  dt_drawlayer_process_state_cleanup(&g->process);
  memset(&g->session.live_patch, 0, sizeof(g->session.live_patch));
  dt_drawlayer_widgets_cleanup(&g->ui.widgets);
  dt_drawlayer_ui_cursor_clear(&g->ui);
  dt_drawlayer_worker_cleanup(&g->stroke.worker);
  
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

  const float pressure_norm
      = _clamp01((pointer_input && pointer_input->has_pressure) ? pointer_input->pressure : 1.0f);
  const float tilt_norm = _clamp01((pointer_input && pointer_input->has_tilt) ? pointer_input->tilt : 0.0f);
  const float accel_norm = _clamp01(pointer_input ? pointer_input->acceleration : 0.0f);
  const drawlayer_mapping_profile_t pressure_profile = _conf_mapping_profile(DRAWLAYER_CONF_PRESSURE_PROFILE);
  const drawlayer_mapping_profile_t tilt_profile = _conf_mapping_profile(DRAWLAYER_CONF_TILT_PROFILE);
  const drawlayer_mapping_profile_t accel_profile = _conf_mapping_profile(DRAWLAYER_CONF_ACCEL_PROFILE);
  const float pressure_coeff = _mapping_profile_value(pressure_profile, pressure_norm);
  const float tilt_coeff = _mapping_profile_value(tilt_profile, tilt_norm);
  const float accel_coeff = _mapping_profile_value(accel_profile, accel_norm);

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
  g_snprintf(lines[0], sizeof(lines[0]), _("size %.1f px  hardness %.2f%%"), state->radius * 2.0f,
             state->hardness * 100.0f);
  g_snprintf(lines[1], sizeof(lines[1]), _("opacity %.2f%%  flow %.2f%%"), state->opacity * 100.0f,
             state->flow * 100.0f);
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
void gui_post_expose(dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx,
                     int32_t pointery)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  if(!g || !self->dev) return;

  cairo_save(cr);
  cairo_set_line_width(cr, 1.0);
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.8);

  if(g->session.pointer_valid)
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
      if(g->ui.cursor_surface)
      {
        const float surface_half_extent = 0.5f * (float)g->ui.cursor_surface_size / (float)g->ui.cursor_surface_ppd;
        cairo_set_source_surface(cr, g->ui.cursor_surface, draw_x - surface_half_extent, draw_y - surface_half_extent);
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
  drawlayer_runtime_host_context_t runtime_host = {
    .runtime = {
      .self = self,
      .runtime_params = (const dt_iop_drawlayer_params_t *)self->params,
      .gui = g,
      .manager = &g->manager,
      .process_state = &g->process,
    },
  };
  const dt_drawlayer_runtime_host_t runtime_manager = {
    .user_data = &runtime_host,
    .collect_inputs = _drawlayer_runtime_collect_inputs,
    .perform_action = _drawlayer_runtime_perform_action,
  };
  const dt_drawlayer_runtime_update_request_t update = {
    .event = DT_DRAWLAYER_RUNTIME_EVENT_GUI_MOUSE_LEAVE,
    .raw_input_kind = DT_DRAWLAYER_RUNTIME_RAW_INPUT_NONE,
  };
  dt_drawlayer_runtime_manager_update(&g->manager, &update, &runtime_manager);
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
    drawlayer_runtime_host_context_t runtime_host = {
      .runtime = {
        .self = self,
        .runtime_params = (const dt_iop_drawlayer_params_t *)self->params,
        .gui = g,
        .manager = &g->manager,
        .process_state = &g->process,
      },
    };
    const dt_drawlayer_runtime_host_t runtime_manager = {
      .user_data = &runtime_host,
      .collect_inputs = _drawlayer_runtime_collect_inputs,
      .perform_action = _drawlayer_runtime_perform_action,
    };
    const dt_drawlayer_runtime_update_request_t leave_update = {
      .event = DT_DRAWLAYER_RUNTIME_EVENT_GUI_MOUSE_LEAVE,
      .raw_input_kind = DT_DRAWLAYER_RUNTIME_RAW_INPUT_NONE,
    };
    const dt_drawlayer_runtime_update_request_t update = {
      .event = DT_DRAWLAYER_RUNTIME_EVENT_GUI_STROKE_ABORT,
      .raw_input_kind = DT_DRAWLAYER_RUNTIME_RAW_INPUT_NONE,
    };
    if(g->session.pointer_valid) dt_drawlayer_runtime_manager_update(&g->manager, &leave_update, &runtime_manager);
    dt_drawlayer_runtime_manager_update(&g->manager, &update, &runtime_manager);
    return 0;
  }

  if(!g->session.pointer_valid)
  {
    drawlayer_runtime_host_context_t enter_host = {
      .runtime = {
        .self = self,
        .runtime_params = (const dt_iop_drawlayer_params_t *)self->params,
        .gui = g,
        .manager = &g->manager,
        .process_state = &g->process,
      },
    };
    const dt_drawlayer_runtime_host_t enter_manager = {
      .user_data = &enter_host,
      .collect_inputs = _drawlayer_runtime_collect_inputs,
      .perform_action = _drawlayer_runtime_perform_action,
    };
    dt_drawlayer_runtime_manager_update(
        &g->manager,
        &(dt_drawlayer_runtime_update_request_t){
          .event = DT_DRAWLAYER_RUNTIME_EVENT_GUI_MOUSE_ENTER,
          .raw_input_kind = DT_DRAWLAYER_RUNTIME_RAW_INPUT_NONE,
        },
        &enter_manager);
  }

  if(g->manager.painting_active)
  {
    dt_drawlayer_paint_raw_input_t input = { 0 };
    drawlayer_runtime_host_context_t runtime_host = {
      .runtime = {
        .self = self,
        .runtime_params = (const dt_iop_drawlayer_params_t *)self->params,
        .gui = g,
        .manager = &g->manager,
        .process_state = &g->process,
      },
      .raw_input = &input,
    };
    const dt_drawlayer_runtime_host_t runtime_manager = {
      .user_data = &runtime_host,
      .collect_inputs = _drawlayer_runtime_collect_inputs,
      .perform_action = _drawlayer_runtime_perform_action,
    };
    const dt_drawlayer_runtime_update_request_t update = {
      .event = DT_DRAWLAYER_RUNTIME_EVENT_GUI_RAW_INPUT,
      .raw_input_kind = DT_DRAWLAYER_RUNTIME_RAW_INPUT_SAMPLE,
    };
    const dt_drawlayer_runtime_result_t dispatch
        = _build_raw_input_event(self, x, y, pressure, DT_DRAWLAYER_PAINT_STROKE_MIDDLE, &input)
              ? dt_drawlayer_runtime_manager_update(&g->manager, &update, &runtime_manager)
              : (dt_drawlayer_runtime_result_t){ .ok = FALSE, .raw_input_ok = FALSE };
    if(!dispatch.ok || !dispatch.raw_input_ok)
    {
      /* Queue overflow or enqueue failure aborts the current stroke so GUI and
       * worker stay in sync on stroke boundaries. */
      dt_drawlayer_runtime_manager_update(
          &g->manager,
          &(dt_drawlayer_runtime_update_request_t){
            .event = DT_DRAWLAYER_RUNTIME_EVENT_GUI_STROKE_ABORT,
            .raw_input_kind = DT_DRAWLAYER_RUNTIME_RAW_INPUT_NONE,
          },
          &runtime_manager);
    }
  }
  else
  {
    drawlayer_runtime_host_context_t runtime_host = {
      .runtime = {
        .self = self,
        .runtime_params = (const dt_iop_drawlayer_params_t *)self->params,
        .gui = g,
        .manager = &g->manager,
        .process_state = &g->process,
      },
    };
    const dt_drawlayer_runtime_host_t runtime_manager = {
      .user_data = &runtime_host,
      .collect_inputs = _drawlayer_runtime_collect_inputs,
      .perform_action = _drawlayer_runtime_perform_action,
    };
    const dt_drawlayer_runtime_update_request_t update = {
      .event = DT_DRAWLAYER_RUNTIME_EVENT_GUI_RAW_INPUT,
      .raw_input_kind = DT_DRAWLAYER_RUNTIME_RAW_INPUT_SAMPLE,
    };
    dt_drawlayer_runtime_manager_update(&g->manager, &update, &runtime_manager);
  }

  gtk_widget_queue_draw(dt_ui_center(darktable.gui->ui));
  return g->manager.painting_active ? 1 : 0;
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

  dt_control_pointer_input_t pointer_input = { 0 };
  dt_control_get_pointer_input(&pointer_input);
  const float input_wx = isfinite(pointer_input.x) ? (float)pointer_input.x : (float)x;
  const float input_wy = isfinite(pointer_input.y) ? (float)pointer_input.y : (float)y;
  const float pressure_norm = pointer_input.has_pressure ? _clamp01(pointer_input.pressure) : _clamp01(pressure);
  uint32_t stroke_batch = g->stroke.current_stroke_batch + 1u;
  if(stroke_batch == 0u) stroke_batch++;
  dt_drawlayer_paint_raw_input_t first = {
    .wx = input_wx,
    .wy = input_wy,
    .pressure = pressure_norm,
    .tilt = (float)_clamp01(pointer_input.tilt),
    .acceleration = (float)_clamp01(pointer_input.acceleration),
    .event_ts = g_get_monotonic_time(),
    .stroke_batch = stroke_batch,
    .event_index = 1u,
    .stroke_pos = DT_DRAWLAYER_PAINT_STROKE_FIRST,
  };
  _fill_input_layer_coords(self, &first);
  _fill_input_brush_settings(self, &first);
  drawlayer_runtime_host_context_t runtime_host = {
    .runtime = {
      .self = self,
      .runtime_params = (const dt_iop_drawlayer_params_t *)self->params,
      .gui = g,
      .manager = &g->manager,
      .process_state = &g->process,
    },
    .raw_input = &first,
  };
  const dt_drawlayer_runtime_host_t runtime_manager = {
    .user_data = &runtime_host,
    .collect_inputs = _drawlayer_runtime_collect_inputs,
    .perform_action = _drawlayer_runtime_perform_action,
  };
  const dt_drawlayer_runtime_update_request_t update = {
    .event = DT_DRAWLAYER_RUNTIME_EVENT_GUI_RAW_INPUT,
    .raw_input_kind = DT_DRAWLAYER_RUNTIME_RAW_INPUT_STROKE_BEGIN,
  };
  const dt_drawlayer_runtime_result_t dispatch
      = dt_drawlayer_runtime_manager_update(&g->manager, &update, &runtime_manager);
  if(!dispatch.ok)
    return 0;
  if(!dispatch.raw_input_ok)
    dt_control_log(_("failed to queue live drawing stroke"));
  dt_control_queue_redraw_center();
  return 1;
}

/** @brief Button release handler (ends current stroke). */
int button_released(dt_iop_module_t *self, double x, double y, int which, uint32_t state)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  if(!g || !self->dev || which != 1) return 0;

  if(g->manager.painting_active)
  {
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
      .stroke_batch = g->stroke.current_stroke_batch,
      .event_index = ++g->stroke.stroke_event_index,
      .stroke_pos = DT_DRAWLAYER_PAINT_STROKE_END,
    };
    _fill_input_layer_coords(self, &end);
    _fill_input_brush_settings(self, &end);
    drawlayer_runtime_host_context_t runtime_host = {
      .runtime = {
        .self = self,
        .runtime_params = (const dt_iop_drawlayer_params_t *)self->params,
        .gui = g,
        .manager = &g->manager,
        .process_state = &g->process,
      },
      .raw_input = &end,
    };
    const dt_drawlayer_runtime_host_t runtime_manager = {
      .user_data = &runtime_host,
      .collect_inputs = _drawlayer_runtime_collect_inputs,
      .perform_action = _drawlayer_runtime_perform_action,
    };
    const dt_drawlayer_runtime_update_request_t update = {
      .event = DT_DRAWLAYER_RUNTIME_EVENT_GUI_RAW_INPUT,
      .raw_input_kind = DT_DRAWLAYER_RUNTIME_RAW_INPUT_STROKE_END,
    };
    const dt_drawlayer_runtime_result_t dispatch
        = dt_drawlayer_runtime_manager_update(&g->manager, &update, &runtime_manager);
    if(!dispatch.ok || !dispatch.raw_input_ok)
      dt_control_log(_("failed to queue drawing stroke end"));
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
    dt_bauhaus_slider_set(g->controls.size, new_size);
    drawlayer_runtime_host_context_t runtime_host = {
      .runtime = {
        .self = self,
        .runtime_params = (const dt_iop_drawlayer_params_t *)self->params,
        .gui = g,
        .manager = &g->manager,
        .process_state = &g->process,
      },
    };
    const dt_drawlayer_runtime_host_t runtime_manager = {
      .user_data = &runtime_host,
      .collect_inputs = _drawlayer_runtime_collect_inputs,
      .perform_action = _drawlayer_runtime_perform_action,
    };
    const dt_drawlayer_runtime_update_request_t update = {
      .event = DT_DRAWLAYER_RUNTIME_EVENT_GUI_SCROLL,
      .raw_input_kind = DT_DRAWLAYER_RUNTIME_RAW_INPUT_NONE,
    };
    dt_drawlayer_runtime_manager_update(&g->manager, &update, &runtime_manager);
  }
  return 1;
}

#ifdef HAVE_OPENCL
/** @brief OpenCL processing path for layer-over-input compositing. */
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const gint64 process_t0 = g_get_monotonic_time();
  const dt_iop_drawlayer_global_data_t *global = (const dt_iop_drawlayer_global_data_t *)self->global_data;
  dt_iop_drawlayer_gui_data_t *gui = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  const gboolean have_gui = (gui != NULL);
  {
    const gboolean display_pipe = have_gui && (piece->pipe == self->dev->pipe || piece->pipe == self->dev->preview_pipe);
    dt_iop_drawlayer_data_t *data = (dt_iop_drawlayer_data_t *)piece->data;
    dt_drawlayer_runtime_manager_bind_piece(&data->headless_manager, &data->process, gui ? &gui->manager : NULL,
                                            gui ? &gui->process : NULL, display_pipe, &data->runtime_manager,
                                            &data->runtime_process, &data->runtime_display_pipe);
  }
  dt_iop_drawlayer_data_t *runtime_data = (dt_iop_drawlayer_data_t *)piece->data;
  const dt_iop_drawlayer_params_t *runtime_params
      = &runtime_data->params;
  const drawlayer_runtime_request_t runtime_request = {
    .self = self,
    .piece = piece,
    .runtime_params = runtime_params,
    .gui = gui,
    .manager = runtime_data->runtime_manager,
    .process_state = runtime_data->runtime_process,
    .display_pipe = runtime_data->runtime_display_pipe,
    .roi_in = roi_in,
    .roi_out = roi_out,
    .use_opencl = TRUE,
  };
  drawlayer_runtime_host_context_t runtime_host = {
    .runtime = runtime_request,
  };
  const dt_drawlayer_runtime_host_t runtime_manager = {
    .user_data = &runtime_host,
    .collect_inputs = _drawlayer_runtime_collect_inputs,
    .perform_action = _drawlayer_runtime_perform_action,
  };
  const dt_drawlayer_runtime_update_request_t process_pre = {
    .event = DT_DRAWLAYER_RUNTIME_EVENT_PROCESS_CL_BEFORE,
    .raw_input_kind = DT_DRAWLAYER_RUNTIME_RAW_INPUT_NONE,
  };
  dt_drawlayer_runtime_update_request_t process_post = {
    .event = DT_DRAWLAYER_RUNTIME_EVENT_PROCESS_CL_AFTER,
    .raw_input_kind = DT_DRAWLAYER_RUNTIME_RAW_INPUT_NONE,
  };
  dt_drawlayer_runtime_manager_t *const manager = runtime_request.manager;
  dt_drawlayer_runtime_manager_update(manager, &process_pre, &runtime_manager);
  if(!global || global->kernel_premult_over < 0)
  {
    dt_drawlayer_runtime_manager_update(manager, &process_post, &runtime_manager);
    return FALSE;
  }
  gboolean fallback = (runtime_params->layer_name[0] == '\0');

  const drawlayer_preview_background_t preview_bg = _resolve_preview_background(self, gui);
  drawlayer_process_scratch_t *scratch = _get_process_scratch();
  if(!scratch)
  {
    dt_drawlayer_runtime_manager_update(manager, &process_post, &runtime_manager);
    return FALSE;
  }

  dt_drawlayer_runtime_source_t source = { 0 };
  if(!fallback) fallback = !_update_runtime_state(&runtime_request, &source);
  if(!fallback) fallback = (!have_gui && source.kind == DT_DRAWLAYER_SOURCE_GUI_PROCESS);
  if(!fallback)
  {
    const char *const source_label
        = (source.kind == DT_DRAWLAYER_SOURCE_GUI_PROCESS) ? "process-patch" : "headless";
    const gboolean realtime = dt_dev_pixelpipe_get_realtime(piece->pipe);
    const gboolean reuse_process_clmem = (source.kind == DT_DRAWLAYER_SOURCE_GUI_PROCESS);
    const gboolean reuse_device_buffers = realtime || reuse_process_clmem;
    dt_iop_roi_t target_roi = source.target_roi;
    dt_iop_roi_t source_roi = source.source_roi;
    int source_width = source.width;
    int source_height = source.height;
    gboolean direct_copy = source.direct_copy;
    const float *source_pixels = source.pixels;
    dt_pixel_cache_entry_t *source_entry = source.cache_entry;
    cl_mem source_mem = NULL;
    if(reuse_process_clmem)
    {
      const dt_drawlayer_cache_patch_t *const process_patch = source.process_view.patch;
      dt_drawlayer_cache_patch_rdlock(process_patch);
      if(process_patch->pixels && process_patch->width > 0 && process_patch->height > 0
         && dt_drawlayer_cache_build_process_blend_rois(process_patch, runtime_request.process_state->process_patch_padding,
                                                        roi_out, &target_roi, &source_roi, &direct_copy))
      {
        source_width = process_patch->width;
        source_height = process_patch->height;
        source_entry = process_patch->cache_entry;
        source_mem = gui ? dt_drawlayer_process_state_ensure_read_clmem_locked(&gui->process, piece->pipe->devid)
                         : NULL;
        if(!source_mem)
        {
          const size_t source_pixel_count = (size_t)source_width * source_height;
          float *local_copy = dt_drawlayer_cache_ensure_scratch_buffer(&scratch->layerbuf, &scratch->layerbuf_pixels,
                                                                       source_pixel_count,
                                                                       "drawlayer process scratch");
          if(local_copy)
          {
            memcpy(local_copy, process_patch->pixels, source_pixel_count * 4 * sizeof(float));
            source_pixels = local_copy;
            source_entry = NULL;
          }
          else
            fallback = TRUE;
        }
      }
      else
        fallback = TRUE;
      dt_drawlayer_cache_patch_rdunlock(process_patch);
    }

    if(fallback)
      goto process_cl_fallback;

    gboolean ok = _blend_layer_over_input_cl(
        piece->pipe->devid, global->kernel_premult_over, dev_out, dev_in, scratch, source_pixels, source_entry,
        source_mem, source_width, source_height, &target_roi, &source_roi, direct_copy,
        preview_bg.enabled, preview_bg.value, reuse_device_buffers,
        reuse_process_clmem && !source_mem);

    if(!ok && reuse_process_clmem && source_mem)
    {
      if(gui) dt_drawlayer_process_state_clear_clmem(&gui->process);
      const dt_drawlayer_cache_patch_t *const process_patch = source.process_view.patch;
      dt_drawlayer_cache_patch_rdlock(process_patch);
      if(process_patch->pixels && process_patch->width > 0 && process_patch->height > 0
         && dt_drawlayer_cache_build_process_blend_rois(process_patch, runtime_request.process_state->process_patch_padding,
                                                        roi_out, &target_roi, &source_roi, &direct_copy))
      {
        const size_t source_pixel_count = (size_t)process_patch->width * process_patch->height;
        float *local_copy = dt_drawlayer_cache_ensure_scratch_buffer(&scratch->layerbuf, &scratch->layerbuf_pixels,
                                                                     source_pixel_count,
                                                                     "drawlayer process scratch");
        if(local_copy)
        {
          memcpy(local_copy, process_patch->pixels, source_pixel_count * 4 * sizeof(float));
          ok = _blend_layer_over_input_cl(piece->pipe->devid, global->kernel_premult_over, dev_out, dev_in, scratch,
                                          local_copy, NULL, NULL, process_patch->width, process_patch->height,
                                          &target_roi, &source_roi, direct_copy,
                                          preview_bg.enabled, preview_bg.value, reuse_device_buffers, TRUE);
        }
      }
      dt_drawlayer_cache_patch_rdunlock(process_patch);
    }

    process_post.release = (dt_drawlayer_runtime_release_t){
      .process = runtime_request.process_state,
      .source = &source,
    };
    if(darktable.unmuted & DT_DEBUG_VERBOSE)
      dt_print(DT_DEBUG_PERF, "[drawlayer] process_cl step=blend-%s total=%.3f ok=%d\n",
               source_label, (g_get_monotonic_time() - process_t0) / 1000.0, ok ? 1 : 0);
    dt_drawlayer_runtime_manager_update(manager, &process_post, &runtime_manager);
    return ok;
  }

process_cl_fallback:
  if(darktable.unmuted & DT_DEBUG_VERBOSE)
    dt_print(DT_DEBUG_PERF, "[drawlayer] process_cl step=no-cache-pass-through total=%.3f\n",
             (g_get_monotonic_time() - process_t0) / 1000.0);
  const gboolean ok = dt_iop_clip_and_zoom_roi_cl(piece->pipe->devid, dev_out, dev_in, roi_out, roi_in)
                      == CL_SUCCESS;
  dt_drawlayer_runtime_manager_update(manager, &process_post, &runtime_manager);
  return ok;
}
#endif

/** @brief CPU processing path for layer-over-input compositing. */
int process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
            const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_drawlayer_gui_data_t *gui = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  const gboolean have_gui = (gui != NULL);
  {
    const gboolean display_pipe = have_gui && (piece->pipe == self->dev->pipe || piece->pipe == self->dev->preview_pipe);
    dt_iop_drawlayer_data_t *data = (dt_iop_drawlayer_data_t *)piece->data;
    dt_drawlayer_runtime_manager_bind_piece(&data->headless_manager, &data->process, gui ? &gui->manager : NULL,
                                            gui ? &gui->process : NULL, display_pipe, &data->runtime_manager,
                                            &data->runtime_process, &data->runtime_display_pipe);
  }
  dt_iop_drawlayer_data_t *runtime_data = (dt_iop_drawlayer_data_t *)piece->data;
  const dt_iop_drawlayer_params_t *runtime_params
      = &runtime_data->params;
  const drawlayer_runtime_request_t runtime_request = {
    .self = self,
    .piece = piece,
    .runtime_params = runtime_params,
    .gui = gui,
    .manager = runtime_data->runtime_manager,
    .process_state = runtime_data->runtime_process,
    .display_pipe = runtime_data->runtime_display_pipe,
    .roi_in = roi_in,
    .roi_out = roi_out,
    .use_opencl = FALSE,
  };
  drawlayer_runtime_host_context_t runtime_host = {
    .runtime = runtime_request,
  };
  const dt_drawlayer_runtime_host_t runtime_manager = {
    .user_data = &runtime_host,
    .collect_inputs = _drawlayer_runtime_collect_inputs,
    .perform_action = _drawlayer_runtime_perform_action,
  };
  const dt_drawlayer_runtime_update_request_t process_pre = {
    .event = DT_DRAWLAYER_RUNTIME_EVENT_PROCESS_CPU_BEFORE,
    .raw_input_kind = DT_DRAWLAYER_RUNTIME_RAW_INPUT_NONE,
  };
  dt_drawlayer_runtime_update_request_t process_post = {
    .event = DT_DRAWLAYER_RUNTIME_EVENT_PROCESS_CPU_AFTER,
    .raw_input_kind = DT_DRAWLAYER_RUNTIME_RAW_INPUT_NONE,
  };
  const float *input = (const float *)ivoid;
  float *output = (float *)ovoid;
  const size_t pixels = (size_t)roi_out->width * roi_out->height;
  const gint64 process_t0 = g_get_monotonic_time();
  dt_drawlayer_runtime_manager_t *const manager = runtime_request.manager;
  dt_drawlayer_runtime_manager_update(manager, &process_pre, &runtime_manager);

  /* `process()` keeps the pipeline contract simple:
   * - the in-memory cache and stroke math stay in float32,
   * - TIFF I/O converts to/from half-float only at the file boundary. */
  gboolean fallback = (runtime_params->layer_name[0] == '\0');
  const drawlayer_preview_background_t preview_bg = _resolve_preview_background(self, gui);

  dt_drawlayer_runtime_source_t source = { 0 };
  if(!fallback) fallback = !_update_runtime_state(&runtime_request, &source);
  if(!fallback) fallback = (!have_gui && source.kind == DT_DRAWLAYER_SOURCE_GUI_PROCESS);
  if(!fallback)
  {
    const char *const source_label
        = (source.kind == DT_DRAWLAYER_SOURCE_GUI_PROCESS) ? "process-patch" : "headless";
    const float *layer_pixels = source.pixels;
    if(source.kind == DT_DRAWLAYER_SOURCE_GUI_PROCESS)
    {
      drawlayer_process_scratch_t *scratch = _get_process_scratch();
      if(!scratch) fallback = TRUE;

      float *layerbuf = NULL;
      if(!fallback)
        layerbuf = dt_drawlayer_cache_ensure_scratch_buffer(&scratch->layerbuf, &scratch->layerbuf_pixels, pixels,
                                                            "drawlayer process scratch");
      if(!fallback && !layerbuf) fallback = TRUE;
      if(fallback)
      {
        process_post.release = (dt_drawlayer_runtime_release_t){
          .process = runtime_request.process_state,
          .source = &source,
        };
        goto fallback_pass_through;
      }

      dt_drawlayer_cache_patch_rdlock(source.process_view.patch);
      if(source.process_view.patch->pixels && source.process_view.patch->width > 0
         && source.process_view.patch->height > 0
         && dt_drawlayer_cache_build_process_blend_rois(source.process_view.patch,
                                                        runtime_request.process_state->process_patch_padding,
                                                        roi_out, &source.target_roi, &source.source_roi,
                                                        &source.direct_copy))
      {
        if(source.direct_copy)
          memcpy(layerbuf, source.process_view.patch->pixels, pixels * 4 * sizeof(float));
        else
          dt_iop_clip_and_zoom(layerbuf, source.process_view.patch->pixels, &source.target_roi, &source.source_roi,
                               roi_out->width, source.process_view.patch->width);
      }
      else
        fallback = TRUE;
      dt_drawlayer_cache_patch_rdunlock(source.process_view.patch);
      if(fallback)
      {
        process_post.release = (dt_drawlayer_runtime_release_t){
          .process = runtime_request.process_state,
          .source = &source,
        };
        goto fallback_pass_through;
      }
      layer_pixels = layerbuf;
    }
    else if(!source.direct_copy)
    {
      drawlayer_process_scratch_t *scratch = _get_process_scratch();
      if(!scratch) fallback = TRUE;

      float *layerbuf = NULL;
      if(!fallback)
        layerbuf = dt_drawlayer_cache_ensure_scratch_buffer(&scratch->layerbuf, &scratch->layerbuf_pixels, pixels,
                                                            "drawlayer process scratch");
      if(!fallback && !layerbuf) fallback = TRUE;
      if(fallback)
      {
        process_post.release = (dt_drawlayer_runtime_release_t){
          .process = runtime_request.process_state,
          .source = &source,
        };
        goto fallback_pass_through;
      }
      dt_iop_clip_and_zoom(layerbuf, source.pixels, &source.target_roi, &source.source_roi, roi_out->width,
                           source.width);
      layer_pixels = layerbuf;
    }

    _blend_layer_over_input(output, input, layer_pixels, pixels, preview_bg.enabled, preview_bg.value);
    if(darktable.unmuted & DT_DEBUG_VERBOSE)
      dt_print(DT_DEBUG_PERF, "[drawlayer] process step=blend-%s total=%.3f\n", source_label,
               (g_get_monotonic_time() - process_t0) / 1000.0);

    process_post.release = (dt_drawlayer_runtime_release_t){
      .process = runtime_request.process_state,
      .source = &source,
    };
    dt_drawlayer_runtime_manager_update(manager, &process_post, &runtime_manager);
    return 0;
  }

  /* The sidecar is intentionally managed outside of `process()`. Once a layer
   * is loaded, the in-memory caches are authoritative until module-level flush
   * points write them back. If we do not have a usable in-memory cache here,
   * the correct backend behavior is therefore a no-op pass-through rather than
   * reopening/scanning/loading the TIFF in the hot process path. */
fallback_pass_through:
  if(darktable.unmuted & DT_DEBUG_VERBOSE)
    dt_print(DT_DEBUG_PERF, "[drawlayer] process step=no-cache-pass-through total=%.3f\n",
             (g_get_monotonic_time() - process_t0) / 1000.0);
  dt_iop_image_copy_by_size(output, input, roi_out->width, roi_out->height, 4);
  dt_drawlayer_runtime_manager_update(manager, &process_post, &runtime_manager);
  return 0;
}
