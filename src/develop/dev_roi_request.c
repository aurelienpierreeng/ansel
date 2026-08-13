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

#include "develop/dev_roi_request.h"
#include "develop/dev_geometry.h"
#include "develop/dev_viewport.h"
#include "develop/develop.h"
#include "develop/pixelpipe_hb.h"

#include <string.h>

/* Same publication protocol as the geometry record and the viewport: one writer (the GUI
 * thread), a generation that is odd mid-publication, readers that copy and retry.
 */

/**
 * @brief Do these two records carry the same numbers, ignoring the generation stamp?
 *
 * @details Member by member, NOT memcmp: the record's uint64_t generation forces 8-byte
 * alignment, so there are four bytes of tail padding after `valid' whose contents C does not
 * define across an assignment. A memcmp would occasionally report a change where none happened,
 * and since the generation is meant to be a cache key, that is not a cosmetic difference -- it
 * is a spurious invalidation of the pipeline cache chain, of exactly the kind this gate exists
 * to prevent.
 *
 * The floats are compared with ==, deliberately: these are copied verbatim from the viewport
 * and the geometry record, never recomputed here, so the question really is "are these the same
 * bits I published last time", not "are these numerically close".
 */
static inline gboolean _payload_equal(const dt_dev_roi_request_t *a, const dt_dev_roi_request_t *b)
{
  return a->box_width == b->box_width
         && a->box_height == b->box_height
         && a->processed_width == b->processed_width
         && a->processed_height == b->processed_height
         && a->preview_width == b->preview_width
         && a->preview_height == b->preview_height
         && a->natural_scale == b->natural_scale
         && a->scaling == b->scaling
         && a->center_x == b->center_x
         && a->center_y == b->center_y
         && a->valid == b->valid;
}

dt_dev_roi_request_t dt_dev_roi_request_neutral(void)
{
  dt_dev_roi_request_t request;
  memset(&request, 0, sizeof(request));
  request.natural_scale = -1.f;
  request.scaling = 1.f;
  request.center_x = 0.5f;
  request.center_y = 0.5f;
  return request;
}

void dt_dev_roi_request_init(dt_develop_t *dev)
{
  if(IS_NULL_PTR(dev)) return;

  dev->roi_request.value = dt_dev_roi_request_neutral();
  dt_atomic_set_uint64(&dev->roi_request.generation, 0);
}

dt_dev_roi_request_t dt_dev_roi_request_get(const dt_develop_t *dev)
{
  dt_dev_roi_request_t request;
  memset(&request, 0, sizeof(request));
  if(IS_NULL_PTR(dev)) return request;

  const dt_dev_roi_request_store_t *store = &dev->roi_request;

  for(;;)
  {
    const uint64_t before = dt_atomic_get_uint64(&store->generation);
    if(before & 1) continue;

    request = store->value;

    const uint64_t after = dt_atomic_get_uint64(&store->generation);
    if(before == after) break;
  }

  return request;
}

