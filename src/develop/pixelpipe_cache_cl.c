/*
    This file is part of the Ansel project.
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

/**
 * @file pixelpipe_cache_cl.c
 *
 * @brief Pixelpipe cache ↔ OpenCL buffer lifecycle helpers.
 *
 * @details
 * This file contains the "plumbing" between:
 *
 * - the **pixelpipe cache** (host/RAM buffers, lifetime + locks handled by `pixelpipe_cache.[ch]`), and
 * - **OpenCL images** (device-side objects, optionally backed by host memory via `CL_MEM_USE_HOST_PTR`).
 *
 * Why this exists
 * ---------------
 *
 * The pixelpipe is mostly written as a classic CPU pipeline: each module consumes a packed host buffer and
 * produces a packed host buffer. When OpenCL is enabled, some modules can run GPU kernels using `process_cl()`.
 *
 * The performance goal is to avoid unnecessary copies between RAM and vRAM:
 *
 * - If we can keep buffers on the device between GPU modules, we do so.
 * - If a CPU module needs the buffer, we synchronize device → host.
 *
 * However, correctness has strict requirements:
 *
 * - The pixelpipe cache can **evict** entries (LRU + fragmentation mitigation).
 * - The pixelpipe cache can **auto-destroy** entries when their refcount drops.
 * - Host buffers can be **reused** for other images/ROIs once unlocked.
 *
 * When using `CL_MEM_USE_HOST_PTR`, OpenCL may:
 *
 * - truly run **zero-copy**, reading host memory directly (best case), or
 * - allocate a **device-side staging copy** (still legal), requiring explicit transfers for correctness.
 *
 * Therefore we must:
 *
 * 1. Detect when a `CL_MEM_USE_HOST_PTR` image is *really* zero-copy for a given driver/device.
 * 2. Keep the cache entry appropriately locked while the GPU may still read from host memory.
 * 3. Provide robust sync primitives (map/unmap or explicit transfers) for device ↔ host transitions.
 * 4. Avoid leaving stale `cl_mem` pointers in our cache-side bookkeeping when we release a buffer.
 *
 * Where these helpers are used
 * ----------------------------
 *
 * The high-level OpenCL control-flow lives in `pixelpipe_hb.c` (whether to run on GPU, tiling decisions,
 * CPU fallbacks, etc.). This file focuses on the *mechanics* of OpenCL buffers:
 *
 * - allocating and reusing pinned buffers,
 * - caching `cl_mem` objects inside cache entries for later reuse,
 * - clearing/releasing/caching `cl_mem` objects in a safe way,
 * - synchronizing contents between device and host.
 *
 * Threading / locking model
 * -------------------------
 *
 * - Pixelpipe cache entries have their own locks (read/write) and reference counting.
 * - When OpenCL uses true zero-copy pinned buffers, the GPU may read host memory **asynchronously**.
 *   In that case we must keep a **read lock** on the cache entry until all queued GPU work is finished,
 *   otherwise another code path could overwrite the host buffer while the GPU is still reading it.
 *
 * Important warning
 * --------------------------------------
 *
 * You can't "just" call a CPU fallback when a GPU module fails: the CPU code expects a host buffer.
 * In OpenCL mode, the host buffer can legitimately be NULL (GPU-only intermediate), while the correct
 * data exists only in `cl_mem`. The fallback path must allocate the host buffer and synchronize it
 * before CPU code can proceed.
 */

#include "common/atomic.h"
#include "common/darktable.h"
#include "common/opencl.h"
#include "develop/pixelpipe.h"
#include "develop/pixelpipe_cache.h"

#include <stddef.h>

#ifdef HAVE_OPENCL

