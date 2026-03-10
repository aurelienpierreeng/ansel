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
 * drawlayer public brush API implementation
 *
 * Self-contained dab-level rasterization and profile math.
 * This file is intentionally independent from drawlayer module internals.
 */

#include "iop/drawlayer/brush.h"
#include "iop/drawlayer/paint.h"
#include "iop/drawlayer/brush_profile.h"

#include "common/darktable.h"
#include "develop/noise_generator.h"

#include <math.h>
#include <string.h>

/** @brief Clamp scalar to [0,1]. */
static inline float _clamp01(const float v)
{
  return fminf(fmaxf(v, 0.0f), 1.0f);
}

/** @brief Linear interpolation helper. */
static inline float _lerpf(const float a, const float b, const float t)
{
  return a + (b - a) * t;
}

/** @brief Stable scalar hash in [0,1] from precomputed cell seed. */
static inline float _cell_hash01_from_seed(const uint64_t cell_seed, const uint64_t salt)
{
  return (float)splitmix32(cell_seed ^ salt) / (float)UINT32_MAX;
}

/** @brief Grain-like round cellular field. Peaks at grain centers, falls off radially. */
static inline float _cellular_grain_2d(const uint64_t seed, const float x, const float y)
{
  const int cell_x = (int)floorf(x);
  const int cell_y = (int)floorf(y);
  float accum = 0.0f;
  float weight_sum = 0.0f;

  for(int oy = -1; oy <= 1; oy++)
  {
    for(int ox = -1; ox <= 1; ox++)
    {
      const int ix = cell_x + ox;
      const int iy = cell_y + oy;
      const uint64_t cell_seed = seed
                                 ^ ((uint64_t)(uint32_t)ix * 0x9e3779b185ebca87ull)
                                 ^ ((uint64_t)(uint32_t)iy * 0xc2b2ae3d27d4eb4full);
      const float jitter_x = _cell_hash01_from_seed(cell_seed, 0x94d049bb133111ebull);
      const float jitter_y = _cell_hash01_from_seed(cell_seed, 0xbf58476d1ce4e5b9ull);
      const float grain_gain = 0.65f + 0.35f * _cell_hash01_from_seed(cell_seed, 0xda942042e4dd58b5ull);
      const float dx = x - ((float)ix + jitter_x);
      const float dy = y - ((float)iy + jitter_y);
      const float dist2 = dx * dx + dy * dy;
      const float radius = 0.42f + 0.22f * _cell_hash01_from_seed(cell_seed, 0x369dea0f31a53f85ull);
      const float grain = fmaxf(0.0f, 1.0f - dist2 / (radius * radius));
      const float shaped = grain * grain * (3.0f - 2.0f * grain);
      accum += grain_gain * shaped;
      weight_sum += grain_gain;
    }
  }

  return (weight_sum > 1e-6f) ? _clamp01(accum / weight_sum) : 0.0f;
}

/** @brief Resolve octave weights from coarseness control. */
static inline void _sprinkle_octave_weights(const float coarseness, float *w0, float *w1, float *w2)
{
  const float c = 1.0f - _clamp01(coarseness);
  if(c <= 0.5f)
  {
    const float t = c * 2.0f;
    if(w0) *w0 = _lerpf(1.0f, 1.0f / 3.0f, t);
    if(w1) *w1 = _lerpf(0.0f, 1.0f / 3.0f, t);
    if(w2) *w2 = _lerpf(0.0f, 1.0f / 3.0f, t);
  }
  else
  {
    const float t = (c - 0.5f) * 2.0f;
    if(w0) *w0 = _lerpf(1.0f / 3.0f, 0.0f, t);
    if(w1) *w1 = _lerpf(1.0f / 3.0f, 0.0f, t);
    if(w2) *w2 = _lerpf(1.0f / 3.0f, 1.0f, t);
  }
}

/** @brief Evaluate sprinkle modulation from precomputed dab constants. */
static inline float _sprinkle_noise_at_pixel_precomputed(const float px, const float py,
                                                         const float scale, const float strength,
                                                         const float w0, const float w1, const float w2,
                                                         const uint64_t seed0, const uint64_t seed1, const uint64_t seed2)
{
  if(strength <= 1e-6f) return 1.0f;
  const float x = px * scale;
  const float y = py * scale;
  const float g0 = (w0 > 1e-6f) ? _cellular_grain_2d(seed0, x, y) : 0.0f;
  const float g1 = (w1 > 1e-6f) ? _cellular_grain_2d(seed1, x * 1.93f + 4.7f, y * 1.93f - 2.9f) : 0.0f;
  const float g2 = (w2 > 1e-6f) ? _cellular_grain_2d(seed2, x * 3.71f - 6.2f, y * 3.71f + 8.4f) : 0.0f;
  const float field = w0 * g0 + w1 * g1 + w2 * g2;
  const float centered = 2.0f * field - 1.0f;
  return fmaxf(0.0f, 1.0f + strength * centered);
}

typedef struct dt_drawlayer_sprinkle_preview_t
{
  float scale;
  float strength;
  float w0;
  float w1;
  float w2;
  uint64_t seed0;
  uint64_t seed1;
  uint64_t seed2;
  float gain;
  gboolean enabled;
} dt_drawlayer_sprinkle_preview_t;

