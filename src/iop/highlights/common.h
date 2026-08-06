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

#ifndef DT_IOP_HIGHLIGHTS_COMMON_H
#define DT_IOP_HIGHLIGHTS_COMMON_H

#include "common/openmp.h"        // HL_PFOR/__OMP_PARALLEL_FOR__
#include "common/simd.h"          // dt_aligned_pixel_t + SIMD macros
#include "common/gaussian.h"      // dt_gaussian_t / dt_gaussian_cl_t (in _hl_gauss_slot_t + CL protos)
#include "develop/pixelpipe_hb.h" // dt_dev_pixelpipe_t (in _hl_region_ctx_t) + dt_dev_pixelpipe_iop_t
#include <glib.h>                 // CLAMP
#include <math.h>
#include <stdint.h>

// Shared macros and struct definitions for the highlights harmonic-transposition mode,
// used by both the C (the CPU stages) and OpenCL (process.h / the OpenCL stages)
// halves. This is a textual include unit of highlights.c (see the note in _cpu.h).

/*
    Harmonic transposition: Ansel's highlight-reconstruction method (2026 rebuild).

    Everything specific to the DT_IOP_HIGHLIGHTS_HARMONIC mode lives here: the sensor-rolloff
    knee inversion, the per-region segmentation, the coefficient-field colour-line transport
    (windowed joint fits, harmonic diffusion of the model, deep-channel cascade), the sparse
    SPD Cholesky and the direct solvers built on it (biharmonic dome, screened chroma,
    divergence-form structure-steered chroma), the HF-band hybrid, the CPU drivers for Bayer
    and X-Trans, and the hybrid OpenCL driver (GPU gather/remosaic around the CPU middle).

    This is a textual include unit of highlights.c (not a standalone translation unit): it
    relies on the CFA helpers, gather/scatter, buffer conventions and the global kernel
    handles defined there. See the companion article for the method's derivation.
*/

// =====================================================================================
//  Segmented full-resolution guided-laplacian reconstruction
//
//  Rather than reconstruct the whole downsampled frame with the a-trous wavelet stack,
//  isolate each connected clipped region and run a coarse->fine FULL-VALUE guided filter
//  at full resolution on the small rectangle enclosing it (plus padding that gives the
//  colour-line fit a valid rim). This recovers the clipped channel's magnitude (the
//  intercept carries the DC), not merely its texture, and only touches clipped
//  neighbourhoods. See doc/ and the companion article for the derivation.
//
//  NOTE: this first stage ports the guided-filter "bug fix" only. The confidence blend,
//  the all-clipped joint core and the uncertainty regulariser (all needing a small
//  per-region biharmonic solve) land in a follow-up; all-clipped cores are meanwhile
//  left to the surrounding fill.
// =====================================================================================

// DEBUG TOGGLE: set to 0 to disable the biharmonic + chroma refinement (self-dome, joint
// all-clipped core, flat-colour) and run ONLY the guided-filter stage (increment 1), to
// validate the segmentation + guided path in isolation.
#define DT_HL_BIHARMONIC 1

// BAND OVERRIDE: for channels whose sensor rolloff ENGAGED (knee), extend the clip detection
// down to this fraction of the threshold. The knee's value-map restores the band's level but
// cannot restore a slope the sensor compressed away; the colour-line model, anchored on the
// truly linear data below the band, can -- and each band pixel's knee-lifted measurement acts
// as its per-pixel saturation floor, so the override can only raise, never lose data. On the
// rolloff bench scene this removes the residual contour arc and cuts the zone RMSE 5x
// (0.012 -> 0.0025); on hard-clipping channels (knee not engaged) detection is unchanged and
// the output is bit-identical. Values in [0.7, 1.0); 1.0 disables.
#define DT_HL_BAND_OVR 0.9f

#define HL_PFOR(...) __OMP_PARALLEL_FOR__(__VA_ARGS__)

