/*
    This file is part of the Ansel project.
    Copyright (C) 2025 Aurélien PIERRE.

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Ansel is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Ansel.  If not, see <http://gnu.org>.
*/
#include "common/logging.h"
#include "colorprofiles/colorspaces.h"   // dt_colorprofiles_xyz_to_display()
#include "common/colorspaces_inline_conversions.h"   // dt_Lab_to_XYZ
#include "common/conf.h"
#include "system/macros.h"
#include "system/mem_alloc.h"
#include "system/simd.h"
#include "common/times.h"
#include "control/signal.h"
#include "gui/application.h"
#include "control/control.h"
#include "caches/image_cache.h"
#include "views/view.h"

#include <gtk/gtk.h>
#include "widgets/widget_settings.h"

#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

typedef struct dt_preview_window_t
{
  int32_t imgid;
  cairo_surface_t *surface;
  dt_view_image_surface_fetcher_t fetcher;
  GtkWidget *area;
  int width;
  int height;
} dt_preview_window_t;

void _preview_redraw(gpointer instance, dt_preview_window_t *preview);

/* --- Multi-screen placement helpers (added by Roland Sahli with the help of Google Ai and Euria) --- */
/* The currently visible preview dialog, used by the monitors-changed
 * handler so it can re-park the window when a screen is hot-plugged.
 * Single instance: the preview is a singleton window. */
static GtkWidget *_preview_dialog = NULL;
static gboolean   _preview_monitors_signal_connected = FALSE;

static GdkMonitor *_preview_find_secondary_monitor(GtkWidget *main_win)
{
  GdkDisplay *display = gtk_widget_get_display(main_win);
  GdkWindow *main_gdk = GTK_IS_WINDOW(main_win) ? gtk_widget_get_window(main_win) : NULL;
  GdkMonitor *main_mon = NULL;
  if (main_gdk) {
      main_mon = gdk_display_get_monitor_at_window(display, main_gdk);
  }
  const int n = gdk_display_get_n_monitors(display);
  for(int i = 0; i < n; i++)
  {
    GdkMonitor *mon = gdk_display_get_monitor(display, i);
    if(mon != main_mon) return mon;   // First monitor that is NOT the main window's
  }
  return NULL;                        // Single screen only
}

static void _preview_place_on_monitor(GtkWidget *dialog, GdkMonitor *mon)
{
  GdkRectangle geo;
  gdk_monitor_get_geometry(mon, &geo);
  gtk_window_unmaximize(GTK_WINDOW(dialog));
  gtk_window_move(GTK_WINDOW(dialog), geo.x, geo.y);
  gtk_window_maximize(GTK_WINDOW(dialog));
}

static void _preview_on_monitors_changed(GdkDisplay *display, gpointer user_data)
{
  (void)display; (void)user_data;
  if(!_preview_dialog || !gtk_widget_get_visible(_preview_dialog)) return;

  GtkWidget *main_win = dt_gui_main_window();
  GdkMonitor *sec = _preview_find_secondary_monitor(main_win);
  if(sec != NULL)
    _preview_place_on_monitor(_preview_dialog, sec);
  else
    gtk_window_unmaximize(GTK_WINDOW(_preview_dialog));
}
/* --- End placement helpers --- */

/* --- Per-screen persistent geometry (added by Roland Sahli with the help of Google Ai and Euria) --- */
/* Uses the modern GdkMonitor API only: this build defines GDK_DISABLE_DEPRECATED,
 * so the GdkScreen monitor accessors are hidden and must not be used. */

/* Helper to strip string down into clean anselrc underscores */
static void _preview_sanitize_string(gchar *str)
{
  for(int i = 0; str[i] != '\0'; i++)
  {
    if(!g_ascii_isalnum(str[i]) && str[i] != '/')
    {
      str[i] = '_';
    }
  }
}

/* Stable per-screen key derived from the monitor model name.
 * Automatically replaces spaces and non-alphanumeric characters with underscores '_' 
 * to adhere strictly to Ansel's configuration layout constraints. */
