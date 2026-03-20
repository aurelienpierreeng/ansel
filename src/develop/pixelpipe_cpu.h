#pragma once

#include "develop/pixelpipe_process.h"

int pixelpipe_process_on_CPU(dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece,
                             const dt_dev_pixelpipe_iop_t *previous_piece,
                             dt_develop_tiling_t *tiling, dt_pixelpipe_flow_t *pixelpipe_flow,
                             gboolean *cache_output,
                             dt_pixel_cache_entry_t *input_entry, dt_pixel_cache_entry_t *output_entry);
