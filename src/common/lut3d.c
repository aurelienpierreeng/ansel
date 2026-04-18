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

#include "common/darktable.h"
#include "common/lut3d.h"

#include <math.h>

static inline void _prepare_lut_input(const float *const input, float normalized[3], float residual[3],
                                      float rgbd[3], int rgbi[3], const uint16_t level,
                                      const float safe_normalization)
{
  for(int c = 0; c < 3; c++)
  {
    const float unclamped = input[c] / safe_normalization;
    normalized[c] = fminf(fmaxf(unclamped, 0.f), 1.f);
    residual[c] = unclamped - normalized[c];
  }

  for(int c = 0; c < 3; c++)
    rgbd[c] = normalized[c] * (float)(level - 1);

  for(int c = 0; c < 3; c++)
    rgbi[c] = ((int)rgbd[c] < 0) ? 0 : (((int)rgbd[c] > level - 2) ? level - 2 : (int)rgbd[c]);

  for(int c = 0; c < 3; c++)
    rgbd[c] -= rgbi[c];
}

static inline __attribute__((always_inline)) void _finish_lut_output(const float *const input, float *const output, const float residual[3],
                                      const float safe_normalization)
{
  /**
   * The LUT only samples the unit RGB cube. Outside that domain, extend the
   * mapping by keeping the boundary deformation and adding back the part of
   * the input that lies beyond the cube. This keeps identity LUTs truly
   * neutral and avoids artificial clipping when the surrounding profile
   * transform produces values slightly outside `[0, 1]`.
   */
  output[0] = (output[0] + residual[0]) * safe_normalization;
  output[1] = (output[1] + residual[1]) * safe_normalization;
  output[2] = (output[2] + residual[2]) * safe_normalization;
  output[3] = input[3];
}

