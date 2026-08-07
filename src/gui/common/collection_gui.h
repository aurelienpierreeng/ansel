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

#ifndef DT_GUI_COMMON_COLLECTION_GUI_H
#define DT_GUI_COMMON_COLLECTION_GUI_H

/* The GUI half of common/collection.c: the recently-used-collections list, which exists to
 * fill a menu and is read by nothing in the backend. */

/** Register the recents bookkeeping with common/collection.c. */
void dt_collection_gui_register_handlers(void);

#endif // DT_GUI_COMMON_COLLECTION_GUI_H
