/*
    This file is part of darktable,
    Copyright (C) 2009-2014 johannes hanika.
    Copyright (C) 2010-2011, 2013 Henrik Andersson.
    Copyright (C) 2011 Robert Bieber.
    Copyright (C) 2011-2019 Tobias Ellinghaus.
    Copyright (C) 2011, 2015 Ulrich Pegelow.
    Copyright (C) 2012, 2014, 2019-2022 Aldric Renaudin.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2013, 2015, 2018-2022 Pascal Obry.
    Copyright (C) 2013-2016, 2020 Roman Lebedev.
    Copyright (C) 2013 Simon Spannagel.
    Copyright (C) 2014 Mikhail Trishchenkov.
    Copyright (C) 2014-2016, 2019 parafin.
    Copyright (C) 2015, 2017 Jérémy Rosen.
    Copyright (C) 2015 Pedro Côrte-Real.
    Copyright (C) 2016-2017 Peter Budai.
    Copyright (C) 2017-2018 Dan Torop.
    Copyright (C) 2017-2018 Matthieu Moy.
    Copyright (C) 2018 Heiko Bauke.
    Copyright (C) 2018 Rikard Öxler.
    Copyright (C) 2019-2020, 2022-2023, 2025 Aurélien PIERRE.
    Copyright (C) 2019 Kevin Daudt.
    Copyright (C) 2020 Bill Ferguson.
    Copyright (C) 2020-2022 Chris Elston.
    Copyright (C) 2020-2022 Diederik Ter Rahe.
    Copyright (C) 2020 Hanno Schwalm.
    Copyright (C) 2020 Harold le Clément de Saint-Marcq.
    Copyright (C) 2020 Hubert Kowalski.
    Copyright (C) 2020 Mark-64.
    Copyright (C) 2020-2021 Philippe Weyland.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2022 luzpaz.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Nicolas Auffray.
    Copyright (C) 2023 Luca Zulberti.
    Copyright (C) 2025 Alynx Zhou.
    Copyright (C) 2026 Guillaume Stutin.
    
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
#include "common/dtpthread.h"
#include "dtgtk/thumbtable.h"
#include "gui/window_manager.h"
#include "gui/accelerators.h"

#include <gtk/gtk.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Mouse hit-test radius in darkroom image space, clamped for usable overlay selection.
#define DT_GUI_MOUSE_EFFECT_RADIUS darktable.gui->mouse.effect_radius_clamped

/* Pixel scaling - two intents, chosen by the *destination sink* (not by platform).
 * See doc/gui.md "Pixel scaling" for the full rationale.
 *
 * DT_UI_SCALE_UI: logical-px GUI sinks (gtk_widget_set_size_request, window default
 *   size, anything fed to a GTK widget geometry setter). GTK already multiplies these
 *   by the integer scale-factor (ppd) at render time, so we must NOT pre-apply ppd here;
 *   we only add the font/UI zoom carried by dpi_factor (the X11 Xft.dpi path).
 *
 * DT_UI_SCALE_DEVICE: raw device-pixel buffers (cairo image surfaces, pixbuf-at-size,
 *   mouse hit-test radii). The toolkit does not auto-scale these, so we carry both the
 *   UI zoom (dpi_factor) and the integer scale-factor (ppd) ourselves.
 *
 * Input values are device-independent pixels at the 96 DPI baseline. */
#define DT_UI_SCALE_UI(value) ((value) * darktable.gui->dpi_factor)
#define DT_UI_SCALE_DEVICE(value) ((value) * darktable.gui->dpi_factor * darktable.gui->ppd)

/* Deprecated spellings kept so the existing call sites keep compiling. Prefer the
 * intent-named macros above in new code. */
#define DT_PIXEL_APPLY_DPI(value) DT_UI_SCALE_UI(value)
#define DT_PIXEL_APPLY_DPI_DPP(value) DT_UI_SCALE_DEVICE(value)

