#include "common/darktable.h"
#include "common/collection.h"
#include "control/control.h"
#include "gui/actions/menu.h"
#include "gui/gtk.h"

#include "gui/window_manager.h"

#include <gtk/gtk.h>


/** FULL SCREEN MODE **/
gboolean full_screen_checked_callback(GtkWidget *w)
{
  GtkWidget *widget = dt_ui_main_window(darktable.gui->ui);
  return gdk_window_get_state(gtk_widget_get_window(widget)) & GDK_WINDOW_STATE_FULLSCREEN;
}

static gboolean full_screen_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  GtkWidget *window = dt_ui_main_window(darktable.gui->ui);

  if(full_screen_checked_callback(window))
  {
    gtk_window_unfullscreen(GTK_WINDOW(window));

    // workaround for GTK Quartz backend bug
    gtk_window_set_title(GTK_WINDOW(window), "Ansel");

    // Hide window controls
    dt_ui_set_window_buttons_visible(darktable.gui->ui, FALSE);
  }
  else
  {
    gtk_window_fullscreen(GTK_WINDOW(window));

    // workaround for GTK Quartz backend bug
    gtk_window_set_title(GTK_WINDOW(window), "Ansel Preview");

    // Show window controls
    dt_ui_set_window_buttons_visible(darktable.gui->ui, TRUE);
  }

  // Mac OS workaround: always re-anchor the window to the bottom of the screen
  GdkWindow *win = gtk_widget_get_window(window);
  GdkDisplay *display = gtk_widget_get_display(window);
  GdkMonitor *monitor = gdk_display_get_monitor_at_window(display, win);
  GdkRectangle geometry;
  gdk_monitor_get_geometry(monitor, &geometry);

  int w, h;
  gtk_window_get_size(GTK_WINDOW(window), &w, &h);
  gtk_window_move(GTK_WINDOW(window), geometry.width - geometry.x - w, geometry.height - geometry.y - h);

  dt_dev_pixelpipe_change_zoom_main(darktable.develop);
  dt_dev_process_all(darktable.develop);

  return TRUE;
}

/** SIDE PANELS COLLAPSE **/
static gboolean _panel_is_visible(dt_ui_panel_t panel)
{
  gchar *key = panels_get_view_path("panel_collaps_state");
  if(dt_conf_get_int(key))
  {
    g_free(key);
    return FALSE;
  }
  key = panels_get_panel_path(panel, "_visible");
  const gboolean ret = dt_conf_get_bool(key);
  g_free(key);
  return ret;
}

static gboolean _toggle_side_borders_accel_callback(GtkAccelGroup *accel_group, GObject *accelerable, guint keyval,
                                                GdkModifierType modifier, gpointer data)
{
  dt_ui_toggle_panels_visibility(darktable.gui->ui);

  /* trigger invalidation of centerview to reprocess pipe */
  dt_dev_pixelpipe_change_zoom_main(darktable.develop);
  dt_dev_process_all(darktable.develop);
  return TRUE;
}

void dt_ui_toggle_panels_visibility(dt_ui_t *ui)
{
  gchar *key = panels_get_view_path("panel_collaps_state");
  const uint32_t state = dt_conf_get_int(key);

  if(state) dt_conf_set_int(key, 0);
  else dt_conf_set_int(key, 1);

  dt_ui_restore_panels(ui);
  g_free(key);
}

