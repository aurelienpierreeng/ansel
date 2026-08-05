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

// R9 sensor-rolloff (knee) estimation and inversion (CPU + OpenCL). (implementation; see knee.h for the public
// API.)

#include "common/macros.h"
#include "common/simd.h"
#include "common/target_clones.h"
#include "develop/pixelpipe_cache_alloc.h"
#include "common/solvers/sparse_cholesky_cl.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "iop/highlights/knee.h"
#include <math.h>
#include <stdlib.h>

static inline float _knee_lift_of(const _hl_knee_curve_t *const k, const float x)
{
  const float step
      = (DT_HL_KNEE_DET - DT_HL_KNEE_LO) / (float)DT_HL_KNEE_BINS; // bin width over the band [LO, DET)
  const float bin_pos
      = (x - (DT_HL_KNEE_LO + 0.5f * step)) / step; // x in bin-center units (knot 0 sits at LO + step/2)

  if(bin_pos <= -0.5f) return 0.f; // at/below LO: no lift (identity anchor)
  if(bin_pos <= 0.f)
    return k->lift[0] * 2.f * (bin_pos + 0.5f); // first half-bin: ramp 0 -> lift[0] for a smooth start
  if(bin_pos >= (float)(DT_HL_KNEE_BINS - 1))
    return k->lift[DT_HL_KNEE_BINS - 1]; // past last center: flat-extend the lift

  const int i = (int)bin_pos;                                       // lower knot (bin) index
  const float bin_frac = bin_pos - (float)i;                        // interpolation weight toward the next knot
  return k->lift[i] * (1.f - bin_frac) + k->lift[i + 1] * bin_frac; // linear blend of adjacent per-bin lifts
}

// Blur up to four PLANAR planes in one four-channel pass: the recursive gaussian's per-pixel
// recursion is the bottleneck and its 4-channel variant runs the four lanes in SIMD, so this
// is ~3x cheaper than four single-plane calls. Pack/unpack are cheap linear passes. Per plane
// the result is identical to _knee_blur (the channels never mix in the recursion).
__DT_CLONE_TARGETS__
static void _knee_blur4(const float *const planes[4], float *const outs[4], const int n_planes, const int region_w,
                        const int region_h, const float sigma, float *const restrict pack_in,
                        float *const restrict pack_out)
{
  const size_t region_pixels = (size_t)region_w * region_h;
  dt_gaussian_t *const gaussian = _hl_gauss_get(region_w, region_h, 4, sigma);

  if(!gaussian)
  {
    for(int k = 0; k < n_planes; k++) memcpy(outs[k], planes[k], region_pixels * sizeof(float));
    return;
  }

  HL_PFOR()
  for(size_t i = 0; i < region_pixels; i++)
    for(int k = 0; k < 4; k++) pack_in[i * 4 + k] = (k < n_planes) ? planes[k][i] : 0.f;

  dt_gaussian_blur_4c(gaussian, pack_in, pack_out);

  HL_PFOR()
  for(size_t i = 0; i < region_pixels; i++)
    for(int k = 0; k < 4; k++)
      if(k < n_planes) outs[k][i] = pack_out[i * 4 + k];
}

// qsort comparator for floats (ascending)
static int _knee_cmp_float(const void *ptr_a, const void *ptr_b)
{
  const float float_a = *(const float *)ptr_a;
  const float float_b = *(const float *)ptr_b;
  return (float_a > float_b) - (float_a < float_b);
}

// Median of values[0..count-1]; sorts in place. Serves both the robust per-bin lift
// median{ v_hat_i - v_i } and the MAD spread of those same votes (Step 2).
static float _knee_median(float *const values, const size_t count)
{
  qsort(values, count, sizeof(float), _knee_cmp_float);
  return (count & 1) ? values[count / 2] : 0.5f * (values[count / 2 - 1] + values[count / 2]);
}

// Symmetric second-moment plane index for channels (chan_a, chan_b) in the joint moment buffer
// layout: planes 0 = n (trusted mass), 1..3 = means R G B, 4..9 = second moments RR RG RB GG GB BB.
// Once divided by n and de-meaned these give Var(u_a) / Cov(u_a,u_b), the entries of the 2x2 normal
// matrix (indexing is symmetric: _knee_p2(a,b) == _knee_p2(b,a)).
static inline int _knee_p2(const int chan_a, const int chan_b)
{
  static const int plane_lut[3][3] = { { 4, 5, 6 }, { 5, 7, 8 }, { 6, 8, 9 } };
  return plane_lut[chan_a][chan_b];
}

