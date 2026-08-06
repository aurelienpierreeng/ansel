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

// Self-dome fallback and all-clip joint core stages (CPU + OpenCL). (implementation; see core.h for the public
// API.)

#include "common/openmp.h"
#include "common/simd.h"
#include "common/target_clones.h"
#include "develop/pixelpipe_cache_alloc.h"
#include "develop/imageop.h"
#include "iop/highlights/coefficient_field.h"
#include "iop/highlights/core.h"
#include "iop/highlights/dome.h"
#include "iop/highlights/knee.h"
#include "iop/highlights/pde.h"
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>

__DT_CLONE_TARGETS__
void _selfdome(_hl_region_ctx_t *const ctx)
{
  const _hl_region_t *const region = ctx->region;
  const dt_dev_pixelpipe_t *const pipe = ctx->pipe;
  const int region_w = ctx->region_w;
  const int region_h = ctx->region_h;
  const size_t region_pixels = ctx->region_pixels;
  const float epsilon = ctx->epsilon;
  float *const restrict estimate = ctx->estimate;
  float *const restrict valid = ctx->valid;
  float *const restrict plane1 = ctx->plane1;
  float *const restrict valid_variance = ctx->valid_variance;
  float *const restrict clip0 = ctx->clip0;
  uint8_t *const restrict hole = ctx->hole;
  float *const restrict solver_field = ctx->solver_field;
  float *const restrict dome_lum = ctx->dome_lum;
  float *const restrict lum_accum = ctx->lum_accum;
  float *const restrict flat_target = ctx->flat_target;

  // --- decide whether the per-channel self-dome fallback is worth solving ---
  // It only matters where a channel is clipped, a guide survives, yet the colour-line is
  // weak (We = Wc^2 well below 1): decorrelated content. Correlated content stays on the
  // guide (We ~ 1), so skip the three biharmonic solves entirely -- the common case.
  int need_self = 0;
  for(size_t i = 0; i < region_pixels; i++)
  {
    const int anyvalid = (valid[i * 4 + 0] >= 0.5f) || (valid[i * 4 + 1] >= 0.5f) || (valid[i * 4 + 2] >= 0.5f);
    if(!anyvalid) continue;
    for(int c = 0; c < 3; c++)
      if(valid[i * 4 + c] < 0.5f && valid_variance[i * 4 + c] * valid_variance[i * 4 + c] < 0.9f) need_self = 1;
    if(need_self) break;
  }

  // --- self-dome fallback, only if needed ---
  if(need_self)
  {
    // One SHARED downsampling factor sized from the UNION (any-clip) hole -- the largest, so
    // the coarse grid stays within DT_HL_DOME_NMAX and every channel is approximated at the
    // same resolution.
    size_t nh_union = 0;
    for(size_t i = 0; i < region_pixels; i++)
      if(valid[i * 4 + 0] < 0.5f || valid[i * 4 + 1] < 0.5f || valid[i * 4 + 2] < 0.5f) nh_union++;

    const int ds_shared = MAX(1, (int)ceilf(sqrtf((float)nh_union / (float)DT_HL_DOME_NMAX_SPARSE)));

    // HUE-COUPLED dome: three independently-domed channels can drift apart exactly where the
    // fallback engages (a low-R^2 zone), splitting the hue toward green/magenta -- the original
    // failure this fallback used to be disabled for. Instead dome ONE shared quantity per kind:
    // the LUMINANCE (biharmonic, gradient-extending) and a SMOOTH chromaticity (harmonic fill
    // of the ratios from the rim). dome_c = L_dome * chroma_c: every channel shares the same
    // shape, so the fallback cannot drift the hue by construction.
    HL_PFOR()
    for(size_t i = 0; i < region_pixels; i++)
    {
      hole[i] = (valid[i * 4 + 0] < 0.5f || valid[i * 4 + 1] < 0.5f || valid[i * 4 + 2] < 0.5f);
      lum_accum[i] = estimate[i * 4 + 0] + estimate[i * 4 + 1] + estimate[i * 4 + 2]; // L_sum = R+G+B
      solver_field[i] = lum_accum[i];
    }

    // one shared biharmonic BRIGHTNESS dome over the union hole: Delta^2 L_sum = 0 with the
    // valid rim as Dirichlet data (term 2 of E_bihar, hue-coupled form). Doming L_sum once and
    // reusing it for all channels is what prevents three per-channel domes drifting the hue.
    _biharmonic_dome(solver_field, hole, region_w, region_h, ds_shared, pipe);
    memcpy(dome_lum, solver_field, region_pixels * sizeof(float));

    // smooth chromaticity over the union hole (ratio planes stored in s1's 4-ch layout): each
    // channel's ratio r_c = est_c / L_sum is a BOUNDED quantity, so a plain harmonic fill (flat
    // rim-matched inpaint, no biharmonic doming) is the right tool -- brightness gets the dome,
    // colour gets the harmonic fill, and recombining as dome_c = L_dome * r_c couples the hue.
    const int cf_base = (int)(CLAMP(region->radius / 6.f, 8.f, 64.f) / 4.f);
    const float floor_gate = ctx->floor_gate; // clip-asymmetry gate, see _hl_floor_gate (common.h)

    // Mean valid chromaticity (gate > 0 only): the flat target the hole-interior dome chroma is
    // pulled toward, lifting it off the biased rim-harmonic value toward the true surround. The
    // pull strength scales with the gate (0 at unit-WB clips = approved behavior).
    dt_aligned_pixel_t cmean = { 0.f, 0.f, 0.f, 0.f };
    float cmean_beta = 0.f;
    float refine_gate = 0.f; // floor_gate x trusted-ring vote (see below)
    if(floor_gate > 1e-6f)
    {
      // BRIGHT valid pixels only (>= 0.35 x blown-zone plateau): the whole-window mean is
      // contaminated by dark, unrelated content (the cgrad anchors learned the same lesson) --
      // measured on MAC/sunrise: the ring vote against the all-valid mean stays closed on
      // exactly the scenes the refinements are for.
      double plateau_sum = 0.0, plateau_count = 0.0;
      for(size_t i = 0; i < region_pixels; i++)
        if(hole[i])
        {
          plateau_sum += (double)lum_accum[i];
          plateau_count += 1.0;
        }
      const float lum_min = (plateau_count > 0.0) ? 0.35f * (float)(plateau_sum / plateau_count) : 0.f;
      double cmean_accum[3] = { 0.0, 0.0, 0.0 };
      double cmean_count = 0.0;
      for(size_t i = 0; i < region_pixels; i++)
        if(!hole[i] && lum_accum[i] >= lum_min)
        {
          const float inv_lum = 1.f / fmaxf(lum_accum[i], epsilon);
          cmean_accum[0] += (double)(estimate[i * 4 + 0] * inv_lum);
          cmean_accum[1] += (double)(estimate[i * 4 + 1] * inv_lum);
          cmean_accum[2] += (double)(estimate[i * 4 + 2] * inv_lum);
          cmean_count += 1.0;
        }
      if(cmean_count > 0.0)
      {
        for(int c = 0; c < 3; c++) cmean[c] = (float)(cmean_accum[c] / cmean_count);
        // Trusted-ring vote on the flat-mean prior: the surround-importing refinements (this
        // pull + the decoupled recombine below) only engage where the 1-clip ring confirms the
        // region's mean colour describes the blown core (white lamps, uniform skies); they stand
        // down on self-coloured emitters and gradient skies, leaving the joint floors.
        refine_gate = floor_gate * _hl_ring_flat_mean_vote(estimate, valid, cmean, region_pixels);
        cmean_beta = 0.5f * refine_gate;
      }
    }

    for(int c = 0; c < 3; c++)
    {
      HL_PFOR()
      for(size_t i = 0; i < region_pixels; i++)
        flat_target[i] = estimate[i * 4 + c] / fmaxf(lum_accum[i], epsilon); // ratio r_c = est_c / L_sum

      _cf_harmonic_fill(flat_target, hole, region_w, region_h, cf_base, NULL, pipe); // harmonic (Delta r = 0)

      HL_PFOR()
      for(size_t i = 0; i < region_pixels; i++)
      {
        float ratio = fmaxf(flat_target[i], 0.f);
        if(hole[i] && cmean_beta > 0.f) ratio = (1.f - cmean_beta) * ratio + cmean_beta * cmean[c];
        plane1[i * 4 + c] = ratio;
      }
    }

    // recombine dome_c = L_dome * (r_c / sum r) and blend it into the estimate by the depth-gated
    // KEEP weight conf_weight = Wc^2 (= 1 - dome_fraction of step 6): est = keep*est + (1-keep)*dome.
    // A pixel with no surviving guide takes the dome outright (the all-clip core rebuilds it just after).
    //
    // CHROMA-DECOUPLED variant (blended by the clip-asymmetry gate): the per-channel blend lets the
    // colour-line fit's biased chromaticity survive on multi-clip pixels; decoupling keeps the fit's
    // per-channel LUMINANCE (keep = Wc^2) but reprojects the clipped SUBSET onto the dome's
    // chromaticity. At gate 0 the per-channel blend runs verbatim (approved behavior).
    HL_PFOR()
    for(size_t i = 0; i < region_pixels; i++)
    {
      if(!hole[i]) continue;

      const float caccum = fmaxf(plane1[i * 4 + 0] + plane1[i * 4 + 1] + plane1[i * 4 + 2], epsilon); // sum r
      const int anyvalid = (valid[i * 4 + 0] >= 0.5f) || (valid[i * 4 + 1] >= 0.5f) || (valid[i * 4 + 2] >= 0.5f);

      float blended_sub = 0.f, dome_sub = 0.f;
      // the approved per-channel depth-gated blend (keep*est + (1-keep)*dome), kept verbatim as
      // the gate-0 path and as one leg of the gated chroma-decoupled blend below;
      // only clipped channels are written AND read; the init quiets -Wmaybe-uninitialized
      float per_channel_blend[3] = { 0.f, 0.f, 0.f };
      for(int c = 0; c < 3; c++)
        if(valid[i * 4 + c] < 0.5f)
        {
          const float dome = dome_lum[i] * (plane1[i * 4 + c] / caccum); // dome_c = L_dome * chroma share
          const float conf_weight = valid_variance[i * 4 + c] * valid_variance[i * 4 + c]; // keep = Wc^2
          per_channel_blend[c] = anyvalid ? (conf_weight * estimate[i * 4 + c] + (1.f - conf_weight) * dome) : dome;
          blended_sub += per_channel_blend[c];
          dome_sub += dome;
        }
      for(int c = 0; c < 3; c++)
        if(valid[i * 4 + c] < 0.5f)
        {
          if(refine_gate <= 1e-6f || !anyvalid || dome_sub <= epsilon)
          {
            estimate[i * 4 + c] = per_channel_blend[c]; // bit-exact approved path
            continue;
          }
          const float decoupled = blended_sub * (dome_lum[i] * (plane1[i * 4 + c] / caccum) / dome_sub);
          estimate[i * 4 + c] = refine_gate * decoupled + (1.f - refine_gate) * per_channel_blend[c];
        }
    }

    // Re-assert the saturation floor AFTER the self dome (the prototype floors here): the dome only
    // continues the valid rim, it does not know about saturation, so it can undershoot a clipped
    // channel below its clip level. JOINT form blended by the clip-asymmetry gate (one scalar lift
    // of the clipped subset preserves the reconstruction's chromaticity; per-channel at gate 0).
    __OMP_PARALLEL_FOR__()
    for(size_t i = 0; i < region_pixels; i++)
    {
      float lift = 1.f;
      if(floor_gate > 1e-6f)
        for(int c = 0; c < 3; c++)
          if(valid[i * 4 + c] < 0.5f)
          {
            const float e = fmaxf(estimate[i * 4 + c], 1e-6f);
            lift = fmaxf(lift, fminf(fmaxf(e, clip0[i * 4 + c]) / e, 8.f));
          }
      for(int c = 0; c < 3; c++)
        if(valid[i * 4 + c] < 0.5f)
        {
          const float per_chan = fmaxf(estimate[i * 4 + c], clip0[i * 4 + c]);
          if(floor_gate <= 1e-6f)
          {
            estimate[i * 4 + c] = per_chan; // bit-exact approved path
            continue;
          }
          const float joint
              = fmaxf(fmaxf(estimate[i * 4 + c], 1e-6f) * lift, clip0[i * 4 + c]);
          estimate[i * 4 + c] = floor_gate * joint + (1.f - floor_gate) * per_chan;
        }
    }
  }
}

