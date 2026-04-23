/*
    This file is part of darktable,
    Copyright (C) 2011 Henrik Andersson.
    Copyright (C) 2011-2012, 2016 johannes hanika.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012-2014, 2016-2018 Tobias Ellinghaus.
    Copyright (C) 2013 José Carlos García Sogo.
    Copyright (C) 2013-2016 Roman Lebedev.
    Copyright (C) 2014 parafin.
    Copyright (C) 2016 Alexander V. Smal.
    Copyright (C) 2016 Asma.
    Copyright (C) 2017-2018, 2020-2021 Dan Torop.
    Copyright (C) 2018 Maurizio Paglia.
    Copyright (C) 2018 rawfiner.
    Copyright (C) 2019-2020 Aldric Renaudin.
    Copyright (C) 2019, 2021-2026 Aurélien PIERRE.
    Copyright (C) 2019 Edgardo Hoszowski.
    Copyright (C) 2019-2021 Pascal Obry.
    Copyright (C) 2019 Ulrich Pegelow.
    Copyright (C) 2020 Bill Ferguson.
    Copyright (C) 2020-2021 Chris Elston.
    Copyright (C) 2020 GrahamByrnes.
    Copyright (C) 2020 Hubert Kowalski.
    Copyright (C) 2020 Martin Straeten.
    Copyright (C) 2020-2021 Philippe Weyland.
    Copyright (C) 2020-2021 Ralf Brown.
    Copyright (C) 2021-2022 Diederik Ter Rahe.
    Copyright (C) 2021 luzpaz.
    Copyright (C) 2021 Marco Carrarini.
    Copyright (C) 2021 Mark-64.
    Copyright (C) 2021 Paolo DePetrillo.
    Copyright (C) 2021 paolodepetrillo.
    Copyright (C) 2021 Sakari Kapanen.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Philipp Lutz.
    Copyright (C) 2022 Victor Forsiuk.
    Copyright (C) 2024 Alynx Zhou.
    Copyright (C) 2024 Marrony Neris.
    
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

#include <stdint.h>

#include "bauhaus/bauhaus.h"
#include "common/atomic.h"
#include "common/color_picker.h"
#include "common/color_vocabulary.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/histogram.h"
#include "common/iop_profile.h"
#include "common/imagebuf.h"
#include "common/image_cache.h"
#include "common/math.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/signal.h"
#include "develop/dev_pixelpipe.h"
#include "develop/develop.h"
#include "develop/pixelpipe_cache.h"
#include "dtgtk/drawingarea.h"
#include "dtgtk/button.h"
#include "gui/color_picker_proxy.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "libs/colorpicker.h"

#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#define HISTOGRAM_BINS 256
#define TONES 128
#define GAMMA 1.f / 1.5f

DT_MODULE(1)

typedef enum dt_lib_histogram_scope_type_t
{
  DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM = 0,
  DT_LIB_HISTOGRAM_SCOPE_WAVEFORM_HORIZONTAL,
  DT_LIB_HISTOGRAM_SCOPE_WAVEFORM_VERTICAL,
  DT_LIB_HISTOGRAM_SCOPE_PARADE_HORIZONTAL,
  DT_LIB_HISTOGRAM_SCOPE_PARADE_VERTICAL,
  DT_LIB_HISTOGRAM_SCOPE_VECTORSCOPE,
  DT_LIB_HISTOGRAM_SCOPE_N // needs to be the last one
} dt_lib_histogram_scope_type_t;


typedef struct dt_lib_histogram_cache_t
{
  // If any of those params changes, we need to recompute the Cairo buffer.
  float zoom;
  int width;
  int height;
  uint64_t hash;
  dt_lib_histogram_scope_type_t view;
} dt_lib_histogram_cache_t;

typedef enum dt_lib_colorpicker_model_t
{
  DT_LIB_COLORPICKER_MODEL_RGB = 0,
  DT_LIB_COLORPICKER_MODEL_LAB,
  DT_LIB_COLORPICKER_MODEL_LCH,
  DT_LIB_COLORPICKER_MODEL_HSL,
  DT_LIB_COLORPICKER_MODEL_HSV,
  DT_LIB_COLORPICKER_MODEL_NONE,
} dt_lib_colorpicker_model_t;

const gchar *dt_lib_colorpicker_model_names[]
  = { N_("RGB"), N_("Lab"), N_("LCh"), N_("HSL"), N_("HSV"), N_("none"), NULL };
const gchar *dt_lib_colorpicker_statistic_names[]
  = { N_("mean"), N_("min"), N_("max"), NULL };


typedef struct dt_lib_histogram_t
{
  GtkWidget *scope_draw;               // GtkDrawingArea -- scope, scale, and draggable overlays
  GtkWidget *stage;                    // Module at which stage we sample histogram
  GtkWidget *display;                  // Kind of display
  dt_backbuf_t *backbuf;               // reference to the dev backbuf currently in use
  const char *op;
  float zoom; // zoom level for the vectorscope

  dt_lib_histogram_cache_t cache;
  cairo_surface_t *cst;

  dt_lib_colorpicker_model_t model;
  dt_lib_colorpicker_statistic_t statistic;
  GtkWidget *color_mode_selector;
  GtkWidget *statistic_selector;
  GtkWidget *picker_button;
  GtkWidget *samples_container;
  GtkWidget *add_sample_button;
  GtkWidget *display_samples_check_box;
  GArray *pending_hashes;
  guint refresh_idle_source;

} dt_lib_histogram_t;

typedef struct dt_histogram_pending_module_refresh_t
{
  dt_iop_module_t *module;
  uint64_t hash;
} dt_histogram_pending_module_refresh_t;

typedef struct dt_histogram_preview_refresh_state_t
{
  uint64_t initialscale_hash;
  uint64_t colorout_hash;
  uint64_t gamma_hash;
  GList *module_histograms;
} dt_histogram_preview_refresh_state_t;

static dt_histogram_preview_refresh_state_t _preview_refresh_state
    = { DT_PIXELPIPE_CACHE_HASH_INVALID, DT_PIXELPIPE_CACHE_HASH_INVALID,
        DT_PIXELPIPE_CACHE_HASH_INVALID, NULL };

static void _clear_pending_preview_histograms(void)
{
  g_list_free_full(_preview_refresh_state.module_histograms, g_free);
  _preview_refresh_state.module_histograms = NULL;
  _preview_refresh_state.initialscale_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
  _preview_refresh_state.colorout_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
  _preview_refresh_state.gamma_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
}

static void _refresh_module_histogram(const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece,
                                      const float *pixel, dt_iop_module_t *module)
{
  dt_dev_histogram_collection_params_t histogram_params = piece->histogram_params;
  dt_histogram_roi_t histogram_roi;

  if(IS_NULL_PTR(histogram_params.roi))
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

static dt_backbuf_t *_get_histogram_backbuf(dt_develop_t *dev, const char *op)
{
  if(IS_NULL_PTR(dev) || IS_NULL_PTR(op)) return NULL;

  if(!strcmp(op, "initialscale"))
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
  if(IS_NULL_PTR(backbuf)) return;

  /* Global histogram backbuffers keep one structural ref on top of the module-output lifetime.
   * Clearing that published view therefore means releasing the extra GUI-side keepalive ref here. */
  dt_dev_pixelpipe_cache_unref_hash(darktable.pixelpipe_cache, dt_dev_backbuf_get_hash(backbuf));
  dt_dev_set_backbuf(backbuf, 0, 0, 0, DT_PIXELPIPE_CACHE_HASH_INVALID, DT_PIXELPIPE_CACHE_HASH_INVALID);
}

static gboolean _refresh_global_histogram_backbuf_for_hash(dt_develop_t *dev, const char *op,
                                                           const uint64_t expected_hash)
{
  dt_backbuf_t *const backbuf = _get_histogram_backbuf(dev, op);
  if(IS_NULL_PTR(backbuf) || IS_NULL_PTR(dev) || IS_NULL_PTR(dev->preview_pipe)) return FALSE;

  const dt_dev_pixelpipe_iop_t *const piece = dt_dev_pixelpipe_get_module_piece(
      dev->preview_pipe, dt_iop_get_module_by_op_priority(dev->iop, op, 0));
  if(IS_NULL_PTR(piece))
  {
    _clear_histogram_backbuf(backbuf);
    return FALSE;
  }

  const dt_dev_pixelpipe_iop_t *const previous_piece
      = dt_dev_pixelpipe_get_prev_enabled_piece(dev->preview_pipe, piece);

  const dt_iop_roi_t *roi = &piece->roi_out;
  const dt_iop_buffer_dsc_t *dsc = &piece->dsc_out;
  uint64_t hash = piece->global_hash;

  if(!strcmp(op, "gamma"))
  {
    if(IS_NULL_PTR(previous_piece))
    {
      _clear_histogram_backbuf(backbuf);
      return FALSE;
    }

    roi = &previous_piece->roi_out;
    dsc = &previous_piece->dsc_out;
    hash = previous_piece->global_hash;
  }

  if(expected_hash != DT_PIXELPIPE_CACHE_HASH_INVALID && hash != expected_hash) return FALSE;

  dt_pixel_cache_entry_t *entry = NULL;
  if(hash == DT_PIXELPIPE_CACHE_HASH_INVALID
     || !dt_dev_pixelpipe_cache_peek_gui(dev->preview_pipe, !strcmp(op, "gamma") ? previous_piece : piece,
                                         NULL, &entry, NULL, NULL, NULL))
  {
    _clear_histogram_backbuf(backbuf);
    return FALSE;
  }

  const uint64_t previous_hash = dt_dev_backbuf_get_hash(backbuf);
  if(previous_hash != hash)
  {
    /* The module output already owns its producer ref. Tagging it as a global histogram backbuffer
     * reserves one additional consumer ref so GUI readers only need `peek()` and read locks later. */
    dt_dev_pixelpipe_cache_unref_hash(darktable.pixelpipe_cache, previous_hash);
    dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, TRUE, entry);
  }

  dt_dev_set_backbuf(backbuf, roi->width, roi->height, dsc->bpp, hash, DT_PIXELPIPE_CACHE_HASH_INVALID);
  return TRUE;
}

static gboolean _refresh_preview_module_histogram_for_hash(dt_develop_t *dev, dt_iop_module_t *module,
                                                           const uint64_t expected_hash)
{
  if(IS_NULL_PTR(dev) || IS_NULL_PTR(dev->preview_pipe) || !module) return FALSE;

  const dt_dev_pixelpipe_iop_t *const piece = dt_dev_pixelpipe_get_module_piece(dev->preview_pipe, module);
  if(IS_NULL_PTR(piece) || !(piece->request_histogram & DT_REQUEST_ON)) return FALSE;
  if((piece->request_histogram & DT_REQUEST_ONLY_IN_GUI) && !dev->gui_attached) return FALSE;

  const dt_dev_pixelpipe_iop_t *const previous_piece
      = dt_dev_pixelpipe_get_prev_enabled_piece(dev->preview_pipe, piece);
  if(IS_NULL_PTR(previous_piece) || previous_piece->global_hash == DT_PIXELPIPE_CACHE_HASH_INVALID) return FALSE;
  if(expected_hash != DT_PIXELPIPE_CACHE_HASH_INVALID && previous_piece->global_hash != expected_hash) return FALSE;
  if(previous_piece->dsc_out.datatype != TYPE_FLOAT) return FALSE;

  void *input = NULL;
  dt_pixel_cache_entry_t *input_entry = NULL;
  if(!dt_dev_pixelpipe_cache_peek_gui(dev->preview_pipe, previous_piece, &input, &input_entry, NULL, NULL, NULL))
    return FALSE;

  dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, TRUE, input_entry);
  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, TRUE, input_entry);

  const float *histogram_input = input;
  float *transformed_input = NULL;
  dt_iop_buffer_dsc_t input_dsc = previous_piece->dsc_out;

  if(input_dsc.cst != piece->dsc_in.cst
     && !(dt_iop_colorspace_is_rgb(input_dsc.cst) && dt_iop_colorspace_is_rgb(piece->dsc_in.cst)))
  {
    const size_t pixels = (size_t)piece->roi_in.width * (size_t)piece->roi_in.height;
    const size_t bytes = pixels * (size_t)piece->dsc_in.channels * sizeof(float);
    transformed_input = dt_alloc_align(bytes);

    if(IS_NULL_PTR(transformed_input))
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
  else if(input_dsc.cst != piece->dsc_in.cst)
  {
    input_dsc.cst = piece->dsc_in.cst;
  }

  _refresh_module_histogram(dev->preview_pipe, piece, histogram_input, module);

  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, FALSE, input_entry);
  dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, FALSE, input_entry);
  dt_free_align(transformed_input);

  return TRUE;
}

