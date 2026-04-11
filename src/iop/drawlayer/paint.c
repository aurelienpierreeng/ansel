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
 * drawlayer public paint API implementation
 *
 * Stroke-level processing:
 * raw input events -> smoothed/resampled dab stream.
 */

#include "iop/drawlayer/paint.h"
#include "iop/drawlayer/cache.h"
#include "iop/drawlayer/brush_profile.h"

#include "common/darktable.h"
#include "common/math.h"
#include "develop/noise_generator.h"

#include <math.h>
#include <string.h>

/** @file
 *  @brief Stroke-level path processing and raster dispatch for drawlayer.
 */

/** @brief Clamp scalar value to [0, 1]. */
static inline float _clamp01(const float v)
{
  return fminf(fmaxf(v, 0.0f), 1.0f);
}

/** @brief Linear interpolation helper. */
static inline float _lerpf(const float a, const float b, const float t)
{
  return a + (b - a) * t;
}

/** @brief Compute angular measure used by strip-based profile integration. */
static inline float _paint_voronoi_strip_angle_measure(const float rho, const float strip_ratio)
{
  if(strip_ratio <= 0.0f) return 0.0f;
  if(rho <= strip_ratio + 1e-6f) return 2.0f * (float)G_PI;
  return 4.0f * asinf(_clamp01(strip_ratio / fmaxf(rho, 1e-6f)));
}

/** @brief Resolve dab-to-dab center spacing from radius and distance percentage. */
static inline float _paint_dab_sample_spacing(const dt_drawlayer_brush_dab_t *dab, const float distance_percent)
{
  if(!dab) return 1.0f;
  const float radius = fmaxf(0.5f, dab->radius);
  const float diameter = 2.0f * radius;
  return _lerpf(1.0f, diameter, _clamp01(distance_percent));
}

/** @brief Resolve one segment spacing target from edge dab radii. */
static inline float _paint_segment_sample_spacing(const dt_drawlayer_brush_dab_t *dabs, const int count,
                                                  const float distance_percent)
{
  if(count <= 0) return 1.0f;
  if(count == 1) return _paint_dab_sample_spacing(&dabs[0], distance_percent);
  const dt_drawlayer_brush_dab_t *p_start = &dabs[count - 2];
  const dt_drawlayer_brush_dab_t *p_end = &dabs[count - 1];
  const float min_radius = fmaxf(0.5f, fminf(p_start->radius, p_end->radius));
  const dt_drawlayer_brush_dab_t tmp = { .radius = min_radius };
  return _paint_dab_sample_spacing(&tmp, distance_percent);
}

/** @brief Cubic Hermite scalar interpolation helper. */
static inline float _paint_cubic_hermitef(const float p0, const float p1, const float m0, const float m1, const float t)
{
  const float t2 = t * t;
  const float t3 = t2 * t;
  return (2.0f * t3 - 3.0f * t2 + 1.0f) * p0 + (t3 - 2.0f * t2 + t) * m0
         + (-2.0f * t3 + 3.0f * t2) * p1 + (t3 - t2) * m1;
}

/**
 * @brief Build one interpolated dab sample in the current segment window.
 * @details Interpolates full dab properties, not just coordinates.
 */
static dt_drawlayer_brush_dab_t _paint_build_segment_window_sample(const dt_drawlayer_brush_dab_t *dabs,
                                                                    const int count, const float t)
{
  const dt_drawlayer_brush_dab_t *p_prev = (count >= 3) ? &dabs[count - 3] : &dabs[count - 2];
  const dt_drawlayer_brush_dab_t *p_start = &dabs[count - 2];
  const dt_drawlayer_brush_dab_t *p_end = &dabs[count - 1];

  const float seg_dx = p_end->x - p_start->x;
  const float seg_dy = p_end->y - p_start->y;
  const float seg_len = hypotf(seg_dx, seg_dy);
  const float dir_x = (seg_len > 1e-6f) ? (seg_dx / seg_len) : p_start->dir_x;
  const float dir_y = (seg_len > 1e-6f) ? (seg_dy / seg_len) : p_start->dir_y;
  const float m1x = (count >= 3) ? 0.5f * (p_end->x - p_prev->x) : (p_end->x - p_start->x);
  const float m1y = (count >= 3) ? 0.5f * (p_end->y - p_prev->y) : (p_end->y - p_start->y);
  const float m2x = p_end->x - p_start->x;
  const float m2y = p_end->y - p_start->y;
  const dt_aligned_pixel_simd_t t4 = dt_simd_set1(t);
  const dt_aligned_pixel_simd_t color_start = dt_load_simd(p_start->color);
  const dt_aligned_pixel_simd_t color_end = dt_load_simd(p_end->color);

  dt_drawlayer_brush_dab_t dab = {
    .x = _paint_cubic_hermitef(p_start->x, p_end->x, m1x, m2x, t),
    .y = _paint_cubic_hermitef(p_start->y, p_end->y, m1y, m2y, t),
    .wx = _lerpf(p_start->wx, p_end->wx, t),
    .wy = _lerpf(p_start->wy, p_end->wy, t),
    .radius = fmaxf(0.5f, _lerpf(p_start->radius, p_end->radius, t)),
    .dir_x = dir_x,
    .dir_y = dir_y,
    .opacity = _clamp01(_lerpf(p_start->opacity, p_end->opacity, t)),
    .flow = _clamp01(_lerpf(p_start->flow, p_end->flow, t)),
    .sprinkles = _clamp01(_lerpf(p_start->sprinkles, p_end->sprinkles, t)),
    .sprinkle_size = fmaxf(0.0f, _lerpf(p_start->sprinkle_size, p_end->sprinkle_size, t)),
    .sprinkle_coarseness = _clamp01(_lerpf(p_start->sprinkle_coarseness, p_end->sprinkle_coarseness, t)),
    .hardness = _clamp01(_lerpf(p_start->hardness, p_end->hardness, t)),
    .color = { 0.0f, 0.0f, 0.0f, 0.0f },
    .display_color = {
      _lerpf(p_start->display_color[0], p_end->display_color[0], t),
      _lerpf(p_start->display_color[1], p_end->display_color[1], t),
      _lerpf(p_start->display_color[2], p_end->display_color[2], t),
    },
    .shape = (t < 0.5f) ? p_start->shape : p_end->shape,
    .mode = (t < 0.5f) ? p_start->mode : p_end->mode,
  };
  dt_store_simd(dab.color, color_start + (color_end - color_start) * t4);
  return dab;
}

