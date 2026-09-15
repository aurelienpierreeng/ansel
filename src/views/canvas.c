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

/**
 * @file views/canvas.c
 * @brief The Canvas atelier: an infinite plane to lay images and notes out on.
 *
 * @details The view owns one `dt_canvas_t` (see canvas/canvas.h) and everything about
 * showing and editing it: the viewport, the selection, the drag gestures, the drop from
 * the filmstrip, the context menus, the dialogs, the undo records and the renders it asks
 * canvas/canvas_render.h for. The document outlives a view switch -- leaving the atelier
 * keeps it in memory, and a dirty untitled canvas is written to a recovery file at exit
 * and read back at the next start -- so nothing typed here is lost by wandering off to
 * the darkroom. Canvas-level actions are exposed through `proxy.canvas` for the toolbar
 * (libs/tools/canvas_toolbar.c) and bound to the `canvas` accelerator group.
 *
 * Coordinates: the centre widget is in logical pixels, the document in canvas units;
 * `_to_canvas()` / `_to_screen()` are the two conversions and everything goes through them.
 */

#include "canvas/canvas.h"
#include "canvas/canvas_actions.h"
#include "canvas/canvas_paint.h"
#include "canvas/canvas_props.h"
#include "canvas/canvas_export.h"
#include "canvas/canvas_handles.h"
#include "canvas/canvas_place.h"
#include "canvas/canvas_place_shapes.h"
#include "canvas/canvas_render.h"
#include "colorprofiles/colorspaces.h"
#include "common/conf.h"
#include "common/file_location.h"
#include "common/image.h"
#include "common/module_versioning.h"
#include "common/pdf.h"
#include "common/selection.h"
#include "common/undo.h"
#include "control/control.h"
#include "control/signal.h"
#include "control/user_message.h"
#include "gui/actions/menu.h"
#include "gui/application.h"
#include "gui/drag_and_drop.h"
#include "gui/dtgtk/thumbtable.h"
#include "gui/window_manager.h"
#include "system/macros.h"
#include "system/mem_alloc.h"
#include "views/canvas_props_gtk.h"
#include "views/view.h"
#include "views/view_api.h"
#include "widgets/accelerators.h"
#include "widgets/dialog.h"
#include "widgets/gdkkeys.h"
#include "widgets/widget_settings.h"

#include <gdk/gdkkeysyms.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>

DT_MODULE(1)

#define CANVAS_ZOOM_MIN 0.02
#define CANVAS_ZOOM_MAX 8.0
#define CANVAS_ZOOM_STEP 1.15
#define CANVAS_DRAG_THRESHOLD_PIXELS 3.0
#define CANVAS_DROP_STAGGER 40.0
#define CANVAS_FIT_MARGIN 0.9
#define CANVAS_BADGE_PIXELS 7.0
#define CANVAS_NEIGHBOUR_SNAP_PIXELS 8.0
#define CANVAS_RECOVERY_FILE "canvas-recovery" DT_CANVAS_FILE_EXTENSION
#define CANVAS_FLOWER_RADIUS 46.0
#define CANVAS_FLOWER_INNER_RADIUS 25.0
#define CANVAS_FLOWER_CENTER_RADIUS 9.0
#define CANVAS_FLOWER_MARGIN 22.0
#define CANVAS_FLOWER_PAN_FRACTION 0.25
#define CANVAS_FLOWER_ZOOM_STEP 1.5
// The floating properties: the gap they keep around what they avoid and from the view's edges;
// the pointer they never open under; the other frames they had better not cover; the least card a
// view too short for the whole one still shows. There is no most: the card is shown whole.
#define CANVAS_PROPS_AIR_PIXELS 6.0
#define CANVAS_PROPS_POINTER_PIXELS 12.0
#define CANVAS_PROPS_SOFT_FRAMES 64
#define CANVAS_PROPS_CARD_MIN_PIXELS 120.0
/** Where the canvas shortcuts live in the accel map, and the one a menu names by its binding. */
#define CANVAS_ACCEL_SCOPE N_("Canvas/Actions")
#define CANVAS_ACCEL_PROPERTIES N_("Show the properties of the selected object")

/** The parts of the navigation flower, floating at the bottom right of the view. */
typedef enum dt_canvas_flower_part_t
{
  DT_CANVAS_FLOWER_NONE = -1,
  DT_CANVAS_FLOWER_PAN_UP = 0,
  DT_CANVAS_FLOWER_PAN_RIGHT,
  DT_CANVAS_FLOWER_PAN_DOWN,
  DT_CANVAS_FLOWER_PAN_LEFT,
  DT_CANVAS_FLOWER_ZOOM_IN,
  DT_CANVAS_FLOWER_ZOOM_OUT,
  DT_CANVAS_FLOWER_FIT,
} dt_canvas_flower_part_t;

typedef enum dt_canvas_drag_t
{
  DT_CANVAS_DRAG_NONE = 0,
  DT_CANVAS_DRAG_PAN,
  DT_CANVAS_DRAG_MOVE,
  DT_CANVAS_DRAG_SCALE,
  DT_CANVAS_DRAG_ROTATE,
  DT_CANVAS_DRAG_RUBBERBAND,
  DT_CANVAS_DRAG_VIA,
  DT_CANVAS_DRAG_HANDLE_FROM,   ///< the start's tangent handle: its length along the normal
  DT_CANVAS_DRAG_HANDLE_TO,     ///< the end's
  DT_CANVAS_DRAG_HANDLE_VIA,    ///< the waypoint's tangent handle, either side
  DT_CANVAS_DRAG_MASK_CENTER,   ///< a cutout's centre, or the gradient's anchor
  DT_CANVAS_DRAG_MASK_RADIUS_X, ///< the circle's radius, the ellipse's first radius and its rotation
  DT_CANVAS_DRAG_MASK_RADIUS_Y, ///< the ellipse's second radius
  DT_CANVAS_DRAG_MASK_REACH,    ///< the gradient's extent and rotation
  DT_CANVAS_DRAG_MASK_NODE,     ///< one polygon node, `mask_handle`
  DT_CANVAS_DRAG_MASK_FEATHER,  ///< the circle's or the ellipse's fall-off, on its dashed ring
  DT_CANVAS_DRAG_MASK_NODE_BORDER,   ///< one polygon node's own fall-off radius
  DT_CANVAS_DRAG_MASK_NODE_CTRL_IN,  ///< its control point on the previous node's side
  DT_CANVAS_DRAG_MASK_NODE_CTRL_OUT, ///< and on the next node's side
} dt_canvas_drag_t;

typedef struct dt_canvas_view_t
{
  dt_canvas_t *canvas;
  uint64_t token;                       ///< identifies the open document to render callbacks
  dt_canvas_surface_cache_t *cache;

  // viewport
  double zoom;                          ///< screen pixels per canvas unit
  double center_x;                      ///< canvas point at the centre of the view
  double center_y;
  int width;                            ///< centre allocation, logical pixels
  int height;

  // selection and gestures
  GArray *selection;                    ///< uint32_t object ids
  dt_canvas_drag_t drag;
  double press_x;                       ///< press position, canvas units
  double press_y;
  double press_screen_x;                ///< press position, screen pixels
  double press_screen_y;
  double last_x;                        ///< last motion position, canvas units
  double last_y;
  double pointer_x;                     ///< current pointer, canvas units
  double pointer_y;
  gboolean drag_moved;
  dt_canvas_t *drag_snapshot;           ///< the document before the gesture, for undo
  int scale_corner;                     ///< 0..3, the corner being dragged
  int handle_sign;                      ///< +1 or -1: which side of the waypoint's tangent is dragged
  int mask_node_hover;                  ///< the polygon node whose own handles are showing, -1 for none
  double gesture_start_rotation;
  double gesture_start_angle;
  gboolean connecting;                  ///< connector-drawing mode, armed from the toolbar
  uint32_t connect_from;                ///< the source frame once its anchor was clicked, 0 before
  uint32_t connect_from_anchor;         ///< dt_canvas_anchor_t chosen on the source
  uint32_t anchor_hover_id;             ///< frame whose anchors are shown, 0 when none
  uint32_t anchor_hover;                ///< dt_canvas_anchor_t under the pointer, AUTO when none
  uint32_t hover;                       ///< object under the pointer, 0 when none
  gboolean pointer_inside;
  dt_canvas_flower_part_t flower_hover;  ///< the flower part under the pointer

  gboolean dnd_connected;

  dt_canvas_t *menu_snapshot;           ///< the document when a slider menu opened, for one undo record on close
  gboolean interacting;                 ///< a gesture is in flight: frames are composited at half the resolution
  guint interaction_timeout;            ///< the full-quality repaint once the gesture pauses
  // same-size guides, shown while a resize snaps to a neighbour's size
  gboolean guide_width_valid;
  dt_canvas_rect_t guide_width;
  gboolean guide_height_valid;
  dt_canvas_rect_t guide_height;
  gboolean mask_editing;                ///< the cutout's handles are shown and take the pointer
  int mask_handle;                      ///< the polygon node being dragged
  // The properties are OPEN for one object or for none, and only a double click, the I key or
  // the context menu opens them; everything else can refill, move, hide or close them.
  uint32_t props_id;                    ///< the object whose properties are open, 0 when closed
  double props_anchor_u;                ///< a frame: where they were asked for, in its unit square,
  double props_anchor_v;                ///< so the place follows the frame through moves and turns
  double props_anchor_t;                ///< a connector: that place as a fraction of its route's length
  gboolean props_suspended;             ///< open, but hidden while a gesture moves things under them
  guint props_request_idle;             ///< the opening or the content action a gesture deferred
  dt_canvas_click_sequence_t click_sequence; ///< the presses the double-click drill rule reads
  // The properties themselves: a strip that can grow a card, one overlay child of the centre, built
  // on entering the atelier. The widget holds nothing of the document's; the view refills it.
  dt_canvas_props_gtk_t *props;         ///< NULL outside the atelier
  gulong props_position_handler;        ///< the overlay's get-child-position hook
  guint props_idle;                     ///< pending refill and placement, scheduled off the draw path
  gboolean props_filled;                ///< the widget shows `props_filled_id` as the three below were
  uint32_t props_filled_id;
  uint64_t props_filled_generation;
  gboolean props_filled_editing;
  gboolean props_refill_owed;           ///< a render's status changed, which moves no generation
  gboolean props_card_open;             ///< the user asked for the card; never remembered past a showing
  double props_last_card_height[DT_CANVAS_OBJECT_SVG + 1]; ///< per kind, the card last shown: room kept for it
  dt_canvas_place_t props_card_told;    ///< the card as the widget was last told to show it
  gboolean props_card_told_valid;
  // An edit in the properties, from its first step to its commit: one snapshot, one undo step.
  dt_canvas_t *props_snapshot;          ///< the document before the edit, NULL when none is open
  uint32_t props_session_id;            ///< the object the edit is for
  uint32_t props_effects;               ///< DT_CANVAS_EFFECT_* the edit's steps owe so far
  // Where the properties are on screen. canvas_place.c decides, and never while the user is in
  // them: under the pointer, typing into them or in the middle of an edit, a placement waits until
  // they are left.
  dt_canvas_place_t props_place;        ///< the last placement
  gboolean props_placed;                ///< `props_place` is where the showing properties went: the next placement keeps it
  gboolean props_toasted;               ///< this showing already said there is no room for them
  gboolean props_pointer_inside;        ///< the pointer is over them
  gboolean props_live;                  ///< an edit in them previews and has not committed yet
  gboolean props_place_pending;         ///< a placement waited for the user to leave them
  gulong props_focus_handler;           ///< the main window's set-focus hook
  GArray *props_shapes;                 ///< dt_canvas_place_shape_t: what the last placement kept clear, for the debug overlay
  dt_cursor_t cursor;                   ///< the shape last queued, to queue only on change
} dt_canvas_view_t;

typedef struct dt_canvas_undo_t
{
  dt_canvas_t *before;
  dt_canvas_t *after;
} dt_canvas_undo_t;

/* forward declarations */
static void _proxy_action(dt_view_t *self, int action);
static const dt_canvas_t *_proxy_document(dt_view_t *self);
static void _proxy_set_grid_size(dt_view_t *self, float size);
static void _proxy_set_border(dt_view_t *self, const float *rgba, float width);
static gboolean _proxy_is_connecting(dt_view_t *self);
static void _props_sync(dt_view_t *self);
static void _props_forget_pending(dt_view_t *self);
static void _props_close(dt_view_t *self);
static void _props_suspend(dt_view_t *self);
static void _props_open(dt_view_t *self, uint32_t object_id, gboolean has_point, double x, double y);
static void _connect_mode_set(dt_view_t *self, gboolean on);
static gboolean _connector_handles(const dt_canvas_view_t *view, const dt_canvas_object_t *connector,
                                   dt_canvas_route_t *route);
static void _paint_tangent_handle(cairo_t *cr, const dt_canvas_view_t *view, const double anchor_x,
                                  const double anchor_y, const double handle_x, const double handle_y);
static void _render_done(uint32_t object_id, uint64_t token, GBytes *jpeg, int32_t pixel_width, int32_t pixel_height,
                         uint64_t history_hash, gpointer user_data);
static gboolean _start_map_render(dt_view_t *self, dt_canvas_object_t *object);
static dt_canvas_drag_t _mask_handle_at(const dt_canvas_view_t *view, const dt_canvas_object_t *object,
                                        const double x, const double y, int *index);
static int _mask_segment_at(const dt_canvas_view_t *view, const dt_canvas_object_t *object, const double x,
                            const double y);
static int _mask_node_at(const dt_canvas_view_t *view, const dt_canvas_object_t *object, const double x,
                         const double y);
static void _flower_center(const dt_canvas_view_t *view, double *center_x, double *center_y);

/* --- module identity ---------------------------------------------------------- */

const char *name(const dt_view_t *self)
{
  return _("Canvas");
}

uint32_t view(const dt_view_t *self)
{
  return DT_VIEW_CANVAS;
}

uint32_t flags(void)
{
  return VIEW_FLAGS_PAINTS_WHOLE_AREA;
}

/* --- interaction quality ----------------------------------------------------------- */

#define CANVAS_INTERACTION_IDLE_MS 180

static gboolean _interaction_settled(gpointer data)
{
  dt_view_t *self = (dt_view_t *)data;
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  view->interaction_timeout = 0;
  view->interacting = FALSE;
  // The gesture has paused: properties it hid come back where the objects now are.
  _props_sync(self);
  dt_control_queue_redraw_center();
  return G_SOURCE_REMOVE;
}

/**
 * A gesture moved the view or an object: the frames until it pauses are composited at half
 * the resolution and scaled up, then one full frame follows. Every motion re-arms the pause.
 * The properties are hidden for as long: they would otherwise sit over whatever the wheel or
 * the flower brought under them, and moving them per motion re-allocates the overlay.
 */
static void _interaction_touch(dt_view_t *self)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  view->interacting = TRUE;
  _props_suspend(self);
  if(view->interaction_timeout != 0) g_source_remove(view->interaction_timeout);
  view->interaction_timeout = g_timeout_add(CANVAS_INTERACTION_IDLE_MS, _interaction_settled, self);
}

/* --- coordinates --------------------------------------------------------------- */

static void _to_canvas(const dt_canvas_view_t *view, const double screen_x, const double screen_y, double *canvas_x,
                       double *canvas_y)
{
  *canvas_x = (screen_x - view->width * 0.5) / view->zoom + view->center_x;
  *canvas_y = (screen_y - view->height * 0.5) / view->zoom + view->center_y;
}

static dt_canvas_rect_t _visible_rect(const dt_canvas_view_t *view)
{
  dt_canvas_rect_t rect;
  _to_canvas(view, 0.0, 0.0, &rect.x, &rect.y);
  rect.width = view->width / view->zoom;
  rect.height = view->height / view->zoom;
  return rect;
}

static void _set_zoom(dt_canvas_view_t *view, const double zoom)
{
  view->zoom = CLAMP(zoom, CANVAS_ZOOM_MIN, CANVAS_ZOOM_MAX);
}

static void _zoom_around(dt_canvas_view_t *view, const double screen_x, const double screen_y, const double factor)
{
  double anchor_x = 0.0;
  double anchor_y = 0.0;
  _to_canvas(view, screen_x, screen_y, &anchor_x, &anchor_y);
  _set_zoom(view, view->zoom * factor);
  // Keep the canvas point under the pointer where it is.
  view->center_x = anchor_x - (screen_x - view->width * 0.5) / view->zoom;
  view->center_y = anchor_y - (screen_y - view->height * 0.5) / view->zoom;
}

static void _zoom_fit(dt_canvas_view_t *view)
{
  const dt_canvas_rect_t bounds = dt_canvas_bounds(view->canvas);
  if(bounds.width <= 0.0 || bounds.height <= 0.0 || view->width <= 0 || view->height <= 0)
  {
    view->center_x = 0.0;
    view->center_y = 0.0;
    _set_zoom(view, 1.0);
    return;
  }
  const double fit = fmin(view->width / bounds.width, view->height / bounds.height) * CANVAS_FIT_MARGIN;
  _set_zoom(view, fit);
  view->center_x = bounds.x + bounds.width * 0.5;
  view->center_y = bounds.y + bounds.height * 0.5;
}

static void _store_viewport(dt_canvas_view_t *view)
{
  if(IS_NULL_PTR(view->canvas)) return;
  view->canvas->view_zoom = view->zoom;
  view->canvas->view_x = view->center_x;
  view->canvas->view_y = view->center_y;
}

static void _restore_viewport(dt_canvas_view_t *view)
{
  if(IS_NULL_PTR(view->canvas)) return;
  _set_zoom(view, view->canvas->view_zoom > 0.0 ? view->canvas->view_zoom : 1.0);
  view->center_x = view->canvas->view_x;
  view->center_y = view->canvas->view_y;
}

/* --- selection --------------------------------------------------------------------- */

static gboolean _is_selected(const dt_canvas_view_t *view, const uint32_t id)
{
  for(guint idx = 0; idx < view->selection->len; idx++)
  {
    if(g_array_index(view->selection, uint32_t, idx) == id) return TRUE;
  }
  return FALSE;
}

static void _select_only(dt_canvas_view_t *view, const uint32_t id)
{
  g_array_set_size(view->selection, 0);
  if(id != 0) g_array_append_val(view->selection, id);
}

static void _select_toggle(dt_canvas_view_t *view, const uint32_t id)
{
  for(guint idx = 0; idx < view->selection->len; idx++)
  {
    if(g_array_index(view->selection, uint32_t, idx) == id)
    {
      g_array_remove_index(view->selection, idx);
      return;
    }
  }
  g_array_append_val(view->selection, id);
}

/** Drop selected ids that no longer name an object. */
static void _selection_prune(dt_canvas_view_t *view)
{
  for(guint idx = view->selection->len; idx > 0; idx--)
  {
    const uint32_t id = g_array_index(view->selection, uint32_t, idx - 1);
    if(IS_NULL_PTR(dt_canvas_find_object(view->canvas, id))) g_array_remove_index(view->selection, idx - 1);
  }
}

static dt_canvas_object_t *_single_selected(const dt_canvas_view_t *view)
{
  if(view->selection->len != 1) return NULL;
  return dt_canvas_find_object(view->canvas, g_array_index(view->selection, uint32_t, 0));
}

/* --- undo -------------------------------------------------------------------------- */

static void _undo_free(gpointer data)
{
  dt_canvas_undo_t *record = (dt_canvas_undo_t *)data;
  if(IS_NULL_PTR(record)) return;
  dt_canvas_free(record->before);
  dt_canvas_free(record->after);
  dt_free(record);
}

static void _undo_pop(gpointer user_data, dt_undo_type_t type, dt_undo_data_t item, dt_undo_action_t action,
                      GList **imgs)
{
  dt_view_t *self = (dt_view_t *)user_data;
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  dt_canvas_undo_t *record = (dt_canvas_undo_t *)item;
  if(IS_NULL_PTR(view) || IS_NULL_PTR(view->canvas) || IS_NULL_PTR(record)) return;
  // The canvas's own undo commits the properties before it gets here; the Edit menu's does not.
  _props_forget_pending(self);
  dt_canvas_restore(view->canvas, action == DT_ACTION_UNDO ? record->before : record->after);
  _selection_prune(view);
  // An undo that took the object away takes its properties with it.
  _props_sync(self);
  dt_control_queue_redraw_center();
  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_CANVAS_CHANGED);
}

/** Record an undo step from `before` (consumed) to the document's current state. */
static void _record_undo(dt_view_t *self, dt_canvas_t *before)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(before)) return;
  dt_canvas_undo_t *record = g_new0(dt_canvas_undo_t, 1);
  record->before = before;
  record->after = dt_canvas_copy(view->canvas);
  dt_undo_record(dt_undo_get_global(), self, DT_UNDO_CANVAS, record, _undo_pop, _undo_free);
}

/** Snapshot the document before an edit. Pair with _record_undo() once the edit is done. */
static dt_canvas_t *_begin_edit(dt_canvas_view_t *view)
{
  return dt_canvas_copy(view->canvas);
}

/* --- document lifecycle ------------------------------------------------------------- */

static void _canvas_apply_conf_defaults(dt_canvas_t *canvas)
{
  canvas->grid_size = (float)dt_conf_get_int("canvas/grid_size");
  canvas->grid_flags = (dt_conf_get_bool("canvas/grid_visible") ? DT_CANVAS_GRID_VISIBLE : 0)
                       | ((uint32_t)dt_conf_get_int("canvas/snap_mode") & DT_CANVAS_SNAP_ALL);
  canvas->border_width = dt_conf_get_float("canvas/border_width");
  canvas->padding = dt_conf_get_float("canvas/padding");
  canvas->background_style = (uint32_t)CLAMP(dt_conf_get_int("canvas/background_style"), 0, DT_CANVAS_BACKGROUND_LAST - 1);
  const char *grid_color = dt_conf_get_string_const("canvas/grid_color");
  dt_canvas_color_parse(grid_color, &canvas->grid_color);
  canvas->paper_size = (uint32_t)CLAMP(dt_conf_get_int("canvas/paper_size"), 0, DT_CANVAS_PAPER_LAST - 1);
  canvas->page_margin = (float)fmax(dt_conf_get_float("canvas/page_margin"), 0.0);
  canvas->page_bleed = (float)fmax(dt_conf_get_float("canvas/page_bleed"), 0.0);
  dt_canvas_color_parse(dt_conf_get_string_const("canvas/guide_margin_color"), &canvas->margin_color);
  dt_canvas_color_parse(dt_conf_get_string_const("canvas/guide_bleed_color"), &canvas->bleed_color);
  if(dt_conf_get_bool("canvas/margin_visible")) canvas->grid_flags |= DT_CANVAS_MARGIN_VISIBLE;
  else canvas->grid_flags &= ~(uint32_t)DT_CANVAS_MARGIN_VISIBLE;
  if(dt_conf_get_bool("canvas/bleed_visible")) canvas->grid_flags |= DT_CANVAS_BLEED_VISIBLE;
  else canvas->grid_flags &= ~(uint32_t)DT_CANVAS_BLEED_VISIBLE;
  canvas->paper_landscape = dt_conf_get_bool("canvas/paper_landscape") ? 1u : 0u;
  const char *page_color = dt_conf_get_string_const("canvas/trim_color");
  dt_canvas_color_parse(page_color, &canvas->page_color);
  if(dt_conf_get_bool("canvas/page_visible")) canvas->grid_flags |= DT_CANVAS_PAGE_VISIBLE;
  else canvas->grid_flags &= ~(uint32_t)DT_CANVAS_PAGE_VISIBLE;
  if(dt_conf_get_bool("canvas/guides_over")) canvas->grid_flags |= DT_CANVAS_GUIDES_OVER;
  else canvas->grid_flags &= ~(uint32_t)DT_CANVAS_GUIDES_OVER;
  canvas->resolution = (float)CLAMP(dt_conf_get_float("canvas/resolution"), 18.0, 2400.0);
  canvas->spread_cols = (uint32_t)CLAMP(dt_conf_get_int("canvas/spread_cols"), 0, 64);
  canvas->spread_rows = (uint32_t)CLAMP(dt_conf_get_int("canvas/spread_rows"), 0, 64);
  canvas->bind_gutter = (float)CLAMP(dt_conf_get_float("canvas/bind_gutter"), 0.0, 2000.0);
  const char *border = dt_conf_get_string_const("canvas/border_color");
  dt_canvas_color_parse(border, &canvas->border_color);
  const char *background = dt_conf_get_string_const("canvas/background_color");
  dt_canvas_color_parse(background, &canvas->background);
  const char *font = dt_conf_get_string_const("canvas/default_font");
  if(!IS_NULL_PTR(font) && font[0] != '\0') g_strlcpy(canvas->default_font, font, sizeof(canvas->default_font));
  canvas->image_long_edge = dt_conf_get_int("canvas/image_long_edge");
  canvas->jpeg_quality = dt_conf_get_int("canvas/jpeg_quality");
  const char *shadow_color = dt_conf_get_string_const("canvas/shadow_color");
  dt_canvas_color_parse(shadow_color, &canvas->shadow.color);
  canvas->shadow.offset_x = dt_conf_get_float("canvas/shadow_offset_x");
  canvas->shadow.offset_y = dt_conf_get_float("canvas/shadow_offset_y");
  canvas->shadow.blur = dt_conf_get_float("canvas/shadow_radius");
  const char *padding_color = dt_conf_get_string_const("canvas/guide_padding_color");
  dt_canvas_color_parse(padding_color, &canvas->padding_color);
  if(dt_conf_get_bool("canvas/padding_visible")) canvas->grid_flags |= DT_CANVAS_PADDING_VISIBLE;
  else canvas->grid_flags &= ~(uint32_t)DT_CANVAS_PADDING_VISIBLE;
  canvas->texture_contrast = dt_conf_get_float("canvas/texture_contrast");
  canvas->texture_detail = dt_conf_get_float("canvas/texture_detail");
  canvas->texture_scale = dt_conf_get_float("canvas/texture_scale");
  canvas->texture_grain = dt_conf_get_float("canvas/texture_grain");
  canvas->corner_radius = dt_conf_get_float("canvas/corner_radius");
  canvas->dirty = FALSE;
}

static void _recovery_path(char *path, const size_t path_len)
{
  char configdir[DT_PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(configdir, sizeof(configdir));
  snprintf(path, path_len, "%s/%s", configdir, CANVAS_RECOVERY_FILE);
}

static void _announce_document(dt_view_t *self)
{
  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_CANVAS_CHANGED);
  dt_control_queue_redraw_center();
}

/** Replace the open document. Takes ownership of `canvas`; NULL opens a fresh one. */
static void _set_document(dt_view_t *self, dt_canvas_t *canvas)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  dt_canvas_render_cancel_all();
  dt_undo_clear(dt_undo_get_global(), DT_UNDO_CANVAS);
  dt_canvas_free(view->drag_snapshot);
  view->drag_snapshot = NULL;
  // An edit the properties still held open was an edit of the document going away: nothing to record.
  dt_canvas_free(view->props_snapshot);
  view->props_snapshot = NULL;
  view->props_effects = 0u;
  view->props_live = FALSE;
  view->drag = DT_CANVAS_DRAG_NONE;
  view->connecting = FALSE;
  view->connect_from = 0;
  view->anchor_hover_id = 0;
  view->hover = 0;
  view->mask_node_hover = -1;
  view->mask_node_hover = -1;
  g_array_set_size(view->selection, 0);
  dt_canvas_surface_cache_clear(view->cache);
  dt_canvas_free(view->canvas);
  if(IS_NULL_PTR(canvas))
  {
    canvas = dt_canvas_new();
    _canvas_apply_conf_defaults(canvas);
  }
  view->canvas = canvas;
  view->token++;
  _restore_viewport(view);
  _props_sync(self);
  _announce_document(self);
}

static void _sync_check_all(dt_canvas_view_t *view)
{
  for(guint idx = 0; idx < dt_canvas_object_count(view->canvas); idx++)
  {
    dt_canvas_object_t *object = dt_canvas_object_at(view->canvas, idx);
    if(object->kind != DT_CANVAS_OBJECT_IMAGE) continue;
    if(object->image.sync_status == DT_CANVAS_SYNC_RENDERING) continue;
    dt_canvas_render_check(&object->image);
  }
}

/**
 * A picture's render started, finished or failed. The strip's summary says which, and the status is no
 * edit: it moves no generation, and a failure touches nothing else either, so the properties showing
 * that picture are told to fill again rather than left saying "rendering". A map's rows show no status.
 */
static void _props_render_status_changed(dt_view_t *self, const uint32_t object_id)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(view->props_id == 0 || view->props_id != object_id) return;
  view->props_refill_owed = TRUE;
  _props_sync(self);
}

static gboolean _start_render(dt_view_t *self, dt_canvas_object_t *object, const int32_t imgid)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(imgid <= 0 || IS_NULL_PTR(object) || object->kind != DT_CANVAS_OBJECT_IMAGE) return FALSE;
  const gboolean queued = dt_canvas_render_start(imgid, object->id, view->token, view->canvas->image_long_edge,
                                                 view->canvas->jpeg_quality, _render_done, self);
  if(!queued) return FALSE;
  object->image.sync_status = DT_CANVAS_SYNC_RENDERING;
  _props_render_status_changed(self, object->id);
  return TRUE;
}

/** Fetch a map frame's tiles at twice its size on the canvas, so it stays sharp when zoomed. */
static gboolean _start_map_render(dt_view_t *self, dt_canvas_object_t *object)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(object) || object->kind != DT_CANVAS_OBJECT_MAP) return FALSE;
  const int32_t width = (int32_t)CLAMP(lround(object->width * 2.0), 256, 2048);
  const int32_t height = (int32_t)CLAMP(lround(object->height * 2.0), 256, 2048);
  const gboolean queued = dt_canvas_render_map_start(object->id, view->token, object->map.latitude,
                                                     object->map.longitude, object->map.zoom, object->map.source,
                                                     width, height, view->canvas->jpeg_quality, _render_done, self);
  if(queued) object->map.sync_status = DT_CANVAS_SYNC_RENDERING;
  return queued;
}

/** Re-render every image frame whose status is `only` (or every one when `only` is UNKNOWN). */
static int _refresh_images(dt_view_t *self, const dt_canvas_sync_status_t only)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  int started = 0;
  for(guint idx = 0; idx < dt_canvas_object_count(view->canvas); idx++)
  {
    dt_canvas_object_t *object = dt_canvas_object_at(view->canvas, idx);
    if(object->kind == DT_CANVAS_OBJECT_MAP)
    {
      // A map is refreshed with everything, or when it has no render yet.
      if(object->map.sync_status == DT_CANVAS_SYNC_RENDERING) continue;
      if(only != DT_CANVAS_SYNC_UNKNOWN && !IS_NULL_PTR(object->map.jpeg)) continue;
      if(_start_map_render(self, object)) started++;
      continue;
    }
    if(object->kind != DT_CANVAS_OBJECT_IMAGE) continue;
    if(object->image.sync_status == DT_CANVAS_SYNC_RENDERING) continue;
    if(only != DT_CANVAS_SYNC_UNKNOWN && object->image.sync_status != only) continue;
    const int32_t imgid = dt_canvas_render_locate_source(&object->image);
    if(imgid <= 0)
    {
      object->image.sync_status = DT_CANVAS_SYNC_MISSING;
      continue;
    }
    // The identity record follows the library too: a moved file or a new duplicate is recorded.
    dt_canvas_render_describe_source(imgid, &object->image);
    if(_start_render(self, object, imgid)) started++;
  }
  return started;
}

static void _render_done(uint32_t object_id, uint64_t token, GBytes *jpeg, int32_t pixel_width, int32_t pixel_height,
                         uint64_t history_hash, gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view) || IS_NULL_PTR(view->canvas) || token != view->token) return;
  dt_canvas_object_t *object = dt_canvas_find_object(view->canvas, object_id);
  if(IS_NULL_PTR(object)) return;
  if(object->kind == DT_CANVAS_OBJECT_MAP)
  {
    if(IS_NULL_PTR(jpeg))
    {
      // Keep whatever the frame showed: a failed provider must not blank a map that was fine.
      object->map.sync_status = DT_CANVAS_SYNC_MISSING;
      dt_control_log(_("the canvas could not fetch the map tiles from %s"),
                     dt_canvas_map_source_name(dt_canvas_map_source_index(object->map.source)));
    }
    else
    {
      dt_canvas_map_set_render(view->canvas, object, jpeg, pixel_width, pixel_height,
                               (int64_t)g_get_real_time() / G_USEC_PER_SEC);
    }
    dt_control_queue_redraw_center();
    return;
  }
  if(object->kind != DT_CANVAS_OBJECT_IMAGE) return;
  if(IS_NULL_PTR(jpeg))
  {
    object->image.sync_status = IS_NULL_PTR(object->image.jpeg) ? DT_CANVAS_SYNC_MISSING : DT_CANVAS_SYNC_STALE;
    dt_control_log(_("the canvas could not render `%s'"), object->image.filename);
  }
  else
  {
    // The render job leaves the pipeline in Adobe RGB, the canvas's own encoding.
    dt_canvas_image_set_render(view->canvas, object, jpeg, pixel_width, pixel_height, history_hash,
                               (int64_t)g_get_real_time() / G_USEC_PER_SEC, DT_CANVAS_COLORSPACE_ADOBERGB);
  }
  _props_render_status_changed(self, object_id);
  dt_control_queue_redraw_center();
}

/* --- file dialogs ------------------------------------------------------------------- */

static GtkFileFilter *_canvas_file_filter(void)
{
  GtkFileFilter *filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, _("Ansel canvas"));
  gtk_file_filter_add_pattern(filter, "*" DT_CANVAS_FILE_EXTENSION);
  return filter;
}

static void _remember_directory(const char *path)
{
  gchar *directory = g_path_get_dirname(path);
  dt_conf_set_string("canvas/last_directory", directory);
  dt_free(directory);
}

static gchar *_choose_file(const char *title, const GtkFileChooserAction action, const char *suggested_name,
                           GtkFileFilter *filter)
{
  GtkWindow *parent = GTK_WINDOW(dt_ui_main_window(dt_gui_get_ui()));
  GtkFileChooserNative *chooser = gtk_file_chooser_native_new(
      title, parent, action, action == GTK_FILE_CHOOSER_ACTION_SAVE ? _("_Save") : _("_Open"), _("_Cancel"));
  if(!IS_NULL_PTR(filter)) gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), filter);
  const char *last_directory = dt_conf_get_string_const("canvas/last_directory");
  if(!IS_NULL_PTR(last_directory) && last_directory[0] != '\0')
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser), last_directory);
  if(action == GTK_FILE_CHOOSER_ACTION_SAVE)
  {
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(chooser), TRUE);
    if(!IS_NULL_PTR(suggested_name)) gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(chooser), suggested_name);
  }
  gchar *path = NULL;
  if(gtk_native_dialog_run(GTK_NATIVE_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT)
    path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
  g_object_unref(chooser);
  dt_gui_refocus_parent(parent);
  return path;
}

static gboolean _save_to(dt_view_t *self, const char *path)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  _store_viewport(view);
  GError *error = NULL;
  if(!dt_canvas_save(view->canvas, path, &error))
  {
    dt_control_log(_("saving the canvas failed: %s"), IS_NULL_PTR(error) ? "?" : error->message);
    g_clear_error(&error);
    return FALSE;
  }
  _remember_directory(path);
  dt_control_log(_("canvas saved to `%s'"), path);
  _announce_document(self);
  return TRUE;
}

static gboolean _save_as(dt_view_t *self)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  gchar *suggested = NULL;
  if(!IS_NULL_PTR(view->canvas->path))
    suggested = g_path_get_basename(view->canvas->path);
  else if(view->canvas->title[0] != '\0')
    suggested = g_strdup_printf("%s" DT_CANVAS_FILE_EXTENSION, view->canvas->title);
  else
    suggested = g_strdup("untitled" DT_CANVAS_FILE_EXTENSION);
  gchar *path = _choose_file(_("Save the canvas"), GTK_FILE_CHOOSER_ACTION_SAVE, suggested, _canvas_file_filter());
  dt_free(suggested);
  if(IS_NULL_PTR(path)) return FALSE;
  if(!g_str_has_suffix(path, DT_CANVAS_FILE_EXTENSION))
  {
    gchar *with_extension = g_strconcat(path, DT_CANVAS_FILE_EXTENSION, NULL);
    dt_free(path);
    path = with_extension;
  }
  const gboolean saved = _save_to(self, path);
  dt_free(path);
  return saved;
}

