/*
    This file is part of darktable,
    Copyright (C) 2009-2022 darktable developers.

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
#include "common/colorspaces.h"
#include "common/l10n.h"
#include "common/file_location.h"
#include "common/ratings.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "gui/guides.h"
#include "bauhaus/bauhaus.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "dtgtk/button.h"
#include "dtgtk/expander.h"
#include "dtgtk/sidepanel.h"

#include "gui/gtk.h"
#include "gui/splash.h"

#include "common/styles.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "control/signal.h"
#include "gui/presets.h"
#include "views/view.h"

#include <gdk/gdkkeysyms.h>
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#endif
#include <gtk/gtk.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef MAC_INTEGRATION
#include <gtkosxapplication.h>
#endif
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif
#include <pthread.h>

/*
 * NEW UI API
 */

/* generic callback for redraw widget signals */
static void _ui_widget_redraw_callback(gpointer instance, GtkWidget *widget);
/* callback for redraw log signals */
static void _ui_log_redraw_callback(gpointer instance, GtkWidget *widget);
static void _ui_toast_redraw_callback(gpointer instance, GtkWidget *widget);

// set class function to add CSS classes with just a simple line call
void dt_gui_add_class(GtkWidget *widget, const gchar *class_name)
{
  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  if(!gtk_style_context_has_class(context, class_name))
  {
    gtk_style_context_add_class(context, class_name);
    gtk_widget_queue_draw(widget);
  }
}

void dt_gui_remove_class(GtkWidget *widget, const gchar *class_name)
{
  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  if(gtk_style_context_has_class(context, class_name))
  {
    gtk_style_context_remove_class(context, class_name);
    gtk_widget_queue_draw(widget);
  }
}

/*
 * OLD UI API
 */
static void _init_widgets(dt_gui_gtk_t *gui);

gboolean dt_gui_get_scroll_deltas(const GdkEventScroll *event, gdouble *delta_x, gdouble *delta_y)
{
  // avoid double counting real and emulated events when receiving smooth scrolls
  if(gdk_event_get_pointer_emulated((GdkEvent*)event)) return FALSE;

  gboolean handled = FALSE;
  switch(event->direction)
  {
    // is one-unit cardinal, e.g. from a mouse scroll wheel
    case GDK_SCROLL_LEFT:
      if(delta_x)
      {
        *delta_x = dt_conf_get_bool("scroll/reverse_x") ? 1.0 : -1.0;
        if(delta_y) *delta_y = 0.0;
        handled = TRUE;
      }
      break;
    case GDK_SCROLL_RIGHT:
      if(delta_x)
      {
        *delta_x = dt_conf_get_bool("scroll/reverse_x") ? -1.0 : 1.0;
        if(delta_y) *delta_y = 0.0;
        handled = TRUE;
      }
      break;
    case GDK_SCROLL_UP:
      if(delta_y)
      {
        if(delta_x) *delta_x = 0.0;
        *delta_y = dt_conf_get_bool("scroll/reverse_y") ? 1.0 : -1.0;
        handled = TRUE;
      }
      break;
    case GDK_SCROLL_DOWN:
      if(delta_y)
      {
        if(delta_x) *delta_x = 0.0;
        *delta_y = dt_conf_get_bool("scroll/reverse_y") ? -1.0 : 1.0;
        handled = TRUE;
      }
      break;
    // is trackpad (or touch) scroll
    case GDK_SCROLL_SMOOTH:
      if((delta_x && event->delta_x != 0) || (delta_y && event->delta_y != 0))
      {
#ifdef GDK_WINDOWING_QUARTZ // on macOS deltas need to be scaled
        if(delta_x) *delta_x = dt_conf_get_bool("scroll/reverse_x") ? -event->delta_x / 50. : event->delta_x / 50.;
        if(delta_y) *delta_y = dt_conf_get_bool("scroll/reverse_y") ? -event->delta_y / 50. : event->delta_y / 50.;
#else
        if(delta_x) *delta_x = dt_conf_get_bool("scroll/reverse_x") ? -event->delta_x : event->delta_x;
        if(delta_y) *delta_y = dt_conf_get_bool("scroll/reverse_y") ? -event->delta_y : event->delta_y;
#endif
        handled = TRUE;
      }
    default:
      break;
    }
  return handled;
}

gboolean dt_gui_get_scroll_unit_deltas(const GdkEventScroll *event, int *delta_x, int *delta_y)
{
  // avoid double counting real and emulated events when receiving smooth scrolls
  if(gdk_event_get_pointer_emulated((GdkEvent*)event)) return FALSE;

  // accumulates scrolling regardless of source or the widget being scrolled
  static gdouble acc_x = 0.0, acc_y = 0.0;

  gboolean handled = FALSE;

  switch(event->direction)
  {
    // is one-unit cardinal, e.g. from a mouse scroll wheel
    case GDK_SCROLL_LEFT:
      if(delta_x)
      {
        *delta_x = dt_conf_get_bool("scroll/reverse_x") ? 1 : -1;
        if(delta_y) *delta_y = 0;
        handled = TRUE;
      }
      break;
    case GDK_SCROLL_RIGHT:
      if(delta_x)
      {
        *delta_x = dt_conf_get_bool("scroll/reverse_x") ? -1 : 1;
        if(delta_y) *delta_y = 0;
        handled = TRUE;
      }
      break;
    case GDK_SCROLL_UP:
      if(delta_y)
      {
        if(delta_x) *delta_x = 0;
        *delta_y = dt_conf_get_bool("scroll/reverse_y") ? 1 : -1;
        handled = TRUE;
      }
      break;
    case GDK_SCROLL_DOWN:
      if(delta_y)
      {
        if(delta_x) *delta_x = 0;
        *delta_y = dt_conf_get_bool("scroll/reverse_y") ? -1 : 1;
        handled = TRUE;
      }
      break;
    // is trackpad (or touch) scroll
    case GDK_SCROLL_SMOOTH:
      // stop events reset accumulated delta
      if(event->is_stop)
      {
        acc_x = acc_y = 0.0;
        break;
      }
      // accumulate trackpad/touch scrolls until they make a unit
      // scroll, and only then tell caller that there is a scroll to
      // handle
#ifdef GDK_WINDOWING_QUARTZ // on macOS deltas need to be scaled
      acc_x += dt_conf_get_bool("scroll/reverse_x") ? -event->delta_x / 50. : event->delta_x / 50.;
      acc_y += dt_conf_get_bool("scroll/reverse_y") ? -event->delta_y / 50. : event->delta_y / 50.;
#else
      acc_x += dt_conf_get_bool("scroll/reverse_x") ? -event->delta_x : event->delta_x;
      acc_y += dt_conf_get_bool("scroll/reverse_y") ? -event->delta_y : event->delta_y;
#endif
      const gdouble amt_x = trunc(acc_x);
      const gdouble amt_y = trunc(acc_y);
      if(amt_x != 0 || amt_y != 0)
      {
        acc_x -= amt_x;
        acc_y -= amt_y;
        if((delta_x && amt_x != 0) || (delta_y && amt_y != 0))
        {
          if(delta_x) *delta_x = (int)amt_x;
          if(delta_y) *delta_y = (int)amt_y;
          handled = TRUE;
        }
      }
      break;
    default:
      break;
  }
  return handled;
}

gboolean dt_gui_get_scroll_delta(const GdkEventScroll *event, gdouble *delta)
{
  gdouble delta_x, delta_y;
  if(dt_gui_get_scroll_deltas(event, &delta_x, &delta_y))
  {
    *delta = delta_x + delta_y;
    return TRUE;
  }
  return FALSE;
}

gboolean dt_gui_get_scroll_unit_delta(const GdkEventScroll *event, int *delta)
{
  int delta_x, delta_y;
  if(dt_gui_get_scroll_unit_deltas(event, &delta_x, &delta_y))
  {
    *delta = delta_x + delta_y;
    return TRUE;
  }
  return FALSE;
}


static gboolean _draw(GtkWidget *da, cairo_t *cr, gpointer user_data)
{
  dt_control_expose(NULL);
  if(darktable.gui->surface)
  {
    cairo_set_source_surface(cr, darktable.gui->surface, 0, 0);
    cairo_paint(cr);
  }

  return TRUE;
}

static gboolean _scrolled(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  if(!gtk_window_is_active(GTK_WINDOW(darktable.gui->ui->main_window))) return FALSE;

  int delta_y;
  if(dt_gui_get_scroll_unit_delta(event, &delta_y))
  {
    return dt_view_manager_scrolled(darktable.view_manager, event->x, event->y,
                                    delta_y < 0,
                                    event->state, delta_y);
  }

  return FALSE;
}


int dt_gui_gtk_write_config()
{
  dt_pthread_mutex_lock(&darktable.gui->mutex);
  GtkWidget *widget = dt_ui_main_window(darktable.gui->ui);
  dt_conf_set_bool("ui_last/maximized",
                   (gdk_window_get_state(gtk_widget_get_window(widget)) & GDK_WINDOW_STATE_MAXIMIZED));
  int width, height;
  gtk_window_get_size(GTK_WINDOW(widget), &width, &height);
  dt_conf_set_int("ui_last/window_width", width);
  dt_conf_set_int("ui_last/window_height", height);
  dt_pthread_mutex_unlock(&darktable.gui->mutex);

  return 0;
}

void dt_gui_gtk_set_source_rgb(cairo_t *cr, dt_gui_color_t color)
{
  const GdkRGBA bc = darktable.gui->colors[color];
  cairo_set_source_rgb(cr, bc.red, bc.green, bc.blue);
}

