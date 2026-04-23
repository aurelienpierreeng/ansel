/*
    This file is part of darktable,
    Copyright (C) 2018-2021 Pascal Obry.
    Copyright (C) 2019-2021 Diederik Ter Rahe.
    Copyright (C) 2019 Edgardo Hoszowski.
    Copyright (C) 2019 Ulrich Pegelow.
    Copyright (C) 2020 Harold le Clément de Saint-Marcq.
    Copyright (C) 2020 Hubert Kowalski.
    Copyright (C) 2020 Marco.
    Copyright (C) 2021 Dan Torop.
    Copyright (C) 2021 Paolo DePetrillo.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2022 Aldric Renaudin.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Philipp Lutz.
    Copyright (C) 2023 Alynx Zhou.
    Copyright (C) 2023, 2025-2026 Aurélien PIERRE.
    
    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    
    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.
    
    You should have received a copy of the GNU Lesser General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "gui/color_picker_proxy.h"
#include "bauhaus/bauhaus.h"
#include "common/color_picker.h"
#include "control/signal.h"
#include "control/control.h"
#include "develop/dev_pixelpipe.h"
#include "develop/pixelpipe_cache.h"
#include "gui/gtk.h"
#include "libs/colorpicker.h"
#include "libs/lib.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>

/*
  The color_picker_proxy code is an interface which links the UI
  colorpicker buttons in iops (and the colorpicker lib) with the rest
  of the implementation (selecting/drawing colorpicker area in center
  view, reading color value from preview pipe, and displaying results
  in the colorpicker lib).

  From the iop (or lib) POV, all that is necessary is to instantiate
  color picker(s) via dt_color_picker_new() or
  dt_color_picker_new_with_cst(), then subscribe to
  DT_SIGNAL_CONTROL_PICKERDATA_READY and resolve the current ready sample
  through dt_iop_color_picker_get_ready_data().

  This code will initialize new pickers with a default area, then
  remember the last area of the picker and use that when the picker is
  reactivated.

  The actual work of cache lookup and sampling happens here on the GUI
  thread. The drawing & mouse-sensitivity of the picker overlay in the
  center view happens in darkroom.c. The display of current sample
  values occurs via libs/colorpicker.c, which uses this code to
  activate its own picker.

  The sample position is potentially stored in two places:

  1. For each sampler widget, in dt_iop_color_picker_t.
  2. For the active iop, the primary, and the live samples in
     dt_colorpicker_sample_t.

  There will be at most one editable sample, with one active picker, at
  one time in the center view.
*/


// FIXME: should this be here or perhaps lib.c?
gboolean dt_iop_color_picker_is_visible(const dt_develop_t *dev)
{
  const gboolean module_picker = dev->gui_module
    && dev->gui_module->enabled
    && dev->color_picker.enabled
    && dev->color_picker.module == dev->gui_module;

  const gboolean primary_picker = dev && dev->color_picker.enabled && !dev->color_picker.module;

  return module_picker || primary_picker;
}

gboolean dt_iop_color_picker_is_active_module(const dt_iop_module_t *module)
{
  return module && module->dev
      && module->dev->color_picker.enabled
      && module->dev->color_picker.module == module;
}

/**
 * @brief Synchronize one picker cached geometry with the primary sample.
 *
 * @details
 * The active picker keeps a copy of the sample point/box so reopening it restores the previous
 * GUI geometry. This function only mirrors the current primary sample into that cached geometry
 * and reports whether the coordinates changed since the previous sampling pass.
 *
 * It does not consume @p self->update_pending. That flag is the same update logic seen from the
 * non-geometric side: first activation and colorspace changes request one callback even if the
 * picker geometry itself did not move.
 */
