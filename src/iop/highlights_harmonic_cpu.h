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

// C (CPU) code of the highlights harmonic-transposition mode. Textual include unit of
// highlights.c (not a standalone TU): relies on its CFA helpers, buffer conventions and
// on highlights_harmonic_common.h (included before this by highlights.c). The OpenCL twin
// lives in highlights_harmonic_cl.h with the same section order.



static __thread _hl_gauss_slot_t _hl_gauss_cache[HL_GAUSS_SLOTS] = { { 0 } };
static __thread int _hl_gauss_rr = 0;

static dt_gaussian_t *_hl_gauss_get(const int width, const int height, const int channels, const float sigma)
{
  for(int i = 0; i < HL_GAUSS_SLOTS; i++)
    if(_hl_gauss_cache[i].gaussian && _hl_gauss_cache[i].width == width && _hl_gauss_cache[i].height == height
       && _hl_gauss_cache[i].channels == channels && _hl_gauss_cache[i].sigma == sigma)
      return _hl_gauss_cache[i].gaussian;

  _hl_gauss_slot_t *slot = &_hl_gauss_cache[(_hl_gauss_rr++) % HL_GAUSS_SLOTS];
  if(slot->gaussian) dt_gaussian_free(slot->gaussian);
  float vmax[4] = { 1e9f, 1e9f, 1e9f, 1e9f };
  float vmin[4] = { -1e9f, -1e9f, -1e9f, -1e9f };
  slot->gaussian = dt_gaussian_init(width, height, channels, vmax, vmin, sigma, 0);
  slot->width = width;
  slot->height = height;
  slot->channels = channels;
  slot->sigma = sigma;
  return slot->gaussian;
}

static void _hl_gauss_cache_flush(void)
{
  for(int i = 0; i < HL_GAUSS_SLOTS; i++)
  {
    if(_hl_gauss_cache[i].gaussian) dt_gaussian_free(_hl_gauss_cache[i].gaussian);
    _hl_gauss_cache[i] = (_hl_gauss_slot_t){ 0 };
  }
}



// Connected-component (8-neighbour) labelling of the any-clip mask (channel 3 > 0).
// Each region's read box is its bbox dilated by pad = clamp(pad_factor*max_extent,
// pad_min, pad_max). Returns region count; writes a malloc'd array to *regions_out.
//
// MATHS BRIDGE -- Step 1 (segmentation), article "The algorithm" step 1: connected-component
// (8-neighbour) segmentation groups the clipped pixels into regions Omega. The labelling is an
// explicit-stack flood fill (not union-find); each region accumulates, as it is visited, its bbox and
// its reconstruction radius R = max_{x in Omega} delta(x) -- the deepest distance-transform depth in
// the region (article §"The reconstruction radius": the radius of a hole is its DEPTH, not its size;
// for a thin streak R is its half-thickness, so R, not the bbox, sets the reach). The read box is the
// bbox padded by pad = ceil(pad_factor * R) so the guide fit sees valid data as far out as the deepest
// pixel needs, and no farther. A second pass (union-find) merges regions whose padded read boxes
// overlap, since those share reconstruction context.
static int _segment_clipped_regions(const uint8_t *const restrict maskb, const float *const restrict depth,
                                    const int width, const int height, const float pad_factor, const int pad_min,
                                    const int pad_max, _hl_region_t **regions_out)
{
  const size_t npix = (size_t)width * height;
  int *const restrict label = calloc(npix, sizeof(int));  // 0 = background / unvisited
  int *const restrict stack = malloc(npix * sizeof(int)); // flood-fill work stack
  if(!label || !stack)
  {
    free(label);
    free(stack);
    *regions_out = NULL;
    return 0;
  }

  int capacity = 64, count = 0;
  _hl_region_t *regions = malloc((size_t)capacity * sizeof(_hl_region_t));
  if(!regions)
  {
    free(label);
    free(stack);
    *regions_out = NULL;
    return 0;
  }

  for(size_t pixel_index = 0; pixel_index < npix; pixel_index++)
  {
    // seed on the REAL feather support: the 5x5 box mean's genuine values are >= 1/25, while
    // the CPU running-sum blur leaves ~1e-7 cancellation residue on millions of pixels whose
    // true value is zero -- seeding on > 0 made the region topology depend on float noise
    // (and differ between the CPU and OpenCL gathers, which compute exact zeros)
    if(label[pixel_index] || !maskb[pixel_index]) continue;
    int stack_top = 0;
    stack[stack_top++] = (int)pixel_index;
    label[pixel_index] = count + 1;
    // bounding box of the region, grown pixel by pixel as the flood fill visits them
    int x_min = (int)(pixel_index % (size_t)width);
    int x_max = x_min;
    int y_min = (int)(pixel_index / (size_t)width);
    int y_max = y_min;
    float rmax = depth[pixel_index]; // reconstruction radius = deepest clip-to-valid distance in the region
    while(stack_top > 0)
    {
      const int visited_index = stack[--stack_top];
      const int visited_x = visited_index % width;
      const int visited_y = visited_index / width;

      // grow the region's bounding box to include the visited pixel, and keep the deepest
      // clip-to-valid distance seen so far (it becomes the region's reconstruction radius)
      if(visited_x < x_min) x_min = visited_x;
      if(visited_x > x_max) x_max = visited_x;
      if(visited_y < y_min) y_min = visited_y;
      if(visited_y > y_max) y_max = visited_y;
      if(depth[visited_index] > rmax) rmax = depth[visited_index];

      // push every in-bounds, still-unlabelled clipped neighbour (8-connectivity) onto the
      // flood-fill stack, so connected clipped pixels end up in the same region
      for(int delta_y = -1; delta_y <= 1; delta_y++)
        for(int delta_x = -1; delta_x <= 1; delta_x++)
        {
          if(!delta_x && !delta_y) continue;

          const int neighbour_x = visited_x + delta_x;
          const int neighbour_y = visited_y + delta_y;
          if(neighbour_x < 0 || neighbour_y < 0 || neighbour_x >= width || neighbour_y >= height) continue;

          const size_t neighbour_index = (size_t)neighbour_y * width + neighbour_x;
          if(label[neighbour_index] || !maskb[neighbour_index]) continue;

          label[neighbour_index] = count + 1;
          stack[stack_top++] = (int)neighbour_index;
        }
    }
    if(count >= capacity)
    {
      capacity *= 2;
      _hl_region_t *const tmp = realloc(regions, (size_t)capacity * sizeof(_hl_region_t));
      if(!tmp)
      {
        free(regions);
        free(label);
        free(stack);
        *regions_out = NULL;
        return 0;
      }
      regions = tmp;
    }
    // radius = deepest clip-to-valid distance (distance transform), padded by pad_factor of it
    const int pad = CLAMP((int)(pad_factor * rmax + 0.5f), pad_min, pad_max); // pad = clamp(ceil(pad_factor * R))
    regions[count].x0 = x_min; // clipped-pixel bbox (accumulated over the flood fill)
    regions[count].y0 = y_min;
    regions[count].x1 = x_max;
    regions[count].y1 = y_max;
    regions[count].pad = pad;
    regions[count].radius = rmax; // reconstruction radius R = max_{x in Omega} delta(x)
    regions[count].rx0 = MAX(x_min - pad, 0);
    regions[count].ry0 = MAX(y_min - pad, 0);
    regions[count].rx1 = MIN(x_max + pad, width - 1);
    regions[count].ry1 = MIN(y_max + pad, height - 1);
    count++;
  }
  free(label);
  free(stack);

  // Merge regions whose padded READ boxes overlap. Such regions share reconstruction context, so
  // processing them separately is redundant and leaves a seam where their fills meet (each sees the
  // other only as unreconstructed clip values). Union-find on padded-box intersection, then rebuild
  // one region per group: union of the member CLIPPED bboxes, re-padded from the merged extent.
  if(count > 1)
  {
    int *const restrict parent = malloc((size_t)count * sizeof(int));
    _hl_region_t *const restrict merged = malloc((size_t)count * sizeof(_hl_region_t));
    int *const restrict map = malloc((size_t)count * sizeof(int));
    if(!parent || !merged || !map)
    {
      free(parent);
      free(merged);
      free(map);
      *regions_out = regions;
      return count;
    }
    for(int i = 0; i < count; i++) parent[i] = i;

    // union every pair whose padded read boxes intersect (they share reconstruction context)
    for(int i = 0; i < count; i++)
    {
      for(int j = i + 1; j < count; j++)
      {
        // skip disjoint padded boxes
        if(regions[i].rx0 > regions[j].rx1 || regions[j].rx0 > regions[i].rx1) continue;
        if(regions[i].ry0 > regions[j].ry1 || regions[j].ry0 > regions[i].ry1) continue;

        // find the root of i (path halving)
        int root_i = i;
        while(parent[root_i] != root_i)
        {
          parent[root_i] = parent[parent[root_i]];
          root_i = parent[root_i];
        }

        // find the root of j (path halving)
        int root_j = j;
        while(parent[root_j] != root_j)
        {
          parent[root_j] = parent[parent[root_j]];
          root_j = parent[root_j];
        }

        // link the two components
        if(root_i != root_j) parent[root_j] = root_i;
      }
    }

    for(int i = 0; i < count; i++) map[i] = -1;

    // fold each component into its root: union the clipped bboxes, keep the group's MAX padding
    int mcount = 0;
    for(int i = 0; i < count; i++)
    {
      // find the root (path halving)
      int root_i = i;
      while(parent[root_i] != root_i)
      {
        parent[root_i] = parent[parent[root_i]];
        root_i = parent[root_i];
      }

      if(map[root_i] < 0)
      {
        // first member of this group: seed the merged region with it
        map[root_i] = mcount;
        merged[mcount] = regions[i];
        mcount++;
      }
      else
      {
        // grow the group's bbox and keep the largest reconstruction radius in the group, so the
        // smaller holes inherit enough context (per the merge rule)
        _hl_region_t *const merged_region = &merged[map[root_i]];
        merged_region->x0 = MIN(merged_region->x0, regions[i].x0);
        merged_region->y0 = MIN(merged_region->y0, regions[i].y0);
        merged_region->x1 = MAX(merged_region->x1, regions[i].x1);
        merged_region->y1 = MAX(merged_region->y1, regions[i].y1);
        merged_region->pad = MAX(merged_region->pad, regions[i].pad);
        merged_region->radius = fmaxf(merged_region->radius, regions[i].radius);
      }
    }

    // pad every merged region by the group's largest radius, clamped to the image
    for(int merged_region = 0; merged_region < mcount; merged_region++)
    {
      const int pad = merged[merged_region].pad;
      merged[merged_region].rx0 = MAX(merged[merged_region].x0 - pad, 0);
      merged[merged_region].ry0 = MAX(merged[merged_region].y0 - pad, 0);
      merged[merged_region].rx1 = MIN(merged[merged_region].x1 + pad, width - 1);
      merged[merged_region].ry1 = MIN(merged[merged_region].y1 + pad, height - 1);
    }
    free(parent);
    free(map);
    free(regions);
    *regions_out = merged;
    return mcount;
  }

  *regions_out = regions;
  return count;
}

// PERF (DT_DEBUG_PERF): time spent inside _region_blur, per region. Thread-local because the preview
// and main pipes can run _region_guided_filter concurrently; each accumulates on its own thread.
static __thread double _hl_blur_seconds = 0.0;


// Gaussian blur of a packed 4-channel region buffer (in -> out) at the given sigma.
static inline void _region_blur(const float *const restrict in, float *const restrict out, const int region_w,
                                const int region_h, const float sigma)
{
  const double blur_start_time = dt_get_wtime();
  dt_gaussian_t *const gaussian = _hl_gauss_get(region_w, region_h, 4, sigma); // cached, do not free

  if(!gaussian)
  {
    memcpy(out, in, (size_t)region_w * region_h * 4 * sizeof(float));
    return;
  }

  dt_gaussian_blur_4c(gaussian, in, out);
  _hl_blur_seconds += dt_get_wtime() - blur_start_time;
}



// Isotropic 9-point Laplacian (Oono & Puri 1987) with clamped/replicate borders, as in the original
// reconstruction. The Laplacian measures how much each pixel deviates from the average of its
// neighbours; this 9-point version is much less grid-anisotropic (less axis-biased) than the
// 5-point stencil:
//         [ 1   4   1 ]
//   1/6 * [ 4  -20  4 ]
//         [ 1   4   1 ]
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
static int _sp_pde_assemble(const uint8_t *const restrict hole, const float *const restrict diffusion,
                            const float diffusion_const, const int order, const float lambda, const int region_w,
                            const int region_h, int **matrix_col_ptr_out, int **matrix_row_index_out,
                            double **matrix_values_out, int **perm_grid_out, int *n_unknowns_out)
{
  const size_t region_pixels = (size_t)region_w * region_h;
  int n_unknowns = 0;
  for(size_t i = 0; i < region_pixels; i++)
    if(hole[i]) n_unknowns++;
  if(n_unknowns == 0 || n_unknowns > DT_HL_SPARSE_MAX) return 0;

  int *grid_to_unknown = (int *)dt_alloc_align(sizeof(int) * region_pixels);
  int *unknown_to_grid = (int *)dt_alloc_align(sizeof(int) * n_unknowns);
  int *unknown_x = (int *)dt_alloc_align(sizeof(int) * n_unknowns);
  int *unknown_y = (int *)dt_alloc_align(sizeof(int) * n_unknowns);
  int *permutation = (int *)dt_alloc_align(sizeof(int) * n_unknowns);
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

  inverse_perm = (int *)dt_alloc_align(sizeof(int) * n_unknowns);
  if(!inverse_perm) goto done;
  for(int perm_index = 0; perm_index < n_unknowns; perm_index++)
    inverse_perm[permutation[perm_index]] = perm_index;

  // assembly, two passes (count then fill), upper triangle in permuted indexing
  matrix_col_ptr = (int *)dt_alloc_align(sizeof(int) * (n_unknowns + 1));
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
      matrix_row_index = (int *)dt_alloc_align(sizeof(int) * total);
      matrix_values = (double *)dt_alloc_align(sizeof(double) * total);
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
  perm_grid = (int *)dt_alloc_align(sizeof(int) * n_unknowns);
  if(!perm_grid) goto done;
  for(int perm_index = 0; perm_index < n_unknowns; perm_index++)
    perm_grid[perm_index] = unknown_to_grid[permutation[perm_index]];
  success = 1;

done:
  dt_free_align(grid_to_unknown);
  dt_free_align(unknown_x);
  dt_free_align(unknown_y);
  dt_free_align(inverse_perm);
  dt_free_align(unknown_to_grid);
  dt_free_align(permutation);
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
    dt_free_align(matrix_col_ptr);
    dt_free_align(matrix_row_index);
    dt_free_align(matrix_values);
    dt_free_align(perm_grid);
  }
  return success;
}

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
static _sp_chol_t *_sp_pde_factor(const uint8_t *const restrict hole, const float *const restrict diffusion,
                                  const int order, const float lambda, const int region_w, const int region_h,
                                  int **perm_out, int *n_unknowns_out, const dt_dev_pixelpipe_t *pipe)
{
  int *matrix_col_ptr = NULL, *matrix_row_index = NULL, *perm_grid = NULL;
  double *matrix_values = NULL;
  int n_unknowns = 0;
  if(!_sp_pde_assemble(hole, diffusion, 0.f, order, lambda, region_w, region_h, &matrix_col_ptr, &matrix_row_index,
                       &matrix_values, &perm_grid, &n_unknowns))
    return NULL;

  _sp_chol_t *factor = _sp_chol_factor(n_unknowns, matrix_col_ptr, matrix_row_index, matrix_values, pipe);
  dt_free_align(matrix_col_ptr);
  dt_free_align(matrix_row_index);
  dt_free_align(matrix_values);
  if(!factor)
  {
    dt_free_align(perm_grid);
    return NULL;
  }
  *perm_out = perm_grid;
  *n_unknowns_out = n_unknowns;
  return factor;
}

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
static void _sp_pde_solve(const _sp_chol_t *const factor, const int *const restrict perm_grid,
                          float *const restrict field, const uint8_t *const restrict hole,
                          const float *const restrict diffusion, const float *const restrict target,
                          const float *const restrict source, const int order, const float lambda,
                          const int region_w, const int region_h, double *const restrict rhs,
                          float *const restrict embedded, float *const restrict operator_out,
                          float *const restrict scratch)
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
static void _region_pde_solve(float *const restrict field, const uint8_t *const restrict hole,
                              const float *const restrict diffusion, const float *const restrict target,
                              const float *const restrict source, const int order, const float lambda,
                              const int region_w, const int region_h, float *const restrict residual,
                              float *const restrict search_dir, float *const restrict operator_dir,
                              float *const restrict embedded, float *const restrict scratch, const int maxiter)
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

// True biharmonic (thin-plate) dome: DIRECT solve of Delta^2 u = 0 on the hole with u fixed
// outside (2-ring Dirichlet, as the Delta^2 stencil reaches two rings) -- exactly the Python
// prototype's grid solve, but with the dense Cholesky (solve_hermitian) instead of scipy's sparse
// spsolve. The restricted biharmonic operator is symmetric positive-definite, so Cholesky applies,
// and a DIRECT solve gives the true dome regardless of conditioning (unlike CG, which stalls in
// float at kappa ~ L^4 and tilts to black). A dense solve is O(N^3), so we assemble it on a grid
// coarse enough (target <= ~2000 hole unknowns; identity when the hole is already small) and
// bilinearly upsample the low-frequency dome. `field` (rw x rh) holds boundary values on non-hole
// and is filled on `hole`.
//
// MATHS BRIDGE -- article "First principles / Biharmonic inpainting" + E_bihar (§"The optimization
// problem", term 2 / the all-clip core term): minimize the thin-plate (biharmonic) energy
//   E_bihar[u] = int_Omega (Delta u)^2 dOmega   ==>   Delta^2 u = 0 on Omega,  u|dOmega = u_valid.
// Its Euler-Lagrange equation Delta^2 u = 0 (the Laplacian of the Laplacian) makes Delta u itself
// harmonic, so the rim's rising CURVATURE is carried into the hole instead of flattened (harmonic
// Delta u = 0 would leave a matte disk) -- a smooth dome continuing both value AND slope of the
// valid rim. The Dirichlet data u|dOmega = u_valid is the non-hole `field`; here it is used both
// for the per-channel fallback (u = one clipped channel) and, hue-coupled, for the shared
// luminance dome u = L_sum = R+G+B of the all-clipped core.
static void _biharmonic_dome(float *const restrict field, const uint8_t *const restrict hole, const int region_w,
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
  uint8_t *const restrict coarse_hole = (uint8_t *)dt_alloc_align(sizeof(uint8_t) * coarse_pixels);
  int *const restrict coarse_index = (int *)dt_alloc_align(sizeof(int) * coarse_pixels);
  if(!coarse_field || !coarse_hole || !coarse_index)
  {
    dt_pixelpipe_cache_free_align(coarse_field);
    dt_free_align(coarse_hole);
    dt_free_align(coarse_index);
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
      int *unknown_x = (int *)dt_alloc_align(sizeof(int) * n_unknowns);
      int *unknown_y = (int *)dt_alloc_align(sizeof(int) * n_unknowns);
      int *permutation = (int *)dt_alloc_align(sizeof(int) * n_unknowns);
      int *inverse_perm = (int *)dt_alloc_align(sizeof(int) * n_unknowns);
      int *matrix_col_ptr = (int *)dt_alloc_align(sizeof(int) * (n_unknowns + 1));
      double *right_hand_side = (double *)dt_alloc_align(sizeof(double) * n_unknowns);
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
            matrix_row_index = (int *)dt_alloc_align(sizeof(int) * total);
            matrix_values = (double *)dt_alloc_align(sizeof(double) * total);
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

      dt_free_align(unknown_x);
      dt_free_align(unknown_y);
      dt_free_align(permutation);
      dt_free_align(inverse_perm);
      dt_free_align(matrix_col_ptr);
      dt_free_align(matrix_row_index);
      dt_free_align(matrix_values);
      dt_free_align(right_hand_side);
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
  dt_free_align(coarse_hole);
  dt_free_align(coarse_index);
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

// diffusion tensor from a luminance plane (blurred by two 3x3 box passes to stabilise gradients)
// MATHS BRIDGE: builds D = t t^T + c2 g g^T, g = unit gradient (across the edge), t = isophote
// (along the edge), c2 = exp(-|grad L|/(4 <|grad L|>)) the cross-edge damping (strong edges block
// smoothing across them; flat areas diffuse isotropically, D -> I). Same isophote/gradient design
// as the E_transport tensor of _cf_adaptive_tensor, without the variance-adaptive m blend.
static void _aniso_tensor(const float *const restrict luminance, float *const restrict tensor_xx,
                          float *const restrict tensor_xy, float *const restrict tensor_yy,
                          float *const restrict scratch, const int region_w, const int region_h)
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
    const float grad_unit_x = grad_x * inv_mag + (1.f - nonzero); // cos(theta_grad)
    const float grad_unit_y = grad_y * inv_mag;                   // sin(theta_grad)
    const float cross_damp = expf(-grad_mag / (4.f * grad_mean)); // c2 = exp(-|grad L|/(4 <|grad L|>))
    const float isophote_x = -grad_unit_y, isophote_y = grad_unit_x; // t = g rotated 90 deg (level line)

    // D = t t^T + c2 * g g^T (isophote outer product + damped gradient outer product)
    tensor_xx[i] = isophote_x * isophote_x + cross_damp * grad_unit_x * grad_unit_x;
    tensor_xy[i] = isophote_x * isophote_y + cross_damp * grad_unit_x * grad_unit_y;
    tensor_yy[i] = isophote_y * isophote_y + cross_damp * grad_unit_y * grad_unit_y;
  }
}


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
static void _aniso_iterate_obs(float *const restrict field, const float *const restrict obstacle,
                               const uint8_t *const restrict hole, const float *const restrict tensor_xx,
                               const float *const restrict tensor_xy, const float *const restrict tensor_yy,
                               float *const restrict tmp, const int region_w, const int region_h, const int iters,
                               const int box_x_lo, const int box_y_lo, const int box_x_hi, const int box_y_hi)
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

        // r <- max( r + 0.18*(D_xx d_xx r + 2 D_xy d_xy r + D_yy d_yy r), obstacle ): explicit
        // trace-form step tr(D Hess r) then obstacle projection (article Step 8 update rule)
        tmp[i] = fmaxf(center + 0.18f * (tensor_xx[i] * d2_xx + 2.f * tensor_xy[i] * d2_xy + tensor_yy[i] * d2_yy),
                       obstacle[i]);
      }

    __OMP_PARALLEL_FOR__()
    for(int y = box_y_lo; y <= box_y_hi; y++)
      memcpy(field + (size_t)y * region_w + box_x_lo, tmp + (size_t)y * region_w + box_x_lo,
             (size_t)(box_x_hi - box_x_lo + 1) * sizeof(float));
  }
}

// ---- steady-state solvers for the structure-steered chroma (solver bake-off) ---------------
// DT_HL_ANISO_SOLVER: 0 = shipped explicit pyramid (240 it/level, full-grid sweeps)
//                     1 = same flow, empty-hole skip + bounding-box sweeps (output-identical)
//                     2 = DIVERGENCE-form steady state div(D grad u) = 0, Weickert
//                         nonnegativity stencil (a symmetric-positive-definite matrix whose
//                         off-diagonals are all <= 0, so the solution can never overshoot
//                         its boundary values), exact sparse Cholesky, factor shared by the
//                         three channels, no pyramid
//                     3 = TRACE-form steady state tr(D Hu) = 0 (today's discretization,
//                         converged instead of truncated), matrix-free BiCGSTAB (an
//                         iterative solver for nonsymmetric systems), no pyramid

