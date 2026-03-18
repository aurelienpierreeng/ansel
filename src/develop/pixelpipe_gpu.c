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

#include "develop/pixelpipe_cache_cl.c"

void dt_dev_pixelpipe_gpu_clear_buffer(void **cl_mem_buffer, dt_pixel_cache_entry_t *cache_entry, void *host_ptr,
                                       int cst, gboolean allow_reuse)
{
  _gpu_clear_buffer(cl_mem_buffer, cache_entry, host_ptr, cst, allow_reuse);
}

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

static int _is_opencl_supported(dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece, dt_iop_module_t *module)
{
  return dt_opencl_is_inited() && piece->process_cl_ready && module->process_cl;
}

static int _gpu_init_input(dt_dev_pixelpipe_t *pipe,
                           float **input, void **cl_mem_input,
                           dt_iop_buffer_dsc_t *input_format, dt_iop_colorspace_type_t input_cst_cl,
                           dt_dev_pixelpipe_iop_t *piece, dt_develop_tiling_t *tiling,
                           dt_pixel_cache_entry_t *input_entry, dt_pixel_cache_entry_t *output_entry)
{
  dt_iop_module_t *module = piece->module;
  const dt_iop_roi_t *roi_in = &piece->roi_in;
  const size_t in_bpp = input_format->bpp;

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
  const int fail = _cl_pinned_memory_copy(pipe->devid, *input, *cl_mem_input, roi_in, CL_MAP_READ, in_bpp, module,
                                          "cpu fallback input copy to cache");
  dt_opencl_finish(pipe->devid);
  dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, FALSE, input_entry);

  if(fail)
  {
    dt_print(DT_DEBUG_OPENCL,
             "[dev_pixelpipe] %s couldn't resync GPU input to cache for CPU fallback\n",
             module->name());
    return 1;
  }

  input_format->cst = input_cst_cl;
  return 0;
}

static int _gpu_cpu_fallback_from_opencl_error(dt_dev_pixelpipe_t *pipe, float *input,
                                               void *cl_mem_input,
                                               dt_iop_buffer_dsc_t *input_format,
                                               dt_iop_colorspace_type_t input_cst_cl,
                                               void **cl_mem_output, dt_dev_pixelpipe_iop_t *piece,
                                               dt_develop_tiling_t *tiling, dt_pixelpipe_flow_t *pixelpipe_flow,
                                               dt_pixel_cache_entry_t *input_entry,
                                               dt_pixel_cache_entry_t *output_entry,
                                               dt_pixel_cache_entry_t *locked_input_entry)
{
  dt_iop_module_t *module = piece->module;
  if(locked_input_entry)
    dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, FALSE, locked_input_entry);

  if(cl_mem_input != NULL)
  {
    if(_gpu_init_input(pipe, &input, &cl_mem_input, input_format, input_cst_cl, piece, tiling,
                       input_entry, output_entry))
      return 1;
  }
  else if(input == NULL)
  {
    dt_print(DT_DEBUG_OPENCL,
             "[dev_pixelpipe] %s CPU fallback has no input buffer (cache allocation failed?)\n",
             module->name());
    dt_dev_pixelpipe_gpu_clear_buffer(cl_mem_output, output_entry, NULL, IOP_CS_NONE,
                                      dt_dev_pixelpipe_cache_gpu_device_buffer(pipe, output_entry));
    return 1;
  }

  dt_dev_pixelpipe_gpu_clear_buffer(cl_mem_output, output_entry, NULL, IOP_CS_NONE,
                                    dt_dev_pixelpipe_cache_gpu_device_buffer(pipe, output_entry));
  dt_dev_pixelpipe_gpu_clear_buffer(&cl_mem_input, input_entry, NULL, IOP_CS_NONE,
                                    dt_dev_pixelpipe_cache_gpu_device_buffer(pipe, input_entry));

  return pixelpipe_process_on_CPU(pipe, piece, tiling, pixelpipe_flow, input_entry, output_entry);
}

