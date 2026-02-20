/*
    This file is part of the Ansel project.
    Copyright (C) 2009-2022 darktable developers.
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

/**
 * @file pixelpipe_gui.c
 *
 * @brief Pixelpipe GUI sampling helpers (histograms + color picker).
 *
 * @details
 * This file centralizes the code paths that exist **only** to feed the GUI:
 *
 * - per-module histograms (small, module-local),
 * - the global "raw/output/display" histograms stored in `dt_develop_t`,
 * - the interactive color picker (box/point sample) used by the currently edited module.
 *
 * Why this is separate from the pixel processing code
 * ---------------------------------------------------
 *
 * The pixelpipe (in `pixelpipe_hb.c`) is primarily a functional pipeline:
 * "given an input buffer + module params → compute an output buffer".
 *
 * GUI sampling is different:
 *
 * - it is conditional on the GUI being attached,
 * - it depends on *current UI state* (which module is active, which picker sample is enabled),
 * - it may require *special-case* handling (e.g. `gamma` outputs `uint8_t`, but we want float32 for histograms),
 * - it must obey the pixelpipe cache invariants (locks + refcounts) while reading data.
 *
 * This means it adds complexity and cross-cutting concerns that should not pollute the main processing code.
 *
 * Caveats / expectations (read this before editing)
 * ------------------------------------------------
 *
 * 1) **Preview-only by design**
 *    These helpers are intended for `DT_DEV_PIXELPIPE_PREVIEW` when `dev->gui_attached` is true.
 *    They must not be invoked for exports or background processing.
 *
 * 2) **Cache entries are the source of truth**
 *    The GUI should never access transient buffers directly. We always sample through cache entries, with
 *    appropriate cache locks, because the cache controls lifetime and eviction.
 *
 * 3) **Gamma special case**
 *    The `gamma` module produces `uint8` output for display. Histograms and picker sampling expect float buffers.
 *    Therefore global histogram sampling for `gamma` uses the *input* cache entry, not the output.
 *
 * 4) **Distortion backtransform for picker**
 *    The picker position is expressed in final preview coordinates. We must backtransform it to the module
 *    coordinates, and the transform direction depends on whether we sample input or output.
 *
 * 5) **OpenCL is not involved here**
 *    GUI sampling runs on host buffers (RAM). Any OpenCL device buffers must have been synchronized into the cache
 *    earlier in the control flow (by design, the GUI always samples cache-backed host buffers).
 *
 * 6) **In-place colorspace conversions**
 *    Some sampling paths perform colorspace conversions in-place to make values meaningful to the user.
 *    This relies on higher-level pixelpipe control flow ensuring those buffers are not used afterward in a way that
 *    would be corrupted by the conversion (typically this is guarded by module activation / forced caching).
 *    If you change where these helpers are called, revisit those assumptions.
 */

#include "common/color_picker.h"
#include "common/darktable.h"
#include "common/histogram.h"
#include "common/iop_order.h"
#include "control/signal.h"
#include "develop/blend.h"
#include "develop/pixelpipe.h"
#include "develop/pixelpipe_cache.h"
#include "gui/color_picker_proxy.h"
#include "libs/colorpicker.h"

#include <math.h>
#include <string.h>

/**
 * @brief Identify whether the picker sampling is applied on the module input or output.
 *
 * @details
 * The GUI lets users request either:
 * - sampling values at the module input ("what does this module receive?"), or
 * - sampling values at the module output ("what does this module produce?").
 *
 * This choice affects:
 * - which buffer we sample (`input` vs `*output`),
 * - the distortion backtransform direction (include/exclude the current module transform),
 * - and the colorspace metadata attached to the sample.
 */
typedef enum dt_pixelpipe_picker_source_t
{
  PIXELPIPE_PICKER_INPUT = 0,
  PIXELPIPE_PICKER_OUTPUT = 1
} dt_pixelpipe_picker_source_t;

/**
 * @brief Compute a histogram for a given module piece.
 *
 * @details
 * This is the per-module histogram that can be shown in module UIs. Each module may set
 * `piece->histogram_params` to define a ROI. If no ROI is specified, we use the full ROI.
 */
