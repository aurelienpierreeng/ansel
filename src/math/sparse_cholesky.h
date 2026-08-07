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

#ifndef DT_MATH_SPARSE_CHOLESKY_H
#define DT_MATH_SPARSE_CHOLESKY_H

// Reusable exact sparse SPD Cholesky solver (double precision), factored out of the
// highlights harmonic-transposition code. Header-only, like common/../choleski.h. Takes a
// symmetric-positive-definite matrix in upper-triangular compressed-sparse-column form and
// solves A x = b; the caller assembles the matrix (see e.g. the region PDE assembly in the
// highlights module). Large scratch buffers use the pipeline-cache arena, so the caller
// passes the arena id (dt_dev_pixelpipe_t.type) -- an int, NOT the pipeline itself:
// this file is maths and must not depend on develop/.

#include <glib.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "common/macros.h"
#include "common/pixelpipe_cache_alloc.h"

// Factored SPD matrix (lower-triangular Cholesky factor L, column-compressed).
typedef struct
{
  int dimension;
  int *col_ptr;   // dimension+1 column pointers
  int *row_index; // row indices
  double *values; // the FIRST entry of each column is the diagonal
} _sp_chol_t;

// ===== standalone sparse Cholesky for the region solvers ===================================
// Exact direct solver for the symmetric-positive-definite (SPD) diffusion systems, in DOUBLE
// precision (64-bit floats: single-precision conjugate gradient stalls -- its convergence
// degrades with the fourth power of the hole size -- and single-precision Cholesky loses the
// near-singular biharmonic modes). CSparse-style "up-looking" factorization A = L * L^T (L
// lower-triangular, computed row by row) with a GEOMETRIC nested-dissection ordering: the
// unknowns are 2D grid points, so the fill-reducing reordering that normally needs a generic
// heuristic (approximate minimum degree) comes free from recursive bisection of the pixel
// coordinates. No external dependency. Used by _biharmonic_dome (exact dome on a much finer
// grid than the dense O(N^3) solve allowed) and _region_pde_solve (exact solve, factor shared
// by the three chroma channels). Both keep their previous solver as fallback.



// release a CPU sparse Cholesky factor (arrays + struct); NULL-safe
static inline void _sp_chol_free(_sp_chol_t *factor)
{
  if(!factor) return;
  dt_pixelpipe_cache_free_align(factor->col_ptr);
  dt_pixelpipe_cache_free_align(factor->row_index);
  dt_pixelpipe_cache_free_align(factor->values);
  free(factor);
}

