/*
    This file is part of darktable,
    Copyright (C) 2009-2016 johannes hanika.
    Copyright (C) 2010-2012 Henrik Andersson.
    Copyright (C) 2011 Bruce Guenter.
    Copyright (C) 2011 Robert Bieber.
    Copyright (C) 2011 Rostyslav Pidgornyi.
    Copyright (C) 2011-2017, 2019 Ulrich Pegelow.
    Copyright (C) 2012, 2021 Aldric Renaudin.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012-2019 Tobias Ellinghaus.
    Copyright (C) 2013-2016 Roman Lebedev.
    Copyright (C) 2013 Simon Spannagel.
    Copyright (C) 2014, 2016 Pedro Côrte-Real.
    Copyright (C) 2016 Matthieu Moy.
    Copyright (C) 2017, 2019 luzpaz.
    Copyright (C) 2018, 2020-2026 Aurélien PIERRE.
    Copyright (C) 2018-2019 Edgardo Hoszowski.
    Copyright (C) 2018-2022 Pascal Obry.
    Copyright (C) 2019 Andreas Schneider.
    Copyright (C) 2019-2022 Dan Torop.
    Copyright (C) 2019-2022 Hanno Schwalm.
    Copyright (C) 2019 Heiko Bauke.
    Copyright (C) 2020 Chris Elston.
    Copyright (C) 2020 Diederik Ter Rahe.
    Copyright (C) 2020 GrahamByrnes.
    Copyright (C) 2020-2021 Harold le Clément de Saint-Marcq.
    Copyright (C) 2020-2021 Hubert Kowalski.
    Copyright (C) 2020-2021 Ralf Brown.
    Copyright (C) 2021 Sakari Kapanen.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Philipp Lutz.
    Copyright (C) 2023-2024 Alynx Zhou.
    Copyright (C) 2023 lologor.
    Copyright (C) 2023 Luca Zulberti.
    Copyright (C) 2024 Alban Gruin.
    Copyright (C) 2024 tatu.
    Copyright (C) 2025-2026 Guillaume Stutin.
    Copyright (C) 2025 Miguel Moquillon.
    
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
#include "common/atomic.h"
#include "common/opencl.h"
#include "common/iop_order.h"
#include "control/control.h"
#include "control/conf.h"
#include "control/signal.h"
#include "develop/blend.h"
#include "develop/format.h"
#include "develop/imageop_math.h"
#include "develop/pixelpipe.h"
#include "develop/pixelpipe_cache.h"
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

#include "develop/pixelpipe_cache_cl.c"
#include "develop/pixelpipe_gui.c"

static void get_output_format(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                              dt_develop_t *dev, dt_iop_buffer_dsc_t *dsc);

char *dt_pixelpipe_get_pipe_name(dt_dev_pixelpipe_type_t pipe_type)
{
  char *r = NULL;

  switch(pipe_type)
  {
    case DT_DEV_PIXELPIPE_PREVIEW:
      r = _("preview");
      break;
    case DT_DEV_PIXELPIPE_FULL:
      r = _("full");
      break;
    case DT_DEV_PIXELPIPE_THUMBNAIL:
      r = _("thumbnail");
      break;
    case DT_DEV_PIXELPIPE_EXPORT:
      r = _("export");
      break;
    default:
      r = _("invalid");
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
    // Since we crop 1-channel RAW arbitrarily here, alignment is never guaranteed
    memcpy(output + bpp * j * o_width,
           input + bpp * (x_offset + (y_offset + j) * i_width),
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


int dt_dev_pixelpipe_init_export(dt_dev_pixelpipe_t *pipe, int levels, gboolean store_masks)
{
  const int res = dt_dev_pixelpipe_init_cached(pipe);
  pipe->type = DT_DEV_PIXELPIPE_EXPORT;
  pipe->levels = levels;
  pipe->store_all_raster_masks = store_masks;
  return res;
}

int dt_dev_pixelpipe_init_thumbnail(dt_dev_pixelpipe_t *pipe)
{
  const int res = dt_dev_pixelpipe_init_cached(pipe);
  pipe->type = DT_DEV_PIXELPIPE_THUMBNAIL;
  return res;
}

int dt_dev_pixelpipe_init_dummy(dt_dev_pixelpipe_t *pipe)
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
  pipe->processed_width = pipe->iwidth = 0;
  pipe->processed_height = pipe->iheight = 0;
  pipe->nodes = NULL;
  pipe->hash = 0;
  pipe->history_hash = 0;
  pipe->bypass_cache = 0;
  dt_dev_set_backbuf(&pipe->backbuf, 0, 0, 0, -1, -1);

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
  dt_pthread_mutex_init(&(pipe->busy_mutex), NULL);
  pipe->icc_type = DT_COLORSPACE_NONE;
  pipe->icc_filename = NULL;
  pipe->icc_intent = DT_INTENT_LAST;
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
  // blocks while busy and sets shutdown bit:
  dt_dev_pixelpipe_cleanup_nodes(pipe);
  // so now it's safe to clean up cache:
  dt_dev_pixelpipe_cache_unref_hash(darktable.pixelpipe_cache, pipe->backbuf.hash);
  pipe->backbuf.hash = -1;
  dt_pthread_mutex_destroy(&(pipe->busy_mutex));
  pipe->icc_type = DT_COLORSPACE_NONE;
  g_free(pipe->icc_filename);
  pipe->icc_filename = NULL;

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
  for(GList *nodes = g_list_first(pipe->nodes); nodes; nodes = g_list_next(nodes))
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
    // printf("cleanup module `%s'\n", piece->module->name());
    dt_iop_cleanup_pipe(piece->module, pipe, piece);
    free(piece->histogram);
    piece->histogram = NULL;
    dt_pixelpipe_raster_cleanup(piece->raster_masks);
    free(piece);
  }
  g_list_free(pipe->nodes);
  pipe->nodes = NULL;
  // and iop order
  g_list_free_full(pipe->iop_order_list, free);
  pipe->iop_order_list = NULL;
}

void dt_dev_pixelpipe_create_nodes(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev)
{
  // check that the pipe was actually properly cleaned up after the last run
  g_assert(pipe->nodes == NULL);
  g_assert(pipe->iop_order_list == NULL);
  pipe->iop_order_list = dt_ioppr_iop_order_copy_deep(dev->iop_order_list);

  // for all modules in dev:
  for(GList *modules = g_list_first(dev->iop); modules; modules = g_list_next(modules))
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
    memset(&piece->planned_roi_in, 0, sizeof(piece->planned_roi_in));
    memset(&piece->planned_roi_out, 0, sizeof(piece->planned_roi_out));

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

#define KILL_SWITCH_ABORT                                                                                         \
  if(dt_atomic_get_int(&pipe->shutdown))                                                                          \
  {                                                                                                               \
    if(*cl_mem_output != NULL)                                                                                    \
    {                                                                                                             \
      _gpu_clear_buffer(cl_mem_output, NULL, NULL, IOP_CS_NONE);                                                   \
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
      _gpu_clear_buffer(cl_mem_output, NULL, NULL, IOP_CS_NONE);                                                   \
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
  assert(input == dt_pixel_cache_entry_get_data(input_entry));

  if(input == NULL)
  {
    fprintf(stdout, "[dev_pixelpipe] %s got a NULL input, report that to developers\n", module->name());
    return 1;
  }
  if(output == NULL || *output == NULL)
  {
    fprintf(stdout, "[dev_pixelpipe] %s got a NULL output, report that to developers\n", module->name());
    return 1;
  }

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

  const size_t in_bpp = dt_iop_buffer_dsc_to_bpp(input_format);
  const size_t bpp = dt_iop_buffer_dsc_to_bpp(*out_format);

  const gboolean fitting = dt_tiling_piece_fits_host_memory(MAX(roi_in->width, roi_out->width),
                                                            MAX(roi_in->height, roi_out->height), MAX(in_bpp, bpp),
                                                            tiling->factor, tiling->overhead);

  /* process module on cpu. use tiling if needed and possible. */
  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, 0, TRUE, input_entry);
  int err = 0;
  if(!fitting && piece->process_tiling_ready)
  {
    err = module->process_tiling(module, piece, input, *output, roi_in, roi_out, in_bpp);
    *pixelpipe_flow |= (PIXELPIPE_FLOW_PROCESSED_ON_CPU | PIXELPIPE_FLOW_PROCESSED_WITH_TILING);
    *pixelpipe_flow &= ~(PIXELPIPE_FLOW_PROCESSED_ON_GPU);
  }
  else
  {
    err = module->process(module, piece, input, *output, roi_in, roi_out);
    *pixelpipe_flow |= (PIXELPIPE_FLOW_PROCESSED_ON_CPU);
    *pixelpipe_flow &= ~(PIXELPIPE_FLOW_PROCESSED_ON_GPU | PIXELPIPE_FLOW_PROCESSED_WITH_TILING);
  }
  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, 0, FALSE, input_entry);

  if(err)
  {
    fprintf(stdout, "[pixelpipe] %s process on CPU returned with an error\n", module->name());
    return err;
  }

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
  err = dt_develop_blend_process(module, piece, input, *output, roi_in, roi_out);
  *pixelpipe_flow |= (PIXELPIPE_FLOW_BLENDED_ON_CPU);
  *pixelpipe_flow &= ~(PIXELPIPE_FLOW_BLENDED_ON_GPU);

  return err; //no errors
}


