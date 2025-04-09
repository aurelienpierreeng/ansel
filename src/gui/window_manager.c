#include "control/control.h"
#include "views/view.h"
#include "gui/window_manager.h"
#include "gui/actions/menu.h"
#include "dtgtk/sidepanel.h"

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
  GtkWidget *restore;
} dt_header_t;

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

  g_free(key);
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
  }
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
    int win_w, win_h;
    gtk_window_get_size(GTK_WINDOW(window), &win_w, &win_h);

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
      sx = CLAMP(sx + darktable.gui->widgets.panel_handle_x - x, 150, win_w / 2);
      key = panels_get_panel_path(DT_UI_PANEL_RIGHT, "_size");
      gtk_widget_set_size_request(widget, sx, -1);
    }
    else if(strcmp(gtk_widget_get_name(w), "panel-handle-left") == 0)
    {
      sx = CLAMP(sx - darktable.gui->widgets.panel_handle_x + x, 150, win_w / 2);
      key = panels_get_panel_path(DT_UI_PANEL_LEFT, "_size");
      gtk_widget_set_size_request(widget, sx, -1);
    }
    else if(strcmp(gtk_widget_get_name(w), "panel-handle-bottom") == 0)
    {
      // yes, we write sx for uniformity with the others
      sx = CLAMP((sy + darktable.gui->widgets.panel_handle_y - y), 48, win_h / 3.);
      key = panels_get_panel_path(DT_UI_PANEL_BOTTOM, "_size");
      gtk_widget_set_size_request(widget, -1, sx);
    }

    // we store and apply the new value
    dt_conf_set_int(key, sx);
    g_free(key);

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
  _ui_init_panel_size(widget, ui);

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
  _ui_init_panel_size(widget, ui);

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


void dt_ui_init_titlebar(dt_ui_t *ui)
{
  ui->header = g_malloc0(sizeof(dt_header_t));

  // Remove useless desktop environment titlebar. We will handle closing buttons internally
  ui->header->titlebar = gtk_header_bar_new();
  gtk_widget_set_name(ui->header->titlebar, "top-first-line");
  gtk_widget_set_size_request(ui->header->titlebar, -1, -1);
  gtk_window_set_titlebar(GTK_WINDOW(ui->main_window), ui->header->titlebar);

  // Reset header bar properties
  gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(ui->header->titlebar), FALSE);
  gtk_header_bar_set_decoration_layout(GTK_HEADER_BAR(ui->header->titlebar), NULL);

  // Gtk mandatorily adds an empty label that is still "visible" for the title.
  // Since it's centered, it can collide with the hinter width.
  // Plus it adds mandatory padding. AKA scrap that.
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_header_bar_set_custom_title(GTK_HEADER_BAR(ui->header->titlebar), box);
  gtk_widget_set_no_show_all(box, TRUE);

  ui->header->menu_bar = gtk_menu_bar_new();
  gtk_widget_set_name(ui->header->menu_bar, "menu-bar");
}

void _home_callback()
{
  dt_ctl_switch_mode_to("lighttable");
}

void _close_callback(GtkWidget *w, gpointer data)
{
  gtk_window_close(GTK_WINDOW((GtkWidget *)data));
}

void _restore_callback(GtkWidget *w, gpointer data)
{
  GtkWindow *window = GTK_WINDOW((GtkWidget *)data);
  if(gtk_window_is_maximized(window))
    gtk_window_unmaximize(window);
  else
    gtk_window_maximize(window);
}

void _iconify_callback(GtkWidget *w, gpointer data)
{
  gtk_window_iconify(GTK_WINDOW((GtkWidget *)data));
}