// Edge weight of the nonnegativity-preserving divergence stencil between two pixels; the
// cross tensor component is clamped to +-min(a, c) at the edge so all weights stay >= 0.
// All-nonnegative weights guarantee the solved value at any pixel stays a weighted average
// of its neighbours, so the solve can never overshoot the rim chroma.
//
// MATHS BRIDGE -- Weickert's nonnegativity discretization of div(D grad r) (article §"The update
// rules", the "edge-weighted 8-neighbour graph Laplacian"): the off-diagonal weight w_ij between
// pixel i and neighbour j, from the edge-averaged tensor D = [[a, b],[b, c]]. Clamping the mixed
// term b to +-min(a,c) and splitting it over axis vs diagonal edges keeps every w_ij >= 0, so the
// assembled matrix (diagonal = sum of edge weights, off-diagonals = -w_ij) is an SPD M-matrix ->
// its solution obeys the discrete maximum principle (no overshoot of the rim chroma).
static inline float _aniso_edge_w(const float *const restrict tensor_xx, const float *const restrict tensor_xy,
                                  const float *const restrict tensor_yy, const size_t i, const size_t j,
                                  const int offset_x, const int offset_y)
{
  const float avg_xx = 0.5f * (tensor_xx[i] + tensor_xx[j]); // a = D_xx averaged across the edge
  const float avg_yy = 0.5f * (tensor_yy[i] + tensor_yy[j]); // c = D_yy averaged across the edge
  const float limit = fminf(avg_xx, avg_yy);
  const float cross = CLAMP(0.5f * (tensor_xy[i] + tensor_xy[j]), -limit, limit); // b clamped to +-min(a,c) >= 0 guarantee

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
static int _aniso_div_solve(float *const restrict ratios, const float *const restrict valid,
                            const float *const restrict luminance, float *const restrict scratch_planes,
                            const int region_w, const int region_h, const dt_dev_pixelpipe_t *pipe)
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

  int *grid_to_unknown = (int *)dt_alloc_align(sizeof(int) * region_pixels);
  int *unknown_to_grid = (int *)dt_alloc_align(sizeof(int) * n_unknowns);
  int *unknown_x = (int *)dt_alloc_align(sizeof(int) * n_unknowns);
  int *unknown_y = (int *)dt_alloc_align(sizeof(int) * n_unknowns);
  int *permutation = (int *)dt_alloc_align(sizeof(int) * n_unknowns);
  int *inverse_perm = (int *)dt_alloc_align(sizeof(int) * n_unknowns);
  int *matrix_col_ptr = (int *)dt_alloc_align(sizeof(int) * (n_unknowns + 1));
  double *right_hand_side = (double *)dt_alloc_align(sizeof(double) * (size_t)n_unknowns * 3);
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
        matrix_row_index = (int *)dt_alloc_align(sizeof(int) * total);
        matrix_values = (double *)dt_alloc_align(sizeof(double) * total);
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
                matrix_values[matrix_col_ptr[perm_index] + n_col_entries] = -(double)weight; // off-diagonal A_ij = -w_ij
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

        // diagonal last (any order works: columns need not be sorted)
        if(pass == 1)
        {
          matrix_row_index[matrix_col_ptr[perm_index] + n_col_entries] = perm_index;
          matrix_values[matrix_col_ptr[perm_index] + n_col_entries] = diagonal;
        }
        n_col_entries++;
        if(pass == 0) matrix_col_ptr[perm_index] = n_col_entries;
      }
    }

    if(success)
    {
      _sp_chol_t *factor = _sp_chol_factor(n_unknowns, matrix_col_ptr, matrix_row_index, matrix_values, pipe);
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

  dt_free_align(grid_to_unknown);
  dt_free_align(unknown_to_grid);
  dt_free_align(unknown_x);
  dt_free_align(unknown_y);
  dt_free_align(permutation);
  dt_free_align(inverse_perm);
  dt_free_align(matrix_col_ptr);
  dt_free_align(matrix_row_index);
  dt_free_align(matrix_values);
  dt_free_align(right_hand_side);
  return success;
}



// Coarse->fine full-value guided filter on one padded region -- THE CPU REFERENCE for the
// whole per-region reconstruction (the *_cl drivers above mirror its stages). Rebuilds the
// clipped channels of `interp` (full-res [red, green, blue, norm], white-balance normalized)
// in place, using the per-channel clip mask. Only clipped channels are written; the guide
// comes from the surviving (unclipped) channels along the local colour-line (a clipped
// channel predicted as a weighted sum of the other two plus an offset), fit on VALID samples.
// defined with the knee block below; used by the feathered joint-core composite too
static inline void _knee_blur(const float *const restrict in, float *const restrict out, const int width,
                              const int height, const float sigma);


// Variance-adaptive steering tensor: a continuous blend between the isophote
// tensor (transport along level lines, correct where a HARD EDGE crosses the zone: the content
// beyond the edge follows another colour-line) and the gradient tensor (radial transport,
// correct on a clean halo: the model lives on the rim and must travel inward). The blend weight
// m is the TREND-CORRECTED windowed variance of the steering plane: raw windowed variance minus
// the part the local linear ramp explains -- a smooth halo ramp has variance but no residual,
// a hard edge has residual variance no ramp can explain. m = v_res / (v_res + (k * mean)^2),
// scale-free (k = relative std threshold). D = [m + (1-m) c2] t t^T + [m c2 + (1-m)] g g^T,
// both weights in (0, 1], D SPD, so the Weickert stencil stays nonnegative (maximum principle).
//
// MATHS BRIDGE -- article "The algorithm" step 3, the E_transport steering tensor. Builds the D of
// E_transport = Sum_p integral grad(p)^T D grad(p) dOmega, whose Euler-Lagrange div(D grad p)=0 is the
// anisotropic fill relaxed below. Article eq:
//   D = [ m + (1-m) c2 ] t t^T  +  [ m c2 + (1-m) ] g g^T ,  c2 = exp(-|grad L_mean| / (4 <|grad L_mean|>))
//   m = v / (v + (k Lbar_mean)^2) ,  v = max( var_w(L_mean) - (4/3)|grad L_mean|^2 , 0 ) ,  k = 0.15
// g = unit gradient (uphill) of the steering plane L_mean, t = unit isophote (level-line), m in [0,1] the
// edge probability. m->0 (clean halo ramp) => D -> g g^T + c2 t t^T, transport radial inward along the
// ramp; m->1 (hard edge in the zone) => D -> t t^T + c2 g g^T, transport along the boundary, not across it.
static void _cf_adaptive_tensor(const float *const restrict luminance, float *const restrict tensor_xx,
                                float *const restrict tensor_xy, float *const restrict tensor_yy,
                                float *const restrict scratch_lin, float *const restrict scratch_quad,
                                const int region_w, const int region_h, const float k)
{
  const size_t region_pixels = (size_t)region_w * region_h;

  // two 3x3 box passes on L (into scratch_lin) and on L^2 (into scratch_quad)
  for(int pass = 0; pass < 2; pass++)
  {
    const float *const src_lin = (pass == 0) ? luminance : scratch_lin;
    const float *const src_quad = (pass == 0) ? luminance : scratch_quad; // pass 0 squares on the fly

    HL_PFOR(collapse(2))
    for(int y = 0; y < region_h; y++)
      for(int x = 0; x < region_w; x++)
      {
        double sum_lin = 0.0;
        double sum_quad = 0.0;
        int count = 0;
        for(int offset_y = -1; offset_y <= 1; offset_y++)
          for(int offset_x = -1; offset_x <= 1; offset_x++)
          {
            const int neighbour_y = CLAMP(y + offset_y, 0, region_h - 1);
            const int neighbour_x = CLAMP(x + offset_x, 0, region_w - 1);
            const float value_lin = src_lin[(size_t)neighbour_y * region_w + neighbour_x];
            sum_lin += value_lin;
            sum_quad += (pass == 0) ? (double)value_lin * value_lin
                                    : src_quad[(size_t)neighbour_y * region_w + neighbour_x];
            count++;
          }
        tensor_xx[(size_t)y * region_w + x] = (float)(sum_lin / count);
        tensor_yy[(size_t)y * region_w + x] = (float)(sum_quad / count);
      }

    HL_PFOR()
    for(size_t i = 0; i < region_pixels; i++)
    {
      scratch_lin[i] = tensor_xx[i];
      scratch_quad[i] = tensor_yy[i];
    }
  }

  // gradients of the blurred L + mean magnitude
  double grad_sum = 0.0;
  HL_PFOR(collapse(2) reduction(+ : grad_sum))
  for(int y = 0; y < region_h; y++)
    for(int x = 0; x < region_w; x++)
    {
      const int x_lo = MAX(x - 1, 0), x_hi = MIN(x + 1, region_w - 1);
      const int y_lo = MAX(y - 1, 0), y_hi = MIN(y + 1, region_h - 1);
      const float grad_x
          = 0.5f * (scratch_lin[(size_t)y * region_w + x_hi] - scratch_lin[(size_t)y * region_w + x_lo]);
      const float grad_y
          = 0.5f * (scratch_lin[(size_t)y_hi * region_w + x] - scratch_lin[(size_t)y_lo * region_w + x]);
      tensor_xx[(size_t)y * region_w + x] = grad_x;
      tensor_xy[(size_t)y * region_w + x] = grad_y;
      grad_sum += dt_fast_hypotf(grad_x, grad_y);
    }
  const float grad_mean = fmaxf((float)(grad_sum / (double)region_pixels), 1e-9f); // <|grad L_mean|>, the regional mean magnitude (exposure-independent normaliser)

  HL_PFOR()
  for(size_t i = 0; i < region_pixels; i++)
  {
    const float grad_x = tensor_xx[i];
    const float grad_y = tensor_xy[i];
    const float grad_mag = dt_fast_hypotf(grad_x, grad_y);
    const float nonzero = (grad_mag > 1e-12f) ? 1.f : 0.f;
    const float inv_mag = nonzero / (grad_mag + (1.f - nonzero));
    const float grad_unit_x = grad_x * inv_mag + (1.f - nonzero); // g = unit gradient direction (uphill)
    const float grad_unit_y = grad_y * inv_mag;
    const float isophote_x = -grad_unit_y, isophote_y = grad_unit_x; // t = unit isophote = g rotated 90deg
    const float cross_damp = expf(-grad_mag / (4.f * grad_mean)); // c2 = exp(-|grad L_mean| / (4 <|grad L_mean|>)), edge-crossing damping

    // trend-corrected windowed variance: two 3x3 box passes have spatial variance 4/3 per axis
    const float variance = fmaxf(scratch_quad[i] - scratch_lin[i] * scratch_lin[i], 0.f); // var_w(L_mean) = E[L^2]-E[L]^2 (centred by construction of the box passes)
    const float residual_var = fmaxf(variance - (4.f / 3.f) * (grad_x * grad_x + grad_y * grad_y), 0.f); // v = max(var_w - (4/3)|grad L_mean|^2, 0): subtract the variance the local ramp explains
    const float k_term = sqf(k * fmaxf(scratch_lin[i], 1e-9f)); // (k * Lbar_mean)^2, the scale-free contrast threshold
    const float edge_prob = residual_var / (residual_var + k_term + 1e-18f); // m = v / (v + (k Lbar_mean)^2) in [0,1]

    const float diffuse_tangent = edge_prob + (1.f - edge_prob) * cross_damp; // coeff of t t^T = m + (1-m) c2
    const float diffuse_gradient = edge_prob * cross_damp + (1.f - edge_prob); // coeff of g g^T = m c2 + (1-m)

    // D = diffuse_tangent * t t^T + diffuse_gradient * g g^T, stored as its symmetric xx/xy/yy entries
    tensor_xx[i] = diffuse_tangent * isophote_x * isophote_x + diffuse_gradient * grad_unit_x * grad_unit_x;
    tensor_xy[i] = diffuse_tangent * isophote_x * isophote_y + diffuse_gradient * grad_unit_x * grad_unit_y;
    tensor_yy[i] = diffuse_tangent * isophote_y * isophote_y + diffuse_gradient * grad_unit_y * grad_unit_y;
  }
}

// Coarse-to-fine harmonic fill of up to DT_HL_FILL_MAXP coefficient planes SHARING ONE anchor
// mask: hole pixels relax toward their 4-neighbour average (Jacobi) with anchors pinned, each
// pyramid level seeding the next finer one. Unconditionally stable by the maximum principle
// (values stay within the anchors' range), unlike a float CG on the near-singular pure-harmonic
// system, which diverges stochastically when the hole reaches the region border. Coefficients
// are smooth, so the solve runs on a base grid downsampled by `base_ds` and is bilinearly
// upsampled into the hole pixels.
// With `steer` non-NULL (the coefficient planes), the relaxation is tensor-weighted instead
// of uniform: per level, the variance-adaptive tensor is built from the downsampled steering
// plane (_cf_adaptive_tensor) and the update becomes an 8-neighbour average with the Weickert
// nonnegativity weights (_aniso_edge_w) -- all weights >= 0, so the fill stays a convex
// combination of anchors (maximum principle intact). NULL steer = plain isotropic fill (the
// rim-chrominance ratios, and any plane with no guide structure to follow).

// One level's Jacobi relaxation of NP planes sharing one anchor mask, macro-generated so NP
// is a compile-time literal: the plane guards fold away and the per-plane accumulators stay in
// registers. (An inline function with a runtime plane count does NOT specialize -- GCC outlines
// the OpenMP region and the count arrives through the shared-args struct, so the su[] array
// spilled to the stack on every fma and the fused sweep measured 2.5x SLOWER than the
// single-plane one. Literal NP recovers it.)
//
// Jacobi relaxation of the holes (anchors pinned): a flat 100-sweep budget per level.
// Convergence is guaranteed by the pyramid depth of the caller, NOT by the sweep count --
// boosting sweeps at the coarsest level instead was measured pathological (thousands of
// parallel sweeps of microsecond work = pure scheduling overhead, seconds per fill on small
// regions). One parallel region for the whole relaxation: launching a fresh team per sweep
// was pure scheduling overhead on these small grids (the sweep's work is microseconds; 100
// sweeps x levels x fills x regions reached tens of thousands of launches per image). Threads
// ping-pong between u and tmp (no per-sweep memcpy); the even sweep count lands the final
// solution in u. The omp-for barrier at the end of each sweep keeps Jacobi ordering.
// All NP planes advance inside the same sweep: the weights are read once per cell.
//
// MATHS BRIDGE -- article "The algorithm" step 3, one Jacobi sweep of the E_transport solver
// div(D grad p)=0 on the coefficient planes p in {a, b, d, R^2}. Discrete update rules (anchors are
// Dirichlet boundary data, pinned = copied through unchanged):
//   steered  (D != I): dst(i) = Sum_k w_ik * src(neighbour_k) / Sum_k w_ik  over the 8-neighbour
//                      Weickert nonnegativity stencil weights w_ik = _aniso_edge_w(D) >= 0, so the
//                      update is a convex combination of neighbours -> maximum principle holds.
//   isotropic (D = I): dst(i) = 1/4 (north + south + west + east), the plain harmonic (Laplace) fill.
// NOTE (C preprocessor): every comment inside this macro body MUST be a /* ... */ closed on its own
// physical line before the trailing backslash -- a // comment would splice with the next line and
// swallow the rest of the macro. That is why the annotations below use block-comment form.
#define DEFINE_CF_FILL_RELAX(NP)                                                                                  \
  static void _cf_fill_relax_##NP(                                                                                \
      float *const restrict field, float *const restrict tmp, const uint8_t *const restrict level_anchor,         \
      const float *const restrict edge_weights, const float *const restrict edge_weight_sum, const int coarse_w,  \
      const int coarse_h, const size_t cell_count, const int steered)                                             \
  {                                                                                                               \
    const int n_sweeps = 100;                                                                                     \
    __OMP_PARALLEL__()                                                                                            \
    for(int sweep = 0; sweep < n_sweeps; sweep++)                                                                 \
    {                                                                                                             \
      const float *const source = (sweep & 1) ? tmp : field;                                                      \
      float *const dest = (sweep & 1) ? field : tmp;                                                              \
      const float *const src0 = source;                                                                           \
      const float *const src1 = source + ((NP) > 1 ? cell_count : 0);                                             \
      const float *const src2 = source + ((NP) > 2 ? 2 * cell_count : 0);                                         \
      const float *const src3 = source + ((NP) > 3 ? 3 * cell_count : 0);                                         \
      float *const dst0 = dest;                                                                                   \
      float *const dst1 = dest + ((NP) > 1 ? cell_count : 0);                                                     \
      float *const dst2 = dest + ((NP) > 2 ? 2 * cell_count : 0);                                                 \
      float *const dst3 = dest + ((NP) > 3 ? 3 * cell_count : 0);                                                 \
                                                                                                                  \
      __OMP_FOR__(collapse(2))                                                                                    \
      for(int cell_y = 0; cell_y < coarse_h; cell_y++)                                                            \
        for(int cell_x = 0; cell_x < coarse_w; cell_x++)                                                          \
        {                                                                                                         \
          const size_t i = (size_t)cell_y * coarse_w + cell_x;                                                    \
                                                                                                                  \
          /* anchor cell = Dirichlet boundary datum p|anchors = p_fit: copy it through unchanged */             \
          if(level_anchor[i])                                                                                     \
          {                                                                                                       \
            dst0[i] = src0[i];                                                                                    \
            if((NP) > 1) dst1[i] = src1[i];                                                                       \
            if((NP) > 2) dst2[i] = src2[i];                                                                       \
            if((NP) > 3) dst3[i] = src3[i];                                                                       \
            continue;                                                                                             \
          }                                                                                                       \
                                                                                                                  \
          const size_t idx_north = (size_t)MAX(cell_y - 1, 0) * coarse_w + cell_x;                                \
          const size_t idx_south = (size_t)MIN(cell_y + 1, coarse_h - 1) * coarse_w + cell_x;                     \
          const size_t idx_west = (size_t)cell_y * coarse_w + MAX(cell_x - 1, 0);                                 \
          const size_t idx_east = (size_t)cell_y * coarse_w + MIN(cell_x + 1, coarse_w - 1);                      \
                                                                                                                  \
          if(steered)                                                                                             \
          {                                                                                                       \
            /* 8-neighbour Jacobi with the precomputed Weickert nonnegativity weights: every  */                  \
            /* weight >= 0, so the update is a convex combination and the maximum principle   */                  \
            /* is preserved.                                                                  */                  \
            static const int neighbour_dy[8] = { 0, 0, -1, 1, -1, 1, 1, -1 };                                     \
            static const int neighbour_dx[8] = { -1, 1, 0, 0, -1, 1, -1, 1 };                                     \
            float accum0 = 0.f, accum1 = 0.f, accum2 = 0.f, accum3 = 0.f;                                         \
            for(int k = 0; k < 8; k++)                                                                            \
            {                                                                                                     \
              const int neighbour_y = CLAMP(cell_y + neighbour_dy[k], 0, coarse_h - 1);                           \
              const int neighbour_x = CLAMP(cell_x + neighbour_dx[k], 0, coarse_w - 1);                           \
              const size_t j = (size_t)neighbour_y * coarse_w + neighbour_x;                                      \
              const float weight = edge_weights[i * 8 + k];                                                       \
              accum0 += weight * src0[j];                                                                         \
              if((NP) > 1) accum1 += weight * src1[j];                                                            \
              if((NP) > 2) accum2 += weight * src2[j];                                                            \
              if((NP) > 3) accum3 += weight * src3[j];                                                            \
            }                                                                                                     \
            /* dst = Sum_k w_ik src(nb_k) / Sum_k w_ik : the steered div(D grad p)=0 Jacobi update */             \
            const float weight_sum = edge_weight_sum[i];                                                          \
            const int valid = (weight_sum > 1e-9f);                                                               \
            dst0[i] = valid ? accum0 / weight_sum : src0[i];                                                      \
            if((NP) > 1) dst1[i] = valid ? accum1 / weight_sum : src1[i];                                         \
            if((NP) > 2) dst2[i] = valid ? accum2 / weight_sum : src2[i];                                         \
            if((NP) > 3) dst3[i] = valid ? accum3 / weight_sum : src3[i];                                         \
          }                                                                                                       \
          /* D = I: plain 4-neighbour average, the discrete harmonic (Laplace) fill div(grad p)=0 */              \
          else                                                                                                    \
          {                                                                                                       \
            dst0[i] = 0.25f * (src0[idx_north] + src0[idx_south] + src0[idx_west] + src0[idx_east]);              \
            if((NP) > 1) dst1[i] = 0.25f * (src1[idx_north] + src1[idx_south] + src1[idx_west] + src1[idx_east]); \
            if((NP) > 2) dst2[i] = 0.25f * (src2[idx_north] + src2[idx_south] + src2[idx_west] + src2[idx_east]); \
            if((NP) > 3) dst3[i] = 0.25f * (src3[idx_north] + src3[idx_south] + src3[idx_west] + src3[idx_east]); \
          }                                                                                                       \
        }                                                                                                         \
    }                                                                                                             \
  }

DEFINE_CF_FILL_RELAX(1)
DEFINE_CF_FILL_RELAX(2)
DEFINE_CF_FILL_RELAX(3)
DEFINE_CF_FILL_RELAX(4)

