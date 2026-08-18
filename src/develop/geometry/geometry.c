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

#include "develop/geometry/geometry.h"

#include "common/logging.h"
#include "develop/dev_geometry.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "system/mem_alloc.h"

#include <math.h>
#include <string.h>

/**
 * THE ROSTER.
 *
 * Which modules owe this service a record. It is a hand-maintained list and cannot be anything
 * else: dt_iop_module_t's geometry callbacks are bound through the DEFAULT() macros in
 * common/module_api.h, so EVERY module has a non-NULL, no-op distort_transform() and an identity
 * modify_roi_out(). There is no runtime predicate for "is this a geometry module" to test.
 *
 * Membership is by source audit (doc/geometry-service.md §1): the 15 modules that implement
 * modify_roi_out plus the 11 that implement distort_transform, minus the two that implement
 * neither in a way this service is concerned with.
 *
 * NOT on the roster, deliberately:
 *   - retouch, spots: identity modify_roi_out and no point transform at all. Their
 *     modify_roi_in expands the read window for source patches, which is a RENDERING concern --
 *     it reads live dev->forms on the pipeline thread and exists so a clone source is inside
 *     the tile. Nothing about it belongs to GUI geometry.
 *   - initialscale, finalscale: no modify_roi_out whatsoever, so identity in a scale-1 fold,
 *     and no point transform. finalscale's enabled state additionally depends on pipe type and
 *     zoom, which is exactly the kind of per-pipe fact a pipe-less service must not try to own.
 *   - useless: the module template, not built.
 *
 * A module added to this list without a geometry_record() implementation keeps the chain
 * non-authoritative forever, which is the safe direction: consumers go on using the pipe.
 */
static const char *const _roster[] = {
  "rawprepare", "basebuffer", "demosaic", "lens",     "ashift",  "liquify", "rotatepixels",
  "scalepixels", "flip",      "clipping", "crop",     "borders",
};

struct dt_geometry_chain_t
{
  GList *records;   /**< dt_geometry_record_t*, ordered by iop_order, one per ENABLED module */

  int32_t raw_width, raw_height;
  int32_t processed_width, processed_height;
  gboolean sized;

  /** Every roster module that is enabled has published a record. See the header. */
  gboolean authoritative;

  /* What the last rebuild could not get. Kept so shadow mode can name it instead of just
   * reporting "not ready", which would be true and useless. */
  GList *missing;   /**< gchar*, owned */

  /* Query-time GUI state, published by dt_geometry_set_focus(). Held BY VALUE, not as a
   * dt_iop_module_t pointer: the focus outlives individual publications, and a module can be
   * destroyed (instance removed, image changed, darkroom left) without anyone telling this
   * chain. A stored pointer would then be dereferenced by the next query. The two things the
   * exception needs -- who is focused, and what that module filters -- are both immutable for
   * a given module, so copying them costs nothing and cannot dangle. */
  char focus_op[32];
  int focus_instance;
  int focus_tags_filter;
  gboolean focus_active;
};

static gboolean _on_roster(const char *op)
{
  if(IS_NULL_PTR(op)) return FALSE;
  for(size_t i = 0; i < G_N_ELEMENTS(_roster); i++)
    if(!strcmp(op, _roster[i])) return TRUE;
  return FALSE;
}

static void _record_free(void *ptr)
{
  dt_geometry_record_t *record = (dt_geometry_record_t *)ptr;
  if(IS_NULL_PTR(record)) return;
  if(!IS_NULL_PTR(record->free_data) && !IS_NULL_PTR(record->data)) record->free_data(record->data);
  dt_free(record);
}

static void _chain_clear(dt_geometry_chain_t *chain)
{
  g_list_free_full(chain->records, _record_free);
  chain->records = NULL;
  g_list_free_full(chain->missing, dt_free_gpointer);
  chain->missing = NULL;
  chain->authoritative = FALSE;
  chain->sized = FALSE;
  chain->processed_width = chain->processed_height = 0;
}

dt_geometry_chain_t *dt_geometry_chain_new(void)
{
  return (dt_geometry_chain_t *)g_malloc0(sizeof(dt_geometry_chain_t));
}

