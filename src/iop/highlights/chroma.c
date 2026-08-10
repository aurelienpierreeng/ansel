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

// Anisotropic (divergence-form) chrominance-coherence stage (CPU + OpenCL). (implementation; see chroma.h for the
// public API.)

#include "system/openmp.h"
#include "system/simd.h"
#include "system/target_clones.h"
#include "caches/pixelpipe_cache_alloc.h"
#include "iop/highlights/chroma.h"
#include "iop/highlights/pde.h"
#include <string.h>

__DT_CLONE_TARGETS__
void _aniso_tensor(const float *const restrict luminance, float *const restrict tensor_xx,
                   float *const restrict tensor_xy, float *const restrict tensor_yy, float *const restrict scratch,
                   const int region_w, const int region_h)
{
  const size_t region_pixels = (size_t)region_w * region_h;

  // two 3x3 box passes ~ small gaussian on the luminance, into scratch
  for(int pass = 0; pass < 2; pass++)
  {
    const float *const src = (pass == 0) ? luminance : tensor_xx;

    __OMP_PARALLEL_FOR__(collapse(2))
    for(int y = 0; y < region_h; y++)
      for(int x = 0; x < region_w; x++)
      {
        double accum = 0.0;
        int count = 0;

        for(int offset_y = -1; offset_y <= 1; offset_y++)
          for(int offset_x = -1; offset_x <= 1; offset_x++)
          {
            const int neighbour_y = CLAMP(y + offset_y, 0, region_h - 1);
            const int neighbour_x = CLAMP(x + offset_x, 0, region_w - 1);
            accum += src[(size_t)neighbour_y * region_w + neighbour_x];
            count++;
          }

        ((pass == 0) ? tensor_xx : scratch)[(size_t)y * region_w + x] = (float)(accum / count);
      }
  }

  // mean gradient magnitude of the blurred luminance = the anisotropy normalisation
  double grad_sum = 0.0;

  __OMP_PARALLEL_FOR__(collapse(2) reduction(+ : grad_sum))
  for(int y = 0; y < region_h; y++)
    for(int x = 0; x < region_w; x++)
    {
      const int x_lo = MAX(x - 1, 0), x_hi = MIN(x + 1, region_w - 1);
      const int y_lo = MAX(y - 1, 0), y_hi = MIN(y + 1, region_h - 1);
      const float grad_x = 0.5f * (scratch[(size_t)y * region_w + x_hi] - scratch[(size_t)y * region_w + x_lo]);
      const float grad_y = 0.5f * (scratch[(size_t)y_hi * region_w + x] - scratch[(size_t)y_lo * region_w + x]);
      tensor_xx[(size_t)y * region_w + x] = grad_x; // stash gradients temporarily
      tensor_xy[(size_t)y * region_w + x] = grad_y;
      grad_sum += dt_fast_hypotf(grad_x, grad_y);
    }

  const float grad_mean = fmaxf((float)(grad_sum / (double)region_pixels), 1e-9f);

  // D = isophote outer product + damped gradient outer product (k = 4 mean-gradients crossover)
  __OMP_PARALLEL_FOR__()
  for(size_t i = 0; i < region_pixels; i++)
  {
    const float grad_x = tensor_xx[i];
    const float grad_y = tensor_xy[i];
    const float grad_mag = dt_fast_hypotf(grad_x, grad_y);
    const float nonzero = (grad_mag > 1e-12f) ? 1.f : 0.f;
    const float inv_mag = nonzero / (grad_mag + (1.f - nonzero));
    const float grad_unit_x = grad_x * inv_mag + (1.f - nonzero);    // cos(theta_grad)
    const float grad_unit_y = grad_y * inv_mag;                      // sin(theta_grad)
    const float cross_damp = expf(-grad_mag / (4.f * grad_mean));    // c2 = exp(-|grad L|/(4 <|grad L|>))
    const float isophote_x = -grad_unit_y, isophote_y = grad_unit_x; // t = g rotated 90 deg (level line)

    // D = t t^T + c2 * g g^T (isophote outer product + damped gradient outer product)
    tensor_xx[i] = isophote_x * isophote_x + cross_damp * grad_unit_x * grad_unit_x;
    tensor_xy[i] = isophote_x * isophote_y + cross_damp * grad_unit_x * grad_unit_y;
    tensor_yy[i] = isophote_y * isophote_y + cross_damp * grad_unit_y * grad_unit_y;
  }
}

__DT_CLONE_TARGETS__
void _aniso_iterate_obs(float *const restrict field, const float *const restrict obstacle,
                        const uint8_t *const restrict hole, const float *const restrict tensor_xx,
                        const float *const restrict tensor_xy, const float *const restrict tensor_yy,
                        float *const restrict tmp, const int region_w, const int region_h, const int iters,
                        const int box_x_lo, const int box_y_lo, const int box_x_hi, const int box_y_hi,
                        const float react, const float react_target)
{
  // project the seed once so the first sweep already sees an admissible field
  // (r <- max(r, obstacle), obstacle = c0/L in ratio space -- the saturation floor)
  __OMP_PARALLEL_FOR__(collapse(2))
  for(int y = box_y_lo; y <= box_y_hi; y++)
    for(int x = box_x_lo; x <= box_x_hi; x++)
    {
      const size_t i = (size_t)y * region_w + x;
      if(hole[i]) field[i] = fmaxf(field[i], obstacle[i]);
    }

  for(int iter = 0; iter < iters; iter++)
  {
    __OMP_PARALLEL_FOR__(collapse(2))
    for(int y = box_y_lo; y <= box_y_hi; y++)
      for(int x = box_x_lo; x <= box_x_hi; x++)
      {
        const size_t i = (size_t)y * region_w + x;

        if(!hole[i])
        {
          tmp[i] = field[i];
          continue;
        }

        const int x_lo = MAX(x - 1, 0), x_hi = MIN(x + 1, region_w - 1);
        const int y_lo = MAX(y - 1, 0), y_hi = MIN(y + 1, region_h - 1);
        const float center = field[i];
        // second differences of r: d_xx r, d_yy r, and the mixed d_xy r (the Hessian of r)
        const float d2_xx = field[(size_t)y * region_w + x_hi] - 2.f * center + field[(size_t)y * region_w + x_lo];
        const float d2_yy = field[(size_t)y_hi * region_w + x] - 2.f * center + field[(size_t)y_lo * region_w + x];
        const float d2_xy = 0.25f
                            * (field[(size_t)y_hi * region_w + x_hi] - field[(size_t)y_hi * region_w + x_lo]
                               - field[(size_t)y_lo * region_w + x_hi] + field[(size_t)y_lo * region_w + x_lo]);

        // r <- max( r + 0.18*(D_xx d_xx r + 2 D_xy d_xy r + D_yy d_yy r) - 0.18*react*(r - target),
        // obstacle ): explicit trace-form step tr(D Hess r), the screened reaction of the
        // "inpaint a flat color" user parameter (lambda_solid pulls the core chroma toward the
        // mean valid colour -- same semantics as the joint core's screened-Poisson solve, applied
        // here because THIS stage owns the final all-clip chroma), then the obstacle projection
        // (article Step 8 update rule).
        tmp[i] = fmaxf(center + 0.18f * (tensor_xx[i] * d2_xx + 2.f * tensor_xy[i] * d2_xy + tensor_yy[i] * d2_yy)
                           - 0.18f * react * (center - react_target),
                       obstacle[i]);
      }

    __OMP_PARALLEL_FOR__()
    for(int y = box_y_lo; y <= box_y_hi; y++)
      memcpy(field + (size_t)y * region_w + box_x_lo, tmp + (size_t)y * region_w + box_x_lo,
             (size_t)(box_x_hi - box_x_lo + 1) * sizeof(float));
  }
}