__DT_CLONE_TARGETS__
void dt_lut3d_tetrahedral_interp(const float *const in, float *const out, const size_t pixel_nb,
                                 const float *const restrict clut, const uint16_t level,
                                 const float normalization)
{
  const int level2 = level * level;
  const float safe_normalization = fmaxf(normalization, 1e-6f);
  __OMP_PARALLEL_FOR__()
  for(size_t k = 0; k < (size_t)(pixel_nb * 4); k += 4)
  {
    const float *const input = in + k;
    float *const output = out + k;
    float normalized[3];
    float residual[3];
    float rgbd[3];
    int rgbi[3];

    _prepare_lut_input(input, normalized, residual, rgbd, rgbi, level, safe_normalization);

    const int color = rgbi[0] + rgbi[1] * level + rgbi[2] * level2;
    const int i000 = color * 3;
    const int i100 = i000 + 3;
    const int i010 = (color + level) * 3;
    const int i110 = i010 + 3;
    const int i001 = (color + level2) * 3;
    const int i101 = i001 + 3;
    const int i011 = (color + level + level2) * 3;
    const int i111 = i011 + 3;

    if(rgbd[0] > rgbd[1])
    {
      if(rgbd[1] > rgbd[2])
      {
        output[0] = (1 - rgbd[0]) * clut[i000] + (rgbd[0] - rgbd[1]) * clut[i100]
                    + (rgbd[1] - rgbd[2]) * clut[i110] + rgbd[2] * clut[i111];
        output[1] = (1 - rgbd[0]) * clut[i000 + 1] + (rgbd[0] - rgbd[1]) * clut[i100 + 1]
                    + (rgbd[1] - rgbd[2]) * clut[i110 + 1] + rgbd[2] * clut[i111 + 1];
        output[2] = (1 - rgbd[0]) * clut[i000 + 2] + (rgbd[0] - rgbd[1]) * clut[i100 + 2]
                    + (rgbd[1] - rgbd[2]) * clut[i110 + 2] + rgbd[2] * clut[i111 + 2];
      }
      else if(rgbd[0] > rgbd[2])
      {
        output[0] = (1 - rgbd[0]) * clut[i000] + (rgbd[0] - rgbd[2]) * clut[i100]
                    + (rgbd[2] - rgbd[1]) * clut[i101] + rgbd[1] * clut[i111];
        output[1] = (1 - rgbd[0]) * clut[i000 + 1] + (rgbd[0] - rgbd[2]) * clut[i100 + 1]
                    + (rgbd[2] - rgbd[1]) * clut[i101 + 1] + rgbd[1] * clut[i111 + 1];
        output[2] = (1 - rgbd[0]) * clut[i000 + 2] + (rgbd[0] - rgbd[2]) * clut[i100 + 2]
                    + (rgbd[2] - rgbd[1]) * clut[i101 + 2] + rgbd[1] * clut[i111 + 2];
      }
      else
      {
        output[0] = (1 - rgbd[2]) * clut[i000] + (rgbd[2] - rgbd[0]) * clut[i001]
                    + (rgbd[0] - rgbd[1]) * clut[i101] + rgbd[1] * clut[i111];
        output[1] = (1 - rgbd[2]) * clut[i000 + 1] + (rgbd[2] - rgbd[0]) * clut[i001 + 1]
                    + (rgbd[0] - rgbd[1]) * clut[i101 + 1] + rgbd[1] * clut[i111 + 1];
        output[2] = (1 - rgbd[2]) * clut[i000 + 2] + (rgbd[2] - rgbd[0]) * clut[i001 + 2]
                    + (rgbd[0] - rgbd[1]) * clut[i101 + 2] + rgbd[1] * clut[i111 + 2];
      }
    }
    else
    {
      if(rgbd[2] > rgbd[1])
      {
        output[0] = (1 - rgbd[2]) * clut[i000] + (rgbd[2] - rgbd[1]) * clut[i001]
                    + (rgbd[1] - rgbd[0]) * clut[i011] + rgbd[0] * clut[i111];
        output[1] = (1 - rgbd[2]) * clut[i000 + 1] + (rgbd[2] - rgbd[1]) * clut[i001 + 1]
                    + (rgbd[1] - rgbd[0]) * clut[i011 + 1] + rgbd[0] * clut[i111 + 1];
        output[2] = (1 - rgbd[2]) * clut[i000 + 2] + (rgbd[2] - rgbd[1]) * clut[i001 + 2]
                    + (rgbd[1] - rgbd[0]) * clut[i011 + 2] + rgbd[0] * clut[i111 + 2];
      }
      else if(rgbd[2] > rgbd[0])
      {
        output[0] = (1 - rgbd[1]) * clut[i000] + (rgbd[1] - rgbd[2]) * clut[i010]
                    + (rgbd[2] - rgbd[0]) * clut[i011] + rgbd[0] * clut[i111];
        output[1] = (1 - rgbd[1]) * clut[i000 + 1] + (rgbd[1] - rgbd[2]) * clut[i010 + 1]
                    + (rgbd[2] - rgbd[0]) * clut[i011 + 1] + rgbd[0] * clut[i111 + 1];
        output[2] = (1 - rgbd[1]) * clut[i000 + 2] + (rgbd[1] - rgbd[2]) * clut[i010 + 2]
                    + (rgbd[2] - rgbd[0]) * clut[i011 + 2] + rgbd[0] * clut[i111 + 2];
      }
      else
      {
        output[0] = (1 - rgbd[1]) * clut[i000] + (rgbd[1] - rgbd[0]) * clut[i010]
                    + (rgbd[0] - rgbd[2]) * clut[i110] + rgbd[2] * clut[i111];
        output[1] = (1 - rgbd[1]) * clut[i000 + 1] + (rgbd[1] - rgbd[0]) * clut[i010 + 1]
                    + (rgbd[0] - rgbd[2]) * clut[i110 + 1] + rgbd[2] * clut[i111 + 1];
        output[2] = (1 - rgbd[1]) * clut[i000 + 2] + (rgbd[1] - rgbd[0]) * clut[i010 + 2]
                    + (rgbd[0] - rgbd[2]) * clut[i110 + 2] + rgbd[2] * clut[i111 + 2];
      }
    }

    _finish_lut_output(input, output, residual, safe_normalization);
  }
}