static gboolean _record_point_area(dt_iop_color_picker_t *self)
{
  dt_develop_t *const dev = darktable.develop;
  const dt_colorpicker_sample_t *const sample = dev ? dev->color_picker.primary_sample : NULL;
  gboolean changed = FALSE;
  if(self && sample)
  {
    if(sample->size == DT_LIB_COLORPICKER_SIZE_POINT)
      for(int k = 0; k < 2; k++)
      {
        if(self->pick_pos[k] != sample->point[k])
        {
          self->pick_pos[k] = sample->point[k];
          changed = TRUE;
        }
      }
    else if(sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
      for(int k = 0; k < 4; k++)
      {
        if (self->pick_box[k] != sample->box[k])
        {
          self->pick_box[k] = sample->box[k];
          changed = TRUE;
        }
      }
  }
  return changed;
}

typedef enum dt_pixelpipe_picker_source_t
{
  PIXELPIPE_PICKER_INPUT = 0,
  PIXELPIPE_PICKER_OUTPUT = 1
} dt_pixelpipe_picker_source_t;

typedef enum dt_color_picker_resample_status_t
{
  DT_COLOR_PICKER_RESAMPLE_CONSUMED = 0,
  DT_COLOR_PICKER_RESAMPLE_RETRY = 1,
  DT_COLOR_PICKER_RESAMPLE_EMITTED = 2
} dt_color_picker_resample_status_t;

static void _refresh_active_picker(dt_develop_t *dev);

static int _picker_sample_box(const dt_iop_module_t *module, const dt_iop_roi_t *roi,
                              const dt_pixelpipe_picker_source_t picker_source, int *box)
{
  dt_develop_t *const dev = darktable.develop;
  const dt_colorpicker_sample_t *const sample = dev ? dev->color_picker.primary_sample : NULL;
  if(IS_NULL_PTR(dev) || IS_NULL_PTR(module) || IS_NULL_PTR(roi) || IS_NULL_PTR(sample)) return 1;

  dt_boundingbox_t fbox = { 0.0f };

  if(sample->size == DT_LIB_COLORPICKER_SIZE_BOX)
  {
    memcpy(fbox, sample->box, sizeof(float) * 4);
    dt_dev_coordinates_image_norm_to_preview_abs(dev, fbox, 2);
  }
  else if(sample->size == DT_LIB_COLORPICKER_SIZE_POINT)
  {
    fbox[0] = sample->point[0];
    fbox[1] = sample->point[1];
    dt_dev_coordinates_image_norm_to_preview_abs(dev, fbox, 1);
    fbox[2] = fbox[0];
    fbox[3] = fbox[1];
  }

  dt_dev_distort_backtransform_plus(dev->preview_pipe, module->iop_order,
                                    picker_source == PIXELPIPE_PICKER_INPUT
                                      ? DT_DEV_TRANSFORM_DIR_FORW_INCL
                                      : DT_DEV_TRANSFORM_DIR_FORW_EXCL,
                                    fbox, 2);

  fbox[0] -= roi->x;
  fbox[1] -= roi->y;
  fbox[2] -= roi->x;
  fbox[3] -= roi->y;

  box[0] = fminf(fbox[0], fbox[2]);
  box[1] = fminf(fbox[1], fbox[3]);
  box[2] = fmaxf(fbox[0], fbox[2]);
  box[3] = fmaxf(fbox[1], fbox[3]);

  if(sample->size == DT_LIB_COLORPICKER_SIZE_POINT)
  {
    box[2] += 1;
    box[3] += 1;
  }

  if(box[0] >= roi->width || box[1] >= roi->height || box[2] < 0 || box[3] < 0) return 1;

  box[0] = MIN(roi->width - 1, MAX(0, box[0]));
  box[1] = MIN(roi->height - 1, MAX(0, box[1]));
  box[2] = MIN(roi->width - 1, MAX(0, box[2]));
  box[3] = MIN(roi->height - 1, MAX(0, box[3]));

  if(box[2] <= box[0] || box[3] <= box[1]) return 1;

  return 0;
}

static gboolean _sample_picker_buffer(dt_dev_pixelpipe_t *pipe, dt_iop_module_t *module,
                                      const dt_iop_buffer_dsc_t *dsc, const dt_iop_roi_t *roi,
                                      const float *pixel, dt_aligned_pixel_t avg_out,
                                      dt_aligned_pixel_t min_out, dt_aligned_pixel_t max_out,
                                      const dt_pixelpipe_picker_source_t picker_source)
{
  int box[4];
  if(_picker_sample_box(module, roi, picker_source, box)) return FALSE;

  dt_aligned_pixel_t avg = { 0.0f };
  dt_aligned_pixel_t min = { INFINITY, INFINITY, INFINITY, INFINITY };
  dt_aligned_pixel_t max = { -INFINITY, -INFINITY, -INFINITY, -INFINITY };

  const dt_iop_colorspace_type_t picker_cst = dt_iop_color_picker_get_active_cst(module);
  const dt_iop_order_iccprofile_info_t *const profile = dt_ioppr_get_pipe_current_profile_info(module, pipe);
  dt_color_picker_helper(dsc, pixel, roi, box, avg, min, max, dsc->cst, picker_cst, profile);

  for(int k = 0; k < 4; k++)
  {
    avg_out[k] = avg[k];
    min_out[k] = min[k];
    max_out[k] = max[k];
  }

  return TRUE;
}

int dt_iop_color_picker_get_ready_data(const dt_iop_module_t *module, GtkWidget **picker,
                                       dt_dev_pixelpipe_t **pipe,
                                       const dt_dev_pixelpipe_iop_t **piece)
{
  dt_develop_t *const dev = darktable.develop;
  if(IS_NULL_PTR(dev) || IS_NULL_PTR(module) || dev->color_picker.pending_module != module || IS_NULL_PTR(dev->color_picker.pending_pipe))
    return 1;

  dt_dev_pixelpipe_t *const current_pipe = dev->color_picker.pending_pipe;
  const dt_dev_pixelpipe_iop_t *current_piece = NULL;

  /* We walk the current preview graph and look for the current piece matching the sampled hash.
     Piece pointers cannot cross the signal boundary because the pipe may have been rebuilt. */
  for(GList *pieces = g_list_first(current_pipe->nodes); pieces; pieces = g_list_next(pieces))
  {
    const dt_dev_pixelpipe_iop_t *const current = pieces->data;
    if(current->module == module && current->global_hash == dev->color_picker.piece_hash)
    {
      current_piece = current;
      break;
    }
  }

  if(IS_NULL_PTR(current_piece))
  {
    dt_print(DT_DEBUG_DEV,
             "[picker] ready-data miss module=%s pending_hash=%" PRIu64 " pipe=%p\n",
             module->op, dev->color_picker.piece_hash, (void *)current_pipe);
    return 1;
  }

  if(picker) *picker = dev->color_picker.widget;
  if(pipe) *pipe = current_pipe;
  if(piece) *piece = current_piece;
  dt_print(DT_DEBUG_DEV,
           "[picker] ready-data module=%s hash=%" PRIu64 " pipe=%p picker=%p\n",
           module->op, current_piece->global_hash, (void *)current_pipe, (void *)dev->color_picker.widget);
  return 0;
}

static dt_color_picker_resample_status_t _sample_picker_from_cache(dt_develop_t *dev)
{
  if(IS_NULL_PTR(dev) || IS_NULL_PTR(dev->preview_pipe) || IS_NULL_PTR(dev->color_picker.picker) || !dev->color_picker.enabled
     || !dev->color_picker.module || !dev->gui_module || dev->color_picker.module != dev->gui_module
     || !dev->gui_module->enabled)
    return DT_COLOR_PICKER_RESAMPLE_CONSUMED;

  dt_dev_pixelpipe_t *const pipe = dev->preview_pipe;
  dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)dt_dev_pixelpipe_get_module_piece(pipe, dev->color_picker.module);
  const dt_dev_pixelpipe_iop_t *const previous_piece
      = dt_dev_pixelpipe_get_prev_enabled_piece(pipe, piece);
  if(IS_NULL_PTR(piece) || IS_NULL_PTR(previous_piece))
  {
    dt_print(DT_DEBUG_DEV, "[picker] sample retry module=%s piece=%p prev=%p\n",
             dev->color_picker.module ? dev->color_picker.module->op : "-", (void *)piece, (void *)previous_piece);
    return DT_COLOR_PICKER_RESAMPLE_RETRY;
  }

  if(piece->global_hash == DT_PIXELPIPE_CACHE_HASH_INVALID
     || previous_piece->global_hash == DT_PIXELPIPE_CACHE_HASH_INVALID)
  {
    dt_print(DT_DEBUG_DEV,
             "[picker] invalid hash module=%s piece=%" PRIu64 " prev=%" PRIu64 "\n",
             piece->module->op, piece->global_hash, previous_piece->global_hash);
    return DT_COLOR_PICKER_RESAMPLE_RETRY;
  }

  void *input = NULL;
  dt_pixel_cache_entry_t *input_entry = NULL;
  if(!dt_dev_pixelpipe_cache_peek_gui(pipe, previous_piece, &input, &input_entry, NULL, NULL, NULL))
  {
    dev->color_picker.wait_input_hash = previous_piece->global_hash;
    dt_print(DT_DEBUG_DEV, "[picker] input cache miss module=%s prev_hash=%" PRIu64 "\n",
             piece->module->op, previous_piece->global_hash);
    return DT_COLOR_PICKER_RESAMPLE_RETRY;
  }

  void *output = NULL;
  dt_pixel_cache_entry_t *output_entry = NULL;
  const gboolean have_output = dt_dev_pixelpipe_cache_peek_gui(pipe, piece, &output, &output_entry, NULL, NULL, NULL);
  if(!have_output)
  {
    /* Module GUIs such as color equalizer only consume the module-input sample. A missing output
       cacheline must therefore not block picker feedback forever, otherwise one unavailable host
       cache entry feeds an endless recompute/retry loop. Keep output statistics explicitly invalid
       so output-dependent consumers can detect the missing sample, but still publish the ready
       input sample. */
    dt_print(DT_DEBUG_DEV, "[picker] output cache miss module=%s hash=%" PRIu64 "\n",
             piece->module->op, piece->global_hash);
    dev->color_picker.wait_output_hash = piece->global_hash;

    for(int k = 0; k < 4; k++)
    {
      piece->module->picked_output_color[k] = 0.0f;
      piece->module->picked_output_color_min[k] = 666.0f;
      piece->module->picked_output_color_max[k] = -666.0f;
    }
  }

  if(previous_piece->dsc_out.datatype != TYPE_FLOAT || (have_output && piece->dsc_out.datatype != TYPE_FLOAT))
  {
    dt_print(DT_DEBUG_DEV,
             "[picker] non-float buffers module=%s input_type=%d output_type=%d\n",
             piece->module->op, previous_piece->dsc_out.datatype, piece->dsc_out.datatype);
    return DT_COLOR_PICKER_RESAMPLE_CONSUMED;
  }

  /* Unlike histogram/global backbuffers, module color-pickers do not publish a dedicated long-lived buffer.
   * They reopen the current module input/output cachelines by immutable `global_hash`, then take a temporary
   * ref plus read lock only for the duration of the sampling pass so concurrent cache recycling cannot free
   * the payload mid-read. */
  dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, TRUE, input_entry);
  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, TRUE, input_entry);
  if(have_output)
  {
    dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, TRUE, output_entry);
    dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, TRUE, output_entry);
  }

  const gboolean sampled_input
      = _sample_picker_buffer(pipe, piece->module, &previous_piece->dsc_out, &previous_piece->roi_out,
                              input, piece->module->picked_color, piece->module->picked_color_min,
                              piece->module->picked_color_max, PIXELPIPE_PICKER_INPUT);
  const gboolean sampled_output
      = have_output
          ? _sample_picker_buffer(pipe, piece->module, &piece->dsc_out, &piece->roi_out, output,
                                  piece->module->picked_output_color, piece->module->picked_output_color_min,
                                  piece->module->picked_output_color_max, PIXELPIPE_PICKER_OUTPUT)
          : FALSE;

  if(have_output)
  {
    dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, FALSE, output_entry);
    dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, FALSE, output_entry);
  }
  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, FALSE, input_entry);
  dt_dev_pixelpipe_cache_ref_count_entry(darktable.pixelpipe_cache, FALSE, input_entry);

  if(!sampled_input)
  {
    dt_print(DT_DEBUG_DEV, "[picker] sample failed module=%s input=%d output=%d\n",
             piece->module->op, sampled_input, sampled_output);
    dev->color_picker.picker->update_pending = FALSE;
    dev->color_picker.update_pending = FALSE;
    return DT_COLOR_PICKER_RESAMPLE_CONSUMED;
  }

  dt_print(DT_DEBUG_DEV,
           "[picker] sampled module=%s hash=%" PRIu64 " avg=(%g,%g,%g) min=(%g,%g,%g) max=(%g,%g,%g)\n",
           piece->module->op, piece->global_hash,
           piece->module->picked_color[0], piece->module->picked_color[1], piece->module->picked_color[2],
           piece->module->picked_color_min[0], piece->module->picked_color_min[1], piece->module->picked_color_min[2],
           piece->module->picked_color_max[0], piece->module->picked_color_max[1], piece->module->picked_color_max[2]);

  dev->color_picker.piece_hash = piece->global_hash;
  dev->color_picker.pending_module = piece->module;
  dev->color_picker.pending_pipe = pipe;
  dev->color_picker.wait_input_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
  dev->color_picker.wait_output_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
  dev->color_picker.picker->update_pending = FALSE;
  dev->color_picker.update_pending = FALSE;

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_CONTROL_PICKERDATA_READY);
  dev->color_picker.pending_module = NULL;
  dev->color_picker.pending_pipe = NULL;
  dev->color_picker.piece_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
  return DT_COLOR_PICKER_RESAMPLE_EMITTED;
}

