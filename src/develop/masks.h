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

#pragma once

#include "common/darktable.h"
#include "common/opencl.h"
#include "develop/pixelpipe.h"
#include "dtgtk/button.h"
#include "dtgtk/gradientslider.h"
#include "gui/draw.h"
#include "control/control.h"

#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVELOP_MASKS_VERSION (6)

/**forms types */
typedef enum dt_masks_type_t
{
  DT_MASKS_NONE = 0, // keep first
  DT_MASKS_CIRCLE = 1 << 0,
  DT_MASKS_POLYGON = 1 << 1,
  DT_MASKS_GROUP = 1 << 2,
  DT_MASKS_CLONE = 1 << 3,
  DT_MASKS_GRADIENT = 1 << 4,
  DT_MASKS_ELLIPSE = 1 << 5,
  DT_MASKS_BRUSH = 1 << 6,
  DT_MASKS_NON_CLONE = 1 << 7,

  DT_MASKS_ALL = DT_MASKS_CIRCLE | DT_MASKS_POLYGON | DT_MASKS_GROUP |
                 DT_MASKS_GRADIENT | DT_MASKS_ELLIPSE | DT_MASKS_BRUSH,

  DT_MASKS_IS_CLOSED_SHAPE = DT_MASKS_CIRCLE | DT_MASKS_ELLIPSE | DT_MASKS_POLYGON,
  DT_MASKS_IS_OPEN_SHAPE   = DT_MASKS_ALL & ~DT_MASKS_IS_CLOSED_SHAPE,
  
  DT_MASKS_IS_PATH_SHAPE   = DT_MASKS_POLYGON | DT_MASKS_BRUSH,
  DT_MASKS_IS_SIMPLE_SHAPE = DT_MASKS_CIRCLE | DT_MASKS_ELLIPSE | DT_MASKS_GRADIENT

} dt_masks_type_t;

/**masts states */
typedef enum dt_masks_state_t
{
  DT_MASKS_STATE_NONE = 0,
  DT_MASKS_STATE_USE = 1 << 0,
  DT_MASKS_STATE_SHOW = 1 << 1,
  DT_MASKS_STATE_INVERSE = 1 << 2,
  DT_MASKS_STATE_UNION = 1 << 3,
  DT_MASKS_STATE_INTERSECTION = 1 << 4,
  DT_MASKS_STATE_DIFFERENCE = 1 << 5,
  DT_MASKS_STATE_EXCLUSION = 1 << 6
} dt_masks_state_t;

typedef enum dt_masks_points_states_t
{
  DT_MASKS_POINT_STATE_NORMAL = 1,
  DT_MASKS_POINT_STATE_USER = 2
} dt_masks_points_states_t;

typedef enum dt_masks_gradient_states_t
{
  DT_MASKS_GRADIENT_STATE_LINEAR = 1,
  DT_MASKS_GRADIENT_STATE_SIGMOIDAL = 2
} dt_masks_gradient_states_t;

typedef enum dt_masks_increment_t
{
  DT_MASKS_INCREMENT_ABSOLUTE = 0,
  DT_MASKS_INCREMENT_SCALE = 1,
  DT_MASKS_INCREMENT_OFFSET = 2
} dt_masks_increment_t;

typedef enum dt_masks_edit_mode_t
{
  DT_MASKS_EDIT_OFF = 0,
  DT_MASKS_EDIT_FULL = 1,
  DT_MASKS_EDIT_RESTRICTED = 2
} dt_masks_edit_mode_t;

typedef enum dt_masks_pressure_sensitivity_t
{
  DT_MASKS_PRESSURE_OFF = 0,
  DT_MASKS_PRESSURE_HARDNESS_REL = 1,
  DT_MASKS_PRESSURE_HARDNESS_ABS = 2,
  DT_MASKS_PRESSURE_OPACITY_REL = 3,
  DT_MASKS_PRESSURE_OPACITY_ABS = 4,
  DT_MASKS_PRESSURE_BRUSHSIZE_REL = 5
} dt_masks_pressure_sensitivity_t;

typedef enum dt_masks_ellipse_flags_t
{
  DT_MASKS_ELLIPSE_EQUIDISTANT = 0,
  DT_MASKS_ELLIPSE_PROPORTIONAL = 1
} dt_masks_ellipse_flags_t;