/**
 * @brief Determine whether a `CL_MEM_USE_HOST_PTR` OpenCL image is truly "zero-copy".
 *
 * @param devid OpenCL device index.
 * @param mem OpenCL image object (`cl_mem`).
 * @param host_ptr The CPU pointer we passed to `CL_MEM_USE_HOST_PTR`.
 * @param roi Image dimensions.
 * @param bpp Bytes per pixel.
 *
 * @return TRUE if the OpenCL driver mapped the image to the exact same `host_ptr` pointer, meaning
 *         the device is directly accessing the host memory (true zero-copy). FALSE otherwise.
 *
 * @details
 * Many drivers accept `CL_MEM_USE_HOST_PTR` but still create an internal device-side copy.
 * In that case, host memory is not automatically in sync with device memory, and explicit transfers
 * are required for correctness.
 *
 * We use a conservative runtime test:
 *
 * - map the image (blocking),
 * - compare the returned pointer with the original `host_ptr`,
 * - unmap and `clFinish` to ensure the unmap/synchronization completed.
 *
 * If the pointer matches, we treat the image as zero-copy and keep the cache entry locked while GPU
 * work is in flight.
 */
static gboolean _cl_is_zero_copy_image(const int devid, cl_mem mem, void *host_ptr, const dt_iop_roi_t *roi,
                                       const size_t bpp)
{
  if(devid < 0 || !mem || !host_ptr || !roi || roi->width <= 0 || roi->height <= 0 || bpp == 0) return FALSE;

  void *mapped = dt_opencl_map_image(devid, mem, TRUE, CL_MAP_READ, roi->width, roi->height, (int)bpp);
  if(!mapped) return FALSE;

  const gboolean ptr_matches = (mapped == host_ptr);
  const gboolean is_zero_copy = ptr_matches;
  const cl_int unmap_err = dt_opencl_unmap_mem_object(devid, mem, mapped);
  if(unmap_err != CL_SUCCESS) return FALSE;

  // Use clFinish rather than event wait: some drivers disable event tracking, but we still need to guarantee
  // the unmap (and implicit sync) is complete before touching host memory or unlocking the cache entry.
  dt_opencl_finish(devid);

  return is_zero_copy;
}

/**
 * @brief Try to fetch a reusable pinned OpenCL image from a cache entry.
 *
 * @param cache_entry Pixelpipe cache entry currently associated with `host_ptr`.
 * @param host_ptr Host buffer pointer used as the key for pinned images.
 * @param devid OpenCL device index.
 * @param roi Image dimensions.
 * @param bpp Bytes per pixel.
 * @param flags OpenCL mem flags we require (`CL_MEM_USE_HOST_PTR` etc.).
 * @param[out] out_cst Optional colorspace tag stored with the cached `cl_mem`.
 * @param[out] out_reused Optional boolean set to TRUE when we got a cached image.
 *
 * @return `cl_mem` as a `void *` if found, NULL otherwise.
 *
 * @details
 * We cache pinned images per *host pointer* in each pixelpipe cache entry. This matters because the host
 * pointer is the actual backing store for `CL_MEM_USE_HOST_PTR`. Reusing the pinned allocation avoids
 * repeated driver overhead and reduces fragmentation in OpenCL memory pools.
 */
static void *_gpu_try_reuse_pinned_from_cache(dt_pixel_cache_entry_t *cache_entry, void *host_ptr, int devid,
                                              const dt_iop_roi_t *roi, const size_t bpp, const int flags,
                                              int *out_cst, gboolean *out_reused)
{
  if(out_reused) *out_reused = FALSE;
  if(!cache_entry || !host_ptr || devid < 0) return NULL;

  int cached_cst = IOP_CS_NONE;
  void *mem = dt_pixel_cache_clmem_get(cache_entry, host_ptr, devid, roi->width, roi->height, (int)bpp, flags,
                                       &cached_cst);
  if(mem)
  {
    if(out_reused) *out_reused = TRUE;
    if(out_cst && cached_cst != IOP_CS_NONE) *out_cst = cached_cst;
  }

  return mem;
}

static void *_gpu_alloc_device_with_flush(int devid, const dt_iop_roi_t *roi, const size_t bpp);

