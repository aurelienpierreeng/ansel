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

/*
    Sparse SPD Cholesky on the GPU, for the highlights harmonic-transposition solvers.

    The host performs the symbolic analysis (nested-dissection ordering, elimination tree,
    L pattern, update schedules, level schedule) on integer metadata; these kernels run the
    NUMERIC factorization and the triangular solves in double precision, level-scheduled over
    the elimination tree: all columns of one level are independent and factor in parallel
    (one work-group per column), the sequential tail near the root is a handful of levels.

    All irregularity is resolved at symbolic time: every cmod(j,k) contribution is grouped by its
    DESTINATION entry, so the numeric kernel runs one thread per matrix entry -- a pure
    gather-fma stream with no atomics and a deterministic summation order.
*/

// Plain-words guide to the header above. "Sparse Cholesky" is an exact direct solver for the
// diffusion equations of the highlight reconstruction: the system matrix ("SPD" = symmetric
// positive definite, the matrix shape this factorization requires) is factorized once into a
// lower-triangular factor L and reused for all three colour channels. The host (CPU) does all
// the integer bookkeeping ahead of time; these kernels only crunch numbers, in double
// precision (64-bit floats, needed because the diffusion systems amplify rounding errors too
// much in 32-bit). "Level" = a set of matrix columns that can be processed in parallel
// because they do not depend on each other; the schedule is precomputed on the CPU. A
// "cmod(j,k)" update = subtract a multiple of an earlier column k from column j; each one is
// a flat precomputed list of (src, dst) index pairs, so the kernels are pure
// gather-multiply-subtract streams ("fma" = fused multiply-add).
// Mirrors the CPU _sp_chol_factor / _sp_chol_solve (src/iop/highlights_harmonic.h) -- any
// change here must be mirrored there and re-validated with the HL_SPCL_TEST self-test.

#if defined(cl_khr_fp64)
#pragma OPENCL EXTENSION cl_khr_fp64 : enable
#define HL_SPARSE_FP64 1
#elif defined(cl_amd_fp64)
#pragma OPENCL EXTENSION cl_amd_fp64 : enable
#define HL_SPARSE_FP64 1
#endif

#ifdef HL_SPARSE_FP64

// Numeric factorization of one level, update half: ONE THREAD PER MATRIX ENTRY of the
// level's columns. The host groups every cmod contribution BY DESTINATION at symbolic time
// (cont_ptr/cont_src/cont_ljk: for entry d, the list of (source, L[j,k]) value positions that
// subtract into it, in ascending update order), so each thread accumulates its own entry with
// no atomics, no barriers, and the exact sequential summation order -- bit-reproducible, and
// the whole device is busy even on the thin levels near the elimination-tree root (the old
// one-work-group-per-column form idled ~98% of the device there). pos_of lists the entry
// positions grouped by level.
kernel void
sparse_chol_update_level(global double *values,
                         global const int *cont_ptr,    // nnz+1: contribution range per entry
                         global const int *cont_src,    // per contribution: source position in Lx
                         global const int *cont_ljk,    // per contribution: position of L[j,k]
                         global const int *pos_of,      // nnz: entry positions grouped by level
                         const int lev_start,
                         const int npos)
{
  const int thread_index = get_global_id(0);
  if(thread_index >= npos) return;
  const int entry = pos_of[lev_start + thread_index];
  // accum = A[i,j] - sum_k L[i,k]*L[j,k], the pre-scale numerator of entry (i,j) of L:
  // values[entry] starts at A[i,j]; each contribution subtracts one product L[j,k]*L[i,k]
  // (cont_ljk -> position of L[j,k], cont_src -> position of L[i,k]), in ascending-k order
  double accum = values[entry];
  for(int contribution = cont_ptr[entry]; contribution < cont_ptr[entry + 1]; contribution++)
    accum -= values[cont_ljk[contribution]] * values[cont_src[contribution]];
  values[entry] = accum;
}

