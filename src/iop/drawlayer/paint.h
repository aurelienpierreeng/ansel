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

#pragma once

#include "iop/drawlayer/brush.h"

#include <glib.h>
#include <stdint.h>

/**
 * @file paint.h
 * @brief Stroke-level path sampling and runtime-state API for drawlayer.
 *
 * This layer consumes raw pointer input events and emits evenly spaced,
 * optionally smoothed dabs. Rasterization is delegated to brush.h.
 */

/** @brief Position of a raw input event inside a stroke lifecycle. */
typedef enum dt_drawlayer_paint_stroke_pos_t
{
  DT_DRAWLAYER_PAINT_STROKE_FIRST = 0,  /**< Button-press / stroke start marker. */
  DT_DRAWLAYER_PAINT_STROKE_MIDDLE = 1, /**< In-stroke motion sample. */
  DT_DRAWLAYER_PAINT_STROKE_END = 2,    /**< Button-release / stroke end marker. */
} dt_drawlayer_paint_stroke_pos_t;

/**
 * @brief One raw pointer event queued to stroke processing.
 *
 * All brush/user settings are snapshotted per event so asynchronous processing
 * does not depend on mutable GUI state.
 */
typedef struct dt_drawlayer_paint_raw_input_t
{
  float wx;               /**< Pointer X in widget coordinates. */
  float wy;               /**< Pointer Y in widget coordinates. */
  float pressure;         /**< Normalized pressure in [0,1]. */
  float tilt;             /**< Normalized tilt magnitude in [0,1]. */
  float acceleration;     /**< Normalized pointer acceleration in [0,1]. */
  gint64 event_ts;        /**< Monotonic event timestamp (microseconds). */
  uint32_t stroke_batch;  /**< Monotonic stroke id for the session. */
  uint32_t event_index;   /**< Monotonic index within the current stroke. */
  uint8_t stroke_pos;     /**< Value from @ref dt_drawlayer_paint_stroke_pos_t. */
  uint8_t pressure_profile; /**< Mapping profile enum for pressure modifiers. */
  uint8_t tilt_profile;     /**< Mapping profile enum for tilt modifiers. */
  uint8_t accel_profile;    /**< Mapping profile enum for acceleration modifiers. */
  uint32_t map_flags;     /**< Bitmask of active input-to-parameter mappings. */
  float distance_percent; /**< Sampling distance control in [0,1]. */
  float smoothing_percent;/**< Smoothing control in [0,1]. */
  float brush_radius;     /**< Base brush radius before dynamic mappings. */
  float brush_opacity;    /**< Base brush opacity before dynamic mappings. */
  float brush_flow;       /**< Base brush flow before dynamic mappings. */
  float brush_hardness;   /**< Base brush hardness before dynamic mappings. */
  float brush_sprinkles;  /**< Base sprinkles amount before dynamic mappings. */
  float brush_sprinkle_size; /**< Base sprinkle size in pixels. */
  float brush_sprinkle_coarseness; /**< Base sprinkle octave mix in [0,1]. */
  int brush_shape;        /**< Brush shape enum value, see brush.h. */
  int brush_mode;         /**< Brush mode enum value, see brush.h. */
  float color[3];         /**< Source color in pipeline space (RGB). */
  float display_color[3]; /**< Source color in display space (RGB). */
} dt_drawlayer_paint_raw_input_t;

/**
 * @brief Integer axis-aligned rectangle in buffer coordinates.
 *
 * Convention: `nw` is inclusive, `se` is exclusive.
 */
typedef struct dt_drawlayer_damaged_rect_t
{
  gboolean valid; /**< TRUE when rectangle contains meaningful coordinates. */
  int nw[2];      /**< North-west corner `[x,y]`, inclusive. */
  int se[2];      /**< South-east corner `[x,y]`, exclusive. */
} dt_drawlayer_damaged_rect_t;

/**
 * @brief Mutable stroke runtime state owned by worker/backend code.
 *
 * The same object carries path generation state and raster-time transient data.
 * It is reset at stroke boundaries by the worker lifecycle.
 */