static gboolean _save(dt_view_t *self)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view->canvas->path)) return _save_as(self);
  return _save_to(self, view->canvas->path);
}

/** Offer to save a dirty document. FALSE means the user cancelled whatever was about to happen. */
static gboolean _confirm_discard(dt_view_t *self)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view->canvas) || !view->canvas->dirty) return TRUE;
  const int choice = dt_gui_show_standalone_three_choice_dialog(
      _("Unsaved canvas"), _("The canvas has unsaved changes.\nDo you want to save them?"), _("Save"),
      _("Discard"), _("Cancel"));
  if(choice == 0) return _save(self);
  return choice == 1;
}

static void _open_path(dt_view_t *self, const char *path)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  GError *error = NULL;
  dt_canvas_t *canvas = dt_canvas_load(path, &error);
  if(IS_NULL_PTR(canvas))
  {
    dt_control_log(_("opening `%s' failed: %s"), path, IS_NULL_PTR(error) ? "?" : error->message);
    g_clear_error(&error);
    return;
  }
  _remember_directory(path);
  _set_document(self, canvas);
  _sync_check_all(view);
  if(dt_conf_get_bool("canvas/auto_refresh"))
  {
    const int started = _refresh_images(self, DT_CANVAS_SYNC_STALE);
    if(started > 0) dt_control_log(ngettext("re-rendering %d image", "re-rendering %d images", started), started);
  }
  if(view->width > 0 && view->height > 0 && canvas->view_zoom == 1.0 && canvas->view_x == 0.0 && canvas->view_y == 0.0)
    _zoom_fit(view);
  dt_control_queue_redraw_center();
}

static void _open_canvas(dt_view_t *self)
{
  if(!_confirm_discard(self)) return;
  gchar *path = _choose_file(_("Open a canvas"), GTK_FILE_CHOOSER_ACTION_OPEN, NULL, _canvas_file_filter());
  if(IS_NULL_PTR(path)) return;
  _open_path(self, path);
  dt_free(path);
}

/* --- the PDF export dialog ------------------------------------------------------------ */

typedef struct dt_canvas_export_dialog_t
{
  GtkWidget *format;
  dt_canvas_export_format_t offered[DT_CANVAS_EXPORT_LAST]; ///< what each row of `format` stands for
  int offered_count;
  GtkWidget *dpi;
  GtkWidget *quality;
  GtkWidget *profile;
  GtkWidget *intent;
  dt_colorprofile_desc_t *profiles;
  size_t profile_count;
} dt_canvas_export_dialog_t;

static GtkWidget *_labelled_row(GtkWidget *grid, const int row, const char *label, GtkWidget *widget)
{
  GtkWidget *text = gtk_label_new(label);
  gtk_widget_set_halign(text, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), text, 0, row, 1, 1);
  gtk_widget_set_hexpand(widget, TRUE);
  gtk_grid_attach(GTK_GRID(grid), widget, 1, row, 1, 1);
  return widget;
}

/** Only the formats that compress lossily have a quality to set. */
static void _export_format_changed(GtkComboBox *combo, gpointer data)
{
  dt_canvas_export_dialog_t *widgets = (dt_canvas_export_dialog_t *)data;
  const int row = CLAMP(gtk_combo_box_get_active(combo), 0, MAX(widgets->offered_count - 1, 0));
  const dt_canvas_export_format_t format = widgets->offered[row];
  const gboolean lossy = format == DT_CANVAS_EXPORT_JPEG || format == DT_CANVAS_EXPORT_PDF;
  gtk_widget_set_sensitive(widgets->quality, lossy);
}

static void _export_canvas(dt_view_t *self)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  GtkWindow *parent = GTK_WINDOW(dt_ui_main_window(dt_gui_get_ui()));
  GtkWidget *dialog = gtk_dialog_new_with_buttons(_("Export the canvas"), parent, GTK_DIALOG_MODAL, _("_Cancel"),
                                                  GTK_RESPONSE_CANCEL, _("_Export"), GTK_RESPONSE_OK, NULL);
  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(6));
  gtk_grid_set_column_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(12));
  gtk_container_set_border_width(GTK_CONTAINER(grid), DT_PIXEL_APPLY_DPI(12));
  gtk_box_pack_start(GTK_BOX(content), grid, TRUE, TRUE, 0);

  dt_canvas_export_dialog_t widgets;
  memset(&widgets, 0, sizeof(widgets));

  // A transparent canvas composites to real holes, and a format with no alpha channel cannot
  // carry them: it is left out of the list rather than offered and then refused.
  const gboolean transparent = dt_canvas_background_is_transparent(view->canvas->background_style);
  const dt_canvas_export_format_t every[DT_CANVAS_EXPORT_LAST]
      = { DT_CANVAS_EXPORT_PDF, DT_CANVAS_EXPORT_PNG, DT_CANVAS_EXPORT_JPEG, DT_CANVAS_EXPORT_TIFF };
  const char *labels[DT_CANVAS_EXPORT_LAST] = { N_("PDF, every page in one file"), N_("PNG, one file per page"),
                                                N_("JPEG, one file per page"), N_("TIFF, every page in one file") };
  const int stored = CLAMP(dt_conf_get_int("canvas/export/format"), 0, DT_CANVAS_EXPORT_LAST - 1);
  widgets.format = gtk_combo_box_text_new();
  for(int idx = 0; idx < DT_CANVAS_EXPORT_LAST; idx++)
  {
    if(transparent && !dt_canvas_export_format_carries_alpha(every[idx])) continue;
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widgets.format), _(labels[idx]));
    if((int)every[idx] == stored) gtk_combo_box_set_active(GTK_COMBO_BOX(widgets.format), widgets.offered_count);
    widgets.offered[widgets.offered_count++] = every[idx];
  }
  if(gtk_combo_box_get_active(GTK_COMBO_BOX(widgets.format)) < 0)
    gtk_combo_box_set_active(GTK_COMBO_BOX(widgets.format), 0);
  g_signal_connect(widgets.format, "changed", G_CALLBACK(_export_format_changed), &widgets);
  _labelled_row(grid, 0, _("Format"), widgets.format);

  widgets.dpi = gtk_spin_button_new_with_range(72.0, 1200.0, 1.0);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(widgets.dpi), dt_conf_get_int("canvas/pdf/dpi"));
  gtk_widget_set_tooltip_text(widgets.dpi,
                              _("A page is rasterised at exactly this many pixels per inch of its own size, and no more"));
  _labelled_row(grid, 1, _("Resolution (dpi)"), widgets.dpi);

  widgets.quality = gtk_spin_button_new_with_range(50.0, 100.0, 1.0);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(widgets.quality), CLAMP(dt_conf_get_int("canvas/export/quality"), 50, 100));
  gtk_widget_set_tooltip_text(widgets.quality,
                              _("How hard the pages are compressed. A PDF page is a photograph and is carried as one, "
                                "which is what keeps the file from weighing what its pixels weigh; 100 keeps every code "
                                "and makes it many times larger. PNG and TIFF are always lossless."));
  _labelled_row(grid, 2, _("Quality"), widgets.quality);

  widgets.profile = gtk_combo_box_text_new();
  widgets.profile_count = dt_colorspaces_enumerate_profiles(DT_PROFILE_ROLE_OUTPUT, &widgets.profiles);
  const int conf_type = dt_conf_get_int("canvas/pdf/icc_type");
  const char *conf_filename = dt_conf_get_string_const("canvas/pdf/icc_filename");
  for(size_t idx = 0; idx < widgets.profile_count; idx++)
  {
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widgets.profile), widgets.profiles[idx].name);
    if(widgets.profiles[idx].type == conf_type
       && (conf_type != DT_COLORSPACE_FILE || !g_strcmp0(widgets.profiles[idx].filename, conf_filename)))
      gtk_combo_box_set_active(GTK_COMBO_BOX(widgets.profile), (int)idx);
  }
  if(gtk_combo_box_get_active(GTK_COMBO_BOX(widgets.profile)) < 0 && widgets.profile_count > 0)
    gtk_combo_box_set_active(GTK_COMBO_BOX(widgets.profile), 0);
  _labelled_row(grid, 3, _("Output profile"), widgets.profile);

  widgets.intent = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widgets.intent), _("perceptual"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widgets.intent), _("relative colorimetric"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widgets.intent), _("saturation"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widgets.intent), _("absolute colorimetric"));
  gtk_combo_box_set_active(GTK_COMBO_BOX(widgets.intent), CLAMP(dt_conf_get_int("canvas/pdf/intent"), 0, 3));
  _labelled_row(grid, 4, _("Rendering intent"), widgets.intent);

  // The page is the document's: say which one, so nobody looks for it here.
  double canvas_paper_width = 0.0;
  double canvas_paper_height = 0.0;
  gchar *note = NULL;
  if(dt_canvas_paper_dimensions(view->canvas, &canvas_paper_width, &canvas_paper_height))
    note = g_strdup_printf(_("One page per canvas page of %.0f x %.0f mm, empty ones skipped. The page size and its "
                             "orientation are the canvas's, under Guides in the toolbar."),
                           dt_pdf_point_to_mm(canvas_paper_width), dt_pdf_point_to_mm(canvas_paper_height));
  else
    note = g_strdup(_("The canvas is not divided into pages, so this is ONE page around everything on it, with the "
                      "canvas's margin as the white space around the content. Give it a page size under Guides in "
                      "the toolbar to export several."));
  if(transparent)
  {
    gchar *both = g_strconcat(note, "\n",
                              _("The canvas is transparent, so only the formats that can carry a hole are offered: "
                                "JPEG has no alpha channel."),
                              NULL);
    dt_free(note);
    note = both;
  }
  GtkWidget *note_label = gtk_label_new(note);
  gtk_label_set_line_wrap(GTK_LABEL(note_label), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(note_label), 60);
  gtk_widget_set_halign(note_label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), note_label, 0, 5, 2, 1);
  dt_free(note);

  gtk_widget_show_all(dialog);
  _export_format_changed(GTK_COMBO_BOX(widgets.format), &widgets);
  const gint response = gtk_dialog_run(GTK_DIALOG(dialog));
  dt_canvas_export_options_t options = dt_canvas_export_options_default();
  const gboolean proceed = response == GTK_RESPONSE_OK;
  if(proceed)
  {
    options.format = widgets.offered[CLAMP(gtk_combo_box_get_active(GTK_COMBO_BOX(widgets.format)), 0,
                                          MAX(widgets.offered_count - 1, 0))];
    options.dpi = (float)gtk_spin_button_get_value(GTK_SPIN_BUTTON(widgets.dpi));
    options.quality = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(widgets.quality));
    options.intent = (dt_iop_color_intent_t)CLAMP(gtk_combo_box_get_active(GTK_COMBO_BOX(widgets.intent)), 0, 3);
    const int profile = gtk_combo_box_get_active(GTK_COMBO_BOX(widgets.profile));
    if(profile >= 0 && (size_t)profile < widgets.profile_count)
    {
      options.icc_type = widgets.profiles[profile].type;
      g_strlcpy(options.icc_filename, widgets.profiles[profile].filename, sizeof(options.icc_filename));
    }
    dt_conf_set_int("canvas/export/format", (int)options.format);
    dt_conf_set_int("canvas/pdf/dpi", (int)options.dpi);
    dt_conf_set_int("canvas/export/quality", options.quality);
    dt_conf_set_int("canvas/pdf/icc_type", options.icc_type);
    dt_conf_set_string("canvas/pdf/icc_filename", options.icc_filename);
    dt_conf_set_int("canvas/pdf/intent", options.intent);
  }
  dt_free_align(widgets.profiles);
  gtk_widget_destroy(dialog);
  dt_gui_refocus_parent(parent);
  if(!proceed) return;

  const char *extension = dt_canvas_export_extension(options.format);
  GtkFileFilter *filter = gtk_file_filter_new();
  gchar *pattern = g_strconcat("*", extension, NULL);
  gtk_file_filter_set_name(filter, extension + 1);
  gtk_file_filter_add_pattern(filter, pattern);
  dt_free(pattern);
  gchar *suggested = NULL;
  if(!IS_NULL_PTR(view->canvas->path))
  {
    gchar *base = g_path_get_basename(view->canvas->path);
    if(g_str_has_suffix(base, DT_CANVAS_FILE_EXTENSION)) base[strlen(base) - strlen(DT_CANVAS_FILE_EXTENSION)] = '\0';
    suggested = g_strconcat(base, extension, NULL);
    dt_free(base);
  }
  else
  {
    suggested = g_strconcat("canvas", extension, NULL);
  }
  gchar *path = _choose_file(_("Export the canvas"), GTK_FILE_CHOOSER_ACTION_SAVE, suggested, filter);
  dt_free(suggested);
  if(IS_NULL_PTR(path)) return;
  GError *error = NULL;
  if(dt_canvas_export(view->canvas, path, &options, &error))
  {
    dt_control_log(_("canvas exported to `%s'"), path);
  }
  else
  {
    dt_control_log(_("exporting the canvas failed: %s"), IS_NULL_PTR(error) ? "?" : error->message);
    g_clear_error(&error);
  }
  dt_free(path);
}

/* --- text and colour dialogs ------------------------------------------------------- */

static void _edit_text(dt_view_t *self, dt_canvas_object_t *object)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(object) || object->kind != DT_CANVAS_OBJECT_TEXT) return;
  GtkWindow *parent = GTK_WINDOW(dt_ui_main_window(dt_gui_get_ui()));
  GtkWidget *dialog = gtk_dialog_new_with_buttons(_("Edit the text frame"), parent, GTK_DIALOG_MODAL, _("_Cancel"),
                                                  GTK_RESPONSE_CANCEL, _("_Apply"), GTK_RESPONSE_OK, NULL);
  gtk_window_set_default_size(GTK_WINDOW(dialog), DT_PIXEL_APPLY_DPI(560), DT_PIXEL_APPLY_DPI(420));
  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_PIXEL_APPLY_DPI(6));
  gtk_container_set_border_width(GTK_CONTAINER(box), DT_PIXEL_APPLY_DPI(12));
  gtk_box_pack_start(GTK_BOX(content), box, TRUE, TRUE, 0);

  GtkWidget *hint = gtk_label_new(_("Markdown: # headings, **bold**, *italic*, `code`, - lists, [links](url)"));
  gtk_widget_set_halign(hint, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(box), hint, FALSE, FALSE, 0);

  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
  GtkWidget *text_view = gtk_text_view_new();
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(text_view), TRUE);
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
  gtk_text_buffer_set_text(buffer, dt_canvas_text_get_markdown(object), -1);
  gtk_container_add(GTK_CONTAINER(scroll), text_view);
  gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);

  GtkWidget *font_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(6));
  gtk_box_pack_start(GTK_BOX(font_row), gtk_label_new(_("Font")), FALSE, FALSE, 0);
  GtkWidget *font_button = gtk_font_button_new_with_font(dt_canvas_text_effective_font(view->canvas, object));
  gtk_box_pack_start(GTK_BOX(font_row), font_button, TRUE, TRUE, 0);
  GtkWidget *fit_height = gtk_check_button_new_with_label(_("fit the frame height to the text"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(fit_height), TRUE);
  gtk_box_pack_start(GTK_BOX(box), font_row, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), fit_height, FALSE, FALSE, 0);

  gtk_widget_show_all(dialog);
  const gint response = gtk_dialog_run(GTK_DIALOG(dialog));
  if(response == GTK_RESPONSE_OK)
  {
    dt_canvas_t *before = _begin_edit(view);
    GtkTextIter start;
    GtkTextIter end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    gchar *markdown = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    dt_canvas_text_set_markdown(view->canvas, object, markdown);
    dt_free(markdown);
    gchar *font = gtk_font_chooser_get_font(GTK_FONT_CHOOSER(font_button));
    if(!IS_NULL_PTR(font))
    {
      // A font equal to the canvas default is stored as "no font of its own".
      if(!g_strcmp0(font, view->canvas->default_font))
        object->text.font[0] = '\0';
      else
        g_strlcpy(object->text.font, font, sizeof(object->text.font));
      dt_free(font);
    }
    if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fit_height))
       || (object->text.text_flags & DT_CANVAS_TEXT_AUTO_HEIGHT))
      dt_canvas_paint_text_fit_height(view->canvas, object);
    dt_canvas_touch(view->canvas);
    _record_undo(self, before);
  }
  gtk_widget_destroy(dialog);
  dt_gui_refocus_parent(parent);
  // The dialog may have changed the font and the frame's height, which the properties show.
  _props_sync(self);
  dt_control_queue_redraw_center();
}

/* --- editing the selection ----------------------------------------------------------- */

static void _delete_selection(dt_view_t *self)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(view->selection->len == 0) return;
  dt_canvas_t *before = _begin_edit(view);
  for(guint idx = 0; idx < view->selection->len; idx++)
  {
    dt_canvas_remove_object(view->canvas, g_array_index(view->selection, uint32_t, idx));
  }
  g_array_set_size(view->selection, 0);
  view->hover = 0;
  _record_undo(self, before);
  _props_sync(self);
  dt_control_queue_redraw_center();
}

static void _select_all(dt_canvas_view_t *view)
{
  g_array_set_size(view->selection, 0);
  for(guint idx = 0; idx < dt_canvas_object_count(view->canvas); idx++)
  {
    const dt_canvas_object_t *object = dt_canvas_object_at(view->canvas, idx);
    if(dt_canvas_object_is_frame(object)) g_array_append_val(view->selection, object->id);
  }
  dt_control_queue_redraw_center();
}

static void _apply_layout(dt_view_t *self, const dt_canvas_layout_t layout)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  dt_canvas_t *before = _begin_edit(view);
  const int columns = dt_conf_get_int("canvas/masonry_columns");
  // A single selected frame is not a group to arrange: lay the whole canvas out instead.
  const GArray *ids = view->selection->len > 1 ? view->selection : NULL;
  const dt_canvas_sort_t sort = (dt_canvas_sort_t)CLAMP(dt_conf_get_int("canvas/layout_sort"), 0, DT_CANVAS_SORT_LAST - 1);
  dt_canvas_layout_apply(view->canvas, ids, layout, columns, sort);
  _record_undo(self, before);
  dt_control_queue_redraw_center();
}

static void _add_text_frame(dt_view_t *self, const double x, const double y)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  dt_canvas_t *before = _begin_edit(view);
  dt_canvas_object_t *object = dt_canvas_add_text(view->canvas, dt_canvas_snap(view->canvas, x),
                                                  dt_canvas_snap(view->canvas, y), 0.0, 0.0, _("New note"));
  _select_only(view, object->id);
  _record_undo(self, before);
  _edit_text(self, object);
}

/** Read the `.txt` sidecar of a frame's source image into a linked text frame. */
static gboolean _load_sidecar_text(dt_canvas_view_t *view, dt_canvas_object_t *text)
{
  if(IS_NULL_PTR(text) || text->kind != DT_CANVAS_OBJECT_TEXT || text->text.linked_object == 0) return FALSE;
  const dt_canvas_object_t *image = dt_canvas_find_object(view->canvas, text->text.linked_object);
  if(IS_NULL_PTR(image) || image->kind != DT_CANVAS_OBJECT_IMAGE) return FALSE;
  const int32_t imgid = dt_canvas_render_locate_source(&image->image);
  if(imgid <= 0) return FALSE;
  char image_path[DT_PATH_MAX] = { 0 };
  gboolean from_cache = FALSE;
  dt_image_full_path(imgid, image_path, sizeof(image_path), &from_cache, __FUNCTION__);
  gchar *note_path = dt_image_build_text_path_from_path(image_path);
  gchar *contents = NULL;
  gboolean loaded = FALSE;
  if(!IS_NULL_PTR(note_path) && g_file_get_contents(note_path, &contents, NULL, NULL))
  {
    dt_canvas_text_set_markdown(view->canvas, text, contents);
    dt_canvas_props_settle(view->canvas, text);
    loaded = TRUE;
  }
  dt_free(contents);
  dt_free(note_path);
  return loaded;
}

/** Add a map frame at a point, of a place; starts fetching its tiles. */
static dt_canvas_object_t *_add_map(dt_view_t *self, const double x, const double y, const double latitude,
                                    const double longitude)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  // The stored preference may name a provider this build does not have: fall back to the first one.
  const uint32_t source
      = dt_canvas_map_source_id(dt_canvas_map_source_index((uint32_t)dt_conf_get_int("canvas/map_source")));
  dt_canvas_t *before = _begin_edit(view);
  dt_canvas_object_t *map = dt_canvas_add_map(view->canvas, dt_canvas_snap(view->canvas, x),
                                              dt_canvas_snap(view->canvas, y), latitude, longitude,
                                              dt_conf_get_int("canvas/map_zoom"), source);
  dt_conf_set_float("canvas/map_latitude", (float)latitude);
  dt_conf_set_float("canvas/map_longitude", (float)longitude);
  _select_only(view, map->id);
  _record_undo(self, before);
  _start_map_render(self, map);
  _props_sync(self);
  dt_control_queue_redraw_center();
  return map;
}

/** The text frame already showing an image frame's note, if any. */
static dt_canvas_object_t *_note_frame_of(const dt_canvas_view_t *view, const uint32_t image_id)
{
  for(guint idx = 0; idx < dt_canvas_object_count(view->canvas); idx++)
  {
    dt_canvas_object_t *object = dt_canvas_object_at(view->canvas, idx);
    if(object->kind == DT_CANVAS_OBJECT_TEXT && object->text.source == DT_CANVAS_TEXT_SOURCE_SIDECAR
       && object->text.linked_object == image_id)
      return object;
  }
  return NULL;
}

/**
 * Add a text frame under an image frame showing its `.txt` note.
 * @param require_note when TRUE and the image has no note file, nothing is added.
 * @return the new frame, or NULL when nothing was added.
 */
static dt_canvas_object_t *_add_sidecar_note(dt_canvas_view_t *view, dt_canvas_object_t *image,
                                             const gboolean require_note)
{
  if(IS_NULL_PTR(image) || image->kind != DT_CANVAS_OBJECT_IMAGE) return NULL;
  const dt_canvas_rect_t bounds = dt_canvas_object_bounds(image);
  dt_canvas_object_t *text = dt_canvas_add_text(view->canvas, image->x, bounds.y + bounds.height + 120.0,
                                                fmax(image->width, 200.0), 200.0, "");
  text->text.source = DT_CANVAS_TEXT_SOURCE_SIDECAR;
  text->text.linked_object = image->id;
  if(!_load_sidecar_text(view, text))
  {
    if(require_note)
    {
      dt_canvas_remove_object(view->canvas, text->id);
      return NULL;
    }
    dt_canvas_text_set_markdown(view->canvas, text, _("*No text note found for this image.*"));
    dt_canvas_props_settle(view->canvas, text);
  }
  return text;
}

static void _show_sidecar_note(dt_view_t *self, dt_canvas_object_t *image)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(image) || image->kind != DT_CANVAS_OBJECT_IMAGE) return;
  dt_canvas_object_t *existing = _note_frame_of(view, image->id);
  if(!IS_NULL_PTR(existing))
  {
    _select_only(view, existing->id);
    _props_sync(self);
    dt_control_queue_redraw_center();
    return;
  }
  dt_canvas_t *before = _begin_edit(view);
  dt_canvas_object_t *text = _add_sidecar_note(view, image, FALSE);
  if(IS_NULL_PTR(text))
  {
    dt_canvas_free(before);
    return;
  }
  _select_only(view, text->id);
  _record_undo(self, before);
  _props_sync(self);
  dt_control_queue_redraw_center();
}

/** Add the notes of the selected image frames -- of every image frame when none is selected. */
static void _add_notes(dt_view_t *self)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  GArray *targets = g_array_new(FALSE, FALSE, sizeof(uint32_t));
  for(guint idx = 0; idx < dt_canvas_object_count(view->canvas); idx++)
  {
    const dt_canvas_object_t *object = dt_canvas_object_at(view->canvas, idx);
    if(object->kind != DT_CANVAS_OBJECT_IMAGE) continue;
    if(view->selection->len > 0 && !_is_selected(view, object->id)) continue;
    if(!IS_NULL_PTR(_note_frame_of(view, object->id))) continue;
    g_array_append_val(targets, object->id);
  }
  dt_canvas_t *before = _begin_edit(view);
  int added = 0;
  g_array_set_size(view->selection, 0);
  for(guint idx = 0; idx < targets->len; idx++)
  {
    dt_canvas_object_t *image = dt_canvas_find_object(view->canvas, g_array_index(targets, uint32_t, idx));
    dt_canvas_object_t *text = _add_sidecar_note(view, image, TRUE);
    if(IS_NULL_PTR(text)) continue;
    g_array_append_val(view->selection, text->id);
    added++;
  }
  g_array_free(targets, TRUE);
  if(added > 0)
    _record_undo(self, before);
  else
    dt_canvas_free(before);
  dt_control_log(ngettext("added %d text note", "added %d text notes", added), added);
  dt_control_queue_redraw_center();
}

static void _refresh_sidecar_texts(dt_canvas_view_t *view)
{
  for(guint idx = 0; idx < dt_canvas_object_count(view->canvas); idx++)
  {
    dt_canvas_object_t *object = dt_canvas_object_at(view->canvas, idx);
    if(object->kind == DT_CANVAS_OBJECT_TEXT && object->text.source == DT_CANVAS_TEXT_SOURCE_SIDECAR)
      _load_sidecar_text(view, object);
  }
}

static void _open_in_darkroom(dt_canvas_object_t *image)
{
  if(IS_NULL_PTR(image) || image->kind != DT_CANVAS_OBJECT_IMAGE) return;
  const int32_t imgid = dt_canvas_render_locate_source(&image->image);
  if(imgid <= 0)
  {
    dt_control_log(_("`%s' is not in the library"), image->image.filename);
    return;
  }
  dt_ctl_open_image_in_darkroom(imgid);
}

/* --- context menus ---------------------------------------------------------------- */

typedef struct dt_canvas_menu_context_t
{
  dt_view_t *self;
  uint32_t object_id;
  double x;      ///< canvas point the menu was opened at
  double y;
  int value;     ///< an item-specific argument (a border width, a rotation step...)
} dt_canvas_menu_context_t;

static dt_canvas_menu_context_t *_menu_context(dt_view_t *self, const uint32_t object_id, const double x,
                                               const double y, const int value)
{
  dt_canvas_menu_context_t *context = g_new0(dt_canvas_menu_context_t, 1);
  context->self = self;
  context->object_id = object_id;
  context->x = x;
  context->y = y;
  context->value = value;
  return context;
}

/** Attach a context to a menu item so it is freed with the item. */
static GtkWidget *_menu_item(GtkWidget *menu, const char *label, void (*callback)(GtkWidget *, gpointer),
                             dt_canvas_menu_context_t *context)
{
  GtkWidget *item = ctx_gtk_menu_item_new_with_icon(label, menu, callback, context, DT_MENU_ICON_NONE);
  g_object_set_data_full(G_OBJECT(item), "canvas-context", context, g_free);
  return item;
}

/**
 * An item that names the key doing the same thing, the way a menu bar does. `shortcut` is a
 * label already (NULL for none) and `tooltip` says what else does it; both are translated.
 */
static GtkWidget *_menu_item_with_shortcut(GtkWidget *menu, const char *label, const char *shortcut,
                                           const char *tooltip, void (*callback)(GtkWidget *, gpointer),
                                           dt_canvas_menu_context_t *context)
{
  GtkWidget *item = NULL;
  if(IS_NULL_PTR(shortcut))
    item = ctx_gtk_menu_item_new_with_icon(label, menu, callback, context, DT_MENU_ICON_NONE);
  else
    item = ctx_gtk_menu_item_new_with_markup_and_shortcut(label, shortcut, menu, callback, context);
  g_object_set_data_full(G_OBJECT(item), "canvas-context", context, g_free);
  if(!IS_NULL_PTR(tooltip)) gtk_widget_set_tooltip_text(item, tooltip);
  return item;
}

/**
 * The key a canvas action is bound to, as the user bound it, ready to show; NULL when it has
 * none. Read from the accel map every time rather than from the table's default, since the
 * binding is the user's to change.
 */
static gchar *_accel_label(const char *action_name)
{
  gchar *path = dt_accels_build_path(CANVAS_ACCEL_SCOPE, action_name);
  GtkAccelKey key = { 0 };
  const gboolean bound = gtk_accel_map_lookup_entry(path, &key) && key.accel_key != 0;
  dt_free(path);
  if(!bound) return NULL;
  return gtk_accelerator_get_label(key.accel_key, dt_accels_display_mods(key.accel_mods));
}

static dt_canvas_object_t *_menu_object(const dt_canvas_menu_context_t *context)
{
  const dt_canvas_view_t *view = (const dt_canvas_view_t *)context->self->data;
  return dt_canvas_find_object(view->canvas, context->object_id);
}

/** The properties of the object the menu was opened on, anchored where it was opened. */
static void _menu_properties(GtkWidget *widget, gpointer data)
{
  dt_canvas_menu_context_t *context = (dt_canvas_menu_context_t *)data;
  _props_open(context->self, context->object_id, TRUE, context->x, context->y);
}

static void _menu_edit_text(GtkWidget *widget, gpointer data)
{
  dt_canvas_menu_context_t *context = (dt_canvas_menu_context_t *)data;
  _edit_text(context->self, _menu_object(context));
}

static void _menu_fit_text(GtkWidget *widget, gpointer data)
{
  dt_canvas_menu_context_t *context = (dt_canvas_menu_context_t *)data;
  dt_canvas_view_t *view = (dt_canvas_view_t *)context->self->data;
  dt_canvas_object_t *object = _menu_object(context);
  if(IS_NULL_PTR(object) || object->kind != DT_CANVAS_OBJECT_TEXT) return;
  cairo_surface_t *scratch = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
  cairo_t *cr = cairo_create(scratch);
  const double natural = dt_canvas_paint_text_natural_height(cr, view->canvas, object);
  cairo_destroy(cr);
  cairo_surface_destroy(scratch);
  if(natural <= 0.0) return;
  dt_canvas_t *before = _begin_edit(view);
  object->height = natural;
  dt_canvas_touch(view->canvas);
  _record_undo(context->self, before);
  dt_control_queue_redraw_center();
}

static void _menu_reload_sidecar(GtkWidget *widget, gpointer data)
{
  dt_canvas_menu_context_t *context = (dt_canvas_menu_context_t *)data;
  dt_canvas_view_t *view = (dt_canvas_view_t *)context->self->data;
  dt_canvas_object_t *object = _menu_object(context);
  dt_canvas_t *before = _begin_edit(view);
  if(_load_sidecar_text(view, object))
    _record_undo(context->self, before);
  else
    dt_canvas_free(before);
  dt_control_queue_redraw_center();
}

static void _menu_open_darkroom(GtkWidget *widget, gpointer data)
{
  dt_canvas_menu_context_t *context = (dt_canvas_menu_context_t *)data;
  _open_in_darkroom(_menu_object(context));
}

/**
 * Render a picture again from its development in the library, or fetch a map's tiles again. The
 * context menu asks for it, and so does the picture's button in its properties.
 */
static void _refresh_object(dt_view_t *self, dt_canvas_object_t *object)
{
  if(!IS_NULL_PTR(object) && object->kind == DT_CANVAS_OBJECT_MAP)
  {
    _start_map_render(self, object);
    dt_control_queue_redraw_center();
    return;
  }
  if(IS_NULL_PTR(object) || object->kind != DT_CANVAS_OBJECT_IMAGE) return;
  const int32_t imgid = dt_canvas_render_locate_source(&object->image);
  if(imgid <= 0)
  {
    object->image.sync_status = DT_CANVAS_SYNC_MISSING;
    dt_control_log(_("`%s' is not in the library"), object->image.filename);
    return;
  }
  dt_canvas_render_describe_source(imgid, &object->image);
  _start_render(self, object, imgid);
  dt_control_queue_redraw_center();
}

static void _menu_refresh_image(GtkWidget *widget, gpointer data)
{
  dt_canvas_menu_context_t *context = (dt_canvas_menu_context_t *)data;
  _refresh_object(context->self, _menu_object(context));
}

/**
 * Add a map of where a picture was taken, under it. The context menu asks for it, and so does the
 * picture's button in its properties.
 */
static void _map_of_image(dt_view_t *self, dt_canvas_object_t *image)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(image) || image->kind != DT_CANVAS_OBJECT_IMAGE) return;
  const int32_t imgid = dt_canvas_render_locate_source(&image->image);
  if(imgid <= 0)
  {
    dt_control_log(_("`%s' is not in the library"), image->image.filename);
    return;
  }
  const dt_image_t *img = dt_image_cache_get(imgid, 'r');
  if(IS_NULL_PTR(img)) return;
  const double latitude = img->geoloc.latitude;
  const double longitude = img->geoloc.longitude;
  dt_image_cache_read_release(img);
  if(isnan(latitude) || isnan(longitude))
  {
    dt_control_log(_("`%s' carries no location"), image->image.filename);
    return;
  }
  const dt_canvas_rect_t bounds = dt_canvas_object_bounds(image);
  _add_map(self, image->x, bounds.y + bounds.height + view->canvas->padding + image->height * 0.375, latitude,
           longitude);
}

static void _menu_map_of_image(GtkWidget *widget, gpointer data)
{
  dt_canvas_menu_context_t *context = (dt_canvas_menu_context_t *)data;
  _map_of_image(context->self, _menu_object(context));
}

static void _menu_show_note(GtkWidget *widget, gpointer data)
{
  dt_canvas_menu_context_t *context = (dt_canvas_menu_context_t *)data;
  _show_sidecar_note(context->self, _menu_object(context));
}

static void _menu_z_order(GtkWidget *widget, gpointer data)
{
  dt_canvas_menu_context_t *context = (dt_canvas_menu_context_t *)data;
  dt_canvas_view_t *view = (dt_canvas_view_t *)context->self->data;
  dt_canvas_t *before = _begin_edit(view);
  switch(context->value)
  {
    case 0:
      dt_canvas_object_to_front(view->canvas, context->object_id);
      break;
    case 1:
      dt_canvas_object_raise(view->canvas, context->object_id);
      break;
    case 2:
      dt_canvas_object_lower(view->canvas, context->object_id);
      break;
    default:
      dt_canvas_object_to_back(view->canvas, context->object_id);
      break;
  }
  _record_undo(context->self, before);
  dt_control_queue_redraw_center();
}

/* The cutout's properties as sliders in the context menu, the way the darkroom's mask menu
 * offers a shape's: a scale inside a menu item, the item's pointer events forwarded to it,
 * its activation blocked so the menu stays open. One undo record for the whole menu, taken
 * when it opens and written when it closes if anything moved. */

typedef enum dt_canvas_menu_property_t
{
  DT_CANVAS_MENU_FEATHER = 0,
  DT_CANVAS_MENU_OPACITY,
  DT_CANVAS_MENU_SIZE,
  DT_CANVAS_MENU_ROTATION,
  DT_CANVAS_MENU_CURVATURE,
  DT_CANVAS_MENU_EXTENT,
} dt_canvas_menu_property_t;

static void _menu_slider_block_activate(GtkWidget *item, gpointer data)
{
  g_signal_stop_emission_by_name(item, "activate");
}

static gboolean _menu_slider_forward_event(GtkWidget *item, GdkEvent *event, gpointer data)
{
  GtkWidget *scale = GTK_WIDGET(data);
  GdkWindow *scale_window = gtk_widget_get_window(scale);
  if(IS_NULL_PTR(scale_window)) return FALSE;
  // The event's coordinates are the item's: move them into the scale's window.
  GtkAllocation allocation;
  gtk_widget_get_allocation(scale, &allocation);
  GdkEvent *copy = gdk_event_copy(event);
  double *x = NULL;
  double *y = NULL;
  if(copy->type == GDK_BUTTON_PRESS || copy->type == GDK_BUTTON_RELEASE || copy->type == GDK_2BUTTON_PRESS)
  {
    x = &copy->button.x;
    y = &copy->button.y;
  }
  else if(copy->type == GDK_MOTION_NOTIFY)
  {
    x = &copy->motion.x;
    y = &copy->motion.y;
  }
  else if(copy->type == GDK_SCROLL)
  {
    x = &copy->scroll.x;
    y = &copy->scroll.y;
  }
  if(!IS_NULL_PTR(x))
  {
    gint item_x = 0;
    gint item_y = 0;
    gtk_widget_translate_coordinates(item, scale, (gint)*x, (gint)*y, &item_x, &item_y);
    *x = item_x;
    *y = item_y;
  }
  if(!IS_NULL_PTR(copy->any.window)) g_object_unref(copy->any.window);
  copy->any.window = g_object_ref(scale_window);
  copy->any.send_event = TRUE;
  gtk_widget_event(scale, copy);
  gdk_event_free(copy);
  return TRUE;
}

