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

#ifndef DT_IOP_HIGHLIGHTS_PDE_H
#define DT_IOP_HIGHLIGHTS_PDE_H

// Sparse-SPD PDE assembly/solve on the region grid (screened Poisson / diffusion), CPU + OpenCL.
// Public API of this highlights harmonic-transposition module (a compiled TU). Include
// this header to call into the module; internals are static in the .c. See common.h.

#include "common/solvers/sparse_cholesky.h"
#include "common/solvers/sparse_cholesky_cl.h"
#include "iop/highlights/common.h"
#include <stdint.h>

// Assemble the sparse matrix A = diag(d) + lam*Op over the `hole` pixels of the region grid
// (Op = the diffusion operator from _sp_row_op; pixels outside the hole are fixed boundary
// values, eliminated into the right-hand side by the caller, exactly like the conjugate
// gradient does). Outputs the upper triangle in compressed-sparse-column form, with unknowns
// permuted by geometric nested dissection. Returns 0 when the system is empty or too big for
// the direct solver. *pgrid_out (size *nh_out) maps permuted unknown -> grid index; free with
// dt_free_align.
//
// MATHS BRIDGE -- step 7 all-clip core, E_chrominance screened-Poisson (article §"The optimization
// problem", term 3 / §"Chrominance, by diffusion"): this is the LHS matrix A of the diffusion the
// all-clip core solves per channel. With order 1 the row operator Op = -Delta (minus the 9-point
// Laplacian) and diag(d) = lambda*I, so A = lambda*I - Delta discretizes the modified-Helmholtz /
// screened-Poisson operator (lambda - Delta) whose Euler-Lagrange minimizer of
// int (||grad r||^2 + lambda ||r||^2) dOmega is (Delta - lambda) r = 0, r|dOmega = r_valid. d is the
// per-pixel screening/reaction strength (react = solid_color^2 * 4, the "inpaint a flat colour"
// pull toward the mean valid chroma); lam scales Op. A is SPD, hence the sparse Cholesky.
int _sp_pde_assemble(const uint8_t *const restrict hole, const float *const restrict diffusion,
                     const float diffusion_const, const int order, const float lambda, const int region_w,
                     const int region_h, int **matrix_col_ptr_out, int **matrix_row_index_out,
                     double **matrix_values_out, int **perm_grid_out, int *n_unknowns_out,
                     const dt_dev_pixelpipe_t *const pipe);

// Assemble + factor the diffusion system over the hole pixels (see _sp_pde_assemble).
// Returns NULL when the system is too big for the direct solver, not positive definite, or
// on out-of-memory -- callers keep their previous iterative solver as fallback. The returned
// factor is reused for all three colour channels (same hole, same operator, different
// right-hand sides). *perm_out maps permuted unknown -> grid index.
//
// MATHS BRIDGE -- Cholesky factor of A = diag(d) + lambda*Op for the step-7 all-clip core chroma
// solve; order 1 -> A = lambda_solid*I - Delta (screened-Poisson, E_chrominance). One factorization
// serves the three channel right-hand sides (r_R, r_G, r_B share the same A, differing only in the
// Dirichlet rim data and the flat-colour target).
_sp_chol_t *_sp_pde_factor(const uint8_t *const restrict hole, const float *const restrict diffusion,
                           const int order, const float lambda, const int region_w, const int region_h,
                           int **perm_out, int *n_unknowns_out, const dt_dev_pixelpipe_t *pipe);

// Exact-solver counterpart of _region_pde_solve for a prebuilt Cholesky factor: same
// right-hand-side construction as the conjugate-gradient prologue (the fixed boundary values
// are pushed through the diffusion operator into the right-hand side), then the two
// triangular solves. b/t1/t2/sc are scratch buffers.
//
// MATHS BRIDGE -- solves (lambda*I - Delta) r = lambda_solid*r_target on the hole with r fixed on
// the rim (Dirichlet r|dOmega = r_valid): the screened-Poisson chroma fill of the all-clip core
// (article step 7 / §"Chrominance, by diffusion"). The Dirichlet data enters the RHS by embedding
// the boundary values, applying the operator, and subtracting -- the standard elimination of fixed
// unknowns into the right-hand side. r_target = the mean valid chromaticity (flat-colour target).
void _sp_pde_solve(const _sp_chol_t *const factor, const int *const restrict perm_grid,
                   float *const restrict field, const uint8_t *const restrict hole,
                   const float *const restrict diffusion, const float *const restrict target,
                   const float *const restrict source, const int order, const float lambda, const int region_w,
                   const int region_h, double *const restrict rhs, float *const restrict embedded,
                   float *const restrict operator_out, float *const restrict scratch);