int _aniso_div_solve(float *const restrict ratios, const float *const restrict valid,
                     const float *const restrict luminance, float *const restrict scratch_planes,
                     const int region_w, const int region_h, const float react,
                     const dt_aligned_pixel_t react_target, const dt_dev_pixelpipe_t *pipe)
{
  const size_t region_pixels = (size_t)region_w * region_h;
  float *const restrict tensor_xx = scratch_planes;
  float *const restrict tensor_xy = scratch_planes + region_pixels;
  float *const restrict tensor_yy = scratch_planes + 2 * region_pixels;
  float *const restrict tensor_scratch = scratch_planes + 3 * region_pixels;

  // the three channels must share one hole (all-clip core); bail out otherwise
  int n_unknowns = 0;
  for(size_t i = 0; i < region_pixels; i++)
  {
    const int is_hole = (valid[i * 4 + 0] < 0.5f);
    if(is_hole != (valid[i * 4 + 1] < 0.5f) || is_hole != (valid[i * 4 + 2] < 0.5f)) return 0;
    n_unknowns += is_hole;
  }
  if(n_unknowns == 0) return 1;
  if(n_unknowns > DT_HL_SPARSE_MAX) return 0;

  _aniso_tensor(luminance, tensor_xx, tensor_xy, tensor_yy, tensor_scratch, region_w, region_h);

  int *grid_to_unknown = (int *)dt_pixelpipe_cache_alloc_align(sizeof(int) * region_pixels, pipe);
  int *unknown_to_grid = (int *)dt_pixelpipe_cache_alloc_align(sizeof(int) * n_unknowns, pipe);
  int *unknown_x = (int *)dt_pixelpipe_cache_alloc_align(sizeof(int) * n_unknowns, pipe);
  int *unknown_y = (int *)dt_pixelpipe_cache_alloc_align(sizeof(int) * n_unknowns, pipe);
  int *permutation = (int *)dt_pixelpipe_cache_alloc_align(sizeof(int) * n_unknowns, pipe);
  int *inverse_perm = (int *)dt_pixelpipe_cache_alloc_align(sizeof(int) * n_unknowns, pipe);
  int *matrix_col_ptr = (int *)dt_pixelpipe_cache_alloc_align(sizeof(int) * (n_unknowns + 1), pipe);
  double *right_hand_side
      = (double *)dt_pixelpipe_cache_alloc_align(sizeof(double) * (size_t)n_unknowns * 3, pipe);
  int *matrix_row_index = NULL;
  double *matrix_values = NULL;
  int success = (grid_to_unknown && unknown_to_grid && unknown_x && unknown_y && permutation && inverse_perm
                 && matrix_col_ptr && right_hand_side);

  if(success)
  {
    int unknown_index = 0;
    for(size_t i = 0; i < region_pixels; i++)
    {
      const int is_hole = (valid[i * 4 + 0] < 0.5f);
      grid_to_unknown[i] = is_hole ? unknown_index : -1;
      if(is_hole)
      {
        unknown_to_grid[unknown_index] = (int)i;
        unknown_y[unknown_index] = (int)(i / region_w);
        unknown_x[unknown_index] = (int)(i - (size_t)unknown_y[unknown_index] * region_w);
        unknown_index++;
      }
    }

    for(int i = 0; i < n_unknowns; i++) permutation[i] = i;
    _sp_nd_order(permutation, n_unknowns, unknown_x, unknown_y, 1);
    for(int perm_index = 0; perm_index < n_unknowns; perm_index++)
      inverse_perm[permutation[perm_index]] = perm_index;

    static const int neighbour_dy[8] = { 0, 0, -1, 1, -1, 1, -1, 1 };
    static const int neighbour_dx[8] = { -1, 1, 0, 0, -1, 1, 1, -1 };

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
        if(!matrix_row_index || !matrix_values)
          success = 0;
        else
          memset(right_hand_side, 0, sizeof(double) * (size_t)n_unknowns * 3);
      }

      for(int perm_index = 0; perm_index < n_unknowns && success; perm_index++)
      {
        const int origin_grid = unknown_to_grid[permutation[perm_index]];
        const int origin_y = origin_grid / region_w;
        const int origin_x = origin_grid - origin_y * region_w;
        double diagonal = 0.0;
        int n_col_entries = 0;

        for(int edge = 0; edge < 8; edge++)
        {
          const int neighbour_x = origin_x + neighbour_dx[edge];
          const int neighbour_y = origin_y + neighbour_dy[edge];
          // note: at the region border, missing neighbours simply drop out (no-flux boundary)
          if(neighbour_x < 0 || neighbour_y < 0 || neighbour_x >= region_w || neighbour_y >= region_h)
            continue; // Neumann at the region border
          const size_t j = (size_t)neighbour_y * region_w + neighbour_x;
          const float weight = _aniso_edge_w(tensor_xx, tensor_xy, tensor_yy, (size_t)origin_grid, j,
                                             neighbour_dx[edge], neighbour_dy[edge]); // w_ij >= 0
          if(weight <= 0.f) continue;
          diagonal += weight; // diagonal = sum_j w_ij (graph-Laplacian row sum)

          if(grid_to_unknown[j] >= 0)
          {
            const int target_row = inverse_perm[grid_to_unknown[j]];
            if(target_row < perm_index)
            {
              if(pass == 1)
              {
                matrix_row_index[matrix_col_ptr[perm_index] + n_col_entries] = target_row;
                matrix_values[matrix_col_ptr[perm_index] + n_col_entries]
                    = -(double)weight; // off-diagonal A_ij = -w_ij
              }
              n_col_entries++;
            }
          }
          else if(pass == 1)
            // Dirichlet neighbour (rim, not an unknown): its fixed r_valid moves to the RHS as
            // +w_ij * r_valid_j, one per colour channel (same matrix, three right-hand sides)
            for(int c = 0; c < 3; c++)
              right_hand_side[(size_t)c * n_unknowns + perm_index] += (double)weight * ratios[j * 4 + c];
        }

        // diagonal last (any order works: columns need not be sorted). The screened reaction of
        // the "inpaint a flat color" user parameter adds lambda_solid to the diagonal (and
        // lambda_solid * target to each channel's RHS below): (lambda I + Op) r = lambda target
        // + boundary terms -- the same semantics as the joint core's screened-Poisson solve,
        // applied here because THIS stage owns the final all-clip chroma.
        if(pass == 1)
        {
          matrix_row_index[matrix_col_ptr[perm_index] + n_col_entries] = perm_index;
          matrix_values[matrix_col_ptr[perm_index] + n_col_entries] = diagonal + (double)react;
          if(react > 0.f)
            for(int c = 0; c < 3; c++)
              right_hand_side[(size_t)c * n_unknowns + perm_index] += (double)react * react_target[c];
        }
        n_col_entries++;
        if(pass == 0) matrix_col_ptr[perm_index] = n_col_entries;
      }
    }

    if(success)
    {
      _sp_chol_t *factor = _sp_chol_factor(n_unknowns, matrix_col_ptr, matrix_row_index, matrix_values, pipe->type);
      if(factor)
      {
        for(int c = 0; c < 3; c++)
        {
          _sp_chol_solve(factor, right_hand_side + (size_t)c * n_unknowns);
          for(int perm_index = 0; perm_index < n_unknowns; perm_index++)
            ratios[(size_t)unknown_to_grid[permutation[perm_index]] * 4 + c]
                = (float)right_hand_side[(size_t)c * n_unknowns + perm_index];
        }
        _sp_chol_free(factor);
      }
      else
        success = 0;
    }
  }

  dt_pixelpipe_cache_free_align(grid_to_unknown);
  dt_pixelpipe_cache_free_align(unknown_to_grid);
  dt_pixelpipe_cache_free_align(unknown_x);
  dt_pixelpipe_cache_free_align(unknown_y);
  dt_pixelpipe_cache_free_align(permutation);
  dt_pixelpipe_cache_free_align(inverse_perm);
  dt_pixelpipe_cache_free_align(matrix_col_ptr);
  dt_pixelpipe_cache_free_align(matrix_row_index);
  dt_pixelpipe_cache_free_align(matrix_values);
  dt_pixelpipe_cache_free_align(right_hand_side);
  return success;
}

