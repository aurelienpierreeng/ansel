/*
    This file is part of the Ansel project.
    Copyright (C) 2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2023 Luca Zulberti.
    Copyright (C) 2025 Alynx Zhou.
    
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
#include "common/darktable.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "views/view.h"
#include "bauhaus/bauhaus.h"
#include "gui/window_manager.h"
#include "gui/actions/menu.h"
#include "dtgtk/sidepanel.h"
#include "libs/lib.h"

#define WINDOW_DEBUG 0

typedef struct dt_header_t
{
  GtkWidget *titlebar;
  GtkWidget *menu_bar;
  GtkWidget *menus[DT_MENU_LAST];
  GList *item_lists[DT_MENU_LAST];
  GtkWidget *hinter;
  GtkWidget *home;
  GtkWidget *close;
  GtkWidget *iconify;
  GtkWidget *image_info;
} dt_header_t;

typedef enum dt_panel_side_t
{
  LEFT_PANNEL = 0,
  RIGHT_PANNEL = 1,
  PANEL_SIDE_COUNT
} dt_panel_side_t;

const char *_ui_panel_config_names[]
    = { "header", "toolbar_top", "toolbar_bottom", "left", "right", "bottom" };


gchar * panels_get_view_path(char *suffix)
{

  if(IS_NULL_PTR(darktable.view_manager)) return NULL;
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(IS_NULL_PTR(cv)) return NULL;
  char lay[32] = "";

  if(!strcmp(cv->module_name, "lighttable"))
    g_snprintf(lay, sizeof(lay), "%d/", 0);
  else if(!strcmp(cv->module_name, "darkroom"))
    g_snprintf(lay, sizeof(lay), "%d/", dt_view_darkroom_get_layout(darktable.view_manager));

  return g_strdup_printf("%s/ui/%s%s", cv->module_name, lay, suffix);
}

gchar * panels_get_panel_path(dt_ui_panel_t panel, char *suffix)
{
  gchar *v = panels_get_view_path("");
  if(IS_NULL_PTR(v)) return NULL;
  return dt_util_dstrcat(v, "%s%s", _ui_panel_config_names[panel], suffix);
}

int dt_ui_panel_get_size(dt_ui_t *ui, const dt_ui_panel_t p)
{
  gchar *key = NULL;

  if(p == DT_UI_PANEL_LEFT || p == DT_UI_PANEL_RIGHT || p == DT_UI_PANEL_BOTTOM)
  {
    int size = 0;

    key = panels_get_panel_path(p, "_size");
    if(key && dt_conf_key_exists(key))
    {
      size = dt_conf_get_int(key);
      dt_free(key);
    }
    else // size hasn't been adjusted, so return default sizes
    {
      if(p == DT_UI_PANEL_BOTTOM)
        size = DT_UI_PANEL_BOTTOM_DEFAULT_SIZE;
      else
        size = DT_UI_PANEL_SIDE_DEFAULT_SIZE;

      if(!IS_NULL_PTR(key)) dt_free(key);
    }
    return size;
  }
  return -1;
}

gboolean dt_ui_panel_ancestor(dt_ui_t *ui, const dt_ui_panel_t p, GtkWidget *w)
{
  g_return_val_if_fail(GTK_IS_WIDGET(ui->panels[p]), FALSE);
  return gtk_widget_is_ancestor(w, ui->panels[p]) || gtk_widget_is_ancestor(ui->panels[p], w);
}

GtkWidget *dt_ui_center(dt_ui_t *ui)
{
  return ui->center;
}
GtkWidget *dt_ui_center_base(dt_ui_t *ui)
{
  return ui->center_base;
}

GtkWidget *dt_ui_log_msg(dt_ui_t *ui)
{
  return ui->log_msg;
}
GtkWidget *dt_ui_toast_msg(dt_ui_t *ui)
{
  return ui->toast_msg;
}

GtkWidget *dt_ui_main_window(dt_ui_t *ui)
{
  return ui->main_window;
}

GtkBox *dt_ui_get_container(dt_ui_t *ui, const dt_ui_container_t c)
{
  return GTK_BOX(ui->containers[c]);
}

void dt_ui_container_add_widget(dt_ui_t *ui, const dt_ui_container_t c, GtkWidget *w)
{
  switch(c)
  {
    /* These should be flexboxes/flowboxes so line wrapping is turned on when line width is too small to contain everything
    *  but flexboxes don't seem to work here as advertised (everything either goes to new line or same line, no wrapping),
    *  maybe because they will get added to boxes at the end, and Gtk heuristics to decide final width are weird.
    */
    /* if box is right lets pack at end for nicer alignment */
    /* if box is center we want it to fill as much as it can */
    case DT_UI_CONTAINER_PANEL_TOP_SECOND_ROW:
      gtk_box_pack_start(GTK_BOX(ui->containers[c]), w, TRUE, TRUE, 0);
      break;

    default:
      gtk_box_pack_start(GTK_BOX(ui->containers[c]), w, FALSE, FALSE, 0);
      break;
  }
  gtk_widget_show_all(w);
}

