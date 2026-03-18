#pragma once

#include "develop/pixelpipe_process.h"

void dt_dev_pixelpipe_gpu_flush_host_pinned_images(dt_dev_pixelpipe_t *pipe, void *host_ptr,
                                                   dt_pixel_cache_entry_t *cache_entry, const char *reason);

int pixelpipe_process_on_GPU(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev,
                             float *input, void *cl_mem_input, dt_iop_buffer_dsc_t *input_format,
                             const dt_iop_roi_t *roi_in,
                             void **output, void **cl_mem_output, dt_iop_buffer_dsc_t **out_format,
                             const dt_iop_roi_t *roi_out,
                             dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                             dt_develop_tiling_t *tiling, dt_pixelpipe_flow_t *pixelpipe_flow,
                             size_t in_bpp, size_t bpp,
                             dt_pixel_cache_entry_t *input_entry, dt_pixel_cache_entry_t *output_entry);
