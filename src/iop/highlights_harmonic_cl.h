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

// OpenCL code of the highlights harmonic-transposition mode. Textual include unit of
// highlights.c, included AFTER highlights_harmonic_cpu.h (it calls CPU functions and the
// self-tests compare against the CPU twins). Same section order as _cpu.h.

#ifdef HAVE_OPENCL
// PERF instrumentation of the GPU middle's host-device traffic (DT_DEBUG_PERF): time the
// host spends BLOCKED on the device (reads, finishes) vs pure enqueue counts, plus the
// host-side sparse Cholesky work. Accumulated per thread, reset at the middle's entry,
// printed with the "gpu middle" line.
static __thread double _hl_cl_wait_s = 0.0; // blocked in buffer readbacks
static __thread int _hl_cl_wait_n = 0;
static __thread double _hl_cl_finish_s = 0.0; // blocked in queue drains
static __thread int _hl_cl_finish_n = 0;
static __thread int _hl_cl_enq_n = 0;       // kernel launches enqueued
static __thread double _hl_cl_chol_s = 0.0; // sparse Cholesky factorization (host+enqueue)
static __thread int _hl_cl_chol_n = 0;
static __thread int _hl_cl_cg_n = 0; // CG iterations

static inline void _hl_cl_stats_reset(void)
{
  _hl_cl_wait_s = _hl_cl_finish_s = _hl_cl_chol_s = 0.0;
  _hl_cl_wait_n = _hl_cl_finish_n = _hl_cl_enq_n = _hl_cl_chol_n = _hl_cl_cg_n = 0;
}

static inline cl_int _hl_cl_read_timed(const int devid, void *host, cl_mem dev, const size_t offset,
                                       const size_t size, const int blocking)
{
  const double start_time = dt_get_wtime();
  const cl_int cl_error = dt_opencl_read_buffer_from_device(devid, host, dev, offset, size, blocking);
  _hl_cl_wait_s += dt_get_wtime() - start_time;
  _hl_cl_wait_n++;
  return cl_error;
}

static inline void _hl_cl_finish_timed(const int devid)
{
  const double start_time = dt_get_wtime();
  dt_opencl_finish(devid);
  _hl_cl_finish_s += dt_get_wtime() - start_time;
  _hl_cl_finish_n++;
}

static inline cl_int _hl_cl_enq2d(const int devid, const int kernel, const size_t *sizes)
{
  _hl_cl_enq_n++;
  return dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
}

static inline cl_int _hl_cl_enq2dl(const int devid, const int kernel, const size_t *sizes, const size_t *local)
{
  _hl_cl_enq_n++;
  return dt_opencl_enqueue_kernel_2d_with_local(devid, kernel, sizes, local);
}
#endif // HAVE_OPENCL
#ifdef HAVE_OPENCL
// GPU counterpart of _cf_harmonic_fill_n: harmonic fill (repeatedly replace each hole pixel by
// the average of its four neighbours -- Jacobi iterations -- run coarse-to-fine on shrunken
// copies of the grid) of up to 3 planes SHARING ONE anchor mask, executed entirely on device
// buffers. The mask pyramid, the tensor and the edge weights depend only on (mask, steer,
// geometry), so the planes share one build and the fused Jacobi kernels advance all of them
// per launch, reading the weights once per cell.
// vals[p] (float, rw*rh) hold the planes to fill; despite its name, `hole` (uchar, rw*rh) must
// be the ANCHOR mask (1 = trusted pixel to keep, 0 = hole to fill) -- see the caller-contract
// note below. Fills the hole cells of every vals[p] in place. Mirrors _cf_harmonic_fill_n on
// the CPU: any change here must be mirrored there and re-validated with the HL_FILLCL_TEST
// self-test (_cf_harmonic_fill_cl_selftest).

static cl_int _cf_harmonic_fill_cl_n(const int devid, void *gd_void, cl_mem *vals, const int n_planes_in,
                                     cl_mem hole, const int region_w, const int region_h, const int base_ds,
                                     const int mask_is_hole, cl_mem steer)
{
  dt_iop_highlights_global_data_t *global_data = (dt_iop_highlights_global_data_t *)gd_void;
  const int n_planes = CLAMP(n_planes_in, 1, DT_HL_FILL_CL_MAXP);
  const int downsample = CLAMP(base_ds, 1, 8);
  const int base_w = (region_w + downsample - 1) / downsample;
  const int base_h = (region_h + downsample - 1) / downsample;
  const size_t cell_count = (size_t)base_w * base_h;
  cl_int cl_err = DT_OPENCL_DEFAULT_ERROR;
  const int steered = (steer != NULL);
  const float steer_k = DT_HL_CF_K;

  cl_mem base_vals[DT_HL_FILL_CL_MAXP] = { NULL };
  cl_mem level_vals[DT_HL_FILL_CL_MAXP] = { NULL };
  cl_mem level_solution[DT_HL_FILL_CL_MAXP] = { NULL };
  cl_mem level_scratch[DT_HL_FILL_CL_MAXP] = { NULL };
  cl_mem prev_level_solution[DT_HL_FILL_CL_MAXP] = { NULL };
  int alloc_ok = 1;
  for(int plane = 0; plane < n_planes; plane++)
  {
    base_vals[plane] = dt_opencl_alloc_device_buffer(devid, sizeof(float) * cell_count);
    level_vals[plane] = dt_opencl_alloc_device_buffer(devid, sizeof(float) * cell_count);
    level_solution[plane] = dt_opencl_alloc_device_buffer(devid, sizeof(float) * cell_count);
    level_scratch[plane] = dt_opencl_alloc_device_buffer(devid, sizeof(float) * cell_count);
    prev_level_solution[plane] = dt_opencl_alloc_device_buffer(devid, sizeof(float) * cell_count);
    alloc_ok &= (base_vals[plane] && level_vals[plane] && level_solution[plane] && level_scratch[plane]
                 && prev_level_solution[plane]);
  }
  cl_mem base_anchor_mask = dt_opencl_alloc_device_buffer(devid, cell_count);
  cl_mem level_anchor_mask = dt_opencl_alloc_device_buffer(devid, cell_count);
  // aniso steering planes, only needed when a steering plane was passed in (all sized to the
  // base grid). Allocate them in one guarded block and fold their null checks into alloc_ok,
  // so the abort decision is taken exactly once below.
  cl_mem base_steer = NULL;
  cl_mem level_steer = NULL;
  cl_mem steer_blur_lin = NULL;
  cl_mem steer_blur_quad = NULL;
  cl_mem steer_grad_x = NULL;
  cl_mem steer_grad_y = NULL;
  cl_mem steer_tensor_xx = NULL;
  cl_mem steer_tensor_xy = NULL;
  cl_mem steer_tensor_yy = NULL;
  cl_mem grad_partial_sums = NULL;
  cl_mem grad_mean_norm = NULL;
  cl_mem neighbour_weights = NULL;
  cl_mem neighbour_weights_sum = NULL;
  if(steered)
  {
    base_steer = dt_opencl_alloc_device_buffer(devid, sizeof(float) * cell_count);
    level_steer = dt_opencl_alloc_device_buffer(devid, sizeof(float) * cell_count);
    steer_blur_lin = dt_opencl_alloc_device_buffer(devid, sizeof(float) * cell_count);
    steer_blur_quad = dt_opencl_alloc_device_buffer(devid, sizeof(float) * cell_count);
    steer_grad_x = dt_opencl_alloc_device_buffer(devid, sizeof(float) * cell_count);
    steer_grad_y = dt_opencl_alloc_device_buffer(devid, sizeof(float) * cell_count);
    steer_tensor_xx = dt_opencl_alloc_device_buffer(devid, sizeof(float) * cell_count);
    steer_tensor_xy = dt_opencl_alloc_device_buffer(devid, sizeof(float) * cell_count);
    steer_tensor_yy = dt_opencl_alloc_device_buffer(devid, sizeof(float) * cell_count);
    grad_partial_sums = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 256);
    grad_mean_norm = dt_opencl_alloc_device_buffer(devid, sizeof(float));
    neighbour_weights = dt_opencl_alloc_device_buffer(devid, sizeof(float) * cell_count * 8);
    neighbour_weights_sum = dt_opencl_alloc_device_buffer(devid, sizeof(float) * cell_count);
    alloc_ok &= (base_steer && level_steer && steer_blur_lin && steer_blur_quad && steer_grad_x && steer_grad_y
                 && steer_tensor_xx && steer_tensor_xy && steer_tensor_yy && grad_partial_sums && grad_mean_norm
                 && neighbour_weights && neighbour_weights_sum)
                    ? 1
                    : 0;
  }
  if(!alloc_ok || !base_anchor_mask || !level_anchor_mask) goto out;

  // base grid from full resolution, per plane (base_anchor_mask is identical every time). The caller's mask
  // may be in either convention (1 = trusted anchor, or 1 = hole with mask_is_hole set);
  // hl_fill_down normalizes iter, and every internal level mask below is in the ANCHOR
  // convention regardless.
  for(int plane = 0; plane < n_planes; plane++)
  {
    const int kernel = global_data->kernel_hl_fill_down;
    size_t size[3] = { ROUNDUPDWD(base_w, devid), ROUNDUPDHT(base_h, devid), 1 };
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &vals[plane]);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &hole);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &base_vals[plane]);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &base_anchor_mask);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &base_w);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &base_h);
    dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &downsample);
    dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(int), &mask_is_hole);
    cl_err = _hl_cl_enq2d(devid, kernel, size);
    if(cl_err != CL_SUCCESS) goto out;
  }

  // aniso: steering plane on the base grid (plain block mean)
  if(steered)
  {
    const int kernel = global_data->kernel_hl_cfa_down;
    size_t size_level[3] = { ROUNDUPDWD(base_w, devid), ROUNDUPDHT(base_h, devid), 1 };
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &steer);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &base_steer);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &base_w);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &base_h);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &downsample);
    cl_err = _hl_cl_enq2d(devid, kernel, size_level);
    if(cl_err != CL_SUCCESS) goto out;
  }

  // pyramid depth: halve until the LONG side is <= 8 cells (same rationale as the CPU fill:
  // the coarsest flat seed must be trivially relaxable within the fixed sweep budget)
  int nlev = 1;
  while((MAX(base_w, base_h) >> nlev) > 8 && nlev < 12) nlev++;

  // coarse-to-fine sweep: solve on the coarsest grid first, then use each solved level to seed
  // the next finer one (prev_level_w/prev_level_h remember the previous level's dimensions for the seed upsample)
  int prev_level_w = 0;
  int prev_level_h = 0;
  for(int level = nlev - 1; level >= 0; level--)
  {
    const int step = 1 << level;
    const int level_w = (base_w + step - 1) / step;
    const int level_h = (base_h + step - 1) / step;
    size_t size[3] = { ROUNDUPDWD(level_w, devid), ROUNDUPDHT(level_h, devid), 1 };

    // level grid from the base grid, per plane (level_anchor_mask identical every time)
    for(int plane = 0; plane < n_planes; plane++)
    {
      const int kernel_down = global_data->kernel_hl_fill_down;
      dt_opencl_set_kernel_arg(devid, kernel_down, 0, sizeof(cl_mem), &base_vals[plane]);
      dt_opencl_set_kernel_arg(devid, kernel_down, 1, sizeof(cl_mem), &base_anchor_mask);
      dt_opencl_set_kernel_arg(devid, kernel_down, 2, sizeof(cl_mem), &level_vals[plane]);
      dt_opencl_set_kernel_arg(devid, kernel_down, 3, sizeof(cl_mem), &level_anchor_mask);
      dt_opencl_set_kernel_arg(devid, kernel_down, 4, sizeof(int), &base_w);
      dt_opencl_set_kernel_arg(devid, kernel_down, 5, sizeof(int), &base_h);
      dt_opencl_set_kernel_arg(devid, kernel_down, 6, sizeof(int), &level_w);
      dt_opencl_set_kernel_arg(devid, kernel_down, 7, sizeof(int), &level_h);
      dt_opencl_set_kernel_arg(devid, kernel_down, 8, sizeof(int), &step);
      const int level_anchor = 0; // internal level masks are always in the anchor convention
      dt_opencl_set_kernel_arg(devid, kernel_down, 9, sizeof(int), &level_anchor);
      cl_err = _hl_cl_enq2d(devid, kernel_down, size);
      if(cl_err != CL_SUCCESS) goto out;
    }

    if(level == nlev - 1)
    {
      // coarsest level: seed every hole cell with the mean of the anchor cells (single workgroup)
      for(int plane = 0; plane < n_planes; plane++)
      {
        const int kernel_seed = global_data->kernel_hl_fill_seed;
        const int n_cells = level_w * level_h;
        const int local_size = 256;
        size_t size_level[3] = { local_size, 1, 1 };
        size_t local[3] = { local_size, 1, 1 };
        dt_opencl_set_kernel_arg(devid, kernel_seed, 0, sizeof(cl_mem), &level_solution[plane]);
        dt_opencl_set_kernel_arg(devid, kernel_seed, 1, sizeof(cl_mem), &level_vals[plane]);
        dt_opencl_set_kernel_arg(devid, kernel_seed, 2, sizeof(cl_mem), &level_anchor_mask);
        dt_opencl_set_kernel_arg(devid, kernel_seed, 3, sizeof(int), &n_cells);
        dt_opencl_set_kernel_arg(devid, kernel_seed, 4, sizeof(float) * local_size, NULL);
        dt_opencl_set_kernel_arg(devid, kernel_seed, 5, sizeof(int) * local_size, NULL);
        cl_err = _hl_cl_enq2dl(devid, kernel_seed, size_level, local);
        if(cl_err != CL_SUCCESS) goto out;
      }
    }
    else
    {
      // finer levels: seed hole cells by upsampling the previous (coarser) level's solution
      for(int plane = 0; plane < n_planes; plane++)
      {
        const int kernel_seed_up = global_data->kernel_hl_fill_seed_up;
        dt_opencl_set_kernel_arg(devid, kernel_seed_up, 0, sizeof(cl_mem), &level_solution[plane]);
        dt_opencl_set_kernel_arg(devid, kernel_seed_up, 1, sizeof(cl_mem), &level_vals[plane]);
        dt_opencl_set_kernel_arg(devid, kernel_seed_up, 2, sizeof(cl_mem), &level_anchor_mask);
        dt_opencl_set_kernel_arg(devid, kernel_seed_up, 3, sizeof(cl_mem), &prev_level_solution[plane]);
        dt_opencl_set_kernel_arg(devid, kernel_seed_up, 4, sizeof(int), &level_w);
        dt_opencl_set_kernel_arg(devid, kernel_seed_up, 5, sizeof(int), &level_h);
        dt_opencl_set_kernel_arg(devid, kernel_seed_up, 6, sizeof(int), &prev_level_w);
        dt_opencl_set_kernel_arg(devid, kernel_seed_up, 7, sizeof(int), &prev_level_h);
        cl_err = _hl_cl_enq2d(devid, kernel_seed_up, size);
        if(cl_err != CL_SUCCESS) goto out;
      }
    }

    // aniso: level steering plane -> blurred L/L^2 -> gradients (+ mean-magnitude reduction)
    // -> Weickert tensor -> precomputed edge weights. Mirrors the CPU per-level build exactly;
    // the gnorm reduction is finished on device so the queue never drains mid-fill. Shared by
    // all n_planes planes -- fusing them is what amortizes this whole chain.
    if(steered)
    {
      const int n_cells = level_w * level_h;
      {
        const int kernel = global_data->kernel_hl_cfa_down;
        dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &base_steer);
        dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &level_steer);
        dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &base_w);
        dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &base_h);
        dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &level_w);
        dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &level_h);
        dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &step);
        cl_err = _hl_cl_enq2d(devid, kernel, size);
        if(cl_err != CL_SUCCESS) goto out;
      }
      for(int pass = 0; pass < 2; pass++)
      {
        const int kernel = global_data->kernel_hl_cfa_box;
        cl_mem blur_in_lin = pass ? steer_grad_x : level_steer;
        cl_mem blur_in_quad = pass ? steer_grad_y : level_steer;
        cl_mem outL = pass ? steer_blur_lin : steer_grad_x;
        cl_mem outQ = pass ? steer_blur_quad : steer_grad_y;
        const int square = (pass == 0);
        dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &blur_in_lin);
        dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &blur_in_quad);
        dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &outL);
        dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &outQ);
        dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &level_w);
        dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &level_h);
        dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &square);
        cl_err = _hl_cl_enq2d(devid, kernel, size);
        if(cl_err != CL_SUCCESS) goto out;
      }
      {
        const int kernel = global_data->kernel_hl_cfa_grad;
        const int local_size = 64, n_groups = 256;
        size_t size_1d[3] = { (size_t)n_groups * local_size, 1, 1 };
        size_t local_size_1d[3] = { local_size, 1, 1 };
        dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &steer_blur_lin);
        dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &steer_grad_x);
        dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &steer_grad_y);
        dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &grad_partial_sums);
        dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &level_w);
        dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &level_h);
        dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(float) * local_size, NULL);
        cl_err = _hl_cl_enq2dl(devid, kernel, size_1d, local_size_1d);
        if(cl_err != CL_SUCCESS) goto out;
      }
      {
        // finish the reduction on device (single work-item): no blocking readback
        const int kernel = global_data->kernel_hl_cfa_gnorm;
        const int ngroups = 256;
        size_t size_1d[3] = { 1, 1, 1 };
        dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &grad_partial_sums);
        dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &grad_mean_norm);
        dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &ngroups);
        dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &n_cells);
        cl_err = _hl_cl_enq2d(devid, kernel, size_1d);
        if(cl_err != CL_SUCCESS) goto out;
      }
      {
        const int kernel = global_data->kernel_hl_cfa_tensor;
        size_t size_1d[3] = { ROUNDUPDWD(n_cells, devid), 1, 1 };
        dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &steer_grad_x);
        dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &steer_grad_y);
        dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &steer_blur_lin);
        dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &steer_blur_quad);
        dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &steer_tensor_xx);
        dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(cl_mem), &steer_tensor_xy);
        dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(cl_mem), &steer_tensor_yy);
        dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(cl_mem), &grad_mean_norm);
        dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(float), &steer_k);
        dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(int), &n_cells);
        cl_err = _hl_cl_enq2d(devid, kernel, size_1d);
        if(cl_err != CL_SUCCESS) goto out;
      }
      // edge weights are constant across the level's sweeps: precompute once -- but only for
      // the small grids the block kernel serves (the large-grid launch loop reads the tensor
      // planes directly, see above)
      if(level_w * level_h <= 4096)
      {
        const int kernel = global_data->kernel_hl_cfa_weights;
        dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &steer_tensor_xx);
        dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &steer_tensor_xy);
        dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &steer_tensor_yy);
        dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &neighbour_weights);
        dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &neighbour_weights_sum);
        dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &level_w);
        dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &level_h);
        cl_err = _hl_cl_enq2d(devid, kernel, size);
        if(cl_err != CL_SUCCESS) goto out;
      }
    }

    // small grids: all 100 iterations inside ONE single-workgroup launch (bit-identical);
    // the 100-launch ping-pong was the region loop's dominant enqueue cost on small regions
    if(level_w * level_h <= 4096)
    {
      const int iters = 100; // flat budget; convergence comes from the pyramid depth
      size_t size_box[3] = { 256, 1, 1 };
      size_t local_box[3] = { 256, 1, 1 };
      if(steered)
      {
        // fused: all n_planes planes advance inside the single launch (dummy slots read plane 0)
        const int kernel_block = global_data->kernel_hl_cfa_jacobi_block;
        cl_mem solution1 = (n_planes > 1) ? level_solution[1] : level_solution[0];
        cl_mem solution2 = (n_planes > 2) ? level_solution[2] : level_solution[0];
        cl_mem scratch1 = (n_planes > 1) ? level_scratch[1] : level_scratch[0];
        cl_mem scratch2 = (n_planes > 2) ? level_scratch[2] : level_scratch[0];
        int arg_index = 0;
        dt_opencl_set_kernel_arg(devid, kernel_block, arg_index++, sizeof(cl_mem), &level_solution[0]);
        dt_opencl_set_kernel_arg(devid, kernel_block, arg_index++, sizeof(cl_mem), &solution1);
        dt_opencl_set_kernel_arg(devid, kernel_block, arg_index++, sizeof(cl_mem), &solution2);
        dt_opencl_set_kernel_arg(devid, kernel_block, arg_index++, sizeof(cl_mem), &level_scratch[0]);
        dt_opencl_set_kernel_arg(devid, kernel_block, arg_index++, sizeof(cl_mem), &scratch1);
        dt_opencl_set_kernel_arg(devid, kernel_block, arg_index++, sizeof(cl_mem), &scratch2);
        dt_opencl_set_kernel_arg(devid, kernel_block, arg_index++, sizeof(cl_mem), &level_anchor_mask);
        dt_opencl_set_kernel_arg(devid, kernel_block, arg_index++, sizeof(cl_mem), &neighbour_weights);
        dt_opencl_set_kernel_arg(devid, kernel_block, arg_index++, sizeof(cl_mem), &neighbour_weights_sum);
        dt_opencl_set_kernel_arg(devid, kernel_block, arg_index++, sizeof(int), &level_w);
        dt_opencl_set_kernel_arg(devid, kernel_block, arg_index++, sizeof(int), &level_h);
        dt_opencl_set_kernel_arg(devid, kernel_block, arg_index++, sizeof(int), &iters);
        dt_opencl_set_kernel_arg(devid, kernel_block, arg_index++, sizeof(int), &n_planes);
        cl_err = _hl_cl_enq2dl(devid, kernel_block, size_box, local_box);
        if(cl_err != CL_SUCCESS) goto out;
      }
      else
        for(int plane = 0; plane < n_planes; plane++)
        {
          const int kernel_block = global_data->kernel_hl_fill_jacobi_block;
          int arg_index = 0;
          dt_opencl_set_kernel_arg(devid, kernel_block, arg_index++, sizeof(cl_mem), &level_solution[plane]);
          dt_opencl_set_kernel_arg(devid, kernel_block, arg_index++, sizeof(cl_mem), &level_scratch[plane]);
          dt_opencl_set_kernel_arg(devid, kernel_block, arg_index++, sizeof(cl_mem), &level_anchor_mask);
          dt_opencl_set_kernel_arg(devid, kernel_block, arg_index++, sizeof(int), &level_w);
          dt_opencl_set_kernel_arg(devid, kernel_block, arg_index++, sizeof(int), &level_h);
          dt_opencl_set_kernel_arg(devid, kernel_block, arg_index++, sizeof(int), &iters);
          cl_err = _hl_cl_enq2dl(devid, kernel_block, size_box, local_box);
          if(cl_err != CL_SUCCESS) goto out;
        }
      // 100 (even) internal swaps leave the solution in u, exactly like the launch loop:
      // rotate iter into prev for the next finer level's seed
      for(int plane = 0; plane < n_planes; plane++)
      {
        cl_mem swap_buf = prev_level_solution[plane];
        prev_level_solution[plane] = level_solution[plane];
        level_solution[plane] = swap_buf;
      }
      prev_level_w = level_w;
      prev_level_h = level_h;
      continue;
    }

    // larger grids: Jacobi sweeps as separate launches, ping-ponging between the two buffers
    const int n_iter = 100; // flat budget; convergence comes from the pyramid depth
    cl_mem solution_planes[DT_HL_FILL_CL_MAXP], scratch_planes[DT_HL_FILL_CL_MAXP];
    for(int plane = 0; plane < n_planes; plane++)
    {
      solution_planes[plane] = level_solution[plane];
      scratch_planes[plane] = level_scratch[plane];
    }
    for(int iter = 0; iter < n_iter; iter++)
    {
      if(steered)
      {
        // fused sweep: one launch advances all n_planes planes (dummy slots read plane 0).
        // large grids keep the tensor form (see the kernel comment: cache reuse beats
        // precomputed weights there); only the small-grid block kernel uses neighbour_weights/neighbour_weights_sum
        const int kernel_jacobi = global_data->kernel_hl_cfa_jacobi;
        cl_mem solution1 = (n_planes > 1) ? solution_planes[1] : solution_planes[0];
        cl_mem solution2 = (n_planes > 2) ? solution_planes[2] : solution_planes[0];
        cl_mem scratch1 = (n_planes > 1) ? scratch_planes[1] : scratch_planes[0];
        cl_mem scratch2 = (n_planes > 2) ? scratch_planes[2] : scratch_planes[0];
        int arg_index = 0;
        dt_opencl_set_kernel_arg(devid, kernel_jacobi, arg_index++, sizeof(cl_mem), &solution_planes[0]);
        dt_opencl_set_kernel_arg(devid, kernel_jacobi, arg_index++, sizeof(cl_mem), &solution1);
        dt_opencl_set_kernel_arg(devid, kernel_jacobi, arg_index++, sizeof(cl_mem), &solution2);
        dt_opencl_set_kernel_arg(devid, kernel_jacobi, arg_index++, sizeof(cl_mem), &scratch_planes[0]);
        dt_opencl_set_kernel_arg(devid, kernel_jacobi, arg_index++, sizeof(cl_mem), &scratch1);
        dt_opencl_set_kernel_arg(devid, kernel_jacobi, arg_index++, sizeof(cl_mem), &scratch2);
        dt_opencl_set_kernel_arg(devid, kernel_jacobi, arg_index++, sizeof(cl_mem), &level_anchor_mask);
        dt_opencl_set_kernel_arg(devid, kernel_jacobi, arg_index++, sizeof(cl_mem), &steer_tensor_xx);
        dt_opencl_set_kernel_arg(devid, kernel_jacobi, arg_index++, sizeof(cl_mem), &steer_tensor_xy);
        dt_opencl_set_kernel_arg(devid, kernel_jacobi, arg_index++, sizeof(cl_mem), &steer_tensor_yy);
        dt_opencl_set_kernel_arg(devid, kernel_jacobi, arg_index++, sizeof(int), &level_w);
        dt_opencl_set_kernel_arg(devid, kernel_jacobi, arg_index++, sizeof(int), &level_h);
        dt_opencl_set_kernel_arg(devid, kernel_jacobi, arg_index++, sizeof(int), &n_planes);
        cl_err = _hl_cl_enq2d(devid, kernel_jacobi, size);
        if(cl_err != CL_SUCCESS) goto out;
      }
      else
        for(int plane = 0; plane < n_planes; plane++)
        {
          const int kernel_jacobi = global_data->kernel_hl_fill_jacobi;
          int arg_index = 0;
          dt_opencl_set_kernel_arg(devid, kernel_jacobi, arg_index++, sizeof(cl_mem), &solution_planes[plane]);
          dt_opencl_set_kernel_arg(devid, kernel_jacobi, arg_index++, sizeof(cl_mem), &scratch_planes[plane]);
          dt_opencl_set_kernel_arg(devid, kernel_jacobi, arg_index++, sizeof(cl_mem), &level_anchor_mask);
          dt_opencl_set_kernel_arg(devid, kernel_jacobi, arg_index++, sizeof(int), &level_w);
          dt_opencl_set_kernel_arg(devid, kernel_jacobi, arg_index++, sizeof(int), &level_h);
          cl_err = _hl_cl_enq2d(devid, kernel_jacobi, size);
          if(cl_err != CL_SUCCESS) goto out;
        }
      for(int plane = 0; plane < n_planes; plane++)
      {
        cl_mem swap_buf = solution_planes[plane];
        solution_planes[plane] = scratch_planes[plane];
        scratch_planes[plane] = swap_buf;
      }
    }
    // solution of this level in `a`: stash into prev for the next finer seed, keeping u/v the
    // two scratches distinct from prev
    for(int plane = 0; plane < n_planes; plane++)
    {
      cl_mem swap_buf = prev_level_solution[plane];
      prev_level_solution[plane] = solution_planes[plane];
      solution_planes[plane] = swap_buf;
      level_solution[plane]
          = (prev_level_solution[plane] == level_solution[plane]) ? solution_planes[plane] : level_solution[plane];
      level_scratch[plane]
          = (prev_level_solution[plane] == level_scratch[plane]) ? solution_planes[plane] : level_scratch[plane];
    }
    prev_level_w = level_w;
    prev_level_h = level_h;
  }

  // upsample prev (base-grid solution) into the full-res holes: kernel expects HOLE mask; our
  // `hole` buffer holds ANCHORS (caller contract), so hl_fill_up's test is inverted there.
  for(int plane = 0; plane < n_planes; plane++)
  {
    const int kernel_seed_up = global_data->kernel_hl_fill_up;
    size_t size_upsample[3] = { ROUNDUPDWD(region_w, devid), ROUNDUPDHT(region_h, devid), 1 };
    dt_opencl_set_kernel_arg(devid, kernel_seed_up, 0, sizeof(cl_mem), &vals[plane]);
    dt_opencl_set_kernel_arg(devid, kernel_seed_up, 1, sizeof(cl_mem), &hole);
    dt_opencl_set_kernel_arg(devid, kernel_seed_up, 2, sizeof(cl_mem), &prev_level_solution[plane]);
    dt_opencl_set_kernel_arg(devid, kernel_seed_up, 3, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel_seed_up, 4, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel_seed_up, 5, sizeof(int), &base_w);
    dt_opencl_set_kernel_arg(devid, kernel_seed_up, 6, sizeof(int), &base_h);
    dt_opencl_set_kernel_arg(devid, kernel_seed_up, 7, sizeof(int), &downsample);
    dt_opencl_set_kernel_arg(devid, kernel_seed_up, 8, sizeof(int), &mask_is_hole);
    cl_err = _hl_cl_enq2d(devid, kernel_seed_up, size_upsample);
    if(cl_err != CL_SUCCESS) goto out;
  }

