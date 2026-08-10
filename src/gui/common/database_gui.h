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

#ifndef DT_GUI_COMMON_DATABASE_GUI_H
#define DT_GUI_COMMON_DATABASE_GUI_H

#include <glib.h>

/* The GUI half of database/database.c. */

/** Report why the database would not open, and offer to quit, retry, or delete the lock
 *  files. Consumes the database's pending error.
 *
 *  Returns TRUE if the failure is fatal, FALSE if the caller should try opening again --
 *  the caller's init loop in darktable.c closes and re-runs dt_database_open() on FALSE.
 *
 *  Postponed until after dbus has been tried, so another running instance gets the chance
 *  to answer first. */
gboolean dt_database_show_error(void);

/** Register the handler database/database.c puts its prompts through.
 *  Must be called BEFORE dt_database_open(), so darktable.c does it -- dt_gui_gtk_init()
 *  runs too late. See dt_database_set_prompt_handler(). */
void dt_database_gui_register_handlers(void);

#endif // DT_GUI_COMMON_DATABASE_GUI_H