#ifdef HAVE_OPENCL

static int _is_opencl_supported(dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece, dt_iop_module_t *module)
{
  return dt_opencl_is_inited() && piece->process_cl_ready && module->process_cl;
}

static int _gpu_cpu_fallback_from_opencl_error(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev,
                                               float *input, void *cl_mem_input,
                                               dt_iop_buffer_dsc_t *input_format, dt_iop_colorspace_type_t input_cst_cl,
                                               const dt_iop_roi_t *roi_in,
                                               void **output, void **cl_mem_output, dt_iop_buffer_dsc_t **out_format,
                                               const dt_iop_roi_t *roi_out,
                                               dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                                               dt_develop_tiling_t *tiling, dt_pixelpipe_flow_t *pixelpipe_flow,
                                               const size_t in_bpp,
                                               dt_pixel_cache_entry_t *input_entry, dt_pixel_cache_entry_t *output_entry,
                                               dt_pixel_cache_entry_t *locked_input_entry)
{
  void *cl_mem_input_local = cl_mem_input;

  // If we kept a read lock for true zero-copy, drop it before attempting any write lock / cache alloc.
  if(locked_input_entry)
    dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, 0, FALSE, locked_input_entry);

  // Ensure we have a host output buffer for the CPU fallback.
  if(output && *output == NULL)
  {
    *output = dt_pixel_cache_alloc(darktable.pixelpipe_cache, output_entry);
    if(*output == NULL)
    {
      dt_print(DT_DEBUG_OPENCL,
               "[dev_pixelpipe] %s CPU fallback has no output buffer (cache allocation failed?)\n",
               module->name());
      _gpu_clear_buffer(cl_mem_output, output_entry, NULL, IOP_CS_NONE);
      _gpu_clear_buffer(&cl_mem_input_local, input_entry, NULL, IOP_CS_NONE);
      return 1;
    }
  }

  // If upstream ran GPU-only, `input` can be NULL while `cl_mem_input` contains the correct data.
  if(cl_mem_input_local != NULL)
  {
    if(input == NULL)
    {
      dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, 0, TRUE, input_entry);
      input = dt_pixel_cache_alloc(darktable.pixelpipe_cache, input_entry);
      dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, 0, FALSE, input_entry);
    }

    if(input == NULL)
    {
      dt_print(DT_DEBUG_OPENCL,
               "[dev_pixelpipe] %s CPU fallback has no input buffer (cache allocation failed?)\n",
               module->name());
      _gpu_clear_buffer(cl_mem_output, output_entry, NULL, IOP_CS_NONE);
      _gpu_clear_buffer(&cl_mem_input_local, input_entry, NULL, IOP_CS_NONE);
      return 1;
    }

    dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, 0, TRUE, input_entry);
    const int fail = _cl_pinned_memory_copy(pipe->devid, input, cl_mem_input_local, roi_in, CL_MAP_READ, in_bpp, module,
                                           "cpu fallback input copy to cache");
    // Enforce sync with the CPU/RAM cache so lock validity is guaranteed.
    dt_opencl_finish(pipe->devid);
    dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, 0, FALSE, input_entry);

    if(fail)
    {
      dt_print(DT_DEBUG_OPENCL,
               "[dev_pixelpipe] %s couldn't resync GPU input to cache for CPU fallback\n",
               module->name());
      _gpu_clear_buffer(cl_mem_output, output_entry, NULL, IOP_CS_NONE);
      _gpu_clear_buffer(&cl_mem_input_local, input_entry, NULL, IOP_CS_NONE);
      return 1;
    }

    // Color conversions happen in-place on OpenCL buffers: keep CPU metadata in sync.
    input_format->cst = input_cst_cl;
  }
  else if(input == NULL)
  {
    dt_print(DT_DEBUG_OPENCL,
             "[dev_pixelpipe] %s CPU fallback has no input buffer (cache allocation failed?)\n",
             module->name());
    _gpu_clear_buffer(cl_mem_output, output_entry, NULL, IOP_CS_NONE);
    return 1;
  }

  // Release any OpenCL buffers from the failed GPU attempt before running the CPU module.
  _gpu_clear_buffer(cl_mem_output, output_entry, NULL, IOP_CS_NONE);
  _gpu_clear_buffer(&cl_mem_input_local, input_entry, NULL, IOP_CS_NONE);

  return pixelpipe_process_on_CPU(pipe, dev, input, input_format, roi_in, output, out_format,
                                  roi_out, module, piece, tiling, pixelpipe_flow, input_entry);
}

