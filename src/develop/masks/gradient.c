/*
    This file is part of darktable,
    Copyright (C) 2013, 2016, 2019-2021 Pascal Obry.
    Copyright (C) 2013-2017 Tobias Ellinghaus.
    Copyright (C) 2013-2015, 2019-2020 Ulrich Pegelow.
    Copyright (C) 2014-2016, 2021 Roman Lebedev.
    Copyright (C) 2016, 2019, 2021 Aldric Renaudin.
    Copyright (C) 2018 Edgardo Hoszowski.
    Copyright (C) 2018 johannes hanika.
    Copyright (C) 2019 Andreas Schneider.
    Copyright (C) 2019, 2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2020-2022 Chris Elston.
    Copyright (C) 2020 GrahamByrnes.
    Copyright (C) 2020-2021 Hubert Kowalski.
    Copyright (C) 2020 Paolo DePetrillo.
    Copyright (C) 2020-2021 Ralf Brown.
    Copyright (C) 2022 Martin Bařinka.
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
#include "bauhaus/bauhaus.h"
#include "common/debug.h"
#include "common/undo.h"
#include "control/conf.h"
#include "develop/blend.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "develop/openmp_maths.h"

#define extent_MIN 0.0005f
#define extent_MAX 1.0f
#define CURVATURE_MIN -2.0f
#define CURVATURE_MAX 2.0f

#define BORDER_MIN 0.00005f
#define BORDER_MAX 0.5f

// Helper function to find the INFINITY separator in border array
static int _find_border_separator(const float *border, int count)
{

  if(!border || count <= 0) return -1;

#ifdef _OPENMP
  int found = count;
#pragma omp parallel for reduction(min:found)
  for(int i = 0; i < count; i++)
  {
    if(isinf(border[i * 2]) && isinf(border[i * 2 + 1]))
      found = i;
  }
  return (found == count) ? -1 : found;
#else
  for(int i = 0; i < count; i++)
  {
    if(isinf(border[i * 2]) && isinf(border[i * 2 + 1]))
      return i;
  }
  return -1;
#endif
}


// Helper function to find closest point on a line segment to a given point
static void _closest_point_on_segment(float px, float py, float x1, float y1, float x2, float y2,
                                     float *closest_x, float *closest_y, float *distance_sq)
{
  const float seg_dx = x2 - x1;
  const float seg_dy = y2 - y1;
  const float seg_length_sq = seg_dx * seg_dx + seg_dy * seg_dy;
  
  if(seg_length_sq < 1e-10f)
  {
    // Degenerate segment, return first point
    *closest_x = x1;
    *closest_y = y1;
    *distance_sq = (px - x1) * (px - x1) + (py - y1) * (py - y1);
    return;
  }
  
  // Project point onto line segment (clamped to [0,1])
  const float t = fmaxf(0.0f, fminf(1.0f, 
    ((px - x1) * seg_dx + (py - y1) * seg_dy) / seg_length_sq));
  
  *closest_x = x1 + t * seg_dx;
  *closest_y = y1 + t * seg_dy;
  *distance_sq = (px - *closest_x) * (px - *closest_x) + (py - *closest_y) * (py - *closest_y);
}

// Helper function to find closest point on a polyline to a given point
static void _closest_point_on_line(float px, float py, const float *border, int start_idx, int end_idx,
                  float *closest_x, float *closest_y, float *min_distance_sq)
{
  *min_distance_sq = FLT_MAX;
  *closest_x = *closest_y = 0.0f;

  if(start_idx >= end_idx - 1) return;

#ifdef _OPENMP
  float global_min = FLT_MAX;
  float global_x = 0.0f, global_y = 0.0f;

#pragma omp parallel
  {
  float local_min = FLT_MAX;
  float local_x = 0.0f, local_y = 0.0f;

#pragma omp for nowait
  for(int i = start_idx; i < end_idx - 1; i++)
  {
    float seg_closest_x, seg_closest_y, seg_dist_sq;
    _closest_point_on_segment(px, py,
                border[i * 2], border[i * 2 + 1],
                border[(i + 1) * 2], border[(i + 1) * 2 + 1],
                &seg_closest_x, &seg_closest_y, &seg_dist_sq);

    if(seg_dist_sq < local_min)
    {
      local_min = seg_dist_sq;
      local_x = seg_closest_x;
      local_y = seg_closest_y;
    }
  }

  if(local_min < global_min)
  {
#pragma omp critical
    {
      if(local_min < global_min)
      {
        global_min = local_min;
        global_x = local_x;
        global_y = local_y;
      }
    }
  }
  } // end parallel

  *min_distance_sq = global_min;
  *closest_x = global_x;
  *closest_y = global_y;
#else
  for(int i = start_idx; i < end_idx - 1; i++)
  {
    float seg_closest_x, seg_closest_y, seg_dist_sq;
    _closest_point_on_segment(px, py,
                border[i * 2], border[i * 2 + 1],
                border[(i + 1) * 2], border[(i + 1) * 2 + 1],
                &seg_closest_x, &seg_closest_y, &seg_dist_sq);

    if(seg_dist_sq < *min_distance_sq)
    {
      *min_distance_sq = seg_dist_sq;
      *closest_x = seg_closest_x;
      *closest_y = seg_closest_y;
    }
  }
#endif
}

static float _gradient_get_border_len_sq(const dt_masks_form_gui_points_t *gpt)
{
  const float gradient_dx = gpt->points[2] - gpt->points[0];
  const float gradient_dy = gpt->points[3] - gpt->points[1];
  return gradient_dx * gradient_dx + gradient_dy * gradient_dy;
}

static void _gradient_get_distance(float x, float y, float dist_mouse, dt_masks_form_gui_t *gui, int index,
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
  const float sqr_dist_mouse = dist_mouse * dist_mouse;

  const dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return;

  // check if we are between the two border lines
  if(!gui->form_rotating && !gui->form_dragging && gpt->border_count > 6 && gpt->points_count >= 4)
  {
    const int separator_idx = _find_border_separator(gpt->border, gpt->border_count);
    if(separator_idx > 0 && separator_idx < gpt->border_count - 1)
    {
      // Get gradient direction from segment (points[0],points[1]) to (points[2],points[3])
      const float gradient_len_sq = _gradient_get_border_len_sq(gpt);
      
      if(gradient_len_sq > 1e-12f)
      {
        // Find closest points on both lines
        float closest_x1, closest_y1, dist1_sq;
        float closest_x2, closest_y2, dist2_sq;
        
        _closest_point_on_line(x, y, gpt->border, 0, separator_idx, 
                              &closest_x1, &closest_y1, &dist1_sq);
        
        _closest_point_on_line(x, y, gpt->border, separator_idx + 1, gpt->border_count,
                              &closest_x2, &closest_y2, &dist2_sq);

        // Check if mouse is between the two closest points along gradient axis
        if(dist1_sq < FLT_MAX && dist2_sq < FLT_MAX)
        {
          // Vectors from mouse to each closest point
          const float to_line1_x = closest_x1 - x;
          const float to_line1_y = closest_y1 - y;
          const float to_line2_x = closest_x2 - x;
          const float to_line2_y = closest_y2 - y;

          const float gradient_dx = gpt->points[2] - gpt->points[0];
          const float gradient_dy = gpt->points[3] - gpt->points[1];
          // Project these vectors onto the (unnormalized) gradient direction.
          // Using the unnormalized direction preserves sign, so we avoid sqrt().
          const float proj1 = to_line1_x * gradient_dx + to_line1_y * gradient_dy;
          const float proj2 = to_line2_x * gradient_dx + to_line2_y * gradient_dy;

          // Mouse is between lines if projections have opposite signs
          if(proj1 * proj2 < 0.0f)
          {
            *inside_border = 1;

            const float min_dist_sq = fminf(dist1_sq, dist2_sq);
            if(min_dist_sq <= sqr_dist_mouse * 10)
            {
              *inside = 1;
            }
          }
        }
      }
    }
  }

  // and we check if we are near a segment (single continuous segment starting at gpt->points[3])
  if(gpt->points_count > 3)
  {
    for(int i = 3; i < gpt->points_count; i++)
    {
      const float xx = gpt->points[i * 2];
      const float yy = gpt->points[i * 2 + 1];

      const float dx = x - xx;
      const float dy = y - yy;
      const float dd = dx * dx + dy * dy;

      *dist = fminf(*dist, dd);

      // only one segment present: if any guide point is within the mouse distance,
      // mark the (only) segment as near (index 0)
      if(dd < sqr_dist_mouse)
        *near = 0;
    }
  }
}

static int _find_closest_handle(struct dt_iop_module_t *module, float pzx, float pzy, dt_masks_form_t *form, int parentid,
                                 dt_masks_form_gui_t *gui, int index)
{
  if(!gui) return 0;
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return 0;

  // get the zoom scale

  const dt_develop_t *dev = (const dt_develop_t *)darktable.develop;

  // we define a distance to the cursor for handle detection (in backbuf dimensions)
  const float dist_curs = DT_GUI_MOUSE_EFFECT_RADIUS_SCALED; // transformed to backbuf dimensions

  gui->form_selected = FALSE;
  gui->border_selected = FALSE;
  gui->source_selected = FALSE;
  gui->handle_selected = -1;
  gui->node_selected = -1;
  gui->seg_selected = -1;
  gui->handle_border_selected = -1;
  gui->pivot_selected = FALSE;
  const guint nb = g_list_length(form->points);

  pzx *= darktable.develop->preview_width / dev->natural_scale;
  pzy *= darktable.develop->preview_height / dev->natural_scale;

  // This allow to rotate if the mouse is at a certain distance from center
  /*if((gui->group_selected == index))
  {
    // are we away enough from the pivot ?
    const float dx = pzx - gpt->points[0];
    const float dy = pzy - gpt->points[1];
    const float distance_sq = dx * dx + dy * dy;
    const float rotation_dist = DT_DRAW_SELECTION_ROTATION_RADIUS(dev);
    const float threshold_sq = rotation_dist * rotation_dist;
    if(distance_sq >= threshold_sq && distance_sq <= (threshold_sq * 3.0f))
    {
      gui->pivot_selected = gui->form_selected = TRUE;
      return 1;
    }
  }*/

  // are we inside the form or the borders or near a segment ???
  int inside, inside_border, near, inside_source;
  float dist;
  _gradient_get_distance(pzx, pzy, dist_curs, gui, index, nb, &inside, &inside_border, &near, &inside_source, &dist);
  if(near >= 0)
    gui->seg_selected = near;
  else
  {
    if(inside)
    {
      gui->pivot_selected = gui->form_selected = TRUE;
      return 1;
    }
    else if(inside_border)
    {
      gui->form_selected = TRUE;
      gui->border_selected = TRUE;
      return 1;
    }
  }

  return 0;
}


