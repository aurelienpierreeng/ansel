/*
    This file was part of darktable,
    This file is part of Ansel,
    Copyright (C) 2009-2021 darktable developers.
    Copyright (C) 2023 Aurélien Pierre.

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
#include "common/color_picker.h"
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/histogram.h"
#include "common/imageio.h"
#include "common/opencl.h"
#include "common/iop_order.h"
#include "control/control.h"
#include "control/conf.h"
#include "control/signal.h"
#include "develop/blend.h"
#include "develop/format.h"
#include "develop/imageop_math.h"
#include "develop/pixelpipe.h"
#include "develop/tiling.h"
#include "develop/masks.h"
#include "gui/gtk.h"
#include "libs/colorpicker.h"
#include "libs/lib.h"
#include "gui/color_picker_proxy.h"

#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

typedef enum dt_pixelpipe_flow_t
{
  PIXELPIPE_FLOW_NONE = 0,
  PIXELPIPE_FLOW_HISTOGRAM_NONE = 1 << 0,
  PIXELPIPE_FLOW_HISTOGRAM_ON_CPU = 1 << 1,
  PIXELPIPE_FLOW_HISTOGRAM_ON_GPU = 1 << 2,
  PIXELPIPE_FLOW_PROCESSED_ON_CPU = 1 << 3,
  PIXELPIPE_FLOW_PROCESSED_ON_GPU = 1 << 4,
  PIXELPIPE_FLOW_PROCESSED_WITH_TILING = 1 << 5,
  PIXELPIPE_FLOW_BLENDED_ON_CPU = 1 << 6,
  PIXELPIPE_FLOW_BLENDED_ON_GPU = 1 << 7
} dt_pixelpipe_flow_t;

typedef enum dt_pixelpipe_picker_source_t
{
  PIXELPIPE_PICKER_INPUT = 0,
  PIXELPIPE_PICKER_OUTPUT = 1
} dt_pixelpipe_picker_source_t;

#include "develop/pixelpipe_cache.c"

static void get_output_format(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                              dt_develop_t *dev, dt_iop_buffer_dsc_t *dsc);

char *dt_pixelpipe_get_pipe_name(dt_dev_pixelpipe_type_t pipe_type)
{
  char *r = NULL;

  switch(pipe_type & DT_DEV_PIXELPIPE_ANY)
  {
    case DT_DEV_PIXELPIPE_PREVIEW:
      r = _("PREVIEW");
      break;
    case DT_DEV_PIXELPIPE_FULL:
      r = _("FULL");
      break;
    case DT_DEV_PIXELPIPE_THUMBNAIL:
      r = _("THUMBNAIL");
      break;
    case DT_DEV_PIXELPIPE_EXPORT:
      r = _("EXPORT");
      break;
    default:
      r = _("UNKNOWN");
  }
  return r;
}

inline static void _copy_buffer(const char *const restrict input, char *const restrict output,
                                const size_t height, const size_t o_width, const size_t i_width,
                                const size_t x_offset, const size_t y_offset,
                                const size_t stride, const size_t bpp)
{
#ifdef _OPENMP
#pragma omp parallel for default(none) \
          dt_omp_firstprivate(input, output, bpp, o_width, i_width, height, x_offset, y_offset, stride) \
          schedule(static)
#endif
  for(size_t j = 0; j < height; j++)
    memcpy(__builtin_assume_aligned(output, 64) + bpp * j * o_width,
           __builtin_assume_aligned(input, 64) + bpp * (x_offset + (y_offset + j) * i_width),
           stride);
}


inline static void _uint8_to_float(const uint8_t *const input, float *const output,
                                   const size_t width, const size_t height, const size_t chan)
{
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
        aligned(input, output: 64) \
        dt_omp_firstprivate(input, output, width, height, chan) \
        schedule(static)
#endif
  for(size_t k = 0; k < height * width; k++)
  {
    const size_t index = k * chan;
    // Warning: we take BGRa and put it back into RGBa
    output[index + 0] = (float)input[index + 2] / 255.f;
    output[index + 1] = (float)input[index + 1] / 255.f;
    output[index + 2] = (float)input[index + 0] / 255.f;
    output[index + 3] = 0.f;
  }
}


int dt_dev_pixelpipe_init_export(dt_dev_pixelpipe_t *pipe, int32_t width, int32_t height, int levels,
                                 gboolean store_masks)
{
  const int res = dt_dev_pixelpipe_init_cached(pipe);
  pipe->type = DT_DEV_PIXELPIPE_EXPORT;
  pipe->levels = levels;
  pipe->store_all_raster_masks = store_masks;
  return res;
}

int dt_dev_pixelpipe_init_thumbnail(dt_dev_pixelpipe_t *pipe, int32_t width, int32_t height)
{
  const int res = dt_dev_pixelpipe_init_cached(pipe);
  pipe->type = DT_DEV_PIXELPIPE_THUMBNAIL;
  return res;
}

int dt_dev_pixelpipe_init_dummy(dt_dev_pixelpipe_t *pipe, int32_t width, int32_t height)
{
  const int res = dt_dev_pixelpipe_init_cached(pipe);
  pipe->type = DT_DEV_PIXELPIPE_THUMBNAIL;
  return res;
}

int dt_dev_pixelpipe_init_preview(dt_dev_pixelpipe_t *pipe)
{
  // Init with the size of MIPMAP_F
  const int res = dt_dev_pixelpipe_init_cached(pipe);
  pipe->type = DT_DEV_PIXELPIPE_PREVIEW;

  // Needed for caching
  pipe->store_all_raster_masks = TRUE;
  return res;
}

int dt_dev_pixelpipe_init(dt_dev_pixelpipe_t *pipe)
{
  const int res = dt_dev_pixelpipe_init_cached(pipe);
  pipe->type = DT_DEV_PIXELPIPE_FULL;

  // Needed for caching
  pipe->store_all_raster_masks = TRUE;
  return res;
}

int dt_dev_pixelpipe_init_cached(dt_dev_pixelpipe_t *pipe)
{
  pipe->devid = -1;
  pipe->changed = DT_DEV_PIPE_UNCHANGED;
  pipe->processed_width = pipe->backbuf_width = pipe->iwidth = 0;
  pipe->processed_height = pipe->backbuf_height = pipe->iheight = 0;
  pipe->nodes = NULL;
  pipe->backbuf = NULL;
  pipe->hash = 0;
  pipe->history_hash = 0;
  pipe->backbuf_pipe_hash = 0;
  pipe->backbuf_hist_hash = 0;
  pipe->backbuf_timestamp = 0;
  pipe->resync_timestamp = 0;
  pipe->bypass_cache = 0;

  pipe->output_backbuf = NULL;
  pipe->output_backbuf_width = 0;
  pipe->output_backbuf_height = 0;
  pipe->output_imgid = UNKNOWN_IMAGE;

  pipe->rawdetail_mask_data = NULL;
  pipe->want_detail_mask = DT_DEV_DETAIL_MASK_NONE;

  pipe->processing = 0;
  pipe->running = 0;
  dt_atomic_set_int(&pipe->shutdown, FALSE);
  pipe->opencl_error = 0;
  pipe->tiling = 0;
  pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
  pipe->bypass_blendif = 0;
  pipe->input_timestamp = 0;
  pipe->levels = IMAGEIO_RGB | IMAGEIO_INT8;
  dt_pthread_mutex_init(&(pipe->backbuf_mutex), NULL);
  dt_pthread_mutex_init(&(pipe->busy_mutex), NULL);
  pipe->icc_type = DT_COLORSPACE_NONE;
  pipe->icc_filename = NULL;
  pipe->icc_intent = DT_INTENT_LAST;
  pipe->iop = NULL;
  pipe->iop_order_list = NULL;
  pipe->forms = NULL;
  pipe->store_all_raster_masks = FALSE;
  pipe->work_profile_info = NULL;
  pipe->input_profile_info = NULL;
  pipe->output_profile_info = NULL;

  pipe->status = DT_DEV_PIXELPIPE_DIRTY;
  pipe->last_history_hash = 0;
  pipe->flush_cache = FALSE;
  pipe->timeout = 0;

  dt_dev_pixelpipe_reset_reentry(pipe);
  return 1;
}

void dt_dev_pixelpipe_set_input(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, int32_t imgid, int width, int height,
                                dt_mipmap_size_t size)
{
  pipe->iwidth = width;
  pipe->iheight = height;
  pipe->imgid = imgid;
  pipe->image = dev->image_storage;
  pipe->size = size;

  dt_dev_pixelpipe_reset_reentry(pipe);
  get_output_format(NULL, pipe, NULL, dev, &pipe->dsc);
}

void dt_dev_pixelpipe_set_icc(dt_dev_pixelpipe_t *pipe, dt_colorspaces_color_profile_type_t icc_type,
                              const gchar *icc_filename, dt_iop_color_intent_t icc_intent)
{
  pipe->icc_type = icc_type;
  g_free(pipe->icc_filename);
  pipe->icc_filename = g_strdup(icc_filename ? icc_filename : "");
  pipe->icc_intent = icc_intent;
}

void dt_dev_pixelpipe_cleanup(dt_dev_pixelpipe_t *pipe)
{
  dt_pthread_mutex_lock(&pipe->backbuf_mutex);
  pipe->backbuf = NULL;
  // blocks while busy and sets shutdown bit:
  dt_dev_pixelpipe_cleanup_nodes(pipe);
  // so now it's safe to clean up cache:
  dt_pthread_mutex_unlock(&pipe->backbuf_mutex);
  dt_pthread_mutex_destroy(&(pipe->backbuf_mutex));
  dt_pthread_mutex_destroy(&(pipe->busy_mutex));
  pipe->icc_type = DT_COLORSPACE_NONE;
  g_free(pipe->icc_filename);
  pipe->icc_filename = NULL;

  g_free(pipe->output_backbuf);
  pipe->output_backbuf = NULL;
  pipe->output_backbuf_width = 0;
  pipe->output_backbuf_height = 0;
  pipe->output_imgid = UNKNOWN_IMAGE;

  dt_dev_clear_rawdetail_mask(pipe);

  if(pipe->forms)
  {
    g_list_free_full(pipe->forms, (void (*)(void *))dt_masks_free_form);
    pipe->forms = NULL;
  }
}


gboolean dt_dev_pixelpipe_set_reentry(dt_dev_pixelpipe_t *pipe, uint64_t hash)
{
  if(pipe->reentry_hash == 0)
  {
    pipe->reentry = TRUE;
    pipe->reentry_hash = hash;
    dt_print(DT_DEBUG_DEV, "[dev_pixelpipe] re-entry flag set for %" PRIu64 "\n", hash);
    return TRUE;
  }

  return FALSE;
}


gboolean dt_dev_pixelpipe_unset_reentry(dt_dev_pixelpipe_t *pipe, uint64_t hash)
{
  if(pipe->reentry_hash == hash)
  {
    pipe->reentry = FALSE;
    pipe->reentry_hash = 0;
    dt_print(DT_DEBUG_DEV, "[dev_pixelpipe] re-entry flag unset for %" PRIu64 "\n", hash);
    return TRUE;
  }

  return FALSE;
}

gboolean dt_dev_pixelpipe_has_reentry(dt_dev_pixelpipe_t *pipe)
{
  return pipe->reentry;
}

void dt_dev_pixelpipe_reset_reentry(dt_dev_pixelpipe_t *pipe)
{
  pipe->reentry = FALSE;
  pipe->reentry_hash = 0;
  pipe->flush_cache = FALSE;
  dt_print(DT_DEBUG_DEV, "[dev_pixelpipe] re-entry flag reset\n");
}

void dt_dev_pixelpipe_cleanup_nodes(dt_dev_pixelpipe_t *pipe)
{
  // destroy all nodes
  for(GList *nodes = pipe->nodes; nodes; nodes = g_list_next(nodes))
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
    // printf("cleanup module `%s'\n", piece->module->name());
    piece->module->cleanup_pipe(piece->module, pipe, piece);
    free(piece->blendop_data);
    piece->blendop_data = NULL;
    free(piece->histogram);
    piece->histogram = NULL;
    dt_pixelpipe_raster_cleanup(piece->raster_masks);
    free(piece);
  }
  g_list_free(pipe->nodes);
  pipe->nodes = NULL;
  // also cleanup iop here
  if(pipe->iop)
  {
    g_list_free(pipe->iop);
    pipe->iop = NULL;
  }
  // and iop order
  g_list_free_full(pipe->iop_order_list, free);
  pipe->iop_order_list = NULL;
}

void dt_dev_pixelpipe_create_nodes(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev)
{
  // check that the pipe was actually properly cleaned up after the last run
  g_assert(pipe->nodes == NULL);
  g_assert(pipe->iop == NULL);
  g_assert(pipe->iop_order_list == NULL);
  pipe->iop_order_list = dt_ioppr_iop_order_copy_deep(dev->iop_order_list);

  // for all modules in dev:
  // TODO: don't add deprecated modules that are not enabled are not added to pipeline.
  // currently, that loads 84 modules of which a solid third are not used anymore.
  // if(module->flags() & IOP_FLAGS_DEPRECATED && !(module->enabled)) continue;
  pipe->iop = g_list_copy(dev->iop);
  for(GList *modules = pipe->iop; modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)calloc(1, sizeof(dt_dev_pixelpipe_iop_t));
    piece->enabled = module->enabled;
    piece->request_histogram = DT_REQUEST_ONLY_IN_GUI;
    piece->histogram_params.roi = NULL;
    piece->histogram_params.bins_count = 256;
    piece->histogram_stats.bins_count = 0;
    piece->histogram_stats.pixels = 0;
    piece->colors
        = ((module->default_colorspace(module, pipe, NULL) == IOP_CS_RAW) && (dt_image_is_raw(&pipe->image)))
              ? 1
              : 4;
    piece->iwidth = pipe->iwidth;
    piece->iheight = pipe->iheight;
    piece->module = module;
    piece->pipe = pipe;
    piece->data = NULL;
    piece->hash = 0;
    piece->blendop_hash = 0;
    piece->global_hash = 0;
    piece->global_mask_hash = 0;
    piece->bypass_cache = FALSE;
    piece->force_opencl_cache = TRUE;
    piece->process_cl_ready = 0;
    piece->process_tiling_ready = 0;
    piece->raster_masks = dt_pixelpipe_raster_alloc();
    memset(&piece->processed_roi_in, 0, sizeof(piece->processed_roi_in));
    memset(&piece->processed_roi_out, 0, sizeof(piece->processed_roi_out));

    // dsc_mask is static, single channel float image
    memset(&piece->dsc_mask, 0, sizeof(piece->dsc_mask));
    piece->dsc_mask.channels = 1;
    piece->dsc_mask.datatype = TYPE_FLOAT;
    piece->dsc_mask.filters = 0;

    dt_iop_init_pipe(piece->module, pipe, piece);
    pipe->nodes = g_list_append(pipe->nodes, piece);
  }
}

static void get_output_format(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                              dt_develop_t *dev, dt_iop_buffer_dsc_t *dsc)
{
  if(module) return module->output_format(module, pipe, piece, dsc);

  // first input.
  *dsc = pipe->image.buf_dsc;

  if(!(dt_image_is_raw(&pipe->image)))
  {
    // image max is normalized before
    for(int k = 0; k < 4; k++) dsc->processed_maximum[k] = 1.0f;
  }
}


// helper to get per module histogram
static void histogram_collect(dt_dev_pixelpipe_iop_t *piece, const void *pixel, const dt_iop_roi_t roi,
                              uint32_t **histogram, uint32_t *histogram_max)
{
  dt_dev_histogram_collection_params_t histogram_params = piece->histogram_params;

  dt_histogram_roi_t histogram_roi;

  // if the current module does did not specified its own ROI, use the full ROI
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

dt_backbuf_t * _get_backuf(dt_develop_t *dev, const char *op)
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

static void pixelpipe_get_histogram_backbuf(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev,
                                            void *output, const dt_iop_roi_t roi,
                                            dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                                            const uint64_t hash, const size_t bpp)
{
  // Runs only on full image but downscaled for perf, aka preview pipe
  // Not an RGBa float buffer ?
  if(!(bpp == 4 * sizeof(float))) return;

  dt_backbuf_t *backbuf = _get_backuf(dev, module->op);
  if(backbuf == NULL) return; // This module is not wired to global histograms
  if(backbuf->hash == hash) return; // Hash didn't change, nothing to update.

  // Prepare the buffer if needed
  if(backbuf->buffer == NULL)
  {
    // Buffer uninited
    backbuf->buffer = dt_alloc_align(roi.width * roi.height * bpp);
    if(backbuf->buffer == NULL)
    {
      // Out of memory to allocate. Notify histogram
      backbuf->hash = -1;
      return;
    }

    backbuf->height = roi.height;
    backbuf->width = roi.width;
    backbuf->bpp = bpp;
  }
  else if((backbuf->height != roi.height) || (backbuf->width != roi.width) || (backbuf->bpp != bpp))
  {
    // Cached buffer size doesn't match current one.
    // There is no reason yet why this should happen because the preview pipe doesn't change size during its lifetime.
    // But let's future-proof it in case someone gets creative.
    if(backbuf->buffer) dt_free_align(backbuf->buffer); // maybe write a dt_realloc_align routine ?
    backbuf->buffer = dt_alloc_align(roi.width * roi.height * bpp);
    if(backbuf->buffer == NULL)
    {
      // Out of memory to allocate. Notify histogram
      backbuf->hash = -1;
      return;
    }

    backbuf->height = roi.height;
    backbuf->width = roi.width;
    backbuf->bpp = bpp;
  }

  if(backbuf->buffer == NULL)
  {
    // Out of memory to allocate. Notify histogram
    backbuf->hash = -1;
    return;
  }

  // Integrity hash, mixing interal module params state, and params states of previous modules in pipe.
  backbuf->hash = hash;

  // Copy to histogram cache
  dt_times_t start;
  dt_get_times(&start);

  if(output)
    _copy_buffer(output, (char *)backbuf->buffer, roi.height, roi.width, roi.width, 0, 0, roi.width * bpp, bpp);
  else
    backbuf->hash = -1;

  dt_show_times_f(&start, "[dev_pixelpipe]", "copying global histogram for %s", module->op);

  // That's all. From there, histogram catches the "preview pipeline finished recomputing" signal and redraws if needed.
  // We don't manage thread locks because there is only one writing point and one reading point, synchronized
  // through signal & callback.

  // Note that we don't compute the histogram here because, depending on the type of scope requested in GUI,
  // intermediate color conversions might be needed (vectorscope) or various pixel binnings required (waveform).
  // Color conversions and binning are deferred to the GUI thread, prior to drawing update.
}


// helper for per-module color picking
static int pixelpipe_picker_helper(dt_iop_module_t *module, const dt_iop_roi_t roi, dt_aligned_pixel_t picked_color,
                                   dt_aligned_pixel_t picked_color_min, dt_aligned_pixel_t picked_color_max,
                                   dt_pixelpipe_picker_source_t picker_source, int *box)
{
  const float wd = darktable.develop->preview_pipe->backbuf_width;
  const float ht = darktable.develop->preview_pipe->backbuf_height;
  const int width = roi.width;
  const int height = roi.height;
  const dt_colorpicker_sample_t *const sample = darktable.lib->proxy.colorpicker.primary_sample;

  dt_boundingbox_t fbox = { 0.0f };

  // get absolute pixel coordinates in final preview image
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

  // transform back to current module coordinates
  dt_dev_distort_backtransform_plus(darktable.develop, darktable.develop->preview_pipe, module->iop_order,
                               ((picker_source == PIXELPIPE_PICKER_INPUT) ? DT_DEV_TRANSFORM_DIR_FORW_INCL
                               : DT_DEV_TRANSFORM_DIR_FORW_EXCL),fbox, 2);

  fbox[0] -= roi.x;
  fbox[1] -= roi.y;
  fbox[2] -= roi.x;
  fbox[3] -= roi.y;

  // re-order edges of bounding box
  box[0] = fminf(fbox[0], fbox[2]);
  box[1] = fminf(fbox[1], fbox[3]);
  box[2] = fmaxf(fbox[0], fbox[2]);
  box[3] = fmaxf(fbox[1], fbox[3]);

  if(sample->size == DT_LIB_COLORPICKER_SIZE_POINT)
  {
    // if we are sampling one point, make sure that we actually sample it.
    for(int k = 2; k < 4; k++) box[k] += 1;
  }

  // do not continue if box is completely outside of roi
  if(box[0] >= width || box[1] >= height || box[2] < 0 || box[3] < 0) return 1;

  // clamp bounding box to roi
  for(int k = 0; k < 4; k += 2) box[k] = MIN(width - 1, MAX(0, box[k]));
  for(int k = 1; k < 4; k += 2) box[k] = MIN(height - 1, MAX(0, box[k]));

  // safety check: area needs to have minimum 1 pixel width and height
  if(box[2] - box[0] < 1 || box[3] - box[1] < 1) return 1;

  return 0;
}

static void pixelpipe_picker(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_iop_buffer_dsc_t *dsc,
                             const float *pixel, const dt_iop_roi_t roi, float *picked_color,
                             float *picked_color_min, float *picked_color_max,
                             const dt_iop_colorspace_type_t image_cst, dt_pixelpipe_picker_source_t picker_source)
{
  int box[4] = { 0 };

  if(pixelpipe_picker_helper(module, roi, picked_color, picked_color_min, picked_color_max, picker_source, box))
  {
    for(int k = 0; k < 4; k++)
    {
      picked_color_min[k] = INFINITY;
      picked_color_max[k] = -INFINITY;
      picked_color[k] = 0.0f;
    }

    return;
  }

  dt_aligned_pixel_t min, max, avg;
  for(int k = 0; k < 4; k++)
  {
    min[k] = INFINITY;
    max[k] = -INFINITY;
    avg[k] = 0.0f;
  }

  const dt_iop_order_iccprofile_info_t *const profile = dt_ioppr_get_pipe_current_profile_info(module, piece->pipe);
  dt_color_picker_helper(dsc, pixel, &roi, box, avg, min, max, image_cst,
                         dt_iop_color_picker_get_active_cst(module), profile);

  for(int k = 0; k < 4; k++)
  {
    picked_color_min[k] = min[k];
    picked_color_max[k] = max[k];
    picked_color[k] = avg[k];
  }
}

// returns 1 if blend process need the module default colorspace
static gboolean _transform_for_blend(const dt_iop_module_t *const self, const dt_dev_pixelpipe_iop_t *const piece)
{
  const dt_develop_blend_params_t *const d = (const dt_develop_blend_params_t *)piece->blendop_data;
  if(d)
  {
    // check only if blend is active
    if((self->flags() & IOP_FLAGS_SUPPORTS_BLENDING) && (d->mask_mode != DEVELOP_MASK_DISABLED))
    {
      return TRUE;
    }
  }
  return FALSE;
}

static dt_iop_colorspace_type_t _transform_for_picker(dt_iop_module_t *self, const dt_iop_colorspace_type_t cst)
{
  const dt_iop_colorspace_type_t picker_cst =
    dt_iop_color_picker_get_active_cst(self);

  switch(picker_cst)
  {
    case IOP_CS_RAW:
      return IOP_CS_RAW;
    case IOP_CS_LAB:
    case IOP_CS_LCH:
      return IOP_CS_LAB;
    case IOP_CS_RGB:
    case IOP_CS_HSL:
    case IOP_CS_JZCZHZ:
      return IOP_CS_RGB;
    case IOP_CS_NONE:
      // IOP_CS_NONE is used by temperature.c as it may work in RAW or RGB
      // return the pipe color space to avoid any additional conversions
      return cst;
    default:
      return picker_cst;
  }
}

static void collect_histogram_on_CPU(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev,
                                     float *input, const dt_iop_roi_t roi_in,
                                     dt_iop_buffer_dsc_t *input_format,
                                     dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece)
{
  // histogram collection for module
  if((dev->gui_attached || !(piece->request_histogram & DT_REQUEST_ONLY_IN_GUI))
     && (piece->request_histogram & DT_REQUEST_ON))
  {
    const dt_iop_order_iccprofile_info_t *const work_profile
        = (input_format->cst != IOP_CS_RAW) ? dt_ioppr_get_pipe_work_profile_info(pipe) : NULL;

    // transform to module input colorspace
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
  return;
}

#define KILL_SWITCH_ABORT                                                                                         \
  if(dt_atomic_get_int(&pipe->shutdown))                                                                          \
  {                                                                                                               \
    if(*cl_mem_output != NULL)                                                                                    \
    {                                                                                                             \
      dt_opencl_release_mem_object(*cl_mem_output);                                                               \
      *cl_mem_output = NULL;                                                                                      \
    }                                                                                                             \
    dt_iop_nap(5000);                                                                                             \
    pipe->status = DT_DEV_PIXELPIPE_DIRTY;                                                                        \
    return 1;                                                                                                     \
  }

// Once we have a cache, stopping computation before full completion
// has good chances of leaving it corrupted. So we invalidate it.
#define KILL_SWITCH_AND_FLUSH_CACHE                                                                               \
  if(dt_atomic_get_int(&pipe->shutdown))                                                                          \
  {                                                                                                               \
    dt_dev_pixelpipe_cache_remove(darktable.pixelpipe_cache, hash, TRUE, output_entry);                           \
    *output = NULL;                                                                                               \
    if(*cl_mem_output != NULL)                                                                                    \
    {                                                                                                             \
      dt_opencl_release_mem_object(*cl_mem_output);                                                               \
      *cl_mem_output = NULL;                                                                                      \
    }                                                                                                             \
    dt_iop_nap(5000);                                                                                             \
    pipe->status = DT_DEV_PIXELPIPE_DIRTY;                                                                        \
    return 1;                                                                                                     \
  }

static int pixelpipe_process_on_CPU(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev,
                                    float *input, dt_iop_buffer_dsc_t *input_format, const dt_iop_roi_t *roi_in,
                                    void **output, dt_iop_buffer_dsc_t **out_format, const dt_iop_roi_t *roi_out,
                                    dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                                    dt_develop_tiling_t *tiling, dt_pixelpipe_flow_t *pixelpipe_flow,
                                    dt_pixel_cache_entry_t *input_entry)
{
  // Fetch RGB working profile
  // if input is RAW, we can't color convert because RAW is not in a color space
  // so we send NULL to by-pass
  const dt_iop_order_iccprofile_info_t *const work_profile
      = (input_format->cst != IOP_CS_RAW) ? dt_ioppr_get_pipe_work_profile_info(pipe) : NULL;

  // transform to module input colorspace
  dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, 0, TRUE, input_entry);
  dt_ioppr_transform_image_colorspace(module, input, input, roi_in->width, roi_in->height, input_format->cst,
                                      module->input_colorspace(module, pipe, piece), &input_format->cst,
                                      work_profile);
  dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, 0, FALSE, input_entry);

  //fprintf(stdout, "input color space for %s : %i\n", module->op, module->input_colorspace(module, pipe, piece));
  const size_t in_bpp = dt_iop_buffer_dsc_to_bpp(input_format);
  const size_t bpp = dt_iop_buffer_dsc_to_bpp(*out_format);

  const gboolean fitting = dt_tiling_piece_fits_host_memory(MAX(roi_in->width, roi_out->width),
                                       MAX(roi_in->height, roi_out->height), MAX(in_bpp, bpp),
                                          tiling->factor, tiling->overhead);

  /* process module on cpu. use tiling if needed and possible. */
  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, 0, TRUE, input_entry);
  if(!fitting && piece->process_tiling_ready)
  {
    module->process_tiling(module, piece, input, *output, roi_in, roi_out, in_bpp);
    *pixelpipe_flow |= (PIXELPIPE_FLOW_PROCESSED_ON_CPU | PIXELPIPE_FLOW_PROCESSED_WITH_TILING);
    *pixelpipe_flow &= ~(PIXELPIPE_FLOW_PROCESSED_ON_GPU);
  }
  else
  {
    if(!fitting)
      fprintf(stderr, "[pixelpipe_process_on_CPU] Warning: processes `%s' even if memory requirements are not met\n", module->op);

    module->process(module, piece, input, *output, roi_in, roi_out);
    *pixelpipe_flow |= (PIXELPIPE_FLOW_PROCESSED_ON_CPU);
    *pixelpipe_flow &= ~(PIXELPIPE_FLOW_PROCESSED_ON_GPU | PIXELPIPE_FLOW_PROCESSED_WITH_TILING);
  }
  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, 0, FALSE, input_entry);

  // and save the output colorspace
  pipe->dsc.cst = module->output_colorspace(module, pipe, piece);

  // blend needs input/output images with default colorspace
  if(_transform_for_blend(module, piece))
  {
    dt_iop_colorspace_type_t blend_cst = dt_develop_blend_colorspace(piece, pipe->dsc.cst);
    dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, 0, TRUE, input_entry);
    dt_ioppr_transform_image_colorspace(module, input, input, roi_in->width, roi_in->height,
                                        input_format->cst, blend_cst, &input_format->cst,
                                        work_profile);
    dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, 0, FALSE, input_entry);

    dt_ioppr_transform_image_colorspace(module, *output, *output, roi_out->width, roi_out->height,
                                        pipe->dsc.cst, blend_cst, &pipe->dsc.cst,
                                        work_profile);
  }

  /* process blending on CPU */
  int err = dt_develop_blend_process(module, piece, input, *output, roi_in, roi_out);
  *pixelpipe_flow |= (PIXELPIPE_FLOW_BLENDED_ON_CPU);
  *pixelpipe_flow &= ~(PIXELPIPE_FLOW_BLENDED_ON_GPU);

  return err; //no errors
}

