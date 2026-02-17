/*
    This file is part of the Ansel project.
    Copyright (C) 2023, 2025 Aurélien PIERRE.
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
#pragma once

#include "common/darktable.h"

#include <gtk/gtk.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DT_UI_PANEL_MODULE_SPACING 0
#define DT_UI_PANEL_SIDE_DEFAULT_SIZE 350
#define DT_UI_PANEL_BOTTOM_DEFAULT_SIZE 120

typedef enum dt_ui_panel_t
{
  /* the header panel */
  DT_UI_PANEL_TOP,
  /* left panel */
  DT_UI_PANEL_LEFT,
  /* right panel */
  DT_UI_PANEL_RIGHT,
  /* bottom panel */
  DT_UI_PANEL_BOTTOM,

  DT_UI_PANEL_SIZE
} dt_ui_panel_t;

typedef enum dt_ui_container_t
{
  /* the top container of left panel, the top container
     disables the module expander and does not scroll with other modules
  */
  DT_UI_CONTAINER_PANEL_LEFT_TOP = 0,

  /* the center container of left panel, the center container
     contains the scrollable area that all plugins are placed within and last
     widget is the end marker.
     This container will always expand|fill empty vertical space
  */
  DT_UI_CONTAINER_PANEL_LEFT_CENTER = 1,

  /* the bottom container of left panel, this container works just like
     the top container but will be attached to bottom in the panel, such as
     plugins like background jobs module in lighttable and the plugin selection
     module in darkroom,
  */
  DT_UI_CONTAINER_PANEL_LEFT_BOTTOM = 2,

  DT_UI_CONTAINER_PANEL_RIGHT_TOP = 3,
  DT_UI_CONTAINER_PANEL_RIGHT_CENTER = 4,
  DT_UI_CONTAINER_PANEL_RIGHT_BOTTOM = 5,

  /* menu bar and toolbar */
  DT_UI_CONTAINER_PANEL_TOP_SECOND_ROW = 6,

  /* Count of containers */
  DT_UI_CONTAINER_SIZE,

  // The following are special containers linked to the header bar,
  // they will never be destroyed in loops, so put them after the container "size"
} dt_ui_container_t;

typedef struct dt_ui_t
{
  /* container widgets */
  GtkWidget *containers[DT_UI_CONTAINER_SIZE];

  /* panel widgets */
  GtkWidget *panels[DT_UI_PANEL_SIZE];

  /* The top panel contains the global menu and is not to be deleted/recreated between each view change */
  GtkWidget *top_panel;

  /* center widget */
  GtkWidget *center;
  GtkWidget *center_base;

  /* main widget */
  GtkWidget *main_window;

  /* thumb table */
  dt_thumbtable_t *thumbtable_lighttable;
  dt_thumbtable_t *thumbtable_filmstrip;

  /* log msg and toast labels */
  GtkWidget *log_msg, *toast_msg;

  /* Header/title bar */
  struct dt_header_t *header;
} dt_ui_t;

gchar *panels_get_view_path(char *suffix);
gchar *panels_get_panel_path(dt_ui_panel_t panel, char *suffix);

int dt_ui_panel_get_size(dt_ui_t *ui, const dt_ui_panel_t p);

gboolean dt_ui_panel_ancestor(dt_ui_t *ui, const dt_ui_panel_t p, GtkWidget *w);

// Drawing area used to paint background image.
// Hide it in lighttable mode.
GtkWidget *dt_ui_center(dt_ui_t *ui);
GtkWidget *dt_ui_center_base(dt_ui_t *ui);
GtkWidget *dt_ui_log_msg(dt_ui_t *ui);
GtkWidget *dt_ui_toast_msg(dt_ui_t *ui);
GtkWidget *dt_ui_main_window(dt_ui_t *ui);

void dt_ui_init_main_table(GtkWidget *container, dt_ui_t *ui);
void dt_ui_cleanup_main_table(dt_ui_t *ui);

GtkBox *dt_ui_get_container(dt_ui_t *ui, const dt_ui_container_t c);
void dt_ui_container_add_widget(dt_ui_t *ui, const dt_ui_container_t c, GtkWidget *w);

void dt_ui_restore_panels(dt_ui_t *ui);

void dt_ui_init_titlebar(dt_ui_t *ui);
void dt_ui_cleanup_titlebar(dt_ui_t *ui);
void dt_ui_init_global_menu(dt_ui_t *ui);
void dt_ui_set_window_buttons_visible(dt_ui_t *ui, gboolean visible);

void dt_hinter_set_message(dt_ui_t *ui, const char *message);
void dt_ui_set_image_info_label(dt_ui_t *ui, const char *label);


#ifdef __cplusplus
}
#endif