out:
  for(int plane = 0; plane < DT_HL_FILL_CL_MAXP; plane++)
  {
    dt_opencl_release_mem_object(base_vals[plane]);
    dt_opencl_release_mem_object(level_vals[plane]);
    dt_opencl_release_mem_object(level_solution[plane]);
    dt_opencl_release_mem_object(level_scratch[plane]);
    dt_opencl_release_mem_object(prev_level_solution[plane]);
  }
  dt_opencl_release_mem_object(base_anchor_mask);
  dt_opencl_release_mem_object(level_anchor_mask);
  dt_opencl_release_mem_object(base_steer);
  dt_opencl_release_mem_object(level_steer);
  dt_opencl_release_mem_object(steer_blur_lin);
  dt_opencl_release_mem_object(steer_blur_quad);
  dt_opencl_release_mem_object(steer_grad_x);
  dt_opencl_release_mem_object(steer_grad_y);
  dt_opencl_release_mem_object(steer_tensor_xx);
  dt_opencl_release_mem_object(steer_tensor_xy);
  dt_opencl_release_mem_object(steer_tensor_yy);
  dt_opencl_release_mem_object(grad_partial_sums);
  dt_opencl_release_mem_object(grad_mean_norm);
  dt_opencl_release_mem_object(neighbour_weights);
  dt_opencl_release_mem_object(neighbour_weights_sum);
  return cl_err;
}

// Single-plane convenience wrapper (isotropic callers and lone planes).
static cl_int _cf_harmonic_fill_cl(const int devid, void *gd_void, cl_mem val, cl_mem hole, const int region_w,
                                   const int region_h, const int base_ds, const int mask_is_hole, cl_mem steer)
{
  cl_mem val_planes[1] = { val };
  return _cf_harmonic_fill_cl_n(devid, gd_void, val_planes, 1, hole, region_w, region_h, base_ds, mask_is_hole,
                                steer);
}
#endif // HAVE_OPENCL
#ifdef HAVE_OPENCL
// (defined further down; the self-test compares against it)
static void _cf_harmonic_fill(float *const restrict val, const uint8_t *const restrict hole, const int region_w,
                              const int region_h, const int base_ds, const float *const restrict steer,
                              const dt_dev_pixelpipe_t *pipe);


static cl_int _region_blur_cl(const int devid, cl_mem in, cl_mem out, const int region_w, const int region_h,
                              const float sigma);

// ---- coefficient-field JOINT stage on the GPU (pattern-setter for the per-pixel port) ----
// One "fit clipped channel c from the two guides g1/g2" pass: fit the colour-line coefficients
// per pixel from the pre-blurred windowed moments (local means and channel-product averages),
// harmonically diffuse the coefficient fields across the clipped zone, then evaluate the
// prediction against the measured guides and write the result into est (also updating the
// fit-quality score bsc).
// est/vld/bsc are float4 buffers (rn * 4); moments go through image2d for the CL blur; the
// diffused coefficients live in single-channel buffers feeding _cf_harmonic_fill_cl.
// Mirrors the joint coefficient-field stage inside _region_guided_filter (CPU): any change here
// must be mirrored there and re-validated with the HL_CFCL_TEST self-tests
// (_cf_joint_stage_cl_selftest / _cf_stage_cl_selftest).
static cl_int _cf_joint_stage_cl(const int devid, void *gd_void, cl_mem estimate, cl_mem valid,
                                 cl_mem model_quality, cl_mem mom0, cl_mem mom1, cl_mem mom2, cl_mem steer,
                                 const float *const restrict channel_means, const int region_w, const int region_h,
                                 const float cf_sigma, const float cf_fmin, const int c, const int guide1,
                                 const int guide2)
{
  dt_iop_highlights_global_data_t *global_data = (dt_iop_highlights_global_data_t *)gd_void;
  const size_t region_pixels = (size_t)region_w * region_h;
  cl_int cl_err = DT_OPENCL_DEFAULT_ERROR;
  size_t work_size[3] = { ROUNDUPDWD(region_w, devid), ROUNDUPDHT(region_h, devid), 1 };

  cl_mem coeff_slope_a = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem coeff_slope_b = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem coeff_offset = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem coeff_r2 = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem anchor = dt_opencl_alloc_device_buffer(devid, region_pixels);
  cl_mem broad = dt_opencl_alloc_device_buffer(devid, region_pixels);
  if(!coeff_slope_a || !coeff_slope_b || !coeff_offset || !coeff_r2 || !anchor || !broad) goto out;

  // per-pixel colour-line fit from the blurred moments; also writes the anchor masks
  {
    const int kernel = global_data->kernel_hl_cf_fit_joint;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &mom0);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &mom1);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &mom2);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &c);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &guide1);
    dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &guide2);
    dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(float), &cf_fmin);
    dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(cl_mem), &coeff_slope_a);
    dt_opencl_set_kernel_arg(devid, kernel, 11, sizeof(cl_mem), &coeff_slope_b);
    dt_opencl_set_kernel_arg(devid, kernel, 12, sizeof(cl_mem), &coeff_offset);
    dt_opencl_set_kernel_arg(devid, kernel, 13, sizeof(cl_mem), &coeff_r2);
    dt_opencl_set_kernel_arg(devid, kernel, 14, sizeof(cl_mem), &anchor);
    dt_opencl_set_kernel_arg(devid, kernel, 15, sizeof(cl_mem), &broad);
    dt_opencl_set_kernel_arg(devid, kernel, 16, sizeof(float), &channel_means[0]);
    dt_opencl_set_kernel_arg(devid, kernel, 17, sizeof(float), &channel_means[1]);
    dt_opencl_set_kernel_arg(devid, kernel, 18, sizeof(float), &channel_means[2]);
    cl_err = _hl_cl_enq2d(devid, kernel, work_size);
    if(cl_err != CL_SUCCESS) goto out;
  }

  // harmonic diffusion of the coefficient fields across the clipped zone (fit quality cr2 uses
  // the broader anchor mask)
  {
    const int base_ds = (int)(cf_sigma / 4.f);
    cl_mem coeff_planes[3]
        = { coeff_slope_a, coeff_slope_b, coeff_offset }; // shared anchor mask -> one fused fill
    cl_err
        = _cf_harmonic_fill_cl_n(devid, gd_void, coeff_planes, 3, anchor, region_w, region_h, base_ds, 0, steer);
    if(cl_err != CL_SUCCESS) goto out;
    cl_err = _cf_harmonic_fill_cl(devid, gd_void, coeff_r2, broad, region_w, region_h, base_ds, 0, steer);
    if(cl_err != CL_SUCCESS) goto out;
  }

  // evaluate the diffused colour line against the measured guides; write est + fit score bsc
  {
    const int kernel = global_data->kernel_hl_cf_eval_joint;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &coeff_slope_a);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &coeff_slope_b);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &coeff_offset);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &coeff_r2);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(cl_mem), &model_quality);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(int), &c);
    dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(int), &guide1);
    dt_opencl_set_kernel_arg(devid, kernel, 11, sizeof(int), &guide2);
    cl_err = _hl_cl_enq2d(devid, kernel, work_size);
  }

out:
  dt_opencl_release_mem_object(coeff_slope_a);
  dt_opencl_release_mem_object(coeff_slope_b);
  dt_opencl_release_mem_object(coeff_offset);
  dt_opencl_release_mem_object(coeff_r2);
  dt_opencl_release_mem_object(anchor);
  dt_opencl_release_mem_object(broad);
  return cl_err;
}

// Pair stage for one orientation: predict one clipped channel from a SINGLE guide channel
// (slope + intercept fitted from the windowed moments), diffuse the fitted coefficients across
// the clipped zone, then evaluate against the measured guide and write into est.
// `a`/`b` name the channel pair and `o` picks the orientation (which of the two is the target
// tc and which is the guide gc); oc is the remaining third channel.
// Runs unconditionally (no target-count guard: an empty target set writes nothing).
// Mirrors the pair coefficient-field stage inside _region_guided_filter (CPU): any change here
// must be mirrored there and re-validated with the HL_CFCL_TEST self-tests.
static cl_int _cf_pair_stage_cl(const int devid, void *gd_void, cl_mem estimate, cl_mem valid,
                                cl_mem model_quality, cl_mem moment_a, cl_mem moment_b, cl_mem steer,
                                const float *const restrict channel_means, const int region_w, const int region_h,
                                const float cf_sigma, const float cf_fmin, const int chan_a, const int chan_b,
                                const int orientation)
{
  dt_iop_highlights_global_data_t *global_data = (dt_iop_highlights_global_data_t *)gd_void;
  const size_t region_pixels = (size_t)region_w * region_h;
  cl_int cl_err = DT_OPENCL_DEFAULT_ERROR;
  size_t work_size[3] = { ROUNDUPDWD(region_w, devid), ROUNDUPDHT(region_h, devid), 1 };
  const int target_chan = orientation ? chan_b : chan_a;
  const int guide_chan = orientation ? chan_a : chan_b;
  const int other_chan = 3 - chan_a - chan_b;

  cl_mem coeff_slope = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem coeff_intercept = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem coeff_r2 = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem anchor = dt_opencl_alloc_device_buffer(devid, region_pixels);
  cl_mem broad = dt_opencl_alloc_device_buffer(devid, region_pixels);
  if(!coeff_slope || !coeff_intercept || !coeff_r2 || !anchor || !broad) goto out;

  // per-pixel single-guide fit (slope cs, intercept ci, fit quality cr2) from the moments
  {
    const int kernel = global_data->kernel_hl_cf_fit_pair;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &moment_a);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &moment_b);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &target_chan);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &orientation);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(float), &cf_fmin);
    dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(cl_mem), &coeff_slope);
    dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(cl_mem), &coeff_intercept);
    dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(cl_mem), &coeff_r2);
    dt_opencl_set_kernel_arg(devid, kernel, 11, sizeof(cl_mem), &anchor);
    dt_opencl_set_kernel_arg(devid, kernel, 12, sizeof(cl_mem), &broad);
    dt_opencl_set_kernel_arg(devid, kernel, 13, sizeof(float), &channel_means[target_chan]);
    dt_opencl_set_kernel_arg(devid, kernel, 14, sizeof(float), &channel_means[guide_chan]);
    cl_err = _hl_cl_enq2d(devid, kernel, work_size);
    if(cl_err != CL_SUCCESS) goto out;
  }

  // harmonic diffusion of slope/intercept/fit-quality fields across the clipped zone
  {
    const int base_ds = (int)(cf_sigma / 4.f);
    cl_mem coeff_planes[2] = { coeff_slope, coeff_intercept }; // shared anchor mask -> one fused fill
    cl_err
        = _cf_harmonic_fill_cl_n(devid, gd_void, coeff_planes, 2, anchor, region_w, region_h, base_ds, 0, steer);
    if(cl_err != CL_SUCCESS) goto out;
    cl_err = _cf_harmonic_fill_cl(devid, gd_void, coeff_r2, broad, region_w, region_h, base_ds, 0, steer);
    if(cl_err != CL_SUCCESS) goto out;
  }

  // evaluate the diffused fit against the measured guide; write est + fit score bsc
  {
    const int kernel = global_data->kernel_hl_cf_eval_pair;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &coeff_slope);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &coeff_intercept);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &coeff_r2);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(cl_mem), &model_quality);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &target_chan);
    dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(int), &guide_chan);
    dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(int), &other_chan);
    cl_err = _hl_cl_enq2d(devid, kernel, work_size);
  }

out:
  dt_opencl_release_mem_object(coeff_slope);
  dt_opencl_release_mem_object(coeff_intercept);
  dt_opencl_release_mem_object(coeff_r2);
  dt_opencl_release_mem_object(anchor);
  dt_opencl_release_mem_object(broad);
  return cl_err;
}

// Variant of the joint stage that fits and DIFFUSES the coefficient fields but defers the
// evaluation: the caller keeps the four returned buffers (slope a, slope b, offset d, fit
// quality r2) to evaluate later in the deep-channel cascade (deep-channel stash).
// Caller releases the returned cl_mem buffers. Mirrors the deferred joint fit inside
// _region_guided_filter (CPU): any change here must be mirrored there and re-validated with
// the HL_CFCL_TEST self-tests.
static cl_int _cf_joint_fit_cl(const int devid, void *gd_void, cl_mem estimate, cl_mem valid, cl_mem mom0,
                               cl_mem mom1, cl_mem mom2, cl_mem steer, const float *const restrict channel_means,
                               const int region_w, const int region_h, const float cf_sigma, const float cf_fmin,
                               const int c, const int guide1, const int guide2, cl_mem *ca_out, cl_mem *cb_out,
                               cl_mem *cd_out, cl_mem *cr2_out)
{
  dt_iop_highlights_global_data_t *global_data = (dt_iop_highlights_global_data_t *)gd_void;
  const size_t region_pixels = (size_t)region_w * region_h;
  cl_int cl_err = DT_OPENCL_DEFAULT_ERROR;
  size_t work_size[3] = { ROUNDUPDWD(region_w, devid), ROUNDUPDHT(region_h, devid), 1 };

  cl_mem coeff_slope_a = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem coeff_slope_b = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem coeff_offset = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem coeff_r2 = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem anchor = dt_opencl_alloc_device_buffer(devid, region_pixels);
  cl_mem broad = dt_opencl_alloc_device_buffer(devid, region_pixels);
  *ca_out = *cb_out = *cd_out = *cr2_out = NULL;
  if(!coeff_slope_a || !coeff_slope_b || !coeff_offset || !coeff_r2 || !anchor || !broad) goto out;

  {
    const int kernel = global_data->kernel_hl_cf_fit_joint;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &mom0);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &mom1);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &mom2);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &c);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &guide1);
    dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &guide2);
    dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(float), &cf_fmin);
    dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(cl_mem), &coeff_slope_a);
    dt_opencl_set_kernel_arg(devid, kernel, 11, sizeof(cl_mem), &coeff_slope_b);
    dt_opencl_set_kernel_arg(devid, kernel, 12, sizeof(cl_mem), &coeff_offset);
    dt_opencl_set_kernel_arg(devid, kernel, 13, sizeof(cl_mem), &coeff_r2);
    dt_opencl_set_kernel_arg(devid, kernel, 14, sizeof(cl_mem), &anchor);
    dt_opencl_set_kernel_arg(devid, kernel, 15, sizeof(cl_mem), &broad);
    dt_opencl_set_kernel_arg(devid, kernel, 16, sizeof(float), &channel_means[0]);
    dt_opencl_set_kernel_arg(devid, kernel, 17, sizeof(float), &channel_means[1]);
    dt_opencl_set_kernel_arg(devid, kernel, 18, sizeof(float), &channel_means[2]);
    cl_err = _hl_cl_enq2d(devid, kernel, work_size);
    if(cl_err != CL_SUCCESS) goto out;
  }

  {
    const int base_ds = (int)(cf_sigma / 4.f);
    cl_mem coeff_planes[3]
        = { coeff_slope_a, coeff_slope_b, coeff_offset }; // shared anchor mask -> one fused fill
    cl_err
        = _cf_harmonic_fill_cl_n(devid, gd_void, coeff_planes, 3, anchor, region_w, region_h, base_ds, 0, steer);
    if(cl_err != CL_SUCCESS) goto out;
    cl_err = _cf_harmonic_fill_cl(devid, gd_void, coeff_r2, broad, region_w, region_h, base_ds, 0, steer);
    if(cl_err != CL_SUCCESS) goto out;
  }

  *ca_out = coeff_slope_a;
  *cb_out = coeff_slope_b;
  *cd_out = coeff_offset;
  *cr2_out = coeff_r2;
  coeff_slope_a = coeff_slope_b = coeff_offset = coeff_r2 = NULL;
  cl_err = CL_SUCCESS;