static int _init_extent(dt_masks_form_t *form, const float amount, const dt_masks_increment_t increment, const int flow)
{
  float mask_hardness = dt_masks_get_set_conf_value(form, "extent", amount, extent_MIN, extent_MAX, increment, flow);
  dt_toast_log(_("extent: %3.2f%%"), mask_hardness * 100.0f);
  return 1;
}

static int _init_curvature(dt_masks_form_t *form, const float amount, const dt_masks_increment_t increment, const int flow)
{
  float mask_curvature = dt_masks_get_set_conf_value(form, "curvature", amount, CURVATURE_MIN, CURVATURE_MAX, increment, flow);
  dt_toast_log(_("Curvature: %3.2f%%"), mask_curvature * 50.f);
  return 1;
}

static int _init_opacity(dt_masks_form_t *form, const float amount, const dt_masks_increment_t increment, const int flow)
{
  float mask_opacity = dt_masks_get_set_conf_value(form, "opacity", amount, 0.f, 1.f, increment, flow);
  dt_toast_log(_("Opacity: %3.2f%%"), mask_opacity*100.f);
  return 1;
}

static int _init_rotation(dt_masks_form_t *form, const float amount, const dt_masks_increment_t increment, const int flow)
{
  float mask_angle = dt_masks_get_set_conf_value(form, "rotation", amount, 0.f, 360.f, increment, flow);
  dt_toast_log(_("Rotation: %3.2f\302\260"), mask_angle);
  return 1;
}

static int _change_extent(dt_masks_form_t *form, dt_masks_form_gui_t *gui, struct dt_iop_module_t *module, int index, const float amount, const dt_masks_increment_t increment, const int flow)
{
  if(!form || !form->points) return 0;
  dt_masks_anchor_gradient_t *gradient = (dt_masks_anchor_gradient_t *)(form->points)->data;
  if(!gradient) return 0;

  const float masks_extent = gradient->extent;
  if(increment)
    gradient->extent = MAX(extent_MIN, MIN(masks_extent * powf(amount, (float)flow), extent_MAX));
  else
    gradient->extent = MAX(extent_MIN, MIN(amount, extent_MAX));


  _init_extent(form, amount, increment, flow);

  // we recreate the form points
  dt_masks_gui_form_create(form, gui, index, module);

  return 1;
}

static int _change_curvature(dt_masks_form_t *form, dt_masks_form_gui_t *gui, struct dt_iop_module_t *module, int index, const float amount, const dt_masks_increment_t increment, const int flow)
{
  if(!form || !form->points) return 0;
  dt_masks_anchor_gradient_t *gradient = (dt_masks_anchor_gradient_t *)(form->points)->data;
  if(!gradient) return 0;

  // Sanitize
  // do not exceed upper limit of 2.0 and lower limit of -2.0
  if(amount > 2.0f && (gradient->curvature > 2.0f ))
    return 1;

  // bending
  if(gui->node_selected == -1 || gui->node_selected == 0)
  {
    switch(increment)
    {
      case(DT_MASKS_INCREMENT_SCALE):
      {
        gradient->curvature *= powf(amount, (float)flow);
        break;
      }
      case(DT_MASKS_INCREMENT_OFFSET):
      {
        gradient->curvature += amount * (float)flow;
        break;
      }
      case(DT_MASKS_INCREMENT_ABSOLUTE):
      {
        gradient->curvature = amount;
      }
    }
  }

  _init_curvature(form, amount, DT_MASKS_INCREMENT_SCALE, flow);

  // we recreate the form points
  dt_masks_gui_form_create(form, gui, index, module);

  return 1;
}

