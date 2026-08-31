/*
    This file is part of darktable,
    Copyright (C) 2013-2014, 2016, 2021 Aldric Renaudin.
    Copyright (C) 2013, 2018, 2020-2022 Pascal Obry.
    Copyright (C) 2013 Simon Spannagel.
    Copyright (C) 2013-2017 Tobias Ellinghaus.
    Copyright (C) 2013-2016, 2019 Ulrich Pegelow.
    Copyright (C) 2014-2016 Roman Lebedev.
    Copyright (C) 2017-2019 Edgardo Hoszowski.
    Copyright (C) 2018 johannes hanika.
    Copyright (C) 2019 Andreas Schneider.
    Copyright (C) 2019, 2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2019 Matthieu Moy.
    Copyright (C) 2020, 2022 Chris Elston.
    Copyright (C) 2020 GrahamByrnes.
    Copyright (C) 2020 Heiko Bauke.
    Copyright (C) 2020-2021 Hubert Kowalski.
    Copyright (C) 2020-2021 Ralf Brown.
    Copyright (C) 2021 Diederik Ter Rahe.
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
#include "system/macros.h"
#include "system/openmp.h"
#include "common/logging.h"
#include "common/times.h"
#include "caches/pixelpipe_cache_alloc.h"
#include "common/conf.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "develop/masks_gui.h"
#include "develop/masks/masks_functions.h"
#include "develop/masks/masks_touched.h"
#include "math/openmp_maths.h"
#include "widgets/accelerators.h"

#define FADING_MIN 0.0005f
#define FADING_MAX 1.0f

#define BORDER_MIN 0.00005f
#define BORDER_MAX 0.5f

static void _circle_get_distance(float x, float y, float as, dt_masks_form_gui_t *gui, int index,
                                 int num_points, int *inside, int *inside_border, int *near_handle,
                                 int *inside_source, float *dist)
{
  // initialise returned values
  *inside_source = 0;
  *inside = 0;
  *inside_border = 0;
  *near_handle = -1;
  *dist = FLT_MAX;

  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(IS_NULL_PTR(gpt)) return;

  // we first check if we are inside the source form
  const float pt[2] = { x, y };

  if(gpt->source && gpt->source_count > 0
     && dt_masks_point_in_form_exact(pt, 1, gpt->source, 1, gpt->source_count, NULL, 0) >= 0)
  {
    *inside_source = 1;
    *inside = 1;

    // distance from source center
    const float center_dx = x - gpt->source[0];
    const float center_dy = y - gpt->source[1];
    *dist = sqf(center_dx) + sqf(center_dy);

    return;
  }

  if(IS_NULL_PTR(gpt->points) || gpt->points_count <= 0 || !gpt->border || gpt->border_count <= 0) return;

  // distance from center
  const float center_dx = x - gpt->points[0];
  const float center_dy = y - gpt->points[1];
  *dist = sqf(center_dx) + sqf(center_dy);

  // we check if it's inside borders
  if(dt_masks_point_in_form_exact(pt, 1, gpt->border, 1, gpt->border_count, NULL, 0) < 0) return;
  *inside = 1;

  // and we check if it's inside form
  if(dt_masks_point_in_form_exact(pt, 1, gpt->points, 1, gpt->points_count, NULL, 0) < 0)
    *inside_border = 1;
}

/**
 * @brief Circle-specific inside/border hit testing adapter.
 */
static void _circle_distance_cb(float pointer_x, float pointer_y, float cursor_radius,
                                dt_masks_form_gui_t *mask_gui, int form_index, int node_count, int *inside,
                                int *inside_border, int *near_handle, int *inside_source, float *dist, void *user_data)
{
  _circle_get_distance(pointer_x, pointer_y, cursor_radius, mask_gui, form_index, 0, inside,
                       inside_border, near_handle, inside_source, dist);
}

static int _find_closest_handle(dt_masks_form_t *mask_form, dt_masks_form_gui_t *mask_gui, int form_index)
{
  return dt_masks_find_closest_handle_common(mask_form, mask_gui, form_index, 0,
                                             NULL, NULL, NULL, _circle_distance_cb, NULL, NULL);
}

static void _circle_get_creation_values(const dt_masks_form_t *form, float *radius, float *border)
{
  const gboolean use_spot_defaults = dt_masks_form_uses_spot_defaults(form);
  *radius = dt_conf_get_float(use_spot_defaults ? "plugins/darkroom/spots/circle/size"
                                                : "plugins/darkroom/masks/circle/size");
  *border = dt_conf_get_float(use_spot_defaults ? "plugins/darkroom/spots/circle/border"
                                                : "plugins/darkroom/masks/circle/border");
}

static void _circle_init_new(dt_masks_form_t *form, dt_masks_form_gui_t *gui, dt_masks_node_circle_t *circle)
{
  dt_masks_gui_cursor_to_raw_norm(gui->dev, gui, circle->center);
  _circle_get_creation_values(form, &circle->radius, &circle->border);

  if(dt_masks_form_is_clone(form))
    dt_masks_set_source_pos_initial_value(gui, form);
  else
    dt_masks_reset_source(form);
}

