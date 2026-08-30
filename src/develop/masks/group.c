/*
    This file is part of darktable,
    Copyright (C) 2013 Aldric Renaudin.
    Copyright (C) 2013 Simon Spannagel.
    Copyright (C) 2013-2016 Tobias Ellinghaus.
    Copyright (C) 2013-2014, 2019 Ulrich Pegelow.
    Copyright (C) 2014, 2016 Roman Lebedev.
    Copyright (C) 2016, 2019-2021 Pascal Obry.
    Copyright (C) 2018 Edgardo Hoszowski.
    Copyright (C) 2018 johannes hanika.
    Copyright (C) 2019 Andreas Schneider.
    Copyright (C) 2019 Heiko Bauke.
    Copyright (C) 2020 GrahamByrnes.
    Copyright (C) 2020 Hubert Kowalski.
    Copyright (C) 2020-2021 Ralf Brown.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Miloš Komarčević.
    Copyright (C) 2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2025-2026 Guillaume Stutin.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    
    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "common/hash.h"   // dt_hash()
#include "system/macros.h"
#include "system/openmp.h"
#include "system/mem_alloc.h"
#include "common/logging.h"
#include "common/times.h"
#include "caches/pixelpipe_cache_alloc.h"
#include "widgets/gdkkeys.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "develop/masks_gui.h"
#include "develop/masks/masks_functions.h"
#include "develop/masks/masks_touched.h"

/* Shape handlers receive widget-space coordinates, while normalized output-image
 * coordinates come from `gui->rel_pos` and absolute output-image
 * coordinates come from `gui->pos`. */
// Centralize group child lookup so all dispatchers and expose code resolve the same
// selected child the same way.
static dt_masks_form_t *_group_get_child_at(dt_develop_t *dev, dt_masks_form_t *form, const int group_index,
                                            dt_masks_form_group_t **group_entry)
{
  dt_masks_form_group_t *entry = (dt_masks_form_group_t *)g_list_nth_data(form->points, group_index);
  if(IS_NULL_PTR(entry)) return NULL;
  if(group_entry) *group_entry = entry;
  return dt_masks_get_from_id(dev, entry->formid);
}

static dt_masks_form_t *_group_get_selected_child(dt_masks_form_t *form, dt_masks_form_gui_t *gui,
                                                  dt_masks_form_group_t **group_entry)
{
  if(gui->group_selected < 0) return NULL;
  return _group_get_child_at(gui->dev, form, gui->group_selected, group_entry);
}

static int _group_events_mouse_scrolled(struct dt_iop_module_t *module, double x, double y, int up, const int flow,
                                        uint32_t state, dt_masks_form_t *form, int unused1, dt_masks_form_gui_t *gui,
                                        int unused, dt_masks_interaction_t interaction)
{
  return 0;
}

static gboolean _group_events_button_pressed(struct dt_iop_module_t *module, double x, double y,
                                        double pressure, int which, int type, uint32_t state,
                                        dt_masks_form_t *form, int unused1, dt_masks_form_gui_t *gui, int unused2)
{
  return FALSE;
}

static int _group_events_button_released(struct dt_iop_module_t *module, double x, double y, int which,
                                         uint32_t state, dt_masks_form_t *form, int unused1, dt_masks_form_gui_t *gui,
                                         int unused2)
{
  return 0;
}

static int _group_events_key_pressed(struct dt_iop_module_t *module, GdkEventKey *event, dt_masks_form_t *form, int parentid, dt_masks_form_gui_t *gui, int index)
{
  if(IS_NULL_PTR(form)) return 0;

  gboolean return_value = FALSE;
  guint key = dt_keys_mainpad_alternatives(event->keyval);

  // Global key bindings for groups
  if(!return_value)
  {
    switch(key)
    {
      case GDK_KEY_Escape:
      {
        return_value = dt_masks_form_exit_creation(module, gui);
        break;
      }
      case GDK_KEY_Delete:
      {
        if(gui->group_selected >= 0)
        {
          // Remove shape from current group
          dt_masks_form_group_t *group_entry = NULL;
          dt_masks_form_t *selected_form = _group_get_selected_child(form, gui, &group_entry);
          if(IS_NULL_PTR(selected_form)) return 0;
          return_value = dt_masks_gui_remove(module, selected_form, gui, group_entry->parentid);
          break;
        }
      }
    }
  }
  
  return return_value;
}

static int _group_events_mouse_moved(struct dt_iop_module_t *module, double x, double y, double pressure,
                                     int which, dt_masks_form_t *form, int unused1, dt_masks_form_gui_t *gui,
                                     int unused2)
{
  return 0;
}

