/*
    This file is part of Ansel.
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

#ifndef DT_GUI_ACTIONS_DOC_SCREENSHOT_H
#define DT_GUI_ACTIONS_DOC_SCREENSHOT_H

#include <glib.h>

/** @file doc_screenshot.h
 *
 * Documentation screenshot mode: a capture panel that renders individual GUI widgets
 * (panels, tool modules, darkroom modules) to image files, so the manual's illustrations
 * can be re-generated from the running application instead of being cropped by hand out
 * of full-screen captures.
 *
 * Where the destination is a documentation tree, a plain-text map (DOC_SCREENSHOT_MAP_FILE
 * at its root) binds a widget to the illustration it stands for, and the language code the
 * GUI currently runs in is inserted before the extension -- exposure.fr.jpg next to
 * exposure.en.jpg. Running the application once per language and capturing the same
 * selection therefore refreshes the whole illustrated documentation, language by language.
 *
 * The whole feature is inert unless `--doc` was passed on the command line:
 * this module owns that flag (the orchestrator only announces it), and gui/actions/run.c
 * asks for it before adding the entry to the "Run" menu. Nothing else in the application
 * knows the mode exists.
 */

/** Announce that `--doc` was given. Called once by the argument parser, before
 * the GUI is built -- the menu is assembled later and reads the flag back. */
void dt_gui_doc_screenshot_enable(void);

/** TRUE when `--doc` was given, i.e. when the capture panel may be offered. */
gboolean dt_gui_doc_screenshot_enabled(void);

/** Root of the documentation tree to write into, as given after `--doc`. It pre-fills the
 * panel's destination folder and is where the widget-to-page map is looked for, so a
 * documentation pass is one command away from being ready to capture. */
void dt_gui_doc_screenshot_set_directory(const char *path);

/** Show (or raise) the capture panel: a tree of screenshotable widgets going from the whole
 * window down to a single slider, with a check box on every row, a destination folder and a
 * capture button. */
void dt_gui_doc_screenshot_window_show(void);

#endif // DT_GUI_ACTIONS_DOC_SCREENSHOT_H
