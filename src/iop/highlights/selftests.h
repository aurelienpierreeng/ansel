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

#ifndef DT_IOP_HIGHLIGHTS_SELFTESTS_H
#define DT_IOP_HIGHLIGHTS_SELFTESTS_H

// CPU/GPU parity self-tests of the harmonic OpenCL port (each vs a CPU replica).
// Public API of this highlights harmonic-transposition module (a compiled TU). Include
// this header to call into the module; internals are static in the .c. See common.h.


#ifdef HAVE_OPENCL
// Self-test (enable with HL_SPCL_TEST=1): factor + solve the 13-point biharmonic system on a
// disc hole with both the CPU and the GPU sparse Cholesky solver, print the maximum
// CPU-vs-GPU relative difference. Runs once per process.
void _sp_chol_cl_selftest(const int devid, void *gd_void, const dt_dev_pixelpipe_t *pipe);
#endif

#ifdef HAVE_OPENCL
// Self-test (enable with HL_BLURCL_TEST=1): blur a synthetic 4-channel plane with the CPU and
// the GPU recursive gaussians, print the maximum CPU-vs-GPU absolute difference. Runs once per
// process.
void _region_blur_cl_selftest(const int devid, const dt_dev_pixelpipe_t *pipe);
#endif

#ifdef HAVE_OPENCL
// self-test (HL_FILLCL_TEST=1): fill a synthetic plane with the CPU and GPU coarse-to-fine
// Jacobi, print max abs difference over the filled cells. Run once per process.
void _cf_harmonic_fill_cl_selftest(const int devid, void *gd_void, const dt_dev_pixelpipe_t *pipe);
#endif

#ifdef HAVE_OPENCL
// Self-test (enable with HL_CFCL_TEST=1): compares the GPU joint coefficient-field stage
// (_cf_joint_stage_cl) against an inline CPU replica of the exact production loops (pack,
// blur, fit, fills, eval) on a synthetic correlated region with a green-clipped disc.
// Prints the maximum CPU-vs-GPU absolute difference of est over the evaluated targets.
void _cf_joint_stage_cl_selftest(const int devid, void *gd_void, const dt_dev_pixelpipe_t *pipe);
#endif

#ifdef HAVE_OPENCL
// Self-test (enable with HL_CFCL_TEST=1, part 2): the COMPLETE coefficient-field stage
// (_cf_stage_cl: joint + pair + deep cascade) against an inline CPU replica, on a
// green-clipped disc with an inner red-clipped core (the occluded topology: strict targets,
// multi-clip pair targets and the depth-split blend all exercised). Prints the maximum
// CPU-vs-GPU absolute difference of est and of the fit-quality plane bsc.
void _cf_stage_cl_selftest(const int devid, void *gd_void, const dt_dev_pixelpipe_t *pipe);
#endif

#ifdef HAVE_OPENCL
// Self-test (enable with HL_HFCL_TEST=1): the high-frequency detail-band stage (_hf_stage_cl)
// GPU vs an inline CPU replica, on the two-disc topology with textured content and a synthetic
// fit-quality plane. Prints the maximum CPU-vs-GPU absolute difference.
void _hf_stage_cl_selftest(const int devid, void *gd_void, const dt_dev_pixelpipe_t *pipe);
#endif

#ifdef HAVE_OPENCL
// Self-test (enable with HL_DOMECL_TEST=1): clip floors + hue-coupled self-dome stage
// (_selfdome_stage_cl) GPU vs the CPU production functions, on the two-disc topology with a
// synthetic depth plane. Prints the maximum CPU-vs-GPU absolute difference. If a region dump
// from HL_REG_DUMP exists in /tmp, replays that real region through both dome paths first.
void _selfdome_stage_cl_selftest(const int devid, void *gd_void, const dt_dev_pixelpipe_t *pipe);
#endif

#ifdef HAVE_OPENCL
// Self-test (enable with HL_CORECL_TEST=1): all-clip joint core (_joint_core_stage_cl) GPU vs
// the CPU production path on a synthetic all-clip disc + partial-clip annulus; prints the
// maximum CPU-vs-GPU absolute difference. Runs once per process.
void _joint_core_stage_cl_selftest(const int devid, void *gd_void, const dt_dev_pixelpipe_t *pipe);
#endif

#ifdef HAVE_OPENCL
// self-test (HL_ANISOCL_TEST=1): divergence-form aniso chroma GPU vs the CPU production path
// on an all-clip disc with a textured surround, once per process
void _aniso_stage_cl_selftest(const int devid, void *gd_void, const dt_dev_pixelpipe_t *pipe);

// HL_CGRADCL_TEST: gradient-extending chroma stage, GPU vs the CPU _chromaticity_gradient
void _chromaticity_gradient_stage_cl_selftest(const int devid, void *gd_void, const dt_dev_pixelpipe_t *pipe);
#endif

#ifdef HAVE_OPENCL
// Self-test (enable with HL_REGCL_TEST=1): the full GPU region orchestrator
// (_region_guided_filter_cl) vs the production CPU _region_guided_filter on one synthetic
// staggered-clip blob; prints the mean and maximum CPU-vs-GPU absolute difference. Runs once
// per process.
void _region_guided_filter_cl_selftest(const int devid, void *gd_void, const dt_dev_pixelpipe_t *pipe);
#endif

#ifdef HAVE_OPENCL
// Self-test (enable with HL_KNEECL_TEST=1): GPU knee estimate + apply
// (_hl_knee_estimate_cl / _hl_knee_apply_cfa_cl) vs the CPU production path on a synthetic
// Bayer (RGGB) mosaic with a soft saturation rolloff; prints the maximum CPU-vs-GPU
// difference. Runs once per process.
void _knee_cl_selftest(const int devid, void *gd_void, const dt_dev_pixelpipe_t *pipe);
#endif
#endif // DT_IOP_HIGHLIGHTS_SELFTESTS_H