// Clip-asymmetry gate for the chromaticity-preserving floor family. The per-channel saturation
// floors imprint the CLIP LEVELS' chromaticity on multi-clip pixels wherever the solver
// under-predicts; whether that imprint is plausible scene content depends only on the clip levels
// themselves: with NEAR-EQUAL per-channel clips (unit WB -- every synthetic bench case) the imprint
// is neutral and usually near the truth of a bright core, with strongly UNEQUAL clips (real cameras:
// clips = WB coefficients) the imprint is the inverse-WB magenta, never a plausible emitter. Gate
// the joint (hue-preserving) floor family by the clip asymmetry A = max_c/min_c.
// The ramp starts at 1.25, NOT 1.0: clips inherit processed_maximum, which carries a ~10% non-WB
// wiggle from the input profile handling even at unit white balance (measured A = 1.145 on the
// unit-WB article-bench DNGs, AsShotNeutral = (1,1,1)) -- that regime must keep the approved
// per-channel behavior exactly (g = 0). Real-camera white balance sits at A ~ 2..2.6 (the Bayer
// green photosite is ~2x more sensitive than red/blue), so g = 1 from A >= 2 covers every real
// raw while the dead zone below 1.25 absorbs profile wiggle.
static inline float _hl_floor_gate(const float clips[4])
{
  const float mn = fminf(clips[0], fminf(clips[1], clips[2]));
  const float mx = fmaxf(clips[0], fmaxf(clips[1], clips[2]));
  const float asym = (mn > 1e-9f) ? mx / mn : 1.f;
  const float t = CLAMP((asym - 1.25f) / 0.75f, 0.f, 1.f);
  return t * t * (3.f - 2.f * t);
}

// Trusted-ring validation of the flat-mean colour prior -- the surround-importing refinements'
// own scene assumption ("blown-core colour ~ bright-surround mean colour"). At 1-clip pixels
// (exactly one clipped channel: reconstructed from TWO measured guides, measured
// chromaticity-correct), compare the RING MEAN chromaticity shares to cmean's shares, normalized
// by the ring's own dispersion: t = |ring_mean - cmean|_L1 / max(ring_std_sum, 0.02),
// vote = exp(-(t/5)^2). A self-coloured emitter shifts the whole ring COHERENTLY (measured
// magentasun: bias 0.21 over dispersion 0.007 -> t ~ 10 floored, vote ~ 0), while real scenes
// scatter the ring by noise/texture AROUND the mean (measured MAC/sunrise: t = 0.3..1.5,
// vote ~ 1). A per-pixel agreement average fails here: real-ring spread caps it at ~0.3
// regardless of tolerance, and any tolerance wide enough for real content re-opens the emitter.
// Mirrors py_flat_mean_vote (validate.py); the cgrad ring gate answers a DIFFERENT question
// (continuation of the extended gradient field) and may disagree (MAC: cgrad closes, this opens).
// Serial double accumulation on purpose: deterministic run-to-run; the ring is a small subset.
static inline float _hl_ring_flat_mean_vote(const float *const restrict estimate,
                                            const float *const restrict valid,
                                            const dt_aligned_pixel_t cmean, const size_t region_pixels)
{
  const float cmean_sum = fmaxf(cmean[0] + cmean[1] + cmean[2], 1e-9f);
  const float cmean_share[3]
      = { cmean[0] / cmean_sum, cmean[1] / cmean_sum, cmean[2] / cmean_sum };
  double share_sum[3] = { 0.0, 0.0, 0.0 };
  double share_sq[3] = { 0.0, 0.0, 0.0 };
  double ring_count = 0.0;
  for(size_t i = 0; i < region_pixels; i++)
  {
    const int n_clipped = (valid[i * 4 + 0] < 0.5f) + (valid[i * 4 + 1] < 0.5f) + (valid[i * 4 + 2] < 0.5f);
    if(n_clipped != 1) continue;
    const float sum = fmaxf(estimate[i * 4 + 0] + estimate[i * 4 + 1] + estimate[i * 4 + 2], 1e-9f);
    for(int c = 0; c < 3; c++)
    {
      const double share = (double)(estimate[i * 4 + c] / sum);
      share_sum[c] += share;
      share_sq[c] += share * share;
    }
    ring_count += 1.0;
  }
  if(ring_count <= 0.0) return 0.f; // no trusted ring: the prior stays unproven
  float bias = 0.f;
  float dispersion = 0.f;
  for(int c = 0; c < 3; c++)
  {
    const double mean = share_sum[c] / ring_count;
    bias += fabsf((float)mean - cmean_share[c]);
    dispersion += sqrtf(fmaxf((float)(share_sq[c] / ring_count - mean * mean), 0.f));
  }
  const float t = bias / fmaxf(dispersion, 0.02f);
  const float arg = t / 5.f;
  return expf(-arg * arg);
}