static void _ui_init_panel_size(GtkWidget *widget, dt_ui_t *ui)
{
  gchar *key = NULL;
  int s = DT_UI_PANEL_SIDE_DEFAULT_SIZE; // default panel size
  if(strcmp(gtk_widget_get_name(widget), "right") == 0)
  {
    key = panels_get_panel_path(DT_UI_PANEL_RIGHT, "_size");
    if(key && dt_conf_key_exists(key))
      s = MAX(dt_conf_get_int(key), 120);
    if(key) gtk_widget_set_size_request(widget, s, -1);
  }
  else if(strcmp(gtk_widget_get_name(widget), "left") == 0)
  {
    key = panels_get_panel_path(DT_UI_PANEL_LEFT, "_size");
    if(key && dt_conf_key_exists(key))
      s = MAX(dt_conf_get_int(key), 120);
    if(key) gtk_widget_set_size_request(widget, s, -1);
  }
  else if(strcmp(gtk_widget_get_name(widget), "bottom") == 0)
  {
    key = panels_get_panel_path(DT_UI_PANEL_BOTTOM, "_size");
    s = DT_UI_PANEL_BOTTOM_DEFAULT_SIZE; // default panel size
    if(key && dt_conf_key_exists(key))
      s = MAX(dt_conf_get_int(key), 48);
    if(key) gtk_widget_set_size_request(widget, -1, s);
  }

  dt_free(key);
}

void dt_ui_restore_panels(dt_ui_t *ui)
{
  /* restore left & right panel size */
  _ui_init_panel_size(ui->panels[DT_UI_PANEL_LEFT], ui);
  _ui_init_panel_size(ui->panels[DT_UI_PANEL_RIGHT], ui);
  _ui_init_panel_size(ui->panels[DT_UI_PANEL_BOTTOM], ui);

  /* restore from a previous collapse all panel state if enabled */
  gchar *key = panels_get_view_path("panel_collaps_state");
  const uint32_t state = dt_conf_get_int(key);
  dt_free(key);
  if(state)
  {
    /* hide all panels (we let saved state as it is, to recover them when pressing TAB)*/
    for(int k = 0; k < DT_UI_PANEL_SIZE; k++) dt_ui_panel_show(ui, k, FALSE, FALSE);
  }
  else
  {
    /* restore the visible state of panels */
    for(int k = 0; k < DT_UI_PANEL_SIZE; k++)
    {
      key = panels_get_panel_path(k, "_visible");
      if(dt_conf_key_exists(key))
        dt_ui_panel_show(ui, k, dt_conf_get_bool(key), FALSE);
      else
        dt_ui_panel_show(ui, k, TRUE, TRUE);

      dt_free(key);
    }
  }
}

/* The main panels share the generic resize-handle primitive (dt_bauhaus_resize_handle_new),
 * so they get the same grip visual, hover border and cursor as every other resizable area.
 * These two callbacks let the handle query and apply the panel size; the panel widget is passed
 * as user_data and identified by its name ("left"/"right"/"bottom"). */
static int _panel_handle_get_size(gpointer user_data)
{
  GtkWidget *widget = GTK_WIDGET(user_data);
  const gboolean bottom = (strcmp(gtk_widget_get_name(widget), "bottom") == 0);
  gint w = 0, h = 0;
  gtk_widget_get_size_request(widget, &w, &h);
  if(bottom) return (h > 0) ? h : gtk_widget_get_allocated_height(widget);
  return (w > 0) ? w : gtk_widget_get_allocated_width(widget);
}

