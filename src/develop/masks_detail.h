/*
    This file is part of Ansel,
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

/** @file develop/masks_detail.h
 *
 * @brief The detail-mask pixel math: a Scharr-style detail estimate over a raw or scene-referred
 * buffer, and the small blur it is smoothed with.
 *
 * @details These five functions were declared in develop/masks.h, and they have nothing to do
 * with the forms model that header describes. They take float buffers, a width, a height and a
 * threshold; no dt_masks_form_t, no group, no GUI, no pipeline. The only thing they ever shared
 * with drawn masks is the word "mask" and the fact that blending consumes both.
 *
 * Keeping them there had a cost that is easy to miss: iop/detailmask.c and iop/demosaic.c name
 * NOTHING else from masks.h, yet including it handed each of them develop/develop.h,
 * develop/pixelpipe.h, caches/pixelpipe_cache_alloc.h, common/logging.h, system/atomic.h,
 * system/simd.h and common/times.h -- and, being a supply line nobody asked for, made those
 * headers impossible to drop later. Part of the masks enclosure, issue #1299.
 *
 * Implemented in src/develop/masks/detail.c.
 */

#ifndef DT_DEVELOP_MASKS_DETAIL_H
#define DT_DEVELOP_MASKS_DETAIL_H

#include <glib.h>

#include "system/simd.h"   // dt_aligned_pixel_t

#ifdef __cplusplus
extern "C" {
#endif

/** Replicate the outermost valid row and column outwards into a @p border-wide margin, so a
 * subsequent stencil can run without a boundary test. Operates in place on @p mask. */
void dt_masks_extend_border(float *const mask, const int width, const int height, const int border);

/** Fill @p coeffs with the 9x9 separable Gaussian weights for @p sigma. */
void dt_masks_blur_9x9_coeff(float *coeffs, const float sigma);

/** 9x9 Gaussian blur of @p src into @p out. */
void dt_masks_blur_9x9(float *const src, float *const out, const int width, const int height,
                       const float sigma);

/** Detail estimate over a RAW (CFA) buffer: @p wb carries the white-balance coefficients the
 * estimate has to divide out before it can compare neighbouring photosites. */
void dt_masks_calc_rawdetail_mask(float *const src, float *const out, float *const tmp,
                                  const int width, const int height, const dt_aligned_pixel_t wb);

/** Turn a detail estimate into a mask: threshold it, and invert when @p detail is FALSE so the
 * caller gets the flat regions instead. */
void dt_masks_calc_detail_mask(float *const src, float *const out, float *const tmp, const int width,
                               const int height, const float threshold, const gboolean detail);

#ifdef __cplusplus
}
#endif

#endif // DT_DEVELOP_MASKS_DETAIL_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
