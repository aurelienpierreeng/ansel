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

#include "common/darktable.h"
#include "iop/drawlayer/cache.h"

#include "develop/imageop_math.h"

#include <math.h>
#include <string.h>

/** @brief Allocate temporary aligned cache buffer. */
void *dt_drawlayer_cache_alloc_temp_buffer(const size_t bytes, const char *name)
{
  if(bytes == 0) return NULL;
  return dt_pixelpipe_cache_alloc_align_cache_impl(darktable.pixelpipe_cache, bytes, DT_DEV_PIXELPIPE_NONE, name);
}

/** @brief Free temporary aligned cache buffer. */
void dt_drawlayer_cache_free_temp_buffer(void **buffer, const char *name)
{
  if(!buffer || !*buffer) return;
  dt_pixelpipe_cache_free_align_cache(darktable.pixelpipe_cache, buffer, name);
}

/** @brief Ensure scratch RGBA float capacity in pixels. */
float *dt_drawlayer_cache_ensure_scratch_buffer(float **buffer, size_t *capacity_pixels, const size_t needed_pixels,
                                                const char *name)
{
  if(!buffer || !capacity_pixels || needed_pixels == 0) return NULL;
  if(*capacity_pixels < needed_pixels)
  {
    dt_drawlayer_cache_free_temp_buffer((void **)buffer, name);
    float *new_buffer = dt_drawlayer_cache_alloc_temp_buffer(needed_pixels * 4 * sizeof(float), name);
    if(!new_buffer) return NULL;
    *buffer = new_buffer;
    *capacity_pixels = needed_pixels;
  }
  return *buffer;
}

/** @brief Fill float RGBA buffer with transparent black. */
void dt_drawlayer_cache_clear_transparent_float(float *pixels, const size_t pixel_count)
{
  if(!pixels) return;
  memset(pixels, 0, pixel_count * 4 * sizeof(float));
}

/** @brief Release patch storage and reset patch metadata. */
void dt_drawlayer_cache_patch_clear(dt_drawlayer_cache_patch_t *patch, const char *external_alloc_name)
{
  if(!patch) return;
  if(patch->external_alloc)
  {
    if(patch->cache_entry)
      dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, FALSE, patch->cache_entry);
    void *buffer = patch->pixels;
    dt_pixelpipe_cache_free_align_cache(darktable.pixelpipe_cache, &buffer, external_alloc_name);
  }
  else if(patch->cache_entry)
  {
    dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, FALSE, patch->cache_entry);
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
  if(!patch || pixel_count == 0 || width <= 0 || height <= 0) return FALSE;

  void *data = NULL;
  dt_pixel_cache_entry_t *entry = NULL;
  const int created = dt_dev_pixelpipe_cache_get(darktable.pixelpipe_cache, hash, pixel_count * 4 * sizeof(float),
                                                 name, DT_DEV_PIXELPIPE_NONE, TRUE, &data, &entry);
  if(created_out) *created_out = created;
  if(!data || !entry)
  {
    if(entry)
    {
      if(created) dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, FALSE, entry);
      dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, FALSE, entry);
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

  if(created) dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, FALSE, entry);
  return TRUE;
}

/** @brief Ensure a float stroke-mask buffer exists and matches the requested size. */
gboolean dt_drawlayer_cache_ensure_mask_buffer(dt_drawlayer_cache_patch_t *mask, const int width,
                                               const int height, const char *name)
{
  if(!mask || width <= 0 || height <= 0) return FALSE;

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
    if(!mask->pixels)
    {
      mask->width = 0;
      mask->height = 0;
      return FALSE;
    }

    mask->cache_entry = dt_dev_pixelpipe_cache_ref_entry_for_host_ptr(darktable.pixelpipe_cache, mask->pixels);
    mask->cache_hash = mask->cache_entry ? mask->cache_entry->hash : DT_PIXELPIPE_CACHE_HASH_INVALID;
    if(!mask->cache_entry)
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
  if(!patch || !patch->cache_entry) return;
  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, TRUE, patch->cache_entry);
}

/** @brief Release read lock on shared patch cache entry. */
void dt_drawlayer_cache_patch_rdunlock(const dt_drawlayer_cache_patch_t *patch)
{
  if(!patch || !patch->cache_entry) return;
  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, FALSE, patch->cache_entry);
}