// Per-thread cache of dt_gaussian handles keyed on (width, height, channels, sigma).
// dt_gaussian_init allocates its recursion temporaries on every call, and the region stages
// fire dozens of same-shaped blurs per region (the knee, over a hundred per image): reusing
// the handle removes pure allocation churn. The cache is __thread and the blur calls happen
// serially on the caller's thread (parallelism lives INSIDE dt_gaussian_blur), so no locking.
// Drivers flush it on exit (_hl_gauss_cache_flush) so nothing leaks across pipeline runs.
typedef struct
{
  int width;
  int height;
  int channels;
  float sigma;
  dt_gaussian_t *gaussian;
} _hl_gauss_slot_t;

#define HL_GAUSS_SLOTS 4

// GUIDE-SELECTION TUNABLES -- traced from magenta regressions on real amber/orange highlights.
// DT_HL_PAIR_VARIANCE: 1 = score guides by the pair-restricted variance already computed for the fit
//   where valid pairs are scarce -- e.g. the thin valid surround of a saturated highlight).
// DT_HL_GUIDE_GATE: may a CLIPPED channel guide through its clip value? On amber/orange highlights
//   the only surviving channel is a LOW blue -- a poor guide that collapses green to magenta -- while
//   the clipped red at its clip level pulls green back up. But allowing that everywhere magentas the
//   shallow annulus (there a clipped guide's clip value is wrong). So gate it by DEPTH:
//   2 = DEPTH-GATED (default): clipped guide allowed only where dep >= 0.5 * region radius (the
//       saturated core/inner ring -> recovers amber); the shallow annulus keeps valid-only (accurate).
//   1 = never (prototype: annulus good, core magenta/dark);  0 = always (core good, annulus magenta).
#define DT_HL_PAIR_VARIANCE 1
#define DT_HL_GUIDE_GATE 2

// DT_HL_CLIP_FLOOR: a clipped channel SATURATED, so its true value is >= its clip level. If the
// colour-line fit (from a low surviving guide) comes out below that, floor it back up. Physical,
// parameter-free, and monotone (only raises below-clip values -> no overshoot, no per-pixel guide
// switching -> no patchwork). Fixes the amber core/ring collapsing to a dark magenta without the
// clipped-guide hacks (DT_HL_GUIDE_GATE 0/2), which magenta the annulus / stain the core.
#define DT_HL_CLIP_FLOOR 1

// Max unknowns in the dense-Cholesky biharmonic dome before it downsamples (O(N^3)). Larger = finer
// dome grid (ds->1 = exact full-res) at more cost -- raise it to test if the coarse solve matters.
#define DT_HL_DOME_NMAX 2000

// With the sparse direct solver the dome grid can be MUCH finer at less cost than the dense
// O(N^3) solve ever allowed: coarse-grid unknowns cap for the sparse path (ds -> 1 up to this).
#define DT_HL_DOME_NMAX_SPARSE 8192

// REFINEMENT stages inside the biharmonic block. The joint core (all-clip magnitude dome + diffused
// chroma) is ALWAYS on -- it recovers the sun disc. The per-channel SELF-DOME and SEAM-REG were
// disabled after they turned a correctly-guided amber annulus magenta. That was traced NOT to the
// per-channel method (the RGB prototype validates it: it never magentas, even on a 59%-clipped amber
// sun) but to three C-only divergences from the prototype, now fixed:
//   1. the self-dome domed each channel at its OWN downsample factor -> per-channel-inconsistent
//      approximation -> chroma drift. Now all three share ONE ds (sized from the union hole).
//   2. the seam regulariser ran only "iterations" (~30) CG steps on an ill-conditioned biharmonic;
//      each channel stopped at a different point -> chroma drift. Now runs the full CG budget.
//   3. the confidence R^2 was gated to 0 in low-valid-weight regions, starving the seam reg of data
//      fidelity (Wd = R^4 -> 0) at the all-clip core. Gate removed (matches the prototype).
// Also: the saturation floor is now re-asserted AFTER the self dome (the prototype floors there).
// Re-enabled for real-image A/B; flip either to 0 to isolate. They help genuinely decorrelated
// content (rare in natural images) and are near no-ops on correlated content.
#define DT_HL_SELF_DOME 1

// Structure-steered chroma post-pass: keep the ladder's MAGNITUDE (norm), re-diffuse the clipped
// channels' RATIOS along the isophotes of the recovered luminance, coarse-to-fine so the diffusion
// seeds the entire hole (unreached interior would keep its magenta ratios). Fixes the guide-flip /
// scale hand-off chroma patches without touching the recovery. See _aniso_tensor/_aniso_iterate.
#define DT_HL_ANISO_CHROMA 1