static void _refresh_preview_histograms(dt_develop_t *dev)
{
  if(IS_NULL_PTR(dev) || !dev->gui_attached || IS_NULL_PTR(dev->preview_pipe)) return;
  if(!dev->preview_pipe->gui_observable_source) return;

  _clear_pending_preview_histograms();

  const char *ops[] = { "initialscale", "colorout", "gamma" };
  uint64_t *pending_hashes[] = {
    &_preview_refresh_state.initialscale_hash,
    &_preview_refresh_state.colorout_hash,
    &_preview_refresh_state.gamma_hash
  };

  for(size_t i = 0; i < G_N_ELEMENTS(ops); i++)
  {
    const dt_dev_pixelpipe_iop_t *piece = dt_dev_pixelpipe_get_module_piece(
        dev->preview_pipe, dt_iop_get_module_by_op_priority(dev->iop, ops[i], 0));
    if(IS_NULL_PTR(piece))
    {
      _clear_histogram_backbuf(_get_histogram_backbuf(dev, ops[i]));
      continue;
    }

    uint64_t hash = piece->global_hash;
    if(!strcmp(ops[i], "gamma"))
    {
      const dt_dev_pixelpipe_iop_t *const previous_piece
          = dt_dev_pixelpipe_get_prev_enabled_piece(dev->preview_pipe, piece);
      hash = previous_piece ? previous_piece->global_hash : DT_PIXELPIPE_CACHE_HASH_INVALID;
    }

    if(hash == DT_PIXELPIPE_CACHE_HASH_INVALID
       || !_refresh_global_histogram_backbuf_for_hash(dev, ops[i], hash))
      *pending_hashes[i] = hash;
  }

  for(GList *node = g_list_first(dev->preview_pipe->nodes); node; node = g_list_next(node))
  {
    dt_dev_pixelpipe_iop_t *piece = node->data;
    if(IS_NULL_PTR(piece) || !piece->enabled) continue;
    if(!(piece->request_histogram & DT_REQUEST_ON)) continue;
    if((piece->request_histogram & DT_REQUEST_ONLY_IN_GUI) && !dev->gui_attached) continue;

    const dt_dev_pixelpipe_iop_t *const previous_piece
        = dt_dev_pixelpipe_get_prev_enabled_piece(dev->preview_pipe, piece);
    if(previous_piece && !_refresh_preview_module_histogram_for_hash(dev, piece->module, previous_piece->global_hash))
    {
      dt_histogram_pending_module_refresh_t *pending = g_malloc0(sizeof(*pending));
      pending->module = piece->module;
      pending->hash = previous_piece->global_hash;
      _preview_refresh_state.module_histograms
          = g_list_prepend(_preview_refresh_state.module_histograms, pending);
    }
  }
}

static void _preview_history_resync_callback(gpointer instance, gpointer user_data)
{
  (void)instance;
  (void)user_data;
  _refresh_preview_histograms(darktable.develop);
}

static void _preview_cacheline_ready_callback(gpointer instance, const guint64 hash, gpointer user_data)
{
  (void)instance;
  (void)user_data;

  dt_develop_t *const dev = darktable.develop;
  if(IS_NULL_PTR(dev) || !dev->gui_attached || IS_NULL_PTR(dev->preview_pipe)) return;

  if(_preview_refresh_state.initialscale_hash == hash)
  {
    _refresh_global_histogram_backbuf_for_hash(dev, "initialscale", hash);
    _preview_refresh_state.initialscale_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
  }

  if(_preview_refresh_state.colorout_hash == hash)
  {
    _refresh_global_histogram_backbuf_for_hash(dev, "colorout", hash);
    _preview_refresh_state.colorout_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
  }

  if(_preview_refresh_state.gamma_hash == hash)
  {
    _refresh_global_histogram_backbuf_for_hash(dev, "gamma", hash);
    _preview_refresh_state.gamma_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
  }

  for(GList *l = _preview_refresh_state.module_histograms; l;)
  {
    GList *next = g_list_next(l);
    dt_histogram_pending_module_refresh_t *pending = l->data;
    if(pending->hash == hash)
    {
      _refresh_preview_module_histogram_for_hash(dev, pending->module, pending->hash);
      _preview_refresh_state.module_histograms = g_list_delete_link(_preview_refresh_state.module_histograms, l);
      g_free(pending);
    }
    l = next;
  }
}

const char *name(struct dt_lib_module_t *self)
{
  return _("scopes");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"darkroom", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int expandable(dt_lib_module_t *self)
{
  return 1;
}

int position()
{
  return 1000;
}

static void _update_picker_output(dt_lib_module_t *self);
static void _update_sample_label(dt_lib_module_t *self, dt_colorpicker_sample_t *sample);
static void _update_everything(dt_lib_module_t *self);


void _backbuf_int_to_op(const int value, dt_lib_histogram_t *d)
{
  switch(value)
  {
    case 0:
    {
      d->op = "initialscale";
      break;
    }
    case 1:
    {
      d->op = "colorout";
      break;
    }
    case 2:
    {
      d->op = "gamma";
    }
  }
}


int _backbuf_op_to_int(dt_lib_histogram_t *d)
{
  if(!strcmp(d->op, "initialscale") || !strcmp(d->op, "demosaic")) return 0;
  if(!strcmp(d->op, "colorout")) return 1;
  if(!strcmp(d->op, "gamma")) return 2;
  return 2;
}


#ifdef _OPENMP
#pragma omp declare simd \
  aligned(rgb_in, xyz_out:64) \
  uniform(rgb_in, xyz_out)
#endif
void _scope_pixel_to_xyz(const dt_aligned_pixel_t rgb_in, dt_aligned_pixel_t xyz_out, dt_lib_histogram_t *d)
{
  if(_backbuf_op_to_int(d) > 0)
  {
    // We are in display RGB
    const dt_iop_order_iccprofile_info_t *const profile = darktable.develop->preview_pipe->output_profile_info;
    dt_ioppr_rgb_matrix_to_xyz(rgb_in, xyz_out, profile->matrix_in_transposed, profile->lut_in, profile->unbounded_coeffs_in,
                               profile->lutsize, profile->nonlinearlut);
  }
  else
  {
    // We are in sensor RGB
    const dt_iop_order_iccprofile_info_t *const profile = darktable.develop->preview_pipe->input_profile_info;
    dt_ioppr_rgb_matrix_to_xyz(rgb_in, xyz_out, profile->matrix_in_transposed, profile->lut_in, profile->unbounded_coeffs_in,
                               profile->lutsize, profile->nonlinearlut);
  }
}


#ifdef _OPENMP
#pragma omp declare simd \
  aligned(rgb_in, rgb_out:64) \
  uniform(rgb_in, rgb_out)
#endif
void _scope_pixel_to_display_rgb(const dt_aligned_pixel_t rgb_in, dt_aligned_pixel_t rgb_out, dt_lib_histogram_t *d)
{
  if(_backbuf_op_to_int(d) > 0)
  {
    // We are in display RGB
    for_each_channel(c)
      rgb_out[c] = rgb_in[c];
  }
  else
  {
    // We are in the sensor/input RGB profile. Convert directly between the
    // cached stage profile and the display profile so the swatch patch uses
    // the same authoritative ICC path as the rest of the GUI.
    const dt_iop_order_iccprofile_info_t *const profile_in = darktable.develop->preview_pipe->input_profile_info;
    const dt_iop_order_iccprofile_info_t *const profile_out = darktable.develop->preview_pipe->output_profile_info;
    dt_ioppr_transform_image_colorspace_rgb(rgb_in, rgb_out, 1, 1, profile_in, profile_out,
                                            "[histogram] sample swatch");
  }
}

void _reset_cache(dt_lib_histogram_t *d)
{
  d->cache.view = DT_LIB_HISTOGRAM_SCOPE_N;
  d->cache.width = -1;
  d->cache.height = -1;
  d->cache.hash = (uint64_t)-1;
  d->cache.zoom = -1.;
}

static void _clear_pending_hashes(dt_lib_histogram_t *d)
{
  if(IS_NULL_PTR(d) || !d->pending_hashes) return;
  g_array_set_size(d->pending_hashes, 0);
}

static gboolean _has_pending_hash(const dt_lib_histogram_t *d, const uint64_t hash)
{
  if(IS_NULL_PTR(d) || !d->pending_hashes || hash == DT_PIXELPIPE_CACHE_HASH_INVALID) return FALSE;

  for(guint i = 0; i < d->pending_hashes->len; i++)
    if(g_array_index(d->pending_hashes, uint64_t, i) == hash) return TRUE;

  return FALSE;
}

static void _add_pending_hash(dt_lib_histogram_t *d, const uint64_t hash)
{
  if(IS_NULL_PTR(d) || !d->pending_hashes || hash == DT_PIXELPIPE_CACHE_HASH_INVALID || _has_pending_hash(d, hash)) return;
  g_array_append_val(d->pending_hashes, hash);
}

static gboolean _remove_pending_hash(dt_lib_histogram_t *d, const uint64_t hash)
{
  if(IS_NULL_PTR(d) || !d->pending_hashes || hash == DT_PIXELPIPE_CACHE_HASH_INVALID) return FALSE;

  for(guint i = 0; i < d->pending_hashes->len; i++)
  {
    if(g_array_index(d->pending_hashes, uint64_t, i) == hash)
    {
      g_array_remove_index(d->pending_hashes, i);
      return TRUE;
    }
  }

  return FALSE;
}

static uint64_t _get_live_histogram_hash(const char *op)
{
  dt_develop_t *const dev = darktable.develop;
  dt_dev_pixelpipe_t *const pipe = dev ? dev->preview_pipe : NULL;
  if(IS_NULL_PTR(dev) || IS_NULL_PTR(pipe) || IS_NULL_PTR(op)) return DT_PIXELPIPE_CACHE_HASH_INVALID;

  dt_iop_module_t *const module = dt_iop_get_module_by_op_priority(dev->iop, op, 0);
  if(IS_NULL_PTR(module)) return DT_PIXELPIPE_CACHE_HASH_INVALID;

  const dt_dev_pixelpipe_iop_t *piece = dt_dev_pixelpipe_get_module_piece(pipe, module);
  if(IS_NULL_PTR(piece)) return DT_PIXELPIPE_CACHE_HASH_INVALID;

  if(!strcmp(op, "gamma"))
    piece = dt_dev_pixelpipe_get_prev_enabled_piece(pipe, piece);

  return piece ? piece->global_hash : DT_PIXELPIPE_CACHE_HASH_INVALID;
}

static gboolean _histogram_refresh_idle(gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_histogram_t *d = self ? self->data : NULL;
  if(IS_NULL_PTR(d)) return G_SOURCE_REMOVE;

  d->refresh_idle_source = 0;
  d->backbuf = _get_histogram_backbuf(darktable.develop, d->op);
  _update_everything(self);
  return G_SOURCE_REMOVE;
}

static void _schedule_histogram_refresh(dt_lib_module_t *self)
{
  dt_lib_histogram_t *d = self ? self->data : NULL;
  if(IS_NULL_PTR(d) || d->refresh_idle_source != 0) return;
  d->refresh_idle_source = g_idle_add(_histogram_refresh_idle, self);
}

static void _sync_pending_histogram_hashes(dt_lib_module_t *self)
{
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  if(IS_NULL_PTR(d)) return;

  _clear_pending_hashes(d);
  _add_pending_hash(d, _get_live_histogram_hash(d->op));

  for(GSList *samples = darktable.develop->color_picker.samples; samples; samples = g_slist_next(samples))
  {
    dt_colorpicker_sample_t *sample = samples->data;
    if(sample->locked) continue;
    _add_pending_hash(d, _get_live_histogram_hash(sample->backbuf_op[0] ? sample->backbuf_op : d->op));
  }

  if(darktable.develop->color_picker.picker)
    _add_pending_hash(d, _get_live_histogram_hash(d->op));

  d->backbuf = _get_histogram_backbuf(darktable.develop, d->op);
  _update_everything(self);
}


static gboolean _is_backbuf_ready(dt_lib_histogram_t *d)
{
  if(IS_NULL_PTR(d) || IS_NULL_PTR(d->backbuf)) return FALSE;

  return (dt_dev_pixelpipe_is_backbufer_valid(darktable.develop->preview_pipe)) &&
         (dt_dev_backbuf_get_hash(d->backbuf) != (uint64_t)-1);
}

static void _redraw_scopes(dt_lib_histogram_t *d)
{
  gtk_widget_queue_draw(d->scope_draw);
}

uint32_t _find_max_histogram(const uint32_t *const restrict bins, const size_t binning_size)
{
  uint32_t max_hist = 0;
  __OMP_PARALLEL_FOR_SIMD__(aligned(bins: 64)  reduction(max: max_hist) )
  for(size_t k = 0; k < binning_size; k++) if(bins[k] > max_hist) max_hist = bins[k];

  return max_hist;
}


static inline void _bin_pixels_histogram_in_roi(const float *const restrict image, uint32_t *const restrict bins,
                                                const size_t min_x, const size_t max_x,
                                                const size_t min_y, const size_t max_y,
                                                const size_t width)
{
  //fprintf(stdout, "computing histogram from x = [%lu;%lu], y = [%lu;%lu]\n", min_x, max_x, min_y, max_y);
  // Process
#ifdef _OPENMP
#ifndef _WIN32
#pragma omp parallel for default(firstprivate) \
        reduction(+: bins[0: HISTOGRAM_BINS * 4])  collapse(3)
#else
#pragma omp parallel for default(firstprivate) \
        shared(bins)  collapse(3)
#endif
#endif
  for(size_t i = min_y; i < max_y; i++)
    for(size_t j = min_x; j < max_x; j++)
      for(size_t c = 0; c < 3; c++)
      {
        const float value = image[(i * width + j) * 4 + c];
        const size_t index = (size_t)CLAMP(roundf(value * (HISTOGRAM_BINS - 1)), 0, HISTOGRAM_BINS - 1);
        bins[index * 4 + c]++;
      }
}


static inline void _bin_pickers_histogram(const float *const restrict image,
                                          const size_t width, const size_t height,
                                          uint32_t *bins,
                                          dt_colorpicker_sample_t *sample)
{
  if(sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
  {
    const size_t box[4] = {
      CLAMP((size_t)roundf(sample->box[0] * width), 0, width),
      CLAMP((size_t)roundf(sample->box[1] * height), 0, height),
      CLAMP((size_t)roundf(sample->box[2] * width), 0, width),
      CLAMP((size_t)roundf(sample->box[3] * height), 0, height)
    };
    _bin_pixels_histogram_in_roi(image, bins, box[0], box[2], box[1], box[3], width);
  }
  else
  {
    const size_t x = CLAMP((size_t)roundf(sample->point[0] * width), 0, width - 1);
    const size_t y = CLAMP((size_t)roundf(sample->point[1] * height), 0, height - 1);
    _bin_pixels_histogram_in_roi(image, bins, x, x + 1, y, y + 1, width);
  }
}

static const dt_dev_pixelpipe_iop_t *_get_backbuf_source_piece(const dt_backbuf_t *backbuf, const char *op);

static void _process_histogram(dt_backbuf_t *backbuf, const char *op, cairo_t *cr, const int width,
                               const int height)
{
  // Histogram backbuffers already own their keepalive ref in the pipeline state. Drawing only borrows them.
  struct dt_pixel_cache_entry_t *entry = NULL;
  void *data = NULL;
  const dt_dev_pixelpipe_iop_t *const piece = _get_backbuf_source_piece(backbuf, op);
  if(IS_NULL_PTR(piece) || !dt_dev_pixelpipe_cache_peek_gui(darktable.develop->preview_pipe, piece, &data, &entry, NULL, NULL, NULL))
    return;

  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, TRUE, entry);

  uint32_t *bins = calloc(4 * HISTOGRAM_BINS, sizeof(uint32_t));
  if(IS_NULL_PTR(bins)) 
  {
    dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, FALSE, entry);
    return;
  }

  if(dt_conf_get_bool("ui_last/colorpicker_restrict_histogram"))
  {
    // Bin only areas within color pickers
    GSList *samples = darktable.develop->color_picker.samples;
    while(samples)
    {
      dt_colorpicker_sample_t *sample = samples->data;
      _bin_pickers_histogram(data, backbuf->width, backbuf->height,
                             bins, sample);
      samples = g_slist_next(samples);
    }

    if(darktable.develop->color_picker.picker)
      _bin_pickers_histogram(data, backbuf->width, backbuf->height,
                             bins, darktable.develop->color_picker.primary_sample);
  }
  else
  {
    _bin_pixels_histogram_in_roi(data, bins, 0, backbuf->width, 0, backbuf->height, width);
  }

  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, FALSE, entry);

  uint32_t overall_histogram_max = _find_max_histogram(bins, 4 * HISTOGRAM_BINS);

  // Draw thingy
  if(overall_histogram_max > 0)
  {
    // Paint background
    cairo_rectangle(cr, 0, 0, width, height);
    set_color(cr, darktable.bauhaus->graph_bg);
    cairo_fill(cr);

    set_color(cr, darktable.bauhaus->graph_grid);
    dt_draw_grid(cr, 4, 0, 0, width, height);

    cairo_save(cr);
    cairo_push_group_with_content(cr, CAIRO_CONTENT_COLOR);
    cairo_translate(cr, 0, height);
    cairo_scale(cr, width / 255.0, - (double)height / (double)(1. + log(overall_histogram_max)));
    cairo_set_operator(cr, CAIRO_OPERATOR_ADD);

    for(int k = 0; k < 3; k++)
    {
      set_color(cr, darktable.bauhaus->graph_colors[k]);
      dt_draw_histogram_8(cr, bins, 4, k, FALSE);
    }

    cairo_pop_group_to_source(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
    cairo_paint_with_alpha(cr, 0.5);
    cairo_restore(cr);
  }

  dt_free(bins);
}