static void _group_events_post_expose_draw(cairo_t *cr, float zoom_scale, dt_masks_form_t *form,
                                          dt_masks_form_gui_t *gui, int pos)
{
  dt_masks_form_t *selected_form = _group_get_child_at(gui->dev, form, pos, NULL);
  if(selected_form && selected_form->functions && selected_form->functions->post_expose)
  {
    /* Timed per shape, with the size of what is being stroked.
     *
     * The overlay redraw is the expensive half of a mask-heavy darkroom and nothing measured it:
     * on the #1158 logs the darkroom expose runs 270-390 ms with only ~15 ms of it accounted for
     * by the outline rebuilds around it, and the unaccounted time falls BETWEEN consecutive
     * shapes -- which is this call. Whether that is the cairo stroking, and whether it scales
     * with the point count or with the node count (dt_masks_draw_path_seg_by_seg() strokes once
     * per node), is exactly what these two numbers separate. */
    const dt_times_t start = { 0 };
    dt_get_times((dt_times_t *)&start);

    gui->type = selected_form->type;
    selected_form->functions->post_expose(cr, zoom_scale, gui, pos, g_list_length(selected_form->points));

    if(dt_get_debug_flags() & DT_DEBUG_MASKS)
    {
      const dt_masks_form_gui_points_t *const gui_points
          = (const dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, pos);
      dt_show_times_f(&start, "[masks]", "shape %d (%s) drawn: %d outline points, %d border points, %d nodes",
                      pos, selected_form->name, IS_NULL_PTR(gui_points) ? -1 : gui_points->points_count,
                      IS_NULL_PTR(gui_points) ? -1 : gui_points->border_count,
                      g_list_length(selected_form->points));
    }
  }
}

void dt_group_events_post_expose(cairo_t *cr, float zoom_scale, dt_masks_form_t *form,
                                 dt_masks_form_gui_t *gui)
{
  int pos = 0;
  // draw the selected form last so it's drawn on top of the others.
  // we loop over all forms and skip the selected one
  for(GList *fpts = form->points; fpts; fpts = g_list_next(fpts))
  {
    // skip drawing for the selected one
    if(gui->group_selected != pos)
      _group_events_post_expose_draw(cr, zoom_scale, form, gui, pos);
    
    pos++;
  }
  // now draw the selected one on top, if any
  if(gui->group_selected >= 0)
    _group_events_post_expose_draw(cr, zoom_scale, form, gui, gui->group_selected);
}

static int _inverse_mask(const dt_iop_module_t *const module, const dt_dev_pixelpipe_iop_t *const piece,
                         dt_masks_form_t *const form,
                         float **buffer, int *width, int *height, int *posx, int *posy)
{
  // we create a new buffer
  const int wt = piece->iwidth;
  const int ht = piece->iheight;
  float *buf = dt_pixelpipe_cache_alloc_align_float_cache((size_t)ht * wt, 0);
  if(IS_NULL_PTR(buf)) return 1;

  // we fill this buffer
  const int posx_ = *posx;
  const int posy_ = *posy;
  const int width_ = *width;
  const int height_ = *height;
  const float *const src = *buffer;
  __OMP_PARALLEL_FOR__(if(wt * ht > 50000))
  for(int yy = 0; yy < MIN(posy_, ht); yy++)
  {
    float *const row = buf + (size_t)yy * wt;
    for(int xx = 0; xx < wt; xx++) row[xx] = 1.0f;
  }
  __OMP_PARALLEL_FOR__(if(wt * ht > 50000))
  for(int yy = MAX(posy_, 0); yy < MIN(ht, posy_ + height_); yy++)
  {
    float *const row = buf + (size_t)yy * wt;
    for(int xx = 0; xx < MIN(posx_, wt); xx++) row[xx] = 1.0f;
    const int xstart = MAX(posx_, 0);
    const int xend = MIN(wt, posx_ + width_);
    const float *const src_row = src + (size_t)(yy - posy_) * width_;
    for(int xx = xstart; xx < xend; xx++)
      row[xx] = 1.0f - src_row[xx - posx_];
    for(int xx = MAX(posx_ + width_, 0); xx < wt; xx++) row[xx] = 1.0f;
  }
  __OMP_PARALLEL_FOR__(if(wt * ht > 50000))
  for(int yy = MAX(posy_ + height_, 0); yy < ht; yy++)
  {
    float *const row = buf + (size_t)yy * wt;
    for(int xx = 0; xx < wt; xx++) row[xx] = 1.0f;
  }

  // we free the old buffer
  dt_pixelpipe_cache_free_align(*buffer);
  (*buffer) = buf;

  // we return correct values for positions;
  *posx = *posy = 0;
  *width = wt;
  *height = ht;
  return 0;
}