__DT_CLONE_TARGETS__
void _joint_core(_hl_region_ctx_t *const ctx)
{
  const _hl_region_t *const region = ctx->region;
  const dt_dev_pixelpipe_t *const pipe = ctx->pipe;
  const int region_w = ctx->region_w;
  const int region_h = ctx->region_h;
  const size_t region_pixels = ctx->region_pixels;
  const float epsilon = ctx->epsilon;
  const int max_cg_iter = ctx->max_cg_iter;
  const float solid_color = ctx->solid_color;
  float *const restrict estimate = ctx->estimate;
  float *const restrict valid = ctx->valid;
  float *const restrict plane1 = ctx->plane1;
  float *const restrict clip0 = ctx->clip0;
  uint8_t *const restrict hole = ctx->hole;
  float *const restrict solver_field = ctx->solver_field;
  float *const restrict dome_lum = ctx->dome_lum;
  float *const restrict lum_accum = ctx->lum_accum;
  float *const restrict reaction_weight = ctx->reaction_weight;
  float *const restrict flat_target = ctx->flat_target;
  float *const restrict cg_residual = ctx->cg_residual;
  float *const restrict cg_dir = ctx->cg_dir;
  float *const restrict cg_operator = ctx->cg_operator;
  float *const restrict cg_tmp1 = ctx->cg_tmp1;
  float *const restrict cg_tmp2 = ctx->cg_tmp2;

  // --- all-clipped core: shared biharmonic luminance dome x diffused chromaticity ---
  // Only pixels with NO surviving channel. Extending this to 2-clip pixels was tried and reverted:
  // the bright sky is itself 2-clip (R,G clipped, B not), so it got swept into the coupled core
  // and filled with diffused magenta chroma that bled into the sky. 2-clip pixels keep their
  // (two-or-one-guide) guided/self-dome estimate; only the truly guide-less core is rebuilt here.
  //
  // MATHS BRIDGE -- Step 7 all-clip core (article §"Filling holes with no survivor", §"The
  // algorithm" step 7). Magnitude and chrominance are split and reconstructed by different
  // operators: ONE shared biharmonic luminance dome L_dome (Delta^2 L_sum = 0, E_bihar) for the
  // magnitude common to all three channels, and the screened-Poisson rim-diffused chrominance
  // r = RGB/L_sum ((lambda*I-Delta) r = lambda_solid*r_target, E_chrominance) carried inward from
  // the reconstructed annulus. Recombination core_c = L_dome * (r_c / sum_j r_j), then a feathered
  // blurred hand-over into the surrounding coefficient-field reconstruction (no hard core rim).
  int has_allc = 0;
  __OMP_PARALLEL_FOR__(reduction(| : has_allc))
  for(size_t i = 0; i < region_pixels; i++)
  {
    hole[i] = (valid[i * 4 + 0] < 0.5f && valid[i * 4 + 1] < 0.5f && valid[i * 4 + 2] < 0.5f);
    if(hole[i]) has_allc = 1;
  }

  if(has_allc)
  {
    // one shared luminance dome (biharmonic) from the reconstructed annulus rim
    // L_sum = R + G + B (the summed luminance, the magnitude shared by all three channels)
    __OMP_PARALLEL_FOR__()
    for(size_t i = 0; i < region_pixels; i++)
    {
      lum_accum[i] = estimate[i * 4 + 0] + estimate[i * 4 + 1] + estimate[i * 4 + 2];
      solver_field[i] = lum_accum[i];
    }

    // Delta^2 L_sum = 0 on the core, L_sum|dOmega = L_valid on the reconstructed annulus rim:
    // E_bihar magnitude dome (one scalar solve, not three, so no channel collapses off-hue)
    _biharmonic_dome(solver_field, hole, region_w, region_h, 0,
                     pipe); // shared biharmonic luminance dome (auto ds)
    memcpy(dome_lum, solver_field, region_pixels * sizeof(float));

    // The all-clip core has EVERY channel saturated, so its luminance is at least the accum of the
    // clip levels -- the brightest, not something to extrapolate downward. The biharmonic dome can
    // dip below that (the floored rim has no upward gradient to continue), which darkens the centre
    // below the annulus. Floor the dome at the saturated accum so the core is never darker than "all
    // channels at clip". Above-clip doming is kept where the dome exceeds it.
    __OMP_PARALLEL_FOR__()
    for(size_t i = 0; i < region_pixels; i++)
      if(hole[i])
      {
        // saturation floor on the dome: L_dome >= sum_c clip0_c ("all three channels at clip",
        // the brightest the core can be); monotone, so it never dims a valid rim or shifts hue
        const float lsat = clip0[i * 4 + 0] + clip0[i * 4 + 1] + clip0[i * 4 + 2];
        dome_lum[i] = fmaxf(dome_lum[i], lsat);
      }

    // mean valid chromaticity -> flat target for the "inpaint a flat color" slider
    // r_target = <RGB/L_sum> over fully-valid pixels: the screened-Poisson reaction pulls the
    // core chroma toward this flat colour (article's bar-c_c, the mean valid chromaticity)
    // accumulate in DOUBLE: a float running accum of ~1e5 terms carries an ULP of ~4e-3 per
    // add near its final magnitude, which biased the mean by ~1e-4 relative (enough to show
    // as a 4e-4 CPU-vs-GPU divergence on the reaction target)
    dt_aligned_pixel_t cmean = { 0.f, 0.f, 0.f, 0.f };
    double cacc[3] = { 0.0, 0.0, 0.0 };
    double count = 0.0;
    for(size_t i = 0; i < region_pixels; i++)
    {
      if(!(valid[i * 4 + 0] >= 0.5f && valid[i * 4 + 1] >= 0.5f && valid[i * 4 + 2] >= 0.5f)) continue;
      const float invL = 1.f / fmaxf(lum_accum[i], epsilon);
      cacc[0] += (double)(estimate[i * 4 + 0] * invL);
      cacc[1] += (double)(estimate[i * 4 + 1] * invL);
      cacc[2] += (double)(estimate[i * 4 + 2] * invL);
      count += 1.0;
    }
    if(count > 0.0)
      for(int c = 0; c < 3; c++) cmean[c] = (float)(cacc[c] / count);

    // Re-hue the all-clip core's saturation floor toward the mean valid chromaticity, blended by
    // the clip-asymmetry gate (see _hl_floor_gate). With WB'd clips, clip0's own chromaticity is
    // the inverse-WB magenta -- it is a magnitude floor, not a colour -- yet the downstream aniso
    // stage uses it as its ratio-space obstacle and reassembly floor, pinning the core to neutral
    // raw. Redistributing clip0 to cmean preserves the magnitude sum_c clip0_c (cmean sums to 1 by
    // construction) while the obstacle/floor now enforces the surround chromaticity. At gate 0
    // clip0 is untouched (approved behavior on equal clips).
    // BRIGHT surround mean for the rehue + its vote (separate from `cmean`, which feeds the
    // APPROVED screened-Poisson seed and must stay the all-valid mean at any gate): dark
    // foreground contaminates the all-valid mean and closes the vote on exactly the scenes the
    // rehue is for (measured on MAC/sunrise).
    dt_aligned_pixel_t cmean_bright = { 0.f, 0.f, 0.f, 0.f };
    double bright_count = 0.0;
    if(ctx->floor_gate > 1e-6f)
    {
      double plateau_sum = 0.0, plateau_count = 0.0;
      for(size_t i = 0; i < region_pixels; i++)
        if(valid[i * 4 + 0] < 0.5f || valid[i * 4 + 1] < 0.5f || valid[i * 4 + 2] < 0.5f)
        {
          plateau_sum += (double)lum_accum[i];
          plateau_count += 1.0;
        }
      const float lum_min = (plateau_count > 0.0) ? 0.35f * (float)(plateau_sum / plateau_count) : 0.f;
      double bright_accum[3] = { 0.0, 0.0, 0.0 };
      for(size_t i = 0; i < region_pixels; i++)
      {
        if(!(valid[i * 4 + 0] >= 0.5f && valid[i * 4 + 1] >= 0.5f && valid[i * 4 + 2] >= 0.5f)) continue;
        if(lum_accum[i] < lum_min) continue;
        const float invL = 1.f / fmaxf(lum_accum[i], epsilon);
        bright_accum[0] += (double)(estimate[i * 4 + 0] * invL);
        bright_accum[1] += (double)(estimate[i * 4 + 1] * invL);
        bright_accum[2] += (double)(estimate[i * 4 + 2] * invL);
        bright_count += 1.0;
      }
      if(bright_count > 0.0)
        for(int c = 0; c < 3; c++) cmean_bright[c] = (float)(bright_accum[c] / bright_count);
    }
    const float rehue_gate
        = (bright_count > 0.0 && ctx->floor_gate > 1e-6f)
              ? ctx->floor_gate * _hl_ring_flat_mean_vote(estimate, valid, cmean_bright, region_pixels)
              : 0.f; // trusted-ring vote: rehue only where the bright-surround prior holds
    if(rehue_gate > 1e-6f)
    {
      __OMP_PARALLEL_FOR__()
      for(size_t i = 0; i < region_pixels; i++)
        if(hole[i])
        {
          const float lsat = clip0[i * 4 + 0] + clip0[i * 4 + 1] + clip0[i * 4 + 2];
          for(int c = 0; c < 3; c++)
            clip0[i * 4 + c] = rehue_gate * (lsat * cmean_bright[c]) + (1.f - rehue_gate) * clip0[i * 4 + c];
        }
    }

    // chromaticity: harmonic diffusion from the rim, with a screened-Poisson reaction
    // pulling the core hue toward the flat mean by solid_color ("inpaint a flat color").
    // react = lambda_solid = solid_color^2 * 4: the screening strength; 0 -> pure harmonic
    // (Delta r = 0), larger -> a flatter, more uniform "solid colour" fill
    const float react = solid_color * solid_color * 4.f;
    __OMP_PARALLEL_FOR__()
    for(size_t i = 0; i < region_pixels; i++) reaction_weight[i] = react;

    // factor A = lambda_solid*I - Delta (order 1) ONCE; it serves the three channels (same matrix,
    // three right-hand sides) -- the direct solve is EXACT where the float CG stopped at a tolerance
    int *sp_pgrid = NULL;
    int sp_nh = 0;
    _sp_chol_t *sp_S = _sp_pde_factor(hole, (react > 0.f) ? reaction_weight : NULL, 1, 1.f, region_w, region_h,
                                      &sp_pgrid, &sp_nh, pipe);
    double *sp_b = sp_S ? (double *)dt_pixelpipe_cache_alloc_align(sizeof(double) * sp_nh, ctx->pipe) : NULL;
    if(sp_S && !sp_b)
    {
      _sp_chol_free(sp_S);
      sp_S = NULL;
    }

    for(int c = 0; c < 3; c++)
    {
      __OMP_PARALLEL_FOR__()
      for(size_t i = 0; i < region_pixels; i++)
      {
        // boundary (Dirichlet) = the real rim chroma r_valid = est_c/L_sum; hole initial guess =
        // the mean valid (amber) chroma r_target, so an under-converged core centre biases to
        // amber, never to the guided magenta
        solver_field[i] = hole[i] ? cmean[c] : (estimate[i * 4 + c] / fmaxf(lum_accum[i], epsilon));
        flat_target[i] = cmean[c]; // r_target plane for the screening reaction term
      }

      // solve (lambda_solid*I - Delta) r_c = lambda_solid*r_target on the hole, r_c|dOmega = r_valid
      if(sp_S)
        _sp_pde_solve(sp_S, sp_pgrid, solver_field, hole, (react > 0.f) ? reaction_weight : NULL,
                      (react > 0.f) ? flat_target : NULL, NULL, 1, 1.f, region_w, region_h, sp_b, cg_tmp1, cg_tmp2,
                      cg_residual);
      else
        _region_pde_solve(solver_field, hole, (react > 0.f) ? reaction_weight : NULL,
                          (react > 0.f) ? flat_target : NULL, NULL, 1, 1.f, region_w, region_h, cg_residual,
                          cg_dir, cg_operator, cg_tmp1, cg_tmp2, max_cg_iter);

      __OMP_PARALLEL_FOR__()
      for(size_t i = 0; i < region_pixels; i++) plane1[i * 4 + c] = fmaxf(solver_field[i], 0.f);
    }

    _sp_chol_free(sp_S);
    dt_pixelpipe_cache_free_align(sp_pgrid);
    dt_pixelpipe_cache_free_align(sp_b);

    // FEATHERED composite: a hard all-clip mask makes the core <-> annulus hand-off a seam by
    // construction. The dome (ldb ~ lsb outside the hole) and the diffused chroma (s1 = real
    // ratios outside) are both valid past the hole boundary, so blending them in over a
    // blurred mask is continuous in space at no cost to the core rebuild itself.
    // core mask -> 1 inside, 0 outside; blurred into a smooth feather weight (the one smooth
    // weight in the method: it blends two RECONSTRUCTIONS, never reclassifies measurements)
    __OMP_PARALLEL_FOR__()
    for(size_t i = 0; i < region_pixels; i++) solver_field[i] = hole[i] ? 1.f : 0.f;

    _knee_blur(solver_field, reaction_weight, region_w, region_h,
               fmaxf(4.f, CLAMP(region->radius / 6.f, 8.f, 64.f) / 4.f));

    __OMP_PARALLEL_FOR__()
    for(size_t i = 0; i < region_pixels; i++)
    {
      const float fit_weight = CLAMP(reaction_weight[i], 0.f, 1.f); // feather alpha (blurred core mask)
      const float caccum = fmaxf(plane1[i * 4 + 0] + plane1[i * 4 + 1] + plane1[i * 4 + 2], epsilon); // sum_j r_j

      if(hole[i])
      {
        // interior: core rebuild, full strength: core_c = L_dome * (r_c / sum_j r_j) (RGB = L*r)
        for(int c = 0; c < 3; c++) estimate[i * 4 + c] = dome_lum[i] * (plane1[i * 4 + c] / caccum);
      }
      else if(fit_weight > 1e-4f)
      {
        // feather ring outside the core: alpha*core_c + (1-alpha)*est, on CLIPPED channels of
        // the surrounding reconstruction only -- valid data is never touched
        for(int c = 0; c < 3; c++)
          if(valid[i * 4 + c] < 0.5f)
            estimate[i * 4 + c] = fit_weight * dome_lum[i] * (plane1[i * 4 + c] / caccum)
                                  + (1.f - fit_weight) * estimate[i * 4 + c];
      }
    }
  }
}

