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

#include "common/darktable.h"
#include "common/solvers/choleski.h"
#include "control/control.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "iop/highlights/blur.h"
#include "iop/highlights/coefficient_field.h"
#include "iop/highlights/core.h"
#include "iop/highlights/dome.h"
#include "iop/highlights/knee.h"
#include "iop/highlights/pde.h"
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

    for(int c = 0; c < 3; c++)
    {
      HL_PFOR()
      for(size_t i = 0; i < region_pixels; i++)
        flat_target[i] = estimate[i * 4 + c] / fmaxf(lum_accum[i], epsilon); // ratio r_c = est_c / L_sum

      _cf_harmonic_fill(flat_target, hole, region_w, region_h, cf_base, NULL, pipe); // harmonic (Delta r = 0)

      HL_PFOR()
      for(size_t i = 0; i < region_pixels; i++) plane1[i * 4 + c] = fmaxf(flat_target[i], 0.f);
    }

    // recombine dome_c = L_dome * (r_c / sum r) and blend it into the estimate by the depth-gated
    // KEEP weight conf_weight = Wc^2 (= 1 - dome_fraction of step 6): est = keep*est + (1-keep)*dome.
    // A pixel with no surviving guide takes the dome outright (the all-clip core rebuilds it just after).
    HL_PFOR()
    for(size_t i = 0; i < region_pixels; i++)
    {
      if(!hole[i]) continue;

      const float caccum = fmaxf(plane1[i * 4 + 0] + plane1[i * 4 + 1] + plane1[i * 4 + 2], epsilon); // sum r
      const int anyvalid = (valid[i * 4 + 0] >= 0.5f) || (valid[i * 4 + 1] >= 0.5f) || (valid[i * 4 + 2] >= 0.5f);

      for(int c = 0; c < 3; c++)
        if(valid[i * 4 + c] < 0.5f)
        {
          const float dome = dome_lum[i] * (plane1[i * 4 + c] / caccum); // dome_c = L_dome * chroma share
          const float conf_weight = valid_variance[i * 4 + c] * valid_variance[i * 4 + c]; // keep = Wc^2
          estimate[i * 4 + c] = anyvalid ? (conf_weight * estimate[i * 4 + c] + (1.f - conf_weight) * dome) : dome;
        }
    }

    // Re-assert the saturation floor AFTER the self dome (the prototype floors here): the dome only
    // continues the valid rim, it does not know about saturation, so it can undershoot a clipped
    // channel below its clip level. Monotone (only raises), so it never overshoots or drifts hue.
    __OMP_PARALLEL_FOR__()
    for(size_t i = 0; i < region_pixels; i++)
      for(int c = 0; c < 3; c++)
        if(valid[i * 4 + c] < 0.5f) estimate[i * 4 + c] = fmaxf(estimate[i * 4 + c], clip0[i * 4 + c]);
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

// ============================ OpenCL ============================

#if defined(HAVE_OPENCL) && DT_HL_SPARSE_SOLVE
cl_int _selfdome_stage_cl(const int devid, void *gd_void, cl_mem estimate, cl_mem valid, cl_mem model_quality,
                          cl_mem clip0, cl_mem depth, const int region_w, const int region_h, const float cf_sigma,
                          const float reg_radius, const int ds_shared, const dt_dev_pixelpipe_t *pipe)
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
  if(!luminance || !hole || !dome_lum || !ratio0 || !ratio1 || !ratio2) goto out;

  // soft floor first (production order: floor -> dome gate -> self dome)
  {
    const int kernel = global_data->kernel_hl_soft_floor;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &clip0);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_h);
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
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
  }

out:
  dt_opencl_release_mem_object(luminance);
  dt_opencl_release_mem_object(hole);
  dt_opencl_release_mem_object(dome_lum);
  dt_opencl_release_mem_object(ratio0);
  dt_opencl_release_mem_object(ratio1);
  dt_opencl_release_mem_object(ratio2);
  return cl_err;
}

#endif // HAVE_OPENCL && DT_HL_SPARSE_SOLVE

#if defined(HAVE_OPENCL) && DT_HL_SPARSE_SOLVE

#endif // HAVE_OPENCL && DT_HL_SPARSE_SOLVE

#if defined(HAVE_OPENCL) && DT_HL_SPARSE_SOLVE
cl_int _joint_core_stage_cl(const int devid, void *gd_void, cl_mem estimate, cl_mem valid, cl_mem clip0,
                            const int region_w, const int region_h, const float solid_color,
                            const float reg_radius, const int extent, const dt_dev_pixelpipe_t *pipe)
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
    partial_sums = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 4 * n_groups);
    if(!partial_sums)
    {
      cl_err = DT_OPENCL_DEFAULT_ERROR;
      goto out;
    }
    const int kernel = global_data->kernel_hl_cmean_reduce;
    size_t sizes[3] = { (size_t)n_groups * local_size, 1, 1 };
    size_t local[3] = { local_size, 1, 1 };
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &luminance);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &partial_sums);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &n_pixels);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(float), &epsilon);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(float) * 4 * local_size, NULL);
    cl_err = dt_opencl_enqueue_kernel_2d_with_local(devid, kernel, sizes, local);
    if(cl_err != CL_SUCCESS) goto out;

    float partial_host[4 * 256];
    cl_err = dt_opencl_read_buffer_from_device(devid, partial_host, partial_sums, 0, sizeof(float) * 4 * n_groups,
                                               CL_TRUE);
    if(cl_err != CL_SUCCESS) goto out;
    double accum[4] = { 0.0, 0.0, 0.0, 0.0 };
    for(int group = 0; group < n_groups; group++)
      for(int k = 0; k < 4; k++) accum[k] += (double)partial_host[group * 4 + k];
    if(accum[3] > 0.0)
      for(int c = 0; c < 3; c++) chroma_mean[c] = (float)(accum[c] / accum[3]);
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

#endif // HAVE_OPENCL && DT_HL_SPARSE_SOLVE