/** @brief Acquire write lock on shared patch cache entry. */
void dt_drawlayer_cache_patch_wrlock(const dt_drawlayer_cache_patch_t *patch)
{
  if(!patch || !patch->cache_entry) return;
  dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, TRUE, patch->cache_entry);
}

/** @brief Release write lock on shared patch cache entry. */
void dt_drawlayer_cache_patch_wrunlock(const dt_drawlayer_cache_patch_t *patch)
{
  if(!patch || !patch->cache_entry) return;
  dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, FALSE, patch->cache_entry);
}

/** @brief Reset process-patch validity and dirty-state bookkeeping. */
void dt_drawlayer_cache_invalidate_process_patch_state(gboolean *process_patch_valid, gboolean *process_patch_dirty,
                                                       dt_drawlayer_damaged_rect_t *process_dirty_rect,
                                                       int *process_patch_padding,
                                                       dt_iop_roi_t *process_combined_roi)
{
  if(process_patch_valid) *process_patch_valid = FALSE;
  if(process_patch_dirty) *process_patch_dirty = FALSE;
  if(process_dirty_rect) dt_drawlayer_paint_runtime_state_reset(process_dirty_rect);
  if(process_patch_padding) *process_patch_padding = 0;
  if(process_combined_roi) memset(process_combined_roi, 0, sizeof(*process_combined_roi));
}

