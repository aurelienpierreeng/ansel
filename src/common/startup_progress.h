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

#ifndef DT_COMMON_STARTUP_PROGRESS_H
#define DT_COMMON_STARTUP_PROGRESS_H

/* "Still working, here is what on" -- reported by whatever is slow during startup, shown by
 * whatever can show it.
 *
 * common/opencl.c can spend a long time building kernels on first run, and was calling
 * dt_gui_splash_init()/dt_gui_splash_updatef() directly to say so: layer 1 driving a layer-4
 * splash screen, and carrying the "have I opened the splash yet?" state to do it.
 *
 * The reporter now only reports. Whether that becomes a splash screen, a log line, or
 * nothing at all belongs to whoever registers the handler -- including the decision to open
 * the window on first message, which is display state and lives with the display.
 *
 * With no handler registered (any headless run) reporting is a no-op, so callers need no
 * "is there a GUI?" test of their own.
 */
typedef void (*dt_startup_progress_handler_t)(const char *message);

/** Install the handler that displays startup progress. NULL removes it. */
void dt_startup_progress_set_handler(dt_startup_progress_handler_t handler);

/** Report progress. printf-style; safe to call with no handler registered. */
void dt_startup_progress_report(const char *format, ...) __attribute__((format(printf, 1, 2)));

#endif // DT_COMMON_STARTUP_PROGRESS_H
