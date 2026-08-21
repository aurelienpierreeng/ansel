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
 * where a point on it lands after the distorting modules have had their say. Both used to be
 * answered by a complete, pixel-less clone of all ~95 IOP modules -- their history committed
 * into real pipeline nodes, resynchronised on the GUI thread at every history commit for 0.10 to
 * 0.33 s. It rendered nothing. It existed only to be walked.
 *
 * This module is the data that walk actually consumed. Each module that changes geometry
 * publishes a small record -- its transform as values, not as a node -- and the GUI composes
 * sizes and coordinates from the ordered list. See doc/geometry-service.md for the decision, the
 * survey behind it, and what each tranche moved; the traps section there is not optional reading.
 *
 * THREADING. This is GUI-thread state and takes no locks. Nothing else may touch it. The pixel
 * pipelines keep their own piece-based modify_roi and distort callbacks for rendering, and the
 * two must never be crossed: a worker that needs geometry asks its own pieces. That separation
 * is the whole design: the GUI's copy of the module stack is gone, not shared.
 */

#ifndef DT_DEVELOP_GEOMETRY_GEOMETRY_H
#define DT_DEVELOP_GEOMETRY_GEOMETRY_H

#include <glib.h>
#include <stdint.h>

#include "develop/pixelpipe_hb.h"   // dt_iop_roi_t

/* C linkage: geometry.c is a C translation unit and iop/lens.cc -- the first module to publish
 * a record -- is a C++ one. Without this its calls and its vtable would name mangled symbols
 * nothing defines. Linux would still link the plugin (a shared object may carry undefined
 * symbols and resolve them at dlopen time) and fail at runtime; macOS and Windows fail the
 * build. Same reason control/user_message.h carries these guards. */
#ifdef __cplusplus
extern "C" {
#endif

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
   * dt_dev_pixelpipe_get_roi_out() writes into a piece's buf_in/buf_out, which is what GUI
   * consumers used to resolve a piece in order to read. */
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
 * @details Called wherever the pixel-less pipe used to be resynchronised. Cheap by construction
 * -- a record is a small derivation of already-committed params, with no LUT, no colour transform
 * and no disk access -- which is the point: it runs in the same step as the history write, where
 * its predecessor's 0.1-0.3 s could not.
 */
void dt_geometry_chain_rebuild(struct dt_develop_t *dev);

/**
 * @brief Can this chain answer questions yet?
 *
 * @details TRUE only when every enabled module the roster names has published a record.
 * Authority is WHOLESALE: composing some modules from records and the rest from pipeline pieces
 * would interleave two states, and the result would be wrong in a way that looks plausible.
 *
 * There is no longer anything to fall back TO -- the pipe this replaced is deleted -- so a FALSE
 * here is not a degraded mode, it is the GUI declining to answer: sizes come back FALSE, the
 * transforms return 0 and leave their points untouched. Before an image is loaded that is simply
 * the truth. After one is, it is a defect, and ::dt_geometry_self_check names which module owes
 * a record.
 */
gboolean dt_geometry_chain_authoritative(const dt_geometry_chain_t *chain);

/**
 * @brief How many times this chain has been rebuilt. A GUI cache key for anything derived from
 * the composed geometry.
 *
 * @details A consumer that caches something it composed through this service -- a mask outline in
 * image coordinates, say -- needs to know when to throw that cache away. The answer is "when the
 * geometry moved", and this counter is that, cheaply: it advances once per rebuild, and a rebuild
 * happens exactly where a pipe flag is raised, i.e. where the module stack or the history changed.
 *
 * It is deliberately NOT a content hash. A rebuild that lands on identical geometry still advances
 * it, which costs a consumer one redundant recompute; the alternative -- hashing each record's
 * module-owned data blob, whose size this service does not know -- would have to guess, and a
 * missed change here is an overlay drawn in the wrong place. Over-invalidating is the safe
 * direction for a key.
 *
 * What it must NOT be replaced by is a PIXEL identity. Keying an outline cache on a pipe's
 * backbuffer hash, which is what iop/masks did before, ties a geometric fact to a rendering event:
 * every republished preview frame -- continuous while a brush is being dragged -- then invalidates
 * outlines whose inputs did not change. Measured on the report in #1158: 566 rebuilds of two brush
 * outlines in 80 seconds, ~2 s of coordinate transform, gravity centres recomputed bit-identical
 * every time, and a darkroom expose growing from 23 ms to 137 ms as strokes accumulated.
 *
 * @return 0 for a chain that has never been built, which no live generation can equal.
 */
uint64_t dt_geometry_chain_generation(const dt_geometry_chain_t *chain);

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
 * @brief Apply ONE module's own transform, and nothing else.
 *
 * @details No direction bound expresses this: FORW_INCL at a module's own iop_order includes
 * everything after it as well. iop/ashift.c wants exactly its own homography applied to the
 * corners of its input, to work out where its output lands, and reached it by resolving its
 * piece and calling its own distort_transform() through the module vtable.
 *
 * Honours the focused-module exception like every other composition here, so a module suppressed
 * by whatever is being edited contributes nothing, exactly as it would in a full walk.
 *
 * @return 0 when the chain cannot answer -- not authoritative, no record, or that module has no
 * transform -- in which case @p points is untouched.
 */
int dt_geometry_module_transform(struct dt_develop_t *dev, const struct dt_iop_module_t *module,
                                 float *points, size_t points_count);

/**
 * @brief Compose the chain over @p points, for a record evaluator that needs the transform
 * stack around its own module.
 *
 * @details The nested case, and the reason ::dt_geometry_vtable_t hands every evaluator the
 * chain. iop/liquify.c is the one that needs it: its warps are stored in RAW sensor
 * coordinates, so before it can rasterise anything it has to push its own path nodes through
 * everything upstream of itself. On the pixel pipe it does that by re-entering the pipe walker
 * mid-walk; here it re-enters this.
 *
 * Bounded exactly like the walkers, so BACK_EXCL of the caller's own iop_order excludes the
 * caller and the recursion terminates. Do not call it with a bound that includes the caller.
 */
int dt_geometry_chain_compose(dt_geometry_chain_t *chain, double iop_order, int direction, float *points,
                              size_t points_count);

/* THE FOCUS EXCEPTION is not an entry point, deliberately. It used to be one -- a setter the GUI
 * was supposed to call when the focused module or its editing state changed -- and nothing ever
 * called it, so the chain composed every module while the pipe was skipping the ones the focused
 * module's tag filter disables. The visible result was ashift's detected-lines overlay drawn
 * against a different module stack than the image under it.
 *
 * A published copy of view state has to be refreshed to stay true and is wrong in between; that
 * is the same lesson control/input.h records about the stored mouse-button state. So the chain
 * now reads it where the pipe reads it -- dev->gui_module, its operation_tags_filter() and its
 * live cache-bypass flag, at query time -- and the two cannot drift because there is nothing to
 * keep in step. Do not reintroduce a setter for this.
 */

/**
 * @brief Check the chain against itself, and report what the rebuild cost.
 *
 * @details What the shadow harness became. It compared every answer against the pipe while the
 * pipe still owned them; with the pipe gone the only checks left are identities the chain must
 * satisfy on its own -- the round trip, and the bound partition. See the comment on the
 * definition for what that does and does not still catch. Under `-d dev'; never changes
 * behaviour.
 *
 * @param chain_ms what the rebuild cost.
 */
void dt_geometry_self_check(struct dt_develop_t *dev, double chain_ms);

#ifdef __cplusplus
}
#endif

#endif // DT_DEVELOP_GEOMETRY_GEOMETRY_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