__DT_CLONE_TARGETS__
void _aniso_chroma(_hl_region_ctx_t *const ctx)
{
  const _hl_region_t *const region = ctx->region;
  const dt_dev_pixelpipe_t *const pipe = ctx->pipe;
  const int region_w = ctx->region_w;
  const int region_h = ctx->region_h;
  const size_t region_pixels = ctx->region_pixels;
  const float epsilon = ctx->epsilon;
  float *const restrict estimate = ctx->estimate;
  float *const restrict prev_scale = ctx->prev_scale;
  float *const restrict valid = ctx->valid;
  float *const restrict blur_in = ctx->blur_in;
  float *const restrict plane1 = ctx->plane1;
  float *const restrict clip0 = ctx->clip0;
  uint8_t *const restrict hole = ctx->hole;
  float *const restrict solver_field = ctx->solver_field;
  float *const restrict lum_accum = ctx->lum_accum;
  float *const restrict reaction_weight = ctx->reaction_weight;
  float *const restrict flat_target = ctx->flat_target;

  // --- uncertainty-aware biharmonic seam regulariser (fix_prototype.py _weighted_solve) ---
  // The steps above recover magnitude well but leave SEAMS where the method changes: the
  // guide-flip on decorrelated content, and above all the all-clip-core <-> partial-clip
  // handoff (a bright joint-core dome meeting an under-estimated single-guide reconstruction).
  // No confidence weight can hide a discontinuity in the thing it weights, so iron the seams
  // out afterwards: per channel solve (diag(Wd) + lambda*Delta^2) u = diag(Wd)*rec over the
  // any-clip region, with the finished reconstruction as both the data target and the initial
  // guess, and Wd = Wc^2 (= R^4) the fidelity weight. Where the recon is trustworthy (Wd high)
  // u = rec is preserved; where it is not (Wd low: seams, decorrelated, all-clip core) the
  // biharmonic prior flattens the seam's CURVATURE spike while preserving smooth domes and
  // gradients (a harmonic prior would over-smooth them). Magnitude is preserved because the
  // target is the recon itself. Full-res CG: the dome is already built, so the solve only has
  // to relax the localised seams -- no dome-building stall. See the companion article.

  // --- structure-steered chroma: diffuse the clipped channels' ratios est_c/L along the
  //     isophotes of the recovered luminance, coarse-to-fine (pyramid) so the whole hole is
  //     seeded before refinement. Magnitude (the norm L) is untouched: only direction changes.
  //
  // MATHS BRIDGE -- Step 8 chrominance coherence (article §"Chrominance coherence", the
  // anisotropic chroma pass): minimize E_chrominance = int_Omega grad(r)^T D grad(r) dOmega
  // subject to r_c >= c0/L_sum, Euler-Lagrange div(D grad r) = 0, D structure-steered. Restricted
  // to the all-clip pixels; the coefficient-field results act as Dirichlet anchors. Solver picked
  // by size: _aniso_div_solve (direct, small cores) or the coarse-to-fine _aniso_iterate_obs
  // pyramid (large cores), then a full-res projected polish. Reassembly RGB = L_sum * r.
  {
    // the aniso pass must not rewrite the coefficient-field estimates: only the guide-less
    // all-clip core diffuses, and the coefficient-field pixels act as valid anchors
    // vld_an: all-clip pixels keep valid < 0.5 (they diffuse); every other pixel is promoted to
    // an anchor (validity raised to >= 0.6), so div(D grad r)=0 sees them as fixed Dirichlet data
    HL_PFOR()
    for(size_t i = 0; i < region_pixels; i++)
    {
      const int allc = (valid[i * 4 + 0] < 0.5f && valid[i * 4 + 1] < 0.5f && valid[i * 4 + 2] < 0.5f);

      for(int c = 0; c < 4; c++) 
        prev_scale[i * 4 + c] = allc ? valid[i * 4 + c] : fmaxf(valid[i * 4 + c], 0.6f);
    }

    const float *const restrict vld_an = prev_scale;

    // fine-level luminance and per-channel ratios (ratio planes packed in s1's 4-ch layout)
    // L_sum = R+G+B, r_c = est_c / L_sum: the split of magnitude from chrominance (step 8 diffuses
    // only r; L_sum is left untouched and re-multiplied back at the reassembly)
    __OMP_PARALLEL_FOR__()
    for(size_t i = 0; i < region_pixels; i++)
    {
      const float lum_val = fmaxf(estimate[i * 4 + 0] + estimate[i * 4 + 1] + estimate[i * 4 + 2], epsilon);
      lum_accum[i] = lum_val;

      for(int c = 0; c < 3; c++) 
        plane1[i * 4 + c] = estimate[i * 4 + c] / lum_val;
    }

    // unknowns and their bounding box: the diffusion only ever writes pixels with
    // vld_an < 0.5 (the all-clip core in coefficient-field mode)
    size_t n_aniso = 0;
    int abx0 = region_w, aby0 = region_h, abx1 = -1, aby1 = -1;
    for(int y = 0; y < region_h; y++)
      for(int x = 0; x < region_w; x++)
      {
        const size_t i = (size_t)y * region_w + x;
        if(vld_an[i * 4 + 0] < 0.5f || vld_an[i * 4 + 1] < 0.5f || vld_an[i * 4 + 2] < 0.5f)
        {
          n_aniso++;
          abx0 = MIN(abx0, x);
          abx1 = MAX(abx1, x);
          aby0 = MIN(aby0, y);
          aby1 = MAX(aby1, y);
        }
      }

    int aniso_done = 0;
    if(n_aniso == 0) aniso_done = 1; // nothing to diffuse: skip the whole machinery

    // "inpaint a flat color": the screened reaction lambda_solid = solid_color^2 * 4 pulls the
    // all-clip chroma toward the mean valid chromaticity. It must live in THIS stage's solves:
    // the joint core applies the same reaction, but this stage re-solves the all-clip interior
    // afterwards (direct solve or pyramid, both anchor-determined), so a reaction applied only
    // there never reaches the output -- the user parameter was dead from release until this fix.
    const float react = ctx->solid_color * ctx->solid_color * 4.f;
    dt_aligned_pixel_t react_target = { 0.f, 0.f, 0.f, 0.f };
    if(react > 0.f && !aniso_done)
    {
      double target_accum[3] = { 0.0, 0.0, 0.0 };
      double target_count = 0.0;
      for(size_t i = 0; i < region_pixels; i++)
      {
        if(!(valid[i * 4 + 0] >= 0.5f && valid[i * 4 + 1] >= 0.5f && valid[i * 4 + 2] >= 0.5f)) continue;
        for(int c = 0; c < 3; c++) target_accum[c] += (double)plane1[i * 4 + c];
        target_count += 1.0;
      }
      if(target_count > 0.0)
        for(int c = 0; c < 3; c++) react_target[c] = (float)(target_accum[c] / target_count);
    }

    // primary Step-8 estimator: exact div(D grad r)=0 direct solve (returns 0 -> fall back to
    // the coarse-to-fine pyramid below for cores too large for the sparse Cholesky)
    if(!aniso_done)
      aniso_done = _aniso_div_solve(plane1, vld_an, lum_accum, blur_in, region_w, region_h, react,
                                    react_target, pipe);

    // pyramid depth: halve until the deepest hole spans ~8 px at the coarsest level
    int nlev = 1;

    while(((int)region->radius >> (nlev - 1)) > 8 && nlev < 7) nlev++;

    // coarse -> fine; each level diffuses each channel's ratio over ITS clipped mask, then the
    // result seeds the next finer level's hole pixels. Explicit iterations travel only
    // ~sqrt(iters) px, so the coarsest level fills the whole hole first (the "unreached interior
    // stays magenta" fix) -- the multiscale seeding of the div(D grad r)=0 fill for large cores.
    if(!aniso_done)
      for(int level = nlev - 1; level >= 0; level--)
      {
        const int step = 1 << level;
        const int down_w = (region_w + step - 1) / step;
        const int down_h = (region_h + step - 1) / step;
        const size_t down_pixels = (size_t)down_w * down_h;
        float *const restrict dome_L = dt_pixelpipe_cache_alloc_align_float(down_pixels, pipe);
        float *const restrict dome_ratio = dt_pixelpipe_cache_alloc_align_float(down_pixels * 3, pipe);
        float *const restrict tensor_xx = dt_pixelpipe_cache_alloc_align_float(down_pixels, pipe);
        float *const restrict tensor_xy = dt_pixelpipe_cache_alloc_align_float(down_pixels, pipe);
        float *const restrict tensor_yy = dt_pixelpipe_cache_alloc_align_float(down_pixels, pipe);
        float *const restrict tensor_scratch = dt_pixelpipe_cache_alloc_align_float(down_pixels, pipe);
        float *const restrict dobs = dt_pixelpipe_cache_alloc_align_float(down_pixels * 3, pipe);
        float *const restrict dobc = dt_pixelpipe_cache_alloc_align_float(down_pixels, pipe);
        uint8_t *const restrict dhole
            = (uint8_t *)dt_pixelpipe_cache_alloc_align(sizeof(uint8_t) * down_pixels * 3, ctx->pipe);
        uint8_t *const restrict hplane
            = (uint8_t *)dt_pixelpipe_cache_alloc_align(sizeof(uint8_t) * down_pixels, ctx->pipe);

        if(!dome_L || !dome_ratio || !tensor_xx || !tensor_xy || !tensor_yy || !tensor_scratch || !dobs || !dobc
           || !dhole || !hplane)
        {
          dt_pixelpipe_cache_free_align(dome_L);
          dt_pixelpipe_cache_free_align(dome_ratio);
          dt_pixelpipe_cache_free_align(tensor_xx);
          dt_pixelpipe_cache_free_align(tensor_xy);
          dt_pixelpipe_cache_free_align(tensor_yy);
          dt_pixelpipe_cache_free_align(tensor_scratch);
          dt_pixelpipe_cache_free_align(dobs);
          dt_pixelpipe_cache_free_align(dobc);
          dt_pixelpipe_cache_free_align(dhole);
          dt_pixelpipe_cache_free_align(hplane);
          break;
        }

        // box-downsample: luminance = cell mean; ratio = mean over the cell (current estimate,
        // already seeded by the coarser level); hole = majority of the cell clipped
        __OMP_PARALLEL_FOR__(collapse(2))
        for(int cell_y = 0; cell_y < down_h; cell_y++)
          for(int cell_x = 0; cell_x < down_w; cell_x++)
          {
            double accL = 0.0;
            double accr[3] = { 0.0, 0.0, 0.0 };
            int n_unknowns[3] = { 0, 0, 0 };
            int n_total = 0;

            double accc[3] = { 0.0, 0.0, 0.0 };
            for(int nb_y = cell_y * step; nb_y < MIN((cell_y + 1) * step, region_h); nb_y++)
              for(int nb_x = cell_x * step; nb_x < MIN((cell_x + 1) * step, region_w); nb_x++)
              {
                const size_t fine_index = (size_t)nb_y * region_w + nb_x;
                accL += lum_accum[fine_index];
                n_total++;

                for(int c = 0; c < 3; c++)
                {
                  accr[c] += plane1[fine_index * 4 + c];
                  accc[c] += clip0[fine_index * 4 + c];
                  n_unknowns[c] += (vld_an[fine_index * 4 + c] < 0.5f);
                }
              }

            const size_t cell_index = (size_t)cell_y * down_w + cell_x;
            dome_L[cell_index] = (float)(accL / n_total);

            for(int c = 0; c < 3; c++)
            {
              dome_ratio[cell_index * 3 + c] = (float)(accr[c] / n_total);
              // per-cell obstacle: the saturation floor in ratio space, clip0_c / L
              dobs[cell_index * 3 + c] = (float)(accc[c] / fmax(accL, 1e-9));
              dhole[cell_index * 3 + c] = (2 * n_unknowns[c] > n_total) ? 1 : 0;
            }
          }

        // structure tensor D of this level's luminance, then diffuse each channel's ratio plane
        // under the obstacle (per-level projected relaxation of div(D grad r)=0, r >= c0/L)
        _aniso_tensor(dome_L, tensor_xx, tensor_xy, tensor_yy, tensor_scratch, down_w, down_h);

        const int box_x_lo = MAX(abx0 / step - 2, 0), box_y_lo = MAX(aby0 / step - 2, 0);
        const int box_x_hi = MIN(abx1 / step + 2, down_w - 1), box_y_hi = MIN(aby1 / step + 2, down_h - 1);

        for(int c = 0; c < 3; c++)
        {
          size_t n_channels = 0;
          __OMP_PARALLEL_FOR__(reduction(+ : n_channels))
          for(size_t cell_index = 0; cell_index < down_pixels; cell_index++)
          {
            dome_L[cell_index] = dome_ratio[cell_index * 3 + c]; // reuse dL as the working plane for channel c
            dobc[cell_index] = dobs[cell_index * 3 + c];
            hplane[cell_index] = dhole[cell_index * 3 + c];
            n_channels += hplane[cell_index];
          }

          if(n_channels == 0) continue; // no hole cell at this level for this channel

          _aniso_iterate_obs(dome_L, dobc, hplane, tensor_xx, tensor_xy, tensor_yy, tensor_scratch, down_w, down_h,
                             240, box_x_lo, box_y_lo, box_x_hi, box_y_hi, 0.f, 0.f);

          __OMP_PARALLEL_FOR__()
          for(size_t cell_index = 0; cell_index < down_pixels; cell_index++)
            dome_ratio[cell_index * 3 + c] = dome_L[cell_index];
        }

        // splat this level's hole ratios back into the fine planes (bilinear prolongation),
        // seeding the next finer level; valid fine pixels keep their true ratios (anchors)
        __OMP_PARALLEL_FOR__(collapse(2))
        for(int y = 0; y < region_h; y++)
          for(int x = 0; x < region_w; x++)
          {
            const size_t fine_index = (size_t)y * region_w + x;
            const float grad_x = ((float)x + 0.5f) / step - 0.5f;
            const float grad_y = ((float)y + 0.5f) / step - 0.5f;
            const int x_lo = CLAMP((int)floorf(grad_x), 0, down_w - 1);
            const int y_lo = CLAMP((int)floorf(grad_y), 0, down_h - 1);
            const int x_hi = MIN(x_lo + 1, down_w - 1);
            const int y_hi = MIN(y_lo + 1, down_h - 1);
            const float frac_x = CLAMP(grad_x - x_lo, 0.f, 1.f);
            const float frac_y = CLAMP(grad_y - y_lo, 0.f, 1.f);

            for(int c = 0; c < 3; c++)
            {
              if(vld_an[fine_index * 4 + c] >= 0.5f) continue;

              const float interp_a = dome_ratio[((size_t)y_lo * down_w + x_lo) * 3 + c] * (1.f - frac_x)
                                     + dome_ratio[((size_t)y_lo * down_w + x_hi) * 3 + c] * frac_x;
              const float interp_b = dome_ratio[((size_t)y_hi * down_w + x_lo) * 3 + c] * (1.f - frac_x)
                                     + dome_ratio[((size_t)y_hi * down_w + x_hi) * 3 + c] * frac_x;
              plane1[fine_index * 4 + c] = interp_a * (1.f - frac_y) + interp_b * frac_y;
            }
          }

        dt_pixelpipe_cache_free_align(dome_L);
        dt_pixelpipe_cache_free_align(dome_ratio);
        dt_pixelpipe_cache_free_align(tensor_xx);
        dt_pixelpipe_cache_free_align(tensor_xy);
        dt_pixelpipe_cache_free_align(tensor_yy);
        dt_pixelpipe_cache_free_align(tensor_scratch);
        dt_pixelpipe_cache_free_align(dobs);
        dt_pixelpipe_cache_free_align(dobc);
        dt_pixelpipe_cache_free_align(dhole);
        dt_pixelpipe_cache_free_align(hplane);
      }

    // Full-resolution projected polish, both solver paths (the direct solve cannot project
    // mid-solve, and the pyramid's finest sweeps only correct locally): a short obstacle-
    // projected relaxation at full resolution lets the field settle smoothly around the
    // active set of the constraint.
    if(n_aniso > 0)
    {
      HL_PFOR()
      for(size_t i = 0; i < region_pixels; i++)
        hole[i] = (vld_an[i * 4 + 0] < 0.5f && vld_an[i * 4 + 1] < 0.5f && vld_an[i * 4 + 2] < 0.5f);

      // Activity gate: the polish exists to settle the field around the ACTIVE set of the
      // obstacle. Where no all-clip pixel sits at (or below) its obstacle, the projection
      // never fires and the 60 sweeps only re-run a diffusion the solvers already
      // converged -- skip them. The 1.001 band catches pixels the pyramid projection left
      // exactly ON the obstacle.
      int act0 = 0, act1 = 0, act2 = 0;
      HL_PFOR(reduction(| : act0, act1, act2))
      for(size_t i = 0; i < region_pixels; i++)
      {
        if(!hole[i]) continue;
        const float invL = 1.f / fmaxf(lum_accum[i], epsilon);
        act0 |= (plane1[i * 4 + 0] <= clip0[i * 4 + 0] * invL * 1.001f);
        act1 |= (plane1[i * 4 + 1] <= clip0[i * 4 + 1] * invL * 1.001f);
        act2 |= (plane1[i * 4 + 2] <= clip0[i * 4 + 2] * invL * 1.001f);
      }
      // the reaction changes the fixed point everywhere in the core, not just at the active
      // set of the obstacle: with lambda_solid > 0 the polish must always run
      const int react_on = (react > 0.f);
      const int active[3] = { act0 | react_on, act1 | react_on, act2 | react_on };

      if(act0 | act1 | act2 | react_on)
      {
        float *const restrict otxx = blur_in + 0 * region_pixels; // `in` (rn*4) is free scratch here
        float *const restrict otxy = blur_in + 1 * region_pixels;
        float *const restrict otyy = blur_in + 2 * region_pixels;
        float *const restrict otsc = blur_in + 3 * region_pixels;
        _aniso_tensor(lum_accum, otxx, otxy, otyy, otsc, region_w, region_h);

        for(int c = 0; c < 3; c++)
        {
          if(!active[c]) continue;

          HL_PFOR()
          for(size_t i = 0; i < region_pixels; i++)
          {
            solver_field[i] = plane1[i * 4 + c];
            reaction_weight[i] = clip0[i * 4 + c] / fmaxf(lum_accum[i], epsilon); // the obstacle
          }

          _aniso_iterate_obs(solver_field, reaction_weight, hole, otxx, otxy, otyy, flat_target, region_w,
                             region_h, 60, abx0, aby0, abx1, aby1, react, react_target[c]);

          HL_PFOR()
          for(size_t i = 0; i < region_pixels; i++) plane1[i * 4 + c] = solver_field[i];
        }
      }
    }

    // reassemble. This pass only ever writes the all-clip core (vld_an flags every channel
    // of a partially-valid pixel >= 0.6, so those pixels are anchors, settled by the
    // coefficient-field stages): the magnitude is the dome luminance L split by the
    // diffused ratios. (A ladder-era magnitude-transfer branch for partially-valid pixels
    // used to live here; the anchor construction made it unreachable and it was removed.)
    // SOFT saturation floor on the way out (same rounding as the coefficient-field floor): the
    // hard max() prints an exactly-flat shelf at the clip level plus a gradient kink wherever the
    // magnitude transfer under-predicts a channel near its own rim inside the core (measured on
    // DSC00078's sun: ~10 px flat at clip0_B, then a 2x-slope break). JOINT variant blended by the
    // clip-asymmetry gate ctx->floor_gate (see the cf Step-5 floor for the rationale): one scalar
    // lift of the clipped subset preserves the diffused chromaticity; per-channel at gate 0.
    const float floor_gate = ctx->floor_gate;
    __OMP_PARALLEL_FOR__()
    for(size_t i = 0; i < region_pixels; i++)
    {
      const float raccum = fmaxf(plane1[i * 4 + 0] + plane1[i * 4 + 1] + plane1[i * 4 + 2], epsilon); // sum_j r_j

      float lift = 1.f;
      if(floor_gate > 1e-6f)
        for(int c = 0; c < 3; c++)
          if(vld_an[i * 4 + c] < 0.5f)
          {
            const float ratio_c = fmaxf(plane1[i * 4 + c], 0.f);
            const float value = fmaxf(lum_accum[i] * ratio_c / raccum, 1e-6f);
            const float clip_floor_c = clip0[i * 4 + c];
            const float delta = value - clip_floor_c;
            const float weight = 0.02f * fmaxf(clip_floor_c, 1e-6f);
            const float target = clip_floor_c + 0.5f * (delta + sqrtf(delta * delta + weight * weight));
            lift = fmaxf(lift, fminf(target / value, 8.f));
          }

      for(int c = 0; c < 3; c++)
        if(vld_an[i * 4 + c] < 0.5f)
        {
          const float ratio_c = fmaxf(plane1[i * 4 + c], 0.f);
          const float value = lum_accum[i] * ratio_c / raccum; // recombine u_c = L_sum * r_c / sum_j r_j
          // soft saturation floor u_c <- c0 + 0.5*((u-c0) + sqrt((u-c0)^2 + w^2)), w = 0.02*c0
          // (article rule 3 / step 5 soft-max): a smooth max(u, c0) with no shelf-and-kink
          const float clip_floor_c = clip0[i * 4 + c];
          const float weight = 0.02f * fmaxf(clip_floor_c, 1e-6f);
          const float delta = value - clip_floor_c;
          const float per_chan = clip_floor_c + 0.5f * (delta + sqrtf(delta * delta + weight * weight));
          if(floor_gate <= 1e-6f)
          {
            estimate[i * 4 + c] = per_chan; // bit-exact approved path
            continue;
          }
          const float lifted = fmaxf(value, 1e-6f) * lift;
          const float delta_joint = lifted - clip_floor_c;
          const float joint
              = clip_floor_c + 0.5f * (delta_joint + sqrtf(delta_joint * delta_joint + weight * weight));
          estimate[i * 4 + c] = floor_gate * joint + (1.f - floor_gate) * per_chan;
        }
    }
  }
}

