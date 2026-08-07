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
    along with Ansel.  If not, see <http://www.gnu.org/licenses/>.
*/

// Device kernels of the "harmonic transposition" highlight reconstruction (single precision).
// The host orchestration lives in src/iop/highlights_harmonic.h; every kernel here mirrors a
// CPU reference function there and is validated against it by the env-gated HL_*_TEST
// self-tests. The double-precision solver kernels live in highlights_sparse.cl (they need
// the fp64 extension, which this file must not require on every device).

#include "common.h"

// ===== harmonic-fill kernels (highlights harmonic transposition) ============================
// "Harmonic fill" = repeatedly replace every hole pixel by the average of its four neighbours
// (Jacobi relaxation) until the surface is smooth -- like letting heat diffuse from the
// anchors (anchor = pixel with a trustworthy value; hole = pixel that must be filled). Runs
// coarse-to-fine on shrunken copies of the grid (a pyramid) so large holes converge fast:
// block-downsample with anchor-weighted mean + majority anchor flag, per-level Jacobi sweeps
// on ping-pong buffers (two buffers alternately used as source and destination), bilinear
// upsample of the solution into the finer level's holes. Single-channel float buffers.
// Used to smooth the fitted colour-line coefficient fields across the clipped areas.
// Mirrors the CPU _cf_harmonic_fill (src/iop/highlights_harmonic.h) -- any change here must
// be mirrored there and re-validated with the HL_FILLCL_TEST self-test.

// Shrink one pyramid level by an integer factor `step`. Each coarse cell scans its step x step
// block of fine cells: writes dval = mean of the anchor values in the block (0 if none) and
// danc = 1 when anchors are the majority of the block. Reads val/anc at (w,h); writes
// dval/danc at (dw,dh). Runs once per pyramid level on the way down.
kernel void
hl_fill_down(global const float *value, global const uchar *anchor,
             global float *coarse_value, global uchar *coarse_anchor,
             const int width, const int height, const int coarse_w, const int coarse_h, const int step,
             const int mask_is_hole)
{
  const int coarse_x = get_global_id(0);
  const int coarse_y = get_global_id(1);
  if(coarse_x >= coarse_w || coarse_y >= coarse_h) return;

  float accum = 0.f;
  int n_anchors = 0;
  int n_total = 0;
  for(int y = coarse_y * step; y < min((coarse_y + 1) * step, height); y++)
    for(int x = coarse_x * step; x < min((coarse_x + 1) * step, width); x++)
    {
      const int i = y * width + x;
      n_total++;
      // the mask may arrive in either convention: 1 = trusted anchor (default) or,
      // with mask_is_hole set, 1 = hole to fill (saves the callers a whole inversion pass)
      const int is_anchor = mask_is_hole ? !anchor[i] : (int)anchor[i];
      if(is_anchor) { accum += value[i]; n_anchors++; }
    }
  coarse_value[coarse_y * coarse_w + coarse_x] = n_anchors ? accum / n_anchors : 0.f;
  coarse_anchor[coarse_y * coarse_w + coarse_x] = (2 * n_anchors > n_total);
}

// Coarsest-level seed, a single-workgroup kernel (launched with exactly one group of threads
// so barrier() synchronizes ALL of them; only valid because this level is tiny). Threads
// stride the level summing anchor values, tree-sum the partial sums in local memory
// (reduction), then write u = the anchor value f at anchors, the anchor mean everywhere
// else -- the starting guess for the Jacobi sweeps.
kernel void
hl_fill_seed(global float *solution, global const float *anchor_value, global const uchar *anchor,
             const int n_pixels, local float *local_sum, local int *local_count)
{
  const int local_id = get_local_id(0);
  const int local_size = get_local_size(0);
  float anchor_sum = 0.f;
  int n_anchors = 0;
  for(int i = local_id; i < n_pixels; i += local_size)
    if(anchor[i]) { anchor_sum += anchor_value[i]; n_anchors++; }
  local_sum[local_id] = anchor_sum;
  local_count[local_id] = n_anchors;
  barrier(CLK_LOCAL_MEM_FENCE);
  for(int offset = local_size / 2; offset > 0; offset /= 2)
  {
    if(local_id < offset) { local_sum[local_id] += local_sum[local_id + offset]; local_count[local_id] += local_count[local_id + offset]; }
    barrier(CLK_LOCAL_MEM_FENCE);
  }
  const float mean = local_count[0] ? local_sum[0] / local_count[0] : 0.f;
  barrier(CLK_LOCAL_MEM_FENCE);
  for(int i = local_id; i < n_pixels; i += local_size)
    solution[i] = anchor[i] ? anchor_value[i] : mean;
}

// Seed a finer pyramid level from the coarser solved level: anchor cells take this level's
// own block means f, hole cells take the bilinear interpolation of the coarse solution
// (the coarse grid is half size, pw x ph). Writes the level's starting guess u at (dw,dh).
kernel void
hl_fill_seed_up(global float *solution, global const float *anchor_value, global const uchar *anchor,
                global const float *coarse, const int level_w, const int level_h,
                const int coarse_w, const int coarse_h)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= level_w || y >= level_h) return;
  const int i = y * level_w + x;
  if(anchor[i]) { solution[i] = anchor_value[i]; return; }
  const float src_x = ((float)x + 0.5f) * 0.5f - 0.5f;
  const float src_y = ((float)y + 0.5f) * 0.5f - 0.5f;
  const int x_lo = clamp((int)floor(src_x), 0, coarse_w - 1);
  const int y_lo = clamp((int)floor(src_y), 0, coarse_h - 1);
  const int x_hi = min(x_lo + 1, coarse_w - 1);
  const int y_hi = min(y_lo + 1, coarse_h - 1);
  const float frac_x = clamp(src_x - x_lo, 0.f, 1.f);
  const float frac_y = clamp(src_y - y_lo, 0.f, 1.f);
  const float top_row = coarse[y_lo * coarse_w + x_lo] * (1.f - frac_x) + coarse[y_lo * coarse_w + x_hi] * frac_x;
  const float bottom_row = coarse[y_hi * coarse_w + x_lo] * (1.f - frac_x) + coarse[y_hi * coarse_w + x_hi] * frac_x;
  solution[i] = top_row * (1.f - frac_y) + bottom_row * frac_y;
}

// One Jacobi relaxation sweep, ping-pong (reads u, writes v; the host swaps the two buffers
// between launches): anchor cells copy their value through unchanged, hole cells become the
// plain average of their 4 neighbours (edge-clamped at the level border).
// MATHS BRIDGE -- isotropic (D = I) harmonic fill: dst = 1/4 (N+S+W+E), the discrete div(grad p)=0
// update; anchors are Dirichlet boundary data (p|anchors = p_fit), copied through unchanged.
kernel void
hl_fill_jacobi(global const float *source, global float *dest, global const uchar *anchor,
               const int level_w, const int level_h)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= level_w || y >= level_h) return;
  const int i = y * level_w + x;
  if(anchor[i]) { dest[i] = source[i]; return; }   // anchor = Dirichlet datum, pinned
  const int idx_north = max(y - 1, 0) * level_w + x;
  const int idx_south = min(y + 1, level_h - 1) * level_w + x;
  const int idx_west = y * level_w + max(x - 1, 0);
  const int idx_east = y * level_w + min(x + 1, level_w - 1);
  dest[i] = 0.25f * (source[idx_north] + source[idx_south] + source[idx_west] + source[idx_east]);
}

// Single-workgroup Jacobi: for SMALL level grids, run ALL the iterations inside one kernel
// launch. Launched with exactly one group of threads, so barrier() synchronizes all of them
// between sweeps; each sweep reads only the previous buffer (u) and writes the other (v),
// so the order threads run in does not matter -- the result is bit-identical to `iters`
// separate hl_fill_jacobi launches, just without the per-launch overhead.
kernel void
hl_fill_jacobi_block(global float *buffer_a, global float *buffer_b, global const uchar *anchor,
                     const int level_w, const int level_h, const int iters)
{
  const int local_id = get_local_id(0);
  const int local_size = get_local_size(0);
  const int level_pixels = level_w * level_h;
  global float *source = buffer_a;
  global float *dest = buffer_b;
  for(int iteration = 0; iteration < iters; iteration++)
  {
    for(int i = local_id; i < level_pixels; i += local_size)
    {
      if(anchor[i]) { dest[i] = source[i]; continue; }
      const int y = i / level_w;
      const int x = i - y * level_w;
      const int idx_north = max(y - 1, 0) * level_w + x;
      const int idx_south = min(y + 1, level_h - 1) * level_w + x;
      const int idx_west = y * level_w + max(x - 1, 0);
      const int idx_east = y * level_w + min(x + 1, level_w - 1);
      dest[i] = 0.25f * (source[idx_north] + source[idx_south] + source[idx_west] + source[idx_east]);
    }
    barrier(CLK_GLOBAL_MEM_FENCE);
    global float *tmp = source; source = dest; dest = tmp;
  }
}

// Final upsample: write the solved base-grid values u (bw x bh, downsample factor ds) into
// the full-resolution hole pixels of val by bilinear interpolation. Anchor pixels return
// early and keep their exact original values.
kernel void
hl_fill_up(global float *value, global const uchar *anchor, global const float *solution,
           const int width, const int height, const int base_w, const int base_h, const int downsample, const int mask_is_hole)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  const int is_anchor = mask_is_hole ? !anchor[i] : (int)anchor[i];
  if(is_anchor) return;   // anchors keep their exact values; only holes receive the upsample   // anchors keep their exact values; only non-anchor cells are filled
  const float src_x = ((float)x + 0.5f) / downsample - 0.5f;
  const float src_y = ((float)y + 0.5f) / downsample - 0.5f;
  const int x_lo = clamp((int)floor(src_x), 0, base_w - 1);
  const int y_lo = clamp((int)floor(src_y), 0, base_h - 1);
  const int x_hi = min(x_lo + 1, base_w - 1);
  const int y_hi = min(y_lo + 1, base_h - 1);
  const float frac_x = clamp(src_x - x_lo, 0.f, 1.f);
  const float frac_y = clamp(src_y - y_lo, 0.f, 1.f);
  const float top_row = solution[y_lo * base_w + x_lo] * (1.f - frac_x) + solution[y_lo * base_w + x_hi] * frac_x;
  const float bottom_row = solution[y_hi * base_w + x_lo] * (1.f - frac_x) + solution[y_hi * base_w + x_hi] * frac_x;
  value[i] = top_row * (1.f - frac_y) + bottom_row * frac_y;
}

// ==== anisotropic coefficient transport (mirrors the CPU _cf_adaptive_tensor path) ==========

// Steering plane for the coefficient fills: the measured guide structure. Mean of the VALID
// channels where at least one survives; the flat mean of all three elsewhere (all-clip core),
// where a flat plane degenerates the tensor to identity, i.e. the isotropic fill.
kernel void
hl_cfa_steer(global const float *estimate, global const float *valid, global float *steer, const int n_pixels)
{
  const int i = get_global_id(0);
  if(i >= n_pixels) return;
  float accum = 0.f;
  float sum_all = 0.f;
  int n_valid = 0;
  for(int c = 0; c < 3; c++)
  {
    const float channel_value = estimate[i * 4 + c];
    sum_all += channel_value;
    if(valid[i * 4 + c] >= 0.5f) { accum += channel_value; n_valid++; }
  }
  steer[i] = n_valid ? accum / n_valid : sum_all / 3.f;
}

// Plain block mean (no mask): downsample the steering plane to the base grid, then per level.
kernel void
hl_cfa_down(global const float *source, global float *dest,
            const int width, const int height, const int coarse_w, const int coarse_h, const int step)
{
  const int coarse_x = get_global_id(0);
  const int coarse_y = get_global_id(1);
  if(coarse_x >= coarse_w || coarse_y >= coarse_h) return;
  float accum = 0.f;
  int n_total = 0;
  for(int y = coarse_y * step; y < min((coarse_y + 1) * step, height); y++)
    for(int x = coarse_x * step; x < min((coarse_x + 1) * step, width); x++)
    {
      accum += source[y * width + x];
      n_total++;
    }
  dest[coarse_y * coarse_w + coarse_x] = accum / n_total;
}

// One 3x3 box pass on TWO planes at once (edge-clamped). With square set (pass 0), the second
// output accumulates the square of the first input, so two calls produce the double-box blur
// of L and of L^2 needed by the variance-adaptive tensor.
kernel void
hl_cfa_box(global const float *in_lum, global const float *in_sq,
           global float *out_lum, global float *out_sq,
           const int width, const int height, const int square)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  float accum_lum = 0.f;
  float accum_sq = 0.f;
  int count = 0;
  for(int offset_y = -1; offset_y <= 1; offset_y++)
    for(int offset_x = -1; offset_x <= 1; offset_x++)
    {
      const int sample_y = clamp(y + offset_y, 0, height - 1);
      const int sample_x = clamp(x + offset_x, 0, width - 1);
      const float lum_value = in_lum[sample_y * width + sample_x];
      accum_lum += lum_value;
      accum_sq += square ? lum_value * lum_value : in_sq[sample_y * width + sample_x];
      count++;
    }
  out_lum[y * width + x] = accum_lum / count;
  out_sq[y * width + x] = accum_sq / count;
}

// Central-difference gradients of the blurred steering plane + per-group partial sums of the
// gradient magnitude (the anisotropy normalisation is its mean). 1D strided launch.
kernel void
hl_cfa_grad(global const float *blurred_lum, global float *grad_x, global float *grad_y,
            global float *partials, const int width, const int height, local float *local_sum)
{
  const int local_id = get_local_id(0);
  const int local_size = get_local_size(0);
  const int group_id = get_group_id(0);
  const int stride = get_global_size(0);
  const int n_pixels = width * height;
  float grad_sum = 0.f;
  for(int i = get_global_id(0); i < n_pixels; i += stride)
  {
    const int y = i / width;
    const int x = i - y * width;
    const int x_west = max(x - 1, 0);
    const int x_east = min(x + 1, width - 1);
    const int y_north = max(y - 1, 0);
    const int y_south = min(y + 1, height - 1);
    const float grad_dx = 0.5f * (blurred_lum[y * width + x_east] - blurred_lum[y * width + x_west]);
    const float grad_dy = 0.5f * (blurred_lum[y_south * width + x] - blurred_lum[y_north * width + x]);
    grad_x[i] = grad_dx;
    grad_y[i] = grad_dy;
    grad_sum += hypot(grad_dx, grad_dy);
  }
  local_sum[local_id] = grad_sum;
  barrier(CLK_LOCAL_MEM_FENCE);
  for(int offset = local_size / 2; offset > 0; offset /= 2)
  {
    if(local_id < offset) local_sum[local_id] += local_sum[local_id + offset];
    barrier(CLK_LOCAL_MEM_FENCE);
  }
  if(local_id == 0) partials[group_id] = local_sum[0];
}

// Variance-adaptive steering tensor from the blurred plane's gradients (mirrors the CPU
// _cf_adaptive_tensor): m = trend-corrected windowed variance / (itself + (k*mean)^2) leans
// the tensor from gradient transport (clean halo ramp, m -> 0: the model travels radially
// inward from the rim) toward isophote transport (hard edge crossing the zone, m -> 1: do not
// mix colour-lines across an object boundary).
// MATHS BRIDGE -- article "The algorithm" step 3, the E_transport steering tensor D:
//   D = [ m + (1-m) c2 ] t t^T + [ m c2 + (1-m) ] g g^T ,  c2 = exp(-|grad L| / (4 <|grad L|>))
//   m = v / (v + (k Lbar)^2) ,  v = max( var_w(L) - (4/3)|grad L|^2 , 0 ) ,  k = 0.15
// g = unit gradient (uphill), t = unit isophote (level line = g rotated 90 deg), m in [0,1].
// Finish the mean-gradient-magnitude reduction on device: one work-item sums the per-group
// partials (same order as the former host loop) and writes the final gnorm scalar, so the
// host never has to block on a mid-fill readback (one full queue drain per level per fill).
kernel void
hl_cfa_gnorm(global const float *partials, global float *gnorm, const int ngroups, const int n_pixels)
{
  if(get_global_id(0) > 0) return;
  float group_sum = 0.f;
  for(int group = 0; group < ngroups; group++)
    group_sum += partials[group];
  gnorm[0] = fmax(group_sum / (float)n_pixels, 1e-9f);
}

kernel void
hl_cfa_tensor(global const float *grad_x, global const float *grad_y,
              global const float *blurred_lum, global const float *blurred_sq,
              global float *tensor_xx, global float *tensor_xy, global float *tensor_yy,
              global const float *gnorm_buffer, const float k, const int n_pixels)
{
  const int i = get_global_id(0);
  if(i >= n_pixels) return;
  const float gnorm = gnorm_buffer[0];
  const float grad_dx = grad_x[i];
  const float grad_dy = grad_y[i];
  const float magnitude = hypot(grad_dx, grad_dy);
  const float nonzero = (magnitude > 1e-12f) ? 1.f : 0.f;
  const float inv_mag = nonzero / (magnitude + (1.f - nonzero));
  const float grad_ux = grad_dx * inv_mag + (1.f - nonzero);   // g = unit gradient direction (uphill)
  const float grad_uy = grad_dy * inv_mag;
  const float iso_x = -grad_uy;
  const float iso_y = grad_ux;             // t = unit isophote direction (g rotated 90 deg)
  const float cross_damp = exp(-magnitude / (4.f * gnorm));   // c2 = exp(-|grad L| / (4 <|grad L|>))

  const float mean = blurred_lum[i];                          // Lbar (windowed mean of the steering plane)
  const float variance = fmax(blurred_sq[i] - mean * mean, 0.f);   // var_w(L)
  const float var_residual = fmax(variance - (4.f / 3.f) * (grad_dx * grad_dx + grad_dy * grad_dy), 0.f);   // v = var_w - (4/3)|grad L|^2
  const float k_scaled = k * fmax(mean, 1e-9f);              // k * Lbar
  const float edge_measure = var_residual / (var_residual + k_scaled * k_scaled + 1e-18f);   // m = v / (v + (k Lbar)^2)
  const float diffusivity_tangent = edge_measure + (1.f - edge_measure) * cross_damp;       // coeff of t t^T = m + (1-m) c2
  const float diffusivity_gradient = edge_measure * cross_damp + (1.f - edge_measure);       // coeff of g g^T = m c2 + (1-m)

  // D = diffusivity_tangent * t t^T + diffusivity_gradient * g g^T (symmetric xx/xy/yy entries)
  tensor_xx[i] = diffusivity_tangent * iso_x * iso_x + diffusivity_gradient * grad_ux * grad_ux;
  tensor_xy[i] = diffusivity_tangent * iso_x * iso_y + diffusivity_gradient * grad_ux * grad_uy;
  tensor_yy[i] = diffusivity_tangent * iso_y * iso_y + diffusivity_gradient * grad_uy * grad_uy;
}

// Weickert nonnegativity edge weight between two cells (same clamping as the CPU
// _aniso_edge_w): all weights >= 0, so the aniso Jacobi update below stays a convex
// combination of neighbours -- the maximum principle survives the steering.
static float
cfa_edge_w(global const float *tensor_xx, global const float *tensor_xy, global const float *tensor_yy,
           const int i, const int j, const int dir_x, const int dir_y)
{
  const float weight_a = 0.5f * (tensor_xx[i] + tensor_xx[j]);
  const float weight_c = 0.5f * (tensor_yy[i] + tensor_yy[j]);
  const float min_ac = fmin(weight_a, weight_c);
  const float weight_b = clamp(0.5f * (tensor_xy[i] + tensor_xy[j]), -min_ac, min_ac);
  if(dir_y == 0) return fmax(weight_a - fabs(weight_b), 1e-4f);
  if(dir_x == 0) return fmax(weight_c - fabs(weight_b), 1e-4f);
  if(dir_x == dir_y) return fmax(weight_b, 0.f);
  return fmax(-weight_b, 0.f);
}