static gboolean _refresh_active_picker_idle(gpointer user_data)
{
  dt_develop_t *const dev = (dt_develop_t *)user_data;
  if(dev) dev->color_picker.refresh_idle_source = 0;
  _refresh_active_picker(dev);
  return G_SOURCE_REMOVE;
}

static void _queue_refresh_active_picker(dt_develop_t *dev)
{
  if(IS_NULL_PTR(dev)) return;
  if(dev->color_picker.refresh_idle_source) return;
  dev->color_picker.refresh_idle_source = g_idle_add(_refresh_active_picker_idle, dev);
}

static void _refresh_active_picker(dt_develop_t *dev)
{
  if(IS_NULL_PTR(dev) || IS_NULL_PTR(dev->color_picker.picker) || !dev->color_picker.enabled) return;
  if(!dev->color_picker.picker->update_pending && !dev->color_picker.update_pending) return;

  dt_print(DT_DEBUG_DEV,
           "[picker] refresh module=%s update_pending=%d widget_pending=%d processing=%d\n",
           dev->color_picker.module ? dev->color_picker.module->op : "-",
           dev->color_picker.update_pending, dev->color_picker.picker->update_pending,
           dev->preview_pipe ? dev->preview_pipe->processing : -1);

  _record_point_area(dev->color_picker.picker);

  /* A picker update is satisfied either from the current preview cache or from the next completed
     preview run. If preview is already processing, re-dirtying it here only feeds TOP_CHANGED
     loops and prevents the current run from ever publishing the cacheline we are waiting for. */
  if(dev->preview_pipe && dev->preview_pipe->processing) return;

  if(IS_NULL_PTR(dev->color_picker.module))
  {
    /* The global picker already samples from histogram backbuffers published by preview updates.
       Refresh it directly from that GUI-owned cache when possible so dragging the picker updates
       the histogram labels immediately without waiting for another preview completion. */
    if(dev->color_picker.histogram_module
       && dev->color_picker.refresh_global_picker
       && dev->color_picker.refresh_global_picker(dev->color_picker.histogram_module))
    {
      dev->color_picker.picker->update_pending = FALSE;
      dev->color_picker.update_pending = FALSE;
      dt_control_queue_redraw_center();
      return;
    }
  }

  if(dev->preview_pipe)
  {
    const dt_color_picker_resample_status_t sampled = _sample_picker_from_cache(dev);
    if(sampled != DT_COLOR_PICKER_RESAMPLE_RETRY)
      return;
  }
}