/* Spacing between children widgets within Gtk boxes/grids/flowboxes cannot be set from
 * CSS (margins/paddings on the children would recess the ones sitting on the container
 * edges relative to the inner ones). GTK exposes a "spacing" property for this, but only
 * from code - so it is centralized here, in ONE place, for the whole app.
 *
 * It is expressed as a fraction of 1em (the resolved root font size, cached in
 * darktable.gui->em by dt_gui_update_em()), so the inner gutters scale with the user's
 * font size exactly like the em-based margins/paddings in ansel.css. 0.625em == 10px at
 * the 16px reference font. Because the font's point->px conversion already folds in the
 * screen DPI, this needs NO DT_PIXEL_APPLY_DPI on top.
 *
 * Falls back to the 10px reference before gui->em has been resolved (early init). */
#define DT_GUI_EM_SIZE (gint)(darktable.gui->em > 0.0 ? darktable.gui->em : 16.0)
#define DT_GUI_BOX_SPACING_EM 0.625
#define DT_GUI_BOX_SPACING                                                                                     \
  ((gint)(DT_GUI_EM_SIZE * DT_GUI_BOX_SPACING_EM + 0.5))

enum
{
  TREE_LIST_MIN_ROWS = 3,
  TREE_LIST_MAX_ROWS = 11
};

typedef struct dt_gui_widgets_t
{
  /* left panel */
  GtkGrid *panel_left; // panel grid 3 rows, top,center,bottom and file on center
  GtkGrid *panel_right;
} dt_gui_widgets_t;

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
  DT_GUI_COLOR_LAST
} dt_gui_color_t;

typedef struct dt_gui_gtk_t
{

  dt_ui_t *ui;

  dt_gui_widgets_t widgets;

  cairo_surface_t *surface;
  GtkMenu *presets_popup_menu;
  char *last_preset;

  int32_t reset;
  GdkRGBA colors[DT_GUI_COLOR_LAST];

  int32_t center_tooltip; // 0 = no tooltip, 1 = new tooltip, 2 = old tooltip

  struct {
    guint timeout_source;
    struct dt_view_t *view;
    float velocity[2];
    gint64 last_time_us;
    gboolean enabled;
    gboolean block_normal_pan;
  } pan_edge;

  // Culling mode is a special case of collection filter that is restricted to user selection
  gboolean culling_mode;

  // Track if the current selection has pushed on the backup copy
  // see common/selection.h:dt_selection_push()
  gboolean selection_stacked;

  // Global accelerators for main menu, needed for GtkMenu mnemonics.
  dt_accels_t *accels;

  GList *input_devices;

  double overlay_red, overlay_blue, overlay_green, overlay_contrast;

  double dpi, dpi_factor, ppd;

  // Resolved root font size (1em) in device-independent px, read from the active
  // theme/font by dt_gui_update_em(). Drives DT_GUI_BOX_SPACING so inner gutters
  // track the font size like em-based CSS margins. 0.0 until first resolved.
  double em;


  struct {
    // Raw mouse hit-test radius in display pixels
    float effect_radius;
    // Mouse hit-test radius coordinates, clamped for usable overlay selection.
    float effect_radius_clamped;
    gboolean is_dragging;
  } mouse;

  int icon_size; // size of top panel icons

  // store which gtkrc we loaded:
  char gtkrc[PATH_MAX];

  GtkWidget *scroll_to[2]; // one for left, one for right
  GtkWidget *scroll_to_header_once; // one-shot: module expander that should scroll to its header once

  gint scroll_mask;

  // scrolling focus
  // This emulates the same feature as Gtk focus, but to capture scrolling events
  GtkWidget *has_scroll_focus;

  cairo_filter_t filter_image;    // filtering used for all modules expect darkroom
  cairo_filter_t dr_filter_image; // filtering used in the darkroom

  // Export popup window
  struct {
    GtkWidget *window;
    GtkWidget *module;
  } export_popup;
  struct {
    GtkWidget *window;
    GtkWidget *module;
  } styles_popup;

  dt_pthread_mutex_t mutex;
} dt_gui_gtk_t;

typedef struct _gui_collapsible_section_t
{
  GtkBox *parent;       // the parent widget
  const char *confname; // configuration name for the toggle status
  GtkWidget *toggle;    // toggle button
  GtkWidget *expander;  // the expanded
  GtkBox *container;    // the container for all widgets into the section
  GtkWidget *label;     // The section label
} dt_gui_collapsible_section_t;

