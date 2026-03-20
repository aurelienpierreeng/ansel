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
 * 1) **GUI-observable pipes only**
 *    These helpers are intended for pipes advertising `pipe->gui_observable_source` when
 *    `dev->gui_attached` is true. They must not be invoked for exports or background processing.
 *
 * 2) **Cache entries are the source of truth**
 *    The GUI should never access transient buffers directly. We always sample through cache entries, and the
 *    main pixelpipe recursion is responsible for holding the relevant cache locks while calling these helpers.
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
 * 6) **No buffer ownership here**
 *    This file does not acquire/release cache locks and does not decide whether a cacheline is safe to read.
 *    It assumes the caller already established cache consistency and passes locked cache entries.
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

static const char *_picker_source_name(const dt_pixelpipe_picker_source_t picker_source)
{
  switch(picker_source)
  {
    case PIXELPIPE_PICKER_INPUT:
      return "input";
    case PIXELPIPE_PICKER_OUTPUT:
      return "output";
    default:
      return "unknown";
  }
}

static const char *_picker_cst_name(const dt_iop_colorspace_type_t cst)
{
  switch(cst)
  {
    case IOP_CS_RAW:
      return "RAW";
    case IOP_CS_LAB:
      return "LAB";
    case IOP_CS_RGB:
      return "RGB";
    case IOP_CS_LCH:
      return "LCH";
    case IOP_CS_HSL:
      return "HSL";
    case IOP_CS_JZCZHZ:
      return "JZCZHZ";
    case IOP_CS_NONE:
      return "NONE";
    default:
      return "UNKNOWN";
  }
}

static const char *_picker_datatype_name(const dt_iop_buffer_type_t datatype)
{
  switch(datatype)
  {
    case TYPE_FLOAT:
      return "f32";
    case TYPE_UINT16:
      return "u16";
    case TYPE_UINT8:
      return "u8";
    default:
      return "?";
  }
}

static void _trace_picker_sample(const dt_dev_pixelpipe_t *pipe, const dt_iop_module_t *module,
                                 const dt_dev_pixelpipe_iop_t *piece, const dt_iop_buffer_dsc_t *dsc,
                                 const dt_iop_roi_t *roi, const int *box, const uint64_t hash,
                                 const dt_iop_colorspace_type_t image_cst,
                                 const dt_iop_colorspace_type_t picker_cst,
                                 const dt_pixelpipe_picker_source_t picker_source,
                                 const dt_aligned_pixel_t avg, const dt_aligned_pixel_t min,
                                 const dt_aligned_pixel_t max)
{
  if(!(darktable.unmuted & DT_DEBUG_PIPE)) return;

  dt_print(DT_DEBUG_PIPE,
           "[picker] pipe=%s module=%s piece=%s source=%s hash=%" PRIu64
           " roi=%ix%i+%i+%i box=%i,%i,%i,%i dsc(ch=%u type=%s cst=%s) image_cst=%s picker_cst=%s "
           "avg=%g,%g,%g,%g min=%g,%g,%g,%g max=%g,%g,%g,%g\n",
           pipe ? dt_pixelpipe_get_pipe_name(pipe->type) : "-",
           module ? module->op : "-",
           (piece && piece->module) ? piece->module->op : "-",
           _picker_source_name(picker_source),
           hash,
           roi ? roi->width : 0, roi ? roi->height : 0, roi ? roi->x : 0, roi ? roi->y : 0,
           box ? box[0] : -1, box ? box[1] : -1, box ? box[2] : -1, box ? box[3] : -1,
           dsc ? dsc->channels : 0,
           dsc ? _picker_datatype_name(dsc->datatype) : "-",
           dsc ? _picker_cst_name(dsc->cst) : "-",
           _picker_cst_name(image_cst), _picker_cst_name(picker_cst),
           avg[0], avg[1], avg[2], avg[3],
           min[0], min[1], min[2], min[3],
           max[0], max[1], max[2], max[3]);
}