static gchar *_preview_geom_key(GdkMonitor *mon)
{
  const char *model = gdk_monitor_get_model(mon);
  gchar *raw_key = NULL;

  if(model != NULL)
  {
    raw_key = g_strdup_printf("plugins/lighttable/preview/geom/%s", model);
  }
  else
  {
    GdkRectangle g;
    gdk_monitor_get_geometry(mon, &g);
    raw_key = g_strdup_printf("plugins/lighttable/preview/geom/%dx%d%dx%d",
                              g.x, g.y, g.width, g.height);
  }

  _preview_sanitize_string(raw_key);
  return raw_key;
}

/* TRUE if the rect (x,y,w,h) intersects any currently-connected monitor.
 * Guards against restoring a window onto a screen that is no longer plugged. */
static gboolean _preview_rect_on_screen(GdkDisplay *display, int x, int y, int w, int h)
{
  const int n = gdk_display_get_n_monitors(display);
  for(int i = 0; i < n; i++)
  {
    GdkRectangle m;
    gdk_monitor_get_geometry(gdk_display_get_monitor(display, i), &m);

    const int ix = MAX(x, m.x);
    const int iy = MAX(y, m.y);
    const int iw = MIN(x + w, m.x + m.width)  - ix;
    const int ih = MIN(y + h, m.y + m.height) - iy;

    if(iw > 0 && ih > 0) return TRUE;   /* Non-empty intersection */
  }
  return FALSE;
}

/* Save current window geometry for the screen it currently sits on. */
static void _preview_save_geom(GtkWidget *dialog)
{
  GdkWindow *gw = gtk_widget_get_window(dialog);
  if(gw == NULL) return;

  GdkDisplay *display = gtk_widget_get_display(dialog);
  GdkMonitor *mon = gdk_display_get_monitor_at_window(display, gw);
  if(mon == NULL) return;

  // 1. Save unique per-monitor layout geometry configurations
  gchar *key = _preview_geom_key(mon);
  const gboolean maximized = gtk_window_is_maximized(GTK_WINDOW(dialog));

  int x = 0, y = 0, w = 0, h = 0;
  gtk_window_get_size(GTK_WINDOW(dialog), &w, &h);
  gtk_window_get_position(GTK_WINDOW(dialog), &x, &y);

  if(w <= 0 || h <= 0) 
  {
    g_free(key);
    return;
  }

  gchar *val = g_strdup_printf("%d,%d,%d,%d,%d", x, y, w, h, maximized ? 1 : 0);
  dt_conf_set_string(key, val);
  dt_print(DT_DEBUG_LIGHTTABLE, "[preview] saved geom %s = %s\n", key, val);
  g_free(key);
  g_free(val);

  // 2. Save the active monitor model name globally instead of its volatile index
  const char *model = gdk_monitor_get_model(mon);
  if(model != NULL)
  {
    gchar *sanitized_model = g_strdup(model);
    _preview_sanitize_string(sanitized_model);
    dt_conf_set_string("plugins/lighttable/preview/geom/last_monitor_model", sanitized_model);
    g_free(sanitized_model);
  }
  else
  {
    dt_conf_set_string("plugins/lighttable/preview/geom/last_monitor_model", "FALLBACK_GENERIC");
  }
}

/* Load saved geometry for a monitor. Returns TRUE only if an entry exists
 * AND its rect still lands on a connected screen (off-screen guard). */
static gboolean _preview_load_geom(GdkDisplay *display, GdkMonitor *mon,
                                   int *x, int *y, int *w, int *h, gboolean *maximized)
{
  gchar *key = _preview_geom_key(mon);
  const gchar *val = dt_conf_get_string(key);
  gboolean ok = FALSE;

  if(val != NULL)
  {
    int mx = 0;
    if(sscanf(val, "%d,%d,%d,%d,%d", x, y, w, h, &mx) == 5
       && _preview_rect_on_screen(display, *x, *y, *w, *h))
    {
      *maximized = (mx != 0);
      ok = TRUE;
    }
    g_free((gchar *)val);
  }
  g_free(key);
  return ok;
}

/* Intercept geometry manipulation hooks at runtime to sidestep widget destruction bugs */
static gboolean _preview_configure_event(GtkWidget *widget, GdkEventConfigure *event, gpointer user_data)
{
  (void)event; (void)user_data;
  _preview_save_geom(widget);
  return FALSE; // Allow GTK engine default event cascades
}

