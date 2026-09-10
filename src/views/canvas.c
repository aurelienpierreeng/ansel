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
#include "canvas/canvas_pdf.h"
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
#include "views/view.h"
#include "views/view_api.h"
#include "widgets/accelerators.h"
#include "widgets/dialog.h"
#include "widgets/gdkkeys.h"
#include "widgets/widget_settings.h"
#include "widgets/widget_style.h"

#include <gdk/gdkkeysyms.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>

DT_MODULE(1)

#define CANVAS_ZOOM_MIN 0.02
#define CANVAS_ZOOM_MAX 8.0
#define CANVAS_ZOOM_STEP 1.15
#define CANVAS_HANDLE_PIXELS 8.0
#define CANVAS_ROTATE_HANDLE_OFFSET_PIXELS 28.0
#define CANVAS_PICK_TOLERANCE_PIXELS 4.0
#define CANVAS_DRAG_THRESHOLD_PIXELS 3.0
#define CANVAS_DROP_STAGGER 40.0
#define CANVAS_FIT_MARGIN 0.9
#define CANVAS_BADGE_PIXELS 7.0
#define CANVAS_NEIGHBOUR_SNAP_PIXELS 8.0
#define CANVAS_VIA_HANDLE_PIXELS 7.0
#define CANVAS_RECOVERY_FILE "canvas-recovery" DT_CANVAS_FILE_EXTENSION
#define CANVAS_FLOWER_RADIUS 46.0
#define CANVAS_FLOWER_INNER_RADIUS 25.0
#define CANVAS_FLOWER_CENTER_RADIUS 9.0
#define CANVAS_FLOWER_MARGIN 22.0
#define CANVAS_FLOWER_PAN_FRACTION 0.25
#define CANVAS_FLOWER_ZOOM_STEP 1.5

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

  // the floating property bar, an overlay child of the centre, shown under the one selected
  // object: one row per topic, the kind's own row first, the others shared by every kind
  GtkWidget *bar;
  GtkWidget *row_text;
  GtkWidget *text_font;
  GtkWidget *text_color;
  GtkWidget *text_align_h;
  GtkWidget *text_align_v;
  GtkWidget *row_connector;
  GtkWidget *connector_route;
  GtkWidget *connector_arrows;
  GtkWidget *connector_via;
  GtkWidget *row_map;
  GtkWidget *map_latitude;
  GtkWidget *map_longitude;
  GtkWidget *map_zoom;
  GtkWidget *map_source;
  GtkWidget *row_geometry;
  GtkWidget *geometry_x;
  GtkWidget *geometry_y;
  GtkWidget *geometry_width;
  GtkWidget *geometry_height;
  GtkWidget *geometry_rotation;
  GtkWidget *row_opacity;
  GtkWidget *object_opacity;
  GtkWidget *object_background;
  GtkWidget *object_no_background;
  GtkWidget *row_border;
  GtkWidget *border_default;
  GtkWidget *border_custom;
  GtkWidget *object_border_width;
  GtkWidget *object_border_color;
  GtkWidget *corner_default;
  GtkWidget *object_corner_radius;
  GtkWidget *row_line;
  GtkWidget *connector_width;
  GtkWidget *connector_dashed;
  GtkWidget *connector_color;
  GtkWidget *row_shadow;
  GtkWidget *shadow_default;
  GtkWidget *shadow_custom;
  GtkWidget *object_shadow_offset_x;
  GtkWidget *object_shadow_offset_y;
  GtkWidget *object_shadow_blur;
  GtkWidget *object_shadow_color;
  GtkWidget *row_cutout;
  GtkWidget *object_cutout_shape;
  GtkWidget *object_cutout_feather;
  GtkWidget *object_cutout_size_x;
  GtkWidget *object_cutout_size_y;
  GtkWidget *object_cutout_invert;
  GtkWidget *object_cutout_edit;
  dt_canvas_t *menu_snapshot;           ///< the document when a slider menu opened, for one undo record on close
  gulong bars_position_handler;         ///< the overlay's get-child-position hook
  // same-size guides, shown while a resize snaps to a neighbour's size
  gboolean guide_width_valid;
  dt_canvas_rect_t guide_width;
  gboolean guide_height_valid;
  dt_canvas_rect_t guide_height;
  gboolean mask_editing;                ///< the cutout's handles are shown and take the pointer
  int mask_handle;                      ///< the polygon node being dragged
  gboolean bars_refilling;
  uint64_t bars_signature;              ///< selection + document state the bars were last filled for
  guint bars_idle;                      ///< pending placement, scheduled off the draw path
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
static void _bars_refresh(dt_view_t *self, gboolean force);
static void _bars_request(dt_view_t *self);
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
  dt_canvas_restore(view->canvas, action == DT_ACTION_UNDO ? record->before : record->after);
  _selection_prune(view);
  _bars_request(self);
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
  canvas->gutter = dt_conf_get_float("canvas/gutter");
  canvas->background_style = (uint32_t)CLAMP(dt_conf_get_int("canvas/background_style"), 0, DT_CANVAS_BACKGROUND_LAST - 1);
  const char *grid_color = dt_conf_get_string_const("canvas/grid_color");
  dt_canvas_color_parse(grid_color, &canvas->grid_color);
  canvas->paper_size = (uint32_t)CLAMP(dt_conf_get_int("canvas/paper_size"), 0, 5);
  canvas->paper_landscape = dt_conf_get_bool("canvas/paper_landscape") ? 1u : 0u;
  const char *page_color = dt_conf_get_string_const("canvas/page_color");
  dt_canvas_color_parse(page_color, &canvas->page_color);
  if(dt_conf_get_bool("canvas/page_visible")) canvas->grid_flags |= DT_CANVAS_PAGE_VISIBLE;
  else canvas->grid_flags &= ~(uint32_t)DT_CANVAS_PAGE_VISIBLE;
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
  const char *gutter_color = dt_conf_get_string_const("canvas/gutter_color");
  dt_canvas_color_parse(gutter_color, &canvas->gutter_color);
  if(dt_conf_get_bool("canvas/gutter_visible")) canvas->grid_flags |= DT_CANVAS_GUTTER_VISIBLE;
  else canvas->grid_flags &= ~(uint32_t)DT_CANVAS_GUTTER_VISIBLE;
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
  view->drag = DT_CANVAS_DRAG_NONE;
  view->connecting = FALSE;
  view->connect_from = 0;
  view->anchor_hover_id = 0;
  view->hover = 0;
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
  _bars_request(self);
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

static gboolean _start_render(dt_view_t *self, dt_canvas_object_t *object, const int32_t imgid)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(imgid <= 0 || IS_NULL_PTR(object) || object->kind != DT_CANVAS_OBJECT_IMAGE) return FALSE;
  const gboolean queued = dt_canvas_render_start(imgid, object->id, view->token, view->canvas->image_long_edge,
                                                 view->canvas->jpeg_quality, _render_done, self);
  if(queued) object->image.sync_status = DT_CANVAS_SYNC_RENDERING;
  return queued;
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

typedef struct dt_canvas_pdf_dialog_t
{
  GtkWidget *paper;
  GtkWidget *landscape;
  GtkWidget *dpi;
  GtkWidget *margin;
  GtkWidget *profile;
  GtkWidget *intent;
  dt_colorprofile_desc_t *profiles;
  size_t profile_count;
} dt_canvas_pdf_dialog_t;

static GtkWidget *_labelled_row(GtkWidget *grid, const int row, const char *label, GtkWidget *widget)
{
  GtkWidget *text = gtk_label_new(label);
  gtk_widget_set_halign(text, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), text, 0, row, 1, 1);
  gtk_widget_set_hexpand(widget, TRUE);
  gtk_grid_attach(GTK_GRID(grid), widget, 1, row, 1, 1);
  return widget;
}

static void _export_pdf(dt_view_t *self)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  GtkWindow *parent = GTK_WINDOW(dt_ui_main_window(dt_gui_get_ui()));
  GtkWidget *dialog = gtk_dialog_new_with_buttons(_("Export the canvas as PDF"), parent, GTK_DIALOG_MODAL,
                                                  _("_Cancel"), GTK_RESPONSE_CANCEL, _("_Export"),
                                                  GTK_RESPONSE_OK, NULL);
  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(6));
  gtk_grid_set_column_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(12));
  gtk_container_set_border_width(GTK_CONTAINER(grid), DT_PIXEL_APPLY_DPI(12));
  gtk_box_pack_start(GTK_BOX(content), grid, TRUE, TRUE, 0);

  double canvas_paper_width = 0.0;
  double canvas_paper_height = 0.0;
  const gboolean canvas_has_paper = dt_canvas_paper_dimensions(view->canvas, &canvas_paper_width, &canvas_paper_height);
  if(canvas_has_paper)
  {
    gchar *note = g_strdup_printf(_("One PDF page per canvas page of %.0f x %.0f mm; empty pages are skipped."),
                                  dt_pdf_point_to_mm(canvas_paper_width), dt_pdf_point_to_mm(canvas_paper_height));
    GtkWidget *note_label = gtk_label_new(note);
    gtk_label_set_line_wrap(GTK_LABEL(note_label), TRUE);
    gtk_widget_set_halign(note_label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), note_label, 0, 6, 2, 1);
    dt_free(note);
  }
  dt_canvas_pdf_dialog_t widgets;
  memset(&widgets, 0, sizeof(widgets));
  widgets.paper = gtk_combo_box_text_new();
  const char *paper_conf = dt_conf_get_string_const("canvas/pdf/paper");
  for(int idx = 0; idx < dt_pdf_paper_sizes_n; idx++)
  {
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widgets.paper), _(dt_pdf_paper_sizes[idx].name));
    if(!g_strcmp0(paper_conf, dt_pdf_paper_sizes[idx].name)) gtk_combo_box_set_active(GTK_COMBO_BOX(widgets.paper), idx);
  }
  if(gtk_combo_box_get_active(GTK_COMBO_BOX(widgets.paper)) < 0) gtk_combo_box_set_active(GTK_COMBO_BOX(widgets.paper), 0);
  _labelled_row(grid, 0, _("Paper"), widgets.paper);

  widgets.landscape = gtk_check_button_new_with_label(_("landscape"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widgets.landscape), dt_conf_get_bool("canvas/pdf/landscape"));
  _labelled_row(grid, 1, _("Orientation"), widgets.landscape);

  widgets.dpi = gtk_spin_button_new_with_range(72.0, 1200.0, 1.0);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(widgets.dpi), dt_conf_get_int("canvas/pdf/dpi"));
  _labelled_row(grid, 2, _("Resolution (dpi)"), widgets.dpi);

  widgets.margin = gtk_spin_button_new_with_range(0.0, 100.0, 1.0);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(widgets.margin), dt_conf_get_float("canvas/pdf/margin_mm"));
  _labelled_row(grid, 3, _("Margin (mm)"), widgets.margin);

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
  _labelled_row(grid, 4, _("Output profile"), widgets.profile);

  widgets.intent = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widgets.intent), _("perceptual"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widgets.intent), _("relative colorimetric"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widgets.intent), _("saturation"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widgets.intent), _("absolute colorimetric"));
  gtk_combo_box_set_active(GTK_COMBO_BOX(widgets.intent), CLAMP(dt_conf_get_int("canvas/pdf/intent"), 0, 3));
  _labelled_row(grid, 5, _("Rendering intent"), widgets.intent);

  gtk_widget_show_all(dialog);
  if(canvas_has_paper)
  {
    // The canvas's paper is the page; the margin means nothing on a page that IS the canvas page.
    gtk_widget_hide(widgets.paper);
    gtk_widget_hide(widgets.landscape);
    gtk_widget_hide(widgets.margin);
  }
  const gint response = gtk_dialog_run(GTK_DIALOG(dialog));
  dt_canvas_pdf_options_t options = dt_canvas_pdf_options_default();
  gboolean proceed = response == GTK_RESPONSE_OK;
  if(proceed)
  {
    const int paper = CLAMP(gtk_combo_box_get_active(GTK_COMBO_BOX(widgets.paper)), 0, dt_pdf_paper_sizes_n - 1);
    const gboolean landscape = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widgets.landscape));
    const float paper_width = dt_pdf_point_to_mm(dt_pdf_paper_sizes[paper].width);
    const float paper_height = dt_pdf_point_to_mm(dt_pdf_paper_sizes[paper].height);
    options.page_width_mm = landscape ? paper_height : paper_width;
    options.page_height_mm = landscape ? paper_width : paper_height;
    options.dpi = (float)gtk_spin_button_get_value(GTK_SPIN_BUTTON(widgets.dpi));
    options.margin_mm = (float)gtk_spin_button_get_value(GTK_SPIN_BUTTON(widgets.margin));
    options.intent = (dt_iop_color_intent_t)CLAMP(gtk_combo_box_get_active(GTK_COMBO_BOX(widgets.intent)), 0, 3);
    const int profile = gtk_combo_box_get_active(GTK_COMBO_BOX(widgets.profile));
    if(profile >= 0 && (size_t)profile < widgets.profile_count)
    {
      options.icc_type = widgets.profiles[profile].type;
      g_strlcpy(options.icc_filename, widgets.profiles[profile].filename, sizeof(options.icc_filename));
    }
    dt_conf_set_string("canvas/pdf/paper", dt_pdf_paper_sizes[paper].name);
    dt_conf_set_bool("canvas/pdf/landscape", landscape);
    dt_conf_set_int("canvas/pdf/dpi", (int)options.dpi);
    dt_conf_set_float("canvas/pdf/margin_mm", options.margin_mm);
    dt_conf_set_int("canvas/pdf/icc_type", options.icc_type);
    dt_conf_set_string("canvas/pdf/icc_filename", options.icc_filename);
    dt_conf_set_int("canvas/pdf/intent", options.intent);
  }
  dt_free_align(widgets.profiles);
  gtk_widget_destroy(dialog);
  dt_gui_refocus_parent(parent);
  if(!proceed) return;

  GtkFileFilter *filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, _("PDF document"));
  gtk_file_filter_add_pattern(filter, "*.pdf");
  gchar *suggested = NULL;
  if(!IS_NULL_PTR(view->canvas->path))
  {
    gchar *base = g_path_get_basename(view->canvas->path);
    if(g_str_has_suffix(base, DT_CANVAS_FILE_EXTENSION)) base[strlen(base) - strlen(DT_CANVAS_FILE_EXTENSION)] = '\0';
    suggested = g_strconcat(base, ".pdf", NULL);
    dt_free(base);
  }
  else
  {
    suggested = g_strdup("canvas.pdf");
  }
  gchar *path = _choose_file(_("Export the canvas as PDF"), GTK_FILE_CHOOSER_ACTION_SAVE, suggested, filter);
  dt_free(suggested);
  if(IS_NULL_PTR(path)) return;
  GError *error = NULL;
  if(dt_canvas_pdf_export(view->canvas, path, &options, &error))
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
    if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fit_height)))
    {
      cairo_surface_t *scratch = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
      cairo_t *cr = cairo_create(scratch);
      const double natural = dt_canvas_paint_text_natural_height(cr, view->canvas, object);
      cairo_destroy(cr);
      cairo_surface_destroy(scratch);
      if(natural > 0.0) object->height = natural;
    }
    dt_canvas_touch(view->canvas);
    _record_undo(self, before);
  }
  gtk_widget_destroy(dialog);
  dt_gui_refocus_parent(parent);
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
  _bars_request(self);
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
  _bars_request(self);
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

