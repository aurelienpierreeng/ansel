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
#include "bauhaus/bauhaus.h"
#include "common/debug.h"
#include "common/undo.h"
#include "control/conf.h"

#include "develop/blend.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "develop/openmp_maths.h"

#define HARDNESS_MIN 0.0005f
#define HARDNESS_MAX 1.0f

#define BORDER_MIN 0.00005f
#define BORDER_MAX 0.5f

static void _circle_get_distance(float x, float y, float as, dt_masks_form_gui_t *gui, int index,
                                 int num_points, int *inside, int *inside_border, int *near, int *inside_source, float *dist)
{
  (void)num_points; // unused arg, keep compiler from complaining
  if(!gui) return;

  // initialise returned values
  *inside_source = 0;
  *inside = 0;
  *inside_border = 0;
  *near = -1;
  *dist = FLT_MAX;

  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return;

  // we first check if we are inside the source form
  if(dt_masks_point_in_form_exact(x, y, gpt->source, 1, gpt->source_count))
  {
    *inside_source = 1;
    *inside = 1;

    // distance from source center
    const float cx = x - gpt->source[0];
    const float cy = y - gpt->source[1];
    *dist = sqf(cx) + sqf(cy);

    return;
  }

  // distance from center
  const float cx = x - gpt->points[0];
  const float cy = y - gpt->points[1];
  *dist = sqf(cx) + sqf(cy);

  // we check if it's inside borders
  if(!dt_masks_point_in_form_exact(x, y, gpt->border, 1, gpt->border_count)) return;
  *inside = 1;
  *near = 0;

  // and we check if it's inside form
  *inside_border = !(dt_masks_point_in_form_near(x, y, gpt->points, 1, gpt->points_count, as, near));
}

static int _find_closest_handle(struct dt_iop_module_t *module, float pzx, float pzy, dt_masks_form_t *form, int parentid,
                                 dt_masks_form_gui_t *gui, int index)
{
  if(!gui) return 0;

  // get the zoom scale
  dt_develop_t *dev = (dt_develop_t *)darktable.develop;
  const float zoom_scale = dt_dev_get_zoom_level(dev);

  // we define a distance to the cursor for handle detection (in backbuf dimensions)
  const float dist_curs = DT_MASKS_SELECTION_DISTANCE / zoom_scale; // transformed to backbuf dimensions

  gui->form_selected = FALSE;
  gui->border_selected = FALSE;
  gui->source_selected = FALSE;
  gui->handle_selected = -1;

  pzx *= darktable.develop->preview_pipe->backbuf_width / dev->natural_scale;
  pzy *= darktable.develop->preview_pipe->backbuf_height / dev->natural_scale;

  int in, inside_border, near, inside_source;
  float dist;
  
  _circle_get_distance(pzx, pzy, dist_curs, gui, index, 0, &in, &inside_border, &near, &inside_source, &dist);

  if(inside_source)
  {
    gui->form_selected = TRUE;
    gui->source_selected = TRUE;
    return 1;
  }
  else if(inside_border)
  {
    gui->form_selected = TRUE;
    gui->border_selected = TRUE;
    return 1;
  }
  else if(in)
  {
    gui->form_selected = TRUE;
    return 1;
  }
  
  return 0;
}

static int _init_hardness(dt_masks_form_t *form, const float amount, const dt_masks_increment_t increment, const int flow)
{
  float mask_hardness = dt_masks_get_set_conf_value(form, "border", amount, HARDNESS_MIN, HARDNESS_MAX, increment, flow);
  dt_toast_log(_("Hardness: %3.2f%%"), mask_hardness * 100.0f);
  return 1;
}

static int _init_size(dt_masks_form_t *form, const float amount, const dt_masks_increment_t increment, const int flow)
{

  float mask_size = dt_masks_get_set_conf_value(form, "size", amount, HARDNESS_MIN, HARDNESS_MAX, increment, flow);
  dt_toast_log(_("Size: %3.2f%%"), mask_size * 2.f * 100.f);
  return 1;
}

static int _init_opacity(dt_masks_form_t *form, const float amount, const dt_masks_increment_t increment, const int flow)
{
  float mask_opacity = dt_masks_get_set_conf_value(form, "opacity", amount, 0.f, 1.f, increment, flow);
  dt_toast_log(_("Opacity: %3.2f%%"), mask_opacity*100.f);
  return 1;
}

static int _change_hardness(dt_masks_form_t *form, dt_masks_form_gui_t *gui, struct dt_iop_module_t *module, int index, const float amount, const dt_masks_increment_t increment, const int flow)
{
  
  dt_masks_node_circle_t *circle = (dt_masks_node_circle_t *)(form->points)->data;
  if(!circle) return 0;

  const float masks_hardness = circle->border;
  if(increment)
    circle->border = MAX(HARDNESS_MIN, MIN(masks_hardness * powf(amount, (float)flow), HARDNESS_MAX));
  else
    circle->border = MAX(HARDNESS_MIN, MIN(amount, HARDNESS_MAX));


  _init_hardness(form, amount, increment, flow);

  // we recreate the form points
  dt_masks_gui_form_remove(form, gui, index);
  dt_masks_gui_form_create(form, gui, index, module);

  return 1;
}

