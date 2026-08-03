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

// Coefficient-field colour-line transport + HF-refit stage (CPU + OpenCL). (implementation; see
// coefficient_field.h for the public API.)

#include "common/darktable.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "iop/highlights/blur.h"
#include "iop/highlights/chroma.h"
#include "iop/highlights/coefficient_field.h"
#include "iop/highlights/knee.h"
#include <math.h>
#include <string.h>

// Variance-adaptive steering tensor: a continuous blend between the isophote
// tensor (transport along level lines, correct where a HARD EDGE crosses the zone: the content
// beyond the edge follows another colour-line) and the gradient tensor (radial transport,
// correct on a clean halo: the model lives on the rim and must travel inward). The blend weight
// m is the TREND-CORRECTED windowed variance of the steering plane: raw windowed variance minus
// the part the local linear ramp explains -- a smooth halo ramp has variance but no residual,
// a hard edge has residual variance no ramp can explain. m = v_res / (v_res + (k * mean)^2),
// scale-free (k = relative std threshold). D = [m + (1-m) c2] t t^T + [m c2 + (1-m)] g g^T,
// both weights in (0, 1], D SPD, so the Weickert stencil stays nonnegative (maximum principle).
//
// MATHS BRIDGE -- article "The algorithm" step 3, the E_transport steering tensor. Builds the D of
// E_transport = Sum_p integral grad(p)^T D grad(p) dOmega, whose Euler-Lagrange div(D grad p)=0 is the
// anisotropic fill relaxed below. Article eq:
//   D = [ m + (1-m) c2 ] t t^T  +  [ m c2 + (1-m) ] g g^T ,  c2 = exp(-|grad L_mean| / (4 <|grad L_mean|>))
//   m = v / (v + (k Lbar_mean)^2) ,  v = max( var_w(L_mean) - (4/3)|grad L_mean|^2 , 0 ) ,  k = 0.15
// g = unit gradient (uphill) of the steering plane L_mean, t = unit isophote (level-line), m in [0,1] the
// edge probability. m->0 (clean halo ramp) => D -> g g^T + c2 t t^T, transport radial inward along the
// ramp; m->1 (hard edge in the zone) => D -> t t^T + c2 g g^T, transport along the boundary, not across it.
__DT_CLONE_TARGETS__
static void _cf_adaptive_tensor(const float *const restrict luminance, float *const restrict tensor_xx,
                                float *const restrict tensor_xy, float *const restrict tensor_yy,
                                float *const restrict scratch_lin, float *const restrict scratch_quad,
                                const int region_w, const int region_h, const float k)
{
  const size_t region_pixels = (size_t)region_w * region_h;

  // two 3x3 box passes on L (into scratch_lin) and on L^2 (into scratch_quad)
  for(int pass = 0; pass < 2; pass++)
  {
    const float *const src_lin = (pass == 0) ? luminance : scratch_lin;
    const float *const src_quad = (pass == 0) ? luminance : scratch_quad; // pass 0 squares on the fly

    HL_PFOR(collapse(2))
    for(int y = 0; y < region_h; y++)
      for(int x = 0; x < region_w; x++)
      {
        double sum_lin = 0.0;
        double sum_quad = 0.0;
        int count = 0;
        for(int offset_y = -1; offset_y <= 1; offset_y++)
          for(int offset_x = -1; offset_x <= 1; offset_x++)
          {
            const int neighbour_y = CLAMP(y + offset_y, 0, region_h - 1);
            const int neighbour_x = CLAMP(x + offset_x, 0, region_w - 1);
            const float value_lin = src_lin[(size_t)neighbour_y * region_w + neighbour_x];
            sum_lin += value_lin;
            sum_quad += (pass == 0) ? (double)value_lin * value_lin
                                    : src_quad[(size_t)neighbour_y * region_w + neighbour_x];
            count++;
          }
        tensor_xx[(size_t)y * region_w + x] = (float)(sum_lin / count);
        tensor_yy[(size_t)y * region_w + x] = (float)(sum_quad / count);
      }

    HL_PFOR()
    for(size_t i = 0; i < region_pixels; i++)
    {
      scratch_lin[i] = tensor_xx[i];
      scratch_quad[i] = tensor_yy[i];
    }
  }

  // gradients of the blurred L + mean magnitude
  double grad_sum = 0.0;
  HL_PFOR(collapse(2) reduction(+ : grad_sum))
  for(int y = 0; y < region_h; y++)
    for(int x = 0; x < region_w; x++)
    {
      const int x_lo = MAX(x - 1, 0), x_hi = MIN(x + 1, region_w - 1);
      const int y_lo = MAX(y - 1, 0), y_hi = MIN(y + 1, region_h - 1);
      const float grad_x
          = 0.5f * (scratch_lin[(size_t)y * region_w + x_hi] - scratch_lin[(size_t)y * region_w + x_lo]);
      const float grad_y
          = 0.5f * (scratch_lin[(size_t)y_hi * region_w + x] - scratch_lin[(size_t)y_lo * region_w + x]);
      tensor_xx[(size_t)y * region_w + x] = grad_x;
      tensor_xy[(size_t)y * region_w + x] = grad_y;
      grad_sum += dt_fast_hypotf(grad_x, grad_y);
    }
  const float grad_mean
      = fmaxf((float)(grad_sum / (double)region_pixels),
              1e-9f); // <|grad L_mean|>, the regional mean magnitude (exposure-independent normaliser)

  HL_PFOR()
  for(size_t i = 0; i < region_pixels; i++)
  {
    const float grad_x = tensor_xx[i];
    const float grad_y = tensor_xy[i];
    const float grad_mag = dt_fast_hypotf(grad_x, grad_y);
    const float nonzero = (grad_mag > 1e-12f) ? 1.f : 0.f;
    const float inv_mag = nonzero / (grad_mag + (1.f - nonzero));
    const float grad_unit_x = grad_x * inv_mag + (1.f - nonzero); // g = unit gradient direction (uphill)
    const float grad_unit_y = grad_y * inv_mag;
    const float isophote_x = -grad_unit_y, isophote_y = grad_unit_x; // t = unit isophote = g rotated 90deg
    const float cross_damp = expf(-grad_mag / (4.f * grad_mean));    // c2 = exp(-|grad L_mean| / (4 <|grad
                                                                     // L_mean|>)), edge-crossing damping

    // trend-corrected windowed variance: two 3x3 box passes have spatial variance 4/3 per axis
    const float variance = fmaxf(scratch_quad[i] - scratch_lin[i] * scratch_lin[i],
                                 0.f); // var_w(L_mean) = E[L^2]-E[L]^2 (centred by construction of the box passes)
    const float residual_var
        = fmaxf(variance - (4.f / 3.f) * (grad_x * grad_x + grad_y * grad_y),
                0.f); // v = max(var_w - (4/3)|grad L_mean|^2, 0): subtract the variance the local ramp explains
    const float k_term
        = sqf(k * fmaxf(scratch_lin[i], 1e-9f)); // (k * Lbar_mean)^2, the scale-free contrast threshold
    const float edge_prob
        = residual_var / (residual_var + k_term + 1e-18f); // m = v / (v + (k Lbar_mean)^2) in [0,1]

    const float diffuse_tangent = edge_prob + (1.f - edge_prob) * cross_damp;  // coeff of t t^T = m + (1-m) c2
    const float diffuse_gradient = edge_prob * cross_damp + (1.f - edge_prob); // coeff of g g^T = m c2 + (1-m)

    // D = diffuse_tangent * t t^T + diffuse_gradient * g g^T, stored as its symmetric xx/xy/yy entries
    tensor_xx[i] = diffuse_tangent * isophote_x * isophote_x + diffuse_gradient * grad_unit_x * grad_unit_x;
    tensor_xy[i] = diffuse_tangent * isophote_x * isophote_y + diffuse_gradient * grad_unit_x * grad_unit_y;
    tensor_yy[i] = diffuse_tangent * isophote_y * isophote_y + diffuse_gradient * grad_unit_y * grad_unit_y;
  }
}

// Coarse-to-fine harmonic fill of up to DT_HL_FILL_MAXP coefficient planes SHARING ONE anchor
// mask: hole pixels relax toward their 4-neighbour average (Jacobi) with anchors pinned, each
// pyramid level seeding the next finer one. Unconditionally stable by the maximum principle
// (values stay within the anchors' range), unlike a float CG on the near-singular pure-harmonic
// system, which diverges stochastically when the hole reaches the region border. Coefficients
// are smooth, so the solve runs on a base grid downsampled by `base_ds` and is bilinearly
// upsampled into the hole pixels.
// With `steer` non-NULL (the coefficient planes), the relaxation is tensor-weighted instead
// of uniform: per level, the variance-adaptive tensor is built from the downsampled steering
// plane (_cf_adaptive_tensor) and the update becomes an 8-neighbour average with the Weickert
// nonnegativity weights (_aniso_edge_w) -- all weights >= 0, so the fill stays a convex
// combination of anchors (maximum principle intact). NULL steer = plain isotropic fill (the
// rim-chrominance ratios, and any plane with no guide structure to follow).

// One level's Jacobi relaxation of NP planes sharing one anchor mask, macro-generated so NP
// is a compile-time literal: the plane guards fold away and the per-plane accumulators stay in
// registers. (An inline function with a runtime plane count does NOT specialize -- GCC outlines
// the OpenMP region and the count arrives through the shared-args struct, so the su[] array
// spilled to the stack on every fma and the fused sweep measured 2.5x SLOWER than the
// single-plane one. Literal NP recovers it.)
//
// Jacobi relaxation of the holes (anchors pinned): a flat 100-sweep budget per level.
// Convergence is guaranteed by the pyramid depth of the caller, NOT by the sweep count --
// boosting sweeps at the coarsest level instead was measured pathological (thousands of
// parallel sweeps of microsecond work = pure scheduling overhead, seconds per fill on small
// regions). One parallel region for the whole relaxation: launching a fresh team per sweep
// was pure scheduling overhead on these small grids (the sweep's work is microseconds; 100
// sweeps x levels x fills x regions reached tens of thousands of launches per image). Threads
// ping-pong between u and tmp (no per-sweep memcpy); the even sweep count lands the final
// solution in u. The omp-for barrier at the end of each sweep keeps Jacobi ordering.
// All NP planes advance inside the same sweep: the weights are read once per cell.
//
// MATHS BRIDGE -- article "The algorithm" step 3, one Jacobi sweep of the E_transport solver
// div(D grad p)=0 on the coefficient planes p in {a, b, d, R^2}. Discrete update rules (anchors are
// Dirichlet boundary data, pinned = copied through unchanged):
//   steered  (D != I): dst(i) = Sum_k w_ik * src(neighbour_k) / Sum_k w_ik  over the 8-neighbour
//                      Weickert nonnegativity stencil weights w_ik = _aniso_edge_w(D) >= 0, so the
//                      update is a convex combination of neighbours -> maximum principle holds.
//   isotropic (D = I): dst(i) = 1/4 (north + south + west + east), the plain harmonic (Laplace) fill.
// NOTE (C preprocessor): every comment inside this macro body MUST be a /* ... */ closed on its own
// physical line before the trailing backslash -- a // comment would splice with the next line and
// swallow the rest of the macro. That is why the annotations below use block-comment form.
#define DEFINE_CF_FILL_RELAX(NP)                                                                                  \
  __DT_CLONE_TARGETS__                                                                                            \
  static void _cf_fill_relax_##NP(                                                                                \
      float *const restrict field, float *const restrict tmp, const uint8_t *const restrict level_anchor,         \
      const float *const restrict edge_weights, const float *const restrict edge_weight_sum, const int coarse_w,  \
      const int coarse_h, const size_t cell_count, const int steered)                                             \
  {                                                                                                               \
    const int n_sweeps = 100;                                                                                     \
    __OMP_PARALLEL__()                                                                                            \
    for(int sweep = 0; sweep < n_sweeps; sweep++)                                                                 \
    {                                                                                                             \
      const float *const source = (sweep & 1) ? tmp : field;                                                      \
      float *const dest = (sweep & 1) ? field : tmp;                                                              \
      const float *const src0 = source;                                                                           \
      const float *const src1 = source + ((NP) > 1 ? cell_count : 0);                                             \
      const float *const src2 = source + ((NP) > 2 ? 2 * cell_count : 0);                                         \
      const float *const src3 = source + ((NP) > 3 ? 3 * cell_count : 0);                                         \
      float *const dst0 = dest;                                                                                   \
      float *const dst1 = dest + ((NP) > 1 ? cell_count : 0);                                                     \
      float *const dst2 = dest + ((NP) > 2 ? 2 * cell_count : 0);                                                 \
      float *const dst3 = dest + ((NP) > 3 ? 3 * cell_count : 0);                                                 \
                                                                                                                  \
      __OMP_FOR__(collapse(2))                                                                                    \
      for(int cell_y = 0; cell_y < coarse_h; cell_y++)                                                            \
        for(int cell_x = 0; cell_x < coarse_w; cell_x++)                                                          \
        {                                                                                                         \
          const size_t i = (size_t)cell_y * coarse_w + cell_x;                                                    \
                                                                                                                  \
          /* anchor cell = Dirichlet boundary datum p|anchors = p_fit: copy it through unchanged */               \
          if(level_anchor[i])                                                                                     \
          {                                                                                                       \
            dst0[i] = src0[i];                                                                                    \
            if((NP) > 1) dst1[i] = src1[i];                                                                       \
            if((NP) > 2) dst2[i] = src2[i];                                                                       \
            if((NP) > 3) dst3[i] = src3[i];                                                                       \
            continue;                                                                                             \
          }                                                                                                       \
                                                                                                                  \
          const size_t idx_north = (size_t)MAX(cell_y - 1, 0) * coarse_w + cell_x;                                \
          const size_t idx_south = (size_t)MIN(cell_y + 1, coarse_h - 1) * coarse_w + cell_x;                     \
          const size_t idx_west = (size_t)cell_y * coarse_w + MAX(cell_x - 1, 0);                                 \
          const size_t idx_east = (size_t)cell_y * coarse_w + MIN(cell_x + 1, coarse_w - 1);                      \
                                                                                                                  \
          if(steered)                                                                                             \
          {                                                                                                       \
            /* 8-neighbour Jacobi with the precomputed Weickert nonnegativity weights: every  */                  \
            /* weight >= 0, so the update is a convex combination and the maximum principle   */                  \
            /* is preserved.                                                                  */                  \
            static const int neighbour_dy[8] = { 0, 0, -1, 1, -1, 1, 1, -1 };                                     \
            static const int neighbour_dx[8] = { -1, 1, 0, 0, -1, 1, -1, 1 };                                     \
            float accum0 = 0.f, accum1 = 0.f, accum2 = 0.f, accum3 = 0.f;                                         \
            for(int k = 0; k < 8; k++)                                                                            \
            {                                                                                                     \
              const int neighbour_y = CLAMP(cell_y + neighbour_dy[k], 0, coarse_h - 1);                           \
              const int neighbour_x = CLAMP(cell_x + neighbour_dx[k], 0, coarse_w - 1);                           \
              const size_t j = (size_t)neighbour_y * coarse_w + neighbour_x;                                      \
              const float weight = edge_weights[i * 8 + k];                                                       \
              accum0 += weight * src0[j];                                                                         \
              if((NP) > 1) accum1 += weight * src1[j];                                                            \
              if((NP) > 2) accum2 += weight * src2[j];                                                            \
              if((NP) > 3) accum3 += weight * src3[j];                                                            \
            }                                                                                                     \
            /* dst = Sum_k w_ik src(nb_k) / Sum_k w_ik : the steered div(D grad p)=0 Jacobi update */             \
            const float weight_sum = edge_weight_sum[i];                                                          \
            const int valid = (weight_sum > 1e-9f);                                                               \
            dst0[i] = valid ? accum0 / weight_sum : src0[i];                                                      \
            if((NP) > 1) dst1[i] = valid ? accum1 / weight_sum : src1[i];                                         \
            if((NP) > 2) dst2[i] = valid ? accum2 / weight_sum : src2[i];                                         \
            if((NP) > 3) dst3[i] = valid ? accum3 / weight_sum : src3[i];                                         \
          }                                                                                                       \
          /* D = I: plain 4-neighbour average, the discrete harmonic (Laplace) fill div(grad p)=0 */              \
          else                                                                                                    \
          {                                                                                                       \
            dst0[i] = 0.25f * (src0[idx_north] + src0[idx_south] + src0[idx_west] + src0[idx_east]);              \
            if((NP) > 1) dst1[i] = 0.25f * (src1[idx_north] + src1[idx_south] + src1[idx_west] + src1[idx_east]); \
            if((NP) > 2) dst2[i] = 0.25f * (src2[idx_north] + src2[idx_south] + src2[idx_west] + src2[idx_east]); \
            if((NP) > 3) dst3[i] = 0.25f * (src3[idx_north] + src3[idx_south] + src3[idx_west] + src3[idx_east]); \
          }                                                                                                       \
        }                                                                                                         \
    }                                                                                                             \
  }

DEFINE_CF_FILL_RELAX(1)
DEFINE_CF_FILL_RELAX(2)
DEFINE_CF_FILL_RELAX(3)
DEFINE_CF_FILL_RELAX(4)