/**
 * @brief Compute per-sample opacity normalization from spacing.
 * @note Result is a [0,1] scale factor consumed by brush flow logic.
 */
static inline float _paint_stroke_sample_opacity_scale(const dt_drawlayer_brush_dab_t *dab,
                                                       const float sample_step)
{
  /* Estimate the fraction of a dab support area covered by one spacing strip.
   * This is used by brush flow logic to normalize per-sample opacity at
   * different sampling distances. */
  if(!dab || !isfinite(sample_step)) return 1.0f;
  const float support_radius = fmaxf(dab->radius, 0.5f);
  const float overlap_span = 2.0f * support_radius;
  if(sample_step <= 1e-6f || overlap_span <= 1e-6f || sample_step >= overlap_span - 1e-6f) return 1.0f;

  const float half_strip = 0.5f * sample_step;
  if(dab->shape != DT_DRAWLAYER_BRUSH_SHAPE_GAUSSIAN && _clamp01(dab->hardness) >= 1.0f - 1e-6f)
  {
    const float clamped_half_strip = fminf(half_strip, support_radius);
    const float chord_half = sqrtf(fmaxf(support_radius * support_radius
                                         - clamped_half_strip * clamped_half_strip, 0.0f));
    const float strip_area = sample_step * chord_half
                             + 2.0f * support_radius * support_radius
                                   * asinf(_clamp01(clamped_half_strip / support_radius));
    const float full_area = (float)G_PI * support_radius * support_radius;
    return _clamp01(strip_area / fmaxf(full_area, 1e-6f));
  }

  const float strip_ratio = _clamp01(half_strip / support_radius);
  const float full_mass = 2.0f * (float)G_PI * dt_drawlayer_brush_mass_primitive_eval(dab, 1.0f);
  if(!isfinite(full_mass) || full_mass <= 1e-6f) return 1.0f;

  const float weight_samples = 32.f;
  const float dr = 1.0f / weight_samples;
  float strip_mass = 0.0f;
  for(int ir = 0; ir < weight_samples; ir++)
  {
    const float rho = ((float)ir + 0.5f) * dr;
    const float profile = dt_drawlayer_brush_profile_eval(dab, rho * rho);
    if(!isfinite(profile) || profile <= 0.0f) continue;
    const float angle = _paint_voronoi_strip_angle_measure(rho, strip_ratio);
    if(!isfinite(angle) || angle <= 0.0f) continue;
    strip_mass += angle * profile * rho * dr;
  }
  const float scale = strip_mass / full_mass;
  return isfinite(scale) ? _clamp01(scale) : 1.0f;
}

/** @brief Lazily allocate raw-input queue storage for one stroke state. */
static gboolean _ensure_raw_inputs(dt_drawlayer_paint_stroke_t *state)
{
  /* Lazily allocate queue storage so idle modules keep a tiny footprint. */
  if(!state) return FALSE;
  if(state->raw_inputs) return TRUE;
  state->raw_inputs = g_array_new(FALSE, FALSE, sizeof(dt_drawlayer_paint_raw_input_t));
  return !IS_NULL_PTR(state->raw_inputs);
}

/** @brief Lazily allocate pending-dab batch storage for one stroke state. */
static gboolean _ensure_pending_dabs(dt_drawlayer_paint_stroke_t *state)
{
  if(!state) return FALSE;
  if(state->pending_dabs) return TRUE;
  state->pending_dabs = g_array_new(FALSE, FALSE, sizeof(dt_drawlayer_brush_dab_t));
  return !IS_NULL_PTR(state->pending_dabs);
}

/** @brief Sample the current raw segment at parametric position `t`. */
static dt_drawlayer_brush_dab_t _sample_raw_segment_cubic_param(const dt_drawlayer_paint_stroke_t *state,
                                                                const dt_drawlayer_brush_dab_t *segment_start,
                                                                const dt_drawlayer_brush_dab_t *segment_end,
                                                                const float t)
{
  dt_drawlayer_brush_dab_t window[3] = { 0 };
  int count = 2;
  if(state && state->have_prev_raw_dab)
  {
    window[0] = state->prev_raw_dab;
    window[1] = *segment_start;
    window[2] = *segment_end;
    count = 3;
  }
  else
  {
    window[0] = *segment_start;
    window[1] = *segment_end;
  }
  return _paint_build_segment_window_sample(window, count, _clamp01(t));
}

/**
 * @brief Build arc-length lookup for the current cubic segment.
 * @pre `cumulative` must contain `segments + 1` items.
 */
static void _build_raw_segment_cubic_arclen_lut(const dt_drawlayer_paint_stroke_t *state,
                                                const dt_drawlayer_brush_dab_t *segment_start,
                                                const dt_drawlayer_brush_dab_t *segment_end,
                                                float *cumulative, const int segments, float *total_len)
{
  /* Build a short arc-length LUT for cubic interpolation, then sample by
   * distance instead of parameter t to keep spacing uniform at high speed. */
  if(!cumulative || segments <= 0 || !total_len) return;
  cumulative[0] = 0.0f;
  *total_len = 0.0f;
  dt_drawlayer_brush_dab_t prev = _sample_raw_segment_cubic_param(state, segment_start, segment_end, 0.0f);
  for(int i = 1; i <= segments; i++)
  {
    const float t = (float)i / (float)segments;
    const dt_drawlayer_brush_dab_t cur = _sample_raw_segment_cubic_param(state, segment_start, segment_end, t);
    *total_len += hypotf(cur.x - prev.x, cur.y - prev.y);
    cumulative[i] = *total_len;
    prev = cur;
  }
}

