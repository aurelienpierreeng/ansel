/*
 *    This file is part of Ansel,
 *    Copyright (C) 2026 Aurélien PIERRE.
 *
 *    Ansel is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    Ansel is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with Ansel.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef DT_WIDGETS_WIDGET_SETTINGS_H
#define DT_WIDGETS_WIDGET_SETTINGS_H

/* Only what the declarations below need: GtkWidget/GdkRGBA/cairo_t (gtk.h), pthread_t, and
 * va_list. This header used to include gui/screen_metrics.h purely to re-export
 * dt_cairo_image_surface_*() to ~45 consumers that never asked it for them -- a supply line
 * nobody declared and nobody could see, which breaks somewhere unrelated the day it is
 * tidied. Those files include screen_metrics.h themselves now. */
#include <gtk/gtk.h>
#include <pthread.h>
#include <stdarg.h>

G_BEGIN_DECLS

/* Toolkit-wide state that widgets need and the application merely configures.
 *
 * These three used to be fields of dt_gui_gtk_t, which meant a widget had to reach the
 * application global to read its own event mask. They are not application data -- a scroll
 * mask, a "who owns scroll right now" register and a cairo filter are properties of the
 * widget toolkit. Ownership moves here so widgets/ needs nothing from gui/.
 *
 * The application sets them once during GUI init; everything else reads them.
 */

/** Event mask widgets must add to receive scroll events. Set once at GUI init. */
GdkEventMask dt_widget_scroll_mask(void);
void dt_widget_set_scroll_mask(GdkEventMask mask);

/* Which widget currently owns scroll input.
 *
 * Ansel routes scroll to one widget at a time rather than letting it propagate: a slider
 * under the pointer takes the wheel, and views clear the register when they change. */
GtkWidget *dt_widget_scroll_focus(void);
void dt_widget_set_scroll_focus(GtkWidget *widget);

/** Cairo filter used when scaling images outside the darkroom. Set once at GUI init. */
cairo_filter_t dt_widget_image_filter(void);
void dt_widget_set_image_filter(cairo_filter_t filter);


/* Widget-update suppression.
 *
 * Programmatic widget updates must not be mistaken for user input. Code wraps such updates
 * in dt_gui_freeze_begin()/end() and every widget callback opens with
 * `if(dt_gui_widgets_suppressed()) return;`.
 *
 * The depth counter used to live in dt_gui_gtk_t, which meant a widget had to reach the
 * application global to find out whether it should ignore its own callback. */
gboolean dt_gui_widgets_suppressed(void);

/** Register the thread that owns widget state. Freeze/unfreeze is a deliberate no-op on any
 *  other thread -- worker-thread reload_defaults has no widgets to suppress, and a concurrent
 *  non-atomic ++/-- would drift the depth and break suppression for the GUI thread.
 *  Until this is called, freezing is inert. */
void dt_widget_set_gui_thread(pthread_t thread);

/** @brief Is the calling thread the one that owns widget state?
 *
 * FALSE before dt_widget_set_gui_thread() has run, and in every headless process, which is
 * the safe answer for both users: a caller that must reach GTK defers instead of calling it,
 * and a caller that merely wants to skip work skips it.
 *
 * GTK is not thread-safe and nothing in it says so at the call site, so anything reached from
 * a job, a pipeline callback or an import worker has to ask before touching a widget. */
gboolean dt_widget_on_gui_thread(void);
void dt_gui_freeze_begin_(const char *file, int line);
void dt_gui_freeze_end_(const char *file, int line);
void dt_gui_freeze_reset(void); // hard-reset depth to 0 (GUI init only)

/* Bracket programmatic widget updates with these so the widget's own "value-changed" handler
 * does not mistake them for user input. The scope-guard form ends the freeze automatically on
 * every exit path, including an early return, which the raw pair leaks. */
#define dt_gui_freeze_begin() dt_gui_freeze_begin_(__FILE__, __LINE__)
#define dt_gui_freeze_end()   dt_gui_freeze_end_(__FILE__, __LINE__)

