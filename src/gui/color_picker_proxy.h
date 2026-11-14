/*
    This file is part of darktable,
    Copyright (C) 2018-2020 Pascal Obry.
    Copyright (C) 2019 Edgardo Hoszowski.
    Copyright (C) 2019 Ulrich Pegelow.
    Copyright (C) 2020 Diederik Ter Rahe.
    Copyright (C) 2020 Harold le Clément de Saint-Marcq.
    Copyright (C) 2021 Dan Torop.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Philipp Lutz.
    Copyright (C) 2026 Aurélien PIERRE.
    Copyright (C) 2026 Guillaume Stutin.
    
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
#ifndef DT_GUI_COLOR_PICKER_PROXY_H
#define DT_GUI_COLOR_PICKER_PROXY_H

/*
  This API encapsulates color-picker state ownership, cache sampling, and
  `DT_SIGNAL_CONTROL_PICKERDATA_READY` publication for darkroom.

  Module GUIs instantiate picker widgets here, then consume ready samples from
  the data-ready signal by querying the current picker state with
  `dt_iop_color_picker_get_ready_data()`.
*/

#include <gtk/gtk.h>
#include "develop/imageop.h"
#include "develop/develop.h"

typedef enum _iop_color_picker_kind_t
{
  DT_COLOR_PICKER_POINT = 0,
  // FIXME: s/AREA/BOX/
  DT_COLOR_PICKER_AREA,
  DT_COLOR_PICKER_POINT_AREA // allow the user to select between point and area
} dt_iop_color_picker_kind_t;

/** Which per-channel extremes of an area a module reads, so the darkroom marks where they lie. */
typedef enum dt_iop_color_picker_extremes_t
{
  DT_COLOR_PICKER_EXTREMES_NONE = 0,
  DT_COLOR_PICKER_EXTREMES_MIN = 1 << 0,
  DT_COLOR_PICKER_EXTREMES_MAX = 1 << 1,
} dt_iop_color_picker_extremes_t;

typedef struct dt_iop_color_picker_t
{
  // iop which contains this picker, or NULL if primary colorpicker
  dt_iop_module_t *module;
  dt_iop_color_picker_kind_t kind;
  /** requested colorspace for the color picker, valid options are:
   * IOP_CS_NONE: module colorspace
   * IOP_CS_LCH: for Lab modules
   * IOP_CS_HSL: for RGB modules
   */
  dt_iop_colorspace_type_t picker_cst;
  /** used to avoid recursion when a parameter is modified in the apply() */
  GtkWidget *colorpick;
  // positions are associated with the current picker widget: will set
  // the picker request for the primary picker when this picker is
  // activated, and will remember the most recent picker position
  float pick_pos[2];
  dt_boundingbox_t pick_box;
  gboolean geometry_is_raw;
  /** the extremes the module reads from this picker's input sample */
  dt_iop_color_picker_extremes_t extremes;
  /** One-shot request for the next signal emission after activation or colorspace change. */
  gboolean update_pending;
} dt_iop_color_picker_t;


gboolean dt_iop_color_picker_is_visible(const dt_develop_t *dev);

/**
 * @brief Tell whether one module currently owns the active darkroom picker.
 *
 * @details
 * Module code often needs to know whether a GUI callback is running for the module that currently
 * captures picker updates. That ownership lives under `dt_develop_t::color_picker`, so callers
 * should query it through this API instead of open-coding the develop-state test at each call site.
 *
 * @param module Candidate module.
 *
 * @return TRUE when the picker manager is enabled and the active picker belongs to @p module.
 */
gboolean dt_iop_color_picker_is_active_module(const dt_iop_module_t *module);

/* reset current color picker unless keep is TRUE */
void dt_iop_color_picker_reset(dt_iop_module_t *module, gboolean keep);

/* sets the picker colorspace */
void dt_iop_color_picker_set_cst(dt_iop_module_t *module, const dt_iop_colorspace_type_t picker_cst);

/* returns the active picker colorspace (if any) */
dt_iop_colorspace_type_t dt_iop_color_picker_get_active_cst(dt_iop_module_t *module);