// Return -1 if no fallback happened. Otherwise return the CPU processing error code (0 on success).
static int _gpu_early_cpu_fallback_if_unsupported(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev,
                                                 float **input, void **cl_mem_input,
                                                 dt_iop_buffer_dsc_t *input_format, const dt_iop_roi_t *roi_in,
                                                 void **output, dt_iop_buffer_dsc_t **out_format,
                                                 const dt_iop_roi_t *roi_out,
                                                 dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                                                 dt_develop_tiling_t *tiling, dt_pixelpipe_flow_t *pixelpipe_flow,
                                                 const size_t in_bpp,
                                                 dt_pixel_cache_entry_t *input_entry,
                                                 dt_pixel_cache_entry_t *output_entry)
{
  const dt_iop_colorspace_type_t input_cst_cl = input_format->cst;

  dt_print(DT_DEBUG_OPENCL, "[dev_pixelpipe] %s will run directly on CPU\n", module->name());

  // Ensure we have a host output buffer for the CPU path.
  if(output && *output == NULL)
  {
    *output = dt_pixel_cache_alloc(darktable.pixelpipe_cache, output_entry);
    if(*output == NULL)
    {
      dt_print(DT_DEBUG_OPENCL,
               "[dev_pixelpipe] %s CPU fallback has no output buffer (cache allocation failed?)\n",
               module->name());
      _gpu_clear_buffer(cl_mem_input, input_entry, NULL, input_cst_cl);
      return 1;
    }
  }

  // If we are falling back from GPU state, ensure the host buffer exists and is in sync before CPU reads.
  if(cl_mem_input && *cl_mem_input != NULL)
  {
    if(input && *input == NULL)
    {
      dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, 0, TRUE, input_entry);
      *input = dt_pixel_cache_alloc(darktable.pixelpipe_cache, input_entry);
      dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, 0, FALSE, input_entry);
    }

    if(!input || *input == NULL)
    {
      dt_print(DT_DEBUG_OPENCL,
               "[dev_pixelpipe] %s CPU fallback has no input buffer (cache allocation failed?)\n",
               module->name());
      _gpu_clear_buffer(cl_mem_input, input_entry, NULL, input_cst_cl);
      return 1;
    }

    *input = _resync_input_gpu_to_cache(pipe, *input, *cl_mem_input, input_format, roi_in, module,
                                        input_cst_cl, in_bpp, input_entry,
                                        "cpu fallback input copy to cache");
  }
  else if(!input || *input == NULL)
  {
    dt_print(DT_DEBUG_OPENCL,
             "[dev_pixelpipe] %s CPU fallback has no input buffer (cache allocation failed?)\n",
             module->name());
    return 1;
  }

  _gpu_clear_buffer(cl_mem_input, input_entry, *input, input_cst_cl);

  return pixelpipe_process_on_CPU(pipe, dev, *input, input_format, roi_in, output, out_format,
                                  roi_out, module, piece, tiling, pixelpipe_flow, input_entry);
}


