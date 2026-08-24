/* The application's GUI: the window it owns, the panels in it, the theme it wears, and the
 * process of bringing all of that up and taking it down.
 *
 * This is the half of the old gui/gtk.c that is genuinely about *this* application. The other
 * half -- containers, labels, popovers, notebooks, dialogs, resizable panes -- was toolkit
 * code that merely happened to live here, and now lives in widgets/ with no way back to any
 * of this. The file is no longer called gtk.c partly because that name described its
 * accidental contents rather than its purpose, and partly because a gtk.h of our own, two
 * directories away from the system <gtk/gtk.h>, is a trap nobody needs -- as the first
 * attempt at this rewrite proved by matching that path with a sloppy pattern.
 */

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

#ifndef DT_GUI_APPLICATION_H
#define DT_GUI_APPLICATION_H

/* Only what the declarations below need, and nothing else -- a header that includes more
 * becomes an invisible supply line for its consumers (see CLAUDE.md, "A header includes only
 * what its own declarations need"). Audited symbol by symbol:
 *   paths.h        DT_PATH_MAX, for the gtkrc member
 *   dtpthread.h    dt_pthread_mutex_t, likewise a member -- it used to arrive transitively
 *   window_manager dt_ui_t, dt_ui_container_t, dt_ui_panel_t in the panel signatures
 *   accelerators   dt_accels_t, and dt_accels_new_action_shortcut() for the macros below
 *   gtk.h          GtkWidget, GtkGrid, GtkMenu, cairo_surface_t, cairo_filter_t, PangoLayout
 *   stdint.h       int32_t
 * glib_utils.h, macros.h, mem_alloc.h and dtgtk/thumbtable.h were here for code that has
 * since moved to widgets/; nothing declared here uses them. */
#include "system/dtpthread.h"
#include "common/paths.h"
#include "gui/window_manager.h"
#include "widgets/accelerators.h"

#include <gtk/gtk.h>
#include <stdint.h>