// In-place geometric nested dissection (a reordering of the unknowns that keeps the Cholesky
// factor sparse): rearrange ids[] so that recursive halves come first and their separating
// band (width = the stencil reach) comes last -- the elimination order that keeps Cholesky
// fill-in at the 2D-optimal O(N log N). Iterative with an explicit range stack.
// unknown_x/unknown_y give each unknown's pixel coordinates.
//
// Nested dissection (maths bridge): the unknowns are pixels of a 2D grid, and the PDE operators
// factored here (biharmonic Delta^2 L_sum = 0 for the luminance dome, screened Poisson
// Delta^2 r - lambda r = ... for the chrominance, anisotropic div(D grad p) = 0 for the coefficient
// transport -- article "Guided laplacian highlights", sections "Biharmonic inpainting" and "The
// optimization problem", energies E_bihar / E_chrominance / E_transport) are all local stencils, so
// two grid halves separated by a band of width `reach` interact ONLY through that band. Ordering
// each half's unknowns before the separator makes the halves' blocks factor with no mutual fill;
// only the (small) separator block fills. Recursing gives O(N log N) factor nonzeros / O(N^1.5)
// flops, versus O(N^1.5) fill / O(N^2) flops for the natural raster order. This geometric bisection
// replaces the generic approximate-minimum-degree heuristic a black-box sparse solver would need.
static inline void _sp_nd_order(int *const restrict unknown_ids, const int count, const int *const restrict unknown_x,
                         const int *const restrict unknown_y, const int reach)
{
  typedef struct
  {
    int begin, end;
  } _index_range;
  int capacity = 64;
  int stack_top = 0;
  _index_range *stack = (_index_range *)malloc(sizeof(_index_range) * capacity);
  if(!stack) return; // natural order still works, just with more fill
  stack[stack_top++] = (_index_range){ 0, count };

  // Iterative recursion over an explicit work stack (no call stack): each stack entry is a
  // still-to-dissect sub-range of unknown_ids[]. Each pass pops one range, splits its unknowns
  // into two halves plus a separator band, and pushes the two halves back to be dissected in turn.
  while(stack_top > 0)
  {
    // `range` = the [begin, end) slice of unknown_ids[] currently being dissected.
    const _index_range range = stack[--stack_top];
    const int length = range.end - range.begin;
    if(length <= 64) continue;

    int xmin = INT_MAX, xmax = INT_MIN, ymin = INT_MAX, ymax = INT_MIN;
    for(int i = range.begin; i < range.end; i++)
    {
      const int unknown_id = unknown_ids[i];
      xmin = MIN(xmin, unknown_x[unknown_id]);
      xmax = MAX(xmax, unknown_x[unknown_id]);
      ymin = MIN(ymin, unknown_y[unknown_id]);
      ymax = MAX(ymax, unknown_y[unknown_id]);
    }

    const int extent_x = xmax - xmin + 1;
    const int extent_y = ymax - ymin + 1;
    if(MAX(extent_x, extent_y) <= 2 * reach + 1) continue; // too thin to dissect

    // bisect along the longer axis (keeps the separator band as short as possible => less fill),
    // cutting at the midpoint coordinate of that axis
    const int split_on_x = (extent_x >= extent_y);
    const int *const coord = split_on_x ? unknown_x : unknown_y;
    const int cut_position = (split_on_x ? xmin + extent_x / 2 : ymin + extent_y / 2);

    // Three-way partition of the unknowns in unknown_ids[range.begin .. range.end) by each one's
    // coordinate along the split axis, into: [ < cut_position | >= cut_position + reach | separator ].
    // The separator (the middle band of width `reach`) is moved to the tail so it is eliminated
    // last; the two flanking halves are pushed back onto the stack to dissect recursively.
    int left_end = range.begin;
    int right_end = range.end;
    int i = range.begin;
    while(i < right_end)
    {
      const int coord_value = coord[unknown_ids[i]];
      if(coord_value < cut_position)
      {
        const int swap_id = unknown_ids[i];
        unknown_ids[i] = unknown_ids[left_end];
        unknown_ids[left_end] = swap_id;
        left_end++;
        i++;
      }
      else if(coord_value >= cut_position + reach)
      {
        i++;
      }
      else
      {
        right_end--;
        const int swap_id = unknown_ids[i];
        unknown_ids[i] = unknown_ids[right_end];
        unknown_ids[right_end] = swap_id;
      }
    }
    // now [range.begin, left_end) = left, [left_end, right_end) = right,
    // [right_end, range.end) = separator (eliminated last)
    if(left_end == range.begin && right_end == range.end) continue; // no separator found: done
    if(stack_top + 2 > capacity)
    {
      capacity *= 2;
      _index_range *grown = (_index_range *)realloc(stack, sizeof(_index_range) * capacity);
      if(!grown) break;
      stack = grown;
    }
    if(left_end - range.begin > 64) stack[stack_top++] = (_index_range){ range.begin, left_end };
    if(right_end - left_end > 64) stack[stack_top++] = (_index_range){ left_end, right_end };
  }
  free(stack);
}

// Elimination tree (the column dependency order of the factorization: each column's parent is
// the first column that uses its result) of an upper-triangular compressed-sparse-column (CSC)
// matrix. Liu's classic algorithm with path compression via `ancestor`.
//
// Maths bridge: parent[k] = min{ i > k : L[i,k] != 0 } = the first row below the diagonal in
// column k of the factor L, i.e. the first column whose elimination consumes column k's result.
// This forest is exactly the column-dependency DAG the GPU level schedule parallelizes (columns
// with disjoint root-paths are independent); on the CPU it drives _sp_ereach's pattern walk.
static inline void _sp_etree(const int dimension, const int *const restrict col_ptr, const int *const restrict row_index,
                      int *const restrict parent, int *const restrict ancestor)
{
  // Not OpenMP-parallelizable: `ancestor` is path-compressed across columns, so column k reads and
  // rewrites ancestor[] entries that earlier columns set -- a loop-carried dependency a parallel
  // for would race on. It is also cheap (O(nnz * inverse-Ackermann)), far below the numeric
  // factorization; the parallel factorization is the level-scheduled OpenCL path, not this.
  for(int k = 0; k < dimension; k++)
  {
    parent[k] = -1;
    ancestor[k] = -1;
    for(int entry = col_ptr[k]; entry < col_ptr[k + 1]; entry++)
    {
      int i = row_index[entry];
      // climb the partial tree from each above-diagonal nonzero A[i,k] (i < k) toward the root,
      // compressing the path so every visited node points directly at k
      while(i != -1 && i < k)
      {
        const int next_ancestor = ancestor[i];
        ancestor[i] = k;
        if(next_ancestor == -1) parent[i] = k; // i had no parent yet: k is its first user => parent[i] = k
        i = next_ancestor;
      }
    }
  }
}