// MATHS BRIDGE -- article "The algorithm" step 3, the E_transport solver: the anchored, coarse-to-fine
// anisotropic transport of the coefficient planes. Minimizes E_transport = Sum_p int grad(p)^T D grad(p)
// with p|anchors = p_fit by relaxing div(D grad p)=0 to its fixed point. `hole` marks the cells to fill
// (holes); its complement are the anchors (the gated colour-line fits, R^2 > 0.25, bounded slopes).
// `steer` non-NULL feeds _cf_adaptive_tensor to build D (steered fill); NULL => D = I (plain harmonic
// fill). Coefficients are smooth, so the whole relaxation runs on a base grid at pitch ~sigma/4
// (article "Cell"), and Jacobi convergence comes from the PYRAMID DEPTH, not the fixed 100-sweep budget:
// the coarsest level starts from a flat anchor mean and each finer level is bilinearly seeded from the
// coarser solution, then corrected. Final result is bilinearly upsampled into the full-res hole pixels.
static void _cf_harmonic_fill_n(float *const restrict *vals, const int n_planes_in,
                                const uint8_t *const restrict hole, const int region_w, const int region_h,
                                const int base_ds, const float *const restrict steer,
                                const dt_dev_pixelpipe_t *pipe)
{
  const int n_planes = CLAMP(n_planes_in, 1, DT_HL_FILL_MAXP);
  const int downsample = CLAMP(base_ds, 1, 8);
  const int base_w = (region_w + downsample - 1) / downsample;
  const int base_h = (region_h + downsample - 1) / downsample;
  const size_t cell_count = (size_t)base_w * base_h;

  float *const restrict base_vals = dt_pixelpipe_cache_alloc_align_float(cell_count * n_planes, pipe);
  uint8_t *const restrict base_anchor = (uint8_t *)dt_alloc_align(sizeof(uint8_t) * cell_count);
  // field, tmp, f (n_planes planes each, plane-major) + shared L
  float *const restrict level_buffers
      = dt_pixelpipe_cache_alloc_align_float(cell_count * 3 * (size_t)n_planes, pipe);
  uint8_t *const restrict level_anchor = (uint8_t *)dt_alloc_align(sizeof(uint8_t) * cell_count);
  // aniso: base-grid steering plane + per-level {level_steer, tensor_xx, tensor_xy, tensor_yy, scratch,
  // var-scratch}
  // + per-cell edge weights (8, interleaved) + their sum, precomputed once per level
  float *const restrict aniso_aux = steer ? dt_pixelpipe_cache_alloc_align_float(cell_count * 16, pipe) : NULL;
  const int steered = (steer && aniso_aux) ? 1 : 0;

  if(!base_vals || !base_anchor || !level_buffers || !level_anchor || (steer && !aniso_aux))
  {
    // fallback: fill the holes with the global anchor mean (never leave garbage coefficients)
    for(int plane = 0; plane < n_planes; plane++)
    {
      float *const restrict plane_vals = vals[plane];
      double anchor_sum = 0.0;
      size_t anchor_count = 0;
      for(size_t i = 0; i < (size_t)region_w * region_h; i++)
        if(!hole[i])
        {
          anchor_sum += plane_vals[i];
          anchor_count++;
        }

      const float anchor_mean = anchor_count ? (float)(anchor_sum / (double)anchor_count) : 0.f;
      HL_PFOR()
      for(size_t i = 0; i < (size_t)region_w * region_h; i++)
        if(hole[i]) plane_vals[i] = anchor_mean;
    }

    dt_pixelpipe_cache_free_align(base_vals);
    dt_free_align(base_anchor);
    dt_pixelpipe_cache_free_align(level_buffers);
    dt_free_align(level_anchor);
    dt_pixelpipe_cache_free_align(aniso_aux);
    return;
  }

  // aniso: steering plane on the base grid (plain cell mean; the tensor smooths later)
  float *const restrict base_steer = steered ? aniso_aux + 5 * cell_count : NULL; // plane 6 = adaptive scratch
  if(steered)
  {
    HL_PFOR(collapse(2))
    for(int base_y = 0; base_y < base_h; base_y++)
      for(int base_x = 0; base_x < base_w; base_x++)
      {
        double accum = 0.0;
        int n_total = 0;
        for(int y = base_y * downsample; y < MIN((base_y + 1) * downsample, region_h); y++)
          for(int x = base_x * downsample; x < MIN((base_x + 1) * downsample, region_w); x++)
          {
            accum += steer[(size_t)y * region_w + x];
            n_total++;
          }
        base_steer[(size_t)base_y * base_w + base_x] = (float)(accum / n_total);
      }
  }

  // base grid: anchor-weighted mean per cell and per plane, anchor = cell majority (shared)
  HL_PFOR(collapse(2))
  for(int base_y = 0; base_y < base_h; base_y++)
    for(int base_x = 0; base_x < base_w; base_x++)
    {
      double accum[DT_HL_FILL_MAXP] = { 0.0 };
      int n_anchor = 0;
      int n_total = 0;

      for(int y = base_y * downsample; y < MIN((base_y + 1) * downsample, region_h); y++)
        for(int x = base_x * downsample; x < MIN((base_x + 1) * downsample, region_w); x++)
        {
          const size_t i = (size_t)y * region_w + x;
          n_total++;

          if(!hole[i])
          {
            for(int plane = 0; plane < n_planes; plane++) accum[plane] += vals[plane][i];
            n_anchor++;
          }
        }

      const size_t cell_index = (size_t)base_y * base_w + base_x;
      for(int plane = 0; plane < n_planes; plane++)
        base_vals[plane * cell_count + cell_index] = n_anchor ? (float)(accum[plane] / n_anchor) : 0.f;
      base_anchor[cell_index] = (2 * n_anchor > n_total);
    }

  // Pyramid depth (article step 3, "Convergence comes from the pyramid's depth"): the slowest Jacobi
  // error mode on a hole N cells wide decays in O(N^2) sweeps, so the coarsest grid must be small.
  // Halve until the LONG side is <= 8 cells. The coarsest level is seeded
  // with a flat anchor mean, and Jacobi needs ~O(N^2) sweeps to relax a flat seed on a hole
  // N cells wide -- so the coarsest grid must be small enough that the fixed per-level sweep
  // budget genuinely converges it; every finer level then only corrects local interpolation
  // error. (The previous short-side floor of 16 left elongated coarsest grids under-converged
  // on deep holes: pk1synth -22% RMSE, occluded -13% once actually converged.)
  int n_levels = 1;
  while((MAX(base_w, base_h) >> n_levels) > 8 && n_levels < 12) n_levels++;

  float *const restrict field = level_buffers + 0 * cell_count;              // n_planes planes, stride cell_count
  float *const restrict tmp = level_buffers + (size_t)n_planes * cell_count; // n_planes planes, stride cell_count
  float *const restrict level_vals
      = level_buffers + 2 * (size_t)n_planes * cell_count; // n_planes planes, stride cell_count

  int prev_level_w = 0;
  int prev_level_h = 0;

  for(int level = n_levels - 1; level >= 0; level--)
  {
    const int step = 1 << level;
    const int level_w = (base_w + step - 1) / step;
    const int level_h = (base_h + step - 1) / step;

    // downsample the base grid to this level (anchor-weighted mean + majority), into f/level_anchor
    HL_PFOR(collapse(2))
    for(int level_y = 0; level_y < level_h; level_y++)
      for(int level_x = 0; level_x < level_w; level_x++)
      {
        double accum[DT_HL_FILL_MAXP] = { 0.0 };
        int n_anchor = 0;
        int n_total = 0;

        for(int y = level_y * step; y < MIN((level_y + 1) * step, base_h); y++)
          for(int x = level_x * step; x < MIN((level_x + 1) * step, base_w); x++)
          {
            const size_t i = (size_t)y * base_w + x;
            n_total++;

            if(base_anchor[i])
            {
              for(int plane = 0; plane < n_planes; plane++) accum[plane] += base_vals[plane * cell_count + i];
              n_anchor++;
            }
          }

        const size_t cell_index = (size_t)level_y * level_w + level_x;
        for(int plane = 0; plane < n_planes; plane++)
          level_vals[plane * cell_count + cell_index] = n_anchor ? (float)(accum[plane] / n_anchor) : 0.f;
        level_anchor[cell_index] = (2 * n_anchor > n_total);
      }


    // aniso: level steering plane -> structure tensor (Weickert-stencil weights)
    float *const restrict level_steer = steered ? aniso_aux + 0 * cell_count : NULL;
    float *const restrict tensor_xx = steered ? aniso_aux + 1 * cell_count : NULL;
    float *const restrict tensor_xy = steered ? aniso_aux + 2 * cell_count : NULL;
    float *const restrict tensor_yy = steered ? aniso_aux + 3 * cell_count : NULL;

    if(steered)
    {
      HL_PFOR(collapse(2))
      for(int level_y = 0; level_y < level_h; level_y++)
        for(int level_x = 0; level_x < level_w; level_x++)
        {
          double steer_sum = 0.0;
          int n_total = 0;
          for(int y = level_y * step; y < MIN((level_y + 1) * step, base_h); y++)
            for(int x = level_x * step; x < MIN((level_x + 1) * step, base_w); x++)
            {
              steer_sum += base_steer[(size_t)y * base_w + x];
              n_total++;
            }
          level_steer[(size_t)level_y * level_w + level_x] = (float)(steer_sum / n_total);
        }

      // build the steering tensor D at this pyramid level from the downsampled L_mean plane
      _cf_adaptive_tensor(level_steer, tensor_xx, tensor_xy, tensor_yy, aniso_aux + 4 * cell_count,
                          aniso_aux + 6 * cell_count, level_w, level_h, DT_HL_CF_K);

      // The Weickert edge weights are constant across every sweep of this level (the tensor is
      // fixed): precompute the 8 weights per cell (interleaved) plus their sum once, so the
      // Jacobi inner loop is a pure multiply-accumulate. Same values, same accumulation order
      // as the previous inline computation -- the relaxation result is bit-identical.
      float *const restrict edge_weights = aniso_aux + 7 * cell_count;
      float *const restrict edge_weight_sum = aniso_aux + 15 * cell_count;
      HL_PFOR(collapse(2))
      for(int level_y = 0; level_y < level_h; level_y++)
        for(int level_x = 0; level_x < level_w; level_x++)
        {
          static const int neighbour_dy[8] = { 0, 0, -1, 1, -1, 1, 1, -1 };
          static const int neighbour_dx[8] = { -1, 1, 0, 0, -1, 1, -1, 1 };
          const size_t i = (size_t)level_y * level_w + level_x;
          float weight_sum = 0.f;
          for(int k = 0; k < 8; k++)
          {
            const int neighbour_y = CLAMP(level_y + neighbour_dy[k], 0, level_h - 1);
            const int neighbour_x = CLAMP(level_x + neighbour_dx[k], 0, level_w - 1);
            const size_t cell_index = (size_t)neighbour_y * level_w + neighbour_x;
            // w_ik: Weickert nonnegativity stencil weight for direction k, derived from D (>= 0)
            const float weight
                = _aniso_edge_w(tensor_xx, tensor_xy, tensor_yy, i, cell_index, neighbour_dx[k], neighbour_dy[k]);
            edge_weights[i * 8 + k] = weight;
            weight_sum += weight;
          }
          edge_weight_sum[i] = weight_sum;
        }
    }

    if(level == n_levels - 1)
    {
      // coarsest: seed the holes with the level's anchor mean, per plane (the flat starting state,
      // farthest from the solution; the pyramid depth guarantees Jacobi relaxes it within budget)
      double anchor_sum[DT_HL_FILL_MAXP] = { 0.0 };
      size_t anchor_count = 0;
      for(size_t i = 0; i < (size_t)level_w * level_h; i++)
        if(level_anchor[i])
        {
          for(int plane = 0; plane < n_planes; plane++) anchor_sum[plane] += level_vals[plane * cell_count + i];
          anchor_count++;
        }

      float anchor_mean[DT_HL_FILL_MAXP];
      for(int plane = 0; plane < n_planes; plane++)
        anchor_mean[plane] = anchor_count ? (float)(anchor_sum[plane] / (double)anchor_count) : 0.f;
      HL_PFOR()
      for(size_t i = 0; i < (size_t)level_w * level_h; i++)
        for(int plane = 0; plane < n_planes; plane++)
          tmp[plane * cell_count + i] = level_anchor[i] ? level_vals[plane * cell_count + i] : anchor_mean[plane];
    }
    else
    {
      // seed the holes from the coarser solution (bilinear), anchors from this level's means
      HL_PFOR(collapse(2))
      for(int level_y = 0; level_y < level_h; level_y++)
        for(int level_x = 0; level_x < level_w; level_x++)
        {
          const size_t i = (size_t)level_y * level_w + level_x;

          if(level_anchor[i])
          {
            for(int plane = 0; plane < n_planes; plane++)
              tmp[plane * cell_count + i] = level_vals[plane * cell_count + i];
            continue;
          }

          const float grid_x = ((float)level_x + 0.5f) * 0.5f - 0.5f;
          const float grid_y = ((float)level_y + 0.5f) * 0.5f - 0.5f;
          const int x_lo = CLAMP((int)floorf(grid_x), 0, prev_level_w - 1);
          const int y_lo = CLAMP((int)floorf(grid_y), 0, prev_level_h - 1);
          const int x_hi = MIN(x_lo + 1, prev_level_w - 1);
          const int y_hi = MIN(y_lo + 1, prev_level_h - 1);
          const float frac_x = CLAMP(grid_x - x_lo, 0.f, 1.f);
          const float frac_y = CLAMP(grid_y - y_lo, 0.f, 1.f);
          for(int plane = 0; plane < n_planes; plane++)
          {
            const float *const plane_field = field + plane * cell_count;
            const float interp_top = plane_field[(size_t)y_lo * prev_level_w + x_lo] * (1.f - frac_x)
                                     + plane_field[(size_t)y_lo * prev_level_w + x_hi] * frac_x;
            const float interp_bottom = plane_field[(size_t)y_hi * prev_level_w + x_lo] * (1.f - frac_x)
                                        + plane_field[(size_t)y_hi * prev_level_w + x_hi] * frac_x;
            tmp[plane * cell_count + i] = interp_top * (1.f - frac_y) + interp_bottom * frac_y;
          }
        }
    }

    for(int plane = 0; plane < n_planes; plane++)
      memcpy(field + plane * cell_count, tmp + plane * cell_count, (size_t)level_w * level_h * sizeof(float));

    // relaxation: iterate the div(D grad p)=0 Jacobi update to its fixed point on this level
    // (specialized on the plane count, see DEFINE_CF_FILL_RELAX)
    {
      const float *const restrict edge_weights = steered ? aniso_aux + 7 * cell_count : NULL;
      const float *const restrict edge_weight_sum = steered ? aniso_aux + 15 * cell_count : NULL;
      switch(n_planes)
      {
        case 1:
          _cf_fill_relax_1(field, tmp, level_anchor, edge_weights, edge_weight_sum, level_w, level_h, cell_count,
                           steered);
          break;
        case 2:
          _cf_fill_relax_2(field, tmp, level_anchor, edge_weights, edge_weight_sum, level_w, level_h, cell_count,
                           steered);
          break;
        case 3:
          _cf_fill_relax_3(field, tmp, level_anchor, edge_weights, edge_weight_sum, level_w, level_h, cell_count,
                           steered);
          break;
        default:
          _cf_fill_relax_4(field, tmp, level_anchor, edge_weights, edge_weight_sum, level_w, level_h, cell_count,
                           steered);
          break;
      }
    }

    prev_level_w = level_w;
    prev_level_h = level_h;
  }

  // upsample the base-grid coefficient solution into the full-res hole pixels by bilinear interp
  // (anchors keep their exact fitted values -- the Dirichlet data is never overwritten)
  HL_PFOR(collapse(2))
  for(int y = 0; y < region_h; y++)
    for(int x = 0; x < region_w; x++)
    {
      const size_t i = (size_t)y * region_w + x;

      if(!hole[i]) continue;

      const float grid_x = ((float)x + 0.5f) / downsample - 0.5f;
      const float grid_y = ((float)y + 0.5f) / downsample - 0.5f;
      const int x_lo = CLAMP((int)floorf(grid_x), 0, base_w - 1);
      const int y_lo = CLAMP((int)floorf(grid_y), 0, base_h - 1);
      const int x_hi = MIN(x_lo + 1, base_w - 1);
      const int y_hi = MIN(y_lo + 1, base_h - 1);
      const float frac_x = CLAMP(grid_x - x_lo, 0.f, 1.f);
      const float frac_y = CLAMP(grid_y - y_lo, 0.f, 1.f);
      for(int plane = 0; plane < n_planes; plane++)
      {
        const float *const plane_field = field + plane * cell_count;
        const float interp_top = plane_field[(size_t)y_lo * base_w + x_lo] * (1.f - frac_x)
                                 + plane_field[(size_t)y_lo * base_w + x_hi] * frac_x;
        const float interp_bottom = plane_field[(size_t)y_hi * base_w + x_lo] * (1.f - frac_x)
                                    + plane_field[(size_t)y_hi * base_w + x_hi] * frac_x;
        vals[plane][i] = interp_top * (1.f - frac_y) + interp_bottom * frac_y;
      }
    }

  dt_pixelpipe_cache_free_align(base_vals);
  dt_free_align(base_anchor);
  dt_pixelpipe_cache_free_align(level_buffers);
  dt_free_align(level_anchor);
  dt_pixelpipe_cache_free_align(aniso_aux);
}

// Single-plane convenience wrapper (isotropic callers and lone planes).
static void _cf_harmonic_fill(float *const restrict val, const uint8_t *const restrict hole, const int region_w,
                              const int region_h, const int base_ds, const float *const restrict steer,
                              const dt_dev_pixelpipe_t *pipe)
{
  float *plane_ptrs[1] = { val };
  _cf_harmonic_fill_n((float *const restrict *)plane_ptrs, 1, hole, region_w, region_h, base_ds, steer, pipe);
}