void dt_geometry_chain_free(dt_geometry_chain_t *chain)
{
  if(IS_NULL_PTR(chain)) return;
  _chain_clear(chain);
  dt_free(chain);
}

static gint _by_iop_order(gconstpointer a, gconstpointer b)
{
  const dt_geometry_record_t *ra = (const dt_geometry_record_t *)a;
  const dt_geometry_record_t *rb = (const dt_geometry_record_t *)b;
  if(ra->iop_order < rb->iop_order) return -1;
  if(ra->iop_order > rb->iop_order) return 1;
  return 0;
}

/**
 * @brief Is this record disabled for the current query by the focused module?
 *
 * @details The chain's copy of dt_dev_pixelpipe_activemodule_disables_currentmodule(). It is
 * evaluated HERE, per query, and never stored in a record: the inputs are the focused module's
 * identity, its tag filter, and whether it is in editing mode -- all of which change with no
 * history commit, which is precisely why records cannot carry them.
 */
static gboolean _suppressed_by_focus(const dt_geometry_chain_t *chain, const dt_geometry_record_t *record)
{
  if(!chain->focus_active) return FALSE;
  if(!strcmp(chain->focus_op, record->op) && chain->focus_instance == record->instance) return FALSE;
  return (chain->focus_tags_filter & record->operation_tags) != 0;
}

/**
 * @brief Fold every record's map_size in pipe order, at full resolution and scale 1.
 *
 * @details Reproduces dt_dev_pixelpipe_get_roi_out(): seed the input rect from the raw
 * dimensions at scale 1, hand each enabled module its input and take its output as the next
 * module's input, and record both on the way through. Those per-record rects are a product in
 * their own right -- consumers read their own module's input/output dimensions off them, and
 * not only the geometric modules do (graduatednd has no geometry callbacks and reads its output
 * rect for its overlay).
 *
 * A record with no vtable, or one suppressed by the focused module, is identity. The suppression
 * matters for SIZE and not only for coordinates: the fold this mirrors clears piece->enabled for
 * such modules, so the developed size genuinely changes when the user enters crop's edit mode.
 */
static void _fold_sizes(dt_geometry_chain_t *chain)
{
  dt_iop_roi_t rect = (dt_iop_roi_t){ 0, 0, chain->raw_width, chain->raw_height, 1.0f };

  for(GList *node = g_list_first(chain->records); node; node = g_list_next(node))
  {
    dt_geometry_record_t *record = (dt_geometry_record_t *)node->data;
    record->in = rect;

    if(!IS_NULL_PTR(record->vtable) && !IS_NULL_PTR(record->vtable->map_size)
       && !_suppressed_by_focus(chain, record))
      record->vtable->map_size(record->data, &rect, &record->out);
    else
      record->out = rect;

    rect = record->out;
  }

  chain->processed_width = rect.width;
  chain->processed_height = rect.height;
  chain->sized = (chain->raw_width > 0 && chain->raw_height > 0);
}

void dt_geometry_chain_rebuild(dt_develop_t *dev)
{
  if(IS_NULL_PTR(dev) || IS_NULL_PTR(dev->geometry_chain)) return;

  dt_geometry_chain_t *chain = dev->geometry_chain;
  _chain_clear(chain);

  int32_t raw_width = 0;
  int32_t raw_height = 0;
  if(!dt_dev_geometry_get_raw_size(dev, &raw_width, &raw_height)) return;
  chain->raw_width = raw_width;
  chain->raw_height = raw_height;

  gboolean complete = TRUE;

  for(GList *node = g_list_first(dev->iop); node; node = g_list_next(node))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)node->data;
    if(IS_NULL_PTR(module) || !module->enabled) continue;

    dt_geometry_record_t *record = (dt_geometry_record_t *)g_malloc0(sizeof(dt_geometry_record_t));
    if(IS_NULL_PTR(record)) continue;

    g_strlcpy(record->op, module->op, sizeof(record->op));
    record->instance = module->multi_priority;
    record->iop_order = module->iop_order;
    record->enabled = TRUE;
    record->operation_tags = module->operation_tags();

    /* A module that publishes nothing is identity, which is correct for the ~80 modules that
     * do not touch geometry -- their record exists only to carry dimensions. A ROSTER module
     * that publishes nothing is a gap, and the chain must not be trusted until it closes. */
    const gboolean published = !IS_NULL_PTR(module->geometry_record)
                               && module->geometry_record(module, record);

    if(!published && _on_roster(module->op))
    {
      complete = FALSE;
      chain->missing = g_list_prepend(chain->missing,
                                      g_strdup_printf("%s.%d", module->op, module->multi_priority));
    }

    chain->records = g_list_insert_sorted(chain->records, record, _by_iop_order);
  }

  _fold_sizes(chain);
  chain->authoritative = complete && chain->sized;
}

