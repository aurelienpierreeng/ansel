#pragma once

#include "iop/drawlayer/runtime.h"

#include <math.h>

/** @file
 *  @brief Private drawlayer module types and lightweight shared helpers.
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

typedef dt_drawlayer_cache_patch_t drawlayer_patch_t;

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

static inline float _clamp01(const float value)
{
  return fminf(fmaxf(value, 0.0f), 1.0f);
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