static dt_dev_pixelpipe_iop_t *_last_node_in_pipe(dt_dev_pixelpipe_t *pipe)
{
  for(GList *node = g_list_last(pipe->nodes); node; node = g_list_previous(node))
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)node->data;
    if(piece->enabled) return piece;
  }

  return NULL;
}

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

  // Fetch RGB working profile
  // if input is RAW, we can't color convert because RAW is not in a color space
  // so we send NULL to by-pass
  const dt_iop_order_iccprofile_info_t *const work_profile
      = (input_format->cst != IOP_CS_RAW) ? dt_ioppr_get_pipe_work_profile_info(pipe) : NULL;

  // ensure that we are using the right color space
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


#ifdef HAVE_OPENCL

// if host_ptr is NULL, we use an unpinned device buffer
static void *_gpu_init_buffer(int devid, void *const host_ptr, const dt_iop_roi_t *roi, const size_t bpp,
                              dt_iop_module_t *module, const char *message)
{
  // Need to use read-write mode because of in-place color space conversions
  void *cl_mem_input = NULL;
  
  if(host_ptr)
    cl_mem_input = dt_opencl_alloc_device_use_host_pointer(devid, roi->width, roi->height, bpp, host_ptr,
                                                               CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR);
  else
    cl_mem_input = dt_opencl_alloc_device(devid, roi->width, roi->height, bpp);

  if(cl_mem_input == NULL)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_pixelpipe] couldn't generate %s buffer for module %s\n", message,
             module->op);
  }

  return cl_mem_input;
}