// Numeric factorization of one level, finalize half: one work-group per column j of the
// level -- square-root the diagonal entry (stored first in each column) and divide the
// entries below the diagonal by it. Runs after sparse_chol_update_level of the same level.
kernel void
sparse_chol_final_level(global double *values,
                        global const int *colptr,      // n+1, diag first per column
                        global const int *levelcols,
                        const int lev_start,
                        const int nlevel)
{
  const int group = get_group_id(0);
  if(group >= nlevel) return;
  const int j = levelcols[lev_start + group];
  const int local_id = get_local_id(0);
  const int local_size = get_local_size(0);

  // L[j,j] = sqrt( A[j,j] - sum_k L[j,k]^2 ): the diagonal slot already holds the numerator from
  // sparse_chol_update_level, so finalizing is just its square root
  if(local_id == 0) values[colptr[j]] = sqrt(values[colptr[j]]);
  barrier(CLK_GLOBAL_MEM_FENCE);
  const double diag_value = values[colptr[j]];
  // L[i,j] = ( A[i,j] - sum_k L[i,k] L[j,k] ) / L[j,j]: divide each sub-diagonal numerator by L[j,j]
  for(int entry = colptr[j] + 1 + local_id; entry < colptr[j + 1]; entry += local_size)
    values[entry] /= diag_value;
}

// Forward triangular solve (L y = b), first half of applying the factorization to one
// right-hand side. One work-group per row j of the level: threads gather the already-solved
// entries of x referenced by row j through a row-oriented mirror of L ("CSR" = compressed
// sparse row layout), tree-sum the products in local memory (reduction), and thread 0
// finishes x[j]. In/out: x holds the right-hand side b on entry and y on exit.
kernel void
sparse_chol_fwd_level(global double *solution,
                      global const double *values,
                      global const int *rowptr,     // n+1: off-diagonal entries of row j
                      global const int *rowcol,     // column index k of each row entry
                      global const int *rowpos,     // position of L[j,k] in Lx
                      global const int *diagpos,    // n: position of L[j,j] in Lx
                      global const int *levelrows,
                      const int lev_start,
                      const int nlevel,
                      local double *scratch)
{
  const int group = get_group_id(0);
  if(group >= nlevel) return;
  const int j = levelrows[lev_start + group];
  const int local_id = get_local_id(0);
  const int local_size = get_local_size(0);

  // y_j = ( b_j - sum_{k<j} L[j,k] y_k ) / L[j,j]: gather the products L[j,k]*y_k over row j's
  // off-diagonal entries (already-solved y_k), tree-reduce them, then thread 0 divides by L[j,j]
  double accum = 0.0;
  for(int entry = rowptr[j] + local_id; entry < rowptr[j + 1]; entry += local_size)
    accum += values[rowpos[entry]] * solution[rowcol[entry]]; // += L[j,k] * y_k
  scratch[local_id] = accum;
  barrier(CLK_LOCAL_MEM_FENCE);
  for(int offset = local_size / 2; offset > 0; offset /= 2)
  {
    if(local_id < offset) scratch[local_id] += scratch[local_id + offset];
    barrier(CLK_LOCAL_MEM_FENCE);
  }
  if(local_id == 0) solution[j] = (solution[j] - scratch[0]) / values[diagpos[j]]; // (b_j - sum) / L[j,j]
}

// Backward triangular solve (L^T x = y, the transpose of L), second half. One work-group per
// column j of the level: threads gather the entries of x below the diagonal of column j
// ("CSC" = compressed sparse column, the native layout of L), tree-sum the products in local
// memory, and thread 0 finishes x[j]. In/out: x holds y on entry and the solution on exit.
kernel void
sparse_chol_bwd_level(global double *solution,
                      global const double *values,
                      global const int *colptr,
                      global const int *rowind,     // row index of each column entry (diag first)
                      global const int *levelcols,
                      const int lev_start,
                      const int nlevel,
                      local double *scratch)
{
  const int group = get_group_id(0);
  if(group >= nlevel) return;
  const int j = levelcols[lev_start + group];
  const int local_id = get_local_id(0);
  const int local_size = get_local_size(0);

  // x_j = ( y_j - sum_{i>j} L[i,j] x_i ) / L[j,j]: gather the products L[i,j]*x_i over the
  // below-diagonal entries of column j (= row j of L^T, the already-solved x_i), reduce, divide
  double accum = 0.0;
  for(int entry = colptr[j] + 1 + local_id; entry < colptr[j + 1]; entry += local_size)
    accum += values[entry] * solution[rowind[entry]]; // += L[i,j] * x_i
  scratch[local_id] = accum;
  barrier(CLK_LOCAL_MEM_FENCE);
  for(int offset = local_size / 2; offset > 0; offset /= 2)
  {
    if(local_id < offset) scratch[local_id] += scratch[local_id + offset];
    barrier(CLK_LOCAL_MEM_FENCE);
  }
  if(local_id == 0) solution[j] = (solution[j] - scratch[0]) / values[colptr[j]]; // (y_j - sum) / L[j,j]
}

