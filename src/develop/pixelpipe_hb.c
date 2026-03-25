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
#include "develop/dev_pixelpipe.h"
#include "develop/format.h"
#include "develop/imageop_math.h"
#include "develop/pixelpipe.h"
#include "develop/pixelpipe_cache.h"
#include "develop/pixelpipe_cpu.h"
#include "develop/pixelpipe_gpu.h"
#include "develop/pixelpipe_gui.h"
#include "develop/pixelpipe_process.h"
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

#include "develop/pixelpipe_gui.c"
#include "develop/pixelpipe_raster_masks.c"
#include "develop/pixelpipe_rawdetail.c"

static void _trace_cache_owner(const dt_dev_pixelpipe_t *pipe, const dt_iop_module_t *module,
                               const char *phase, const char *slot, const uint64_t requested_hash,
                               const void *buffer, const dt_pixel_cache_entry_t *entry,
                               const gboolean verbose)
{
  if(!(darktable.unmuted & DT_DEBUG_CACHE)) return;
  if(verbose && !(darktable.unmuted & DT_DEBUG_VERBOSE)) return;

  dt_print(DT_DEBUG_CACHE,
           "[pixelpipe_owner] pipe=%s module=%s phase=%s slot=%s req=%" PRIu64
           " entry=%" PRIu64 "/%" PRIu64 " refs=%i auto=%i data=%p buf=%p name=%s\n",
           pipe ? dt_pixelpipe_get_pipe_name(pipe->type) : "-",
           module ? module->op : "base",
           phase ? phase : "-",
           slot ? slot : "-",
           requested_hash,
           entry ? entry->hash : DT_PIXELPIPE_CACHE_HASH_INVALID,
           entry ? entry->serial : 0,
           entry ? dt_atomic_get_int((dt_atomic_int *)&entry->refcount) : -1,
           entry ? entry->auto_destroy : -1,
           entry ? entry->data : NULL,
           buffer,
           (entry && entry->name) ? entry->name : "-");
}


static void _trace_buffer_content(const dt_dev_pixelpipe_t *pipe, const dt_iop_module_t *module,
                                  const char *phase, const void *buffer,
                                  const dt_iop_buffer_dsc_t *format, const dt_iop_roi_t *roi)
{
  if(!(darktable.unmuted & DT_DEBUG_CACHE)) return;
  if(!(darktable.unmuted & DT_DEBUG_VERBOSE)) return;
  if(!buffer || !format || !roi) return;
  if(roi->width <= 0 || roi->height <= 0) return;

  const size_t pixels = (size_t)roi->width * (size_t)roi->height;
  const unsigned int channels = format->channels;

  if(format->datatype == TYPE_FLOAT && channels >= 1)
  {
    const float *in = (const float *)buffer;
    float minv[4] = { FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX };
    float maxv[4] = { -FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX };
    size_t nonfinite = 0;
    size_t near_black = 0;

    for(size_t k = 0; k < pixels; k++, in += channels)
    {
      gboolean finite = TRUE;
      for(unsigned int c = 0; c < MIN(channels, 4U); c++)
      {
        if(!isfinite(in[c]))
        {
          finite = FALSE;
          continue;
        }
        minv[c] = fminf(minv[c], in[c]);
        maxv[c] = fmaxf(maxv[c], in[c]);
      }

      if(!finite)
      {
        nonfinite++;
        continue;
      }

      const float energy = fabsf(in[0]) + ((channels > 1) ? fabsf(in[1]) : 0.0f)
                           + ((channels > 2) ? fabsf(in[2]) : 0.0f);
      if(energy < 1e-6f) near_black++;
    }

    dt_print(DT_DEBUG_CACHE,
             "[pixelpipe_stats] pipe=%s module=%s phase=%s type=float ch=%u roi=%dx%d "
             "rgb_min=(%g,%g,%g) rgb_max=(%g,%g,%g) a_min=%g a_max=%g near_black=%zu/%zu nonfinite=%zu\n",
             dt_pixelpipe_get_pipe_name(pipe->type), module->op, phase ? phase : "-",
             channels, roi->width, roi->height,
             minv[0], (channels > 1) ? minv[1] : 0.0f, (channels > 2) ? minv[2] : 0.0f,
             maxv[0], (channels > 1) ? maxv[1] : 0.0f, (channels > 2) ? maxv[2] : 0.0f,
             (channels > 3) ? minv[3] : 0.0f, (channels > 3) ? maxv[3] : 0.0f,
             near_black, pixels, nonfinite);
  }
  else if(format->datatype == TYPE_UINT8 && channels >= 1)
  {
    const uint8_t *in = (const uint8_t *)buffer;
    int minv[4] = { 255, 255, 255, 255 };
    int maxv[4] = { 0, 0, 0, 0 };
    size_t near_black = 0;

    for(size_t k = 0; k < pixels; k++, in += channels)
    {
      for(unsigned int c = 0; c < MIN(channels, 4U); c++)
      {
        minv[c] = MIN(minv[c], in[c]);
        maxv[c] = MAX(maxv[c], in[c]);
      }

      const int energy = in[0] + ((channels > 1) ? in[1] : 0) + ((channels > 2) ? in[2] : 0);
      if(energy == 0) near_black++;
    }

    dt_print(DT_DEBUG_CACHE,
             "[pixelpipe_stats] pipe=%s module=%s phase=%s type=u8 ch=%u roi=%dx%d "
             "rgb_min=(%d,%d,%d) rgb_max=(%d,%d,%d) a_min=%d a_max=%d near_black=%zu/%zu\n",
             dt_pixelpipe_get_pipe_name(pipe->type), module->op, phase ? phase : "-",
             channels, roi->width, roi->height,
             minv[0], (channels > 1) ? minv[1] : 0, (channels > 2) ? minv[2] : 0,
             maxv[0], (channels > 1) ? maxv[1] : 0, (channels > 2) ? maxv[2] : 0,
             (channels > 3) ? minv[3] : 0, (channels > 3) ? maxv[3] : 0,
             near_black, pixels);
  }
}