__constant int cfa_neighbor_dy[8] = { 0, 0, -1, 1, -1, 1, 1, -1 };
__constant int cfa_neighbor_dx[8] = { -1, 1, 0, 0, -1, 1, -1, 1 };

// The Weickert edge weights are constant across every sweep of a level (the tensor is fixed):
// precompute the 8 weights per cell (interleaved) plus their sum once, so the Jacobi kernels
// below are pure multiply-accumulates. Same values, same accumulation order as the former
// inline computation. Mirrors the CPU precompute in _cf_harmonic_fill.
kernel void
hl_cfa_weights(global const float *tensor_xx, global const float *tensor_xy, global const float *tensor_yy,
               global float *edge_weights, global float *weight_sum, const int level_w, const int level_h)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= level_w || y >= level_h) return;
  const int i = y * level_w + x;
  float sum_weights = 0.f;
  for(int direction = 0; direction < 8; direction++)
  {
    const int neighbor_y = clamp(y + cfa_neighbor_dy[direction], 0, level_h - 1);
    const int neighbor_x = clamp(x + cfa_neighbor_dx[direction], 0, level_w - 1);
    const int j = neighbor_y * level_w + neighbor_x;
    const float weight_value = cfa_edge_w(tensor_xx, tensor_xy, tensor_yy, i, j, cfa_neighbor_dx[direction], cfa_neighbor_dy[direction]);
    edge_weights[i * 8 + direction] = weight_value;
    sum_weights += weight_value;
  }
  weight_sum[i] = sum_weights;
}

// One tensor-weighted Jacobi sweep (8 neighbours) over up to 3 planes sharing the anchor
// mask, ping-pong like hl_fill_jacobi. The planes share the mask, the tensor and hence the
// weights, so one launch advances all of them with the weights computed once per cell.
// MATHS BRIDGE -- steered (D != I) fill: dst = Sum_k w_ik src(nb_k) / Sum_k w_ik, the discrete
// div(D grad p)=0 update over the 8-neighbour Weickert nonnegativity stencil (w_ik = cfa_edge_w >= 0,
// so the update is a convex combination of neighbours -> maximum principle holds).
// This large-grid variant recomputes the edge weights from the three tensor planes on
// purpose: the planes enjoy ~9x neighbour reuse in cache, whereas a precomputed-weights
// buffer carries 3x the unique bytes and made this bandwidth-bound kernel 50% slower
// (measured). The single-workgroup block variant below IS weight-precomputed: its grids
// are L2-resident and it re-reads the weights across all its internal iterations.
// Unused plane slots (p >= np) must be passed a valid dummy buffer (they are never written).
kernel void
hl_cfa_jacobi(global const float *source0, global const float *source1, global const float *source2,
              global float *dest0, global float *dest1, global float *dest2,
              global const uchar *anchor,
              global const float *tensor_xx, global const float *tensor_xy, global const float *tensor_yy,
              const int level_w, const int level_h, const int n_planes)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= level_w || y >= level_h) return;
  const int i = y * level_w + x;
  if(anchor[i])
  {
    dest0[i] = source0[i];
    if(n_planes > 1) dest1[i] = source1[i];
    if(n_planes > 2) dest2[i] = source2[i];
    return;
  }
  float sum_weights = 0.f;
  float sum0 = 0.f;
  float sum1 = 0.f;
  float sum2 = 0.f;
  for(int direction = 0; direction < 8; direction++)
  {
    const int neighbor_y = clamp(y + cfa_neighbor_dy[direction], 0, level_h - 1);
    const int neighbor_x = clamp(x + cfa_neighbor_dx[direction], 0, level_w - 1);
    const int j = neighbor_y * level_w + neighbor_x;
    const float weight_value = cfa_edge_w(tensor_xx, tensor_xy, tensor_yy, i, j, cfa_neighbor_dx[direction], cfa_neighbor_dy[direction]); // w_ik >= 0
    sum_weights += weight_value;                         // Sum_k w_ik
    sum0 += weight_value * source0[j];                   // Sum_k w_ik * p(nb_k)
    if(n_planes > 1) sum1 += weight_value * source1[j];
    if(n_planes > 2) sum2 += weight_value * source2[j];
  }
  const int has_weight = (sum_weights > 1e-9f);
  dest0[i] = has_weight ? sum0 / sum_weights : source0[i];   // p(i) <- Sum_k w_ik p(nb_k) / Sum_k w_ik
  if(n_planes > 1) dest1[i] = has_weight ? sum1 / sum_weights : source1[i];
  if(n_planes > 2) dest2[i] = has_weight ? sum2 / sum_weights : source2[i];
}

// Single-workgroup variant: all iterations of the tensor-weighted sweep in one launch
// (small level grids), bit-identical to `iters` separate hl_cfa_jacobi launches.
kernel void
hl_cfa_jacobi_block(global float *u_plane0, global float *u_plane1, global float *u_plane2,
                    global float *v_plane0, global float *v_plane1, global float *v_plane2,
                    global const uchar *anchor,
                    global const float *edge_weights, global const float *weight_sum,
                    const int level_w, const int level_h, const int iters, const int n_planes)
{
  const int local_id = get_local_id(0);
  const int local_size = get_local_size(0);
  const int level_pixels = level_w * level_h;
  global float *src0 = u_plane0;
  global float *src1 = u_plane1;
  global float *src2 = u_plane2;
  global float *dst0 = v_plane0;
  global float *dst1 = v_plane1;
  global float *dst2 = v_plane2;
  for(int iteration = 0; iteration < iters; iteration++)
  {
    for(int i = local_id; i < level_pixels; i += local_size)
    {
      if(anchor[i])
      {
        dst0[i] = src0[i];
        if(n_planes > 1) dst1[i] = src1[i];
        if(n_planes > 2) dst2[i] = src2[i];
        continue;
      }
      const int y = i / level_w;
      const int x = i - y * level_w;
      float sum0 = 0.f;
      float sum1 = 0.f;
      float sum2 = 0.f;
      for(int direction = 0; direction < 8; direction++)
      {
        const int neighbor_y = clamp(y + cfa_neighbor_dy[direction], 0, level_h - 1);
        const int neighbor_x = clamp(x + cfa_neighbor_dx[direction], 0, level_w - 1);
        const int j = neighbor_y * level_w + neighbor_x;
        const float weight_value = edge_weights[i * 8 + direction];
        sum0 += weight_value * src0[j];
        if(n_planes > 1) sum1 += weight_value * src1[j];
        if(n_planes > 2) sum2 += weight_value * src2[j];
      }
      const float cell_weight_sum = weight_sum[i];
      const int has_weight = (cell_weight_sum > 1e-9f);
      dst0[i] = has_weight ? sum0 / cell_weight_sum : src0[i];
      if(n_planes > 1) dst1[i] = has_weight ? sum1 / cell_weight_sum : src1[i];
      if(n_planes > 2) dst2[i] = has_weight ? sum2 / cell_weight_sum : src2[i];
    }
    barrier(CLK_GLOBAL_MEM_FENCE);
    global float *tmp;
    tmp = src0; src0 = dst0; dst0 = tmp;
    tmp = src1; src1 = dst1; dst1 = tmp;
    tmp = src2; src2 = dst2; dst2 = tmp;
  }
}



// ===== coefficient-field joint-stage kernels (harmonic transposition) =======================
// "Colour line" idea: within a small window the three channels rise and fall together, so a
// clipped channel (the sensor pixel hit its maximum -- the value is a floor, not a
// measurement) can be predicted as a weighted sum of the two other channels (the "guides")
// plus an offset. The weights are fitted per window from "windowed moments": local averages
// of the channels and of channel pair products, obtained by gaussian-blurring per-pixel
// product images. Buffers used throughout: est = the working red/green/blue/norm pixel
// estimates (4 floats per pixel, progressively overwritten by the reconstruction); vld = the
// validity mask (per pixel and channel: 1.0 = real measured data, below 0.5 = clipped, must
// be reconstructed); bsc = fit quality (0..1 per pixel and channel: how well that channel
// was predicted from the other two; later stages blend more conservatively where it is low).
// The moments are packed into float4 images for the recursive-gaussian blur; the fit writes
// single-channel coefficient buffers that the harmonic-fill kernels above then smooth across
// the clipped area before evaluation. Mirrors the coefficient-field stage of the CPU
// _region_guided_filter, driven by _cf_stage_cl -- any change here must be mirrored there
// and re-validated with the HL_CFCL_TEST self-test.

// Reduction (per-group partial sums, small array finished on the CPU): sum and count of the
// luminance proxy (red + green + blue of est) over pixels where ANY channel is clipped
// (vld < 0.5). The host turns this into lref, the mean brightness of the clipped area, which
// scales the soft occlusion weight used when packing the moments below.
kernel void
hl_cf_lref_partials(global const float *estimate, global const float *valid,
                    const int n_pixels, global float *partials, local float *local_sum, local float *local_count)
{
  const int local_id = get_local_id(0);
  const int local_size = get_local_size(0);
  const int group_id = get_group_id(0);
  float lum_sum = 0.f;
  float count = 0.f;
  for(int i = group_id * local_size + local_id; i < n_pixels; i += get_global_size(0))
  {
    if(valid[i * 4 + 0] < 0.5f || valid[i * 4 + 1] < 0.5f || valid[i * 4 + 2] < 0.5f)
    {
      lum_sum += estimate[i * 4 + 0] + estimate[i * 4 + 1] + estimate[i * 4 + 2];
      count += 1.f;
    }
  }
  local_sum[local_id] = lum_sum;
  local_count[local_id] = count;
  barrier(CLK_LOCAL_MEM_FENCE);
  for(int offset = local_size / 2; offset > 0; offset /= 2)
  {
    if(local_id < offset) { local_sum[local_id] += local_sum[local_id + offset]; local_count[local_id] += local_count[local_id + offset]; }
    barrier(CLK_LOCAL_MEM_FENCE);
  }
  if(local_id == 0)
  {
    partials[group_id * 2 + 0] = local_sum[0];
    partials[group_id * 2 + 1] = local_count[0];
  }
}

// Pack the joint windowed-moment product images (gaussian-blurred afterwards by the host to
// become local averages). Per-pixel weight = 1 only where all three channels hold real data
// (vld >= 0.5), times a soft luminance weight that fades out pixels much darker than the
// clipped area. Three launches fill three float4 images (wR = weight x red, wRG = weight x
// red x green, etc.; "unweighted" = the plain all-valid indicator used by the mass checks in
// the fit):
// mode 0: [w, wR, wG, wB]   mode 1: [wRR, wGG, wBB, wRG]   mode 2: [wRB, wGB, unweighted, 0]
// MATHS BRIDGE -- article step 3: these three float4 images, once Gaussian-blurred, are the TEN windowed
// moment planes (1 mass n + 3 means + 6 second moments) that the 2x2 normal equations are solved from;
// the blur realizes the windowed weighted sum Sum_y w(y) G_sigma(x-y)(.) at every x.
kernel void
hl_cf_pack_joint(global const float *estimate, global const float *valid, global const float *luminance_plane,
                 write_only image2d_t output, const int width, const int height,
                 const float cf_binv, const int mode,
                 const float shift_r, const float shift_g, const float shift_b)
{
  // moments are CENTERED on the per-region valid means (shR/G/B): E[u^2]-E[u]^2 in float32
  // on a smooth plane cancels catastrophically and the fit division amplifies the noise
  // device-dependently; centering makes the blurred moments carry the (co)variances directly
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  const float val_r = estimate[i * 4 + 0] - shift_r;
  const float val_g = estimate[i * 4 + 1] - shift_g;
  const float val_b = estimate[i * 4 + 2] - shift_b;
  const int all_valid = (valid[i * 4 + 0] >= 0.5f && valid[i * 4 + 1] >= 0.5f && valid[i * 4 + 2] >= 0.5f);
  // the occlusion weight reads the PRE-LADDER luminance plane (lsb = luminance proxy, the
  // sum of the three channel estimates, captured once BEFORE any reconstruction step): the
  // CPU freezes it so that windows containing already-reconstructed pixels keep their
  // original weights instead of shifting as est gets rewritten
  const float luminance = luminance_plane[i];
  const float bright_weight = (cf_binv > 0.f) ? min(luminance * cf_binv, 1.f) * min(luminance * cf_binv, 1.f) : 1.f;
  const float weight = all_valid ? bright_weight : 0.f;
  float4 out_pixel;
  if(mode == 0)      out_pixel = (float4)(weight, weight * val_r, weight * val_g, weight * val_b);
  else if(mode == 1) out_pixel = (float4)(weight * val_r * val_r, weight * val_g * val_g, weight * val_b * val_b, weight * val_r * val_g);
  else               out_pixel = (float4)(weight * val_r * val_b, weight * val_g * val_b, all_valid ? 1.f : 0.f, 0.f);
  write_imagef(output, (int2)(x, y), out_pixel);
}

// Joint colour-line fit, one work-item per pixel: from the blurred moment images m1/m2/m3,
// solve the small 2x2 linear system that best predicts channel c from the two guide channels
// g1 and g2 as c ~= a*g1 + b*g2 + d over the local window. Writes the raw per-pixel
// coefficient planes ca/cb/cd, the fit quality cr2 (0..1, fraction of the target's local
// variation the fit explains), and two byte masks: anchor = the fit is trustworthy here
// (enough valid window mass, channel c itself valid, decent fit quality, bounded weights);
// broad = same gates without the fit-quality/bounds conditions. lam is a tiny stabiliser
// added to the diagonal so the division never blows up on flat windows.
kernel void
hl_cf_fit_joint(read_only image2d_t moments1_img, read_only image2d_t moments2_img, read_only image2d_t moments3_img,
                global const float *valid, const int width, const int height,
                const int c, const int guide1, const int guide2, const float min_mass,
                global float *coeff_a, global float *coeff_b, global float *coeff_d, global float *fit_quality,
                global uchar *anchor, global uchar *broad,
                const float shift_r, const float shift_g, const float shift_b)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  const float4 moments1 = read_imagef(moments1_img, sampleri, (int2)(x, y));
  const float4 moments2 = read_imagef(moments2_img, sampleri, (int2)(x, y));
  const float4 moments3 = read_imagef(moments3_img, sampleri, (int2)(x, y));

  const float weight_sum = fmax(moments1.x, 1e-9f);
  const float inv_weight = 1.f / weight_sum;
  // means indexed by channel: M1 = [n, wR, wG, wB]
  const float means[3] = { moments1.y * inv_weight, moments1.z * inv_weight, moments1.w * inv_weight };
  // second moments: diag in M2.xyz, RG = M2.w, RB = M3.x, GB = M3.y
  const float second_moments[3] = { moments2.x * inv_weight, moments2.y * inv_weight, moments2.z * inv_weight };
  const float mean_rg = moments2.w * inv_weight;
  const float mean_rb = moments3.x * inv_weight;
  const float mean_gb = moments3.y * inv_weight;

  // MATHS BRIDGE -- article step 3: solve the 2x2 normal equations Sigma [a;b] = [Cov(u1,v); Cov(u2,v)],
  // Sigma = [[Var u1, Cov(u1,u2)],[Cov(u1,u2), Var u2]] + ridge, by Cramer's rule. u1=guide1, u2=guide2, v=c.
  const float mean_guide1 = means[guide1];  // E[u1]
  const float mean_guide2 = means[guide2];  // E[u2]
  const float mean_target = means[c];       // E[v]
  const float var_11 = fmax(second_moments[guide1] - mean_guide1 * mean_guide1, 0.f);   // Var(u1) = E[u1^2]-E[u1]^2
  const float var_22 = fmax(second_moments[guide2] - mean_guide2 * mean_guide2, 0.f);   // Var(u2)
  // off-diagonal (a,b): pick from RG/RB/GB by index sum
  #define OFF(ch_a, ch_b) (((ch_a) + (ch_b)) == 1 ? mean_rg : (((ch_a) + (ch_b)) == 2 ? mean_rb : mean_gb))
  const float var_12 = OFF(guide1, guide2) - mean_guide1 * mean_guide2;   // Cov(u1,u2)
  const float cov_1 = OFF(c, guide1) - mean_target * mean_guide1;         // Cov(v,u1) = RHS_1
  const float cov_2 = OFF(c, guide2) - mean_target * mean_guide2;         // Cov(v,u2) = RHS_2
  const float var_target = fmax(second_moments[c] - mean_target * mean_target, 0.f);   // Var(v), denom of R^2
  #undef OFF

  const float lambda = 1e-3f * 0.5f * (var_11 + var_22) + 1e-12f;   // relative Tikhonov ridge = 1e-3 * (Var u1 + Var u2)/2
  const float determinant = fmax((var_11 + lambda) * (var_22 + lambda) - var_12 * var_12, 1e-18f);   // det Sigma (with ridge)
  const float slope_a = ((var_22 + lambda) * cov_1 - var_12 * cov_2) / determinant;   // a = (Sigma^-1 RHS)_1 (Cramer)
  const float slope_b = ((var_11 + lambda) * cov_2 - var_12 * cov_1) / determinant;   // b = (Sigma^-1 RHS)_2 (Cramer)
  const float r_sq = clamp((slope_a * cov_1 + slope_b * cov_2) / (var_target + 1e-12f), 0.f, 1.f);   // R^2 = (a Cov(v,u1)+b Cov(v,u2))/Var(v)

  coeff_a[i] = slope_a;
  coeff_b[i] = slope_b;
  // intercept of the CENTERED fit, unshifted back to absolute values: d = E[v] - a E[u1] - b E[u2]
  const float shifts[3] = { shift_r, shift_g, shift_b };
  coeff_d[i] = (mean_target + shifts[c]) - slope_a * (mean_guide1 + shifts[guide1]) - slope_b * (mean_guide2 + shifts[guide2]);
  fit_quality[i] = r_sq;

  const int mass_ok = (moments3.z > min_mass && moments1.x > 0.25f * moments3.z);
  const int valid_ok = (valid[i * 4 + c] >= 0.5f);
  // anchor gate (article: R^2 > 0.25 with bounded slopes) = the Dirichlet data for the E_transport fill;
  // |a|,|b| < 64 rejects only degenerate windows whose exploding slopes would poison the fill boundary
  anchor[i] = (mass_ok && valid_ok && r_sq > 0.25f && fabs(slope_a) < 64.f && fabs(slope_b) < 64.f);
  broad[i] = (mass_ok && valid_ok);   // broader mass-only anchor set for the diffused R^2 plane
}

