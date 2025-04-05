#include "control/control.h"
#include "views/view.h"
#include "gui/window_manager.h"
#include "dtgtk/sidepanel.h"

#define WINDOW_DEBUG 0


const char *_ui_panel_config_names[]
    = { "header", "toolbar_top", "toolbar_bottom", "left", "right", "bottom" };


gchar * panels_get_view_path(char *suffix)
{

  if(!darktable.view_manager) return NULL;
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(!cv) return NULL;
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
  if(!v) return NULL;
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
      g_free(key);
    }
    else // size hasn't been adjusted, so return default sizes
    {
      if(p == DT_UI_PANEL_BOTTOM)
        size = DT_UI_PANEL_BOTTOM_DEFAULT_SIZE;
      else
        size = DT_UI_PANEL_SIDE_DEFAULT_SIZE;
    }
    return size;
  }
  return -1;
}

void dt_ui_panel_set_size(dt_ui_t *ui, const dt_ui_panel_t p, int s)
{
  gchar *key = NULL;

  if(p == DT_UI_PANEL_LEFT || p == DT_UI_PANEL_RIGHT || p == DT_UI_PANEL_BOTTOM)
  {
    const int width = CLAMP(s, dt_conf_get_int("min_panel_width"), dt_conf_get_int("max_panel_width"));
    gtk_widget_set_size_request(ui->panels[p], width, -1);
    key = panels_get_panel_path(p, "_size");
    dt_conf_set_int(key, width);
    g_free(key);
  }
}

static void refresh_manager_sizes(dt_ui_t *ui)
{
  // GUI sizes to data representation

  dt_window_manager_t *manager = &ui->manager;
  GtkWidget *window = dt_ui_main_window(ui);
  GdkWindow *win = gtk_widget_get_window(window);
  GdkMonitor *monitor = gdk_display_get_monitor_at_window(gdk_window_get_display(win), win);

  // Note : all sizes are in viewport pixels, not physical pixels.
  // physical pix = viewport pix * gdk_monitor_get_scale_factor(monitor);
  // the scale factor is the highDPI factor set on desktop environment if any.

  // Display in which the current window fits
  gdk_monitor_get_geometry(monitor, &manager->viewport);

#if WINDOW_DEBUG
  fprintf(stdout, "viewport: %i x %i\n", manager->viewport.width, manager->viewport.height);
#endif

  // Main window
  gtk_window_get_size(GTK_WINDOW(window), &manager->window.width, &manager->window.height);

#if WINDOW_DEBUG
  fprintf(stdout, "main window: %i x %i\n", manager->window.width, manager->window.height);
#endif

  gdk_window_get_origin(gtk_widget_get_window(window), &manager->window.x, &manager->window.y);
  //gtk_window_get_position(GTK_WINDOW(window), &manager->window.x, &manager->window.y);

#if WINDOW_DEBUG
  fprintf(stdout, "position : %i, %i\n", manager->window.x, manager->window.y);
#endif

  // Panels (sidebars, menubar, toolbars, filmstrip)
  for(int i = 0; i < DT_UI_PANEL_SIZE; i++)
  {
    gtk_widget_get_allocation(GTK_WIDGET(ui->panels[i]), &manager->panels[i]);

#if WINDOW_DEBUG
    fprintf(stdout, "panel %i : %i x %i\n", i, manager->panels[i].width, manager->panels[i].height);
#endif
  }

  // Center view
  gtk_widget_get_allocation(dt_ui_center(ui), &manager->center);

#if WINDOW_DEBUG
  fprintf(stdout, "center: %i x %i\n", manager->center.width, manager->center.height);
#endif

}

/*
* Problem :
*  Gtk sets the size of containers by adding the size of their children,
*  with zero fuck given for the ability of the final window to fit within the screen area.
*  When users resize sidebars, the window width may increase indefinitely.
*  There is nothing we can do here, because widget_set_size_request defines a wish,
*  and widget_get_allocation defines the finally rendered size, but nothing advertised the
*  minimal size. So we can't fetch the min width of the central area and sanitize sidebars widths
*  as to ensure `window width - min central width = left sidebar width + right sidebar width`.
*/