static int _abort_module_shutdown_cleanup(dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                                          dt_iop_module_t *module, const uint64_t input_hash,
                                          const void *input, dt_pixel_cache_entry_t *input_entry,
                                          const uint64_t output_hash, void **output,
                                          void **cl_mem_output, dt_pixel_cache_entry_t *output_entry)
{
  _trace_cache_owner(pipe, module, "shutdown-drop", "input", input_hash, input, input_entry, FALSE);
  _trace_cache_owner(pipe, module, "shutdown-drop", "output", output_hash,
                     output ? *output : NULL, output_entry, FALSE);

  _reset_piece_cache_entry(piece);

  if(input_entry)
  {
    dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, FALSE, input_entry);
    dt_dev_pixelpipe_cache_auto_destroy_apply(darktable.pixelpipe_cache, input_entry);
  }

  if(output_entry)
  {
    dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, FALSE, output_entry);

    if(dt_dev_pixelpipe_cache_remove(darktable.pixelpipe_cache, TRUE, output_entry))
      dt_dev_pixelpipe_cache_flag_auto_destroy(darktable.pixelpipe_cache, output_entry);
  }

  if(output) *output = NULL;

  if(*cl_mem_output != NULL)
    dt_dev_pixelpipe_cache_release_cl_buffer(cl_mem_output, NULL, NULL, FALSE);

  dt_iop_nap(5000);
  return 1;
}

