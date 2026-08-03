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

// Sparse-SPD PDE assembly/solve on the region grid (screened Poisson / diffusion), CPU + OpenCL. (implementation;
// see pde.h for the public API.)

#include "common/darktable.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "iop/highlights/blur.h"
#include "iop/highlights/pde.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static inline void _lap5(const float *const restrict field, float *const restrict laplacian, const int region_w,
                         const int region_h)
{
  __OMP_PARALLEL_FOR__(collapse(2))
  for(int y = 0; y < region_h; y++)
  {
    for(int x = 0; x < region_w; x++)
    {
      // clamped neighbour coordinates (replicate at the borders)
      const int y_north = (y > 0) ? (y - 1) : y;
      const int y_south = (y < region_h - 1) ? (y + 1) : y;
      const int x_west = (x > 0) ? (x - 1) : x;
      const int x_east = (x < region_w - 1) ? (x + 1) : x;

      // centre, 4 edge-neighbours, 4 diagonal-neighbours
      const float c = field[(size_t)y * region_w + x];
      const float north = field[(size_t)y_north * region_w + x];
      const float south = field[(size_t)y_south * region_w + x];
      const float west = field[(size_t)y * region_w + x_west];
      const float east = field[(size_t)y * region_w + x_east];
      const float north_west = field[(size_t)y_north * region_w + x_west];
      const float north_east = field[(size_t)y_north * region_w + x_east];
      const float south_west = field[(size_t)y_south * region_w + x_west];
      const float south_east = field[(size_t)y_south * region_w + x_east];

      // isotropic Laplacian = (4 * edges + corners - 20 * centre) / 6
      laplacian[(size_t)y * region_w + x]
          = (4.f * (north + south + west + east) + (north_west + north_east + south_west + south_east) - 20.f * c)
            / 6.f;
    }
  }
}

// Apply the diffusion operator (the matrix of the partial differential equation) to a
// full-grid field: order 1 -> minus the Laplacian (harmonic smoothing), order 2 -> the
// biharmonic operator (Laplacian applied twice: smooth in value AND slope). Both are
// symmetric positive definite, which is what the Cholesky/conjugate-gradient solvers
// require. `sc` is single-channel scratch.
static inline void _apply_op(const float *const restrict field, float *const restrict output_field,
                             float *const restrict scratch, const int order, const int region_w,
                             const int region_h)
{
  const size_t region_pixels = (size_t)region_w * region_h;
  if(order == 1)
  {
    _lap5(field, output_field, region_w, region_h);
    for(size_t i = 0; i < region_pixels; i++) output_field[i] = -output_field[i];
  }
  else
  {
    _lap5(field, scratch, region_w, region_h);
    _lap5(scratch, output_field, region_w, region_h);
  }
}

#include "common/solvers/sparse_cholesky.h"

