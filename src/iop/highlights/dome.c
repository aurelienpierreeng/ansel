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

// Biharmonic luminance dome solve (CPU + OpenCL). (implementation; see dome.h for the public API.)

#include "common/darktable.h"
#include "common/solvers/choleski.h"
#include "control/control.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "iop/highlights/blur.h"
#include "iop/highlights/dome.h"
#include "iop/highlights/pde.h"
#include <math.h>
#include <string.h>

__DT_CLONE_TARGETS__
void _biharmonic_dome(float *const restrict field, const uint8_t *const restrict hole, const int region_w,
                      const int region_h, const int forced_downsample, const dt_dev_pixelpipe_t *pipe)
{
  const size_t region_pixels = (size_t)region_w * region_h;
  size_t n_hole_fine = 0;
  for(size_t i = 0; i < region_pixels; i++)
    if(hole[i]) n_hole_fine++;
  if(n_hole_fine == 0) return;

  // pick a downsampling factor so the coarse hole has at most ~DT_HL_DOME_NMAX unknowns (the dense
  // Cholesky is O(N^3)). Raise DT_HL_DOME_NMAX to make the dome grid finer / exact (downsample -> 1)
  // at more cost -- a quick way to test whether the coarse approximation matters for a given image.
  const int max_unknowns = DT_HL_DOME_NMAX_SPARSE;
  // The caller may force the factor (forced_downsample > 0) so several per-channel domes share ONE
  // grid resolution. With a per-channel factor (each channel picking its own from its own hole size)
  // the three domes are approximated at different scales, their ratio drifts, and a saturated colour
  // collapses off-hue. forced_downsample == 0 keeps the standalone behaviour (auto from this hole).
  int downsample = (forced_downsample > 0) ? forced_downsample
                                           : MAX(1, (int)ceilf(sqrtf((float)n_hole_fine / (float)max_unknowns)));
  int coarse_w = (region_w + downsample - 1) / downsample;
  int coarse_h = (region_h + downsample - 1) / downsample;
  const size_t coarse_pixels = (size_t)coarse_w * coarse_h;

  float *const restrict coarse_field = dt_pixelpipe_cache_alloc_align_float(coarse_pixels, pipe);
  uint8_t *const restrict coarse_hole
      = (uint8_t *)dt_pixelpipe_cache_alloc_align(sizeof(uint8_t) * coarse_pixels, pipe);
  int *const restrict coarse_index = (int *)dt_pixelpipe_cache_alloc_align(sizeof(int) * coarse_pixels, pipe);
  if(!coarse_field || !coarse_hole || !coarse_index)
  {
    dt_pixelpipe_cache_free_align(coarse_field);
    dt_pixelpipe_cache_free_align(coarse_hole);
    dt_pixelpipe_cache_free_align(coarse_index);
    return;
  }

  // box-downsample: coarse value = mean of the block's VALID (non-hole) fine pixels; a coarse cell
  // is a hole if the majority of its block is hole (so boundary cells keep real rim data)
  __OMP_PARALLEL_FOR__(collapse(2))
  for(int coarse_y = 0; coarse_y < coarse_h; coarse_y++)
    for(int coarse_x = 0; coarse_x < coarse_w; coarse_x++)
    {
      double accum = 0.0;
      int n_valid = 0, n_hole_block = 0, n_total = 0;
      for(int fine_y = coarse_y * downsample; fine_y < MIN((coarse_y + 1) * downsample, region_h); fine_y++)
        for(int fine_x = coarse_x * downsample; fine_x < MIN((coarse_x + 1) * downsample, region_w); fine_x++)
        {
          const size_t fine_index = (size_t)fine_y * region_w + fine_x;
          n_total++;
          if(hole[fine_index])
          {
            n_hole_block++;
          }
          else
          {
            accum += field[fine_index];
            n_valid++;
          }
        }
      const size_t coarse_i = (size_t)coarse_y * coarse_w + coarse_x;
      coarse_hole[coarse_i] = (2 * n_hole_block > n_total) ? 1 : 0;
      coarse_field[coarse_i] = (n_valid > 0) ? (float)(accum / n_valid) : 0.f;
    }

  // enumerate coarse hole unknowns
  int n_unknowns = 0;
  for(size_t coarse_i = 0; coarse_i < coarse_pixels; coarse_i++)
    coarse_index[coarse_i] = coarse_hole[coarse_i] ? n_unknowns++ : -1;

  if(n_unknowns > 0)
  {
    // 13-point Delta^2 stencil (Laplacian of the 5-point Laplacian): the discrete biharmonic
    // operator Delta^2 u = Delta(Delta u), reaching TWO rings out (hence the +-2 taps and the
    // 2-ring Dirichlet). Weights {20,-8,-8,-8,-8, 2,2,2,2, 1,1,1,1} = the standard 5-point
    // Laplacian convolved with itself (center 20, edge -8, diagonal 2, far-axis 1).
    const int stencil_dy[13] = { 0, -1, 1, 0, 0, -1, -1, 1, 1, -2, 2, 0, 0 };
    const int stencil_dx[13] = { 0, 0, 0, -1, 1, -1, 1, -1, 1, 0, 0, -2, 2 };
    const float stencil_weight[13] = { 20.f, -8.f, -8.f, -8.f, -8.f, 2.f, 2.f, 2.f, 2.f, 1.f, 1.f, 1.f, 1.f };
    int solved = 0;

    // ---- sparse direct solve (the DT_HL_DOME_NMAX_SPARSE-sized grid) ----
    {
      int *unknown_x = (int *)dt_pixelpipe_cache_alloc_align(sizeof(int) * n_unknowns, pipe);
      int *unknown_y = (int *)dt_pixelpipe_cache_alloc_align(sizeof(int) * n_unknowns, pipe);
      int *permutation = (int *)dt_pixelpipe_cache_alloc_align(sizeof(int) * n_unknowns, pipe);
      int *inverse_perm = (int *)dt_pixelpipe_cache_alloc_align(sizeof(int) * n_unknowns, pipe);
      int *matrix_col_ptr = (int *)dt_pixelpipe_cache_alloc_align(sizeof(int) * (n_unknowns + 1), pipe);
      double *right_hand_side = (double *)dt_pixelpipe_cache_alloc_align(sizeof(double) * n_unknowns, pipe);
      int *matrix_row_index = NULL;
      double *matrix_values = NULL;

      if(unknown_x && unknown_y && permutation && inverse_perm && matrix_col_ptr && right_hand_side)
      {
        for(size_t coarse_i = 0; coarse_i < coarse_pixels; coarse_i++)
          if(coarse_hole[coarse_i])
          {
            unknown_x[coarse_index[coarse_i]] = (int)(coarse_i % coarse_w);
            unknown_y[coarse_index[coarse_i]] = (int)(coarse_i / coarse_w);
          }

        for(int i = 0; i < n_unknowns; i++) permutation[i] = i;
        _sp_nd_order(permutation, n_unknowns, unknown_x, unknown_y, 2);
        for(int perm_index = 0; perm_index < n_unknowns; perm_index++)
          inverse_perm[permutation[perm_index]] = perm_index;

        // assembly (count pass, then fill), upper triangle, permuted indexing; border-clamped
        // rows keep the later-eliminated unknown's row value, matching the dense solver's
        // lower-triangle convention (see the same note in _sp_pde_assemble)
        int success = 1;
        int targets[13];
        double target_weights[13];

        for(int pass = 0; pass < 2 && success; pass++)
        {
          if(pass == 1)
          {
            int total = 0;
            for(int perm_index = 0; perm_index < n_unknowns; perm_index++)
            {
              const int col_count = matrix_col_ptr[perm_index];
              matrix_col_ptr[perm_index] = total;
              total += col_count;
            }
            matrix_col_ptr[n_unknowns] = total;
            matrix_row_index = (int *)dt_pixelpipe_cache_alloc_align(sizeof(int) * total, pipe);
            matrix_values = (double *)dt_pixelpipe_cache_alloc_align(sizeof(double) * total, pipe);
            if(!matrix_row_index || !matrix_values) success = 0;
          }

          for(int perm_index = 0; perm_index < n_unknowns && success; perm_index++)
          {
            const int coarse_y = unknown_y[permutation[perm_index]];
            const int coarse_x = unknown_x[permutation[perm_index]];

            // row of the 13-point stencil at (coarse_y, coarse_x), clamped, duplicates summed:
            // one row of Delta^2 u = 0 restricted to the hole unknowns
            int count = 0;
            double boundary_sum = 0.0;
            for(int k = 0; k < 13; k++)
            {
              const int neighbour_y = CLAMP(coarse_y + stencil_dy[k], 0, coarse_h - 1);
              const int neighbour_x = CLAMP(coarse_x + stencil_dx[k], 0, coarse_w - 1);
              const size_t neighbour_i = (size_t)neighbour_y * coarse_w + neighbour_x;
              if(!coarse_hole[neighbour_i])
              {
                // Dirichlet boundary term: a non-hole neighbour is fixed data (u|dOmega = u_valid),
                // so its stencil contribution moves to the RHS as -weight * u_valid
                boundary_sum -= (double)stencil_weight[k] * coarse_field[neighbour_i];
                continue;
              }
              const int target = neighbour_y * coarse_w + neighbour_x;
              int slot = 0;
              for(; slot < count; slot++)
                if(targets[slot] == target)
                {
                  target_weights[slot] += stencil_weight[k];
                  break;
                }
              if(slot == count)
              {
                targets[count] = target;
                target_weights[count] = stencil_weight[k];
                count++;
              }
            }
            if(pass == 1) right_hand_side[perm_index] = boundary_sum;

            int n_col_entries = 0;
            for(int slot = 0; slot < count; slot++)
            {
              const int target_row = inverse_perm[coarse_index[targets[slot]]];
              if(target_row > perm_index) continue;
              // border rows: keep the row value (the dense solver's lower-triangle convention)
              const double value = target_weights[slot];
              if(pass == 1)
              {
                matrix_row_index[matrix_col_ptr[perm_index] + n_col_entries] = target_row;
                matrix_values[matrix_col_ptr[perm_index] + n_col_entries] = value;
              }
              n_col_entries++;
            }
            if(pass == 0) matrix_col_ptr[perm_index] = n_col_entries;
          }
        }

        if(success)
        {
          // solve the restricted biharmonic system A u = b (A = Delta^2 over the hole unknowns,
          // b = boundary_sum). A is symmetric positive-definite, so the sparse Cholesky applies
          // (SPD factorization annotated in common/solvers/sparse_cholesky.h); a DIRECT solve is
          // exact regardless of conditioning, unlike CG which stalls in float at kappa ~ L^4.
          _sp_chol_t *factor = _sp_chol_factor(n_unknowns, matrix_col_ptr, matrix_row_index, matrix_values, pipe);
          if(factor)
          {
            _sp_chol_solve(factor, right_hand_side);
            for(size_t coarse_i = 0; coarse_i < coarse_pixels; coarse_i++)
              if(coarse_hole[coarse_i])
                coarse_field[coarse_i] = (float)right_hand_side[(size_t)inverse_perm[coarse_index[coarse_i]]];
            solved = 1;
            _sp_chol_free(factor);
          }
        }
      }

      dt_pixelpipe_cache_free_align(unknown_x);
      dt_pixelpipe_cache_free_align(unknown_y);
      dt_pixelpipe_cache_free_align(permutation);
      dt_pixelpipe_cache_free_align(inverse_perm);
      dt_pixelpipe_cache_free_align(matrix_col_ptr);
      dt_pixelpipe_cache_free_align(matrix_row_index);
      dt_pixelpipe_cache_free_align(matrix_values);
      dt_pixelpipe_cache_free_align(right_hand_side);
    }

    if(!solved && n_unknowns <= DT_HL_DOME_NMAX)
    {
      // dense fallback (previous solver), only affordable on the small dense-era grids
      float *const restrict matrix = dt_pixelpipe_cache_alloc_align_float((size_t)n_unknowns * n_unknowns, pipe);
      float *const restrict right_hand_side = dt_pixelpipe_cache_alloc_align_float((size_t)n_unknowns, pipe);
      if(matrix && right_hand_side)
      {
        memset(matrix, 0, (size_t)n_unknowns * n_unknowns * sizeof(float));
        __OMP_PARALLEL_FOR__()
        for(int coarse_y = 0; coarse_y < coarse_h; coarse_y++)
          for(int coarse_x = 0; coarse_x < coarse_w; coarse_x++)
          {
            const size_t coarse_i = (size_t)coarse_y * coarse_w + coarse_x;
            if(!coarse_hole[coarse_i]) continue;
            const int unknown_index = coarse_index[coarse_i];
            float boundary_sum = 0.f;
            for(int k = 0; k < 13; k++)
            {
              const int neighbour_y = CLAMP(coarse_y + stencil_dy[k], 0, coarse_h - 1);
              const int neighbour_x = CLAMP(coarse_x + stencil_dx[k], 0, coarse_w - 1);
              const size_t neighbour_i = (size_t)neighbour_y * coarse_w + neighbour_x;
              if(coarse_hole[neighbour_i])
                matrix[(size_t)unknown_index * n_unknowns + coarse_index[neighbour_i]] += stencil_weight[k];
              else
                boundary_sum -= stencil_weight[k] * coarse_field[neighbour_i];
            }
            right_hand_side[unknown_index] = boundary_sum;
          }

        // direct SPD solve (dense Cholesky) of the same restricted Delta^2 u = 0 system, only for
        // the small dense-era grids. right_hand_side holds the solution on return.
        if(solve_hermitian(matrix, right_hand_side, (size_t)n_unknowns, TRUE) == 0)
        {
          for(size_t coarse_i = 0; coarse_i < coarse_pixels; coarse_i++)
            if(coarse_hole[coarse_i]) coarse_field[coarse_i] = right_hand_side[coarse_index[coarse_i]];
          solved = 1;
        }
      }
      dt_pixelpipe_cache_free_align(matrix);
      dt_pixelpipe_cache_free_align(right_hand_side);
    }

    if(!solved)
    {
      // last resort (OOM): fill the coarse hole with the anchor mean -- never leave the zeroed
      // hole cells to be upsampled as a black dome
      double anchor_sum = 0.0;
      size_t anchor_count = 0;
      for(size_t coarse_i = 0; coarse_i < coarse_pixels; coarse_i++)
        if(!coarse_hole[coarse_i])
        {
          anchor_sum += coarse_field[coarse_i];
          anchor_count++;
        }
      const float anchor_mean = anchor_count ? (float)(anchor_sum / (double)anchor_count) : 0.f;
      for(size_t coarse_i = 0; coarse_i < coarse_pixels; coarse_i++)
        if(coarse_hole[coarse_i]) coarse_field[coarse_i] = anchor_mean;
    }
  }

  // bilinear-upsample the coarse dome into the fine hole
  __OMP_PARALLEL_FOR__(collapse(2))
  for(int y = 0; y < region_h; y++)
    for(int x = 0; x < region_w; x++)
    {
      const size_t fine_index = (size_t)y * region_w + x;
      if(!hole[fine_index]) continue;
      const float grid_x = ((float)x + 0.5f) / downsample - 0.5f;
      const float grid_y = ((float)y + 0.5f) / downsample - 0.5f;
      const int x_lo = CLAMP((int)floorf(grid_x), 0, coarse_w - 1);
      const int y_lo = CLAMP((int)floorf(grid_y), 0, coarse_h - 1);
      const int x_hi = MIN(x_lo + 1, coarse_w - 1);
      const int y_hi = MIN(y_lo + 1, coarse_h - 1);
      const float frac_x = CLAMP(grid_x - x_lo, 0.f, 1.f);
      const float frac_y = CLAMP(grid_y - y_lo, 0.f, 1.f);
      const float interp_top = coarse_field[(size_t)y_lo * coarse_w + x_lo] * (1.f - frac_x)
                               + coarse_field[(size_t)y_lo * coarse_w + x_hi] * frac_x;
      const float interp_bottom = coarse_field[(size_t)y_hi * coarse_w + x_lo] * (1.f - frac_x)
                                  + coarse_field[(size_t)y_hi * coarse_w + x_hi] * frac_x;
      field[fine_index] = interp_top * (1.f - frac_y) + interp_bottom * frac_y;
    }

  dt_pixelpipe_cache_free_align(coarse_field);
  dt_pixelpipe_cache_free_align(coarse_hole);
  dt_pixelpipe_cache_free_align(coarse_index);
}

