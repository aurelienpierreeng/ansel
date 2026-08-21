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

#ifndef DT_MATH_SPARSE_CHOLESKY_CL_H
#define DT_MATH_SPARSE_CHOLESKY_CL_H

// Reusable GPU sparse SPD Cholesky solver (double precision, level-scheduled), factored out
// of the highlights harmonic-transposition code. Host-side symbolic analysis (reusing the
// CPU solver's elimination tree / reach), device-side numeric factorization and triangular
// solves. The numeric kernels live in data/kernels/highlights_sparse.cl; the caller owns
// them and passes their handles through _sp_chol_cl_kernels_t.

#include "common/logging.h"
#include "system/macros.h"
#include "math/sparse_cholesky.h" // _sp_etree / _sp_ereach (host symbolic)
#include "common/times.h"

#ifdef HAVE_OPENCL
#include "common/opencl.h"

// Handles of the four data/kernels/highlights_sparse.cl kernels the solver enqueues.
typedef struct
{
  int update_level; // sparse_chol_update_level
  int final_level;  // sparse_chol_final_level
  int fwd_level;    // sparse_chol_fwd_level
  int bwd_level;    // sparse_chol_bwd_level
} _sp_chol_cl_kernels_t;

// GPU sparse Cholesky factor handle (device buffers + host-side symbolic metadata).
typedef struct
{
  int dimension;
  int n_nonzero;
  int nlev;
  int *lev_off; // host: nlev+1 offsets into the level column list
  int *lev_off_bwd;
  int nlev_bwd;
  int *lev_pos_off; // host: nlev+1 offsets into the level-grouped entry-position list
  cl_mem values, colptr, rowind, contptr, contsrc, contljk, posof;
  cl_mem rowptr, rowcol, rowpos, diagpos, levcols, levrows_bwd;
  int devid;
} _sp_chol_cl_t;

// ===== GPU sparse Cholesky (double precision, level-scheduled) ==============================
// Same symmetric-positive-definite systems as the CPU solver, factored and solved on the
// device: the host performs the symbolic analysis on integer metadata only (elimination tree,
// pattern of the factor L, flat column-modification update schedules, level schedule = groups
// of mutually independent columns that can be processed in parallel), then the kernels in
// data/kernels/highlights_sparse.cl run the numeric work in 64-bit floats. This is the
// foundation of the no-roundtrip GPU pipeline (no mid-pipeline download/re-upload of pixels):
// the right-hand side b and the solution x live in device buffers.

// release a GPU sparse Cholesky factor (device buffers + host offsets); NULL-safe
static inline void _sp_chol_cl_free(_sp_chol_cl_t *factor)
{
  if(!factor) return;
  dt_opencl_release_mem_object(factor->values);
  dt_opencl_release_mem_object(factor->colptr);
  dt_opencl_release_mem_object(factor->rowind);
  dt_opencl_release_mem_object(factor->contptr);
  dt_opencl_release_mem_object(factor->contsrc);
  dt_opencl_release_mem_object(factor->contljk);
  dt_opencl_release_mem_object(factor->posof);
  free(factor->lev_pos_off);
  dt_opencl_release_mem_object(factor->rowptr);
  dt_opencl_release_mem_object(factor->rowcol);
  dt_opencl_release_mem_object(factor->rowpos);
  dt_opencl_release_mem_object(factor->diagpos);
  dt_opencl_release_mem_object(factor->levcols);
  dt_opencl_release_mem_object(factor->levrows_bwd);
  free(factor->lev_off);
  free(factor->lev_off_bwd);
  free(factor);
}

// allocate a device buffer and upload host data into it; returns NULL on failure
static inline cl_mem _sp_cl_upload(const int devid, const void *data, const size_t bytes)
{
  cl_mem mem = dt_opencl_alloc_device_buffer(devid, bytes);
  if(IS_NULL_PTR(mem)) return NULL;
  if(dt_opencl_write_buffer_to_device(devid, (void *)data, mem, 0, bytes, CL_TRUE) != CL_SUCCESS)
  {
    dt_opencl_release_mem_object(mem);
    return NULL;
  }
  return mem;
}