static void _trace_histogram_backbuf(const dt_dev_pixelpipe_t *pipe, const dt_iop_module_t *module,
                                     const dt_backbuf_t *backbuf, const dt_iop_roi_t roi,
                                     const dt_pixel_cache_entry_t *entry, const uint64_t previous_hash,
                                     const uint64_t hash, const int bpp)
{
  if(!(darktable.unmuted & DT_DEBUG_PIPE)) return;

  dt_print(DT_DEBUG_PIPE,
           "[histogram_backbuf] pipe=%s module=%s prev=%" PRIu64 " new=%" PRIu64
           " entry=%" PRIu64 " roi=%ix%i+%i+%i bpp=%i ready=%i\n",
           pipe ? dt_pixelpipe_get_pipe_name(pipe->type) : "-",
           module ? module->op : "-",
           previous_hash, hash,
           entry ? entry->hash : DT_PIXELPIPE_CACHE_HASH_INVALID,
           roi.width, roi.height, roi.x, roi.y, bpp,
           backbuf ? (dt_dev_backbuf_get_hash((dt_backbuf_t *)backbuf) != DT_PIXELPIPE_CACHE_HASH_INVALID) : 0);
}

/**
 * @brief Compute a histogram for a given module piece.
 *
 * @details
 * This is the per-module histogram that can be shown in module UIs. Each module may set
 * `piece->histogram_params` to define a ROI. If no ROI is specified, we use the full ROI.
 */
static void histogram_collect(dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece, const void *pixel,
                              uint32_t **histogram, uint32_t *histogram_max)
{
  dt_dev_histogram_collection_params_t histogram_params = piece->histogram_params;
  dt_histogram_roi_t histogram_roi;

  // If the current module did not specify its own ROI, use the full ROI.
  if(histogram_params.roi == NULL)
  {
    histogram_roi = (dt_histogram_roi_t){
      .width = piece->roi_in.width, .height = piece->roi_in.height,
      .crop_x = 0, .crop_y = 0, .crop_width = 0, .crop_height = 0
    };

    histogram_params.roi = &histogram_roi;
  }

  const dt_iop_colorspace_type_t cst = piece->dsc_in.cst;

  dt_histogram_helper(&histogram_params, &piece->histogram_stats, cst, piece->module->histogram_cst, pixel, histogram,
                      piece->module->histogram_middle_grey, dt_ioppr_get_pipe_work_profile_info(pipe));
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

static void pixelpipe_get_histogram_backbuf(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, const dt_iop_roi_t roi,
                                            dt_pixel_cache_entry_t *entry, dt_iop_module_t *module,
                                            const uint64_t hash);

static inline gboolean _pipe_tracks_gui_observables(const dt_dev_pixelpipe_t *pipe, const dt_develop_t *dev)
{
  return pipe && dev && dev->gui_attached && pipe->gui_observable_source;
}

static inline gboolean _module_requests_color_picker(const dt_develop_t *dev, const dt_iop_module_t *module)
{
  return dev && module && dev->gui_module && darktable.lib->proxy.colorpicker.picker_proxy
         && module == dev->gui_module && dev->gui_module->enabled
         && dev->gui_module->request_color_pick != DT_REQUEST_COLORPICK_OFF;
}

static inline gboolean _module_requests_input_histogram(const dt_develop_t *dev,
                                                        const dt_dev_pixelpipe_iop_t *piece)
{
  return dev && piece && (piece->request_histogram & DT_REQUEST_ON)
         && (dev->gui_attached || !(piece->request_histogram & DT_REQUEST_ONLY_IN_GUI));
}

static inline gboolean _module_needs_gui_host_input_sampling(const dt_dev_pixelpipe_t *pipe,
                                                             const dt_develop_t *dev,
                                                             const dt_iop_module_t *module,
                                                             const dt_dev_pixelpipe_iop_t *piece)
{
  return _pipe_tracks_gui_observables(pipe, dev)
         && (_module_requests_color_picker(dev, module)
             || _module_requests_input_histogram(dev, piece));
}

static inline gboolean _module_needs_gui_input_backbuf_sync(const dt_dev_pixelpipe_t *pipe,
                                                            const dt_develop_t *dev,
                                                            const dt_iop_module_t *module)
{
  return _pipe_tracks_gui_observables(pipe, dev) && !dt_dev_pixelpipe_get_realtime(pipe)
         && module && !strcmp(module->op, "gamma");
}

/**
 * @brief Tell whether a preview recompute must walk the whole pipe to refresh GUI observables.
 *
 * @details
 * Preview processing is not only about the final image. The darkroom histogram widgets and the module-local
 * color picker observe intermediate stages (`demosaic`, `colorout`, `gamma`, and the active GUI module).
 *
 * When the preview pipe is recomputed in standard mode, we must therefore recurse through the whole pipe even if
 * downstream modules exact-hit in cache:
 * - global histograms need their stage backbuffers refreshed after upstream param/topology changes,
 * - the active module color picker must reach the module that captures it,
 * - exact-hit modules are still allowed to skip pixel recomputation once their upstream recursion completed.
 *
 * Realtime mode keeps its historical behavior and skips the full traversal because global histograms are disabled
 * there on purpose.
 */
static inline gboolean _preview_pipe_needs_observable_traversal(const dt_dev_pixelpipe_t *pipe,
                                                                const dt_develop_t *dev)
{
  return _pipe_tracks_gui_observables(pipe, dev)
         && pipe->type == DT_DEV_PIXELPIPE_PREVIEW
         && !dt_dev_pixelpipe_get_realtime(pipe);
}

static gboolean _pipe_needs_gui_sampling_traversal(const dt_dev_pixelpipe_t *pipe, const dt_develop_t *dev)
{
  if(!_pipe_tracks_gui_observables(pipe, dev) || !pipe->nodes) return FALSE;
  if(_preview_pipe_needs_observable_traversal(pipe, dev)) return TRUE;

  for(GList *pieces = g_list_first(pipe->nodes); pieces; pieces = g_list_next(pieces))
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)pieces->data;
    if(piece && piece->enabled
       && _module_needs_gui_host_input_sampling(pipe, dev, piece->module, piece))
      return TRUE;
  }

  return FALSE;
}