typedef enum dt_masks_source_pos_type_t
{
  DT_MASKS_SOURCE_POS_RELATIVE = 0,
  DT_MASKS_SOURCE_POS_RELATIVE_TEMP = 1,
  DT_MASKS_SOURCE_POS_ABSOLUTE = 2
} dt_masks_source_pos_type_t;

/** structure used to store 1 node for a circle */
typedef struct dt_masks_node_circle_t
{
  float center[2]; // point in normalized input space
  float radius;
  float border;
} dt_masks_node_circle_t;

/** structure used to store 1 node for an ellipse */
typedef struct dt_masks_node_ellipse_t
{
  float center[2];
  float radius[2];
  float rotation;
  float border;
  dt_masks_ellipse_flags_t flags;
} dt_masks_node_ellipse_t;

/** structure used to store 1 node for a path form */
typedef struct dt_masks_node_polygon_t
{
  float node[2];
  float ctrl1[2];
  float ctrl2[2];
  float border[2];
  dt_masks_points_states_t state;
} dt_masks_node_polygon_t;

/** structure used to store 1 node for a brush form */
typedef struct dt_masks_node_brush_t
{
  float node[2];
  float ctrl1[2];
  float ctrl2[2];
  float border[2];
  float density;
  float hardness;
  dt_masks_points_states_t state;
} dt_masks_node_brush_t;

/** structure used to store anchor for a gradient */
typedef struct dt_masks_anchor_gradient_t
{
  float center[2];
  float rotation;
  float extent;
  float steepness;
  float curvature;
  dt_masks_gradient_states_t state;
} dt_masks_anchor_gradient_t;

/** structure used to store all forms's id for a group */
typedef struct dt_masks_form_group_t
{
  int formid;
  int parentid;
  int state;
  float opacity;
} dt_masks_form_group_t;


/*
* Type of user interaction to map with internal properties of masks.
* Those were initially handled implicitly by Shift/Ctrl/Shift+Ctrl + mouse scroll
* at the scope of each mask type, which is a shitty design when using Wacom tablets.
* This case is now covered by the DT_MASK_INTERACTION_UNDEF.
* Otherwise, when calling the mouse_scroll callback from GUI, we set the case
* explicitly, along with a value.
*/
typedef enum dt_masks_interaction_t
{
  DT_MASKS_INTERACTION_UNDEF = 0,    // let it be deduced contextually from key modifiers, implicit
  DT_MASKS_INTERACTION_SIZE = 1,     // property of the form (shape), explicit
  DT_MASKS_INTERACTION_HARDNESS = 2, // property of the form (shape), explicit
  DT_MASKS_INTERACTION_OPACITY = 3,  // property of the group in which the form is included, explicit
  DT_MASKS_INTERACTION_LAST
} dt_masks_interaction_t;

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
  void (*initial_source_pos)(const float iwd, const float iht, float *x, float *y);
  void (*get_distance)(float x, float y, float as, struct dt_masks_form_gui_t *gui, int index, int num_points,
                       int *inside, int *inside_border, int *near, int *inside_source, float *dist);
  int (*get_points)(struct dt_develop_t *dev, float x, float y, float radius_a, float radius_b, float rotation,
                    float **points, int *points_count);
  int (*get_points_border)(struct dt_develop_t *dev, struct dt_masks_form_t *form, float **points, int *points_count,
                           float **border, int *border_count, int source, const dt_iop_module_t *const module);
  int (*get_mask)(const dt_iop_module_t *const module, const dt_dev_pixelpipe_iop_t *const piece,
                  struct dt_masks_form_t *const form,
                  float **buffer, int *width, int *height, int *posx, int *posy);
  int (*get_mask_roi)(const dt_iop_module_t *const fmodule, const dt_dev_pixelpipe_iop_t *const piece,
                      struct dt_masks_form_t *const form,
                      const dt_iop_roi_t *roi, float *buffer);
  int (*get_area)(const dt_iop_module_t *const module, const dt_dev_pixelpipe_iop_t *const piece,
                  struct dt_masks_form_t *const form,
                  int *width, int *height, int *posx, int *posy);
  int (*get_source_area)(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, struct dt_masks_form_t *form,
                         int *width, int *height, int *posx, int *posy);
  /* Mouse pzx and pzy are normalized coordinates in full image space */
  int (*mouse_moved)(struct dt_iop_module_t *module, float pzx, float pzy, double pressure, int which,
                     struct dt_masks_form_t *form, int parentid, struct dt_masks_form_gui_t *gui, int index);
  /* Mouse pzx and pzy are normalized coordinates in full image space */
  int (*mouse_scrolled)(struct dt_iop_module_t *module, float pzx, float pzy, int up, const int delta_y, uint32_t state,
                        struct dt_masks_form_t *form, int parentid, struct dt_masks_form_gui_t *gui, int index,
                        dt_masks_interaction_t interaction);
  /* Mouse pzx and pzy are normalized coordinates in full image space */
  int (*button_pressed)(struct dt_iop_module_t *module, float pzx, float pzy,
                        double pressure, int which, int type, uint32_t state,
                        struct dt_masks_form_t *form, int parentid, struct dt_masks_form_gui_t *gui, int index);
  /* Mouse pzx and pzy are normalized coordinates in full image space */
  int (*button_released)(struct dt_iop_module_t *module, float pzx, float pzy, int which, uint32_t state,
                         struct dt_masks_form_t *form, int parentid, struct dt_masks_form_gui_t *gui, int index);
  /* Key event */
  int (*key_pressed)(struct dt_iop_module_t *module, GdkEventKey *event, struct dt_masks_form_t *form, int parentid, struct dt_masks_form_gui_t *gui, int index);
  void (*post_expose)(cairo_t *cr, float zoom_scale, struct dt_masks_form_gui_t *gui, int index, int num_points);
  // The function to draw the shape in question.
  void (*draw_shape)(cairo_t *cr, const float *points, const int points_count, const int nb, const gboolean border, const gboolean source);
  /** initialise all control points to eventually match a catmull-rom like spline */
  void (*init_ctrl_points)(struct dt_masks_form_t *form);
  int (*populate_context_menu)(GtkWidget *menu, struct dt_masks_form_t *form, struct dt_masks_form_gui_t *gui);
} dt_masks_functions_t;