gboolean dt_geometry_chain_authoritative(const dt_geometry_chain_t *chain)
{
  return !IS_NULL_PTR(chain) && chain->authoritative;
}

gboolean dt_geometry_chain_processed_size(const dt_geometry_chain_t *chain, int *width, int *height)
{
  if(IS_NULL_PTR(chain) || !chain->sized) return FALSE;
  if(!IS_NULL_PTR(width)) *width = chain->processed_width;
  if(!IS_NULL_PTR(height)) *height = chain->processed_height;
  return TRUE;
}

const dt_geometry_record_t *dt_geometry_chain_find(const dt_geometry_chain_t *chain, const char *op,
                                                   const int instance)
{
  if(IS_NULL_PTR(chain) || IS_NULL_PTR(op)) return NULL;
  for(const GList *node = g_list_first(chain->records); node; node = g_list_next(node))
  {
    const dt_geometry_record_t *record = (const dt_geometry_record_t *)node->data;
    if(!strcmp(record->op, op) && record->instance == instance) return record;
  }
  return NULL;
}

/**
 * @brief Does this record fall inside the requested bound?
 *
 * @details The five modes are dt_dev_distort_transform_plus()'s, reproduced exactly. Every
 * existing caller's bound has to keep meaning what it meant -- the mask GUI in particular
 * composes BACK_EXCL up to a module, shifts, then FORW_INCL back out, and a bound that shifted
 * by one module would move every clone source by that module's transform.
 */
static gboolean _in_bound(const dt_geometry_record_t *record, const double iop_order, const int direction)
{
  switch(direction)
  {
    case DT_DEV_TRANSFORM_DIR_FORW_INCL: return record->iop_order >= iop_order;
    case DT_DEV_TRANSFORM_DIR_FORW_EXCL: return record->iop_order > iop_order;
    case DT_DEV_TRANSFORM_DIR_BACK_INCL: return record->iop_order <= iop_order;
    case DT_DEV_TRANSFORM_DIR_BACK_EXCL: return record->iop_order < iop_order;
    case DT_DEV_TRANSFORM_DIR_ALL:
    default: return TRUE;
  }
}

int dt_geometry_transform(dt_develop_t *dev, const double iop_order, const int direction, float *points,
                          const size_t points_count)
{
  if(IS_NULL_PTR(dev) || IS_NULL_PTR(dev->geometry_chain) || IS_NULL_PTR(points)) return 0;
  dt_geometry_chain_t *chain = dev->geometry_chain;
  if(!chain->authoritative) return 0;

  for(GList *node = g_list_first(chain->records); node; node = g_list_next(node))
  {
    dt_geometry_record_t *record = (dt_geometry_record_t *)node->data;
    if(IS_NULL_PTR(record->vtable) || IS_NULL_PTR(record->vtable->transform)) continue;
    if(!_in_bound(record, iop_order, direction)) continue;
    if(_suppressed_by_focus(chain, record)) continue;
    record->vtable->transform(record->data, record, chain, points, points_count);
  }
  return 1;
}

