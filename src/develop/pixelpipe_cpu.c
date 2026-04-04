/*
    Private CPU pixelpipe backend.
*/

#include "common/darktable.h"
#include "common/iop_order.h"
#include "develop/blend.h"
#include "develop/pixelpipe_cpu.h"
#include "develop/pixelpipe_gpu.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

int pixelpipe_process_on_CPU(dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece,
                             const dt_dev_pixelpipe_iop_t *previous_piece,
                             dt_develop_tiling_t *tiling, dt_pixelpipe_flow_t *pixelpipe_flow,
                             gboolean *const cache_output,
                             dt_pixel_cache_entry_t *input_entry, dt_pixel_cache_entry_t *output_entry)
{
  dt_iop_module_t *module = piece->module;
  float *input = input_entry ? dt_pixel_cache_entry_get_data(input_entry) : NULL;
  void *output = dt_pixel_cache_entry_get_data(output_entry);
  float *process_input_temp = NULL;
  float *blend_input_temp = NULL;
  float *blend_output_temp = NULL;
  const float *process_input = input;
  const float *blend_input = input;
  void *blend_output = output;
  const dt_iop_buffer_dsc_t actual_input_dsc = previous_piece ? previous_piece->dsc_out : pipe->dev->image_storage.dsc;
  dt_iop_buffer_dsc_t process_input_dsc = actual_input_dsc;
  dt_iop_buffer_dsc_t blend_input_dsc = actual_input_dsc;
  dt_iop_buffer_dsc_t blend_output_dsc = piece->dsc_out;
  gboolean input_locked = FALSE;

  if(IS_NULL_PTR(input) && !(module->flags() & IOP_FLAGS_TAKE_NO_INPUT))
  {
    fprintf(stdout, "[dev_pixelpipe] %s got a NULL input, report that to developers\n", module->name());
    return 1;
  }
  if(IS_NULL_PTR(output))
    output = dt_pixel_cache_alloc(darktable.pixelpipe_cache, output_entry);

  if(IS_NULL_PTR(output))
  {
    fprintf(stdout, "[dev_pixelpipe] %s got a NULL output, report that to developers\n", module->name());
    return 1;
  }

  const dt_iop_order_iccprofile_info_t *const work_profile
      = (process_input_dsc.cst != IOP_CS_RAW || piece->dsc_in.cst != IOP_CS_RAW)
            ? dt_ioppr_get_pipe_work_profile_info(pipe)
            : NULL;

  const int cst_before = process_input_dsc.cst;
  if(process_input_dsc.cst != piece->dsc_in.cst
     && !(dt_iop_colorspace_is_rgb(process_input_dsc.cst) && dt_iop_colorspace_is_rgb(piece->dsc_in.cst)))
  {
    process_input_temp
        = dt_pixelpipe_cache_alloc_align_float((size_t)piece->roi_in.width * piece->roi_in.height * 4, pipe);
    if(IS_NULL_PTR(process_input_temp))
      return 1;

    dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, TRUE, input_entry);
    input_locked = TRUE;
    dt_ioppr_transform_image_colorspace(module, input, process_input_temp, piece->roi_in.width,
                                        piece->roi_in.height, process_input_dsc.cst, piece->dsc_in.cst,
                                        &process_input_dsc.cst, work_profile);
    dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, FALSE, input_entry);
    input_locked = FALSE;
    process_input = process_input_temp;
  }
  else if(process_input_dsc.cst != piece->dsc_in.cst)
  {
    process_input_dsc.cst = piece->dsc_in.cst;
    if(input_entry)
    {
      dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, TRUE, input_entry);
      input_locked = TRUE;
    }
  }
  else
  {
    if(input_entry)
    {
      dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, TRUE, input_entry);
      input_locked = TRUE;
    }
  }
  const int cst_after = process_input_dsc.cst;

  dt_dev_pixelpipe_debug_dump_module_io(pipe, module, "pre", FALSE, &piece->dsc_in, &piece->dsc_out,
                                        &piece->roi_in, &piece->roi_out,
                                        process_input_dsc.bpp, piece->dsc_out.bpp, cst_before, cst_after);

  if((darktable.unmuted & DT_DEBUG_NAN) && !IS_NULL_PTR(output) && piece->dsc_out.datatype == TYPE_FLOAT)
  {
    const size_t ch = piece->dsc_out.channels;
    const size_t count = (size_t)piece->roi_out.width * (size_t)piece->roi_out.height * ch;
    float *out = (float *)output;
    __OMP_PARALLEL_FOR_SIMD__()
    for(size_t k = 0; k < count; k++)
      out[k] = NAN;
  }

  const gboolean fitting = dt_tiling_piece_fits_host_memory(MAX(piece->roi_in.width, piece->roi_out.width),
                                                            MAX(piece->roi_in.height, piece->roi_out.height),
                                                            MAX(process_input_dsc.bpp, piece->dsc_out.bpp),
                                                            tiling->factor, tiling->overhead);

  int err = 0;
  if(!fitting && piece->process_tiling_ready)
  {
    err = module->process_tiling(module, pipe, piece, process_input, output, process_input_dsc.bpp);
    *pixelpipe_flow |= (PIXELPIPE_FLOW_PROCESSED_ON_CPU | PIXELPIPE_FLOW_PROCESSED_WITH_TILING);
    *pixelpipe_flow &= ~(PIXELPIPE_FLOW_PROCESSED_ON_GPU);
  }
  else
  {
    err = module->process(module, pipe, piece, process_input, output);
    *pixelpipe_flow |= PIXELPIPE_FLOW_PROCESSED_ON_CPU;
    *pixelpipe_flow &= ~(PIXELPIPE_FLOW_PROCESSED_ON_GPU | PIXELPIPE_FLOW_PROCESSED_WITH_TILING);
  }

  if(err)
  {
    fprintf(stdout, "[pixelpipe] %s process on CPU returned with an error\n", module->name());
    if(input_locked)
      dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, FALSE, input_entry);
    dt_pixelpipe_cache_free_align(process_input_temp);
    return err;
  }

  if(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
  {
    blend_input = process_input;
    blend_input_dsc = process_input_dsc;
    blend_output = output;
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
      const int blend_in_before = blend_input_dsc.cst;
      if(blend_transforms & DT_DEV_PIXELPIPE_BLEND_TRANSFORM_INPUT)
      {
        blend_input_temp
            = dt_pixelpipe_cache_alloc_align_float((size_t)piece->roi_in.width * piece->roi_in.height * 4, pipe);
        if(IS_NULL_PTR(blend_input_temp))
        {
          if(input_locked)
            dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, FALSE, input_entry);
          dt_pixelpipe_cache_free_align(process_input_temp);
          return 1;
        }

        dt_ioppr_transform_image_colorspace(module, process_input, blend_input_temp, piece->roi_in.width,
                                            piece->roi_in.height, blend_input_dsc.cst, blend_cst,
                                            &blend_input_dsc.cst, work_profile);
        blend_input = blend_input_temp;
        if(input_locked)
        {
          dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, FALSE, input_entry);
          input_locked = FALSE;
        }
      }
      const int blend_in_after = blend_input_dsc.cst;

      dt_dev_pixelpipe_debug_dump_module_io(pipe, module, "blend-in", FALSE, &process_input_dsc, &blend_input_dsc,
                                            &piece->roi_in, &piece->roi_in,
                                            process_input_dsc.bpp, blend_input_dsc.bpp,
                                            blend_in_before, blend_in_after);

      const int blend_out_before = blend_output_dsc.cst;
      if(blend_transforms & DT_DEV_PIXELPIPE_BLEND_TRANSFORM_OUTPUT)
      {
        blend_output_temp
            = dt_pixelpipe_cache_alloc_align_float((size_t)piece->roi_out.width * piece->roi_out.height * 4, pipe);
        if(IS_NULL_PTR(blend_output_temp))
        {
          if(input_locked)
            dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, FALSE, input_entry);
          dt_pixelpipe_cache_free_align(blend_input_temp);
          dt_pixelpipe_cache_free_align(process_input_temp);
          return 1;
        }

        dt_ioppr_transform_image_colorspace(module, output, blend_output_temp, piece->roi_out.width,
                                            piece->roi_out.height, blend_output_dsc.cst, blend_cst,
                                            &blend_output_dsc.cst, work_profile);
        blend_output = blend_output_temp;
      }
      const int blend_out_after = blend_output_dsc.cst;

      dt_dev_pixelpipe_debug_dump_module_io(pipe, module, "blend-out", FALSE, &piece->dsc_out, &blend_output_dsc,
                                            &piece->roi_out, &piece->roi_out,
                                            piece->dsc_out.bpp, blend_output_dsc.bpp,
                                            blend_out_before, blend_out_after);
    }

    err = dt_develop_blend_process(module, pipe, piece, blend_input, blend_output);
    *pixelpipe_flow |= PIXELPIPE_FLOW_BLENDED_ON_CPU;
    *pixelpipe_flow &= ~(PIXELPIPE_FLOW_BLENDED_ON_GPU);

    if(!err && (blend_transforms & DT_DEV_PIXELPIPE_BLEND_TRANSFORM_OUTPUT))
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
  }

  if(input_locked)
    dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, FALSE, input_entry);
  dt_pixelpipe_cache_free_align(blend_output_temp);
  dt_pixelpipe_cache_free_align(blend_input_temp);
  dt_pixelpipe_cache_free_align(process_input_temp);

  return err;
}
