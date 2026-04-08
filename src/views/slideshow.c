/*
    This file is part of darktable,
    Copyright (C) 2013-2016 johannes hanika.
    Copyright (C) 2014, 2019-2021 Aldric Renaudin.
    Copyright (C) 2014 Moritz Lipp.
    Copyright (C) 2014 parafin.
    Copyright (C) 2014-2016 Roman Lebedev.
    Copyright (C) 2014-2017, 2019-2020 Tobias Ellinghaus.
    Copyright (C) 2015 Edouard Gomez.
    Copyright (C) 2015, 2019-2021 Pascal Obry.
    Copyright (C) 2019-2020, 2022-2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2019 Denis Dyakov.
    Copyright (C) 2019 Philippe Weyland.
    Copyright (C) 2020 Hanno Schwalm.
    Copyright (C) 2020 quovadit.
    Copyright (C) 2021-2022 Diederik Ter Rahe.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2024 Alynx Zhou.
    
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

#include "common/darktable.h"
#include "common/collection.h"
#include "common/selection.h"
#include "common/colorspaces.h"
#include "common/debug.h"
#include "common/dtpthread.h"
#include "common/imageio.h"
#include "common/imageio_module.h"
#include "control/conf.h"
#include "control/control.h"
#include "dtgtk/thumbtable.h"

#include "gui/gtk.h"
#include "views/view.h"
#include "views/view_api.h"

#include <gdk/gdkkeysyms.h>
#include <stdint.h>

DT_MODULE(1)

typedef enum dt_slideshow_event_t
{
  S_REQUEST_STEP,
  S_REQUEST_STEP_BACK,
} dt_slideshow_event_t;

typedef enum dt_slideshow_slot_t
{
  S_LEFT      = 0,
  S_CURRENT   = 1,
  S_RIGHT     = 2,
  S_SLOT_LAST = 3
} dt_slideshow_slot_t;

typedef struct _slideshow_buf_t
{
  struct dt_slideshow_cache_t *cache;
  int32_t imgid;
  int32_t rank;
  gboolean invalidated;
} dt_slideshow_buf_t;

typedef struct dt_slideshow_cache_t
{
  cairo_surface_t *surface;
  dt_view_image_surface_fetcher_t fetcher;
} dt_slideshow_cache_t;

typedef struct dt_slideshow_t
{
  int32_t col_count;
  uint32_t width, height;
  GList *incoming_selection;
  GList *playlist;

  // buffers
  dt_slideshow_buf_t buf[S_SLOT_LAST];
  dt_slideshow_cache_t cache[S_SLOT_LAST];

  // state machine stuff for image transitions:
  dt_pthread_mutex_t lock;

  gboolean auto_advance;
  int delay;
  guint auto_advance_timeout;

  // some magic to hide the mouse pointer
  guint mouse_timeout;
} dt_slideshow_t;

// fwd declare state machine mechanics:
static void _step_state(dt_slideshow_t *d, dt_slideshow_event_t event);

static void shift_left(dt_slideshow_t *d)
{
  dt_slideshow_cache_t *tmp_cache = d->buf[S_LEFT].cache;

  for(int k=S_LEFT; k<S_RIGHT; k++)
  {
    d->buf[k].cache       = d->buf[k+1].cache;
    d->buf[k].imgid       = d->buf[k+1].imgid;
    d->buf[k].rank        = d->buf[k+1].rank;
    d->buf[k].invalidated = d->buf[k+1].invalidated;
  }
  d->buf[S_RIGHT].invalidated = TRUE;
  d->buf[S_RIGHT].imgid = UNKNOWN_IMAGE;
  d->buf[S_RIGHT].rank = d->buf[S_CURRENT].rank + 1;
  d->buf[S_RIGHT].cache = tmp_cache;
}

static void shift_right(dt_slideshow_t *d)
{
  dt_slideshow_cache_t *tmp_cache = d->buf[S_RIGHT].cache;

  for(int k=S_RIGHT; k>S_LEFT; k--)
  {
    d->buf[k].cache       = d->buf[k-1].cache;
    d->buf[k].imgid       = d->buf[k-1].imgid;
    d->buf[k].rank        = d->buf[k-1].rank;
    d->buf[k].invalidated = d->buf[k-1].invalidated;
  }
  d->buf[S_LEFT].invalidated = TRUE;
  d->buf[S_LEFT].imgid = UNKNOWN_IMAGE;
  d->buf[S_LEFT].rank = d->buf[S_CURRENT].rank - 1;
  d->buf[S_LEFT].cache = tmp_cache;
}

static void _set_delay(dt_slideshow_t *d, int value)
{
  d->delay = CLAMP(d->delay + value, 1, 60);
  dt_conf_set_int("slideshow_delay", d->delay);
}

static int32_t _slideshow_get_imgid_from_rank(const dt_slideshow_t *d, const int32_t rank)
{
  if(rank < 0) return UNKNOWN_IMAGE;

  if(d->playlist)
  {
    const GList *link = g_list_nth(d->playlist, rank);
    return link ? GPOINTER_TO_INT(link->data) : UNKNOWN_IMAGE;
  }

  const gchar *query = dt_collection_get_query(darktable.collection);
  if(!query) return UNKNOWN_IMAGE;

  int32_t id = 0;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, rank);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, 1);
  if(sqlite3_step(stmt) == SQLITE_ROW) id = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return id;
}

static dt_view_surface_value_t _slideshow_request_slot(dt_slideshow_t *d, const dt_slideshow_slot_t slot)
{
  dt_slideshow_buf_t *buffer = &d->buf[slot];
  if(buffer->rank < 0 || buffer->rank >= d->col_count)
  {
    buffer->imgid = UNKNOWN_IMAGE;
    buffer->invalidated = FALSE;
    dt_view_image_surface_fetcher_invalidate(&buffer->cache->fetcher, &buffer->cache->surface);
    return DT_VIEW_SURFACE_KO;
  }

  const int32_t imgid = _slideshow_get_imgid_from_rank(d, buffer->rank);
  if(imgid <= UNKNOWN_IMAGE)
  {
    buffer->imgid = UNKNOWN_IMAGE;
    buffer->invalidated = TRUE;
    dt_view_image_surface_fetcher_invalidate(&buffer->cache->fetcher, &buffer->cache->surface);
    return DT_VIEW_SURFACE_KO;
  }

  buffer->imgid = imgid;
  const int width = MAX(2, (int)d->width);
  const int height = MAX(2, (int)d->height);
  const dt_view_surface_value_t res =
      dt_view_image_get_surface_async(&buffer->cache->fetcher, imgid, width, height, &buffer->cache->surface,
                                      dt_ui_center(darktable.gui->ui), DT_THUMBTABLE_ZOOM_FIT);
  buffer->invalidated = (res != DT_VIEW_SURFACE_OK);
  return res;
}

static gboolean auto_advance(gpointer user_data)
{
  dt_slideshow_t *d = (dt_slideshow_t *)user_data;
  d->auto_advance_timeout = 0;
  if(!d->auto_advance) return FALSE;

  dt_pthread_mutex_lock(&d->lock);
  _slideshow_request_slot(d, S_CURRENT);
  _slideshow_request_slot(d, S_LEFT);
  _slideshow_request_slot(d, S_RIGHT);
  const gboolean can_advance = d->buf[S_RIGHT].rank >= 0 && d->buf[S_RIGHT].rank < d->col_count
                               && !d->buf[S_RIGHT].invalidated && d->buf[S_RIGHT].cache->surface;
  const gboolean at_end = d->buf[S_CURRENT].rank >= d->col_count - 1;
  dt_pthread_mutex_unlock(&d->lock);

  if(can_advance || at_end)
  {
    _step_state(d, S_REQUEST_STEP);
    return FALSE;
  }

  /* Autoplay waits for the next slot surface instead of stepping onto an
   * empty screen. Keep the right-hand prefetch alive and poll shortly until it
   * lands, then the normal delayed cadence resumes from _step_state(). */
  dt_control_queue_redraw_center();
  d->auto_advance_timeout = g_timeout_add(200, auto_advance, d);
  return FALSE;
}