/** @brief Sample a cubic segment at normalized arc length using LUT inversion. */
static dt_drawlayer_brush_dab_t _sample_raw_segment_cubic_arclen(const dt_drawlayer_paint_stroke_t *state,
                                                                 const dt_drawlayer_brush_dab_t *segment_start,
                                                                 const dt_drawlayer_brush_dab_t *segment_end,
                                                                 const float target_norm,
                                                                 const float *cumulative,
                                                                 const int segments,
                                                                 const float total_len)
{
  /* Invert normalized arc length against the LUT (piecewise linear in t). */
  if(!cumulative || segments <= 0 || total_len <= 1e-6f)
    return _sample_raw_segment_cubic_param(state, segment_start, segment_end, target_norm);

  const float target_len = _clamp01(target_norm) * total_len;
  int k = 0;
  while(k < segments && cumulative[k + 1] < target_len) k++;

  const float l0 = cumulative[k];
  const float l1 = cumulative[MIN(k + 1, segments)];
  const float span = fmaxf(l1 - l0, 1e-6f);
  const float local = _clamp01((target_len - l0) / span);
  const float t0 = (float)k / (float)segments;
  const float t1 = (float)MIN(k + 1, segments) / (float)segments;
  return _sample_raw_segment_cubic_param(state, segment_start, segment_end, _lerpf(t0, t1, local));
}

/**
 * @brief Apply optional quadratic smoothing to one emitted dab.
 * @details Blends prediction with real position and properties.
 */
static void _apply_quadratic_dab_smoothing(dt_drawlayer_paint_stroke_t *state,
                                           dt_drawlayer_brush_dab_t *dab,
                                           const float sample_spacing,
                                           const float smoothing_percent,
                                           const dt_drawlayer_paint_layer_to_widget_cb layer_to_widget,
                                           void *user_data)
{
  if(!state || !dab || smoothing_percent <= 0.0f) return;
  if(!state->history || state->history->len < 3) return;
  const float real_x = dab->x;
  const float real_y = dab->y;

  const dt_drawlayer_brush_dab_t *h = (const dt_drawlayer_brush_dab_t *)state->history->data;
  const int n = (int)state->history->len;
  const dt_drawlayer_brush_dab_t *p0 = &h[n - 3];
  const dt_drawlayer_brush_dab_t *p1 = &h[n - 2];
  const dt_drawlayer_brush_dab_t *p2 = &h[n - 1];
  const float real_radius = dab->radius;
  const float real_opacity = dab->opacity;
  const float real_flow = dab->flow;
  const float real_hardness = dab->hardness;
  const float real_sprinkles = dab->sprinkles;
  const float real_sprinkle_size = dab->sprinkle_size;
  const float real_sprinkle_coarseness = dab->sprinkle_coarseness;
  const float real_color[4] = { dab->color[0], dab->color[1], dab->color[2], dab->color[3] };
  const float real_display_color[3] = { dab->display_color[0], dab->display_color[1], dab->display_color[2] };

  /* Quadratic extrapolation from previous 3 dabs, then enforce exactly one
   * sample-spacing ahead from p2 so smoothing honors the sampling constraint. */
  const float qx = 3.0f * p2->x - 3.0f * p1->x + p0->x;
  const float qy = 3.0f * p2->y - 3.0f * p1->y + p0->y;
  float dvx = qx - p2->x;
  float dvy = qy - p2->y;
  float dlen = hypotf(dvx, dvy);
  if(dlen <= 1e-6f)
  {
    dvx = real_x - p2->x;
    dvy = real_y - p2->y;
    dlen = hypotf(dvx, dvy);
  }
  const float step = fmaxf(sample_spacing, 1e-6f);
  const float pred_x = (dlen > 1e-6f) ? (p2->x + dvx * (step / dlen)) : real_x;
  const float pred_y = (dlen > 1e-6f) ? (p2->y + dvy * (step / dlen)) : real_y;
  const float blend = 0.5f * _clamp01(smoothing_percent);
  const float pred_radius = fmaxf(0.5f, 3.0f * p2->radius - 3.0f * p1->radius + p0->radius);
  const float pred_opacity = _clamp01(3.0f * p2->opacity - 3.0f * p1->opacity + p0->opacity);
  const float pred_flow = _clamp01(3.0f * p2->flow - 3.0f * p1->flow + p0->flow);
  const float pred_hardness = _clamp01(3.0f * p2->hardness - 3.0f * p1->hardness + p0->hardness);
  const float pred_sprinkles = _clamp01(3.0f * p2->sprinkles - 3.0f * p1->sprinkles + p0->sprinkles);
  const float pred_sprinkle_size = fmaxf(0.0f, 3.0f * p2->sprinkle_size - 3.0f * p1->sprinkle_size + p0->sprinkle_size);
  const float pred_sprinkle_coarseness
      = _clamp01(3.0f * p2->sprinkle_coarseness - 3.0f * p1->sprinkle_coarseness + p0->sprinkle_coarseness);
  dab->x = _lerpf(dab->x, pred_x, blend);
  dab->y = _lerpf(dab->y, pred_y, blend);
  dab->radius = _lerpf(real_radius, pred_radius, blend);
  dab->opacity = _lerpf(real_opacity, pred_opacity, blend);
  dab->flow = _lerpf(real_flow, pred_flow, blend);
  dab->hardness = _lerpf(real_hardness, pred_hardness, blend);
  dab->sprinkles = _lerpf(real_sprinkles, pred_sprinkles, blend);
  dab->sprinkle_size = _lerpf(real_sprinkle_size, pred_sprinkle_size, blend);
  dab->sprinkle_coarseness = _lerpf(real_sprinkle_coarseness, pred_sprinkle_coarseness, blend);
  dab->color[0] = _lerpf(real_color[0], 3.0f * p2->color[0] - 3.0f * p1->color[0] + p0->color[0], blend);
  dab->color[1] = _lerpf(real_color[1], 3.0f * p2->color[1] - 3.0f * p1->color[1] + p0->color[1], blend);
  dab->color[2] = _lerpf(real_color[2], 3.0f * p2->color[2] - 3.0f * p1->color[2] + p0->color[2], blend);
  dab->color[3] = _lerpf(real_color[3], 3.0f * p2->color[3] - 3.0f * p1->color[3] + p0->color[3], blend);
  dab->display_color[0]
      = _lerpf(real_display_color[0], 3.0f * p2->display_color[0] - 3.0f * p1->display_color[0] + p0->display_color[0], blend);
  dab->display_color[1]
      = _lerpf(real_display_color[1], 3.0f * p2->display_color[1] - 3.0f * p1->display_color[1] + p0->display_color[1], blend);
  dab->display_color[2]
      = _lerpf(real_display_color[2], 3.0f * p2->display_color[2] - 3.0f * p1->display_color[2] + p0->display_color[2], blend);

  const dt_drawlayer_brush_dab_t *prev = &h[n - 1];
  const float rvx = real_x - prev->x;
  const float rvy = real_y - prev->y;
  const float svx = dab->x - prev->x;
  const float svy = dab->y - prev->y;
  const float real_dist = hypotf(rvx, rvy);
  const float smooth_dist = hypotf(svx, svy);
  const float min_safe = 0.5f * fmaxf(sample_spacing, 1e-6f);
  const float dot = rvx * svx + rvy * svy;
  if((smooth_dist < min_safe && real_dist > smooth_dist) || dot <= 0.0f)
  {
    dab->x = real_x;
    dab->y = real_y;
  }

  if(layer_to_widget) layer_to_widget(user_data, dab->x, dab->y, &dab->wx, &dab->wy);
}

