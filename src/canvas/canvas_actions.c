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

#include "canvas/canvas_actions.h"

#include <glib/gi18n.h>

dt_canvas_tool_t dt_canvas_tool_for_action(const dt_canvas_action_t action)
{
  switch(action)
  {
    case DT_CANVAS_ACTION_CONNECT_MODE:
      return DT_CANVAS_TOOL_CONNECTOR;
    case DT_CANVAS_ACTION_DRAW_LINE:
      return DT_CANVAS_TOOL_LINE;
    case DT_CANVAS_ACTION_DRAW_CURVE:
      return DT_CANVAS_TOOL_CURVE;
    case DT_CANVAS_ACTION_DRAW_RECTANGLE:
      return DT_CANVAS_TOOL_RECTANGLE;
    case DT_CANVAS_ACTION_DRAW_POLYGON:
      return DT_CANVAS_TOOL_POLYGON;
    case DT_CANVAS_ACTION_DRAW_STAR:
      return DT_CANVAS_TOOL_STAR;
    default:
      return DT_CANVAS_TOOL_NONE;
  }
}

dt_canvas_tool_t dt_canvas_tool_toggled(const dt_canvas_tool_t armed, const dt_canvas_tool_t asked)
{
  // A value from nowhere is no tool: it neither arms one nor leaves the armed one to answer a press
  // it was not asked about.
  if(asked <= DT_CANVAS_TOOL_NONE || asked >= DT_CANVAS_TOOL_COUNT) return DT_CANVAS_TOOL_NONE;
  if(asked == armed) return DT_CANVAS_TOOL_NONE;
  return asked;
}

gboolean dt_canvas_tool_draws_line(const dt_canvas_tool_t tool)
{
  return tool == DT_CANVAS_TOOL_LINE || tool == DT_CANVAS_TOOL_CURVE;
}

gboolean dt_canvas_tool_draws_shape(const dt_canvas_tool_t tool)
{
  return tool == DT_CANVAS_TOOL_RECTANGLE || dt_canvas_tool_draws_regular(tool);
}

gboolean dt_canvas_tool_draws_regular(const dt_canvas_tool_t tool)
{
  return tool == DT_CANVAS_TOOL_POLYGON || tool == DT_CANVAS_TOOL_STAR;
}

gboolean dt_canvas_tool_draws(const dt_canvas_tool_t tool)
{
  return dt_canvas_tool_draws_line(tool) || dt_canvas_tool_draws_shape(tool);
}

/*
 * Indexed by the action, so the table cannot fall out of step with the enum: an action appended
 * without a name here reads as having no shortcut, which the unit test notices for the tools.
 */
static const char *const _accel_names[DT_CANVAS_ACTION_LAST] = {
  [DT_CANVAS_ACTION_NEW] = N_("New canvas"),
  [DT_CANVAS_ACTION_OPEN] = N_("Open a canvas"),
  [DT_CANVAS_ACTION_SAVE] = N_("Save the canvas"),
  [DT_CANVAS_ACTION_SAVE_AS] = N_("Save the canvas as"),
  [DT_CANVAS_ACTION_EXPORT] = N_("Export the canvas..."),
  [DT_CANVAS_ACTION_ADD_TEXT] = N_("Add a text frame"),
  [DT_CANVAS_ACTION_ADD_NOTES] = N_("Add the text notes of the selected images"),
  [DT_CANVAS_ACTION_ADD_MAP] = N_("Add a map"),
  [DT_CANVAS_ACTION_ZOOM_FIT] = N_("Fit the view to the canvas"),
  [DT_CANVAS_ACTION_ZOOM_100] = N_("Zoom to 100%"),
  [DT_CANVAS_ACTION_SYNC_CHECK] = N_("Check the images against the library"),
  [DT_CANVAS_ACTION_SYNC_REFRESH_STALE] = N_("Refresh the stale images"),
  [DT_CANVAS_ACTION_LAYOUT_GRID] = N_("Arrange as a grid"),
  [DT_CANVAS_ACTION_LAYOUT_MASONRY] = N_("Arrange as a masonry"),
  [DT_CANVAS_ACTION_LAYOUT_ROW] = N_("Arrange as a row"),
  [DT_CANVAS_ACTION_LAYOUT_COLUMN] = N_("Arrange as a column"),
  [DT_CANVAS_ACTION_TOGGLE_GRID] = N_("Toggle the grid"),
  [DT_CANVAS_ACTION_TOGGLE_SNAP] = N_("Toggle snapping to the grid"),
  [DT_CANVAS_ACTION_UNDO] = N_("Undo"),
  [DT_CANVAS_ACTION_REDO] = N_("Redo"),
  [DT_CANVAS_ACTION_CONNECT_MODE] = N_("Draw a connector"),
  [DT_CANVAS_ACTION_ADD_SVG] = N_("Place a drawing"),
  [DT_CANVAS_ACTION_PROPERTIES] = N_("Show the properties of the selected object"),
  [DT_CANVAS_ACTION_DRAW_LINE] = N_("Draw a line"),
  [DT_CANVAS_ACTION_DRAW_CURVE] = N_("Draw a curve"),
  [DT_CANVAS_ACTION_DRAW_RECTANGLE] = N_("Draw a rectangle"),
  [DT_CANVAS_ACTION_DRAW_POLYGON] = N_("Draw a polygon"),
  [DT_CANVAS_ACTION_DRAW_STAR] = N_("Draw a star"),
};

const char *dt_canvas_action_accel_name(const dt_canvas_action_t action)
{
  if(action < DT_CANVAS_ACTION_NEW || action >= DT_CANVAS_ACTION_LAST) return NULL;
  return _accel_names[action];
}

const char *dt_canvas_action_accel_scope(void)
{
  return N_("Canvas/Actions");
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
