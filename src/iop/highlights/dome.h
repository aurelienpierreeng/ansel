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

#ifndef DT_IOP_HIGHLIGHTS_DOME_H
#define DT_IOP_HIGHLIGHTS_DOME_H

// Biharmonic luminance dome solve (CPU + OpenCL).
// Public API of this highlights harmonic-transposition module (a compiled TU). Include
// this header to call into the module; internals are static in the .c. See common.h.

#include "iop/highlights/common.h"
#include <stdint.h>

void _biharmonic_dome(float *const restrict field, const uint8_t *const restrict hole, const int region_w,
                      const int region_h, const int forced_downsample, const dt_dev_pixelpipe_t *pipe);

#if defined(HAVE_OPENCL) && DT_HL_SPARSE_SOLVE
// Biharmonic dome (smooth hill continuing the rim brightness into a fully-clipped area,
// smooth in value AND slope) as a GPU unit: coarse-grid reduction on device, tiny
// coarse-metadata download for the symbolic analysis and matrix assembly (<= a few hundred
// KB of COARSE cells, never full-res planes), GPU sparse Cholesky solve, bilinear upsample
// into the full-res holes on device.
// field/hole are full-res device buffers (float / uchar); ds (downsample factor) forced by
// the caller like the CPU _biharmonic_dome's force_ds.
// Mirrors _biharmonic_dome on the CPU: any change here must be mirrored there and
// re-validated with the HL_DOMECL_TEST self-test (_selfdome_stage_cl_selftest).
//
// MATHS BRIDGE -- article "Biharmonic inpainting" / E_bihar: solves Delta^2 u = 0 on the (coarse)
// hole with u|dOmega = u_valid Dirichlet data, so the rim curvature is extended into a smooth dome
// (value AND slope continued). Restricting the SPD biharmonic operator to the hole unknowns gives
// the linear system A u = b factored below by the GPU sparse Cholesky (SPD system, annotated in
// common/solvers/sparse_cholesky.h); the bilinear upsample restores the low-frequency dome to full res.
cl_int _biharmonic_dome_cl(const int devid, void *gd_void, cl_mem field, cl_mem hole, const int region_w,
                           const int region_h, const int downsample, const dt_dev_pixelpipe_t *pipe);
#endif
#endif // DT_IOP_HIGHLIGHTS_DOME_H