static inline void _prepare_sprinkle_preview(const dt_drawlayer_brush_dab_t *dab,
                                             const float center_x, const float center_y,
                                             const float radius,
                                             dt_drawlayer_sprinkle_preview_t *preview)
{
  if(!preview)
    return;

  *preview = (dt_drawlayer_sprinkle_preview_t){
    .scale = 1.0f,
    .strength = 0.0f,
    .w0 = 0.0f,
    .w1 = 0.0f,
    .w2 = 0.0f,
    .seed0 = 0u,
    .seed1 = 0u,
    .seed2 = 0u,
    .gain = 1.0f,
    .enabled = FALSE,
  };

  if(!dab || dab->sprinkles <= 1e-6f)
    return;

  preview->scale = 1.0f / fmaxf(dab->sprinkle_size, 1.0f);
  preview->strength = _clamp01(dab->sprinkles);
  _sprinkle_octave_weights(dab->sprinkle_coarseness, &preview->w0, &preview->w1, &preview->w2);
  preview->seed0 = ((uint64_t)dab->stroke_batch << 32) ^ 0x7f4a7c159e3779b9ull;
  preview->seed1 = preview->seed0 ^ 0xbf58476d1ce4e5b9ull;
  preview->seed2 = preview->seed0 ^ 0x94d049bb133111ebull;
  preview->enabled = TRUE;

  float noise_sum = 0.0f;
  int noise_count = 0;
  for(int sy = -2; sy <= 2; sy++)
  {
    for(int sx = -2; sx <= 2; sx++)
    {
      const float nx = 0.4f * (float)sx;
      const float ny = 0.4f * (float)sy;
      if(nx * nx + ny * ny > 1.0f) continue;
      const int pixel_x = (int)lrintf(center_x + nx * radius);
      const int pixel_y = (int)lrintf(center_y + ny * radius);
      noise_sum += _sprinkle_noise_at_pixel_precomputed(pixel_x, pixel_y,
                                                        preview->scale, preview->strength,
                                                        preview->w0, preview->w1, preview->w2,
                                                        preview->seed0, preview->seed1, preview->seed2);
      noise_count++;
    }
  }

  if(noise_count > 0)
  {
    const float mean_noise = noise_sum / (float)noise_count;
    preview->gain = (mean_noise > 1e-6f) ? (1.0f / mean_noise) : 1.0f;
  }
}

static inline float _sample_sprinkle_preview(const dt_drawlayer_sprinkle_preview_t *preview,
                                             const float px, const float py)
{
  if(!preview || !preview->enabled) return 1.0f;
  return _sprinkle_noise_at_pixel_precomputed(px, py,
                                              preview->scale, preview->strength,
                                              preview->w0, preview->w1, preview->w2,
                                              preview->seed0, preview->seed1, preview->seed2) * preview->gain;
}

typedef struct dt_drawlayer_brush_runtime_view_t
{
  /* Read-only dab + precomputed geometry for one rasterization call. */
  const dt_drawlayer_brush_dab_t *dab;
  dt_drawlayer_damaged_rect_t bounds;
  int sample_origin_x;
  int sample_origin_y;
  float scaled_radius;
  float center_x;
  float center_y;
  float inv_radius;
  float tx;
  float ty;
  float alpha_noise_gain;
  float sprinkle_coord_scale;
  float sprinkle_scale;
  float sprinkle_strength;
  float sprinkle_w0;
  float sprinkle_w1;
  float sprinkle_w2;
  uint64_t sprinkle_seed0;
  uint64_t sprinkle_seed1;
  uint64_t sprinkle_seed2;
  gboolean use_stroke_mask;
  gboolean have_sprinkles;
} dt_drawlayer_brush_runtime_view_t;

typedef struct dt_drawlayer_brush_pixel_eval_t
{
  /* Per-pixel analytic evaluation (profile and resolved alpha controls). */
  float profile;
  float brush_alpha;
  float src_alpha;
  float stroke_old_alpha;
  float *stroke_alpha;
} dt_drawlayer_brush_pixel_eval_t;

/**
 * @brief Build immutable per-dab raster view from stroke runtime state.
 * @return TRUE when dab bounds are valid and non-empty.
 */
