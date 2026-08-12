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

/** @file develop/dev_history_gui.h
 *
 * @brief The GTK half of the history engine, following the blend.c/blend_gui.c pattern.
 *
 * @details `dev_history.c` is the history ENGINE -- item lifecycle, commit, replay,
 * DB read/write, all under `history_mutex` discipline. What lives here is the part that
 * touches widgets: the handlers the engine calls when presentation state rides along with
 * an operation (undo records restoring the mask-edit view), installed once at GUI startup.
 * With nothing installed -- ansel-cli, a unit test -- the engine skips the calls, which is
 * what headless means.
 */

#ifndef DT_DEVELOP_DEV_HISTORY_GUI_H
#define DT_DEVELOP_DEV_HISTORY_GUI_H

#include <glib.h>

G_BEGIN_DECLS

/** @brief Install the history engine's GUI handlers. Called once from dt_init() beside
 *  the other handler installs -- safe under ansel-cli too, because every handler fires
 *  only for a dev with a focused GUI module, which a headless dev never has. */
void dt_dev_history_gui_init(void);

G_END_DECLS

#endif // DT_DEVELOP_DEV_HISTORY_GUI_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