// R9: blind sensor-rolloff (knee) pre-correction. Real sensors compress the last few percent below
// the clip level (saturation rolloff), so the near-clip BAND holds values biased LOW. Every
// reconstruction hand-off against that biased data seams (R2-R8 lesson: seam energy = estimator
// disagreement, and no weighting can hide it) -- the only zero-seam fix is to DEBIAS THE BAND DATA
// ITSELF. A windowed colour-line fitted on fully-trusted pixels predicts what each band value should
// be; binned robust medians of (measured, predicted) pairs trace the knee inverse, which is applied
// to the band before reconstruction. On hard-clipped (unbiased) data the fitted curve is the
// identity, so the correction has a NO-OP GUARANTEE. Estimation is global per channel (the knee is
// a sensor property) and runs on a downsampled copy; like the Laplacian normalization it is local
// to the tile being processed.
#define DT_HL_KNEE 1

// COEFFICIENT-FIELD reconstruction (replaces the guided ladder when 1). The PK1 step study
// showed the ladder's coarse scales write FLAT fills into the deep zone (heterogeneous windows
// attenuate the fitted slope toward the window mean) in depth-level-set annuli whose boundaries
// are the visible hard arcs (scale hand-offs between disagreeing estimators). Values
// extrapolated from far are unstable -- but the local colour-line COEFFICIENTS are smooth by
// nature. So: fit est_c = a*g1 + b*g2 + d in windows where channel c is trusted, harmonically
// diffuse the coefficient fields (a, b, d) across the clipped zone, and evaluate against the
// MEASURED valid guides at every pixel. The clipped channel inherits the guides' true structure;
// no scales, no depth gates, no level-set writes -> no seams by construction.
//
// Around the core fit, four hand-off-free safeguards (each validated on the synthetic bench):
//  - the fit quality R^2 is diffused alongside the coefficients and scales the estimate's
//    HIGH FREQUENCIES (a weak colour-line must not print the guides' fine texture);
//  - a DEPTH-GATED per-channel self-dome takes over where the model is doubtful (low R^2) AND
//    the dome is trustworthy (shallow: biharmonic extrapolation degrades with distance) --
//    depth is the only reliable arbiter between correlated transfer and decorrelated printing,
//    which is undecidable from rim statistics alone;
//  - the clip floor is SOFT (rounded over ~2% of the clip level), so a prediction oscillating
//    around saturation does not print the binding contour;
//  - the all-clip core rebuild is FEATHERED into the surrounding reconstruction over a blurred
//    mask instead of a hard hand-off. All-clip pixels have no guides: they stay at the clip
//    floor and the joint core rebuilds them, with the aniso chroma pass restricted to them
//    (coefficient-field results act as anchors).
#define DT_HL_COEFF_FIELD 1

// Laplacian-band (HF) guiding, hybridized: the detail band gets its OWN windowed colour-line
// (R^2-shrunk gains -- on a zero-mean band shrinkage is the correct estimator), and the
// reconstruction's high frequencies are blended between this guided resynthesis and the
// R^2-damped transfer by QUADRATIC MIN-ENERGY odds: a mixed-window gain misfiring at an object
// edge shows up as an HF energy spike, so the failure detects itself and the damped path takes
// over there. Restores the 2021 method's namesake where it is measurably right (texture whose
// detail-band correlation is real) at no cost where it is not.
#define DT_HL_HF_GUIDE 1

// The coefficient-field pipeline is CFA-agnostic (it works on the interpolated RGB planes and
// masks): Bayer and X-Trans share the whole reconstruction and differ only in the gather
// (bilinear interpolation), the scatter (remosaic) and the knee's raw-mosaic access.

typedef struct
{
  int x0, y0, x1, y1;     // inclusive bbox of the clipped pixels
  int rx0, ry0, rx1, ry1; // padded read box (clamped to image), context for the guide
  int pad;                // padding width = ceil(radius)
  float radius;           // reconstruction radius = deepest clip-to-valid distance in this region
} _hl_region_t;