/** @brief Emit one dab and append it to emitted-history tracking. */
static void _emit_dab(dt_drawlayer_paint_stroke_t *state, dt_drawlayer_brush_dab_t *dab)
{
  /* Emit one dab and keep a full emitted history for smoothing/prediction. */
  if(!state || !dab || !state->history || !state->pending_dabs) return;
  if(state->history && state->history->len > 0)
  {
    const dt_drawlayer_brush_dab_t *prev
        = &g_array_index(state->history, dt_drawlayer_brush_dab_t, state->history->len - 1);
    const float dx = dab->x - prev->x;
    const float dy = dab->y - prev->y;
    const float len = hypotf(dx, dy);
    if(len > 1e-6f)
    {
      dab->dir_x = dx / len;
      dab->dir_y = dy / len;
    }
  }
  g_array_append_val(state->history, *dab);
  g_array_append_val(state->pending_dabs, *dab);
}

/** @brief Freeze raster-time normalization into one emitted dab record. */
static inline void _freeze_emitted_dab_raster_state(dt_drawlayer_brush_dab_t *dab, const float sample_spacing)
{
  if(!dab) return;
  dab->sample_spacing = fmaxf(sample_spacing, 1e-6f);
  dab->sample_opacity_scale = _paint_stroke_sample_opacity_scale(dab, dab->sample_spacing);
}

/** @brief Re-project current dab center to exact target spacing from previous dab. */
static void _enforce_dab_center_spacing(dt_drawlayer_paint_stroke_t *state,
                                        dt_drawlayer_brush_dab_t *dab,
                                        const float sample_spacing,
                                        const dt_drawlayer_paint_layer_to_widget_cb layer_to_widget,
                                        void *user_data)
{
  if(!state || !dab || !state->history || state->history->len == 0) return;
  const dt_drawlayer_brush_dab_t *prev
      = &g_array_index(state->history, dt_drawlayer_brush_dab_t, state->history->len - 1);

  const float target = fmaxf(sample_spacing, 1e-6f);
  float dx = dab->x - prev->x;
  float dy = dab->y - prev->y;
  float d = hypotf(dx, dy);
  if(!(d > 1e-6f))
  {
    float dir_x = dab->dir_x;
    float dir_y = dab->dir_y;
    float dir_len = hypotf(dir_x, dir_y);
    if(dir_len <= 1e-6f)
    {
      dir_x = prev->dir_x;
      dir_y = prev->dir_y;
      dir_len = hypotf(dir_x, dir_y);
    }
    if(dir_len <= 1e-6f)
    {
      dir_x = 1.0f;
      dir_y = 0.0f;
      dir_len = 1.0f;
    }
    dx = dir_x / dir_len;
    dy = dir_y / dir_len;
  }
  else
  {
    dx /= d;
    dy /= d;
  }

  dab->x = prev->x + dx * target;
  dab->y = prev->y + dy * target;
  dab->dir_x = dx;
  dab->dir_y = dy;
  if(layer_to_widget) layer_to_widget(user_data, dab->x, dab->y, &dab->wx, &dab->wy);
}

/** @brief Reset only path-generation state while keeping reusable allocations. */
static void _paint_reset_path_runtime_state(dt_drawlayer_paint_stroke_t *state)
{
  /* Reset only per-stroke path generation state.
   * Queue storage and reusable allocations are kept by the owner. */
  if(!state) return;
  if(state->history) g_array_set_size(state->history, 0);
  if(state->pending_dabs) g_array_set_size(state->pending_dabs, 0);
  if(state->dab_window) g_array_set_size(state->dab_window, 0);
  state->last_input_dab = (dt_drawlayer_brush_dab_t){ 0 };
  state->have_last_input_dab = FALSE;
  state->prev_raw_dab = (dt_drawlayer_brush_dab_t){ 0 };
  state->have_prev_raw_dab = FALSE;
  state->stroke_arc_length = 0.0f;
  state->sampled_arc_length = 0.0f;
  state->distance_percent = 0.0f;
  state->stroke_seed = 0u;
}