static int _panel_handle_resize(int requested_size, gboolean finished, gpointer user_data)
{
  GtkWidget *widget = GTK_WIDGET(user_data);
  const char *name = gtk_widget_get_name(widget);
  GtkWidget *window = dt_ui_main_window(darktable.gui->ui);
  int win_w = 0, win_h = 0;
  gtk_window_get_size(GTK_WINDOW(window), &win_w, &win_h);

  gchar *key = NULL;
  int size = requested_size;
  if(strcmp(name, "right") == 0)
  {
    size = CLAMP(requested_size, 150, win_w / 2);
    key = panels_get_panel_path(DT_UI_PANEL_RIGHT, "_size");
    gtk_widget_set_size_request(widget, size, -1);
  }
  else if(strcmp(name, "left") == 0)
  {
    size = CLAMP(requested_size, 150, win_w / 2);
    key = panels_get_panel_path(DT_UI_PANEL_LEFT, "_size");
    gtk_widget_set_size_request(widget, size, -1);
  }
  else if(strcmp(name, "bottom") == 0)
  {
    size = CLAMP(requested_size, 48, win_h / 3);
    key = panels_get_panel_path(DT_UI_PANEL_BOTTOM, "_size");
    gtk_widget_set_size_request(widget, -1, size);
  }

  // Persist only when the gesture ends, but apply the size live during the drag.
  if(finished && key) dt_conf_set_int(key, size);
  dt_free(key);

  return size;
}

/* initialize the top container of panel */
static GtkWidget *_ui_init_panel_container_top(GtkWidget *container)
{
  GtkWidget *w = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);
  gtk_box_pack_start(GTK_BOX(container), w, FALSE, FALSE, 0);
  return w;
}

/**
 * @brief Check whether a deferred panel scroll target is still a live module widget.
 *
 * We only accept widgets currently referenced by module descriptors. This avoids
 * dereferencing stale pointers left after asynchronous GTK teardown.
 *
 * @param target widget pointer queued for panel scrolling.
 * @param side panel side (`LEFT_PANNEL` or `RIGHT_PANNEL`).
 *
 * @return gboolean TRUE if target is still referenced by a visible module tree.
 */
static gboolean _ui_scroll_target_is_live_widget(const GtkWidget *target, const int side)
{
  if(IS_NULL_PTR(target)) return FALSE;
  if(side != LEFT_PANNEL && side != RIGHT_PANNEL) return FALSE;

  if(!IS_NULL_PTR(darktable.lib))
  {
    // Walk all lib modules and look for the exact expander address queued for scrolling.
    for(const GList *libs = darktable.lib->plugins; libs; libs = g_list_next(libs))
    {
      const dt_lib_module_t *module = (const dt_lib_module_t *)libs->data;
      if(!IS_NULL_PTR(module) && module->expander == target) return TRUE;
    }
  }

  if(side == RIGHT_PANNEL && !IS_NULL_PTR(darktable.develop))
  {
    // Walk darkroom iop modules and accept either header or expander scroll anchors.
    for(const GList *iops = darktable.develop->iop; iops; iops = g_list_next(iops))
    {
      const dt_iop_module_t *module = (const dt_iop_module_t *)iops->data;
      if(IS_NULL_PTR(module)) continue;
      if(module->expander == target || module->header == target) return TRUE;
    }
  }

  return FALSE;
}

// this should work as long as everything happens in the gui thread
static void _ui_panel_size_changed(GtkAdjustment *adjustment, GParamSpec *pspec, gpointer user_data)
{
  GtkAllocation allocation;
  static float last_height[PANEL_SIDE_COUNT] = { 0 };

  const int side = GPOINTER_TO_INT(user_data);
  if(side != LEFT_PANNEL && side != RIGHT_PANNEL) return;

  // don't do anything when the size didn't actually change.
  const float height = gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_lower(adjustment);

  if(height == last_height[side]) return;
  last_height[side] = height;

  if(IS_NULL_PTR(darktable.gui->scroll_to[side])) return;
  if(!_ui_scroll_target_is_live_widget(darktable.gui->scroll_to[side], side))
  {
    darktable.gui->scroll_to[side] = NULL;
    return;
  }

  if(GTK_IS_WIDGET(darktable.gui->scroll_to[side]))
  {
    gtk_widget_get_allocation(darktable.gui->scroll_to[side], &allocation);
    gtk_adjustment_set_value(adjustment, allocation.y);
  }

  darktable.gui->scroll_to[side] = NULL;
}