static int _change_size(dt_masks_form_t *form, dt_masks_form_gui_t *gui, struct dt_iop_module_t *module, int index, const float amount, const dt_masks_increment_t increment, const int flow)
{
  dt_masks_node_circle_t *circle = (dt_masks_node_circle_t *)(form->points)->data;
  if(!circle) return 0;

  // Sanitize
  // do not exceed upper limit of 1.0 and lower limit of 0.004
  if(amount > 1.0f && (circle->border > 1.0f ))
    return 1;

  // Growing/shrinking
  if(gui->node_selected == -1 || gui->node_selected == 0)
  {
    switch(increment)
    {
      case(DT_MASKS_INCREMENT_SCALE):
      {
        circle->radius *= powf(amount, (float)flow);
        break;
      }
      case(DT_MASKS_INCREMENT_OFFSET):
      {
        circle->radius += amount * (float)flow;
        break;
      }
      case(DT_MASKS_INCREMENT_ABSOLUTE):
      {
        circle->radius = amount;
      }
    }
  }

  _init_size(form, amount, increment, flow);

  // we recreate the form points
  dt_masks_gui_form_remove(form, gui, index);
  dt_masks_gui_form_create(form, gui, index, module);

  return 1;
}

static int _circle_events_mouse_scrolled(struct dt_iop_module_t *module, float pzx, float pzy, int up, const int flow,
                                         uint32_t state, dt_masks_form_t *form, int parentid,
                                         dt_masks_form_gui_t *gui, int index,
                                         dt_masks_interaction_t interaction)
{
  if(gui->creation)
  {
    if(dt_modifier_is(state, GDK_CONTROL_MASK))
      return _init_opacity(form, up ? +0.02f : -0.02f, DT_MASKS_INCREMENT_OFFSET, flow);
    else if(dt_modifier_is(state, GDK_SHIFT_MASK))
      return _init_hardness(form, up ? +1.02f : 0.98f, DT_MASKS_INCREMENT_SCALE, flow);
    else
      return _init_size(form, up ? +1.02f : 0.98f, DT_MASKS_INCREMENT_SCALE, flow);
  }
  else if(gui->form_selected)
  {
    if(dt_modifier_is(state, GDK_CONTROL_MASK))
      return dt_masks_form_set_opacity(form, parentid, up ? +0.02f : -0.02f, DT_MASKS_INCREMENT_OFFSET, flow);
    else if(dt_modifier_is(state, GDK_SHIFT_MASK))
      return _change_hardness(form, gui, module, index, up ? +1.02f : 0.98f, DT_MASKS_INCREMENT_SCALE, flow);
    else
      return _change_size(form, gui, module, index, up ? +1.02f : 0.98f, DT_MASKS_INCREMENT_SCALE, flow);
  }
  return 0;
}

static int _circle_events_button_pressed(struct dt_iop_module_t *module, float pzx, float pzy,
                                         double pressure, int which, int type, uint32_t state,
                                         dt_masks_form_t *form, int parentid, dt_masks_form_gui_t *gui, int index)
{
  if(!gui) return 0;

  _find_closest_handle(module, pzx, pzy, form, parentid, gui, index);


  if(which == 1)
  {
    if(gui->creation)
    {
      if((dt_modifier_is(state, GDK_CONTROL_MASK | GDK_SHIFT_MASK)) || dt_modifier_is(state, GDK_SHIFT_MASK))
      {
        // set some absolute or relative position for the source of the clone mask
        if(form->type & DT_MASKS_CLONE) dt_masks_set_source_pos_initial_state(gui, state, pzx, pzy);
        return 1;
      }

      dt_iop_module_t *crea_module = gui->creation_module;
      // we create the circle
      dt_masks_node_circle_t *circle = (dt_masks_node_circle_t *)(malloc(sizeof(dt_masks_node_circle_t)));

      // we change the center value
      dt_dev_roi_to_input_space(darktable.develop, TRUE, pzx, pzy, &circle->center[0], &circle->center[1]);

      if(form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE))
      {
        circle->radius = dt_conf_get_float("plugins/darkroom/spots/circle/size");
        circle->border = dt_conf_get_float("plugins/darkroom/spots/circle/border");

        // calculate the source position
        if(form->type & DT_MASKS_CLONE)
        {
          dt_masks_set_source_pos_initial_value(gui, form, pzx, pzy);
        }
        else
        {
          // not used by regular masks
          form->source[0] = form->source[1] = 0.0f;
        }
      }
      else
      {
        circle->radius = dt_conf_get_float("plugins/darkroom/masks/circle/size");
        circle->border = dt_conf_get_float("plugins/darkroom/masks/circle/border");
        // not used for masks
        form->source[0] = form->source[1] = 0.0f;
      }
      form->points = g_list_append(form->points, circle);
      dt_masks_gui_form_save_creation(darktable.develop, crea_module, form, gui);

      if(crea_module)
      {
        // we save the move
        dt_masks_set_edit_mode(crea_module, DT_MASKS_EDIT_FULL);
        dt_masks_iop_update(crea_module);
        dt_dev_masks_selection_change(darktable.develop, crea_module, form->formid, TRUE);
        gui->creation_module = NULL;
      }
      else
      {
        // we select the new form
        dt_dev_masks_selection_change(darktable.develop, NULL, form->formid, TRUE);
      }

      // if we draw a clone circle, we start now the source dragging
      if(form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE))
      {
        dt_masks_form_t *grp = darktable.develop->form_visible;
        if(!grp || !(grp->type & DT_MASKS_GROUP)) return 1;
        int pos3 = 0, pos2 = -1;
        for(GList *fs = grp->points; fs; fs = g_list_next(fs))
        {
          dt_masks_form_group_t *pt = (dt_masks_form_group_t *)fs->data;
          if(pt->formid == form->formid)
          {
            pos2 = pos3;
            break;
          }
          pos3++;
        }
        if(pos2 < 0) return 1;
        dt_masks_form_gui_t *gui2 = darktable.develop->form_gui;
        if(!gui2) return 1;
        if(form->type & DT_MASKS_CLONE)
          gui2->source_dragging = TRUE;
        else
          gui2->form_dragging = TRUE;
        gui2->group_selected = pos2;
        gui2->pos[0] = pzx * darktable.develop->preview_pipe->backbuf_width;
        gui2->pos[1] = pzy * darktable.develop->preview_pipe->backbuf_height;
        gui2->delta[0] = 0.0;
        gui2->delta[1] = 0.0;
        gui2->scrollx = pzx;
        gui2->scrolly = pzy;
        gui2->form_selected = TRUE; // we also want to be selected after button released

        dt_masks_select_form(module, dt_masks_get_from_id(darktable.develop, form->formid));
      }
      return 1;
    }
    else // creation is FALSE
    {
      dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
      if(!gpt) return 0;

      if(gui->source_selected && gui->edit_mode == DT_MASKS_EDIT_FULL)
      {
        // we start the source dragging
        gui->source_dragging = TRUE;
        gui->delta[0] = gpt->source[0] - gui->pos[0];
        gui->delta[1] = gpt->source[1] - gui->pos[1];
        return 1;
      }
      else if(gui->form_selected && gui->edit_mode == DT_MASKS_EDIT_FULL)
      {
        // we start the form dragging
        gui->form_dragging = TRUE;
        gui->delta[0] = gpt->points[0] - gui->pos[0];
        gui->delta[1] = gpt->points[1] - gui->pos[1];
        return 1;
      }
      else if(gui->handle_selected && gui->edit_mode == DT_MASKS_EDIT_FULL)
      {
        gui->handle_dragging = gui->handle_selected;

        return 1;
      }
    }
  }

  else if(gui->creation && which == 3)
  {
    dt_masks_set_edit_mode(module, DT_MASKS_EDIT_FULL);
    dt_masks_iop_update(module);

    return 1;
  }

  return 0;
}

