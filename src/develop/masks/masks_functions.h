/*
    This file is part of darktable,
    Copyright (C) 2013-2014, 2016, 2019, 2021 Aldric Renaudin.
    Copyright (C) 2013, 2018, 2020-2021 Pascal Obry.
    Copyright (C) 2013 Simon Spannagel.
    Copyright (C) 2013-2014, 2016-2018 Tobias Ellinghaus.
    Copyright (C) 2013-2017, 2019-2020 Ulrich Pegelow.
    Copyright (C) 2014-2016 Roman Lebedev.
    Copyright (C) 2017-2019 Edgardo Hoszowski.
    Copyright (C) 2021 Hanno Schwalm.
    Copyright (C) 2021 Hubert Kowalski.
    Copyright (C) 2021 luzpaz.
    Copyright (C) 2021 Philipp Lutz.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2023 Luca Zulberti.
    Copyright (C) 2025 Alynx Zhou.
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

/** @file develop/masks/masks_functions.h
 *
 * @brief The per-shape function table, private to the masks implementation.
 *
 * @details Every member is dispatched from inside src/develop/masks/ and nowhere else
 * (measured before the move), so nothing outside the shape files and the masks core has
 * any business seeing the table's layout. Public code holds it only as the opaque
 * `dt_masks_form_t.functions` pointer, forward-declared in develop/masks.h. Keeping the
 * definition here is also what keeps drawing and event types out of the public header:
 * several members take dt_masks_form_gui_t* or a shape_draw_function_t.
 */

#ifndef DT_DEVELOP_MASKS_MASKS_FUNCTIONS_H
#define DT_DEVELOP_MASKS_MASKS_FUNCTIONS_H

#include "develop/masks.h"
#include "develop/masks_gui.h"