static void sanitize_manager_size(dt_ui_t *ui)
{
  dt_window_manager_t *manager = &ui->manager;

  // Ensure window fits in viewport NOT taking top-left corner position into account
  manager->window.width = MIN(manager->window.width, manager->viewport.width);
  manager->window.height = MIN(manager->window.height, manager->viewport.height);

#if WINDOW_DEBUG
  fprintf(stdout, "new window size: %i x %i\n", manager->window.width, manager->window.height);
#endif
  // Warning : the window.height doesn't account for the titlebar/decoration set by desktop manager.
  // The code above assumes zero titlebar height because Gtk doesn't have a way of retrieving this info.
  // Setting window.height to viewport.height doesn't guarantee it fits.
}

static void reset_manager_sizes(dt_ui_t *ui)
{
  dt_window_manager_t *manager = &ui->manager;
  gtk_window_resize(GTK_WINDOW(dt_ui_main_window(ui)), manager->window.width, manager->window.height);
}


static void update_manager_sizes(dt_ui_t *ui)
{
  refresh_manager_sizes(ui);
  sanitize_manager_size(ui);
  reset_manager_sizes(ui);
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
    case DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_RIGHT:
      gtk_box_pack_end(GTK_BOX(ui->containers[c]), w, FALSE, FALSE, 0);
      break;

    /* if box is center we want it to fill as much as it can */
    case DT_UI_CONTAINER_PANEL_TOP_THIRD_ROW:
    case DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_CENTER:
    case DT_UI_CONTAINER_PANEL_TOP_SECOND_ROW:
      gtk_box_pack_start(GTK_BOX(ui->containers[c]), w, TRUE, TRUE, 0);
      break;

    case DT_UI_CONTAINER_PANEL_TOP_FIRST_ROW_LEFT:
      gtk_header_bar_pack_start(GTK_HEADER_BAR(ui->header), w);
      break;

    case DT_UI_CONTAINER_PANEL_TOP_FIRST_ROW_RIGHT:
      gtk_header_bar_pack_end(GTK_HEADER_BAR(ui->header), w);
      break;

    default:
      gtk_box_pack_start(GTK_BOX(ui->containers[c]), w, FALSE, FALSE, 0);
      break;
  }
  gtk_widget_show_all(w);
}

static void _ui_init_panel_size(GtkWidget *widget)
{
  gchar *key = NULL;

  int s = 128;
  if(strcmp(gtk_widget_get_name(widget), "right") == 0)
  {
    key = panels_get_panel_path(DT_UI_PANEL_RIGHT, "_size");
    s = DT_UI_PANEL_SIDE_DEFAULT_SIZE; // default panel size
    if(key && dt_conf_key_exists(key))
      s = CLAMP(dt_conf_get_int(key), 120, darktable.gui->ui->manager.window.width / 3);
    if(key) gtk_widget_set_size_request(widget, s, -1);
  }
  else if(strcmp(gtk_widget_get_name(widget), "left") == 0)
  {
    key = panels_get_panel_path(DT_UI_PANEL_LEFT, "_size");
    s = DT_UI_PANEL_SIDE_DEFAULT_SIZE; // default panel size
    if(key && dt_conf_key_exists(key))
      s = CLAMP(dt_conf_get_int(key), 120, darktable.gui->ui->manager.window.width / 3);
    if(key) gtk_widget_set_size_request(widget, s, -1);
  }
  else if(strcmp(gtk_widget_get_name(widget), "bottom") == 0)
  {
    key = panels_get_panel_path(DT_UI_PANEL_BOTTOM, "_size");
    s = DT_UI_PANEL_BOTTOM_DEFAULT_SIZE; // default panel size
    if(key && dt_conf_key_exists(key))
      s = CLAMP(dt_conf_get_int(key), 64, darktable.gui->ui->manager.window.height / 3);
    if(key) gtk_widget_set_size_request(widget, -1, s);
  }

  g_free(key);
}