/* initialize the center container of panel */
static GtkWidget *_ui_init_panel_container_center(GtkWidget *container, gboolean left)
{
  GtkWidget *widget;
  GtkAdjustment *a[4];

  a[0] = GTK_ADJUSTMENT(gtk_adjustment_new(0, 0, 100, 1, 10, 10));
  a[1] = GTK_ADJUSTMENT(gtk_adjustment_new(0, 0, 100, 1, 10, 10));
  a[2] = GTK_ADJUSTMENT(gtk_adjustment_new(0, 0, 100, 1, 10, 10));
  a[3] = GTK_ADJUSTMENT(gtk_adjustment_new(0, 0, 100, 1, 10, 10));

  /* create the scrolled window */
  widget = gtk_scrolled_window_new(a[0], a[1]);
  gtk_widget_set_can_focus(widget, TRUE);
  gtk_scrolled_window_set_placement(GTK_SCROLLED_WINDOW(widget),
                                    left ? GTK_CORNER_TOP_LEFT : GTK_CORNER_TOP_RIGHT);
  gtk_box_pack_start(GTK_BOX(container), widget, TRUE, TRUE, 0);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(widget), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

  g_signal_connect(G_OBJECT(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(widget))), "notify::lower",
                   G_CALLBACK(_ui_panel_size_changed),
                   GINT_TO_POINTER(left ? RIGHT_PANNEL : LEFT_PANNEL));

  /* create the scrolled viewport */
  container = widget;
  widget = gtk_viewport_new(a[2], a[3]);
  gtk_viewport_set_shadow_type(GTK_VIEWPORT(widget), GTK_SHADOW_NONE);
  gtk_container_add(GTK_CONTAINER(container), widget);

  /* create the container */
  container = widget;
  // WARNING: no spacing between modules in left sidebar
  widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(widget, "plugins_box");
  gtk_container_add(GTK_CONTAINER(container), widget);

  return widget;
}

/* initialize the bottom container of panel */
static GtkWidget *_ui_init_panel_container_bottom(GtkWidget *container)
{
  GtkWidget *w = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);
  gtk_box_pack_start(GTK_BOX(container), w, FALSE, FALSE, 0);
  return w;
}

/* initialize the whole left panel */
static void _ui_init_panel_left(dt_ui_t *ui, GtkWidget *container)
{
  GtkWidget *widget;

  /* create left panel main widget and add it to ui */
  widget = ui->panels[DT_UI_PANEL_LEFT] = dtgtk_side_panel_new();
  gtk_widget_set_name(widget, "left");
  _ui_init_panel_size(widget, ui);

  GtkWidget *over = gtk_overlay_new();
  gtk_container_add(GTK_CONTAINER(over), widget);
  // resize grip overlaid on the panel's inner (right) edge: drag right to grow
  GtkWidget *handle = dt_bauhaus_resize_handle_new(GTK_ORIENTATION_HORIZONTAL, FALSE,
                                                   _("Drag to resize panel"),
                                                   _panel_handle_get_size, _panel_handle_resize, widget);
  gtk_overlay_add_overlay(GTK_OVERLAY(over), handle);
  gtk_widget_show(handle);

  gtk_grid_attach(GTK_GRID(container), over, 1, 1, 1, 1);

  /* add top,center,bottom*/
  container = widget;
  ui->containers[DT_UI_CONTAINER_PANEL_LEFT_TOP] = _ui_init_panel_container_top(container);
  ui->containers[DT_UI_CONTAINER_PANEL_LEFT_CENTER] = _ui_init_panel_container_center(container, FALSE);
  ui->containers[DT_UI_CONTAINER_PANEL_LEFT_BOTTOM] = _ui_init_panel_container_bottom(container);

  /* lets show all widgets */
  gtk_widget_show_all(ui->panels[DT_UI_PANEL_LEFT]);
}