static inline void _bin_pixels_waveform_in_roi(const float *const restrict image, uint32_t *const restrict bins,
                                               const size_t min_x, const size_t max_x,
                                               const size_t min_y, const size_t max_y,
                                               const size_t width,
                                               const size_t binning_size,
                                               const gboolean vertical)
{
  // Process
  __OMP_PARALLEL_FOR__(shared(bins)  collapse(3))
  for(size_t i = min_y; i < max_y; i++)
    for(size_t j = min_x; j < max_x; j++)
      for(size_t c = 0; c < 3; c++)
      {
        const float value = image[(i * width + j) * 4 + c];
        const size_t index = (uint8_t)CLAMP(roundf(value * (TONES - 1)), 0, TONES - 1);
        if(vertical)
          bins[((i * (TONES)) + index) * 4 + c]++;
        else
          bins[(((TONES - 1) - index) * width + j) * 4 + c]++;
      }
}

static inline void _bin_pickers_waveforms(const float *const restrict image, uint32_t *const restrict bins,
                                          const size_t width, const size_t height, const size_t binning_size,
                                          const gboolean vertical, dt_colorpicker_sample_t *sample)
{
  if(sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
  {
    const size_t box[4] = {
      CLAMP((size_t)roundf(sample->box[0] * width), 0, width),
      CLAMP((size_t)roundf(sample->box[1] * height), 0, height),
      CLAMP((size_t)roundf(sample->box[2] * width), 0, width),
      CLAMP((size_t)roundf(sample->box[3] * height), 0, height)
    };
    _bin_pixels_waveform_in_roi(image, bins, box[0], box[2], box[1], box[3], width, binning_size, vertical);
  }
  else
  {
    const size_t x = CLAMP((size_t)roundf(sample->point[0] * width), 0, width - 1);
    const size_t y = CLAMP((size_t)roundf(sample->point[1] * height), 0, height - 1);
    _bin_pixels_waveform_in_roi(image, bins, x, x + 1, y, y + 1, width, binning_size, vertical);
  }
}


static inline void _bin_pixels_waveform(const float *const restrict image, uint32_t *const restrict bins,
                                        const size_t width, const size_t height, const size_t binning_size,
                                        const gboolean vertical)
{
  // Init
  __OMP_FOR_SIMD__(aligned(bins: 64) )
  for(size_t k = 0; k < binning_size; k++) bins[k] = 0;

  if(dt_conf_get_bool("ui_last/colorpicker_restrict_histogram"))
  {
    // Bin only areas within color pickers
    GSList *samples = darktable.develop->color_picker.samples;
    while(samples)
    {
      dt_colorpicker_sample_t *sample = samples->data;
      _bin_pickers_waveforms(image, bins, width, height, binning_size, vertical, sample);
      samples = g_slist_next(samples);
    }

    if(darktable.develop->color_picker.picker)
      _bin_pickers_waveforms(image, bins, width, height, binning_size, vertical,
                             darktable.develop->color_picker.primary_sample);
  }
  else
  {
    // Bin the whole image
    _bin_pixels_waveform_in_roi(image, bins, 0, width, 0, height, width, binning_size, vertical);
  }
}

static void _create_waveform_image(const uint32_t *const restrict bins, uint8_t *const restrict image,
                                   const uint32_t max_hist,
                                   const size_t width, const size_t height)
{
  __OMP_FOR_SIMD__(aligned(image, bins: 64) )
  for(size_t k = 0; k < height * width * 4; k += 4)
  {
    image[k + 3] = 255; // alpha

    // We apply a slight "gamma" boost for legibility
    image[k + 2] = (uint8_t)CLAMP(roundf(powf((float)bins[k + 0] / (float)max_hist, GAMMA) * 255.f), 0, 255);
    image[k + 1] = (uint8_t)CLAMP(roundf(powf((float)bins[k + 1] / (float)max_hist, GAMMA) * 255.f), 0, 255);
    image[k + 0] = (uint8_t)CLAMP(roundf(powf((float)bins[k + 2] / (float)max_hist, GAMMA) * 255.f), 0, 255);
  }
}

static void _mask_waveform(const uint8_t *const restrict image, uint8_t *const restrict masked, const size_t width, const size_t height, const size_t channel)
{
  // Channel masking, aka extract the desired channel out of the RGBa image
  uint8_t mask[4] = { 0, 0, 0, 0 };
  mask[channel] = 1;

  __OMP_PARALLEL_FOR__(collapse(2))
  for(size_t i = 0; i < height; i++)
    for(size_t j = 0; j < width; j++)
    {
      const size_t index = (i * width + j) * 4;
      const uint8_t *const restrict pixel_in = image + index;
      uint8_t *const restrict pixel_out = masked + index;

      __OMP_SIMD__(aligned(mask, pixel_in, pixel_out: 16))
      for(size_t c = 0; c < 4; c++) pixel_out[c] = pixel_in[c] * mask[c];
    }
}

static void _paint_waveform(cairo_t *cr, uint8_t *const restrict image, const int width, const int height, const size_t img_width, const size_t img_height, const gboolean vertical)
{
  const size_t stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, img_width);
  cairo_surface_t *background = cairo_image_surface_create_for_data(image, CAIRO_FORMAT_ARGB32, img_width, img_height, stride);
  const double scale_w = (vertical) ? (double)width / (double)TONES
                                    : (double)width / (double)img_width;
  const double scale_h = (vertical) ? (double)height / (double)img_height
                                    : (double)height / (double)TONES;
  cairo_scale(cr, scale_w, scale_h);
  cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  cairo_set_source_surface(cr, background, 0., 0.);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
  cairo_paint(cr);
  cairo_surface_destroy(background);
}

static void _paint_parade(cairo_t *cr, uint8_t *const restrict image, const int width, const int height, const size_t img_width, const size_t img_height, const gboolean vertical)
{
  const size_t stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, img_width);
  const double scale_w = (vertical) ? (double)width / (double)TONES
                                    : (double)width / (double)img_width / 3.;
  const double scale_h = (vertical) ? (double)height / (double)img_height / 3.
                                    : (double)height / (double)TONES;
  cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
  cairo_scale(cr, scale_w, scale_h);

  // The parade is basically a waveform where channels are shown
  // next to each other instead of on top of each other.
  // We need to isolate each channel, then paint it at a third of the nominal image width/height.
  for(int c = 0; c < 3; c++)
  {
    uint8_t *const restrict channel = dt_pixelpipe_cache_alloc_align_cache(
        img_width * img_height * 4 * sizeof(uint8_t),
        0);
    if(channel)
    {
      _mask_waveform(image, channel, img_width, img_height, c);
      cairo_surface_t *background = cairo_image_surface_create_for_data(channel, CAIRO_FORMAT_ARGB32, img_width, img_height, stride);
      const double x = (vertical) ? 0. : (double)c * img_width;
      const double y = (vertical) ? (double)c * img_height : 0.;
      cairo_set_source_surface(cr, background, x, y);
      cairo_paint(cr);
      cairo_surface_destroy(background);
      dt_pixelpipe_cache_free_align(channel);
    }
  }
}


