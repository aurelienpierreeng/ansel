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

#include "develop/dev_viewport.h"
#include "develop/dev_roi_request.h"
#include "develop/develop.h"

#include <string.h>

/* The publication protocol is the geometry record's, for the same reason: the GUI thread is
 * the only writer, while the darkroom worker and drawlayer's paint thread read. A mutator
 * builds the whole next state in a local, compares it, and publishes it in one bracketed
 * store -- so "half the old pan and half the new" is not a state a reader can observe.
 */

static inline void _publish(dt_dev_viewport_t *viewport, const dt_dev_viewport_state_t *next)
{
  dt_atomic_set_uint64(&viewport->generation, dt_atomic_get_uint64(&viewport->generation) + 1);
  viewport->state = *next;
  dt_atomic_set_uint64(&viewport->generation, dt_atomic_get_uint64(&viewport->generation) + 1);
}

dt_dev_viewport_state_t dt_dev_viewport_neutral(void)
{
  // Not a zeroed struct: these are the values dt_dev_reset_roi() left on every dev, including
  // headless ones, before the viewport was an object. A dev with no viewport must keep reading
  // them, or every consumer of the product (scaling * natural_scale) changes answer.
  dt_dev_viewport_state_t state;
  memset(&state, 0, sizeof(state));
  state.scaling = 1.f;
  state.center_x = 0.5f;
  state.center_y = 0.5f;
  return state;
}

dt_dev_viewport_t *dt_dev_viewport_new(void)
{
  dt_dev_viewport_t *viewport = (dt_dev_viewport_t *)calloc(1, sizeof(dt_dev_viewport_t));
  if(IS_NULL_PTR(viewport)) return NULL;

  viewport->state = dt_dev_viewport_neutral();
  dt_atomic_set_uint64(&viewport->generation, 0);
  return viewport;
}

void dt_dev_viewport_free(dt_dev_viewport_t *viewport)
{
  dt_free(viewport);
}

gboolean dt_dev_viewport_exists(const dt_develop_t *dev)
{
  return !IS_NULL_PTR(dev) && !IS_NULL_PTR(dev->viewport);
}

dt_dev_viewport_state_t dt_dev_viewport_get(const dt_develop_t *dev)
{
  if(!dt_dev_viewport_exists(dev)) return dt_dev_viewport_neutral();

  const dt_dev_viewport_t *viewport = dev->viewport;
  dt_dev_viewport_state_t state;

  // Seqlock read: copy, then check nothing was published while we copied.
  for(;;)
  {
    const uint64_t before = dt_atomic_get_uint64(&viewport->generation);
    if(before & 1) continue;

    state = viewport->state;

    const uint64_t after = dt_atomic_get_uint64(&viewport->generation);
    if(before == after) break;
  }

  return state;
}

float dt_dev_viewport_scaling(const dt_develop_t *dev) { return dt_dev_viewport_get(dev).scaling; }
float dt_dev_viewport_center_x(const dt_develop_t *dev) { return dt_dev_viewport_get(dev).center_x; }
float dt_dev_viewport_center_y(const dt_develop_t *dev) { return dt_dev_viewport_get(dev).center_y; }
int32_t dt_dev_viewport_box_width(const dt_develop_t *dev) { return dt_dev_viewport_get(dev).box_width; }
int32_t dt_dev_viewport_box_height(const dt_develop_t *dev) { return dt_dev_viewport_get(dev).box_height; }
int32_t dt_dev_viewport_widget_width(const dt_develop_t *dev) { return dt_dev_viewport_get(dev).widget_width; }
int32_t dt_dev_viewport_widget_height(const dt_develop_t *dev) { return dt_dev_viewport_get(dev).widget_height; }
int32_t dt_dev_viewport_border_size(const dt_develop_t *dev) { return dt_dev_viewport_get(dev).border_size; }
gboolean dt_dev_viewport_configured(const dt_develop_t *dev) { return dt_dev_viewport_get(dev).configured; }

gboolean dt_dev_viewport_set_widget_size(dt_develop_t *dev, const int32_t widget_width,
                                         const int32_t widget_height)
{
  if(!dt_dev_viewport_exists(dev)) return FALSE;

  dt_dev_viewport_state_t next = dt_dev_viewport_get(dev);
  if(next.widget_width == widget_width && next.widget_height == widget_height) return FALSE;

  next.widget_width = widget_width;
  next.widget_height = widget_height;
  _publish(dev->viewport, &next);
  // The ROI request is derived from this state; republish it so the two cannot disagree.
  dt_dev_roi_request_publish(dev);
  return TRUE;
}

gboolean dt_dev_viewport_set_border(dt_develop_t *dev, const int32_t border_size)
{
  if(!dt_dev_viewport_exists(dev)) return FALSE;

  dt_dev_viewport_state_t next = dt_dev_viewport_get(dev);
  if(next.border_size == border_size) return FALSE;

  next.border_size = border_size;
  _publish(dev->viewport, &next);
  // The ROI request is derived from this state; republish it so the two cannot disagree.
  dt_dev_roi_request_publish(dev);
  return TRUE;
}

gboolean dt_dev_viewport_set_box(dt_develop_t *dev, const int32_t box_width, const int32_t box_height)
{
  if(!dt_dev_viewport_exists(dev)) return FALSE;

  dt_dev_viewport_state_t next = dt_dev_viewport_get(dev);
  if(next.box_width == box_width && next.box_height == box_height && next.configured) return FALSE;

  next.box_width = box_width;
  next.box_height = box_height;
  next.configured = TRUE;
  _publish(dev->viewport, &next);
  // The ROI request is derived from this state; republish it so the two cannot disagree.
  dt_dev_roi_request_publish(dev);
  return TRUE;
}

gboolean dt_dev_viewport_set_center(dt_develop_t *dev, const float center_x, const float center_y)
{
  if(!dt_dev_viewport_exists(dev)) return FALSE;

  dt_dev_viewport_state_t next = dt_dev_viewport_get(dev);
  if(next.center_x == center_x && next.center_y == center_y) return FALSE;

  // The pair is published together, which is the whole point: the two coordinates were eight
  // independent stores before, and the worker read them as two independent loads.
  next.center_x = center_x;
  next.center_y = center_y;
  _publish(dev->viewport, &next);
  // The ROI request is derived from this state; republish it so the two cannot disagree.
  dt_dev_roi_request_publish(dev);
  return TRUE;
}

gboolean dt_dev_viewport_set_scaling(dt_develop_t *dev, const float scaling)
{
  if(!dt_dev_viewport_exists(dev)) return FALSE;

  dt_dev_viewport_state_t next = dt_dev_viewport_get(dev);
  if(next.scaling == scaling) return FALSE;

  next.scaling = scaling;
  _publish(dev->viewport, &next);
  // The ROI request is derived from this state; republish it so the two cannot disagree.
  dt_dev_roi_request_publish(dev);
  return TRUE;
}

void dt_dev_viewport_reset(dt_develop_t *dev)
{
  if(!dt_dev_viewport_exists(dev)) return;

  const dt_dev_viewport_state_t neutral = dt_dev_viewport_neutral();
  dt_dev_viewport_state_t next = dt_dev_viewport_get(dev);
  next.scaling = neutral.scaling;
  next.center_x = neutral.center_x;
  next.center_y = neutral.center_y;
  _publish(dev->viewport, &next);
  dt_dev_roi_request_publish(dev);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