int dt_geometry_backtransform(dt_develop_t *dev, const double iop_order, const int direction, float *points,
                              const size_t points_count)
{
  if(IS_NULL_PTR(dev) || IS_NULL_PTR(dev->geometry_chain) || IS_NULL_PTR(points)) return 0;
  dt_geometry_chain_t *chain = dev->geometry_chain;
  if(!chain->authoritative) return 0;

  for(GList *node = g_list_last(chain->records); node; node = g_list_previous(node))
  {
    dt_geometry_record_t *record = (dt_geometry_record_t *)node->data;
    if(IS_NULL_PTR(record->vtable) || IS_NULL_PTR(record->vtable->backtransform)) continue;
    if(!_in_bound(record, iop_order, direction)) continue;
    if(_suppressed_by_focus(chain, record)) continue;
    record->vtable->backtransform(record->data, record, chain, points, points_count);
  }
  return 1;
}

void dt_geometry_set_focus(dt_develop_t *dev, const dt_iop_module_t *focused, const gboolean editing)
{
  if(IS_NULL_PTR(dev) || IS_NULL_PTR(dev->geometry_chain)) return;
  dt_geometry_chain_t *chain = dev->geometry_chain;

  if(IS_NULL_PTR(focused) || !editing)
  {
    chain->focus_active = FALSE;
    return;
  }

  g_strlcpy(chain->focus_op, focused->op, sizeof(chain->focus_op));
  chain->focus_instance = focused->multi_priority;
  chain->focus_tags_filter = focused->operation_tags_filter();
  chain->focus_active = TRUE;
}

void dt_geometry_clear_focus(dt_develop_t *dev)
{
  if(IS_NULL_PTR(dev) || IS_NULL_PTR(dev->geometry_chain)) return;
  dev->geometry_chain->focus_active = FALSE;
}

