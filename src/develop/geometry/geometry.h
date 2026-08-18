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

/** @file develop/geometry/geometry.h
 *
 * @brief Where things are on the image, answered without a pipeline.
 *
 * @details The GUI constantly needs two geometric facts: how big the developed image is, and
 * where a point on it lands after the distorting modules have had their say. Today both are
 * answered by `dev->virtual_pipe' -- a complete, pixel-less clone of all ~95 IOP modules, their
 * history committed into real pipeline nodes, resynchronised on the GUI thread at every history
 * commit for 0.10 to 0.33 s. It renders nothing. It exists only to be walked.
 *
 * This module replaces that with the data the walk actually consumes. Each module that changes
 * geometry publishes a small record -- its transform as values, not as a node -- and the GUI
 * composes sizes and coordinates from the ordered list. See doc/geometry-service.md for the
 * decision, the survey behind it, and the tranche plan; the traps section there is not
 * optional reading.
 *
 * THREADING. This is GUI-thread state and takes no locks. Nothing else may touch it. The pixel
 * pipelines keep their own piece-based modify_roi and distort callbacks for rendering, and the
 * two must never be crossed: a worker that needs geometry asks its own pieces.
 *
 * STATUS: skeleton (tranche G2). No module publishes a record yet, so the chain is never
 * authoritative and nothing consumes it; it runs beside the virtual pipe and reports what it
 * would need in order to take over. Shadow mode is the whole point of this tranche.
 */

#ifndef DT_DEVELOP_GEOMETRY_GEOMETRY_H
#define DT_DEVELOP_GEOMETRY_GEOMETRY_H

#include <glib.h>
#include <stdint.h>

#include "develop/pixelpipe_hb.h"   // dt_iop_roi_t

struct dt_develop_t;
struct dt_iop_module_t;
struct dt_geometry_record_t;
struct dt_geometry_chain_t;

/**
 * @brief A module's geometry, evaluated. Pure functions of the record's own data.
 *
 * @details Every entry may be NULL, which means "identity for this operation": a module that
 * resizes but does not move points (demosaic's downsample) has a map_size and no transform, and
 * a module that is only ever asked for its dimensions (graduatednd) has neither.
 *
 * These evaluators are the SAME code the module's own distort_transform()/modify_roi_out() run
 * on the pixel pipe -- one shared static helper per module, called from both sides. That rule is
 * not stylistic: two derivations of the same geometry drift, and the drift shows up as an
 * overlay that no longer sits on the thing it describes, months later, on one image.
 */
typedef struct dt_geometry_vtable_t
{
  /** @brief Full-resolution input rect -> output rect. Mirrors modify_roi_out() at scale 1. */
  void (*map_size)(const void *data, const dt_iop_roi_t *const in, dt_iop_roi_t *out);

  /** @brief Image coordinates in -> image coordinates out, in place, @p points_count pairs.
   *  @p ctx composes the sub-chain upstream of this record, for the one module that needs it
   *  (liquify's warps live in RAW coordinates). NULL for every other module. */
  int (*transform)(const void *data, const struct dt_geometry_record_t *const record,
                   struct dt_geometry_chain_t *chain, float *points, size_t points_count);

  /** @brief The inverse of ::transform. */
  int (*backtransform)(const void *data, const struct dt_geometry_record_t *const record,
                       struct dt_geometry_chain_t *chain, float *points, size_t points_count);
} dt_geometry_vtable_t;

/**
 * @brief One module instance's contribution, as data.
 *
 * @details There is a record for EVERY enabled module, not only the geometric ones: consumers
 * ask this list for their own module's input and output dimensions, and `graduatednd' -- which
 * has no geometry callbacks at all -- is one of them. A module with no geometry gets a record
 * with a NULL vtable, which the size fold treats as identity and the walkers skip.
 */
typedef struct dt_geometry_record_t
{
  char op[32];             /**< module operation name */
  int instance;            /**< multi_priority: which instance of that operation */
  double iop_order;        /**< position in the pipe; the walkers' ordering and bound */
  gboolean enabled;

  /* The candidate half of the query-time GUI exception: the FOCUSED module's tag filter is
   * tested against this. The focused module's own filter is not stored per record -- it is one
   * value for the whole query, and it lives on the chain. See dt_geometry_set_focus(). */
  int operation_tags;

  const dt_geometry_vtable_t *vtable;   /**< NULL: this module is geometrically identity */
  void *data;                           /**< module-owned blob, freed by ::free_data */
  void (*free_data)(void *data);        /**< NULL when ::data needs no teardown */

  /* Filled by the chain's own size fold, at full resolution and scale 1 -- the same numbers
   * dt_dev_pixelpipe_get_roi_out() writes into piece->buf_in/buf_out today, which is what
   * consumers actually read off the virtual pipe. */
  dt_iop_roi_t in;
  dt_iop_roi_t out;
} dt_geometry_record_t;