static void _gpu_clear_buffer(void **cl_mem_buffer)
{
  if(*cl_mem_buffer != NULL)
  {
    dt_opencl_release_mem_object(*cl_mem_buffer);
    *cl_mem_buffer = NULL;
  }
}

static gboolean _check_zero_memory(void *cl_mem_pinned, void *host_ptr, dt_iop_module_t *module, const char *message)
{
  if(cl_mem_pinned == host_ptr)
  {
    //printf("✅ Zero-copy: GPU is using your host memory directly for %s %s\n", module->op, message);
    return TRUE;
  }
  else
  {
    printf("❌ Not zero-copy: OpenCL made a temporary device-side copy for %s %s\n", (module) ? module->op : "base buffer", message);
    return FALSE;
  }
}


// mode : CL_MAP_WRITE = copy from host to device, CL_MAP_READ = copy from device to host
static int _cl_pinned_memory_copy(const int devid, void *host_ptr, void *cl_mem_buffer, const dt_iop_roi_t *roi,
                                  int cl_mode, size_t bpp, dt_iop_module_t *module, const char *message)
{
  // Mapping step: make device memory accessible to host for read or write.
  // Host is not allowed to write in there in the meantime
  void *cl_mem_pinned_input = dt_opencl_map_image(devid, cl_mem_buffer, TRUE, cl_mode, roi->width,
                                                  roi->height, bpp);
  if(cl_mem_pinned_input == NULL)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_pixelpipe] couldn't map pinned image to opencl device for module %s (%s)\n",
                (module) ? module->op : "base buffer", message);
    return 1;
  }
  
  // Unmapping step: give back ownership of cl_mem_buffer to the device
  cl_int err = dt_opencl_unmap_mem_object(devid, cl_mem_buffer, cl_mem_pinned_input);
  if(err != CL_SUCCESS)
    return 1;

  // Map/Unmap synchronizes host <-> device  pixels if we have a zero-copy buffer.
  // If we couldn't get a zero-copy buffer, we need to manually copy pixels
  if(!_check_zero_memory(cl_mem_pinned_input, host_ptr, module, message))
  {
    err = CL_SUCCESS;

    if(cl_mode == CL_MAP_WRITE)
      err = dt_opencl_write_host_to_device(devid, host_ptr, cl_mem_buffer, roi->width, roi->height, bpp);
    else if(cl_mode == CL_MAP_READ)
      err = dt_opencl_read_host_from_device(devid, host_ptr, cl_mem_buffer, roi->width, roi->height, bpp);

    if(err != CL_SUCCESS)
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_pixelpipe] couldn't copy image to opencl device for module %s (%s)\n",
                (module) ? module->op : "base buffer", message);
      return 1;
    }
  }

  dt_print(DT_DEBUG_OPENCL, "[opencl_pixelpipe] successfully mapped image %s for module %s (%s)\n",
                (cl_mode == CL_MAP_WRITE) ? "host to device" : "device to host",
                (module) ? module->op : "base buffer", message);

  return 0;
}

static int _is_opencl_supported(dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece, dt_iop_module_t *module)
{
  return dt_opencl_is_inited() && piece->process_cl_ready && module->process_cl;
}

static int _resync_input_gpu_to_cache(dt_dev_pixelpipe_t *pipe, float *input, void *cl_mem_input,
                                      dt_iop_buffer_dsc_t *input_format, const dt_iop_roi_t *roi_in,
                                      dt_iop_module_t *module, dt_iop_colorspace_type_t input_cst_cl,
                                      const size_t in_bpp, dt_pixel_cache_entry_t *input_entry,
                                      const char *message)
{
  dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, 0, TRUE, input_entry);
  int fail = _cl_pinned_memory_copy(pipe->devid, input, cl_mem_input, roi_in, CL_MAP_READ, in_bpp, module,
                                    message);

  // Color conversions happen inplace, so we need to ensure colorspace metadata are up-to-date
  if(!fail) input_format->cst = input_cst_cl;

  // We need to enforce the OpenCL pipe to run in sync with CPU RAM cache
  // to ensure the validaty of the locks
  dt_opencl_events_wait_for(pipe->devid);
  dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, 0, FALSE, input_entry);

  return fail;
}


