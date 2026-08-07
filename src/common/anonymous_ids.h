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

#ifndef DT_COMMON_ANONYMOUS_IDS_H
#define DT_COMMON_ANONYMOUS_IDS_H

/* Anonymous correlation ids shared by crash reporting (common/sentry.c) and usage
 * analytics (common/telemetry.c). Generated/persisted by the orchestrator
 * (darktable.c) so both subsystems agree on them. */

#ifdef __cplusplus
extern "C" {
#endif

/** Stable, anonymous identifier for the current process/run (a random UUID
 * generated once). Sent to both crash reporting (Sentry) and usage analytics
 * (PostHog) so the same session can be correlated across the two without being
 * double-counted. Not tied to the user or machine. */
const char *dt_session_id(void);

/** Stable, anonymous per-installation identifier (a random UUID persisted in
 * conf). Used as the Sentry user id and the PostHog distinct_id so the same
 * installation/user can be de-duplicated across both systems. Not tied to the
 * machine or any account. */
const char *dt_install_id(void);

#ifdef __cplusplus
}
#endif

#endif // DT_COMMON_ANONYMOUS_IDS_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