void dt_gui_gtk_set_source_rgba(cairo_t *cr, dt_gui_color_t color, float opacity_coef)
{
  GdkRGBA bc = darktable.gui->colors[color];
  cairo_set_source_rgba(cr, bc.red, bc.green, bc.blue, bc.alpha * opacity_coef);
}

void dt_gui_gtk_quit()
{
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  dt_gui_add_class(win, "dt_gui_quit");
  gtk_window_set_title(GTK_WINDOW(win), _("closing Ansel..."));

  g_list_free(darktable.gui->input_devices);
  dt_ui_cleanup_titlebar(darktable.gui->ui);

  // Write out windows dimension
  dt_gui_gtk_write_config();

  // hide main window
  gtk_widget_hide(dt_ui_main_window(darktable.gui->ui));
}

gboolean dt_gui_quit_callback(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  dt_control_quit();
  return TRUE;
}

void dt_gui_store_last_preset(const char *name)
{
  g_free(darktable.gui->last_preset);
  darktable.gui->last_preset = g_strdup(name);
}

#ifdef MAC_INTEGRATION
#ifdef GTK_TYPE_OSX_APPLICATION
static gboolean _osx_quit_callback(GtkOSXApplication *OSXapp, gpointer user_data)
#else
static gboolean _osx_quit_callback(GtkosxApplication *OSXapp, gpointer user_data)
#endif
{
  GList *windows, *window;
  windows = gtk_window_list_toplevels();
  for(window = windows; window != NULL; window = g_list_next(window))
    if(gtk_window_get_modal(GTK_WINDOW(window->data)) && gtk_widget_get_visible(GTK_WIDGET(window->data)))
      break;
  if(window == NULL) dt_control_quit();
  g_list_free(windows);
  return TRUE;
}

#ifdef GTK_TYPE_OSX_APPLICATION
static gboolean _osx_openfile_callback(GtkOSXApplication *OSXapp, gchar *path, gpointer user_data)
#else
static gboolean _osx_openfile_callback(GtkosxApplication *OSXapp, gchar *path, gpointer user_data)
#endif
{
  return dt_load_from_string(path, TRUE, NULL) > 0;
}
#endif

static gboolean _configure(GtkWidget *da, GdkEventConfigure *event, gpointer user_data)
{
  static int oldw = 0;
  static int oldh = 0;
  // make our selves a properly sized pixmap if our window has been resized
  if(oldw != event->width || oldh != event->height)
  {
    // create our new pixmap with the correct size.
    cairo_surface_t *tmpsurface
        = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, event->width, event->height);
    // copy the contents of the old pixmap to the new pixmap.  This keeps ugly uninitialized
    // pixmaps from being painted upon resize
    //     int minw = oldw, minh = oldh;
    //     if(event->width  < minw) minw = event->width;
    //     if(event->height < minh) minh = event->height;

    cairo_t *cr = cairo_create(tmpsurface);
    cairo_set_source_surface(cr, darktable.gui->surface, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);

    // we're done with our old pixmap, so we can get rid of it and replace it with our properly-sized one.
    cairo_surface_destroy(darktable.gui->surface);
    darktable.gui->surface = tmpsurface;
    dt_colorspaces_set_display_profile(
        DT_COLORSPACE_DISPLAY); // maybe we are on another screen now with > 50% of the area
  }
  oldw = event->width;
  oldh = event->height;

#ifndef GDK_WINDOWING_QUARTZ
  dt_configure_ppd_dpi((dt_gui_gtk_t *) user_data);
#endif

  return dt_control_configure(da, event, user_data);
}

static gboolean _window_configure(GtkWidget *da, GdkEvent *event, gpointer user_data)
{
  static int oldx = 0;
  static int oldy = 0;
  if(oldx != event->configure.x || oldy != event->configure.y)
  {
    dt_colorspaces_set_display_profile(
        DT_COLORSPACE_DISPLAY); // maybe we are on another screen now with > 50% of the area
    oldx = event->configure.x;
    oldy = event->configure.y;
  }
  return FALSE;
}

static gboolean _button_pressed(GtkWidget *w, GdkEventButton *event, gpointer user_data)
{
  if(!gtk_window_is_active(GTK_WINDOW(darktable.gui->ui->main_window))) return FALSE;

  /* Reset Gtk focus */
  darktable.gui->has_scroll_focus = NULL;
  gtk_widget_grab_focus(w);

  double pressure = 1.0;
  GdkDevice *device = gdk_event_get_source_device((GdkEvent *)event);

  if(device && (gdk_device_get_axes(device) & GDK_AXIS_FLAG_PRESSURE))
  {
    gdk_event_get_axis((GdkEvent *)event, GDK_AXIS_PRESSURE, &pressure);
  }
  dt_control_button_pressed(event->x, event->y, pressure, event->button, event->type, event->state & 0xf);
  return FALSE;
}

static gboolean _button_released(GtkWidget *w, GdkEventButton *event, gpointer user_data)
{
  if(!gtk_window_is_active(GTK_WINDOW(darktable.gui->ui->main_window))) return FALSE;
  dt_control_button_released(event->x, event->y, event->button, event->state & 0xf);
  return TRUE;
}

static gboolean _mouse_moved(GtkWidget *w, GdkEventMotion *event, gpointer user_data)
{
  if(!gtk_window_is_active(GTK_WINDOW(darktable.gui->ui->main_window))) return FALSE;

  double pressure = 1.0;
  GdkDevice *device = gdk_event_get_source_device((GdkEvent *)event);

  if(device && (gdk_device_get_axes(device) & GDK_AXIS_FLAG_PRESSURE))
  {
    gdk_event_get_axis((GdkEvent *)event, GDK_AXIS_PRESSURE, &pressure);
  }
  dt_control_mouse_moved(event->x, event->y, pressure, event->state & 0xf);
  return FALSE;
}

static gboolean _key_pressed(GtkWidget *w, GdkEventKey *event)
{
  if(!gtk_window_is_active(GTK_WINDOW(darktable.gui->ui->main_window))) return FALSE;
  dt_control_key_pressed(event);
  return TRUE;
}

static gboolean _center_leave(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_control_mouse_leave();
  return TRUE;
}

static gboolean _center_enter(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_control_mouse_enter();
  return TRUE;
}

static const char* _get_source_name(int pos)
{
  static const gchar *SOURCE_NAMES[]
    = { "GDK_SOURCE_MOUSE",    "GDK_SOURCE_PEN",         "GDK_SOURCE_ERASER",   "GDK_SOURCE_CURSOR",
        "GDK_SOURCE_KEYBOARD", "GDK_SOURCE_TOUCHSCREEN", "GDK_SOURCE_TOUCHPAD", "GDK_SOURCE_TRACKPOINT",
        "GDK_SOURCE_TABLET_PAD" };
  if(pos >= G_N_ELEMENTS(SOURCE_NAMES)) return "<UNKNOWN>";
  return SOURCE_NAMES[pos];
}

static const char* _get_mode_name(int pos)
{
  static const gchar *MODE_NAMES[] = { "GDK_MODE_DISABLED", "GDK_MODE_SCREEN", "GDK_MODE_WINDOW" };
  if(pos >= G_N_ELEMENTS(MODE_NAMES)) return "<UNKNOWN>";
  return MODE_NAMES[pos];
}

static const char* _get_axis_name(int pos)
{
  static const gchar *AXIS_NAMES[]
    = { "GDK_AXIS_IGNORE",   "GDK_AXIS_X",      "GDK_AXIS_Y",     "GDK_AXIS_PRESSURE",
        "GDK_AXIS_XTILT",    "GDK_AXIS_YTILT",  "GDK_AXIS_WHEEL", "GDK_AXIS_DISTANCE",
        "GDK_AXIS_ROTATION", "GDK_AXIS_SLIDER", "GDK_AXIS_LAST" };
  if(pos >= G_N_ELEMENTS(AXIS_NAMES)) return "<UNKNOWN>";
  return AXIS_NAMES[pos];
}