static void _menu_slider_changed(GtkRange *range, gpointer data)
{
  dt_canvas_menu_context_t *context = (dt_canvas_menu_context_t *)data;
  dt_canvas_view_t *view = (dt_canvas_view_t *)context->self->data;
  dt_canvas_object_t *object = _menu_object(context);
  if(!dt_canvas_object_is_frame(object)) return;
  const float value = (float)gtk_range_get_value(range);
  switch((dt_canvas_menu_property_t)context->value)
  {
    case DT_CANVAS_MENU_FEATHER:
      object->mask.feather = CLAMP(value / 100.0f, 0.0f, 1.0f);
      break;
    case DT_CANVAS_MENU_OPACITY:
      object->transparency = 1.0f - CLAMP(value / 100.0f, 0.0f, 1.0f);
      break;
    case DT_CANVAS_MENU_SIZE:
    {
      // Both radii scale together, so an ellipse keeps its shape.
      const float scale = object->mask.radius_x > 0.0f ? value / 100.0f / object->mask.radius_x : 1.0f;
      object->mask.radius_x = CLAMP(value / 100.0f, 0.005f, 2.0f);
      if(object->mask.shape == DT_CANVAS_MASK_ELLIPSE) object->mask.radius_y = CLAMP(object->mask.radius_y * scale, 0.005f, 2.0f);
      break;
    }
    case DT_CANVAS_MENU_ROTATION:
      object->mask.rotation = value;
      break;
    case DT_CANVAS_MENU_CURVATURE:
      object->mask.radius_y = CLAMP(value, -2.0f, 2.0f);
      break;
    case DT_CANVAS_MENU_EXTENT:
      object->mask.radius_x = CLAMP(value / 100.0f, 0.0005f, 1.0f);
      break;
    default:
      break;
  }
  dt_canvas_touch(view->canvas);
  _props_sync(context->self);
  dt_control_queue_redraw_center();
}

static GtkWidget *_menu_slider(GtkWidget *menu, const char *label, const double low, const double high,
                               const double step, const double value, dt_canvas_menu_context_t *context)
{
  GtkWidget *item = gtk_menu_item_new();
  gtk_widget_set_can_focus(item, FALSE);
  g_signal_connect(item, "activate", G_CALLBACK(_menu_slider_block_activate), NULL);
  gtk_widget_add_events(item, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(6));
  GtkWidget *name = gtk_label_new(label);
  gtk_widget_set_size_request(name, DT_PIXEL_APPLY_DPI(80), -1);
  gtk_widget_set_halign(name, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(box), name, FALSE, FALSE, 0);
  GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, low, high, step);
  gtk_scale_set_draw_value(GTK_SCALE(scale), TRUE);
  gtk_scale_set_value_pos(GTK_SCALE(scale), GTK_POS_RIGHT);
  gtk_range_set_value(GTK_RANGE(scale), value);
  gtk_widget_set_size_request(scale, DT_PIXEL_APPLY_DPI(220), -1);
  gtk_widget_set_hexpand(scale, TRUE);
  gtk_box_pack_start(GTK_BOX(box), scale, TRUE, TRUE, 0);
  gtk_container_add(GTK_CONTAINER(item), box);
  g_object_set_data_full(G_OBJECT(item), "canvas-context", context, g_free);
  g_signal_connect(scale, "value-changed", G_CALLBACK(_menu_slider_changed), context);
  g_signal_connect(item, "button-press-event", G_CALLBACK(_menu_slider_forward_event), scale);
  g_signal_connect(item, "button-release-event", G_CALLBACK(_menu_slider_forward_event), scale);
  g_signal_connect(item, "motion-notify-event", G_CALLBACK(_menu_slider_forward_event), scale);
  g_signal_connect(item, "scroll-event", G_CALLBACK(_menu_slider_forward_event), scale);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
  return item;
}

/** A title line in a menu: what the menu is about, not something to click. */
static void _menu_title(GtkWidget *menu, const char *text)
{
  GtkWidget *item = gtk_menu_item_new();
  GtkWidget *label = gtk_label_new(NULL);
  gchar *markup = g_markup_printf_escaped("<b>%s</b>", text);
  gtk_label_set_markup(GTK_LABEL(label), markup);
  dt_free(markup);
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_container_add(GTK_CONTAINER(item), label);
  gtk_widget_set_sensitive(item, FALSE);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

/** The menu closed: one undo record for whatever its sliders moved. */
static void _menu_closed(GtkWidget *menu, gpointer data)
{
  dt_view_t *self = (dt_view_t *)data;
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view->menu_snapshot)) return;
  if(view->menu_snapshot->generation != view->canvas->generation)
    _record_undo(self, view->menu_snapshot);
  else
    dt_canvas_free(view->menu_snapshot);
  view->menu_snapshot = NULL;
}

static void _menu_cutout_shape(GtkWidget *widget, gpointer data)
{
  dt_canvas_menu_context_t *context = (dt_canvas_menu_context_t *)data;
  dt_canvas_view_t *view = (dt_canvas_view_t *)context->self->data;
  dt_canvas_object_t *object = _menu_object(context);
  if(!dt_canvas_object_is_frame(object)) return;
  dt_canvas_t *before = _begin_edit(view);
  dt_canvas_mask_set_shape(view->canvas, object, (uint32_t)CLAMP(context->value, 0, DT_CANVAS_MASK_GRADIENT));
  view->mask_editing = object->mask.shape != DT_CANVAS_MASK_NONE;
  _record_undo(context->self, before);
  _props_sync(context->self);
  dt_control_queue_redraw_center();
}

static void _menu_cutout_edit(GtkWidget *widget, gpointer data)
{
  dt_canvas_menu_context_t *context = (dt_canvas_menu_context_t *)data;
  dt_canvas_view_t *view = (dt_canvas_view_t *)context->self->data;
  view->mask_editing = context->value != 0;
  _props_sync(context->self);
  dt_control_queue_redraw_center();
}

static void _menu_cutout_invert(GtkWidget *widget, gpointer data)
{
  dt_canvas_menu_context_t *context = (dt_canvas_menu_context_t *)data;
  dt_canvas_view_t *view = (dt_canvas_view_t *)context->self->data;
  dt_canvas_object_t *object = _menu_object(context);
  if(!dt_canvas_object_is_frame(object)) return;
  dt_canvas_t *before = _begin_edit(view);
  object->mask.flags ^= DT_CANVAS_MASK_INVERT;
  dt_canvas_touch(view->canvas);
  _record_undo(context->self, before);
  // The properties carry this same flag as a toggle, and they show what they last read.
  _props_sync(context->self);
  dt_control_queue_redraw_center();
}

/** Polygon nodes from the menu: `value` is the node (or the edge's first node) the menu was opened on. */
static void _menu_cutout_add_node(GtkWidget *widget, gpointer data)
{
  dt_canvas_menu_context_t *context = (dt_canvas_menu_context_t *)data;
  dt_canvas_view_t *view = (dt_canvas_view_t *)context->self->data;
  dt_canvas_object_t *object = _menu_object(context);
  if(!dt_canvas_object_is_frame(object)) return;
  double local_x = 0.0;
  double local_y = 0.0;
  dt_canvas_object_to_local(object, context->x, context->y, &local_x, &local_y);
  dt_canvas_t *before = _begin_edit(view);
  if(dt_canvas_mask_insert_node(view->canvas, object, (uint32_t)context->value + 1,
                                (float)(local_x / object->width + 0.5), (float)(local_y / object->height + 0.5)))
    _record_undo(context->self, before);
  else
    dt_canvas_free(before);
  dt_control_queue_redraw_center();
}

static void _menu_cutout_remove_node(GtkWidget *widget, gpointer data)
{
  dt_canvas_menu_context_t *context = (dt_canvas_menu_context_t *)data;
  dt_canvas_view_t *view = (dt_canvas_view_t *)context->self->data;
  dt_canvas_object_t *object = _menu_object(context);
  if(!dt_canvas_object_is_frame(object)) return;
  dt_canvas_t *before = _begin_edit(view);
  if(dt_canvas_mask_remove_node(view->canvas, object, (uint32_t)context->value))
    _record_undo(context->self, before);
  else
    dt_canvas_free(before);
  dt_control_queue_redraw_center();
}

/** Write a node's effective control points down as its own, so a change of kind moves nothing. */
static void _mask_node_freeze_controls(dt_canvas_mask_t *mask, const uint32_t index)
{
  float *node = mask->nodes + (size_t)index * DT_CANVAS_MASK_NODE_FLOATS;
  float incoming[2];
  float outgoing[2];
  dt_canvas_mask_node_controls(mask, index, incoming, outgoing);
  node[DT_CANVAS_MASK_NODE_CTRL1_X] = incoming[0];
  node[DT_CANVAS_MASK_NODE_CTRL1_Y] = incoming[1];
  node[DT_CANVAS_MASK_NODE_CTRL2_X] = outgoing[0];
  node[DT_CANVAS_MASK_NODE_CTRL2_Y] = outgoing[1];
}

/**
 * A node between its two kinds. Becoming a cusp keeps the curve that was there: the tangent
 * it had, computed or steered, is written down first, and the two sides are free of each
 * other from then on. Becoming smooth hands the tangent back to the neighbours, which is the
 * change the user asked for. The context menu and the double click share this so the two
 * cannot drift apart.
 */
static void _mask_node_toggle_smooth(dt_canvas_mask_t *mask, const uint32_t index)
{
  float *node = mask->nodes + (size_t)index * DT_CANVAS_MASK_NODE_FLOATS;
  const gboolean smooth = node[DT_CANVAS_MASK_NODE_SMOOTH] != (float)DT_CANVAS_MASK_NODE_CUSP;
  if(smooth) _mask_node_freeze_controls(mask, index);
  node[DT_CANVAS_MASK_NODE_SMOOTH] = (float)(smooth ? DT_CANVAS_MASK_NODE_CUSP : DT_CANVAS_MASK_NODE_AUTO);
}

static void _menu_cutout_smooth_node(GtkWidget *widget, gpointer data)
{
  dt_canvas_menu_context_t *context = (dt_canvas_menu_context_t *)data;
  dt_canvas_view_t *view = (dt_canvas_view_t *)context->self->data;
  dt_canvas_object_t *object = _menu_object(context);
  if(!dt_canvas_object_is_frame(object) || object->mask.shape != DT_CANVAS_MASK_POLYGON) return;
  if(context->value < 0 || (uint32_t)context->value >= object->mask.node_count) return;
  dt_canvas_t *before = _begin_edit(view);
  _mask_node_toggle_smooth(&object->mask, (uint32_t)context->value);
  dt_canvas_touch(view->canvas);
  _record_undo(context->self, before);
  dt_control_queue_redraw_center();
}

/** A steered node back to the tangent its neighbours give it. */
static void _menu_cutout_reset_node(GtkWidget *widget, gpointer data)
{
  dt_canvas_menu_context_t *context = (dt_canvas_menu_context_t *)data;
  dt_canvas_view_t *view = (dt_canvas_view_t *)context->self->data;
  dt_canvas_object_t *object = _menu_object(context);
  if(!dt_canvas_object_is_frame(object) || object->mask.shape != DT_CANVAS_MASK_POLYGON) return;
  if(context->value < 0 || (uint32_t)context->value >= object->mask.node_count) return;
  dt_canvas_t *before = _begin_edit(view);
  float *node = object->mask.nodes + (size_t)context->value * DT_CANVAS_MASK_NODE_FLOATS;
  node[DT_CANVAS_MASK_NODE_SMOOTH] = (float)DT_CANVAS_MASK_NODE_AUTO;
  dt_canvas_touch(view->canvas);
  _record_undo(context->self, before);
  dt_control_queue_redraw_center();
}

static void _menu_rotate(GtkWidget *widget, gpointer data)
{
  dt_canvas_menu_context_t *context = (dt_canvas_menu_context_t *)data;
  dt_canvas_view_t *view = (dt_canvas_view_t *)context->self->data;
  dt_canvas_object_t *object = _menu_object(context);
  if(!dt_canvas_object_is_frame(object)) return;
  dt_canvas_t *before = _begin_edit(view);
  if(context->value == 0)
    object->rotation = 0.0;
  else
    object->rotation += context->value * M_PI / 2.0;
  dt_canvas_touch(view->canvas);
  _record_undo(context->self, before);
  // The properties show the angle, and they show what they last read.
  _props_sync(context->self);
  dt_control_queue_redraw_center();
}

static void _menu_duplicate(GtkWidget *widget, gpointer data)
{
  dt_canvas_menu_context_t *context = (dt_canvas_menu_context_t *)data;
  dt_canvas_view_t *view = (dt_canvas_view_t *)context->self->data;
  dt_canvas_t *before = _begin_edit(view);
  dt_canvas_object_t *copy = dt_canvas_duplicate_object(view->canvas, context->object_id);
  if(IS_NULL_PTR(copy))
  {
    dt_canvas_free(before);
    return;
  }
  _select_only(view, copy->id);
  _record_undo(context->self, before);
  _props_sync(context->self);
  dt_control_queue_redraw_center();
}

static void _menu_delete(GtkWidget *widget, gpointer data)
{
  dt_canvas_menu_context_t *context = (dt_canvas_menu_context_t *)data;
  dt_canvas_view_t *view = (dt_canvas_view_t *)context->self->data;
  if(!_is_selected(view, context->object_id)) _select_only(view, context->object_id);
  _delete_selection(context->self);
}

static void _menu_add_text_here(GtkWidget *widget, gpointer data)
{
  dt_canvas_menu_context_t *context = (dt_canvas_menu_context_t *)data;
  _add_text_frame(context->self, context->x, context->y);
}

/** Ask for an SVG file and place it at a point, at the size the file states. */
static void _add_drawing(dt_view_t *self, const double x, const double y)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  GtkWidget *chooser = gtk_file_chooser_dialog_new(_("Place a drawing"), GTK_WINDOW(dt_ui_main_window(dt_gui_get_ui())),
                                                   GTK_FILE_CHOOSER_ACTION_OPEN, _("Cancel"), GTK_RESPONSE_CANCEL,
                                                   _("Place"), GTK_RESPONSE_ACCEPT, NULL);
  GtkFileFilter *filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, _("Drawings (SVG)"));
  gtk_file_filter_add_pattern(filter, "*.svg");
  gtk_file_filter_add_pattern(filter, "*.SVG");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), filter);
  gchar *chosen = NULL;
  if(gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT)
    chosen = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
  GtkWindow *parent = gtk_window_get_transient_for(GTK_WINDOW(chooser));
  gtk_widget_destroy(chooser);
  dt_gui_refocus_parent(parent);
  if(IS_NULL_PTR(chosen)) return;

  dt_canvas_t *before = _begin_edit(view);
  GError *error = NULL;
  dt_canvas_object_t *drawing
      = dt_canvas_add_svg(view->canvas, dt_canvas_snap(view->canvas, x), dt_canvas_snap(view->canvas, y), chosen,
                          &error);
  dt_free(chosen);
  if(IS_NULL_PTR(drawing))
  {
    dt_canvas_free(before);
    dt_control_log(_("cannot read that drawing: %s"), IS_NULL_PTR(error) ? _("unknown reason") : error->message);
    g_clear_error(&error);
    return;
  }
  _select_only(view, drawing->id);
  _record_undo(self, before);
  _props_sync(self);
  dt_control_queue_redraw_center();
}

static void _menu_add_drawing_here(GtkWidget *widget, gpointer data)
{
  dt_canvas_menu_context_t *context = (dt_canvas_menu_context_t *)data;
  _add_drawing(context->self, context->x, context->y);
}

/**
 * Read the drawing's file again, for when it has been edited since it was placed. The context
 * menu asks for it, and so does a double click on a drawing whose properties are showing.
 */
static void _reload_drawing(dt_view_t *self, dt_canvas_object_t *object)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(object) || object->kind != DT_CANVAS_OBJECT_SVG) return;
  dt_canvas_t *before = _begin_edit(view);
  GError *error = NULL;
  const gboolean changed = dt_canvas_svg_reload(view->canvas, object, &error);
  if(!IS_NULL_PTR(error))
  {
    dt_canvas_free(before);
    dt_control_log(_("cannot read that drawing again: %s"), error->message);
    g_clear_error(&error);
    return;
  }
  if(!changed)
  {
    dt_canvas_free(before);
    dt_control_log(_("the drawing has not changed since it was placed"));
    return;
  }
  _record_undo(self, before);
  _props_sync(self);
  dt_control_queue_redraw_center();
}

static void _menu_reload_drawing(GtkWidget *widget, gpointer data)
{
  dt_canvas_menu_context_t *context = (dt_canvas_menu_context_t *)data;
  _reload_drawing(context->self, _menu_object(context));
}

static void _menu_zoom_fit(GtkWidget *widget, gpointer data)
{
  dt_canvas_menu_context_t *context = (dt_canvas_menu_context_t *)data;
  _zoom_fit((dt_canvas_view_t *)context->self->data);
  dt_control_queue_redraw_center();
}

static void _popup_menu(dt_view_t *self, dt_canvas_object_t *object, const double x, const double y)
{
  GtkWidget *menu = gtk_menu_new();
  const uint32_t id = IS_NULL_PTR(object) ? 0 : object->id;
  // What an object contains is also a double click away once its properties are showing, and
  // Return reaches it from the keyboard: the items that open it say both.
  gchar *return_label = gtk_accelerator_get_label(GDK_KEY_Return, 0);
  const char *drill_hint = _("Double-clicking the object while its properties are showing does the same.");
  if(!IS_NULL_PTR(object))
  {
    gchar *properties_label = _accel_label(CANVAS_ACCEL_PROPERTIES);
    _menu_item_with_shortcut(menu, _("Properties"), properties_label,
                             _("Double-clicking an object shows its properties too."), _menu_properties,
                             _menu_context(self, id, x, y, 0));
    dt_free(properties_label);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
  }

  if(IS_NULL_PTR(object))
  {
    _menu_item(menu, _("Add a text frame here"), _menu_add_text_here, _menu_context(self, 0, x, y, 0));
    _menu_item(menu, _("Place a drawing here..."), _menu_add_drawing_here, _menu_context(self, 0, x, y, 0));
    _menu_item(menu, _("Fit the view to the canvas"), _menu_zoom_fit, _menu_context(self, 0, x, y, 0));
  }
  else if(object->kind == DT_CANVAS_OBJECT_CONNECTOR)
  {
    // Everything else it has is in its properties (I); here is what it shares with the frames.
    GtkWidget *order_item = gtk_menu_item_new_with_label(_("Order"));
    GtkWidget *order_menu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(order_item), order_menu);
    _menu_item(order_menu, _("Bring to front"), _menu_z_order, _menu_context(self, id, x, y, 0));
    _menu_item(order_menu, _("Bring forward"), _menu_z_order, _menu_context(self, id, x, y, 1));
    _menu_item(order_menu, _("Send backward"), _menu_z_order, _menu_context(self, id, x, y, 2));
    _menu_item(order_menu, _("Send to back"), _menu_z_order, _menu_context(self, id, x, y, 3));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), order_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    _menu_item(menu, _("Delete"), _menu_delete, _menu_context(self, id, x, y, 0));
  }
  else
  {
    if(object->kind == DT_CANVAS_OBJECT_TEXT)
    {
      _menu_item_with_shortcut(menu, _("Edit the text..."), return_label, drill_hint, _menu_edit_text,
                               _menu_context(self, id, x, y, 0));
      _menu_item(menu, _("Fit the frame to the text"), _menu_fit_text, _menu_context(self, id, x, y, 0));
      if(object->text.source == DT_CANVAS_TEXT_SOURCE_SIDECAR)
        _menu_item(menu, _("Reload the image's text note"), _menu_reload_sidecar, _menu_context(self, id, x, y, 0));
    }
    else if(object->kind == DT_CANVAS_OBJECT_MAP)
    {
      _menu_item(menu, _("Fetch the map again"), _menu_refresh_image, _menu_context(self, id, x, y, 0));
    }
    else if(object->kind == DT_CANVAS_OBJECT_SVG)
    {
      _menu_item_with_shortcut(menu, _("Read the drawing's file again"), return_label, drill_hint,
                               _menu_reload_drawing, _menu_context(self, id, x, y, 0));
    }
    else
    {
      _menu_item_with_shortcut(menu, _("Open in the darkroom"), return_label, drill_hint, _menu_open_darkroom,
                               _menu_context(self, id, x, y, 0));
      _menu_item(menu, _("Refresh from the library"), _menu_refresh_image, _menu_context(self, id, x, y, 0));
      _menu_item(menu, _("Show the image's text note"), _menu_show_note, _menu_context(self, id, x, y, 0));
      _menu_item(menu, _("Add a map of where it was taken"), _menu_map_of_image, _menu_context(self, id, x, y, 0));
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    GtkWidget *order_item = gtk_menu_item_new_with_label(_("Order"));
    GtkWidget *order_menu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(order_item), order_menu);
    _menu_item(order_menu, _("Bring to front"), _menu_z_order, _menu_context(self, id, x, y, 0));
    _menu_item(order_menu, _("Bring forward"), _menu_z_order, _menu_context(self, id, x, y, 1));
    _menu_item(order_menu, _("Send backward"), _menu_z_order, _menu_context(self, id, x, y, 2));
    _menu_item(order_menu, _("Send to back"), _menu_z_order, _menu_context(self, id, x, y, 3));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), order_item);

    dt_canvas_view_t *canvas_view = (dt_canvas_view_t *)self->data;
    // The cutout: its shape, editing it, and the node under the pointer when there is one.
    GtkWidget *cutout_item = gtk_menu_item_new_with_label(_("Cutout"));
    GtkWidget *cutout_menu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(cutout_item), cutout_menu);
    _menu_item(cutout_menu, _("None"), _menu_cutout_shape, _menu_context(self, id, x, y, DT_CANVAS_MASK_NONE));
    _menu_item(cutout_menu, _("Circle"), _menu_cutout_shape, _menu_context(self, id, x, y, DT_CANVAS_MASK_CIRCLE));
    _menu_item(cutout_menu, _("Ellipse"), _menu_cutout_shape, _menu_context(self, id, x, y, DT_CANVAS_MASK_ELLIPSE));
    _menu_item(cutout_menu, _("Polygon"), _menu_cutout_shape, _menu_context(self, id, x, y, DT_CANVAS_MASK_POLYGON));
    _menu_item(cutout_menu, _("Gradient"), _menu_cutout_shape, _menu_context(self, id, x, y, DT_CANVAS_MASK_GRADIENT));
    if(object->mask.shape != DT_CANVAS_MASK_NONE && canvas_view->mask_editing)
    {
      // Editing: the shape's properties as sliders, at the top of the main menu, the darkroom's way.
      static const char *shape_names[] = { "", N_("Circle"), N_("Ellipse"), N_("Polygon"), N_("Gradient") };
      int hovered_index = -1;
      const dt_canvas_drag_t hovered = _mask_handle_at(canvas_view, object, x, y, &hovered_index);
      gchar *title = hovered == DT_CANVAS_DRAG_MASK_NODE
                         ? g_strdup_printf(_("%s cutout, node %d"), _(shape_names[CLAMP(object->mask.shape, 0, 4)]), hovered_index)
                         : g_strdup_printf(_("%s cutout"), _(shape_names[CLAMP(object->mask.shape, 0, 4)]));
      _menu_title(menu, title);
      dt_free(title);
      if(IS_NULL_PTR(canvas_view->menu_snapshot)) canvas_view->menu_snapshot = _begin_edit(canvas_view);
      g_signal_connect(menu, "deactivate", G_CALLBACK(_menu_closed), self);
      if(object->mask.shape != DT_CANVAS_MASK_GRADIENT)
        _menu_slider(menu, _("Feather"), 0.0, 100.0, 1.0, object->mask.feather * 100.0,
                     _menu_context(self, id, x, y, DT_CANVAS_MENU_FEATHER));
      _menu_slider(menu, _("Opacity"), 0.0, 100.0, 1.0, (1.0 - object->transparency) * 100.0,
                   _menu_context(self, id, x, y, DT_CANVAS_MENU_OPACITY));
      if(object->mask.shape == DT_CANVAS_MASK_CIRCLE || object->mask.shape == DT_CANVAS_MASK_ELLIPSE)
        _menu_slider(menu, _("Size"), 0.5, 200.0, 0.5, object->mask.radius_x * 100.0,
                     _menu_context(self, id, x, y, DT_CANVAS_MENU_SIZE));
      if(object->mask.shape == DT_CANVAS_MASK_ELLIPSE || object->mask.shape == DT_CANVAS_MASK_GRADIENT)
        _menu_slider(menu, _("Rotation"), -180.0, 180.0, 1.0, object->mask.rotation,
                     _menu_context(self, id, x, y, DT_CANVAS_MENU_ROTATION));
      if(object->mask.shape == DT_CANVAS_MASK_GRADIENT)
      {
        _menu_slider(menu, _("Extent"), 0.05, 100.0, 0.5, object->mask.radius_x * 100.0,
                     _menu_context(self, id, x, y, DT_CANVAS_MENU_EXTENT));
        _menu_slider(menu, _("Curvature"), -2.0, 2.0, 0.05, object->mask.radius_y,
                     _menu_context(self, id, x, y, DT_CANVAS_MENU_CURVATURE));
      }
      // A polygon's nodes: the node or the edge under the pointer, right here in the menu.
      if(object->mask.shape == DT_CANVAS_MASK_POLYGON)
      {
        const int node_here = _mask_node_at(canvas_view, object, x, y);
        const int segment = node_here < 0 ? _mask_segment_at(canvas_view, object, x, y) : -1;
        if(node_here >= 0)
        {
          const float *node = object->mask.nodes + (size_t)node_here * DT_CANVAS_MASK_NODE_FLOATS;
          const gboolean smooth = node[DT_CANVAS_MASK_NODE_SMOOTH] != (float)DT_CANVAS_MASK_NODE_CUSP;
          _menu_item(menu, smooth ? _("Switch to a cusp node") : _("Switch to a smooth node"),
                     _menu_cutout_smooth_node, _menu_context(self, id, x, y, node_here));
          if(node[DT_CANVAS_MASK_NODE_SMOOTH] == (float)DT_CANVAS_MASK_NODE_STEERED)
            _menu_item(menu, _("Give this node its computed curve back"), _menu_cutout_reset_node,
                       _menu_context(self, id, x, y, node_here));
          _menu_item(menu, _("Remove this node"), _menu_cutout_remove_node, _menu_context(self, id, x, y, node_here));
        }
        else if(segment >= 0)
        {
          _menu_item(menu, _("Add a node here"), _menu_cutout_add_node, _menu_context(self, id, x, y, segment));
        }
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
      }
    }
    if(object->mask.shape != DT_CANVAS_MASK_NONE)
    {
      gtk_menu_shell_append(GTK_MENU_SHELL(cutout_menu), gtk_separator_menu_item_new());
      _menu_item(cutout_menu, canvas_view->mask_editing ? _("Stop editing the shape") : _("Edit the shape"),
                 _menu_cutout_edit, _menu_context(self, id, x, y, canvas_view->mask_editing ? 0 : 1));
      _menu_item(cutout_menu, (object->mask.flags & DT_CANVAS_MASK_INVERT) ? _("Keep the inside") : _("Keep the outside"),
                 _menu_cutout_invert, _menu_context(self, id, x, y, 0));
      // The node entries live once, at the top of the menu, and only while the shape is being
      // edited: this submenu used to carry a second copy of them keyed on `_mask_handle_at()`,
      // which answers NOTHING outside the edit mode -- so they were a duplicate whenever they
      // showed and dead the rest of the time. What a polygon offers here is the way in.
      if(object->mask.shape == DT_CANVAS_MASK_POLYGON && !canvas_view->mask_editing
         && _mask_node_at(canvas_view, object, x, y) >= 0)
      {
        gtk_menu_shell_append(GTK_MENU_SHELL(cutout_menu), gtk_separator_menu_item_new());
        _menu_item(cutout_menu, _("Edit the shape to work on this node"), _menu_cutout_edit,
                   _menu_context(self, id, x, y, 1));
      }
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), cutout_item);

    GtkWidget *rotate_item = gtk_menu_item_new_with_label(_("Rotate"));
    GtkWidget *rotate_menu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(rotate_item), rotate_menu);
    _menu_item(rotate_menu, _("90° clockwise"), _menu_rotate, _menu_context(self, id, x, y, 1));
    _menu_item(rotate_menu, _("90° counter-clockwise"), _menu_rotate, _menu_context(self, id, x, y, -1));
    _menu_item(rotate_menu, _("Reset the rotation"), _menu_rotate, _menu_context(self, id, x, y, 0));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), rotate_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    _menu_item(menu, _("Duplicate"), _menu_duplicate, _menu_context(self, id, x, y, 0));
    _menu_item(menu, _("Delete"), _menu_delete, _menu_context(self, id, x, y, 0));
  }
  dt_free(return_label);
  gtk_widget_show_all(menu);
  gtk_menu_popup_at_pointer(GTK_MENU(menu), NULL);
}

/**
 * Ctrl held during a handle's drag keeps it on one axis: the one it has travelled furthest
 * along since the press, so the user chooses which by moving. It applies to the handles that
 * are free to go anywhere -- a cutout's, a connector's waypoint and its tangents -- while a
 * handle already confined to a line, like a connector's reach along its anchor's normal, has
 * nothing to constrain. A frame's rotation reads it as 45 degree steps instead, which is the
 * same request answered in the only way an angle can answer it: the two axes, and the two
 * diagonals between them.
 */
static void _axis_lock(const dt_canvas_view_t *view, const int state, double *x, double *y)
{
  if(!dt_modifier_is(state, DT_PRIMARY_MASK)) return;
  if(fabs(*x - view->press_x) >= fabs(*y - view->press_y))
    *y = view->press_y;
  else
    *x = view->press_x;
}

/**
 * Ctrl on a handle that sets a DIRECTION -- a connector's tangents, which are the gradient
 * its curve leaves by -- snaps that direction to 45 degree steps about the point it turns
 * around, keeping how far out the handle was pulled. The axes are among those steps, so this
 * is the same lock the free handles get, said in the terms an angle has.
 */
static void _angle_lock(const int state, const double origin_x, const double origin_y, double *x, double *y)
{
  if(!dt_modifier_is(state, DT_PRIMARY_MASK)) return;
  const double reach = hypot(*x - origin_x, *y - origin_y);
  if(!(reach > 0.0)) return;
  const double step = M_PI / 4.0;
  const double angle = round(atan2(*y - origin_y, *x - origin_x) / step) * step;
  *x = origin_x + cos(angle) * reach;
  *y = origin_y + sin(angle) * reach;
}

/* --- connector drawing mode ---------------------------------------------------------- */

#define CANVAS_ANCHOR_REACH_PIXELS 12.0
#define CANVAS_ANCHOR_DOT_PIXELS 6.0

/** Every point a connector can be attached to, in the order their dots are drawn. */
static const dt_canvas_anchor_t _frame_anchors[9]
    = { DT_CANVAS_ANCHOR_NORTH,      DT_CANVAS_ANCHOR_EAST,       DT_CANVAS_ANCHOR_SOUTH,
        DT_CANVAS_ANCHOR_WEST,       DT_CANVAS_ANCHOR_NORTH_EAST, DT_CANVAS_ANCHOR_SOUTH_EAST,
        DT_CANVAS_ANCHOR_SOUTH_WEST, DT_CANVAS_ANCHOR_NORTH_WEST, DT_CANVAS_ANCHOR_CENTRE };
#define CANVAS_FRAME_ANCHORS ((int)(sizeof(_frame_anchors) / sizeof(_frame_anchors[0])))

/** The nearest anchor of any frame within reach of the canvas point. */
static gboolean _anchor_at(const dt_canvas_view_t *view, const double x, const double y, uint32_t *frame_id,
                           uint32_t *anchor)
{
  const double reach = CANVAS_ANCHOR_REACH_PIXELS / view->zoom;
  double best = reach;
  gboolean found = FALSE;
  for(guint idx = 0; idx < dt_canvas_object_count(view->canvas); idx++)
  {
    const dt_canvas_object_t *object = dt_canvas_object_at(view->canvas, idx);
    if(!dt_canvas_object_is_frame(object) || (object->flags & DT_CANVAS_OBJECT_FLAG_HIDDEN)) continue;
    for(int candidate = 0; candidate < CANVAS_FRAME_ANCHORS; candidate++)
    {
      double anchor_x = 0.0;
      double anchor_y = 0.0;
      dt_canvas_object_anchor_handle(view->canvas, object, _frame_anchors[candidate], &anchor_x, &anchor_y);
      const double distance = hypot(anchor_x - x, anchor_y - y);
      if(distance <= best)
      {
        best = distance;
        *frame_id = object->id;
        *anchor = _frame_anchors[candidate];
        found = TRUE;
      }
    }
  }
  return found;
}

static void _connect_mode_set(dt_view_t *self, gboolean on)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  view->connecting = on;
  view->connect_from = 0;
  view->connect_from_anchor = DT_CANVAS_ANCHOR_AUTO;
  view->anchor_hover_id = 0;
  view->anchor_hover = DT_CANVAS_ANCHOR_AUTO;
  // Drawing a connector is clicking frames one after the other: properties left open would be
  // one object's while the clicks are about two others.
  if(on) _props_close(self);
  if(on) dt_control_log(_("click an anchor point on the first frame, then one on the second; Escape leaves"));
  _props_sync(self);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_CANVAS_CHANGED);
  dt_control_queue_redraw_center();
}

/** A click in connector mode: pick the source anchor, then the target anchor. */
static void _connect_click(dt_view_t *self, const double x, const double y)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  uint32_t frame_id = 0;
  uint32_t anchor = DT_CANVAS_ANCHOR_AUTO;
  if(!_anchor_at(view, x, y, &frame_id, &anchor)) return;
  if(view->connect_from == 0)
  {
    view->connect_from = frame_id;
    view->connect_from_anchor = anchor;
    dt_control_queue_redraw_center();
    return;
  }
  if(frame_id == view->connect_from) return;
  dt_canvas_t *before = _begin_edit(view);
  dt_canvas_object_t *connector = dt_canvas_add_connector(view->canvas, view->connect_from, frame_id);
  if(IS_NULL_PTR(connector))
  {
    dt_canvas_free(before);
    return;
  }
  connector->connector.from_anchor = view->connect_from_anchor;
  connector->connector.to_anchor = anchor;
  _select_only(view, connector->id);
  _record_undo(self, before);
  _connect_mode_set(self, FALSE);
}

static void _paint_anchor_dots(cairo_t *cr, const dt_canvas_view_t *view, const dt_canvas_object_t *frame,
                               const uint32_t chosen)
{
  if(!dt_canvas_object_is_frame(frame)) return;
  const double radius = CANVAS_ANCHOR_DOT_PIXELS / view->zoom;
  for(int candidate = 0; candidate < CANVAS_FRAME_ANCHORS; candidate++)
  {
    const dt_canvas_anchor_t anchor = _frame_anchors[candidate];
    double anchor_x = 0.0;
    double anchor_y = 0.0;
    dt_canvas_object_anchor_handle(view->canvas, frame, anchor, &anchor_x, &anchor_y);
    const gboolean hovered = frame->id == view->anchor_hover_id && view->anchor_hover == anchor;
    const gboolean picked = chosen == anchor;
    const double dot = hovered ? radius * 1.5 : radius;
    cairo_arc(cr, anchor_x, anchor_y, dot, 0.0, 2.0 * M_PI);
    if(picked)
      cairo_set_source_rgba(cr, 1.0, 0.75, 0.2, 1.0);
    else
      cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, hovered ? 1.0 : 0.85);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.8);
    cairo_set_line_width(cr, 1.0 / view->zoom);
    cairo_stroke(cr);
    if(anchor == DT_CANVAS_ANCHOR_CENTRE)
    {
      // A ring, since this one is not where the route will touch: it is the frame itself,
      // and the route leaves by whichever edge faces the other end.
      cairo_arc(cr, anchor_x, anchor_y, dot * 0.45, 0.0, 2.0 * M_PI);
      cairo_stroke(cr);
    }
  }
}