void dt_ui_init_global_menu(dt_ui_t *ui)
{

  /* Init top-level menus */
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
  append_run(ui->header->menus, &ui->header->item_lists[DT_MENU_RUN], DT_MENU_RUN);
  append_display(ui->header->menus, &ui->header->item_lists[DT_MENU_DISPLAY], DT_MENU_DISPLAY);
  append_views(ui->header->menus, &ui->header->item_lists[DT_MENU_ATELIERS], DT_MENU_ATELIERS);
  append_help(ui->header->menus, &ui->header->item_lists[DT_MENU_HELP], DT_MENU_HELP);

  gtk_header_bar_pack_start(GTK_HEADER_BAR(ui->header->titlebar), ui->header->menu_bar);
  gtk_widget_show_all(ui->header->menu_bar);

  // From there, we pack_end meaning it should be done in reverse order of appearance
  ui->header->close = gtk_button_new_from_icon_name("window-close", GTK_ICON_SIZE_LARGE_TOOLBAR);
  g_signal_connect(G_OBJECT(ui->header->close), "clicked", G_CALLBACK(_close_callback), ui->main_window);
  gtk_widget_set_size_request(ui->header->close, 24, 24);
  dt_gui_add_class(ui->header->close, "window-button");
  gtk_header_bar_pack_end(GTK_HEADER_BAR(ui->header->titlebar), ui->header->close);
  gtk_widget_show(ui->header->close);

  ui->header->restore = gtk_button_new_from_icon_name("window-restore", GTK_ICON_SIZE_LARGE_TOOLBAR);
  g_signal_connect(G_OBJECT(ui->header->restore), "clicked", G_CALLBACK(_restore_callback), ui->main_window);
  gtk_widget_set_size_request(ui->header->restore, 24, 24);
  dt_gui_add_class(ui->header->restore, "window-button");
  gtk_header_bar_pack_end(GTK_HEADER_BAR(ui->header->titlebar), ui->header->restore);
  gtk_widget_show(ui->header->restore);

  ui->header->iconify = gtk_button_new_from_icon_name("window-minimize", GTK_ICON_SIZE_LARGE_TOOLBAR);
  g_signal_connect(G_OBJECT(ui->header->iconify), "clicked", G_CALLBACK(_iconify_callback), ui->main_window);
  gtk_widget_set_size_request(ui->header->iconify, 24, 24);
  dt_gui_add_class(ui->header->iconify, "window-button");
  gtk_header_bar_pack_end(GTK_HEADER_BAR(ui->header->titlebar), ui->header->iconify);
  gtk_widget_show(ui->header->iconify);

  ui->header->home = gtk_button_new_from_icon_name("go-home", GTK_ICON_SIZE_LARGE_TOOLBAR);
  gtk_widget_set_tooltip_text(ui->header->home, _("Go back to lighttable"));
  g_signal_connect(G_OBJECT(ui->header->home), "clicked", _home_callback, NULL);
  gtk_widget_set_size_request(ui->header->home, 24, 24);
  dt_gui_add_class(ui->header->home, "window-button");
  gtk_header_bar_pack_end(GTK_HEADER_BAR(ui->header->titlebar), ui->header->home);
  gtk_widget_show(ui->header->home);

  GtkWidget *spacer = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(ui->header->titlebar), spacer);
  gtk_widget_show(spacer);

  /* Init hinter */
  ui->header->hinter = gtk_label_new("");
  gtk_label_set_ellipsize(GTK_LABEL(ui->header->hinter), PANGO_ELLIPSIZE_END);
  gtk_widget_set_name(ui->header->hinter, "hinter");
  gtk_widget_set_halign(ui->header->hinter, GTK_ALIGN_END);
  gtk_label_set_justify(GTK_LABEL(ui->header->hinter), GTK_JUSTIFY_RIGHT);
  gtk_label_set_line_wrap(GTK_LABEL(ui->header->hinter), TRUE);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(ui->header->titlebar), ui->header->hinter);
  gtk_widget_show(ui->header->hinter);
}

void dt_hinter_set_message(dt_ui_t *ui, const char *message)
{
  // Remove hacky attempts of line wrapping with hardcoded newline :
  // Line wrap is handled by Gtk at the label scope.
  char **split = g_strsplit(message, "\n", -1);
  gtk_label_set_markup(GTK_LABEL(ui->header->hinter), g_strjoinv(", ", split));
  g_strfreev(split);
}


void dt_ui_cleanup_titlebar(dt_ui_t *ui)
{
  for(int i = 0; i < DT_MENU_LAST; i++)
    g_list_free_full(ui->header->item_lists[i], g_free);
  g_free(ui->header);
}