// Apply the joint coefficients (now smoothed across the hole by the harmonic fill): at every
// pixel where channel c is clipped (vld < 0.5) but BOTH guides g1 and g2 still hold real
// data, overwrite est with a*g1 + b*g2 + d and record the fit quality in bsc. All other
// pixels are left untouched.
kernel void
hl_cf_eval_joint(global const float *coeff_a, global const float *coeff_b, global const float *coeff_d,
                 global const float *fit_quality, global const float *valid,
                 global float *estimate, global float *model_quality,
                 const int width, const int height, const int c, const int guide1, const int guide2)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  if(valid[i * 4 + c] < 0.5f && valid[i * 4 + guide1] >= 0.5f && valid[i * 4 + guide2] >= 0.5f)
  {
    // evaluation v_hat = a*u1 + b*u2 + d against the MEASURED guides (diffused a,b,d; true guide pixels)
    estimate[i * 4 + c] = coeff_a[i] * estimate[i * 4 + guide1] + coeff_b[i] * estimate[i * 4 + guide2] + coeff_d[i];
    model_quality[i * 4 + c] = clamp(fit_quality[i], 0.f, 1.f);
  }
}


// ===== coefficient-field pair stage + deep-cascade evaluation ===============================
// Same colour-line idea as above but with a SINGLE guide: where two channels clipped at once,
// the one surviving channel predicts each clipped one as slope x guide + intercept. The
// "deep" channel is the most-clipped channel of the region; where its own guides are also
// clipped its final value is blended between the chained joint model and the one-hop pair
// fit (see hl_cf_eval_deep). Same _cf_stage_cl driver and HL_CFCL_TEST validation as above.

// Pack the pair windowed-moment product images for channels a and b (gaussian-blurred by the
// host afterwards). Weight = 1 only where BOTH pair channels hold real data (vld >= 0.5),
// times the same soft luminance weight as the joint pack. Two launches fill two images:
// mode 0: [w, w*va, w*vb, w*va*va]   mode 1: [w*vb*vb, w*va*vb, unweighted, 0]
kernel void
hl_cf_pack_pair(global const float *estimate, global const float *valid, global const float *luminance_plane,
                write_only image2d_t output, const int width, const int height,
                const float cf_binv, const int chan_a, const int chan_b, const int mode,
                const float shift_a, const float shift_b)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  const float val_a = estimate[i * 4 + chan_a] - shift_a;
  const float val_b = estimate[i * 4 + chan_b] - shift_b;   // centered moments
  const float luminance = luminance_plane[i];   // pre-ladder luminance (see hl_cf_pack_joint)
  const float bright_weight = (cf_binv > 0.f) ? min(luminance * cf_binv, 1.f) * min(luminance * cf_binv, 1.f) : 1.f;
  const int pair_valid = (valid[i * 4 + chan_a] >= 0.5f && valid[i * 4 + chan_b] >= 0.5f);
  const float weight = pair_valid ? bright_weight : 0.f;
  float4 out_pixel;
  if(mode == 0) out_pixel = (float4)(weight, weight * val_a, weight * val_b, weight * val_a * val_a);
  else          out_pixel = (float4)(weight * val_b * val_b, weight * val_a * val_b, pair_valid ? 1.f : 0.f, 0.f);
  write_imagef(output, (int2)(x, y), out_pixel);
}

// Single-guide colour-line fit for orientation o (o=0: predict channel a from guide b;
// o=1: predict b from a). slope = local covariance of the pair divided by the guide's local
// variance, intercept from the local means, fit quality r2 = squared correlation (0..1).
// Writes the slope plane cs, intercept plane ci, fit quality cr2 and the anchor/broad byte
// masks (same gating idea as hl_cf_fit_joint).
kernel void
hl_cf_fit_pair(read_only image2d_t moments_a_img, read_only image2d_t moments_b_img,
               global const float *valid, const int width, const int height,
               const int target_channel, const int orientation, const float min_mass,
               global float *coeff_slope, global float *coeff_intercept, global float *fit_quality,
               global uchar *anchor, global uchar *broad,
               const float shift_target, const float shift_guide)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  const float4 moments_a = read_imagef(moments_a_img, sampleri, (int2)(x, y));   // [n, wa, wb, waa]
  const float4 moments_b = read_imagef(moments_b_img, sampleri, (int2)(x, y));   // [wbb, wab, unweighted, 0]

  const float weight_sum = fmax(moments_a.x, 1e-9f);
  const float inv_weight = 1.f / weight_sum;
  // MATHS BRIDGE -- article step 3 single-guide fallback: v_hat = a*u + d, one 1x1 normal equation.
  const float mean_target = (orientation ? moments_a.z : moments_a.y) * inv_weight;   // E[v] (target)
  const float mean_guide = (orientation ? moments_a.y : moments_a.z) * inv_weight;    // E[u] (guide)
  const float var_guide = fmax((orientation ? moments_a.w : moments_b.x) * inv_weight - mean_guide * mean_guide, 0.f);   // Var(u)
  const float var_target = fmax((orientation ? moments_b.x : moments_a.w) * inv_weight - mean_target * mean_target, 0.f);   // Var(v)
  const float covariance = moments_b.y * inv_weight - mean_target * mean_guide;       // Cov(u,v)
  const float slope = covariance / (var_guide * (1.f + 1e-3f) + 1e-12f);              // a = Cov(u,v)/Var(u), 1e-3 relative ridge
  const float r_sq = clamp(covariance * covariance / (var_guide * var_target + 1e-18f), 0.f, 1.f);   // R^2 = Cov^2/(Var u Var v)

  coeff_slope[i] = slope;
  // intercept of the CENTERED fit, unshifted back to absolute values: d = E[v] - a E[u]
  coeff_intercept[i] = (mean_target + shift_target) - slope * (mean_guide + shift_guide);
  fit_quality[i] = r_sq;
  const int mass_ok = (moments_b.z > min_mass && moments_a.x > 0.25f * moments_b.z);
  const int valid_ok = (valid[i * 4 + target_channel] >= 0.5f);
  anchor[i] = (mass_ok && valid_ok && r_sq > 0.25f && fabs(slope) < 64.f);   // R^2 > 0.25, bounded slope -> Dirichlet anchor
  broad[i] = (mass_ok && valid_ok);
}

// Apply the smoothed pair fit: at pixels where the target channel tc is clipped, the guide
// gc holds real data, and the OTHER channel oc is also clipped (a two-channels-clipped
// pixel), hard-overwrite est with slope x guide + intercept and record the fit quality in
// bsc. The deep channel's deferred evaluation (hl_cf_eval_deep) later blends this value
// back in where appropriate.
kernel void
hl_cf_eval_pair(global const float *coeff_slope, global const float *coeff_intercept, global const float *fit_quality,
                global const float *valid, global float *estimate, global float *model_quality,
                const int width, const int height, const int target_channel, const int guide_channel, const int other_channel)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  if(valid[i * 4 + target_channel] < 0.5f && valid[i * 4 + guide_channel] >= 0.5f && valid[i * 4 + other_channel] < 0.5f)
  {
    // evaluation v_hat = a*u + d against the measured guide (diffused a,d; true guide pixel)
    estimate[i * 4 + target_channel] = coeff_slope[i] * estimate[i * 4 + guide_channel] + coeff_intercept[i];
    model_quality[i * 4 + target_channel] = clamp(fit_quality[i], 0.f, 1.f);
  }
}

// Binary indicator image: 1 where the deep channel cdeep is clipped AND at least one of its
// two guides is clipped too (a "multi-clip" pixel). The host gaussian-blurs this into the
// feathered 0..1 mask that hl_cf_eval_deep reads as its blend weight.
kernel void
hl_cf_pack_deepmask(global const float *valid, write_only image2d_t output,
                    const int width, const int height, const int deep_channel, const int guide1, const int guide2)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  const int is_multiclip = (valid[i * 4 + deep_channel] < 0.5f
                 && (valid[i * 4 + guide1] < 0.5f || valid[i * 4 + guide2] < 0.5f));
  write_imagef(output, (int2)(x, y), (float4)(is_multiclip ? 1.f : 0.f, 0.f, 0.f, 0.f));
}

// Deferred deep-channel evaluation, run last: where the deep channel cdeep is clipped and at
// least one channel is still valid, blend two candidates -- the chained joint prediction
// a*g1 + b*g2 + d (dominant near the rim of the multi-clip zone, acting as the "fence") and
// the one-hop pair value already written into est by hl_cf_eval_pair (dominant deep inside
// the zone). The blend weight wd maps the blurred multi-clip mask through a smooth ramp
// (0 below mask value 0.7, 1 above 0.95) so the hand-over shows no seam; bsc gets the same
// blend of the two fit qualities.
kernel void
hl_cf_eval_deep(global const float *coeff_a, global const float *coeff_b, global const float *coeff_d,
                global const float *fit_quality, read_only image2d_t deepmask,
                global const float *valid, global float *estimate, global float *model_quality,
                const int width, const int height, const int deep_channel, const int guide1, const int guide2)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  const int anyvalid = (valid[i * 4 + 0] >= 0.5f) || (valid[i * 4 + 1] >= 0.5f)
                       || (valid[i * 4 + 2] >= 0.5f);
  if(valid[i * 4 + deep_channel] >= 0.5f || !anyvalid) return;

  // deferred deep-channel evaluation v_hat = a*u1 + b*u2 + d (stashed diffused coefficients,
  // now-reconstructed continuous guides), blended with the one-hop pair value by the depth split
  const float joint_pred = coeff_a[i] * estimate[i * 4 + guide1] + coeff_b[i] * estimate[i * 4 + guide2] + coeff_d[i];
  const int has_pair = (valid[i * 4 + guide1] < 0.5f || valid[i * 4 + guide2] < 0.5f);
  const float mask_value = clamp(read_imagef(deepmask, sampleri, (int2)(x, y)).x, 0.f, 1.f);
  const float ramp = clamp((mask_value - 0.7f) / 0.25f, 0.f, 1.f);
  const float blend_weight = has_pair ? ramp * ramp * (3.f - 2.f * ramp) : 0.f;
  estimate[i * 4 + deep_channel] = blend_weight * estimate[i * 4 + deep_channel] + (1.f - blend_weight) * joint_pred;
  model_quality[i * 4 + deep_channel] = blend_weight * model_quality[i * 4 + deep_channel] + (1.f - blend_weight) * clamp(fit_quality[i], 0.f, 1.f);
}


// ===== HF-band hybrid guiding kernels (harmonic transposition) ===============================
// "HF band" = high-frequency detail band: est minus its gaussian-blurred copy (the "low"
// image). Texture lost to clipping is rebuilt by transferring the guides' detail band into
// the clipped channel with fitted gains, then arbitrating per pixel between that transfer
// and the channel's own damped detail. Mirrors the detail-band stage of the CPU
// _region_guided_filter, driven by _hf_stage_cl -- any change here must be mirrored there
// and re-validated with the HL_HFCL_TEST self-test.
//
// MATHS BRIDGE -- Step 4 (HF refit), article §"Hybrid Laplacian-band guiding of the high frequencies"
// / §"Rebuild the high frequencies": the estimate is split at sigma/4 into a low band ubar (the "low"
// image) and a detail band u - ubar. The detail band gets its OWN windowed colour-line with R^2-shrunk
// gains (hl_hf_fit: on a zero-mean band shrinkage is the correct estimator -- no magnitude to lose,
// only noise to not print), diffused by the E_transport fill. Then the HF is blended between the guided
// resynthesis h_g = a(u_g1-ubar_g1)+b(u_g2-ubar_g2) (hl_hf_energy/hl_hf_eval) and the R^2-damped
// transfer h_d = R^2 (u_c - ubar_c) by quadratic min-energy odds w = e_d^2/(e_d^2 + e_g^2),
// e_{d,g} = blurred |HF_{d,g}| -- an edge-misfire spikes the guided HF energy e_g, so w -> 0 and the
// damped path wins there (the failure self-detects, no content discriminator needed). Single-guide
// pixels keep only the damped detail (hl_hf_damp).

// Plain copy of a 4-float-per-pixel buffer into an OpenCL image, because the gaussian-blur
// routine consumes images, not buffers.
kernel void
hl_buf_to_img(global const float *buffer, write_only image2d_t output, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  write_imagef(output, (int2)(x, y), (float4)(buffer[i*4+0], buffer[i*4+1], buffer[i*4+2], buffer[i*4+3]));
}

// Pack the detail-band windowed-moment product images: hR/hG/hB = est minus its blurred copy
// (the per-channel detail), weighted like the coefficient-field packs (all three channels
// valid x soft luminance weight, reading the frozen pre-ladder luminance plane lsbp).
// Three launches fill three images (entries are weight x detail products):
// mode 0: [n, hR, hG, hB]  mode 1: [hRR, hGG, hBB, hRG]  mode 2: [hRB, hGB, unweighted, 0]
kernel void
hl_hf_pack(global const float *estimate, global const float *valid, global const float *luminance_plane,
           read_only image2d_t low_image,
           write_only image2d_t output, const int width, const int height,
           const float cf_binv, const int mode)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  const float4 low_pixel = read_imagef(low_image, sampleri, (int2)(x, y));
  // detail band H = est - ubar (the low band): a zero-mean high-frequency plane per channel
  const float detail_r = estimate[i*4+0] - low_pixel.x;
  const float detail_g = estimate[i*4+1] - low_pixel.y;
  const float detail_b = estimate[i*4+2] - low_pixel.z;
  const float luminance = luminance_plane[i];   // pre-ladder luminance (see hl_cf_pack_joint)
  const float bright_weight = (cf_binv > 0.f) ? min(luminance * cf_binv, 1.f) * min(luminance * cf_binv, 1.f) : 1.f;
  const int all_valid = (valid[i*4+0] >= 0.5f && valid[i*4+1] >= 0.5f && valid[i*4+2] >= 0.5f);
  const float weight = all_valid ? bright_weight : 0.f;
  float4 out_pixel;
  if(mode == 0)      out_pixel = (float4)(weight, weight * detail_r, weight * detail_g, weight * detail_b);
  else if(mode == 1) out_pixel = (float4)(weight * detail_r * detail_r, weight * detail_g * detail_g, weight * detail_b * detail_b, weight * detail_r * detail_g);
  else               out_pixel = (float4)(weight * detail_r * detail_b, weight * detail_g * detail_b, all_valid ? 1.f : 0.f, 0.f);
  write_imagef(output, (int2)(x, y), out_pixel);
}

// Detail-band fit: the same 2x2 solve as hl_cf_fit_joint but on the blurred detail moments,
// giving gains aH/bH that predict channel c's detail from the two guides' details. Each gain
// is multiplied by the fit quality r2H ("shrunk"), so weak fits transfer proportionally less
// texture. Writes the gain planes ga/gb and the anchor byte mask (enough unweighted window
// mass, channel c valid, bounded gains); the harmonic fill then smooths the gains across the
// hole.
kernel void
hl_hf_fit(read_only image2d_t moments1_img, read_only image2d_t moments2_img, read_only image2d_t moments3_img,
          global const float *valid, const int width, const int height,
          const int c, const int guide1, const int guide2, const float min_mass,
          global float *gain_a, global float *gain_b, global uchar *anchor)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  const float4 moments1 = read_imagef(moments1_img, sampleri, (int2)(x, y));
  const float4 moments2 = read_imagef(moments2_img, sampleri, (int2)(x, y));
  const float4 moments3 = read_imagef(moments3_img, sampleri, (int2)(x, y));
  const float weight_sum = fmax(moments1.x, 1e-9f);
  const float inv_weight = 1.f / weight_sum;
  const float means[3] = { moments1.y * inv_weight, moments1.z * inv_weight, moments1.w * inv_weight };
  const float second_moments[3] = { moments2.x * inv_weight, moments2.y * inv_weight, moments2.z * inv_weight };
  const float off_rg = moments2.w * inv_weight;
  const float off_rb = moments3.x * inv_weight;
  const float off_gb = moments3.y * inv_weight;
  #define OFF(ch_a, ch_b) (((ch_a) + (ch_b)) == 1 ? off_rg : (((ch_a) + (ch_b)) == 2 ? off_rb : off_gb))
  const float mean_guide1 = means[guide1];
  const float mean_guide2 = means[guide2];
  const float mean_target = means[c];
  const float var_11 = fmax(second_moments[guide1] - mean_guide1 * mean_guide1, 0.f);
  const float var_22 = fmax(second_moments[guide2] - mean_guide2 * mean_guide2, 0.f);
  const float var_12 = OFF(guide1, guide2) - mean_guide1 * mean_guide2;
  const float cov_1 = OFF(c, guide1) - mean_target * mean_guide1;
  const float cov_2 = OFF(c, guide2) - mean_target * mean_guide2;
  const float var_target = fmax(second_moments[c] - mean_target * mean_target, 0.f);
  #undef OFF
  // MATHS BRIDGE -- article step 4: the SAME 2x2 normal equations as hl_cf_fit_joint, but on the
  // detail-band moments, giving raw gains (a,b) that predict channel c's detail from the two guides'
  // details; R^2 = fraction of the target detail's variance the fit explains. u1=guide1, u2=guide2, v=c.
  const float lambda = 1e-3f * 0.5f * (var_11 + var_22) + 1e-12f;   // relative Tikhonov ridge
  const float determinant = fmax((var_11 + lambda) * (var_22 + lambda) - var_12 * var_12, 1e-18f);   // det Sigma
  const float gain_a_raw = ((var_22 + lambda) * cov_1 - var_12 * cov_2) / determinant;   // a = (Sigma^-1 RHS)_1 (Cramer)
  const float gain_b_raw = ((var_11 + lambda) * cov_2 - var_12 * cov_1) / determinant;   // b = (Sigma^-1 RHS)_2 (Cramer)
  const float r_sq = clamp((gain_a_raw * cov_1 + gain_b_raw * cov_2) / (var_target + 1e-12f), 0.f, 1.f);   // R^2
  // R^2-shrunk gains: on a zero-mean detail band shrinkage IS the correct estimator (a weak
  // colour-line then transfers proportionally less texture), unlike the full-signal fit which keeps a,b raw
  gain_a[i] = gain_a_raw * r_sq;
  gain_b[i] = gain_b_raw * r_sq;
  const int mass_ok = (moments3.z > min_mass && moments1.x > 0.25f * moments3.z);
  anchor[i] = (mass_ok && valid[i*4+c] >= 0.5f && fabs(gain_a[i]) < 64.f && fabs(gain_b[i]) < 64.f);
}

// Local magnitudes of the two candidate details for channel c, written as an image the host
// blurs afterwards: hg = guide-transferred detail (fitted gains x the guides' details) and
// hd = the channel's own current detail damped by its fit quality bsc. The blurred
// magnitudes drive the per-pixel arbitration in hl_hf_eval.
kernel void
hl_hf_energy(global const float *estimate, global const float *valid, global const float *model_quality,
             read_only image2d_t low_image, global const float *gain_a, global const float *gain_b,
             write_only image2d_t output, const int width, const int height,
             const int c, const int guide1, const int guide2)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  const float4 low_pixel = read_imagef(low_image, sampleri, (int2)(x, y));
  const float low_band[3] = { low_pixel.x, low_pixel.y, low_pixel.z };
  // h_g = a(u_g1-ubar_g1) + b(u_g2-ubar_g2): the guide-transferred detail (diffused shrunk gains)
  const float detail_guide = gain_a[i] * (estimate[i*4+guide1] - low_band[guide1]) + gain_b[i] * (estimate[i*4+guide2] - low_band[guide2]);
  // h_d = R^2 (u_c - ubar_c): the channel's own detail, damped by its diffused fit quality
  const float detail_own = clamp(model_quality[i*4+c], 0.f, 1.f) * (estimate[i*4+c] - low_band[c]);
  // write |h_g|, |h_d|; the host blurs these into the local energies e_g, e_d the min-energy odds use
  write_imagef(output, (int2)(x, y), (float4)(fabs(detail_guide), fabs(detail_own), 0.f, 0.f));
}

