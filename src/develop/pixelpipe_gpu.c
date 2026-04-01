/*
    Private OpenCL pixelpipe backend.
*/

#include "common/darktable.h"
#include "common/iop_order.h"
#include "common/opencl.h"
#include "develop/blend.h"
#include "develop/pixelpipe_cache.h"
#include "develop/pixelpipe_cpu.h"
#include "develop/pixelpipe_gpu.h"

#include <math.h>
#include <stdio.h>

void dt_dev_pixelpipe_gpu_flush_host_pinned_images(dt_dev_pixelpipe_t *pipe, void *host_ptr,
                                                   dt_pixel_cache_entry_t *cache_entry, const char *reason)
{
#ifdef HAVE_OPENCL
  if(pipe && !pipe->realtime && pipe->devid >= 0 && host_ptr && cache_entry)
  {
    /* Non-realtime host writes invalidate reusable pinned images bound to the previous ROI/hash.
     * Realtime keeps its pinned reuse untouched to avoid stalling the live draw path. */
    dt_dev_pixelpipe_cache_flush_host_pinned_image(darktable.pixelpipe_cache, host_ptr, cache_entry,
                                                   pipe->devid);
    dt_print(DT_DEBUG_OPENCL, "[dev_pixelpipe] flushed pinned OpenCL images after %s\n",
             reason ? reason : "host write");
  }
#else
  (void)pipe;
  (void)host_ptr;
  (void)cache_entry;
  (void)reason;
#endif
}

#ifdef HAVE_OPENCL

static int _is_opencl_supported(dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece, dt_iop_module_t *module)
{
  return dt_opencl_is_inited() && piece->process_cl_ready && module->process_cl;
}

static int _gpu_init_input(dt_dev_pixelpipe_t *pipe,
                           float **input, void **cl_mem_input,
                           const dt_dev_pixelpipe_iop_t *piece, dt_develop_tiling_t *tiling,
                           dt_pixel_cache_entry_t *input_entry, dt_pixel_cache_entry_t *output_entry)
{
  dt_iop_module_t *module = piece->module;

  if(*input == NULL)
  {
    dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, TRUE, input_entry);
    *input = dt_pixel_cache_alloc(darktable.pixelpipe_cache, input_entry);
    dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, FALSE, input_entry);
  }

  if(*input == NULL)
  {
    dt_print(DT_DEBUG_OPENCL,
             "[dev_pixelpipe] %s CPU fallback has no input buffer (cache allocation failed?)\n",
             module->name());
    return 1;
  }

  dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, TRUE, input_entry);
  const int fail = dt_dev_pixelpipe_cache_sync_cl_buffer(pipe->devid, *input, *cl_mem_input, &piece->roi_in, CL_MAP_READ,
                                          piece->dsc_in.bpp, module,
                                          "cpu fallback input copy to cache");
  dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, FALSE, input_entry);

  if(fail)
  {
    dt_print(DT_DEBUG_OPENCL,
             "[dev_pixelpipe] %s couldn't resync GPU input to cache for CPU fallback\n",
             module->name());
    return 1;
  }
  return 0;
}

