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

#ifndef DT_IOP_HIGHLIGHTS_KNEE_H
#define DT_IOP_HIGHLIGHTS_KNEE_H

// R9 sensor-rolloff (knee) estimation and inversion (CPU + OpenCL).
// Public API of this highlights harmonic-transposition module (a compiled TU). Include
// this header to call into the module; internals are static in the .c. See common.h.

#include "common/gaussian.h"
#include "iop/highlights/blur.h"
#include "iop/highlights/common.h"
#include <string.h>

static inline void _knee_blur(const float *const restrict in, float *const restrict out, const int width,
                              const int height, const float sigma)
{
  dt_gaussian_t *const gaussian = _hl_gauss_get(width, height, 1, sigma); // cached handle, do not free

  if(!gaussian)
  {
    memcpy(out, in, (size_t)width * height * sizeof(float));
    return;
  }

  dt_gaussian_blur(gaussian, in, out);
}

// Step 2 (article "The algorithm", Sensor rolloff (knee) inversion): build the per-channel inverse
//   k^-1(v) = v + median{ v_hat_i - v_i | v_i in bin(v) }
// from the image itself. v_i are the measured band pixels; v_hat_i is what a windowed colour-line
// regression predicts each SHOULD read from its still-trusted neighbouring channels; the per-bin
// median of the votes v_hat_i - v_i is the accepted lift.
// Works on the RAW CFA `input`, NOT the bilinear-demosaiced buffer: the demosaic samples each
// channel through a different spatial filter per Bayer phase, and that alternating error is the same
// size as the knee signal -- it decorrelates the colour-lines and kills the estimate. A 2x2 quad
// binning of the CFA instead gives co-located R / mean-G / B per cell with no inter-site
// interpolation (the "quad-binned copy of the raw mosaic" of the article). `clipval_raw` is the
// clip level per channel in raw units (= clips / DET). Writes 3 curves (engaged = 0 means
// identity). Never fails: any shortage of data or memory returns identity.
void _hl_knee_estimate(const float *const restrict input, const size_t width, const size_t height,
                       const uint32_t filters, const dt_iop_roi_t *const roi_in, const uint8_t (*const xtrans)[6],
                       const dt_aligned_pixel_t clipval_raw, _hl_knee_curve_t curves[3],
                       const dt_dev_pixelpipe_t *pipe);

// Step 2 application on the demosaiced [R,G,B,norm] buffer: for each engaged channel whose value
// lies in the band, replace v by k^-1(v) = v + L(v). Keeps the norm channel consistent with the
// corrected raw RGB (the "correction applied to the reconstruction anchors" of the article).
void _hl_knee_apply_interpolated(float *const restrict interpolated, const size_t npix,
                                 const dt_aligned_pixel_t clipvaln, const dt_aligned_pixel_t wb4,
                                 const _hl_knee_curve_t curves[3]);

// Apply the engaged curves to a CFA copy (raw units) so the final composition hands the corrected
// band values to the output; unclipped/clipped values pass through untouched.
void _hl_knee_apply_cfa(const float *const restrict input, float *const restrict input_corr, const size_t width,
                        const size_t height, const uint32_t filters, const dt_iop_roi_t *const roi_in,
                        const uint8_t (*const xtrans)[6], const dt_aligned_pixel_t clipval_raw,
                        const _hl_knee_curve_t curves[3]);

#ifdef HAVE_OPENCL
// Step 2 (article "The algorithm", knee inversion) on the GPU: build the per-channel inverse
//   k^-1(v) = v + median{ v_hat_i - v_i | v_i in bin(v) },
// v_hat from a windowed colour-line regression v_hat = a*u1 + b*u2 + d (joint) or a*u + d (single).
// GPU knee estimation (sensor saturation-rolloff curve, see the DT_HL_KNEE macro comment):
// the device runs Phase A (colour-filter-array binning, the 5-sigma windowed moment blurs --
// packed 4 planes per gaussian pass -- and the colour-line regressions producing v_hat/R^2); the
// host keeps Phase B (vote medians + significance gate + monotone curve fit) on the downloaded
// BINNED planes (<= 1.5 Mpx grid, x/pred/r2s/done only). The full-res raw mosaic never crosses the
// bus. Mirrors _hl_knee_estimate on the CPU: any change here must be mirrored there and
// re-validated with the HL_KNEECL_TEST self-test (_knee_cl_selftest).
cl_int _hl_knee_estimate_cl(const int devid, void *gd_void, cl_mem dev_in, const size_t width, const size_t height,
                            const uint32_t filters, const dt_iop_roi_t *const roi_in, cl_mem dev_xtrans,
                            const int is_xtrans, const dt_aligned_pixel_t clipval_raw, _hl_knee_curve_t curves[3],
                            const dt_dev_pixelpipe_t *pipe);

// Step 2 application on the device: replace each engaged-channel band pixel v of the raw mosaic by
// k^-1(v) = v + L(v) (the hl_knee_apply kernel does the per-pixel curve lookup). The per-channel
// lift[] knot arrays are flattened and uploaded; engaged flags gate which channels are corrected.
// Mirrors _hl_knee_apply_cfa on the CPU: any change here must be mirrored there and
// re-validated with the HL_KNEECL_TEST self-test (_knee_cl_selftest).
cl_int _hl_knee_apply_cfa_cl(const int devid, void *gd_void, cl_mem dev_in, cl_mem dev_out, const size_t width,
                             const size_t height, const uint32_t filters, const dt_iop_roi_t *const roi_in,
                             cl_mem dev_xtrans, const int is_xtrans, const dt_aligned_pixel_t clipval_raw,
                             const _hl_knee_curve_t curves[3]);
#endif
#endif // DT_IOP_HIGHLIGHTS_KNEE_H