// Rebuild the detail of clipped-channel pixels whose two guides both hold real data:
// new est = low (the blurred base) + a weighted mix of the guide-transferred detail hg and
// the damped own detail hd. The weight wen compares the two blurred local magnitudes
// (squared ratio) and favours whichever candidate is locally SMALLER, so the quieter
// solution wins over the noisier one.
kernel void
hl_hf_eval(global float *estimate, global const float *valid, global const float *model_quality,
           read_only image2d_t low_image, read_only image2d_t energy,
           global const float *gain_a, global const float *gain_b,
           const int width, const int height, const int c, const int guide1, const int guide2)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  if(!(valid[i*4+c] < 0.5f && valid[i*4+guide1] >= 0.5f && valid[i*4+guide2] >= 0.5f)) return;
  const float4 low_pixel = read_imagef(low_image, sampleri, (int2)(x, y));
  const float low_band[3] = { low_pixel.x, low_pixel.y, low_pixel.z };
  const float detail_guide = gain_a[i] * (estimate[i*4+guide1] - low_band[guide1]) + gain_b[i] * (estimate[i*4+guide2] - low_band[guide2]);
  const float detail_own = clamp(model_quality[i*4+c], 0.f, 1.f) * (estimate[i*4+c] - low_band[c]);
  const float4 energy_pixel = read_imagef(energy, sampleri, (int2)(x, y));
  // quadratic min-energy odds: e_g = energy.x = blurred|h_g|, e_d = energy.y = blurred|h_d|;
  // w = e_d^2/(e_d^2 + e_g^2) favours whichever candidate carries LESS local HF energy -- a
  // guide misfire spikes e_g, driving w -> 0 so the damped path wins exactly there
  const float weight_guide = energy_pixel.y * energy_pixel.y / fmax(energy_pixel.y * energy_pixel.y + energy_pixel.x * energy_pixel.x, 1e-18f);
  // resynthesis: u_c = ubar_c + w*h_g + (1-w)*h_d
  estimate[i*4+c] = low_band[c] + weight_guide * detail_guide + (1.f - weight_guide) * detail_own;
}

// Pixels where exactly ONE channel still holds real data get no guide transfer: each clipped
// channel keeps its blurred base plus its own detail scaled down by its fit quality bsc, so
// poorly-predicted channels lose their unreliable texture instead of amplifying it.
kernel void
hl_hf_damp(global float *estimate, global const float *valid, global const float *model_quality,
           read_only image2d_t low_image, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  const int n_valid = (valid[i*4+0] >= 0.5f) + (valid[i*4+1] >= 0.5f) + (valid[i*4+2] >= 0.5f);
  if(n_valid != 1) return;
  const float4 low_pixel = read_imagef(low_image, sampleri, (int2)(x, y));
  const float low_band[3] = { low_pixel.x, low_pixel.y, low_pixel.z };
  for(int c = 0; c < 3; c++)
    if(valid[i*4+c] < 0.5f)
    {
      // no second guide -> no h_g candidate: keep only the R^2-damped own detail
      // u_c = ubar_c + R^2 (u_c - ubar_c)
      const float detail_keep = clamp(model_quality[i*4+c], 0.f, 1.f);
      estimate[i*4+c] = low_band[c] + detail_keep * (estimate[i*4+c] - low_band[c]);
    }
}


// ===== floors, dome gate and hue-coupled self-dome kernels ==================================
// clip0 = the channel value at the clipping threshold; a reconstructed value may never go
// below it ("the light was at least this bright"). The "self dome" rebuilds brightness
// inside large blown areas as a biharmonic dome: a smooth hill-shaped surface continuing the
// brightness from the rim, smooth in both value and slope so no crease shows at the rim.
// Mirrors the CPU floors + _biharmonic_dome path, driven by _selfdome_stage_cl -- any change
// here must be mirrored there and re-validated with the HL_DOMECL_TEST self-test.

// Soft saturation floor on every clipped channel: push est back above its clip level clip0,
// rounding the transition over about 2% of the clip level (a smooth maximum computed as
// half of the difference plus the square root of difference squared plus width squared) so
// no hard crease appears where estimates cross the floor. Valid channels are untouched.
// MATHS BRIDGE -- article step 5: out = c0 + 1/2 ( (e-c0) + sqrt((e-c0)^2 + (0.02 c0)^2) ), a smooth
// max(e, c0) softened over width 0.02*c0 so the hard max()'s floor-binding contour never prints as an edge.
// JOINT (hue-preserving) form -- mirror of the CPU Step-5 floor in coefficient_field.c: a per-channel
// independent floor imprints the floors' own chroma (the WB coefficients = magenta) on multi-clip
// pixels; instead ONE scalar lift of the whole clipped subset meets the most-demanding floor while
// the reconstruction's hue survives. Identical to the per-channel soft floor for 1-clip pixels. The
// lift is capped at 8x and the per-channel soft floor runs after as the degenerate-pixel safety net.
// JOINT (chromaticity-preserving) variant blended by the clip-asymmetry floor gate (see the CPU
// Step-5 floor in coefficient_field.c): per-channel at gate 0 (approved unit-WB behavior), one
// scalar lift of the clipped subset at gate 1 (real-camera WB'd clips, where the per-channel
// imprint is the inverse-WB magenta).
kernel void
hl_soft_floor(global float *estimate, global const float *valid, global const float *clip0,
              const int width, const int height, const float floor_gate)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  float lift = 1.f;
  if(floor_gate > 1e-6f)
    for(int c = 0; c < 3; c++)
      if(valid[i * 4 + c] < 0.5f)
      {
        const float e = fmax(estimate[i * 4 + c], 1e-6f);
        const float clip_level = clip0[i * 4 + c];
        const float diff = e - clip_level;
        const float soft_width = 0.02f * fmax(clip_level, 1e-6f);
        const float target = clip_level + 0.5f * (diff + sqrt(diff * diff + soft_width * soft_width));
        lift = fmax(lift, fmin(target / e, 8.f));
      }
  for(int c = 0; c < 3; c++)
    if(valid[i * 4 + c] < 0.5f)
    {
      const float clip_level = clip0[i * 4 + c];               // c0, the saturated reading
      const float soft_width = 0.02f * fmax(clip_level, 1e-6f); // transition width = 2% of c0
      const float diff = estimate[i * 4 + c] - clip_level;     // e - c0
      const float per_chan = clip_level + 0.5f * (diff + sqrt(diff * diff + soft_width * soft_width));
      if(floor_gate <= 1e-6f)
      {
        estimate[i * 4 + c] = per_chan; // bit-exact approved path
        continue;
      }
      const float lifted = fmax(estimate[i * 4 + c], 1e-6f) * lift;
      const float diff_joint = lifted - clip_level;
      const float joint
          = clip_level + 0.5f * (diff_joint + sqrt(diff_joint * diff_joint + soft_width * soft_width));
      estimate[i * 4 + c] = floor_gate * joint + (1.f - floor_gate) * per_chan;
    }
}

// Hard saturation floor: clamp every clipped channel of est to at least its clip level
// clip0. Re-asserted after the self dome, whose blend may have pulled values below the
// floor. JOINT form (see hl_soft_floor / the CPU Step-5 floor for the rationale).
kernel void
hl_hard_floor(global float *estimate, global const float *valid, global const float *clip0,
              const int width, const int height, const float floor_gate)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  float lift = 1.f;
  if(floor_gate > 1e-6f)
    for(int c = 0; c < 3; c++)
      if(valid[i * 4 + c] < 0.5f)
      {
        const float e = fmax(estimate[i * 4 + c], 1e-6f);
        lift = fmax(lift, fmin(fmax(e, clip0[i * 4 + c]) / e, 8.f));
      }
  for(int c = 0; c < 3; c++)
    if(valid[i * 4 + c] < 0.5f)
    {
      const float per_chan = fmax(estimate[i * 4 + c], clip0[i * 4 + c]);
      if(floor_gate <= 1e-6f)
      {
        estimate[i * 4 + c] = per_chan; // bit-exact approved path
        continue;
      }
      const float joint = fmax(fmax(estimate[i * 4 + c], 1e-6f) * lift, clip0[i * 4 + c]);
      estimate[i * 4 + c] = floor_gate * joint + (1.f - floor_gate) * per_chan;
    }
}

// Build the luminance-proxy plane (lsb = red + green + blue of est, a cheap brightness
// measure) and the byte hole mask: with allmode = 1 a hole is a pixel where ALL three
// channels are clipped, with allmode = 0 a pixel where ANY channel is clipped.
// MATHS BRIDGE -- step 7 magnitude/chrominance split: lsb = L_sum = R+G+B (the summed luminance
// that gets the one shared biharmonic dome); allmode=1 marks the all-clip core Omega (no survivor)
// this joint-core stage rebuilds.
kernel void
hl_lsb_hole(global const float *estimate, global const float *valid,
            global float *luminance, global uchar *hole, const int width, const int height,
            const int allmode)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  luminance[i] = estimate[i * 4 + 0] + estimate[i * 4 + 1] + estimate[i * 4 + 2]; // L_sum = R+G+B
  const int clipped_r = (valid[i * 4 + 0] < 0.5f);
  const int clipped_g = (valid[i * 4 + 1] < 0.5f);
  const int clipped_b = (valid[i * 4 + 2] < 0.5f);
  hole[i] = allmode ? (clipped_r && clipped_g && clipped_b) : (clipped_r || clipped_g || clipped_b);
}

// One chroma-ratio plane: ratio = channel c of est divided by the luminance proxy lsb
// (floored at eps so dark pixels never divide by zero). The dome stage smooths these ratios
// with the harmonic fill so hue stays coherent while the brightness is replaced by the dome.
// MATHS BRIDGE -- article step 6, the hue-coupled chromaticity: r_c = est_c / L_sum is a BOUNDED
// quantity, so it is carried inward by a plain harmonic fill (Delta r = 0) while brightness gets the
// biharmonic dome; recombining as dome_c = L_dome * r_c couples the channels so the hue cannot drift.
kernel void
hl_ratio_plane(global const float *estimate, global const float *luminance, global float *ratio,
               const int width, const int height, const int c, const float epsilon)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  ratio[i] = estimate[i * 4 + c] / fmax(luminance[i], epsilon);
}

// Pyramid downsample with the DOME's semantics (contrast with hl_fill_down, which averages
// anchors): coarse value = mean of the NON-hole fine pixels of the block (0 if none), coarse
// hole flag = 1 when hole pixels are the majority of the block. Feeds the biharmonic-dome
// solve on the coarse grid. The downsample keeps the coarse hole small enough for the direct
// SPD Cholesky (O(N^3)); the dome is low-frequency, so solving Delta^2 u = 0 coarse then bilinearly
// upsampling loses nothing. Boundary cells keep real rim data (majority-hole flag, non-hole mean).
// Pull the harmonically-filled chroma ratio toward the mean valid chromaticity inside the hole
// (mirrors the CPU _selfdome cmean pull, beta = 0.5 * floor_gate): the rim-harmonic value is
// biased toward the fence band's chromaticity; the flat mean lifts it toward the true surround.
// Only enqueued when the clip-asymmetry gate is open (beta > 0).
kernel void
hl_ratio_cmean_blend(global float *ratio, global const uchar *hole,
                     const int width, const int height, const float cmeanc, const float beta)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  if(!hole[i]) return;
  ratio[i] = (1.f - beta) * fmax(ratio[i], 0.f) + beta * cmeanc;
}

kernel void
hl_dome_down(global const float *value, global const uchar *hole,
             global float *coarse_value, global uchar *coarse_hole,
             const int width, const int height, const int coarse_w, const int coarse_h, const int step)
{
  const int coarse_x = get_global_id(0);
  const int coarse_y = get_global_id(1);
  if(coarse_x >= coarse_w || coarse_y >= coarse_h) return;
  float accum = 0.f;
  int n_valid = 0;
  int n_hole = 0;
  int n_total = 0;
  for(int y = coarse_y * step; y < min((coarse_y + 1) * step, height); y++)
    for(int x = coarse_x * step; x < min((coarse_x + 1) * step, width); x++)
    {
      const int i = y * width + x;
      n_total++;
      if(hole[i]) n_hole++;
      else { accum += value[i]; n_valid++; }
    }
  coarse_value[coarse_y * coarse_w + coarse_x] = n_valid ? accum / n_valid : 0.f;
  coarse_hole[coarse_y * coarse_w + coarse_x] = (2 * n_hole > n_total);
}

// Depth-gated hue-coupled dome blend, on any-clip hole pixels and their clipped channels
// only. dome value = the dome luminance ldb x this channel's share of the smoothed chroma
// ratios (rr[c] / sum of the three). The keep weight per channel starts from the fit quality
// bsc mapped through a smooth ramp (0 below 0.4, 1 above 0.85), then is pulled toward the
// dome near the rim by a gaussian of dep (the distance-transform depth: how deep inside the
// blown zone the pixel sits, in units of 1.5 x the blur sigma):
// we = 1 - (1 - ramp) * gaussian, squared. Where NO channel survived (anyvalid = 0) the dome
// wins outright; elsewhere est = we * est + (1 - we) * dome.
// MATHS BRIDGE -- article step 6: keep = 1 - dome_fraction with
//   dome_fraction = (1 - S_{0.4}^{0.85}(R^2)) * exp(-(delta/1.5 sigma)^2),
// R^2 (bsc) asking "is the colour-line real" via smoothstep S, delta (dep) asking "is the dome
// trustworthy" via a gaussian of depth; dome_c = L_dome * (r_c / sum r) recombines hue-coupled.


// CHROMA-DECOUPLED variant blended by the clip-asymmetry floor gate (mirrors the CPU _selfdome
// recombine): the per-channel blend lets the colour-line fit's biased chromaticity survive on
// multi-clip pixels; decoupling keeps the blend's summed LUMINANCE but reprojects the clipped
// subset onto the dome's chromaticity. At gate 0 the per-channel blend runs verbatim.
kernel void
hl_dome_blend(global float *estimate, global const float *valid, global const float *model_quality,
              global const float *clip_depth, global const float *dome_lum,
              global const float *ratio0, global const float *ratio1, global const float *ratio2,
              global const uchar *hole, const int width, const int height,
              const float cf_sigma, const float epsilon, const float floor_gate)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  if(!hole[i]) return;
  const float ratios[3] = { fmax(ratio0[i], 0.f), fmax(ratio1[i], 0.f), fmax(ratio2[i], 0.f) };
  const float ratio_sum = fmax(ratios[0] + ratios[1] + ratios[2], epsilon);
  const int anyvalid = (valid[i * 4 + 0] >= 0.5f) || (valid[i * 4 + 1] >= 0.5f)
                       || (valid[i * 4 + 2] >= 0.5f);
  // the approved per-channel depth-gated blend, kept verbatim as the gate-0 path and as one leg
  // of the gated chroma-decoupled blend below (mirror of the CPU _selfdome recombine)
  float per_channel_blend[3];
  float domes[3];
  float blended_sub = 0.f;
  float dome_sub = 0.f;
  for(int c = 0; c < 3; c++)
    if(valid[i * 4 + c] < 0.5f)
    {
      const float quality_ramp = clamp((model_quality[i * 4 + c] - 0.4f) / 0.45f, 0.f, 1.f);   // ramp arg (0.4..0.85)
      const float weight_quality = quality_ramp * quality_ramp * (3.f - 2.f * quality_ramp);   // S_{0.4}^{0.85}(R^2)
      const float depth_norm = clip_depth[i] / (1.5f * cf_sigma);                                // delta / (1.5 sigma)
      const float depth_gauss = exp(-depth_norm * depth_norm);                                   // exp(-(delta/1.5 sigma)^2)
      const float keep_root = sqrt(clamp(1.f - (1.f - weight_quality) * depth_gauss, 0.f, 1.f)); // sqrt(keep)
      const float keep_weight = keep_root * keep_root;                                           // keep = 1 - dome_fraction
      const float dome_value = dome_lum[i] * (ratios[c] / ratio_sum);                            // dome_c = L_dome * chroma share
      // est = keep*est + (1-keep)*dome; no surviving guide -> take the dome outright
      per_channel_blend[c] = anyvalid ? (keep_weight * estimate[i * 4 + c] + (1.f - keep_weight) * dome_value) : dome_value;
      domes[c] = dome_value;
      blended_sub += per_channel_blend[c];
      dome_sub += dome_value;
    }
  for(int c = 0; c < 3; c++)
    if(valid[i * 4 + c] < 0.5f)
    {
      if(floor_gate <= 1e-6f || !anyvalid || dome_sub <= epsilon)
      {
        estimate[i * 4 + c] = per_channel_blend[c]; // bit-exact approved path
        continue;
      }
      const float decoupled = blended_sub * (domes[c] / dome_sub);
      estimate[i * 4 + c] = floor_gate * decoupled + (1.f - floor_gate) * per_channel_blend[c];
    }
}



// ===== all-clip joint core (shared luminance dome x diffused chromaticity) =================
// Inside a core where all three channels clipped there is no guide channel left: brightness
// comes from a single shared biharmonic dome (smooth hill continuing the rim, see the
// previous section) and colour comes from chroma ratios diffused inward from the rim by a
// screened diffusion solve ("screened" = with an extra pull toward a flat target colour,
// the user's "inpaint a flat colour" slider). The solve itself runs in highlights_sparse.cl
// (direct sparse Cholesky) or through the conjugate-gradient kernels further below when the
// core is too large. Mirrors the CPU joint core (_region_pde_solve and its neighbours),
// driven by _joint_core_stage_cl -- any change here must be mirrored there and re-validated
// with the HL_CORECL_TEST self-test.

// Floor the shared luminance dome at the saturated sum inside the all-clip core: every
// channel is at least at its clip level clip0 there, so the dome must never extrapolate
// below "all three channels at clip" (the sum of the three clip levels).
// MATHS BRIDGE -- step 7 saturation floor on the dome (article §"A saturation floor"):
// L_dome >= sum_c clip0_c. Monotone (only raises), so it never dims the rim or shifts the hue.
kernel void
hl_core_floor(global float *dome_lum, global const uchar *hole, global const float *clip0,
              const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  if(!hole[i]) return;
  // L_dome <- max(L_dome, clip0_R + clip0_G + clip0_B): floor at "all channels at clip"
  dome_lum[i] = fmax(dome_lum[i], clip0[i * 4 + 0] + clip0[i * 4 + 1] + clip0[i * 4 + 2]);
}