// ============================ OpenCL ============================

#if defined(HAVE_OPENCL) && DT_HL_SPARSE_SOLVE && (DT_HL_ANISO_SOLVER == 2)

// Explicit coarse-to-fine structure-steered diffusion on the device, mirroring the CPU
// pyramid (_aniso_tensor + _aniso_iterate in the DT_HL_ANISO_CHROMA block) that handles cores
// beyond DT_HL_SPARSE_MAX unknowns: each level box-downsamples the brightness/ratios/holes,
// rebuilds the structure tensor (local edge direction and strength), runs 240 damped stencil
// steps per channel over the hole bounding box (ping-pong buffers instead of the CPU's
// write-back copy), and bilinearly splats the ratios into the fine planes' clipped channels
// to seed the next level. Any change here must be mirrored in the CPU pyramid and
// re-validated with the HL_ANISOCL_TEST self-test (_aniso_stage_cl_selftest).
//
// MATHS BRIDGE -- Step 8 large-core path (article §"The update rules", the explicit trace-form
// pyramid): the multiscale solver for min int grad(r)^T D grad(r) s.t. r >= c0/L when the core
// exceeds DT_HL_SPARSE_MAX. Each level projects onto the obstacle then runs 240 explicit steps of
// r <- max(r + 0.18*tr(D Hess r), c0/L) (kernel hl_aniso_iter[_block]); coarsest level first so
// the whole hole is seeded before refinement. D = structure tensor of the recovered luminance.
static cl_int _aniso_pyramid_cl(const int devid, void *gd_void, cl_mem ratios, cl_mem valid, cl_mem luminance,
                                cl_mem clip0, const int region_w, const int region_h, const float radius,
                                const int box_x_lo, const int box_y_lo, const int box_x_hi, const int box_y_hi,
                                const dt_dev_pixelpipe_t *pipe)
{
  // the pyramid levels run reaction-free; the "inpaint a flat color" pull is applied by the
  // direct solve and the full-resolution polish only, like the CPU twin
  const float no_react = 0.f;
  dt_iop_highlights_global_data_t *global_data = (dt_iop_highlights_global_data_t *)gd_void;
  cl_int cl_err = CL_SUCCESS;

  int n_levels = 1;
  while(((int)radius >> (n_levels - 1)) > 8 && n_levels < 7) n_levels++;

  for(int level = n_levels - 1; level >= 0 && cl_err == CL_SUCCESS; level--)
  {
    const int step = 1 << level;
    const int coarse_w = (region_w + step - 1) / step;
    const int coarse_h = (region_h + step - 1) / step;
    const size_t coarse_pixels = (size_t)coarse_w * coarse_h;
    size_t size_coarse[3] = { ROUNDUPDWD(coarse_w, devid), ROUNDUPDHT(coarse_h, devid), 1 };
    size_t size_full[3] = { ROUNDUPDWD(region_w, devid), ROUNDUPDHT(region_h, devid), 1 };

    cl_mem coarse_lum = dt_opencl_alloc_device_buffer(devid, sizeof(float) * coarse_pixels);
    cl_mem coarse_ratios = dt_opencl_alloc_device_buffer(devid, sizeof(float) * coarse_pixels * 3);
    cl_mem coarse_obstacle = dt_opencl_alloc_device_buffer(devid, sizeof(float) * coarse_pixels * 3);
    cl_mem coarse_hole = dt_opencl_alloc_device_buffer(devid, coarse_pixels * 3);
    cl_mem grad_x = dt_opencl_alloc_device_buffer(devid, sizeof(float) * coarse_pixels);
    cl_mem tensor_xx = dt_opencl_alloc_device_buffer(devid, sizeof(float) * coarse_pixels);
    cl_mem grad_y = dt_opencl_alloc_device_buffer(devid, sizeof(float) * coarse_pixels);
    cl_mem tensor_xy = dt_opencl_alloc_device_buffer(devid, sizeof(float) * coarse_pixels);
    cl_mem tensor_yy = dt_opencl_alloc_device_buffer(devid, sizeof(float) * coarse_pixels);
    cl_mem diffuse_a = dt_opencl_alloc_device_buffer(devid, sizeof(float) * coarse_pixels);
    cl_mem diffuse_b = dt_opencl_alloc_device_buffer(devid, sizeof(float) * coarse_pixels);
    cl_mem grad_partials = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 256);
    if(!coarse_lum || !coarse_ratios || !coarse_obstacle || !coarse_hole || !grad_x || !tensor_xx || !grad_y
       || !tensor_xy || !tensor_yy || !diffuse_a || !diffuse_b || !grad_partials)
      cl_err = DT_OPENCL_DEFAULT_ERROR;

    if(cl_err == CL_SUCCESS)
    {
      const int kernel = global_data->kernel_hl_aniso_pyr_down;
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &luminance);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &ratios);
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &valid);
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &clip0);
      dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &coarse_lum);
      dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(cl_mem), &coarse_ratios);
      dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(cl_mem), &coarse_obstacle);
      dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(cl_mem), &coarse_hole);
      dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &region_w);
      dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(int), &region_h);
      dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(int), &coarse_w);
      dt_opencl_set_kernel_arg(devid, kernel, 11, sizeof(int), &coarse_h);
      dt_opencl_set_kernel_arg(devid, kernel, 12, sizeof(int), &step);
      cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size_coarse);
    }

    // structure tensor of this level's luminance (box3 x2, gradient + mean magnitude, D)
    if(cl_err == CL_SUCCESS)
    {
      const int kernel = global_data->kernel_hl_box3;
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &coarse_lum);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &grad_x);
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &coarse_w);
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &coarse_h);
      cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size_coarse);
      if(cl_err == CL_SUCCESS)
      {
        dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &grad_x);
        dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &tensor_xx);
        cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size_coarse);
      }
    }
    if(cl_err == CL_SUCCESS)
    {
      const int local_size = 64, n_groups = 256;
      const int kernel = global_data->kernel_hl_grad_reduce;
      size_t sizes[3] = { (size_t)n_groups * local_size, 1, 1 };
      size_t local[3] = { local_size, 1, 1 };
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &tensor_xx);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &grad_x); // gx
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &grad_y); // gy
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &grad_partials);
      dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &coarse_w);
      dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &coarse_h);
      dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(float) * local_size, NULL);
      cl_err = dt_opencl_enqueue_kernel_2d_with_local(devid, kernel, sizes, local);
      if(cl_err == CL_SUCCESS)
      {
        float partial_sums[256];
        cl_err = dt_opencl_read_buffer_from_device(devid, partial_sums, grad_partials, 0, sizeof(float) * n_groups,
                                                   CL_TRUE);
        if(cl_err == CL_SUCCESS)
        {
          double grad_sum = 0.0;
          for(int group = 0; group < n_groups; group++) grad_sum += (double)partial_sums[group];
          const float grad_mean = fmaxf((float)(grad_sum / (double)coarse_pixels), 1e-9f);
          const int kernel_tensor = global_data->kernel_hl_aniso_tensor;
          dt_opencl_set_kernel_arg(devid, kernel_tensor, 0, sizeof(cl_mem), &grad_x);
          dt_opencl_set_kernel_arg(devid, kernel_tensor, 1, sizeof(cl_mem), &grad_y);
          dt_opencl_set_kernel_arg(devid, kernel_tensor, 2, sizeof(cl_mem), &tensor_xx); // txx
          dt_opencl_set_kernel_arg(devid, kernel_tensor, 3, sizeof(cl_mem), &tensor_xy); // txy
          dt_opencl_set_kernel_arg(devid, kernel_tensor, 4, sizeof(cl_mem), &tensor_yy);
          dt_opencl_set_kernel_arg(devid, kernel_tensor, 5, sizeof(int), &coarse_w);
          dt_opencl_set_kernel_arg(devid, kernel_tensor, 6, sizeof(int), &coarse_h);
          dt_opencl_set_kernel_arg(devid, kernel_tensor, 7, sizeof(float), &grad_mean);
          cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel_tensor, size_coarse);
        }
      }
    }

    // per-channel 240-step diffusion over the level's hole bbox
    if(cl_err == CL_SUCCESS)
    {
      const int level_x_lo = MAX(box_x_lo / step - 2, 0), level_y_lo = MAX(box_y_lo / step - 2, 0);
      const int level_x_hi = MIN(box_x_hi / step + 2, coarse_w - 1),
                level_y_hi = MIN(box_y_hi / step + 2, coarse_h - 1);
      size_t size_box[3]
          = { ROUNDUPDWD(level_x_hi - level_x_lo + 1, devid), ROUNDUPDHT(level_y_hi - level_y_lo + 1, devid), 1 };

      for(int c = 0; c < 3 && cl_err == CL_SUCCESS; c++)
      {
        {
          const int kernel = global_data->kernel_hl_pyr_getc;
          dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &coarse_ratios);
          dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &diffuse_a);
          dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &coarse_w);
          dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &coarse_h);
          dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &c);
          cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size_coarse);
        }
        if(cl_err == CL_SUCCESS)
        {
          // seed projection onto the obstacle (mirrors the CPU _aniso_iterate_obs entry clamp)
          const int kernel = global_data->kernel_hl_pyr_project;
          dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &diffuse_a);
          dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &coarse_obstacle);
          dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &coarse_hole);
          dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &coarse_w);
          dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &coarse_h);
          dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &c);
          cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size_coarse);
        }
        if(cl_err == CL_SUCCESS)
          cl_err = dt_opencl_enqueue_copy_buffer_to_buffer(devid, diffuse_a, diffuse_b, 0, 0,
                                                           sizeof(float) * coarse_pixels);

        cl_mem current_buf = diffuse_a, other_buf = diffuse_b;
        if((level_x_hi - level_x_lo + 1) * (level_y_hi - level_y_lo + 1) <= 4096)
        {
          // all 240 steps in one single-workgroup launch (bit-identical, see the fill)
          const int kernel = global_data->kernel_hl_aniso_iter_block;
          const int iters = 240;
          size_t size_block[3] = { 256, 1, 1 };
          size_t local_block[3] = { 256, 1, 1 };
          dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &diffuse_a);
          dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &diffuse_b);
          dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &coarse_hole);
          dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &tensor_xx);
          dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &tensor_xy);
          dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(cl_mem), &tensor_yy);
          dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(cl_mem), &coarse_obstacle);
          dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &coarse_w);
          dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &coarse_h);
          dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(int), &c);
          dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(int), &level_x_lo);
          dt_opencl_set_kernel_arg(devid, kernel, 11, sizeof(int), &level_y_lo);
          dt_opencl_set_kernel_arg(devid, kernel, 12, sizeof(int), &level_x_hi);
          dt_opencl_set_kernel_arg(devid, kernel, 13, sizeof(int), &level_y_hi);
          dt_opencl_set_kernel_arg(devid, kernel, 14, sizeof(int), &iters);
          dt_opencl_set_kernel_arg(devid, kernel, 15, sizeof(float), &no_react);
          dt_opencl_set_kernel_arg(devid, kernel, 16, sizeof(float), &no_react);
          cl_err = dt_opencl_enqueue_kernel_2d_with_local(devid, kernel, size_block, local_block);
        }
        else
          for(int iter = 0; iter < 240 && cl_err == CL_SUCCESS; iter++)
          {
            const int kernel = global_data->kernel_hl_aniso_iter;
            dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &current_buf);
            dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &other_buf);
            dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &coarse_hole);
            dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &tensor_xx);
            dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &tensor_xy);
            dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(cl_mem), &tensor_yy);
            dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(cl_mem), &coarse_obstacle);
            dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &coarse_w);
            dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &coarse_h);
            dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(int), &c);
            dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(int), &level_x_lo);
            dt_opencl_set_kernel_arg(devid, kernel, 11, sizeof(int), &level_y_lo);
            dt_opencl_set_kernel_arg(devid, kernel, 12, sizeof(int), &level_x_hi);
            dt_opencl_set_kernel_arg(devid, kernel, 13, sizeof(int), &level_y_hi);
            dt_opencl_set_kernel_arg(devid, kernel, 14, sizeof(float), &no_react);
            dt_opencl_set_kernel_arg(devid, kernel, 15, sizeof(float), &no_react);
            cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size_box);
            cl_mem swap_buf = current_buf;
            current_buf = other_buf;
            other_buf = swap_buf;
          }
        if(cl_err == CL_SUCCESS)
        {
          const int kernel = global_data->kernel_hl_pyr_putc;
          dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &current_buf);
          dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &coarse_ratios);
          dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &coarse_w);
          dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &coarse_h);
          dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &c);
          cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size_coarse);
        }
      }
    }

    if(cl_err == CL_SUCCESS)
    {
      const int kernel = global_data->kernel_hl_aniso_splat;
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &ratios);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &coarse_ratios);
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region_w);
      dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_h);
      dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &coarse_w);
      dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &coarse_h);
      dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &step);
      cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size_full);
    }

    dt_opencl_release_mem_object(coarse_lum);
    dt_opencl_release_mem_object(coarse_ratios);
    dt_opencl_release_mem_object(coarse_obstacle);
    dt_opencl_release_mem_object(coarse_hole);
    dt_opencl_release_mem_object(grad_x);
    dt_opencl_release_mem_object(tensor_xx);
    dt_opencl_release_mem_object(grad_y);
    dt_opencl_release_mem_object(tensor_xy);
    dt_opencl_release_mem_object(tensor_yy);
    dt_opencl_release_mem_object(diffuse_a);
    dt_opencl_release_mem_object(diffuse_b);
    dt_opencl_release_mem_object(grad_partials);
  }
  return cl_err;
}

