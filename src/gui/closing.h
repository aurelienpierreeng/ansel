/*
    This file is part of Ansel,
    Copyright (C) 2026 Guillaume Stutin.

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

#ifndef DT_GUI_CLOSING_H
#define DT_GUI_CLOSING_H

/** Quit, as the user asked from the window, the menu or the dock.
 *
 * While background tasks are running or queued -- those the background jobs panel shows, an
 * export, a preload -- a modal window first names them and asks whether to quit all the same or
 * go back. Nothing is cancelled either way: a quit finishes the running jobs and drops the queued
 * ones. Without background tasks, the quit starts at once. GUI thread only.
 */
void dt_gui_closing_quit(void);

/** Keep the GUI thread in its main loop until every control worker has returned.
 *
 * Quitting hides the main window at once, but the jobs running at that moment -- an export, the
 * thumbnails being rendered, a pipeline -- are finished, not abandoned. When that takes more
 * than a moment, a small window says so, names the jobs that have a name, and goes away with
 * the last of them.
 *
 * This is the handler dt_control_shutdown() waits in: see dt_control_set_shutdown_wait_handler().
 * GUI thread only. Returns at once when nothing is running.
 */
void dt_gui_closing_wait(void);

#endif // DT_GUI_CLOSING_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