static void _paint_connect_mode(cairo_t *cr, const dt_canvas_view_t *view)
{
  if(!view->connecting) return;
  cairo_save(cr);
  const dt_canvas_object_t *from = dt_canvas_find_object(view->canvas, view->connect_from);
  if(!IS_NULL_PTR(from))
  {
    double anchor_x = 0.0;
    double anchor_y = 0.0;
    double normal_x = 0.0;
    double normal_y = 0.0;
    dt_canvas_object_anchor_point(view->canvas, from, (dt_canvas_anchor_t)view->connect_from_anchor, 0.0, 0.0, &anchor_x,
                                  &anchor_y, &normal_x, &normal_y);
    if(view->pointer_inside)
    {
      cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.6);
      cairo_set_line_width(cr, 2.0 / view->zoom);
      const double dashes[2] = { 8.0 / view->zoom, 6.0 / view->zoom };
      cairo_set_dash(cr, dashes, 2, 0.0);
      cairo_move_to(cr, anchor_x, anchor_y);
      cairo_line_to(cr, view->pointer_x, view->pointer_y);
      cairo_stroke(cr);
      cairo_set_dash(cr, NULL, 0, 0.0);
    }
    _paint_anchor_dots(cr, view, from, view->connect_from_anchor);
  }
  const dt_canvas_object_t *hovered = dt_canvas_find_object(view->canvas, view->anchor_hover_id);
  if(!IS_NULL_PTR(hovered) && hovered != from) _paint_anchor_dots(cr, view, hovered, DT_CANVAS_ANCHOR_AUTO);
  cairo_restore(cr);
}

/* --- the floating properties ------------------------------------------------------------ */

/**
 * The object whose properties are open, while they still may be: it exists, it is the whole
 * selection and no connector is being drawn. NULL otherwise, whatever `props_id` says --
 * `_props_sync()` closes what this refuses.
 */
static dt_canvas_object_t *_props_object(const dt_canvas_view_t *view)
{
  if(view->props_id == 0 || view->connecting || view->selection->len != 1) return NULL;
  if(g_array_index(view->selection, uint32_t, 0) != view->props_id) return NULL;
  return dt_canvas_find_object(view->canvas, view->props_id);
}

/**
 * The section a kind's properties open on, stored by name, one key per kind. The enum's order is the
 * order sections are shown in and may change; a value in the configuration may not. These names are a
 * storage format, declared with their keys in anselconfig.xml.in: never translate them, never reorder
 * them against dt_canvas_prop_section_t.
 */
static const char *const _props_section_names[] = {
  "character", "paragraph", "text_box", "picture", "drawing", "map", "route",
  "arrange",   "fill",      "stroke",   "corners", "shadow",  "cutout",
};
G_STATIC_ASSERT(G_N_ELEMENTS(_props_section_names) == DT_CANVAS_SECTION_COUNT);
#define CANVAS_PROPS_SECTION_NONE "none"

/** The key a kind's open section is stored under; FALSE for a kind that has no properties. */
static gboolean _props_section_key(const uint32_t kind, char *key, const size_t length)
{
  const char *kind_name = NULL;
  switch(kind)
  {
    case DT_CANVAS_OBJECT_TEXT:
      kind_name = "text";
      break;
    case DT_CANVAS_OBJECT_IMAGE:
      kind_name = "image";
      break;
    case DT_CANVAS_OBJECT_SVG:
      kind_name = "svg";
      break;
    case DT_CANVAS_OBJECT_MAP:
      kind_name = "map";
      break;
    case DT_CANVAS_OBJECT_CONNECTOR:
      kind_name = "connector";
      break;
    default:
      return FALSE;
  }
  g_snprintf(key, length, "plugins/canvas/props/section/%s", kind_name);
  return TRUE;
}

/** The section the user last left open on this kind, -1 for none. */
static int _props_section_stored(const uint32_t kind)
{
  char key[128] = { 0 };
  if(!_props_section_key(kind, key, sizeof(key))) return -1;
  const char *stored = dt_conf_get_string_const(key);
  if(IS_NULL_PTR(stored)) return -1;
  for(int section = 0; section < DT_CANVAS_SECTION_COUNT; section++)
  {
    if(strcmp(stored, _props_section_names[section]) == 0) return section;
  }
  return -1;
}

/** Remember the section the user opened on this kind, or that they folded it: -1. */
static void _props_section_store(const uint32_t kind, const int section)
{
  char key[128] = { 0 };
  if(!_props_section_key(kind, key, sizeof(key))) return;
  const gboolean named = section >= 0 && section < DT_CANVAS_SECTION_COUNT;
  dt_conf_set_string(key, named ? _props_section_names[section] : CANVAS_PROPS_SECTION_NONE);
}

/* --- where the properties go ----------------------------------------------------------- */

/** A canvas point on screen, in the logical pixels configure() and the pointer events use. */
static void _to_screen(const dt_canvas_view_t *view, const double canvas_x, const double canvas_y, double *screen_x,
                       double *screen_y)
{
  *screen_x = (canvas_x - view->center_x) * view->zoom + view->width * 0.5;
  *screen_y = (canvas_y - view->center_y) * view->zoom + view->height * 0.5;
}

/** The widget on screen, NULL outside the atelier. */
static GtkWidget *_props_root(const dt_canvas_view_t *view)
{
  return IS_NULL_PTR(view->props) ? NULL : dt_canvas_props_gtk_root(view->props);
}

/** Hide the properties. A hidden widget is under no pointer, whatever the last crossing said. */
static void _props_hide(dt_canvas_view_t *view)
{
  view->props_pointer_inside = FALSE;
  GtkWidget *root = _props_root(view);
  if(!IS_NULL_PTR(root)) gtk_widget_hide(root);
}

/**
 * Is the user in the properties right now? Then they must not move: a control jumping away from
 * under the pointer, or from under the keys being typed into it, is the one thing a floating panel
 * is never forgiven. The placement waits for the pointer to leave, the digits to be applied or the
 * edit to commit.
 *
 * The keyboard focus alone holds nothing. A slider and a spin button keep the focus once clicked,
 * so a hold on the focus never ends by itself: a click on the + of a width, the pointer back over
 * the canvas, and the properties would stay over the corners the frame had just grown into until
 * the canvas was clicked. What the focus does protect -- digits typed and not applied yet -- is held
 * on its own. A colour or font dialog holds nothing either: it is modal, and the pointer that
 * clicked the button that opened it is still reported inside until the dialog lets go.
 */
static gboolean _props_place_held(const dt_canvas_view_t *view)
{
  GtkWidget *root = _props_root(view);
  if(IS_NULL_PTR(root) || !gtk_widget_get_visible(root)) return FALSE;
  return view->props_pointer_inside || view->props_live || dt_canvas_props_gtk_typing(view->props);
}

/**
 * The main window's focus is about to move. A spin button losing it applies what was typed into it,
 * so a placement held back for the typing can run -- from the idle `_props_sync()` schedules, by which
 * time the focus has moved and the digits are applied.
 */
static void _props_focus_changed(GtkWindow *window, GtkWidget *widget, gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view) || !view->props_place_pending) return;
  _props_sync(self);
}

/**
 * Where the pointer is on screen right now, when it is over the view. Read from the device, not
 * from the last motion: the properties are opened from a context menu too, whose grab told the
 * view the pointer had left, and the item was released somewhere else than the right click that
 * opened the menu -- so neither the flag nor the last motion's point says where it is.
 */
static gboolean _props_pointer_position(const dt_canvas_view_t *view, double *screen_x, double *screen_y)
{
  GtkWidget *center = dt_gui_center_widget();
  GdkWindow *window = IS_NULL_PTR(center) ? NULL : gtk_widget_get_window(center);
  GdkDisplay *display = IS_NULL_PTR(window) ? NULL : gdk_window_get_display(window);
  GdkSeat *seat = IS_NULL_PTR(display) ? NULL : gdk_display_get_default_seat(display);
  GdkDevice *pointer = IS_NULL_PTR(seat) ? NULL : gdk_seat_get_pointer(seat);
  if(IS_NULL_PTR(window) || IS_NULL_PTR(pointer)) return FALSE;
  double pointer_x = 0.0;
  double pointer_y = 0.0;
  gdk_window_get_device_position_double(window, pointer, &pointer_x, &pointer_y, NULL);
  if(pointer_x < 0.0 || pointer_y < 0.0 || pointer_x >= view->width || pointer_y >= view->height) return FALSE;
  *screen_x = pointer_x;
  *screen_y = pointer_y;
  return TRUE;
}

/**
 * Everything the properties must keep clear of, or had better, on screen: the object's handles,
 * its line and its body; what one click in them would add; the flower; at an opening, the pointer
 * that asked for them; and, softly, the other frames, the status line and the toast.
 */
static void _props_shapes(const dt_canvas_view_t *view, const dt_canvas_object_t *object,
                          const dt_canvas_place_reason_t reason, GArray *shapes)
{
  const dt_canvas_place_view_t projection = { .center_x = view->center_x,
                                              .center_y = view->center_y,
                                              .zoom = view->zoom,
                                              .width = view->width,
                                              .height = view->height,
                                              .margin = DT_PIXEL_APPLY_DPI(CANVAS_PROPS_AIR_PIXELS) + 1.0 };
  dt_canvas_place_object_shapes(shapes, &projection, view->canvas, object, view->mask_editing);

  double flower_x = 0.0;
  double flower_y = 0.0;
  _flower_center(view, &flower_x, &flower_y);
  const double flower = DT_PIXEL_APPLY_DPI(CANVAS_FLOWER_RADIUS);
  dt_canvas_place_shape_add(shapes, DT_CANVAS_PLACE_HARD, flower_x - flower, flower_y - flower, flower_x + flower,
                            flower_y + flower, 0.0, FALSE);
  double pointer_x = 0.0;
  double pointer_y = 0.0;
  if(reason == DT_CANVAS_PLACE_OPEN && _props_pointer_position(view, &pointer_x, &pointer_y))
  {
    // Only when they open: they must not appear under the pointer that asked for them. Afterwards
    // it moves on its own, and following it would move them.
    const double pointer = DT_PIXEL_APPLY_DPI(CANVAS_PROPS_POINTER_PIXELS);
    dt_canvas_place_shape_add(shapes, DT_CANVAS_PLACE_HARD, pointer_x - pointer, pointer_y - pointer,
                              pointer_x + pointer, pointer_y + pointer, 0.0, FALSE);
  }

  const dt_canvas_rect_t visible = _visible_rect(view);
  int frames = 0;
  for(guint idx = dt_canvas_object_count(view->canvas); idx > 0 && frames < CANVAS_PROPS_SOFT_FRAMES; idx--)
  {
    const dt_canvas_object_t *other = dt_canvas_object_at(view->canvas, idx - 1);
    if(other == object || !dt_canvas_object_is_frame(other) || (other->flags & DT_CANVAS_OBJECT_FLAG_HIDDEN))
      continue;
    const dt_canvas_rect_t bounds = dt_canvas_object_bounds(other);
    if(bounds.x > visible.x + visible.width || bounds.x + bounds.width < visible.x || bounds.y > visible.y + visible.height
       || bounds.y + bounds.height < visible.y)
      continue;
    double left = 0.0;
    double top = 0.0;
    _to_screen(view, bounds.x, bounds.y, &left, &top);
    dt_canvas_place_shape_add(shapes, DT_CANVAS_PLACE_SOFT, left, top, left + bounds.width * view->zoom,
                              top + bounds.height * view->zoom, DT_CANVAS_PLACE_WEIGHT_FRAME, FALSE);
    frames++;
  }
  // The status line along the bottom left, and the toast at the top in the middle.
  dt_canvas_place_shape_add(shapes, DT_CANVAS_PLACE_SOFT, DT_PIXEL_APPLY_DPI(8), view->height - DT_PIXEL_APPLY_DPI(28),
                            DT_PIXEL_APPLY_DPI(368), view->height - DT_PIXEL_APPLY_DPI(4), DT_CANVAS_PLACE_WEIGHT_BAND,
                            FALSE);
  dt_canvas_place_shape_add(shapes, DT_CANVAS_PLACE_SOFT, view->width * 0.25, 0.0, view->width * 0.75,
                            DT_PIXEL_APPLY_DPI(40), DT_CANVAS_PLACE_WEIGHT_BAND, FALSE);
}

/** Where on screen the properties were asked for: the place remembered on the object, where the object now is. */
static void _props_anchor_screen(const dt_canvas_view_t *view, const dt_canvas_object_t *object, double *screen_x,
                                 double *screen_y)
{
  double canvas_x = object->x;
  double canvas_y = object->y;
  if(dt_canvas_object_is_frame(object))
  {
    const double local_x = (view->props_anchor_u - 0.5) * object->width;
    const double local_y = (view->props_anchor_v - 0.5) * object->height;
    const double cos_r = cos(object->rotation);
    const double sin_r = sin(object->rotation);
    canvas_x = object->x + local_x * cos_r - local_y * sin_r;
    canvas_y = object->y + local_x * sin_r + local_y * cos_r;
  }
  else
  {
    dt_canvas_route_t route;
    if(dt_canvas_connector_route(view->canvas, object, &route))
      dt_canvas_route_point_at(&route, view->props_anchor_t, &canvas_x, &canvas_y);
  }
  _to_screen(view, canvas_x, canvas_y, screen_x, screen_y);
}

#ifdef _DEBUG
/** No HARD shape, grown by the air the placement keeps, overlaps where the properties went. */
static void _props_place_assert(const dt_canvas_view_t *view, const double air)
{
  if(!view->props_place.visible) return;
  const dt_canvas_place_rect_t footprint = dt_canvas_place_footprint(&view->props_place);
  for(guint idx = 0; idx < view->props_shapes->len; idx++)
  {
    const dt_canvas_place_shape_t *shape = &g_array_index(view->props_shapes, dt_canvas_place_shape_t, idx);
    if(shape->shape_class != DT_CANVAS_PLACE_HARD) continue;
    const gboolean overlaps = footprint.x < shape->rect.x + shape->rect.width + air
                              && footprint.x + footprint.width > shape->rect.x - air
                              && footprint.y < shape->rect.y + shape->rect.height + air
                              && footprint.y + footprint.height > shape->rect.y - air;
    g_assert(!overlaps);
  }
}
#endif

/**
 * Tell the widget which side its card is on, how tall it may be and whether it had no room. Only on a
 * change from what it was last told, or when asked to: the widget repaints on every call, and a repaint
 * of an overlay child repaints the canvas under it. Compared with what was last TOLD, not with the last
 * placement, since a placement that found no room tells the widget nothing.
 */
static void _props_card_apply(dt_canvas_view_t *view, const dt_canvas_place_t *place, const gboolean force)
{
  const dt_canvas_place_t *told = &view->props_card_told;
  const gboolean changed = force || !view->props_card_told_valid || place->card_shown != told->card_shown
                           || place->growth != told->growth || place->clipped != told->clipped
                           || lround(place->card_height) != lround(told->card_height);
  if(!changed) return;
  dt_canvas_props_gtk_set_card(view->props, place->card_shown, place->growth == DT_CANVAS_PLACE_UP,
                               (int)lround(place->card_height), place->clipped);
  view->props_card_told = *place;
  view->props_card_told_valid = TRUE;
}

/**
 * Place the properties: build what they must avoid, measure them, solve, tell the card where it goes
 * and move the overlay child only when the rectangle changed. Runs from idles and from the card's and
 * the sections' own handlers, never from a draw or a motion: moving an overlay child from inside a
 * draw glitches, and per motion it re-allocates the overlay. Held back while the user is in them,
 * unless they are opening or the user just asked for the card: that one grows from where the strip
 * already is, which is the whole point of GROW.
 */
static void _props_place(dt_view_t *self, const dt_canvas_place_reason_t reason)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  const dt_canvas_object_t *object = _props_object(view);
  GtkWidget *root = _props_root(view);
  if(IS_NULL_PTR(root) || IS_NULL_PTR(object) || view->width <= 0 || view->height <= 0) return;
  if(reason == DT_CANVAS_PLACE_RESOLVE && _props_place_held(view))
  {
    view->props_place_pending = TRUE;
    return;
  }
  view->props_place_pending = FALSE;
  if(IS_NULL_PTR(view->props_shapes)) view->props_shapes = g_array_new(FALSE, FALSE, sizeof(dt_canvas_place_shape_t));
  g_array_set_size(view->props_shapes, 0);
  _props_shapes(view, object, reason, view->props_shapes);

  const double air = DT_PIXEL_APPLY_DPI(CANVAS_PROPS_AIR_PIXELS);
  dt_canvas_place_input_t input;
  memset(&input, 0, sizeof(input));
  input.view.x = air;
  input.view.y = air;
  input.view.width = view->width - 2.0 * air;
  input.view.height = view->height - 2.0 * air;
  input.air = air;
  input.shapes = (const dt_canvas_place_shape_t *)view->props_shapes->data;
  input.shape_count = view->props_shapes->len;
  _props_anchor_screen(view, object, &input.anchor_x, &input.anchor_y);
  input.has_press = view->click_sequence.press_count > 0;
  input.press_x = view->press_screen_x;
  input.press_y = view->press_screen_y;
  // One width for the strip and the card, and no zoom changes it. In a view narrower than that, the
  // properties are placed as if they were as wide as the view and the overlay cuts off what does not
  // fit: hiding them there, with a toast telling the user to zoom out, asked for what could not help.
  const int widget_width = dt_canvas_props_gtk_strip_width(view->props);
  const int width = MAX(MIN(widget_width, (int)floor(input.view.width)), 1);
  int strip_height = 0;
  int card_content_height = 0;
  dt_canvas_props_gtk_measure(view->props, width, &strip_height, &card_content_height);
  input.width = width;
  input.strip_height = MIN((double)strip_height, input.view.height);
  input.card_open = view->props_card_open;
  input.card_content_height = card_content_height;
  input.card_min = DT_PIXEL_APPLY_DPI(CANVAS_PROPS_CARD_MIN_PIXELS);
  const uint32_t kind = MIN(object->kind, (uint32_t)(G_N_ELEMENTS(view->props_last_card_height) - 1));
  input.last_card_height = view->props_last_card_height[kind];
  input.reason = reason;
  input.previous = view->props_placed ? &view->props_place : NULL;
  dt_canvas_place_t place;
  dt_canvas_place_solve(&input, &place);
  if(place.visible && place.card_shown) view->props_last_card_height[kind] = place.card_height;

  const gboolean was_shown = gtk_widget_get_visible(root);
  const dt_canvas_place_rect_t old_footprint = dt_canvas_place_footprint(&view->props_place);
  const dt_canvas_place_rect_t new_footprint = dt_canvas_place_footprint(&place);
  const gboolean moved = !view->props_placed || place.strip.x != view->props_place.strip.x
                         || place.strip.y != view->props_place.strip.y
                         || place.strip.width != view->props_place.strip.width
                         || place.strip.height != view->props_place.strip.height
                         || new_footprint.y != old_footprint.y || new_footprint.height != old_footprint.height;
  // The user's own click on the card button always hears back: the button shows what is on screen, and
  // a click the placement answered with the same clipped card must still put the button back up.
  if(place.visible) _props_card_apply(view, &place, reason != DT_CANVAS_PLACE_RESOLVE);
  view->props_place = place;
  view->props_placed = TRUE;
#ifdef _DEBUG
  _props_place_assert(view, air);
#endif
  if(dt_conf_get_bool("canvas/debug/placement")) dt_control_queue_redraw_center();
  if(!place.visible)
  {
    _props_hide(view);
    // Once per showing: a pan that finds no room either should not say it again.
    if(!view->props_toasted) dt_toast_log(_("No room for the properties here: zoom out"));
    view->props_toasted = TRUE;
    return;
  }
  gtk_widget_show(root);
  // A placement that changed nothing repaints nothing: invalidating the overlay child repaints the
  // canvas under it through the toplevel, and a held placement released on leaving the properties, or
  // an edit that moved nothing, would pay a canvas paint for no change on screen.
  if(!moved && was_shown) return;
  gtk_widget_queue_allocate(dt_ui_center_base(dt_gui_get_ui()));
  gtk_widget_queue_draw(root);
}

/**
 * The card was put away. Closing never moves anything: the strip keeps its rectangle, whichever side
 * the card was on, so the button just clicked is still under the pointer.
 */
static void _props_collapse(dt_view_t *self)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  GtkWidget *root = _props_root(view);
  if(IS_NULL_PTR(root)) return;
  dt_canvas_place_t place = view->props_place;
  place.card_shown = FALSE;
  place.card_height = 0.0;
  place.clipped = FALSE;
  _props_card_apply(view, &place, TRUE);
  view->props_place = place;
  if(dt_conf_get_bool("canvas/debug/placement")) dt_control_queue_redraw_center();
  if(!gtk_widget_get_visible(root)) return;
  gtk_widget_queue_allocate(dt_ui_center_base(dt_gui_get_ui()));
  gtk_widget_queue_draw(root);
}

/**
 * Where the properties go: the rectangle the last placement solved. Answering the overlay's own
 * question is what keeps a move to a re-allocation of the overlay, where a margin change is a
 * resize that climbs to the toplevel and lays the window out again.
 *
 * The card is the part that may be shorter than it was solved for -- its content changed since, a
 * section folded -- so its height is read from the widget at this width, capped by what was solved;
 * the strip's rectangle is the solved one whichever side the card grows on. Never below what the
 * widget needs: GTK refuses to allocate a widget less than its minimum. A minimum grown past what
 * was solved asks for a placement again, from an idle, never from inside an allocation.
 */
static gboolean _props_child_position(GtkOverlay *overlay, GtkWidget *widget, GdkRectangle *allocation,
                                      gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  const dt_canvas_view_t *view = (const dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view) || IS_NULL_PTR(widget) || widget != _props_root(view)) return FALSE;
  const dt_canvas_place_t *place = &view->props_place;
  int minimum_width = 0;
  gtk_widget_get_preferred_width(widget, &minimum_width, NULL);
  const int width = MAX((int)place->strip.width, minimum_width);
  int minimum_height = 0;
  int natural_height = 0;
  gtk_widget_get_preferred_height_for_width(widget, width, &minimum_height, &natural_height);
  const int strip_height = (int)place->strip.height;
  const int solved_card = place->card_shown ? (int)lround(place->card_height) : 0;
  const int card = place->card_shown ? CLAMP(natural_height - strip_height, 0, solved_card) : 0;
  const int height = MAX(minimum_height, strip_height + card);
  allocation->x = (int)place->strip.x;
  allocation->y = place->growth == DT_CANVAS_PLACE_UP ? (int)place->strip.y - card : (int)place->strip.y;
  allocation->width = width;
  allocation->height = height;
  if(height > strip_height + solved_card) _props_sync(self);
  return TRUE;
}

/** The debug overlay: what the last placement kept clear, and where the properties went. */
static void _paint_props_placement(cairo_t *cr, const dt_canvas_view_t *view)
{
  if(IS_NULL_PTR(view->props_shapes) || !view->props_placed || view->props_id == 0 || view->props_suspended) return;
  if(!dt_conf_get_bool("canvas/debug/placement")) return;
  cairo_save(cr);
  cairo_set_line_width(cr, 1.0);
  for(guint idx = 0; idx < view->props_shapes->len; idx++)
  {
    const dt_canvas_place_shape_t *shape = &g_array_index(view->props_shapes, dt_canvas_place_shape_t, idx);
    double red = 1.0;
    double green = 0.0;
    if(shape->shape_class == DT_CANVAS_PLACE_PREDICTED)
      green = 0.55;
    else if(shape->shape_class == DT_CANVAS_PLACE_BODY)
      green = 0.9;
    else if(shape->shape_class != DT_CANVAS_PLACE_HARD)
      continue;
    cairo_rectangle(cr, shape->rect.x, shape->rect.y, shape->rect.width, shape->rect.height);
    cairo_set_source_rgba(cr, red, green, 0.0, 0.15);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, red, green, 0.0, 0.8);
    cairo_stroke(cr);
  }
  if(view->props_place.visible)
  {
    const dt_canvas_place_rect_t footprint = dt_canvas_place_footprint(&view->props_place);
    cairo_rectangle(cr, footprint.x, footprint.y, footprint.width, footprint.height);
    cairo_set_source_rgba(cr, 0.1, 0.85, 0.2, 0.2);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.1, 0.85, 0.2, 0.9);
    cairo_stroke(cr);
  }
  cairo_restore(cr);
}

/* --- edits made in the properties ----------------------------------------------------------- */

static void _props_session_end(dt_view_t *self);

/**
 * Open the edit session an edit belongs to, unless one is open for this object already: one snapshot of
 * the document, however many steps a slider drag or a burst of arrow keys takes. A session left open on
 * another object is over first, and is that object's undo step.
 */
static void _props_session_begin(dt_view_t *self, const uint32_t object_id)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(!IS_NULL_PTR(view->props_snapshot) && view->props_session_id == object_id) return;
  if(!IS_NULL_PTR(view->props_snapshot)) _props_session_end(self);
  view->props_snapshot = _begin_edit(view);
  view->props_session_id = object_id;
  view->props_effects = 0u;
}

/**
 * One step of the session, paid at once: the frames flowing around what moved refitted, the document
 * touched ONCE -- the painter keeps the frame it last composited for a generation it has already seen,
 * so a step that forgot it would show nothing until something else moved -- and one repaint. What the
 * steps owe only once, the undo record, the renders and the configuration, waits for the session's end.
 */
static void _props_session_step(dt_view_t *self, const uint32_t effects)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  view->props_effects |= effects;
  if(effects & DT_CANVAS_EFFECT_SETTLE_ALL) dt_canvas_props_settle_all(view->canvas);
  if(effects & DT_CANVAS_EFFECT_CHANGED) dt_canvas_touch(view->canvas);
  if(effects != 0u) dt_control_queue_redraw_center();
}

/**
 * End the session, if one is open: one undo step for everything it changed, the map's tiles fetched
 * and its settings kept for the next map once, and the toolbar told, since what it shows may be what
 * the object inherits. A placement held back for the edit runs now.
 */
static void _props_session_end(dt_view_t *self)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  view->props_live = FALSE;
  if(IS_NULL_PTR(view->props_snapshot)) return;
  dt_canvas_t *before = view->props_snapshot;
  const uint32_t effects = view->props_effects;
  view->props_snapshot = NULL;
  view->props_effects = 0u;
  if(effects & DT_CANVAS_EFFECT_CHANGED)
    _record_undo(self, before);
  else
    dt_canvas_free(before);
  dt_canvas_object_t *object = dt_canvas_find_object(view->canvas, view->props_session_id);
  if(!IS_NULL_PTR(object) && object->kind == DT_CANVAS_OBJECT_MAP)
  {
    if(effects & DT_CANVAS_EFFECT_COMMIT_CONF)
    {
      // The map just edited is where the next one starts.
      dt_conf_set_int("canvas/map_zoom", object->map.zoom);
      dt_conf_set_int("canvas/map_source", (int)object->map.source);
      dt_conf_set_float("canvas/map_latitude", (float)object->map.latitude);
      dt_conf_set_float("canvas/map_longitude", (float)object->map.longitude);
    }
    if(effects & DT_CANVAS_EFFECT_COMMIT_RENDER) _start_map_render(self, object);
  }
  if(effects & DT_CANVAS_EFFECT_CHANGED)
    DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_CANVAS_CHANGED);
  if(effects != 0u) dt_control_queue_redraw_center();
  _props_sync(self);
}

/**
 * Commit whatever the properties hold and have not committed yet, before anything else reaches the
 * document or the properties change hands: digits typed into a spin button count once the field is
 * left, and a close, a new object or a view switch leaves it without the focus ever moving; a slider
 * session a burst of wheel notches left waiting for its pause ends here as its own undo step, rather
 * than taking the next action's edit into it. A colour or font dialog still open would write its pick
 * to whatever is shown next, so it closes.
 *
 * The widget's own session is committed first, and through the host like any other commit: ending only
 * the view's session would leave the widget's timer armed, and when it fired it would send the
 * control's value onto the document as it had become since -- an undo re-done behind the user's back
 * and the redo list gone with it, or a frame's old width written in the middle of a drag of its corner.
 */
static void _props_commit_pending(dt_view_t *self)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(!IS_NULL_PTR(view->props))
  {
    dt_canvas_props_gtk_commit(view->props);
    dt_canvas_props_gtk_close_dialogs(view->props);
  }
  _props_session_end(self);
}

/**
 * Forget what the properties hold instead of committing it, for the one place that cannot record it: an
 * undo popped by the Edit menu, which reaches the view only through `_undo_pop()`. The undo stack is
 * locked while that runs and silently drops a record made from inside it, so the edit could not be kept
 * -- and the widget's session, left alone, would be committed by its timer after the undo and re-do it,
 * wiping the redo list. What the edit changed goes with the document the record replaces.
 */
static void _props_forget_pending(dt_view_t *self)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(!IS_NULL_PTR(view->props)) dt_canvas_props_gtk_forget(view->props);
  view->props_live = FALSE;
  if(IS_NULL_PTR(view->props_snapshot)) return;
  dt_canvas_free(view->props_snapshot);
  view->props_snapshot = NULL;
  view->props_effects = 0u;
}

/** What the view holds of an edit rather than the document: whether the cutout's handles are out. */
static void _props_view_state(dt_canvas_view_t *view, const dt_canvas_object_t *object,
                              const dt_canvas_prop_id_t prop_id, const dt_canvas_prop_value_t *value)
{
  switch(prop_id)
  {
    case DT_CANVAS_PROP_CUTOUT_SHAPE:
      // A new shape is edited at once, and no shape has nothing to edit.
      view->mask_editing = object->mask.shape != DT_CANVAS_MASK_NONE;
      break;
    case DT_CANVAS_PROP_CUTOUT_EDIT:
      view->mask_editing = value->flag;
      break;
    default:
      break;
  }
}

/**
 * The picture's buttons: what the document cannot do on its own. Each keeps the undo step, the
 * selection and the renders its menu entry has, so it runs once the session is over -- and looks the
 * object up again, since adding a frame may move every object in memory.
 */
static void _props_view_action(dt_view_t *self, const uint32_t object_id, const dt_canvas_prop_id_t prop_id)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  dt_canvas_object_t *object = dt_canvas_find_object(view->canvas, object_id);
  if(IS_NULL_PTR(object)) return;
  switch(prop_id)
  {
    case DT_CANVAS_PROP_IMAGE_REFRESH:
      _refresh_object(self, object);
      break;
    case DT_CANVAS_PROP_IMAGE_NOTE:
      _show_sidecar_note(self, object);
      break;
    case DT_CANVAS_PROP_IMAGE_ADD_MAP:
      _map_of_image(self, object);
      break;
    default:
      break;
  }
}

/* --- when the properties show ------------------------------------------------------------ */

/** Forget the opening or the content action a double click or Return deferred, if it has not run. */
static void _props_request_drop(dt_canvas_view_t *view)
{
  if(view->props_request_idle == 0) return;
  g_source_remove(view->props_request_idle);
  view->props_request_idle = 0;
}

/**
 * Fill the properties from the object, and remember what they were filled for. A new object -- the
 * first one shown since they opened, or since the widget was built again on the way back to the
 * atelier -- opens on the section the user last left open on objects of its kind; the card itself is
 * never remembered open.
 */
static void _props_refill(dt_view_t *self, const dt_canvas_object_t *object)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view->props) || IS_NULL_PTR(object)) return;
  const gboolean new_object = !view->props_filled || view->props_filled_id != object->id;
  dt_canvas_props_gtk_refill(view->props, view->canvas, object);
  if(new_object) dt_canvas_props_gtk_set_section(view->props, _props_section_stored(object->kind));
  view->props_filled = TRUE;
  view->props_filled_id = object->id;
  view->props_filled_generation = view->canvas->generation;
  view->props_filled_editing = view->mask_editing;
  view->props_refill_owed = FALSE;
}

/**
 * Close the properties: nothing reopens them but one of the gestures that open them. What they hold
 * is committed first; what was asked of them and has not run yet goes; and the run of clicks forgets
 * they were showing, so the next double click opens them again rather than going into the object.
 * The keyboard focus they held goes back to the canvas, where the next key is meant for.
 */
static void _props_close(dt_view_t *self)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(view->props_id == 0) return;
  _props_commit_pending(self);
  _props_request_drop(view);
  dt_canvas_click_sequence_closed(&view->click_sequence);
  view->props_id = 0;
  view->props_suspended = FALSE;
  view->props_placed = FALSE;
  view->props_live = FALSE;
  view->props_place_pending = FALSE;
  view->props_card_open = FALSE;
  view->props_filled = FALSE;
  if(!IS_NULL_PTR(view->props))
  {
    if(dt_canvas_props_gtk_focus_inside(view->props)) dt_widget_refocus();
    dt_canvas_props_gtk_refill(view->props, view->canvas, NULL);
  }
  _props_hide(view);
}

/**
 * Hide open properties for the gesture in flight, once: every later motion of it finds them
 * hidden already. Only a gesture that actually moves something gets here, so the first click
 * of a double click never makes them blink.
 */
static void _props_suspend(dt_view_t *self)
{
  if(IS_NULL_PTR(self)) return;
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view) || view->props_id == 0 || view->props_suspended) return;
  view->props_suspended = TRUE;
  _props_hide(view);
}

/**
 * Show the properties as the document and the view now are: refilled when the object, its document or
 * the view state they show changed, then placed -- afresh for a showing that has not been placed yet,
 * relative to where they were otherwise.
 */
static void _props_refresh(dt_view_t *self)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view->props)) return;
  const dt_canvas_object_t *object = _props_object(view);
  if(IS_NULL_PTR(object) || view->props_suspended)
  {
    _props_hide(view);
    return;
  }
  const gboolean stale = !view->props_filled || view->props_filled_id != object->id
                         || view->props_filled_generation != view->canvas->generation
                         || view->props_filled_editing != view->mask_editing || view->props_refill_owed;
  if(stale) _props_refill(self, object);
  _props_place(self, view->props_placed ? DT_CANVAS_PLACE_RESOLVE : DT_CANVAS_PLACE_OPEN);
}

static gboolean _props_idle(gpointer data)
{
  dt_view_t *self = (dt_view_t *)data;
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  view->props_idle = 0;
  if(view->props_id != 0 && IS_NULL_PTR(_props_object(view))) _props_close(self);
  // Suspended properties come back once nothing is moving: no button held, no wheel turning.
  if(view->props_suspended && view->drag == DT_CANVAS_DRAG_NONE && !view->interacting)
    view->props_suspended = FALSE;
  _props_refresh(self);
  return G_SOURCE_REMOVE;
}

/**
 * Bring the properties up to date with the document, the selection and the gesture: refill,
 * place, bring back, hide or close them -- but never OPEN them, which is what keeps a click, a
 * drag or a rubber band from making them appear. A selection that is no longer exactly the
 * open object closes them at once, so they never linger over the next object; the rest is
 * coalesced into one idle, off the draw path, since moving an overlay child from inside a draw
 * glitches.
 */
static void _props_sync(dt_view_t *self)
{
  if(IS_NULL_PTR(self)) return;
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view)) return;
  if(view->props_id != 0 && IS_NULL_PTR(_props_object(view))) _props_close(self);
  if(IS_NULL_PTR(view->props) || view->props_idle != 0) return;
  view->props_idle = g_idle_add(_props_idle, self);
}

/**
 * Remember where on the object its properties were asked for, as a place ON the object rather
 * than on the screen: a frame's unit-square point and a connector's fraction of its route both
 * follow the object wherever it is moved, turned or stretched. Without a point, the middle of
 * a frame's bottom edge and the middle of a route.
 */
static void _props_anchor_set(dt_canvas_view_t *view, const dt_canvas_object_t *object, const gboolean has_point,
                              const double x, const double y)
{
  view->props_anchor_u = 0.5;
  view->props_anchor_v = 1.0;
  view->props_anchor_t = 0.5;
  if(!has_point) return;
  if(dt_canvas_object_is_frame(object) && object->width > 0.0 && object->height > 0.0)
  {
    double local_x = 0.0;
    double local_y = 0.0;
    dt_canvas_object_to_local(object, x, y, &local_x, &local_y);
    view->props_anchor_u = CLAMP(local_x / object->width + 0.5, 0.0, 1.0);
    view->props_anchor_v = CLAMP(local_y / object->height + 0.5, 0.0, 1.0);
    return;
  }
  dt_canvas_route_t route;
  if(object->kind == DT_CANVAS_OBJECT_CONNECTOR && dt_canvas_connector_route(view->canvas, object, &route))
    view->props_anchor_t = dt_canvas_route_fraction_at(&route, x, y);
}