// ===== anisotropic chroma diffusion (structure-steered, coarse-to-fine) ======================
// The guided ladder recovers MAGNITUDE well but its chroma carries guide-flip seams and scale
// hand-off patches. Chromaticity (est_c / L) is a BOUNDED quantity, so interpolation is the right
// tool for it -- provided it flows ALONG image structure, never across it, or unrelated colours
// (warm horizon glow vs cool upper sky) mix into magenta. This implements the diffuse.c model on
// the region buffer: per-pixel diffusion tensor D = t x t + exp(-|grad L|/k) * g x g, where g is
// the unit gradient of the RECOVERED luminance (content!) and t its orthogonal (the isophote).
// Explicit iterations only travel ~sqrt(iters) pixels, so a COARSE-TO-FINE pyramid seeds the whole
// hole at the coarsest level first (the "unreached interior stays magenta" fix), like diffuse.c's
// multiscale scheme.
//
// MATHS BRIDGE -- Step 8 / E_chrominance anisotropic (article §"The optimization problem" term 3,
// §"Chrominance coherence", §"The saturation floors, as obstacles"): the whole block minimizes
// int_Omega grad(r_c)^T D grad(r_c) dOmega subject to the obstacle r_c >= c0/L_sum, whose
// Euler-Lagrange (unconstrained) is the divergence-form steered fill div(D grad r) = 0. D here is
// the structure-steered tensor built from the recovered luminance: gradient-dominant on a clean
// halo ramp (transport radially inward), isophote-dominant where a hard edge crosses (transport
// along level lines, never across a boundary). r = RGB/L_sum, recombined RGB = L_sum * r.

