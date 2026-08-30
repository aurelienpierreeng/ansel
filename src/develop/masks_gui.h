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

/** @file develop/masks_gui.h
 *
 * @brief The interactive half of the masks subsystem: the editing state
 * (dt_masks_form_gui_t), hit-testing, event dispatchers, drawing helpers, shape buttons,
 * menus and dialogs.
 *
 * @details Cut out of develop/masks.h so that the data model and the rasterisation API no
 * longer feed widgets/draw.h (and GTK behind it) to every consumer that only wants
 * get_mask() -- which was every IOP that includes blend.h. This header is what the
 * darkroom view, libs/masks.c, blend_gui.c and the shape editors include.
 */

#ifndef DT_DEVELOP_MASKS_GUI_H
#define DT_DEVELOP_MASKS_GUI_H

#include "develop/masks.h"
#include "widgets/draw.h"

#include <gtk/gtk.h>

#ifdef __cplusplus
extern "C" {
#endif
struct dt_masks_form_t;
struct dt_masks_form_gui_t;
struct dt_develop_t;
/** structure used to define all the gui points to draw in viewport*/
typedef struct dt_masks_form_gui_points_t
{
  float *points;   // points in absolute coordinates in output image space
  int points_count;
  float *border;   // border points in absolute coordinates in output image space
  int border_count;
  // Self-intersection cuts of `border', out-of-band -- see dt_masks_skip_range_t. NULL/0 for
  // every shape whose border cannot fold over itself (all but polygon today).
  dt_masks_skip_range_t *border_skips;
  int border_skip_count;
  float *source;   // source point in absolute coordinates in output image space
  int source_count;
  gboolean clockwise;
} dt_masks_form_gui_points_t;


/** structure used to display a form */
typedef struct dt_masks_form_gui_t
{
  // Owning develop instance, set once at init. Mask GUI code must use this (or an explicit
  // dev/module argument) instead of the darktable.develop global: shape handlers can run with
  // module == NULL (mask-manager editing), and reaching for the global both couples every
  // shape file to the whole application and hides which develop instance the code mutates.
  struct dt_develop_t *dev;

  dt_masks_type_t type;
  // currently visible form when editing masks (GUI-only; may be a temporary copy)
  dt_masks_form_t *form_visible;
  // points used to draw the form
  GList *points; // list of dt_masks_form_gui_points_t 

  // points used to sample mouse moves
  dt_masks_dynbuf_t *guipoints, *guipoints_payload;
  int guipoints_count;

  // values for mouse positions, etc...

  // Mouse position in absolute coordinates in final image space
  // This is used to map input event handlers to *_post_expose() drawing functions
  // and to record drag & drop starting coordinates.
  float pos[2];

  // Mouse position in normalized coordinates in output image space.
  // This is cached once per top-level event and replaces ad-hoc pzx/pzy recomputation.
  float rel_pos[2];

  // Mouse position in absolute coordinates in raw input image space.
  // This is cached once per top-level event so nested handlers can reuse it.
  float raw_pos[2];

  // delta movement of the mouse in absolute coordinates in final image space
  // This is used to map input event handlers to *_post_expose() drawing functions
  float delta[2];

  // scroll offset
  float scrollx, scrolly;

  // Position of a clone mask's source point (in what coordinates space ?)
  float pos_source[2];

  dt_masks_edit_mode_t edit_mode;

  int node_hovered;           // this is the index of the node, refreshed on mouse_moved when a a group is selected
  int handle_hovered;         // this is the index of the node, refreshed on mouse_moved when a a group is selected
  int seg_hovered;            // this is the index of the segment, refreshed on mouse_moved when a a group is selected
  int handle_border_hovered;  // this is the index of the node, refreshed on mouse_moved when a a group is selected

  gboolean node_selected;     // this is the state of the node referenced by node_hovered
  gboolean handle_selected;   // this is the state of the handle referenced by handle_hovered
  gboolean seg_selected;      // this is the state of the segment referenced by segment_hovered
  gboolean handle_border_selected; // this is the state of the border handle referenced by handle_border_hovered
  int node_selected_idx;      // stable selected node index, distinct from current hover

  gboolean form_selected;
  gboolean border_selected;
  gboolean source_selected;
  gboolean pivot_selected;

  int group_selected;
  
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

  // Throttle GUI rebuilds while dragging to avoid heavy border recomputation.
  double last_rebuild_ts;
  float last_rebuild_pos[2];
  gboolean rebuild_pending;

  // Throttle handle hit-testing when cursor barely moves.
  float last_hit_test_pos[2];

  gboolean creation;
  gboolean creation_closing_form;
  dt_iop_module_t *creation_module;
  // Shape type reused to create the next temporary form in a continuous creation session.
  dt_masks_type_t creation_type;
  // Form ids completed during the active creation session; only these are drawn while creation stays active.
  GList *creation_formids;
  // Last completed form id, selected when the creation session is disabled.
  int creation_last_formid;

  dt_masks_pressure_sensitivity_t pressure_sensitivity;

  // ids
  int formid;
  /* Which composed geometry the cached outlines in ::points were built against
   * (dt_geometry_chain_generation()). Zero means "nothing cached".
   *
   * This used to be the PREVIEW PIPE'S BACKBUFFER HASH, which made it a pixel identity standing in
   * for a geometric one: every republished preview frame invalidated outlines whose inputs had not
   * changed, and while a brush is dragged the preview republishes continuously. That is #1158 --
   * the outlines of every shape in the group rebuilt on every mouse move, and the 1/60 s throttle
   * in dt_masks_gui_form_create_throttled() bypassed by its own force_rebuild clause. */
  uint64_t geometry_generation;
} dt_masks_form_gui_t;

/** Reset a form GUI state and bind it to its owning develop instance (gui->dev). */
void dt_masks_init_form_gui(dt_develop_t *dev, dt_masks_form_gui_t *gui);
dt_masks_form_t *dt_masks_get_visible_form(const struct dt_develop_t *dev);
void dt_masks_set_visible_form(struct dt_develop_t *dev, dt_masks_form_t *form);
void dt_masks_gui_init(struct dt_develop_t *dev);
void dt_masks_gui_cleanup(struct dt_develop_t *dev);
void dt_masks_gui_set_dragging(dt_masks_form_gui_t *gui);
void dt_masks_gui_reset_dragging(dt_masks_form_gui_t *gui);
gboolean dt_masks_gui_is_dragging(const dt_masks_form_gui_t *gui);

// Test wether the form, the border, the source or the pivot is selected 
static inline gboolean dt_masks_gui_was_anything_selected(const dt_masks_form_gui_t *gui)
{
  return gui && (gui->form_selected || gui->border_selected || gui->source_selected || gui->pivot_selected);
}

static inline int dt_masks_gui_selected_node_index(const dt_masks_form_gui_t *gui)
{
  return (gui && gui->node_selected) ? gui->node_selected_idx : -1;
}

static inline int dt_masks_gui_selected_handle_index(const dt_masks_form_gui_t *gui)
{
  return (gui && gui->handle_selected) ? gui->handle_hovered : -1;
}

static inline int dt_masks_gui_selected_handle_border_index(const dt_masks_form_gui_t *gui)
{
  return (gui && gui->handle_border_selected) ? gui->handle_border_hovered : -1;
}

static inline int dt_masks_gui_selected_segment_index(const dt_masks_form_gui_t *gui)
{
  return (gui) ? gui->seg_hovered : -1;
}

static inline gboolean dt_masks_gui_change_affects_selected_node_or_all(const dt_masks_form_gui_t *gui,
                                                                        const int index)
{
  if(IS_NULL_PTR(gui)) return TRUE;

  const int selected_node = dt_masks_gui_selected_node_index(gui);
  return selected_node < 0 || selected_node == index;
}

static inline float dt_masks_get_form_size_from_nodes(const GList *points)
{
  if(IS_NULL_PTR(points) || IS_NULL_PTR(points->data)) return 0.0f;

  // Brush and polygon node payloads both start with `float node[2]`.
  const float *first = (const float *)points->data;
  float min_x = first[0];
  float max_x = first[0];
  float min_y = first[1];
  float max_y = first[1];

  for(const GList *point_node = points; point_node; point_node = g_list_next(point_node))
  {
    const float *node = (const float *)point_node->data;
    if(IS_NULL_PTR(node)) continue;
    min_x = fminf(min_x, node[0]);
    max_x = fmaxf(max_x, node[0]);
    min_y = fminf(min_y, node[1]);
    max_y = fmaxf(max_y, node[1]);
  }

  return fmaxf(max_x - min_x, max_y - min_y);
}

static inline gboolean dt_masks_gui_should_hit_test(dt_masks_form_gui_t *gui)
{
  const float hit_thresh = DT_GUI_MOUSE_EFFECT_RADIUS * 0.5f;
  const float dx = gui->pos[0] - gui->last_hit_test_pos[0];
  const float dy = gui->pos[1] - gui->last_hit_test_pos[1];
  if(gui->last_hit_test_pos[0] < 0.0f || (dx * dx + dy * dy) > (hit_thresh * hit_thresh))
  {
    gui->last_hit_test_pos[0] = gui->pos[0];
    gui->last_hit_test_pos[1] = gui->pos[1];
    return TRUE;
  }
  return FALSE;
}

// High-level mask event dispatchers cache the current cursor in raw absolute coordinates.
// Reuse that cache for current-cursor conversions instead of backtransforming `gui->pos` again.
static inline void dt_masks_gui_cursor_to_raw_norm(dt_develop_t *dev, const dt_masks_form_gui_t *gui, float point[2])
{
  point[0] = gui->raw_pos[0];
  point[1] = gui->raw_pos[1];
  dt_dev_coordinates_raw_abs_to_raw_norm(dev, point, 1);
}

// Reuse the cached absolute output-image cursor and drag delta to derive a raw-normalized point.
static inline void dt_masks_gui_delta_to_raw_norm(dt_develop_t *dev, const dt_masks_form_gui_t *gui, float point[2])
{
  point[0] = gui->pos[0] + gui->delta[0];
  point[1] = gui->pos[1] + gui->delta[1];
  dt_dev_coordinates_image_abs_to_raw_norm(dev, point, 1);
}

static inline void dt_masks_gui_delta_to_image_abs(const dt_masks_form_gui_t *gui, float point[2])
{
  point[0] = gui->pos[0] + gui->delta[0];
  point[1] = gui->pos[1] + gui->delta[1];
}

// Drag branches need the same "cursor + drag delta" converted back to raw space.
// Keep that conversion in one place so all shapes use the same anchor semantics.
static inline void dt_masks_gui_delta_from_raw_anchor(dt_develop_t *dev, const dt_masks_form_gui_t *gui,
                                                      const float anchor[2], float *delta_x, float *delta_y)
{
  float point[2];
  dt_masks_gui_delta_to_raw_norm(dev, gui, point);
  *delta_x = point[0] - anchor[0];
  *delta_y = point[1] - anchor[1];
}

gboolean dt_masks_node_is_cusp(const dt_masks_form_gui_points_t *gpt, const int index);
void dt_masks_gui_form_create(dt_masks_form_t *form, dt_masks_form_gui_t *gui, int index,
                              struct dt_iop_module_t *module);

// Brush and polygon nodes share the same node/control-point edit semantics.
gboolean dt_masks_toggle_bezier_node_type(struct dt_iop_module_t *module,
                                                        struct dt_masks_form_t *mask_form,
                                                        struct dt_masks_form_gui_t *mask_gui,
                                                        const int form_index,
                                                        const struct dt_masks_form_gui_points_t *gui_points,
                                                        const int node_index,
                                                        float node[2], float ctrl1[2], float ctrl2[2],
                                                        dt_masks_points_states_t *state);


gboolean dt_masks_reset_bezier_ctrl_points(struct dt_iop_module_t *module,
                                                         struct dt_masks_form_t *mask_form,
                                                         struct dt_masks_form_gui_t *mask_gui,
                                                         const int form_index,
                                                         const struct dt_masks_form_gui_points_t *gui_points,
                                                         const int node_index,
                                                         dt_masks_points_states_t *state);


// Brush and polygon border handles both constrain the cursor to the node->handle axis.
static inline void dt_masks_project_on_line(const float cursor[2], const float node[2],
                                            const float handle[2], float point[2])
{
  const float dx_line = handle[0] - node[0];
  const float dy_line = handle[1] - node[1];

  if(fabsf(dx_line) < 1e-6f)
  {
    point[0] = node[0];
    point[1] = cursor[1];
  }
  else
  {
    const float a = dy_line / dx_line;
    const float b = node[1] - a * node[0];
    const float denom = a * a + 1.0f;
    const float xproj = (a * cursor[1] + cursor[0] - b * a) / denom;

    point[0] = xproj;
    point[1] = a * xproj + b;
  }
}

// Border handles store normalized raw-space radii, but hit/drag code works in image space.
// Convert both ends once here so all shapes derive border thickness the same way.
static inline float dt_masks_border_from_projected_handle(dt_develop_t *dev, const float node[2],
                                                          const float projected_image_pos[2],
                                                          const float scale_ref)
{
  float projected_raw[2] = { projected_image_pos[0], projected_image_pos[1] };
  float node_raw[2] = { node[0], node[1] };
  dt_dev_coordinates_image_abs_to_raw_abs(dev, projected_raw, 1);
  dt_dev_coordinates_raw_norm_to_raw_abs(dev, node_raw, 1);

  const float delta_x = projected_raw[0] - node_raw[0];
  const float delta_y = projected_raw[1] - node_raw[1];
  return sqrtf(delta_x * delta_x + delta_y * delta_y) / scale_ref;
}

// Circle, ellipse and gradient creation previews all follow the same drawing sequence:
// optional save/restore, draw the shape, then draw the border preview if present.
/** Append a shape's outline to @p cr as the complement of its exclusion list: one cairo sub-path
 * per run between the spans in @p skips, which are where the offset curve doubles back on itself
 * and must not be drawn. Pass NULL and 0 for a shape with nothing to exclude. Implemented in
 * masks/masks_gui.c -- see there for why the excluded spans travel beside the buffer and not
 * inside it. */
void dt_masks_draw_outline_runs(cairo_t *cr, const float *const points, const int first, const int last,
                                const dt_masks_skip_range_t *skips, const int skip_count);

static inline void dt_masks_draw_preview_shape(struct dt_develop_t *dev, cairo_t *cr, const float zoom_scale,
                                               const int num_points,
                                               float *points, const int points_count,
                                               float *border, const int border_count,
                                               const shape_draw_function_t *draw_shape,
                                               const cairo_line_cap_t shape_cap,
                                               const cairo_line_cap_t border_cap,
                                               const gboolean save_restore,
                                               const gboolean source)
{
  if(save_restore) cairo_save(cr);
  if(points && points_count > 0)
    dt_draw_shape_lines(dev, DT_MASKS_NO_DASH, source, cr, num_points, FALSE, zoom_scale, points, points_count,
                        draw_shape, shape_cap, NULL, 0);
  if(border && border_count > 0)
    /* a creation preview is drawn while the shape is still being placed: it has no cuts yet */
    dt_draw_shape_lines(dev, DT_MASKS_DASH_STICK, source, cr, num_points, FALSE, zoom_scale, border, border_count,
                        draw_shape, border_cap, NULL, 0);
  if(save_restore) cairo_restore(cr);
}

// Shared scratch buffers for creation previews. Keeping them grouped makes the shape
// preview helpers return a single value and centralizes cleanup.
typedef struct dt_masks_preview_buffers_t
{
  float *points;
  int points_count;
  float *border;
  int border_count;

  float *source_points;
} dt_masks_preview_buffers_t;

static inline void dt_masks_preview_buffers_cleanup(dt_masks_preview_buffers_t *buffers)
{
  dt_pixelpipe_cache_free_align(buffers->points);
  dt_pixelpipe_cache_free_align(buffers->border);
  dt_pixelpipe_cache_free_align(buffers->source_points);
}

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
} dt_masks_gui_center_point_t;void dt_masks_append_form(dt_develop_t *dev, dt_masks_form_t *form);
void dt_masks_remove_form(dt_develop_t *dev, dt_masks_form_t *form);
void dt_masks_remove_node(struct dt_iop_module_t *module, dt_masks_form_t *form, int parentid,
                          dt_masks_form_gui_t *gui, int index, int node_index);