// MATHS BRIDGE -- article "The algorithm" step 3, the E_transport solver: the anchored, coarse-to-fine
// anisotropic transport of the coefficient planes. Minimizes E_transport = Sum_p int grad(p)^T D grad(p)
// with p|anchors = p_fit by relaxing div(D grad p)=0 to its fixed point. `hole` marks the cells to fill
// (holes); its complement are the anchors (the gated colour-line fits, R^2 > 0.25, bounded slopes).
// `steer` non-NULL feeds _cf_adaptive_tensor to build D (steered fill); NULL => D = I (plain harmonic
// fill). Coefficients are smooth, so the whole relaxation runs on a base grid at pitch ~sigma/4
// (article "Cell"), and Jacobi convergence comes from the PYRAMID DEPTH, not the fixed 100-sweep budget:
// the coarsest level starts from a flat anchor mean and each finer level is bilinearly seeded from the
// coarser solution, then corrected. Final result is bilinearly upsampled into the full-res hole pixels.
__DT_CLONE_TARGETS__
static void _cf_harmonic_fill_n(float *const restrict *vals, const int n_planes_in,
                                const uint8_t *const restrict hole, const int region_w, const int region_h,
                                const int base_ds, const float *const restrict steer,
                                const dt_dev_pixelpipe_t *pipe)
{
  const int n_planes = CLAMP(n_planes_in, 1, DT_HL_FILL_MAXP);
  const int downsample = CLAMP(base_ds, 1, 8);
  const int base_w = (region_w + downsample - 1) / downsample;
  const int base_h = (region_h + downsample - 1) / downsample;
  const size_t cell_count = (size_t)base_w * base_h;

  float *const restrict base_vals = dt_pixelpipe_cache_alloc_align_float(cell_count * n_planes, pipe);
  uint8_t *const restrict base_anchor
      = (uint8_t *)dt_pixelpipe_cache_alloc_align(sizeof(uint8_t) * cell_count, pipe);
  // field, tmp, f (n_planes planes each, plane-major) + shared L
  float *const restrict level_buffers
      = dt_pixelpipe_cache_alloc_align_float(cell_count * 3 * (size_t)n_planes, pipe);
  uint8_t *const restrict level_anchor
      = (uint8_t *)dt_pixelpipe_cache_alloc_align(sizeof(uint8_t) * cell_count, pipe);
  // aniso: base-grid steering plane + per-level {level_steer, tensor_xx, tensor_xy, tensor_yy, scratch,
  // var-scratch}
  // + per-cell edge weights (8, interleaved) + their sum, precomputed once per level
  float *const restrict aniso_aux = steer ? dt_pixelpipe_cache_alloc_align_float(cell_count * 16, pipe) : NULL;
  const int steered = (steer && aniso_aux) ? 1 : 0;

  if(!base_vals || !base_anchor || !level_buffers || !level_anchor || (steer && !aniso_aux))
  {
    // fallback: fill the holes with the global anchor mean (never leave garbage coefficients)
    for(int plane = 0; plane < n_planes; plane++)
    {
      float *const restrict plane_vals = vals[plane];
      double anchor_sum = 0.0;
      size_t anchor_count = 0;
      for(size_t i = 0; i < (size_t)region_w * region_h; i++)
        if(!hole[i])
        {
          anchor_sum += plane_vals[i];
          anchor_count++;
        }

      const float anchor_mean = anchor_count ? (float)(anchor_sum / (double)anchor_count) : 0.f;
      HL_PFOR()
      for(size_t i = 0; i < (size_t)region_w * region_h; i++)
        if(hole[i]) plane_vals[i] = anchor_mean;
    }

    dt_pixelpipe_cache_free_align(base_vals);
    dt_pixelpipe_cache_free_align(base_anchor);
    dt_pixelpipe_cache_free_align(level_buffers);
    dt_pixelpipe_cache_free_align(level_anchor);
    dt_pixelpipe_cache_free_align(aniso_aux);
    return;
  }

  // aniso: steering plane on the base grid (plain cell mean; the tensor smooths later)
  float *const restrict base_steer = steered ? aniso_aux + 5 * cell_count : NULL; // plane 6 = adaptive scratch
  if(steered)
  {
    HL_PFOR(collapse(2))
    for(int base_y = 0; base_y < base_h; base_y++)
      for(int base_x = 0; base_x < base_w; base_x++)
      {
        double accum = 0.0;
        int n_total = 0;
        for(int y = base_y * downsample; y < MIN((base_y + 1) * downsample, region_h); y++)
          for(int x = base_x * downsample; x < MIN((base_x + 1) * downsample, region_w); x++)
          {
            accum += steer[(size_t)y * region_w + x];
            n_total++;
          }
        base_steer[(size_t)base_y * base_w + base_x] = (float)(accum / n_total);
      }
  }

  // base grid: anchor-weighted mean per cell and per plane, anchor = cell majority (shared)
  HL_PFOR(collapse(2))
  for(int base_y = 0; base_y < base_h; base_y++)
    for(int base_x = 0; base_x < base_w; base_x++)
    {
      double accum[DT_HL_FILL_MAXP] = { 0.0 };
      int n_anchor = 0;
      int n_total = 0;

      for(int y = base_y * downsample; y < MIN((base_y + 1) * downsample, region_h); y++)
        for(int x = base_x * downsample; x < MIN((base_x + 1) * downsample, region_w); x++)
        {
          const size_t i = (size_t)y * region_w + x;
          n_total++;

          if(!hole[i])
          {
            for(int plane = 0; plane < n_planes; plane++) accum[plane] += vals[plane][i];
            n_anchor++;
          }
        }

      const size_t cell_index = (size_t)base_y * base_w + base_x;
      for(int plane = 0; plane < n_planes; plane++)
        base_vals[plane * cell_count + cell_index] = n_anchor ? (float)(accum[plane] / n_anchor) : 0.f;
      base_anchor[cell_index] = (2 * n_anchor > n_total);
    }

  // Pyramid depth (article step 3, "Convergence comes from the pyramid's depth"): the slowest Jacobi
  // error mode on a hole N cells wide decays in O(N^2) sweeps, so the coarsest grid must be small.
  // Halve until the LONG side is <= 8 cells. The coarsest level is seeded
  // with a flat anchor mean, and Jacobi needs ~O(N^2) sweeps to relax a flat seed on a hole
  // N cells wide -- so the coarsest grid must be small enough that the fixed per-level sweep
  // budget genuinely converges it; every finer level then only corrects local interpolation
  // error. (The previous short-side floor of 16 left elongated coarsest grids under-converged
  // on deep holes: pk1synth -22% RMSE, occluded -13% once actually converged.)
  int n_levels = 1;
  while((MAX(base_w, base_h) >> n_levels) > 8 && n_levels < 12) n_levels++;

  float *const restrict field = level_buffers + 0 * cell_count;              // n_planes planes, stride cell_count
  float *const restrict tmp = level_buffers + (size_t)n_planes * cell_count; // n_planes planes, stride cell_count
  float *const restrict level_vals
      = level_buffers + 2 * (size_t)n_planes * cell_count; // n_planes planes, stride cell_count

  int prev_level_w = 0;
  int prev_level_h = 0;

  for(int level = n_levels - 1; level >= 0; level--)
  {
    const int step = 1 << level;
    const int level_w = (base_w + step - 1) / step;
    const int level_h = (base_h + step - 1) / step;

    // downsample the base grid to this level (anchor-weighted mean + majority), into f/level_anchor
    HL_PFOR(collapse(2))
    for(int level_y = 0; level_y < level_h; level_y++)
      for(int level_x = 0; level_x < level_w; level_x++)
      {
        double accum[DT_HL_FILL_MAXP] = { 0.0 };
        int n_anchor = 0;
        int n_total = 0;

        for(int y = level_y * step; y < MIN((level_y + 1) * step, base_h); y++)
          for(int x = level_x * step; x < MIN((level_x + 1) * step, base_w); x++)
          {
            const size_t i = (size_t)y * base_w + x;
            n_total++;

            if(base_anchor[i])
            {
              for(int plane = 0; plane < n_planes; plane++) accum[plane] += base_vals[plane * cell_count + i];
              n_anchor++;
            }
          }

        const size_t cell_index = (size_t)level_y * level_w + level_x;
        for(int plane = 0; plane < n_planes; plane++)
          level_vals[plane * cell_count + cell_index] = n_anchor ? (float)(accum[plane] / n_anchor) : 0.f;
        level_anchor[cell_index] = (2 * n_anchor > n_total);
      }

    // aniso: level steering plane -> structure tensor (Weickert-stencil weights)
    float *const restrict level_steer = steered ? aniso_aux + 0 * cell_count : NULL;
    float *const restrict tensor_xx = steered ? aniso_aux + 1 * cell_count : NULL;
    float *const restrict tensor_xy = steered ? aniso_aux + 2 * cell_count : NULL;
    float *const restrict tensor_yy = steered ? aniso_aux + 3 * cell_count : NULL;

    if(steered)
    {
      HL_PFOR(collapse(2))
      for(int level_y = 0; level_y < level_h; level_y++)
        for(int level_x = 0; level_x < level_w; level_x++)
        {
          double steer_sum = 0.0;
          int n_total = 0;
          for(int y = level_y * step; y < MIN((level_y + 1) * step, base_h); y++)
            for(int x = level_x * step; x < MIN((level_x + 1) * step, base_w); x++)
            {
              steer_sum += base_steer[(size_t)y * base_w + x];
              n_total++;
            }
          level_steer[(size_t)level_y * level_w + level_x] = (float)(steer_sum / n_total);
        }

      // build the steering tensor D at this pyramid level from the downsampled L_mean plane
      _cf_adaptive_tensor(level_steer, tensor_xx, tensor_xy, tensor_yy, aniso_aux + 4 * cell_count,
                          aniso_aux + 6 * cell_count, level_w, level_h, DT_HL_CF_K);

      // The Weickert edge weights are constant across every sweep of this level (the tensor is
      // fixed): precompute the 8 weights per cell (interleaved) plus their sum once, so the
      // Jacobi inner loop is a pure multiply-accumulate. Same values, same accumulation order
      // as the previous inline computation -- the relaxation result is bit-identical.
      float *const restrict edge_weights = aniso_aux + 7 * cell_count;
      float *const restrict edge_weight_sum = aniso_aux + 15 * cell_count;
      HL_PFOR(collapse(2))
      for(int level_y = 0; level_y < level_h; level_y++)
        for(int level_x = 0; level_x < level_w; level_x++)
        {
          static const int neighbour_dy[8] = { 0, 0, -1, 1, -1, 1, 1, -1 };
          static const int neighbour_dx[8] = { -1, 1, 0, 0, -1, 1, -1, 1 };
          const size_t i = (size_t)level_y * level_w + level_x;
          float weight_sum = 0.f;
          for(int k = 0; k < 8; k++)
          {
            const int neighbour_y = CLAMP(level_y + neighbour_dy[k], 0, level_h - 1);
            const int neighbour_x = CLAMP(level_x + neighbour_dx[k], 0, level_w - 1);
            const size_t cell_index = (size_t)neighbour_y * level_w + neighbour_x;
            // w_ik: Weickert nonnegativity stencil weight for direction k, derived from D (>= 0)
            const float weight
                = _aniso_edge_w(tensor_xx, tensor_xy, tensor_yy, i, cell_index, neighbour_dx[k], neighbour_dy[k]);
            edge_weights[i * 8 + k] = weight;
            weight_sum += weight;
          }
          edge_weight_sum[i] = weight_sum;
        }
    }

    if(level == n_levels - 1)
    {
      // coarsest: seed the holes with the level's anchor mean, per plane (the flat starting state,
      // farthest from the solution; the pyramid depth guarantees Jacobi relaxes it within budget)
      double anchor_sum[DT_HL_FILL_MAXP] = { 0.0 };
      size_t anchor_count = 0;
      for(size_t i = 0; i < (size_t)level_w * level_h; i++)
        if(level_anchor[i])
        {
          for(int plane = 0; plane < n_planes; plane++) anchor_sum[plane] += level_vals[plane * cell_count + i];
          anchor_count++;
        }

      float anchor_mean[DT_HL_FILL_MAXP];
      for(int plane = 0; plane < n_planes; plane++)
        anchor_mean[plane] = anchor_count ? (float)(anchor_sum[plane] / (double)anchor_count) : 0.f;
      HL_PFOR()
      for(size_t i = 0; i < (size_t)level_w * level_h; i++)
        for(int plane = 0; plane < n_planes; plane++)
          tmp[plane * cell_count + i] = level_anchor[i] ? level_vals[plane * cell_count + i] : anchor_mean[plane];
    }
    else
    {
      // seed the holes from the coarser solution (bilinear), anchors from this level's means
      HL_PFOR(collapse(2))
      for(int level_y = 0; level_y < level_h; level_y++)
        for(int level_x = 0; level_x < level_w; level_x++)
        {
          const size_t i = (size_t)level_y * level_w + level_x;

          if(level_anchor[i])
          {
            for(int plane = 0; plane < n_planes; plane++)
              tmp[plane * cell_count + i] = level_vals[plane * cell_count + i];
            continue;
          }

          const float grid_x = ((float)level_x + 0.5f) * 0.5f - 0.5f;
          const float grid_y = ((float)level_y + 0.5f) * 0.5f - 0.5f;
          const int x_lo = CLAMP((int)floorf(grid_x), 0, prev_level_w - 1);
          const int y_lo = CLAMP((int)floorf(grid_y), 0, prev_level_h - 1);
          const int x_hi = MIN(x_lo + 1, prev_level_w - 1);
          const int y_hi = MIN(y_lo + 1, prev_level_h - 1);
          const float frac_x = CLAMP(grid_x - x_lo, 0.f, 1.f);
          const float frac_y = CLAMP(grid_y - y_lo, 0.f, 1.f);
          for(int plane = 0; plane < n_planes; plane++)
          {
            const float *const plane_field = field + plane * cell_count;
            const float interp_top = plane_field[(size_t)y_lo * prev_level_w + x_lo] * (1.f - frac_x)
                                     + plane_field[(size_t)y_lo * prev_level_w + x_hi] * frac_x;
            const float interp_bottom = plane_field[(size_t)y_hi * prev_level_w + x_lo] * (1.f - frac_x)
                                        + plane_field[(size_t)y_hi * prev_level_w + x_hi] * frac_x;
            tmp[plane * cell_count + i] = interp_top * (1.f - frac_y) + interp_bottom * frac_y;
          }
        }
    }

    for(int plane = 0; plane < n_planes; plane++)
      memcpy(field + plane * cell_count, tmp + plane * cell_count, (size_t)level_w * level_h * sizeof(float));

    // relaxation: iterate the div(D grad p)=0 Jacobi update to its fixed point on this level
    // (specialized on the plane count, see DEFINE_CF_FILL_RELAX)
    {
      const float *const restrict edge_weights = steered ? aniso_aux + 7 * cell_count : NULL;
      const float *const restrict edge_weight_sum = steered ? aniso_aux + 15 * cell_count : NULL;
      switch(n_planes)
      {
        case 1:
          _cf_fill_relax_1(field, tmp, level_anchor, edge_weights, edge_weight_sum, level_w, level_h, cell_count,
                           steered);
          break;
        case 2:
          _cf_fill_relax_2(field, tmp, level_anchor, edge_weights, edge_weight_sum, level_w, level_h, cell_count,
                           steered);
          break;
        case 3:
          _cf_fill_relax_3(field, tmp, level_anchor, edge_weights, edge_weight_sum, level_w, level_h, cell_count,
                           steered);
          break;
        default:
          _cf_fill_relax_4(field, tmp, level_anchor, edge_weights, edge_weight_sum, level_w, level_h, cell_count,
                           steered);
          break;
      }
    }

    prev_level_w = level_w;
    prev_level_h = level_h;
  }

  // upsample the base-grid coefficient solution into the full-res hole pixels by bilinear interp
  // (anchors keep their exact fitted values -- the Dirichlet data is never overwritten)
  HL_PFOR(collapse(2))
  for(int y = 0; y < region_h; y++)
    for(int x = 0; x < region_w; x++)
    {
      const size_t i = (size_t)y * region_w + x;

      if(!hole[i]) continue;

      const float grid_x = ((float)x + 0.5f) / downsample - 0.5f;
      const float grid_y = ((float)y + 0.5f) / downsample - 0.5f;
      const int x_lo = CLAMP((int)floorf(grid_x), 0, base_w - 1);
      const int y_lo = CLAMP((int)floorf(grid_y), 0, base_h - 1);
      const int x_hi = MIN(x_lo + 1, base_w - 1);
      const int y_hi = MIN(y_lo + 1, base_h - 1);
      const float frac_x = CLAMP(grid_x - x_lo, 0.f, 1.f);
      const float frac_y = CLAMP(grid_y - y_lo, 0.f, 1.f);
      for(int plane = 0; plane < n_planes; plane++)
      {
        const float *const plane_field = field + plane * cell_count;
        const float interp_top = plane_field[(size_t)y_lo * base_w + x_lo] * (1.f - frac_x)
                                 + plane_field[(size_t)y_lo * base_w + x_hi] * frac_x;
        const float interp_bottom = plane_field[(size_t)y_hi * base_w + x_lo] * (1.f - frac_x)
                                    + plane_field[(size_t)y_hi * base_w + x_hi] * frac_x;
        vals[plane][i] = interp_top * (1.f - frac_y) + interp_bottom * frac_y;
      }
    }

  dt_pixelpipe_cache_free_align(base_vals);
  dt_pixelpipe_cache_free_align(base_anchor);
  dt_pixelpipe_cache_free_align(level_buffers);
  dt_pixelpipe_cache_free_align(level_anchor);
  dt_pixelpipe_cache_free_align(aniso_aux);
}

void _cf_harmonic_fill(float *const restrict val, const uint8_t *const restrict hole, const int region_w,
                       const int region_h, const int base_ds, const float *const restrict steer,
                       const dt_dev_pixelpipe_t *pipe)
{
  float *plane_ptrs[1] = { val };
  _cf_harmonic_fill_n((float *const restrict *)plane_ptrs, 1, hole, region_w, region_h, base_ds, steer, pipe);
}