void dt_ui_panel_show(dt_ui_t *ui, const dt_ui_panel_t p, gboolean show, gboolean write)
{
  g_return_if_fail(GTK_IS_WIDGET(ui->panels[p]));

  // for left and right sides, panels are inside a gtkoverlay
  GtkWidget *over_panel = NULL;
  if(p == DT_UI_PANEL_LEFT || p == DT_UI_PANEL_RIGHT || p == DT_UI_PANEL_BOTTOM)
    over_panel = gtk_widget_get_parent(ui->panels[p]);

  if(show)
  {
    gtk_widget_show(ui->panels[p]);
    if(over_panel) gtk_widget_show(over_panel);
  }
  else
  {
    gtk_widget_hide(ui->panels[p]);
    if(over_panel) gtk_widget_hide(over_panel);
  }

  if(write)
  {
    gchar *key;
    if(show)
    {
      // we reset the collaps_panel value if we show a panel
      key = panels_get_view_path("panel_collaps_state");
      if(dt_conf_get_int(key) != 0)
      {
        dt_conf_set_int(key, 0);
        g_free(key);
        // we ensure that all panels state are recorded as hidden
        for(int k = 0; k < DT_UI_PANEL_SIZE; k++)
        {
          key = panels_get_panel_path(k, "_visible");
          dt_conf_set_bool(key, FALSE);
          g_free(key);
        }
      }
      else
        g_free(key);
      key = panels_get_panel_path(p, "_visible");
      dt_conf_set_bool(key, show);
      g_free(key);
    }
    else
    {
      // if it was the last visible panel, we set collaps_panel value instead
      // so collapsing panels after will have an effect
      gboolean collapse = TRUE;
      for(int k = 0; k < DT_UI_PANEL_SIZE; k++)
      {
        if(k != p && dt_ui_panel_visible(ui, k))
        {
          collapse = FALSE;
          break;
        }
      }

      if(collapse)
      {
        key = panels_get_view_path("panel_collaps_state");
        dt_conf_set_int(key, 1);
        g_free(key);
      }
      else
      {
        key = panels_get_panel_path(p, "_visible");
        dt_conf_set_bool(key, show);
        g_free(key);
      }
    }
  }
}

gboolean dt_ui_panel_visible(dt_ui_t *ui, const dt_ui_panel_t p)
{
  g_return_val_if_fail(GTK_IS_WIDGET(ui->panels[p]), FALSE);
  return gtk_widget_get_visible(ui->panels[p]);
}


static gboolean panel_left_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_LEFT, !_panel_is_visible(DT_UI_PANEL_LEFT), TRUE);
  return TRUE;
}

static gboolean panel_left_checked_callback(GtkWidget *widget)
{
  return dt_ui_panel_visible(darktable.gui->ui, DT_UI_PANEL_LEFT);
}

static gboolean panel_top_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_TOP, !_panel_is_visible(DT_UI_PANEL_TOP), TRUE);
  return TRUE;
}

static gboolean panel_top_checked_callback(GtkWidget *widget)
{
  return dt_ui_panel_visible(darktable.gui->ui, DT_UI_PANEL_TOP);
}

static gboolean available_in_lighttable_callback()
{
  // Filmstrip is not visible in lighttable
  const dt_view_t *view = dt_view_manager_get_current_view(darktable.view_manager);
  return (view && strcmp(view->module_name, "lighttable"));
}


static gboolean panel_right_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  if(available_in_lighttable_callback())
    dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_RIGHT, !_panel_is_visible(DT_UI_PANEL_RIGHT), TRUE);

  return TRUE;
}

static gboolean panel_right_checked_callback(GtkWidget *widget)
{
  return dt_ui_panel_visible(darktable.gui->ui, DT_UI_PANEL_RIGHT);
}

static gboolean filmstrip_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  if(available_in_lighttable_callback())
    dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_BOTTOM, !_panel_is_visible(DT_UI_PANEL_BOTTOM), TRUE);

  return TRUE;
}

static gboolean filmstrip_checked_callback(GtkWidget *widget)
{
  return dt_ui_panel_visible(darktable.gui->ui, DT_UI_PANEL_BOTTOM);
}

static gboolean profile_checked_callback(GtkWidget *widget)
{
  dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)get_custom_data(widget);
  return (prof->type == darktable.color_profiles->display_type
          && (prof->type != DT_COLORSPACE_FILE
              || !strcmp(prof->filename, darktable.color_profiles->display_filename)));
}