static dt_masks_raster_result_t _group_get_mask(const dt_iop_module_t *const module, dt_dev_pixelpipe_t *pipe,
                           const dt_dev_pixelpipe_iop_t *const piece,
                           dt_masks_form_t *const form,
                           float **buffer, int *width, int *height, int *posx, int *posy)
{
  *buffer = NULL;
  *width = 0;
  *height = 0;
  *posx = 0;
  *posy = 0;

  // we allocate buffers and values
  const guint nb = g_list_length(form->points);
  if(nb == 0) return DT_MASKS_RASTER_EMPTY;
  float **bufs = dt_calloc_align(nb * sizeof(float *));
  int *w = dt_alloc_align(sizeof(int) * nb);
  int *h = dt_alloc_align(sizeof(int) * nb);
  int *px = dt_alloc_align(sizeof(int) * nb);
  int *py = dt_alloc_align(sizeof(int) * nb);
  int *states = dt_alloc_align(sizeof(int) * nb);
  float *op = dt_alloc_align_float(nb);
  if(IS_NULL_PTR(bufs) || IS_NULL_PTR(w) || IS_NULL_PTR(h) || IS_NULL_PTR(px) || IS_NULL_PTR(py) || IS_NULL_PTR(states) || IS_NULL_PTR(op))
  {
    dt_free_align(op);
    dt_free_align(states);
    dt_free_align(py);
    dt_free_align(px);
    dt_free_align(h);
    dt_free_align(w);
    dt_free_align(bufs);
    return DT_MASKS_RASTER_ERROR;
  }

  /* Slots are filled DENSELY: `nb_ok' is both the count of usable children and the index of the
   * next one. A child that resolves to nothing -- an id that is gone, or a shape with no geometry
   * -- simply does not take a slot. That matters because the bounding-box and copy loops below
   * read every slot they iterate: indexing by list position instead left a hole whose px/py/w/h
   * were never written (the arrays are malloc'd, not calloc'd), and one unresolved id was enough
   * to drag the group's bounding box to garbage. */
  int nb_ok = 0;
  dt_masks_raster_result_t err = DT_MASKS_RASTER_OK;
  for(GList *fpts = form->points; fpts; fpts = g_list_next(fpts))
  {
    dt_masks_form_group_t *fpt = (dt_masks_form_group_t *)fpts->data;
    dt_masks_form_t *sel = dt_masks_get_from_id(module->dev, fpt->formid);
    if(sel)
    {
      const dt_masks_raster_result_t child
          = dt_masks_get_mask(module, pipe, piece, sel, &bufs[nb_ok], &w[nb_ok], &h[nb_ok],
                              &px[nb_ok], &py[nb_ok]);
      if(child == DT_MASKS_RASTER_ERROR)
      {
        err = DT_MASKS_RASTER_ERROR;
        break;
      }
      /* A shape with nothing to draw contributes nothing and must NOT stop the fold: the other
       * members of the group are still theirs to render. It takes no slot, so the loops below
       * never see it. (dt_masks_get_mask() has already zeroed this slot's out-parameters.) */
      if(child == DT_MASKS_RASTER_EMPTY) continue;
      if(fpt->state & DT_MASKS_STATE_INVERSE)
      {
        const double start = dt_get_wtime();
        if(_inverse_mask(module, piece, sel, &bufs[nb_ok], &w[nb_ok], &h[nb_ok], &px[nb_ok], &py[nb_ok]) != 0)
        {
          err = DT_MASKS_RASTER_ERROR;
          break;
        }
        if(dt_get_debug_flags() & DT_DEBUG_PERF)
          dt_print(DT_DEBUG_MASKS, "[masks %s] inverse took %0.04f sec\n", sel->name, dt_get_wtime() - start);
      }
      op[nb_ok] = fpt->opacity;
      states[nb_ok] = fpt->state;
      nb_ok++;
    }
  }
  if(err) goto cleanup;
  if(nb_ok == 0)
  {
    *buffer = NULL;
    *width = 0;
  *height = 0;
  *posx = 0;
  *posy = 0;
    goto cleanup;
  }

  // now we get the min, max, width, height of the final mask
  int l = INT_MAX, r = INT_MIN, t = INT_MAX, b = INT_MIN;
  for(int i = 0; i < nb_ok; i++)
  {
    l = MIN(l, px[i]);
    t = MIN(t, py[i]);
    r = MAX(r, px[i] + w[i]);
    b = MAX(b, py[i] + h[i]);
  }
  *posx = l;
  *posy = t;
  *width = r - l;
  *height = b - t;

  // we allocate the buffer
  *buffer = dt_pixelpipe_cache_alloc_align_float_cache((size_t)(r - l) * (b - t), 0);
  if(IS_NULL_PTR(*buffer))
  {
    err = DT_MASKS_RASTER_ERROR;
    goto cleanup;
  }

  // and we copy each buffer inside, row by row
  const int dst_w = r - l;
  const int dst_h = b - t;
  float *const dst = *buffer;
  for(int i = 0; i < nb_ok; i++)
  {
    const double start = dt_get_wtime();
    const int wi = w[i];
    const int hi = h[i];
    const int ox = px[i] - l;
    const int oy = py[i] - t;
    const float opacity = op[i];
    const float *const src = bufs[i];
    if(states[i] & DT_MASKS_STATE_UNION)
    {
      __OMP_PARALLEL_FOR__(if((size_t)wi * hi > 10000))
      for(int y = 0; y < hi; y++)
      {
        float *const dst_row = dst + (size_t)(oy + y) * dst_w + ox;
        const float *const src_row = src + (size_t)y * wi;
        for(int x = 0; x < wi; x++)
        {
          const float v = src_row[x] * opacity;
          if(v > dst_row[x]) dst_row[x] = v;
        }
      }
    }
    else if(states[i] & DT_MASKS_STATE_INTERSECTION)
    {
      const int x0 = MAX(px[i], l);
      const int y0 = MAX(py[i], t);
      const int x1 = MIN(px[i] + wi, r);
      const int y1 = MIN(py[i] + hi, b);
      if(x0 >= x1 || y0 >= y1)
      {
        memset(dst, 0, (size_t)dst_w * dst_h * sizeof(float));
      }
      else
      {
        const int row_start = y0 - t;
        const int row_end = y1 - t;
        const int col_start = x0 - l;
        const int col_end = x1 - l;
        const int src_x_offset = x0 - px[i];
        const int src_y_offset = t - py[i];
        __OMP_PARALLEL_FOR__(if((size_t)dst_w * dst_h > 10000))
        for(int y = 0; y < dst_h; y++)
        {
          float *const dst_row = dst + (size_t)y * dst_w;
          if(y < row_start || y >= row_end)
          {
            memset(dst_row, 0, (size_t)dst_w * sizeof(float));
            continue;
          }

          const int src_y = y + src_y_offset;
          const float *const src_row = src + (size_t)src_y * wi + src_x_offset;
          float *const dst_mid = dst_row + col_start;
          const int mid_w = col_end - col_start;
          for(int x = 0; x < mid_w; x++)
          {
            const float b1 = dst_mid[x];
            const float b2 = src_row[x];
            if(b1 > 0.0f && b2 > 0.0f)
              dst_mid[x] = fminf(b1, b2 * opacity);
            else
              dst_mid[x] = 0.0f;
          }

          if(col_start > 0) memset(dst_row, 0, (size_t)col_start * sizeof(float));
          if(col_end < dst_w) memset(dst_row + col_end, 0, (size_t)(dst_w - col_end) * sizeof(float));
        }
      }
    }
    else if(states[i] & DT_MASKS_STATE_DIFFERENCE)
    {
      __OMP_PARALLEL_FOR__(if((size_t)wi * hi > 10000))
      for(int y = 0; y < hi; y++)
      {
        float *const dst_row = dst + (size_t)(oy + y) * dst_w + ox;
        const float *const src_row = src + (size_t)y * wi;
        for(int x = 0; x < wi; x++)
        {
          const float b1 = dst_row[x];
          const float b2 = src_row[x] * opacity;
          if(b1 > 0.0f && b2 > 0.0f) dst_row[x] = b1 * (1.0f - b2);
        }
      }
    }
    else if(states[i] & DT_MASKS_STATE_EXCLUSION)
    {
      __OMP_PARALLEL_FOR__(if((size_t)wi * hi > 10000))
      for(int y = 0; y < hi; y++)
      {
        float *const dst_row = dst + (size_t)(oy + y) * dst_w + ox;
        const float *const src_row = src + (size_t)y * wi;
        for(int x = 0; x < wi; x++)
        {
          const float b1 = dst_row[x];
          const float b2 = src_row[x] * opacity;
          if(b1 > 0.0f && b2 > 0.0f)
            dst_row[x] = fmaxf((1.0f - b1) * b2, b1 * (1.0f - b2));
          else
            dst_row[x] = fmaxf(dst_row[x], b2);
        }
      }
    }
    else // if we are here, this mean that we just have to copy the shape and null other parts
    {
      const int x0 = MAX(px[i], l);
      const int y0 = MAX(py[i], t);
      const int x1 = MIN(px[i] + wi, r);
      const int y1 = MIN(py[i] + hi, b);
      if(x0 >= x1 || y0 >= y1)
      {
        memset(dst, 0, (size_t)dst_w * dst_h * sizeof(float));
      }
      else
      {
        const int row_start = y0 - t;
        const int row_end = y1 - t;
        const int col_start = x0 - l;
        const int col_end = x1 - l;
        const int src_x_offset = x0 - px[i];
        const int src_y_offset = t - py[i];
        __OMP_PARALLEL_FOR__(if((size_t)dst_w * dst_h > 10000))
        for(int y = 0; y < dst_h; y++)
        {
          float *const dst_row = dst + (size_t)y * dst_w;
          if(y < row_start || y >= row_end)
          {
            memset(dst_row, 0, (size_t)dst_w * sizeof(float));
            continue;
          }

          const int src_y = y + src_y_offset;
          const float *const src_row = src + (size_t)src_y * wi + src_x_offset;
          float *const dst_mid = dst_row + col_start;
          const int mid_w = col_end - col_start;
          for(int x = 0; x < mid_w; x++)
          {
            dst_mid[x] = src_row[x] * opacity;
          }

          if(col_start > 0) memset(dst_row, 0, (size_t)col_start * sizeof(float));
          if(col_end < dst_w) memset(dst_row + col_end, 0, (size_t)(dst_w - col_end) * sizeof(float));
        }
      }
    }

    if(dt_get_debug_flags() & DT_DEBUG_PERF)
      dt_print(DT_DEBUG_MASKS, "[masks %d] combine took %0.04f sec\n", i, dt_get_wtime() - start);
  }

cleanup:
  dt_free_align(op);
  dt_free_align(states);
  dt_free_align(py);
  dt_free_align(px);
  dt_free_align(h);
  dt_free_align(w);
  for(int i = 0; i < nb; i++) dt_pixelpipe_cache_free_align(bufs[i]);
  dt_free_align(bufs);
  return err;
}

