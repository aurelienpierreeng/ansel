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


#ifndef DT_GUI_COMMON_FOLDER_SURVEY_GUI_H
#define DT_GUI_COMMON_FOLDER_SURVEY_GUI_H

#include <glib.h>

G_BEGIN_DECLS

/** Register the handler common/folder_survey.c calls to ask about pending imports.
 *  Called from dt_gui_gtk_init(); without it the backend simply never asks. */
void dt_folder_survey_gui_register_handlers();

/**
 * @brief Propose to resume an interrupted studio session at application start.
 *
 * When the previous session quit while monitoring, ask the user whether to resume; on
 * acceptance, switch to the Studio Capture view and start monitoring, which in turn asks
 * whether files that appeared meanwhile should be imported now. Call after the GUI and
 * views are ready.
 *
 * @return gboolean always G_SOURCE_REMOVE, so it can be scheduled with g_idle_add().
 */
gboolean dt_folder_survey_propose_resume();

G_END_DECLS

#endif // DT_GUI_COMMON_FOLDER_SURVEY_GUI_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
