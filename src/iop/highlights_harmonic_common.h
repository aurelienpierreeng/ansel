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

// Shared macros and struct definitions for the highlights harmonic-transposition mode,
// used by both the C (highlights_harmonic_cpu.h) and OpenCL (highlights_harmonic_cl.h)
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
//   (perf: -3 blurs/scale); 0 = score by each guide's SINGLE-channel variance (original, more robust
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