typedef struct { const char *file; int line; } dt_gui_freeze_token_t;
static inline void dt_gui_freeze_release_(dt_gui_freeze_token_t *t)
{
  dt_gui_freeze_end_(t->file, t->line);
}
#define DT_FREEZE_CAT_(a, b) a##b
#define DT_FREEZE_CAT(a, b) DT_FREEZE_CAT_(a, b)
#define dt_gui_widget_freeze()                                                       \
  dt_gui_freeze_token_t DT_FREEZE_CAT(_dt_freeze_guard_, __LINE__)                    \
      __attribute__((cleanup(dt_gui_freeze_release_))) = { __FILE__, __LINE__ };      \
  dt_gui_freeze_begin_(__FILE__, __LINE__)

/* Scroll deltas in discrete units, accumulating smooth-scroll fractions and discarding
 * pointer-emulated duplicates. Pure GTK event arithmetic. */
gboolean dt_gui_get_scroll_unit_deltas(const GdkEventScroll *event, int *delta_x, int *delta_y);
gboolean dt_gui_get_scroll_unit_delta(const GdkEventScroll *event, int *delta);

/** Whether each scroll axis is inverted. A user preference, supplied by the application --
 *  a widget does not read configuration. */
void dt_widget_set_scroll_reversed(gboolean reverse_x, gboolean reverse_y);

/* The host's root window, used for things that exist before any widget does: resolving theme
 * colours at toolkit init, and parenting a popup so Wayland compositors place it correctly.
 * Unregistered: NULL, and callers fall back to screen defaults. */
typedef GtkWidget *(*dt_widget_root_window_handler_t)(void);
void dt_widget_set_root_window_handler(dt_widget_root_window_handler_t handler);
GtkWidget *dt_widget_root_window(void);

/* How wide the host would like `widget` to be naturally -- it knows which panel the widget
 * sits in and how wide that panel is. Returns -1 when the host has no opinion, which is also
 * what an unregistered handler reports. */
typedef gint (*dt_widget_natural_width_handler_t)(GtkWidget *widget);
void dt_widget_set_natural_width_handler(dt_widget_natural_width_handler_t handler);
gint dt_widget_natural_width(GtkWidget *widget);

/* Transient user-facing message ("that widget no longer exists"). How and where it is shown
 * is the host's decision -- a toast, a status line, or nothing at all. Unregistered, the
 * message is dropped. */
typedef void (*dt_widget_message_handler_t)(const char *message);
void dt_widget_set_message_handler(dt_widget_message_handler_t handler);
void dt_widget_message(const char *message);

/* Pointer shape during widget interaction (a panel-handle drag wants a resize cursor).
 * The host owns the window whose cursor changes, so it supplies the setter. Unregistered,
 * the cursor is left alone. */
typedef void (*dt_widget_cursor_handler_t)(GdkCursorType cursor);
void dt_widget_set_cursor_handler(dt_widget_cursor_handler_t handler);
void dt_widget_set_cursor(GdkCursorType cursor);

/* ------------------------------------------------------------------------------------------
 * Diagnostics.
 *
 * A debug build prints straight to stdout; a release build compiles the call away. widgets/
 * deliberately does not route diagnostics through the application: a logging system means a
 * global flags word and a global stream, and reaching for either is what this module exists to
 * avoid. Nothing is lost by keeping it local -- these messages describe widget internals, and
 * whoever is reading them is running a debug build anyway.
 * ------------------------------------------------------------------------------------------ */
#ifdef _DEBUG
void dt_widget_log(const char *format, ...) G_GNUC_PRINTF(1, 2);
#define dt_widget_log_enabled() TRUE
#else
#define dt_widget_log(...) do { } while(0)
#define dt_widget_log_enabled() FALSE
#endif

/** Whether to draw diagnostic overlays (hit-test radii and the like) on top of normal
 *  rendering. A debugging aid the host switches on; off by default. */
gboolean dt_widget_debug_overlays(void);
void dt_widget_set_debug_overlays(gboolean enabled);

/* ------------------------------------------------------------------------------------------
 * Per-widget persistence.
 *
 * A resizable panel remembers the height the user dragged it to; a collapsible section
 * remembers whether they left it open. The key is supplied by whoever built the widget --
 * widgets/ neither invents keys nor knows where they are stored, because a configuration
 * system is application state reached through a global, which is exactly what this module
 * exists to keep out of the toolkit.
 *
 * Unregistered, nothing is stored and every read reports "not set", so a widget falls back to
 * its default size or its collapsed state. That is also the correct behaviour for any host
 * that has no preferences to offer.
 * ------------------------------------------------------------------------------------------ */