cl_int _aniso_stage_cl(const int devid, void *gd_void, cl_mem estimate, cl_mem valid, cl_mem clip0,
                       const int region_w, const int region_h, const float radius, const float floor_gate, const float solid_color, const dt_dev_pixelpipe_t *pipe)
{
  dt_iop_highlights_global_data_t *global_data = (dt_iop_highlights_global_data_t *)gd_void;
  const size_t region_pixels = (size_t)region_w * region_h;
  cl_int cl_err = DT_OPENCL_DEFAULT_ERROR;
  size_t size[3] = { ROUNDUPDWD(region_w, devid), ROUNDUPDHT(region_h, devid), 1 };
  const float epsilon = 1e-6f;

  // "inpaint a flat color": lambda_solid = solid_color^2 * 4 pulls the all-clip chroma toward
  // the mean valid chromaticity; it lives in THIS stage's solves (see the CPU twin -- the joint
  // core's reaction is re-solved away by this stage, so applying it only there leaves the user
  // parameter dead). The target is reduced on the device once and reused by the direct RHS and
  // the full-resolution polish.
  const float react = solid_color * solid_color * 4.f;
  float react_target[3] = { 0.f, 0.f, 0.f };

  if(global_data->kernel_hl_aniso_rhs < 0 || global_data->kernel_hl_aniso_scatter < 0) return cl_err; // no fp64

  cl_mem valid_packed = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels * 4);
  cl_mem luminance = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem ratios = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels * 4);
  cl_mem hole = dt_opencl_alloc_device_buffer(devid, region_pixels);
  cl_mem scratch1 = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem scratch2 = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem tensor_xx = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem tensor_xy = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem tensor_yy = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem partials = NULL, perm_grid_dev = NULL, edge_weights_dev = NULL, rhs_dev = NULL;
  uint8_t *hole_mask = (uint8_t *)dt_pixelpipe_cache_alloc_align(region_pixels, pipe);
  int *grid_to_unknown = NULL, *unknown_to_grid = NULL, *unknown_x = NULL, *unknown_y = NULL, *perm = NULL,
      *inverse_perm = NULL;
  int *matrix_col_ptr = NULL, *matrix_row_index = NULL, *perm_grid = NULL;
  double *matrix_values = NULL;
  float *edge_weights = NULL;
  _sp_chol_cl_t *factor = NULL;
  if(!valid_packed || !luminance || !ratios || !hole || !scratch1 || !scratch2 || !tensor_xx || !tensor_xy
     || !tensor_yy || !hole_mask)
    goto out;

  // validity mask + luminance + ratio planes + all-clip hole in one sweep
  {
    const int kernel = global_data->kernel_hl_aniso_prep;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &valid_packed);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &luminance);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &ratios);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(cl_mem), &hole);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(float), &epsilon);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
    if(cl_err != CL_SUCCESS) goto out;
  }

  cl_err = dt_opencl_read_buffer_from_device(devid, hole_mask, hole, 0, region_pixels, CL_TRUE);
  if(cl_err != CL_SUCCESS) goto out;

  int n_unknowns = 0;
  for(size_t i = 0; i < region_pixels; i++)
    if(hole_mask[i]) n_unknowns++;
  if(n_unknowns == 0)
  {
    cl_err = CL_SUCCESS; // nothing to diffuse
    goto out;
  }
  int box_x_lo = region_w, box_y_lo = region_h, box_x_hi = -1, box_y_hi = -1;
  for(int y = 0; y < region_h; y++)
    for(int x = 0; x < region_w; x++)
      if(hole_mask[(size_t)y * region_w + x])
      {
        box_x_lo = MIN(box_x_lo, x);
        box_x_hi = MAX(box_x_hi, x);
        box_y_lo = MIN(box_y_lo, y);
        box_y_hi = MAX(box_y_hi, y);
      }

  if(react > 0.f)
  {
    // all-valid mean chromaticity (mirrors the CPU double accumulation over plane1): reuse the
    // cmean reduction with lum_min = 0 -- estimate/max(luminance, epsilon) IS the ratios plane
    const int local_size = 64, n_groups = 256;
    const int n_pixels = (int)region_pixels;
    const float lum_min = 0.f;
    cl_mem target_partials = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 4 * n_groups);
    if(!target_partials)
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
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &target_partials);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &n_pixels);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(float), &epsilon);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(float), &lum_min);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(float) * 4 * local_size, NULL);
    cl_err = dt_opencl_enqueue_kernel_2d_with_local(devid, kernel, sizes, local);
    float partial_host[4 * 256];
    if(cl_err == CL_SUCCESS)
      cl_err = dt_opencl_read_buffer_from_device(devid, partial_host, target_partials, 0,
                                                 sizeof(float) * 4 * n_groups, CL_TRUE);
    dt_opencl_release_mem_object(target_partials);
    if(cl_err != CL_SUCCESS) goto out;
    double accum[4] = { 0.0, 0.0, 0.0, 0.0 };
    for(int group = 0; group < n_groups; group++)
      for(int k = 0; k < 4; k++) accum[k] += (double)partial_host[group * 4 + k];
    if(accum[3] > 0.0)
      for(int c = 0; c < 3; c++) react_target[c] = (float)(accum[c] / accum[3]);
  }

  if(n_unknowns > DT_HL_SPARSE_MAX)
  {
    // beyond the direct solve: the explicit coarse-to-fine pyramid, like the CPU
    cl_err = _aniso_pyramid_cl(devid, gd_void, ratios, valid_packed, luminance, clip0, region_w, region_h, radius,
                               box_x_lo, box_y_lo, box_x_hi, box_y_hi, pipe);
    if(cl_err != CL_SUCCESS) goto out;
    goto reassemble;
  }

  // structure tensor of the recovered luminance
  {
    const int kernel = global_data->kernel_hl_box3;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &luminance);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &scratch1);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region_h);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
    if(cl_err != CL_SUCCESS) goto out;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &scratch1);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &scratch2);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
    if(cl_err != CL_SUCCESS) goto out;
  }
  {
    const int local_size = 64, n_groups = 256;
    partials = dt_opencl_alloc_device_buffer(devid, sizeof(float) * n_groups);
    if(!partials)
    {
      cl_err = DT_OPENCL_DEFAULT_ERROR;
      goto out;
    }
    const int kernel = global_data->kernel_hl_grad_reduce;
    size_t sizes[3] = { (size_t)n_groups * local_size, 1, 1 };
    size_t local[3] = { local_size, 1, 1 };
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &scratch2);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &tensor_xx); // gx stash
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &tensor_xy); // grad_y stash
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &partials);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(float) * local_size, NULL);
    cl_err = dt_opencl_enqueue_kernel_2d_with_local(devid, kernel, sizes, local);
    if(cl_err != CL_SUCCESS) goto out;

    float psum[256];
    cl_err = dt_opencl_read_buffer_from_device(devid, psum, partials, 0, sizeof(float) * n_groups, CL_TRUE);
    if(cl_err != CL_SUCCESS) goto out;
    double gsum = 0.0;
    for(int group_index = 0; group_index < n_groups; group_index++) gsum += (double)psum[group_index];
    const float gnorm = fmaxf((float)(gsum / (double)region_pixels), 1e-9f);

    const int kernel_tensor = global_data->kernel_hl_aniso_tensor;
    dt_opencl_set_kernel_arg(devid, kernel_tensor, 0, sizeof(cl_mem), &tensor_xx);
    dt_opencl_set_kernel_arg(devid, kernel_tensor, 1, sizeof(cl_mem), &tensor_xy);
    dt_opencl_set_kernel_arg(devid, kernel_tensor, 2, sizeof(cl_mem), &scratch1); // tensor_xx out (reuse)
    dt_opencl_set_kernel_arg(devid, kernel_tensor, 3, sizeof(cl_mem), &scratch2); // tensor_xy out (reuse)
    dt_opencl_set_kernel_arg(devid, kernel_tensor, 4, sizeof(cl_mem), &tensor_yy);
    dt_opencl_set_kernel_arg(devid, kernel_tensor, 5, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel_tensor, 6, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel_tensor, 7, sizeof(float), &gnorm);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel_tensor, size);
    if(cl_err != CL_SUCCESS) goto out;
  }
  // tensor now lives in (scratch1, scratch2, tensor_yy) = (tensor_xx, tensor_xy, tensor_yy)

  // host symbolic: unknown list + ND ordering (reach 1: 8-neighbour stencil)
  grid_to_unknown = (int *)dt_pixelpipe_cache_alloc_align(sizeof(int) * region_pixels, pipe);
  unknown_to_grid = (int *)dt_pixelpipe_cache_alloc_align(sizeof(int) * n_unknowns, pipe);
  unknown_x = (int *)dt_pixelpipe_cache_alloc_align(sizeof(int) * n_unknowns, pipe);
  unknown_y = (int *)dt_pixelpipe_cache_alloc_align(sizeof(int) * n_unknowns, pipe);
  perm = (int *)dt_pixelpipe_cache_alloc_align(sizeof(int) * n_unknowns, pipe);
  inverse_perm = (int *)dt_pixelpipe_cache_alloc_align(sizeof(int) * n_unknowns, pipe);
  matrix_col_ptr = (int *)dt_pixelpipe_cache_alloc_align(sizeof(int) * (n_unknowns + 1), pipe);
  perm_grid = (int *)dt_pixelpipe_cache_alloc_align(sizeof(int) * n_unknowns, pipe);
  edge_weights = (float *)dt_pixelpipe_cache_alloc_align(sizeof(float) * (size_t)n_unknowns * 8, pipe);
  if(!grid_to_unknown || !unknown_to_grid || !unknown_x || !unknown_y || !perm || !inverse_perm || !matrix_col_ptr
     || !perm_grid || !edge_weights)
  {
    cl_err = DT_OPENCL_DEFAULT_ERROR;
    goto out;
  }
  {
    int unknown_index = 0;
    for(size_t i = 0; i < region_pixels; i++)
    {
      grid_to_unknown[i] = hole_mask[i] ? unknown_index : -1;
      if(hole_mask[i])
      {
        unknown_to_grid[unknown_index] = (int)i;
        unknown_y[unknown_index] = (int)(i / region_w);
        unknown_x[unknown_index] = (int)(i - (size_t)unknown_y[unknown_index] * region_w);
        unknown_index++;
      }
    }
    for(int i = 0; i < n_unknowns; i++) perm[i] = i;
    _sp_nd_order(perm, n_unknowns, unknown_x, unknown_y, 1);
    for(int perm_index = 0; perm_index < n_unknowns; perm_index++) inverse_perm[perm[perm_index]] = perm_index;
    for(int perm_index = 0; perm_index < n_unknowns; perm_index++)
      perm_grid[perm_index] = unknown_to_grid[perm[perm_index]];
  }

  // edge weights on the device (they steer the RHS kernels too), compact download for assembly
  perm_grid_dev = _sp_cl_upload(devid, perm_grid, sizeof(int) * n_unknowns);
  edge_weights_dev = dt_opencl_alloc_device_buffer(devid, sizeof(float) * (size_t)n_unknowns * 8);
  rhs_dev = dt_opencl_alloc_device_buffer(devid, sizeof(double) * n_unknowns);
  if(!perm_grid_dev || !edge_weights_dev || !rhs_dev)
  {
    cl_err = DT_OPENCL_DEFAULT_ERROR;
    goto out;
  }
  {
    const int kernel = global_data->kernel_hl_aniso_weights;
    size_t size_1d[3] = { ROUNDUP(n_unknowns, 64), 1, 1 };
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &scratch1);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &scratch2);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &tensor_yy);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &perm_grid_dev);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &edge_weights_dev);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &n_unknowns);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &region_h);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size_1d);
    if(cl_err != CL_SUCCESS) goto out;
  }
  cl_err = dt_opencl_read_buffer_from_device(devid, edge_weights, edge_weights_dev, 0,
                                             sizeof(float) * (size_t)n_unknowns * 8, CL_TRUE);
  if(cl_err != CL_SUCCESS) goto out;

  // host assembly from the downloaded weights, exactly the CPU _aniso_div_solve pattern
  {
    static const int neighbour_dy[8] = { 0, 0, -1, 1, -1, 1, -1, 1 };
    static const int neighbour_dx[8] = { -1, 1, 0, 0, -1, 1, 1, -1 };
    int success = 1;
    for(int pass = 0; pass < 2 && success; pass++)
    {
      if(pass == 1)
      {
        int total = 0;
        for(int perm_index = 0; perm_index < n_unknowns; perm_index++)
        {
          const int c = matrix_col_ptr[perm_index];
          matrix_col_ptr[perm_index] = total;
          total += c;
        }
        matrix_col_ptr[n_unknowns] = total;
        matrix_row_index = (int *)dt_pixelpipe_cache_alloc_align(sizeof(int) * total, pipe);
        matrix_values = (double *)dt_pixelpipe_cache_alloc_align(sizeof(double) * total, pipe);
        if(!matrix_row_index || !matrix_values) success = 0;
      }

      for(int perm_index = 0; perm_index < n_unknowns && success; perm_index++)
      {
        const int origin_grid = perm_grid[perm_index];
        const int origin_y = origin_grid / region_w, origin_x = origin_grid - origin_y * region_w;
        double diag = 0.0;
        int n_col_entries = 0;

        for(int edge = 0; edge < 8; edge++)
        {
          const float weight_value = edge_weights[(size_t)perm_index * 8 + edge];
          // NaN-safe: !(weight_value > 0) also skips NaN weights (NaN pixels survive the blurs), which
          // 'weight_value <= 0' would let through into a wildly out-of-bounds grid_to_unknown read below
          if(!(weight_value > 0.f)) continue; // outside the border, a zeroed diagonal, or NaN
          const int neighbour_x = origin_x + neighbour_dx[edge], neighbour_y = origin_y + neighbour_dy[edge];
          if(neighbour_x < 0 || neighbour_y < 0 || neighbour_x >= region_w || neighbour_y >= region_h)
            continue; // same guard as the CPU
          diag += weight_value;
          const size_t j = (size_t)neighbour_y * region_w + neighbour_x;
          if(grid_to_unknown[j] >= 0)
          {
            const int target_row = inverse_perm[grid_to_unknown[j]];
            if(target_row < perm_index)
            {
              if(pass == 1)
              {
                matrix_row_index[matrix_col_ptr[perm_index] + n_col_entries] = target_row;
                matrix_values[matrix_col_ptr[perm_index] + n_col_entries] = -(double)weight_value;
              }
              n_col_entries++;
            }
          }
        }
        if(pass == 1)
        {
          matrix_row_index[matrix_col_ptr[perm_index] + n_col_entries] = perm_index;
          matrix_values[matrix_col_ptr[perm_index] + n_col_entries] = diag + (double)react;
        }
        n_col_entries++;
        if(pass == 0) matrix_col_ptr[perm_index] = n_col_entries;
      }
    }
    if(!success)
    {
      cl_err = DT_OPENCL_DEFAULT_ERROR;
      goto out;
    }
  }

  factor = _sp_chol_factor_cl(devid, _hl_sp_chol_kernels(gd_void), n_unknowns, matrix_col_ptr, matrix_row_index,
                              matrix_values);
  if(!factor)
  {
    cl_err = DT_OPENCL_DEFAULT_ERROR;
    goto out;
  }

  for(int c = 0; c < 3; c++)
  {
    {
      const int kernel = global_data->kernel_hl_aniso_rhs;
      size_t size_1d[3] = { ROUNDUP(n_unknowns, 64), 1, 1 };
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &edge_weights_dev);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid_packed);
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &ratios);
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &perm_grid_dev);
      dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &rhs_dev);
      dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &n_unknowns);
      dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &region_w);
      dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &region_h);
      dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &c);
      dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(float), &react);
      dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(float), &react_target[c]);
      cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size_1d);
      if(cl_err != CL_SUCCESS) goto out;
    }
    if(_sp_chol_solve_cl(factor, _hl_sp_chol_kernels(gd_void), rhs_dev))
    {
      cl_err = DT_OPENCL_DEFAULT_ERROR;
      goto out;
    }
    {
      const int kernel = global_data->kernel_hl_aniso_scatter;
      size_t size_1d[3] = { ROUNDUP(n_unknowns, 64), 1, 1 };
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &rhs_dev);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &perm_grid_dev);
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &ratios);
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &n_unknowns);
      dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &c);
      cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size_1d);
      if(cl_err != CL_SUCCESS) goto out;
    }
  }

