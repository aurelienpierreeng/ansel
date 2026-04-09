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
#include "common/darktable.h"
#include "common/debug.h"
#include "control/conf.h"
#include"control/control.h"
#include "develop/blend.h"
#include "develop/imageop.h"
#include "develop/masks.h"

/* Shape handlers receive widget-space coordinates, while normalized output-image
 * coordinates come from `gui->rel_pos` and absolute output-image
 * coordinates come from `gui->pos`. */
// Centralize group child lookup so all dispatchers and expose code resolve the same
// selected child the same way.
static dt_masks_form_t *_group_get_child_at(dt_masks_form_t *form, const int group_index,
                                            dt_masks_form_group_t **group_entry)
{
  dt_masks_form_group_t *entry = (dt_masks_form_group_t *)g_list_nth_data(form->points, group_index);
  if(!entry) return NULL;
  if(group_entry) *group_entry = entry;
  return dt_masks_get_from_id(darktable.develop, entry->formid);
}

static dt_masks_form_t *_group_get_selected_child(dt_masks_form_t *form, dt_masks_form_gui_t *gui,
                                                  dt_masks_form_group_t **group_entry)
{
  if(gui->group_selected < 0) return NULL;
  return _group_get_child_at(form, gui->group_selected, group_entry);
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
  if(!form) return 0;

  gboolean return_value = FALSE;

  // Global key bindings for groups
  // TODO: map that to context menu
  if(!return_value)
  {
    switch(event->keyval)
    {
      case GDK_KEY_Escape:
      {
        return_value = dt_masks_form_cancel_creation(module, gui);
        break;
      }
      case GDK_KEY_Delete:
      {
        if(gui->group_selected >= 0)
        {
          // Delete shape from current group
          dt_masks_form_group_t *group_entry = NULL;
          dt_masks_form_t *selected_form = _group_get_selected_child(form, gui, &group_entry);
          if(!selected_form) return 0;
          return_value = dt_masks_gui_delete(module, selected_form, gui, group_entry->parentid);
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
  dt_masks_form_t *selected_form = _group_get_child_at(form, pos, NULL);
  if(selected_form && selected_form->functions && selected_form->functions->post_expose)
  {
    gui->type = selected_form->type;
    selected_form->functions->post_expose(cr, zoom_scale, gui, pos, g_list_length(selected_form->points));
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
  if(buf == NULL) return 1;

  // we fill this buffer
  const int posx_ = *posx;
  const int posy_ = *posy;
  const int width_ = *width;
  const int height_ = *height;
  const float *const src = *buffer;

#ifdef _OPENMP
#pragma omp parallel for default(firstprivate) \
  schedule(static) if(wt * ht > 50000)
#endif
  for(int yy = 0; yy < MIN(posy_, ht); yy++)
  {
    float *const row = buf + (size_t)yy * wt;
    for(int xx = 0; xx < wt; xx++) row[xx] = 1.0f;
  }

#ifdef _OPENMP
#pragma omp parallel for default(firstprivate) \
  schedule(static) if(wt * ht > 50000)
#endif
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

#ifdef _OPENMP
#pragma omp parallel for default(firstprivate) \
  schedule(static) if(wt * ht > 50000)
#endif
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

static int _group_get_mask(const dt_iop_module_t *const module, dt_dev_pixelpipe_t *pipe,
                           const dt_dev_pixelpipe_iop_t *const piece,
                           dt_masks_form_t *const form,
                           float **buffer, int *width, int *height, int *posx, int *posy)
{
  // we allocate buffers and values
  const guint nb = g_list_length(form->points);
  if(nb == 0)
  {
    *buffer = NULL;
    *width = *height = *posx = *posy = 0;
    return 0;
  }
  float **bufs = calloc(nb, sizeof(float *));
  int *w = malloc(sizeof(int) * nb);
  int *h = malloc(sizeof(int) * nb);
  int *px = malloc(sizeof(int) * nb);
  int *py = malloc(sizeof(int) * nb);
  int *states = malloc(sizeof(int) * nb);
  float *op = malloc(sizeof(float) * nb);
  if(!bufs || !w || !h || !px || !py || !states || !op)
  {
    dt_free(op);
    dt_free(states);
    dt_free(py);
    dt_free(px);
    dt_free(h);
    dt_free(w);
    dt_free(bufs);
    return 1;
  }

  // and we get all masks
  int pos = 0;
  int nb_ok = 0;
  int err = 0;
  for(GList *fpts = form->points; fpts; fpts = g_list_next(fpts))
  {
    dt_masks_form_group_t *fpt = (dt_masks_form_group_t *)fpts->data;
    dt_masks_form_t *sel = dt_masks_get_from_id(module->dev, fpt->formid);
    if(sel)
    {
      if(dt_masks_get_mask(module, pipe, piece, sel, &bufs[pos], &w[pos], &h[pos], &px[pos], &py[pos]) != 0)
      {
        err = 1;
        break;
      }
      if(fpt->state & DT_MASKS_STATE_INVERSE)
      {
        const double start = dt_get_wtime();
        if(_inverse_mask(module, piece, sel, &bufs[pos], &w[pos], &h[pos], &px[pos], &py[pos]) != 0)
        {
          err = 1;
          break;
        }
        if(darktable.unmuted & DT_DEBUG_PERF)
          dt_print(DT_DEBUG_MASKS, "[masks %s] inverse took %0.04f sec\n", sel->name, dt_get_wtime() - start);
      }
      op[pos] = fpt->opacity;
      states[pos] = fpt->state;
      nb_ok++;
    }
    pos++;
  }
  if(err) goto cleanup;
  if(nb_ok == 0)
  {
    *buffer = NULL;
    *width = *height = *posx = *posy = 0;
    goto cleanup;
  }

  // now we get the min, max, width, height of the final mask
  int l = INT_MAX, r = INT_MIN, t = INT_MAX, b = INT_MIN;
  for(int i = 0; i < nb; i++)
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
  if(*buffer == NULL)
  {
    err = 1;
    goto cleanup;
  }

  // and we copy each buffer inside, row by row
  const int dst_w = r - l;
  const int dst_h = b - t;
  float *const dst = *buffer;
  for(int i = 0; i < nb; i++)
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
#ifdef _OPENMP
#pragma omp parallel for default(firstprivate) \
  schedule(static) if((size_t)wi * hi > 10000)
#endif
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

#ifdef _OPENMP
#pragma omp parallel for default(firstprivate) \
  schedule(static) if((size_t)dst_w * dst_h > 10000)
#endif
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
#ifdef _OPENMP
#pragma omp parallel for default(firstprivate) \
  schedule(static) if((size_t)wi * hi > 10000)
#endif
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
#ifdef _OPENMP
#pragma omp parallel for default(firstprivate) \
  schedule(static) if((size_t)wi * hi > 10000)
#endif
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

#ifdef _OPENMP
#pragma omp parallel for default(firstprivate) \
  schedule(static) if((size_t)dst_w * dst_h > 10000)
#endif
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

    if(darktable.unmuted & DT_DEBUG_PERF)
      dt_print(DT_DEBUG_MASKS, "[masks %d] combine took %0.04f sec\n", i, dt_get_wtime() - start);
  }

cleanup:
  dt_free(op);
  dt_free(states);
  dt_free(py);
  dt_free(px);
  dt_free(h);
  dt_free(w);
  for(int i = 0; i < nb; i++) dt_pixelpipe_cache_free_align(bufs[i]);
  dt_free(bufs);
  return err;
}

static void _combine_masks_union(float *const restrict dest, float *const restrict newmask, const size_t npixels,
                                 const float opacity, const int inverted)
{
  if (inverted)
  {
#ifdef _OPENMP
#pragma omp parallel for simd default(firstprivate) \
  aligned(dest, newmask : 64) schedule(simd:static) if(npixels > 10000)
#endif
    for(int index = 0; index < npixels; index++)
    {
      const float mask = opacity * (1.0f - newmask[index]);
      dest[index] = MAX(dest[index], mask);
    }
  }
  else
  {
#ifdef _OPENMP
#pragma omp parallel for simd default(firstprivate) \
  aligned(dest, newmask : 64) schedule(simd:static) if(npixels > 10000)
#endif
    for(int index = 0; index < npixels; index++)
    {
      const float mask = opacity * newmask[index];
      dest[index] = MAX(dest[index], mask);
    }
  }
}

static void _combine_masks_intersect(float *const restrict dest, float *const restrict newmask, const size_t npixels,
                                     const float opacity, const int inverted)
{
  if (inverted)
  {
#ifdef _OPENMP
#pragma omp parallel for simd default(firstprivate) \
  aligned(dest, newmask : 64) schedule(simd:static) if(npixels > 10000)
#endif
    for(int index = 0; index < npixels; index++)
    {
      const float mask = opacity * (1.0f - newmask[index]);
      dest[index] = MIN(MAX(dest[index], 0.0f), MAX(mask, 0.0f));
    }
  }
  else
  {
#ifdef _OPENMP
#pragma omp parallel for simd default(firstprivate) \
  aligned(dest, newmask : 64) schedule(simd:static) if(npixels > 10000)
#endif
    for(int index = 0; index < npixels; index++)
    {
      const float mask = opacity * newmask[index];
      dest[index] = MIN(MAX(dest[index], 0.0f), MAX(mask, 0.0f));
    }
  }
}

#ifdef _OPENMP
#pragma omp declare simd
#endif
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
#ifdef _OPENMP
#pragma omp parallel for simd default(firstprivate) \
  aligned(dest, newmask : 64) schedule(simd:static) if(npixels > 10000)
#endif
    for(int index = 0; index < npixels; index++)
    {
      const float mask = opacity * (1.0f - newmask[index]);
      dest[index] *= (1.0f - mask * both_positive(dest[index],mask));
    }
  }
  else
  {
#ifdef _OPENMP
#pragma omp parallel for simd default(firstprivate) \
  aligned(dest, newmask : 64) schedule(simd:static) if(npixels > 10000)

#endif
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
#ifdef _OPENMP
#pragma omp parallel for simd default(firstprivate) \
  aligned(dest, newmask : 64) schedule(simd:static) if(npixels > 10000)

#endif
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
#ifdef _OPENMP
#pragma omp parallel for simd default(firstprivate) \
  aligned(dest, newmask : 64) schedule(simd:static) if(npixels > 10000)
#endif
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

static int _group_get_mask_roi(const dt_iop_module_t *const restrict module, dt_dev_pixelpipe_t *pipe,
                               const dt_dev_pixelpipe_iop_t *const restrict piece,
                               dt_masks_form_t *const form, const dt_iop_roi_t *const roi,
                               float *const restrict buffer)
{
  double start = dt_get_wtime();
  if(!form->points) return 0;
  int nb_ok = 0;

  const int width = roi->width;
  const int height = roi->height;
  const size_t npixels = (size_t)width * height;

  // we need to allocate a zeroed temporary buffer for intermediate creation of individual shapes
  float *const restrict bufs = dt_pixelpipe_cache_alloc_align_float_cache(npixels, 0);
  if(bufs == NULL) return 1;
  int err = 0;

  int i = 0;
  // and we get all masks
  for(GList *fpts = form->points; fpts; fpts = g_list_next(fpts))
  {
    dt_masks_form_group_t *fpt = (dt_masks_form_group_t *)fpts->data;
    dt_masks_form_t *sel = dt_masks_get_from_id(module->dev, fpt->formid);

    if(sel)
    {
      // ensure that we start with a zeroed buffer regardless of what was previously written into 'bufs'
      memset(bufs, 0, npixels*sizeof(float));
      const int err_child = dt_masks_get_mask_roi(module, pipe, piece, sel, roi, bufs);
      const float op = fpt->opacity;
      // Add a foolproof to ensure that the first shape is no-op
      const int no_op_state = fpt->state & ~(DT_MASKS_STATE_IS_COMBINE_OP) ;
      const int state = (i == 0) ? no_op_state : fpt->state;
      if(err_child != 0)
      {
        err = 1;
        break;
      }
      if(err_child == 0)
      {
        // first see if we need to invert this shape
        const int inverted = (state & DT_MASKS_STATE_INVERSE);

        if(state & DT_MASKS_STATE_UNION)
        {
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
        else // if we are here, this mean that we just have to copy the shape and null other parts
        {
#ifdef _OPENMP
#pragma omp parallel for simd default(firstprivate) \
          schedule(simd:static) aligned(buffer, bufs : 64) if(npixels > 10000)
#endif
          for(int index = 0; index < npixels; index++)
          {
            buffer[index] = op * (inverted ? (1.0f - bufs[index]) : bufs[index]);
          }
        }

        if(darktable.unmuted & DT_DEBUG_PERF)
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

  return err;
}

int dt_masks_group_render_roi(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe,
                              const dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form,
                              const dt_iop_roi_t *roi, float *buffer)
{
  const double start = dt_get_wtime();
  if(!form) return 0;

  const int err = dt_masks_get_mask_roi(module, pipe, piece, form, roi, buffer);

  if(darktable.unmuted & DT_DEBUG_PERF)
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

static gboolean _group_get_gravity_center(const dt_masks_form_t *form, float center[2], float *area)
{
  if(!form || !form->points || !center || !area) return FALSE;

  float sum_x = 0.0f;
  float sum_y = 0.0f;
  float sum_w = 0.0f;
  int count = 0;

  for(const GList *l = form->points; l; l = g_list_next(l))
  {
    const dt_masks_form_group_t *pt = (const dt_masks_form_group_t *)l->data;
    if(!pt) continue;
    dt_masks_form_t *child = dt_masks_get_from_id(darktable.develop, pt->formid);
    if(!child) continue;

    float child_center[2] = { 0.0f, 0.0f };
    float child_area = 0.0f;
    if(!dt_masks_form_get_gravity_center(child, child_center, &child_area)) continue;

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
