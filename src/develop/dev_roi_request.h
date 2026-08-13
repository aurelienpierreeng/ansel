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

#ifndef DT_DEVELOP_DEV_ROI_REQUEST_H
#define DT_DEVELOP_DEV_ROI_REQUEST_H

#include <glib.h>
#include <math.h>
#include <stdint.h>

#include "system/atomic.h"

struct dt_develop_t;

/**
 * @brief The coherent set of numbers a darkroom pipe plans its ROI from.
 *
 * @details These were loose fields of dev->roi, written by the GUI thread in three separate
 * functions and read one at a time by the worker inside _update_darkroom_roi(). Every consumer
 * uses them as a product -- natural_scale * scaling * processed_size -- so a read that straddles
 * a write yields a frame whose geometry never existed, and which is internally consistent and
 * correctly hashed, so nothing downstream can tell.
 *
 * They now cross the GUI/pipeline boundary only together, published as one record with a
 * generation. The viewport and the geometry record are the inputs; this is what is derived from
 * them and handed to the pipeline.
 */
typedef struct dt_dev_roi_request_t
{
  /** Bumped only when the payload below actually changed. */
  uint64_t generation;

  int32_t box_width, box_height;             /**< from the viewport */
  int32_t processed_width, processed_height; /**< from the geometry record */
  int32_t preview_width, preview_height;     /**< roundf(natural_scale * processed_*) */
  float natural_scale;                       /**< fit-to-box scale, see dt_dev_roi_natural_scale() */
  float scaling;                             /**< user zoom, from the viewport */
  float center_x, center_y;                  /**< ROI centre, from the viewport */

  /**
   * Was roi.output_inited. DERIVED at every publication, never asserted and never cleared on
   * its own: it is `viewport.configured && geometry.raw_inited && geometry.processed_inited',
   * i.e. "all three inputs of this record have been published at least once".
   *
   * It is NOT a statement about which image those numbers describe. Between an image change
   * republishing the raw pair and dt_dev_get_thumbnail_size() rerunning the virtual pipe,
   * processed_* -- and everything derived from it -- still measures the PREVIOUS image, with
   * valid TRUE throughout. On the darkroom's own image-change path that window contains no
   * running worker (views/darkroom.c's leave() joins it before the new raw geometry is
   * published, and enter() only restarts it after the republish), which is why this record does
   * not try to describe it. Closing it for the other producers needs the geometry setters to
   * publish this record themselves; it is not something this flag can be made to express by
   * clearing it somewhere.
   */
  gboolean valid;
} dt_dev_roi_request_t;

typedef struct dt_dev_roi_request_store_t
{
  dt_atomic_uint64 generation;   /**< odd while a publication is in flight */
  dt_dev_roi_request_t value;
} dt_dev_roi_request_store_t;

/**
 * @brief natural scaling = MIN(box / processed, 1): the image fits the widget minus its borders.
 *
 * @details Pure, so the publisher and any test can call it with numbers rather than a dev.
 * Returns -1 for a non-positive processed size, which is the sentinel dt_dev_get_natural_scale()
 * has always returned when the geometry was not ready.
 */
static inline float dt_dev_roi_natural_scale(const int32_t box_width, const int32_t box_height,
                                             const int32_t processed_width,
                                             const int32_t processed_height)
{
  if(processed_width <= 0 || processed_height <= 0) return -1.f;

  return fminf(fminf((float)box_width / (float)processed_width,
                     (float)box_height / (float)processed_height),
               1.f);
}

/** Seed the store. Call once, from dt_dev_init(). */
void dt_dev_roi_request_init(struct dt_develop_t *dev);

/**
 * @brief Recompute the derived members from the viewport and the geometry record, and publish
 * if anything changed. Returns the current generation.
 *
 * @details The ONE place the derived values are computed, and GUI-thread only: it reads the
 * viewport, which only the GUI thread writes. The generation advances only on a real payload
 * change, which is what lets a consumer treat it as a cache key.
 */
uint64_t dt_dev_roi_request_publish(struct dt_develop_t *dev);

/** Coherent copy (seqlock read). The returned record is zeroed with valid == FALSE when nothing
 *  usable has been published. */
dt_dev_roi_request_t dt_dev_roi_request_get(const struct dt_develop_t *dev);

/* Single-field readers, one line each. Use dt_dev_roi_request_get() wherever more than one is
 * needed: the whole point of the record is that its members are consumed as a product. */
int32_t dt_dev_roi_request_preview_width(const struct dt_develop_t *dev);
int32_t dt_dev_roi_request_preview_height(const struct dt_develop_t *dev);
float dt_dev_roi_request_natural_scale(const struct dt_develop_t *dev);
gboolean dt_dev_roi_request_valid(const struct dt_develop_t *dev);

#endif // DT_DEVELOP_DEV_ROI_REQUEST_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