// Factor the matrix A (upper-triangular compressed-sparse-column, symmetric positive
// definite, double precision) on the GPU: host-side symbolic analysis (integer metadata),
// device-side numeric factorization, level by level. gd carries the kernel handles.
// Mirrors _sp_chol_factor on the CPU: any change here must be mirrored there and re-validated
// with the HL_SPCL_TEST self-test (_sp_chol_cl_selftest).
//
// Maths bridge: this computes the same factorization A = L * L^T as the CPU _sp_chol_factor
// (SPD region-PDE / biharmonic-dome systems, article "Guided laplacian highlights"), but
// reorganized for the GPU. The numeric recurrences are unchanged --
//   L[i,j] = ( A[i,j] - sum_{k<j} L[i,k] L[j,k] ) / L[j,j] ,   L[j,j] = sqrt( A[j,j] - sum_k L[j,k]^2 )
// -- only the schedule differs: the host precomputes (1) the symbolic pattern of L, (2) every
// cmod(j,k) contribution -L[i,k]*L[j,k] grouped by the destination entry it lands in, and (3) an
// elimination-tree LEVEL SCHEDULE, so the device factors one whole level of mutually independent
// columns at a time (article "Performance -> The OpenCL pipe": the level-scheduled factorization).
static inline _sp_chol_cl_t *_sp_chol_factor_cl(const int devid, const _sp_chol_cl_kernels_t kernels, const int dimension,
                                         const int *const restrict matrix_col_ptr,
                                         const int *const restrict matrix_row_index,
                                         const double *const restrict matrix_values)
{
  if(devid < 0 || kernels.update_level < 0 || kernels.final_level < 0) return NULL;

  int *parent = malloc(sizeof(int) * dimension);
  int *ancestor = malloc(sizeof(int) * dimension);
  int *workspace = malloc(sizeof(int) * dimension);
  int *sstk = malloc(sizeof(int) * dimension);
  _sp_chol_cl_t *factor = calloc(1, sizeof(_sp_chol_cl_t));
  int *colptr = NULL, *rowind = NULL, *colfill = NULL, *col_level = NULL;
  int *updoff = NULL, *updljk = NULL, *updk = NULL;
  int *contptr_h = NULL, *contsrc_h = NULL, *contljk_h = NULL, *fillc = NULL;
  int *posof_h = NULL, *levcols = NULL;
  int *rowptr = NULL, *rowcol = NULL, *rowpos = NULL, *diagpos = NULL, *rowfill = NULL;
  double *values_host = NULL;
  if(!parent || !ancestor || !workspace || !sstk || !factor) goto fail;
  factor->devid = devid;
  factor->dimension = dimension;

  const double _tf0 = dt_get_wtime();
  _sp_etree(dimension, matrix_col_ptr, matrix_row_index, parent, ancestor);

  // pass 1a: column counts of L (diag + one entry per (row j, col k) pattern element)
  colptr = calloc(dimension + 1, sizeof(int));
  updoff = calloc(dimension + 2, sizeof(int));
  if(!colptr || !updoff) goto fail;
  for(int i = 0; i < dimension; i++) workspace[i] = -1;
  size_t nupd = 0;
  for(int j = 0; j < dimension; j++)
  {
    colptr[j + 1] += 1; // diagonal
    const int stack_top = _sp_ereach(dimension, matrix_col_ptr, matrix_row_index, j, parent, sstk, workspace);
    for(int stack_pos = stack_top; stack_pos < dimension; stack_pos++)
    {
      colptr[sstk[stack_pos] + 1] += 1;
      nupd++;
    }
    updoff[j + 1] = (int)nupd;
  }
  for(int j = 0; j < dimension; j++) colptr[j + 1] += colptr[j];
  factor->n_nonzero = colptr[dimension];

  // pass 1b: fill the sorted column patterns (ascending rows: j increases) + the update list
  rowind = malloc(sizeof(int) * factor->n_nonzero);
  colfill = malloc(sizeof(int) * dimension);
  updljk = malloc(sizeof(int) * (nupd ? nupd : 1));
  updk = malloc(sizeof(int) * (nupd ? nupd : 1));
  if(!rowind || !colfill || !updljk || !updk) goto fail;
  for(int j = 0; j < dimension; j++)
  {
    rowind[colptr[j]] = j; // diag first
    colfill[j] = 1;
  }
  for(int i = 0; i < dimension; i++) workspace[i] = -1;
  size_t upd_index = 0;
  size_t npairs = 0;
  for(int j = 0; j < dimension; j++)
  {
    const int stack_top = _sp_ereach(dimension, matrix_col_ptr, matrix_row_index, j, parent, sstk, workspace);
    for(int stack_pos = stack_top; stack_pos < dimension; stack_pos++)
    {
      const int k = sstk[stack_pos];
      const int pos = colptr[k] + colfill[k];
      rowind[pos] = j;
      colfill[k]++;
      updljk[upd_index] = pos; // position of L[j,k]
      updk[upd_index] = k;
      upd_index++;
    }
  }

  // pass 2: column-modification ("cmod") contribution streams, grouped BY DESTINATION entry.
  // For update (j,k): rows >= j of column k map into positions of column j; storing each
  // contribution under the entry it subtracts INTO lets the numeric kernel run one thread per
  // matrix entry with no atomics and the exact ascending-update summation order (the target
  // columns' destination sets are disjoint, so both merge sweeps parallelize over j).
  // Maths bridge: for k in reach(j), each shared row `row` (>= j) contributes the single product
  // -L[row,k]*L[j,k] to entry (row,j) of L -- i.e. it realizes one term of the sum
  // A[row,j] - sum_{k} L[row,k] L[j,k]. contsrc = position of L[row,k], contljk = position of
  // L[j,k]; the update kernel later sums all contributions landing on each entry.
  contptr_h = calloc((size_t)factor->n_nonzero + 1, sizeof(int));
  fillc = calloc(factor->n_nonzero, sizeof(int));
  if(!contptr_h || !fillc) goto fail;

  OMP_PRAGMA(omp parallel for default(firstprivate) schedule(dynamic, 16))
  for(int j = 0; j < dimension; j++)
  {
    for(int upd_slot = updoff[j]; upd_slot < updoff[j + 1]; upd_slot++)
    {
      const int k = updk[upd_slot];
      int pos_j = colptr[j]; // both sorted ascending; rows >= j of col k are a subset
      for(int pos_k = updljk[upd_slot]; pos_k < colptr[k + 1]; pos_k++)
      {
        const int row = rowind[pos_k];
        while(rowind[pos_j] != row) pos_j++;
        contptr_h[pos_j + 1]++;
      }
    }
  }
  for(int entry = 0; entry < factor->n_nonzero; entry++) contptr_h[entry + 1] += contptr_h[entry];
  npairs = (size_t)contptr_h[factor->n_nonzero];
  contsrc_h = malloc(sizeof(int) * (npairs ? npairs : 1));
  contljk_h = malloc(sizeof(int) * (npairs ? npairs : 1));
  if(!contsrc_h || !contljk_h) goto fail;

  OMP_PRAGMA(omp parallel for default(firstprivate) schedule(dynamic, 16))
  for(int j = 0; j < dimension; j++)
  {
    for(int upd_slot = updoff[j]; upd_slot < updoff[j + 1]; upd_slot++)
    {
      const int k = updk[upd_slot];
      const int ljk_pos = updljk[upd_slot];
      int pos_j = colptr[j];
      for(int pos_k = ljk_pos; pos_k < colptr[k + 1]; pos_k++)
      {
        const int row = rowind[pos_k];
        while(rowind[pos_j] != row) pos_j++;
        const int dest_index = contptr_h[pos_j] + fillc[pos_j]++;
        contsrc_h[dest_index] = pos_k;
        contljk_h[dest_index] = ljk_pos;
      }
    }
  }

  // level schedule: group columns into dependency levels -- columns in the same level are
  // mutually independent and can be factored/solved in parallel. Forward levels serve the
  // factorization and the forward solve; backward levels are built separately below.
  // Maths bridge: level(j) = 1 + max_{k in reach(j)} level(k) = the longest chain of column
  // dependencies feeding j in the elimination tree. Columns sharing a level have disjoint
  // dependency cones, so factoring/solving them together is exact -- this is the parallelism the
  // sequential CPU column sweep cannot expose.
  col_level = calloc(dimension, sizeof(int));
  if(!col_level) goto fail;
  int maxlev = 0;
  for(int j = 0; j < dimension; j++)
  {
    int neighbor_max = -1;
    for(int upd_slot = updoff[j]; upd_slot < updoff[j + 1]; upd_slot++)
      if(col_level[updk[upd_slot]] > neighbor_max) neighbor_max = col_level[updk[upd_slot]]; // deepest predecessor
    col_level[j] = neighbor_max + 1; // one level after all columns j depends on
    if(col_level[j] > maxlev) maxlev = col_level[j];
  }
  factor->nlev = maxlev + 1;
  factor->lev_off = calloc(factor->nlev + 1, sizeof(int));
  levcols = malloc(sizeof(int) * dimension);
  if(!factor->lev_off || !levcols) goto fail;
  for(int j = 0; j < dimension; j++) factor->lev_off[col_level[j] + 1]++;
  for(int level = 0; level < factor->nlev; level++) factor->lev_off[level + 1] += factor->lev_off[level];
  {
    int *fill = calloc(factor->nlev, sizeof(int));
    if(!fill) goto fail;
    for(int j = 0; j < dimension; j++) levcols[factor->lev_off[col_level[j]] + fill[col_level[j]]++] = j;
    free(fill);
  }

  // entry positions grouped by level: drives the one-thread-per-entry update kernel
  posof_h = malloc(sizeof(int) * factor->n_nonzero);
  factor->lev_pos_off = calloc(factor->nlev + 1, sizeof(int));
  if(!posof_h || !factor->lev_pos_off) goto fail;
  {
    int dest_index = 0;
    for(int level = 0; level < factor->nlev; level++)
    {
      factor->lev_pos_off[level] = dest_index;
      for(int c = factor->lev_off[level]; c < factor->lev_off[level + 1]; c++)
      {
        const int j = levcols[c];
        for(int pos = colptr[j]; pos < colptr[j + 1]; pos++) posof_h[dest_index++] = pos;
      }
    }
    factor->lev_pos_off[factor->nlev] = dest_index;
  }

  // backward levels: x[j] depends on x[rows > j of column j] (its ancestors)
  // Maths bridge: the backward solve L^T x = y computes x_j = (y_j - sum_{i>j} L[i,j] x_i)/L[j,j],
  // so x_j needs every x_i for the below-diagonal rows i of column j first -- the dependency order
  // is the reverse of the factorization, hence a separately built level schedule.
  {
    int *col_level_bwd = calloc(dimension, sizeof(int));
    int *level_rows = malloc(sizeof(int) * dimension);
    if(!col_level_bwd || !level_rows)
    {
      free(col_level_bwd);
      free(level_rows);
      goto fail;
    }
    int max_bwd = 0;
    for(int j = dimension - 1; j >= 0; j--)
    {
      int neighbor_max = -1;
      for(int pos = colptr[j] + 1; pos < colptr[j + 1]; pos++)
        if(col_level_bwd[rowind[pos]] > neighbor_max) neighbor_max = col_level_bwd[rowind[pos]];
      col_level_bwd[j] = neighbor_max + 1;
      if(col_level_bwd[j] > max_bwd) max_bwd = col_level_bwd[j];
    }
    factor->nlev_bwd = max_bwd + 1;
    factor->lev_off_bwd = calloc(factor->nlev_bwd + 1, sizeof(int));
    if(!factor->lev_off_bwd)
    {
      free(col_level_bwd);
      free(level_rows);
      goto fail;
    }
    for(int j = 0; j < dimension; j++) factor->lev_off_bwd[col_level_bwd[j] + 1]++;
    for(int level = 0; level < factor->nlev_bwd; level++)
      factor->lev_off_bwd[level + 1] += factor->lev_off_bwd[level];
    int *fill = calloc(factor->nlev_bwd, sizeof(int));
    if(!fill)
    {
      free(col_level_bwd);
      free(level_rows);
      goto fail;
    }
    for(int j = 0; j < dimension; j++)
      level_rows[factor->lev_off_bwd[col_level_bwd[j]] + fill[col_level_bwd[j]]++] = j;
    free(fill);
    factor->levrows_bwd = _sp_cl_upload(devid, level_rows, sizeof(int) * dimension);
    free(col_level_bwd);
    free(level_rows);
    if(IS_NULL_PTR(factor->levrows_bwd)) goto fail;
  }

  // row-wise (compressed-sparse-row) mirror of the off-diagonal pattern for the forward solve,
  // plus the position of each column's diagonal value
  rowptr = calloc(dimension + 1, sizeof(int));
  diagpos = malloc(sizeof(int) * dimension);
  rowfill = calloc(dimension, sizeof(int));
  if(!rowptr || !diagpos || !rowfill) goto fail;
  for(int c = 0; c < dimension; c++)
  {
    diagpos[c] = colptr[c];
    for(int pos = colptr[c] + 1; pos < colptr[c + 1]; pos++) rowptr[rowind[pos] + 1]++;
  }
  for(int j = 0; j < dimension; j++) rowptr[j + 1] += rowptr[j];
  rowcol = malloc(sizeof(int) * (factor->n_nonzero - dimension + 1));
  rowpos = malloc(sizeof(int) * (factor->n_nonzero - dimension + 1));
  if(!rowcol || !rowpos) goto fail;
  for(int c = 0; c < dimension; c++)
    for(int pos = colptr[c] + 1; pos < colptr[c + 1]; pos++)
    {
      const int row = rowind[pos];
      const int dest_index = rowptr[row] + rowfill[row]++;
      rowcol[dest_index] = c;
      rowpos[dest_index] = pos;
    }

  // numeric init: scatter the upper-CSC A into the (lower) L pattern
  values_host = calloc(factor->n_nonzero, sizeof(double));
  if(!values_host) goto fail;
  for(int c = 0; c < dimension; c++)
    for(int pos = matrix_col_ptr[c]; pos < matrix_col_ptr[c + 1]; pos++)
    {
      const int row = matrix_row_index[pos]; // r <= c: lower entry (c, r) -> column r
      if(row == c)
        values_host[colptr[c]] = matrix_values[pos];
      else
      {
        int dest_index = colptr[row];
        while(rowind[dest_index] != c) dest_index++;
        values_host[dest_index] = matrix_values[pos];
      }
    }

  // upload all the symbolic metadata + the seeded values to the device
  const double _tf1 = dt_get_wtime();
  factor->values = _sp_cl_upload(devid, values_host, sizeof(double) * factor->n_nonzero);
  factor->colptr = _sp_cl_upload(devid, colptr, sizeof(int) * (dimension + 1));
  factor->rowind = _sp_cl_upload(devid, rowind, sizeof(int) * factor->n_nonzero);
  factor->contptr = _sp_cl_upload(devid, contptr_h, sizeof(int) * ((size_t)factor->n_nonzero + 1));
  factor->contsrc = _sp_cl_upload(devid, contsrc_h, sizeof(int) * (npairs ? npairs : 1));
  factor->contljk = _sp_cl_upload(devid, contljk_h, sizeof(int) * (npairs ? npairs : 1));
  factor->posof = _sp_cl_upload(devid, posof_h, sizeof(int) * factor->n_nonzero);
  factor->rowptr = _sp_cl_upload(devid, rowptr, sizeof(int) * (dimension + 1));
  factor->rowcol = _sp_cl_upload(devid, rowcol, sizeof(int) * (factor->n_nonzero - dimension + 1));
  factor->rowpos = _sp_cl_upload(devid, rowpos, sizeof(int) * (factor->n_nonzero - dimension + 1));
  factor->diagpos = _sp_cl_upload(devid, diagpos, sizeof(int) * dimension);
  factor->levcols = _sp_cl_upload(devid, levcols, sizeof(int) * dimension);
  if(IS_NULL_PTR(factor->values) || IS_NULL_PTR(factor->colptr) || IS_NULL_PTR(factor->rowind)
     || IS_NULL_PTR(factor->contptr) || IS_NULL_PTR(factor->contsrc) || IS_NULL_PTR(factor->contljk)
     || IS_NULL_PTR(factor->posof) || IS_NULL_PTR(factor->rowptr) || IS_NULL_PTR(factor->rowcol)
     || IS_NULL_PTR(factor->rowpos) || IS_NULL_PTR(factor->diagpos) || IS_NULL_PTR(factor->levcols))
    goto fail;

  // level-scheduled numeric factorization: per level, one thread per matrix entry applies
  // every contribution scheduled onto its entry, then each column of the level finalizes
  // (sqrt of the diagonal, scale of the sub-diagonal)
  // Maths bridge, per level: sparse_chol_update_level forms  A[i,j] - sum_k L[i,k] L[j,k]  into
  // every entry (the cmod sum from pass 2); sparse_chol_final_level then sets
  // L[j,j] = sqrt(that diagonal value) and L[i,j] /= L[j,j] for the sub-diagonal rows -- together
  // the two recurrences at the top of this function, run in parallel across the level's columns.
  const double _tf2 = dt_get_wtime();
  {
    const int kernel_update = kernels.update_level;
    const int kernel_final = kernels.final_level;
    const int local_size = 64;
    for(int level = 0; level < factor->nlev; level++)
    {
      const int n_level_cols = factor->lev_off[level + 1] - factor->lev_off[level];
      if(!n_level_cols) continue;
      const int npos = factor->lev_pos_off[level + 1] - factor->lev_pos_off[level];
      if(npos)
      {
        size_t sizes[3] = { ROUNDUP(npos, 64), 1, 1 };
        dt_opencl_set_kernel_arg(devid, kernel_update, 0, sizeof(cl_mem), &factor->values);
        dt_opencl_set_kernel_arg(devid, kernel_update, 1, sizeof(cl_mem), &factor->contptr);
        dt_opencl_set_kernel_arg(devid, kernel_update, 2, sizeof(cl_mem), &factor->contsrc);
        dt_opencl_set_kernel_arg(devid, kernel_update, 3, sizeof(cl_mem), &factor->contljk);
        dt_opencl_set_kernel_arg(devid, kernel_update, 4, sizeof(cl_mem), &factor->posof);
        dt_opencl_set_kernel_arg(devid, kernel_update, 5, sizeof(int), &factor->lev_pos_off[level]);
        dt_opencl_set_kernel_arg(devid, kernel_update, 6, sizeof(int), &npos);
        if(dt_opencl_enqueue_kernel_2d(devid, kernel_update, sizes) != CL_SUCCESS) goto fail;
      }
      {
        size_t sizes[3] = { (size_t)n_level_cols * local_size, 1, 1 };
        size_t local[3] = { local_size, 1, 1 };
        dt_opencl_set_kernel_arg(devid, kernel_final, 0, sizeof(cl_mem), &factor->values);
        dt_opencl_set_kernel_arg(devid, kernel_final, 1, sizeof(cl_mem), &factor->colptr);
        dt_opencl_set_kernel_arg(devid, kernel_final, 2, sizeof(cl_mem), &factor->levcols);
        dt_opencl_set_kernel_arg(devid, kernel_final, 3, sizeof(int), &factor->lev_off[level]);
        dt_opencl_set_kernel_arg(devid, kernel_final, 4, sizeof(int), &n_level_cols);
        if(dt_opencl_enqueue_kernel_2d_with_local(devid, kernel_final, sizes, local) != CL_SUCCESS) goto fail;
      }
    }
  }

  {
    const double _tf3 = dt_get_wtime();
    dt_opencl_finish(devid);
    dt_print(DT_DEBUG_PERF,
             "[sparse cholesky] factor n=%d nnz=%d nlev=%d npairs=%llu: sym=%.0fms up=%.0fms"
             " enq=%.0fms gpu=%.0fms\n",
             dimension, factor->n_nonzero, factor->nlev, (unsigned long long)npairs, (_tf1 - _tf0) * 1e3,
             (_tf2 - _tf1) * 1e3, (_tf3 - _tf2) * 1e3, (dt_get_wtime() - _tf3) * 1e3);
  }

  free(parent);
  free(ancestor);
  free(workspace);
  free(sstk);
  free(colptr);
  free(rowind);
  free(colfill);
  free(col_level);
  free(updoff);
  free(updljk);
  free(updk);
  free(contptr_h);
  free(contsrc_h);
  free(contljk_h);
  free(fillc);
  free(posof_h);
  free(levcols);
  free(rowptr);
  free(rowcol);
  free(rowpos);
  free(diagpos);
  free(rowfill);
  free(values_host);
  return factor;

fail:
  free(parent);
  free(ancestor);
  free(workspace);
  free(sstk);
  free(colptr);
  free(rowind);
  free(colfill);
  free(col_level);
  free(updoff);
  free(updljk);
  free(updk);
  free(contptr_h);
  free(contsrc_h);
  free(contljk_h);
  free(fillc);
  free(posof_h);
  free(levcols);
  free(rowptr);
  free(rowcol);
  free(rowpos);
  free(diagpos);
  free(rowfill);
  free(values_host);
  _sp_chol_cl_free(factor);
  return NULL;
}