static gboolean _brush_runtime_view_from_state(const dt_drawlayer_paint_stroke_t *stroke,
                                               const dt_drawlayer_brush_dab_t *dab,
                                               const int origin_x, const int origin_y,
                                               const float scale,
                                               dt_drawlayer_brush_runtime_view_t *view)
{
  if(!stroke || !dab || !view || !stroke->bounds.valid) return FALSE;
  if(stroke->bounds.se[0] <= stroke->bounds.nw[0]
     || stroke->bounds.se[1] <= stroke->bounds.nw[1])
    return FALSE;

  const float dir_len = hypotf(dab->dir_x, dab->dir_y);
  float sprinkle_w0 = 0.0f, sprinkle_w1 = 0.0f, sprinkle_w2 = 0.0f;
  _sprinkle_octave_weights(dab->sprinkle_coarseness, &sprinkle_w0, &sprinkle_w1, &sprinkle_w2);
  const uint64_t sprinkle_seed0 = ((uint64_t)dab->stroke_batch << 32) ^ 0x7f4a7c159e3779b9ull;

  *view = (dt_drawlayer_brush_runtime_view_t){
    .dab = dab,
    .bounds = stroke->bounds,
    .sample_origin_x = origin_x,
    .sample_origin_y = origin_y,
    .scaled_radius = fmaxf(dab->radius * scale, 0.5f),
    .center_x = dab->x * scale - (float)origin_x,
    .center_y = dab->y * scale - (float)origin_y,
    .inv_radius = 0.0f,
    .tx = (dir_len > 1e-6f) ? (dab->dir_x / dir_len) : 0.0f,
    .ty = (dir_len > 1e-6f) ? (dab->dir_y / dir_len) : 1.0f,
    .alpha_noise_gain = 1.0f,
    .sprinkle_coord_scale = 1.0f / fmaxf(scale, 1e-6f),
    .sprinkle_scale = 1.0f / fmaxf(dab->sprinkle_size, 1.0f),
    .sprinkle_strength = _clamp01(dab->sprinkles),
    .sprinkle_w0 = sprinkle_w0,
    .sprinkle_w1 = sprinkle_w1,
    .sprinkle_w2 = sprinkle_w2,
    .sprinkle_seed0 = sprinkle_seed0,
    .sprinkle_seed1 = sprinkle_seed0 ^ 0xbf58476d1ce4e5b9ull,
    .sprinkle_seed2 = sprinkle_seed0 ^ 0x94d049bb133111ebull,
    .use_stroke_mask = (dab->mode == DT_DRAWLAYER_BRUSH_MODE_PAINT || dab->mode == DT_DRAWLAYER_BRUSH_MODE_ERASE),
    .have_sprinkles = (dab->sprinkles > 1e-6f),
  };
  view->inv_radius = 1.0f / view->scaled_radius;
  return TRUE;
}

/**
 * @brief Resolve multiplicative alpha noise at one pixel.
 * @return Texture factor.
 */
static inline float _sample_alpha_noise_raw(const dt_drawlayer_brush_dab_t *dab,
                                            const dt_drawlayer_brush_runtime_view_t *view,
                                            const int pixel_x, const int pixel_y)
{
  float alpha_noise = 1.0f;
  if(view && view->have_sprinkles)
  {
    const float layer_x = ((float)pixel_x + 0.5f) * view->sprinkle_coord_scale;
    const float layer_y = ((float)pixel_y + 0.5f) * view->sprinkle_coord_scale;
    alpha_noise *= _sprinkle_noise_at_pixel_precomputed(layer_x, layer_y,
                                                        view->sprinkle_scale,
                                                        view->sprinkle_strength,
                                                        view->sprinkle_w0,
                                                        view->sprinkle_w1,
                                                        view->sprinkle_w2,
                                                        view->sprinkle_seed0,
                                                        view->sprinkle_seed1,
                                                        view->sprinkle_seed2);
  }
  return alpha_noise;
}

/** @brief Estimate per-dab texture gain so noise preserves average opacity across samples. */
static inline float _estimate_alpha_noise_gain(const dt_drawlayer_brush_dab_t *dab,
                                               const dt_drawlayer_brush_runtime_view_t *view)
{
  if(!dab || !view || !view->have_sprinkles) return 1.0f;

  float noise_sum = 0.0f;
  int noise_count = 0;
  for(int sy = -2; sy <= 2; sy++)
  {
    for(int sx = -2; sx <= 2; sx++)
    {
      const float nx = 0.4f * (float)sx;
      const float ny = 0.4f * (float)sy;
      if(nx * nx + ny * ny > 1.0f) continue;

      const int pixel_x = (int)lrintf(view->sample_origin_x + view->center_x + nx * view->scaled_radius);
      const int pixel_y = (int)lrintf(view->sample_origin_y + view->center_y + ny * view->scaled_radius);
      noise_sum += _sample_alpha_noise_raw(dab, view, pixel_x, pixel_y);
      noise_count++;
    }
  }

  if(noise_count <= 0) return 1.0f;
  const float mean_noise = noise_sum / (float)noise_count;
  return (mean_noise > 1e-6f) ? (1.0f / mean_noise) : 1.0f;
}

/**
 * @brief Compute per-pixel source alpha from opacity/flow model.
 *
 * Assumptions:
 * - `flow` uses internal brush convention (see caller conversion),
 * - `sample_opacity_scale` already reflects spacing normalization.
 */