static int _change_rotation(dt_masks_form_t *form, dt_masks_form_gui_t *gui, struct dt_iop_module_t *module, int index, const float amount, const dt_masks_increment_t increment, const int flow)
{
  if(!form || !form->points) return 0;
  dt_masks_anchor_gradient_t *gradient = (dt_masks_anchor_gradient_t *)(form->points)->data;
  if(!gradient) return 0;

  // Rotation
  int flow_increased = (flow > 1) ? (flow - 1) * 5 : flow;
  switch(increment)
  {
    case(DT_MASKS_INCREMENT_SCALE):
    {
      gradient->rotation *= powf(amount, (float)flow_increased);
      break;
    }
    case(DT_MASKS_INCREMENT_OFFSET):
    {
      gradient->rotation += amount * (float)flow_increased;
      break;
    }
    case(DT_MASKS_INCREMENT_ABSOLUTE):
    {
      gradient->rotation = amount;
    }
  }

  // Ensure the rotation value warps within the interval [0, 360)
  if(gradient->rotation > 360.f) gradient->rotation = fmodf(gradient->rotation, 360.f);
  else if(gradient->rotation < 0.f) gradient->rotation = 360.f - fmodf(-gradient->rotation, 360.f);

  _init_rotation(form, amount, DT_MASKS_INCREMENT_OFFSET, flow);

  // we recreate the form points
  dt_masks_gui_form_create(form, gui, index, module);

  return 1;
}

static int _gradient_events_mouse_scrolled(struct dt_iop_module_t *module, float pzx, float pzy, int up, const int flow,
                                           uint32_t state, dt_masks_form_t *form, int parentid,
                                           dt_masks_form_gui_t *gui, int index, dt_masks_interaction_t interaction)
{
  if(gui->creation)
  {
    if(dt_modifier_is(state, GDK_SHIFT_MASK | GDK_CONTROL_MASK))
      return _init_rotation(form, (up ? +0.2f : -0.2f), DT_MASKS_INCREMENT_OFFSET, flow);
    else if(dt_modifier_is(state, GDK_CONTROL_MASK))
      return _init_opacity(form, up ? +0.02f : -0.02f, DT_MASKS_INCREMENT_OFFSET, flow);
    else if(dt_modifier_is(state, GDK_SHIFT_MASK))
      return _init_curvature(form, up ? +0.02f : -0.02f, DT_MASKS_INCREMENT_OFFSET, flow);
    else
      return _init_extent(form, (up ? +1.02f : 0.98f), DT_MASKS_INCREMENT_SCALE, flow); // simple scroll to adjust curvature, calling func adjusts opacity with Ctrl
  }
  else if(gui->form_selected  || gui->seg_selected >= 0 || gui->pivot_selected)
  {
    if(dt_modifier_is(state, GDK_SHIFT_MASK | GDK_CONTROL_MASK))
      return _change_rotation(form, gui, module, index, (up ? +0.2f : -0.2f), DT_MASKS_INCREMENT_OFFSET, flow);
    else if(dt_modifier_is(state, GDK_CONTROL_MASK))
      return dt_masks_form_change_opacity(form, parentid, up, flow);
    else if(dt_modifier_is(state, GDK_SHIFT_MASK))
      return _change_curvature(form, gui, module, index, (up ? +0.02f : -0.02f), DT_MASKS_INCREMENT_OFFSET, flow);
    else
      return _change_extent(form, gui, module, index, (up ? 1.02f : 0.98f), DT_MASKS_INCREMENT_SCALE, flow);
  }
  return 0;
}

static int _gradient_events_button_pressed(struct dt_iop_module_t *module, float pzx, float pzy,
                                           double pressure, int which, int type, uint32_t state,
                                           dt_masks_form_t *form, int parentid, dt_masks_form_gui_t *gui, int index)
{
  if(!gui) return 0;
  if(!form) return 0;

  // Do we need to refresh currently active node ?
  // Its requested to give back the focus when clicking outside current shape.
  _find_closest_handle(module, pzx, pzy, form, parentid, gui, index);

  if(gui->creation)
  {
    if(which == 1)
    {
      if(dt_modifier_is(state, GDK_SHIFT_MASK))
      {
        gui->gradient_toggling = TRUE;
        return 1;
      }

      dt_iop_module_t *crea_module = gui->creation_module;
      // we create the gradient
      dt_masks_anchor_gradient_t *gradient = (dt_masks_anchor_gradient_t *)(malloc(sizeof(dt_masks_anchor_gradient_t)));
      if(!gradient) return 0;
      // we change the center value
      dt_dev_roi_to_input_space(darktable.develop, TRUE, pzx, pzy, &gradient->center[0], &gradient->center[1]);

      gradient->extent = dt_conf_get_float("plugins/darkroom/masks/gradient/extent");
      gradient->curvature = dt_conf_get_float("plugins/darkroom/masks/gradient/curvature");
      gradient->rotation = dt_conf_get_float("plugins/darkroom/masks/gradient/rotation");

      form->points = g_list_append(form->points, gradient);
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
      
      return 1;
    }
  }

  else if(which == 1)
  {
    // double-click resets curvature
    if(type == GDK_2BUTTON_PRESS)
    {
      _change_curvature(form, gui, module, index, 0, DT_MASKS_INCREMENT_ABSOLUTE, 0);
      dt_masks_gui_form_create(form, gui, index, module);
      return 1;
    }

    const dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
    if(!gpt) return 0;

    else if((gui->form_selected || gui->seg_selected >= 0) && gui->edit_mode == DT_MASKS_EDIT_FULL)
    {
      // we start the form dragging or rotating
      if(gui->pivot_selected)
        gui->form_rotating = TRUE;
      else if(dt_modifier_is(state, GDK_SHIFT_MASK))
        gui->border_toggling = TRUE;
      else
        gui->form_dragging = TRUE;

      if(gui->form_rotating)
      {
        gui->delta[0] = gui->pos[0];
        gui->delta[1] = gui->pos[1];
      }
      else
      {
        gui->delta[0] = gpt->points[0] - gui->pos[0];
        gui->delta[1] = gpt->points[1] - gui->pos[1];
      }

      return 1;
    }
  }

  return 0;
}

static int _gradient_events_button_released(struct dt_iop_module_t *module, float pzx, float pzy, int which,
                                            uint32_t state, dt_masks_form_t *form, int parentid,
                                            dt_masks_form_gui_t *gui, int index)
{
  if(!form || !form->points) return 0;

  if(gui->form_dragging && gui->edit_mode == DT_MASKS_EDIT_FULL)
  {
    // we end the form dragging
    gui->form_dragging = FALSE;
    return 1;
  }

  else if(gui->form_rotating && gui->edit_mode == DT_MASKS_EDIT_FULL)
  {
    // we end the form rotating
    gui->form_rotating = FALSE;
    return 1;
  }
  else if(gui->gradient_toggling)
  {
    // we get the gradient
    dt_masks_anchor_gradient_t *gradient = (dt_masks_anchor_gradient_t *)((form->points)->data);
    if(!gradient) return 0;
    // we end the gradient toggling
    gui->gradient_toggling = FALSE;

    // toggle transition type of gradient
    if(gradient->state == DT_MASKS_GRADIENT_STATE_LINEAR)
      gradient->state = DT_MASKS_GRADIENT_STATE_SIGMOIDAL;
    else
      gradient->state = DT_MASKS_GRADIENT_STATE_LINEAR;

    dt_conf_set_int("plugins/darkroom/masks/gradient/state", gradient->state);
    
    // we recreate the form points
    dt_masks_gui_form_create(form, gui, index, module);

    // we save the new parameters

    return 1;
  }
  return 0;
}