/**
 * @brief Tell whether an exact cache hit must still recurse to reach the active color picker module.
 *
 * @details
 * Moving a picker changes only the sampled coordinates, not the module output hashes.
 * This means downstream modules can exact-hit in cache while an upstream GUI module still
 * needs fresh host-buffer sampling to emit `DT_SIGNAL_CONTROL_PICKERDATA_READY`.
 *
 * If we returned early from those downstream exact hits, the recursion would never reach the
 * active GUI module and the picker would move on screen without updating any parameter.
 */
static inline gboolean _module_exact_hit_must_recurse_for_picker(const dt_dev_pixelpipe_t *pipe,
                                                                 const dt_develop_t *dev,
                                                                 const dt_iop_module_t *module)
{
  return _pipe_tracks_gui_observables(pipe, dev)
         && module
         && dev
         && dev->gui_module
         && darktable.lib->proxy.colorpicker.picker_proxy
         && dev->gui_module->enabled
         && dev->gui_module->request_color_pick != DT_REQUEST_COLORPICK_OFF
         && module != dev->gui_module
         && module->iop_order > dev->gui_module->iop_order;
}

/**
 * @brief Update the global histogram backbuffer to reference a specific cache entry.
 *
 * @details
 * Global histograms are displayed outside of the pixelpipe processing call stack, so we must keep a cache
 * reference (refcount increment) to prevent eviction of the buffer being displayed.
 *
 * The backbuffer metadata must be refreshed every time because the histogram widget only sees this shared state.
 * We only touch cache refcounts when the cache hash changes.
 */