/**
 * @brief Allocate a pinned (`CL_MEM_USE_HOST_PTR`) OpenCL image, with optional reuse from cache and a flush retry.
 *
 * @details
 * We prefer pinned buffers because they enable:
 *
 * - fast DMA transfers (map/unmap or explicit copies),
 * - potential true zero-copy on some devices/drivers,
 * - caching/reuse of the OpenCL image object across runs.
 *
 * If allocation fails, we flush cached `cl_mem` objects in the pixelpipe cache (`dt_dev_pixelpipe_cache_flush_clmem`)
 * and retry once. This is a pragmatic workaround for driver-side memory fragmentation and stale allocations.
 */
static void *_gpu_get_pinned_or_alloc(int devid, void *host_ptr, const dt_iop_roi_t *roi, const size_t bpp,
                                      dt_pixel_cache_entry_t *cache_entry, const gboolean reuse_pinned,
                                      int *out_cst, gboolean *out_reused)
{
  const int flags = CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR;
  void *mem = NULL;

  if(out_reused) *out_reused = FALSE;

  if(reuse_pinned)
    mem = _gpu_try_reuse_pinned_from_cache(cache_entry, host_ptr, devid, roi, bpp, flags, out_cst, out_reused);

  if(!mem)
    mem = dt_opencl_alloc_device_use_host_pointer(devid, roi->width, roi->height, (int)bpp, host_ptr, flags);

  if(!mem)
  {
    dt_dev_pixelpipe_cache_flush_clmem(darktable.pixelpipe_cache, devid);
    if(reuse_pinned)
      mem = _gpu_try_reuse_pinned_from_cache(cache_entry, host_ptr, devid, roi, bpp, flags, out_cst, out_reused);
    if(!mem)
      mem = dt_opencl_alloc_device_use_host_pointer(devid, roi->width, roi->height, (int)bpp, host_ptr, flags);
  }

  return mem;
}

/**
 * @brief Allocate a pure device-side OpenCL image, retrying once after flushing cached pinned buffers.
 *
 * @details
 * This is used when we intentionally do not want a pinned host-backed image (e.g. output buffers that we do
 * not plan to cache in RAM). Allocation failure triggers a clmem cache flush and one retry.
 */
static void *_gpu_alloc_device_with_flush(int devid, const dt_iop_roi_t *roi, const size_t bpp)
{
  void *mem = dt_opencl_alloc_device(devid, roi->width, roi->height, bpp);
  if(!mem)
  {
    dt_dev_pixelpipe_cache_flush_clmem(darktable.pixelpipe_cache, devid);
    mem = dt_opencl_alloc_device(devid, roi->width, roi->height, bpp);
  }
  return mem;
}

/**
 * @brief Optional debug counters for pinned-buffer reuse.
 *
 * @details
 * This is purely informational: it helps assess whether our pinned buffer caching strategy is effective.
 * It is intentionally static and local to the process to keep overhead negligible.
 */
static void _gpu_log_pinned_reuse(dt_iop_module_t *module, const gboolean reused_from_cache)
{
  static dt_atomic_int clmem_reuse_hits;
  static dt_atomic_int clmem_reuse_misses;

  if(reused_from_cache)
  {
    const int hits = dt_atomic_add_int(&clmem_reuse_hits, 1) + 1;
    const int misses = dt_atomic_get_int(&clmem_reuse_misses);
    dt_print(DT_DEBUG_OPENCL,
             "[opencl_pixelpipe] %s reused pinned input from cache (hits=%d, misses=%d)\n",
             module ? module->name() : "unknown", hits, misses);
  }
  else
  {
    (void)dt_atomic_add_int(&clmem_reuse_misses, 1);
  }
}