typedef enum dt_ui_resize_mode_t
{
  // Auto-fit: the area shrinks to its content (up to the user/max height). Best for widgets
  // updated rarely; their height following content is helpful, not disruptive.
  DT_UI_RESIZE_DYNAMIC = 0,
  // Fixed: the area keeps the user-set (or default) height regardless of content, so it never
  // shifts the surrounding layout when its content changes. Best for widgets that refresh on
  // hover/selection (tags, notes, metadata) and the collection/library list.
  DT_UI_RESIZE_STATIC
} dt_ui_resize_mode_t;

typedef struct dt_gui_widget_auto_height_t
{
  char *config_str;   // conf key persisting the user-chosen height (px); owned
  int min_size;       // minimum height floor in device pixels
  int last_height;    // last applied bare (pre-padding) height, shared with the drag handle
  dt_ui_resize_mode_t mode;
  GtkTreeModel *model;
  GtkTextBuffer *buffer;
  gulong model_row_inserted;
  gulong model_row_deleted;
  gulong model_row_changed;
  gulong model_rows_reordered;
  gulong model_row_expanded;
  gulong model_row_collapsed;
  gulong buffer_changed;
} dt_gui_widget_auto_height_t;


#ifdef _DEBUG
/** \brief Queue a GTK widget redraw with the Ansel call site in diagnostics.
 *
 * GTK only reports its own assertion site when a non-widget reaches
 * gtk_widget_queue_draw(). Keep the caller location explicit in debug-capable
 * builds so invalid GUI ownership/lifetime bugs point to the Ansel source line
 * that queued the redraw.
 */
void dt_gtk_widget_queue_draw_ext(GtkWidget *widget, const char *name, const char *file, const int line);
#define dt_gtk_widget_queue_draw(widget) dt_gtk_widget_queue_draw_ext((GtkWidget *)(widget), #widget, __FILE__, __LINE__)
#define gtk_widget_queue_draw(widget) dt_gtk_widget_queue_draw(widget)

/** \brief Set a GTK toggle button state with the Ansel call site in diagnostics.
 *
 * Toggle state changes are often followed by redraws, so reporting the original
 * invalid toggle object makes the first ownership/lifetime error visible before
 * GTK emits secondary redraw assertions.
 */
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


static inline cairo_surface_t *dt_cairo_image_surface_create(cairo_format_t format, int width, int height) {
  cairo_surface_t *cst = cairo_image_surface_create(format, width * darktable.gui->ppd, height * darktable.gui->ppd);
  cairo_surface_set_device_scale(cst, darktable.gui->ppd, darktable.gui->ppd);
  return cst;
}

static inline cairo_surface_t *dt_cairo_image_surface_create_for_data(unsigned char *data, cairo_format_t format, int width, int height, int stride) {
  cairo_surface_t *cst = cairo_image_surface_create_for_data(data, format, width, height, stride);
  cairo_surface_set_device_scale(cst, darktable.gui->ppd, darktable.gui->ppd);
  return cst;
}

static inline cairo_surface_t *dt_cairo_image_surface_create_from_png(const char *filename) {
  cairo_surface_t *cst = cairo_image_surface_create_from_png(filename);
  cairo_surface_set_device_scale(cst, darktable.gui->ppd, darktable.gui->ppd);
  return cst;
}

static inline int dt_cairo_image_surface_get_width(cairo_surface_t *surface) {
  return cairo_image_surface_get_width(surface) / darktable.gui->ppd;
}

static inline int dt_cairo_image_surface_get_height(cairo_surface_t *surface) {
  return cairo_image_surface_get_height(surface) / darktable.gui->ppd;
}

static inline cairo_surface_t *dt_gdk_cairo_surface_create_from_pixbuf(const GdkPixbuf *pixbuf, int scale, GdkWindow *for_window) {
  cairo_surface_t *cst = gdk_cairo_surface_create_from_pixbuf(pixbuf, scale, for_window);
  cairo_surface_set_device_scale(cst, darktable.gui->ppd, darktable.gui->ppd);
  return cst;
}

static inline GdkPixbuf *dt_gdk_pixbuf_new_from_file_at_size(const char *filename, int width, int height, GError **error) {
  return gdk_pixbuf_new_from_file_at_size(filename, width * darktable.gui->ppd, height * darktable.gui->ppd, error);
}