/** @brief Reset full stroke state including queued raw input events. */
void dt_drawlayer_paint_path_state_reset(dt_drawlayer_paint_stroke_t *state)
{
  /* Full stroke reset: pending raw events + generated path state. */
  if(!state) return;
  if(state->raw_inputs) g_array_set_size(state->raw_inputs, 0);
  state->raw_input_cursor = 0;
  _paint_reset_path_runtime_state(state);
}

/** @brief Test if current input denotes a new stroke boundary. */
static gboolean _paint_input_starts_new_stroke(const dt_drawlayer_paint_stroke_t *state,
                                                const dt_drawlayer_paint_raw_input_t *input)
{
  if(!state || !input) return FALSE;
  return input->stroke_pos == DT_DRAWLAYER_PAINT_STROKE_FIRST
         || (state->have_last_input_dab && input->stroke_batch != 0u
             && state->last_input_dab.stroke_batch != 0u
             && input->stroke_batch != state->last_input_dab.stroke_batch);
}

/** @brief Build deterministic stroke seed from batch/time/coordinates. */
static uint64_t _paint_make_stroke_seed(const dt_drawlayer_paint_raw_input_t *input)
{
  if(!input) return 0u;
  const uint64_t qx = (uint64_t)(int64_t)llrintf((double)input->wx * 256.0);
  const uint64_t qy = (uint64_t)(int64_t)llrintf((double)input->wy * 256.0);
  return ((uint64_t)input->stroke_batch << 32)
         ^ (uint64_t)input->event_ts
         ^ (qx * 0x9e3779b185ebca87ull)
         ^ (qy * 0xc2b2ae3d27d4eb4full);
}

/** @brief Optionally emit first sample immediately when stroke starts. */
static void _emit_first_sample_if_needed(dt_drawlayer_paint_stroke_t *state,
                                         const dt_drawlayer_brush_dab_t *dab)
{
  if(!state || !dab || !state->history || !_ensure_pending_dabs(state)) return;
  state->last_input_dab = *dab;
  state->have_last_input_dab = TRUE;

  dt_drawlayer_brush_dab_t first = *dab;
  first.stroke_pos = DT_DRAWLAYER_PAINT_STROKE_FIRST;
  _freeze_emitted_dab_raster_state(&first, _paint_dab_sample_spacing(&first, _clamp01(state->distance_percent)));
  _emit_dab(state, &first);
  state->sampled_arc_length = 0.0f;
}

/**
 * @brief Finalize stroke by force-emitting the pending first sample if needed.
 * @note Used for very short strokes that collected no middle sample.
 */
void dt_drawlayer_paint_finalize_path(dt_drawlayer_paint_stroke_t *state)
{
  if(!state || !state->have_last_input_dab || !state->history || !_ensure_pending_dabs(state)) return;
  if(!state->history || state->history->len > 0) return;

  dt_drawlayer_brush_dab_t dab = state->last_input_dab;
  dab.stroke_pos = DT_DRAWLAYER_PAINT_STROKE_FIRST;
  _freeze_emitted_dab_raster_state(&dab, _paint_dab_sample_spacing(&dab, _clamp01(state->distance_percent)));
  _emit_dab(state, &dab);
  state->sampled_arc_length = 0.0f;
}

/** @brief Flush deferred initial dab before regular segment emission starts. */
static void _flush_pending_initial_if_needed(dt_drawlayer_paint_stroke_t *state,
                                             const dt_drawlayer_brush_dab_t *dab)
{
  if(!state || !dab || !state->history || state->history->len != 0) return;

  const float dx = dab->x - state->last_input_dab.x;
  const float dy = dab->y - state->last_input_dab.y;
  const float dir_len = hypotf(dx, dy);
  if(dir_len > 1e-6f)
  {
    state->last_input_dab.dir_x = dx / dir_len;
    state->last_input_dab.dir_y = dy / dir_len;
  }

  dt_drawlayer_paint_finalize_path(state);
}

/**
 * @brief Process one raw input event into zero or more emitted dabs.
 * @details Performs cubic arc-length sampling, smoothing and spacing enforcement.
 */