static inline float _stroke_flow_alpha(const dt_drawlayer_brush_dab_t *dab, const float opacity, const float flow,
                                       const float sample_opacity_scale, const float profile, const float brush_alpha,
                                       const float old_alpha, const float stroke_old_alpha,
                                       const gboolean have_stroke_alpha)
{
  (void)profile;
  const float opacity_scale
      = isfinite(sample_opacity_scale) ? CLAMP(sample_opacity_scale, 1e-6f, 1.0f) : 1.0f;

  if(dab->mode == DT_DRAWLAYER_BRUSH_MODE_SMUDGE
     || dab->mode == DT_DRAWLAYER_BRUSH_MODE_BLUR)
  {
    /* Replacement modes mix their sampled source against the destination, they
     * do not build over destination alpha like paint mode. */
    const float normalized = 1.0f - powf(fmaxf(1.0f - brush_alpha, 0.0f), opacity_scale);
    return _clamp01(normalized);
  }

  const float flow_ref_alpha = have_stroke_alpha ? stroke_old_alpha
                                                 : ((dab->mode == DT_DRAWLAYER_BRUSH_MODE_ERASE) ? 0.0f : old_alpha);
  /* Capped watercolor path:
   * a stroke may build locally, but the stroke-local alpha must never exceed
   * the user-requested stroke opacity. The current dab may only contribute its
   * own local brush alpha, clipped by the remaining headroom to that cap. */
  const float stroke_cap = _clamp01(opacity);
  const float remaining_to_cap = fmaxf(stroke_cap - flow_ref_alpha, 0.0f);
  const float capped_alpha = fminf(_clamp01(brush_alpha),
                                   remaining_to_cap / fmaxf(1.0f - flow_ref_alpha, 1e-6f));
  const float accum_alpha = 1.0f - powf(fmaxf(1.0f - brush_alpha, 0.0f), opacity_scale);
  /* Internal flow convention is inverse of UI flow:
   * - internal flow=0 (UI 100%) -> union/capped watercolor behavior,
   * - internal flow=1 (UI 0%)   -> accumulative highlighter behavior. */
  return _clamp01(_lerpf(capped_alpha, accum_alpha, flow));
}

/**
 * @brief Compute full analytic per-pixel brush context.
 * @return TRUE when pixel contributes non-zero alpha.
 */
static gboolean _prepare_analytic_pixel_context(const dt_drawlayer_brush_runtime_view_t *view,
                                                const float sample_opacity_scale,
                                                float *stroke_mask, const int stroke_mask_width,
                                                const int stroke_mask_height,
                                                const int x, const int y, const float old_alpha,
                                                dt_drawlayer_brush_pixel_eval_t *pixel_eval)
{
  if(!view || !pixel_eval) return FALSE;
  const dt_drawlayer_brush_dab_t *dab = view->dab;

  const float dy = ((float)y + 0.5f - view->center_y) * view->inv_radius;
  const float dx = ((float)x + 0.5f - view->center_x) * view->inv_radius;
  const float norm2 = dx * dx + dy * dy;
  pixel_eval->profile = dt_drawlayer_brush_profile_eval(dab, norm2);
  if(pixel_eval->profile <= 0.0f) return FALSE;

  const float alpha_noise = fmaxf(0.0f, _sample_alpha_noise_raw(dab, view,
                                                                view->sample_origin_x + x,
                                                                view->sample_origin_y + y)
                                           * view->alpha_noise_gain);

  pixel_eval->brush_alpha = _clamp01(dab->opacity * pixel_eval->profile * alpha_noise);
  if(pixel_eval->brush_alpha <= 0.0f) return FALSE;

  // Flow caps the stroke-wise opacity to the user-specified value
  // For this reason, we need to resolve first the stroke over transparent content,
  // then slap the transparent layer over the background. Aka temporary buffer.
  if(view->use_stroke_mask)
  {
    pixel_eval->stroke_alpha = stroke_mask + (size_t)y * stroke_mask_width + x;
    pixel_eval->stroke_old_alpha = _clamp01(*pixel_eval->stroke_alpha);
  }

  pixel_eval->src_alpha
      = _stroke_flow_alpha(dab, dab->opacity, dab->flow, sample_opacity_scale,
                           pixel_eval->profile, pixel_eval->brush_alpha, old_alpha,
                           pixel_eval->stroke_old_alpha, pixel_eval->stroke_alpha != NULL);
  return pixel_eval->src_alpha > 0.0f;
}

/**
 * @brief Build blur gather color for current dab footprint.
 * @return TRUE when footprint contributed at least one weighted sample.
 */
static gboolean _prepare_blur_context(dt_aligned_pixel_simd_t *blur_px, const float *buffer, const int width,
                                      const dt_drawlayer_brush_runtime_view_t *view)
{
  if(!blur_px) return FALSE;
  float blur_weight_sum = 0.0f;
  dt_aligned_pixel_simd_t blur_sum = dt_simd_set1(0.0f);
  const dt_drawlayer_brush_dab_t *dab = view->dab;

  for(int y = view->bounds.nw[1]; y < view->bounds.se[1]; y++)
  {
    const float dy = ((float)y + 0.5f - view->center_y) * view->inv_radius;
    const float dy2 = dy * dy;
    for(int x = view->bounds.nw[0]; x < view->bounds.se[0]; x++)
    {
      const float dx = ((float)x + 0.5f - view->center_x) * view->inv_radius;
      const float blur_weight = dt_drawlayer_brush_profile_eval(dab, dx * dx + dy2);
      if(blur_weight <= 0.0f) continue;

      const float *pixel = buffer + 4 * ((size_t)y * width + x);
      blur_sum += dt_load_simd(pixel) * dt_simd_set1(blur_weight);
      blur_weight_sum += blur_weight;
    }
  }

  if(blur_weight_sum <= 1e-8f) return FALSE;
  *blur_px = blur_sum * dt_simd_set1(1.0f / blur_weight_sum);
  return TRUE;
}