out:
  dt_opencl_release_mem_object(coeff_slope_a);
  dt_opencl_release_mem_object(coeff_slope_b);
  dt_opencl_release_mem_object(coeff_offset);
  dt_opencl_release_mem_object(coeff_r2);
  dt_opencl_release_mem_object(anchor);
  dt_opencl_release_mem_object(broad);
  return cl_err;
}

// The COMPLETE coefficient-field stage on the GPU: joint fits (predict each clipped channel
// from the other two) with the deep channel deferred, pair fallbacks (single-guide fits),
// then the deferred deep-channel evaluation with the depth-split blend. cdeep is the channel
// with the most clipped pixels (host-side decision from region metadata); it is evaluated
// last so its guides are already reconstructed.
// Inputs: est (working red/green/blue/norm pixels), vld (validity mask), bsc (fit-quality
// score), lsb (frozen brightness plane for the fit weights); all float4 device buffers.
// Mirrors the coefficient-field stage of _region_guided_filter (CPU): any change here must be
// mirrored there and re-validated with the HL_CFCL_TEST self-tests (_cf_stage_cl_selftest).
static cl_int _cf_stage_cl(const int devid, void *gd_void, cl_mem estimate, cl_mem valid, cl_mem model_quality,
                           cl_mem luminance, cl_mem steer, const float *const restrict channel_means,
                           dt_gaussian_cl_t *gaussian, const int region_w, const int region_h,
                           const float cf_sigma, const float cf_fmin, const float cf_binv, const int cdeep)
{
  cl_int cl_err = CL_SUCCESS;
  cl_mem deep_a = NULL, deep_b = NULL, deep_d = NULL, deep_r2 = NULL;
  dt_iop_highlights_global_data_t *global_data = (dt_iop_highlights_global_data_t *)gd_void;
  size_t size[3] = { ROUNDUPDWD(region_w, devid), ROUNDUPDHT(region_h, devid), 1 };

  // the windowed moments are shared: the joint weight (all-valid x frozen bw_) does not depend
  // on the target channel and the evals only write CLIPPED channels, so estimate never changes at chan_a
  // weighted pixel -- packed + blur ONCE and fit the three channels from the same fields (the
  // per-channel repack the CPU does is redundant on both sides; here the blurs dominate)
  cl_mem packed = dt_opencl_alloc_device(devid, size[0], size[1], sizeof(float) * 4);
  cl_mem moment0 = dt_opencl_alloc_device(devid, size[0], size[1], sizeof(float) * 4);
  cl_mem moment1 = dt_opencl_alloc_device(devid, size[0], size[1], sizeof(float) * 4);
  cl_mem moment2 = dt_opencl_alloc_device(devid, size[0], size[1], sizeof(float) * 4);
  cl_mem moments[3];
  moments[0] = moment0;
  moments[1] = moment1;
  moments[2] = moment2;
  if(!packed || !moment0 || !moment1 || !moment2)
  {
    cl_err = DT_OPENCL_DEFAULT_ERROR;
    goto out;
  }
  for(int mode = 0; mode < 3 && cl_err == CL_SUCCESS; mode++)
  {
    const int kernel = global_data->kernel_hl_cf_pack_joint;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &luminance);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &packed);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(float), &cf_binv);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &mode);
    dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(float), &channel_means[0]);
    dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(float), &channel_means[1]);
    dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(float), &channel_means[2]);
    cl_err = _hl_cl_enq2d(devid, kernel, size);
    if(cl_err == CL_SUCCESS)
      cl_err = gaussian ? dt_gaussian_blur_cl(gaussian, packed, moments[mode])
                        : _region_blur_cl(devid, packed, moments[mode], region_w, region_h, cf_sigma);
  }
  if(cl_err != CL_SUCCESS) goto out;

  // joint fits: immediate strict eval for the non-deep channels, stash for the deep one
  for(int c = 0; c < 3; c++)
  {
    const int guide1 = (c == 0) ? 1 : 0;
    const int guide2 = (c == 2) ? 1 : 2;
    if(c == cdeep)
    {
      cl_err = _cf_joint_fit_cl(devid, gd_void, estimate, valid, moment0, moment1, moment2, steer, channel_means,
                                region_w, region_h, cf_sigma, cf_fmin, c, guide1, guide2, &deep_a, &deep_b,
                                &deep_d, &deep_r2);
    }
    else
      cl_err = _cf_joint_stage_cl(devid, gd_void, estimate, valid, model_quality, moment0, moment1, moment2, steer,
                                  channel_means, region_w, region_h, cf_sigma, cf_fmin, c, guide1, guide2);
    if(cl_err != CL_SUCCESS) goto out;
  }

  // pair fallbacks: the pair weight (both-valid x frozen bw_) is orientation-independent and
  // estimate at weighted pixels never changes, so packed each pair's moments once for both orientations
  for(int chan_a = 0; chan_a < 3; chan_a++)
    for(int chan_b = chan_a + 1; chan_b < 3; chan_b++)
    {
      for(int mode = 0; mode < 2 && cl_err == CL_SUCCESS; mode++)
      {
        const int kernel = global_data->kernel_hl_cf_pack_pair;
        dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
        dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
        dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &luminance);
        dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &packed);
        dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_w);
        dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &region_h);
        dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(float), &cf_binv);
        dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &chan_a);
        dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &chan_b);
        dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(int), &mode);
        dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(float), &channel_means[chan_a]);
        dt_opencl_set_kernel_arg(devid, kernel, 11, sizeof(float), &channel_means[chan_b]);
        cl_err = _hl_cl_enq2d(devid, kernel, size);
        if(cl_err == CL_SUCCESS)
          cl_err = gaussian
                       ? dt_gaussian_blur_cl(gaussian, packed, mode ? moment1 : moment0)
                       : _region_blur_cl(devid, packed, mode ? moment1 : moment0, region_w, region_h, cf_sigma);
      }
      for(int orientation = 0; orientation < 2 && cl_err == CL_SUCCESS; orientation++)
        cl_err
            = _cf_pair_stage_cl(devid, gd_void, estimate, valid, model_quality, moment0, moment1, steer,
                                channel_means, region_w, region_h, cf_sigma, cf_fmin, chan_a, chan_b, orientation);
      if(cl_err != CL_SUCCESS) goto out;
    }

  // deferred deep evaluation: blur the deep-channel validity masks (feathered depth split),
  // then evaluate the stashed coefficient fields now that the guide channels are reconstructed
  if(deep_a)
  {
    const int guide1 = (cdeep == 0) ? 1 : 0;
    const int guide2 = (cdeep == 2) ? 1 : 2;
    cl_mem packed = dt_opencl_alloc_device(devid, size[0], size[1], sizeof(float) * 4);
    cl_mem mask_blurred = dt_opencl_alloc_device(devid, size[0], size[1], sizeof(float) * 4);
    if(!packed || !mask_blurred)
    {
      dt_opencl_release_mem_object(packed);
      dt_opencl_release_mem_object(mask_blurred);
      cl_err = DT_OPENCL_DEFAULT_ERROR;
      goto out;
    }

    const int kernel_mask = global_data->kernel_hl_cf_pack_deepmask;
    dt_opencl_set_kernel_arg(devid, kernel_mask, 0, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 1, sizeof(cl_mem), &packed);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 2, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 3, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 4, sizeof(int), &cdeep);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 5, sizeof(int), &guide1);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 6, sizeof(int), &guide2);
    cl_err = _hl_cl_enq2d(devid, kernel_mask, size);
    if(cl_err == CL_SUCCESS)
      cl_err = gaussian ? dt_gaussian_blur_cl(gaussian, packed, mask_blurred)
                        : _region_blur_cl(devid, packed, mask_blurred, region_w, region_h, cf_sigma);

    if(cl_err == CL_SUCCESS)
    {
      const int kernel_eval = global_data->kernel_hl_cf_eval_deep;
      dt_opencl_set_kernel_arg(devid, kernel_eval, 0, sizeof(cl_mem), &deep_a);
      dt_opencl_set_kernel_arg(devid, kernel_eval, 1, sizeof(cl_mem), &deep_b);
      dt_opencl_set_kernel_arg(devid, kernel_eval, 2, sizeof(cl_mem), &deep_d);
      dt_opencl_set_kernel_arg(devid, kernel_eval, 3, sizeof(cl_mem), &deep_r2);
      dt_opencl_set_kernel_arg(devid, kernel_eval, 4, sizeof(cl_mem), &mask_blurred);
      dt_opencl_set_kernel_arg(devid, kernel_eval, 5, sizeof(cl_mem), &valid);
      dt_opencl_set_kernel_arg(devid, kernel_eval, 6, sizeof(cl_mem), &estimate);
      dt_opencl_set_kernel_arg(devid, kernel_eval, 7, sizeof(cl_mem), &model_quality);
      dt_opencl_set_kernel_arg(devid, kernel_eval, 8, sizeof(int), &region_w);
      dt_opencl_set_kernel_arg(devid, kernel_eval, 9, sizeof(int), &region_h);
      dt_opencl_set_kernel_arg(devid, kernel_eval, 10, sizeof(int), &cdeep);
      dt_opencl_set_kernel_arg(devid, kernel_eval, 11, sizeof(int), &guide1);
      dt_opencl_set_kernel_arg(devid, kernel_eval, 12, sizeof(int), &guide2);
      cl_err = _hl_cl_enq2d(devid, kernel_eval, size);
    }
    dt_opencl_release_mem_object(packed);
    dt_opencl_release_mem_object(mask_blurred);
  }

out:
  dt_opencl_release_mem_object(packed);
  dt_opencl_release_mem_object(moment0);
  dt_opencl_release_mem_object(moment1);
  dt_opencl_release_mem_object(moment2);
  dt_opencl_release_mem_object(deep_a);
  dt_opencl_release_mem_object(deep_b);
  dt_opencl_release_mem_object(deep_d);
  dt_opencl_release_mem_object(deep_r2);
  return cl_err;
}



// High-frequency (detail band) hybrid stage on the GPU: one lowpass blur of estimate (the detail
// band is estimate minus this lowpass), windowed moments of the detail band, per-channel gains
// shrunk by the fit quality (chan_a weak colour-line must not print the guides' fine texture),
// gains diffused across the clipped zone, minimum-energy blend of guided resynthesis vs the
// damped detail at strict targets, damped-only treatment at single-guide pixels.
// Mirrors the DT_HL_HF_GUIDE block of _region_guided_filter (CPU): any change here must be
// mirrored there and re-validated with the HL_HFCL_TEST self-test (_hf_stage_cl_selftest).
static cl_int _hf_stage_cl(const int devid, void *gd_void, cl_mem estimate, cl_mem valid, cl_mem model_quality,
                           cl_mem luminance, cl_mem steer, dt_gaussian_cl_t *gaussian, const int region_w,
                           const int region_h, const float cf_sigma, const float cf_fmin, const float cf_binv)
{
  dt_iop_highlights_global_data_t *global_data = (dt_iop_highlights_global_data_t *)gd_void;
  const size_t region_pixels = (size_t)region_w * region_h;
  cl_int cl_err = DT_OPENCL_DEFAULT_ERROR;
  size_t size[3] = { ROUNDUPDWD(region_w, devid), ROUNDUPDHT(region_h, devid), 1 };
  const float blur_sigma = fmaxf(cf_sigma / 4.f, 2.f);

  cl_mem packed = dt_opencl_alloc_device(devid, size[0], size[1], sizeof(float) * 4);
  cl_mem lowpass = dt_opencl_alloc_device(devid, size[0], size[1], sizeof(float) * 4);
  cl_mem moment0 = dt_opencl_alloc_device(devid, size[0], size[1], sizeof(float) * 4);
  cl_mem moment1 = dt_opencl_alloc_device(devid, size[0], size[1], sizeof(float) * 4);
  cl_mem moment2 = dt_opencl_alloc_device(devid, size[0], size[1], sizeof(float) * 4);
  cl_mem energy = dt_opencl_alloc_device(devid, size[0], size[1], sizeof(float) * 4);
  cl_mem moments[3];
  cl_mem gain_a = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem gain_b = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem anchor = dt_opencl_alloc_device_buffer(devid, region_pixels);
  moments[0] = moment0;
  moments[1] = moment1;
  moments[2] = moment2;
  if(!packed || !lowpass || !moment0 || !moment1 || !moment2 || !energy || !gain_a || !gain_b || !anchor) goto out;

  // lowpass of estimate (computed ONCE, shared by every channel and the damped path)
  {
    const int kernel = global_data->kernel_hl_buf_to_img;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &packed);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region_h);
    cl_err = _hl_cl_enq2d(devid, kernel, size);
    if(cl_err != CL_SUCCESS) goto out;
    cl_err = _region_blur_cl(devid, packed, lowpass, region_w, region_h, blur_sigma);
    if(cl_err != CL_SUCCESS) goto out;
  }

  // windowed moments of the detail band (estimate minus lowpass), packed then blurred
  for(int mode = 0; mode < 3; mode++)
  {
    const int kernel = global_data->kernel_hl_hf_pack;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &luminance);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &lowpass);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &packed);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(float), &cf_binv);
    dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &mode);
    cl_err = _hl_cl_enq2d(devid, kernel, size);
    if(cl_err != CL_SUCCESS) goto out;
    cl_err = gaussian ? dt_gaussian_blur_cl(gaussian, packed, moments[mode])
                      : _region_blur_cl(devid, packed, moments[mode], region_w, region_h, cf_sigma);
    if(cl_err != CL_SUCCESS) goto out;
  }

  // per channel: fit detail-band gains, diffuse them, measure both candidates' energy, evaluate
  for(int c = 0; c < 3; c++)
  {
    const int guide1 = (c == 0) ? 1 : 0;
    const int guide2 = (c == 2) ? 1 : 2;

    // fit the quality-shrunk detail-band gains (gain_a, gain_b) and the anchor mask
    {
      const int kernel = global_data->kernel_hl_hf_fit;
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &moment0);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &moment1);
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &moment2);
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &valid);
      dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_w);
      dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &region_h);
      dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &c);
      dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &guide1);
      dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &guide2);
      dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(float), &cf_fmin);
      dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(cl_mem), &gain_a);
      dt_opencl_set_kernel_arg(devid, kernel, 11, sizeof(cl_mem), &gain_b);
      dt_opencl_set_kernel_arg(devid, kernel, 12, sizeof(cl_mem), &anchor);
      cl_err = _hl_cl_enq2d(devid, kernel, size);
      if(cl_err != CL_SUCCESS) goto out;
    }

    // harmonic diffusion of the two gain fields across the clipped zone
    const int base_ds = (int)(cf_sigma / 4.f);
    cl_mem gain_pair[2] = { gain_a, gain_b }; // shared anchor mask -> one fused fill
    cl_err = _cf_harmonic_fill_cl_n(devid, gd_void, gain_pair, 2, anchor, region_w, region_h, base_ds, 0, steer);
    if(cl_err != CL_SUCCESS) goto out;

    // local energy of the guided vs damped detail candidates (blurred absolute values)
    {
      const int kernel = global_data->kernel_hl_hf_energy;
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &model_quality);
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &lowpass);
      dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &gain_a);
      dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(cl_mem), &gain_b);
      dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(cl_mem), &packed);
      dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &region_w);
      dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &region_h);
      dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(int), &c);
      dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(int), &guide1);
      dt_opencl_set_kernel_arg(devid, kernel, 11, sizeof(int), &guide2);
      cl_err = _hl_cl_enq2d(devid, kernel, size);
      if(cl_err != CL_SUCCESS) goto out;
      cl_err = _region_blur_cl(devid, packed, energy, region_w, region_h, blur_sigma);
      if(cl_err != CL_SUCCESS) goto out;
    }

    // minimum-energy blend of the two candidates at strict (both-guides-valid) targets
    {
      const int kernel = global_data->kernel_hl_hf_eval;
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &model_quality);
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &lowpass);
      dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &energy);
      dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(cl_mem), &gain_a);
      dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(cl_mem), &gain_b);
      dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &region_w);
      dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &region_h);
      dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(int), &c);
      dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(int), &guide1);
      dt_opencl_set_kernel_arg(devid, kernel, 11, sizeof(int), &guide2);
      cl_err = _hl_cl_enq2d(devid, kernel, size);
      if(cl_err != CL_SUCCESS) goto out;
    }
  }

  // single-guide pixels: only the quality-damped detail (no guided resynthesis possible)
  {
    const int kernel = global_data->kernel_hl_hf_damp;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &model_quality);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &lowpass);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &region_h);
    cl_err = _hl_cl_enq2d(devid, kernel, size);
  }

out:
  dt_opencl_release_mem_object(packed);
  dt_opencl_release_mem_object(lowpass);
  dt_opencl_release_mem_object(moment0);
  dt_opencl_release_mem_object(moment1);
  dt_opencl_release_mem_object(moment2);
  dt_opencl_release_mem_object(energy);
  dt_opencl_release_mem_object(gain_a);
  dt_opencl_release_mem_object(gain_b);
  dt_opencl_release_mem_object(anchor);
  return cl_err;
}


// GPU counterpart of _region_blur: the exact same Young-van-Vliet recursive gaussian, through
// the existing dt_gaussian OpenCL implementation. in/out are 4-channel image2d of the region.
static dt_gaussian_cl_t *_region_blur_handle(const int devid, const int region_w, const int region_h,
                                             const float sigma)
{
  const float vmax[4] = { 1e9f, 1e9f, 1e9f, 1e9f };
  const float vmin[4] = { -1e9f, -1e9f, -1e9f, -1e9f };
  return dt_gaussian_init_cl(devid, region_w, region_h, 4, vmax, vmin, sigma, 0);
}

// One-shot gaussian blur of a 4-channel region image on the GPU (init + blur + free).
// Mirrors _region_blur on the CPU: any change here must be mirrored there and re-validated
// with the HL_BLURCL_TEST self-test (_region_blur_cl_selftest).
static cl_int _region_blur_cl(const int devid, cl_mem in, cl_mem out, const int region_w, const int region_h,
                              const float sigma)
{
  dt_gaussian_cl_t *gaussian = _region_blur_handle(devid, region_w, region_h, sigma);
  if(!gaussian) return DT_OPENCL_DEFAULT_ERROR;
  const cl_int cl_err = dt_gaussian_blur_cl(gaussian, in, out);
  dt_gaussian_free_cl(gaussian);
  return cl_err;
}

#endif // HAVE_OPENCL
#include "common/solvers/sparse_cholesky_cl.h"

#ifdef HAVE_OPENCL
// Gather the sparse-Cholesky kernel handles from the highlights global data into the solver's
// kernel-handle struct (the solver is CFA-agnostic and owns no kernels of its own).
static inline _sp_chol_cl_kernels_t _hl_sp_chol_kernels(void *gd_void)
{
  const dt_iop_highlights_global_data_t *const global_data = (const dt_iop_highlights_global_data_t *)gd_void;
  _sp_chol_cl_kernels_t kernels
      = { global_data->kernel_sparse_chol_update_level, global_data->kernel_sparse_chol_final_level,
          global_data->kernel_sparse_chol_fwd_level, global_data->kernel_sparse_chol_bwd_level };
  return kernels;
}
#endif // HAVE_OPENCL

#if defined(HAVE_OPENCL) && DT_HL_SPARSE_SOLVE
// Biharmonic dome (smooth hill continuing the rim brightness into a fully-clipped area,
// smooth in value AND slope) as a GPU unit: coarse-grid reduction on device, tiny
// coarse-metadata download for the symbolic analysis and matrix assembly (<= a few hundred
// KB of COARSE cells, never full-res planes), GPU sparse Cholesky solve, bilinear upsample
// into the full-res holes on device.
// field/hole are full-res device buffers (float / uchar); ds (downsample factor) forced by
// the caller like the CPU _biharmonic_dome's force_ds.
// Mirrors _biharmonic_dome on the CPU: any change here must be mirrored there and
// re-validated with the HL_DOMECL_TEST self-test (_selfdome_stage_cl_selftest).
static cl_int _biharmonic_dome_cl(const int devid, void *gd_void, cl_mem field, cl_mem hole, const int region_w,
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
  uint8_t *coarse_hole = (uint8_t *)dt_alloc_align(coarse_pixels);
  int *idx = (int *)dt_alloc_align(sizeof(int) * coarse_pixels);
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
    cl_err = _hl_cl_enq2d(devid, kernel, work_size);
    if(cl_err != CL_SUCCESS) goto out;
  }

  // coarse metadata to host: assembly + symbolic analysis (integer work)
  cl_err = _hl_cl_read_timed(devid, cf, dval, 0, sizeof(float) * coarse_pixels, CL_TRUE);
  if(cl_err != CL_SUCCESS) goto out;
  cl_err = _hl_cl_read_timed(devid, coarse_hole, dhole, 0, coarse_pixels, CL_TRUE);
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
      // assemble the 13-point biharmonic operator (Laplacian applied twice), with the unknowns
      // permuted by geometric nested dissection (the CPU dome's exact system)
      static const int stencil_off_y[13] = { 0, -1, 1, 0, 0, -1, -1, 1, 1, -2, 2, 0, 0 };
      static const int stencil_off_x[13] = { 0, 0, 0, -1, 1, -1, 1, -1, 1, 0, 0, -2, 2 };
      static const double stencil_coef[13] = { 20., -8., -8., -8., -8., 2., 2., 2., 2., 1., 1., 1., 1. };

      int *unknown_x = (int *)dt_alloc_align(sizeof(int) * unknown_count);
      int *unknown_y = (int *)dt_alloc_align(sizeof(int) * unknown_count);
      int *perm = (int *)dt_alloc_align(sizeof(int) * unknown_count);
      int *inv_perm = (int *)dt_alloc_align(sizeof(int) * unknown_count);
      matrix_col_ptr = (int *)dt_alloc_align(sizeof(int) * (unknown_count + 1));
      matrix_row_index = (int *)dt_alloc_align(sizeof(int) * (size_t)unknown_count * 13);
      matrix_values = (double *)dt_alloc_align(sizeof(double) * (size_t)unknown_count * 13);
      rhs = (double *)dt_alloc_align(sizeof(double) * unknown_count);
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

        const double _tch = dt_get_wtime();
        factor = _sp_chol_factor_cl(devid, _hl_sp_chol_kernels(gd_void), unknown_count, matrix_col_ptr,
                                    matrix_row_index, matrix_values);
        _hl_cl_chol_s += dt_get_wtime() - _tch;
        _hl_cl_chol_n++;
        int solved = 0;
        if(factor)
        {
          cl_mem rhs_device = _sp_cl_upload(devid, rhs, sizeof(double) * unknown_count);
          if(rhs_device && !_sp_chol_solve_cl(factor, _hl_sp_chol_kernels(gd_void), rhs_device)
             && _hl_cl_read_timed(devid, rhs, rhs_device, 0, sizeof(double) * unknown_count, CL_TRUE) == CL_SUCCESS)
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
      dt_free_align(unknown_x);
      dt_free_align(unknown_y);
      dt_free_align(perm);
      dt_free_align(inv_perm);
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
      cl_err = _hl_cl_enq2d(devid, kernel, work_size);
    }
  }