// Chromaticity-gradient continuation (see core.h). Runs LAST in the region chain (after
// _aniso_chroma), so it reprojects the final reconstructed values; the group-B scratch (solver_field, hole) is reused freely.
void _chromaticity_gradient(_hl_region_ctx_t *const ctx)
{
  const dt_dev_pixelpipe_t *const pipe = ctx->pipe;
  const int region_w = ctx->region_w;
  const int region_h = ctx->region_h;
  const size_t region_pixels = ctx->region_pixels;
  const float epsilon = ctx->epsilon;
  float *const restrict estimate = ctx->estimate;
  float *const restrict valid = ctx->valid;
  float *const restrict clip0 = ctx->clip0;
  float *const restrict plane2 = ctx->plane2;       // extended chroma-share planes (4-ch layout)
  float *const restrict solver_field = ctx->solver_field; // per-channel dome scratch
  uint8_t *const restrict hole = ctx->hole;         // rewritten: the field's extension domain
  float *const restrict gate_src = ctx->cg_tmp1;    // group-B scratch: agreement-weight source
  float *const restrict gate_msk = ctx->cg_tmp2;    // group-B scratch: agreement-weight mass
  float *const restrict gate_wgt = ctx->cg_residual; // group-B scratch: diffused agreement weight
  float *const restrict gate_nrm = ctx->cg_dir;      // group-B scratch: diffused mass

  // --- 1. anchors: fully-valid AND bright (>= 35% of the blown zone's plateau luminance) ---
  // The extension must be anchored on the sky/emitter material only: dark valid content (occluder
  // silhouettes, foreground) carries near-neutral noise chroma, and the fence band right at the clip
  // contour is unrepresentative -- the dome's gradient continuation from the BRIGHT surround sails
  // over both. Plateau proxy = mean current luminance over the any-clip pixels.
  double plateau_accum = 0.0;
  size_t plateau_count = 0;
  for(size_t i = 0; i < region_pixels; i++)
    if(valid[i * 4 + 0] < 0.5f || valid[i * 4 + 1] < 0.5f || valid[i * 4 + 2] < 0.5f)
    {
      plateau_accum += (double)(estimate[i * 4 + 0] + estimate[i * 4 + 1] + estimate[i * 4 + 2]);
      plateau_count++;
    }
  if(plateau_count == 0) return; // nothing blown in this region
  const float lum_anchor_min = 0.35f * (float)(plateau_accum / (double)plateau_count);

  // Guard band: the last valid band hugging the clip contour is itself unrepresentative (sensor
  // rolloff, flare/scatter whitening -- measured bluer than the wider sky), so anchors must stand
  // clear of it. Blur the any-clip mask into a proximity field and require anchors to be far enough
  // that the blurred mask has decayed (~2-3 sigma from any clipped pixel).
  float *const restrict guard_src = ctx->flat_target;      // group-B scratch, free at this point
  float *const restrict guard_blur = ctx->reaction_weight; // group-B scratch, free at this point
  HL_PFOR()
  for(size_t i = 0; i < region_pixels; i++)
    guard_src[i]
        = (valid[i * 4 + 0] < 0.5f || valid[i * 4 + 1] < 0.5f || valid[i * 4 + 2] < 0.5f) ? 1.f : 0.f;
  // thin ring only: a wide moat exiles the anchors to unrepresentative far content (measured: the
  // field then inherited the wrong quadrant's hue) -- the fence is a few pixels of rolloff, not tens
  const float guard_sigma = 4.f;
  _knee_blur(guard_src, guard_blur, region_w, region_h, guard_sigma);

  size_t n_anchor = 0;
  HL_PFOR(reduction(+ : n_anchor))
  for(size_t i = 0; i < region_pixels; i++)
  {
    const int fully_valid
        = (valid[i * 4 + 0] >= 0.5f) && (valid[i * 4 + 1] >= 0.5f) && (valid[i * 4 + 2] >= 0.5f);
    const float lum = estimate[i * 4 + 0] + estimate[i * 4 + 1] + estimate[i * 4 + 2];
    const int anchor = fully_valid && (lum >= lum_anchor_min) && (guard_blur[i] < 0.05f);
    hole[i] = !anchor;
    n_anchor += anchor;
  }
  // no usable bright surround (e.g. an emitter in darkness): the fence chromaticity is all there is
  if(n_anchor < 64 || n_anchor < region_pixels / 256) return;

  // --- 2. per channel: extend the anchor chroma share biharmonically over everything else ---
  for(int c = 0; c < 3; c++)
  {
    HL_PFOR()
    for(size_t i = 0; i < region_pixels; i++)
    {
      const float lum = estimate[i * 4 + 0] + estimate[i * 4 + 1] + estimate[i * 4 + 2];
      solver_field[i] = estimate[i * 4 + c] / fmaxf(lum, epsilon); // share everywhere (hole = init guess)
    }
    _biharmonic_dome(solver_field, hole, region_w, region_h, 0, pipe); // gradient-extending (auto ds)
    HL_PFOR()
    for(size_t i = 0; i < region_pixels; i++)
      plane2[i * 4 + c] = CLAMP(solver_field[i], 0.f, 1.f); // shares are bounded; clamp dome overshoot
  }

  // --- 3. CONTENT GATE: validate the extended field against the 1-clip annulus ---
  // The chromaticity-continuation prior is a scene assumption: true for gradient skies (the
  // measured sunrise win), false for self-coloured emitters whose core carries its OWN
  // chromaticity (a magenta sun, coloured lamps -- the bench regressions). The method's own
  // trusted zone arbitrates: 1-clip pixels are reconstructed from TWO measured guides
  // (measured chromaticity-correct on every test image), and they ring every deeper zone. Where
  // the extended field agrees with the 1-clip ring, the surround genuinely continues inward ->
  // apply the reprojection; where it disagrees, the blown object is self-coloured -> keep the
  // solver. The per-pixel agreement weight is diffused inward from the ring by normalized
  // convolution, the same trick as every other hand-off in this module (no printable level set).
  const float gate_tau = 0.10f; // L1 share-difference tolerance (~one just-noticeable hue step)
  HL_PFOR()
  for(size_t i = 0; i < region_pixels; i++)
  {
    const int n_clip = (valid[i * 4 + 0] < 0.5f) + (valid[i * 4 + 1] < 0.5f) + (valid[i * 4 + 2] < 0.5f);
    float weight_src = 0.f, mask_src = 0.f;
    // floor-authored 1-clip pixels are NOT evidence: the fit landed at/below the pixel's own
    // saturation floor (measured on the flare-veiled sunrise: 92.5% of the 1-clip zone, mean fit
    // lift +0.9%), so their chromaticity is the floor's, not the solver's -- the ring votes only
    // on 1-clip pixels whose fit genuinely spoke, and the authored ones get reprojected below.
    int floor_authored = 0;
    if(n_clip == 1 && ctx->floor_gate > 1e-6f) // WB'd clips only: at unit WB the floor imprint is
    {                                          // neutral ~ truth (approved bench behavior)
      const int cc = (valid[i * 4 + 0] < 0.5f) ? 0 : ((valid[i * 4 + 1] < 0.5f) ? 1 : 2);
      floor_authored = estimate[i * 4 + cc] <= 1.03f * fmaxf(clip0[i * 4 + cc], 1e-9f);
    }
    if(n_clip == 1 && !floor_authored)
    {
      const float lum = fmaxf(estimate[i * 4 + 0] + estimate[i * 4 + 1] + estimate[i * 4 + 2], epsilon);
      const float share_sum = fmaxf(plane2[i * 4 + 0] + plane2[i * 4 + 1] + plane2[i * 4 + 2], epsilon);
      float err = 0.f;
      for(int c = 0; c < 3; c++)
        err += fabsf(plane2[i * 4 + c] / share_sum - estimate[i * 4 + c] / lum);
      const float t = err / gate_tau;
      weight_src = expf(-t * t);
      mask_src = 1.f;
    }
    gate_src[i] = weight_src;
    gate_msk[i] = mask_src;
  }
  const float gate_sigma = CLAMP(ctx->region->radius / 4.f, 8.f, 96.f);
  _knee_blur(gate_src, gate_wgt, region_w, region_h, gate_sigma);
  _knee_blur(gate_msk, gate_nrm, region_w, region_h, gate_sigma);

  // region-level ring vote: deep interiors of large blown zones sit beyond the blur's reach of the
  // thin 1-clip ring; there the ring's GLOBAL agreement decides (a gradient sky's ring votes yes
  // everywhere, a self-coloured emitter's ring votes no). The per-pixel weight shrinks toward the
  // vote as local evidence mass vanishes: w = (blur_w + lambda*vote) / (blur_m + lambda).
  double vote_wsum = 0.0, vote_msum = 0.0;
  for(size_t i = 0; i < region_pixels; i++)
  {
    vote_wsum += (double)gate_src[i];
    vote_msum += (double)gate_msk[i];
  }
  const float gate_vote = (vote_msum > 0.0) ? (float)(vote_wsum / vote_msum) : 0.f;

  // --- 4. reproject the multi-clip subsets onto the extended field, blended by the gate ---
  HL_PFOR()
  for(size_t i = 0; i < region_pixels; i++)
  {
    const int clip_r = valid[i * 4 + 0] < 0.5f;
    const int clip_g = valid[i * 4 + 1] < 0.5f;
    const int clip_b = valid[i * 4 + 2] < 0.5f;
    const int n_clip = clip_r + clip_g + clip_b;
    if(n_clip < 2) continue; // 1-clip pixels: measured-correct where the fit spoke; the
                             // floor-authored ones are handled by PASS 2 below (core-anchored field)

    // diffused agreement weight, shrunk toward the region-level ring vote as local evidence thins
    const float gate_lambda = 0.05f;
    const float gate_w
        = CLAMP((gate_wgt[i] + gate_lambda * gate_vote) / (gate_nrm[i] + gate_lambda), 0.f, 1.f);
    if(gate_w <= 1e-4f) continue;

    const float share_sum = fmaxf(plane2[i * 4 + 0] + plane2[i * 4 + 1] + plane2[i * 4 + 2], epsilon);
    const int anyvalid = !(clip_r && clip_g && clip_b);

    if(anyvalid)
    {
      // partial multi-clip: the SURVIVING channels anchor the brightness against the field
      // (scale = sum valid est / sum valid shares), the clipped channels take the field's shares
      // outright -- the pixel lands exactly on the extended hue, and the measured data is honored.
      // (Magnitude-preserving redistribution was tried first and cannot fix the hue here: the
      // clipped subset's total IS the under-prediction the fence-hue fits produced.)
      float sv_est = 0.f, sv_share = 0.f;
      for(int c = 0; c < 3; c++)
        if(valid[i * 4 + c] >= 0.5f)
        {
          sv_est += estimate[i * 4 + c];
          sv_share += plane2[i * 4 + c] / share_sum;
        }
      if(sv_share <= epsilon || sv_est <= epsilon) continue;
      // survivor-anchored scale, bounded: a tiny surviving share amplifies measurement noise into
      // arbitrarily bright reprojections; cap the implied pixel magnitude at 4x its current one
      const float scale
          = fminf(sv_est / sv_share, 4.f * (estimate[i * 4 + 0] + estimate[i * 4 + 1] + estimate[i * 4 + 2]));
      for(int c = 0; c < 3; c++)
        if(valid[i * 4 + c] < 0.5f)
          estimate[i * 4 + c]
              = gate_w * (scale * (plane2[i * 4 + c] / share_sum)) + (1.f - gate_w) * estimate[i * 4 + c];
    }
    else
    {
      // all-clip pixels are NOT reprojected: with no measured anchor the only magnitude authority is
      // the joint core's dome, and redistributing ITS total by the field's shares is unstable when the
      // core estimate is poor (measured on the gradsky bench case: single blown channels far above the
      // others turn a hue reprojection into a large radiance error, down to negative pixels). The core
      // keeps the joint-core/aniso result; the continuation prior only refines pixels that still hold
      // at least one measurement.
      continue;
    }

    // joint saturation floor re-assert (scalar-subset lift + per-channel safety, hue preserved)
    float lift = 1.f;
    for(int c = 0; c < 3; c++)
      if(valid[i * 4 + c] < 0.5f)
      {
        const float e = fmaxf(estimate[i * 4 + c], 1e-6f);
        lift = fmaxf(lift, fminf(fmaxf(e, clip0[i * 4 + c]) / e, 8.f));
      }
    for(int c = 0; c < 3; c++)
      if(valid[i * 4 + c] < 0.5f)
      {
        if(lift > 1.f) estimate[i * 4 + c] = fmaxf(estimate[i * 4 + c], 1e-6f) * lift;
        estimate[i * 4 + c] = fmaxf(estimate[i * 4 + c], clip0[i * 4 + c]);
      }
  }

  // --- 5. PASS 2 = a3, VALUE continuation of the floor-authored 1-clip band (WB'd clips only).
  // The share-based reprojections cannot help here and the floor rightly vetoes them (measured
  // twice: the sunrise band, and the P1000388 blue LED array where the artifact is a B-value
  // DISCONTINUITY across the G-clip contour -- B lifted above clip inside the 2-clip core, B
  // pinned AT clip in the floor-authored collar, the jagged contour printed as a seam). The
  // clipped channel's own VALUE is instead extended biharmonically over the authored band,
  // anchored on BOTH sides -- the multi-clip reconstruction inside (lifted) and the measured
  // data outside (just under clip) -- and floored at saturation. Writes only the clipped
  // channel; approaches clip0 at the outer contour by construction (no seam); the same
  // operator the luminance dome already trusts.
  if(ctx->floor_gate > 1e-6f)
  {
    for(int c = 0; c < 3; c++)
    {
      size_t n_hole_c = 0;
      HL_PFOR(reduction(+ : n_hole_c))
      for(size_t i = 0; i < region_pixels; i++)
      {
        const int clip_r = valid[i * 4 + 0] < 0.5f;
        const int clip_g = valid[i * 4 + 1] < 0.5f;
        const int clip_b = valid[i * 4 + 2] < 0.5f;
        const int cc = clip_r ? 0 : (clip_g ? 1 : 2);
        const int is_hole = (clip_r + clip_g + clip_b == 1) && (cc == c)
                            && (estimate[i * 4 + c] <= 1.03f * fmaxf(clip0[i * 4 + c], 1e-9f));
        hole[i] = is_hole;
        solver_field[i] = estimate[i * 4 + c];
        n_hole_c += is_hole;
      }
      if(n_hole_c == 0) continue;
      _biharmonic_dome(solver_field, hole, region_w, region_h, 0, pipe);
      HL_PFOR()
      for(size_t i = 0; i < region_pixels; i++)
        if(hole[i]) estimate[i * 4 + c] = fmaxf(solver_field[i], clip0[i * 4 + c]);
    }
  }

}