// MATHS/FLOW BRIDGE -- per-region reconstruction (article §"The algorithm", steps 3-8), the whole
// second half of the mermaid flowchart run once per merged region Omega on its PADDED read window
// (article §"The C production code": each region is cropped to region->rx0..ry1, reconstructed in a
// contiguous rw x rh buffer, then scattered back -- so the cost is linear in the padded area, not the
// image, article §"Linear in the padded area"). The stages compose as:
//   3 colour-line coefficient field   (_region fit+transport+eval block below; minimizes E_affine per
//                                       pixel, then transports the coefficients by E_transport) ->
//   4 HF refit                        (re-fits the high-frequency detail band on the same colour line) ->
//   5-6 soft floors + self-dome       (5: clip-level floor so a fit can only RAISE a saturated channel;
//                                       6: depth-gated blend of the guided estimate with a per-channel
//                                       biharmonic self-dome, weight We = R^4, minimizing E_bihar where
//                                       the colour line is weak) ->
//   7 all-clip luminance dome + chroma (E_bihar luminance dome shared by R,G,B, times an E_chrominance
//                                       screened-Poisson chromaticity fill, for pixels where NO channel
//                                       survives) ->
//   8 anisotropic chroma coherence    (final E_chrominance diffusion ironing the core<->annulus seam) ->
//   composite                          (scatter the reconstructed CLIPPED channels back, floored at 0).
// The region radius R (deepest clip-to-valid depth, from _segment_clipped_regions) sets the reach: the
// coarsest guided scale and the coefficient-field window sigma = clip(R/6, 8, 64) are both derived from
// it below, so the +-3 sigma window just reaches the deepest pixel and no farther. The internal step
// sections are annotated in place; this header only ties them together.
static void _region_guided_filter(float *const restrict interp, const float *const restrict mask,
                                  const float *const restrict depth, const int width,
                                  const _hl_region_t *const region, const dt_dev_pixelpipe_t *pipe,
                                  const float solid_color, const int max_iter, const float noise_level)
{
  const int region_w = region->rx1 - region->rx0 + 1;
  const int region_h = region->ry1 - region->ry0 + 1;
  if(region_w < 2 || region_h < 2) return;
  const size_t region_pixels = (size_t)region_w * region_h;
  // Sanity guard only (the pipe-cache arena handles memory): skip a pathologically huge region.
  // Normal clipped regions in a full raw stay well under this; keep it high so nothing is missed.
  if(region_pixels > (size_t)64 * 1024 * 1024) return;

  // Per-region padded-window working buffers (article §"Linear in the padded area"): every stage below
  // runs on these contiguous rw x rh arrays, not on the full image, so the whole reconstruction cost is
  // proportional to Sum_regions (padded area). All come from the pipe-cache arena and are freed at the
  // tail; the 4-channel packing is [R, G, B, norm] to match the interpolated input.
  float *const restrict estimate
      = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe); // running estimate (RGB+norm)
  float *const restrict prev_scale
      = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe); // snapshot at scale start
  float *const restrict valid
      = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe); // per-channel validity (0..1)
  float *const restrict blur_in
      = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe); // blur scratch (in)
  float *const restrict plane1
      = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe); // per-channel fit accumulator
  float *const restrict plane2
      = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe); // blur scratch (out)
  float *const restrict plane3
      = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe); // blur scratch (out)
  float *const restrict valid_variance
      = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe); // per-channel valid variance
  float *const restrict guide_score
      = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe); // per-channel best guide score
  float *const restrict clip_depth
      = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe); // per-pixel clip-to-valid depth
  float *const restrict clip0
      = dt_pixelpipe_cache_alloc_align_float(region_pixels * 4, pipe); // saturated (clipped) value per channel
  if(!estimate || !prev_scale || !valid || !blur_in || !plane1 || !plane2 || !plane3 || !valid_variance
     || !guide_score || !clip_depth || !clip0)
  {
    dt_pixelpipe_cache_free_align(estimate);
    dt_pixelpipe_cache_free_align(prev_scale);
    dt_pixelpipe_cache_free_align(valid);
    dt_pixelpipe_cache_free_align(blur_in);
    dt_pixelpipe_cache_free_align(plane1);
    dt_pixelpipe_cache_free_align(plane2);
    dt_pixelpipe_cache_free_align(plane3);
    dt_pixelpipe_cache_free_align(valid_variance);
    dt_pixelpipe_cache_free_align(guide_score);
    dt_pixelpipe_cache_free_align(clip_depth);
    dt_pixelpipe_cache_free_align(clip0);
    return;
  }

  // gather region into contiguous buffers
  HL_PFOR(collapse(2))
  for(int y = 0; y < region_h; y++)
    for(int x = 0; x < region_w; x++)
    {
      const size_t pixel_index = (size_t)(region->ry0 + y) * width + (region->rx0 + x);
      const size_t src_offset = pixel_index * 4;
      const size_t dst_offset = ((size_t)y * region_w + x) * 4;
      clip_depth[(size_t)y * region_w + x] = depth[pixel_index];
      for(int k = 0; k < 4; k++)
      {
        estimate[dst_offset + k] = interp[src_offset + k];
        clip0[dst_offset + k] = interp[src_offset + k]; // saturated value, physical floor for clipped ch.
        valid[dst_offset + k] = fmaxf(1.f - mask[src_offset + k], 0.f); // per-channel validity
      }
    }

  // Coarse -> fine sigma ladder, descending by 2x down to ~2 px. The COARSEST scale is the region's
  // reconstruction radius (deepest clip-to-valid distance) so the guided window just reaches the
  // deepest pixel -- the proven minimal-sufficient reach. Shallower pixels are gated to finer scales
  // below (per-pixel), so a small hole merged into a big tile is not over-reached. Floored at the
  // prototype's 40 px so tiny holes still get the reference ladder.
  const int extent = MAX(region->x1 - region->x0, region->y1 - region->y0) + 1;
  const float epsilon = 1e-6f;
  _hl_blur_seconds = 0.0; // PERF (DT_DEBUG_PERF): reset per-thread blur accumulator

  const double _thl1 = dt_get_wtime(); // PERF (DT_DEBUG_PERF): guided ladder done

  // A clipped channel saturated, so its true value is >= its clip level: floor the reconstruction at
  // the saturated value so a low-guide fit cannot push it below saturation (the amber -> magenta
  // collapse). Monotone (only raises), so no overshoot and no per-pixel switching. Applied before the
  // joint core, so the all-clip dome and chroma diffusion are fed the corrected (brighter) rim.
  HL_PFOR()
  for(size_t i = 0; i < region_pixels; i++)
    for(int c = 0; c < 3; c++)
      if(valid[i * 4 + c] < 0.5f) estimate[i * 4 + c] = fmaxf(estimate[i * 4 + c], clip0[i * 4 + c]);

  // ===== biharmonic + chroma refinement =====================================================
  //  Where a colour-line survives, blend the guided estimate with a per-channel biharmonic
  //  self-dome by the squared confidence We = R^4 (correlated -> guide; decorrelated -> smooth
  //  own gradient). Where NO channel survives (all-clipped core), rebuild one shared luminance
  //  dome times a harmonic-diffused chromaticity (fixes the magenta core). Finally iron out the
  //  seams with an uncertainty-weighted biharmonic pass. All solves are matrix-free CG on the
  //  small region. See the companion article and fix_prototype.py.
  // Full-resolution biharmonic domes + harmonic chroma diffusion, all via matrix-free CG (stops
  // early on convergence). The chroma diffusion MUST converge for the all-clip core to take the rim
  // hue -- otherwise the deepest centre keeps its initial magenta -- so it uses the full natural
  // budget, NOT the "iterations" perf cap. iterations caps only the seam regulariser (a refinement).
  const int max_cg_iter = CLAMP(2 * extent, 200, 2000);
  // The prototype solves the seam regulariser with a direct sparse solve (exact). C has no sparse
  // direct solver, so run the FULL CG budget instead of capping at the user "iterations" param:
  // an under-converged biharmonic CG stops each channel at a different point -> per-channel
  // inconsistency -> chroma drift. maxit (not max_iter) is the honest best-effort here.
  (void)max_iter;
  uint8_t *const restrict hole = (uint8_t *)dt_alloc_align(sizeof(uint8_t) * region_pixels);
  float *const restrict solver_field
      = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe); // solver working field
  float *const restrict fill_planes
      = dt_pixelpipe_cache_alloc_align_float(region_pixels * 3, pipe);                        // fused-fill planes
  float *const restrict dome_lum = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe); // luminance dome
  float *const restrict lum_accum
      = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe); // luminance accum (chroma denom)
  float *const restrict reaction_weight
      = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe); // chroma reaction weight
  float *const restrict flat_target
      = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe); // chroma flat target
  float *const restrict cg_residual = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe); // CG scratch
  float *const restrict cg_dir = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
  float *const restrict cg_operator = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
  float *const restrict cg_tmp1 = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
  float *const restrict cg_tmp2 = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
  if(hole && solver_field && fill_planes && dome_lum && lum_accum && reaction_weight && flat_target && cg_residual
     && cg_dir && cg_operator && cg_tmp1 && cg_tmp2)
  {
    // ===== coefficient-field reconstruction (see the DT_HL_COEFF_FIELD macro comment) =====
    // MATHS BRIDGE -- article "The algorithm" step 3, "The coefficient field". The windowed weighted
    // least squares (a,b,d)(x) = argmin_{a,b,d} Sum_y w(y) G_sigma(x-y) (v(y) - a u1(y) - b u2(y) - d)^2,
    // with v the clipped channel, u1/u2 its two guides, w the trust mask (all channels valid), and
    // G_sigma a Gaussian window at a single scale sigma = clip(r/6, 8, 64). The evaluation is
    // v_hat(x) = a(x) u1(x) + b(x) u2(x) + d(x). Rather than solve per pixel, this fits from TEN blurred
    // moment planes (1 trusted-mass count + 3 means + 6 second moments, gathered in three 4-channel
    // Gaussian blurs) through the 2x2 normal equations, then TRANSPORTS the coefficient planes into the
    // hole with _cf_harmonic_fill (E_transport) before evaluating against the measured guides.
    // This block owns the whole fit+transport+evaluation; the HF-refit, self-dome and core stages follow.
    {
      const float cf_sigma = CLAMP(region->radius / 6.f, 8.f, 64.f); // sigma = clip(r/6, 8, 64): +/-3 sigma window reaches the deepest pixel; floor/cap bound samples/cost
      const float cf_fmin = 0.05f;

      // region luminance + the blown zone's plateau level, for the occlusion-aware fills
      HL_PFOR()
      for(size_t i = 0; i < region_pixels; i++)
        lum_accum[i] = estimate[i * 4 + 0] + estimate[i * 4 + 1] + estimate[i * 4 + 2];

      double laccum = 0.0;
      size_t lcnt = 0;
      HL_PFOR(reduction(+ : laccum, lcnt))
      for(size_t i = 0; i < region_pixels; i++)
        if(valid[i * 4 + 0] < 0.5f || valid[i * 4 + 1] < 0.5f || valid[i * 4 + 2] < 0.5f)
        {
          laccum += lum_accum[i];
          lcnt++;
        }

      const float cf_lref = lcnt ? (float)(laccum / (double)lcnt) : 0.f;

      // Steering plane for the coefficient fills = the measured guide structure.
      // Mean of the VALID channels where at least one survives (real data inside the
      // partial-clip zone); the flat plateau mean elsewhere (all-clip core), where a flat
      // steer degenerates the tensor to identity, i.e. back to the isotropic fill.
      float *const restrict steer = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
      if(steer)
      {
        HL_PFOR()
        for(size_t i = 0; i < region_pixels; i++)
        {
          float accum = 0.f;
          int n_valid = 0;
          for(int c = 0; c < 3; c++)
            if(valid[i * 4 + c] >= 0.5f)
            {
              accum += estimate[i * 4 + c];
              n_valid++;
            }
          steer[i] = n_valid ? accum / n_valid : lum_accum[i] / 3.f;
        }
      }

      // Per-channel means of the VALID values: the moment packs below are CENTERED on them.
      // var = E[u^2] - E[u]^2 in float32 on a smooth plane cancels catastrophically (the mean
      // squared dwarfs the variance, ~4 digits lost) and the fit's cov/var division amplifies
      // the surviving noise -- measured as a device-dependent slope error growing with depth.
      // Centering the packs makes the blurred moments carry the (co)variances directly ; the
      // slopes and R^2 are shift-invariant, and the intercept is unshifted right after the fit.
      double maccum[3] = { 0.0, 0.0, 0.0 };
      size_t mcnt[3] = { 0, 0, 0 };
      HL_PFOR(reduction(+ : maccum[:3], mcnt[:3]))
      for(size_t i = 0; i < region_pixels; i++)
        for(int c = 0; c < 3; c++)
          if(valid[i * 4 + c] >= 0.5f)
          {
            maccum[c] += estimate[i * 4 + c];
            mcnt[c]++;
          }
      const float channel_means[3]
          = { mcnt[0] ? (float)(maccum[0] / mcnt[0]) : 0.f, mcnt[1] ? (float)(maccum[1] / mcnt[1]) : 0.f,
              mcnt[2] ? (float)(maccum[2] / mcnt[2]) : 0.f };

      // Soft luminance affinity for the FIT WINDOWS: pixels much darker than the blown zone's
      // plateau contribute ~nothing to the windowed moments, so a window straddling a dark
      // occluder and the sky fits the SKY's colour-line instead of a poisoned mixture. Content
      // brighter than ~a third of the plateau keeps full weight, so unoccluded scenes are
      // untouched by construction.
      const float cf_binv = (cf_lref > 1e-9f) ? 1.f / (0.35f * cf_lref) : 0.f;

      // broad-anchor mask for the model-quality plane (bounded even where the fit degenerates)
      uint8_t *const restrict hole2 = (uint8_t *)dt_alloc_align(sizeof(uint8_t) * region_pixels);

      // bsc collects the DIFFUSED fit quality (R^2) per channel: the weight of the
      // high-frequency damping and of the depth-gated self-dome blend below. It is spatially
      // smooth (windowed moments + harmonic fill), so neither conaccumer introduces a hand-off.
      HL_PFOR()
      for(size_t i = 0; i < region_pixels; i++)
        for(int c = 0; c < 4; c++) guide_score[i * 4 + c] = 0.f;

      // The TEN blurred moment planes of the fit (article step 3: "solved from ten blurred moment
      // planes ... through the 2x2 normal equations"). Each _region_blur below IS the windowed weighted
      // sum Sum_y w(y) G_sigma(x-y) (.) : packing the per-pixel product then Gaussian-blurring gives the
      // windowed moment at x. w(y) = [all three channels valid] * lum_weight (the soft occlusion weight).
      // Moment 1 of 3 (blur -> prev_scale): the mass count n and the 3 centred means.
      // joint windowed moments, weight = all three channels valid at the pixel, packed as
      // prev = [n, wR, wG, wB], s1 = [wRR, wGG, wBB, wRG], s3 = [wRB, wGB, 0, 0]
      HL_PFOR()
      for(size_t i = 0; i < region_pixels; i++)
      {
        const float lum_weight = (cf_binv > 0.f) ? sqf(fminf(lum_accum[i] * cf_binv, 1.f)) : 1.f;
        const float weight = (valid[i * 4 + 0] >= 0.5f && valid[i * 4 + 1] >= 0.5f && valid[i * 4 + 2] >= 0.5f)
                                 ? lum_weight
                                 : 0.f;
        blur_in[i * 4 + 0] = weight;                                     // Sum w -> n (trusted mass)
        blur_in[i * 4 + 1] = weight * (estimate[i * 4 + 0] - channel_means[0]); // Sum w*(R-Rbar) -> centred mean of R
        blur_in[i * 4 + 2] = weight * (estimate[i * 4 + 1] - channel_means[1]); // Sum w*(G-Gbar) -> centred mean of G
        blur_in[i * 4 + 3] = weight * (estimate[i * 4 + 2] - channel_means[2]); // Sum w*(B-Bbar) -> centred mean of B
      }

      _region_blur(blur_in, prev_scale, region_w, region_h, cf_sigma);

      HL_PFOR()
      for(size_t i = 0; i < region_pixels; i++)
      {
        const float lum_weight = (cf_binv > 0.f) ? sqf(fminf(lum_accum[i] * cf_binv, 1.f)) : 1.f;
        const float weight = (valid[i * 4 + 0] >= 0.5f && valid[i * 4 + 1] >= 0.5f && valid[i * 4 + 2] >= 0.5f)
                                 ? lum_weight
                                 : 0.f;
        // Moment 2 of 3 (blur -> plane1): four of the six centred second moments (products of x-xbar)
        const float val_r = estimate[i * 4 + 0] - channel_means[0]; // R - Rbar (centred, avoids the E[u^2]-E[u]^2 cancellation)
        const float val_g = estimate[i * 4 + 1] - channel_means[1]; // G - Gbar
        const float val_b = estimate[i * 4 + 2] - channel_means[2]; // B - Bbar
        blur_in[i * 4 + 0] = weight * val_r * val_r; // -> E[(R-Rbar)^2] = Var(R)
        blur_in[i * 4 + 1] = weight * val_g * val_g; // -> Var(G)
        blur_in[i * 4 + 2] = weight * val_b * val_b; // -> Var(B)
        blur_in[i * 4 + 3] = weight * val_r * val_g; // -> Cov(R,G)
      }

      _region_blur(blur_in, plane1, region_w, region_h, cf_sigma);

      HL_PFOR()
      for(size_t i = 0; i < region_pixels; i++)
      {
        const float lum_weight = (cf_binv > 0.f) ? sqf(fminf(lum_accum[i] * cf_binv, 1.f)) : 1.f;
        const float weight = (valid[i * 4 + 0] >= 0.5f && valid[i * 4 + 1] >= 0.5f && valid[i * 4 + 2] >= 0.5f)
                                 ? lum_weight
                                 : 0.f;
        // Moment 3 of 3 (blur -> plane3): the last two centred second moments + the unweighted mass
        blur_in[i * 4 + 0]
            = weight * (estimate[i * 4 + 0] - channel_means[0]) * (estimate[i * 4 + 2] - channel_means[2]); // -> Cov(R,B)
        blur_in[i * 4 + 1]
            = weight * (estimate[i * 4 + 1] - channel_means[1]) * (estimate[i * 4 + 2] - channel_means[2]); // -> Cov(G,B)
        blur_in[i * 4 + 2] = (valid[i * 4 + 0] >= 0.5f && valid[i * 4 + 1] >= 0.5f && valid[i * 4 + 2] >= 0.5f)
                                 ? 1.f
                                 : 0.f; // UNWEIGHTED valid mass: anchors must exist at the rim
        blur_in[i * 4 + 3] = 0.f;
      }

      _region_blur(blur_in, plane3, region_w, region_h, cf_sigma);

      // second-moment plane lookup: diag (c,c) -> s1 slot c; off-diag (a,b) -> slot 2+a+b,
      // where slots 3 = RG (s1), 4 = RB (s3[0]), 5 = GB (s3[1])
      // CF_M2(i, a, b) returns the (unnormalized) windowed sum Sum w*(u_a-ubar_a)(u_b-ubar_b) at pixel i,
      // i.e. n*Cov(u_a,u_b) once divided by the mass n -- the raw material of the normal matrix Sigma.
#define CF_M2(nb_index, coef_a, coef_b)                                                                           \
  (((coef_a) == (coef_b)) ? plane1[(nb_index) * 4 + (coef_a)]                                                     \
                          : ((2 + (coef_a) + (coef_b)) < 4 ? plane1[(nb_index) * 4 + 2 + (coef_a) + (coef_b)]     \
                                                           : plane3[(nb_index) * 4 + (coef_a) + (coef_b) - 2]))

      // The DEEP channel (most clipped pixels: its zone contains the multi-clip cores) is not
      // evaluated here -- its diffused coefficients are STASHED and evaluated after the pair
      // fallbacks have reconstructed the other clipped channels, so its joint model reads
      // CONTINUOUS guides everywhere. Evaluating it against a guide that jumps from measured
      // to clip-plateau at the guide's own clip contour printed that contour as an arc.
      size_t nclip_c[3] = { 0, 0, 0 };
      for(size_t i = 0; i < region_pixels; i++)
        for(int c = 0; c < 3; c++)
          if(valid[i * 4 + c] < 0.5f) nclip_c[c]++;

      // cdeep = the channel with the most clipped pixels (its zone holds the multi-clip cores)
      const int cdeep
          = (nclip_c[0] >= nclip_c[1] && nclip_c[0] >= nclip_c[2]) ? 0 : ((nclip_c[1] >= nclip_c[2]) ? 1 : 2);
      int deep_stashed = 0;

      // ---- per channel: joint 2-guide coefficients, harmonic diffusion, evaluation ----
      for(int c = 0; c < 3; c++)
      {
        // guide-pair selection: predict clipped channel v = c from its two OTHER channels u1=guide1, u2=guide2
        const int guide1 = (c == 0) ? 1 : 0;
        const int guide2 = (c == 2) ? 1 : 2;

        size_t ntarget = 0;
        HL_PFOR(reduction(+ : ntarget))
        for(size_t i = 0; i < region_pixels; i++)
          if(valid[i * 4 + c] < 0.5f && (valid[i * 4 + guide1] >= 0.5f || valid[i * 4 + guide2] >= 0.5f))
            ntarget++;

        if(ntarget == 0) continue;

        // NOTE the ntarget gate accepts ONE surviving guide so the DEEP channel is fitted
        // (and stashed) even when its zone has no strict two-guide pixel; the immediate
        // evaluation below stays strict.

        // coefficients (a, b, d) from the windowed moments at every pixel (garbage where the
        // window held no trusted mass -- replaced by the diffusion); anchor = trusted window
        // Solve the 2x2 normal equations of the weighted least squares at every pixel (article step 3):
        //   Sigma [a;b] = [Cov(u1,v); Cov(u2,v)],  Sigma = [[Var u1, Cov(u1,u2)],[Cov(u1,u2), Var u2]]
        // via Cramer's rule, with relative Tikhonov ridge lambda added to the diagonal.
        HL_PFOR()
        for(size_t i = 0; i < region_pixels; i++)
        {
          const float norm = fmaxf(prev_scale[i * 4 + 0], 1e-9f); // n = windowed trusted mass
          const float inv_det = 1.f / norm;                       // 1/n, turns the summed moments into expectations
          const float mean1 = prev_scale[i * 4 + 1 + guide1] * inv_det;      // E[u1] (of the centred pack)
          const float mean2 = prev_scale[i * 4 + 1 + guide2] * inv_det;      // E[u2]
          const float mean_target = prev_scale[i * 4 + 1 + c] * inv_det;     // E[v]
          const float var11 = fmaxf(CF_M2(i, guide1, guide1) * inv_det - mean1 * mean1, 0.f); // Var(u1) = E[u1^2]-E[u1]^2
          const float var22 = fmaxf(CF_M2(i, guide2, guide2) * inv_det - mean2 * mean2, 0.f); // Var(u2)
          const float var12 = CF_M2(i, guide1, guide2) * inv_det - mean1 * mean2;             // Cov(u1,u2)
          const float cov_tg1 = CF_M2(i, c, guide1) * inv_det - mean_target * mean1;          // Cov(v,u1) = RHS_1
          const float cov_tg2 = CF_M2(i, c, guide2) * inv_det - mean_target * mean2;          // Cov(v,u2) = RHS_2

          const float var_target = fmaxf(CF_M2(i, c, c) * inv_det - mean_target * mean_target, 0.f); // Var(v), denom of R^2

          // relative Tikhonov: scales with the signal, never eats a weak-but-real slope
          const float lambda = 1e-3f * 0.5f * (var11 + var22) + 1e-12f;                     // ridge = 1e-3 * (Var u1 + Var u2)/2
          const float determinant = fmaxf((var11 + lambda) * (var22 + lambda) - var12 * var12, 1e-18f); // det Sigma (with ridge)
          const float slope_a = ((var22 + lambda) * cov_tg1 - var12 * cov_tg2) / determinant; // a = (Sigma^-1 RHS)_1 (Cramer)
          const float slope_b = ((var11 + lambda) * cov_tg2 - var12 * cov_tg1) / determinant; // b = (Sigma^-1 RHS)_2 (Cramer)
          const float r_sq = CLAMP((slope_a * cov_tg1 + slope_b * cov_tg2) / (var_target + 1e-12f), 0.f, 1.f); // R^2 = (a Cov(v,u1)+b Cov(v,u2)) / Var(v) = explained/total

          valid_variance[i * 4 + 0] = slope_a;
          valid_variance[i * 4 + 1] = slope_b;
          // intercept of the CENTERED fit, unshifted back to absolute values: d = E[v] - a E[u1] - b E[u2]
          valid_variance[i * 4 + 2] = (mean_target + channel_means[c]) - slope_a * (mean1 + channel_means[guide1])
                                      - slope_b * (mean2 + channel_means[guide2]);
          valid_variance[i * 4 + 3] = r_sq;

          // anchor = trusted window AND a sane fit: degenerate (near-zero-variance) windows
          // produce exploding slopes that would poison the diffusion boundary
          // anchors EXIST wherever enough valid pixels are in reach (continuity at the rim
          // needs locally-exact fits there), and their weighted fits are bright-content-pure;
          // windows that are MOSTLY dark (weighted mass a small fraction of the valid mass)
          // describe unrelated content and must not anchor
          const int mass_ok = (plane3[i * 4 + 2] > cf_fmin && prev_scale[i * 4 + 0] > 0.25f * plane3[i * 4 + 2]);
          // anchor gate (article: R^2 > 0.25 with bounded slopes) -> the Dirichlet data for E_transport.
          // hole = NOT an anchor (the cell to be filled by the transport); |a|,|b| < 64 rejects only
          // degenerate near-zero-variance windows whose exploding slopes would poison the fill boundary.
          hole[i] = !(mass_ok && valid[i * 4 + c] >= 0.5f && r_sq > 0.25f && fabsf(slope_a) < 64.f
                      && fabsf(slope_b) < 64.f);
          if(hole2) hole2[i] = !(mass_ok && valid[i * 4 + c] >= 0.5f); // broader (mass-only) anchor set for the R^2 plane

        }

        // harmonic diffusion of each coefficient field into the non-anchor area (stable
        // coarse-to-fine Jacobi fill; base grid at ~sigma/4 since coefficients are smooth).
        // a/b/d share the anchor mask, so they ride ONE fused fill (one mask pyramid, one
        // tensor, one sweep pass); r2 may use its own broader mask and fills alone.
        {
          HL_PFOR()
          for(size_t i = 0; i < region_pixels; i++)
          {
            fill_planes[i] = valid_variance[i * 4 + 0];
            fill_planes[region_pixels + i] = valid_variance[i * 4 + 1];
            fill_planes[2 * region_pixels + i] = valid_variance[i * 4 + 2];
            solver_field[i] = valid_variance[i * 4 + 3];
          }

          // E_transport on p in {a, b, d}: anchored anisotropic fill, base grid pitch ~sigma/4 (article "Cell")
          float *planes[3] = { fill_planes, fill_planes + region_pixels, fill_planes + 2 * region_pixels };
          _cf_harmonic_fill_n((float *const restrict *)planes, 3, hole, region_w, region_h, (int)(cf_sigma / 4.f),
                              steer, pipe);
          // the R^2 plane is diffused too (article: "R^2 is diffused alongside (a,b,d) as a fourth plane"),
          // on the broader mass-only anchor set so it stays bounded even where the fit degenerates
          _cf_harmonic_fill(solver_field, hole2 ? hole2 : hole, region_w, region_h, (int)(cf_sigma / 4.f), steer,
                            pipe);

          HL_PFOR()
          for(size_t i = 0; i < region_pixels; i++)
          {
            valid_variance[i * 4 + 0] = fill_planes[i];
            valid_variance[i * 4 + 1] = fill_planes[region_pixels + i];
            valid_variance[i * 4 + 2] = fill_planes[2 * region_pixels + i];
            valid_variance[i * 4 + 3] = solver_field[i];
          }
        }

        // evaluate against the measured guides at every joint target pixel, keeping the
        // diffused fit R^2 (in-sample R^2 is the honest quality signal: decorrelated content
        // simply has no colour-line and scores 0.25..0.6 against ~0.9 for correlated content)
        if(c == cdeep)
        {
          // stash the diffused fields (dbuf/tbuf/ldb/bsc slot 3 are free until the HF and
          // dome stages); evaluated after the pair fallbacks below
          HL_PFOR()
          for(size_t i = 0; i < region_pixels; i++)
          {
            reaction_weight[i] = valid_variance[i * 4 + 0];
            flat_target[i] = valid_variance[i * 4 + 1];
            dome_lum[i] = valid_variance[i * 4 + 2];
            guide_score[i * 4 + 3] = valid_variance[i * 4 + 3];
          }
          deep_stashed = 1;
          continue;
        }

        // strict two-guide gate: extending this evaluation into the multi-clip band (with the
        // clipped guide at its plateau) was tried and regressed the correlated synthetics --
        // continuity there is the deep channel's deferred evaluation's job, and the non-deep
        // channels' pair fits are locally anchored at their own fences anyway
        HL_PFOR()
        for(size_t i = 0; i < region_pixels; i++)
          if(valid[i * 4 + c] < 0.5f && valid[i * 4 + guide1] >= 0.5f && valid[i * 4 + guide2] >= 0.5f)
          {
            // evaluation v_hat = a*u1 + b*u2 + d against the MEASURED guides (diffused a,b,d; true u1,u2)
            estimate[i * 4 + c] = valid_variance[i * 4 + 0] * estimate[i * 4 + guide1]
                                  + valid_variance[i * 4 + 1] * estimate[i * 4 + guide2]
                                  + valid_variance[i * 4 + 2];
            guide_score[i * 4 + c] = CLAMP(valid_variance[i * 4 + 3], 0.f, 1.f); // carry the diffused R^2 as the model quality
          }
      }

#undef CF_M2

      // ---- single-guide fallback for 2-clip pixels (target + one other channel clipped) ----
      // Article step 3: "Pixels with a single surviving guide get the same treatment with a one-guide
      // fit." Same fit+transport+evaluate, but the model collapses to v_hat = a*u + d (one guide u):
      // a = Cov(u,v)/Var(u) (1x1 normal equation), R^2 = Cov(u,v)^2 / (Var(u) Var(v)) = squared correlation.
      size_t n2clip = 0;
      HL_PFOR(reduction(+ : n2clip))
      for(size_t i = 0; i < region_pixels; i++)
      {
        const int n_valid = (valid[i * 4 + 0] >= 0.5f) + (valid[i * 4 + 1] >= 0.5f) + (valid[i * 4 + 2] >= 0.5f);
        if(n_valid == 1) n2clip++;
      }

      if(n2clip > 0)
        for(int chan_a = 0; chan_a < 3; chan_a++)
          for(int chan_b = chan_a + 1; chan_b < 3; chan_b++)
          {
            // pair moments, weight = both channels of the pair valid, packed as
            // s2 = [n, wa, wb, waa], s3 = [wbb, wab, unweighted n, 0]
            HL_PFOR()
            for(size_t i = 0; i < region_pixels; i++)
            {
              const float lum_weight = (cf_binv > 0.f) ? sqf(fminf(lum_accum[i] * cf_binv, 1.f)) : 1.f;
              const float weight
                  = (valid[i * 4 + chan_a] >= 0.5f && valid[i * 4 + chan_b] >= 0.5f) ? lum_weight : 0.f;
              const float var_a = estimate[i * 4 + chan_a] - channel_means[chan_a];
              const float var_b = estimate[i * 4 + chan_b] - channel_means[chan_b];
              blur_in[i * 4 + 0] = weight;
              blur_in[i * 4 + 1] = weight * var_a;
              blur_in[i * 4 + 2] = weight * var_b;
              blur_in[i * 4 + 3] = weight * var_a * var_a;
            }

            _region_blur(blur_in, plane2, region_w, region_h, cf_sigma);

            HL_PFOR()
            for(size_t i = 0; i < region_pixels; i++)
            {
              const float lum_weight = (cf_binv > 0.f) ? sqf(fminf(lum_accum[i] * cf_binv, 1.f)) : 1.f;
              const float weight
                  = (valid[i * 4 + chan_a] >= 0.5f && valid[i * 4 + chan_b] >= 0.5f) ? lum_weight : 0.f;
              const float var_a = estimate[i * 4 + chan_a] - channel_means[chan_a];
              const float var_b = estimate[i * 4 + chan_b] - channel_means[chan_b];
              blur_in[i * 4 + 0] = weight * var_b * var_b;
              blur_in[i * 4 + 1] = weight * var_a * var_b;
              blur_in[i * 4 + 2] = (valid[i * 4 + chan_a] >= 0.5f && valid[i * 4 + chan_b] >= 0.5f) ? 1.f : 0.f;
              blur_in[i * 4 + 3] = 0.f;
            }

            _region_blur(blur_in, plane3, region_w, region_h, cf_sigma);

            // both orientations: predict a from b, then b from a
            for(int orient = 0; orient < 2; orient++)
            {
              const int target_chan = orient ? chan_b : chan_a; // target channel
              const int guide_chan = orient ? chan_a : chan_b;  // guide channel
              const int other_chan = 3 - chan_a - chan_b;       // the third channel, must be clipped at the target

              size_t ntarget = 0;
              HL_PFOR(reduction(+ : ntarget))
              for(size_t i = 0; i < region_pixels; i++)
                if(valid[i * 4 + target_chan] < 0.5f && valid[i * 4 + guide_chan] >= 0.5f
                   && valid[i * 4 + other_chan] < 0.5f)
                  ntarget++;

              if(ntarget == 0) continue;

              HL_PFOR()
              for(size_t i = 0; i < region_pixels; i++)
              {
                const float norm = fmaxf(plane2[i * 4 + 0], 1e-9f);
                const float inv_det = 1.f / norm;
                const float pair_mean_target = plane2[i * 4 + (orient ? 2 : 1)] * inv_det;
                const float mean_guide = plane2[i * 4 + (orient ? 1 : 2)] * inv_det;
                const float var_guide = fmaxf(
                    (orient ? plane2[i * 4 + 3] : plane3[i * 4 + 0]) * inv_det - mean_guide * mean_guide, 0.f); // Var(u) (guide)
                const float var_t = fmaxf((orient ? plane3[i * 4 + 0] : plane2[i * 4 + 3]) * inv_det
                                              - pair_mean_target * pair_mean_target,
                                          0.f); // Var(v) (target), denom of R^2
                const float covariance = plane3[i * 4 + 1] * inv_det - pair_mean_target * mean_guide; // Cov(u,v)
                const float slope_a = covariance / (var_guide * (1.f + 1e-3f) + 1e-12f); // a = Cov(u,v)/Var(u), 1e-3 relative ridge
                const float r_sq = CLAMP(covariance * covariance / (var_guide * var_t + 1e-18f), 0.f, 1.f); // R^2 = Cov^2/(Var u Var v)

                valid_variance[i * 4 + 0] = slope_a;
                // intercept of the CENTERED fit, unshifted back to absolute values: d = E[v] - a E[u]
                valid_variance[i * 4 + 1] = (pair_mean_target + channel_means[target_chan])
                                            - slope_a * (mean_guide + channel_means[guide_chan]);
                valid_variance[i * 4 + 2] = r_sq;
                const int mass_ok = (plane3[i * 4 + 2] > cf_fmin && plane2[i * 4 + 0] > 0.25f * plane3[i * 4 + 2]);
                hole[i]
                    = !(mass_ok && valid[i * 4 + target_chan] >= 0.5f && r_sq > 0.25f && fabsf(slope_a) < 64.f);
                if(hole2) hole2[i] = !(mass_ok && valid[i * 4 + target_chan] >= 0.5f);
              }

              // slope and intercept share the anchor mask -> one fused fill; r2 may use its
              // own broader mask and fills alone
              {
                HL_PFOR()
                for(size_t i = 0; i < region_pixels; i++)
                {
                  fill_planes[i] = valid_variance[i * 4 + 0];
                  fill_planes[region_pixels + i] = valid_variance[i * 4 + 1];
                  solver_field[i] = valid_variance[i * 4 + 2];
                }

                float *planes[2] = { fill_planes, fill_planes + region_pixels };
                _cf_harmonic_fill_n((float *const restrict *)planes, 2, hole, region_w, region_h,
                                    (int)(cf_sigma / 4.f), steer, pipe);
                _cf_harmonic_fill(solver_field, hole2 ? hole2 : hole, region_w, region_h, (int)(cf_sigma / 4.f),
                                  steer, pipe);

                HL_PFOR()
                for(size_t i = 0; i < region_pixels; i++)
                {
                  valid_variance[i * 4 + 0] = fill_planes[i];
                  valid_variance[i * 4 + 1] = fill_planes[region_pixels + i];
                  valid_variance[i * 4 + 2] = solver_field[i];
                }
              }

              // FEATHERED hand-off: instead of switching hard to the pair model exactly where
              // the third channel clips (its contour prints the joint/pair disagreement as an
              // arc), blend by the blurred oc-clip mask -- ~0 far into the joint region, ~1
              // deep into the multi-clip band, ~0.5 at the contour where BOTH estimates are
              // continuous extrapolations. est currently holds the extended joint estimate.
              // (A sharper ramp was tried and regressed the outer-contour smoothness.)
              // hard write at the multi-clip pixels (the iter-3 semantics). For the deep
              // channel this is only the DEEP-CORE estimate: the deferred stashed-joint
              // evaluation below owns the fence and blends this back in by depth. For the
              // other channels the pair fit is locally anchored at their fence (both its
              // channels are measured in the adjacent band), so the hard write is already
              // continuous there. A feathered joint-ext blend over this write was tried and
              // regressed the correlated synthetics without helping the arc.
              HL_PFOR()
              for(size_t i = 0; i < region_pixels; i++)
                if(valid[i * 4 + target_chan] < 0.5f && valid[i * 4 + guide_chan] >= 0.5f
                   && valid[i * 4 + other_chan] < 0.5f)
                {
                  // evaluation v_hat = a*u + d against the measured guide (diffused a,d; true u)
                  estimate[i * 4 + target_chan]
                      = valid_variance[i * 4 + 0] * estimate[i * 4 + guide_chan] + valid_variance[i * 4 + 1];
                  guide_score[i * 4 + target_chan] = CLAMP(valid_variance[i * 4 + 2], 0.f, 1.f);
                }
            }
          }

      // ---- deep-channel evaluation from the stashed joint model ----
      // Runs after the pair fallbacks so a clipped guide reads as its RECONSTRUCTION (itself
      // continuous: the pair fit of a less-clipped channel is anchored in the adjacent band
      // where both its channels are measured). Smooth coefficient fields x continuous guides
      // = no estimator hand-off anywhere inside the deep channel's zone -- the arc the hard
      // joint <-> pair switch used to print at the second guide's clip contour cannot form.
      // DEPTH SPLIT: the chained evaluation is only NEEDED near the multi-clip fence; deep
      // inside the core the direct pair colour-line is the better estimator on correlated
      // content (one hop, no compounded reconstruction error). Blend pair over stashed-joint
      // by a smoothstep of the blurred multi-clip mask: ~0 at the fence (mask ~0.5 there),
      // ~1 deep inside. Smooth weight x smooth fields = still no printable level set.
      if(deep_stashed)
      {
        const int guide1 = (cdeep == 0) ? 1 : 0;
        const int guide2 = (cdeep == 2) ? 1 : 2;

        HL_PFOR()
        for(size_t i = 0; i < region_pixels; i++)
        {
          blur_in[i * 4 + 0]
              = (valid[i * 4 + cdeep] < 0.5f && (valid[i * 4 + guide1] < 0.5f || valid[i * 4 + guide2] < 0.5f))
                    ? 1.f
                    : 0.f;
          blur_in[i * 4 + 1] = blur_in[i * 4 + 2] = blur_in[i * 4 + 3] = 0.f;
        }

        _region_blur(blur_in, plane2, region_w, region_h,
                     cf_sigma); // s2 is free scratch here (pair moments are done)

        HL_PFOR()
        for(size_t i = 0; i < region_pixels; i++)
        {
          const int anyvalid
              = (valid[i * 4 + 0] >= 0.5f) || (valid[i * 4 + 1] >= 0.5f) || (valid[i * 4 + 2] >= 0.5f);
          if(valid[i * 4 + cdeep] < 0.5f && anyvalid)
          {
            // deferred evaluation of the stashed deep-channel joint model: v_hat = a*u1 + b*u2 + d
            // (a=reaction_weight, b=flat_target, d=dome_lum are the stashed diffused coefficients)
            const float joint = reaction_weight[i] * estimate[i * 4 + guide1]
                                + flat_target[i] * estimate[i * 4 + guide2] + dome_lum[i];
            // pair values exist only at multi-clip px (the pair loop's write gate)
            const int has_pair = (valid[i * 4 + guide1] < 0.5f || valid[i * 4 + guide2] < 0.5f);
            const float pair_conf = CLAMP(plane2[i * 4 + 0], 0.f, 1.f);
            const float smooth_t = CLAMP((pair_conf - 0.7f) / 0.25f, 0.f, 1.f);
            const float floor_width = has_pair ? smooth_t * smooth_t * (3.f - 2.f * smooth_t) : 0.f;
            estimate[i * 4 + cdeep] = floor_width * estimate[i * 4 + cdeep] + (1.f - floor_width) * joint;
            guide_score[i * 4 + cdeep] = floor_width * guide_score[i * 4 + cdeep]
                                         + (1.f - floor_width) * CLAMP(guide_score[i * 4 + 3], 0.f, 1.f);
          }
        }
      }

      // MATHS BRIDGE -- Step 4 (HF refit), article §"Hybrid Laplacian-band guiding of the high
      // frequencies" / §"Rebuild the high frequencies": the estimate is split at sigma/4 into a low band
      // ubar (plane2 below) and a detail band u - ubar. The detail band gets its OWN windowed colour-line
      // with R^2-shrunk gains (on a zero-mean band shrinkage is the correct estimator: no magnitude to
      // lose, only noise to not print), and the HF is blended between this guided resynthesis
      // h_g = a(u_g1-ubar_g1)+b(u_g2-ubar_g2) and the R^2-damped transfer h_d = R^2 (u_c - ubar_c) by
      // quadratic min-energy odds w = e_d^2/(e_d^2 + e_g^2), e_{d,g} = blurred |HF_{d,g}| -- an edge
      // misfire spikes the guided HF energy e_g, so w -> 0 and the damped path wins exactly there (the
      // failure self-detects, no content discriminator needed). Note the band split blurs at sigma/4
      // (floored at 2 px) while the moments below blur at the fit's cf_sigma -- two deliberate scales.
      //
      // R^2-scaled HIGH-FREQUENCY damping: where the colour-line is weak, the guides' fine
      // texture is unrelated to the truth and must not be printed onto the reconstruction.
      // Continuous in the quality weight -- no estimator hand-off.
      memcpy(blur_in, estimate, region_pixels * 4 * sizeof(float));
      _region_blur(blur_in, plane2, region_w, region_h, fmaxf(cf_sigma / 4.f, 2.f)); // ubar = low band, Gaussian at sigma/4 (>= 2 px)

      // ---- Laplacian-band guiding (see the DT_HL_HF_GUIDE macro comment) ----
      // detail-band moments, weight = all three channels valid; packed exactly like the
      // full-signal moments: prev = [n, hR, hG, hB], s1 = [hRR, hGG, hBB, hRG], s3 = [hRB, hGB]
      HL_PFOR()
      for(size_t i = 0; i < region_pixels; i++)
      {
        const float lum_weight = (cf_binv > 0.f) ? sqf(fminf(lum_accum[i] * cf_binv, 1.f)) : 1.f;
        const float weight = (valid[i * 4 + 0] >= 0.5f && valid[i * 4 + 1] >= 0.5f && valid[i * 4 + 2] >= 0.5f)
                                 ? lum_weight
                                 : 0.f;
        // detail band H = est - ubar (plane2), weighted; packed like the CF moments = [n, hR, hG, hB]
        blur_in[i * 4 + 0] = weight;
        blur_in[i * 4 + 1] = weight * (estimate[i * 4 + 0] - plane2[i * 4 + 0]);
        blur_in[i * 4 + 2] = weight * (estimate[i * 4 + 1] - plane2[i * 4 + 1]);
        blur_in[i * 4 + 3] = weight * (estimate[i * 4 + 2] - plane2[i * 4 + 2]);
      }

      _region_blur(blur_in, prev_scale, region_w, region_h, cf_sigma); // windowed means of the detail band (blur at fit sigma)

      HL_PFOR()
      for(size_t i = 0; i < region_pixels; i++)
      {
        const float weight = blur_in[i * 4 + 0];
        const float hf_r = estimate[i * 4 + 0] - plane2[i * 4 + 0];
        const float hf_g = estimate[i * 4 + 1] - plane2[i * 4 + 1];
        const float hf_b = estimate[i * 4 + 2] - plane2[i * 4 + 2];
        blur_in[i * 4 + 0] = weight * hf_r * hf_r;
        blur_in[i * 4 + 1] = weight * hf_g * hf_g;
        blur_in[i * 4 + 2] = weight * hf_b * hf_b;
        blur_in[i * 4 + 3] = weight * hf_r * hf_g;
      }

      _region_blur(blur_in, plane1, region_w, region_h, cf_sigma);

      HL_PFOR()
      for(size_t i = 0; i < region_pixels; i++)
      {
        const float lum_weight = (cf_binv > 0.f) ? sqf(fminf(lum_accum[i] * cf_binv, 1.f)) : 1.f;
        const float weight = (valid[i * 4 + 0] >= 0.5f && valid[i * 4 + 1] >= 0.5f && valid[i * 4 + 2] >= 0.5f)
                                 ? lum_weight
                                 : 0.f;
        blur_in[i * 4 + 0]
            = weight * (estimate[i * 4 + 0] - plane2[i * 4 + 0]) * (estimate[i * 4 + 2] - plane2[i * 4 + 2]);
        blur_in[i * 4 + 1]
            = weight * (estimate[i * 4 + 1] - plane2[i * 4 + 1]) * (estimate[i * 4 + 2] - plane2[i * 4 + 2]);
        blur_in[i * 4 + 2] = (valid[i * 4 + 0] >= 0.5f && valid[i * 4 + 1] >= 0.5f && valid[i * 4 + 2] >= 0.5f)
                                 ? 1.f
                                 : 0.f; // unweighted valid mass for the anchor gate
        blur_in[i * 4 + 3] = 0.f;
      }

      _region_blur(blur_in, plane3, region_w, region_h, cf_sigma);

      // HF_M2(i, a, b) returns the windowed sum Sum w * H_a * H_b at pixel i (H = detail band, already
      // zero-mean, so no centering needed unlike CF_M2), indexing the packed second-moment planes:
      // diag (a==b) in plane1[0..2], RG/RB in plane1[3]/plane3[0], GB in plane3[1] -- feeds Var/Cov below
#define HF_M2(nb_index, coef_a, coef_b)                                                                           \
  (((coef_a) == (coef_b)) ? plane1[(nb_index) * 4 + (coef_a)]                                                     \
                          : ((2 + (coef_a) + (coef_b)) < 4 ? plane1[(nb_index) * 4 + 2 + (coef_a) + (coef_b)]     \
                                                           : plane3[(nb_index) * 4 + (coef_a) + (coef_b) - 2]))

      for(int c = 0; c < 3; c++)
      {
        const int guide1 = (c == 0) ? 1 : 0;
        const int guide2 = (c == 2) ? 1 : 2;

        size_t ntarget = 0;
        HL_PFOR(reduction(+ : ntarget))
        for(size_t i = 0; i < region_pixels; i++)
          if(valid[i * 4 + c] < 0.5f && valid[i * 4 + guide1] >= 0.5f && valid[i * 4 + guide2] >= 0.5f) ntarget++;

        if(ntarget == 0) continue;

        // R^2-shrunk detail-band gains at every pixel; anchors = trusted mass + bounded slopes
        HL_PFOR()
        for(size_t i = 0; i < region_pixels; i++)
        {
          const float norm = fmaxf(prev_scale[i * 4 + 0], 1e-9f);
          const float inv_det = 1.f / norm;
          const float mean1 = prev_scale[i * 4 + 1 + guide1] * inv_det;
          const float mean2 = prev_scale[i * 4 + 1 + guide2] * inv_det;
          const float mean_target = prev_scale[i * 4 + 1 + c] * inv_det;
          // same 2x2 normal equations as the CF fit, but on the detail-band moments (article step 4):
          // solve Sigma [a;b] = [Cov(H_u1,H_c); Cov(H_u2,H_c)] by Cramer's rule. u1=guide1, u2=guide2, v=c.
          const float var11 = fmaxf(HF_M2(i, guide1, guide1) * inv_det - mean1 * mean1, 0.f); // Var(H_u1)
          const float var22 = fmaxf(HF_M2(i, guide2, guide2) * inv_det - mean2 * mean2, 0.f); // Var(H_u2)
          const float var12 = HF_M2(i, guide1, guide2) * inv_det - mean1 * mean2;             // Cov(H_u1,H_u2)
          const float cov_tg1 = HF_M2(i, c, guide1) * inv_det - mean_target * mean1;          // Cov(H_v,H_u1) = RHS_1
          const float cov_tg2 = HF_M2(i, c, guide2) * inv_det - mean_target * mean2;          // Cov(H_v,H_u2) = RHS_2
          const float var_target = fmaxf(HF_M2(i, c, c) * inv_det - mean_target * mean_target, 0.f); // Var(H_v), denom of R^2

          const float lambda = 1e-3f * 0.5f * (var11 + var22) + 1e-12f;                       // relative Tikhonov ridge
          const float determinant = fmaxf((var11 + lambda) * (var22 + lambda) - var12 * var12, 1e-18f); // det Sigma
          const float hf_a = ((var22 + lambda) * cov_tg1 - var12 * cov_tg2) / determinant;    // a (Cramer)
          const float hf_b_slope = ((var11 + lambda) * cov_tg2 - var12 * cov_tg1) / determinant; // b (Cramer)
          const float hf_r2 = CLAMP((hf_a * cov_tg1 + hf_b_slope * cov_tg2) / (var_target + 1e-12f), 0.f, 1.f); // R^2

          // R^2-shrunk gains g*R^2 (correct estimator on a zero-mean band): stashed for the diffusion below
          reaction_weight[i] = hf_a * hf_r2;
          flat_target[i] = hf_b_slope * hf_r2;
          hole[i]
              = !(plane3[i * 4 + 2] > cf_fmin && prev_scale[i * 4 + 0] > 0.25f * plane3[i * 4 + 2]
                  && valid[i * 4 + c] >= 0.5f && fabsf(reaction_weight[i]) < 64.f && fabsf(flat_target[i]) < 64.f);
        }

        {
          // the two HF gain planes share the anchor mask -> one fused fill
          float *planes[2] = { reaction_weight, flat_target };
          _cf_harmonic_fill_n((float *const restrict *)planes, 2, hole, region_w, region_h, (int)(cf_sigma / 4.f),
                              steer, pipe);
        }

        // both HF candidates + their local energies (blurred |.|), packed into varc via one blur
        HL_PFOR()
        for(size_t i = 0; i < region_pixels; i++)
        {
          // h_g = a(u_g1-ubar_g1) + b(u_g2-ubar_g2): guide-transferred detail (diffused shrunk gains)
          const float hf_guided = reaction_weight[i] * (estimate[i * 4 + guide1] - plane2[i * 4 + guide1])
                                  + flat_target[i] * (estimate[i * 4 + guide2] - plane2[i * 4 + guide2]);
          // h_d = R^2 (u_c - ubar_c): the channel's own detail damped by its fit quality
          const float hf_damped
              = CLAMP(guide_score[i * 4 + c], 0.f, 1.f) * (estimate[i * 4 + c] - plane2[i * 4 + c]);
          blur_in[i * 4 + 0] = fabsf(hf_guided); // |h_g| -> blurred to e_g
          blur_in[i * 4 + 1] = fabsf(hf_damped); // |h_d| -> blurred to e_d
          blur_in[i * 4 + 2] = 0.f;
          blur_in[i * 4 + 3] = 0.f;
        }

        _region_blur(blur_in, valid_variance, region_w, region_h, fmaxf(cf_sigma / 4.f, 2.f));

        // quadratic min-energy blend of the two HF sources, then resynthesize
        HL_PFOR()
        for(size_t i = 0; i < region_pixels; i++)
          if(valid[i * 4 + c] < 0.5f && valid[i * 4 + guide1] >= 0.5f && valid[i * 4 + guide2] >= 0.5f)
          {
            const float hf_guided = reaction_weight[i] * (estimate[i * 4 + guide1] - plane2[i * 4 + guide1])
                                    + flat_target[i] * (estimate[i * 4 + guide2] - plane2[i * 4 + guide2]);
            const float hf_damped
                = CLAMP(guide_score[i * 4 + c], 0.f, 1.f) * (estimate[i * 4 + c] - plane2[i * 4 + c]);
            const float energy_g = valid_variance[i * 4 + 0]; // e_g = blurred |h_g|
            const float energy_d = valid_variance[i * 4 + 1]; // e_d = blurred |h_d|
            // quadratic min-energy odds w = e_d^2/(e_d^2 + e_g^2): favours the LOWER-energy candidate,
            // so a guide misfire (spiked e_g) drives w -> 0 and the damped path wins there
            const float energy_weight
                = energy_d * energy_d / fmaxf(energy_d * energy_d + energy_g * energy_g, 1e-18f);
            // resynthesis: u_c = ubar_c + w*h_g + (1-w)*h_d
            estimate[i * 4 + c]
                = plane2[i * 4 + c] + energy_weight * hf_guided + (1.f - energy_weight) * hf_damped;
          }
      }

#undef HF_M2

      // pixels with a single surviving guide keep the damped treatment
      HL_PFOR()
      for(size_t i = 0; i < region_pixels; i++)
      {
        const int n_valid = (valid[i * 4 + 0] >= 0.5f) + (valid[i * 4 + 1] >= 0.5f) + (valid[i * 4 + 2] >= 0.5f);
        if(n_valid != 1) continue;
        for(int c = 0; c < 3; c++)
          if(valid[i * 4 + c] < 0.5f)
          {
            // no second guide -> no h_g: keep only the R^2-damped own detail  u_c = ubar_c + R^2(u_c-ubar_c)
            const float hf_weight = CLAMP(guide_score[i * 4 + c], 0.f, 1.f);
            estimate[i * 4 + c] = plane2[i * 4 + c] + hf_weight * (estimate[i * 4 + c] - plane2[i * 4 + c]);
          }
      }

      // Step 5 / Soft saturation floor (article §"The algorithm" step 5): a clipped channel is
      // physically at least its saturated reading c0, but the hard max(e, c0) prints the
      // floor-binding contour as an edge wherever a weak prediction oscillates around saturation;
      // round the transition over ~2% of c0 instead. out = 1/2 (e + c0 + sqrt((e-c0)^2 + (0.02 c0)^2)),
      // a smooth max: -> e for e >> c0, -> c0 for e << c0, softened over a width 0.02*c0.
      HL_PFOR()
      for(size_t i = 0; i < region_pixels; i++)
        for(int c = 0; c < 3; c++)
          if(valid[i * 4 + c] < 0.5f)
          {
            const float clip_floor_c = clip0[i * 4 + c];         // c0, the saturated reading
            const float delta = estimate[i * 4 + c] - clip_floor_c;   // e - c0
            const float weight = 0.02f * fmaxf(clip_floor_c, 1e-6f);  // transition width = 2% of c0
            // c0 + 1/2 ( (e-c0) + sqrt((e-c0)^2 + width^2) ): the rounded lower bound at c0
            estimate[i * 4 + c] = clip_floor_c + 0.5f * (delta + sqrtf(delta * delta + weight * weight));
          }

      // Step 6 dome gate (article §"The algorithm" step 6): hand the dome-blend weight to the
      // self-dome block (it reads varc as Wc, uses We = Wc^2 as the keep weight). The two factors
      // answer two questions:  dome fraction = (1 - S_{0.4}^{0.85}(R^2)) * exp(-(delta/1.5 sigma)^2)
      //   R^2 (guide_score) -> "is the colour-line real here" via a smoothstep S (0 below 0.4,
      //     1 above 0.85): low R^2 = DOUBTFUL model -> lean on the dome.
      //   delta (clip_depth) -> "is the dome trustworthy here" via a gaussian of depth/(1.5 sigma):
      //     biharmonic extrapolation is excellent near the rim and degrades with distance, so the
      //     hand-over decays over ~1.5 sigma of depth. Deep interiors always stay on the fit.
      // We store Wc = sqrt(keep) with keep = 1 - dome_fraction = 1 - (1 - S(R^2)) * gdep, so the
      // self-dome block's conf_weight = Wc^2 = keep is exactly the coefficient-field share.
      HL_PFOR()
      for(size_t i = 0; i < region_pixels; i++)
        for(int c = 0; c < 3; c++)
        {
          const float dome_t = CLAMP((guide_score[i * 4 + c] - 0.4f) / 0.45f, 0.f, 1.f);   // ramp arg (0.4..0.85)
          const float we_r2 = dome_t * dome_t * (3.f - 2.f * dome_t);                       // S_{0.4}^{0.85}(R^2)
          const float smooth_t = clip_depth[i] / (1.5f * cf_sigma);                          // delta / (1.5 sigma)
          const float gdep = expf(-smooth_t * smooth_t);                                     // exp(-(delta/1.5 sigma)^2)
          valid_variance[i * 4 + c] = sqrtf(CLAMP(1.f - (1.f - we_r2) * gdep, 0.f, 1.f));    // Wc = sqrt(keep)
        }

      dt_free_align(hole2);
      dt_pixelpipe_cache_free_align(steer);
    }


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
        lum_accum[i] = estimate[i * 4 + 0] + estimate[i * 4 + 1] + estimate[i * 4 + 2];  // L_sum = R+G+B
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
          flat_target[i] = estimate[i * 4 + c] / fmaxf(lum_accum[i], epsilon);   // ratio r_c = est_c / L_sum

        _cf_harmonic_fill(flat_target, hole, region_w, region_h, cf_base, NULL, pipe);  // harmonic (Delta r = 0)

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

        const float caccum = fmaxf(plane1[i * 4 + 0] + plane1[i * 4 + 1] + plane1[i * 4 + 2], epsilon);  // sum r
        const int anyvalid
            = (valid[i * 4 + 0] >= 0.5f) || (valid[i * 4 + 1] >= 0.5f) || (valid[i * 4 + 2] >= 0.5f);

        for(int c = 0; c < 3; c++)
          if(valid[i * 4 + c] < 0.5f)
          {
            const float dome = dome_lum[i] * (plane1[i * 4 + c] / caccum);   // dome_c = L_dome * chroma share
            const float conf_weight = valid_variance[i * 4 + c] * valid_variance[i * 4 + c];  // keep = Wc^2
            estimate[i * 4 + c]
                = anyvalid ? (conf_weight * estimate[i * 4 + c] + (1.f - conf_weight) * dome) : dome;
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

    const double _thl2 = dt_get_wtime(); // PERF (DT_DEBUG_PERF): confidence + self-dome done

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
      double *sp_b = sp_S ? (double *)dt_alloc_align(sizeof(double) * sp_nh) : NULL;
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
                        (react > 0.f) ? flat_target : NULL, NULL, 1, 1.f, region_w, region_h, sp_b, cg_tmp1,
                        cg_tmp2, cg_residual);
        else
          _region_pde_solve(solver_field, hole, (react > 0.f) ? reaction_weight : NULL,
                            (react > 0.f) ? flat_target : NULL, NULL, 1, 1.f, region_w, region_h, cg_residual,
                            cg_dir, cg_operator, cg_tmp1, cg_tmp2, max_cg_iter);

        __OMP_PARALLEL_FOR__()
        for(size_t i = 0; i < region_pixels; i++) plane1[i * 4 + c] = fmaxf(solver_field[i], 0.f);
      }

      _sp_chol_free(sp_S);
      dt_free_align(sp_pgrid);
      dt_free_align(sp_b);

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

    const double _thl3 = dt_get_wtime(); // PERF (DT_DEBUG_PERF): joint core done

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

        for(int c = 0; c < 4; c++) prev_scale[i * 4 + c] = allc ? valid[i * 4 + c] : fmaxf(valid[i * 4 + c], 0.6f);
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

        for(int c = 0; c < 3; c++) plane1[i * 4 + c] = estimate[i * 4 + c] / lum_val;
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

      // primary Step-8 estimator: exact div(D grad r)=0 direct solve (returns 0 -> fall back to
      // the coarse-to-fine pyramid below for cores too large for the sparse Cholesky)
      if(!aniso_done) aniso_done = _aniso_div_solve(plane1, vld_an, lum_accum, blur_in, region_w, region_h, pipe);

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
          uint8_t *const restrict dhole = (uint8_t *)dt_alloc_align(sizeof(uint8_t) * down_pixels * 3);
          uint8_t *const restrict hplane = (uint8_t *)dt_alloc_align(sizeof(uint8_t) * down_pixels);

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
            dt_free_align(dhole);
            dt_free_align(hplane);
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

            _aniso_iterate_obs(dome_L, dobc, hplane, tensor_xx, tensor_xy, tensor_yy, tensor_scratch, down_w,
                               down_h, 240, box_x_lo, box_y_lo, box_x_hi, box_y_hi);

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
          dt_free_align(dhole);
          dt_free_align(hplane);
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
        const int active[3] = { act0, act1, act2 };

        if(act0 | act1 | act2)
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
                               region_h, 60, abx0, aby0, abx1, aby1);

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
      __OMP_PARALLEL_FOR__()
      for(size_t i = 0; i < region_pixels; i++)
      {
        const float raccum = fmaxf(plane1[i * 4 + 0] + plane1[i * 4 + 1] + plane1[i * 4 + 2], epsilon); // sum_j r_j

        for(int c = 0; c < 3; c++)
          if(vld_an[i * 4 + c] < 0.5f)
          {
            const float ratio_c = fmaxf(plane1[i * 4 + c], 0.f);
            const float value = lum_accum[i] * ratio_c / raccum; // recombine u_c = L_sum * r_c / sum_j r_j
            // SOFT saturation floor (same rounding as the coefficient-field floor): the hard
            // max() prints an exactly-flat shelf at the clip level plus a gradient kink
            // wherever the magnitude transfer under-predicts a channel near its own rim
            // inside the core (measured on DSC00078's sun: ~10 px flat at clip0_B, then a
            // 2x-slope break).
            // soft saturation floor u_c <- c0 + 0.5*((u-c0) + sqrt((u-c0)^2 + w^2)), w = 0.02*c0
            // (article rule 3 / step 5 soft-max): a smooth max(u, c0) with no shelf-and-kink
            const float clip_floor_c = clip0[i * 4 + c];
            const float delta = value - clip_floor_c;
            const float weight = 0.02f * fmaxf(clip_floor_c, 1e-6f);
            estimate[i * 4 + c] = clip_floor_c + 0.5f * (delta + sqrtf(delta * delta + weight * weight));
          }
      }
    }

    const double _thl4 = dt_get_wtime(); // PERF (DT_DEBUG_PERF): seam reg done
    // Per-region timing breakdown. Only for regions big enough to matter (small ones are noise) and
    // only printed when the perf debug channel is active (dt_print gates on darktable.unmuted).
    if(region_w > 200 || region_h > 200)
      dt_print(DT_DEBUG_PERF,
               "[highlights] region %dx%d (%.1fMpx): coeff+dome=%.0fms (blur=%.0fms) core=%.0fms chroma=%.0fms\n",
               region_w, region_h, (double)region_pixels / 1e6, (_thl2 - _thl1) * 1e3, _hl_blur_seconds * 1e3,
               (_thl3 - _thl2) * 1e3, (_thl4 - _thl3) * 1e3);
  }
  dt_free_align(hole);
  dt_pixelpipe_cache_free_align(solver_field);
  dt_pixelpipe_cache_free_align(fill_planes);
  dt_pixelpipe_cache_free_align(dome_lum);
  dt_pixelpipe_cache_free_align(lum_accum);
  dt_pixelpipe_cache_free_align(reaction_weight);
  dt_pixelpipe_cache_free_align(flat_target);
  dt_pixelpipe_cache_free_align(cg_residual);
  dt_pixelpipe_cache_free_align(cg_dir);
  dt_pixelpipe_cache_free_align(cg_operator);
  dt_pixelpipe_cache_free_align(cg_tmp1);
  dt_pixelpipe_cache_free_align(cg_tmp2);

  // Optional grain: reconstructed highlights are very smooth, so break them up with Poissonian noise
  // whose amplitude scales with the local value (the "noise level" user parameter). Only clipped
  // channels get it; valid channels keep their real data. Matches the legacy last-scale noise.
  if(noise_level > 0.f)
  {
    HL_PFOR(collapse(2))
    for(int y = 0; y < region_h; y++)
    {
      for(int x = 0; x < region_w; x++)
      {
        const size_t i = ((size_t)y * region_w + x) * 4;

        // per-pixel RNG, deterministic in region coordinates so the render is reproducible
        uint32_t DT_ALIGNED_ARRAY state[4]
            = { splitmix32(x + 1), splitmix32((y + 1) * (x + 3)), splitmix32(1337), splitmix32(666) };
        xoshiro128plus(state);
        xoshiro128plus(state);
        xoshiro128plus(state);
        xoshiro128plus(state);

        // per-channel noise standard deviation = value * noise_level
        dt_aligned_pixel_t current = { estimate[i], estimate[i + 1], estimate[i + 2], estimate[i + 3] };
        dt_aligned_pixel_t nsigma = { current[0] * noise_level, current[1] * noise_level, current[2] * noise_level,
                                      current[3] * noise_level };
        const int DT_ALIGNED_ARRAY flip[4] = { TRUE, FALSE, TRUE, FALSE };
        dt_aligned_pixel_t noise = { 0.f };
        dt_noise_generator_simd(DT_NOISE_POISSONIAN, current, nsigma, flip, state, noise);

        // one-sided (brightening) grain, only on the reconstructed (clipped) channels
        for(int c = 0; c < 3; c++)
          if(valid[i + c] < 0.5f) estimate[i + c] = fmaxf(current[c] + fabsf(noise[c] - current[c]), 0.f);
      }
    }
  }

  // FLOW: final per-region composite (article §"The algorithm", the flowchart's remosaic-feeding step).
  // Scatter the reconstructed clipped channels from the padded window back into the full-res interp
  // buffer at the region's absolute offset (region->rx0/ry0). Only the channels that were ACTUALLY
  // clipped (mask > 0.5) are overwritten -- valid channels keep their measured values untouched -- and
  // the write is floored at 0 (no negative radiance). Unclipped pixels outside every region are never
  // visited, so the reconstruction only ever edits the holes.
  HL_PFOR(collapse(2))
  for(int y = 0; y < region_h; y++)
  {
    for(int x = 0; x < region_w; x++)
    {
      const size_t src_offset = ((size_t)y * region_w + x) * 4;
      const size_t dst_offset = ((size_t)(region->ry0 + y) * width + (region->rx0 + x)) * 4;

      // only overwrite the channels that were actually clipped
      for(int c = 0; c < 3; c++)
        if(mask[dst_offset + c] > 0.5f) interp[dst_offset + c] = fmaxf(estimate[src_offset + c], 0.f);
    }
  }

  dt_pixelpipe_cache_free_align(estimate);
  dt_pixelpipe_cache_free_align(prev_scale);
  dt_pixelpipe_cache_free_align(valid);
  dt_pixelpipe_cache_free_align(blur_in);
  dt_pixelpipe_cache_free_align(plane1);
  dt_pixelpipe_cache_free_align(plane2);
  dt_pixelpipe_cache_free_align(plane3);
  dt_pixelpipe_cache_free_align(valid_variance);
  dt_pixelpipe_cache_free_align(guide_score);
  dt_pixelpipe_cache_free_align(clip_depth);
  dt_pixelpipe_cache_free_align(clip0);
}


