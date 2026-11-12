/*
    This file is part of the Ansel project.
    Copyright (C) 2025 Aurélien PIERRE.
    Copyright (C) 2026 Guillaume Stutin.
    Copyright (C) 2026 Roland Sahli.
    
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
#include "control/user_message.h"
#include "caches/image_cache.h"
#include "views/view.h"

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include "widgets/widget_settings.h"

#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

/* ============================================================================
 * PLATFORM ABSTRACTION LAYER
 *
 * Window positioning semantics differ fundamentally across backends:
 *
 *   X11:     gtk_window_move() works after realization. The WM may ignore
 *            initial placement but honors post-map requests.
 *
 *   Wayland: Absolute positioning is architecturally impossible (no global
 *            coordinate system). gtk_window_move() is a no-op for top-level
 *            surfaces. Compositors may honor it as a weak hint. The only
 *            reliable cross-monitor strategy is to move the window's center
 *            point onto the target monitor before maximizing, which most
 *            compositors (GNOME Shell, KWin) interpret as "place here".
 *
 *   Quartz:  GTK delegates to Cocoa. gtk_window_move() works but coordinates
 *            are in Cocoa's flipped Y-axis space. GTK handles the conversion
 *            internally, so we use the same API. The GTK_WIN_POS_CENTER_ON_PARENT
 *            hint is preferred for initial placement.
 *
 *   Win32:   gtk_window_move() works like X11. The DWM honors post-map moves.
 *            Monitor enumeration via gdk_display_get_monitor_* is reliable.
 *
 * Strategy:
 *   - Always store the monitor identity (GdkMonitor*) — this is portable.
 *   - On X11/Win32/Quartz: use absolute coordinates for normal windows.
 *   - On Wayland: for normal windows, set default size and rely on compositor
 *     placement (we cannot do better). For maximized windows, nudge the center
 *     toward the target monitor before maximizing — this is the best available
 *     hint for monitor selection.
 * ========================================================================== */

/* Compile-time backend detection. GDK defines exactly one of these. */
#if defined(GDK_WINDOWING_WAYLAND) && !defined(GDK_WINDOWING_X11)
  #define PREVIEW_BACKEND_IS_WAYLAND_ONLY 1
#else
  #define PREVIEW_BACKEND_IS_WAYLAND_ONLY 0
#endif

/* Runtime backend check (handles multi-backend builds where both X11 and
 * Wayland are compiled in, which is the common case on modern Linux distros). */
static gboolean _preview_backend_is_wayland(void)
{
#ifdef GDK_WINDOWING_WAYLAND
  GdkDisplay *display = gdk_display_get_default();
  if(!IS_NULL_PTR(display) && GDK_IS_WAYLAND_DISPLAY(display))
    return TRUE;
#endif
  return FALSE;
}

/* ============================================================================
 * DATA STRUCTURES
 * ========================================================================== */

typedef struct dt_preview_window_t
{
  int32_t imgid;
  cairo_surface_t *surface;
  dt_view_image_surface_fetcher_t fetcher;
  GtkWidget *area;
  GtkWidget *dialog;
  struct dt_preview_window_t *prev_preview;
  gboolean close_prepared;
  gboolean maximized;
  int width;
  int height;
} dt_preview_window_t;

typedef struct dt_preview_window_geometry_t
{
  int x;
  int y;
  int width;
  int height;
  gboolean is_maximized;
  GdkMonitor *session_monitor; /* Physical monitor identity — portable across all backends */
  gboolean valid;
} dt_preview_window_geometry_t;

static dt_preview_window_geometry_t _preview_geometry = { 0 };
static dt_preview_window_t *_last_opened_preview = NULL;
static int _preview_count = 0;

void _preview_redraw(gpointer instance, int signal, gpointer user_data);
static void _preview_window_store_geometry(GtkWidget *dialog);

/* ============================================================================
 * GEOMETRY PERSISTENCE
 * ========================================================================== */