/**
 * @brief Initialize an OpenCL buffer for the pixelpipe.
 *
 * @param devid OpenCL device index.
 * @param host_ptr If non-NULL, request a pinned host-backed image (`CL_MEM_USE_HOST_PTR`).
 * @param roi Image dimensions.
 * @param bpp Bytes per pixel.
 * @param module Module for debug messages.
 * @param message Human-readable context for debug messages.
 * @param cache_entry Pixelpipe cache entry owning `host_ptr`, used to reuse/categorize pinned allocations.
 * @param reuse_pinned If TRUE and `host_ptr` is non-NULL, attempt to reuse a cached pinned allocation.
 * @param[out] out_cst Optional colorspace metadata (when reusing a cached pinned buffer).
 * @param[out] out_reused Optional flag set to TRUE when the OpenCL image came from the cache.
 *
 * @return An OpenCL image (`cl_mem`) as a `void *`, or NULL on failure.
 *
 * @details
 * If `host_ptr == NULL`, we allocate a plain device image and rely on explicit copies when needed.
 * If `host_ptr != NULL`, we allocate a pinned host-backed image, enabling (potentially) true zero-copy.
 */
static void *_gpu_init_buffer(int devid, void *const host_ptr, const dt_iop_roi_t *roi, const size_t bpp,
                              dt_iop_module_t *module, const char *message,
                              dt_pixel_cache_entry_t *cache_entry, const gboolean reuse_pinned,
                              int *out_cst, gboolean *out_reused)
{
  // Need to use read-write mode because of in-place color space conversions.
  void *cl_mem_input = NULL;
  gboolean reused_from_cache = FALSE;

  if(out_reused) *out_reused = FALSE;

  if(host_ptr)
  {
    cl_mem_input = _gpu_get_pinned_or_alloc(devid, host_ptr, roi, bpp, cache_entry, reuse_pinned,
                                           out_cst, &reused_from_cache);
  }
  else
  {
    cl_mem_input = _gpu_alloc_device_with_flush(devid, roi, bpp);
  }

  if(cl_mem_input == NULL)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_pixelpipe] couldn't generate %s buffer for module %s\n", message,
             module ? module->op : "unknown");
  }
  else if(reuse_pinned && cache_entry && host_ptr)
  {
    if(out_reused) *out_reused = reused_from_cache;
    _gpu_log_pinned_reuse(module, reused_from_cache);
  }

  return cl_mem_input;
}

/**
 * @brief Release or cache an OpenCL image associated with a host cache line.
 *
 * @param[in,out] cl_mem_buffer Pointer to a `cl_mem` stored as `void*`.
 * @param cache_entry Pixelpipe cache entry the host pointer belongs to (may be NULL).
 * @param host_ptr Host pointer backing the OpenCL image (may be NULL).
 * @param cst Colorspace tag to store alongside the cached `cl_mem` (only used when caching is possible).
 *
 * @details
 * This helper is a *single point of truth* for OpenCL image lifetime management in the pixelpipe:
 *
 * - If the image is host-backed (`CL_MEM_USE_HOST_PTR`) and we have both `cache_entry` and `host_ptr`,
 *   we put it in the cache entry's `cl_mem_list` for reuse.
 * - Otherwise, we release it immediately.
 *
 * Additionally, when we release an image, we must ensure there is no stale pointer in `cl_mem_list`
 * (for example, if some earlier path cached it and we are now deciding to free it). We call
 * `dt_pixel_cache_clmem_remove()` before releasing to keep the cache bookkeeping coherent.
 */
static void _gpu_clear_buffer(void **cl_mem_buffer, dt_pixel_cache_entry_t *cache_entry, void *host_ptr, int cst)
{
  if(cl_mem_buffer && *cl_mem_buffer != NULL)
  {
    cl_mem mem = *cl_mem_buffer;
    const cl_mem_flags flags = dt_opencl_get_mem_flags(mem);
    const gboolean can_cache = (cache_entry && host_ptr && (flags & CL_MEM_USE_HOST_PTR));
    if(can_cache)
    {
      const int devid = dt_opencl_get_mem_context_id(mem);
      const int width = dt_opencl_get_image_width(mem);
      const int height = dt_opencl_get_image_height(mem);
      const int bpp = dt_opencl_get_image_element_size(mem);
      dt_pixel_cache_clmem_put(cache_entry, host_ptr, devid, width, height, bpp, (int)flags, cst, mem);
    }
    else
    {
      if(cache_entry) dt_pixel_cache_clmem_remove(cache_entry, mem);
      dt_opencl_release_mem_object(mem);
    }
    *cl_mem_buffer = NULL;
  }
}