// Reduction for the flat target colour: per-workgroup partial sums of the chroma ratios
// (each est channel divided by the luminance proxy lsb) over fully-valid pixels, packed as
// (r, g, b, count); the host sums the per-group results into the mean chroma cmean that the
// screened solve pulls toward.
// MATHS BRIDGE -- step 7 flat-colour target r_target = <RGB/L_sum> over fully-valid pixels
// (article's bar-c_c): the screened-Poisson reaction damps the core chroma toward this mean.
// lum_min gates the accumulation to BRIGHT valid pixels (>= 0.35 x blown-zone plateau) for the
// refinements' surround reference -- the all-valid mean is contaminated by dark foreground (the
// cgrad anchors learned the same lesson). Pass 0.f to keep every valid pixel (the approved
// flat-colour solver target).
kernel void
hl_cmean_reduce(global const float *estimate, global const float *valid, global const float *luminance,
                global float4 *partial, const int n_pixels, const float epsilon, const float lum_min,
                local float4 *scratch)
{
  const int global_id = get_global_id(0);
  const int global_size = get_global_size(0);
  const int local_id = get_local_id(0);
  const int local_size = get_local_size(0);

  float4 accum = (float4)(0.f);
  for(int i = global_id; i < n_pixels; i += global_size)
    if(valid[i * 4 + 0] >= 0.5f && valid[i * 4 + 1] >= 0.5f && valid[i * 4 + 2] >= 0.5f
       && luminance[i] >= lum_min)
    {
      const float inv_lum = 1.f / fmax(luminance[i], epsilon);
      accum += (float4)(estimate[i * 4 + 0] * inv_lum, estimate[i * 4 + 1] * inv_lum, estimate[i * 4 + 2] * inv_lum, 1.f);
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



// Trusted-ring validation of the flat-mean colour prior (mirror of the CPU
// _hl_ring_flat_mean_vote): per-workgroup partial sums of the ring pixels' chromaticity shares
// {s0, s1, s2, count} and squares {s0^2, s1^2, s2^2, 0} over 1-clip pixels. The host computes
// the ring mean/dispersion and the vote t-statistic from them (see the CPU helper for the
// bias-over-dispersion rationale).
kernel void
hl_ring_vote(global const float *estimate, global const float *valid, global float4 *partial,
             const int n_pixels, local float4 *scratch)
{
  const int global_id = get_global_id(0);
  const int global_size = get_global_size(0);
  const int local_id = get_local_id(0);
  const int local_size = get_local_size(0);

  float4 accum_sum = (float4)(0.f);
  float4 accum_sq = (float4)(0.f);
  for(int i = global_id; i < n_pixels; i += global_size)
  {
    const int n_clipped = (valid[i * 4 + 0] < 0.5f) + (valid[i * 4 + 1] < 0.5f) + (valid[i * 4 + 2] < 0.5f);
    if(n_clipped != 1) continue;
    const float sum = fmax(estimate[i * 4 + 0] + estimate[i * 4 + 1] + estimate[i * 4 + 2], 1e-9f);
    const float s0 = estimate[i * 4 + 0] / sum;
    const float s1 = estimate[i * 4 + 1] / sum;
    const float s2 = estimate[i * 4 + 2] / sum;
    accum_sum += (float4)(s0, s1, s2, 1.f);
    accum_sq += (float4)(s0 * s0, s1 * s1, s2 * s2, 0.f);
  }
  scratch[local_id * 2 + 0] = accum_sum;
  scratch[local_id * 2 + 1] = accum_sq;
  barrier(CLK_LOCAL_MEM_FENCE);
  for(int offset = local_size / 2; offset > 0; offset /= 2)
  {
    if(local_id < offset)
    {
      scratch[local_id * 2 + 0] += scratch[(local_id + offset) * 2 + 0];
      scratch[local_id * 2 + 1] += scratch[(local_id + offset) * 2 + 1];
    }
    barrier(CLK_LOCAL_MEM_FENCE);
  }
  if(local_id == 0)
  {
    partial[get_group_id(0) * 2 + 0] = scratch[0];
    partial[get_group_id(0) * 2 + 1] = scratch[1];
  }
}

// Re-hue the all-clip core's saturation floor toward the mean valid chromaticity, blended by the
// clip-asymmetry floor gate (mirrors the CPU _joint_core rehue): with WB'd clips, clip0's own
// chromaticity is the inverse-WB magenta -- a magnitude floor, not a colour -- yet the aniso stage
// uses it as its ratio-space obstacle and reassembly floor, pinning the core to neutral raw.
// Redistributing clip0 to cmean preserves the magnitude sum_c clip0_c (cmean sums to 1 by
// construction) while the obstacle/floor now enforces the surround chromaticity. Only enqueued
// when the gate is open (untouched clip0 = approved behavior on equal clips).
kernel void
hl_clip0_rehue(global float *clip0, global const uchar *hole, const int width, const int height,
               const float cmean0, const float cmean1, const float cmean2, const float gate)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  if(!hole[i]) return;
  const float lsat = clip0[i * 4 + 0] + clip0[i * 4 + 1] + clip0[i * 4 + 2];
  const float cmean[3] = { cmean0, cmean1, cmean2 };
  for(int c = 0; c < 3; c++)
    clip0[i * 4 + c] = gate * (lsat * cmean[c]) + (1.f - gate) * clip0[i * 4 + c];
}

// Prepare the three planes of the core chroma solve for channel c. t1 = the known boundary
// values (the chroma ratio est/lsb outside the hole, forced to zero inside -- the rim values
// the solution must attach to); rat = the output ratio plane pre-seeded with the mean chroma
// cmeanc inside the hole (the solver's scatter overwrites the hole entries with the exact
// solution afterwards); u = the solver's starting guess (ratio outside, mean chroma inside).
// MATHS BRIDGE -- step 7 screened-Poisson setup for channel c: boundary_ratio = the Dirichlet rim
// data r_valid = est_c/L_sum (zero on the hole, so applying Op to it gives the eliminated boundary
// term); the hole seed r_target = cmeanc biases an under-converged centre toward the flat mean.
kernel void
hl_pde_init(global const float *estimate, global const float *luminance, global const uchar *hole,
            global float *boundary_ratio, global float *ratio, global float *solution,
            const int width, const int height, const int c, const float cmeanc, const float epsilon)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  const float chroma_ratio = estimate[i * 4 + c] / fmax(luminance[i], epsilon); // r_c = est_c / L_sum
  const float seed_value = hole[i] ? cmeanc : chroma_ratio;
  boundary_ratio[i] = hole[i] ? 0.f : seed_value;
  ratio[i] = fmax(seed_value, 0.f);
  solution[i] = seed_value;
}

// Feather source: copy the byte hole mask into a single-channel float image (1 inside the
// hole, 0 outside); the host gaussian-blurs it into the feather-ring weight image that
// hl_core_blend reads.
kernel void
hl_mask_to_img1(global const uchar *mask, write_only image2d_t output, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  write_imagef(output, (int2)(x, y), (float4)(mask[y * width + x] ? 1.f : 0.f));
}

// Feathered composite of the joint core. Inside the hole: est = the dome luminance ldb x
// each channel's share of the diffused chroma ratios (r0/r1/r2), at full strength. In the
// feather ring around the hole (blurred-mask weight wf > 0): blend that core value into the
// CLIPPED channels only, leaving measured data untouched.
// MATHS BRIDGE -- step 7 recombination + feathered hand-over: core_c = L_dome * (r_c / sum_j r_j)
// (RGB = L*r); the blurred core mask wf gives a smooth alpha-blend of the core reconstruction into
// the surrounding coefficient-field reconstruction (the method's one smooth weight: it blends two
// reconstructions, never reclassifies a measurement).
kernel void
hl_core_blend(global float *estimate, global const float *valid, global const uchar *hole,
              global const float *dome_lum, global const float *ratio0, global const float *ratio1,
              global const float *ratio2, read_only image2d_t feather_img,
              const int width, const int height, const float epsilon)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  const float feather_weight = clamp(read_imagef(feather_img, samplerA, (int2)(x, y)).x, 0.f, 1.f); // alpha (blurred core mask)
  const float ratios[3] = { ratio0[i], ratio1[i], ratio2[i] };
  const float ratio_sum = fmax(ratios[0] + ratios[1] + ratios[2], epsilon); // sum_j r_j

  if(hole[i])
  {
    // interior of the core: core_c = L_dome * (r_c / sum_j r_j), full strength
    for(int c = 0; c < 3; c++)
      estimate[i * 4 + c] = dome_lum[i] * (ratios[c] / ratio_sum);
  }
  else if(feather_weight > 1e-4f)
  {
    // feather ring: alpha*core_c + (1-alpha)*est on CLIPPED channels only (valid data untouched)
    for(int c = 0; c < 3; c++)
      if(valid[i * 4 + c] < 0.5f)
        estimate[i * 4 + c] = feather_weight * dome_lum[i] * (ratios[c] / ratio_sum) + (1.f - feather_weight) * estimate[i * 4 + c];
  }
}


// ===== structure-steered chroma diffusion (divergence form) ================================
// Anisotropic diffusion = smooth colours ALONG image edges but not across them; the local
// edge direction and strength live in a "structure tensor" (three planes txx/txy/tyy built
// from the gradient of the blurred luminance). The chroma ratios of the all-clip core are
// diffused with edge-dependent weights so the recovered colour follows the image structure.
// Mirrors the CPU _aniso_tensor / _aniso_div_solve path, driven by _aniso_stage_cl -- any
// change here must be mirrored there and re-validated with the HL_ANISOCL_TEST self-test.
//
// MATHS BRIDGE -- Step 8 / E_chrominance anisotropic (article §"Chrominance coherence"):
// div(D grad r) = 0, r = RGB/L_sum, Dirichlet r|dOmega = r_valid, obstacle r_c >= c0/L_sum.
// D = structure tensor (isophote t vs gradient g blend). Divergence form is discretized as the
// Weickert nonnegativity graph Laplacian (hl_aniso_weights), solved by a shared sparse Cholesky.

// Build the fine-level inputs of the anisotropic pass in one sweep: vldan = the
// diffusion-only validity mask (only all-clip pixels stay marked for diffusion; every other
// pixel is promoted to an anchor by raising its validity to at least 0.6), hole = byte
// all-clip mask, lsb = luminance proxy (sum of the three channels) floored at eps, and
// s1 = the per-channel chroma ratios (est / luminance).
kernel void
hl_aniso_prep(global const float *estimate, global const float *valid,
              global float *valid_anchor, global float *luminance, global float *chroma, global uchar *hole,
              const int width, const int height, const float epsilon)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  const int all_clipped = (valid[i * 4 + 0] < 0.5f && valid[i * 4 + 1] < 0.5f && valid[i * 4 + 2] < 0.5f);
  // vldan: all-clip pixels stay < 0.5 (they diffuse); every other pixel becomes a Dirichlet anchor
  // (validity raised to >= 0.6), so div(D grad r)=0 is restricted to the all-clip hole
  for(int c = 0; c < 4; c++)
    valid_anchor[i * 4 + c] = all_clipped ? valid[i * 4 + c] : fmax(valid[i * 4 + c], 0.6f);
  hole[i] = all_clipped;
  const float lum_sum = fmax(estimate[i * 4 + 0] + estimate[i * 4 + 1] + estimate[i * 4 + 2], epsilon); // L_sum
  luminance[i] = lum_sum;
  for(int c = 0; c < 3; c++)
    chroma[i * 4 + c] = estimate[i * 4 + c] / lum_sum; // r_c = est_c / L_sum
  chroma[i * 4 + 3] = 0.f;
}

// 3x3 box mean with edge-clamped neighbours; the host runs it twice, which approximates a
// small gaussian blur of the luminance before the gradients are taken.
kernel void
hl_box3(global const float *input, global float *output, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  float accum = 0.f;
  for(int offset_y = -1; offset_y <= 1; offset_y++)
    for(int offset_x = -1; offset_x <= 1; offset_x++)
    {
      const int sample_y = clamp(y + offset_y, 0, height - 1);
      const int sample_x = clamp(x + offset_x, 0, width - 1);
      accum += input[sample_y * width + sample_x];
    }
  output[y * width + x] = accum / 9.f;
}

// Centred-difference gradient of the blurred luminance (gx, gy = half the difference between
// the two neighbours on each axis, edge-clamped), plus a reduction: per-workgroup partial
// sums of the gradient magnitude, finished on the CPU into the mean gradient gnorm that
// normalises the edge-strength response in hl_aniso_tensor.
kernel void
hl_grad_reduce(global const float *blur, global float *grad_x, global float *grad_y,
               global float *partial, const int width, const int height, local float *scratch)
{
  const int global_id = get_global_id(0);
  const int global_size = get_global_size(0);
  const int local_id = get_local_id(0);
  const int local_size = get_local_size(0);
  const int n_pixels = width * height;

  float grad_sum = 0.f;
  for(int i = global_id; i < n_pixels; i += global_size)
  {
    const int y = i / width;
    const int x = i - y * width;
    const int x_west = max(x - 1, 0);
    const int x_east = min(x + 1, width - 1);
    const int y_north = max(y - 1, 0);
    const int y_south = min(y + 1, height - 1);
    const float grad_dx = 0.5f * (blur[y * width + x_east] - blur[y * width + x_west]);
    const float grad_dy = 0.5f * (blur[y_south * width + x] - blur[y_north * width + x]);
    grad_x[i] = grad_dx;
    grad_y[i] = grad_dy;
    grad_sum += sqrt(grad_dx * grad_dx + grad_dy * grad_dy);
  }
  scratch[local_id] = grad_sum;
  barrier(CLK_LOCAL_MEM_FENCE);
  for(int offset = local_size / 2; offset > 0; offset /= 2)
  {
    if(local_id < offset) scratch[local_id] += scratch[local_id + offset];
    barrier(CLK_LOCAL_MEM_FENCE);
  }
  if(local_id == 0) partial[get_group_id(0)] = scratch[0];
}

// Per-pixel structure tensor (txx, txy, tyy). (ux, uy) = unit vector along the gradient,
// pointing ACROSS the local edge; (ix, iy) = the perpendicular unit vector, pointing ALONG
// the edge. The tensor sums the products of the along-edge vector with itself (always full
// strength) and the products of the across-edge vector with itself damped by
// c2 = exp(-magnitude / (4 x mean gradient)): strong edges block cross-edge smoothing, flat
// areas diffuse equally in every direction. The nz guard makes zero-gradient pixels use a
// fixed direction instead of dividing by zero.
kernel void
hl_aniso_tensor(global const float *grad_x, global const float *grad_y,
                global float *tensor_xx, global float *tensor_xy, global float *tensor_yy,
                const int width, const int height, const float gnorm)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  const float grad_dx = grad_x[i];
  const float grad_dy = grad_y[i];
  const float magnitude = sqrt(grad_dx * grad_dx + grad_dy * grad_dy);
  const float nonzero = (magnitude > 1e-12f) ? 1.f : 0.f;
  const float inv_mag = nonzero / (magnitude + (1.f - nonzero));
  const float grad_ux = grad_dx * inv_mag + (1.f - nonzero);
  const float grad_uy = grad_dy * inv_mag;
  const float cross_damp = exp(-magnitude / (4.f * gnorm)); // c2 = exp(-|grad L|/(4 <|grad L|>))
  const float iso_x = -grad_uy; // isophote t = gradient g rotated 90 deg (level line)
  const float iso_y = grad_ux;
  // D = t t^T + c2 * g g^T (mirrors CPU _aniso_tensor)
  tensor_xx[i] = iso_x * iso_x + cross_damp * grad_ux * grad_ux;
  tensor_xy[i] = iso_x * iso_y + cross_damp * grad_ux * grad_uy;
  tensor_yy[i] = iso_y * iso_y + cross_damp * grad_uy * grad_uy;
}

// For each unknown hole pixel (indexed through pgrid), the eight edge weights toward its
// neighbours (order: west, east, north, south, then the four diagonals). Each weight
// averages the tensors of the two pixels sharing the edge, then splits the smoothing
// strength over axis and diagonal edges so that every weight stays non-negative (the
// fmax/clamp lines): axis edges get the axis term minus the shared diagonal part, diagonal
// edges get the matching sign of the mixed term. A weight of 0 marks an edge leaving the
// region border. Written compactly (8 floats per unknown) for the host matrix assembly and
// for hl_aniso_rhs in highlights_sparse.cl.
kernel void
hl_aniso_weights(global const float *tensor_xx, global const float *tensor_xy, global const float *tensor_yy,
                 global const int *pgrid, global float *edge_weights,
                 const int n_unknowns, const int width, const int height)
{
  const int unknown = get_global_id(0);
  if(unknown >= n_unknowns) return;
  const int grid_index = pgrid[unknown];
  const int origin_y = grid_index / width;
  const int origin_x = grid_index - origin_y * width;
  const int neighbor_dy[8] = { 0, 0, -1, 1, -1, 1, -1, 1 };
  const int neighbor_dx[8] = { -1, 1, 0, 0, -1, 1, 1, -1 };
  for(int direction = 0; direction < 8; direction++)
  {
    const int neighbor_x = origin_x + neighbor_dx[direction];
    const int neighbor_y = origin_y + neighbor_dy[direction];
    float weight_value = 0.f;
    if(neighbor_x >= 0 && neighbor_y >= 0 && neighbor_x < width && neighbor_y < height)
    {
      // edge-averaged tensor D = [[a,b],[b,c]]; b clamped to +-min(a,c) so every weight stays >= 0
      // (Weickert nonnegativity -> SPD M-matrix -> discrete maximum principle, mirrors _aniso_edge_w)
      const int j = neighbor_y * width + neighbor_x;
      const float weight_a = 0.5f * (tensor_xx[grid_index] + tensor_xx[j]); // a = D_xx
      const float weight_c = 0.5f * (tensor_yy[grid_index] + tensor_yy[j]); // c = D_yy
      const float min_ac = fmin(weight_a, weight_c);
      const float weight_b = clamp(0.5f * (tensor_xy[grid_index] + tensor_xy[j]), -min_ac, min_ac); // b
      if(neighbor_dy[direction] == 0) weight_value = fmax(weight_a - fabs(weight_b), 1e-4f);         // axis x: a - |b|
      else if(neighbor_dx[direction] == 0) weight_value = fmax(weight_c - fabs(weight_b), 1e-4f);    // axis y: c - |b|
      else if(neighbor_dx[direction] == neighbor_dy[direction]) weight_value = fmax(weight_b, 0.f);  // diag (+,+)/(-,-): +b
      else weight_value = fmax(-weight_b, 0.f);                                                      // diag (+,-)/(-,+): -b
    }
    edge_weights[unknown * 8 + direction] = weight_value;
  }
}

// Turn the diffused chroma ratios s1 back into channel values. Pixels with at least one
// valid channel: scale each clipped channel's ratio by (sum of the valid est values / sum of
// the valid ratios), so the surviving channels anchor the absolute brightness. All-clip
// pixels: split the dome luminance lsb by the ratio shares. Every rebuilt value is floored
// at its clip level clip0.
kernel void
hl_aniso_reassemble(global float *estimate, global const float *valid_anchor, global const float *luminance,
                    global const float *chroma, global const float *clip0,
                    const int width, const int height, const float epsilon, const float floor_gate)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  // only the all-clip core is ever written (vldan flags partially-valid pixels' channels
  // >= 0.6): magnitude = dome luminance split by the diffused ratios. Mirrors the CPU
  // reassembly (a ladder-era magnitude-transfer branch was removed from both).
  const float ratio_sum = fmax(chroma[i * 4 + 0] + chroma[i * 4 + 1] + chroma[i * 4 + 2], epsilon); // sum_j r_j

  // JOINT variant blended by the clip-asymmetry floor gate (see hl_soft_floor): one scalar lift of
  // the clipped subset preserves the diffused chromaticity; per-channel at gate 0.
  float lift = 1.f;
  if(floor_gate > 1e-6f)
    for(int c = 0; c < 3; c++)
      if(valid_anchor[i * 4 + c] < 0.5f)
      {
        const float ratio_c = fmax(chroma[i * 4 + c], 0.f);
        const float value = fmax(luminance[i] * ratio_c / ratio_sum, 1e-6f);
        const float clip_level = clip0[i * 4 + c];
        const float delta = value - clip_level;
        const float soft_width = 0.02f * fmax(clip_level, 1e-6f);
        const float target = clip_level + 0.5f * (delta + sqrt(delta * delta + soft_width * soft_width));
        lift = fmax(lift, fmin(target / value, 8.f));
      }

  for(int c = 0; c < 3; c++)
    if(valid_anchor[i * 4 + c] < 0.5f)
    {
      const float ratio_c = fmax(chroma[i * 4 + c], 0.f);
      const float magnitude = luminance[i] * ratio_c / ratio_sum; // u_c = L_sum * r_c / sum_j r_j (RGB = L*r)
      // SOFT saturation floor: the hard max() prints an exactly-flat shelf at the clip level
      // plus a gradient kink wherever the magnitude transfer under-predicts a channel near
      // its own rim. Mirrors the CPU reassembly.
      // u_c <- c0 + 0.5*((u-c0) + sqrt((u-c0)^2 + w^2)), w = 0.02*c0 (smooth max(u, c0), article rule 3)
      const float clip_level = clip0[i * 4 + c];
      const float diff = magnitude - clip_level;
      const float soft_width = 0.02f * fmax(clip_level, 1e-6f);
      const float per_chan = clip_level + 0.5f * (diff + sqrt(diff * diff + soft_width * soft_width));
      if(floor_gate <= 1e-6f)
      {
        estimate[i * 4 + c] = per_chan; // bit-exact approved path
        continue;
      }
      const float lifted = fmax(magnitude, 1e-6f) * lift;
      const float diff_joint = lifted - clip_level;
      const float joint
          = clip_level + 0.5f * (diff_joint + sqrt(diff_joint * diff_joint + soft_width * soft_width));
      estimate[i * 4 + c] = floor_gate * joint + (1.f - floor_gate) * per_chan;
    }
}