/** function used to manipulate forms for masks */
void dt_masks_change_form_gui(dt_develop_t *dev, dt_masks_form_t *newform);
void dt_masks_clear_form_gui(dt_develop_t *dev);
void dt_masks_reset_form_gui(dt_develop_t *dev);
void dt_masks_soft_reset_form_gui(dt_masks_form_gui_t *gui);
void dt_masks_reset_show_masks_icons(dt_develop_t *dev);
typedef enum dt_masks_shape_button_index_t
{
  DT_MASKS_SHAPE_INDEX_GRADIENT = 0,
  DT_MASKS_SHAPE_INDEX_POLYGON = 1,
  DT_MASKS_SHAPE_INDEX_ELLIPSE = 2,
  DT_MASKS_SHAPE_INDEX_CIRCLE = 3,
  DT_MASKS_SHAPE_INDEX_BRUSH = 4,
} dt_masks_shape_button_index_t;

typedef enum dt_masks_shape_buttons_flags_t
{
  /** Do not create any shape button. */
  DT_MASKS_SHAPE_BUTTONS_NONE = 0,
  /** Create/register the circle button. */
  DT_MASKS_SHAPE_BUTTONS_CIRCLE = 1 << 0,
  /** Create/register the ellipse button. */
  DT_MASKS_SHAPE_BUTTONS_ELLIPSE = 1 << 1,
  /** Create/register the polygon button. */
  DT_MASKS_SHAPE_BUTTONS_POLYGON = 1 << 2,
  /** Create/register the brush button. */
  DT_MASKS_SHAPE_BUTTONS_BRUSH = 1 << 3,
  /** Create/register the gradient button. */
  DT_MASKS_SHAPE_BUTTONS_GRADIENT = 1 << 4,
  /** Create/register every shape button. */
  DT_MASKS_SHAPE_BUTTONS_ALL = DT_MASKS_SHAPE_BUTTONS_CIRCLE
                               | DT_MASKS_SHAPE_BUTTONS_ELLIPSE
                               | DT_MASKS_SHAPE_BUTTONS_POLYGON
                               | DT_MASKS_SHAPE_BUTTONS_BRUSH
                               | DT_MASKS_SHAPE_BUTTONS_GRADIENT,
} dt_masks_shape_buttons_flags_t;typedef gboolean (*dt_masks_shape_buttons_start_f)(GtkWidget *button, dt_iop_module_t *module,
                                                   dt_masks_type_t type, gpointer user_data);