int dt_gui_gtk_init(dt_gui_gtk_t *gui)
{
  /* lets zero mem */
  memset(gui, 0, sizeof(dt_gui_gtk_t));

  dt_pthread_mutex_init(&gui->mutex, NULL);

  // force gtk3 to use normal scroll bars instead of the popup thing. they get in the way of controls
  // the alternative would be to gtk_scrolled_window_set_overlay_scrolling(..., FALSE); every single widget
  // that might have scroll bars
  g_setenv("GTK_OVERLAY_SCROLLING", "0", 0);

  // same for ubuntus overlay-scrollbar-gtk3
  g_setenv("LIBOVERLAY_SCROLLBAR", "0", 0);

  // unset gtk rc from kde:
  char path[PATH_MAX] = { 0 }, datadir[PATH_MAX] = { 0 }, sharedir[PATH_MAX] = { 0 }, configdir[PATH_MAX] = { 0 };
  dt_loc_get_datadir(datadir, sizeof(datadir));
  dt_loc_get_sharedir(sharedir, sizeof(sharedir));
  dt_loc_get_user_config_dir(configdir, sizeof(configdir));

  const char *css_theme = dt_conf_get_string_const("ui_last/theme");
  if(css_theme)
    g_strlcpy(gui->gtkrc, css_theme, sizeof(gui->gtkrc));
  else
    g_snprintf(gui->gtkrc, sizeof(gui->gtkrc), "ansel");

#ifdef MAC_INTEGRATION
#ifdef GTK_TYPE_OSX_APPLICATION
  GtkOSXApplication *OSXApp = g_object_new(GTK_TYPE_OSX_APPLICATION, NULL);
  gtk_osxapplication_set_menu_bar(
      OSXApp, GTK_MENU_SHELL(gtk_menu_bar_new())); // needed for default entries to show up
#else
  GtkosxApplication *OSXApp = g_object_new(GTKOSX_TYPE_APPLICATION, NULL);
  gtkosx_application_set_menu_bar(
      OSXApp, GTK_MENU_SHELL(gtk_menu_bar_new())); // needed for default entries to show up
#endif
  g_signal_connect(G_OBJECT(OSXApp), "NSApplicationBlockTermination", G_CALLBACK(_osx_quit_callback), NULL);
  g_signal_connect(G_OBJECT(OSXApp), "NSApplicationOpenFile", G_CALLBACK(_osx_openfile_callback), NULL);
#endif

  GtkWidget *widget;
  gui->ui = g_malloc0(sizeof(dt_ui_t));
  gui->surface = NULL;
  gui->center_tooltip = 0;
  gui->culling_mode = FALSE;
  gui->selection_stacked = FALSE;
  gui->presets_popup_menu = NULL;
  gui->last_preset = NULL;
  gui->export_popup.window = NULL;
  gui->export_popup.module = NULL;
  gui->input_devices = NULL;

  // load the style / theme
  GtkSettings *settings = gtk_settings_get_default();
  g_object_set(G_OBJECT(settings), "gtk-application-prefer-dark-theme", TRUE, (gchar *)0);
  g_object_set(G_OBJECT(settings), "gtk-theme-name", "Adwaita", (gchar *)0);
  g_object_unref(settings);

  // smooth scrolling must be enabled to handle trackpad/touch events
  gui->scroll_mask = GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK;

  // Emulates the same feature as Gtk focus but for scrolling events
  // The GtkWidget capturing scrolling events will write its address in this pointer
  gui->has_scroll_focus = NULL;

  // Init global accels. We localize the config because accels pathes use translated GUI labels.
  // User switching between languages may loose their custom shortcuts if we didn't localize them.
  // NOTE: needs to be inited before widgets, more specifically before the global menu
  gchar *keyboardrc = g_strdup_printf("keyboardrc.%s", dt_l10n_get_current_lang(darktable.l10n));
  gchar *keyboardrc_path = g_build_filename(configdir, keyboardrc, NULL);

  GtkAccelFlags flags = 0;
  if(dt_conf_get_bool("accels/mask")) flags |= GTK_ACCEL_MASK;
  gui->accels = dt_accels_init(keyboardrc_path, flags);
  g_free(keyboardrc);
  g_free(keyboardrc_path);

  // Initializing widgets
  _init_widgets(gui);

  //init overlay colors
  dt_guides_set_overlay_colors();

  snprintf(path, sizeof(path), "%s/icons", datadir);
  gtk_icon_theme_append_search_path(gtk_icon_theme_get_default(), path);
  snprintf(path, sizeof(path), "%s/icons", sharedir);
  gtk_icon_theme_append_search_path(gtk_icon_theme_get_default(), path);

  widget = dt_ui_center(darktable.gui->ui);

  gtk_widget_set_can_focus(widget, TRUE);
  gtk_widget_set_visible(widget, TRUE);
  gtk_widget_grab_focus(widget);

  g_signal_connect(G_OBJECT(widget), "configure-event", G_CALLBACK(_configure), gui);
  g_signal_connect(G_OBJECT(widget), "draw", G_CALLBACK(_draw), NULL);
  g_signal_connect(G_OBJECT(widget), "motion-notify-event", G_CALLBACK(_mouse_moved), NULL);
  g_signal_connect(G_OBJECT(widget), "key-press-event", G_CALLBACK(_key_pressed), NULL);
  g_signal_connect(G_OBJECT(widget), "leave-notify-event", G_CALLBACK(_center_leave), NULL);
  g_signal_connect(G_OBJECT(widget), "enter-notify-event", G_CALLBACK(_center_enter), NULL);
  g_signal_connect(G_OBJECT(widget), "button-press-event", G_CALLBACK(_button_pressed), NULL);
  g_signal_connect(G_OBJECT(widget), "button-release-event", G_CALLBACK(_button_released), NULL);
  g_signal_connect(G_OBJECT(widget), "scroll-event", G_CALLBACK(_scrolled), NULL);

  dt_gui_presets_init();

  dt_colorspaces_set_display_profile(DT_COLORSPACE_DISPLAY);
  // update the profile when the window is moved. resize is already handled in configure()
  widget = dt_ui_main_window(darktable.gui->ui);
  g_signal_connect(G_OBJECT(widget), "configure-event", G_CALLBACK(_window_configure), NULL);

  darktable.gui->reset = 0;

  // load theme
  dt_gui_load_theme(gui->gtkrc);

  // let's try to support pressure sensitive input devices like tablets for mask drawing
  dt_print(DT_DEBUG_INPUT, "[input device] Input devices found:\n\n");

  gui->input_devices
      = gdk_seat_get_slaves(gdk_display_get_default_seat(gdk_display_get_default()), GDK_SEAT_CAPABILITY_ALL);
  for(GList *l = gui->input_devices; l != NULL; l = g_list_next(l))
  {
    GdkDevice *device = (GdkDevice *)l->data;
    const GdkInputSource source = gdk_device_get_source(device);
    const gint n_axes = (source == GDK_SOURCE_KEYBOARD ? 0 : gdk_device_get_n_axes(device));

    // force-enable everything we find in screen mode.
    // TODO: make that an user param ?
    gdk_device_set_mode(device, GDK_MODE_SCREEN);

    dt_print(DT_DEBUG_INPUT, "%s (%s), source: %s, mode: %s, %d axes, %d keys\n", gdk_device_get_name(device),
             (source != GDK_SOURCE_KEYBOARD) && gdk_device_get_has_cursor(device) ? "with cursor" : "no cursor",
             _get_source_name(source),
             _get_mode_name(gdk_device_get_mode(device)), n_axes,
             source != GDK_SOURCE_KEYBOARD ? gdk_device_get_n_keys(device) : 0);

    for(int i = 0; i < n_axes; i++)
    {
      dt_print(DT_DEBUG_INPUT, "  %s\n", _get_axis_name(gdk_device_get_axis_use(device, i)));
    }
    dt_print(DT_DEBUG_INPUT, "\n");
  }

  // Gtk seems to capture some reserved shortcuts (Tab). We need to bypass it entirely
  // by hacking all events.
  gtk_widget_add_events(dt_ui_main_window(gui->ui), gui->scroll_mask);
  g_signal_connect(G_OBJECT(dt_ui_main_window(gui->ui)), "event", G_CALLBACK(dt_accels_dispatch), gui->accels);

  // finally set the cursor to be the default.
  // for some reason this is needed on some systems to pick up the correctly themed cursor
  dt_control_change_cursor(GDK_LEFT_PTR);

  return 0;
}

void dt_gui_gtk_run(dt_gui_gtk_t *gui)
{
  GtkWidget *widget = dt_ui_center(darktable.gui->ui);
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  darktable.gui->surface
      = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, allocation.width, allocation.height);
  // need to pre-configure views to avoid crash caused by draw coming before configure-event
  darktable.control->tabborder = 8;
  const int tb = darktable.control->tabborder;
  dt_view_manager_configure(darktable.view_manager, allocation.width - 2 * tb, allocation.height - 2 * tb);
#ifdef MAC_INTEGRATION
#ifdef GTK_TYPE_OSX_APPLICATION
  gtk_osxapplication_ready(g_object_new(GTK_TYPE_OSX_APPLICATION, NULL));
#else
  gtkosx_application_ready(g_object_new(GTKOSX_TYPE_APPLICATION, NULL));
#endif
#endif
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_focus_window();
#endif
  /* start the event loop */
  gtk_main();

  if (darktable.gui->surface)
  {
    cairo_surface_destroy(darktable.gui->surface);
    darktable.gui->surface = NULL;
  }
  dt_cleanup();
}

// refactored function to read current ppd, because gtk for osx has been unreliable
// we use the specific function here. Anyway, if nothing meaningful is found we default back to 1.0
double dt_get_system_gui_ppd(GtkWidget *widget)
{
  double res = 0.0f;
#ifdef GDK_WINDOWING_QUARTZ
  res = dt_osx_get_ppd();
#else
  res = gtk_widget_get_scale_factor(widget);
#endif
  if((res < 1.0f) || (res > 4.0f))
  {
    dt_print(DT_DEBUG_CONTROL, "[dt_get_system_gui_ppd] can't detect system ppd\n");
    return 1.0f;
  }
  dt_print(DT_DEBUG_CONTROL, "[dt_get_system_gui_ppd] system ppd is %f\n", res);
  return res;
}

void dt_configure_ppd_dpi(dt_gui_gtk_t *gui)
{
  GtkWidget *widget = gui->ui->main_window;

  gui->ppd = dt_get_system_gui_ppd(widget);
  gui->filter_image = CAIRO_FILTER_GOOD;

  // get the screen resolution
  const float screen_dpi_overwrite = dt_conf_get_float("screen_dpi_overwrite");
  if(screen_dpi_overwrite > 0.0)
  {
    gui->dpi = screen_dpi_overwrite;
    gdk_screen_set_resolution(gtk_widget_get_screen(widget), screen_dpi_overwrite);
    dt_print(DT_DEBUG_CONTROL, "[screen resolution] setting the screen resolution to %f dpi as specified in "
                               "the configuration file\n",
             screen_dpi_overwrite);
  }
  else
  {
#ifdef GDK_WINDOWING_QUARTZ
    dt_osx_autoset_dpi(widget);
#endif
    gui->dpi = gdk_screen_get_resolution(gtk_widget_get_screen(widget));
    if(gui->dpi < 0.0)
    {
      gui->dpi = 96.0;
      gdk_screen_set_resolution(gtk_widget_get_screen(widget), 96.0);
      dt_print(DT_DEBUG_CONTROL, "[screen resolution] setting the screen resolution to the default 96 dpi\n");
    }
    else
      dt_print(DT_DEBUG_CONTROL, "[screen resolution] setting the screen resolution to %f dpi\n", gui->dpi);
  }
  gui->dpi_factor
      = gui->dpi / 96; // according to man xrandr and the docs of gdk_screen_set_resolution 96 is the default
}