void dt_geometry_shadow_check(dt_develop_t *dev, const int pipe_width, const int pipe_height,
                              const double chain_ms)
{
  if(IS_NULL_PTR(dev) || IS_NULL_PTR(dev->geometry_chain)) return;
  if(!(dt_get_debug_flags() & DT_DEBUG_DEV)) return;

  const dt_geometry_chain_t *chain = dev->geometry_chain;

  if(!chain->authoritative)
  {
    /* Name the gap. "Not ready" is true and useless; the list below is the actual, per-image
     * work remaining, measured rather than taken from the source audit -- which is how a module
     * that the audit missed, or one that only becomes enabled on some images, gets found. */
    if(IS_NULL_PTR(chain->missing))
    {
      dt_print(DT_DEBUG_DEV, "[geometry] chain not authoritative: no usable raw geometry yet\n");
      return;
    }

    gchar **names = (gchar **)g_malloc0((g_list_length(chain->missing) + 1) * sizeof(gchar *));
    if(IS_NULL_PTR(names)) return;
    int i = 0;
    for(const GList *node = g_list_first(chain->missing); node; node = g_list_next(node))
      names[i++] = (gchar *)node->data;
    gchar *joined = g_strjoinv(", ", names);
    dt_free(names);   // the strings belong to chain->missing; only the vector is ours

    dt_print(DT_DEBUG_DEV, "[geometry] chain not authoritative: %d roster module(s) without a record: %s\n",
             g_list_length(chain->missing), joined);
    dt_free(joined);
    return;
  }

  if(chain->processed_width != pipe_width || chain->processed_height != pipe_height)
  {
    dt_print(DT_DEBUG_DEV, "[geometry] SIZE DIVERGENCE: chain %dx%d, pipe %dx%d\n", chain->processed_width,
             chain->processed_height, pipe_width, pipe_height);
    return;
  }

  /* The size fold only exercises map_size. Probe the point transforms too, or the evaluators
   * that actually move overlays would go unverified until a consumer switched over and a user
   * noticed a mask sitting in the wrong place.
   *
   * Five points spanning the raw frame, forward through everything (iop_order 0, DIR_ALL --
   * what dt_dev_coordinates_raw_abs_to_image_abs() asks for), then the same through the pipe.
   * The tolerance is half a pixel: both sides do the same arithmetic in the same order, so they
   * should agree exactly, but the fold's rects reach the evaluators as ints on one side and
   * through piece->buf_in on the other, and a rounding difference of that size is not a defect
   * worth failing on. Anything larger is a real divergence and is printed with the point. */
  const float w = (float)chain->raw_width;
  const float h = (float)chain->raw_height;
  float chain_points[10] = { 0.f, 0.f, w, 0.f, 0.f, h, w, h, 0.5f * w, 0.5f * h };
  float pipe_points[10];
  memcpy(pipe_points, chain_points, sizeof(pipe_points));

  if(!dt_geometry_transform(dev, 0.0, DT_DEV_TRANSFORM_DIR_ALL, chain_points, 5)) return;
  if(IS_NULL_PTR(dev->virtual_pipe)) return;
  dt_dev_distort_transform_plus(dev->virtual_pipe, 0.0, DT_DEV_TRANSFORM_DIR_ALL, pipe_points, 5);

  int diverged = 0;
  for(int i = 0; i < 5; i++)
  {
    if(fabsf(chain_points[2 * i] - pipe_points[2 * i]) > 0.5f
       || fabsf(chain_points[2 * i + 1] - pipe_points[2 * i + 1]) > 0.5f)
    {
      dt_print(DT_DEBUG_DEV,
               "[geometry] TRANSFORM DIVERGENCE at probe %d: chain (%.2f, %.2f), pipe (%.2f, %.2f)\n", i,
               chain_points[2 * i], chain_points[2 * i + 1], pipe_points[2 * i], pipe_points[2 * i + 1]);
      diverged++;
    }
  }

  /* And the inverse. Comparing forward against the pipe leaves every backtransform evaluator
   * unverified -- they are only ever exercised by GUI consumers that have not switched over yet
   * -- so round-trip the same probes through the chain and require the identity back.
   *
   * This is the check that decides a real question about flip, whose forward flips against the
   * input dimensions and THEN swaps the axes. Its inverse un-swaps and unflips against those
   * same dimensions, in the pre-swap frame where they are the right ones; swapping the
   * dimensions as well -- which the neighbouring modify_roi_in helper legitimately does, in its
   * own rectangle-mapping convention -- would break exactly the 90-degree orientations this
   * probe covers. Composed here rather than argued. */
  float roundtrip[10] = { 0.f, 0.f, w, 0.f, 0.f, h, w, h, 0.5f * w, 0.5f * h };
  const float origin[10] = { 0.f, 0.f, w, 0.f, 0.f, h, w, h, 0.5f * w, 0.5f * h };

  dt_geometry_transform(dev, 0.0, DT_DEV_TRANSFORM_DIR_ALL, roundtrip, 5);
  dt_geometry_backtransform(dev, 0.0, DT_DEV_TRANSFORM_DIR_ALL, roundtrip, 5);

  int not_identity = 0;
  for(int i = 0; i < 5; i++)
  {
    if(fabsf(roundtrip[2 * i] - origin[2 * i]) > 0.5f
       || fabsf(roundtrip[2 * i + 1] - origin[2 * i + 1]) > 0.5f)
    {
      dt_print(DT_DEBUG_DEV,
               "[geometry] ROUND-TRIP DIVERGENCE at probe %d: (%.2f, %.2f) came back as (%.2f, %.2f)\n", i,
               origin[2 * i], origin[2 * i + 1], roundtrip[2 * i], roundtrip[2 * i + 1]);
      not_identity++;
    }
  }

  if(diverged || not_identity)
    dt_print(DT_DEBUG_DEV,
             "[geometry] size agrees (%dx%d) but %d/5 forward probes and %d/5 round-trips diverge\n",
             chain->processed_width, chain->processed_height, diverged, not_identity);
  else
    dt_print(DT_DEBUG_DEV,
             "[geometry] agrees with the pipe: size %dx%d, 5/5 forward probes, 5/5 round-trips\n",
             chain->processed_width, chain->processed_height);

  /* The cost, which is the reason this service exists. Compare against `-d perf's "pipeline
   * resync with history ... for pipe virtual-preview" line: that is the same product, computed
   * the way this replaces. */
  dt_print(DT_DEBUG_DEV, "[geometry] chain rebuilt in %.2f ms\n", chain_ms);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