typedef dt_masks_type_t (*dt_masks_shape_buttons_type_f)(dt_iop_module_t *module, dt_masks_type_t type,
                                                         gpointer user_data);
typedef void (*dt_masks_shape_buttons_notify_f)(GtkWidget *button, dt_iop_module_t *module,
                                                dt_masks_type_t type, gpointer user_data);

typedef struct dt_masks_shape_buttons_config_t
{
  // Owning develop instance. Mandatory: the GTK button callbacks have no other context when
  // creation_module is NULL (the shape-manager toolbar case).
  struct dt_develop_t *dev;
  dt_iop_module_t *owner_module;
  dt_iop_module_t *creation_module;
  GtkWidget **buttons;
  int *types;
  const char *action_section;
  dt_masks_shape_buttons_flags_t flags;
  dt_masks_shape_buttons_flags_t register_flags;
  gboolean local;
  gpointer user_data;
  dt_masks_shape_buttons_start_f can_start;
  dt_masks_shape_buttons_type_f form_type;
  dt_masks_shape_buttons_notify_f started;
  dt_masks_shape_buttons_notify_f exited;
} dt_masks_shape_buttons_config_t;GtkWidget *dt_masks_shape_buttons_create(const dt_masks_shape_buttons_config_t *config);
void dt_masks_shape_buttons_deactivate_all(GtkWidget *active_button);