out:
  dt_opencl_release_mem_object(dval);
  dt_opencl_release_mem_object(dhole);
  dt_opencl_release_mem_object(solution_device);
  dt_pixelpipe_cache_free_align(cf);
  dt_free_align(coarse_hole);
  dt_free_align(idx);
  dt_free_align(matrix_col_ptr);
  dt_free_align(matrix_row_index);
  dt_free_align(matrix_values);
  dt_free_align(rhs);
  _sp_chol_cl_free(factor);
  return cl_err;
}

// The hue-coupled self-dome stage on the GPU: soft clip floor (rounded lower bound at the
// clip level), ONE shared biharmonic brightness dome over the union hole, harmonically
// filled chromaticity ratios (each channel divided by the brightness sum), a depth-gated
// blend (dome takes over where the colour-line fit quality is low AND the pixel is shallow),
// then a hard clip-floor re-assert. est/vld/bsc/clip0/dep are device buffers (working pixels,
// validity mask, fit quality, clip thresholds, distance-to-valid depth).
// Mirrors the DT_HL_SELF_DOME block of _region_guided_filter (CPU): any change here must be
// mirrored there and re-validated with the HL_DOMECL_TEST self-test
// (_selfdome_stage_cl_selftest).
static cl_int _selfdome_stage_cl(const int devid, void *gd_void, cl_mem estimate, cl_mem valid,
                                 cl_mem model_quality, cl_mem clip0, cl_mem depth, const int region_w,
                                 const int region_h, const float cf_sigma, const float reg_radius,
                                 const int ds_shared, const dt_dev_pixelpipe_t *pipe)
{
  dt_iop_highlights_global_data_t *global_data = (dt_iop_highlights_global_data_t *)gd_void;
  const size_t region_pixels = (size_t)region_w * region_h;
  cl_int cl_err = DT_OPENCL_DEFAULT_ERROR;
  size_t size[3] = { ROUNDUPDWD(region_w, devid), ROUNDUPDHT(region_h, devid), 1 };
  const float epsilon = 1e-6f;

  cl_mem luminance = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem hole = dt_opencl_alloc_device_buffer(devid, region_pixels);
  cl_mem dome_lum = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem ratio0 = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem ratio1 = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem ratio2 = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem ratios[3];
  ratios[0] = ratio0;
  ratios[1] = ratio1;
  ratios[2] = ratio2;
  if(!luminance || !hole || !dome_lum || !ratio0 || !ratio1 || !ratio2) goto out;

  // soft floor first (production order: floor -> dome gate -> self dome)
  {
    const int kernel = global_data->kernel_hl_soft_floor;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &clip0);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_h);
    cl_err = _hl_cl_enq2d(devid, kernel, size);
    if(cl_err != CL_SUCCESS) goto out;
  }

  // brightness plane (sum of the three channels) + union hole mask (any clipped channel)
  {
    const int kernel = global_data->kernel_hl_lsb_hole;
    const int allmode = 0; // union hole: ANY clipped channel
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &luminance);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &hole);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &allmode);
    cl_err = _hl_cl_enq2d(devid, kernel, size);
    if(cl_err != CL_SUCCESS) goto out;
  }

  // debug dump (HL_REG_DUMP=<file path>): save this region's brightness plane + hole mask
  // to the given file for offline replay through the HL_DOMECL_TEST self-test (the path is
  // taken from the variable itself: no fixed world-writable location)
  const char *reg_dump_path = getenv("HL_REG_DUMP");
  if(reg_dump_path && reg_dump_path[0])
  {
    float *dump_data = dt_pixelpipe_cache_alloc_align_float(region_pixels, pipe);
    uint8_t *dump_hole = (uint8_t *)dt_alloc_align(region_pixels);
    if(dump_data && dump_hole
       && _hl_cl_read_timed(devid, dump_data, luminance, 0, sizeof(float) * region_pixels, CL_TRUE) == CL_SUCCESS
       && _hl_cl_read_timed(devid, dump_hole, hole, 0, region_pixels, CL_TRUE) == CL_SUCCESS)
    {
      FILE *dump_file = g_fopen(reg_dump_path, "wb");
      if(dump_file)
      {
        fwrite(&region_w, sizeof(int), 1, dump_file);
        fwrite(&region_h, sizeof(int), 1, dump_file);
        const int downsample_val = ds_shared;
        fwrite(&downsample_val, sizeof(int), 1, dump_file);
        fwrite(dump_data, sizeof(float), region_pixels, dump_file);
        fwrite(dump_hole, 1, region_pixels, dump_file);
        fclose(dump_file);
      }
    }
    dt_pixelpipe_cache_free_align(dump_data);
    dt_free_align(dump_hole);
  }
  // shared biharmonic brightness dome over the union hole (GPU sparse Cholesky inside)
  cl_err
      = dt_opencl_enqueue_copy_buffer_to_buffer(devid, luminance, dome_lum, 0, 0, sizeof(float) * region_pixels);
  if(cl_err != CL_SUCCESS) goto out;
  cl_err = _biharmonic_dome_cl(devid, gd_void, dome_lum, hole, region_w, region_h, ds_shared, pipe);
  if(cl_err != CL_SUCCESS) goto out;

  // harmonically filled chromaticity ratios over the union hole
  {
    const int cf_base = (int)(CLAMP(reg_radius / 6.f, 8.f, 64.f) / 4.f);
    for(int c = 0; c < 3 && cl_err == CL_SUCCESS; c++)
    {
      const int kernel = global_data->kernel_hl_ratio_plane;
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &luminance);
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &ratios[c]);
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region_w);
      dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_h);
      dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &c);
      dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(float), &epsilon);
      cl_err = _hl_cl_enq2d(devid, kernel, size);
      if(cl_err == CL_SUCCESS)
        cl_err = _cf_harmonic_fill_cl(devid, gd_void, ratios[c], hole, region_w, region_h, cf_base, 1, NULL);
    }
    if(cl_err != CL_SUCCESS) goto out;
  }

  // depth-gated blend: dome value x filled ratios replaces the estimate where the fit is
  // doubtful and the pixel is shallow enough for the dome to be trustworthy
  {
    const int kernel = global_data->kernel_hl_dome_blend;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &model_quality);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &depth);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &dome_lum);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(cl_mem), &ratio0);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(cl_mem), &ratio1);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(cl_mem), &ratio2);
    dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(cl_mem), &hole);
    dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel, 11, sizeof(float), &cf_sigma);
    dt_opencl_set_kernel_arg(devid, kernel, 12, sizeof(float), &epsilon);
    cl_err = _hl_cl_enq2d(devid, kernel, size);
    if(cl_err != CL_SUCCESS) goto out;
  }

  // hard floor re-assert: a clipped channel saturated, so its true value is >= its clip level
  {
    const int kernel = global_data->kernel_hl_hard_floor;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &clip0);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_h);
    cl_err = _hl_cl_enq2d(devid, kernel, size);
  }

out:
  dt_opencl_release_mem_object(luminance);
  dt_opencl_release_mem_object(hole);
  dt_opencl_release_mem_object(dome_lum);
  dt_opencl_release_mem_object(ratio0);
  dt_opencl_release_mem_object(ratio1);
  dt_opencl_release_mem_object(ratio2);
  return cl_err;
}

#endif // HAVE_OPENCL && DT_HL_SPARSE_SOLVE
#if defined(HAVE_OPENCL) && DT_HL_SPARSE_SOLVE
static void _biharmonic_dome(float *const restrict field, const uint8_t *const restrict hole, const int region_w,
                             const int region_h, const int force_ds, const dt_dev_pixelpipe_t *pipe);


#endif // HAVE_OPENCL && DT_HL_SPARSE_SOLVE (selftest)
#if defined(HAVE_OPENCL) && DT_HL_SPARSE_SOLVE
// single-channel gaussian blur on the device (used to feather the joint-core composite weight)
static cl_int _region_blur1_cl(const int devid, cl_mem in, cl_mem out, const int region_w, const int region_h,
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


// Matrix-free conjugate gradient (iterative solver) on the device, mirroring
// _region_pde_solve for the joint core's screened-harmonic chroma (order 1, lam 1, constant
// reaction strength d and flat target): the fallback when the all-clip core exceeds
// DT_HL_SPARSE_MAX unknowns and the direct factorization is off the table.
// Dot products accumulate in double precision on the device (64-bit-float program); only the
// 2 KB of reduction partials cross the bus per iteration. The CPU conjugate gradient is
// itself OpenMP-summation-order nondeterministic, so tolerance-level (not bit-exact) parity
// is the honest target here. Any change here must be mirrored in _region_pde_solve and
// re-validated with the HL_CORECL_TEST self-test (_joint_core_stage_cl_selftest).
static cl_int _region_pde_cg_cl(const int devid, void *gd_void, cl_mem solution, cl_mem hole, const int region_w,
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
    cl_err = _hl_cl_enq2d(devid, kernel, work_size);                                                              \
    if(cl_err != CL_SUCCESS) goto out;                                                                            \
    const int kernel_op = global_data->kernel_hl_cg_op;                                                           \
    dt_opencl_set_kernel_arg(devid, kernel_op, 0, sizeof(cl_mem), &temp1);                                        \
    dt_opencl_set_kernel_arg(devid, kernel_op, 1, sizeof(cl_mem), &temp2);                                        \
    dt_opencl_set_kernel_arg(devid, kernel_op, 2, sizeof(int), &region_w);                                        \
    dt_opencl_set_kernel_arg(devid, kernel_op, 3, sizeof(int), &region_h);                                        \
    cl_err = _hl_cl_enq2d(devid, kernel_op, work_size);                                                           \
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
    cl_err = _hl_cl_enq2d(devid, kernel, work_size);
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
    cl_err = _hl_cl_enq2dl(devid, kernel, work_size_1d, local_size_1d);
    if(cl_err != CL_SUCCESS) goto out;
    cl_err = _hl_cl_read_timed(devid, partial_sums, partials, 0, sizeof(double) * n_groups, CL_TRUE);
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
    _hl_cl_cg_n++;
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
      cl_err = _hl_cl_enq2dl(devid, kernel, work_size_1d, local_size_1d);
      if(cl_err != CL_SUCCESS) goto out;
      cl_err = _hl_cl_read_timed(devid, partial_sums, partials, 0, sizeof(double) * n_groups, CL_TRUE);
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
      cl_err = _hl_cl_enq2dl(devid, kernel, work_size_1d, local_size_1d);
      if(cl_err != CL_SUCCESS) goto out;
      cl_err = _hl_cl_read_timed(devid, partial_sums, partials, 0, sizeof(double) * n_groups, CL_TRUE);
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
      cl_err = _hl_cl_enq2d(devid, kernel, work_size);
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

// All-clipped joint core on the device (pixels where NO channel survived, so no guide
// exists): shared biharmonic brightness dome (floored at the saturated sum) x
// screened-Poisson diffused chromaticity (diffusion plus a pull toward the mean valid
// colour, strength set by the "inpaint a flat colour" user slider) -- ONE host symbolic
// analysis + GPU numeric factorization serves the three channels, whose right-hand sides are
// assembled on the device (hl_pde_rhs) so no full-res float plane crosses the bus --
// composed through the gaussian-feathered core mask. The only downloads are the full-res
// hole MASK (bytes; the sparse symbolic analysis needs it anyway) and the per-workgroup
// mean-chromaticity partial sums.
// Mirrors the all-clip joint-core block of _region_guided_filter (CPU, DT_HL_COEFF_FIELD):
// any change here must be mirrored there and re-validated with the HL_CORECL_TEST self-test
// (_joint_core_stage_cl_selftest).
static cl_int _joint_core_stage_cl(const int devid, void *gd_void, cl_mem estimate, cl_mem valid, cl_mem clip0,
                                   const int region_w, const int region_h, const float solid_color,
                                   const float reg_radius, const int extent, const dt_dev_pixelpipe_t *pipe)
{
  dt_iop_highlights_global_data_t *global_data = (dt_iop_highlights_global_data_t *)gd_void;
  const size_t region_pixels = (size_t)region_w * region_h;
  cl_int cl_err = DT_OPENCL_DEFAULT_ERROR;
  size_t size[3] = { ROUNDUPDWD(region_w, devid), ROUNDUPDHT(region_h, devid), 1 };
  const float epsilon = 1e-6f;
  const float react = solid_color * solid_color * 4.f;

  if(global_data->kernel_hl_pde_rhs < 0 || global_data->kernel_hl_pde_scatter < 0) return cl_err; // no fp64 device

  cl_mem luminance = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem hole = dt_opencl_alloc_device_buffer(devid, region_pixels);
  cl_mem dome_lum = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem embedded = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem ratio0 = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem ratio1 = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem ratio2 = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem cg_field = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem ratios[3];
  ratios[0] = ratio0;
  ratios[1] = ratio1;
  ratios[2] = ratio2;
  cl_mem partial_sums = NULL, perm_grid_dev = NULL, rhs_dev = NULL, mask_img = NULL, mask_blur = NULL;
  uint8_t *hole_mask = (uint8_t *)dt_alloc_align(region_pixels);
  int *matrix_col_ptr = NULL, *matrix_row_index = NULL, *perm_grid = NULL;
  double *matrix_values = NULL;
  _sp_chol_cl_t *factor = NULL;
  dt_aligned_pixel_t chroma_mean = { 0.f, 0.f, 0.f, 0.f };
  if(!luminance || !hole || !dome_lum || !embedded || !ratio0 || !ratio1 || !ratio2 || !cg_field || !hole_mask)
    goto out;

  // luminance + ALL-clip hole (no surviving channel)
  {
    const int kernel = global_data->kernel_hl_lsb_hole;
    const int all_clip_mode = 1;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &luminance);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &hole);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &all_clip_mode);
    cl_err = _hl_cl_enq2d(devid, kernel, size);
    if(cl_err != CL_SUCCESS) goto out;
  }

  // the sparse symbolic analysis needs the mask on the host anyway; it also gives the
  // all-clip count for the early exit and the CPU's auto grid factor for the dome
  cl_err = _hl_cl_read_timed(devid, hole_mask, hole, 0, region_pixels, CL_TRUE);
  if(cl_err != CL_SUCCESS) goto out;

  size_t n_hole_fine = 0;
  for(size_t i = 0; i < region_pixels; i++)
    if(hole_mask[i]) n_hole_fine++;
  if(n_hole_fine == 0)
  {
    cl_err = CL_SUCCESS;
    goto out;
  }
  const int downsample = MAX(1, (int)ceilf(sqrtf((float)n_hole_fine / (float)DT_HL_DOME_NMAX_SPARSE)));

  // shared biharmonic luminance dome, floored at "all channels at clip"
  cl_err
      = dt_opencl_enqueue_copy_buffer_to_buffer(devid, luminance, dome_lum, 0, 0, sizeof(float) * region_pixels);
  if(cl_err != CL_SUCCESS) goto out;
  cl_err = _biharmonic_dome_cl(devid, gd_void, dome_lum, hole, region_w, region_h, downsample, pipe);
  if(cl_err != CL_SUCCESS) goto out;
  {
    const int kernel = global_data->kernel_hl_core_floor;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &dome_lum);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &hole);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &clip0);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_h);
    cl_err = _hl_cl_enq2d(devid, kernel, size);
    if(cl_err != CL_SUCCESS) goto out;
  }

  // mean valid chromaticity: device partial sums, host finish
  {
    const int local_size = 64, n_groups = 256;
    const int n_pixels = (int)region_pixels;
    partial_sums = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 4 * n_groups);
    if(!partial_sums)
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
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &partial_sums);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &n_pixels);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(float), &epsilon);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(float) * 4 * local_size, NULL);
    cl_err = _hl_cl_enq2dl(devid, kernel, sizes, local);
    if(cl_err != CL_SUCCESS) goto out;

    float partial_host[4 * 256];
    cl_err = _hl_cl_read_timed(devid, partial_host, partial_sums, 0, sizeof(float) * 4 * n_groups, CL_TRUE);
    if(cl_err != CL_SUCCESS) goto out;
    double accum[4] = { 0.0, 0.0, 0.0, 0.0 };
    for(int group = 0; group < n_groups; group++)
      for(int k = 0; k < 4; k++) accum[k] += (double)partial_host[group * 4 + k];
    if(accum[3] > 0.0)
      for(int c = 0; c < 3; c++) chroma_mean[c] = (float)(accum[c] / accum[3]);
  }

  // ONE symbolic analysis + GPU numeric factorization for the three channels; when the core
  // exceeds DT_HL_SPARSE_MAX (or the factorization fails) take the same road as the CPU:
  // the matrix-free CG, here fully on the device
  int n_unknowns = 0;
  int use_cg = !_sp_pde_assemble(hole_mask, NULL, (react > 0.f) ? react : 0.f, 1, 1.f, region_w, region_h,
                                 &matrix_col_ptr, &matrix_row_index, &matrix_values, &perm_grid, &n_unknowns);
  if(!use_cg)
  {
    const double _tch = dt_get_wtime();
    factor = _sp_chol_factor_cl(devid, _hl_sp_chol_kernels(gd_void), n_unknowns, matrix_col_ptr, matrix_row_index,
                                matrix_values);
    _hl_cl_chol_s += dt_get_wtime() - _tch;
    _hl_cl_chol_n++;
    perm_grid_dev = factor ? _sp_cl_upload(devid, perm_grid, sizeof(int) * n_unknowns) : NULL;
    rhs_dev = factor ? dt_opencl_alloc_device_buffer(devid, sizeof(double) * n_unknowns) : NULL;
    if(!factor)
      use_cg = 1;
    else if(!perm_grid_dev || !rhs_dev)
    {
      cl_err = DT_OPENCL_DEFAULT_ERROR;
      goto out;
    }
  }
  const int max_iter = CLAMP(2 * extent, 200, 2000);

  // per channel: build the chromaticity ratio plane, solve its diffusion system, store into ratios[c]
  for(int c = 0; c < 3; c++)
  {
    // init: ratio plane on valid pixels, flat-colour seed on the hole (cg_field = solver unknown)
    {
      const int kernel = global_data->kernel_hl_pde_init;
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &luminance);
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &hole);
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &embedded);
      dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &ratios[c]);
      dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(cl_mem), &cg_field);
      dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &region_w);
      dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &region_h);
      dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &c);
      dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(float), &chroma_mean[c]);
      dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(float), &epsilon);
      cl_err = _hl_cl_enq2d(devid, kernel, size);
      if(cl_err != CL_SUCCESS) goto out;
    }
    // direct path: assemble this channel's right-hand side on the device...
    if(!use_cg)
    {
      const int kernel = global_data->kernel_hl_pde_rhs;
      size_t size_1d[3] = { ROUNDUP(n_unknowns, 64), 1, 1 };
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &embedded);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &perm_grid_dev);
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &rhs_dev);
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &n_unknowns);
      dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_w);
      dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &region_h);
      dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(float), &react);
      dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(float), &chroma_mean[c]);
      cl_err = _hl_cl_enq2d(devid, kernel, size_1d);
      if(cl_err != CL_SUCCESS) goto out;
    }
    // ...solve with the shared Cholesky factor...
    if(!use_cg)
    {
      if(_sp_chol_solve_cl(factor, _hl_sp_chol_kernels(gd_void), rhs_dev))
      {
        cl_err = DT_OPENCL_DEFAULT_ERROR;
        goto out;
      }
      // validate before scattering: the device factor kernel takes sqrt() of the pivots
      // without checking their sign, so a system whose replicate-clamped border rows are not
      // positive definite yields quiet NaN -- the CPU factor REJECTS such systems and falls
      // back to conjugate gradient, and the device path must degrade the same way instead of
      // blending NaN into the output. n_unknowns <= 16384 doubles = at most 128 KB on the bus.
      double *solution_check = (double *)dt_alloc_align(sizeof(double) * n_unknowns);
      int finite = (solution_check != NULL);
      if(solution_check)
      {
        finite = (_hl_cl_read_timed(devid, solution_check, rhs_dev, 0, sizeof(double) * n_unknowns, CL_TRUE)
                  == CL_SUCCESS);
        for(int check_index = 0; finite && check_index < n_unknowns; check_index++)
          if(!isfinite(solution_check[check_index])) finite = 0;
        dt_free_align(solution_check);
      }
      if(!finite)
      {
        _sp_chol_cl_free(factor);
        factor = NULL;
        use_cg = 1; // this channel and the remaining ones take the iterative road
      }
      else
      {
        // ...and scatter the solution back into the ratio plane
        const int kernel = global_data->kernel_hl_pde_scatter;
        size_t size_1d[3] = { ROUNDUP(n_unknowns, 64), 1, 1 };
        dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &rhs_dev);
        dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &perm_grid_dev);
        dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &ratios[c]);
        dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &n_unknowns);
        cl_err = _hl_cl_enq2d(devid, kernel, size_1d);
        if(cl_err != CL_SUCCESS) goto out;
        continue;
      }
    }
    // iterative road: on-device conjugate gradient on the seeded unknown, then clamp
    // the ratios non-negative (also the recovery path when the direct solve was rejected)
    {
      cl_err = _region_pde_cg_cl(devid, gd_void, cg_field, hole, region_w, region_h, (react > 0.f) ? react : 0.f,
                                 (react > 0.f) ? chroma_mean[c] : 0.f, max_iter);
      if(cl_err != CL_SUCCESS) goto out;
      const int kernel = global_data->kernel_hl_relu;
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &cg_field);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &ratios[c]);
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &region_w);
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region_h);
      cl_err = _hl_cl_enq2d(devid, kernel, size);
      if(cl_err != CL_SUCCESS) goto out;
    }
  }

  // feathered composite: blur the core mask and blend dome x ratios into estimate through it
  // (no hard hand-off at the core rim)
  mask_img = dt_opencl_alloc_device(devid, region_w, region_h, sizeof(float));
  mask_blur = dt_opencl_alloc_device(devid, region_w, region_h, sizeof(float));
  if(!mask_img || !mask_blur)
  {
    cl_err = DT_OPENCL_DEFAULT_ERROR;
    goto out;
  }
  {
    const int kernel = global_data->kernel_hl_mask_to_img1;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &hole);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &mask_img);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region_h);
    cl_err = _hl_cl_enq2d(devid, kernel, size);
    if(cl_err != CL_SUCCESS) goto out;
  }
  cl_err = _region_blur1_cl(devid, mask_img, mask_blur, region_w, region_h,
                            fmaxf(4.f, CLAMP(reg_radius / 6.f, 8.f, 64.f) / 4.f));
  if(cl_err != CL_SUCCESS) goto out;
  {
    const int kernel = global_data->kernel_hl_core_blend;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &hole);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &dome_lum);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &ratio0);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(cl_mem), &ratio1);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(cl_mem), &ratio2);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(cl_mem), &mask_blur);
    dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(int), &region_h);
    dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(float), &epsilon);
    cl_err = _hl_cl_enq2d(devid, kernel, size);
  }