static void histogram_collect(dt_dev_pixelpipe_iop_t *piece, const void *pixel, const dt_iop_roi_t roi,
                              uint32_t **histogram, uint32_t *histogram_max)
{
  dt_dev_histogram_collection_params_t histogram_params = piece->histogram_params;
  dt_histogram_roi_t histogram_roi;

  // If the current module did not specify its own ROI, use the full ROI.
  if(histogram_params.roi == NULL)
  {
    histogram_roi = (dt_histogram_roi_t){
      .width = roi.width, .height = roi.height, .crop_x = 0, .crop_y = 0, .crop_width = 0, .crop_height = 0
    };

    histogram_params.roi = &histogram_roi;
  }

  const dt_iop_colorspace_type_t cst = piece->module->input_colorspace(piece->module, piece->pipe, piece);

  dt_histogram_helper(&histogram_params, &piece->histogram_stats, cst, piece->module->histogram_cst, pixel, histogram,
                      piece->module->histogram_middle_grey, dt_ioppr_get_pipe_work_profile_info(piece->pipe));
  dt_histogram_max_helper(&piece->histogram_stats, cst, piece->module->histogram_cst, histogram, histogram_max);
}

/**
 * @brief Map an op name to the corresponding global histogram backbuffer.
 *
 * @return A pointer to one of `dev->raw_histogram`, `dev->output_histogram`, `dev->display_histogram`,
 *         or NULL if the module is not wired to a global histogram.
 *
 * @details
 * The develop module maintains three global histograms for UI display. We keep references to the cache entries
 * feeding those histograms so that the underlying buffers are not evicted while the GUI reads them.
 */
static dt_backbuf_t *_get_backuf(dt_develop_t *dev, const char *op)
{
  if(!strcmp(op, "demosaic"))
    return &dev->raw_histogram;
  else if(!strcmp(op, "colorout"))
    return &dev->output_histogram;
  else if(!strcmp(op, "gamma"))
    return &dev->display_histogram;
  else
    return NULL;
}

/**
 * @brief Update the global histogram backbuffer to reference a specific cache entry.
 *
 * @details
 * Global histograms are displayed outside of the pixelpipe processing call stack, so we must keep a cache
 * reference (refcount increment) to prevent eviction of the buffer being displayed.
 *
 * When the hash changes, we decrement the refcount of the previous entry and increment the refcount of the new one.
 */
static void pixelpipe_get_histogram_backbuf(dt_develop_t *dev, const dt_iop_roi_t roi, dt_pixel_cache_entry_t *entry,
                                            dt_iop_module_t *module, const uint64_t hash)
{
  dt_backbuf_t *backbuf = _get_backuf(dev, module->op);
  if(backbuf == NULL) return;     // This module is not wired to global histograms.
  if(backbuf->hash == hash) return; // Hash didn't change, nothing to update.

  // Hash has changed, our previous stored entry is obsolete: decrement its refcount.
  dt_pixel_cache_entry_t *previous_entry;
  if(dt_dev_pixelpipe_cache_get_existing(darktable.pixelpipe_cache, backbuf->hash, NULL, NULL, &previous_entry))
    dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, backbuf->hash, FALSE, previous_entry);

  // Update metadata. The global histogram backbuf stores its bpp; infer it from cache entry size.
  const size_t entry_size = dt_pixel_cache_entry_get_size(entry);
  const int bpp = (roi.width > 0 && roi.height > 0) ? (int)(entry_size / ((size_t)roi.width * (size_t)roi.height)) : 0;
  dt_dev_set_backbuf(backbuf, roi.width, roi.height, bpp, hash, -1);

  // Increment the refcount on current entry so nobody removes it while the GUI still needs it.
  dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, hash, TRUE, entry);
}

/**
 * @brief Compute the sampling box in module coordinates for the interactive color picker.
 *
 * @return 0 on success, 1 if sampling should not happen (box outside ROI / invalid).
 *
 * @details
 * The GUI defines picker samples in normalized preview coordinates. We must convert them to pixel coordinates,
 * then backtransform them to the current module coordinate system.
 */