// "Elimination reach": the set of columns that contribute to row k of the Cholesky factor
// (mathematically L; stored column-compressed in the returned _sp_chol_t as its
// values / row_index / col_ptr arrays) -- i.e. row k's nonzero pattern excluding the diagonal,
// found by walking the elimination tree upward from each entry of column k of the input matrix A;
// returned in topological (dependency) order. Returns `stack_top` such that
// pattern_stack[stack_top..dimension-1] holds the pattern. mark[] holds per-k marks
// (mark[i] == k means visited).
//
// Maths bridge: row k of L has a nonzero L[k,j] exactly for the columns j reachable from the
// above-diagonal nonzeros of A's column k by walking parent[] up the elimination tree (the
// symbolic Cholesky pattern theorem). Those j are precisely the columns whose contribution the
// numeric factor must subtract when forming row k -- returned deepest-ancestor-last so the numeric
// sweep applies them in valid dependency order (each L[j,*] already finalized when read).
static inline int _sp_ereach(const int dimension, const int *const restrict col_ptr, const int *const restrict row_index,
                      const int k, const int *const restrict parent, int *const restrict pattern_stack,
                      int *const restrict mark)
{
  int stack_top = dimension;
  mark[k] = k;
  // Not OpenMP-parallelizable: the entries share `mark` (which dedups nodes already on the reach)
  // and the single output stack, so parallel iterations would race both. The enclosing per-column
  // loops in _sp_chol_factor are sequential anyway (column k depends on every column in its reach),
  // which is why the parallel solver is the level-scheduled OpenCL path, not an OpenMP version here.
  for(int entry = col_ptr[k]; entry < col_ptr[k + 1]; entry++)
  {
    int i = row_index[entry];
    if(i >= k) continue;
    int length = 0;
    for(; mark[i] != k; i = parent[i])
    {
      pattern_stack[length++] = i;
      mark[i] = k;
    }
    while(length > 0)
    {
      stack_top--;
      pattern_stack[stack_top] = pattern_stack[--length];
      // keep the path contiguous at the back: shift is avoided by the two-stack trick below
    }
  }
  return stack_top;
}

