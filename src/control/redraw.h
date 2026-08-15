/*
    This file is part of Ansel.
    Copyright (C) 2026 Aurélien Pierre.

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

/** @file control/redraw.h
 *
 * @brief Asking the interface to repaint itself.
 *
 * @details Five one-line signal raisers, split out of control/control.h for the same reason
 * control/user_message.h was: a caller that only wants the screen refreshed should not compile
 * libs/lib.h and views/view.h to say so. Nine files use these and nothing else from control.h.
 *
 * Each is a DT_DEBUG_CONTROL_SIGNAL_RAISE and nothing more, so none of them needs a toolkit
 * type and this header includes nothing. The sixth raiser, dt_control_queue_redraw_widget(),
 * takes a GtkWidget* and therefore stays in control.h -- putting it here would drag GTK into
 * every includer and defeat the point.
 *
 * They are safe to call from any thread: the raise is what crosses back to the GUI thread.
 */

#ifndef DT_CONTROL_REDRAW_H
#define DT_CONTROL_REDRAW_H

/* C linkage: these are defined in a C translation unit and reachable from C++ ones. The header
 * this came from wraps its declarations the same way, so without this a C++ caller emits a
 * mangled reference. Linux does not catch it -- a plugin .so may keep undefined symbols and
 * fail at dlopen() instead -- but macOS and Windows fail the link. See control/user_message.h. */
#ifdef __cplusplus
extern "C" {
#endif

/** @brief Request a redraw of the whole workspace. */
void dt_control_queue_redraw();

/** @brief Request a redraw of the centre view. */
void dt_control_queue_redraw_center();

/** @brief Request a redraw of the navigation module's widget. */
void dt_control_navigation_redraw();

/** @brief Request a redraw of the log message label. */
void dt_control_log_redraw();

/** @brief Request a redraw of the toast message label. */
void dt_control_toast_redraw();

#ifdef __cplusplus
}
#endif

#endif // DT_CONTROL_REDRAW_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
