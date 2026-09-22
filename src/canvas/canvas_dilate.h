/*
    This file is part of Ansel,
    Copyright (C) 2026 - Aurélien PIERRE.

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

/**
 * @file canvas/canvas_dilate.h
 * @brief Growing a plane of coverage by a round reach: what a shadow's extent does before its blur.
 *
 * Every pixel becomes the LARGEST value within reach -- a grayscale dilation, a maximum filter --
 * and not a threshold grown by a distance transform, because a shadow's silhouette is not binary.
 * A cutout's feather is a ramp, a frame's transparency scales the whole of it, a glyph's stem is a
 * pixel or two of partial coverage. A threshold hardens the first the moment the extent leaves
 * zero, which is a jump nobody asked for from the slider's first step, and drops the last wherever
 * its coverage sits under the cut. The maximum moves every level outward together, so a feather
 * stays a feather, only further out, and the extent's first step moves it by one step.
 *
 * A maximum over a true disc costs a lookup per ROW of the disc, per pixel: the radius, where the
 * three box blurs after it cost nothing that grows with the radius. So the disc is approximated by
 * a sum of segments along eight lattice directions -- the axes, the diagonals and the four
 * knight's moves -- each a running maximum at a constant cost per pixel (van Herk / Gil-Werman),
 * and the whole grow costs the same whatever the extent. MEASURED (`test_canvas_dilate`, against
 * the disc's own support function, radii in half-pixel steps): within 0.76 px of the disc up to a
 * radius of 20 px and within a pixel up to 40, then 2.4 % at worst, 1.6 % past 160 px and 1.4 %
 * past 640 -- the lattice's sixteen-sided polygon, sized to the disc, tends to 1.3 %. The axes
 * and diagonals alone make an octagon, 4 % off; a square, the separable filter, is 41 % off on
 * its diagonals, which is the error this module exists not to make.
 *
 * A window is only ever computed where it lies inside the plane, so the loop that does the work
 * is two reads and a maximum, with no edge in it. The price is a MARGIN: the grow is exact only
 * that far from every edge, and the caller pads the plane by it with whatever its world holds
 * outside -- nothing for an outset shadow, the uncovered world for an inset one. Measured before
 * the margin, the per-pixel edge logic made the grow two to five times the cost of the three
 * box blurs after it.
 *
 * No canvas types and no GTK: a plane of floats in, the same plane out.
 */

#ifndef DT_CANVAS_CANVAS_DILATE_H
#define DT_CANVAS_CANVAS_DILATE_H

#include <glib.h>

/** Samples either side of the centre along each family of directions. */
typedef struct dt_canvas_dilate_lengths_t
{
  int axis;     ///< along (1, 0) and (0, 1)
  int diagonal; ///< along (1, 1) and (-1, 1)
  int knight;   ///< along (2, 1), (-2, 1), (1, 2) and (-1, 2)
} dt_canvas_dilate_lengths_t;

/**
 * @brief The segment lengths whose sum best matches a disc of `radius` pixels.
 *
 * Chosen to minimise the largest difference between how far the sum reaches in any direction and
 * the radius -- the support function, which is what decides where a grown straight edge lands.
 * All zero under half a pixel, and for a radius that is not a number.
 */
dt_canvas_dilate_lengths_t dt_canvas_dilate_lengths(float radius);

/** @brief How far a sum of these lengths reaches in the direction `angle` (radians), in pixels. */
float dt_canvas_dilate_reach(dt_canvas_dilate_lengths_t lengths, float angle);

/**
 * @brief How far from every edge of a plane `dt_canvas_dilate()` is exact, in pixels.
 *
 * The sum's reach along either axis. Pad a plane by at least this much and its original pixels
 * come out exact whatever the padding's own pixels turn into.
 */
int dt_canvas_dilate_margin(float radius);

/**
 * @brief Grow `plane` by `radius` pixels, in place.
 *
 * A pixel at least `dt_canvas_dilate_margin(radius)` from every edge takes the largest value
 * within the reach of `dt_canvas_dilate_lengths()`. Nearer an edge a pixel ends between its own
 * value and that one, never above it. `prefix` and `suffix` are scratch of the plane's size.
 *
 * @return FALSE, the plane untouched, on a missing buffer or an empty plane.
 */
gboolean dt_canvas_dilate(float *plane, float *prefix, float *suffix, int width, int height, float radius);

#endif // DT_CANVAS_CANVAS_DILATE_H