int dt_masks_events_mouse_moved(dt_develop_t *dev, struct dt_iop_module_t *module, double x, double y, double pressure,
                                int which);
int dt_masks_events_button_released(dt_develop_t *dev, struct dt_iop_module_t *module, double x, double y, int which,
                                    uint32_t state);
int dt_masks_events_button_pressed(dt_develop_t *dev, struct dt_iop_module_t *module, double x, double y, double pressure,
                                   int which, int type, uint32_t state);
int dt_masks_events_mouse_scrolled(dt_develop_t *dev, struct dt_iop_module_t *module, double x, double y, int up, uint32_t state, int delta_y);

int dt_masks_events_key_pressed(dt_develop_t *dev, struct dt_iop_module_t *module, GdkEventKey *event);
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
void dt_masks_draw_path_seg_by_seg(cairo_t *cr, dt_masks_form_gui_t *gui, const int index, const float *points,
                                   const int points_count, const int node_count, const float zoom_scale,
                                   const gboolean round_ends);

/**
 * @brief How the overlay maps image coordinates onto its cairo target.
 *
 * @details The darkroom passes NULL and the mapping comes from the viewport -- zoom, pan, and
 * the GUI's device pixel ratio. None of that exists outside the GUI: dt_gui_get_global() is
 * darktable.gui, which is NULL in ansel-cli and in the test binaries, so the viewport path
 * would dereference it. A headless caller supplies its own mapping instead, which is also what
 * a regression test wants -- a fixed, reproducible transform rather than whatever the window
 * happened to be showing.
 *
 * This exists so the OVERLAY IS DRAWN BY THE SAME CODE in both cases. The alternative -- a
 * second drawing path for diagnostics -- would be a fork that stops agreeing with the GUI
 * exactly when it is needed to explain a GUI problem.
 */