__DT_CLONE_TARGETS__
void _cf_reconstruct(_hl_region_ctx_t *const ctx)
{
  const _hl_region_t *const region = ctx->region;
  const dt_dev_pixelpipe_t *const pipe = ctx->pipe;
  const int region_w = ctx->region_w;
  const int region_h = ctx->region_h;
  const size_t region_pixels = ctx->region_pixels;
  float *const restrict estimate = ctx->estimate;
  float *const restrict prev_scale = ctx->prev_scale;
  float *const restrict valid = ctx->valid;
  float *const restrict blur_in = ctx->blur_in;
  float *const restrict plane1 = ctx->plane1;
  float *const restrict plane2 = ctx->plane2;
  float *const restrict plane3 = ctx->plane3;
  float *const restrict valid_variance = ctx->valid_variance;
  float *const restrict guide_score = ctx->guide_score;
  float *const restrict clip_depth = ctx->clip_depth;
  float *const restrict clip0 = ctx->clip0;
  uint8_t *const restrict hole = ctx->hole;
  float *const restrict solver_field = ctx->solver_field;
  float *const restrict fill_planes = ctx->fill_planes;
  float *const restrict dome_lum = ctx->dome_lum;
  float *const restrict lum_accum = ctx->lum_accum;
  float *const restrict reaction_weight = ctx->reaction_weight;
  float *const restrict flat_target = ctx->flat_target;

  const float cf_sigma
      = CLAMP(region->radius / 6.f, 8.f, 64.f); // sigma = clip(r/6, 8, 64): +/-3 sigma window reaches the deepest
                                                // pixel; floor/cap bound samples/cost
  const float cf_fmin = 0.05f;

  // region luminance + the blown zone's plateau level, for the occlusion-aware fills
  HL_PFOR()
  for(size_t i = 0; i < region_pixels; i++)
    lum_accum[i] = estimate[i * 4 + 0] + estimate[i * 4 + 1] + estimate[i * 4 + 2];

  double laccum = 0.0;
  size_t lcnt = 0;
  HL_PFOR(reduction(+ : laccum, lcnt))
  for(size_t i = 0; i < region_pixels; i++)
    if(valid[i * 4 + 0] < 0.5f || valid[i * 4 + 1] < 0.5f || valid[i * 4 + 2] < 0.5f)
    {
      laccum += lum_accum[i];
      lcnt++;
    }

  const float cf_lref = lcnt ? (float)(laccum / (double)lcnt) : 0.f;

  // Steering plane for the coefficient fills = the measured guide structure.
  // Mean of the VALID channels where at least one survives (real data inside the
  // partial-clip zone); the flat plateau mean elsewhere (all-clip core), where a flat
  // steer degenerates the tensor to identity, i.e. back to the isotropic fill.
  float *const restrict steer = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
  if(steer)
  {
    HL_PFOR()
    for(size_t i = 0; i < region_pixels; i++)
    {
      float accum = 0.f;
      int n_valid = 0;
      for(int c = 0; c < 3; c++)
        if(valid[i * 4 + c] >= 0.5f)
        {
          accum += estimate[i * 4 + c];
          n_valid++;
        }
      steer[i] = n_valid ? accum / n_valid : lum_accum[i] / 3.f;
    }
  }

  // Per-channel means of the VALID values: the moment packs below are CENTERED on them.
  // var = E[u^2] - E[u]^2 in float32 on a smooth plane cancels catastrophically (the mean
  // squared dwarfs the variance, ~4 digits lost) and the fit's cov/var division amplifies
  // the surviving noise -- measured as a device-dependent slope error growing with depth.
  // Centering the packs makes the blurred moments carry the (co)variances directly ; the
  // slopes and R^2 are shift-invariant, and the intercept is unshifted right after the fit.
  double maccum[3] = { 0.0, 0.0, 0.0 };
  size_t mcnt[3] = { 0, 0, 0 };
  HL_PFOR(reduction(+ : maccum[:3], mcnt[:3]))
  for(size_t i = 0; i < region_pixels; i++)
    for(int c = 0; c < 3; c++)
      if(valid[i * 4 + c] >= 0.5f)
      {
        maccum[c] += estimate[i * 4 + c];
        mcnt[c]++;
      }
  const float channel_means[3]
      = { mcnt[0] ? (float)(maccum[0] / mcnt[0]) : 0.f, mcnt[1] ? (float)(maccum[1] / mcnt[1]) : 0.f,
          mcnt[2] ? (float)(maccum[2] / mcnt[2]) : 0.f };

  // Soft luminance affinity for the FIT WINDOWS: pixels much darker than the blown zone's
  // plateau contribute ~nothing to the windowed moments, so a window straddling a dark
  // occluder and the sky fits the SKY's colour-line instead of a poisoned mixture. Content
  // brighter than ~a third of the plateau keeps full weight, so unoccluded scenes are
  // untouched by construction.
  const float cf_binv = (cf_lref > 1e-9f) ? 1.f / (0.35f * cf_lref) : 0.f;

  // broad-anchor mask for the model-quality plane (bounded even where the fit degenerates)
  uint8_t *const restrict hole2
      = (uint8_t *)dt_pixelpipe_cache_alloc_align(sizeof(uint8_t) * region_pixels, ctx->pipe);

  // bsc collects the DIFFUSED fit quality (R^2) per channel: the weight of the
  // high-frequency damping and of the depth-gated self-dome blend below. It is spatially
  // smooth (windowed moments + harmonic fill), so neither conaccumer introduces a hand-off.
  HL_PFOR()
  for(size_t i = 0; i < region_pixels; i++)
    for(int c = 0; c < 4; c++) guide_score[i * 4 + c] = 0.f;

  // The TEN blurred moment planes of the fit (article step 3: "solved from ten blurred moment
  // planes ... through the 2x2 normal equations"). Each _region_blur below IS the windowed weighted
  // sum Sum_y w(y) G_sigma(x-y) (.) : packing the per-pixel product then Gaussian-blurring gives the
  // windowed moment at x. w(y) = [all three channels valid] * lum_weight (the soft occlusion weight).
  // Moment 1 of 3 (blur -> prev_scale): the mass count n and the 3 centred means.
  // joint windowed moments, weight = all three channels valid at the pixel, packed as
  // prev = [n, wR, wG, wB], s1 = [wRR, wGG, wBB, wRG], s3 = [wRB, wGB, 0, 0]
  HL_PFOR()
  for(size_t i = 0; i < region_pixels; i++)
  {
    const float lum_weight = (cf_binv > 0.f) ? sqf(fminf(lum_accum[i] * cf_binv, 1.f)) : 1.f;
    const float weight
        = (valid[i * 4 + 0] >= 0.5f && valid[i * 4 + 1] >= 0.5f && valid[i * 4 + 2] >= 0.5f) ? lum_weight : 0.f;
    blur_in[i * 4 + 0] = weight;                                            // Sum w -> n (trusted mass)
    blur_in[i * 4 + 1] = weight * (estimate[i * 4 + 0] - channel_means[0]); // Sum w*(R-Rbar) -> centred mean of R
    blur_in[i * 4 + 2] = weight * (estimate[i * 4 + 1] - channel_means[1]); // Sum w*(G-Gbar) -> centred mean of G
    blur_in[i * 4 + 3] = weight * (estimate[i * 4 + 2] - channel_means[2]); // Sum w*(B-Bbar) -> centred mean of B
  }

  _region_blur(blur_in, prev_scale, region_w, region_h, cf_sigma);

  HL_PFOR()
  for(size_t i = 0; i < region_pixels; i++)
  {
    const float lum_weight = (cf_binv > 0.f) ? sqf(fminf(lum_accum[i] * cf_binv, 1.f)) : 1.f;
    const float weight
        = (valid[i * 4 + 0] >= 0.5f && valid[i * 4 + 1] >= 0.5f && valid[i * 4 + 2] >= 0.5f) ? lum_weight : 0.f;
    // Moment 2 of 3 (blur -> plane1): four of the six centred second moments (products of x-xbar)
    const float val_r
        = estimate[i * 4 + 0] - channel_means[0]; // R - Rbar (centred, avoids the E[u^2]-E[u]^2 cancellation)
    const float val_g = estimate[i * 4 + 1] - channel_means[1]; // G - Gbar
    const float val_b = estimate[i * 4 + 2] - channel_means[2]; // B - Bbar
    blur_in[i * 4 + 0] = weight * val_r * val_r;                // -> E[(R-Rbar)^2] = Var(R)
    blur_in[i * 4 + 1] = weight * val_g * val_g;                // -> Var(G)
    blur_in[i * 4 + 2] = weight * val_b * val_b;                // -> Var(B)
    blur_in[i * 4 + 3] = weight * val_r * val_g;                // -> Cov(R,G)
  }

  _region_blur(blur_in, plane1, region_w, region_h, cf_sigma);

  HL_PFOR()
  for(size_t i = 0; i < region_pixels; i++)
  {
    const float lum_weight = (cf_binv > 0.f) ? sqf(fminf(lum_accum[i] * cf_binv, 1.f)) : 1.f;
    const float weight
        = (valid[i * 4 + 0] >= 0.5f && valid[i * 4 + 1] >= 0.5f && valid[i * 4 + 2] >= 0.5f) ? lum_weight : 0.f;
    // Moment 3 of 3 (blur -> plane3): the last two centred second moments + the unweighted mass
    blur_in[i * 4 + 0] = weight * (estimate[i * 4 + 0] - channel_means[0])
                         * (estimate[i * 4 + 2] - channel_means[2]); // -> Cov(R,B)
    blur_in[i * 4 + 1] = weight * (estimate[i * 4 + 1] - channel_means[1])
                         * (estimate[i * 4 + 2] - channel_means[2]); // -> Cov(G,B)
    blur_in[i * 4 + 2] = (valid[i * 4 + 0] >= 0.5f && valid[i * 4 + 1] >= 0.5f && valid[i * 4 + 2] >= 0.5f)
                             ? 1.f
                             : 0.f; // UNWEIGHTED valid mass: anchors must exist at the rim
    blur_in[i * 4 + 3] = 0.f;
  }

  _region_blur(blur_in, plane3, region_w, region_h, cf_sigma);

  // second-moment plane lookup: diag (c,c) -> s1 slot c; off-diag (a,b) -> slot 2+a+b,
  // where slots 3 = RG (s1), 4 = RB (s3[0]), 5 = GB (s3[1])
  // CF_M2(i, a, b) returns the (unnormalized) windowed sum Sum w*(u_a-ubar_a)(u_b-ubar_b) at pixel i,
  // i.e. n*Cov(u_a,u_b) once divided by the mass n -- the raw material of the normal matrix Sigma.
#define CF_M2(nb_index, coef_a, coef_b)                                                                           \
  (((coef_a) == (coef_b)) ? plane1[(nb_index) * 4 + (coef_a)]                                                     \
                          : ((2 + (coef_a) + (coef_b)) < 4 ? plane1[(nb_index) * 4 + 2 + (coef_a) + (coef_b)]     \
                                                           : plane3[(nb_index) * 4 + (coef_a) + (coef_b) - 2]))

  // The DEEP channel (most clipped pixels: its zone contains the multi-clip cores) is not
  // evaluated here -- its diffused coefficients are STASHED and evaluated after the pair
  // fallbacks have reconstructed the other clipped channels, so its joint model reads
  // CONTINUOUS guides everywhere. Evaluating it against a guide that jumps from measured
  // to clip-plateau at the guide's own clip contour printed that contour as an arc.
  size_t nclip_c[3] = { 0, 0, 0 };
  for(size_t i = 0; i < region_pixels; i++)
    for(int c = 0; c < 3; c++)
      if(valid[i * 4 + c] < 0.5f) nclip_c[c]++;

  // cdeep = the channel with the most clipped pixels (its zone holds the multi-clip cores)
  const int cdeep
      = (nclip_c[0] >= nclip_c[1] && nclip_c[0] >= nclip_c[2]) ? 0 : ((nclip_c[1] >= nclip_c[2]) ? 1 : 2);
  int deep_stashed = 0;

  // ---- per channel: joint 2-guide coefficients, harmonic diffusion, evaluation ----
  for(int c = 0; c < 3; c++)
  {
    // guide-pair selection: predict clipped channel v = c from its two OTHER channels u1=guide1, u2=guide2
    const int guide1 = (c == 0) ? 1 : 0;
    const int guide2 = (c == 2) ? 1 : 2;

    size_t ntarget = 0;
    HL_PFOR(reduction(+ : ntarget))
    for(size_t i = 0; i < region_pixels; i++)
      if(valid[i * 4 + c] < 0.5f && (valid[i * 4 + guide1] >= 0.5f || valid[i * 4 + guide2] >= 0.5f)) ntarget++;

    if(ntarget == 0) continue;

    // NOTE the ntarget gate accepts ONE surviving guide so the DEEP channel is fitted
    // (and stashed) even when its zone has no strict two-guide pixel; the immediate
    // evaluation below stays strict.

    // coefficients (a, b, d) from the windowed moments at every pixel (garbage where the
    // window held no trusted mass -- replaced by the diffusion); anchor = trusted window
    // Solve the 2x2 normal equations of the weighted least squares at every pixel (article step 3):
    //   Sigma [a;b] = [Cov(u1,v); Cov(u2,v)],  Sigma = [[Var u1, Cov(u1,u2)],[Cov(u1,u2), Var u2]]
    // via Cramer's rule, with relative Tikhonov ridge lambda added to the diagonal.
    HL_PFOR()
    for(size_t i = 0; i < region_pixels; i++)
    {
      const float norm = fmaxf(prev_scale[i * 4 + 0], 1e-9f); // n = windowed trusted mass
      const float inv_det = 1.f / norm;                       // 1/n, turns the summed moments into expectations
      const float mean1 = prev_scale[i * 4 + 1 + guide1] * inv_det;  // E[u1] (of the centred pack)
      const float mean2 = prev_scale[i * 4 + 1 + guide2] * inv_det;  // E[u2]
      const float mean_target = prev_scale[i * 4 + 1 + c] * inv_det; // E[v]
      const float var11
          = fmaxf(CF_M2(i, guide1, guide1) * inv_det - mean1 * mean1, 0.f); // Var(u1) = E[u1^2]-E[u1]^2
      const float var22 = fmaxf(CF_M2(i, guide2, guide2) * inv_det - mean2 * mean2, 0.f); // Var(u2)
      const float var12 = CF_M2(i, guide1, guide2) * inv_det - mean1 * mean2;             // Cov(u1,u2)
      const float cov_tg1 = CF_M2(i, c, guide1) * inv_det - mean_target * mean1;          // Cov(v,u1) = RHS_1
      const float cov_tg2 = CF_M2(i, c, guide2) * inv_det - mean_target * mean2;          // Cov(v,u2) = RHS_2

      const float var_target
          = fmaxf(CF_M2(i, c, c) * inv_det - mean_target * mean_target, 0.f); // Var(v), denom of R^2

      // relative Tikhonov: scales with the signal, never eats a weak-but-real slope
      const float lambda = 1e-3f * 0.5f * (var11 + var22) + 1e-12f; // ridge = 1e-3 * (Var u1 + Var u2)/2
      const float determinant
          = fmaxf((var11 + lambda) * (var22 + lambda) - var12 * var12, 1e-18f); // det Sigma (with ridge)
      const float slope_a
          = ((var22 + lambda) * cov_tg1 - var12 * cov_tg2) / determinant; // a = (Sigma^-1 RHS)_1 (Cramer)
      const float slope_b
          = ((var11 + lambda) * cov_tg2 - var12 * cov_tg1) / determinant; // b = (Sigma^-1 RHS)_2 (Cramer)
      const float r_sq = CLAMP((slope_a * cov_tg1 + slope_b * cov_tg2) / (var_target + 1e-12f), 0.f,
                               1.f); // R^2 = (a Cov(v,u1)+b Cov(v,u2)) / Var(v) = explained/total

      valid_variance[i * 4 + 0] = slope_a;
      valid_variance[i * 4 + 1] = slope_b;
      // intercept of the CENTERED fit, unshifted back to absolute values: d = E[v] - a E[u1] - b E[u2]
      valid_variance[i * 4 + 2] = (mean_target + channel_means[c]) - slope_a * (mean1 + channel_means[guide1])
                                  - slope_b * (mean2 + channel_means[guide2]);
      valid_variance[i * 4 + 3] = r_sq;

      // anchor = trusted window AND a sane fit: degenerate (near-zero-variance) windows
      // produce exploding slopes that would poison the diffusion boundary
      // anchors EXIST wherever enough valid pixels are in reach (continuity at the rim
      // needs locally-exact fits there), and their weighted fits are bright-content-pure;
      // windows that are MOSTLY dark (weighted mass a small fraction of the valid mass)
      // describe unrelated content and must not anchor
      const int mass_ok = (plane3[i * 4 + 2] > cf_fmin && prev_scale[i * 4 + 0] > 0.25f * plane3[i * 4 + 2]);
      // anchor gate (article: R^2 > 0.25 with bounded slopes) -> the Dirichlet data for E_transport.
      // hole = NOT an anchor (the cell to be filled by the transport); |a|,|b| < 64 rejects only
      // degenerate near-zero-variance windows whose exploding slopes would poison the fill boundary.
      hole[i] = !(mass_ok && valid[i * 4 + c] >= 0.5f && r_sq > 0.25f && fabsf(slope_a) < 64.f
                  && fabsf(slope_b) < 64.f);
      if(hole2)
        hole2[i] = !(mass_ok && valid[i * 4 + c] >= 0.5f); // broader (mass-only) anchor set for the R^2 plane
    }

    // harmonic diffusion of each coefficient field into the non-anchor area (stable
    // coarse-to-fine Jacobi fill; base grid at ~sigma/4 since coefficients are smooth).
    // a/b/d share the anchor mask, so they ride ONE fused fill (one mask pyramid, one
    // tensor, one sweep pass); r2 may use its own broader mask and fills alone.
    {
      HL_PFOR()
      for(size_t i = 0; i < region_pixels; i++)
      {
        fill_planes[i] = valid_variance[i * 4 + 0];
        fill_planes[region_pixels + i] = valid_variance[i * 4 + 1];
        fill_planes[2 * region_pixels + i] = valid_variance[i * 4 + 2];
        solver_field[i] = valid_variance[i * 4 + 3];
      }

      // E_transport on p in {a, b, d}: anchored anisotropic fill, base grid pitch ~sigma/4 (article "Cell")
      float *planes[3] = { fill_planes, fill_planes + region_pixels, fill_planes + 2 * region_pixels };
      _cf_harmonic_fill_n((float *const restrict *)planes, 3, hole, region_w, region_h, (int)(cf_sigma / 4.f),
                          steer, pipe);
      // the R^2 plane is diffused too (article: "R^2 is diffused alongside (a,b,d) as a fourth plane"),
      // on the broader mass-only anchor set so it stays bounded even where the fit degenerates
      _cf_harmonic_fill(solver_field, hole2 ? hole2 : hole, region_w, region_h, (int)(cf_sigma / 4.f), steer, pipe);

      HL_PFOR()
      for(size_t i = 0; i < region_pixels; i++)
      {
        valid_variance[i * 4 + 0] = fill_planes[i];
        valid_variance[i * 4 + 1] = fill_planes[region_pixels + i];
        valid_variance[i * 4 + 2] = fill_planes[2 * region_pixels + i];
        valid_variance[i * 4 + 3] = solver_field[i];
      }
    }

    // evaluate against the measured guides at every joint target pixel, keeping the
    // diffused fit R^2 (in-sample R^2 is the honest quality signal: decorrelated content
    // simply has no colour-line and scores 0.25..0.6 against ~0.9 for correlated content)
    if(c == cdeep)
    {
      // stash the diffused fields (dbuf/tbuf/ldb/bsc slot 3 are free until the HF and
      // dome stages); evaluated after the pair fallbacks below
      HL_PFOR()
      for(size_t i = 0; i < region_pixels; i++)
      {
        reaction_weight[i] = valid_variance[i * 4 + 0];
        flat_target[i] = valid_variance[i * 4 + 1];
        dome_lum[i] = valid_variance[i * 4 + 2];
        guide_score[i * 4 + 3] = valid_variance[i * 4 + 3];
      }
      deep_stashed = 1;
      continue;
    }

    // strict two-guide gate: extending this evaluation into the multi-clip band (with the
    // clipped guide at its plateau) was tried and regressed the correlated synthetics --
    // continuity there is the deep channel's deferred evaluation's job, and the non-deep
    // channels' pair fits are locally anchored at their own fences anyway
    HL_PFOR()
    for(size_t i = 0; i < region_pixels; i++)
      if(valid[i * 4 + c] < 0.5f && valid[i * 4 + guide1] >= 0.5f && valid[i * 4 + guide2] >= 0.5f)
      {
        // evaluation v_hat = a*u1 + b*u2 + d against the MEASURED guides (diffused a,b,d; true u1,u2)
        estimate[i * 4 + c] = valid_variance[i * 4 + 0] * estimate[i * 4 + guide1]
                              + valid_variance[i * 4 + 1] * estimate[i * 4 + guide2] + valid_variance[i * 4 + 2];
        guide_score[i * 4 + c]
            = CLAMP(valid_variance[i * 4 + 3], 0.f, 1.f); // carry the diffused R^2 as the model quality
      }
  }

#undef CF_M2

  // ---- single-guide fallback for 2-clip pixels (target + one other channel clipped) ----
  // Article step 3: "Pixels with a single surviving guide get the same treatment with a one-guide
  // fit." Same fit+transport+evaluate, but the model collapses to v_hat = a*u + d (one guide u):
  // a = Cov(u,v)/Var(u) (1x1 normal equation), R^2 = Cov(u,v)^2 / (Var(u) Var(v)) = squared correlation.
  size_t n2clip = 0;
  HL_PFOR(reduction(+ : n2clip))
  for(size_t i = 0; i < region_pixels; i++)
  {
    const int n_valid = (valid[i * 4 + 0] >= 0.5f) + (valid[i * 4 + 1] >= 0.5f) + (valid[i * 4 + 2] >= 0.5f);
    if(n_valid == 1) n2clip++;
  }

  if(n2clip > 0)
    for(int chan_a = 0; chan_a < 3; chan_a++)
      for(int chan_b = chan_a + 1; chan_b < 3; chan_b++)
      {
        // pair moments, weight = both channels of the pair valid, packed as
        // s2 = [n, wa, wb, waa], s3 = [wbb, wab, unweighted n, 0]
        HL_PFOR()
        for(size_t i = 0; i < region_pixels; i++)
        {
          const float lum_weight = (cf_binv > 0.f) ? sqf(fminf(lum_accum[i] * cf_binv, 1.f)) : 1.f;
          const float weight = (valid[i * 4 + chan_a] >= 0.5f && valid[i * 4 + chan_b] >= 0.5f) ? lum_weight : 0.f;
          const float var_a = estimate[i * 4 + chan_a] - channel_means[chan_a];
          const float var_b = estimate[i * 4 + chan_b] - channel_means[chan_b];
          blur_in[i * 4 + 0] = weight;
          blur_in[i * 4 + 1] = weight * var_a;
          blur_in[i * 4 + 2] = weight * var_b;
          blur_in[i * 4 + 3] = weight * var_a * var_a;
        }

        _region_blur(blur_in, plane2, region_w, region_h, cf_sigma);

        HL_PFOR()
        for(size_t i = 0; i < region_pixels; i++)
        {
          const float lum_weight = (cf_binv > 0.f) ? sqf(fminf(lum_accum[i] * cf_binv, 1.f)) : 1.f;
          const float weight = (valid[i * 4 + chan_a] >= 0.5f && valid[i * 4 + chan_b] >= 0.5f) ? lum_weight : 0.f;
          const float var_a = estimate[i * 4 + chan_a] - channel_means[chan_a];
          const float var_b = estimate[i * 4 + chan_b] - channel_means[chan_b];
          blur_in[i * 4 + 0] = weight * var_b * var_b;
          blur_in[i * 4 + 1] = weight * var_a * var_b;
          blur_in[i * 4 + 2] = (valid[i * 4 + chan_a] >= 0.5f && valid[i * 4 + chan_b] >= 0.5f) ? 1.f : 0.f;
          blur_in[i * 4 + 3] = 0.f;
        }

        _region_blur(blur_in, plane3, region_w, region_h, cf_sigma);

        // both orientations: predict a from b, then b from a
        for(int orient = 0; orient < 2; orient++)
        {
          const int target_chan = orient ? chan_b : chan_a; // target channel
          const int guide_chan = orient ? chan_a : chan_b;  // guide channel
          const int other_chan = 3 - chan_a - chan_b;       // the third channel, must be clipped at the target

          size_t ntarget = 0;
          HL_PFOR(reduction(+ : ntarget))
          for(size_t i = 0; i < region_pixels; i++)
            if(valid[i * 4 + target_chan] < 0.5f && valid[i * 4 + guide_chan] >= 0.5f
               && valid[i * 4 + other_chan] < 0.5f)
              ntarget++;

          if(ntarget == 0) continue;

          HL_PFOR()
          for(size_t i = 0; i < region_pixels; i++)
          {
            const float norm = fmaxf(plane2[i * 4 + 0], 1e-9f);
            const float inv_det = 1.f / norm;
            const float pair_mean_target = plane2[i * 4 + (orient ? 2 : 1)] * inv_det;
            const float mean_guide = plane2[i * 4 + (orient ? 1 : 2)] * inv_det;
            const float var_guide
                = fmaxf((orient ? plane2[i * 4 + 3] : plane3[i * 4 + 0]) * inv_det - mean_guide * mean_guide,
                        0.f); // Var(u) (guide)
            const float var_t = fmaxf((orient ? plane3[i * 4 + 0] : plane2[i * 4 + 3]) * inv_det
                                          - pair_mean_target * pair_mean_target,
                                      0.f); // Var(v) (target), denom of R^2
            const float covariance = plane3[i * 4 + 1] * inv_det - pair_mean_target * mean_guide; // Cov(u,v)
            const float slope_a
                = covariance / (var_guide * (1.f + 1e-3f) + 1e-12f); // a = Cov(u,v)/Var(u), 1e-3 relative ridge
            const float r_sq = CLAMP(covariance * covariance / (var_guide * var_t + 1e-18f), 0.f,
                                     1.f); // R^2 = Cov^2/(Var u Var v)

            valid_variance[i * 4 + 0] = slope_a;
            // intercept of the CENTERED fit, unshifted back to absolute values: d = E[v] - a E[u]
            valid_variance[i * 4 + 1] = (pair_mean_target + channel_means[target_chan])
                                        - slope_a * (mean_guide + channel_means[guide_chan]);
            valid_variance[i * 4 + 2] = r_sq;
            const int mass_ok = (plane3[i * 4 + 2] > cf_fmin && plane2[i * 4 + 0] > 0.25f * plane3[i * 4 + 2]);
            hole[i] = !(mass_ok && valid[i * 4 + target_chan] >= 0.5f && r_sq > 0.25f && fabsf(slope_a) < 64.f);
            if(hole2) hole2[i] = !(mass_ok && valid[i * 4 + target_chan] >= 0.5f);
          }

          // slope and intercept share the anchor mask -> one fused fill; r2 may use its
          // own broader mask and fills alone
          {
            HL_PFOR()
            for(size_t i = 0; i < region_pixels; i++)
            {
              fill_planes[i] = valid_variance[i * 4 + 0];
              fill_planes[region_pixels + i] = valid_variance[i * 4 + 1];
              solver_field[i] = valid_variance[i * 4 + 2];
            }

            float *planes[2] = { fill_planes, fill_planes + region_pixels };
            _cf_harmonic_fill_n((float *const restrict *)planes, 2, hole, region_w, region_h,
                                (int)(cf_sigma / 4.f), steer, pipe);
            _cf_harmonic_fill(solver_field, hole2 ? hole2 : hole, region_w, region_h, (int)(cf_sigma / 4.f), steer,
                              pipe);

            HL_PFOR()
            for(size_t i = 0; i < region_pixels; i++)
            {
              valid_variance[i * 4 + 0] = fill_planes[i];
              valid_variance[i * 4 + 1] = fill_planes[region_pixels + i];
              valid_variance[i * 4 + 2] = solver_field[i];
            }
          }

          // FEATHERED hand-off: instead of switching hard to the pair model exactly where
          // the third channel clips (its contour prints the joint/pair disagreement as an
          // arc), blend by the blurred oc-clip mask -- ~0 far into the joint region, ~1
          // deep into the multi-clip band, ~0.5 at the contour where BOTH estimates are
          // continuous extrapolations. est currently holds the extended joint estimate.
          // (A sharper ramp was tried and regressed the outer-contour smoothness.)
          // hard write at the multi-clip pixels (the iter-3 semantics). For the deep
          // channel this is only the DEEP-CORE estimate: the deferred stashed-joint
          // evaluation below owns the fence and blends this back in by depth. For the
          // other channels the pair fit is locally anchored at their fence (both its
          // channels are measured in the adjacent band), so the hard write is already
          // continuous there. A feathered joint-ext blend over this write was tried and
          // regressed the correlated synthetics without helping the arc.
          HL_PFOR()
          for(size_t i = 0; i < region_pixels; i++)
            if(valid[i * 4 + target_chan] < 0.5f && valid[i * 4 + guide_chan] >= 0.5f
               && valid[i * 4 + other_chan] < 0.5f)
            {
              // evaluation v_hat = a*u + d against the measured guide (diffused a,d; true u)
              estimate[i * 4 + target_chan]
                  = valid_variance[i * 4 + 0] * estimate[i * 4 + guide_chan] + valid_variance[i * 4 + 1];
              guide_score[i * 4 + target_chan] = CLAMP(valid_variance[i * 4 + 2], 0.f, 1.f);
            }
        }
      }

  // ---- deep-channel evaluation from the stashed joint model ----
  // Runs after the pair fallbacks so a clipped guide reads as its RECONSTRUCTION (itself
  // continuous: the pair fit of a less-clipped channel is anchored in the adjacent band
  // where both its channels are measured). Smooth coefficient fields x continuous guides
  // = no estimator hand-off anywhere inside the deep channel's zone -- the arc the hard
  // joint <-> pair switch used to print at the second guide's clip contour cannot form.
  // DEPTH SPLIT: the chained evaluation is only NEEDED near the multi-clip fence; deep
  // inside the core the direct pair colour-line is the better estimator on correlated
  // content (one hop, no compounded reconstruction error). Blend pair over stashed-joint
  // by a smoothstep of the blurred multi-clip mask: ~0 at the fence (mask ~0.5 there),
  // ~1 deep inside. Smooth weight x smooth fields = still no printable level set.
  if(deep_stashed)
  {
    const int guide1 = (cdeep == 0) ? 1 : 0;
    const int guide2 = (cdeep == 2) ? 1 : 2;

    HL_PFOR()
    for(size_t i = 0; i < region_pixels; i++)
    {
      blur_in[i * 4 + 0]
          = (valid[i * 4 + cdeep] < 0.5f && (valid[i * 4 + guide1] < 0.5f || valid[i * 4 + guide2] < 0.5f)) ? 1.f
                                                                                                            : 0.f;
      blur_in[i * 4 + 1] = blur_in[i * 4 + 2] = blur_in[i * 4 + 3] = 0.f;
    }

    _region_blur(blur_in, plane2, region_w, region_h,
                 cf_sigma); // s2 is free scratch here (pair moments are done)

    HL_PFOR()
    for(size_t i = 0; i < region_pixels; i++)
    {
      const int anyvalid = (valid[i * 4 + 0] >= 0.5f) || (valid[i * 4 + 1] >= 0.5f) || (valid[i * 4 + 2] >= 0.5f);
      if(valid[i * 4 + cdeep] < 0.5f && anyvalid)
      {
        // deferred evaluation of the stashed deep-channel joint model: v_hat = a*u1 + b*u2 + d
        // (a=reaction_weight, b=flat_target, d=dome_lum are the stashed diffused coefficients)
        const float joint = reaction_weight[i] * estimate[i * 4 + guide1]
                            + flat_target[i] * estimate[i * 4 + guide2] + dome_lum[i];
        // pair values exist only at multi-clip px (the pair loop's write gate)
        const int has_pair = (valid[i * 4 + guide1] < 0.5f || valid[i * 4 + guide2] < 0.5f);
        const float pair_conf = CLAMP(plane2[i * 4 + 0], 0.f, 1.f);
        const float smooth_t = CLAMP((pair_conf - 0.7f) / 0.25f, 0.f, 1.f);
        const float floor_width = has_pair ? smooth_t * smooth_t * (3.f - 2.f * smooth_t) : 0.f;
        estimate[i * 4 + cdeep] = floor_width * estimate[i * 4 + cdeep] + (1.f - floor_width) * joint;
        guide_score[i * 4 + cdeep] = floor_width * guide_score[i * 4 + cdeep]
                                     + (1.f - floor_width) * CLAMP(guide_score[i * 4 + 3], 0.f, 1.f);
      }
    }
  }

  // MATHS BRIDGE -- Step 4 (HF refit), article §"Hybrid Laplacian-band guiding of the high
  // frequencies" / §"Rebuild the high frequencies": the estimate is split at sigma/4 into a low band
  // ubar (plane2 below) and a detail band u - ubar. The detail band gets its OWN windowed colour-line
  // with R^2-shrunk gains (on a zero-mean band shrinkage is the correct estimator: no magnitude to
  // lose, only noise to not print), and the HF is blended between this guided resynthesis
  // h_g = a(u_g1-ubar_g1)+b(u_g2-ubar_g2) and the R^2-damped transfer h_d = R^2 (u_c - ubar_c) by
  // quadratic min-energy odds w = e_d^2/(e_d^2 + e_g^2), e_{d,g} = blurred |HF_{d,g}| -- an edge
  // misfire spikes the guided HF energy e_g, so w -> 0 and the damped path wins exactly there (the
  // failure self-detects, no content discriminator needed). Note the band split blurs at sigma/4
  // (floored at 2 px) while the moments below blur at the fit's cf_sigma -- two deliberate scales.
  //
  // R^2-scaled HIGH-FREQUENCY damping: where the colour-line is weak, the guides' fine
  // texture is unrelated to the truth and must not be printed onto the reconstruction.
  // Continuous in the quality weight -- no estimator hand-off.
  memcpy(blur_in, estimate, region_pixels * 4 * sizeof(float));
  _region_blur(blur_in, plane2, region_w, region_h,
               fmaxf(cf_sigma / 4.f, 2.f)); // ubar = low band, Gaussian at sigma/4 (>= 2 px)

  // ---- Laplacian-band guiding (see the DT_HL_HF_GUIDE macro comment) ----
  // detail-band moments, weight = all three channels valid; packed exactly like the
  // full-signal moments: prev = [n, hR, hG, hB], s1 = [hRR, hGG, hBB, hRG], s3 = [hRB, hGB]
  HL_PFOR()
  for(size_t i = 0; i < region_pixels; i++)
  {
    const float lum_weight = (cf_binv > 0.f) ? sqf(fminf(lum_accum[i] * cf_binv, 1.f)) : 1.f;
    const float weight
        = (valid[i * 4 + 0] >= 0.5f && valid[i * 4 + 1] >= 0.5f && valid[i * 4 + 2] >= 0.5f) ? lum_weight : 0.f;
    // detail band H = est - ubar (plane2), weighted; packed like the CF moments = [n, hR, hG, hB]
    blur_in[i * 4 + 0] = weight;
    blur_in[i * 4 + 1] = weight * (estimate[i * 4 + 0] - plane2[i * 4 + 0]);
    blur_in[i * 4 + 2] = weight * (estimate[i * 4 + 1] - plane2[i * 4 + 1]);
    blur_in[i * 4 + 3] = weight * (estimate[i * 4 + 2] - plane2[i * 4 + 2]);
  }

  _region_blur(blur_in, prev_scale, region_w, region_h,
               cf_sigma); // windowed means of the detail band (blur at fit sigma)

  HL_PFOR()
  for(size_t i = 0; i < region_pixels; i++)
  {
    const float weight = blur_in[i * 4 + 0];
    const float hf_r = estimate[i * 4 + 0] - plane2[i * 4 + 0];
    const float hf_g = estimate[i * 4 + 1] - plane2[i * 4 + 1];
    const float hf_b = estimate[i * 4 + 2] - plane2[i * 4 + 2];
    blur_in[i * 4 + 0] = weight * hf_r * hf_r;
    blur_in[i * 4 + 1] = weight * hf_g * hf_g;
    blur_in[i * 4 + 2] = weight * hf_b * hf_b;
    blur_in[i * 4 + 3] = weight * hf_r * hf_g;
  }

  _region_blur(blur_in, plane1, region_w, region_h, cf_sigma);

  HL_PFOR()
  for(size_t i = 0; i < region_pixels; i++)
  {
    const float lum_weight = (cf_binv > 0.f) ? sqf(fminf(lum_accum[i] * cf_binv, 1.f)) : 1.f;
    const float weight
        = (valid[i * 4 + 0] >= 0.5f && valid[i * 4 + 1] >= 0.5f && valid[i * 4 + 2] >= 0.5f) ? lum_weight : 0.f;
    blur_in[i * 4 + 0]
        = weight * (estimate[i * 4 + 0] - plane2[i * 4 + 0]) * (estimate[i * 4 + 2] - plane2[i * 4 + 2]);
    blur_in[i * 4 + 1]
        = weight * (estimate[i * 4 + 1] - plane2[i * 4 + 1]) * (estimate[i * 4 + 2] - plane2[i * 4 + 2]);
    blur_in[i * 4 + 2] = (valid[i * 4 + 0] >= 0.5f && valid[i * 4 + 1] >= 0.5f && valid[i * 4 + 2] >= 0.5f)
                             ? 1.f
                             : 0.f; // unweighted valid mass for the anchor gate
    blur_in[i * 4 + 3] = 0.f;
  }

  _region_blur(blur_in, plane3, region_w, region_h, cf_sigma);

  // HF_M2(i, a, b) returns the windowed sum Sum w * H_a * H_b at pixel i (H = detail band, already
  // zero-mean, so no centering needed unlike CF_M2), indexing the packed second-moment planes:
  // diag (a==b) in plane1[0..2], RG/RB in plane1[3]/plane3[0], GB in plane3[1] -- feeds Var/Cov below
#define HF_M2(nb_index, coef_a, coef_b)                                                                           \
  (((coef_a) == (coef_b)) ? plane1[(nb_index) * 4 + (coef_a)]                                                     \
                          : ((2 + (coef_a) + (coef_b)) < 4 ? plane1[(nb_index) * 4 + 2 + (coef_a) + (coef_b)]     \
                                                           : plane3[(nb_index) * 4 + (coef_a) + (coef_b) - 2]))

  for(int c = 0; c < 3; c++)
  {
    const int guide1 = (c == 0) ? 1 : 0;
    const int guide2 = (c == 2) ? 1 : 2;

    size_t ntarget = 0;
    HL_PFOR(reduction(+ : ntarget))
    for(size_t i = 0; i < region_pixels; i++)
      if(valid[i * 4 + c] < 0.5f && valid[i * 4 + guide1] >= 0.5f && valid[i * 4 + guide2] >= 0.5f) ntarget++;

    if(ntarget == 0) continue;

    // R^2-shrunk detail-band gains at every pixel; anchors = trusted mass + bounded slopes
    HL_PFOR()
    for(size_t i = 0; i < region_pixels; i++)
    {
      const float norm = fmaxf(prev_scale[i * 4 + 0], 1e-9f);
      const float inv_det = 1.f / norm;
      const float mean1 = prev_scale[i * 4 + 1 + guide1] * inv_det;
      const float mean2 = prev_scale[i * 4 + 1 + guide2] * inv_det;
      const float mean_target = prev_scale[i * 4 + 1 + c] * inv_det;
      // same 2x2 normal equations as the CF fit, but on the detail-band moments (article step 4):
      // solve Sigma [a;b] = [Cov(H_u1,H_c); Cov(H_u2,H_c)] by Cramer's rule. u1=guide1, u2=guide2, v=c.
      const float var11 = fmaxf(HF_M2(i, guide1, guide1) * inv_det - mean1 * mean1, 0.f); // Var(H_u1)
      const float var22 = fmaxf(HF_M2(i, guide2, guide2) * inv_det - mean2 * mean2, 0.f); // Var(H_u2)
      const float var12 = HF_M2(i, guide1, guide2) * inv_det - mean1 * mean2;             // Cov(H_u1,H_u2)
      const float cov_tg1 = HF_M2(i, c, guide1) * inv_det - mean_target * mean1;          // Cov(H_v,H_u1) = RHS_1
      const float cov_tg2 = HF_M2(i, c, guide2) * inv_det - mean_target * mean2;          // Cov(H_v,H_u2) = RHS_2
      const float var_target
          = fmaxf(HF_M2(i, c, c) * inv_det - mean_target * mean_target, 0.f); // Var(H_v), denom of R^2

      const float lambda = 1e-3f * 0.5f * (var11 + var22) + 1e-12f; // relative Tikhonov ridge
      const float determinant = fmaxf((var11 + lambda) * (var22 + lambda) - var12 * var12, 1e-18f); // det Sigma
      const float hf_a = ((var22 + lambda) * cov_tg1 - var12 * cov_tg2) / determinant;              // a (Cramer)
      const float hf_b_slope = ((var11 + lambda) * cov_tg2 - var12 * cov_tg1) / determinant;        // b (Cramer)
      const float hf_r2 = CLAMP((hf_a * cov_tg1 + hf_b_slope * cov_tg2) / (var_target + 1e-12f), 0.f, 1.f); // R^2

      // R^2-shrunk gains g*R^2 (correct estimator on a zero-mean band): stashed for the diffusion below
      reaction_weight[i] = hf_a * hf_r2;
      flat_target[i] = hf_b_slope * hf_r2;
      hole[i] = !(plane3[i * 4 + 2] > cf_fmin && prev_scale[i * 4 + 0] > 0.25f * plane3[i * 4 + 2]
                  && valid[i * 4 + c] >= 0.5f && fabsf(reaction_weight[i]) < 64.f && fabsf(flat_target[i]) < 64.f);
    }

    {
      // the two HF gain planes share the anchor mask -> one fused fill
      float *planes[2] = { reaction_weight, flat_target };
      _cf_harmonic_fill_n((float *const restrict *)planes, 2, hole, region_w, region_h, (int)(cf_sigma / 4.f),
                          steer, pipe);
    }

    // both HF candidates + their local energies (blurred |.|), packed into varc via one blur
    HL_PFOR()
    for(size_t i = 0; i < region_pixels; i++)
    {
      // h_g = a(u_g1-ubar_g1) + b(u_g2-ubar_g2): guide-transferred detail (diffused shrunk gains)
      const float hf_guided = reaction_weight[i] * (estimate[i * 4 + guide1] - plane2[i * 4 + guide1])
                              + flat_target[i] * (estimate[i * 4 + guide2] - plane2[i * 4 + guide2]);
      // h_d = R^2 (u_c - ubar_c): the channel's own detail damped by its fit quality
      const float hf_damped = CLAMP(guide_score[i * 4 + c], 0.f, 1.f) * (estimate[i * 4 + c] - plane2[i * 4 + c]);
      blur_in[i * 4 + 0] = fabsf(hf_guided); // |h_g| -> blurred to e_g
      blur_in[i * 4 + 1] = fabsf(hf_damped); // |h_d| -> blurred to e_d
      blur_in[i * 4 + 2] = 0.f;
      blur_in[i * 4 + 3] = 0.f;
    }

    _region_blur(blur_in, valid_variance, region_w, region_h, fmaxf(cf_sigma / 4.f, 2.f));

    // quadratic min-energy blend of the two HF sources, then resynthesize
    HL_PFOR()
    for(size_t i = 0; i < region_pixels; i++)
      if(valid[i * 4 + c] < 0.5f && valid[i * 4 + guide1] >= 0.5f && valid[i * 4 + guide2] >= 0.5f)
      {
        const float hf_guided = reaction_weight[i] * (estimate[i * 4 + guide1] - plane2[i * 4 + guide1])
                                + flat_target[i] * (estimate[i * 4 + guide2] - plane2[i * 4 + guide2]);
        const float hf_damped
            = CLAMP(guide_score[i * 4 + c], 0.f, 1.f) * (estimate[i * 4 + c] - plane2[i * 4 + c]);
        const float energy_g = valid_variance[i * 4 + 0]; // e_g = blurred |h_g|
        const float energy_d = valid_variance[i * 4 + 1]; // e_d = blurred |h_d|
        // quadratic min-energy odds w = e_d^2/(e_d^2 + e_g^2): favours the LOWER-energy candidate,
        // so a guide misfire (spiked e_g) drives w -> 0 and the damped path wins there
        const float energy_weight = energy_d * energy_d / fmaxf(energy_d * energy_d + energy_g * energy_g, 1e-18f);
        // resynthesis: u_c = ubar_c + w*h_g + (1-w)*h_d
        estimate[i * 4 + c] = plane2[i * 4 + c] + energy_weight * hf_guided + (1.f - energy_weight) * hf_damped;
      }
  }

#undef HF_M2

  // pixels with a single surviving guide keep the damped treatment
  HL_PFOR()
  for(size_t i = 0; i < region_pixels; i++)
  {
    const int n_valid = (valid[i * 4 + 0] >= 0.5f) + (valid[i * 4 + 1] >= 0.5f) + (valid[i * 4 + 2] >= 0.5f);
    if(n_valid != 1) continue;
    for(int c = 0; c < 3; c++)
      if(valid[i * 4 + c] < 0.5f)
      {
        // no second guide -> no h_g: keep only the R^2-damped own detail  u_c = ubar_c + R^2(u_c-ubar_c)
        const float hf_weight = CLAMP(guide_score[i * 4 + c], 0.f, 1.f);
        estimate[i * 4 + c] = plane2[i * 4 + c] + hf_weight * (estimate[i * 4 + c] - plane2[i * 4 + c]);
      }
  }

  // Step 5 / Soft saturation floor (article §"The algorithm" step 5): a clipped channel is
  // physically at least its saturated reading c0, but the hard max(e, c0) prints the
  // floor-binding contour as an edge wherever a weak prediction oscillates around saturation;
  // round the transition over ~2% of c0 instead. out = 1/2 (e + c0 + sqrt((e-c0)^2 + (0.02 c0)^2)),
  // a smooth max: -> e for e >> c0, -> c0 for e << c0, softened over a width 0.02*c0.
  HL_PFOR()
  for(size_t i = 0; i < region_pixels; i++)
    for(int c = 0; c < 3; c++)
      if(valid[i * 4 + c] < 0.5f)
      {
        const float clip_floor_c = clip0[i * 4 + c];             // c0, the saturated reading
        const float delta = estimate[i * 4 + c] - clip_floor_c;  // e - c0
        const float weight = 0.02f * fmaxf(clip_floor_c, 1e-6f); // transition width = 2% of c0
        // c0 + 1/2 ( (e-c0) + sqrt((e-c0)^2 + width^2) ): the rounded lower bound at c0
        estimate[i * 4 + c] = clip_floor_c + 0.5f * (delta + sqrtf(delta * delta + weight * weight));
      }

  // Step 6 dome gate (article §"The algorithm" step 6): hand the dome-blend weight to the
  // self-dome block (it reads varc as Wc, uses We = Wc^2 as the keep weight). The two factors
  // answer two questions:  dome fraction = (1 - S_{0.4}^{0.85}(R^2)) * exp(-(delta/1.5 sigma)^2)
  //   R^2 (guide_score) -> "is the colour-line real here" via a smoothstep S (0 below 0.4,
  //     1 above 0.85): low R^2 = DOUBTFUL model -> lean on the dome.
  //   delta (clip_depth) -> "is the dome trustworthy here" via a gaussian of depth/(1.5 sigma):
  //     biharmonic extrapolation is excellent near the rim and degrades with distance, so the
  //     hand-over decays over ~1.5 sigma of depth. Deep interiors always stay on the fit.
  // We store Wc = sqrt(keep) with keep = 1 - dome_fraction = 1 - (1 - S(R^2)) * gdep, so the
  // self-dome block's conf_weight = Wc^2 = keep is exactly the coefficient-field share.
  HL_PFOR()
  for(size_t i = 0; i < region_pixels; i++)
    for(int c = 0; c < 3; c++)
    {
      const float dome_t = CLAMP((guide_score[i * 4 + c] - 0.4f) / 0.45f, 0.f, 1.f);  // ramp arg (0.4..0.85)
      const float we_r2 = dome_t * dome_t * (3.f - 2.f * dome_t);                     // S_{0.4}^{0.85}(R^2)
      const float smooth_t = clip_depth[i] / (1.5f * cf_sigma);                       // delta / (1.5 sigma)
      const float gdep = expf(-smooth_t * smooth_t);                                  // exp(-(delta/1.5 sigma)^2)
      valid_variance[i * 4 + c] = sqrtf(CLAMP(1.f - (1.f - we_r2) * gdep, 0.f, 1.f)); // Wc = sqrt(keep)
    }

  dt_pixelpipe_cache_free_align(hole2);
  dt_pixelpipe_cache_free_align(steer);
}