/*
 * Stores the current window geometry for later restoration.
 *
 * The monitor identity (session_monitor) is always recorded regardless of
 * window state, because it is the only piece of information that is both
 * portable and meaningful across all backends. Position and size are only
 * recorded for non-maximized windows (maximized geometry is monitor-dependent
 * and transient).
 *
 * CRITICAL FIX: Previously, the maximized early-return skipped setting
 * valid = TRUE, causing the next spawn to lose all placement state.
 */
static void _preview_window_store_geometry(GtkWidget *dialog)
{
  GdkWindow *gdk_win = gtk_widget_get_window(dialog);
  if(!IS_NULL_PTR(gdk_win))
  {
    GdkDisplay *display = gtk_widget_get_display(dialog);
    _preview_geometry.session_monitor = gdk_display_get_monitor_at_window(display, gdk_win);
  }

  if(_preview_geometry.is_maximized)
  {
    /* Even though position/size are meaningless while maximized, we must
     * mark the state as valid so the next spawn knows to restore maximized
     * mode on the correct monitor. */
    _preview_geometry.valid = TRUE;
    return;
  }

  gtk_window_get_position(GTK_WINDOW(dialog), &_preview_geometry.x, &_preview_geometry.y);
  gtk_window_get_size(GTK_WINDOW(dialog), &_preview_geometry.width, &_preview_geometry.height);
  _preview_geometry.valid = TRUE;
}

/* ============================================================================
 * WINDOW STATE & EVENT HANDLERS
 * ========================================================================== */

static gboolean _preview_window_state_changed(GtkWidget *dialog, GdkEventWindowState *event, gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  GdkWindow *gdk_win = gtk_widget_get_window(dialog);

  preview->maximized = (event->new_window_state & GDK_WINDOW_STATE_MAXIMIZED) != 0;
  _preview_geometry.is_maximized = preview->maximized;

  /* Update monitor identity on every state change — the window may have
   * been dragged to a different monitor before being maximized. */
  if(!IS_NULL_PTR(gdk_win))
  {
    GdkDisplay *display = gtk_widget_get_display(dialog);
    _preview_geometry.session_monitor = gdk_display_get_monitor_at_window(display, gdk_win);
  }

  return FALSE;
}

/*
 * Handles a monitor hot-plug event.
 * Re-maximizes any maximized window so the compositor/WM can re-place it if
 * required (some Wayland compositors need this after a topology change).
 */
static void _preview_window_monitor_added(GdkDisplay *display, GdkMonitor *monitor, gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  if(preview->maximized)
    gtk_window_maximize(GTK_WINDOW(preview->dialog));
}

/*
 * Handles a monitor hot-unplug event.
 *
 * CRITICAL: GdkMonitor objects are owned by GdkDisplay and are invalidated
 * (potentially freed) when a monitor is removed. The stored session_monitor
 * pointer must be nullified if it refers to the removed monitor, otherwise
 * _resolve_target_monitor() would later dereference a dangling pointer.
 *
 * Also re-maximizes affected windows so the WM/compositor re-assigns them to
 * a surviving output.
 */
static void _preview_window_monitor_removed(GdkDisplay *display, GdkMonitor *monitor, gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;

  /* Invalidate the stored monitor if it is the one being removed. After this
   * point, _resolve_target_monitor() falls through to the next available
   * monitor instead of dereferencing freed memory. */
  if(_preview_geometry.session_monitor == monitor)
    _preview_geometry.session_monitor = NULL;

  if(preview->maximized)
    gtk_window_maximize(GTK_WINDOW(preview->dialog));
}

/*
 * Configure event: fires on every resize/move.
 * We use this to keep session_monitor current and to record geometry for
 * normal (non-maximized) windows.
 */
static gboolean _preview_window_configure(GtkWidget *dialog, GdkEventConfigure *event, gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  if(preview != _last_opened_preview) return FALSE;

  GdkWindow *gdk_win = gtk_widget_get_window(dialog);
  if(!IS_NULL_PTR(gdk_win))
  {
    GdkDisplay *display = gtk_widget_get_display(dialog);
    _preview_geometry.session_monitor = gdk_display_get_monitor_at_window(display, gdk_win);
  }

  if(_preview_geometry.is_maximized) return FALSE;

  gtk_window_get_position(GTK_WINDOW(dialog), &_preview_geometry.x, &_preview_geometry.y);
  _preview_geometry.width = event->width;
  _preview_geometry.height = event->height;
  _preview_geometry.valid = TRUE;
  return FALSE;
}