typedef struct dt_masks_overlay_transform_t
{
  double scale;               /**< image pixels -> target pixels */
  double offset_x, offset_y;  /**< target-space translation, applied before the scale */
} dt_masks_overlay_transform_t;

/** @brief Draw the mask overlay with an explicit mapping; @p transform NULL means the viewport's. */
void dt_masks_events_post_expose_with(dt_develop_t *dev, struct dt_iop_module_t *module, cairo_t *cr,
                                      int32_t width, int32_t height, int32_t pointerx, int32_t pointery,
                                      const dt_masks_overlay_transform_t *transform);

void dt_masks_events_post_expose(dt_develop_t *dev, struct dt_iop_module_t *module, cairo_t *cr, int32_t width, int32_t height,
                                 int32_t pointerx, int32_t pointery);
int dt_masks_events_mouse_leave(struct dt_iop_module_t *module);
int dt_masks_events_mouse_enter(struct dt_iop_module_t *module);

/** functions used to manipulate gui data */
void dt_masks_gui_form_create(dt_masks_form_t *form, dt_masks_form_gui_t *gui, int index,
                              struct dt_iop_module_t *module);
gboolean dt_masks_gui_form_create_throttled(dt_masks_form_t *form, dt_masks_form_gui_t *gui, int index,
                                            struct dt_iop_module_t *module, float posx, float posy);

/**
 * @brief remove a mask shape or node form from the GUI.
 * This function is used with a popupmenu "Delete" action.
 * 
 * @param module The module owning the mask
 * @param form The form to remove
 * @param gui The GUI state of the form
 * @param parentid The parent ID of the form
 * @return gboolean TRUE if the form was removed, FALSE otherwise
 */
gboolean dt_masks_gui_remove(struct dt_iop_module_t *module, dt_masks_form_t *form, dt_masks_form_gui_t *gui, const int parentid);

/**
 * @brief If the form to remove is used once, ask to the user if he wants to delete it from the list or just remove and keep for later reuse.
 * 
 * @param module The module owning the mask
 * @param sel The form to remove
 * @param parent_id The parent ID of the form
 * @param mask_gui The GUI state of the form
 * @param form_id The form ID of the form to remove
 * @return gboolean TRUE if the form was removed, FALSE otherwise
 */
gboolean dt_masks_remove_or_delete(struct dt_iop_module_t *module, dt_masks_form_t *sel, int parent_id,
                                    dt_masks_form_gui_t *mask_gui, int form_id);

/**
 * @brief Remove the form from its current group only, keeping it around unused for potential
 * reuse. Never asks for confirmation, since nothing is destroyed.
 *
 * @return gboolean TRUE if the form was removed, FALSE otherwise
 */
gboolean dt_masks_remove_shape_from_group(struct dt_iop_module_t *module, dt_masks_form_t *sel, int parent_id,
                                          dt_masks_form_gui_t *mask_gui, int form_id);

/**
 * @brief Permanently delete the form from every group and from dev->forms. Gated by
 * dt_masks_gui_confirm_permanent_delete() (itself gated by the "ask_before_delete_mask_shape" pref).
 *
 * @return gboolean TRUE if the form was deleted, FALSE otherwise (dialog cancelled)
 */
gboolean dt_masks_delete_shape(struct dt_iop_module_t *module, dt_masks_form_t *sel, int parent_id,
                               dt_masks_form_gui_t *mask_gui, int form_id);

// Remove a mask
gboolean dt_masks_form_exit_creation(dt_iop_module_t *module, dt_masks_form_gui_t *gui);

void dt_masks_gui_form_remove(dt_masks_form_t *form, dt_masks_form_gui_t *gui, int index);
void dt_masks_gui_form_test_create(dt_masks_form_t *form, dt_masks_form_gui_t *gui, struct dt_iop_module_t *module);

/**
 * @brief Save the form creation right after a shape has been finished drawing.
 * 
 * @param dev the develop structure
 * @param module the module owning the mask
 * @param form the form to save
 * @param gui the GUI state of the form
 */
void dt_masks_gui_form_save_creation(dt_develop_t *dev, struct dt_iop_module_t *module, dt_masks_form_t *form,
                                     dt_masks_form_gui_t *gui);
void dt_masks_group_ungroup(dt_develop_t *dev, dt_masks_form_t *dest_grp, dt_masks_form_t *grp);
void dt_masks_group_update_name(dt_iop_module_t *module);
dt_masks_form_group_t *dt_masks_group_add_form(dt_develop_t *dev, dt_masks_form_t *grp, dt_masks_form_t *form);