static int _gpu_early_cpu_fallback_if_unsupported(dt_dev_pixelpipe_t *pipe, float **input,
                                                  void **cl_mem_input,
                                                  gboolean *const borrowed_cl_mem_input,
                                                  const dt_dev_pixelpipe_iop_t *piece,
                                                  const dt_dev_pixelpipe_iop_t *previous_piece,
                                                  dt_develop_tiling_t *tiling,
                                                  dt_pixelpipe_flow_t *pixelpipe_flow,
                                                  gboolean *const cache_output,
                                                  dt_pixel_cache_entry_t *input_entry,
                                                  dt_pixel_cache_entry_t *output_entry)
{
  dt_iop_module_t *module = piece->module;

  dt_print(DT_DEBUG_OPENCL, "[dev_pixelpipe] %s will run directly on CPU\n", module->name());

  if(cl_mem_input && *cl_mem_input != NULL)
  {
    if(input && *input == NULL)
    {
      dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, TRUE, input_entry);
      *input = dt_pixel_cache_alloc(darktable.pixelpipe_cache, input_entry);
      dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, FALSE, input_entry);
    }

    if(!input || *input == NULL)
    {
      dt_print(DT_DEBUG_OPENCL,
               "[dev_pixelpipe] %s CPU fallback has no input buffer (cache allocation failed?)\n",
               module->name());
      if(borrowed_cl_mem_input && *borrowed_cl_mem_input)
      {
        dt_dev_pixelpipe_cache_return_cl_payload(input_entry, *cl_mem_input);
        *cl_mem_input = NULL;
        *borrowed_cl_mem_input = FALSE;
      }
      else
        dt_dev_pixelpipe_cache_release_cl_buffer(cl_mem_input, input_entry, NULL,
                                          dt_dev_pixelpipe_cache_gpu_device_buffer(pipe, input_entry));
      return 1;
    }

    *input = dt_dev_pixelpipe_cache_restore_cl_buffer(pipe, *input, *cl_mem_input, &piece->roi_in, module,
                                        piece->dsc_in.bpp, input_entry,
                                        "cpu fallback input copy to cache");
    if(*input == NULL)
    {
      dt_print(DT_DEBUG_OPENCL,
               "[dev_pixelpipe] %s couldn't resync GPU input to cache for CPU fallback\n",
               module->name());
      if(borrowed_cl_mem_input && *borrowed_cl_mem_input)
      {
        dt_dev_pixelpipe_cache_return_cl_payload(input_entry, *cl_mem_input);
        *cl_mem_input = NULL;
        *borrowed_cl_mem_input = FALSE;
      }
      else
        dt_dev_pixelpipe_cache_release_cl_buffer(cl_mem_input, input_entry, NULL,
                                          dt_dev_pixelpipe_cache_gpu_device_buffer(pipe, input_entry));
      return 1;
    }
  }
  else if(input && *input != NULL)
  {
    void *cached_pinned_input = dt_dev_pixelpipe_cache_borrow_cl_payload(input_entry, *input, pipe->devid,
                                                         piece->roi_in.width, piece->roi_in.height,
                                                         piece->dsc_in.bpp,
                                                         CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR);
    if(cached_pinned_input != NULL)
    {
      dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, TRUE, input_entry);
      const int fail = dt_dev_pixelpipe_cache_sync_cl_buffer(pipe->devid, *input, cached_pinned_input, &piece->roi_in,
                                              CL_MAP_READ, piece->dsc_in.bpp, module,
                                              "cpu fallback pinned input copy to cache");
      dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, FALSE, input_entry);
      dt_dev_pixelpipe_cache_return_cl_payload(input_entry, cached_pinned_input);

      if(fail)
      {
        dt_print(DT_DEBUG_OPENCL,
                 "[dev_pixelpipe] %s couldn't resync cached pinned input to cache for CPU fallback\n",
                 module->name());
        return 1;
      }
    }
  }
  else if(!input || *input == NULL)
  {
    dt_print(DT_DEBUG_OPENCL,
             "[dev_pixelpipe] %s CPU fallback has no input buffer (cache allocation failed?)\n",
             module->name());
    return 1;
  }

  if(borrowed_cl_mem_input && *borrowed_cl_mem_input)
  {
    /* Device-only inputs borrowed from the cache stay owned by the cache entry.
     * CPU fallback only needs to drop the temporary borrow after the device->host
     * sync, otherwise releasing the cl_mem here leaves a stale cache-side pointer
     * that later thumbnail runs may reopen as corrupted input. */
    dt_dev_pixelpipe_cache_return_cl_payload(input_entry, *cl_mem_input);
    *cl_mem_input = NULL;
    *borrowed_cl_mem_input = FALSE;
  }
  else
    dt_dev_pixelpipe_cache_release_cl_buffer(cl_mem_input, input_entry, *input,
                                      dt_dev_pixelpipe_cache_gpu_device_buffer(pipe, input_entry));

  return pixelpipe_process_on_CPU(pipe, piece, previous_piece, tiling, pixelpipe_flow,
                                  cache_output, input_entry, output_entry);
}