static int pixelpipe_process_on_GPU(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev,
                                    float *input, void *cl_mem_input, dt_iop_buffer_dsc_t *input_format, const dt_iop_roi_t *roi_in,
                                    void **output, void **cl_mem_output, dt_iop_buffer_dsc_t **out_format, const dt_iop_roi_t *roi_out,
                                    dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                                    dt_develop_tiling_t *tiling, dt_pixelpipe_flow_t *pixelpipe_flow,
                                    const size_t in_bpp, const size_t bpp,
                                    dt_pixel_cache_entry_t *input_entry)
{
  // Not properly speaking an error, but go to CPU fallback straight away
  gboolean no_opencl = !_is_opencl_supported(pipe, piece, module) || !pipe->opencl_enabled;
  if(no_opencl)
  {
    dt_print(DT_DEBUG_OPENCL, "[dev_pixelpipe] %s will run directly on CPU\n", module->name());
    goto error;
  }

  // We don't have OpenCL or we couldn't lock a GPU: fallback to CPU processing
  if(!(pipe->devid >= 0) || input == NULL || *output == NULL)
    goto error;

  // Fetch RGB working profile
  // if input is RAW, we can't color convert because RAW is not in a color space
  // so we send NULL to by-pass
  const dt_iop_order_iccprofile_info_t *const work_profile
      = (input_format->cst != IOP_CS_RAW) ? dt_ioppr_get_pipe_work_profile_info(pipe) : NULL;
  gboolean success_opencl = TRUE;
  dt_iop_colorspace_type_t input_cst_cl = input_format->cst;

  const float required_factor_cl = fmaxf(1.0f, (cl_mem_input != NULL) ? tiling->factor_cl - 1.0f : tiling->factor_cl);
  /* pre-check if there is enough space on device for non-tiled processing */
  const gboolean fits_on_device = dt_opencl_image_fits_device(pipe->devid, MAX(roi_in->width, roi_out->width),
                                                              MAX(roi_in->height, roi_out->height), MAX(in_bpp, bpp),
                                                              required_factor_cl, tiling->overhead);

  /* general remark: in case of opencl errors within modules or out-of-memory on GPU, we transparently
      fall back to the respective cpu module and continue in pixelpipe. If we encounter errors we set
      pipe->opencl_error=1, return this function with value 1, and leave appropriate action to the calling
      function, which normally would restart pixelpipe without opencl.
      Late errors are sometimes detected when trying to get back data from device into host memory and
      are treated in the same manner. */

  /* test for a possible opencl path after checking some module specific pre-requisites */
  gboolean possible_cl = (!no_opencl
      && !((pipe->type & DT_DEV_PIXELPIPE_PREVIEW) == DT_DEV_PIXELPIPE_PREVIEW
          && (module->flags() & IOP_FLAGS_PREVIEW_NON_OPENCL))
      && (fits_on_device || piece->process_tiling_ready));

  // Force caching the output because it will probably be less of an hassle
  // than whatever mitigation strategie we will be using there
  if(!possible_cl || !fits_on_device) piece->force_opencl_cache = TRUE;

  if(possible_cl && !fits_on_device)
  {
    const float cl_px = dt_opencl_get_device_available(pipe->devid) / (sizeof(float) * MAX(in_bpp, bpp) * ceilf(required_factor_cl));
    const float dx = MAX(roi_in->width, roi_out->width);
    const float dy = MAX(roi_in->height, roi_out->height);
    const float border = tiling->overlap + 1;
    /* tests for required gpu mem reflects the different tiling stategies.
        simple tiles over whole height or width or inside rectangles where we need at last the overlapping area.
    */
    const gboolean possible = (cl_px > dx * border) || (cl_px > dy * border) || (cl_px > border * border);
    if(!possible)
    {
      dt_print(DT_DEBUG_OPENCL | DT_DEBUG_TILING, "[dt_dev_pixelpipe_process_rec] CL: tiling impossible in module `%s'. avail=%.1fM, requ=%.1fM (%ix%i). overlap=%i\n",
          module->name(), cl_px / 1e6f, dx*dy / 1e6f, (int)dx, (int)dy, (int)tiling->overlap);
      goto error;
    }
  }

  // Not enough memory for one-shot processing, or no tiling support, or tiling support
  // but still not enough memory for tiling (due to boundary overlap).
  if(!possible_cl) goto error;

  if(fits_on_device)
  {
    /* image is small enough -> try to directly process entire image with opencl */

    /* input is not on gpu memory -> copy it there */
    if(cl_mem_input == NULL)
    {
      dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, 0, TRUE, input_entry);
      cl_mem_input = _gpu_init_buffer(pipe->devid, input, roi_in, in_bpp, module, "input");
      int fail = (cl_mem_input == NULL);

      if(!fail && _cl_pinned_memory_copy(pipe->devid, input, cl_mem_input, roi_in, CL_MAP_WRITE, in_bpp, module,
                                "cache to input"))
        fail = TRUE;

      // We need to enforce the OpenCL pipe to run in sync with CPU RAM cache
      // to ensure the validity of the locks
      dt_opencl_events_wait_for(pipe->devid);
      dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, 0, FALSE, input_entry);

      if(fail) goto error;
    }
    else
    {
      dt_print(DT_DEBUG_OPENCL, "[dev_pixelpipe] %s will use its input directly from vRAM\n", module->name());
    }

    // Allocate GPU memory for output: pinned memory if copying to cache, else device memory.
    if(*cl_mem_output == NULL)
    {
      *cl_mem_output = _gpu_init_buffer(pipe->devid, 
                                        (piece->force_opencl_cache) ? *output : NULL, 
                                        roi_out, bpp, module, "output");
      if(*cl_mem_output == NULL) goto error;
    }

    // transform to input colorspace if we got our input in a different colorspace
    if(!dt_ioppr_transform_image_colorspace_cl(
           module, piece->pipe->devid, cl_mem_input, cl_mem_input, roi_in->width, roi_in->height, input_cst_cl,
           module->input_colorspace(module, pipe, piece), &input_cst_cl, work_profile))
      goto error;

    /* now call process_cl of module; module should emit meaningful messages in case of error */
    if(!module->process_cl(module, piece, cl_mem_input, *cl_mem_output, roi_in, roi_out))
    {
      // If process_cl() failed, we will fallback to CPU path, but we have no guaranty that our
      // input is in cache. Force resync just to be safe.
      _resync_input_gpu_to_cache(pipe, input, cl_mem_input, input_format, roi_in,
                                 module, input_cst_cl, in_bpp, input_entry,
                                 "fallback input copy to cache");
      goto error;
    }

    *pixelpipe_flow |= (PIXELPIPE_FLOW_PROCESSED_ON_GPU);
    *pixelpipe_flow &= ~(PIXELPIPE_FLOW_PROCESSED_ON_CPU | PIXELPIPE_FLOW_PROCESSED_WITH_TILING);

    // and save the output colorspace
    pipe->dsc.cst = module->output_colorspace(module, pipe, piece);

    // blend needs input/output images with default colorspace
    if(_transform_for_blend(module, piece))
    {
      dt_iop_colorspace_type_t blend_cst = dt_develop_blend_colorspace(piece, pipe->dsc.cst);
      success_opencl &= dt_ioppr_transform_image_colorspace_cl(
          module, piece->pipe->devid, cl_mem_input, cl_mem_input, roi_in->width, roi_in->height, input_cst_cl,
          blend_cst, &input_cst_cl, work_profile);
      success_opencl &= dt_ioppr_transform_image_colorspace_cl(
          module, piece->pipe->devid, *cl_mem_output, *cl_mem_output, roi_out->width, roi_out->height,
          pipe->dsc.cst, blend_cst, &pipe->dsc.cst, work_profile);

      if(!success_opencl)
      {
        dt_print(DT_DEBUG_OPENCL, "[opencl_pixelpipe] couldn't transform blending colorspace for module %s\n",
                  module->name());
        goto error;
      }
    }

    /* process blending */
    if(dt_develop_blend_process_cl(module, piece, cl_mem_input, *cl_mem_output, roi_in, roi_out))
    {
      // If process_cl() failed, we will fallback to CPU path, but we have no guaranty that our
      // input is in cache. Force resync just to be safe.
      _resync_input_gpu_to_cache(pipe, input, cl_mem_input, input_format, roi_in,
                                 module, input_cst_cl, in_bpp, input_entry,
                                 "fallback input copy to cache");
      goto error;
    }

    *pixelpipe_flow |= (PIXELPIPE_FLOW_BLENDED_ON_GPU);
    *pixelpipe_flow &= ~(PIXELPIPE_FLOW_BLENDED_ON_CPU);

    // Resync OpenCL output buffer with CPU/RAM cache
    if(piece->force_opencl_cache)
    {
      if(_cl_pinned_memory_copy(pipe->devid, *output, *cl_mem_output, roi_out, CL_MAP_READ, bpp, module,
                              "output to cache"))
        goto error;
      dt_print(DT_DEBUG_OPENCL, "[dev_pixelpipe] output memory was copied to cache for %s\n", module->name());
      // Note : this whole function is already called from within a write locked section
    }

    // Because we color-converted the input several times in place,
    // we need to update the colorspace metadata. But since it's shared
    // between RAM pixel cache and GPU buffer, then we need to resync GPU buffer with cache.
    /* Still needed ?
    if(input_format->cst != input_cst_cl && piece->force_opencl_cache && FALSE)
    {
      int fail = _resync_input_gpu_to_cache(pipe, input, cl_mem_input, input_format, roi_in,
                                             module, input_cst_cl, in_bpp, input_entry,
                                             "color-converted input to cache");
      if(fail) goto error;
      input_format->cst = input_cst_cl;
    }
    */
  }
  else if(piece->process_tiling_ready)
  {
    /* image is too big for direct opencl processing -> try to process image via tiling */
    _gpu_clear_buffer(&cl_mem_input);

    // transform to module input colorspace
    dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, 0, TRUE, input_entry);
    dt_ioppr_transform_image_colorspace(module, input, input, roi_in->width, roi_in->height, input_format->cst,
                                        module->input_colorspace(module, pipe, piece), &input_format->cst,
                                        work_profile);
    dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, 0, FALSE, input_entry);

    /* now call process_tiling_cl of module; module should emit meaningful messages in case of error */
    dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, 0, TRUE, input_entry);
    int fail = !module->process_tiling_cl(module, piece, input, *output, roi_in, roi_out, in_bpp);
    dt_opencl_events_wait_for(pipe->devid);
    dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, 0, FALSE, input_entry);

    if(fail) goto error;

    *pixelpipe_flow |= (PIXELPIPE_FLOW_PROCESSED_ON_GPU | PIXELPIPE_FLOW_PROCESSED_WITH_TILING);
    *pixelpipe_flow &= ~(PIXELPIPE_FLOW_PROCESSED_ON_CPU);

    // and save the output colorspace
    pipe->dsc.cst = module->output_colorspace(module, pipe, piece);

    // blend needs input/output images with default colorspace
    if(_transform_for_blend(module, piece))
    {
      dt_iop_colorspace_type_t blend_cst = dt_develop_blend_colorspace(piece, pipe->dsc.cst);

      dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, 0, TRUE, input_entry);
      dt_ioppr_transform_image_colorspace(module, input, input, roi_in->width, roi_in->height,
                                          input_format->cst, blend_cst, &input_format->cst,
                                          work_profile);
      dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, 0, FALSE, input_entry);

      dt_ioppr_transform_image_colorspace(module, *output, *output, roi_out->width, roi_out->height,
                                          pipe->dsc.cst, blend_cst, &pipe->dsc.cst,
                                          work_profile);
    }

    /* do process blending on cpu (this is anyhow fast enough) */
    dt_develop_blend_process(module, piece, input, *output, roi_in, roi_out);
    *pixelpipe_flow |= (PIXELPIPE_FLOW_BLENDED_ON_CPU);
    *pixelpipe_flow &= ~(PIXELPIPE_FLOW_BLENDED_ON_GPU);
    
    // Because we color-converted the input several times in place,
    // we need to update the colorspace metadata. But since it's shared
    // between RAM pixel cache and GPU buffer, then we need to resync GPU buffer with cache.
    /* Still needed ?
    if(input_format->cst != input_cst_cl && piece->force_opencl_cache && FALSE)
    {
      int fail = _resync_input_gpu_to_cache(pipe, input, cl_mem_input, input_format, roi_in,
                                             module, input_cst_cl, in_bpp, input_entry,
                                             "color-converted input to cache");
      if(fail) goto error;
      input_format->cst = input_cst_cl;
    }
    */
  }
  else
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_pixelpipe] could not run module '%s' on gpu. falling back to cpu path\n",
             module->name());

    goto error;
  }

  // if (rand() % 20 == 0) success_opencl = FALSE; // Test code: simulate spurious failures

  // clean up OpenCL input memory and resync pipeline
  _gpu_clear_buffer(&cl_mem_input);

  // Wait for kernels and copies to complete before accessing the cache again and releasing the locks
  dt_opencl_events_wait_for(pipe->devid);

  return 0;

  // any error in OpenCL ends here
  // free everything and fall back to CPU processing
error:;

  // don't delete RAM output even if requested
  piece->force_opencl_cache = TRUE; 

  _gpu_clear_buffer(cl_mem_output);
  _gpu_clear_buffer(&cl_mem_input);
  dt_opencl_finish(pipe->devid);

  if(input != NULL && *output != NULL)
    return pixelpipe_process_on_CPU(pipe, dev, input, input_format, roi_in, output, out_format, roi_out, module,
                                    piece, tiling, pixelpipe_flow, input_entry);
  
  return 1;
}
#endif


static void _print_perf_debug(dt_dev_pixelpipe_t *pipe, const dt_pixelpipe_flow_t pixelpipe_flow, dt_dev_pixelpipe_iop_t *piece, dt_iop_module_t *module, dt_times_t *start)
{
  char histogram_log[32] = "";
  if(!(pixelpipe_flow & PIXELPIPE_FLOW_HISTOGRAM_NONE))
  {
    snprintf(histogram_log, sizeof(histogram_log), ", collected histogram on %s",
             (pixelpipe_flow & PIXELPIPE_FLOW_HISTOGRAM_ON_GPU
                  ? "GPU"
                  : pixelpipe_flow & PIXELPIPE_FLOW_HISTOGRAM_ON_CPU ? "CPU" : ""));
  }

  gchar *module_label = dt_history_item_get_name(module);
  dt_show_times_f(
      start, "[dev_pixelpipe]", "processed `%s' on %s%s%s, blended on %s [%s]", module_label,
      pixelpipe_flow & PIXELPIPE_FLOW_PROCESSED_ON_GPU
          ? "GPU"
          : pixelpipe_flow & PIXELPIPE_FLOW_PROCESSED_ON_CPU ? "CPU" : "",
      pixelpipe_flow & PIXELPIPE_FLOW_PROCESSED_WITH_TILING ? " with tiling" : "",
      (!(pixelpipe_flow & PIXELPIPE_FLOW_HISTOGRAM_NONE) && (piece->request_histogram & DT_REQUEST_ON))
          ? histogram_log
          : "",
      pixelpipe_flow & PIXELPIPE_FLOW_BLENDED_ON_GPU
          ? "GPU"
          : pixelpipe_flow & PIXELPIPE_FLOW_BLENDED_ON_CPU ? "CPU" : "",
      dt_pixelpipe_get_pipe_name(pipe->type));
  g_free(module_label);
}