static int _circle_get_points(dt_develop_t *dev, float x, float y, float radius, float radius2, float rotation,
                              float **points, int *points_count);

// Build the temporary preview geometry for circle creation so the expose path only
// handles drawing and buffer lifetime.
static int _circle_get_creation_preview(dt_masks_form_t *form, dt_masks_form_gui_t *gui,
                                        dt_masks_preview_buffers_t *preview)
{
  float radius_shape = 0.0f;
  float radius_border = 0.0f;
  _circle_get_creation_values(form, &radius_shape, &radius_border);
  radius_border += radius_shape;

  float center[2];
  dt_masks_gui_cursor_to_raw_norm(gui->dev, gui, center);

  *preview = (dt_masks_preview_buffers_t){ 0 };
  int err = _circle_get_points(gui->dev, center[0], center[1], radius_shape, 0.0f, 0.0f,
                               &preview->points, &preview->points_count);
  if(!err && radius_shape != radius_border)
    err = _circle_get_points(gui->dev, center[0], center[1], radius_border, 0.0f, 0.0f,
                             &preview->border, &preview->border_count);

  if(!err && dt_masks_form_is_clone(form))
    err = dt_masks_preview_add_clone_source(gui, preview);

  if(err)
    dt_masks_preview_buffers_cleanup(preview);

  return err;
}

static int _init_fading(dt_masks_form_t *form, const float amount, const dt_masks_increment_t increment, const int flow)
{
  dt_masks_get_set_conf_value_with_toast(form, "border", amount, FADING_MIN, FADING_MAX,
                                         increment, flow, _("Fading: %3.2f%%"), 100.0f);
  return 1;
}

static int _init_size(dt_masks_form_t *form, const float amount, const dt_masks_increment_t increment, const int flow)
{

  dt_masks_get_set_conf_value_with_toast(form, "size", amount, FADING_MIN, FADING_MAX,
                                         increment, flow, _("Size: %3.2f%%"), 2.f * 100.f);
  return 1;
}

static int _init_opacity(dt_masks_form_t *form, const float amount, const dt_masks_increment_t increment, const int flow)
{
  dt_masks_get_set_conf_value_with_toast(form, "opacity", amount, 0.f, 1.f,
                                         increment, flow, _("Opacity: %3.2f%%"), 100.f);
  return 1;
}

static float _circle_get_interaction_value(const dt_masks_form_t *form, dt_masks_interaction_t interaction)
{
  if(IS_NULL_PTR(form) || IS_NULL_PTR(form->points)) return NAN;
  const dt_masks_node_circle_t *circle = (const dt_masks_node_circle_t *)(form->points)->data;
  if(IS_NULL_PTR(circle)) return NAN;

  switch(interaction)
  {
    case DT_MASKS_INTERACTION_SIZE:
      return circle->radius;
    case DT_MASKS_INTERACTION_FADING:
      return circle->border;
    default:
      return NAN;
  }
}

static gboolean _circle_get_gravity_center(dt_develop_t *dev, const dt_masks_form_t *form, float center[2], float *area)
{
  if(IS_NULL_PTR(form) || IS_NULL_PTR(form->points) || IS_NULL_PTR(center) || IS_NULL_PTR(area)) return FALSE;
  const dt_masks_node_circle_t *circle = (const dt_masks_node_circle_t *)(form->points)->data;
  if(IS_NULL_PTR(circle)) return FALSE;
  center[0] = circle->center[0];
  center[1] = circle->center[1];
  *area = M_PI_F * sqf(circle->radius);
  return TRUE;
}

static int _change_fading(dt_masks_form_t *form, dt_masks_form_gui_t *gui, struct dt_iop_module_t *module,
                          int index, const float amount, const dt_masks_increment_t increment, const int flow);
static int _change_size(dt_masks_form_t *form, dt_masks_form_gui_t *gui, struct dt_iop_module_t *module,
                        int index, const float amount, const dt_masks_increment_t increment, const int flow);

static float _circle_set_interaction_value(dt_masks_form_t *form, dt_masks_interaction_t interaction, float value,
                                           dt_masks_increment_t increment, int flow,
                                           dt_masks_form_gui_t *gui, struct dt_iop_module_t *module)
{
  if(IS_NULL_PTR(form)) return NAN;
  // Mirrors _dt_masks_events_get_dispatch_form()'s form_index: this shape's position in the
  // currently displayed group, so dt_masks_gui_form_create() below refreshes the right
  // mask_gui->points slot instead of clobbering whatever shape sits at index 0.
  const int index = (!IS_NULL_PTR(gui) && gui->group_selected >= 0) ? gui->group_selected : 0;

  switch(interaction)
  {
    case DT_MASKS_INTERACTION_SIZE:
      if(!_change_size(form, gui, module, index, value, increment, flow)) return NAN;
      return _circle_get_interaction_value(form, interaction);
    case DT_MASKS_INTERACTION_FADING:
      if(!_change_fading(form, gui, module, index, value, increment, flow)) return NAN;
      return _circle_get_interaction_value(form, interaction);
    default:
      return NAN;
  }
}