__DT_CLONE_TARGETS__
void _hl_knee_estimate(const float *const restrict input, const size_t width, const size_t height,
                       const uint32_t filters, const dt_iop_roi_t *const roi_in, const uint8_t (*const xtrans)[6],
                       const dt_aligned_pixel_t clipval_raw, _hl_knee_curve_t curves[3],
                       const dt_dev_pixelpipe_t *pipe)
{
  for(int c = 0; c < 3; c++)
  {
    curves[c].engaged = 0;
    memset(curves[c].lift, 0, sizeof(curves[c].lift));
  }

  // The curve is a global (per-channel) property, binned to <= ~1.5 Mpx. The base cell must
  // hold every CFA colour with a consistent phase: 2x2 for Bayer, 6x6 for X-Trans (the full
  // pattern period -- any smaller cell can miss a colour at some alignments).
  const int base = xtrans ? 6 : 2;
  int downsample = 1;
  while((width / ((size_t)base * downsample)) * (height / ((size_t)base * downsample)) > 1500000) downsample++;

  const int quad_size = base * downsample;
  const size_t bin_w = width / quad_size;
  const size_t bin_h = height / quad_size;
  const size_t bin_pixels = bin_w * bin_h;
  if(bin_w < 16 || bin_h < 16) return;

  float *const restrict binned
      = dt_pixelpipe_cache_alloc_align_float(bin_pixels * 3, pipe); // planar, clip-normalized
  float *const restrict pred = dt_pixelpipe_cache_alloc_align_float(bin_pixels * 3, pipe);
  float *const restrict r2_scores = dt_pixelpipe_cache_alloc_align_float(bin_pixels * 3, pipe);
  float *const restrict joint_moments
      = dt_pixelpipe_cache_alloc_align_float(bin_pixels * 10, pipe); // joint moment planes
  float *const restrict pair_moments
      = dt_pixelpipe_cache_alloc_align_float(bin_pixels * 6, pipe);                     // pair moment planes
  float *const restrict votes = dt_pixelpipe_cache_alloc_align_float(bin_pixels, pipe); // lift-fit bin scratch
  float *const restrict pk_in
      = dt_pixelpipe_cache_alloc_align_float(bin_pixels * 4, pipe); // _knee_blur4 pack scratch
  float *const restrict pk_out = dt_pixelpipe_cache_alloc_align_float(bin_pixels * 4, pipe);
  uint8_t *const restrict done = calloc(bin_pixels * 3, sizeof(uint8_t));

  if(IS_NULL_PTR(binned) || IS_NULL_PTR(pred) || IS_NULL_PTR(r2_scores) || IS_NULL_PTR(joint_moments)
     || IS_NULL_PTR(pair_moments) || IS_NULL_PTR(votes) || IS_NULL_PTR(pk_in) || IS_NULL_PTR(pk_out)
     || done == NULL)
    goto cleanup;

  // Bin the CFA per channel into clip-normalized planar planes: every qs x qs cell averages the
  // sites of each CFA colour it contains (phase-consistent, no inter-site interpolation).
  HL_PFOR(collapse(2))
  for(size_t i = 0; i < bin_h; i++)
    for(size_t j = 0; j < bin_w; j++)
    {
      dt_aligned_pixel_t accum = { 0.f, 0.f, 0.f, 0.f };
      dt_aligned_pixel_t counts = { 0.f, 0.f, 0.f, 0.f };

      for(int y = 0; y < quad_size; y++)
        for(int cell_x = 0; cell_x < quad_size; cell_x++)
        {
          const size_t row = i * quad_size + y;
          const size_t col = j * quad_size + cell_x;
          const size_t c = xtrans ? (size_t)FCxtrans((int)row, (int)col, roi_in, xtrans) : FC(row, col, filters);

          if(c <= 2)
          {
            accum[c] += input[row * width + col];
            counts[c] += 1.f;
          }
        }

      // per cell: co-located R / mean-G / B, each normalized to clip units v/(clip level) so the band
      // sits at [LO, DET); empty colours (no site of that colour in the cell) write 0
      for(int c = 0; c < 3; c++)
        binned[c * bin_pixels + i * bin_w + j] = (counts[c] > 0.f) ? accum[c] / (counts[c] * clipval_raw[c]) : 0.f;
    }

  // Band mass per channel: count binned cells in [LO, DET) -- the near-clip band [0.8c, 0.995c) the
  // knee corrects. A channel without a real band (< 200 cells) cannot trace a curve -> stays identity.
  size_t nband[3] = { 0, 0, 0 };
  for(size_t pixel = 0; pixel < bin_pixels; pixel++)
    for(int c = 0; c < 3; c++)
      if(binned[c * bin_pixels + pixel] >= DT_HL_KNEE_LO && binned[c * bin_pixels + pixel] < DT_HL_KNEE_DET)
        nband[c]++;

  if(nband[0] < 200 && nband[1] < 200 && nband[2] < 200) goto cleanup;

  // Multi-scale windowed colour-line predictions: per pixel keep the FINEST window that held
  // enough trusted mass. Joint 2-guide regression first (resolves two latent factors), then a
  // single-guide fallback where only one guide is itself trusted at the pixel. Sigmas are in
  // quad-cell units (x2 in CFA pixels), matching the prototype's 8..128 at scene resolution.
  const float sigmas[DT_HL_KNEE_NSIGMAS] = { 4.f, 8.f, 16.f, 32.f, 64.f };

  for(int sigma_index = 0; sigma_index < DT_HL_KNEE_NSIGMAS; sigma_index++)
  {
    const float sigma = sigmas[sigma_index];

    // ---- joint moments: weight w = 1 only where all three channels are trusted (< LO), so clipped
    // cells never vote; shared by every target channel. These ten raw planes, once blurred by
    // G_sigma below, become the windowed sums sum_y w G_sigma (...) feeding the normal equations. ----
    // All ten raw planes in one pass, then blurred 4-wide in place (via the pack scratch).
    HL_PFOR()
    for(size_t pixel = 0; pixel < bin_pixels; pixel++)
    {
      const float x_red = binned[0 * bin_pixels + pixel];
      const float x_green = binned[1 * bin_pixels + pixel];
      const float x_blue = binned[2 * bin_pixels + pixel];
      const float weight = (x_red < DT_HL_KNEE_LO && x_green < DT_HL_KNEE_LO && x_blue < DT_HL_KNEE_LO)
                               ? 1.f
                               : 0.f;                                     // trust mask w
      joint_moments[0 * bin_pixels + pixel] = weight;                     // plane 0: n = sum w   (trusted mass)
      joint_moments[1 * bin_pixels + pixel] = weight * x_red;             // plane 1: sum w*R  -> E[R]
      joint_moments[2 * bin_pixels + pixel] = weight * x_green;           // plane 2: sum w*G  -> E[G]
      joint_moments[3 * bin_pixels + pixel] = weight * x_blue;            // plane 3: sum w*B  -> E[B]
      joint_moments[4 * bin_pixels + pixel] = weight * x_red * x_red;     // plane 4: sum w*R*R -> E[R^2]
      joint_moments[5 * bin_pixels + pixel] = weight * x_red * x_green;   // plane 5: sum w*R*G -> E[R*G]
      joint_moments[6 * bin_pixels + pixel] = weight * x_red * x_blue;    // plane 6: sum w*R*B -> E[R*B]
      joint_moments[7 * bin_pixels + pixel] = weight * x_green * x_green; // plane 7: sum w*G*G -> E[G^2]
      joint_moments[8 * bin_pixels + pixel] = weight * x_green * x_blue;  // plane 8: sum w*G*B -> E[G*B]
      joint_moments[9 * bin_pixels + pixel] = weight * x_blue * x_blue;   // plane 9: sum w*B*B -> E[B^2]
    }

    for(int plane_base = 0; plane_base < 10; plane_base += 4)
    {
      const int n_planes = MIN(4, 10 - plane_base);
      const float *plane_in[4] = { 0 };
      float *plane_out[4] = { 0 };
      for(int k = 0; k < n_planes; k++)
        plane_in[k] = plane_out[k] = joint_moments + (size_t)(plane_base + k) * bin_pixels;
      _knee_blur4(plane_in, plane_out, n_planes, bin_w, bin_h, sigma, pk_in, pk_out);
    }

    // Joint 2-guide colour-line fit v_hat = a*u1 + b*u2 + d for each target channel c, solved from
    // the blurred moments via the 2x2 normal equations (Cramer's rule). Guides are the other two
    // channels; the two-factor solve resolves scenes a single guide would under-predict.
    for(int c = 0; c < 3; c++)
    {
      if(nband[c] < 200) continue;

      const int guide1 = (c == 0) ? 1 : 0; // u1 = first guide channel
      const int guide2 = (c == 2) ? 1 : 2; // u2 = second guide channel

      HL_PFOR()
      for(size_t pixel = 0; pixel < bin_pixels; pixel++)
      {
        if(done[c * bin_pixels + pixel]) continue; // finer sigma already served this cell (multi-scale)

        const float x_val = binned[c * bin_pixels + pixel];         // measured band value v of target c
        const float x_guide1 = binned[guide1 * bin_pixels + pixel]; // guide u1 at this cell
        const float x_guide2 = binned[guide2 * bin_pixels + pixel]; // guide u2 at this cell
        const float weight_sum = joint_moments[pixel];              // n = windowed trusted mass at this cell

        if(!(x_val >= DT_HL_KNEE_LO && x_val < DT_HL_KNEE_DET)) continue;     // only band cells [LO, DET) vote
        if(!(x_guide1 < DT_HL_KNEE_LO && x_guide2 < DT_HL_KNEE_LO)) continue; // both guides must be trusted here
        if(weight_sum <= DT_HL_KNEE_FMIN) continue; // too little trusted mass in the window -> skip

        const float inv_weight = 1.f / weight_sum; // 1/n, converts summed moments to expectations
        // windowed means E[.] = (sum w*.)/n
        const float mean_target = joint_moments[(size_t)(1 + c) * bin_pixels + pixel] * inv_weight;      // E[v]
        const float mean_guide1 = joint_moments[(size_t)(1 + guide1) * bin_pixels + pixel] * inv_weight; // E[u1]
        const float mean_guide2 = joint_moments[(size_t)(1 + guide2) * bin_pixels + pixel] * inv_weight; // E[u2]
        // second moments de-meaned = Var/Cov, centered about the per-window mean to avoid the float
        // E[u^2]-E[u]^2 cancellation on smooth content (squared mean dwarfs the variance)
        const float var_11 // Var(u1) = E[u1^2] - E[u1]^2   (normal-matrix diagonal, guide 1)
            = fmaxf(joint_moments[(size_t)_knee_p2(guide1, guide1) * bin_pixels + pixel] * inv_weight
                        - mean_guide1 * mean_guide1,
                    0.f);
        const float var_22 // Var(u2) = E[u2^2] - E[u2]^2   (normal-matrix diagonal, guide 2)
            = fmaxf(joint_moments[(size_t)_knee_p2(guide2, guide2) * bin_pixels + pixel] * inv_weight
                        - mean_guide2 * mean_guide2,
                    0.f);
        const float var_12 = joint_moments[(size_t)_knee_p2(guide1, guide2) * bin_pixels + pixel] * inv_weight
                             - mean_guide1 * mean_guide2; // Cov(u1,u2)  (off-diagonal of the normal matrix)
        const float cov_1 = joint_moments[(size_t)_knee_p2(c, guide1) * bin_pixels + pixel] * inv_weight
                            - mean_target * mean_guide1; // Cov(v,u1)  (RHS of the normal equations)
        const float cov_2 = joint_moments[(size_t)_knee_p2(c, guide2) * bin_pixels + pixel] * inv_weight
                            - mean_target * mean_guide2; // Cov(v,u2)  (RHS of the normal equations)
        const float var_target = fmaxf(joint_moments[(size_t)_knee_p2(c, c) * bin_pixels + pixel] * inv_weight
                                           - mean_target * mean_target,
                                       0.f); // Var(v), for the R^2 quality score

        // relative Tikhonov (ridge) damping lambda = 1e-3 * (Var u1 + Var u2)/2: scales with the
        // signal, never eats a weak-but-real slope
        const float lambda = 1e-3f * 0.5f * (var_11 + var_22) + 1e-12f;
        const float diag_11 = var_11 + lambda; // ridged normal-matrix diagonal [0][0]
        const float diag_22 = var_22 + lambda; // ridged normal-matrix diagonal [1][1]
        const float determinant = fmaxf(diag_11 * diag_22 - var_12 * var_12, 1e-18f); // det of the 2x2 system
        const float slope_1 = (diag_22 * cov_1 - var_12 * cov_2) / determinant; // a = slope on u1 (Cramer's rule)
        const float slope_2 = (diag_11 * cov_2 - var_12 * cov_1) / determinant; // b = slope on u2 (Cramer's rule)

        // v_hat(x) = E[v] + a*(u1 - E[u1]) + b*(u2 - E[u2])  (intercept d folded into the centering)
        pred[c * bin_pixels + pixel]
            = mean_target + slope_1 * (x_guide1 - mean_guide1) + slope_2 * (x_guide2 - mean_guide2);
        // R^2 = (a*Cov(v,u1) + b*Cov(v,u2)) / Var(v): explained-variance fraction, the vote's fit quality
        r2_scores[c * bin_pixels + pixel]
            = CLAMP((slope_1 * cov_1 + slope_2 * cov_2) / (var_target + 1e-12f), 0.f, 1.f);
        done[c * bin_pixels + pixel] = 1; // cell served at this (finest-so-far) sigma; coarser passes skip it
      }
    }

    // ---- single-guide fallback: simple regression v_hat = a*u + d where only one guide is itself
    // trusted at the cell (the joint fit needs both). Weight w = 1 where the target-guide PAIR is
    // trusted; slope from Cov(v,u)/Var(u). Fills cells the joint pass left `done == 0`. ----
    for(int chan_a = 0; chan_a < 3; chan_a++)
      for(int chan_b = chan_a + 1; chan_b < 3; chan_b++)
      {
        if(nband[chan_a] < 200 && nband[chan_b] < 200) continue;

        // pair moment planes: 0 = n (=sum w), 1 = sum w*a, 2 = sum w*b, 3 = sum w*a*a, 4 = sum w*b*b,
        // 5 = sum w*a*b -- all raw in one pass, then blurred 4-wide in place (via the pack scratch).
        HL_PFOR()
        for(size_t pixel = 0; pixel < bin_pixels; pixel++)
        {
          const float val_a = binned[chan_a * bin_pixels + pixel];
          const float val_b = binned[chan_b * bin_pixels + pixel];
          const float weight = (val_a < DT_HL_KNEE_LO && val_b < DT_HL_KNEE_LO) ? 1.f : 0.f; // pair trust mask w
          pair_moments[0 * bin_pixels + pixel] = weight;
          pair_moments[1 * bin_pixels + pixel] = weight * val_a;
          pair_moments[2 * bin_pixels + pixel] = weight * val_b;
          pair_moments[3 * bin_pixels + pixel] = weight * val_a * val_a;
          pair_moments[4 * bin_pixels + pixel] = weight * val_b * val_b;
          pair_moments[5 * bin_pixels + pixel] = weight * val_a * val_b;
        }

        for(int plane_base = 0; plane_base < 6; plane_base += 4)
        {
          const int n_planes = MIN(4, 6 - plane_base);
          const float *plane_in[4] = { 0 };
          float *plane_out[4] = { 0 };
          for(int k = 0; k < n_planes; k++)
            plane_in[k] = plane_out[k] = pair_moments + (size_t)(plane_base + k) * bin_pixels;
          _knee_blur4(plane_in, plane_out, n_planes, bin_w, bin_h, sigma, pk_in, pk_out);
        }

        // both orientations of the pair: predict a from b, then b from a (select which plane holds
        // the target's vs the guide's mean/second-moment accordingly)
        for(int orient = 0; orient < 2; orient++)
        {
          const int target_ch = orient ? chan_b : chan_a; // target channel v
          const int guide_ch = orient ? chan_a : chan_b;  // guide channel u
          const int target_mean_plane = orient ? 2 : 1;   // plane holding sum w*v
          const int guide_mean_plane = orient ? 1 : 2;    // plane holding sum w*u
          const int target_sq_plane = orient ? 4 : 3;     // plane holding sum w*v*v
          const int guide_sq_plane = orient ? 3 : 4;      // plane holding sum w*u*u

          if(nband[target_ch] < 200) continue;

          HL_PFOR()
          for(size_t pixel = 0; pixel < bin_pixels; pixel++)
          {
            if(done[target_ch * bin_pixels + pixel]) continue; // already served (joint or finer sigma)

            const float x_val = binned[target_ch * bin_pixels + pixel];  // measured band value v
            const float x_guide = binned[guide_ch * bin_pixels + pixel]; // guide u
            const float weight_sum = pair_moments[pixel];                // n = windowed trusted mass

            if(!(x_val >= DT_HL_KNEE_LO && x_val < DT_HL_KNEE_DET)) continue; // only band cells vote
            if(!(x_guide < DT_HL_KNEE_LO)) continue;                          // the single guide must be trusted
            if(weight_sum <= DT_HL_KNEE_FMIN) continue;                       // too little trusted mass -> skip

            const float inv_weight = 1.f / weight_sum; // 1/n
            const float mean_target
                = pair_moments[(size_t)target_mean_plane * bin_pixels + pixel] * inv_weight; // E[v]
            const float mean_guide
                = pair_moments[(size_t)guide_mean_plane * bin_pixels + pixel] * inv_weight; // E[u]
            const float covariance // Cov(v,u) = E[v*u] - E[v]E[u]  (plane 5 holds sum w*a*b)
                = pair_moments[(size_t)5 * bin_pixels + pixel] * inv_weight - mean_target * mean_guide;
            const float var_guide = fmaxf(pair_moments[(size_t)guide_sq_plane * bin_pixels + pixel] * inv_weight
                                              - mean_guide * mean_guide,
                                          0.f); // Var(u) = E[u^2] - E[u]^2
            const float var_target = fmaxf(pair_moments[(size_t)target_sq_plane * bin_pixels + pixel] * inv_weight
                                               - mean_target * mean_target,
                                           0.f);                                   // Var(v), for the R^2 score
            const float slope = covariance / (var_guide * (1.f + 1e-3f) + 1e-12f); // a = Cov(v,u)/Var(u), ridged

            pred[target_ch * bin_pixels + pixel]
                = mean_target + slope * (x_guide - mean_guide); // v_hat = E[v] + a*(u-E[u])
            r2_scores[target_ch * bin_pixels + pixel]           // R^2 = Cov^2 / (Var(u) Var(v)) for a single guide
                = CLAMP(covariance * covariance / (var_guide * var_target + 1e-18f), 0.f, 1.f);
            done[target_ch * bin_pixels + pixel] = 1; // cell now served
          }
        }
      }
  }

  // ---- Step 2, curve fit: per channel, pool the votes v_hat_i - v_i into 24 bins over the band,
  // take each bin's robust median lift (the median{ v_hat_i - v_i } of the equation), keep it only
  // when statistically significant, then make the curve monotone + raise-only. ----
  for(int c = 0; c < 3; c++)
  {
    if(nband[c] < 200) continue;

    // counting sort of the votes into DT_HL_KNEE_BINS = 24 bins by measured value v (offset[] is the
    // exclusive prefix-sum giving each bin's slot range in the flat `votes` scratch)
    size_t count[DT_HL_KNEE_BINS] = { 0 };
    size_t offset[DT_HL_KNEE_BINS + 1] = { 0 };
    const float bin_width = (DT_HL_KNEE_DET - DT_HL_KNEE_LO) / (float)DT_HL_KNEE_BINS; // band width / 24

    // pass 1: count votes per bin -- only cells that got a prediction (done) and cleared the fit-
    // quality gate R^2 > R2MIN participate (a poorly-fit pair does not get to vote)
    for(size_t pixel = 0; pixel < bin_pixels; pixel++)
    {
      if(!done[c * bin_pixels + pixel] || r2_scores[c * bin_pixels + pixel] <= DT_HL_KNEE_R2MIN) continue;
      const int bin_index // which of the 24 bins the measured value v falls in
          = CLAMP((int)((binned[c * bin_pixels + pixel] - DT_HL_KNEE_LO) / bin_width), 0, DT_HL_KNEE_BINS - 1);
      count[bin_index]++;
    }

    for(int i = 0; i < DT_HL_KNEE_BINS; i++)
      offset[i + 1] = offset[i] + count[i]; // prefix-sum -> per-bin slot base

    size_t fill[DT_HL_KNEE_BINS];
    memcpy(fill, offset, sizeof(fill)); // running write cursor per bin, seeded at each bin's base

    // pass 2: scatter each vote's lift v_hat_i - v_i (pred - measured) into its bin's slots
    for(size_t pixel = 0; pixel < bin_pixels; pixel++)
    {
      if(!done[c * bin_pixels + pixel] || r2_scores[c * bin_pixels + pixel] <= DT_HL_KNEE_R2MIN) continue;
      const float x_val = binned[c * bin_pixels + pixel]; // measured v
      const int bin_index = CLAMP((int)((x_val - DT_HL_KNEE_LO) / bin_width), 0, DT_HL_KNEE_BINS - 1);
      votes[fill[bin_index]++] = pred[c * bin_pixels + pixel] - x_val; // one pixel's vote v_hat_i - v_i
    }

    // per-bin robust lift, accepted only when significant vs the bin median's standard error --
    // the raise-only clamp would otherwise rectify zero-mean noise into fake lift
    float lift[DT_HL_KNEE_BINS];
    int seen[DT_HL_KNEE_BINS];
    int nseen = 0;

    for(int i = 0; i < DT_HL_KNEE_BINS; i++)
    {
      lift[i] = 0.f;
      seen[i] = 0;
      if(count[i] < DT_HL_KNEE_MINVOTES) continue; // need >= 100 votes so the error estimate itself is stable

      float *const bin_votes = votes + offset[i];
      const float median_lift = _knee_median(bin_votes, count[i]); // median{ v_hat_i - v_i } = the bin's raw lift

      // median absolute deviation (MAD) around the median, a robust spread estimate
      // (d is sorted, values get overwritten -- fine, last use)
      for(size_t k = 0; k < count[i]; k++) bin_votes[k] = fabsf(bin_votes[k] - median_lift); // |lift_i - median|
      const float median_abs_dev = _knee_median(bin_votes, count[i]); // MAD = median|lift_i - median(lift)|
      // SE of the bin median = 1.858*MAD/sqrt(n); 1.858 = 1.4826 (MAD->sigma) * 1.2533 (sigma->SE of median)
      const float std_err = 1.858f * median_abs_dev / sqrtf((float)count[i]);

      seen[i] = 1; // this bin is populated (has a usable estimate), whether or not the lift is significant
      nseen++;
      // significance gate: accept the lift only if median > NSIGMA*SE (2*SE, ~95% one-sided) -- otherwise
      // it stays 0, so the raise-only clamp below cannot rectify zero-mean noise into a fake lift
      if(median_lift > DT_HL_KNEE_NSIGMA * std_err) lift[i] = median_lift;
    }

    if(nseen < 3) continue; // too few populated bins to trust a curve -> leave channel at identity

    // interpolate lift over unseen (under-populated) bins (flat-extend past the first/last seen bin),
    // linearly between two seen bins -- the C twin of the prototype's np.interp over centers[seen]
    int prev = -1; // index of the nearest seen bin to the left (-1 = none yet)

    for(int i = 0; i < DT_HL_KNEE_BINS; i++)
    {
      if(seen[i])
      {
        prev = i;
        continue;
      }

      int next = -1;
      for(int k = i + 1; k < DT_HL_KNEE_BINS; k++)
        if(seen[k])
        {
          next = k;
          break;
        }

      if(prev < 0 && next < 0)
        lift[i] = 0.f;
      else if(prev < 0)
        lift[i] = lift[next];
      else if(next < 0)
        lift[i] = lift[prev];
      else
        lift[i] = lift[prev] + (lift[next] - lift[prev]) * (float)(i - prev) / (float)(next - prev);
    }

    // monotone raise-only clamp: cumulative max makes the curve non-decreasing (rolloff bias grows
    // toward clip) and drops any residual negatives -- the C twin of np.maximum.accumulate(max(lift,0))
    float running_max = 0.f;
    float lift_max = 0.f;

    for(int i = 0; i < DT_HL_KNEE_BINS; i++)
    {
      running_max = fmaxf(running_max, fmaxf(lift[i], 0.f)); // running max enforces monotone non-decreasing
      curves[c].lift[i] = running_max;                       // final per-bin lift knot for this channel
      lift_max = fmaxf(lift_max, running_max);               // peak lift, for the engage test below
    }

    // engage threshold: a peak lift below ENGAGE = 0.005 is noise -> stay identity (the no-op guarantee:
    // hard-clipped data yields near-zero medians, so the correction costs nothing)
    curves[c].engaged = (lift_max >= DT_HL_KNEE_ENGAGE);
    if(!curves[c].engaged) memset(curves[c].lift, 0, sizeof(curves[c].lift));
  }

cleanup:;
  dt_pixelpipe_cache_free_align(binned);
  dt_pixelpipe_cache_free_align(pred);
  dt_pixelpipe_cache_free_align(r2_scores);
  dt_pixelpipe_cache_free_align(joint_moments);
  dt_pixelpipe_cache_free_align(pair_moments);
  dt_pixelpipe_cache_free_align(votes);
  dt_pixelpipe_cache_free_align(pk_in);
  dt_pixelpipe_cache_free_align(pk_out);
  free(done);
}