void dt_ui_restore_panels(dt_ui_t *ui)
{
  /* restore left & right panel size */
  _ui_init_panel_size(ui->panels[DT_UI_PANEL_LEFT]);
  _ui_init_panel_size(ui->panels[DT_UI_PANEL_RIGHT]);
  _ui_init_panel_size(ui->panels[DT_UI_PANEL_BOTTOM]);

  /* restore from a previous collapse all panel state if enabled */
  gchar *key = panels_get_view_path("panel_collaps_state");
  const uint32_t state = dt_conf_get_int(key);
  g_free(key);
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

      g_free(key);
    }

    // Force main menu to remain visible. Many users hide the top header bar in Darktable.
    // Coming to Ansel, they don't realize there is a menu there.
    dt_ui_panel_show(ui, DT_UI_PANEL_TOP, TRUE, TRUE);
  }

  update_manager_sizes(ui);
}

static gboolean _panel_handle_button_callback(GtkWidget *w, GdkEventButton *e, gpointer user_data)
{
  if(e->button == 1)
  {
    if(e->type == GDK_BUTTON_PRESS)
    {
      /* store current  mousepointer position */
      gdk_window_get_device_position(e->window,
                                     gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_window_get_display(
                                         gtk_widget_get_window(dt_ui_main_window(darktable.gui->ui))))),
                                     &darktable.gui->widgets.panel_handle_x,
                                     &darktable.gui->widgets.panel_handle_y, 0);

      darktable.gui->widgets.panel_handle_dragging = TRUE;
    }
    else if(e->type == GDK_BUTTON_RELEASE)
    {
      darktable.gui->widgets.panel_handle_dragging = FALSE;
    }
    else if(e->type == GDK_2BUTTON_PRESS)
    {
      darktable.gui->widgets.panel_handle_dragging = FALSE;
    }
  }
  return TRUE;
}

static gboolean _panel_handle_cursor_callback(GtkWidget *w, GdkEventCrossing *e, gpointer user_data)
{
  if(strcmp(gtk_widget_get_name(w), "panel-handle-bottom") == 0)
    dt_control_change_cursor((e->type == GDK_ENTER_NOTIFY) ? GDK_SB_V_DOUBLE_ARROW : GDK_LEFT_PTR);
  else
    dt_control_change_cursor((e->type == GDK_ENTER_NOTIFY) ? GDK_SB_H_DOUBLE_ARROW : GDK_LEFT_PTR);
  return TRUE;
}

static gboolean _panel_handle_motion_callback(GtkWidget *w, GdkEventButton *e, gpointer user_data)
{
  GtkWidget *widget = (GtkWidget *)user_data;
  if(darktable.gui->widgets.panel_handle_dragging)
  {
    gint x, y, sx, sy;
    GtkWidget *window = dt_ui_main_window(darktable.gui->ui);

    // FIXME: can work with the event x,y to skip the gdk_window_get_device_position() call?
    gdk_window_get_device_position(e->window,
                                   gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_window_get_display(
                                       gtk_widget_get_window(window)))),
                                   &x, &y, 0);
    gtk_widget_get_size_request(widget, &sx, &sy);

    // conf entry to store the new size
    gchar *key = NULL;
    if(strcmp(gtk_widget_get_name(w), "panel-handle-right") == 0)
    {
      sx = CLAMP(sx + darktable.gui->widgets.panel_handle_x - x, 150, darktable.gui->ui->manager.window.width / 2);
      key = panels_get_panel_path(DT_UI_PANEL_RIGHT, "_size");
      gtk_widget_set_size_request(widget, sx, -1);
    }
    else if(strcmp(gtk_widget_get_name(w), "panel-handle-left") == 0)
    {
      sx = CLAMP(sx - darktable.gui->widgets.panel_handle_x + x, 150, darktable.gui->ui->manager.window.width / 2);
      key = panels_get_panel_path(DT_UI_PANEL_LEFT, "_size");
      gtk_widget_set_size_request(widget, sx, -1);
    }
    else if(strcmp(gtk_widget_get_name(w), "panel-handle-bottom") == 0)
    {
      sx = CLAMP((sy + darktable.gui->widgets.panel_handle_y - y), 64, darktable.gui->ui->manager.window.height / 2);
      key = panels_get_panel_path(DT_UI_PANEL_BOTTOM, "_size");
      gtk_widget_set_size_request(widget, -1, sx);
    }

    // we store and apply the new value
    dt_conf_set_int(key, sx);
    g_free(key);

    update_manager_sizes(darktable.gui->ui);

    return TRUE;
  }

  return FALSE;
}