// call class function to add or remove CSS classes (need to be set on top of this file as first function is used in this file)
void dt_gui_add_class(GtkWidget *widget, const gchar *class_name);
void dt_gui_remove_class(GtkWidget *widget, const gchar *class_name);

int dt_gui_gtk_init(dt_gui_gtk_t *gui);
void dt_gui_gtk_run(dt_gui_gtk_t *gui);
void dt_gui_gtk_quit();
void dt_gui_store_last_preset(const char *name);
int dt_gui_gtk_write_config();
void dt_gui_gtk_set_source_rgb(cairo_t *cr, dt_gui_color_t);
void dt_gui_gtk_set_source_rgba(cairo_t *cr, dt_gui_color_t, float opacity_coef);
double dt_get_system_gui_ppd(GtkWidget *widget);

/* Return requested scroll delta(s) from event. If delta_x or delta_y
 * is NULL, do not return that delta. Return TRUE if requested deltas
 * can be retrieved. Handles both GDK_SCROLL_UP/DOWN/LEFT/RIGHT and
 * GDK_SCROLL_SMOOTH style scroll events. */
gboolean dt_gui_get_scroll_deltas(const GdkEventScroll *event, gdouble *delta_x, gdouble *delta_y);
/* Same as above, except accumulate smooth scrolls deltas of < 1 and
 * only set deltas and return TRUE once scrolls accumulate to >= 1.
 * Effectively makes smooth scroll events act like old-style unit
 * scroll events. */
gboolean dt_gui_get_scroll_unit_deltas(const GdkEventScroll *event, int *delta_x, int *delta_y);

/* Note that on macOS Shift+vertical scroll can be reported as Shift+horizontal scroll.
 * So if Shift changes scrolling effect, both scrolls should be handled the same.
 * For this case (or if it's otherwise useful) use the following 2 functions. */

/* Return sum of scroll deltas from event. Return TRUE if any deltas
 * can be retrieved. Handles both GDK_SCROLL_UP/DOWN/LEFT/RIGHT and
 * GDK_SCROLL_SMOOTH style scroll events. */
gboolean dt_gui_get_scroll_delta(const GdkEventScroll *event, gdouble *delta);
/* Same as above, except accumulate smooth scrolls deltas of < 1 and
 * only set delta and return TRUE once scrolls accumulate to >= 1.
 * Effectively makes smooth scroll events act like old-style unit
 * scroll events. */
gboolean dt_gui_get_scroll_unit_delta(const GdkEventScroll *event, int *delta);

/** \brief gives a widget focus in the container */
void dt_ui_container_focus_widget(dt_ui_t *ui, const dt_ui_container_t c, GtkWidget *w);
/** \brief calls a callback on all children widgets from container */
void dt_ui_container_foreach(dt_ui_t *ui, const dt_ui_container_t c, GtkCallback callback);
/** \brief destroy all child widgets from container */
void dt_ui_container_destroy_children(dt_ui_t *ui, const dt_ui_container_t c);
/** \brief shows/hide a panel */
void dt_ui_panel_show(dt_ui_t *ui, const dt_ui_panel_t, gboolean show, gboolean write);
/** \brief toggle view of panels eg. collapse/expands to previous view state */
void dt_ui_toggle_panels_visibility(dt_ui_t *ui);
/** \brief draw user's attention */
void dt_ui_notify_user();
/** \brief get visible state of panel */
gboolean dt_ui_panel_visible(dt_ui_t *ui, const dt_ui_panel_t);
/**  \brief get width of right, left, or bottom panel */
int dt_ui_panel_get_size(dt_ui_t *ui, const dt_ui_panel_t p);
/** \brief is the panel ancestor of widget */
gboolean dt_ui_panel_ancestor(dt_ui_t *ui, const dt_ui_panel_t p, GtkWidget *w);
/** \brief get the center drawable widget */
GtkWidget *dt_ui_center(dt_ui_t *ui);
GtkWidget *dt_ui_center_base(dt_ui_t *ui);
/** \brief get the main window widget */
GtkWidget *dt_ui_main_window(dt_ui_t *ui);

/** \brief get the log message widget */
GtkWidget *dt_ui_log_msg(dt_ui_t *ui);
/** \brief get the toast message widget */
GtkWidget *dt_ui_toast_msg(dt_ui_t *ui);