static int _change_fading(dt_masks_form_t *form, dt_masks_form_gui_t *gui, struct dt_iop_module_t *module, int index, const float amount, const dt_masks_increment_t increment, const int flow)
{
  if(IS_NULL_PTR(form) || IS_NULL_PTR(form->points)) return 0;
  dt_masks_node_circle_t *circle = (dt_masks_node_circle_t *)(form->points)->data;
  if(IS_NULL_PTR(circle)) return 0;

  circle->border = CLAMPF(dt_masks_apply_increment(circle->border, amount, increment, flow),
                          FADING_MIN, FADING_MAX);

  _init_fading(form, amount, increment, flow);

  // we recreate the form points
  dt_masks_gui_form_create(form, gui, index, module);

  return 1;
}

static int _change_size(dt_masks_form_t *form, dt_masks_form_gui_t *gui, struct dt_iop_module_t *module, int index, const float amount, const dt_masks_increment_t increment, const int flow)
{
  if(IS_NULL_PTR(form) || IS_NULL_PTR(form->points)) return 0;
  dt_masks_node_circle_t *circle = (dt_masks_node_circle_t *)(form->points)->data;
  if(IS_NULL_PTR(circle)) return 0;

  // Sanitize
  // do not exceed upper limit of 1.0 and lower limit of 0.004
  if(amount > 1.0f && (circle->border > 1.0f ))
    return 1;

  const int node_hovered = gui->node_hovered;

  // Growing/shrinking
  if(node_hovered == -1 || node_hovered == 0)
  {
    circle->radius = dt_masks_apply_increment(circle->radius, amount, increment, flow);
  }

  _init_size(form, amount, increment, flow);

  // we recreate the form points
  dt_masks_gui_form_create(form, gui, index, module);

  return 1;
}

/* Shape handlers receive widget-space coordinates, while normalized output-image
 * coordinates come from `gui->rel_pos` and absolute output-image
 * coordinates come from `gui->pos`. */
static int _circle_events_mouse_scrolled(struct dt_iop_module_t *module, double x, double y, int up, const int flow,
                                         uint32_t state, dt_masks_form_t *form, int parentid,
                                         dt_masks_form_gui_t *gui, int index,
                                         dt_masks_interaction_t interaction)
{
  
  
  
  // `state` is the caller's raw key state, kept for the callback signature: the property to
  // act on was already resolved from it by dt_masks_scroll_get_interaction(). A circle owns no
  // rotation, so that mapping falls through and the wheel does nothing here.
  if(gui->creation)
  {
    switch(interaction)
    {
      case DT_MASKS_INTERACTION_OPACITY:
        return _init_opacity(form, up ? +0.02f : -0.02f, DT_MASKS_INCREMENT_OFFSET, flow);
      case DT_MASKS_INTERACTION_FADING:
        return _init_fading(form, up ? +1.02f : 0.98f, DT_MASKS_INCREMENT_SCALE, flow);
      case DT_MASKS_INTERACTION_SIZE:
        return _init_size(form, up ? +1.02f : 0.98f, DT_MASKS_INCREMENT_SCALE, flow);
      default:
        return 0;
    }
  }
  else if(gui->form_selected)
  {
    switch(interaction)
    {
      case DT_MASKS_INTERACTION_OPACITY:
        return dt_masks_form_change_opacity(gui->dev, form, parentid, up, flow);
      case DT_MASKS_INTERACTION_FADING:
        return _change_fading(form, gui, module, index, up ? +1.02f : 0.98f, DT_MASKS_INCREMENT_SCALE, flow);
      case DT_MASKS_INTERACTION_SIZE:
        return _change_size(form, gui, module, index, up ? +1.02f : 0.98f, DT_MASKS_INCREMENT_SCALE, flow);
      default:
        return 0;
    }
  }
  return 0;
}

static int _circle_events_button_pressed(struct dt_iop_module_t *module, double x, double y,
                                         double pressure, int which, int type, uint32_t state,
                                         dt_masks_form_t *form, int parentid, dt_masks_form_gui_t *gui, int index)
{
  if(which == 1)
  {
    if(gui->creation)
    {
      if((dt_modifier_is(state, DT_PRIMARY_MASK | GDK_SHIFT_MASK)) || dt_modifier_is(state, GDK_SHIFT_MASK))
      {
        // set some absolute or relative position for the source of the clone mask
        if(form->type & DT_MASKS_CLONE) dt_masks_set_source_pos_initial_state(gui, state);
        return 1;
      }

      dt_iop_module_t *crea_module = gui->creation_module;
      // we create the circle
      dt_masks_node_circle_t *circle = (dt_masks_node_circle_t *)(malloc(sizeof(dt_masks_node_circle_t)));
      if(IS_NULL_PTR(circle)) return 0;

      _circle_init_new(form, gui, circle);
      form->points = g_list_append(form->points, circle);
      dt_masks_gui_form_save_creation(gui->dev, crea_module, form, gui);

      return 1;
    }
    else // creation is FALSE
    {
      dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
      if(IS_NULL_PTR(gpt)) return 0;

      if(gui->source_selected && gui->edit_mode == DT_MASKS_EDIT_FULL)
      {
        // we start the source dragging
        gui->delta[0] = gpt->source[0] - gui->pos[0];
        gui->delta[1] = gpt->source[1] - gui->pos[1];
        return 1;
      }
      else if(gui->form_selected && gui->edit_mode == DT_MASKS_EDIT_FULL)
      {
        // we start the form dragging
        gui->delta[0] = gpt->points[0] - gui->pos[0];
        gui->delta[1] = gpt->points[1] - gui->pos[1];
        return 1;
      }
      else if(gui->handle_hovered >= 0 && gui->edit_mode == DT_MASKS_EDIT_FULL)
      {
        return 1;
      }
    }
  }

  return 0;
}