// Matrix-free conjugate-gradient solve (iterative fallback solver: repeats operator-vector
// products until the residual error is small, never forming the matrix explicitly) of
// (diag(d) + lam*Op) u = diag(d)*target + source on the `hole` pixels, with u held fixed at
// its current value on non-hole pixels (the boundary condition). Op is the symmetric
// positive definite diffusion operator (minus the Laplacian for order 1, the biharmonic for
// order 2). d/target/source may be NULL. u is updated in place on the hole. r,p,ap,t1,t2 are
// single-channel scratch of size rw*rh. (A per-pixel `source` lets us solve the Poisson step
// Delta u = w  as  (-Delta) u = -w, the second half of the mixed biharmonic formulation.)
//
// MATHS BRIDGE -- same linear system as _sp_pde_solve, iterative instead of direct: the fallback
// when the all-clip core exceeds DT_HL_SPARSE_MAX unknowns. Order 1 = the screened-Poisson chroma
// fill (lambda*I - Delta) r = lambda_solid*r_target of step 7 (article §"Chrominance, by
// diffusion"); the CG never forms A, only its action A u = diag(d)*u + lambda*Op(u). Because the
// float CG stops at a relative tolerance it is inexact where the direct solve is exact -- the
// direct path is preferred, this is only the large-core fallback.
void _region_pde_solve(float *const restrict field, const uint8_t *const restrict hole,
                       const float *const restrict diffusion, const float *const restrict target,
                       const float *const restrict source, const int order, const float lambda, const int region_w,
                       const int region_h, float *const restrict residual, float *const restrict search_dir,
                       float *const restrict operator_dir, float *const restrict embedded,
                       float *const restrict scratch, const int maxiter);

#ifdef HAVE_OPENCL
static inline _sp_chol_cl_kernels_t _hl_sp_chol_kernels(void *gd_void)
{
  const dt_iop_highlights_global_data_t *const global_data = (const dt_iop_highlights_global_data_t *)gd_void;
  _sp_chol_cl_kernels_t kernels
      = { global_data->kernel_sparse_chol_update_level, global_data->kernel_sparse_chol_final_level,
          global_data->kernel_sparse_chol_fwd_level, global_data->kernel_sparse_chol_bwd_level };
  return kernels;
}
#endif

#if defined(HAVE_OPENCL) && DT_HL_SPARSE_SOLVE
// single-channel gaussian blur on the device (used to feather the joint-core composite weight)
cl_int _region_blur1_cl(const int devid, cl_mem in, cl_mem out, const int region_w, const int region_h,
                        const float sigma);

// Matrix-free conjugate gradient (iterative solver) on the device, mirroring
// _region_pde_solve for the joint core's screened-harmonic chroma (order 1, lam 1, constant
// reaction strength d and flat target): the fallback when the all-clip core exceeds
// DT_HL_SPARSE_MAX unknowns and the direct factorization is off the table.
//
// MATHS BRIDGE -- Step 7 E_chrominance screened-Poisson (article §"Chrominance, by diffusion"):
// solves (dscalar*I - Delta) r = dscalar*tscalar on the hole, r|dOmega = r_valid (Dirichlet), where
// dscalar = lambda_solid = solid_color^2*4 (the flat-colour reaction strength) and tscalar = the
// mean valid chromaticity r_target. A = d*I - Delta is never formed; each iteration applies its
// action A p = d*p + Op(p) via the hl_cg_* kernels (Op = -Delta, order 1) in highlights_sparse.cl.
// Dot products accumulate in double precision on the device (64-bit-float program); only the
// 2 KB of reduction partials cross the bus per iteration. The CPU conjugate gradient is
// itself OpenMP-summation-order nondeterministic, so tolerance-level (not bit-exact) parity
// is the honest target here. Any change here must be mirrored in _region_pde_solve and
// re-validated with the HL_CORECL_TEST self-test (_joint_core_stage_cl_selftest).
cl_int _region_pde_cg_cl(const int devid, void *gd_void, cl_mem solution, cl_mem hole, const int region_w,
                         const int region_h, const float dscalar, const float tscalar, const int maxiter);
#endif
#endif // DT_IOP_HIGHLIGHTS_PDE_H