GtkBox *dt_ui_get_container(dt_ui_t *ui, const dt_ui_container_t c);

/*  activate ellipsization of the combox entries */
void dt_ellipsize_combo(GtkComboBox *cbox);

// capitalize strings. Because grammar says sentences start with a capital,
// and typography says it makes it easier to extract the structure of the text.
void dt_capitalize_label(gchar *text);

#define dt_accels_new_global_action(a, b, c, d, e, f, g) dt_accels_new_action_shortcut(darktable.gui->accels, a, b, darktable.gui->accels->global_accels, c, d, e, f, FALSE, g)

#define dt_accels_new_darkroom_action(a, b, c, d, e, f, g) dt_accels_new_action_shortcut(darktable.gui->accels, a, b, darktable.gui->accels->darkroom_accels, c, d, e, f, FALSE, g)

#define dt_accels_new_lighttable_action(a, b, c, d, e, f, g) dt_accels_new_action_shortcut(darktable.gui->accels, a, b, darktable.gui->accels->lighttable_accels, c, d, e, f, FALSE, g)

#define dt_accels_new_map_action(a, b, c, d, e, f, g) dt_accels_new_action_shortcut(darktable.gui->accels, a, b, darktable.gui->accels->map_accels, c, d, e, f, FALSE, g)

#define dt_accels_new_print_action(a, b, c, d, e, f, g) dt_accels_new_action_shortcut(darktable.gui->accels, a, b, darktable.gui->accels->print_accels, c, d, e, f, FALSE, g)

#define dt_accels_new_slideshow_action(a, b, c, d, e, f, g) dt_accels_new_action_shortcut(darktable.gui->accels, a, b, darktable.gui->accels->slideshow_accels, c, d, e, f, FALSE, g)

#define dt_accels_new_darkroom_locked_action(a, b, c, d, e, f, g) dt_accels_new_action_shortcut(darktable.gui->accels, a, b, darktable.gui->accels->darkroom_accels, c, d, e, f, TRUE, g)


static inline void dt_ui_section_label_set(GtkWidget *label)
{
  gtk_widget_set_halign(label, GTK_ALIGN_FILL); // make it span the whole available width
  gtk_label_set_xalign (GTK_LABEL(label), 0.5f);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END); // ellipsize labels
  dt_gui_add_class(label, "dt_section_label"); // make sure that we can style these easily
}

static inline GtkWidget *dt_ui_section_label_new(const gchar *str)
{
  gchar *str_cpy = g_strdup(str);
  dt_capitalize_label(str_cpy);
  GtkWidget *label = gtk_label_new(str_cpy);
  dt_free(str_cpy);
  dt_ui_section_label_set(label);
  return label;
};

