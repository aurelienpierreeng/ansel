/*
    This file is part of Ansel.
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

#ifndef DT_DEVELOP_MASKS_MASKS_TOUCHED_H
#define DT_DEVELOP_MASKS_MASKS_TOUCHED_H

#include <string.h>

#include "pixel/format.h"

/**
 * The rectangle a shape rasteriser actually wrote, in BUFFER-RELATIVE pixels.
 *
 * Every `get_mask_roi` implementation reports it through its `touched` out-parameter (a
 * `dt_iop_roi_t`, `scale` unused) so the group fold can zero and combine only what the shape
 * covers instead of the whole ROI once per shape. `width == 0 || height == 0` means the shape
 * wrote nothing. A rasteriser that cannot bound its writes reports the full buffer, which is
 * always correct and merely costs what it cost before.
 *
 * The contract is "encloses every pixel written": a box larger than the writes is fine, a box
 * smaller is a wrong mask, since the fold will not look outside it.
 */

static inline void dt_masks_touched_none(dt_iop_roi_t *touched)
{
  if(touched == NULL) return;
  touched->x = 0;
  touched->y = 0;
  touched->width = 0;
  touched->height = 0;
  touched->scale = 1.0f;
}

static inline void dt_masks_touched_full(dt_iop_roi_t *touched, const int width, const int height)
{
  if(touched == NULL) return;
  touched->x = 0;
  touched->y = 0;
  touched->width = width;
  touched->height = height;
  touched->scale = 1.0f;
}

/** Set from an inclusive pixel span [x0, x1] x [y0, y1], clamped to the buffer. Empty if it
 * falls entirely outside. */
static inline void dt_masks_touched_set(dt_iop_roi_t *touched, int x0, int y0, int x1, int y1,
                                        const int width, const int height)
{
  if(touched == NULL) return;
  if(x0 < 0) x0 = 0;
  if(y0 < 0) y0 = 0;
  if(x1 > width - 1) x1 = width - 1;
  if(y1 > height - 1) y1 = height - 1;
  if(x1 < x0 || y1 < y0)
  {
    dt_masks_touched_none(touched);
    return;
  }
  touched->x = x0;
  touched->y = y0;
  touched->width = x1 - x0 + 1;
  touched->height = y1 - y0 + 1;
  touched->scale = 1.0f;
}

static inline int dt_masks_touched_is_empty(const dt_iop_roi_t *touched)
{
  return touched == NULL || touched->width <= 0 || touched->height <= 0;
}

/** Grow `into` so it also encloses `other`. */
static inline void dt_masks_touched_union(dt_iop_roi_t *into, const dt_iop_roi_t *other)
{
  if(into == NULL || dt_masks_touched_is_empty(other)) return;
  if(dt_masks_touched_is_empty(into))
  {
    *into = *other;
    return;
  }
  const int x0 = into->x < other->x ? into->x : other->x;
  const int y0 = into->y < other->y ? into->y : other->y;
  const int ix1 = into->x + into->width - 1;
  const int ox1 = other->x + other->width - 1;
  const int iy1 = into->y + into->height - 1;
  const int oy1 = other->y + other->height - 1;
  const int x1 = ix1 > ox1 ? ix1 : ox1;
  const int y1 = iy1 > oy1 ? iy1 : oy1;
  into->x = x0;
  into->y = y0;
  into->width = x1 - x0 + 1;
  into->height = y1 - y0 + 1;
}

/** Zero a rectangle of a `width`-strided float buffer. */
static inline void dt_masks_touched_clear(float *const buffer, const int width, const dt_iop_roi_t *touched)
{
  if(buffer == NULL || dt_masks_touched_is_empty(touched)) return;
  for(int y = touched->y; y < touched->y + touched->height; y++)
    memset(buffer + (size_t)y * width + touched->x, 0, sizeof(float) * (size_t)touched->width);
}

#endif // DT_DEVELOP_MASKS_MASKS_TOUCHED_H