static void _refresh_display(dt_slideshow_t *d)
{
  _slideshow_request_slot(d, S_CURRENT);
  _slideshow_request_slot(d, S_LEFT);
  _slideshow_request_slot(d, S_RIGHT);
  dt_control_queue_redraw_center();
}

// state machine stepping
static void _step_state(dt_slideshow_t *d, dt_slideshow_event_t event)
{
  dt_pthread_mutex_lock(&d->lock);

  if(event == S_REQUEST_STEP)
  {
    if(d->buf[S_CURRENT].rank < d->col_count - 1)
    {
      shift_left(d);
      if(!d->playlist)
      {
        d->buf[S_CURRENT].imgid = _slideshow_get_imgid_from_rank(d, d->buf[S_CURRENT].rank);
        dt_view_active_images_reset(FALSE);
        if(d->buf[S_CURRENT].imgid > UNKNOWN_IMAGE) dt_view_active_images_add(d->buf[S_CURRENT].imgid, FALSE);
      }
      d->buf[S_RIGHT].rank = d->buf[S_CURRENT].rank + 1;
      d->buf[S_RIGHT].invalidated = d->buf[S_RIGHT].rank < d->col_count;
      _refresh_display(d);
    }
    else
    {
      dt_control_log(_("end of images"));
      d->auto_advance = FALSE;
    }
  }
  else if(event == S_REQUEST_STEP_BACK)
  {
    if(d->buf[S_CURRENT].rank > 0)
    {
      shift_right(d);
      if(!d->playlist)
      {
        d->buf[S_CURRENT].imgid = _slideshow_get_imgid_from_rank(d, d->buf[S_CURRENT].rank);
        dt_view_active_images_reset(FALSE);
        if(d->buf[S_CURRENT].imgid > UNKNOWN_IMAGE) dt_view_active_images_add(d->buf[S_CURRENT].imgid, FALSE);
      }
      d->buf[S_LEFT].rank = d->buf[S_CURRENT].rank - 1;
      d->buf[S_LEFT].invalidated = d->buf[S_LEFT].rank >= 0;
      _refresh_display(d);
    }
    else
    {
      dt_control_log(_("end of images. press any key to return to lighttable mode"));
      d->auto_advance = FALSE;
    }
  }

  dt_pthread_mutex_unlock(&d->lock);

  if(d->auto_advance)
  {
    if(d->auto_advance_timeout > 0) g_source_remove(d->auto_advance_timeout);
    d->auto_advance_timeout = g_timeout_add_seconds(d->delay, auto_advance, d);
  }
}