static void _process_waveform(dt_backbuf_t *backbuf, const char *op, cairo_t *cr, const int width,
                              const int height, const gboolean vertical, const gboolean parade)
{
  struct dt_pixel_cache_entry_t *entry = NULL;
  void *data = NULL;
  const dt_dev_pixelpipe_iop_t *const piece = _get_backbuf_source_piece(backbuf, op);
  if(IS_NULL_PTR(piece) || !dt_dev_pixelpipe_cache_peek_gui(darktable.develop->preview_pipe, piece, &data, &entry, NULL, NULL, NULL))
    return;

  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, TRUE, entry);

  const size_t binning_size = (vertical) ? 4 * TONES * backbuf->height : 4 * TONES * backbuf->width;
  uint32_t *const restrict bins = dt_pixelpipe_cache_alloc_align_cache(
      binning_size * sizeof(uint32_t),
      0);
  uint8_t *const restrict image = dt_pixelpipe_cache_alloc_align_cache(
      binning_size * sizeof(uint8_t),
      0);
  if(IS_NULL_PTR(image) || IS_NULL_PTR(bins)) goto error;

  // 1. Pixel binning along columns/rows, aka compute a column/row-wise histogram
  _bin_pixels_waveform(data, bins, backbuf->width, backbuf->height, binning_size, vertical);

  // 2. Paint image.
  // In a 1D histogram, pixel frequencies are shown as height (y axis) for each RGB quantum (x axis).
  // Here, we do a sort of 2D histogram : pixel frequencies are shown as opacity ("z" axis),
  // for each image column (x axis), for each RGB quantum (y axis)
  const size_t img_width = (vertical) ? TONES : backbuf->width;
  const size_t img_height = (vertical) ? backbuf->height : TONES;
  const uint32_t overall_max_hist = _find_max_histogram(bins, binning_size);
  _create_waveform_image(bins, image, overall_max_hist, img_width, img_height);

  // 3. Send everything to GUI buffer.
  if(overall_max_hist > 0)
  {
    cairo_save(cr);

    // Paint background - Color not exposed to user theme because this is tricky
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
    cairo_fill(cr);

    cairo_set_source_rgb(cr, 0.21, 0.21, 0.21);
    dt_draw_grid(cr, 4, 0, 0, width, height);

    if(parade)
      _paint_parade(cr, image, width, height, img_width, img_height, vertical);
    else
      _paint_waveform(cr, image, width, height, img_width, img_height, vertical);

    cairo_restore(cr);
  }

error:;
  dt_pixelpipe_cache_free_align(bins);
  dt_pixelpipe_cache_free_align(image);
  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, FALSE, entry);
}

static float _Luv_to_vectorscope_coord_zoom(const float value, const float zoom)
{
  // Convert u, v coordinates of Luv vectors into x, y coordinates
  // into the space of the vectorscope square buffer
  return (value + zoom) * (HISTOGRAM_BINS - 1) / (2.f * zoom);
}

static float _vectorscope_coord_zoom_to_Luv(const float value, const float zoom)
{
  // Inverse of the above
  return value * (2.f * zoom) / (HISTOGRAM_BINS - 1) - zoom;
}

static void _bin_pixels_vectorscope_in_roi(const float *const restrict image, uint32_t *const restrict vectorscope,
                                           const size_t min_x, const size_t max_x, const size_t min_y,
                                           const size_t max_y, const size_t width, const float zoom,
                                           dt_lib_histogram_t *d)
{
#ifdef _OPENMP
#ifndef _WIN32
#pragma omp parallel for default(firstprivate) \
        reduction(+: vectorscope[0: HISTOGRAM_BINS * HISTOGRAM_BINS]) 
#else
#pragma omp parallel for default(firstprivate) \
        shared(vectorscope) 
#endif
#endif
  for(size_t i = min_y; i < max_y; i++)
    for(size_t j = min_x; j < max_x; j++)
    {
      dt_aligned_pixel_t XYZ_D50 = { 0.f };
      dt_aligned_pixel_t xyY = { 0.f };
      dt_aligned_pixel_t Luv = { 0.f };
      _scope_pixel_to_xyz(image + (i * width + j) * 4, XYZ_D50, d);
      dt_XYZ_to_xyY(XYZ_D50, xyY);
      dt_xyY_to_Luv(xyY, Luv);

      // Luv is sampled between 0 and 100.0f, u and v between +/- 220.f
      const size_t U_index = (size_t)CLAMP(roundf(_Luv_to_vectorscope_coord_zoom(Luv[1], zoom)), 0, HISTOGRAM_BINS - 1);
      const size_t V_index = (size_t)CLAMP(roundf(_Luv_to_vectorscope_coord_zoom(Luv[2], zoom)), 0, HISTOGRAM_BINS - 1);

      // We put V = 0 at the bottom of the image.
      vectorscope[(HISTOGRAM_BINS - 1 - V_index) * HISTOGRAM_BINS + U_index]++;
    }
}

static inline void _bin_pickers_vectorscope(const float *const restrict image,
                                            uint32_t *const restrict vectorscope, const size_t width,
                                            const size_t height, const float zoom, dt_lib_histogram_t *d,
                                            const dt_colorpicker_sample_t *const sample)
{
  if(sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
  {
    const size_t box[4] = {
      CLAMP((size_t)roundf(sample->box[0] * width), 0, width),
      CLAMP((size_t)roundf(sample->box[1] * height), 0, height),
      CLAMP((size_t)roundf(sample->box[2] * width), 0, width),
      CLAMP((size_t)roundf(sample->box[3] * height), 0, height)
    };
    _bin_pixels_vectorscope_in_roi(image, vectorscope, box[0], box[2], box[1], box[3], width, zoom, d);
  }
  else
  {
    const size_t x = CLAMP((size_t)roundf(sample->point[0] * width), 0, width - 1);
    const size_t y = CLAMP((size_t)roundf(sample->point[1] * height), 0, height - 1);
    _bin_pixels_vectorscope_in_roi(image, vectorscope, x, x + 1, y, y + 1, width, zoom, d);
  }
}


static void _create_vectorscope_image(const uint32_t *const restrict vectorscope, uint8_t *const restrict image,
                                      const uint32_t max_hist, const float zoom)
{
  const dt_iop_order_iccprofile_info_t *const profile = darktable.develop->preview_pipe->output_profile_info;
  __OMP_PARALLEL_FOR__(collapse(2))
  for(size_t i = 0; i < HISTOGRAM_BINS; i++)
    for(size_t j = 0; j < HISTOGRAM_BINS; j++)
    {
      const size_t index = (HISTOGRAM_BINS - 1 - i) * HISTOGRAM_BINS + j;
      const float value = sqrtf((float)vectorscope[index] / (float)max_hist);
      dt_aligned_pixel_t RGB = { 0.f };
      // RGB gamuts tend to have a max chroma around L = 67
      dt_aligned_pixel_t Luv = { 25.f, _vectorscope_coord_zoom_to_Luv(j, zoom), _vectorscope_coord_zoom_to_Luv(i, zoom), 1.f };
      dt_aligned_pixel_t xyY = { 0.f };
      dt_aligned_pixel_t XYZ = { 0.f };
      dt_Luv_to_xyY(Luv, xyY);
      for(int c = 0; c < 2; c++) xyY[c] = fmaxf(xyY[c], 0.f);
      dt_xyY_to_XYZ(xyY, XYZ);
      for(int c = 0; c < 3; c++) XYZ[c] = fmaxf(XYZ[c], 0.f);
      dt_apply_transposed_color_matrix(XYZ, profile->matrix_out_transposed, RGB);

      // We will normalize RGB to get closer to display peak emission
      for(int c = 0; c < 3; c++) RGB[c] = fmaxf(RGB[c], 0.f);
      const float max_RGB = fmax(RGB[0], fmaxf(RGB[1], RGB[2]));
      for(int c = 0; c < 3; c++) RGB[c] /= max_RGB;

      image[index * 4 + 3] = (uint8_t)roundf(value * 255.f); // alpha
      // Premultiply alpha
      image[index * 4 + 2] = (uint8_t)roundf(powf(RGB[0] * value, 1.f / 2.2f) * 255.f);
      image[index * 4 + 1] = (uint8_t)roundf(powf(RGB[1] * value, 1.f / 2.2f) * 255.f);
      image[index * 4 + 0] = (uint8_t)roundf(powf(RGB[2] * value, 1.f / 2.2f) * 255.f);
    }
}

static void _bin_vectorscope(const float *const restrict image, uint32_t *const vectorscope,
                             const size_t width, const size_t height,
                             const float zoom, dt_lib_histogram_t *d)
{
  __OMP_FOR_SIMD__(aligned(vectorscope: 64) )
  for(size_t k = 0; k < HISTOGRAM_BINS * HISTOGRAM_BINS; k++) vectorscope[k] = 0;

  if(dt_conf_get_bool("ui_last/colorpicker_restrict_histogram"))
  {
    // Bin only areas within color pickers
    GSList *samples = darktable.develop->color_picker.samples;
    while(samples)
    {
      dt_colorpicker_sample_t *sample = samples->data;
      _bin_pickers_vectorscope(image, vectorscope, width, height, zoom, d, sample);
      samples = g_slist_next(samples);
    }

    if(darktable.develop->color_picker.picker)
      _bin_pickers_vectorscope(image, vectorscope, width, height, zoom, d,
                               darktable.develop->color_picker.primary_sample);
  }
  else
  {
    // Bin the whole image
    _bin_pixels_vectorscope_in_roi(image, vectorscope, 0, width, 0, height, width, zoom, d);
  }
}


static void _process_vectorscope(dt_backbuf_t *backbuf, const char *op, cairo_t *cr, const int width,
                                 const int height, const float zoom, dt_lib_histogram_t *d)
{
  dt_iop_order_iccprofile_info_t *profile = darktable.develop->preview_pipe->output_profile_info;
  if(IS_NULL_PTR(profile)) return;

  struct dt_pixel_cache_entry_t *entry = NULL;
  void *data = NULL;
  const dt_dev_pixelpipe_iop_t *const piece = _get_backbuf_source_piece(backbuf, op);
  if(IS_NULL_PTR(piece) || !dt_dev_pixelpipe_cache_peek_gui(darktable.develop->preview_pipe, piece, &data, &entry, NULL, NULL, NULL))
    return;

  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, TRUE, entry);

  uint32_t *const restrict vectorscope = dt_pixelpipe_cache_alloc_align_cache(
      HISTOGRAM_BINS * HISTOGRAM_BINS * sizeof(uint32_t),
      0);
  uint8_t *const restrict image = dt_pixelpipe_cache_alloc_align_cache(
      4 * HISTOGRAM_BINS * HISTOGRAM_BINS * sizeof(uint8_t),
      0);
  if(IS_NULL_PTR(vectorscope) || IS_NULL_PTR(image)) goto error;

  _bin_vectorscope(data, vectorscope, backbuf->width, backbuf->height, zoom, d);

  const uint32_t max_hist = _find_max_histogram(vectorscope, HISTOGRAM_BINS * HISTOGRAM_BINS);
  _create_vectorscope_image(vectorscope, image, max_hist, zoom);

  // 2. Draw
  if(max_hist > 0)
  {
    const size_t stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, HISTOGRAM_BINS);
    cairo_surface_t *background = cairo_image_surface_create_for_data(image, CAIRO_FORMAT_ARGB32, HISTOGRAM_BINS, HISTOGRAM_BINS, stride);
    cairo_translate(cr, (double)(width - height) / 2., 0.);
    cairo_scale(cr, (double)height / HISTOGRAM_BINS, (double)height / HISTOGRAM_BINS);

    const double radius = (float)(HISTOGRAM_BINS - 1) / 2 - DT_PIXEL_APPLY_DPI(1.);
    const double x_center = (float)(HISTOGRAM_BINS - 1) / 2;

    // Background circle - Color will not be exposed to user theme because this is tricky
    cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
    cairo_arc(cr, x_center, x_center, radius, 0., 2. * M_PI);
    cairo_fill(cr);

    // Center circle
    cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
    cairo_arc(cr, x_center, x_center, 2., 0., 2. * M_PI);
    cairo_fill(cr);

    // Concentric circles
    for(int k = 0; k < 4; k++)
    {
      cairo_arc(cr, x_center, x_center, (double)k * HISTOGRAM_BINS / 8., 0., 2. * M_PI);
      cairo_stroke(cr);
    }

    // RGB space primaries and secondaries
    dt_aligned_pixel_t colors[6] = { { 1.f, 0.f, 0.f, 0.f }, { 1.f, 1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f, 0.f },
                                     { 0.f, 1.f, 1.f, 0.f }, { 0.f, 0.f, 1.f, 0.f }, { 1.f, 0.f, 1.f, 0.f } };

    cairo_save(cr);
    cairo_arc(cr, x_center, x_center, radius, 0., 2. * M_PI);
    cairo_clip(cr);

    for(size_t k = 0; k < 6; k++)
    {
      dt_aligned_pixel_t XYZ_D50 = { 0.f };
      dt_aligned_pixel_t xyY = { 0.f };
      dt_aligned_pixel_t Luv = { 0.f };
      dt_ioppr_rgb_matrix_to_xyz(colors[k], XYZ_D50, profile->matrix_in_transposed, profile->lut_in, profile->unbounded_coeffs_in,
                                profile->lutsize, profile->nonlinearlut);
      dt_XYZ_to_xyY(XYZ_D50, xyY);
      dt_xyY_to_Luv(xyY, Luv);

      const double x = _Luv_to_vectorscope_coord_zoom(Luv[1], zoom);
      // Remember v = 0 is at the bottom of the square while Cairo puts y = 0 on top
      const double y = HISTOGRAM_BINS - 1 - _Luv_to_vectorscope_coord_zoom(Luv[2], zoom);

      // First, draw hue angles
      dt_aligned_pixel_t Lch = { 0.f };
      dt_Luv_to_Lch(Luv, Lch);

      const double delta_x = radius * cosf(Lch[2]);
      const double delta_y = radius * sinf(Lch[2]);
      const double destination_x = x_center + delta_x;
      const double destination_y = (HISTOGRAM_BINS - 1) - (x_center + delta_y);
      cairo_move_to(cr, x_center, x_center);
      cairo_line_to(cr, destination_x, destination_y);
      cairo_set_source_rgba(cr, colors[k][0], colors[k][1], colors[k][2], 0.5);
      cairo_stroke(cr);

      // Then draw color squares and ensure center is filled with scope background color
      const double half_square = DT_PIXEL_APPLY_DPI(4);
      cairo_arc(cr, x, y, half_square, 0, 2. * M_PI);
      cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
      cairo_fill_preserve(cr);
      cairo_set_source_rgb(cr, colors[k][0], colors[k][1], colors[k][2]);
      cairo_stroke(cr);
    }
    cairo_restore(cr);

    // Hues ring
    cairo_save(cr);
    cairo_arc(cr, x_center, x_center, radius - DT_PIXEL_APPLY_DPI(1.), 0., 2. * M_PI);
    cairo_set_source_rgb(cr, 0.33, 0.33, 0.33);
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));
    cairo_stroke(cr);
    cairo_restore(cr);

    for(size_t h = 0; h < 180; h++)
    {
      dt_aligned_pixel_t Lch = { 50.f, 110.f, h / 180.f * 2.f * M_PI_F, 1.f };
      dt_aligned_pixel_t Luv = { 0.f };
      dt_aligned_pixel_t xyY = { 0.f };
      dt_aligned_pixel_t XYZ = { 0.f };
      dt_aligned_pixel_t RGB = { 0.f };
      dt_Lch_to_Luv(Lch, Luv);
      dt_Luv_to_xyY(Luv, xyY);
      dt_xyY_to_XYZ(xyY, XYZ);
      dt_apply_transposed_color_matrix(XYZ, profile->matrix_out_transposed, RGB);
      const float max_RGB = fmaxf(fmaxf(RGB[0], RGB[1]), RGB[2]);
      for(int c = 0; c < 3; c++) RGB[c] /= max_RGB;
      const double delta_x = (radius - DT_PIXEL_APPLY_DPI(1.)) * cosf(Lch[2]);
      const double delta_y = (radius - DT_PIXEL_APPLY_DPI(1.)) * sinf(Lch[2]);
      const double destination_x = x_center + delta_x;
      const double destination_y = (HISTOGRAM_BINS - 1) - (x_center + delta_y);
      cairo_set_source_rgba(cr, RGB[0], RGB[1], RGB[2], 0.7);
      cairo_arc(cr, destination_x, destination_y, DT_PIXEL_APPLY_DPI(1.), 0, 2. * M_PI);
      cairo_fill(cr);
    }

    // Actual vectorscope
    cairo_arc(cr, x_center, x_center, radius, 0., 2. * M_PI);
    cairo_clip(cr);
    cairo_set_source_surface(cr, background, 0., 0.);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
    cairo_paint(cr);
    cairo_surface_destroy(background);

    // Draw the skin tones area
    // Values obtained with :
    // get_skin_tones_range();
    const float max_c = 49.34f;
    const float min_c = 9.00f;
    const float max_h = 0.99f;
    const float min_h = 0.26f;

    const float n_w_x = min_c * cosf(max_h);
    const float n_w_y = min_c * sinf(max_h);
    const float n_e_x = max_c * cosf(max_h);
    const float n_e_y = max_c * sinf(max_h);
    const float s_e_x = max_c * cosf(min_h);
    const float s_e_y = max_c * sinf(min_h);
    const float s_w_x = min_c * cosf(min_h);
    const float s_w_y = min_c * sinf(min_h);
    cairo_move_to(cr, _Luv_to_vectorscope_coord_zoom(n_w_x, zoom),
                      (HISTOGRAM_BINS - 1) - _Luv_to_vectorscope_coord_zoom(n_w_y, zoom));
    cairo_line_to(cr, _Luv_to_vectorscope_coord_zoom(n_e_x, zoom),
                      (HISTOGRAM_BINS - 1) - _Luv_to_vectorscope_coord_zoom(n_e_y, zoom));
    cairo_line_to(cr, _Luv_to_vectorscope_coord_zoom(s_e_x, zoom),
                      (HISTOGRAM_BINS - 1) - _Luv_to_vectorscope_coord_zoom(s_e_y, zoom));
    cairo_line_to(cr, _Luv_to_vectorscope_coord_zoom(s_w_x, zoom),
                      (HISTOGRAM_BINS - 1) - _Luv_to_vectorscope_coord_zoom(s_w_y, zoom));
    cairo_line_to(cr, _Luv_to_vectorscope_coord_zoom(n_w_x, zoom),
                      (HISTOGRAM_BINS - 1) - _Luv_to_vectorscope_coord_zoom(n_w_y, zoom));
    cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
    cairo_stroke(cr);
  }