static int _circle_events_button_released(struct dt_iop_module_t *module, float pzx, float pzy, int which,
                                          uint32_t state, dt_masks_form_t *form, int parentid,
                                          dt_masks_form_gui_t *gui, int index)
{
    if(gui->form_dragging)
    {
      // we end the form dragging
      gui->form_dragging = FALSE;
      return 1;
    }
    else if(gui->source_dragging)
    {
      // we end the form dragging
      gui->source_dragging = FALSE;

      // select the source as default, if the mouse is not moved we are inside the
      // source and so want to move the source.
      gui->form_selected = TRUE;
      gui->source_selected = TRUE;
      gui->border_selected = FALSE;

      return 1;
    }
  return 0;
}

static int _circle_events_mouse_moved(struct dt_iop_module_t *module, float pzx, float pzy, double pressure,
                                      int which, dt_masks_form_t *form, int parentid,
                                      dt_masks_form_gui_t *gui, int index)
{
  if(!gui) return 0;

  if(gui->creation)
  {
    // Let the cursor motion be redrawn as it moves in GUI
    return 1;
  }
  else if(gui->form_dragging || gui->source_dragging)
  {
    dt_develop_t *dev = (dt_develop_t *)darktable.develop;
    // apply delta to the current mouse position
    float pts[2] = { -1 , -1 };
    const float pointer[2] = { pzx, pzy };
    dt_dev_roi_delta_to_input_space(dev, gui->delta, pointer, pts);

    // we move all points in normalized input space
    if(gui->form_dragging)
    {
      dt_masks_node_circle_t *circle = (dt_masks_node_circle_t *)((form->points)->data);
      if(!circle) return 0;
      circle->center[0] = pts[0];
      circle->center[1] = pts[1];
    }
    else if(gui->source_dragging)
    {
      form->source[0] = pts[0];
      form->source[1] = pts[1];
    }

    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index, module);

    return 1;
  }

  if(_find_closest_handle(module, pzx, pzy, form, parentid, gui, index)) return 1;
  if(gui->edit_mode != DT_MASKS_EDIT_FULL) return 0;
  return 1;
}

static void _circle_draw_shape(cairo_t *cr, const float *points, const int points_count, const int coord_nb, const gboolean border, const gboolean source)
{
  cairo_move_to(cr, points[coord_nb * 2 + 2], points[coord_nb * 2 + 3]);
  for(int i = 2; i < points_count; i++)
    cairo_line_to(cr, points[i * 2], points[i * 2 + 1]);
  cairo_close_path(cr);
}

static float *_points_to_transform(float x, float y, float radius, float wd, float ht, int *points_count)
{
  // how many points do we need?
  const float r = radius * MIN(wd, ht);
  const size_t l = (size_t)(2.0f * M_PI * r);
  // allocate buffer
  float *const restrict points = dt_alloc_align_float((l + 1) * 2);
  if(!points)
  {
    *points_count = 0;
    return NULL;
  }
  *points_count = l + 1;

  // now we set the points, first the center, then the circumference
  const float center_x = x * wd;
  const float center_y = y * ht;
  points[0] = center_x;
  points[1] = center_y;
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(l, points, center_x, center_y, r)      \
    schedule(static) if(l > 100) aligned(points:64)
#endif
  for(int i = 1; i < l + 1; i++)
  {
    const float alpha = (i - 1) * 2.0f * M_PI / (float)l;
    points[i * 2] = center_x + r * cosf(alpha);
    points[i * 2 + 1] = center_y + r * sinf(alpha);
  }
  return points;
}