static dt_canvas_object_t *_menu_object(const dt_canvas_menu_context_t *context)
{
  const dt_canvas_view_t *view = (const dt_canvas_view_t *)context->self->data;
  return dt_canvas_find_object(view->canvas, context->object_id);
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

static void _menu_refresh_image(GtkWidget *widget, gpointer data)
{
  dt_canvas_menu_context_t *context = (dt_canvas_menu_context_t *)data;
  dt_canvas_object_t *object = _menu_object(context);
  if(!IS_NULL_PTR(object) && object->kind == DT_CANVAS_OBJECT_MAP)
  {
    _start_map_render(context->self, object);
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
  _start_render(context->self, object, imgid);
  dt_control_queue_redraw_center();
}

static void _menu_map_of_image(GtkWidget *widget, gpointer data)
{
  dt_canvas_menu_context_t *context = (dt_canvas_menu_context_t *)data;
  dt_canvas_view_t *view = (dt_canvas_view_t *)context->self->data;
  dt_canvas_object_t *image = _menu_object(context);
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
  _add_map(context->self, image->x, bounds.y + bounds.height + view->canvas->gutter + image->height * 0.375,
           latitude, longitude);
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
  _bars_request(context->self);
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
  _bars_refresh(context->self, TRUE);
  dt_control_queue_redraw_center();
}

static void _menu_cutout_edit(GtkWidget *widget, gpointer data)
{
  dt_canvas_menu_context_t *context = (dt_canvas_menu_context_t *)data;
  dt_canvas_view_t *view = (dt_canvas_view_t *)context->self->data;
  view->mask_editing = context->value != 0;
  _bars_refresh(context->self, TRUE);
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

static void _menu_cutout_smooth_node(GtkWidget *widget, gpointer data)
{
  dt_canvas_menu_context_t *context = (dt_canvas_menu_context_t *)data;
  dt_canvas_view_t *view = (dt_canvas_view_t *)context->self->data;
  dt_canvas_object_t *object = _menu_object(context);
  if(!dt_canvas_object_is_frame(object) || object->mask.shape != DT_CANVAS_MASK_POLYGON) return;
  if(context->value < 0 || (uint32_t)context->value >= object->mask.node_count) return;
  dt_canvas_t *before = _begin_edit(view);
  float *node = object->mask.nodes + (size_t)context->value * DT_CANVAS_MASK_NODE_FLOATS;
  node[6] = node[6] != 0.0f ? 0.0f : 1.0f;
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

  if(IS_NULL_PTR(object))
  {
    _menu_item(menu, _("Add a text frame here"), _menu_add_text_here, _menu_context(self, 0, x, y, 0));
    _menu_item(menu, _("Fit the view to the canvas"), _menu_zoom_fit, _menu_context(self, 0, x, y, 0));
  }
  else if(object->kind == DT_CANVAS_OBJECT_CONNECTOR)
  {
    // Its properties live in its floating bar; here is what it shares with the frames.
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
      _menu_item(menu, _("Edit the text..."), _menu_edit_text, _menu_context(self, id, x, y, 0));
      _menu_item(menu, _("Fit the frame to the text"), _menu_fit_text, _menu_context(self, id, x, y, 0));
      if(object->text.source == DT_CANVAS_TEXT_SOURCE_SIDECAR)
        _menu_item(menu, _("Reload the image's text note"), _menu_reload_sidecar, _menu_context(self, id, x, y, 0));
    }
    else if(object->kind == DT_CANVAS_OBJECT_MAP)
    {
      _menu_item(menu, _("Fetch the map again"), _menu_refresh_image, _menu_context(self, id, x, y, 0));
    }
    else
    {
      _menu_item(menu, _("Open in the darkroom"), _menu_open_darkroom, _menu_context(self, id, x, y, 0));
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
        const int segment = hovered == DT_CANVAS_DRAG_NONE ? _mask_segment_at(canvas_view, object, x, y) : -1;
        if(hovered == DT_CANVAS_DRAG_MASK_NODE)
        {
          const float *node = object->mask.nodes + (size_t)hovered_index * DT_CANVAS_MASK_NODE_FLOATS;
          _menu_item(menu, node[6] != 0.0f ? _("Make this node a cusp") : _("Make this node smooth"),
                     _menu_cutout_smooth_node, _menu_context(self, id, x, y, hovered_index));
          _menu_item(menu, _("Remove this node"), _menu_cutout_remove_node, _menu_context(self, id, x, y, hovered_index));
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
      int node_index = -1;
      const dt_canvas_drag_t on_handle = _mask_handle_at(canvas_view, object, x, y, &node_index);
      const int segment = on_handle == DT_CANVAS_DRAG_NONE ? _mask_segment_at(canvas_view, object, x, y) : -1;
      if(on_handle == DT_CANVAS_DRAG_MASK_NODE)
      {
        const float *node = object->mask.nodes + (size_t)node_index * DT_CANVAS_MASK_NODE_FLOATS;
        gtk_menu_shell_append(GTK_MENU_SHELL(cutout_menu), gtk_separator_menu_item_new());
        _menu_item(cutout_menu, node[6] != 0.0f ? _("Make this node a sharp corner") : _("Make this node smooth"),
                   _menu_cutout_smooth_node, _menu_context(self, id, x, y, node_index));
        _menu_item(cutout_menu, _("Remove this node"), _menu_cutout_remove_node, _menu_context(self, id, x, y, node_index));
      }
      else if(segment >= 0 && canvas_view->mask_editing)
      {
        gtk_menu_shell_append(GTK_MENU_SHELL(cutout_menu), gtk_separator_menu_item_new());
        _menu_item(cutout_menu, _("Add a node here"), _menu_cutout_add_node, _menu_context(self, id, x, y, segment));
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
  gtk_widget_show_all(menu);
  gtk_menu_popup_at_pointer(GTK_MENU(menu), NULL);
}

/* --- connector drawing mode ---------------------------------------------------------- */

#define CANVAS_ANCHOR_REACH_PIXELS 12.0
#define CANVAS_ANCHOR_DOT_PIXELS 6.0

static const dt_canvas_anchor_t _cardinal_anchors[4]
    = { DT_CANVAS_ANCHOR_NORTH, DT_CANVAS_ANCHOR_EAST, DT_CANVAS_ANCHOR_SOUTH, DT_CANVAS_ANCHOR_WEST };

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
    for(int candidate = 0; candidate < 4; candidate++)
    {
      double anchor_x = 0.0;
      double anchor_y = 0.0;
      double normal_x = 0.0;
      double normal_y = 0.0;
      dt_canvas_object_anchor_point(object, _cardinal_anchors[candidate], 0.0, 0.0, &anchor_x, &anchor_y, &normal_x,
                                    &normal_y);
      const double distance = hypot(anchor_x - x, anchor_y - y);
      if(distance <= best)
      {
        best = distance;
        *frame_id = object->id;
        *anchor = _cardinal_anchors[candidate];
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
  if(on) dt_control_log(_("click an anchor point on the first frame, then one on the second; Escape leaves"));
  _bars_request(self);
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
  for(int candidate = 0; candidate < 4; candidate++)
  {
    double anchor_x = 0.0;
    double anchor_y = 0.0;
    double normal_x = 0.0;
    double normal_y = 0.0;
    dt_canvas_object_anchor_point(frame, _cardinal_anchors[candidate], 0.0, 0.0, &anchor_x, &anchor_y, &normal_x,
                                  &normal_y);
    const gboolean hovered = frame->id == view->anchor_hover_id && view->anchor_hover == _cardinal_anchors[candidate];
    const gboolean picked = chosen == _cardinal_anchors[candidate];
    cairo_arc(cr, anchor_x, anchor_y, hovered ? radius * 1.5 : radius, 0.0, 2.0 * M_PI);
    if(picked)
      cairo_set_source_rgba(cr, 1.0, 0.75, 0.2, 1.0);
    else
      cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, hovered ? 1.0 : 0.85);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.8);
    cairo_set_line_width(cr, 1.0 / view->zoom);
    cairo_stroke(cr);
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
    dt_canvas_object_anchor_point(from, (dt_canvas_anchor_t)view->connect_from_anchor, 0.0, 0.0, &anchor_x,
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

/* --- the floating property bars -------------------------------------------------------- */

/** The one selected object, when exactly one is selected. */
static dt_canvas_object_t *_bar_target(const dt_canvas_view_t *view)
{
  return _single_selected(view);
}

static dt_canvas_color_t _color_from_button(GtkWidget *button)
{
  GdkRGBA rgba;
  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(button), &rgba);
  return dt_canvas_color((float)rgba.red, (float)rgba.green, (float)rgba.blue, (float)rgba.alpha);
}

static void _color_to_button(GtkWidget *button, const dt_canvas_color_t *color)
{
  GdkRGBA rgba;
  rgba.red = color->red;
  rgba.green = color->green;
  rgba.blue = color->blue;
  rgba.alpha = color->alpha;
  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(button), &rgba);
}

/** Every bar handler: an edit of the one selected object, recorded for undo. */
#define BAR_EDIT_BEGIN(kind_wanted)                                                                        \
  dt_view_t *self = (dt_view_t *)data;                                                                     \
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;                                                 \
  if(view->bars_refilling) return;                                                                         \
  dt_canvas_object_t *object = _bar_target(view);                                                          \
  if(IS_NULL_PTR(object) || object->kind != (kind_wanted)) return;                                        \
  dt_canvas_t *before = _begin_edit(view);

#define BAR_EDIT_END()                                                                                     \
  dt_canvas_touch(view->canvas);                                                                           \
  _record_undo(self, before);                                                                              \
  _bars_request(self);                                                                                     \
  dt_control_queue_redraw_center();

static void _bar_text_font_set(GtkFontButton *button, gpointer data)
{
  BAR_EDIT_BEGIN(DT_CANVAS_OBJECT_TEXT)
  gchar *font = gtk_font_chooser_get_font(GTK_FONT_CHOOSER(button));
  // A font equal to the canvas default is stored as "no font of its own".
  if(IS_NULL_PTR(font) || g_strcmp0(font, view->canvas->default_font) == 0)
    object->text.font[0] = '\0';
  else
    g_strlcpy(object->text.font, font, sizeof(object->text.font));
  dt_free(font);
  BAR_EDIT_END()
}

static void _bar_text_color_set(GtkColorButton *button, gpointer data)
{
  BAR_EDIT_BEGIN(DT_CANVAS_OBJECT_TEXT)
  object->text.text_color = _color_from_button(GTK_WIDGET(button));
  BAR_EDIT_END()
}

/** The colour under an object's content: a text frame keeps its own field, the others share one. */
static void _object_set_background(dt_canvas_object_t *object, const dt_canvas_color_t color)
{
  if(object->kind == DT_CANVAS_OBJECT_TEXT)
    object->text.background = color;
  else
    object->background = color;
}

static void _bar_text_align_changed(GtkComboBox *combo, gpointer data)
{
  BAR_EDIT_BEGIN(DT_CANVAS_OBJECT_TEXT)
  const int choice = gtk_combo_box_get_active(combo);
  if(GTK_WIDGET(combo) == view->text_align_h)
    object->text.align_h = (uint32_t)CLAMP(choice, 0, 3);
  else
    object->text.align_v = (uint32_t)CLAMP(choice, 0, 2);
  BAR_EDIT_END()
}

/** The border handlers serve the image bar and the text bar alike: any frame. */
#define BAR_EDIT_BEGIN_FRAME()                                                                             \
  dt_view_t *self = (dt_view_t *)data;                                                                     \
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;                                                 \
  if(view->bars_refilling) return;                                                                         \
  dt_canvas_object_t *object = _bar_target(view);                                                          \
  if(!dt_canvas_object_is_frame(object)) return;                                                           \
  dt_canvas_t *before = _begin_edit(view);

static void _bar_border_width_changed(GtkSpinButton *spin, gpointer data)
{
  BAR_EDIT_BEGIN_FRAME()
  dt_canvas_color_t color;
  float width = 0.0f;
  dt_canvas_object_effective_border(view->canvas, object, &color, &width);
  object->border_color = color;
  object->border_width = (float)gtk_spin_button_get_value(spin);
  object->flags |= DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE;
  BAR_EDIT_END()
}

static void _bar_border_color_set(GtkColorButton *button, gpointer data)
{
  BAR_EDIT_BEGIN_FRAME()
  dt_canvas_color_t color;
  float width = 0.0f;
  dt_canvas_object_effective_border(view->canvas, object, &color, &width);
  object->border_width = width;
  object->border_color = _color_from_button(GTK_WIDGET(button));
  object->flags |= DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE;
  BAR_EDIT_END()
}

/** The object bar's other handlers serve every kind: frames and connectors alike. */
#define BAR_EDIT_BEGIN_ANY()                                                                               \
  dt_view_t *self = (dt_view_t *)data;                                                                     \
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;                                                 \
  if(view->bars_refilling) return;                                                                         \
  dt_canvas_object_t *object = _bar_target(view);                                                          \
  if(IS_NULL_PTR(object)) return;                                                                          \
  dt_canvas_t *before = _begin_edit(view);

/** The geometry row: the frame's centre, size and rotation, typed in. */
static void _bar_geometry_changed(GtkSpinButton *spin, gpointer data)
{
  BAR_EDIT_BEGIN_FRAME()
  object->x = gtk_spin_button_get_value(GTK_SPIN_BUTTON(view->geometry_x));
  object->y = gtk_spin_button_get_value(GTK_SPIN_BUTTON(view->geometry_y));
  object->width = fmax(gtk_spin_button_get_value(GTK_SPIN_BUTTON(view->geometry_width)), 1.0);
  object->height = fmax(gtk_spin_button_get_value(GTK_SPIN_BUTTON(view->geometry_height)), 1.0);
  object->rotation = gtk_spin_button_get_value(GTK_SPIN_BUTTON(view->geometry_rotation)) * M_PI / 180.0;
  BAR_EDIT_END()
  if(object->kind == DT_CANVAS_OBJECT_MAP) _start_map_render(self, object);
}

/** Read the shadow widgets into the object's own shadow, and make it the one that applies. */
static void _bar_shadow_apply(dt_canvas_view_t *view, dt_canvas_object_t *object)
{
  dt_canvas_shadow_t shadow;
  dt_canvas_object_effective_shadow(view->canvas, object, &shadow);
  shadow.color = _color_from_button(view->object_shadow_color);
  shadow.offset_x = (float)gtk_spin_button_get_value(GTK_SPIN_BUTTON(view->object_shadow_offset_x));
  shadow.offset_y = (float)gtk_spin_button_get_value(GTK_SPIN_BUTTON(view->object_shadow_offset_y));
  shadow.blur = (float)gtk_spin_button_get_value(GTK_SPIN_BUTTON(view->object_shadow_blur));
  object->shadow = shadow;
  object->flags |= DT_CANVAS_OBJECT_FLAG_SHADOW_OVERRIDE;
}

static void _bar_shadow_changed(GtkWidget *widget, gpointer data)
{
  BAR_EDIT_BEGIN_ANY()
  _bar_shadow_apply(view, object);
  BAR_EDIT_END()
}

/** "Canvas default" on: the object's own shadow is dropped; off: it starts as the canvas's. */
static void _bar_shadow_default_toggled(GtkToggleButton *toggle, gpointer data)
{
  BAR_EDIT_BEGIN_ANY()
  if(gtk_toggle_button_get_active(toggle))
    object->flags &= ~DT_CANVAS_OBJECT_FLAG_SHADOW_OVERRIDE;
  else
  {
    dt_canvas_object_effective_shadow(view->canvas, object, &object->shadow);
    object->flags |= DT_CANVAS_OBJECT_FLAG_SHADOW_OVERRIDE;
  }
  BAR_EDIT_END()
  _bars_refresh(self, TRUE);
}

static void _bar_border_default_toggled(GtkToggleButton *toggle, gpointer data)
{
  BAR_EDIT_BEGIN_FRAME()
  if(gtk_toggle_button_get_active(toggle))
    object->flags &= ~DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE;
  else
  {
    dt_canvas_object_effective_border(view->canvas, object, &object->border_color, &object->border_width);
    object->flags |= DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE;
  }
  BAR_EDIT_END()
  _bars_refresh(self, TRUE);
}

static void _bar_corner_default_toggled(GtkToggleButton *toggle, gpointer data)
{
  BAR_EDIT_BEGIN_FRAME()
  if(gtk_toggle_button_get_active(toggle))
    object->flags &= ~DT_CANVAS_OBJECT_FLAG_CORNER_OVERRIDE;
  else
  {
    object->corner_radius = (float)dt_canvas_object_effective_corner_radius(view->canvas, object);
    object->flags |= DT_CANVAS_OBJECT_FLAG_CORNER_OVERRIDE;
  }
  BAR_EDIT_END()
  _bars_refresh(self, TRUE);
}

static void _bar_corner_changed(GtkSpinButton *spin, gpointer data)
{
  BAR_EDIT_BEGIN_FRAME()
  object->corner_radius = (float)fmax(gtk_spin_button_get_value(spin), 0.0);
  object->flags |= DT_CANVAS_OBJECT_FLAG_CORNER_OVERRIDE;
  BAR_EDIT_END()
}

static void _bar_opacity_changed(GtkSpinButton *spin, gpointer data)
{
  BAR_EDIT_BEGIN_ANY()
  object->transparency = 1.0f - (float)CLAMP(gtk_spin_button_get_value(spin) / 100.0, 0.0, 1.0);
  BAR_EDIT_END()
}

static void _bar_background_set(GtkColorButton *button, gpointer data)
{
  BAR_EDIT_BEGIN_FRAME()
  _object_set_background(object, _color_from_button(GTK_WIDGET(button)));
  BAR_EDIT_END()
}

static void _bar_no_background_toggled(GtkToggleButton *toggle, gpointer data)
{
  BAR_EDIT_BEGIN_FRAME()
  dt_canvas_color_t color = dt_canvas_object_background(object);
  color.alpha = gtk_toggle_button_get_active(toggle) ? 0.0f : 1.0f;
  _object_set_background(object, color);
  BAR_EDIT_END()
}

static void _bar_cutout_shape_changed(GtkComboBox *combo, gpointer data)
{
  BAR_EDIT_BEGIN_FRAME()
  const int shape = gtk_combo_box_get_active(combo);
  dt_canvas_mask_set_shape(view->canvas, object, (uint32_t)CLAMP(shape, 0, DT_CANVAS_MASK_GRADIENT));
  if(object->mask.shape == DT_CANVAS_MASK_NONE) view->mask_editing = FALSE;
  else view->mask_editing = TRUE;
  BAR_EDIT_END()
  _bars_refresh(self, TRUE);
}

static void _bar_cutout_feather_changed(GtkSpinButton *spin, gpointer data)
{
  BAR_EDIT_BEGIN_FRAME()
  object->mask.feather = (float)CLAMP(gtk_spin_button_get_value(spin) / 100.0, 0.0, 1.0);
  BAR_EDIT_END()
}

/** The shape's size: the circle's radius, the ellipse's two radii, the gradient's extent; percent of the shorter side. */
static void _bar_cutout_size_changed(GtkSpinButton *spin, gpointer data)
{
  BAR_EDIT_BEGIN_FRAME()
  const double size_x = gtk_spin_button_get_value(GTK_SPIN_BUTTON(view->object_cutout_size_x)) / 100.0;
  const double size_y = gtk_spin_button_get_value(GTK_SPIN_BUTTON(view->object_cutout_size_y)) / 100.0;
  if(object->mask.shape == DT_CANVAS_MASK_GRADIENT)
    object->mask.radius_x = (float)CLAMP(size_x, 0.0005, 1.0);
  else
  {
    object->mask.radius_x = (float)CLAMP(size_x, 0.005, 2.0);
    if(object->mask.shape == DT_CANVAS_MASK_ELLIPSE) object->mask.radius_y = (float)CLAMP(size_y, 0.005, 2.0);
  }
  BAR_EDIT_END()
}

static void _bar_cutout_invert_toggled(GtkToggleButton *button, gpointer data)
{
  BAR_EDIT_BEGIN_FRAME()
  if(gtk_toggle_button_get_active(button))
    object->mask.flags |= DT_CANVAS_MASK_INVERT;
  else
    object->mask.flags &= ~(uint32_t)DT_CANVAS_MASK_INVERT;
  BAR_EDIT_END()
}

static void _bar_cutout_edit_toggled(GtkToggleButton *button, gpointer data)
{
  dt_view_t *self = (dt_view_t *)data;
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(view->bars_refilling) return;
  view->mask_editing = gtk_toggle_button_get_active(button);
  dt_control_queue_redraw_center();
}

static void _bar_connector_route_changed(GtkComboBox *combo, gpointer data)
{
  BAR_EDIT_BEGIN(DT_CANVAS_OBJECT_CONNECTOR)
  object->connector.routing = (uint32_t)CLAMP(gtk_combo_box_get_active(combo), 0, 2);
  BAR_EDIT_END()
}

static void _bar_connector_arrows_changed(GtkComboBox *combo, gpointer data)
{
  BAR_EDIT_BEGIN(DT_CANVAS_OBJECT_CONNECTOR)
  static const uint32_t arrow_bits[4] = { 0, DT_CANVAS_CONNECTOR_ARROW_END, DT_CANVAS_CONNECTOR_ARROW_START,
                                          DT_CANVAS_CONNECTOR_ARROW_END | DT_CANVAS_CONNECTOR_ARROW_START };
  object->connector.style &= ~(uint32_t)(DT_CANVAS_CONNECTOR_ARROW_END | DT_CANVAS_CONNECTOR_ARROW_START);
  object->connector.style |= arrow_bits[CLAMP(gtk_combo_box_get_active(combo), 0, 3)];
  BAR_EDIT_END()
}

static void _bar_connector_reverse_clicked(GtkWidget *button, gpointer data)
{
  BAR_EDIT_BEGIN(DT_CANVAS_OBJECT_CONNECTOR)
  const uint32_t from_id = object->connector.from_id;
  const uint32_t from_anchor = object->connector.from_anchor;
  object->connector.from_id = object->connector.to_id;
  object->connector.from_anchor = object->connector.to_anchor;
  object->connector.to_id = from_id;
  object->connector.to_anchor = from_anchor;
  BAR_EDIT_END()
}

static void _bar_connector_width_changed(GtkSpinButton *spin, gpointer data)
{
  BAR_EDIT_BEGIN(DT_CANVAS_OBJECT_CONNECTOR)
  object->connector.line_width = (float)gtk_spin_button_get_value(spin);
  BAR_EDIT_END()
}

static void _bar_connector_dashed_toggled(GtkToggleButton *toggle, gpointer data)
{
  BAR_EDIT_BEGIN(DT_CANVAS_OBJECT_CONNECTOR)
  if(gtk_toggle_button_get_active(toggle))
    object->connector.style |= DT_CANVAS_CONNECTOR_DASHED;
  else
    object->connector.style &= ~(uint32_t)DT_CANVAS_CONNECTOR_DASHED;
  BAR_EDIT_END()
}

static void _bar_connector_color_set(GtkColorButton *button, gpointer data)
{
  BAR_EDIT_BEGIN(DT_CANVAS_OBJECT_CONNECTOR)
  object->connector.color = _color_from_button(GTK_WIDGET(button));
  BAR_EDIT_END()
}

static void _bar_connector_via_toggled(GtkToggleButton *toggle, gpointer data)
{
  BAR_EDIT_BEGIN(DT_CANVAS_OBJECT_CONNECTOR)
  if(gtk_toggle_button_get_active(toggle))
    dt_canvas_connector_add_via(view->canvas, object);
  else
    dt_canvas_connector_remove_via(view->canvas, object);
  BAR_EDIT_END()
}

static void _bar_map_changed(GtkWidget *widget, gpointer data)
{
  BAR_EDIT_BEGIN(DT_CANVAS_OBJECT_MAP)
  object->map.latitude = gtk_spin_button_get_value(GTK_SPIN_BUTTON(view->map_latitude));
  object->map.longitude = gtk_spin_button_get_value(GTK_SPIN_BUTTON(view->map_longitude));
  object->map.zoom = (int32_t)gtk_spin_button_get_value(GTK_SPIN_BUTTON(view->map_zoom));
  object->map.source = dt_canvas_map_source_id(gtk_combo_box_get_active(GTK_COMBO_BOX(view->map_source)));
  dt_conf_set_int("canvas/map_zoom", object->map.zoom);
  dt_conf_set_int("canvas/map_source", (int)object->map.source);
  dt_conf_set_float("canvas/map_latitude", (float)object->map.latitude);
  dt_conf_set_float("canvas/map_longitude", (float)object->map.longitude);
  BAR_EDIT_END()
  _start_map_render(self, object);
}

static void _bar_map_refresh_clicked(GtkWidget *button, gpointer data)
{
  dt_view_t *self = (dt_view_t *)data;
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  dt_canvas_object_t *object = _bar_target(view);
  if(IS_NULL_PTR(object) || object->kind != DT_CANVAS_OBJECT_MAP) return;
  _start_map_render(self, object);
  dt_control_queue_redraw_center();
}

static GtkWidget *_bar_color_button(GtkWidget *row, const char *tooltip, GCallback callback, gpointer data)
{
  GtkWidget *button = gtk_color_button_new();
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(button), TRUE);
  gtk_widget_set_tooltip_text(button, tooltip);
  g_signal_connect(button, "color-set", callback, data);
  gtk_box_pack_start(GTK_BOX(row), button, FALSE, FALSE, 0);
  return button;
}

static GtkWidget *_bar_spin(GtkWidget *row, const double low, const double high, const double step,
                            const int digits, const char *tooltip, GCallback callback, gpointer data)
{
  GtkWidget *spin = gtk_spin_button_new_with_range(low, high, step);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin), digits);
  gtk_entry_set_width_chars(GTK_ENTRY(spin), 5);
  gtk_widget_set_tooltip_text(spin, tooltip);
  g_signal_connect(spin, "value-changed", callback, data);
  gtk_box_pack_start(GTK_BOX(row), spin, FALSE, FALSE, 0);
  return spin;
}

static GtkWidget *_bar_toggle(GtkWidget *row, const char *label, const char *tooltip, GCallback callback, gpointer data)
{
  GtkWidget *toggle = gtk_toggle_button_new_with_label(label);
  gtk_widget_set_tooltip_text(toggle, tooltip);
  g_signal_connect(toggle, "toggled", callback, data);
  gtk_box_pack_start(GTK_BOX(row), toggle, FALSE, FALSE, 0);
  return toggle;
}

static GtkWidget *_bar_button(GtkWidget *row, const char *label, const char *tooltip, GCallback callback, gpointer data)
{
  GtkWidget *button = gtk_button_new_with_label(label);
  gtk_widget_set_tooltip_text(button, tooltip);
  g_signal_connect(button, "clicked", callback, data);
  gtk_box_pack_start(GTK_BOX(row), button, FALSE, FALSE, 0);
  return button;
}

/** A row of the bar, headed by its topic; every kind's bar reads the same way, row by row. */
static GtkWidget *_bar_row(GtkWidget *bar, const char *topic)
{
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(4));
  GtkWidget *label = gtk_label_new(NULL);
  gchar *markup = g_markup_printf_escaped("<b>%s</b>", topic);
  gtk_label_set_markup(GTK_LABEL(label), markup);
  dt_free(markup);
  gtk_widget_set_size_request(label, DT_PIXEL_APPLY_DPI(70), -1);
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(bar), row, FALSE, FALSE, 0);
  return row;
}

/** A group inside a row: a label and its controls, set off from the group before. */
static GtkWidget *_bar_group(GtkWidget *row, const char *label)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(4));
  if(!IS_NULL_PTR(label)) gtk_box_pack_start(GTK_BOX(box), gtk_label_new(label), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(row), box, FALSE, FALSE, DT_PIXEL_APPLY_DPI(6));
  return box;
}