error:;
  dt_pixelpipe_cache_free_align(image);
  dt_pixelpipe_cache_free_align(vectorscope);
  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, FALSE, entry);
}

gboolean _needs_recompute(dt_lib_histogram_t *d, const int width, const int height)
{
  // Check if cache is up-to-date
  dt_lib_histogram_scope_type_t view = dt_bauhaus_combobox_get(d->display);
  gboolean hash_match = (d->cache.hash == dt_dev_backbuf_get_hash(d->backbuf));
  gboolean size_match = (d->cache.width == width && d->cache.height == height);
  gboolean zoom_match = (d->cache.zoom == d->zoom);
  gboolean view_match = (d->cache.view == view);
  gboolean has_surface = (!IS_NULL_PTR(d->cst));
  return !(hash_match && size_match && zoom_match && view_match && has_surface);
}


static gboolean _draw_callback(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  // Note: the draw callback is called from our own callback (mapped to "preview pipe finished recomputing" signal)
  // but is also called by Gtk when the main window is resized, exposed, etc.
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)user_data;
  if(IS_NULL_PTR(d->cst)) return 1;
  cairo_set_source_surface(crf, d->cst, 0, 0);
  cairo_paint(crf);
  return 0;
}


void _get_allocation_size(dt_lib_histogram_t *d, int *width, int *height)
{
  GtkAllocation allocation;
  gtk_widget_get_allocation(d->scope_draw, &allocation);
  *width = allocation.width;
  *height = allocation.height;
}

gboolean _redraw_surface(dt_lib_histogram_t *d)
{
  if(IS_NULL_PTR(d->cst)) return 1;

  dt_times_t start;
  dt_get_times(&start);

  int width, height;
  _get_allocation_size(d, &width, &height);

  // Save cache integrity
  d->cache.hash = dt_dev_backbuf_get_hash(d->backbuf);
  d->cache.width = width;
  d->cache.height = height;
  d->cache.zoom = d->zoom;
  d->cache.view = dt_bauhaus_combobox_get(d->display);

  cairo_t *cr = cairo_create(d->cst);

  // Paint background
  gtk_render_background(gtk_widget_get_style_context(d->scope_draw), cr, 0, 0, width, height);
  cairo_set_line_width(cr, 1.); // we want exactly 1 px no matter the resolution

  // Paint content
  switch(dt_bauhaus_combobox_get(d->display))
  {
    case DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM:
    {
      _process_histogram(d->backbuf, d->op, cr, width, height);
      break;
    }
    case DT_LIB_HISTOGRAM_SCOPE_WAVEFORM_HORIZONTAL:
    {
      _process_waveform(d->backbuf, d->op, cr, width, height, FALSE, FALSE);
      break;
    }
    case DT_LIB_HISTOGRAM_SCOPE_WAVEFORM_VERTICAL:
    {
      _process_waveform(d->backbuf, d->op, cr, width, height, TRUE, FALSE);
      break;
    }
    case DT_LIB_HISTOGRAM_SCOPE_PARADE_HORIZONTAL:
    {
      _process_waveform(d->backbuf, d->op, cr, width, height, FALSE, TRUE);
      break;
    }
    case DT_LIB_HISTOGRAM_SCOPE_PARADE_VERTICAL:
    {
      _process_waveform(d->backbuf, d->op, cr, width, height, TRUE, TRUE);
      break;
    }
    case DT_LIB_HISTOGRAM_SCOPE_VECTORSCOPE:
    {
      _process_vectorscope(d->backbuf, d->op, cr, width, height, d->zoom, d);
      break;
    }
    default:
      break;
  }

  cairo_destroy(cr);
  dt_show_times_f(&start, "[histogram]", "redraw");
  return 0;
}

static void _destroy_surface(dt_lib_histogram_t *d)
{
  if(d->cst && cairo_surface_get_reference_count(d->cst) > 0) cairo_surface_destroy(d->cst);
  if(d->cst && cairo_surface_get_reference_count(d->cst) == 0) d->cst = NULL;
}

gboolean _trigger_recompute(dt_lib_histogram_t *d)
{
  int width, height;
  _get_allocation_size(d, &width, &height);

  if(_is_backbuf_ready(d) && _needs_recompute(d, width, height))
  {
    _destroy_surface(d);
    // If width and height have changed, we need to recreate the surface.
    // Recreate it anyway.
    d->cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    _redraw_surface(d);
    // Don't send gtk_queue_redraw event from here, catch the return value and do it in the calling function
    //fprintf(stdout, "recreate histograms\n");
    return 1;
  }

  return 0;
}

static const dt_dev_pixelpipe_iop_t *_get_backbuf_source_piece(const dt_backbuf_t *backbuf, const char *op)
{
  dt_develop_t *const dev = darktable.develop;
  dt_dev_pixelpipe_t *const pipe = dev ? dev->preview_pipe : NULL;
  if(IS_NULL_PTR(dev) || IS_NULL_PTR(pipe) || IS_NULL_PTR(op) || IS_NULL_PTR(backbuf)) return NULL;

  dt_iop_module_t *const module = dt_iop_get_module_by_op_priority(dev->iop, op, 0);
  if(IS_NULL_PTR(module)) return NULL;

  const dt_dev_pixelpipe_iop_t *piece = dt_dev_pixelpipe_get_module_piece(pipe, module);
  if(IS_NULL_PTR(piece)) return NULL;

  if(!strcmp(op, "gamma"))
    piece = dt_dev_pixelpipe_get_prev_enabled_piece(pipe, piece);

  if(IS_NULL_PTR(piece)) return NULL;
  if(piece->global_hash != dt_dev_backbuf_get_hash(backbuf)) return NULL;

  return piece;
}

/**
 * @brief Resolve the live preview-stage geometry behind one global histogram backbuffer.
 *
 * @details
 * The global histogram stores only the published cache hash, width, and height. For color-picker
 * sampling we also need the live ROI, colorspace, and backtransform cut in the current preview
 * graph so the picker point drawn on the final image is mapped back to the selected histogram
 * stage before sampling pixels.
 *
 * We reopen the stage by operation name because the current preview graph is authoritative once the
 * preview run completed. The published backbuffer hash must still match that live graph, otherwise
 * we are looking at stale geometry and should not trust it.
 *
 * @param op Histogram stage operation name.
 * @param backbuf Published histogram backbuffer for @p op.
 * @param roi Output ROI of the sampled stage buffer.
 * @param dsc Buffer descriptor of the sampled stage buffer.
 * @param iop_order IOP order at which to stop the backtransform.
 * @param direction Inclusive/exclusive transform cut matching the sampled stage buffer.
 *
 * @return TRUE when the live preview graph still matches the published backbuffer, FALSE otherwise.
 */
static gboolean _resolve_backbuf_sampling_source(const char *const op, const dt_backbuf_t *const backbuf,
                                                 dt_iop_roi_t *const roi, dt_iop_buffer_dsc_t *const dsc,
                                                 double *const iop_order, int *const direction)
{
  dt_develop_t *const dev = darktable.develop;
  dt_dev_pixelpipe_t *const pipe = dev ? dev->preview_pipe : NULL;
  if(IS_NULL_PTR(dev) || IS_NULL_PTR(pipe) || IS_NULL_PTR(op) || IS_NULL_PTR(backbuf) || IS_NULL_PTR(roi) || IS_NULL_PTR(dsc) || !iop_order || IS_NULL_PTR(direction)) return FALSE;

  dt_iop_module_t *const module = dt_iop_get_module_by_op_priority(dev->iop, op, 0);
  if(IS_NULL_PTR(module)) return FALSE;

  const dt_dev_pixelpipe_iop_t *const piece = dt_dev_pixelpipe_get_module_piece(pipe, module);
  if(IS_NULL_PTR(piece)) return FALSE;

  uint64_t hash = piece->global_hash;
  *roi = piece->roi_out;
  *dsc = piece->dsc_out;
  *iop_order = module->iop_order;
  *direction = DT_DEV_TRANSFORM_DIR_FORW_EXCL;

  if(!strcmp(op, "gamma"))
  {
    const dt_dev_pixelpipe_iop_t *const previous_piece = dt_dev_pixelpipe_get_prev_enabled_piece(pipe, piece);
    if(IS_NULL_PTR(previous_piece)) return FALSE;

    hash = previous_piece->global_hash;
    *roi = previous_piece->roi_out;
    *dsc = previous_piece->dsc_out;
    *direction = DT_DEV_TRANSFORM_DIR_FORW_INCL;
  }

  return hash == dt_dev_backbuf_get_hash(backbuf);
}