static int _gradient_events_key_pressed(struct dt_iop_module_t *module, GdkEventKey *event, dt_masks_form_t *form,
                                              int parentid, dt_masks_form_gui_t *gui, int index)
{
  return 0;
}

static int _gradient_events_mouse_moved(struct dt_iop_module_t *module, float pzx, float pzy,
                                        double pressure, int which, dt_masks_form_t *form, int parentid,
                                        dt_masks_form_gui_t *gui, int index)
{
  if(!gui) return 0;

  if(gui->creation)
  {
    // Let the cursor motion be redrawn as it moves in GUI
    return 1;
  }

  if(!form || !form->points) return 0;

  // we get the gradient
  dt_masks_anchor_gradient_t *gradient = (dt_masks_anchor_gradient_t *)((form->points)->data);
  if(!gradient) return 0;

  // we need the reference points
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return 0;

  if(gui->form_dragging)
  {
    // we change the center value
    dt_develop_t *dev = (dt_develop_t *)darktable.develop;
    float pts[2] = { -1 , -1 };
    const float pointer[2] = { pzx, pzy };
    dt_dev_roi_delta_to_input_space(dev, gui->delta, pointer, pts);

    gradient->center[0] = pts[0];
    gradient->center[1] = pts[1];

    // we recreate the form points
    dt_masks_gui_form_create(form, gui, index, module);

    return 1;
  }

  //rotation with the mouse
  if(gui->form_rotating)
  {
    const float origin_point[2] = { gpt->points[0], gpt->points[1] };
    const float angle = - dt_masks_rotate_with_anchor(darktable.develop, gui->pos, origin_point, gui);
    _change_rotation(form, gui, module, index, angle , DT_MASKS_INCREMENT_OFFSET, 1);

    // we recreate the form points
    dt_masks_gui_form_create(form, gui, index, module);

    return 1;
  }

  if(_find_closest_handle(module, pzx, pzy, form, parentid, gui, index)) return 1;
  if(gui->edit_mode != DT_MASKS_EDIT_FULL) return 0;
  return 1;
}

// check if (x,y) lies within reasonable limits relative to image frame
static inline gboolean _gradient_is_canonical(const float x, const float y, const float wd, const float ht)
{
  return (isnormal(x) && isnormal(y) && (x >= -wd) && (x <= 2 * wd) && (y >= -ht) && (y <= 2 * ht)) ? TRUE : FALSE;
}

static int _gradient_get_points(dt_develop_t *dev, float x, float y, float rotation, float curvature,
                                float **points, int *points_count)
{
  *points = NULL;
  *points_count = 0;

  const float wd = dev->preview_pipe->iwidth;
  const float ht = dev->preview_pipe->iheight;
  const float scale = sqrtf(wd * wd + ht * ht);
  const float distance = 0.1f * fminf(wd, ht);

  const float v = (-rotation / 180.0f) * M_PI;
  const float cosv = cosf(v);
  const float sinv = sinf(v);

  const int count = sqrtf(wd * wd + ht * ht) + 3;
  *points = dt_pixelpipe_cache_alloc_align_float_cache((size_t)2 * count, 0);
  if(*points == NULL) return 1;

  // we set the anchor point
  (*points)[0] = x * wd;
  (*points)[1] = y * ht;

  // we set the pivot points
  const float v1 = (-(rotation - 90.0f) / 180.0f) * M_PI;
  const float x1 = x * wd + distance * cosf(v1);
  const float y1 = y * ht + distance * sinf(v1);
  (*points)[2] = x1;
  (*points)[3] = y1;
  const float v2 = (-(rotation + 90.0f) / 180.0f) * M_PI;
  const float x2 = x * wd + distance * cosf(v2);
  const float y2 = y * ht + distance * sinf(v2);
  (*points)[4] = x2;
  (*points)[5] = y2;

  const int nthreads = omp_get_max_threads();
  size_t c_padded_size;
  uint32_t *pts_count = dt_pixelpipe_cache_calloc_perthread(nthreads, sizeof(uint32_t), &c_padded_size);
  float *const restrict pts = dt_pixelpipe_cache_alloc_align_float_cache((size_t)2 * count * nthreads, 0);
  if(pts_count == NULL || pts == NULL)
  {
    if(pts_count) dt_pixelpipe_cache_free_align(pts_count);
    if(pts) dt_pixelpipe_cache_free_align(pts);
    dt_pixelpipe_cache_free_align(*points);
    *points = NULL;
    *points_count = 0;
    return 1;
  }

  // we set the line point
  const float xstart = fabsf(curvature) > 1.0f ? -sqrtf(1.0f / fabsf(curvature)) : -1.0f;
  const float xdelta = -2.0f * xstart / (count - 3);

//  gboolean in_frame = FALSE;
#ifdef _OPENMP
#pragma omp parallel for default(none)                                                                       \
    dt_omp_firstprivate(nthreads, pts, pts_count, count, cosv, sinv, xstart, xdelta, curvature, scale, x, y, wd,  \
                        ht, c_padded_size, points) schedule(static) if(count > 100)
#endif
  for(int i = 3; i < count; i++)
  {
    const float xi = xstart + (i - 3) * xdelta;
    const float yi = curvature * xi * xi;
    const float xii = (cosv * xi + sinv * yi) * scale;
    const float yii = (sinv * xi - cosv * yi) * scale;
    const float xiii = xii + x * wd;
    const float yiii = yii + y * ht;

    // don't generate guide points if they extend too far beyond the image frame;
    // this is to avoid that modules like lens correction fail on out of range coordinates
    if(!(xiii < -wd || xiii > 2 * wd || yiii < -ht || yiii > 2 * ht))
    {
      const int thread = omp_get_thread_num();
      uint32_t *tcount = dt_get_perthread(pts_count, c_padded_size);
      pts[(thread * count) + *tcount * 2]     = xiii;
      pts[(thread * count) + *tcount * 2 + 1] = yiii;
      (*tcount)++;
    }
  }

  *points_count = 3;
  for(int thread = 0; thread < nthreads; thread++)
  {
    const uint32_t tcount = *(uint32_t *)dt_get_bythread(pts_count, c_padded_size, thread);
    for(int k = 0; k < tcount; k++)
    {
      (*points)[(*points_count) * 2]     = pts[(thread * count) + k * 2];
      (*points)[(*points_count) * 2 + 1] = pts[(thread * count) + k * 2 + 1];
      (*points_count)++;
    }
  }

  dt_pixelpipe_cache_free_align(pts_count);
  dt_pixelpipe_cache_free_align(pts);

  // and we transform them with all distorted modules
  if(!dt_dev_distort_transform(dev, *points, *points_count))
  {
    dt_pixelpipe_cache_free_align(*points);
    *points = NULL;
    *points_count = 0;
    return 1;
  }

  return 0;
}

