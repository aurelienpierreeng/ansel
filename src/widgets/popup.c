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

#include "widgets/popup.h"

#include "system/macros.h"        // IS_NULL_PTR
#include "widgets/widget_settings.h"  // DT_GUI_BOX_SPACING, DT_PIXEL_APPLY_DPI, root window
#include "widgets/widget_style.h"     // dt_gui_add_class

GtkWidget *dt_gui_get_popup_relative_widget(GtkWidget *widget, GdkRectangle *rect)
{
  if(IS_NULL_PTR(widget)) return NULL;

  GtkWidget *relative = widget;

  // Wayland only accepts the top-most enclosing popup as transient parent.
  for(GtkWidget *parent = gtk_widget_get_parent(widget); parent; parent = gtk_widget_get_parent(parent))
    if(GTK_IS_POPOVER(parent)) relative = parent;

  if(rect)
  {
    rect->x = 0;
    rect->y = 0;
    rect->width = MAX(gtk_widget_get_allocated_width(widget), 1);
    rect->height = MAX(gtk_widget_get_allocated_height(widget), 1);

    if(relative != widget
       && !gtk_widget_translate_coordinates(widget, relative, 0, 0, &rect->x, &rect->y))
    {
      rect->x = 0;
      rect->y = 0;
    }
  }

  return relative;
}

void dt_gui_menu_popup(GtkMenu *menu, GtkWidget *button, GdkGravity widget_anchor, GdkGravity menu_anchor)
{
  gtk_widget_show_all(GTK_WIDGET(menu));

  GdkEvent *event = gtk_get_current_event();
  if(button)
  {
    GdkRectangle rect = { 0 };
    GtkWidget *relative = dt_gui_get_popup_relative_widget(button, &rect);

    if(relative && relative != button && gtk_widget_get_window(relative))
      gtk_menu_popup_at_rect(menu, gtk_widget_get_window(relative), &rect, widget_anchor, menu_anchor, event);
    else
      gtk_menu_popup_at_widget(menu, button, widget_anchor, menu_anchor, event);
  }
  else
  {
    if(IS_NULL_PTR(event))
    {
      event = gdk_event_new(GDK_BUTTON_PRESS);
      event->button.device = gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default()));
      event->button.window = gtk_widget_get_window(dt_widget_root_window());
      g_object_ref(event->button.window);
    }

    gtk_menu_popup_at_pointer(menu, event);
  }
  gdk_event_free(event);
}

static void _popover_set_relative_to_topmost_parent(GtkPopover *popover, GtkWidget *button)
{
  GdkRectangle rect = { 0 };
  GtkWidget *relative = dt_gui_get_popup_relative_widget(button, &rect);
  gtk_popover_set_relative_to(popover, relative ? relative : button);
  gtk_popover_set_pointing_to(popover, &rect);
}

GtkBox * attach_popover(GtkWidget *widget, const char *icon, GtkWidget *content)
{
  // Create the wrapping box and add the original widget to it
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
  gtk_box_pack_start(GTK_BOX(box), widget, FALSE, FALSE, 0);

  // Create the info icon button that will trigger the popover
  GtkWidget *button = gtk_menu_button_new();
  dt_gui_add_class(button, "popover-button");
  GtkWidget *image = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON);
  gtk_button_set_image(GTK_BUTTON(button), image);
  gtk_widget_set_hexpand(button, FALSE);
  gtk_widget_set_vexpand(button, FALSE);
  gtk_widget_set_size_request(button, DT_PIXEL_APPLY_DPI(16), DT_PIXEL_APPLY_DPI(16));
  gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);

  // Create the content of the popover
  GtkWidget *popover_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);
  gtk_box_pack_start(GTK_BOX(popover_box), content, FALSE, FALSE, 0);

  // Wrap the content into a popover and attach it to the button
  GtkWidget *popover = gtk_popover_new(button);
  gtk_container_add(GTK_CONTAINER(popover), popover_box);
  gtk_popover_set_modal(GTK_POPOVER(popover), FALSE);
  g_signal_connect(G_OBJECT(popover), "show", G_CALLBACK(_popover_set_relative_to_topmost_parent), button);
  gtk_menu_button_set_popover(GTK_MENU_BUTTON(button), popover);
  gtk_widget_show_all(popover_box);

  return GTK_BOX(box);
}

GtkBox * attach_help_popover(GtkWidget *widget, const char *label)
{
  // Create the content of the popover
  GtkWidget *popover_label = gtk_label_new(label);
  gtk_label_set_line_wrap(GTK_LABEL(popover_label), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(popover_label), 60);
  return attach_popover(widget, "help-about", popover_label);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