reassemble:;
  // Full-resolution projected polish, both solver paths (mirrors the CPU block): the
  // saturation floors active as an obstacle inside a short structure-steered relaxation, so
  // the field settles smoothly around the constraint instead of being clamped pointwise
  // at the reassembly.
  {
    cl_mem grad_y = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
    cl_mem dobs3 = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels * 3);
    cl_mem dhole3 = dt_opencl_alloc_device_buffer(devid, region_pixels * 3);
    cl_mem diffuse_a = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
    cl_mem diffuse_b = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
    cl_mem ppart = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 256);
    cl_mem aflags = dt_opencl_alloc_device_buffer(devid, sizeof(int) * 3);
    if(!grad_y || !dobs3 || !dhole3 || !diffuse_a || !diffuse_b || !ppart || !aflags)
      cl_err = DT_OPENCL_DEFAULT_ERROR;

    // full-res structure tensor of the recovered luminance (box3 x2, gradient, D)
    if(cl_err == CL_SUCCESS)
    {
      const int kernel = global_data->kernel_hl_box3;
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &luminance);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &scratch1);
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &region_w);
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region_h);
      cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
      if(cl_err == CL_SUCCESS)
      {
        dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &scratch1);
        dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &scratch2);
        cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
      }
    }
    if(cl_err == CL_SUCCESS)
    {
      const int local_size = 64, n_groups = 256;
      const int kernel = global_data->kernel_hl_grad_reduce;
      size_t sizes[3] = { (size_t)n_groups * local_size, 1, 1 };
      size_t local[3] = { local_size, 1, 1 };
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &scratch2);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &scratch1); // gx
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &grad_y);   // grad_y
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &ppart);
      dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_w);
      dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &region_h);
      dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(float) * local_size, NULL);
      cl_err = dt_opencl_enqueue_kernel_2d_with_local(devid, kernel, sizes, local);
      if(cl_err == CL_SUCCESS)
      {
        float partial_sums[256];
        cl_err = dt_opencl_read_buffer_from_device(devid, partial_sums, ppart, 0, sizeof(float) * 256, CL_TRUE);
        if(cl_err == CL_SUCCESS)
        {
          double gsum = 0.0;
          for(int group_index = 0; group_index < 256; group_index++) gsum += (double)partial_sums[group_index];
          const float gnorm = fmaxf((float)(gsum / (double)region_pixels), 1e-9f);
          const int kernel_tensor = global_data->kernel_hl_aniso_tensor;
          dt_opencl_set_kernel_arg(devid, kernel_tensor, 0, sizeof(cl_mem), &scratch1);
          dt_opencl_set_kernel_arg(devid, kernel_tensor, 1, sizeof(cl_mem), &grad_y);
          dt_opencl_set_kernel_arg(devid, kernel_tensor, 2, sizeof(cl_mem), &tensor_xx);
          dt_opencl_set_kernel_arg(devid, kernel_tensor, 3, sizeof(cl_mem), &tensor_xy);
          dt_opencl_set_kernel_arg(devid, kernel_tensor, 4, sizeof(cl_mem), &tensor_yy);
          dt_opencl_set_kernel_arg(devid, kernel_tensor, 5, sizeof(int), &region_w);
          dt_opencl_set_kernel_arg(devid, kernel_tensor, 6, sizeof(int), &region_h);
          dt_opencl_set_kernel_arg(devid, kernel_tensor, 7, sizeof(float), &gnorm);
          cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel_tensor, size);
        }
      }
    }
    if(cl_err == CL_SUCCESS)
    {
      const int kernel = global_data->kernel_hl_aniso_obs_full;
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &valid_packed);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &clip0);
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &luminance);
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &dobs3);
      dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &dhole3);
      dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &region_w);
      dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &region_h);
      dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(float), &epsilon);
      cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
    }

    // Activity gate (mirrors the CPU block): a channel whose obstacle can never fire skips
    // its 60 full-res sweeps entirely -- the field is already settled by the solvers.
    int active[3] = { 0, 0, 0 };
    if(cl_err == CL_SUCCESS)
    {
      cl_err = dt_opencl_write_buffer_to_device(devid, active, aflags, 0, sizeof(int) * 3, CL_TRUE);
      if(cl_err == CL_SUCCESS)
      {
        const int kernel = global_data->kernel_hl_aniso_obs_flags;
        dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &ratios);
        dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &dobs3);
        dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &dhole3);
        dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &aflags);
        dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_w);
        dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &region_h);
        cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
      }
      if(cl_err == CL_SUCCESS)
        cl_err = dt_opencl_read_buffer_from_device(devid, active, aflags, 0, sizeof(int) * 3, CL_TRUE);
    }
    // the reaction must run its sweeps even where the obstacle can never fire (CPU react_on)
    if(react > 0.f) active[0] = active[1] = active[2] = 1;

    size_t size_box[3]
        = { ROUNDUPDWD(box_x_hi - box_x_lo + 1, devid), ROUNDUPDHT(box_y_hi - box_y_lo + 1, devid), 1 };
    for(int c = 0; c < 3 && cl_err == CL_SUCCESS; c++)
    {
      if(!active[c]) continue;
      {
        const int kernel = global_data->kernel_hl_pyr_getc4;
        dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &ratios);
        dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &diffuse_a);
        dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &region_w);
        dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region_h);
        dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &c);
        cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
      }
      if(cl_err == CL_SUCCESS)
      {
        const int kernel = global_data->kernel_hl_pyr_project;
        dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &diffuse_a);
        dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &dobs3);
        dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &dhole3);
        dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region_w);
        dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_h);
        dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &c);
        cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
      }
      if(cl_err == CL_SUCCESS)
        cl_err = dt_opencl_enqueue_copy_buffer_to_buffer(devid, diffuse_a, diffuse_b, 0, 0,
                                                         sizeof(float) * region_pixels);

      cl_mem current_buf = diffuse_a, other_buf = diffuse_b;
      for(int iter = 0; iter < 60 && cl_err == CL_SUCCESS; iter++)
      {
        const int kernel = global_data->kernel_hl_aniso_iter;
        dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &current_buf);
        dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &other_buf);
        dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &dhole3);
        dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &tensor_xx);
        dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &tensor_xy);
        dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(cl_mem), &tensor_yy);
        dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(cl_mem), &dobs3);
        dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &region_w);
        dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &region_h);
        dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(int), &c);
        dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(int), &box_x_lo);
        dt_opencl_set_kernel_arg(devid, kernel, 11, sizeof(int), &box_y_lo);
        dt_opencl_set_kernel_arg(devid, kernel, 12, sizeof(int), &box_x_hi);
        dt_opencl_set_kernel_arg(devid, kernel, 13, sizeof(int), &box_y_hi);
        dt_opencl_set_kernel_arg(devid, kernel, 14, sizeof(float), &react);
        dt_opencl_set_kernel_arg(devid, kernel, 15, sizeof(float), &react_target[c]);
        cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size_box);
        cl_mem swap_buf = current_buf;
        current_buf = other_buf;
        other_buf = swap_buf;
      }
      if(cl_err == CL_SUCCESS)
      {
        const int kernel = global_data->kernel_hl_pyr_putc4;
        dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &current_buf);
        dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &ratios);
        dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &region_w);
        dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region_h);
        dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &c);
        cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
      }
    }

    dt_opencl_release_mem_object(grad_y);
    dt_opencl_release_mem_object(dobs3);
    dt_opencl_release_mem_object(dhole3);
    dt_opencl_release_mem_object(diffuse_a);
    dt_opencl_release_mem_object(diffuse_b);
    dt_opencl_release_mem_object(ppart);
    dt_opencl_release_mem_object(aflags);
    if(cl_err != CL_SUCCESS) goto out;
  }

  {
    const int kernel = global_data->kernel_hl_aniso_reassemble;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid_packed);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &luminance);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &ratios);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &clip0);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(float), &epsilon);
    dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(float), &floor_gate);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, size);
  }