static int _circle_events_button_released(struct dt_iop_module_t *module, double x, double y, int which,
                                          uint32_t state, dt_masks_form_t *form, int parentid,
                                          dt_masks_form_gui_t *gui, int index)
{
  
  
  
  
  
  
  
  if(gui->form_dragging)
  {
    // we end the form dragging
    return 1;
  }
  else if(gui->source_dragging)
  {
    return 1;
  }
  return 0;
}

static int _circle_events_key_pressed(struct dt_iop_module_t *module, GdkEventKey *event, dt_masks_form_t *form,
                                              int parentid, dt_masks_form_gui_t *gui, int index)
{
  return 0;
}

static int _circle_events_mouse_moved(struct dt_iop_module_t *module, double x, double y, double pressure,
                                      int which, dt_masks_form_t *form, int parentid,
                                      dt_masks_form_gui_t *gui, int index)
{
  if(gui->creation)
  {
    // Let the cursor motion be redrawn as it moves in GUI
    return 1;
  }

  if(IS_NULL_PTR(form) || IS_NULL_PTR(form->points)) return 0;

  if(gui->form_dragging || gui->source_dragging)
  {
    dt_develop_t *dev = gui->dev;
    // apply delta to the current mouse position
    float pts[2];
    dt_masks_gui_delta_to_raw_norm(dev, gui, pts);

    // we move all points in normalized input space
    if(gui->form_dragging)
    {
      dt_masks_node_circle_t *circle = (dt_masks_node_circle_t *)((form->points)->data);
      if(IS_NULL_PTR(circle)) return 0;
      circle->center[0] = pts[0];
      circle->center[1] = pts[1];
    }
    else if(gui->source_dragging)
    {
      form->source[0] = pts[0];
      form->source[1] = pts[1];
    }

    // we recreate the form points
    dt_masks_gui_form_create(form, gui, index, module);

    return 1;
  }
  return 0;
}

static void _circle_draw_shape(dt_develop_t *dev, cairo_t *cr, const float *points, const int points_count, const int coord_nb, const gboolean border, const gboolean source,
                              const dt_masks_skip_range_t *skips, const int skip_count)
{
  /* A circle has no self-intersections, so it never carries an exclusion list -- these are the
   * shared shape_draw_function_t signature, not something to honour here. */
  (void)dev; (void)border; (void)source; (void)skips; (void)skip_count;

  /* One line_to per point sampled at RAW resolution is several per device pixel; emit at the
   * resolution the context can actually show. See dt_draw_min_emit_step() (mirrors the brush). */
  const double min_step = dt_draw_min_emit_step(cr);
  const double min_step2 = min_step * min_step;
  double last_x = points[coord_nb * 2 + 2], last_y = points[coord_nb * 2 + 3];
  cairo_move_to(cr, last_x, last_y);
  for(int i = 2; i < points_count; i++)
  {
    const double x = points[i * 2], y = points[i * 2 + 1];
    const double dx = x - last_x, dy = y - last_y;
    if((dx * dx + dy * dy) < min_step2) continue;
    cairo_line_to(cr, x, y);
    last_x = x; last_y = y;
  }
  cairo_close_path(cr);
}

static float *_points_to_transform(dt_develop_t *dev, float x, float y, float radius, float wd, float ht, int *points_count)
{
  // how many points do we need?
  const float r = radius * MIN(wd, ht);
  const size_t l = (size_t)(2.0f * M_PI * r);
  // allocate buffer
  float *const restrict points = dt_pixelpipe_cache_alloc_align_float_cache((l + 1) * 2, 0);
  if(IS_NULL_PTR(points))
  {
    *points_count = 0;
    return NULL;
  }
  *points_count = l + 1;

  // now we set the points, first the center, then the circumference
  float center[2] = { x, y };
  dt_dev_coordinates_raw_norm_to_raw_abs(dev, center, 1);
  const float center_x = center[0];
  const float center_y = center[1];
  points[0] = center_x;
  points[1] = center_y;
  __OMP_PARALLEL_FOR_SIMD__(if(l > 100) aligned(points:64))
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
   // global callback signature
  
  const dt_dev_image_geometry_t geometry = dt_dev_geometry_snapshot(dev);
  const float wd = geometry.raw_width;
  const float ht = geometry.raw_height;

  // compute the points of the target (center and circumference of circle)
  // we get the point in RAW image reference
  *points = _points_to_transform(dev, x, y, radius, wd, ht, points_count);
  if(IS_NULL_PTR(*points)) return 1;

  return dt_masks_points_shift_to_source(dev, module, points, points_count, xs, ys, 0);
}