static inline gboolean _is_focused_realtime_gui_module(const dt_dev_pixelpipe_t *pipe,
                                                       const dt_develop_t *dev,
                                                       const dt_iop_module_t *module)
{
  return pipe && (pipe->type == DT_DEV_PIXELPIPE_FULL || pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
         && pipe->realtime && dev && module && dev->gui_module == module;
}


gboolean dt_dev_pixelpipe_cache_gpu_device_buffer(const dt_dev_pixelpipe_t *pipe,
                                                  const dt_pixel_cache_entry_t *cache_entry)
{
  return pipe && (pipe->realtime
             || (cache_entry
                 && dt_pixel_cache_entry_get_data((dt_pixel_cache_entry_t *)cache_entry) == NULL));
}

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

static const char *_debug_cst_to_string(const int cst)
{
  switch(cst)
  {
    case IOP_CS_RAW:
      return "raw";
    case IOP_CS_LAB:
      return "lab";
    case IOP_CS_RGB:
      return "rgb";
    case IOP_CS_LCH:
      return "lch";
    case IOP_CS_HSL:
      return "hsl";
    case IOP_CS_JZCZHZ:
      return "jzczhz";
    case IOP_CS_NONE:
      return "none";
    default:
      return "unknown";
  }
}

static const char *_debug_type_to_string(const dt_iop_buffer_type_t type)
{
  switch(type)
  {
    case TYPE_FLOAT:
      return "float";
    case TYPE_UINT16:
      return "uint16";
    case TYPE_UINT8:
      return "uint8";
    case TYPE_UNKNOWN:
    default:
      return "unknown";
  }
}

void dt_dev_pixelpipe_debug_dump_module_io(dt_dev_pixelpipe_t *pipe, dt_iop_module_t *module, const char *stage,
                                           const gboolean is_cl,
                                           const dt_iop_buffer_dsc_t *in_dsc, const dt_iop_buffer_dsc_t *out_dsc,
                                           const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                                           const size_t in_bpp, const size_t out_bpp,
                                           const int cst_before, const int cst_after)
{
  if(!(darktable.unmuted & DT_DEBUG_PIPE)) return;
  if(!(darktable.unmuted & DT_DEBUG_VERBOSE)) return;
  const char *module_name = module ? module->op : "base";
  const char *pipe_name = dt_pixelpipe_get_pipe_name(pipe->type);
  const char *stage_name = stage ? stage : "process";

  if(in_dsc && out_dsc)
  {
    dt_print(DT_DEBUG_PIPE,
             "[pixelpipe] %s %s %s %s: in cst=%s->%s ch=%d type=%s bpp=%zu roi=%dx%d | "
             "out cst=%s ch=%d type=%s bpp=%zu roi=%dx%d\n",
             pipe_name, module_name, is_cl ? "cl" : "cpu", stage_name,
             _debug_cst_to_string(cst_before), _debug_cst_to_string(cst_after),
             in_dsc->channels, _debug_type_to_string(in_dsc->datatype), in_bpp,
             roi_in ? roi_in->width : 0, roi_in ? roi_in->height : 0,
             _debug_cst_to_string(out_dsc->cst), out_dsc->channels, _debug_type_to_string(out_dsc->datatype),
             out_bpp, roi_out ? roi_out->width : 0, roi_out ? roi_out->height : 0);
  }
  else if(out_dsc)
  {
    dt_print(DT_DEBUG_PIPE,
             "[pixelpipe] %s %s %s %s: out cst=%s ch=%d type=%s bpp=%zu roi=%dx%d\n",
             pipe_name, module_name, is_cl ? "cl" : "cpu", stage_name,
             _debug_cst_to_string(out_dsc->cst), out_dsc->channels, _debug_type_to_string(out_dsc->datatype),
             out_bpp, roi_out ? roi_out->width : 0, roi_out ? roi_out->height : 0);
  }
}


int dt_dev_pixelpipe_init_export(dt_dev_pixelpipe_t *pipe, int levels, gboolean store_masks)
{
  const int res = dt_dev_pixelpipe_init_cached(pipe);
  pipe->type = DT_DEV_PIXELPIPE_EXPORT;
  pipe->gui_observable_source = FALSE;
  pipe->levels = levels;
  pipe->store_all_raster_masks = store_masks;
  return res;
}

int dt_dev_pixelpipe_init_thumbnail(dt_dev_pixelpipe_t *pipe)
{
  const int res = dt_dev_pixelpipe_init_cached(pipe);
  pipe->type = DT_DEV_PIXELPIPE_THUMBNAIL;
  pipe->no_cache = TRUE;
  return res;
}

int dt_dev_pixelpipe_init_dummy(dt_dev_pixelpipe_t *pipe)
{
  const int res = dt_dev_pixelpipe_init_cached(pipe);
  pipe->type = DT_DEV_PIXELPIPE_THUMBNAIL;
  pipe->no_cache = TRUE;
  return res;
}

int dt_dev_pixelpipe_init_preview(dt_dev_pixelpipe_t *pipe)
{
  // Init with the size of MIPMAP_F
  const int res = dt_dev_pixelpipe_init_cached(pipe);
  pipe->type = DT_DEV_PIXELPIPE_PREVIEW;
  pipe->gui_observable_source = TRUE;

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
  // Set everything to 0 = NULL = FALSE
  memset(pipe, 0, sizeof(dt_dev_pixelpipe_t));

  // Set only the stuff that doesn't take 0 as default
  pipe->devid = -1;
  dt_dev_pixelpipe_set_changed(pipe, DT_DEV_PIPE_UNCHANGED);
  dt_dev_pixelpipe_set_hash(pipe, DT_PIXELPIPE_CACHE_HASH_INVALID);
  dt_dev_pixelpipe_set_history_hash(pipe, DT_PIXELPIPE_CACHE_HASH_INVALID);
  dt_dev_set_backbuf(&pipe->backbuf, 0, 0, 0, -1, -1);
  pipe->last_history_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;

  pipe->output_imgid = UNKNOWN_IMAGE;
  dt_atomic_set_int(&pipe->shutdown, FALSE);
  dt_atomic_set_int(&pipe->realtime, FALSE);

  pipe->levels = IMAGEIO_RGB | IMAGEIO_INT8;
  dt_pthread_mutex_init(&(pipe->busy_mutex), NULL);

  pipe->icc_type = DT_COLORSPACE_NONE;
  pipe->icc_intent = DT_INTENT_LAST;

  dt_dev_pixelpipe_reset_reentry(pipe);
  return 1;
}

void dt_dev_pixelpipe_set_realtime(dt_dev_pixelpipe_t *pipe, gboolean state)
{
  if(!pipe) return;
  dt_atomic_set_int(&pipe->realtime, state ? TRUE : FALSE);
}

gboolean dt_dev_pixelpipe_get_realtime(const dt_dev_pixelpipe_t *pipe)
{
  return pipe ? dt_atomic_get_int((dt_atomic_int *)&pipe->realtime) : FALSE;
}

void dt_dev_pixelpipe_set_input(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, int32_t imgid, int width, int height,
                                dt_mipmap_size_t size)
{
  pipe->iwidth = width;
  pipe->iheight = height;
  pipe->imgid = imgid;
  pipe->image = dev->image_storage;
  pipe->size = size;
}

void dt_dev_pixelpipe_set_icc(dt_dev_pixelpipe_t *pipe, dt_colorspaces_color_profile_type_t icc_type,
                              const gchar *icc_filename, dt_iop_color_intent_t icc_intent)
{
  pipe->icc_type = icc_type;
  dt_free(pipe->icc_filename);
  pipe->icc_filename = g_strdup(icc_filename ? icc_filename : "");
  pipe->icc_intent = icc_intent;
}

void dt_dev_pixelpipe_cleanup(dt_dev_pixelpipe_t *pipe)
{
  /* Device-side cache payloads are only an acceleration layer. Once darkroom
   * leaves and all pipe workers are quiescent, drop all cached cl_mem objects
   * so a later reopen can only exact-hit host-authoritative cachelines. */
  dt_dev_pixelpipe_cache_flush_clmem(darktable.pixelpipe_cache, -1, NULL);

  // blocks while busy and sets shutdown bit:
  dt_dev_pixelpipe_cleanup_nodes(pipe);
  // so now it's safe to clean up cache:
  const uint64_t old_backbuf_hash = dt_dev_backbuf_get_hash(&pipe->backbuf);
  if(pipe->no_cache)
  {
    dt_pixel_cache_entry_t *old_backbuf_entry
        = dt_dev_pixelpipe_cache_get_entry(darktable.pixelpipe_cache, old_backbuf_hash);
    dt_dev_pixelpipe_cache_remove(darktable.pixelpipe_cache, TRUE, old_backbuf_entry);
    dt_dev_set_backbuf(&pipe->backbuf, 0, 0, 0, DT_PIXELPIPE_CACHE_HASH_INVALID, DT_PIXELPIPE_CACHE_HASH_INVALID);
  }
  dt_pthread_mutex_destroy(&(pipe->busy_mutex));
  pipe->icc_type = DT_COLORSPACE_NONE;
  dt_free(pipe->icc_filename);

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
  if(pipe->reentry_hash == DT_PIXELPIPE_CACHE_HASH_INVALID)
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
    pipe->reentry_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
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
  pipe->reentry_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
  pipe->flush_cache = FALSE;
}

void dt_dev_pixelpipe_cleanup_nodes(dt_dev_pixelpipe_t *pipe)
{
  // destroy all nodes
  for(GList *nodes = g_list_first(pipe->nodes); nodes; nodes = g_list_next(nodes))
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
    if(piece == NULL) continue;
    // printf("cleanup module `%s'\n", piece->module->name());
    if(piece->module) dt_iop_cleanup_pipe(piece->module, pipe, piece);
    dt_pixelpipe_raster_cleanup(piece->raster_masks);
    dt_free(piece);
  }
  g_list_free(pipe->nodes);
  pipe->nodes = NULL;
  // and iop order
  g_list_free_full(pipe->iop_order_list, dt_free_gpointer);
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
    if(!piece) continue;
    piece->enabled = module->enabled;
    piece->request_histogram = DT_REQUEST_ONLY_IN_GUI;
    piece->histogram_params.bins_count = 256;
    piece->iwidth = pipe->iwidth;
    piece->iheight = pipe->iheight;
    piece->module = module;
    piece->hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
    piece->blendop_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
    piece->global_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
    piece->global_mask_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
    _reset_piece_cache_entry(piece);
    piece->force_opencl_cache = TRUE;
    piece->raster_masks = dt_pixelpipe_raster_alloc();

    // dsc_mask is static, single channel float image
    piece->dsc_mask.channels = 1;
    piece->dsc_mask.datatype = TYPE_FLOAT;
    dt_iop_buffer_dsc_update_bpp(&piece->dsc_mask);

    dt_iop_init_pipe(piece->module, pipe, piece);
    
    pipe->nodes = g_list_append(pipe->nodes, piece);
  }
}