// Helper function to copy points, skipping the first 3 metadata points
static void _copy_points(float *dest, const float *src, int count, int *k)
{
  for(int i = 3; i < count; i++, (*k)++)
  {
    dest[(*k) * 2] = src[i * 2];
    dest[(*k) * 2 + 1] = src[i * 2 + 1];
  }
}

static int _gradient_get_pts_border(dt_develop_t *dev, float x, float y, float rotation, float distance,
                                    float curvature, float **points, int *points_count)
{
  *points = NULL;
  *points_count = 0;

  // Get border curve dimensions and scaling
  const float wd = dev->preview_pipe->iwidth;
  const float ht = dev->preview_pipe->iheight;
  const float scale = sqrtf(wd * wd + ht * ht);
  
  // Calculate perpendicular offsets (±90 degrees from rotation)
  const float v1 = (-(rotation - 90.0f) / 180.0f) * M_PI;
  const float v2 = (-(rotation + 90.0f) / 180.0f) * M_PI;

  // Generate offset positions for both curves
  const float x1 = (x * wd + distance * scale * cosf(v1)) / wd;
  const float y1 = (y * ht + distance * scale * sinf(v1)) / ht;
  const float x2 = (x * wd + distance * scale * cosf(v2)) / wd;
  const float y2 = (y * ht + distance * scale * sinf(v2)) / ht;

  // Get points for both curves
  float *points1 = NULL, *points2 = NULL;
  int points_count1 = 0, points_count2 = 0;
  const int err1 = _gradient_get_points(dev, x1, y1, rotation, curvature, &points1, &points_count1);
  const int err2 = _gradient_get_points(dev, x2, y2, rotation, curvature, &points2, &points_count2);

  // Check which curves are valid (need more than 4 points: 3 metadata + at least 1 data)
  const gboolean valid1 = (err1 == 0) && points_count1 > 4;
  const gboolean valid2 = (err2 == 0) && points_count2 > 4;

  int err = 1;
  
  if(valid1 && valid2)
  {
    // Both curves valid - combine them with INFINITY separator
    const int total_points = (points_count1 - 3) + (points_count2 - 3) + 1;
    *points = dt_pixelpipe_cache_alloc_align_float_cache((size_t)2 * total_points, 0);
    if(*points == NULL) goto cleanup;
    
    *points_count = total_points;
    int k = 0;
    
    _copy_points(*points, points1, points_count1, &k);
    (*points)[k * 2] = (*points)[k * 2 + 1] = INFINITY; // Separator
    k++;
    _copy_points(*points, points2, points_count2, &k);
    err = 0;
  }
  else if(valid1)
  {
    // Only first curve valid
    *points_count = points_count1 - 3;
    *points = dt_pixelpipe_cache_alloc_align_float_cache((size_t)2 * (*points_count), 0);
    if(*points == NULL) goto cleanup;
    
    int k = 0;
    _copy_points(*points, points1, points_count1, &k);
    err = 0;
  }
  else if(valid2)
  {
    // Only second curve valid
    *points_count = points_count2 - 3;
    *points = dt_pixelpipe_cache_alloc_align_float_cache((size_t)2 * (*points_count), 0);
    if(*points == NULL) goto cleanup;
    
    int k = 0;
    _copy_points(*points, points2, points_count2, &k);
    err = 0;
  }

cleanup:
  dt_pixelpipe_cache_free_align(points1);
  dt_pixelpipe_cache_free_align(points2);
  return err;
}

static void _gradient_draw_shape(cairo_t *cr, const float *pts_line, const int pts_line_count, const int nb, const gboolean border, const gboolean source)
{
  // safeguard in case of malformed arrays of points
  if(border && pts_line_count <= 3) return;
  if(!border && pts_line_count <= 4) return;

  const float *points = (border) ? pts_line : pts_line + 6;
  const int points_count = (border) ? pts_line_count : pts_line_count - 3;
  
  const float wd = darktable.develop->preview_pipe->iwidth;
  const float ht = darktable.develop->preview_pipe->iheight;

  int i = 0;
  while(i < points_count)
  {
    const float px = points[i * 2];
    const float py = points[i * 2 + 1];

    if(!isnormal(px) || !_gradient_is_canonical(px, py, wd, ht))
    {
      i++;
      continue;
    }

    cairo_move_to(cr, px, py);
    i++;

    // continue the current segment until a non-normal or out-of-range point
    while(i < points_count)
    {
      const float qx = points[i * 2];
      const float qy = points[i * 2 + 1];
      if(!isnormal(qx) || !_gradient_is_canonical(qx, qy, wd, ht)) break;
      cairo_line_to(cr, qx, qy);
      i++;
    }
  }
}

static void _gradient_draw_arrow(cairo_t *cr, const gboolean selected, const gboolean pivot_selected, const gboolean is_rotating,
                                  const float zoom_scale, float *pts, int pts_count)
{
  if(pts_count < 3) return;

  const float anchor_x = pts[0];
  const float anchor_y = pts[1];
  const float pivot_end_x = pts[2];
  const float pivot_end_y = pts[3];
  const float pivot_start_x = pts[4];
  const float pivot_start_y = pts[5];

  // draw a dotted line across the gradient for better visibility while dragging
  if(is_rotating)
  {
    // extend the axis line beyond the pivot points
    const float scale = 1 / zoom_scale;
    const float dx = pivot_end_x - pivot_start_x;
    const float dy = pivot_end_y - pivot_start_y;

    const float new_x1 = pivot_start_x - (dx * scale * 0.5f);
    const float new_y1 = pivot_start_y - (dy * scale * 0.5f);
    const float new_x2 = pivot_end_x + (dx * scale * 0.5f);
    const float new_y2 = pivot_end_y + (dy * scale * 0.5f);
    cairo_move_to(cr, new_x1, new_y1);
    cairo_line_to(cr, new_x2, new_y2);

    dt_draw_stroke_line(DT_MASKS_DASH_ROUND, FALSE, cr, FALSE, zoom_scale, CAIRO_LINE_CAP_ROUND);
  }

  // always draw arrow to clearly display the direction
  {
    // size & width of the arrow
    const float arrow_angle = 0.25f;
    const float arrow_length = (DT_DRAW_SCALE_ARROW * 2) / zoom_scale;

    // compute direction from anchor toward pivot_end and build an arrow
    const float dx = pivot_end_x - anchor_x;
    const float dy = pivot_end_y - anchor_y;
    const float angle_dir = atan2f(dy, dx); // direction the arrow should point to

    // tip of the arrow (ahead of anchor along angle_dir)
    const float tip_x = anchor_x + arrow_length * cosf(angle_dir);
    const float tip_y = anchor_y + arrow_length * sinf(angle_dir);

    // half width of the arrow head
    const float half_w = arrow_length * tanf(arrow_angle);

    // perpendicular vector to the direction (unit)
    const float nx = -sinf(angle_dir);
    const float ny =  cosf(angle_dir);

    // two corner points of the arrow base, centered on (anchor_x, anchor_y)
    const float arrow_x1 = anchor_x + nx * half_w;
    const float arrow_y1 = anchor_y + ny * half_w;
    const float arrow_x2 = anchor_x - nx * half_w;
    const float arrow_y2 = anchor_y - ny * half_w;

    // we will draw the triangle as tip -> base1 -> base2
    cairo_move_to(cr, tip_x, tip_y);
    cairo_line_to(cr, arrow_x1, arrow_y1);
    cairo_line_to(cr, arrow_x2, arrow_y2);
    cairo_close_path(cr);

    dt_draw_set_color_overlay(cr, TRUE, 0.8);
    cairo_fill_preserve(cr);
    double line_width = pivot_selected ? (DT_DRAW_SIZE_LINE_SELECTED / zoom_scale) : (DT_DRAW_SIZE_LINE / zoom_scale);
    cairo_set_line_width(cr, line_width);
    dt_draw_set_color_overlay(cr, FALSE, 0.9);
    cairo_stroke(cr);
  }

  // draw the origin anchor point on top of everything
  dt_draw_node(cr, FALSE, FALSE, pivot_selected, zoom_scale, anchor_x, anchor_y);
}