/* ============================================================================
 * LIFECYCLE MANAGEMENT
 * ========================================================================== */

static void _preview_window_prepare_close(GtkWidget *dialog, dt_preview_window_t *preview)
{
  if(preview->close_prepared) return;

  if(preview == _last_opened_preview)
  {
    _preview_window_store_geometry(dialog);
    _last_opened_preview = preview->prev_preview;
    if(!IS_NULL_PTR(_last_opened_preview) && !IS_NULL_PTR(_last_opened_preview->dialog))
      gtk_window_present_with_time(GTK_WINDOW(_last_opened_preview->dialog), gtk_get_current_event_time());
  }
  else
  {
    dt_preview_window_t *current = _last_opened_preview;
    while(!IS_NULL_PTR(current) && current->prev_preview != preview)
      current = current->prev_preview;
    if(!IS_NULL_PTR(current))
      current->prev_preview = preview->prev_preview;
    if(!IS_NULL_PTR(_last_opened_preview) && !IS_NULL_PTR(_last_opened_preview->dialog))
      gtk_window_present_with_time(GTK_WINDOW(_last_opened_preview->dialog), gtk_get_current_event_time());
  }

  if(_preview_count == 1)
  {
    GtkWidget *main_window = dt_gui_main_window();
    if(!IS_NULL_PTR(main_window))
      gtk_window_present_with_time(GTK_WINDOW(main_window), gtk_get_current_event_time());
  }

  preview->close_prepared = TRUE;
}

static void _preview_window_destroy(GtkWidget *dialog, gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  _preview_window_prepare_close(dialog, preview);

  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(dt_control_signal_get_global(), G_CALLBACK(_preview_redraw), preview);

  /* Disconnect both monitor handlers separately (they were split to handle
   * the stale-pointer risk on hot-unplug). */
  GdkDisplay *display = gtk_widget_get_display(dialog);
  g_signal_handlers_disconnect_by_func(display, G_CALLBACK(_preview_window_monitor_added), preview);
  g_signal_handlers_disconnect_by_func(display, G_CALLBACK(_preview_window_monitor_removed), preview);
  dt_view_image_surface_fetcher_cleanup(&preview->fetcher);

  /* Repository convention: this dt_free() releases the GLib-allocated block
   * created by g_malloc0() in dt_preview_window_spawn(). dt_free() routes
   * through the project's tracking layer and is safe on GLib-allocated memory. */
  dt_free(preview);

  _preview_count--;
  if(_preview_count == 0)
  {
    _preview_geometry.valid = FALSE;
    _preview_geometry.is_maximized = FALSE;
    _preview_geometry.session_monitor = NULL;
    _last_opened_preview = NULL;
  }
}

static void _close_preview_popup(GtkWidget *dialog, gpointer data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)data;
  _preview_window_prepare_close(dialog, preview);
  gtk_widget_destroy(dialog);
}

static gboolean _preview_window_delete(GtkWidget *dialog, GdkEvent *event, gpointer user_data)
{
  _close_preview_popup(dialog, user_data);
  return TRUE;
}

static gboolean _preview_key_press_handler(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
  if(event->keyval == GDK_KEY_Escape)
  {
    _close_preview_popup(widget, user_data);
    return TRUE;
  }
  return FALSE;
}

/* ============================================================================
 * RENDERING
 * ========================================================================== */

static void _preview_window_size_allocate(GtkWidget *widget, GtkAllocation *allocation, gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  if(IS_NULL_PTR(preview) || IS_NULL_PTR(allocation)) return;
  if(allocation->width < 2 || allocation->height < 2) return;
  if(preview->width == allocation->width && preview->height == allocation->height) return;

  preview->width = allocation->width;
  preview->height = allocation->height;

  dt_view_image_surface_fetcher_invalidate(&preview->fetcher, &preview->surface);
  gtk_widget_queue_draw(widget);
}