static void _color_picker_reset(dt_iop_color_picker_t *picker)
{
  if(picker)
  {
    if(picker->module) dt_iop_set_cache_bypass(picker->module, FALSE);

    ++darktable.gui->reset;

    if(DTGTK_IS_TOGGLEBUTTON(picker->colorpick))
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(picker->colorpick), FALSE);
    else
      dt_bauhaus_widget_set_quad_active(picker->colorpick, FALSE);

    --darktable.gui->reset;
  }
}

static void _color_picker_widget_destroy(GtkWidget *widget, dt_iop_color_picker_t *picker)
{
  (void)widget;
  dt_develop_t *const dev = darktable.develop;
  if(IS_NULL_PTR(dev) || dev->color_picker.picker != picker) return;

  dev->color_picker.picker = NULL;
  dev->color_picker.widget = NULL;
}

void dt_iop_color_picker_reset(dt_iop_module_t *module, gboolean keep)
{
  dt_develop_t *const dev = darktable.develop;
  dt_iop_color_picker_t *picker = dev ? dev->color_picker.picker : NULL;
  if(picker && picker->module == module)
  {
    if(!keep)
    {
      _color_picker_reset(picker);
      dev->color_picker.picker = NULL;
      dev->color_picker.widget = NULL;
      dev->color_picker.module = NULL;
      dev->color_picker.kind = DT_COLOR_PICKER_POINT;
      dev->color_picker.picker_cst = IOP_CS_NONE;
      dev->color_picker.enabled = FALSE;
      dev->color_picker.update_pending = FALSE;
      if(module) module->request_color_pick = DT_REQUEST_COLORPICK_OFF;
    }
  }
}