dt_pixelpipe_blend_transform_t dt_dev_pixelpipe_transform_for_blend(const dt_iop_module_t *const self,
                                                                    const dt_dev_pixelpipe_iop_t *const piece,
                                                                    const dt_iop_buffer_dsc_t *const output_dsc)
{
  const dt_develop_blend_params_t *const d = (const dt_develop_blend_params_t *)piece->blendop_data;
  if(!d) return DT_DEV_PIXELPIPE_BLEND_TRANSFORM_NONE;
  if(!(self->flags() & IOP_FLAGS_SUPPORTS_BLENDING)) return DT_DEV_PIXELPIPE_BLEND_TRANSFORM_NONE;
  if(d->mask_mode == DEVELOP_MASK_DISABLED) return DT_DEV_PIXELPIPE_BLEND_TRANSFORM_NONE;

  const dt_iop_colorspace_type_t blend_cst = dt_develop_blend_colorspace(piece, output_dsc->cst);
  dt_pixelpipe_blend_transform_t transforms = DT_DEV_PIXELPIPE_BLEND_TRANSFORM_NONE;
  if(piece->dsc_in.cst != blend_cst)
    transforms |= DT_DEV_PIXELPIPE_BLEND_TRANSFORM_INPUT;
  if(output_dsc->cst != blend_cst)
    transforms |= DT_DEV_PIXELPIPE_BLEND_TRANSFORM_OUTPUT;

  return transforms;
}

#define KILL_SWITCH_ABORT                                                                                         \
  if(dt_atomic_get_int(&pipe->shutdown))                                                                          \
  {                                                                                                               \
    if(cl_mem_output != NULL)                                                                                     \
    {                                                                                                             \
      dt_dev_pixelpipe_cache_release_cl_buffer(&cl_mem_output, NULL, NULL, FALSE);                                      \
    }                                                                                                             \
    dt_iop_nap(5000);                                                                                             \
    return 1;                                                                                                     \
  }

// Once we have a cache, stopping computation before full completion
// has good chances of leaving it corrupted. So we invalidate it.
#define KILL_SWITCH_AND_FLUSH_CACHE                                                                               \
  if(dt_atomic_get_int(&pipe->shutdown))                                                                          \
  {                                                                                                               \
    return _abort_module_shutdown_cleanup(pipe, piece, module, input_hash, input, input_entry, hash, &output,    \
                                          &cl_mem_output, output_entry);                                          \
  }

static void _print_perf_debug(dt_dev_pixelpipe_t *pipe, const dt_pixelpipe_flow_t pixelpipe_flow,
                              dt_dev_pixelpipe_iop_t *piece, dt_iop_module_t *module,
                              const gboolean recycled_output_cacheline, dt_times_t *start)
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
      start, "[dev_pixelpipe]", "processed `%s' on %s%s%s%s, blended on %s [%s]", module_label,
      pixelpipe_flow & PIXELPIPE_FLOW_PROCESSED_ON_GPU
          ? "GPU"
          : pixelpipe_flow & PIXELPIPE_FLOW_PROCESSED_ON_CPU ? "CPU" : "",
      pixelpipe_flow & PIXELPIPE_FLOW_PROCESSED_WITH_TILING ? " with tiling" : "",
      recycled_output_cacheline ? ", recycled cacheline" : "",
      (!(pixelpipe_flow & PIXELPIPE_FLOW_HISTOGRAM_NONE) && (piece->request_histogram & DT_REQUEST_ON))
          ? histogram_log
          : "",
      pixelpipe_flow & PIXELPIPE_FLOW_BLENDED_ON_GPU
          ? "GPU"
          : pixelpipe_flow & PIXELPIPE_FLOW_BLENDED_ON_CPU ? "CPU" : "",
      dt_pixelpipe_get_pipe_name(pipe->type));
  dt_free(module_label);
}


static void _print_nan_debug(dt_dev_pixelpipe_t *pipe, void *cl_mem_output, void *output,
                             const dt_iop_roi_t *roi_out, dt_iop_buffer_dsc_t *out_format,
                             dt_iop_module_t *module)
{
  if(!(darktable.unmuted & DT_DEBUG_NAN)) return;
  if(!(darktable.unmuted & DT_DEBUG_VERBOSE)) return;
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

    dt_free(module_label);
  }
}

static dt_iop_roi_t *_get_first_roi(dt_dev_pixelpipe_t *pipe)
{
  for(GList *node = g_list_first(pipe->nodes); node; node = g_list_next(node))
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)node->data;
    if(piece && piece->enabled)
      return &piece->roi_in;
  }

  return NULL;
}