// Solve the factored system L L^T x = b on the GPU, with b in a device double buffer
// (n doubles), overwritten in place with the solution; level-scheduled forward then backward
// substitution. Returns 0 on success. gd = kernel handles.
// Mirrors _sp_chol_solve on the CPU: any change here must be mirrored there and re-validated
// with the HL_SPCL_TEST self-test (_sp_chol_cl_selftest).
//
// Maths bridge: x = A^{-1} b via the two triangular solves  L y = b  (forward) then  L^T x = y
// (backward), identical to the CPU _sp_chol_solve, but each solve runs level by level (all rows/
// columns of one level are mutually independent and solved by one work-group each). Forward uses
// the CSR row mirror of L; backward uses the native CSC columns of L (= rows of L^T).
static inline int _sp_chol_solve_cl(const _sp_chol_cl_t *const factor, const _sp_chol_cl_kernels_t kernels, cl_mem rhs)
{
  const int devid = factor->devid;
  const int local_size = 64;

  // forward substitution (L y = b), one launch per dependency level
  const int kernel_fwd = kernels.fwd_level;
  for(int level = 0; level < factor->nlev; level++)
  {
    const int n_level_cols = factor->lev_off[level + 1] - factor->lev_off[level];
    if(!n_level_cols) continue;
    size_t sizes[3] = { (size_t)n_level_cols * local_size, 1, 1 };
    size_t local[3] = { local_size, 1, 1 };
    dt_opencl_set_kernel_arg(devid, kernel_fwd, 0, sizeof(cl_mem), &rhs);
    dt_opencl_set_kernel_arg(devid, kernel_fwd, 1, sizeof(cl_mem), &factor->values);
    dt_opencl_set_kernel_arg(devid, kernel_fwd, 2, sizeof(cl_mem), &factor->rowptr);
    dt_opencl_set_kernel_arg(devid, kernel_fwd, 3, sizeof(cl_mem), &factor->rowcol);
    dt_opencl_set_kernel_arg(devid, kernel_fwd, 4, sizeof(cl_mem), &factor->rowpos);
    dt_opencl_set_kernel_arg(devid, kernel_fwd, 5, sizeof(cl_mem), &factor->diagpos);
    dt_opencl_set_kernel_arg(devid, kernel_fwd, 6, sizeof(cl_mem), &factor->levcols);
    dt_opencl_set_kernel_arg(devid, kernel_fwd, 7, sizeof(int), &factor->lev_off[level]);
    dt_opencl_set_kernel_arg(devid, kernel_fwd, 8, sizeof(int), &n_level_cols);
    dt_opencl_set_kernel_arg(devid, kernel_fwd, 9, sizeof(double) * local_size, NULL);
    if(dt_opencl_enqueue_kernel_2d_with_local(devid, kernel_fwd, sizes, local) != CL_SUCCESS) return 1;
  }

  // backward substitution (L^T x = y), one launch per backward dependency level
  const int kernel_bwd = kernels.bwd_level;
  for(int level = 0; level < factor->nlev_bwd; level++)
  {
    const int n_level_cols = factor->lev_off_bwd[level + 1] - factor->lev_off_bwd[level];
    if(!n_level_cols) continue;
    size_t sizes[3] = { (size_t)n_level_cols * local_size, 1, 1 };
    size_t local[3] = { local_size, 1, 1 };
    dt_opencl_set_kernel_arg(devid, kernel_bwd, 0, sizeof(cl_mem), &rhs);
    dt_opencl_set_kernel_arg(devid, kernel_bwd, 1, sizeof(cl_mem), &factor->values);
    dt_opencl_set_kernel_arg(devid, kernel_bwd, 2, sizeof(cl_mem), &factor->colptr);
    dt_opencl_set_kernel_arg(devid, kernel_bwd, 3, sizeof(cl_mem), &factor->rowind);
    dt_opencl_set_kernel_arg(devid, kernel_bwd, 4, sizeof(cl_mem), &factor->levrows_bwd);
    dt_opencl_set_kernel_arg(devid, kernel_bwd, 5, sizeof(int), &factor->lev_off_bwd[level]);
    dt_opencl_set_kernel_arg(devid, kernel_bwd, 6, sizeof(int), &n_level_cols);
    dt_opencl_set_kernel_arg(devid, kernel_bwd, 7, sizeof(double) * local_size, NULL);
    if(dt_opencl_enqueue_kernel_2d_with_local(devid, kernel_bwd, sizes, local) != CL_SUCCESS) return 1;
  }
  return 0;
}

#endif // HAVE_OPENCL
#endif // DT_MATH_SPARSE_CHOLESKY_CL_H