static int _circle_get_points(dt_develop_t *dev, float x, float y, float radius, float radius2, float rotation,
                              float **points, int *points_count)
{
   // global callback signature
  
  const dt_dev_image_geometry_t geometry = dt_dev_geometry_snapshot(dev);
  const float wd = geometry.raw_width;
  const float ht = geometry.raw_height;

  // compute the points we need to transform (center and circumference of circle)
  *points = _points_to_transform(dev, x, y, radius, wd, ht, points_count);
  if(IS_NULL_PTR(*points)) return 1;

  // and transform them with all distorted modules
  if(!dt_dev_coordinates_raw_abs_to_image_abs(dev, *points, *points_count))
  {
    dt_pixelpipe_cache_free_align(*points);
    *points = NULL;
    *points_count = 0;
    return 1;
  }

  // if we failed, then free all and return
  return 0;
}

static void _circle_events_post_expose(cairo_t *cr, float zoom_scale, dt_masks_form_gui_t *gui, int index, int num_points)
{
  // add a preview when creating a circle
  // in creation mode
  if(gui->creation)
  {
    dt_masks_form_t *form = dt_masks_get_visible_form(gui->dev);
    dt_masks_preview_buffers_t preview;
    if(_circle_get_creation_preview(form, gui, &preview)) return;

    dt_masks_draw_preview_shape(gui->dev, cr, zoom_scale, num_points, preview.points, preview.points_count,
                                preview.border, preview.border_count,
                                &dt_masks_functions_circle.draw_shape, CAIRO_LINE_CAP_BUTT,
                                CAIRO_LINE_CAP_ROUND, FALSE, FALSE);

    // draw a cross where the source will be created
    if(dt_masks_form_is_clone(form))
    {
      dt_masks_draw_source_preview(cr, zoom_scale, gui, gui->pos[0], gui->pos[1], gui->pos[0], gui->pos[1], FALSE);
      dt_masks_draw_preview_shape(gui->dev, cr, zoom_scale, num_points, preview.source_points, preview.points_count,
                                NULL, 0, &dt_masks_functions_circle.draw_shape, CAIRO_LINE_CAP_BUTT,
                                CAIRO_LINE_CAP_ROUND, FALSE, TRUE);
    }
    dt_masks_preview_buffers_cleanup(&preview);

    return;
  } // creation

  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(IS_NULL_PTR(gpt)) return;
  
  // we draw the main shape
  const gboolean selected = (gui->group_selected == index) && (gui->form_selected || gui->form_dragging);
  if(gpt->points && gpt->points_count > 1)
    dt_draw_shape_lines(gui->dev, DT_MASKS_NO_DASH, FALSE, cr, num_points, selected, zoom_scale, gpt->points,
                        gpt->points_count, &dt_masks_functions_circle.draw_shape, CAIRO_LINE_CAP_BUTT, NULL, 0);
  // we draw the borders
  if(gui->group_selected == index)
  { 
    if(gpt->border && gpt->border_count > 1)
      dt_draw_shape_lines(gui->dev, DT_MASKS_DASH_STICK, FALSE, cr, num_points, (gui->border_selected), zoom_scale, gpt->border,
                          gpt->border_count, &dt_masks_functions_circle.draw_shape, CAIRO_LINE_CAP_ROUND, NULL, 0);
  }

  // draw the source if any
  if(gpt->source && gpt->source_count > 6 && gpt->points && gpt->points_count > 0)
  {
    dt_masks_gui_center_point_t center_pt = { .main = { gpt->points[0], gpt->points[1] },
                                              .source = { gpt->source[0], gpt->source[1] }};

    dt_masks_draw_source(cr, gui, index, num_points, zoom_scale, &center_pt, &dt_masks_functions_circle.draw_shape);
  }
}