static void _paint_process_one_raw_input(dt_drawlayer_paint_stroke_t *state,
                                         const dt_drawlayer_paint_raw_input_t *input,
                                         const dt_drawlayer_paint_callbacks_t *callbacks,
                                         void *user_data)
{
  /* Raw event -> one segment update:
   * - build input dab from callbacks,
   * - update cumulative arc length,
   * - emit uniformly spaced samples using arc-length interpolation,
   * - apply optional smoothing and spacing enforcement. */
  if(!state || !input || !callbacks || !callbacks->build_dab || !_ensure_pending_dabs(state)) return;

  const float distance_percent = _clamp01(input->distance_percent);
  state->distance_percent = distance_percent;
  const float smoothing_percent = _clamp01(input->smoothing_percent);

  if(_paint_input_starts_new_stroke(state, input))
  {
    _paint_reset_path_runtime_state(state);
    state->stroke_seed = _paint_make_stroke_seed(input);
    if(callbacks->on_stroke_seed) callbacks->on_stroke_seed(user_data, state->stroke_seed);
  }

  dt_drawlayer_brush_dab_t dab = { 0 };
  if(!callbacks->build_dab(user_data, state, input, &dab)) return;

  if(!state->have_last_input_dab)
  {
    _emit_first_sample_if_needed(state, &dab);
    return;
  }

  const dt_drawlayer_brush_dab_t segment_start = state->last_input_dab;
  const float seg_len = hypotf(dab.x - segment_start.x, dab.y - segment_start.y);
  const float prev_arc = state->stroke_arc_length;
  enum { arc_lut_segments = 24 };
  float arc_lut[arc_lut_segments + 1] = { 0.0f };
  float arc_total = 0.0f;
  _build_raw_segment_cubic_arclen_lut(state, &segment_start, &dab,
                                      arc_lut, arc_lut_segments, &arc_total);
  const float seg_arc = (arc_total > 1e-6f) ? arc_total : seg_len;
  state->stroke_arc_length += seg_arc;
  _flush_pending_initial_if_needed(state, &dab);

  if(seg_arc > 1e-6f)
  {
    const dt_drawlayer_brush_dab_t spacing_ref_segment[2] = { segment_start, dab };
    const float segment_spacing = _paint_segment_sample_spacing(spacing_ref_segment, 2, distance_percent);
    while(TRUE)
    {
      const float sample_spacing = segment_spacing;
      const float target_arc = state->sampled_arc_length + sample_spacing;
      if(target_arc > state->stroke_arc_length + 1e-6f) break;

      if(target_arc <= prev_arc + 1e-6f)
      {
        state->sampled_arc_length = target_arc;
        continue;
      }

      const float t = _clamp01((target_arc - prev_arc) / seg_arc);
      dt_drawlayer_brush_dab_t sample
          = _sample_raw_segment_cubic_arclen(state, &segment_start, &dab, t,
                                             arc_lut, arc_lut_segments, arc_total);
      sample.stroke_batch = input->stroke_batch;
      sample.stroke_pos = DT_DRAWLAYER_PAINT_STROKE_MIDDLE;
      _apply_quadratic_dab_smoothing(state, &sample, sample_spacing, smoothing_percent,
                                     callbacks->layer_to_widget, user_data);
      _enforce_dab_center_spacing(state, &sample, sample_spacing,
                                  callbacks->layer_to_widget, user_data);
      _freeze_emitted_dab_raster_state(&sample, sample_spacing);
      _emit_dab(state, &sample);
      state->sampled_arc_length = target_arc;
    }
  }

  state->prev_raw_dab = segment_start;
  state->have_prev_raw_dab = TRUE;
  state->last_input_dab = dab;
}

gboolean dt_drawlayer_paint_queue_raw_input(dt_drawlayer_paint_stroke_t *state,
                                            const dt_drawlayer_paint_raw_input_t *input)
{
  if(!state || !input || !_ensure_raw_inputs(state)) return FALSE;
  g_array_append_val(state->raw_inputs, *input);
  return TRUE;
}

static void _paint_compact_raw_input_queue(dt_drawlayer_paint_stroke_t *state)
{
  /* FIFO compaction after processing to avoid unbounded queue growth. */
  if(!state || !state->raw_inputs) return;
  if(state->raw_input_cursor == 0) return;

  const guint processed = state->raw_input_cursor;
  const guint len = state->raw_inputs->len;
  if(processed >= len)
    g_array_set_size(state->raw_inputs, 0);
  else
    g_array_remove_range(state->raw_inputs, 0, processed);

  state->raw_input_cursor = 0;
}

void dt_drawlayer_paint_interpolate_path(dt_drawlayer_paint_stroke_t *state,
                                         const dt_drawlayer_paint_callbacks_t *callbacks,
                                         void *user_data)
{
  /* Drain all queued raw inputs in FIFO order. No coalescing here. */
  if(!state || !state->raw_inputs || !callbacks) return;

  while(state->raw_input_cursor < state->raw_inputs->len)
  {
    const dt_drawlayer_paint_raw_input_t *input
        = &g_array_index(state->raw_inputs, dt_drawlayer_paint_raw_input_t, state->raw_input_cursor);
    _paint_process_one_raw_input(state, input, callbacks, user_data);
    state->raw_input_cursor++;
  }

  _paint_compact_raw_input_queue(state);
}

static inline void _advance_smudge_pickup_state(dt_drawlayer_paint_stroke_t *state,
                                                const dt_drawlayer_brush_dab_t *current,
                                                const dt_drawlayer_brush_dab_t *previous)
{
  /* Smudge pickup follows stroke motion with a damped response. */
  if(!state || !current) return;

  gboolean have_pickup = dt_drawlayer_paint_runtime_have_smudge_pickup(state);
  float pickup_x = 0.0f;
  float pickup_y = 0.0f;
  dt_drawlayer_paint_runtime_get_smudge_pickup(state, &pickup_x, &pickup_y);

  if(!have_pickup)
  {
    dt_drawlayer_paint_runtime_set_smudge_pickup(state, current->x, current->y, TRUE);
    return;
  }

  const float dx = previous ? (current->x - previous->x) : 0.0f;
  const float dy = previous ? (current->y - previous->y) : 0.0f;
  const float travel = hypotf(dx, dy);
  if(travel <= 1e-6f) return;

  const float radius = fmaxf(current->radius, 0.5f);
  const float response = 1.0f - expf(-0.5f * travel / radius);
  pickup_x = _lerpf(pickup_x, current->x, response);
  pickup_y = _lerpf(pickup_y, current->y, response);
  dt_drawlayer_paint_runtime_set_smudge_pickup(state, pickup_x, pickup_y, TRUE);
}