#ifdef __cplusplus
extern "C" {
#endif

/* --- Moved from darktable.h: GUI-flavored helpers belong to the GUI layer, and
 * the orchestrator header must not export GTK/Pango API to the whole application. --- */

/* Application-wide GUI singleton accessor: declared here by the owning lib, implemented by
 * the orchestrator (darktable.c, next to dt_pixelpipe_cache_get_global()). It binds
 * this header's macros and inline helpers to the `dt_gui_get_global()` instance without importing
 * darktable.h into every GUI translation unit. */
struct dt_gui_gtk_t;
struct dt_gui_gtk_t *dt_gui_get_global(void);

/* Sub-handles of the GUI singleton, and the two window lookups that dominate its use.
 * The census behind doc/globals-migration.md showed dt_gui_get_global() is not one dependency
 * but three -- the dt_ui_t handle, the write-once accelerator registry, and gtk.c's own
 * scroll/DPI state -- so expose the first two directly instead of making every caller
 * walk the application struct. dt_ui_main_window()/dt_ui_center() were called with the
 * very same argument at 178 sites; these give them a name. */
struct dt_ui_t *dt_gui_get_ui(void);
struct dt_accels_t *dt_gui_get_accels(void);
GtkWidget *dt_gui_main_window(void);

/** The list of available GTK theme names (gchar*), rebuilt by the preferences dialog,
 * read by gtk.c for padding decisions. dt_gui_set_themes() TAKES OWNERSHIP and frees
 * the previous list. */
GList *dt_gui_get_themes(void);
void dt_gui_set_themes(GList *themes);
GtkWidget *dt_gui_center_widget(void);

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
// DT_UI_SCALE_* / DT_PIXEL_APPLY_DPI* now come from widgets/widget_settings.h

/* Deprecated spellings kept so the existing call sites keep compiling. Prefer the
 * intent-named macros above in new code. */

/* Spacing between children widgets within Gtk boxes/grids/flowboxes cannot be set from
 * CSS (margins/paddings on the children would recess the ones sitting on the container
 * edges relative to the inner ones). GTK exposes a "spacing" property for this, but only
 * from code - so it is centralized here, in ONE place, for the whole app.
 *
 * It is expressed as a fraction of 1em (the resolved root font size, cached in
 * dt_gui_get_global()->em by dt_gui_update_em()), so the inner gutters scale with the user's
 * font size exactly like the em-based margins/paddings in ansel.css. 0.625em == 10px at
 * the 16px reference font. Because the font's point->px conversion already folds in the
 * screen DPI, this needs NO DT_PIXEL_APPLY_DPI on top.
 *
 * Falls back to the 10px reference before the GUI exists or before gui->em has
 * been resolved. Standalone dialogs may run after gtk_init() but before the
 * main Ansel GUI allocation when startup needs user input. */
// DT_GUI_BOX_SPACING now comes from widgets/widget_settings.h

typedef struct dt_gui_widgets_t
{
  /* left panel */
  GtkGrid *panel_left; // panel grid 3 rows, top,center,bottom and file on center
  GtkGrid *panel_right;
} dt_gui_widgets_t;

typedef struct dt_gui_gtk_t
{

  dt_ui_t *ui;

  dt_gui_widgets_t widgets;

  cairo_surface_t *surface;
  GtkMenu *presets_popup_menu;
  char *last_preset;

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

  // Global accelerators for main menu, needed for GtkMenu mnemonics.
  dt_accels_t *accels;

  GList *input_devices;

  double dpi, dpi_factor, ppd;

  // Resolved root font size (1em) in device-independent px, read from the active
  // theme/font by dt_gui_update_em(). Drives DT_GUI_BOX_SPACING so inner gutters
  // track the font size like em-based CSS margins. 0.0 until first resolved.
  double em;


  struct {
    gboolean is_dragging;
    gboolean is_painting;
  } mouse;

  int icon_size; // size of top panel icons

  // store which gtkrc we loaded:
  char gtkrc[DT_PATH_MAX];

  GtkWidget *scroll_to[2]; // one for left, one for right
  GtkWidget *scroll_to_header_once; // one-shot: module expander that should scroll to its header once


  // scrolling focus
  // This emulates the same feature as Gtk focus, but to capture scrolling events

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







// call class function to add or remove CSS classes (need to be set on top of this file as first function is used in this file)

int dt_gui_gtk_init(dt_gui_gtk_t *gui);
void dt_gui_gtk_run(dt_gui_gtk_t *gui);
void dt_gui_gtk_quit();
void dt_gui_store_last_preset(const char *name);
int dt_gui_gtk_write_config();

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
/** \brief is the panel ancestor of widget */
/** \brief get the center drawable widget */
/** \brief get the main window widget */

/** \brief get the log message widget */
/** \brief get the toast message widget */


// capitalize strings. Because grammar says sentences start with a capital,
// and typography says it makes it easier to extract the structure of the text.

#define dt_accels_new_global_action(a, b, c, d, e, f, g) dt_accels_new_action_shortcut(dt_gui_get_global()->accels, a, b, dt_gui_get_global()->accels->global_accels, c, d, e, f, FALSE, g)

// dt_accels_new_darkroom_action() now lives in widgets/accelerators.h

#define dt_accels_new_lighttable_action(a, b, c, d, e, f, g) dt_accels_new_action_shortcut(dt_gui_get_global()->accels, a, b, dt_gui_get_global()->accels->lighttable_accels, c, d, e, f, FALSE, g)

#define dt_accels_new_map_action(a, b, c, d, e, f, g) dt_accels_new_action_shortcut(dt_gui_get_global()->accels, a, b, dt_gui_get_global()->accels->map_accels, c, d, e, f, FALSE, g)

#define dt_accels_new_print_action(a, b, c, d, e, f, g) dt_accels_new_action_shortcut(dt_gui_get_global()->accels, a, b, dt_gui_get_global()->accels->print_accels, c, d, e, f, FALSE, g)

#define dt_accels_new_slideshow_action(a, b, c, d, e, f, g) dt_accels_new_action_shortcut(dt_gui_get_global()->accels, a, b, dt_gui_get_global()->accels->slideshow_accels, c, d, e, f, FALSE, g)

#define dt_accels_new_darkroom_locked_action(a, b, c, d, e, f, g) dt_accels_new_action_shortcut(dt_gui_get_global()->accels, a, b, dt_gui_get_global()->accels->darkroom_accels, c, d, e, f, TRUE, g)


void dt_gui_add_help_link(GtkWidget *widget, char *link);

// load a CSS theme
void dt_gui_load_theme(const char *theme);

// reload GUI scalings
void dt_configure_ppd_dpi(dt_gui_gtk_t *gui);

// Recompute the cached 1em size (dt_gui_get_global()->em) from the main window's resolved
// font. Call after the theme/font or the screen DPI changes. Also re-applies the standard
// inter-child spacing (DT_GUI_BOX_SPACING) to existing containers so the change is live.
void dt_gui_update_em(void);

// Set a PangoLayout's resolution to the screen DPI for crisp cairo-drawn text. Use this
// instead of hand-writing pango_cairo_context_set_resolution(..., dt_gui_get_global()->dpi).
void dt_gui_set_pango_resolution(PangoLayout *layout);

// Apply the system's text-rendering options (anti-aliasing, hinting, subpixel order,
// hint-metrics/kerning) to a Cairo context, sourced from @p widget's Pango context (the same
// settings native GTK widgets use). Call on any off-screen/scratch Cairo surface before drawing
// text so it matches the rest of the UI instead of Cairo's defaults. @p widget may be NULL (falls
// back to the main window, then the screen). Pair with dt_gui_set_pango_resolution() for the DPI.




// Give back the focus to the main/center widget, either
// image in darkroom or thumbtable in lighttable
void dt_gui_refocus_center();

#ifdef __cplusplus
}
#endif

#endif // DT_GUI_APPLICATION_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