static dt_masks_raster_result_t _circle_get_points_border(dt_develop_t *dev, struct dt_masks_form_t *form,
                                     float **points,
                                     int *points_count, float **border, int *border_count,
                                     dt_masks_skip_range_t **border_skips, int *border_skip_count,
                                     int source,
                                     const dt_iop_module_t *module)
{
  if(!IS_NULL_PTR(border_skips)) *border_skips = NULL;
  if(!IS_NULL_PTR(border_skip_count)) *border_skip_count = 0;

  /* No geometry to build an outline from: the outline is empty, which is a result, not a
   * failure. Reporting success here (as this did) told the caller to cache a NULL outline as
   * if it had been built. */
  if(IS_NULL_PTR(form) || IS_NULL_PTR(form->points)) return DT_MASKS_RASTER_EMPTY;
  dt_masks_node_circle_t *circle = (dt_masks_node_circle_t *)((form->points)->data);
  if(IS_NULL_PTR(circle)) return DT_MASKS_RASTER_EMPTY;
  float x = circle->center[0];
  float y = circle->center[1];
  if(source)
  {
    float xs = form->source[0];
    float ys = form->source[1];
    return dt_masks_raster_from_status(
        _circle_get_points_source(dev, x, y, xs, ys, circle->radius, circle->radius, 0.0f, points, points_count, module));
  }
  else
  {
    if(form->functions->get_points(dev, x, y, circle->radius, circle->radius, 0, points, points_count) != 0)
      return DT_MASKS_RASTER_ERROR;
    if(!IS_NULL_PTR(border))
    {
      float outer_radius = circle->radius + circle->border;
      return dt_masks_raster_from_status(
          form->functions->get_points(dev, x, y, outer_radius, outer_radius, 0, border, border_count));
    }
    return DT_MASKS_RASTER_OK;
  }
}

static dt_masks_raster_result_t _circle_get_source_area(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe,
                                   dt_dev_pixelpipe_iop_t *piece,
                                   dt_masks_form_t *form, int *width, int *height, int *posx, int *posy)
{
  /* Every early exit below must leave the out-parameters defined: callers read them
   * whenever this reports anything but ERROR, and three of them used to report success
   * for a shape with no geometry while writing none of these. */
  *width = 0;
  *height = 0;
  *posx = 0;
  *posy = 0;
  // we get the circle values
  if(IS_NULL_PTR(form) || IS_NULL_PTR(form->points)) return DT_MASKS_RASTER_EMPTY;
  dt_masks_node_circle_t *circle = (dt_masks_node_circle_t *)((form->points)->data);
  if(IS_NULL_PTR(circle)) return DT_MASKS_RASTER_EMPTY;
  float wd = pipe->iwidth, ht = pipe->iheight;

  // compute the points we need to transform (center and circumference of circle)
  const float outer_radius = circle->radius + circle->border;
  int num_points;
  float *const restrict points =
    _points_to_transform(pipe->dev, form->source[0], form->source[1], outer_radius, wd, ht, &num_points);
  if(IS_NULL_PTR(points))
    return DT_MASKS_RASTER_ERROR;

  // and transform them with all distorted modules
  if(!dt_dev_distort_transform_plus(pipe, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, points, num_points))
  {
    dt_pixelpipe_cache_free_align(points);
    return DT_MASKS_RASTER_ERROR;
  }

  dt_masks_points_bounding_box(points, num_points, width, height, posx, posy);
  dt_pixelpipe_cache_free_align(points);
  return DT_MASKS_RASTER_OK;
}

static dt_masks_raster_result_t _circle_get_area(const dt_iop_module_t *const restrict module, dt_dev_pixelpipe_t *pipe,
                            const dt_dev_pixelpipe_iop_t *const restrict piece,
                            dt_masks_form_t *const restrict form,
                            int *width, int *height, int *posx, int *posy)
{
  *width = 0;
  *height = 0;
  *posx = 0;
  *posy = 0;
  if(IS_NULL_PTR(form) || IS_NULL_PTR(form->points)) return DT_MASKS_RASTER_EMPTY;
  // we get the circle values
  dt_masks_node_circle_t *circle = (dt_masks_node_circle_t *)((form->points)->data);
  if(IS_NULL_PTR(circle)) return DT_MASKS_RASTER_EMPTY;
  float wd = pipe->iwidth, ht = pipe->iheight;

  // compute the points we need to transform (center and circumference of circle)
  const float outer_radius = circle->radius + circle->border;
  int num_points;
  float *const restrict points =
    _points_to_transform(pipe->dev, circle->center[0], circle->center[1], outer_radius, wd, ht, &num_points);
  if(IS_NULL_PTR(points))
    return DT_MASKS_RASTER_ERROR;

  // and transform them with all distorted modules
  if(!dt_dev_distort_transform_plus(pipe, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, points, num_points))
  {
    dt_pixelpipe_cache_free_align(points);
    return DT_MASKS_RASTER_ERROR;
  }

  dt_masks_points_bounding_box(points, num_points, width, height, posx, posy);
  dt_pixelpipe_cache_free_align(points);
  return DT_MASKS_RASTER_OK;
}