/**
 * Where an overlay child goes: the position a placement stored on it. Answering the
 * overlay's own question is what keeps a move to a re-allocation of the overlay, where
 * a margin change is a resize that climbs to the toplevel and lays the window out again.
 */
static gboolean _bars_child_position(GtkOverlay *overlay, GtkWidget *widget, GdkRectangle *allocation,
                                     gpointer user_data)
{
  if(!GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "canvas-bar"))) return FALSE;
  GtkRequisition natural;
  gtk_widget_get_preferred_size(widget, NULL, &natural);
  allocation->x = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "bar-left"));
  allocation->y = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "bar-top"));
  allocation->width = natural.width;
  allocation->height = natural.height;
  return TRUE;
}

/**
 * The bar: one vertical box of rows, the selected object's kind first, then what every
 * object has -- geometry, border and shadow, the cutout -- so the bars of two kinds differ
 * only by their first row. Rows and controls a kind has no use for are hidden at refill.
 */
static void _bars_create(dt_view_t *self)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(!IS_NULL_PTR(view->bar)) return;
  GtkWidget *base = dt_ui_center_base(dt_gui_get_ui());
  view->bars_position_handler = g_signal_connect(base, "get-child-position", G_CALLBACK(_bars_child_position), self);

  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_PIXEL_APPLY_DPI(3));
  dt_gui_add_class(bar, "dt-canvas-floating");
  gtk_widget_set_halign(bar, GTK_ALIGN_START);
  gtk_widget_set_valign(bar, GTK_ALIGN_START);
  gtk_container_set_border_width(GTK_CONTAINER(bar), DT_PIXEL_APPLY_DPI(4));
  gtk_overlay_add_overlay(GTK_OVERLAY(base), bar);
  view->bar = bar;

  // 1. The kind's own row.
  view->row_text = _bar_row(bar, _("Text"));
  view->text_font = gtk_font_button_new();
  gtk_font_button_set_show_size(GTK_FONT_BUTTON(view->text_font), TRUE);
  gtk_widget_set_tooltip_text(view->text_font, _("Font family and size"));
  g_signal_connect(view->text_font, "font-set", G_CALLBACK(_bar_text_font_set), self);
  gtk_box_pack_start(GTK_BOX(view->row_text), view->text_font, FALSE, FALSE, 0);
  view->text_color = _bar_color_button(view->row_text, _("Text colour"), G_CALLBACK(_bar_text_color_set), self);
  view->text_align_h = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(view->text_align_h), _("Left"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(view->text_align_h), _("Centred"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(view->text_align_h), _("Right"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(view->text_align_h), _("Justified"));
  gtk_widget_set_tooltip_text(view->text_align_h, _("Horizontal alignment"));
  g_signal_connect(view->text_align_h, "changed", G_CALLBACK(_bar_text_align_changed), self);
  gtk_box_pack_start(GTK_BOX(view->row_text), view->text_align_h, FALSE, FALSE, 0);
  view->text_align_v = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(view->text_align_v), _("Top"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(view->text_align_v), _("Middle"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(view->text_align_v), _("Bottom"));
  gtk_widget_set_tooltip_text(view->text_align_v, _("Vertical alignment"));
  g_signal_connect(view->text_align_v, "changed", G_CALLBACK(_bar_text_align_changed), self);
  gtk_box_pack_start(GTK_BOX(view->row_text), view->text_align_v, FALSE, FALSE, 0);

  view->row_connector = _bar_row(bar, _("Connector"));
  view->connector_route = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(view->connector_route), _("Straight"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(view->connector_route), _("Square"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(view->connector_route), _("Cubic spline"));
  gtk_widget_set_tooltip_text(view->connector_route, _("Route"));
  g_signal_connect(view->connector_route, "changed", G_CALLBACK(_bar_connector_route_changed), self);
  gtk_box_pack_start(GTK_BOX(view->row_connector), view->connector_route, FALSE, FALSE, 0);
  view->connector_arrows = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(view->connector_arrows), _("Flat line"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(view->connector_arrows), _("Arrow at the end"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(view->connector_arrows), _("Arrow at the start"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(view->connector_arrows), _("Arrows at both ends"));
  gtk_widget_set_tooltip_text(view->connector_arrows, _("Arrow heads"));
  g_signal_connect(view->connector_arrows, "changed", G_CALLBACK(_bar_connector_arrows_changed), self);
  gtk_box_pack_start(GTK_BOX(view->row_connector), view->connector_arrows, FALSE, FALSE, 0);
  _bar_button(view->row_connector, _("Reverse"), _("Swap the start and the end"), G_CALLBACK(_bar_connector_reverse_clicked), self);
  view->connector_via = _bar_toggle(view->row_connector, _("Waypoint"),
                                    _("Add a point the connector passes by, to go around other frames. Drag it into place."),
                                    G_CALLBACK(_bar_connector_via_toggled), self);

  view->row_map = _bar_row(bar, _("Map"));
  view->map_latitude = _bar_spin(view->row_map, -85.0, 85.0, 0.0001, 5, _("Latitude, degrees"), G_CALLBACK(_bar_map_changed), self);
  gtk_entry_set_width_chars(GTK_ENTRY(view->map_latitude), 9);
  view->map_longitude = _bar_spin(view->row_map, -180.0, 180.0, 0.0001, 5, _("Longitude, degrees"), G_CALLBACK(_bar_map_changed), self);
  gtk_entry_set_width_chars(GTK_ENTRY(view->map_longitude), 9);
  view->map_zoom = _bar_spin(view->row_map, 1.0, 19.0, 1.0, 0, _("Zoom level, 1 (the world) to 19 (a street)"),
                             G_CALLBACK(_bar_map_changed), self);
  view->map_source = gtk_combo_box_text_new();
  for(int idx = 0; idx < dt_canvas_map_source_count(); idx++)
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(view->map_source), dt_canvas_map_source_name(idx));
  gtk_widget_set_tooltip_text(view->map_source, _("Map style and provider"));
  g_signal_connect(view->map_source, "changed", G_CALLBACK(_bar_map_changed), self);
  gtk_box_pack_start(GTK_BOX(view->row_map), view->map_source, FALSE, FALSE, 0);
  _bar_button(view->row_map, _("Fetch"), _("Fetch the tiles again"), G_CALLBACK(_bar_map_refresh_clicked), self);

  // 2. Geometry: the frame's centre, size and rotation.
  view->row_geometry = _bar_row(bar, _("Geometry"));
  GtkWidget *position = _bar_group(view->row_geometry, _("Centre"));
  view->geometry_x = _bar_spin(position, -1e6, 1e6, 1.0, 0, _("Horizontal position of the centre, canvas units"),
                               G_CALLBACK(_bar_geometry_changed), self);
  view->geometry_y = _bar_spin(position, -1e6, 1e6, 1.0, 0, _("Vertical position of the centre, canvas units"),
                               G_CALLBACK(_bar_geometry_changed), self);
  GtkWidget *size = _bar_group(view->row_geometry, _("Size"));
  view->geometry_width = _bar_spin(size, 1.0, 1e6, 1.0, 0, _("Width, canvas units"), G_CALLBACK(_bar_geometry_changed), self);
  view->geometry_height = _bar_spin(size, 1.0, 1e6, 1.0, 0, _("Height, canvas units"), G_CALLBACK(_bar_geometry_changed), self);
  GtkWidget *angle = _bar_group(view->row_geometry, _("Angle"));
  view->geometry_rotation = _bar_spin(angle, -360.0, 360.0, 1.0, 1, _("Rotation, degrees clockwise"),
                                      G_CALLBACK(_bar_geometry_changed), self);
  gtk_spin_button_set_wrap(GTK_SPIN_BUTTON(view->geometry_rotation), TRUE);

  // 3. Opacity, and the colour under the content.
  view->row_opacity = _bar_row(bar, _("Opacity"));
  view->object_opacity = _bar_spin(view->row_opacity, 0.0, 100.0, 5.0, 0, _("Opacity, percent"),
                                   G_CALLBACK(_bar_opacity_changed), self);
  GtkWidget *background = _bar_group(view->row_opacity, _("Background"));
  view->object_background = _bar_color_button(background,
                                              _("Colour under the content, filling the frame or the whole cutout: what a feather dissolves into"),
                                              G_CALLBACK(_bar_background_set), self);
  view->object_no_background = _bar_toggle(background, _("Transparent"), _("No background: the canvas shows through"),
                                           G_CALLBACK(_bar_no_background_toggled), self);

  // 4. The border, or a connector's line.
  view->row_border = _bar_row(bar, _("Border"));
  view->border_default = _bar_toggle(view->row_border, _("Canvas default"), _("Use the canvas's uniform border"),
                                     G_CALLBACK(_bar_border_default_toggled), self);
  view->border_custom = _bar_group(view->row_border, NULL);
  GtkWidget *border_width = _bar_group(view->border_custom, _("Width"));
  view->object_border_width = _bar_spin(border_width, 0.0, 200.0, 1.0, 0,
                                        _("Border width, in canvas units. A rectangular frame's border sits inside its edge; a cut-out frame's starts past the feather, outward."),
                                        G_CALLBACK(_bar_border_width_changed), self);
  view->object_border_color = _bar_color_button(view->border_custom, _("Border colour and opacity"),
                                                G_CALLBACK(_bar_border_color_set), self);
  GtkWidget *corners = _bar_group(view->row_border, _("Corners"));
  view->corner_default = _bar_toggle(corners, _("Canvas default"), _("Use the canvas's default corner radius"),
                                     G_CALLBACK(_bar_corner_default_toggled), self);
  view->object_corner_radius = _bar_spin(corners, 0.0, 5000.0, 1.0, 0,
                                         _("Radius of the frame's rounded corners, in canvas units; 0 is square"),
                                         G_CALLBACK(_bar_corner_changed), self);
  view->row_line = _bar_row(bar, _("Line"));
  GtkWidget *line_width = _bar_group(view->row_line, _("Width"));
  view->connector_width = _bar_spin(line_width, 1.0, 40.0, 1.0, 0, _("Line width, in canvas units"),
                                    G_CALLBACK(_bar_connector_width_changed), self);
  view->connector_dashed = _bar_toggle(view->row_line, _("Dashed"), NULL, G_CALLBACK(_bar_connector_dashed_toggled), self);
  view->connector_color = _bar_color_button(view->row_line, _("Colour"), G_CALLBACK(_bar_connector_color_set), self);

  // 5. The shadow: a signed radius, outside the object when positive, inside when negative, none at zero.
  view->row_shadow = _bar_row(bar, _("Shadow"));
  view->shadow_default = _bar_toggle(view->row_shadow, _("Canvas default"), _("Use the canvas's default shadow"),
                                     G_CALLBACK(_bar_shadow_default_toggled), self);
  view->shadow_custom = _bar_group(view->row_shadow, NULL);
  GtkWidget *shadow_x = _bar_group(view->shadow_custom, _("X offset"));
  view->object_shadow_offset_x = _bar_spin(shadow_x, -500.0, 500.0, 1.0, 0, _("Shadow offset to the right, in canvas units"),
                                           G_CALLBACK(_bar_shadow_changed), self);
  GtkWidget *shadow_y = _bar_group(view->shadow_custom, _("Y offset"));
  view->object_shadow_offset_y = _bar_spin(shadow_y, -500.0, 500.0, 1.0, 0, _("Shadow offset downwards, in canvas units"),
                                           G_CALLBACK(_bar_shadow_changed), self);
  GtkWidget *shadow_radius = _bar_group(view->shadow_custom, _("Radius"));
  view->object_shadow_blur = _bar_spin(shadow_radius, -500.0, 500.0, 1.0, 0,
                                       _("Shadow radius, in canvas units: 0 is no shadow, positive drops it outside the object, negative casts it inside along the edges"),
                                       G_CALLBACK(_bar_shadow_changed), self);
  view->object_shadow_color = _bar_color_button(view->shadow_custom, _("Shadow colour and strength"), G_CALLBACK(_bar_shadow_changed), self);

  // 6. The cutout.
  view->row_cutout = _bar_row(bar, _("Cutout"));
  view->object_cutout_shape = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(view->object_cutout_shape), _("None"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(view->object_cutout_shape), _("Circle"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(view->object_cutout_shape), _("Ellipse"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(view->object_cutout_shape), _("Polygon"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(view->object_cutout_shape), _("Gradient"));
  gtk_widget_set_tooltip_text(view->object_cutout_shape,
                              _("A drawn shape that cuts the frame out of its rectangle, with a fall-off past its edge"));
  g_signal_connect(view->object_cutout_shape, "changed", G_CALLBACK(_bar_cutout_shape_changed), self);
  gtk_box_pack_start(GTK_BOX(view->row_cutout), view->object_cutout_shape, FALSE, FALSE, 0);
  GtkWidget *feather = _bar_group(view->row_cutout, _("Feather"));
  view->object_cutout_feather = _bar_spin(feather, 0.0, 100.0, 1.0, 0,
                                          _("Fall-off past the shape's edge, percent of the frame's shorter side. The wheel over the frame changes it while editing."),
                                          G_CALLBACK(_bar_cutout_feather_changed), self);
  GtkWidget *cutout_size = _bar_group(view->row_cutout, _("Size"));
  view->object_cutout_size_x = _bar_spin(cutout_size, 0.5, 200.0, 0.5, 1,
                                         _("The circle's radius, the ellipse's horizontal radius or the gradient's extent, percent of the frame's shorter side"),
                                         G_CALLBACK(_bar_cutout_size_changed), self);
  view->object_cutout_size_y = _bar_spin(cutout_size, 0.5, 200.0, 0.5, 1,
                                         _("The ellipse's vertical radius, percent of the frame's shorter side"),
                                         G_CALLBACK(_bar_cutout_size_changed), self);
  view->object_cutout_invert = _bar_toggle(view->row_cutout, _("Invert"), _("Keep what is outside the shape"),
                                           G_CALLBACK(_bar_cutout_invert_toggled), self);
  view->object_cutout_edit = _bar_toggle(view->row_cutout, _("Edit"),
                                         _("Show the shape's handles: drag them; the wheel sets the feather, Shift+wheel the opacity. "
                                           "The right-click menu edits the shape's properties and its nodes."),
                                         G_CALLBACK(_bar_cutout_edit_toggled), self);

  gtk_widget_show_all(bar);
  gtk_widget_hide(bar);
  g_object_set_data(G_OBJECT(bar), "canvas-bar", GINT_TO_POINTER(1));
  g_object_set_data(G_OBJECT(bar), "bar-left", GINT_TO_POINTER(-1));
  g_object_set_data(G_OBJECT(bar), "bar-top", GINT_TO_POINTER(-1));
}

static void _bars_destroy(dt_view_t *self)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  GtkWidget *base = dt_ui_center_base(dt_gui_get_ui());
  if(view->bars_idle != 0)
  {
    g_source_remove(view->bars_idle);
    view->bars_idle = 0;
  }
  if(!IS_NULL_PTR(view->bar)) gtk_container_remove(GTK_CONTAINER(base), view->bar);
  if(view->bars_position_handler != 0)
  {
    g_signal_handler_disconnect(base, view->bars_position_handler);
    view->bars_position_handler = 0;
  }
  view->bar = NULL;
  view->bars_signature = 0;
}

/** The screen box of an object: its frame, or a connector's whole route. */
static gboolean _object_screen_box(const dt_canvas_view_t *view, const dt_canvas_object_t *object, double *min_x,
                                   double *min_y, double *max_x, double *max_y)
{
  *min_x = INFINITY;
  *min_y = INFINITY;
  *max_x = -INFINITY;
  *max_y = -INFINITY;
  if(dt_canvas_object_is_frame(object))
  {
    const dt_canvas_rect_t bounds = dt_canvas_object_bounds(object);
    *min_x = (bounds.x - view->center_x) * view->zoom + view->width * 0.5;
    *min_y = (bounds.y - view->center_y) * view->zoom + view->height * 0.5;
    *max_x = *min_x + bounds.width * view->zoom;
    *max_y = *min_y + bounds.height * view->zoom;
    return TRUE;
  }
  dt_canvas_route_t route;
  if(!dt_canvas_connector_route(view->canvas, object, &route)) return FALSE;
  for(int idx = 0; idx < route.point_count; idx++)
  {
    const double screen_x = (route.points[2 * idx] - view->center_x) * view->zoom + view->width * 0.5;
    const double screen_y = (route.points[2 * idx + 1] - view->center_y) * view->zoom + view->height * 0.5;
    *min_x = fmin(*min_x, screen_x);
    *min_y = fmin(*min_y, screen_y);
    *max_x = fmax(*max_x, screen_x);
    *max_y = fmax(*max_y, screen_y);
  }
  return TRUE;
}

/** Place the bar immediately below the object, above it when there is no room below. */
static void _bar_place(dt_canvas_view_t *view, const dt_canvas_object_t *object)
{
  double min_x = 0.0;
  double min_y = 0.0;
  double max_x = 0.0;
  double max_y = 0.0;
  if(!_object_screen_box(view, object, &min_x, &min_y, &max_x, &max_y)) return;
  GtkWidget *bar = view->bar;
  GtkRequisition natural;
  gtk_widget_get_preferred_size(bar, NULL, &natural);
  const int spacing = DT_PIXEL_APPLY_DPI(6);
  int left = (int)lround(min_x);
  int top = (int)lround(max_y) + spacing;
  if(top + natural.height > view->height - spacing) top = (int)lround(min_y) - natural.height - spacing;
  left = CLAMP(left, spacing, MAX(spacing, view->width - natural.width - spacing));
  top = CLAMP(top, spacing, MAX(spacing, view->height - natural.height - spacing));
  const int last_left = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(bar), "bar-left"));
  const int last_top = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(bar), "bar-top"));
  if(left != last_left || top != last_top)
  {
    g_object_set_data(G_OBJECT(bar), "bar-left", GINT_TO_POINTER(left));
    g_object_set_data(G_OBJECT(bar), "bar-top", GINT_TO_POINTER(top));
    gtk_widget_queue_allocate(dt_ui_center_base(dt_gui_get_ui()));
  }
}

/** Hide the bar at once, without waiting for the idle: a click on the background dismisses it. */
static void _bars_hide_now(dt_canvas_view_t *view)
{
  if(IS_NULL_PTR(view->bar)) return;
  gtk_widget_hide(view->bar);
  view->bars_signature = 0;
}

static void _bars_refresh(dt_view_t *self, gboolean force)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view->bar)) return;
  // No bar while a gesture is running: it would follow every motion through a re-allocation.
  const gboolean dragging = view->drag != DT_CANVAS_DRAG_NONE;
  const dt_canvas_object_t *object = (view->connecting || dragging) ? NULL : _bar_target(view);
  const uint32_t kind = IS_NULL_PTR(object) ? DT_CANVAS_OBJECT_NONE : object->kind;
  const uint64_t signature = view->canvas->generation * 131u + (IS_NULL_PTR(object) ? 0u : object->id) * 7u + kind
                             + (view->mask_editing ? 3u : 0u);
  if(force || signature != view->bars_signature)
  {
    view->bars_signature = signature;
    view->bars_refilling = TRUE;
    if(kind == DT_CANVAS_OBJECT_TEXT)
    {
      gtk_font_chooser_set_font(GTK_FONT_CHOOSER(view->text_font), dt_canvas_text_effective_font(view->canvas, object));
      _color_to_button(view->text_color, &object->text.text_color);
      gtk_combo_box_set_active(GTK_COMBO_BOX(view->text_align_h), CLAMP((int)object->text.align_h, 0, 3));
      gtk_combo_box_set_active(GTK_COMBO_BOX(view->text_align_v), CLAMP((int)object->text.align_v, 0, 2));
    }
    else if(kind == DT_CANVAS_OBJECT_CONNECTOR)
    {
      const uint32_t arrows = object->connector.style
                              & (DT_CANVAS_CONNECTOR_ARROW_END | DT_CANVAS_CONNECTOR_ARROW_START);
      int arrows_index = 0;
      if(arrows == DT_CANVAS_CONNECTOR_ARROW_END) arrows_index = 1;
      else if(arrows == DT_CANVAS_CONNECTOR_ARROW_START) arrows_index = 2;
      else if(arrows != 0) arrows_index = 3;
      gtk_combo_box_set_active(GTK_COMBO_BOX(view->connector_route), CLAMP((int)object->connector.routing, 0, 2));
      gtk_combo_box_set_active(GTK_COMBO_BOX(view->connector_arrows), arrows_index);
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(view->connector_via), object->connector.via_count > 0);
    }
    else if(kind == DT_CANVAS_OBJECT_MAP)
    {
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(view->map_latitude), object->map.latitude);
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(view->map_longitude), object->map.longitude);
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(view->map_zoom), object->map.zoom);
      gtk_combo_box_set_active(GTK_COMBO_BOX(view->map_source), dt_canvas_map_source_index(object->map.source));
    }
    if(!IS_NULL_PTR(object))
    {
      const gboolean frame = dt_canvas_object_is_frame(object);
      const gboolean connector = kind == DT_CANVAS_OBJECT_CONNECTOR;
      if(frame)
      {
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(view->geometry_x), object->x);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(view->geometry_y), object->y);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(view->geometry_width), object->width);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(view->geometry_height), object->height);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(view->geometry_rotation), object->rotation * 180.0 / M_PI);
        const dt_canvas_color_t background = dt_canvas_object_background(object);
        _color_to_button(view->object_background, &background);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(view->object_no_background), background.alpha <= 0.0f);
        dt_canvas_color_t border_color;
        float border_width = 0.0f;
        dt_canvas_object_effective_border(view->canvas, object, &border_color, &border_width);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(view->object_border_width), border_width);
        _color_to_button(view->object_border_color, &border_color);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(view->border_default),
                                     !(object->flags & DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(view->corner_default),
                                     !(object->flags & DT_CANVAS_OBJECT_FLAG_CORNER_OVERRIDE));
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(view->object_corner_radius),
                                  dt_canvas_object_effective_corner_radius(view->canvas, object));
        gtk_widget_set_visible(view->object_corner_radius, (object->flags & DT_CANVAS_OBJECT_FLAG_CORNER_OVERRIDE) != 0);
      }
      if(connector)
      {
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(view->connector_width), object->connector.line_width);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(view->connector_dashed),
                                     (object->connector.style & DT_CANVAS_CONNECTOR_DASHED) != 0);
        _color_to_button(view->connector_color, &object->connector.color);
      }
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(view->object_opacity), (1.0 - CLAMP(object->transparency, 0.0f, 1.0f)) * 100.0);
      dt_canvas_shadow_t shadow;
      dt_canvas_object_effective_shadow(view->canvas, object, &shadow);
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(view->shadow_default),
                                   !(object->flags & DT_CANVAS_OBJECT_FLAG_SHADOW_OVERRIDE));
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(view->object_shadow_offset_x), shadow.offset_x);
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(view->object_shadow_offset_y), shadow.offset_y);
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(view->object_shadow_blur), shadow.blur);
      _color_to_button(view->object_shadow_color, &shadow.color);
      gtk_combo_box_set_active(GTK_COMBO_BOX(view->object_cutout_shape), CLAMP((int)object->mask.shape, 0, DT_CANVAS_MASK_GRADIENT));
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(view->object_cutout_feather), object->mask.feather * 100.0);
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(view->object_cutout_size_x), object->mask.radius_x * 100.0);
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(view->object_cutout_size_y), object->mask.radius_y * 100.0);
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(view->object_cutout_invert), (object->mask.flags & DT_CANVAS_MASK_INVERT) != 0);
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(view->object_cutout_edit), view->mask_editing);
      const gboolean cut = frame && object->mask.shape != DT_CANVAS_MASK_NONE;
      gtk_widget_set_visible(view->row_text, kind == DT_CANVAS_OBJECT_TEXT);
      gtk_widget_set_visible(view->row_connector, connector);
      gtk_widget_set_visible(view->row_map, kind == DT_CANVAS_OBJECT_MAP);
      gtk_widget_set_visible(view->row_geometry, frame);
      gtk_widget_set_visible(gtk_widget_get_parent(view->object_background), frame);
      gtk_widget_set_visible(view->row_border, frame);
      gtk_widget_set_visible(view->border_custom, frame && (object->flags & DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE));
      gtk_widget_set_visible(view->row_line, connector);
      gtk_widget_set_visible(view->shadow_custom, (object->flags & DT_CANVAS_OBJECT_FLAG_SHADOW_OVERRIDE) != 0);
      gtk_widget_set_visible(view->row_cutout, frame);
      gtk_widget_set_visible(gtk_widget_get_parent(view->object_cutout_feather), cut && object->mask.shape != DT_CANVAS_MASK_GRADIENT);
      gtk_widget_set_visible(gtk_widget_get_parent(view->object_cutout_size_x), cut && object->mask.shape != DT_CANVAS_MASK_POLYGON);
      gtk_widget_set_visible(view->object_cutout_size_y, cut && object->mask.shape == DT_CANVAS_MASK_ELLIPSE);
      gtk_widget_set_visible(view->object_cutout_invert, cut);
      gtk_widget_set_visible(view->object_cutout_edit, cut);
    }
    view->bars_refilling = FALSE;
    gtk_widget_set_visible(view->bar, !IS_NULL_PTR(object));
  }
  if(!IS_NULL_PTR(object)) _bar_place(view, object);
}

