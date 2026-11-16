/*
 *    This file is part of Ansel,
 *    Copyright (C) 2026 Champions007.
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

#ifndef DT_GUI_WAYLAND_COLOR_H
#define DT_GUI_WAYLAND_COLOR_H

#include <gtk/gtk.h>

/* Colour management with a Wayland compositor (the wp_color_management_v1 protocol).
 *
 * A compositor that manages colour converts every window from the colour description the
 * window declares to the one of the monitor, and reads a window that declares nothing as
 * sRGB. It also says which description it would rather receive for a given window -- the
 * monitor's own, as a rule -- so that a client able to render in it keeps the monitor's
 * whole gamut and spares the compositor a conversion.
 *
 * This module does both halves, and they only make sense together: it turns the description
 * the compositor prefers for the main window into an ICC profile, offers it to
 * colorprofiles/ as the display profile, and -- only while that offer is the profile the
 * pixels are really encoded for -- declares every window of the application in a matching
 * description. Whether the offer is taken is not decided here: see
 * dt_colorspaces_offer_compositor_display_profile().
 *
 * Compiled only when the build found the protocol (HAVE_WAYLAND_COLOR), and inert at run time
 * on anything but a Wayland session whose compositor has it.
 *
 * See doc/wayland-color.md.
 */

/** Start following the colour description the compositor prefers for `main_window`.
 *
 *  Returns at once: everything that follows is driven by compositor events on the GUI thread.
 *  Does nothing on a display that is not Wayland. */
void dt_gui_wayland_color_init(GtkWidget *main_window);

/** Release every protocol object. To be called before the main window is hidden: GDK
 *  destroys a window's surface when it hides it, and a compositor event arriving after that
 *  must find nothing left that would answer it on a dead surface. */
void dt_gui_wayland_color_cleanup(void);

#endif // DT_GUI_WAYLAND_COLOR_H