__DT_CLONE_TARGETS__
void _hl_knee_apply_interpolated(float *const restrict interpolated, const size_t npix,
                                 const dt_aligned_pixel_t clipvaln, const dt_aligned_pixel_t wb4,
                                 const _hl_knee_curve_t curves[3])
{
  HL_PFOR()
  for(size_t pixel = 0; pixel < npix; pixel++)
  {
    int touched = 0;

    for(int c = 0; c < 3; c++)
    {
      if(!curves[c].engaged) continue; // channel with no measured rolloff -> pass through untouched

      const float norm_val = interpolated[pixel * 4 + c] / clipvaln[c]; // v in clip units

      if(norm_val >= DT_HL_KNEE_LO && norm_val < DT_HL_KNEE_DET) // only band values are corrected
      {
        const float lift = _knee_lift_of(&curves[c], norm_val); // L(v) from the fitted curve

        if(lift > 0.f)
        {
          interpolated[pixel * 4 + c] = (norm_val + lift) * clipvaln[c]; // v + L(v), back to raw-scaled units
          touched = 1;
        }
      }
    }

    if(touched) // rebuild norm = || white-balanced RGB || so the guide norm stays consistent
    {
      const float val_r = interpolated[pixel * 4 + 0] * wb4[0];
      const float val_g = interpolated[pixel * 4 + 1] * wb4[1];
      const float val_b = interpolated[pixel * 4 + 2] * wb4[2];
      interpolated[pixel * 4 + 3] = sqrtf(sqf(val_r) + sqf(val_g) + sqf(val_b));
    }
  }
}