/**
 * @brief Synchronize between host memory and a pinned OpenCL image.
 *
 * @param devid OpenCL device index.
 * @param host_ptr Host pointer to read from / write to.
 * @param cl_mem_buffer OpenCL image.
 * @param roi Image dimensions.
 * @param cl_mode `CL_MAP_WRITE` for host→device, `CL_MAP_READ` for device→host.
 * @param bpp Bytes per pixel.
 * @param module Module for debug logs (may be NULL).
 * @param message Context string for debug logs.
 *
 * @return 0 on success, 1 on failure.
 *
 * @details
 * This function intentionally tries a hierarchy of synchronization mechanisms:
 *
 * 1. For `CL_MEM_USE_HOST_PTR` images, we *attempt* a map/unmap cycle. If the mapped pointer equals `host_ptr`,
 *    we treat it as true zero-copy and the map/unmap acts as a synchronization barrier (fast, avoids extra copies).
 * 2. Otherwise, we fall back to explicit blocking transfers (`dt_opencl_write_host_to_device` /
 *    `dt_opencl_read_host_from_device`).
 *
 * The map/unmap approach is used as a synchronization barrier because on many drivers it will:
 *
 * - flush CPU caches / invalidate as needed,
 * - ensure GPU work touching that memory is completed (for blocking map),
 * - and potentially avoid a full copy when true zero-copy is supported.
 */
static int _cl_pinned_memory_copy(const int devid, void *host_ptr, void *cl_mem_buffer, const dt_iop_roi_t *roi,
                                  int cl_mode, size_t bpp, dt_iop_module_t *module, const char *message)
{
  if(!host_ptr || !cl_mem_buffer) return 1;

  const cl_mem mem = (cl_mem)cl_mem_buffer;
  const cl_mem_flags flags = dt_opencl_get_mem_flags(mem);

  // Fast path for true zero-copy pinned images: map/unmap is enough to synchronize host<->device.
  if(flags & CL_MEM_USE_HOST_PTR)
  {
    void *mapped = dt_opencl_map_image(devid, mem, TRUE, cl_mode, roi->width, roi->height, (int)bpp);
    if(mapped)
    {
      const gboolean ptr_matches = (mapped == host_ptr);
      const cl_int unmap_err = dt_opencl_unmap_mem_object(devid, mem, mapped);
      if(unmap_err != CL_SUCCESS) return 1;

      // Ensure unmap (and any implicit sync) completed before we possibly enqueue explicit transfers.
      // When event tracking is disabled, clFinish is the only reliable barrier.
      dt_opencl_finish(devid);

      if(ptr_matches)
      {
        dt_print(DT_DEBUG_OPENCL,
                 "[opencl_pixelpipe] successfully synced image %s via map/unmap for module %s (%s)\n",
                 (cl_mode == CL_MAP_WRITE) ? "host to device" : "device to host",
                 (module) ? module->op : "base buffer", message);
        return 0;
      }
    }
  }

  // Fallback: explicit blocking transfers (safe on all drivers).
  cl_int err = CL_SUCCESS;
  if(cl_mode == CL_MAP_WRITE)
    err = dt_opencl_write_host_to_device(devid, host_ptr, mem, roi->width, roi->height, (int)bpp);
  else if(cl_mode == CL_MAP_READ)
    err = dt_opencl_read_host_from_device(devid, host_ptr, mem, roi->width, roi->height, (int)bpp);
  else
    return 1;

  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_pixelpipe] couldn't copy image %s for module %s (%s)\n",
             (cl_mode == CL_MAP_WRITE) ? "host to device" : "device to host",
             (module) ? module->op : "base buffer", message);
    return 1;
  }

  dt_print(DT_DEBUG_OPENCL, "[opencl_pixelpipe] successfully copied image %s for module %s (%s)\n",
           (cl_mode == CL_MAP_WRITE) ? "host to device" : "device to host",
           (module) ? module->op : "base buffer", message);
  return 0;
}

