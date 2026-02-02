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
#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif


// Force a full rebuild of the pipe, needed when module order is changed.
// Resync the full history, which may be expensive.
// Pixelpipe cache will need to be flushed too when this is called,
// for raster masks to work properly.
void dt_dev_pixelpipe_rebuild_all(struct dt_develop_t *dev);

void dt_dev_pixelpipe_update_history_main_real(struct dt_develop_t *dev);
// Invalidate the main image preview in darkroom, resync only the last history item(s)
// with pipeline nodes.
// This is the most common usecase when interacting with modules and masks.
#define dt_dev_pixelpipe_update_history_main(dev) DT_DEBUG_TRACE_WRAPPER(DT_DEBUG_DEV, dt_dev_pixelpipe_update_history_main_real, (dev))

void dt_dev_pixelpipe_update_history_preview_real(struct dt_develop_t *dev);
// Invalidate the thumbnail preview in darkroom, resync only the last history item.
#define dt_dev_pixelpipe_update_preview(dev) DT_DEBUG_TRACE_WRAPPER(DT_DEBUG_DEV, dt_dev_pixelpipe_update_history_preview_real, (dev))

void dt_dev_pixelpipe_update_history_all_real(struct dt_develop_t *dev);
// Invalidate the main image and the thumbnail in darkroom, resync only the last history item.
#define dt_dev_pixelpipe_update_history_all(dev) DT_DEBUG_TRACE_WRAPPER(DT_DEBUG_DEV, dt_dev_pixelpipe_update_history_all_real, (dev))

void dt_dev_pixelpipe_update_zoom_main_real(struct dt_develop_t *dev);
// Invalidate the main image preview in darkroom.
// This doesn't resync history at all, only update the coordinates of the region of interest (ROI).
#define dt_dev_pixelpipe_update_zoom_main(dev) DT_DEBUG_TRACE_WRAPPER(DT_DEBUG_DEV, dt_dev_pixelpipe_update_zoom_main_real, (dev))

// Invalidate the preview in darkroom.
// This doesn't resync history at all, only update the coordinates of the region of interest (ROI).
void dt_dev_pixelpipe_update_zoom_preview(struct dt_develop_t *dev);

// Invalidate the main image and the thumbnail in darkroom.
// Resync the whole history with the pipeline nodes, which may be expensive.
void dt_dev_pixelpipe_resync_history_all(struct dt_develop_t *dev);

// Invalidate the main image in darkroom.
// Resync the whole history with the pipeline nodes, which may be expensive.
void dt_dev_pixelpipe_resync_history_main(struct dt_develop_t *dev);

// Invalidate the thumbnail in darkroom.
// Resync the whole history with the pipeline nodes, which may be expensive.
void dt_dev_pixelpipe_resync_history_preview(struct dt_develop_t *dev);

// Flush caches of dev pipes and force a full recompute
void dt_dev_pixelpipe_reset_all(struct dt_develop_t *dev);

// Queue a pipeline update and reprocess the main image pipeline at once.
// If full, resync the whole history (might get expensive), else only the last history item(s)
void dt_dev_pixelpipe_refresh_main(struct dt_develop_t *dev, gboolean full);

// Queue a pipeline update and reprocess the preview image pipeline at once.
// If full, resync the whole history (might get expensive), else only the last history item(s)
void dt_dev_pixelpipe_refresh_preview(struct dt_develop_t *dev, gboolean full);

// Queue a pipeline update and reprocess the preview and main image pipelines at once.
// If full, resync the whole history (might get expensive), else only the last history item(s)
void dt_dev_pixelpipe_refresh_all(struct dt_develop_t *dev, gboolean full);

// Queue a pipeline ROI change and reprocess the main image pipeline.
void dt_dev_pixelpipe_change_zoom_main(struct dt_develop_t *dev);

// returns the dimensions of a virtual image of size (width_in, height_in) image after processing
// all modules of the pipe. This chains calls to module's modify_roi_out() methods in pipeline order.
// Doesn't actually compute pixels.
void dt_dev_pixelpipe_get_roi_out(struct dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev, const int width_in,
                                  const int height_in, int *width, int *height);
                                
// Compute and save into each piece->planned_roi_out/in the proper module-wise ROI to achieve
// the desired sizes from roi_out, from end to start. This chains calls to module's modify_roi_in() methods
// in pipeline reverse order.
// Doesn't actually compute pixels.
void dt_dev_pixelpipe_get_roi_in(struct dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev, const struct dt_iop_roi_t roi_out);

// Check if current_module is performing operations that dev->gui_module (active GUI module)
// wants disabled. Use that to disable some features of current_module.
// This is used mostly with distortion operations when the active GUI module
// needs a full-ROI/undistorted input for its own editing mode,
// like moving the framing on the full image.
// WARNING: this doesn't check WHAT particular operations are performed and
// and what operations should be cancelled (nor if they should all be cancelled).
// So far, all the code uses that to prevent distortions on module output, masks and roi_out changes (cropping).
// Meaning ANY of these operations will disable ALL of these operations.
gboolean dt_dev_pixelpipe_activemodule_disables_currentmodule(struct dt_develop_t *dev,
                                                              struct dt_iop_module_t *current_module);

// wrapper for cleanup_nodes, create_nodes, synch_all and synch_top, decides upon changed event which one to
// take on. also locks dev->history_mutex.
void dt_dev_pixelpipe_change(struct dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev);

// Get the global hash of a pipe node (piece), or a fallback if none.
uint64_t dt_dev_pixelpipe_node_hash(struct dt_dev_pixelpipe_t *pipe, 
                                    const struct dt_dev_pixelpipe_iop_t *piece, 
                                    const struct dt_iop_roi_t, const int pos);

// Compute the sequential hash over the pipeline for each module.
// Need to run after dt_dev_pixelpipe_get_roi_in() has updated processed ROI in/out
void dt_pixelpipe_get_global_hash(struct dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev);

// Return TRUE if the current backbuffer for the current pipe is in sync with current dev history stack.
gboolean dt_dev_pixelpipe_is_backbufer_valid(struct dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev);

// Return TRUE if the current pipeline (topology and node parameters) is in sync with current dev history stack.
gboolean dt_dev_pixelpipe_is_pipeline_valid(struct dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev);

// Get the outbut backbuffer associated with the specified pipeline from the pixelpipe cache.
// If no cache entry is found, we restart a new pipeline recomputation.
// If a cache entry is found, remember that the cache line has its ref_count increased
// and will to be manually decreased once the output is consumed, otherwise it will
// never be freed.
// Note that we don't check if the backup is valid (up-to-date), only if it exists.
// Return the pointer reference to the pixel data. It doesn't belong to the caller and should not be freed.
void *dt_dev_pixelpipe_get_backbuf(struct dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev);


#ifdef __cplusplus
}
#endif