typedef gboolean (*dt_widget_stored_int_getter_t)(const char *key, int *value);
typedef void (*dt_widget_stored_int_setter_t)(const char *key, int value);
typedef gboolean (*dt_widget_stored_bool_getter_t)(const char *key);
typedef void (*dt_widget_stored_bool_setter_t)(const char *key, gboolean value);

void dt_widget_set_storage_handlers(dt_widget_stored_int_getter_t get_int,
                                    dt_widget_stored_int_setter_t set_int,
                                    dt_widget_stored_bool_getter_t get_bool,
                                    dt_widget_stored_bool_setter_t set_bool);

/** Read a stored integer. Returns FALSE (leaving @p value untouched) if nothing is stored. */
gboolean dt_widget_stored_int(const char *key, int *value);
void dt_widget_store_int(const char *key, int value);

/** Read a stored flag. FALSE when nothing is stored, which is the collapsed/default state. */
gboolean dt_widget_stored_bool(const char *key);
void dt_widget_store_bool(const char *key, gboolean value);

/* Has the application loaded its CSS theme yet? Dialogs that can run during startup -- before
 * any styling exists -- pad themselves by hand when it has not. */
gboolean dt_widget_theme_loaded(void);
void dt_widget_set_theme_loaded(gboolean loaded);

/* Return keyboard focus to the application's main working area. A widget that swallows keys
 * (a text entry) has to hand focus back when the user presses Escape, but which widget is
 * "the main area" is the host's business -- the image in darkroom, the grid in lighttable.
 * Unregistered, the request is dropped. */
typedef void (*dt_widget_refocus_handler_t)(void);
void dt_widget_set_refocus_handler(dt_widget_refocus_handler_t handler);
void dt_widget_refocus(void);

/* A GtkNotebook the host registered an owner for has switched page. The host relays this on
 * its own signal bus; widgets/ has no bus and no idea what the owner is. */
typedef void (*dt_widget_notebook_page_handler_t)(gpointer owner);
void dt_widget_set_notebook_page_handler(dt_widget_notebook_page_handler_t handler);
void dt_widget_notebook_page_changed(gpointer owner);

/* Toolkit metrics: the UI zoom factor, the integer device scale, and the resolved root font
 * size in pixels. Widgets scale themselves by these; the application computes them from the
 * screen and the theme and pushes them here. They lived in dt_gui_gtk_t, which meant a widget
 * had to reach the application global to size itself. */
/** Screen resolution in dots per inch; 96.0 (the X/GDK default) until resolved. */
double dt_widget_dpi(void);
void dt_widget_set_dpi(double dpi);

/** TRUE once something has actually interrogated a display. The getters are always safe to
 *  call, but answer with neutral defaults until then -- use this only where reporting an
 *  invented 96dpi would be worse than reporting nothing (a crash report, a telemetry
 *  payload), never as an "is there a GUI?" test for scaling. */
gboolean dt_widget_metrics_probed(void);

double dt_widget_dpi_factor(void);
void dt_widget_set_dpi_factor(double factor);

double dt_widget_ppd(void);
void dt_widget_set_ppd(double ppd);

/** Minimum width a side panel may shrink to, in logical px. A user preference the
 *  application supplies; the panel widget reads it when its class is initialised. 350 until
 *  the application says otherwise. */
gint dt_widget_min_panel_width(void);
void dt_widget_set_min_panel_width(gint width);

/** Resolved root font size in px; 16.0 until the application resolves it. */
double dt_widget_em_size(void);
void dt_widget_set_em_size(double em);

/** The device scale GTK reports for the monitor `widget` is on, i.e. how many device pixels
 *  it puts in a logical one. Probed from the toolkit, not from our own settings. */
double dt_get_system_gui_ppd(GtkWidget *widget);

/** Modifier keys currently held, independent of any key event. */
GdkModifierType dt_key_modifier_state(void);

/* Scale a 96-DPI-baseline value. UI: logical pixels GTK will scale further. DEVICE: raw
 * device pixels for cairo surfaces and hit-tests, which GTK does not scale for us. */
