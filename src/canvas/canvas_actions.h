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

#ifndef DT_CANVAS_CANVAS_ACTIONS_H
#define DT_CANVAS_CANVAS_ACTIONS_H

/**
 * @file canvas_actions.h
 * @brief The canvas-level actions a toolbar, a menu or a shortcut can ask the atelier for.
 *
 * @details The Canvas view owns the document and answers these through the view
 * manager's `proxy.canvas`; the toolbar lib (libs/tools/canvas_toolbar.c) and the
 * shortcuts only name an action. Keeping the vocabulary here, below both, is what lets
 * the two stay strangers.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum dt_canvas_action_t
{
  DT_CANVAS_ACTION_NEW = 0,
  DT_CANVAS_ACTION_OPEN,
  DT_CANVAS_ACTION_SAVE,
  DT_CANVAS_ACTION_SAVE_AS,
  DT_CANVAS_ACTION_EXPORT,
  DT_CANVAS_ACTION_ADD_TEXT,
  DT_CANVAS_ACTION_ADD_NOTES, ///< the .txt notes of the selected images, as linked text frames
  DT_CANVAS_ACTION_ADD_MAP,   ///< a map frame at the centre of the view
  DT_CANVAS_ACTION_ZOOM_FIT,
  DT_CANVAS_ACTION_ZOOM_100,
  DT_CANVAS_ACTION_SELECT_ALL,
  DT_CANVAS_ACTION_DELETE,
  DT_CANVAS_ACTION_SYNC_CHECK,
  DT_CANVAS_ACTION_SYNC_REFRESH_STALE,
  DT_CANVAS_ACTION_SYNC_REFRESH_ALL,
  DT_CANVAS_ACTION_LAYOUT_GRID,
  DT_CANVAS_ACTION_LAYOUT_MASONRY,
  DT_CANVAS_ACTION_LAYOUT_ROW,
  DT_CANVAS_ACTION_LAYOUT_COLUMN,
  DT_CANVAS_ACTION_TOGGLE_GRID,
  DT_CANVAS_ACTION_TOGGLE_SNAP,
  DT_CANVAS_ACTION_UNDO,
  DT_CANVAS_ACTION_REDO,
  DT_CANVAS_ACTION_CONNECT_MODE, ///< toggle the connector-drawing mode
  DT_CANVAS_ACTION_LAST
} dt_canvas_action_t;


#ifdef __cplusplus
}
#endif

#endif // DT_CANVAS_CANVAS_ACTIONS_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