__DT_CLONE_TARGETS__
void _hl_knee_apply_cfa(const float *const restrict input, float *const restrict input_corr, const size_t width,
                        const size_t height, const uint32_t filters, const dt_iop_roi_t *const roi_in,
                        const uint8_t (*const xtrans)[6], const dt_aligned_pixel_t clipval_raw,
                        const _hl_knee_curve_t curves[3])
{
  HL_PFOR(collapse(2))
  for(size_t i = 0; i < height; i++)
    for(size_t j = 0; j < width; j++)
    {
      const size_t idx = i * width + j;
      const size_t c
          = xtrans ? (size_t)FCxtrans((int)i, (int)j, roi_in, xtrans) : FC(i, j, filters); // CFA colour here
      float value = input[idx];

      if(c <= 2 && curves[c].engaged)
      {
        const float norm_val = value / clipval_raw[c]; // v in clip units

        if(norm_val >= DT_HL_KNEE_LO && norm_val < DT_HL_KNEE_DET) // only band pixels get k^-1(v) = v + L(v)
          value = (norm_val + _knee_lift_of(&curves[c], norm_val)) * clipval_raw[c];
      }

      input_corr[idx] = value; // unclipped/clipped/out-of-band values pass through unchanged
    }
}

// env-gated CPU/GPU parity self-tests (same translation unit, see the file header)