static void _print_nan_debug(dt_dev_pixelpipe_t *pipe, void *cl_mem_output, void *output, const dt_iop_roi_t *roi_out, dt_iop_buffer_dsc_t *out_format, dt_iop_module_t *module, const size_t bpp)
{
  if((darktable.unmuted & DT_DEBUG_NAN) && strcmp(module->op, "gamma") != 0)
  {
    gchar *module_label = dt_history_item_get_name(module);

    if(out_format->datatype == TYPE_FLOAT && out_format->channels == 4)
    {
      int hasinf = 0, hasnan = 0;
      dt_aligned_pixel_t min = { FLT_MAX };
      dt_aligned_pixel_t max = { FLT_MIN };

      for(int k = 0; k < 4 * roi_out->width * roi_out->height; k++)
      {
        if((k & 3) < 3)
        {
          float f = ((float *)(output))[k];
          if(isnan(f))
            hasnan = 1;
          else if(isinf(f))
            hasinf = 1;
          else
          {
            min[k & 3] = fmin(f, min[k & 3]);
            max[k & 3] = fmax(f, max[k & 3]);
          }
        }
      }
      if(hasnan)
        fprintf(stderr, "[dev_pixelpipe] module `%s' outputs NaNs! [%s]\n", module_label,
                dt_pixelpipe_get_pipe_name(pipe->type));
      if(hasinf)
        fprintf(stderr, "[dev_pixelpipe] module `%s' outputs non-finite floats! [%s]\n", module_label,
                dt_pixelpipe_get_pipe_name(pipe->type));
      fprintf(stderr, "[dev_pixelpipe] module `%s' min: (%f; %f; %f) max: (%f; %f; %f) [%s]\n", module_label,
              min[0], min[1], min[2], max[0], max[1], max[2], dt_pixelpipe_get_pipe_name(pipe->type));
    }
    else if(out_format->datatype == TYPE_FLOAT && out_format->channels == 1)
    {
      int hasinf = 0, hasnan = 0;
      float min = FLT_MAX;
      float max = FLT_MIN;

      for(int k = 0; k < roi_out->width * roi_out->height; k++)
      {
        float f = ((float *)(output))[k];
        if(isnan(f))
          hasnan = 1;
        else if(isinf(f))
          hasinf = 1;
        else
        {
          min = fmin(f, min);
          max = fmax(f, max);
        }
      }
      if(hasnan)
        fprintf(stderr, "[dev_pixelpipe] module `%s' outputs NaNs! [%s]\n", module_label,
                dt_pixelpipe_get_pipe_name(pipe->type));
      if(hasinf)
        fprintf(stderr, "[dev_pixelpipe] module `%s' outputs non-finite floats! [%s]\n", module_label,
                dt_pixelpipe_get_pipe_name(pipe->type));
      fprintf(stderr, "[dev_pixelpipe] module `%s' min: (%f) max: (%f) [%s]\n", module_label, min, max,
                dt_pixelpipe_get_pipe_name(pipe->type));
    }

    g_free(module_label);
  }
}

// return 1 on error
static int _init_base_buffer(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, void **output,
                             void **cl_mem_output, dt_iop_buffer_dsc_t **out_format,
                             dt_iop_roi_t roi_in, const dt_iop_roi_t roi_out,
                             const uint64_t hash,
                             const gboolean bypass_cache,
                             const size_t bufsize, const size_t bpp)
{
  // Note: dt_dev_pixelpipe_cache_get actually init/alloc *output
  dt_pixel_cache_entry_t *cache_entry;
  int new_entry = dt_dev_pixelpipe_cache_get(darktable.pixelpipe_cache, hash, bufsize, "base buffer", pipe->type,
                                             output, out_format, &cache_entry);
  if(cache_entry == NULL) return 1;

  int err = 0;

  if(bypass_cache || new_entry)
  {
    if(dev->gui_attached) 
    {
      dev->loading_cache = TRUE;
      dt_toast_log(_("Loading full-resolution image in cache. This may take some time..."));
    }

    // Grab input buffer from mipmap cache.
    // We will have to copy it here and in pixelpipe cache because it can get evicted from mipmap cache
    // anytime after we release the lock, so it would not be thread-safe to just use a reference
    // to full-sized buffer. Otherwise, skip dt_dev_pixelpipe_cache_get and
    // *output = buf.buf for 1:1 at full resolution.
    dt_mipmap_buffer_t buf;
    dt_mipmap_cache_get(darktable.mipmap_cache, &buf, pipe->imgid, pipe->size, DT_MIPMAP_BLOCKING, 'r');

    // Cache size has changed since we inited pipe input ?
    // Note: we know pipe->iwidth/iheight are non-zero or we would have not launched a pipe.
    // Note 2: there is no valid reason for a cacheline to change size during runtime.
    if(!buf.buf || buf.height != pipe->iheight || buf.width != pipe->iwidth || !*output)
    {
      // Nothing we can do, we need to recompute roi_in and roi_out from scratch
      // for all modules with new sizes. Exit on error and catch that in develop.
      dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
      err = 1;
    }
    else if(roi_in.scale == 1.0f)
    {
      // fast branch for 1:1 pixel copies.
      if(roi_out.width > 0 && roi_out.height > 0)
      {
        _copy_buffer((const char *const)buf.buf, (char *const)*output, roi_out.height, roi_out.width,
                      pipe->iwidth, roi_out.x, roi_out.y, bpp * roi_out.width, bpp);
        dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
        err = 0;

#ifdef HAVE_OPENCL
        // In case the first module of the pipeline (straight after what we do here)
        // supports OpenCL natively, skip the RAM cache and directly copy the RAW
        // base buffer into GPU.
        // Note that _copy_buffer may crop a few pixels, so it's not always a direct copy.
        // In this situation, OpenCL can't use a pinned memory to copy a cropped buf.buf, 
        // so we will need to copy the copy, aka output (the cache).
        GList *pieces = g_list_first(pipe->nodes);
        GList *modules = g_list_first(pipe->iop);
        dt_dev_pixelpipe_iop_t *first_piece = (dt_dev_pixelpipe_iop_t *)pieces->data;
        dt_iop_module_t *first_module = (dt_iop_module_t *)modules->data;
        const int supports_opencl = _is_opencl_supported(pipe, first_piece, first_module);

        if(supports_opencl)
        {
          *cl_mem_output = _gpu_init_buffer(pipe->devid, *output, &roi_out, bpp, NULL, "base buffer");
          int fail = (*cl_mem_output == NULL);

          if(!fail)
            err = _cl_pinned_memory_copy(pipe->devid, *output, *cl_mem_output, &roi_out,
                                         CL_MAP_WRITE, bpp, NULL, "base buffer");

          // We need to synch the OpenCL events pipeline with CPU timeline because
          // we use a read lock on the cache that lives in the CPU timeline.
          dt_opencl_events_wait_for(pipe->devid);
          
          if(fail) _gpu_clear_buffer(cl_mem_output);
        }
#endif
      }
      else
      {
        // Invalid dimensions
        dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
        err = 1;
      }
    }
    else
    {
      fprintf(stdout,
                "Base buffer init: scale %f != 1.0 but the input has %li bytes per pixel. This case is not "
                "covered by the pipeline, please report the bug.\n",
                roi_out.scale, bpp);

      dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
      err = 1;
    }
  }
  // else found in cache.

  if(new_entry)
    dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, hash, FALSE, cache_entry);

  return err;
}

static void _sample_all(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, void *input, void **output,
                        const dt_iop_roi_t roi_in, const dt_iop_roi_t roi_out, dt_iop_buffer_dsc_t *input_format,
                        dt_iop_buffer_dsc_t **output_format, dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                        const uint64_t input_hash, const uint64_t hash, const size_t in_bpp, const size_t bpp,
                        dt_pixel_cache_entry_t *const input_entry, dt_pixel_cache_entry_t *const output_entry)
{
  if(!(dev->gui_attached && pipe == dev->preview_pipe
       && (pipe->type & DT_DEV_PIXELPIPE_PREVIEW) == DT_DEV_PIXELPIPE_PREVIEW
       && piece->enabled))
    return;

  // Lock all buffers in write mode because we might be doing in-place color conversion
  dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, hash, TRUE, output_entry);
  dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, input_hash, TRUE, input_entry);

  // Need to go first because we want module output RGB without color conversion.
  // Gamma outputs uint8_t so we take its input. We want float32.
  if(strcmp(module->op, "gamma") == 0)
    pixelpipe_get_histogram_backbuf(pipe, dev, input, roi_in, module, piece, input_hash, in_bpp);
  else
    pixelpipe_get_histogram_backbuf(pipe, dev, *output, roi_out, module, piece, hash, bpp);

  // sample internal histogram on input and color pickers
  collect_histogram_on_CPU(pipe, dev, input, roi_in, input_format, module, piece);
  _sample_color_picker(pipe, dev, input, input_format, roi_in, output, output_format, roi_out, module, piece);

  dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, hash, FALSE, output_entry);
  dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, input_hash, FALSE, input_entry);
}