// ---------------------------------------------------------------------------------------------
// R9 sensor-rolloff (knee) estimation + inversion. See the DT_HL_KNEE macro comment for the why.
// All values are handled in CLIP-NORMALIZED units: x = value / (clip level), so the detection
// threshold sits at DT_HL_KNEE_DET (the clips[] passed around equal 0.995 * clip level) and the
// band under estimation is [DT_HL_KNEE_LO, DT_HL_KNEE_DET).
// ---------------------------------------------------------------------------------------------


// Step 2 (article "The algorithm"): evaluate the lift term of the inverse correction
//   k^-1(v) = v + L(v),  where  L(v) = interp of the accepted per-bin median lift  median{ v_hat_i - v_i }.
// This returns L(v) only; the caller forms v + L(v). Piecewise-linear over knots
// [LO, center_0 .. center_last] with values [0, lift_0 .. lift_last]:
// identity-anchored at LO, flat-clamped past the last center (monotone raise-only by construction).
static inline float _knee_lift_of(const _hl_knee_curve_t *const k, const float x)
{
  const float step = (DT_HL_KNEE_DET - DT_HL_KNEE_LO) / (float)DT_HL_KNEE_BINS; // bin width over the band [LO, DET)
  const float bin_pos = (x - (DT_HL_KNEE_LO + 0.5f * step)) / step; // x in bin-center units (knot 0 sits at LO + step/2)

  if(bin_pos <= -0.5f) return 0.f;                              // at/below LO: no lift (identity anchor)
  if(bin_pos <= 0.f) return k->lift[0] * 2.f * (bin_pos + 0.5f); // first half-bin: ramp 0 -> lift[0] for a smooth start
  if(bin_pos >= (float)(DT_HL_KNEE_BINS - 1)) return k->lift[DT_HL_KNEE_BINS - 1]; // past last center: flat-extend the lift

  const int i = (int)bin_pos;                                  // lower knot (bin) index
  const float bin_frac = bin_pos - (float)i;                   // interpolation weight toward the next knot
  return k->lift[i] * (1.f - bin_frac) + k->lift[i + 1] * bin_frac; // linear blend of adjacent per-bin lifts
}

