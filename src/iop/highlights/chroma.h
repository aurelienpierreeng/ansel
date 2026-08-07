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

#ifndef DT_IOP_HIGHLIGHTS_CHROMA_H
#define DT_IOP_HIGHLIGHTS_CHROMA_H

// Anisotropic (divergence-form) chrominance-coherence stage (CPU + OpenCL).
// Public API of this highlights harmonic-transposition module (a compiled TU). Include
// this header to call into the module; internals are static in the .c. See common.h.

#include "iop/highlights/common.h"
#include <math.h>
#include <stdint.h>

void _aniso_tensor(const float *const restrict luminance, float *const restrict tensor_xx,
                   float *const restrict tensor_xy, float *const restrict tensor_yy, float *const restrict scratch,
                   const int region_w, const int region_h);

// Obstacle-projected variant (stage A of the core-shelf fix): the saturation floors are
// information, not just output clamps -- a clipped channel's ratio can never fall below
// clip0_c / L. Projecting after EVERY smoothing step (u = max(u, obs)) turns the diffusion
// into a monotone obstacle-problem relaxation: the bound's influence spreads smoothly through
// the field instead of leaving an exactly-flat floor-clamped shelf at the reassembly.
//
// MATHS BRIDGE -- Step 8 explicit trace-form relaxation under the obstacle (article §"The update
// rules", the r_c <- max(r_c + 0.18(D_xx d_xx r + 2 D_xy d_xy r + D_yy d_yy r), c0/L) update, and
// §"The saturation floors, as obstacles"): one explicit Euler step of dr/dt = tr(D Hess r) (the
// trace form of the steered diffusion) with time step 0.18, followed by the projection
// r <- max(r, obstacle). Every neighbour weight is nonnegative, so the projected scheme is
// monotone and converges to the variational-inequality solution of
// min int grad(r)^T D grad(r) s.t. r >= c0/L. This is the coarse-to-fine ladder's per-level solver
// and, for the large-core (pyramid) path, the primary Step-8 estimator.
void _aniso_iterate_obs(float *const restrict field, const float *const restrict obstacle,
                        const uint8_t *const restrict hole, const float *const restrict tensor_xx,
                        const float *const restrict tensor_xy, const float *const restrict tensor_yy,
                        float *const restrict tmp, const int region_w, const int region_h, const int iters,
                        const int box_x_lo, const int box_y_lo, const int box_x_hi, const int box_y_hi,
                        const float react, const float react_target);

static inline float _aniso_edge_w(const float *const restrict tensor_xx, const float *const restrict tensor_xy,
                                  const float *const restrict tensor_yy, const size_t i, const size_t j,
                                  const int offset_x, const int offset_y)
{
  const float avg_xx = 0.5f * (tensor_xx[i] + tensor_xx[j]); // a = D_xx averaged across the edge
  const float avg_yy = 0.5f * (tensor_yy[i] + tensor_yy[j]); // c = D_yy averaged across the edge
  const float limit = fminf(avg_xx, avg_yy);
  const float cross
      = CLAMP(0.5f * (tensor_xy[i] + tensor_xy[j]), -limit, limit); // b clamped to +-min(a,c) >= 0 guarantee

  if(offset_y == 0) return fmaxf(avg_xx - fabsf(cross), 1e-4f); // axis x: a - |b|
  if(offset_x == 0) return fmaxf(avg_yy - fabsf(cross), 1e-4f); // axis y: c - |b|
  if(offset_x == offset_y) return fmaxf(cross, 0.f);            // diagonal (+,+) / (-,-): +b/2 share
  return fmaxf(-cross, 0.f);                                    // diagonal (+,-) / (-,+): -b/2 share
}

// Divergence-form direct solve: fills the three ratio planes of s1 (rn*4 layout) over the
// pixels where vld_an < 0.5 (identical hole for the three channels in the coefficient-field
// mode: the all-clip core). `planes` is rn*4 float scratch (tensor + scratch). Returns 1 on
// success, 0 to fall back to the explicit path.
//
// MATHS BRIDGE -- Step 8 PRIMARY estimator (article §"The update rules", "divergence-form exact
// solve"): the exact steady state div(D grad r) = 0 (no obstacle in the matrix; the floor is
// applied by the polish pass afterward), r|dOmega = r_valid Dirichlet. Assembles the Weickert
// nonnegativity graph Laplacian (diagonal = sum of the 8 edge weights _aniso_edge_w, off-diagonals
// = -w_ij, Dirichlet neighbours eliminated into the RHS), then ONE sparse Cholesky factorization
// serves the three channel right-hand sides. Used when the core fits DT_HL_SPARSE_MAX unknowns;
// larger cores take _aniso_iterate_obs on the coarse-to-fine pyramid instead.
int _aniso_div_solve(float *const restrict ratios, const float *const restrict valid,
                     const float *const restrict luminance, float *const restrict scratch_planes,
                     const int region_w, const int region_h, const float react,
                     const dt_aligned_pixel_t react_target, const dt_dev_pixelpipe_t *pipe);

void _aniso_chroma(_hl_region_ctx_t *const ctx);

#if defined(HAVE_OPENCL) && DT_HL_SPARSE_SOLVE && (DT_HL_ANISO_SOLVER == 2)
// Divergence-form structure-steered chroma diffusion on the device (smooth the colour ratios
// along image edges, never across them), mirroring the DT_HL_ANISO_CHROMA production block
// with _aniso_div_solve: the structure tensor is computed on the GPU from the recovered
// brightness, and the host downloads only the all-clip mask (the sparse symbolic analysis
// needs it) plus the COMPACT per-unknown 8-edge weight list -- the matrix values -- for the
// exact CPU assembly; the three right-hand sides are built on-device from the same weight
// buffer, so no full-res float plane crosses the bus. Any change here must be mirrored in
// _aniso_div_solve (CPU) and re-validated with the HL_ANISOCL_TEST self-test
// (_aniso_stage_cl_selftest).
//
// MATHS BRIDGE -- Step 8 chrominance coherence (article §"Chrominance coherence"): the
// divergence-form exact solve of div(D grad r) = 0, r|dOmega = r_valid, restricted to the all-clip
// hole (partial-clip pixels are Dirichlet anchors). Weickert nonnegativity graph Laplacian from D
// (edge weights = hl_aniso_weights), ONE Cholesky factor for the three channel RHS, then a full-res
// obstacle-projected polish (r >= c0/L) since a direct factorization cannot project mid-solve.
// Cores above DT_HL_SPARSE_MAX fall to _aniso_pyramid_cl. Reassembly RGB = L_sum * r.
cl_int _aniso_stage_cl(const int devid, void *gd_void, cl_mem estimate, cl_mem valid, cl_mem clip0,
                       const int region_w, const int region_h, const float radius, const float floor_gate,
                       const float solid_color, const dt_dev_pixelpipe_t *pipe);
#endif
#endif // DT_IOP_HIGHLIGHTS_CHROMA_H