// recursive helper for process:
static int dt_dev_pixelpipe_process_rec(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, void **output,
                                        void **cl_mem_output, dt_iop_buffer_dsc_t **out_format,
                                        dt_iop_roi_t roi_out, GList *modules, GList *pieces, int pos)
{
  // The pipeline is executed recursively, from the end. For each module n, starting from the end,
  // if output is cached, take it, else if input is cached, take it, process it and output,
  // else recurse to the previous module n-1 to get a an input.
  KILL_SWITCH_ABORT;

  dt_iop_roi_t roi_in = roi_out;

  void *input = NULL;
  void *cl_mem_input = NULL;
  *cl_mem_output = NULL;
  dt_iop_module_t *module = NULL;
  dt_dev_pixelpipe_iop_t *piece = NULL;

  if(modules)
  {
    module = (dt_iop_module_t *)modules->data;
    piece = (dt_dev_pixelpipe_iop_t *)pieces->data;

    if(piece) 
    {
      roi_out = piece->planned_roi_out;
      roi_in = piece->planned_roi_in;
    }

    // skip this module?
    if(!piece->enabled)
      return dt_dev_pixelpipe_process_rec(pipe, dev, output, cl_mem_output, out_format, roi_in,
                                          g_list_previous(modules), g_list_previous(pieces), pos - 1);

    if(dev->gui_attached) dev->progress.total++;
  }

  KILL_SWITCH_ABORT;

  get_output_format(module, pipe, piece, dev, *out_format);
  const size_t bpp = dt_iop_buffer_dsc_to_bpp(*out_format);
  const size_t bufsize = (size_t)bpp * roi_out.width * roi_out.height;
  uint64_t hash = dt_dev_pixelpipe_node_hash(pipe, piece, roi_out, pos);
  const gboolean bypass_cache = (module) ? piece->bypass_cache : FALSE;

  // 1) Fast-track:
  // If we have a cache entry for this hash, return it straight away,
  // don't recurse through pipeline and don't process.
  // We can't do it for the preview pipe because it needs to resync
  // the global histograms, so we will need to recurse through pipeline anyway.
  // This case is handled below.
  dt_pixel_cache_entry_t *existing_cache;
  if(((pipe->type & DT_DEV_PIXELPIPE_PREVIEW) != DT_DEV_PIXELPIPE_PREVIEW)
      && !bypass_cache && !pipe->reentry
      && dt_dev_pixelpipe_cache_get_existing(darktable.pixelpipe_cache, hash, output, out_format, &existing_cache))
  {
    // FIXME: on CPU path and GPU path with tiling, when 2 modules taking different color spaces are back to back,
    // the color conversion for the next is done in-place in the output of the previous. We should check
    // here if out_format->cst matches wathever we are expecting, and convert back if it doesn't.
    dt_print(DT_DEBUG_PIPE, "[dev_pixelpipe] found %" PRIu64 " (%s) for %s pipeline in cache\n", hash, module ? module->op : "noop",
             dt_pixelpipe_get_pipe_name(pipe->type));
    return 0;
  }

  // 2) no module means step 0 of the pipe : importing the input buffer
  if(!modules)
  {
    dt_times_t start;
    dt_get_times(&start);

    if(_init_base_buffer(pipe, dev, output, cl_mem_output, out_format, roi_in, roi_out, hash, bypass_cache, bufsize,
                      bpp))
      return 1;

    dt_show_times_f(&start, "[dev_pixelpipe]", "initing base buffer [%s]", dt_pixelpipe_get_pipe_name(pipe->type));
    return 0;
  }

  // 3) now recurse through the pipeline.
  dt_iop_buffer_dsc_t _input_format = { 0 };
  dt_iop_buffer_dsc_t *input_format = &_input_format;

  piece = (dt_dev_pixelpipe_iop_t *)pieces->data;
  piece->processed_roi_in = roi_in;
  piece->processed_roi_out = roi_out;

  if(dt_dev_pixelpipe_process_rec(pipe, dev, &input, &cl_mem_input, &input_format, roi_in,
                                  g_list_previous(modules), g_list_previous(pieces), pos - 1))
    return 1;

  KILL_SWITCH_ABORT;

  const size_t in_bpp = dt_iop_buffer_dsc_to_bpp(input_format);
  piece->dsc_out = piece->dsc_in = *input_format;
  module->output_format(module, pipe, piece, &piece->dsc_out);
  **out_format = pipe->dsc = piece->dsc_out;
  const size_t out_bpp = dt_iop_buffer_dsc_to_bpp(*out_format);

  // 3c) actually process this module BUT treat all bypasses first.

  // special case: user requests to see channel data in the parametric mask of a module, or the blending
  // mask. In that case we skip all modules manipulating pixel content and only process image distorting
  // modules. Finally "gamma" is responsible for displaying channel/mask data accordingly.
  if(strcmp(module->op, "gamma") != 0
     && (pipe->mask_display != DT_DEV_PIXELPIPE_DISPLAY_NONE)
     && !(module->operation_tags() & IOP_TAG_DISTORT)
     && (in_bpp == out_bpp)
     && !memcmp(&roi_in, &roi_out, sizeof(struct dt_iop_roi_t)))
  {
    // since we're not actually running the module, the output format is the same as the input format
    **out_format = pipe->dsc = piece->dsc_out = piece->dsc_in;
    *output = input;
    return 0;
  }

  if(dev->gui_attached)
  {
    gchar *module_label = dt_history_item_get_name(module);
    g_free(darktable.main_message);
    darktable.main_message = g_strdup_printf(_("Processing module `%s` for pipeline %s (%ix%i px @ %0.f%%)..."), 
                                             module_label, dt_pixelpipe_get_pipe_name(pipe->type), 
                                             roi_out.width, roi_out.height, roi_out.scale * 100.f);
    g_free(module_label);
    dt_control_queue_redraw_center();
  }

  // Get cache lines for input and output, possibly allocating a new one for output
  dt_pixel_cache_entry_t *input_entry = NULL;
  uint64_t input_hash = dt_dev_pixelpipe_cache_get_hash_data(darktable.pixelpipe_cache, input, &input_entry);

  // On OpenCL path, it's possibly that there is no cached entry if the buffer is only on GPU vRAM.
  // But if we have neither cache nor vRAM buffer, we need to fetch the previous step again.
  if(input_entry == NULL && cl_mem_input == NULL)
    if(dt_dev_pixelpipe_process_rec(pipe, dev, &input, &cl_mem_input, &input_format, roi_in,
                                    g_list_previous(modules), g_list_previous(pieces), pos - 1))
      return 1;

  dt_pixel_cache_entry_t *output_entry = NULL;
  gchar *type = dt_pixelpipe_get_pipe_name(pipe->type);
  gchar *name = g_strdup_printf("module %s (%s) for pipe %s", module->op, module->multi_name, type);
  gboolean new_entry = dt_dev_pixelpipe_cache_get(darktable.pixelpipe_cache, hash, bufsize, name, pipe->type,
                                                  output, out_format, &output_entry);
  g_free(name);
  if(output_entry == NULL) return 1;

  dt_pixelpipe_flow_t pixelpipe_flow = (PIXELPIPE_FLOW_NONE | PIXELPIPE_FLOW_HISTOGRAM_NONE);

  // If we found an existing cache entry for this hash (= !new_entry), and
  // bypassing the cache is not requested by the pipe, stop before processing.
  // This is mostly for the preview pipe since we didn't stop the recursion earlier
  // at the last-found cache line.
  if(!pipe->reentry && !new_entry && pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
  {
    dt_print(DT_DEBUG_PIPE, "[pipeline] found %" PRIu64 " (%s) for %s pipeline in cache\n", hash, module ? module->op : "noop", type);

    // Sample all color pickers and histograms
    _sample_all(pipe, dev, input, output, roi_in, roi_out, input_format, out_format, module,
                piece, input_hash, hash, in_bpp, bpp, input_entry, output_entry);

    dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, input_hash, FALSE, input_entry);

    return 0;
  }

  if(!new_entry)
  {
    // We have an output cache entry already, lock it for writing. 
    // There is no reason why this should happen except if another thread created an entry
    // with the same hash just before us and is not done writing in it.
    // But in this case, we could just wait for it to finish writing, and process nothin here.
    dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, hash, TRUE, output_entry);
  }

  /* get tiling requirement of module */
  dt_develop_tiling_t tiling = { 0 };
  tiling.factor_cl = tiling.maxbuf_cl = -1;	// set sentinel value to detect whether callback set sizes
  module->tiling_callback(module, piece, &roi_in, &roi_out, &tiling);
  if (tiling.factor_cl < 0) tiling.factor_cl = tiling.factor; // default to CPU size if callback didn't set GPU
  if (tiling.maxbuf_cl < 0) tiling.maxbuf_cl = tiling.maxbuf;

  /* does this module involve blending? */
  if(piece->blendop_data && ((dt_develop_blend_params_t *)piece->blendop_data)->mask_mode != DEVELOP_MASK_DISABLED)
  {
    /* get specific memory requirement for blending */
    dt_develop_tiling_t tiling_blendop = { 0 };
    tiling_callback_blendop(module, piece, &roi_in, &roi_out, &tiling_blendop);

    /* aggregate in structure tiling */
    tiling.factor = fmax(tiling.factor, tiling_blendop.factor);
    tiling.factor_cl = fmax(tiling.factor_cl, tiling_blendop.factor);
    tiling.maxbuf = fmax(tiling.maxbuf, tiling_blendop.maxbuf);
    tiling.maxbuf_cl = fmax(tiling.maxbuf_cl, tiling_blendop.maxbuf);
    tiling.overhead = fmax(tiling.overhead, tiling_blendop.overhead);
  }

  /* remark: we do not do tiling for blendop step, neither in opencl nor on cpu. if overall tiling
     requirements (maximum of module and blendop) require tiling for opencl path, then following blend
     step is anyhow done on cpu. we assume that blending itself will never require tiling in cpu path,
     because memory requirements will still be low enough. */

  assert(tiling.factor > 0.0f);
  assert(tiling.factor_cl > 0.0f);

  // Actual pixel processing for this module
  int error = 0;
  dt_times_t start;
  dt_get_times(&start);

#ifdef HAVE_OPENCL
  error = pixelpipe_process_on_GPU(pipe, dev, input, cl_mem_input, input_format, &roi_in, output, cl_mem_output,
                                   out_format, &roi_out, module, piece, &tiling, &pixelpipe_flow, in_bpp, bpp, input_entry);
#else
  error = pixelpipe_process_on_CPU(pipe, dev, input, input_format, &roi_in, output, out_format, &roi_out, module,
                                   piece, &tiling, &pixelpipe_flow, input_entry);
#endif

  _print_perf_debug(pipe, pixelpipe_flow, piece, module, &start);

  if(dev->gui_attached) dev->progress.completed++;   

  // Flag to throw away the output as soon as we are done consuming it in this thread, at the next module.
  // Cache bypass is requested by modules like crop/perspective, when they show the full image,
  // and when doing anything transient.
  if(bypass_cache || pipe->reentry || !piece->force_opencl_cache)
    dt_dev_pixelpipe_cache_flag_auto_destroy(darktable.pixelpipe_cache, hash, output_entry);

  // in case we get this buffer from the cache in the future, cache some stuff:
  **out_format = piece->dsc_out = pipe->dsc;

  // Unlock read and write locks, decrease reference count on input
  dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, hash, FALSE, output_entry);
  dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, input_hash, FALSE, input_entry);

  // Flush the input cache if flagged for auto destroy previously
  dt_dev_pixel_pipe_cache_auto_destroy_apply(darktable.pixelpipe_cache, input_hash, input_entry);

  if(dev->gui_attached)
  {
    g_free(darktable.main_message);
    darktable.main_message = NULL;
    dt_control_queue_redraw_center();

    if(dev->loading_cache && strcmp(module->op, "initialscale") == 0)
    {
      dt_toast_log(_("Full-resolution image loaded in cache !"));
      dev->loading_cache = FALSE;
    }
  }
  
  if(error)
  {
    // No point in keeping garbled output
    dt_dev_pixelpipe_cache_remove(darktable.pixelpipe_cache, hash, TRUE, output_entry);
    dt_dev_pixel_pipe_cache_auto_destroy_apply(darktable.pixelpipe_cache, input_hash, input_entry);
    dt_iop_nap(5000);
    return 1;
  }

  KILL_SWITCH_AND_FLUSH_CACHE;

  // Sample all color pickers and histograms
  _sample_all(pipe, dev, input, output, roi_in, roi_out, input_format, out_format, module, piece, input_hash,
              hash, in_bpp, bpp, input_entry, output_entry);

  // Print min/max/Nan in debug mode only
  if((darktable.unmuted & DT_DEBUG_NAN) && strcmp(module->op, "gamma") != 0 && *output)
  {
    dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, hash, TRUE, output_entry);
    _print_nan_debug(pipe, *cl_mem_output, *output, &roi_out, *out_format, module, bpp);
    dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, hash, FALSE, output_entry);
  }

  KILL_SWITCH_AND_FLUSH_CACHE;

  return 0;
}


void dt_dev_pixelpipe_disable_after(dt_dev_pixelpipe_t *pipe, const char *op)
{
  GList *nodes = g_list_last(pipe->nodes);
  dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
  while(strcmp(piece->module->op, op))
  {
    piece->enabled = 0;
    piece = NULL;
    nodes = g_list_previous(nodes);
    if(!nodes) break;
    piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
  }
}

void dt_dev_pixelpipe_disable_before(dt_dev_pixelpipe_t *pipe, const char *op)
{
  GList *nodes = pipe->nodes;
  dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
  while(strcmp(piece->module->op, op))
  {
    piece->enabled = 0;
    piece = NULL;
    nodes = g_list_next(nodes);
    if(!nodes) break;
    piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
  }
}

#define KILL_SWITCH_PIPE                                                                                          \
  if(dt_atomic_get_int(&pipe->shutdown))                                                                          \
  {                                                                                                               \
    if(pipe->devid >= 0)                                                                                          \
    {                                                                                                             \
      dt_opencl_unlock_device(pipe->devid);                                                                       \
      pipe->devid = -1;                                                                                           \
    }                                                                                                             \
    pipe->status = DT_DEV_PIXELPIPE_DIRTY;                                                                        \
    if(pipe->forms)                                                                                               \
    {                                                                                                             \
      g_list_free_full(pipe->forms, (void (*)(void *))dt_masks_free_form);                                        \
      pipe->forms = NULL;                                                                                         \
    }                                                                                                             \
    dt_iop_nap(5000);                                                                                             \
    return 1;                                                                                                     \
  }


static void _print_opencl_errors(int error, dt_dev_pixelpipe_t *pipe)
{
  switch(error)
  {
    case 1:
      dt_print(DT_DEBUG_OPENCL, "[opencl] Opencl errors; disabling opencl for %s pipeline!\n", dt_pixelpipe_get_pipe_name(pipe->type));
      dt_control_log(_("Ansel discovered problems with your OpenCL setup; disabling OpenCL for %s pipeline!"), dt_pixelpipe_get_pipe_name(pipe->type));
      break;
    case 2:
      dt_print(DT_DEBUG_OPENCL,
                 "[opencl] Too many opencl errors; disabling opencl for this session!\n");
      dt_control_log(_("Ansel discovered problems with your OpenCL setup; disabling OpenCL for this session!"));
      break;
    default:
      break;
  }
}

static void _update_backbuf_cache_reference(dt_dev_pixelpipe_t *pipe, void *buf, dt_iop_roi_t roi, int pos)
{
  const dt_dev_pixelpipe_iop_t *last_module = _last_node_in_pipe(pipe);
  pipe->backbuf_pipe_hash = dt_dev_pixelpipe_node_hash(pipe, last_module, roi, pos);
  pipe->backbuf_hist_hash = pipe->history_hash;
  pipe->backbuf = buf;
  pipe->backbuf_width = roi.width;
  pipe->backbuf_height = roi.height;
  pipe->backbuf_timestamp = time(NULL);

  // Note: when we know this is always true, we can use pipe->hash directly
  if(pipe->hash != pipe->backbuf_pipe_hash)
    fprintf(stdout, "inconsistency in pipe backbuf hash. Please report it\n");
}