out:
  dt_opencl_release_mem_object(luminance);
  dt_opencl_release_mem_object(hole);
  dt_opencl_release_mem_object(dome_lum);
  dt_opencl_release_mem_object(embedded);
  dt_opencl_release_mem_object(ratio0);
  dt_opencl_release_mem_object(ratio1);
  dt_opencl_release_mem_object(ratio2);
  dt_opencl_release_mem_object(cg_field);
  dt_opencl_release_mem_object(partial_sums);
  dt_opencl_release_mem_object(perm_grid_dev);
  dt_opencl_release_mem_object(rhs_dev);
  dt_opencl_release_mem_object(mask_img);
  dt_opencl_release_mem_object(mask_blur);
  dt_free_align(hole_mask);
  dt_free_align(matrix_col_ptr);
  dt_free_align(matrix_row_index);
  dt_free_align(matrix_values);
  dt_free_align(perm_grid);
  _sp_chol_cl_free(factor);
  return cl_err;
}

static inline void _knee_blur(const float *const restrict in, float *const restrict out, const int width,
                              const int height, const float sigma);

#endif // HAVE_OPENCL && DT_HL_SPARSE_SOLVE
#if defined(HAVE_OPENCL) && DT_HL_SPARSE_SOLVE && (DT_HL_ANISO_SOLVER == 2)

// Explicit coarse-to-fine structure-steered diffusion on the device, mirroring the CPU
// pyramid (_aniso_tensor + _aniso_iterate in the DT_HL_ANISO_CHROMA block) that handles cores
// beyond DT_HL_SPARSE_MAX unknowns: each level box-downsamples the brightness/ratios/holes,
// rebuilds the structure tensor (local edge direction and strength), runs 240 damped stencil
// steps per channel over the hole bounding box (ping-pong buffers instead of the CPU's
// write-back copy), and bilinearly splats the ratios into the fine planes' clipped channels
// to seed the next level. Any change here must be mirrored in the CPU pyramid and
// re-validated with the HL_ANISOCL_TEST self-test (_aniso_stage_cl_selftest).
static cl_int _aniso_pyramid_cl(const int devid, void *gd_void, cl_mem ratios, cl_mem valid, cl_mem luminance,
                                cl_mem clip0, const int region_w, const int region_h, const float radius,
                                const int box_x_lo, const int box_y_lo, const int box_x_hi, const int box_y_hi,
                                const dt_dev_pixelpipe_t *pipe)
{
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
      cl_err = _hl_cl_enq2d(devid, kernel, size_coarse);
    }

    // structure tensor of this level's luminance (box3 x2, gradient + mean magnitude, D)
    if(cl_err == CL_SUCCESS)
    {
      const int kernel = global_data->kernel_hl_box3;
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &coarse_lum);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &grad_x);
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &coarse_w);
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &coarse_h);
      cl_err = _hl_cl_enq2d(devid, kernel, size_coarse);
      if(cl_err == CL_SUCCESS)
      {
        dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &grad_x);
        dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &tensor_xx);
        cl_err = _hl_cl_enq2d(devid, kernel, size_coarse);
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
      cl_err = _hl_cl_enq2dl(devid, kernel, sizes, local);
      if(cl_err == CL_SUCCESS)
      {
        float partial_sums[256];
        cl_err = _hl_cl_read_timed(devid, partial_sums, grad_partials, 0, sizeof(float) * n_groups, CL_TRUE);
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
          cl_err = _hl_cl_enq2d(devid, kernel_tensor, size_coarse);
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
          cl_err = _hl_cl_enq2d(devid, kernel, size_coarse);
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
          cl_err = _hl_cl_enq2d(devid, kernel, size_coarse);
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
          cl_err = _hl_cl_enq2dl(devid, kernel, size_block, local_block);
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
            cl_err = _hl_cl_enq2d(devid, kernel, size_box);
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
          cl_err = _hl_cl_enq2d(devid, kernel, size_coarse);
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
      cl_err = _hl_cl_enq2d(devid, kernel, size_full);
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

// Divergence-form structure-steered chroma diffusion on the device (smooth the colour ratios
// along image edges, never across them), mirroring the DT_HL_ANISO_CHROMA production block
// with _aniso_div_solve: the structure tensor is computed on the GPU from the recovered
// brightness, and the host downloads only the all-clip mask (the sparse symbolic analysis
// needs it) plus the COMPACT per-unknown 8-edge weight list -- the matrix values -- for the
// exact CPU assembly; the three right-hand sides are built on-device from the same weight
// buffer, so no full-res float plane crosses the bus. Any change here must be mirrored in
// _aniso_div_solve (CPU) and re-validated with the HL_ANISOCL_TEST self-test
// (_aniso_stage_cl_selftest).
static cl_int _aniso_stage_cl(const int devid, void *gd_void, cl_mem estimate, cl_mem valid, cl_mem clip0,
                              const int region_w, const int region_h, const float radius,
                              const dt_dev_pixelpipe_t *pipe)
{
  dt_iop_highlights_global_data_t *global_data = (dt_iop_highlights_global_data_t *)gd_void;
  const size_t region_pixels = (size_t)region_w * region_h;
  cl_int cl_err = DT_OPENCL_DEFAULT_ERROR;
  size_t size[3] = { ROUNDUPDWD(region_w, devid), ROUNDUPDHT(region_h, devid), 1 };
  const float epsilon = 1e-6f;

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
  uint8_t *hole_mask = (uint8_t *)dt_alloc_align(region_pixels);
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
    cl_err = _hl_cl_enq2d(devid, kernel, size);
    if(cl_err != CL_SUCCESS) goto out;
  }

  cl_err = _hl_cl_read_timed(devid, hole_mask, hole, 0, region_pixels, CL_TRUE);
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
    cl_err = _hl_cl_enq2d(devid, kernel, size);
    if(cl_err != CL_SUCCESS) goto out;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &scratch1);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &scratch2);
    cl_err = _hl_cl_enq2d(devid, kernel, size);
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
    cl_err = _hl_cl_enq2dl(devid, kernel, sizes, local);
    if(cl_err != CL_SUCCESS) goto out;

    float psum[256];
    cl_err = _hl_cl_read_timed(devid, psum, partials, 0, sizeof(float) * n_groups, CL_TRUE);
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
    cl_err = _hl_cl_enq2d(devid, kernel_tensor, size);
    if(cl_err != CL_SUCCESS) goto out;
  }
  // tensor now lives in (scratch1, scratch2, tensor_yy) = (tensor_xx, tensor_xy, tensor_yy)

  // host symbolic: unknown list + ND ordering (reach 1: 8-neighbour stencil)
  grid_to_unknown = (int *)dt_alloc_align(sizeof(int) * region_pixels);
  unknown_to_grid = (int *)dt_alloc_align(sizeof(int) * n_unknowns);
  unknown_x = (int *)dt_alloc_align(sizeof(int) * n_unknowns);
  unknown_y = (int *)dt_alloc_align(sizeof(int) * n_unknowns);
  perm = (int *)dt_alloc_align(sizeof(int) * n_unknowns);
  inverse_perm = (int *)dt_alloc_align(sizeof(int) * n_unknowns);
  matrix_col_ptr = (int *)dt_alloc_align(sizeof(int) * (n_unknowns + 1));
  perm_grid = (int *)dt_alloc_align(sizeof(int) * n_unknowns);
  edge_weights = (float *)dt_alloc_align(sizeof(float) * (size_t)n_unknowns * 8);
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
    cl_err = _hl_cl_enq2d(devid, kernel, size_1d);
    if(cl_err != CL_SUCCESS) goto out;
  }
  cl_err = _hl_cl_read_timed(devid, edge_weights, edge_weights_dev, 0, sizeof(float) * (size_t)n_unknowns * 8,
                             CL_TRUE);
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
        matrix_row_index = (int *)dt_alloc_align(sizeof(int) * total);
        matrix_values = (double *)dt_alloc_align(sizeof(double) * total);
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
          matrix_values[matrix_col_ptr[perm_index] + n_col_entries] = diag;
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

  const double _tch = dt_get_wtime();
  factor = _sp_chol_factor_cl(devid, _hl_sp_chol_kernels(gd_void), n_unknowns, matrix_col_ptr, matrix_row_index,
                              matrix_values);
  _hl_cl_chol_s += dt_get_wtime() - _tch;
  _hl_cl_chol_n++;
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
      cl_err = _hl_cl_enq2d(devid, kernel, size_1d);
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
      cl_err = _hl_cl_enq2d(devid, kernel, size_1d);
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
      cl_err = _hl_cl_enq2d(devid, kernel, size);
      if(cl_err == CL_SUCCESS)
      {
        dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &scratch1);
        dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &scratch2);
        cl_err = _hl_cl_enq2d(devid, kernel, size);
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
      cl_err = _hl_cl_enq2dl(devid, kernel, sizes, local);
      if(cl_err == CL_SUCCESS)
      {
        float partial_sums[256];
        cl_err = _hl_cl_read_timed(devid, partial_sums, ppart, 0, sizeof(float) * 256, CL_TRUE);
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
          cl_err = _hl_cl_enq2d(devid, kernel_tensor, size);
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
      cl_err = _hl_cl_enq2d(devid, kernel, size);
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
        cl_err = _hl_cl_enq2d(devid, kernel, size);
      }
      if(cl_err == CL_SUCCESS) cl_err = _hl_cl_read_timed(devid, active, aflags, 0, sizeof(int) * 3, CL_TRUE);
    }

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
        cl_err = _hl_cl_enq2d(devid, kernel, size);
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
        cl_err = _hl_cl_enq2d(devid, kernel, size);
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
        cl_err = _hl_cl_enq2d(devid, kernel, size_box);
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
        cl_err = _hl_cl_enq2d(devid, kernel, size);
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
    cl_err = _hl_cl_enq2d(devid, kernel, size);
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
  dt_free_align(hole_mask);
  dt_free_align(grid_to_unknown);
  dt_free_align(unknown_to_grid);
  dt_free_align(unknown_x);
  dt_free_align(unknown_y);
  dt_free_align(perm);
  dt_free_align(inverse_perm);
  dt_free_align(matrix_col_ptr);
  dt_free_align(matrix_row_index);
  dt_free_align(matrix_values);
  dt_free_align(perm_grid);
  dt_free_align(edge_weights);
  _sp_chol_cl_free(factor);
  return cl_err;
}

#endif // HAVE_OPENCL && DT_HL_SPARSE_SOLVE && ANISO_SOLVER 2
#if defined(HAVE_OPENCL) && DT_HL_COEFF_FIELD && DT_HL_SPARSE_SOLVE && (DT_HL_ANISO_SOLVER == 2)
// Device counterpart (per-region GPU orchestrator) of _region_guided_filter: gathers the
// padded region window, derives the stage parameters from one on-device reduction
// (union-hole plateau brightness -> cf_binv, per-channel clip counts -> deep channel, union
// count -> shared dome grid), then chains the proven stages -- coefficient field
// (_cf_stage_cl), high-frequency detail hybrid (_hf_stage_cl), floors + gated self-dome
// (_selfdome_stage_cl), all-clip joint core (_joint_core_stage_cl), divergence-form
// anisotropic chroma (_aniso_stage_cl) -- and scatters the clipped channels back. Everything
// stays on the device except the reduction partials. Caller must handle noise_level > 0 on
// the CPU (the grain epilogue is not ported).
// Any change here must be mirrored in _region_guided_filter (CPU) and re-validated with the
// HL_REGCL_TEST self-test (_region_guided_filter_cl_selftest).
// Regions below this pixel count are reconstructed on the CPU even when the pipe runs on the
// GPU: a device region pays ~1000 kernel launches (iterative stages, per-level sparse solves)

// CPU offload of one region inside the GPU middle: gather the padded window to host, run the
// production CPU reconstruction on it (coordinates translated to the window), scatter back.
// Sequentially consistent with the device regions: the blocking readback drains the in-order
// queue, and the unpack rewrites the exact window the CPU read.
static cl_int _region_cpu_offload_cl(const int devid, void *gd_void, cl_mem interp, cl_mem mask, cl_mem depth,
                                     const int width, const _hl_region_t *const region,
                                     const dt_dev_pixelpipe_t *pipe, const float solid_color, const int max_iter,
                                     const float noise_level)
{
  dt_iop_highlights_global_data_t *global_data = (dt_iop_highlights_global_data_t *)gd_void;
  const int region_w = region->rx1 - region->rx0 + 1;
  const int region_h = region->ry1 - region->ry0 + 1;
  if(region_w < 2 || region_h < 2) return CL_SUCCESS;
  const size_t region_pixels = (size_t)region_w * region_h;

  cl_int cl_err = DT_OPENCL_DEFAULT_ERROR;
  size_t work_size[3] = { ROUNDUPDWD(region_w, devid), ROUNDUPDHT(region_h, devid), 1 };

  cl_mem staging = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels * 9);
  float *host = dt_pixelpipe_cache_alloc_align_float(region_pixels * 9, pipe);
  if(!staging || IS_NULL_PTR(host)) goto out;

  {
    const int kernel = global_data->kernel_hl_window_pack;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &interp);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &mask);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &depth);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &staging);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &region->rx0);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &region->ry0);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &region_h);
    cl_err = _hl_cl_enq2d(devid, kernel, work_size);
    if(cl_err != CL_SUCCESS) goto out;
  }

  cl_err = _hl_cl_read_timed(devid, host, staging, 0, sizeof(float) * region_pixels * 9, CL_TRUE);
  if(cl_err != CL_SUCCESS) goto out;

  {
    float *const hw_interp = host;
    const float *const hw_mask = host + region_pixels * 4;
    const float *const hw_depth = host + region_pixels * 8;

    _hl_region_t translated_region = *region;
    translated_region.x0 -= region->rx0;
    translated_region.x1 -= region->rx0;
    translated_region.y0 -= region->ry0;
    translated_region.y1 -= region->ry0;
    translated_region.rx1 -= region->rx0;
    translated_region.ry1 -= region->ry0;
    translated_region.rx0 = 0;
    translated_region.ry0 = 0;

    _region_guided_filter(hw_interp, hw_mask, hw_depth, region_w, &translated_region, pipe, solid_color, max_iter,
                          noise_level);
  }

  cl_err = dt_opencl_write_buffer_to_device(devid, host, staging, 0, sizeof(float) * region_pixels * 4, CL_TRUE);
  if(cl_err != CL_SUCCESS) goto out;

  {
    const int kernel = global_data->kernel_hl_window_unpack;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &staging);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &interp);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region->rx0);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region->ry0);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &region_h);
    cl_err = _hl_cl_enq2d(devid, kernel, work_size);
  }

out:
  dt_opencl_release_mem_object(staging);
  dt_pixelpipe_cache_free_align(host);
  return cl_err;
}

static cl_int _region_guided_filter_cl(const int devid, void *gd_void, cl_mem interp, cl_mem mask, cl_mem depth,
                                       const int width, const _hl_region_t *const region,
                                       const dt_dev_pixelpipe_t *pipe, const float solid_color)
{
  dt_iop_highlights_global_data_t *global_data = (dt_iop_highlights_global_data_t *)gd_void;
  const int region_w = region->rx1 - region->rx0 + 1;
  const int region_h = region->ry1 - region->ry0 + 1;
  if(region_w < 2 || region_h < 2) return CL_SUCCESS;
  const size_t region_pixels = (size_t)region_w * region_h;
  if(region_pixels > (size_t)64 * 1024 * 1024) return CL_SUCCESS;

  cl_int cl_err = DT_OPENCL_DEFAULT_ERROR;
  size_t work_size[3] = { ROUNDUPDWD(region_w, devid), ROUNDUPDHT(region_h, devid), 1 };
  dt_gaussian_cl_t *cf_gaussian = NULL;

  cl_mem estimate = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels * 4);
  cl_mem valid = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels * 4);
  cl_mem clip0 = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels * 4);
  cl_mem model_quality = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels * 4);
  cl_mem clip_depth = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
  cl_mem lsb0 = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels); // pre-ladder luminance
  cl_mem partials = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 8 * 256);
  cl_mem steer = NULL; // coefficient-fill steering plane (guide structure)
  if(!estimate || !valid || !clip0 || !model_quality || !clip_depth || !lsb0 || !partials) goto out;

  // gather the padded region window into contiguous device buffers (est/clip0/vld/dep/lsb0)
  {
    const int kernel = global_data->kernel_hl_region_gather;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &interp);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &mask);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &depth);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &clip0);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(cl_mem), &clip_depth);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(cl_mem), &model_quality);
    dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(cl_mem), &lsb0);
    dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(int), &region->rx0);
    dt_opencl_set_kernel_arg(devid, kernel, 11, sizeof(int), &region->ry0);
    dt_opencl_set_kernel_arg(devid, kernel, 12, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 13, sizeof(int), &region_h);
    cl_err = _hl_cl_enq2d(devid, kernel, work_size);
    if(cl_err != CL_SUCCESS) goto out;
  }

  // ladder parameters from the pre-ladder statistics
  const float cf_sigma = CLAMP(region->radius / 6.f, 8.f, 64.f);
  const float cf_fmin = 0.05f;
  float cf_binv;
  float channel_means[3] = { 0.f, 0.f, 0.f }; // per-channel valid means (moment-pack centering)
  int cdeep, ds_shared;
  {
    const int local_size = 64, n_groups = 256;
    const int pixel_count = (int)region_pixels;
    const int kernel = global_data->kernel_hl_region_stats;
    size_t sizes[3] = { (size_t)n_groups * local_size, 1, 1 };
    size_t local[3] = { local_size, 1, 1 };
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &partials);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &pixel_count);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(float) * 8 * local_size, NULL);
    cl_err = _hl_cl_enq2dl(devid, kernel, sizes, local);
    if(cl_err != CL_SUCCESS) goto out;

    float partials_host[8 * 256];
    cl_err = _hl_cl_read_timed(devid, partials_host, partials, 0, sizeof(float) * 8 * n_groups, CL_TRUE);
    if(cl_err != CL_SUCCESS) goto out;
    double lsum = 0.0, lcnt = 0.0, clip_count_r = 0.0, clip_count_g = 0.0, clip_count_b = 0.0;
    double msum[3] = { 0.0, 0.0, 0.0 };
    for(int group = 0; group < n_groups; group++)
    {
      lsum += (double)partials_host[8 * group + 0];
      lcnt += (double)partials_host[8 * group + 1];
      clip_count_r += (double)partials_host[8 * group + 2];
      clip_count_g += (double)partials_host[8 * group + 3];
      clip_count_b += (double)partials_host[8 * group + 4];
      msum[0] += (double)partials_host[8 * group + 5];
      msum[1] += (double)partials_host[8 * group + 6];
      msum[2] += (double)partials_host[8 * group + 7];
    }
    if(lcnt <= 0.0)
    {
      cl_err = CL_SUCCESS; // no clipped pixel in this window: nothing to do
      goto out;
    }
    const float cf_lref = (float)(lsum / lcnt);
    cf_binv = (cf_lref > 1e-9f) ? 1.f / (0.35f * cf_lref) : 0.f;
    cdeep = (clip_count_r >= clip_count_g && clip_count_r >= clip_count_b)
                ? 0
                : ((clip_count_g >= clip_count_b) ? 1 : 2);
    ds_shared = MAX(1, (int)ceilf(sqrtf((float)lcnt / (float)DT_HL_DOME_NMAX_SPARSE)));
    // per-channel means of the VALID values: the moment packs are centered on them (see the
    // CPU counterpart for the cancellation rationale)
    const double valid_count_r = (double)region_pixels - clip_count_r,
                 valid_count_g = (double)region_pixels - clip_count_g,
                 valid_count_b = (double)region_pixels - clip_count_b;
    channel_means[0] = valid_count_r > 0.5 ? (float)(msum[0] / valid_count_r) : 0.f;
    channel_means[1] = valid_count_g > 0.5 ? (float)(msum[1] / valid_count_g) : 0.f;
    channel_means[2] = valid_count_b > 0.5 ? (float)(msum[2] / valid_count_b) : 0.f;
  }

  // one gaussian handle serves every cf_sigma blur of the region (each init allocates two
  // region-sized temp buffers -- 13+ per-blur re-allocations were pure churn)
  cf_gaussian = _region_blur_handle(devid, region_w, region_h, cf_sigma);

  // Steering plane for the coefficient fills = the measured guide structure, built ONCE here
  // (same est state as the CPU: after the saturation floor) and shared by the coefficient-field
  // and HF stages, exactly like the CPU path.
  {
    steer = dt_opencl_alloc_device_buffer(devid, sizeof(float) * region_pixels);
    if(steer)
    {
      const int kernel = global_data->kernel_hl_cfa_steer;
      const int pixel_count = (int)region_pixels;
      size_t work_size_1d[3] = { ROUNDUPDWD(pixel_count, devid), 1, 1 };
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &steer);
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &pixel_count);
      cl_err = _hl_cl_enq2d(devid, kernel, work_size_1d);
      if(cl_err != CL_SUCCESS) goto out;
    }
  }

  const double _tg1 = dt_get_wtime();
  cl_err = _cf_stage_cl(devid, gd_void, estimate, valid, model_quality, lsb0, steer, channel_means, cf_gaussian,
                        region_w, region_h, cf_sigma, cf_fmin, cf_binv, cdeep);
  if(cl_err == CL_SUCCESS) _hl_cl_finish_timed(devid);
  const double _tg2 = dt_get_wtime();
  if(cl_err != CL_SUCCESS) goto out;
  cl_err = _hf_stage_cl(devid, gd_void, estimate, valid, model_quality, lsb0, steer, cf_gaussian, region_w,
                        region_h, cf_sigma, cf_fmin, cf_binv);
  if(cl_err == CL_SUCCESS) _hl_cl_finish_timed(devid);
  const double _tg3 = dt_get_wtime();
  if(cl_err != CL_SUCCESS) goto out;

  // gated self-dome: the soft floor is unconditional (production applies it right after the
  // HF hybrid); the dome + blend + hard floor only run where a clipped channel with a
  // surviving guide sits on a weak colour-line
  int need_self = 0;
  {
    const int local_size = 64, n_groups = 256;
    const int pixel_count = (int)region_pixels;
    const int kernel = global_data->kernel_hl_need_self;
    size_t sizes[3] = { (size_t)n_groups * local_size, 1, 1 };
    size_t local[3] = { local_size, 1, 1 };
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &model_quality);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &clip_depth);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &partials);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &pixel_count);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(float), &cf_sigma);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(float) * local_size, NULL);
    cl_err = _hl_cl_enq2dl(devid, kernel, sizes, local);
    if(cl_err != CL_SUCCESS) goto out;
    float partials_host[256];
    cl_err = _hl_cl_read_timed(devid, partials_host, partials, 0, sizeof(float) * n_groups, CL_TRUE);
    if(cl_err != CL_SUCCESS) goto out;
    for(int group = 0; group < n_groups; group++)
      if(partials_host[group] > 0.f) need_self = 1;
  }

  if(need_self)
  {
    cl_err = _selfdome_stage_cl(devid, gd_void, estimate, valid, model_quality, clip0, clip_depth, region_w,
                                region_h, cf_sigma, region->radius, ds_shared, pipe);
    if(cl_err != CL_SUCCESS) goto out;
  }
  else
  {
    const int kernel = global_data->kernel_hl_soft_floor;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &valid);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &clip0);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region_h);
    cl_err = _hl_cl_enq2d(devid, kernel, work_size);
    if(cl_err != CL_SUCCESS) goto out;
  }

  {
    const int extent = MAX(region->x1 - region->x0, region->y1 - region->y0) + 1;
    cl_err = _joint_core_stage_cl(devid, gd_void, estimate, valid, clip0, region_w, region_h, solid_color,
                                  region->radius, extent, pipe);
    if(cl_err == CL_SUCCESS) _hl_cl_finish_timed(devid);
  }
  if(cl_err != CL_SUCCESS) goto out;
  const double _tg5 = dt_get_wtime();
  cl_err = _aniso_stage_cl(devid, gd_void, estimate, valid, clip0, region_w, region_h, region->radius, pipe);
  if(cl_err == CL_SUCCESS) _hl_cl_finish_timed(devid);
  const double _tg6 = dt_get_wtime();
  if(cl_err == CL_SUCCESS)
    dt_print(DT_DEBUG_PERF, "[highlights] gpu region %dx%d: cf=%.0fms hf=%.0fms dome+core=%.0fms aniso=%.0fms\n",
             region_w, region_h, (_tg2 - _tg1) * 1e3, (_tg3 - _tg2) * 1e3, (_tg5 - _tg3) * 1e3,
             (_tg6 - _tg5) * 1e3);
  if(cl_err != CL_SUCCESS) goto out;

  // scatter the reconstructed clipped channels back into the full-res buffer
  {
    const int kernel = global_data->kernel_hl_region_scatter;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &interp);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &mask);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &estimate);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &region->rx0);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &region->ry0);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &region_w);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &region_h);
    cl_err = _hl_cl_enq2d(devid, kernel, work_size);
  }