#define DT_UI_SCALE_UI(value) ((value) * dt_widget_dpi_factor())
#define DT_UI_SCALE_DEVICE(value) ((value) * dt_widget_dpi_factor() * dt_widget_ppd())
#define DT_PIXEL_APPLY_DPI(value) DT_UI_SCALE_UI(value)
#define DT_PIXEL_APPLY_DPI_DPP(value) DT_UI_SCALE_DEVICE(value)

/* Gutter between children of boxes/grids/flowboxes -- settable only from code, so it is
 * centralised here for the whole app. Expressed as a fraction of 1em so it tracks the user's
 * font size like the em-based margins in ansel.css. 0.625em == 10px at the 16px reference.
 * The font's pt->px conversion already folds in DPI, so this needs no DPI scaling on top. */
#define DT_GUI_EM_SIZE ((gint)dt_widget_em_size())
#define DT_GUI_BOX_SPACING_EM 0.625
#define DT_GUI_BOX_SPACING ((gint)(DT_GUI_EM_SIZE * DT_GUI_BOX_SPACING_EM + 0.5))

/* Colour-label slots. These mirror the application's dt_colorlabels_enum, and gui/gtk.c
 * carries a _Static_assert that they cannot drift apart -- that is the one place both
 * headers are visible. Declaring them here keeps widgets/ free of application headers. */
enum
{
  DT_WIDGET_COLORLABEL_RED = 0,
  DT_WIDGET_COLORLABEL_YELLOW,
  DT_WIDGET_COLORLABEL_GREEN,
  DT_WIDGET_COLORLABEL_BLUE,
  DT_WIDGET_COLORLABEL_PURPLE,
  DT_WIDGET_COLORLABEL_COUNT
};

/* The colour-label palette, as RGBA. Widgets paint colour labels; which colours those are is
 * a theme decision the application supplies. Indices match dt_colorlabels_enum. */
const GdkRGBA *dt_widget_colorlabel(int index);
void dt_widget_set_colorlabels(const GdkRGBA *labels, int count);

/* On the Quartz GDK backend, a physical Cmd keypress is reported through GDK_MOD2_MASK
 * (see widgets/accelerators.h's DT_PRIMARY_MASK, which every accelerator *registration*
 * already resolves to instead of GDK_CONTROL_MASK). Every mouse/keyboard interaction check
 * in the codebase asks for GDK_CONTROL_MASK meaning "the platform's primary modifier", so
 * that substitution is done once, here, rather than at each of the ~80 call sites. */
static inline GdkModifierType dt_modifier_primary_mask(GdkModifierType desired_modifier_mask)
{
#ifdef GDK_WINDOWING_QUARTZ
  if(desired_modifier_mask & GDK_CONTROL_MASK)
    desired_modifier_mask = (GdkModifierType)((desired_modifier_mask & ~GDK_CONTROL_MASK) | GDK_MOD2_MASK);
#endif
  return desired_modifier_mask;
}

/* GDK_MOD2_MASK is the bit a real Cmd keypress produces, matched against for actual dispatch
 * (dt_modifier_primary_mask() above) and the bit GTK's own gtk_accelerator_name() collapses to
 * the portable "<Primary>" token for on Quartz (see widgets/accelerators.h's DT_PRIMARY_MASK
 * doc comment) -- so it must stay what is stored and saved. But GTK's accelerator-label
 * renderer (gtk_accelerator_get_label(), and anything built on it: GtkCellRendererAccel,
 * GtkAccelLabel) only ever emits the "⌘" glyph for GDK_META_MASK, never GDK_MOD2_MASK, on any
 * platform. This is the reverse, display-only swap: takes a mask meant for storage/matching
 * and returns what to hand to a label/rendering function instead. Never store or match against
 * this result -- GDK_META_MASK is not reliably present on mouse-sourced events, only real key
 * events (see dt_modifier_is() above). */
static inline GdkModifierType dt_accels_display_mods(GdkModifierType mods)
{
#ifdef GDK_WINDOWING_QUARTZ
  if(mods & GDK_MOD2_MASK)
    mods = (GdkModifierType)((mods & ~GDK_MOD2_MASK) | GDK_META_MASK);
#endif
  return mods;
}