static gboolean _focus_in_out_event(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  gtk_window_set_urgency_hint(GTK_WINDOW(widget), FALSE);
  return FALSE;
}

static gboolean _ui_log_button_press_event(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  gtk_widget_hide(GTK_WIDGET(user_data));
  return TRUE;
}

static gboolean _ui_toast_button_press_event(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  gtk_widget_hide(GTK_WIDGET(user_data));
  return TRUE;
}

static void _init_widgets(dt_gui_gtk_t *gui)
{
  GtkWidget *container;
  GtkWidget *widget;

  // Creating the main window
  gui->ui->main_window  = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_widget_set_name(gui->ui->main_window , "main_window");
  gtk_window_set_role(GTK_WINDOW(gui->ui->main_window ), "main-app");
  gtk_window_set_icon_name(GTK_WINDOW(gui->ui->main_window ), "ansel");
  gtk_window_set_title(GTK_WINDOW(gui->ui->main_window ), "Ansel");

  // Init the titlebar ASAP because we replace the desktop titlebar & decoration with ours
  dt_ui_init_titlebar(gui->ui);

  dt_configure_ppd_dpi(gui);

  gtk_window_set_default_size(GTK_WINDOW(gui->ui->main_window), DT_PIXEL_APPLY_DPI(1200), DT_PIXEL_APPLY_DPI(800));

  // NOTE: allowing full-screen on startup shits the bed with MacOS
  if(dt_conf_get_bool("ui_last/maximized"))
  {
    gtk_window_maximize(GTK_WINDOW(gui->ui->main_window));
  }
  else
  {
    int width = dt_conf_get_int("ui_last/window_width");
    int height = dt_conf_get_int("ui_last/window_height");
    gtk_window_resize(GTK_WINDOW(gui->ui->main_window), width, height);
  }

  dt_gui_splash_set_transient_for(gui->ui->main_window);

  g_signal_connect(G_OBJECT(gui->ui->main_window ), "delete_event", G_CALLBACK(dt_gui_quit_callback), NULL);
  g_signal_connect(G_OBJECT(gui->ui->main_window ), "focus-in-event", G_CALLBACK(_focus_in_out_event), NULL);
  g_signal_connect(G_OBJECT(gui->ui->main_window ), "focus-out-event", G_CALLBACK(_focus_in_out_event), NULL);

  container = gui->ui->main_window;

  // Adding the outermost vbox
  widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(container), widget);
  gtk_widget_show(widget);

  /* connect to signal redraw all */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_CONTROL_REDRAW_ALL,
                            G_CALLBACK(_ui_widget_redraw_callback), gui->ui->main_window);

  container = widget;

  // Initializing the main table
  dt_ui_init_main_table(container, gui->ui);

  /* the log message */
  GtkWidget *eb = gtk_event_box_new();
  darktable.gui->ui->log_msg = gtk_label_new("");
  g_signal_connect(G_OBJECT(eb), "button-press-event", G_CALLBACK(_ui_log_button_press_event),
                   darktable.gui->ui->log_msg);
  gtk_label_set_ellipsize(GTK_LABEL(darktable.gui->ui->log_msg), PANGO_ELLIPSIZE_MIDDLE);
  dt_gui_add_class(darktable.gui->ui->log_msg, "dt_messages");
  gtk_container_add(GTK_CONTAINER(eb), darktable.gui->ui->log_msg);
  gtk_widget_set_valign(eb, GTK_ALIGN_CENTER);
  gtk_widget_set_halign(eb, GTK_ALIGN_CENTER);
  gtk_overlay_add_overlay(GTK_OVERLAY(darktable.gui->ui->center_base), eb);
  //gtk_overlay_reorder_overlay(GTK_OVERLAY(darktable.gui->ui->center_base), eb, -1);

  /* the toast message */
  eb = gtk_event_box_new();
  darktable.gui->ui->toast_msg = gtk_label_new("");
  g_signal_connect(G_OBJECT(eb), "button-press-event", G_CALLBACK(_ui_toast_button_press_event),
                   darktable.gui->ui->toast_msg);
  gtk_widget_set_events(eb, GDK_BUTTON_PRESS_MASK | darktable.gui->scroll_mask);
  g_signal_connect(G_OBJECT(eb), "scroll-event", G_CALLBACK(_scrolled), NULL);
  gtk_label_set_ellipsize(GTK_LABEL(darktable.gui->ui->toast_msg), PANGO_ELLIPSIZE_MIDDLE);

  PangoAttrList *attrlist = pango_attr_list_new();
  PangoAttribute *attr = pango_attr_font_features_new("tnum");
  pango_attr_list_insert(attrlist, attr);
  gtk_label_set_attributes(GTK_LABEL(darktable.gui->ui->toast_msg), attrlist);
  pango_attr_list_unref(attrlist);

  dt_gui_add_class(darktable.gui->ui->toast_msg, "dt_messages");
  gtk_container_add(GTK_CONTAINER(eb), darktable.gui->ui->toast_msg);
  gtk_widget_set_valign(eb, GTK_ALIGN_START);
  gtk_widget_set_halign(eb, GTK_ALIGN_CENTER);
  gtk_overlay_add_overlay(GTK_OVERLAY(darktable.gui->ui->center_base), eb);
  //gtk_overlay_reorder_overlay(GTK_OVERLAY(darktable.gui->ui->center_base), eb, -1);

  /* update log message label */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_CONTROL_LOG_REDRAW, G_CALLBACK(_ui_log_redraw_callback),
                            darktable.gui->ui->log_msg);

  /* update toast message label */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_CONTROL_TOAST_REDRAW, G_CALLBACK(_ui_toast_redraw_callback),
                            darktable.gui->ui->toast_msg);


  // Showing everything
  gtk_widget_show_all(dt_ui_main_window(gui->ui));

  gtk_widget_set_visible(dt_ui_log_msg(gui->ui), FALSE);
  gtk_widget_set_visible(dt_ui_toast_msg(gui->ui), FALSE);
}

void dt_ui_container_focus_widget(dt_ui_t *ui, const dt_ui_container_t c, GtkWidget *w)
{
  g_return_if_fail(GTK_IS_CONTAINER(ui->containers[c]));

  if(GTK_WIDGET(ui->containers[c]) != gtk_widget_get_parent(w)) return;

  gtk_container_set_focus_child(GTK_CONTAINER(ui->containers[c]), w);
  gtk_widget_queue_draw(ui->containers[c]);
}

void dt_ui_container_foreach(dt_ui_t *ui, const dt_ui_container_t c, GtkCallback callback)
{
  g_return_if_fail(GTK_IS_CONTAINER(ui->containers[c]));
  gtk_container_foreach(GTK_CONTAINER(ui->containers[c]), callback, (gpointer)ui->containers[c]);
}

void dt_ui_container_destroy_children(dt_ui_t *ui, const dt_ui_container_t c)
{
  dt_gui_container_destroy_children(GTK_CONTAINER(ui->containers[c]));
}

void dt_ui_notify_user()
{
  if(darktable.gui && !gtk_window_is_active(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui))))
  {
    gtk_window_set_urgency_hint(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), TRUE);
#ifdef MAC_INTEGRATION
#ifdef GTK_TYPE_OSX_APPLICATION
    gtk_osxapplication_attention_request(g_object_new(GTK_TYPE_OSX_APPLICATION, NULL), INFO_REQUEST);
#else
    gtkosx_application_attention_request(g_object_new(GTKOSX_TYPE_APPLICATION, NULL), INFO_REQUEST);
#endif
#endif
  }
}

/* this is called as a signal handler, the signal raising logic asserts the gdk lock. */
static void _ui_widget_redraw_callback(gpointer instance, GtkWidget *widget)
{
   gtk_widget_queue_draw(widget);
}

static void _ui_log_redraw_callback(gpointer instance, GtkWidget *widget)
{
  // draw log message, if any
  dt_pthread_mutex_lock(&darktable.control->log_mutex);
  if(!GTK_IS_LABEL(widget))
  {
    dt_pthread_mutex_unlock(&darktable.control->log_mutex);
    return;
  }
  if(darktable.control->log_ack != darktable.control->log_pos)
  {
    if(strcmp(darktable.control->log_message[darktable.control->log_ack], gtk_label_get_text(GTK_LABEL(widget))))
      gtk_label_set_markup(GTK_LABEL(widget), darktable.control->log_message[darktable.control->log_ack]);
    gtk_widget_show(widget);
  }
  else
  {
    gtk_widget_hide(widget);
  }
  dt_pthread_mutex_unlock(&darktable.control->log_mutex);
}