/* initialize the whole right panel */
static void _ui_init_panel_right(dt_ui_t *ui, GtkWidget *container)
{
  GtkWidget *widget;

  /* create right panel main widget and add it to ui */
  widget = ui->panels[DT_UI_PANEL_RIGHT] = dtgtk_side_panel_new();
  gtk_widget_set_name(widget, "right");
  _ui_init_panel_size(widget, ui);

  GtkWidget *over = gtk_overlay_new();
  gtk_container_add(GTK_CONTAINER(over), widget);
  // resize grip overlaid on the panel's inner (left) edge: drag left to grow (inverted)
  GtkWidget *handle = dt_bauhaus_resize_handle_new(GTK_ORIENTATION_HORIZONTAL, TRUE,
                                                   _("Drag to resize panel"),
                                                   _panel_handle_get_size, _panel_handle_resize, widget);
  gtk_overlay_add_overlay(GTK_OVERLAY(over), handle);
  gtk_widget_show(handle);

  gtk_grid_attach(GTK_GRID(container), over, 3, 1, 1, 1);

  /* add top,center,bottom*/
  container = widget;
  ui->containers[DT_UI_CONTAINER_PANEL_RIGHT_TOP] = _ui_init_panel_container_top(container);
  ui->containers[DT_UI_CONTAINER_PANEL_RIGHT_CENTER] = _ui_init_panel_container_center(container, TRUE);
  ui->containers[DT_UI_CONTAINER_PANEL_RIGHT_BOTTOM] = _ui_init_panel_container_bottom(container);

  /* lets show all widgets */
  gtk_widget_show_all(ui->panels[DT_UI_PANEL_RIGHT]);
}

/* initialize the top container of panel */
static void _ui_init_panel_top(dt_ui_t *ui, GtkWidget *container)
{
  GtkWidget *widget;

  /* create the panel box */
  // Warning: No spacing between lines !!!
  ui->panels[DT_UI_PANEL_TOP] = widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(ui->panels[DT_UI_PANEL_TOP], "top");
  gtk_widget_set_hexpand(GTK_WIDGET(widget), TRUE);
  gtk_grid_attach(GTK_GRID(container), widget, 1, 0, 3, 1);

  /* add container for top center */
  ui->top_panel = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
  gtk_widget_set_name(ui->top_panel, "top-first-line");
  gtk_box_pack_start(GTK_BOX(widget), ui->top_panel, FALSE, FALSE,
                     DT_UI_PANEL_MODULE_SPACING);

  ui->containers[DT_UI_CONTAINER_PANEL_TOP_SECOND_ROW] = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
  gtk_widget_set_name(ui->containers[DT_UI_CONTAINER_PANEL_TOP_SECOND_ROW], "top-second-line");
  gtk_box_pack_start(GTK_BOX(widget), ui->containers[DT_UI_CONTAINER_PANEL_TOP_SECOND_ROW], FALSE, FALSE,
                     DT_UI_PANEL_MODULE_SPACING);
}


/* initialize the bottom panel */
static void _ui_init_panel_bottom(dt_ui_t *ui, GtkWidget *container)
{
  /* create the panel box */
  GtkWidget *over = gtk_overlay_new();
  ui->thumbtable_filmstrip = dt_thumbtable_new(DT_THUMBTABLE_MODE_FILMSTRIP);
  gtk_container_add(GTK_CONTAINER(over), ui->thumbtable_filmstrip->parent_overlay);

  ui->panels[DT_UI_PANEL_BOTTOM] = ui->thumbtable_filmstrip->parent_overlay;
  gtk_widget_set_name(ui->thumbtable_filmstrip->parent_overlay, "bottom");
  _ui_init_panel_size(ui->thumbtable_filmstrip->parent_overlay, ui);
  gtk_grid_attach(GTK_GRID(container), over, 1, 2, 3, 1);

  // resize grip overlaid on the panel's top edge: drag up to grow (inverted).
  // We resize the actual bottom panel widget (named "bottom"), not the overlay wrapper.
  // Otherwise the panel can be grown (outer overlay expands) but not shrunk because the
  // filmstrip panel keeps its previous size request until the view is recreated.
  GtkWidget *handle = dt_bauhaus_resize_handle_new(GTK_ORIENTATION_VERTICAL, TRUE,
                                                   _("Drag to resize panel"),
                                                   _panel_handle_get_size, _panel_handle_resize,
                                                   ui->thumbtable_filmstrip->parent_overlay);
  gtk_overlay_add_overlay(GTK_OVERLAY(over), handle);
  gtk_widget_show(handle);
}

/* this is called as a signal handler, the signal raising logic asserts the gdk lock. */
static void _ui_widget_redraw_callback(gpointer instance, GtkWidget *widget)
{
   gtk_widget_queue_draw(widget);
}