// ============================ OpenCL ============================

#ifdef HAVE_OPENCL
// GPU counterpart of _cf_harmonic_fill_n: harmonic fill (repeatedly replace each hole pixel by
// the average of its four neighbours -- Jacobi iterations -- run coarse-to-fine on shrunken
// copies of the grid) of up to 3 planes SHARING ONE anchor mask, executed entirely on device
// buffers. The mask pyramid, the tensor and the edge weights depend only on (mask, steer,
// geometry), so the planes share one build and the fused Jacobi kernels advance all of them
// per launch, reading the weights once per cell.
// vals[p] (float, rw*rh) hold the planes to fill; despite its name, `hole` (uchar, rw*rh) must
// be the ANCHOR mask (1 = trusted pixel to keep, 0 = hole to fill) -- see the caller-contract
// note below. Fills the hole cells of every vals[p] in place. Mirrors _cf_harmonic_fill_n on
// the CPU: any change here must be mirrored there and re-validated with the HL_FILLCL_TEST
// self-test (_cf_harmonic_fill_cl_selftest).
//
// MATHS BRIDGE -- article "The algorithm" step 3, the E_transport solver on the GPU: the anchored,
// coarse-to-fine anisotropic transport that minimizes E_transport = Sum_p int grad(p)^T D grad(p),
// p|anchors = p_fit, by relaxing div(D grad p)=0 (steer != NULL builds D via the hl_cfa_* kernels;
// steer == NULL => D = I, the plain harmonic fill). Structure mirrors the CPU _cf_harmonic_fill_n:
// base grid at pitch ~sigma/4, pyramid halved until the long side <= 8 cells (convergence from depth,
// not sweep count), flat anchor-mean seed at the coarsest level, bilinear seed of each finer level,
// 100 Jacobi sweeps per level, then bilinear upsample into the full-res hole pixels.
static cl_int _cf_harmonic_fill_cl_n(const int devid, void *gd_void, cl_mem *vals, const int n_planes_in,
                                     cl_mem hole, const int region_w, const int region_h, const int base_ds,
                                     const int mask_is_hole, cl_mem steer)
{
  dt_iop_highlights_global_data_t *global_data = (dt_iop_highlights_global_data_t *)gd_void;
  const int n_planes = CLAMP(n_planes_in, 1, DT_HL_FILL_CL_MAXP);
  const int downsample = CLAMP(base_ds, 1, 8);
  const int base_w = (region_w + downsample - 1) / downsample;
  const int base_h = (region_h + downsample - 1) / downsample;
  const size_t cell_count = (size_t)base_w * base_h;
  cl_int cl_err = DT_OPENCL_DEFAULT_ERROR;
  const int steered = (steer != NULL);
  const float steer_k = DT_HL_CF_K;

  cl_mem base_vals[DT_HL_FILL_CL_MAXP] = { NULL };
  cl_mem level_vals[DT_HL_FILL_CL_MAXP] = { NULL };
  cl_mem level_solution[DT_HL_FILL_CL_MAXP] = { NULL };
  cl_mem level_scratch[DT_HL_FILL_CL_MAXP] = { NULL };
  cl_mem prev_level_solution[DT_HL_FILL_CL_MAXP] = { NULL };
  int alloc_ok = 1;
  for(int plane = 0; plane < n_planes; plane++)
  {
    base_vals[plane] = dt_opencl_alloc_device_buffer(devid, sizeof(float) * cell_count);
    level_vals[plane] = dt_opencl_alloc_device_buffer(devid, sizeof(float) * cell_count);
    level_solution[plane] = dt_opencl_alloc_device_buffer(devid, sizeof(float) * cell_count);
    level_scratch[plane] = dt_opencl_alloc_device_buffer(devid, sizeof(float) * cell_count);
    prev_level_solution[plane] = dt_opencl_alloc_device_buffer(devid, sizeof(float) * cell_count);
    alloc_ok &= (base_vals[plane] && level_vals[plane] && level_solution[plane] && level_scratch[plane]
                 && prev_level_solution[plane]);
  }
  cl_mem base_anchor_mask = dt_opencl_alloc_device_buffer(devid, cell_count);
  cl_mem level_anchor_mask = dt_opencl_alloc_device_buffer(devid, cell_count);
  // aniso steering planes, only needed when a steering plane was passed in (all sized to the
  // base grid). Allocate them in one guarded block and fold their null checks into alloc_ok,
  // so the abort decision is taken exactly once below.
  cl_mem base_steer = NULL;
  cl_mem level_steer = NULL;
  cl_mem steer_blur_lin = NULL;
  cl_mem steer_blur_quad = NULL;
  cl_mem steer_grad_x = NULL;
  cl_mem steer_grad_y = NULL;
  cl_mem steer_tensor_xx = NULL;
  cl_mem steer_tensor_xy = NULL;
  cl_mem steer_tensor_yy = NULL;
  cl_mem grad_partial_sums = NULL;
  cl_mem grad_mean_norm = NULL;
  cl_mem neighbour_weights = NULL;
  cl_mem neighbour_weights_sum = NULL;
  if(steered)
  {
    base_steer = dt_opencl_alloc_device_buffer(devid, sizeof(float) * cell_count);
    level_steer = dt_opencl_alloc_device_buffer(devid, sizeof(float) * cell_count);
    steer_blur_lin = dt_opencl_alloc_device_buffer(devid, sizeof(float) * cell_count);
    steer_blur_quad = dt_opencl_alloc_device_buffer(devid, sizeof(float) * cell_count);
    steer_grad_x = dt_opencl_alloc_device_buffer(devid, sizeof(float) * cell_count);
    steer_grad_y = dt_opencl_alloc_device_buffer(devid, sizeof(float) * cell_count);
    steer_tensor_xx = dt_opencl_alloc_device_buffer(devid, sizeof(float) * cell_count);
    steer_tensor_xy = dt_opencl_alloc_device_buffer(devid, sizeof(float) * cell_count);
    steer_tensor_yy = dt_opencl_alloc_device_buffer(devid, sizeof(float) * cell_count);
    grad_partial_sums = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 256);
    grad_mean_norm = dt_opencl_alloc_device_buffer(devid, sizeof(float));
    neighbour_weights = dt_opencl_alloc_device_buffer(devid, sizeof(float) * cell_count * 8);
    neighbour_weights_sum = dt_opencl_alloc_device_buffer(devid, sizeof(float) * cell_count);
    alloc_ok &= (base_steer && level_steer && steer_blur_lin && steer_blur_quad && steer_grad_x && steer_grad_y
                 && steer_tensor_xx && steer_tensor_xy && steer_tensor_yy && grad_partial_sums && grad_mean_norm
                 && neighbour_weights && neighbour_weights_sum)
                    ? 1
                    : 0;
  }
  if(!alloc_ok || !base_anchor_mask || !level_anchor_mask) goto out;

  // base grid from full resolution, per plane (base_anchor_mask is identical every time). The caller's mask
  // may be in either convention (1 = trusted anchor, or 1 = hole with mask_is_hole set);
  // hl_fill_down normalizes iter, and every internal level mask below is in the ANCHOR
  // convention regardless.
  for(int plane = 0; plane < n_planes; plane++)
  {
    const int kernel = global_data->kernel_hl_fill_down;
    size_t size[3] = { ROUNDUPDWD(base_w, devid), ROUNDUPDHT(base_h, devid), 1 };
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &vals[plane]);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &hole);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &base_vals[plane]);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &base_anchor_mask);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &base_w);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &base_h);
    dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &downsample);
    dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(int), &mask_is_hole);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
    if(cl_err != CL_SUCCESS) goto out;
  }

  // aniso: steering plane on the base grid (plain block mean)
  if(steered)
  {
    const int kernel = global_data->kernel_hl_cfa_down;
    size_t size_level[3] = { ROUNDUPDWD(base_w, devid), ROUNDUPDHT(base_h, devid), 1 };
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &steer);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &base_steer);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &base_w);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &base_h);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &downsample);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size_level);
    if(cl_err != CL_SUCCESS) goto out;
  }

  // pyramid depth: halve until the LONG side is <= 8 cells (same rationale as the CPU fill:
  // the coarsest flat seed must be trivially relaxable within the fixed sweep budget)
  int nlev = 1;
  while((MAX(base_w, base_h) >> nlev) > 8 && nlev < 12) nlev++;

  // coarse-to-fine sweep: solve on the coarsest grid first, then use each solved level to seed
  // the next finer one (prev_level_w/prev_level_h remember the previous level's dimensions for the seed upsample)
  int prev_level_w = 0;
  int prev_level_h = 0;
  for(int level = nlev - 1; level >= 0; level--)
  {
    const int step = 1 << level;
    const int level_w = (base_w + step - 1) / step;
    const int level_h = (base_h + step - 1) / step;
    size_t size[3] = { ROUNDUPDWD(level_w, devid), ROUNDUPDHT(level_h, devid), 1 };

    // level grid from the base grid, per plane (level_anchor_mask identical every time)
    for(int plane = 0; plane < n_planes; plane++)
    {
      const int kernel_down = global_data->kernel_hl_fill_down;
      dt_opencl_set_kernel_arg(devid, kernel_down, 0, sizeof(cl_mem), &base_vals[plane]);
      dt_opencl_set_kernel_arg(devid, kernel_down, 1, sizeof(cl_mem), &base_anchor_mask);
      dt_opencl_set_kernel_arg(devid, kernel_down, 2, sizeof(cl_mem), &level_vals[plane]);
      dt_opencl_set_kernel_arg(devid, kernel_down, 3, sizeof(cl_mem), &level_anchor_mask);
      dt_opencl_set_kernel_arg(devid, kernel_down, 4, sizeof(int), &base_w);
      dt_opencl_set_kernel_arg(devid, kernel_down, 5, sizeof(int), &base_h);
      dt_opencl_set_kernel_arg(devid, kernel_down, 6, sizeof(int), &level_w);
      dt_opencl_set_kernel_arg(devid, kernel_down, 7, sizeof(int), &level_h);
      dt_opencl_set_kernel_arg(devid, kernel_down, 8, sizeof(int), &step);
      const int level_anchor = 0; // internal level masks are always in the anchor convention
      dt_opencl_set_kernel_arg(devid, kernel_down, 9, sizeof(int), &level_anchor);
      cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel_down, size);
      if(cl_err != CL_SUCCESS) goto out;
    }

    if(level == nlev - 1)
    {
      // coarsest level: seed every hole cell with the mean of the anchor cells (single workgroup)
      for(int plane = 0; plane < n_planes; plane++)
      {
        const int kernel_seed = global_data->kernel_hl_fill_seed;
        const int n_cells = level_w * level_h;
        const int local_size = 256;
        size_t size_level[3] = { local_size, 1, 1 };
        size_t local[3] = { local_size, 1, 1 };
        dt_opencl_set_kernel_arg(devid, kernel_seed, 0, sizeof(cl_mem), &level_solution[plane]);
        dt_opencl_set_kernel_arg(devid, kernel_seed, 1, sizeof(cl_mem), &level_vals[plane]);
        dt_opencl_set_kernel_arg(devid, kernel_seed, 2, sizeof(cl_mem), &level_anchor_mask);
        dt_opencl_set_kernel_arg(devid, kernel_seed, 3, sizeof(int), &n_cells);
        dt_opencl_set_kernel_arg(devid, kernel_seed, 4, sizeof(float) * local_size, NULL);
        dt_opencl_set_kernel_arg(devid, kernel_seed, 5, sizeof(int) * local_size, NULL);
        cl_err = dt_opencl_enqueue_kernel_2d_with_local(devid, kernel_seed, size_level, local);
        if(cl_err != CL_SUCCESS) goto out;
      }
    }
    else
    {
      // finer levels: seed hole cells by upsampling the previous (coarser) level's solution
      for(int plane = 0; plane < n_planes; plane++)
      {
        const int kernel_seed_up = global_data->kernel_hl_fill_seed_up;
        dt_opencl_set_kernel_arg(devid, kernel_seed_up, 0, sizeof(cl_mem), &level_solution[plane]);
        dt_opencl_set_kernel_arg(devid, kernel_seed_up, 1, sizeof(cl_mem), &level_vals[plane]);
        dt_opencl_set_kernel_arg(devid, kernel_seed_up, 2, sizeof(cl_mem), &level_anchor_mask);
        dt_opencl_set_kernel_arg(devid, kernel_seed_up, 3, sizeof(cl_mem), &prev_level_solution[plane]);
        dt_opencl_set_kernel_arg(devid, kernel_seed_up, 4, sizeof(int), &level_w);
        dt_opencl_set_kernel_arg(devid, kernel_seed_up, 5, sizeof(int), &level_h);
        dt_opencl_set_kernel_arg(devid, kernel_seed_up, 6, sizeof(int), &prev_level_w);
        dt_opencl_set_kernel_arg(devid, kernel_seed_up, 7, sizeof(int), &prev_level_h);
        cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel_seed_up, size);
        if(cl_err != CL_SUCCESS) goto out;
      }
    }

    // aniso: build the E_transport steering tensor D at this level (article step 3 D equation):
    // level steering plane -> blurred L/L^2 -> gradients (+ mean-magnitude reduction)
    // -> Weickert tensor (hl_cfa_tensor = _cf_adaptive_tensor) -> precomputed edge weights.
    // Mirrors the CPU per-level build exactly;
    // the gnorm reduction is finished on device so the queue never drains mid-fill. Shared by
    // all n_planes planes -- fusing them is what amortizes this whole chain.
    if(steered)
    {
      const int n_cells = level_w * level_h;
      {
        const int kernel = global_data->kernel_hl_cfa_down;
        dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &base_steer);
        dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &level_steer);
        dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &base_w);
        dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &base_h);
        dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &level_w);
        dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &level_h);
        dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &step);
        cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
        if(cl_err != CL_SUCCESS) goto out;
      }
      for(int pass = 0; pass < 2; pass++)
      {
        const int kernel = global_data->kernel_hl_cfa_box;
        cl_mem blur_in_lin = pass ? steer_grad_x : level_steer;
        cl_mem blur_in_quad = pass ? steer_grad_y : level_steer;
        cl_mem outL = pass ? steer_blur_lin : steer_grad_x;
        cl_mem outQ = pass ? steer_blur_quad : steer_grad_y;
        const int square = (pass == 0);
        dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &blur_in_lin);
        dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &blur_in_quad);
        dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &outL);
        dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &outQ);
        dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &level_w);
        dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &level_h);
        dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &square);
        cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
        if(cl_err != CL_SUCCESS) goto out;
      }
      {
        const int kernel = global_data->kernel_hl_cfa_grad;
        const int local_size = 64, n_groups = 256;
        size_t size_1d[3] = { (size_t)n_groups * local_size, 1, 1 };
        size_t local_size_1d[3] = { local_size, 1, 1 };
        dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &steer_blur_lin);
        dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &steer_grad_x);
        dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &steer_grad_y);
        dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &grad_partial_sums);
        dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &level_w);
        dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &level_h);
        dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(float) * local_size, NULL);
        cl_err = dt_opencl_enqueue_kernel_2d_with_local(devid, kernel, size_1d, local_size_1d);
        if(cl_err != CL_SUCCESS) goto out;
      }
      {
        // finish the reduction on device (single work-item): no blocking readback
        const int kernel = global_data->kernel_hl_cfa_gnorm;
        const int ngroups = 256;
        size_t size_1d[3] = { 1, 1, 1 };
        dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &grad_partial_sums);
        dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &grad_mean_norm);
        dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &ngroups);
        dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &n_cells);
        cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size_1d);
        if(cl_err != CL_SUCCESS) goto out;
      }
      {
        const int kernel = global_data->kernel_hl_cfa_tensor;
        size_t size_1d[3] = { ROUNDUPDWD(n_cells, devid), 1, 1 };
        dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &steer_grad_x);
        dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &steer_grad_y);
        dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &steer_blur_lin);
        dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &steer_blur_quad);
        dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &steer_tensor_xx);
        dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(cl_mem), &steer_tensor_xy);
        dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(cl_mem), &steer_tensor_yy);
        dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(cl_mem), &grad_mean_norm);
        dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(float), &steer_k);
        dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(int), &n_cells);
        cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size_1d);
        if(cl_err != CL_SUCCESS) goto out;
      }
      // edge weights are constant across the level's sweeps: precompute once -- but only for
      // the small grids the block kernel serves (the large-grid launch loop reads the tensor
      // planes directly, see above)
      if(level_w * level_h <= 4096)
      {
        const int kernel = global_data->kernel_hl_cfa_weights;
        dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &steer_tensor_xx);
        dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &steer_tensor_xy);
        dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &steer_tensor_yy);
        dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &neighbour_weights);
        dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &neighbour_weights_sum);
        dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &level_w);
        dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &level_h);
        cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
        if(cl_err != CL_SUCCESS) goto out;
      }
    }

    // small grids: all 100 iterations inside ONE single-workgroup launch (bit-identical);
    // the 100-launch ping-pong was the region loop's dominant enqueue cost on small regions
    if(level_w * level_h <= 4096)
    {
      const int iters = 100; // flat budget; convergence comes from the pyramid depth
      size_t size_box[3] = { 256, 1, 1 };
      size_t local_box[3] = { 256, 1, 1 };
      if(steered)
      {
        // fused: all n_planes planes advance inside the single launch (dummy slots read plane 0)
        const int kernel_block = global_data->kernel_hl_cfa_jacobi_block;
        cl_mem solution1 = (n_planes > 1) ? level_solution[1] : level_solution[0];
        cl_mem solution2 = (n_planes > 2) ? level_solution[2] : level_solution[0];
        cl_mem scratch1 = (n_planes > 1) ? level_scratch[1] : level_scratch[0];
        cl_mem scratch2 = (n_planes > 2) ? level_scratch[2] : level_scratch[0];
        int arg_index = 0;
        dt_opencl_set_kernel_arg(devid, kernel_block, arg_index++, sizeof(cl_mem), &level_solution[0]);
        dt_opencl_set_kernel_arg(devid, kernel_block, arg_index++, sizeof(cl_mem), &solution1);
        dt_opencl_set_kernel_arg(devid, kernel_block, arg_index++, sizeof(cl_mem), &solution2);
        dt_opencl_set_kernel_arg(devid, kernel_block, arg_index++, sizeof(cl_mem), &level_scratch[0]);
        dt_opencl_set_kernel_arg(devid, kernel_block, arg_index++, sizeof(cl_mem), &scratch1);
        dt_opencl_set_kernel_arg(devid, kernel_block, arg_index++, sizeof(cl_mem), &scratch2);
        dt_opencl_set_kernel_arg(devid, kernel_block, arg_index++, sizeof(cl_mem), &level_anchor_mask);
        dt_opencl_set_kernel_arg(devid, kernel_block, arg_index++, sizeof(cl_mem), &neighbour_weights);
        dt_opencl_set_kernel_arg(devid, kernel_block, arg_index++, sizeof(cl_mem), &neighbour_weights_sum);
        dt_opencl_set_kernel_arg(devid, kernel_block, arg_index++, sizeof(int), &level_w);
        dt_opencl_set_kernel_arg(devid, kernel_block, arg_index++, sizeof(int), &level_h);
        dt_opencl_set_kernel_arg(devid, kernel_block, arg_index++, sizeof(int), &iters);
        dt_opencl_set_kernel_arg(devid, kernel_block, arg_index++, sizeof(int), &n_planes);
        cl_err = dt_opencl_enqueue_kernel_2d_with_local(devid, kernel_block, size_box, local_box);
        if(cl_err != CL_SUCCESS) goto out;
      }
      else
        for(int plane = 0; plane < n_planes; plane++)
        {
          const int kernel_block = global_data->kernel_hl_fill_jacobi_block;
          int arg_index = 0;
          dt_opencl_set_kernel_arg(devid, kernel_block, arg_index++, sizeof(cl_mem), &level_solution[plane]);
          dt_opencl_set_kernel_arg(devid, kernel_block, arg_index++, sizeof(cl_mem), &level_scratch[plane]);
          dt_opencl_set_kernel_arg(devid, kernel_block, arg_index++, sizeof(cl_mem), &level_anchor_mask);
          dt_opencl_set_kernel_arg(devid, kernel_block, arg_index++, sizeof(int), &level_w);
          dt_opencl_set_kernel_arg(devid, kernel_block, arg_index++, sizeof(int), &level_h);
          dt_opencl_set_kernel_arg(devid, kernel_block, arg_index++, sizeof(int), &iters);
          cl_err = dt_opencl_enqueue_kernel_2d_with_local(devid, kernel_block, size_box, local_box);
          if(cl_err != CL_SUCCESS) goto out;
        }
      // 100 (even) internal swaps leave the solution in u, exactly like the launch loop:
      // rotate iter into prev for the next finer level's seed
      for(int plane = 0; plane < n_planes; plane++)
      {
        cl_mem swap_buf = prev_level_solution[plane];
        prev_level_solution[plane] = level_solution[plane];
        level_solution[plane] = swap_buf;
      }
      prev_level_w = level_w;
      prev_level_h = level_h;
      continue;
    }

    // larger grids: Jacobi sweeps as separate launches, ping-ponging between the two buffers
    const int n_iter = 100; // flat budget; convergence comes from the pyramid depth
    cl_mem solution_planes[DT_HL_FILL_CL_MAXP], scratch_planes[DT_HL_FILL_CL_MAXP];
    for(int plane = 0; plane < n_planes; plane++)
    {
      solution_planes[plane] = level_solution[plane];
      scratch_planes[plane] = level_scratch[plane];
    }
    for(int iter = 0; iter < n_iter; iter++)
    {
      if(steered)
      {
        // fused sweep: one launch advances all n_planes planes (dummy slots read plane 0).
        // large grids keep the tensor form (see the kernel comment: cache reuse beats
        // precomputed weights there); only the small-grid block kernel uses neighbour_weights/neighbour_weights_sum
        const int kernel_jacobi = global_data->kernel_hl_cfa_jacobi;
        cl_mem solution1 = (n_planes > 1) ? solution_planes[1] : solution_planes[0];
        cl_mem solution2 = (n_planes > 2) ? solution_planes[2] : solution_planes[0];
        cl_mem scratch1 = (n_planes > 1) ? scratch_planes[1] : scratch_planes[0];
        cl_mem scratch2 = (n_planes > 2) ? scratch_planes[2] : scratch_planes[0];
        int arg_index = 0;
        dt_opencl_set_kernel_arg(devid, kernel_jacobi, arg_index++, sizeof(cl_mem), &solution_planes[0]);
        dt_opencl_set_kernel_arg(devid, kernel_jacobi, arg_index++, sizeof(cl_mem), &solution1);
        dt_opencl_set_kernel_arg(devid, kernel_jacobi, arg_index++, sizeof(cl_mem), &solution2);
        dt_opencl_set_kernel_arg(devid, kernel_jacobi, arg_index++, sizeof(cl_mem), &scratch_planes[0]);
        dt_opencl_set_kernel_arg(devid, kernel_jacobi, arg_index++, sizeof(cl_mem), &scratch1);
        dt_opencl_set_kernel_arg(devid, kernel_jacobi, arg_index++, sizeof(cl_mem), &scratch2);
        dt_opencl_set_kernel_arg(devid, kernel_jacobi, arg_index++, sizeof(cl_mem), &level_anchor_mask);
        dt_opencl_set_kernel_arg(devid, kernel_jacobi, arg_index++, sizeof(cl_mem), &steer_tensor_xx);
        dt_opencl_set_kernel_arg(devid, kernel_jacobi, arg_index++, sizeof(cl_mem), &steer_tensor_xy);
        dt_opencl_set_kernel_arg(devid, kernel_jacobi, arg_index++, sizeof(cl_mem), &steer_tensor_yy);
        dt_opencl_set_kernel_arg(devid, kernel_jacobi, arg_index++, sizeof(int), &level_w);
        dt_opencl_set_kernel_arg(devid, kernel_jacobi, arg_index++, sizeof(int), &level_h);
        dt_opencl_set_kernel_arg(devid, kernel_jacobi, arg_index++, sizeof(int), &n_planes);
        cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel_jacobi, size);
        if(cl_err != CL_SUCCESS) goto out;
      }
      else
        for(int plane = 0; plane < n_planes; plane++)
        {
          const int kernel_jacobi = global_data->kernel_hl_fill_jacobi;
          int arg_index = 0;
          dt_opencl_set_kernel_arg(devid, kernel_jacobi, arg_index++, sizeof(cl_mem), &solution_planes[plane]);
          dt_opencl_set_kernel_arg(devid, kernel_jacobi, arg_index++, sizeof(cl_mem), &scratch_planes[plane]);
          dt_opencl_set_kernel_arg(devid, kernel_jacobi, arg_index++, sizeof(cl_mem), &level_anchor_mask);
          dt_opencl_set_kernel_arg(devid, kernel_jacobi, arg_index++, sizeof(int), &level_w);
          dt_opencl_set_kernel_arg(devid, kernel_jacobi, arg_index++, sizeof(int), &level_h);
          cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel_jacobi, size);
          if(cl_err != CL_SUCCESS) goto out;
        }
      for(int plane = 0; plane < n_planes; plane++)
      {
        cl_mem swap_buf = solution_planes[plane];
        solution_planes[plane] = scratch_planes[plane];
        scratch_planes[plane] = swap_buf;
      }
    }
    // solution of this level in `a`: stash into prev for the next finer seed, keeping u/v the
    // two scratches distinct from prev
    for(int plane = 0; plane < n_planes; plane++)
    {
      cl_mem swap_buf = prev_level_solution[plane];
      prev_level_solution[plane] = solution_planes[plane];
      solution_planes[plane] = swap_buf;
      level_solution[plane]
          = (prev_level_solution[plane] == level_solution[plane]) ? solution_planes[plane] : level_solution[plane];
      level_scratch[plane]
          = (prev_level_solution[plane] == level_scratch[plane]) ? solution_planes[plane] : level_scratch[plane];
    }
    prev_level_w = level_w;
    prev_level_h = level_h;
  }

  // upsample prev (base-grid solution) into the full-res holes: kernel expects HOLE mask; our
  // `hole` buffer holds ANCHORS (caller contract), so hl_fill_up's test is inverted there.
  for(int plane = 0; plane < n_planes; plane++)
  {
    const int kernel_seed_up = global_data->kernel_hl_fill_up;
    size_t size_upsample[3] = { ROUNDUPDWD(region_w, devid), ROUNDUPDHT(region_h, devid), 1 };
    dt_opencl_set_kernel_arg(devid, kernel_seed_up, 0, sizeof(cl_mem), &vals[plane]);
    dt_opencl_set_kernel_arg(devid, kernel_seed_up, 1, sizeof(cl_mem), &hole);
    dt_opencl_set_kernel_arg(devid, kernel_seed_up, 2, sizeof(cl_mem), &prev_level_solution[plane]);
    dt_opencl_set_kernel_arg(devid, kernel_seed_up, 3, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel_seed_up, 4, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel_seed_up, 5, sizeof(int), &base_w);
    dt_opencl_set_kernel_arg(devid, kernel_seed_up, 6, sizeof(int), &base_h);
    dt_opencl_set_kernel_arg(devid, kernel_seed_up, 7, sizeof(int), &downsample);
    dt_opencl_set_kernel_arg(devid, kernel_seed_up, 8, sizeof(int), &mask_is_hole);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel_seed_up, size_upsample);
    if(cl_err != CL_SUCCESS) goto out;
  }