// Right-hand side (the constant vector b) of the screened chroma diffusion solve inside an
// all-clip core (area where all three channels clipped), one work-item per unknown hole
// pixel. "Screened" = an extra pull toward a flat target colour (react * cmeanc, controlled
// by the user's "inpaint a flat colour" slider). The known rim values enter through the
// 9-point Laplacian of the boundary-embedded plane t1 (the chroma ratio outside the hole,
// zero inside): a weighted average of the 8 neighbours (axis weight 4, diagonal weight 1,
// divided by 6) minus the centre, edge-clamped -- exactly the CPU _lap5. pgrid maps unknown
// index -> pixel index. Mirrors the right-hand-side assembly of the CPU _region_pde_solve --
// any change here must be mirrored there and re-validated with the HL_CORECL_TEST self-test.
// Maths bridge: this is the RHS b of the screened-Poisson system that minimizes E_chrominance =
// integral( |grad r|^2 + lambda |r|^2 )  (article "The optimization problem", energy 3), whose
// Euler-Lagrange equation is  Delta r - lambda r = -lambda r_target . Discretized, the SPD system
// solved by the Cholesky factor is (lambda I - Delta) r = lambda r_target + (Delta of the known
// rim, moved to the RHS). Here react = lambda, cmeanc = the flat target colour r_target (the
// "inpaint a flat colour" bias), `laplacian` = the boundary-embedded plane's Laplacian carrying
// the known rim into b. The obstacle constraint r_c >= c0/L of E_chrominance is enforced elsewhere.
kernel void
hl_pde_rhs(global const float *boundary_ratio, global const int *pgrid, global double *rhs,
           const int n_unknowns, const int width, const int height, const float react, const float cmeanc)
{
  const int unknown = get_global_id(0);
  if(unknown >= n_unknowns) return;
  const int grid_index = pgrid[unknown];
  const int y = grid_index / width;
  const int x = grid_index - y * width;
  const int y_north = (y > 0) ? y - 1 : y;
  const int y_south = (y < height - 1) ? y + 1 : y;
  const int x_west = (x > 0) ? x - 1 : x;
  const int x_east = (x < width - 1) ? x + 1 : x;
  const float center = boundary_ratio[y * width + x];
  const float north = boundary_ratio[y_north * width + x];
  const float south = boundary_ratio[y_south * width + x];
  const float west = boundary_ratio[y * width + x_west];
  const float east = boundary_ratio[y * width + x_east];
  const float val_nw = boundary_ratio[y_north * width + x_west];
  const float val_ne = boundary_ratio[y_north * width + x_east];
  const float val_sw = boundary_ratio[y_south * width + x_west];
  const float val_se = boundary_ratio[y_south * width + x_east];
  // 9-point discrete Laplacian of the rim-embedded ratio plane (Delta applied to the known boundary)
  const float laplacian = (4.f * (north + south + west + east) + (val_nw + val_ne + val_sw + val_se) - 20.f * center) / 6.f;
  rhs[unknown] = (double)react * (double)cmeanc + (double)laplacian; // b = lambda*r_target + Delta(rim)
}

// Scatter the solved chroma values (double-precision vector b, one entry per unknown hole
// pixel) back into the full ratio plane rat at the pixel positions listed in pgrid, clamped
// at zero exactly like the CPU _region_pde_solve does.
kernel void
hl_pde_scatter(global const double *rhs, global const int *pgrid, global float *ratio, const int n_unknowns)
{
  const int unknown = get_global_id(0);
  if(unknown >= n_unknowns) return;
  ratio[pgrid[unknown]] = fmax((float)rhs[unknown], 0.f); // project the solved ratio onto r >= 0
}