static void _gradient_events_post_expose(cairo_t *cr, float zoom_scale, dt_masks_form_gui_t *gui, int index, int nb)
{
  if(!gui) return;

  // preview gradient creation
  if(gui->creation)
  {
    dt_masks_form_t *form = darktable.develop->form_visible;
    if(!form) return;

    float rotation = 0.0f;
    float extent = 0.0f;
    float curvature = 0.0f;

    extent = dt_conf_get_float("plugins/darkroom/masks/gradient/extent");
    curvature = dt_conf_get_float("plugins/darkroom/masks/gradient/curvature");
    rotation = dt_conf_get_float("plugins/darkroom/masks/gradient/rotation");

    // we get the gradient center
    float xpos = gui->pos[0];
    float ypos = gui->pos[1];

    if((xpos == -1.f && ypos == -1.f) || gui->mouse_leaved_center)
    {
      const dt_develop_t *dev = (const dt_develop_t *)darktable.develop;
      xpos = (.5f + dev->roi.x) * darktable.develop->preview_width;
      ypos = (.5f + dev->roi.y) * darktable.develop->preview_height;
    }
    float pts[2] = { xpos, ypos };
    dt_dev_distort_backtransform(darktable.develop, pts, 1);
    float x = pts[0] / darktable.develop->preview_pipe->iwidth;
    float y = pts[1] / darktable.develop->preview_pipe->iheight;

    // we get all the points, distorted if needed of the sample form
    float *points = NULL;
    int points_count = 0;
    float *border = NULL;
    int border_count = 0;
    int err = _gradient_get_points(darktable.develop, x, y, rotation, curvature, &points, &points_count);
    if(err)
    {
      if(points) dt_pixelpipe_cache_free_align(points);
      if(border) dt_pixelpipe_cache_free_align(border);
      return;
    }
    if(extent > 0.0f)
    {
      err = _gradient_get_pts_border(darktable.develop, x, y, rotation, extent, curvature, &border, &border_count);
      if(err)
      {
        if(points) dt_pixelpipe_cache_free_align(points);
        if(border) dt_pixelpipe_cache_free_align(border);
        return;
      }
    }

    // draw main line
    dt_draw_shape_lines(DT_MASKS_NO_DASH, FALSE, cr, nb, FALSE, zoom_scale, points, points_count, &dt_masks_functions_gradient.draw_shape, CAIRO_LINE_CAP_ROUND);
    _gradient_draw_arrow(cr, FALSE, FALSE, gui->form_rotating, zoom_scale, points, points_count);

    // draw borders
    dt_draw_shape_lines(DT_MASKS_DASH_STICK, FALSE, cr, nb, FALSE, zoom_scale, border, border_count, &dt_masks_functions_gradient.draw_shape, CAIRO_LINE_CAP_ROUND);
    if(points) dt_pixelpipe_cache_free_align(points);
    if(border) dt_pixelpipe_cache_free_align(border);
  
    return;
  }

  const dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return;

  const gboolean seg_selected = (gui->group_selected == index) && (gui->seg_selected >= 0);
  const gboolean all_selected = (gui->group_selected == index) && (gui->form_selected || gui->form_dragging); 
  // draw main line
  dt_draw_shape_lines(DT_MASKS_NO_DASH, FALSE, cr, nb, (seg_selected), zoom_scale, gpt->points, gpt->points_count, &dt_masks_functions_gradient.draw_shape, CAIRO_LINE_CAP_ROUND);
  // draw borders
  if(gui->group_selected == index)
  {
    dt_draw_shape_lines(DT_MASKS_DASH_STICK, FALSE, cr, nb, (gui->border_selected), zoom_scale, gpt->border, gpt->border_count, &dt_masks_functions_gradient.draw_shape, CAIRO_LINE_CAP_ROUND);
  }

  _gradient_draw_arrow(cr, (seg_selected || all_selected), ((gui->group_selected == index) && gui->pivot_selected), gui->form_rotating,
                        zoom_scale, gpt->points, gpt->points_count);
}

static int _gradient_get_points_border(dt_develop_t *dev, dt_masks_form_t *form, float **points, int *points_count,
                                       float **border, int *border_count, int source,
                                       const dt_iop_module_t *module)
{
  (void)source;  // unused arg, keep compiler from complaining
  if(!form || !form->points) return 0;
  dt_masks_anchor_gradient_t *gradient = (dt_masks_anchor_gradient_t *)form->points->data;
  if(!gradient) return 0;
  if(_gradient_get_points(dev, gradient->center[0], gradient->center[1], gradient->rotation, gradient->curvature,
                          points, points_count) != 0)
    return 1;
  if(border)
    return _gradient_get_pts_border(dev, gradient->center[0], gradient->center[1],
                                    gradient->rotation, gradient->extent, gradient->curvature,
                                    border, border_count);
  return 0;
}

static int _gradient_get_area(const dt_iop_module_t *const module, const dt_dev_pixelpipe_iop_t *const piece,
                              dt_masks_form_t *const form,
                              int *width, int *height, int *posx, int *posy)
{
  const float wd = piece->pipe->iwidth, ht = piece->pipe->iheight;

  float points[8] = { 0.0f, 0.0f, wd, 0.0f, wd, ht, 0.0f, ht };

  // and we transform them with all distorted modules
  if(!dt_dev_distort_transform_plus(module->dev, piece->pipe, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, points, 4))
    return 1;

  // now we search min and max
  float xmin = 0.0f, xmax = 0.0f, ymin = 0.0f, ymax = 0.0f;
  xmin = ymin = FLT_MAX;
  xmax = ymax = FLT_MIN;
  for(int i = 0; i < 4; i++)
  {
    xmin = fminf(points[i * 2], xmin);
    xmax = fmaxf(points[i * 2], xmax);
    ymin = fminf(points[i * 2 + 1], ymin);
    ymax = fmaxf(points[i * 2 + 1], ymax);
  }

  // and we set values
  *posx = xmin;
  *posy = ymin;
  *width = (xmax - xmin);
  *height = (ymax - ymin);
  return 0;
}

// caller needs to make sure that input remains within bounds
static inline float dt_gradient_lookup(const float *lut, const float i)
{
  const int bin0 = i;
  const int bin1 = i + 1;
  const float f = i - bin0;
  return lut[bin1] * f + lut[bin0] * (1.0f - f);
}