/**
 * Open one object's properties: it becomes the whole selection, and they show at once, as a strip.
 * What the properties held for another object is committed before they change hands.
 */
static void _props_open(dt_view_t *self, const uint32_t object_id, const gboolean has_point, const double x,
                        const double y)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  const dt_canvas_object_t *object = dt_canvas_find_object(view->canvas, object_id);
  if(IS_NULL_PTR(object) || view->connecting) return;
  _props_commit_pending(self);
  // The commit may have written to the document: find the object again.
  object = dt_canvas_find_object(view->canvas, object_id);
  if(IS_NULL_PTR(object)) return;
  _select_only(view, object_id);
  view->props_id = object_id;
  _props_anchor_set(view, object, has_point, x, y);
  // A new showing searches the whole view again, and may say once more that there is no room.
  view->props_placed = FALSE;
  view->props_toasted = FALSE;
  view->props_card_open = FALSE;
  view->props_card_told_valid = FALSE;
  view->props_filled = FALSE;
  // Asked for with a button still held (the I key mid-drag), they wait for the gesture to end.
  view->props_suspended = view->drag != DT_CANVAS_DRAG_NONE;
  _props_refresh(self);
  dt_control_queue_redraw_center();
}

/** What a double click on an object whose properties are showing goes into, and what Return does. */
static void _props_content_action(dt_view_t *self, dt_canvas_object_t *object)
{
  if(IS_NULL_PTR(object)) return;
  switch(object->kind)
  {
    case DT_CANVAS_OBJECT_TEXT:
      _edit_text(self, object);
      break;
    case DT_CANVAS_OBJECT_IMAGE:
      _open_in_darkroom(object);
      break;
    case DT_CANVAS_OBJECT_SVG:
      _reload_drawing(self, object);
      break;
    default:
      // A map and a connector have nothing inside them: their properties are all there is.
      break;
  }
}

/** An opening or a content action, deferred until the gesture that asked for it is over. */
typedef struct dt_canvas_props_request_t
{
  dt_view_t *self;
  uint64_t token;       ///< the document it was asked about
  uint64_t press_count; ///< the presses counted when it was asked: one more, and it is dropped
  uint32_t object_id;   ///< looked up again when it runs: the object may be gone by then
  gboolean content;     ///< run the content action rather than open the properties
  gboolean has_point;   ///< the canvas point it was asked at, for the anchor
  double x;
  double y;
} dt_canvas_props_request_t;

static gboolean _props_request_run(gpointer data)
{
  dt_canvas_props_request_t *request = (dt_canvas_props_request_t *)data;
  dt_view_t *self = request->self;
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view)) return G_SOURCE_REMOVE;
  view->props_request_idle = 0;
  if(request->token != view->token) return G_SOURCE_REMOVE;
  if(dt_view_manager_get_current_view(dt_view_manager_get_global()) != self) return G_SOURCE_REMOVE;
  // A press handled since it was asked answered something newer -- another object, the
  // background, a third click arming a move -- and whatever it did stands. And no dialog runs
  // with a gesture armed: its modal grab would swallow the release the gesture is owed.
  if(request->press_count != view->click_sequence.press_count) return G_SOURCE_REMOVE;
  if(view->drag != DT_CANVAS_DRAG_NONE) return G_SOURCE_REMOVE;
  if(request->content)
  {
    _props_commit_pending(self);
    _props_content_action(self, dt_canvas_find_object(view->canvas, request->object_id));
  }
  else
    _props_open(self, request->object_id, request->has_point, request->x, request->y);
  return G_SOURCE_REMOVE;
}

/**
 * Defer an opening or a content action to an idle. A press handler must not run a modal
 * dialog: the dialog's nested main loop would swallow the release the gesture is still owed,
 * and a drag armed by that very press would be left to answer it afterwards.
 *
 * The idle runs ahead of the redraw the press queued. Behind it, it would wait for the paint,
 * and on a heavy page a third click landing inside that paint is dispatched first and drops the
 * request: a triple click would open nothing at all.
 */
static void _props_request(dt_view_t *self, const uint32_t object_id, const gboolean content,
                           const gboolean has_point, const double x, const double y)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  _props_request_drop(view);
  dt_canvas_props_request_t *request = g_new0(dt_canvas_props_request_t, 1);
  request->self = self;
  request->token = view->token;
  request->press_count = view->click_sequence.press_count;
  request->object_id = object_id;
  request->content = content;
  request->has_point = has_point;
  request->x = x;
  request->y = y;
  view->props_request_idle = g_idle_add_full(G_PRIORITY_HIGH_IDLE, _props_request_run, request, g_free);
}

/* --- what the properties ask of the view ---------------------------------------------------- */

/**
 * One property changed in the properties. Every edit goes through the table's writer, which makes the
 * change and says what it owes, and the view pays: each step at once, the session's bill at its end.
 * LIVE is a step of a gesture still in progress, COMMIT ends that gesture with the control's final
 * value, and ONCE is a whole gesture in one call.
 *
 * Whatever the phase, the properties are filled again from the document straight away: a width that
 * kept a picture's proportions moved its height, a shape moved which rows apply, and a value the writer
 * refused -- the inherited one, written while inheriting -- must not stay on the control as though it
 * had been kept. The control being dragged is left alone by the widget itself.
 */
static void _props_host_edit(gpointer data, const dt_canvas_prop_id_t prop_id, const dt_canvas_prop_value_t *value,
                             const dt_canvas_edit_phase_t phase)
{
  dt_view_t *self = (dt_view_t *)data;
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view) || IS_NULL_PTR(value)) return;
  dt_canvas_object_t *object = _props_object(view);
  if(IS_NULL_PTR(object))
  {
    // What the edit was for is gone; whatever it did stands as recorded.
    if(phase != DT_CANVAS_EDIT_LIVE) _props_session_end(self);
    return;
  }
  const uint32_t object_id = object->id;
  _props_session_begin(self, object_id);
  const uint32_t effects = dt_canvas_prop_write(view->canvas, object, prop_id, value);
  if(effects & DT_CANVAS_EFFECT_VIEW) _props_view_state(view, object, prop_id, value);
  _props_session_step(self, effects);
  if(phase == DT_CANVAS_EDIT_LIVE)
  {
    view->props_live = TRUE;
    _props_refill(self, object);
    return;
  }
  _props_session_end(self);
  if(effects & DT_CANVAS_EFFECT_VIEW) _props_view_action(self, object_id, prop_id);
  // The session's end and the picture's buttons may have changed the selection, and with it the object.
  const dt_canvas_object_t *shown = _props_object(view);
  if(IS_NULL_PTR(shown)) return;
  _props_refill(self, shown);
  // A shape that brings rows of its own grows the card the user is looking at, from where the strip is.
  if((effects & DT_CANVAS_EFFECT_RESTRUCTURE) && view->props_place.visible && view->props_place.card_shown)
    _props_place(self, DT_CANVAS_PLACE_GROW);
}

/**
 * A section's own switch: the object takes its own values for the group, or gives them back. A gesture
 * of its own -- except for a double click resetting a slider of the group, whose reset gives the group
 * back inside the LIVE session its first click opened, so the whole double click is one undo step.
 */
static void _props_host_group_own(gpointer data, const dt_canvas_prop_group_t group, const gboolean own)
{
  dt_view_t *self = (dt_view_t *)data;
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view)) return;
  dt_canvas_object_t *object = _props_object(view);
  if(IS_NULL_PTR(object)) return;
  const gboolean inside_session
      = view->props_live && !IS_NULL_PTR(view->props_snapshot) && view->props_session_id == object->id;
  if(!inside_session) _props_session_end(self);
  _props_session_begin(self, object->id);
  const uint32_t effects = dt_canvas_group_set_own(view->canvas, object, group, own);
  _props_session_step(self, effects);
  if(!inside_session) _props_session_end(self);
  const dt_canvas_object_t *shown = _props_object(view);
  if(IS_NULL_PTR(shown)) return;
  _props_refill(self, shown);
  // Giving the font back is another face, with other features: the card fits its rows again, from
  // where the strip is, as after an edit.
  if((effects & DT_CANVAS_EFFECT_RESTRUCTURE) && view->props_place.visible && view->props_place.card_shown)
    _props_place(self, DT_CANVAS_PLACE_GROW);
}

/** The strip's own buttons: go into the object, or close its properties. */
static void _props_host_action(gpointer data, const dt_canvas_props_action_t action)
{
  dt_view_t *self = (dt_view_t *)data;
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view)) return;
  switch(action)
  {
    case DT_CANVAS_PROPS_ACTION_CONTENT:
    {
      const dt_canvas_object_t *object = _props_object(view);
      // From an idle, like a double click's: the text editor is a modal dialog, and the darkroom a view
      // switch that destroys the very button being released.
      if(!IS_NULL_PTR(object) && dt_canvas_props_has_content_action(object->kind))
        _props_request(self, object->id, TRUE, FALSE, 0.0, 0.0);
      break;
    }
    case DT_CANVAS_PROPS_ACTION_CLOSE:
      _props_close(self);
      dt_control_queue_redraw_center();
      break;
    default:
      break;
  }
}

/**
 * The card button. Asking for the card grows it at once from where the strip is, whichever side has
 * the room, and it is not remembered: every showing starts as a strip. Putting it away moves nothing.
 */
static void _props_host_card_toggled(gpointer data, const gboolean open)
{
  dt_view_t *self = (dt_view_t *)data;
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view) || IS_NULL_PTR(_props_object(view))) return;
  view->props_card_open = open;
  if(open)
    _props_place(self, DT_CANVAS_PLACE_GROW);
  else
    _props_collapse(self);
}

/**
 * A section opened or folded by the user, or grown or shrunk by its own rows: the kind remembers which
 * one was left open, and the card is fitted again at once from where the strip is.
 */
static void _props_host_section_toggled(gpointer data, const dt_canvas_prop_section_t section, const gboolean open)
{
  dt_view_t *self = (dt_view_t *)data;
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view)) return;
  const dt_canvas_object_t *object = _props_object(view);
  if(IS_NULL_PTR(object)) return;
  _props_section_store(object->kind, open ? (int)section : -1);
  if(view->props_place.visible && view->props_place.card_shown) _props_place(self, DT_CANVAS_PLACE_GROW);
}

/** The pointer crossed the properties' edge. Leaving them releases a placement they held back. */
static void _props_host_pointer_inside(gpointer data, const gboolean inside)
{
  dt_view_t *self = (dt_view_t *)data;
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view)) return;
  view->props_pointer_inside = inside;
  if(!inside && view->props_place_pending) _props_sync(self);
}

/** What the view holds rather than the document: editing the cutout is a mode of the view. */
static gboolean _props_host_view_value(gpointer data, const dt_canvas_prop_id_t prop_id, dt_canvas_prop_value_t *value)
{
  const dt_view_t *self = (const dt_view_t *)data;
  const dt_canvas_view_t *view = (const dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view) || prop_id != DT_CANVAS_PROP_CUTOUT_EDIT) return FALSE;
  value->flag = view->mask_editing;
  return TRUE;
}

/**
 * The document changed behind the properties' back -- a canvas default the toolbar set, an undo. What
 * an inheriting object shows is the canvas's value, so they are refilled like any other reader of the
 * document; the refill runs at idle with the widget's handlers blocked, so it cannot answer itself.
 */
static void _props_canvas_changed(gpointer instance, gpointer user_data)
{
  _props_sync((dt_view_t *)user_data);
}

/**
 * Keys the properties' own controls did not take, while one of them holds the focus. A plain key never
 * fires a shortcut there (the widget is tagged for that), but the keys the view itself reads would
 * still reach the canvas behind it: Delete would delete the object being edited, an arrow would nudge
 * it, Return would go into it. They stop here. Escape gives the focus back to the canvas, so a second
 * Escape closes the properties, as it does from the canvas.
 * @return TRUE when the key is the properties' business.
 */
static gboolean _props_key_guard(dt_canvas_view_t *view, const guint key)
{
  if(IS_NULL_PTR(view->props) || !dt_canvas_props_gtk_focus_inside(view->props)) return FALSE;
  switch(key)
  {
    case GDK_KEY_Escape:
      dt_widget_refocus();
      return TRUE;
    case GDK_KEY_Delete:
    case GDK_KEY_KP_Delete:
    case GDK_KEY_BackSpace:
    case GDK_KEY_Left:
    case GDK_KEY_Right:
    case GDK_KEY_Up:
    case GDK_KEY_Down:
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
      return TRUE;
    default:
      return FALSE;
  }
}

/**
 * Build the properties for the atelier being entered: one overlay child of the centre, placed by
 * answering the overlay's own question, hidden until an object's properties are shown. Every kind's
 * rows are built here, once.
 */
static void _props_create(dt_view_t *self)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(!IS_NULL_PTR(view->props)) return;
  const dt_canvas_props_host_t host = { .data = self,
                                        .edit = _props_host_edit,
                                        .group_own = _props_host_group_own,
                                        .action = _props_host_action,
                                        .card_toggled = _props_host_card_toggled,
                                        .section_toggled = _props_host_section_toggled,
                                        .pointer_inside = _props_host_pointer_inside,
                                        .view_value = _props_host_view_value };
  view->props = dt_canvas_props_gtk_new(&host);
  GtkWidget *root = dt_canvas_props_gtk_root(view->props);
  // Shown by a placement only: a show_all from outside must not bring back properties nobody opened.
  gtk_widget_set_no_show_all(root, TRUE);
  GtkWidget *base = dt_ui_center_base(dt_gui_get_ui());
  gtk_overlay_add_overlay(GTK_OVERLAY(base), root);
  view->props_position_handler
      = g_signal_connect(base, "get-child-position", G_CALLBACK(_props_child_position), self);
  view->props_focus_handler = g_signal_connect(dt_ui_main_window(dt_gui_get_ui()), "set-focus",
                                               G_CALLBACK(_props_focus_changed), self);
  view->props_card_told_valid = FALSE;
}

/**
 * Take the properties down with the atelier. What they held is committed by the caller first; the open
 * state stays, for `enter()` to show them again, as a strip.
 */
static void _props_destroy(dt_view_t *self)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(view->props_idle != 0)
  {
    g_source_remove(view->props_idle);
    view->props_idle = 0;
  }
  GtkWidget *base = dt_ui_center_base(dt_gui_get_ui());
  if(view->props_position_handler != 0)
  {
    g_signal_handler_disconnect(base, view->props_position_handler);
    view->props_position_handler = 0;
  }
  if(view->props_focus_handler != 0)
  {
    g_signal_handler_disconnect(dt_ui_main_window(dt_gui_get_ui()), view->props_focus_handler);
    view->props_focus_handler = 0;
  }
  if(!IS_NULL_PTR(view->props))
  {
    // The focus goes before the widget holding it does, so nothing is left focused inside a dead widget.
    if(dt_canvas_props_gtk_focus_inside(view->props))
      gtk_window_set_focus(GTK_WINDOW(dt_ui_main_window(dt_gui_get_ui())), NULL);
    dt_canvas_props_gtk_free(view->props);
    view->props = NULL;
  }
  // Shown again on the way back, the properties are placed afresh for the view they come back to.
  view->props_placed = FALSE;
  view->props_toasted = FALSE;
  view->props_pointer_inside = FALSE;
  view->props_place_pending = FALSE;
  view->props_card_open = FALSE;
  view->props_card_told_valid = FALSE;
  view->props_filled = FALSE;
}

/* --- drag and drop from the filmstrip ------------------------------------------------- */

static void _drop_images(dt_view_t *self, const uint32_t *imgids, const int count, const double x, const double y)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(count <= 0) return;
  dt_canvas_t *before = _begin_edit(view);
  g_array_set_size(view->selection, 0);
  int added = 0;
  for(int idx = 0; idx < count; idx++)
  {
    const int32_t imgid = (int32_t)imgids[idx];
    if(imgid <= 0) continue;
    dt_canvas_image_t source;
    memset(&source, 0, sizeof(source));
    if(!dt_canvas_render_describe_source(imgid, &source)) continue;
    // Each dropped image lands a little further, so a multi-drop is not one pile.
    const double drop_x = dt_canvas_snap(view->canvas, x + added * CANVAS_DROP_STAGGER);
    const double drop_y = dt_canvas_snap(view->canvas, y + added * CANVAS_DROP_STAGGER);
    dt_canvas_object_t *object = dt_canvas_add_image(view->canvas, drop_x, drop_y, source.source_width,
                                                     source.source_height);
    source.jpeg = NULL;
    source.sync_status = DT_CANVAS_SYNC_UNKNOWN;
    object->image = source;
    g_array_append_val(view->selection, object->id);
    _start_render(self, object, imgid);
    added++;
  }
  if(added > 0)
    _record_undo(self, before);
  else
    dt_canvas_free(before);
  _props_sync(self);
  dt_control_queue_redraw_center();
}

static void _drag_data_received(GtkWidget *widget, GdkDragContext *context, gint x, gint y,
                                GtkSelectionData *selection_data, guint target_type, guint time, gpointer data)
{
  dt_view_t *self = (dt_view_t *)data;
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  gboolean success = FALSE;
  if(!IS_NULL_PTR(selection_data) && target_type == DND_TARGET_IMGID)
  {
    const int count = gtk_selection_data_get_length(selection_data) / (int)sizeof(uint32_t);
    const uint32_t *imgids = (const uint32_t *)gtk_selection_data_get_data(selection_data);
    double canvas_x = 0.0;
    double canvas_y = 0.0;
    _to_canvas(view, x, y, &canvas_x, &canvas_y);
    _drop_images(self, imgids, count, canvas_x, canvas_y);
    success = count > 0;
  }
  gtk_drag_finish(context, success, FALSE, time);
}

static gboolean _drag_motion(GtkWidget *widget, GdkDragContext *context, gint x, gint y, guint time, gpointer data)
{
  // GTK_DEST_DEFAULT_MOTION has already matched the targets and answered the source with
  // the action they share. Answering here with a different one -- COPY, which the filmstrip
  // does not offer, it drags as a MOVE -- is what made every drop silently refused.
  return TRUE;
}

static void _filmstrip_drag_begin(gpointer instance, int32_t imgid, gpointer user_data)
{
  dt_selection_select_single(dt_selection_get_global(), imgid);
  dt_control_set_mouse_over_id(imgid);
  dt_control_set_keyboard_over_id(imgid);
}

static void _profile_changed(gpointer instance, gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  dt_canvas_surface_cache_clear(view->cache);
  dt_control_queue_redraw_center();
}

/* --- the navigation flower --------------------------------------------------------- */

static void _flower_center(const dt_canvas_view_t *view, double *center_x, double *center_y)
{
  *center_x = view->width - DT_PIXEL_APPLY_DPI(CANVAS_FLOWER_MARGIN + CANVAS_FLOWER_RADIUS);
  *center_y = view->height - DT_PIXEL_APPLY_DPI(CANVAS_FLOWER_MARGIN + CANVAS_FLOWER_RADIUS);
}

/** Which part of the flower is under a screen point. */
static dt_canvas_flower_part_t _flower_hit(const dt_canvas_view_t *view, const double screen_x, const double screen_y)
{
  if(view->width <= 0 || view->height <= 0) return DT_CANVAS_FLOWER_NONE;
  double center_x = 0.0;
  double center_y = 0.0;
  _flower_center(view, &center_x, &center_y);
  const double offset_x = screen_x - center_x;
  const double offset_y = screen_y - center_y;
  const double distance = hypot(offset_x, offset_y);
  if(distance > DT_PIXEL_APPLY_DPI(CANVAS_FLOWER_RADIUS)) return DT_CANVAS_FLOWER_NONE;
  if(distance <= DT_PIXEL_APPLY_DPI(CANVAS_FLOWER_CENTER_RADIUS)) return DT_CANVAS_FLOWER_FIT;
  if(distance <= DT_PIXEL_APPLY_DPI(CANVAS_FLOWER_INNER_RADIUS))
    return offset_y < 0.0 ? DT_CANVAS_FLOWER_ZOOM_IN : DT_CANVAS_FLOWER_ZOOM_OUT;
  // The outer ring is four petals, split on the diagonals.
  const double angle = atan2(offset_y, offset_x);
  if(angle >= -3.0 * M_PI / 4.0 && angle < -M_PI / 4.0) return DT_CANVAS_FLOWER_PAN_UP;
  if(angle >= -M_PI / 4.0 && angle < M_PI / 4.0) return DT_CANVAS_FLOWER_PAN_RIGHT;
  if(angle >= M_PI / 4.0 && angle < 3.0 * M_PI / 4.0) return DT_CANVAS_FLOWER_PAN_DOWN;
  return DT_CANVAS_FLOWER_PAN_LEFT;
}

static void _flower_activate(dt_canvas_view_t *view, const dt_canvas_flower_part_t part)
{
  _interaction_touch(dt_view_manager_get_global()->proxy.canvas.view);
  const double pan_x = view->width * CANVAS_FLOWER_PAN_FRACTION / view->zoom;
  const double pan_y = view->height * CANVAS_FLOWER_PAN_FRACTION / view->zoom;
  switch(part)
  {
    case DT_CANVAS_FLOWER_PAN_UP:
      view->center_y -= pan_y;
      break;
    case DT_CANVAS_FLOWER_PAN_DOWN:
      view->center_y += pan_y;
      break;
    case DT_CANVAS_FLOWER_PAN_LEFT:
      view->center_x -= pan_x;
      break;
    case DT_CANVAS_FLOWER_PAN_RIGHT:
      view->center_x += pan_x;
      break;
    case DT_CANVAS_FLOWER_ZOOM_IN:
      _zoom_around(view, view->width * 0.5, view->height * 0.5, CANVAS_FLOWER_ZOOM_STEP);
      break;
    case DT_CANVAS_FLOWER_ZOOM_OUT:
      _zoom_around(view, view->width * 0.5, view->height * 0.5, 1.0 / CANVAS_FLOWER_ZOOM_STEP);
      break;
    case DT_CANVAS_FLOWER_FIT:
      _zoom_fit(view);
      break;
    default:
      break;
  }
  _props_sync(dt_view_manager_get_global()->proxy.canvas.view);
  dt_control_queue_redraw_center();
}

static void _flower_petal(cairo_t *cr, const double center_x, const double center_y, const double inner,
                          const double outer, const double start_angle, const gboolean hovered)
{
  const double gap = 0.035;
  cairo_new_path(cr);
  cairo_arc(cr, center_x, center_y, outer, start_angle + gap, start_angle + M_PI / 2.0 - gap);
  cairo_arc_negative(cr, center_x, center_y, inner, start_angle + M_PI / 2.0 - gap, start_angle + gap);
  cairo_close_path(cr);
  cairo_set_source_rgba(cr, 0.15, 0.15, 0.15, hovered ? 0.95 : 0.7);
  cairo_fill_preserve(cr);
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, hovered ? 0.9 : 0.35);
  cairo_set_line_width(cr, 1.0);
  cairo_stroke(cr);
}

static void _flower_triangle(cairo_t *cr, const double tip_x, const double tip_y, const double direction_x,
                             const double direction_y, const double size)
{
  const double base_x = tip_x - direction_x * size;
  const double base_y = tip_y - direction_y * size;
  const double side_x = -direction_y * size * 0.6;
  const double side_y = direction_x * size * 0.6;
  cairo_move_to(cr, tip_x, tip_y);
  cairo_line_to(cr, base_x + side_x, base_y + side_y);
  cairo_line_to(cr, base_x - side_x, base_y - side_y);
  cairo_close_path(cr);
  cairo_fill(cr);
}

static void _paint_flower(cairo_t *cr, const dt_canvas_view_t *view)
{
  if(view->width <= 0 || view->height <= 0) return;
  double center_x = 0.0;
  double center_y = 0.0;
  _flower_center(view, &center_x, &center_y);
  const double outer = DT_PIXEL_APPLY_DPI(CANVAS_FLOWER_RADIUS);
  const double inner = DT_PIXEL_APPLY_DPI(CANVAS_FLOWER_INNER_RADIUS);
  const double core = DT_PIXEL_APPLY_DPI(CANVAS_FLOWER_CENTER_RADIUS);
  const dt_canvas_flower_part_t hover = view->flower_hover;

  cairo_save(cr);
  // Four petals, starting at the top-left diagonal and going clockwise: up, right, down, left.
  static const dt_canvas_flower_part_t petals[4]
      = { DT_CANVAS_FLOWER_PAN_UP, DT_CANVAS_FLOWER_PAN_RIGHT, DT_CANVAS_FLOWER_PAN_DOWN, DT_CANVAS_FLOWER_PAN_LEFT };
  for(int idx = 0; idx < 4; idx++)
  {
    const double start_angle = -3.0 * M_PI / 4.0 + idx * M_PI / 2.0;
    _flower_petal(cr, center_x, center_y, inner + 2.0, outer, start_angle, hover == petals[idx]);
  }
  // Inner disc: zoom in above, zoom out below, fit at the core.
  cairo_new_path(cr);
  cairo_arc(cr, center_x, center_y, inner, M_PI, 2.0 * M_PI);
  cairo_close_path(cr);
  cairo_set_source_rgba(cr, 0.15, 0.15, 0.15, hover == DT_CANVAS_FLOWER_ZOOM_IN ? 0.95 : 0.7);
  cairo_fill(cr);
  cairo_new_path(cr);
  cairo_arc(cr, center_x, center_y, inner, 0.0, M_PI);
  cairo_close_path(cr);
  cairo_set_source_rgba(cr, 0.15, 0.15, 0.15, hover == DT_CANVAS_FLOWER_ZOOM_OUT ? 0.95 : 0.7);
  cairo_fill(cr);
  // Outlines: the hovered half or the hovered core gets the petals' bright stroke.
  cairo_set_line_width(cr, 1.0);
  cairo_new_path(cr);
  cairo_arc(cr, center_x, center_y, inner, M_PI, 2.0 * M_PI);
  cairo_close_path(cr);
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, hover == DT_CANVAS_FLOWER_ZOOM_IN ? 0.9 : 0.35);
  cairo_stroke(cr);
  cairo_new_path(cr);
  cairo_arc(cr, center_x, center_y, inner, 0.0, M_PI);
  cairo_close_path(cr);
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, hover == DT_CANVAS_FLOWER_ZOOM_OUT ? 0.9 : 0.35);
  cairo_stroke(cr);
  cairo_new_path(cr);
  cairo_arc(cr, center_x, center_y, core, 0.0, 2.0 * M_PI);
  const double core_grey = hover == DT_CANVAS_FLOWER_FIT ? 0.9 : 0.55;
  cairo_set_source_rgba(cr, core_grey, core_grey, core_grey, 0.95);
  cairo_fill_preserve(cr);
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, hover == DT_CANVAS_FLOWER_FIT ? 0.9 : 0.35);
  cairo_stroke(cr);

  // Glyphs: arrows on the petals, + and - on the disc.
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.9);
  const double glyph_radius = (inner + outer) * 0.5;
  const double glyph = DT_PIXEL_APPLY_DPI(9.0);
  _flower_triangle(cr, center_x, center_y - glyph_radius - glyph * 0.5, 0.0, -1.0, glyph);
  _flower_triangle(cr, center_x + glyph_radius + glyph * 0.5, center_y, 1.0, 0.0, glyph);
  _flower_triangle(cr, center_x, center_y + glyph_radius + glyph * 0.5, 0.0, 1.0, glyph);
  _flower_triangle(cr, center_x - glyph_radius - glyph * 0.5, center_y, -1.0, 0.0, glyph);
  const double sign = DT_PIXEL_APPLY_DPI(5.0);
  const double sign_y_in = center_y - (inner + core) * 0.5;
  const double sign_y_out = center_y + (inner + core) * 0.5;
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.0));
  cairo_move_to(cr, center_x - sign, sign_y_in);
  cairo_line_to(cr, center_x + sign, sign_y_in);
  cairo_move_to(cr, center_x, sign_y_in - sign);
  cairo_line_to(cr, center_x, sign_y_in + sign);
  cairo_move_to(cr, center_x - sign, sign_y_out);
  cairo_line_to(cr, center_x + sign, sign_y_out);
  cairo_stroke(cr);
  cairo_restore(cr);
}

/* --- painting ---------------------------------------------------------------------- */

static void _paint_handles(cairo_t *cr, const dt_canvas_view_t *view, const dt_canvas_object_t *object)
{
  const double handle = DT_CANVAS_HANDLE_PIXELS / view->zoom;
  const double hairline = 1.0 / view->zoom;
  cairo_save(cr);
  cairo_translate(cr, object->x, object->y);
  cairo_rotate(cr, object->rotation);
  const double half_width = object->width * 0.5;
  const double half_height = object->height * 0.5;
  cairo_set_line_width(cr, hairline * 1.5);
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.9);
  cairo_rectangle(cr, -half_width, -half_height, object->width, object->height);
  cairo_stroke(cr);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.5);
  const double dashes[2] = { 4.0 * hairline, 4.0 * hairline };
  cairo_set_dash(cr, dashes, 2, 0.0);
  cairo_rectangle(cr, -half_width, -half_height, object->width, object->height);
  cairo_stroke(cr);
  cairo_set_dash(cr, NULL, 0, 0.0);
  if(!(object->flags & DT_CANVAS_OBJECT_FLAG_LOCKED))
  {
    const double corners[8] = { -half_width, -half_height, half_width, -half_height,
                                half_width,  half_height,  -half_width, half_height };
    for(int idx = 0; idx < 4; idx++)
    {
      cairo_rectangle(cr, corners[2 * idx] - handle * 0.5, corners[2 * idx + 1] - handle * 0.5, handle, handle);
    }
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.8);
    cairo_set_line_width(cr, hairline);
    cairo_stroke(cr);
    // The rotation handle floats above the top edge.
    const double rotate_y = -half_height - DT_CANVAS_ROTATE_HANDLE_OFFSET_PIXELS / view->zoom;
    cairo_move_to(cr, 0.0, -half_height);
    cairo_line_to(cr, 0.0, rotate_y);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.7);
    cairo_stroke(cr);
    cairo_arc(cr, 0.0, rotate_y, handle * 0.6, 0.0, 2.0 * M_PI);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.8);
    cairo_stroke(cr);
  }
  cairo_restore(cr);
}

/* --- cutout handles ------------------------------------------------------------------ */

/** The node whose own handles are showing: the nearest one the pointer is still working near. */
static int _mask_node_near(const dt_canvas_view_t *view, const dt_canvas_object_t *object, const double x,
                           const double y)
{
  if(!view->mask_editing || !dt_canvas_object_is_frame(object)) return -1;
  if(object->mask.shape != DT_CANVAS_MASK_POLYGON || object->mask.node_count < 3) return -1;
  double local_x = 0.0;
  double local_y = 0.0;
  dt_canvas_object_to_local(object, x, y, &local_x, &local_y);
  const double reach = DT_CANVAS_HANDLE_PIXELS / view->zoom;
  int nearest = -1;
  double best = INFINITY;
  for(uint32_t idx = 0; idx < object->mask.node_count; idx++)
  {
    const float *node = object->mask.nodes + (size_t)idx * DT_CANVAS_MASK_NODE_FLOATS;
    double node_x = 0.0;
    double node_y = 0.0;
    dt_canvas_mask_to_local(object, node[0], node[1], &node_x, &node_y);
    const double distance = hypot(local_x - node_x, local_y - node_y);
    // Far enough to keep the handles up while the pointer travels out to one of them.
    const double keep = dt_canvas_mask_node_border(&object->mask, idx) * dt_canvas_mask_side(object) + 3.0 * reach;
    if(distance < best && distance <= keep)
    {
      best = distance;
      nearest = (int)idx;
    }
  }
  return nearest;
}

static void _paint_dot(cairo_t *cr, const double x, const double y, const double radius, const double hairline)
{
  cairo_arc(cr, x, y, radius, 0.0, 2.0 * M_PI);
  cairo_set_source_rgba(cr, 1.0, 0.85, 0.3, 0.95);
  cairo_fill_preserve(cr);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.8);
  cairo_set_line_width(cr, hairline);
  cairo_stroke(cr);
}

/** The polygon's closed path in the frame's local units, curved where its nodes are smooth. */
static void _mask_polygon_path(cairo_t *cr, const dt_canvas_object_t *object)
{
  const dt_canvas_mask_t *mask = &object->mask;
  if(mask->node_count < 2) return;
  for(uint32_t idx = 0; idx < mask->node_count; idx++)
  {
    const float *from = mask->nodes + (size_t)idx * DT_CANVAS_MASK_NODE_FLOATS;
    const float *to = mask->nodes + (size_t)((idx + 1) % mask->node_count) * DT_CANVAS_MASK_NODE_FLOATS;
    double from_x = 0.0;
    double from_y = 0.0;
    dt_canvas_mask_to_local(object, from[0], from[1], &from_x, &from_y);
    if(idx == 0) cairo_move_to(cr, from_x, from_y);
    float control1[2];
    float control2[2];
    dt_canvas_mask_polygon_controls(mask, idx, control1, control2);
    double control1_x = 0.0;
    double control1_y = 0.0;
    double control2_x = 0.0;
    double control2_y = 0.0;
    double to_x = 0.0;
    double to_y = 0.0;
    dt_canvas_mask_to_local(object, control1[0], control1[1], &control1_x, &control1_y);
    dt_canvas_mask_to_local(object, control2[0], control2[1], &control2_x, &control2_y);
    dt_canvas_mask_to_local(object, to[0], to[1], &to_x, &to_y);
    cairo_curve_to(cr, control1_x, control1_y, control2_x, control2_y, to_x, to_y);
  }
  cairo_close_path(cr);
}

