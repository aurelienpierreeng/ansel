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

#ifndef DT_SYSTEM_DISPLAY_PROFILE_H
#define DT_SYSTEM_DISPLAY_PROFILE_H

#include <gtk/gtk.h>

/* Which monitor is this window on, and what ICC profile has the OS attached to it?
 *
 * This is hardware interrogation -- the same category as querying RAM or CPU features --
 * and it lived in common/colorspaces.c only because that is who consumes the answer.
 *
 * It is unavoidably windowing-system code: the monitor is identified BY the window shown
 * on it, and each platform answers differently (X11 _ICC_PROFILE atom, the Windows colour
 * API, the macOS ColorSync API). It therefore depends on GDK/GTK -- an external library,
 * not on src/gui/ -- so nothing here inverts the layer order.
 *
 * The caller supplies the widget. This module never asks the GUI which window to look at;
 * that decision belongs to whoever owns the window.
 */

/** Read the ICC profile the OS associates with the monitor showing `widget`.
 *
 *  On success sets `*buffer` (caller frees with dt_free) and `*buffer_size`, and sets
 *  `*source` to a short human-readable origin for logging (caller frees with dt_free).
 *  On platforms with no implementation, leaves everything untouched.
 *
 *  Does NOT cover colord: that path is asynchronous and calls back into the colour-profile
 *  subsystem's own state, so it stays with its consumer in common/colorspaces.c. */
void dt_display_profile_read(GtkWidget *widget, guint8 **buffer, gint *buffer_size, gchar **source);

#endif // DT_SYSTEM_DISPLAY_PROFILE_H
