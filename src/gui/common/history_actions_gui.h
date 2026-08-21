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

#ifndef DT_GUI_COMMON_HISTORY_ACTIONS_GUI_H
#define DT_GUI_COMMON_HISTORY_ACTIONS_GUI_H

#include <glib.h>
#include <gtk/gtk.h>
#include <inttypes.h>

G_BEGIN_DECLS

/* The history actions that ask the user something, split out of common/history_actions.c.
 * The pure-backend half stays there and knows nothing about any of this. */

/** Copy history from @p imgid, then run the module-picker dialog to narrow it down.
 *  Returns FALSE if the user cancelled. The chosen modules land in the copy/paste proxy,
 *  where dt_history_paste_parts_on_{image,list}() read them. */
gboolean dt_history_copy_parts(int32_t imgid);

/** Run the module-picker dialog over what was copied, ahead of a partial paste.
 *  Returns FALSE if the user cancelled or nothing was copied. */
gboolean dt_history_paste_parts_prepare(void);

/** Confirm (if `ask_before_discard` is set), then delete the history of the active images.
 *  Shared verbatim by the Edit menu's "Delete history" action and the darkroom history
 *  module's reset button, which is why it is a GtkAccelGroup callback rather than a plain
 *  function. */
gboolean delete_history_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods,
                                 gpointer user_data);

G_END_DECLS

#endif // DT_GUI_COMMON_HISTORY_ACTIONS_GUI_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
