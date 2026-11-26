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

/**
 * @brief What a picker can be given on the image: a point, an area, or either.
 *
 * @details The picker's button only turns it on and off. Which of the two the user gets is
 * decided on the image, by the gesture: **a click places a point, a drag draws an area.** A
 * picker declares here which of those it can make sense of, and the gesture is clamped to it.
 *
 * The rule for choosing: a picker whose module reads `picked_color_min` or `picked_color_max`
 * is `DT_COLOR_PICKER_AREA`, since a point has no extent to take an extreme over -- its minimum,
 * its maximum and its mean are one pixel. A picker whose module only reads the mean can take
 * either, and the last two values differ only in what the picker shows the FIRST time it is
 * turned on, before the user has drawn anything.
 */
typedef enum _iop_color_picker_kind_t
{
  /** a point only: a click places it, a drag moves it */
  DT_COLOR_PICKER_POINT = 0,
  // FIXME: s/AREA/BOX/
  /** an area only: a click drops a small one, a drag draws it */
  DT_COLOR_PICKER_AREA,
  /** either, opening on a point */
  DT_COLOR_PICKER_POINT_AREA,
  /** either, opening on an area covering the image */
  DT_COLOR_PICKER_AREA_POINT
} dt_iop_color_picker_kind_t;

/** @brief Can a picker of this kind be given a point? */
gboolean dt_iop_color_picker_kind_takes_point(dt_iop_color_picker_kind_t kind);

/** @brief Can a picker of this kind be given an area? */
gboolean dt_iop_color_picker_kind_takes_area(dt_iop_color_picker_kind_t kind);

/**
 * @brief Where a button press on the image has got to.
 *
 * @details A press cannot tell a click from the start of a drag, so for a picker that takes
 * either it decides nothing: it is `PENDING` until the pointer travels (an area) or the button
 * comes up where it went down (a point). Sampling at the press instead would hand the module a
 * point it then has to take back -- and a module's sample is not free to take back, it is a
 * history item.
 */
typedef enum dt_iop_color_picker_pointer_t
{
  DT_COLOR_PICKER_POINTER_IDLE = 0,
  /** the button is down and has not moved: a point if it comes up here, an area if it travels */
  DT_COLOR_PICKER_POINTER_PENDING,
  /** the button is down and the point follows it */
  DT_COLOR_PICKER_POINTER_POINT,
  /** the button is down and stretches an area against a fixed corner */
  DT_COLOR_PICKER_POINTER_BOX
} dt_iop_color_picker_pointer_t;

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
  /** which of `pick_pos` and `pick_box` the picker last showed, and shows again when turned on */
  gboolean pick_is_box;
  /** not one of the pickers the picking modifier chooses between; see
   * dt_color_picker_exclude_from_gesture() */
  gboolean excluded_from_gesture;
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

/**
 * @name The pointer on the image, for the picker that is turned on
 *
 * @details One implementation of "a click places a point, a drag draws an area", for every view
 * that shows a picker over an image. A view converts its own widget coordinates to
 * IMAGE-normalised ones and forwards its three button events here; it keeps no picker state of
 * its own. (Each view used to carry a copy of this, with its own anchor and its own idea of
 * what a press meant.)
 *
 * All three are no-ops returning `DT_COLOR_PICKER_POINTER_IDLE` when no picker is turned on.
 * @{
 */

/**
 * @brief The left button went down on the image.
 *
 * @param image_point where, in image-normalised coordinates.
 * @param slop how far the pointer may travel, in image-normalised units along each axis, and
 * still count as not having moved. It is also the reach of an area's corner handles. A view
 * passes a few widget pixels converted to its own scale.
 *
 * @return the phase the gesture is now in. `DT_COLOR_PICKER_POINTER_BOX` means an area is
 * being stretched, which a view will want to show with its cursor.
 */
dt_iop_color_picker_pointer_t dt_iop_color_picker_pointer_pressed(const float image_point[2], const float slop[2]);

/** @brief The pointer moved with the button down. @return the phase, as above. */
dt_iop_color_picker_pointer_t dt_iop_color_picker_pointer_moved(const float image_point[2]);

/**
 * @brief The left button came up.
 *
 * @details A press that never travelled becomes a point here, where it went down. This is also
 * where a picker the modifier turned on is turned off again, if the modifier came up while the
 * button was still down.
 */
void dt_iop_color_picker_pointer_released(void);