uint64_t dt_dev_roi_request_publish(dt_develop_t *dev)
{
  if(IS_NULL_PTR(dev)) return 0;

  // One coherent read of each input, so the request cannot mix two viewport states or two
  // geometries -- the whole point of publishing them as records in the first place.
  const dt_dev_viewport_state_t viewport = dt_dev_viewport_get(dev);
  const dt_dev_image_geometry_t geometry = dt_dev_geometry_snapshot(dev);

  dt_dev_roi_request_t next = dev->roi_request.value;

  next.box_width = viewport.box_width;
  next.box_height = viewport.box_height;
  next.scaling = viewport.scaling;
  next.center_x = viewport.center_x;
  next.center_y = viewport.center_y;
  next.processed_width = geometry.processed_width;
  next.processed_height = geometry.processed_height;

  // dt_dev_get_natural_scale()'s guard, reproduced: no viewport allocation or no raw geometry
  // means there is nothing to fit the image into, and -1 is the sentinel every consumer of the
  // product already handles.
  next.natural_scale = (viewport.configured && geometry.raw_inited)
                           ? dt_dev_roi_natural_scale(viewport.box_width, viewport.box_height,
                                                      geometry.processed_width, geometry.processed_height)
                           : -1.f;

  // roundf(), NOT a truncation: these must match the ROI the worker requests, which rounds too.
  // A truncation here disagreed by 1px whenever the fraction was >= 0.5, which is image-dependent,
  // and silently broke dt_dev_pixelpipe_has_preview_output() -- hence ashift structure detection
  // and drawing -- on some images but not others.
  next.preview_width = roundf(next.natural_scale * next.processed_width);
  next.preview_height = roundf(next.natural_scale * next.processed_height);

  // DERIVED, never asserted. This used to be an unconditional TRUE, which made the record claim
  // it was usable from dt_dev_init() onwards -- before any viewport, any image, any pipe -- and
  // made dt_dev_reset_roi()'s invalidation unobservable, since the viewport reset republishes on
  // the very next line.
  //
  // Each of these three flags is FALSE only while its own numbers are still zero: box_* is
  // written only by dt_dev_viewport_set_box(), which sets `configured' in the same publication;
  // raw_* only by _dt_dev_mipmap_prefetch_full(), which pairs a FALSE with 0x0; processed_* only
  // by dt_dev_geometry_set_processed_size(), which always sets its flag. So a FALSE here always
  // travels with a zero, which is what keeps _darkroom_pipeline_inputs_ready() (it tests the
  // numbers, not the flags) rejecting exactly the states this reports as unusable -- the worker
  // naps and retries instead of reaching _update_darkroom_roi(), whose !valid branch returns
  // without writing its out-params and would leave the caller planning a 0x0 ROI.
  //
  // Do not clear one of these flags without zeroing its numbers, and do not gate this on
  // anything a publication cannot re-derive.
  next.valid = viewport.configured && geometry.raw_inited && geometry.processed_inited;

  // Advance the generation only on a real change, so a consumer can use it as a cache key
  // without being invalidated by every republication of identical numbers.
  const uint64_t published = dt_atomic_get_uint64(&dev->roi_request.generation);
  if(_payload_equal(&dev->roi_request.value, &next)) return published;

  next.generation = published + 2;   // stays even: the store's counter is the odd/even flag
  dt_atomic_set_uint64(&dev->roi_request.generation, published + 1);
  dev->roi_request.value = next;
  dt_atomic_set_uint64(&dev->roi_request.generation, published + 2);

  return next.generation;
}

void dt_dev_roi_request_latch(dt_dev_pixelpipe_t *pipe, const dt_dev_roi_request_t *request)
{
  if(IS_NULL_PTR(pipe) || IS_NULL_PTR(request)) return;

  const uint64_t published = dt_atomic_get_uint64(&pipe->roi_request.generation);
  dt_atomic_set_uint64(&pipe->roi_request.generation, published + 1);
  pipe->roi_request.value = *request;
  dt_atomic_set_uint64(&pipe->roi_request.generation, published + 2);
}

dt_dev_roi_request_t dt_dev_roi_request_of_pipe(const dt_dev_pixelpipe_t *pipe)
{
  dt_dev_roi_request_t request = dt_dev_roi_request_neutral();
  if(IS_NULL_PTR(pipe)) return request;

  for(;;)
  {
    const uint64_t before = dt_atomic_get_uint64(&pipe->roi_request.generation);
    if(before & 1) continue;

    request = pipe->roi_request.value;

    const uint64_t after = dt_atomic_get_uint64(&pipe->roi_request.generation);
    if(before == after) break;
  }

  return request;
}

int32_t dt_dev_roi_request_preview_width(const dt_develop_t *dev)
{
  return dt_dev_roi_request_get(dev).preview_width;
}

int32_t dt_dev_roi_request_preview_height(const dt_develop_t *dev)
{
  return dt_dev_roi_request_get(dev).preview_height;
}

float dt_dev_roi_request_natural_scale(const dt_develop_t *dev)
{
  return dt_dev_roi_request_get(dev).natural_scale;
}

gboolean dt_dev_roi_request_valid(const dt_develop_t *dev)
{
  return dt_dev_roi_request_get(dev).valid;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