/** Add a shape to a group with an EXPLICIT combination state and opacity, rather than the
 * defaults dt_masks_group_add_form() applies (SHOW|USE|UNION at the configured opacity).
 *
 * This exists because three call sites wanted their own state and opacity and therefore built the
 * membership row by hand -- malloc, four assignments, g_list_append -- which silently skipped both
 * of the things the real adder does: the self-inclusion guard that stops a group containing
 * itself, and the gravity-centre refresh the group needs before anything hit-tests it.
 *
 * Takes the group by pointer, not by id, because the callers legitimately build into a group that
 * is not in dev->forms yet: it is created, filled, and only then published. Copy-on-write is the
 * caller's business here for the same reason -- an unpublished group is referenced by nobody, and
 * a published one must be touched before this is called, exactly as for dt_masks_group_add_form().
 *
 * @param parentid the row's AUTHORED origin, which is NOT always the group holding it: two
 *        callers assemble a temporary group for an ungroup/regroup round trip and keep each row
 *        pointing at the group it really came from. Pass grp->formid unless you mean otherwise.
 * @return the new row, or NULL if the group is not a group or the add would nest it in itself. */
dt_masks_form_group_t *dt_masks_group_add_form_with_state(dt_develop_t *dev, dt_masks_form_t *grp,
                                                          dt_masks_form_t *form, int parentid,
                                                          dt_masks_state_t state, float opacity);

/** utils functions */

/** allow to select a shape inside an iop */
void dt_masks_select_form(dt_develop_t *dev, struct dt_iop_module_t *module, dt_masks_form_t *sel);

/** utils for selecting the source of a clone mask while creating it */
void dt_masks_set_source_pos_initial_state(dt_masks_form_gui_t *gui, const uint32_t state);
void dt_masks_set_source_pos_initial_value(dt_masks_form_gui_t *gui, dt_masks_form_t *form);
void dt_masks_calculate_source_pos_origin(dt_masks_form_gui_t *gui, const float initial_xpos,
                                         const float initial_ypos, const float xpos, const float ypos, float *px,
                                         float *py, const int adding);
static inline void dt_masks_draw_source_preview(cairo_t *cr, const float zoom_scale, dt_masks_form_gui_t *gui,
                                                const float initial_xpos, const float initial_ypos,
                                                const float xpos, const float ypos, const int adding)
{
  float source_pos[2] = { 0.0f, 0.0f };
  dt_masks_calculate_source_pos_origin(gui, initial_xpos, initial_ypos, xpos, ypos,
                                      &source_pos[0], &source_pos[1], adding);
  dt_draw_cross(cr, zoom_scale, source_pos[0], source_pos[1]);
}
/**
 * @brief Rotate a mask shape around its center.
 * WARNING: gui->delta will be updated with the new position after rotation.
 * 
 * @param dev the develop structure
 * @param anchor the current cursor position in absolute output-image coordinates.
 * @param center the origin point of rotation in absolute output-image coordinates.
 * @param gui the GUI form structure
 * @return * float : The signed angle to increment.
 */
float dt_masks_rotate_with_anchor(dt_develop_t *dev, const float anchor[2], const float center[2], dt_masks_form_gui_t *gui);

/** Getters and setters for direct GUI interaction */
dt_masks_form_group_t *dt_masks_form_group_from_parentid(dt_develop_t *dev, int parentid, int formid);
/**
 * @brief Find the group-membership entry for `formid` inside `group_form`'s own `points` list
 * (not recursive into subgroups) -- the shared primitive behind every "find this shape's row
 * within its parent group" call site.
 * @param out_index if non-NULL, receives the entry's position in the list (-1 if not found).
 */
dt_masks_form_group_t *dt_masks_form_group_find_entry(dt_masks_form_t *group_form, int formid, int *out_index);
/**
 * @brief Find any group that currently references `formid`, searching every top-level group
 * in dev->forms and their nested subgroups (first match wins -- a shape used by more than one
 * module has no single "correct" answer). Meant for UI listings that show a shape without
 * already knowing which group (if any) it belongs to, e.g. a flat "all shapes" row.
 * @param out_parentid if non-NULL, receives the owning group's formid (0 if none found).
 */

int dt_masks_group_index_from_formid(const dt_masks_form_t *group_form, int formid);
dt_masks_form_group_t *dt_masks_form_get_selected_group(const struct dt_masks_form_t *form,
                                                        const struct dt_masks_form_gui_t *gui);

/**
 * Mouse-wheel mapping.
 *
 * Which mask property the wheel edits is the user's choice, not a rule baked into each shape:
 * one row per wheel/modifier combination, each holding the property it acts on (or
 * DT_MASKS_INTERACTION_UNDEF for "does nothing"). The mapping is application-wide -- it is a
 * user habit, not a property of one shape or one module -- and persists in conf.
 *
 * Shapes never read key modifiers: dt_masks_events_mouse_scrolled() resolves the row once and
 * hands the result to the shape's mouse_scrolled callback, which acts on the named property or
 * ignores the event. A shape that does not own the property (rotating a circle) ignores it too.
 */
typedef enum dt_masks_scroll_modifier_t
{
  DT_MASKS_SCROLL_PLAIN = 0,        // wheel alone
  DT_MASKS_SCROLL_SHIFT,            // shift + wheel
  DT_MASKS_SCROLL_PRIMARY,          // ctrl + wheel (cmd on macOS)
  DT_MASKS_SCROLL_PRIMARY_SHIFT,    // ctrl + shift + wheel
  DT_MASKS_SCROLL_MODIFIER_LAST
} dt_masks_scroll_modifier_t;

/** Property mapped to `modifier`, or DT_MASKS_INTERACTION_UNDEF when that row is unmapped. */
dt_masks_interaction_t dt_masks_scroll_mapping_get(dt_masks_scroll_modifier_t modifier);