gboolean dt_drawlayer_paint_rasterize_segment_to_buffer(const dt_drawlayer_brush_dab_t *dab,
                                                        const float distance_percent,
                                                        const dt_drawlayer_cache_patch_t *sample_patch,
                                                        dt_drawlayer_cache_patch_t *patch,
                                                        const float scale,
                                                        dt_drawlayer_cache_patch_t *stroke_mask,
                                                        dt_drawlayer_damaged_rect_t *runtime_state,
                                                        dt_drawlayer_paint_stroke_t *runtime_private)
{
  /* Backend helper for raster path replay:
   * keep a tiny dab window, compute spacing normalization, rasterize one sample. */
  if(!dab || !runtime_private || !runtime_private->dab_window) return FALSE;

  GArray *const history = runtime_private->dab_window;
  if(dab->stroke_pos == DT_DRAWLAYER_PAINT_STROKE_FIRST) g_array_set_size(history, 0);
  g_array_append_val(history, *dab);

  const int total = (int)history->len;
  const int count = MIN(total, 3);
  dt_drawlayer_brush_dab_t window[3] = { 0 };
  memcpy(window, ((dt_drawlayer_brush_dab_t *)history->data) + (total - count),
         (size_t)count * sizeof(dt_drawlayer_brush_dab_t));

  const dt_drawlayer_brush_dab_t *sample = &window[count - 1];
  const dt_drawlayer_brush_dab_t *previous_sample = (count > 1) ? &window[count - 2] : NULL;
  const float spacing = (sample->sample_spacing > 1e-6f)
                            ? sample->sample_spacing
                            : _paint_dab_sample_spacing(sample, _clamp01(distance_percent));
  const float sample_opacity_scale = (sample->sample_opacity_scale > 1e-6f)
                                         ? sample->sample_opacity_scale
                                         : _paint_stroke_sample_opacity_scale(sample, spacing);

  if(count == 1 && runtime_private)
    dt_drawlayer_paint_runtime_set_smudge_pickup(runtime_private, 0.0f, 0.0f, FALSE);

  if(runtime_private)
  {
    if(dab->mode == DT_DRAWLAYER_BRUSH_MODE_SMUDGE)
    {
      if(previous_sample) _advance_smudge_pickup_state(runtime_private, sample, previous_sample);
    }
    else
      dt_drawlayer_paint_runtime_set_smudge_pickup(runtime_private, 0.0f, 0.0f, FALSE);
  }

  const double t0 = dt_get_wtime();
  const gboolean rasterized
      = dt_drawlayer_brush_rasterize(sample_patch, patch, scale, sample, sample_opacity_scale, stroke_mask,
                                     runtime_private);
  const double t1 = dt_get_wtime();
  if(rasterized && runtime_state && runtime_private && runtime_private->bounds.valid)
    dt_drawlayer_paint_runtime_note_dab_damage(runtime_state, &runtime_private->bounds);

  if(darktable.unmuted & DT_DEBUG_VERBOSE)
  {
    if(runtime_private && runtime_private->bounds.valid)
    {
      const int bounds_w = runtime_private->bounds.se[0] - runtime_private->bounds.nw[0];
      const int bounds_h = runtime_private->bounds.se[1] - runtime_private->bounds.nw[1];
      dt_print(DT_DEBUG_PERF,
               "[drawlayer] paint raster mode=%d pos=%d spacing=%.3f alpha_scale=%.4f area=%dx%d ms=%.3f\n",
               sample->mode, sample->stroke_pos, spacing, sample_opacity_scale, bounds_w, bounds_h,
               1000.0 * (t1 - t0));
    }
    else
      dt_print(DT_DEBUG_PERF,
               "[drawlayer] paint raster mode=%d pos=%d spacing=%.3f alpha_scale=%.4f area=0x0 ms=%.3f\n",
               sample->mode, sample->stroke_pos, spacing, sample_opacity_scale, 1000.0 * (t1 - t0));
  }

  if(history->len > 3) g_array_remove_range(history, 0, history->len - 3);
  return TRUE;
}

dt_drawlayer_damaged_rect_t *dt_drawlayer_paint_runtime_state_create(void)
{
  dt_drawlayer_damaged_rect_t *state = g_malloc0(sizeof(*state));
  if(!state) return NULL;
  dt_drawlayer_paint_runtime_state_reset(state);
  return state;
}

void dt_drawlayer_paint_runtime_state_destroy(dt_drawlayer_damaged_rect_t **state)
{
  if(!state || !*state) return;
  dt_free(*state);
}

void dt_drawlayer_paint_runtime_state_reset(dt_drawlayer_damaged_rect_t *state)
{
  if(!state) return;
  state->valid = FALSE;
  state->nw[0] = 0;
  state->nw[1] = 0;
  state->se[0] = 0;
  state->se[1] = 0;
}

dt_drawlayer_paint_stroke_t *dt_drawlayer_paint_runtime_private_create(void)
{
  dt_drawlayer_paint_stroke_t *state = g_malloc0(sizeof(*state));
  if(!state) return NULL;
  dt_drawlayer_paint_runtime_private_reset(state);
  return state;
}

void dt_drawlayer_paint_runtime_private_destroy(dt_drawlayer_paint_stroke_t **state)
{
  if(!state || !*state) return;
  if((*state)->raw_inputs) g_array_free((*state)->raw_inputs, TRUE);
  dt_free((*state)->smudge_pixels);
  dt_free(*state);
}

void dt_drawlayer_paint_runtime_private_reset(dt_drawlayer_paint_stroke_t *state)
{
  /* Reset per-stroke transient payload while preserving reusable allocations. */
  if(!state) return;
  state->smudge_pickup_x = 0.0f;
  state->smudge_pickup_y = 0.0f;
  state->have_smudge_pickup = FALSE;
  if(state->smudge_pixels && state->smudge_width > 0 && state->smudge_height > 0)
    memset(state->smudge_pixels, 0, (size_t)state->smudge_width * state->smudge_height * 4 * sizeof(float));
}

void dt_drawlayer_paint_runtime_set_stroke_seed(dt_drawlayer_paint_stroke_t *state, const uint64_t seed)
{
  if(!state) return;
  state->stroke_seed = seed;
}

uint64_t dt_drawlayer_paint_runtime_get_stroke_seed(const dt_drawlayer_paint_stroke_t *state)
{
  return state ? state->stroke_seed : 0u;
}

