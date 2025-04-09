#include <gtk/gtk.h>
#include <glib.h>

#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#pragma once

typedef enum dt_menus_t
{
  DT_MENU_FILE = 0,
  DT_MENU_EDIT,
  DT_MENU_SELECTION,
  DT_MENU_IMAGE,
  DT_MENU_STYLES,
  DT_MENU_RUN,
  DT_MENU_DISPLAY,
  DT_MENU_ATELIERS,
  DT_MENU_HELP,
  DT_MENU_LAST
} dt_menus_t;

typedef enum dt_menu_entry_style_t
{
  DT_MENU_ENTRY_DEFAULT = 0,
  DT_MENU_ENTRY_CHECKBUTTON = 1,
  DT_MENU_ENTRY_RADIOBUTTON = 2,
  DT_MENU_ENTRY_LAST
} dt_menu_entry_style_t;


typedef struct dt_menu_entry_t
{
  GtkWidget *widget;         // Bounding box for the whole menu item
  gboolean has_checkbox;     // Show or hide the checkbox, aka "is the current action setting a boolean ?"
  void (*action_callback)(GtkWidget *widget); // Callback to run when the menu item is clicked, aka the action
  gboolean (*sensitive_callback)(GtkWidget *widget);      // Callback checking some conditions to decide whether the menu item should be made insensitive in current context
  gboolean (*checked_callback)(GtkWidget *widget);        // Callback checking some conditions to determine if the boolean pref set by the item is currently TRUE or FALSE
  gboolean (*active_callback)(GtkWidget *widget);         // Callback checking some conditions to determine if the menu item is the current view
  dt_menus_t menu;           // Index of first-level menu
  dt_menu_entry_style_t style;
} dt_menu_entry_t;


/** How to use:
 *  1. write callback functions returning a gboolean that will check the context to decide if
 *  the menu item should be insensitive, checked, active. These should only use the content of
 *  globally accessible structures like `darktable.gui` since they take no arguments.
 *
 *  2. re-use the action callback functions already used for global keyboard shortcuts (actions/accels).
 *  Again, all inputs and internal functions should be globally accessible, for example using proxies.
 *
 *  3. wire everything with the `set_menu_entry` function below. GUI states of the children menu items
 *  will be updated automatically everytime a top-level menu is opened.
 **/

dt_menu_entry_t *
set_menu_entry(GtkWidget **menus, GList **items_list, const gchar *label, dt_menus_t menu_index, void *data,
               void (*action_callback)(GtkWidget *widget), gboolean (*checked_callback)(GtkWidget *widget),
               gboolean (*active_callback)(GtkWidget *widget), gboolean (*sensitive_callback)(GtkWidget *widget),
               guint key_val, GdkModifierType mods, GtkAccelGroup *accel_group);

void update_entry(dt_menu_entry_t *entry);
void update_menu_entries(GtkWidget *widget, gpointer user_data);

// Use for first-level entries in any menubar
void add_generic_top_menu_entry(GtkWidget *menu_bar, GtkWidget **menus, GList **lists, const dt_menus_t index,
                                gchar *label, GtkAccelGroup *accel_group, const char *accel_path_prefix);


// Use for first-level entries in the global menubar
void add_top_menu_entry(GtkWidget *menu_bar, GtkWidget **menus, GList **lists, const dt_menus_t index,
                        gchar *label);

// Special submenus entries that only open a sub-submenu
void add_generic_top_submenu_entry(GtkWidget **menus, GList **lists, const gchar *label, const dt_menus_t index,
                                   GtkAccelGroup *accel_group);

// Global menu only
void add_top_submenu_entry(GtkWidget **menus, GList **lists, const gchar *label, const dt_menus_t index);
void add_generic_sub_menu_entry(GtkWidget **menus, GList **lists, const gchar *label, const dt_menus_t index,
                                void *data, void (*action_callback)(GtkWidget *widget),
                                gboolean (*checked_callback)(GtkWidget *widget),
                                gboolean (*active_callback)(GtkWidget *widget),
                                gboolean (*sensitive_callback)(GtkWidget *widget), guint key_val,
                                GdkModifierType mods, GtkAccelGroup *accel_group);

void add_sub_menu_entry(GtkWidget **menus, GList **lists, const gchar *label, const dt_menus_t index, void *data,
                        void (*action_callback)(GtkWidget *widget),
                        gboolean (*checked_callback)(GtkWidget *widget),
                        gboolean (*active_callback)(GtkWidget *widget),
                        gboolean (*sensitive_callback)(GtkWidget *widget), guint key_val, GdkModifierType mods);

void add_generic_sub_sub_menu_entry(GtkWidget **menus, GtkWidget *parent, GList **lists, const gchar *label,
                                    const dt_menus_t index, void *data, void (*action_callback)(GtkWidget *widget),
                                    gboolean (*checked_callback)(GtkWidget *widget),
                                    gboolean (*active_callback)(GtkWidget *widget),
                                    gboolean (*sensitive_callback)(GtkWidget *widget), guint key_val,
                                    GdkModifierType mods, GtkAccelGroup *accel_group);
void add_sub_sub_menu_entry(GtkWidget **menus, GtkWidget *parent, GList **lists, const gchar *label,
                            const dt_menus_t index, void *data, void (*action_callback)(GtkWidget *widget),
                            gboolean (*checked_callback)(GtkWidget *widget),
                            gboolean (*active_callback)(GtkWidget *widget),
                            gboolean (*sensitive_callback)(GtkWidget *widget), guint key_val,
                            GdkModifierType mods);

// We don't go further than 3 levels of menus. This is not a Dassault Systems software.

void add_menu_separator(GtkWidget *menu);

void add_sub_menu_separator(GtkWidget *parent);

void *get_custom_data(GtkWidget *widget);

GtkWidget *get_last_widget(GList **list);

gboolean has_selection();
gboolean has_active_images();
gboolean _is_lighttable();


void append_display(GtkWidget **menus, GList **lists, const dt_menus_t index);
void append_edit(GtkWidget **menus, GList **lists, const dt_menus_t index);
void append_file(GtkWidget **menus, GList **lists, const dt_menus_t index);
void append_help(GtkWidget **menus, GList **lists, const dt_menus_t index);
void append_image(GtkWidget **menus, GList **lists, const dt_menus_t index);
void append_run(GtkWidget **menus, GList **lists, const dt_menus_t index);
void append_select(GtkWidget **menus, GList **lists, const dt_menus_t index);
void append_views(GtkWidget **menus, GList **lists, const dt_menus_t index);