static void _set_opencl_cache(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev)
{
  // Starting with the end of the pipe, gamma sends its buffer to GUI, so it needs RAM caching.
  // Any module not supporting OpenCL will set this to TRUE for the previous
  gboolean opencl_cache = TRUE;

  GList *pieces = g_list_last(pipe->nodes);
  GList *modules = g_list_last(pipe->iop);
  while(pieces)
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)pieces->data;
    dt_iop_module_t *module = (dt_iop_module_t *)modules->data;

    if(piece->enabled)
    {
      // OpenCL cache is forced if:
      // - current module requires it (heavy processing)
      // - next module doesn't support OpenCL (will take its input from cache only)
      // - current module has global histogram sampling
      // - current module has colorpicker/internal histogram
      // - current module is currently being modified in GUI
      gboolean supports_opencl = FALSE;

#ifdef HAVE_OPENCL
      supports_opencl = _is_opencl_supported(pipe, piece, module);
#endif

      gboolean color_picker_on
          = (dev->gui_module && darktable.lib->proxy.colorpicker.picker_proxy 
             && module == dev->gui_module && dev->gui_module->enabled
             && dev->gui_module->request_color_pick != DT_REQUEST_COLORPICK_OFF);

      gboolean histogram_on
          = (!(piece->request_histogram & DT_REQUEST_ONLY_IN_GUI) 
             && (piece->request_histogram & DT_REQUEST_ON));

      gboolean global_hist_on
          = (_get_backuf(dev, piece->module->op)
             && pipe->type == DT_DEV_PIXELPIPE_PREVIEW);

      gboolean requested = piece->force_opencl_cache
          || color_picker_on || histogram_on || global_hist_on;

      piece->force_opencl_cache = (requested || opencl_cache || !supports_opencl);

      fprintf(stdout, "%s has OpenCL cache %i, requested intern %i, requested next %i\n", piece->module->op, piece->force_opencl_cache, requested, opencl_cache);

      gboolean active_in_gui 
          = (dev->gui_attached && dev->gui_module == module);

      // previous module in pipeline will need to cache its output to RAM
      // if the current one doesn't handle OpenCL or is being edited
      opencl_cache = !supports_opencl || active_in_gui;
    }

    pieces = g_list_previous(pieces);
    modules = g_list_previous(modules);
  }
}

int dt_dev_pixelpipe_process(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, int x, int y, int width, int height,
                             double scale)
{
  if(darktable.unmuted & DT_DEBUG_MEMORY)
  {
    fprintf(stderr, "[memory] before pixelpipe process\n");
    dt_print_mem_usage();
  }

  dt_dev_pixelpipe_cache_print(darktable.pixelpipe_cache);

  // Get the roi_out hash of all nodes.
  // Get the previous output size of the module, for cache invalidation.
  const guint pos = g_list_length(pipe->iop);
  dt_iop_roi_t roi = (dt_iop_roi_t){ x, y, width, height, scale };
  dt_dev_pixelpipe_get_roi_in(pipe, dev, roi);
  _set_opencl_cache(pipe, dev);
  dt_pixelpipe_get_global_hash(pipe, dev);

  void *buf = NULL;

  // If the last backbuf image is still valid with regard to current pipe topology 
  // and history, and we still have an entry cache, abort now. Nothing to do.
  // For preview pipe, if using color pickers, we still need to traverse the pipeline.
  if(!pipe->reentry && !pipe->bypass_cache 
     && !(dev->gui_attached && darktable.lib->proxy.colorpicker.picker_proxy && pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
     && dt_dev_pixelpipe_cache_get_existing(darktable.pixelpipe_cache, pipe->hash, &buf, NULL, NULL))
  {
    // Remember that dt_dev_pixelpipe_cache_get_existing()
    // increases the ref_count of the cache entry, so it won't be
    // deleted from the cache until you manually decrease it 
    // when you are done consuming it.

    if(pipe->backbuf_pipe_hash != pipe->hash)
      _update_backbuf_cache_reference(pipe, buf, roi, pos);
    // else : backbuf ref is still up-to-date

    return 0;
  }

  dt_print(DT_DEBUG_DEV, "[pixelpipe] Started %s pipeline recompute at %i×%i px\n", dt_pixelpipe_get_pipe_name(pipe->type), width, height);

  // get a snapshot of the mask list
  dt_pthread_rwlock_rdlock(&dev->masks_mutex);
  pipe->forms = dt_masks_dup_forms_deep(dev->forms, NULL);
  dt_pthread_rwlock_unlock(&dev->masks_mutex);

  // go through the list of modules from the end:
  GList *modules = g_list_last(pipe->iop);
  GList *pieces = g_list_last(pipe->nodes);

  pipe->backbuf = NULL;
  pipe->opencl_enabled = dt_opencl_update_settings(); // update enabled flag and profile from preferences
  pipe->devid = (pipe->opencl_enabled) ? dt_opencl_lock_device(pipe->type)
                                       : -1; // try to get/lock opencl resource

  if(pipe->devid > -1) dt_opencl_events_reset(pipe->devid);
  dt_print(DT_DEBUG_OPENCL, "[pixelpipe_process] [%s] using device %d\n", dt_pixelpipe_get_pipe_name(pipe->type),
           pipe->devid);

  KILL_SWITCH_PIPE

  gboolean keep_running = TRUE;
  int opencl_error = 0;
  int err = 0;

  while(keep_running)
  {

#ifdef HAVE_OPENCL
    dt_opencl_check_tuning(pipe->devid);
#endif

    // WARNING: buf will actually be a reference to a pixelpipe cache line, so it will be freed
    // when the cache line is flushed or invalidated.
    void *cl_mem_out = NULL;

    dt_iop_buffer_dsc_t _out_format = { 0 };
    dt_iop_buffer_dsc_t *out_format = &_out_format;

    KILL_SWITCH_PIPE

    dt_times_t start;
    dt_get_times(&start);
    err = dt_dev_pixelpipe_process_rec(pipe, dev, &buf, &cl_mem_out, &out_format, roi, modules, pieces, pos);
    gchar *msg = g_strdup_printf("[pixelpipe] %s pipeline processing", dt_pixelpipe_get_pipe_name(pipe->type));
    dt_show_times(&start, msg);
    g_free(msg);

    // The pipeline has copied cl_mem_out into buf, so we can release it now.
  #ifdef HAVE_OPENCL
    _gpu_clear_buffer(&cl_mem_out);
  #endif

    // get status summary of opencl queue by checking the eventlist
    const int oclerr = (pipe->devid > -1) ? dt_opencl_events_flush(pipe->devid, TRUE) != 0 : 0;

    // Check if we had opencl errors ....
    // remark: opencl errors can come in two ways: pipe->opencl_error is TRUE (and err is TRUE) OR oclerr is
    // TRUE
    keep_running = (oclerr || (err && pipe->opencl_error));
    if(keep_running)
    {
      // Log the error
      darktable.opencl->error_count++; // increase error count
      opencl_error = 1; // = any OpenCL error, next run goes to CPU

      // Disable OpenCL for this pipe
      dt_opencl_unlock_device(pipe->devid);
      pipe->opencl_enabled = 0;
      pipe->opencl_error = 0;
      pipe->devid = -1;

      if(darktable.opencl->error_count >= DT_OPENCL_MAX_ERRORS)
      {
        // Too many errors : dispable OpenCL for this session
        darktable.opencl->stopped = 1;
        dt_capabilities_remove("opencl");
        opencl_error = 2; // = too many OpenCL errors, all runs go to CPU
      }

      _print_opencl_errors(opencl_error, pipe);

      // Relinquish the CPU because we are in a realtime thread
      dt_iop_nap(5000);
    }
    else if(!dt_atomic_get_int(&pipe->shutdown))
    {
      // No opencl errors, no killswitch triggered: we should have a valid output buffer now.
      _update_backbuf_cache_reference(pipe, buf, roi, pos);

      // Note : the last output (backbuf) of the pixelpipe cache is internally locked
      // Whatever consuming it will need to unlock it.
    }
  }

  // release resources:
  if(pipe->forms)
  {
    g_list_free_full(pipe->forms, (void (*)(void *))dt_masks_free_form);
    pipe->forms = NULL;
  }
  if(pipe->devid >= 0)
  {
    dt_opencl_unlock_device(pipe->devid);
    pipe->devid = -1;
  }

  // terminate
  dt_dev_pixelpipe_cache_print(darktable.pixelpipe_cache);

  // If an intermediate module set that, be sure to reset it at the end
  pipe->flush_cache = FALSE;
  return err;
}

/**
 * @brief Checks the validity of the raster mask source and target modules,
 * outputs errors if necessary. Also tells the user what to do.
 *
 * @param source_piece
 * @param current_piece
 * @param target_module
 * @return gboolean TRUE when all is good, FALSE otherwise.
 */
static gboolean _dt_dev_raster_mask_check(dt_dev_pixelpipe_iop_t *source_piece, dt_dev_pixelpipe_iop_t
  *current_piece, const dt_iop_module_t *target_module)
{
  gboolean success = TRUE;
  gchar *clean_target_name = delete_underscore(target_module->name());
  gchar *target_name = g_strdup_printf("%s (%s)", clean_target_name, target_module->multi_name);

  if(source_piece == NULL || current_piece == NULL)
  {
    fprintf(stderr,"[raster masks] ERROR: source: %s, current: %s\n",
            (source_piece != NULL) ? "is defined" : "is undefined",
            (current_piece != NULL) ? "is definded" : "is undefined");

    gchar *hint = NULL;
    if(source_piece == NULL)
    {
      // The loop searching linked modules to the raster masks
      // terminated without finding the source module.
      // that means the source module has been deleted.
      hint = g_strdup_printf(
            _("- Check if the module providing the masks for the module %s has not been deleted.\n"),
            target_name);
    }
    else if(current_piece == NULL)
    {
      // The loop searching linked modules to the raster masks
      // has stopped when it finds the source module but before it has
      // found the current module:
      // That means the raster mask is above current module.
      hint = g_strdup_printf(_("- Check if the module %s (%s) providing the masks has not been moved above %s.\n"),
                      delete_underscore(source_piece->module->name()), source_piece->module->multi_name, clean_target_name);
    }

    dt_control_log(_("The %s module is trying to reuse a mask from a module but it can't be found.\n"
                      "\n%s"),
                      target_name, hint ? hint : "");
    g_free(hint);

    fprintf(stderr, "[raster masks] no source module for module %s could be found\n", target_name);
    success = FALSE;
  }

  if(success && !source_piece->enabled)
  {
    gchar *clean_source_name = delete_underscore(source_piece->module->name());
    gchar *source_name = g_strdup_printf("%s (%s)", clean_source_name, source_piece->module->multi_name);
    // there might be stale masks from disabled modules left over. don't use those!
    dt_control_log(_("The `%s` module is trying to reuse a mask from disabled module `%s`.\n"
                     "Disabled modules cannot provide their masks to other modules.\n"
                     "\n- Please enable `%s` or change the raster mask in `%s`."),
                   target_name, source_name, source_name, target_name);

    fprintf(stderr, "[raster masks] module %s trying to reuse a mask from disabled instance of %s\n",
            target_name, source_name);

    g_free(clean_source_name);
    g_free(source_name);
    success = FALSE;
  }

  g_free(clean_target_name);
  g_free(target_name);
  return success;
}

float *dt_dev_get_raster_mask(dt_dev_pixelpipe_t *pipe, const dt_iop_module_t *raster_mask_source,
                              const int raster_mask_id, const dt_iop_module_t *target_module,
                              gboolean *free_mask, int *error)
{
  if(!error) return NULL;
  // TODO: refactor this mess to limit for/if nesting
  *error = 0;

  gchar *clean_target_name = delete_underscore(target_module->name());
  gchar *target_name = g_strdup_printf("%s (%s)", clean_target_name, target_module->multi_name);

  if(!raster_mask_source)
  {
    fprintf(stderr, "[raster masks] The source module of the mask for %s was not found\n", target_name);
    g_free(clean_target_name);
    g_free(target_name);
    return NULL;
  }

  *free_mask = FALSE;
  float *raster_mask = NULL;

  // Find the module objects associated with mask provider and consumer
  dt_dev_pixelpipe_iop_t *source_piece = NULL;
  dt_dev_pixelpipe_iop_t *current_piece = NULL;
  GList *source_iter = NULL;
  for(source_iter = g_list_last(pipe->nodes); source_iter; source_iter = g_list_previous(source_iter))
  {
    dt_dev_pixelpipe_iop_t *candidate = (dt_dev_pixelpipe_iop_t *)source_iter->data;
    if(candidate->module == target_module)
    {
      current_piece = candidate;
    }
    else if(candidate->module == raster_mask_source)
    {
      source_piece = candidate;
      break;
    }
  }

  int err_ret = !_dt_dev_raster_mask_check(source_piece, current_piece, target_module);

  // Pass on the error to the returning pointer
  *error = err_ret;

  if(!err_ret)
  {
    const uint64_t raster_hash = current_piece->global_mask_hash;

    gchar *clean_source_name = delete_underscore(source_piece->module->name());
    gchar *source_name = g_strdup_printf("%s (%s)", clean_source_name, source_piece->module->multi_name);
    raster_mask = dt_pixelpipe_raster_get(source_piece->raster_masks, raster_mask_id);
    

    // Print debug stuff
    gchar *type = dt_pixelpipe_get_pipe_name(pipe->type);
    if(raster_mask)
    {
      dt_print(DT_DEBUG_MASKS,
        "[raster masks] found in %s mask id %i from %s for module %s in pipe %s with hash %" PRIu64 "\n",
        "internal", raster_mask_id, source_name, target_name, type, raster_hash);

      // Disable re-entry if any
      dt_dev_pixelpipe_unset_reentry(pipe, raster_hash);
    }
    else
    {
      fprintf(stderr,
        "[raster masks] mask id %i from %s for module %s could not be found in pipe %s. Pipe re-entry will be attempted.\n",
        raster_mask_id, source_name, target_name, type);

      // Ask for a pipeline re-entry and flush all cache
      if(dt_dev_pixelpipe_set_reentry(pipe, raster_hash))
        pipe->flush_cache = TRUE;

      // This should terminate the pipeline now:
      if(error) *error = 1;

      g_free(clean_target_name);
      g_free(target_name);
      return NULL;
    }
    // If we fetch the raster mask again, straight from its provider, we need to distort it
    for(GList *iter = g_list_next(source_iter); iter; iter = g_list_next(iter))
    {
      // Pass the raster mask through all distortion steps between the provider and the consumer
      dt_dev_pixelpipe_iop_t *module = (dt_dev_pixelpipe_iop_t *)iter->data;

      if(module->enabled
          && !dt_dev_pixelpipe_activemodule_disables_currentmodule(module->module->dev, module->module))
      {
        if(module->module->distort_mask
          && !(!strcmp(module->module->op, "finalscale") // hack against pipes not using finalscale
                && module->processed_roi_in.width == 0
                && module->processed_roi_in.height == 0))
        {
          float *transformed_mask = dt_alloc_align_float((size_t)module->processed_roi_out.width
                                                          * module->processed_roi_out.height);
          if(!transformed_mask)
          {
            fprintf(stderr, "[raster masks] could not allocate memory for transformed mask\n");
            if(error) *error = 1;
            g_free(clean_target_name);
            g_free(target_name);
            return NULL;
          }

          module->module->distort_mask(module->module,
                                      module,
                                      raster_mask,
                                      transformed_mask,
                                      &module->processed_roi_in,
                                      &module->processed_roi_out);
          if(*free_mask) dt_free_align(raster_mask);
          *free_mask = TRUE;
          raster_mask = transformed_mask;
          dt_print(DT_DEBUG_MASKS, "[raster masks] doing transform\n");
        }
        else if(!module->module->distort_mask &&
                (module->processed_roi_in.width != module->processed_roi_out.width ||
                  module->processed_roi_in.height != module->processed_roi_out.height ||
                  module->processed_roi_in.x != module->processed_roi_out.x ||
                  module->processed_roi_in.y != module->processed_roi_out.y))
          fprintf(stderr, "FIXME: module `%s' changed the roi from %d x %d @ %d / %d to %d x %d | %d / %d but doesn't have "
                  "distort_mask() implemented!\n", module->module->op, module->processed_roi_in.width,
                  module->processed_roi_in.height, module->processed_roi_in.x, module->processed_roi_in.y,
                  module->processed_roi_out.width, module->processed_roi_out.height, module->processed_roi_out.x,
                  module->processed_roi_out.y);
      }

      if(module->module == target_module)
      {
        dt_print(DT_DEBUG_MASKS, "[raster masks] found mask id %i from %s for module %s (%s) in pipe %s\n",
                    raster_mask_id, source_name, delete_underscore(module->module->name()),
                    module->module->multi_name, dt_pixelpipe_get_pipe_name(pipe->type));
        break;
      }
    }
  }

  g_free(clean_target_name);
  g_free(target_name);
  return raster_mask;
}

void dt_dev_clear_rawdetail_mask(dt_dev_pixelpipe_t *pipe)
{
  if(pipe->rawdetail_mask_data) dt_free_align(pipe->rawdetail_mask_data);
  pipe->rawdetail_mask_data = NULL;
}

gboolean dt_dev_write_rawdetail_mask(dt_dev_pixelpipe_iop_t *piece, float *const rgb, const dt_iop_roi_t *const roi_in, const int mode)
{
  dt_dev_pixelpipe_t *p = piece->pipe;
  if((p->want_detail_mask & DT_DEV_DETAIL_MASK_REQUIRED) == 0)
  {
    if(p->rawdetail_mask_data)
      dt_dev_clear_rawdetail_mask(p);
    return FALSE;
  }
  if((p->want_detail_mask & ~DT_DEV_DETAIL_MASK_REQUIRED) != mode) return FALSE;

  dt_dev_clear_rawdetail_mask(p);

  const int width = roi_in->width;
  const int height = roi_in->height;
  float *mask = dt_alloc_align_float((size_t)width * height);
  float *tmp = dt_alloc_align_float((size_t)width * height);
  if((mask == NULL) || (tmp == NULL)) goto error;

  p->rawdetail_mask_data = mask;
  memcpy(&p->rawdetail_mask_roi, roi_in, sizeof(dt_iop_roi_t));

  dt_aligned_pixel_t wb = { piece->pipe->dsc.temperature.coeffs[0],
                            piece->pipe->dsc.temperature.coeffs[1],
                            piece->pipe->dsc.temperature.coeffs[2] };
  if((p->want_detail_mask & ~DT_DEV_DETAIL_MASK_REQUIRED) == DT_DEV_DETAIL_MASK_RAWPREPARE)
  {
    wb[0] = wb[1] = wb[2] = 1.0f;
  }
  dt_masks_calc_rawdetail_mask(rgb, mask, tmp, width, height, wb);
  dt_free_align(tmp);
  dt_print(DT_DEBUG_MASKS, "[dt_dev_write_rawdetail_mask] %i (%ix%i)\n", mode, roi_in->width, roi_in->height);
  return FALSE;

  error:
  fprintf(stderr, "[dt_dev_write_rawdetail_mask] couldn't write detail mask\n");
  dt_free_align(mask);
  dt_free_align(tmp);
  return TRUE;
}

#ifdef HAVE_OPENCL
gboolean dt_dev_write_rawdetail_mask_cl(dt_dev_pixelpipe_iop_t *piece, cl_mem in, const dt_iop_roi_t *const roi_in, const int mode)
{
  dt_dev_pixelpipe_t *p = piece->pipe;
  if((p->want_detail_mask & DT_DEV_DETAIL_MASK_REQUIRED) == 0)
  {
    if(p->rawdetail_mask_data)
      dt_dev_clear_rawdetail_mask(p);
    return FALSE;
  }

  if((p->want_detail_mask & ~DT_DEV_DETAIL_MASK_REQUIRED) != mode) return FALSE;

  dt_dev_clear_rawdetail_mask(p);

  const int width = roi_in->width;
  const int height = roi_in->height;

  cl_mem out = NULL;
  cl_mem tmp = NULL;
  float *mask = NULL;
  const int devid = p->devid;

  cl_int err = CL_SUCCESS;
  mask = dt_alloc_align_float((size_t)width * height);
  if(mask == NULL) goto error;
  out = dt_opencl_alloc_device(devid, width, height, sizeof(float));
  if(out == NULL) goto error;
  tmp = dt_opencl_alloc_device_buffer(devid, sizeof(float) * width * height);
  if(tmp == NULL) goto error;

  {
    const int kernel = darktable.opencl->blendop->kernel_calc_Y0_mask;
    dt_aligned_pixel_t wb = { piece->pipe->dsc.temperature.coeffs[0],
                              piece->pipe->dsc.temperature.coeffs[1],
                              piece->pipe->dsc.temperature.coeffs[2] };
    if((p->want_detail_mask & ~DT_DEV_DETAIL_MASK_REQUIRED) == DT_DEV_DETAIL_MASK_RAWPREPARE)
    {
      wb[0] = wb[1] = wb[2] = 1.0f;
    }
    size_t sizes[3] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &tmp);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &in);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &height);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(float), &wb[0]);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(float), &wb[1]);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(float), &wb[2]);
    err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
    if(err != CL_SUCCESS) goto error;
  }
  {
    size_t sizes[3] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
    const int kernel = darktable.opencl->blendop->kernel_write_scharr_mask;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &tmp);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &out);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &height);
    err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
    if(err != CL_SUCCESS) goto error;
  }

  {
    err = dt_opencl_read_host_from_device(devid, mask, out, width, height, sizeof(float));
    if(err != CL_SUCCESS) goto error;
  }

  p->rawdetail_mask_data = mask;
  memcpy(&p->rawdetail_mask_roi, roi_in, sizeof(dt_iop_roi_t));

  dt_opencl_release_mem_object(out);
  dt_opencl_release_mem_object(tmp);
  dt_print(DT_DEBUG_MASKS, "[dt_dev_write_rawdetail_mask_cl] mode %i (%ix%i)", mode, roi_in->width, roi_in->height);
  return FALSE;

  error:
  fprintf(stderr, "[dt_dev_write_rawdetail_mask_cl] couldn't write detail mask: %i\n", err);
  dt_dev_clear_rawdetail_mask(p);
  dt_opencl_release_mem_object(out);
  dt_opencl_release_mem_object(tmp);
  dt_free_align(mask);
  return TRUE;
}
#endif