/** @brief Stable signed pseudo-random helper in [-1,1]. */
static inline float _smudge_hash_signed(const int x, const int y, const int lane)
{
  guint32 h = (guint32)(x * 73856093u) ^ (guint32)(y * 19349663u) ^ (guint32)(lane * 83492791u);
  h ^= h >> 13;
  h *= 1274126177u;
  h ^= h >> 16;
  return ((h & 0xffffu) / 32767.5f) - 1.0f;
}

/**
 * @brief Bilinear RGBA sample from float buffer.
 * @note Coordinates are clamped to valid image bounds.
 */
static inline __attribute__((always_inline)) dt_aligned_pixel_simd_t
_sample_rgba_float_bilinear(const float *buffer, const int width, const int height, const float x,
                            const float y)
{
  if(!buffer || width <= 0 || height <= 0) return dt_simd_set1(0.0f);

  const float fx = CLAMP(x, 0.0f, (float)(width - 1));
  const float fy = CLAMP(y, 0.0f, (float)(height - 1));
  const int x0 = (int)floorf(fx);
  const int y0 = (int)floorf(fy);
  const int x1 = MIN(width - 1, x0 + 1);
  const int y1 = MIN(height - 1, y0 + 1);
  const float tx = fx - x0;
  const float ty = fy - y0;

  const float *p00 = buffer + 4 * ((size_t)y0 * width + x0);
  const float *p10 = buffer + 4 * ((size_t)y0 * width + x1);
  const float *p01 = buffer + 4 * ((size_t)y1 * width + x0);
  const float *p11 = buffer + 4 * ((size_t)y1 * width + x1);
  const dt_aligned_pixel_simd_t p00v = dt_load_simd(p00);
  const dt_aligned_pixel_simd_t p10v = dt_load_simd(p10);
  const dt_aligned_pixel_simd_t p01v = dt_load_simd(p01);
  const dt_aligned_pixel_simd_t p11v = dt_load_simd(p11);
  const dt_aligned_pixel_simd_t txv = dt_simd_set1(tx);
  const dt_aligned_pixel_simd_t tyv = dt_simd_set1(ty);
  const dt_aligned_pixel_simd_t av = p00v + (p10v - p00v) * txv;
  const dt_aligned_pixel_simd_t bv = p01v + (p11v - p01v) * txv;
  return av + (bv - av) * tyv;
}

/**
 * @brief Multi-tap smudge source sample with directional jitter.
 * @param motion_dx Motion X used to orient anisotropic taps.
 * @param motion_dy Motion Y used to orient anisotropic taps.
 */
static inline __attribute__((always_inline)) dt_aligned_pixel_simd_t
_sample_smudge_source_float(const float *buffer, const int width, const int height, const float sx,
                            const float sy, const float motion_dx, const float motion_dy,
                            const int jitter_x, const int jitter_y)
{
  dt_aligned_pixel_simd_t rgba_sum = dt_simd_set1(0.0f);
  if(!buffer || width <= 0 || height <= 0) return rgba_sum;

  float dir_x = motion_dx;
  float dir_y = motion_dy;
  const float motion = hypotf(dir_x, dir_y);
  if(motion > 1e-6f)
  {
    dir_x /= motion;
    dir_y /= motion;
  }
  else
  {
    dir_x = 1.0f;
    dir_y = 0.0f;
  }

  const float perp_x = -dir_y;
  const float perp_y = dir_x;
  const float jitter = 0.60f * _smudge_hash_signed(jitter_x, jitter_y, 0);
  const float side = 0.90f + 0.30f * _smudge_hash_signed(jitter_x, jitter_y, 1);
  const float trail = 0.80f + 0.25f * _smudge_hash_signed(jitter_x, jitter_y, 2);

  const float taps[7][3] = {
    { 0.00f, jitter, 0.24f },
    { -trail, 0.25f + jitter, 0.18f },
    { -0.45f, -0.35f + jitter, 0.15f },
    { -0.15f, side + jitter, 0.11f },
    { -0.15f, -side + jitter, 0.11f },
    { 0.25f, 0.45f * side + jitter, 0.11f },
    { 0.25f, -0.45f * side + jitter, 0.10f },
  };

  float weight_sum = 0.0f;
  for(int i = 0; i < 7; i++)
  {
    const float px = sx + dir_x * taps[i][0] + perp_x * taps[i][1];
    const float py = sy + dir_y * taps[i][0] + perp_y * taps[i][1];
    const float w = taps[i][2];
    rgba_sum += _sample_rgba_float_bilinear(buffer, width, height, px, py) * dt_simd_set1(w);
    weight_sum += w;
  }

  if(weight_sum > 1e-8f)
    rgba_sum *= dt_simd_set1(1.0f / weight_sum);
  return rgba_sum;
}

/** @brief Resolve effective smudge deposit alpha for one pixel. */
static inline float _smudge_deposit_alpha(const float src_alpha, const float carried_alpha, const float opacity)
{
  const float carry = _clamp01(carried_alpha);
  const float base = _clamp01(opacity);
  const float influence = base + (1.0f - base) * carry;
  return _clamp01(src_alpha * influence);
}