/* mark that the active picker geometry or selection changed and resample from cache if possible */
void dt_iop_color_picker_request_update(void);

/**
 * @brief Resolve the current ready picker payload for one module.
 *
 * @details
 * `DT_SIGNAL_CONTROL_PICKERDATA_READY` does not carry the sampled module, pipe,
 * picker widget, or piece as signal parameters anymore. The picker manager stores
 * that ready state under `dt_develop_t` until signal subscribers consume it on
 * the GUI thread. This accessor lets any subscriber check whether the ready sample
 * belongs to @p module and, if so, recover the current live piece from the current
 * preview pipe graph through its immutable `global_hash`.
 *
 * @param module Signal subscriber asking whether the current ready sample is for it.
 * @param picker Optional output pointer receiving the active picker widget.
 * @param pipe Optional output pointer receiving the preview pipe used for sampling.
 * @param piece Optional output pointer receiving the current live piece matching the
 *        sampled `global_hash`.
 *
 * @return `0` when the ready sample belongs to @p module and all requested outputs
 *         were resolved, `1` otherwise.
 */
int dt_iop_color_picker_get_ready_data(const dt_iop_module_t *module, GtkWidget **picker,
                                       struct dt_dev_pixelpipe_t **pipe,
                                       const struct dt_dev_pixelpipe_iop_t **piece);

/* global init: link signal */
void dt_iop_color_picker_init();

/* global cleanup */
void dt_iop_color_picker_cleanup();

/* link color picker to widget */
GtkWidget *dt_color_picker_new(dt_iop_module_t *module, dt_iop_color_picker_kind_t kind, GtkWidget *w);

/* link color picker to widget and initialize color picker color space with given value */
GtkWidget *dt_color_picker_new_with_cst(dt_iop_module_t *module, dt_iop_color_picker_kind_t kind, GtkWidget *w,
                                        const dt_iop_colorspace_type_t cst);

/**
 * @brief Declare that a module reads the minimum or the maximum of each channel from this picker.
 *
 * @details
 * The picker then locates, on each input sample, the module-input pixel holding each RGB channel's
 * extreme over the picked area, in the buffer the statistics were taken from, so that pixel holds
 * the value the module received. The darkroom marks it with a cross in the channel's colour while
 * `dt_develop_t::color_picker.display_extremes` is set.
 *
 * @param picker Widget returned by dt_color_picker_new() or dt_color_picker_new_with_cst().
 * @param extremes Extremes to mark.
 */
void dt_color_picker_set_extremes(GtkWidget *picker, dt_iop_color_picker_extremes_t extremes);

/**
 * @brief Read all the channels of the pixel holding one channel's extreme over the current sample.
 *
 * @details
 * The pixel is the one the darkroom marks for that channel, in the module input the statistics were
 * taken from, so its @p channel equals `picked_color_min` or `picked_color_max` there.
 *
 * @param module Module owning the active picker.
 * @param which DT_COLOR_PICKER_EXTREMES_MIN or DT_COLOR_PICKER_EXTREMES_MAX, which the picker must declare.
 * @param channel 0, 1 or 2.
 * @param pixel Receives the pixel's four channels.
 *
 * @return 0 when the pixel was located on the current sample, 1 otherwise.
 */
int dt_iop_color_picker_get_extreme_pixel(const dt_iop_module_t *module, dt_iop_color_picker_extremes_t which,
                                          int channel, dt_aligned_pixel_t pixel);

/**
 * @brief Name the channel whose extreme decided the value a module derived from the current sample.
 *
 * @details
 * A module that reduces the three per-channel extremes to one value keeps one channel. The darkroom
 * then draws that channel's mark as a +, and the other two as smaller, dimmed crosses. A new sample
 * forgets the channel until the module names it again.
 *
 * @param module Module owning the active picker; ignored otherwise.
 * @param channel 0, 1 or 2; -1 for none.
 */
void dt_iop_color_picker_set_deciding_channel(const dt_iop_module_t *module, int channel);

#endif // DT_GUI_COLOR_PICKER_PROXY_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