// Right-hand side of the edge-aware (anisotropic: smooth ALONG image edges, not across them)
// chroma diffusion solve, one work-item per unknown hole pixel. For each of the 8 neighbour
// directions: if the precomputed edge weight w8 is positive and in-bounds, and the neighbour
// is a valid anchor pixel (vldan >= 0.5 = real data, so it lives outside the matrix), add
// weight x that neighbour's ratio value, accumulated in double precision like the CPU
// assembly. Mirrors the CPU _aniso_div_solve -- any change here must be mirrored there and
// re-validated with the HL_ANISOCL_TEST self-test.
// Maths bridge: this assembles b for the anisotropic transport E_transport = integral( grad p^T D
// grad p )  (article "The optimization problem", term 1b), whose Euler-Lagrange equation is
// div(D grad p) = 0 -- a weighted-Laplacian SPD system sum_k w_k (p_k - p_center) = 0. The
// Weickert weights w_k = edge_weights steer D (isophote- vs gradient-dominant). Neighbours that
// are valid anchors (data outside the matrix) move to the RHS as sum_k w_k * p_k; hole neighbours
// stay in the matrix. The matrix diagonal (sum of weights) is assembled on the host side.
kernel void
hl_aniso_rhs(global const float *edge_weights, global const float *valid_anchor, global const float *chroma,
             global const int *pgrid, global double *rhs,
             const int n_unknowns, const int width, const int height, const int c)
{
  const int unknown = get_global_id(0);
  if(unknown >= n_unknowns) return;
  const int grid_index = pgrid[unknown];
  const int origin_y = grid_index / width;
  const int origin_x = grid_index - origin_y * width;
  const int neighbor_dy[8] = { 0, 0, -1, 1, -1, 1, -1, 1 };
  const int neighbor_dx[8] = { -1, 1, 0, 0, -1, 1, 1, -1 };
  double accum = 0.0;
  for(int direction = 0; direction < 8; direction++)
  {
    const float weight_value = edge_weights[unknown * 8 + direction];
    if(weight_value <= 0.f) continue;
    const int neighbor_x = origin_x + neighbor_dx[direction], neighbor_y = origin_y + neighbor_dy[direction];
    const int j = neighbor_y * width + neighbor_x;
    if(valid_anchor[j * 4 + 0] < 0.5f) continue;   // hole neighbour: lives in the matrix (LHS), not the RHS
    accum += (double)weight_value * (double)chroma[j * 4 + c]; // += w_k * p_k for anchor neighbours
  }
  rhs[unknown] = accum; // b = sum_{k in anchors} w_k p_k
}

// Scatter the solved ratio values back into channel c of the packed 4-float-per-pixel plane
// s1 at the positions listed in pgrid (no clamp here: the CPU reassembles the raw values and
// clamps later, so the GPU must match).
kernel void
hl_aniso_scatter(global const double *rhs, global const int *pgrid, global float *chroma,
                 const int n_unknowns, const int c)
{
  const int unknown = get_global_id(0);
  if(unknown >= n_unknowns) return;
  chroma[pgrid[unknown] * 4 + c] = (float)rhs[unknown];
}

// CG (conjugate gradient) kernels: the iterative solver used instead of the direct
// factorization above when the clipped area is too large; it repeats matrix-vector products
// until the error is small. Each kernel fuses one vector update with a partial dot product:
// threads stride the 1D arrays, accumulate in double precision, tree-sum in local memory,
// and each work-group writes one partial sum to `partial` (the small array of per-group
// results is finished on the CPU). hole = byte mask of the unknown pixels; everything
// outside it is untouched or zeroed. Mirrors the CG fallback of the CPU _region_pde_solve,
// driven by _region_pde_cg_cl -- any change here must be mirrored there and re-validated
// with the HL_CORECL_TEST self-test.

