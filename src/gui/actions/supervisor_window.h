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

#pragma once

// Show (or raise) the event supervisor window: a browsable view of the
// in-memory supervisor event log. Toggling "Record" captures live events; rows
// expand to show per-event detail, and every hash is a clickable link that
// jumps to the declaration of the linked object. See develop/supervisor.h.
void dt_gui_supervisor_window_show(void);