static gboolean _bars_idle(gpointer data)
{
  dt_view_t *self = (dt_view_t *)data;
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  view->bars_idle = 0;
  _bars_refresh(self, FALSE);
  return G_SOURCE_REMOVE;
}

/** Schedule a placement off the draw path: moving an overlay child from inside a draw glitches. */
static void _bars_request(dt_view_t *self)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view) || IS_NULL_PTR(view->bar) || view->bars_idle != 0) return;
  view->bars_idle = g_idle_add(_bars_idle, self);
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
  _bars_request(dt_view_manager_get_global()->proxy.canvas.view);
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
  const double handle = CANVAS_HANDLE_PIXELS / view->zoom;
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
    const double rotate_y = -half_height - CANVAS_ROTATE_HANDLE_OFFSET_PIXELS / view->zoom;
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

/** The frame's shorter side, the unit of a cutout's radii and feather. */
static double _mask_side(const dt_canvas_object_t *object)
{
  return fmax(fmin(object->width, object->height), 1.0);
}

/** A cutout's unit-square point in the frame's local units, origin at its centre. */
static void _mask_to_local(const dt_canvas_object_t *object, const double u, const double v, double *local_x,
                           double *local_y)
{
  *local_x = (u - 0.5) * object->width;
  *local_y = (v - 0.5) * object->height;
}