static void _init_picker(dt_iop_color_picker_t *picker, dt_iop_module_t *module,
                         dt_iop_color_picker_kind_t kind, GtkWidget *button)
{
  // module is NULL if primary colorpicker
  picker->module     = module;
  picker->kind       = kind;
  picker->picker_cst = module ? module->default_colorspace(module, NULL, NULL) : IOP_CS_NONE;
  picker->colorpick  = button;
  picker->update_pending = FALSE;

  // default values
  const float middle = 0.5f;
  const float area = 0.99f;
  picker->pick_pos[0] = picker->pick_pos[1] = middle;
  picker->pick_box[0] = picker->pick_box[1] = 1.0f - area;
  picker->pick_box[2] = picker->pick_box[3] = area;

  _color_picker_reset(picker);
}

static gboolean _color_picker_callback_button_press(GtkWidget *button, GdkEventButton *e, dt_iop_color_picker_t *self)
{
  // module is NULL if primary colorpicker
  dt_iop_module_t *module = self->module;
  dt_develop_t *const dev = darktable.develop;

  if(darktable.gui->reset) return FALSE;

  dt_iop_color_picker_t *prior_picker = dev ? dev->color_picker.picker : NULL;
  if(prior_picker && prior_picker != self)
  {
    _color_picker_reset(prior_picker);
    if(prior_picker->module) prior_picker->module->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  }

  if(module && module->off)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->off), TRUE);

  const GdkModifierType state = !IS_NULL_PTR(e) ? e->state : dt_key_modifier_state();
  const gboolean ctrl_key_pressed = dt_modifier_is(state, GDK_CONTROL_MASK) || (!IS_NULL_PTR(e) && e->button == 3);
  dt_iop_color_picker_kind_t kind = self->kind;

  if (prior_picker != self || (kind == DT_COLOR_PICKER_POINT_AREA &&
      (ctrl_key_pressed ^ (dev->color_picker.primary_sample->size == DT_LIB_COLORPICKER_SIZE_BOX))))
  {
    dev->color_picker.picker = self;
    dev->color_picker.widget = self->colorpick;
    dev->color_picker.module = module;
    dev->color_picker.kind = kind;
    dev->color_picker.picker_cst = self->picker_cst;
    dev->color_picker.enabled = TRUE;

    if(module) module->request_color_pick = DT_REQUEST_COLORPICK_MODULE;

    if(kind == DT_COLOR_PICKER_POINT_AREA)
    {
      kind = ctrl_key_pressed ? DT_COLOR_PICKER_AREA : DT_COLOR_PICKER_POINT;
    }
    // pull picker's last recorded positions
    if(kind == DT_COLOR_PICKER_AREA)
      dt_lib_colorpicker_set_box_area(darktable.lib, self->pick_box);
    else if(kind == DT_COLOR_PICKER_POINT)
      dt_lib_colorpicker_set_point(darktable.lib, self->pick_pos);
    else
      dt_unreachable_codepath();

    // important to have set up state before toggling button and
    // triggering more callbacks
    ++darktable.gui->reset;
    if(DTGTK_IS_TOGGLEBUTTON(self->colorpick))
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->colorpick), TRUE);
    else
      dt_bauhaus_widget_set_quad_active(self->colorpick, TRUE);
    --darktable.gui->reset;

    if(module)
      dt_iop_request_focus(module);

    dt_print(DT_DEBUG_DEV, "[picker] activate module=%s picker=%p widget=%p kind=%d cst=%d\n",
             module ? module->op : "global", (void *)self, (void *)self->colorpick, kind, self->picker_cst);

  }
  else
  {
    dev->color_picker.picker = NULL;
    dev->color_picker.widget = NULL;
    dev->color_picker.module = NULL;
    dev->color_picker.kind = DT_COLOR_PICKER_POINT;
    dev->color_picker.picker_cst = IOP_CS_NONE;
    dev->color_picker.enabled = FALSE;
    dev->color_picker.update_pending = FALSE;
    _color_picker_reset(self);
    if(module)
    {
      module->request_color_pick = DT_REQUEST_COLORPICK_OFF;
    }
    dt_print(DT_DEBUG_DEV, "[picker] deactivate module=%s picker=%p widget=%p\n",
             module ? module->op : "global", (void *)self, (void *)self->colorpick);
  }

  if(dev->color_picker.enabled)
    dt_iop_color_picker_request_update();
  else
    dt_control_queue_redraw_center();

  return TRUE;
}