// this expects a mask prepared by the demosaicer and distorts the mask through all pipeline modules
// until target
float *dt_dev_distort_detail_mask(const dt_dev_pixelpipe_t *pipe, float *src, const dt_iop_module_t *target_module)
{
  if(!pipe->rawdetail_mask_data) return NULL;
  gboolean valid = FALSE;
  const int check = pipe->want_detail_mask & ~DT_DEV_DETAIL_MASK_REQUIRED;

  GList *source_iter;
  for(source_iter = pipe->nodes; source_iter; source_iter = g_list_next(source_iter))
  {
    const dt_dev_pixelpipe_iop_t *candidate = (dt_dev_pixelpipe_iop_t *)source_iter->data;
    if(((!strcmp(candidate->module->op, "demosaic")) && candidate->enabled) && (check == DT_DEV_DETAIL_MASK_DEMOSAIC))
    {
      valid = TRUE;
      break;
    }
    if(((!strcmp(candidate->module->op, "rawprepare")) && candidate->enabled) && (check == DT_DEV_DETAIL_MASK_RAWPREPARE))
    {
      valid = TRUE;
      break;
    }
  }

  if(!valid) return NULL;
  dt_vprint(DT_DEBUG_MASKS, "[dt_dev_distort_detail_mask] (%ix%i) for module %s\n", pipe->rawdetail_mask_roi.width, pipe->rawdetail_mask_roi.height, target_module->op);

  float *resmask = src;
  float *inmask  = src;
  if(source_iter)
  {
    for(GList *iter = source_iter; iter; iter = g_list_next(iter))
    {
      dt_dev_pixelpipe_iop_t *module = (dt_dev_pixelpipe_iop_t *)iter->data;
      if(module->enabled
         && !dt_dev_pixelpipe_activemodule_disables_currentmodule(module->module->dev, module->module))
      {
        if(module->module->distort_mask
              && !(!strcmp(module->module->op, "finalscale") // hack against pipes not using finalscale
                    && module->processed_roi_in.width == 0
                    && module->processed_roi_in.height == 0))
        {
          float *tmp = dt_alloc_align_float((size_t)module->processed_roi_out.width * module->processed_roi_out.height);
          dt_vprint(DT_DEBUG_MASKS, "   %s %ix%i -> %ix%i\n", module->module->op, module->processed_roi_in.width, module->processed_roi_in.height, module->processed_roi_out.width, module->processed_roi_out.height);
          module->module->distort_mask(module->module, module, inmask, tmp, &module->processed_roi_in, &module->processed_roi_out);
          resmask = tmp;
          if(inmask != src) dt_free_align(inmask);
          inmask = tmp;
        }
        else if(!module->module->distort_mask &&
                (module->processed_roi_in.width != module->processed_roi_out.width ||
                 module->processed_roi_in.height != module->processed_roi_out.height ||
                 module->processed_roi_in.x != module->processed_roi_out.x ||
                 module->processed_roi_in.y != module->processed_roi_out.y))
              fprintf(stderr, "FIXME: module `%s' changed the roi from %d x %d @ %d / %d to %d x %d | %d / %d but doesn't have "
                 "distort_mask() implemented!\n", module->module->op, module->processed_roi_in.width,
                 module->processed_roi_in.height, module->processed_roi_in.x, module->processed_roi_in.y,
                 module->processed_roi_out.width, module->processed_roi_out.height, module->processed_roi_out.x,
                 module->processed_roi_out.y);

        if(module->module == target_module) break;
      }
    }
  }
  return resmask;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