// return 1 on error
static int _init_base_buffer(dt_dev_pixelpipe_t *pipe)
{
  /* `dev` is only needed for darkroom pipes to expose the long base-buffer load in the GUI.
   * Headless callers pass NULL and keep this path side-effect free. */
  const dt_iop_buffer_dsc_t *out_format = &pipe->image.dsc;
  const size_t bpp = out_format->bpp;
  const dt_iop_roi_t roi = *_get_first_roi(pipe);
  const size_t bufsize = bpp * roi.width * roi.height;
  const uint64_t hash = dt_dev_pixelpipe_node_hash(pipe, NULL, roi, 0);

  // Note: dt_dev_pixelpipe_cache_get actually init/alloc *output
  dt_pixel_cache_entry_t *cache_entry;
  void *output;
  int new_entry = dt_dev_pixelpipe_cache_get(darktable.pixelpipe_cache, hash, bufsize, "base buffer", pipe->type,
                                             TRUE, &output, &cache_entry);
  if(cache_entry == NULL) return 1;

  int err = 0;
  gboolean host_rewritten = FALSE;

  if(new_entry)
  {
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
    if(!buf.buf || buf.height != pipe->iheight || buf.width != pipe->iwidth || !output)
    {
      // Nothing we can do, we need to recompute roi_in and roi_out from scratch
      // for all modules with new sizes. Exit on error and catch that in develop.
      dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
      err = 1;
    }
    else
    {
      // 1:1 pixel copies.
      if(roi.width > 0 && roi.height > 0)
      {
        // last minute clamping to catch potential out-of-bounds in roi_in and roi_out
        // FIXME: this is too late to catch this. Find out why it's needed here and fix upstream.
        const int in_x = MAX(roi.x, 0);
        const int in_y = MAX(roi.y, 0);
        const int cp_width = MIN(roi.width, pipe->iwidth - in_x);
        const int cp_height = MIN(roi.height, pipe->iheight - in_y);

        _copy_buffer((const char *const)buf.buf, (char *const)output, cp_height, roi.width,
                      pipe->iwidth, in_x, in_y, bpp * cp_width, bpp);
        host_rewritten = TRUE;

        dt_dev_pixelpipe_debug_dump_module_io(pipe, NULL, "base-init", FALSE, NULL, out_format, NULL, &roi,
                              0, bpp, IOP_CS_NONE, IOP_CS_NONE);

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
  }
  // else found in cache.

  if(new_entry)
    dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, FALSE, cache_entry);

  if(!err && host_rewritten)
  {
    dt_dev_pixelpipe_gpu_flush_host_pinned_images(pipe, output, cache_entry, "base buffer copy");
    /* The base buffer now owns the authoritative host pixels for this cacheline. Any older device-only
     * payloads attached to the reused cache entry are stale and must not survive into later thumbnail runs,
     * otherwise a downstream GPU->CPU fallback can reopen previous-image pixels from the same entry. */
    dt_dev_pixelpipe_cache_flush_entry_clmem(cache_entry);
  }

  // For one-shot pipelines (thumbnail export), ensure the base buffer cacheline is not kept around.
  // It will be freed as soon as the next module is done consuming it as input.
  if(pipe->no_cache)
    dt_dev_pixelpipe_cache_flag_auto_destroy(darktable.pixelpipe_cache, cache_entry);

  if(err)
    dt_dev_pixelpipe_cache_unref_hash(darktable.pixelpipe_cache, hash);

  return err;
}

static int dt_dev_pixelpipe_process_rec(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev,
                                        uint64_t *out_hash, const dt_dev_pixelpipe_iop_t **out_piece,
                                        GList *pieces, int pos)
{
  // The pipeline is executed recursively, from the end. For each module n, starting from the end,
  // if output is cached, take it, else if input is cached, take it, process it and output,
  // else recurse to the previous module n-1 to get a an input.
  void *input = NULL;
  void *output = NULL;
  void *cl_mem_output = NULL;

  KILL_SWITCH_ABORT;

  if(pieces == NULL)
  {
    // No pieces means step 0 : init the base buffer
    dt_times_t start;
    dt_get_times(&start);
    if(_init_base_buffer(pipe))
      return 1;

    dt_show_times_f(&start, "[dev_pixelpipe]", "initing base buffer [%s]", dt_pixelpipe_get_pipe_name(pipe->type));
    *out_hash = dt_dev_pixelpipe_node_hash(pipe, NULL, *_get_first_roi(pipe), pos);
    *out_piece = NULL;
    return 0;
  }

  dt_iop_module_t *module = NULL;
  dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)pieces->data;
  
  KILL_SWITCH_ABORT;

  // skip this module?
  if(!piece->enabled)
    return dt_dev_pixelpipe_process_rec(pipe, dev, out_hash, out_piece, g_list_previous(pieces), pos - 1);

  module = piece->module;

  if(dev->gui_attached) dev->progress.total++;
  
  KILL_SWITCH_ABORT;

  const uint64_t hash = dt_dev_pixelpipe_node_hash(pipe, piece, piece->roi_out, pos);

  // 1) Fast-track:
  // If we have a cache entry for this hash, return it straight away,
  // don't recurse through pipeline and don't process, unless this module still
  // needs GUI-side sampling from its host input or the gamma display histogram
  // needs the upstream cache entry.
  dt_pixel_cache_entry_t *existing_cache = NULL;
  void *existing_output = NULL;
  const gboolean exact_output_cache_hit
      = _requests_cache(pipe, piece)
        && dt_dev_pixelpipe_cache_peek(darktable.pixelpipe_cache, hash, &existing_output, &existing_cache,
                                       -1, NULL);

  if(exact_output_cache_hit)
  {
    _trace_cache_owner(pipe, module, "exact-hit-direct", "output", hash, NULL, existing_cache, FALSE);
    *out_hash = hash;
    *out_piece = piece;
    return 0;
  }

  // 3) now recurse through the pipeline.
  uint64_t input_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
  const dt_dev_pixelpipe_iop_t *previous_piece = NULL;
  if(dt_dev_pixelpipe_process_rec(pipe, dev, &input_hash, &previous_piece, g_list_previous(pieces), pos - 1))
  {
    /* Child recursion failed before this module acquired any output cache entry.
     * Dropping `hash` here underflows cached exact-hit outputs during shutdown. */
    dt_print(DT_DEBUG_DEV,
             "[picker/rec] module=%s child recursion failed input_hash=%" PRIu64 " output_hash=%" PRIu64 "\n",
             module->op, input_hash, hash);
    return 1;
  }

  KILL_SWITCH_ABORT;

  // Child recursion just published this hash. Reopen the live cache entry directly instead of going
  // through exact-hit lookup, because exact-hit intentionally rejects auto-destroy entries while
  // the parent recursion still needs to consume transient outputs in the same run.
  dt_pixel_cache_entry_t *input_entry
      = dt_dev_pixelpipe_cache_get_entry(darktable.pixelpipe_cache, input_hash);
  if(!input_entry)
  {
    dt_print(DT_DEBUG_DEV,
             "[picker/rec] module=%s input cache entry missing input_hash=%" PRIu64 " output_hash=%" PRIu64
             " prev_module=%s prev_hash=%" PRIu64 "\n",
             module->op, input_hash, hash,
             previous_piece ? previous_piece->module->op : "base",
             previous_piece ? previous_piece->global_hash
                           : dt_dev_pixelpipe_node_hash(pipe, NULL, *_get_first_roi(pipe), 0));
    return 1;
  }
  dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, TRUE, input_entry);
  input = dt_pixel_cache_entry_get_data(input_entry);
  _trace_cache_owner(pipe, module, "acquire", "input", input_hash, input, input_entry, FALSE);
  _trace_buffer_content(pipe, module, "input-acquire", input, &piece->dsc_in, &piece->roi_in);
  const size_t bufsize = (size_t)piece->dsc_out.bpp * piece->roi_out.width * piece->roi_out.height;
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
  if(dev->gui_attached
     && (pipe->mask_display != DT_DEV_PIXELPIPE_DISPLAY_NONE)
     && !(module->operation_tags() & IOP_TAG_DISTORT)
     && (piece->dsc_in.bpp == piece->dsc_out.bpp)
     && !memcmp(&piece->roi_in, &piece->roi_out, sizeof(struct dt_iop_roi_t)))
  {
    /* Mask/channel passthrough keeps the exact child buffer alive for the next
     * downstream module. This stage does not publish a new cacheline, so it
     * must forward the child hash and actual previous piece contract exactly as
     * they came from recursion. */
    dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, FALSE, input_entry);
    *out_hash = input_hash;
    *out_piece = previous_piece;
    return 0;
  }

  if(dev->gui_attached)
  {
    gchar *module_label = dt_history_item_get_name(module);
    dt_free(darktable.main_message);
    darktable.main_message = g_strdup_printf(_("Processing module `%s` for pipeline %s (%ix%i px @ %0.f%%)..."),
                                             module_label, dt_pixelpipe_get_pipe_name(pipe->type),
                                             piece->roi_out.width, piece->roi_out.height, piece->roi_out.scale * 100.f);
    dt_free(module_label);
    dt_control_queue_redraw_center();
  }

  dt_pixel_cache_entry_t *output_entry = NULL;
  gchar *type = dt_pixelpipe_get_pipe_name(pipe->type);

  dt_pixelpipe_flow_t pixelpipe_flow = (PIXELPIPE_FLOW_NONE | PIXELPIPE_FLOW_HISTOGRAM_NONE);

  gchar *name = g_strdup_printf("module %s (%s) for pipe %s", module->op, module->multi_name, type);
  gboolean cache_output = piece->force_opencl_cache;
  const gboolean allow_cache_reuse = !(darktable.unmuted & DT_DEBUG_NOCACHE_REUSE);
  /* `piece->cache_entry` is only valid as a writable-reuse hint for transient outputs that will
   * be fully overwritten later. As soon as we keep the current output as a published cacheline in
   * RAM, rekey reuse must stop for that piece so later runs cannot overwrite a long-term state in
   * place just because the pipe is running in realtime. */
  const gboolean allow_rekey_reuse = dev && _requests_cache(pipe, piece) && allow_cache_reuse
                                     && !cache_output;
  const dt_dev_pixelpipe_cache_writable_status_t acquire_status
      = dt_dev_pixelpipe_cache_get_writable(darktable.pixelpipe_cache, hash, bufsize, name, pipe->type,
                                            cache_output, allow_rekey_reuse,
                                            allow_rekey_reuse ? &piece->cache_entry : NULL,
                                            &output, &output_entry);
  dt_free(name);
  if(acquire_status == DT_DEV_PIXELPIPE_CACHE_WRITABLE_EXACT_HIT)
  {
    /* Another pipe already owns a cacheline for this exact hash. If that cacheline is
     * still write-locked, this is not a processing error: it only means the concurrent
     * publisher has not finished exposing the exact-hit payload yet. Wait for that
     * publication to complete instead of aborting the whole recursion. */
    dt_pixel_cache_entry_t *exact_entry
        = dt_dev_pixelpipe_cache_get_entry(darktable.pixelpipe_cache, hash);
    if(!exact_entry)
    {
      dt_print(DT_DEBUG_DEV,
               "[picker/rec] module=%s exact-hit entry missing output_hash=%" PRIu64 "\n",
               module->op, hash);
      dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, FALSE, input_entry);
      return 1;
    }

    dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, TRUE, exact_entry);
    dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, TRUE, exact_entry);
    dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, FALSE, exact_entry);
    dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, FALSE, exact_entry);

    _trace_cache_owner(pipe, module, "exact-hit-wait", "output", hash,
                       dt_pixel_cache_entry_get_data(exact_entry), exact_entry, FALSE);

    dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, FALSE, input_entry);
    *out_hash = hash;
    *out_piece = piece;
    return 0;
  }
  if(!output_entry)
  {
    dt_print(DT_DEBUG_DEV,
             "[picker/rec] module=%s writable output acquisition failed output_hash=%" PRIu64
             " acquire_status=%d\n",
             module->op, hash, acquire_status);
    dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, FALSE, input_entry);
    return 1;
  }
  const gboolean new_entry = (acquire_status == DT_DEV_PIXELPIPE_CACHE_WRITABLE_CREATED);
  _trace_cache_owner(pipe, module, (acquire_status == DT_DEV_PIXELPIPE_CACHE_WRITABLE_REKEYED) ? "acquire-rekeyed"
                                                         : (new_entry ? "acquire-new" : "acquire-existing"),
                     "output", hash, output, output_entry, FALSE);

  /* get tiling requirement of module */
  dt_develop_tiling_t tiling = { 0 };
  tiling.factor_cl = tiling.maxbuf_cl = -1;	// set sentinel value to detect whether callback set sizes
  module->tiling_callback(module, pipe, piece, &tiling);
  if (tiling.factor_cl < 0) tiling.factor_cl = tiling.factor; // default to CPU size if callback didn't set GPU
  if (tiling.maxbuf_cl < 0) tiling.maxbuf_cl = tiling.maxbuf;

  /* does this module involve blending? */
  if(piece->blendop_data && ((dt_develop_blend_params_t *)piece->blendop_data)->mask_mode != DEVELOP_MASK_DISABLED)
  {
    /* get specific memory requirement for blending */
    dt_develop_tiling_t tiling_blendop = { 0 };
    tiling_callback_blendop(module, pipe, piece, &tiling_blendop);

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
  error = pixelpipe_process_on_GPU(pipe, piece, previous_piece, &tiling, &pixelpipe_flow,
                                   &cache_output,
                                   input_entry, output_entry);
#else
  error = pixelpipe_process_on_CPU(pipe, piece, previous_piece, &tiling, &pixelpipe_flow,
                                   &cache_output,
                                   input_entry, output_entry);
#endif

  dt_pixelpipe_cache_set_current_module(prev_module);
  output = dt_pixel_cache_entry_get_data(output_entry);

  _print_perf_debug(pipe, pixelpipe_flow, piece, module,
                    (acquire_status != DT_DEV_PIXELPIPE_CACHE_WRITABLE_CREATED), &start);

  if(dev->gui_attached) dev->progress.completed++;

  if(error)
  {
    dt_print(DT_DEBUG_DEV,
             "[picker/rec] module=%s backend processing failed input_hash=%" PRIu64 " output_hash=%" PRIu64
             " input_cst=%d output_cst=%d roi_in=%dx%d roi_out=%dx%d\n",
             module->op, input_hash, hash, piece->dsc_in.cst, piece->dsc_out.cst,
             piece->roi_in.width, piece->roi_in.height, piece->roi_out.width, piece->roi_out.height);
    _trace_cache_owner(pipe, module, "error-cleanup", "input", input_hash, input, input_entry, FALSE);
    _trace_cache_owner(pipe, module, "error-cleanup", "output", hash, output, output_entry, FALSE);
    // Ensure we always release locks and cache references on error, otherwise cache eviction/GC will stall.
    _reset_piece_cache_entry(piece);
    dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, FALSE, output_entry);
    dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, FALSE, input_entry);
    dt_dev_pixelpipe_cache_auto_destroy_apply(darktable.pixelpipe_cache, input_entry);

    // No point in keeping garbled output
    dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, FALSE, output_entry);
    if(dt_dev_pixelpipe_cache_remove(darktable.pixelpipe_cache, TRUE, output_entry))
      dt_dev_pixelpipe_cache_flag_auto_destroy(darktable.pixelpipe_cache, output_entry);
    return 1;
  }

  // Publish the module output descriptor authored for this stage. The cache entry keeps the
  // descriptor of the pixels this module actually published, not the stale descriptor of its input.
  _trace_cache_owner(pipe, module, "publish", "output", hash, output, output_entry, FALSE);
  _trace_buffer_content(pipe, module, "publish", output, &piece->dsc_out, &piece->roi_out);

  if(piece && allow_rekey_reuse && output_entry && !cache_output)
  {
    piece->cache_entry = *output_entry;
  }
  else
    _reset_piece_cache_entry(piece);

  if(output_entry && output != NULL
     && ((pixelpipe_flow & PIXELPIPE_FLOW_PROCESSED_ON_CPU)
         || (pixelpipe_flow & PIXELPIPE_FLOW_PROCESSED_WITH_TILING)))
  {
    dt_dev_pixelpipe_gpu_flush_host_pinned_images(pipe, output, output_entry, "module output host rewrite");
    /* CPU/tiling processing rewrote the whole host buffer for this output. If the cache entry was rekeyed
     * from an older GPU stage, any cached device-only images still hanging off the same entry now point to
     * obsolete pixels. Drop them here so later mixed GPU/CPU modules cannot resurrect stale device payloads. */
    dt_dev_pixelpipe_cache_flush_entry_clmem(output_entry);
  }

  if(cl_mem_output != NULL)
    dt_dev_pixelpipe_cache_release_cl_buffer(&cl_mem_output, output_entry, output,
                                      dt_dev_pixelpipe_cache_gpu_device_buffer(pipe, output_entry));

  // Flag to throw away intermediate outputs as soon as the next module consumes them.
  // `cache_output` only means the backend had to keep a host-authoritative payload for this
  // stage (for example because the next module may need RAM or because the current stage ran
  // through an OpenCL cache path). In no-cache/bypass pipelines, that does not make the
  // published cacheline long-lived: once the downstream module takes its input ref, this
  // stage is transient and must disappear on release. Only the final published output needs
  // to survive long enough for dt_dev_pixelpipe_process() to promote it to the backbuffer,
  // otherwise thumbnail/export callers only see a missing exact-hit and fall back to invalid
  // placeholder pixels.
  const gboolean keep_final_output = (hash == dt_dev_pixelpipe_get_hash(pipe));
  if(_bypass_cache(pipe, piece) && !keep_final_output)
    dt_dev_pixelpipe_cache_flag_auto_destroy(darktable.pixelpipe_cache, output_entry);

  if(dev->gui_attached)
  {
    dt_free(darktable.main_message);
    dt_control_queue_redraw_center();
  }

  // From here on we only publish/inspect the finished output. Keep the writable lock strictly
  // around cacheline allocation and backend processing, then release it at one visible point
  // before the generic tail cleanup shared by darkroom and headless paths.
  dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, FALSE, output_entry);
  
  KILL_SWITCH_AND_FLUSH_CACHE;

  // Decrease reference count on input and flush it if it was flagged for auto destroy previously
  _trace_cache_owner(pipe, module, "release", "input", input_hash, input, input_entry, FALSE);
  dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, FALSE, input_entry);
  dt_dev_pixelpipe_cache_auto_destroy_apply(darktable.pixelpipe_cache, input_entry);

  // Print min/max/Nan in debug mode only
  if((darktable.unmuted & DT_DEBUG_NAN) && strcmp(module->op, "gamma") != 0 && output)
  {
    dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, TRUE, output_entry);
    _print_nan_debug(pipe, cl_mem_output, output, &piece->roi_out, &piece->dsc_out, module);
    dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, FALSE, output_entry);
  }

  KILL_SWITCH_AND_FLUSH_CACHE;

  /* `dt_dev_pixelpipe_cache_get_writable()` increments the output cacheline refcount so backend processing
   * and publication own one explicit reference while the entry is write-locked. Once the module published
   * its result, downstream recursion reopens the same cacheline by hash and acquires its own input ref.
   * Release the writable-owner ref here, in the same visible tail path where we already release the input,
   * otherwise every successful publication leaves one permanent "used" entry behind and the LRU can no longer
   * evict anything once the cache fills up. */
  _trace_cache_owner(pipe, module, "release", "output", hash, output, output_entry, FALSE);
  dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, FALSE, output_entry);

  *out_hash = hash;
  *out_piece = piece;
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
  const uint64_t requested_hash = dt_dev_pixelpipe_get_hash(pipe);
  const uint64_t entry_hash = entry->hash;

  _trace_cache_owner(pipe, NULL, "backbuf-update", "backbuf", requested_hash,
                     entry ? entry->data : NULL, entry, FALSE);

  if(requested_hash == DT_PIXELPIPE_CACHE_HASH_INVALID 
     || entry_hash == DT_PIXELPIPE_CACHE_HASH_INVALID
     || entry_hash != requested_hash)
  {
    dt_dev_pixelpipe_cache_unref_hash(darktable.pixelpipe_cache, dt_dev_backbuf_get_hash(&pipe->backbuf));
    dt_dev_set_backbuf(&pipe->backbuf, 0, 0, 0, DT_PIXELPIPE_CACHE_HASH_INVALID,
                       dt_dev_pixelpipe_get_history_hash(pipe));
    return;
  }

  // Keep exactly one cache reference to the last valid output ("backbuf") for display.
  // This prevents the cache entry from being evicted while still in use by the GUI,
  // without leaking references on repeated cache hits.
  const gboolean hash_changed = (dt_dev_backbuf_get_hash(&pipe->backbuf) != entry_hash);
  if(hash_changed)
  {
    dt_dev_pixelpipe_cache_unref_hash(darktable.pixelpipe_cache, dt_dev_backbuf_get_hash(&pipe->backbuf));
    dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, TRUE, entry);
  }

  int bpp = 0;
  if(roi.width > 0 && roi.height > 0)
    bpp = (int)(dt_pixel_cache_entry_get_size(entry) / ((size_t)roi.width * (size_t)roi.height));

  // Always refresh backbuf geometry/state, even when the cache key is unchanged.
  // Realtime drawing can update pixels in-place in the same cacheline, so width/height/history
  // must stay synchronized independently from key changes.
  dt_dev_set_backbuf(&pipe->backbuf, roi.width, roi.height, bpp, entry_hash,
                     dt_dev_pixelpipe_get_history_hash(pipe));
}