// Anisotropic transport of the coefficient planes (production default, CPU + OpenCL, parity-
// tested by the HL_FILLCL_TEST aniso leg). The coefficient fills are steered by the measured
// guide structure through a variance-adaptive tensor (_cf_adaptive_tensor below): where a HARD
// EDGE crosses the blown zone, transport runs along the isophotes (a boundary means the content
// beyond follows another colour-line -- do not mix models across it); on a clean halo ramp it
// runs along the steepest gradient (the model lives on the rim and must travel radially inward).
// Validated on the 6 ground-truth scenes (never worse than the isotropic fill; pk1synth -7%,
// occluded -2% RMSE at equal convergence) and on natural-raw A/B (visually structureless).
//
// DT_HL_CF_K = the relative-std threshold of the edge detector. Fine-sweep optimum: every
// ground-truth scene at or below the isotropic RMSE across k in [0.14, 0.25]; the occluded
// scene improves monotonically toward low k (the isophote lean engages earlier on the boundary)
// while the correlated scene's radial gain evaporates below ~0.12 -- 0.15 takes the boundary
// win with margin from that frontier.
#define DT_HL_CF_K 0.15f

// max planes sharing one anchor mask in the fused GPU harmonic fill
#define DT_HL_FILL_CL_MAXP 3

// exact sparse SPD Cholesky direct solvers (dome, region PDE); iterative fallback kept.
// The factor structs and the solvers themselves live in the reusable libraries
// common/solvers/sparse_cholesky.h (CPU) and common/solvers/sparse_cholesky_cl.h (GPU).
#define DT_HL_SPARSE_SOLVE 1

// cap on the direct-solve size (number of hole unknowns) for the full-resolution diffusion
// systems; beyond it the iterative conjugate-gradient fallback runs (the factor's memory
// grows as O(N log N) and its arithmetic as O(N^1.5))
#define DT_HL_SPARSE_MAX (1 << 14)

// anisotropic chroma solver selector (2 = divergence-form exact/pyramid; see _cpu.h options)
#define DT_HL_ANISO_SOLVER 2

// Fusing the planes matters: the mask pyramid, the tensor and the edge weights depend only on
// (hole, steer, geometry), so np planes share ONE build and ONE sweep pass reads the weights
// once per cell for np accumulations. Per plane, the arithmetic is identical to np separate
// fills (same weights, same accumulation order).
#define DT_HL_FILL_MAXP 4

// whose fixed cost dwarfs the arithmetic on small windows -- a 24x29 region measured 22 ms on
// device vs <1 ms on host. The window crosses the bus once in each direction (pack kernel +
// one readback, one upload + unpack kernel), so the traffic is 9*rn floats down, 4*rn up.
// Threshold overridable via HL_CL_CPU_PX for tuning.
#define DT_HL_CL_CPU_REGION_PX (1u << 20)

#define DT_HL_KNEE_LO 0.80f      // trust threshold: values below are assumed strictly linear
#define DT_HL_KNEE_DET 0.995f    // clip-detection threshold in clip units
#define DT_HL_KNEE_BINS 24       // curve resolution over the band
#define DT_HL_KNEE_FMIN 0.02f    // minimum trusted mass a stats window must hold
#define DT_HL_KNEE_R2MIN 0.25f   // minimum colour-line fit quality for a pair to vote
#define DT_HL_KNEE_MINVOTES 100  // minimum votes per bin: no evidence -> identity (safe default)
#define DT_HL_KNEE_NSIGMA 2.0f   // lift must exceed NSIGMA * standard error of the bin median
#define DT_HL_KNEE_ENGAGE 0.005f // curves lifting less than this are noise: stay identity
#define DT_HL_KNEE_NSIGMAS 5     // multi-scale stats windows, finest with trusted mass wins

typedef struct _hl_knee_curve_t
{
  int engaged;                 // 0 = identity (no correction for this channel)
  float lift[DT_HL_KNEE_BINS]; // additive lift per bin center, clip-normalized units
} _hl_knee_curve_t;

