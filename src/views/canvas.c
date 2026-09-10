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

  // floating property bars, overlay children of the centre, shown under the one selected object
  GtkWidget *text_bar;
  GtkWidget *text_font;
  GtkWidget *text_color;
  GtkWidget *text_background;
  GtkWidget *text_no_background;
  GtkWidget *text_align_h;
  GtkWidget *text_align_v;
  GtkWidget *text_border_width;
  GtkWidget *text_border_color;
  gulong bars_position_handler;         ///< the overlay's get-child-position hook
  // same-size guides, shown while a resize snaps to a neighbour's size
  gboolean guide_width_valid;
  dt_canvas_rect_t guide_width;
  gboolean guide_height_valid;
  dt_canvas_rect_t guide_height;
  GtkWidget *image_bar;
  GtkWidget *image_border_width;
  GtkWidget *image_border_color;
  GtkWidget *connector_bar;
  GtkWidget *connector_route;
  GtkWidget *connector_arrows;
  GtkWidget *connector_width;
  GtkWidget *connector_dashed;
  GtkWidget *connector_color;
  GtkWidget *connector_via;
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
static void _render_done(uint32_t object_id, uint64_t token, GBytes *jpeg, int32_t pixel_width, int32_t pixel_height,
                         uint64_t history_hash, gpointer user_data);

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
  canvas->background_style = (uint32_t)CLAMP(dt_conf_get_int("canvas/background_style"), 0, 2);
  const char *grid_color = dt_conf_get_string_const("canvas/grid_color");
  dt_canvas_color_parse(grid_color, &canvas->grid_color);
  canvas->paper_size = (uint32_t)CLAMP(dt_conf_get_int("canvas/paper_size"), 0, 5);
  canvas->paper_landscape = dt_conf_get_bool("canvas/paper_landscape") ? 1u : 0u;
  const char *border = dt_conf_get_string_const("canvas/border_color");
  dt_canvas_color_parse(border, &canvas->border_color);
  const char *background = dt_conf_get_string_const("canvas/background_color");
  dt_canvas_color_parse(background, &canvas->background);
  const char *font = dt_conf_get_string_const("canvas/default_font");
  if(!IS_NULL_PTR(font) && font[0] != '\0') g_strlcpy(canvas->default_font, font, sizeof(canvas->default_font));
  canvas->image_long_edge = dt_conf_get_int("canvas/image_long_edge");
  canvas->jpeg_quality = dt_conf_get_int("canvas/jpeg_quality");
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