// ============================ OpenCL ============================

#if defined(HAVE_OPENCL) && DT_HL_SPARSE_SOLVE
cl_int _selfdome_stage_cl(const int devid, void *gd_void, cl_mem estimate, cl_mem valid, cl_mem model_quality,
                          cl_mem clip0, cl_mem depth, const int region_w, const int region_h, const float cf_sigma,
                          const float reg_radius, const int ds_shared, const float floor_gate,
                          const dt_dev_pixelpipe_t *pipe)
{
  dt_iop_highlights_global_data_t *global_data = (dt_iop_highlights_global_data_t *)gd_void;
  const size_t region_pixels = (size_t)region_w * region_h;
  cl_int cl_err = DT_OPENCL_DEFAULT_ERROR;
  size_t size[3] = { ROUNDUPDWD(region_w, devid), ROUNDUPDHT(region_h, devid), 1 };
  const float epsilon = 1e-6f;

  cl_mem luminance = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem hole = dt_opencl_alloc_device_buffer(devid, region_pixels);
  cl_mem dome_lum = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem ratio0 = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem ratio1 = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem ratio2 = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem ratios[3];
  ratios[0] = ratio0;
  ratios[1] = ratio1;
  ratios[2] = ratio2;
  cl_mem partial_sums = NULL; // mean-chromaticity partial sums (gate > 0 only)
  if(!luminance || !hole || !dome_lum || !ratio0 || !ratio1 || !ratio2) goto out;

  // soft floor first (production order: floor -> dome gate -> self dome)
  {
    const int kernel = global_data->kernel_hl_soft_floor;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &clip0);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(float), &floor_gate);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
    if(cl_err != CL_SUCCESS) goto out;
  }

  // brightness plane (sum of the three channels) + union hole mask (any clipped channel)
  {
    const int kernel = global_data->kernel_hl_lsb_hole;
    const int allmode = 0; // union hole: ANY clipped channel
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &luminance);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &hole);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &allmode);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
    if(cl_err != CL_SUCCESS) goto out;
  }

  // Mean valid chromaticity (gate > 0 only): the flat target the hole-interior dome chroma is
  // pulled toward, mirroring the CPU _selfdome cmean pull (fully-valid == !hole here, so the
  // joint-core reduction kernel serves unchanged). ratio_beta stays 0 when no valid pixel exists.
  float cmean[3] = { 0.f, 0.f, 0.f };
  float ratio_beta = 0.f;
  float refine_gate = 0.f; // floor_gate x trusted-ring vote (set with the cmean below)
  if(floor_gate > 1e-6f)
  {
    const int local_size = 64, n_groups = 256;
    const int n_pixels = (int)region_pixels;
    partial_sums = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 8 * n_groups);
    if(!partial_sums)
    {
      cl_err = DT_OPENCL_DEFAULT_ERROR;
      goto out;
    }
    size_t sizes[3] = { (size_t)n_groups * local_size, 1, 1 };
    size_t local[3] = { local_size, 1, 1 };
    float partial_host[8 * 256];

    // blown-zone plateau -> bright-valid gate for the refinements' surround mean: the
    // whole-window mean is contaminated by dark, unrelated content (the cgrad anchors learned
    // the same lesson) -- measured on MAC/sunrise, the ring vote against the all-valid mean
    // stays closed on exactly the scenes the refinements are for.
    float lum_min = 0.f;
    {
      const int plateau_kernel = global_data->kernel_hl_cgrad_plateau;
      dt_opencl_set_kernel_arg(devid, plateau_kernel, 0, sizeof(cl_mem), &estimate);
      dt_opencl_set_kernel_arg(devid, plateau_kernel, 1, sizeof(cl_mem), &valid);
      dt_opencl_set_kernel_arg(devid, plateau_kernel, 2, sizeof(cl_mem), &partial_sums);
      dt_opencl_set_kernel_arg(devid, plateau_kernel, 3, sizeof(int), &n_pixels);
      dt_opencl_set_kernel_arg(devid, plateau_kernel, 4, sizeof(float) * 2 * local_size, NULL);
      cl_err = dt_opencl_enqueue_kernel_2d_with_local(devid, plateau_kernel, sizes, local);
      if(cl_err != CL_SUCCESS) goto out;
      cl_err = dt_opencl_read_buffer_from_device(devid, partial_host, partial_sums, 0,
                                                 sizeof(float) * 2 * n_groups, CL_TRUE);
      if(cl_err != CL_SUCCESS) goto out;
      double plateau_sum = 0.0, plateau_count = 0.0;
      for(int group = 0; group < n_groups; group++)
      {
        plateau_sum += (double)partial_host[group * 2 + 0];
        plateau_count += (double)partial_host[group * 2 + 1];
      }
      if(plateau_count > 0.0) lum_min = 0.35f * (float)(plateau_sum / plateau_count);
    }

    const int kernel = global_data->kernel_hl_cmean_reduce;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &luminance);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &partial_sums);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &n_pixels);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(float), &epsilon);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(float), &lum_min);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(float) * 4 * local_size, NULL);
    cl_err = dt_opencl_enqueue_kernel_2d_with_local(devid, kernel, sizes, local);
    if(cl_err != CL_SUCCESS) goto out;

    cl_err = dt_opencl_read_buffer_from_device(devid, partial_host, partial_sums, 0, sizeof(float) * 4 * n_groups,
                                               CL_TRUE);
    if(cl_err != CL_SUCCESS) goto out;
    double accum[4] = { 0.0, 0.0, 0.0, 0.0 };
    for(int group = 0; group < n_groups; group++)
      for(int k = 0; k < 4; k++) accum[k] += (double)partial_host[group * 4 + k];
    if(accum[3] > 0.0)
    {
      for(int c = 0; c < 3; c++) cmean[c] = (float)(accum[c] / accum[3]);

      // trusted-ring vote on the flat-mean prior (CPU _hl_ring_flat_mean_vote mirror): the
      // refinements (ratio pull + decoupled recombine) engage only where the 1-clip ring
      // confirms the region mean describes the blown core
      const float cmean_sum = fmaxf(cmean[0] + cmean[1] + cmean[2], 1e-9f);
      const float cmean_share[3]
          = { cmean[0] / cmean_sum, cmean[1] / cmean_sum, cmean[2] / cmean_sum };
      const int vote_kernel = global_data->kernel_hl_ring_vote;
      dt_opencl_set_kernel_arg(devid, vote_kernel, 0, sizeof(cl_mem), &estimate);
      dt_opencl_set_kernel_arg(devid, vote_kernel, 1, sizeof(cl_mem), &valid);
      dt_opencl_set_kernel_arg(devid, vote_kernel, 2, sizeof(cl_mem), &partial_sums);
      dt_opencl_set_kernel_arg(devid, vote_kernel, 3, sizeof(int), &n_pixels);
      dt_opencl_set_kernel_arg(devid, vote_kernel, 4, sizeof(float) * 8 * local_size, NULL);
      cl_err = dt_opencl_enqueue_kernel_2d_with_local(devid, vote_kernel, sizes, local);
      if(cl_err != CL_SUCCESS) goto out;
      cl_err = dt_opencl_read_buffer_from_device(devid, partial_host, partial_sums, 0,
                                                 sizeof(float) * 8 * n_groups, CL_TRUE);
      if(cl_err != CL_SUCCESS) goto out;
      double share_sum[3] = { 0.0, 0.0, 0.0 };
      double share_sq[3] = { 0.0, 0.0, 0.0 };
      double ring_count = 0.0;
      for(int group = 0; group < n_groups; group++)
      {
        for(int c = 0; c < 3; c++)
        {
          share_sum[c] += (double)partial_host[group * 8 + c];
          share_sq[c] += (double)partial_host[group * 8 + 4 + c];
        }
        ring_count += (double)partial_host[group * 8 + 3];
      }
      float ring_vote = 0.f;
      if(ring_count > 0.0)
      {
        float bias = 0.f;
        float dispersion = 0.f;
        for(int c = 0; c < 3; c++)
        {
          const double mean = share_sum[c] / ring_count;
          bias += fabsf((float)mean - cmean_share[c]);
          dispersion += sqrtf(fmaxf((float)(share_sq[c] / ring_count - mean * mean), 0.f));
        }
        const float t_stat = bias / fmaxf(dispersion, 0.02f);
        const float arg = t_stat / 5.f;
        ring_vote = expf(-arg * arg);
      }
      refine_gate = floor_gate * ring_vote;
      ratio_beta = 0.5f * refine_gate;
    }
  }

  // debug dump (HL_REG_DUMP=<file path>): save this region's brightness plane + hole mask
  // to the given file for offline replay through the HL_DOMECL_TEST self-test (the path is
  // taken from the variable itself: no fixed world-writable location)
  const char *reg_dump_path = getenv("HL_REG_DUMP");
  if(reg_dump_path && reg_dump_path[0])
  {
    float *dump_data = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
    uint8_t *dump_hole = (uint8_t *)dt_pixelpipe_cache_alloc_align(region_pixels, pipe);
    if(dump_data && dump_hole
       && dt_opencl_read_buffer_from_device(devid, dump_data, luminance, 0, sizeof(float) * region_pixels, CL_TRUE)
              == CL_SUCCESS
       && dt_opencl_read_buffer_from_device(devid, dump_hole, hole, 0, region_pixels, CL_TRUE) == CL_SUCCESS)
    {
      FILE *dump_file = g_fopen(reg_dump_path, "wb");
      if(dump_file)
      {
        fwrite(&region_w, sizeof(int), 1, dump_file);
        fwrite(&region_h, sizeof(int), 1, dump_file);
        const int downsample_val = ds_shared;
        fwrite(&downsample_val, sizeof(int), 1, dump_file);
        fwrite(dump_data, sizeof(float), region_pixels, dump_file);
        fwrite(dump_hole, 1, region_pixels, dump_file);
        fclose(dump_file);
      }
    }
    dt_pixelpipe_cache_free_align(dump_data);
    dt_pixelpipe_cache_free_align(dump_hole);
  }
  // shared biharmonic brightness dome over the union hole (GPU sparse Cholesky inside)
  cl_err
      = dt_opencl_enqueue_copy_buffer_to_buffer(devid, luminance, dome_lum, 0, 0, sizeof(float) * region_pixels);
  if(cl_err != CL_SUCCESS) goto out;
  cl_err = _biharmonic_dome_cl(devid, gd_void, dome_lum, hole, region_w, region_h, ds_shared, pipe);
  if(cl_err != CL_SUCCESS) goto out;

  // harmonically filled chromaticity ratios over the union hole
  {
    const int cf_base = (int)(CLAMP(reg_radius / 6.f, 8.f, 64.f) / 4.f);
    for(int c = 0; c < 3 && cl_err == CL_SUCCESS; c++)
    {
      const int kernel = global_data->kernel_hl_ratio_plane;
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &luminance);
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &ratios[c]);
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region_w);
      dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_h);
      dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &c);
      dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(float), &epsilon);
      cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
      if(cl_err == CL_SUCCESS)
        cl_err = _cf_harmonic_fill_cl(devid, gd_void, ratios[c], hole, region_w, region_h, cf_base, 1, NULL);
      if(cl_err == CL_SUCCESS && ratio_beta > 0.f)
      {
        // pull the filled ratio toward the mean valid chromaticity (CPU cmean pull mirror)
        const int blend_kernel = global_data->kernel_hl_ratio_cmean_blend;
        dt_opencl_set_kernel_arg(devid, blend_kernel, 0, sizeof(cl_mem), &ratios[c]);
        dt_opencl_set_kernel_arg(devid, blend_kernel, 1, sizeof(cl_mem), &hole);
        dt_opencl_set_kernel_arg(devid, blend_kernel, 2, sizeof(int), &region_w);
        dt_opencl_set_kernel_arg(devid, blend_kernel, 3, sizeof(int), &region_h);
        dt_opencl_set_kernel_arg(devid, blend_kernel, 4, sizeof(float), &cmean[c]);
        dt_opencl_set_kernel_arg(devid, blend_kernel, 5, sizeof(float), &ratio_beta);
        cl_err = dt_opencl_enqueue_kernel_2d(devid, blend_kernel, size);
      }
    }
    if(cl_err != CL_SUCCESS) goto out;
  }

  // depth-gated blend: dome value x filled ratios replaces the estimate where the fit is
  // doubtful and the pixel is shallow enough for the dome to be trustworthy
  {
    const int kernel = global_data->kernel_hl_dome_blend;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &model_quality);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &depth);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &dome_lum);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(cl_mem), &ratio0);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(cl_mem), &ratio1);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(cl_mem), &ratio2);
    dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(cl_mem), &hole);
    dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel, 11, sizeof(float), &cf_sigma);
    dt_opencl_set_kernel_arg(devid, kernel, 12, sizeof(float), &epsilon);
    dt_opencl_set_kernel_arg(devid, kernel, 13, sizeof(float), &refine_gate);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
    if(cl_err != CL_SUCCESS) goto out;
  }

  // hard floor re-assert: a clipped channel saturated, so its true value is >= its clip level
  {
    const int kernel = global_data->kernel_hl_hard_floor;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &clip0);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(float), &floor_gate);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
  }