static void _color_picker_callback(GtkWidget *button, dt_iop_color_picker_t *self)
{
  _color_picker_callback_button_press(button, NULL, self);
}

void dt_iop_color_picker_set_cst(dt_iop_module_t *module, const dt_iop_colorspace_type_t picker_cst)
{
  dt_develop_t *const dev = darktable.develop;
  dt_iop_color_picker_t *const picker = dev ? dev->color_picker.picker : NULL;
  if(picker && picker->module == module && picker->picker_cst != picker_cst)
  {
    picker->picker_cst = picker_cst;
    dt_iop_color_picker_request_update();
  }
}

dt_iop_colorspace_type_t dt_iop_color_picker_get_active_cst(dt_iop_module_t *module)
{
  dt_develop_t *const dev = darktable.develop;
  dt_iop_color_picker_t *picker = dev ? dev->color_picker.picker : NULL;
  if(picker && picker->module == module)
    return picker->picker_cst;
  else
    return IOP_CS_NONE;
}

void dt_iop_color_picker_request_update(void)
{
  dt_develop_t *const dev = darktable.develop;
  dt_iop_color_picker_t *picker = dev ? dev->color_picker.picker : NULL;
  if(picker) picker->update_pending = TRUE;
  if(dev)
  {
    dev->color_picker.update_pending = TRUE;
  }
  if(dev)
    dt_print(DT_DEBUG_DEV, "[picker] request update module=%s picker=%p widget=%p\n",
             dev->color_picker.module ? dev->color_picker.module->op : "-",
             (void *)dev->color_picker.picker, (void *)dev->color_picker.widget);
  _queue_refresh_active_picker(dev);
}