typedef struct dt_drawlayer_paint_stroke_t
{
  GArray *history;      /**< Emitted, evenly-spaced dabs (`dt_drawlayer_brush_dab_t`). */
  GArray *raw_inputs;   /**< FIFO raw input queue (`dt_drawlayer_paint_raw_input_t`). */
  guint raw_input_cursor; /**< Cursor of next raw input to consume in `raw_inputs`. */
  GArray *dab_window;   /**< Rolling local raster window (`dt_drawlayer_brush_dab_t`, max ~3). */
  dt_drawlayer_brush_dab_t last_input_dab; /**< Last converted raw-input dab (pre-resampling). */
  gboolean have_last_input_dab;            /**< TRUE when `last_input_dab` is initialized. */
  dt_drawlayer_brush_dab_t prev_raw_dab;   /**< Previous raw segment anchor for cubic interpolation. */
  gboolean have_prev_raw_dab;              /**< TRUE when `prev_raw_dab` is initialized. */
  float stroke_arc_length;   /**< Cumulative raw-path arc length in layer coordinates. */
  float sampled_arc_length;  /**< Cumulative arc position of last emitted sample. */
  float distance_percent;    /**< Last applied distance control in [0,1]. */
  uint64_t stroke_seed;      /**< Deterministic per-stroke seed (noise/sprinkles). */
  dt_drawlayer_damaged_rect_t bounds; /**< Last dab footprint bounds in target buffer coordinates. */
  float *smudge_pixels;      /**< Smudge carry buffer (RGBA float, local dab footprint). */
  int smudge_width;          /**< Smudge carry buffer width in pixels. */
  int smudge_height;         /**< Smudge carry buffer height in pixels. */
  float smudge_pickup_x;     /**< Smudge pickup center X in layer coordinates. */
  float smudge_pickup_y;     /**< Smudge pickup center Y in layer coordinates. */
  gboolean have_smudge_pickup; /**< TRUE when smudge pickup coordinates are valid. */
} dt_drawlayer_paint_stroke_t;

/**
 * @brief Build a resolved dab from one raw input event.
 * @param user_data Opaque caller context.
 * @param state Mutable stroke runtime state.
 * @param input Raw input event.
 * @param out_dab Output resolved dab.
 * @return TRUE when dab creation succeeds.
 *
 * @pre Coordinate transforms and dynamic mapping policy are handled by callback implementation.
 */
typedef gboolean (*dt_drawlayer_paint_build_dab_cb)(void *user_data,
                                                     dt_drawlayer_paint_stroke_t *state,
                                                     const dt_drawlayer_paint_raw_input_t *input,
                                                     dt_drawlayer_brush_dab_t *out_dab);
/** @brief Convert layer-space coordinates back to widget-space (for HUD/preview alignment). */
typedef gboolean (*dt_drawlayer_paint_layer_to_widget_cb)(void *user_data, float lx, float ly,
                                                           float *wx, float *wy);
/** @brief Consume one emitted dab sample from path processing. */
typedef void (*dt_drawlayer_paint_emit_dab_cb)(void *user_data, const dt_drawlayer_brush_dab_t *dab);
/** @brief Notify caller when a new stroke seed is started. */
typedef void (*dt_drawlayer_paint_stroke_seed_cb)(void *user_data, uint64_t stroke_seed);

/** @brief Callback bundle used by stroke processing entry points. */
typedef struct dt_drawlayer_paint_callbacks_t
{
  dt_drawlayer_paint_build_dab_cb build_dab; /**< Mandatory: raw event -> resolved dab conversion. */
  dt_drawlayer_paint_layer_to_widget_cb layer_to_widget; /**< Optional: layer->widget transform callback. */
  dt_drawlayer_paint_emit_dab_cb emit_dab; /**< Mandatory: emitted dab sink. */
  dt_drawlayer_paint_stroke_seed_cb on_stroke_seed; /**< Optional: stroke-seed notification hook. */
} dt_drawlayer_paint_callbacks_t;

/** @brief Reset stroke path state and pending raw queue for a new stroke. */
void dt_drawlayer_paint_path_state_reset(dt_drawlayer_paint_stroke_t *state);
/** @brief Queue one raw input event (FIFO). */
gboolean dt_drawlayer_paint_queue_raw_input(dt_drawlayer_paint_stroke_t *state,
                                            const dt_drawlayer_paint_raw_input_t *input);
/**
 * @brief Drain queued raw input events and emit evenly spaced dabs.
 *
 * @note No coalescing is performed here. FIFO order is preserved.
 */
void dt_drawlayer_paint_raster_path(dt_drawlayer_paint_stroke_t *state,
                                    const dt_drawlayer_paint_callbacks_t *callbacks,
                                    void *user_data);
/** @brief Force emission of a pending initial dab when a stroke had no emitted samples yet. */
void dt_drawlayer_paint_finalize_path(dt_drawlayer_paint_stroke_t *state,
                                      const dt_drawlayer_paint_callbacks_t *callbacks,
                                      void *user_data);
/**
 * @brief Replay one emitted dab segment into a float buffer through brush API.
 *
 * @param dab Input dab sample from path stream.
 * @param distance_percent Sampling distance parameter in [0,1].
 * @param buffer Destination float RGBA buffer.
 * @param width Buffer width.
 * @param height Buffer height.
 * @param origin_x Layer-to-buffer X origin offset.
 * @param origin_y Layer-to-buffer Y origin offset.
 * @param scale Layer-to-buffer scale factor.
 * @param stroke_mask Optional stroke-local alpha mask.
 * @param stroke_mask_width Stroke mask width.
 * @param stroke_mask_height Stroke mask height.
 * @param runtime_state Accumulated stroke damage output.
 * @param runtime_private Mutable stroke runtime payload.
 * @return TRUE when dab replay succeeded.
 */