/** @brief The composed geometry of one image, for one dev. GUI thread only. */
typedef struct dt_geometry_chain_t dt_geometry_chain_t;

dt_geometry_chain_t *dt_geometry_chain_new(void);
void dt_geometry_chain_free(dt_geometry_chain_t *chain);

/**
 * @brief Rebuild the chain from the dev's current modules and history. GUI thread only.
 *
 * @details Called wherever the virtual pipe is resynchronised today. Cheap by construction --
 * a record is a small derivation of already-committed params, with no LUT, no colour transform
 * and no disk access -- which is the point: it can run in the same step as the history write,
 * where the virtual pipe's 0.1-0.3 s could not.
 */
void dt_geometry_chain_rebuild(struct dt_develop_t *dev);

/**
 * @brief Can this chain answer questions yet?
 *
 * @details TRUE only when every enabled module the roster names has published a record.
 * Authority is WHOLESALE: composing some modules from records and the rest from pipeline
 * pieces would interleave two states, and the result would be wrong in a way that looks
 * plausible. Until the last roster module lands, this stays FALSE and every consumer keeps
 * using the pipe.
 */
gboolean dt_geometry_chain_authoritative(const dt_geometry_chain_t *chain);

/** @brief The developed image's full-resolution size, from the chain's own fold. */
gboolean dt_geometry_chain_processed_size(const dt_geometry_chain_t *chain, int *width, int *height);

/** @brief One module instance's record, or NULL. Use it for that module's own in/out dims. */
const dt_geometry_record_t *dt_geometry_chain_find(const dt_geometry_chain_t *chain, const char *op,
                                                   int instance);

/* Direction modes, matching dt_dev_distort_transform_plus()'s DT_DEV_TRANSFORM_DIR_* exactly:
 * the walkers this replaces are bounded folds, and every existing caller's bound has to keep
 * meaning what it meant. */

/** @brief Compose forward over the chain, in place. @p direction is a DT_DEV_TRANSFORM_DIR_*. */
int dt_geometry_transform(struct dt_develop_t *dev, double iop_order, int direction, float *points,
                          size_t points_count);

/** @brief Compose backward over the chain, in place. */
int dt_geometry_backtransform(struct dt_develop_t *dev, double iop_order, int direction, float *points,
                              size_t points_count);

/**
 * @brief Publish the focused module and its editing state, for the query-time GUI exception.
 *
 * @details dt_dev_pixelpipe_activemodule_disables_currentmodule() reads the focused module, its
 * operation_tags_filter() and its live cache-bypass flag, and disables matching modules for the
 * duration of a walk -- and, in the size fold, for the resulting processed size too. That is
 * view state: it changes when the user clicks into crop's edit mode, with no history commit
 * anywhere. It therefore cannot be baked into records; the chain reads it at query time, from
 * whatever the GUI last published here.
 */
void dt_geometry_set_focus(struct dt_develop_t *dev, const struct dt_iop_module_t *focused,
                           gboolean editing);

/** @brief Forget the focused module. Call before any module list teardown -- see the note on
 *  dt_geometry_set_focus(): the chain keeps the focus by VALUE precisely so a stale publication
 *  cannot dereference a destroyed module, but clearing it at teardown keeps the state honest. */
void dt_geometry_clear_focus(struct dt_develop_t *dev);

/**
 * @brief Shadow mode: compare the chain against the pipe that still owns the answer.
 *
 * @details Called from the size path with whatever the virtual pipe just produced. While the
 * roster is incomplete this reports exactly which enabled modules are still missing a record --
 * measured per image, rather than assumed from a source audit -- and once it is complete it
 * reports any divergence. Both under `-d dev'. It never changes behaviour.
 */
void dt_geometry_shadow_check_size(struct dt_develop_t *dev, int pipe_width, int pipe_height);

#endif // DT_DEVELOP_GEOMETRY_GEOMETRY_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