out:
  dt_opencl_release_mem_object(valid_packed);
  dt_opencl_release_mem_object(luminance);
  dt_opencl_release_mem_object(ratios);
  dt_opencl_release_mem_object(hole);
  dt_opencl_release_mem_object(scratch1);
  dt_opencl_release_mem_object(scratch2);
  dt_opencl_release_mem_object(tensor_xx);
  dt_opencl_release_mem_object(tensor_xy);
  dt_opencl_release_mem_object(tensor_yy);
  dt_opencl_release_mem_object(partials);
  dt_opencl_release_mem_object(perm_grid_dev);
  dt_opencl_release_mem_object(edge_weights_dev);
  dt_opencl_release_mem_object(rhs_dev);
  dt_pixelpipe_cache_free_align(hole_mask);
  dt_pixelpipe_cache_free_align(grid_to_unknown);
  dt_pixelpipe_cache_free_align(unknown_to_grid);
  dt_pixelpipe_cache_free_align(unknown_x);
  dt_pixelpipe_cache_free_align(unknown_y);
  dt_pixelpipe_cache_free_align(perm);
  dt_pixelpipe_cache_free_align(inverse_perm);
  dt_pixelpipe_cache_free_align(matrix_col_ptr);
  dt_pixelpipe_cache_free_align(matrix_row_index);
  dt_pixelpipe_cache_free_align(matrix_values);
  dt_pixelpipe_cache_free_align(perm_grid);
  dt_pixelpipe_cache_free_align(edge_weights);
  _sp_chol_cl_free(factor);
  return cl_err;
}

#endif // HAVE_OPENCL && DT_HL_SPARSE_SOLVE && ANISO_SOLVER 2
