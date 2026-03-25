/*
    Private pixelpipe process API shared by pixelpipe_hb.c, pixelpipe_cpu.c and pixelpipe_gpu.c.
*/

#pragma once

#include "common/darktable.h"
#include "develop/pixelpipe_hb.h"
#include "develop/tiling.h"

#include <string.h>

typedef enum dt_pixelpipe_flow_t
{
  PIXELPIPE_FLOW_NONE = 0,
  PIXELPIPE_FLOW_HISTOGRAM_NONE = 1 << 0,
  PIXELPIPE_FLOW_HISTOGRAM_ON_CPU = 1 << 1,
  PIXELPIPE_FLOW_HISTOGRAM_ON_GPU = 1 << 2,
  PIXELPIPE_FLOW_PROCESSED_ON_CPU = 1 << 3,
  PIXELPIPE_FLOW_PROCESSED_ON_GPU = 1 << 4,
  PIXELPIPE_FLOW_PROCESSED_WITH_TILING = 1 << 5,
  PIXELPIPE_FLOW_BLENDED_ON_CPU = 1 << 6,
  PIXELPIPE_FLOW_BLENDED_ON_GPU = 1 << 7
} dt_pixelpipe_flow_t;

typedef enum dt_pixelpipe_blend_transform_t
{
  DT_DEV_PIXELPIPE_BLEND_TRANSFORM_NONE = 0,
  DT_DEV_PIXELPIPE_BLEND_TRANSFORM_INPUT = 1 << 0,
  DT_DEV_PIXELPIPE_BLEND_TRANSFORM_OUTPUT = 1 << 1
} dt_pixelpipe_blend_transform_t;

/**
 * @brief Tell whether the current pipeline state forbids keeping this module output in cache.
 *
 * @details
 * This aggregates all pipeline-wide and module-local policy switches that turn cache lines into
 * disposable buffers:
 *
 * - pipe re-entry,
 * - pipe-wide cache bypass,
 * - no-cache pipelines,
 * - module-local cache bypass.
 */
static inline gboolean _bypass_cache(const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece)
{
  return pipe && (pipe->reentry || pipe->bypass_cache || pipe->no_cache || (piece && piece->bypass_cache));
}

/**
 * @brief Tell whether cache lookups and published cachelines are allowed for the current pipeline state.
 *
 * @details
 * `DT_DEBUG_NOCACHE_REUSE` is intentionally not handled here. That debug mode is meant to
 * disable writable output reuse, not to disable cache reads entirely. Exact hits on already
 * published cachelines are still valid in that mode, but later writable acquisition must
 * refuse to reopen those cachelines for overwrite.
 */
static inline gboolean _requests_cache(const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece)
{
  return pipe && !_bypass_cache(pipe, piece);
}

/**
 * @brief Drop the writable-reuse snapshot attached to a pipeline piece.
 *
 * @details
 * `piece->cache_entry` is only a hint telling the cache that a previously published
 * cacheline may be rekeyed and fully overwritten on a later pass. If we decide to keep
 * the current module output as a long-term cacheline instead, that hint must disappear
 * completely so no stale pointer, serial, lock or metadata survives the decision.
 */
static inline void _reset_piece_cache_entry(dt_dev_pixelpipe_iop_t *piece)
{
  if(piece == NULL) return;

  memset(&piece->cache_entry, 0, sizeof(piece->cache_entry));
  piece->cache_entry.hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
}

void dt_dev_pixelpipe_debug_dump_module_io(dt_dev_pixelpipe_t *pipe, dt_iop_module_t *module, const char *stage,
                                           gboolean is_cl, const dt_iop_buffer_dsc_t *in_dsc,
                                           const dt_iop_buffer_dsc_t *out_dsc, const dt_iop_roi_t *roi_in,
                                           const dt_iop_roi_t *roi_out, size_t in_bpp, size_t out_bpp,
                                           int cst_before, int cst_after);

dt_pixelpipe_blend_transform_t dt_dev_pixelpipe_transform_for_blend(const dt_iop_module_t *self,
                                                                    const dt_dev_pixelpipe_iop_t *piece,
                                                                    const dt_iop_buffer_dsc_t *output_dsc);

gboolean dt_dev_pixelpipe_cache_gpu_device_buffer(const dt_dev_pixelpipe_t *pipe,
                                                  const dt_pixel_cache_entry_t *cache_entry);