/** The cutout's outline, its fall-off and its handles, over the selected frame. */
static void _paint_mask_handles(cairo_t *cr, const dt_canvas_view_t *view, const dt_canvas_object_t *object)
{
  const dt_canvas_mask_t *mask = &object->mask;
  if(mask->shape == DT_CANVAS_MASK_NONE) return;
  const double hairline = 1.0 / view->zoom;
  const double handle = DT_CANVAS_HANDLE_PIXELS * 0.6 / view->zoom;
  const double side = dt_canvas_mask_side(object);
  const double dashes[2] = { 4.0 * hairline, 4.0 * hairline };
  cairo_save(cr);
  cairo_translate(cr, object->x, object->y);
  cairo_rotate(cr, object->rotation);
  cairo_set_line_width(cr, hairline * 1.5);
  double points[8] = { 0.0 };
  const int count = dt_canvas_mask_handle_points(object, points);
  const double angle = mask->rotation * M_PI / 180.0;
  cairo_set_source_rgba(cr, 1.0, 0.85, 0.3, 0.9);
  switch(mask->shape)
  {
    case DT_CANVAS_MASK_CIRCLE:
      cairo_arc(cr, points[0], points[1], mask->radius_x * side, 0.0, 2.0 * M_PI);
      cairo_stroke(cr);
      cairo_set_dash(cr, dashes, 2, 0.0);
      cairo_arc(cr, points[0], points[1], (mask->radius_x + mask->feather) * side, 0.0, 2.0 * M_PI);
      cairo_stroke(cr);
      cairo_set_dash(cr, NULL, 0, 0.0);
      break;
    case DT_CANVAS_MASK_ELLIPSE:
      for(int pass = 0; pass < 2; pass++)
      {
        const double grow = pass == 0 ? 0.0 : mask->feather;
        cairo_save(cr);
        cairo_translate(cr, points[0], points[1]);
        cairo_rotate(cr, angle);
        cairo_scale(cr, fmax((mask->radius_x + grow) * side, 1e-3), fmax((mask->radius_y + grow) * side, 1e-3));
        cairo_arc(cr, 0.0, 0.0, 1.0, 0.0, 2.0 * M_PI);
        cairo_restore(cr);
        if(pass == 1) cairo_set_dash(cr, dashes, 2, 0.0);
        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0.0);
      }
      break;
    case DT_CANVAS_MASK_GRADIENT:
    {
      // The line the fall-off starts from, and where it ends.
      const double reach = hypot(object->width, object->height);
      const double dir_x = cos(angle);
      const double dir_y = sin(angle);
      cairo_move_to(cr, points[0] - dir_x * reach, points[1] - dir_y * reach);
      cairo_line_to(cr, points[0] + dir_x * reach, points[1] + dir_y * reach);
      cairo_stroke(cr);
      cairo_set_dash(cr, dashes, 2, 0.0);
      cairo_move_to(cr, points[2] - dir_x * reach, points[3] - dir_y * reach);
      cairo_line_to(cr, points[2] + dir_x * reach, points[3] + dir_y * reach);
      cairo_stroke(cr);
      cairo_set_dash(cr, NULL, 0, 0.0);
      break;
    }
    case DT_CANVAS_MASK_POLYGON:
    {
      _mask_polygon_path(cr, object);
      cairo_stroke(cr);
      // The node the pointer is working near shows what that node alone owns: the two control
      // points its curve leaves by, and its own fall-off. Every node at once would bury the
      // shape under its own handles.
      const int shown = view->drag == DT_CANVAS_DRAG_NONE ? view->mask_node_hover : view->mask_handle;
      if(shown >= 0 && (uint32_t)shown < mask->node_count)
      {
        double border[2] = { 0.0, 0.0 };
        double incoming[2] = { 0.0, 0.0 };
        double outgoing[2] = { 0.0, 0.0 };
        dt_canvas_mask_node_handles(object, (uint32_t)shown, border, incoming, outgoing);
        const float *node = mask->nodes + (size_t)shown * DT_CANVAS_MASK_NODE_FLOATS;
        double node_x = 0.0;
        double node_y = 0.0;
        dt_canvas_mask_to_local(object, node[0], node[1], &node_x, &node_y);
        // The tethers, so it reads which node each handle belongs to.
        cairo_set_line_width(cr, hairline);
        cairo_set_source_rgba(cr, 1.0, 0.85, 0.3, 0.6);
        cairo_move_to(cr, incoming[0], incoming[1]);
        cairo_line_to(cr, node_x, node_y);
        cairo_line_to(cr, outgoing[0], outgoing[1]);
        cairo_stroke(cr);
        cairo_set_dash(cr, dashes, 2, 0.0);
        cairo_move_to(cr, node_x, node_y);
        cairo_line_to(cr, border[0], border[1]);
        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0.0);
        // The control points are round, the fall-off is the dashed one's end: three shapes,
        // three jobs, told apart without a legend.
        for(int which = 0; which < 2; which++)
        {
          const double *point = which == 0 ? incoming : outgoing;
          cairo_arc(cr, point[0], point[1], handle * 0.7, 0.0, 2.0 * M_PI);
          cairo_set_source_rgba(cr, 0.4, 0.8, 1.0, 0.95);
          cairo_fill_preserve(cr);
          cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.8);
          cairo_stroke(cr);
        }
        cairo_arc(cr, border[0], border[1], handle * 0.7, 0.0, 2.0 * M_PI);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.8);
        cairo_stroke(cr);
      }
      for(uint32_t idx = 0; idx < mask->node_count; idx++)
      {
        const float *node = mask->nodes + (size_t)idx * DT_CANVAS_MASK_NODE_FLOATS;
        double local_x = 0.0;
        double local_y = 0.0;
        dt_canvas_mask_to_local(object, node[0], node[1], &local_x, &local_y);
        // Square for a cusp, round for a smooth node, the way the darkroom tells its own
        // two apart -- a toggle nobody can see the result of is a toggle nobody trusts.
        if(node[DT_CANVAS_MASK_NODE_SMOOTH] == (float)DT_CANVAS_MASK_NODE_CUSP)
          cairo_rectangle(cr, local_x - handle, local_y - handle, 2.0 * handle, 2.0 * handle);
        else
          cairo_arc(cr, local_x, local_y, handle, 0.0, 2.0 * M_PI);
        cairo_set_source_rgba(cr, 1.0, 0.85, 0.3, 0.95);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.8);
        cairo_set_line_width(cr, hairline);
        cairo_stroke(cr);
      }
      break;
    }
    default:
      break;
  }
  for(int idx = 0; idx < count; idx++) _paint_dot(cr, points[2 * idx], points[2 * idx + 1], handle, hairline);
  cairo_restore(cr);
}

/**
 * An object's handle sites, in a buffer the caller frees with dt_free(): asked for once to learn
 * how many there are. A polygon may carry hundreds of nodes, which is no size for the stack.
 */
static dt_canvas_handle_site_t *_handle_sites(const dt_canvas_view_t *view, const dt_canvas_object_t *object,
                                              const uint32_t what, size_t *count)
{
  *count = dt_canvas_handle_sites(view->canvas, object, what, NULL, 0);
  if(*count == 0) return NULL;
  dt_canvas_handle_site_t *sites = g_new(dt_canvas_handle_site_t, *count);
  dt_canvas_handle_sites(view->canvas, object, what, sites, *count);
  return sites;
}

/** Which cutout handle is under a canvas point, when the cutout is being edited. */
static dt_canvas_drag_t _mask_handle_at(const dt_canvas_view_t *view, const dt_canvas_object_t *object,
                                        const double x, const double y, int *index)
{
  *index = -1;
  if(!view->mask_editing || !dt_canvas_object_is_frame(object) || object->mask.shape == DT_CANVAS_MASK_NONE)
    return DT_CANVAS_DRAG_NONE;
  if(view->selection->len != 1) return DT_CANVAS_DRAG_NONE;
  if(object->mask.shape == DT_CANVAS_MASK_POLYGON)
  {
    // The handles of the node being worked on sit over the shape and are reached first. Every
    // node has them; only the hovered one's are showing, so only those can be taken.
    if(view->mask_node_hover >= 0 && (uint32_t)view->mask_node_hover < object->mask.node_count)
    {
      size_t count = 0;
      dt_canvas_handle_site_t *sites = _handle_sites(view, object, DT_CANVAS_HANDLES_MASK_NODE_OWN, &count);
      dt_canvas_drag_t drag = DT_CANVAS_DRAG_NONE;
      for(size_t idx = 0; idx < count && drag == DT_CANVAS_DRAG_NONE; idx++)
      {
        const dt_canvas_handle_site_t *site = &sites[idx];
        if(site->index != view->mask_node_hover) continue;
        if(!dt_canvas_handle_site_hit(site, x, y, view->zoom)) continue;
        if(site->part == DT_CANVAS_HANDLE_PART_BORDER)
          drag = DT_CANVAS_DRAG_MASK_NODE_BORDER;
        else if(site->part == DT_CANVAS_HANDLE_PART_INCOMING)
          drag = DT_CANVAS_DRAG_MASK_NODE_CTRL_IN;
        else
          drag = DT_CANVAS_DRAG_MASK_NODE_CTRL_OUT;
      }
      dt_free(sites);
      if(drag != DT_CANVAS_DRAG_NONE)
      {
        *index = view->mask_node_hover;
        return drag;
      }
    }
    *index = _mask_node_at(view, object, x, y);
    return *index >= 0 ? DT_CANVAS_DRAG_MASK_NODE : DT_CANVAS_DRAG_NONE;
  }
  size_t count = 0;
  dt_canvas_handle_site_t *sites = _handle_sites(view, object, DT_CANVAS_HANDLES_MASK_POINTS, &count);
  dt_canvas_drag_t drag = DT_CANVAS_DRAG_NONE;
  // The outer handles first: with a small shape they sit over the centre.
  for(size_t remaining = count; remaining > 0 && drag == DT_CANVAS_DRAG_NONE; remaining--)
  {
    const dt_canvas_handle_site_t *site = &sites[remaining - 1];
    if(!dt_canvas_handle_site_hit(site, x, y, view->zoom)) continue;
    if(site->index == 0)
      drag = DT_CANVAS_DRAG_MASK_CENTER;
    else if(site->index == 3)
      drag = DT_CANVAS_DRAG_MASK_FEATHER;
    else if(site->index == 2)
      drag = DT_CANVAS_DRAG_MASK_RADIUS_Y;
    else
      drag = object->mask.shape == DT_CANVAS_MASK_GRADIENT ? DT_CANVAS_DRAG_MASK_REACH : DT_CANVAS_DRAG_MASK_RADIUS_X;
  }
  dt_free(sites);
  return drag;
}

/**
 * The polygon node under a point, or -1. Deliberately NOT gated on the edit mode, the way
 * `_mask_segment_at()` is not: `_mask_handle_at()` answers what a DRAG would grab and refuses
 * everything while the shape is not being edited, which is right for a drag and wrong for a
 * menu -- the right click that asks about a node is itself the statement of intent, and for
 * as long as the menu asked the drag's question its node entries were unreachable.
 */
static int _mask_node_at(const dt_canvas_view_t *view, const dt_canvas_object_t *object, const double x,
                         const double y)
{
  if(!dt_canvas_object_is_frame(object) || object->mask.shape != DT_CANVAS_MASK_POLYGON) return -1;
  size_t count = 0;
  dt_canvas_handle_site_t *sites = _handle_sites(view, object, DT_CANVAS_HANDLES_MASK_NODES, &count);
  int node = -1;
  for(size_t idx = 0; idx < count && node < 0; idx++)
  {
    if(dt_canvas_handle_site_hit(&sites[idx], x, y, view->zoom)) node = sites[idx].index;
  }
  dt_free(sites);
  return node;
}

/** The polygon edge under a canvas point: the index of the node it starts at, or -1. */
static int _mask_segment_at(const dt_canvas_view_t *view, const dt_canvas_object_t *object, const double x,
                            const double y)
{
  if(!dt_canvas_object_is_frame(object) || object->mask.shape != DT_CANVAS_MASK_POLYGON) return -1;
  size_t count = 0;
  dt_canvas_handle_site_t *sites = _handle_sites(view, object, DT_CANVAS_HANDLES_MASK_EDGES, &count);
  int edge = -1;
  for(size_t idx = 0; idx < count && edge < 0; idx++)
  {
    if(dt_canvas_handle_site_hit(&sites[idx], x, y, view->zoom)) edge = sites[idx].index;
  }
  dt_free(sites);
  return edge;
}

/** Apply a cutout drag: the handle follows the pointer, in the frame's own unit square. */
static void _mask_drag(dt_canvas_view_t *view, dt_canvas_object_t *object, const double x, const double y)
{
  dt_canvas_mask_t *mask = &object->mask;
  double local_x = 0.0;
  double local_y = 0.0;
  dt_canvas_object_to_local(object, x, y, &local_x, &local_y);
  const double side = dt_canvas_mask_side(object);
  const double u = local_x / object->width + 0.5;
  const double v = local_y / object->height + 0.5;
  double center_x = 0.0;
  double center_y = 0.0;
  dt_canvas_mask_to_local(object, mask->center_x, mask->center_y, &center_x, &center_y);
  const double delta_x = local_x - center_x;
  const double delta_y = local_y - center_y;
  const double distance = hypot(delta_x, delta_y);
  const double angle = mask->rotation * M_PI / 180.0;
  switch(view->drag)
  {
    case DT_CANVAS_DRAG_MASK_CENTER:
      mask->center_x = (float)u;
      mask->center_y = (float)v;
      break;
    case DT_CANVAS_DRAG_MASK_RADIUS_X:
      mask->radius_x = (float)fmax(distance / side, 0.005);
      if(mask->shape == DT_CANVAS_MASK_ELLIPSE) mask->rotation = (float)(atan2(delta_y, delta_x) * 180.0 / M_PI);
      break;
    case DT_CANVAS_DRAG_MASK_RADIUS_Y:
      mask->radius_y = (float)fmax(fabs(-sin(angle) * delta_x + cos(angle) * delta_y) / side, 0.005);
      break;
    case DT_CANVAS_DRAG_MASK_REACH:
      mask->radius_x = (float)CLAMP(distance / side, 0.0005, 1.0);
      mask->rotation = (float)(atan2(delta_y, delta_x) * 180.0 / M_PI - 90.0);
      break;
    case DT_CANVAS_DRAG_MASK_FEATHER:
    {
      // The ring's distance from the shape's edge: past the radius for a circle, the first radius for an ellipse.
      const double edge = mask->shape == DT_CANVAS_MASK_ELLIPSE
                              ? fabs(cos(angle) * delta_x + sin(angle) * delta_y) / side
                              : distance / side;
      mask->feather = (float)CLAMP(edge - mask->radius_x, 0.0, 1.0);
      break;
    }
    case DT_CANVAS_DRAG_MASK_NODE_BORDER:
      if(view->mask_handle >= 0 && (uint32_t)view->mask_handle < mask->node_count)
      {
        // How far the handle was pulled from its node IS the fall-off, the way the circle's
        // and the ellipse's read theirs off their dashed ring.
        float *node = mask->nodes + (size_t)view->mask_handle * DT_CANVAS_MASK_NODE_FLOATS;
        double node_x = 0.0;
        double node_y = 0.0;
        dt_canvas_mask_to_local(object, node[0], node[1], &node_x, &node_y);
        const float own = (float)CLAMP(hypot(local_x - node_x, local_y - node_y) / side, 0.001, 2.0);
        node[DT_CANVAS_MASK_NODE_BORDER1] = own;
        node[DT_CANVAS_MASK_NODE_BORDER2] = own;
      }
      break;
    case DT_CANVAS_DRAG_MASK_NODE_CTRL_IN:
    case DT_CANVAS_DRAG_MASK_NODE_CTRL_OUT:
      if(view->mask_handle >= 0 && (uint32_t)view->mask_handle < mask->node_count)
      {
        // Steering a control point makes the tangent the user's: the curve stops being
        // computed through the node, so the other control is written down as it stood and
        // nothing jumps.
        float *node = mask->nodes + (size_t)view->mask_handle * DT_CANVAS_MASK_NODE_FLOATS;
        float incoming[2];
        float outgoing[2];
        dt_canvas_mask_node_controls(mask, (uint32_t)view->mask_handle, incoming, outgoing);
        node[DT_CANVAS_MASK_NODE_CTRL1_X] = incoming[0];
        node[DT_CANVAS_MASK_NODE_CTRL1_Y] = incoming[1];
        node[DT_CANVAS_MASK_NODE_CTRL2_X] = outgoing[0];
        node[DT_CANVAS_MASK_NODE_CTRL2_Y] = outgoing[1];
        // A smooth node STAYS smooth while it is steered -- that is the whole difference
        // between the two kinds, and without it the first touch of a handle turned every
        // node into a cusp and there was no way to give a smooth node a tangent of its own.
        const gboolean smooth = node[DT_CANVAS_MASK_NODE_SMOOTH] != (float)DT_CANVAS_MASK_NODE_CUSP;
        node[DT_CANVAS_MASK_NODE_SMOOTH]
            = (float)(smooth ? DT_CANVAS_MASK_NODE_STEERED : DT_CANVAS_MASK_NODE_CUSP);
        const int base = view->drag == DT_CANVAS_DRAG_MASK_NODE_CTRL_IN ? DT_CANVAS_MASK_NODE_CTRL1_X
                                                                        : DT_CANVAS_MASK_NODE_CTRL2_X;
        node[base] = (float)u;
        node[base + 1] = (float)v;
        // The handle drives the tangent's DIRECTION and its TENSION: on a smooth node the
        // opposite control turns with it, through the node, and keeps the length it had, so
        // one handle sets which way the curve leaves and how hard it pulls. On a cusp the
        // two sides are free of each other and only the dragged one moves.
        const int other = base == DT_CANVAS_MASK_NODE_CTRL1_X ? DT_CANVAS_MASK_NODE_CTRL2_X
                                                              : DT_CANVAS_MASK_NODE_CTRL1_X;
        const double pull_x = (double)node[base] - node[DT_CANVAS_MASK_NODE_X];
        const double pull_y = (double)node[base + 1] - node[DT_CANVAS_MASK_NODE_Y];
        const double pull = hypot(pull_x, pull_y);
        if(smooth && pull > 1e-6)
        {
          const double other_x = (double)node[other] - node[DT_CANVAS_MASK_NODE_X];
          const double other_y = (double)node[other + 1] - node[DT_CANVAS_MASK_NODE_Y];
          // A control sitting on its node has no length to keep: give it the dragged one's,
          // so a node whose tangent was never pulled out becomes symmetric rather than
          // staying collapsed on one side.
          const double kept = fmax(hypot(other_x, other_y), pull);
          node[other] = (float)(node[DT_CANVAS_MASK_NODE_X] - pull_x / pull * kept);
          node[other + 1] = (float)(node[DT_CANVAS_MASK_NODE_Y] - pull_y / pull * kept);
        }
      }
      break;
    case DT_CANVAS_DRAG_MASK_NODE:
      if(view->mask_handle >= 0 && (uint32_t)view->mask_handle < mask->node_count)
      {
        float *node = mask->nodes + (size_t)view->mask_handle * DT_CANVAS_MASK_NODE_FLOATS;
        node[0] = (float)u;
        node[1] = (float)v;
        node[2] = node[0];
        node[3] = node[1];
        node[4] = node[0];
        node[5] = node[1];
      }
      break;
    default:
      break;
  }
  dt_canvas_touch(view->canvas);
}

static void _paint_badge(cairo_t *cr, const dt_canvas_view_t *view, const dt_canvas_object_t *object)
{
  if(object->kind != DT_CANVAS_OBJECT_IMAGE && object->kind != DT_CANVAS_OBJECT_MAP) return;
  double red = 0.0;
  double green = 0.0;
  double blue = 0.0;
  switch(object->kind == DT_CANVAS_OBJECT_MAP ? object->map.sync_status : object->image.sync_status)
  {
    case DT_CANVAS_SYNC_STALE:
      red = 1.0;
      green = 0.65;
      break;
    case DT_CANVAS_SYNC_MISSING:
      red = 0.9;
      green = 0.2;
      blue = 0.2;
      break;
    case DT_CANVAS_SYNC_RENDERING:
      red = 0.3;
      green = 0.6;
      blue = 1.0;
      break;
    default:
      return;
  }
  const double radius = CANVAS_BADGE_PIXELS / view->zoom;
  double corners[8];
  dt_canvas_object_corners(object, corners);
  cairo_save(cr);
  cairo_arc(cr, corners[2] - radius * 1.5, corners[3] + radius * 1.5, radius, 0.0, 2.0 * M_PI);
  cairo_set_source_rgba(cr, red, green, blue, 0.95);
  cairo_fill_preserve(cr);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.7);
  cairo_set_line_width(cr, 1.0 / view->zoom);
  cairo_stroke(cr);
  cairo_restore(cr);
}

/**
 * A text frame set to follow its content takes the height its text actually needs. It is done
 * here, once per frame, because the height depends on the laid-out text and the text depends
 * on everything that can change it -- the markdown, the font, the margins, the width. Only a
 * height that actually MOVED touches the canvas, so this settles on the first frame and does
 * not hand the painter a new generation for ever.
 */
void expose(dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  const gboolean first_layout = view->width <= 0 || view->height <= 0;
  view->width = width;
  view->height = height;
  if(first_layout && view->canvas->view_zoom == 1.0 && view->canvas->view_x == 0.0 && view->canvas->view_y == 0.0
     && dt_canvas_object_count(view->canvas) > 0)
    _zoom_fit(view);

  cairo_save(cr);
  cairo_translate(cr, width * 0.5, height * 0.5);
  cairo_scale(cr, view->zoom, view->zoom);
  cairo_translate(cr, -view->center_x, -view->center_y);
  dt_canvas_paint_options_t options = dt_canvas_paint_options_display(view->cache, 1.0 / view->zoom, _visible_rect(view));
  options.quality = view->interacting ? 0.5 : 1.0;
  dt_canvas_paint(cr, view->canvas, &options);

  for(guint idx = 0; idx < dt_canvas_object_count(view->canvas); idx++)
  {
    const dt_canvas_object_t *object = dt_canvas_object_at(view->canvas, idx);
    if(object->flags & DT_CANVAS_OBJECT_FLAG_HIDDEN) continue;
    _paint_badge(cr, view, object);
  }
  for(guint idx = 0; idx < view->selection->len; idx++)
  {
    const dt_canvas_object_t *object = dt_canvas_find_object(view->canvas, g_array_index(view->selection, uint32_t, idx));
    if(dt_canvas_object_is_frame(object))
    {
      _paint_handles(cr, view, object);
      if(view->mask_editing && view->selection->len == 1) _paint_mask_handles(cr, view, object);
    }
    dt_canvas_route_t route;
    if(_connector_handles(view, object, &route))
    {
      // The tangent handles: one per end along its anchor's normal, two about the waypoint.
      cairo_save(cr);
      _paint_tangent_handle(cr, view, route.from_x, route.from_y, route.control1_x, route.control1_y);
      if(route.segment_count == 2)
      {
        _paint_tangent_handle(cr, view, route.via_x, route.via_y, route.control2_x, route.control2_y);
        _paint_tangent_handle(cr, view, route.via_x, route.via_y, route.control3_x, route.control3_y);
        _paint_tangent_handle(cr, view, route.to_x, route.to_y, route.control4_x, route.control4_y);
      }
      else
      {
        _paint_tangent_handle(cr, view, route.to_x, route.to_y, route.control2_x, route.control2_y);
      }
      cairo_restore(cr);
    }
    if(!IS_NULL_PTR(object) && object->kind == DT_CANVAS_OBJECT_CONNECTOR && object->connector.via_count > 0)
    {
      // The waypoint, as a diamond the pointer can take hold of.
      const double handle = DT_CANVAS_VIA_HANDLE_PIXELS / view->zoom;
      cairo_save(cr);
      cairo_move_to(cr, object->connector.via_x, object->connector.via_y - handle);
      cairo_line_to(cr, object->connector.via_x + handle, object->connector.via_y);
      cairo_line_to(cr, object->connector.via_x, object->connector.via_y + handle);
      cairo_line_to(cr, object->connector.via_x - handle, object->connector.via_y);
      cairo_close_path(cr);
      cairo_set_source_rgba(cr, 1.0, 0.75, 0.2, 0.95);
      cairo_fill_preserve(cr);
      cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.8);
      cairo_set_line_width(cr, 1.0 / view->zoom);
      cairo_stroke(cr);
      cairo_restore(cr);
    }
  }
  if(view->hover != 0 && !_is_selected(view, view->hover))
  {
    const dt_canvas_object_t *object = dt_canvas_find_object(view->canvas, view->hover);
    if(dt_canvas_object_is_frame(object))
    {
      // Two lines, light against the frame and dark just outside it: one of the two is
      // legible whatever the frame and the canvas are, where a single pale hairline
      // disappeared over anything bright. Both sit past the frame's edge, so what they
      // outline is never covered by them.
      const double hairline = 1.0 / view->zoom;
      const double half_width = object->width * 0.5;
      const double half_height = object->height * 0.5;
      cairo_save(cr);
      cairo_translate(cr, object->x, object->y);
      cairo_rotate(cr, object->rotation);
      cairo_set_line_width(cr, hairline);
      const double insets[2] = { 0.5 * hairline, 1.5 * hairline };
      const double shades[2] = { 1.0, 0.0 };
      const double alphas[2] = { 0.9, 0.7 };
      for(int line = 0; line < 2; line++)
      {
        cairo_set_source_rgba(cr, shades[line], shades[line], shades[line], alphas[line]);
        cairo_rectangle(cr, -half_width - insets[line], -half_height - insets[line],
                        object->width + 2.0 * insets[line], object->height + 2.0 * insets[line]);
        cairo_stroke(cr);
      }
      cairo_restore(cr);
    }
  }
  if(view->drag == DT_CANVAS_DRAG_SCALE && (view->guide_width_valid || view->guide_height_valid))
  {
    // The frame(s) the size was taken from, and a line along the matched dimension on both.
    const dt_canvas_object_t *resized = _single_selected(view);
    cairo_save(cr);
    cairo_set_source_rgba(cr, 0.3, 0.75, 1.0, 0.95);
    cairo_set_line_width(cr, 1.5 / view->zoom);
    const double dashes[2] = { 6.0 / view->zoom, 4.0 / view->zoom };
    cairo_set_dash(cr, dashes, 2, 0.0);
    if(view->guide_width_valid)
    {
      const dt_canvas_rect_t reference = view->guide_width;
      cairo_rectangle(cr, reference.x, reference.y, reference.width, reference.height);
      cairo_stroke(cr);
      cairo_set_dash(cr, NULL, 0, 0.0);
      cairo_move_to(cr, reference.x, reference.y - 8.0 / view->zoom);
      cairo_line_to(cr, reference.x + reference.width, reference.y - 8.0 / view->zoom);
      if(!IS_NULL_PTR(resized))
      {
        const dt_canvas_rect_t bounds = dt_canvas_object_bounds(resized);
        cairo_move_to(cr, bounds.x, bounds.y - 8.0 / view->zoom);
        cairo_line_to(cr, bounds.x + bounds.width, bounds.y - 8.0 / view->zoom);
      }
      cairo_stroke(cr);
      cairo_set_dash(cr, dashes, 2, 0.0);
    }
    if(view->guide_height_valid)
    {
      const dt_canvas_rect_t reference = view->guide_height;
      cairo_rectangle(cr, reference.x, reference.y, reference.width, reference.height);
      cairo_stroke(cr);
      cairo_set_dash(cr, NULL, 0, 0.0);
      cairo_move_to(cr, reference.x - 8.0 / view->zoom, reference.y);
      cairo_line_to(cr, reference.x - 8.0 / view->zoom, reference.y + reference.height);
      if(!IS_NULL_PTR(resized))
      {
        const dt_canvas_rect_t bounds = dt_canvas_object_bounds(resized);
        cairo_move_to(cr, bounds.x - 8.0 / view->zoom, bounds.y);
        cairo_line_to(cr, bounds.x - 8.0 / view->zoom, bounds.y + bounds.height);
      }
      cairo_stroke(cr);
    }
    cairo_restore(cr);
  }
  if(view->drag == DT_CANVAS_DRAG_RUBBERBAND)
  {
    cairo_save(cr);
    cairo_rectangle(cr, fmin(view->press_x, view->pointer_x), fmin(view->press_y, view->pointer_y),
                    fabs(view->pointer_x - view->press_x), fabs(view->pointer_y - view->press_y));
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.12);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.7);
    cairo_set_line_width(cr, 1.0 / view->zoom);
    cairo_stroke(cr);
    cairo_restore(cr);
  }
  _paint_connect_mode(cr, view);
  cairo_restore(cr);

  // Status line: file name, zoom, and what the pointer is over.
  cairo_save(cr);
  gchar *status = NULL;
  const char *file_name = IS_NULL_PTR(view->canvas->path) ? _("untitled") : view->canvas->path;
  if(dt_canvas_object_count(view->canvas) == 0)
    status = g_strdup_printf(_("%s%s — %d%% — drop images from the filmstrip"), file_name,
                             view->canvas->dirty ? "*" : "", (int)lround(view->zoom * 100.0));
  else
    status = g_strdup_printf("%s%s — %d%%", file_name, view->canvas->dirty ? "*" : "", (int)lround(view->zoom * 100.0));
  // Ink against the plane's luminance, and a halo of the opposite so it reads over a picture too.
  // A paper is painted in the canvas's own colour -- the relief is a zero-mean modulation of
  // it -- so that one colour answers for every style that paints something, and a second copy
  // of the tint table here would only go stale as papers are added. A hole shows the view's
  // own backdrop instead, which is dark whatever colour the document carries.
  double background[3] = { 0.2, 0.2, 0.2 };
  if(!dt_canvas_background_is_transparent(view->canvas->background_style))
  {
    background[0] = view->canvas->background.red;
    background[1] = view->canvas->background.green;
    background[2] = view->canvas->background.blue;
  }
  const double luminance = 0.2126 * background[0] + 0.7152 * background[1] + 0.0722 * background[2];
  const double ink = luminance > 0.5 ? 0.1 : 0.9;
  const double halo = luminance > 0.5 ? 1.0 : 0.0;
  cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, DT_PIXEL_APPLY_DPI(11));
  cairo_move_to(cr, DT_PIXEL_APPLY_DPI(8), height - DT_PIXEL_APPLY_DPI(8));
  cairo_text_path(cr, status);
  cairo_set_source_rgba(cr, halo, halo, halo, 0.55);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.5));
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  cairo_stroke_preserve(cr);
  cairo_set_source_rgba(cr, ink, ink, ink, 0.85);
  cairo_fill(cr);
  dt_free(status);
  cairo_restore(cr);

  _paint_flower(cr, view);
  _paint_props_placement(cr, view);
}

/* --- gestures ---------------------------------------------------------------------- */

/** Which handle of a selected frame is under the canvas point: 0..3 a corner, 4 the rotation, -1 none. */
static int _handle_at(const dt_canvas_view_t *view, const dt_canvas_object_t *object, const double x, const double y)
{
  size_t count = 0;
  dt_canvas_handle_site_t *sites = _handle_sites(view, object, DT_CANVAS_HANDLES_FRAME, &count);
  int handle = -1;
  // The corners come before the knob, which is the order the sites are listed in.
  for(size_t idx = 0; idx < count && handle < 0; idx++)
  {
    const dt_canvas_handle_site_t *site = &sites[idx];
    if(site->role != DT_CANVAS_HANDLE_CORNER && site->role != DT_CANVAS_HANDLE_ROTATE) continue;
    if(!dt_canvas_handle_site_hit(site, x, y, view->zoom)) continue;
    handle = site->role == DT_CANVAS_HANDLE_ROTATE ? 4 : site->index;
  }
  dt_free(sites);
  return handle;
}

/** The tangent handles of a selected cubic connector, in canvas units: control points of its route. */
static gboolean _connector_handles(const dt_canvas_view_t *view, const dt_canvas_object_t *connector,
                                   dt_canvas_route_t *route)
{
  if(IS_NULL_PTR(connector) || connector->kind != DT_CANVAS_OBJECT_CONNECTOR) return FALSE;
  if(connector->connector.routing != DT_CANVAS_ROUTING_CUBIC) return FALSE;
  return dt_canvas_connector_route(view->canvas, connector, route);
}

/** Which tangent handle of a selected connector is under the canvas point, and on which connector. */
static dt_canvas_drag_t _tangent_handle_at(const dt_canvas_view_t *view, const double x, const double y,
                                           dt_canvas_object_t **owner, int *sign)
{
  for(guint idx = 0; idx < view->selection->len; idx++)
  {
    dt_canvas_object_t *object = dt_canvas_find_object(view->canvas, g_array_index(view->selection, uint32_t, idx));
    size_t count = 0;
    dt_canvas_handle_site_t *sites = _handle_sites(view, object, DT_CANVAS_HANDLES_TANGENTS, &count);
    dt_canvas_drag_t drag = DT_CANVAS_DRAG_NONE;
    // A connector's control points in route order: the start's, the waypoint's either side, the end's.
    for(size_t site_idx = 0; site_idx < count && drag == DT_CANVAS_DRAG_NONE; site_idx++)
    {
      const dt_canvas_handle_site_t *site = &sites[site_idx];
      if(site->role != DT_CANVAS_HANDLE_TANGENT) continue;
      if(!dt_canvas_handle_site_hit(site, x, y, view->zoom)) continue;
      if(site->part == DT_CANVAS_HANDLE_PART_FROM)
        drag = DT_CANVAS_DRAG_HANDLE_FROM;
      else if(site->part == DT_CANVAS_HANDLE_PART_TO)
        drag = DT_CANVAS_DRAG_HANDLE_TO;
      else
      {
        drag = DT_CANVAS_DRAG_HANDLE_VIA;
        *sign = site->sign;
      }
    }
    dt_free(sites);
    if(drag == DT_CANVAS_DRAG_NONE) continue;
    *owner = object;
    return drag;
  }
  *owner = NULL;
  return DT_CANVAS_DRAG_NONE;
}

static void _paint_tangent_handle(cairo_t *cr, const dt_canvas_view_t *view, const double anchor_x,
                                  const double anchor_y, const double handle_x, const double handle_y)
{
  const double radius = (DT_CANVAS_VIA_HANDLE_PIXELS - 2.0) / view->zoom;
  // A dark halo under the light line, so the handle reads on a bright ground too.
  cairo_move_to(cr, anchor_x, anchor_y);
  cairo_line_to(cr, handle_x, handle_y);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.6);
  cairo_set_line_width(cr, 3.0 / view->zoom);
  cairo_stroke_preserve(cr);
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.9);
  cairo_set_line_width(cr, 1.0 / view->zoom);
  cairo_stroke(cr);
  cairo_arc(cr, handle_x, handle_y, radius, 0.0, 2.0 * M_PI);
  cairo_set_source_rgba(cr, 0.3, 0.75, 1.0, 0.95);
  cairo_fill_preserve(cr);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.9);
  cairo_set_line_width(cr, 1.5 / view->zoom);
  cairo_stroke(cr);
}

/** Is the canvas point on a selected connector's waypoint handle? */
static dt_canvas_object_t *_via_handle_at(const dt_canvas_view_t *view, const double x, const double y)
{
  for(guint idx = 0; idx < view->selection->len; idx++)
  {
    dt_canvas_object_t *object = dt_canvas_find_object(view->canvas, g_array_index(view->selection, uint32_t, idx));
    size_t count = 0;
    dt_canvas_handle_site_t *sites = _handle_sites(view, object, DT_CANVAS_HANDLES_VIA, &count);
    gboolean hit = FALSE;
    for(size_t site_idx = 0; site_idx < count && !hit; site_idx++)
      hit = dt_canvas_handle_site_hit(&sites[site_idx], x, y, view->zoom);
    dt_free(sites);
    if(hit) return object;
  }
  return NULL;
}

static void _move_selection(dt_canvas_view_t *view, const double delta_x, const double delta_y)
{
  for(guint idx = 0; idx < view->selection->len; idx++)
  {
    dt_canvas_object_t *object = dt_canvas_find_object(view->canvas, g_array_index(view->selection, uint32_t, idx));
    if(!dt_canvas_object_is_frame(object) || (object->flags & DT_CANVAS_OBJECT_FLAG_LOCKED)) continue;
    object->x += delta_x;
    object->y += delta_y;
  }
}

/**
 * Snap the dragged frame, moving the rest of the selection with it, in the canvas's order of
 * rules: the grid first, then a neighbour one padding away or in line, which wins when within reach.
 */
static void _snap_selection(dt_canvas_view_t *view, const uint32_t leader_id)
{
  const dt_canvas_object_t *leader = dt_canvas_find_object(view->canvas, leader_id);
  if(!dt_canvas_object_is_frame(leader)) return;
  const uint32_t rules = view->canvas->grid_flags;
  dt_canvas_rect_t bounds = dt_canvas_object_bounds(leader);
  double delta_x = 0.0;
  double delta_y = 0.0;
  if(rules & DT_CANVAS_GRID_SNAP)
  {
    delta_x = dt_canvas_snap(view->canvas, bounds.x) - bounds.x;
    delta_y = dt_canvas_snap(view->canvas, bounds.y) - bounds.y;
  }
  if(rules & DT_CANVAS_SNAP_PADDING)
  {
    double padding_x = 0.0;
    double padding_y = 0.0;
    dt_canvas_snap_to_neighbours(view->canvas, &bounds, view->selection, CANVAS_NEIGHBOUR_SNAP_PIXELS / view->zoom,
                                 DT_CANVAS_EDGE_ALL, &padding_x, &padding_y);
    if(padding_x != 0.0) delta_x = padding_x;
    if(padding_y != 0.0) delta_y = padding_y;
  }
  if(rules & DT_CANVAS_SNAP_PAGE)
  {
    double page_x = 0.0;
    double page_y = 0.0;
    dt_canvas_snap_to_pages(view->canvas, &bounds, CANVAS_NEIGHBOUR_SNAP_PIXELS / view->zoom, DT_CANVAS_EDGE_ALL,
                            &page_x, &page_y);
    if(page_x != 0.0) delta_x = page_x;
    if(page_y != 0.0) delta_y = page_y;
  }
  if(delta_x != 0.0 || delta_y != 0.0) _move_selection(view, delta_x, delta_y);
}