// ===== chromaticity-gradient continuation (article addendum) ================================
// Extend the BRIGHT valid surround's chroma shares into the blown zone biharmonically (value +
// gradient continuation, the same operator as the luminance dome), then re-hue every multi-clip
// pixel's clipped subset to the extended field. Every value-continuing estimator inherits the
// unrepresentative fence-band hue at the clip contour; the C1 continuation restores the scene's
// hue trend and is smooth across occluders by construction. Mirrors the CPU _chromaticity_gradient
// (core.c) -- any change here must be mirrored there and re-validated with HL_CGRADCL_TEST.

// Reduction: per-workgroup partial sums of {luminance, count} over any-clip pixels -- the host
// turns them into the plateau luminance that gates the anchors.
kernel void
hl_cgrad_plateau(global const float *estimate, global const float *valid, global float *partial,
               const int n_pixels, local float *scratch)
{
  const int gid = get_global_id(0), gsz = get_global_size(0);
  const int lid = get_local_id(0), lsz = get_local_size(0);
  float lum_sum = 0.f, count = 0.f;
  for(int i = gid; i < n_pixels; i += gsz)
    if(valid[i * 4 + 0] < 0.5f || valid[i * 4 + 1] < 0.5f || valid[i * 4 + 2] < 0.5f)
    {
      lum_sum += estimate[i * 4 + 0] + estimate[i * 4 + 1] + estimate[i * 4 + 2];
      count += 1.f;
    }
  scratch[lid * 2 + 0] = lum_sum;
  scratch[lid * 2 + 1] = count;
  barrier(CLK_LOCAL_MEM_FENCE);
  for(int off = lsz / 2; off > 0; off /= 2)
  {
    if(lid < off)
    {
      scratch[lid * 2 + 0] += scratch[(lid + off) * 2 + 0];
      scratch[lid * 2 + 1] += scratch[(lid + off) * 2 + 1];
    }
    barrier(CLK_LOCAL_MEM_FENCE);
  }
  if(lid == 0)
  {
    partial[get_group_id(0) * 2 + 0] = scratch[0];
    partial[get_group_id(0) * 2 + 1] = scratch[1];
  }
}

// Guard source: 1 inside the any-clip zone, 0 outside, written to a float IMAGE (the device
// gaussian works on images); the host blurs it into the proximity field so anchors can stand
// clear of the thin unrepresentative fence band at the clip contour.
kernel void
hl_cgrad_guard(global const float *valid, write_only image2d_t guard_src, const int width, const int height)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  const float any_clip
      = (valid[i * 4 + 0] < 0.5f || valid[i * 4 + 1] < 0.5f || valid[i * 4 + 2] < 0.5f) ? 1.f : 0.f;
  write_imagef(guard_src, (int2)(x, y), (float4)(any_clip));
}

// Anchor mask (as the dome's hole convention: hole = NOT anchor): anchors are fully-valid, bright
// (>= the host-computed plateau fraction) and clear of the blurred clip proximity.
kernel void
hl_cgrad_anchor(global const float *estimate, global const float *valid, read_only image2d_t guard_blur,
              global uchar *hole, const int width, const int height, const float lum_min)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  const int fully_valid
      = (valid[i * 4 + 0] >= 0.5f) && (valid[i * 4 + 1] >= 0.5f) && (valid[i * 4 + 2] >= 0.5f);
  const float lum = estimate[i * 4 + 0] + estimate[i * 4 + 1] + estimate[i * 4 + 2];
  const float proximity = read_imagef(guard_blur, sampleri, (int2)(x, y)).x;
  hole[i] = !(fully_valid && (lum >= lum_min) && (proximity < 0.05f));
}

// Chroma share plane for channel c (share everywhere; hole values only serve as the dome's guess).
kernel void
hl_cgrad_share(global const float *estimate, global float *field, const int width, const int height,
             const int c, const float epsilon)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  const float lum = estimate[i * 4 + 0] + estimate[i * 4 + 1] + estimate[i * 4 + 2];
  field[i] = estimate[i * 4 + c] / fmax(lum, epsilon);
}

// Store the extended share plane (clamped: shares are bounded, the dome may overshoot).
kernel void
hl_cgrad_store(global const float *field, global float *shares, const int width, const int height, const int c)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  shares[i * 4 + c] = clamp(field[i], 0.f, 1.f);
}

// Content gate source: at 1-clip pixels (the method's trusted zone -- reconstructed from TWO
// measured guides), compare the extended field's chromaticity to the solver's; write the
// gaussian-of-error agreement weight and its mask into float IMAGES for the device blur. The
// diffused weight decides per-pixel whether the chromaticity-continuation prior applies (gradient
// skies) or the blown object is self-coloured and keeps the solver (coloured emitters).
kernel void
hl_cgrad_gate(global const float *estimate, global const float *valid, global const float *shares,
              global const float *clip0, write_only image2d_t gate_src, write_only image2d_t gate_msk,
              const int width, const int height, const float epsilon, const float gate_tau,
              const float floor_gate)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  const int n_clip = (valid[i * 4 + 0] < 0.5f) + (valid[i * 4 + 1] < 0.5f) + (valid[i * 4 + 2] < 0.5f);
  float weight_src = 0.f, mask_src = 0.f;
  // floor-authored 1-clip pixels are not evidence (see the CPU stage): ring votes only where the
  // fit genuinely spoke
  int floor_authored = 0;
  if(n_clip == 1 && floor_gate > 1e-6f) // WB'd clips only (see the CPU stage)
  {
    const int cc = (valid[i * 4 + 0] < 0.5f) ? 0 : ((valid[i * 4 + 1] < 0.5f) ? 1 : 2);
    floor_authored = estimate[i * 4 + cc] <= 1.03f * fmax(clip0[i * 4 + cc], 1e-9f);
  }
  if(n_clip == 1 && !floor_authored)
  {
    const float lum = fmax(estimate[i * 4 + 0] + estimate[i * 4 + 1] + estimate[i * 4 + 2], epsilon);
    const float share_sum = fmax(shares[i * 4 + 0] + shares[i * 4 + 1] + shares[i * 4 + 2], epsilon);
    float err = 0.f;
    for(int c = 0; c < 3; c++)
      err += fabs(shares[i * 4 + c] / share_sum - estimate[i * 4 + c] / lum);
    const float t = err / gate_tau;
    weight_src = exp(-t * t);
    mask_src = 1.f;
  }
  write_imagef(gate_src, (int2)(x, y), (float4)(weight_src));
  write_imagef(gate_msk, (int2)(x, y), (float4)(mask_src));
}

// Reproject every multi-clip pixel's clipped subset onto the extended field, BLENDED by the
// diffused 1-clip-annulus agreement weight (the content gate), then re-assert the joint
// saturation floor. Partial pixels: the surviving channels anchor the brightness against the
// field and the clipped channels take the field's shares; all-clip pixels keep the dome-luminance
// magnitude and redistribute it by the field. 1-clip pixels are left untouched. Mirror of the CPU
// _chromaticity_gradient reprojection.
kernel void
hl_cgrad_reproject(global float *estimate, global const float *valid, global const float *clip0,
             global const float *shares, read_only image2d_t gate_wgt, read_only image2d_t gate_nrm,
             const int width, const int height, const float epsilon, const float gate_vote)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  const int clip_r = valid[i * 4 + 0] < 0.5f;
  const int clip_g = valid[i * 4 + 1] < 0.5f;
  const int clip_b = valid[i * 4 + 2] < 0.5f;
  const int n_clip = clip_r + clip_g + clip_b;
  if(n_clip < 2) return; // 1-clip: measured-correct where the fit spoke; floor-authored ones are
                         // handled by the pass-2 kernels (hl_cgrad_hole1c/hl_cgrad_write1c)

  // diffused agreement weight, shrunk toward the region-level ring vote as local evidence thins
  const float gate_lambda = 0.05f;
  const float gate_w = clamp((read_imagef(gate_wgt, sampleri, (int2)(x, y)).x + gate_lambda * gate_vote)
                                 / (read_imagef(gate_nrm, sampleri, (int2)(x, y)).x + gate_lambda),
                             0.f, 1.f);
  if(gate_w <= 1e-4f) return;

  const float share_sum = fmax(shares[i * 4 + 0] + shares[i * 4 + 1] + shares[i * 4 + 2], epsilon);
  const int anyvalid = !(clip_r && clip_g && clip_b);

  if(anyvalid)
  {
    float sv_est = 0.f, sv_share = 0.f;
    for(int c = 0; c < 3; c++)
      if(valid[i * 4 + c] >= 0.5f)
      {
        sv_est += estimate[i * 4 + c];
        sv_share += shares[i * 4 + c] / share_sum;
      }
    if(sv_share <= epsilon || sv_est <= epsilon) return;
    // survivor-anchored scale, bounded (mirror of the CPU stability cap)
    const float scale
        = fmin(sv_est / sv_share, 4.f * (estimate[i * 4 + 0] + estimate[i * 4 + 1] + estimate[i * 4 + 2]));
    for(int c = 0; c < 3; c++)
      if(valid[i * 4 + c] < 0.5f)
        estimate[i * 4 + c]
            = gate_w * (scale * (shares[i * 4 + c] / share_sum)) + (1.f - gate_w) * estimate[i * 4 + c];
  }
  else
  {
    // all-clip pixels are not reprojected (see the CPU stage: no measured anchor -> the dome keeps
    // magnitude authority; redistribution of a poor core total is unstable)
    return;
  }

  // joint saturation floor (scalar-subset lift + per-channel safety, hue preserved)
  float lift = 1.f;
  for(int c = 0; c < 3; c++)
    if(valid[i * 4 + c] < 0.5f)
    {
      const float e = fmax(estimate[i * 4 + c], 1e-6f);
      lift = fmax(lift, fmin(fmax(e, clip0[i * 4 + c]) / e, 8.f));
    }
  for(int c = 0; c < 3; c++)
    if(valid[i * 4 + c] < 0.5f)
    {
      if(lift > 1.f) estimate[i * 4 + c] = fmax(estimate[i * 4 + c], 1e-6f) * lift;
      estimate[i * 4 + c] = fmax(estimate[i * 4 + c], clip0[i * 4 + c]);
    }
}

// PASS 2 = a3 (see the CPU stage): build one clipped channel's floor-authored hole (1-clip,
// this channel clipped, estimate at/below its own saturation floor) and seed the fill plane
// with the channel's current values everywhere -- the biharmonic dome then extends the value
// across the hole from BOTH sides (multi-clip reconstruction inside, measured data outside).
kernel void
hl_cgrad_hole1c(global const float *estimate, global const float *valid, global const float *clip0,
                global uchar *hole, global float *field, const int width, const int height, const int c)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  const int clip_r = valid[i * 4 + 0] < 0.5f;
  const int clip_g = valid[i * 4 + 1] < 0.5f;
  const int clip_b = valid[i * 4 + 2] < 0.5f;
  const int cc = clip_r ? 0 : (clip_g ? 1 : 2);
  const int is_hole = (clip_r + clip_g + clip_b == 1) && (cc == c)
                      && (estimate[i * 4 + c] <= 1.03f * fmax(clip0[i * 4 + c], 1e-9f));
  hole[i] = is_hole;
  field[i] = estimate[i * 4 + c];
}

// PASS 2 = a3 write-back: the filled value replaces the floor-authored pixel's clipped channel,
// floored at saturation (the fill approaches clip0 at the outer contour by construction).
kernel void
hl_cgrad_write1c(global float *estimate, global const uchar *hole, global const float *field,
                 global const float *clip0, const int width, const int height, const int c)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  if(!hole[i]) return;
  estimate[i * 4 + c] = fmax(field[i], clip0[i * 4 + c]);
}

// ===== knee (rolloff pre-correction) estimation ============================================
// Cameras roll off smoothly just below saturation instead of clipping abruptly; the knee
// stage measures that rolloff from the image itself and lifts the affected band back, per
// channel. It bins the CFA (colour filter array: the sensor mosaic where each pixel measures
// only red, green or blue; Bayer = 2x2 repeating pattern, Fujifilm X-Trans = 6x6) into
// coarse per-channel planes, predicts what each suspect cell SHOULD read from the still-
// trusted channels via colour-line regressions, and the host turns the prediction gaps into
// per-channel piecewise-linear lift curves. Mirrors the CPU _hl_knee_estimate /
// _hl_knee_apply_cfa -- any change here must be mirrored there and re-validated with the
// HL_KNEECL_TEST self-test.

// Bin the raw CFA mosaic into three coarse planes (one per channel): each qs x qs cell
// averages the sensor pixels of that colour inside it (mosaic colour decoded by FC /
// FCxtrans with the region offset rx/ry), then divides by that channel's raw clip level so
// all planes live in 0..1 "fraction of clip" units. Cells with no pixel of a colour write 0.
kernel void
hl_knee_bin(global const float *input, global float *binned_planes,
            const int width, const int height, const int bin_w, const int bin_h, const int cell_size,
            const unsigned int filters, const int region_x, const int region_y, const int is_xtrans,
            global const unsigned char (*const xtrans)[6], const float4 clipraw)
{
  const int j = get_global_id(0);
  const int i = get_global_id(1);
  if(j >= bin_w || i >= bin_h) return;
  const int bin_pixels = bin_w * bin_h;
  float accum[3] = { 0.f, 0.f, 0.f };
  float count[3] = { 0.f, 0.f, 0.f };
  const float clip_rgb[3] = { clipraw.x, clipraw.y, clipraw.z };
  for(int y = 0; y < cell_size; y++)
    for(int sample_x = 0; sample_x < cell_size; sample_x++)
    {
      const int row = i * cell_size + y;
      const int col = j * cell_size + sample_x;
      const int c = is_xtrans ? FCxtrans(region_y + row, region_x + col, xtrans) : FC(region_y + row, region_x + col, filters);
      if(c <= 2)
      {
        accum[c] += input[row * width + col];
        count[c] += 1.f;
      }
    }
  for(int c = 0; c < 3; c++)
    binned_planes[c * bin_pixels + i * bin_w + j] = (count[c] > 0.f) ? accum[c] / (count[c] * clip_rgb[c]) : 0.f;
}

// Joint windowed-moment images over the binned planes (x0/x1/x2 = the three channel planes),
// packed for the 4-channel blur. Weight w = 1 only where all three channels sit below the
// trusted level lo, i.e. well under the rolloff band. A = (w, w*x0, w*x1, w*x2),
// B = (w*x0*x0, w*x0*x1, w*x0*x2, w*x1*x1), C = (w*x1*x2, w*x2*x2, 0, 0); once blurred these
// give the local means and channel products the regressions read.
kernel void
hl_knee_jmom(global const float *binned_planes, write_only image2d_t moments_a, write_only image2d_t moments_b,
             write_only image2d_t moments_c, const int bin_w, const int bin_h, const float knee_lo)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= bin_w || y >= bin_h) return;
  const int pixel = y * bin_w + x;
  const int bin_pixels = bin_w * bin_h;
  const float plane0 = binned_planes[pixel];
  const float plane1 = binned_planes[bin_pixels + pixel];
  const float plane2 = binned_planes[2 * bin_pixels + pixel];
  const float weight = (plane0 < knee_lo && plane1 < knee_lo && plane2 < knee_lo) ? 1.f : 0.f;
  write_imagef(moments_a, (int2)(x, y), (float4)(weight, weight * plane0, weight * plane1, weight * plane2));
  write_imagef(moments_b, (int2)(x, y), (float4)(weight * plane0 * plane0, weight * plane0 * plane1, weight * plane0 * plane2, weight * plane1 * plane1));
  write_imagef(moments_c, (int2)(x, y), (float4)(weight * plane1 * plane2, weight * plane2 * plane2, 0.f, 0.f));
}

// Pair windowed-moment images for binned channels a and b, weight w = 1 only where both sit
// below the trusted level lo. A = (w, w*a, w*b, w*(a*a)), B = (w*(b*b), w*(a*b), 0, 0).
kernel void
hl_knee_pmom(global const float *binned_planes, write_only image2d_t moments_a, write_only image2d_t moments_b,
             const int bin_w, const int bin_h, const int chan_a, const int chan_b, const float knee_lo)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= bin_w || y >= bin_h) return;
  const int pixel = y * bin_w + x;
  const int bin_pixels = bin_w * bin_h;
  const float val_a = binned_planes[chan_a * bin_pixels + pixel];
  const float val_b = binned_planes[chan_b * bin_pixels + pixel];
  const float weight = (val_a < knee_lo && val_b < knee_lo) ? 1.f : 0.f;
  write_imagef(moments_a, (int2)(x, y), (float4)(weight, weight * val_a, weight * val_b, weight * (val_a * val_a)));
  write_imagef(moments_b, (int2)(x, y), (float4)(weight * (val_b * val_b), weight * (val_a * val_b), 0.f, 0.f));
}

