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
#include <json-glib/json-glib.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dt_image_t;

/** Initialize opt-in usage analytics (PostHog, EU region).
 *
 * This is SEPARATE from crash reporting (Sentry): it has its own consent dialog
 * and its own preference (`telemetry/enabled`). On first launch with a GUI it
 * asks the user, once, whether to share anonymous usage statistics. Analytics is
 * started only if the user opted in and a PostHog API key was configured at build
 * time. It sends anonymous, aggregate data (no images, file names or personal
 * data) keyed by a random per-installation id.
 *
 * Safe to call without telemetry support (USE_TELEMETRY=OFF): no-op.
 *
 * @param have_gui whether a GUI is up and a consent dialog may be shown.
 */
void dt_telemetry_init(const gboolean have_gui);

/** Flush pending analytics events and stop the background worker. No-op if
 * analytics was never started. */
void dt_telemetry_shutdown(void);

/** Queue one analytics event for asynchronous delivery to PostHog.
 *
 * @param event       event name, e.g. "session_start".
 * @param properties  a JsonObject of event properties, or NULL. Ownership is
 *                    transferred to this function (it is unref'd internally).
 *
 * Non-blocking and thread-safe. No-op (and still consumes @p properties) when
 * analytics is disabled or not opted in. */
void dt_telemetry_capture(const char *event, JsonObject *properties);

/** Count one usage of a module/view/panel for the current session.
 *
 * Accumulated in-memory (counts only, no order, no timing) and sent aggregated
 * in the "session_end" event at shutdown. Thread-safe; no-op when analytics is
 * disabled.
 *
 * @param category  one of "view", "lib", "iop".
 * @param name      the module's stable operation/plugin name. */
void dt_telemetry_record_module_usage(const char *category, const char *name);

/** Record the kind of an image processed during this session (extension and
 * type flags only, never the file name or path).
 *
 * De-duplicated per image+pipeline like the crash context, accumulated in-memory
 * and sent aggregated in the "session_end" event. Thread-safe; no-op when
 * analytics is disabled.
 *
 * @param img       the image being processed.
 * @param pipeline  pipeline label, e.g. "darkroom" or "export". */
void dt_telemetry_record_file_type(const struct dt_image_t *img, const char *pipeline);

#ifdef __cplusplus
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