static int _circle_get_points_source(dt_develop_t *dev, float x, float y, float xs, float ys, float radius,
                                     float radius2, float rotation, float **points, int *points_count,
                                     const dt_iop_module_t *module)
{
  (void)radius2; // keep compiler from complaining about unused arg
  (void)rotation;
  const float wd = dev->preview_pipe->iwidth;
  const float ht = dev->preview_pipe->iheight;

  // compute the points of the target (center and circumference of circle)
  // we get the point in RAW image reference
  *points = _points_to_transform(x, y, radius, wd, ht, points_count);
  if(!*points) return 0;

  // we transform with all distortion that happen *before* the module
  // so we have now the TARGET points in module input reference
  if(dt_dev_distort_transform_plus(dev, dev->preview_pipe, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_EXCL,
                                   *points, *points_count))
  {
    // now we move all the points by the shift
    // so we have now the SOURCE points in module input reference
    float pts[2] = { xs * wd, ys * ht };
    if(dt_dev_distort_transform_plus(dev, dev->preview_pipe, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_EXCL,
                                     pts, 1))
    {
      const float dx = pts[0] - (*points)[0];
      const float dy = pts[1] - (*points)[1];
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(points_count, points, dx, dy)              \
    schedule(static) if(*points_count > 100) aligned(points:64)
#endif
      for(int i = 0; i < *points_count; i++)
      {
        (*points)[i * 2] += dx;
        (*points)[i * 2 + 1] += dy;
      }

      // we apply the rest of the distortions (those after the module)
      // so we have now the SOURCE points in final image reference
      if(dt_dev_distort_transform_plus(dev, dev->preview_pipe, module->iop_order, DT_DEV_TRANSFORM_DIR_FORW_INCL,
                                       *points, *points_count))
        return 1;
    }
  }

  // if we failed, then free all and return
  dt_free_align(*points);
  *points = NULL;
  *points_count = 0;
  return 0;
}

static int _circle_get_points(dt_develop_t *dev, float x, float y, float radius, float radius2, float rotation,
                              float **points, int *points_count)
{
  (void)radius2; // keep compiler from complaining about unused arg
  (void)rotation;
  const float wd = dev->preview_pipe->iwidth;
  const float ht = dev->preview_pipe->iheight;

  // compute the points we need to transform (center and circumference of circle)
  *points = _points_to_transform(x, y, radius, wd, ht, points_count);
  if(!*points) return 0;

  // and transform them with all distorted modules
  if(dt_dev_distort_transform(dev, *points, *points_count)) return 1;

  // if we failed, then free all and return
  dt_free_align(*points);
  *points = NULL;
  *points_count = 0;
  return 0;
}

static void _circle_events_post_expose(cairo_t *cr, float zoom_scale, dt_masks_form_gui_t *gui, int index, int num_points)
{
  const dt_develop_t *const dev = (const dt_develop_t *)darktable.develop;
  if(!gui || !dev) return;

  // add a preview when creating a circle
  // in creation mode
  if(gui->creation)
  {
    dt_masks_form_t *form = darktable.develop->form_visible;
    if(!form) return;

    // we get the default radius values
    float radius_shape = 0.0f;
    float radius_border = 0.0f;
    if(form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE))
    {
      radius_shape = dt_conf_get_float("plugins/darkroom/spots/circle/size");
      radius_border = dt_conf_get_float("plugins/darkroom/spots/circle/border");
      fprintf(stderr, "Get mask config key %s to value %f\n", "plugins/darkroom/spots/circle/size", radius_shape);
    }
    else
    {
      radius_shape = dt_conf_get_float("plugins/darkroom/masks/circle/size");
      radius_border = dt_conf_get_float("plugins/darkroom/masks/circle/border");
    }
    radius_border += radius_shape;

    // we get the circle center at mouse position
    float xpos = gui->pos[0];
    float ypos = gui->pos[1];
    // fallback to center of the current view
    if((xpos == -1.f && ypos == -1.f) || gui->mouse_leaved_center)
    {
      xpos = (.5f + dev->x) * dev->preview_pipe->backbuf_width;
      ypos = (.5f + dev->y) * dev->preview_pipe->backbuf_height;
    }
    // we backtransform the point to get them in input space
    float back_pts[2] = { xpos, ypos };
    dt_dev_distort_backtransform(darktable.develop, back_pts, 1);
    // normalize
    float x = back_pts[0] / dev->preview_pipe->iwidth;
    float y = back_pts[1] / dev->preview_pipe->iheight;
    // we get all the points, distorted if needed, of the sample form
    float *points = NULL;
    int points_count = 0;
    float *border = NULL;
    int border_count = 0;
    int draw = _circle_get_points(darktable.develop, x, y, radius_shape, 0.0, 0.0, &points, &points_count);
    if(draw && radius_shape != radius_border)
    {
      draw = _circle_get_points(darktable.develop, x, y, radius_border, 0.0, 0.0, &border, &border_count);
    }
    if(!draw) return;

    // we draw the form and it's border

    // we draw the main shape
    dt_masks_draw_lines(DT_MASKS_NO_DASH, FALSE, cr, num_points, FALSE, zoom_scale, points, points_count, &dt_masks_functions_circle);
    // we draw the borders
    dt_masks_draw_lines(DT_MASKS_DASH_STICK, FALSE, cr, num_points, FALSE, zoom_scale, border, border_count, &dt_masks_functions_circle);

    // draw a cross where the source will be created
    if(form->type & DT_MASKS_CLONE)
    {
      float pts[2] = { 0.0, 0.0 };
      dt_masks_calculate_source_pos_value(gui, DT_MASKS_CIRCLE, xpos, ypos, xpos, ypos, &pts[0], &pts[1], FALSE);
      dt_masks_draw_clone_source_pos(cr, zoom_scale, pts[0], pts[1]);
    }

    if(points) dt_free_align(points);
    if(border) dt_free_align(border);
  

    return;
  } // creation

  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return;
  
  // we draw the main shape
  const gboolean selected = (gui->group_selected == index) && (gui->form_selected || gui->form_dragging);
  dt_masks_draw_lines(DT_MASKS_NO_DASH, FALSE, cr, num_points, selected, zoom_scale, gpt->points, gpt->points_count, &dt_masks_functions_circle);
  // we draw the borders
  if(gui->group_selected == index)
  { 
    dt_masks_draw_lines(DT_MASKS_DASH_STICK, FALSE, cr, num_points, (gui->border_selected), zoom_scale, gpt->border,
                       gpt->border_count, &dt_masks_functions_circle);
  }

  // draw the source if any
  if(gpt->source_count > 6)
  { 
    dt_masks_draw_source(cr, gui, index, num_points, zoom_scale, &dt_masks_functions_circle);
  }
}