// ============================ OpenCL ============================

#ifdef HAVE_OPENCL
cl_int _hl_knee_estimate_cl(const int devid, void *gd_void, cl_mem dev_in, const size_t width, const size_t height,
                            const uint32_t filters, const dt_iop_roi_t *const roi_in, cl_mem dev_xtrans,
                            const int is_xtrans, const dt_aligned_pixel_t clipval_raw, _hl_knee_curve_t curves[3],
                            const dt_dev_pixelpipe_t *pipe)
{
  dt_iop_highlights_global_data_t *global_data = (dt_iop_highlights_global_data_t *)gd_void;
  cl_int cl_err = DT_OPENCL_DEFAULT_ERROR;
  dt_gaussian_cl_t *gsig = NULL; // one blur handle per sigma (9 blurs each)

  for(int c = 0; c < 3; c++)
  {
    curves[c].engaged = 0;
    memset(curves[c].lift, 0, sizeof(curves[c].lift));
  }

  const int base = is_xtrans ? 6 : 2;
  int downsample = 1;
  while((width / ((size_t)base * downsample)) * (height / ((size_t)base * downsample)) > 1500000) downsample++;

  const int quad_size = base * downsample;
  const size_t bin_w = width / quad_size;
  const size_t bin_h = height / quad_size;
  const size_t bin_pixels = bin_w * bin_h;
  if(bin_w < 16 || bin_h < 16) return CL_SUCCESS; // like the CPU: no estimate, identity curves

  const int bin_w_int = (int)bin_w, bin_h_int = (int)bin_h;
  size_t work_sizes[3] = { ROUNDUPDWD(bin_w_int, devid), ROUNDUPDHT(bin_h_int, devid), 1 };

  cl_mem dev_binned = dt_opencl_alloc_device_buffer(devid, sizeof(float) * bin_pixels * 3);
  cl_mem dev_pred = dt_opencl_alloc_device_buffer(devid, sizeof(float) * bin_pixels * 3);
  cl_mem dev_r2 = dt_opencl_alloc_device_buffer(devid, sizeof(float) * bin_pixels * 3);
  cl_mem dev_done = dt_opencl_alloc_device_buffer(devid, bin_pixels * 3);
  cl_mem moment_a = dt_opencl_alloc_device(devid, bin_w_int, bin_h_int, 4 * sizeof(float));
  cl_mem moment_b = dt_opencl_alloc_device(devid, bin_w_int, bin_h_int, 4 * sizeof(float));
  cl_mem moment_c = dt_opencl_alloc_device(devid, bin_w_int, bin_h_int, 4 * sizeof(float));
  cl_mem blur_a = dt_opencl_alloc_device(devid, bin_w_int, bin_h_int, 4 * sizeof(float));
  cl_mem blur_b = dt_opencl_alloc_device(devid, bin_w_int, bin_h_int, 4 * sizeof(float));
  cl_mem blur_c = dt_opencl_alloc_device(devid, bin_w_int, bin_h_int, 4 * sizeof(float));
  float *binned = dt_pixelpipe_cache_alloc_align_float(bin_pixels * 3, pipe);
  float *pred = dt_pixelpipe_cache_alloc_align_float(bin_pixels * 3, pipe);
  float *r2_scores = dt_pixelpipe_cache_alloc_align_float(bin_pixels * 3, pipe);
  float *votes = dt_pixelpipe_cache_alloc_align_float(bin_pixels, pipe);
  uint8_t *done = calloc(bin_pixels * 3, sizeof(uint8_t));
  if(!dev_binned || !dev_pred || !dev_r2 || !dev_done || !moment_a || !moment_b || !moment_c || !blur_a || !blur_b
     || !blur_c || !binned || !pred || !r2_scores || !votes || !done)
    goto cleanup;

  cl_err = dt_opencl_write_buffer_to_device(devid, done, dev_done, 0, bin_pixels * 3, CL_TRUE);
  if(cl_err != CL_SUCCESS) goto cleanup;

  // ---- binning ----
  {
    const int kernel = global_data->kernel_hl_knee_bin;
    const int width_int = (int)width;
    const int height_int = (int)height;
    const int quad_size_int = quad_size;
    const int roi_x = roi_in ? roi_in->x : 0;
    const int roi_y = roi_in ? roi_in->y : 0;
    const cl_float4 clip4 = { { clipval_raw[0], clipval_raw[1], clipval_raw[2], 1.f } };
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &dev_in);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &dev_binned);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &width_int);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &height_int);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &bin_w_int);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &bin_h_int);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &quad_size_int);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(uint32_t), &filters);
    dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &roi_x);
    dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(int), &roi_y);
    dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(int), &is_xtrans);
    dt_opencl_set_kernel_arg(devid, kernel, 11, sizeof(cl_mem), &dev_xtrans);
    dt_opencl_set_kernel_arg(devid, kernel, 12, sizeof(cl_float4), &clip4);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, work_sizes);
    if(cl_err != CL_SUCCESS) goto cleanup;
  }

  // the binned planes come home once: Phase B needs them, and they carry the band mass
  cl_err
      = dt_opencl_read_buffer_from_device(devid, binned, dev_binned, 0, sizeof(float) * bin_pixels * 3, CL_TRUE);
  if(cl_err != CL_SUCCESS) goto cleanup;

  // band mass per channel: count binned cells in [LO, DET) (mirrors the CPU); a channel with < 200
  // stays identity
  size_t nband[3] = { 0, 0, 0 };
  for(size_t pixel = 0; pixel < bin_pixels; pixel++)
    for(int c = 0; c < 3; c++)
      if(binned[c * bin_pixels + pixel] >= DT_HL_KNEE_LO && binned[c * bin_pixels + pixel] < DT_HL_KNEE_DET)
        nband[c]++;

  if(nband[0] < 200 && nband[1] < 200 && nband[2] < 200)
  {
    cl_err = CL_SUCCESS;
    goto cleanup;
  }

  // ---- Phase A: multi-scale windowed regressions on the device ----
  {
    const float sigmas[DT_HL_KNEE_NSIGMAS] = { 4.f, 8.f, 16.f, 32.f, 64.f };
    const float knee_lo = DT_HL_KNEE_LO;
    const float knee_det = DT_HL_KNEE_DET;
    const float knee_fmin = DT_HL_KNEE_FMIN;

    for(int sigma_index = 0; sigma_index < DT_HL_KNEE_NSIGMAS; sigma_index++)
    {
      const float sigma = sigmas[sigma_index];
      dt_gaussian_free_cl(gsig); // previous sigma's handle
      gsig = NULL;

      // joint moments (n, means, second moments; 10 planes packed in 3 float4 images), then blurred
      // by G_sigma to realise the windowed sums sum_y w G_sigma(...) of the 2x2 normal equations
      {
        const int kernel = global_data->kernel_hl_knee_jmom;
        dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &dev_binned);
        dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &moment_a);
        dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &moment_b);
        dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &moment_c);
        dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &bin_w_int);
        dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &bin_h_int);
        dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(float), &knee_lo);
        cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, work_sizes);
        if(cl_err != CL_SUCCESS) goto cleanup;
      }
      gsig = _region_blur_handle(devid, bin_w_int, bin_h_int, sigma);
      if(!gsig)
      {
        cl_err = DT_OPENCL_DEFAULT_ERROR;
        goto cleanup;
      }
      cl_err = dt_gaussian_blur_cl(gsig, moment_a, blur_a);
      if(cl_err == CL_SUCCESS) cl_err = dt_gaussian_blur_cl(gsig, moment_b, blur_b);
      if(cl_err == CL_SUCCESS) cl_err = dt_gaussian_blur_cl(gsig, moment_c, blur_c);
      if(cl_err != CL_SUCCESS) goto cleanup;

      // joint 2-guide regression v_hat = a*u1 + b*u2 + d per target channel c, solving the 2x2 normal
      // system from the blurred moments (the kernel does the Cramer's-rule solve; writes pred/r2/done)
      for(int c = 0; c < 3; c++)
      {
        if(nband[c] < 200) continue;
        const int guide1 = (c == 0) ? 1 : 0; // u1 guide channel
        const int guide2 = (c == 2) ? 1 : 2; // u2 guide channel
        const int kernel = global_data->kernel_hl_knee_joint_reg;
        dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &dev_binned);
        dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &blur_a);
        dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &blur_b);
        dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &blur_c);
        dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &dev_pred);
        dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(cl_mem), &dev_r2);
        dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(cl_mem), &dev_done);
        dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &bin_w_int);
        dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &bin_h_int);
        dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(int), &c);
        dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(int), &guide1);
        dt_opencl_set_kernel_arg(devid, kernel, 11, sizeof(int), &guide2);
        dt_opencl_set_kernel_arg(devid, kernel, 12, sizeof(float), &knee_lo);
        dt_opencl_set_kernel_arg(devid, kernel, 13, sizeof(float), &knee_det);
        dt_opencl_set_kernel_arg(devid, kernel, 14, sizeof(float), &knee_fmin);
        cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, work_sizes);
        if(cl_err != CL_SUCCESS) goto cleanup;
      }

      // single-guide fallback v_hat = a*u + d (a = Cov(v,u)/Var(u)) for cells the joint pass left
      // done==0, both orientations of each pair (predict a from b, then b from a)
      for(int chan_a = 0; chan_a < 3; chan_a++)
        for(int chan_b = chan_a + 1; chan_b < 3; chan_b++)
        {
          if(nband[chan_a] < 200 && nband[chan_b] < 200) continue;
          {
            const int kernel = global_data->kernel_hl_knee_pmom;
            dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &dev_binned);
            dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &moment_a);
            dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &moment_b);
            dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &bin_w_int);
            dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &bin_h_int);
            dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &chan_a);
            dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &chan_b);
            dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(float), &knee_lo);
            cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, work_sizes);
            if(cl_err != CL_SUCCESS) goto cleanup;
          }
          cl_err = dt_gaussian_blur_cl(gsig, moment_a, blur_a);
          if(cl_err == CL_SUCCESS) cl_err = dt_gaussian_blur_cl(gsig, moment_b, blur_b);
          if(cl_err != CL_SUCCESS) goto cleanup;

          for(int orient = 0; orient < 2; orient++)
          {
            const int target_ch = orient ? chan_b : chan_a;
            const int guide_ch = orient ? chan_a : chan_b;
            const int is_first_orient = (orient == 0);
            if(nband[target_ch] < 200) continue;
            const int kernel = global_data->kernel_hl_knee_pair_reg;
            dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &dev_binned);
            dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &blur_a);
            dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &blur_b);
            dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &dev_pred);
            dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &dev_r2);
            dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(cl_mem), &dev_done);
            dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &bin_w_int);
            dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &bin_h_int);
            dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &target_ch);
            dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(int), &guide_ch);
            dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(int), &is_first_orient);
            dt_opencl_set_kernel_arg(devid, kernel, 11, sizeof(float), &knee_lo);
            dt_opencl_set_kernel_arg(devid, kernel, 12, sizeof(float), &knee_det);
            dt_opencl_set_kernel_arg(devid, kernel, 13, sizeof(float), &knee_fmin);
            cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, work_sizes);
            if(cl_err != CL_SUCCESS) goto cleanup;
          }
        }
    }
  }

  cl_err = dt_opencl_read_buffer_from_device(devid, pred, dev_pred, 0, sizeof(float) * bin_pixels * 3, CL_TRUE);
  if(cl_err == CL_SUCCESS)
    cl_err
        = dt_opencl_read_buffer_from_device(devid, r2_scores, dev_r2, 0, sizeof(float) * bin_pixels * 3, CL_TRUE);
  if(cl_err == CL_SUCCESS)
    cl_err = dt_opencl_read_buffer_from_device(devid, done, dev_done, 0, bin_pixels * 3, CL_TRUE);
  if(cl_err != CL_SUCCESS) goto cleanup;

  // ---- Phase B (host, identical to the CPU _hl_knee_estimate; see there for the full maths):
  // pool the votes v_hat_i - v_i into 24 band bins, take each bin's significant median lift, then
  // make the curve monotone + raise-only. ----
  for(int c = 0; c < 3; c++)
  {
    if(nband[c] < 200) continue;

    size_t count[DT_HL_KNEE_BINS] = { 0 };
    size_t offset[DT_HL_KNEE_BINS + 1] = { 0 };
    const float bin_width = (DT_HL_KNEE_DET - DT_HL_KNEE_LO) / (float)DT_HL_KNEE_BINS; // band width / 24

    // pass 1: count votes per bin (only predicted cells clearing the R^2 > R2MIN fit-quality gate)
    for(size_t pixel = 0; pixel < bin_pixels; pixel++)
    {
      if(!done[c * bin_pixels + pixel] || r2_scores[c * bin_pixels + pixel] <= DT_HL_KNEE_R2MIN) continue;
      const int bin_index // bin of the measured value v
          = CLAMP((int)((binned[c * bin_pixels + pixel] - DT_HL_KNEE_LO) / bin_width), 0, DT_HL_KNEE_BINS - 1);
      count[bin_index]++;
    }
    for(int i = 0; i < DT_HL_KNEE_BINS; i++) offset[i + 1] = offset[i] + count[i]; // prefix-sum -> per-bin base

    size_t fill[DT_HL_KNEE_BINS];
    memcpy(fill, offset, sizeof(fill));
    // pass 2: scatter each vote v_hat_i - v_i (pred - measured) into its bin's slots
    for(size_t pixel = 0; pixel < bin_pixels; pixel++)
    {
      if(!done[c * bin_pixels + pixel] || r2_scores[c * bin_pixels + pixel] <= DT_HL_KNEE_R2MIN) continue;
      const float x_val = binned[c * bin_pixels + pixel]; // measured v
      const int bin_index = CLAMP((int)((x_val - DT_HL_KNEE_LO) / bin_width), 0, DT_HL_KNEE_BINS - 1);
      votes[fill[bin_index]++] = pred[c * bin_pixels + pixel] - x_val; // one pixel's vote
    }

    float lift[DT_HL_KNEE_BINS];
    int seen[DT_HL_KNEE_BINS];
    int nseen = 0;
    for(int i = 0; i < DT_HL_KNEE_BINS; i++)
    {
      lift[i] = 0.f;
      seen[i] = 0;
      if(count[i] < DT_HL_KNEE_MINVOTES) continue; // need >= 100 votes for a stable error estimate
      float *const bin_votes = votes + offset[i];
      const float median_lift = _knee_median(bin_votes, count[i]); // median{ v_hat_i - v_i } = raw bin lift
      for(size_t k = 0; k < count[i]; k++) bin_votes[k] = fabsf(bin_votes[k] - median_lift); // |lift_i - median|
      const float median_abs_dev = _knee_median(bin_votes, count[i]);                        // MAD (robust spread)
      // SE of the bin median = 1.858*MAD/sqrt(n); 1.858 = 1.4826 (MAD->sigma) * 1.2533 (sigma->SE of median)
      const float std_err = 1.858f * median_abs_dev / sqrtf((float)count[i]);
      seen[i] = 1;
      nseen++;
      if(median_lift > DT_HL_KNEE_NSIGMA * std_err)
        lift[i] = median_lift; // accept only if lift > 2*SE (~95% gate)
    }
    if(nseen < 3) continue; // too few populated bins -> identity

    // interpolate lift over unseen bins (flat-extend the ends, linear between two seen bins)
    int prev = -1;
    for(int i = 0; i < DT_HL_KNEE_BINS; i++)
    {
      if(seen[i])
      {
        prev = i;
        continue;
      }
      int next = -1;
      for(int k = i + 1; k < DT_HL_KNEE_BINS; k++)
        if(seen[k])
        {
          next = k;
          break;
        }
      if(prev < 0 && next < 0)
        lift[i] = 0.f;
      else if(prev < 0)
        lift[i] = lift[next]; // flat-extend before the first seen bin
      else if(next < 0)
        lift[i] = lift[prev]; // flat-extend past the last seen bin
      else
        lift[i] = lift[prev] + (lift[next] - lift[prev]) * (float)(i - prev) / (float)(next - prev); // linear
    }

    // monotone raise-only clamp: cumulative max (rolloff bias grows toward clip), negatives dropped
    float running_max = 0.f;
    float lift_max = 0.f;
    for(int i = 0; i < DT_HL_KNEE_BINS; i++)
    {
      running_max = fmaxf(running_max, fmaxf(lift[i], 0.f));
      curves[c].lift[i] = running_max;
      lift_max = fmaxf(lift_max, running_max);
    }
    // engage threshold: peak lift below ENGAGE = 0.005 is noise -> identity (no-op guarantee)
    curves[c].engaged = (lift_max >= DT_HL_KNEE_ENGAGE);
    if(!curves[c].engaged) memset(curves[c].lift, 0, sizeof(curves[c].lift));
  }
  cl_err = CL_SUCCESS;