static int pixelpipe_process_on_GPU(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev,
                                    float *input, void *cl_mem_input, dt_iop_buffer_dsc_t *input_format, const dt_iop_roi_t *roi_in,
                                    void **output, void **cl_mem_output, dt_iop_buffer_dsc_t **out_format, const dt_iop_roi_t *roi_out,
                                    dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                                    dt_develop_tiling_t *tiling, dt_pixelpipe_flow_t *pixelpipe_flow,
                                    const size_t in_bpp, const size_t bpp,
                                    dt_pixel_cache_entry_t *input_entry, dt_pixel_cache_entry_t *output_entry)
{
  dt_iop_colorspace_type_t input_cst_cl = input_format->cst;
  dt_pixel_cache_entry_t *cpu_input_entry = input_entry;
  dt_pixel_cache_entry_t *locked_input_entry = NULL;
  
  // Go to CPU fallback straight away if we know we can't do OpenCL.
  if(!_is_opencl_supported(pipe, piece, module) 
     || !pipe->opencl_enabled 
     || !(pipe->devid >= 0))
  {
    return _gpu_early_cpu_fallback_if_unsupported(pipe, dev, &input, &cl_mem_input, input_format, roi_in,
                                                 output, out_format, roi_out, module, piece, tiling, pixelpipe_flow,
                                                 in_bpp, input_entry, output_entry);
  }

  // No input, nothing to do
  if(input == NULL && cl_mem_input == NULL)
  {
    dt_print(DT_DEBUG_OPENCL,
             "[dev_pixelpipe] %s has no RAM nor vRAM input... aborting.\n",
             module->name());
    return 1;
  }

  // Fetch RGB working profile
  // if input is RAW, we can't color convert because RAW is not in a color space
  // so we send NULL to by-pass
  const dt_iop_order_iccprofile_info_t *const work_profile
      = (input_format->cst != IOP_CS_RAW) ? dt_ioppr_get_pipe_work_profile_info(pipe) : NULL;

  const float required_factor_cl = fmaxf(1.0f, (cl_mem_input != NULL) 
                                    ? tiling->factor_cl - 1.0f 
                                    : tiling->factor_cl);

  /* pre-check if there is enough space on device for non-tiled processing */
  const size_t precheck_width = ROUNDUPDWD(MAX(roi_in->width, roi_out->width), pipe->devid);
  const size_t precheck_height = ROUNDUPDHT(MAX(roi_in->height, roi_out->height), pipe->devid);
  gboolean fits_on_device = dt_opencl_image_fits_device(pipe->devid, precheck_width, precheck_height,
                                                        MAX(in_bpp, bpp), required_factor_cl, tiling->overhead);
  if(!fits_on_device)
  {
    dt_print(DT_DEBUG_OPENCL,
             "[dev_pixelpipe] %s pre-check didn't fit on device, flushing cached pinned buffers and retrying\n",
             module->name());
    dt_dev_pixelpipe_cache_flush_clmem(darktable.pixelpipe_cache, pipe->devid);
    fits_on_device = dt_opencl_image_fits_device(pipe->devid, precheck_width, precheck_height,
                                                 MAX(in_bpp, bpp), required_factor_cl, tiling->overhead);
  }

  /* General remark: in case of OpenCL errors within modules or out-of-memory on GPU, we transparently
      fall back to the respective CPU module and continue in the pixelpipe.
      OpenCL command queue failures not caught here are detected by dt_opencl_events_flush() upstream. */

  /* test for a possible opencl path after checking some module specific pre-requisites */
  // FIXME: the non-opencl preview pipe can be planned ahead in the cache
  // policy. Don't do it here.
  gboolean possible_cl = (
      !(pipe->type == DT_DEV_PIXELPIPE_PREVIEW
          && (module->flags() & IOP_FLAGS_PREVIEW_NON_OPENCL))
      && (fits_on_device || piece->process_tiling_ready));

  // Force caching the output because it will probably be less of an hassle
  // than whatever mitigation strategie we will be using there
  if(!possible_cl || !fits_on_device) piece->force_opencl_cache = TRUE;
  if(piece->force_opencl_cache && *output == NULL)
  {
    *output = dt_pixel_cache_alloc(darktable.pixelpipe_cache, output_entry);
    if(*output == NULL) goto error;
  }

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
	    if(_gpu_prepare_cl_input(pipe, module, input, &cl_mem_input, &input_cst_cl,
	                             roi_in, in_bpp, input_entry, &locked_input_entry))
	      goto error;

    // Allocate GPU memory for output: pinned memory if copying to cache, else device memory.
    // Note: device memory is faster but can't be reused, we need to free it after use.
    if(*cl_mem_output == NULL)
    {
      // Note : *output will be NULL unless piece->force_opencl_cache is TRUE
      // In this case, we start a pinned memory alloc. If NULL, it's device alloc.
      // *output decides which it is going to be.
      *cl_mem_output = _gpu_init_buffer(pipe->devid, *output, roi_out, bpp, module, "output", output_entry, FALSE,
                                        &(*out_format)->cst, NULL);
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
      int success = 1;
      success &= dt_ioppr_transform_image_colorspace_cl(
          module, piece->pipe->devid, cl_mem_input, cl_mem_input, roi_in->width, roi_in->height, input_cst_cl,
          blend_cst, &input_cst_cl, work_profile);
      success &= dt_ioppr_transform_image_colorspace_cl(
          module, piece->pipe->devid, *cl_mem_output, *cl_mem_output, roi_out->width, roi_out->height,
          pipe->dsc.cst, blend_cst, &pipe->dsc.cst, work_profile);

      if(!success)
      {
        dt_print(DT_DEBUG_OPENCL, "[opencl_pixelpipe] couldn't transform blending colorspace for module %s\n",
                  module->name());
        goto error;
      }
    }

    /* process blending */
    if(dt_develop_blend_process_cl(module, piece, cl_mem_input, *cl_mem_output, roi_in, roi_out))
    {
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
  }
  else if(piece->process_tiling_ready && input != NULL)
  {
    /* image is too big for direct opencl processing -> try to process image via tiling */
    _gpu_clear_buffer(&cl_mem_input, input_entry, input, input_cst_cl);

    // transform to module input colorspace
    dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, 0, TRUE, input_entry);
    dt_ioppr_transform_image_colorspace(module, input, input, roi_in->width, roi_in->height, input_format->cst,
                                        module->input_colorspace(module, pipe, piece), &input_format->cst,
                                        work_profile);
    dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, 0, FALSE, input_entry);

    /* now call process_tiling_cl of module; module should emit meaningful messages in case of error */
    dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, 0, TRUE, input_entry);

    int fail = !module->process_tiling_cl(module, piece, input, *output, roi_in, roi_out, in_bpp);
    // We must fully synchronize the command queue here: the next steps run on CPU and will
    // access the output buffer directly.
    dt_opencl_finish(pipe->devid);
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
  }
  else
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_pixelpipe] could not run module '%s' on gpu. falling back to cpu path\n",
             module->name());

    goto error;
  }

  // if (rand() % 20 == 0) success_opencl = FALSE; // Test code: simulate spurious failures

  // Always resync the GPU output to the host cache buffer for correctness, even when we keep the
  // device buffer for downstream GPU modules. This avoids stale/garbled host copies when a later CPU
  // stage (or the GUI) reads the cache without forcing GPU caching.
  // clean up OpenCL input memory and resync pipeline
  _gpu_clear_buffer(&cl_mem_input, input_entry, input, input_cst_cl);

  // Wait for kernels and copies to complete before accessing the cache again and releasing the locks
  // Don't rely solely on the OpenCL event list here: not all drivers/modules consistently track
  // every queued command with an event. We must ensure the whole queue is idle before we release
  // cache refs/locks (auto-destroy may free host buffers).
  dt_opencl_finish(pipe->devid);
  if(locked_input_entry)
    dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, 0, FALSE, locked_input_entry);

  return 0;

  // any error in OpenCL ends here
  // free everything and fall back to CPU processing