// Two-guide colour-line prediction of the rolloff band, one work-item per coarse cell. Only
// cells whose channel c sits in the suspect band (>= lo, below the hard-clip detection level
// det_) while BOTH guide channels are still trusted (< lo) participate. Solves the same
// stabilised 2x2 system as hl_cf_fit_joint from the blurred moments and writes the predicted
// "true" value pred, the fit quality r2s, and a done flag: the host re-launches with wider
// blur windows, and done stops a coarser window from overwriting a cell already predicted
// at a finer one.
kernel void
hl_knee_joint_reg(global const float *binned_planes, read_only image2d_t moments_a_img, read_only image2d_t moments_b_img,
                  read_only image2d_t moments_c_img, global float *pred, global float *r2_scores,
                  global uchar *done, const int bin_w, const int bin_h,
                  const int c, const int guide1, const int guide2,
                  const float knee_lo, const float knee_det, const float min_mass)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= bin_w || y >= bin_h) return;
  const int pixel = y * bin_w + x;
  const int bin_pixels = bin_w * bin_h;
  if(done[c * bin_pixels + pixel]) return;

  const float target_val = binned_planes[c * bin_pixels + pixel];
  const float guide1_val = binned_planes[guide1 * bin_pixels + pixel];
  const float guide2_val = binned_planes[guide2 * bin_pixels + pixel];
  if(!(target_val >= knee_lo && target_val < knee_det)) return;
  if(!(guide1_val < knee_lo && guide2_val < knee_lo)) return;

  const float4 moments_a = read_imagef(moments_a_img, samplerA, (int2)(x, y));
  const float weight_sum = moments_a.x;
  if(weight_sum <= min_mass) return;

  const float4 moments_b = read_imagef(moments_b_img, samplerA, (int2)(x, y));
  const float4 moments_c = read_imagef(moments_c_img, samplerA, (int2)(x, y));
  const float first_moments[4] = { moments_a.x, moments_a.y, moments_a.z, moments_a.w };
  const float second_moments[6] = { moments_b.x, moments_b.y, moments_b.z, moments_b.w, moments_c.x, moments_c.y };
  const int index_lut[3][3] = { { 0, 1, 2 }, { 1, 3, 4 }, { 2, 4, 5 } };

  const float inv_weight = 1.f / weight_sum;                     // 1/n, converts summed moments to E[.]
  const float mean_target = first_moments[1 + c] * inv_weight;   // E[v]
  const float mean_guide1 = first_moments[1 + guide1] * inv_weight; // E[u1]
  const float mean_guide2 = first_moments[1 + guide2] * inv_weight; // E[u2]
  // second moments de-meaned = Var/Cov (centered about the window mean to avoid float E[u^2]-E[u]^2 cancellation)
  const float var_11 = fmax(second_moments[index_lut[guide1][guide1]] * inv_weight - mean_guide1 * mean_guide1, 0.f); // Var(u1)
  const float var_22 = fmax(second_moments[index_lut[guide2][guide2]] * inv_weight - mean_guide2 * mean_guide2, 0.f); // Var(u2)
  const float var_12 = second_moments[index_lut[guide1][guide2]] * inv_weight - mean_guide1 * mean_guide2; // Cov(u1,u2) off-diag
  const float cov_1 = second_moments[index_lut[c][guide1]] * inv_weight - mean_target * mean_guide1; // Cov(v,u1) RHS
  const float cov_2 = second_moments[index_lut[c][guide2]] * inv_weight - mean_target * mean_guide2; // Cov(v,u2) RHS
  const float var_target = fmax(second_moments[index_lut[c][c]] * inv_weight - mean_target * mean_target, 0.f); // Var(v) for R^2

  const float lambda = 1e-3f * 0.5f * (var_11 + var_22) + 1e-12f; // relative Tikhonov ridge = 1e-3*(Var u1+Var u2)/2
  const float diag_11 = var_11 + lambda;                          // ridged normal-matrix diagonal [0][0]
  const float diag_22 = var_22 + lambda;                          // ridged normal-matrix diagonal [1][1]
  const float determinant = fmax(diag_11 * diag_22 - var_12 * var_12, 1e-18f); // det of the 2x2 system
  const float slope_1 = (diag_22 * cov_1 - var_12 * cov_2) / determinant; // a = slope on u1 (Cramer's rule)
  const float slope_2 = (diag_11 * cov_2 - var_12 * cov_1) / determinant; // b = slope on u2 (Cramer's rule)

  // v_hat = E[v] + a*(u1 - E[u1]) + b*(u2 - E[u2])  (the colour-line prediction; intercept d folded in)
  pred[c * bin_pixels + pixel] = mean_target + slope_1 * (guide1_val - mean_guide1) + slope_2 * (guide2_val - mean_guide2);
  // R^2 = (a*Cov(v,u1) + b*Cov(v,u2)) / Var(v): explained-variance fraction (fit quality for the vote gate)
  r2_scores[c * bin_pixels + pixel] = clamp((slope_1 * cov_1 + slope_2 * cov_2) / (var_target + 1e-12f), 0.f, 1.f);
  done[c * bin_pixels + pixel] = 1;                               // cell served; coarser sigma passes skip it
}

// Single-guide fallback prediction for cells the joint regression could not serve (only one
// trusted guide left): slope = local covariance / guide variance from the blurred pair
// moments, prediction = target mean + slope x (guide - guide mean); ta = 1 when the target
// channel is the pair's `a` component. Same band gating and done-flag protocol as
// hl_knee_joint_reg.
kernel void
hl_knee_pair_reg(global const float *binned_planes, read_only image2d_t moments_a_img, read_only image2d_t moments_b_img,
                 global float *pred, global float *r2_scores, global uchar *done,
                 const int bin_w, const int bin_h, const int target_channel, const int guide_channel, const int target_is_a,
                 const float knee_lo, const float knee_det, const float min_mass)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= bin_w || y >= bin_h) return;
  const int pixel = y * bin_w + x;
  const int bin_pixels = bin_w * bin_h;
  if(done[target_channel * bin_pixels + pixel]) return;

  const float target_val = binned_planes[target_channel * bin_pixels + pixel];
  const float guide_val = binned_planes[guide_channel * bin_pixels + pixel];
  if(!(target_val >= knee_lo && target_val < knee_det)) return;
  if(!(guide_val < knee_lo)) return;

  const float4 moments_a = read_imagef(moments_a_img, samplerA, (int2)(x, y));
  const float weight_sum = moments_a.x;
  if(weight_sum <= min_mass) return;
  const float4 moments_b = read_imagef(moments_b_img, samplerA, (int2)(x, y));

  const float inv_weight = 1.f / weight_sum;                                        // 1/n
  const float mean_target = (target_is_a ? moments_a.y : moments_a.z) * inv_weight; // E[v]
  const float mean_guide = (target_is_a ? moments_a.z : moments_a.y) * inv_weight;  // E[u]
  const float covariance = moments_b.y * inv_weight - mean_target * mean_guide;     // Cov(v,u) (moments_b.y = sum w*a*b)
  const float var_guide = fmax((target_is_a ? moments_b.x : moments_a.w) * inv_weight - mean_guide * mean_guide, 0.f);  // Var(u)
  const float var_target = fmax((target_is_a ? moments_a.w : moments_b.x) * inv_weight - mean_target * mean_target, 0.f); // Var(v)
  const float slope = covariance / (var_guide * (1.f + 1e-3f) + 1e-12f);            // a = Cov(v,u)/Var(u), ridged

  pred[target_channel * bin_pixels + pixel] = mean_target + slope * (guide_val - mean_guide); // v_hat = E[v] + a*(u-E[u])
  r2_scores[target_channel * bin_pixels + pixel] = clamp(covariance * covariance / (var_guide * var_target + 1e-18f), 0.f, 1.f); // R^2 = Cov^2/(Var(u)Var(v))
  done[target_channel * bin_pixels + pixel] = 1;
}

// Apply the fitted lift curves to the raw CFA mosaic. For each sensor pixel of an engaged
// channel whose clip-normalised value falls in the rolloff band (>= lo, < det_): look up the
// lift in that channel's piecewise-linear curve (`bins` samples, linear interpolation
// between them, ramped in from zero over the first half-bin so the curve starts smoothly)
// and add it before scaling back to raw units. Everything else is copied through unchanged.
// Mirrors the CPU _hl_knee_apply_cfa.
kernel void
hl_knee_apply(global const float *input, global float *output,
              const int width, const int height,
              const unsigned int filters, const int region_x, const int region_y, const int is_xtrans,
              global const unsigned char (*const xtrans)[6],
              const float4 clipraw, global const float *lift, const int4 engaged,
              const float knee_lo, const float knee_det, const int bins)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int idx = y * width + x;
  const int c = is_xtrans ? FCxtrans(region_y + y, region_x + x, xtrans) : FC(region_y + y, region_x + x, filters);
  float raw_value = input[idx];
  const float clip_rgb[3] = { clipraw.x, clipraw.y, clipraw.z };
  const int engaged_flags[3] = { engaged.x, engaged.y, engaged.z };

  if(c <= 2 && engaged_flags[c]) // only engaged channels' band pixels are corrected
  {
    const float norm_val = raw_value / clip_rgb[c]; // v in clip units
    if(norm_val >= knee_lo && norm_val < knee_det)  // only band values [LO, DET)
    {
      // L(v) lookup, mirroring the CPU _knee_lift_of: piecewise-linear over bin-center knots,
      // ramped in over the first half-bin, flat-extended past the last center
      const float step = (knee_det - knee_lo) / (float)bins;              // bin width
      const float bin_pos = (norm_val - (knee_lo + 0.5f * step)) / step;  // v in bin-center units
      global const float *lift_curve = lift + c * bins;                   // this channel's knot array
      float lift_value;
      if(bin_pos <= -0.5f) lift_value = 0.f;                              // at/below LO: no lift
      else if(bin_pos <= 0.f) lift_value = lift_curve[0] * 2.f * (bin_pos + 0.5f); // first half-bin ramp 0 -> lift[0]
      else if(bin_pos >= (float)(bins - 1)) lift_value = lift_curve[bins - 1];     // past last center: flat-extend
      else
      {
        const int i = (int)bin_pos;                                       // lower knot
        const float frac = bin_pos - (float)i;                           // blend weight
        lift_value = lift_curve[i] * (1.f - frac) + lift_curve[i + 1] * frac; // linear blend
      }
      raw_value = (norm_val + lift_value) * clip_rgb[c]; // k^-1(v) = v + L(v), back to raw units
    }
  }
  output[idx] = raw_value; // out-of-band / clipped / non-engaged pixels pass through unchanged
}


// ===== full-image glue for the no-roundtrip orchestrator ===================================
// These kernels keep the whole reconstruction on the GPU (no image roundtrip to the CPU):
// pack masks for the host's segmentation, gather each region (a rectangular working window
// around one connected blob of clipped pixels; reconstruction runs independently per region)
// into contiguous stage buffers, and scatter the result back. Driven by
// _region_guided_filter_cl and validated against the CPU _region_guided_filter with the
// HL_REGCL_TEST self-test.

// Quantize channel 3 of the feathered clipping mask into two byte masks the host downloads:
// seed (> 0.5) feeds the EDT (distance transform: for each pixel, the distance to the
// nearest unclipped pixel -- how deep inside the blown zone it sits), and member (>= 1e-3)
// feeds the flood-fill that groups clipped pixels into connected regions.
kernel void
hl_mask_pack(global const float *mask, global uchar *seed, global uchar *member,
             const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  const float mask_value = mask[i * 4 + 3];
  seed[i] = (mask_value > 0.5f);
  member[i] = (mask_value >= 1e-3f);
}

// Gather one padded region window out of the full-image buffers into contiguous per-region
// stage buffers: est (working estimates) and clip0 (the clip floor) both start as copies of
// the interpolated input; vld = 1 - clipping mask (validity: below 0.5 = clipped, must be
// reconstructed); bsc (fit quality) starts at 0; dep = distance-transform depth; lsbp = the
// frozen pre-ladder luminance proxy (sum of the three channels, captured here ONCE so later
// weighting decisions do not shift as est gets rewritten).
kernel void
hl_region_gather(global const float *interp, global const float *mask, global const float *depth,
                 global float *estimate, global float *clip0, global float *valid, global float *clip_depth,
                 global float *model_quality, global float *luminance_plane,
                 const int width, const int region_x0, const int region_y0, const int region_w, const int region_h)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= region_w || y >= region_h) return;
  const int src_pixel = (region_y0 + y) * width + (region_x0 + x);
  const int src_index = src_pixel * 4;
  const int dst_index = (y * region_w + x) * 4;
  clip_depth[y * region_w + x] = depth[src_pixel];
  for(int k = 0; k < 4; k++)
  {
    const float value = interp[src_index + k];
    estimate[dst_index + k] = value;
    clip0[dst_index + k] = value;
    valid[dst_index + k] = fmax(1.f - mask[src_index + k], 0.f);
    model_quality[dst_index + k] = 0.f;
  }
  luminance_plane[y * region_w + x] = interp[src_index + 0] + interp[src_index + 1] + interp[src_index + 2];
}

// Scatter the region result back: for every pixel of the region window, overwrite the
// full-image channels whose clipping mask is set (> 0.5) with the reconstructed estimate,
// clamped non-negative. Unclipped channels keep their measured values.
kernel void
hl_region_scatter(global float *interp, global const float *mask, global const float *estimate,
                  const int width, const int region_x0, const int region_y0, const int region_w, const int region_h)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= region_w || y >= region_h) return;
  const int src_index = (y * region_w + x) * 4;
  const int dst_index = ((region_y0 + y) * width + (region_x0 + x)) * 4;
  for(int c = 0; c < 3; c++)
    if(mask[dst_index + c] > 0.5f)
      interp[dst_index + c] = fmax(estimate[src_index + c], 0.f);
}


// Pre-ladder region statistics, a reduction with two float4 partial sums per work-group
// (small array finished on the CPU): p0.x / p0.y = luminance sum and pixel count over
// pixels where any channel is clipped (feeds the soft-weight reference), and
// p0.z / p0.w / p1.x = the per-channel clipped-pixel counts, which elect the "deep" channel
// (the most-clipped one) for the pair/deep cascade.
kernel void
hl_region_stats(global const float *estimate, global const float *valid,
                global float4 *partial, const int n_pixels, local float4 *scratch)
{
  const int global_id = get_global_id(0);
  const int global_size = get_global_size(0);
  const int local_id = get_local_id(0);
  const int local_size = get_local_size(0);

  float4 partial0 = (float4)(0.f);
  float4 partial1 = (float4)(0.f);
  for(int i = global_id; i < n_pixels; i += global_size)
  {
    const int clipped_r = (valid[i * 4 + 0] < 0.5f);
    const int clipped_g = (valid[i * 4 + 1] < 0.5f);
    const int clipped_b = (valid[i * 4 + 2] < 0.5f);
    if(clipped_r || clipped_g || clipped_b)
    {
      partial0.x += estimate[i * 4 + 0] + estimate[i * 4 + 1] + estimate[i * 4 + 2];
      partial0.y += 1.f;
    }
    partial0.z += clipped_r;
    partial0.w += clipped_g;
    partial1.x += clipped_b;
    // per-channel VALID-value sums: the moment packs are centered on these means
    partial1.y += clipped_r ? 0.f : estimate[i * 4 + 0];
    partial1.z += clipped_g ? 0.f : estimate[i * 4 + 1];
    partial1.w += clipped_b ? 0.f : estimate[i * 4 + 2];
  }
  scratch[2 * local_id] = partial0;
  scratch[2 * local_id + 1] = partial1;
  barrier(CLK_LOCAL_MEM_FENCE);
  for(int offset = local_size / 2; offset > 0; offset /= 2)
  {
    if(local_id < offset)
    {
      scratch[2 * local_id] += scratch[2 * (local_id + offset)];
      scratch[2 * local_id + 1] += scratch[2 * (local_id + offset) + 1];
    }
    barrier(CLK_LOCAL_MEM_FENCE);
  }
  if(local_id == 0)
  {
    partial[2 * get_group_id(0)] = scratch[0];
    partial[2 * get_group_id(0) + 1] = scratch[1];
  }
}

// Self-dome necessity gate, a max-reduction (host finishes the per-group results): flags 1
// if ANY clipped channel with at least one surviving guide would keep less than 90% of its
// own estimate in the depth-gated dome blend -- meaning its fit quality bsc is weak AND it
// sits deep inside the blown zone. The keep weight varc^2 is computed EXACTLY as
// hl_dome_blend computes it, so the gate and the blend always agree; if nothing flags, the
// host skips the whole self-dome stage.
kernel void
hl_need_self(global const float *valid, global const float *model_quality, global const float *clip_depth,
             global float *partial, const int n_pixels, const float cf_sigma, local float *scratch)
{
  const int global_id = get_global_id(0);
  const int global_size = get_global_size(0);
  const int local_id = get_local_id(0);
  const int local_size = get_local_size(0);

  float need_dome = 0.f;
  for(int i = global_id; i < n_pixels; i += global_size)
  {
    const int anyvalid = (valid[i * 4 + 0] >= 0.5f) || (valid[i * 4 + 1] >= 0.5f) || (valid[i * 4 + 2] >= 0.5f);
    if(!anyvalid) continue;
    for(int c = 0; c < 3; c++)
      if(valid[i * 4 + c] < 0.5f)
      {
        const float quality_ramp = clamp((model_quality[i * 4 + c] - 0.4f) / 0.45f, 0.f, 1.f);
        const float weight_quality = quality_ramp * quality_ramp * (3.f - 2.f * quality_ramp);
        const float depth_norm = clip_depth[i] / (1.5f * cf_sigma);
        const float depth_gauss = exp(-depth_norm * depth_norm);
        const float keep_weight = clamp(1.f - (1.f - weight_quality) * depth_gauss, 0.f, 1.f);
        if(keep_weight < 0.9f) need_dome = 1.f;
      }
  }
  scratch[local_id] = need_dome;
  barrier(CLK_LOCAL_MEM_FENCE);
  for(int offset = local_size / 2; offset > 0; offset /= 2)
  {
    if(local_id < offset) scratch[local_id] = fmax(scratch[local_id], scratch[local_id + offset]);
    barrier(CLK_LOCAL_MEM_FENCE);
  }
  if(local_id == 0) partial[get_group_id(0)] = scratch[0];
}


// Same lift-curve lookup as hl_knee_apply, but on the demosaiced 4-float pixels (red, green,
// blue, norm) instead of the raw mosaic: each engaged channel whose clip-normalised value
// falls in the rolloff band gets its interpolated lift added. If any channel moved, the norm
// channel (index 3) is rebuilt as the length of the white-balance-weighted RGB vector.
// Mirrors the CPU _hl_knee_apply_interpolated -- any change here must be mirrored there and
// re-validated with the HL_KNEECL_TEST self-test.
kernel void
hl_knee_apply_interp(global float *interp, const int width, const int height,
                     const float4 clipvaln, const float4 wb4,
                     global const float *lift, const int4 engaged,
                     const float knee_lo, const float knee_det, const int bins)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int pixel = y * width + x;
  const float clip_rgb[3] = { clipvaln.x, clipvaln.y, clipvaln.z };
  const float wb_rgb[3] = { wb4.x, wb4.y, wb4.z };
  const int engaged_flags[3] = { engaged.x, engaged.y, engaged.z };
  const float step = (knee_det - knee_lo) / (float)bins;
  int touched = 0;

  for(int c = 0; c < 3; c++)
  {
    if(!engaged_flags[c]) continue;                              // channels with no rolloff pass through
    const float norm_val = interp[pixel * 4 + c] / clip_rgb[c];  // v in clip units
    if(norm_val >= knee_lo && norm_val < knee_det)               // only band values
    {
      // L(v) lookup identical to hl_knee_apply / the CPU _knee_lift_of
      const float bin_pos = (norm_val - (knee_lo + 0.5f * step)) / step;
      global const float *lift_curve = lift + c * bins;
      float lift_value;
      if(bin_pos <= -0.5f) lift_value = 0.f;
      else if(bin_pos <= 0.f) lift_value = lift_curve[0] * 2.f * (bin_pos + 0.5f);
      else if(bin_pos >= (float)(bins - 1)) lift_value = lift_curve[bins - 1];
      else
      {
        const int i = (int)bin_pos;
        const float frac = bin_pos - (float)i;
        lift_value = lift_curve[i] * (1.f - frac) + lift_curve[i + 1] * frac;
      }
      if(lift_value > 0.f)
      {
        interp[pixel * 4 + c] = (norm_val + lift_value) * clip_rgb[c]; // k^-1(v) = v + L(v)
        touched = 1;
      }
    }
  }
  if(touched)
  {
    const float val_r = interp[pixel * 4 + 0] * wb_rgb[0];
    const float val_g = interp[pixel * 4 + 1] * wb_rgb[1];
    const float val_b = interp[pixel * 4 + 2] * wb_rgb[2];
    interp[pixel * 4 + 3] = sqrt(val_r * val_r + val_g * val_g + val_b * val_b);
  }
}