out:
  dt_opencl_release_mem_object(luminance);
  dt_opencl_release_mem_object(hole);
  dt_opencl_release_mem_object(dome_lum);
  dt_opencl_release_mem_object(ratio0);
  dt_opencl_release_mem_object(ratio1);
  dt_opencl_release_mem_object(ratio2);
  dt_opencl_release_mem_object(partial_sums);
  return cl_err;
}

#endif // HAVE_OPENCL && DT_HL_SPARSE_SOLVE

#if defined(HAVE_OPENCL) && DT_HL_SPARSE_SOLVE

#endif // HAVE_OPENCL && DT_HL_SPARSE_SOLVE

#if defined(HAVE_OPENCL) && DT_HL_SPARSE_SOLVE
cl_int _joint_core_stage_cl(const int devid, void *gd_void, cl_mem estimate, cl_mem valid, cl_mem clip0,
                            const int region_w, const int region_h, const float solid_color,
                            const float reg_radius, const int extent, const float floor_gate,
                            const dt_dev_pixelpipe_t *pipe)
{
  dt_iop_highlights_global_data_t *global_data = (dt_iop_highlights_global_data_t *)gd_void;
  const size_t region_pixels = (size_t)region_w * region_h;
  cl_int cl_err = DT_OPENCL_DEFAULT_ERROR;
  size_t size[3] = { ROUNDUPDWD(region_w, devid), ROUNDUPDHT(region_h, devid), 1 };
  const float epsilon = 1e-6f;
  const float react
      = solid_color * solid_color * 4.f; // lambda_solid: the screened-Poisson reaction (flat-colour pull)

  if(global_data->kernel_hl_pde_rhs < 0 || global_data->kernel_hl_pde_scatter < 0) return cl_err; // no fp64 device

  cl_mem luminance = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem hole = dt_opencl_alloc_device_buffer(devid, region_pixels);
  cl_mem dome_lum = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem embedded = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem ratio0 = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem ratio1 = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem ratio2 = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem cg_field = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem ratios[3];
  ratios[0] = ratio0;
  ratios[1] = ratio1;
  ratios[2] = ratio2;
  cl_mem partial_sums = NULL, perm_grid_dev = NULL, rhs_dev = NULL, mask_img = NULL, mask_blur = NULL;
  uint8_t *hole_mask = (uint8_t *)dt_pixelpipe_cache_alloc_align(region_pixels, pipe);
  int *matrix_col_ptr = NULL, *matrix_row_index = NULL, *perm_grid = NULL;
  double *matrix_values = NULL;
  _sp_chol_cl_t *factor = NULL;
  dt_aligned_pixel_t chroma_mean = { 0.f, 0.f, 0.f, 0.f };
  if(!luminance || !hole || !dome_lum || !embedded || !ratio0 || !ratio1 || !ratio2 || !cg_field || !hole_mask)
    goto out;

  // luminance + ALL-clip hole (no surviving channel)
  {
    const int kernel = global_data->kernel_hl_lsb_hole;
    const int all_clip_mode = 1;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &luminance);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &hole);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &all_clip_mode);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
    if(cl_err != CL_SUCCESS) goto out;
  }

  // the sparse symbolic analysis needs the mask on the host anyway; it also gives the
  // all-clip count for the early exit and the CPU's auto grid factor for the dome
  cl_err = dt_opencl_read_buffer_from_device(devid, hole_mask, hole, 0, region_pixels, CL_TRUE);
  if(cl_err != CL_SUCCESS) goto out;

  size_t n_hole_fine = 0;
  for(size_t i = 0; i < region_pixels; i++)
    if(hole_mask[i]) n_hole_fine++;
  if(n_hole_fine == 0)
  {
    cl_err = CL_SUCCESS;
    goto out;
  }
  const int downsample = MAX(1, (int)ceilf(sqrtf((float)n_hole_fine / (float)DT_HL_DOME_NMAX_SPARSE)));

  // shared biharmonic luminance dome, floored at "all channels at clip"
  cl_err
      = dt_opencl_enqueue_copy_buffer_to_buffer(devid, luminance, dome_lum, 0, 0, sizeof(float) * region_pixels);
  if(cl_err != CL_SUCCESS) goto out;
  cl_err = _biharmonic_dome_cl(devid, gd_void, dome_lum, hole, region_w, region_h, downsample, pipe);
  if(cl_err != CL_SUCCESS) goto out;
  {
    const int kernel = global_data->kernel_hl_core_floor;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &dome_lum);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &hole);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &clip0);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_h);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
    if(cl_err != CL_SUCCESS) goto out;
  }

  // mean valid chromaticity: device partial sums, host finish
  {
    const int local_size = 64, n_groups = 256;
    const int n_pixels = (int)region_pixels;
    partial_sums = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 8 * n_groups);
    if(!partial_sums)
    {
      cl_err = DT_OPENCL_DEFAULT_ERROR;
      goto out;
    }
    const int kernel = global_data->kernel_hl_cmean_reduce;
    size_t sizes[3] = { (size_t)n_groups * local_size, 1, 1 };
    size_t local[3] = { local_size, 1, 1 };
    const float lum_min_all = 0.f; // APPROVED flat-colour solver target: every valid pixel
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &luminance);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &partial_sums);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &n_pixels);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(float), &epsilon);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(float), &lum_min_all);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(float) * 4 * local_size, NULL);
    cl_err = dt_opencl_enqueue_kernel_2d_with_local(devid, kernel, sizes, local);
    if(cl_err != CL_SUCCESS) goto out;

    float partial_host[8 * 256];
    cl_err = dt_opencl_read_buffer_from_device(devid, partial_host, partial_sums, 0, sizeof(float) * 4 * n_groups,
                                               CL_TRUE);
    if(cl_err != CL_SUCCESS) goto out;
    double accum[4] = { 0.0, 0.0, 0.0, 0.0 };
    for(int group = 0; group < n_groups; group++)
      for(int k = 0; k < 4; k++) accum[k] += (double)partial_host[group * 4 + k];
    if(accum[3] > 0.0)
      for(int c = 0; c < 3; c++) chroma_mean[c] = (float)(accum[c] / accum[3]);

    // Re-hue the all-clip core's saturation floor toward the mean valid chromaticity, blended by
    // the clip-asymmetry gate x the trusted-ring vote (CPU _joint_core rehue mirror): clip0 is a
    // magnitude floor, not a colour -- redistribute it to cmean so the aniso obstacle/floor
    // enforces the surround chromaticity instead of the inverse-WB magenta. The vote confines
    // this to cores the region's flat mean actually describes (off on self-coloured emitters and
    // gradient skies). Untouched at gate 0 (approved behavior).
    if(accum[3] > 0.0 && floor_gate > 1e-6f)
    {
      // BRIGHT surround mean for the rehue + its vote (chroma_mean above stays the approved
      // all-valid solver target; see the CPU counterpart for the rationale)
      float lum_min = 0.f;
      {
        const int plateau_kernel = global_data->kernel_hl_cgrad_plateau;
        dt_opencl_set_kernel_arg(devid, plateau_kernel, 0, sizeof(cl_mem), &estimate);
        dt_opencl_set_kernel_arg(devid, plateau_kernel, 1, sizeof(cl_mem), &valid);
        dt_opencl_set_kernel_arg(devid, plateau_kernel, 2, sizeof(cl_mem), &partial_sums);
        dt_opencl_set_kernel_arg(devid, plateau_kernel, 3, sizeof(int), &n_pixels);
        dt_opencl_set_kernel_arg(devid, plateau_kernel, 4, sizeof(float) * 2 * local_size, NULL);
        cl_err = dt_opencl_enqueue_kernel_2d_with_local(devid, plateau_kernel, sizes, local);
        if(cl_err != CL_SUCCESS) goto out;
        cl_err = dt_opencl_read_buffer_from_device(devid, partial_host, partial_sums, 0,
                                                   sizeof(float) * 2 * n_groups, CL_TRUE);
        if(cl_err != CL_SUCCESS) goto out;
        double plateau_sum = 0.0, plateau_count = 0.0;
        for(int group = 0; group < n_groups; group++)
        {
          plateau_sum += (double)partial_host[group * 2 + 0];
          plateau_count += (double)partial_host[group * 2 + 1];
        }
        if(plateau_count > 0.0) lum_min = 0.35f * (float)(plateau_sum / plateau_count);
      }
      float cmean_bright[3] = { 0.f, 0.f, 0.f };
      double bright_count = 0.0;
      {
        dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
        dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
        dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &luminance);
        dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &partial_sums);
        dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &n_pixels);
        dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(float), &epsilon);
        dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(float), &lum_min);
        dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(float) * 4 * local_size, NULL);
        cl_err = dt_opencl_enqueue_kernel_2d_with_local(devid, kernel, sizes, local);
        if(cl_err != CL_SUCCESS) goto out;
        cl_err = dt_opencl_read_buffer_from_device(devid, partial_host, partial_sums, 0,
                                                   sizeof(float) * 4 * n_groups, CL_TRUE);
        if(cl_err != CL_SUCCESS) goto out;
        double bright_accum[4] = { 0.0, 0.0, 0.0, 0.0 };
        for(int group = 0; group < n_groups; group++)
          for(int k = 0; k < 4; k++) bright_accum[k] += (double)partial_host[group * 4 + k];
        bright_count = bright_accum[3];
        if(bright_count > 0.0)
          for(int c = 0; c < 3; c++) cmean_bright[c] = (float)(bright_accum[c] / bright_count);
      }
      if(bright_count > 0.0)
      {
      const float cmean_sum = fmaxf(cmean_bright[0] + cmean_bright[1] + cmean_bright[2], 1e-9f);
      const float cmean_share[3] = { cmean_bright[0] / cmean_sum, cmean_bright[1] / cmean_sum,
                                     cmean_bright[2] / cmean_sum };
      const int vote_kernel = global_data->kernel_hl_ring_vote;
      dt_opencl_set_kernel_arg(devid, vote_kernel, 0, sizeof(cl_mem), &estimate);
      dt_opencl_set_kernel_arg(devid, vote_kernel, 1, sizeof(cl_mem), &valid);
      dt_opencl_set_kernel_arg(devid, vote_kernel, 2, sizeof(cl_mem), &partial_sums);
      dt_opencl_set_kernel_arg(devid, vote_kernel, 3, sizeof(int), &n_pixels);
      dt_opencl_set_kernel_arg(devid, vote_kernel, 4, sizeof(float) * 8 * local_size, NULL);
      cl_err = dt_opencl_enqueue_kernel_2d_with_local(devid, vote_kernel, sizes, local);
      if(cl_err != CL_SUCCESS) goto out;
      cl_err = dt_opencl_read_buffer_from_device(devid, partial_host, partial_sums, 0,
                                                 sizeof(float) * 8 * n_groups, CL_TRUE);
      if(cl_err != CL_SUCCESS) goto out;
      double share_sum[3] = { 0.0, 0.0, 0.0 };
      double share_sq[3] = { 0.0, 0.0, 0.0 };
      double ring_count = 0.0;
      for(int group = 0; group < n_groups; group++)
      {
        for(int c = 0; c < 3; c++)
        {
          share_sum[c] += (double)partial_host[group * 8 + c];
          share_sq[c] += (double)partial_host[group * 8 + 4 + c];
        }
        ring_count += (double)partial_host[group * 8 + 3];
      }
      float ring_vote = 0.f;
      if(ring_count > 0.0)
      {
        float bias = 0.f;
        float dispersion = 0.f;
        for(int c = 0; c < 3; c++)
        {
          const double mean = share_sum[c] / ring_count;
          bias += fabsf((float)mean - cmean_share[c]);
          dispersion += sqrtf(fmaxf((float)(share_sq[c] / ring_count - mean * mean), 0.f));
        }
        const float t_stat = bias / fmaxf(dispersion, 0.02f);
        const float arg = t_stat / 5.f;
        ring_vote = expf(-arg * arg);
      }
      const float rehue_gate = floor_gate * ring_vote;

      if(rehue_gate > 1e-6f)
      {
        const int rehue_kernel = global_data->kernel_hl_clip0_rehue;
        dt_opencl_set_kernel_arg(devid, rehue_kernel, 0, sizeof(cl_mem), &clip0);
        dt_opencl_set_kernel_arg(devid, rehue_kernel, 1, sizeof(cl_mem), &hole);
        dt_opencl_set_kernel_arg(devid, rehue_kernel, 2, sizeof(int), &region_w);
        dt_opencl_set_kernel_arg(devid, rehue_kernel, 3, sizeof(int), &region_h);
        dt_opencl_set_kernel_arg(devid, rehue_kernel, 4, sizeof(float), &cmean_bright[0]);
        dt_opencl_set_kernel_arg(devid, rehue_kernel, 5, sizeof(float), &cmean_bright[1]);
        dt_opencl_set_kernel_arg(devid, rehue_kernel, 6, sizeof(float), &cmean_bright[2]);
        dt_opencl_set_kernel_arg(devid, rehue_kernel, 7, sizeof(float), &rehue_gate);
        cl_err = dt_opencl_enqueue_kernel_2d(devid, rehue_kernel, size);
        if(cl_err != CL_SUCCESS) goto out;
      }
      }
    }
  }

  // ONE symbolic analysis + GPU numeric factorization for the three channels; when the core
  // exceeds DT_HL_SPARSE_MAX (or the factorization fails) take the same road as the CPU:
  // the matrix-free CG, here fully on the device
  // assemble A = lambda_solid*I - Delta (order 1) over the all-clip hole; use_cg when the core
  // is too large for the direct factorization (mirrors the CPU _sp_pde_factor / CG choice)
  int n_unknowns = 0;
  int use_cg
      = !_sp_pde_assemble(hole_mask, NULL, (react > 0.f) ? react : 0.f, 1, 1.f, region_w, region_h,
                          &matrix_col_ptr, &matrix_row_index, &matrix_values, &perm_grid, &n_unknowns, pipe);
  if(!use_cg)
  {
    factor = _sp_chol_factor_cl(devid, _hl_sp_chol_kernels(gd_void), n_unknowns, matrix_col_ptr, matrix_row_index,
                                matrix_values);
    perm_grid_dev = factor ? _sp_cl_upload(devid, perm_grid, sizeof(int) * n_unknowns) : NULL;
    rhs_dev = factor ? dt_opencl_alloc_device_buffer(devid, sizeof(double) * n_unknowns) : NULL;
    if(!factor)
      use_cg = 1;
    else if(!perm_grid_dev || !rhs_dev)
    {
      cl_err = DT_OPENCL_DEFAULT_ERROR;
      goto out;
    }
  }
  const int max_iter = CLAMP(2 * extent, 200, 2000);

  // per channel: build the chromaticity ratio plane, solve its diffusion system, store into ratios[c]
  for(int c = 0; c < 3; c++)
  {
    // init: ratio plane on valid pixels, flat-colour seed on the hole (cg_field = solver unknown)
    {
      const int kernel = global_data->kernel_hl_pde_init;
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &luminance);
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &hole);
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &embedded);
      dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &ratios[c]);
      dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(cl_mem), &cg_field);
      dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &region_w);
      dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &region_h);
      dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &c);
      dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(float), &chroma_mean[c]);
      dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(float), &epsilon);
      cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
      if(cl_err != CL_SUCCESS) goto out;
    }
    // direct path: assemble this channel's right-hand side on the device...
    if(!use_cg)
    {
      const int kernel = global_data->kernel_hl_pde_rhs;
      size_t size_1d[3] = { ROUNDUP(n_unknowns, 64), 1, 1 };
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &embedded);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &perm_grid_dev);
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &rhs_dev);
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &n_unknowns);
      dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_w);
      dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &region_h);
      dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(float), &react);
      dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(float), &chroma_mean[c]);
      cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size_1d);
      if(cl_err != CL_SUCCESS) goto out;
    }
    // ...solve with the shared Cholesky factor...
    if(!use_cg)
    {
      if(_sp_chol_solve_cl(factor, _hl_sp_chol_kernels(gd_void), rhs_dev))
      {
        cl_err = DT_OPENCL_DEFAULT_ERROR;
        goto out;
      }
      // validate before scattering: the device factor kernel takes sqrt() of the pivots
      // without checking their sign, so a system whose replicate-clamped border rows are not
      // positive definite yields quiet NaN -- the CPU factor REJECTS such systems and falls
      // back to conjugate gradient, and the device path must degrade the same way instead of
      // blending NaN into the output. n_unknowns <= 16384 doubles = at most 128 KB on the bus.
      double *solution_check = (double *)dt_pixelpipe_cache_alloc_align(sizeof(double) * n_unknowns, pipe);
      int finite = (solution_check != NULL);
      if(solution_check)
      {
        finite = (dt_opencl_read_buffer_from_device(devid, solution_check, rhs_dev, 0, sizeof(double) * n_unknowns,
                                                    CL_TRUE)
                  == CL_SUCCESS);
        for(int check_index = 0; finite && check_index < n_unknowns; check_index++)
          if(!isfinite(solution_check[check_index])) finite = 0;
        dt_pixelpipe_cache_free_align(solution_check);
      }
      if(!finite)
      {
        _sp_chol_cl_free(factor);
        factor = NULL;
        use_cg = 1; // this channel and the remaining ones take the iterative road
      }
      else
      {
        // ...and scatter the solution back into the ratio plane
        const int kernel = global_data->kernel_hl_pde_scatter;
        size_t size_1d[3] = { ROUNDUP(n_unknowns, 64), 1, 1 };
        dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &rhs_dev);
        dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &perm_grid_dev);
        dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &ratios[c]);
        dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &n_unknowns);
        cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size_1d);
        if(cl_err != CL_SUCCESS) goto out;
        continue;
      }
    }
    // iterative road: on-device conjugate gradient on the seeded unknown, then clamp
    // the ratios non-negative (also the recovery path when the direct solve was rejected)
    {
      cl_err = _region_pde_cg_cl(devid, gd_void, cg_field, hole, region_w, region_h, (react > 0.f) ? react : 0.f,
                                 (react > 0.f) ? chroma_mean[c] : 0.f, max_iter);
      if(cl_err != CL_SUCCESS) goto out;
      const int kernel = global_data->kernel_hl_relu;
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &cg_field);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &ratios[c]);
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &region_w);
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region_h);
      cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
      if(cl_err != CL_SUCCESS) goto out;
    }
  }

  // feathered composite: blur the core mask and blend dome x ratios into estimate through it
  // (no hard hand-off at the core rim)
  mask_img = dt_opencl_alloc_device(devid, region_w, region_h, sizeof(float));
  mask_blur = dt_opencl_alloc_device(devid, region_w, region_h, sizeof(float));
  if(!mask_img || !mask_blur)
  {
    cl_err = DT_OPENCL_DEFAULT_ERROR;
    goto out;
  }
  {
    const int kernel = global_data->kernel_hl_mask_to_img1;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &hole);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &mask_img);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region_h);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
    if(cl_err != CL_SUCCESS) goto out;
  }
  cl_err = _region_blur1_cl(devid, mask_img, mask_blur, region_w, region_h,
                            fmaxf(4.f, CLAMP(reg_radius / 6.f, 8.f, 64.f) / 4.f));
  if(cl_err != CL_SUCCESS) goto out;
  {
    const int kernel = global_data->kernel_hl_core_blend;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &hole);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &dome_lum);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &ratio0);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(cl_mem), &ratio1);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(cl_mem), &ratio2);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(cl_mem), &mask_blur);
    dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(float), &epsilon);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
  }