error:;

  dt_print(DT_DEBUG_OPENCL, "[dev_pixelpipe] %s couldn't process on GPU\n", module->name());

  // don't delete RAM output even if requested. If we fallback to CPU,
  // we want to keep a cache copy for performance.
  piece->force_opencl_cache = TRUE; 

  return _gpu_cpu_fallback_from_opencl_error(pipe, dev, input, cl_mem_input, input_format, input_cst_cl, roi_in,
                                            output, cl_mem_output, out_format, roi_out, module, piece, tiling,
                                            pixelpipe_flow, in_bpp, cpu_input_entry, output_entry, locked_input_entry);
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
                                             TRUE, output, out_format, &cache_entry);
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
        // last minute clamping to catch potential out-of-bounds in roi_in and roi_out
        // FIXME: this is too late to catch this. Find out why it's needed here and fix upstream.
        const int in_x = MAX(roi_in.x, 0);
        const int in_y = MAX(roi_in.y, 0);
        const int cp_width = MIN(roi_out.width, pipe->iwidth - in_x);
        const int cp_height = MIN(roi_out.height, pipe->iheight - in_y);

        _copy_buffer((const char *const)buf.buf, (char *const)*output, cp_height, roi_out.width,
                      pipe->iwidth, in_x, in_y, bpp * cp_width, bpp);

        dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
        err = 0;
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

// recursive helper for process:
static int dt_dev_pixelpipe_process_rec(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, void **output,
                                        void **cl_mem_output, dt_iop_buffer_dsc_t **out_format,
                                        uint64_t *out_hash, dt_iop_roi_t roi_out, GList *pieces, int pos)
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

  if(pieces)
  {
    piece = (dt_dev_pixelpipe_iop_t *)pieces->data;

    if(piece) 
    {
      roi_out = piece->planned_roi_out;
      roi_in = piece->planned_roi_in;
      module = piece->module;
    }

    // skip this module?
    if(!piece->enabled)
      return dt_dev_pixelpipe_process_rec(pipe, dev, output, cl_mem_output, out_format, out_hash, roi_in,
                                          g_list_previous(pieces), pos - 1);

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
  if(!bypass_cache && !pipe->reentry
     && dt_dev_pixelpipe_cache_get_existing(darktable.pixelpipe_cache, hash, output, out_format, &existing_cache))
  {
    dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, hash, TRUE, existing_cache);
    dt_print(DT_DEBUG_PIPE, "[dev_pixelpipe] found %" PRIu64 " (%s) for %s pipeline in cache\n", hash, module ? module->op : "noop",
             dt_pixelpipe_get_pipe_name(pipe->type));
    if(out_hash) *out_hash = hash;
    return 0;
  }

  // 2) no module means step 0 of the pipe : importing the input buffer
  if(!module)
  {
    dt_times_t start;
    dt_get_times(&start);

    if(_init_base_buffer(pipe, dev, output, cl_mem_output, out_format, roi_in, roi_out, hash, bypass_cache, bufsize,
                      bpp))
    {
      // On error: release the cache line
      dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, hash, FALSE, NULL);
      return 1;
    }

    dt_show_times_f(&start, "[dev_pixelpipe]", "initing base buffer [%s]", dt_pixelpipe_get_pipe_name(pipe->type));
    if(out_hash) *out_hash = hash;
    return 0;
  }

  // 3) now recurse through the pipeline.
  dt_iop_buffer_dsc_t _input_format = { 0 };
  dt_iop_buffer_dsc_t *input_format = &_input_format;
  piece = (dt_dev_pixelpipe_iop_t *)pieces->data;

  uint64_t input_hash = 0;
  if(dt_dev_pixelpipe_process_rec(pipe, dev, &input, &cl_mem_input, &input_format, &input_hash, roi_in,
                                  g_list_previous(pieces), pos - 1))
  {
    // On error: release the cache line
    dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, hash, FALSE, NULL);
    return 1;
  }

  KILL_SWITCH_ABORT;

  const size_t in_bpp = dt_iop_buffer_dsc_to_bpp(input_format);
  piece->dsc_out = piece->dsc_in = *input_format;
  module->output_format(module, pipe, piece, &piece->dsc_out);
  **out_format = pipe->dsc = piece->dsc_out;
  const size_t out_bpp = dt_iop_buffer_dsc_to_bpp(*out_format);

  // Get cache line for input as early as possible: this is needed for correctness (locks/refcounts)
  // and to ensure `input` points to the host buffer when it exists.
  dt_pixel_cache_entry_t *input_entry = NULL;
  if(!dt_dev_pixelpipe_cache_get_existing(darktable.pixelpipe_cache, input_hash, &input, &input_format, &input_entry))
  {
    dt_print(DT_DEBUG_OPENCL, "[dev_pixelpipe] %s has no cache-backed input buffer\n", module->name());
    return 1;
  }

  // Note: input == NULL is valid if we are on a GPU-only path, aka previous module ran on GPU
  // without leaving its output on a RAM cache copy, and current module will also run on GPU.
  // In this case, we rely on cl_mem_input for best performance (avoid memcpy between RAM and GPU).
  // Should the GPU path fail at process time, we will init input and flush cl_mem_input into it.
  // In any case, this avoids carrying a possibly-uninited input buffer, without knowing if it has
  // data on it (or having to blindly copy back from vRAM to RAM).

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
    *cl_mem_output = cl_mem_input;
    if(out_hash) *out_hash = input_hash;
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

  // Get cache line for output, possibly allocating a new one for output
  // Immediately alloc output buffer only if we know we force the use of the cache.
  // Otherwise, it's handled in OpenCL fallbacks.
  dt_pixel_cache_entry_t *output_entry = NULL;
  gchar *type = dt_pixelpipe_get_pipe_name(pipe->type);
  gchar *name = g_strdup_printf("module %s (%s) for pipe %s", module->op, module->multi_name, type);
  const gboolean alloc_output = piece->force_opencl_cache;
  gboolean new_entry = dt_dev_pixelpipe_cache_get(darktable.pixelpipe_cache, hash, bufsize, name, pipe->type,
                                                  alloc_output, output, out_format, &output_entry);
  g_free(name);
  if(output_entry == NULL) 
  {
    // On error: release the cache line
    dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, input_hash, FALSE, input_entry);
    return 1;
  }

  dt_pixelpipe_flow_t pixelpipe_flow = (PIXELPIPE_FLOW_NONE | PIXELPIPE_FLOW_HISTOGRAM_NONE);

  // If we found an existing cache entry for this hash (= !new_entry), and
  // bypassing the cache is not requested by the pipe, stop before processing.
  // This is mostly for the preview pipe since we didn't stop the recursion earlier
  // at the last-found cache line.
  if(!pipe->reentry && !new_entry && pipe->type == DT_DEV_PIXELPIPE_PREVIEW && input)
  {
    dt_print(DT_DEBUG_PIPE, "[pipeline] found %" PRIu64 " (%s) for %s pipeline in cache\n", hash, module ? module->op : "noop", type);

    // Sample all color pickers and histograms
    _sample_gui(pipe, dev, input, output, roi_in, roi_out, input_format, out_format, module,
                piece, input_hash, hash, in_bpp, bpp, input_entry, output_entry);

    // Note: the write lock is not held here since it's not a new entry.
    dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, input_hash, FALSE, input_entry);

    if(out_hash) *out_hash = hash;
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

  const char *prev_module = dt_pixelpipe_cache_set_current_module(module ? module->op : NULL);