/**
 * @brief Apply smudge mode for one pixel and update carried sample.
 * @pre Smudge buffer must be allocated for current dab footprint.
 */
static dt_aligned_pixel_simd_t _apply_smudge_stroke_mode(float *buffer, const int width, const int height,
                                                         dt_drawlayer_paint_stroke_t *runtime_private,
                                                         const dt_drawlayer_brush_runtime_view_t *view,
                                                         const float scale, const int origin_x,
                                                         const int origin_y, const int x, const int y,
                                                         const float src_alpha,
                                                         const dt_aligned_pixel_simd_t old_px)
{
  const dt_drawlayer_brush_dab_t *dab = view->dab;
  float sample_x = (float)x;
  float sample_y = (float)y;
  float motion_dx = 0.0f;
  float motion_dy = 0.0f;
  float *smudge_pixels = dt_drawlayer_paint_runtime_smudge_pixels(runtime_private);
  const int smudge_width = dt_drawlayer_paint_runtime_smudge_width(runtime_private);

  if(runtime_private && dt_drawlayer_paint_runtime_have_smudge_pickup(runtime_private))
  {
    /* Smudge samples from a lagging pickup point that follows dab centers. */
    float pickup_center_x = 0.0f;
    float pickup_center_y = 0.0f;
    dt_drawlayer_paint_runtime_get_smudge_pickup(runtime_private, &pickup_center_x, &pickup_center_y);
    pickup_center_x = pickup_center_x * scale - (float)origin_x;
    pickup_center_y = pickup_center_y * scale - (float)origin_y;
    sample_x = (float)x + (pickup_center_x - view->center_x);
    sample_y = (float)y + (pickup_center_y - view->center_y);
    motion_dx = view->center_x - pickup_center_x;
    motion_dy = view->center_y - pickup_center_y;
  }

  const dt_aligned_pixel_simd_t sampled_px
      = _sample_smudge_source_float(buffer, width, height, sample_x, sample_y, motion_dx, motion_dy,
                                    x - view->bounds.nw[0], y - view->bounds.nw[1]);

  if(!smudge_pixels || smudge_width <= 0) return old_px;
  float *carry = smudge_pixels + 4 * ((size_t)(y - view->bounds.nw[1]) * smudge_width + (x - view->bounds.nw[0]));
  const dt_aligned_pixel_simd_t carried_px = dt_load_simd(carry);
  const float pickup_blend = _clamp01(dab->opacity);
  const float carried_alpha = _clamp01(carry[3]);
  const float deposit_alpha = _smudge_deposit_alpha(src_alpha, carried_alpha, dab->opacity);
  const float inv_alpha = 1.0f - deposit_alpha;

  const dt_aligned_pixel_simd_t out_px = carried_px * dt_simd_set1(deposit_alpha)
                                         + old_px * dt_simd_set1(inv_alpha);
  const dt_aligned_pixel_simd_t next_carry = carried_px
                                             + (sampled_px - carried_px) * dt_simd_set1(pickup_blend);
  dt_store_simd(carry, next_carry);
  return out_px;
}

/**
 * @brief Public dab rasterization entry point.
 * @details Assumes caller already converted coordinates to layer space.
 */
