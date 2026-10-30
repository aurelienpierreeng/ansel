/*
    This file is part of the Ansel project.
    Copyright (C) 2026 Aurélien PIERRE.

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

/** @file
 *  @brief The drawlayer's layer store: the layer cache and its sidecar synchronisation.
 */

#ifndef DT_IOP_DRAWLAYER_LAYERS_H
#define DT_IOP_DRAWLAYER_LAYERS_H

#include "iop/drawlayer/common.h"      // dt_iop_drawlayer_gui_data_t
#include "develop/imageop.h"           // dt_iop_module_t
#include "iop/drawlayer/coordinates.h"  // dt_drawlayer_layer_canvas_for_pipe()
#include "common/hash.h"                // dt_hash()
#include "system/macros.h"              // IS_NULL_PTR()

/** @brief What one sidecar directory scan found about a layer. */
typedef struct drawlayer_dir_info_t
{
  gboolean found;
  int index;
  int count;
  uint32_t width;
  uint32_t height;
  char name[DRAWLAYER_NAME_SIZE];
  char work_profile[DRAWLAYER_PROFILE_SIZE];
} drawlayer_dir_info_t;

/* These two are inline ON PURPOSE, the exception the developer notes allow for a header
 * whose published interface IS code: drawlayer.c calls each five times on the per-frame
 * path. A static inline gives both translation units their own copy, so always_inline
 * still holds without relying on link-time optimisation. This header has exactly two
 * includers, which is what keeps that trade proportionate. */
static inline __attribute__((always_inline)) gboolean dt_drawlayer_resolve_layer_geometry(dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                                        const dt_dev_pixelpipe_iop_t *piece, int *layer_width,
                                        int *layer_height, int *origin_x, int *origin_y)
{
  if(!IS_NULL_PTR(layer_width)) *layer_width = 0;
  if(!IS_NULL_PTR(layer_height)) *layer_height = 0;
  if(!IS_NULL_PTR(origin_x)) *origin_x = 0;
  if(!IS_NULL_PTR(origin_y)) *origin_y = 0;
  if(IS_NULL_PTR(self) || IS_NULL_PTR(self->dev)) return FALSE;
  /* The canvas has the RAW image's dimensions, in this image's own orientation, and is centred
   * on the module's frame -- the same for every pipe, every zoom and every crop, which is the
   * point of anchoring to it: a crop changes which window of the canvas a render reads, not
   * where the paint sits. See iop/drawlayer/coordinates.h.
   *
   * It used to be the module's own stage frame, lifted back through roi_out.scale. That size
   * moves with every crop, and the authored raster was then fitted onto the new frame -- the
   * paint moved and stretched with the crop.
   *
   * Thumbnail and export pipes may start from a downscaled mipmap; that shrinks the module's
   * frame, not the canvas, and the placement's scale absorbs it. */
  int resolved_width = 0;
  int resolved_height = 0;
  if(!dt_drawlayer_layer_canvas_for_pipe(self, pipe, &resolved_width, &resolved_height)) return FALSE;

  (void)piece;

  if(!IS_NULL_PTR(layer_width)) *layer_width = resolved_width;
  if(!IS_NULL_PTR(layer_height)) *layer_height = resolved_height;
  return resolved_width > 0 && resolved_height > 0;
}

static inline __attribute__((always_inline)) uint64_t dt_drawlayer_params_cache_hash(const int32_t imgid, const dt_iop_drawlayer_params_t *params,
                                             const int layer_width, const int layer_height)
{
  /* Internal drawlayer base-cache identity must stay stable across transient
   * stroke/hash updates. Key only by image + layer identity + working profile,
   * not by volatile fields (stroke hash, sidecar timestamp...).
   *
   * This keeps the shared base patch line hot across interactive drawing ticks
   * and avoids expensive rekey conflicts/republishing during realtime updates. */
  uint64_t hash = 5381u;
  hash = dt_hash(hash, (const char *)&imgid, sizeof(imgid));
  hash = dt_hash(hash, (const char *)&layer_width, sizeof(layer_width));
  hash = dt_hash(hash, (const char *)&layer_height, sizeof(layer_height));
  if(!IS_NULL_PTR(params))
  {
    hash = dt_hash(hash, params->layer_name, sizeof(params->layer_name));
    hash = dt_hash(hash, (const char *)&params->layer_order, sizeof(params->layer_order));
    hash = dt_hash(hash, params->work_profile, sizeof(params->work_profile));
  }
  return hash ? hash : 1u;
}

void dt_drawlayer_layers_append_error(GString *errors, const char *message);

void dt_drawlayer_layers_log_errors(GString *errors);

void dt_drawlayer_layers_populate_list(dt_iop_module_t *self);

void dt_drawlayer_layers_reset_stroke_session(dt_iop_drawlayer_gui_data_t *g);

#endif
