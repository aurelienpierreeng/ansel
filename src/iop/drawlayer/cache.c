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

#include "system/macros.h"
#include "system/mem_alloc.h"
#include "iop/drawlayer/cache.h"


#include <string.h>

/** @brief Allocate temporary aligned cache buffer. */
void *dt_drawlayer_cache_alloc_temp_buffer(const size_t bytes, const char *name)
{
  if(bytes == 0) return NULL;
  return dt_pixelpipe_cache_alloc_align_cache_impl(bytes, DT_DEV_PIXELPIPE_NONE, name);
}

/** @brief Free temporary aligned cache buffer. */
void dt_drawlayer_cache_free_temp_buffer(void **buffer, const char *name)
{
  if(IS_NULL_PTR(buffer) || !*buffer) return;
  dt_pixelpipe_cache_free_align_cache(buffer, name);
}

/** @brief Ensure scratch RGBA float capacity in pixels. */
float *dt_drawlayer_cache_ensure_scratch_buffer(float **buffer, size_t *capacity_pixels, const size_t needed_pixels,
                                                const char *name)
{
  if(IS_NULL_PTR(buffer) || IS_NULL_PTR(capacity_pixels) || needed_pixels == 0) return NULL;
  if(*capacity_pixels < needed_pixels)
  {
    dt_drawlayer_cache_free_temp_buffer((void **)buffer, name);
    float *new_buffer = dt_drawlayer_cache_alloc_temp_buffer(needed_pixels * 4 * sizeof(float), name);
    if(IS_NULL_PTR(new_buffer)) return NULL;
    *buffer = new_buffer;
    *capacity_pixels = needed_pixels;
  }
  return *buffer;
}

/** @brief Fill float RGBA buffer with transparent black. */
void dt_drawlayer_cache_clear_transparent_float(float *pixels, const size_t pixel_count)
{
  if(IS_NULL_PTR(pixels)) return;
  memset(pixels, 0, pixel_count * 4 * sizeof(float));
}

/** @brief Release patch storage and reset patch metadata. */
void dt_drawlayer_cache_patch_clear(dt_drawlayer_cache_patch_t *patch, const char *external_alloc_name)
{
  if(IS_NULL_PTR(patch)) return;
  if(patch->external_alloc)
  {
    if(patch->cache_entry)
      dt_dev_pixelpipe_cache_ref_count_entry(FALSE, patch->cache_entry);
    void *buffer = patch->pixels;
    dt_pixelpipe_cache_free_align_cache(&buffer, external_alloc_name);
  }
  else if(patch->cache_entry)
  {
    dt_dev_pixelpipe_cache_ref_count_entry(FALSE, patch->cache_entry);
  }
  else
  {
    dt_free(patch->pixels);
  }
  memset(patch, 0, sizeof(*patch));
}

/** @brief Allocate patch storage from shared pixel cache entry. */
gboolean dt_drawlayer_cache_patch_alloc_shared(dt_drawlayer_cache_patch_t *patch, const uint64_t hash,
                                               const size_t pixel_count, const int width, const int height,
                                               const char *name, int *created_out)
{
  if(IS_NULL_PTR(patch) || pixel_count == 0 || width <= 0 || height <= 0) return FALSE;

  void *data = NULL;
  dt_pixel_cache_entry_t *entry = NULL;
  const int created = dt_dev_pixelpipe_cache_get(hash, pixel_count * 4 * sizeof(float),
                                                 name, DT_DEV_PIXELPIPE_NONE, TRUE, &data, &entry);
  if(!IS_NULL_PTR(created_out)) *created_out = created;
  if(IS_NULL_PTR(data) || IS_NULL_PTR(entry))
  {
    if(entry)
    {
      if(created) dt_dev_pixelpipe_cache_wrlock_entry(FALSE, entry);
      dt_dev_pixelpipe_cache_ref_count_entry(FALSE, entry);
    }
    return FALSE;
  }

  dt_drawlayer_cache_patch_clear(patch, "drawlayer patch");
  patch->x = 0;
  patch->y = 0;
  patch->width = width;
  patch->height = height;
  patch->pixels = (float *)data;
  patch->cache_entry = entry;
  patch->cache_hash = hash;
  patch->external_alloc = FALSE;

  if(created) dt_dev_pixelpipe_cache_wrlock_entry(FALSE, entry);
  return TRUE;
}

/** @brief Ensure a float stroke-mask buffer exists and matches the requested size. */
gboolean dt_drawlayer_cache_ensure_mask_buffer(dt_drawlayer_cache_patch_t *mask, const int width,
                                               const int height, const char *name)
{
  if(IS_NULL_PTR(mask) || width <= 0 || height <= 0) return FALSE;

  const gboolean size_changed = (mask->width != width || mask->height != height || !mask->pixels);
  if(size_changed)
  {
    dt_drawlayer_cache_patch_clear(mask, name);
    mask->width = width;
    mask->height = height;
    mask->x = 0;
    mask->y = 0;
    mask->pixels = dt_drawlayer_cache_alloc_temp_buffer((size_t)width * height * sizeof(float), name);
    mask->external_alloc = TRUE;
    if(IS_NULL_PTR(mask->pixels))
    {
      mask->width = 0;
      mask->height = 0;
      return FALSE;
    }

    mask->cache_entry = dt_dev_pixelpipe_cache_ref_entry_for_host_ptr(mask->pixels);
    mask->cache_hash = mask->cache_entry ? mask->cache_entry->hash : DT_PIXELPIPE_CACHE_HASH_INVALID;
    if(IS_NULL_PTR(mask->cache_entry))
    {
      dt_drawlayer_cache_patch_clear(mask, name);
      return FALSE;
    }
  }

  memset(mask->pixels, 0, (size_t)width * height * sizeof(float));
  return TRUE;
}