/** @brief The phase of the gesture in progress, `DT_COLOR_PICKER_POINTER_IDLE` when none is. */
dt_iop_color_picker_pointer_t dt_iop_color_picker_pointer_phase(void);

/**
 * @brief The `slop` to pass with a press, for a caller working in the darkroom's widget space.
 *
 * @details 6 widget pixels along each axis, converted with the develop's own zoom to the
 * image-normalised units the picker works in. Slightly more than an area's corner handles as
 * drawn, to give room for an unsteady hand. A view with a geometry of its own (studio capture)
 * computes its own.
 */
void dt_iop_color_picker_darkroom_slop(dt_develop_t *dev, float slop[2]);

/** @} */

/**
 * @name The picking modifier: sample without turning a picker on by hand
 *
 * @details While the primary modifier is held, a click in the image samples for the one picker
 * there is no doubt about, and a drag samples an area for it if it takes one. Releasing the
 * modifier turns that picker off again, so the image goes back to whatever it was for.
 *
 * "No doubt" is decided from what is on screen (dt_iop_color_picker_gesture_has_target()):
 * - the focused module shows exactly one picker of its own: that one;
 * - it shows several: none, the modifier does nothing -- there is no telling which was meant;
 * - it shows none, or no module has the focus: the global picker, if its panel shows it.
 *
 * A picker that is not shown is never turned on this way: the user could neither see that it
 * is on nor turn it off.
 * @{
 */

/** @brief Is this key the picking modifier itself? (Ctrl; Cmd on macOS.) */
gboolean dt_iop_color_picker_is_gesture_key(guint keyval);

/** @brief Would a modifier-click in the image sample for something right now? */
gboolean dt_iop_color_picker_gesture_has_target(void);

/**
 * @brief Turn a picker on for the modifier, and take the press that did it.
 *
 * @param picker_widget the picker to use, for a module that knows which one it means and
 * handles the click itself; NULL to let the rule above choose.
 * @param image_point where the button went down, in image-normalised coordinates.
 * @param slop as dt_iop_color_picker_pointer_pressed().
 *
 * @return FALSE when nothing was turned on: no unambiguous picker, or widgets are suppressed.
 * The caller then leaves the click to whoever else wants it.
 *
 * @details It turns the picker on WITHOUT sampling what it remembered. A picker turned on by
 * its button shows its previous point or area at once and the module acts on it; here that
 * would act on an area the user is not looking at, an instant before the click they meant.
 *
 * The motion and the release that follow are the view's to forward, through the pointer calls
 * above -- the picker is on by then, and a view offers a picker the pointer before anything else.
 */
gboolean dt_iop_color_picker_gesture_begin(GtkWidget *picker_widget, const float image_point[2], const float slop[2]);

/**
 * @brief The modifier came up: turn off the picker it turned on.
 *
 * @details A picker the user turned on with its button is left alone. So is one whose sample
 * has not come back yet: a sample is taken from the pipeline's cache and may have to wait for a
 * recompute, and the modifier is routinely released before that. Turning the picker off at once
 * would throw the click away, so it is turned off when the sample lands instead.
 *
 * Safe to call when no gesture is in progress.
 */
void dt_iop_color_picker_gesture_end(void);

/** @brief Is the picker that is on one the modifier turned on? */
gboolean dt_iop_color_picker_gesture_is_active(void);

/**
 * @brief Was it begun by the focused module, naming its picker to
 * dt_iop_color_picker_gesture_begin(), rather than by the view?
 *
 * @details Such a module claims every click on the image and draws its own cursor in place of
 * the system one (drawlayer, its brush; while the modifier is held, its cross). The view goes
 * on calling its gui_post_expose() while the picker owns the image, so that cross stays the
 * cursor for the whole gesture instead of disappearing at the press.
 */
gboolean dt_iop_color_picker_gesture_is_by_module(void);

/** @} */

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
 * @brief Take a picker out of the ones the picking modifier chooses between.
 *
 * @details For a picker that samples for something other than its module's own controls. The
 * blending's two pickers are the case: they are created on behalf of every module that blends,
 * and counted as that module's they would make every such module ambiguous, exposure and color
 * calibration included. A module with a main picker and an incidental one can use it the same way.
 *
 * @param picker Widget returned by dt_color_picker_new() or dt_color_picker_new_with_cst().
 */
void dt_color_picker_exclude_from_gesture(GtkWidget *picker);

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