static void _ui_toast_redraw_callback(gpointer instance, GtkWidget *widget)
{
  // draw toast message, if any
  dt_pthread_mutex_lock(&darktable.control->toast_mutex);
  if(!GTK_IS_LABEL(widget))
  {
    dt_pthread_mutex_unlock(&darktable.control->toast_mutex);
    return;
  }
  if(darktable.control->toast_ack != darktable.control->toast_pos)
  {
    if(strcmp(darktable.control->toast_message[darktable.control->toast_ack], gtk_label_get_text(GTK_LABEL(widget))))
      gtk_label_set_markup(GTK_LABEL(widget), darktable.control->toast_message[darktable.control->toast_ack]);
    if(!gtk_widget_get_visible(widget))
    {
      const int h = gtk_widget_get_allocated_height(dt_ui_center_base(darktable.gui->ui));
      gtk_widget_set_margin_bottom(gtk_widget_get_parent(widget), 0.15 * h - DT_PIXEL_APPLY_DPI(10));
      gtk_widget_show(widget);
    }
  }
  else
  {
    if(gtk_widget_get_visible(widget)) gtk_widget_hide(widget);
  }
  dt_pthread_mutex_unlock(&darktable.control->toast_mutex);
}

void dt_ellipsize_combo(GtkComboBox *cbox)
{
  GList *renderers = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(cbox));
  for(const GList *it = renderers; it; it = g_list_next(it))
  {
    GtkCellRendererText *tr = GTK_CELL_RENDERER_TEXT(it->data);
    g_object_set(G_OBJECT(tr), "ellipsize", PANGO_ELLIPSIZE_MIDDLE, (gchar *)0);
  }
  g_list_free(renderers);
}

typedef struct result_t
{
  enum {RESULT_NONE, RESULT_NO, RESULT_YES} result;
  char *entry_text;
  GtkWidget *window, *entry, *button_yes, *button_no;
} result_t;

static void _gtk_main_quit_safe(GtkWidget *widget, gpointer data)
{
  (void)widget;
  (void)data;
  if(gtk_main_level() > 0) gtk_main_quit();
}

static void _yes_no_button_handler(GtkButton *button, gpointer data)
{
  result_t *result = (result_t *)data;

  if((void *)button == (void *)result->button_yes)
    result->result = RESULT_YES;
  else if((void *)button == (void *)result->button_no)
    result->result = RESULT_NO;

  if(result->entry)
    result->entry_text = g_strdup(gtk_entry_get_text(GTK_ENTRY(result->entry)));
  gtk_widget_destroy(result->window);
  _gtk_main_quit_safe(NULL, NULL);
}

gboolean dt_gui_show_standalone_yes_no_dialog(const char *title, const char *markup, const char *no_text,
                                              const char *yes_text)
{
  GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(window);
#endif

  // themes not yet loaded, no CSS add some manual padding
  const int padding = darktable.themes ? 0 : 5;

  gtk_window_set_icon_name(GTK_WINDOW(window), "ansel");
  gtk_window_set_title(GTK_WINDOW(window), title);
  g_signal_connect(window, "destroy", G_CALLBACK(_gtk_main_quit_safe), NULL);

  if(darktable.gui)
  {
    GtkWindow *win = GTK_WINDOW(dt_ui_main_window(darktable.gui->ui));
    gtk_window_set_transient_for(GTK_WINDOW(window), win);
    gtk_window_set_modal(GTK_WINDOW(window), TRUE);
    if(gtk_widget_get_visible(GTK_WIDGET(win)))
    {
      gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER_ON_PARENT);
    }
    else
    {
      gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
    }
  }
  else
  {
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
  }

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, padding);
  gtk_container_add(GTK_CONTAINER(window), vbox);

  GtkWidget *mhbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, padding);
  gtk_box_pack_start(GTK_BOX(vbox), mhbox, TRUE, TRUE, padding);

  if(padding)
  {
    gtk_box_pack_start(GTK_BOX(mhbox),
                       gtk_box_new(GTK_ORIENTATION_VERTICAL, padding), TRUE, TRUE, padding);
  }

  GtkWidget *label = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(label), markup);
  gtk_box_pack_start(GTK_BOX(mhbox), label, TRUE, TRUE, padding);

  if(padding)
  {
    gtk_box_pack_start(GTK_BOX(mhbox),
                       gtk_box_new(GTK_ORIENTATION_VERTICAL, padding), TRUE, TRUE, padding);
  }

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(vbox), hbox, TRUE, TRUE, 0);

  result_t result = {.result = RESULT_NONE, .window = window};

  GtkWidget *button;

  if(no_text)
  {
    button = gtk_button_new_with_label(no_text);
    result.button_no = button;
    g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_yes_no_button_handler), &result);
    gtk_box_pack_start(GTK_BOX(hbox), button, TRUE, TRUE, 0);
  }

  if(yes_text)
  {
    button = gtk_button_new_with_label(yes_text);
    result.button_yes = button;
    g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_yes_no_button_handler), &result);
    gtk_box_pack_start(GTK_BOX(hbox), button, TRUE, TRUE, 0);
  }

  gtk_widget_show_all(window);
  gtk_main();

  return result.result == RESULT_YES;
}

char *dt_gui_show_standalone_string_dialog(const char *title, const char *markup, const char *placeholder,
                                           const char *no_text, const char *yes_text)
{
  GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(window);
#endif

  gtk_window_set_icon_name(GTK_WINDOW(window), "ansel");
  gtk_window_set_title(GTK_WINDOW(window), title);
  g_signal_connect(window, "destroy", G_CALLBACK(_gtk_main_quit_safe), NULL);

  if(darktable.gui)
  {
    GtkWindow *win = GTK_WINDOW(dt_ui_main_window(darktable.gui->ui));
    gtk_window_set_transient_for(GTK_WINDOW(window), win);
    if(gtk_widget_get_visible(GTK_WIDGET(win)))
    {
      gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER_ON_PARENT);
    }
    else
    {
      gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_MOUSE);
    }
  }
  else
  {
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_MOUSE);
  }

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  gtk_widget_set_margin_start(vbox, 10);
  gtk_widget_set_margin_end(vbox, 10);
  gtk_widget_set_margin_top(vbox, 7);
  gtk_widget_set_margin_bottom(vbox, 5);
  gtk_container_add(GTK_CONTAINER(window), vbox);

  GtkWidget *label = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(label), markup);
  gtk_box_pack_start(GTK_BOX(vbox), label, TRUE, TRUE, 0);

  GtkWidget *entry = gtk_entry_new();
  dt_accels_disconnect_on_text_input(entry);

  g_object_ref(entry);
  if(placeholder)
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder);
  gtk_box_pack_start(GTK_BOX(vbox), entry, TRUE, TRUE, 0);

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_widget_set_margin_top(hbox, 10);
  gtk_box_pack_start(GTK_BOX(vbox), hbox, TRUE, TRUE, 0);

  result_t result = {.result = RESULT_NONE, .window = window, .entry = entry};

  GtkWidget *button;

  if(no_text)
  {
    button = gtk_button_new_with_label(no_text);
    result.button_no = button;
    g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_yes_no_button_handler), &result);
    gtk_box_pack_start(GTK_BOX(hbox), button, TRUE, TRUE, 0);
  }

  if(yes_text)
  {
    button = gtk_button_new_with_label(yes_text);
    result.button_yes = button;
    g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_yes_no_button_handler), &result);
    gtk_box_pack_start(GTK_BOX(hbox), button, TRUE, TRUE, 0);
  }

  gtk_widget_show_all(window);
  gtk_main();

  if(result.result == RESULT_YES)
    return result.entry_text;

  g_free(result.entry_text);
  return NULL;
}

// TODO: should that go to another place than gtk.c?
void dt_gui_add_help_link(GtkWidget *widget, const char *link)
{
  g_object_set_data(G_OBJECT(widget), "dt-help-url", (void *)link);
  gtk_widget_add_events(widget, GDK_BUTTON_PRESS_MASK);
}