static int _gradient_get_mask(const dt_iop_module_t *const module, const dt_dev_pixelpipe_iop_t *const piece,
                              dt_masks_form_t *const form,
                              float **buffer, int *width, int *height, int *posx, int *posy)
{
  if(!form || !form->points) return 0;
  double start2 = 0.0;
  if(darktable.unmuted & DT_DEBUG_PERF) start2 = dt_get_wtime();
  // we get the area
  if(_gradient_get_area(module, piece, form, width, height, posx, posy) != 0) return 1;

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] gradient area took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we get the gradient values
  dt_masks_anchor_gradient_t *gradient = (dt_masks_anchor_gradient_t *)((form->points)->data);
  if(!gradient) return 0;
  // we create a buffer of grid points for later interpolation. mainly in order to reduce memory footprint
  const int w = *width;
  const int h = *height;
  const int px = *posx;
  const int py = *posy;
  const int grid = 8;
  const int gw = (w + grid - 1) / grid + 1;
  const int gh = (h + grid - 1) / grid + 1;

  float *points = dt_pixelpipe_cache_alloc_align_float_cache((size_t)2 * gw * gh, 0);
  if(points == NULL) return 1;

#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(grid, gh, gw, px, py) \
  shared(points) schedule(static) collapse(2)
#else
#pragma omp parallel for shared(points)
#endif
#endif
  for(int j = 0; j < gh; j++)
    for(int i = 0; i < gw; i++)
    {
      points[(j * gw + i) * 2] = (grid * i + px);
      points[(j * gw + i) * 2 + 1] = (grid * j + py);
    }

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] gradient draw took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we backtransform all these points
  if(!dt_dev_distort_backtransform_plus(module->dev, piece->pipe, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, points, (size_t)gw * gh))
  {
    dt_pixelpipe_cache_free_align(points);
    return 1;
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] gradient transform took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we calculate the mask at grid points and recycle point buffer to store results
  const float wd = piece->pipe->iwidth;
  const float ht = piece->pipe->iheight;
  const float hwscale = 1.0f / sqrtf(wd * wd + ht * ht);
  const float ihwscale = 1.0f / hwscale;
  const float v = (-gradient->rotation / 180.0f) * M_PI;
  const float sinv = sinf(v);
  const float cosv = cosf(v);
  const float xoffset = cosv * gradient->center[0] * wd + sinv * gradient->center[1] * ht;
  const float yoffset = sinv * gradient->center[0] * wd - cosv * gradient->center[1] * ht;
  const float extent = fmaxf(gradient->extent, 0.001f);
  const float normf = 1.0f / extent;
  const float curvature = gradient->curvature;
  const dt_masks_gradient_states_t state = gradient->state;

  const int lutmax = ceilf(4 * extent * ihwscale);
  const int lutsize = 2 * lutmax + 2;
  float *lut = dt_pixelpipe_cache_alloc_align_float_cache((size_t)lutsize, 0);
  if(lut == NULL)
  {
    dt_pixelpipe_cache_free_align(points);
    return 1;
  }

#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(lutsize, lutmax, hwscale, state, normf, extent) \
  shared(lut) schedule(static)
#else
#pragma omp parallel for shared(points)
#endif
#endif
  for(int n = 0; n < lutsize; n++)
  {
    const float distance = (n - lutmax) * hwscale;
    const float value = 0.5f + 0.5f * ((state == DT_MASKS_GRADIENT_STATE_LINEAR) ? normf * distance: erff(distance / extent));
    lut[n] = (value < 0.0f) ? 0.0f : ((value > 1.0f) ? 1.0f : value);
  }

  // center lut around zero
  float *clut = lut + lutmax;


#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(gh, gw, sinv, cosv, xoffset, yoffset, hwscale, ihwscale, curvature, extent) \
  shared(points, clut) schedule(static) collapse(2)
#else
#pragma omp parallel for shared(points)
#endif
#endif
  for(int j = 0; j < gh; j++)
  {
    for(int i = 0; i < gw; i++)
    {
      const float x = points[(j * gw + i) * 2];
      const float y = points[(j * gw + i) * 2 + 1];

      const float x0 = (cosv * x + sinv * y - xoffset) * hwscale;
      const float y0 = (sinv * x - cosv * y - yoffset) * hwscale;

      const float distance = y0 - curvature * x0 * x0;

      points[(j * gw + i) * 2] = (distance <= -4.0f * extent) ? 0.0f :
                                    ((distance >= 4.0f * extent) ? 1.0f : dt_gradient_lookup(clut, distance * ihwscale));
    }
  }

  dt_pixelpipe_cache_free_align(lut);

  // we allocate the buffer
  float *const bufptr = *buffer = dt_pixelpipe_cache_alloc_align_float_cache((size_t)w * h, 0);
  if(*buffer == NULL)
  {
    dt_pixelpipe_cache_free_align(points);
    return 1;
  }

// we fill the mask buffer by interpolation
#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(h, w, gw, grid, bufptr) \
  shared(points) schedule(simd:static)
#else
#pragma omp parallel for shared(points, buffer)
#endif
#endif
  for(int j = 0; j < h; j++)
  {
    const int jj = j % grid;
    const int mj = j / grid;
    const int grid_jj = grid - jj;
    for(int i = 0; i < w; i++)
    {
      const int ii = i % grid;
      const int mi = i / grid;
      const int grid_ii = grid - ii;
      const size_t pt_index = mj * gw + mi;
      bufptr[j * w + i] = (points[2 * pt_index] * grid_ii * grid_jj
                           + points[2 * (pt_index + 1)] * ii * grid_jj
                           + points[2 * (pt_index + gw)] * grid_ii * jj
                           + points[2 * (pt_index + gw + 1)] * ii * jj) / (grid * grid);
    }
  }

  dt_pixelpipe_cache_free_align(points);

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] gradient fill took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);

  return 0;
}


