/*
    This file is part of darktable,
    Copyright (C) 2013-2021 darktable developers.

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
#include "common/debug.h"
#include "control/conf.h"
#include"control/control.h"
#include "develop/blend.h"
#include "develop/imageop.h"
#include "develop/masks.h"

static int _group_events_mouse_scrolled(struct dt_iop_module_t *module, float pzx, float pzy, int up, const int flow,
                                        uint32_t state, dt_masks_form_t *form, int unused1, dt_masks_form_gui_t *gui,
                                        int unused, dt_masks_interaction_t interaction)
{
  if(gui->group_selected >= 0)
  {
    // we get the form
    dt_masks_form_group_t *fpt = (dt_masks_form_group_t *)g_list_nth_data(form->points, gui->group_selected);
    if(!fpt) return 0;
    dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
    if(sel && sel->functions)
      return sel->functions->mouse_scrolled(module, pzx, pzy, up, flow, state, sel, fpt->parentid, gui, gui->group_selected, interaction);
  }
  return 0;
}

static gboolean _detect_new_shape_selection(dt_masks_form_t *form, dt_masks_form_gui_t *gui, float pzx, float pzy)
{
  if(!form || !gui) return FALSE;

  // If we get here, it's either because:
  // 1. no group is being currently edited
  // 2. the currently-edited group didn't capture the event, meaning the click didn't happen near a control node.
  // Both ways, we need to redetect where the click happened to see if a new shape should be selected.

  // reset selection
  dt_masks_soft_reset_form_gui(gui);
  dt_develop_t *dev = (dt_develop_t *)darktable.develop;
  dev->mask_form_selected_id = -1;

  // now we check if we are near a new form
  const float as = DT_GUI_MOUSE_EFFECT_RADIUS_SCALED;  // transformed to backbuf dimensions
  gui->form_selected = gui->border_selected = FALSE;
  gui->source_selected = gui->source_dragging = FALSE;
  gui->pivot_selected = FALSE;
  gui->handle_selected = -1;
  gui->node_edited = gui->node_selected = -1;
  gui->seg_selected = -1;
  gui->handle_border_selected = -1;
  gui->group_selected = -1;

  dt_masks_form_t *sel = NULL;
  int sel_index = 0;
  float sel_dist = FLT_MAX;

  const float scale = dev->natural_scale;
  const float xx = (pzx * dev->preview_width)  / scale,
              yy = (pzy * dev->preview_height) / scale;

  int index = 0;
  for(GList *fpts = form->points; fpts; fpts = g_list_next(fpts))
  {
    dt_masks_form_group_t *fpt = (dt_masks_form_group_t *)fpts->data;
    if(!fpt) continue;
    dt_masks_form_t *frm = dt_masks_get_from_id(dev, fpt->formid);
    if(!frm) continue; // it means the form has been deleted meanwhile

    int inside, inside_border, near, inside_source;
    float dist = FLT_MAX;
    inside = inside_border = inside_source = 0;
    near = -1;
    

    //float xx = -1, yy = -1;
    //dt_dev_retrieve_full_pos(darktable.develop, pzx, pzy, &xx, &yy);
    

    if(frm->functions && frm->functions->get_distance)
      frm->functions->get_distance(xx, yy, as, gui, index, g_list_length(frm->points),
                                   &inside, &inside_border, &near, &inside_source, &dist);

    if(inside || inside_border || near >= 0 || inside_source)
    {

      // Look for the closest
      if(sel_dist > dist)
      {
        sel = frm;
        sel_dist = dist;
        sel_index = index;
      }
    }
    index++;
  }

  if(sel && sel->functions)
  {
    gui->group_selected = sel_index;
    dev->mask_form_selected_id = sel->formid;
    return TRUE;
  }

  return FALSE;
}

static int _group_events_button_pressed(struct dt_iop_module_t *module, float pzx, float pzy,
                                        double pressure, int which, int type, uint32_t state,
                                        dt_masks_form_t *form, int unused1, dt_masks_form_gui_t *gui, int unused2)
{
  if(!form) return 0;

  gboolean return_value = FALSE;

  if(gui->group_selected >= 0)
  {
    // we get the form
    dt_masks_form_group_t *fpt = (dt_masks_form_group_t *)g_list_nth_data(form->points, gui->group_selected);
    if(!fpt) return 0;
    dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
    if(sel && sel->functions)
      return_value = sel->functions->button_pressed(module, pzx, pzy, pressure, which, type, state, sel,
                                           fpt->parentid, gui, gui->group_selected);
    if(which == 3 && !return_value)
    {
      return_value = dt_masks_gui_delete(module, sel, gui, fpt->parentid);
    }
  }
  return return_value;
}

static int _group_events_button_released(struct dt_iop_module_t *module, float pzx, float pzy, int which,
                                         uint32_t state, dt_masks_form_t *form, int unused1, dt_masks_form_gui_t *gui,
                                         int unused2)
{
  if(gui->group_selected >= 0)
  {
    // we get the form
    dt_masks_form_group_t *fpt = (dt_masks_form_group_t *)g_list_nth_data(form->points, gui->group_selected);
    if(!fpt) return 0;
    dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
    if(sel && sel->functions)
      if(sel->functions->button_released(module, pzx, pzy, which, state, sel, fpt->parentid, gui,
                                             gui->group_selected))
        return 1;
  }

  return _detect_new_shape_selection(form, gui, pzx, pzy);
}

static int _group_events_mouse_moved(struct dt_iop_module_t *module, float pzx, float pzy, double pressure,
                                     int which, dt_masks_form_t *form, int unused1, dt_masks_form_gui_t *gui,
                                     int unused2)
{
  const float as = DT_GUI_MOUSE_EFFECT_RADIUS_SCALED;

  // we first don't do anything if we are inside a scrolling session
  if(gui->scrollx != 0.0f && gui->scrolly != 0.0f)
  {
    if((gui->scrollx - pzx < as && gui->scrollx - pzx > -as)
       && (gui->scrolly - pzy < as && gui->scrolly - pzy > -as))
      return 1;
    gui->scrollx = gui->scrolly = 0.0f;
  }

  // if a form is in edit mode, capture movement
  if(gui->group_selected >= 0)
  {
    // we get the form
    dt_masks_form_group_t *fpt = (dt_masks_form_group_t *)g_list_nth_data(form->points, gui->group_selected);
    if(!fpt) return 0;
    dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
    if(sel && sel->functions)
      return sel->functions->mouse_moved(module, pzx, pzy, pressure, which, sel, fpt->parentid, gui,
                                       gui->group_selected);
  }

  // capturing scroll event outside of editing mode is dangerous (zoom).
  // Nothing will happen then.
  return 0;
}

static void _group_events_post_expose_draw(cairo_t *cr, float zoom_scale, dt_masks_form_t *form,
                                          dt_masks_form_gui_t *gui, int pos)
{
  // we get the form
  dt_masks_form_group_t *fpt = (dt_masks_form_group_t *)g_list_nth_data(form->points, pos);
  dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
  if(sel && sel->functions)
    sel->functions->post_expose(cr, zoom_scale, gui, pos, g_list_length(sel->points));
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
  for(int yy = 0; yy < MIN(*posy, ht); yy++)
  {
    for(int xx = 0; xx < wt; xx++) buf[(size_t)yy * wt + xx] = 1.0f;
  }

  for(int yy = MAX(*posy, 0); yy < MIN(ht, (*posy) + (*height)); yy++)
  {
    for(int xx = 0; xx < MIN((*posx), wt); xx++) buf[(size_t)yy * wt + xx] = 1.0f;
    for(int xx = MAX((*posx), 0); xx < MIN(wt, (*posx) + (*width)); xx++)
      buf[(size_t)yy * wt + xx] = 1.0f - (*buffer)[((size_t)yy - (*posy)) * (*width) + xx - (*posx)];
    for(int xx = MAX((*posx) + (*width), 0); xx < wt; xx++) buf[(size_t)yy * wt + xx] = 1.0f;
  }

  for(int yy = MAX((*posy) + (*height), 0); yy < ht; yy++)
  {
    for(int xx = 0; xx < wt; xx++) buf[(size_t)yy * wt + xx] = 1.0f;
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

static int _group_get_mask(const dt_iop_module_t *const module, const dt_dev_pixelpipe_iop_t *const piece,
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
    free(op);
    free(states);
    free(py);
    free(px);
    free(h);
    free(w);
    free(bufs);
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
      if(dt_masks_get_mask(module, piece, sel, &bufs[pos], &w[pos], &h[pos], &px[pos], &py[pos]) != 0)
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
  for(int i = 0; i < nb; i++)
  {
    const double start = dt_get_wtime();
    if(states[i] & DT_MASKS_STATE_UNION)
    {
      for(int y = 0; y < h[i]; y++)
      {
        for(int x = 0; x < w[i]; x++)
        {
          (*buffer)[(py[i] + y - t) * (r - l) + px[i] + x - l]
              = fmaxf((*buffer)[(py[i] + y - t) * (r - l) + px[i] + x - l], bufs[i][y * w[i] + x] * op[i]);
        }
      }
    }
    else if(states[i] & DT_MASKS_STATE_INTERSECTION)
    {
      for(int y = 0; y < b - t; y++)
      {
        for(int x = 0; x < r - l; x++)
        {
          const float b1 = (*buffer)[y * (r - l) + x];
          float b2 = 0.0f;
          if(y + t - py[i] >= 0 && y + t - py[i] < h[i] && x + l - px[i] >= 0 && x + l - px[i] < w[i])
            b2 = bufs[i][(y + t - py[i]) * w[i] + x + l - px[i]];
          if(b1 > 0.0f && b2 > 0.0f)
            (*buffer)[y * (r - l) + x] = fminf(b1, b2 * op[i]);
          else
            (*buffer)[y * (r - l) + x] = 0.0f;
        }
      }
    }
    else if(states[i] & DT_MASKS_STATE_DIFFERENCE)
    {
      for(int y = 0; y < h[i]; y++)
      {
        for(int x = 0; x < w[i]; x++)
        {
          const float b1 = (*buffer)[(py[i] + y - t) * (r - l) + px[i] + x - l];
          const float b2 = bufs[i][y * w[i] + x] * op[i];
          if(b1 > 0.0f && b2 > 0.0f) (*buffer)[(py[i] + y - t) * (r - l) + px[i] + x - l] = b1 * (1.0f - b2);
        }
      }
    }
    else if(states[i] & DT_MASKS_STATE_EXCLUSION)
    {
      for(int y = 0; y < h[i]; y++)
      {
        for(int x = 0; x < w[i]; x++)
        {
          const float b1 = (*buffer)[(py[i] + y - t) * (r - l) + px[i] + x - l];
          const float b2 = bufs[i][y * w[i] + x] * op[i];
          if(b1 > 0.0f && b2 > 0.0f)
            (*buffer)[(py[i] + y - t) * (r - l) + px[i] + x - l] = fmaxf((1.0f - b1) * b2, b1 * (1.0f - b2));
          else
            (*buffer)[(py[i] + y - t) * (r - l) + px[i] + x - l]
                = fmaxf((*buffer)[(py[i] + y - t) * (r - l) + px[i] + x - l], bufs[i][y * w[i] + x] * op[i]);
        }
      }
    }
    else // if we are here, this mean that we just have to copy the shape and null other parts
    {
      for(int y = 0; y < b - t; y++)
      {
        for(int x = 0; x < r - l; x++)
        {
          float b2 = 0.0f;
          if(y + t - py[i] >= 0 && y + t - py[i] < h[i] && x + l - px[i] >= 0 && x + l - px[i] < w[i])
            b2 = bufs[i][(y + t - py[i]) * w[i] + x + l - px[i]];
          (*buffer)[y * (r - l) + x] = b2 * op[i];
        }
      }
    }

    if(darktable.unmuted & DT_DEBUG_PERF)
      dt_print(DT_DEBUG_MASKS, "[masks %d] combine took %0.04f sec\n", i, dt_get_wtime() - start);
  }

cleanup:
  free(op);
  free(states);
  free(py);
  free(px);
  free(h);
  free(w);
  for(int i = 0; i < nb; i++) dt_pixelpipe_cache_free_align(bufs[i]);
  free(bufs);
  return err;
}

static void _combine_masks_union(float *const restrict dest, float *const restrict newmask, const size_t npixels,
                                 const float opacity, const int inverted)
{
  if (inverted)
  {
#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(npixels, opacity) \
  dt_omp_sharedconst(dest, newmask) aligned(dest, newmask : 64) \
  schedule(simd:static)
#else
#pragma omp parallel for shared(dest, newmask)
#endif
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
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(npixels, opacity) \
  dt_omp_sharedconst(dest, newmask) aligned(dest, newmask : 64) \
  schedule(simd:static)
#else
#pragma omp parallel for shared(dest, newmask)
#endif
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
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(npixels, opacity) \
  dt_omp_sharedconst(dest, newmask) aligned(dest, newmask : 64) \
  schedule(simd:static)
#else
#pragma omp parallel for shared(dest, newmask)
#endif
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
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(npixels, opacity) \
  dt_omp_sharedconst(dest, newmask) aligned(dest, newmask : 64) \
  schedule(simd:static)
#else
#pragma omp parallel for shared(dest, newmask)
#endif
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
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(npixels, opacity) \
  dt_omp_sharedconst(dest, newmask) aligned(dest, newmask : 64) \
  schedule(simd:static)
#else
#pragma omp parallel for shared(dest, newmask)
#endif
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
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(npixels, opacity) \
  dt_omp_sharedconst(dest, newmask) aligned(dest, newmask : 64) \
  schedule(simd:static)
#else
#pragma omp parallel for shared(dest, newmask)
#endif
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
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(npixels, opacity) \
  dt_omp_sharedconst(dest, newmask) aligned(dest, newmask : 64) \
  schedule(simd:static)
#else
#pragma omp parallel for shared(dest, newmask)
#endif
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
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(npixels, opacity) \
  dt_omp_sharedconst(dest, newmask) aligned(dest, newmask : 64) \
  schedule(simd:static)
#else
#pragma omp parallel for shared(dest, newmask)
#endif
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

static int _group_get_mask_roi(const dt_iop_module_t *const restrict module,
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

  // and we get all masks
  for(GList *fpts = form->points; fpts; fpts = g_list_next(fpts))
  {
    dt_masks_form_group_t *fpt = (dt_masks_form_group_t *)fpts->data;
    dt_masks_form_t *sel = dt_masks_get_from_id(module->dev, fpt->formid);

    if(sel)
    {
      // ensure that we start with a zeroed buffer regardless of what was previously written into 'bufs'
      memset(bufs, 0, npixels*sizeof(float));
      const int err_child = dt_masks_get_mask_roi(module, piece, sel, roi, bufs);
      const float op = fpt->opacity;
      const int state = fpt->state;

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
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for simd default(none) \
          dt_omp_firstprivate(npixels, op, inverted) \
          dt_omp_sharedconst(buffer, bufs) schedule(simd:static) aligned(buffer, bufs : 64)
#else
#pragma omp parallel for shared(bufs, buffer)
#endif
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
  }
  // and we free the intermediate buffer
  dt_pixelpipe_cache_free_align(bufs);

  if(nb_ok == 0)
    memset(buffer, 0, npixels * sizeof(float));

  return err;
}

int dt_masks_group_render_roi(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form,
                              const dt_iop_roi_t *roi, float *buffer)
{
  const double start = dt_get_wtime();
  if(!form) return 0;

  const int err = dt_masks_get_mask_roi(module, piece, form, roi, buffer);

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
  .mouse_moved = _group_events_mouse_moved,
  .mouse_scrolled = _group_events_mouse_scrolled,
  .button_pressed = _group_events_button_pressed,
  .button_released = _group_events_button_released,
//TODO:  .post_expose = _group_events_post_expose
  .draw_shape = NULL
};


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