// Up-looking sparse Cholesky  A = L * L^T  (Cholesky-Banachiewicz, row by row): factors the SPD
// system of the region PDE / biharmonic dome (article "Guided laplacian highlights", sections
// "Biharmonic inpainting" and "The optimization problem"). For each row k the elimination reach
// (_sp_ereach) gives the columns j < k that contribute; the classic recurrences realized below are
//   off-diagonal   L[k,j] = ( A[k,j] - sum_{m<j} L[k,m] L[j,m] ) / L[j,j]      (j in reach of k)
//   diagonal       L[k,k] = sqrt( A[k,k] - sum_{j<k} L[k,j]^2 )
// implemented with a dense scratch row `work[]` (the running numerator A[k,*] - accumulated
// products) that is scattered from column A[:,k], reduced by each reach column j, then read off.
// Notation bridge (math symbol -> code name):
//   A          the input matrix         -> matrix_col_ptr / matrix_row_index / matrix_values
//   L          the computed factor      -> factor->col_ptr / factor->row_index / factor->values
//   L[k,k]     the pivot (pre-sqrt)     -> `pivot`
//   L[k,j]     multiplier applied from earlier column j -> `multiplier`
//   the dense scratch row being eliminated for column k -> `work[]`
// Returns NULL if the matrix turns out not positive definite or on out-of-memory.
static inline _sp_chol_t *_sp_chol_factor(const int dimension, const int *const restrict matrix_col_ptr,
                                   const int *const restrict matrix_row_index,
                                   const double *const restrict matrix_values, const int cache_id)
{
  // every O(dimension) or larger buffer lives in the pipeline-cache arena, so the LRU can evict
  // cachelines to make room instead of the factorization competing blindly with them
  _sp_chol_t *factor = (_sp_chol_t *)calloc(1, sizeof(_sp_chol_t));
  int *parent = dt_pixelpipe_cache_alloc_align_int_cache(dimension, cache_id);
  int *ancestor = dt_pixelpipe_cache_alloc_align_int_cache(dimension, cache_id);
  int *mark = dt_pixelpipe_cache_alloc_align_int_cache(dimension, cache_id);
  int *elim_stack = dt_pixelpipe_cache_alloc_align_int_cache(dimension, cache_id);
  int *col_count = dt_pixelpipe_cache_alloc_align_int_cache(dimension, cache_id);
  int *col_fill = dt_pixelpipe_cache_alloc_align_int_cache(dimension, cache_id);
  double *work = dt_pixelpipe_cache_alloc_align_double_cache(dimension, cache_id);
  if(!factor || IS_NULL_PTR(parent) || IS_NULL_PTR(ancestor) || IS_NULL_PTR(mark) || IS_NULL_PTR(elim_stack)
     || IS_NULL_PTR(col_count) || IS_NULL_PTR(col_fill) || IS_NULL_PTR(work))
    goto fail;

  _sp_etree(dimension, matrix_col_ptr, matrix_row_index, parent, ancestor);

  // symbolic: column counts of L (each row-k pattern entry j adds one entry to column j)
  for(int i = 0; i < dimension; i++)
  {
    mark[i] = -1;
    col_count[i] = 1; // diagonal
  }
  for(int k = 0; k < dimension; k++)
  {
    const int reach_top = _sp_ereach(dimension, matrix_col_ptr, matrix_row_index, k, parent, elim_stack, mark);
    for(int reach_pos = reach_top; reach_pos < dimension; reach_pos++) col_count[elim_stack[reach_pos]]++;
  }

  factor->dimension = dimension;
  factor->col_ptr = dt_pixelpipe_cache_alloc_align_int_cache((size_t)dimension + 1, cache_id);
  if(IS_NULL_PTR(factor->col_ptr)) goto fail;
  factor->col_ptr[0] = 0;
  for(int i = 0; i < dimension; i++) factor->col_ptr[i + 1] = factor->col_ptr[i] + col_count[i];
  const size_t nonzeros = factor->col_ptr[dimension];
  factor->row_index = dt_pixelpipe_cache_alloc_align_int_cache(nonzeros, cache_id);
  factor->values = dt_pixelpipe_cache_alloc_align_double_cache(nonzeros, cache_id);
  if(IS_NULL_PTR(factor->row_index) || IS_NULL_PTR(factor->values)) goto fail;

  // numeric
  for(int i = 0; i < dimension; i++)
  {
    mark[i] = -1;
    work[i] = 0.0;
    col_fill[i] = factor->col_ptr[i] + 1; // slot 0 = diagonal, filled when row i is processed
  }
  // Not OpenMP-parallelizable as written: column k subtracts contributions from every earlier
  // column in its elimination reach (reading their finished factor->values and accumulating into
  // the shared `work` row), so the k-loop carries a true dependency and the inner reach-loop
  // scatters into overlapping work[] entries -- neither is a safe parallel for. This is the
  // bit-reproducible CPU reference the self-tests validate the GPU against; the parallel
  // factorization is the OpenCL path (_sp_chol_factor_cl), which first builds an elimination-tree
  // level schedule (independent columns per level) precisely because this straight column sweep
  // cannot be parallelized in place.
  for(int k = 0; k < dimension; k++)
  {
    const int reach_top = _sp_ereach(dimension, matrix_col_ptr, matrix_row_index, k, parent, elim_stack, mark);
    // seed the scratch row with column k of A: work[i] = A[k,i] for i<k (numerator of L[k,i]),
    // pivot = A[k,k] (numerator of the diagonal, before the sum of squares is subtracted)
    double pivot = 0.0;
    for(int entry = matrix_col_ptr[k]; entry < matrix_col_ptr[k + 1]; entry++)
    {
      const int i = matrix_row_index[entry];
      if(i < k)
        work[i] = matrix_values[entry];
      else if(i == k)
        pivot = matrix_values[entry];
    }
    for(int reach_pos = reach_top; reach_pos < dimension; reach_pos++)
    {
      const int j = elim_stack[reach_pos];
      // L[k,j] = ( A[k,j] - sum_{m<j} L[k,m] L[j,m] ) / L[j,j]: work[j] holds the fully-reduced
      // numerator here (every earlier reach column m<j already subtracted its L[k,m]L[j,m]),
      // values[col_ptr[j]] = L[j,j] (the diagonal is stored first in each column)
      const double multiplier = work[j] / factor->values[factor->col_ptr[j]];
      work[j] = 0.0;
      // apply column j's contribution to the still-pending numerators: work[i] -= L[i,j] * L[k,j]
      // for the below-diagonal rows i of column j (this is the "up-looking" left-of-diagonal update)
      for(int entry = factor->col_ptr[j] + 1; entry < col_fill[j]; entry++)
        work[factor->row_index[entry]] -= factor->values[entry] * multiplier;
      pivot -= multiplier * multiplier; // subtract L[k,j]^2 from the diagonal accumulator A[k,k]
      // store L[k,j] as entry (row k) of column j -- CSC lower-triangular: (k,j) with k>j lives in column j
      const int slot = col_fill[j]++;
      factor->row_index[slot] = k;
      factor->values[slot] = multiplier;
    }
    if(!(pivot > 0.0)) goto fail; // pivot = A[k,k] - sum_j L[k,j]^2 <= 0 (or NaN): matrix not SPD
    factor->row_index[factor->col_ptr[k]] = k;
    factor->values[factor->col_ptr[k]] = sqrt(pivot); // L[k,k] = sqrt( A[k,k] - sum_j L[k,j]^2 )
  }

  dt_pixelpipe_cache_free_align(parent);
  dt_pixelpipe_cache_free_align(ancestor);
  dt_pixelpipe_cache_free_align(mark);
  dt_pixelpipe_cache_free_align(elim_stack);
  dt_pixelpipe_cache_free_align(col_count);
  dt_pixelpipe_cache_free_align(col_fill);
  dt_pixelpipe_cache_free_align(work);
  return factor;

fail:
  dt_pixelpipe_cache_free_align(parent);
  dt_pixelpipe_cache_free_align(ancestor);
  dt_pixelpipe_cache_free_align(mark);
  dt_pixelpipe_cache_free_align(elim_stack);
  dt_pixelpipe_cache_free_align(col_count);
  dt_pixelpipe_cache_free_align(col_fill);
  dt_pixelpipe_cache_free_align(work);
  _sp_chol_free(factor);
  return NULL;
}