/**
 * The cutout's handles in local units: [0] the centre or anchor, [1] the radius (circle), the
 * first radius (ellipse) or the reach (gradient), [2] the ellipse's second radius, [3] the
 * feather of a circle or an ellipse, on its dashed ring.
 * @return how many there are; a polygon's nodes are its own handles.
 */
static int _mask_handle_points(const dt_canvas_object_t *object, double points[8])
{
  const dt_canvas_mask_t *mask = &object->mask;
  const double side = _mask_side(object);
  double center_x = 0.0;
  double center_y = 0.0;
  _mask_to_local(object, mask->center_x, mask->center_y, &center_x, &center_y);
  points[0] = center_x;
  points[1] = center_y;
  const double angle = mask->rotation * M_PI / 180.0;
  switch(mask->shape)
  {
    case DT_CANVAS_MASK_CIRCLE:
      points[2] = center_x + mask->radius_x * side;
      points[3] = center_y;
      // The feather handle sits on the dashed ring, off the radius handle's axis.
      points[6] = center_x + M_SQRT1_2 * (mask->radius_x + mask->feather) * side;
      points[7] = center_y + M_SQRT1_2 * (mask->radius_x + mask->feather) * side;
      return 4;
    case DT_CANVAS_MASK_ELLIPSE:
      points[2] = center_x + cos(angle) * mask->radius_x * side;
      points[3] = center_y + sin(angle) * mask->radius_x * side;
      points[4] = center_x - sin(angle) * mask->radius_y * side;
      points[5] = center_y + cos(angle) * mask->radius_y * side;
      points[6] = center_x - cos(angle) * (mask->radius_x + mask->feather) * side;
      points[7] = center_y - sin(angle) * (mask->radius_x + mask->feather) * side;
      return 4;
    case DT_CANVAS_MASK_GRADIENT:
      // The reach handle sits across the line, on the side the fall-off goes.
      points[2] = center_x - sin(angle) * mask->radius_x * side;
      points[3] = center_y + cos(angle) * mask->radius_x * side;
      return 2;
    default:
      return mask->shape == DT_CANVAS_MASK_NONE ? 0 : 1;
  }
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

/**
 * The polygon's control points for the segment leaving node `index`, as the masks module
 * computes them: a Catmull-Rom tangent through a smooth node, the stored points otherwise.
 */
static void _mask_polygon_controls(const dt_canvas_mask_t *mask, const uint32_t index, float control1[2],
                                   float control2[2])
{
  const uint32_t count = mask->node_count;
  const float *previous = mask->nodes + (size_t)((index + count - 1) % count) * DT_CANVAS_MASK_NODE_FLOATS;
  const float *from = mask->nodes + (size_t)index * DT_CANVAS_MASK_NODE_FLOATS;
  const float *to = mask->nodes + (size_t)((index + 1) % count) * DT_CANVAS_MASK_NODE_FLOATS;
  const float *after = mask->nodes + (size_t)((index + 2) % count) * DT_CANVAS_MASK_NODE_FLOATS;
  if(from[6] != 0.0f)
  {
    control1[0] = (-previous[0] + 6.0f * from[0] + to[0]) / 6.0f;
    control1[1] = (-previous[1] + 6.0f * from[1] + to[1]) / 6.0f;
  }
  else
  {
    control1[0] = from[4];
    control1[1] = from[5];
  }
  if(to[6] != 0.0f)
  {
    control2[0] = (from[0] + 6.0f * to[0] - after[0]) / 6.0f;
    control2[1] = (from[1] + 6.0f * to[1] - after[1]) / 6.0f;
  }
  else
  {
    control2[0] = to[2];
    control2[1] = to[3];
  }
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
    _mask_to_local(object, from[0], from[1], &from_x, &from_y);
    if(idx == 0) cairo_move_to(cr, from_x, from_y);
    float control1[2];
    float control2[2];
    _mask_polygon_controls(mask, idx, control1, control2);
    double control1_x = 0.0;
    double control1_y = 0.0;
    double control2_x = 0.0;
    double control2_y = 0.0;
    double to_x = 0.0;
    double to_y = 0.0;
    _mask_to_local(object, control1[0], control1[1], &control1_x, &control1_y);
    _mask_to_local(object, control2[0], control2[1], &control2_x, &control2_y);
    _mask_to_local(object, to[0], to[1], &to_x, &to_y);
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
  const double handle = CANVAS_HANDLE_PIXELS * 0.6 / view->zoom;
  const double side = _mask_side(object);
  const double dashes[2] = { 4.0 * hairline, 4.0 * hairline };
  cairo_save(cr);
  cairo_translate(cr, object->x, object->y);
  cairo_rotate(cr, object->rotation);
  cairo_set_line_width(cr, hairline * 1.5);
  double points[8] = { 0.0 };
  const int count = _mask_handle_points(object, points);
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
      _mask_polygon_path(cr, object);
      cairo_stroke(cr);
      for(uint32_t idx = 0; idx < mask->node_count; idx++)
      {
        const float *node = mask->nodes + (size_t)idx * DT_CANVAS_MASK_NODE_FLOATS;
        double local_x = 0.0;
        double local_y = 0.0;
        _mask_to_local(object, node[0], node[1], &local_x, &local_y);
        cairo_rectangle(cr, local_x - handle, local_y - handle, 2.0 * handle, 2.0 * handle);
        cairo_set_source_rgba(cr, 1.0, 0.85, 0.3, 0.95);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.8);
        cairo_set_line_width(cr, hairline);
        cairo_stroke(cr);
      }
      break;
    default:
      break;
  }
  for(int idx = 0; idx < count; idx++) _paint_dot(cr, points[2 * idx], points[2 * idx + 1], handle, hairline);
  cairo_restore(cr);
}

