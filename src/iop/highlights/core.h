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

// Self-dome fallback and all-clip joint core stages (CPU + OpenCL).
// Public API of this highlights harmonic-transposition module (a compiled TU). Include
// this header to call into the module; internals are static in the .c. See common.h.

#include "common/opencl.h"
#include "iop/highlights/common.h"
#include <stdint.h>

void _selfdome(_hl_region_ctx_t *const ctx);

void _joint_core(_hl_region_ctx_t *const ctx);

#if defined(HAVE_OPENCL) && DT_HL_SPARSE_SOLVE
// The hue-coupled self-dome stage on the GPU: soft clip floor (rounded lower bound at the
// clip level), ONE shared biharmonic brightness dome over the union hole, harmonically
// filled chromaticity ratios (each channel divided by the brightness sum), a depth-gated
// blend (dome takes over where the colour-line fit quality is low AND the pixel is shallow),
// then a hard clip-floor re-assert. est/vld/bsc/clip0/dep are device buffers (working pixels,
// validity mask, fit quality, clip thresholds, distance-to-valid depth).
// Mirrors the DT_HL_SELF_DOME block of _region_guided_filter (CPU): any change here must be
// mirrored there and re-validated with the HL_DOMECL_TEST self-test
// (_selfdome_stage_cl_selftest).
//
// MATHS BRIDGE -- article "The algorithm" steps 5-6. Step 5: soft saturation floor (rounded lower
// bound at c0). Step 6: hue-coupled self-dome -- ONE shared biharmonic brightness dome (Delta^2 L_sum
// = 0, hl_soft_floor->hl_lsb_hole->_biharmonic_dome_cl) times harmonically-filled chromaticity ratios
// r_c = est_c/L_sum, blended in by the depth-gated keep weight
//   keep = 1 - dome_fraction,  dome_fraction = (1 - S_{0.4}^{0.85}(R^2)) * exp(-(delta/1.5 sigma)^2)
// (hl_dome_blend), then a hard clip-floor re-assert. The hue coupling (dome_c = L_dome * r_c) is what
// stops three per-channel domes drifting the hue -- the failure that kept the per-channel ancestor off.
cl_int _selfdome_stage_cl(const int devid, void *gd_void, cl_mem estimate, cl_mem valid, cl_mem model_quality,
                          cl_mem clip0, cl_mem depth, const int region_w, const int region_h, const float cf_sigma,
                          const float reg_radius, const int ds_shared, const dt_dev_pixelpipe_t *pipe);

// All-clipped joint core on the device (pixels where NO channel survived, so no guide
// exists): shared biharmonic brightness dome (floored at the saturated sum) x
// screened-Poisson diffused chromaticity (diffusion plus a pull toward the mean valid
// colour, strength set by the "inpaint a flat colour" user slider) -- ONE host symbolic
// analysis + GPU numeric factorization serves the three channels, whose right-hand sides are
// assembled on the device (hl_pde_rhs) so no full-res float plane crosses the bus --
// composed through the gaussian-feathered core mask. The only downloads are the full-res
// hole MASK (bytes; the sparse symbolic analysis needs it anyway) and the per-workgroup
// mean-chromaticity partial sums.
// Mirrors the all-clip joint-core block of _region_guided_filter (CPU, DT_HL_COEFF_FIELD):
// any change here must be mirrored there and re-validated with the HL_CORECL_TEST self-test
// (_joint_core_stage_cl_selftest).
//
// MATHS BRIDGE -- Step 7 all-clip core (article §"Filling holes with no survivor"): magnitude =
// shared biharmonic dome L_dome (Delta^2 L_sum = 0, floored at sum_c clip0_c), chrominance =
// screened-Poisson rim fill r_c ((lambda_solid*I - Delta) r = lambda_solid*r_target) with
// r_target = mean valid chromaticity, solved per channel by ONE shared Cholesky factor (direct) or
// the on-device CG (large cores). Recombine core_c = L_dome * (r_c / sum_j r_j), composed through a
// gaussian-feathered core mask (no hard hand-off at the core rim).
cl_int _joint_core_stage_cl(const int devid, void *gd_void, cl_mem estimate, cl_mem valid, cl_mem clip0,
                            const int region_w, const int region_h, const float solid_color,
                            const float reg_radius, const int extent, const dt_dev_pixelpipe_t *pipe);
                            
#endif