gboolean dt_drawlayer_brush_rasterize(float *buffer, const int width, const int height, const int origin_x,
                                      const int origin_y, const float scale,
                                      const dt_drawlayer_brush_dab_t *dab,
                                      const float sample_opacity_scale, float *stroke_mask,
                                      const int stroke_mask_width, const int stroke_mask_height,
                                      dt_drawlayer_paint_stroke_t *runtime_private)
{
  /* Entry point for dab-level rasterization.
   * Steps:
   * 1) precompute dab bounds and orientation,
   * 2) resolve per-pixel alpha (profile/noise/flow),
   * 3) blend into target buffer and update stroke-local alpha mask. */
  if(!buffer || !dab || !runtime_private) return FALSE;
  if(dab->radius <= 0.0f || dab->opacity <= 0.0f || scale <= 0.0f) return FALSE;

  dt_drawlayer_brush_dab_t prepared = *dab;
  prepared.opacity = _clamp01(prepared.opacity);
  /* Internal flow convention is inverse of UI flow. */
  prepared.flow = 1.0f - _clamp01(prepared.flow);

  if(!dt_drawlayer_paint_runtime_prepare_dab_context(runtime_private, &prepared, width, height,
                                                      origin_x, origin_y, scale))
    return FALSE;

  dt_drawlayer_brush_runtime_view_t view = { 0 };
  if(!_brush_runtime_view_from_state(runtime_private, &prepared, origin_x, origin_y, scale, &view))
    return FALSE;
  view.alpha_noise_gain = _estimate_alpha_noise_gain(&prepared, &view);

  dt_aligned_pixel_simd_t blur_px = dt_simd_set1(0.0f);
  switch(view.dab->mode)
  {
    case DT_DRAWLAYER_BRUSH_MODE_BLUR:
      if(!_prepare_blur_context(&blur_px, buffer, width, &view)) return FALSE;
      break;
    case DT_DRAWLAYER_BRUSH_MODE_SMUDGE:
      if(!dt_drawlayer_paint_runtime_ensure_smudge_pixels(runtime_private,
                                                          view.bounds.se[0] - view.bounds.nw[0],
                                                          view.bounds.se[1] - view.bounds.nw[1]))
        return FALSE;
      break;
    case DT_DRAWLAYER_BRUSH_MODE_PAINT:
    case DT_DRAWLAYER_BRUSH_MODE_ERASE:
    default:
      break;
  }

  if(view.dab->mode == DT_DRAWLAYER_BRUSH_MODE_SMUDGE)
  {
    for(int y = view.bounds.nw[1]; y < view.bounds.se[1]; y++)
    {
      for(int x = view.bounds.nw[0]; x < view.bounds.se[0]; x++)
      {
        float *pixel = buffer + 4 * ((size_t)y * width + x);
        const float old_alpha = _clamp01(pixel[3]);
        const dt_aligned_pixel_simd_t old_px = (old_alpha > 1e-8f) ? dt_load_simd(pixel) : dt_simd_set1(0.0f);
        dt_drawlayer_brush_pixel_eval_t pixel_eval = { 0 };
        if(!_prepare_analytic_pixel_context(&view, sample_opacity_scale,
                                            stroke_mask, stroke_mask_width, stroke_mask_height,
                                            x, y, old_alpha, &pixel_eval))
          continue;

        const dt_aligned_pixel_simd_t out_px
            = _apply_smudge_stroke_mode(buffer, width, height, runtime_private, &view,
                                        scale, origin_x, origin_y,
                                        x, y, pixel_eval.src_alpha, old_px);
        dt_store_simd(pixel, out_px);

        if(pixel_eval.stroke_alpha)
        {
          *pixel_eval.stroke_alpha
              = pixel_eval.src_alpha
                + pixel_eval.stroke_old_alpha * (1.0f - pixel_eval.src_alpha);
        }
      }
    }
  }
  else
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) collapse(2) \
  dt_omp_firstprivate(view, buffer, stroke_mask, sample_opacity_scale, blur_px, width, height, origin_x, origin_y, scale, stroke_mask_width, stroke_mask_height)
#endif
    for(int y = view.bounds.nw[1]; y < view.bounds.se[1]; y++)
    {
      for(int x = view.bounds.nw[0]; x < view.bounds.se[0]; x++)
      {
        float *pixel = buffer + 4 * ((size_t)y * width + x);
        const float old_alpha = _clamp01(pixel[3]);
        const dt_aligned_pixel_simd_t old_px = (old_alpha > 1e-8f) ? dt_load_simd(pixel) : dt_simd_set1(0.0f);
        dt_drawlayer_brush_pixel_eval_t pixel_eval = { 0 };
        if(!_prepare_analytic_pixel_context(&view, sample_opacity_scale,
                                            stroke_mask, stroke_mask_width, stroke_mask_height,
                                            x, y, old_alpha, &pixel_eval))
          continue;

        dt_aligned_pixel_simd_t out_px;
        const dt_aligned_pixel_simd_t inv_alpha = dt_simd_set1(1.0f - pixel_eval.src_alpha);
        switch(view.dab->mode)
        {
          case DT_DRAWLAYER_BRUSH_MODE_ERASE:
            out_px = old_px * inv_alpha;
          case DT_DRAWLAYER_BRUSH_MODE_BLUR:
            out_px = blur_px * dt_simd_set1(pixel_eval.src_alpha) + old_px * inv_alpha;    
          case DT_DRAWLAYER_BRUSH_MODE_PAINT:
          default:
            out_px = dt_load_simd(view.dab->color) * dt_simd_set1(pixel_eval.src_alpha) + old_px * inv_alpha;
        }

        dt_store_simd(pixel, out_px);

        if(pixel_eval.stroke_alpha)
        {
          *pixel_eval.stroke_alpha
              = pixel_eval.src_alpha
                + pixel_eval.stroke_old_alpha * (1.0f - pixel_eval.src_alpha);
        }
      }
    }
  }

  return TRUE;
}

/**
 * @brief Render one dab to 8-bit ARGB surface for GUI cursor preview.
 * @return TRUE on success.
 */