static void _colormanage_ui_color(const float L, const float a, const float b, dt_aligned_pixel_t RGB)
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

  dt_aligned_pixel_t bg_color;
  _colormanage_ui_color((float)dt_conf_get_int("display/brightness"), 0.f, 0.f, bg_color);

  cairo_set_source_rgb(cr, (double)bg_color[0], (double)bg_color[1], (double)bg_color[2]);
  cairo_paint(cr);

  int borders = DT_PIXEL_APPLY_DPI(dt_conf_get_int("plugins/darkroom/ui/border_size"));
  w -= 2 * borders;
  h -= 2 * borders;

  const dt_view_surface_value_t res =
      dt_view_image_get_surface_async(&preview->fetcher, preview->imgid, w, h, &preview->surface, widget, 0);

  if(!IS_NULL_PTR(preview->surface) && res == DT_VIEW_SURFACE_OK)
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

void _preview_redraw(gpointer instance, int signal, gpointer user_data)
{
  dt_preview_window_t *preview = (dt_preview_window_t *)user_data;
  if(!IS_NULL_PTR(preview) && !IS_NULL_PTR(preview->area))
    gtk_widget_queue_draw(preview->area);
}

/* ============================================================================
 * PLATFORM-SPECIFIC WINDOW PLACEMENT
 *
 * This is the core of the cross-platform abstraction. The function resolves
 * the target monitor and then applies backend-appropriate placement logic.
 * ========================================================================== */

/*
 * Resolves the best target monitor from stored geometry.
 * Falls back through: stored monitor -> point-based lookup -> main window -> primary.
 * This logic is fully portable (gdk_display_get_monitor_at_* works on all backends).
 *
 * NOTE: session_monitor is only trusted here because _preview_window_monitor_removed
 * nullifies it the moment the underlying GdkMonitor is invalidated by GDK.
 */
static GdkMonitor *_resolve_target_monitor(GtkWidget *window, const dt_preview_window_geometry_t *geo)
{
  GdkDisplay *display = gtk_widget_get_display(window);

  /* Priority 1: The explicitly stored monitor handle from the last session.
   * This is the most reliable identifier across all platforms. */
  if(!IS_NULL_PTR(geo->session_monitor))
    return geo->session_monitor;

  /* Priority 2: Derive monitor from stored position (only meaningful on X11/Win32). */
  if(geo->valid && !_preview_backend_is_wayland())
  {
    GdkMonitor *m = gdk_display_get_monitor_at_point(display, geo->x, geo->y);
    if(!IS_NULL_PTR(m)) return m;
  }

  /* Priority 3: Use the monitor hosting the main application window. */
  GtkWidget *main_win = dt_gui_main_window();
  if(!IS_NULL_PTR(main_win) && !IS_NULL_PTR(gtk_widget_get_window(main_win)))
  {
    GdkMonitor *m = gdk_display_get_monitor_at_window(display, gtk_widget_get_window(main_win));
    if(!IS_NULL_PTR(m)) return m;
  }

  /* Priority 4: Primary monitor. */
  return gdk_display_get_primary_monitor(display);
}

/*
 * Applies window placement using a strategy appropriate for the active backend.
 *
 * Called BEFORE gtk_widget_show_all(), so the window is not yet mapped.
 * The maximize call is deferred to AFTER show_all() by the caller.
 *
 * Backend-specific behavior:
 *
 *   X11 / Win32 / Quartz:
 *     - Normal windows: absolute move + resize (fully supported).
 *     - Maximized: move center to target monitor, set size to monitor geometry,
 *       then the caller will call gtk_window_maximize() after mapping.
 *
 *   Wayland:
 *     - Normal windows: we cannot control position. We set default_size only.
 *       The compositor chooses placement. We accept this limitation.
 *     - Maximized: we move the window center to the target monitor's center.
 *       Most Wayland compositors (GNOME Shell >=42, KWin >=5.27) use the
 *       window's current position as a hint for which output to maximize on.
 *       This is not guaranteed by the protocol but is de facto behavior.
 */