// callbacks for a view module:

const char *name(const dt_view_t *self)
{
  return _("slideshow");
}

uint32_t view(const dt_view_t *self)
{
  return DT_VIEW_SLIDESHOW;
}

void init(dt_view_t *self)
{
  self->data = calloc(1, sizeof(dt_slideshow_t));
  dt_slideshow_t *lib = (dt_slideshow_t *)self->data;
  dt_pthread_mutex_init(&lib->lock, 0);
  for(int k = S_LEFT; k < S_SLOT_LAST; k++)
  {
    lib->buf[k].cache = &lib->cache[k];
    lib->buf[k].imgid = UNKNOWN_IMAGE;
    lib->buf[k].rank = -1;
    lib->buf[k].invalidated = TRUE;
    dt_view_image_surface_fetcher_init(&lib->cache[k].fetcher);
  }
}


void cleanup(dt_view_t *self)
{
  dt_slideshow_t *lib = (dt_slideshow_t *)self->data;
  if(lib->mouse_timeout > 0) g_source_remove(lib->mouse_timeout);
  if(lib->auto_advance_timeout > 0) g_source_remove(lib->auto_advance_timeout);
  g_list_free(lib->incoming_selection);
  g_list_free(lib->playlist);
  for(int k = S_LEFT; k < S_SLOT_LAST; k++)
    dt_view_image_surface_fetcher_cleanup(&lib->cache[k].fetcher);
  dt_pthread_mutex_destroy(&lib->lock);
  dt_free(self->data);
}

int try_enter(dt_view_t *self)
{
  dt_slideshow_t *d = (dt_slideshow_t *)self->data;
  g_list_free(d->incoming_selection);
  d->incoming_selection = dt_selection_get_list(darktable.selection);
  if(!d->incoming_selection && dt_view_active_images_get_all())
    d->incoming_selection = g_list_copy((GList *)dt_view_active_images_get_all());

  if(d->incoming_selection || dt_collection_get_count(darktable.collection) != 0) return 0;

  dt_control_log(_("there are no images in this collection"));
  return 1;
}

