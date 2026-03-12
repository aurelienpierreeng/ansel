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

#include <glib.h>
#include <stdint.h>

#ifndef OUTER_LOOP
#define OUTER_LOOP 1
#endif

typedef struct dt_drawlayer_cache_patch_t dt_drawlayer_cache_patch_t;

/**
 * @file brush.h
 * @brief Dab-level brush rasterization API for drawlayer.
 *
 * This API is intentionally independent from drawlayer module internals.
 * Callers provide fully resolved dab inputs in target buffer coordinates.
 */

/** @brief Supported analytic fall-off profiles for brush alpha. */
typedef enum dt_drawlayer_brush_shape_t
{
  DT_DRAWLAYER_BRUSH_SHAPE_LINEAR = 0,    /**< Linear ramp from inner to outer radius. */
  DT_DRAWLAYER_BRUSH_SHAPE_GAUSSIAN = 1,  /**< Smooth bell-like fall-off. */
  DT_DRAWLAYER_BRUSH_SHAPE_QUADRATIC = 2, /**< Stronger center emphasis than linear. */
  DT_DRAWLAYER_BRUSH_SHAPE_SIGMOIDAL = 3  /**< S-curve transition near edge. */
} dt_drawlayer_brush_shape_t;

/** @brief Pixel blending behavior used while stamping a dab. */
typedef enum dt_drawlayer_brush_mode_t
{
  DT_DRAWLAYER_BRUSH_MODE_PAINT = 0,  /**< Standard source-over paint in premultiplied space. */
  DT_DRAWLAYER_BRUSH_MODE_ERASE = 1,  /**< Alpha attenuation / erase mode. */
  DT_DRAWLAYER_BRUSH_MODE_BLUR = 2,   /**< Local blur gather then blend. */
  DT_DRAWLAYER_BRUSH_MODE_SMUDGE = 3  /**< Carry-and-deposit color pickup mode. */
} dt_drawlayer_brush_mode_t;

typedef dt_drawlayer_brush_shape_t dt_iop_drawlayer_brush_shape_t;
typedef dt_drawlayer_brush_mode_t dt_iop_drawlayer_brush_mode_t;

/**
 * @brief Fully resolved input dab descriptor.
 *
 * Instances are produced by the stroke-level paint API (or equivalent caller
 * code) and consumed by the brush rasterizer.
 */
typedef struct dt_drawlayer_brush_dab_t
{
  float x;                /**< Dab center X in layer/buffer-space pixels. */
  float y;                /**< Dab center Y in layer/buffer-space pixels. */
  float wx;               /**< Dab center X in widget-space coordinates (for GUI overlays). */
  float wy;               /**< Dab center Y in widget-space coordinates (for GUI overlays). */
  float radius;           /**< Dab radius in layer-space pixels (>0.0f expected). */
  float dir_x;            /**< Unit direction X along local stroke tangent (or 0 when unknown). */
  float dir_y;            /**< Unit direction Y along local stroke tangent (or 1 when unknown). */
  float sample_spacing;   /**< Emission spacing chosen by the stroke sampler for this dab. */
  float sample_opacity_scale; /**< Precomputed spacing normalization consumed by rasterization. */
  float opacity;          /**< Per-dab target opacity in [0,1] after input mapping. */
  float flow;             /**< Per-dab flow in [0,1] after input mapping (API-level convention). */
  float sprinkles;        /**< Sprinkle/noise amount in [0,1]. */
  float sprinkle_size;    /**< Sprinkle pattern spatial scale in pixels. */
  float sprinkle_coarseness; /**< Octave mix control in [0,1] from fine (0) to coarse (1). */
  float hardness;         /**< Hardness/inner support control in [0,1]. */
  float color[4];         /**< Premultiplied-space source RGBA, alpha conventionally set to 1. */
  float display_color[3]; /**< Display-space RGB counterpart for GUI preview widgets. */
  int shape;              /**< One of @ref dt_drawlayer_brush_shape_t. */
  int mode;               /**< One of @ref dt_drawlayer_brush_mode_t. */
  uint32_t stroke_batch;  /**< Monotonic stroke id used for deterministic per-stroke noise seeding. */
  uint8_t stroke_pos;     /**< Stroke position tag (first/middle/end), see paint API enum. */
} dt_drawlayer_brush_dab_t;

struct dt_drawlayer_paint_stroke_t;

/**
 * @brief Rasterize one dab into a float RGBA buffer.
 *
 * @param patch Destination premultiplied RGBA float patch.
 * @param scale Layer-to-buffer scale factor, must be >0.
 * @param dab Fully resolved dab input (coordinates already in layer space).
 * @param sample_opacity_scale Opacity normalization factor derived from spacing.
 * @param stroke_mask Optional stroke-local alpha mask patch for overlap-aware flow.
 * @param stroke Mutable stroke runtime payload (smudge context + dab bounds output).
 * @return TRUE when at least one pixel was processed; FALSE on invalid inputs or empty footprint.
 *
 * @pre `dab->radius > 0`, `dab->opacity > 0`, `scale > 0`.
 * @pre Coordinates transformation to layer space is handled by the caller.
 */
gboolean dt_drawlayer_brush_rasterize(dt_drawlayer_cache_patch_t *patch, float scale,
                                      const dt_drawlayer_brush_dab_t *dab,
                                      float sample_opacity_scale,
                                      dt_drawlayer_cache_patch_t *stroke_mask,
                                      struct dt_drawlayer_paint_stroke_t *stroke);

/**
 * @brief Rasterize a single dab preview into ARGB8 for GUI overlays.
 *
 * @param dab Input dab descriptor.
 * @param argb Destination 8-bit ARGB surface memory.
 * @param width Surface width.
 * @param height Surface height.
 * @param stride Surface stride in bytes.
 * @param center_x Dab center X in surface coordinates.
 * @param center_y Dab center Y in surface coordinates.
 * @param opacity_multiplier Additional UI-time opacity scalar in [0,1].
 * @return TRUE on success, FALSE on invalid inputs.
 *
 * @note This path is for cursor/post-expose rendering only and does not update
 * stroke runtime state.
 */
gboolean dt_drawlayer_brush_rasterize_dab_argb8(const dt_drawlayer_brush_dab_t *dab, uint8_t *argb,
                                                 int width, int height, int stride,
                                                 float center_x, float center_y,
                                                 float opacity_multiplier);

/**
 * @brief Rasterize a single dab preview in linear float RGBA over an opaque background.
 *
 * @param dab Input dab descriptor.
 * @param rgba Destination float RGBA buffer (`width*height*4`), filled in linear display RGB.
 * @param width Surface width.
 * @param height Surface height.
 * @param center_x Dab center X in surface coordinates.
 * @param center_y Dab center Y in surface coordinates.
 * @param opacity_multiplier Additional UI-time opacity scalar in [0,1].
 * @param background_rgb Opaque linear RGB background color, or NULL for white.
 * @return TRUE on success, FALSE on invalid inputs.
 */
gboolean dt_drawlayer_brush_rasterize_dab_rgbaf(const dt_drawlayer_brush_dab_t *dab, float *rgba,
                                                 int width, int height,
                                                 float center_x, float center_y,
                                                 float opacity_multiplier,
                                                 const float background_rgb[3]);