static inline GtkWidget *dt_ui_label_new(const gchar *str)
{
  gchar *str_cpy = g_strdup(str);
  dt_capitalize_label(str_cpy);
  GtkWidget *label = gtk_label_new(str_cpy);
  dt_free(str_cpy);
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_label_set_xalign (GTK_LABEL(label), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  return label;
};

GtkNotebook *dt_ui_notebook_new();

GtkWidget *dt_ui_notebook_page(GtkNotebook *notebook, const char *text, const char *tooltip);

// show a dialog box with 2 buttons in case some user interaction is required BEFORE dt's gui is initialised.
// this expects gtk_init() to be called already which should be the case during most of dt's init phase.
gboolean dt_gui_show_standalone_yes_no_dialog(const char *title, const char *markup, const char *no_text,
                                              const char *yes_text);

// similar to the one above. this one asks the user for some string. the hint is shown in the empty entry box
char *dt_gui_show_standalone_string_dialog(const char *title, const char *markup, const char *placeholder,
                                           const char *no_text, const char *yes_text);

void dt_gui_add_help_link(GtkWidget *widget, char *link);

// load a CSS theme
void dt_gui_load_theme(const char *theme);

// reload GUI scalings
void dt_configure_ppd_dpi(dt_gui_gtk_t *gui);

// Recompute the cached 1em size (darktable.gui->em) from the main window's resolved
// font. Call after the theme/font or the screen DPI changes. Also re-applies the standard
// inter-child spacing (DT_GUI_BOX_SPACING) to existing containers so the change is live.
void dt_gui_update_em(void);

// Set a PangoLayout's resolution to the screen DPI for crisp cairo-drawn text. Use this
// instead of hand-writing pango_cairo_context_set_resolution(..., darktable.gui->dpi).
void dt_gui_set_pango_resolution(PangoLayout *layout);

// Apply the system's text-rendering options (anti-aliasing, hinting, subpixel order,
// hint-metrics/kerning) to a Cairo context, sourced from @p widget's Pango context (the same
// settings native GTK widgets use). Call on any off-screen/scratch Cairo surface before drawing
// text so it matches the rest of the UI instead of Cairo's defaults. @p widget may be NULL (falls
// back to the main window, then the screen). Pair with dt_gui_set_pango_resolution() for the DPI.
void dt_gui_cairo_set_font_options(cairo_t *cr, GtkWidget *widget);

// return modifier keys currently pressed, independent of any key event
GdkModifierType dt_key_modifier_state();


/**
 * @brief Wrap a scrollable widget in a recessed, vertically resizable scrolled window with a drag handle.
 *
 * Compatible with GtkTreeView, GtkTextView and any other content widget. A drag grip floats on the
 * scrolled window's bottom edge (invisible until hovered); the chosen height is persisted under
 * @p config_str. Returns the wrapper overlay, not the scrolled window.
 *
 * @param w content widget.
 * @param min_size minimum height floor, in device-independent pixels (rescaled by DT_PIXEL_APPLY_DPI).
 *                 In DT_UI_RESIZE_STATIC mode it also serves as the default height before the user drags.
 * @param config_str conf key persisting the user-chosen height (copied internally).
 * @param mode DT_UI_RESIZE_DYNAMIC to auto-fit content, or DT_UI_RESIZE_STATIC to keep a fixed height
 *             regardless of content (avoids layout shifts for hover-/selection-driven widgets).
 */
GtkWidget *dt_ui_scroll_wrap(GtkWidget *w, gint min_size, char *config_str, dt_ui_resize_mode_t mode);

/**
 * @brief Return the inner GtkScrolledWindow of a dt_ui_scroll_wrap() wrapper, or NULL.
 */
GtkWidget *dt_ui_scroll_wrap_get_scrolled_window(GtkWidget *wrapper);

/**
 * @brief Make a self-drawing widget (typically a GtkDrawingArea graph or scope) vertically resizable.
 *
 * The widget is given a fixed height-request (persisted under @p config_str) and a drag grip floating
 * on its bottom edge — the same grip used by panels, scroll wrappers and the histogram scope. The
 * content is not scrolled: it keeps drawing to its live allocation, only the height-request changes.
 * Returns a wrapper overlay to pack in place of @p area.
 *
 * @param area the drawing widget (its callbacks/refs stay valid; pack the returned overlay instead).
 * @param config_str conf key persisting the user-chosen height (copied internally).
 * @param default_height default height in device-independent px (rescaled by DT_PIXEL_APPLY_DPI).
 * @param min_height minimum height floor in device-independent px.
 */
GtkWidget *dt_ui_resizable_drawing_area(GtkWidget *area, char *config_str, int default_height, int min_height);

/**
 * @brief Apply the standard recessed-input text padding to a GtkTextView.
 *
 * CSS padding on the textview "text" node is parsed but ignored for layout
 * in GTK3, so the 2px/4px inset matching `entry`/`treeview` (see
 * data/themes/.css) has to be set on the widget itself.
 *
 * @param textview The GtkTextView to update.
 */
void dt_gui_textview_set_padding(GtkTextView *textview);
// check whether the given container has any user-added children
gboolean dt_gui_container_has_children(GtkContainer *container);
// return a count of the user-added children in the given container
int dt_gui_container_num_children(GtkContainer *container);
// return the first child of the given container
GtkWidget *dt_gui_container_first_child(GtkContainer *container);
// return the requested child of the given container, or NULL if it has fewer children
GtkWidget *dt_gui_container_nth_child(GtkContainer *container, int which);

// remove all of the children we've added to the container.  Any which no longer have any references will
// be destroyed.
void dt_gui_container_remove_children(GtkContainer *container);

// delete all of the children we've added to the container.  Use this function only if you are SURE
// there are no other references to any of the children (if in doubt, use dt_gui_container_remove_children
// instead; it's a bit slower but safer).
void dt_gui_container_destroy_children(GtkContainer *container);

void dt_gui_menu_popup(GtkMenu *menu, GtkWidget *button, GdkGravity widget_anchor, GdkGravity menu_anchor);

/**
 * @brief Resolve the widget used as parent for nested popups on Wayland.
 *
 * Gtk on Wayland requires popups to use the top-most enclosing popup as parent.
 * This helper walks the parent chain to find that anchor while keeping the caller
 * in charge of the popup logic. When @p rect is not NULL, it returns the position
 * and size of @p widget in the coordinate system of the returned anchor.
 *
 * @param widget the widget the popup should visually point to.
 * @param rect optional output rectangle receiving the geometry of @p widget.
 * @return the widget to use as popup parent, or NULL when @p widget is NULL.
 */
GtkWidget *dt_gui_get_popup_relative_widget(GtkWidget *widget, GdkRectangle *rect);

void dt_gui_draw_rounded_rectangle(cairo_t *cr, float width, float height, float x, float y);

// event handler for "key-press-event" of GtkTreeView to decide if focus switches to GtkSearchEntry
gboolean dt_gui_search_start(GtkWidget *widget, GdkEventKey *event, GtkSearchEntry *entry);

// event handler for "stop-search" of GtkSearchEntry
void dt_gui_search_stop(GtkSearchEntry *entry, GtkWidget *widget);

/**
 * @brief Create a collapsible section and pack it into the parent box.
 *
 * The `pack` argument makes the insertion side explicit so callers control
 * layout order without reordering children later.
 *
 * @param cs section storage owned by the caller.
 * @param confname configuration key used to persist the expanded state.
 * @param label UI label for the section header.
 * @param parent GtkBox that receives the section.
 * @param pack either `GTK_PACK_START` or `GTK_PACK_END` to choose insertion side.
 */
void dt_gui_new_collapsible_section(dt_gui_collapsible_section_t *cs,
                                    const char *confname, const char *label,
                                    GtkBox *parent, GtkPackType pack);
// routine to be called from gui_update
void dt_gui_update_collapsible_section(dt_gui_collapsible_section_t *cs);

// routine to hide the collapsible section
void dt_gui_hide_collapsible_section(dt_gui_collapsible_section_t *cs);

/**
 * Add an arbitrary button next to the widget that opens a popover with arbitrary content.
 * @param widget the original widget next to which the popover button will be added. DON'T add it to a container.
 * @param icon the Freedesktop icon name to put in the button
 * @param content the widget that will fit inside the popover
 * @return the GtkBox containing both the original widget and its popover button.
 * That's what you will need to add it to your container.
*/
GtkBox *attach_popover(GtkWidget *widget, const char *icon, GtkWidget *content);

/**
 * Add an help button triggering a popover label next to an arbitrary widget, to document its action.
 * This is a better take at help tooltips that most people don't see, unless they know about them.
 * Also tooltips window positionning is wonky (can easily overflow viewport),
 * line breaks are added manually (ugly hack),
 * and they appear and disappear on hover (not available on touch screens),
 * so it's flimsy UI.
 * @param widget the original widget to document. DON'T add it to a container.
 * @param label the in-app "docstring" for the widget
 * @return the GtkBox containing both the original widget and its popover button.
 * That's what you will need to add it to your container.
*/
GtkBox *attach_help_popover(GtkWidget *widget, const char *label);


/**
 * @brief Disconnects accels when a text or search entry gets the focus,
 * and reconnects them when it looses it. This helps dealing with one-key shortcuts.
 *
 * @param widget
 */
void dt_accels_disconnect_on_text_input(GtkWidget *widget);

// Get the top-most window attached to a widget.
// This is a dynamic get that takes into account destroyed widgets and such.
static inline GtkWindow *dt_gtk_get_window(GtkWidget *widget)
{
  if(IS_NULL_PTR(widget)) return NULL;
  GtkWidget *toplevel = gtk_widget_get_toplevel(widget);
  if(toplevel && gtk_widget_is_toplevel(toplevel)) return GTK_WINDOW(toplevel);
  return NULL;
}


// Give back the focus to the main/center widget, either
// image in darkroom or thumbtable in lighttable
void dt_gui_refocus_center();

#ifdef __cplusplus
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
