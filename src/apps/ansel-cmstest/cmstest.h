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

#ifndef DT_APPS_ANSEL_CMSTEST_CMSTEST_H
#define DT_APPS_ANSEL_CMSTEST_CMSTEST_H

#include <glib.h>
#include <stddef.h>

/* ansel-cmstest says what the system knows about each monitor's colour, for the windowing
 * system ansel would run on: one file per windowing system (x11.c, wayland.c), and main.c for
 * what they share and for choosing between them. Each of the two is compiled only where its
 * libraries were found, and each prints its own report and its own conclusion. */

/** The description tag of an ICC profile.
 *
 *  @return a string to free with dt_free(), or NULL when there is no data or lcms2 cannot read
 *  a profile from it. */
char *dt_cmstest_profile_description(const unsigned char *data, size_t size);

/** The file of the profile colord holds for a monitor.
 *
 *  @param output_name the connector the monitor is plugged in, "eDP-1" or "DP-2": the name
 *  XRandR gives an output, and the one a Wayland compositor gives it too. It is what colord's
 *  devices are filed under, and what ansel itself asks colord with.
 *  @return a path to free with dt_free(); NULL when colord has no profile for that monitor,
 *  cannot be reached, or was not compiled in. */
char *dt_cmstest_colord_filename(const char *output_name);

/** Report on the X11 display named by $DISPLAY. @return EXIT_SUCCESS or EXIT_FAILURE. */
int dt_cmstest_x11(void);

/** Is there a Wayland compositor to connect to, as GDK would find it? */
gboolean dt_cmstest_wayland_available(void);

/** Report on the Wayland compositor. @return EXIT_SUCCESS or EXIT_FAILURE. */
int dt_cmstest_wayland(void);

#endif // DT_APPS_ANSEL_CMSTEST_CMSTEST_H