/**
 * @brief Force device → host resynchronization of the pixelpipe input cache line.
 *
 * @details
 * This is used when we are about to switch from GPU processing to CPU processing for a given module.
 * In that scenario, the most recent correct pixels may only exist in `cl_mem_input` (GPU-only intermediate),
 * while `input` (host pointer) is either NULL or stale.
 *
 * The function:
 *
 * - write-locks the cache entry (we are modifying host memory),
 * - performs a device→host copy (map/unmap if possible, explicit copy otherwise),
 * - updates the buffer descriptor colorspace tag,
 * - calls `dt_opencl_finish()` to ensure command queue completion before releasing the lock.
 */
static float *_resync_input_gpu_to_cache(dt_dev_pixelpipe_t *pipe, float *input, void *cl_mem_input,
                                         dt_iop_buffer_dsc_t *input_format, const dt_iop_roi_t *roi_in,
                                         dt_iop_module_t *module, dt_iop_colorspace_type_t input_cst_cl,
                                         const size_t in_bpp, dt_pixel_cache_entry_t *input_entry,
                                         const char *message)
{
  if(!cl_mem_input) return input;
  dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, 0, TRUE, input_entry);

  int fail = _cl_pinned_memory_copy(pipe->devid, input, cl_mem_input, roi_in, CL_MAP_READ, in_bpp, module, message);

  // Color conversions happen inplace, so we need to ensure colorspace metadata are up-to-date.
  if(!fail) input_format->cst = input_cst_cl;

  // Enforce the OpenCL pipe to run in sync with CPU RAM cache so lock validity is guaranteed.
  dt_opencl_finish(pipe->devid);
  dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, 0, FALSE, input_entry);

  // Update colorspace tag (again, for safety).
  input_format->cst = input_cst_cl;
  return input;
}

/**
 * @brief Prepare/obtain the OpenCL input image for a module.
 *
 * @param pipe Current pixelpipe (provides device id + global settings).
 * @param module Module being processed (for debug logs).
 * @param input Host input pointer (may be NULL on GPU-only paths).
 * @param[in,out] cl_mem_input OpenCL input image; may already be set by the previous module.
 * @param[in,out] input_cst_cl Colorspace tag associated with `cl_mem_input`.
 * @param roi_in ROI for the input buffer.
 * @param in_bpp Input bytes-per-pixel.
 * @param input_entry Pixelpipe cache entry corresponding to the input hash.
 * @param[out] locked_input_entry If non-NULL on return, the caller must unlock it after GPU work completed.
 *
 * @return 0 on success, 1 on failure.
 *
 * @details
 * There are two major cases:
 *
 * 1) `*cl_mem_input != NULL`:
 *    The previous module already produced an OpenCL buffer and we are continuing on GPU. We may still need to
 *    keep the cache entry locked if it is a true zero-copy pinned image.
 *
 * 2) `*cl_mem_input == NULL`:
 *    We start from a host cache buffer (`input`). We allocate (or reuse) a pinned image backed by that host buffer,
 *    and if it is not true zero-copy we push host→device once before running kernels.
 */