static void _scale_object(dt_canvas_view_t *view, dt_canvas_object_t *object, const double x, const double y)
{
  // The dragged corner follows the pointer; the opposite corner stays put. A picture and a
  // drawing keep their proportions unless told not to; a text frame has none to keep.
  double local_x = 0.0;
  double local_y = 0.0;
  dt_canvas_object_to_local(object, x, y, &local_x, &local_y);
  const double sign_x = (view->scale_corner == 1 || view->scale_corner == 2) ? 1.0 : -1.0;
  const double sign_y = (view->scale_corner == 2 || view->scale_corner == 3) ? 1.0 : -1.0;
  const gboolean proportional = dt_canvas_object_keeps_ratio(object);
  const double old_width = object->width;
  const double old_height = object->height;
  const double ratio = old_height > 0.0 ? old_width / old_height : 1.0;
  // Distance from the fixed corner to the pointer, along the dragged corner's diagonal.
  const double span_x = fmax((local_x * sign_x) + old_width * 0.5, 20.0);
  const double span_y = fmax((local_y * sign_y) + old_height * 0.5, 20.0);
  double new_width = span_x;
  double new_height = span_y;
  if(proportional)
  {
    new_height = span_x / ratio;
    if(span_y * ratio > span_x)
    {
      new_height = span_y;
      new_width = span_y * ratio;
    }
  }

  // Snapping, in the canvas's order of rules: the grid, then the padding, then a neighbour's size.
  // Each later rule that triggers replaces the earlier answer; a proportional frame follows its width.
  const uint32_t rules = view->canvas->grid_flags;
  const double threshold = CANVAS_NEIGHBOUR_SNAP_PIXELS / view->zoom;
  if(rules & DT_CANVAS_GRID_SNAP)
  {
    new_width = fmax(dt_canvas_snap(view->canvas, new_width), 20.0);
    new_height = proportional ? new_width / ratio : fmax(dt_canvas_snap(view->canvas, new_height), 20.0);
  }
  if(rules & DT_CANVAS_SNAP_PADDING)
  {
    // Only the dragged edges may snap; the box is the frame as it would be, unrotated.
    dt_canvas_rect_t box;
    box.width = new_width;
    box.height = new_height;
    box.x = object->x + (sign_x > 0.0 ? -old_width * 0.5 : old_width * 0.5 - new_width);
    box.y = object->y + (sign_y > 0.0 ? -old_height * 0.5 : old_height * 0.5 - new_height);
    const uint32_t edges = (sign_x > 0.0 ? DT_CANVAS_EDGE_RIGHT : DT_CANVAS_EDGE_LEFT)
                           | (sign_y > 0.0 ? DT_CANVAS_EDGE_BOTTOM : DT_CANVAS_EDGE_TOP);
    double delta_x = 0.0;
    double delta_y = 0.0;
    if(dt_canvas_snap_to_neighbours(view->canvas, &box, view->selection, threshold, edges, &delta_x, &delta_y))
    {
      if(delta_x != 0.0) new_width = fmax(new_width + delta_x * sign_x, 20.0);
      if(delta_y != 0.0) new_height = fmax(new_height + delta_y * sign_y, 20.0);
      if(proportional) new_height = new_width / ratio;
    }
  }
  if(rules & DT_CANVAS_SNAP_PAGE)
  {
    dt_canvas_rect_t box;
    box.width = new_width;
    box.height = new_height;
    box.x = object->x + (sign_x > 0.0 ? -old_width * 0.5 : old_width * 0.5 - new_width);
    box.y = object->y + (sign_y > 0.0 ? -old_height * 0.5 : old_height * 0.5 - new_height);
    const uint32_t edges = (sign_x > 0.0 ? DT_CANVAS_EDGE_RIGHT : DT_CANVAS_EDGE_LEFT)
                           | (sign_y > 0.0 ? DT_CANVAS_EDGE_BOTTOM : DT_CANVAS_EDGE_TOP);
    double delta_x = 0.0;
    double delta_y = 0.0;
    if(dt_canvas_snap_to_pages(view->canvas, &box, threshold, edges, &delta_x, &delta_y))
    {
      if(delta_x != 0.0) new_width = fmax(new_width + delta_x * sign_x, 20.0);
      if(delta_y != 0.0) new_height = fmax(new_height + delta_y * sign_y, 20.0);
      if(proportional) new_height = new_width / ratio;
    }
  }
  view->guide_width_valid = FALSE;
  view->guide_height_valid = FALSE;
  if(rules & DT_CANVAS_SNAP_SIZE)
  {
    double snapped_width = new_width;
    double snapped_height = new_height;
    dt_canvas_rect_t width_reference = { 0.0, 0.0, 0.0, 0.0 };
    dt_canvas_rect_t height_reference = { 0.0, 0.0, 0.0, 0.0 };
    if(dt_canvas_snap_size(view->canvas, view->selection, threshold, &snapped_width, &snapped_height,
                           &width_reference, &height_reference))
    {
      const gboolean width_snapped = snapped_width != new_width;
      const gboolean height_snapped = snapped_height != new_height;
      if(proportional)
      {
        // Match the width when it snapped, else the height; the other follows the ratio.
        if(width_snapped)
          new_width = snapped_width;
        else
          new_width = snapped_height * ratio;
        new_height = new_width / ratio;
      }
      else
      {
        new_width = snapped_width;
        new_height = snapped_height;
      }
      if(width_snapped)
      {
        view->guide_width_valid = TRUE;
        view->guide_width = width_reference;
      }
      if(height_snapped && (!proportional || !width_snapped))
      {
        view->guide_height_valid = TRUE;
        view->guide_height = height_reference;
      }
    }
  }
  // Keep the opposite corner fixed: the centre moves by half the size change along the diagonal.
  const double shift_x = (new_width - old_width) * 0.5 * sign_x;
  const double shift_y = (new_height - old_height) * 0.5 * sign_y;
  const double cos_r = cos(object->rotation);
  const double sin_r = sin(object->rotation);
  object->x += shift_x * cos_r - shift_y * sin_r;
  object->y += shift_x * sin_r + shift_y * cos_r;
  object->width = new_width;
  object->height = new_height;
}

static void _end_gesture(dt_view_t *self)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(view->drag == DT_CANVAS_DRAG_MOVE || view->drag == DT_CANVAS_DRAG_SCALE || view->drag == DT_CANVAS_DRAG_ROTATE
     || view->drag == DT_CANVAS_DRAG_VIA || view->drag == DT_CANVAS_DRAG_HANDLE_FROM
     || view->drag == DT_CANVAS_DRAG_HANDLE_TO || view->drag == DT_CANVAS_DRAG_HANDLE_VIA
     || view->drag >= DT_CANVAS_DRAG_MASK_CENTER)
  {
    if(view->drag_moved)
    {
      // The gesture is over, so the geometry is final: fit every auto-height frame to it now,
      // once. A frame that was moved, resized or rotated changes what its own text flows
      // around, and so does one that merely passed over another frame's text.
      dt_canvas_props_settle_all(view->canvas);
      dt_canvas_touch(view->canvas);
      _record_undo(self, view->drag_snapshot);
      view->drag_snapshot = NULL;
      // A resized map is fetched again at its new size, so the crop it shows is at full detail.
      dt_canvas_object_t *resized = _single_selected(view);
      if(view->drag == DT_CANVAS_DRAG_SCALE && !IS_NULL_PTR(resized) && resized->kind == DT_CANVAS_OBJECT_MAP)
        _start_map_render(self, resized);
    }
  }
  else if(view->drag == DT_CANVAS_DRAG_RUBBERBAND)
  {
    dt_canvas_rect_t band;
    band.x = fmin(view->press_x, view->pointer_x);
    band.y = fmin(view->press_y, view->pointer_y);
    band.width = fabs(view->pointer_x - view->press_x);
    band.height = fabs(view->pointer_y - view->press_y);
    for(guint idx = 0; idx < dt_canvas_object_count(view->canvas); idx++)
    {
      const dt_canvas_object_t *object = dt_canvas_object_at(view->canvas, idx);
      if(!dt_canvas_object_is_frame(object) || (object->flags & DT_CANVAS_OBJECT_FLAG_HIDDEN)) continue;
      const dt_canvas_rect_t bounds = dt_canvas_object_bounds(object);
      const gboolean inside = bounds.x >= band.x && bounds.y >= band.y
                              && bounds.x + bounds.width <= band.x + band.width
                              && bounds.y + bounds.height <= band.y + band.height;
      if(inside && !_is_selected(view, object->id)) g_array_append_val(view->selection, object->id);
    }
  }
  dt_canvas_free(view->drag_snapshot);
  view->drag_snapshot = NULL;
  view->drag = DT_CANVAS_DRAG_NONE;
  view->drag_moved = FALSE;
  view->guide_width_valid = FALSE;
  view->guide_height_valid = FALSE;
  view->cursor = GDK_LEFT_PTR;
  dt_control_change_cursor(GDK_LEFT_PTR);
  // Properties the gesture hid come back; a rubber band that changed the selection closes them.
  _props_sync(self);
  dt_control_queue_redraw_center();
}

/**
 * Drop the gesture a press armed, leaving the document as it is: the second press of a double
 * click arms a move before GDK reports the double click, and that move is not what the user
 * asked for. Nothing has moved yet, so there is nothing to restore and no undo step to record.
 */
static void _gesture_cancel(dt_view_t *self)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  dt_canvas_free(view->drag_snapshot);
  view->drag_snapshot = NULL;
  view->drag = DT_CANVAS_DRAG_NONE;
  view->drag_moved = FALSE;
  view->guide_width_valid = FALSE;
  view->guide_height_valid = FALSE;
  view->cursor = GDK_LEFT_PTR;
  dt_control_change_cursor(GDK_LEFT_PTR);
}

/** Snapshot the document for the gesture a press arms, dropping one a previous press left. */
static void _gesture_snapshot(dt_canvas_view_t *view)
{
  dt_canvas_free(view->drag_snapshot);
  view->drag_snapshot = _begin_edit(view);
}

/** The object whose properties are on screen right now, 0 when they are closed or hidden. */
static uint32_t _props_shown_id(const dt_canvas_view_t *view)
{
  GtkWidget *root = _props_root(view);
  if(IS_NULL_PTR(root) || view->props_suspended || !gtk_widget_get_visible(root)) return 0;
  const dt_canvas_object_t *object = _props_object(view);
  return IS_NULL_PTR(object) ? 0 : object->id;
}

/**
 * Count a press into its run of clicks, with the toolkit's own double-click delay and distance
 * and the event's own timestamp, so the run ends where GDK stops pairing presses. Outside of an
 * event dispatch there is no timestamp, and the monotonic clock stands in: a gap measured across
 * the two clocks is meaningless, which only ever begins a new run.
 */
static void _click_sequence_press(dt_canvas_view_t *view, const double x, const double y, const int button)
{
  int delay_ms = 0;
  int distance_px = 0;
  g_object_get(gtk_settings_get_default(), "gtk-double-click-time", &delay_ms, "gtk-double-click-distance",
               &distance_px, NULL);
  guint32 time_ms = gtk_get_current_event_time();
  if(time_ms == GDK_CURRENT_TIME) time_ms = (guint32)(g_get_monotonic_time() / 1000);
  const dt_canvas_click_t click = { .time_ms = time_ms, .x = x, .y = y, .button = button };
  dt_canvas_click_sequence_press(&view->click_sequence, &click, (guint)MAX(delay_ms, 0), (guint)MAX(distance_px, 0),
                                 _props_shown_id(view));
}

/**
 * A left press on one of the handles of what is selected: a cutout's node or edge, a connector's
 * tangent or waypoint, a frame's corner or knob. They come before any object, since they sit over
 * the objects they belong to. TRUE when a handle took the press.
 */
static gboolean _press_handles(dt_view_t *self, const double canvas_x, const double canvas_y, const int type,
                               const gboolean primary, const gboolean shift)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  // The cutout's handles, when it is being edited: they sit over the frame they cut.
  dt_canvas_object_t *mask_owner = _single_selected(view);
  int mask_index = -1;
  const dt_canvas_drag_t mask_drag = _mask_handle_at(view, mask_owner, canvas_x, canvas_y, &mask_index);
  if(mask_drag != DT_CANVAS_DRAG_NONE)
  {
    if(mask_drag == DT_CANVAS_DRAG_MASK_NODE && type == GDK_2BUTTON_PRESS)
    {
      dt_canvas_t *before = _begin_edit(view);
      _mask_node_toggle_smooth(&mask_owner->mask, (uint32_t)mask_index);
      dt_canvas_touch(view->canvas);
      _record_undo(self, before);
      _end_gesture(self);
      return TRUE;
    }
    if(mask_drag == DT_CANVAS_DRAG_MASK_NODE && shift)
    {
      dt_canvas_t *before = _begin_edit(view);
      if(dt_canvas_mask_remove_node(view->canvas, mask_owner, (uint32_t)mask_index))
        _record_undo(self, before);
      else
        dt_canvas_free(before);
      dt_control_queue_redraw_center();
      return TRUE;
    }
    _gesture_snapshot(view);
    view->drag = mask_drag;
    view->mask_handle = mask_index;
    dt_control_change_cursor(GDK_FLEUR);
    return TRUE;
  }
  if(primary && view->mask_editing && !IS_NULL_PTR(mask_owner))
  {
    const int segment = _mask_segment_at(view, mask_owner, canvas_x, canvas_y);
    if(segment >= 0)
    {
      double local_x = 0.0;
      double local_y = 0.0;
      dt_canvas_object_to_local(mask_owner, canvas_x, canvas_y, &local_x, &local_y);
      dt_canvas_t *before = _begin_edit(view);
      if(dt_canvas_mask_insert_node(view->canvas, mask_owner, (uint32_t)segment + 1,
                                    (float)(local_x / mask_owner->width + 0.5), (float)(local_y / mask_owner->height + 0.5)))
        _record_undo(self, before);
      else
        dt_canvas_free(before);
      dt_control_queue_redraw_center();
      return TRUE;
    }
  }
  dt_canvas_object_t *handle_owner = NULL;
  int handle_sign = 1;
  const dt_canvas_drag_t handle_drag = _tangent_handle_at(view, canvas_x, canvas_y, &handle_owner, &handle_sign);
  if(handle_drag != DT_CANVAS_DRAG_NONE)
  {
    _select_only(view, handle_owner->id);
    _gesture_snapshot(view);
    view->drag = handle_drag;
    view->handle_sign = handle_sign;
    _props_sync(self);
    dt_control_change_cursor(GDK_FLEUR);
    return TRUE;
  }
  dt_canvas_object_t *via_owner = _via_handle_at(view, canvas_x, canvas_y);
  if(!IS_NULL_PTR(via_owner))
  {
    _select_only(view, via_owner->id);
    _gesture_snapshot(view);
    view->drag = DT_CANVAS_DRAG_VIA;
    _props_sync(self);
    dt_control_change_cursor(GDK_FLEUR);
    return TRUE;
  }
  // Handles of the selected frames come first: they overlap the frames they belong to.
  for(guint idx = 0; idx < view->selection->len; idx++)
  {
    dt_canvas_object_t *object = dt_canvas_find_object(view->canvas, g_array_index(view->selection, uint32_t, idx));
    const int handle = _handle_at(view, object, canvas_x, canvas_y);
    if(handle < 0) continue;
    _select_only(view, object->id);
    _gesture_snapshot(view);
    _props_sync(self);
    if(handle == 4)
    {
      view->drag = DT_CANVAS_DRAG_ROTATE;
      view->gesture_start_rotation = object->rotation;
      view->gesture_start_angle = atan2(canvas_y - object->y, canvas_x - object->x);
    }
    else
    {
      view->drag = DT_CANVAS_DRAG_SCALE;
      view->scale_corner = handle;
    }
    dt_control_queue_redraw_center();
    return TRUE;
  }
  return FALSE;
}

int button_pressed(dt_view_t *self, double x, double y, double pressure, int which, int type, uint32_t state)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  double canvas_x = 0.0;
  double canvas_y = 0.0;
  _to_canvas(view, x, y, &canvas_x, &canvas_y);
  view->press_x = canvas_x;
  view->press_y = canvas_y;
  view->press_screen_x = x;
  view->press_screen_y = y;
  view->last_x = canvas_x;
  view->last_y = canvas_y;
  view->pointer_x = canvas_x;
  view->pointer_y = canvas_y;
  view->drag_moved = FALSE;
  const gboolean primary = dt_modifier_is(state, DT_PRIMARY_MASK);
  const gboolean shift = dt_modifier_is(state, GDK_SHIFT_MASK);
  const double tolerance = DT_CANVAS_PICK_TOLERANCE_PIXELS / view->zoom;
  // Digits typed into the properties are already committed here: the view manager takes the focus
  // away before the view hears of the press (view.c), and a spin button commits on focus-out. What can
  // be left is a session nothing holds -- wheel notches over a slider, waiting for their pause -- and it
  // ends here, as its own undo step, before the press changes anything.
  _props_commit_pending(self);

  // Every press of every button is counted into its run of clicks BEFORE it changes anything, so
  // the run knows whose properties were on screen when it came. GDK reports the press completing
  // a double or a triple click a second time, as GDK_2BUTTON_PRESS or GDK_3BUTTON_PRESS: those
  // are not presses of their own.
  if(type == GDK_BUTTON_PRESS) _click_sequence_press(view, x, y, which);

  // The flower floats over the plane: a press on it is navigation, never a pick.
  const dt_canvas_flower_part_t flower_part = _flower_hit(view, x, y);
  if(flower_part != DT_CANVAS_FLOWER_NONE)
  {
    if(which == 1) _flower_activate(view, flower_part);
    return 1;
  }

  if(which == 2 || (which == 1 && dt_modifier_is(state, GDK_MOD1_MASK)))
  {
    view->drag = DT_CANVAS_DRAG_PAN;
    dt_control_change_cursor(GDK_FLEUR);
    return 1;
  }

  if(view->connecting)
  {
    if(which == 1) _connect_click(self, canvas_x, canvas_y);
    else if(which == 3) _connect_mode_set(self, FALSE);
    return 1;
  }

  if(which == 1)
  {
    // A double click takes a handle only where its first press took one: that first press can
    // only take the handles of what was already selected, so on a frame it has just selected the
    // second press finds handles the user never aimed at, a corner covering most of a small
    // frame, and the double click belongs to the frame.
    const gboolean handles_answer
        = type != GDK_2BUTTON_PRESS || dt_canvas_click_sequence_began_on_handle(&view->click_sequence);
    if(handles_answer && _press_handles(self, canvas_x, canvas_y, type, primary, shift))
    {
      if(type == GDK_BUTTON_PRESS) dt_canvas_click_sequence_took_handle(&view->click_sequence);
      return 1;
    }

    dt_canvas_object_t *object = dt_canvas_pick(view->canvas, canvas_x, canvas_y, tolerance);
    if(!IS_NULL_PTR(object))
    {
      if(type == GDK_2BUTTON_PRESS)
      {
        // The second press has just armed a move of the object, which the double click takes
        // back before anything happens.
        _gesture_cancel(self);
        // With Shift or Ctrl held, a click adds to the selection or takes from it, and two of them
        // are two toggles: the selection being built stays as they left it, and nothing opens.
        if(primary || shift)
        {
          dt_control_queue_redraw_center();
          return 1;
        }
        // The drill rule. The double click's own first press says whether this object's
        // properties were on screen: if not, they open; if they were, the double click goes into
        // the object. Both wait for an idle, so no dialog and no view switch runs inside the press.
        const dt_canvas_double_click_t answer = dt_canvas_click_sequence_double(&view->click_sequence, object->id);
        if(answer == DT_CANVAS_DOUBLE_CLICK_OPEN)
          _props_request(self, object->id, FALSE, TRUE, canvas_x, canvas_y);
        else if(answer == DT_CANVAS_DOUBLE_CLICK_DRILL && dt_canvas_props_has_content_action(object->kind))
          _props_request(self, object->id, TRUE, TRUE, canvas_x, canvas_y);
        dt_control_queue_redraw_center();
        return 1;
      }
      if(primary || shift)
        _select_toggle(view, object->id);
      else if(!_is_selected(view, object->id))
        _select_only(view, object->id);
      if(dt_canvas_object_is_frame(object) && _is_selected(view, object->id))
      {
        view->drag = DT_CANVAS_DRAG_MOVE;
        _gesture_snapshot(view);
        dt_control_change_cursor(GDK_FLEUR);
      }
      _props_sync(self);
      dt_control_queue_redraw_center();
      return 1;
    }

    // The background: the selection and the properties go at once, before the rubber band starts.
    if(!primary && !shift) g_array_set_size(view->selection, 0);
    view->drag = DT_CANVAS_DRAG_RUBBERBAND;
    _props_sync(self);
    dt_control_queue_redraw_center();
    return 1;
  }

  if(which == 3)
  {
    dt_canvas_object_t *object = dt_canvas_pick(view->canvas, canvas_x, canvas_y, tolerance);
    if(!IS_NULL_PTR(object) && !_is_selected(view, object->id)) _select_only(view, object->id);
    _props_sync(self);
    _popup_menu(self, object, canvas_x, canvas_y);
    dt_control_queue_redraw_center();
    return 1;
  }
  return 0;
}

static const dt_cursor_t _corner_cursors[4]
    = { GDK_TOP_LEFT_CORNER, GDK_TOP_RIGHT_CORNER, GDK_BOTTOM_RIGHT_CORNER, GDK_BOTTOM_LEFT_CORNER };

/** The cursor that names what a press here would do. */
static void _queue_cursor_for(dt_view_t *self, const double screen_x, const double screen_y, const double x,
                              const double y, const dt_canvas_flower_part_t flower_part,
                              const dt_canvas_object_t *under)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  dt_cursor_t cursor = GDK_LEFT_PTR;
  if(flower_part != DT_CANVAS_FLOWER_NONE)
  {
    cursor = GDK_HAND2;
  }
  else if(view->connecting)
  {
    cursor = view->anchor_hover != DT_CANVAS_ANCHOR_AUTO ? GDK_CROSSHAIR : GDK_LEFT_PTR;
  }
  else if(_mask_handle_at(view, _single_selected(view), x, y, &(int){ -1 }) != DT_CANVAS_DRAG_NONE)
  {
    cursor = GDK_FLEUR;
  }
  else if(!IS_NULL_PTR(_via_handle_at(view, x, y)))
  {
    cursor = GDK_FLEUR;
  }
  else if(_tangent_handle_at(view, x, y, &(dt_canvas_object_t *){ NULL }, &(int){ 1 }) != DT_CANVAS_DRAG_NONE)
  {
    cursor = GDK_FLEUR;
  }
  else
  {
    gboolean on_handle = FALSE;
    for(guint idx = 0; idx < view->selection->len && !on_handle; idx++)
    {
      const dt_canvas_object_t *object
          = dt_canvas_find_object(view->canvas, g_array_index(view->selection, uint32_t, idx));
      const int handle = _handle_at(view, object, x, y);
      if(handle < 0) continue;
      on_handle = TRUE;
      if(handle == 4)
        cursor = GDK_EXCHANGE;
      else
      {
        // The corner's cursor follows the frame's rotation by quarter turns.
        const int quarter = (int)lround(object->rotation / (M_PI / 2.0));
        cursor = _corner_cursors[((handle + quarter) % 4 + 4) % 4];
      }
    }
    if(!on_handle && !IS_NULL_PTR(under))
      cursor = under->kind == DT_CANVAS_OBJECT_CONNECTOR ? GDK_HAND1
               : (under->flags & DT_CANVAS_OBJECT_FLAG_LOCKED) ? GDK_LEFT_PTR : GDK_HAND1;
  }
  if(cursor != view->cursor)
  {
    view->cursor = cursor;
    dt_control_change_cursor(cursor);
  }
}

/**
 * Whether this gesture is changing the document rather than the view. Every one of these owes
 * the canvas a touch per motion: the painter keeps the frame it last composited and blits it
 * again for a key it has already seen, and the document's generation is what tells the two
 * apart. Without it a drag paints its first frame over and over and the object, the route or
 * the waypoint only catches up when something else moves the key -- which is what "the path
 * does not follow the handle" looks like from the outside.
 */
static gboolean _drag_changes_the_document(const dt_canvas_drag_t drag)
{
  switch(drag)
  {
    case DT_CANVAS_DRAG_NONE:
    case DT_CANVAS_DRAG_PAN:
    case DT_CANVAS_DRAG_RUBBERBAND:
      return FALSE;
    default:
      return TRUE;
  }
}

void mouse_moved(dt_view_t *self, double x, double y, double pressure, int which)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  double canvas_x = 0.0;
  double canvas_y = 0.0;
  _to_canvas(view, x, y, &canvas_x, &canvas_y);
  view->pointer_inside = TRUE;
  if(view->props_pointer_inside)
  {
    // The drawing area only hears the pointer that is not over the properties: whatever crossing
    // was missed, it has left them, and a placement they held back can run -- from an idle, never
    // from here.
    view->props_pointer_inside = FALSE;
    if(view->props_place_pending) _props_sync(self);
  }
  const double delta_x = canvas_x - view->last_x;
  const double delta_y = canvas_y - view->last_y;

  switch(view->drag)
  {
    case DT_CANVAS_DRAG_PAN:
      _interaction_touch(self);
      view->center_x -= delta_x;
      view->center_y -= delta_y;
      // The pan changed the mapping: recompute where the pointer is now.
      _to_canvas(view, x, y, &canvas_x, &canvas_y);
      break;
    case DT_CANVAS_DRAG_MOVE:
      if(!view->drag_moved
         && hypot(x - view->press_screen_x, y - view->press_screen_y) < CANVAS_DRAG_THRESHOLD_PIXELS)
        break;
      view->drag_moved = TRUE;
      _interaction_touch(self);
      _move_selection(view, delta_x, delta_y);
      if(view->selection->len > 0) _snap_selection(view, g_array_index(view->selection, uint32_t, 0));
      break;
    case DT_CANVAS_DRAG_SCALE:
    {
      dt_canvas_object_t *object = _single_selected(view);
      if(!IS_NULL_PTR(object))
      {
        view->drag_moved = TRUE;
        _interaction_touch(self);
        _scale_object(view, object, canvas_x, canvas_y);
      }
      break;
    }
    case DT_CANVAS_DRAG_ROTATE:
    {
      dt_canvas_object_t *object = _single_selected(view);
      if(!IS_NULL_PTR(object))
      {
        view->drag_moved = TRUE;
        _interaction_touch(self);
        const double angle = atan2(canvas_y - object->y, canvas_x - object->x);
        double rotation = view->gesture_start_rotation + angle - view->gesture_start_angle;
        // Shift snaps to 15 degree steps, Ctrl to 45: upright, on its side, or on a diagonal.
        if(dt_modifier_is(which, GDK_SHIFT_MASK)) rotation = round(rotation / (M_PI / 12.0)) * (M_PI / 12.0);
        if(dt_modifier_is(which, DT_PRIMARY_MASK)) rotation = round(rotation / (M_PI / 4.0)) * (M_PI / 4.0);
        object->rotation = rotation;
      }
      break;
    }
    case DT_CANVAS_DRAG_VIA:
    {
      dt_canvas_object_t *connector = _single_selected(view);
      if(!IS_NULL_PTR(connector) && connector->kind == DT_CANVAS_OBJECT_CONNECTOR)
      {
        view->drag_moved = TRUE;
        double via_x = canvas_x;
        double via_y = canvas_y;
        _axis_lock(view, which, &via_x, &via_y);
        connector->connector.via_x = dt_canvas_snap(view->canvas, via_x);
        connector->connector.via_y = dt_canvas_snap(view->canvas, via_y);
      }
      break;
    }
    case DT_CANVAS_DRAG_HANDLE_FROM:
    case DT_CANVAS_DRAG_HANDLE_TO:
    {
      // The handle stays on the anchor's normal, orthogonal to the frame's edge: only its length moves.
      dt_canvas_object_t *connector = _single_selected(view);
      dt_canvas_route_t route;
      if(_connector_handles(view, connector, &route))
      {
        view->drag_moved = TRUE;
        const gboolean start = view->drag == DT_CANVAS_DRAG_HANDLE_FROM;
        const double anchor_x = start ? route.from_x : route.to_x;
        const double anchor_y = start ? route.from_y : route.to_y;
        const double normal_x = start ? route.from_normal_x : route.to_normal_x;
        const double normal_y = start ? route.from_normal_y : route.to_normal_y;
        const double reach = fmax((canvas_x - anchor_x) * normal_x + (canvas_y - anchor_y) * normal_y, 10.0);
        if(start)
          connector->connector.from_reach = (float)reach;
        else
          connector->connector.to_reach = (float)reach;
      }
      break;
    }
    case DT_CANVAS_DRAG_HANDLE_VIA:
    {
      dt_canvas_object_t *connector = _single_selected(view);
      if(!IS_NULL_PTR(connector) && connector->kind == DT_CANVAS_OBJECT_CONNECTOR && connector->connector.via_count > 0)
      {
        view->drag_moved = TRUE;
        double tangent_x = canvas_x;
        double tangent_y = canvas_y;
        _angle_lock(which, connector->connector.via_x, connector->connector.via_y, &tangent_x, &tangent_y);
        connector->connector.via_tangent_x = (tangent_x - connector->connector.via_x) * view->handle_sign;
        connector->connector.via_tangent_y = (tangent_y - connector->connector.via_y) * view->handle_sign;
      }
      break;
    }
    case DT_CANVAS_DRAG_MASK_CENTER:
    case DT_CANVAS_DRAG_MASK_RADIUS_X:
    case DT_CANVAS_DRAG_MASK_RADIUS_Y:
    case DT_CANVAS_DRAG_MASK_REACH:
    case DT_CANVAS_DRAG_MASK_NODE:
    case DT_CANVAS_DRAG_MASK_FEATHER:
    case DT_CANVAS_DRAG_MASK_NODE_BORDER:
    case DT_CANVAS_DRAG_MASK_NODE_CTRL_IN:
    case DT_CANVAS_DRAG_MASK_NODE_CTRL_OUT:
    {
      dt_canvas_object_t *object = _single_selected(view);
      if(dt_canvas_object_is_frame(object) && object->mask.shape != DT_CANVAS_MASK_NONE)
      {
        view->drag_moved = TRUE;
        _interaction_touch(self);
        double handle_x = canvas_x;
        double handle_y = canvas_y;
        _axis_lock(view, which, &handle_x, &handle_y);
        _mask_drag(view, object, handle_x, handle_y);
      }
      break;
    }
    case DT_CANVAS_DRAG_RUBBERBAND:
      break;
    default:
    {
      const dt_canvas_flower_part_t flower_part = _flower_hit(view, x, y);
      if(flower_part != view->flower_hover)
      {
        view->flower_hover = flower_part;
        dt_control_queue_redraw_center();
      }
      const int mask_node = _mask_node_near(view, _single_selected(view), canvas_x, canvas_y);
      if(mask_node != view->mask_node_hover)
      {
        view->mask_node_hover = mask_node;
        dt_control_queue_redraw_center();
      }
      const double tolerance = DT_CANVAS_PICK_TOLERANCE_PIXELS / view->zoom;
      const dt_canvas_object_t *object
          = flower_part == DT_CANVAS_FLOWER_NONE ? dt_canvas_pick(view->canvas, canvas_x, canvas_y, tolerance) : NULL;
      const uint32_t hover = IS_NULL_PTR(object) ? 0 : object->id;
      if(hover != view->hover)
      {
        view->hover = hover;
        dt_control_queue_redraw_center();
      }
      if(view->connecting)
      {
        // The anchors of the frame under the pointer are shown; the one within reach lights up.
        uint32_t anchor_frame = 0;
        uint32_t anchor = DT_CANVAS_ANCHOR_AUTO;
        if(!_anchor_at(view, canvas_x, canvas_y, &anchor_frame, &anchor))
        {
          anchor_frame = dt_canvas_object_is_frame(object) ? object->id : 0;
          anchor = DT_CANVAS_ANCHOR_AUTO;
        }
        view->anchor_hover_id = anchor_frame;
        view->anchor_hover = anchor;
        dt_control_queue_redraw_center();
      }
      _queue_cursor_for(self, x, y, canvas_x, canvas_y, flower_part, object);
      view->pointer_x = canvas_x;
      view->pointer_y = canvas_y;
      view->last_x = canvas_x;
      view->last_y = canvas_y;
      return;
    }
  }
  if(_drag_changes_the_document(view->drag)) dt_canvas_touch(view->canvas);
  // A gesture that has really moved something hides the properties until it settles. A rubber
  // band moves nothing, so the same threshold as a move decides when it has started.
  const gboolean band_started = view->drag == DT_CANVAS_DRAG_RUBBERBAND
                                && hypot(x - view->press_screen_x, y - view->press_screen_y)
                                       >= CANVAS_DRAG_THRESHOLD_PIXELS;
  if(view->drag_moved || view->drag == DT_CANVAS_DRAG_PAN || band_started) _props_suspend(self);
  view->pointer_x = canvas_x;
  view->pointer_y = canvas_y;
  view->last_x = canvas_x;
  view->last_y = canvas_y;
  dt_control_queue_redraw_center();
}

int button_released(dt_view_t *self, double x, double y, int which, uint32_t state)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(view->drag == DT_CANVAS_DRAG_NONE) return 0;
  _end_gesture(self);
  return 1;
}

void mouse_leave(dt_view_t *self)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  view->pointer_inside = FALSE;
  if(view->cursor != GDK_LEFT_PTR && view->drag == DT_CANVAS_DRAG_NONE)
  {
    view->cursor = GDK_LEFT_PTR;
    dt_control_change_cursor(GDK_LEFT_PTR);
  }
  if(view->hover != 0 || view->flower_hover != DT_CANVAS_FLOWER_NONE || view->mask_node_hover >= 0)
  {
    view->hover = 0;
    view->flower_hover = DT_CANVAS_FLOWER_NONE;
    view->mask_node_hover = -1;
    view->cursor = GDK_LEFT_PTR;
    dt_control_queue_redraw_center();
  }
}

int scrolled(dt_view_t *self, double x, double y, int up, int state, int delta_y)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  // Over the frame whose cutout is being edited, the wheel edits the cutout: the feather,
  // with Shift the opacity, with Ctrl the gradient's curvature or the ellipse's rotation.
  dt_canvas_object_t *edited = _single_selected(view);
  if(view->mask_editing && dt_canvas_object_is_frame(edited) && edited->mask.shape != DT_CANVAS_MASK_NONE)
  {
    double canvas_x = 0.0;
    double canvas_y = 0.0;
    _to_canvas(view, x, y, &canvas_x, &canvas_y);
    if(dt_canvas_object_contains(view->canvas, edited, canvas_x, canvas_y, DT_CANVAS_PICK_TOLERANCE_PIXELS / view->zoom))
    {
      // The card sits beside the frame, and a wheel over its Feather a moment ago left a session open:
      // committed later, it would come after this step in the history and write its feather over this one.
      _props_commit_pending(self);
      dt_canvas_t *before = _begin_edit(view);
      const double step = up ? 1.0 : -1.0;
      if(dt_modifier_is(state, DT_PRIMARY_MASK))
      {
        if(edited->mask.shape == DT_CANVAS_MASK_GRADIENT)
          edited->mask.radius_y = (float)CLAMP(edited->mask.radius_y + 0.1 * step, -2.0, 2.0);
        else if(edited->mask.shape == DT_CANVAS_MASK_ELLIPSE)
          edited->mask.rotation = (float)(edited->mask.rotation + 5.0 * step);
      }
      else if(dt_modifier_is(state, GDK_SHIFT_MASK))
        edited->transparency = (float)CLAMP(edited->transparency - 0.05 * step, 0.0, 1.0);
      else
        edited->mask.feather = (float)CLAMP(edited->mask.feather + 0.01 * step, 0.0, 1.0);
      dt_canvas_touch(view->canvas);
      _record_undo(self, before);
      _props_sync(self);
      dt_control_queue_redraw_center();
      return 1;
    }
  }
  _interaction_touch(self);
  if(dt_modifier_is(state, GDK_SHIFT_MASK))
  {
    // Shift + wheel pans sideways, plain wheel zooms: a plane has no natural scroll direction.
    view->center_x += (up ? -1.0 : 1.0) * 60.0 / view->zoom;
  }
  else
  {
    _zoom_around(view, x, y, up ? CANVAS_ZOOM_STEP : 1.0 / CANVAS_ZOOM_STEP);
  }
  _props_sync(self);
  dt_control_queue_redraw_center();
  return 1;
}