static gboolean profile_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)get_custom_data(GTK_WIDGET(user_data));

  gboolean profile_changed = FALSE;
  if(darktable.color_profiles->display_type != pp->type
      || (darktable.color_profiles->display_type == DT_COLORSPACE_FILE
           && strcmp(darktable.color_profiles->display_filename, pp->filename)))
  {
    darktable.color_profiles->display_type = pp->type;
    g_strlcpy(darktable.color_profiles->display_filename, pp->filename,
              sizeof(darktable.color_profiles->display_filename));
    profile_changed = TRUE;
  }

  if(!profile_changed)
  {
    // profile not found, fall back to system display profile. shouldn't happen
    fprintf(stderr, "can't find display profile `%s', using system display profile instead\n", pp->filename);
    profile_changed = darktable.color_profiles->display_type != DT_COLORSPACE_DISPLAY;
    darktable.color_profiles->display_type = DT_COLORSPACE_DISPLAY;
    darktable.color_profiles->display_filename[0] = '\0';
  }

  if(profile_changed)
  {
    pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);
    dt_colorspaces_update_display_transforms();
    pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED, DT_COLORSPACES_PROFILE_TYPE_DISPLAY);
  }
  return TRUE;
}

dt_iop_color_intent_t string_to_color_intent(const char *string)
{
  if(!strcmp(string, "perceptual"))
    return DT_INTENT_PERCEPTUAL;
  else if(!strcmp(string, "relative colorimetric"))
    return DT_INTENT_RELATIVE_COLORIMETRIC;
  else if(!strcmp(string, "saturation"))
    return DT_INTENT_SATURATION;
  else if(!strcmp(string, "absolute colorimetric"))
    return DT_INTENT_ABSOLUTE_COLORIMETRIC;
  else
    return DT_INTENT_PERCEPTUAL;

  // Those seem to make no difference with most ICC profiles anyway.
  // Perceptual needs A_to_B and B_to_A LUT defined in the .icc profile to work.
  // Since most profiles don't have them, it falls back to something close to relative colorimetric.
  // Really not sure if it's our implementation or if it's LittleCMS2 that is faulty here.
  // This option just makes it look like pRoFESsional CoLoR mAnAgeMEnt®©.
  // ICC intents are pretty much bogus in the first place... (gamut mapping by RGB clipping...)
}

static gboolean intent_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  dt_iop_color_intent_t old_intent = darktable.color_profiles->display_intent;
  dt_iop_color_intent_t new_intent = string_to_color_intent(get_custom_data(GTK_WIDGET(user_data)));
  if(new_intent != old_intent)
  {
    darktable.color_profiles->display_intent = new_intent;
    pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);
    dt_colorspaces_update_display_transforms();
    pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED, DT_COLORSPACES_PROFILE_TYPE_DISPLAY);
  }
  return TRUE;
}

static gboolean intent_checked_callback(GtkWidget *widget)
{
  return darktable.color_profiles->display_intent == string_to_color_intent(get_custom_data(widget));
}

static gboolean always_hide_overlays_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  dt_thumbtable_set_overlays_mode(darktable.gui->ui->thumbtable_lighttable, DT_THUMBNAIL_OVERLAYS_NONE);
  dt_thumbtable_set_overlays_mode(darktable.gui->ui->thumbtable_filmstrip, DT_THUMBNAIL_OVERLAYS_NONE);
  return TRUE;
}

static gboolean always_hide_overlays_checked_callback(GtkWidget *widget)
{
  return dt_conf_get_int("plugins/lighttable/overlays/global") == DT_THUMBNAIL_OVERLAYS_NONE;
}

static gboolean hover_overlays_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  dt_thumbtable_set_overlays_mode(darktable.gui->ui->thumbtable_lighttable, DT_THUMBNAIL_OVERLAYS_HOVER_NORMAL);
  dt_thumbtable_set_overlays_mode(darktable.gui->ui->thumbtable_filmstrip, DT_THUMBNAIL_OVERLAYS_HOVER_NORMAL);
  return TRUE;
}

static gboolean hover_overlays_checked_callback(GtkWidget *widget)
{
  return dt_conf_get_int("plugins/lighttable/overlays/global") == DT_THUMBNAIL_OVERLAYS_HOVER_NORMAL;
}

