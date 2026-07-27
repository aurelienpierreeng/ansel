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

// Top-level Bayer/X-Trans CPU drivers and the hybrid OpenCL driver.
// Public API of this highlights harmonic-transposition module (a compiled TU). Include
// this header to call into the module; internals are static in the .c. See common.h.

#include "common/opencl.h"
#include "develop/imageop.h"
#include "iop/highlights/common.h"

int process_harmonic_bayer(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                           const dt_dev_pixelpipe_iop_t *piece, const void *const restrict ivoid,
                           void *const restrict ovoid, const dt_iop_roi_t *const roi_in,
                           const dt_iop_roi_t *const roi_out, const dt_aligned_pixel_t clips);

// CPU driver for Fujifilm X-Trans sensors; same start-to-finish flow as
// process_harmonic_bayer, with X-Trans gather/scatter/knee access.
//
// MATHS/FLOW BRIDGE -- identical 8-step orchestration to process_harmonic_bayer (see its header for the
// step-by-step map): step 1a gather (_interpolate_and_mask_xtrans, through the 6x6 X-Trans bilinear
// lookup) -> step 2 knee (_hl_knee_estimate/_apply, 6x6 binning) -> step 1b depth + segmentation
// (annotated below) -> steps 3-8 per region (_region_guided_filter, CFA-agnostic, shared with Bayer) ->
// remosaic + composite (_remosaic_and_replace_xtrans). Only the CFA-touching endpoints differ.
int process_harmonic_xtrans(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                            const dt_dev_pixelpipe_iop_t *piece, const void *const restrict ivoid,
                            void *const restrict ovoid, const dt_iop_roi_t *const roi_in,
                            const dt_iop_roi_t *const roi_out, const dt_aligned_pixel_t clips);

#ifdef HAVE_OPENCL
// Top-level OpenCL entry point for the harmonic-transposition mode (called from highlights.c
// when the pipe runs on a GPU). First fires the env-gated CPU-vs-GPU self-tests (each a
// no-op unless its HL_*_TEST variable is set), then runs the hybrid driver: GPU gather
// (bilinear interpolation + clip mask + feathering), the reconstruction middle (fully on GPU
// when possible, otherwise on downloaded host planes), and the GPU remosaic. Any GPU failure
// falls back to _harmonic_cl_roundtrip (bit-identical CPU path through a host roundtrip).
//
// MATHS/PIPELINE BRIDGE (article §"The OpenCL pipe"): the hybrid CPU-orchestrated GPU driver. GPU gather
// (step 1a: kernel_highlights_bilinear_and_mask + box_blur -> [R,G,B,norm] planes and clip mask, on
// device) and GPU remosaic (the terminal composite) BRACKET a middle that runs one of three ways, in
// preference order: (1) _harmonic_reconstruct_cl -- steps 2 + 1b + 3-8 device-resident, small regions
// offloaded per the ~1 Mpx routing threshold, only masks/depth/partials crossing the bus; (2) if the
// device middle cannot run (fp64 device, grain requested, oversized hole), the working planes are pulled
// down and _harmonic_reconstruct_host runs the CPU middle, then the reconstruction is uploaded and the
// GPU remosaic still runs; (3) if any GPU gather/remosaic step itself fails, the whole thing falls back
// to _harmonic_cl_roundtrip (bit-identical to the CPU path). The article steps 3-8 maths live in
// _region_guided_filter_cl / _region_guided_filter; this driver is pure host<->device routing glue.
cl_int process_harmonic_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                           const dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
                           const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                           const dt_aligned_pixel_t clips);
#endif