// Solve the factored system L L^T x = b in place (forward substitution then backward).
// Notation bridge (math symbol -> code name): L is the factor (factor->values / row_index /
// col_ptr); b, the intermediate y, and the solution x all live in `rhs`, overwritten in place
// (rhs holds b on entry, y after the forward sweep, x after the backward sweep).
// `forward_value` = y[j]; `accum` = x[j] accumulated before its final divide by the diagonal.
// Two triangular solves realize x = A^{-1} b via  L y = b  then  L^T x = y.
static inline void _sp_chol_solve(const _sp_chol_t *const factor, double *const restrict rhs)
{
  const int dimension = factor->dimension;
  for(int j = 0; j < dimension; j++) // forward: L y = b  (column-oriented: y_j = (b_j - sum_{i<j} L[j,i] y_i)/L[j,j])
  {
    const double forward_value = rhs[j] / factor->values[factor->col_ptr[j]]; // y_j = (reduced b_j) / L[j,j]
    rhs[j] = forward_value;
    // once y_j is known, push its contribution forward: b_i -= L[i,j] y_j for all rows i>j of column j
    for(int entry = factor->col_ptr[j] + 1; entry < factor->col_ptr[j + 1]; entry++)
      rhs[factor->row_index[entry]] -= factor->values[entry] * forward_value;
  }
  for(int j = dimension - 1; j >= 0; j--) // backward: L^T x = y  (x_j = (y_j - sum_{i>j} L[i,j] x_i)/L[j,j])
  {
    double accum = rhs[j]; // y_j
    // gather the already-solved x_i (rows i>j of column j = columns i>j of row j in L^T)
    for(int entry = factor->col_ptr[j] + 1; entry < factor->col_ptr[j + 1]; entry++)
      accum -= factor->values[entry] * rhs[factor->row_index[entry]];
    rhs[j] = accum / factor->values[factor->col_ptr[j]]; // x_j = (y_j - sum_{i>j} L[i,j] x_i) / L[j,j]
  }
}
#endif // DT_MATH_SPARSE_CHOLESKY_H