out:
  for(int plane = 0; plane < DT_HL_FILL_CL_MAXP; plane++)
  {
    dt_opencl_release_mem_object(base_vals[plane]);
    dt_opencl_release_mem_object(level_vals[plane]);
    dt_opencl_release_mem_object(level_solution[plane]);
    dt_opencl_release_mem_object(level_scratch[plane]);
    dt_opencl_release_mem_object(prev_level_solution[plane]);
  }
  dt_opencl_release_mem_object(base_anchor_mask);
  dt_opencl_release_mem_object(level_anchor_mask);
  dt_opencl_release_mem_object(base_steer);
  dt_opencl_release_mem_object(level_steer);
  dt_opencl_release_mem_object(steer_blur_lin);
  dt_opencl_release_mem_object(steer_blur_quad);
  dt_opencl_release_mem_object(steer_grad_x);
  dt_opencl_release_mem_object(steer_grad_y);
  dt_opencl_release_mem_object(steer_tensor_xx);
  dt_opencl_release_mem_object(steer_tensor_xy);
  dt_opencl_release_mem_object(steer_tensor_yy);
  dt_opencl_release_mem_object(grad_partial_sums);
  dt_opencl_release_mem_object(grad_mean_norm);
  dt_opencl_release_mem_object(neighbour_weights);
  dt_opencl_release_mem_object(neighbour_weights_sum);
  return cl_err;
}

