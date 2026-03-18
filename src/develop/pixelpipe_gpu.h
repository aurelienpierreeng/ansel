#pragma once

#include "develop/pixelpipe_process.h"

void dt_dev_pixelpipe_gpu_flush_host_pinned_images(dt_dev_pixelpipe_t *pipe, void *host_ptr,
                                                   dt_pixel_cache_entry_t *cache_entry, const char *reason);

int pixelpipe_process_on_GPU(dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                             dt_develop_tiling_t *tiling,
                             dt_pixelpipe_flow_t *pixelpipe_flow,
                             dt_pixel_cache_entry_t *input_entry, dt_pixel_cache_entry_t *output_entry);