void dt_ui_init_main_table(GtkWidget *parent, dt_ui_t *ui)
{
  GtkWidget *widget;

  // Creating the table
  GtkWidget *container = gtk_grid_new();
  gtk_box_pack_start(GTK_BOX(parent), container, TRUE, TRUE, 0);
  gtk_widget_show(container);

  /* initialize toolboxes panels */
  _ui_init_panel_top(ui, container);
  _ui_init_panel_bottom(ui, container);
  _ui_init_panel_left(ui, container);
  _ui_init_panel_right(ui, container);

  /* initialize the main drawing widget (center) */
  widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);
  gtk_widget_set_name(widget, "main-widget");
  gtk_widget_set_hexpand(GTK_WIDGET(widget), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(widget), TRUE);
  gtk_grid_attach(GTK_GRID(container), widget, 2, 1, 1, 1);

  /* initialize the thumb panel */
  ui->thumbtable_lighttable = dt_thumbtable_new(DT_THUMBTABLE_MODE_FILEMANAGER);

  /* setup center drawing area */
  GtkWidget *ocda = gtk_overlay_new();
  gtk_widget_set_size_request(ocda, DT_PIXEL_APPLY_DPI(200), DT_PIXEL_APPLY_DPI(200));
  gtk_widget_show(ocda);

  GtkWidget *cda = gtk_drawing_area_new();
  gtk_widget_set_hexpand(ocda, TRUE);
  gtk_widget_set_vexpand(ocda, TRUE);
  gtk_widget_set_app_paintable(cda, TRUE);
  gtk_widget_set_events(cda, GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK | GDK_KEY_PRESS_MASK
                             | GDK_BUTTON_RELEASE_MASK | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK
                             | darktable.gui->scroll_mask);
  gtk_overlay_add_overlay(GTK_OVERLAY(ocda), cda);

  // Add the reserved overlay for the thumbtable in central position
  // Then we insert into container, instead of dynamically adding/removing a new overlay
  // because log and toast messages need to go on top too.
  gtk_overlay_add_overlay(GTK_OVERLAY(ocda), ui->thumbtable_lighttable->parent_overlay);

  gtk_box_pack_start(GTK_BOX(widget), ocda, TRUE, TRUE, 0);

  ui->center = cda;
  ui->center_base = ocda;

  /* center should redraw when signal redraw center is raised*/
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_CONTROL_REDRAW_CENTER,
                            G_CALLBACK(_ui_widget_redraw_callback), ui->center);

  gtk_widget_show_all(container);
}

void dt_ui_cleanup_main_table(dt_ui_t *ui)
{
  // Avoid dangling UI pointers during shutdown: background threads might query
  // thumbnail info while mipmap cache is being flushed.
  dt_thumbtable_t *filmstrip = ui->thumbtable_filmstrip;
  dt_thumbtable_t *lighttable = ui->thumbtable_lighttable;

  ui->thumbtable_filmstrip = NULL;
  ui->thumbtable_lighttable = NULL;

  if(filmstrip) dt_thumbtable_cleanup(filmstrip);
  if(lighttable) dt_thumbtable_cleanup(lighttable);
}


void dt_ui_init_titlebar(dt_ui_t *ui)
{
  ui->header = g_malloc0(sizeof(dt_header_t));

#ifdef MERGE_MENUBAR
  // Remove useless desktop environment titlebar. We will handle closing buttons internally
  ui->header->titlebar = gtk_header_bar_new();
  gtk_widget_set_size_request(ui->header->titlebar, -1, -1);
  gtk_window_set_titlebar(GTK_WINDOW(ui->main_window), ui->header->titlebar);

  // Reset header bar properties
  gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(ui->header->titlebar), FALSE);
  gtk_header_bar_set_decoration_layout(GTK_HEADER_BAR(ui->header->titlebar), NULL);

  // Gtk mandatorily adds an empty label that is still "visible" for the title.
  // Since it's centered, it can collide with the hinter width.
  // Plus it adds mandatory padding. AKA scrap that.
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
  gtk_header_bar_set_custom_title(GTK_HEADER_BAR(ui->header->titlebar), box);
  gtk_widget_set_no_show_all(box, TRUE);
#else
  ui->header->titlebar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
#endif

  gtk_widget_show(ui->header->titlebar);
}

void dt_ui_titlebar_pack_start(dt_ui_t *ui, GtkWidget *widget)
{
#ifdef MERGE_MENUBAR
  gtk_header_bar_pack_start(GTK_HEADER_BAR(ui->header->titlebar), widget);
#else
  gtk_box_pack_start(GTK_BOX(ui->header->titlebar), widget, FALSE, FALSE, 0);
#endif
}