// Windowed-statistics engine (Steps 2-3): the Gaussian window G_sigma(x-y) that weights each
// neighbour y in the local colour-line regression. Blurring the raw moment planes (w, w*u, w*u*v,
// w*u*u, ...) by G_sigma realises the windowed sums  sum_y w(y) G_sigma(x-y) (...)  of the
// weighted least-squares fit at every pixel x at once.
// Single-plane Gaussian blur (the windowed-stats engine; cost independent of sigma).
static inline void _knee_blur(const float *const restrict in, float *const restrict out, const int width,
                              const int height, const float sigma)
{
  dt_gaussian_t *const gaussian = _hl_gauss_get(width, height, 1, sigma); // cached handle, do not free

  if(!gaussian)
  {
    memcpy(out, in, (size_t)width * height * sizeof(float));
    return;
  }

  dt_gaussian_blur(gaussian, in, out);
}

// Blur up to four PLANAR planes in one four-channel pass: the recursive gaussian's per-pixel
// recursion is the bottleneck and its 4-channel variant runs the four lanes in SIMD, so this
// is ~3x cheaper than four single-plane calls. Pack/unpack are cheap linear passes. Per plane
// the result is identical to _knee_blur (the channels never mix in the recursion).
static void _knee_blur4(const float *const planes[4], float *const outs[4], const int n_planes, const int region_w,
                        const int region_h, const float sigma, float *const restrict pack_in,
                        float *const restrict pack_out)
{
  const size_t region_pixels = (size_t)region_w * region_h;
  dt_gaussian_t *const gaussian = _hl_gauss_get(region_w, region_h, 4, sigma);

  if(!gaussian)
  {
    for(int k = 0; k < n_planes; k++) memcpy(outs[k], planes[k], region_pixels * sizeof(float));
    return;
  }

  HL_PFOR()
  for(size_t i = 0; i < region_pixels; i++)
    for(int k = 0; k < 4; k++) pack_in[i * 4 + k] = (k < n_planes) ? planes[k][i] : 0.f;

  dt_gaussian_blur_4c(gaussian, pack_in, pack_out);

  HL_PFOR()
  for(size_t i = 0; i < region_pixels; i++)
    for(int k = 0; k < 4; k++)
      if(k < n_planes) outs[k][i] = pack_out[i * 4 + k];
}

// qsort comparator for floats (ascending)
static int _knee_cmp_float(const void *ptr_a, const void *ptr_b)
{
  const float float_a = *(const float *)ptr_a;
  const float float_b = *(const float *)ptr_b;
  return (float_a > float_b) - (float_a < float_b);
}

// Median of values[0..count-1]; sorts in place. Serves both the robust per-bin lift
// median{ v_hat_i - v_i } and the MAD spread of those same votes (Step 2).
static float _knee_median(float *const values, const size_t count)
{
  qsort(values, count, sizeof(float), _knee_cmp_float);
  return (count & 1) ? values[count / 2] : 0.5f * (values[count / 2 - 1] + values[count / 2]);
}

// Symmetric second-moment plane index for channels (chan_a, chan_b) in the joint moment buffer
// layout: planes 0 = n (trusted mass), 1..3 = means R G B, 4..9 = second moments RR RG RB GG GB BB.
// Once divided by n and de-meaned these give Var(u_a) / Cov(u_a,u_b), the entries of the 2x2 normal
// matrix (indexing is symmetric: _knee_p2(a,b) == _knee_p2(b,a)).
static inline int _knee_p2(const int chan_a, const int chan_b)
{
  static const int plane_lut[3][3] = { { 4, 5, 6 }, { 5, 7, 8 }, { 6, 8, 9 } };
  return plane_lut[chan_a][chan_b];
}

