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

#include "develop/dev_geometry.h"
#include "develop/develop.h"

#include <string.h>

/* The seqlock, both halves in one place.
 *
 * Writers are serialised by convention, not by a lock: the raw pair is published by the one
 * function that loads the image, the processed pair by the one function that runs the virtual
 * pipe, and both run on whichever thread owns that dev. Two concurrent writers would break
 * this, so if a second publisher ever appears it needs a real lock, not a second odd counter.
 */

static inline void _publish_begin(dt_dev_geometry_store_t *store)
{
  dt_atomic_set_uint64(&store->generation, dt_atomic_get_uint64(&store->generation) + 1);
}

static inline void _publish_end(dt_dev_geometry_store_t *store)
{
  dt_atomic_set_uint64(&store->generation, dt_atomic_get_uint64(&store->generation) + 1);
}

void dt_dev_geometry_init(dt_develop_t *dev)
{
  if(IS_NULL_PTR(dev)) return;

  memset(&dev->geometry.value, 0, sizeof(dev->geometry.value));
  dt_atomic_set_uint64(&dev->geometry.generation, 0);
}

void dt_dev_geometry_set_raw_size(dt_develop_t *dev, const int32_t width, const int32_t height,
                                  const gboolean valid)
{
  if(IS_NULL_PTR(dev)) return;

  _publish_begin(&dev->geometry);
  dev->geometry.value.raw_width = width;
  dev->geometry.value.raw_height = height;
  dev->geometry.value.raw_inited = valid;
  _publish_end(&dev->geometry);
}

void dt_dev_geometry_set_processed_size(dt_develop_t *dev, const int32_t width, const int32_t height)
{
  if(IS_NULL_PTR(dev)) return;

  _publish_begin(&dev->geometry);
  dev->geometry.value.processed_width = width;
  dev->geometry.value.processed_height = height;
  dev->geometry.value.processed_inited = TRUE;
  _publish_end(&dev->geometry);
}

gboolean dt_dev_geometry_get(const dt_develop_t *dev, dt_dev_image_geometry_t *out)
{
  if(IS_NULL_PTR(out)) return FALSE;

  memset(out, 0, sizeof(*out));
  if(IS_NULL_PTR(dev)) return FALSE;

  const dt_dev_geometry_store_t *store = &dev->geometry;

  // Retry until we read a settled generation twice with no write in between. A publication is
  // three stores between two counter bumps, so this converges immediately unless a writer is
  // running right now; there is no waiting and no lock to order against anything.
  for(;;)
  {
    const uint64_t before = dt_atomic_get_uint64(&store->generation);
    if(before & 1) continue;   // publication in flight

    *out = store->value;

    const uint64_t after = dt_atomic_get_uint64(&store->generation);
    if(before == after) break;
  }

  return out->raw_inited || out->processed_inited;
}

dt_dev_image_geometry_t dt_dev_geometry_snapshot(const dt_develop_t *dev)
{
  dt_dev_image_geometry_t geometry;
  dt_dev_geometry_get(dev, &geometry);
  return geometry;
}

gboolean dt_dev_geometry_get_raw_size(const dt_develop_t *dev, int32_t *width, int32_t *height)
{
  const dt_dev_image_geometry_t geometry = dt_dev_geometry_snapshot(dev);

  if(!IS_NULL_PTR(width)) *width = geometry.raw_width;
  if(!IS_NULL_PTR(height)) *height = geometry.raw_height;

  return geometry.raw_inited;
}

gboolean dt_dev_geometry_get_processed_size(const dt_develop_t *dev, int32_t *width, int32_t *height)
{
  const dt_dev_image_geometry_t geometry = dt_dev_geometry_snapshot(dev);

  if(!IS_NULL_PTR(width)) *width = geometry.processed_width;
  if(!IS_NULL_PTR(height)) *height = geometry.processed_height;

  return geometry.processed_inited;
}

int32_t dt_dev_geometry_raw_width(const dt_develop_t *dev)
{
  return dt_dev_geometry_snapshot(dev).raw_width;
}

int32_t dt_dev_geometry_raw_height(const dt_develop_t *dev)
{
  return dt_dev_geometry_snapshot(dev).raw_height;
}

int32_t dt_dev_geometry_processed_width(const dt_develop_t *dev)
{
  return dt_dev_geometry_snapshot(dev).processed_width;
}

int32_t dt_dev_geometry_processed_height(const dt_develop_t *dev)
{
  return dt_dev_geometry_snapshot(dev).processed_height;
}

gboolean dt_dev_geometry_raw_inited(const dt_develop_t *dev)
{
  return dt_dev_geometry_snapshot(dev).raw_inited;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