/* Capture maximize and screen-change events instantly before state data is lost */
static gboolean _preview_window_state_event(GtkWidget *widget, GdkEventWindowState *event, gpointer user_data)
{
  (void)event; (void)user_data;
  _preview_save_geom(widget);
  return FALSE; // Continue standard GTK event handling
}
/* --- End per-screen geometry --- */

static void _preview_window_destroy(GtkWidget *dialog, gpointer user_data)
{
  _preview_save_geom(dialog);   // Final safety persistence checkpoint before clearing allocation
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  
  // Disconnect from DARKROOM_UI_CHANGED before freeing to avoid runtime callback crashes
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(dt_control_signal_get_global(), G_CALLBACK(_preview_redraw), preview);
  if(_preview_dialog == dialog) _preview_dialog = NULL;   // Clear singleton lifecycle hook
  dt_view_image_surface_fetcher_cleanup(&preview->fetcher);
  dt_free(preview);
}

static gboolean _close_preview_window(GtkWidget *widget, GdkEvent *event, gpointer data)
{
  (void)event; (void)data;
  gtk_widget_destroy(widget);
  return TRUE; // Event fully handled
}

static void _preview_window_size_allocate(GtkWidget *widget, GtkAllocation *allocation, gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  if(IS_NULL_PTR(preview) || IS_NULL_PTR(allocation)) return;
  if(allocation->width < 2 || allocation->height < 2) return;
  if(preview->width == allocation->width && preview->height == allocation->height) return;

  preview->width = allocation->width;
  preview->height = allocation->height;

  /* Invalidate current pixelpipes instantly when canvas dimensions transform */
  dt_view_image_surface_fetcher_invalidate(&preview->fetcher, &preview->surface);
  gtk_widget_queue_draw(widget);
}