static int _gpu_early_cpu_fallback_if_unsupported(dt_dev_pixelpipe_t *pipe, float **input,
                                                  void **cl_mem_input,
                                                  dt_iop_buffer_dsc_t *input_format, dt_dev_pixelpipe_iop_t *piece,
                                                  dt_develop_tiling_t *tiling,
                                                  dt_pixelpipe_flow_t *pixelpipe_flow,
                                                  dt_pixel_cache_entry_t *input_entry,
                                                  dt_pixel_cache_entry_t *output_entry)
{
  dt_iop_module_t *module = piece->module;
  const dt_iop_roi_t *roi_in = &piece->roi_in;
  const dt_iop_colorspace_type_t input_cst_cl = input_format->cst;
  const size_t in_bpp = input_format->bpp;

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
      dt_dev_pixelpipe_gpu_clear_buffer(cl_mem_input, input_entry, NULL, input_cst_cl,
                                        dt_dev_pixelpipe_cache_gpu_device_buffer(pipe, input_entry));
      return 1;
    }

    *input = _resync_input_gpu_to_cache(pipe, *input, *cl_mem_input, input_format, roi_in, module, input_cst_cl,
                                        in_bpp, input_entry, "cpu fallback input copy to cache");
  }
  else if(!input || *input == NULL)
  {
    dt_print(DT_DEBUG_OPENCL,
             "[dev_pixelpipe] %s CPU fallback has no input buffer (cache allocation failed?)\n",
             module->name());
    return 1;
  }

  dt_dev_pixelpipe_gpu_clear_buffer(cl_mem_input, input_entry, *input, input_cst_cl,
                                    dt_dev_pixelpipe_cache_gpu_device_buffer(pipe, input_entry));

  return pixelpipe_process_on_CPU(pipe, piece, tiling, pixelpipe_flow, input_entry, output_entry);
}

