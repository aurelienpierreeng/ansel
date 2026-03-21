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
 * @brief GUI-side histogram sampling from preview cache.
 *
 * @details
 * Histogram sampling no longer runs inside the pixel processing recursion.
 * Once the preview pipe finished and published its cachelines, the GUI thread
 * resolves the immutable piece contracts currently present in
 * `dev->preview_pipe->nodes`, reopens the corresponding cachelines by
 * `piece->global_hash`, and samples histograms directly from there.
 *
 * This keeps all histogram logic in one place and makes it follow the same
 * cache lookup rules as the direct color-picker sampler:
 * - find the live piece in the current pipe graph,
 * - reopen the cacheline by hash,
 * - lock it while reading,
 * - write the result into GUI-owned module/backbuffer state.
 */

#include "develop/pixelpipe_gui.h"

#include "common/darktable.h"
#include "common/histogram.h"
#include "common/iop_profile.h"
#include "control/signal.h"
#include "develop/dev_pixelpipe.h"
#include "develop/pixelpipe_cache.h"

#include <string.h>

static void _refresh_module_histogram(const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece,
                                      const float *pixel, dt_iop_module_t *module)
{
  dt_dev_histogram_collection_params_t histogram_params = piece->histogram_params;
  dt_histogram_roi_t histogram_roi;

  if(histogram_params.roi == NULL)
  {
    histogram_roi = (dt_histogram_roi_t){
      .width = piece->roi_in.width, .height = piece->roi_in.height,
      .crop_x = 0, .crop_y = 0, .crop_width = 0, .crop_height = 0
    };
    histogram_params.roi = &histogram_roi;
  }

  dt_iop_gui_enter_critical_section(module);
  dt_histogram_helper(&histogram_params, &module->histogram_stats, piece->dsc_in.cst, module->histogram_cst,
                      pixel, &module->histogram, module->histogram_middle_grey,
                      dt_ioppr_get_pipe_work_profile_info(pipe));
  dt_histogram_max_helper(&module->histogram_stats, piece->dsc_in.cst, module->histogram_cst,
                          &module->histogram, module->histogram_max);
  dt_iop_gui_leave_critical_section(module);

  if(module->widget) dt_control_queue_redraw_widget(module->widget);
}

dt_backbuf_t *dt_dev_get_histogram_backbuf(dt_develop_t *dev, const char *op)
{
  if(!dev || !op) return NULL;

  if(!strcmp(op, "demosaic"))
    return &dev->raw_histogram;
  else if(!strcmp(op, "colorout"))
    return &dev->output_histogram;
  else if(!strcmp(op, "gamma"))
    return &dev->display_histogram;
  else
    return NULL;
}

static void _clear_histogram_backbuf(dt_backbuf_t *backbuf)
{
  if(!backbuf) return;

  dt_dev_pixelpipe_cache_unref_hash(darktable.pixelpipe_cache, dt_dev_backbuf_get_hash(backbuf));
  dt_dev_set_backbuf(backbuf, 0, 0, 0, DT_PIXELPIPE_CACHE_HASH_INVALID, DT_PIXELPIPE_CACHE_HASH_INVALID);
}

static void _refresh_global_histogram_backbuf(dt_develop_t *dev, const char *op)
{
  dt_backbuf_t *const backbuf = dt_dev_get_histogram_backbuf(dev, op);
  if(!backbuf || !dev || !dev->preview_pipe) return;

  const dt_dev_pixelpipe_iop_t *const piece = dt_dev_pixelpipe_get_module_piece(dev->preview_pipe,
                                                                                dt_iop_get_module_by_op_priority(dev->iop, op, 0));
  if(!piece)
  {
    _clear_histogram_backbuf(backbuf);
    return;
  }

  const dt_dev_pixelpipe_iop_t *const previous_piece
      = dt_dev_pixelpipe_get_prev_enabled_piece(dev->preview_pipe, piece);

  const dt_iop_roi_t *roi = &piece->roi_out;
  const dt_iop_buffer_dsc_t *dsc = &piece->dsc_out;
  uint64_t hash = piece->global_hash;

  if(!strcmp(op, "gamma"))
  {
    if(!previous_piece)
    {
      _clear_histogram_backbuf(backbuf);
      return;
    }

    roi = &previous_piece->roi_out;
    dsc = &previous_piece->dsc_out;
    hash = previous_piece->global_hash;
  }

  dt_pixel_cache_entry_t *const entry = dt_dev_pixelpipe_cache_get_entry(darktable.pixelpipe_cache, hash);
  if(!entry || hash == DT_PIXELPIPE_CACHE_HASH_INVALID)
  {
    _clear_histogram_backbuf(backbuf);
    return;
  }

  const uint64_t previous_hash = dt_dev_backbuf_get_hash(backbuf);
  if(previous_hash != hash)
  {
    dt_dev_pixelpipe_cache_unref_hash(darktable.pixelpipe_cache, previous_hash);
    dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, TRUE, entry);
  }

  dt_dev_set_backbuf(backbuf, roi->width, roi->height, dsc->bpp, hash, DT_PIXELPIPE_CACHE_HASH_INVALID);
}

