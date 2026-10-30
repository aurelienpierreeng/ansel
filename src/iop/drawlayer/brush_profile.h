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

#ifndef DT_IOP_DRAWLAYER_BRUSH_PROFILE_H
#define DT_IOP_DRAWLAYER_BRUSH_PROFILE_H

#include "iop/drawlayer/brush.h"

#include <math.h>

/** @file
 *  @brief Inline brush profile and mass primitives shared by paint/brush code.
 */

/** @brief Clamp scalar value to [0, 1]. */
static inline float dt_drawlayer_brush_profile_clamp01(const float v)
{
  return fminf(fmaxf(v, 0.0f), 1.0f);
}

/** @brief Evaluate normalized edge-falloff transition profile by shape. */
static inline float dt_drawlayer_brush_transition_profile_eval(const int shape, const float t, const float inv_t)
{
  switch(shape)
  {
    case DT_DRAWLAYER_BRUSH_SHAPE_QUADRATIC:
      return inv_t * inv_t;
    case DT_DRAWLAYER_BRUSH_SHAPE_SIGMOIDAL:
    {
      const float smooth = t * t * (3.0f - 2.0f * t);
      return 1.0f - smooth;
    }
    case DT_DRAWLAYER_BRUSH_SHAPE_LINEAR:
    default:
      return inv_t;
  }
}

/** @brief Evaluate integrated mass primitive of transition zone by shape. */
static inline float dt_drawlayer_brush_transition_mass_primitive_eval(const int shape, const float u,
                                                                      const float inner, const float w,
                                                                      const float base)
{
  switch(shape)
  {
    case DT_DRAWLAYER_BRUSH_SHAPE_QUADRATIC:
    {
      const float q_u = 0.5f * u * u - (2.0f / 3.0f) * u * u * u + 0.25f * u * u * u * u;
      const float q_i = 0.5f * inner * inner - (2.0f / 3.0f) * inner * inner * inner
                        + 0.25f * inner * inner * inner * inner;
      return base + (q_u - q_i) / (w * w);
    }
    case DT_DRAWLAYER_BRUSH_SHAPE_SIGMOIDAL:
    {
      const float s = dt_drawlayer_brush_profile_clamp01((u - inner) / w);
      const float s2 = s * s;
      const float s3 = s2 * s;
      const float s4 = s2 * s2;
      const float s5 = s4 * s;
      const float delta = w * (inner * (s - s3 + 0.5f * s4)
                               + w * (0.5f * s2 - 0.75f * s4 + 0.4f * s5));
      return base + delta;
    }
    case DT_DRAWLAYER_BRUSH_SHAPE_LINEAR:
    default:
    {
      const float l_u = 0.5f * u * u - (1.0f / 3.0f) * u * u * u;
      const float l_i = 0.5f * inner * inner - (1.0f / 3.0f) * inner * inner * inner;
      return base + (l_u - l_i) / w;
    }
  }
}

/**
 * @brief The part of the profile that depends on the dab and not on the pixel.
 *
 * `dt_drawlayer_brush_profile_eval` recomputed `hardness`, `min_inner`, `inner` and the
 * transition width for EVERY pixel of every dab, although all four are properties of the
 * dab. Resolve them once with `dt_drawlayer_brush_profile_prepare` and evaluate with
 * `dt_drawlayer_brush_profile_eval_fast`, which performs the identical arithmetic on the
 * identical operands and therefore returns identical floats.
 */
typedef struct dt_drawlayer_brush_profile_const_t
{
  int shape;         /**< Copy of the dab's shape. */
  gboolean gaussian; /**< Shape is GAUSSIAN: the analytic branch, no inner/width. */
  gboolean flat;     /**< Hardness saturates: the profile is 1 everywhere inside the disc. */
  float inner;       /**< Normalized radius at which the fall-off starts. */
  float width;       /**< `max(1 - inner, 1e-6)`, the transition width, kept as a DIVISOR so
                      *   the division matches the original bit for bit. */
} dt_drawlayer_brush_profile_const_t;

/** @brief Resolve the per-dab half of the profile once. */
static inline void dt_drawlayer_brush_profile_prepare(const dt_drawlayer_brush_dab_t *dab,
                                                      dt_drawlayer_brush_profile_const_t *out)
{
  if(IS_NULL_PTR(dab) || IS_NULL_PTR(out)) return;
  out->shape = dab->shape;
  out->gaussian = (dab->shape == DT_DRAWLAYER_BRUSH_SHAPE_GAUSSIAN);
  out->flat = FALSE;
  out->inner = 0.0f;
  out->width = 1.0f;
  if(out->gaussian) return;

  const float hardness = dt_drawlayer_brush_profile_clamp01(dab->hardness);
  if(hardness >= 1.0f - 1e-6f)
  {
    out->flat = TRUE;
    return;
  }
  const float min_inner = 0.5f / fmaxf(dab->radius, 0.5f);
  out->inner = fmaxf(hardness, dt_drawlayer_brush_profile_clamp01(min_inner));
  out->width = fmaxf(1.0f - out->inner, 1e-6f);
}