static void _combine_masks_union(float *const restrict dest, float *const restrict newmask, const size_t npixels,
                                 const float opacity, const int inverted)
{
  if (inverted)
  {
    __OMP_FOR_SIMD__(aligned(dest, newmask : 64)  if(npixels > 10000))
    for(int index = 0; index < npixels; index++)
    {
      const float mask = opacity * (1.0f - newmask[index]);
      dest[index] = MAX(dest[index], mask);
    }
  }
  else
  {
    __OMP_FOR_SIMD__(aligned(dest, newmask : 64)  if(npixels > 10000))
    for(int index = 0; index < npixels; index++)
    {
      const float mask = opacity * newmask[index];
      dest[index] = MAX(dest[index], mask);
    }
  }
}

/** Union bounded to `box' (buffer-relative): outside it the child is zero and max(dest, 0) is dest. */
static void _combine_masks_union_box(float *const restrict dest, const float *const restrict newmask,
                                     const int width, const dt_iop_roi_t *const box, const float opacity)
{
  for(int y = box->y; y < box->y + box->height; y++)
  {
    float *const restrict dest_row = dest + (size_t)y * width + box->x;
    const float *const restrict src_row = newmask + (size_t)y * width + box->x;
    for(int x = 0; x < box->width; x++)
    {
      const float mask = opacity * src_row[x];
      dest_row[x] = MAX(dest_row[x], mask);
    }
  }
}

