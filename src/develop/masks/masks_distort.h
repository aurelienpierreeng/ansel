/*
    This file is part of Ansel.
    Copyright (C) 2026 Aurélien Pierre.

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

/** @file develop/masks/masks_distort.h
 *
 * @brief What a mask outline needs in order to place itself, and who supplies it.
 *
 * @details The shape outline builders -- `_brush_get_pts_border()`, `_polygon_get_pts_border()`
 * -- are run from two entirely different places. The GUI calls them to draw an outline the user
 * can grab; the pixel pipeline calls them, on a worker thread, to rasterise the mask a module
 * actually blends with. They are the same geometry either way, which is why they are one
 * function, and the difference between the two callers used to be expressed by handing them a
 * different `dt_dev_pixelpipe_t *`: the GUI passed a pixel-less clone of the pipeline, the worker
 * passed its own.
 *
 * That stopped working when the clone was deleted, and the fix was not to give the GUI a
 * different pipe but to name what the builders were reading out of one. It is three things: the
 * full-resolution image size, how finely to sample the outline, and a way to compose the module
 * stack around a given iop_order. This record supplies all three, and there are exactly two
 * suppliers.
 *
 * THE WORKER SUPPLIER keeps the piece-based machinery, unchanged and forever: mask rasterisation
 * belongs to rendering, runs on the pipeline thread, and must compose the pipe it is rendering
 * -- not some other view of the same history.
 *
 * THE GUI SUPPLIER composes through develop/geometry/geometry.h, which answers from published
 * records.
 *
 * Nothing else may implement this. Two suppliers are a seam; three are a fork.
 */

#ifndef DT_DEVELOP_MASKS_MASKS_DISTORT_H
#define DT_DEVELOP_MASKS_MASKS_DISTORT_H

#include <glib.h>
#include <stdint.h>

#include "develop/develop.h"
#include "develop/pixelpipe_hb.h"

/**
 * @brief Where an outline builder gets its geometry from.
 *
 * @details Build one with ::dt_masks_distort_for_pipe or ::dt_masks_distort_for_gui; do not
 * fill it by hand, so that the two suppliers stay the only two.
 */
typedef struct dt_masks_distort_t
{
  /** The pipe to compose, or NULL to compose through the geometry service. */
  dt_dev_pixelpipe_t *pipe;
  /** Needed by the GUI supplier, and harmless for the other. */
  dt_develop_t *dev;

  /** Full-resolution image size the outline's normalised coordinates scale against. */
  int32_t iwidth, iheight;

  /**
   * How far apart consecutive samples of the outline may land, in image pixels. Above one, the
   * samples spread out and the radial spokes stamped between them leave gaps (#1116), so a
   * caller that wants a pixel-accurate outline asks for one.
   */
  int rasterization_step;
} dt_masks_distort_t;

/** @brief The rendering supplier: compose this pipe, sample as this pipe was told to. */
static inline dt_masks_distort_t dt_masks_distort_for_pipe(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev)
{
  dt_masks_distort_t d = { pipe, dev, 0, 0, 1 };
  if(!IS_NULL_PTR(pipe))
  {
    d.iwidth = pipe->iwidth;
    d.iheight = pipe->iheight;
    d.rasterization_step = pipe->mask_rasterization_step;
  }
  return d;
}

/**
 * @brief The GUI supplier: compose through the geometry service, at full resolution, one pixel
 * at a time.
 *
 * @details The step is 1 because that is what the GUI has always had: the pixel-less pipe this
 * replaces was never given a rasterisation step, so it kept the one its init set, and outlines
 * the user drags are drawn pixel-accurate. The size is the raw geometry, which is what that
 * pipe's input was set from.
 */
static inline dt_masks_distort_t dt_masks_distort_for_gui(dt_develop_t *dev)
{
  dt_masks_distort_t d = { NULL, dev, 0, 0, 1 };
  int32_t raw_width = 0;
  int32_t raw_height = 0;
  if(dt_dev_geometry_get_raw_size(dev, &raw_width, &raw_height))
  {
    d.iwidth = raw_width;
    d.iheight = raw_height;
  }
  return d;
}

/** @brief Compose forward, bounded exactly as dt_dev_distort_transform_plus() is. */
static inline int dt_masks_distort_transform(const dt_masks_distort_t *const d, const double iop_order,
                                             const int transf_direction, float *points,
                                             size_t points_count)
{
  if(!IS_NULL_PTR(d->pipe))
    return dt_dev_distort_transform_plus(d->pipe, iop_order, transf_direction, points, points_count);
  return dt_dev_distort_transform_gui(d->dev, iop_order, transf_direction, points, points_count);
}

/** @brief Compose backward, same bounds. */
static inline int dt_masks_distort_backtransform(const dt_masks_distort_t *const d, const double iop_order,
                                                 const int transf_direction, float *points,
                                                 size_t points_count)
{
  if(!IS_NULL_PTR(d->pipe))
    return dt_dev_distort_backtransform_plus(d->pipe, iop_order, transf_direction, points, points_count);
  return dt_dev_distort_backtransform_gui(d->dev, iop_order, transf_direction, points, points_count);
}

#endif // DT_DEVELOP_MASKS_MASKS_DISTORT_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