static void _apply_window_placement(GtkWindow *window, const dt_preview_window_geometry_t *geo)
{
  GdkMonitor *target_monitor = _resolve_target_monitor(GTK_WIDGET(window), geo);
  if(IS_NULL_PTR(target_monitor)) return;

  GdkRectangle geometry;
  gdk_monitor_get_geometry(target_monitor, &geometry);

  const gboolean is_wayland = _preview_backend_is_wayland();

  if(geo->valid && geo->is_maximized)
  {
    /* --- MAXIMIZED RESTORATION --- */

    /* Both backends nudge the center onto the target monitor: on X11/Win32/
     * Quartz this guarantees the WM maximizes on the right output; on Wayland
     * it is the only available placement hint before the maximize request. */
    const int cx = geometry.x + geometry.width / 2;
    const int cy = geometry.y + geometry.height / 2;
    gtk_window_move(window, cx, cy);
    gtk_window_set_default_size(window, geometry.width, geometry.height);
    /* NOTE: gtk_window_maximize() is called by the caller AFTER show_all(),
     * because maximizing before mapping can cause the WM to ignore the
     * monitor assignment on some backends (particularly X11 WMs). */
  }
  else if(geo->valid)
  {
    /* --- NORMAL (RESTORED) WINDOW --- */

    if(is_wayland)
    {
      /* Wayland: gtk_window_move() is a no-op for top-level surfaces.
       * We can only control the size. The compositor decides placement. */
      gtk_window_set_default_size(window, geo->width, geo->height);
    }
    else
    {
      /* X11 / Win32 / Quartz: full absolute placement. */
      gtk_window_move(window, geo->x, geo->y);
      gtk_window_set_default_size(window, geo->width, geo->height);
    }
  }
  else
  {
    /* --- FIRST LAUNCH / NO STORED STATE --- */

    const int def_w = DT_PIXEL_APPLY_DPI(420);
    const int def_h = DT_PIXEL_APPLY_DPI(420);

    if(is_wayland)
    {
      /* Let the compositor place the window; just set a reasonable size. */
      gtk_window_set_default_size(window, def_w, def_h);
    }
    else
    {
      /* Center on the target monitor. */
      const int center_x = geometry.x + (geometry.width - def_w) / 2;
      const int center_y = geometry.y + (geometry.height - def_h) / 2;
      gtk_window_move(window, center_x, center_y);
      gtk_window_set_default_size(window, def_w, def_h);
    }
  }
}

/* ============================================================================
 * SPAWN
 * ========================================================================== */

void dt_preview_window_spawn(const int32_t imgid)
{
  const dt_image_t *img = dt_image_cache_get(imgid, 'r');
  if(IS_NULL_PTR(img))
  {
    dt_control_log(("No image to open !"));
    return;
  }

  gchar *name = g_strdup_printf(("Ansel - Preview : %s [%d]"), img->filename, img->version);
  dt_image_cache_read_release(img);

  /* Repository convention: g_malloc0() returns zero-initialized memory and is
   * paired with dt_free() in _preview_window_destroy(). */
  dt_preview_window_t *preview = g_malloc0(sizeof(dt_preview_window_t));
  preview->imgid = imgid;
  preview->prev_preview = _last_opened_preview;
  dt_view_image_surface_fetcher_init(&preview->fetcher);
  _preview_count++;
  _last_opened_preview = preview;

  GtkWidget *dialog = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  preview->dialog = dialog;
  gtk_window_set_title(GTK_WINDOW(dialog), name);
  dt_free(name);

  /* --- PLATFORM-SPECIFIC WINDOW HINTS --- */

#ifdef GDK_WINDOWING_QUARTZ
  /* macOS: prevent native fullscreen mode (green button) which would
   * conflict with our own maximize logic and break monitor targeting. */
  dt_osx_disallow_fullscreen(dialog);
  /* On macOS, CENTER_ON_PARENT gives the best initial placement. */
  gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ON_PARENT);