static void _pixelpipe_pick_from_image(const dt_backbuf_t *const backbuf,
                                       dt_colorpicker_sample_t *const sample, dt_lib_histogram_t *d,
                                       const char *op)
{
  struct dt_pixel_cache_entry_t *entry = NULL;
  void *data = NULL;
  const dt_dev_pixelpipe_iop_t *const source_piece = _get_backbuf_source_piece(backbuf, op);
  if(IS_NULL_PTR(source_piece)
     || !dt_dev_pixelpipe_cache_peek_gui(darktable.develop->preview_pipe, source_piece, &data, &entry, NULL, NULL, NULL))
    return;

  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, TRUE, entry);

  const float *const pixel = data;
  dt_iop_buffer_dsc_t dsc = {
    .channels = 4,
    .datatype = TYPE_FLOAT,
    .bpp = 4 * sizeof(float),
    .cst = IOP_CS_RGB
  };
  dt_iop_roi_t roi = {
    .x = 0,
    .y = 0,
    .width = (int)backbuf->width,
    .height = (int)backbuf->height,
    .scale = 1.0
  };
  double iop_order = 0.0;
  int direction = DT_DEV_TRANSFORM_DIR_FORW_EXCL;
  const gboolean have_live_stage = _resolve_backbuf_sampling_source(op, backbuf, &roi, &dsc, &iop_order, &direction);
  int box[4];

  if(have_live_stage)
  {
    dt_boundingbox_t fbox = { 0.0f };

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

    dt_dev_distort_backtransform_plus(darktable.develop->preview_pipe, iop_order,
                                      direction, fbox, 2);

    /* The backtransform returns stage coordinates in the sampled piece space. Shift them by the
       stage ROI before clamping so the picker box matches the published cache window exactly. */
    fbox[0] -= roi.x;
    fbox[1] -= roi.y;
    fbox[2] -= roi.x;
    fbox[3] -= roi.y;

    box[0] = fminf(fbox[0], fbox[2]);
    box[1] = fminf(fbox[1], fbox[3]);
    box[2] = fmaxf(fbox[0], fbox[2]);
    box[3] = fmaxf(fbox[1], fbox[3]);

    if(sample->size == DT_LIB_COLORPICKER_SIZE_POINT)
    {
      box[2] += 1;
      box[3] += 1;
    }

    box[0] = MIN(roi.width - 1, MAX(0, box[0]));
    box[1] = MIN(roi.height - 1, MAX(0, box[1]));
    box[2] = MIN(roi.width - 1, MAX(0, box[2]));
    box[3] = MIN(roi.height - 1, MAX(0, box[3]));
  }
  else
  {
    dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, FALSE, entry);
    return;
  }

  dt_aligned_pixel_t mean = { 0.0f };
  dt_aligned_pixel_t min = { INFINITY, INFINITY, INFINITY, INFINITY };
  dt_aligned_pixel_t max = { -INFINITY, -INFINITY, -INFINITY, -INFINITY };

  dt_color_picker_helper(&dsc, pixel, &roi, box, mean, min, max, dsc.cst, IOP_CS_RGB, NULL);

  for_four_channels(k)
  {
    sample->scope[DT_LIB_COLORPICKER_STATISTIC_MEAN][k] = mean[k];
    sample->scope[DT_LIB_COLORPICKER_STATISTIC_MIN][k] = min[k];
    sample->scope[DT_LIB_COLORPICKER_STATISTIC_MAX][k] = max[k];
  }

  dt_lib_histogram_t scope = *d;
  scope.op = op;

  for(dt_lib_colorpicker_statistic_t k = 0; k < DT_LIB_COLORPICKER_STATISTIC_N; k++)
  {
    _scope_pixel_to_display_rgb(sample->scope[k], sample->display[k], &scope);

    dt_aligned_pixel_t XYZ = { 0.f };
    _scope_pixel_to_xyz(sample->scope[k], XYZ, &scope);
    dt_XYZ_to_Lab(XYZ, sample->lab[k]);
  }

  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, FALSE, entry);
}

static void _pixelpipe_pick_samples(dt_lib_histogram_t *d)
{
  GSList *samples = darktable.develop->color_picker.samples;
  while(samples)
  {
    dt_colorpicker_sample_t *sample = samples->data;
    if(!sample->locked)
    {
      const char *const op = sample->backbuf_op[0] ? sample->backbuf_op : d->op;
      dt_backbuf_t *const backbuf = _get_histogram_backbuf(darktable.develop, op);
      if(backbuf) _pixelpipe_pick_from_image(backbuf, sample, d, op);
    }
    samples = g_slist_next(samples);
  }

  if(darktable.develop->color_picker.picker)
    _pixelpipe_pick_from_image(d->backbuf, darktable.develop->color_picker.primary_sample, d, d->op);
}


static void _update_everything(dt_lib_module_t *self)
{
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;

  if(_trigger_recompute(d))
  {
    _redraw_scopes(d);
  }

  _pixelpipe_pick_samples(d);

  for(GSList *samples = darktable.develop->color_picker.samples;
      samples;
      samples = g_slist_next(samples))
  {
    _update_sample_label(self, samples->data);
  }

  _update_sample_label(self, darktable.develop->color_picker.primary_sample);

  // allow live sample button to work for iop samples
  gtk_widget_set_sensitive(GTK_WIDGET(d->add_sample_button),
                           !IS_NULL_PTR(darktable.develop->color_picker.picker));
}

static gboolean _refresh_global_picker(dt_lib_module_t *self)
{
  if(IS_NULL_PTR(self) || IS_NULL_PTR(self->data)) return FALSE;

  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  d->backbuf = _get_histogram_backbuf(darktable.develop, d->op);

  if(!_is_backbuf_ready(d)) return FALSE;

  _update_everything(self);
  return TRUE;
}


static void _lib_histogram_history_resync_callback(gpointer instance, dt_lib_module_t *self)
{
  (void)instance;
  _sync_pending_histogram_hashes(self);
}

static void _lib_histogram_cacheline_ready_callback(gpointer instance, const guint64 hash, dt_lib_module_t *self)
{
  (void)instance;
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  if(IS_NULL_PTR(d) || !_remove_pending_hash(d, hash)) return;

  _schedule_histogram_refresh(self);
}

// this is only called in darkroom view when history was resynchronized
void view_enter(struct dt_lib_module_t *self, struct dt_view_t *old_view, struct dt_view_t *new_view)
{
  dt_lib_histogram_t *d = self->data;
  _reset_cache(d);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_HISTORY_RESYNC,
                                  G_CALLBACK(_preview_history_resync_callback), NULL);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_CACHELINE_READY,
                                  G_CALLBACK(_preview_cacheline_ready_callback), NULL);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_HISTORY_RESYNC,
                                  G_CALLBACK(_lib_histogram_history_resync_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_CACHELINE_READY,
                                  G_CALLBACK(_lib_histogram_cacheline_ready_callback), self);
}

void view_leave(struct dt_lib_module_t *self, struct dt_view_t *old_view, struct dt_view_t *new_view)
{
  dt_lib_histogram_t *d = self->data;
  _reset_cache(d);

  _clear_pending_preview_histograms();
  _clear_pending_hashes(d);
  if(d->refresh_idle_source != 0)
  {
    g_source_remove(d->refresh_idle_source);
    d->refresh_idle_source = 0;
  }

  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_preview_history_resync_callback), NULL);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_preview_cacheline_ready_callback), NULL);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_lib_histogram_history_resync_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_lib_histogram_cacheline_ready_callback), self);
}


void _stage_callback(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  const int value = dt_bauhaus_combobox_get(widget);
  _backbuf_int_to_op(value, d);
  dt_conf_set_string("plugin/darkroom/histogram/op", d->op);

  // Disable vectorscope for RAW stage
  dt_bauhaus_combobox_entry_set_sensitive(d->display, DT_LIB_HISTOGRAM_SCOPE_VECTORSCOPE,
                                          strcmp(d->op, "initialscale"));

  _refresh_preview_histograms(darktable.develop);
  _sync_pending_histogram_hashes(self);
}


void _display_callback(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  dt_conf_set_int("plugin/darkroom/histogram/display", dt_bauhaus_combobox_get(d->display));
  if(_trigger_recompute(d)) _redraw_scopes(d);
}


static void _resize_callback(GtkWidget *widget, GdkRectangle *allocation, dt_lib_histogram_t *d)
{
  _reset_cache(d);
  _trigger_recompute(d);
  // Don't start a redraw from here, Gtk does it automatically on resize event
}

static gboolean _area_scrolled_callback(GtkWidget *widget, GdkEventScroll *event, dt_lib_histogram_t *d)
{
  if(dt_bauhaus_combobox_get(d->display) != DT_LIB_HISTOGRAM_SCOPE_VECTORSCOPE) return FALSE;

  int delta_y = 0;
  if (!dt_gui_get_scroll_unit_deltas(event, NULL, &delta_y)) return TRUE;

  const float new_value = 4.f * delta_y + d->zoom;

  if(new_value < 512.f && new_value > 32.f)
  {
    d->zoom = new_value;
    dt_conf_set_float("plugin/darkroom/histogram/zoom", new_value);
    if(_is_backbuf_ready(d))
    {
      _redraw_surface(d);
      _redraw_scopes(d);
    }
  }
  return TRUE;
}

void _set_params(dt_lib_histogram_t *d)
{
  d->op = dt_conf_get_string_const("plugin/darkroom/histogram/op");
  if(!strcmp(d->op, "demosaic"))
  {
    d->op = "initialscale";
    dt_conf_set_string("plugin/darkroom/histogram/op", d->op);
  }
  d->backbuf = _get_histogram_backbuf(darktable.develop, d->op);
  d->zoom = fminf(fmaxf(dt_conf_get_float("plugin/darkroom/histogram/zoom"), 32.f), 252.f);

  // Disable RAW stage for non-RAW images
  dt_bauhaus_combobox_entry_set_sensitive(d->stage, 0, dt_image_is_raw(&darktable.develop->image_storage));

  // Disable vectorscope if RAW stage is selected
  dt_bauhaus_combobox_entry_set_sensitive(d->display, DT_LIB_HISTOGRAM_SCOPE_VECTORSCOPE,
                                          strcmp(d->op, "initialscale"));

  dt_bauhaus_combobox_set(d->display, dt_conf_get_int("plugin/darkroom/histogram/display"));
  dt_bauhaus_combobox_set(d->stage, _backbuf_op_to_int(d));
}


static gboolean _sample_draw_callback(GtkWidget *widget, cairo_t *cr, dt_colorpicker_sample_t *sample)
{
  const guint width = gtk_widget_get_allocated_width(widget);
  const guint height = gtk_widget_get_allocated_height(widget);

  set_color(cr, sample->swatch);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill (cr);

  // if the sample is locked we want to add a lock
  if(sample->locked)
  {
    const int border = DT_PIXEL_APPLY_DPI(2);
    const int icon_width = width - 2 * border;
    const int icon_height = height - 2 * border;
    if(icon_width > 0 && icon_height > 0)
    {
      GdkRGBA fg_color;
      gtk_style_context_get_color(gtk_widget_get_style_context(widget), gtk_widget_get_state_flags(widget), &fg_color);

      gdk_cairo_set_source_rgba(cr, &fg_color);
      dtgtk_cairo_paint_lock(cr, border, border, icon_width, icon_height, 0, NULL);
    }
  }

  return FALSE;
}