// load a CSS theme
void dt_gui_load_theme(const char *theme)
{
  char theme_css[PATH_MAX] = { 0 };
  g_snprintf(theme_css, sizeof(theme_css), "%s.css", theme);

  if(!dt_conf_key_exists("use_system_font"))
    dt_conf_set_bool("use_system_font", TRUE);

  //set font size
  if(dt_conf_get_bool("use_system_font"))
    gtk_settings_reset_property(gtk_settings_get_default(), "gtk-font-name");
  else
  {
    //font name can only use period as decimal separator
    //but printf format strings use comma for some locales, so replace comma with period
    gchar *font_size = g_strdup_printf(_("%.1f"), dt_conf_get_float("font_size"));
    gchar *font_size_updated = dt_util_str_replace(font_size, ",", ".");
    gchar *font_name = g_strdup_printf(_("Sans %s"), font_size_updated);
    g_object_set(gtk_settings_get_default(), "gtk-font-name", font_name, NULL);
    g_free(font_size_updated);
    g_free(font_size);
    g_free(font_name);
  }

  gchar *path, *usercsspath;
  char datadir[PATH_MAX] = { 0 }, configdir[PATH_MAX] = { 0 };
  dt_loc_get_datadir(datadir, sizeof(datadir));
  dt_loc_get_user_config_dir(configdir, sizeof(configdir));

  // user dir theme
  path = g_build_filename(configdir, "themes", theme_css, NULL);
  if(!g_file_test(path, G_FILE_TEST_EXISTS))
  {
    // dt dir theme
    g_free(path);
    path = g_build_filename(datadir, "themes", theme_css, NULL);
    if(!g_file_test(path, G_FILE_TEST_EXISTS))
    {
      // fallback to default theme
      g_free(path);
      path = g_build_filename(datadir, "themes", "ansel.css", NULL);
      dt_conf_set_string("ui_last/theme", "ansel");
    }
    else
      dt_conf_set_string("ui_last/theme", theme);
  }
  else
    dt_conf_set_string("ui_last/theme", theme);

  GError *error = NULL;

  GtkStyleProvider *themes_style_provider = GTK_STYLE_PROVIDER(gtk_css_provider_new());
  gtk_style_context_add_provider_for_screen
    (gdk_screen_get_default(), themes_style_provider, GTK_STYLE_PROVIDER_PRIORITY_USER + 1);

  usercsspath = g_build_filename(configdir, "user.css", NULL);

  gchar *path_uri = g_filename_to_uri(path, NULL, &error);
  if(path_uri == NULL)
    fprintf(stderr, "%s: could not convert path %s to URI. Error: %s\n", G_STRFUNC, path, error->message);

  gchar *usercsspath_uri = g_filename_to_uri(usercsspath, NULL, &error);
  if(usercsspath_uri == NULL)
    fprintf(stderr, "%s: could not convert path %s to URI. Error: %s\n", G_STRFUNC, usercsspath, error->message);

  gchar *themecss = NULL;
  if(dt_conf_get_bool("themes/usercss") && g_file_test(usercsspath, G_FILE_TEST_EXISTS))
  {
    themecss = g_strjoin(NULL, "@import url('", path_uri,
                                           "'); @import url('", usercsspath_uri, "');", NULL);
  }
  else
  {
    themecss = g_strjoin(NULL, "@import url('", path_uri, "');", NULL);
  }

  g_free(path_uri);
  g_free(usercsspath_uri);
  g_free(path);
  g_free(usercsspath);

  if(dt_conf_get_bool("ui/hide_tooltips"))
  {
    gchar *newcss = g_strjoin(NULL, themecss, " tooltip {opacity: 0; background: transparent;}", NULL);
    g_free(themecss);
    themecss = newcss;
  }

  if(!gtk_css_provider_load_from_data(GTK_CSS_PROVIDER(themes_style_provider), themecss, -1, &error))
  {
    fprintf(stderr, "%s: error parsing combined CSS %s: %s\n", G_STRFUNC, themecss, error->message);
    g_clear_error(&error);
  }

  g_free(themecss);

  g_object_unref(themes_style_provider);

  // setup the colors

  GdkRGBA *c = darktable.gui->colors;
  GtkWidget *main_window = dt_ui_main_window(darktable.gui->ui);
  GtkStyleContext *ctx = gtk_widget_get_style_context(main_window);

  c[DT_GUI_COLOR_BG] = (GdkRGBA){ 0.1333, 0.1333, 0.1333, 1.0 };

  struct color_init
  {
    const char *name;
    GdkRGBA default_col;
  } init[DT_GUI_COLOR_LAST] = {
    [DT_GUI_COLOR_DARKROOM_BG] = { "darkroom_bg_color", { .2, .2, .2, 1.0 } },
    [DT_GUI_COLOR_DARKROOM_PREVIEW_BG] = { "darkroom_preview_bg_color", { .1, .1, .1, 1.0 } },
    [DT_GUI_COLOR_LIGHTTABLE_BG] = { "lighttable_bg_color", { .2, .2, .2, 1.0 } },
    [DT_GUI_COLOR_LIGHTTABLE_PREVIEW_BG] = { "lighttable_preview_bg_color", { .1, .1, .1, 1.0 } },
    [DT_GUI_COLOR_LIGHTTABLE_FONT] = { "lighttable_bg_font_color", { .7, .7, .7, 1.0 } },
    [DT_GUI_COLOR_PRINT_BG] = { "print_bg_color", { .2, .2, .2, 1.0 } },
    [DT_GUI_COLOR_BRUSH_CURSOR] = { "brush_cursor", { 1., 1., 1., 0.9 } },
    [DT_GUI_COLOR_BRUSH_TRACE] = { "brush_trace", { 0., 0., 0., 0.8 } },
    [DT_GUI_COLOR_BUTTON_FG] = { "button_fg", { 0.7, 0.7, 0.7, 0.55 } },
    [DT_GUI_COLOR_THUMBNAIL_BG] = { "thumbnail_bg_color", { 0.4, 0.4, 0.4, 1.0 } },
    [DT_GUI_COLOR_THUMBNAIL_SELECTED_BG] = { "thumbnail_selected_bg_color", { 0.8, 0.8, 0.8, 1.0 } },
    [DT_GUI_COLOR_THUMBNAIL_HOVER_BG] = { "thumbnail_hover_bg_color", { 0.65, 0.65, 0.65, 1.0 } },
    [DT_GUI_COLOR_THUMBNAIL_OUTLINE] = { "thumbnail_outline_color", { 0.2, 0.2, 0.2, 1.0 } },
    [DT_GUI_COLOR_THUMBNAIL_SELECTED_OUTLINE] = { "thumbnail_selected_outline_color", { 0.4, 0.4, 0.4, 1.0 } },
    [DT_GUI_COLOR_THUMBNAIL_HOVER_OUTLINE] = { "thumbnail_hover_outline_color", { 0.6, 0.6, 0.6, 1.0 } },
    [DT_GUI_COLOR_THUMBNAIL_FONT] = { "thumbnail_font_color", { 0.425, 0.425, 0.425, 1.0 } },
    [DT_GUI_COLOR_THUMBNAIL_SELECTED_FONT] = { "thumbnail_selected_font_color", { 0.5, 0.5, 0.5, 1.0 } },
    [DT_GUI_COLOR_THUMBNAIL_HOVER_FONT] = { "thumbnail_hover_font_color", { 0.7, 0.7, 0.7, 1.0 } },
    [DT_GUI_COLOR_THUMBNAIL_BORDER] = { "thumbnail_border_color", { 0.1, 0.1, 0.1, 1.0 } },
    [DT_GUI_COLOR_THUMBNAIL_SELECTED_BORDER] = { "thumbnail_selected_border_color", { 0.9, 0.9, 0.9, 1.0 } },
    [DT_GUI_COLOR_FILMSTRIP_BG] = { "filmstrip_bg_color", { 0.2, 0.2, 0.2, 1.0 } },
    [DT_GUI_COLOR_PREVIEW_HOVER_BORDER] = { "preview_hover_border_color", { 0.9, 0.9, 0.9, 1.0 } },
    [DT_GUI_COLOR_LOG_BG] = { "log_bg_color", { 0.1, 0.1, 0.1, 1.0 } },
    [DT_GUI_COLOR_LOG_FG] = { "log_fg_color", { 0.6, 0.6, 0.6, 1.0 } },
    [DT_GUI_COLOR_MAP_COUNT_SAME_LOC] = { "map_count_same_loc_color", { 1.0, 1.0, 1.0, 1.0 } },
    [DT_GUI_COLOR_MAP_COUNT_DIFF_LOC] = { "map_count_diff_loc_color", { 1.0, 0.85, 0.0, 1.0 } },
    [DT_GUI_COLOR_MAP_COUNT_BG] = { "map_count_bg_color", { 0.0, 0.0, 0.0, 1.0 } },
    [DT_GUI_COLOR_MAP_LOC_SHAPE_HIGH] = { "map_count_circle_color_h", { 1.0, 1.0, 0.8, 1.0 } },
    [DT_GUI_COLOR_MAP_LOC_SHAPE_LOW] = { "map_count_circle_color_l", { 0.0, 0.0, 0.0, 1.0 } },
    [DT_GUI_COLOR_MAP_LOC_SHAPE_DEF] = { "map_count_circle_color_d", { 1.0, 0.0, 0.0, 1.0 } },
  };

  // starting from 1 as DT_GUI_COLOR_BG is not part of this table
  for(int i = 1; i < DT_GUI_COLOR_LAST; i++)
  {
    if(!gtk_style_context_lookup_color(ctx, init[i].name, &c[i]))
    {
      c[i] = init[i].default_col;
    }
  }
}

GdkModifierType dt_key_modifier_state()
{
  guint state = 0;
  GdkWindow *window = gtk_widget_get_window(dt_ui_main_window(darktable.gui->ui));
  gdk_device_get_state(gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_window_get_display(window))), window, NULL, &state);
  return state;

/* FIXME double check correct way of doing this (merge conflict with Input System NG 20210319)
  GdkKeymap *keymap = gdk_keymap_get_for_display(gdk_display_get_default());
  return gdk_keymap_get_modifier_state(keymap) & gdk_keymap_get_modifier_mask(keymap, GDK_MODIFIER_INTENT_DEFAULT_MOD_MASK);
*/
}

static void _notebook_size_callback(GtkNotebook *notebook, GdkRectangle *allocation, gpointer *data)
{
  const int n = gtk_notebook_get_n_pages(notebook);
  g_return_if_fail(n > 0);

  GtkRequestedSize *sizes = g_malloc_n(n, sizeof(GtkRequestedSize));

  for(int i = 0; i < n; i++)
  {
    sizes[i].data = gtk_notebook_get_tab_label(notebook, gtk_notebook_get_nth_page(notebook, i));
    sizes[i].minimum_size = 0;
    GtkRequisition natural_size;
    gtk_widget_get_preferred_size(sizes[i].data, NULL, &natural_size);
    sizes[i].natural_size = natural_size.width;
  }

  GtkAllocation first, last;
  gtk_widget_get_allocation(sizes[0].data, &first);
  gtk_widget_get_allocation(sizes[n - 1].data, &last);

  const gint total_space = last.x + last.width - first.x; // ignore tab padding; CSS sets padding for label

  if(total_space > 0)
  {
    gtk_distribute_natural_allocation(total_space, n, sizes);

    for(int i = 0; i < n; i++)
      gtk_widget_set_size_request(sizes[i].data, sizes[i].minimum_size, -1);

    gtk_widget_size_allocate(GTK_WIDGET(notebook), allocation);

    for(int i = 0; i < n; i++)
      gtk_widget_set_size_request(sizes[i].data, -1, -1);
  }

  g_free(sizes);
}

