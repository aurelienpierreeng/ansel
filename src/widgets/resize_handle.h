/*
    This file is part of Ansel,
    Copyright (C) 2026 Aurélien PIERRE.

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

#ifndef DT_WIDGETS_RESIZE_HANDLE_H
#define DT_WIDGETS_RESIZE_HANDLE_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* The drag grip that resizes a panel, the histogram scope, the filmstrip.
 *
 * It lived in bauhaus.c, which is the slider/combobox toolkit and has nothing to do with
 * dragging a panel edge. Nothing here touches a dt_bauhaus_widget_t; it is a GtkEventBox
 * that converts a pointer drag into a size, and hands the size to its owner.
 */

/** Current size of the resized target, along the axis the handle drags. */
typedef int (*dtgtk_resize_handle_get_size_f)(gpointer user_data);

/** Apply @p requested_size to the target and return the size actually adopted (the caller
 *  clamps). @p finished is FALSE for each motion sample and TRUE on button release. */
typedef int (*dtgtk_resize_handle_resize_f)(int requested_size, gboolean finished, gpointer user_data);

/**
 * @brief Create a themed handle widget driving one-dimensional resize gestures.
 *
 * @details The handle owns the GTK event bookkeeping: hover state, cursor, grab lifetime,
 * drawing and drag delta computation. The caller owns the resized target and keeps that
 * ownership visible through @p get_size and @p resize. During pointer motion @p resize receives
 * `finished == FALSE`; on button release it receives `finished == TRUE` so callers can persist
 * the final size without writing settings at every motion sample.
 *
 * @param invert When FALSE the target grows as the pointer moves in the positive axis direction
 * (down for vertical, right for horizontal) — the natural case for a handle sitting below/at the
 * right of its target. Set TRUE when the target grows in the opposite direction, e.g. a right
 * panel that grows as it is dragged left, or a bottom panel that grows as it is dragged up.
 *
 * The grip is meant to be added as an overlay child on the resized widget. It pins itself to the
 * correct edge (from @p orientation and @p invert) and tags itself with an edge CSS class
 * (.resize-handle-{top,bottom,left,right}); its thickness and centering live in the stylesheet.
 */
GtkWidget *dtgtk_resize_handle_new(GtkOrientation orientation, gboolean invert, const char *tooltip,
                                   dtgtk_resize_handle_get_size_f get_size,
                                   dtgtk_resize_handle_resize_f resize, gpointer user_data);

G_END_DECLS

#endif // DT_WIDGETS_RESIZE_HANDLE_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