/* initialize the top container of panel */
static GtkWidget *_ui_init_panel_container_top(GtkWidget *container)
{
  GtkWidget *w = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_UI_PANEL_MODULE_SPACING);
  gtk_box_pack_start(GTK_BOX(container), w, FALSE, FALSE, 0);
  return w;
}

// this should work as long as everything happens in the gui thread
static void _ui_panel_size_changed(GtkAdjustment *adjustment, GParamSpec *pspec, gpointer user_data)
{
  GtkAllocation allocation;
  static float last_height[2] = { 0 };

  const int side = GPOINTER_TO_INT(user_data);

  // don't do anything when the size didn't actually change.
  const float height = gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_lower(adjustment);

  if(height == last_height[side]) return;
  last_height[side] = height;

  if(!darktable.gui->scroll_to[side]) return;

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
                   G_CALLBACK(_ui_panel_size_changed), GINT_TO_POINTER(left ? 1 : 0));

  /* create the scrolled viewport */
  container = widget;
  widget = gtk_viewport_new(a[2], a[3]);
  gtk_viewport_set_shadow_type(GTK_VIEWPORT(widget), GTK_SHADOW_NONE);
  gtk_container_add(GTK_CONTAINER(container), widget);

  /* create the container */
  container = widget;
  widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(widget, "plugins_box");
  gtk_container_add(GTK_CONTAINER(container), widget);

  return widget;
}

/* initialize the bottom container of panel */
static GtkWidget *_ui_init_panel_container_bottom(GtkWidget *container)
{
  GtkWidget *w = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(container), w, FALSE, FALSE, 0);
  return w;
}