static int _gpu_prepare_cl_input(dt_dev_pixelpipe_t *pipe, dt_iop_module_t *module,
                                 float *input, void **cl_mem_input, dt_iop_colorspace_type_t *input_cst_cl,
                                 const dt_iop_roi_t *roi_in, const size_t in_bpp,
                                 dt_pixel_cache_entry_t *input_entry, dt_pixel_cache_entry_t **locked_input_entry)
{
  if(!locked_input_entry) return 1;
  *locked_input_entry = NULL;

  if(*cl_mem_input != NULL)
  {
    // We passed the OpenCL memory buffer through directly on vRAM from previous module.
    // This is fast and efficient.
    // If it's a true zero-copy pinned image, keep the input cache entry read-locked until kernels complete,
    // otherwise another thread may overwrite host memory while the GPU is still reading it.
    dt_print(DT_DEBUG_OPENCL, "[dev_pixelpipe] %s will use its input directly from vRAM\n", module->name());
    const cl_mem mem = (cl_mem)*cl_mem_input;
    const cl_mem_flags flags = dt_opencl_get_mem_flags(mem);
    if(flags & CL_MEM_USE_HOST_PTR)
      if(_cl_is_zero_copy_image(pipe->devid, mem, input, roi_in, in_bpp))
      {
        dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, 0, TRUE, input_entry);
        *locked_input_entry = input_entry;
      }
    return 0;
  }

  if(!input)
  {
    dt_print(DT_DEBUG_OPENCL, "[dev_pixelpipe] %s has no input (cache)\n", module->name());
    return 1;
  }

  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, 0, TRUE, input_entry);

  // Try to reuse a cached pinned buffer; otherwise allocate a new pinned image backed by `input`.
  gboolean input_reused_from_cache = FALSE;
  *cl_mem_input = _gpu_init_buffer(pipe->devid, input, roi_in, in_bpp, module, "input", input_entry, TRUE,
                                  input_cst_cl, &input_reused_from_cache);
  int fail = (*cl_mem_input == NULL);

  // If the input is true zero-copy, the GPU will access host memory asynchronously: keep the cache
  // entry read-locked until all kernels have completed. If not, drivers may use a device-side copy
  // which must be synchronized from the host before running kernels.
  gboolean keep_lock = FALSE;
  cl_mem mem = NULL;
  if(!fail && *cl_mem_input)
  {
    mem = (cl_mem)*cl_mem_input;
    const cl_mem_flags flags = dt_opencl_get_mem_flags(mem);
    if(flags & CL_MEM_USE_HOST_PTR)
      keep_lock = _cl_is_zero_copy_image(pipe->devid, mem, input, roi_in, in_bpp);
  }

  if(!fail && mem && !keep_lock)
  {
    const cl_int err = dt_opencl_write_host_to_device(pipe->devid, input, mem, roi_in->width, roi_in->height,
                                                      (int)in_bpp);
    if(err != CL_SUCCESS)
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_pixelpipe] couldn't copy image host to device for module %s (%s)\n",
               (module) ? module->op : "base buffer", "cache to input");
      fail = TRUE;
    }
    else
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_pixelpipe] successfully copied image host to device for module %s (%s)\n",
               (module) ? module->op : "base buffer", "cache to input");
    }
  }

  // Enforce sync with the CPU/RAM cache so lock validity is guaranteed.
  dt_opencl_events_wait_for(pipe->devid);

  if(keep_lock)
    *locked_input_entry = input_entry;
  else
    dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, 0, FALSE, input_entry);

  return fail ? 1 : 0;
}

#else // HAVE_OPENCL

/**
 * @brief No-OpenCL stub for `_gpu_clear_buffer()`.
 *
 * @details
 * The pixelpipe code keeps `cl_mem` pointers around even when OpenCL is not compiled in, because the control
 * flow is shared. In non-OpenCL builds those pointers must be treated as "always NULL".
 *
 * This stub keeps the caller code simple and avoids littering the pixelpipe with preprocessor conditionals.
 */
static inline void _gpu_clear_buffer(void **cl_mem_buffer, dt_pixel_cache_entry_t *cache_entry, void *host_ptr, int cst)
{
  (void)cache_entry;
  (void)host_ptr;
  (void)cst;
  if(cl_mem_buffer) *cl_mem_buffer = NULL;
}

#endif // HAVE_OPENCL

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