// Step 2 (article "The algorithm", Sensor rolloff (knee) inversion): build the per-channel inverse
//   k^-1(v) = v + median{ v_hat_i - v_i | v_i in bin(v) }
// from the image itself. v_i are the measured band pixels; v_hat_i is what a windowed colour-line
// regression predicts each SHOULD read from its still-trusted neighbouring channels; the per-bin
// median of the votes v_hat_i - v_i is the accepted lift.
// Works on the RAW CFA `input`, NOT the bilinear-demosaiced buffer: the demosaic samples each
// channel through a different spatial filter per Bayer phase, and that alternating error is the same
// size as the knee signal -- it decorrelates the colour-lines and kills the estimate. A 2x2 quad
// binning of the CFA instead gives co-located R / mean-G / B per cell with no inter-site
// interpolation (the "quad-binned copy of the raw mosaic" of the article). `clipval_raw` is the
// clip level per channel in raw units (= clips / DET). Writes 3 curves (engaged = 0 means
// identity). Never fails: any shortage of data or memory returns identity.
static void _hl_knee_estimate(const float *const restrict input, const size_t width, const size_t height,
                              const uint32_t filters, const dt_iop_roi_t *const roi_in,
                              const uint8_t (*const xtrans)[6], const dt_aligned_pixel_t clipval_raw,
                              _hl_knee_curve_t curves[3], const dt_dev_pixelpipe_t *pipe)
{
  for(int c = 0; c < 3; c++)
  {
    curves[c].engaged = 0;
    memset(curves[c].lift, 0, sizeof(curves[c].lift));
  }

  // The curve is a global (per-channel) property, binned to <= ~1.5 Mpx. The base cell must
  // hold every CFA colour with a consistent phase: 2x2 for Bayer, 6x6 for X-Trans (the full
  // pattern period -- any smaller cell can miss a colour at some alignments).
  const int base = xtrans ? 6 : 2;
  int downsample = 1;
  while((width / ((size_t)base * downsample)) * (height / ((size_t)base * downsample)) > 1500000) downsample++;

  const int quad_size = base * downsample;
  const size_t bin_w = width / quad_size;
  const size_t bin_h = height / quad_size;
  const size_t bin_pixels = bin_w * bin_h;
  if(bin_w < 16 || bin_h < 16) return;

  float *const restrict binned
      = dt_pixelpipe_cache_alloc_align_float(bin_pixels * 3, pipe); // planar, clip-normalized
  float *const restrict pred = dt_pixelpipe_cache_alloc_align_float(bin_pixels * 3, pipe);
  float *const restrict r2_scores = dt_pixelpipe_cache_alloc_align_float(bin_pixels * 3, pipe);
  float *const restrict joint_moments
      = dt_pixelpipe_cache_alloc_align_float(bin_pixels * 10, pipe); // joint moment planes
  float *const restrict pair_moments
      = dt_pixelpipe_cache_alloc_align_float(bin_pixels * 6, pipe);                     // pair moment planes
  float *const restrict votes = dt_pixelpipe_cache_alloc_align_float(bin_pixels, pipe); // lift-fit bin scratch
  float *const restrict pk_in
      = dt_pixelpipe_cache_alloc_align_float(bin_pixels * 4, pipe); // _knee_blur4 pack scratch
  float *const restrict pk_out = dt_pixelpipe_cache_alloc_align_float(bin_pixels * 4, pipe);
  uint8_t *const restrict done = calloc(bin_pixels * 3, sizeof(uint8_t));

  if(IS_NULL_PTR(binned) || IS_NULL_PTR(pred) || IS_NULL_PTR(r2_scores) || IS_NULL_PTR(joint_moments)
     || IS_NULL_PTR(pair_moments) || IS_NULL_PTR(votes) || IS_NULL_PTR(pk_in) || IS_NULL_PTR(pk_out)
     || done == NULL)
    goto cleanup;

  // Bin the CFA per channel into clip-normalized planar planes: every qs x qs cell averages the
  // sites of each CFA colour it contains (phase-consistent, no inter-site interpolation).
  HL_PFOR(collapse(2))
  for(size_t i = 0; i < bin_h; i++)
    for(size_t j = 0; j < bin_w; j++)
    {
      dt_aligned_pixel_t accum = { 0.f, 0.f, 0.f, 0.f };
      dt_aligned_pixel_t counts = { 0.f, 0.f, 0.f, 0.f };

      for(int y = 0; y < quad_size; y++)
        for(int cell_x = 0; cell_x < quad_size; cell_x++)
        {
          const size_t row = i * quad_size + y;
          const size_t col = j * quad_size + cell_x;
          const size_t c = xtrans ? (size_t)FCxtrans((int)row, (int)col, roi_in, xtrans) : FC(row, col, filters);

          if(c <= 2)
          {
            accum[c] += input[row * width + col];
            counts[c] += 1.f;
          }
        }

      // per cell: co-located R / mean-G / B, each normalized to clip units v/(clip level) so the band
      // sits at [LO, DET); empty colours (no site of that colour in the cell) write 0
      for(int c = 0; c < 3; c++)
        binned[c * bin_pixels + i * bin_w + j] = (counts[c] > 0.f) ? accum[c] / (counts[c] * clipval_raw[c]) : 0.f;
    }

  // Band mass per channel: count binned cells in [LO, DET) -- the near-clip band [0.8c, 0.995c) the
  // knee corrects. A channel without a real band (< 200 cells) cannot trace a curve -> stays identity.
  size_t nband[3] = { 0, 0, 0 };
  for(size_t pixel = 0; pixel < bin_pixels; pixel++)
    for(int c = 0; c < 3; c++)
      if(binned[c * bin_pixels + pixel] >= DT_HL_KNEE_LO && binned[c * bin_pixels + pixel] < DT_HL_KNEE_DET)
        nband[c]++;

  if(nband[0] < 200 && nband[1] < 200 && nband[2] < 200) goto cleanup;

  // Multi-scale windowed colour-line predictions: per pixel keep the FINEST window that held
  // enough trusted mass. Joint 2-guide regression first (resolves two latent factors), then a
  // single-guide fallback where only one guide is itself trusted at the pixel. Sigmas are in
  // quad-cell units (x2 in CFA pixels), matching the prototype's 8..128 at scene resolution.
  const float sigmas[DT_HL_KNEE_NSIGMAS] = { 4.f, 8.f, 16.f, 32.f, 64.f };

  for(int sigma_index = 0; sigma_index < DT_HL_KNEE_NSIGMAS; sigma_index++)
  {
    const float sigma = sigmas[sigma_index];

    // ---- joint moments: weight w = 1 only where all three channels are trusted (< LO), so clipped
    // cells never vote; shared by every target channel. These ten raw planes, once blurred by
    // G_sigma below, become the windowed sums sum_y w G_sigma (...) feeding the normal equations. ----
    // All ten raw planes in one pass, then blurred 4-wide in place (via the pack scratch).
    HL_PFOR()
    for(size_t pixel = 0; pixel < bin_pixels; pixel++)
    {
      const float x_red = binned[0 * bin_pixels + pixel];
      const float x_green = binned[1 * bin_pixels + pixel];
      const float x_blue = binned[2 * bin_pixels + pixel];
      const float weight
          = (x_red < DT_HL_KNEE_LO && x_green < DT_HL_KNEE_LO && x_blue < DT_HL_KNEE_LO) ? 1.f : 0.f; // trust mask w
      joint_moments[0 * bin_pixels + pixel] = weight;                    // plane 0: n = sum w   (trusted mass)
      joint_moments[1 * bin_pixels + pixel] = weight * x_red;            // plane 1: sum w*R  -> E[R]
      joint_moments[2 * bin_pixels + pixel] = weight * x_green;          // plane 2: sum w*G  -> E[G]
      joint_moments[3 * bin_pixels + pixel] = weight * x_blue;           // plane 3: sum w*B  -> E[B]
      joint_moments[4 * bin_pixels + pixel] = weight * x_red * x_red;    // plane 4: sum w*R*R -> E[R^2]
      joint_moments[5 * bin_pixels + pixel] = weight * x_red * x_green;  // plane 5: sum w*R*G -> E[R*G]
      joint_moments[6 * bin_pixels + pixel] = weight * x_red * x_blue;   // plane 6: sum w*R*B -> E[R*B]
      joint_moments[7 * bin_pixels + pixel] = weight * x_green * x_green; // plane 7: sum w*G*G -> E[G^2]
      joint_moments[8 * bin_pixels + pixel] = weight * x_green * x_blue; // plane 8: sum w*G*B -> E[G*B]
      joint_moments[9 * bin_pixels + pixel] = weight * x_blue * x_blue;  // plane 9: sum w*B*B -> E[B^2]
    }

    for(int base = 0; base < 10; base += 4)
    {
      const int n_planes = MIN(4, 10 - base);
      const float *plane_in[4] = { 0 };
      float *plane_out[4] = { 0 };
      for(int k = 0; k < n_planes; k++)
        plane_in[k] = plane_out[k] = joint_moments + (size_t)(base + k) * bin_pixels;
      _knee_blur4(plane_in, plane_out, n_planes, bin_w, bin_h, sigma, pk_in, pk_out);
    }

    // Joint 2-guide colour-line fit v_hat = a*u1 + b*u2 + d for each target channel c, solved from
    // the blurred moments via the 2x2 normal equations (Cramer's rule). Guides are the other two
    // channels; the two-factor solve resolves scenes a single guide would under-predict.
    for(int c = 0; c < 3; c++)
    {
      if(nband[c] < 200) continue;

      const int guide1 = (c == 0) ? 1 : 0; // u1 = first guide channel
      const int guide2 = (c == 2) ? 1 : 2; // u2 = second guide channel

      HL_PFOR()
      for(size_t pixel = 0; pixel < bin_pixels; pixel++)
      {
        if(done[c * bin_pixels + pixel]) continue; // finer sigma already served this cell (multi-scale)

        const float x_val = binned[c * bin_pixels + pixel];       // measured band value v of target c
        const float x_guide1 = binned[guide1 * bin_pixels + pixel]; // guide u1 at this cell
        const float x_guide2 = binned[guide2 * bin_pixels + pixel]; // guide u2 at this cell
        const float weight_sum = joint_moments[pixel];            // n = windowed trusted mass at this cell

        if(!(x_val >= DT_HL_KNEE_LO && x_val < DT_HL_KNEE_DET)) continue; // only band cells [LO, DET) vote
        if(!(x_guide1 < DT_HL_KNEE_LO && x_guide2 < DT_HL_KNEE_LO)) continue; // both guides must be trusted here
        if(weight_sum <= DT_HL_KNEE_FMIN) continue;               // too little trusted mass in the window -> skip

        const float inv_weight = 1.f / weight_sum;                // 1/n, converts summed moments to expectations
        // windowed means E[.] = (sum w*.)/n
        const float mean_target = joint_moments[(size_t)(1 + c) * bin_pixels + pixel] * inv_weight;      // E[v]
        const float mean_guide1 = joint_moments[(size_t)(1 + guide1) * bin_pixels + pixel] * inv_weight; // E[u1]
        const float mean_guide2 = joint_moments[(size_t)(1 + guide2) * bin_pixels + pixel] * inv_weight; // E[u2]
        // second moments de-meaned = Var/Cov, centered about the per-window mean to avoid the float
        // E[u^2]-E[u]^2 cancellation on smooth content (squared mean dwarfs the variance)
        const float var_11 // Var(u1) = E[u1^2] - E[u1]^2   (normal-matrix diagonal, guide 1)
            = fmaxf(joint_moments[(size_t)_knee_p2(guide1, guide1) * bin_pixels + pixel] * inv_weight
                        - mean_guide1 * mean_guide1,
                    0.f);
        const float var_22 // Var(u2) = E[u2^2] - E[u2]^2   (normal-matrix diagonal, guide 2)
            = fmaxf(joint_moments[(size_t)_knee_p2(guide2, guide2) * bin_pixels + pixel] * inv_weight
                        - mean_guide2 * mean_guide2,
                    0.f);
        const float var_12 = joint_moments[(size_t)_knee_p2(guide1, guide2) * bin_pixels + pixel] * inv_weight
                             - mean_guide1 * mean_guide2; // Cov(u1,u2)  (off-diagonal of the normal matrix)
        const float cov_1 = joint_moments[(size_t)_knee_p2(c, guide1) * bin_pixels + pixel] * inv_weight
                            - mean_target * mean_guide1; // Cov(v,u1)  (RHS of the normal equations)
        const float cov_2 = joint_moments[(size_t)_knee_p2(c, guide2) * bin_pixels + pixel] * inv_weight
                            - mean_target * mean_guide2; // Cov(v,u2)  (RHS of the normal equations)
        const float var_target = fmaxf(joint_moments[(size_t)_knee_p2(c, c) * bin_pixels + pixel] * inv_weight
                                           - mean_target * mean_target,
                                       0.f); // Var(v), for the R^2 quality score

        // relative Tikhonov (ridge) damping lambda = 1e-3 * (Var u1 + Var u2)/2: scales with the
        // signal, never eats a weak-but-real slope
        const float lambda = 1e-3f * 0.5f * (var_11 + var_22) + 1e-12f;
        const float diag_11 = var_11 + lambda; // ridged normal-matrix diagonal [0][0]
        const float diag_22 = var_22 + lambda; // ridged normal-matrix diagonal [1][1]
        const float determinant = fmaxf(diag_11 * diag_22 - var_12 * var_12, 1e-18f); // det of the 2x2 system
        const float slope_1 = (diag_22 * cov_1 - var_12 * cov_2) / determinant; // a = slope on u1 (Cramer's rule)
        const float slope_2 = (diag_11 * cov_2 - var_12 * cov_1) / determinant; // b = slope on u2 (Cramer's rule)

        // v_hat(x) = E[v] + a*(u1 - E[u1]) + b*(u2 - E[u2])  (intercept d folded into the centering)
        pred[c * bin_pixels + pixel]
            = mean_target + slope_1 * (x_guide1 - mean_guide1) + slope_2 * (x_guide2 - mean_guide2);
        // R^2 = (a*Cov(v,u1) + b*Cov(v,u2)) / Var(v): explained-variance fraction, the vote's fit quality
        r2_scores[c * bin_pixels + pixel]
            = CLAMP((slope_1 * cov_1 + slope_2 * cov_2) / (var_target + 1e-12f), 0.f, 1.f);
        done[c * bin_pixels + pixel] = 1; // cell served at this (finest-so-far) sigma; coarser passes skip it
      }
    }

    // ---- single-guide fallback: simple regression v_hat = a*u + d where only one guide is itself
    // trusted at the cell (the joint fit needs both). Weight w = 1 where the target-guide PAIR is
    // trusted; slope from Cov(v,u)/Var(u). Fills cells the joint pass left `done == 0`. ----
    for(int chan_a = 0; chan_a < 3; chan_a++)
      for(int chan_b = chan_a + 1; chan_b < 3; chan_b++)
      {
        if(nband[chan_a] < 200 && nband[chan_b] < 200) continue;

        // pair moment planes: 0 = n (=sum w), 1 = sum w*a, 2 = sum w*b, 3 = sum w*a*a, 4 = sum w*b*b,
        // 5 = sum w*a*b -- all raw in one pass, then blurred 4-wide in place (via the pack scratch).
        HL_PFOR()
        for(size_t pixel = 0; pixel < bin_pixels; pixel++)
        {
          const float val_a = binned[chan_a * bin_pixels + pixel];
          const float val_b = binned[chan_b * bin_pixels + pixel];
          const float weight = (val_a < DT_HL_KNEE_LO && val_b < DT_HL_KNEE_LO) ? 1.f : 0.f; // pair trust mask w
          pair_moments[0 * bin_pixels + pixel] = weight;
          pair_moments[1 * bin_pixels + pixel] = weight * val_a;
          pair_moments[2 * bin_pixels + pixel] = weight * val_b;
          pair_moments[3 * bin_pixels + pixel] = weight * val_a * val_a;
          pair_moments[4 * bin_pixels + pixel] = weight * val_b * val_b;
          pair_moments[5 * bin_pixels + pixel] = weight * val_a * val_b;
        }

        for(int base = 0; base < 6; base += 4)
        {
          const int n_planes = MIN(4, 6 - base);
          const float *plane_in[4] = { 0 };
          float *plane_out[4] = { 0 };
          for(int k = 0; k < n_planes; k++)
            plane_in[k] = plane_out[k] = pair_moments + (size_t)(base + k) * bin_pixels;
          _knee_blur4(plane_in, plane_out, n_planes, bin_w, bin_h, sigma, pk_in, pk_out);
        }

        // both orientations of the pair: predict a from b, then b from a (select which plane holds
        // the target's vs the guide's mean/second-moment accordingly)
        for(int orient = 0; orient < 2; orient++)
        {
          const int target_ch = orient ? chan_b : chan_a; // target channel v
          const int guide_ch = orient ? chan_a : chan_b;  // guide channel u
          const int target_mean_plane = orient ? 2 : 1;   // plane holding sum w*v
          const int guide_mean_plane = orient ? 1 : 2;    // plane holding sum w*u
          const int target_sq_plane = orient ? 4 : 3;     // plane holding sum w*v*v
          const int guide_sq_plane = orient ? 3 : 4;      // plane holding sum w*u*u

          if(nband[target_ch] < 200) continue;

          HL_PFOR()
          for(size_t pixel = 0; pixel < bin_pixels; pixel++)
          {
            if(done[target_ch * bin_pixels + pixel]) continue; // already served (joint or finer sigma)

            const float x_val = binned[target_ch * bin_pixels + pixel]; // measured band value v
            const float x_guide = binned[guide_ch * bin_pixels + pixel]; // guide u
            const float weight_sum = pair_moments[pixel];               // n = windowed trusted mass

            if(!(x_val >= DT_HL_KNEE_LO && x_val < DT_HL_KNEE_DET)) continue; // only band cells vote
            if(!(x_guide < DT_HL_KNEE_LO)) continue;                    // the single guide must be trusted
            if(weight_sum <= DT_HL_KNEE_FMIN) continue;                 // too little trusted mass -> skip

            const float inv_weight = 1.f / weight_sum;                  // 1/n
            const float mean_target = pair_moments[(size_t)target_mean_plane * bin_pixels + pixel] * inv_weight; // E[v]
            const float mean_guide = pair_moments[(size_t)guide_mean_plane * bin_pixels + pixel] * inv_weight;   // E[u]
            const float covariance // Cov(v,u) = E[v*u] - E[v]E[u]  (plane 5 holds sum w*a*b)
                = pair_moments[(size_t)5 * bin_pixels + pixel] * inv_weight - mean_target * mean_guide;
            const float var_guide = fmaxf(pair_moments[(size_t)guide_sq_plane * bin_pixels + pixel] * inv_weight
                                              - mean_guide * mean_guide,
                                          0.f); // Var(u) = E[u^2] - E[u]^2
            const float var_target = fmaxf(pair_moments[(size_t)target_sq_plane * bin_pixels + pixel] * inv_weight
                                               - mean_target * mean_target,
                                           0.f); // Var(v), for the R^2 score
            const float slope = covariance / (var_guide * (1.f + 1e-3f) + 1e-12f); // a = Cov(v,u)/Var(u), ridged

            pred[target_ch * bin_pixels + pixel] = mean_target + slope * (x_guide - mean_guide); // v_hat = E[v] + a*(u-E[u])
            r2_scores[target_ch * bin_pixels + pixel] // R^2 = Cov^2 / (Var(u) Var(v)) for a single guide
                = CLAMP(covariance * covariance / (var_guide * var_target + 1e-18f), 0.f, 1.f);
            done[target_ch * bin_pixels + pixel] = 1;                   // cell now served
          }
        }
      }
  }

  // ---- Step 2, curve fit: per channel, pool the votes v_hat_i - v_i into 24 bins over the band,
  // take each bin's robust median lift (the median{ v_hat_i - v_i } of the equation), keep it only
  // when statistically significant, then make the curve monotone + raise-only. ----
  for(int c = 0; c < 3; c++)
  {
    if(nband[c] < 200) continue;

    // counting sort of the votes into DT_HL_KNEE_BINS = 24 bins by measured value v (offset[] is the
    // exclusive prefix-sum giving each bin's slot range in the flat `votes` scratch)
    size_t count[DT_HL_KNEE_BINS] = { 0 };
    size_t offset[DT_HL_KNEE_BINS + 1] = { 0 };
    const float bin_width = (DT_HL_KNEE_DET - DT_HL_KNEE_LO) / (float)DT_HL_KNEE_BINS; // band width / 24

    // pass 1: count votes per bin -- only cells that got a prediction (done) and cleared the fit-
    // quality gate R^2 > R2MIN participate (a poorly-fit pair does not get to vote)
    for(size_t pixel = 0; pixel < bin_pixels; pixel++)
    {
      if(!done[c * bin_pixels + pixel] || r2_scores[c * bin_pixels + pixel] <= DT_HL_KNEE_R2MIN) continue;
      const int bin_index // which of the 24 bins the measured value v falls in
          = CLAMP((int)((binned[c * bin_pixels + pixel] - DT_HL_KNEE_LO) / bin_width), 0, DT_HL_KNEE_BINS - 1);
      count[bin_index]++;
    }

    for(int i = 0; i < DT_HL_KNEE_BINS; i++) offset[i + 1] = offset[i] + count[i]; // prefix-sum -> per-bin slot base

    size_t fill[DT_HL_KNEE_BINS];
    memcpy(fill, offset, sizeof(fill)); // running write cursor per bin, seeded at each bin's base

    // pass 2: scatter each vote's lift v_hat_i - v_i (pred - measured) into its bin's slots
    for(size_t pixel = 0; pixel < bin_pixels; pixel++)
    {
      if(!done[c * bin_pixels + pixel] || r2_scores[c * bin_pixels + pixel] <= DT_HL_KNEE_R2MIN) continue;
      const float x_val = binned[c * bin_pixels + pixel]; // measured v
      const int bin_index = CLAMP((int)((x_val - DT_HL_KNEE_LO) / bin_width), 0, DT_HL_KNEE_BINS - 1);
      votes[fill[bin_index]++] = pred[c * bin_pixels + pixel] - x_val; // one pixel's vote v_hat_i - v_i
    }

    // per-bin robust lift, accepted only when significant vs the bin median's standard error --
    // the raise-only clamp would otherwise rectify zero-mean noise into fake lift
    float lift[DT_HL_KNEE_BINS];
    int seen[DT_HL_KNEE_BINS];
    int nseen = 0;

    for(int i = 0; i < DT_HL_KNEE_BINS; i++)
    {
      lift[i] = 0.f;
      seen[i] = 0;
      if(count[i] < DT_HL_KNEE_MINVOTES) continue; // need >= 100 votes so the error estimate itself is stable

      float *const bin_votes = votes + offset[i];
      const float median_lift = _knee_median(bin_votes, count[i]); // median{ v_hat_i - v_i } = the bin's raw lift

      // median absolute deviation (MAD) around the median, a robust spread estimate
      // (d is sorted, values get overwritten -- fine, last use)
      for(size_t k = 0; k < count[i]; k++) bin_votes[k] = fabsf(bin_votes[k] - median_lift); // |lift_i - median|
      const float median_abs_dev = _knee_median(bin_votes, count[i]); // MAD = median|lift_i - median(lift)|
      // SE of the bin median = 1.858*MAD/sqrt(n); 1.858 = 1.4826 (MAD->sigma) * 1.2533 (sigma->SE of median)
      const float std_err = 1.858f * median_abs_dev / sqrtf((float)count[i]);

      seen[i] = 1; // this bin is populated (has a usable estimate), whether or not the lift is significant
      nseen++;
      // significance gate: accept the lift only if median > NSIGMA*SE (2*SE, ~95% one-sided) -- otherwise
      // it stays 0, so the raise-only clamp below cannot rectify zero-mean noise into a fake lift
      if(median_lift > DT_HL_KNEE_NSIGMA * std_err) lift[i] = median_lift;
    }

    if(nseen < 3) continue; // too few populated bins to trust a curve -> leave channel at identity

    // interpolate lift over unseen (under-populated) bins (flat-extend past the first/last seen bin),
    // linearly between two seen bins -- the C twin of the prototype's np.interp over centers[seen]
    int prev = -1; // index of the nearest seen bin to the left (-1 = none yet)

    for(int i = 0; i < DT_HL_KNEE_BINS; i++)
    {
      if(seen[i])
      {
        prev = i;
        continue;
      }

      int next = -1;
      for(int k = i + 1; k < DT_HL_KNEE_BINS; k++)
        if(seen[k])
        {
          next = k;
          break;
        }

      if(prev < 0 && next < 0)
        lift[i] = 0.f;
      else if(prev < 0)
        lift[i] = lift[next];
      else if(next < 0)
        lift[i] = lift[prev];
      else
        lift[i] = lift[prev] + (lift[next] - lift[prev]) * (float)(i - prev) / (float)(next - prev);
    }

    // monotone raise-only clamp: cumulative max makes the curve non-decreasing (rolloff bias grows
    // toward clip) and drops any residual negatives -- the C twin of np.maximum.accumulate(max(lift,0))
    float running_max = 0.f;
    float lift_max = 0.f;

    for(int i = 0; i < DT_HL_KNEE_BINS; i++)
    {
      running_max = fmaxf(running_max, fmaxf(lift[i], 0.f)); // running max enforces monotone non-decreasing
      curves[c].lift[i] = running_max;                       // final per-bin lift knot for this channel
      lift_max = fmaxf(lift_max, running_max);               // peak lift, for the engage test below
    }

    // engage threshold: a peak lift below ENGAGE = 0.005 is noise -> stay identity (the no-op guarantee:
    // hard-clipped data yields near-zero medians, so the correction costs nothing)
    curves[c].engaged = (lift_max >= DT_HL_KNEE_ENGAGE);
    if(!curves[c].engaged) memset(curves[c].lift, 0, sizeof(curves[c].lift));
  }

cleanup:;
  dt_pixelpipe_cache_free_align(binned);
  dt_pixelpipe_cache_free_align(pred);
  dt_pixelpipe_cache_free_align(r2_scores);
  dt_pixelpipe_cache_free_align(joint_moments);
  dt_pixelpipe_cache_free_align(pair_moments);
  dt_pixelpipe_cache_free_align(votes);
  dt_pixelpipe_cache_free_align(pk_in);
  dt_pixelpipe_cache_free_align(pk_out);
  free(done);
}

// Step 2 application on the demosaiced [R,G,B,norm] buffer: for each engaged channel whose value
// lies in the band, replace v by k^-1(v) = v + L(v). Keeps the norm channel consistent with the
// corrected raw RGB (the "correction applied to the reconstruction anchors" of the article).
static void _hl_knee_apply_interpolated(float *const restrict interpolated, const size_t npix,
                                        const dt_aligned_pixel_t clipvaln, const dt_aligned_pixel_t wb4,
                                        const _hl_knee_curve_t curves[3])
{
  HL_PFOR()
  for(size_t pixel = 0; pixel < npix; pixel++)
  {
    int touched = 0;

    for(int c = 0; c < 3; c++)
    {
      if(!curves[c].engaged) continue; // channel with no measured rolloff -> pass through untouched

      const float norm_val = interpolated[pixel * 4 + c] / clipvaln[c]; // v in clip units

      if(norm_val >= DT_HL_KNEE_LO && norm_val < DT_HL_KNEE_DET) // only band values are corrected
      {
        const float lift = _knee_lift_of(&curves[c], norm_val); // L(v) from the fitted curve

        if(lift > 0.f)
        {
          interpolated[pixel * 4 + c] = (norm_val + lift) * clipvaln[c]; // v + L(v), back to raw-scaled units
          touched = 1;
        }
      }
    }

    if(touched) // rebuild norm = || white-balanced RGB || so the guide norm stays consistent
    {
      const float val_r = interpolated[pixel * 4 + 0] * wb4[0];
      const float val_g = interpolated[pixel * 4 + 1] * wb4[1];
      const float val_b = interpolated[pixel * 4 + 2] * wb4[2];
      interpolated[pixel * 4 + 3] = sqrtf(sqf(val_r) + sqf(val_g) + sqf(val_b));
    }
  }
}

