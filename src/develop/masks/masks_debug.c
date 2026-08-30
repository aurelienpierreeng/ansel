/*
    This file is part of Ansel,
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

/* dt_masks_debug_write_png() is NOT here: compositing the GUI overlay needs cairo, and
 * src/develop is held toolkit-free by tools/check_module_boundaries.sh -- the pixel engine has
 * to stay portable. It lives beside the overlay code it calls, in masks/masks_gui.c. What is
 * left in this file is the toolkit-free half: the rasteriser and the outline dump. */

#include "develop/masks_debug.h"

#include "common/logging.h"
#include "develop/develop.h"
#include "develop/dev_geometry.h"
#include "develop/masks.h"
#include "develop/masks_gui.h"
#include "develop/masks/masks_functions.h"
#include "develop/pixelpipe_hb.h"
#include "system/mem_alloc.h"

#include <math.h>

float *dt_masks_debug_rasterise(dt_develop_t *dev, dt_masks_form_t *form, const int width, const int height)
{
  if(IS_NULL_PTR(dev) || IS_NULL_PTR(form) || width <= 0 || height <= 0) return NULL;

  int32_t raw_width = 0;
  int32_t raw_height = 0;
  if(!dt_dev_geometry_get_raw_size(dev, &raw_width, &raw_height) || raw_width <= 0 || raw_height <= 0)
  {
    dt_print(DT_DEBUG_ALWAYS, "[masks debug] the dev has no raw geometry; nothing to rasterise against\n");
    return NULL;
  }

  float *const buffer = dt_calloc_align_float((size_t)width * height);
  if(IS_NULL_PTR(buffer)) return NULL;

  /* A pipe with no nodes: dt_dev_distort_transform_plus() then walks an empty list, so the
   * composition is the identity and the shape lands on raw coordinates. That is deliberate --
   * a geometry regression must not move because some other module's distortion changed. The
   * three fields dt_masks_distort_for_pipe() reads are the only ones that matter here, plus
   * `forms', which is how a group resolves its children (they are borrowed, not referenced:
   * this pipe does not outlive the call). */
  dt_dev_pixelpipe_t pipe = { 0 };
  pipe.iwidth = raw_width;
  pipe.iheight = raw_height;
  pipe.mask_rasterization_step = 1;
  pipe.forms = dev->forms;

  dt_dev_pixelpipe_iop_t piece = { 0 };
  dt_iop_module_t module = { 0 };
  module.dev = dev;
  module.iop_order = 0.0;

  const dt_iop_roi_t roi = { .x = 0, .y = 0, .width = width, .height = height,
                             .scale = (double)width / (double)raw_width };
  dt_iop_roi_t touched = { 0 };

  const dt_masks_raster_result_t result
      = dt_masks_get_mask_roi(&module, &pipe, &piece, form, &roi, buffer, &touched);

  if(result == DT_MASKS_RASTER_ERROR)
  {
    dt_print(DT_DEBUG_ALWAYS, "[masks debug] rasterising '%s' failed\n", form->name);
    dt_free_align(buffer);
    return NULL;
  }

  // EMPTY is a result, not a failure: the caller gets the zeroed buffer it describes.
  return buffer;
}

gboolean dt_masks_debug_write_outline_csv(dt_develop_t *dev, dt_masks_form_t *form, const char *path)
{
  if(IS_NULL_PTR(dev) || IS_NULL_PTR(form) || IS_NULL_PTR(path)) return FALSE;

  float *points = NULL, *border = NULL;
  int points_count = 0, border_count = 0;
  dt_masks_skip_range_t *skips = NULL;
  int skip_count = 0;

  const dt_masks_raster_result_t st = dt_masks_get_points_border(dev, form, &points, &points_count,
                                                                 &border, &border_count,
                                                                 &skips, &skip_count, 0, NULL);
  dt_print(DT_DEBUG_ALWAYS, "[masks debug] outline: status=%d points=%d border=%d\n", st, points_count, border_count);
  if(st == DT_MASKS_RASTER_ERROR) return FALSE;

  FILE *f = fopen(path, "w");
  if(IS_NULL_PTR(f))
  {
    dt_pixelpipe_cache_free_align(points);
    dt_pixelpipe_cache_free_align(border);
    dt_pixelpipe_cache_free_align(skips);
    return FALSE;
  }

  fprintf(f, "# points=%d border=%d skips=%d\n", points_count, border_count, skip_count);
  for(int i = 0; i < skip_count; i++)
    fprintf(f, "# skip %d..%d\n", skips[i].jump_from, skips[i].resume_at);
  fprintf(f, "i,px,py,bx,by\n");
  const int n = MIN(points_count, border_count);
  for(int i = 0; i < n; i++)
    fprintf(f, "%d,%.4f,%.4f,%.4f,%.4f\n", i, points[i * 2], points[i * 2 + 1],
            border[i * 2], border[i * 2 + 1]);
  fclose(f);

  dt_pixelpipe_cache_free_align(points);
  dt_pixelpipe_cache_free_align(border);
  dt_pixelpipe_cache_free_align(skips);
  return TRUE;
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