/** Which cutout handle is under a canvas point, when the cutout is being edited. */
static dt_canvas_drag_t _mask_handle_at(const dt_canvas_view_t *view, const dt_canvas_object_t *object,
                                        const double x, const double y, int *index)
{
  *index = -1;
  if(!view->mask_editing || !dt_canvas_object_is_frame(object) || object->mask.shape == DT_CANVAS_MASK_NONE)
    return DT_CANVAS_DRAG_NONE;
  if(view->selection->len != 1) return DT_CANVAS_DRAG_NONE;
  double local_x = 0.0;
  double local_y = 0.0;
  dt_canvas_object_to_local(object, x, y, &local_x, &local_y);
  const double reach = CANVAS_HANDLE_PIXELS / view->zoom;
  if(object->mask.shape == DT_CANVAS_MASK_POLYGON)
  {
    for(uint32_t idx = 0; idx < object->mask.node_count; idx++)
    {
      const float *node = object->mask.nodes + (size_t)idx * DT_CANVAS_MASK_NODE_FLOATS;
      double node_x = 0.0;
      double node_y = 0.0;
      _mask_to_local(object, node[0], node[1], &node_x, &node_y);
      if(fabs(local_x - node_x) <= reach && fabs(local_y - node_y) <= reach)
      {
        *index = (int)idx;
        return DT_CANVAS_DRAG_MASK_NODE;
      }
    }
    return DT_CANVAS_DRAG_NONE;
  }
  double points[8] = { 0.0 };
  const int count = _mask_handle_points(object, points);
  // The outer handles first: with a small shape they sit over the centre.
  for(int idx = count - 1; idx >= 0; idx--)
  {
    if(hypot(local_x - points[2 * idx], local_y - points[2 * idx + 1]) > reach) continue;
    if(idx == 0) return DT_CANVAS_DRAG_MASK_CENTER;
    if(idx == 3) return DT_CANVAS_DRAG_MASK_FEATHER;
    if(idx == 2) return DT_CANVAS_DRAG_MASK_RADIUS_Y;
    return object->mask.shape == DT_CANVAS_MASK_GRADIENT ? DT_CANVAS_DRAG_MASK_REACH : DT_CANVAS_DRAG_MASK_RADIUS_X;
  }
  return DT_CANVAS_DRAG_NONE;
}