static void _bounding_box(const float *const points, int num_points, int *width, int *height, int *posx, int *posy)
{
  // search for min/max X and Y coordinates
  float xmin = FLT_MAX, xmax = FLT_MIN, ymin = FLT_MAX, ymax = FLT_MIN;
  for(int i = 1; i < num_points; i++) // skip point[0], which is circle's center
  {
    xmin = fminf(points[i * 2], xmin);
    xmax = fmaxf(points[i * 2], xmax);
    ymin = fminf(points[i * 2 + 1], ymin);
    ymax = fmaxf(points[i * 2 + 1], ymax);
  }
  // set the min/max values we found
  *posx = xmin;
  *posy = ymin;
  *width = (xmax - xmin);
  *height = (ymax - ymin);
}

static int _circle_get_points_border(dt_develop_t *dev, struct dt_masks_form_t *form, float **points,
                                     int *points_count, float **border, int *border_count, int source,
                                     const dt_iop_module_t *module)
{
  dt_masks_node_circle_t *circle = (dt_masks_node_circle_t *)((form->points)->data);
  float x = circle->center[0];
  float y = circle->center[1];
  if(source)
  {
    float xs = form->source[0];
    float ys = form->source[1];
    return _circle_get_points_source(dev, x, y, xs, ys, circle->radius, circle->radius, 0, points, points_count, module);
  }
  else
  {
    if(form->functions->get_points(dev, x, y, circle->radius, circle->radius, 0, points, points_count))
    {
      if(border)
      {
        float outer_radius = circle->radius + circle->border;
        return form->functions->get_points(dev, x, y, outer_radius, outer_radius, 0, border, border_count);
      }
      else
        return 1;
    }
  }
  return 0;
}

static int _circle_get_source_area(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                                   dt_masks_form_t *form, int *width, int *height, int *posx, int *posy)
{
  // we get the circle values
  dt_masks_node_circle_t *circle = (dt_masks_node_circle_t *)((form->points)->data);
  float wd = piece->pipe->iwidth, ht = piece->pipe->iheight;

  // compute the points we need to transform (center and circumference of circle)
  const float outer_radius = circle->radius + circle->border;
  int num_points;
  float *const restrict points =
    _points_to_transform(form->source[0], form->source[1], outer_radius, wd, ht, &num_points);
  if(points == NULL)
    return 0;

  // and transform them with all distorted modules
  if(!dt_dev_distort_transform_plus(darktable.develop, piece->pipe, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, points, num_points))
  {
    dt_free_align(points);
    return 0;
  }

  _bounding_box(points, num_points, width, height, posx, posy);
  dt_free_align(points);
  return 1;
}

static int _circle_get_area(const dt_iop_module_t *const restrict module,
                            const dt_dev_pixelpipe_iop_t *const restrict piece,
                            dt_masks_form_t *const restrict form,
                            int *width, int *height, int *posx, int *posy)
{
  // we get the circle values
  dt_masks_node_circle_t *circle = (dt_masks_node_circle_t *)((form->points)->data);
  float wd = piece->pipe->iwidth, ht = piece->pipe->iheight;

  // compute the points we need to transform (center and circumference of circle)
  const float outer_radius = circle->radius + circle->border;
  int num_points;
  float *const restrict points =
    _points_to_transform(circle->center[0], circle->center[1], outer_radius, wd, ht, &num_points);
  if(points == NULL)
    return 0;

  // and transform them with all distorted modules
  if(!dt_dev_distort_transform_plus(module->dev, piece->pipe, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, points, num_points))
  {
    dt_free_align(points);
    return 0;
  }

  _bounding_box(points, num_points, width, height, posx, posy);
  dt_free_align(points);
  return 1;
}