void enter(dt_view_t *self)
{
  dt_slideshow_t *d = (dt_slideshow_t *)self->data;

  dt_control_change_cursor(GDK_BLANK_CURSOR);
  d->mouse_timeout = 0;
  d->auto_advance_timeout = 0;

  dt_accels_connect_accels(darktable.gui->accels);
  dt_accels_connect_active_group(darktable.gui->accels, "slideshow");

  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_LEFT, FALSE, TRUE);
  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_RIGHT, FALSE, TRUE);
  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_TOP, FALSE, TRUE);
  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_BOTTOM, FALSE, TRUE);

  // also hide arrows
  dt_control_queue_redraw();

  GtkWidget *window = dt_ui_main_window(darktable.gui->ui);
  GdkRectangle rect;

  GdkDisplay *display = gtk_widget_get_display(window);
  GdkMonitor *mon = gdk_display_get_monitor_at_window(display, gtk_widget_get_window(window));
  gdk_monitor_get_geometry(mon, &rect);

  dt_pthread_mutex_lock(&d->lock);

  d->width = rect.width;
  d->height = rect.height;

  dt_view_active_images_reset(FALSE);
  g_list_free(d->playlist);
  d->playlist = NULL;
  if(d->incoming_selection)
  {
    /* Slideshow keeps either the incoming selection list or a single current
     * image as its working set while the global selection stays cleared. */
    dt_view_active_images_set(d->incoming_selection, FALSE);
    d->incoming_selection = NULL;
    d->playlist = g_list_copy(dt_view_active_images_get_all());
  }

  d->col_count = d->playlist ? g_list_length(d->playlist) : dt_collection_get_count(darktable.collection);

  for(int k = S_LEFT; k < S_SLOT_LAST; k++)
  {
    dt_view_image_surface_fetcher_invalidate(&d->cache[k].fetcher, &d->cache[k].surface);
    d->buf[k].imgid = UNKNOWN_IMAGE;
    d->buf[k].rank = -1;
    d->buf[k].invalidated = TRUE;
  }

  if(d->playlist)
  {
    d->buf[S_CURRENT].rank = 0;
    d->buf[S_CURRENT].imgid = GPOINTER_TO_INT(d->playlist->data);
  }
  else if(d->col_count > 0)
  {
    d->buf[S_CURRENT].rank = 0;
    d->buf[S_CURRENT].imgid = _slideshow_get_imgid_from_rank(d, 0);
    if(d->buf[S_CURRENT].imgid > UNKNOWN_IMAGE)
    {
      dt_view_active_images_add(d->buf[S_CURRENT].imgid, FALSE);
    }
  }

  d->buf[S_LEFT].rank = d->buf[S_CURRENT].rank - 1;
  d->buf[S_RIGHT].rank = d->buf[S_CURRENT].rank + 1;

  d->auto_advance = FALSE;
  d->delay = dt_conf_get_int("slideshow_delay");
  dt_pthread_mutex_unlock(&d->lock);

  dt_selection_clear(darktable.selection);

  dt_gui_refocus_center();
  _refresh_display(d);
  dt_control_log(_("waiting to start slideshow"));
}

void leave(dt_view_t *self)
{
  dt_slideshow_t *d = (dt_slideshow_t *)self->data;

  if(d->mouse_timeout > 0) g_source_remove(d->mouse_timeout);
  d->mouse_timeout = 0;
  if(d->auto_advance_timeout > 0) g_source_remove(d->auto_advance_timeout);
  d->auto_advance_timeout = 0;
  dt_control_change_cursor(GDK_LEFT_PTR);
  d->auto_advance = FALSE;
  dt_accels_disconnect_active_group(darktable.gui->accels);

  dt_selection_clear(darktable.selection);
  if(dt_view_active_images_get_all())
    dt_selection_select_list(darktable.selection, dt_view_active_images_get_all());
  dt_view_active_images_reset(FALSE);
  g_list_free(d->incoming_selection);
  d->incoming_selection = NULL;
  g_list_free(d->playlist);
  d->playlist = NULL;
  for(int k = S_LEFT; k < S_SLOT_LAST; k++)
  {
    dt_view_image_surface_fetcher_invalidate(&d->cache[k].fetcher, &d->cache[k].surface);
    d->buf[k].imgid = UNKNOWN_IMAGE;
    d->buf[k].rank = -1;
    d->buf[k].invalidated = TRUE;
  }
}