cl_int _cf_harmonic_fill_cl(const int devid, void *gd_void, cl_mem val, cl_mem hole, const int region_w,
                            const int region_h, const int base_ds, const int mask_is_hole, cl_mem steer)
{
  cl_mem val_planes[1] = { val };
  return _cf_harmonic_fill_cl_n(devid, gd_void, val_planes, 1, hole, region_w, region_h, base_ds, mask_is_hole,
                                steer);
}
#endif // HAVE_OPENCL

#ifdef HAVE_OPENCL

cl_int _cf_joint_stage_cl(const int devid, void *gd_void, cl_mem estimate, cl_mem valid, cl_mem model_quality,
                          cl_mem mom0, cl_mem mom1, cl_mem mom2, cl_mem steer,
                          const float *const restrict channel_means, const int region_w, const int region_h,
                          const float cf_sigma, const float cf_fmin, const int c, const int guide1,
                          const int guide2)
{
  dt_iop_highlights_global_data_t *global_data = (dt_iop_highlights_global_data_t *)gd_void;
  const size_t region_pixels = (size_t)region_w * region_h;
  cl_int cl_err = DT_OPENCL_DEFAULT_ERROR;
  size_t work_size[3] = { ROUNDUPDWD(region_w, devid), ROUNDUPDHT(region_h, devid), 1 };

  cl_mem coeff_slope_a = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem coeff_slope_b = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem coeff_offset = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem coeff_r2 = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem anchor = dt_opencl_alloc_device_buffer(devid, region_pixels);
  cl_mem broad = dt_opencl_alloc_device_buffer(devid, region_pixels);
  if(!coeff_slope_a || !coeff_slope_b || !coeff_offset || !coeff_r2 || !anchor || !broad) goto out;

  // per-pixel colour-line fit from the blurred moments; also writes the anchor masks
  {
    const int kernel = global_data->kernel_hl_cf_fit_joint;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &mom0);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &mom1);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &mom2);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &c);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &guide1);
    dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &guide2);
    dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(float), &cf_fmin);
    dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(cl_mem), &coeff_slope_a);
    dt_opencl_set_kernel_arg(devid, kernel, 11, sizeof(cl_mem), &coeff_slope_b);
    dt_opencl_set_kernel_arg(devid, kernel, 12, sizeof(cl_mem), &coeff_offset);
    dt_opencl_set_kernel_arg(devid, kernel, 13, sizeof(cl_mem), &coeff_r2);
    dt_opencl_set_kernel_arg(devid, kernel, 14, sizeof(cl_mem), &anchor);
    dt_opencl_set_kernel_arg(devid, kernel, 15, sizeof(cl_mem), &broad);
    dt_opencl_set_kernel_arg(devid, kernel, 16, sizeof(float), &channel_means[0]);
    dt_opencl_set_kernel_arg(devid, kernel, 17, sizeof(float), &channel_means[1]);
    dt_opencl_set_kernel_arg(devid, kernel, 18, sizeof(float), &channel_means[2]);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, work_size);
    if(cl_err != CL_SUCCESS) goto out;
  }

  // harmonic diffusion of the coefficient fields across the clipped zone (fit quality cr2 uses
  // the broader anchor mask)
  {
    const int base_ds = (int)(cf_sigma / 4.f);
    cl_mem coeff_planes[3]
        = { coeff_slope_a, coeff_slope_b, coeff_offset }; // shared anchor mask -> one fused fill
    cl_err
        = _cf_harmonic_fill_cl_n(devid, gd_void, coeff_planes, 3, anchor, region_w, region_h, base_ds, 0, steer);
    if(cl_err != CL_SUCCESS) goto out;
    cl_err = _cf_harmonic_fill_cl(devid, gd_void, coeff_r2, broad, region_w, region_h, base_ds, 0, steer);
    if(cl_err != CL_SUCCESS) goto out;
  }

  // evaluate the diffused colour line against the measured guides; write est + fit score bsc
  {
    const int kernel = global_data->kernel_hl_cf_eval_joint;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &coeff_slope_a);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &coeff_slope_b);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &coeff_offset);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &coeff_r2);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(cl_mem), &model_quality);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(int), &c);
    dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(int), &guide1);
    dt_opencl_set_kernel_arg(devid, kernel, 11, sizeof(int), &guide2);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, work_size);
  }

