/*
    This file is part of the Ansel project.
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

#ifndef DT_COMMON_UPDATES_H
#define DT_COMMON_UPDATES_H

#include <glib.h>

/** @file updates.h
 *
 * @brief The nightly update check.
 *
 * Once a day, a nightly build fetches https://ansel.photos/nightly.json -- the one
 * file the nightly CI writes after each build, naming the newest build of every
 * format -- and compares the commit recorded for its own format with the commit it
 * was built from. A difference is announced with a toast and made actionable through
 * Help > Update to the latest nightly build, which opens the download for the running
 * format (AppImage, Flatpak, dmg per architecture, exe).
 *
 * The check exists only in the nightly channel (DT_BUILD_CHANNEL), only with a GUI,
 * and only when updates/enabled is set -- a third line in the first-launch privacy
 * dialog, and a toggle in Preferences > Storage > Privacy. It sends nothing but an
 * HTTP GET carrying the application's User-Agent; there is no identity, no telemetry
 * and no dependency on the analytics or crash-reporting modules.
 */

/** Start the check if it is due. Returns immediately; the fetch runs on its own thread. */
void dt_updates_init(const gboolean have_gui);

/** Stop the worker. Bounded by the request timeout. */
void dt_updates_shutdown(void);

/** The download URL of the newer build found by the last check, or NULL when none was
 * found (or the check did not run). The string is owned by the module. */
const char *dt_updates_get_download_url(void);

/** The version string of the newer build found by the last check, or NULL. */
const char *dt_updates_get_available_version(void);

/** Where the running binary came from, as a key of the manifest's "formats" object
 * ("appimage", "flatpak", "dmg-arm64", "dmg-i386", "exe"), or NULL when it cannot tell.
 * Exposed for the consent dialog and for diagnostics. */
const char *dt_updates_runtime_format(void);

#endif // DT_COMMON_UPDATES_H