/* initialize the whole left panel */
static void _ui_init_panel_left(dt_ui_t *ui, GtkWidget *container)
{
  GtkWidget *widget;

  /* create left panel main widget and add it to ui */
  darktable.gui->widgets.panel_handle_dragging = FALSE;
  widget = ui->panels[DT_UI_PANEL_LEFT] = dtgtk_side_panel_new();
  gtk_widget_set_name(widget, "left");
  _ui_init_panel_size(widget);

  GtkWidget *over = gtk_overlay_new();
  gtk_container_add(GTK_CONTAINER(over), widget);
  // we add a transparent overlay over the modules margins to resize the panel
  GtkWidget *handle = gtk_drawing_area_new();
  gtk_widget_set_halign(handle, GTK_ALIGN_END);
  gtk_widget_set_valign(handle, GTK_ALIGN_FILL);
  gtk_widget_set_size_request(handle, DT_PIXEL_APPLY_DPI(5), -1);
  gtk_overlay_add_overlay(GTK_OVERLAY(over), handle);
  gtk_widget_set_events(handle, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_ENTER_NOTIFY_MASK
                                    | GDK_LEAVE_NOTIFY_MASK | GDK_POINTER_MOTION_MASK);
  gtk_widget_set_name(GTK_WIDGET(handle), "panel-handle-left");
  g_signal_connect(G_OBJECT(handle), "button-press-event", G_CALLBACK(_panel_handle_button_callback), handle);
  g_signal_connect(G_OBJECT(handle), "button-release-event", G_CALLBACK(_panel_handle_button_callback), handle);
  g_signal_connect(G_OBJECT(handle), "motion-notify-event", G_CALLBACK(_panel_handle_motion_callback), widget);
  g_signal_connect(G_OBJECT(handle), "leave-notify-event", G_CALLBACK(_panel_handle_cursor_callback), handle);
  g_signal_connect(G_OBJECT(handle), "enter-notify-event", G_CALLBACK(_panel_handle_cursor_callback), handle);
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

  /* create left panel main widget and add it to ui */
  darktable.gui->widgets.panel_handle_dragging = FALSE;
  widget = ui->panels[DT_UI_PANEL_RIGHT] = dtgtk_side_panel_new();
  gtk_widget_set_name(widget, "right");
  _ui_init_panel_size(widget);

  GtkWidget *over = gtk_overlay_new();
  gtk_container_add(GTK_CONTAINER(over), widget);
  // we add a transparent overlay over the modules margins to resize the panel
  GtkWidget *handle = gtk_drawing_area_new();
  gtk_widget_set_halign(handle, GTK_ALIGN_START);
  gtk_widget_set_valign(handle, GTK_ALIGN_FILL);
  gtk_widget_set_size_request(handle, DT_PIXEL_APPLY_DPI(5), -1);
  gtk_overlay_add_overlay(GTK_OVERLAY(over), handle);
  gtk_widget_set_events(handle, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_ENTER_NOTIFY_MASK
                                    | GDK_LEAVE_NOTIFY_MASK | GDK_POINTER_MOTION_MASK);
  gtk_widget_set_name(GTK_WIDGET(handle), "panel-handle-right");
  g_signal_connect(G_OBJECT(handle), "button-press-event", G_CALLBACK(_panel_handle_button_callback), handle);
  g_signal_connect(G_OBJECT(handle), "button-release-event", G_CALLBACK(_panel_handle_button_callback), handle);
  g_signal_connect(G_OBJECT(handle), "motion-notify-event", G_CALLBACK(_panel_handle_motion_callback), widget);
  g_signal_connect(G_OBJECT(handle), "leave-notify-event", G_CALLBACK(_panel_handle_cursor_callback), handle);
  g_signal_connect(G_OBJECT(handle), "enter-notify-event", G_CALLBACK(_panel_handle_cursor_callback), handle);
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
  ui->panels[DT_UI_PANEL_TOP] = widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(ui->panels[DT_UI_PANEL_TOP], "top");
  gtk_widget_set_hexpand(GTK_WIDGET(widget), TRUE);
  gtk_grid_attach(GTK_GRID(container), widget, 1, 0, 3, 1);

  /* add container for top center */
  ui->containers[DT_UI_CONTAINER_PANEL_TOP_SECOND_ROW] = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(ui->containers[DT_UI_CONTAINER_PANEL_TOP_SECOND_ROW], "top-second-line");
  gtk_box_pack_start(GTK_BOX(widget), ui->containers[DT_UI_CONTAINER_PANEL_TOP_SECOND_ROW], FALSE, FALSE,
                     DT_UI_PANEL_MODULE_SPACING);

  /* add container for top right */
  ui->containers[DT_UI_CONTAINER_PANEL_TOP_THIRD_ROW] = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(ui->containers[DT_UI_CONTAINER_PANEL_TOP_THIRD_ROW], "top-third-line");
  gtk_box_pack_end(GTK_BOX(widget), ui->containers[DT_UI_CONTAINER_PANEL_TOP_THIRD_ROW], FALSE, FALSE,
                   DT_UI_PANEL_MODULE_SPACING);
}