static void _update_sample_label(dt_lib_module_t *self, dt_colorpicker_sample_t *sample)
{
  dt_lib_histogram_t *d = self->data;
  const dt_lib_colorpicker_statistic_t statistic
      = (d->model == DT_LIB_COLORPICKER_MODEL_RGB) ? d->statistic : DT_LIB_COLORPICKER_STATISTIC_MEAN;

  sample->swatch.red   = sample->display[statistic][0];
  sample->swatch.green = sample->display[statistic][1];
  sample->swatch.blue  = sample->display[statistic][2];
  for_each_channel(ch)
    sample->label_rgb[ch]  = (int)roundf(sample->scope[statistic][ch] * 255.f);

  // Setting the output label
  char text[128] = { 0 };
  dt_aligned_pixel_t alt = { 0 };

  switch(d->model)
  {
    case DT_LIB_COLORPICKER_MODEL_RGB:
      snprintf(text, sizeof(text), "%6d %6d %6d", sample->label_rgb[0], sample->label_rgb[1], sample->label_rgb[2]);
      break;

    case DT_LIB_COLORPICKER_MODEL_LAB:
      snprintf(text, sizeof(text), "%6.02f %6.02f %6.02f",
               CLAMP(sample->lab[statistic][0], .0f, 100.0f), sample->lab[statistic][1], sample->lab[statistic][2]);
      break;

    case DT_LIB_COLORPICKER_MODEL_LCH:
      dt_Lab_2_LCH(sample->lab[statistic], alt);
      snprintf(text, sizeof(text), "%6.02f %6.02f %6.02f", CLAMP(alt[0], .0f, 100.0f), alt[1], alt[2] * 360.f);
      break;

    case DT_LIB_COLORPICKER_MODEL_HSL:
      dt_RGB_2_HSL(sample->scope[statistic], alt);
      snprintf(text, sizeof(text), "%6.02f %6.02f %6.02f", alt[0] * 360.f, alt[1] * 100.f, alt[2] * 100.f);
      break;

    case DT_LIB_COLORPICKER_MODEL_HSV:
      dt_RGB_2_HSV(sample->scope[statistic], alt);
      snprintf(text, sizeof(text), "%6.02f %6.02f %6.02f", alt[0] * 360.f, alt[1] * 100.f, alt[2] * 100.f);
      break;

    default:
    case DT_LIB_COLORPICKER_MODEL_NONE:
      snprintf(text, sizeof(text), "\342\227\216");
      break;
  }

  if(g_strcmp0(gtk_label_get_text(GTK_LABEL(sample->output_label)), text))
    gtk_label_set_text(GTK_LABEL(sample->output_label), text);

  gtk_widget_queue_draw(sample->color_patch);
}

static void _update_picker_output(dt_lib_module_t *self)
{
  _update_everything(self);
}

static void _picker_button_toggled(GtkToggleButton *button, dt_lib_histogram_t *d)
{
  gtk_widget_set_sensitive(GTK_WIDGET(d->add_sample_button), gtk_toggle_button_get_active(button));
}

static void _update_size(dt_lib_module_t *self, dt_lib_colorpicker_size_t size)
{
  darktable.develop->color_picker.primary_sample->size = size;
  _update_picker_output(self);
}

/* set sample area proxy impl */

static void _set_sample_box_area(dt_lib_module_t *self, const dt_boundingbox_t box)
{
  // primary sample always follows/represents current picker
  for(int k = 0; k < 4; k++)
    darktable.develop->color_picker.primary_sample->box[k] = box[k];

  _update_size(self, DT_LIB_COLORPICKER_SIZE_BOX);
  _update_everything(self);
}

static void _set_sample_point(dt_lib_module_t *self, const float pos[2])
{
  // primary sample always follows/represents current picker
  darktable.develop->color_picker.primary_sample->point[0] = pos[0];
  darktable.develop->color_picker.primary_sample->point[1] = pos[1];

  _update_size(self, DT_LIB_COLORPICKER_SIZE_POINT);
  _update_everything(self);
}


static gboolean _sample_tooltip_callback(GtkWidget *widget, gint x, gint y, gboolean keyboard_mode,
                                         GtkTooltip *tooltip, const dt_colorpicker_sample_t *sample)
{
  gchar **sample_parts = g_malloc0_n(14, sizeof(char*));

  sample_parts[3] = g_strdup_printf("%22s(0x%02X%02X%02X)\n<big><b>%14s</b></big>", " ",
                                    CLAMP(sample->label_rgb[0], 0, 255), CLAMP(sample->label_rgb[1], 0, 255),
                                    CLAMP(sample->label_rgb[2], 0, 255), _("RGB"));
  sample_parts[7] = g_strdup_printf("\n<big><b>%14s</b></big>", _("Lab"));

  for(int i = 0; i < DT_LIB_COLORPICKER_STATISTIC_N; i++)
  {
    sample_parts[i] = g_strdup_printf("<span background='#%02X%02X%02X'>%32s</span>",
                                      (int)roundf(CLAMP(sample->display[i][0], 0.f, 1.f) * 255.f),
                                      (int)roundf(CLAMP(sample->display[i][1], 0.f, 1.f) * 255.f),
                                      (int)roundf(CLAMP(sample->display[i][2], 0.f, 1.f) * 255.f), " ");

    sample_parts[i + 4] = g_strdup_printf("<span foreground='#FF7F7F'>%6d</span>  "
                                          "<span foreground='#7FFF7F'>%6d</span>  "
                                          "<span foreground='#7F7FFF'>%6d</span>  %s",
                                          (int)roundf(sample->scope[i][0] * 255.f),
                                          (int)roundf(sample->scope[i][1] * 255.f),
                                          (int)roundf(sample->scope[i][2] * 255.f),
                                          _(dt_lib_colorpicker_statistic_names[i]));

    sample_parts[i + 8] = g_strdup_printf("%6.02f  %6.02f  %6.02f  %s",
                                          sample->lab[i][0], sample->lab[i][1], sample->lab[i][2],
                                          _(dt_lib_colorpicker_statistic_names[i]));
  }

  dt_aligned_pixel_t color;
  dt_Lab_2_LCH(sample->lab[DT_LIB_COLORPICKER_STATISTIC_MEAN], color);
  sample_parts[11] = g_strdup_printf("\n<big><b>%14s</b></big>", _("color"));
  sample_parts[12] = g_strdup_printf("%6s", Lch_to_color_name(color));

  gchar *tooltip_text = g_strjoinv("\n", sample_parts);
  g_strfreev(sample_parts);

  static GtkWidget *view = NULL;
  if(IS_NULL_PTR(view))
  {
    view = gtk_text_view_new();
    dt_gui_add_class(view, "dt_transparent_background");
    dt_gui_add_class(view, "dt_monospace");
    g_signal_connect(G_OBJECT(view), "destroy", G_CALLBACK(gtk_widget_destroyed), &view);
  }

  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
  gtk_text_buffer_set_text(buffer, "", -1);
  GtkTextIter iter;
  gtk_text_buffer_get_start_iter(buffer, &iter);
  gtk_text_buffer_insert_markup(buffer, &iter, tooltip_text, -1);
  gtk_tooltip_set_custom(tooltip, view);
  gtk_widget_map(view); // FIXME: workaround added in order to fix #9908, probably a Gtk issue, remove when fixed upstream

  dt_free(tooltip_text);

  return TRUE;
}

static void _statistic_changed(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_histogram_t *d = self->data;
  d->statistic = dt_bauhaus_combobox_get(widget);
  darktable.develop->color_picker.statistic = (int)d->statistic;
  dt_conf_set_string("ui_last/colorpicker_mode", dt_lib_colorpicker_statistic_names[d->statistic]);
  _update_everything(self);
}

static void _color_mode_changed(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_histogram_t *d = self->data;
  d->model = dt_bauhaus_combobox_get(widget);
  dt_conf_set_string("ui_last/colorpicker_model", dt_lib_colorpicker_model_names[d->model]);
  _update_everything(self);
}

static void _label_size_allocate_callback(GtkWidget *widget, GdkRectangle *allocation, gpointer user_data)
{
  gint label_width;
  gtk_label_set_attributes(GTK_LABEL(widget), NULL);

  PangoStretch stretch = PANGO_STRETCH_NORMAL;

  while(gtk_widget_get_preferred_width(widget, NULL, &label_width),
        label_width > allocation->width && stretch != PANGO_STRETCH_ULTRA_CONDENSED)
  {
    stretch--;

    PangoAttrList *attrlist = pango_attr_list_new();
    PangoAttribute *attr = pango_attr_stretch_new(stretch);
    pango_attr_list_insert(attrlist, attr);
    gtk_label_set_attributes(GTK_LABEL(widget), attrlist);
    pango_attr_list_unref(attrlist);
  }
}

static gboolean _sample_enter_callback(GtkWidget *widget, GdkEvent *event, dt_colorpicker_sample_t *sample)
{
  if(darktable.develop->color_picker.picker)
  {
    darktable.develop->color_picker.selected_sample = sample;
   	dt_control_queue_redraw_center();
  }

  return FALSE;
}

static gboolean _sample_leave_callback(GtkWidget *widget, GdkEvent *event, gpointer data)
{
  if(event->crossing.detail == GDK_NOTIFY_INFERIOR) return FALSE;

  if(darktable.develop->color_picker.selected_sample)
  {
    darktable.develop->color_picker.selected_sample = NULL;
   	dt_control_queue_redraw_center();
  }

  return FALSE;
}

static void _remove_sample(dt_colorpicker_sample_t *sample)
{
  gtk_widget_destroy(sample->container);
  darktable.develop->color_picker.samples
    = g_slist_remove(darktable.develop->color_picker.samples, (gpointer)sample);
  dt_free(sample);
}

static void _remove_sample_cb(GtkButton *widget, dt_colorpicker_sample_t *sample)
{
  _remove_sample(sample);
  dt_control_queue_redraw_center();
}