static int pixelpipe_picker_helper(dt_iop_module_t *module, const dt_iop_roi_t roi, dt_aligned_pixel_t picked_color,
                                   dt_aligned_pixel_t picked_color_min, dt_aligned_pixel_t picked_color_max,
                                   dt_pixelpipe_picker_source_t picker_source, int *box)
{
  const float wd = darktable.develop->preview_width;
  const float ht = darktable.develop->preview_height;
  const int width = roi.width;
  const int height = roi.height;
  const dt_colorpicker_sample_t *const sample = darktable.lib->proxy.colorpicker.primary_sample;

  dt_boundingbox_t fbox = { 0.0f };

  // Get absolute pixel coordinates in final preview image.
  if(sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
  {
    for(int k = 0; k < 4; k += 2) fbox[k] = sample->box[k] * wd;
    for(int k = 1; k < 4; k += 2) fbox[k] = sample->box[k] * ht;
  }
  else if(sample->size == DT_LIB_COLORPICKER_SIZE_POINT)
  {
    fbox[0] = fbox[2] = sample->point[0] * wd;
    fbox[1] = fbox[3] = sample->point[1] * ht;
  }

  // Transform back to current module coordinates.
  dt_dev_distort_backtransform_plus(darktable.develop, darktable.develop->preview_pipe, module->iop_order,
                                    (picker_source == PIXELPIPE_PICKER_INPUT) ? DT_DEV_TRANSFORM_DIR_FORW_INCL
                                                                              : DT_DEV_TRANSFORM_DIR_FORW_EXCL,
                                    fbox, 2);

  fbox[0] -= roi.x;
  fbox[1] -= roi.y;
  fbox[2] -= roi.x;
  fbox[3] -= roi.y;

  // Re-order edges of bounding box.
  box[0] = fminf(fbox[0], fbox[2]);
  box[1] = fminf(fbox[1], fbox[3]);
  box[2] = fmaxf(fbox[0], fbox[2]);
  box[3] = fmaxf(fbox[1], fbox[3]);

  if(sample->size == DT_LIB_COLORPICKER_SIZE_POINT)
  {
    // If we are sampling one point, make sure that we actually sample it.
    for(int k = 2; k < 4; k++) box[k] += 1;
  }

  // Do not continue if box is completely outside of roi.
  if(box[0] >= width || box[1] >= height || box[2] < 0 || box[3] < 0) return 1;

  // Clamp bounding box to roi.
  for(int k = 0; k < 4; k += 2) box[k] = MIN(width - 1, MAX(0, box[k]));
  for(int k = 1; k < 4; k += 2) box[k] = MIN(height - 1, MAX(0, box[k]));

  // Safety check: area needs to have minimum 1 pixel width and height.
  if(box[2] <= box[0] || box[3] <= box[1]) return 1;

  // If module isn't the one where pickers are assigned, reset the values and don't sample.
  if(module != darktable.develop->gui_module)
  {
    for(int k = 0; k < 4; k++)
    {
      picked_color_min[k] = INFINITY;
      picked_color_max[k] = -INFINITY;
      picked_color[k] = 0.0f;
    }
    return 1;
  }

  return 0;
}

/**
 * @brief Sample the color picker values (avg/min/max) from a pixel buffer.
 *
 * @details
 * The picker expects float buffers with known colorspace metadata. This function delegates the pixel
 * aggregation to `dt_color_picker_helper()`.
 */
static void pixelpipe_picker(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_iop_buffer_dsc_t *dsc,
                             const float *pixel, const dt_iop_roi_t roi, float *picked_color,
                             float *picked_color_min, float *picked_color_max,
                             const dt_iop_colorspace_type_t image_cst, dt_pixelpipe_picker_source_t picker_source)
{
  int box[4];
  if(pixelpipe_picker_helper(module, roi, picked_color, picked_color_min, picked_color_max, picker_source, box))
    return;

  dt_aligned_pixel_t avg = { 0.0f };
  dt_aligned_pixel_t min = { 0.0f };
  dt_aligned_pixel_t max = { 0.0f };

  const dt_iop_order_iccprofile_info_t *const profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);

  dt_color_picker_helper(dsc, pixel, &roi, box, avg, min, max, image_cst,
                         dt_iop_color_picker_get_active_cst(module), profile);

  for(int k = 0; k < 4; k++)
  {
    picked_color_min[k] = min[k];
    picked_color_max[k] = max[k];
    picked_color[k] = avg[k];
  }
}

/**
 * @brief Select a safe colorspace for picker sampling.
 *
 * @details
 * Some modules operate in RAW or specialized spaces. The picker wants meaningful values in an RGB-like space.
 * This helper maps the active picker colorspace request to a safe in-pipe colorspace, falling back to the
 * pipe colorspace when needed.
 */
