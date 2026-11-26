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

#ifndef DT_PIXEL_HEAL_DUST_H
#define DT_PIXEL_HEAL_DUST_H

#include "pixel/heal.h"

/**
 * @file heal_dust.h
 * @brief Repair a speck of dust or a scratch from its border and the grain of a source.
 *
 * A heal (dt_heal()) carries the whole source over, its shapes with its grain. A speck or a scratch
 * is repaired best by the harmonic interpolation of its border, which brings no shape in, with the
 * source's grain alone on it (doc/iop-notes.md, the dust measurements).
 */

/** How a speck or a scratch is repaired; every setting comes from the calling module. */
typedef struct dt_heal_dust_t
{
  /** the relaxation, and the domain the repair is solved in */
  dt_heal_solver_t solver;
  /** the source's detail finer than a box of this radius is carried over, at least 1 */
  int texture_radius;
} dt_heal_dust_t;

/**
 * @brief Repair the pixels of `mask` in `dest`, from their border and the grain of `src`.
 *
 * The source's detail finer than a box of dt_heal_dust_t.texture_radius is carried over the
 * harmonic interpolation of the destination's border, in the solver's domain.
 *
 * @param src the source, its pixels aligned with those of `dest`, four channels
 * @param dest the destination, four channels, repaired in place where `mask` is not 0
 * @param mask one channel, the same size
 * @param width @param height the size of the three buffers
 * @param repair the settings
 * @return 0, or 1 out of memory, `dest` left as it came
 */
int dt_heal_dust(const float *src, float *dest, const float *mask, int width, int height,
                 const dt_heal_dust_t *repair);

#ifdef HAVE_OPENCL
#include <CL/cl.h> // cl_mem, cl_int: used below

/** dt_heal_dust() on two device buffers, through the host. */
cl_int dt_heal_dust_cl(int devid, cl_mem dev_src, cl_mem dev_dest, const float *mask, int width, int height,
                       const dt_heal_dust_t *repair);
#endif

#endif // DT_PIXEL_HEAL_DUST_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
