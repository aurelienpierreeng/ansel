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

#pragma once

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Show, once, the combined privacy consent dialog.
 *
 * On first launch (when no decision has been recorded yet) this presents a single
 * dialog with one checkbox per opt-in data flow that was built in:
 *   - crash reporting (Sentry), writing `sentry/enabled`,
 *   - usage analytics (PostHog), writing `telemetry/enabled`.
 * It links to the user-facing "Data privacy" documentation page so users can read
 * exactly what is collected before deciding, then records the answer so it is
 * never asked again (sentinel `privacy/consent_asked`).
 *
 * Must be called BEFORE dt_sentry_init()/dt_telemetry_init() so the per-feature
 * enabled flags are set when those modules read them. No dialog is shown when a
 * decision already exists, when there is no GUI, or when neither crash reporting
 * nor analytics was compiled in.
 *
 * @param have_gui whether a GUI is up and a modal dialog may be shown.
 */
void dt_privacy_ask_consent(const gboolean have_gui);

#ifdef __cplusplus
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