static int _circle_get_mask(const dt_iop_module_t *const restrict module,
                            const dt_dev_pixelpipe_iop_t *const restrict piece,
                            dt_masks_form_t *const restrict form,
                            float **buffer, int *width, int *height, int *posx, int *posy)
{
  double start2 = 0.0;
  if(darktable.unmuted & DT_DEBUG_PERF) start2 = dt_get_wtime();

  // we get the area
  if(!_circle_get_area(module, piece, form, width, height, posx, posy)) return 0;

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle area took %0.04f sec\n", form->name, dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we get the circle values
  dt_masks_node_circle_t *const restrict circle = (dt_masks_node_circle_t *)((form->points)->data);

  // we create a buffer of points with all points in the area
  const int w = *width, h = *height;
  float *const restrict points = dt_alloc_align_float((size_t)w * h * 2);
  if(points == NULL)
    return 0;

  const float pos_x = *posx;
  const float pos_y = *posy;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(h, w) \
  dt_omp_sharedconst(points, pos_x, pos_y) \
  schedule(static) if(h*w > 50000) num_threads(MIN(darktable.num_openmp_threads,(h*w)/20000))
#endif
  for(int i = 0; i < h; i++)
  {
    float *const restrict p = points + 2 * i * w;
    const float y = i + pos_y;
#ifdef _OPENMP
#pragma omp simd aligned(points : 64)
#endif
    for(int j = 0; j < w; j++)
    {
      p[2*j] = pos_x + j;
      p[2*j + 1] = y;
    }
  }
  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle draw took %0.04f sec\n", form->name, dt_get_wtime() - start2);

    start2 = dt_get_wtime();
  }
  // we back transform all this points
  if(!dt_dev_distort_backtransform_plus(module->dev, piece->pipe, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, points, (size_t)w * h))
  {
    dt_free_align(points);
    return 0;
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle transform took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we allocate the buffer
  *buffer = dt_alloc_align_float((size_t)w * h);
  if(*buffer == NULL)
  {
    dt_free_align(points);
    return 0;
  }

  // we populate the buffer
  float *const restrict ptbuffer = *buffer;
  const int wi = piece->pipe->iwidth, hi = piece->pipe->iheight;
  const int mindim = MIN(wi, hi);
  const float centerx = circle->center[0] * wi;
  const float centery = circle->center[1] * hi;
  const float radius2 = circle->radius * mindim * circle->radius * mindim;
  const float total2 = (circle->radius + circle->border) * mindim * (circle->radius + circle->border) * mindim;
  const float border2 = total2 - radius2;
  const float *const points_y = points + 1;
#ifdef _OPENMP
#pragma omp parallel for default(none)  \
  dt_omp_firstprivate(h, w) \
  dt_omp_sharedconst(border2, total2, centerx, centery, points, points_y, ptbuffer) \
  schedule(simd:static) if(h*w > 50000) num_threads(MIN(darktable.num_openmp_threads,(h*w)/20000))
#endif
  for(int i = 0 ; i < h*w; i++)
  {
    // find the square of the distance from the center
    const float l2 = sqf(points[2 * i] - centerx) + sqf(points_y[2 * i] - centery);
    // quadratic falloff between the circle's radius and the radius of the outside of the feathering
    const float ratio = (total2 - l2) / border2;
    // enforce 1.0 inside the circle and 0.0 outside the feathering
    const float f = CLIP(ratio);
    ptbuffer[i] = sqf(f);
  }

  dt_free_align(points);

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle fill took %0.04f sec\n", form->name, dt_get_wtime() - start2);

  return 1;
}