__DT_CLONE_TARGETS__
void dt_lut3d_trilinear_interp(const float *const in, float *const out, const size_t pixel_nb,
                               const float *const restrict clut, const uint16_t level,
                               const float normalization)
{
  const int level2 = level * level;
  const float safe_normalization = fmaxf(normalization, 1e-6f);
  __OMP_PARALLEL_FOR__()
  for(size_t k = 0; k < (size_t)(pixel_nb * 4); k += 4)
  {
    const float *const input = in + k;
    float *const output = out + k;
    float normalized[3];
    float residual[3];
    float rgbd[3];
    int rgbi[3];
    float tmp[6];

    _prepare_lut_input(input, normalized, residual, rgbd, rgbi, level, safe_normalization);

    const int color = rgbi[0] + rgbi[1] * level + rgbi[2] * level2;
    int i = color * 3;
    int j = (color + 1) * 3;

    tmp[0] = clut[i] * (1 - rgbd[0]) + clut[j] * rgbd[0];
    tmp[1] = clut[i + 1] * (1 - rgbd[0]) + clut[j + 1] * rgbd[0];
    tmp[2] = clut[i + 2] * (1 - rgbd[0]) + clut[j + 2] * rgbd[0];

    i = (color + level) * 3;
    j = (color + level + 1) * 3;

    tmp[3] = clut[i] * (1 - rgbd[0]) + clut[j] * rgbd[0];
    tmp[4] = clut[i + 1] * (1 - rgbd[0]) + clut[j + 1] * rgbd[0];
    tmp[5] = clut[i + 2] * (1 - rgbd[0]) + clut[j + 2] * rgbd[0];

    output[0] = tmp[0] * (1 - rgbd[1]) + tmp[3] * rgbd[1];
    output[1] = tmp[1] * (1 - rgbd[1]) + tmp[4] * rgbd[1];
    output[2] = tmp[2] * (1 - rgbd[1]) + tmp[5] * rgbd[1];

    i = (color + level2) * 3;
    j = (color + level2 + 1) * 3;

    tmp[0] = clut[i] * (1 - rgbd[0]) + clut[j] * rgbd[0];
    tmp[1] = clut[i + 1] * (1 - rgbd[0]) + clut[j + 1] * rgbd[0];
    tmp[2] = clut[i + 2] * (1 - rgbd[0]) + clut[j + 2] * rgbd[0];

    i = (color + level + level2) * 3;
    j = (color + level + level2 + 1) * 3;

    tmp[3] = clut[i] * (1 - rgbd[0]) + clut[j] * rgbd[0];
    tmp[4] = clut[i + 1] * (1 - rgbd[0]) + clut[j + 1] * rgbd[0];
    tmp[5] = clut[i + 2] * (1 - rgbd[0]) + clut[j + 2] * rgbd[0];

    tmp[0] = tmp[0] * (1 - rgbd[1]) + tmp[3] * rgbd[1];
    tmp[1] = tmp[1] * (1 - rgbd[1]) + tmp[4] * rgbd[1];
    tmp[2] = tmp[2] * (1 - rgbd[1]) + tmp[5] * rgbd[1];

    output[0] = output[0] * (1 - rgbd[2]) + tmp[0] * rgbd[2];
    output[1] = output[1] * (1 - rgbd[2]) + tmp[1] * rgbd[2];
    output[2] = output[2] * (1 - rgbd[2]) + tmp[2] * rgbd[2];

    _finish_lut_output(input, output, residual, safe_normalization);
  }
}