int pixelpipe_process_on_GPU(dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                             dt_develop_tiling_t *tiling,
                             dt_pixelpipe_flow_t *pixelpipe_flow,
                             dt_pixel_cache_entry_t *input_entry, dt_pixel_cache_entry_t *output_entry)
{
  dt_iop_module_t *module = piece->module;
  const dt_iop_roi_t *roi_in = &piece->roi_in;
  const dt_iop_roi_t *roi_out = &piece->roi_out;
  float *input = dt_pixel_cache_entry_get_data(input_entry);
  void *output = dt_pixel_cache_entry_get_data(output_entry);
  dt_iop_buffer_dsc_t *input_format = &input_entry->dsc;
  dt_iop_buffer_dsc_t *out_format = &output_entry->dsc;
  void *cl_mem_input = NULL;
  void *cl_mem_output = NULL;
  const size_t in_bpp = input_format->bpp;
  const size_t bpp = out_format->bpp;
  dt_iop_colorspace_type_t input_cst_cl = input_format->cst;
  dt_pixel_cache_entry_t *cpu_input_entry = input_entry;
  dt_pixel_cache_entry_t *locked_input_entry = NULL;
  gboolean input_rewritten_on_host = FALSE;

  dt_dev_pixelpipe_cache_peek(darktable.pixelpipe_cache, input_entry->hash, NULL, NULL, NULL, roi_in, in_bpp,
                              pipe->devid, &cl_mem_input);

  if(input == NULL && cl_mem_input == NULL)
  {
    dt_print(DT_DEBUG_OPENCL, "[dev_pixelpipe] %s has no RAM nor vRAM input... aborting.\n", module->name());
    return 1;
  }

  if(!_is_opencl_supported(pipe, piece, module) || !pipe->opencl_enabled || !(pipe->devid >= 0))
  {
    return _gpu_early_cpu_fallback_if_unsupported(pipe, &input, &cl_mem_input, input_format, piece, tiling,
                                                  pixelpipe_flow, input_entry, output_entry);
  }

  const dt_iop_order_iccprofile_info_t *const work_profile
      = (input_format->cst != IOP_CS_RAW) ? dt_ioppr_get_pipe_work_profile_info(pipe) : NULL;

  const float required_factor_cl
      = fmaxf(1.0f, (cl_mem_input != NULL) ? tiling->factor_cl - 1.0f : tiling->factor_cl);

  const size_t precheck_width = ROUNDUPDWD(MAX(roi_in->width, roi_out->width), pipe->devid);
  const size_t precheck_height = ROUNDUPDHT(MAX(roi_in->height, roi_out->height), pipe->devid);
  gboolean fits_on_device = dt_opencl_image_fits_device(pipe->devid, precheck_width, precheck_height,
                                                        MAX(in_bpp, bpp), required_factor_cl, tiling->overhead);
  if(!fits_on_device)
  {
    dt_print(DT_DEBUG_OPENCL,
             "[dev_pixelpipe] %s pre-check didn't fit on device, flushing cached pinned buffers and retrying\n",
             module->name());
    dt_dev_pixelpipe_cache_flush_clmem(darktable.pixelpipe_cache, pipe->devid, cl_mem_input);
    fits_on_device = dt_opencl_image_fits_device(pipe->devid, precheck_width, precheck_height,
                                                 MAX(in_bpp, bpp), required_factor_cl, tiling->overhead);
  }

  gboolean possible_cl = !(pipe->type == DT_DEV_PIXELPIPE_PREVIEW
                           && (module->flags() & IOP_FLAGS_PREVIEW_NON_OPENCL))
                         && (fits_on_device || piece->process_tiling_ready);

  if(!possible_cl || !fits_on_device) piece->force_opencl_cache = TRUE;
  if(piece->force_opencl_cache && output == NULL)
  {
    output = dt_pixel_cache_alloc(darktable.pixelpipe_cache, output_entry);
    if(output == NULL) goto error;
  }

  if(possible_cl && !fits_on_device)
  {
    const float cl_px = dt_opencl_get_device_available(pipe->devid)
                        / (sizeof(float) * MAX(in_bpp, bpp) * ceilf(required_factor_cl));
    const float dx = MAX(roi_in->width, roi_out->width);
    const float dy = MAX(roi_in->height, roi_out->height);
    const float border = tiling->overlap + 1;
    const gboolean possible = (cl_px > dx * border) || (cl_px > dy * border) || (cl_px > border * border);
    if(!possible)
    {
      dt_print(DT_DEBUG_OPENCL | DT_DEBUG_TILING,
               "[dt_dev_pixelpipe_process_rec] CL: tiling impossible in module `%s'. avail=%.1fM, requ=%.1fM (%ix%i). overlap=%i\n",
               module->name(), cl_px / 1e6f, dx * dy / 1e6f, (int)dx, (int)dy, (int)tiling->overlap);
      goto error;
    }

    if(_gpu_init_input(pipe, &input, &cl_mem_input, input_format, input_cst_cl, piece, tiling,
                       input_entry, output_entry))
      goto error;
  }

  if(!possible_cl) goto error;

  if(fits_on_device)
  {
    if(_gpu_prepare_cl_input(pipe, module, input, &cl_mem_input, &input_cst_cl, roi_in, in_bpp, input_entry,
                             &locked_input_entry, NULL))
      goto error;

    if(cl_mem_output == NULL)
    {
      const gboolean reuse_output_cacheline = _requests_cache(pipe, piece)
                                              && (pipe->realtime || !piece->force_opencl_cache);
      const gboolean reuse_output_pinned = reuse_output_cacheline;
      cl_mem_output = _gpu_init_buffer(pipe->devid, output, roi_out, bpp, module, "output", output_entry,
                                        reuse_output_pinned, reuse_output_cacheline, &out_format->cst, NULL,
                                        cl_mem_input);
      if(cl_mem_output == NULL) goto error;
    }

    const int cst_before_cl = input_cst_cl;
    if(!dt_ioppr_transform_image_colorspace_cl(module, piece->pipe->devid, cl_mem_input, cl_mem_input,
                                               roi_in->width, roi_in->height, input_cst_cl,
                                               module->input_colorspace(module, pipe, piece), &input_cst_cl,
                                               work_profile))
      goto error;
    const int cst_after_cl = input_cst_cl;

    dt_dev_pixelpipe_debug_dump_module_io(pipe, module, "pre", TRUE, input_format, out_format, roi_in, roi_out,
                                          in_bpp, bpp, cst_before_cl, cst_after_cl);

    if(!module->process_cl(module, piece, cl_mem_input, cl_mem_output, roi_in, roi_out))
      goto error;

    *pixelpipe_flow |= PIXELPIPE_FLOW_PROCESSED_ON_GPU;
    *pixelpipe_flow &= ~(PIXELPIPE_FLOW_PROCESSED_ON_CPU | PIXELPIPE_FLOW_PROCESSED_WITH_TILING);

    pipe->dsc.cst = module->output_colorspace(module, pipe, piece);

    if(dt_dev_pixelpipe_transform_for_blend(module, piece))
    {
      dt_iop_colorspace_type_t blend_cst = dt_develop_blend_colorspace(piece, pipe->dsc.cst);
      int success = 1;
      const int blend_in_before = input_cst_cl;
      success &= dt_ioppr_transform_image_colorspace_cl(module, piece->pipe->devid, cl_mem_input, cl_mem_input,
                                                        roi_in->width, roi_in->height, input_cst_cl, blend_cst,
                                                        &input_cst_cl, work_profile);
      const int blend_in_after = input_cst_cl;
      dt_dev_pixelpipe_debug_dump_module_io(pipe, module, "blend-in", TRUE, input_format, input_format, roi_in,
                                            roi_in, in_bpp, in_bpp, blend_in_before, blend_in_after);
      const int blend_out_before = pipe->dsc.cst;
      success &= dt_ioppr_transform_image_colorspace_cl(module, piece->pipe->devid, cl_mem_output,
                                                        cl_mem_output, roi_out->width, roi_out->height,
                                                        pipe->dsc.cst, blend_cst, &pipe->dsc.cst, work_profile);
      const int blend_out_after = pipe->dsc.cst;
      dt_dev_pixelpipe_debug_dump_module_io(pipe, module, "blend-out", TRUE, out_format, &pipe->dsc, roi_out,
                                            roi_out, bpp, bpp, blend_out_before, blend_out_after);

      if(!success)
      {
        dt_print(DT_DEBUG_OPENCL, "[opencl_pixelpipe] couldn't transform blending colorspace for module %s\n",
                 module->name());
        goto error;
      }
    }

    if(dt_develop_blend_process_cl(module, piece, cl_mem_input, cl_mem_output, roi_in, roi_out))
      goto error;

    *pixelpipe_flow |= PIXELPIPE_FLOW_BLENDED_ON_GPU;
    *pixelpipe_flow &= ~(PIXELPIPE_FLOW_BLENDED_ON_CPU);

    if(piece->force_opencl_cache)
    {
      if(_cl_pinned_memory_copy(pipe->devid, output, cl_mem_output, roi_out, CL_MAP_READ, bpp, module,
                                "output to cache"))
        goto error;
      dt_print(DT_DEBUG_OPENCL, "[dev_pixelpipe] output memory was copied to cache for %s\n", module->name());
    }
  }
  else if(piece->process_tiling_ready && input != NULL)
  {
    dt_dev_pixelpipe_gpu_clear_buffer(&cl_mem_input, input_entry, input, input_cst_cl,
                                      dt_dev_pixelpipe_cache_gpu_device_buffer(pipe, input_entry));

    dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, TRUE, input_entry);
    dt_ioppr_transform_image_colorspace(module, input, input, roi_in->width, roi_in->height, input_format->cst,
                                        module->input_colorspace(module, pipe, piece), &input_format->cst,
                                        work_profile);
    dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, FALSE, input_entry);
    input_rewritten_on_host = TRUE;

    dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, TRUE, input_entry);
    int fail = !module->process_tiling_cl(module, piece, input, output, roi_in, roi_out, in_bpp);
    dt_opencl_finish(pipe->devid);
    dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, FALSE, input_entry);

    if(fail) goto error;

    *pixelpipe_flow |= (PIXELPIPE_FLOW_PROCESSED_ON_GPU | PIXELPIPE_FLOW_PROCESSED_WITH_TILING);
    *pixelpipe_flow &= ~(PIXELPIPE_FLOW_PROCESSED_ON_CPU);

    pipe->dsc.cst = module->output_colorspace(module, pipe, piece);

    if(dt_dev_pixelpipe_transform_for_blend(module, piece))
    {
      dt_iop_colorspace_type_t blend_cst = dt_develop_blend_colorspace(piece, pipe->dsc.cst);

      dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, TRUE, input_entry);
      dt_ioppr_transform_image_colorspace(module, input, input, roi_in->width, roi_in->height, input_format->cst,
                                          blend_cst, &input_format->cst, work_profile);
      dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, FALSE, input_entry);
      input_rewritten_on_host = TRUE;

      dt_ioppr_transform_image_colorspace(module, output, output, roi_out->width, roi_out->height, pipe->dsc.cst,
                                          blend_cst, &pipe->dsc.cst, work_profile);
    }

    dt_develop_blend_process(module, piece, input, output, roi_in, roi_out);
    *pixelpipe_flow |= PIXELPIPE_FLOW_BLENDED_ON_CPU;
    *pixelpipe_flow &= ~(PIXELPIPE_FLOW_BLENDED_ON_GPU);
  }
  else
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_pixelpipe] could not run module '%s' on gpu. falling back to cpu path\n",
             module->name());
    goto error;
  }

  dt_dev_pixelpipe_gpu_clear_buffer(&cl_mem_input, input_entry, input, input_cst_cl,
                                    dt_dev_pixelpipe_cache_gpu_device_buffer(pipe, input_entry));

  if(input_rewritten_on_host)
    dt_dev_pixelpipe_gpu_flush_host_pinned_images(pipe, input, input_entry, "host-side GPU tiling input rewrite");

  dt_opencl_finish(pipe->devid);
  if(locked_input_entry)
    dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, FALSE, locked_input_entry);

  return 0;

error:
  dt_print(DT_DEBUG_OPENCL, "[dev_pixelpipe] %s couldn't process on GPU\n", module->name());
  piece->force_opencl_cache = TRUE;

  return _gpu_cpu_fallback_from_opencl_error(pipe, input, cl_mem_input, input_format, input_cst_cl,
                                             &cl_mem_output, piece, tiling,
                                             pixelpipe_flow, cpu_input_entry, output_entry,
                                             locked_input_entry);
}

#else

int pixelpipe_process_on_GPU(dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                             dt_develop_tiling_t *tiling,
                             dt_pixelpipe_flow_t *pixelpipe_flow,
                             dt_pixel_cache_entry_t *input_entry, dt_pixel_cache_entry_t *output_entry)
{
  return pixelpipe_process_on_CPU(pipe, piece, tiling, pixelpipe_flow, input_entry, output_entry);
}

#endif
