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

// Per-region gather/composite + the region reconstruction driver (CPU + OpenCL).
// Public API of this highlights harmonic-transposition module (a compiled TU). Include
// this header to call into the module; internals are static in the .c. See common.h.

#include "common/opencl.h"
#include "iop/highlights/common.h"
#include <stdint.h>

void _region_guided_filter(float *const restrict interp, const float *const restrict mask,
                           const float *const restrict depth, const int width, const _hl_region_t *const region,
                           const dt_dev_pixelpipe_t *pipe, const float solid_color, const int max_iter,
                           const float noise_level, const float floor_gate);

#if defined(HAVE_OPENCL) && DT_HL_COEFF_FIELD && DT_HL_SPARSE_SOLVE && (DT_HL_ANISO_SOLVER == 2)
// CPU offload of one region inside the GPU middle: gather the padded window to host, run the
// production CPU reconstruction on it (coordinates translated to the window), scatter back.
// Sequentially consistent with the device regions: the blocking readback drains the in-order
// queue, and the unpack rewrites the exact window the CPU read.
//
// MATHS BRIDGE -- runs the full CPU _region_guided_filter on this window, so ALL stages including
// the Step-7 all-clip core (biharmonic dome x screened-Poisson chroma) and the Step-8 anisotropic
// div(D grad r)=0 chroma coherence are the CPU versions annotated in the CPU stages.
// Used for regions too small for the ~1000-launch GPU path to pay off; no maths of its own.
cl_int _region_cpu_offload_cl(const int devid, void *gd_void, cl_mem interp, cl_mem mask, cl_mem depth,
                              const int width, const _hl_region_t *const region, const dt_dev_pixelpipe_t *pipe,
                              const float solid_color, const int max_iter, const float noise_level,
                              const float floor_gate);

// MATHS/PIPELINE BRIDGE -- device-resident per-region reconstruction, the GPU twin of the CPU
// _region_guided_filter (article §"The algorithm", steps 3-8, and §"The OpenCL pipe": a big region
// stays resident on the device for its whole rebuild). Same step composition as the CPU header, each
// step a device stage minimizing the same energy:
//   3 colour-line coefficient field  _cf_stage_cl            (E_affine per pixel + E_transport diffusion)
//   4 HF refit                       _hf_stage_cl            (detail-band re-fit on the colour line)
//   5-6 floors + gated self-dome     _selfdome_stage_cl      (E_bihar self-dome, depth-gated by We = R^4)
//   7 all-clip luminance dome+chroma _joint_core_stage_cl    (E_bihar dome x E_chrominance screened Poisson)
//   8 anisotropic chroma coherence   _aniso_stage_cl         (E_chrominance div(D grad r)=0)
// The stage parameters (cf_sigma = clip(R/6, 8, 64), deep channel, shared dome grid ds_shared) come from
// ONE on-device reduction (kernel_hl_region_stats) so the queue never drains mid-region; only the 8x256
// reduction partials cross the bus. The reconstructed clipped channels are scattered back on device.
cl_int _region_guided_filter_cl(const int devid, void *gd_void, cl_mem interp, cl_mem mask, cl_mem depth,
                                const int width, const _hl_region_t *const region, const dt_dev_pixelpipe_t *pipe,
                                const float solid_color, const float floor_gate);
#endif
