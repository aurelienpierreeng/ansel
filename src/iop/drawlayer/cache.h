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

#ifndef DT_IOP_DRAWLAYER_CACHE_H
#define DT_IOP_DRAWLAYER_CACHE_H

#include "develop/imageop.h"
#include "caches/pixelpipe_cache.h"
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

/** @brief Ensure process patch and its stroke mask buffers are allocated. */
gboolean dt_drawlayer_cache_ensure_process_patch_buffer(dt_drawlayer_cache_patch_t *process_patch,
                                                        dt_drawlayer_cache_patch_t *process_stroke_mask,
                                                        int width, int height,
                                                        const char *patch_buffer_name,
                                                        const char *mask_buffer_name);

#endif // DT_IOP_DRAWLAYER_CACHE_H