/** The polygon edge under a canvas point: the index of the node it starts at, or -1. */
static int _mask_segment_at(const dt_canvas_view_t *view, const dt_canvas_object_t *object, const double x,
                            const double y)
{
  if(!dt_canvas_object_is_frame(object) || object->mask.shape != DT_CANVAS_MASK_POLYGON) return -1;
  double local_x = 0.0;
  double local_y = 0.0;
  dt_canvas_object_to_local(object, x, y, &local_x, &local_y);
  const double reach = CANVAS_HANDLE_PIXELS / view->zoom;
  for(uint32_t idx = 0; idx < object->mask.node_count; idx++)
  {
    const float *from = object->mask.nodes + (size_t)idx * DT_CANVAS_MASK_NODE_FLOATS;
    const float *to = object->mask.nodes + (size_t)((idx + 1) % object->mask.node_count) * DT_CANVAS_MASK_NODE_FLOATS;
    double from_x = 0.0;
    double from_y = 0.0;
    double to_x = 0.0;
    double to_y = 0.0;
    _mask_to_local(object, from[0], from[1], &from_x, &from_y);
    _mask_to_local(object, to[0], to[1], &to_x, &to_y);
    const double edge_x = to_x - from_x;
    const double edge_y = to_y - from_y;
    const double length2 = edge_x * edge_x + edge_y * edge_y;
    const double t = length2 > 0.0 ? CLAMP(((local_x - from_x) * edge_x + (local_y - from_y) * edge_y) / length2, 0.0, 1.0)
                                   : 0.0;
    if(hypot(local_x - (from_x + t * edge_x), local_y - (from_y + t * edge_y)) <= reach) return (int)idx;
  }
  return -1;
}

/** Apply a cutout drag: the handle follows the pointer, in the frame's own unit square. */
static void _mask_drag(dt_canvas_view_t *view, dt_canvas_object_t *object, const double x, const double y)
{
  dt_canvas_mask_t *mask = &object->mask;
  double local_x = 0.0;
  double local_y = 0.0;
  dt_canvas_object_to_local(object, x, y, &local_x, &local_y);
  const double side = _mask_side(object);
  const double u = local_x / object->width + 0.5;
  const double v = local_y / object->height + 0.5;
  double center_x = 0.0;
  double center_y = 0.0;
  _mask_to_local(object, mask->center_x, mask->center_y, &center_x, &center_y);
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
      const double handle = CANVAS_VIA_HANDLE_PIXELS / view->zoom;
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
      cairo_save(cr);
      cairo_translate(cr, object->x, object->y);
      cairo_rotate(cr, object->rotation);
      cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.5);
      cairo_set_line_width(cr, 1.0 / view->zoom);
      cairo_rectangle(cr, -object->width * 0.5, -object->height * 0.5, object->width, object->height);
      cairo_stroke(cr);
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
  double background[3] = { 0.2, 0.2, 0.2 };
  if(view->canvas->background_style == DT_CANVAS_BACKGROUND_MOLESKINE)
  {
    background[0] = 0.961;
    background[1] = 0.941;
    background[2] = 0.886;
  }
  else if(view->canvas->background_style >= DT_CANVAS_BACKGROUND_WATERCOLOUR
          && view->canvas->background_style < DT_CANVAS_BACKGROUND_LAST)
  {
    background[0] = 0.97;
    background[1] = 0.96;
    background[2] = 0.94;
  }
  else
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
}

/* --- gestures ---------------------------------------------------------------------- */

/** Which handle of a selected frame is under the canvas point: 0..3 a corner, 4 the rotation, -1 none. */
static int _handle_at(const dt_canvas_view_t *view, const dt_canvas_object_t *object, const double x, const double y)
{
  if(!dt_canvas_object_is_frame(object) || (object->flags & DT_CANVAS_OBJECT_FLAG_LOCKED)) return -1;
  double local_x = 0.0;
  double local_y = 0.0;
  dt_canvas_object_to_local(object, x, y, &local_x, &local_y);
  const double reach = CANVAS_HANDLE_PIXELS / view->zoom;
  const double half_width = object->width * 0.5;
  const double half_height = object->height * 0.5;
  const double corners[8] = { -half_width, -half_height, half_width, -half_height,
                              half_width,  half_height,  -half_width, half_height };
  for(int idx = 0; idx < 4; idx++)
  {
    if(fabs(local_x - corners[2 * idx]) <= reach && fabs(local_y - corners[2 * idx + 1]) <= reach) return idx;
  }
  const double rotate_y = -half_height - CANVAS_ROTATE_HANDLE_OFFSET_PIXELS / view->zoom;
  if(fabs(local_x) <= reach && fabs(local_y - rotate_y) <= reach) return 4;
  return -1;
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
  const double reach = (CANVAS_VIA_HANDLE_PIXELS + 3.0) / view->zoom;
  for(guint idx = 0; idx < view->selection->len; idx++)
  {
    dt_canvas_object_t *object = dt_canvas_find_object(view->canvas, g_array_index(view->selection, uint32_t, idx));
    dt_canvas_route_t route;
    if(!_connector_handles(view, object, &route)) continue;
    *owner = object;
    if(hypot(route.control1_x - x, route.control1_y - y) <= reach) return DT_CANVAS_DRAG_HANDLE_FROM;
    if(route.segment_count == 2)
    {
      if(hypot(route.control2_x - x, route.control2_y - y) <= reach)
      {
        *sign = -1;
        return DT_CANVAS_DRAG_HANDLE_VIA;
      }
      if(hypot(route.control3_x - x, route.control3_y - y) <= reach)
      {
        *sign = 1;
        return DT_CANVAS_DRAG_HANDLE_VIA;
      }
      if(hypot(route.control4_x - x, route.control4_y - y) <= reach) return DT_CANVAS_DRAG_HANDLE_TO;
    }
    else if(hypot(route.control2_x - x, route.control2_y - y) <= reach)
    {
      return DT_CANVAS_DRAG_HANDLE_TO;
    }
  }
  *owner = NULL;
  return DT_CANVAS_DRAG_NONE;
}

static void _paint_tangent_handle(cairo_t *cr, const dt_canvas_view_t *view, const double anchor_x,
                                  const double anchor_y, const double handle_x, const double handle_y)
{
  const double radius = (CANVAS_VIA_HANDLE_PIXELS - 2.0) / view->zoom;
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
  const double reach = (CANVAS_VIA_HANDLE_PIXELS + 3.0) / view->zoom;
  for(guint idx = 0; idx < view->selection->len; idx++)
  {
    dt_canvas_object_t *object = dt_canvas_find_object(view->canvas, g_array_index(view->selection, uint32_t, idx));
    if(IS_NULL_PTR(object) || object->kind != DT_CANVAS_OBJECT_CONNECTOR || object->connector.via_count == 0) continue;
    if(fabs(object->connector.via_x - x) <= reach && fabs(object->connector.via_y - y) <= reach) return object;
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
 * rules: the grid first, then a neighbour one gutter away or in line, which wins when within reach.
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
  if(rules & DT_CANVAS_SNAP_GUTTER)
  {
    double gutter_x = 0.0;
    double gutter_y = 0.0;
    dt_canvas_snap_to_neighbours(view->canvas, &bounds, view->selection, CANVAS_NEIGHBOUR_SNAP_PIXELS / view->zoom,
                                 DT_CANVAS_EDGE_ALL, &gutter_x, &gutter_y);
    if(gutter_x != 0.0) delta_x = gutter_x;
    if(gutter_y != 0.0) delta_y = gutter_y;
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
  // The dragged corner follows the pointer; the opposite corner stays put. An image keeps its
  // aspect ratio, a text frame resizes freely.
  double local_x = 0.0;
  double local_y = 0.0;
  dt_canvas_object_to_local(object, x, y, &local_x, &local_y);
  const double sign_x = (view->scale_corner == 1 || view->scale_corner == 2) ? 1.0 : -1.0;
  const double sign_y = (view->scale_corner == 2 || view->scale_corner == 3) ? 1.0 : -1.0;
  const gboolean proportional = object->kind == DT_CANVAS_OBJECT_IMAGE;
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

  // Snapping, in the canvas's order of rules: the grid, then the gutter, then a neighbour's size.
  // Each later rule that triggers replaces the earlier answer; a proportional frame follows its width.
  const uint32_t rules = view->canvas->grid_flags;
  const double threshold = CANVAS_NEIGHBOUR_SNAP_PIXELS / view->zoom;
  if(rules & DT_CANVAS_GRID_SNAP)
  {
    new_width = fmax(dt_canvas_snap(view->canvas, new_width), 20.0);
    new_height = proportional ? new_width / ratio : fmax(dt_canvas_snap(view->canvas, new_height), 20.0);
  }
  if(rules & DT_CANVAS_SNAP_GUTTER)
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
  _bars_request(self);
  dt_control_queue_redraw_center();
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
  const double tolerance = CANVAS_PICK_TOLERANCE_PIXELS / view->zoom;

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
    _bars_hide_now(view);
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
    // The cutout's handles, when it is being edited: they sit over the frame they cut.
    dt_canvas_object_t *mask_owner = _single_selected(view);
    int mask_index = -1;
    const dt_canvas_drag_t mask_drag = _mask_handle_at(view, mask_owner, canvas_x, canvas_y, &mask_index);
    if(mask_drag != DT_CANVAS_DRAG_NONE)
    {
      if(mask_drag == DT_CANVAS_DRAG_MASK_NODE && type == GDK_2BUTTON_PRESS)
      {
        dt_canvas_t *before = _begin_edit(view);
        float *node = mask_owner->mask.nodes + (size_t)mask_index * DT_CANVAS_MASK_NODE_FLOATS;
        node[6] = node[6] != 0.0f ? 0.0f : 1.0f;
        dt_canvas_touch(view->canvas);
        _record_undo(self, before);
        _end_gesture(self);
        return 1;
      }
      if(mask_drag == DT_CANVAS_DRAG_MASK_NODE && shift)
      {
        dt_canvas_t *before = _begin_edit(view);
        if(dt_canvas_mask_remove_node(view->canvas, mask_owner, (uint32_t)mask_index))
          _record_undo(self, before);
        else
          dt_canvas_free(before);
        dt_control_queue_redraw_center();
        return 1;
      }
      view->drag_snapshot = _begin_edit(view);
      view->drag = mask_drag;
      view->mask_handle = mask_index;
      _bars_hide_now(view);
      dt_control_change_cursor(GDK_FLEUR);
      return 1;
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
        return 1;
      }
    }
    dt_canvas_object_t *handle_owner = NULL;
    int handle_sign = 1;
    const dt_canvas_drag_t handle_drag = _tangent_handle_at(view, canvas_x, canvas_y, &handle_owner, &handle_sign);
    if(handle_drag != DT_CANVAS_DRAG_NONE)
    {
      _select_only(view, handle_owner->id);
      view->drag_snapshot = _begin_edit(view);
      view->drag = handle_drag;
      view->handle_sign = handle_sign;
      _bars_hide_now(view);
      dt_control_change_cursor(GDK_FLEUR);
      return 1;
    }
    dt_canvas_object_t *via_owner = _via_handle_at(view, canvas_x, canvas_y);
    if(!IS_NULL_PTR(via_owner))
    {
      _select_only(view, via_owner->id);
      view->drag_snapshot = _begin_edit(view);
      view->drag = DT_CANVAS_DRAG_VIA;
      _bars_hide_now(view);
      dt_control_change_cursor(GDK_FLEUR);
      return 1;
    }
    // Handles of the selected frames come first: they overlap the frames they belong to.
    for(guint idx = 0; idx < view->selection->len; idx++)
    {
      dt_canvas_object_t *object = dt_canvas_find_object(view->canvas, g_array_index(view->selection, uint32_t, idx));
      const int handle = _handle_at(view, object, canvas_x, canvas_y);
      if(handle < 0) continue;
      _select_only(view, object->id);
      view->drag_snapshot = _begin_edit(view);
      _bars_hide_now(view);
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
      return 1;
    }

    dt_canvas_object_t *object = dt_canvas_pick(view->canvas, canvas_x, canvas_y, tolerance);
    if(!IS_NULL_PTR(object))
    {
      if(type == GDK_2BUTTON_PRESS)
      {
        if(object->kind == DT_CANVAS_OBJECT_TEXT)
          _edit_text(self, object);
        else if(object->kind == DT_CANVAS_OBJECT_IMAGE)
          _open_in_darkroom(object);
        return 1;
      }
      if(primary || shift)
        _select_toggle(view, object->id);
      else if(!_is_selected(view, object->id))
        _select_only(view, object->id);
      if(dt_canvas_object_is_frame(object) && _is_selected(view, object->id))
      {
        view->drag = DT_CANVAS_DRAG_MOVE;
        view->drag_snapshot = _begin_edit(view);
        _bars_hide_now(view);
        dt_control_change_cursor(GDK_FLEUR);
      }
      _bars_request(self);
      dt_control_queue_redraw_center();
      return 1;
    }

    // The background: the selection and its bar go at once, before the rubber band starts.
    if(!primary && !shift) g_array_set_size(view->selection, 0);
    _bars_hide_now(view);
    view->drag = DT_CANVAS_DRAG_RUBBERBAND;
    _bars_request(self);
    dt_control_queue_redraw_center();
    return 1;
  }

  if(which == 3)
  {
    dt_canvas_object_t *object = dt_canvas_pick(view->canvas, canvas_x, canvas_y, tolerance);
    if(!IS_NULL_PTR(object) && !_is_selected(view, object->id)) _select_only(view, object->id);
    _bars_request(self);
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

void mouse_moved(dt_view_t *self, double x, double y, double pressure, int which)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  double canvas_x = 0.0;
  double canvas_y = 0.0;
  _to_canvas(view, x, y, &canvas_x, &canvas_y);
  view->pointer_inside = TRUE;
  const double delta_x = canvas_x - view->last_x;
  const double delta_y = canvas_y - view->last_y;

  switch(view->drag)
  {
    case DT_CANVAS_DRAG_PAN:
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
      _move_selection(view, delta_x, delta_y);
      if(view->selection->len > 0) _snap_selection(view, g_array_index(view->selection, uint32_t, 0));
      break;
    case DT_CANVAS_DRAG_SCALE:
    {
      dt_canvas_object_t *object = _single_selected(view);
      if(!IS_NULL_PTR(object))
      {
        view->drag_moved = TRUE;
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
        const double angle = atan2(canvas_y - object->y, canvas_x - object->x);
        double rotation = view->gesture_start_rotation + angle - view->gesture_start_angle;
        // Shift snaps to 15 degree steps.
        if(dt_modifier_is(which, GDK_SHIFT_MASK)) rotation = round(rotation / (M_PI / 12.0)) * (M_PI / 12.0);
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
        connector->connector.via_x = dt_canvas_snap(view->canvas, canvas_x);
        connector->connector.via_y = dt_canvas_snap(view->canvas, canvas_y);
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
        connector->connector.via_tangent_x = (canvas_x - connector->connector.via_x) * view->handle_sign;
        connector->connector.via_tangent_y = (canvas_y - connector->connector.via_y) * view->handle_sign;
      }
      break;
    }
    case DT_CANVAS_DRAG_MASK_CENTER:
    case DT_CANVAS_DRAG_MASK_RADIUS_X:
    case DT_CANVAS_DRAG_MASK_RADIUS_Y:
    case DT_CANVAS_DRAG_MASK_REACH:
    case DT_CANVAS_DRAG_MASK_NODE:
    case DT_CANVAS_DRAG_MASK_FEATHER:
    {
      dt_canvas_object_t *object = _single_selected(view);
      if(dt_canvas_object_is_frame(object) && object->mask.shape != DT_CANVAS_MASK_NONE)
      {
        view->drag_moved = TRUE;
        _mask_drag(view, object, canvas_x, canvas_y);
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
      const double tolerance = CANVAS_PICK_TOLERANCE_PIXELS / view->zoom;
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
  if(view->hover != 0 || view->flower_hover != DT_CANVAS_FLOWER_NONE)
  {
    view->hover = 0;
    view->flower_hover = DT_CANVAS_FLOWER_NONE;
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
    if(dt_canvas_object_contains(view->canvas, edited, canvas_x, canvas_y, CANVAS_PICK_TOLERANCE_PIXELS / view->zoom))
    {
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
      _bars_request(self);
      dt_control_queue_redraw_center();
      return 1;
    }
  }
  if(dt_modifier_is(state, GDK_SHIFT_MASK))
  {
    // Shift + wheel pans sideways, plain wheel zooms: a plane has no natural scroll direction.
    view->center_x += (up ? -1.0 : 1.0) * 60.0 / view->zoom;
  }
  else
  {
    _zoom_around(view, x, y, up ? CANVAS_ZOOM_STEP : 1.0 / CANVAS_ZOOM_STEP);
  }
  _bars_request(self);
  dt_control_queue_redraw_center();
  return 1;
}

int key_pressed(dt_view_t *self, GdkEventKey *event)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  const guint key = dt_keys_mainpad_alternatives(event->keyval);
  const gboolean primary = dt_modifiers_include(event->state, DT_PRIMARY_MASK);
  if(key == GDK_KEY_Escape)
  {
    if(view->connecting)
      _connect_mode_set(self, FALSE);
    else if(view->drag != DT_CANVAS_DRAG_NONE)
    {
      // Abort the gesture: put the document back the way it was before the press.
      if(!IS_NULL_PTR(view->drag_snapshot)) dt_canvas_restore(view->canvas, view->drag_snapshot);
      dt_canvas_free(view->drag_snapshot);
      view->drag_snapshot = NULL;
      view->drag = DT_CANVAS_DRAG_NONE;
      dt_control_change_cursor(GDK_LEFT_PTR);
    }
    else
      g_array_set_size(view->selection, 0);
    _bars_request(self);
    dt_control_queue_redraw_center();
    return 1;
  }
  if(key == GDK_KEY_Delete || key == GDK_KEY_BackSpace)
  {
    _delete_selection(self);
    return 1;
  }
  if(primary && (key == GDK_KEY_a || key == GDK_KEY_A))
  {
    _select_all(view);
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
    dt_canvas_t *before = _begin_edit(view);
    _move_selection(view, nudge_x, nudge_y);
    dt_canvas_touch(view->canvas);
    _record_undo(self, before);
    _bars_request(self);
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
    case DT_CANVAS_ACTION_EXPORT_PDF:
      _export_pdf(self);
      break;
    case DT_CANVAS_ACTION_ADD_TEXT:
      _add_text_frame(self, view->center_x, view->center_y);
      break;
    case DT_CANVAS_ACTION_ADD_NOTES:
      _add_notes(self);
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
    default:
      break;
  }
  _bars_request(self);
  _announce_document(self);
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
  if(style >= 0)
  {
    const uint32_t chosen = (uint32_t)CLAMP(style, 0, DT_CANVAS_BACKGROUND_LAST - 1);
    // A paper comes in its own colour: choosing one sets it, and the colour patch stays live to recolour it.
    if(chosen != view->canvas->background_style && chosen != DT_CANVAS_BACKGROUND_PLAIN && IS_NULL_PTR(rgba))
    {
      view->canvas->background = dt_canvas_background_tint(chosen);
      char text[16];
      dt_canvas_color_format(&view->canvas->background, text, sizeof(text));
      dt_conf_set_string("canvas/background_color", text);
    }
    view->canvas->background_style = chosen;
    dt_conf_set_int("canvas/background_style", (int)view->canvas->background_style);
  }
  dt_canvas_touch(view->canvas);
  dt_control_queue_redraw_center();
}

static void _proxy_set_corner_radius(dt_view_t *self, float radius)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view) || IS_NULL_PTR(view->canvas)) return;
  view->canvas->corner_radius = fmaxf(radius, 0.0f);
  dt_conf_set_float("canvas/corner_radius", view->canvas->corner_radius);
  dt_canvas_touch(view->canvas);
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
    view->canvas->paper_size = (uint32_t)CLAMP(paper, 0, 5);
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

static void _proxy_set_guides(dt_view_t *self, int mask, int value)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view) || IS_NULL_PTR(view->canvas)) return;
  view->canvas->grid_flags = (view->canvas->grid_flags & ~(uint32_t)mask) | ((uint32_t)value & (uint32_t)mask);
  dt_conf_set_bool("canvas/grid_visible", (view->canvas->grid_flags & DT_CANVAS_GRID_VISIBLE) != 0);
  dt_conf_set_bool("canvas/page_visible", (view->canvas->grid_flags & DT_CANVAS_PAGE_VISIBLE) != 0);
  dt_conf_set_bool("canvas/gutter_visible", (view->canvas->grid_flags & DT_CANVAS_GUTTER_VISIBLE) != 0);
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
  dt_conf_set_string("canvas/page_color", text);
  dt_canvas_touch(view->canvas);
  dt_control_queue_redraw_center();
}