out:
  dt_gaussian_free_cl(cf_gaussian);
  dt_opencl_release_mem_object(estimate);
  dt_opencl_release_mem_object(valid);
  dt_opencl_release_mem_object(clip0);
  dt_opencl_release_mem_object(model_quality);
  dt_opencl_release_mem_object(clip_depth);
  dt_opencl_release_mem_object(lsb0);
  dt_opencl_release_mem_object(partials);
  dt_opencl_release_mem_object(steer);
  return cl_err;
}

#endif // HAVE_OPENCL && DT_HL_COEFF_FIELD && DT_HL_SPARSE_SOLVE && ANISO_SOLVER 2
#ifdef HAVE_OPENCL
// GPU knee estimation (sensor saturation-rolloff curve, see the DT_HL_KNEE macro comment):
// the device runs Phase A (colour-filter-array binning, the 5-sigma windowed moment blurs --
// packed 4 planes per gaussian pass -- and the colour-line regressions); the host keeps
// Phase B (vote medians + monotone curve fit) on the downloaded BINNED planes (<= 1.5 Mpx
// grid, x/pred/r2s/done only). The full-res raw mosaic never crosses the bus.
// Mirrors _hl_knee_estimate on the CPU: any change here must be mirrored there and
// re-validated with the HL_KNEECL_TEST self-test (_knee_cl_selftest).
static cl_int _hl_knee_estimate_cl(const int devid, void *gd_void, cl_mem dev_in, const size_t width,
                                   const size_t height, const uint32_t filters, const dt_iop_roi_t *const roi_in,
                                   cl_mem dev_xtrans, const int is_xtrans, const dt_aligned_pixel_t clipval_raw,
                                   _hl_knee_curve_t curves[3], const dt_dev_pixelpipe_t *pipe)
{
  dt_iop_highlights_global_data_t *global_data = (dt_iop_highlights_global_data_t *)gd_void;
  cl_int cl_err = DT_OPENCL_DEFAULT_ERROR;
  dt_gaussian_cl_t *gsig = NULL; // one blur handle per sigma (9 blurs each)

  for(int c = 0; c < 3; c++)
  {
    curves[c].engaged = 0;
    memset(curves[c].lift, 0, sizeof(curves[c].lift));
  }

  const int base = is_xtrans ? 6 : 2;
  int downsample = 1;
  while((width / ((size_t)base * downsample)) * (height / ((size_t)base * downsample)) > 1500000) downsample++;

  const int quad_size = base * downsample;
  const size_t bin_w = width / quad_size;
  const size_t bin_h = height / quad_size;
  const size_t bin_pixels = bin_w * bin_h;
  if(bin_w < 16 || bin_h < 16) return CL_SUCCESS; // like the CPU: no estimate, identity curves

  const int bin_w_int = (int)bin_w, bin_h_int = (int)bin_h;
  size_t work_sizes[3] = { ROUNDUPDWD(bin_w_int, devid), ROUNDUPDHT(bin_h_int, devid), 1 };

  cl_mem dev_binned = dt_opencl_alloc_device_buffer(devid, sizeof(float) * bin_pixels * 3);
  cl_mem dev_pred = dt_opencl_alloc_device_buffer(devid, sizeof(float) * bin_pixels * 3);
  cl_mem dev_r2 = dt_opencl_alloc_device_buffer(devid, sizeof(float) * bin_pixels * 3);
  cl_mem dev_done = dt_opencl_alloc_device_buffer(devid, bin_pixels * 3);
  cl_mem moment_a = dt_opencl_alloc_device(devid, bin_w_int, bin_h_int, 4 * sizeof(float));
  cl_mem moment_b = dt_opencl_alloc_device(devid, bin_w_int, bin_h_int, 4 * sizeof(float));
  cl_mem moment_c = dt_opencl_alloc_device(devid, bin_w_int, bin_h_int, 4 * sizeof(float));
  cl_mem blur_a = dt_opencl_alloc_device(devid, bin_w_int, bin_h_int, 4 * sizeof(float));
  cl_mem blur_b = dt_opencl_alloc_device(devid, bin_w_int, bin_h_int, 4 * sizeof(float));
  cl_mem blur_c = dt_opencl_alloc_device(devid, bin_w_int, bin_h_int, 4 * sizeof(float));
  float *binned = dt_pixelpipe_cache_alloc_align_float(bin_pixels * 3, pipe);
  float *pred = dt_pixelpipe_cache_alloc_align_float(bin_pixels * 3, pipe);
  float *r2_scores = dt_pixelpipe_cache_alloc_align_float(bin_pixels * 3, pipe);
  float *votes = dt_pixelpipe_cache_alloc_align_float(bin_pixels, pipe);
  uint8_t *done = calloc(bin_pixels * 3, sizeof(uint8_t));
  if(!dev_binned || !dev_pred || !dev_r2 || !dev_done || !moment_a || !moment_b || !moment_c || !blur_a || !blur_b
     || !blur_c || !binned || !pred || !r2_scores || !votes || !done)
    goto cleanup;

  cl_err = dt_opencl_write_buffer_to_device(devid, done, dev_done, 0, bin_pixels * 3, CL_TRUE);
  if(cl_err != CL_SUCCESS) goto cleanup;

  // ---- binning ----
  {
    const int kernel = global_data->kernel_hl_knee_bin;
    const int width_int = (int)width;
    const int height_int = (int)height;
    const int quad_size_int = quad_size;
    const int roi_x = roi_in ? roi_in->x : 0;
    const int roi_y = roi_in ? roi_in->y : 0;
    const cl_float4 clip4 = { { clipval_raw[0], clipval_raw[1], clipval_raw[2], 1.f } };
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &dev_in);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &dev_binned);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &width_int);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &height_int);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &bin_w_int);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &bin_h_int);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &quad_size_int);
    dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(uint32_t), &filters);
    dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &roi_x);
    dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(int), &roi_y);
    dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(int), &is_xtrans);
    dt_opencl_set_kernel_arg(devid, kernel, 11, sizeof(cl_mem), &dev_xtrans);
    dt_opencl_set_kernel_arg(devid, kernel, 12, sizeof(cl_float4), &clip4);
    cl_err = _hl_cl_enq2d(devid, kernel, work_sizes);
    if(cl_err != CL_SUCCESS) goto cleanup;
  }

  // the binned planes come home once: Phase B needs them, and they carry the band mass
  cl_err = _hl_cl_read_timed(devid, binned, dev_binned, 0, sizeof(float) * bin_pixels * 3, CL_TRUE);
  if(cl_err != CL_SUCCESS) goto cleanup;

  size_t nband[3] = { 0, 0, 0 };
  for(size_t pixel = 0; pixel < bin_pixels; pixel++)
    for(int c = 0; c < 3; c++)
      if(binned[c * bin_pixels + pixel] >= DT_HL_KNEE_LO && binned[c * bin_pixels + pixel] < DT_HL_KNEE_DET)
        nband[c]++;

  if(nband[0] < 200 && nband[1] < 200 && nband[2] < 200)
  {
    cl_err = CL_SUCCESS;
    goto cleanup;
  }

  // ---- Phase A: multi-scale windowed regressions on the device ----
  {
    const float sigmas[DT_HL_KNEE_NSIGMAS] = { 4.f, 8.f, 16.f, 32.f, 64.f };
    const float knee_lo = DT_HL_KNEE_LO;
    const float knee_det = DT_HL_KNEE_DET;
    const float knee_fmin = DT_HL_KNEE_FMIN;

    for(int sigma_index = 0; sigma_index < DT_HL_KNEE_NSIGMAS; sigma_index++)
    {
      const float sigma = sigmas[sigma_index];
      dt_gaussian_free_cl(gsig); // previous sigma's handle
      gsig = NULL;

      // joint moments (10 planes packed in 3 float4 images), blurred at this sigma
      {
        const int kernel = global_data->kernel_hl_knee_jmom;
        dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &dev_binned);
        dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &moment_a);
        dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &moment_b);
        dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &moment_c);
        dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &bin_w_int);
        dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &bin_h_int);
        dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(float), &knee_lo);
        cl_err = _hl_cl_enq2d(devid, kernel, work_sizes);
        if(cl_err != CL_SUCCESS) goto cleanup;
      }
      gsig = _region_blur_handle(devid, bin_w_int, bin_h_int, sigma);
      if(!gsig)
      {
        cl_err = DT_OPENCL_DEFAULT_ERROR;
        goto cleanup;
      }
      cl_err = dt_gaussian_blur_cl(gsig, moment_a, blur_a);
      if(cl_err == CL_SUCCESS) cl_err = dt_gaussian_blur_cl(gsig, moment_b, blur_b);
      if(cl_err == CL_SUCCESS) cl_err = dt_gaussian_blur_cl(gsig, moment_c, blur_c);
      if(cl_err != CL_SUCCESS) goto cleanup;

      for(int c = 0; c < 3; c++)
      {
        if(nband[c] < 200) continue;
        const int guide1 = (c == 0) ? 1 : 0;
        const int guide2 = (c == 2) ? 1 : 2;
        const int kernel = global_data->kernel_hl_knee_joint_reg;
        dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &dev_binned);
        dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &blur_a);
        dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &blur_b);
        dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &blur_c);
        dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &dev_pred);
        dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(cl_mem), &dev_r2);
        dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(cl_mem), &dev_done);
        dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &bin_w_int);
        dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &bin_h_int);
        dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(int), &c);
        dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(int), &guide1);
        dt_opencl_set_kernel_arg(devid, kernel, 11, sizeof(int), &guide2);
        dt_opencl_set_kernel_arg(devid, kernel, 12, sizeof(float), &knee_lo);
        dt_opencl_set_kernel_arg(devid, kernel, 13, sizeof(float), &knee_det);
        dt_opencl_set_kernel_arg(devid, kernel, 14, sizeof(float), &knee_fmin);
        cl_err = _hl_cl_enq2d(devid, kernel, work_sizes);
        if(cl_err != CL_SUCCESS) goto cleanup;
      }

      // single-guide fallback, both orientations of each pair
      for(int chan_a = 0; chan_a < 3; chan_a++)
        for(int chan_b = chan_a + 1; chan_b < 3; chan_b++)
        {
          if(nband[chan_a] < 200 && nband[chan_b] < 200) continue;
          {
            const int kernel = global_data->kernel_hl_knee_pmom;
            dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &dev_binned);
            dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &moment_a);
            dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &moment_b);
            dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &bin_w_int);
            dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &bin_h_int);
            dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &chan_a);
            dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &chan_b);
            dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(float), &knee_lo);
            cl_err = _hl_cl_enq2d(devid, kernel, work_sizes);
            if(cl_err != CL_SUCCESS) goto cleanup;
          }
          cl_err = dt_gaussian_blur_cl(gsig, moment_a, blur_a);
          if(cl_err == CL_SUCCESS) cl_err = dt_gaussian_blur_cl(gsig, moment_b, blur_b);
          if(cl_err != CL_SUCCESS) goto cleanup;

          for(int orient = 0; orient < 2; orient++)
          {
            const int target_ch = orient ? chan_b : chan_a;
            const int guide_ch = orient ? chan_a : chan_b;
            const int is_first_orient = (orient == 0);
            if(nband[target_ch] < 200) continue;
            const int kernel = global_data->kernel_hl_knee_pair_reg;
            dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &dev_binned);
            dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &blur_a);
            dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &blur_b);
            dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &dev_pred);
            dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &dev_r2);
            dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(cl_mem), &dev_done);
            dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &bin_w_int);
            dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &bin_h_int);
            dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &target_ch);
            dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(int), &guide_ch);
            dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(int), &is_first_orient);
            dt_opencl_set_kernel_arg(devid, kernel, 11, sizeof(float), &knee_lo);
            dt_opencl_set_kernel_arg(devid, kernel, 12, sizeof(float), &knee_det);
            dt_opencl_set_kernel_arg(devid, kernel, 13, sizeof(float), &knee_fmin);
            cl_err = _hl_cl_enq2d(devid, kernel, work_sizes);
            if(cl_err != CL_SUCCESS) goto cleanup;
          }
        }
    }
  }

  cl_err = _hl_cl_read_timed(devid, pred, dev_pred, 0, sizeof(float) * bin_pixels * 3, CL_TRUE);
  if(cl_err == CL_SUCCESS)
    cl_err = _hl_cl_read_timed(devid, r2_scores, dev_r2, 0, sizeof(float) * bin_pixels * 3, CL_TRUE);
  if(cl_err == CL_SUCCESS) cl_err = _hl_cl_read_timed(devid, done, dev_done, 0, bin_pixels * 3, CL_TRUE);
  if(cl_err != CL_SUCCESS) goto cleanup;

  // ---- Phase B (host, identical to the CPU): vote medians + monotone curve fit ----
  for(int c = 0; c < 3; c++)
  {
    if(nband[c] < 200) continue;

    size_t count[DT_HL_KNEE_BINS] = { 0 };
    size_t offset[DT_HL_KNEE_BINS + 1] = { 0 };
    const float bin_width = (DT_HL_KNEE_DET - DT_HL_KNEE_LO) / (float)DT_HL_KNEE_BINS;

    for(size_t pixel = 0; pixel < bin_pixels; pixel++)
    {
      if(!done[c * bin_pixels + pixel] || r2_scores[c * bin_pixels + pixel] <= DT_HL_KNEE_R2MIN) continue;
      const int bin_index
          = CLAMP((int)((binned[c * bin_pixels + pixel] - DT_HL_KNEE_LO) / bin_width), 0, DT_HL_KNEE_BINS - 1);
      count[bin_index]++;
    }
    for(int i = 0; i < DT_HL_KNEE_BINS; i++) offset[i + 1] = offset[i] + count[i];

    size_t fill[DT_HL_KNEE_BINS];
    memcpy(fill, offset, sizeof(fill));
    for(size_t pixel = 0; pixel < bin_pixels; pixel++)
    {
      if(!done[c * bin_pixels + pixel] || r2_scores[c * bin_pixels + pixel] <= DT_HL_KNEE_R2MIN) continue;
      const float x_val = binned[c * bin_pixels + pixel];
      const int bin_index = CLAMP((int)((x_val - DT_HL_KNEE_LO) / bin_width), 0, DT_HL_KNEE_BINS - 1);
      votes[fill[bin_index]++] = pred[c * bin_pixels + pixel] - x_val;
    }

    float lift[DT_HL_KNEE_BINS];
    int seen[DT_HL_KNEE_BINS];
    int nseen = 0;
    for(int i = 0; i < DT_HL_KNEE_BINS; i++)
    {
      lift[i] = 0.f;
      seen[i] = 0;
      if(count[i] < DT_HL_KNEE_MINVOTES) continue;
      float *const bin_votes = votes + offset[i];
      const float median_lift = _knee_median(bin_votes, count[i]);
      for(size_t k = 0; k < count[i]; k++) bin_votes[k] = fabsf(bin_votes[k] - median_lift);
      const float median_abs_dev = _knee_median(bin_votes, count[i]);
      const float std_err = 1.858f * median_abs_dev / sqrtf((float)count[i]);
      seen[i] = 1;
      nseen++;
      if(median_lift > DT_HL_KNEE_NSIGMA * std_err) lift[i] = median_lift;
    }
    if(nseen < 3) continue;

    int prev = -1;
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

    float running_max = 0.f;
    float lift_max = 0.f;
    for(int i = 0; i < DT_HL_KNEE_BINS; i++)
    {
      running_max = fmaxf(running_max, fmaxf(lift[i], 0.f));
      curves[c].lift[i] = running_max;
      lift_max = fmaxf(lift_max, running_max);
    }
    curves[c].engaged = (lift_max >= DT_HL_KNEE_ENGAGE);
    if(!curves[c].engaged) memset(curves[c].lift, 0, sizeof(curves[c].lift));
  }
  cl_err = CL_SUCCESS;

cleanup:
  dt_gaussian_free_cl(gsig);
  dt_opencl_release_mem_object(dev_binned);
  dt_opencl_release_mem_object(dev_pred);
  dt_opencl_release_mem_object(dev_r2);
  dt_opencl_release_mem_object(dev_done);
  dt_opencl_release_mem_object(moment_a);
  dt_opencl_release_mem_object(moment_b);
  dt_opencl_release_mem_object(moment_c);
  dt_opencl_release_mem_object(blur_a);
  dt_opencl_release_mem_object(blur_b);
  dt_opencl_release_mem_object(blur_c);
  dt_pixelpipe_cache_free_align(binned);
  dt_pixelpipe_cache_free_align(pred);
  dt_pixelpipe_cache_free_align(r2_scores);
  dt_pixelpipe_cache_free_align(votes);
  free(done);
  return cl_err;
}