// ===== matrix-free CG for the screened-harmonic solve (large all-clip cores) ===============
// CG = conjugate gradient, the iterative solver used instead of the direct sparse Cholesky
// factorization when the all-clip core is too large; it repeats matrix-vector products until
// the error is small. "Matrix-free" = the matrix is never stored: its effect is recomputed
// each iteration by the Laplacian kernel below. The fused vector-update + dot-product
// reduction kernels live in highlights_sparse.cl (hl_cg_r1 / hl_cg_ap / hl_cg_update).
// Mirrors the CG fallback of the CPU _region_pde_solve, driven by _region_pde_cg_cl -- any
// change here must be mirrored there and re-validated with the HL_CORECL_TEST self-test.

// Boundary embedding: split a plane into its known and unknown parts before applying the
// operator. keep_hole=0 keeps the known rim values and zeroes the hole (t1 = hole ? 0 : x);
// keep_hole=1 keeps the hole values and zeroes the rim (t1 = hole ? x : 0).
kernel void
hl_cg_embed(global const float *plane, global const uchar *hole, global float *embedded_plane,
            const int width, const int height, const int keep_hole)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  const int is_hole = (hole[i] != 0);
  embedded_plane[i] = (is_hole == keep_hole) ? plane[i] : 0.f;
}

// The operator's spatial part: the negated 9-point Laplacian -- a weighted average of the 8
// neighbours (axis neighbours weight 4, diagonals weight 1, total divided by 6) minus the
// centre value, edge-clamped, then sign-flipped. Matches the CPU _apply_op at order 1.
kernel void
hl_cg_op(global const float *plane_in, global float *laplacian_out, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int y_north = (y > 0) ? y - 1 : y;
  const int y_south = (y < height - 1) ? y + 1 : y;
  const int x_west = (x > 0) ? x - 1 : x;
  const int x_east = (x < width - 1) ? x + 1 : x;
  const float center = plane_in[y * width + x];
  const float north = plane_in[y_north * width + x];
  const float south = plane_in[y_south * width + x];
  const float west = plane_in[y * width + x_west];
  const float east = plane_in[y * width + x_east];
  const float val_nw = plane_in[y_north * width + x_west];
  const float val_ne = plane_in[y_north * width + x_east];
  const float val_sw = plane_in[y_south * width + x_west];
  const float val_se = plane_in[y_south * width + x_east];
  laplacian_out[y * width + x] = -((4.f * (north + south + west + east) + (val_nw + val_ne + val_sw + val_se) - 20.f * center) / 6.f);
}

// Initial residual = the system's right-hand side on hole pixels: dscalar x tscalar (the
// constant flat-colour reaction pulling toward the mean chroma, matching the CPU's constant
// planes) minus the operator applied to the known rim values (t2, produced by hl_cg_embed
// followed by hl_cg_op). Zero outside the hole.
kernel void
hl_cg_r0(global float *residual, global const float *laplacian_term, global const uchar *hole,
         const int width, const int height, const float dscalar, const float tscalar)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  residual[i] = hole[i] ? (dscalar * tscalar - laplacian_term[i]) : 0.f;
}

// New CG search direction: p = residual + beta x previous direction, on hole pixels only
// (beta is computed on the host from the partial dot-product sums).
kernel void
hl_cg_beta(global float *search_dir, global const float *residual, global const uchar *hole,
           const int width, const int height, const float beta)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  if(hole[i]) search_dir[i] = residual[i] + beta * search_dir[i];
}

// Copy with a clamp at zero (the diffused chroma ratios are stored non-negative).
kernel void
hl_relu(global const float *input, global float *output, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  output[i] = fmax(input[i], 0.f);
}


// ===== aniso explicit pyramid (large all-clip cores, DT_HL_SPARSE_MAX exceeded) =============
// When the all-clip core has more unknowns than the sparse direct solver allows
// (DT_HL_SPARSE_MAX), the edge-aware chroma diffusion falls back to explicit iteration on a
// pyramid: downsample the ratios to coarse grids, run many small diffusion steps steered by
// the structure tensor at each level, and splat the coarse solution back into the fine
// clipped pixels. Mirrors the CPU _aniso_iterate pyramid path, driven by _aniso_pyramid_cl
// -- any change here must be mirrored there and re-validated with the HL_ANISOCL_TEST
// self-test.
//
// MATHS BRIDGE -- Step 8 explicit trace-form pyramid (article §"The update rules"): the multiscale
// solver for min int grad(r)^T D grad(r) s.t. r >= c0/L. Per level: project onto the obstacle then
// run 240 steps of r <- max(r + 0.18*(D_xx d_xx r + 2 D_xy d_xy r + D_yy d_yy r), c0/L); coarsest
// level first so the whole hole is seeded before refinement (bilinear prolongation between levels).

// Box-downsample one pyramid level: coarse luminance dL = block mean of lsb, coarse ratios
// dr = block means of the three chroma ratios (packed 3 floats per pixel), and coarse
// per-channel hole flag dhole = 1 where clipped pixels (vldan < 0.5) are the majority of
// the block.
kernel void
hl_aniso_pyr_down(global const float *luminance, global const float *chroma, global const float *valid_anchor,
                  global const float *clip0,
                  global float *coarse_luminance, global float *coarse_ratio, global float *coarse_obstacle, global uchar *coarse_hole,
                  const int width, const int height, const int coarse_w, const int coarse_h, const int step)
{
  const int coarse_x = get_global_id(0);
  const int coarse_y = get_global_id(1);
  if(coarse_x >= coarse_w || coarse_y >= coarse_h) return;
  float accum_lum = 0.f;
  float accum_ratio[3] = { 0.f, 0.f, 0.f };
  float accum_clip[3] = { 0.f, 0.f, 0.f };
  int n_hole[3] = { 0, 0, 0 };
  int n_total = 0;
  for(int sample_y = coarse_y * step; sample_y < min((coarse_y + 1) * step, height); sample_y++)
    for(int sample_x = coarse_x * step; sample_x < min((coarse_x + 1) * step, width); sample_x++)
    {
      const int fine_index = sample_y * width + sample_x;
      accum_lum += luminance[fine_index];
      n_total++;
      for(int c = 0; c < 3; c++)
      {
        accum_ratio[c] += chroma[fine_index * 4 + c];
        accum_clip[c] += clip0[fine_index * 4 + c];
        n_hole[c] += (valid_anchor[fine_index * 4 + c] < 0.5f);
      }
    }
  const int coarse_index = coarse_y * coarse_w + coarse_x;
  coarse_luminance[coarse_index] = accum_lum / n_total;
  for(int c = 0; c < 3; c++)
  {
    coarse_ratio[coarse_index * 3 + c] = accum_ratio[c] / n_total;
    // per-cell obstacle: the saturation floor in ratio space, clip0_c / L
    coarse_obstacle[coarse_index * 3 + c] = accum_clip[c] / fmax(accum_lum, 1e-9f);
    coarse_hole[coarse_index * 3 + c] = (2 * n_hole[c] > n_total) ? 1 : 0;
  }
}

// Project one channel's working plane onto its obstacle over the hole (seed projection of
// the obstacle relaxation, mirrors the entry clamp of the CPU _aniso_iterate_obs).
kernel void
hl_pyr_project(global float *solution, global const float *coarse_obstacle, global const uchar *coarse_hole,
               const int level_w, const int level_h, const int c)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= level_w || y >= level_h) return;
  const int i = y * level_w + x;
  // r <- max(r, obstacle), obstacle = c0/L in ratio space (saturation floor of the obstacle problem)
  if(coarse_hole[i * 3 + c]) solution[i] = fmax(solution[i], coarse_obstacle[i * 3 + c]);
}

// Full-resolution support for the projected polish (both solver paths): build the 3-packed
// all-clip hole + ratio-space obstacle from the interleaved region planes, and move one
// channel between the rn*4 ratio plane s1 and a single-channel working plane.
kernel void
hl_aniso_obs_full(global const float *valid_anchor, global const float *clip0, global const float *luminance,
                  global float *coarse_obstacle, global uchar *coarse_hole, const int width, const int height,
                  const float epsilon)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  const int all_clipped = (valid_anchor[i * 4 + 0] < 0.5f && valid_anchor[i * 4 + 1] < 0.5f && valid_anchor[i * 4 + 2] < 0.5f);
  for(int c = 0; c < 3; c++)
  {
    coarse_obstacle[i * 3 + c] = clip0[i * 4 + c] / fmax(luminance[i], epsilon); // obstacle = c0/L_sum (ratio-space floor)
    coarse_hole[i * 3 + c] = all_clipped ? 1 : 0;
  }
}

// Pack one region window (interp RGBA + mask RGBA + depth) into a contiguous staging buffer
// so the whole window crosses the bus in ONE readback: small regions are reconstructed on the
// CPU (the ~1000 tiny kernel launches a GPU region costs dwarf the arithmetic), and the only
// device work left is this gather and the mirror scatter below.
kernel void
hl_window_pack(global const float *interp, global const float *mask, global const float *depth,
               global float *staging, const int width, const int region_x0, const int region_y0,
               const int region_w, const int region_h)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= region_w || y >= region_h) return;
  const size_t i = (size_t)y * region_w + x;
  const size_t pixel = (size_t)(region_y0 + y) * width + (region_x0 + x);
  const size_t region_pixels = (size_t)region_w * region_h;
  for(int c = 0; c < 4; c++)
  {
    staging[i * 4 + c] = interp[pixel * 4 + c];
    staging[region_pixels * 4 + i * 4 + c] = mask[pixel * 4 + c];
  }
  staging[region_pixels * 8 + i] = depth[pixel];
}

// Mirror scatter: write the CPU-reconstructed window back into the full-res interp buffer.
kernel void
hl_window_unpack(global const float *staging, global float *interp, const int width,
                 const int region_x0, const int region_y0, const int region_w, const int region_h)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= region_w || y >= region_h) return;
  const size_t i = (size_t)y * region_w + x;
  const size_t pixel = (size_t)(region_y0 + y) * width + (region_x0 + x);
  for(int c = 0; c < 4; c++)
    interp[pixel * 4 + c] = staging[i * 4 + c];
}

// Per-channel activity flags for the polish gate: flags[c] = 1 iff some all-clip pixel sits
// at (or below) its obstacle -- only then can the projection fire during the polish sweeps.
kernel void
hl_aniso_obs_flags(global const float *chroma, global const float *coarse_obstacle, global const uchar *coarse_hole,
                   global int *flags, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  if(!coarse_hole[i * 3]) return;
  // active set of the obstacle problem: some pixel sits at (or below) its floor r_c <= c0/L,
  // so the projection can still fire -- only then are the 60 polish sweeps worth running
  for(int c = 0; c < 3; c++)
    if(chroma[i * 4 + c] <= coarse_obstacle[i * 3 + c] * 1.001f) atomic_or(&flags[c], 1);
}

kernel void
hl_pyr_getc4(global const float *chroma, global float *solution, const int width, const int height, const int c)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  solution[i] = chroma[i * 4 + c];
}

kernel void
hl_pyr_putc4(global const float *solution, global float *chroma, const int width, const int height, const int c)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int i = y * width + x;
  chroma[i * 4 + c] = solution[i];
}

// Extract (hl_pyr_getc) or store back (hl_pyr_putc) one ratio channel between the packed
// 3-per-pixel coarse plane dr and a single-channel working plane u, so the diffusion kernels
// below can ping-pong one channel at a time.
kernel void
hl_pyr_getc(global const float *coarse_ratio, global float *solution, const int level_w, const int level_h, const int c)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= level_w || y >= level_h) return;
  const int i = y * level_w + x;
  solution[i] = coarse_ratio[i * 3 + c];
}

kernel void
hl_pyr_putc(global const float *solution, global float *coarse_ratio, const int level_w, const int level_h, const int c)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= level_w || y >= level_h) return;
  const int i = y * level_w + x;
  coarse_ratio[i * 3 + c] = solution[i];
}

// One explicit diffusion step over the hole's bounding box (ping-pong: reads src, writes
// dst; the host swaps them). Non-hole pixels copy through. Hole pixels take a small step
// (factor 0.18) along the local second differences uxx/uyy/uxy weighted by the structure
// tensor dxx/dxy/dyy, which smooths along image edges but not across them. Neighbours clamp
// to the FULL grid, not to the box, exactly like the CPU sweep in _aniso_iterate.
kernel void
hl_aniso_iter(global const float *source, global float *dest, global const uchar *coarse_hole,
              global const float *tensor_xx, global const float *tensor_xy, global const float *tensor_yy,
              global const float *coarse_obstacle,
              const int width, const int height, const int c,
              const int box_x0, const int box_y0, const int box_x1, const int box_y1,
              const float react, const float react_target)
{
  const int x = box_x0 + get_global_id(0);
  const int y = box_y0 + get_global_id(1);
  if(x > box_x1 || y > box_y1) return;
  const int i = y * width + x;
  if(!coarse_hole[i * 3 + c])
  {
    dest[i] = source[i];
    return;
  }
  const int x_west = max(x - 1, 0);
  const int x_east = min(x + 1, width - 1);
  const int y_north = max(y - 1, 0);
  const int y_south = min(y + 1, height - 1);
  const float center = source[i];
  // second differences (the Hessian of r): d_xx r, d_yy r, mixed d_xy r
  const float d2_xx = source[y * width + x_east] - 2.f * center + source[y * width + x_west];
  const float d2_yy = source[y_south * width + x] - 2.f * center + source[y_north * width + x];
  const float d2_xy = 0.25f * (source[y_south * width + x_east] - source[y_south * width + x_west]
                             - source[y_north * width + x_east] + source[y_north * width + x_west]);
  // r <- max( r + 0.18*(D_xx d_xx r + 2 D_xy d_xy r + D_yy d_yy r), c0/L ): explicit trace-form
  // step tr(D Hess r), then obstacle projection each step (monotone obstacle-problem relaxation,
  // mirrors the CPU _aniso_iterate_obs)
  // the -0.18*react*(r - target) term is the screened reaction of the "inpaint a flat color"
  // parameter (0 on the pyramid levels; active in the full-resolution polish -- see the CPU twin)
  dest[i] = fmax(center + 0.18f * (tensor_xx[i] * d2_xx + 2.f * tensor_xy[i] * d2_xy + tensor_yy[i] * d2_yy)
                     - 0.18f * react * (center - react_target),
                 coarse_obstacle[i * 3 + c]);
}

// Single-workgroup variant of hl_aniso_iter: for a SMALL bounding box, run all `iters`
// diffusion steps inside one launch. Exactly one group of threads, so barrier() synchronizes
// all of them between steps; each step reads only the previous buffer, so the result is
// bit-identical to the separate-launch loop, minus the per-launch overhead.
kernel void
hl_aniso_iter_block(global float *buffer_a, global float *buffer_b, global const uchar *coarse_hole,
                    global const float *tensor_xx, global const float *tensor_xy, global const float *tensor_yy,
                    global const float *coarse_obstacle,
                    const int width, const int height, const int c,
                    const int box_x0, const int box_y0, const int box_x1, const int box_y1, const int iters,
                    const float react, const float react_target)
{
  const int local_id = get_local_id(0);
  const int local_size = get_local_size(0);
  const int box_w = box_x1 - box_x0 + 1;
  const int box_h = box_y1 - box_y0 + 1;
  const int box_pixels = box_w * box_h;
  global float *source = buffer_a;
  global float *dest = buffer_b;
  for(int iteration = 0; iteration < iters; iteration++)
  {
    for(int thread_index = local_id; thread_index < box_pixels; thread_index += local_size)
    {
      const int y = box_y0 + thread_index / box_w;
      const int x = box_x0 + thread_index - (thread_index / box_w) * box_w;
      const int i = y * width + x;
      if(!coarse_hole[i * 3 + c]) { dest[i] = source[i]; continue; }
      const int x_west = max(x - 1, 0);
      const int x_east = min(x + 1, width - 1);
      const int y_north = max(y - 1, 0);
      const int y_south = min(y + 1, height - 1);
      const float center = source[i];
      const float d2_xx = source[y * width + x_east] - 2.f * center + source[y * width + x_west];
      const float d2_yy = source[y_south * width + x] - 2.f * center + source[y_north * width + x];
      const float d2_xy = 0.25f * (source[y_south * width + x_east] - source[y_south * width + x_west]
                                 - source[y_north * width + x_east] + source[y_north * width + x_west]);
      // r <- max(r + 0.18*tr(D Hess r), c0/L): trace-form step + obstacle projection each step
      // (mirrors _aniso_iterate_obs)
      dest[i] = fmax(center + 0.18f * (tensor_xx[i] * d2_xx + 2.f * tensor_xy[i] * d2_xy + tensor_yy[i] * d2_yy)
                             - 0.18f * react * (center - react_target),
                         coarse_obstacle[i * 3 + c]);
    }
    barrier(CLK_GLOBAL_MEM_FENCE);
    global float *tmp = source; source = dest; dest = tmp;
  }
}

// Write the diffused coarse ratios back to full resolution: every clipped channel
// (vldan < 0.5) of every fine pixel takes the bilinear interpolation of the coarse ratio
// plane dr; valid channels keep their values untouched.
// MATHS BRIDGE -- bilinear prolongation of the coarse-level chroma r into the fine hole pixels,
// seeding the next finer level (the coarse->fine step of the Step-8 pyramid); anchors keep their r.
kernel void
hl_aniso_splat(global float *chroma, global const float *valid_anchor, global const float *coarse_ratio,
               const int width, const int height, const int coarse_w, const int coarse_h, const int step)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int fine_index = y * width + x;
  const float src_x = ((float)x + 0.5f) / step - 0.5f;
  const float src_y = ((float)y + 0.5f) / step - 0.5f;
  const int x_lo = clamp((int)floor(src_x), 0, coarse_w - 1);
  const int y_lo = clamp((int)floor(src_y), 0, coarse_h - 1);
  const int x_hi = min(x_lo + 1, coarse_w - 1);
  const int y_hi = min(y_lo + 1, coarse_h - 1);
  const float frac_x = clamp(src_x - x_lo, 0.f, 1.f);
  const float frac_y = clamp(src_y - y_lo, 0.f, 1.f);
  for(int c = 0; c < 3; c++)
  {
    if(valid_anchor[fine_index * 4 + c] >= 0.5f) continue;
    const float top_row = coarse_ratio[(y_lo * coarse_w + x_lo) * 3 + c] * (1.f - frac_x) + coarse_ratio[(y_lo * coarse_w + x_hi) * 3 + c] * frac_x;
    const float bottom_row = coarse_ratio[(y_hi * coarse_w + x_lo) * 3 + c] * (1.f - frac_x) + coarse_ratio[(y_hi * coarse_w + x_hi) * 3 + c] * frac_x;
    chroma[fine_index * 4 + c] = top_row * (1.f - frac_y) + bottom_row * frac_y;
  }
}