static void pixelpipe_get_histogram_backbuf(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, const dt_iop_roi_t roi,
                                            dt_pixel_cache_entry_t *entry, dt_iop_module_t *module,
                                            const uint64_t hash)
{
  dt_backbuf_t *backbuf = _get_backuf(dev, module->op);
  if(backbuf == NULL) return;     // This module is not wired to global histograms.

  // Update metadata first so the histogram widget always sees the current ROI/format, even on pure cache hits.
  const size_t entry_size = dt_pixel_cache_entry_get_size(entry);
  const int bpp = (roi.width > 0 && roi.height > 0) ? (int)(entry_size / ((size_t)roi.width * (size_t)roi.height)) : 0;
  const int64_t previous_hash = dt_dev_backbuf_get_hash(backbuf);
  dt_dev_set_backbuf(backbuf, roi.width, roi.height, bpp, hash, -1);
  _trace_histogram_backbuf(pipe, module, backbuf, roi, entry, previous_hash, hash, bpp);

  if(previous_hash == hash) return; // Same cacheline, metadata is already refreshed above.

  // Hash has changed, our previous stored entry is obsolete: decrement its refcount.
  dt_pixel_cache_entry_t *previous_entry;
  if(dt_dev_pixelpipe_cache_peek(darktable.pixelpipe_cache, previous_hash, NULL, &previous_entry, -1, NULL))
    dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, FALSE, previous_entry);

  // Increment the refcount on current entry so nobody removes it while the GUI still needs it.
  dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, TRUE, entry);
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
  const int width = roi.width;
  const int height = roi.height;
  const dt_colorpicker_sample_t *const sample = darktable.lib->proxy.colorpicker.primary_sample;

  dt_boundingbox_t fbox = { 0.0f };

  // Get absolute pixel coordinates in final preview image.
  if(sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
  {
    memcpy(fbox, sample->box, sizeof(float) * 4);
    dt_dev_coordinates_image_norm_to_preview_abs(darktable.develop, fbox, 2);
  }
  else if(sample->size == DT_LIB_COLORPICKER_SIZE_POINT)
  {
    fbox[0] = sample->point[0];
    fbox[1] = sample->point[1];
    dt_dev_coordinates_image_norm_to_preview_abs(darktable.develop, fbox, 1);
    fbox[2] = fbox[0];
    fbox[3] = fbox[1];
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

  if((darktable.unmuted & DT_DEBUG_PIPE) && (darktable.unmuted & DT_DEBUG_VERBOSE))
  {
    dt_print(DT_DEBUG_PIPE,
             "[picker_box] module=%s source=%s roi=%ix%i+%i+%i box=%i,%i,%i,%i sample=%s\n",
             module ? module->op : "-",
             _picker_source_name(picker_source),
             roi.width, roi.height, roi.x, roi.y,
             box[0], box[1], box[2], box[3],
             sample->size == DT_LIB_COLORPICKER_SIZE_BOX ? "box" : "point");
  }

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
static void pixelpipe_picker(dt_dev_pixelpipe_t *pipe, dt_iop_module_t *module,
                             dt_dev_pixelpipe_iop_t *piece, dt_iop_buffer_dsc_t *dsc,
                             const float *pixel, const dt_iop_roi_t roi, float *picked_color,
                             float *picked_color_min, float *picked_color_max,
                             const dt_iop_colorspace_type_t image_cst, dt_pixelpipe_picker_source_t picker_source,
                             const uint64_t hash)
{
  int box[4];
  if(pixelpipe_picker_helper(module, roi, picked_color, picked_color_min, picked_color_max, picker_source, box))
    return;

  dt_aligned_pixel_t avg = { 0.0f };
  dt_aligned_pixel_t min = { 0.0f };
  dt_aligned_pixel_t max = { 0.0f };

  const dt_iop_order_iccprofile_info_t *const profile = dt_ioppr_get_pipe_work_profile_info(pipe);
  const dt_iop_colorspace_type_t picker_cst = dt_iop_color_picker_get_active_cst(module);

  dt_color_picker_helper(dsc, pixel, &roi, box, avg, min, max, image_cst, picker_cst, profile);
  _trace_picker_sample(pipe, module, piece, dsc, &roi, box, hash, image_cst, picker_cst,
                       picker_source, avg, min, max);

  for(int k = 0; k < 4; k++)
  {
    picked_color_min[k] = min[k];
    picked_color_max[k] = max[k];
    picked_color[k] = avg[k];
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
                                     float *input, dt_iop_buffer_dsc_t *input_format,
                                     dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece)
{
  (void)pipe;
  (void)input_format;
  // Histogram collection for module.
  if((dev->gui_attached || !(piece->request_histogram & DT_REQUEST_ONLY_IN_GUI))
     && (piece->request_histogram & DT_REQUEST_ON))
  {
    histogram_collect(pipe, piece, input, &(piece->histogram), piece->histogram_max);

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
 * Sampling happens while the relevant cache lines are locked for reading, therefore we must never rewrite those
 * buffers here. The helper reads the buffers in their native colorspace and only applies conversions it natively
 * supports while aggregating the picker values.
 */
static void _sample_color_picker(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, float *input,
                                 void **output,
                                 dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                                 dt_pixel_cache_entry_t *input_entry, dt_pixel_cache_entry_t *output_entry,
                                 const uint64_t input_hash, const uint64_t output_hash)
{

  if(!(darktable.lib->proxy.colorpicker.picker_proxy
       && module == dev->gui_module
       && dev->gui_module->enabled
       && module->request_color_pick != DT_REQUEST_COLORPICK_OFF))
    return;

  pixelpipe_picker(pipe, module, piece, &input_entry->dsc, input, piece->roi_in, module->picked_color,
                   module->picked_color_min, module->picked_color_max, input_entry->dsc.cst,
                   PIXELPIPE_PICKER_INPUT, input_hash);
  pixelpipe_picker(pipe, module, piece, &output_entry->dsc, (float *)(*output), piece->roi_out,
                   module->picked_output_color,
                   module->picked_output_color_min, module->picked_output_color_max,
                   output_entry->dsc.cst, PIXELPIPE_PICKER_OUTPUT, output_hash);

  // Sampling happens here while the pipeline still owns the relevant cache locks.
  // The module callback itself is deferred until the pipeline thread released its
  // recursion-owned state, because it may commit history and restart both pipes.
  pipe->pending_picker_module = module;
  pipe->pending_picker_piece = piece;
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
 * The caller must already hold pixel cache locks protecting `input_entry` and `output_entry`.
 */
static void _sample_gui(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, void *input, void **output,
                        dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                        const uint64_t input_hash, const uint64_t hash,
                        dt_pixel_cache_entry_t *const input_entry, dt_pixel_cache_entry_t *const output_entry)
{
  if(!_pipe_tracks_gui_observables(pipe, dev))
    return;

  if(darktable.unmuted & DT_DEBUG_PIPE)
  {
    dt_print(DT_DEBUG_PIPE,
             "[sample_gui] pipe=%s module=%s input=%" PRIu64 " output=%" PRIu64
             " input_dsc(ch=%u type=%s cst=%s) output_dsc(ch=%u type=%s cst=%s)\n",
             pipe ? dt_pixelpipe_get_pipe_name(pipe->type) : "-",
             module ? module->op : "-",
             input_hash, hash,
             input_entry ? input_entry->dsc.channels : 0,
             input_entry ? _picker_datatype_name(input_entry->dsc.datatype) : "-",
             input_entry ? _picker_cst_name(input_entry->dsc.cst) : "-",
             output_entry ? output_entry->dsc.channels : 0,
             output_entry ? _picker_datatype_name(output_entry->dsc.datatype) : "-",
             output_entry ? _picker_cst_name(output_entry->dsc.cst) : "-");
  }

  // GUI observables only sample the cachelines protected by the caller.
  float *const sampled_input = input_entry ? (float *)input_entry->data : NULL;
  void *sampled_output = output_entry ? output_entry->data : NULL;

  // Need to go first because we want module output RGB without color conversion.
  // Gamma outputs uint8_t so we take its input. We want float32.
  dt_iop_roi_t roi;
  dt_pixel_cache_entry_t *entry;
  int64_t buf_hash;

  if(strcmp(module->op, "gamma") == 0)
  {
    roi = piece->roi_in;
    entry = input_entry;
    buf_hash = input_hash;
  }
  else
  {
    roi = piece->roi_out;
    entry = output_entry;
    buf_hash = hash;
  }

  // Copy the cache entry reference to global histogram cache, except in
  // realtime mode where we intentionally skip global histogram sampling.
  if(!dt_dev_pixelpipe_get_realtime(pipe))
    pixelpipe_get_histogram_backbuf(pipe, dev, roi, entry, piece->module, buf_hash);

  // Sample internal histogram on input and color pickers from the locked host cachelines only.
  if(sampled_input && input_entry && input_entry->dsc.datatype == TYPE_FLOAT)
    collect_histogram_on_CPU(pipe, dev, sampled_input, &input_entry->dsc, module, piece);

  if(sampled_input && sampled_output
     && input_entry && output_entry
     && input_entry->dsc.datatype == TYPE_FLOAT
     && output_entry->dsc.datatype == TYPE_FLOAT)
    _sample_color_picker(pipe, dev, sampled_input, &sampled_output, module, piece,
                         input_entry, output_entry, input_hash, hash);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