gboolean dt_drawlayer_paint_rasterize_segment_to_buffer(const dt_drawlayer_brush_dab_t *dab,
                                                        float distance_percent,
                                                        float *buffer,
                                                        int width,
                                                        int height,
                                                        int origin_x,
                                                        int origin_y,
                                                        float scale,
                                                        float *stroke_mask,
                                                        int stroke_mask_width,
                                                        int stroke_mask_height,
                                                        dt_drawlayer_damaged_rect_t *runtime_state,
                                                        dt_drawlayer_paint_stroke_t *runtime_private);

/** @brief Allocate zero-initialized stroke-damage accumulator state. */
dt_drawlayer_damaged_rect_t *dt_drawlayer_paint_runtime_state_create(void);
/** @brief Destroy stroke-damage accumulator state and null pointer. */
void dt_drawlayer_paint_runtime_state_destroy(dt_drawlayer_damaged_rect_t **state);
/** @brief Reset stroke-damage accumulator to empty/invalid. */
void dt_drawlayer_paint_runtime_state_reset(dt_drawlayer_damaged_rect_t *state);

/** @brief Allocate stroke runtime payload object used by paint+brush internals. */
dt_drawlayer_paint_stroke_t *dt_drawlayer_paint_runtime_private_create(void);
/** @brief Destroy stroke runtime payload and null pointer. */
void dt_drawlayer_paint_runtime_private_destroy(dt_drawlayer_paint_stroke_t **state);
/** @brief Reset transient stroke runtime payload between strokes. */
void dt_drawlayer_paint_runtime_private_reset(dt_drawlayer_paint_stroke_t *state);
/** @brief Set deterministic stroke seed for noise-derived effects. */
void dt_drawlayer_paint_runtime_set_stroke_seed(dt_drawlayer_paint_stroke_t *state, uint64_t seed);
/** @brief Get current deterministic stroke seed. */
uint64_t dt_drawlayer_paint_runtime_get_stroke_seed(const dt_drawlayer_paint_stroke_t *state);
/** @brief Ensure smudge carry buffer allocation for given footprint dimensions. */
gboolean dt_drawlayer_paint_runtime_ensure_smudge_pixels(dt_drawlayer_paint_stroke_t *state,
                                                         int width, int height);
/** @brief Get smudge carry buffer pointer (RGBA float). */
float *dt_drawlayer_paint_runtime_smudge_pixels(dt_drawlayer_paint_stroke_t *state);
/** @brief Get smudge carry buffer width. */
int dt_drawlayer_paint_runtime_smudge_width(const dt_drawlayer_paint_stroke_t *state);
/** @brief Get smudge carry buffer height. */
int dt_drawlayer_paint_runtime_smudge_height(const dt_drawlayer_paint_stroke_t *state);
/** @brief Query whether smudge pickup coordinates are initialized. */
gboolean dt_drawlayer_paint_runtime_have_smudge_pickup(const dt_drawlayer_paint_stroke_t *state);
/** @brief Read smudge pickup coordinates. */
void dt_drawlayer_paint_runtime_get_smudge_pickup(const dt_drawlayer_paint_stroke_t *state,
                                                  float *x, float *y);
/** @brief Write smudge pickup coordinates and validity flag. */
void dt_drawlayer_paint_runtime_set_smudge_pickup(dt_drawlayer_paint_stroke_t *state,
                                                  float x, float y, gboolean have_pickup);
/** @brief Compute current dab footprint bounds in target buffer coordinates. */
gboolean dt_drawlayer_paint_runtime_prepare_dab_context(dt_drawlayer_paint_stroke_t *state,
                                                         const dt_drawlayer_brush_dab_t *dab,
                                                         int width, int height,
                                                         int origin_x, int origin_y,
                                                         float scale);
/** @brief Merge one dab rectangle into an accumulator rectangle. */
void dt_drawlayer_paint_runtime_note_dab_damage(dt_drawlayer_damaged_rect_t *state,
                                                const dt_drawlayer_damaged_rect_t *dab_rect);
/** @brief Read accumulated stroke damage rectangle. */
gboolean dt_drawlayer_paint_runtime_get_stroke_damage(const dt_drawlayer_damaged_rect_t *state,
                                                      dt_drawlayer_damaged_rect_t *out_rect);
/**
 * @brief Merge path-state damage into target rectangle and clear path-state accumulator.
 * @return TRUE when a valid rectangle was merged.
 */
gboolean dt_drawlayer_paint_merge_runtime_stroke_damage(dt_drawlayer_damaged_rect_t *path_state,
                                                        dt_drawlayer_damaged_rect_t *target_rect);