gboolean dt_drawlayer_brush_rasterize_dab_argb8(const dt_drawlayer_brush_dab_t *dab, uint8_t *argb,
                                                 const int width, const int height, const int stride,
                                                 const float center_x, const float center_y,
                                                 const float opacity_multiplier)
{
  /* Lightweight GUI preview dab renderer (ARGB8), independent from stroke logic. */
  if(!dab || !argb || width <= 0 || height <= 0 || stride < 4 * width) return FALSE;

  memset(argb, 0, (size_t)stride * height);
  if(dab->radius <= 0.0f) return FALSE;

  dt_drawlayer_brush_dab_t preview = *dab;
  preview.opacity = _clamp01(preview.opacity * _clamp01(opacity_multiplier));
  if(preview.opacity <= 0.0f) return FALSE;

  const float radius = fmaxf(preview.radius, 0.5f);
  const float inv_radius = 1.0f / radius;
  const int x0 = (int)fmaxf(0.0f, floorf(center_x - radius));
  const int y0 = (int)fmaxf(0.0f, floorf(center_y - radius));
  const int x1 = (int)fminf((float)width, ceilf(center_x + radius) + 1.0f);
  const int y1 = (int)fminf((float)height, ceilf(center_y + radius) + 1.0f);

  const float disp_r = _clamp01(preview.display_color[0]);
  const float disp_g = _clamp01(preview.display_color[1]);
  const float disp_b = _clamp01(preview.display_color[2]);
  dt_drawlayer_sprinkle_preview_t sprinkle = { 0 };
  _prepare_sprinkle_preview(&preview, center_x, center_y, radius, &sprinkle);

  for(int y = y0; y < y1; y++)
  {
    const float dy = ((float)y + 0.5f - center_y) * inv_radius;
    const float dy2 = dy * dy;
    for(int x = x0; x < x1; x++)
    {
      const float dx = ((float)x + 0.5f - center_x) * inv_radius;
      const float profile = dt_drawlayer_brush_profile_eval(&preview, dx * dx + dy2);
      if(profile <= 0.0f) continue;

      const float alpha_noise = _sample_sprinkle_preview(&sprinkle, x, y);
      const float alpha = _clamp01(preview.opacity * profile * alpha_noise);
      if(alpha <= 0.0f) continue;

      uint8_t *pixel = argb + (size_t)y * stride + 4 * x;
      pixel[0] = (uint8_t)fminf(fmaxf(roundf(255.0f * _clamp01(disp_b * alpha)), 0.0f), 255.0f);
      pixel[1] = (uint8_t)fminf(fmaxf(roundf(255.0f * _clamp01(disp_g * alpha)), 0.0f), 255.0f);
      pixel[2] = (uint8_t)fminf(fmaxf(roundf(255.0f * _clamp01(disp_r * alpha)), 0.0f), 255.0f);
      pixel[3] = (uint8_t)fminf(fmaxf(roundf(255.0f * alpha), 0.0f), 255.0f);
    }
  }

  return TRUE;
}

gboolean dt_drawlayer_brush_rasterize_dab_rgbaf(const dt_drawlayer_brush_dab_t *dab, float *rgba,
                                                const int width, const int height,
                                                const float center_x, const float center_y,
                                                const float opacity_multiplier,
                                                const float background_rgb[3])
{
  if(!dab || !rgba || width <= 0 || height <= 0) return FALSE;

  const float bg_r = background_rgb ? _clamp01(background_rgb[0]) : 1.0f;
  const float bg_g = background_rgb ? _clamp01(background_rgb[1]) : 1.0f;
  const float bg_b = background_rgb ? _clamp01(background_rgb[2]) : 1.0f;
  for(int i = 0; i < width * height; i++)
  {
    rgba[4 * i + 0] = bg_r;
    rgba[4 * i + 1] = bg_g;
    rgba[4 * i + 2] = bg_b;
    rgba[4 * i + 3] = 1.0f;
  }

  if(dab->radius <= 0.0f) return FALSE;

  dt_drawlayer_brush_dab_t preview = *dab;
  preview.opacity = _clamp01(preview.opacity * _clamp01(opacity_multiplier));
  if(preview.opacity <= 0.0f) return FALSE;

  const float radius = fmaxf(preview.radius, 0.5f);
  const float inv_radius = 1.0f / radius;
  const int x0 = (int)fmaxf(0.0f, floorf(center_x - radius));
  const int y0 = (int)fmaxf(0.0f, floorf(center_y - radius));
  const int x1 = (int)fminf((float)width, ceilf(center_x + radius) + 1.0f);
  const int y1 = (int)fminf((float)height, ceilf(center_y + radius) + 1.0f);

  const float src_r = _clamp01(preview.color[0]);
  const float src_g = _clamp01(preview.color[1]);
  const float src_b = _clamp01(preview.color[2]);
  dt_drawlayer_sprinkle_preview_t sprinkle = { 0 };
  _prepare_sprinkle_preview(&preview, center_x, center_y, radius, &sprinkle);

  for(int y = y0; y < y1; y++)
  {
    const float dy = ((float)y + 0.5f - center_y) * inv_radius;
    const float dy2 = dy * dy;
    for(int x = x0; x < x1; x++)
    {
      const float dx = ((float)x + 0.5f - center_x) * inv_radius;
      const float profile = dt_drawlayer_brush_profile_eval(&preview, dx * dx + dy2);
      if(profile <= 0.0f) continue;

      const float alpha_noise = _sample_sprinkle_preview(&sprinkle, x, y);
      const float alpha = _clamp01(preview.opacity * profile * alpha_noise);
      if(alpha <= 0.0f) continue;

      float *pixel = rgba + 4 * ((size_t)y * width + x);
      const float inv_alpha = 1.0f - alpha;
      pixel[0] = src_r * alpha + pixel[0] * inv_alpha;
      pixel[1] = src_g * alpha + pixel[1] * inv_alpha;
      pixel[2] = src_b * alpha + pixel[2] * inv_alpha;
      pixel[3] = 1.0f;
    }
  }

  return TRUE;
}
