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
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include <stdint.h>
#include <stdlib.h>

typedef enum dt_lut3d_interpolation_t
{
  DT_LUT3D_INTERP_TETRAHEDRAL = 0,
  DT_LUT3D_INTERP_TRILINEAR = 1,
  DT_LUT3D_INTERP_PYRAMID = 2,
} dt_lut3d_interpolation_t;

/**
 * @brief Apply one interpolation model over a packed RGB CLUT.
 *
 * @details
 * The interpolation always happens in the normalized `[0, 1]` lattice domain.
 * `normalization` lets callers map scene-referred RGB into that domain before
 * lookup and restore the white level afterwards without rescaling the whole
 * buffer in separate passes.
 *
 * All modules using a dense RGB CLUT should go through this shared runtime so
 * file-backed LUTs and procedurally-generated LUTs only differ by how they
 * author the lattice values, not by how they traverse the cells.
 */
void dt_lut3d_apply(const float *in, float *out, size_t pixel_nb, const float *clut, uint16_t level,
                    float normalization, dt_lut3d_interpolation_t interpolation);

void dt_lut3d_tetrahedral_interp(const float *in, float *out, size_t pixel_nb, const float *clut,
                                 uint16_t level, float normalization);
void dt_lut3d_trilinear_interp(const float *in, float *out, size_t pixel_nb, const float *clut,
                               uint16_t level, float normalization);
void dt_lut3d_pyramid_interp(const float *in, float *out, size_t pixel_nb, const float *clut,
                             uint16_t level, float normalization);