/** structure used to define a form */
typedef struct dt_masks_form_t
{
  GList *points; // list of point structures (nodes)
  dt_masks_type_t type;
  const dt_masks_functions_t *functions;

  // position of the origin point of source (used only for clone)
  float source[2];
  // name of the form
  char name[128];
  // id used to store the form
  int formid;
  // version of the form
  int version;
} dt_masks_form_t;

/** structure used to define all the gui points to draw in viewport*/
typedef struct dt_masks_form_gui_points_t
{
  float *points;   // points in anormalized out space
  int points_count;
  float *border;   // border points in anormalized out space
  int border_count;
  float *source;   // source point in anormalized out space
  int source_count;
  gboolean clockwise;
} dt_masks_form_gui_points_t;

/** structure for dynamic buffers */
typedef struct dt_masks_dynbuf_t
{
  float *buffer;
  char tag[128];
  size_t pos;
  size_t size;
} dt_masks_dynbuf_t;


/** structure used to display a form */
typedef struct dt_masks_form_gui_t
{
  dt_masks_type_t type;
  // points used to draw the form
  GList *points; // list of dt_masks_form_gui_points_t 

  // points used to sample mouse moves
  dt_masks_dynbuf_t *guipoints, *guipoints_payload;
  int guipoints_count;

  // values for mouse positions, etc...

  // Mouse position (in anormalized out space)
  float pos[2];
  // delta movement of the mouse (in anormalized out space)
  float delta[2];
  // scroll offset
  float scrollx, scrolly;
  // Position of a clone mask's source point
  float pos_source[2];

  // TRUE if mouse has leaved the center window
  gboolean mouse_leaved_center;
  gboolean form_selected;
  gboolean border_selected;
  gboolean source_selected;
  gboolean pivot_selected;
  dt_masks_edit_mode_t edit_mode;
  int node_selected;
  int node_edited;
  int handle_selected;
  int seg_selected;
  int handle_border_selected;
  int source_pos_type;

  gboolean form_dragging;
  gboolean source_dragging;
  gboolean form_rotating;
  gboolean border_toggling;
  gboolean gradient_toggling;
  int node_dragging;
  int handle_dragging;
  int seg_dragging;
  int handle_border_dragging;

  int group_selected;


  gboolean creation;
  gboolean creation_closing_form;
  dt_iop_module_t *creation_module;

  dt_masks_pressure_sensitivity_t pressure_sensitivity;

  // ids
  int formid;
  uint64_t pipe_hash;
} dt_masks_form_gui_t;