out:
  dt_opencl_release_mem_object(coeff_slope_a);
  dt_opencl_release_mem_object(coeff_slope_b);
  dt_opencl_release_mem_object(coeff_offset);
  dt_opencl_release_mem_object(coeff_r2);
  dt_opencl_release_mem_object(anchor);
  dt_opencl_release_mem_object(broad);
  return cl_err;
}

// Pair stage for one orientation: predict one clipped channel from a SINGLE guide channel
// (slope + intercept fitted from the windowed moments), diffuse the fitted coefficients across
// the clipped zone, then evaluate against the measured guide and write into est.
// `a`/`b` name the channel pair and `o` picks the orientation (which of the two is the target
// tc and which is the guide gc); oc is the remaining third channel.
// Runs unconditionally (no target-count guard: an empty target set writes nothing).
// Mirrors the pair coefficient-field stage inside _region_guided_filter (CPU): any change here
// must be mirrored there and re-validated with the HL_CFCL_TEST self-tests.
//
// MATHS BRIDGE -- article "The algorithm" step 3, the single-guide fallback: the model collapses to
// v_hat = a*u + d with a = Cov(u,v)/Var(u) and R^2 = Cov(u,v)^2/(Var(u) Var(v)) (hl_cf_fit_pair),
// transported by the E_transport fill and evaluated by hl_cf_eval_pair. Same fit/transport/evaluate
// skeleton as the joint stage, one guide instead of two.
static cl_int _cf_pair_stage_cl(const int devid, void *gd_void, cl_mem estimate, cl_mem valid,
                                cl_mem model_quality, cl_mem moment_a, cl_mem moment_b, cl_mem steer,
                                const float *const restrict channel_means, const int region_w, const int region_h,
                                const float cf_sigma, const float cf_fmin, const int chan_a, const int chan_b,
                                const int orientation)
{
  dt_iop_highlights_global_data_t *global_data = (dt_iop_highlights_global_data_t *)gd_void;
  const size_t region_pixels = (size_t)region_w * region_h;
  cl_int cl_err = DT_OPENCL_DEFAULT_ERROR;
  size_t work_size[3] = { ROUNDUPDWD(region_w, devid), ROUNDUPDHT(region_h, devid), 1 };
  const int target_chan = orientation ? chan_b : chan_a;
  const int guide_chan = orientation ? chan_a : chan_b;
  const int other_chan = 3 - chan_a - chan_b;

  cl_mem coeff_slope = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem coeff_intercept = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem coeff_r2 = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem anchor = dt_opencl_alloc_device_buffer(devid, region_pixels);
  cl_mem broad = dt_opencl_alloc_device_buffer(devid, region_pixels);
  if(!coeff_slope || !coeff_intercept || !coeff_r2 || !anchor || !broad) goto out;

  // per-pixel single-guide fit (slope cs, intercept ci, fit quality cr2) from the moments
  {
    const int kernel = global_data->kernel_hl_cf_fit_pair;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &moment_a);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &moment_b);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &target_chan);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &orientation);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(float), &cf_fmin);
    dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(cl_mem), &coeff_slope);
    dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(cl_mem), &coeff_intercept);
    dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(cl_mem), &coeff_r2);
    dt_opencl_set_kernel_arg(devid, kernel, 11, sizeof(cl_mem), &anchor);
    dt_opencl_set_kernel_arg(devid, kernel, 12, sizeof(cl_mem), &broad);
    dt_opencl_set_kernel_arg(devid, kernel, 13, sizeof(float), &channel_means[target_chan]);
    dt_opencl_set_kernel_arg(devid, kernel, 14, sizeof(float), &channel_means[guide_chan]);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, work_size);
    if(cl_err != CL_SUCCESS) goto out;
  }

  // harmonic diffusion of slope/intercept/fit-quality fields across the clipped zone
  {
    const int base_ds = (int)(cf_sigma / 4.f);
    cl_mem coeff_planes[2] = { coeff_slope, coeff_intercept }; // shared anchor mask -> one fused fill
    cl_err
        = _cf_harmonic_fill_cl_n(devid, gd_void, coeff_planes, 2, anchor, region_w, region_h, base_ds, 0, steer);
    if(cl_err != CL_SUCCESS) goto out;
    cl_err = _cf_harmonic_fill_cl(devid, gd_void, coeff_r2, broad, region_w, region_h, base_ds, 0, steer);
    if(cl_err != CL_SUCCESS) goto out;
  }

  // evaluate the diffused fit against the measured guide; write est + fit score bsc
  {
    const int kernel = global_data->kernel_hl_cf_eval_pair;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &coeff_slope);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &coeff_intercept);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &coeff_r2);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(cl_mem), &model_quality);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &target_chan);
    dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(int), &guide_chan);
    dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(int), &other_chan);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, work_size);
  }

out:
  dt_opencl_release_mem_object(coeff_slope);
  dt_opencl_release_mem_object(coeff_intercept);
  dt_opencl_release_mem_object(coeff_r2);
  dt_opencl_release_mem_object(anchor);
  dt_opencl_release_mem_object(broad);
  return cl_err;
}