out:
  dt_opencl_release_mem_object(luminance);
  dt_opencl_release_mem_object(hole);
  dt_opencl_release_mem_object(dome_lum);
  dt_opencl_release_mem_object(embedded);
  dt_opencl_release_mem_object(ratio0);
  dt_opencl_release_mem_object(ratio1);
  dt_opencl_release_mem_object(ratio2);
  dt_opencl_release_mem_object(cg_field);
  dt_opencl_release_mem_object(partial_sums);
  dt_opencl_release_mem_object(perm_grid_dev);
  dt_opencl_release_mem_object(rhs_dev);
  dt_opencl_release_mem_object(mask_img);
  dt_opencl_release_mem_object(mask_blur);
  dt_pixelpipe_cache_free_align(hole_mask);
  dt_pixelpipe_cache_free_align(matrix_col_ptr);
  dt_pixelpipe_cache_free_align(matrix_row_index);
  dt_pixelpipe_cache_free_align(matrix_values);
  dt_pixelpipe_cache_free_align(perm_grid);
  _sp_chol_cl_free(factor);
  return cl_err;
}

// Chromaticity-gradient continuation on the device (see _chromaticity_gradient / core.h for the rationale).
// Mirrors the CPU stage step by step: plateau reduction -> guard blur -> anchor mask (host-read for
// the bail decision and the dome's downsample factor, same auto formula as the CPU dome) -> three
// biharmonic share extensions -> reprojection + joint floor. Re-validate with HL_CGRADCL_TEST.
cl_int _chromaticity_gradient_stage_cl(const int devid, void *gd_void, cl_mem estimate, cl_mem valid,
                                       cl_mem clip0, const int region_w, const int region_h,
                                       const float reg_radius, const float floor_gate,
                                       const dt_dev_pixelpipe_t *pipe)
{
  dt_iop_highlights_global_data_t *global_data = (dt_iop_highlights_global_data_t *)gd_void;
  const size_t region_pixels = (size_t)region_w * region_h;
  cl_int cl_err = DT_OPENCL_DEFAULT_ERROR;
  size_t size[3] = { ROUNDUPDWD(region_w, devid), ROUNDUPDHT(region_h, devid), 1 };
  const float epsilon = 1e-6f;

  cl_mem guard_src = dt_opencl_alloc_device(devid, region_w, region_h, sizeof(float));  // image (gaussian)
  cl_mem guard_blur = dt_opencl_alloc_device(devid, region_w, region_h, sizeof(float)); // image (gaussian)
  cl_mem gate_src = dt_opencl_alloc_device(devid, region_w, region_h, sizeof(float));   // image (gaussian)
  cl_mem gate_msk = dt_opencl_alloc_device(devid, region_w, region_h, sizeof(float));   // image (gaussian)
  cl_mem gate_wgt = dt_opencl_alloc_device(devid, region_w, region_h, sizeof(float));   // image (gaussian)
  cl_mem gate_nrm = dt_opencl_alloc_device(devid, region_w, region_h, sizeof(float));   // image (gaussian)
  cl_mem hole = dt_opencl_alloc_device_buffer(devid, region_pixels);
  cl_mem field = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem shares = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels * 4);
  cl_mem partial_sums = NULL;
  uint8_t *hole_host = (uint8_t *)dt_pixelpipe_cache_alloc_align(region_pixels, pipe);
  if(!guard_src || !guard_blur || !gate_src || !gate_msk || !gate_wgt || !gate_nrm || !hole || !field || !shares
     || !hole_host)
    goto out;

  // --- 1. plateau luminance over the any-clip pixels (device partials, host finish) ---
  float lum_anchor_min = 0.f;
  {
    const int local_size = 64, n_groups = 256;
    const int n_pixels = (int)region_pixels;
    partial_sums = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 2 * n_groups);
    if(!partial_sums) goto out;
    const int kernel = global_data->kernel_hl_cgrad_plateau;
    size_t sizes[3] = { (size_t)n_groups * local_size, 1, 1 };
    size_t local[3] = { local_size, 1, 1 };
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &partial_sums);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &n_pixels);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(float) * 2 * local_size, NULL);
    cl_err = dt_opencl_enqueue_kernel_2d_with_local(devid, kernel, sizes, local);
    if(cl_err != CL_SUCCESS) goto out;
    float partial_host[2 * 256];
    cl_err = dt_opencl_read_buffer_from_device(devid, partial_host, partial_sums, 0,
                                               sizeof(float) * 2 * n_groups, CL_TRUE);
    if(cl_err != CL_SUCCESS) goto out;
    double lum_accum = 0.0, count = 0.0;
    for(int group = 0; group < n_groups; group++)
    {
      lum_accum += (double)partial_host[group * 2 + 0];
      count += (double)partial_host[group * 2 + 1];
    }
    if(count == 0.0) // nothing blown in this region
    {
      cl_err = CL_SUCCESS;
      goto out;
    }
    lum_anchor_min = 0.35f * (float)(lum_accum / count);
  }

  // --- 2. guard proximity: blur the any-clip mask (thin ring, sigma 4 -- see the CPU stage) ---
  {
    const int kernel = global_data->kernel_hl_cgrad_guard;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &guard_src);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region_h);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
    if(cl_err != CL_SUCCESS) goto out;
    cl_err = _region_blur1_cl(devid, guard_src, guard_blur, region_w, region_h, 4.f);
    if(cl_err != CL_SUCCESS) goto out;
  }

  // --- 3. anchor mask; host-read for the bail decision + the dome's auto downsample factor ---
  {
    const int kernel = global_data->kernel_hl_cgrad_anchor;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &guard_blur);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &hole);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(float), &lum_anchor_min);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
    if(cl_err != CL_SUCCESS) goto out;
  }
  cl_err = dt_opencl_read_buffer_from_device(devid, hole_host, hole, 0, region_pixels, CL_TRUE);
  if(cl_err != CL_SUCCESS) goto out;
  size_t n_hole = 0;
  for(size_t i = 0; i < region_pixels; i++) n_hole += (hole_host[i] != 0);
  const size_t n_anchor = region_pixels - n_hole;
  if(n_anchor < 64 || n_anchor < region_pixels / 256) // no usable bright surround: keep the fence chromaticity
  {
    cl_err = CL_SUCCESS;
    goto out;
  }
  const int downsample = MAX(1, (int)ceilf(sqrtf((float)n_hole / (float)DT_HL_DOME_NMAX_SPARSE)));

  // --- 4. per channel: biharmonic (gradient-extending) share extension ---
  for(int c = 0; c < 3; c++)
  {
    {
      const int kernel = global_data->kernel_hl_cgrad_share;
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &field);
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &region_w);
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region_h);
      dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &c);
      dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(float), &epsilon);
      cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
      if(cl_err != CL_SUCCESS) goto out;
    }
    cl_err = _biharmonic_dome_cl(devid, gd_void, field, hole, region_w, region_h, downsample, pipe);
    if(cl_err != CL_SUCCESS) goto out;
    {
      const int kernel = global_data->kernel_hl_cgrad_store;
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &field);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &shares);
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &region_w);
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region_h);
      dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &c);
      cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
      if(cl_err != CL_SUCCESS) goto out;
    }
  }

  // --- 5. content gate: agreement with the 1-clip annulus, diffused by normalized convolution
  // (mirrors the CPU gate; see _chromaticity_gradient) ---
  {
    const float gate_tau = 0.10f;
    const int kernel = global_data->kernel_hl_cgrad_gate;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &shares);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &clip0);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &gate_src);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(cl_mem), &gate_msk);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(float), &epsilon);
    dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(float), &gate_tau);
    dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(float), &floor_gate);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
    if(cl_err != CL_SUCCESS) goto out;
    const float gate_sigma = CLAMP(reg_radius / 4.f, 8.f, 96.f);
    cl_err = _region_blur1_cl(devid, gate_src, gate_wgt, region_w, region_h, gate_sigma);
    if(cl_err != CL_SUCCESS) goto out;
    cl_err = _region_blur1_cl(devid, gate_msk, gate_nrm, region_w, region_h, gate_sigma);
    if(cl_err != CL_SUCCESS) goto out;
  }

  // region-level ring vote (see the CPU stage): read the source images back once and sum on the
  // host -- the mirrors must agree exactly, and the gate sources are a single float plane each
  float gate_vote = 0.f;
  {
    float *vote_host = dt_pixelpipe_cache_alloc_align_float(region_pixels * 2, pipe);
    if(!vote_host)
    {
      cl_err = DT_OPENCL_DEFAULT_ERROR;
      goto out;
    }
    const size_t origin[3] = { 0, 0, 0 };
    const size_t reg[3] = { (size_t)region_w, (size_t)region_h, 1 };
    cl_err = dt_opencl_read_host_from_device_raw(devid, vote_host, gate_src, origin, reg,
                                                 region_w * sizeof(float), CL_TRUE);
    if(cl_err == CL_SUCCESS)
      cl_err = dt_opencl_read_host_from_device_raw(devid, vote_host + region_pixels, gate_msk, origin, reg,
                                                   region_w * sizeof(float), CL_TRUE);
    if(cl_err == CL_SUCCESS)
    {
      double wsum = 0.0, msum = 0.0;
      for(size_t i = 0; i < region_pixels; i++)
      {
        wsum += (double)vote_host[i];
        msum += (double)vote_host[region_pixels + i];
      }
      gate_vote = (msum > 0.0) ? (float)(wsum / msum) : 0.f;
    }
    dt_pixelpipe_cache_free_align(vote_host);
    if(cl_err != CL_SUCCESS) goto out;
  }

  // --- 6. reproject the multi-clip subsets onto the field (gate-blended) + joint saturation floor ---
  {
    const int kernel = global_data->kernel_hl_cgrad_reproject;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &clip0);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &shares);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &gate_wgt);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(cl_mem), &gate_nrm);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(float), &epsilon);
    dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(float), &gate_vote);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
  }

  // --- 7. PASS 2 = a3, VALUE continuation of the floor-authored 1-clip band (WB'd clips only;
  // see the CPU stage's pass 2): per clipped channel, extend the channel's own VALUE
  // biharmonically over the authored collar, anchored on both sides, floored at saturation.
  if(floor_gate > 1e-6f)
  {
    for(int c = 0; c < 3; c++)
    {
      {
        const int kernel = global_data->kernel_hl_cgrad_hole1c;
        dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
        dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
        dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &clip0);
        dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &hole);
        dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &field);
        dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &region_w);
        dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &region_h);
        dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &c);
        cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
        if(cl_err != CL_SUCCESS) goto out;
      }
      // the dome's auto downsample mirrors the CPU: count this channel's hole
      cl_err = dt_opencl_read_buffer_from_device(devid, hole_host, hole, 0, region_pixels, CL_TRUE);
      if(cl_err != CL_SUCCESS) goto out;
      size_t n_hole_c = 0;
      for(size_t i = 0; i < region_pixels; i++) n_hole_c += (hole_host[i] != 0);
      if(n_hole_c == 0) continue;
      const int downsample_c = MAX(1, (int)ceilf(sqrtf((float)n_hole_c / (float)DT_HL_DOME_NMAX_SPARSE)));
      cl_err = _biharmonic_dome_cl(devid, gd_void, field, hole, region_w, region_h, downsample_c, pipe);
      if(cl_err != CL_SUCCESS) goto out;
      {
        const int kernel = global_data->kernel_hl_cgrad_write1c;
        dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
        dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &hole);
        dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &field);
        dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &clip0);
        dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_w);
        dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &region_h);
        dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &c);
        cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
        if(cl_err != CL_SUCCESS) goto out;
      }
    }
  }

out:
  dt_opencl_release_mem_object(guard_src);
  dt_opencl_release_mem_object(guard_blur);
  dt_opencl_release_mem_object(gate_src);
  dt_opencl_release_mem_object(gate_msk);
  dt_opencl_release_mem_object(gate_wgt);
  dt_opencl_release_mem_object(gate_nrm);
  dt_opencl_release_mem_object(hole);
  dt_opencl_release_mem_object(field);
  dt_opencl_release_mem_object(shares);
  dt_opencl_release_mem_object(partial_sums);
  dt_pixelpipe_cache_free_align(hole_host);
  return cl_err;
}

#endif // HAVE_OPENCL && DT_HL_SPARSE_SOLVE
