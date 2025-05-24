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

#define DT_GUI_IOP_MODULE_CONTROL_SPACING 0

/* helper macro that applies the DPI transformation to fixed pixel values. input should be defaulting to 96
 * DPI */
#define DT_PIXEL_APPLY_DPI(value) ((value) * darktable.gui->dpi_factor)

typedef struct dt_gui_widgets_t
{
  /* left panel */
  GtkGrid *panel_left; // panel grid 3 rows, top,center,bottom and file on center
  GtkGrid *panel_right;

  /* resize of left/right panels */
  gboolean panel_handle_dragging;
  int panel_handle_x, panel_handle_y;
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

  int icon_size; // size of top panel icons

  // store which gtkrc we loaded:
  char gtkrc[PATH_MAX];

  GtkWidget *scroll_to[2]; // one for left, one for right

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

  dt_pthread_mutex_t mutex;
} dt_gui_gtk_t;

typedef struct _gui_collapsible_section_t
{
  GtkBox *parent;       // the parent widget
  gchar *confname;      // configuration name for the toggle status
  GtkWidget *toggle;    // toggle button
  GtkWidget *expander;  // the expanded
  GtkBox *container;    // the container for all widgets into the section
  GtkWidget *label;     // The section label
} dt_gui_collapsible_section_t;

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
  g_free(str_cpy);
  dt_ui_section_label_set(label);
  return label;
};

static inline GtkWidget *dt_ui_label_new(const gchar *str)
{
  gchar *str_cpy = g_strdup(str);
  dt_capitalize_label(str_cpy);
  GtkWidget *label = gtk_label_new(str_cpy);
  g_free(str_cpy);
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

void dt_gui_add_help_link(GtkWidget *widget, const char *link);

// load a CSS theme
void dt_gui_load_theme(const char *theme);

// reload GUI scalings
void dt_configure_ppd_dpi(dt_gui_gtk_t *gui);

// return modifier keys currently pressed, independent of any key event
GdkModifierType dt_key_modifier_state();

GtkWidget *dt_ui_scroll_wrap(GtkWidget *w, gint min_size, char *config_str);

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

void dt_gui_draw_rounded_rectangle(cairo_t *cr, float width, float height, float x, float y);

// event handler for "key-press-event" of GtkTreeView to decide if focus switches to GtkSearchEntry
gboolean dt_gui_search_start(GtkWidget *widget, GdkEventKey *event, GtkSearchEntry *entry);

// event handler for "stop-search" of GtkSearchEntry
void dt_gui_search_stop(GtkSearchEntry *entry, GtkWidget *widget);

// create a collapsible section, insert in parent, return the container
void dt_gui_new_collapsible_section(dt_gui_collapsible_section_t *cs,
                                    const char *confname, const char *label,
                                    GtkBox *parent);
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
  if(!widget) return NULL;
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