static dt_iop_colorspace_type_t _transform_for_picker(dt_iop_module_t *self, const dt_iop_colorspace_type_t cst)
{
  const dt_iop_colorspace_type_t picker_cst = dt_iop_color_picker_get_active_cst(self);

  switch(picker_cst)
  {
    case IOP_CS_LAB:
    case IOP_CS_RGB:
    case IOP_CS_HSL:
    case IOP_CS_JZCZHZ:
      return IOP_CS_RGB;
    case IOP_CS_NONE:
      // IOP_CS_NONE is used by temperature.c as it may work in RAW or RGB: return the pipe colorspace to avoid
      // extra conversions.
      return cst;
    default:
      return picker_cst;
  }
}

/**
 * @brief Collect the per-module histogram on CPU for GUI display.
 *
 * @details
 * This is gated by:
 * - GUI state (attached) and module request flags,
 * - histogram request mode (`DT_REQUEST_ONLY_IN_GUI`),
 * - and per-module histogram enable flag.
 *
 * The histogram is stored both in the piece (for internal use) and optionally copied to the module (for UI).
 */
static void collect_histogram_on_CPU(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev,
                                     float *input, const dt_iop_roi_t roi_in,
                                     dt_iop_buffer_dsc_t *input_format,
                                     dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece)
{
  // Histogram collection for module.
  if((dev->gui_attached || !(piece->request_histogram & DT_REQUEST_ONLY_IN_GUI))
     && (piece->request_histogram & DT_REQUEST_ON))
  {
    const dt_iop_order_iccprofile_info_t *const work_profile
        = (input_format->cst != IOP_CS_RAW) ? dt_ioppr_get_pipe_work_profile_info(pipe) : NULL;

    // Transform to module input colorspace.
    dt_ioppr_transform_image_colorspace(module, input, input, roi_in.width, roi_in.height, input_format->cst,
                                        module->input_colorspace(module, pipe, piece), &input_format->cst,
                                        work_profile);

    histogram_collect(piece, input, roi_in, &(piece->histogram), piece->histogram_max);

    if(piece->histogram && (module->request_histogram & DT_REQUEST_ON))
    {
      const size_t buf_size = 4 * piece->histogram_stats.bins_count * sizeof(uint32_t);
      module->histogram = realloc(module->histogram, buf_size);
      memcpy(module->histogram, piece->histogram, buf_size);
      module->histogram_stats = piece->histogram_stats;
      memcpy(module->histogram_max, piece->histogram_max, sizeof(piece->histogram_max));
      if(module->widget) dt_control_queue_redraw_widget(module->widget);
    }
  }
}

/**
 * @brief Sample the interactive color picker for the currently edited module.
 *
 * @details
 * This is strictly GUI-only and only applies to the module currently being edited (`dev->gui_module`).
 * We may perform colorspace conversions in-place to match the picker expectations.
 */
static void _sample_color_picker(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, float *input,
                                 dt_iop_buffer_dsc_t *input_format, const dt_iop_roi_t roi_in, void **output,
                                 dt_iop_buffer_dsc_t **out_format, const dt_iop_roi_t roi_out,
                                 dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece)
{
  if(!(darktable.lib->proxy.colorpicker.picker_proxy
       && module == dev->gui_module
       && dev->gui_module->enabled
       && module->request_color_pick != DT_REQUEST_COLORPICK_OFF))
    return;

  const dt_iop_order_iccprofile_info_t *const work_profile
      = (input_format->cst != IOP_CS_RAW) ? dt_ioppr_get_pipe_work_profile_info(pipe) : NULL;

  // Ensure we are using the right colorspace for picker values.
  dt_iop_colorspace_type_t picker_cst = _transform_for_picker(module, pipe->dsc.cst);
  dt_ioppr_transform_image_colorspace(module, input, input, roi_in.width, roi_in.height,
                                      input_format->cst, picker_cst, &input_format->cst,
                                      work_profile);
  dt_ioppr_transform_image_colorspace(module, *output, *output, roi_out.width, roi_out.height,
                                      pipe->dsc.cst, picker_cst, &pipe->dsc.cst,
                                      work_profile);

  pixelpipe_picker(module, piece, &piece->dsc_in, (float *)input, roi_in, module->picked_color,
                   module->picked_color_min, module->picked_color_max, input_format->cst, PIXELPIPE_PICKER_INPUT);
  pixelpipe_picker(module, piece, &pipe->dsc, (float *)(*output), roi_out, module->picked_output_color,
                   module->picked_output_color_min, module->picked_output_color_max,
                   pipe->dsc.cst, PIXELPIPE_PICKER_OUTPUT);

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_CONTROL_PICKERDATA_READY, module, piece);
}