typedef enum dt_masks_menu_icon_t
{
  DT_MASKS_MENU_ICON_NONE = 0,
  DT_MASKS_MENU_ICON_CIRCLE,
  DT_MASKS_MENU_ICON_SQUARE
} dt_masks_menu_icon_t;

typedef struct dt_masks_menu_icon_data_t
{
  dt_masks_menu_icon_t shape;
} dt_masks_menu_icon_data_t;

typedef struct dt_masks_gui_center_point_t
{
  struct
  {
    float x;
    float y;
  }main;

  struct 
  {
    float x;
    float y;
  }source;
} dt_masks_gui_center_point_t;

/** the shape-specific function tables */
extern const dt_masks_functions_t dt_masks_functions_circle;
extern const dt_masks_functions_t dt_masks_functions_ellipse;
extern const dt_masks_functions_t dt_masks_functions_brush;
extern const dt_masks_functions_t dt_masks_functions_polygon;
extern const dt_masks_functions_t dt_masks_functions_gradient;
extern const dt_masks_functions_t dt_masks_functions_group;

/** init dt_masks_form_gui_t struct with default values */
void dt_masks_init_form_gui(dt_masks_form_gui_t *gui);

/** get points in real space with respect of distortion dx and dy are used to eventually move the center of
 * the circle */
int dt_masks_get_points_border(struct dt_develop_t *dev, dt_masks_form_t *form, float **points, int *points_count,
                               float **border, int *border_count, int source, dt_iop_module_t *module);

/** get the rectangle which include the form and his border */
int dt_masks_get_area(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form,
                      int *width, int *height, int *posx, int *posy);
int dt_masks_get_source_area(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form,
                             int *width, int *height, int *posx, int *posy);
/** get the transparency mask of the form and his border */
static inline int dt_masks_get_mask(const dt_iop_module_t *const module, const dt_dev_pixelpipe_iop_t *const piece,
                      dt_masks_form_t *const form,
                      float **buffer, int *width, int *height, int *posx, int *posy)
{
  return (form->functions && form->functions->get_mask)
    ? form->functions->get_mask(module, piece, form, buffer, width, height, posx, posy)
    : 1;
}
static inline int dt_masks_get_mask_roi(const dt_iop_module_t *const module, const dt_dev_pixelpipe_iop_t *const piece,
                          dt_masks_form_t *const form, const dt_iop_roi_t *roi, float *buffer)
{
  return (form->functions && form->functions->get_mask_roi)
    ? form->functions->get_mask_roi(module, piece, form, roi, buffer)
    : 1;
}

int dt_masks_group_render_roi(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form,
                              const dt_iop_roi_t *roi, float *buffer);

// returns current masks version
int dt_masks_version(void);

void dt_masks_append_form(dt_develop_t *dev, dt_masks_form_t *form);
void dt_masks_remove_form(dt_develop_t *dev, dt_masks_form_t *form);
void dt_masks_remove_node(struct dt_iop_module_t *module, dt_masks_form_t *form, int parentid,
                          dt_masks_form_gui_t *gui, int index, int node_index);

// update masks from older versions
int dt_masks_legacy_params(dt_develop_t *dev, void *params, const int old_version, const int new_version);
/*
 * TODO:
 *
 * int
 * dt_masks_legacy_params(
 *   dt_develop_t *dev,
 *   const void *const old_params, const int old_version,
 *   void *new_params,             const int new_version);
 */

/** we create a completely new form. */
dt_masks_form_t *dt_masks_create(dt_masks_type_t type);
/** we create a completely new form and add it to darktable.develop->allforms. */
dt_masks_form_t *dt_masks_create_ext(dt_masks_type_t type);
/** replace dev->forms with forms */
void dt_masks_replace_current_forms(dt_develop_t *dev, GList *forms);
/** returns a form with formid == id from a list of forms */
dt_masks_form_t *dt_masks_get_from_id_ext(GList *forms, int id);
/** returns a form with formid == id from dev->forms */
dt_masks_form_t *dt_masks_get_from_id(dt_develop_t *dev, int id);