int pixelpipe_process_on_GPU(dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece,
                             const dt_dev_pixelpipe_iop_t *previous_piece,
                             dt_develop_tiling_t *tiling,
                             dt_pixelpipe_flow_t *pixelpipe_flow,
                             gboolean *const cache_output,
                             dt_pixel_cache_entry_t *input_entry, dt_pixel_cache_entry_t *output_entry)
{
  dt_iop_module_t *module = piece->module;
  float *input = dt_pixel_cache_entry_get_data(input_entry);
  void *output = dt_pixel_cache_entry_get_data(output_entry);
  void *cl_mem_input = NULL;
  void *cl_mem_output = NULL;
  void *cl_mem_process_input = NULL;
  void *cl_mem_blend_input = NULL;
  void *cl_mem_blend_output = NULL;
  void *cl_mem_process_input_temp = NULL;
  void *cl_mem_blend_input_temp = NULL;
  void *cl_mem_blend_output_temp = NULL;
  dt_pixel_cache_entry_t *cpu_input_entry = input_entry;
  dt_pixel_cache_entry_t *locked_input_entry = NULL;
  gboolean borrowed_cl_mem_input = FALSE;
  const dt_iop_buffer_dsc_t actual_input_dsc = previous_piece ? previous_piece->dsc_out : pipe->image.dsc;
  dt_iop_buffer_dsc_t process_input_dsc = actual_input_dsc;
  dt_iop_buffer_dsc_t blend_input_dsc = actual_input_dsc;
  dt_iop_buffer_dsc_t blend_output_dsc = piece->dsc_out;
  /* The recursion already owns `input_entry`, so GPU payload recovery must happen from that entry
   * directly instead of going back through the hash lookup path. Hash lookup is for published
   * cache hits; here we are consuming the current upstream stage inside the same pipeline run.
   * When the cacheline already carries a host-backed pinned image, reopen that payload first so
   * GPU-to-GPU stages can keep flowing through the cached OpenCL image instead of rebuilding it
   * from host RAM. Base-buffer initialization also seeds this path for the first OpenCL stage. */
  if(input != NULL && pipe->devid >= 0)
  {
    cl_mem_input = dt_dev_pixelpipe_cache_borrow_cl_payload(input_entry, input, pipe->devid,
                                            piece->roi_in.width, piece->roi_in.height,
                                            piece->dsc_in.bpp,
                                            CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR);
    borrowed_cl_mem_input = (cl_mem_input != NULL);
  }

  if(cl_mem_input == NULL && input == NULL && pipe->devid >= 0)
  {
    cl_mem_input = dt_dev_pixelpipe_cache_borrow_cl_payload(input_entry, NULL, pipe->devid,
                                            piece->roi_in.width, piece->roi_in.height,
                                            piece->dsc_in.bpp, CL_MEM_READ_WRITE);
    borrowed_cl_mem_input = (cl_mem_input != NULL);
  }

  /* Recursive thumbnail/export stages can reopen a transient upstream cacheline before it becomes
   * a published exact-hit. When that cacheline only carries a cached device payload, OpenCL reuse
   * should normally recover it through `dt_dev_pixelpipe_cache_borrow_cl_payload()`. If that fast path misses, we
   * still need a host fallback buffer before aborting the whole pipeline, otherwise one bad reopen
   * collapses the thumbnail to the 8x8 skull placeholder. */
  if(input == NULL && cl_mem_input == NULL
     && dt_dev_pixelpipe_cache_restore_host_payload(darktable.pixelpipe_cache, input_entry,
                                                    pipe->devid, (void **)&input))
  {
    dt_print(DT_DEBUG_OPENCL,
             "[dev_pixelpipe] %s restored host input from cached device payload for CPU fallback\n",
             module->name());
  }

  if(input == NULL && cl_mem_input == NULL)
  {
    dt_print(DT_DEBUG_OPENCL, "[dev_pixelpipe] %s has no RAM nor vRAM input... aborting.\n", module->name());
    return 1;
  }

  if(!_is_opencl_supported(pipe, piece, module) || !pipe->opencl_enabled || !(pipe->devid >= 0))
  {
    return _gpu_early_cpu_fallback_if_unsupported(pipe, &input, &cl_mem_input,
                                                  &borrowed_cl_mem_input, piece, previous_piece, tiling,
                                                  pixelpipe_flow, cache_output,
                                                  input_entry, output_entry);
  }

  const dt_iop_order_iccprofile_info_t *const work_profile
      = (process_input_dsc.cst != IOP_CS_RAW || piece->dsc_in.cst != IOP_CS_RAW)
            ? dt_ioppr_get_pipe_work_profile_info(pipe)
            : NULL;

  const float required_factor_cl
      = fmaxf(1.0f, (cl_mem_input != NULL) ? tiling->factor_cl - 1.0f : tiling->factor_cl);

  const size_t precheck_width = ROUNDUPDWD(MAX(piece->roi_in.width, piece->roi_out.width), pipe->devid);
  const size_t precheck_height = ROUNDUPDHT(MAX(piece->roi_in.height, piece->roi_out.height), pipe->devid);
  gboolean fits_on_device = dt_opencl_image_fits_device(pipe->devid, precheck_width, precheck_height,
                                                        MAX(piece->dsc_in.bpp, piece->dsc_out.bpp),
                                                        required_factor_cl, tiling->overhead);
  if(!fits_on_device)
  {
    dt_print(DT_DEBUG_OPENCL,
             "[dev_pixelpipe] %s pre-check didn't fit on device, flushing cached pinned buffers and retrying\n",
             module->name());
    dt_dev_pixelpipe_cache_flush_clmem(darktable.pixelpipe_cache, pipe->devid, cl_mem_input);
    fits_on_device = dt_opencl_image_fits_device(pipe->devid, precheck_width, precheck_height,
                                                 MAX(piece->dsc_in.bpp, piece->dsc_out.bpp),
                                                 required_factor_cl, tiling->overhead);
  }

  gboolean possible_cl = !(pipe->type == DT_DEV_PIXELPIPE_PREVIEW
                           && (module->flags() & IOP_FLAGS_PREVIEW_NON_OPENCL))
                         && (fits_on_device || piece->process_tiling_ready);

  if(!possible_cl || !fits_on_device) *cache_output = TRUE;
  if(*cache_output && output == NULL)
  {
    output = dt_pixel_cache_alloc(darktable.pixelpipe_cache, output_entry);
    if(output == NULL) goto error;
  }

  if(possible_cl && !fits_on_device)
  {
    const float cl_px = dt_opencl_get_device_available(pipe->devid)
                        / (sizeof(float) * MAX(piece->dsc_in.bpp, piece->dsc_out.bpp)
                           * ceilf(required_factor_cl));
    const float dx = MAX(piece->roi_in.width, piece->roi_out.width);
    const float dy = MAX(piece->roi_in.height, piece->roi_out.height);
    const float border = tiling->overlap + 1;
    const gboolean possible = (cl_px > dx * border) || (cl_px > dy * border) || (cl_px > border * border);
    if(!possible)
    {
      dt_print(DT_DEBUG_OPENCL | DT_DEBUG_TILING,
               "[dt_dev_pixelpipe_process_rec] CL: tiling impossible in module `%s'. avail=%.1fM, requ=%.1fM (%ix%i). overlap=%i\n",
               module->name(), cl_px / 1e6f, dx * dy / 1e6f, (int)dx, (int)dy, (int)tiling->overlap);
      goto error;
    }

    if(_gpu_init_input(pipe, &input, &cl_mem_input, piece, tiling,
                       input_entry, output_entry))
      goto error;
  }

  if(!possible_cl) goto error;

  if(fits_on_device)
  {
    if(dt_dev_pixelpipe_cache_prepare_cl_input(pipe, module, input, &cl_mem_input,
                             &piece->roi_in, piece->dsc_in.bpp, input_entry,
                             &locked_input_entry, NULL))
      goto error;
    cl_mem_process_input = cl_mem_input;

    if(cl_mem_output == NULL)
    {
      const gboolean reuse_output_cacheline = _requests_cache(pipe, piece)
                                              && (pipe->realtime || !(*cache_output));
      const gboolean reuse_output_pinned = reuse_output_cacheline;
      cl_mem_output = dt_dev_pixelpipe_cache_get_cl_buffer(pipe->devid, output, &piece->roi_out, piece->dsc_out.bpp, module,
                                       "output", output_entry, reuse_output_pinned, reuse_output_cacheline,
                                       NULL, cl_mem_input);
      if(cl_mem_output == NULL) goto error;
    }

    const int cst_before_cl = process_input_dsc.cst;
    if(process_input_dsc.cst != piece->dsc_in.cst
       && !(dt_iop_colorspace_is_rgb(process_input_dsc.cst) && dt_iop_colorspace_is_rgb(piece->dsc_in.cst)))
    {
      cl_mem_process_input_temp = dt_dev_pixelpipe_cache_alloc_cl_device_buffer(pipe->devid, &piece->roi_in, piece->dsc_in.bpp,
                                                               module, "module input colorspace temp",
                                                               cl_mem_input);
      if(cl_mem_process_input_temp == NULL)
        goto error;

      if(!dt_ioppr_transform_image_colorspace_cl(module, pipe->devid, cl_mem_input, cl_mem_process_input_temp,
                                                 piece->roi_in.width, piece->roi_in.height,
                                                 process_input_dsc.cst, piece->dsc_in.cst,
                                                 &process_input_dsc.cst, work_profile))
        goto error;
      cl_mem_process_input = cl_mem_process_input_temp;
    }
    else if(process_input_dsc.cst != piece->dsc_in.cst)
    {
      process_input_dsc.cst = piece->dsc_in.cst;
    }
    const int cst_after_cl = process_input_dsc.cst;

    dt_dev_pixelpipe_debug_dump_module_io(pipe, module, "pre", TRUE, &piece->dsc_in, &piece->dsc_out,
                                          &piece->roi_in, &piece->roi_out,
                                          process_input_dsc.bpp, piece->dsc_out.bpp,
                                          cst_before_cl, cst_after_cl);

      if(!module->process_cl(module, pipe, piece, cl_mem_process_input, cl_mem_output))
      goto error;

    *pixelpipe_flow |= PIXELPIPE_FLOW_PROCESSED_ON_GPU;
    *pixelpipe_flow &= ~(PIXELPIPE_FLOW_PROCESSED_ON_CPU | PIXELPIPE_FLOW_PROCESSED_WITH_TILING);

    if(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
    {
      const dt_dev_pixelpipe_display_mask_t request_mask_display
          = (module->dev->gui_attached && (module == module->dev->gui_module) && (pipe == module->dev->pipe))
                ? module->request_mask_display
                : DT_DEV_PIXELPIPE_DISPLAY_NONE;
      const dt_pixelpipe_blend_transform_t blend_transforms
          = dt_dev_pixelpipe_transform_for_blend(module, piece, &piece->dsc_out);
      cl_mem_blend_input = cl_mem_process_input;
      cl_mem_blend_output = cl_mem_output;
      blend_input_dsc = process_input_dsc;
      blend_output_dsc = piece->dsc_out;
      if(blend_transforms != DT_DEV_PIXELPIPE_BLEND_TRANSFORM_NONE)
      {
        dt_iop_colorspace_type_t blend_cst = dt_develop_blend_colorspace(piece, piece->dsc_out.cst);
        int success = 1;
        const int blend_in_before = blend_input_dsc.cst;
        if(blend_transforms & DT_DEV_PIXELPIPE_BLEND_TRANSFORM_INPUT)
        {
          cl_mem_blend_input_temp = dt_dev_pixelpipe_cache_alloc_cl_device_buffer(pipe->devid, &piece->roi_in, piece->dsc_in.bpp,
                                                                 module, "blend input colorspace temp",
                                                                 cl_mem_process_input);
          if(cl_mem_blend_input_temp == NULL)
            goto error;

          success &= dt_ioppr_transform_image_colorspace_cl(module, pipe->devid,
                                                            cl_mem_process_input, cl_mem_blend_input_temp,
                                                            piece->roi_in.width, piece->roi_in.height,
                                                            blend_input_dsc.cst, blend_cst,
                                                            &blend_input_dsc.cst, work_profile);
          cl_mem_blend_input = cl_mem_blend_input_temp;
        }
        const int blend_in_after = blend_input_dsc.cst;
        dt_dev_pixelpipe_debug_dump_module_io(pipe, module, "blend-in", TRUE,
                                              &process_input_dsc, &blend_input_dsc,
                                              &piece->roi_in, &piece->roi_in,
                                              process_input_dsc.bpp, blend_input_dsc.bpp,
                                              blend_in_before, blend_in_after);
        const int blend_out_before = blend_output_dsc.cst;
        if(blend_transforms & DT_DEV_PIXELPIPE_BLEND_TRANSFORM_OUTPUT)
        {
          cl_mem_blend_output_temp = dt_dev_pixelpipe_cache_alloc_cl_device_buffer(pipe->devid, &piece->roi_out,
                                                                  piece->dsc_out.bpp, module,
                                                                  "blend output colorspace temp", cl_mem_output);
          if(cl_mem_blend_output_temp == NULL)
            goto error;

          success &= dt_ioppr_transform_image_colorspace_cl(module, pipe->devid, cl_mem_output,
                                                            cl_mem_blend_output_temp, piece->roi_out.width,
                                                            piece->roi_out.height, blend_output_dsc.cst, blend_cst,
                                                            &blend_output_dsc.cst, work_profile);
          cl_mem_blend_output = cl_mem_blend_output_temp;
        }
        const int blend_out_after = blend_output_dsc.cst;
        dt_dev_pixelpipe_debug_dump_module_io(pipe, module, "blend-out", TRUE,
                                              &piece->dsc_out, &blend_output_dsc,
                                              &piece->roi_out, &piece->roi_out,
                                              piece->dsc_out.bpp, blend_output_dsc.bpp,
                                              blend_out_before, blend_out_after);

        if(!success)
        {
          dt_print(DT_DEBUG_OPENCL, "[opencl_pixelpipe] couldn't transform blending colorspace for module %s\n",
                   module->name());
          goto error;
        }
      }

      if(dt_develop_blend_process_cl(module, pipe, piece, cl_mem_blend_input, cl_mem_blend_output))
        goto error;

      if((blend_transforms & DT_DEV_PIXELPIPE_BLEND_TRANSFORM_OUTPUT)
         && request_mask_display & DT_DEV_PIXELPIPE_DISPLAY_ANY)
      {
        size_t origin[] = { 0, 0, 0 };
        size_t region[] = { piece->roi_out.width, piece->roi_out.height, 1 };
        if(dt_opencl_enqueue_copy_image(pipe->devid, cl_mem_blend_output, cl_mem_output, origin, origin,
                                        region) != CL_SUCCESS)
          goto error;
      }
      else if((blend_transforms & DT_DEV_PIXELPIPE_BLEND_TRANSFORM_OUTPUT)
              && !dt_ioppr_transform_image_colorspace_cl(module, pipe->devid, cl_mem_blend_output,
                                                         cl_mem_output, piece->roi_out.width,
                                                         piece->roi_out.height, blend_output_dsc.cst,
                                                         piece->dsc_out.cst, &blend_output_dsc.cst,
                                                         work_profile))
        goto error;

      *pixelpipe_flow |= PIXELPIPE_FLOW_BLENDED_ON_GPU;
      *pixelpipe_flow &= ~(PIXELPIPE_FLOW_BLENDED_ON_CPU);
    }

    if(*cache_output)
    {
      if(dt_dev_pixelpipe_cache_sync_cl_buffer(pipe->devid, output, cl_mem_output, &piece->roi_out, CL_MAP_READ,
                                piece->dsc_out.bpp, module,
                                "output to cache"))
        goto error;
      dt_print(DT_DEBUG_OPENCL, "[dev_pixelpipe] output memory was copied to cache for %s\n", module->name());
    }
  }
  else if(piece->process_tiling_ready && input != NULL)
  {
    const float *module_input = input;
    const float *blend_input = input;
    float *module_input_temp = NULL;
    float *blend_input_temp = NULL;
    gboolean input_locked = FALSE;

    if(borrowed_cl_mem_input)
    {
      dt_dev_pixelpipe_cache_return_cl_payload(input_entry, cl_mem_input);
      cl_mem_input = NULL;
      borrowed_cl_mem_input = FALSE;
    }
    else
      dt_dev_pixelpipe_cache_release_cl_buffer(&cl_mem_input, input_entry, input,
                                        dt_dev_pixelpipe_cache_gpu_device_buffer(pipe, input_entry));

    if(process_input_dsc.cst != piece->dsc_in.cst
       && !(dt_iop_colorspace_is_rgb(process_input_dsc.cst) && dt_iop_colorspace_is_rgb(piece->dsc_in.cst)))
    {
      module_input_temp
          = dt_pixelpipe_cache_alloc_align_float((size_t)piece->roi_in.width * piece->roi_in.height * 4, pipe);
      if(module_input_temp == NULL)
        goto error;

      dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, TRUE, input_entry);
      input_locked = TRUE;
      dt_ioppr_transform_image_colorspace(module, input, module_input_temp, piece->roi_in.width,
                                          piece->roi_in.height, process_input_dsc.cst, piece->dsc_in.cst,
                                          &process_input_dsc.cst, work_profile);
      dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, FALSE, input_entry);
      input_locked = FALSE;
      module_input = module_input_temp;
    }
    else if(process_input_dsc.cst != piece->dsc_in.cst)
    {
      process_input_dsc.cst = piece->dsc_in.cst;
      dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, TRUE, input_entry);
      input_locked = TRUE;
    }
    else
    {
      dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, TRUE, input_entry);
      input_locked = TRUE;
    }

    int fail = !module->process_tiling_cl(module, pipe, piece, module_input, output, piece->dsc_in.bpp);
    dt_opencl_finish(pipe->devid);

    if(fail)
    {
      if(input_locked)
        dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, FALSE, input_entry);
      dt_pixelpipe_cache_free_align(module_input_temp);
      goto error;
    }

    *pixelpipe_flow |= (PIXELPIPE_FLOW_PROCESSED_ON_GPU | PIXELPIPE_FLOW_PROCESSED_WITH_TILING);
    *pixelpipe_flow &= ~(PIXELPIPE_FLOW_PROCESSED_ON_CPU);

    blend_input = module_input;
    blend_input_dsc = process_input_dsc;
    void *blend_output = output;
    blend_output_dsc = piece->dsc_out;

    const dt_dev_pixelpipe_display_mask_t request_mask_display
        = (module->dev->gui_attached && (module == module->dev->gui_module) && (pipe == module->dev->pipe))
              ? module->request_mask_display
              : DT_DEV_PIXELPIPE_DISPLAY_NONE;
    const dt_pixelpipe_blend_transform_t blend_transforms
        = dt_dev_pixelpipe_transform_for_blend(module, piece, &piece->dsc_out);
    if(blend_transforms != DT_DEV_PIXELPIPE_BLEND_TRANSFORM_NONE)
    {
      dt_iop_colorspace_type_t blend_cst = dt_develop_blend_colorspace(piece, piece->dsc_out.cst);

      if(blend_transforms & DT_DEV_PIXELPIPE_BLEND_TRANSFORM_INPUT)
      {
        blend_input_temp
            = dt_pixelpipe_cache_alloc_align_float((size_t)piece->roi_in.width * piece->roi_in.height * 4, pipe);
        if(blend_input_temp == NULL)
        {
          if(input_locked)
            dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, FALSE, input_entry);
          dt_pixelpipe_cache_free_align(module_input_temp);
          goto error;
        }

        dt_ioppr_transform_image_colorspace(module, module_input, blend_input_temp, piece->roi_in.width,
                                            piece->roi_in.height, blend_input_dsc.cst, blend_cst,
                                            &blend_input_dsc.cst, work_profile);
        blend_input = blend_input_temp;
        if(input_locked)
        {
          dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, FALSE, input_entry);
          input_locked = FALSE;
        }
      }

      if(blend_transforms & DT_DEV_PIXELPIPE_BLEND_TRANSFORM_OUTPUT)
      {
        float *blend_output_temp
            = dt_pixelpipe_cache_alloc_align_float((size_t)piece->roi_out.width * piece->roi_out.height * 4, pipe);
        if(blend_output_temp == NULL)
        {
          if(input_locked)
            dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, FALSE, input_entry);
          dt_pixelpipe_cache_free_align(blend_input_temp);
          dt_pixelpipe_cache_free_align(module_input_temp);
          goto error;
        }

        dt_ioppr_transform_image_colorspace(module, output, blend_output_temp, piece->roi_out.width,
                                            piece->roi_out.height, blend_output_dsc.cst, blend_cst,
                                            &blend_output_dsc.cst, work_profile);
        blend_output = blend_output_temp;
      }
    }

    dt_develop_blend_process(module, pipe, piece, blend_input, blend_output);
    *pixelpipe_flow |= PIXELPIPE_FLOW_BLENDED_ON_CPU;
    *pixelpipe_flow &= ~(PIXELPIPE_FLOW_BLENDED_ON_GPU);

    if((blend_transforms & DT_DEV_PIXELPIPE_BLEND_TRANSFORM_OUTPUT))
    {
      if(request_mask_display & DT_DEV_PIXELPIPE_DISPLAY_ANY)
      {
        memcpy(output, blend_output,
               (size_t)piece->roi_out.width * piece->roi_out.height * piece->dsc_out.bpp);
      }
      else
      {
        dt_ioppr_transform_image_colorspace(module, blend_output, output, piece->roi_out.width,
                                            piece->roi_out.height, blend_output_dsc.cst, piece->dsc_out.cst,
                                            &blend_output_dsc.cst, work_profile);
      }
    }

    if(input_locked)
      dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, FALSE, input_entry);
    if(blend_output != output)
      dt_pixelpipe_cache_free_align(blend_output);
    dt_pixelpipe_cache_free_align(blend_input_temp);
    dt_pixelpipe_cache_free_align(module_input_temp);
  }
  else
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_pixelpipe] could not run module '%s' on gpu. falling back to cpu path\n",
             module->name());
    goto error;
  }

  dt_opencl_finish(pipe->devid);

  if(locked_input_entry)
    dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, FALSE, locked_input_entry);

  /* Borrowed vRAM inputs must stay protected until the current queue completed, otherwise
   * another pipe can flush or recycle the shared device buffer while the queued kernels
   * are still reading it. */
  if(borrowed_cl_mem_input)
  {
    dt_dev_pixelpipe_cache_return_cl_payload(input_entry, cl_mem_input);
    cl_mem_input = NULL;
    borrowed_cl_mem_input = FALSE;
  }
  else
    dt_dev_pixelpipe_cache_release_cl_buffer(&cl_mem_input, input_entry, input,
                                      dt_dev_pixelpipe_cache_gpu_device_buffer(pipe, input_entry));

  /* The backend now owns the authoritative module output payload until publish time.
   * When the output stayed GPU-only, the recursion no longer carries `cl_mem_output`
   * back explicitly, so we must cache or release it here before returning. Otherwise
   * the caller publishes a cacheline with metadata only and no recoverable payload. */
  dt_dev_pixelpipe_cache_release_cl_buffer(&cl_mem_output, output_entry, output,
                                    dt_dev_pixelpipe_cache_gpu_device_buffer(pipe, output_entry));

  dt_dev_pixelpipe_cache_release_cl_buffer(&cl_mem_blend_output_temp, NULL, NULL, FALSE);
  dt_dev_pixelpipe_cache_release_cl_buffer(&cl_mem_blend_input_temp, NULL, NULL, FALSE);
  dt_dev_pixelpipe_cache_release_cl_buffer(&cl_mem_process_input_temp, NULL, NULL, FALSE);

  dt_opencl_finish(pipe->devid);

  return 0;