#endif

  gtk_window_set_modal(GTK_WINDOW(dialog), FALSE);
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(dt_gui_main_window()));
  gtk_window_set_type_hint(GTK_WINDOW(dialog), GDK_WINDOW_TYPE_HINT_NORMAL);
  gtk_window_set_decorated(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_resizable(GTK_WINDOW(dialog), TRUE);
  g_object_set(G_OBJECT(dialog), "accept-focus", TRUE, NULL);

  /* Apply cross-platform placement strategy. */
  _apply_window_placement(GTK_WINDOW(dialog), &_preview_geometry);

  /* --- SIGNAL CONNECTIONS --- */
  g_signal_connect(G_OBJECT(dialog), "delete-event", G_CALLBACK(_preview_window_delete), preview);
  g_signal_connect(G_OBJECT(dialog), "destroy", G_CALLBACK(_preview_window_destroy), preview);
  g_signal_connect(G_OBJECT(dialog), "window-state-event", G_CALLBACK(_preview_window_state_changed), preview);
  g_signal_connect(G_OBJECT(dialog), "configure-event", G_CALLBACK(_preview_window_configure), preview);
  g_signal_connect(G_OBJECT(dialog), "key-press-event", G_CALLBACK(_preview_key_press_handler), preview);

  GdkDisplay *display = gtk_widget_get_display(dialog);
  /* Split handlers: added re-maximizes, removed nullifies the stale monitor pointer. */
  g_signal_connect(display, "monitor-added", G_CALLBACK(_preview_window_monitor_added), preview);
  g_signal_connect(display, "monitor-removed", G_CALLBACK(_preview_window_monitor_removed), preview);

  /* --- WIDGET CONSTRUCTION --- */
  GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(dialog), main_box);

  preview->area = gtk_drawing_area_new();
  gtk_widget_set_hexpand(preview->area, TRUE);
  gtk_widget_set_vexpand(preview->area, TRUE);
  gtk_widget_set_halign(preview->area, GTK_ALIGN_FILL);
  gtk_widget_set_valign(preview->area, GTK_ALIGN_FILL);
  gtk_widget_set_size_request(preview->area, DT_PIXEL_APPLY_DPI(420), DT_PIXEL_APPLY_DPI(420));
  gtk_box_pack_start(GTK_BOX(main_box), preview->area, TRUE, TRUE, 0);

  g_signal_connect(G_OBJECT(preview->area), "draw", G_CALLBACK(_thumb_draw_image), preview);
  g_signal_connect(G_OBJECT(preview->area), "size-allocate", G_CALLBACK(_preview_window_size_allocate), preview);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_DARKROOM_UI_CHANGED,
                                  G_CALLBACK(_preview_redraw), preview);
  gtk_widget_set_visible(preview->area, TRUE);

  /* --- MAP & FINALIZE PLACEMENT --- */
  gtk_widget_show_all(dialog);

  /* After mapping, update the monitor identity from the realized window.
   * This is critical because the WM/compositor may have overridden our
   * placement request (common on Wayland and some X11 WMs). */
  GdkWindow *gdk_win = gtk_widget_get_window(dialog);
  if(!IS_NULL_PTR(gdk_win))
  {
    /* Record decorations (portable across all backends via GDK). */
    gdk_window_set_decorations(gdk_win,
        GDK_DECOR_BORDER | GDK_DECOR_TITLE | GDK_DECOR_RESIZEH | GDK_DECOR_MENU | GDK_DECOR_MAXIMIZE);

    /* If we had no prior monitor info, derive it from the realized window. */
    if(IS_NULL_PTR(_preview_geometry.session_monitor))
      _preview_geometry.session_monitor = gdk_display_get_monitor_at_window(display, gdk_win);
  }
  gdk_display_flush(display);

  /* Apply maximize AFTER mapping.
   * Rationale: On X11, maximizing before the window is mapped causes the WM
   * to assign it to an arbitrary monitor. On Wayland, the compositor needs
   * the surface to exist before it can process the maximize request.
   * On macOS, Cocoa requires the NSWindow to be visible before zoom/maximize. */
  if(_preview_geometry.valid && _preview_geometry.is_maximized)
  {
    gtk_window_maximize(GTK_WINDOW(dialog));
  }

  gtk_window_present(GTK_WINDOW(dialog));
}