static gboolean _live_sample_button(GtkWidget *widget, GdkEventButton *event, dt_colorpicker_sample_t *sample)
{
  if(event->button == 1)
  {
    sample->locked = !sample->locked;
    gtk_widget_queue_draw(widget);
  }
  else if(event->button == 3)
  {
    // copy to active picker
    dt_lib_module_t *self = darktable.develop->color_picker.histogram_module;
    dt_iop_color_picker_t *picker = darktable.develop->color_picker.picker;

    // no active picker, too much iffy GTK work to activate a default
    if(IS_NULL_PTR(picker)) return FALSE;

    if(sample->size == DT_LIB_COLORPICKER_SIZE_POINT)
      _set_sample_point(self, sample->point);
    else if(sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
      _set_sample_box_area(self, sample->box);
    else
      return FALSE;

    dt_control_queue_redraw_center();
  }
  return FALSE;
}

static void _add_sample(GtkButton *widget, dt_lib_module_t *self)
{
  dt_lib_histogram_t *d = self->data;

  if(IS_NULL_PTR(darktable.develop->color_picker.picker))
    return;

  dt_colorpicker_sample_t *sample = (dt_colorpicker_sample_t *)malloc(sizeof(dt_colorpicker_sample_t));
  if(IS_NULL_PTR(sample)) return;

  memcpy(sample, darktable.develop->color_picker.primary_sample, sizeof(dt_colorpicker_sample_t));
  sample->locked = FALSE;
  g_strlcpy(sample->backbuf_op, d->op ? d->op : "", sizeof(sample->backbuf_op));

  sample->container = gtk_event_box_new();
  gtk_widget_add_events(sample->container, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect(G_OBJECT(sample->container), "enter-notify-event", G_CALLBACK(_sample_enter_callback), sample);
  g_signal_connect(G_OBJECT(sample->container), "leave-notify-event", G_CALLBACK(_sample_leave_callback), sample);

  GtkWidget *container = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_container_add(GTK_CONTAINER(sample->container), container);

  sample->color_patch = gtk_drawing_area_new();
  gtk_widget_add_events(sample->color_patch, GDK_BUTTON_PRESS_MASK);
  gtk_widget_set_tooltip_text(sample->color_patch, _("hover to highlight sample on canvas,\n"
                                                     "click to lock sample,\n"
                                                     "right-click to load sample area into active color picker"));
  g_signal_connect(G_OBJECT(sample->color_patch), "button-press-event", G_CALLBACK(_live_sample_button), sample);
  g_signal_connect(G_OBJECT(sample->color_patch), "draw", G_CALLBACK(_sample_draw_callback), sample);

  GtkWidget *color_patch_wrapper = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(color_patch_wrapper, "live-sample");
  gtk_box_pack_start(GTK_BOX(color_patch_wrapper), sample->color_patch, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(container), color_patch_wrapper, TRUE, TRUE, 0);

  sample->output_label = gtk_label_new("");
  dt_gui_add_class(sample->output_label, "dt_monospace");
  gtk_label_set_ellipsize(GTK_LABEL(sample->output_label), PANGO_ELLIPSIZE_START);
  gtk_label_set_selectable(GTK_LABEL(sample->output_label), TRUE);
  gtk_widget_set_has_tooltip(sample->output_label, TRUE);
  g_signal_connect(G_OBJECT(sample->output_label), "query-tooltip", G_CALLBACK(_sample_tooltip_callback), sample);
  g_signal_connect(G_OBJECT(sample->output_label), "size-allocate", G_CALLBACK(_label_size_allocate_callback), sample);
  gtk_box_pack_start(GTK_BOX(container), sample->output_label, TRUE, TRUE, 0);

  GtkWidget *delete_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_remove, 0, NULL);
  g_signal_connect(G_OBJECT(delete_button), "clicked", G_CALLBACK(_remove_sample_cb), sample);
  gtk_box_pack_start(GTK_BOX(container), delete_button, FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(d->samples_container), sample->container, FALSE, FALSE, 0);
  gtk_widget_show_all(sample->container);

  darktable.develop->color_picker.samples
      = g_slist_append(darktable.develop->color_picker.samples, sample);

  // remove emphasis on primary sample from mouseover on this button
  darktable.develop->color_picker.selected_sample = NULL;

  // Updating the display
  _update_everything(self);
}

static void _display_samples_changed(GtkToggleButton *button, dt_lib_module_t *self)
{
  dt_conf_set_bool("ui_last/colorpicker_display_samples", gtk_toggle_button_get_active(button));
  darktable.develop->color_picker.display_samples = gtk_toggle_button_get_active(button);
  _update_everything(self);
  dt_control_queue_redraw_center();
}

static void _restrict_histogram_changed(GtkToggleButton *button, dt_lib_module_t *self)
{
  dt_conf_set_bool("ui_last/colorpicker_restrict_histogram", gtk_toggle_button_get_active(button));
  darktable.develop->color_picker.restrict_histogram = gtk_toggle_button_get_active(button);
  _update_everything(self);
}

void gui_reset(dt_lib_module_t *self)
{
  dt_lib_histogram_t *d = self->data;
  dt_colorpicker_sample_t *const primary_sample = darktable.develop->color_picker.primary_sample;

  dt_iop_color_picker_reset(NULL, FALSE);

  // Resetting the picked colors
  for(int i = 0; i < 3; i++)
  {
    for(int s = 0; s < DT_LIB_COLORPICKER_STATISTIC_N; s++)
    {
      primary_sample->display[s][i] = 0.f;
      primary_sample->scope[s][i] = 0.f;
      primary_sample->lab[s][i] = 0.f;
    }
    primary_sample->label_rgb[i] = 0;
  }
  primary_sample->swatch.red = primary_sample->swatch.green
    = primary_sample->swatch.blue = 0.0;

  _update_picker_output(self);

  // Removing any live samples
  while(darktable.develop->color_picker.samples)
    _remove_sample(darktable.develop->color_picker.samples->data);

  // Resetting GUI elements
  dt_bauhaus_combobox_set(d->statistic_selector, 0);
  dt_bauhaus_combobox_set(d->color_mode_selector, 0);
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->display_samples_check_box)))
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->display_samples_check_box), FALSE);

  _reset_cache(d);
  _set_params(d);
  _destroy_surface(d);
  _trigger_recompute(d);

  dt_dev_pixelpipe_update_history_preview(darktable.develop);
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)dt_pixelpipe_cache_alloc_align_cache(sizeof(dt_lib_histogram_t), 0);
  if(d) memset(d, 0, sizeof(dt_lib_histogram_t));
  self->data = (void *)d;
  d->cst = NULL;
  d->pending_hashes = g_array_new(FALSE, FALSE, sizeof(uint64_t));
  d->refresh_idle_source = 0;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  d->scope_draw = dtgtk_drawing_area_new_with_aspect_ratio(1.);
  gtk_widget_add_events(GTK_WIDGET(d->scope_draw), darktable.gui->scroll_mask);
  gtk_widget_set_size_request(d->scope_draw, -1, DT_PIXEL_APPLY_DPI(250));
  g_signal_connect(G_OBJECT(d->scope_draw), "draw", G_CALLBACK(_draw_callback), d);
  g_signal_connect(G_OBJECT(d->scope_draw), "scroll-event", G_CALLBACK(_area_scrolled_callback), d);
  g_signal_connect(G_OBJECT(d->scope_draw), "size-allocate", G_CALLBACK(_resize_callback), d);
  gtk_box_pack_start(GTK_BOX(self->widget), d->scope_draw, TRUE, TRUE, 0);

  d->stage = dt_bauhaus_combobox_new(darktable.bauhaus, DT_GUI_MODULE(NULL));
  dt_bauhaus_widget_set_label(d->stage, _("Show data from"));
  dt_bauhaus_combobox_add(d->stage, _("Raw image"));
  dt_bauhaus_combobox_add(d->stage, _("Output color profile"));
  dt_bauhaus_combobox_add(d->stage, _("Final display"));
  g_signal_connect(G_OBJECT(d->stage), "value-changed", G_CALLBACK(_stage_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), d->stage, FALSE, FALSE, 0);

  d->display = dt_bauhaus_combobox_new(darktable.bauhaus, DT_GUI_MODULE(NULL));
  dt_bauhaus_widget_set_label(d->display, _("Display"));
  dt_bauhaus_combobox_add(d->display, _("Histogram"));
  dt_bauhaus_combobox_add(d->display, _("Waveform (horizontal)"));
  dt_bauhaus_combobox_add(d->display, _("Waveform (vertical)"));
  dt_bauhaus_combobox_add(d->display, _("Parade (horizontal)"));
  dt_bauhaus_combobox_add(d->display, _("Parade (vertical)"));
  dt_bauhaus_combobox_add(d->display, _("Vectorscope"));
  g_signal_connect(G_OBJECT(d->display), "value-changed", G_CALLBACK(_display_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), d->display, FALSE, FALSE, 0);

  // Adding the live samples section
  GtkWidget *label = dt_ui_section_label_new(_("Color picker"));
  gtk_box_pack_start(GTK_BOX(self->widget), label, TRUE, TRUE, 0);

  // The develop module owns the picker state because both the preview pipe and the GUI need to
  // observe it. Histogram only binds its widgets to that shared state.
  darktable.develop->color_picker.histogram_module = self;
  darktable.develop->color_picker.refresh_global_picker = _refresh_global_picker;
  darktable.develop->color_picker.display_samples = dt_conf_get_bool("ui_last/colorpicker_display_samples");
  darktable.develop->color_picker.restrict_histogram = dt_conf_get_bool("ui_last/colorpicker_restrict_histogram");
  darktable.develop->color_picker.primary_sample->swatch.alpha = 1.0;

  const char *str = dt_conf_get_string_const("ui_last/colorpicker_model");
  const char **names = dt_lib_colorpicker_model_names;
  for(dt_lib_colorpicker_model_t i=0; *names; names++, i++)
    if(g_strcmp0(str, *names) == 0)
      d->model = i;

  str = dt_conf_get_string_const("ui_last/colorpicker_mode");
  names = dt_lib_colorpicker_statistic_names;
  for(dt_lib_colorpicker_statistic_t i=0; *names; names++, i++)
    if(g_strcmp0(str, *names) == 0)
      d->statistic = i;

  // The color patch
  GtkWidget *color_patch_wrapper = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(GTK_WIDGET(color_patch_wrapper), "color-picker-area");

  // The picker button, mode and statistic combo boxes
  GtkWidget *picker_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  d->statistic_selector = dt_bauhaus_combobox_new_full(
      darktable.bauhaus, NULL, NULL, _("select which statistic to show"), d->statistic,
      (GtkCallback)_statistic_changed, self, dt_lib_colorpicker_statistic_names);
  dt_bauhaus_combobox_set_entries_ellipsis(d->statistic_selector, PANGO_ELLIPSIZE_NONE);
  dt_bauhaus_widget_set_label(d->statistic_selector, NULL);
  gtk_widget_set_valign(d->statistic_selector, GTK_ALIGN_CENTER);
  gtk_box_pack_start(GTK_BOX(picker_row), d->statistic_selector, TRUE, TRUE, 0);

  d->color_mode_selector = dt_bauhaus_combobox_new_full(
      darktable.bauhaus, NULL, NULL, _("select which color mode to use"), d->model,
      (GtkCallback)_color_mode_changed, self, dt_lib_colorpicker_model_names);
  dt_bauhaus_combobox_set_entries_ellipsis(d->color_mode_selector, PANGO_ELLIPSIZE_NONE);
  dt_bauhaus_widget_set_label(d->color_mode_selector, NULL);
  gtk_widget_set_valign(d->color_mode_selector, GTK_ALIGN_CENTER);
  gtk_box_pack_start(GTK_BOX(picker_row), d->color_mode_selector, TRUE, TRUE, 0);

  d->picker_button = dt_color_picker_new(NULL, DT_COLOR_PICKER_POINT_AREA, picker_row);
  gtk_widget_set_tooltip_text(d->picker_button, _("turn on color picker\nctrl+click or right-click to select an area"));
  gtk_widget_set_name(GTK_WIDGET(d->picker_button), "color-picker-button");
  g_signal_connect(G_OBJECT(d->picker_button), "toggled", G_CALLBACK(_picker_button_toggled), d);

  gtk_box_pack_start(GTK_BOX(self->widget), picker_row, TRUE, TRUE, 0);

  // The small sample, label and add button
  GtkWidget *sample_row_events = gtk_event_box_new();
  gtk_widget_add_events(sample_row_events, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect(G_OBJECT(sample_row_events), "enter-notify-event", G_CALLBACK(_sample_enter_callback),
                   darktable.develop->color_picker.primary_sample);
  g_signal_connect(G_OBJECT(sample_row_events), "leave-notify-event", G_CALLBACK(_sample_leave_callback),
                   darktable.develop->color_picker.primary_sample);
  gtk_box_pack_start(GTK_BOX(self->widget), sample_row_events, TRUE, TRUE, 0);

  GtkWidget *sample_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_container_add(GTK_CONTAINER(sample_row_events), sample_row);

  darktable.develop->color_picker.primary_sample->color_patch = gtk_drawing_area_new();
  g_signal_connect(G_OBJECT(darktable.develop->color_picker.primary_sample->color_patch), "draw",
                   G_CALLBACK(_sample_draw_callback), darktable.develop->color_picker.primary_sample);

  color_patch_wrapper = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(color_patch_wrapper, "live-sample");
  gtk_box_pack_start(GTK_BOX(color_patch_wrapper), darktable.develop->color_picker.primary_sample->color_patch, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(sample_row), color_patch_wrapper, TRUE, TRUE, 0);

  label = darktable.develop->color_picker.primary_sample->output_label = gtk_label_new("");
  gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_START);
  gtk_label_set_selectable(GTK_LABEL(label), TRUE);
  dt_gui_add_class(label, "dt_monospace");
  gtk_widget_set_has_tooltip(label, TRUE);
  g_signal_connect(G_OBJECT(label), "query-tooltip", G_CALLBACK(_sample_tooltip_callback),
                   darktable.develop->color_picker.primary_sample);
  g_signal_connect(G_OBJECT(label), "size-allocate", G_CALLBACK(_label_size_allocate_callback),
                   darktable.develop->color_picker.primary_sample);
  gtk_box_pack_start(GTK_BOX(sample_row), label, TRUE, TRUE, 0);

  d->add_sample_button = dtgtk_button_new(dtgtk_cairo_paint_square_plus, 0, NULL);
  gtk_widget_set_sensitive(GTK_WIDGET(d->add_sample_button), FALSE);
  g_signal_connect(G_OBJECT(d->add_sample_button), "clicked", G_CALLBACK(_add_sample), self);
  gtk_box_pack_end(GTK_BOX(sample_row), d->add_sample_button, FALSE, FALSE, 0);

  // Adding the live samples section
  label = dt_ui_section_label_new(_("Live samples"));
  gtk_box_pack_start(GTK_BOX(self->widget), label, TRUE, TRUE, 0);

  d->samples_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(self->widget),
                     dt_ui_scroll_wrap(d->samples_container, 1, "plugins/darkroom/colorpicker/windowheight"), TRUE, TRUE, 0);

  d->display_samples_check_box = gtk_check_button_new_with_label(_("Display samples on image"));
  gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(d->display_samples_check_box))),
                          PANGO_ELLIPSIZE_MIDDLE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->display_samples_check_box),
                               dt_conf_get_bool("ui_last/colorpicker_display_samples"));
  g_signal_connect(G_OBJECT(d->display_samples_check_box), "toggled",
                   G_CALLBACK(_display_samples_changed), self);
  gtk_box_pack_start(GTK_BOX(self->widget), d->display_samples_check_box, TRUE, TRUE, 0);

  GtkWidget *restrict_button = gtk_check_button_new_with_label(_("Restrict scope to selection"));
  gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(restrict_button))), PANGO_ELLIPSIZE_MIDDLE);
  gboolean restrict_histogram = dt_conf_get_bool("ui_last/colorpicker_restrict_histogram");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(restrict_button), restrict_histogram);
  g_signal_connect(G_OBJECT(restrict_button), "toggled", G_CALLBACK(_restrict_histogram_changed), self);
  gtk_box_pack_start(GTK_BOX(self->widget), restrict_button, TRUE, TRUE, 0);

  _reset_cache(d);
  _set_params(d);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_histogram_t *d = self->data;
  _clear_pending_preview_histograms();
  _clear_pending_hashes(d);
  if(d->refresh_idle_source != 0)
  {
    g_source_remove(d->refresh_idle_source);
    d->refresh_idle_source = 0;
  }
  if(d->pending_hashes) g_array_free(d->pending_hashes, TRUE);
  _destroy_surface(d);
  dt_iop_color_picker_reset(NULL, FALSE);

  darktable.develop->color_picker.histogram_module = NULL;
  darktable.develop->color_picker.refresh_global_picker = NULL;
  while(darktable.develop->color_picker.samples)
    _remove_sample(darktable.develop->color_picker.samples->data);

  dt_pixelpipe_cache_free_align(self->data);
  self->data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