static int _circle_get_mask_roi(const dt_iop_module_t *const restrict module,
                                const dt_dev_pixelpipe_iop_t *const restrict piece,
                                dt_masks_form_t *const form, const dt_iop_roi_t *const roi,
                                float *const restrict buffer)
{
  if(!module) return 0;
  double start1 = 0.0;
  double start2 = start1;
  
  if(darktable.unmuted & DT_DEBUG_PERF) start2 = start1 = dt_get_wtime();

  // we get the circle parameters
  dt_masks_node_circle_t *circle = (dt_masks_node_circle_t *)((form->points)->data);
  if(!circle) return 0;
  const int wi = piece->pipe->iwidth, hi = piece->pipe->iheight;
  const float centerx = circle->center[0] * wi;
  const float centery = circle->center[1] * hi;
  const int min_dimention = MIN(wi, hi);
  const float total_radius = (circle->radius + circle->border) * min_dimention;
  const float sqr_radius = circle->radius * min_dimention * circle->radius * min_dimention;
  const float sqr_total = total_radius * total_radius;
  const float sqr_border = sqr_total - sqr_radius;

  // we create a buffer of grid points for later interpolation: higher speed and reduced memory footprint;
  // we match size of buffer to bounding box around the shape
  const int width = roi->width;
  const int height = roi->height;
  const int px = roi->x;
  const int py = roi->y;
  const float iscale = 1.0f / roi->scale;
  const int grid = CLAMP((10.0f * roi->scale + 2.0f) / 3.0f, 1, 4); // scale dependent resolution
  const int grid_width = (width + grid - 1) / grid + 1;  // grid dimension of total roi
  const int grid_height = (height + grid - 1) / grid + 1;  // grid dimension of total roi

  // initialize output buffer with zero
  memset(buffer, 0, sizeof(float) * width * height);

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle init took %0.04f sec\n", form->name, dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we look at the outer circle of the shape - no effects outside of this circle;
  // we need many points as we do not know how the circle might get distorted in the pixelpipe
  const size_t circpts = dt_masks_roundup(MIN(360, 2 * M_PI * sqr_total), 8);
  float *const restrict circ = dt_alloc_align_float(circpts * 2);
  if(circ == NULL) return 0;

#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(circpts, centerx, centery, total_radius) \
  dt_omp_sharedconst(circ) schedule(static) if(circpts/8 > 1000)
#else
#pragma omp parallel for shared(points) schedule(static)
#endif
#endif
  for(int n = 0; n < circpts / 8; n++)
  {
    const float phi = (2.0f * M_PI * n) / circpts;
    const float x = total_radius * cosf(phi);
    const float y = total_radius * sinf(phi);
    const float cx = centerx;
    const float cy = centery;
    const int index_x = 2 * n * 8;
    const int index_y = 2 * n * 8 + 1;
    // take advantage of symmetry
    circ[index_x] = cx + x;
    circ[index_y] = cy + y;
    circ[index_x + 2] = cx + x;
    circ[index_y + 2] = cy - y;
    circ[index_x + 4] = cx - x;
    circ[index_y + 4] = cy + y;
    circ[index_x + 6] = cx - x;
    circ[index_y + 6] = cy - y;
    circ[index_x + 8] = cx + y;
    circ[index_y + 8] = cy + x;
    circ[index_x + 10] = cx + y;
    circ[index_y + 10] = cy - x;
    circ[index_x + 12] = cx - y;
    circ[index_y + 12] = cy + x;
    circ[index_x + 14] = cx - y;
    circ[index_y + 14] = cy - x;
  }

  // we transform the outer circle from input image coordinates to current point in pixelpipe
  if(!dt_dev_distort_transform_plus(module->dev, piece->pipe, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, circ,
                                        circpts))
  {
    dt_free_align(circ);
    return 0;
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle outline took %0.04f sec\n", form->name, dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we get the min/max values ...
  float xmin = FLT_MAX, ymin = FLT_MAX, xmax = FLT_MIN, ymax = FLT_MIN;
  for(int n = 0; n < circpts; n++)
  {
    // just in case that transform throws surprising values
    if(!(isnormal(circ[2 * n]) && isnormal(circ[2 * n + 1]))) continue;

    xmin = MIN(xmin, circ[2 * n]);
    xmax = MAX(xmax, circ[2 * n]);
    ymin = MIN(ymin, circ[2 * n + 1]);
    ymax = MAX(ymax, circ[2 * n + 1]);
  }

#if 0
  printf("xmin %f, xmax %f, ymin %f, ymax %f\n", xmin, xmax, ymin, ymax);
  printf("wi %d, hi %d, iscale %f\n", wi, hi, iscale);
  printf("w %d, h %d, px %d, py %d\n", w, h, px, py);
#endif

  // ... and calculate the bounding box with a bit of reserve
  const int bbxm = CLAMP((int)floorf(xmin / iscale - px) / grid - 1, 0, grid_width - 1);
  const int bbXM = CLAMP((int)ceilf(xmax / iscale - px) / grid + 2, 0, grid_width - 1);
  const int bbym = CLAMP((int)floorf(ymin / iscale - py) / grid - 1, 0, grid_height - 1);
  const int bbYM = CLAMP((int)ceilf(ymax / iscale - py) / grid + 2, 0, grid_height - 1);
  const int bbw = bbXM - bbxm + 1;
  const int bbh = bbYM - bbym + 1;

#if 0
  printf("bbxm %d, bbXM %d, bbym %d, bbYM %d\n", bbxm, bbXM, bbym, bbYM);
  printf("gw %d, gh %d, bbw %d, bbh %d\n", gw, gh, bbw, bbh);
#endif

  dt_free_align(circ);

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle bounding box took %0.04f sec\n", form->name, dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // check if there is anything to do at all;
  // only if width and height of bounding box is 2 or greater the shape lies inside of roi and requires action
  if(bbw <= 1 || bbh <= 1)
    return 1;

  float *const restrict points = dt_alloc_align_float((size_t)bbw * bbh * 2);
  if(points == NULL) return 0;

  // we populate the grid points in module coordinates
#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(iscale, bbxm, bbym, bbXM, bbYM, bbw, px, py, grid) \
  dt_omp_sharedconst(points) \
  schedule(static) collapse(2) if(bbw*bbh > 50000)
#else
#pragma omp parallel for shared(points)
#endif
#endif
  for(int j = bbym; j <= bbYM; j++)
    for(int i = bbxm; i <= bbXM; i++)
    {
      const size_t index = (size_t)(j - bbym) * bbw + i - bbxm;
      points[index * 2] = (grid * i + px) * iscale;
      points[index * 2 + 1] = (grid * j + py) * iscale;
    }

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle grid took %0.04f sec\n", form->name, dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we back transform all these points to the input image coordinates
  if(!dt_dev_distort_backtransform_plus(module->dev, piece->pipe, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, points,
                                        (size_t)bbw * bbh))
  {
    dt_free_align(points);
    return 0;
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle transform took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we calculate the mask values at the transformed points;
  // for results: re-use the points array
#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(bbh, bbw, centerx, centery, sqr_border, sqr_total) \
  dt_omp_sharedconst(points) \
  schedule(static) collapse(2) if(bbh*bbw > 50000) num_threads(MIN(darktable.num_openmp_threads,(height*width)/20000))
#else
#pragma omp parallel for shared(points)
#endif
#endif
  for(int j = 0; j < bbh; j++)
    for(int i = 0; i < bbw; i++)
    {
      const size_t index = (size_t)j * bbw + i;
      // find the square of the distance from the center
      const float l2 = sqf(points[2 * index] - centerx) + sqf(points[2 * index + 1] - centery);
      // quadratic falloff between the circle's radius and the radius of the outside of the feathering
      const float ratio = (sqr_total - l2) / sqr_border;
      // enforce 1.0 inside the circle and 0.0 outside the feathering
      const float f = CLAMP(ratio, 0.0f, 1.0f);
      points[2*index] = f * f;
    }

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle draw took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we fill the pre-initialized output buffer by interpolation;
  // we only need to take the contents of our bounding box into account
  const int endx = MIN(width, bbXM * grid);
  const int endy = MIN(height, bbYM * grid);
#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(grid, bbxm, bbym, bbw, endx, endy, width) \
  dt_omp_sharedconst(buffer, points) schedule(static)
#else
#pragma omp parallel for shared(buffer)
#endif
#endif
  for(int j = bbym * grid; j < endy; j++)
  {
    const int jj = j % grid;
    const int mj = j / grid - bbym;
    for(int i = bbxm * grid; i < endx; i++)
    {
      const int ii = i % grid;
      const int mi = i / grid - bbxm;
      const size_t mindex = (size_t)mj * bbw + mi;
      buffer[(size_t)j * width + i]
          = (points[mindex * 2] * (grid - ii) * (grid - jj) + points[(mindex + 1) * 2] * ii * (grid - jj)
             + points[(mindex + bbw) * 2] * (grid - ii) * jj + points[(mindex + bbw + 1) * 2] * ii * jj)
            / (grid * grid);
    }
  }

  dt_free_align(points);

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle fill took %0.04f sec\n", form->name, dt_get_wtime() - start2);
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle total render took %0.04f sec\n", form->name,
             dt_get_wtime() - start1);
  }

  return 1;
}

