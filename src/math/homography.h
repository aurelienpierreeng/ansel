/*
 *    This file is part of darktable,
 *    Copyright (C) 2016 johannes hanika.
 *    Copyright (C) 2016, 2020 Tobias Ellinghaus.
 *    Copyright (C) 2020 Pascal Obry.
 *    Copyright (C) 2021 Sakari Kapanen.
 *    Copyright (C) 2022 Martin Bařinka.
 *    
 *    darktable is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *    
 *    darktable is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *    
 *    You should have received a copy of the GNU General Public License
 *    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */


#ifndef DT_MATH_HOMOGRAPHY_H
#define DT_MATH_HOMOGRAPHY_H

/* Planar homography: the 3x3 projective map between two quadrilaterals.
 *
 * This lived in chart/common.c, next to the colour-chart parser, back when the
 * chart tool was its own program. It is plain projective geometry with no
 * dependency on either -- iop/channelmixerrgb.c uses it to map its colour-checker
 * bounding box, and was compiling chart/common.c directly to get at it. */

typedef struct point_t
{
  float x, y;
} point_t;

/** Solve for the homography `h` (9 floats) mapping the 4 `source` points onto
 *  the 4 `target` points. Returns 0 on success. */
int get_homography(const point_t *source, const point_t *target, float *h);

/** Map a single point through the homography `h`. */
point_t apply_homography(point_t p, const float *h);

/** The factor by which `h` scales AREAS at point `p`. */
float apply_homography_scaling(point_t p, const float *h);

#endif // DT_MATH_HOMOGRAPHY_H