gboolean dt_drawlayer_paint_runtime_ensure_smudge_pixels(dt_drawlayer_paint_stroke_t *state, const int width,
                                                         const int height)
{
  if(!state || width <= 0 || height <= 0) return FALSE;
  if(state->smudge_width == width && state->smudge_height == height && state->smudge_pixels) return TRUE;

  float *pixels = g_realloc(state->smudge_pixels, (size_t)width * height * 4 * sizeof(float));
  if(!pixels) return FALSE;
  state->smudge_pixels = pixels;
  state->smudge_width = width;
  state->smudge_height = height;
  memset(state->smudge_pixels, 0, (size_t)width * height * 4 * sizeof(float));
  return TRUE;
}

float *dt_drawlayer_paint_runtime_smudge_pixels(dt_drawlayer_paint_stroke_t *state)
{
  return state ? state->smudge_pixels : NULL;
}

int dt_drawlayer_paint_runtime_smudge_width(const dt_drawlayer_paint_stroke_t *state)
{
  return state ? state->smudge_width : 0;
}

int dt_drawlayer_paint_runtime_smudge_height(const dt_drawlayer_paint_stroke_t *state)
{
  return state ? state->smudge_height : 0;
}

gboolean dt_drawlayer_paint_runtime_have_smudge_pickup(const dt_drawlayer_paint_stroke_t *state)
{
  return state && state->have_smudge_pickup;
}

void dt_drawlayer_paint_runtime_get_smudge_pickup(const dt_drawlayer_paint_stroke_t *state,
                                                  float *x, float *y)
{
  if(x) *x = state ? state->smudge_pickup_x : 0.0f;
  if(y) *y = state ? state->smudge_pickup_y : 0.0f;
}

void dt_drawlayer_paint_runtime_set_smudge_pickup(dt_drawlayer_paint_stroke_t *state,
                                                  const float x, const float y, const gboolean have_pickup)
{
  if(!state) return;
  state->smudge_pickup_x = x;
  state->smudge_pickup_y = y;
  state->have_smudge_pickup = have_pickup;
}

gboolean dt_drawlayer_paint_runtime_prepare_dab_context(dt_drawlayer_paint_stroke_t *state,
                                                         const dt_drawlayer_brush_dab_t *dab,
                                                         const int width, const int height,
                                                         const int origin_x, const int origin_y,
                                                         const float scale)
{
  /* Compute current dab footprint in target buffer coordinates. */
  if(!state || !dab || dab->radius <= 0.0f || dab->opacity <= 0.0f || scale <= 0.0f) return FALSE;
  const float support_radius = dab->radius;

  state->bounds.valid = TRUE;
  state->bounds.nw[0] = MAX(0, (int)floorf((dab->x - support_radius) * scale) - origin_x);
  state->bounds.nw[1] = MAX(0, (int)floorf((dab->y - support_radius) * scale) - origin_y);
  state->bounds.se[0] = MIN(width, (int)ceilf((dab->x + support_radius) * scale) - origin_x + 1);
  state->bounds.se[1] = MIN(height, (int)ceilf((dab->y + support_radius) * scale) - origin_y + 1);
  if(state->bounds.se[0] <= state->bounds.nw[0]
     || state->bounds.se[1] <= state->bounds.nw[1])
    return FALSE;
  return TRUE;
}

void dt_drawlayer_paint_runtime_note_dab_damage(dt_drawlayer_damaged_rect_t *state,
                                                const dt_drawlayer_damaged_rect_t *dab_rect)
{
  if(!state) return;
  if(!dab_rect || !dab_rect->valid) return;
  if(dab_rect->se[0] <= dab_rect->nw[0] || dab_rect->se[1] <= dab_rect->nw[1]) return;
  if(!state->valid)
  {
    state->valid = TRUE;
    *state = *dab_rect;
    return;
  }
  state->nw[0] = MIN(state->nw[0], dab_rect->nw[0]);
  state->nw[1] = MIN(state->nw[1], dab_rect->nw[1]);
  state->se[0] = MAX(state->se[0], dab_rect->se[0]);
  state->se[1] = MAX(state->se[1], dab_rect->se[1]);
}

gboolean dt_drawlayer_paint_runtime_get_stroke_damage(const dt_drawlayer_damaged_rect_t *state,
                                                      dt_drawlayer_damaged_rect_t *out_rect)
{
  if(!state || !state->valid) return FALSE;
  if(out_rect) *out_rect = *state;
  return TRUE;
}

static inline void _paint_union_damage_rect(dt_drawlayer_damaged_rect_t *rect,
                                            const dt_drawlayer_damaged_rect_t *add_rect)
{
  if(!rect) return;
  if(!add_rect || !add_rect->valid) return;
  if(add_rect->se[0] <= add_rect->nw[0] || add_rect->se[1] <= add_rect->nw[1]) return;

  if(!rect->valid)
  {
    *rect = *add_rect;
    return;
  }

  rect->nw[0] = MIN(rect->nw[0], add_rect->nw[0]);
  rect->nw[1] = MIN(rect->nw[1], add_rect->nw[1]);
  rect->se[0] = MAX(rect->se[0], add_rect->se[0]);
  rect->se[1] = MAX(rect->se[1], add_rect->se[1]);
}

gboolean dt_drawlayer_paint_merge_runtime_stroke_damage(dt_drawlayer_damaged_rect_t *path_state,
                                                        dt_drawlayer_damaged_rect_t *target_rect)
{
  if(!path_state || !target_rect) return FALSE;

  dt_drawlayer_damaged_rect_t add = { 0 };
  const gboolean have_damage = dt_drawlayer_paint_runtime_get_stroke_damage(path_state, &add);
  dt_drawlayer_paint_runtime_state_reset(path_state);
  if(!have_damage) return FALSE;

  _paint_union_damage_rect(target_rect, &add);
  return TRUE;
}