static dt_masks_raster_result_t _circle_get_mask(const dt_iop_module_t *const restrict module, dt_dev_pixelpipe_t *pipe,
                            const dt_dev_pixelpipe_iop_t *const restrict piece,
                            dt_masks_form_t *const restrict form,
                            float **buffer, int *width, int *height, int *posx, int *posy)
{
  *buffer = NULL;
  *width = 0;
  *height = 0;
  *posx = 0;
  *posy = 0;
  double start2 = 0.0;
  if(dt_get_debug_flags() & DT_DEBUG_PERF) start2 = dt_get_wtime();

  // we get the area
  const dt_masks_raster_result_t area = _circle_get_area(module, pipe, piece, form, width, height, posx, posy);
  if(area != DT_MASKS_RASTER_OK) return area;

  if(dt_get_debug_flags() & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle area took %0.04f sec\n", form->name, dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we get the circle values
  dt_masks_node_circle_t *const restrict circle = (dt_masks_node_circle_t *)((form->points)->data);

  // we create a buffer of points with all points in the area
  const int w = *width, h = *height;
  float *const restrict points = dt_pixelpipe_cache_alloc_align_float_cache((size_t)w * h * 2, 0);
  if(IS_NULL_PTR(points))
    return DT_MASKS_RASTER_ERROR;

  const float pos_x = *posx;
  const float pos_y = *posy;
  __OMP_PARALLEL_FOR__(if(h*w > 50000) num_threads(MIN(dt_get_num_openmp_threads(),(h*w)/20000)))
  for(int i = 0; i < h; i++)
  {
    float *const restrict p = points + 2 * i * w;
    const float y = i + pos_y;
    __OMP_SIMD__(aligned(points : 64))
    for(int j = 0; j < w; j++)
    {
      p[2*j] = pos_x + j;
      p[2*j + 1] = y;
    }
  }
  if(dt_get_debug_flags() & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle draw took %0.04f sec\n", form->name, dt_get_wtime() - start2);

    start2 = dt_get_wtime();
  }
  // we back transform all this points
  if(!dt_dev_distort_backtransform_plus(pipe, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, points, (size_t)w * h))
  {
    dt_pixelpipe_cache_free_align(points);
    return DT_MASKS_RASTER_ERROR;
  }

  if(dt_get_debug_flags() & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle transform took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we allocate the buffer
  *buffer = dt_pixelpipe_cache_alloc_align_float_cache((size_t)w * h, 0);
  if(IS_NULL_PTR(*buffer))
  {
    dt_pixelpipe_cache_free_align(points);
    return DT_MASKS_RASTER_ERROR;
  }

  // we populate the buffer
  float *const restrict ptbuffer = *buffer;
  const int wi = pipe->iwidth, hi = pipe->iheight;
  const int mindim = MIN(wi, hi);
  const float centerx = circle->center[0] * wi;
  const float centery = circle->center[1] * hi;
  const float radius2 = circle->radius * mindim * circle->radius * mindim;
  const float total2 = (circle->radius + circle->border) * mindim * (circle->radius + circle->border) * mindim;
  const float border2 = total2 - radius2;
  __OMP_PARALLEL_FOR_SIMD__(if(h*w > 50000) num_threads(MIN(dt_get_num_openmp_threads(),(h*w)/20000))  aligned(points, ptbuffer : 64))
  for(int i = 0 ; i < h*w; i++)
  {
    // find the square of the distance from the center
    const float l2 = sqf(points[2 * i] - centerx) + sqf(points[2 * i + 1] - centery);
    // quadratic falloff between the circle's radius and the radius of the outside of the feathering
    const float ratio = (total2 - l2) / border2;
    // enforce 1.0 inside the circle and 0.0 outside the feathering
    const float f = CLIP(ratio);
    ptbuffer[i] = sqf(f);
  }

  dt_pixelpipe_cache_free_align(points);

  if(dt_get_debug_flags() & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle fill took %0.04f sec\n", form->name, dt_get_wtime() - start2);

  return DT_MASKS_RASTER_OK;
}


static dt_masks_raster_result_t _circle_get_mask_roi(const dt_iop_module_t *const restrict module, dt_dev_pixelpipe_t *pipe,
                                const dt_dev_pixelpipe_iop_t *const restrict piece,
                                dt_masks_form_t *const form, const dt_iop_roi_t *const roi,
                                float *const restrict buffer, dt_iop_roi_t *touched)
{
  dt_masks_touched_none(touched);
  if(IS_NULL_PTR(form) || IS_NULL_PTR(form->points)) return DT_MASKS_RASTER_EMPTY;
  if(IS_NULL_PTR(module)) return DT_MASKS_RASTER_ERROR;
  double start1 = 0.0;
  double start2 = start1;
  
  if(dt_get_debug_flags() & DT_DEBUG_PERF) start2 = start1 = dt_get_wtime();

  // we get the circle parameters
  dt_masks_node_circle_t *circle = (dt_masks_node_circle_t *)((form->points)->data);
  if(IS_NULL_PTR(circle)) return DT_MASKS_RASTER_EMPTY;
  const int wi = pipe->iwidth, hi = pipe->iheight;
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
  // scale dependent resolution: when zoomed in (scale > 1), use finer grid to avoid interpolation holes
  const float grid_scale = 1.0f / MAX(roi->scale, 1e-6f);
  const int grid = CLAMP((10.0f * grid_scale + 2.0f) / 3.0f, 1, 4);
  const int grid_width = (width + grid - 1) / grid + 1;  // grid dimension of total roi
  const int grid_height = (height + grid - 1) / grid + 1;  // grid dimension of total roi

  // initialize output buffer with zero
  memset(buffer, 0, sizeof(float) * width * height);

  if(dt_get_debug_flags() & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle init took %0.04f sec\n", form->name, dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we look at the outer circle of the shape - no effects outside of this circle;
  // we need many points as we do not know how the circle might get distorted in the pixelpipe
  const size_t circpts = dt_masks_roundup(MIN(360, 2 * M_PI * sqr_total), 8);
  float *const restrict circ = dt_pixelpipe_cache_alloc_align_float_cache(circpts * 2, 0);
  if(IS_NULL_PTR(circ)) return DT_MASKS_RASTER_ERROR;
  __OMP_PARALLEL_FOR__(if(circpts/8 > 1000))
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
  if(!dt_dev_distort_transform_plus(pipe, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, circ,
                                        circpts))
  {
    dt_pixelpipe_cache_free_align(circ);
    return DT_MASKS_RASTER_ERROR;
  }

  if(dt_get_debug_flags() & DT_DEBUG_PERF)
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

  dt_pixelpipe_cache_free_align(circ);

  if(dt_get_debug_flags() & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle bounding box took %0.04f sec\n", form->name, dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // check if there is anything to do at all;
  // only if width and height of bounding box is 2 or greater the shape lies inside of roi and requires action
  if(bbw <= 1 || bbh <= 1)
    return DT_MASKS_RASTER_EMPTY;

  const dt_masks_sample_grid_t sample_grid
      = { .x0 = bbxm, .y0 = bbym, .width = bbw, .height = bbh,
          .step = grid, .px = px, .py = py, .iscale = iscale };
  float *const restrict points
      = dt_masks_sample_grid_backtransform(pipe, module->iop_order, &sample_grid, "circle", form->name);
  if(IS_NULL_PTR(points)) return DT_MASKS_RASTER_ERROR;
  start2 = dt_get_wtime();

  // we calculate the mask values at the transformed points;
  // for results: re-use the points array
  __OMP_PARALLEL_FOR__(collapse(2) if(bbh*bbw > 50000) num_threads(MIN(dt_get_num_openmp_threads(),(height*width)/20000)))
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

  if(dt_get_debug_flags() & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle draw took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we fill the pre-initialized output buffer by interpolation;
  // we only need to take the contents of our bounding box into account
  int endx = 0, endy = 0;
  dt_masks_sample_grid_interpolate(points, &sample_grid, buffer, width, height, &endx, &endy);

  dt_pixelpipe_cache_free_align(points);

  dt_masks_touched_set(touched, bbxm * grid, bbym * grid, endx - 1, endy - 1, width, height);

  if(dt_get_debug_flags() & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle fill took %0.04f sec\n", form->name, dt_get_wtime() - start2);
    dt_print(DT_DEBUG_MASKS, "[masks %s] circle total render took %0.04f sec\n", form->name,
             dt_get_wtime() - start1);
  }

  return DT_MASKS_RASTER_OK;
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
                                     char *const restrict msgbuf, const size_t msgbuf_len)
{
  // Only gestures that cannot be discovered any other way. What the wheel does is the user's
  // own mapping now (masks_gui.h), shown in the Drawn tab, so it is not repeated here.
  if(gui->creation)
  {
    if(dt_masks_form_is_clone(form))
      g_strlcat(msgbuf, _("<b>Set source</b>: Shift+Click"), msgbuf_len);
  }
  else if(gui->source_selected)
    g_strlcat(msgbuf, _("<b>Move source</b>: Drag"), msgbuf_len);
  else if(gui->form_selected || gui->border_selected)
    g_strlcat(msgbuf, _("<b>Move</b>: Drag"), msgbuf_len);
}

static void _circle_duplicate_points(dt_develop_t *dev, dt_masks_form_t *const base, dt_masks_form_t *const dest)
{
   // unused arg, keep compiler from complaining
  dt_masks_duplicate_points(base, dest, sizeof(dt_masks_node_circle_t));
}

static void _circle_initial_source_pos(dt_develop_t *dev, const float iwd, const float iht, float *x, float *y)
{
  const float radius = MIN(0.5f, dt_conf_get_float("plugins/darkroom/spots/circle/size"));
  float offset[2] = { radius, -radius };
  dt_dev_coordinates_raw_norm_to_raw_abs(dev, offset, 1);
  *x = offset[0];
  *y = offset[1];
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
  .get_gravity_center = _circle_get_gravity_center,
  .get_interaction_value = _circle_get_interaction_value,
  .set_interaction_value = _circle_set_interaction_value,
  .update_hover = _find_closest_handle,
  .mouse_moved = _circle_events_mouse_moved,
  .mouse_scrolled = _circle_events_mouse_scrolled,
  .button_pressed = _circle_events_button_pressed,
  .button_released = _circle_events_button_released,
  .key_pressed = _circle_events_key_pressed,
  .post_expose = _circle_events_post_expose,
  .draw_shape = _circle_draw_shape
};



// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