// GTK_STATE_FLAG_PRELIGHT does not seem to get set on the label on hover so
// state-flags-changed cannot update darktable.control->element for shortcut mapping
static gboolean _notebook_motion_notify_callback(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  GtkAllocation notebook_alloc, label_alloc;
  gtk_widget_get_allocation(widget, &notebook_alloc);

  GtkNotebook *notebook = GTK_NOTEBOOK(widget);
  const int n = gtk_notebook_get_n_pages(notebook);
  for(int i = 0; i < n; i++)
  {
    gtk_widget_get_allocation(gtk_notebook_get_tab_label(notebook, gtk_notebook_get_nth_page(notebook, i)), &label_alloc);
  }

  return FALSE;
}

GtkNotebook *dt_ui_notebook_new()
{
  return GTK_NOTEBOOK(gtk_notebook_new());
}

GtkWidget *dt_ui_notebook_page(GtkNotebook *notebook, const char *text, const char *tooltip)
{
  gchar *text_cpy = g_strdup(_(text));
  dt_capitalize_label(text_cpy);
  GtkWidget *label = gtk_label_new(text_cpy);
  g_free(text_cpy);
  GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  if(strlen(text) > 2)
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_tooltip_text(label, tooltip ? tooltip : _(text));
  gtk_widget_set_has_tooltip(GTK_WIDGET(notebook), FALSE);

  gint page_num = gtk_notebook_append_page(notebook, page, label);
  gtk_container_child_set(GTK_CONTAINER(notebook), page, "tab-expand", TRUE, "tab-fill", TRUE, NULL);
  if(page_num == 1 &&
     !g_signal_handler_find(G_OBJECT(notebook), G_SIGNAL_MATCH_FUNC, 0, 0, NULL, _notebook_size_callback, NULL))
  {
    g_signal_connect(G_OBJECT(notebook), "size-allocate", G_CALLBACK(_notebook_size_callback), NULL);
    g_signal_connect(G_OBJECT(notebook), "motion-notify-event", G_CALLBACK(_notebook_motion_notify_callback), NULL);
  }

  return page;
}

static gint _get_container_row_heigth(GtkWidget *w)
{
  gint height = DT_PIXEL_APPLY_DPI(10);

  if(GTK_IS_TREE_VIEW(w))
  {
    gint row_height = 0;

    const gint num_columns = gtk_tree_view_get_n_columns(GTK_TREE_VIEW(w));
    for(int c = 0; c < num_columns; c++)
    {
      gint cell_height = 0;
      gtk_tree_view_column_cell_get_size(gtk_tree_view_get_column(GTK_TREE_VIEW(w), c),
                                        NULL, NULL, NULL, NULL, &cell_height);
      if(cell_height > row_height) row_height = cell_height;
    }
    GValue separation = { G_TYPE_INT };
    gtk_widget_style_get_property(w, "vertical-separator", &separation);

    if(row_height > 0) height = row_height + g_value_get_int(&separation);
  }
  else if(GTK_IS_TEXT_VIEW(w))
  {
    PangoLayout *layout = gtk_widget_create_pango_layout(w, "X");
    pango_layout_get_pixel_size(layout, NULL, &height);
    g_object_unref(layout);
  }
  else
  {
    GtkWidget *child = dt_gui_container_first_child(GTK_CONTAINER(w));
    if(child)
    {
      height = gtk_widget_get_allocated_height(child);
    }
  }

  return height;
}

static gboolean _scroll_wrap_resize(GtkWidget *w, void *cr, const char *config_str)
{
  GtkWidget *sw = gtk_widget_get_parent(w);
  if(GTK_IS_VIEWPORT(sw)) sw = gtk_widget_get_parent(sw);

  const gint increment = _get_container_row_heigth(w);

  gint height = dt_conf_get_int(config_str);

  // Max height is 75% of window height
  GtkWidget *container = dt_ui_main_window(darktable.gui->ui);
  const gint max_height = (container) ? gtk_widget_get_allocated_height(container) * 3 / 4 : DT_PIXEL_APPLY_DPI(1000);

  height = (height < 1) ? 1 : (height > max_height) ? max_height : height;

  dt_conf_set_int(config_str, height);

  gint content_height;
  gtk_widget_get_preferred_height(w, NULL, &content_height);

  const gint min_height = - gtk_scrolled_window_get_min_content_height(GTK_SCROLLED_WINDOW(sw));

  if(content_height < min_height) content_height = min_height;

  if(height > content_height) height = content_height;

  height += increment - 1;
  height -= height % increment;

  GtkBorder padding;
  gtk_style_context_get_padding(gtk_widget_get_style_context(sw),
                                gtk_widget_get_state_flags(sw),
                                &padding);

  gint old_height = 0;
  gtk_widget_get_size_request(sw, NULL, &old_height);
  const gint new_height = height + padding.top + padding.bottom + (GTK_IS_TEXT_VIEW(w) ? 2 : 0);
  if(new_height != old_height)
  {
    gtk_widget_set_size_request(sw, -1, new_height);

    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw));
    gint value = gtk_adjustment_get_value(adj);
    value -= value % increment;
    gtk_adjustment_set_value(adj, value);
  }
  return FALSE;
}

static gboolean _scroll_wrap_scroll(GtkScrolledWindow *sw, GdkEventScroll *event, const char *config_str)
{
  GtkWidget *w = gtk_bin_get_child(GTK_BIN(sw));
  if(GTK_IS_VIEWPORT(w)) w = gtk_bin_get_child(GTK_BIN(w));

  const gint increment = _get_container_row_heigth(w);

  int delta_y = 0;

  dt_gui_get_scroll_unit_deltas(event, NULL, &delta_y);

  if(dt_modifier_is(event->state, GDK_CONTROL_MASK))
  {
    const gint new_size = dt_conf_get_int(config_str) + increment*delta_y;

    dt_toast_log("%d", 1 + new_size / increment);

    dt_conf_set_int(config_str, new_size);

    _scroll_wrap_resize(w, NULL, config_str);

    return TRUE;
  }

  return FALSE;
}

GtkWidget *dt_ui_scroll_wrap(GtkWidget *w, gint min_size, char *config_str)
{
  GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sw), - DT_PIXEL_APPLY_DPI(min_size));
  g_signal_connect(G_OBJECT(sw), "scroll-event", G_CALLBACK(_scroll_wrap_scroll), config_str);
  g_signal_connect(G_OBJECT(w), "draw", G_CALLBACK(_scroll_wrap_resize), config_str);
  gtk_container_add(GTK_CONTAINER(sw), w);

  return GTK_WIDGET(sw);
}

gboolean dt_gui_container_has_children(GtkContainer *container)
{
  g_return_val_if_fail(GTK_IS_CONTAINER(container), FALSE);
  GList *children = gtk_container_get_children(container);
  gboolean has_children = children != NULL;
  g_list_free(children);
  return has_children;
}

int dt_gui_container_num_children(GtkContainer *container)
{
  g_return_val_if_fail(GTK_IS_CONTAINER(container), FALSE);
  GList *children = gtk_container_get_children(container);
  int num_children = g_list_length(children);
  g_list_free(children);
  return num_children;
}

GtkWidget *dt_gui_container_first_child(GtkContainer *container)
{
  g_return_val_if_fail(GTK_IS_CONTAINER(container), NULL);
  GList *children = gtk_container_get_children(container);
  GtkWidget *child = children ? (GtkWidget*)children->data : NULL;
  g_list_free(children);
  return child;
}

GtkWidget *dt_gui_container_nth_child(GtkContainer *container, int which)
{
  g_return_val_if_fail(GTK_IS_CONTAINER(container), NULL);
  GList *children = gtk_container_get_children(container);
  GtkWidget *child = (GtkWidget*)g_list_nth_data(children, which);
  g_list_free(children);
  return child;
}

static void _remove_child(GtkWidget *widget, gpointer data)
{
  gtk_container_remove((GtkContainer*)data, widget);
}

void dt_gui_container_remove_children(GtkContainer *container)
{
  g_return_if_fail(GTK_IS_CONTAINER(container));
  gtk_container_foreach(container, _remove_child, container);
}

static void _delete_child(GtkWidget *widget, gpointer data)
{
  (void)data;  // avoid unreferenced-parameter warning
  gtk_widget_destroy(widget);
}

void dt_gui_container_destroy_children(GtkContainer *container)
{
  g_return_if_fail(GTK_IS_CONTAINER(container));
  gtk_container_foreach(container, _delete_child, NULL);
}

void dt_gui_menu_popup(GtkMenu *menu, GtkWidget *button, GdkGravity widget_anchor, GdkGravity menu_anchor)
{
  gtk_widget_show_all(GTK_WIDGET(menu));

  GdkEvent *event = gtk_get_current_event();
  if(button && event)
  {
    gtk_menu_popup_at_widget(menu, button, widget_anchor, menu_anchor, event);
  }
  else
  {
    if(!event)
    {
      event = gdk_event_new(GDK_BUTTON_PRESS);
      event->button.device = gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default()));
      event->button.window = gtk_widget_get_window(GTK_WIDGET(darktable.gui->ui->main_window));
      g_object_ref(event->button.window);
    }

    gtk_menu_popup_at_pointer(menu, event);
  }
  gdk_event_free(event);
}