static void _combine_masks_intersect(float *const restrict dest, float *const restrict newmask, const size_t npixels,
                                     const float opacity, const int inverted)
{
  if (inverted)
  {
    __OMP_FOR_SIMD__(aligned(dest, newmask : 64)  if(npixels > 10000))
    for(int index = 0; index < npixels; index++)
    {
      const float mask = opacity * (1.0f - newmask[index]);
      dest[index] = MIN(MAX(dest[index], 0.0f), MAX(mask, 0.0f));
    }
  }
  else
  {
    __OMP_FOR_SIMD__(aligned(dest, newmask : 64)  if(npixels > 10000))
    for(int index = 0; index < npixels; index++)
    {
      const float mask = opacity * newmask[index];
      dest[index] = MIN(MAX(dest[index], 0.0f), MAX(mask, 0.0f));
    }
  }
}

__OMP_DECLARE_SIMD__()
static inline int both_positive(const float val1, const float val2)
{
  // this needs to be a separate inline function to convince the compiler to vectorize
  return (val1 > 0.0f) && (val2 > 0.0f);
}

static void _combine_masks_difference(float *const restrict dest, float *const restrict newmask, const size_t npixels,
                                      const float opacity, const int inverted)
{
  if (inverted)
  {
    __OMP_FOR_SIMD__(aligned(dest, newmask : 64)  if(npixels > 10000))
    for(int index = 0; index < npixels; index++)
    {
      const float mask = opacity * (1.0f - newmask[index]);
      dest[index] *= (1.0f - mask * both_positive(dest[index],mask));
    }
  }
  else
  {
__OMP_FOR_SIMD__(aligned(dest, newmask : 64)  if(npixels > 10000)
)
    for(int index = 0; index < npixels; index++)
    {
      const float mask = opacity * newmask[index];
      dest[index] *= (1.0f - mask * both_positive(dest[index],mask));
    }
  }
}

static void _combine_masks_exclusion(float *const restrict dest, float *const restrict newmask, const size_t npixels,
                                     const float opacity, const int inverted)
{
  if (inverted)
  {
__OMP_FOR_SIMD__(aligned(dest, newmask : 64)  if(npixels > 10000)
)
    for(int index = 0; index < npixels; index++)
    {
      const float mask = opacity * (1.0f - newmask[index]);
      const float pos = both_positive(dest[index], mask);
      const float neg = (1.0f - pos);
      const float b1 = dest[index];
      dest[index] = pos * MAX((1.0f - b1) * mask, b1 * (1.0f - mask)) + neg * MAX(b1, mask);
    }
  }
  else
  {
    __OMP_FOR_SIMD__(aligned(dest, newmask : 64)  if(npixels > 10000))
    for(int index = 0; index < npixels; index++)
    {
      const float mask = opacity * newmask[index];
      const float pos = both_positive(dest[index], mask);
      const float neg = (1.0f - pos);
      const float b1 = dest[index];
      dest[index] = pos * MAX((1.0f - b1) * mask, b1 * (1.0f - mask)) + neg * MAX(b1, mask);
    }
  }
}

/**
 * @brief Identity of the group mask after folding the first @p count shapes, at this ROI.
 *
 * @details The fold below is a left fold: the result after k shapes is a pure function of the
 * result after k-1 and of shape k. So every PREFIX of it is a cacheable value, and the one that
 * matters is the longest one: drawing a new stroke appends a shape and leaves every earlier
 * prefix untouched.
 *
 * What has to be in the key is everything the fold reads:
 *
 *   - each shape's own content, via dt_masks_form_get_own_hash() -- for a brush that is every
 *     node's position, border, fading and density, which is exactly what its rasterisation is
 *     a function of;
 *   - each membership's state and opacity, which decide the combine operator and its weight;
 *   - the ROI, which decides the pixels;
 *   - the module's own input and output rects, which is how a geometry change upstream (a crop
 *     moved, a keystone) reaches the rasterisation without changing any form. Deliberately NOT
 *     the pipe's history hash: that changes on every commit, including the stroke being drawn,
 *     which would make the cache miss exactly when it is worth having.
 */
static uint64_t _group_prefix_hash(const dt_masks_form_t *const form, GList *masks,
                                   const dt_dev_pixelpipe_iop_t *const piece, const dt_iop_roi_t *const roi,
                                   const int count)
{
  uint64_t hash = 5381;
  hash = dt_hash(hash, (const char *)roi, sizeof(dt_iop_roi_t));
  if(!IS_NULL_PTR(piece))
  {
    hash = dt_hash(hash, (const char *)&piece->buf_in, sizeof(dt_iop_roi_t));
    hash = dt_hash(hash, (const char *)&piece->buf_out, sizeof(dt_iop_roi_t));
  }

  int i = 0;
  for(const GList *fpts = form->points; fpts && i < count; fpts = g_list_next(fpts), i++)
  {
    const dt_masks_form_group_t *const fpt = (const dt_masks_form_group_t *)fpts->data;
    const dt_masks_form_t *const sel = dt_masks_get_from_id_ext(masks, fpt->formid);
    if(IS_NULL_PTR(sel)) return 0;   // cannot describe this prefix; refuse to key on it

    hash = dt_hash(hash, (const char *)&fpt->state, sizeof(int));
    hash = dt_hash(hash, (const char *)&fpt->opacity, sizeof(float));
    hash = dt_masks_form_get_own_hash(hash, masks, sel);
  }
  return hash ? hash : 1;
}