void expose(dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  // draw front buffer.
  dt_slideshow_t *d = (dt_slideshow_t *)self->data;

  dt_pthread_mutex_lock(&d->lock);
  cairo_paint(cr);
  d->width = width;
  d->height = height;
  _slideshow_request_slot(d, S_CURRENT);
  _slideshow_request_slot(d, S_LEFT);
  _slideshow_request_slot(d, S_RIGHT);

  const dt_slideshow_buf_t *slot = &(d->buf[S_CURRENT]);
  cairo_surface_t *surface = slot->cache->surface;
  if(surface && slot->rank >= 0 && !slot->invalidated)
  {
    const int surface_width = cairo_image_surface_get_width(surface);
    const int surface_height = cairo_image_surface_get_height(surface);
    double sx = 1.0, sy = 1.0;
    cairo_surface_get_device_scale(surface, &sx, &sy);
    const double logical_width = surface_width / sx;
    const double logical_height = surface_height / sy;
    // cope with possible resize of the window
    const float tr_width = (width < logical_width) ? 0.f : (width - logical_width) * .5f;
    const float tr_height = (height < logical_height) ? 0.f : (height - logical_height) * .5f;

    cairo_save(cr);
    cairo_translate(cr, tr_width, tr_height);
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), darktable.gui->filter_image);
    cairo_rectangle(cr, 0, 0, logical_width, logical_height);
    cairo_fill(cr);
    cairo_restore(cr);
  }
  else
  {
    dt_control_draw_busy_msg(cr, width, height);
  }
  dt_pthread_mutex_unlock(&d->lock);
}

int key_pressed(dt_view_t *self, GdkEventKey *event)
{
  if(!gtk_window_is_active(GTK_WINDOW(darktable.gui->ui->main_window))) return FALSE;
  
  switch(event->keyval)
  {
    case GDK_KEY_Escape:
    {
      dt_ctl_switch_mode_to("lighttable");
      return TRUE;
    }
  }

  return 0;
}

/**
 * @brief Toggle slideshow auto-advance using the same stepping code path as
 * manual navigation, so pause/resume stays consistent with the current buffer
 * state.
 */
static gboolean _slideshow_start_stop_accel(GtkAccelGroup *accel_group, GObject *accelerable, guint keyval,
                                            GdkModifierType mods, gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  dt_slideshow_t *d = (dt_slideshow_t *)self->data;
  if(!d->auto_advance)
  {
    d->auto_advance = TRUE;
    if(d->auto_advance_timeout > 0) g_source_remove(d->auto_advance_timeout);
    d->auto_advance_timeout = g_timeout_add_seconds(d->delay, auto_advance, d);
    dt_control_log(_("slideshow started"));
  }
  else
  {
    d->auto_advance = FALSE;
    if(d->auto_advance_timeout > 0) g_source_remove(d->auto_advance_timeout);
    d->auto_advance_timeout = 0;
    dt_control_log(_("slideshow paused"));
  }
  return TRUE;
}

/**
 * @brief Increase the slideshow delay and store it immediately so repeated
 * keyboard adjustments persist across view changes.
 */
static gboolean _slideshow_slow_down_accel(GtkAccelGroup *accel_group, GObject *accelerable, guint keyval,
                                           GdkModifierType mods, gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  dt_slideshow_t *d = (dt_slideshow_t *)self->data;
  _set_delay(d, 1);
  dt_control_log(ngettext("slideshow delay set to %d second", "slideshow delay set to %d seconds", d->delay),
                 d->delay);
  return TRUE;
}

/**
 * @brief Decrease the slideshow delay and store it immediately so the current
 * playback cadence matches the persisted preference.
 */
static gboolean _slideshow_speed_up_accel(GtkAccelGroup *accel_group, GObject *accelerable, guint keyval,
                                          GdkModifierType mods, gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  dt_slideshow_t *d = (dt_slideshow_t *)self->data;
  _set_delay(d, -1);
  dt_control_log(ngettext("slideshow delay set to %d second", "slideshow delay set to %d seconds", d->delay),
                 d->delay);
  return TRUE;
}

/**
 * @brief Advance the slideshow while keeping auto-advance and global actions
 * synced with the image currently displayed fullscreen.
 */
