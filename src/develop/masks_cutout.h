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

#ifndef DT_DEVELOP_MASKS_CUTOUT_H
#define DT_DEVELOP_MASKS_CUTOUT_H

/** @file develop/masks_cutout.h
 *
 * @brief A drawn-mask shape rasterised on its own, with no darkroom behind it.
 *
 * @details The darkroom's shapes answer through a module, a pipe and a dev. A consumer that
 * only has a rectangle to cut out -- the canvas atelier's frames -- describes the shape here
 * in the rectangle's unit square and gets the raster back, so it never has to build a
 * darkroom to ask the masks module a question. The shapes, their fall-off and their
 * parameters are exactly the darkroom's: the same code rasterises both.
 */

#include <glib.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum dt_masks_cutout_shape_t
{
  DT_MASKS_CUTOUT_NONE = 0,
  DT_MASKS_CUTOUT_CIRCLE = 1,
  DT_MASKS_CUTOUT_ELLIPSE = 2,
  DT_MASKS_CUTOUT_POLYGON = 3,
  DT_MASKS_CUTOUT_GRADIENT = 4,
} dt_masks_cutout_shape_t;

/** Floats per polygon node, in this order. */
enum
{
  DT_MASKS_CUTOUT_NODE_X = 0,
  DT_MASKS_CUTOUT_NODE_Y = 1,
  DT_MASKS_CUTOUT_NODE_CTRL1_X = 2,
  DT_MASKS_CUTOUT_NODE_CTRL1_Y = 3,
  DT_MASKS_CUTOUT_NODE_CTRL2_X = 4,
  DT_MASKS_CUTOUT_NODE_CTRL2_Y = 5,
  DT_MASKS_CUTOUT_NODE_SMOOTH = 6, ///< non-zero: the curve is smoothed through the node and the control points are computed
  DT_MASKS_CUTOUT_NODE_BORDER1 = 7, ///< the fall-off's own radius on one side of the node; 0 takes the shape's
  DT_MASKS_CUTOUT_NODE_BORDER2 = 8, ///< and on the other
  DT_MASKS_CUTOUT_NODE_FLOATS = 9, ///< the least a node record holds; `node_stride` may be larger
};

/**
 * A shape in the unit square of the rectangle it cuts: (0, 0) top-left, (1, 1) bottom-right.
 * Radii, the feather and the gradient's extent are fractions of the rectangle's shorter side,
 * the darkroom's own convention for a shape's size.
 */
typedef struct dt_masks_cutout_t
{
  dt_masks_cutout_shape_t shape;
  float center[2];     ///< circle, ellipse: the centre; gradient: the anchor
  float radius[2];     ///< circle: [0]; ellipse: horizontal, vertical; gradient: extent, curvature
  float rotation;      ///< degrees; ellipse and gradient
  float feather;       ///< the fall-off's extent past the shape's edge, and a polygon node's default
  gboolean invert;     ///< keep the outside of the shape
  uint32_t node_count; ///< polygon nodes
  uint32_t node_stride;///< floats per node in `nodes`, at least DT_MASKS_CUTOUT_NODE_FLOATS
  const float *nodes;
} dt_masks_cutout_t;

/**
 * @brief Rasterise the shape over a width x height raster of the rectangle.
 * @return width * height floats in [0, 1], row-major, 1 inside the shape; free with
 * dt_masks_cutout_free(). NULL when the shape is NONE or cannot be built.
 */
float *dt_masks_cutout_rasterise(const dt_masks_cutout_t *cutout, int width, int height);

void dt_masks_cutout_free(float *raster);

#ifdef __cplusplus
}
#endif

#endif // DT_DEVELOP_MASKS_CUTOUT_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