static gboolean always_show_overlays_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  dt_thumbtable_set_overlays_mode(darktable.gui->ui->thumbtable_lighttable, DT_THUMBNAIL_OVERLAYS_ALWAYS_NORMAL);
  dt_thumbtable_set_overlays_mode(darktable.gui->ui->thumbtable_filmstrip, DT_THUMBNAIL_OVERLAYS_ALWAYS_NORMAL);
  return TRUE;
}

static gboolean always_show_overlays_checked_callback(GtkWidget *widget)
{
  return dt_conf_get_int("plugins/lighttable/overlays/global") == DT_THUMBNAIL_OVERLAYS_ALWAYS_NORMAL;
}

static gboolean group_borders_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  gboolean borders = !dt_conf_get_bool("plugins/lighttable/group_borders");
  dt_conf_set_bool("plugins/lighttable/group_borders", borders);
  dt_thumbtable_set_draw_group_borders(darktable.gui->ui->thumbtable_lighttable, borders);
  dt_thumbtable_set_draw_group_borders(darktable.gui->ui->thumbtable_filmstrip, borders);
  return TRUE;
}

static gboolean group_borders_checked_callback()
{
  return dt_conf_get_bool("plugins/lighttable/group_borders");
}

static gboolean collapse_grouped_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  dt_conf_set_bool("ui_last/grouping", !dt_conf_get_bool("ui_last/grouping"));
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_GROUPING, NULL);
  return TRUE;
}

static gboolean collapse_grouped_checked_callback()
{
  return dt_conf_get_bool("ui_last/grouping");
}

static gboolean _jpg_checked(GtkWidget *widget)
{
  const int item = GPOINTER_TO_INT(get_custom_data(widget));
  return item == dt_conf_get_int("lighttable/embedded_jpg");
}

static gboolean _jpg_combobox_changed(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  const int mode = GPOINTER_TO_INT(get_custom_data(GTK_WIDGET(user_data)));
  if(mode != dt_conf_get_int("lighttable/embedded_jpg"))
  {
    GList *imgs = dt_collection_get_all(darktable.collection, -1);

    // Empty the mipmap cache for the current collection, but only on RAM
    // Don't delete disk cache, but RAM cache may be flushed to disk if user param sets it.
    for(GList *img = g_list_first(imgs); img; img = g_list_next(img))
    {
      const int32_t imgid = GPOINTER_TO_INT(img->data);
      dt_mipmap_cache_remove(darktable.mipmap_cache, imgid, FALSE);
    }
    g_list_free(imgs);

    // Change the mode
    dt_conf_set_int("lighttable/embedded_jpg", mode);

    // Redraw thumbnails
    dt_thumbtable_refresh_thumbnail(darktable.gui->ui->thumbtable_lighttable, UNKNOWN_IMAGE, TRUE);
  }
  return TRUE;
}