// Apply the engaged curves to a CFA copy (raw units) so the final composition hands the corrected
// band values to the output; unclipped/clipped values pass through untouched.
static void _hl_knee_apply_cfa(const float *const restrict input, float *const restrict input_corr,
                               const size_t width, const size_t height, const uint32_t filters,
                               const dt_iop_roi_t *const roi_in, const uint8_t (*const xtrans)[6],
                               const dt_aligned_pixel_t clipval_raw, const _hl_knee_curve_t curves[3])
{
  HL_PFOR(collapse(2))
  for(size_t i = 0; i < height; i++)
    for(size_t j = 0; j < width; j++)
    {
      const size_t idx = i * width + j;
      const size_t c = xtrans ? (size_t)FCxtrans((int)i, (int)j, roi_in, xtrans) : FC(i, j, filters); // CFA colour here
      float value = input[idx];

      if(c <= 2 && curves[c].engaged)
      {
        const float norm_val = value / clipval_raw[c]; // v in clip units

        if(norm_val >= DT_HL_KNEE_LO && norm_val < DT_HL_KNEE_DET) // only band pixels get k^-1(v) = v + L(v)
          value = (norm_val + _knee_lift_of(&curves[c], norm_val)) * clipval_raw[c];
      }

      input_corr[idx] = value; // unclipped/clipped/out-of-band values pass through unchanged
    }
}



// env-gated CPU/GPU parity self-tests (same translation unit, see the file header)

// Harmonic transposition (nee coefficient field): the segmented full-resolution rebuild.
// A separate MODE from the guided laplacians above -- the method drifted too far from the
// 2021 a-trous reconstruction to replace it under existing edits.
// CPU driver for Bayer sensors, start to finish: gather (bilinear interpolation of the raw
// mosaic into [red, green, blue, norm] planes + feathered clip mask), knee pre-correction,
// distance transform + connected-region segmentation, per-region rebuild
// (_region_guided_filter), then remosaic of the reconstructed values back into the raw
// buffer. Returns 0 on success, 1 on allocation failure.
//
// MATHS/FLOW BRIDGE -- once-per-image orchestration of the 8-step procedure (article §"The algorithm"
// flowchart, the "once per image" half). In order:
//   step 1a (gather)        _interpolate_and_mask: bilinear-interpolate the Bayer mosaic into
//                           [R, G, B, norm] planes and build the binary per-channel clip masks;
//   step 2 (knee)           _hl_knee_estimate + _hl_knee_apply_interpolated: sensor-rolloff inversion,
//                           run on the raw mosaic BEFORE the gather freezes the per-pixel floors;
//   step 1b (segmentation)  distance transform -> depth delta(x), then _segment_clipped_regions ->
//                           connected regions with radius R (annotated in place below);
//   steps 3-8 (per region)  the region loop calls _region_guided_filter for each Omega;
//   remosaic + composite    _remosaic_and_replace scatters the reconstructed RGB back into the raw CFA.
// This is the CPU twin of _harmonic_reconstruct_host / process_harmonic_cl in highlights_harmonic_cl.h.
__DT_CLONE_TARGETS__
static int process_harmonic_bayer(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                                  const dt_dev_pixelpipe_iop_t *piece, const void *const restrict ivoid,
                                  void *const restrict ovoid, const dt_iop_roi_t *const roi_in,
                                  const dt_iop_roi_t *const roi_out, const dt_aligned_pixel_t clips)
{
  int err_code = 0;

  // Every helper below (normalization, knee estimate/apply, gather, remosaic) reads
  // FC(row, col, filters) with tile-local row/col (0-based within this buffer, no roi offset
  // added), so filters must be pre-shifted for roi_in's crop position here -- mirrors
  // demosaic.c's tile-local algorithms.
  const uint32_t filters = dt_dev_get_roi_filters(piece, roi_in);

  const size_t height = roi_in->height;
  const size_t width = roi_in->width;
  const size_t size = roi_in->width * roi_in->height;

  float *const restrict interpolated
      = dt_pixelpipe_cache_alloc_align_float(size * 4, pipe); // [R, G, B, norm] for each pixel
  float *const restrict clipping_mask
      = dt_pixelpipe_cache_alloc_align_float(size * 4, pipe); // [R, G, B, norm] for each pixel

  if(IS_NULL_PTR(interpolated) || IS_NULL_PTR(clipping_mask))
  {
    err_code = 1;
    goto error;
  }

  const float *const restrict input = (const float *const restrict)ivoid;
  float *const restrict output = (float *const restrict)ovoid;
  dt_aligned_pixel_t normalization = { 1.f, 1.f, 1.f, 1.f };
  _compute_laplacian_normalization(input, roi_in, filters, NULL, normalization);

  // Rolloff estimation FIRST (raw-based, mask-independent): its engagement decides, per
  // channel, whether the detection extends into the band (the band override,
  // DT_HL_BAND_OVR = 0.9, compile-time). Only channels with a
  // MEASURED rolloff get the override -- on hard-clipping sensors the band is trustworthy
  // data and stays valid.
  _hl_knee_curve_t knee[3];
  dt_aligned_pixel_t clipvaln = { 1.f, 1.f, 1.f, 1.f };
  dt_aligned_pixel_t knee_clipraw = { 1.f, 1.f, 1.f, 1.f };
  for(int c = 0; c < 3; c++)
  {
    clipvaln[c] = clips[c] / (DT_HL_KNEE_DET * fmaxf(normalization[c], 1e-9f));
    knee_clipraw[c] = clips[c] / DT_HL_KNEE_DET;
  }

  const double _tknee = dt_get_wtime();
  // FLOW step 2 (knee): estimate the per-channel sensor-rolloff inverse from the raw mosaic (step-2 maths
  // annotated on _hl_knee_estimate below). Runs on the raw values, before the gather, so the correction
  // is mask-independent; applied to the interpolated planes just below via _hl_knee_apply_interpolated.
  _hl_knee_estimate(input, width, height, filters, roi_in, NULL, knee_clipraw, knee, pipe);
  const int knee_on = knee[0].engaged || knee[1].engaged || knee[2].engaged;

  dt_aligned_pixel_t det_scale = { 1.f, 1.f, 1.f, 1.f };
  for(int c = 0; c < 3; c++)
    if(knee[c].engaged) det_scale[c] = DT_HL_BAND_OVR;

  // FLOW step 1a (gather): bilinear interpolation of the raw mosaic into [R, G, B, norm] planes + the
  // binary per-channel clip masks -- the article's "interpolate + masks" node, input to every later step.
  _interpolate_and_mask(input, interpolated, clipping_mask, clips, det_scale, normalization, filters, width,
                        height);
  // No mask feathering in this mode: the masks stay BINARY end to end. The per-channel
  // validity masks define measurement validity for every fit (feathering them reclassified
  // rim-clipped photosites -- raw values biased at the detection threshold -- as valid anchors
  // and dragged oblique rims toward the clip level), and the compositing alpha is a hard
  // switch (measured equivalent to the feathered composite once validity is binary and clipped
  // raw values are floors -- see the graveyard of the companion article).

  // Rolloff pre-correction of the working planes (the estimation ran before the gather; the
  // lift is value-based and independent of the mask, so band values -- including any the
  // override reclassified as reconstructable -- carry their corrected level, which the region
  // gather then freezes into the per-pixel floors clip0).
  if(knee_on) _hl_knee_apply_interpolated(interpolated, size, clipvaln, normalization, knee);

  dt_print(DT_DEBUG_PERF, "[highlights] knee: %.1f ms engaged=[%d %d %d] max lift=[%.4f %.4f %.4f]\n",
           (dt_get_wtime() - _tknee) * 1e3, knee[0].engaged, knee[1].engaged, knee[2].engaged,
           knee[0].lift[DT_HL_KNEE_BINS - 1], knee[1].lift[DT_HL_KNEE_BINS - 1],
           knee[2].lift[DT_HL_KNEE_BINS - 1]);

  // MATHS BRIDGE -- Step 1 (segmentation + depth), article "The algorithm" step 1: the any-clip mask's
  // Euclidean distance transform gives each clipped pixel its depth delta(x) (distance to the nearest
  // valid pixel); connected-component segmentation then groups clipped pixels into regions, each
  // carrying its reconstruction radius R = max delta over the region.
  //
  // Per-pixel reconstruction depth = distance from each clipped pixel to the nearest valid one
  // (Euclidean distance transform of the any-clip mask). A hole's reconstruction radius is the max
  // of this over the hole -- its true "reach needed", independent of the bbox shape.
  const size_t npix = (size_t)width * height;
  float *const restrict depth = dt_pixelpipe_cache_alloc_align_float(npix, pipe);
  if(!depth)
  {
    err_code = 1;
    goto error;
  }
  uint8_t *const restrict maskb = (uint8_t *)dt_alloc_align(npix);
  if(!maskb)
  {
    dt_pixelpipe_cache_free_align(depth);
    err_code = 1;
    goto error;
  }
  __OMP_PARALLEL_FOR__()
  for(size_t i = 0; i < npix; i++)
  {
    // seed the distance transform: clipped pixels = +inf (to be filled with delta), valid = 0
    depth[i] = (clipping_mask[i * 4 + 3] > 0.5f) ? (float)DT_DISTANCE_TRANSFORM_MAX : 0.f;
    maskb[i] = (clipping_mask[i * 4 + 3] >= 1e-3f); // binary any-clip mask for the connected-component pass
  }
  dt_image_distance_transform(NULL, depth, width, height, 0.f, DT_DISTANCE_TRANSFORM_NONE); // depth[] <- delta(x) (EDT)

  // Segment the clipped areas into connected regions and reconstruct each at full resolution with a
  // coarse->fine full-value guided filter (only clipped neighbourhoods are touched). Each region is
  // padded by its reconstruction radius (the deepest clip-to-valid distance), so the padding gives
  // the colour-line fit a valid rim as far out as the deepest pixel needs, and no farther.
  const dt_iop_highlights_data_t *const data = (const dt_iop_highlights_data_t *)piece->data;
  _hl_region_t *regions = NULL;
  // 8-neighbour connected components; pad = ceil(1.25 * R) clamped to [8, 256] px around each region
  const int nreg = _segment_clipped_regions(maskb, depth, width, height, 1.25f, 8, 256, &regions);

  // DIAGNOSTIC (DT_DEBUG_PERF): report the clip detection + segmentation so a "not reconstructed"
  // image (empty mask -> nothing to do) can be told apart from a segmentation-seam problem (many
  // regions). Count the any-clip pixels (channel 3 of the feathered mask).
  size_t nclipped = 0;
  for(size_t i = 0; i < npix; i++)
    if(clipping_mask[i * 4 + 3] > 0.5f) nclipped++;

  dt_print(DT_DEBUG_PERF,
           "[highlights] bayer %dx%d: procmax=[%.4f %.4f %.4f] clips=[%.4f %.4f %.4f] clipped=%llu (%.2f%%) "
           "regions=%d\n",
           (int)width, (int)height, piece->dsc_in.processed_maximum[0], piece->dsc_in.processed_maximum[1],
           piece->dsc_in.processed_maximum[2], clips[0], clips[1], clips[2], (unsigned long long)nclipped,
           100.0 * (double)nclipped / (double)npix, nreg);



  // FLOW steps 3-8 (per region): reconstruct each connected clipped region on its padded window. Regions
  // are independent (their padded read boxes were merged when they overlapped, in _segment_clipped_regions),
  // so this loop is embarrassingly parallel across regions and linear in the total padded area.
  for(int region_index = 0; region_index < nreg; region_index++)
    _region_guided_filter(interpolated, clipping_mask, depth, width, &regions[region_index], pipe,
                          data->solid_color, data->iterations, data->noise_level);


  free(regions);
  dt_free_align(maskb);
  dt_pixelpipe_cache_free_align(depth);

  // The composition reads `input` back for unmasked pixels, so the band correction must also go
  // through a corrected CFA copy -- otherwise the output band would keep the biased values the
  // reconstruction no longer agrees with (the seam would reappear at the detection contour).
  const float *remosaic_input = input;
  float *input_corr = NULL;

  if(knee_on)
  {
    input_corr = dt_pixelpipe_cache_alloc_align_float(size, pipe);

    if(!IS_NULL_PTR(input_corr))
    {
      _hl_knee_apply_cfa(input, input_corr, width, height, filters, roi_in, NULL, knee_clipraw, knee);
      remosaic_input = input_corr;
    }
  }

  // FLOW: remosaic + composite (the flowchart's terminal node). Scatter the reconstructed RGB back onto
  // the Bayer grid: out = opacity*rec + (1 - opacity)*base with opacity the binary any-clip mask, and
  // (clip_is_floor = TRUE here) base = max(raw, rec) on a clipped photosite -- so the reconstruction can
  // only lift a rolloff-biased sample toward its true level, never pull a valid one down. remosaic_input
  // is the knee-corrected CFA when the knee engaged (so unmasked pixels match the reconstruction's basis).
  _remosaic_and_replace(remosaic_input, input, interpolated, clipping_mask, output, normalization, clips, TRUE,
                        filters, width, height);


  if(!IS_NULL_PTR(input_corr)) dt_pixelpipe_cache_free_align(input_corr);

#if DEBUG_DUMP_PFM
  dump_PFM("/tmp/interpolated.pfm", interpolated, width, height);
  dump_PFM("/tmp/clipping_mask.pfm", clipping_mask, width, height);
#endif

error:;
  dt_pixelpipe_cache_free_align(interpolated);
  dt_pixelpipe_cache_free_align(clipping_mask);
  _hl_gauss_cache_flush();
  return err_code;
}

// CPU driver for Fujifilm X-Trans sensors; same start-to-finish flow as
// process_harmonic_bayer, with X-Trans gather/scatter/knee access.
//
// MATHS/FLOW BRIDGE -- identical 8-step orchestration to process_harmonic_bayer (see its header for the
// step-by-step map): step 1a gather (_interpolate_and_mask_xtrans, through the 6x6 X-Trans bilinear
// lookup) -> step 2 knee (_hl_knee_estimate/_apply, 6x6 binning) -> step 1b depth + segmentation
// (annotated below) -> steps 3-8 per region (_region_guided_filter, CFA-agnostic, shared with Bayer) ->
// remosaic + composite (_remosaic_and_replace_xtrans). Only the CFA-touching endpoints differ.
__DT_CLONE_TARGETS__
static int process_harmonic_xtrans(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                                   const dt_dev_pixelpipe_iop_t *piece, const void *const restrict ivoid,
                                   void *const restrict ovoid, const dt_iop_roi_t *const roi_in,
                                   const dt_iop_roi_t *const roi_out, const dt_aligned_pixel_t clips)
{
  // Mirror of process_harmonic_bayer: the reconstruction is CFA-agnostic (it works on the
  // interpolated RGB planes and masks); only the gather (bilinear interpolation), the scatter
  // (remosaic) and the knee's raw-mosaic access differ, through their X-Trans variants.
  int err_code = 0;

  const size_t height = roi_in->height;
  const size_t width = roi_in->width;
  const size_t size = roi_in->width * roi_in->height;
  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->dsc_in.xtrans;

  float *const restrict interpolated = dt_pixelpipe_cache_alloc_align_float(size * 4, pipe);
  float *const restrict clipping_mask = dt_pixelpipe_cache_alloc_align_float(size * 4, pipe);

  if(IS_NULL_PTR(interpolated) || IS_NULL_PTR(clipping_mask))
  {
    err_code = 1;
    goto error;
  }

  const float *const restrict input = (const float *const restrict)ivoid;
  float *const restrict output = (float *const restrict)ovoid;
  dt_aligned_pixel_t normalization = { 1.f, 1.f, 1.f, 1.f };
  _compute_laplacian_normalization(input, roi_in, 9u, xtrans, normalization);

  int32_t lookup[6][6][32] = { { { 0 } } };
  _build_xtrans_bilinear_lookup(lookup, roi_in, xtrans);
  // Rolloff estimation FIRST (raw-based, mask-independent), so its per-channel engagement
  // decides the band override of the detection -- see the Bayer path for the why.
  _hl_knee_curve_t knee[3];
  dt_aligned_pixel_t clipvaln = { 1.f, 1.f, 1.f, 1.f };
  dt_aligned_pixel_t knee_clipraw = { 1.f, 1.f, 1.f, 1.f };
  for(int c = 0; c < 3; c++)
  {
    clipvaln[c] = clips[c] / (DT_HL_KNEE_DET * fmaxf(normalization[c], 1e-9f));
    knee_clipraw[c] = clips[c] / DT_HL_KNEE_DET;
  }

  const double _tknee = dt_get_wtime();
  // FLOW step 2 (knee): X-Trans rolloff estimate on the raw mosaic (6x6 binning), same role as the Bayer
  // path -- applied to the interpolated planes below via _hl_knee_apply_interpolated when engaged.
  _hl_knee_estimate(input, width, height, 9u, roi_in, xtrans, knee_clipraw, knee, pipe);
  const int knee_on = knee[0].engaged || knee[1].engaged || knee[2].engaged;

  dt_aligned_pixel_t det_scale = { 1.f, 1.f, 1.f, 1.f };
  for(int c = 0; c < 3; c++)
    if(knee[c].engaged) det_scale[c] = DT_HL_BAND_OVR;

  dt_aligned_pixel_t eff_clips;
  for_four_channels(c) eff_clips[c] = clips[c] * det_scale[c];

  // FLOW step 1a (gather): X-Trans variant of the gather -- bilinear interpolation through the 6x6 lookup
  // into [R, G, B, norm] planes + the binary per-channel clip masks. Feeds every later step.
  _interpolate_and_mask_xtrans(input, interpolated, clipping_mask, eff_clips, normalization, roi_in, lookup,
                               xtrans, width, height);
  // No mask feathering in this mode: the masks stay BINARY end to end. The per-channel
  // validity masks define measurement validity for every fit (feathering them reclassified
  // rim-clipped photosites -- raw values biased at the detection threshold -- as valid anchors
  // and dragged oblique rims toward the clip level), and the compositing alpha is a hard
  // switch (measured equivalent to the feathered composite once validity is binary and clipped
  // raw values are floors -- see the graveyard of the companion article).

  // Rolloff pre-correction of the working planes (estimation ran before the gather; the 6x6
  // X-Trans binning estimator is otherwise identical to the Bayer path).
  if(knee_on) _hl_knee_apply_interpolated(interpolated, size, clipvaln, normalization, knee);

  dt_print(DT_DEBUG_PERF, "[highlights] knee: %.1f ms engaged=[%d %d %d] max lift=[%.4f %.4f %.4f]\n",
           (dt_get_wtime() - _tknee) * 1e3, knee[0].engaged, knee[1].engaged, knee[2].engaged,
           knee[0].lift[DT_HL_KNEE_BINS - 1], knee[1].lift[DT_HL_KNEE_BINS - 1],
           knee[2].lift[DT_HL_KNEE_BINS - 1]);

  // MATHS BRIDGE -- Step 1 (segmentation + depth), same as process_harmonic_bayer: the any-clip mask's
  // Euclidean distance transform gives each clipped pixel its depth delta(x); connected-component
  // segmentation groups clipped pixels into regions, each carrying its reconstruction radius R = max delta.
  const size_t npix = (size_t)width * height;
  float *const restrict depth = dt_pixelpipe_cache_alloc_align_float(npix, pipe);
  if(!depth)
  {
    err_code = 1;
    goto error;
  }
  uint8_t *const restrict maskb = (uint8_t *)dt_alloc_align(npix);
  if(!maskb)
  {
    dt_pixelpipe_cache_free_align(depth);
    err_code = 1;
    goto error;
  }
  __OMP_PARALLEL_FOR__()
  for(size_t i = 0; i < npix; i++)
  {
    // seed the distance transform: clipped pixels = +inf (to be filled with delta), valid = 0
    depth[i] = (clipping_mask[i * 4 + 3] > 0.5f) ? (float)DT_DISTANCE_TRANSFORM_MAX : 0.f;
    maskb[i] = (clipping_mask[i * 4 + 3] >= 1e-3f); // binary any-clip mask for the connected-component pass
  }
  dt_image_distance_transform(NULL, depth, width, height, 0.f, DT_DISTANCE_TRANSFORM_NONE); // depth[] <- delta(x) (EDT)

  const dt_iop_highlights_data_t *const data = (const dt_iop_highlights_data_t *)piece->data;
  _hl_region_t *regions = NULL;
  // 8-neighbour connected components; pad = ceil(1.25 * R) clamped to [8, 256] px around each region
  const int nreg = _segment_clipped_regions(maskb, depth, width, height, 1.25f, 8, 256, &regions);

  size_t nclipped = 0;
  for(size_t i = 0; i < npix; i++)
    if(clipping_mask[i * 4 + 3] > 0.5f) nclipped++;

  dt_print(DT_DEBUG_PERF,
           "[highlights] xtrans %dx%d: procmax=[%.4f %.4f %.4f] clips=[%.4f %.4f %.4f] clipped=%llu (%.2f%%) "
           "regions=%d\n",
           (int)width, (int)height, piece->dsc_in.processed_maximum[0], piece->dsc_in.processed_maximum[1],
           piece->dsc_in.processed_maximum[2], clips[0], clips[1], clips[2], (unsigned long long)nclipped,
           100.0 * (double)nclipped / (double)npix, nreg);

  // FLOW steps 3-8 (per region): same CFA-agnostic per-region reconstruction as the Bayer path.
  for(int region_index = 0; region_index < nreg; region_index++)
    _region_guided_filter(interpolated, clipping_mask, depth, width, &regions[region_index], pipe,
                          data->solid_color, data->iterations, data->noise_level);

  free(regions);
  dt_free_align(maskb);
  dt_pixelpipe_cache_free_align(depth);

  const float *remosaic_input = input;
  float *input_corr = NULL;

  if(knee_on)
  {
    input_corr = dt_pixelpipe_cache_alloc_align_float(size, pipe);

    if(!IS_NULL_PTR(input_corr))
    {
      _hl_knee_apply_cfa(input, input_corr, width, height, 9u, roi_in, xtrans, knee_clipraw, knee);
      remosaic_input = input_corr;
    }
  }

  // FLOW: remosaic + composite (terminal node). Same rule as the Bayer path -- out = opacity*rec +
  // (1 - opacity)*base with base = max(raw, rec) on a clipped X-Trans photosite (clip_is_floor = TRUE).
  _remosaic_and_replace_xtrans(remosaic_input, input, interpolated, clipping_mask, output, normalization, clips,
                               TRUE, roi_in, xtrans, width, height);

  if(!IS_NULL_PTR(input_corr)) dt_pixelpipe_cache_free_align(input_corr);

error:;
  dt_pixelpipe_cache_free_align(interpolated);
  dt_pixelpipe_cache_free_align(clipping_mask);
  _hl_gauss_cache_flush();
  (void)roi_out;
  return err_code;
}