/** @brief Evaluate the profile against pre-resolved dab constants. */
static inline float dt_drawlayer_brush_profile_eval_fast(const dt_drawlayer_brush_profile_const_t *pc,
                                                         const float norm2)
{
  if(norm2 >= 1.0f) return 0.0f;

  if(pc->gaussian)
  {
    const float radius = sqrtf(norm2);
    if(radius < 0.5f) return 1.0f - 6.0f * norm2 + 6.0f * norm2 * radius;
    const float inv_r = 1.0f - radius;
    return 2.0f * inv_r * inv_r * inv_r;
  }

  if(pc->flat) return 1.0f;

  const float radius = sqrtf(norm2);
  if(radius <= pc->inner) return 1.0f;

  const float t = dt_drawlayer_brush_profile_clamp01((radius - pc->inner) / pc->width);
  return dt_drawlayer_brush_transition_profile_eval(pc->shape, t, 1.0f - t);
}

/**
 * @brief Evaluate normalized brush profile at squared normalized radius.
 * @param dab Current dab parameters.
 * @param norm2 Squared normalized radius (`r^2` in [0, inf)).
 */
static inline float dt_drawlayer_brush_profile_eval(const dt_drawlayer_brush_dab_t *dab, const float norm2)
{
  if(IS_NULL_PTR(dab) || norm2 >= 1.0f) return 0.0f;

  if(dab->shape == DT_DRAWLAYER_BRUSH_SHAPE_GAUSSIAN)
  {
    const float radius = sqrtf(norm2);
    if(radius < 0.5f) return 1.0f - 6.0f * norm2 + 6.0f * norm2 * radius;
    const float inv_r = 1.0f - radius;
    return 2.0f * inv_r * inv_r * inv_r;
  }

  const float hardness = dt_drawlayer_brush_profile_clamp01(dab->hardness);
  if(hardness >= 1.0f - 1e-6f) return 1.0f;

  const float min_inner = 0.5f / fmaxf(dab->radius, 0.5f);
  const float inner = fmaxf(hardness, dt_drawlayer_brush_profile_clamp01(min_inner));
  const float radius = sqrtf(norm2);
  if(radius <= inner) return 1.0f;

  const float t = dt_drawlayer_brush_profile_clamp01((radius - inner) / fmaxf(1.0f - inner, 1e-6f));
  return dt_drawlayer_brush_transition_profile_eval(dab->shape, t, 1.0f - t);
}

/**
 * @brief Evaluate radial mass primitive from center to normalized radius `u_in`.
 * @note Used by stroke-level overlap normalization.
 */
static inline float dt_drawlayer_brush_mass_primitive_eval(const dt_drawlayer_brush_dab_t *dab, const float u_in)
{
  if(IS_NULL_PTR(dab)) return 0.0f;
  const float u = dt_drawlayer_brush_profile_clamp01(u_in);
  if(u <= 0.0f) return 0.0f;

  if(dab->shape == DT_DRAWLAYER_BRUSH_SHAPE_GAUSSIAN)
  {
    if(u <= 0.5f) return 0.5f * u * u - 1.5f * u * u * u * u + 1.2f * u * u * u * u * u;
    const float u2 = u * u;
    const float u3 = u2 * u;
    const float u4 = u2 * u2;
    const float u5 = u4 * u;
    return u2 - 2.0f * u3 + 1.5f * u4 - 0.4f * u5 - 0.0125f;
  }

  const float hardness = dt_drawlayer_brush_profile_clamp01(dab->hardness);
  if(hardness >= 1.0f - 1e-6f) return 0.5f * u * u;
  const float min_inner = 0.5f / fmaxf(dab->radius, 0.5f);
  const float inner = fmaxf(hardness, dt_drawlayer_brush_profile_clamp01(min_inner));
  if(u <= inner) return 0.5f * u * u;
  const float w = fmaxf(1.0f - inner, 1e-6f);
  const float base = 0.5f * inner * inner;
  return dt_drawlayer_brush_transition_mass_primitive_eval(dab->shape, u, inner, w, base);
}
#endif // DT_IOP_DRAWLAYER_BRUSH_PROFILE_H