/* initialize the center top panel */
static void _ui_init_panel_center_top(dt_ui_t *ui, GtkWidget *container)
{
  GtkWidget *widget;

  /* create the panel box */
  ui->panels[DT_UI_PANEL_CENTER_TOP] = widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(widget, "header-toolbar");
  dt_gui_add_class(widget, "dt_big_btn_canvas");
  gtk_box_pack_start(GTK_BOX(container), widget, FALSE, FALSE, 0);
}

/* initialize the center bottom panel */
static void _ui_init_panel_center_bottom(dt_ui_t *ui, GtkWidget *container)
{
  GtkWidget *widget;

  /* create the panel box */
  ui->panels[DT_UI_PANEL_CENTER_BOTTOM] = widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(widget, "footer-toolbar");
  dt_gui_add_class(widget, "dt_big_btn_canvas");
  gtk_box_pack_start(GTK_BOX(container), widget, FALSE, TRUE, 0);

  /* adding the center bottom left toolbox */
  ui->containers[DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_LEFT] = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(widget), ui->containers[DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_LEFT], TRUE, TRUE,
                     DT_UI_PANEL_MODULE_SPACING);

  /* adding the center box */
  ui->containers[DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_CENTER] = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(widget), ui->containers[DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_CENTER], FALSE, TRUE,
                     DT_UI_PANEL_MODULE_SPACING);

  /* adding the right toolbox */
  ui->containers[DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_RIGHT] = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(widget), ui->containers[DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_RIGHT], TRUE, TRUE,
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
  _ui_init_panel_size(ui->thumbtable_filmstrip->parent_overlay);
  gtk_grid_attach(GTK_GRID(container), over, 1, 2, 3, 1);

  // we add a transparent overlay over the modules margins to resize the panel
  GtkWidget *handle = gtk_drawing_area_new();
  gtk_widget_set_halign(handle, GTK_ALIGN_FILL);
  gtk_widget_set_valign(handle, GTK_ALIGN_START);
  gtk_widget_set_size_request(handle, -1, DT_PIXEL_APPLY_DPI(5));
  gtk_overlay_add_overlay(GTK_OVERLAY(over), handle);
  gtk_widget_set_events(handle, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_ENTER_NOTIFY_MASK
                                    | GDK_LEAVE_NOTIFY_MASK | GDK_POINTER_MOTION_MASK);
  gtk_widget_set_name(GTK_WIDGET(handle), "panel-handle-bottom");
  g_signal_connect(G_OBJECT(handle), "button-press-event", G_CALLBACK(_panel_handle_button_callback), handle);
  g_signal_connect(G_OBJECT(handle), "button-release-event", G_CALLBACK(_panel_handle_button_callback), handle);
  g_signal_connect(G_OBJECT(handle), "motion-notify-event", G_CALLBACK(_panel_handle_motion_callback), over);
  g_signal_connect(G_OBJECT(handle), "leave-notify-event", G_CALLBACK(_panel_handle_cursor_callback), handle);
  g_signal_connect(G_OBJECT(handle), "enter-notify-event", G_CALLBACK(_panel_handle_cursor_callback), handle);
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

  /* initialize the top container */
  _ui_init_panel_top(ui, container);

  /* initialize the center top/center/bottom */
  widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand(GTK_WIDGET(widget), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(widget), TRUE);
  gtk_grid_attach(GTK_GRID(container), widget, 2, 1, 1, 1);

  /* initialize the center top panel */
  _ui_init_panel_center_top(ui, widget);

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

  /* initialize panels */
  _ui_init_panel_center_bottom(ui, widget);
  _ui_init_panel_bottom(ui, container);
  _ui_init_panel_left(ui, container);
  _ui_init_panel_right(ui, container);

  gtk_widget_show_all(container);
}

void dt_ui_cleanup_main_table(dt_ui_t *ui)
{
  dt_thumbtable_cleanup(ui->thumbtable_filmstrip);
  dt_thumbtable_cleanup(ui->thumbtable_lighttable);
}