// First CG step, computing the initial residual and search direction:
// r -= dscalar*u + t2 (t2 holds the Laplacian term of the initial guess u, computed by the
// hl_cg_op kernel in highlights_harmonic.cl); p = r; emits per-group partial sums of r*r.
// Non-hole pixels get their search direction zeroed.
// Maths bridge: the operator is A = dscalar*I + (-Delta) (the same screened-Poisson E_chrominance
// SPD matrix the direct solver factors); this forms the initial residual r = b - A u and search
// direction p = r, and accumulates ||r||^2 (the CG numerator / convergence measure).
kernel void
hl_cg_r1(global float *residual, global float *search_dir, global const float *solution, global const float *laplacian_term,
         global const uchar *hole, global double *partial, const int dimension, const float dscalar,
         local double *scratch)
{
  const int global_id = get_global_id(0);
  const int global_size = get_global_size(0);
  const int local_id = get_local_id(0);
  const int local_size = get_local_size(0);
  double accum = 0.0;
  for(int i = global_id; i < dimension; i += global_size)
  {
    if(!hole[i]) { search_dir[i] = 0.f; continue; }
    residual[i] -= dscalar * solution[i] + laplacian_term[i]; // r = b - A u  (A u = dscalar*u - Delta u)
    search_dir[i] = residual[i];                              // p = r
    accum += (double)residual[i] * residual[i];               // += r_i^2  -> ||r||^2
  }
  scratch[local_id] = accum;
  barrier(CLK_LOCAL_MEM_FENCE);
  for(int offset = local_size / 2; offset > 0; offset /= 2)
  {
    if(local_id < offset) scratch[local_id] += scratch[local_id + offset];
    barrier(CLK_LOCAL_MEM_FENCE);
  }
  if(local_id == 0) partial[get_group_id(0)] = scratch[0];
}

// Matrix-vector product of one CG iteration: ap = dscalar*p + t2 (t2 = Laplacian term of the
// search direction p, from hl_cg_op) on hole pixels, zero elsewhere. Emits per-group partial
// sums of p*ap, the denominator of the CG step size alpha (finished on the CPU).
kernel void
hl_cg_ap(global float *matvec, global const float *search_dir, global const float *laplacian_term,
         global const uchar *hole, global double *partial, const int dimension, const float dscalar,
         local double *scratch)
{
  const int global_id = get_global_id(0);
  const int global_size = get_global_size(0);
  const int local_id = get_local_id(0);
  const int local_size = get_local_size(0);
  double accum = 0.0;
  for(int i = global_id; i < dimension; i += global_size)
  {
    if(!hole[i]) { matvec[i] = 0.f; continue; }
    matvec[i] = dscalar * search_dir[i] + laplacian_term[i]; // ap = A p  (dscalar*p - Delta p)
    accum += (double)search_dir[i] * matvec[i];              // += p_i * ap_i  -> p.Ap (alpha denominator)
  }
  scratch[local_id] = accum;
  barrier(CLK_LOCAL_MEM_FENCE);
  for(int offset = local_size / 2; offset > 0; offset /= 2)
  {
    if(local_id < offset) scratch[local_id] += scratch[local_id + offset];
    barrier(CLK_LOCAL_MEM_FENCE);
  }
  if(local_id == 0) partial[get_group_id(0)] = scratch[0];
}

// CG solution and residual update: advance the solution along the search direction
// (u += alpha*p), update the residual (r -= alpha*ap), and emit per-group partial sums of
// the new r*r for the convergence check (finished on the CPU). Hole pixels only.
kernel void
hl_cg_update(global float *solution, global float *residual, global const float *search_dir, global const float *matvec,
             global const uchar *hole, global double *partial, const int dimension, const float alpha,
             local double *scratch)
{
  const int global_id = get_global_id(0);
  const int global_size = get_global_size(0);
  const int local_id = get_local_id(0);
  const int local_size = get_local_size(0);
  double accum = 0.0;
  for(int i = global_id; i < dimension; i += global_size)
  {
    if(!hole[i]) continue;
    solution[i] += alpha * search_dir[i];        // u <- u + alpha*p   (alpha = ||r||^2 / p.Ap)
    residual[i] -= alpha * matvec[i];            // r <- r - alpha*ap
    accum += (double)residual[i] * residual[i];  // += r_i^2  -> new ||r||^2 for the convergence test
  }
  scratch[local_id] = accum;
  barrier(CLK_LOCAL_MEM_FENCE);
  for(int offset = local_size / 2; offset > 0; offset /= 2)
  {
    if(local_id < offset) scratch[local_id] += scratch[local_id + offset];
    barrier(CLK_LOCAL_MEM_FENCE);
  }
  if(local_id == 0) partial[get_group_id(0)] = scratch[0];
}

#endif // HL_SPARSE_FP64