static int _gradient_get_mask_roi(const dt_iop_module_t *const module, const dt_dev_pixelpipe_iop_t *const piece,
                                  dt_masks_form_t *const form, const dt_iop_roi_t *roi, float *buffer)
{
  if(!form || !form->points) return 0;
  double start2 = 0.0;
  if(darktable.unmuted & DT_DEBUG_PERF) start2 = dt_get_wtime();
  // we get the gradient values
  const dt_masks_anchor_gradient_t *gradient = (dt_masks_anchor_gradient_t *)(form->points->data);
  if(!gradient) return 0;

  // we create a buffer of grid points for later interpolation. mainly in order to reduce memory footprint
  const int w = roi->width;
  const int h = roi->height;
  const int px = roi->x;
  const int py = roi->y;
  const float iscale = 1.0f / roi->scale;
  const int grid = CLAMP((10.0f*roi->scale + 2.0f) / 3.0f, 1, 4);
  const int gw = (w + grid - 1) / grid + 1;
  const int gh = (h + grid - 1) / grid + 1;

  float *points = dt_pixelpipe_cache_alloc_align_float_cache((size_t)2 * gw * gh, 0);
  if(points == NULL) return 1;

#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(iscale, gh, gw, py, px, grid) \
  shared(points) schedule(static) collapse(2)
#else
#pragma omp parallel for shared(points)
#endif
#endif
  for(int j = 0; j < gh; j++)
    for(int i = 0; i < gw; i++)
    {

      const size_t index = (size_t)j * gw + i;
      points[index * 2] = (grid * i + px) * iscale;
      points[index * 2 + 1] = (grid * j + py) * iscale;
    }

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] gradient draw took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we backtransform all these points
  if(!dt_dev_distort_backtransform_plus(module->dev, piece->pipe, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, points,
                                        (size_t)gw * gh))
  {
    dt_pixelpipe_cache_free_align(points);
    return 1;
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] gradient transform took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we calculate the mask at grid points and recycle point buffer to store results
  const float wd = piece->pipe->iwidth;
  const float ht = piece->pipe->iheight;
  const float hwscale = 1.0f / sqrtf(wd * wd + ht * ht);
  const float ihwscale = 1.0f / hwscale;
  const float v = (-gradient->rotation / 180.0f) * M_PI;
  const float sinv = sinf(v);
  const float cosv = cosf(v);
  const float xoffset = cosv * gradient->center[0] * wd + sinv * gradient->center[1] * ht;
  const float yoffset = sinv * gradient->center[0] * wd - cosv * gradient->center[1] * ht;
  const float extent = fmaxf(gradient->extent, 0.001f);
  const float normf = 1.0f / extent;
  const float curvature = gradient->curvature;
  const dt_masks_gradient_states_t state = gradient->state;

  const int lutmax = ceilf(4 * extent * ihwscale);
  const int lutsize = 2 * lutmax + 2;
  float *lut = dt_pixelpipe_cache_alloc_align_float_cache((size_t)lutsize, 0);
  if(lut == NULL)
  {
    dt_pixelpipe_cache_free_align(points);
    return 1;
  }

#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(lutsize, lutmax, hwscale, state, normf, extent) \
  shared(lut) schedule(static)
#else
#pragma omp parallel for shared(points)
#endif
#endif
  for(int n = 0; n < lutsize; n++)
  {
    const float distance = (n - lutmax) * hwscale;
    const float value = 0.5f + 0.5f * ((state == DT_MASKS_GRADIENT_STATE_LINEAR) ? normf * distance: erff(distance / extent));
    lut[n] = (value < 0.0f) ? 0.0f : ((value > 1.0f) ? 1.0f : value);
  }

  // center lut around zero
  float *clut = lut + lutmax;

#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(gh, gw, sinv, cosv, xoffset, yoffset, hwscale, ihwscale, curvature, extent) \
  shared(points, clut) schedule(static) collapse(2)
#else
#pragma omp parallel for shared(points)
#endif
#endif
  for(int j = 0; j < gh; j++)
  {
    for(int i = 0; i < gw; i++)
    {
      const size_t index = (size_t)j * gw + i;
      const float x = points[index * 2];
      const float y = points[index * 2 + 1];

      const float x0 = (cosv * x + sinv * y - xoffset) * hwscale;
      const float y0 = (sinv * x - cosv * y - yoffset) * hwscale;

      const float distance = y0 - curvature * x0 * x0;

      points[index * 2] = (distance <= -4.0f * extent) ? 0.0f : ((distance >= 4.0f * extent) ? 1.0f : dt_gradient_lookup(clut, distance * ihwscale));
    }
  }

  dt_pixelpipe_cache_free_align(lut);

// we fill the mask buffer by interpolation
#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(h, w, grid, gw) \
  shared(buffer, points) schedule(simd:static)
#else
#pragma omp parallel for shared(points, buffer)
#endif
#endif
  for(int j = 0; j < h; j++)
  {
    const int jj = j % grid;
    const int mj = j / grid;
    const int grid_jj = grid - jj;
    for(int i = 0; i < w; i++)
    {
      const int ii = i % grid;
      const int mi = i / grid;
      const int grid_ii = grid - ii;
      const size_t mindex = (size_t)mj * gw + mi;
      buffer[(size_t)j * w + i]
          = (points[mindex * 2] * grid_ii * grid_jj
             + points[(mindex + 1) * 2] * ii * grid_jj
             + points[(mindex + gw) * 2] * grid_ii * jj
             + points[(mindex + gw + 1) * 2] * ii * jj)
            / (grid * grid);
    }
  }

  dt_pixelpipe_cache_free_align(points);

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] gradient fill took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);

  return 0;
}

static void _gradient_sanitize_config(dt_masks_type_t type)
{
  // we always want to start with no curvature
  dt_conf_set_float("plugins/darkroom/masks/gradient/curvature", 0.0f);
}

static void _gradient_set_form_name(struct dt_masks_form_t *const form, const size_t nb)
{
  snprintf(form->name, sizeof(form->name), _("gradient #%d"), (int)nb);
}

static void _gradient_set_hint_message(const dt_masks_form_gui_t *const gui, const dt_masks_form_t *const form,
                                     const int opacity, char *const restrict msgbuf, const size_t msgbuf_len)
{
  if(gui->creation)
    g_snprintf(msgbuf, msgbuf_len, _("<b>Extent</b>: scroll, <b>Curvature</b>: shift+scroll\n"
                                     "<b>Rotate</b>: shift+drag, <b>Opacity</b>: ctrl+scroll (%d%%)"), opacity);
  else if(gui->form_selected || gui->seg_selected >= 0)
    g_snprintf(msgbuf, msgbuf_len, _("<b>Extent</b>: scroll, <b>Curvature</b>: shift+scroll\n"
                                     "<b>Reset curvature</b>: double-click, <b>Opacity</b>: ctrl+scroll (%d%%)"), opacity);
}

static void _gradient_duplicate_points(dt_develop_t *dev, dt_masks_form_t *const base, dt_masks_form_t *const dest)
{
  if(!base || !base->points) return;
  (void)dev; // unused arg, keep compiler from complaining
  for(GList *pts = base->points; pts; pts = g_list_next(pts))
  {
    dt_masks_anchor_gradient_t *pt = (dt_masks_anchor_gradient_t *)pts->data;
    dt_masks_anchor_gradient_t *npt = (dt_masks_anchor_gradient_t *)malloc(sizeof(dt_masks_anchor_gradient_t));
    memcpy(npt, pt, sizeof(dt_masks_anchor_gradient_t));
    dest->points = g_list_append(dest->points, npt);
  }
}

// The function table for gradients.  This must be public, i.e. no "static" keyword.
const dt_masks_functions_t dt_masks_functions_gradient = {
  .point_struct_size = sizeof(struct dt_masks_anchor_gradient_t),
  .sanitize_config = _gradient_sanitize_config,
  .set_form_name = _gradient_set_form_name,
  .set_hint_message = _gradient_set_hint_message,
  .duplicate_points = _gradient_duplicate_points,
  .get_distance = _gradient_get_distance,
  .get_points_border = _gradient_get_points_border,
  .get_mask = _gradient_get_mask,
  .get_mask_roi = _gradient_get_mask_roi,
  .get_area = _gradient_get_area,
  .mouse_moved = _gradient_events_mouse_moved,
  .mouse_scrolled = _gradient_events_mouse_scrolled,
  .button_pressed = _gradient_events_button_pressed,
  .button_released = _gradient_events_button_released,
  .key_pressed = _gradient_events_key_pressed,
  .post_expose = _gradient_events_post_expose,
  .draw_shape = _gradient_draw_shape
};

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
