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

// Coefficient-field colour-line transport + HF-refit stage (CPU + OpenCL).
// Public API of this highlights harmonic-transposition module (a compiled TU). Include
// this header to call into the module; internals are static in the .c. See common.h.

#include "common/opencl.h"
#include "iop/highlights/common.h"
#include <stdint.h>

// Single-plane convenience wrapper (isotropic callers and lone planes).
void _cf_harmonic_fill(float *const restrict val, const uint8_t *const restrict hole, const int region_w,
                       const int region_h, const int base_ds, const float *const restrict steer,
                       const dt_dev_pixelpipe_t *pipe);

// ===== coefficient-field reconstruction (see the DT_HL_COEFF_FIELD macro comment) =====
// MATHS BRIDGE -- article "The algorithm" step 3, "The coefficient field". The windowed weighted
// least squares (a,b,d)(x) = argmin_{a,b,d} Sum_y w(y) G_sigma(x-y) (v(y) - a u1(y) - b u2(y) - d)^2,
// with v the clipped channel, u1/u2 its two guides, w the trust mask (all channels valid), and
// G_sigma a Gaussian window at a single scale sigma = clip(r/6, 8, 64). The evaluation is
// v_hat(x) = a(x) u1(x) + b(x) u2(x) + d(x). Rather than solve per pixel, this fits from TEN blurred
// moment planes (1 trusted-mass count + 3 means + 6 second moments, gathered in three 4-channel
// Gaussian blurs) through the 2x2 normal equations, then TRANSPORTS the coefficient planes into the
// hole with _cf_harmonic_fill (E_transport) before evaluating against the measured guides.
// This block owns the whole fit+transport+evaluation; the HF-refit, self-dome and core stages follow.
void _cf_reconstruct(_hl_region_ctx_t *const ctx);

#ifdef HAVE_OPENCL

// Single-plane convenience wrapper (isotropic callers and lone planes).
cl_int _cf_harmonic_fill_cl(const int devid, void *gd_void, cl_mem val, cl_mem hole, const int region_w,
                            const int region_h, const int base_ds, const int mask_is_hole, cl_mem steer);

// ---- coefficient-field JOINT stage on the GPU (pattern-setter for the per-pixel port) ----
// One "fit clipped channel c from the two guides g1/g2" pass: fit the colour-line coefficients
// per pixel from the pre-blurred windowed moments (local means and channel-product averages),
// harmonically diffuse the coefficient fields across the clipped zone, then evaluate the
// prediction against the measured guides and write the result into est (also updating the
// fit-quality score bsc).
// est/vld/bsc are float4 buffers (rn * 4); moments go through image2d for the CL blur; the
// diffused coefficients live in single-channel buffers feeding _cf_harmonic_fill_cl.
// Mirrors the joint coefficient-field stage inside _region_guided_filter (CPU): any change here
// must be mirrored there and re-validated with the HL_CFCL_TEST self-tests
// (_cf_joint_stage_cl_selftest / _cf_stage_cl_selftest).
//
// MATHS BRIDGE -- article "The algorithm" step 3, one two-guide colour-line: fit (a,b,d) at every pixel
// from the blurred moments through the 2x2 normal equations (hl_cf_fit_joint), transport the coefficient
// planes with the E_transport fill (_cf_harmonic_fill_cl), then evaluate v_hat = a*u1 + b*u2 + d against
// the measured guides (hl_cf_eval_joint). R^2 is diffused as a fourth plane on the broader anchor mask.
cl_int _cf_joint_stage_cl(const int devid, void *gd_void, cl_mem estimate, cl_mem valid, cl_mem model_quality,
                          cl_mem mom0, cl_mem mom1, cl_mem mom2, cl_mem steer,
                          const float *const restrict channel_means, const int region_w, const int region_h,
                          const float cf_sigma, const float cf_fmin, const int c, const int guide1,
                          const int guide2);

// The COMPLETE coefficient-field stage on the GPU: joint fits (predict each clipped channel
// from the other two) with the deep channel deferred, pair fallbacks (single-guide fits),
// then the deferred deep-channel evaluation with the depth-split blend. cdeep is the channel
// with the most clipped pixels (host-side decision from region metadata); it is evaluated
// last so its guides are already reconstructed.
// Inputs: est (working red/green/blue/norm pixels), vld (validity mask), bsc (fit-quality
// score), lsb (frozen brightness plane for the fit weights); all float4 device buffers.
// Mirrors the coefficient-field stage of _region_guided_filter (CPU): any change here must be
// mirrored there and re-validated with the HL_CFCL_TEST self-tests (_cf_stage_cl_selftest).
//
// MATHS BRIDGE -- article "The algorithm" step 3 end to end, the GPU driver of the coefficient field:
// pack + Gaussian-blur the ten windowed moment planes ONCE (three 4-channel blurs via hl_cf_pack_joint),
// run the two-guide joint fits (deep channel deferred), the single-guide pair fallbacks, then the
// deferred deep-channel evaluation with the depth-split blend. sigma = clip(r/6, 8, 64), fit windows
// weighted by w = [all valid] * soft occlusion weight (cf_binv); moments centred on channel_means to
// avoid the E[u^2]-E[u]^2 float cancellation.
cl_int _cf_stage_cl(const int devid, void *gd_void, cl_mem estimate, cl_mem valid, cl_mem model_quality,
                    cl_mem luminance, cl_mem steer, const float *const restrict channel_means,
                    dt_gaussian_cl_t *gaussian, const int region_w, const int region_h, const float cf_sigma,
                    const float cf_fmin, const float cf_binv, const int cdeep);

// High-frequency (detail band) hybrid stage on the GPU: one lowpass blur of estimate (the detail
// band is estimate minus this lowpass), windowed moments of the detail band, per-channel gains
// shrunk by the fit quality (chan_a weak colour-line must not print the guides' fine texture),
// gains diffused across the clipped zone, minimum-energy blend of guided resynthesis vs the
// damped detail at strict targets, damped-only treatment at single-guide pixels.
// Mirrors the DT_HL_HF_GUIDE block of _region_guided_filter (CPU): any change here must be
// mirrored there and re-validated with the HL_HFCL_TEST self-test (_hf_stage_cl_selftest).
//
// MATHS BRIDGE -- Step 4 (HF refit), article §"Hybrid Laplacian-band guiding of the high frequencies"
// / §"Rebuild the high frequencies": split estimate at sigma/4 into low band ubar (lowpass) and detail
// u - ubar; fit the detail band's OWN colour-line with R^2-shrunk gains (hl_hf_fit: gain *= R^2, the
// correct shrinkage on a zero-mean band), transport the gains with the E_transport fill, then blend the
// guided resynthesis h_g = a(u_g1-ubar_g1)+b(u_g2-ubar_g2) against the R^2-damped transfer
// h_d = R^2 (u_c - ubar_c) by quadratic min-energy odds w = e_d^2/(e_d^2 + e_g^2), e_{d,g} = blurred
// |HF| (hl_hf_energy + hl_hf_eval) -- a guide misfire spikes e_g so the damped path self-selects.
// Single-guide pixels keep only the damped detail (hl_hf_damp). The band split blurs at sigma/4 (floored
// at 2 px) while the moments blur at the fit's cf_sigma -- the two scales are deliberately different.
cl_int _hf_stage_cl(const int devid, void *gd_void, cl_mem estimate, cl_mem valid, cl_mem model_quality,
                    cl_mem luminance, cl_mem steer, dt_gaussian_cl_t *gaussian, const int region_w,
                    const int region_h, const float cf_sigma, const float cf_fmin, const float cf_binv);

#endif // HAVE_OPENCL
