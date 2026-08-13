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

#ifndef DT_DEVELOP_DEV_GEOMETRY_H
#define DT_DEVELOP_DEV_GEOMETRY_H

#include <glib.h>
#include <stdint.h>

#include "system/atomic.h"

struct dt_develop_t;

/**
 * @brief Objective facts about the image a dev is working on.
 *
 * @details This is not GUI state, and the distinction is not academic. Every field here is
 * needed with no GUI at all: export, thumbnail generation and dev_snapshot's frozen dev all
 * resolve drawn-mask geometry against the raw dimensions, and the darkroom worker feeds them
 * to dt_dev_pixelpipe_set_input() on every loop tick.
 *
 * Leaving them at zero does not fail loudly. dt_dev_coordinates_raw_norm_to_raw_abs()
 * early-returns on raw_width == 0 WITHOUT transforming, so a shape's normalized centre
 * masquerades as absolute pixel coordinates and every mask on the image collapses to within a
 * few hundred pixels of the origin. That shipped once, because these fields were mistaken for
 * GUI state and written only `if(dev->gui_attached)`. See CLAUDE.md, and do not gate
 * dt_dev_geometry_set_raw_size() on anything.
 */
typedef struct dt_dev_image_geometry_t
{
  /** Dimensions of the full-resolution RAW image being worked on. */
  int32_t raw_width, raw_height;

  /** Dimensions of the final processed image if it were processed full-resolution: the final
   *  aspect ratio, with all cropping and distortions taken into account. */
  int32_t processed_width, processed_height;

  /** TRUE once the raw pair has been published from a successful mipmap read. */
  gboolean raw_inited;

  /** TRUE once the processed pair has been published from the virtual pipe. There is no
   *  headless producer for it today, so a caller that gets FALSE must not read 0x0 as though
   *  it were an answer -- that is the raw_width mistake, one field over. */
  gboolean processed_inited;
} dt_dev_image_geometry_t;

/**
 * @brief Single-writer seqlock around the record.
 *
 * @details `generation` is odd while a publication is in flight and even once it has settled.
 * A reader copies the value, re-reads the generation and retries on a mismatch, so a pipeline
 * thread can never observe a new width paired with an old height -- which is the failure this
 * record exists to remove, the pair having been two independent unlocked stores before.
 *
 * Deliberately not a mutex: one here would create a lock-ordering obligation against
 * history_mutex and the pipe's busy_mutex, both of which are already held across reads of
 * these values.
 */
typedef struct dt_dev_geometry_store_t
{
  dt_atomic_uint64 generation;
  dt_dev_image_geometry_t value;
} dt_dev_geometry_store_t;

/** Seed the store. Call once, from dt_dev_init(), before anything can read it. */
void dt_dev_geometry_init(struct dt_develop_t *dev);

/** Publish the raw pair, and whether it is usable. MUST NOT be gated on gui_attached. */
void dt_dev_geometry_set_raw_size(struct dt_develop_t *dev, int32_t width, int32_t height,
                                  gboolean valid);

/** Publish the processed pair. GUI thread only: its producer runs the virtual pipe. */
void dt_dev_geometry_set_processed_size(struct dt_develop_t *dev, int32_t width, int32_t height);

/** Coherent copy of the whole record, by value -- the ordinary way to read it:
 *
 *    const dt_dev_image_geometry_t geometry = dt_dev_geometry_snapshot(dev);
 *
 *  One seqlock read, then plain member access, so a caller that needs several fields cannot
 *  accidentally pair them across a publication. Zeroed, with both _inited bits FALSE, when
 *  nothing has been published yet. */
dt_dev_image_geometry_t dt_dev_geometry_snapshot(const struct dt_develop_t *dev);

/** Out-param form, for callers that also want the "is anything published" answer. */
gboolean dt_dev_geometry_get(const struct dt_develop_t *dev, dt_dev_image_geometry_t *out);

/** Coherent copy of the raw pair. Returns raw_inited; the out-params are written either way,
 *  so a caller that ignores the result sees exactly today's values. */
gboolean dt_dev_geometry_get_raw_size(const struct dt_develop_t *dev, int32_t *width,
                                      int32_t *height);

/** Coherent copy of the processed pair. Returns processed_inited, same out-param contract. */
gboolean dt_dev_geometry_get_processed_size(const struct dt_develop_t *dev, int32_t *width,
                                            int32_t *height);

/* Single-field readers, each a coherent read of the whole record returning one member.
 * Use the pair getters above wherever the two are consumed as one geometric fact -- these
 * exist for the many sites that genuinely want one number, and for a mechanical migration
 * off the old struct members that cannot silently change what a site reads. */
int32_t dt_dev_geometry_raw_width(const struct dt_develop_t *dev);
int32_t dt_dev_geometry_raw_height(const struct dt_develop_t *dev);
int32_t dt_dev_geometry_processed_width(const struct dt_develop_t *dev);
int32_t dt_dev_geometry_processed_height(const struct dt_develop_t *dev);
gboolean dt_dev_geometry_raw_inited(const struct dt_develop_t *dev);

#endif // DT_DEVELOP_DEV_GEOMETRY_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