cleanup:
  dt_gaussian_free_cl(gsig);
  dt_opencl_release_mem_object(dev_binned);
  dt_opencl_release_mem_object(dev_pred);
  dt_opencl_release_mem_object(dev_r2);
  dt_opencl_release_mem_object(dev_done);
  dt_opencl_release_mem_object(moment_a);
  dt_opencl_release_mem_object(moment_b);
  dt_opencl_release_mem_object(moment_c);
  dt_opencl_release_mem_object(blur_a);
  dt_opencl_release_mem_object(blur_b);
  dt_opencl_release_mem_object(blur_c);
  dt_pixelpipe_cache_free_align(binned);
  dt_pixelpipe_cache_free_align(pred);
  dt_pixelpipe_cache_free_align(r2_scores);
  dt_pixelpipe_cache_free_align(votes);
  free(done);
  return cl_err;
}

cl_int _hl_knee_apply_cfa_cl(const int devid, void *gd_void, cl_mem dev_in, cl_mem dev_out, const size_t width,
                             const size_t height, const uint32_t filters, const dt_iop_roi_t *const roi_in,
                             cl_mem dev_xtrans, const int is_xtrans, const dt_aligned_pixel_t clipval_raw,
                             const _hl_knee_curve_t curves[3])
{
  dt_iop_highlights_global_data_t *global_data = (dt_iop_highlights_global_data_t *)gd_void;
  const int width_int = (int)width;
  const int height_int = (int)height;
  size_t work_sizes[3] = { ROUNDUPDWD(width_int, devid), ROUNDUPDHT(height_int, devid), 1 };

  float lift[3 * DT_HL_KNEE_BINS];
  for(int c = 0; c < 3; c++) memcpy(lift + c * DT_HL_KNEE_BINS, curves[c].lift, sizeof(curves[c].lift));
  cl_mem dev_lift = _sp_cl_upload(devid, lift, sizeof(lift));
  if(!dev_lift) return DT_OPENCL_DEFAULT_ERROR;

  const int kernel = global_data->kernel_hl_knee_apply;
  const int roi_x = roi_in ? roi_in->x : 0;
  const int roi_y = roi_in ? roi_in->y : 0;
  const cl_float4 clip4 = { { clipval_raw[0], clipval_raw[1], clipval_raw[2], 1.f } };
  const cl_int4 engaged_flags = { { curves[0].engaged, curves[1].engaged, curves[2].engaged, 0 } };
  const float knee_lo = DT_HL_KNEE_LO;
  const float knee_det = DT_HL_KNEE_DET;
  const int bins = DT_HL_KNEE_BINS;
  dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &dev_in);
  dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &dev_out);
  dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &width_int);
  dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &height_int);
  dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(uint32_t), &filters);
  dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &roi_x);
  dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &roi_y);
  dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &is_xtrans);
  dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(cl_mem), &dev_xtrans);
  dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(cl_float4), &clip4);
  dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(cl_mem), &dev_lift);
  dt_opencl_set_kernel_arg(devid, kernel, 11, sizeof(cl_int4), &engaged_flags);
  dt_opencl_set_kernel_arg(devid, kernel, 12, sizeof(float), &knee_lo);
  dt_opencl_set_kernel_arg(devid, kernel, 13, sizeof(float), &knee_det);
  dt_opencl_set_kernel_arg(devid, kernel, 14, sizeof(int), &bins);
  const cl_int cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, work_sizes);
  dt_opencl_release_mem_object(dev_lift);
  return cl_err;
}

#endif // HAVE_OPENCL