void append_display(GtkWidget **menus, GList **lists, const dt_menus_t index)
{
  // Parent sub-menu color profile
  add_top_submenu_entry(menus, lists, _("Monitor color profile"), index);
  GtkWidget *parent = get_last_widget(lists);

  // Add available color profiles to the sub-menu
  for(const GList *l = darktable.color_profiles->profiles; l; l = g_list_next(l))
  {
    dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)l->data;
    if(prof->display_pos > -1)
    {
      add_sub_sub_menu_entry(menus, parent, lists, prof->name, index, prof, profile_callback, profile_checked_callback, NULL, NULL, 0, 0);
      //gtk_check_menu_item_set_draw_as_radio(GTK_CHECK_MENU_ITEM(get_last_widget(lists)), TRUE);
    }
  }

  // Parent sub-menu profile intent
  add_top_submenu_entry(menus, lists, _("Monitor color intent"), index);
  parent = get_last_widget(lists);

  const char *intents[4] = { _("Perceptual"), _("Relative colorimetric"), C_("rendering intent", "Saturation"),
                             _("Absolute colorimetric") };
  // non-translatable strings to store in menu items for later mapping with dt_iop_color_intent_t
  const char *data[4] = { "perceptual", "relative colorimetric", "saturation", "absolute colorimetric" };

  for(int i = 0; i < 4; i++)
    add_sub_sub_menu_entry(menus, parent, lists, intents[i], index, (void *)data[i], intent_callback, intent_checked_callback, NULL, NULL, 0, 0);

  add_menu_separator(menus[index]);

  // Parent sub-menu panels
  add_top_submenu_entry(menus, lists, _("Panels"), index);
  parent = get_last_widget(lists);

  // Children of sub-menu panels
  add_sub_sub_menu_entry(menus, parent, lists, _("Top"), index, NULL, panel_top_callback,
                         panel_top_checked_callback, NULL, NULL, GDK_KEY_t, GDK_CONTROL_MASK | GDK_SHIFT_MASK);

  add_sub_sub_menu_entry(menus, parent, lists, _("Left"), index, NULL,
                         panel_left_callback, panel_left_checked_callback, NULL, NULL, GDK_KEY_l, GDK_CONTROL_MASK | GDK_SHIFT_MASK);

  add_sub_sub_menu_entry(menus, parent, lists, _("Right"), index, NULL,
                         panel_right_callback, panel_right_checked_callback, NULL, available_in_lighttable_callback, GDK_KEY_r, GDK_CONTROL_MASK | GDK_SHIFT_MASK);

  add_sub_sub_menu_entry(menus, parent, lists, _("Filmstrip"), index, NULL,
                         filmstrip_callback, filmstrip_checked_callback, NULL, available_in_lighttable_callback, GDK_KEY_f, GDK_CONTROL_MASK | GDK_SHIFT_MASK);

  add_menu_separator(menus[index]);

  // Lighttable & Filmstrip options
  add_top_submenu_entry(menus, lists, _("Thumbnail overlays"), index);
  parent = get_last_widget(lists);

  add_sub_sub_menu_entry(menus, parent, lists, _("Always hide"), index, NULL,
                         always_hide_overlays_callback, always_hide_overlays_checked_callback, NULL, NULL, GDK_KEY_h, GDK_CONTROL_MASK | GDK_SHIFT_MASK);

  add_sub_sub_menu_entry(menus, parent, lists, _("Show on hover"), index, NULL,
                         hover_overlays_callback, hover_overlays_checked_callback, NULL, NULL, 0, 0);

  add_sub_sub_menu_entry(menus, parent, lists, _("Always show"), index, NULL,
                         always_show_overlays_callback, always_show_overlays_checked_callback, NULL, NULL, GDK_KEY_o, GDK_CONTROL_MASK | GDK_SHIFT_MASK);

  // Submenu embedded JPEG
  add_top_submenu_entry(menus, lists, _("Show embedded JPEG"), index);
  parent = get_last_widget(lists);
  add_sub_sub_menu_entry(menus, parent, lists, _("Never, always process the raw"), index, GINT_TO_POINTER(0),
                                  _jpg_combobox_changed, _jpg_checked, NULL, NULL, 0, 0);
  add_sub_sub_menu_entry(menus, parent, lists, _("For unedited pictures"), index, GINT_TO_POINTER(1),
                                  _jpg_combobox_changed, _jpg_checked, NULL, NULL, 0, 0);
  add_sub_sub_menu_entry(menus, parent, lists, _("Always, never process the raw"), index, GINT_TO_POINTER(2),
                                  _jpg_combobox_changed, _jpg_checked, NULL, NULL, 0, 0);

  add_sub_menu_entry(menus, lists, _("Collapse grouped images"), index, NULL, collapse_grouped_callback, collapse_grouped_checked_callback, NULL, NULL, 0, 0);

  add_sub_menu_entry(menus, lists, _("Show group borders"), index, NULL, group_borders_callback,
                     group_borders_checked_callback, NULL, NULL, GDK_KEY_p, GDK_CONTROL_MASK | GDK_SHIFT_MASK);

  add_menu_separator(menus[index]);

  add_sub_menu_entry(menus, lists, _("Full screen"), index, NULL, full_screen_callback,
                     full_screen_checked_callback, NULL, NULL, GDK_KEY_F11, 0);

  dt_accels_new_global_action(_toggle_side_borders_accel_callback, NULL, N_("Global/Actions"), N_("Toggle all panels visibility"), GDK_KEY_F11, GDK_SHIFT_MASK, _("Triggers the action"));
}