/** read the forms from the db */
void dt_masks_read_masks_history(dt_develop_t *dev, const int32_t imgid);
/** write the forms into the db */
void dt_masks_write_masks_history_item(const int32_t imgid, const int num, dt_masks_form_t *form);
void dt_masks_free_form(dt_masks_form_t *form);
void dt_masks_cleanup_unused(dt_develop_t *dev);

/** function used to manipulate forms for masks */
void dt_masks_change_form_gui(dt_masks_form_t *newform);
void dt_masks_clear_form_gui(dt_develop_t *dev);
void dt_masks_reset_form_gui(void);
void dt_masks_soft_reset_form_gui(dt_masks_form_gui_t *gui);
void dt_masks_reset_show_masks_icons(void);

int dt_masks_events_mouse_moved(struct dt_iop_module_t *module, double x, double y, double pressure,
                                int which);
int dt_masks_events_button_released(struct dt_iop_module_t *module, double x, double y, int which,
                                    uint32_t state);
int dt_masks_events_button_pressed(struct dt_iop_module_t *module, double x, double y, double pressure,
                                   int which, int type, uint32_t state);
int dt_masks_events_mouse_scrolled(struct dt_iop_module_t *module, double x, double y, int up, uint32_t state, int delta_y);

int dt_masks_events_key_pressed(struct dt_iop_module_t *module, GdkEventKey *event);
/**
 * @brief returns wether a node is a corner or not.
 * A node is a corner if its 2 control handles are at the same position, else it's a curve.
 *
 * @param gpt the GUI points of the mask form
 * @param index the index of the node to test
 * @param nb the number of coord by node
 * @param coord_offset the offset of the coordinates in the points array
 * 
 * @return TRUE if the node is a corner, FALSE it's a curve.
 */
gboolean dt_masks_node_is_cusp(const dt_masks_form_gui_points_t *gpt, const int index);

/**
 * @brief Draw the source for a correction mask.
 *
 * @param cr the cairo context to draw into
 * @param gui the GUI state of the mask form
 * @param index the index of the mask form
 * @param nb the number of coord for that shape
 * @param zoom_scale the current zoom scale of the image
 * @param shape_function the function to draw the shape
 */
void dt_masks_draw_source(cairo_t *cr, dt_masks_form_gui_t *gui, const int index, const int nb, 
  const float zoom_scale, struct dt_masks_gui_center_point_t *center_point, const shape_draw_function_t *draw_shape_func);

void dt_masks_events_post_expose(struct dt_iop_module_t *module, cairo_t *cr, int32_t width, int32_t height,
                                 int32_t pointerx, int32_t pointery);
int dt_masks_events_mouse_leave(struct dt_iop_module_t *module);
int dt_masks_events_mouse_enter(struct dt_iop_module_t *module);

/** functions used to manipulate gui data */
void dt_masks_gui_form_create(dt_masks_form_t *form, dt_masks_form_gui_t *gui, int index,
                              struct dt_iop_module_t *module);

/**
 * @brief Delete a mask shape or node form from the GUI.
 * This function is to be used with a popupmenu "Delete" action in the future.
 * 
 * @param module The module owning the mask
 * @param form The form to delete
 * @param gui The GUI state of the form
 * @param parentid The parent ID of the form
 * @return gboolean TRUE if the form was deleted, FALSE otherwise
 */
gboolean dt_masks_gui_delete(struct dt_iop_module_t *module, dt_masks_form_t *form, dt_masks_form_gui_t *gui, const int parentid);

// Remove a mask
gboolean dt_masks_form_cancel_creation(dt_iop_module_t *module, dt_masks_form_gui_t *gui);


void dt_masks_gui_form_remove(dt_masks_form_t *form, dt_masks_form_gui_t *gui, int index);
void dt_masks_gui_form_test_create(dt_masks_form_t *form, dt_masks_form_gui_t *gui, struct dt_iop_module_t *module);
void dt_masks_gui_form_save_creation(dt_develop_t *dev, struct dt_iop_module_t *module, dt_masks_form_t *form,
                                     dt_masks_form_gui_t *gui);