__DT_CLONE_TARGETS__
void dt_lut3d_pyramid_interp(const float *const in, float *const out, const size_t pixel_nb,
                             const float *const restrict clut, const uint16_t level,
                             const float normalization)
{
  const int level2 = level * level;
  const float safe_normalization = fmaxf(normalization, 1e-6f);
  __OMP_PARALLEL_FOR__()
  for(size_t k = 0; k < (size_t)(pixel_nb * 4); k += 4)
  {
    const float *const input = in + k;
    float *const output = out + k;
    float normalized[3];
    float residual[3];
    float rgbd[3];
    int rgbi[3];

    _prepare_lut_input(input, normalized, residual, rgbd, rgbi, level, safe_normalization);

    const int color = rgbi[0] + rgbi[1] * level + rgbi[2] * level2;
    const int i000 = color * 3;
    const int i100 = i000 + 3;
    const int i010 = (color + level) * 3;
    const int i110 = i010 + 3;
    const int i001 = (color + level2) * 3;
    const int i101 = i001 + 3;
    const int i011 = (color + level + level2) * 3;
    const int i111 = i011 + 3;

    if(rgbd[1] > rgbd[0] && rgbd[2] > rgbd[0])
    {
      output[0] = clut[i000] + (clut[i111] - clut[i011]) * rgbd[0] + (clut[i010] - clut[i000]) * rgbd[1]
                  + (clut[i001] - clut[i000]) * rgbd[2]
                  + (clut[i011] - clut[i001] - clut[i010] + clut[i000]) * rgbd[1] * rgbd[2];
      output[1] = clut[i000 + 1] + (clut[i111 + 1] - clut[i011 + 1]) * rgbd[0]
                  + (clut[i010 + 1] - clut[i000 + 1]) * rgbd[1]
                  + (clut[i001 + 1] - clut[i000 + 1]) * rgbd[2]
                  + (clut[i011 + 1] - clut[i001 + 1] - clut[i010 + 1] + clut[i000 + 1]) * rgbd[1] * rgbd[2];
      output[2] = clut[i000 + 2] + (clut[i111 + 2] - clut[i011 + 2]) * rgbd[0]
                  + (clut[i010 + 2] - clut[i000 + 2]) * rgbd[1]
                  + (clut[i001 + 2] - clut[i000 + 2]) * rgbd[2]
                  + (clut[i011 + 2] - clut[i001 + 2] - clut[i010 + 2] + clut[i000 + 2]) * rgbd[1] * rgbd[2];
    }
    else if(rgbd[0] > rgbd[1] && rgbd[2] > rgbd[1])
    {
      output[0] = clut[i000] + (clut[i100] - clut[i000]) * rgbd[0] + (clut[i111] - clut[i101]) * rgbd[1]
                  + (clut[i001] - clut[i000]) * rgbd[2]
                  + (clut[i101] - clut[i001] - clut[i100] + clut[i000]) * rgbd[0] * rgbd[2];
      output[1] = clut[i000 + 1] + (clut[i100 + 1] - clut[i000 + 1]) * rgbd[0]
                  + (clut[i111 + 1] - clut[i101 + 1]) * rgbd[1]
                  + (clut[i001 + 1] - clut[i000 + 1]) * rgbd[2]
                  + (clut[i101 + 1] - clut[i001 + 1] - clut[i100 + 1] + clut[i000 + 1]) * rgbd[0] * rgbd[2];
      output[2] = clut[i000 + 2] + (clut[i100 + 2] - clut[i000 + 2]) * rgbd[0]
                  + (clut[i111 + 2] - clut[i101 + 2]) * rgbd[1]
                  + (clut[i001 + 2] - clut[i000 + 2]) * rgbd[2]
                  + (clut[i101 + 2] - clut[i001 + 2] - clut[i100 + 2] + clut[i000 + 2]) * rgbd[0] * rgbd[2];
    }
    else
    {
      output[0] = clut[i000] + (clut[i100] - clut[i000]) * rgbd[0] + (clut[i010] - clut[i000]) * rgbd[1]
                  + (clut[i111] - clut[i110]) * rgbd[2]
                  + (clut[i110] - clut[i100] - clut[i010] + clut[i000]) * rgbd[0] * rgbd[1];
      output[1] = clut[i000 + 1] + (clut[i100 + 1] - clut[i000 + 1]) * rgbd[0]
                  + (clut[i010 + 1] - clut[i000 + 1]) * rgbd[1]
                  + (clut[i111 + 1] - clut[i110 + 1]) * rgbd[2]
                  + (clut[i110 + 1] - clut[i100 + 1] - clut[i010 + 1] + clut[i000 + 1]) * rgbd[0] * rgbd[1];
      output[2] = clut[i000 + 2] + (clut[i100 + 2] - clut[i000 + 2]) * rgbd[0]
                  + (clut[i010 + 2] - clut[i000 + 2]) * rgbd[1]
                  + (clut[i111 + 2] - clut[i110 + 2]) * rgbd[2]
                  + (clut[i110 + 2] - clut[i100 + 2] - clut[i010 + 2] + clut[i000 + 2]) * rgbd[0] * rgbd[1];
    }

    _finish_lut_output(input, output, residual, safe_normalization);
  }
}

void dt_lut3d_apply(const float *const in, float *const out, const size_t pixel_nb, const float *const clut,
                    const uint16_t level, const float normalization,
                    const dt_lut3d_interpolation_t interpolation)
{
  switch(interpolation)
  {
    case DT_LUT3D_INTERP_TRILINEAR:
      dt_lut3d_trilinear_interp(in, out, pixel_nb, clut, level, normalization);
      return;
    case DT_LUT3D_INTERP_PYRAMID:
      dt_lut3d_pyramid_interp(in, out, pixel_nb, clut, level, normalization);
      return;
    case DT_LUT3D_INTERP_TETRAHEDRAL:
    default:
      dt_lut3d_tetrahedral_interp(in, out, pixel_nb, clut, level, normalization);
      return;
  }
}
