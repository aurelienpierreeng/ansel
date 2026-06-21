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

struct dt_image_t;

/** Initialize sentry.io crash reporting.
 *
 * On the very first launch (no consent decision recorded yet) and when a GUI is
 * available, this shows a modal consent dialog explaining what is collected and
 * why, and stores the user's choice. Sentry is only initialized when the user
 * has opted in (conf key "sentry/enabled"). It enriches reports with OS and
 * hardware context and enables crash-free session tracking.
 *
 * Safe to call when built without sentry support (USE_SENTRY=OFF): it is a no-op.
 *
 * @param have_gui whether a GUI is up and a consent dialog may be shown.
 */
void dt_sentry_init(const gboolean have_gui);

/** Flush and shut down sentry. Marks the current session as a clean (crash-free)
 * exit and increments the local clean-session counter. No-op if sentry was never
 * initialized. */
void dt_sentry_shutdown(void);

/** Whether sentry's crash handler has already captured a gdb backtrace for the
 * current crash. The local signal handler uses this to avoid running gdb twice.
 * Returns FALSE when built without sentry support. */
gboolean dt_sentry_backtrace_captured(void);

/** Record that a module was used during this session, e.g. a view was entered,
 * an iop module was enabled, or a lib panel was opened. Per-module counts are
 * attached to crash reports as the "module_usage" context, so a crash shows
 * which modules were exercised beforehand.
 *
 * @param category short kind, e.g. "view", "iop", "lib".
 * @param name     module identifier (view module_name, iop op, lib plugin_name).
 *
 * Must be called from the GUI thread. No-op without sentry, or before init /
 * when the user has not opted in. */
void dt_sentry_record_module_usage(const char *category, const char *name);

/** Record the image currently being processed, so crash reports show what was
 * on the pipeline at the time. Only the file extension and image type flags are
 * recorded (no file name or path). Attached as the "processed_image" context.
 *
 * Cheap to call on every pipeline run: it is a no-op unless the image or
 * pipeline actually changed. Safe to call from any pipeline thread.
 *
 * @param img      the image on the pipeline (its extension + type flags are read).
 * @param pipeline short label of the pipeline, e.g. "darkroom"/"export".
 *
 * No-op without sentry, or before init / when the user has not opted in. */
void dt_sentry_set_processed_image(const struct dt_image_t *img, const char *pipeline);

#ifdef __cplusplus
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