/** @brief Ensure process patch and process-stroke-mask backing buffers exist. */
gboolean dt_drawlayer_cache_ensure_process_patch_buffer(dt_drawlayer_cache_patch_t *process_patch,
                                                        dt_drawlayer_cache_patch_t *process_stroke_mask,
                                                        const int width,
                                                        const int height, const char *patch_buffer_name,
                                                        const char *mask_buffer_name)
{
  if(!process_patch || !process_stroke_mask || width <= 0 || height <= 0)
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
    if(!process_patch->pixels) return FALSE;
    process_patch->cache_entry
        = dt_dev_pixelpipe_cache_ref_entry_for_host_ptr(darktable.pixelpipe_cache, process_patch->pixels);
    process_patch->cache_hash = process_patch->cache_entry ? process_patch->cache_entry->hash
                                                           : DT_PIXELPIPE_CACHE_HASH_INVALID;
    if(!process_patch->cache_entry) return FALSE;
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
        = dt_dev_pixelpipe_cache_ref_entry_for_host_ptr(darktable.pixelpipe_cache, process_stroke_mask->pixels);
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

/** @brief Build combined ROI mapping process tile coordinates to fitted source image. */
void dt_drawlayer_cache_build_combined_process_roi(const dt_dev_pixelpipe_iop_t *piece,
                                                   const dt_iop_roi_t *process_roi, const int current_full_w,
                                                   const int current_full_h, const int src_w, const int src_h,
                                                   const int module_origin_x, const int module_origin_y,
                                                   dt_iop_roi_t *combined_roi)
{
  if(!piece || !process_roi || !combined_roi || current_full_w <= 0 || current_full_h <= 0
     || src_w <= 0 || src_h <= 0)
  {
    if(combined_roi) memset(combined_roi, 0, sizeof(*combined_roi));
    return;
  }

  const float fit = fminf((float)current_full_w / (float)src_w, (float)current_full_h / (float)src_h);
  const int scaled_w = MAX(1, MIN(current_full_w, (int)lroundf(src_w * fit)));
  const int scaled_h = MAX(1, MIN(current_full_h, (int)lroundf(src_h * fit)));
  const int fit_offset_x = MAX((current_full_w - scaled_w) / 2, 0);
  const int fit_offset_y = MAX((current_full_h - scaled_h) / 2, 0);

  const float inv_scale = process_roi->scale > 1e-6f ? (1.0f / process_roi->scale) : 0.0f;
  const float tile_origin_canvas_x = (float)module_origin_x + process_roi->x * inv_scale;
  const float tile_origin_canvas_y = (float)module_origin_y + process_roi->y * inv_scale;

  combined_roi->x = (int)lroundf((tile_origin_canvas_x - fit_offset_x) * process_roi->scale);
  combined_roi->y = (int)lroundf((tile_origin_canvas_y - fit_offset_y) * process_roi->scale);
  combined_roi->width = process_roi->width;
  combined_roi->height = process_roi->height;
  combined_roi->scale = process_roi->scale * fmaxf(fit, 1e-6f);
}

/** @brief Build combined ROI using module origin heuristics from piece buffers. */
void dt_drawlayer_cache_resolve_piece_input_origin(const dt_dev_pixelpipe_iop_t *piece,
                                                   const int current_full_w, const int current_full_h,
                                                   int *module_origin_x, int *module_origin_y)
{
  int origin_x = 0;
  int origin_y = 0;

  if(piece)
  {
    origin_x = piece->buf_in.x;
    origin_y = piece->buf_in.y;

    if(origin_x == 0 && piece->buf_in.width < current_full_w && piece->buf_in.height == current_full_h)
      origin_x = MAX((current_full_w - piece->buf_in.width) / 2, 0);
    if(origin_y == 0 && piece->buf_in.height < current_full_h && piece->buf_in.width == current_full_w)
      origin_y = MAX((current_full_h - piece->buf_in.height) / 2, 0);
  }

  if(module_origin_x) *module_origin_x = origin_x;
  if(module_origin_y) *module_origin_y = origin_y;
}

/** @brief Build combined ROI using module origin heuristics from piece buffers. */
void dt_drawlayer_cache_build_combined_process_roi_for_piece(const dt_dev_pixelpipe_iop_t *piece,
                                                             const dt_iop_roi_t *process_roi,
                                                             const int current_full_w,
                                                             const int current_full_h,
                                                             const int src_w, const int src_h,
                                                             dt_iop_roi_t *combined_roi)
{
  if(!piece || !process_roi || !combined_roi || current_full_w <= 0 || current_full_h <= 0
     || src_w <= 0 || src_h <= 0)
  {
    if(combined_roi) memset(combined_roi, 0, sizeof(*combined_roi));
    return;
  }

  int module_origin_x = 0;
  int module_origin_y = 0;
  dt_drawlayer_cache_resolve_piece_input_origin(piece, current_full_w, current_full_h,
                                                &module_origin_x, &module_origin_y);

  dt_drawlayer_cache_build_combined_process_roi(piece, process_roi, current_full_w, current_full_h,
                                                src_w, src_h, module_origin_x, module_origin_y, combined_roi);
}

/** @brief Resolve source/target ROIs used when blending process patch to output ROI. */
gboolean dt_drawlayer_cache_build_process_blend_rois(const dt_drawlayer_cache_patch_t *process_patch,
                                                     const int process_patch_padding,
                                                     const dt_iop_roi_t *roi_out,
                                                     dt_iop_roi_t *blend_target_roi,
                                                     dt_iop_roi_t *source_process_roi,
                                                     gboolean *direct_copy)
{
  if(!process_patch || !roi_out || !direct_copy) return FALSE;
  if(process_patch->width <= 0 || process_patch->height <= 0 || roi_out->width <= 0 || roi_out->height <= 0)
    return FALSE;

  if(source_process_roi)
  {
    source_process_roi->x = 0;
    source_process_roi->y = 0;
    source_process_roi->width = process_patch->width;
    source_process_roi->height = process_patch->height;
    source_process_roi->scale = 1.0f;
  }

  const int process_pad = MAX(process_patch_padding, 0);
  if(blend_target_roi)
  {
    blend_target_roi->x = process_pad;
    blend_target_roi->y = process_pad;
    blend_target_roi->width = roi_out->width;
    blend_target_roi->height = roi_out->height;
    blend_target_roi->scale = 1.0f;
  }

  *direct_copy = (process_patch->width == roi_out->width && process_patch->height == roi_out->height);
  return TRUE;
}

/** @brief Copy or resample process patch into output layer buffer. */
gboolean dt_drawlayer_cache_resample_process_patch_to_output(const dt_drawlayer_cache_patch_t *process_patch,
                                                             const int process_patch_padding,
                                                             const dt_iop_roi_t *roi_out,
                                                             float *layerbuf,
                                                             const int layerbuf_width)
{
  if(!process_patch || !roi_out || !layerbuf || !process_patch->pixels || layerbuf_width <= 0) return FALSE;

  dt_iop_roi_t target_roi = { 0 };
  dt_iop_roi_t source_process_roi = { 0 };
  gboolean direct_copy = FALSE;
  if(!dt_drawlayer_cache_build_process_blend_rois(process_patch, process_patch_padding, roi_out,
                                                  &target_roi, &source_process_roi, &direct_copy))
    return FALSE;

  if(direct_copy)
  {
    memcpy(layerbuf, process_patch->pixels, (size_t)roi_out->width * roi_out->height * 4 * sizeof(float));
    return TRUE;
  }

  dt_iop_clip_and_zoom(layerbuf, process_patch->pixels, &target_roi, &source_process_roi,
                       layerbuf_width, process_patch->width);
  return TRUE;
}

/** @brief Populate process patch from base patch using ROI crop/scale rules. */
gboolean dt_drawlayer_cache_populate_process_patch_from_base(const dt_drawlayer_cache_patch_t *base_patch,
                                                             const dt_drawlayer_cache_patch_t *base_stroke_mask,
                                                             dt_drawlayer_cache_patch_t *process_patch,
                                                             dt_drawlayer_cache_patch_t *process_stroke_mask,
                                                             const dt_iop_roi_t *combined_roi, const int process_pad,
                                                             const int patch_width, const int patch_height,
                                                             gboolean *process_patch_valid,
                                                             gboolean *process_patch_dirty,
                                                             dt_drawlayer_damaged_rect_t *process_dirty_rect,
                                                             int *process_patch_padding,
                                                             dt_iop_roi_t *process_combined_roi,
                                                             const char *patch_buffer_name,
                                                             const char *mask_buffer_name)
{
  if(!base_patch || !process_patch || !combined_roi || !base_patch->pixels || base_patch->width <= 0
     || base_patch->height <= 0 || patch_width <= 0 || patch_height <= 0 || combined_roi->scale <= 1e-6f)
    return FALSE;

  process_patch->x = 0;
  process_patch->y = 0;
  if(process_patch_padding) *process_patch_padding = process_pad;
  if(process_combined_roi) *process_combined_roi = *combined_roi;

  dt_drawlayer_cache_patch_rdlock(base_patch);
  if(fabs(combined_roi->scale - 1.0) <= 1e-6f)
  {
    const int src_x0 = MAX(0, combined_roi->x);
    const int src_y0 = MAX(0, combined_roi->y);
    const int src_x1 = MIN(base_patch->width, combined_roi->x + combined_roi->width);
    const int src_y1 = MIN(base_patch->height, combined_roi->y + combined_roi->height);
    const int copy_w = src_x1 - src_x0;
    const int copy_h = src_y1 - src_y0;

    if(copy_w > 0 && copy_h > 0)
    {
      const int dst_x0 = src_x0 - combined_roi->x;
      const int dst_y0 = src_y0 - combined_roi->y;
      const gboolean full_coverage = (dst_x0 == 0 && dst_y0 == 0
                                      && copy_w == process_patch->width
                                      && copy_h == process_patch->height);
      if(!full_coverage)
      {
        memset(process_patch->pixels, 0,
               (size_t)process_patch->width * process_patch->height * 4 * sizeof(float));
      }
      __OMP_PARALLEL_FOR__(if(copy_h > 8))
      for(int y = 0; y < copy_h; y++)
      {
        const float *src = base_patch->pixels + 4 * ((size_t)(src_y0 + y) * base_patch->width + src_x0);
        float *dst = process_patch->pixels + 4 * ((size_t)(dst_y0 + y) * process_patch->width + dst_x0);
        memcpy(dst, src, (size_t)copy_w * 4 * sizeof(float));
      }
    }
    else
    {
      memset(process_patch->pixels, 0, (size_t)process_patch->width * process_patch->height * 4 * sizeof(float));
    }
  }
  else
  {
    const dt_iop_roi_t source_full_roi = {
      .x = 0,
      .y = 0,
      .width = base_patch->width,
      .height = base_patch->height,
      .scale = 1.0f,
    };
    dt_iop_clip_and_zoom(process_patch->pixels, base_patch->pixels, combined_roi, &source_full_roi,
                         process_patch->width, base_patch->width);
  }
  dt_drawlayer_cache_patch_rdunlock(base_patch);

  if(process_patch_valid) *process_patch_valid = TRUE;
  if(process_patch_dirty) *process_patch_dirty = FALSE;
  if(process_dirty_rect) dt_drawlayer_paint_runtime_state_reset(process_dirty_rect);

  if(process_stroke_mask && process_stroke_mask->pixels
     && process_stroke_mask->width == process_patch->width
     && process_stroke_mask->height == process_patch->height)
  {
    const gboolean have_base_mask = base_stroke_mask && base_stroke_mask->pixels
                                    && base_stroke_mask->width > 0 && base_stroke_mask->height > 0;
    if(!have_base_mask)
      memset(process_stroke_mask->pixels, 0,
             (size_t)process_stroke_mask->width * process_stroke_mask->height * sizeof(float));
    else if(fabs(combined_roi->scale - 1.0) <= 1e-6)
    {
      const int src_x0 = MAX(0, combined_roi->x);
      const int src_y0 = MAX(0, combined_roi->y);
      const int src_x1 = MIN(base_stroke_mask->width, combined_roi->x + combined_roi->width);
      const int src_y1 = MIN(base_stroke_mask->height, combined_roi->y + combined_roi->height);
      const int copy_w = src_x1 - src_x0;
      const int copy_h = src_y1 - src_y0;
      memset(process_stroke_mask->pixels, 0,
             (size_t)process_stroke_mask->width * process_stroke_mask->height * sizeof(float));
      if(copy_w > 0 && copy_h > 0)
      {
        const int dst_x0 = src_x0 - combined_roi->x;
        const int dst_y0 = src_y0 - combined_roi->y;
        __OMP_PARALLEL_FOR__(if(copy_h > 8))
        for(int y = 0; y < copy_h; y++)
        {
          const float *src = base_stroke_mask->pixels + (size_t)(src_y0 + y) * base_stroke_mask->width + src_x0;
          float *dst = process_stroke_mask->pixels + (size_t)(dst_y0 + y) * process_stroke_mask->width + dst_x0;
          memcpy(dst, src, (size_t)copy_w * sizeof(float));
        }
      }
    }
    else
    {
      __OMP_PARALLEL_FOR__(if(process_stroke_mask->height > 8))
      for(int y = 0; y < process_stroke_mask->height; y++)
      {
        const float src_y = ((float)y + 0.5f) / combined_roi->scale + (float)combined_roi->y - 0.5f;
        const int sy = CLAMP((int)lroundf(src_y), 0, base_stroke_mask->height - 1);
        float *dst = process_stroke_mask->pixels + (size_t)y * process_stroke_mask->width;
        for(int x = 0; x < process_stroke_mask->width; x++)
        {
          const float src_x = ((float)x + 0.5f) / combined_roi->scale + (float)combined_roi->x - 0.5f;
          const int sx = CLAMP((int)lroundf(src_x), 0, base_stroke_mask->width - 1);
          dst[x] = base_stroke_mask->pixels[(size_t)sy * base_stroke_mask->width + sx];
        }
      }
    }
  }
#ifdef HAVE_OPENCL
  dt_dev_pixelpipe_cache_flush_host_pinned_image(darktable.pixelpipe_cache, process_patch->pixels, NULL, -1);
#endif
  return TRUE;
}

/** @brief Flush dirty process patch region back into the base patch. */
gboolean dt_drawlayer_cache_flush_process_patch_to_base(dt_drawlayer_cache_patch_t *base_patch,
                                                        dt_drawlayer_cache_patch_t *base_stroke_mask,
                                                        const dt_iop_roi_t *process_combined_roi,
                                                        dt_drawlayer_cache_patch_t *process_patch,
                                                        dt_drawlayer_cache_patch_t *process_stroke_mask,
                                                        float **process_update_pixels,
                                                        size_t *process_update_capacity_pixels,
                                                        gboolean *cache_dirty, gboolean *process_patch_dirty,
                                                        dt_drawlayer_damaged_rect_t *process_dirty_rect,
                                                        const char *update_buffer_name)
{
  if(!base_patch || !process_combined_roi || !process_patch
     || !process_update_pixels || !process_update_capacity_pixels)
    return FALSE;
  if(!process_patch->pixels || !base_patch->pixels || !process_patch_dirty || !*process_patch_dirty) return TRUE;
  if(process_patch->width <= 0 || process_patch->height <= 0 || base_patch->width <= 0 || base_patch->height <= 0)
    return TRUE;
  if(process_combined_roi->scale <= 1e-6f) return TRUE;

  const float inv_scale = 1.0f / process_combined_roi->scale;
  int src_x0 = MAX((int)floorf(process_combined_roi->x * inv_scale), 0);
  int src_y0 = MAX((int)floorf(process_combined_roi->y * inv_scale), 0);
  int src_x1 = MIN((int)ceilf((process_combined_roi->x + process_patch->width) * inv_scale), base_patch->width);
  int src_y1 = MIN((int)ceilf((process_combined_roi->y + process_patch->height) * inv_scale), base_patch->height);

  const gboolean has_dirty_bounds = process_dirty_rect && process_dirty_rect->valid;
  if(has_dirty_bounds)
  {
    const int dirty_x0 = CLAMP(process_dirty_rect->nw[0], 0, process_patch->width);
    const int dirty_y0 = CLAMP(process_dirty_rect->nw[1], 0, process_patch->height);
    const int dirty_x1 = CLAMP(process_dirty_rect->se[0], 0, process_patch->width);
    const int dirty_y1 = CLAMP(process_dirty_rect->se[1], 0, process_patch->height);
    src_x0 = MAX((int)floorf((dirty_x0 + process_combined_roi->x) * inv_scale), 0);
    src_y0 = MAX((int)floorf((dirty_y0 + process_combined_roi->y) * inv_scale), 0);
    src_x1 = MIN((int)ceilf((dirty_x1 + process_combined_roi->x) * inv_scale), base_patch->width);
    src_y1 = MIN((int)ceilf((dirty_y1 + process_combined_roi->y) * inv_scale), base_patch->height);
  }

  if(src_x1 <= src_x0 || src_y1 <= src_y0) return TRUE;

  const int dst_w = src_x1 - src_x0;
  const int dst_h = src_y1 - src_y0;
  const size_t needed_pixels = (size_t)dst_w * dst_h;
  float *update_buffer = dt_drawlayer_cache_ensure_scratch_buffer(process_update_pixels,
                                                                   process_update_capacity_pixels,
                                                                   needed_pixels, update_buffer_name);
  if(!update_buffer)
    return FALSE;

  const dt_iop_roi_t process_roi = {
    .x = 0,
    .y = 0,
    .width = process_patch->width,
    .height = process_patch->height,
    .scale = 1.0f,
  };
  const dt_iop_roi_t inverse_roi = {
    .x = (int)lroundf((float)src_x0 - process_combined_roi->x * inv_scale),
    .y = (int)lroundf((float)src_y0 - process_combined_roi->y * inv_scale),
    .width = dst_w,
    .height = dst_h,
    .scale = inv_scale,
  };

  dt_iop_clip_and_zoom(update_buffer, process_patch->pixels, &inverse_roi, &process_roi, dst_w, process_patch->width);

  dt_drawlayer_cache_patch_wrlock(base_patch);
  __OMP_PARALLEL_FOR__(if(dst_h > 8))
  for(int yy = 0; yy < dst_h; yy++)
  {
    memcpy(base_patch->pixels + 4 * ((size_t)(src_y0 + yy) * base_patch->width + src_x0),
           update_buffer + 4 * ((size_t)yy * dst_w),
           (size_t)dst_w * 4 * sizeof(float));
  }
#ifdef HAVE_OPENCL
  dt_dev_pixelpipe_cache_flush_host_pinned_image(darktable.pixelpipe_cache, base_patch->pixels, NULL, -1);
#endif
  dt_drawlayer_cache_patch_wrunlock(base_patch);

  if(base_stroke_mask && process_stroke_mask && base_stroke_mask->pixels && process_stroke_mask->pixels
     && base_stroke_mask->width > 0 && base_stroke_mask->height > 0
     && process_stroke_mask->width == process_patch->width
     && process_stroke_mask->height == process_patch->height)
  {
    float *mask_update_buffer = dt_drawlayer_cache_ensure_scratch_buffer(process_update_pixels,
                                                                         process_update_capacity_pixels,
                                                                         needed_pixels,
                                                                         update_buffer_name);
    if(!mask_update_buffer) return FALSE;

    dt_iop_clip_and_zoom(mask_update_buffer, process_stroke_mask->pixels, &inverse_roi, &process_roi,
                         dst_w, process_stroke_mask->width);
    __OMP_PARALLEL_FOR__(if(dst_h > 8))
    for(int yy = 0; yy < dst_h; yy++)
    {
      memcpy(base_stroke_mask->pixels + (size_t)(src_y0 + yy) * base_stroke_mask->width + src_x0,
             mask_update_buffer + (size_t)yy * dst_w,
             (size_t)dst_w * sizeof(float));
    }
  }

  if(cache_dirty) *cache_dirty = TRUE;
  *process_patch_dirty = FALSE;
  if(process_dirty_rect) dt_drawlayer_paint_runtime_state_reset(process_dirty_rect);
  return TRUE;
}