#ifdef HAVE_OPENCL
  error = pixelpipe_process_on_GPU(pipe, dev, input, cl_mem_input, input_format, &roi_in, output, cl_mem_output,
                                   out_format, &roi_out, module, piece, &tiling, &pixelpipe_flow, in_bpp, bpp, 
                                   input_entry, output_entry);
#else
  error = pixelpipe_process_on_CPU(pipe, dev, input, input_format, &roi_in, output, out_format, &roi_out, module,
                                   piece, &tiling, &pixelpipe_flow, input_entry);
#endif

  dt_pixelpipe_cache_set_current_module(prev_module);

  _print_perf_debug(pipe, pixelpipe_flow, piece, module, &start);

  if(dev->gui_attached) dev->progress.completed++;   

  if(error)
  {
    // Ensure we always release locks and cache references on error, otherwise cache eviction/GC will stall.
    dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, hash, FALSE, output_entry);
    dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, input_hash, FALSE, input_entry);
    dt_dev_pixelpipe_cache_auto_destroy_apply(darktable.pixelpipe_cache, input_hash, input_entry);

    // No point in keeping garbled output
    dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, hash, FALSE, output_entry);
    if(dt_dev_pixelpipe_cache_remove(darktable.pixelpipe_cache, hash, TRUE, output_entry))
      dt_dev_pixelpipe_cache_flag_auto_destroy(darktable.pixelpipe_cache, hash, output_entry);
    return 1;
  }

  // Flag to throw away the output as soon as we are done consuming it in this thread, at the next module.
  // Cache bypass is requested by modules like crop/perspective, when they show the full image,
  // and when doing anything transient.
  if(bypass_cache || pipe->reentry || !piece->force_opencl_cache)
    dt_dev_pixelpipe_cache_flag_auto_destroy(darktable.pixelpipe_cache, hash, output_entry);

  // in case we get this buffer from the cache in the future, cache some stuff:
  **out_format = piece->dsc_out = pipe->dsc;

  // Release the output write lock before we potentially read it back for GUI sampling/debug.
  dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, hash, FALSE, output_entry);

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

  KILL_SWITCH_AND_FLUSH_CACHE;

  // Sample all color pickers and histograms
  if(piece->force_opencl_cache && input)
    _sample_gui(pipe, dev, input, output, roi_in, roi_out, input_format, out_format, module, piece, input_hash,
                hash, in_bpp, bpp, input_entry, output_entry);

  // Decrease reference count on input and flush it if it was flagged for auto destroy previously
  dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, input_hash, FALSE, input_entry);
  dt_dev_pixelpipe_cache_auto_destroy_apply(darktable.pixelpipe_cache, input_hash, input_entry);

  // Print min/max/Nan in debug mode only
  if((darktable.unmuted & DT_DEBUG_NAN) && strcmp(module->op, "gamma") != 0 && *output)
  {
    dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, hash, TRUE, output_entry);
    _print_nan_debug(pipe, *cl_mem_output, *output, &roi_out, *out_format, module, bpp);
    dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, hash, FALSE, output_entry);
  }

  KILL_SWITCH_AND_FLUSH_CACHE;

  if(out_hash) *out_hash = hash;
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
    dt_pthread_mutex_unlock(&darktable.pipeline_threadsafe);                                                      \
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