// Apply the engaged knee curves to the raw mosaic (colour filter array) on the device.
// Mirrors _hl_knee_apply_cfa on the CPU: any change here must be mirrored there and
// re-validated with the HL_KNEECL_TEST self-test (_knee_cl_selftest).
static cl_int _hl_knee_apply_cfa_cl(const int devid, void *gd_void, cl_mem dev_in, cl_mem dev_out,
                                    const size_t width, const size_t height, const uint32_t filters,
                                    const dt_iop_roi_t *const roi_in, cl_mem dev_xtrans, const int is_xtrans,
                                    const dt_aligned_pixel_t clipval_raw, const _hl_knee_curve_t curves[3])
{
  dt_iop_highlights_global_data_t *global_data = (dt_iop_highlights_global_data_t *)gd_void;
  const int width_int = (int)width;
  const int height_int = (int)height;
  size_t work_sizes[3] = { ROUNDUPDWD(width_int, devid), ROUNDUPDHT(height_int, devid), 1 };

  float lift[3 * DT_HL_KNEE_BINS];
  for(int c = 0; c < 3; c++) memcpy(lift + c * DT_HL_KNEE_BINS, curves[c].lift, sizeof(curves[c].lift));
  cl_mem dev_lift = _sp_cl_upload(devid, lift, sizeof(lift));
  if(!dev_lift) return DT_OPENCL_DEFAULT_ERROR;

  const int kernel = global_data->kernel_hl_knee_apply;
  const int roi_x = roi_in ? roi_in->x : 0;
  const int roi_y = roi_in ? roi_in->y : 0;
  const cl_float4 clip4 = { { clipval_raw[0], clipval_raw[1], clipval_raw[2], 1.f } };
  const cl_int4 engaged_flags = { { curves[0].engaged, curves[1].engaged, curves[2].engaged, 0 } };
  const float knee_lo = DT_HL_KNEE_LO;
  const float knee_det = DT_HL_KNEE_DET;
  const int bins = DT_HL_KNEE_BINS;
  dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &dev_in);
  dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &dev_out);
  dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &width_int);
  dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &height_int);
  dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(uint32_t), &filters);
  dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &roi_x);
  dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &roi_y);
  dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &is_xtrans);
  dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(cl_mem), &dev_xtrans);
  dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(cl_float4), &clip4);
  dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(cl_mem), &dev_lift);
  dt_opencl_set_kernel_arg(devid, kernel, 11, sizeof(cl_int4), &engaged_flags);
  dt_opencl_set_kernel_arg(devid, kernel, 12, sizeof(float), &knee_lo);
  dt_opencl_set_kernel_arg(devid, kernel, 13, sizeof(float), &knee_det);
  dt_opencl_set_kernel_arg(devid, kernel, 14, sizeof(int), &bins);
  const cl_int cl_err = _hl_cl_enq2d(devid, kernel, work_sizes);
  dt_opencl_release_mem_object(dev_lift);
  return cl_err;
}

#endif // HAVE_OPENCL
#include "iop/highlights_selftests.h"

#ifdef HAVE_OPENCL

// Shared host middle of the harmonic reconstruction: knee estimation/correction, distance
// transform, segmentation and the per-region rebuild -- everything between the gather and the
// remosaic, CFA-agnostic. Used by the OpenCL hybrid driver after its GPU gather; the CPU
// drivers keep their historical inline copies (same code, kept verbatim to avoid touching the
// validated path -- unify when the CPU drivers next change).
// On success *remosaic_input_out points to `input` or to a knee-corrected CFA copy
// (*input_corr_out, caller frees with dt_pixelpipe_cache_free_align).
static int _harmonic_reconstruct_host(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                                      const dt_dev_pixelpipe_iop_t *piece, const float *const restrict input,
                                      float *const restrict interpolated, float *const restrict clipping_mask,
                                      const dt_iop_roi_t *const roi_in, const dt_aligned_pixel_t clips,
                                      const dt_aligned_pixel_t normalization, const float **remosaic_input_out,
                                      float **input_corr_out, const _hl_knee_curve_t knee_pre[3])
{
  // _hl_knee_apply_cfa below reads FC(row, col, filters) with tile-local row/col, so filters
  // must be pre-shifted for roi_in's crop position (mirrors process_harmonic_bayer).
  const uint32_t filters = dt_dev_get_roi_filters(piece, roi_in);
  const uint8_t(*const xtrans)[6] = (filters == 9u) ? (const uint8_t(*const)[6])piece->dsc_in.xtrans : NULL;
  const size_t width = roi_in->width;
  const size_t height = roi_in->height;
  const size_t size = width * height;

  *remosaic_input_out = input;
  *input_corr_out = NULL;

  // the knee was estimated by the caller BEFORE the gather (its engagement drives the band
  // override of the detection); reuse the curves here
  _hl_knee_curve_t knee[3];
  memcpy(knee, knee_pre, sizeof(knee));
  dt_aligned_pixel_t clipvaln = { 1.f, 1.f, 1.f, 1.f };
  dt_aligned_pixel_t knee_clipraw = { 1.f, 1.f, 1.f, 1.f };
  for(int c = 0; c < 3; c++)
  {
    clipvaln[c] = clips[c] / (DT_HL_KNEE_DET * fmaxf(normalization[c], 1e-9f));
    knee_clipraw[c] = clips[c] / DT_HL_KNEE_DET;
  }
  const int knee_on = knee[0].engaged || knee[1].engaged || knee[2].engaged;

  if(knee_on) _hl_knee_apply_interpolated(interpolated, size, clipvaln, normalization, knee);

  const size_t npix = size;
  float *const restrict depth = dt_pixelpipe_cache_alloc_align_float(npix, pipe);
  if(!depth) return 1;
  uint8_t *const restrict maskb = (uint8_t *)dt_alloc_align(npix);
  if(!maskb)
  {
    dt_pixelpipe_cache_free_align(depth);
    return 1;
  }
  __OMP_PARALLEL_FOR__()
  for(size_t i = 0; i < npix; i++)
  {
    depth[i] = (clipping_mask[i * 4 + 3] > 0.5f) ? (float)DT_DISTANCE_TRANSFORM_MAX : 0.f;
    maskb[i] = (clipping_mask[i * 4 + 3] >= 1e-3f);
  }
  dt_image_distance_transform(NULL, depth, width, height, 0.f, DT_DISTANCE_TRANSFORM_NONE);


  const dt_iop_highlights_data_t *const data = (const dt_iop_highlights_data_t *)piece->data;
  _hl_region_t *regions = NULL;
  const int nreg = _segment_clipped_regions(maskb, depth, width, height, 1.25f, 8, 256, &regions);

  size_t nclipped = 0;
  for(size_t i = 0; i < npix; i++)
    if(clipping_mask[i * 4 + 3] > 0.5f) nclipped++;

  dt_print(
      DT_DEBUG_PERF,
      "[highlights] %s %dx%d: procmax=[%.4f %.4f %.4f] clips=[%.4f %.4f %.4f] clipped=%llu (%.2f%%) regions=%d\n",
      (filters == 9u) ? "xtrans (cl gather)" : "bayer (cl gather)", (int)width, (int)height,
      piece->dsc_in.processed_maximum[0], piece->dsc_in.processed_maximum[1], piece->dsc_in.processed_maximum[2],
      clips[0], clips[1], clips[2], (unsigned long long)nclipped, 100.0 * (double)nclipped / (double)npix, nreg);

  for(int region_index = 0; region_index < nreg; region_index++)
    _region_guided_filter(interpolated, clipping_mask, depth, width, &regions[region_index], pipe,
                          data->solid_color, data->iterations, data->noise_level);

  free(regions);
  dt_free_align(maskb);
  dt_pixelpipe_cache_free_align(depth);

  if(knee_on)
  {
    float *input_corr = dt_pixelpipe_cache_alloc_align_float(size, pipe);
    if(!IS_NULL_PTR(input_corr))
    {
      _hl_knee_apply_cfa(input, input_corr, width, height, filters, roi_in, xtrans, knee_clipraw, knee);
      *remosaic_input_out = input_corr;
      *input_corr_out = input_corr;
    }
  }

  _hl_gauss_cache_flush();
  return 0;
}

#define HL_CL_RELEASE(mem_obj)                                                                                    \
  do                                                                                                              \
  {                                                                                                               \
    dt_opencl_release_mem_object(mem_obj);                                                                        \
    (mem_obj) = NULL;                                                                                             \
  } while(0)

// Harmonic transposition on an OpenCL pipe: hybrid CPU-orchestrated, stage 1.
// The reconstruction's heart is CPU by design (sparse Cholesky factorizations, per-region
// segmentation and orchestration), so the module roundtrips the single-channel raw through
// the host and runs the exact CPU pipeline -- the output is BIT-IDENTICAL to the CPU path
// by construction, and the pipe keeps its CL chain (up/downstream modules stay on the GPU,
// no scheduler-level fallback). Stage 2 (planned) slots GPU kernels into this driver where
// they pay: the gather/remosaic kernels already exist from the a-trous path, and the
// region moment blurs + harmonic fills are the dominant remaining cost -- at the price of
// bit-identity with the CPU, so it must go through the full validation protocol.
// Stage-1 fallback: full host roundtrip running the exact CPU driver (bit-identical to the
// CPU pipe by construction). Used when any GPU gather/remosaic step fails.
static cl_int _harmonic_cl_roundtrip(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                                     const dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
                                     const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                                     const dt_aligned_pixel_t clips)
{
  const int devid = pipe->devid;
  const uint32_t filters = piece->dsc_in.filters;
  const size_t n_in = (size_t)roi_in->width * roi_in->height;
  const size_t n_out = (size_t)roi_out->width * roi_out->height;

  float *host_in = dt_pixelpipe_cache_alloc_align_float(n_in, pipe);
  float *host_out = dt_pixelpipe_cache_alloc_align_float(n_out, pipe);
  cl_int cl_err = DT_OPENCL_DEFAULT_ERROR;

  if(IS_NULL_PTR(host_in) || IS_NULL_PTR(host_out)) goto error;

  cl_err = dt_opencl_copy_device_to_host(devid, host_in, dev_in, roi_in->width, roi_in->height, sizeof(float));
  if(cl_err != CL_SUCCESS) goto error;

  if((filters == 9u && process_harmonic_xtrans(self, pipe, piece, host_in, host_out, roi_in, roi_out, clips))
     || (filters != 9u && process_harmonic_bayer(self, pipe, piece, host_in, host_out, roi_in, roi_out, clips)))
  {
    cl_err = DT_OPENCL_DEFAULT_ERROR;
    goto error;
  }

  cl_err
      = dt_opencl_write_host_to_device(devid, host_out, dev_out, roi_out->width, roi_out->height, sizeof(float));

error:
  dt_pixelpipe_cache_free_align(host_in);
  dt_pixelpipe_cache_free_align(host_out);
  return cl_err;
}

// Stage 2: the gather (normalization reduce, bilinear interpolation + clip mask, mask
// feathering) and the scatter (remosaic) run on the GPU with the kernels shared with the
// a-trous path; the reconstruction middle (knee, segmentation, regions -- the solvers are CPU
// by design) runs on downloaded host planes. Any GPU failure falls back to the stage-1
// roundtrip above.

// GPU middle of the harmonic pipeline: knee estimation + application, segmentation support
// (byte masks down, depth up -- the EDT and flood fill stay on the host, exact), and the
// per-region reconstruction, all on device buffers. Returns CL_SUCCESS when the whole middle
// ran on the GPU; any failure leaves the caller to run the host middle instead. corr_out
// receives the knee-corrected 1-channel CFA buffer when the knee engages (caller releases).
static cl_int _harmonic_reconstruct_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                                       const dt_dev_pixelpipe_iop_t *piece, cl_mem raw_buf, cl_mem interp_buf,
                                       cl_mem mask_buf, cl_mem *corr_out, const dt_iop_roi_t *const roi_in,
                                       const dt_aligned_pixel_t clips, const dt_aligned_pixel_t norm,
                                       cl_mem dev_xtrans, const _hl_knee_curve_t knee_pre[3])
{
  dt_iop_highlights_global_data_t *global_data = (dt_iop_highlights_global_data_t *)self->global_data;
  const dt_iop_highlights_data_t *const data = (const dt_iop_highlights_data_t *)piece->data;
  const int devid = pipe->devid;
  const uint32_t filters = piece->dsc_in.filters;
  const int width = roi_in->width;
  const int height = roi_in->height;
  const double _tgpu0 = dt_get_wtime();
  _hl_cl_stats_reset();
  const size_t npix = (size_t)width * height;
  const int is_xtrans = (filters == 9u);
  cl_int cl_err = DT_OPENCL_DEFAULT_ERROR;

  if(data->noise_level > 0.f) return cl_err; // grain epilogue is not ported

  size_t sizes[3] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };

  cl_mem seed = NULL;
  cl_mem member = NULL;
  cl_mem depth_dev = NULL;
  cl_mem corr = NULL;
  uint8_t *h_seed = NULL;
  uint8_t *h_member = NULL;
  float *depth = NULL;
  _hl_region_t *regions = NULL;
  *corr_out = NULL;

  {
    // the knee was estimated by the caller BEFORE the gather (its engagement drives the band
    // override of the detection); reuse the curves here
    _hl_knee_curve_t knee[3];
    memcpy(knee, knee_pre, sizeof(knee));
    dt_aligned_pixel_t clipvaln = { 1.f, 1.f, 1.f, 1.f };
    dt_aligned_pixel_t knee_clipraw = { 1.f, 1.f, 1.f, 1.f };
    for(int c = 0; c < 3; c++)
    {
      clipvaln[c] = clips[c] / (DT_HL_KNEE_DET * fmaxf(norm[c], 1e-9f));
      knee_clipraw[c] = clips[c] / DT_HL_KNEE_DET;
    }
    const int knee_on = knee[0].engaged || knee[1].engaged || knee[2].engaged;

    if(knee_on)
    {
      // band correction on the interpolated RGBN planes (reconstruction fits unbiased data)
      float lift[3 * DT_HL_KNEE_BINS];
      for(int c = 0; c < 3; c++) memcpy(lift + c * DT_HL_KNEE_BINS, knee[c].lift, sizeof(knee[c].lift));
      cl_mem dev_lift = _sp_cl_upload(devid, lift, sizeof(lift));
      if(!dev_lift)
      {
        cl_err = DT_OPENCL_DEFAULT_ERROR;
        goto out;
      }
      const int kernel = global_data->kernel_hl_knee_apply_interp;
      const cl_float4 clip4 = { { clipvaln[0], clipvaln[1], clipvaln[2], 1.f } };
      const cl_float4 wb4 = { { norm[0], norm[1], norm[2], 1.f } };
      const cl_int4 engaged_flags = { { knee[0].engaged, knee[1].engaged, knee[2].engaged, 0 } };
      const float knee_lo = DT_HL_KNEE_LO;
      const float knee_det = DT_HL_KNEE_DET;
      const int bins = DT_HL_KNEE_BINS;
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &interp_buf);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(int), &width);
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &height);
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_float4), &clip4);
      dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_float4), &wb4);
      dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(cl_mem), &dev_lift);
      dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(cl_int4), &engaged_flags);
      dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(float), &knee_lo);
      dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(float), &knee_det);
      dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(int), &bins);
      cl_err = _hl_cl_enq2d(devid, kernel, sizes);
      dt_opencl_release_mem_object(dev_lift);
      if(cl_err != CL_SUCCESS) goto out;

      // corrected CFA copy for the remosaic composition
      corr = dt_opencl_alloc_device_buffer(devid, sizeof(float) * npix);
      if(!corr)
      {
        cl_err = DT_OPENCL_DEFAULT_ERROR;
        goto out;
      }
      cl_err = _hl_knee_apply_cfa_cl(devid, global_data, raw_buf, corr, width, height, filters, roi_in, dev_xtrans,
                                     is_xtrans, knee_clipraw, knee);
      if(cl_err != CL_SUCCESS) goto out;
    }
  }

  // ---- segmentation support: byte masks down, exact host EDT + flood fill, depth up ----
  seed = dt_opencl_alloc_device_buffer(devid, npix);
  member = dt_opencl_alloc_device_buffer(devid, npix);
  h_seed = (uint8_t *)dt_alloc_align(npix);
  h_member = (uint8_t *)dt_alloc_align(npix);
  depth = dt_pixelpipe_cache_alloc_align_float(npix, pipe);
  if(!seed || !member || !h_seed || !h_member || !depth)
  {
    cl_err = DT_OPENCL_DEFAULT_ERROR;
    goto out;
  }
  {
    const int kernel = global_data->kernel_hl_mask_pack;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &mask_buf);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &seed);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &member);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &height);
    cl_err = _hl_cl_enq2d(devid, kernel, sizes);
    if(cl_err != CL_SUCCESS) goto out;
  }
  cl_err = _hl_cl_read_timed(devid, h_seed, seed, 0, npix, CL_TRUE);
  if(cl_err == CL_SUCCESS) cl_err = _hl_cl_read_timed(devid, h_member, member, 0, npix, CL_TRUE);
  if(cl_err != CL_SUCCESS) goto out;

  __OMP_PARALLEL_FOR__()
  for(size_t i = 0; i < npix; i++) depth[i] = h_seed[i] ? (float)DT_DISTANCE_TRANSFORM_MAX : 0.f;
  dt_image_distance_transform(NULL, depth, width, height, 0.f, DT_DISTANCE_TRANSFORM_NONE);

  const int nreg = _segment_clipped_regions(h_member, depth, width, height, 1.25f, 8, 256, &regions);

  depth_dev = dt_opencl_alloc_device_buffer(devid, sizeof(float) * npix);
  if(!depth_dev)
  {
    cl_err = DT_OPENCL_DEFAULT_ERROR;
    goto out;
  }
  cl_err = dt_opencl_write_buffer_to_device(devid, depth, depth_dev, 0, sizeof(float) * npix, CL_TRUE);
  if(cl_err != CL_SUCCESS) goto out;

  {
    size_t cpu_px = DT_HL_CL_CPU_REGION_PX;
    const char *override_env = getenv("HL_CL_CPU_PX");
    if(override_env) cpu_px = (size_t)strtoull(override_env, NULL, 10);
    int n_cpu = 0;
    for(int region_index = 0; region_index < nreg && cl_err == CL_SUCCESS; region_index++)
    {
      const size_t region_px = (size_t)(regions[region_index].rx1 - regions[region_index].rx0 + 1)
                               * (size_t)(regions[region_index].ry1 - regions[region_index].ry0 + 1);
      if(region_px <= cpu_px)
      {
        cl_err = _region_cpu_offload_cl(devid, global_data, interp_buf, mask_buf, depth_dev, width,
                                        &regions[region_index], pipe, data->solid_color, data->iterations,
                                        data->noise_level);
        n_cpu++;
      }
      else
        cl_err = _region_guided_filter_cl(devid, global_data, interp_buf, mask_buf, depth_dev, width,
                                          &regions[region_index], pipe, data->solid_color);
    }
    if(n_cpu) dt_print(DT_DEBUG_PERF, "[highlights] gpu middle: %d/%d regions offloaded to CPU\n", n_cpu, nreg);
  }

  if(cl_err == CL_SUCCESS)
  {
    _hl_cl_finish_timed(devid);
    dt_print(DT_DEBUG_PERF, "[highlights] gpu middle: %.0f ms (%d regions)\n", (dt_get_wtime() - _tgpu0) * 1e3,
             nreg);
    dt_print(DT_DEBUG_PERF,
             "[highlights] gpu middle sync: reads=%d (%.0f ms) finish=%d (%.0f ms) enq=%d"
             " chol=%d (%.0f ms) cg_iters=%d\n",
             _hl_cl_wait_n, _hl_cl_wait_s * 1e3, _hl_cl_finish_n, _hl_cl_finish_s * 1e3, _hl_cl_enq_n,
             _hl_cl_chol_n, _hl_cl_chol_s * 1e3, _hl_cl_cg_n);
  }

out:
  _hl_gauss_cache_flush(); // the CPU-offloaded regions run _region_blur on this thread
  dt_opencl_release_mem_object(seed);
  dt_opencl_release_mem_object(member);
  dt_opencl_release_mem_object(depth_dev);
  dt_free_align(h_seed);
  dt_free_align(h_member);
  dt_pixelpipe_cache_free_align(depth);
  free(regions);
  if(cl_err == CL_SUCCESS)
    *corr_out = corr;
  else
    dt_opencl_release_mem_object(corr);
  return cl_err;
}