static void _circle_sanitize_config(dt_masks_type_t type)
{
  if(type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE))
  {
    dt_conf_get_and_sanitize_float("plugins/darkroom/spots/circle/size", 0.001f, 0.5f);
    dt_conf_get_and_sanitize_float("plugins/darkroom/spots/circle/border", 0.0005f, 0.5f);
  }
  else
  {
    dt_conf_get_and_sanitize_float("plugins/darkroom/masks/circle/size", 0.001f, 0.5f);
    dt_conf_get_and_sanitize_float("plugins/darkroom/masks/circle/border", 0.0005f, 0.5f);
  }
}

static void _circle_set_form_name(struct dt_masks_form_t *const form, const size_t nb)
{
  snprintf(form->name, sizeof(form->name), _("circle #%d"), (int)nb);
}

static void _circle_set_hint_message(const dt_masks_form_gui_t *const gui, const dt_masks_form_t *const form,
                                     const int opacity, char *const restrict msgbuf, const size_t msgbuf_len)
{
  // circle has same controls on creation and on edit
  g_snprintf(msgbuf, msgbuf_len,
             _("<b>size</b>: scroll, <b>feather size</b>: shift+scroll\n"
               "<b>opacity</b>: ctrl+scroll (%d%%)"), opacity);
}

static void _circle_duplicate_points(dt_develop_t *dev, dt_masks_form_t *const base, dt_masks_form_t *const dest)
{
  (void)dev; // unused arg, keep compiler from complaining
  for(GList *pts = base->points; pts; pts = g_list_next(pts))
  {
    dt_masks_node_circle_t *pt = (dt_masks_node_circle_t *)pts->data;
    dt_masks_node_circle_t *npt = (dt_masks_node_circle_t *)malloc(sizeof(dt_masks_node_circle_t));
    memcpy(npt, pt, sizeof(dt_masks_node_circle_t));
    dest->points = g_list_append(dest->points, npt);
  }
}

static void _circle_initial_source_pos(const float iwd, const float iht, float *x, float *y)
{
  const float radius = MIN(0.5f, dt_conf_get_float("plugins/darkroom/spots/circle/size"));

  *x = (radius * iwd);
  *y = -(radius * iht);
}

// The function table for circles.  This must be public, i.e. no "static" keyword.
const dt_masks_functions_t dt_masks_functions_circle = {
  .point_struct_size = sizeof(struct dt_masks_node_circle_t),
  .sanitize_config = _circle_sanitize_config,
  .set_form_name = _circle_set_form_name,
  .set_hint_message = _circle_set_hint_message,
  .duplicate_points = _circle_duplicate_points,
  .initial_source_pos = _circle_initial_source_pos,
  .get_distance = _circle_get_distance,
  .get_points = _circle_get_points,
  .get_points_border = _circle_get_points_border,
  .get_mask = _circle_get_mask,
  .get_mask_roi = _circle_get_mask_roi,
  .get_area = _circle_get_area,
  .get_source_area = _circle_get_source_area,
  .mouse_moved = _circle_events_mouse_moved,
  .mouse_scrolled = _circle_events_mouse_scrolled,
  .button_pressed = _circle_events_button_pressed,
  .button_released = _circle_events_button_released,
  .post_expose = _circle_events_post_expose,
  .draw_shape = _circle_draw_shape
};



// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