void dt_masks_group_ungroup(dt_masks_form_t *dest_grp, dt_masks_form_t *grp);
void dt_masks_group_update_name(dt_iop_module_t *module);
dt_masks_form_group_t *dt_masks_group_add_form(dt_masks_form_t *grp, dt_masks_form_t *form);

void dt_masks_iop_value_changed_callback(GtkWidget *widget, struct dt_iop_module_t *module);
dt_masks_edit_mode_t dt_masks_get_edit_mode(struct dt_iop_module_t *module);
void dt_masks_set_edit_mode(struct dt_iop_module_t *module, dt_masks_edit_mode_t value);
void dt_masks_iop_update(struct dt_iop_module_t *module);
void dt_masks_iop_combo_populate(GtkWidget *w, void *module);
void dt_masks_iop_use_same_as(struct dt_iop_module_t *module, struct dt_iop_module_t *src);
uint64_t dt_masks_group_get_hash(uint64_t hash, dt_masks_form_t *form);

void dt_masks_form_remove(struct dt_iop_module_t *module, dt_masks_form_t *grp, dt_masks_form_t *form);
int dt_masks_form_change_opacity(dt_masks_form_t *form, int parentid, int up, const int flow);
void dt_masks_form_move(dt_masks_form_t *grp, int formid, int up);
int dt_masks_form_duplicate(dt_develop_t *dev, int formid);
/* returns a duplicate tof form, including the formid */
dt_masks_form_t *dt_masks_dup_masks_form(const dt_masks_form_t *form);
/* duplicate the list of forms, replace item in the list with form with the same formid */
GList *dt_masks_dup_forms_deep(GList *forms, dt_masks_form_t *form);

/** utils functions */
int dt_masks_point_in_form_exact(const float *pts, int num_pts, const float *points, int points_start, int points_count);

/** allow to select a shape inside an iop */
void dt_masks_select_form(struct dt_iop_module_t *module, dt_masks_form_t *sel);

/** utils for selecting the source of a clone mask while creating it */
void dt_masks_set_source_pos_initial_state(dt_masks_form_gui_t *gui, const uint32_t state, const float pzx,
                                           const float pzy);
void dt_masks_set_source_pos_initial_value(dt_masks_form_gui_t *gui, dt_masks_form_t *form,
                                                   const float pzx, const float pzy);
void dt_masks_calculate_source_pos_value(dt_masks_form_gui_t *gui, const float initial_xpos,
                                         const float initial_ypos, const float xpos, const float ypos, float *px,
                                         float *py, const int adding);
/**
 * @brief Rotate a mask shape around its center.
 * WARNING: gui->delta will be updated with the new position after rotation.
 * 
 * @param dev the develop structure
 * @param anchor the array representing the anchor position (grabbing point) in normalized coordinates.
 * @param center the array representing the origin point of rotation in normalized coordinates
 * @param gui the GUI form structure
 * @return * float : The signed angle to increment.
 */
float dt_masks_rotate_with_anchor(dt_develop_t *dev, const float anchor[2], const float center[2], dt_masks_form_gui_t *gui);

/** Getters and setters for direct GUI interaction */
float dt_masks_form_get_opacity(dt_masks_form_t *form, int parentid);
int dt_masks_form_set_opacity(dt_masks_form_t *form, int parentid, float opacity, dt_masks_increment_t offset, const int flow);

/**
 * @brief Change a numerical property of a mask shape, either by in/de-crementing the current value
 * or setting it in an absolute fashion, then save it to configuration.
 *
 * @param form the shape to change. We will read its type internally
 * @param feature the propertie to change: hardness, size, curvature (for gradients)
 * @param new_value if increment is set to absolute, this is directly the updated value. if increment is offset, the updated value is old_value + new_value. if increment is scale, the updated value is old value * new_value.
 * @param v_min minimum acceptable value of the property for sanitization
 * @param v_max maximum acceptable value of the property for sanitization
 * @param increment the increment type: absolute, offset or scale.
 * @param flow the value of the scroll distance that can be postive or negative.
 */
