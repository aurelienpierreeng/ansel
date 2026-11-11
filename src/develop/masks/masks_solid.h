/*
    This file is part of Ansel,
    Copyright (C) 2026 Guillaume Stutin.

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

#ifndef DT_DEVELOP_MASKS_MASKS_SOLID_H
#define DT_DEVELOP_MASKS_MASKS_SOLID_H

#ifdef __cplusplus
extern "C" {
#endif

struct dt_masks_form_gui_points_t;

/** The edge of the part of a stroke painted at full density, as closed loops.
 *
 * The rasteriser paints every spoke solid from its spine sample to its @p contour sample, so the
 * solid part is the union of the bands between neighbouring spokes. That union is filled on a grid
 * of @p cell image pixels and its edge traced at half coverage.
 *
 * Reads `points` and `border_count` of @p gui_points, and @p contour, index-aligned with `points`,
 * their first @p header samples being node data. Writes `fading`, `fading_count`, `fading_breaks`
 * and `fading_break_count`: every loop ends on its first point, and a zero-length break starts each
 * loop but the first.
 *
 * @return 0 on success, an empty edge included; 1 when an allocation failed, nothing written. */
int dt_masks_solid_edge(struct dt_masks_form_gui_points_t *gui_points, const float *contour, int header,
                        float cell);

#ifdef __cplusplus
}
#endif

#endif // DT_DEVELOP_MASKS_MASKS_SOLID_H