/** Map `interaction` (DT_MASKS_INTERACTION_UNDEF to unmap) to `modifier`, persisted in conf. */
void dt_masks_scroll_mapping_set(dt_masks_scroll_modifier_t modifier, dt_masks_interaction_t interaction);

/** Untranslated name of a wheel row / of a property, for GUI labels. Never NULL. */
const char *dt_masks_scroll_modifier_name(dt_masks_scroll_modifier_t modifier);
const char *dt_masks_interaction_name(dt_masks_interaction_t interaction);

/**
 * @brief The other word for a property, when a shape family spells it its own way.
 *
 * A gradient stores its fade extent in the SIZE slot and its curvature in the FADING one --
 * same property of the interaction API, different vocabulary in front of the user, which is
 * also why the context menu renames those two sliders for gradients. Any UI naming a property
 * generically must show both words, or the gradient's own vocabulary has no visible home.
 *
 * @return the untranslated second name, or NULL when the property has only one.
 */
const char *dt_masks_interaction_alias_name(dt_masks_interaction_t interaction);

/**
 * @brief Resolve a wheel event's key state into the property it edits.
 * @return DT_MASKS_INTERACTION_UNDEF when this combination is unmapped, or when the key state
 *         is not one of the four the mapping covers (a stray Alt, for instance).
 */
dt_masks_interaction_t dt_masks_scroll_get_interaction(uint32_t key_state);

/** Returns TRUE if anything in the mask is selected at all, regardless of what it is. */
gboolean dt_masks_is_anything_selected(const dt_masks_form_gui_t *mask_gui);

/** Returns TRUE if anything in the mask is hovered at all, regardless of what it is. */
gboolean dt_masks_is_anything_hovered(const dt_masks_form_gui_t *mask_gui);

/**
 * @brief Return the currently selected group entry, resolving to the live form group when the GUI
 *        is operating on a temporary copy (for example the visible group created for editing).
 *
 * The selection is taken from `gui->group_selected`. If the selected entry belongs to a temporary
 * group (non-zero parentid), the function resolves and returns the corresponding entry from the
 * real group in `dev->forms`.
 */
dt_masks_form_group_t *dt_masks_form_get_selected_group_live(const struct dt_masks_form_t *form,
                                                             const struct dt_masks_form_gui_t *gui);
float dt_masks_form_get_interaction_value(dt_develop_t *dev, int group_id, int formid,
                                          dt_masks_interaction_t interaction);
gboolean dt_masks_form_get_gravity_center(dt_develop_t *dev, const struct dt_masks_form_t *form, float center[2], float *area);
void dt_masks_form_update_gravity_center(dt_develop_t *dev, struct dt_masks_form_t *form);
/** Marks gravity_center/area stale instead of recomputing them right away. Use for bulk
 * paths (loading history, undo/redo) that swap in many forms at once; the one GUI
 * hit-testing read site recomputes lazily on first actual use. */
void dt_masks_form_invalidate_gravity_center(struct dt_masks_form_t *form);
int dt_masks_center_view_on_form(struct dt_develop_t *dev, const struct dt_masks_form_t *form);
float dt_masks_form_set_interaction_value(dt_develop_t *dev, int group_id, int formid,
                                          dt_masks_interaction_t interaction,
                                          float value, dt_masks_increment_t increment, int flow,
                                          struct dt_masks_form_gui_t *gui, struct dt_iop_module_t *module);

/**
 * @brief Change a numerical property of a mask shape, either by in/de-crementing the current value
 * or setting it in an absolute fashion, then save it to configuration.
 *
 * @param form the shape to change. We will read its type internally
 * @param feature the propertie to change: fading, size, curvature (for gradients)
 * @param new_value if increment is set to absolute, this is directly the updated value. if increment is offset, the updated value is old_value + new_value. if increment is scale, the updated value is old value * new_value.
 * @param v_min minimum acceptable value of the property for sanitization
 * @param v_max maximum acceptable value of the property for sanitization
 * @param increment the increment type: absolute, offset or scale.
 * @param flow the value of the scroll distance that can be postive or negative.
 */
float dt_masks_get_set_conf_value(dt_masks_form_t *form, char *feature, float new_value, float v_min, float v_max,
                                  dt_masks_increment_t increment, const int flow);
/**
 * @brief Update a mask configuration value and emit a toast message.
 *
 * This is a convenience wrapper around dt_masks_get_set_conf_value() that keeps UI
 * feedback consistent across mask types.
 */
float dt_masks_get_set_conf_value_with_toast(dt_masks_form_t *form, const char *feature, float amount,
                                             float v_min, float v_max, dt_masks_increment_t increment, int flow,
                                             const char *toast_fmt, float toast_scale);

/**
 * @brief Apply a scroll increment to a scalar value.
 */
float dt_masks_apply_increment(float current, float amount, dt_masks_increment_t increment, int flow);

/**
 * @brief Apply a scroll increment using precomputed scale/offset factors.
 */
float dt_masks_apply_increment_precomputed(float current, float amount, float scale_amount, float offset_amount,
                                            dt_masks_increment_t increment);

void dt_group_events_post_expose(cairo_t *cr, float zoom_scale, dt_masks_form_t *form,
                                 dt_masks_form_gui_t *gui);

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

/**
 * @brief Shape-specific callback to fetch a node's border handle in GUI space.
 *
 * @return TRUE if the handle is valid and written to (handle_x, handle_y).
 */