static dt_masks_raster_result_t _group_get_mask_roi(const dt_iop_module_t *const restrict module, dt_dev_pixelpipe_t *pipe,
                               const dt_dev_pixelpipe_iop_t *const restrict piece,
                               dt_masks_form_t *const form, const dt_iop_roi_t *const roi,
                               float *const restrict buffer, dt_iop_roi_t *touched)
{
  double start = dt_get_wtime();
  dt_masks_touched_none(touched);
  if(IS_NULL_PTR(form->points)) return DT_MASKS_RASTER_EMPTY;
  int nb_ok = 0;

  const int width = roi->width;
  const int height = roi->height;
  const size_t npixels = (size_t)width * height;

  /* Resume from the longest cached prefix.
   *
   * Without this every shape is rasterised and combined from scratch on every render, so drawing
   * the tenth stroke costs ten shapes and drawing the first costs one -- measured on #1158 at
   * ~9 ms to rasterise and ~21 ms to combine per shape, on the preview pipe and the full pipe
   * both, growing `render all masks' from 205 ms to 260 ms across one session. Appending a stroke
   * leaves every earlier prefix identical, so the work that actually has to be redone is one
   * shape's.
   *
   * Shapes are walked newest-first: the append case hits on the first probe. The forms come from
   * pipe->forms, the refcounted snapshot this run owns -- never dev->forms, which the GUI thread
   * is mutating while this runs on a worker. */
  GList *const masks = !IS_NULL_PTR(pipe) && !IS_NULL_PTR(pipe->forms) ? pipe->forms : module->dev->forms;
  const int shape_count = g_list_length(form->points);
  int resume_from = 0;

  for(int count = shape_count - 1; count > 0; count--)
  {
    const uint64_t prefix = _group_prefix_hash(form, masks, piece, roi, count);
    if(prefix == 0) break;

    void *cached = NULL;
    dt_pixel_cache_entry_t *entry = NULL;
    if(dt_dev_pixelpipe_cache_peek(prefix, &cached, &entry, -1, NULL) && !IS_NULL_PTR(cached))
    {
      dt_dev_pixelpipe_cache_rdlock_entry(TRUE, entry);
      memcpy(buffer, cached, npixels * sizeof(float));
      dt_dev_pixelpipe_cache_rdlock_entry(FALSE, entry);
      dt_dev_pixelpipe_cache_ref_count_entry(FALSE, entry);
      resume_from = count;
      nb_ok = count;
      if(dt_get_debug_flags() & DT_DEBUG_PERF)
        dt_print(DT_DEBUG_MASKS, "[masks] group fold resumed from cached prefix of %d/%d shapes\n", count,
                 shape_count);
      break;
    }
  }

  // we need to allocate a zeroed temporary buffer for intermediate creation of individual shapes
  float *const restrict bufs = dt_pixelpipe_cache_alloc_align_float_cache(npixels, 0);
  if(IS_NULL_PTR(bufs)) return DT_MASKS_RASTER_ERROR;
  dt_masks_raster_result_t err = DT_MASKS_RASTER_OK;

  /* Somewhere to hold the fold minus its last shape. Only needed when there is something new to
   * publish; a failure to allocate simply means this render publishes nothing. */
  float *publishable = NULL;
  if(shape_count > 1 && resume_from < shape_count - 1)
    publishable = dt_pixelpipe_cache_alloc_align_float_cache(npixels, 0);

  if(resume_from == 0) memset(buffer, 0, npixels * sizeof(float));
  gboolean bufs_zeroed = FALSE;
  dt_iop_roi_t child_touched;
  dt_masks_touched_none(&child_touched);
  dt_iop_roi_t group_touched;
  dt_masks_touched_none(&group_touched);
  if(resume_from > 0) dt_masks_touched_full(&group_touched, width, height); // the prefix is opaque to us

  int i = 0;
  // and we get all masks
  for(GList *fpts = form->points; fpts; fpts = g_list_next(fpts))
  {
    dt_masks_form_group_t *fpt = (dt_masks_form_group_t *)fpts->data;
    dt_masks_form_t *sel = dt_masks_get_from_id_ext(masks, fpt->formid);

    if(sel && i < resume_from)
    {
      // already folded into `buffer' by the cached prefix above
      i++;
      continue;
    }

    if(sel)
    {
      /* Snapshot the fold before the LAST shape goes in: that is what gets published, and it is
       * the only state a later render can start from without redoing the shape being edited. */
      if(i == shape_count - 1 && !IS_NULL_PTR(publishable))
        memcpy(publishable, buffer, npixels * sizeof(float));

      /* `bufs' must be zero wherever the child will not write. Clearing only the PREVIOUS child's
       * reported box keeps that invariant at the cost of that box, not of the whole ROI per shape. */
      if(!bufs_zeroed)
      {
        memset(bufs, 0, npixels * sizeof(float));
        bufs_zeroed = TRUE;
      }
      else
        dt_masks_touched_clear(bufs, width, &child_touched);
      const dt_masks_raster_result_t child_result
          = dt_masks_get_mask_roi(module, pipe, piece, sel, roi, bufs, &child_touched);
      const float op = fpt->opacity;
      // Add a foolproof to ensure that the first shape is no-op
      const int no_op_state = fpt->state & ~(DT_MASKS_STATE_IS_COMBINE_OP) ;
      const int state = (i == 0) ? no_op_state : fpt->state;
      if(child_result == DT_MASKS_RASTER_ERROR)
      {
        /* Undefined buffer: what this child left in `bufs' cannot be folded in, and the fold as
         * a whole must not be published. */
        err = DT_MASKS_RASTER_ERROR;
        break;
      }
      /* EMPTY combines exactly like OK, because `bufs' is fully zeroed before every child (the
       * first child memsets it, each later one clears the previous child's reported box), so a
       * shape that drew nothing IS an all-zero child. That distinction matters: an EMPTY child
       * must still be intersected with -- a brush lying outside this ROI genuinely has no
       * coverage here, and skipping the combine would leave the previous fold standing where
       * the mask should have gone to zero.
       *
       * What EMPTY changes is only that it is no longer an ERROR. A NULL points list used to
       * abort the entire group for a circle and fold as zeros for an ellipse: the same
       * degenerate data killed or spared the whole mask depending only on which shape carried
       * it. Both now fold as zeros. */
      else
      {
        // first see if we need to invert this shape
        const int inverted = (state & DT_MASKS_STATE_INVERSE);

        /* Union and plain copy leave `buffer' untouched outside the child's box: max(dest,0) is
         * dest, and the copy writes op * 0 = 0 over an already-zero buffer. Those two run over the
         * box alone. Intersection/difference/exclusion rewrite the outside too, and an inverted
         * child is non-zero outside its box, so those keep the full-ROI pass. */
        const gboolean box_only = !inverted && !dt_masks_touched_is_empty(&child_touched)
                                  && ((state & DT_MASKS_STATE_UNION)
                                      || !(state & (DT_MASKS_STATE_INTERSECTION | DT_MASKS_STATE_DIFFERENCE
                                                    | DT_MASKS_STATE_EXCLUSION)));

        if(state & DT_MASKS_STATE_UNION)
        {
          if(box_only)
            _combine_masks_union_box(buffer, bufs, width, &child_touched, op);
          else
            _combine_masks_union(buffer, bufs, npixels, op, inverted);
        }
        else if(state & DT_MASKS_STATE_INTERSECTION)
        {
          _combine_masks_intersect(buffer, bufs, npixels, op, inverted);
        }
        else if(state & DT_MASKS_STATE_DIFFERENCE)
        {
          _combine_masks_difference(buffer, bufs, npixels, op, inverted);
        }
        else if(state & DT_MASKS_STATE_EXCLUSION)
        {
          _combine_masks_exclusion(buffer, bufs, npixels, op, inverted);
        }
        else if(box_only) // plain copy: op * child inside the box, zero everywhere else
        {
          if(i != 0) memset(buffer, 0, npixels * sizeof(float)); // a later copy discards the fold so far
          for(int y = child_touched.y; y < child_touched.y + child_touched.height; y++)
          {
            float *const restrict dest_row = buffer + (size_t)y * width + child_touched.x;
            const float *const restrict src_row = bufs + (size_t)y * width + child_touched.x;
            for(int x = 0; x < child_touched.width; x++) dest_row[x] = op * src_row[x];
          }
        }
        else
        {
          __OMP_PARALLEL_FOR_SIMD__(aligned(buffer, bufs : 64) if(npixels > 10000))
          for(int index = 0; index < npixels; index++)
          {
            buffer[index] = op * (inverted ? (1.0f - bufs[index]) : bufs[index]);
          }
        }

        if(box_only && (state & DT_MASKS_STATE_UNION))
          dt_masks_touched_union(&group_touched, &child_touched);
        else if(box_only)
          group_touched = child_touched; // the copy discarded everything outside this box
        else
          dt_masks_touched_full(&group_touched, width, height);

        if(dt_get_debug_flags() & DT_DEBUG_PERF)
          dt_print(DT_DEBUG_MASKS, "[masks %d] combine took %0.04f sec\n", nb_ok, dt_get_wtime() - start);
        start = dt_get_wtime();

        nb_ok++;
      }
    }
    i++;
  }
  // and we free the intermediate buffer
  dt_pixelpipe_cache_free_align(bufs);


  if(nb_ok == 0)
    memset(buffer, 0, npixels * sizeof(float));
  else if(!IS_NULL_PTR(touched))
    *touched = group_touched;

  /* Publish the fold WITHOUT the last shape, never the complete one.
   *
   * The last shape is the one being edited: while a stroke is dragged its content changes on every
   * frame, so the complete fold's key changes on every frame too. Publishing that would leave one
   * full-ROI cacheline per rendered frame behind, each dead the moment it is written -- memory
   * pressure that evicts the pipeline's own outputs and costs more than the fold ever saved.
   * Measured: publishing the complete fold flattened `render all masks' as intended (205-260 ms
   * down to 32-100 ms) while the darkroom redraw grew from 5 ms to 416 ms over one session.
   *
   * The fold minus its last shape is stable across every frame of a stroke, so it is published
   * once and hit on every later frame -- and when the next shape is appended the resume covers all
   * but two. Nothing is published when the resume already came from that prefix: it is what we
   * would be writing back.
   *
   * Only on a complete, error-free fold: a prefix that describes fewer shapes than its key claims
   * is not a wrong-looking mask, it is a mask. */
  if(err == DT_MASKS_RASTER_OK && shape_count > 1 && nb_ok == shape_count && resume_from < shape_count - 1
     && !IS_NULL_PTR(publishable))
  {
    const uint64_t prefix = _group_prefix_hash(form, masks, piece, roi, shape_count - 1);
    if(prefix != 0)
    {
      void *slot = NULL;
      dt_pixel_cache_entry_t *entry = NULL;
      const int created = dt_dev_pixelpipe_cache_get(prefix, npixels * sizeof(float), "masks group prefix",
                                                    IS_NULL_PTR(pipe) ? -1 : pipe->type, TRUE, &slot, &entry);
      if(!IS_NULL_PTR(slot))
      {
        if(created)
        {
          memcpy(slot, publishable, npixels * sizeof(float));
          dt_dev_pixelpipe_cache_wrlock_entry(FALSE, entry);
        }
        dt_dev_pixelpipe_cache_ref_count_entry(FALSE, entry);
      }
    }
  }

  dt_pixelpipe_cache_free_align(publishable);

  return err;
}