float dt_masks_get_set_conf_value(dt_masks_form_t *form, char *feature, float new_value, float v_min, float v_max,
                                  dt_masks_increment_t increment, const int flow);

/** detail mask support */
void dt_masks_extend_border(float *const mask, const int width, const int height, const int border);
void dt_masks_blur_9x9_coeff(float *coeffs, const float sigma);
void dt_masks_blur_9x9(float *const src, float *const out, const int width, const int height, const float sigma);
void dt_masks_calc_rawdetail_mask(float *const src, float *const out, float *const tmp, const int width,
                                  const int height, const dt_aligned_pixel_t wb);
void dt_masks_calc_detail_mask(float *const src, float *const out, float *const tmp, const int width, const int height, const float threshold, const gboolean detail);

void dt_group_events_post_expose(cairo_t *cr, float zoom_scale, dt_masks_form_t *form,
                                 dt_masks_form_gui_t *gui);

/** code for dynamic handling of intermediate buffers */
static inline gboolean _dt_masks_dynbuf_growto(dt_masks_dynbuf_t *a, size_t size)
{
  const size_t newsize = dt_round_size_sse(sizeof(float) * size) / sizeof(float);
  float *newbuf = dt_pixelpipe_cache_alloc_align_float_cache(newsize, 0);
  if (!newbuf)
  {
    // not much we can do here except emit an error message
    fprintf(stderr, "critical: out of memory for dynbuf '%s' with size request %zu!\n", a->tag, size);
    return FALSE;
  }
  if (a->buffer)
  {
    memcpy(newbuf, a->buffer, a->size * sizeof(float));
    dt_print(DT_DEBUG_MASKS, "[masks dynbuf '%s'] grows to size %lu (is %p, was %p)\n", a->tag,
             (unsigned long)a->size, newbuf, a->buffer);
    dt_pixelpipe_cache_free_align(a->buffer);
  }
  a->size = newsize;
  a->buffer = newbuf;
  return TRUE;
}

static inline dt_masks_dynbuf_t *dt_masks_dynbuf_init(size_t size, const char *tag)
{
  assert(size > 0);
  dt_masks_dynbuf_t *a = (dt_masks_dynbuf_t *)calloc(1, sizeof(dt_masks_dynbuf_t));

  if(a != NULL)
  {
    g_strlcpy(a->tag, tag, sizeof(a->tag)); //only for debugging purposes
    a->pos = 0;
    if(_dt_masks_dynbuf_growto(a, size))
      dt_print(DT_DEBUG_MASKS, "[masks dynbuf '%s'] with initial size %lu (is %p)\n", a->tag,
               (unsigned long)a->size, a->buffer);
    if(a->buffer == NULL)
    {
      free(a);
      a = NULL;
    }
  }
  return a;
}

static inline void dt_masks_dynbuf_add_2(dt_masks_dynbuf_t *a, float value1, float value2)
{
  assert(a != NULL);
  assert(a->pos <= a->size);
  if(__builtin_expect(a->pos + 2 >= a->size, 0))
  {
    if (a->size == 0 || !_dt_masks_dynbuf_growto(a, 2 * (a->size+1)))
      return;
  }
  a->buffer[a->pos++] = value1;
  a->buffer[a->pos++] = value2;
}

// Return a pointer to N floats past the current end of the dynbuf's contents, marking them as already in use.
// The caller should then fill in the reserved elements using the returned pointer.
static inline float *dt_masks_dynbuf_reserve_n(dt_masks_dynbuf_t *a, const int n)
{
  assert(a != NULL);
  assert(a->pos <= a->size);
  if(__builtin_expect(a->pos + n >= a->size, 0))
  {
    if(a->size == 0) return NULL;
    size_t newsize = a->size;
    while(a->pos + n >= newsize) newsize *= 2;
    if (!_dt_masks_dynbuf_growto(a, newsize))
    {
      return NULL;
    }
  }
  // get the current end of the (possibly reallocated) buffer, then mark the next N items as in-use
  float *reserved = a->buffer + a->pos;
  a->pos += n;
  return reserved;
}