#ifdef __cplusplus
extern "C" {
#endif


/** structure used to store pointers to the functions implementing operations on a mask shape */
/** plus a few per-class descriptive data items */
typedef struct dt_masks_functions_t
{
  int point_struct_size;   // sizeof(struct dt_masks_point_*_t)
  void (*sanitize_config)(dt_masks_type_t type_flags);
  void (*set_form_name)(struct dt_masks_form_t *const form, const size_t nb);
  void (*set_hint_message)(const struct dt_masks_form_gui_t *const gui, const struct dt_masks_form_t *const form,
                           const int opacity, char *const __restrict__ msgbuf, const size_t msgbuf_len);
  void (*duplicate_points)(struct dt_develop_t *const dev, struct dt_masks_form_t *base, struct dt_masks_form_t *dest);
  void (*initial_source_pos)(struct dt_develop_t *dev, const float iwd, const float iht, float *x, float *y);
  // input coordinates are in absolute output-image space, dist is squared in the same space
  void (*get_distance)(float x, float y, float as, struct dt_masks_form_gui_t *gui, int index, int num_points,
                       int *inside, int *inside_border, int *near_handle, int *inside_source, float *dist);
  int (*get_points)(struct dt_develop_t *dev, float x, float y, float radius_a, float radius_b, float rotation,
                    float **points, int *points_count);
  int (*get_points_border)(struct dt_develop_t *dev, struct dt_masks_form_t *form, float **points, int *points_count,
                           float **border, int *border_count, int source, const dt_iop_module_t *const module);
  int (*get_mask)(const dt_iop_module_t *const module, struct dt_dev_pixelpipe_t *pipe,
                  const dt_dev_pixelpipe_iop_t *const piece,
                  struct dt_masks_form_t *const form,
                  float **buffer, int *width, int *height, int *posx, int *posy);
  int (*get_mask_roi)(const dt_iop_module_t *const fmodule, struct dt_dev_pixelpipe_t *pipe,
                      const dt_dev_pixelpipe_iop_t *const piece,
                      struct dt_masks_form_t *const form,
                      const dt_iop_roi_t *roi, float *buffer);
  int (*get_area)(const dt_iop_module_t *const module, struct dt_dev_pixelpipe_t *pipe,
                  const dt_dev_pixelpipe_iop_t *const piece,
                  struct dt_masks_form_t *const form,
                  int *width, int *height, int *posx, int *posy);
  int (*get_source_area)(dt_iop_module_t *module, struct dt_dev_pixelpipe_t *pipe,
                         dt_dev_pixelpipe_iop_t *piece, struct dt_masks_form_t *form,
                         int *width, int *height, int *posx, int *posy);
  gboolean (*get_gravity_center)(struct dt_develop_t *dev, const struct dt_masks_form_t *form, float center[2], float *area);
  float (*get_interaction_value)(const struct dt_masks_form_t *form, dt_masks_interaction_t interaction);
  float (*set_interaction_value)(struct dt_masks_form_t *form, dt_masks_interaction_t interaction, float value,
                                 dt_masks_increment_t increment, int flow,
                                 struct dt_masks_form_gui_t *gui, struct dt_iop_module_t *module);
  /* Recompute hovered handles/nodes from the cached cursor state in gui. */
  int (*update_hover)(struct dt_masks_form_t *form, struct dt_masks_form_gui_t *gui, int index);
  /* Mouse x and y are widget-space coordinates from GTK/Cairo */
  int (*mouse_moved)(struct dt_iop_module_t *module, double x, double y, double pressure, int which,
                     struct dt_masks_form_t *form, int parentid, struct dt_masks_form_gui_t *gui, int index);
  /* Mouse x and y are widget-space coordinates from GTK/Cairo */
  int (*mouse_scrolled)(struct dt_iop_module_t *module, double x, double y, int up, const int delta_y, uint32_t state,
                        struct dt_masks_form_t *form, int parentid, struct dt_masks_form_gui_t *gui, int index,
                        dt_masks_interaction_t interaction);
  /* Mouse x and y are widget-space coordinates from GTK/Cairo */
  int (*button_pressed)(struct dt_iop_module_t *module, double x, double y,
                        double pressure, int which, int type, uint32_t state,
                        struct dt_masks_form_t *form, int parentid, struct dt_masks_form_gui_t *gui, int index);
  /* Mouse x and y are widget-space coordinates from GTK/Cairo */
  int (*button_released)(struct dt_iop_module_t *module, double x, double y, int which, uint32_t state,
                         struct dt_masks_form_t *form, int parentid, struct dt_masks_form_gui_t *gui, int index);
  /* Key event */
  int (*key_pressed)(struct dt_iop_module_t *module, GdkEventKey *event, struct dt_masks_form_t *form, int parentid, struct dt_masks_form_gui_t *gui, int index);
  void (*post_expose)(cairo_t *cr, float zoom_scale, struct dt_masks_form_gui_t *gui, int index, int num_points);
  // The function to draw the shape in question. Signature must match shape_draw_function_t.
  shape_draw_function_t draw_shape;
  /** initialise all control points to eventually match a catmull-rom like spline */
  void (*init_ctrl_points)(struct dt_masks_form_t *form);
  int (*populate_context_menu)(GtkWidget *menu, struct dt_masks_form_t *form, struct dt_masks_form_gui_t *gui, const float pzx, const float pzy);
} dt_masks_functions_t;

/** the shape-specific function tables */
extern const dt_masks_functions_t dt_masks_functions_circle;
extern const dt_masks_functions_t dt_masks_functions_ellipse;
extern const dt_masks_functions_t dt_masks_functions_brush;
extern const dt_masks_functions_t dt_masks_functions_polygon;
extern const dt_masks_functions_t dt_masks_functions_gradient;
extern const dt_masks_functions_t dt_masks_functions_group;

/** init dt_masks_form_gui_t struct with default values */


/* Shared between masks.c (data/persistence) and masks_gui.c (interaction) since the
 * GUI half moved out; internal to the masks subsystem like everything here. */
void dt_masks_form_gui_points_free(gpointer data);
void _check_id(dt_develop_t *dev, dt_masks_form_t *mask_form);
void _set_group_name_from_module(dt_iop_module_t *module, dt_masks_form_t *group_form);
dt_masks_form_t *_group_create(dt_develop_t *develop, dt_iop_module_t *module, dt_masks_type_t group_type);
dt_masks_form_t *_group_from_module(dt_develop_t *develop, dt_iop_module_t *module);
float _change_opacity(dt_masks_form_group_t *form_group, float value,
                             const dt_masks_increment_t increment, const int flow);
int _find_in_group(dt_develop_t *dev, dt_masks_form_t *group_form, int form_id);

#ifdef __cplusplus
}
#endif

#endif // DT_DEVELOP_MASKS_MASKS_FUNCTIONS_H