static void _proxy_set_gutter_color(dt_view_t *self, const float *rgba)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view) || IS_NULL_PTR(view->canvas) || IS_NULL_PTR(rgba)) return;
  view->canvas->gutter_color = dt_canvas_color(rgba[0], rgba[1], rgba[2], rgba[3]);
  char text[16];
  dt_canvas_color_format(&view->canvas->gutter_color, text, sizeof(text));
  dt_conf_set_string("canvas/gutter_color", text);
  dt_canvas_touch(view->canvas);
  dt_control_queue_redraw_center();
}

static void _proxy_set_shadow(dt_view_t *self, const float *rgba, float offset_x, float offset_y, float blur)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view) || IS_NULL_PTR(view->canvas) || IS_NULL_PTR(rgba)) return;
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

static void _proxy_set_gutter(dt_view_t *self, float gutter)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view) || IS_NULL_PTR(view->canvas) || gutter < 0.0f) return;
  view->canvas->gutter = gutter;
  dt_conf_set_float("canvas/gutter", gutter);
  dt_canvas_touch(view->canvas);
  dt_control_queue_redraw_center();
}

static const dt_canvas_t *_proxy_document(dt_view_t *self)
{
  const dt_canvas_view_t *view = (const dt_canvas_view_t *)self->data;
  return IS_NULL_PTR(view) ? NULL : view->canvas;
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
  if(!IS_NULL_PTR(rgba))
  {
    view->canvas->border_color = dt_canvas_color(rgba[0], rgba[1], rgba[2], rgba[3]);
    char text[16];
    dt_canvas_color_format(&view->canvas->border_color, text, sizeof(text));
    dt_conf_set_string("canvas/border_color", text);
  }
  if(width >= 0.0f)
  {
    view->canvas->border_width = width;
    dt_conf_set_float("canvas/border_width", width);
  }
  dt_canvas_touch(view->canvas);
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
  { N_("Export the canvas as PDF"), DT_CANVAS_ACTION_EXPORT_PDF, GDK_KEY_p, DT_PRIMARY_MASK },
  { N_("Add a text frame"), DT_CANVAS_ACTION_ADD_TEXT, GDK_KEY_t, 0 },
  { N_("Add the text notes of the selected images"), DT_CANVAS_ACTION_ADD_NOTES, GDK_KEY_t, GDK_SHIFT_MASK },
  { N_("Add a map"), DT_CANVAS_ACTION_ADD_MAP, GDK_KEY_m, 0 },
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
  manager->proxy.canvas.set_gutter = _proxy_set_gutter;
  manager->proxy.canvas.set_snap_mode = _proxy_set_snap_mode;
  manager->proxy.canvas.set_background = _proxy_set_background;
  manager->proxy.canvas.set_grid_color = _proxy_set_grid_color;
  manager->proxy.canvas.set_paper = _proxy_set_paper;
  manager->proxy.canvas.set_guides = _proxy_set_guides;
  manager->proxy.canvas.set_page_color = _proxy_set_page_color;
  manager->proxy.canvas.set_gutter_color = _proxy_set_gutter_color;
  manager->proxy.canvas.set_shadow = _proxy_set_shadow;
  manager->proxy.canvas.set_texture = _proxy_set_texture;
  manager->proxy.canvas.set_corner_radius = _proxy_set_corner_radius;
}

void gui_init(dt_view_t *self)
{
  for(size_t idx = 0; idx < G_N_ELEMENTS(_accels); idx++)
  {
    dt_accels_new_canvas_action(_accel_callback, (gpointer)&_accels[idx], NULL, N_("Canvas/Actions"), _accels[idx].name,
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
  dt_canvas_free(view->drag_snapshot);
  dt_canvas_free(view->canvas);
  dt_canvas_surface_cache_free(view->cache);
  g_array_free(view->selection, TRUE);
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
  _bars_create(self);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_VIEWMANAGER_FILMSTRIP_DRAG_BEGIN,
                                  G_CALLBACK(_filmstrip_drag_begin), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_CONTROL_PROFILE_CHANGED,
                                  G_CALLBACK(_profile_changed), self);

  // The library may have moved on while we were away: compare, and refresh when asked to.
  _sync_check_all(view);
  if(dt_conf_get_bool("canvas/auto_refresh")) _refresh_images(self, DT_CANVAS_SYNC_STALE);
  _announce_document(self);
  dt_gui_refocus_center();
}

void leave(dt_view_t *self)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(dt_control_signal_get_global(), G_CALLBACK(_filmstrip_drag_begin), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(dt_control_signal_get_global(), G_CALLBACK(_profile_changed), self);
  if(view->dnd_connected)
  {
    GtkWidget *center = dt_gui_center_widget();
    g_signal_handlers_disconnect_by_func(center, G_CALLBACK(_drag_data_received), self);
    g_signal_handlers_disconnect_by_func(center, G_CALLBACK(_drag_motion), self);
    gtk_drag_dest_unset(center);
    view->dnd_connected = FALSE;
  }
  if(view->drag != DT_CANVAS_DRAG_NONE) _end_gesture(self);
  _bars_destroy(self);
  view->cursor = GDK_LEFT_PTR;
  dt_control_change_cursor(GDK_LEFT_PTR);
  view->connecting = FALSE;
  view->connect_from = 0;
  view->anchor_hover_id = 0;
  view->hover = 0;
  _store_viewport(view);
  dt_accels_disconnect_active_group(dt_gui_get_accels());
  dt_thumbtable_hide(dt_gui_get_ui()->thumbtable_filmstrip);
}

void configure(dt_view_t *self, int width, int height)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(dt_view_manager_get_current_view(dt_view_manager_get_global()) != self) return;
  view->width = width;
  view->height = height;
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