// ---------------------------------------------------------------------------------------------
// Per-region working-set handle shared by the CPU reconstruction stages (region.h et al.).
// MATHS/FLOW BRIDGE -- per-region reconstruction (article §"The algorithm", steps 3-8), the whole
// second half of the mermaid flowchart run once per merged region Omega on its PADDED read window
// (article §"The C production code": each region is cropped to region->rx0..ry1, reconstructed in a
// contiguous rw x rh buffer, then scattered back -- so the cost is linear in the padded area, not the
// image, article §"Linear in the padded area"). The stages compose as:
//   3 colour-line coefficient field   (_region fit+transport+eval block below; minimizes E_affine per
//                                       pixel, then transports the coefficients by E_transport) ->
//   4 HF refit                        (re-fits the high-frequency detail band on the same colour line) ->
//   5-6 soft floors + self-dome       (5: clip-level floor so a fit can only RAISE a saturated channel;
//                                       6: depth-gated blend of the guided estimate with a per-channel
//                                       biharmonic self-dome, weight We = R^4, minimizing E_bihar where
//                                       the colour line is weak) ->
//   7 all-clip luminance dome + chroma (E_bihar luminance dome shared by R,G,B, times an E_chrominance
//                                       screened-Poisson chromaticity fill, for pixels where NO channel
//                                       survives) ->
//   8 anisotropic chroma coherence    (final E_chrominance diffusion ironing the core<->annulus seam) ->
//   composite                          (scatter the reconstructed CLIPPED channels back, floored at 0).
// The region radius R (deepest clip-to-valid depth, from _segment_clipped_regions) sets the reach: the
// coarsest guided scale and the coefficient-field window sigma = clip(R/6, 8, 64) are both derived from
// it below, so the +-3 sigma window just reaches the deepest pixel and no farther. The internal step
// sections are annotated in place; this header only ties them together.
// Per-region working set shared by the reconstruction stages of _region_guided_filter.
// Holds the padded-window buffers (allocated once by the driver) and the region geometry so
// each stage operates on the same arrays through a single handle instead of a ~35-argument
// call. Buffer reuse across stages is intentional and documented at each site (e.g.
// valid_variance carries the CF fit coefficients, then the dome-gate weight Wc; prev_scale
// carries blur moments, then the anisotropic anchor validity). Do not "tidy up" the reuse.
typedef struct _hl_region_ctx_t
{
  // full-resolution I/O (indexed with `width` stride at the region's rx0/ry0 offset)
  float *interp;
  const float *mask;
  const float *depth;
  int width;
  // region geometry + tunables
  const _hl_region_t *region;
  const dt_dev_pixelpipe_t *pipe;
  int region_w, region_h;
  size_t region_pixels;
  int extent;
  float epsilon;
  int max_cg_iter;
  float solid_color;
  float noise_level;
  float floor_gate; // clip-asymmetry gate g in [0,1]: 0 = per-channel floors (unit-WB clips, the
                    // approved behavior, bit-exact), 1 = joint chromaticity-preserving floors +
                    // surround-chroma refinements (real-camera WB'd clips). See _hl_floor_gate().
  // group-A padded-window buffers (live for the whole region)
  float *estimate, *prev_scale, *valid, *blur_in;
  float *plane1, *plane2, *plane3;
  float *valid_variance, *guide_score, *clip_depth, *clip0;
  // group-B solver working set (freed after the all-clip / anisotropic stages)
  uint8_t *hole;
  float *solver_field, *fill_planes, *dome_lum, *lum_accum, *reaction_weight, *flat_target;
  float *cg_residual, *cg_dir, *cg_operator, *cg_tmp1, *cg_tmp2;
} _hl_region_ctx_t;

// ---------------------------------------------------------------------------------------------
// Highlights module parameters + per-module OpenCL global data. Defined here (rather than in
// highlights.c) so every per-stage module TU can see them; highlights.c keeps only its GUI
// data struct. The params struct carries the introspection $DEFAULT/$DESCRIPTION annotations.

typedef enum dt_iop_highlights_mode_t
{
  DT_IOP_HIGHLIGHTS_CLIP = 0,      // $DESCRIPTION: "clip highlights"
  DT_IOP_HIGHLIGHTS_LCH = 1,       // $DESCRIPTION: "reconstruct in LCh"
  DT_IOP_HIGHLIGHTS_INPAINT = 2,   // $DESCRIPTION: "reconstruct color"
  DT_IOP_HIGHLIGHTS_LAPLACIAN = 3, //$DESCRIPTION: "guided laplacians"
  DT_IOP_HIGHLIGHTS_HARMONIC = 4,  //$DESCRIPTION: "harmonic transposition"
} dt_iop_highlights_mode_t;

