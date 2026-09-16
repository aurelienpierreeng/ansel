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

#include <glib.h>

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
  /**
   * Place a drawing read from an SVG file. APPENDED, not slotted in beside the other "add"
   * actions: these values are what the toolbar's buttons and the shortcut table carry, so
   * inserting one in the middle renumbers every action after it.
   */
  DT_CANVAS_ACTION_ADD_SVG,
  DT_CANVAS_ACTION_PROPERTIES, ///< show the one selected object's properties, as a double click does
  DT_CANVAS_ACTION_DRAW_LINE,  ///< arm the line tool, or put it away when it is the one armed
  DT_CANVAS_ACTION_DRAW_CURVE, ///< the same for the curve tool
  DT_CANVAS_ACTION_DRAW_RECTANGLE, ///< the same for the rectangle tool
  DT_CANVAS_ACTION_DRAW_POLYGON,   ///< the same for the polygon tool
  DT_CANVAS_ACTION_DRAW_STAR,      ///< the same for the star tool
  DT_CANVAS_ACTION_LAST
} dt_canvas_action_t;

/**
 * What a left press on the plane does while the atelier is in a drawing mode, as opposed to picking,
 * moving and selecting. At most one tool is armed at a time, and it stays armed across the objects it
 * draws until it is put away. Never stored: a document knows nothing of how its objects were drawn, so
 * a tool may be appended or slotted in without a file ever noticing.
 */
typedef enum dt_canvas_tool_t
{
  DT_CANVAS_TOOL_NONE = 0,  ///< presses pick, move and select
  DT_CANVAS_TOOL_CONNECTOR, ///< a click on one frame's anchor, then on another's
  DT_CANVAS_TOOL_LINE,      ///< a straight line with both ends free, dragged or placed with a click
  DT_CANVAS_TOOL_CURVE,     ///< the same, bent into an arc
  DT_CANVAS_TOOL_RECTANGLE, ///< a drawn rectangle, its box dragged or placed with a click
  DT_CANVAS_TOOL_POLYGON,   ///< a regular polygon, always regular: the drag gives one side
  DT_CANVAS_TOOL_STAR,      ///< the same, with notches between its points
  DT_CANVAS_TOOL_COUNT
} dt_canvas_tool_t;

/**
 * @brief The tool an action arms, or DT_CANVAS_TOOL_NONE for an action that is not a tool's.
 */
dt_canvas_tool_t dt_canvas_tool_for_action(dt_canvas_action_t action);

/**
 * @brief The tool left armed when `asked` is asked for while `armed` is.
 * @details A tool's action is a toggle: asked again, it puts its tool away; asked while another is
 * armed, it takes that one's place, since two tools cannot both answer the same press. Anything that
 * is not a tool asks for nothing and leaves nothing armed.
 */
dt_canvas_tool_t dt_canvas_tool_toggled(dt_canvas_tool_t armed, dt_canvas_tool_t asked);

/** @brief Whether the tool draws a line with both ends free, as the line and the curve do. */
gboolean dt_canvas_tool_draws_line(dt_canvas_tool_t tool);

/** @brief Whether the tool draws a shape by its box, as the rectangle, the polygon and the star do. */
gboolean dt_canvas_tool_draws_shape(dt_canvas_tool_t tool);

/**
 * @brief Whether the tool draws a shape that is REGULAR: one whose height follows its width.
 * @details The polygon and the star. Their drag gives whichever side the pointer went further along,
 * measured in the shape's own proportions, and the outline's ratio gives the other -- so Ctrl has
 * nothing left to constrain: a regular shape is already square in the only sense it can be.
 */
gboolean dt_canvas_tool_draws_regular(dt_canvas_tool_t tool);

/**
 * @brief Whether the tool draws a new object on the plane, by a drag or by a click.
 * @details Every tool but the connector, which joins two frames that are there already and so
 * takes two clicks on their anchors rather than one gesture of its own. It is what the crosshair,
 * the start marker and the "a press draws instead of picking" rule all ask.
 */
gboolean dt_canvas_tool_draws(dt_canvas_tool_t tool);

/**
 * @brief The name an action's shortcut is registered under, untranslated, or NULL for an action no
 * shortcut is offered for.
 * @details These names ARE the accel map's paths, which is where the user's own bindings are saved:
 * renaming one orphans the key a user gave it, so they are fixed once published. The atelier's shortcut
 * table, its menus and its tooltips all ask here, so a name cannot be spelled two ways.
 */
const char *dt_canvas_action_accel_name(dt_canvas_action_t action);

/** @brief The scope the atelier's shortcuts are grouped under in the accel map, untranslated. */
const char *dt_canvas_action_accel_scope(void);

/**
 * The colours the toolbar sets for the whole canvas, as it names them to the view's colour edits.
 * The guides' colours, the plane's background and the defaults every frame inherits.
 */
typedef enum dt_canvas_color_target_t
{
  DT_CANVAS_COLOR_GRID = 0,
  DT_CANVAS_COLOR_TRIM,
  DT_CANVAS_COLOR_MARGIN,
  DT_CANVAS_COLOR_BLEED,
  DT_CANVAS_COLOR_PADDING,
  DT_CANVAS_COLOR_BACKGROUND,
  DT_CANVAS_COLOR_BORDER,
  DT_CANVAS_COLOR_SHADOW,
  DT_CANVAS_COLOR_LAST
} dt_canvas_color_target_t;

/**
 * The stored list of recently used colours every colour of the atelier shares, the object
 * properties' and the toolbar's: a border matched to a text colour is found where that was picked.
 */
#define DT_CANVAS_COLOR_HISTORY_KEY "plugins/canvas/color_history"


#ifdef __cplusplus
}
#endif

#endif // DT_CANVAS_CANVAS_ACTIONS_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