dt_masks_raster_result_t dt_masks_group_render_roi(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe,
                                                   const dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form,
                                                   const dt_iop_roi_t *roi, float *buffer)
{
  const double start = dt_get_wtime();
  if(IS_NULL_PTR(form)) return DT_MASKS_RASTER_EMPTY;

  const dt_masks_raster_result_t err = dt_masks_get_mask_roi(module, pipe, piece, form, roi, buffer, NULL);

  if(dt_get_debug_flags() & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks] render all masks took %0.04f sec\n", dt_get_wtime() - start);
  return err;
}

static void _group_duplicate_points(dt_develop_t *const dev, dt_masks_form_t *const base,
                                    dt_masks_form_t *const dest)
{
  for(GList *pts = base->points; pts; pts = g_list_next(pts))
  {
    dt_masks_form_group_t *pt = (dt_masks_form_group_t *)pts->data;
    dt_masks_form_group_t *npt = (dt_masks_form_group_t *)malloc(sizeof(dt_masks_form_group_t));

    npt->formid = dt_masks_form_duplicate(dev, pt->formid);
    npt->parentid = dest->formid;
    npt->state = pt->state;
    npt->opacity = pt->opacity;
    dest->points = g_list_append(dest->points, npt);
  }
}

static gboolean _group_get_gravity_center(dt_develop_t *dev, const dt_masks_form_t *form, float center[2], float *area)
{
  if(IS_NULL_PTR(form) || IS_NULL_PTR(form->points) || IS_NULL_PTR(center) || IS_NULL_PTR(area)) return FALSE;

  float sum_x = 0.0f;
  float sum_y = 0.0f;
  float sum_w = 0.0f;
  int count = 0;

  for(const GList *l = form->points; l; l = g_list_next(l))
  {
    const dt_masks_form_group_t *pt = (const dt_masks_form_group_t *)l->data;
    if(IS_NULL_PTR(pt)) continue;
    dt_masks_form_t *child = dt_masks_get_from_id(dev, pt->formid);
    if(IS_NULL_PTR(child)) continue;

    float child_center[2] = { 0.0f, 0.0f };
    float child_area = 0.0f;
    if(!dt_masks_form_get_gravity_center(dev, child, child_center, &child_area)) continue;

    const float w = (child_area > 0.0f) ? child_area : 1.0f;
    sum_x += child_center[0] * w;
    sum_y += child_center[1] * w;
    sum_w += w;
    count++;
  }

  if(count == 0)
  {
    center[0] = 0.0f;
    center[1] = 0.0f;
    *area = 0.0f;
    return FALSE;
  }

  if(sum_w <= 0.0f)
  {
    center[0] = sum_x / (float)count;
    center[1] = sum_y / (float)count;
    *area = 0.0f;
  }
  else
  {
    center[0] = sum_x / sum_w;
    center[1] = sum_y / sum_w;
    *area = sum_w;
  }

  return TRUE;
}

// The function table for groups.  This must be public, i.e. no "static" keyword.
const dt_masks_functions_t dt_masks_functions_group = {
  .point_struct_size = sizeof(struct dt_masks_form_group_t),
  .sanitize_config = NULL,
  .set_form_name = NULL,
  .set_hint_message = NULL,
  .duplicate_points = _group_duplicate_points,
  .initial_source_pos = NULL,
  .get_distance = NULL,
  .get_points = NULL,
  .get_points_border = NULL,
  .get_mask = _group_get_mask,
  .get_mask_roi = _group_get_mask_roi,
  .get_area = NULL,
  .get_source_area = NULL,
  .get_gravity_center = _group_get_gravity_center,
  .get_interaction_value = NULL,
  .set_interaction_value = NULL,
  .update_hover = NULL,
  .mouse_moved = _group_events_mouse_moved,
  .mouse_scrolled = _group_events_mouse_scrolled,
  .button_pressed = _group_events_button_pressed,
  .button_released = _group_events_button_released,
  .key_pressed = _group_events_key_pressed,
//TODO:  .post_expose = _group_events_post_expose
  .draw_shape = NULL
};


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