typedef enum dt_atrous_wavelets_scales_t
{
  WAVELETS_1_SCALE = 0,   // $DESCRIPTION: "2 px"
  WAVELETS_2_SCALE = 1,   // $DESCRIPTION: "4 px"
  WAVELETS_3_SCALE = 2,   // $DESCRIPTION: "8 px"
  WAVELETS_4_SCALE = 3,   // $DESCRIPTION: "16 px"
  WAVELETS_5_SCALE = 4,   // $DESCRIPTION: "32 px"
  WAVELETS_6_SCALE = 5,   // $DESCRIPTION: "64 px"
  WAVELETS_7_SCALE = 6,   // $DESCRIPTION: "128 px (slow)"
  WAVELETS_8_SCALE = 7,   // $DESCRIPTION: "256 px (slow)"
  WAVELETS_9_SCALE = 8,   // $DESCRIPTION: "512 px (very slow)"
  WAVELETS_10_SCALE = 9,  // $DESCRIPTION: "1024 px (very slow)"
  WAVELETS_11_SCALE = 10, // $DESCRIPTION: "2048 px (insanely slow)"
  WAVELETS_12_SCALE = 11, // $DESCRIPTION: "4096 px (insanely slow)"
} dt_atrous_wavelets_scales_t;

typedef struct dt_iop_highlights_params_t
{
  // params of v1
  dt_iop_highlights_mode_t mode; // $DEFAULT: DT_IOP_HIGHLIGHTS_CLIP $DESCRIPTION: "method"
  float blendL;                  // unused $DEFAULT: 1.0
  float blendC;                  // unused $DEFAULT: 0.0
  float blendh;                  // unused $DEFAULT: 0.0
  // params of v2
  float clip; // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 1.0 $DESCRIPTION: "clipping threshold"
  // params of v3
  float noise_level;                  // $MIN: 0. $MAX: 1.0 $DEFAULT: 0.00 $DESCRIPTION: "noise level"
  int iterations;                     // $MIN: 1 $MAX: 512 $DEFAULT: 30 $DESCRIPTION: "iterations"
  dt_atrous_wavelets_scales_t scales; // $DEFAULT: 8 $DESCRIPTION: "diameter of reconstruction"
  float reconstructing;               // $MIN: 0.0 $MAX: 1.0  $DEFAULT: 0.4 $DESCRIPTION: "cast balance"
  float combine;                      // $MIN: 0.0 $MAX: 10.0 $DEFAULT: 2.0 $DESCRIPTION: "combine segments"
  int debugmode;
  // params of v4
  float solid_color; // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.5 $DESCRIPTION: "inpaint a flat color"
} dt_iop_highlights_params_t;

typedef dt_iop_highlights_params_t dt_iop_highlights_data_t;