int key_pressed(dt_view_t *self, GdkEventKey *event)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  const guint key = dt_keys_mainpad_alternatives(event->keyval);
  const gboolean primary = dt_modifiers_include(event->state, DT_PRIMARY_MASK);
  // Keys the properties' own controls did not take reach here: while one of them holds the focus,
  // the view's own keys are theirs and never the object's.
  if(_props_key_guard(view, key)) return 1;
  // Whatever the properties hold is committed before a key acts on the document -- by each branch
  // below that acts, and by no other key. A modifier pressed on its own reaches here too, and so does
  // every key nothing reads: committing on those would apply digits half typed into a spin (Shift
  // gives the digits on some keyboards) and cut a Shift+wheel or Ctrl+arrow burst on a slider into
  // two undo steps.
  if(key == GDK_KEY_Escape)
  {
    _props_commit_pending(self);
    // One step back per press: out of connector drawing, out of the gesture, the properties
    // closed, and only then the selection dropped. An opening or a content action still waiting
    // to run is taken back whichever step this is.
    _props_request_drop(view);
    if(view->connecting)
      _connect_mode_set(self, FALSE);
    else if(view->drag != DT_CANVAS_DRAG_NONE)
    {
      // Abort the gesture: put the document back the way it was before the press.
      if(!IS_NULL_PTR(view->drag_snapshot)) dt_canvas_restore(view->canvas, view->drag_snapshot);
      _gesture_cancel(self);
    }
    else if(view->props_id != 0)
      _props_close(self);
    else
      g_array_set_size(view->selection, 0);
    _props_sync(self);
    dt_control_queue_redraw_center();
    return 1;
  }
  if(key == GDK_KEY_Return && !view->connecting && view->drag == DT_CANVAS_DRAG_NONE
     && dt_modifier_is(event->state, 0))
  {
    // Return goes into the one selected object, the way a second double click does.
    const dt_canvas_object_t *object = _single_selected(view);
    if(IS_NULL_PTR(object) || !dt_canvas_props_has_content_action(object->kind)) return 0;
    // Committed by the request itself, when it runs.
    _props_request(self, object->id, TRUE, FALSE, 0.0, 0.0);
    return 1;
  }
  if(key == GDK_KEY_Delete || key == GDK_KEY_BackSpace)
  {
    _props_commit_pending(self);
    _delete_selection(self);
    return 1;
  }
  if(primary && (key == GDK_KEY_a || key == GDK_KEY_A))
  {
    _props_commit_pending(self);
    _select_all(view);
    _props_sync(self);
    return 1;
  }
  const double nudge = (event->state & GDK_SHIFT_MASK) ? 10.0 / view->zoom : 1.0 / view->zoom;
  double nudge_x = 0.0;
  double nudge_y = 0.0;
  if(key == GDK_KEY_Left) nudge_x = -nudge;
  else if(key == GDK_KEY_Right) nudge_x = nudge;
  else if(key == GDK_KEY_Up) nudge_y = -nudge;
  else if(key == GDK_KEY_Down) nudge_y = nudge;
  if((nudge_x != 0.0 || nudge_y != 0.0) && view->selection->len > 0)
  {
    _props_commit_pending(self);
    dt_canvas_t *before = _begin_edit(view);
    _move_selection(view, nudge_x, nudge_y);
    dt_canvas_touch(view->canvas);
    _record_undo(self, before);
    _props_sync(self);
    dt_control_queue_redraw_center();
    return 1;
  }
  return 0;
}

/* --- actions, proxy and accelerators ---------------------------------------------------- */

static void _proxy_action(dt_view_t *self, int action)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view) || IS_NULL_PTR(view->canvas)) return;
  // An undo must find the edit in the properties recorded, and a new document must not inherit it.
  _props_commit_pending(self);
  switch((dt_canvas_action_t)action)
  {
    case DT_CANVAS_ACTION_NEW:
      if(_confirm_discard(self)) _set_document(self, NULL);
      break;
    case DT_CANVAS_ACTION_OPEN:
      _open_canvas(self);
      break;
    case DT_CANVAS_ACTION_SAVE:
      _save(self);
      break;
    case DT_CANVAS_ACTION_SAVE_AS:
      _save_as(self);
      break;
    case DT_CANVAS_ACTION_EXPORT:
      _export_canvas(self);
      break;
    case DT_CANVAS_ACTION_ADD_TEXT:
      _add_text_frame(self, view->center_x, view->center_y);
      break;
    case DT_CANVAS_ACTION_ADD_NOTES:
      _add_notes(self);
      break;
    case DT_CANVAS_ACTION_ADD_SVG:
      _add_drawing(self, view->center_x, view->center_y);
      break;
    case DT_CANVAS_ACTION_ADD_MAP:
      _add_map(self, view->center_x, view->center_y, dt_conf_get_float("canvas/map_latitude"),
               dt_conf_get_float("canvas/map_longitude"));
      break;
    case DT_CANVAS_ACTION_ZOOM_FIT:
      _zoom_fit(view);
      break;
    case DT_CANVAS_ACTION_ZOOM_100:
      _set_zoom(view, 1.0);
      break;
    case DT_CANVAS_ACTION_SELECT_ALL:
      _select_all(view);
      break;
    case DT_CANVAS_ACTION_DELETE:
      _delete_selection(self);
      break;
    case DT_CANVAS_ACTION_SYNC_CHECK:
    {
      _sync_check_all(view);
      int stale = 0;
      int missing = 0;
      for(guint idx = 0; idx < dt_canvas_object_count(view->canvas); idx++)
      {
        const dt_canvas_object_t *object = dt_canvas_object_at(view->canvas, idx);
        if(object->kind != DT_CANVAS_OBJECT_IMAGE) continue;
        if(object->image.sync_status == DT_CANVAS_SYNC_STALE) stale++;
        if(object->image.sync_status == DT_CANVAS_SYNC_MISSING) missing++;
      }
      dt_control_log(_("canvas check: %d image(s) to refresh, %d missing from the library"), stale, missing);
      break;
    }
    case DT_CANVAS_ACTION_SYNC_REFRESH_STALE:
      _sync_check_all(view);
      _refresh_sidecar_texts(view);
      dt_control_log(ngettext("re-rendering %d image", "re-rendering %d images", 0), _refresh_images(self, DT_CANVAS_SYNC_STALE));
      break;
    case DT_CANVAS_ACTION_SYNC_REFRESH_ALL:
      _refresh_sidecar_texts(view);
      dt_control_log(ngettext("re-rendering %d image", "re-rendering %d images", 0), _refresh_images(self, DT_CANVAS_SYNC_UNKNOWN));
      break;
    case DT_CANVAS_ACTION_LAYOUT_GRID:
      _apply_layout(self, DT_CANVAS_LAYOUT_GRID);
      break;
    case DT_CANVAS_ACTION_LAYOUT_MASONRY:
      _apply_layout(self, DT_CANVAS_LAYOUT_MASONRY);
      break;
    case DT_CANVAS_ACTION_LAYOUT_ROW:
      _apply_layout(self, DT_CANVAS_LAYOUT_ROW);
      break;
    case DT_CANVAS_ACTION_LAYOUT_COLUMN:
      _apply_layout(self, DT_CANVAS_LAYOUT_COLUMN);
      break;
    case DT_CANVAS_ACTION_TOGGLE_GRID:
      view->canvas->grid_flags ^= DT_CANVAS_GRID_VISIBLE;
      dt_conf_set_bool("canvas/grid_visible", (view->canvas->grid_flags & DT_CANVAS_GRID_VISIBLE) != 0);
      dt_canvas_touch(view->canvas);
      break;
    case DT_CANVAS_ACTION_TOGGLE_SNAP:
      // The shortcut cycles: nothing, grid, all.
      if(!(view->canvas->grid_flags & DT_CANVAS_SNAP_ALL))
        view->canvas->grid_flags |= DT_CANVAS_GRID_SNAP;
      else if((view->canvas->grid_flags & DT_CANVAS_SNAP_ALL) == DT_CANVAS_GRID_SNAP)
        view->canvas->grid_flags |= DT_CANVAS_SNAP_ALL;
      else
        view->canvas->grid_flags &= ~(uint32_t)DT_CANVAS_SNAP_ALL;
      dt_conf_set_int("canvas/snap_mode", (int)(view->canvas->grid_flags & DT_CANVAS_SNAP_ALL));
      dt_canvas_touch(view->canvas);
      break;
    case DT_CANVAS_ACTION_UNDO:
      dt_undo_do_undo(dt_undo_get_global(), DT_UNDO_CANVAS);
      break;
    case DT_CANVAS_ACTION_REDO:
      dt_undo_do_redo(dt_undo_get_global(), DT_UNDO_CANVAS);
      break;
    case DT_CANVAS_ACTION_CONNECT_MODE:
      _connect_mode_set(self, !view->connecting);
      break;
    case DT_CANVAS_ACTION_PROPERTIES:
    {
      // The keyboard's way to what a double click opens, for the one selected object.
      const dt_canvas_object_t *object = _single_selected(view);
      if(IS_NULL_PTR(object))
        dt_control_log(_("select one object to show its properties"));
      else if(view->props_id != object->id)
        _props_open(self, object->id, FALSE, 0.0, 0.0);
      else if(_props_shown_id(view) == object->id)
        // Open already: the key takes the keyboard into them, to their first control.
        dt_canvas_props_gtk_focus_first(view->props);
      break;
    }
    default:
      break;
  }
  _props_sync(self);
  _announce_document(self);
}

/**
 * The four texture weights back to 1: the paper as it was designed. Choosing a paper is
 * choosing a whole surface, so the weights the previous one was tuned with do not carry over
 * -- they are the canvas's, not each paper's, and leaving them on means the sheet that
 * arrives is not the one the list named.
 */
static void _texture_to_defaults(dt_canvas_t *canvas)
{
  canvas->texture_contrast = 1.0f;
  canvas->texture_detail = 1.0f;
  canvas->texture_scale = 1.0f;
  canvas->texture_grain = 1.0f;
  dt_conf_set_float("canvas/texture_contrast", 1.0);
  dt_conf_set_float("canvas/texture_detail", 1.0);
  dt_conf_set_float("canvas/texture_scale", 1.0);
  dt_conf_set_float("canvas/texture_grain", 1.0);
}

static void _proxy_set_background(dt_view_t *self, const float *rgba, int style)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view) || IS_NULL_PTR(view->canvas)) return;
  if(!IS_NULL_PTR(rgba))
  {
    view->canvas->background = dt_canvas_color(rgba[0], rgba[1], rgba[2], 1.0f);
    char text[16];
    dt_canvas_color_format(&view->canvas->background, text, sizeof(text));
    dt_conf_set_string("canvas/background_color", text);
  }
  gboolean style_changed = FALSE;
  if(style >= 0)
  {
    const uint32_t chosen = (uint32_t)CLAMP(style, 0, DT_CANVAS_BACKGROUND_LAST - 1);
    style_changed = chosen != view->canvas->background_style;
    // A paper comes in its own colour: choosing one sets it, and the colour patch stays live to recolour it.
    if(style_changed && chosen != DT_CANVAS_BACKGROUND_PLAIN
       && !dt_canvas_background_is_transparent(chosen) && IS_NULL_PTR(rgba))
    {
      view->canvas->background = dt_canvas_background_tint(chosen);
      char text[16];
      dt_canvas_color_format(&view->canvas->background, text, sizeof(text));
      dt_conf_set_string("canvas/background_color", text);
    }
    if(style_changed) _texture_to_defaults(view->canvas);
    view->canvas->background_style = chosen;
    dt_conf_set_int("canvas/background_style", (int)view->canvas->background_style);
  }
  dt_canvas_touch(view->canvas);
  // Choosing a background rewrites the colour and the four weights behind the toolbar's back,
  // so the toolbar has to be told: it owns no state and only ever shows what it last read, so
  // without this it goes on showing the previous paper's colour while this one is painted.
  if(style_changed) DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_CANVAS_CHANGED);
  dt_control_queue_redraw_center();
}

static void _proxy_set_corner_radius(dt_view_t *self, float radius)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view) || IS_NULL_PTR(view->canvas)) return;
  const float wanted = fmaxf(radius, 0.0f);
  if(wanted == view->canvas->corner_radius) return;
  // An edit still open in the properties is its own undo step, and an earlier one.
  _props_commit_pending(self);
  dt_canvas_t *before = _begin_edit(view);
  view->canvas->corner_radius = wanted;
  dt_conf_set_float("canvas/corner_radius", view->canvas->corner_radius);
  dt_canvas_touch(view->canvas);
  _record_undo(self, before);
  // The properties show the inherited radius: they own no state and only show what they last
  // read, so without this they go on showing the previous default under a frame drawn with the new one.
  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_CANVAS_CHANGED);
  dt_control_queue_redraw_center();
}

static void _proxy_set_texture(dt_view_t *self, float contrast, float detail, float scale, float grain)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view) || IS_NULL_PTR(view->canvas)) return;
  view->canvas->texture_contrast = CLAMP(contrast, 0.05f, 8.0f);
  view->canvas->texture_detail = CLAMP(detail, 0.0f, 8.0f);
  view->canvas->texture_scale = CLAMP(scale, 0.1f, 8.0f);
  view->canvas->texture_grain = CLAMP(grain, 0.0f, 8.0f);
  dt_conf_set_float("canvas/texture_contrast", view->canvas->texture_contrast);
  dt_conf_set_float("canvas/texture_detail", view->canvas->texture_detail);
  dt_conf_set_float("canvas/texture_scale", view->canvas->texture_scale);
  dt_conf_set_float("canvas/texture_grain", view->canvas->texture_grain);
  dt_canvas_touch(view->canvas);
  dt_control_queue_redraw_center();
}

static void _proxy_set_grid_color(dt_view_t *self, const float *rgba)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view) || IS_NULL_PTR(view->canvas) || IS_NULL_PTR(rgba)) return;
  view->canvas->grid_color = dt_canvas_color(rgba[0], rgba[1], rgba[2], rgba[3]);
  char text[16];
  dt_canvas_color_format(&view->canvas->grid_color, text, sizeof(text));
  dt_conf_set_string("canvas/grid_color", text);
  dt_canvas_touch(view->canvas);
  dt_control_queue_redraw_center();
}

static void _proxy_set_paper(dt_view_t *self, int paper, int landscape)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view) || IS_NULL_PTR(view->canvas)) return;
  if(paper >= 0)
  {
    view->canvas->paper_size = (uint32_t)CLAMP(paper, 0, DT_CANVAS_PAPER_LAST - 1);
    dt_conf_set_int("canvas/paper_size", (int)view->canvas->paper_size);
  }
  if(landscape >= 0)
  {
    view->canvas->paper_landscape = landscape ? 1u : 0u;
    dt_conf_set_bool("canvas/paper_landscape", landscape != 0);
  }
  dt_canvas_touch(view->canvas);
  dt_control_queue_redraw_center();
}

static void _proxy_set_page_guides(dt_view_t *self, float margin, float bleed)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view) || IS_NULL_PTR(view->canvas)) return;
  if(margin >= 0.0f)
  {
    view->canvas->page_margin = margin;
    dt_conf_set_float("canvas/page_margin", margin);
  }
  if(bleed >= 0.0f)
  {
    view->canvas->page_bleed = bleed;
    dt_conf_set_float("canvas/page_bleed", bleed);
  }
  dt_canvas_touch(view->canvas);
  dt_control_queue_redraw_center();
}

static void _proxy_set_margin_color(dt_view_t *self, const float *rgba)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view) || IS_NULL_PTR(view->canvas) || IS_NULL_PTR(rgba)) return;
  view->canvas->margin_color = dt_canvas_color(rgba[0], rgba[1], rgba[2], rgba[3]);
  char text[16];
  dt_canvas_color_format(&view->canvas->margin_color, text, sizeof(text));
  dt_conf_set_string("canvas/guide_margin_color", text);
  dt_canvas_touch(view->canvas);
  dt_control_queue_redraw_center();
}

static void _proxy_set_bleed_color(dt_view_t *self, const float *rgba)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view) || IS_NULL_PTR(view->canvas) || IS_NULL_PTR(rgba)) return;
  view->canvas->bleed_color = dt_canvas_color(rgba[0], rgba[1], rgba[2], rgba[3]);
  char text[16];
  dt_canvas_color_format(&view->canvas->bleed_color, text, sizeof(text));
  dt_conf_set_string("canvas/guide_bleed_color", text);
  dt_canvas_touch(view->canvas);
  dt_control_queue_redraw_center();
}

static void _proxy_set_guides(dt_view_t *self, int mask, int value)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view) || IS_NULL_PTR(view->canvas)) return;
  view->canvas->grid_flags = (view->canvas->grid_flags & ~(uint32_t)mask) | ((uint32_t)value & (uint32_t)mask);
  dt_conf_set_bool("canvas/grid_visible", (view->canvas->grid_flags & DT_CANVAS_GRID_VISIBLE) != 0);
  dt_conf_set_bool("canvas/page_visible", (view->canvas->grid_flags & DT_CANVAS_PAGE_VISIBLE) != 0);
  dt_conf_set_bool("canvas/padding_visible", (view->canvas->grid_flags & DT_CANVAS_PADDING_VISIBLE) != 0);
  dt_conf_set_bool("canvas/margin_visible", (view->canvas->grid_flags & DT_CANVAS_MARGIN_VISIBLE) != 0);
  dt_conf_set_bool("canvas/bleed_visible", (view->canvas->grid_flags & DT_CANVAS_BLEED_VISIBLE) != 0);
  dt_conf_set_bool("canvas/guides_over", (view->canvas->grid_flags & DT_CANVAS_GUIDES_OVER) != 0);
  dt_conf_set_int("canvas/snap_mode", (int)(view->canvas->grid_flags & DT_CANVAS_SNAP_ALL));
  dt_canvas_touch(view->canvas);
  dt_control_queue_redraw_center();
}

static void _proxy_set_page_color(dt_view_t *self, const float *rgba)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view) || IS_NULL_PTR(view->canvas) || IS_NULL_PTR(rgba)) return;
  view->canvas->page_color = dt_canvas_color(rgba[0], rgba[1], rgba[2], rgba[3]);
  char text[16];
  dt_canvas_color_format(&view->canvas->page_color, text, sizeof(text));
  dt_conf_set_string("canvas/trim_color", text);
  dt_canvas_touch(view->canvas);
  dt_control_queue_redraw_center();
}

static void _proxy_set_padding_color(dt_view_t *self, const float *rgba)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view) || IS_NULL_PTR(view->canvas) || IS_NULL_PTR(rgba)) return;
  view->canvas->padding_color = dt_canvas_color(rgba[0], rgba[1], rgba[2], rgba[3]);
  char text[16];
  dt_canvas_color_format(&view->canvas->padding_color, text, sizeof(text));
  dt_conf_set_string("canvas/guide_padding_color", text);
  dt_canvas_touch(view->canvas);
  dt_control_queue_redraw_center();
}

static void _proxy_set_shadow(dt_view_t *self, const float *rgba, float offset_x, float offset_y, float blur)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view) || IS_NULL_PTR(view->canvas) || IS_NULL_PTR(rgba)) return;
  // An edit still open in the properties is its own undo step, and an earlier one.
  _props_commit_pending(self);
  dt_canvas_t *before = _begin_edit(view);
  view->canvas->shadow.color = dt_canvas_color(rgba[0], rgba[1], rgba[2], rgba[3]);
  view->canvas->shadow.offset_x = offset_x;
  view->canvas->shadow.offset_y = offset_y;
  view->canvas->shadow.blur = blur;
  char text[16];
  dt_canvas_color_format(&view->canvas->shadow.color, text, sizeof(text));
  dt_conf_set_string("canvas/shadow_color", text);
  dt_conf_set_float("canvas/shadow_offset_x", offset_x);
  dt_conf_set_float("canvas/shadow_offset_y", offset_y);
  dt_conf_set_float("canvas/shadow_radius", blur);
  dt_canvas_touch(view->canvas);
  _record_undo(self, before);
  // Every object that inherits its shadow is drawn with the new one, and the properties show it.
  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_CANVAS_CHANGED);
  dt_control_queue_redraw_center();
}

static void _proxy_set_snap_mode(dt_view_t *self, int mode)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view) || IS_NULL_PTR(view->canvas)) return;
  view->canvas->grid_flags = (view->canvas->grid_flags & ~(uint32_t)DT_CANVAS_SNAP_ALL)
                             | ((uint32_t)mode & DT_CANVAS_SNAP_ALL);
  dt_conf_set_int("canvas/snap_mode", (int)(view->canvas->grid_flags & DT_CANVAS_SNAP_ALL));
  dt_canvas_touch(view->canvas);
  dt_control_queue_redraw_center();
}

static void _proxy_set_padding(dt_view_t *self, float padding)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view) || IS_NULL_PTR(view->canvas) || padding < 0.0f) return;
  view->canvas->padding = padding;
  dt_conf_set_float("canvas/padding", padding);
  dt_canvas_touch(view->canvas);
  dt_control_queue_redraw_center();
}

static const dt_canvas_t *_proxy_document(dt_view_t *self)
{
  const dt_canvas_view_t *view = (const dt_canvas_view_t *)self->data;
  return IS_NULL_PTR(view) ? NULL : view->canvas;
}

/** The sheet: how many pages it holds, and what the binding takes out of a fold. */
static void _proxy_set_spread(dt_view_t *self, const int cols, const int rows, const float bind_gutter)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view) || IS_NULL_PTR(view->canvas)) return;
  view->canvas->spread_cols = (uint32_t)CLAMP(cols, 0, 64);
  view->canvas->spread_rows = (uint32_t)CLAMP(rows, 0, 64);
  view->canvas->bind_gutter = CLAMP(bind_gutter, 0.0f, 2000.0f);
  dt_conf_set_int("canvas/spread_cols", (int)view->canvas->spread_cols);
  dt_conf_set_int("canvas/spread_rows", (int)view->canvas->spread_rows);
  dt_conf_set_float("canvas/bind_gutter", view->canvas->bind_gutter);
  dt_canvas_touch(view->canvas);
  dt_control_queue_redraw_center();
}

/** Canvas units per inch: what a paper size is measured against. */
static void _proxy_set_resolution(dt_view_t *self, float resolution)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view) || IS_NULL_PTR(view->canvas)) return;
  view->canvas->resolution = CLAMP(resolution, 18.0f, 2400.0f);
  dt_conf_set_float("canvas/resolution", view->canvas->resolution);
  dt_canvas_touch(view->canvas);
  dt_control_queue_redraw_center();
}

static void _proxy_set_grid_size(dt_view_t *self, float size)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view) || IS_NULL_PTR(view->canvas) || size <= 0.0f) return;
  view->canvas->grid_size = size;
  dt_conf_set_int("canvas/grid_size", (int)lroundf(size));
  dt_canvas_touch(view->canvas);
  dt_control_queue_redraw_center();
}

static void _proxy_set_border(dt_view_t *self, const float *rgba, float width)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view) || IS_NULL_PTR(view->canvas)) return;
  const dt_canvas_color_t color = IS_NULL_PTR(rgba) ? view->canvas->border_color
                                                    : dt_canvas_color(rgba[0], rgba[1], rgba[2], rgba[3]);
  const float wanted = width >= 0.0f ? width : view->canvas->border_width;
  const gboolean same_color = memcmp(&color, &view->canvas->border_color, sizeof(color)) == 0;
  if(same_color && wanted == view->canvas->border_width) return;
  // An edit still open in the properties is its own undo step, and an earlier one.
  _props_commit_pending(self);
  dt_canvas_t *before = _begin_edit(view);
  view->canvas->border_color = color;
  char text[16];
  dt_canvas_color_format(&view->canvas->border_color, text, sizeof(text));
  dt_conf_set_string("canvas/border_color", text);
  view->canvas->border_width = wanted;
  dt_conf_set_float("canvas/border_width", wanted);
  dt_canvas_touch(view->canvas);
  _record_undo(self, before);
  // The properties show the inherited border: tell them, as a paper's colour tells the toolbar.
  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_CANVAS_CHANGED);
  dt_control_queue_redraw_center();
}

static gboolean _proxy_is_connecting(dt_view_t *self)
{
  const dt_canvas_view_t *view = (const dt_canvas_view_t *)self->data;
  return !IS_NULL_PTR(view) && view->connecting;
}

typedef struct dt_canvas_accel_t
{
  const char *name;
  dt_canvas_action_t action;
  guint key;
  GdkModifierType mods;
} dt_canvas_accel_t;

static const dt_canvas_accel_t _accels[] = {
  { N_("New canvas"), DT_CANVAS_ACTION_NEW, GDK_KEY_n, DT_PRIMARY_MASK },
  { N_("Open a canvas"), DT_CANVAS_ACTION_OPEN, GDK_KEY_o, DT_PRIMARY_MASK },
  { N_("Save the canvas"), DT_CANVAS_ACTION_SAVE, GDK_KEY_s, DT_PRIMARY_MASK },
  { N_("Save the canvas as"), DT_CANVAS_ACTION_SAVE_AS, GDK_KEY_s, DT_PRIMARY_MASK | GDK_SHIFT_MASK },
  { N_("Export the canvas..."), DT_CANVAS_ACTION_EXPORT, GDK_KEY_p, DT_PRIMARY_MASK },
  { N_("Add a text frame"), DT_CANVAS_ACTION_ADD_TEXT, GDK_KEY_t, 0 },
  { N_("Add the text notes of the selected images"), DT_CANVAS_ACTION_ADD_NOTES, GDK_KEY_t, GDK_SHIFT_MASK },
  { N_("Add a map"), DT_CANVAS_ACTION_ADD_MAP, GDK_KEY_m, 0 },
  { N_("Place a drawing"), DT_CANVAS_ACTION_ADD_SVG, GDK_KEY_d, 0 },
  { N_("Fit the view to the canvas"), DT_CANVAS_ACTION_ZOOM_FIT, GDK_KEY_0, DT_PRIMARY_MASK },
  { N_("Zoom to 100%"), DT_CANVAS_ACTION_ZOOM_100, GDK_KEY_1, DT_PRIMARY_MASK },
  { N_("Toggle the grid"), DT_CANVAS_ACTION_TOGGLE_GRID, GDK_KEY_g, 0 },
  { N_("Toggle snapping to the grid"), DT_CANVAS_ACTION_TOGGLE_SNAP, GDK_KEY_g, GDK_SHIFT_MASK },
  { N_("Arrange as a grid"), DT_CANVAS_ACTION_LAYOUT_GRID, GDK_KEY_1, 0 },
  { N_("Arrange as a masonry"), DT_CANVAS_ACTION_LAYOUT_MASONRY, GDK_KEY_2, 0 },
  { N_("Arrange as a row"), DT_CANVAS_ACTION_LAYOUT_ROW, GDK_KEY_3, 0 },
  { N_("Arrange as a column"), DT_CANVAS_ACTION_LAYOUT_COLUMN, GDK_KEY_4, 0 },
  { N_("Check the images against the library"), DT_CANVAS_ACTION_SYNC_CHECK, GDK_KEY_r, 0 },
  { N_("Refresh the stale images"), DT_CANVAS_ACTION_SYNC_REFRESH_STALE, GDK_KEY_r, DT_PRIMARY_MASK },
  { N_("Draw a connector"), DT_CANVAS_ACTION_CONNECT_MODE, GDK_KEY_c, 0 },
  { CANVAS_ACCEL_PROPERTIES, DT_CANVAS_ACTION_PROPERTIES, GDK_KEY_i, 0 },
  { N_("Undo"), DT_CANVAS_ACTION_UNDO, GDK_KEY_z, DT_PRIMARY_MASK },
  { N_("Redo"), DT_CANVAS_ACTION_REDO, GDK_KEY_y, DT_PRIMARY_MASK },
};

static gboolean _accel_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods,
                                gpointer user_data)
{
  const dt_canvas_accel_t *accel = (const dt_canvas_accel_t *)user_data;
  dt_view_t *self = dt_view_manager_get_global()->proxy.canvas.view;
  if(IS_NULL_PTR(self)) return FALSE;
  _proxy_action(self, accel->action);
  return TRUE;
}

/* --- lifecycle ---------------------------------------------------------------------- */

void init(dt_view_t *self)
{
  dt_canvas_view_t *view = g_new0(dt_canvas_view_t, 1);
  self->data = view;
  view->selection = g_array_new(FALSE, FALSE, sizeof(uint32_t));
  view->cache = dt_canvas_surface_cache_new(TRUE, (size_t)CLAMP(dt_conf_get_int("canvas/surface_cache_mb"), 64, 8192)
                                                      * 1024u * 1024u);
  view->zoom = 1.0;
  view->token = 1;
  view->flower_hover = DT_CANVAS_FLOWER_NONE;

  // A canvas left dirty at the last exit comes back, whatever the reason the exit happened.
  char recovery[DT_PATH_MAX] = { 0 };
  _recovery_path(recovery, sizeof(recovery));
  if(g_file_test(recovery, G_FILE_TEST_IS_REGULAR))
  {
    dt_canvas_t *recovered = dt_canvas_load(recovery, NULL);
    if(!IS_NULL_PTR(recovered))
    {
      dt_free(recovered->path);
      recovered->path = NULL;
      recovered->dirty = TRUE;
      view->canvas = recovered;
    }
    g_unlink(recovery);
  }
  if(IS_NULL_PTR(view->canvas))
  {
    view->canvas = dt_canvas_new();
    _canvas_apply_conf_defaults(view->canvas);
  }
  _restore_viewport(view);

  dt_view_manager_t *manager = dt_view_manager_get_global();
  manager->proxy.canvas.view = self;
  manager->proxy.canvas.action = _proxy_action;
  manager->proxy.canvas.document = _proxy_document;
  manager->proxy.canvas.set_grid_size = _proxy_set_grid_size;
  manager->proxy.canvas.set_border = _proxy_set_border;
  manager->proxy.canvas.is_connecting = _proxy_is_connecting;
  manager->proxy.canvas.set_padding = _proxy_set_padding;
  manager->proxy.canvas.set_snap_mode = _proxy_set_snap_mode;
  manager->proxy.canvas.set_background = _proxy_set_background;
  manager->proxy.canvas.set_grid_color = _proxy_set_grid_color;
  manager->proxy.canvas.set_paper = _proxy_set_paper;
  manager->proxy.canvas.set_guides = _proxy_set_guides;
  manager->proxy.canvas.set_page_color = _proxy_set_page_color;
  manager->proxy.canvas.set_padding_color = _proxy_set_padding_color;
  manager->proxy.canvas.set_shadow = _proxy_set_shadow;
  manager->proxy.canvas.set_texture = _proxy_set_texture;
  manager->proxy.canvas.set_resolution = _proxy_set_resolution;
  manager->proxy.canvas.set_spread = _proxy_set_spread;
  manager->proxy.canvas.set_corner_radius = _proxy_set_corner_radius;
  manager->proxy.canvas.set_page_guides = _proxy_set_page_guides;
  manager->proxy.canvas.set_margin_color = _proxy_set_margin_color;
  manager->proxy.canvas.set_bleed_color = _proxy_set_bleed_color;
}

void gui_init(dt_view_t *self)
{
  for(size_t idx = 0; idx < G_N_ELEMENTS(_accels); idx++)
  {
    dt_accels_new_canvas_action(_accel_callback, (gpointer)&_accels[idx], NULL, CANVAS_ACCEL_SCOPE, _accels[idx].name,
                                _accels[idx].key, _accels[idx].mods, NULL);
  }
}

void cleanup(dt_view_t *self)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view)) return;
  dt_canvas_render_cancel_all();
  if(!IS_NULL_PTR(view->canvas) && view->canvas->dirty)
  {
    char recovery[DT_PATH_MAX] = { 0 };
    _recovery_path(recovery, sizeof(recovery));
    _store_viewport(view);
    dt_canvas_save(view->canvas, recovery, NULL);
  }
  dt_view_manager_t *manager = dt_view_manager_get_global();
  if(manager->proxy.canvas.view == self) manager->proxy.canvas.view = NULL;
  _props_request_drop(view);
  dt_canvas_free(view->props_snapshot);
  view->props_snapshot = NULL;
  dt_canvas_free(view->drag_snapshot);
  dt_canvas_free(view->canvas);
  dt_canvas_surface_cache_free(view->cache);
  g_array_free(view->selection, TRUE);
  if(!IS_NULL_PTR(view->props_shapes)) g_array_free(view->props_shapes, TRUE);
  dt_free(view);
  self->data = NULL;
}

int try_enter(dt_view_t *self)
{
  return 0;
}

void enter(dt_view_t *self)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;

  dt_ui_panel_show(dt_gui_get_ui(), DT_UI_PANEL_LEFT, FALSE, TRUE);
  dt_ui_panel_show(dt_gui_get_ui(), DT_UI_PANEL_RIGHT, FALSE, TRUE);
  dt_ui_panel_show(dt_gui_get_ui(), DT_UI_PANEL_BOTTOM, TRUE, TRUE);

  dt_accels_connect_accels(dt_gui_get_accels());
  dt_accels_connect_active_group(dt_gui_get_accels(), "canvas");

  GtkWidget *center = dt_gui_center_widget();
  gtk_widget_show(center);
  // The filmstrip drags with GDK_ACTION_MOVE and the full target list: the destination
  // must accept that action or GTK never delivers the drop (the print view does the same).
  gtk_drag_dest_set(center, GTK_DEST_DEFAULT_ALL, target_list_all, n_targets_all, GDK_ACTION_MOVE);
  g_signal_connect(center, "drag-data-received", G_CALLBACK(_drag_data_received), self);
  g_signal_connect(center, "drag-motion", G_CALLBACK(_drag_motion), self);
  view->dnd_connected = TRUE;

  dt_thumbtable_show(dt_gui_get_ui()->thumbtable_filmstrip);
  dt_thumbtable_update_parent(dt_gui_get_ui()->thumbtable_filmstrip);
  _props_create(self);
  // Properties open when the atelier was left -- a double click into the darkroom, typically --
  // are shown again on the way back, provided their object is still the whole selection.
  _props_sync(self);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_VIEWMANAGER_FILMSTRIP_DRAG_BEGIN,
                                  G_CALLBACK(_filmstrip_drag_begin), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_CONTROL_PROFILE_CHANGED,
                                  G_CALLBACK(_profile_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_CANVAS_CHANGED,
                                  G_CALLBACK(_props_canvas_changed), self);

  // The library may have moved on while we were away: compare, and refresh when asked to.
  _sync_check_all(view);
  if(dt_conf_get_bool("canvas/auto_refresh")) _refresh_images(self, DT_CANVAS_SYNC_STALE);
  _announce_document(self);
  dt_gui_refocus_center();
}

void leave(dt_view_t *self)
{
  {
    dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
    if(view->interaction_timeout != 0)
    {
      g_source_remove(view->interaction_timeout);
      view->interaction_timeout = 0;
    }
    view->interacting = FALSE;
  }
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(dt_control_signal_get_global(), G_CALLBACK(_filmstrip_drag_begin), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(dt_control_signal_get_global(), G_CALLBACK(_profile_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(dt_control_signal_get_global(), G_CALLBACK(_props_canvas_changed), self);
  if(view->dnd_connected)
  {
    GtkWidget *center = dt_gui_center_widget();
    g_signal_handlers_disconnect_by_func(center, G_CALLBACK(_drag_data_received), self);
    g_signal_handlers_disconnect_by_func(center, G_CALLBACK(_drag_motion), self);
    gtk_drag_dest_unset(center);
    view->dnd_connected = FALSE;
  }
  if(view->drag != DT_CANVAS_DRAG_NONE) _end_gesture(self);
  // The widget goes with the view, but the open state stays for `enter()` to show again. An
  // action a double click deferred is dropped: it was asked of the atelier being left.
  _props_commit_pending(self);
  _props_request_drop(view);
  view->props_suspended = FALSE;
  _props_destroy(self);
  view->cursor = GDK_LEFT_PTR;
  dt_control_change_cursor(GDK_LEFT_PTR);
  view->connecting = FALSE;
  view->connect_from = 0;
  view->anchor_hover_id = 0;
  view->hover = 0;
  view->mask_node_hover = -1;
  _store_viewport(view);
  dt_accels_disconnect_active_group(dt_gui_get_accels());
  dt_thumbtable_hide(dt_gui_get_ui()->thumbtable_filmstrip);
}

void configure(dt_view_t *self, int width, int height)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(dt_view_manager_get_current_view(dt_view_manager_get_global()) != self) return;
  const gboolean resized = view->width != width || view->height != height;
  view->width = width;
  view->height = height;
  // A new size changes what fits where. Only on a real change: moving the properties re-allocates
  // the overlay, which configures the drawing area again at the same size.
  if(resized) _props_sync(self);
}

void reset(dt_view_t *self)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  _zoom_fit(view);
  dt_control_queue_redraw_center();
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