// ---- operator rows for the sparse assembly --------------------------------------------------
// row of the 9-point isotropic Laplacian at (y, x), replicate-clamped like _lap5; duplicates
// (folded taps at the borders) are accumulated. Returns the target count (<= 9).
static int _sp_row_l9(const int y, const int x, const int region_w, const int region_h,
                      int *const restrict targets, double *const restrict target_weights)
{
  static const int offset_y[9] = { 0, -1, 1, 0, 0, -1, -1, 1, 1 };
  static const int offset_x[9] = { 0, 0, 0, -1, 1, -1, 1, -1, 1 };
  static const double stencil_weight[9]
      = { -20. / 6., 4. / 6., 4. / 6., 4. / 6., 4. / 6., 1. / 6., 1. / 6., 1. / 6., 1. / 6. };
  int count = 0;
  for(int k = 0; k < 9; k++)
  {
    const int neighbour_y = CLAMP(y + offset_y[k], 0, region_h - 1);
    const int neighbour_x = CLAMP(x + offset_x[k], 0, region_w - 1);
    const int target = neighbour_y * region_w + neighbour_x;
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
  return count;
}

// Row of the diffusion operator at grid index o, for the sparse-matrix assembly: order 1 ->
// minus the 9-point Laplacian, order 2 -> the biharmonic operator (the 9-point Laplacian
// composed with itself), both exactly as _apply_op computes them, including the border
// clamping. Targets cover the WHOLE grid; the caller filters hole/non-hole. Returns the
// number of targets (<= 25).
static int _sp_row_op(const int grid_index, const int order, const int region_w, const int region_h,
                      int *const restrict targets, double *const restrict target_weights)
{
  const int y = grid_index / region_w;
  const int x = grid_index - y * region_w;
  int lap_targets[9];
  double lap_weights[9];
  const int lap_count = _sp_row_l9(y, x, region_w, region_h, lap_targets, lap_weights);
  if(order == 1)
  {
    for(int i = 0; i < lap_count; i++)
    {
      targets[i] = lap_targets[i];
      target_weights[i] = -lap_weights[i];
    }
    return lap_count;
  }
  int count = 0;
  int lap2_targets[9];
  double lap2_weights[9];
  for(int i = 0; i < lap_count; i++)
  {
    const int mid_y = lap_targets[i] / region_w;
    const int mid_x = lap_targets[i] - mid_y * region_w;
    const int lap2_count = _sp_row_l9(mid_y, mid_x, region_w, region_h, lap2_targets, lap2_weights);
    for(int j = 0; j < lap2_count; j++)
    {
      const int target = lap2_targets[j];
      const double value = lap_weights[i] * lap2_weights[j];
      int slot = 0;
      for(; slot < count; slot++)
        if(targets[slot] == target)
        {
          target_weights[slot] += value;
          break;
        }
      if(slot == count)
      {
        targets[count] = target;
        target_weights[count] = value;
        count++;
      }
    }
  }
  return count;
}

int _sp_pde_assemble(const uint8_t *const restrict hole, const float *const restrict diffusion,
                     const float diffusion_const, const int order, const float lambda, const int region_w,
                     const int region_h, int **matrix_col_ptr_out, int **matrix_row_index_out,
                     double **matrix_values_out, int **perm_grid_out, int *n_unknowns_out,
                     const dt_dev_pixelpipe_t *const pipe)
{
  const size_t region_pixels = (size_t)region_w * region_h;
  int n_unknowns = 0;
  for(size_t i = 0; i < region_pixels; i++)
    if(hole[i]) n_unknowns++;
  if(n_unknowns == 0 || n_unknowns > DT_HL_SPARSE_MAX) return 0;

  int *grid_to_unknown = (int *)dt_pixelpipe_cache_alloc_align(sizeof(int) * region_pixels, pipe);
  int *unknown_to_grid = (int *)dt_pixelpipe_cache_alloc_align(sizeof(int) * n_unknowns, pipe);
  int *unknown_x = (int *)dt_pixelpipe_cache_alloc_align(sizeof(int) * n_unknowns, pipe);
  int *unknown_y = (int *)dt_pixelpipe_cache_alloc_align(sizeof(int) * n_unknowns, pipe);
  int *permutation = (int *)dt_pixelpipe_cache_alloc_align(sizeof(int) * n_unknowns, pipe);
  int success = 0;
  int *matrix_col_ptr = NULL, *matrix_row_index = NULL, *inverse_perm = NULL, *perm_grid = NULL;
  double *matrix_values = NULL;
  if(!grid_to_unknown || !unknown_to_grid || !unknown_x || !unknown_y || !permutation) goto done;

  int unknown_index = 0;
  for(size_t i = 0; i < region_pixels; i++)
  {
    grid_to_unknown[i] = hole[i] ? unknown_index : -1;
    if(hole[i])
    {
      unknown_to_grid[unknown_index] = (int)i;
      unknown_y[unknown_index] = (int)(i / region_w);
      unknown_x[unknown_index] = (int)(i - (size_t)unknown_y[unknown_index] * region_w);
      unknown_index++;
    }
  }

  for(int i = 0; i < n_unknowns; i++) permutation[i] = i;
  const int reach = (order == 1) ? 1 : 2;
  _sp_nd_order(permutation, n_unknowns, unknown_x, unknown_y, reach);

  inverse_perm = (int *)dt_pixelpipe_cache_alloc_align(sizeof(int) * n_unknowns, pipe);
  if(!inverse_perm) goto done;
  for(int perm_index = 0; perm_index < n_unknowns; perm_index++)
    inverse_perm[permutation[perm_index]] = perm_index;

  // assembly, two passes (count then fill), upper triangle in permuted indexing
  matrix_col_ptr = (int *)dt_pixelpipe_cache_alloc_align(sizeof(int) * (n_unknowns + 1), pipe);
  if(!matrix_col_ptr) goto done;

  int targets[25];
  double target_weights[25];

  for(int pass = 0; pass < 2; pass++)
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
      if(!matrix_row_index || !matrix_values) goto done;
    }

    for(int perm_index = 0; perm_index < n_unknowns; perm_index++)
    {
      const int origin_grid = unknown_to_grid[permutation[perm_index]];
      const int count = _sp_row_op(origin_grid, order, region_w, region_h, targets, target_weights);
      int n_col_entries = 0;

      for(int slot = 0; slot < count; slot++)
      {
        const int target_grid = targets[slot];
        const int target_unknown = grid_to_unknown[target_grid];
        if(target_unknown < 0) continue; // boundary: lives in the RHS
        const int target_row = inverse_perm[target_unknown];
        if(target_row > perm_index) continue; // upper triangle only

        // replicate-clamping makes border rows nonsymmetric; like the dense solver (which reads
        // only the lower triangle of the row-assembled matrix), keep the row value of the
        // later-eliminated unknown and let the factorization mirror it -- measured better than
        // (A + A^T)/2 on the border-touching test cases, and identical in the interior
        double value = target_weights[slot];
        value *= lambda; // lam * Op entry (the -Delta / biharmonic stencil weight scaled by lambda)
        if(target_row == perm_index)
          // diagonal += diag(d): the screening/reaction term (lambda_solid * I) of (lambda*I - Delta)
          value += (diffusion ? (double)diffusion[origin_grid] : (double)diffusion_const);

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

  // permuted-unknown -> grid mapping (composition of unknown_to_grid and the ND permutation)
  perm_grid = (int *)dt_pixelpipe_cache_alloc_align(sizeof(int) * n_unknowns, pipe);
  if(!perm_grid) goto done;
  for(int perm_index = 0; perm_index < n_unknowns; perm_index++)
    perm_grid[perm_index] = unknown_to_grid[permutation[perm_index]];
  success = 1;

done:
  dt_pixelpipe_cache_free_align(grid_to_unknown);
  dt_pixelpipe_cache_free_align(unknown_x);
  dt_pixelpipe_cache_free_align(unknown_y);
  dt_pixelpipe_cache_free_align(inverse_perm);
  dt_pixelpipe_cache_free_align(unknown_to_grid);
  dt_pixelpipe_cache_free_align(permutation);
  if(success)
  {
    *matrix_col_ptr_out = matrix_col_ptr;
    *matrix_row_index_out = matrix_row_index;
    *matrix_values_out = matrix_values;
    *perm_grid_out = perm_grid;
    *n_unknowns_out = n_unknowns;
  }
  else
  {
    dt_pixelpipe_cache_free_align(matrix_col_ptr);
    dt_pixelpipe_cache_free_align(matrix_row_index);
    dt_pixelpipe_cache_free_align(matrix_values);
    dt_pixelpipe_cache_free_align(perm_grid);
  }
  return success;
}

_sp_chol_t *_sp_pde_factor(const uint8_t *const restrict hole, const float *const restrict diffusion,
                           const int order, const float lambda, const int region_w, const int region_h,
                           int **perm_out, int *n_unknowns_out, const dt_dev_pixelpipe_t *pipe)
{
  int *matrix_col_ptr = NULL, *matrix_row_index = NULL, *perm_grid = NULL;
  double *matrix_values = NULL;
  int n_unknowns = 0;
  if(!_sp_pde_assemble(hole, diffusion, 0.f, order, lambda, region_w, region_h, &matrix_col_ptr, &matrix_row_index,
                       &matrix_values, &perm_grid, &n_unknowns, pipe))
    return NULL;

  _sp_chol_t *factor = _sp_chol_factor(n_unknowns, matrix_col_ptr, matrix_row_index, matrix_values, pipe);
  dt_pixelpipe_cache_free_align(matrix_col_ptr);
  dt_pixelpipe_cache_free_align(matrix_row_index);
  dt_pixelpipe_cache_free_align(matrix_values);
  if(!factor)
  {
    dt_pixelpipe_cache_free_align(perm_grid);
    return NULL;
  }
  *perm_out = perm_grid;
  *n_unknowns_out = n_unknowns;
  return factor;
}

__DT_CLONE_TARGETS__
void _sp_pde_solve(const _sp_chol_t *const factor, const int *const restrict perm_grid,
                   float *const restrict field, const uint8_t *const restrict hole,
                   const float *const restrict diffusion, const float *const restrict target,
                   const float *const restrict source, const int order, const float lambda, const int region_w,
                   const int region_h, double *const restrict rhs, float *const restrict embedded,
                   float *const restrict operator_out, float *const restrict scratch)
{
  const size_t region_pixels = (size_t)region_w * region_h;
  // embed only the fixed boundary (rim) values, zero on the hole, then apply Op to them: this is
  // the Dirichlet contribution Op(r_valid) that gets moved to the RHS
  __OMP_PARALLEL_FOR__()
  for(size_t i = 0; i < region_pixels; i++) embedded[i] = hole[i] ? 0.f : field[i];

  _apply_op(embedded, operator_out, scratch, order, region_w, region_h);

  const int n_unknowns = factor->dimension;
  __OMP_PARALLEL_FOR__()
  for(int unknown_index = 0; unknown_index < n_unknowns; unknown_index++)
  {
    const size_t i = (size_t)perm_grid[unknown_index];
    // RHS = diag(d)*r_target + source - lambda*Op(boundary): the screening pull toward the flat
    // target plus the eliminated Dirichlet rim term, matching A = diag(d) + lambda*Op
    rhs[unknown_index] = (diffusion ? (double)diffusion[i] * target[i] : 0.0) + (source ? (double)source[i] : 0.0)
                         - (double)lambda * operator_out[i];
  }

  _sp_chol_solve(factor, rhs); // two triangular solves against the shared Cholesky factor of A

  __OMP_PARALLEL_FOR__()
  for(int unknown_index = 0; unknown_index < n_unknowns; unknown_index++)
    field[perm_grid[unknown_index]] = (float)rhs[unknown_index];
}

__DT_CLONE_TARGETS__
void _region_pde_solve(float *const restrict field, const uint8_t *const restrict hole,
                       const float *const restrict diffusion, const float *const restrict target,
                       const float *const restrict source, const int order, const float lambda, const int region_w,
                       const int region_h, float *const restrict residual, float *const restrict search_dir,
                       float *const restrict operator_dir, float *const restrict embedded,
                       float *const restrict scratch, const int maxiter)
{
  const size_t region_pixels = (size_t)region_w * region_h;
  // rhs_hole = diffusion*target + source - lambda*Op(boundary embedded, hole=0)
  __OMP_PARALLEL_FOR__()
  for(size_t i = 0; i < region_pixels; i++) embedded[i] = hole[i] ? 0.f : field[i];

  _apply_op(embedded, scratch, operator_dir, order, region_w, region_h);

  __OMP_PARALLEL_FOR__()
  for(size_t i = 0; i < region_pixels; i++)
    residual[i]
        = hole[i]
              ? ((diffusion ? diffusion[i] * target[i] : 0.f) + (source ? source[i] : 0.f) - lambda * scratch[i])
              : 0.f;

  // residual <- rhs - A*x  (x = current field on hole);  A x = diffusion*x + lambda*Op(x embedded)
  __OMP_PARALLEL_FOR__()
  for(size_t i = 0; i < region_pixels; i++) embedded[i] = hole[i] ? field[i] : 0.f;

  _apply_op(embedded, scratch, operator_dir, order, region_w, region_h);

  double residual_sq = 0.0;
  __OMP_PARALLEL_FOR__(reduction(+ : residual_sq))
  for(size_t i = 0; i < region_pixels; i++)
  {
    if(!hole[i])
    {
      search_dir[i] = 0.f;
      continue;
    }

    residual[i] -= (diffusion ? diffusion[i] * field[i] : 0.f) + lambda * scratch[i];
    search_dir[i] = residual[i];
    residual_sq += (double)residual[i] * residual[i];
  }

  const double residual_sq0 = residual_sq;
  if(residual_sq0 < 1e-20) return;
  for(int iter = 0; iter < maxiter; iter++)
  {
    __OMP_PARALLEL_FOR__()
    for(size_t i = 0; i < region_pixels; i++) embedded[i] = hole[i] ? search_dir[i] : 0.f;

    _apply_op(embedded, scratch, operator_dir, order, region_w, region_h);

    double dir_operator_dot = 0.0;
    __OMP_PARALLEL_FOR__(reduction(+ : dir_operator_dot))
    for(size_t i = 0; i < region_pixels; i++)
    {
      if(!hole[i])
      {
        operator_dir[i] = 0.f;
        continue;
      }

      operator_dir[i] = (diffusion ? diffusion[i] * search_dir[i] : 0.f) + lambda * scratch[i];
      dir_operator_dot += (double)search_dir[i] * operator_dir[i];
    }

    if(dir_operator_dot <= 1e-30) break;
    const float alpha = (float)(residual_sq / dir_operator_dot);
    double new_residual_sq = 0.0;

    __OMP_PARALLEL_FOR__(reduction(+ : new_residual_sq))
    for(size_t i = 0; i < region_pixels; i++)
      if(hole[i])
      {
        field[i] += alpha * search_dir[i];
        residual[i] -= alpha * operator_dir[i];
        new_residual_sq += (double)residual[i] * residual[i];
      }

    if(new_residual_sq < 1e-4 * residual_sq0) break;
    const float beta = (float)(new_residual_sq / residual_sq);

    __OMP_PARALLEL_FOR__()
    for(size_t i = 0; i < region_pixels; i++)
      if(hole[i]) search_dir[i] = residual[i] + beta * search_dir[i];

    residual_sq = new_residual_sq;
  }
}

// ============================ OpenCL ============================

#include "common/solvers/sparse_cholesky_cl.h"
#ifdef HAVE_OPENCL
#endif // HAVE_OPENCL

#if defined(HAVE_OPENCL) && DT_HL_SPARSE_SOLVE
cl_int _region_blur1_cl(const int devid, cl_mem in, cl_mem out, const int region_w, const int region_h,
                        const float sigma)
{
  const float vmax[1] = { 1e9f };
  const float vmin[1] = { -1e9f };
  dt_gaussian_cl_t *gaussian = dt_gaussian_init_cl(devid, region_w, region_h, 1, vmax, vmin, sigma, 0);
  if(!gaussian) return DT_OPENCL_DEFAULT_ERROR;
  const cl_int cl_err = dt_gaussian_blur_cl(gaussian, in, out);
  dt_gaussian_free_cl(gaussian);
  return cl_err;
}

cl_int _region_pde_cg_cl(const int devid, void *gd_void, cl_mem solution, cl_mem hole, const int region_w,
                         const int region_h, const float dscalar, const float tscalar, const int maxiter)
{
  dt_iop_highlights_global_data_t *global_data = (dt_iop_highlights_global_data_t *)gd_void;
  const size_t region_pixels = (size_t)region_w * region_h;
  const int unknown_count = (int)region_pixels;
  cl_int cl_err = DT_OPENCL_DEFAULT_ERROR;
  size_t work_size[3] = { ROUNDUPDWD(region_w, devid), ROUNDUPDHT(region_h, devid), 1 };
  const int local_size = 64, n_groups = 256;
  size_t work_size_1d[3] = { (size_t)n_groups * local_size, 1, 1 };
  size_t local_size_1d[3] = { local_size, 1, 1 };

  if(global_data->kernel_hl_cg_r1 < 0) return cl_err; // no fp64 device

  cl_mem temp1 = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem temp2 = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem residual = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem search_dir = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem matvec = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem partials = dt_opencl_alloc_device_buffer(devid, sizeof(double) * n_groups);
  double partial_sums[256];
  if(!temp1 || !temp2 || !residual || !search_dir || !matvec || !partials) goto out;

#define CG_EMBED(src_, keep_)                                                                                     \
  do                                                                                                              \
  {                                                                                                               \
    const int kernel = global_data->kernel_hl_cg_embed;                                                           \
    const int keep_flag = (keep_);                                                                                \
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &(src_));                                          \
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &hole);                                            \
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &temp1);                                           \
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region_w);                                           \
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_h);                                           \
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &keep_flag);                                          \
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, work_size);                                               \
    if(cl_err != CL_SUCCESS) goto out;                                                                            \
    const int kernel_op = global_data->kernel_hl_cg_op;                                                           \
    dt_opencl_set_kernel_arg(devid, kernel_op, 0, sizeof(cl_mem), &temp1);                                        \
    dt_opencl_set_kernel_arg(devid, kernel_op, 1, sizeof(cl_mem), &temp2);                                        \
    dt_opencl_set_kernel_arg(devid, kernel_op, 2, sizeof(int), &region_w);                                        \
    dt_opencl_set_kernel_arg(devid, kernel_op, 3, sizeof(int), &region_h);                                        \
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel_op, work_size);                                            \
    if(cl_err != CL_SUCCESS) goto out;                                                                            \
  } while(0)

  // b = d*target - Op(boundary-embedded u)
  CG_EMBED(solution, 0);
  {
    const int kernel = global_data->kernel_hl_cg_r0;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &residual);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &temp2);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &hole);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(float), &dscalar);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(float), &tscalar);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, work_size);
    if(cl_err != CL_SUCCESS) goto out;
  }

  // r <- b - A u; p = r; rr
  CG_EMBED(solution, 1);
  double residual_norm;
  {
    const int kernel = global_data->kernel_hl_cg_r1;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &residual);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &search_dir);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &solution);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &temp2);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &hole);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(cl_mem), &partials);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &unknown_count);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(float), &dscalar);
    dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(double) * local_size, NULL);
    cl_err = dt_opencl_enqueue_kernel_2d_with_local(devid, kernel, work_size_1d, local_size_1d);
    if(cl_err != CL_SUCCESS) goto out;
    cl_err
        = dt_opencl_read_buffer_from_device(devid, partial_sums, partials, 0, sizeof(double) * n_groups, CL_TRUE);
    if(cl_err != CL_SUCCESS) goto out;
    residual_norm = 0.0;
    for(int group_index = 0; group_index < n_groups; group_index++) residual_norm += partial_sums[group_index];
  }

  const double residual_norm_init = residual_norm;
  if(residual_norm_init < 1e-20)
  {
    cl_err = CL_SUCCESS;
    goto out;
  }

  for(int iteration = 0; iteration < maxiter; iteration++)
  {
    CG_EMBED(search_dir, 1);
    double p_dot_matvec;
    {
      const int kernel = global_data->kernel_hl_cg_ap;
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &matvec);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &search_dir);
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &temp2);
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &hole);
      dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &partials);
      dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &unknown_count);
      dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(float), &dscalar);
      dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(double) * local_size, NULL);
      cl_err = dt_opencl_enqueue_kernel_2d_with_local(devid, kernel, work_size_1d, local_size_1d);
      if(cl_err != CL_SUCCESS) goto out;
      cl_err = dt_opencl_read_buffer_from_device(devid, partial_sums, partials, 0, sizeof(double) * n_groups,
                                                 CL_TRUE);
      if(cl_err != CL_SUCCESS) goto out;
      p_dot_matvec = 0.0;
      for(int group_index = 0; group_index < n_groups; group_index++) p_dot_matvec += partial_sums[group_index];
    }

    if(p_dot_matvec <= 1e-30) break;
    const float alpha = (float)(residual_norm / p_dot_matvec);
    double residual_norm_new;
    {
      const int kernel = global_data->kernel_hl_cg_update;
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &solution);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &residual);
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &search_dir);
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &matvec);
      dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &hole);
      dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(cl_mem), &partials);
      dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &unknown_count);
      dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(float), &alpha);
      dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(double) * local_size, NULL);
      cl_err = dt_opencl_enqueue_kernel_2d_with_local(devid, kernel, work_size_1d, local_size_1d);
      if(cl_err != CL_SUCCESS) goto out;
      cl_err = dt_opencl_read_buffer_from_device(devid, partial_sums, partials, 0, sizeof(double) * n_groups,
                                                 CL_TRUE);
      if(cl_err != CL_SUCCESS) goto out;
      residual_norm_new = 0.0;
      for(int group_index = 0; group_index < n_groups; group_index++)
        residual_norm_new += partial_sums[group_index];
    }

    if(residual_norm_new < 1e-4 * residual_norm_init) break;
    const float beta = (float)(residual_norm_new / residual_norm);
    {
      const int kernel = global_data->kernel_hl_cg_beta;
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &search_dir);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &residual);
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &hole);
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region_w);
      dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_h);
      dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(float), &beta);
      cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, work_size);
      if(cl_err != CL_SUCCESS) goto out;
    }
    residual_norm = residual_norm_new;
  }
  cl_err = CL_SUCCESS;

#undef CG_EMBED
out:
  dt_opencl_release_mem_object(temp1);
  dt_opencl_release_mem_object(temp2);
  dt_opencl_release_mem_object(residual);
  dt_opencl_release_mem_object(search_dir);
  dt_opencl_release_mem_object(matvec);
  dt_opencl_release_mem_object(partials);
  return cl_err;
}

#endif // HAVE_OPENCL && DT_HL_SPARSE_SOLVE