static void _update_backbuf_cache_reference(dt_dev_pixelpipe_t *pipe, dt_iop_roi_t roi, dt_pixel_cache_entry_t *entry)
{
  // Keep exactly one cache reference to the last valid output ("backbuf") for display.
  // This prevents the cache entry from being evicted while still in use by the GUI,
  // without leaking references on repeated cache hits.
  if(pipe->backbuf.hash == pipe->hash) return;

  dt_dev_pixelpipe_cache_unref_hash(darktable.pixelpipe_cache, pipe->backbuf.hash);

  int bpp = 0;
  if(entry && roi.width > 0 && roi.height > 0)
    bpp = (int)(dt_pixel_cache_entry_get_size(entry) / ((size_t)roi.width * (size_t)roi.height));

  dt_dev_set_backbuf(&pipe->backbuf, roi.width, roi.height, bpp, pipe->hash, pipe->history_hash);
}

static void _set_opencl_cache(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev)
{
  // Starting with the end of the pipe, gamma sends its buffer to GUI, so it needs RAM caching.
  // Any module not supporting OpenCL will set this to TRUE for the previous
  gboolean opencl_cache = TRUE;

  for(GList *pieces = g_list_last(pipe->nodes); pieces; pieces = g_list_previous(pieces))
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)pieces->data;
    dt_iop_module_t *module = piece->module;

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

      // Get user caching requirements
      gchar *string = g_strdup_printf("/plugins/%s/cache", module->op);
      
      if(!dt_conf_key_exists(string) || !dt_conf_key_not_empty(string)) 
          dt_conf_set_bool(string, piece->force_opencl_cache);

      piece->force_opencl_cache = dt_conf_get_bool(string);

      g_free(string);

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

      //fprintf(stdout, "%s has OpenCL cache %i, requested intern %i, requested next %i\n", piece->module->op, piece->force_opencl_cache, requested, opencl_cache);

      gboolean active_in_gui 
          = (dev->gui_attached && dev->gui_module == module);

      // previous module in pipeline will need to cache its output to RAM
      // if the current one doesn't handle OpenCL or is being edited
      opencl_cache = !supports_opencl || active_in_gui;
    }
  }
}