// Top-level OpenCL entry point for the harmonic-transposition mode (called from highlights.c
// when the pipe runs on a GPU). First fires the env-gated CPU-vs-GPU self-tests (each a
// no-op unless its HL_*_TEST variable is set), then runs the hybrid driver: GPU gather
// (bilinear interpolation + clip mask + feathering), the reconstruction middle (fully on GPU
// when possible, otherwise on downloaded host planes), and the GPU remosaic. Any GPU failure
// falls back to _harmonic_cl_roundtrip (bit-identical CPU path through a host roundtrip).
static cl_int process_harmonic_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                                  const dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
                                  const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                                  const dt_aligned_pixel_t clips)
{
  _sp_chol_cl_selftest(pipe->devid, self->global_data, pipe);
  _region_blur_cl_selftest(pipe->devid, pipe);
  _cf_harmonic_fill_cl_selftest(pipe->devid, self->global_data, pipe);
  _cf_joint_stage_cl_selftest(pipe->devid, self->global_data, pipe);
  _cf_stage_cl_selftest(pipe->devid, self->global_data, pipe);
  _hf_stage_cl_selftest(pipe->devid, self->global_data, pipe);
  _selfdome_stage_cl_selftest(pipe->devid, self->global_data, pipe);
  _joint_core_stage_cl_selftest(pipe->devid, self->global_data, pipe);
  _knee_cl_selftest(pipe->devid, self->global_data, pipe);
  _aniso_stage_cl_selftest(pipe->devid, self->global_data, pipe);
  _region_guided_filter_cl_selftest(pipe->devid, self->global_data, pipe);

  dt_iop_highlights_global_data_t *global_data = (dt_iop_highlights_global_data_t *)self->global_data;
  const int devid = pipe->devid;
  // _hl_knee_estimate_cl and the hl_knee_* kernels are self-correcting: they take this raw
  // filters value PLUS roi_in->x/y as separate kernel args and add them themselves. The shared
  // interpolate_and_mask/remosaic_and_replace Bayer kernels (and the host-side
  // _compute_laplacian_normalization call below) have no roi offset arg at all -- they need
  // filters pre-shifted for roi_in's crop position instead (mirrors the CPU driver's fix).
  const uint32_t filters = piece->dsc_in.filters;
  const uint32_t filters_shifted = dt_dev_get_roi_filters(piece, roi_in);
  const int width = roi_in->width;
  const int height = roi_in->height;
  const size_t npix = (size_t)width * height;
  const int is_xtrans = (filters == 9u);

  size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };

  cl_mem interpolated = NULL;
  cl_mem clipping_mask = NULL;
  cl_mem temp = NULL;
  cl_mem clips_cl = NULL;
  cl_mem normalization_final = NULL;
  cl_mem dev_xtrans = NULL;
  cl_mem lookup_cl = NULL;
  cl_mem corr_cl = NULL;
  cl_mem det_clips_cl = NULL;
  float *h_interp = NULL;
  float *h_mask = NULL;
  float *h_raw = NULL;
  float *input_corr = NULL;
  const float *remosaic_input = NULL;
  cl_int cl_err = DT_OPENCL_DEFAULT_ERROR;

  interpolated = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);
  clipping_mask = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);
  temp = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);
  clips_cl = dt_opencl_copy_host_to_device_constant(devid, 4 * sizeof(float), (float *)clips);
  if(IS_NULL_PTR(interpolated) || IS_NULL_PTR(clipping_mask) || IS_NULL_PTR(temp) || IS_NULL_PTR(clips_cl))
    goto fallback;

  if(is_xtrans)
  {
    dev_xtrans = dt_opencl_copy_host_to_device_constant(devid, sizeof(piece->dsc_in.xtrans),
                                                        (void *)piece->dsc_in.xtrans);
    int32_t lookup[6][6][32] = { { { 0 } } };
    _build_xtrans_bilinear_lookup(lookup, roi_in, (const uint8_t(*const)[6])piece->dsc_in.xtrans);
    lookup_cl = dt_opencl_copy_host_to_device_constant(devid, sizeof(lookup), lookup);
    if(IS_NULL_PTR(dev_xtrans) || IS_NULL_PTR(lookup_cl)) goto fallback;
  }

  // ---- per-channel normalization: computed on the HOST with the exact CPU function ----
  // The raw is needed on the host anyway (knee estimation reads the mosaic), and the GPU
  // max-reduce kernels are not bit-faithful to _compute_laplacian_normalization: the tiny
  // normalization difference shifted the clip mask by a few hundred pixels and the whole
  // reconstruction with it. Downloading first keeps the mask identical to the CPU path.
  h_raw = dt_pixelpipe_cache_alloc_align_float(npix, pipe);
  if(IS_NULL_PTR(h_raw)) goto fallback;
  cl_err = dt_opencl_copy_device_to_host(devid, h_raw, dev_in, width, height, sizeof(float));
  if(cl_err != CL_SUCCESS) goto fallback;

  dt_aligned_pixel_t norm_host = { 1.f, 1.f, 1.f, 1.f };
  _compute_laplacian_normalization(h_raw, roi_in, filters_shifted,
                                   is_xtrans ? (const uint8_t(*const)[6])piece->dsc_in.xtrans : NULL, norm_host);
  normalization_final = dt_opencl_copy_host_to_device_constant(devid, 4 * sizeof(float), norm_host);
  if(IS_NULL_PTR(normalization_final)) goto fallback;

  // ---- rolloff estimation FIRST (raw-based): its per-channel engagement drives the band
  //      override of the detection thresholds, exactly like the CPU drivers ----
  _hl_knee_curve_t knee[3];
  {
    dt_aligned_pixel_t knee_clipraw = { 1.f, 1.f, 1.f, 1.f };
    for(int c = 0; c < 3; c++) knee_clipraw[c] = clips[c] / DT_HL_KNEE_DET;

    // the knee kernels read the raw as a BUFFER; dev_in is an image2d -> copy first
    cl_mem knee_raw = dt_opencl_alloc_device_buffer(devid, sizeof(float) * npix);
    if(IS_NULL_PTR(knee_raw)) goto fallback;
    size_t korigin[3] = { 0, 0, 0 };
    size_t kregion[3] = { (size_t)width, (size_t)height, 1 };
    cl_err = dt_opencl_enqueue_copy_image_to_buffer(devid, dev_in, knee_raw, korigin, kregion, 0);
    if(cl_err != CL_SUCCESS)
    {
      dt_opencl_release_mem_object(knee_raw);
      goto fallback;
    }

    const double _tknee = dt_get_wtime();
    cl_err = _hl_knee_estimate_cl(devid, global_data, knee_raw, width, height, filters, roi_in, dev_xtrans,
                                  is_xtrans, knee_clipraw, knee, pipe);
    dt_opencl_release_mem_object(knee_raw);
    if(cl_err != CL_SUCCESS) goto fallback;
    dt_print(DT_DEBUG_PERF, "[highlights] knee: %.1f ms engaged=[%d %d %d] max lift=[%.4f %.4f %.4f] (GPU)\n",
             (dt_get_wtime() - _tknee) * 1e3, knee[0].engaged, knee[1].engaged, knee[2].engaged,
             knee[0].lift[DT_HL_KNEE_BINS - 1], knee[1].lift[DT_HL_KNEE_BINS - 1],
             knee[2].lift[DT_HL_KNEE_BINS - 1]);
  }

  dt_aligned_pixel_t eff_clips;
  for_four_channels(c) eff_clips[c] = clips[c];
  for(int c = 0; c < 3; c++)
    if(knee[c].engaged) eff_clips[c] = clips[c] * DT_HL_BAND_OVR;
  det_clips_cl = dt_opencl_copy_host_to_device_constant(devid, 4 * sizeof(float), eff_clips);
  if(IS_NULL_PTR(det_clips_cl)) goto fallback;

  // ---- gather: bilinear interpolation + clip mask, then 5x5 feathering ----
  if(is_xtrans)
  {
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask_xtrans, 0, sizeof(cl_mem),
                             &dev_in);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask_xtrans, 1, sizeof(cl_mem),
                             &interpolated);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask_xtrans, 2, sizeof(cl_mem),
                             &clipping_mask);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask_xtrans, 3, sizeof(cl_mem),
                             &det_clips_cl);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask_xtrans, 4, sizeof(cl_mem),
                             &normalization_final);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask_xtrans, 5, sizeof(int),
                             &width);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask_xtrans, 6, sizeof(int),
                             &height);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask_xtrans, 7, sizeof(int),
                             &roi_in->x);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask_xtrans, 8, sizeof(int),
                             &roi_in->y);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask_xtrans, 9, sizeof(cl_mem),
                             &dev_xtrans);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask_xtrans, 10, sizeof(cl_mem),
                             &lookup_cl);
    cl_err = _hl_cl_enq2d(devid, global_data->kernel_highlights_bilinear_and_mask_xtrans, sizes);
  }
  else
  {
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask, 0, sizeof(cl_mem), &dev_in);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask, 1, sizeof(cl_mem),
                             &interpolated);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask, 2, sizeof(cl_mem),
                             &clipping_mask);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask, 3, sizeof(cl_mem),
                             &det_clips_cl);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask, 4, sizeof(cl_mem),
                             &normalization_final);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask, 5, sizeof(int), &filters_shifted);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask, 6, sizeof(int),
                             &roi_out->width);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask, 7, sizeof(int),
                             &roi_out->height);
    cl_err = _hl_cl_enq2d(devid, global_data->kernel_highlights_bilinear_and_mask, sizes);
  }
  if(cl_err != CL_SUCCESS) goto fallback;


  // ---- GPU middle first: knee + segmentation support + per-region reconstruction on device
  //      buffers; only byte masks, the depth plane and reduction partials cross the bus ----
  {
    cl_mem raw_buf = dt_opencl_alloc_device_buffer(devid, sizeof(float) * npix);
    cl_mem interp_buf = dt_opencl_alloc_device_buffer(devid, sizeof(float) * npix * 4);
    cl_mem mask_buf = dt_opencl_alloc_device_buffer(devid, sizeof(float) * npix * 4);
    cl_mem corr_buf = NULL;
    size_t origin[3] = { 0, 0, 0 };
    size_t region1[3] = { (size_t)width, (size_t)height, 1 };
    cl_int gpu_err = (raw_buf && interp_buf && mask_buf) ? CL_SUCCESS : DT_OPENCL_DEFAULT_ERROR;
    if(gpu_err == CL_SUCCESS)
      gpu_err = dt_opencl_enqueue_copy_image_to_buffer(devid, dev_in, raw_buf, origin, region1, 0);
    if(gpu_err == CL_SUCCESS)
      gpu_err = dt_opencl_enqueue_copy_image_to_buffer(devid, interpolated, interp_buf, origin, region1, 0);
    if(gpu_err == CL_SUCCESS)
      gpu_err = dt_opencl_enqueue_copy_image_to_buffer(devid, clipping_mask, mask_buf, origin, region1, 0);
    int staged = 0; // 1 = the three images below were released and must be re-created
    if(gpu_err == CL_SUCCESS)
    {
      // the middle works on the buffers: release the three full-image images (~1.7 GB on a
      // 36 Mpx raw) so the region planes and stage temporaries fit in vRAM; the two that are
      // consumed downstream are re-created from the buffers right after. The HL_MIDDLE_AB
      // diagnostic keeps them alive instead: its reference run needs the PRISTINE planes.
      _hl_cl_finish_timed(devid); // the async image->buffer copies must land first
      if(!getenv("HL_MIDDLE_AB"))
      {
        HL_CL_RELEASE(temp);
        HL_CL_RELEASE(interpolated);
        HL_CL_RELEASE(clipping_mask);
        staged = 1;
      }
      gpu_err = _harmonic_reconstruct_cl(self, pipe, piece, raw_buf, interp_buf, mask_buf, &corr_buf, roi_in,
                                         clips, norm_host, dev_xtrans, knee);
    }
    // materialize the knee-corrected mosaic BEFORE restoring the working images: a failure
    // here must take the same pristine re-gather road as a mid-middle failure. Falling
    // through with the reconstruction already copied back would hand the host middle
    // knee-lifted, partially reconstructed planes -- the knee would be applied twice and
    // the regions re-solved on reconstructed anchors, silently.
    if(gpu_err == CL_SUCCESS && corr_buf)
    {
      corr_cl = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float));
      if(corr_cl)
        gpu_err = dt_opencl_enqueue_copy_buffer_to_image(devid, corr_buf, corr_cl, 0, origin, region1);
      else
        gpu_err = DT_OPENCL_DEFAULT_ERROR;
      if(gpu_err != CL_SUCCESS) HL_CL_RELEASE(corr_cl);
    }
    // HL_MIDDLE_AB=1 (diagnostic): run the host middle on the pristine planes (kept alive
    // above) and print its divergence from the device middle; the device result still ships
    if(gpu_err == CL_SUCCESS && getenv("HL_MIDDLE_AB"))
    {
      float *gpu_interp = dt_pixelpipe_cache_alloc_align_float(npix * 4, pipe);
      float *host_interp = dt_pixelpipe_cache_alloc_align_float(npix * 4, pipe);
      float *host_mask = dt_pixelpipe_cache_alloc_align_float(npix * 4, pipe);
      float *host_raw = dt_pixelpipe_cache_alloc_align_float(npix, pipe);
      if(gpu_interp && host_interp && host_mask && host_raw
         && _hl_cl_read_timed(devid, gpu_interp, interp_buf, 0, sizeof(float) * npix * 4, CL_TRUE) == CL_SUCCESS
         && dt_opencl_copy_device_to_host(devid, host_interp, interpolated, width, height, sizeof(float) * 4)
                == CL_SUCCESS
         && dt_opencl_copy_device_to_host(devid, host_mask, clipping_mask, width, height, sizeof(float) * 4)
                == CL_SUCCESS
         && dt_opencl_copy_device_to_host(devid, host_raw, dev_in, width, height, sizeof(float)) == CL_SUCCESS)
      {
        const float *remosaic_ptr = NULL;
        float *input_corr_ab = NULL;
        if(!_harmonic_reconstruct_host(self, pipe, piece, host_raw, host_interp, host_mask, roi_in, clips,
                                       norm_host, &remosaic_ptr, &input_corr_ab, knee))
        {
          float max_diff = 0.f;
          double sum_diff = 0.0;
          size_t arg_index = 0;
          for(size_t i = 0; i < npix * 4; i++)
          {
            const float diff = fabsf(gpu_interp[i] - host_interp[i]);
            if(diff > max_diff)
            {
              max_diff = diff;
              arg_index = i;
            }
            sum_diff += (double)diff;
          }
          fprintf(stderr, "[hl middle AB] max=%.3e mean=%.3e at px=(%llu,%llu) c=%llu gpu=%f cpu=%f\n", max_diff,
                  sum_diff / (double)(npix * 4), (unsigned long long)((arg_index / 4) % width),
                  (unsigned long long)((arg_index / 4) / width), (unsigned long long)(arg_index % 4),
                  gpu_interp[arg_index], host_interp[arg_index]);
        }
        dt_pixelpipe_cache_free_align(input_corr_ab);
      }
      dt_pixelpipe_cache_free_align(gpu_interp);
      dt_pixelpipe_cache_free_align(host_interp);
      dt_pixelpipe_cache_free_align(host_mask);
      dt_pixelpipe_cache_free_align(host_raw);
    }
    // restore the images for the downstream blend/remosaic -- ONLY when they were released:
    // an early staging failure leaves them alive and pristine, and reallocating over the
    // live handles would leak them (~1.7 GB) while doubling vRAM demand under the very
    // pressure that made staging fail. On success, interpolated carries
    // the reconstruction and the mask is untouched (buffer copy-back). On FAILURE the middle
    // may have partially scattered and knee-lifted interp_buf, so the host fallback must NOT
    // reuse it: re-run the interpolation + mask blur from the still-alive inputs instead.
    if(staged)
    {
      interpolated = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);
      clipping_mask = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);
      cl_int restore_err = (interpolated && clipping_mask) ? CL_SUCCESS : DT_OPENCL_DEFAULT_ERROR;
      if(restore_err == CL_SUCCESS && gpu_err == CL_SUCCESS)
      {
        restore_err = dt_opencl_enqueue_copy_buffer_to_image(devid, interp_buf, interpolated, 0, origin, region1);
        if(restore_err == CL_SUCCESS)
          restore_err = dt_opencl_enqueue_copy_buffer_to_image(devid, mask_buf, clipping_mask, 0, origin, region1);
      }
      else if(restore_err == CL_SUCCESS)
      {
        temp = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);
        if(!temp) restore_err = DT_OPENCL_DEFAULT_ERROR;
        if(restore_err == CL_SUCCESS)
        {
          if(is_xtrans)
          {
            const int kernel = global_data->kernel_highlights_bilinear_and_mask_xtrans;
            dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &dev_in);
            dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &interpolated);
            dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &temp);
            dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &clips_cl);
            dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &normalization_final);
            dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &roi_out->width);
            dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &roi_out->height);
            dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &roi_in->x);
            dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &roi_in->y);
            dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(cl_mem), &dev_xtrans);
            dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(cl_mem), &lookup_cl);
            restore_err = _hl_cl_enq2d(devid, kernel, sizes);
          }
          else
          {
            const int kernel = global_data->kernel_highlights_bilinear_and_mask;
            dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &dev_in);
            dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &interpolated);
            dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &temp);
            dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &clips_cl);
            dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &normalization_final);
            dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &filters_shifted);
            dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &roi_out->width);
            dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &roi_out->height);
            restore_err = _hl_cl_enq2d(devid, kernel, sizes);
          }
        }
        if(restore_err == CL_SUCCESS)
        {
          const int kernel = global_data->kernel_highlights_box_blur;
          dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &temp);
          dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &clipping_mask);
          dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &roi_out->width);
          dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &roi_out->height);
          restore_err = _hl_cl_enq2d(devid, kernel, sizes);
        }
      }
      if(restore_err != CL_SUCCESS)
      {
        dt_opencl_release_mem_object(raw_buf);
        dt_opencl_release_mem_object(interp_buf);
        dt_opencl_release_mem_object(mask_buf);
        dt_opencl_release_mem_object(corr_buf);
        cl_err = restore_err;
        goto fallback;
      }
    }
    else if(gpu_err == CL_SUCCESS)
    {
      // diagnostic mode kept the images alive: publish the device result into them now
      cl_int restore_err
          = dt_opencl_enqueue_copy_buffer_to_image(devid, interp_buf, interpolated, 0, origin, region1);
      if(restore_err == CL_SUCCESS)
        restore_err = dt_opencl_enqueue_copy_buffer_to_image(devid, mask_buf, clipping_mask, 0, origin, region1);
      if(restore_err != CL_SUCCESS)
      {
        dt_opencl_release_mem_object(raw_buf);
        dt_opencl_release_mem_object(interp_buf);
        dt_opencl_release_mem_object(mask_buf);
        dt_opencl_release_mem_object(corr_buf);
        cl_err = restore_err;
        goto fallback;
      }
    }
    dt_opencl_release_mem_object(raw_buf);
    dt_opencl_release_mem_object(interp_buf);
    dt_opencl_release_mem_object(mask_buf);
    dt_opencl_release_mem_object(corr_buf);
    if(gpu_err == CL_SUCCESS) goto remosaic;
    // GPU middle unavailable (fp64 device, grain requested, oversized hole...): host middle below
    HL_CL_RELEASE(corr_cl);
    dt_print(DT_DEBUG_OPENCL, "[opencl_highlights] harmonic GPU middle failed (%i), using the host middle\n",
             gpu_err);
  }

  // ---- download the working planes + raw (knee estimation reads the mosaic) ----
  h_interp = dt_pixelpipe_cache_alloc_align_float(npix * 4, pipe);
  h_mask = dt_pixelpipe_cache_alloc_align_float(npix * 4, pipe);
  if(IS_NULL_PTR(h_interp) || IS_NULL_PTR(h_mask)) goto fallback;

  cl_err = dt_opencl_copy_device_to_host(devid, h_interp, interpolated, width, height, sizeof(float) * 4);
  if(cl_err != CL_SUCCESS) goto fallback;
  cl_err = dt_opencl_copy_device_to_host(devid, h_mask, clipping_mask, width, height, sizeof(float) * 4);
  if(cl_err != CL_SUCCESS) goto fallback;

  // ---- CPU middle: knee, segmentation, per-region reconstruction ----
  if(_harmonic_reconstruct_host(self, pipe, piece, h_raw, h_interp, h_mask, roi_in, clips, norm_host,
                                &remosaic_input, &input_corr, knee))
    goto fallback;

  // ---- upload the reconstruction (+ the knee-corrected CFA when engaged) and remosaic ----
  cl_err = dt_opencl_write_host_to_device(devid, h_interp, interpolated, width, height, sizeof(float) * 4);
  if(cl_err != CL_SUCCESS) goto fallback;

remosaic:;
  cl_mem remosaic_in_cl = dev_in;
  if(corr_cl)
    remosaic_in_cl = corr_cl;
  else if(remosaic_input != h_raw && remosaic_input != NULL)
  {
    corr_cl = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float));
    if(IS_NULL_PTR(corr_cl)) goto fallback;
    cl_err = dt_opencl_write_host_to_device(devid, input_corr, corr_cl, width, height, sizeof(float));
    if(cl_err != CL_SUCCESS) goto fallback;
    remosaic_in_cl = corr_cl;
  }

  if(is_xtrans)
  {
    const int clip_floor_on = TRUE; // clipped raw values are floors, never blend targets
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace_xtrans, 0, sizeof(cl_mem),
                             &remosaic_in_cl);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace_xtrans, 1, sizeof(cl_mem),
                             &dev_in);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace_xtrans, 2, sizeof(cl_mem),
                             &interpolated);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace_xtrans, 3, sizeof(cl_mem),
                             &clipping_mask);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace_xtrans, 4, sizeof(cl_mem),
                             &dev_out);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace_xtrans, 5, sizeof(cl_mem),
                             &normalization_final);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace_xtrans, 6, sizeof(cl_mem),
                             &clips_cl);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace_xtrans, 7, sizeof(int),
                             &clip_floor_on);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace_xtrans, 8, sizeof(int),
                             &width);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace_xtrans, 9, sizeof(int),
                             &height);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace_xtrans, 10, sizeof(int),
                             &roi_in->x);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace_xtrans, 11, sizeof(int),
                             &roi_in->y);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace_xtrans, 12, sizeof(cl_mem),
                             &dev_xtrans);
    cl_err = _hl_cl_enq2d(devid, global_data->kernel_highlights_remosaic_and_replace_xtrans, sizes);
  }
  else
  {
    const int clip_floor_on = TRUE; // clipped raw values are floors, never blend targets
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace, 0, sizeof(cl_mem),
                             &remosaic_in_cl);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace, 1, sizeof(cl_mem),
                             &dev_in);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace, 2, sizeof(cl_mem),
                             &interpolated);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace, 3, sizeof(cl_mem),
                             &clipping_mask);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace, 4, sizeof(cl_mem),
                             &dev_out);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace, 5, sizeof(cl_mem),
                             &normalization_final);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace, 6, sizeof(cl_mem),
                             &clips_cl);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace, 7, sizeof(int),
                             &clip_floor_on);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace, 8, sizeof(int), &filters_shifted);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace, 9, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace, 10, sizeof(int), &height);
    cl_err = _hl_cl_enq2d(devid, global_data->kernel_highlights_remosaic_and_replace, sizes);
  }
  if(cl_err != CL_SUCCESS) goto fallback;

  // success: release and return
  HL_CL_RELEASE(clips_cl);
  HL_CL_RELEASE(det_clips_cl);
  HL_CL_RELEASE(normalization_final);
  HL_CL_RELEASE(interpolated);
  HL_CL_RELEASE(clipping_mask);
  HL_CL_RELEASE(temp);
  HL_CL_RELEASE(dev_xtrans);
  HL_CL_RELEASE(lookup_cl);
  HL_CL_RELEASE(corr_cl);
  dt_pixelpipe_cache_free_align(h_interp);
  dt_pixelpipe_cache_free_align(h_mask);
  dt_pixelpipe_cache_free_align(h_raw);
  dt_pixelpipe_cache_free_align(input_corr);
  return CL_SUCCESS;

fallback:
  dt_print(DT_DEBUG_OPENCL,
           "[opencl_highlights] harmonic GPU gather failed (%i), falling back to the host roundtrip\n", cl_err);
  HL_CL_RELEASE(clips_cl);
  HL_CL_RELEASE(det_clips_cl);
  HL_CL_RELEASE(normalization_final);
  HL_CL_RELEASE(interpolated);
  HL_CL_RELEASE(clipping_mask);
  HL_CL_RELEASE(temp);
  HL_CL_RELEASE(dev_xtrans);
  HL_CL_RELEASE(lookup_cl);
  HL_CL_RELEASE(corr_cl);
  dt_pixelpipe_cache_free_align(h_interp);
  dt_pixelpipe_cache_free_align(h_mask);
  dt_pixelpipe_cache_free_align(h_raw);
  dt_pixelpipe_cache_free_align(input_corr);
  return _harmonic_cl_roundtrip(self, pipe, piece, dev_in, dev_out, roi_in, roi_out, clips);
}
#endif // HAVE_OPENCL