/** Re-render every image frame whose status is `only` (or every one when `only` is UNKNOWN). */
static int _refresh_images(dt_view_t *self, const dt_canvas_sync_status_t only)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  int started = 0;
  for(guint idx = 0; idx < dt_canvas_object_count(view->canvas); idx++)
  {
    dt_canvas_object_t *object = dt_canvas_object_at(view->canvas, idx);
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
  if(IS_NULL_PTR(object) || object->kind != DT_CANVAS_OBJECT_IMAGE) return;
  if(IS_NULL_PTR(jpeg))
  {
    object->image.sync_status = IS_NULL_PTR(object->image.jpeg) ? DT_CANVAS_SYNC_MISSING : DT_CANVAS_SYNC_STALE;
    dt_control_log(_("the canvas could not render `%s'"), object->image.filename);
  }
  else
  {
    dt_canvas_image_set_render(view->canvas, object, jpeg, pixel_width, pixel_height, history_hash,
                               (int64_t)g_get_real_time() / G_USEC_PER_SEC);
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
  dt_canvas_layout_apply(view->canvas, ids, layout, columns);
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
    // Its properties live in the toolbar's Connector menu, applied to the selection.
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
    else
    {
      _menu_item(menu, _("Open in the darkroom"), _menu_open_darkroom, _menu_context(self, id, x, y, 0));
      _menu_item(menu, _("Refresh from the library"), _menu_refresh_image, _menu_context(self, id, x, y, 0));
      _menu_item(menu, _("Show the image's text note"), _menu_show_note, _menu_context(self, id, x, y, 0));
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
  const dt_canvas_color_t color = _color_from_button(GTK_WIDGET(button));
  if(GTK_WIDGET(button) == view->text_color)
    object->text.text_color = color;
  else
    object->text.background = color;
  BAR_EDIT_END()
}

static void _bar_text_no_background_toggled(GtkToggleButton *toggle, gpointer data)
{
  BAR_EDIT_BEGIN(DT_CANVAS_OBJECT_TEXT)
  object->text.background.alpha = gtk_toggle_button_get_active(toggle) ? 0.0f : 1.0f;
  BAR_EDIT_END()
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

static void _bar_border_default_clicked(GtkWidget *button, gpointer data)
{
  BAR_EDIT_BEGIN_FRAME()
  object->flags &= ~DT_CANVAS_OBJECT_FLAG_BORDER_OVERRIDE;
  BAR_EDIT_END()
  _bars_refresh(self, TRUE);
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

static GtkWidget *_bar_new(GtkWidget *base)
{
  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(4));
  dt_gui_add_class(bar, "dt-canvas-floating");
  gtk_widget_set_halign(bar, GTK_ALIGN_START);
  gtk_widget_set_valign(bar, GTK_ALIGN_START);
  gtk_container_set_border_width(GTK_CONTAINER(bar), DT_PIXEL_APPLY_DPI(4));
  gtk_overlay_add_overlay(GTK_OVERLAY(base), bar);
  return bar;
}

static void _bar_finish(GtkWidget *bar)
{
  gtk_widget_show_all(bar);
  gtk_widget_hide(bar);
  g_object_set_data(G_OBJECT(bar), "canvas-bar", GINT_TO_POINTER(1));
  g_object_set_data(G_OBJECT(bar), "bar-left", GINT_TO_POINTER(-1));
  g_object_set_data(G_OBJECT(bar), "bar-top", GINT_TO_POINTER(-1));
}

static GtkWidget *_bar_color_button(GtkWidget *bar, const char *tooltip, GCallback callback, gpointer data)
{
  GtkWidget *button = gtk_color_button_new();
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(button), TRUE);
  gtk_widget_set_tooltip_text(button, tooltip);
  g_signal_connect(button, "color-set", callback, data);
  gtk_box_pack_start(GTK_BOX(bar), button, FALSE, FALSE, 0);
  return button;
}

/** Border width, colour with opacity, and the button back to the canvas border. */
static void _bar_add_border_controls(dt_view_t *self, GtkWidget *bar, GtkWidget **width, GtkWidget **color)
{
  gtk_box_pack_start(GTK_BOX(bar), gtk_label_new(_("Border")), FALSE, FALSE, 0);
  *width = gtk_spin_button_new_with_range(0.0, 200.0, 1.0);
  gtk_widget_set_tooltip_text(*width, _("Border width, in canvas units. The border is part of the frame: the content shrinks inside it."));
  g_signal_connect(*width, "value-changed", G_CALLBACK(_bar_border_width_changed), self);
  gtk_box_pack_start(GTK_BOX(bar), *width, FALSE, FALSE, 0);
  *color = _bar_color_button(bar, _("Border colour and opacity"), G_CALLBACK(_bar_border_color_set), self);
  GtkWidget *default_button = gtk_button_new_with_label(_("Canvas border"));
  gtk_widget_set_tooltip_text(default_button, _("Use the canvas's uniform border for this frame"));
  g_signal_connect(default_button, "clicked", G_CALLBACK(_bar_border_default_clicked), self);
  gtk_box_pack_start(GTK_BOX(bar), default_button, FALSE, FALSE, 0);
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

static void _bars_create(dt_view_t *self)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(!IS_NULL_PTR(view->text_bar)) return;
  GtkWidget *base = dt_ui_center_base(dt_gui_get_ui());
  view->bars_position_handler = g_signal_connect(base, "get-child-position", G_CALLBACK(_bars_child_position), self);

  view->text_bar = _bar_new(base);
  view->text_font = gtk_font_button_new();
  gtk_font_button_set_show_size(GTK_FONT_BUTTON(view->text_font), TRUE);
  gtk_widget_set_tooltip_text(view->text_font, _("Font family and size"));
  g_signal_connect(view->text_font, "font-set", G_CALLBACK(_bar_text_font_set), self);
  gtk_box_pack_start(GTK_BOX(view->text_bar), view->text_font, FALSE, FALSE, 0);
  view->text_color = _bar_color_button(view->text_bar, _("Text colour"), G_CALLBACK(_bar_text_color_set), self);
  view->text_background
      = _bar_color_button(view->text_bar, _("Background colour"), G_CALLBACK(_bar_text_color_set), self);
  view->text_no_background = gtk_toggle_button_new_with_label(_("Transparent"));
  gtk_widget_set_tooltip_text(view->text_no_background, _("No background: the canvas shows through"));
  g_signal_connect(view->text_no_background, "toggled", G_CALLBACK(_bar_text_no_background_toggled), self);
  gtk_box_pack_start(GTK_BOX(view->text_bar), view->text_no_background, FALSE, FALSE, 0);
  view->text_align_h = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(view->text_align_h), _("Left"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(view->text_align_h), _("Centred"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(view->text_align_h), _("Right"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(view->text_align_h), _("Justified"));
  gtk_widget_set_tooltip_text(view->text_align_h, _("Horizontal alignment"));
  g_signal_connect(view->text_align_h, "changed", G_CALLBACK(_bar_text_align_changed), self);
  gtk_box_pack_start(GTK_BOX(view->text_bar), view->text_align_h, FALSE, FALSE, 0);
  view->text_align_v = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(view->text_align_v), _("Top"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(view->text_align_v), _("Middle"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(view->text_align_v), _("Bottom"));
  gtk_widget_set_tooltip_text(view->text_align_v, _("Vertical alignment"));
  g_signal_connect(view->text_align_v, "changed", G_CALLBACK(_bar_text_align_changed), self);
  gtk_box_pack_start(GTK_BOX(view->text_bar), view->text_align_v, FALSE, FALSE, 0);
  _bar_add_border_controls(self, view->text_bar, &view->text_border_width, &view->text_border_color);
  _bar_finish(view->text_bar);

  view->image_bar = _bar_new(base);
  _bar_add_border_controls(self, view->image_bar, &view->image_border_width, &view->image_border_color);
  _bar_finish(view->image_bar);

  view->connector_bar = _bar_new(base);
  view->connector_route = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(view->connector_route), _("Straight"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(view->connector_route), _("Square"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(view->connector_route), _("Cubic spline"));
  gtk_widget_set_tooltip_text(view->connector_route, _("Route"));
  g_signal_connect(view->connector_route, "changed", G_CALLBACK(_bar_connector_route_changed), self);
  gtk_box_pack_start(GTK_BOX(view->connector_bar), view->connector_route, FALSE, FALSE, 0);
  view->connector_arrows = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(view->connector_arrows), _("Flat line"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(view->connector_arrows), _("Arrow at the end"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(view->connector_arrows), _("Arrow at the start"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(view->connector_arrows), _("Arrows at both ends"));
  gtk_widget_set_tooltip_text(view->connector_arrows, _("Arrow heads"));
  g_signal_connect(view->connector_arrows, "changed", G_CALLBACK(_bar_connector_arrows_changed), self);
  gtk_box_pack_start(GTK_BOX(view->connector_bar), view->connector_arrows, FALSE, FALSE, 0);
  GtkWidget *reverse = gtk_button_new_with_label(_("Reverse"));
  gtk_widget_set_tooltip_text(reverse, _("Swap the start and the end"));
  g_signal_connect(reverse, "clicked", G_CALLBACK(_bar_connector_reverse_clicked), self);
  gtk_box_pack_start(GTK_BOX(view->connector_bar), reverse, FALSE, FALSE, 0);
  view->connector_width = gtk_spin_button_new_with_range(1.0, 40.0, 1.0);
  gtk_widget_set_tooltip_text(view->connector_width, _("Line width, in canvas units"));
  g_signal_connect(view->connector_width, "value-changed", G_CALLBACK(_bar_connector_width_changed), self);
  gtk_box_pack_start(GTK_BOX(view->connector_bar), view->connector_width, FALSE, FALSE, 0);
  view->connector_dashed = gtk_toggle_button_new_with_label(_("Dashed"));
  g_signal_connect(view->connector_dashed, "toggled", G_CALLBACK(_bar_connector_dashed_toggled), self);
  gtk_box_pack_start(GTK_BOX(view->connector_bar), view->connector_dashed, FALSE, FALSE, 0);
  view->connector_color = _bar_color_button(view->connector_bar, _("Colour"), G_CALLBACK(_bar_connector_color_set), self);
  view->connector_via = gtk_toggle_button_new_with_label(_("Waypoint"));
  gtk_widget_set_tooltip_text(view->connector_via, _("Add a point the connector passes by, to go around other frames. Drag it into place."));
  g_signal_connect(view->connector_via, "toggled", G_CALLBACK(_bar_connector_via_toggled), self);
  gtk_box_pack_start(GTK_BOX(view->connector_bar), view->connector_via, FALSE, FALSE, 0);
  _bar_finish(view->connector_bar);
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
  if(!IS_NULL_PTR(view->text_bar)) gtk_container_remove(GTK_CONTAINER(base), view->text_bar);
  if(!IS_NULL_PTR(view->image_bar)) gtk_container_remove(GTK_CONTAINER(base), view->image_bar);
  if(!IS_NULL_PTR(view->connector_bar)) gtk_container_remove(GTK_CONTAINER(base), view->connector_bar);
  if(view->bars_position_handler != 0)
  {
    g_signal_handler_disconnect(base, view->bars_position_handler);
    view->bars_position_handler = 0;
  }
  view->text_bar = NULL;
  view->image_bar = NULL;
  view->connector_bar = NULL;
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

/** Place a bar immediately below the object, above it when there is no room below. */
static void _bar_place(dt_canvas_view_t *view, GtkWidget *bar, const dt_canvas_object_t *object)
{
  double min_x = 0.0;
  double min_y = 0.0;
  double max_x = 0.0;
  double max_y = 0.0;
  if(!_object_screen_box(view, object, &min_x, &min_y, &max_x, &max_y)) return;
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

/** Hide the bars at once, without waiting for the idle: a click on the background dismisses them. */
static void _bars_hide_now(dt_canvas_view_t *view)
{
  if(IS_NULL_PTR(view->text_bar)) return;
  gtk_widget_hide(view->text_bar);
  gtk_widget_hide(view->image_bar);
  gtk_widget_hide(view->connector_bar);
  view->bars_signature = 0;
}

static void _bars_refresh(dt_view_t *self, gboolean force)
{
  dt_canvas_view_t *view = (dt_canvas_view_t *)self->data;
  if(IS_NULL_PTR(view->text_bar) || IS_NULL_PTR(view->image_bar) || IS_NULL_PTR(view->connector_bar)) return;
  // No bar while a gesture is running: it would follow every motion through a re-allocation.
  const gboolean dragging = view->drag != DT_CANVAS_DRAG_NONE;
  const dt_canvas_object_t *object = (view->connecting || dragging) ? NULL : _bar_target(view);
  const uint32_t kind = IS_NULL_PTR(object) ? DT_CANVAS_OBJECT_NONE : object->kind;
  const uint64_t signature = view->canvas->generation * 131u + (IS_NULL_PTR(object) ? 0u : object->id) * 7u + kind;
  if(force || signature != view->bars_signature)
  {
    view->bars_signature = signature;
    view->bars_refilling = TRUE;
    if(kind == DT_CANVAS_OBJECT_TEXT)
    {
      gtk_font_chooser_set_font(GTK_FONT_CHOOSER(view->text_font), dt_canvas_text_effective_font(view->canvas, object));
      _color_to_button(view->text_color, &object->text.text_color);
      _color_to_button(view->text_background, &object->text.background);
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(view->text_no_background), object->text.background.alpha <= 0.0f);
      gtk_combo_box_set_active(GTK_COMBO_BOX(view->text_align_h), CLAMP((int)object->text.align_h, 0, 3));
      gtk_combo_box_set_active(GTK_COMBO_BOX(view->text_align_v), CLAMP((int)object->text.align_v, 0, 2));
      dt_canvas_color_t border_color;
      float border_width = 0.0f;
      dt_canvas_object_effective_border(view->canvas, object, &border_color, &border_width);
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(view->text_border_width), border_width);
      _color_to_button(view->text_border_color, &border_color);
    }
    else if(kind == DT_CANVAS_OBJECT_IMAGE)
    {
      dt_canvas_color_t color;
      float width = 0.0f;
      dt_canvas_object_effective_border(view->canvas, object, &color, &width);
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(view->image_border_width), width);
      _color_to_button(view->image_border_color, &color);
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
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(view->connector_width), object->connector.line_width);
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(view->connector_dashed),
                                   (object->connector.style & DT_CANVAS_CONNECTOR_DASHED) != 0);
      _color_to_button(view->connector_color, &object->connector.color);
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(view->connector_via), object->connector.via_count > 0);
    }
    view->bars_refilling = FALSE;
    gtk_widget_set_visible(view->text_bar, kind == DT_CANVAS_OBJECT_TEXT);
    gtk_widget_set_visible(view->image_bar, kind == DT_CANVAS_OBJECT_IMAGE);
    gtk_widget_set_visible(view->connector_bar, kind == DT_CANVAS_OBJECT_CONNECTOR);
  }
  if(kind == DT_CANVAS_OBJECT_TEXT) _bar_place(view, view->text_bar, object);
  if(kind == DT_CANVAS_OBJECT_IMAGE) _bar_place(view, view->image_bar, object);
  if(kind == DT_CANVAS_OBJECT_CONNECTOR) _bar_place(view, view->connector_bar, object);
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
  if(IS_NULL_PTR(view) || IS_NULL_PTR(view->text_bar) || view->bars_idle != 0) return;
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

static void _paint_badge(cairo_t *cr, const dt_canvas_view_t *view, const dt_canvas_object_t *object)
{
  if(object->kind != DT_CANVAS_OBJECT_IMAGE) return;
  double red = 0.0;
  double green = 0.0;
  double blue = 0.0;
  switch(object->image.sync_status)
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
    if(dt_canvas_object_is_frame(object)) _paint_handles(cr, view, object);
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
  else if(view->canvas->background_style == DT_CANVAS_BACKGROUND_WATERCOLOUR)
  {
    background[0] = 1.0;
    background[1] = 1.0;
    background[2] = 1.0;
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
     || view->drag == DT_CANVAS_DRAG_VIA)
  {
    if(view->drag_moved)
    {
      dt_canvas_touch(view->canvas);
      _record_undo(self, view->drag_snapshot);
      view->drag_snapshot = NULL;
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
  else if(!IS_NULL_PTR(_via_handle_at(view, x, y)))
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
    view->canvas->background_style = (uint32_t)CLAMP(style, 0, 2);
    dt_conf_set_int("canvas/background_style", (int)view->canvas->background_style);
  }
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