gboolean dt_iop_color_picker_force_cache(const dt_dev_pixelpipe_t *pipe,
                                         const dt_iop_module_t *module)
{
  const dt_dev_pixelpipe_iop_t *const piece = dt_dev_pixelpipe_get_module_piece((dt_dev_pixelpipe_t *)pipe,
                                                                                 pipe->dev->color_picker.module);
  const dt_dev_pixelpipe_iop_t *const previous_piece = dt_dev_pixelpipe_get_prev_enabled_piece(pipe, piece);

  return module == pipe->dev->color_picker.module || (previous_piece && previous_piece->module == module);
}

static void _track_active_picker_hashes(dt_develop_t *dev)
{
  if(IS_NULL_PTR(dev))
    return;

  dev->color_picker.wait_input_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
  dev->color_picker.wait_output_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;

  if(IS_NULL_PTR(dev->preview_pipe) || IS_NULL_PTR(dev->color_picker.picker) || !dev->color_picker.enabled
     || !dev->color_picker.module || !dev->gui_module || dev->color_picker.module != dev->gui_module
     || !dev->gui_module->enabled)
    return;

  const dt_dev_pixelpipe_iop_t *const piece = dt_dev_pixelpipe_get_module_piece(dev->preview_pipe,
                                                                                 dev->color_picker.module);
  const dt_dev_pixelpipe_iop_t *const previous_piece
      = dt_dev_pixelpipe_get_prev_enabled_piece(dev->preview_pipe, piece);
  if(piece) dev->color_picker.wait_output_hash = piece->global_hash;
  if(previous_piece) dev->color_picker.wait_input_hash = previous_piece->global_hash;
}

static void _iop_color_picker_history_resync_callback(gpointer instance, gpointer user_data)
{
  (void)instance;
  (void)user_data;
  dt_develop_t *const dev = darktable.develop;
  _track_active_picker_hashes(dev);
  _queue_refresh_active_picker(dev);
}