/**
 * @brief Sample all GUI observables for a processed module node.
 *
 * @details
 * This function is called after a module was processed and its input/output are available in the cache.
 *
 * It performs:
 * - global histogram cache reference update (raw/output/display),
 * - per-module histogram computation,
 * - color picker sampling for the active module (if enabled).
 *
 * It locks the relevant cache entries for reading while sampling.
 */
static void _sample_gui(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, void *input, void **output,
                        const dt_iop_roi_t roi_in, const dt_iop_roi_t roi_out, dt_iop_buffer_dsc_t *input_format,
                        dt_iop_buffer_dsc_t **output_format, dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                        const uint64_t input_hash, const uint64_t hash, const size_t in_bpp, const size_t bpp,
                        dt_pixel_cache_entry_t *const input_entry, dt_pixel_cache_entry_t *const output_entry)
{
  (void)in_bpp;
  (void)bpp;

  if(!(dev->gui_attached && pipe->type == DT_DEV_PIXELPIPE_PREVIEW))
    return;

  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, hash, TRUE, output_entry);
  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, input_hash, TRUE, input_entry);

  // Need to go first because we want module output RGB without color conversion.
  // Gamma outputs uint8_t so we take its input. We want float32.
  dt_iop_roi_t roi;
  dt_pixel_cache_entry_t *entry;
  int64_t buf_hash;

  if(strcmp(module->op, "gamma") == 0)
  {
    roi = roi_in;
    entry = input_entry;
    buf_hash = input_hash;
  }
  else
  {
    roi = roi_out;
    entry = output_entry;
    buf_hash = hash;
  }

  // Copy the cache entry reference to histogram cache.
  pixelpipe_get_histogram_backbuf(dev, roi, entry, piece->module, buf_hash);

  // Sample internal histogram on input and color pickers.
  collect_histogram_on_CPU(pipe, dev, (float *)input, roi_in, input_format, module, piece);
  _sample_color_picker(pipe, dev, (float *)input, input_format, roi_in, output, output_format, roi_out, module, piece);

  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, hash, FALSE, output_entry);
  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, input_hash, FALSE, input_entry);
}

/**
 * @brief Re-sync the global histogram cache references on a pure cache hit.
 *
 * @return TRUE if all required cache lines exist, FALSE if a recompute is needed.
 *
 * @details
 * The preview pipe can exit early if the final output cache entry is valid.
 * When that happens, we still need to update the global histogram backbuffers to point at the right cache
 * entries for `demosaic/colorout/gamma`.
 *
 * If any required cache line is missing, we return FALSE so the caller recomputes the pipeline.
 */
static gboolean _resync_global_histograms(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev)
{
  if(pipe->type != DT_DEV_PIXELPIPE_PREVIEW) return 1;

  GList *pieces = g_list_first(pipe->nodes);
  int64_t input_hash = -1;

  while(pieces)
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)pieces->data;
    if(piece->enabled)
    {
      int64_t hash = piece->global_hash;

      if(_get_backuf(dev, piece->module->op))
      {
        // Gamma outputs uint8_t so we take its input. We want float32.
        dt_iop_roi_t roi;
        dt_pixel_cache_entry_t *entry;
        int64_t buf_hash;

        if(strcmp(piece->module->op, "gamma") == 0)
        {
          roi = piece->planned_roi_in;
          buf_hash = input_hash;
        }
        else
        {
          roi = piece->planned_roi_out;
          buf_hash = hash;
        }

        if(!dt_dev_pixelpipe_cache_get_existing(darktable.pixelpipe_cache, buf_hash, NULL, NULL, &entry))
          return 0;

        pixelpipe_get_histogram_backbuf(dev, roi, entry, piece->module, buf_hash);
      }

      input_hash = hash;
    }

    pieces = g_list_next(pieces);
  }

  return 1;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
