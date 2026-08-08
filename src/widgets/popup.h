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

#ifndef DT_WIDGETS_POPUP_H
#define DT_WIDGETS_POPUP_H

/* Menus and popovers, and the Wayland rule that governs where they may be anchored.
 *
 * Wayland compositors only accept the top-most enclosing popup as a popup's parent. Anchoring
 * to the widget the user actually clicked -- which is what every naive call does -- produces a
 * popup placed at the wrong end of the screen, or none at all. Every popup in the application
 * goes through here so that rule is applied once. */

#include <gtk/gtk.h>

G_BEGIN_DECLS

/** Get the top-most window attached to a widget. Dynamic: accounts for destroyed widgets. */
static inline GtkWindow *dt_gtk_get_window(GtkWidget *widget)
{
  if(!widget) return NULL;
  GtkWidget *toplevel = gtk_widget_get_toplevel(widget);
  if(toplevel && gtk_widget_is_toplevel(toplevel)) return GTK_WINDOW(toplevel);
  return NULL;
}

/**
 * @brief Resolve the widget to use as parent for a nested popup on Wayland.
 *
 * Walks the parent chain to the top-most enclosing popup while leaving the caller in charge of
 * the popup logic. When @p rect is not NULL, it receives the position and size of @p widget in
 * the returned anchor's coordinate system.
 *
 * @param widget the widget the popup should visually point to.
 * @param rect optional output rectangle receiving the geometry of @p widget.
 * @return the widget to use as popup parent, or NULL when @p widget is NULL.
 */
GtkWidget *dt_gui_get_popup_relative_widget(GtkWidget *widget, GdkRectangle *rect);

/** Pop a menu up at @p button (or at the pointer when @p button is NULL), anchored correctly
 * even when the button itself sits inside another popover. */
void dt_gui_menu_popup(GtkMenu *menu, GtkWidget *button, GdkGravity widget_anchor, GdkGravity menu_anchor);

/**
 * Add an arbitrary button next to the widget that opens a popover with arbitrary content.
 * @param widget the original widget next to which the popover button will be added. DON'T add
 *        it to a container.
 * @param icon the Freedesktop icon name to put in the button
 * @param content the widget that will fit inside the popover
 * @return the GtkBox containing both the original widget and its popover button. That's what
 *         you will need to add to your container.
 */
GtkBox *attach_popover(GtkWidget *widget, const char *icon, GtkWidget *content);

/**
 * Add a help button triggering a popover label next to an arbitrary widget, to document its
 * action. This is a better take at help tooltips that most people don't see unless they know
 * about them. Tooltip window positioning is also wonky (can easily overflow the viewport),
 * line breaks are added manually (ugly hack), and they appear and disappear on hover (not
 * available on touch screens), so it's flimsy UI.
 * @param widget the original widget to document. DON'T add it to a container.
 * @param label the in-app "docstring" for the widget
 * @return the GtkBox containing both the original widget and its popover button.
 */
GtkBox *attach_help_popover(GtkWidget *widget, const char *label);

G_END_DECLS

#endif // DT_WIDGETS_POPUP_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