static inline void dt_masks_dynbuf_add_zeros(dt_masks_dynbuf_t *a, const int n)
{
  assert(a != NULL);
  assert(a->pos <= a->size);
  if(__builtin_expect(a->pos + n >= a->size, 0))
  {
    if(a->size == 0) return;
    size_t newsize = a->size;
    while(a->pos + n >= newsize) newsize *= 2;
    if (!_dt_masks_dynbuf_growto(a, newsize))
    {
      return;
    }
  }
  // now that we've ensured a sufficiently large buffer add N zeros to the end of the existing data
  memset(a->buffer + a->pos, 0, n * sizeof(float));
  a->pos += n;
}


static inline float dt_masks_dynbuf_get(dt_masks_dynbuf_t *a, int offset)
{
  assert(a != NULL);
  // offset: must be negative distance relative to end of buffer
  assert(offset < 0);
  assert((long)a->pos + offset >= 0);
  return (a->buffer[a->pos + offset]);
}

static inline void dt_masks_dynbuf_set(dt_masks_dynbuf_t *a, int offset, float value)
{
  assert(a != NULL);
  // offset: must be negative distance relative to end of buffer
  assert(offset < 0);
  assert((long)a->pos + offset >= 0);
  a->buffer[a->pos + offset] = value;
}

static inline float *dt_masks_dynbuf_buffer(dt_masks_dynbuf_t *a)
{
  assert(a != NULL);
  return a->buffer;
}

static inline size_t dt_masks_dynbuf_position(dt_masks_dynbuf_t *a)
{
  assert(a != NULL);
  return a->pos;
}

static inline void dt_masks_dynbuf_reset(dt_masks_dynbuf_t *a)
{
  assert(a != NULL);
  a->pos = 0;
}

static inline float *dt_masks_dynbuf_harvest(dt_masks_dynbuf_t *a)
{
  // take out data buffer and make dynamic buffer obsolete
  if(a == NULL) return NULL;
  float *r = a->buffer;
  a->buffer = NULL;
  a->pos = a->size = 0;
  return r;
}

static inline void dt_masks_dynbuf_free(dt_masks_dynbuf_t *a)
{
  if(a == NULL) return;
  dt_print(DT_DEBUG_MASKS, "[masks dynbuf '%s'] freed (was %p)\n", a->tag,
          a->buffer);
  dt_pixelpipe_cache_free_align(a->buffer);
  free(a);
}

static inline int dt_masks_roundup(int num, int mult)
{
  const int rem = num % mult;

  return (rem == 0) ? num : num + mult - rem;
}

/**
 * @brief Check if a point (px,py) is inside a radius from a center point (cx,cy)
 * 
 * @param px x coord of the point to test
 * @param py y coord of the point to test
 * @param cx center x coord
 * @param cy center y coord
 * @param radius the radius from center
 * @return gboolean TRUE if the point is inside the radius from center, FALSE otherwise
 */
gboolean dt_masks_point_is_within_radius(const float px, const float py,
                                        const float cx, const float cy,
                                        const float radius);

gboolean dt_masks_creation_mode(dt_iop_module_t *module, const dt_masks_type_t type);

/** Contextual menu */

#define menu_item_set_fake_accel(menu_item, keyval, mods)             \
                                                                      \
{                                                                     \
  GtkWidget *child = gtk_bin_get_child(GTK_BIN(menu_item));           \
  if(GTK_IS_ACCEL_LABEL(child))                                       \
    gtk_accel_label_set_accel(GTK_ACCEL_LABEL(child), keyval, mods);  \
}

void _masks_gui_delete_node_callback(GtkWidget *menu, struct dt_masks_form_gui_t *gui);

GdkModifierType dt_masks_get_accel_mods(dt_masks_interaction_t interaction);

GtkWidget *dt_masks_create_menu(dt_masks_form_gui_t *gui, dt_masks_form_t *form);

GtkWidget *masks_gtk_menu_item_new_with_icon(const char *label, GtkWidget *menu,
                                                 void (*activate_callback)(GtkWidget *widget, dt_masks_form_gui_t *gui),
                                                 dt_masks_form_gui_t *gui, dt_masks_menu_icon_t icon);

GtkWidget *masks_gtk_menu_item_new_with_markup(const char *label, GtkWidget *menu,
                                                 void (*activate_callback)(GtkWidget *widget, dt_masks_form_gui_t *gui),
                                                 struct dt_masks_form_gui_t *gui);

#ifdef __cplusplus
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