gboolean dt_dev_refresh_module_histogram(dt_develop_t *dev, dt_iop_module_t *module)
{
  if(!dev || !dev->preview_pipe || !module) return FALSE;
  if(dev->preview_pipe->status != DT_DEV_PIXELPIPE_VALID) return FALSE;

  const dt_dev_pixelpipe_iop_t *const piece = dt_dev_pixelpipe_get_module_piece(dev->preview_pipe, module);
  if(!piece || !(piece->request_histogram & DT_REQUEST_ON)) return FALSE;
  if((piece->request_histogram & DT_REQUEST_ONLY_IN_GUI) && !dev->gui_attached) return FALSE;

  const dt_dev_pixelpipe_iop_t *const previous_piece
      = dt_dev_pixelpipe_get_prev_enabled_piece(dev->preview_pipe, piece);
  if(!previous_piece || previous_piece->global_hash == DT_PIXELPIPE_CACHE_HASH_INVALID) return FALSE;
  if(previous_piece->dsc_out.datatype != TYPE_FLOAT) return FALSE;

  void *input = NULL;
  dt_pixel_cache_entry_t *input_entry = NULL;
  if(!dt_dev_pixelpipe_cache_peek(darktable.pixelpipe_cache, previous_piece->global_hash,
                                  &input, &input_entry, -1, NULL)
     || !input_entry || !input)
    return FALSE;

  dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, TRUE, input_entry);
  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, TRUE, input_entry);

  const float *histogram_input = input;
  float *transformed_input = NULL;
  dt_iop_buffer_dsc_t input_dsc = previous_piece->dsc_out;

  if(input_dsc.cst != piece->dsc_in.cst)
  {
    const size_t pixels = (size_t)piece->roi_in.width * (size_t)piece->roi_in.height;
    const size_t bytes = pixels * (size_t)piece->dsc_in.channels * sizeof(float);
    transformed_input = dt_alloc_align(bytes);

    if(!transformed_input)
    {
      dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, FALSE, input_entry);
      dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, FALSE, input_entry);
      return FALSE;
    }

    memcpy(transformed_input, input, bytes);
    dt_ioppr_transform_image_colorspace(module, transformed_input, transformed_input,
                                        piece->roi_in.width, piece->roi_in.height,
                                        input_dsc.cst, piece->dsc_in.cst, &input_dsc.cst,
                                        dt_ioppr_get_pipe_work_profile_info(dev->preview_pipe));
    histogram_input = transformed_input;
  }

  _refresh_module_histogram(dev->preview_pipe, piece, histogram_input, module);

  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, FALSE, input_entry);
  dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, FALSE, input_entry);
  dt_free_align(transformed_input);

  return TRUE;
}

gboolean dt_dev_module_requires_global_histogram_output_cache(const dt_dev_pixelpipe_t *pipe,
                                                              const dt_iop_module_t *module)
{
  if(!pipe || !module) return FALSE;
  if(pipe->type != DT_DEV_PIXELPIPE_PREVIEW) return FALSE;
  if(dt_dev_pixelpipe_get_realtime(pipe)) return FALSE;
  if(!pipe->gui_observable_source) return FALSE;

  return !strcmp(module->op, "demosaic") || !strcmp(module->op, "colorout");
}

gboolean dt_dev_module_requires_global_histogram_input_cache(const dt_dev_pixelpipe_t *pipe,
                                                             const dt_iop_module_t *module)
{
  if(!pipe || !module) return FALSE;
  if(pipe->type != DT_DEV_PIXELPIPE_PREVIEW) return FALSE;
  if(dt_dev_pixelpipe_get_realtime(pipe)) return FALSE;
  if(!pipe->gui_observable_source) return FALSE;

  return !strcmp(module->op, "gamma");
}

void dt_dev_refresh_preview_histograms(dt_develop_t *dev)
{
  if(!dev || !dev->gui_attached || !dev->preview_pipe) return;
  if(dev->preview_pipe->status != DT_DEV_PIXELPIPE_VALID) return;
  if(!dev->preview_pipe->gui_observable_source) return;

  _refresh_global_histogram_backbuf(dev, "demosaic");
  _refresh_global_histogram_backbuf(dev, "colorout");
  _refresh_global_histogram_backbuf(dev, "gamma");

  for(GList *node = g_list_first(dev->preview_pipe->nodes); node; node = g_list_next(node))
  {
    dt_dev_pixelpipe_iop_t *const piece = node->data;
    if(piece && piece->enabled)
      dt_dev_refresh_module_histogram(dev, piece->module);
  }
}

static void _preview_pipe_finished_callback(gpointer instance, gpointer user_data)
{
  (void)instance;
  (void)user_data;
  dt_dev_refresh_preview_histograms(darktable.develop);
}

void dt_dev_pixelpipe_gui_init(void)
{
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED,
                                  G_CALLBACK(_preview_pipe_finished_callback), NULL);
}

void dt_dev_pixelpipe_gui_cleanup(void)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_preview_pipe_finished_callback), NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