/** @brief Acquire read lock on shared patch cache entry. */
void dt_drawlayer_cache_patch_rdlock(const dt_drawlayer_cache_patch_t *patch)
{
  if(IS_NULL_PTR(patch) || IS_NULL_PTR(patch->cache_entry)) return;
  dt_dev_pixelpipe_cache_rdlock_entry(TRUE, patch->cache_entry);
}

/** @brief Release read lock on shared patch cache entry. */
void dt_drawlayer_cache_patch_rdunlock(const dt_drawlayer_cache_patch_t *patch)
{
  if(IS_NULL_PTR(patch) || IS_NULL_PTR(patch->cache_entry)) return;
  dt_dev_pixelpipe_cache_rdlock_entry(FALSE, patch->cache_entry);
}

/** @brief Acquire write lock on shared patch cache entry. */
void dt_drawlayer_cache_patch_wrlock(const dt_drawlayer_cache_patch_t *patch)
{
  if(IS_NULL_PTR(patch) || IS_NULL_PTR(patch->cache_entry)) return;
  dt_dev_pixelpipe_cache_wrlock_entry(TRUE, patch->cache_entry);
}

/** @brief Release write lock on shared patch cache entry. */
void dt_drawlayer_cache_patch_wrunlock(const dt_drawlayer_cache_patch_t *patch)
{
  if(IS_NULL_PTR(patch) || IS_NULL_PTR(patch->cache_entry)) return;
  dt_dev_pixelpipe_cache_wrlock_entry(FALSE, patch->cache_entry);
}

/** @brief Ensure process patch and process-stroke-mask backing buffers exist. */
gboolean dt_drawlayer_cache_ensure_process_patch_buffer(dt_drawlayer_cache_patch_t *process_patch,
                                                        dt_drawlayer_cache_patch_t *process_stroke_mask,
                                                        const int width,
                                                        const int height, const char *patch_buffer_name,
                                                        const char *mask_buffer_name)
{
  if(IS_NULL_PTR(process_patch) || !process_stroke_mask || width <= 0 || height <= 0)
    return FALSE;

  const gboolean size_changed = (process_patch->width != width || process_patch->height != height
                                 || !process_patch->pixels);
  if(size_changed)
  {
    dt_drawlayer_cache_patch_clear(process_patch, patch_buffer_name);
    process_patch->width = width;
    process_patch->height = height;
    process_patch->x = 0;
    process_patch->y = 0;
    process_patch->pixels = dt_drawlayer_cache_alloc_temp_buffer((gsize)width * height * 4 * sizeof(float),
                                                                 patch_buffer_name);
    process_patch->external_alloc = TRUE;
    if(IS_NULL_PTR(process_patch->pixels)) return FALSE;
    process_patch->cache_entry
        = dt_dev_pixelpipe_cache_ref_entry_for_host_ptr(process_patch->pixels);
    process_patch->cache_hash = process_patch->cache_entry ? process_patch->cache_entry->hash
                                                           : DT_PIXELPIPE_CACHE_HASH_INVALID;
    if(IS_NULL_PTR(process_patch->cache_entry)) return FALSE;
  }

  const size_t mask_count = (size_t)width * height;
  if(size_changed || process_stroke_mask->width != width || process_stroke_mask->height != height
     || !process_stroke_mask->pixels)
  {
    dt_drawlayer_cache_patch_clear(process_stroke_mask, mask_buffer_name);
    process_stroke_mask->width = width;
    process_stroke_mask->height = height;
    process_stroke_mask->x = 0;
    process_stroke_mask->y = 0;
    process_stroke_mask->pixels = dt_drawlayer_cache_alloc_temp_buffer(mask_count * sizeof(float),
                                                                        mask_buffer_name);
    process_stroke_mask->external_alloc = TRUE;
    if(!process_stroke_mask->pixels)
    {
      process_stroke_mask->width = 0;
      process_stroke_mask->height = 0;
      return FALSE;
    }
    process_stroke_mask->cache_entry
        = dt_dev_pixelpipe_cache_ref_entry_for_host_ptr(process_stroke_mask->pixels);
    process_stroke_mask->cache_hash = process_stroke_mask->cache_entry ? process_stroke_mask->cache_entry->hash
                                                                       : DT_PIXELPIPE_CACHE_HASH_INVALID;
    if(!process_stroke_mask->cache_entry)
    {
      process_stroke_mask->width = 0;
      process_stroke_mask->height = 0;
      return FALSE;
    }
  }

  return TRUE;
}