static void _iop_color_picker_cacheline_ready_callback(gpointer instance, const guint64 hash, gpointer user_data)
{
  (void)instance;
  (void)user_data;

  dt_develop_t *const dev = darktable.develop;
  if(IS_NULL_PTR(dev)) return;

  gboolean matched = FALSE;
  if(dev->color_picker.wait_input_hash == hash)
  {
    dev->color_picker.wait_input_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
    matched = TRUE;
  }
  if(dev->color_picker.wait_output_hash == hash)
  {
    dev->color_picker.wait_output_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
    matched = TRUE;
  }

  if(matched) _queue_refresh_active_picker(dev);
}

void dt_iop_color_picker_init(void)
{
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_HISTORY_RESYNC,
                                  G_CALLBACK(_iop_color_picker_history_resync_callback), NULL);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_CACHELINE_READY,
                                  G_CALLBACK(_iop_color_picker_cacheline_ready_callback), NULL);
}

void dt_iop_color_picker_cleanup(void)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals,
                                     G_CALLBACK(_iop_color_picker_history_resync_callback), NULL);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals,
                                     G_CALLBACK(_iop_color_picker_cacheline_ready_callback), NULL);
}

static GtkWidget *_color_picker_new(dt_iop_module_t *module, dt_iop_color_picker_kind_t kind, GtkWidget *w,
                                    const gboolean init_cst, const dt_iop_colorspace_type_t cst)
{
  dt_iop_color_picker_t *color_picker = (dt_iop_color_picker_t *)g_malloc(sizeof(dt_iop_color_picker_t));

  if(IS_NULL_PTR(w) || GTK_IS_BOX(w))
  {
    GtkWidget *button = dtgtk_togglebutton_new(dtgtk_cairo_paint_colorpicker, 0, NULL);
    dt_gui_add_class(button, "dt_transparent_background");
    _init_picker(color_picker, module, kind, button);
    if(init_cst)
      color_picker->picker_cst = cst;
    g_signal_connect_data(G_OBJECT(button), "button-press-event",
                          G_CALLBACK(_color_picker_callback_button_press), color_picker, (GClosureNotify)g_free, 0);
    g_signal_connect(G_OBJECT(button), "destroy", G_CALLBACK(_color_picker_widget_destroy), color_picker);
    if (w) gtk_box_pack_start(GTK_BOX(w), button, FALSE, FALSE, 0);

    dt_develop_t *const dev = darktable.develop;
    if(dev && dev->color_picker.enabled && dev->color_picker.module == module
       && IS_NULL_PTR(dev->color_picker.widget)
       && dev->color_picker.kind == kind
       && dev->color_picker.picker_cst == color_picker->picker_cst)
    {
      dev->color_picker.picker = color_picker;
      dev->color_picker.widget = button;

      ++darktable.gui->reset;
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), TRUE);
      --darktable.gui->reset;
    }

    return button;
  }
  else
  {
    dt_bauhaus_widget_set_quad_paint(w, dtgtk_cairo_paint_colorpicker, 0, NULL);
    dt_bauhaus_widget_set_quad_toggle(w, TRUE);
    _init_picker(color_picker, module, kind, w);
    if(init_cst)
      color_picker->picker_cst = cst;
    g_signal_connect_data(G_OBJECT(w), "quad-pressed",
                          G_CALLBACK(_color_picker_callback), color_picker, (GClosureNotify)g_free, 0);
    g_signal_connect(G_OBJECT(w), "destroy", G_CALLBACK(_color_picker_widget_destroy), color_picker);

    dt_develop_t *const dev = darktable.develop;
    if(dev && dev->color_picker.enabled && dev->color_picker.module == module
       && IS_NULL_PTR(dev->color_picker.widget)
       && dev->color_picker.kind == kind
       && dev->color_picker.picker_cst == color_picker->picker_cst)
    {
      dev->color_picker.picker = color_picker;
      dev->color_picker.widget = w;

      ++darktable.gui->reset;
      dt_bauhaus_widget_set_quad_active(w, TRUE);
      --darktable.gui->reset;
    }

    return w;
  }
}

GtkWidget *dt_color_picker_new(dt_iop_module_t *module, dt_iop_color_picker_kind_t kind, GtkWidget *w)
{
  return _color_picker_new(module, kind, w, FALSE, IOP_CS_NONE);
}

GtkWidget *dt_color_picker_new_with_cst(dt_iop_module_t *module, dt_iop_color_picker_kind_t kind, GtkWidget *w,
                                        const dt_iop_colorspace_type_t cst)
{
  return _color_picker_new(module, kind, w, TRUE, cst);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