/* Does `state` carry exactly `desired_modifier_mask`, ignoring lock/scroll bits? */
static inline gboolean dt_modifier_is(GdkModifierType state, const GdkModifierType desired_modifier_mask)
{
  const GdkModifierType modifiers = gtk_accelerator_get_default_mod_mask();
#ifdef GDK_WINDOWING_QUARTZ
  // A real GDK_KEY_PRESS/RELEASE's state carries both GDK_MOD2_MASK and GDK_META_MASK for a
  // single physical Cmd press (GDK's own virtual-modifier step adds GDK_META_MASK next to the
  // real GDK_MOD2_MASK bit for every key event -- see _accels_keys_decode() in
  // widgets/accelerators.c for the same dedup on the accelerator-dispatch path). Mouse/scroll
  // events never carry GDK_META_MASK, so this is a no-op for the majority of this function's
  // callers; it only matters for the callers that read a real GdkEventKey.state directly.
  /* Cast, not a compound assignment: this header is included from C++ translation units
   * too (iop/lens.cc, iop/tonemap.cc, imageio/format/exr.cc, ...), and in C++ the operand
   * `~GDK_META_MASK` is an int, which cannot be assigned back into an enum. */
  if((state & GDK_MOD2_MASK) && (state & GDK_META_MASK))
    state = (GdkModifierType)(state & ~GDK_META_MASK);
#endif
  return (state & modifiers) == dt_modifier_primary_mask(desired_modifier_mask);
}

/* Does `state` carry AT LEAST `desired_modifier_mask`? Same arithmetic, weaker test. */
static inline gboolean dt_modifiers_include(GdkModifierType state, const GdkModifierType desired_modifier_mask)
{
  const GdkModifierType modifiers = gtk_accelerator_get_default_mod_mask();
  const GdkModifierType wanted = dt_modifier_primary_mask(desired_modifier_mask);
#ifdef GDK_WINDOWING_QUARTZ
  // See dt_modifier_is() just above, cast included.
  if((state & GDK_MOD2_MASK) && (state & GDK_META_MASK))
    state = (GdkModifierType)(state & ~GDK_META_MASK);
#endif
  return (state & (modifiers & wanted)) == wanted;
}

/* Scroll deltas as fractions, for consumers that want the raw smooth-scroll amount rather
 * than the accumulated discrete units above. */
gboolean dt_gui_get_scroll_deltas(const GdkEventScroll *event, gdouble *delta_x, gdouble *delta_y);
gboolean dt_gui_get_scroll_delta(const GdkEventScroll *event, gdouble *delta);


/* ------------------------------------------------------------------------------------------
 * Theme palette.
 *
 * The colours widgets paint with are a theme decision the application resolves (from CSS, at
 * dt_gui_load_theme() time) and pushes here. Widgets read them; they do not look them up.
 * Storage lives here rather than in the application struct so that widgets/draw.h can be a
 * leaf header -- it paints with DT_GUI_COLOR_BUTTON_FG and must not reach for gui/.
 * ------------------------------------------------------------------------------------------ */
typedef enum dt_gui_color_t
{
  DT_GUI_COLOR_BG = 0,
  DT_GUI_COLOR_DARKROOM_BG,
  DT_GUI_COLOR_DARKROOM_PREVIEW_BG,
  DT_GUI_COLOR_LIGHTTABLE_BG,
  DT_GUI_COLOR_LIGHTTABLE_PREVIEW_BG,
  DT_GUI_COLOR_LIGHTTABLE_FONT,
  DT_GUI_COLOR_PRINT_BG,
  DT_GUI_COLOR_BRUSH_CURSOR,
  DT_GUI_COLOR_BRUSH_TRACE,
  DT_GUI_COLOR_BUTTON_FG,
  DT_GUI_COLOR_THUMBNAIL_BG,
  DT_GUI_COLOR_THUMBNAIL_SELECTED_BG,
  DT_GUI_COLOR_THUMBNAIL_HOVER_BG,
  DT_GUI_COLOR_THUMBNAIL_OUTLINE,
  DT_GUI_COLOR_THUMBNAIL_SELECTED_OUTLINE,
  DT_GUI_COLOR_THUMBNAIL_HOVER_OUTLINE,
  DT_GUI_COLOR_THUMBNAIL_FONT,
  DT_GUI_COLOR_THUMBNAIL_SELECTED_FONT,
  DT_GUI_COLOR_THUMBNAIL_HOVER_FONT,
  DT_GUI_COLOR_THUMBNAIL_BORDER,
  DT_GUI_COLOR_THUMBNAIL_SELECTED_BORDER,
  DT_GUI_COLOR_FILMSTRIP_BG,
  DT_GUI_COLOR_PREVIEW_HOVER_BORDER,
  DT_GUI_COLOR_LOG_BG,
  DT_GUI_COLOR_LOG_FG,
  DT_GUI_COLOR_MAP_COUNT_SAME_LOC,
  DT_GUI_COLOR_MAP_COUNT_DIFF_LOC,
  DT_GUI_COLOR_MAP_COUNT_BG,
  DT_GUI_COLOR_MAP_LOC_SHAPE_HIGH,
  DT_GUI_COLOR_MAP_LOC_SHAPE_LOW,
  DT_GUI_COLOR_MAP_LOC_SHAPE_DEF,
  DT_GUI_COLOR_WARNING,
  DT_GUI_COLOR_LAST
} dt_gui_color_t;