void dt_ui_titlebar_pack_end(dt_ui_t *ui, GtkWidget *widget)
{
#ifdef MERGE_MENUBAR
  gtk_header_bar_pack_end(GTK_HEADER_BAR(ui->header->titlebar), widget);
#else
  gtk_box_pack_end(GTK_BOX(ui->header->titlebar), widget, FALSE, FALSE, 0);
#endif
}

void _home_callback()
{
  dt_ctl_switch_mode_to("lighttable");
}

void _close_callback(GtkWidget *w, gpointer data)
{
  gtk_window_close(GTK_WINDOW((GtkWidget *)data));
}

void _iconify_callback(GtkWidget *w, gpointer data)
{
  gtk_window_iconify(GTK_WINDOW((GtkWidget *)data));
}

void _open_accel_search_callback(GtkWidget *w, gpointer data)
{
  dt_accels_search(darktable.gui->accels, GTK_WINDOW(darktable.gui->ui->main_window), w);
}

void dt_ui_init_global_menu(dt_ui_t *ui)
{
  /* if user_pref != merge menubar in titlebar */
  GtkWidget *parent = ui->top_panel;
  gtk_box_pack_start(GTK_BOX(parent), ui->header->titlebar, TRUE, TRUE, 0);
  /* endif */

  /* Init top-level menus */
  ui->header->menu_bar = gtk_menu_bar_new();
  gtk_widget_set_name(ui->header->menu_bar, "menu-bar");
  gchar *labels [DT_MENU_LAST] = { _("_File"), _("_Edit"), _("_Selection"), _("_Image"), _("_Styles"), _("_Run"), _("_Display"), _("_Ateliers"), _("_Help") };
  for(int i = 0; i < DT_MENU_LAST; i++)
  {
    ui->header->item_lists[i] = NULL;
    add_top_menu_entry(ui->header->menu_bar, ui->header->menus, &ui->header->item_lists[i], i, labels[i]);
  }

  gtk_widget_set_halign(ui->header->menu_bar, GTK_ALIGN_START);
  gtk_widget_set_hexpand(ui->header->menu_bar, FALSE);

  /* Populate sub-menus */
  append_file(ui->header->menus, &ui->header->item_lists[DT_MENU_FILE], DT_MENU_FILE);
  append_edit(ui->header->menus, &ui->header->item_lists[DT_MENU_EDIT], DT_MENU_EDIT);
  append_select(ui->header->menus, &ui->header->item_lists[DT_MENU_SELECTION], DT_MENU_SELECTION);
  append_image(ui->header->menus, &ui->header->item_lists[DT_MENU_IMAGE], DT_MENU_IMAGE);
  append_styles(ui->header->menus, &ui->header->item_lists[DT_MENU_STYLES], DT_MENU_STYLES);
  append_run(ui->header->menus, &ui->header->item_lists[DT_MENU_RUN], DT_MENU_RUN);
  append_display(ui->header->menus, &ui->header->item_lists[DT_MENU_DISPLAY], DT_MENU_DISPLAY);
  append_views(ui->header->menus, &ui->header->item_lists[DT_MENU_ATELIERS], DT_MENU_ATELIERS);
  append_help(ui->header->menus, &ui->header->item_lists[DT_MENU_HELP], DT_MENU_HELP);

  dt_ui_titlebar_pack_start(ui, ui->header->menu_bar);
  gtk_widget_show_all(ui->header->menu_bar);

  GtkWidget *search_button = gtk_button_new_from_icon_name("edit-find", GTK_ICON_SIZE_SMALL_TOOLBAR);
  gtk_button_set_label (GTK_BUTTON(search_button), _("Search actions..."));
  gtk_widget_set_halign(search_button, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(search_button, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand(search_button, TRUE);
  gtk_widget_set_name(search_button, "search-button");
  g_signal_connect(G_OBJECT(search_button), "clicked", G_CALLBACK(_open_accel_search_callback), NULL);
  dt_ui_titlebar_pack_start(ui, search_button);
  gtk_widget_show(search_button);

  // From there, we pack_end meaning it should be done in reverse order of appearance
  ui->header->close = gtk_button_new_from_icon_name("window-close", GTK_ICON_SIZE_LARGE_TOOLBAR);
  g_signal_connect(G_OBJECT(ui->header->close), "clicked", G_CALLBACK(_close_callback), ui->main_window);
  gtk_widget_set_size_request(ui->header->close, 24, 24);
  dt_gui_add_class(ui->header->close, "window-button");
  dt_ui_titlebar_pack_end(ui, ui->header->close);

  ui->header->iconify = gtk_button_new_from_icon_name("window-minimize", GTK_ICON_SIZE_LARGE_TOOLBAR);
  g_signal_connect(G_OBJECT(ui->header->iconify), "clicked", G_CALLBACK(_iconify_callback), ui->main_window);
  gtk_widget_set_size_request(ui->header->iconify, 24, 24);
  dt_gui_add_class(ui->header->iconify, "window-button");
  dt_ui_titlebar_pack_end(ui, ui->header->iconify);

  ui->header->home = gtk_button_new_from_icon_name("go-home", GTK_ICON_SIZE_LARGE_TOOLBAR);
  gtk_widget_set_tooltip_text(ui->header->home, _("Go back to lighttable"));
  g_signal_connect(G_OBJECT(ui->header->home), "clicked", _home_callback, NULL);
  gtk_widget_set_size_request(ui->header->home, 24, 24);
  dt_gui_add_class(ui->header->home, "window-button");
  dt_ui_titlebar_pack_end(ui, ui->header->home);
  gtk_widget_show(ui->header->home);

  GtkWidget *spacer = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
  dt_ui_titlebar_pack_end(ui, spacer);
  gtk_widget_show(spacer);

  /* Init hinter */
  ui->header->hinter = gtk_label_new("");
  gtk_label_set_ellipsize(GTK_LABEL(ui->header->hinter), PANGO_ELLIPSIZE_END);
  gtk_widget_set_name(ui->header->hinter, "hinter");
  gtk_widget_set_halign(ui->header->hinter, GTK_ALIGN_END);
  gtk_label_set_justify(GTK_LABEL(ui->header->hinter), GTK_JUSTIFY_RIGHT);
  gtk_label_set_line_wrap(GTK_LABEL(ui->header->hinter), TRUE);
  dt_ui_titlebar_pack_end(ui, ui->header->hinter);
  gtk_widget_show(ui->header->hinter);

  spacer = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
  dt_ui_titlebar_pack_end(ui, spacer);
  gtk_widget_show(spacer);

  /* Image info */
  ui->header->image_info = gtk_label_new("");
  gtk_label_set_ellipsize(GTK_LABEL(ui->header->image_info), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_set_name(ui->header->image_info, "image-info");
  dt_ui_titlebar_pack_end(ui, ui->header->image_info);
  gtk_widget_show(ui->header->image_info);
}

void dt_ui_set_image_info_label(dt_ui_t *ui, const char *label)
{
  if(IS_NULL_PTR(ui) || IS_NULL_PTR(ui->header) || !GTK_IS_LABEL(ui->header->image_info)) return;
  gtk_label_set_markup(GTK_LABEL(ui->header->image_info), label);
}

void dt_ui_set_window_buttons_visible(dt_ui_t *ui, gboolean visible)
{
  gtk_widget_set_visible(ui->header->close, visible);
  gtk_widget_set_visible(ui->header->iconify, visible);
}

void dt_hinter_set_message(dt_ui_t *ui, const char *message)
{
  if(IS_NULL_PTR(ui) || IS_NULL_PTR(ui->header) || !GTK_IS_LABEL(ui->header->hinter)) return;
  // Remove hacky attempts of line wrapping with hardcoded newline :
  // Line wrap is handled by Gtk at the label scope.
  char **split = g_strsplit(message, "\n", -1);
  char *joined = g_strjoinv(", ", split);
  gtk_label_set_markup(GTK_LABEL(ui->header->hinter), joined);
  dt_free(joined);
  g_strfreev(split);
}


void dt_ui_cleanup_titlebar(dt_ui_t *ui)
{
  if(IS_NULL_PTR(ui) || IS_NULL_PTR(ui->header)) return;

  if(!IS_NULL_PTR(ui->header->titlebar) && GTK_IS_WIDGET(ui->header->titlebar))
  {
    gtk_widget_destroy(ui->header->titlebar);
    ui->header->titlebar = NULL;
  }

  for(int i = 0; i < DT_MENU_LAST; i++)
  {
    g_list_free(ui->header->item_lists[i]);
    ui->header->item_lists[i] = NULL;
  }
  dt_free(ui->header);
  ui->header = NULL;
}