typedef struct dt_iop_highlights_global_data_t
{
  int kernel_highlights_1f_clip;
  int kernel_highlights_1f_lch_bayer;
  int kernel_highlights_1f_lch_xtrans;
  int kernel_highlights_4f_clip;
  int kernel_highlights_bilinear_and_mask;
  int kernel_highlights_bilinear_and_mask_xtrans;
  int kernel_highlights_bilinear_and_mask_passthrough;
  int kernel_highlights_normalize_reduce_first;
  int kernel_highlights_normalize_reduce_first_xtrans;
  int kernel_highlights_normalize_reduce_first_passthrough;
  int kernel_highlights_normalize_reduce_second;
  int kernel_highlights_remosaic_and_replace;
  int kernel_highlights_remosaic_and_replace_xtrans;
  int kernel_highlights_remosaic_and_replace_passthrough;
  int kernel_highlights_guide_laplacians;
  int kernel_highlights_diffuse_color;
  int kernel_highlights_box_blur;
  int kernel_sparse_chol_update_level;
  int kernel_sparse_chol_final_level;
  int kernel_sparse_chol_fwd_level;
  int kernel_sparse_chol_bwd_level;
  int kernel_hl_cfa_steer;
  int kernel_hl_cfa_down;
  int kernel_hl_cfa_box;
  int kernel_hl_cfa_grad;
  int kernel_hl_cfa_tensor;
  int kernel_hl_cfa_gnorm;
  int kernel_hl_cfa_weights;
  int kernel_hl_cfa_jacobi;
  int kernel_hl_cfa_jacobi_block;
  int kernel_hl_fill_down;
  int kernel_hl_fill_seed;
  int kernel_hl_fill_seed_up;
  int kernel_hl_fill_jacobi;
  int kernel_hl_fill_jacobi_block;
  int kernel_hl_fill_up;
  int kernel_hl_cf_lref_partials;
  int kernel_hl_cf_pack_joint;
  int kernel_hl_cf_fit_joint;
  int kernel_hl_cf_eval_joint;
  int kernel_hl_cf_pack_pair;
  int kernel_hl_cf_fit_pair;
  int kernel_hl_cf_eval_pair;
  int kernel_hl_cf_pack_deepmask;
  int kernel_hl_cf_eval_deep;
  int kernel_hl_buf_to_img;
  int kernel_hl_hf_pack;
  int kernel_hl_hf_fit;
  int kernel_hl_hf_energy;
  int kernel_hl_hf_eval;
  int kernel_hl_hf_damp;
  int kernel_hl_soft_floor;
  int kernel_hl_hard_floor;
  int kernel_hl_lsb_hole;
  int kernel_hl_ratio_plane;
  int kernel_hl_dome_down;
  int kernel_hl_dome_blend;
  int kernel_hl_core_floor;
  int kernel_hl_cmean_reduce;
  int kernel_hl_ratio_cmean_blend;
  int kernel_hl_clip0_rehue;
  int kernel_hl_ring_vote;
  int kernel_hl_cgrad_plateau;
  int kernel_hl_cgrad_guard;
  int kernel_hl_cgrad_anchor;
  int kernel_hl_cgrad_share;
  int kernel_hl_cgrad_store;
  int kernel_hl_cgrad_gate;
  int kernel_hl_cgrad_reproject;
  int kernel_hl_cgrad_hole1c;
  int kernel_hl_cgrad_write1c;
  int kernel_hl_pde_init;
  int kernel_hl_mask_to_img1;
  int kernel_hl_core_blend;
  int kernel_hl_pde_rhs;
  int kernel_hl_pde_scatter;
  int kernel_hl_aniso_prep;
  int kernel_hl_box3;
  int kernel_hl_grad_reduce;
  int kernel_hl_aniso_tensor;
  int kernel_hl_aniso_weights;
  int kernel_hl_aniso_reassemble;
  int kernel_hl_aniso_rhs;
  int kernel_hl_aniso_scatter;
  int kernel_hl_knee_bin;
  int kernel_hl_knee_jmom;
  int kernel_hl_knee_pmom;
  int kernel_hl_knee_joint_reg;
  int kernel_hl_knee_pair_reg;
  int kernel_hl_knee_apply;
  int kernel_hl_mask_pack;
  int kernel_hl_region_gather;
  int kernel_hl_region_scatter;
  int kernel_hl_region_stats;
  int kernel_hl_need_self;
  int kernel_hl_knee_apply_interp;
  int kernel_hl_cg_embed;
  int kernel_hl_cg_op;
  int kernel_hl_cg_r0;
  int kernel_hl_cg_beta;
  int kernel_hl_relu;
  int kernel_hl_cg_r1;
  int kernel_hl_cg_ap;
  int kernel_hl_cg_update;
  int kernel_hl_aniso_pyr_down;
  int kernel_hl_pyr_getc;
  int kernel_hl_pyr_putc;
  int kernel_hl_pyr_getc4;
  int kernel_hl_pyr_putc4;
  int kernel_hl_pyr_project;
  int kernel_hl_aniso_obs_full;
  int kernel_hl_aniso_obs_flags;
  int kernel_hl_window_pack;
  int kernel_hl_window_unpack;
  int kernel_hl_aniso_iter;
  int kernel_hl_aniso_iter_block;
  int kernel_hl_aniso_splat;
  int kernel_highlights_false_color;

  int kernel_filmic_bspline_vertical;
  int kernel_filmic_bspline_horizontal;
  int kernel_filmic_bspline_vertical_local;
  int kernel_filmic_bspline_horizontal_local;

  int kernel_interpolate_bilinear;
} dt_iop_highlights_global_data_t;

// ---------------------------------------------------------------------------------------------
// Guided-laplacian (2021 a-trous) shared constants + scale enums. Used by the laplacian mode TU
// and by highlights.c's tiling_callback; kept here so both see one definition.

#define MAX_NUM_SCALES 12
#define REDUCESIZE 64
#define DS_FACTOR 4
#define SQRT3 1.7320508075688772935274463415058723669L
#define SQRT12 3.4641016151377545870548926830117447339L // 2*SQRT3
typedef enum diffuse_reconstruct_variant_t
{
  DIFFUSE_RECONSTRUCT_RGB = 0,
  DIFFUSE_RECONSTRUCT_CHROMA
} diffuse_reconstruct_variant_t;
enum wavelets_scale_t
{
  ANY_SCALE = 1 << 0,   // any wavelets scale   : reconstruct += HF
  FIRST_SCALE = 1 << 1, // first wavelets scale : reconstruct = 0
  LAST_SCALE = 1 << 2,  // last wavelets scale  : reconstruct += residual
};
#endif // DT_IOP_HIGHLIGHTS_COMMON_H