// Variant of the joint stage that fits and DIFFUSES the coefficient fields but defers the
// evaluation: the caller keeps the four returned buffers (slope a, slope b, offset d, fit
// quality r2) to evaluate later in the deep-channel cascade (deep-channel stash).
// Caller releases the returned cl_mem buffers. Mirrors the deferred joint fit inside
// _region_guided_filter (CPU): any change here must be mirrored there and re-validated with
// the HL_CFCL_TEST self-tests.
//
// MATHS BRIDGE -- article "The algorithm" step 3, the deep channel's ordering subtlety: fit (a,b,d)
// and transport them (E_transport) exactly as the joint stage, but DEFER v_hat = a*u1 + b*u2 + d so it
// is evaluated only after the other clipped channels are reconstructed -- then every guide it reads is
// a continuous surface (no clip-contour arc). Returns the four diffused planes (a, b, d, R^2) to stash.
static cl_int _cf_joint_fit_cl(const int devid, void *gd_void, cl_mem estimate, cl_mem valid, cl_mem mom0,
                               cl_mem mom1, cl_mem mom2, cl_mem steer, const float *const restrict channel_means,
                               const int region_w, const int region_h, const float cf_sigma, const float cf_fmin,
                               const int c, const int guide1, const int guide2, cl_mem *ca_out, cl_mem *cb_out,
                               cl_mem *cd_out, cl_mem *cr2_out)
{
  dt_iop_highlights_global_data_t *global_data = (dt_iop_highlights_global_data_t *)gd_void;
  const size_t region_pixels = (size_t)region_w * region_h;
  cl_int cl_err = DT_OPENCL_DEFAULT_ERROR;
  size_t work_size[3] = { ROUNDUPDWD(region_w, devid), ROUNDUPDHT(region_h, devid), 1 };

  cl_mem coeff_slope_a = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem coeff_slope_b = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem coeff_offset = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem coeff_r2 = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem anchor = dt_opencl_alloc_device_buffer(devid, region_pixels);
  cl_mem broad = dt_opencl_alloc_device_buffer(devid, region_pixels);
  *ca_out = *cb_out = *cd_out = *cr2_out = NULL;
  if(!coeff_slope_a || !coeff_slope_b || !coeff_offset || !coeff_r2 || !anchor || !broad) goto out;

  {
    const int kernel = global_data->kernel_hl_cf_fit_joint;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &mom0);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &mom1);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &mom2);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &c);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &guide1);
    dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &guide2);
    dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(float), &cf_fmin);
    dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(cl_mem), &coeff_slope_a);
    dt_opencl_set_kernel_arg(devid, kernel, 11, sizeof(cl_mem), &coeff_slope_b);
    dt_opencl_set_kernel_arg(devid, kernel, 12, sizeof(cl_mem), &coeff_offset);
    dt_opencl_set_kernel_arg(devid, kernel, 13, sizeof(cl_mem), &coeff_r2);
    dt_opencl_set_kernel_arg(devid, kernel, 14, sizeof(cl_mem), &anchor);
    dt_opencl_set_kernel_arg(devid, kernel, 15, sizeof(cl_mem), &broad);
    dt_opencl_set_kernel_arg(devid, kernel, 16, sizeof(float), &channel_means[0]);
    dt_opencl_set_kernel_arg(devid, kernel, 17, sizeof(float), &channel_means[1]);
    dt_opencl_set_kernel_arg(devid, kernel, 18, sizeof(float), &channel_means[2]);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, work_size);
    if(cl_err != CL_SUCCESS) goto out;
  }

  {
    const int base_ds = (int)(cf_sigma / 4.f);
    cl_mem coeff_planes[3]
        = { coeff_slope_a, coeff_slope_b, coeff_offset }; // shared anchor mask -> one fused fill
    cl_err
        = _cf_harmonic_fill_cl_n(devid, gd_void, coeff_planes, 3, anchor, region_w, region_h, base_ds, 0, steer);
    if(cl_err != CL_SUCCESS) goto out;
    cl_err = _cf_harmonic_fill_cl(devid, gd_void, coeff_r2, broad, region_w, region_h, base_ds, 0, steer);
    if(cl_err != CL_SUCCESS) goto out;
  }

  *ca_out = coeff_slope_a;
  *cb_out = coeff_slope_b;
  *cd_out = coeff_offset;
  *cr2_out = coeff_r2;
  coeff_slope_a = coeff_slope_b = coeff_offset = coeff_r2 = NULL;
  cl_err = CL_SUCCESS;

out:
  dt_opencl_release_mem_object(coeff_slope_a);
  dt_opencl_release_mem_object(coeff_slope_b);
  dt_opencl_release_mem_object(coeff_offset);
  dt_opencl_release_mem_object(coeff_r2);
  dt_opencl_release_mem_object(anchor);
  dt_opencl_release_mem_object(broad);
  return cl_err;
}

cl_int _cf_stage_cl(const int devid, void *gd_void, cl_mem estimate, cl_mem valid, cl_mem model_quality,
                    cl_mem luminance, cl_mem steer, const float *const restrict channel_means,
                    dt_gaussian_cl_t *gaussian, const int region_w, const int region_h, const float cf_sigma,
                    const float cf_fmin, const float cf_binv, const int cdeep)
{
  cl_int cl_err = CL_SUCCESS;
  cl_mem deep_a = NULL, deep_b = NULL, deep_d = NULL, deep_r2 = NULL;
  dt_iop_highlights_global_data_t *global_data = (dt_iop_highlights_global_data_t *)gd_void;
  size_t size[3] = { ROUNDUPDWD(region_w, devid), ROUNDUPDHT(region_h, devid), 1 };

  // the windowed moments are shared: the joint weight (all-valid x frozen bw_) does not depend
  // on the target channel and the evals only write CLIPPED channels, so estimate never changes at chan_a
  // weighted pixel -- packed + blur ONCE and fit the three channels from the same fields (the
  // per-channel repack the CPU does is redundant on both sides; here the blurs dominate)
  cl_mem packed = dt_opencl_alloc_device(devid, size[0], size[1], sizeof(float) * 4);
  cl_mem moment0 = dt_opencl_alloc_device(devid, size[0], size[1], sizeof(float) * 4);
  cl_mem moment1 = dt_opencl_alloc_device(devid, size[0], size[1], sizeof(float) * 4);
  cl_mem moment2 = dt_opencl_alloc_device(devid, size[0], size[1], sizeof(float) * 4);
  cl_mem moments[3];
  moments[0] = moment0;
  moments[1] = moment1;
  moments[2] = moment2;
  if(!packed || !moment0 || !moment1 || !moment2)
  {
    cl_err = DT_OPENCL_DEFAULT_ERROR;
    goto out;
  }
  // three modes = the ten centred moment planes: mode 0 -> [n, wR, wG, wB], mode 1 -> [wRR, wGG, wBB, wRG],
  // mode 2 -> [wRB, wGB, unweighted-n, 0]; each packed product image is Gaussian-blurred to a windowed moment
  for(int mode = 0; mode < 3 && cl_err == CL_SUCCESS; mode++)
  {
    const int kernel = global_data->kernel_hl_cf_pack_joint;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &luminance);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &packed);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(float), &cf_binv);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &mode);
    dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(float), &channel_means[0]);
    dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(float), &channel_means[1]);
    dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(float), &channel_means[2]);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
    if(cl_err == CL_SUCCESS)
      cl_err = gaussian ? dt_gaussian_blur_cl(gaussian, packed, moments[mode])
                        : _region_blur_cl(devid, packed, moments[mode], region_w, region_h, cf_sigma);
  }
  if(cl_err != CL_SUCCESS) goto out;

  // joint fits: immediate strict eval for the non-deep channels, stash for the deep one
  for(int c = 0; c < 3; c++)
  {
    const int guide1 = (c == 0) ? 1 : 0;
    const int guide2 = (c == 2) ? 1 : 2;
    if(c == cdeep)
    {
      cl_err = _cf_joint_fit_cl(devid, gd_void, estimate, valid, moment0, moment1, moment2, steer, channel_means,
                                region_w, region_h, cf_sigma, cf_fmin, c, guide1, guide2, &deep_a, &deep_b,
                                &deep_d, &deep_r2);
    }
    else
      cl_err = _cf_joint_stage_cl(devid, gd_void, estimate, valid, model_quality, moment0, moment1, moment2, steer,
                                  channel_means, region_w, region_h, cf_sigma, cf_fmin, c, guide1, guide2);
    if(cl_err != CL_SUCCESS) goto out;
  }

  // pair fallbacks: the pair weight (both-valid x frozen bw_) is orientation-independent and
  // estimate at weighted pixels never changes, so packed each pair's moments once for both orientations
  for(int chan_a = 0; chan_a < 3; chan_a++)
    for(int chan_b = chan_a + 1; chan_b < 3; chan_b++)
    {
      for(int mode = 0; mode < 2 && cl_err == CL_SUCCESS; mode++)
      {
        const int kernel = global_data->kernel_hl_cf_pack_pair;
        dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
        dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
        dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &luminance);
        dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &packed);
        dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_w);
        dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &region_h);
        dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(float), &cf_binv);
        dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &chan_a);
        dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &chan_b);
        dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(int), &mode);
        dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(float), &channel_means[chan_a]);
        dt_opencl_set_kernel_arg(devid, kernel, 11, sizeof(float), &channel_means[chan_b]);
        cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
        if(cl_err == CL_SUCCESS)
          cl_err = gaussian
                       ? dt_gaussian_blur_cl(gaussian, packed, mode ? moment1 : moment0)
                       : _region_blur_cl(devid, packed, mode ? moment1 : moment0, region_w, region_h, cf_sigma);
      }
      for(int orientation = 0; orientation < 2 && cl_err == CL_SUCCESS; orientation++)
        cl_err
            = _cf_pair_stage_cl(devid, gd_void, estimate, valid, model_quality, moment0, moment1, steer,
                                channel_means, region_w, region_h, cf_sigma, cf_fmin, chan_a, chan_b, orientation);
      if(cl_err != CL_SUCCESS) goto out;
    }

  // deferred deep evaluation: blur the deep-channel validity masks (feathered depth split),
  // then evaluate the stashed coefficient fields now that the guide channels are reconstructed
  if(deep_a)
  {
    const int guide1 = (cdeep == 0) ? 1 : 0;
    const int guide2 = (cdeep == 2) ? 1 : 2;
    cl_mem deep_packed = dt_opencl_alloc_device(devid, size[0], size[1], sizeof(float) * 4);
    cl_mem mask_blurred = dt_opencl_alloc_device(devid, size[0], size[1], sizeof(float) * 4);
    if(!deep_packed || !mask_blurred)
    {
      dt_opencl_release_mem_object(deep_packed);
      dt_opencl_release_mem_object(mask_blurred);
      cl_err = DT_OPENCL_DEFAULT_ERROR;
      goto out;
    }

    const int kernel_mask = global_data->kernel_hl_cf_pack_deepmask;
    dt_opencl_set_kernel_arg(devid, kernel_mask, 0, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 1, sizeof(cl_mem), &deep_packed);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 2, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 3, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 4, sizeof(int), &cdeep);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 5, sizeof(int), &guide1);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 6, sizeof(int), &guide2);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel_mask, size);
    if(cl_err == CL_SUCCESS)
      cl_err = gaussian ? dt_gaussian_blur_cl(gaussian, deep_packed, mask_blurred)
                        : _region_blur_cl(devid, deep_packed, mask_blurred, region_w, region_h, cf_sigma);

    if(cl_err == CL_SUCCESS)
    {
      const int kernel_eval = global_data->kernel_hl_cf_eval_deep;
      dt_opencl_set_kernel_arg(devid, kernel_eval, 0, sizeof(cl_mem), &deep_a);
      dt_opencl_set_kernel_arg(devid, kernel_eval, 1, sizeof(cl_mem), &deep_b);
      dt_opencl_set_kernel_arg(devid, kernel_eval, 2, sizeof(cl_mem), &deep_d);
      dt_opencl_set_kernel_arg(devid, kernel_eval, 3, sizeof(cl_mem), &deep_r2);
      dt_opencl_set_kernel_arg(devid, kernel_eval, 4, sizeof(cl_mem), &mask_blurred);
      dt_opencl_set_kernel_arg(devid, kernel_eval, 5, sizeof(cl_mem), &valid);
      dt_opencl_set_kernel_arg(devid, kernel_eval, 6, sizeof(cl_mem), &estimate);
      dt_opencl_set_kernel_arg(devid, kernel_eval, 7, sizeof(cl_mem), &model_quality);
      dt_opencl_set_kernel_arg(devid, kernel_eval, 8, sizeof(int), &region_w);
      dt_opencl_set_kernel_arg(devid, kernel_eval, 9, sizeof(int), &region_h);
      dt_opencl_set_kernel_arg(devid, kernel_eval, 10, sizeof(int), &cdeep);
      dt_opencl_set_kernel_arg(devid, kernel_eval, 11, sizeof(int), &guide1);
      dt_opencl_set_kernel_arg(devid, kernel_eval, 12, sizeof(int), &guide2);
      cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel_eval, size);
    }
    dt_opencl_release_mem_object(deep_packed);
    dt_opencl_release_mem_object(mask_blurred);
  }

out:
  dt_opencl_release_mem_object(packed);
  dt_opencl_release_mem_object(moment0);
  dt_opencl_release_mem_object(moment1);
  dt_opencl_release_mem_object(moment2);
  dt_opencl_release_mem_object(deep_a);
  dt_opencl_release_mem_object(deep_b);
  dt_opencl_release_mem_object(deep_d);
  dt_opencl_release_mem_object(deep_r2);
  return cl_err;
}

cl_int _hf_stage_cl(const int devid, void *gd_void, cl_mem estimate, cl_mem valid, cl_mem model_quality,
                    cl_mem luminance, cl_mem steer, dt_gaussian_cl_t *gaussian, const int region_w,
                    const int region_h, const float cf_sigma, const float cf_fmin, const float cf_binv)
{
  dt_iop_highlights_global_data_t *global_data = (dt_iop_highlights_global_data_t *)gd_void;
  const size_t region_pixels = (size_t)region_w * region_h;
  cl_int cl_err = DT_OPENCL_DEFAULT_ERROR;
  size_t size[3] = { ROUNDUPDWD(region_w, devid), ROUNDUPDHT(region_h, devid), 1 };
  const float blur_sigma = fmaxf(cf_sigma / 4.f, 2.f);

  cl_mem packed = dt_opencl_alloc_device(devid, size[0], size[1], sizeof(float) * 4);
  cl_mem lowpass = dt_opencl_alloc_device(devid, size[0], size[1], sizeof(float) * 4);
  cl_mem moment0 = dt_opencl_alloc_device(devid, size[0], size[1], sizeof(float) * 4);
  cl_mem moment1 = dt_opencl_alloc_device(devid, size[0], size[1], sizeof(float) * 4);
  cl_mem moment2 = dt_opencl_alloc_device(devid, size[0], size[1], sizeof(float) * 4);
  cl_mem energy = dt_opencl_alloc_device(devid, size[0], size[1], sizeof(float) * 4);
  cl_mem moments[3];
  cl_mem gain_a = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem gain_b = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem anchor = dt_opencl_alloc_device_buffer(devid, region_pixels);
  moments[0] = moment0;
  moments[1] = moment1;
  moments[2] = moment2;
  if(!packed || !lowpass || !moment0 || !moment1 || !moment2 || !energy || !gain_a || !gain_b || !anchor) goto out;

  // lowpass of estimate (computed ONCE, shared by every channel and the damped path)
  {
    const int kernel = global_data->kernel_hl_buf_to_img;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &packed);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region_h);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
    if(cl_err != CL_SUCCESS) goto out;
    cl_err = _region_blur_cl(devid, packed, lowpass, region_w, region_h, blur_sigma);
    if(cl_err != CL_SUCCESS) goto out;
  }

  // windowed moments of the detail band (estimate minus lowpass), packed then blurred
  for(int mode = 0; mode < 3; mode++)
  {
    const int kernel = global_data->kernel_hl_hf_pack;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &luminance);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &lowpass);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &packed);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(float), &cf_binv);
    dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &mode);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
    if(cl_err != CL_SUCCESS) goto out;
    cl_err = gaussian ? dt_gaussian_blur_cl(gaussian, packed, moments[mode])
                      : _region_blur_cl(devid, packed, moments[mode], region_w, region_h, cf_sigma);
    if(cl_err != CL_SUCCESS) goto out;
  }

  // per channel: fit detail-band gains, diffuse them, measure both candidates' energy, evaluate
  for(int c = 0; c < 3; c++)
  {
    const int guide1 = (c == 0) ? 1 : 0;
    const int guide2 = (c == 2) ? 1 : 2;

    // fit the quality-shrunk detail-band gains (gain_a, gain_b) and the anchor mask
    {
      const int kernel = global_data->kernel_hl_hf_fit;
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &moment0);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &moment1);
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &moment2);
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &valid);
      dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_w);
      dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &region_h);
      dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &c);
      dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &guide1);
      dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &guide2);
      dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(float), &cf_fmin);
      dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(cl_mem), &gain_a);
      dt_opencl_set_kernel_arg(devid, kernel, 11, sizeof(cl_mem), &gain_b);
      dt_opencl_set_kernel_arg(devid, kernel, 12, sizeof(cl_mem), &anchor);
      cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
      if(cl_err != CL_SUCCESS) goto out;
    }

    // harmonic diffusion of the two gain fields across the clipped zone
    const int base_ds = (int)(cf_sigma / 4.f);
    cl_mem gain_pair[2] = { gain_a, gain_b }; // shared anchor mask -> one fused fill
    cl_err = _cf_harmonic_fill_cl_n(devid, gd_void, gain_pair, 2, anchor, region_w, region_h, base_ds, 0, steer);
    if(cl_err != CL_SUCCESS) goto out;

    // local energy of the guided vs damped detail candidates (blurred absolute values)
    {
      const int kernel = global_data->kernel_hl_hf_energy;
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &model_quality);
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &lowpass);
      dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &gain_a);
      dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(cl_mem), &gain_b);
      dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(cl_mem), &packed);
      dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &region_w);
      dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &region_h);
      dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(int), &c);
      dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(int), &guide1);
      dt_opencl_set_kernel_arg(devid, kernel, 11, sizeof(int), &guide2);
      cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
      if(cl_err != CL_SUCCESS) goto out;
      cl_err = _region_blur_cl(devid, packed, energy, region_w, region_h, blur_sigma);
      if(cl_err != CL_SUCCESS) goto out;
    }

    // minimum-energy blend of the two candidates at strict (both-guides-valid) targets
    {
      const int kernel = global_data->kernel_hl_hf_eval;
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &model_quality);
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &lowpass);
      dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &energy);
      dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(cl_mem), &gain_a);
      dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(cl_mem), &gain_b);
      dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &region_w);
      dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &region_h);
      dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(int), &c);
      dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(int), &guide1);
      dt_opencl_set_kernel_arg(devid, kernel, 11, sizeof(int), &guide2);
      cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
      if(cl_err != CL_SUCCESS) goto out;
    }
  }

  // single-guide pixels: only the quality-damped detail (no guided resynthesis possible)
  {
    const int kernel = global_data->kernel_hl_hf_damp;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &model_quality);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &lowpass);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &region_h);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
  }

out:
  dt_opencl_release_mem_object(packed);
  dt_opencl_release_mem_object(lowpass);
  dt_opencl_release_mem_object(moment0);
  dt_opencl_release_mem_object(moment1);
  dt_opencl_release_mem_object(moment2);
  dt_opencl_release_mem_object(energy);
  dt_opencl_release_mem_object(gain_a);
  dt_opencl_release_mem_object(gain_b);
  dt_opencl_release_mem_object(anchor);
  return cl_err;
}

#endif // HAVE_OPENCL