int dt_dev_pixelpipe_process(dt_dev_pixelpipe_t *pipe, dt_develop_t *dev, dt_iop_roi_t roi)
{
  if(darktable.unmuted & DT_DEBUG_MEMORY)
  {
    fprintf(stderr, "[memory] before pixelpipe process\n");
    dt_print_mem_usage();
  }

  dt_dev_pixelpipe_cache_print(darktable.pixelpipe_cache);

  if(dev->gui_attached)
  {
    dev->color_picker.pending_module = NULL;
    dev->color_picker.pending_pipe = NULL;
    dev->color_picker.piece_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
  }

  // Get the roi_out hash of all nodes.
  // Get the previous output size of the module, for cache invalidation.
  dt_dev_pixelpipe_get_roi_in(pipe, dev, roi);
  dt_pixelpipe_get_global_hash(pipe, dev);
  const guint pos = g_list_length(dev->iop);

  void *buf = NULL;

  // If the final output cacheline already matches the current pipeline state,
  // we can skip compute entirely. Darkroom pipes may still need the post-compute
  // sampling pass for histograms and picker reads.
  dt_pixel_cache_entry_t *entry = NULL;
  if(_requests_cache(pipe, NULL)
     && dt_dev_pixelpipe_cache_peek(darktable.pixelpipe_cache, dt_dev_pixelpipe_get_hash(pipe), &buf, &entry,
                                    -1, NULL)
     && buf != NULL)
  {
    _update_backbuf_cache_reference(pipe, roi, entry);
    return 0;
  }

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

    /* Mask preview is authored while the current run advances through blend.c.
     * Reset it for each retry so a stale state from a previous pass cannot leak
     * into the next recursion before the active module reaches its own blend. */
    pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;

#ifdef HAVE_OPENCL
    dt_opencl_check_tuning(pipe->devid);
#endif

    KILL_SWITCH_PIPE

    dt_times_t start;
    dt_get_times(&start);
    uint64_t final_hash = -1;
    const dt_dev_pixelpipe_iop_t *final_piece = NULL;
    err = dt_dev_pixelpipe_process_rec(pipe, dev, &final_hash, &final_piece, pieces, pos);
    (void)final_piece;
    gchar *msg = g_strdup_printf("[pixelpipe] %s internal pixel pipeline processing", dt_pixelpipe_get_pipe_name(pipe->type));
    dt_show_times(&start, msg);
    dt_free(msg);

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
      void *final_buf = NULL;
      if(dt_dev_pixelpipe_cache_peek(darktable.pixelpipe_cache, dt_dev_pixelpipe_get_hash(pipe), &final_buf,
                                     &final_entry, pipe->devid, NULL)
         && final_buf != NULL)
        _update_backbuf_cache_reference(pipe, roi, final_entry);
      else
        dt_print(DT_DEBUG_DEV,
                 "[picker/rec] final output cache missing pipe=%s hash=%" PRIu64 " history=%" PRIu64
                 " devid=%d err=%d\n",
                 dt_pixelpipe_get_pipe_name(pipe->type), dt_dev_pixelpipe_get_hash(pipe),
                 dt_dev_pixelpipe_get_history_hash(pipe), pipe->devid, err);

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

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
