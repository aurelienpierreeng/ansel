#pragma once

#include "develop/pixelpipe_process.h"

int pixelpipe_process_on_CPU(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev,
                             float *input, dt_iop_buffer_dsc_t *input_format, const dt_iop_roi_t *roi_in,
                             void **output, dt_iop_buffer_dsc_t **out_format, const dt_iop_roi_t *roi_out,
                             dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                             dt_develop_tiling_t *tiling, dt_pixelpipe_flow_t *pixelpipe_flow,
                             dt_pixel_cache_entry_t *input_entry, dt_pixel_cache_entry_t *output_entry);
