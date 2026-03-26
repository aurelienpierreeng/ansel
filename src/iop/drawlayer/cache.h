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

#pragma once

#include "develop/imageop.h"
#include "develop/pixelpipe_cache.h"
#include "iop/drawlayer/paint.h"

#include <glib.h>
#include <stddef.h>
#include <stdint.h>

/** @file
 *  @brief Patch/cache helpers for drawlayer process and preview buffers.
 */

/**
 * @brief Generic float RGBA patch stored either in malloc memory or pixel cache.
 */
typedef struct dt_drawlayer_cache_patch_t
{
  int x;                              /**< Patch origin X in layer coordinates. */
  int y;                              /**< Patch origin Y in layer coordinates. */
  int width;                          /**< Patch width in pixels. */
  int height;                         /**< Patch height in pixels. */
  float *pixels;                      /**< Interleaved RGBA float pixel buffer. */
  dt_pixel_cache_entry_t *cache_entry;/**< Optional shared pixel-cache owner entry. */
  uint64_t cache_hash;                /**< Cache identity hash for `cache_entry`. */
  gboolean external_alloc;            /**< TRUE when `pixels` is externally owned. */
} dt_drawlayer_cache_patch_t;

/** @brief Allocate a temporary RGBA scratch buffer. */
void *dt_drawlayer_cache_alloc_temp_buffer(size_t bytes, const char *name);
/** @brief Release temporary scratch buffer allocated by cache helpers. */
void dt_drawlayer_cache_free_temp_buffer(void **buffer, const char *name);
/** @brief Ensure a float RGBA scratch buffer capacity in pixels. */
float *dt_drawlayer_cache_ensure_scratch_buffer(float **buffer, size_t *capacity_pixels, size_t needed_pixels,
                                                const char *name);

/** @brief Fill RGBA float buffer with transparent black. */
void dt_drawlayer_cache_clear_transparent_float(float *pixels, size_t pixel_count);

/** @brief Drop patch storage and clear metadata. */
void dt_drawlayer_cache_patch_clear(dt_drawlayer_cache_patch_t *patch, const char *external_alloc_name);
/** @brief Allocate/reuse shared patch storage from pixel cache. */
gboolean dt_drawlayer_cache_patch_alloc_shared(dt_drawlayer_cache_patch_t *patch, uint64_t hash, size_t pixel_count,
                                               int width, int height, const char *name, int *created_out);
/** @brief Ensure a float stroke-mask buffer exists for the requested size. */
gboolean dt_drawlayer_cache_ensure_mask_buffer(dt_drawlayer_cache_patch_t *mask, int width, int height,
                                               const char *name);
/** @brief Acquire read lock on shared patch cache entry. */
void dt_drawlayer_cache_patch_rdlock(const dt_drawlayer_cache_patch_t *patch);
/** @brief Release read lock on shared patch cache entry. */
void dt_drawlayer_cache_patch_rdunlock(const dt_drawlayer_cache_patch_t *patch);
/** @brief Acquire write lock on shared patch cache entry. */
void dt_drawlayer_cache_patch_wrlock(const dt_drawlayer_cache_patch_t *patch);
/** @brief Release write lock on shared patch cache entry. */
void dt_drawlayer_cache_patch_wrunlock(const dt_drawlayer_cache_patch_t *patch);

/** @brief Reset process-patch validity/dirty state bookkeeping. */
void dt_drawlayer_cache_invalidate_process_patch_state(gboolean *process_patch_valid, gboolean *process_patch_dirty,
                                                       dt_drawlayer_damaged_rect_t *process_dirty_rect,
                                                       int *process_patch_padding,
                                                       dt_iop_roi_t *process_combined_roi);

/** @brief Ensure process patch and its stroke mask buffers are allocated. */
gboolean dt_drawlayer_cache_ensure_process_patch_buffer(dt_drawlayer_cache_patch_t *process_patch,
                                                        dt_drawlayer_cache_patch_t *process_stroke_mask,
                                                        int width, int height,
                                                        const char *patch_buffer_name,
                                                        const char *mask_buffer_name);

/** @brief Build process+padding ROI in module-buffer coordinates. */
void dt_drawlayer_cache_build_combined_process_roi(const dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *process_roi,
                                                   int current_full_w, int current_full_h, int src_w, int src_h,
                                                   int module_origin_x, int module_origin_y,
                                                   dt_iop_roi_t *combined_roi);
/** @brief Convenience wrapper using piece ROI offsets as module origin. */
void dt_drawlayer_cache_build_combined_process_roi_for_piece(const dt_dev_pixelpipe_iop_t *piece,
                                                             const dt_iop_roi_t *process_roi,
                                                             int current_full_w, int current_full_h,
                                                             int src_w, int src_h,
                                                             dt_iop_roi_t *combined_roi);
/** @brief Resolve effective module input origin, including centered-fit fallback. */
void dt_drawlayer_cache_resolve_piece_input_origin(const dt_dev_pixelpipe_iop_t *piece,
                                                   int current_full_w, int current_full_h,
                                                   int *module_origin_x, int *module_origin_y);

/** @brief Build blend/source ROIs from process patch and output ROI. */
gboolean dt_drawlayer_cache_build_process_blend_rois(const dt_drawlayer_cache_patch_t *process_patch,
                                                     int process_patch_padding, const dt_iop_roi_t *roi_out,
                                                     dt_iop_roi_t *blend_target_roi,
                                                     dt_iop_roi_t *source_process_roi,
                                                     gboolean *direct_copy);

/** @brief Resample process patch into output layer buffer ROI. */
gboolean dt_drawlayer_cache_resample_process_patch_to_output(const dt_drawlayer_cache_patch_t *process_patch,
                                                             int process_patch_padding,
                                                             const dt_iop_roi_t *roi_out,
                                                             float *layerbuf, int layerbuf_width);

/** @brief Populate process patch from base patch with crop/scale and synchronized stroke mask. */
gboolean dt_drawlayer_cache_populate_process_patch_from_base(const dt_drawlayer_cache_patch_t *base_patch,
                                                             const dt_drawlayer_cache_patch_t *base_stroke_mask,
                                                             dt_drawlayer_cache_patch_t *process_patch,
                                                             dt_drawlayer_cache_patch_t *process_stroke_mask,
                                                             const dt_iop_roi_t *combined_roi, int process_pad,
                                                             int patch_width, int patch_height,
                                                             gboolean *process_patch_valid,
                                                             gboolean *process_patch_dirty,
                                                             dt_drawlayer_damaged_rect_t *process_dirty_rect,
                                                             int *process_patch_padding,
                                                             dt_iop_roi_t *process_combined_roi,
                                                             const char *patch_buffer_name,
                                                             const char *mask_buffer_name);

/** @brief Flush dirty process-patch region back into base patch. */
gboolean dt_drawlayer_cache_flush_process_patch_to_base(dt_drawlayer_cache_patch_t *base_patch,
                                                        dt_drawlayer_cache_patch_t *base_stroke_mask,
                                                        const dt_iop_roi_t *process_combined_roi,
                                                        dt_drawlayer_cache_patch_t *process_patch,
                                                        dt_drawlayer_cache_patch_t *process_stroke_mask,
                                                        float **process_update_pixels,
                                                        size_t *process_update_capacity_pixels,
                                                        gboolean *cache_dirty, gboolean *process_patch_dirty,
                                                        dt_drawlayer_damaged_rect_t *process_dirty_rect,
                                                        const char *update_buffer_name);