error:
  dt_print(DT_DEBUG_OPENCL, "[dev_pixelpipe] %s couldn't process on GPU\n", module->name());

  dt_opencl_finish(pipe->devid);

  dt_dev_pixelpipe_cache_release_cl_buffer(&cl_mem_blend_output_temp, NULL, NULL, FALSE);
  dt_dev_pixelpipe_cache_release_cl_buffer(&cl_mem_blend_input_temp, NULL, NULL, FALSE);
  dt_dev_pixelpipe_cache_release_cl_buffer(&cl_mem_process_input_temp, NULL, NULL, FALSE);

  if(locked_input_entry)
    dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, FALSE, locked_input_entry);

  dt_dev_pixelpipe_cache_release_cl_buffer(&cl_mem_output, output_entry, NULL, FALSE);

  dt_opencl_finish(pipe->devid);

  if(cl_mem_input != NULL)
  {
    if(_gpu_init_input(pipe, &input, &cl_mem_input, piece, tiling,
                       cpu_input_entry, output_entry))
    {
      if(borrowed_cl_mem_input)
      {
        dt_dev_pixelpipe_cache_return_cl_payload(cpu_input_entry, cl_mem_input);
        cl_mem_input = NULL;
        borrowed_cl_mem_input = FALSE;
      }
      else
        dt_dev_pixelpipe_cache_release_cl_buffer(&cl_mem_input, cpu_input_entry, NULL,
                                          dt_dev_pixelpipe_cache_gpu_device_buffer(pipe, cpu_input_entry));
      return 1;
    }
  }
  else if(input != NULL)
  {
    void *cached_pinned_input = dt_dev_pixelpipe_cache_borrow_cl_payload(cpu_input_entry, input, pipe->devid,
                                                         piece->roi_in.width, piece->roi_in.height,
                                                         piece->dsc_in.bpp,
                                                         CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR);
    if(cached_pinned_input != NULL)
    {
      dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, TRUE, cpu_input_entry);
      const int fail = dt_dev_pixelpipe_cache_sync_cl_buffer(pipe->devid, input, cached_pinned_input, &piece->roi_in,
                                              CL_MAP_READ, piece->dsc_in.bpp, module,
                                              "gpu error fallback pinned input copy to cache");
      dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, FALSE, cpu_input_entry);
      dt_dev_pixelpipe_cache_return_cl_payload(cpu_input_entry, cached_pinned_input);

      if(fail)
      {
        dt_print(DT_DEBUG_OPENCL,
                 "[dev_pixelpipe] %s couldn't resync cached pinned input to cache after GPU failure\n",
                 module->name());
        return 1;
      }
    }
  }
  else if(input == NULL)
  {
    dt_print(DT_DEBUG_OPENCL,
             "[dev_pixelpipe] %s CPU fallback has no input buffer (cache allocation failed?)\n",
             module->name());
    return 1;
  }

  if(borrowed_cl_mem_input)
  {
    dt_dev_pixelpipe_cache_return_cl_payload(cpu_input_entry, cl_mem_input);
    cl_mem_input = NULL;
  }
  else
    dt_dev_pixelpipe_cache_release_cl_buffer(&cl_mem_input, cpu_input_entry, input,
                                      dt_dev_pixelpipe_cache_gpu_device_buffer(pipe, cpu_input_entry));

  return pixelpipe_process_on_CPU(pipe, piece, previous_piece, tiling, pixelpipe_flow,
                                  cache_output, cpu_input_entry, output_entry);
}

#else

int pixelpipe_process_on_GPU(dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece,
                             const dt_dev_pixelpipe_iop_t *previous_piece,
                             dt_develop_tiling_t *tiling,
                             dt_pixelpipe_flow_t *pixelpipe_flow,
                             gboolean *const cache_output,
                             dt_pixel_cache_entry_t *input_entry, dt_pixel_cache_entry_t *output_entry)
{
  return pixelpipe_process_on_CPU(pipe, piece, previous_piece, tiling, pixelpipe_flow,
                                  cache_output, input_entry, output_entry);
}

#endif