// TODO: we color-manage the preview window assuming it sits on the same screen as the main window
// Will need to grab the color profile of the actual monitor where it sits.
void _colormanage_ui_color(const float L, const float a, const float b, dt_aligned_pixel_t RGB)
{
  dt_aligned_pixel_t Lab = { L, a, b, 1.f };
  dt_aligned_pixel_t XYZ = { 0.f, 0.f, 0.f, 1.f };
  dt_Lab_to_XYZ(Lab, XYZ);
dt_colorprofiles_xyz_to_display(XYZ, RGB);
}
static gboolean
_thumb_draw_image(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
if(IS_NULL_PTR(preview)) return TRUE;
const double start = dt_get_wtime();
int w = gtk_widget_get_allocated_width(widget);
int h = gtk_widget_get_allocated_height(widget);
// Paint background using managed display target vectors
dt_aligned_pixel_t bg_color;
_colormanage_ui_color((float)dt_conf_get_int("display/brightness"), 0., 0., bg_color);
// Secure vector conversion: unpack bg_color vector float elements for cairo cleanly
const float *rgb_val = (const float *)&bg_color;
cairo_set_source_rgb(cr, (double)rgb_val[0], (double)rgb_val[1], (double)rgb_val[2]);
cairo_paint(cr);
int borders = DT_PIXEL_APPLY_DPI(dt_conf_get_int("plugins/darkroom/ui/border_size"));
w -= 2 * borders;
h -= 2 * borders;
// Fetch the color-managed image surface asynchronously
const dt_view_surface_value_t res =
dt_view_image_get_surface_async(&preview->fetcher, preview->imgid, w, h, &preview->surface, widget, 0);
if(preview->surface && res == DT_VIEW_SURFACE_OK)
{
int width = cairo_image_surface_get_width(preview->surface);
int height = cairo_image_surface_get_height(preview->surface);
double sx = 1.0, sy = 1.0;
cairo_surface_get_device_scale(preview->surface, &sx, &sy);
const double logical_width = width / sx;
const double logical_height = height / sy;
cairo_save(cr);
double x_offset = (w - logical_width) / 2. + borders;
double y_offset = (h - logical_height) / 2. + borders;
cairo_set_source_surface(cr, preview->surface, x_offset, y_offset);
GdkRGBA im_color;
GtkStyleContext *context = gtk_widget_get_style_context(widget);
gtk_style_context_get_color(context, gtk_widget_get_state_flags(widget), &im_color);
cairo_paint_with_alpha(cr, im_color.alpha);
gtk_render_frame(context, cr, borders, borders, w, h);
cairo_restore(cr);
}
else
{
dt_control_draw_busy_msg(cr, w, h);
}
dt_print(DT_DEBUG_LIGHTTABLE, "Redrawing the preview window for %i in %0.04f sec\n", preview->imgid,
dt_get_wtime() - start);
return TRUE;
}
void _preview_redraw(gpointer instance, dt_preview_window_t *preview)
{
(void)instance;
gtk_widget_queue_draw(preview->area);
}
/* Handle key presses on the preview window (added by Roland Sahli with the help of Google Ai and Euria) */
static gboolean _preview_key_press_event(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
(void)user_data;
// If the user presses the Escape key, close and destroy the preview window natively
if (event->keyval == GDK_KEY_Escape)
{
gtk_widget_destroy(widget);
return TRUE; // Event fully handled and consumed
}
return FALSE; // Pass other keys down to standard GTK mapping
}
/* Refactored by Roland Sahli with the help of Google Ai and Euria for independent,
• cross-platform, and stable per-screen persistent preview window geometry tracking. */
void dt_preview_window_spawn(const int32_t imgid)
{
dt_preview_window_t *preview = calloc(1, sizeof(dt_preview_window_t));
preview->imgid = imgid;
dt_view_image_surface_fetcher_init(&preview->fetcher);
// Instantiate as an autonomous base Top-Level Window structure for cross-OS independence and taskbar population
GtkWidget *dialog = gtk_window_new(GTK_WINDOW_TOPLEVEL);
// 1. Unified string initialization within safe cache lock lifetime
const dt_image_t *img = dt_image_cache_get(imgid, 'r');
gchar *name = NULL;
if (img)
{
name = g_strdup_printf(("Ansel - Preview : %s [v%d]"), img->filename, img->version);
}
else
{
name = g_strdup_printf(("Ansel - Preview"));
}
if (img)
{
dt_image_cache_read_release(img);
}
gtk_window_set_title(GTK_WINDOW(dialog), name);
GtkWidget *header_bar = gtk_dialog_get_header_bar(GTK_DIALOG(dialog));
if (header_bar && GTK_IS_HEADER_BAR(header_bar))
{
gtk_header_bar_set_title(GTK_HEADER_BAR(header_bar), name);
}
dt_free(name);
#ifdef GDK_WINDOWING_QUARTZ
dt_osx_disallow_fullscreen(dialog);
#endif
GtkWidget *main_win = dt_gui_main_window();
gtk_window_set_modal(GTK_WINDOW(dialog), FALSE);
gtk_window_set_type_hint(GTK_WINDOW(dialog), GDK_WINDOW_TYPE_HINT_NORMAL);
gtk_window_set_decorated(GTK_WINDOW(dialog), TRUE);
gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER);
// Connect core window signal infrastructure
g_signal_connect(G_OBJECT(dialog), "delete-event", G_CALLBACK(_close_preview_window), NULL);
g_signal_connect(G_OBJECT(dialog), "destroy", G_CALLBACK(_preview_window_destroy), preview);
g_signal_connect(G_OBJECT(dialog), "configure-event", G_CALLBACK(_preview_configure_event), NULL);
g_signal_connect(G_OBJECT(dialog), "window-state-event", G_CALLBACK(_preview_window_state_event), NULL);
// Connect key event hook to handle the Escape (Esc) key closure mapping natively
g_signal_connect(G_OBJECT(dialog), "key-press-event", G_CALLBACK(_preview_key_press_event), NULL);
// 2. Window container box layout structure initialization
GtkWidget *content_area = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
gtk_container_add(GTK_CONTAINER(dialog), content_area);
gtk_container_set_border_width(GTK_CONTAINER(dialog), 0);
gtk_container_set_border_width(GTK_CONTAINER(content_area), 0);
gtk_box_set_spacing(GTK_BOX(content_area), 0);
// 3. GTK transparency & theme CSS provider layer
float background_opacity = 1.00f;
GtkStyleContext *dialog_context = gtk_widget_get_style_context(dialog);
GtkCssProvider *provider = gtk_css_provider_new();
char css_buffer[256];
snprintf(css_buffer, sizeof(css_buffer),
"window, headerbar { background-color: rgba(45, 45, 45, %.2f); border: initial; box-shadow: initial; decoration: initial; }",
background_opacity);
gtk_css_provider_load_from_data(provider, css_buffer, -1, NULL);
gtk_style_context_add_provider(dialog_context, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
g_object_unref(provider);
// 4. Rendering drawing canvas properties
preview->area = gtk_drawing_area_new();
gtk_widget_set_hexpand(preview->area, TRUE);
gtk_widget_set_vexpand(preview->area, TRUE);
gtk_widget_set_halign(preview->area, GTK_ALIGN_FILL);
gtk_widget_set_valign(preview->area, GTK_ALIGN_FILL);
gtk_widget_set_size_request(preview->area, 420, 420);
gtk_box_pack_start(GTK_BOX(content_area), preview->area, TRUE, TRUE, 0);
g_signal_connect(G_OBJECT(preview->area), "draw", G_CALLBACK(_thumb_draw_image), preview);
g_signal_connect(G_OBJECT(preview->area), "size-allocate", G_CALLBACK(_preview_window_size_allocate), preview);
DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_DARKROOM_UI_CHANGED, G_CALLBACK(_preview_redraw), preview);
gtk_widget_set_visible(preview->area, TRUE);
// --- Multi-screen placement and per-screen persistent geometry (added by Roland Sahli with the help of Google Ai and Euria) ---
GdkDisplay *display = gtk_widget_get_display(main_win);
GdkMonitor *secondary = _preview_find_secondary_monitor(main_win);
GdkWindow *main_gdk = gtk_widget_get_window(main_win);
GdkMonitor *primary = main_gdk ? gdk_display_get_monitor_at_window(display, main_gdk) : gdk_display_get_monitor(display, 0);
int px = 0, py = 0, pw = 0, ph = 0;
gboolean pmax = FALSE;
gboolean geom_loaded = FALSE;
// 1. Target exact monitor resolution structures matching active hardware device name keys
const gchar *last_model = dt_conf_get_string("plugins/lighttable/preview/geom/last_monitor_model");
if (last_model != NULL)
{
GdkMonitor *target_mon = NULL;
const int monitor_count = gdk_display_get_n_monitors(display);
for (int i = 0; i < monitor_count; i++)
{
GdkMonitor *m = gdk_display_get_monitor(display, i);
const char *m_model = gdk_monitor_get_model(m);
if (m_model != NULL)
{
gchar *sanitized_m_model = g_strdup(m_model);
_preview_sanitize_string(sanitized_m_model);
if (g_strcmp0(sanitized_m_model, last_model) == 0)
{
target_mon = m;
g_free(sanitized_m_model);
break;
}
g_free(sanitized_m_model);
}
}
if (target_mon != NULL && _preview_load_geom(display, target_mon, &px, &py, &pw, &ph, &pmax))
{
geom_loaded = TRUE;
}
g_free((gchar *)last_model);
}
// 2. Sequential fallback execution pipeline
if (!geom_loaded && primary != NULL && _preview_load_geom(display, primary, &px, &py, &pw, &ph, &pmax))
{
geom_loaded = TRUE;
}
else if (!geom_loaded && secondary != NULL && _preview_load_geom(display, secondary, &px, &py, &pw, &ph, &pmax))
{
geom_loaded = TRUE;
}
// Final geometry injection execution block
if(geom_loaded)
{
gtk_widget_show_all(dialog);
gtk_window_move(GTK_WINDOW(dialog), px, py);
gtk_window_resize(GTK_WINDOW(dialog), pw, ph);
if(pmax) gtk_window_maximize(GTK_WINDOW(dialog));
}
else if(secondary != NULL)
{
gtk_widget_show_all(dialog);
_preview_place_on_monitor(dialog, secondary);
}
else
{
gtk_window_unmaximize(GTK_WINDOW(dialog));
gtk_window_resize(GTK_WINDOW(dialog), 420, 420);
gtk_widget_show_all(dialog);
}
_preview_dialog = dialog;
if(!_preview_monitors_signal_connected)
{
g_signal_connect(gtk_widget_get_display(main_win),
"monitors-changed", G_CALLBACK(_preview_on_monitors_changed), NULL);
_preview_monitors_signal_connected = TRUE;
}
}