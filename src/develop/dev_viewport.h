/*
    This file is part of Ansel.
    Copyright (C) 2026 Aurélien Pierre.

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

#ifndef DT_DEVELOP_DEV_VIEWPORT_H
#define DT_DEVELOP_DEV_VIEWPORT_H

#include <glib.h>
#include <stdint.h>

#include "system/atomic.h"

struct dt_develop_t;

/**
 * @brief What the darkroom view asks the pipeline to show: the window onto the image.
 *
 * @details Meaningless without a GUI, and therefore absent without one -- dt_develop_t owns a
 * pointer that is allocated only for a gui_attached dev and NULL otherwise. That NULL IS the
 * flag: it replaces roi.gui_inited, and for this half of the object it replaces
 * dev->gui_attached too.
 *
 * Two devs can hold one at the same time (the darkroom's and studio capture's), and studio
 * capture swaps which one is global. Nothing here resolves dt_dev_get_global() internally;
 * every entry point takes the dev whose viewport it means.
 */
typedef struct dt_dev_viewport_state_t
{
  /** Darkroom main widget size in GUI logical coordinates, as allocated by Gtk from the window
   *  minus all panels. NOT the size of the backbuffer. (was roi.orig_width/orig_height) */
  int32_t widget_width, widget_height;

  /** ISO 12646 borders, or user-defined borders, in GUI logical pixels. */
  int32_t border_size;

  /** (widget size - 2 * border_size) converted to raster pixels through the GUI ppd factor:
   *  the surface an image backbuffer actually covers. Derived here, never set from outside.
   *  (was roi.width/roi.height) */
  int32_t box_width, box_height;

  /** User zoom, applied on top of the natural (fit-to-box) scale. */
  float scaling;

  /** Relative coordinates of the centre of the ROI within the whole image. Written and read as
   *  a pair -- a frame planned from half the old pan and half the new is the failure this
   *  object exists to make unrepresentable. (was roi.x/roi.y) */
  float center_x, center_y;

  /** TRUE once a widget allocation has been handed in. (was roi.gui_inited) */
  gboolean configured;
} dt_dev_viewport_state_t;

typedef struct dt_dev_viewport_t
{
  /** Single-writer seqlock, same contract as dt_dev_geometry_store_t: the GUI thread is the
   *  only writer, while the darkroom worker and drawlayer's "draw-back" thread both read. */
  dt_atomic_uint64 generation;
  dt_dev_viewport_state_t state;
} dt_dev_viewport_t;

/** Allocate/free. dt_dev_init() creates one only for a gui_attached dev. */
dt_dev_viewport_t *dt_dev_viewport_new(void);
void dt_dev_viewport_free(dt_dev_viewport_t *viewport);

/**
 * @brief Coherent copy of the viewport state, by value -- the ordinary way to read it:
 *
 *    const dt_dev_viewport_state_t viewport = dt_dev_viewport_get(dev);
 *
 * @details A dev with no viewport yields the NEUTRAL state, which is not zeroed: scaling is
 * 1.0 and the centre is (0.5, 0.5), reproducing exactly what dt_dev_reset_roi() left on a
 * headless dev before this object existed. Callers that need to distinguish "no viewport" from
 * "viewport at defaults" ask dt_dev_viewport_exists().
 */
dt_dev_viewport_state_t dt_dev_viewport_get(const struct dt_develop_t *dev);

/** The neutral state a dev without a viewport reads as. */
dt_dev_viewport_state_t dt_dev_viewport_neutral(void);

gboolean dt_dev_viewport_exists(const struct dt_develop_t *dev);

/* Single-field readers, one line each, for the sites that want one number. */
float dt_dev_viewport_scaling(const struct dt_develop_t *dev);
float dt_dev_viewport_center_x(const struct dt_develop_t *dev);
float dt_dev_viewport_center_y(const struct dt_develop_t *dev);
int32_t dt_dev_viewport_box_width(const struct dt_develop_t *dev);
int32_t dt_dev_viewport_box_height(const struct dt_develop_t *dev);
int32_t dt_dev_viewport_widget_width(const struct dt_develop_t *dev);
int32_t dt_dev_viewport_widget_height(const struct dt_develop_t *dev);
int32_t dt_dev_viewport_border_size(const struct dt_develop_t *dev);
gboolean dt_dev_viewport_configured(const struct dt_develop_t *dev);

/* ---- mutators ----
 *
 * GUI thread only. Each returns TRUE when the published state actually changed, so a caller
 * can decide whether to trigger a recompute without comparing fields itself. Each publishes
 * once, as a whole state, so no reader can observe a half-applied change; and each applies its
 * own clamps BEFORE publishing, so no value the clamp would take back is ever visible.
 *
 * A dev with no viewport accepts every call and does nothing.
 */

/** The Gtk allocation of the darkroom centre widget, in logical pixels. */
gboolean dt_dev_viewport_set_widget_size(struct dt_develop_t *dev, int32_t widget_width,
                                         int32_t widget_height);

/** Border policy (ISO 12646 toggle, preference), in logical pixels. */
gboolean dt_dev_viewport_set_border(struct dt_develop_t *dev, int32_t border_size);

/** The raster-pixel box an image backbuffer covers, and the "a size has been handed in" flag.
 *
 *  Takes the box already converted to raster pixels, because that is what its one caller
 *  computes today (dt_dev_configure_real, through dt_dev_convert_roi) and because the border
 *  subtraction that precedes it lives at ITS callers (views/dev_toolbox.c). Folding both into
 *  this object is the right end state and a behaviour change to argue separately; this tranche
 *  relocates state, it does not redistribute arithmetic. */
gboolean dt_dev_viewport_set_box(struct dt_develop_t *dev, int32_t box_width, int32_t box_height);

/** Absolute pan, in normalized image coordinates. The caller clamps: the clamp needs the
 *  processed size and the zoom level, which belong to the ROI request, not here. */
gboolean dt_dev_viewport_set_center(struct dt_develop_t *dev, float center_x, float center_y);

/** User zoom, applied on top of the natural (fit-to-box) scale. Clamped by the caller, for the
 *  same reason as the centre. */
gboolean dt_dev_viewport_set_scaling(struct dt_develop_t *dev, float scaling);

/** Reset to the neutral zoom and centre, without touching the allocation or border. */
void dt_dev_viewport_reset(struct dt_develop_t *dev);

#endif // DT_DEVELOP_DEV_VIEWPORT_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