static gboolean _slideshow_step_forward_accel(GtkAccelGroup *accel_group, GObject *accelerable, guint keyval,
                                              GdkModifierType mods, gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  dt_slideshow_t *d = (dt_slideshow_t *)self->data;
  if(d->auto_advance) dt_control_log(_("slideshow paused"));
  d->auto_advance = FALSE;
  if(d->auto_advance_timeout > 0) g_source_remove(d->auto_advance_timeout);
  d->auto_advance_timeout = 0;
  _step_state(d, S_REQUEST_STEP);
  return TRUE;
}

/**
 * @brief Step back in slideshow mode and keep the fullscreen image as the sole
 * active image when the selection is temporarily cleared.
 */
static gboolean _slideshow_step_back_accel(GtkAccelGroup *accel_group, GObject *accelerable, guint keyval,
                                           GdkModifierType mods, gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  dt_slideshow_t *d = (dt_slideshow_t *)self->data;
  if(d->auto_advance) dt_control_log(_("slideshow paused"));
  d->auto_advance = FALSE;
  if(d->auto_advance_timeout > 0) g_source_remove(d->auto_advance_timeout);
  d->auto_advance_timeout = 0;
  _step_state(d, S_REQUEST_STEP_BACK);
  return TRUE;
}

static gboolean _hide_mouse(gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  dt_slideshow_t *d = (dt_slideshow_t *)self->data;
  d->mouse_timeout = 0;
  dt_control_change_cursor(GDK_BLANK_CURSOR);
  return FALSE;
}


void mouse_moved(dt_view_t *self, double x, double y, double pressure, int which)
{
  dt_slideshow_t *d = (dt_slideshow_t *)self->data;

  if(d->mouse_timeout > 0) g_source_remove(d->mouse_timeout);
  else dt_control_change_cursor(GDK_LEFT_PTR);
  d->mouse_timeout = g_timeout_add_seconds(1, _hide_mouse, self);
}


int button_released(dt_view_t *self, double x, double y, int which, uint32_t state)
{
  return 0;
}


int button_pressed(dt_view_t *self, double x, double y, double pressure, int which, int type, uint32_t state)
{
  dt_slideshow_t *d = (dt_slideshow_t *)self->data;

  if(which == 1)
    _step_state(d, S_REQUEST_STEP);
  else if(which == 3)
    _step_state(d, S_REQUEST_STEP_BACK);
  else
    return 1;

  return 0;
}

void gui_init(dt_view_t *self)
{
  dt_accels_new_slideshow_action(_slideshow_start_stop_accel, self, N_("Slideshow/Actions"),
                                 N_("Start and stop"), GDK_KEY_space, 0,
                                 _("Toggles slideshow auto-advance"));
  dt_accels_new_slideshow_action(_slideshow_slow_down_accel, self, N_("Slideshow/Actions"),
                                 N_("Slow down"), GDK_KEY_Up, 0,
                                 _("Increases the slideshow delay"));
  dt_accels_new_slideshow_action(_slideshow_slow_down_accel, self, N_("Slideshow/Actions"),
                                 N_("Slow down"), GDK_KEY_KP_Add, 0,
                                 _("Increases the slideshow delay"));
  dt_accels_new_slideshow_action(_slideshow_slow_down_accel, self, N_("Slideshow/Actions"),
                                 N_("Slow down"), GDK_KEY_plus, 0,
                                 _("Increases the slideshow delay"));
  dt_accels_new_slideshow_action(_slideshow_speed_up_accel, self, N_("Slideshow/Actions"),
                                 N_("Speed up"), GDK_KEY_Down, 0,
                                 _("Decreases the slideshow delay"));
  dt_accels_new_slideshow_action(_slideshow_speed_up_accel, self, N_("Slideshow/Actions"),
                                 N_("Speed up"), GDK_KEY_KP_Subtract, 0,
                                 _("Decreases the slideshow delay"));
  dt_accels_new_slideshow_action(_slideshow_speed_up_accel, self, N_("Slideshow/Actions"),
                                 N_("Speed up"), GDK_KEY_minus, 0,
                                 _("Decreases the slideshow delay"));
  dt_accels_new_slideshow_action(_slideshow_step_back_accel, self, N_("Slideshow/Actions"),
                                 N_("Step back"), GDK_KEY_Left, 0,
                                 _("Displays the previous image"));
  dt_accels_new_slideshow_action(_slideshow_step_forward_accel, self, N_("Slideshow/Actions"),
                                 N_("Step forward"), GDK_KEY_Right, 0,
                                 _("Displays the next image"));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