int dt_dev_pixelpipe_process(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, dt_iop_roi_t roi)
{
  if(darktable.unmuted & DT_DEBUG_MEMORY)
  {
    fprintf(stderr, "[memory] before pixelpipe process\n");
    dt_print_mem_usage();
  }

  dt_dev_pixelpipe_cache_print(darktable.pixelpipe_cache);

  // Get the roi_out hash of all nodes.
  // Get the previous output size of the module, for cache invalidation.
  dt_dev_pixelpipe_get_roi_in(pipe, dev, roi);
  dt_pixelpipe_get_global_hash(pipe, dev);
  const guint pos = g_list_length(dev->iop);
  _set_opencl_cache(pipe, dev);

  void *buf = NULL;

  // If the last backbuf image is still valid with regard to current pipe topology 
  // and history, and we still have an entry cache, abort now. Nothing to do.
  // For preview pipe, if using color pickers, we still need to traverse the pipeline.
  dt_pixel_cache_entry_t *entry = NULL;
  if(!pipe->reentry && !pipe->bypass_cache 
     && dt_dev_pixelpipe_cache_get_existing(darktable.pixelpipe_cache, pipe->hash, &buf, NULL, &entry)
     && _resync_global_histograms(pipe, dev))
  {
    // When GUI consumes the backbuf, it decreases the ref_count,
    // so we need to tell the cache we still need it.
    dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, pipe->hash, TRUE, entry);

    _update_backbuf_cache_reference(pipe, roi, entry);

    return 0;
  }

  // Flag backbuf as invalid
  dt_dev_pixelpipe_cache_unref_hash(darktable.pixelpipe_cache, pipe->backbuf.hash);
  pipe->backbuf.hash = -1;

  dt_print(DT_DEBUG_DEV, "[pixelpipe] Started %s pipeline recompute at %i×%i px\n", 
           dt_pixelpipe_get_pipe_name(pipe->type), roi.width, roi.height);

  // get a snapshot of the mask list
  dt_pthread_rwlock_rdlock(&dev->masks_mutex);
  pipe->forms = dt_masks_dup_forms_deep(dev->forms, NULL);
  dt_pthread_rwlock_unlock(&dev->masks_mutex);

  // go through the list of modules from the end:
  GList *pieces = g_list_last(pipe->nodes);

  // Because it's possible here that we export at full resolution,
  // and our memory planning doesn't account for several concurrent pipelines
  // at full size, we allow only one pipeline at a time to run.
  // This is because wavelets decompositions and such use 6 copies,
  // so the RAM usage can go out of control here.
  dt_pthread_mutex_lock(&darktable.pipeline_threadsafe);

  pipe->opencl_enabled = dt_opencl_update_settings(); // update enabled flag and profile from preferences
  pipe->devid = (pipe->opencl_enabled) ? dt_opencl_lock_device(pipe->type)
                                       : -1; // try to get/lock opencl resource

  if(pipe->devid > -1) dt_opencl_events_reset(pipe->devid);
  dt_print(DT_DEBUG_OPENCL, "[pixelpipe_process] [%s] using device %d\n", dt_pixelpipe_get_pipe_name(pipe->type),
           pipe->devid);

  KILL_SWITCH_PIPE

  gboolean keep_running = TRUE;
  int runs = 0;
  int opencl_error = 0;
  int err = 0;

  while(keep_running && runs < 3)
  {
    ++runs;

#ifdef HAVE_OPENCL
    dt_opencl_check_tuning(pipe->devid);
#endif

    // WARNING: buf will actually be a reference to a pixelpipe cache line, so it will be freed
    // when the cache line is flushed or invalidated.
    void *cl_mem_out = NULL;
    uint64_t buf_hash = 0;

    dt_iop_buffer_dsc_t _out_format = { 0 };
    dt_iop_buffer_dsc_t *out_format = &_out_format;

    KILL_SWITCH_PIPE

    dt_times_t start;
    dt_get_times(&start);
    err = dt_dev_pixelpipe_process_rec(pipe, dev, &buf, &cl_mem_out, &out_format, &buf_hash, roi, pieces, pos);
    gchar *msg = g_strdup_printf("[pixelpipe] %s internal pixel pipeline processing", dt_pixelpipe_get_pipe_name(pipe->type));
    dt_show_times(&start, msg);
    g_free(msg);

	  // The pipeline has copied cl_mem_out into buf, so we can release it now.
	  #ifdef HAVE_OPENCL
	    _gpu_clear_buffer(&cl_mem_out, NULL, NULL, IOP_CS_NONE);
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
    }
    else if(!dt_atomic_get_int(&pipe->shutdown))
    {
      // No opencl errors, no killswitch triggered: we should have a valid output buffer now.
      dt_pixel_cache_entry_t *final_entry = NULL;
      if(dt_dev_pixelpipe_cache_get_existing(darktable.pixelpipe_cache, pipe->hash, NULL, NULL, &final_entry))
        _update_backbuf_cache_reference(pipe, roi, final_entry);
      else
        _update_backbuf_cache_reference(pipe, roi, NULL);

      // Note : the last output (backbuf) of the pixelpipe cache is internally locked
      // Whatever consuming it will need to unlock it.
    }
  }

  dt_pthread_mutex_unlock(&darktable.pipeline_threadsafe);

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
                && module->planned_roi_in.width == 0
                && module->planned_roi_in.height == 0))
        {
          float *transformed_mask = dt_pixelpipe_cache_alloc_align_float_cache(module->planned_roi_out.width
                                                                               * module->planned_roi_out.height, 0);
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
                                      &module->planned_roi_in,
                                      &module->planned_roi_out);
          if(*free_mask) dt_pixelpipe_cache_free_align(raster_mask);
          *free_mask = TRUE;
          raster_mask = transformed_mask;
          dt_print(DT_DEBUG_MASKS, "[raster masks] doing transform\n");
        }
        else if(!module->module->distort_mask &&
                (module->planned_roi_in.width != module->planned_roi_out.width ||
                  module->planned_roi_in.height != module->planned_roi_out.height ||
                  module->planned_roi_in.x != module->planned_roi_out.x ||
                  module->planned_roi_in.y != module->planned_roi_out.y))
          fprintf(stderr, "FIXME: module `%s' changed the roi from %d x %d @ %d / %d to %d x %d | %d / %d but doesn't have "
                  "distort_mask() implemented!\n", module->module->op, module->planned_roi_in.width,
                  module->planned_roi_in.height, module->planned_roi_in.x, module->planned_roi_in.y,
                  module->planned_roi_out.width, module->planned_roi_out.height, module->planned_roi_out.x,
                  module->planned_roi_out.y);
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
  dt_pixelpipe_cache_free_align(pipe->rawdetail_mask_data);
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
  float *mask = dt_pixelpipe_cache_alloc_align_float_cache((size_t)width * height, 0);
  float *tmp = dt_pixelpipe_cache_alloc_align_float_cache((size_t)width * height, 0);
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
  dt_pixelpipe_cache_free_align(tmp);
  dt_print(DT_DEBUG_MASKS, "[dt_dev_write_rawdetail_mask] %i (%ix%i)\n", mode, roi_in->width, roi_in->height);
  return FALSE;

  error:
  fprintf(stderr, "[dt_dev_write_rawdetail_mask] couldn't write detail mask\n");
  dt_pixelpipe_cache_free_align(mask);
  dt_pixelpipe_cache_free_align(tmp);
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
  mask = dt_pixelpipe_cache_alloc_align_float_cache((size_t)width * height, 0);
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
  dt_pixelpipe_cache_free_align(mask);
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
                    && module->planned_roi_in.width == 0
                    && module->planned_roi_in.height == 0))
        {
          float *tmp = dt_pixelpipe_cache_alloc_align_float_cache((size_t)module->planned_roi_out.width * module->planned_roi_out.height, 0);
          dt_vprint(DT_DEBUG_MASKS, "   %s %ix%i -> %ix%i\n", module->module->op, module->planned_roi_in.width, module->planned_roi_in.height, module->planned_roi_out.width, module->planned_roi_out.height);
          module->module->distort_mask(module->module, module, inmask, tmp, &module->planned_roi_in, &module->planned_roi_out);
          resmask = tmp;
          if(inmask != src) dt_pixelpipe_cache_free_align(inmask);
          inmask = tmp;
        }
        else if(!module->module->distort_mask &&
                (module->planned_roi_in.width != module->planned_roi_out.width ||
                 module->planned_roi_in.height != module->planned_roi_out.height ||
                 module->planned_roi_in.x != module->planned_roi_out.x ||
                 module->planned_roi_in.y != module->planned_roi_out.y))
              fprintf(stderr, "FIXME: module `%s' changed the roi from %d x %d @ %d / %d to %d x %d | %d / %d but doesn't have "
                 "distort_mask() implemented!\n", module->module->op, module->planned_roi_in.width,
                 module->planned_roi_in.height, module->planned_roi_in.x, module->planned_roi_in.y,
                 module->planned_roi_out.width, module->planned_roi_out.height, module->planned_roi_out.x,
                 module->planned_roi_out.y);

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