// ============================ OpenCL ============================

#if defined(HAVE_OPENCL) && DT_HL_SPARSE_SOLVE
cl_int _biharmonic_dome_cl(const int devid, void *gd_void, cl_mem field, cl_mem hole, const int region_w,
                           const int region_h, const int downsample, const dt_dev_pixelpipe_t *pipe)
{
  dt_iop_highlights_global_data_t *global_data = (dt_iop_highlights_global_data_t *)gd_void;
  const int coarse_w = (region_w + downsample - 1) / downsample;
  const int coarse_h = (region_h + downsample - 1) / downsample;
  const size_t coarse_pixels = (size_t)coarse_w * coarse_h;
  cl_int cl_err = DT_OPENCL_DEFAULT_ERROR;

  cl_mem dval = dt_opencl_alloc_device_buffer(devid, sizeof(float) * coarse_pixels);
  cl_mem dhole = dt_opencl_alloc_device_buffer(devid, coarse_pixels);
  float *cf = dt_pixelpipe_cache_alloc_align_float(coarse_pixels, pipe);
  uint8_t *coarse_hole = (uint8_t *)dt_pixelpipe_cache_alloc_align(coarse_pixels, pipe);
  int *idx = (int *)dt_pixelpipe_cache_alloc_align(sizeof(int) * coarse_pixels, pipe);
  _sp_chol_cl_t *factor = NULL;
  double *rhs = NULL;
  int *matrix_col_ptr = NULL, *matrix_row_index = NULL;
  double *matrix_values = NULL;
  cl_mem solution_device = NULL;
  if(!dval || !dhole || !cf || !coarse_hole || !idx) goto out;

  // coarse-grid reduction on device: average the full-res field/hole into the ds-downsampled grid
  {
    const int kernel = global_data->kernel_hl_dome_down;
    size_t work_size[3] = { ROUNDUPDWD(coarse_w, devid), ROUNDUPDHT(coarse_h, devid), 1 };
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &field);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &hole);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &dval);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &dhole);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &coarse_w);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &coarse_h);
    dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &downsample);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, work_size);
    if(cl_err != CL_SUCCESS) goto out;
  }

  // coarse metadata to host: assembly + symbolic analysis (integer work)
  cl_err = dt_opencl_read_buffer_from_device(devid, cf, dval, 0, sizeof(float) * coarse_pixels, CL_TRUE);
  if(cl_err != CL_SUCCESS) goto out;
  cl_err = dt_opencl_read_buffer_from_device(devid, coarse_hole, dhole, 0, coarse_pixels, CL_TRUE);
  if(cl_err != CL_SUCCESS) goto out;

  // number the coarse hole cells: these are the unknowns of the linear system
  int unknown_count = 0;
  for(size_t coarse_index = 0; coarse_index < coarse_pixels; coarse_index++)
    idx[coarse_index] = coarse_hole[coarse_index] ? unknown_count++ : -1;
  // Nh == 0 (no coarse cell reached hole majority -- thin streaks, speckle holes): skip the
  // solve but STILL upsample the coarse block means into the fine holes, exactly like the CPU
  // dome, whose bilinear upsample runs unconditionally. Early-exiting here left the field
  // untouched and diverged from the CPU on thin-hole topologies.
  if(unknown_count > 0)
  {

    {
      // assemble the 13-point biharmonic operator Delta^2 = Delta(Delta) (the 5-point Laplacian
      // convolved with itself: center 20, edge -8, diagonal 2, far-axis 1; reaches two rings out),
      // with the unknowns permuted by geometric nested dissection (the CPU dome's exact system)
      static const int stencil_off_y[13] = { 0, -1, 1, 0, 0, -1, -1, 1, 1, -2, 2, 0, 0 };
      static const int stencil_off_x[13] = { 0, 0, 0, -1, 1, -1, 1, -1, 1, 0, 0, -2, 2 };
      static const double stencil_coef[13] = { 20., -8., -8., -8., -8., 2., 2., 2., 2., 1., 1., 1., 1. };

      int *unknown_x = (int *)dt_pixelpipe_cache_alloc_align(sizeof(int) * unknown_count, pipe);
      int *unknown_y = (int *)dt_pixelpipe_cache_alloc_align(sizeof(int) * unknown_count, pipe);
      int *perm = (int *)dt_pixelpipe_cache_alloc_align(sizeof(int) * unknown_count, pipe);
      int *inv_perm = (int *)dt_pixelpipe_cache_alloc_align(sizeof(int) * unknown_count, pipe);
      matrix_col_ptr = (int *)dt_pixelpipe_cache_alloc_align(sizeof(int) * (unknown_count + 1), pipe);
      matrix_row_index = (int *)dt_pixelpipe_cache_alloc_align(sizeof(int) * (size_t)unknown_count * 13, pipe);
      matrix_values = (double *)dt_pixelpipe_cache_alloc_align(sizeof(double) * (size_t)unknown_count * 13, pipe);
      rhs = (double *)dt_pixelpipe_cache_alloc_align(sizeof(double) * unknown_count, pipe);
      int alloc_ok = (unknown_x && unknown_y && perm && inv_perm && matrix_col_ptr && matrix_row_index
                      && matrix_values && rhs);
      if(alloc_ok)
      {
        for(size_t coarse_index = 0; coarse_index < coarse_pixels; coarse_index++)
          if(coarse_hole[coarse_index])
          {
            unknown_x[idx[coarse_index]] = (int)(coarse_index % coarse_w);
            unknown_y[idx[coarse_index]] = (int)(coarse_index / coarse_w);
          }
        for(int i = 0; i < unknown_count; i++) perm[i] = i;
        _sp_nd_order(perm, unknown_count, unknown_x, unknown_y, 2);
        for(int perm_index = 0; perm_index < unknown_count; perm_index++) inv_perm[perm[perm_index]] = perm_index;

        int n_nonzero = 0;
        for(int perm_index = 0; perm_index < unknown_count; perm_index++)
        {
          const int cell_y = unknown_y[perm[perm_index]], cell_x = unknown_x[perm[perm_index]];
          matrix_col_ptr[perm_index] = n_nonzero;
          double rhs_accum = 0.0;
          for(int stencil = 0; stencil < 13; stencil++)
          {
            const int neighbor_y = CLAMP(cell_y + stencil_off_y[stencil], 0, coarse_h - 1);
            const int neighbor_x = CLAMP(cell_x + stencil_off_x[stencil], 0, coarse_w - 1);
            const size_t neighbor_index = (size_t)neighbor_y * coarse_w + neighbor_x;
            if(!coarse_hole[neighbor_index])
            {
              // Dirichlet boundary: a non-hole neighbour is fixed data (u|dOmega = u_valid), so its
              // stencil term moves to the RHS as -coef * u_valid
              rhs_accum -= stencil_coef[stencil] * cf[neighbor_index];
              continue;
            }
            const int row_index = inv_perm[idx[neighbor_index]];
            if(row_index > perm_index) continue;
            int fill_index = matrix_col_ptr[perm_index];
            for(; fill_index < n_nonzero; fill_index++)
              if(matrix_row_index[fill_index] == row_index)
              {
                matrix_values[fill_index] += stencil_coef[stencil];
                break;
              }
            if(fill_index == n_nonzero)
            {
              matrix_row_index[n_nonzero] = row_index;
              matrix_values[n_nonzero] = stencil_coef[stencil];
              n_nonzero++;
            }
          }
          rhs[perm_index] = rhs_accum;
        }
        matrix_col_ptr[unknown_count] = n_nonzero;

        // factor + solve A u = b, A = the restricted Delta^2 (SPD), b = the boundary_sum RHS:
        // the exact biharmonic dome on the coarse hole (GPU sparse Cholesky)
        factor = _sp_chol_factor_cl(devid, _hl_sp_chol_kernels(gd_void), unknown_count, matrix_col_ptr,
                                    matrix_row_index, matrix_values);
        int solved = 0;
        if(factor)
        {
          cl_mem rhs_device = _sp_cl_upload(devid, rhs, sizeof(double) * unknown_count);
          if(rhs_device && !_sp_chol_solve_cl(factor, _hl_sp_chol_kernels(gd_void), rhs_device)
             && dt_opencl_read_buffer_from_device(devid, rhs, rhs_device, 0, sizeof(double) * unknown_count,
                                                  CL_TRUE)
                    == CL_SUCCESS)
          {
            // the GPU factorization does not abort on a non-positive pivot the way the CPU
            // up-looking factor does -- it silently produces NaN/inf. Validate the solution
            // like the CPU validates the factor, and take the same fallback chain when the
            // clamped-border row-assembly breaks SPD on an unlucky hole topology.
            solved = 1;
            for(int k = 0; k < unknown_count && solved; k++)
              if(!isfinite(rhs[k])) solved = 0;
            if(solved)
              for(size_t coarse_index = 0; coarse_index < coarse_pixels; coarse_index++)
                if(coarse_hole[coarse_index]) cf[coarse_index] = (float)rhs[(size_t)inv_perm[idx[coarse_index]]];
          }
          dt_opencl_release_mem_object(rhs_device);
        }

        if(!solved && unknown_count <= DT_HL_DOME_NMAX)
        {
          // dense fallback, exactly the CPU dome's second stage
          float *const restrict dense_matrix
              = dt_pixelpipe_cache_alloc_align_float((size_t)unknown_count * unknown_count, pipe);
          float *const restrict dense_rhs = dt_pixelpipe_cache_alloc_align_float((size_t)unknown_count, pipe);
          if(dense_matrix && dense_rhs)
          {
            memset(dense_matrix, 0, (size_t)unknown_count * unknown_count * sizeof(float));
            for(int cell_y = 0; cell_y < coarse_h; cell_y++)
              for(int cell_x = 0; cell_x < coarse_w; cell_x++)
              {
                const size_t coarse_index = (size_t)cell_y * coarse_w + cell_x;
                if(!coarse_hole[coarse_index]) continue;
                const int k = idx[coarse_index];
                float rhs_accum = 0.f;
                for(int stencil = 0; stencil < 13; stencil++)
                {
                  const int neighbor_y = CLAMP(cell_y + stencil_off_y[stencil], 0, coarse_h - 1);
                  const int neighbor_x = CLAMP(cell_x + stencil_off_x[stencil], 0, coarse_w - 1);
                  const size_t neighbor_index = (size_t)neighbor_y * coarse_w + neighbor_x;
                  if(coarse_hole[neighbor_index])
                    dense_matrix[(size_t)k * unknown_count + idx[neighbor_index]] += stencil_coef[stencil];
                  else
                    rhs_accum -= stencil_coef[stencil] * cf[neighbor_index];
                }
                dense_rhs[k] = rhs_accum;
              }
            if(solve_hermitian(dense_matrix, dense_rhs, (size_t)unknown_count, TRUE) == 0)
            {
              for(size_t coarse_index = 0; coarse_index < coarse_pixels; coarse_index++)
                if(coarse_hole[coarse_index]) cf[coarse_index] = dense_rhs[idx[coarse_index]];
              solved = 1;
            }
          }
          dt_pixelpipe_cache_free_align(dense_matrix);
          dt_pixelpipe_cache_free_align(dense_rhs);
        }

        if(!solved)
        {
          // last resort, exactly the CPU dome's: anchor-mean fill (never upsample a black dome)
          double asum = 0.0;
          size_t acnt = 0;
          for(size_t coarse_index = 0; coarse_index < coarse_pixels; coarse_index++)
            if(!coarse_hole[coarse_index])
            {
              asum += cf[coarse_index];
              acnt++;
            }
          const float amean = acnt ? (float)(asum / (double)acnt) : 0.f;
          for(size_t coarse_index = 0; coarse_index < coarse_pixels; coarse_index++)
            if(coarse_hole[coarse_index]) cf[coarse_index] = amean;
        }
        cl_err = CL_SUCCESS;
      }
      else
        cl_err = DT_OPENCL_DEFAULT_ERROR;
      dt_pixelpipe_cache_free_align(unknown_x);
      dt_pixelpipe_cache_free_align(unknown_y);
      dt_pixelpipe_cache_free_align(perm);
      dt_pixelpipe_cache_free_align(inv_perm);
      if(cl_err != CL_SUCCESS) goto out;
    }
  }

  // upload the coarse solution and upsample into the full-res holes (hl_fill_up wants the
  // ANCHOR mask; our `hole` buffer holds holes, so pass an inverted... hl_fill_up tests
  // anc[i] -> skip: we need "write where hole": pass hole through a dedicated path -- reuse
  // hl_fill_up by noting its test `if(anc[i]) return` writes where the mask is ZERO: our hole
  // mask is 1 on holes -> invert on upload? Simplest: hl_fill_up writes where mask==0, so
  // pass the INVERTED hole mask... we don't have it on device. Use hl_dome_up = hl_fill_up
  // with the hole convention: kernel reuse trick -- write a tiny inverter is more code than
  // benefit; instead upload solution and run hl_fill_up with `anc` = a mask we build by one
  // extra kernel... For now: build the inverted mask on host (we HAVE ch/full-res? no, full
  // -res hole only on device). Add: reuse hl_fill_jacobi convention... -> dedicated kernel
  // exists: hl_fill_up(anc) -- we need anc = !hole full-res. One-line kernel would be
  // cleaner; reuse hl_lsb_hole? No. We add hl_not_mask below in basic.cl? To avoid another
  // kernel this call allocates an inverted mask via clEnqueue... keep it simple:
  //
  // PLAIN-WORDS SUMMARY of the design notes above: upload the coarse solution and upsample
  // it into the full-res holes. Mask-convention mismatch: hl_fill_up writes only where its
  // `anc` (anchor) mask is ZERO, i.e. it expects 1 = trusted / 0 = hole, while this function
  // receives `hole` with 1 = hole. The full-res inverted mask exists nowhere (host or
  // device), so invert `hole` once on device with the tiny hl_not_mask kernel and feed that
  // to hl_fill_up.
  {
    // inverted mask via a tiny kernel would be ideal; as the region planes also need the
    // anchor mask elsewhere, callers of _biharmonic_dome_cl pass `hole`; invert here once.
    solution_device = _sp_cl_upload(devid, cf, sizeof(float) * coarse_pixels);
    if(!solution_device)
    {
      cl_err = DT_OPENCL_DEFAULT_ERROR;
      goto out;
    }
    {
      // upsample the coarse dome into the full-resolution hole pixels; the mask is in the
      // hole convention (1 = fill), which hl_fill_up handles directly via mask_is_hole
      const int kernel = global_data->kernel_hl_fill_up;
      const int mask_is_hole = 1;
      size_t work_size[3] = { ROUNDUPDWD(region_w, devid), ROUNDUPDHT(region_h, devid), 1 };
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &field);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &hole);
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &solution_device);
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region_w);
      dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_h);
      dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &coarse_w);
      dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &coarse_h);
      dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &downsample);
      dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &mask_is_hole);
      cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, work_size);
    }
  }

out:
  dt_opencl_release_mem_object(dval);
  dt_opencl_release_mem_object(dhole);
  dt_opencl_release_mem_object(solution_device);
  dt_pixelpipe_cache_free_align(cf);
  dt_pixelpipe_cache_free_align(coarse_hole);
  dt_pixelpipe_cache_free_align(idx);
  dt_pixelpipe_cache_free_align(matrix_col_ptr);
  dt_pixelpipe_cache_free_align(matrix_row_index);
  dt_pixelpipe_cache_free_align(matrix_values);
  dt_pixelpipe_cache_free_align(rhs);
  _sp_chol_cl_free(factor);
  return cl_err;
}

#endif // HAVE_OPENCL && DT_HL_SPARSE_SOLVE
