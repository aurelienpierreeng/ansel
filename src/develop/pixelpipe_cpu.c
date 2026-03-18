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

int pixelpipe_process_on_CPU(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev,
                             float *input, dt_iop_buffer_dsc_t *input_format, const dt_iop_roi_t *roi_in,
                             void **output, dt_iop_buffer_dsc_t **out_format, const dt_iop_roi_t *roi_out,
                             dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                             dt_develop_tiling_t *tiling, dt_pixelpipe_flow_t *pixelpipe_flow,
                             dt_pixel_cache_entry_t *input_entry, dt_pixel_cache_entry_t *output_entry)
{
  assert(input == dt_pixel_cache_entry_get_data(input_entry));
  gboolean input_rewritten = FALSE;

  if(input == NULL)
  {
    fprintf(stdout, "[dev_pixelpipe] %s got a NULL input, report that to developers\n", module->name());
    return 1;
  }
  if(output && *output == NULL && output_entry)
  {
    *output = dt_pixel_cache_alloc(darktable.pixelpipe_cache, output_entry);
  }

  if(output == NULL || *output == NULL)
  {
    fprintf(stdout, "[dev_pixelpipe] %s got a NULL output, report that to developers\n", module->name());
    return 1;
  }

  const dt_iop_order_iccprofile_info_t *const work_profile
      = (input_format->cst != IOP_CS_RAW) ? dt_ioppr_get_pipe_work_profile_info(pipe) : NULL;

  dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, TRUE, input_entry);
  const int cst_before = input_format->cst;
  dt_ioppr_transform_image_colorspace(module, input, input, roi_in->width, roi_in->height, input_format->cst,
                                      module->input_colorspace(module, pipe, piece), &input_format->cst,
                                      work_profile);
  const int cst_after = input_format->cst;
  dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, FALSE, input_entry);
  input_rewritten = TRUE;

  const size_t in_bpp = dt_iop_buffer_dsc_to_bpp(input_format);
  const size_t bpp = dt_iop_buffer_dsc_to_bpp(*out_format);

  dt_dev_pixelpipe_debug_dump_module_io(pipe, module, "pre", FALSE, input_format, *out_format, roi_in, roi_out,
                                        in_bpp, bpp, cst_before, cst_after);

  if((darktable.unmuted & DT_DEBUG_NAN) && *output && (*out_format)->datatype == TYPE_FLOAT)
  {
    const size_t ch = (*out_format)->channels;
    const size_t count = (size_t)roi_out->width * (size_t)roi_out->height * ch;
    float *out = (float *)(*output);
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(out, count) schedule(static)
#endif
    for(size_t k = 0; k < count; k++)
      out[k] = NAN;
  }

  const gboolean fitting = dt_tiling_piece_fits_host_memory(MAX(roi_in->width, roi_out->width),
                                                            MAX(roi_in->height, roi_out->height), MAX(in_bpp, bpp),
                                                            tiling->factor, tiling->overhead);

  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, TRUE, input_entry);
  int err = 0;
  if(!fitting && piece->process_tiling_ready)
  {
    err = module->process_tiling(module, piece, input, *output, roi_in, roi_out, in_bpp);
    *pixelpipe_flow |= (PIXELPIPE_FLOW_PROCESSED_ON_CPU | PIXELPIPE_FLOW_PROCESSED_WITH_TILING);
    *pixelpipe_flow &= ~(PIXELPIPE_FLOW_PROCESSED_ON_GPU);
  }
  else
  {
    err = module->process(module, piece, input, *output, roi_in, roi_out);
    *pixelpipe_flow |= PIXELPIPE_FLOW_PROCESSED_ON_CPU;
    *pixelpipe_flow &= ~(PIXELPIPE_FLOW_PROCESSED_ON_GPU | PIXELPIPE_FLOW_PROCESSED_WITH_TILING);
  }
  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, FALSE, input_entry);

  if(err)
  {
    fprintf(stdout, "[pixelpipe] %s process on CPU returned with an error\n", module->name());
    return err;
  }

  pipe->dsc.cst = module->output_colorspace(module, pipe, piece);

  if(dt_dev_pixelpipe_transform_for_blend(module, piece))
  {
    dt_iop_colorspace_type_t blend_cst = dt_develop_blend_colorspace(piece, pipe->dsc.cst);
    dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, TRUE, input_entry);
    const int blend_in_before = input_format->cst;
    dt_ioppr_transform_image_colorspace(module, input, input, roi_in->width, roi_in->height,
                                        input_format->cst, blend_cst, &input_format->cst, work_profile);
    const int blend_in_after = input_format->cst;
    dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, FALSE, input_entry);
    input_rewritten = TRUE;

    dt_dev_pixelpipe_debug_dump_module_io(pipe, module, "blend-in", FALSE, input_format, input_format, roi_in,
                                          roi_in, in_bpp, in_bpp, blend_in_before, blend_in_after);

    const int blend_out_before = pipe->dsc.cst;
    dt_ioppr_transform_image_colorspace(module, *output, *output, roi_out->width, roi_out->height,
                                        pipe->dsc.cst, blend_cst, &pipe->dsc.cst, work_profile);
    const int blend_out_after = pipe->dsc.cst;

    dt_dev_pixelpipe_debug_dump_module_io(pipe, module, "blend-out", FALSE, *out_format, &pipe->dsc, roi_out,
                                          roi_out, bpp, bpp, blend_out_before, blend_out_after);
  }

  err = dt_develop_blend_process(module, piece, input, *output, roi_in, roi_out);
  *pixelpipe_flow |= PIXELPIPE_FLOW_BLENDED_ON_CPU;
  *pixelpipe_flow &= ~(PIXELPIPE_FLOW_BLENDED_ON_GPU);

  if(!err && input_rewritten)
    dt_dev_pixelpipe_gpu_flush_host_pinned_images(pipe, input, input_entry, "CPU input rewrite");

  return err;
}
