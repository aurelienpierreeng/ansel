/*
 *    This file is part of Ansel,
 *    Copyright (C) 2026 Guillaume Stutin.
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

#ifndef DT_WIDGETS_SCROLL_LATCH_H
#define DT_WIDGETS_SCROLL_LATCH_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/**
 * @brief Keep the mouse wheel on @p scrolled_window once it has started scrolling.
 *
 * GTK gives each wheel step to the deepest widget under the pointer. While a scrolled window
 * scrolls, its content moves under a still pointer, and the first child that takes the wheel
 * (a graph, a list, a focused slider) stops the scroll and takes the steps that follow. Once
 * @p scrolled_window has taken a step, every step over its content goes to it, until the wheel
 * rests or the pointer moves.
 *
 * @param scrolled_window a GtkScrolledWindow that already has its child: the latch is held on
 * that child.
 */
void dt_ui_scroll_latch(GtkWidget *scrolled_window);

G_END_DECLS

#endif
