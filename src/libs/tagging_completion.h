/*
    This file is part of Ansel,
    Copyright (C) 2026 Paolo SANTUCCI.

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

#include <gtk/gtk.h>

/** Replace a completion model's contents with the current tag vocabulary. */
void dt_tagging_completion_refresh(GtkListStore *store);

/** Replace the active comma-separated token with the selected completion row. */
gboolean dt_tagging_completion_match_selected(GtkEntryCompletion *completion, GtkTreeModel *model,
                                              GtkTreeIter *iter, gpointer user_data);

/** Cancel pending completion work and clear an entry while preserving its completion. */
void dt_tagging_entry_clear(GtkEntry *entry);