/** The palette itself, writable so the theme loader can fill it in place. Zeroed until then. */
GdkRGBA *dt_widget_colors(void);

/** Set a cairo source from the palette, opaque or scaled by the palette entry's own alpha. */
void dt_widget_set_source_rgb(cairo_t *cr, dt_gui_color_t color);
void dt_widget_set_source_rgba(cairo_t *cr, dt_gui_color_t color, float opacity_coef);

/* Overlay tint for shapes drawn over the image (mask outlines, guides, crop handles). A user
 * preference the application resolves; widgets/draw.h paints with it. */
typedef struct dt_widget_overlay_color_t
{
  double red, green, blue, contrast;
} dt_widget_overlay_color_t;

const dt_widget_overlay_color_t *dt_widget_overlay_color(void);
void dt_widget_set_overlay_color(double red, double green, double blue, double contrast);

/* Mouse hit-test radius in device pixels: the raw value, and the one clamped to stay usable
 * for overlay selection at any zoom. The darkroom recomputes both when the zoom changes. */
float dt_widget_mouse_radius(void);
float dt_widget_mouse_radius_clamped(void);
void dt_widget_set_mouse_radius(float radius, float clamped);

/** Mouse hit-test radius in darkroom image space, clamped for usable overlay selection. */
#define DT_GUI_MOUSE_EFFECT_RADIUS dt_widget_mouse_radius_clamped()


/* ------------------------------------------------------------------------------------------
 * Call-site diagnostics for two GTK setters.
 *
 * GTK reports only its own assertion site when a non-widget reaches gtk_widget_queue_draw(),
 * which says nothing about which Ansel code owned the bad pointer. In debug-capable builds
 * both calls are rerouted through a wrapper that names the caller's file and line, so an
 * ownership/lifetime bug points at the source line that queued the redraw. Toggle state
 * changes are wrapped for the same reason: they usually precede a redraw, so catching the
 * invalid object here surfaces the first error rather than the secondary redraw assertion.
 * ------------------------------------------------------------------------------------------ */
#ifdef _DEBUG
void dt_gtk_widget_queue_draw_ext(GtkWidget *widget, const char *name, const char *file, const int line);
#define dt_gtk_widget_queue_draw(widget) dt_gtk_widget_queue_draw_ext((GtkWidget *)(widget), #widget, __FILE__, __LINE__)
#define gtk_widget_queue_draw(widget) dt_gtk_widget_queue_draw(widget)

void dt_gtk_toggle_button_set_active_ext(GtkToggleButton *toggle_button, const char *name, const gboolean active,
                                         const char *file, const int line);
#define dt_gtk_toggle_button_set_active(toggle_button, active)                                                 \
  dt_gtk_toggle_button_set_active_ext((GtkToggleButton *)(toggle_button), #toggle_button, active, __FILE__, __LINE__)
#define gtk_toggle_button_set_active(toggle_button, active)                                                   \
  dt_gtk_toggle_button_set_active(toggle_button, active)
#else
#define dt_gtk_widget_queue_draw(widget) gtk_widget_queue_draw(widget)
#define dt_gtk_toggle_button_set_active(toggle_button, active) gtk_toggle_button_set_active(toggle_button, active)
#endif

G_END_DECLS

#endif // DT_WIDGETS_WIDGET_SETTINGS_H