typedef gboolean (*dt_masks_border_handle_fn)(const dt_masks_form_gui_points_t *gui_points, int node_count,
                                              int node_index, float *handle_x, float *handle_y, void *user_data);
/**
 * @brief Shape-specific callback to fetch a node's curve handle in GUI space.
 *
 * The handle is only queried for non-cusp nodes; implementations may assume that.
 */
typedef void (*dt_masks_curve_handle_fn)(const dt_masks_form_gui_points_t *gui_points, int node_index,
                                         float *handle_x, float *handle_y, void *user_data);
/**
 * @brief Shape-specific callback to fetch a node's position in GUI space.
 *
 * When NULL, the common helper assumes Bezier-like layout at points[k*6+2].
 */
typedef void (*dt_masks_node_position_fn)(const dt_masks_form_gui_points_t *gui_points, int node_index,
                                          float *node_x, float *node_y, void *user_data);
/**
 * @brief Shape-specific callback for inside/border/segment hit testing.
 *
 * This mirrors the per-shape *_get_distance() APIs and returns the same outputs.
 * The dist output is a squared distance in absolute output-image coordinates.
 */
typedef void (*dt_masks_distance_fn)(float pointer_x, float pointer_y, float cursor_radius,
                                     dt_masks_form_gui_t *mask_gui, int form_index, int node_count,
                                     int *inside, int *inside_border, int *near_handle, int *inside_source, float *dist,
                                     void *user_data);
/**
 * @brief Optional hook to customize selection flags after inside/border/source resolution.
 */
typedef void (*dt_masks_post_select_fn)(dt_masks_form_gui_t *mask_gui, int inside, int inside_border,
                                        int inside_source, void *user_data);

/**
 * @brief Shared selection logic for node/handle/segment hit testing.
 *
 * The shape-specific callbacks supply handles and distance tests while this function
 * performs common selection bookkeeping on dt_masks_form_gui_t.
 *
 * The cached cursor in `mask_gui->pos` is authoritative for hit testing.
 */
int dt_masks_find_closest_handle_common(dt_masks_form_t *mask_form, dt_masks_form_gui_t *mask_gui,
                                        int form_index, int node_count_override,
                                        dt_masks_border_handle_fn border_handle_cb,
                                        dt_masks_curve_handle_fn curve_handle_cb,
                                        dt_masks_node_position_fn node_position_cb,
                                        dt_masks_distance_fn distance_cb,
                                        dt_masks_post_select_fn post_select_cb,
                                        void *user_data);

void dt_masks_creation_mode_quit(dt_masks_form_gui_t *gui);
gboolean dt_masks_creation_mode_enter(dt_develop_t *dev, dt_iop_module_t *module, const dt_masks_type_t type);

/** Contextual menu */

#define menu_item_set_fake_accel(menu_item, keyval, mods)             \
                                                                      \
{                                                                     \
  GtkWidget *child = gtk_bin_get_child(GTK_BIN(menu_item));           \
  if(GTK_IS_ACCEL_LABEL(child))                                       \
    gtk_accel_label_set_accel(GTK_ACCEL_LABEL(child), keyval, mods);  \
}

void _masks_gui_delete_node_callback(GtkWidget *menu, gpointer user_data);

GdkModifierType dt_masks_get_accel_mods(dt_masks_interaction_t interaction);

GtkWidget *dt_masks_create_menu(dt_masks_form_gui_t *gui, dt_masks_form_t *form, const dt_masks_form_group_t *fpt,
                                const float pzx, const float pzy);

/**
 * @brief Append a bauhaus-slider menu item to a mask context menu, bound to one shape
 * interaction (size, fading, rotation, opacity). Shared by the darkroom
 * canvas context menu (dt_masks_create_menu) and the blend module's own shape-list
 * context menus, so both stay in sync.
 */
GtkWidget *dt_masks_gui_add_interaction_slider(GtkWidget *menu, const char *label, dt_develop_t *dev,
                                               int group_id, int formid,
                                               dt_masks_interaction_t interaction, dt_masks_increment_t increment,
                                               float min, float max, float step, float value, int digits,
                                               const char *format, float factor,
                                               dt_masks_form_gui_t *gui, struct dt_iop_module_t *module);

/**
 * @brief Append the full set of shape-parameter sliders (size/fading/rotation or
 * curvature/fade/rotation depending on shape type, plus opacity) for `form`/`op_form` to
 * `menu`. `op_form` is the group-membership entry the sliders read/write (see
 * dt_masks_form_group_t) and must already be a live, COW-safe pointer into the owning
 * group's `points` list.
 */
void dt_masks_gui_populate_interaction_sliders(GtkWidget *menu, dt_develop_t *dev, dt_masks_form_t *form,
                                               int group_id,
                                               dt_masks_form_gui_t *gui, struct dt_iop_module_t *module);

int dt_masks_gui_confirm_delete_form_dialog(const char *form_name);

/**
 * @brief Ask the user to confirm a permanent shape deletion, gated by the
 * "ask_before_delete_mask_shape" pref. Shows a "Always ask" checkbox that writes back to the
 * pref regardless of the response. Returns TRUE immediately without showing anything when the
 * pref is off.
 */
gboolean dt_masks_gui_confirm_permanent_delete(const char *form_name);

void dt_masks_iop_value_changed_callback(GtkWidget *widget, struct dt_iop_module_t *module);
void dt_masks_iop_combo_populate(GtkWidget *w, void *module);

#ifdef __cplusplus
}
#endif

#endif // DT_DEVELOP_MASKS_GUI_H