// draw rounded rectangle
void dt_gui_draw_rounded_rectangle(cairo_t *cr, float width, float height, float x, float y)
{
  const float radius = height / 5.0f;
  const float degrees = M_PI / 180.0;
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + width - radius, y + radius, radius, -90 * degrees, 0 * degrees);
  cairo_arc(cr, x + width - radius, y + height - radius, radius, 0 * degrees, 90 * degrees);
  cairo_arc(cr, x + radius, y + height - radius, radius, 90 * degrees, 180 * degrees);
  cairo_arc(cr, x + radius, y + radius, radius, 180 * degrees, 270 * degrees);
  cairo_close_path(cr);
  cairo_fill(cr);
}

gboolean dt_gui_search_start(GtkWidget *widget, GdkEventKey *event, GtkSearchEntry *entry)
{
  if(gtk_search_entry_handle_event(entry, (GdkEvent *)event))
  {
    gtk_entry_grab_focus_without_selecting(GTK_ENTRY(entry));
    return TRUE;
  }

  return FALSE;
}

void dt_gui_search_stop(GtkSearchEntry *entry, GtkWidget *widget)
{
  gtk_widget_grab_focus(widget);

  gtk_entry_set_text(GTK_ENTRY(entry), "");

  if(GTK_IS_TREE_VIEW(widget))
  {
    GtkTreePath *path = NULL;
    gtk_tree_view_get_cursor(GTK_TREE_VIEW(widget), &path, NULL);
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(widget)), path);
    gtk_tree_path_free(path);
  }
}

static void _collapsible_set_states(dt_gui_collapsible_section_t *cs, gboolean active)
{
  if(active)
  {
    // We don't apply the GTK_STATE_SELECTED flag to the container here because it would
    // be inherited by all children, which would mess up the state of checkboxes and togglebuttons.
    dt_gui_add_class(GTK_WIDGET(cs->expander), "active");
  }
  else
  {
    gtk_widget_set_state_flags(GTK_WIDGET(cs->expander), GTK_STATE_FLAG_NORMAL, TRUE);
    dt_gui_remove_class(GTK_WIDGET(cs->expander), "active");
  }
}

static void _coeffs_button_changed(GtkDarktableToggleButton *widget, gpointer user_data)
{
  dt_gui_collapsible_section_t *cs = (dt_gui_collapsible_section_t *)user_data;

  const gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cs->toggle));
  dtgtk_expander_set_expanded(DTGTK_EXPANDER(cs->expander), active);
  dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(cs->toggle), dtgtk_cairo_paint_solid_arrow,
                               (active ? CPF_DIRECTION_DOWN : CPF_DIRECTION_LEFT), NULL);
  dt_conf_set_bool(cs->confname, active);
  _collapsible_set_states(cs, active);
}

static void _coeffs_expander_click(GtkWidget *widget, GdkEventButton *e, gpointer user_data)
{
  if(e->type == GDK_2BUTTON_PRESS || e->type == GDK_3BUTTON_PRESS) return;

  dt_gui_collapsible_section_t *cs = (dt_gui_collapsible_section_t *)user_data;

  const gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cs->toggle));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cs->toggle), !active);
  _collapsible_set_states(cs, !active);
}

void dt_gui_update_collapsible_section(dt_gui_collapsible_section_t *cs)
{
  const gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cs->toggle));
  dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(cs->toggle), dtgtk_cairo_paint_solid_arrow,
                               (active ? CPF_DIRECTION_DOWN : CPF_DIRECTION_LEFT), NULL);
  dtgtk_expander_set_expanded(DTGTK_EXPANDER(cs->expander), active);

  if(active)
    gtk_widget_show(GTK_WIDGET(cs->container));
  else
    gtk_widget_hide(GTK_WIDGET(cs->container));

  _collapsible_set_states(cs, active);
}

void dt_gui_hide_collapsible_section(dt_gui_collapsible_section_t *cs)
{
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cs->toggle), FALSE);
  gtk_widget_hide(GTK_WIDGET(cs->container));
  _collapsible_set_states(cs, FALSE);
}

void dt_gui_new_collapsible_section(dt_gui_collapsible_section_t *cs,
                                    const char *confname, const char *label, GtkBox *parent)
{
  const gboolean expanded = dt_conf_get_bool(confname);

  cs->confname = g_strdup(confname);
  cs->parent = parent;

  // collapsible section header
  GtkWidget *destdisp_head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_BAUHAUS_SPACE);
  GtkWidget *header_evb = gtk_event_box_new();
  cs->label = dt_ui_section_label_new(label);
  dt_gui_add_class(destdisp_head, "dt_section_expander");
  gtk_container_add(GTK_CONTAINER(header_evb), cs->label);

  cs->toggle = dtgtk_togglebutton_new(dtgtk_cairo_paint_solid_arrow,
                                      (expanded ? CPF_DIRECTION_DOWN : CPF_DIRECTION_LEFT), NULL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cs->toggle), expanded);
  dt_gui_add_class(cs->toggle, "dt_ignore_fg_state");

  cs->container = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE));
  gtk_widget_set_name(GTK_WIDGET(cs->container), "collapsible");
  gtk_box_pack_start(GTK_BOX(destdisp_head), header_evb, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(destdisp_head), cs->toggle, FALSE, FALSE, 0);

  cs->expander = dtgtk_expander_new(destdisp_head, GTK_WIDGET(cs->container));
  gtk_box_pack_end(GTK_BOX(cs->parent), cs->expander, FALSE, FALSE, 0);
  dtgtk_expander_set_expanded(DTGTK_EXPANDER(cs->expander), expanded);
  gtk_widget_set_name(cs->expander, "collapse-block");

  g_signal_connect(G_OBJECT(cs->toggle), "toggled",
                   G_CALLBACK(_coeffs_button_changed),  (gpointer)cs);

  g_signal_connect(G_OBJECT(header_evb), "button-release-event",
                   G_CALLBACK(_coeffs_expander_click),
                   (gpointer)cs);
}

void dt_capitalize_label(gchar *text)
{
  if(text)
  {
    const char *underscore = "_";

    // Deal with strings beginning with Mnemonics
    if(text[0] == underscore[0])
      text[1] = g_unichar_toupper(text[1]);
    else
      text[0] = g_unichar_toupper(text[0]);
  }
}

GtkBox * attach_popover(GtkWidget *widget, const char *icon, GtkWidget *content)
{
  // Create the wrapping box and add the original widget to it
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(box), widget, FALSE, FALSE, 0);

  // Create the info icon button that will trigger the popover
  GtkWidget *button = gtk_menu_button_new();
  GtkWidget *image = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON);
  gtk_button_set_image(GTK_BUTTON(button), image);
  gtk_widget_set_hexpand(button, FALSE);
  gtk_widget_set_vexpand(button, FALSE);
  gtk_widget_set_size_request(button, DT_PIXEL_APPLY_DPI(16), DT_PIXEL_APPLY_DPI(16));
  gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);

  // Create the content of the popover
  GtkWidget *popover_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(popover_box), content, FALSE, FALSE, 0);

  // Wrap the content into a popover and attach it to the button
  GtkWidget *popover = gtk_popover_new(button);
  gtk_container_add(GTK_CONTAINER(popover), popover_box);
  gtk_popover_set_modal(GTK_POPOVER(popover), FALSE);
  gtk_menu_button_set_popover(GTK_MENU_BUTTON(button), popover);
  gtk_widget_show_all(popover_box);

  return GTK_BOX(box);
}

GtkBox * attach_help_popover(GtkWidget *widget, const char *label)
{
  // Create the content of the popover
  GtkWidget *popover_label = gtk_label_new(label);
  gtk_label_set_line_wrap(GTK_LABEL(popover_label), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(popover_label), 60);
  return attach_popover(widget, "help-about", popover_label);
}

static gboolean _text_entry_focus_in_event(GtkWidget *self, GdkEventFocus event, gpointer user_data)
{
  dt_accels_disable(darktable.gui->accels, TRUE);
  return FALSE;
}

static gboolean _text_entry_focus_out_event(GtkWidget *self, GdkEventFocus event, gpointer user_data)
{
  dt_accels_disable(darktable.gui->accels, FALSE);
  return FALSE;
}

static gboolean _text_entry_key_pressed(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
  if(event->keyval == GDK_KEY_Escape)
  {
    dt_gui_refocus_center();
    return TRUE;
  }
  return FALSE;
}

void dt_accels_disconnect_on_text_input(GtkWidget *widget)
{
  gtk_widget_add_events(widget, GDK_FOCUS_CHANGE_MASK);
  g_signal_connect(G_OBJECT(widget), "focus-in-event", G_CALLBACK(_text_entry_focus_in_event), NULL);
  g_signal_connect(G_OBJECT(widget), "focus-out-event", G_CALLBACK(_text_entry_focus_out_event), NULL);
  g_signal_connect(G_OBJECT(widget), "key-press-event", G_CALLBACK(_text_entry_key_pressed), NULL);
}


void dt_gui_refocus_center()
{
  // Refocus window, useful if we just closed a popup/modal/transient
  gtk_window_present_with_time(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), GDK_CURRENT_TIME);

  // Desperate measure to refocus the window
  gtk_grab_add(dt_ui_main_window(darktable.gui->ui));
  gtk_grab_remove(dt_ui_main_window(darktable.gui->ui));
  gtk_widget_grab_focus(dt_ui_main_window(darktable.gui->ui));

  const char *current_view = dt_view_manager_name(darktable.view_manager);
  if(g_strcmp0(current_view, "lighttable"))
  {
    gtk_widget_grab_focus(darktable.gui->ui->thumbtable_lighttable->grid);
  }
  else
  {
    gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));
  }

  // Be sure to re-enable accelerators
  dt_accels_disable(darktable.gui->accels, FALSE);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
