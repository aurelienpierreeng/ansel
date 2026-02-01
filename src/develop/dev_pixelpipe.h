/*
    This file is part of darktable,
    Copyright (C) 2026 - Ansel developers

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#pragma once

#include <inttypes.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


// Force a full rebuild of the pipe, needed when module order is changed.
// Resync the full history, which may be expensive.
// Pixelpipe cache will need to be flushed too when this is called,
// for raster masks to work properly.
void dt_dev_pixelpipe_rebuild_all(struct dt_develop_t *dev);

void dt_dev_pixelpipe_update_main_real(struct dt_develop_t *dev);
// Invalidate the main image preview in darkroom, resync only the last history item(s)
// with pipeline nodes.
// This is the most common usecase when interacting with modules and masks.
#define dt_dev_pixelpipe_update_main(dev) DT_DEBUG_TRACE_WRAPPER(DT_DEBUG_DEV, dt_dev_pixelpipe_update_main_real, (dev))

void dt_dev_pixelpipe_update_preview_real(struct dt_develop_t *dev);
// Invalidate the thumbnail preview in darkroom, resync only the last history item.
#define dt_dev_pixelpipe_update_preview(dev) DT_DEBUG_TRACE_WRAPPER(DT_DEBUG_DEV, dt_dev_pixelpipe_update_preview_real, (dev))

void dt_dev_pixelpipe_update_all_real(struct dt_develop_t *dev);
// Invalidate the main image and the thumbnail in darkroom, resync only the last history item.
#define dt_dev_pixelpipe_update_all(dev) DT_DEBUG_TRACE_WRAPPER(DT_DEBUG_DEV, dt_dev_pixelpipe_update_all_real, (dev))

void dt_dev_pixelpipe_change_zoom_main_real(struct dt_develop_t *dev);
// Invalidate the main image preview in darkroom.
// This doesn't resync history at all, only update the coordinates of the region of interest (ROI).
#define dt_dev_pixelpipe_change_zoom_main(dev) DT_DEBUG_TRACE_WRAPPER(DT_DEBUG_DEV, dt_dev_pixelpipe_change_zoom_main_real, (dev))

// Invalidate the preview in darkroom.
// This doesn't resync history at all, only update the coordinates of the region of interest (ROI).
void dt_dev_pixelpipe_change_zoom_preview(struct dt_develop_t *dev);

// Invalidate the main image and the thumbnail in darkroom.
// Resync the whole history with the pipeline nodes, which may be expensive.
void dt_dev_pixelpipe_resync_all(struct dt_develop_t *dev);

// Invalidate the main image in darkroom.
// Resync the whole history with the pipeline nodes, which may be expensive.
void dt_dev_pixelpipe_resync_main(struct dt_develop_t *dev);

// Invalidate the thumbnail in darkroom.
// Resync the whole history with the pipeline nodes, which may be expensive.
void dt_dev_pixelpipe_resync_preview(struct dt_develop_t *dev);

// Flush caches of dev pipes and force a full recompute
void dt_dev_pixelpipe_reset_all(struct dt_develop_t *dev);

// Queue a pipeline update and reprocess the main image pipeline at once.
// If full, resync the whole history (might get expensive), else only the last history item(s)
void dt_dev_pixelpipe_refresh_main(struct dt_develop_t *dev, gboolean full);

// Queue a pipeline update and reprocess the preview image pipeline at once.
// If full, resync the whole history (might get expensive), else only the last history item(s)
void dt_dev_pixel_pipe_refresh_preview(struct dt_develop_t *dev, gboolean full);

#ifdef __cplusplus
}
#endif
