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

/**
 * The atelier's vocabulary of actions and tools, which is GTK-free on purpose: the toolbar, the
 * shortcut table and the view all name the same things through it, and this is where they are pinned.
 */

#include "canvas/canvas_actions.h"

#include "system/macros.h" // IS_NULL_PTR

#include <glib.h>
// cmocka.h declares `extern jmp_buf global_expect_assert_env' without including <setjmp.h>.
#include <setjmp.h>  // NOLINT(misc-include-cleaner)
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <cmocka.h>

/** Each tool action names its own tool, and nothing else names one. */
static void _each_tool_action_arms_its_own_tool(void **state)
{
  (void)state;
  assert_int_equal(dt_canvas_tool_for_action(DT_CANVAS_ACTION_CONNECT_MODE), DT_CANVAS_TOOL_CONNECTOR);
  assert_int_equal(dt_canvas_tool_for_action(DT_CANVAS_ACTION_DRAW_LINE), DT_CANVAS_TOOL_LINE);
  assert_int_equal(dt_canvas_tool_for_action(DT_CANVAS_ACTION_DRAW_CURVE), DT_CANVAS_TOOL_CURVE);
  assert_int_equal(dt_canvas_tool_for_action(DT_CANVAS_ACTION_DRAW_RECTANGLE), DT_CANVAS_TOOL_RECTANGLE);
  int tools = 0;
  for(int action = 0; action < DT_CANVAS_ACTION_LAST; action++)
    if(dt_canvas_tool_for_action((dt_canvas_action_t)action) != DT_CANVAS_TOOL_NONE) tools++;
  assert_int_equal(tools, DT_CANVAS_TOOL_COUNT - 1);
  assert_int_equal(dt_canvas_tool_for_action(DT_CANVAS_ACTION_ADD_TEXT), DT_CANVAS_TOOL_NONE);
  assert_int_equal(dt_canvas_tool_for_action(DT_CANVAS_ACTION_LAST), DT_CANVAS_TOOL_NONE);
  assert_int_equal(dt_canvas_tool_for_action((dt_canvas_action_t)-1), DT_CANVAS_TOOL_NONE);
}

/**
 * A tool's action is a toggle: it arms its tool, takes an armed one's place, and puts its own away
 * when it is asked for again. That last case is what keeps a tool armed across the objects it draws
 * and still lets its own key be the way out.
 */
static void _asking_for_the_armed_tool_puts_it_away(void **state)
{
  (void)state;
  for(int asked = DT_CANVAS_TOOL_CONNECTOR; asked < DT_CANVAS_TOOL_COUNT; asked++)
  {
    // From nothing armed, every tool arms itself.
    assert_int_equal(dt_canvas_tool_toggled(DT_CANVAS_TOOL_NONE, (dt_canvas_tool_t)asked), asked);
    // Asked again while it is the one armed, it leaves nothing armed.
    assert_int_equal(dt_canvas_tool_toggled((dt_canvas_tool_t)asked, (dt_canvas_tool_t)asked), DT_CANVAS_TOOL_NONE);
    for(int armed = DT_CANVAS_TOOL_CONNECTOR; armed < DT_CANVAS_TOOL_COUNT; armed++)
    {
      if(armed == asked) continue;
      assert_int_equal(dt_canvas_tool_toggled((dt_canvas_tool_t)armed, (dt_canvas_tool_t)asked), asked);
    }
  }
  // Nothing, and anything that is not a tool, arms nothing -- whatever was armed before.
  assert_int_equal(dt_canvas_tool_toggled(DT_CANVAS_TOOL_LINE, DT_CANVAS_TOOL_NONE), DT_CANVAS_TOOL_NONE);
  assert_int_equal(dt_canvas_tool_toggled(DT_CANVAS_TOOL_LINE, DT_CANVAS_TOOL_COUNT), DT_CANVAS_TOOL_NONE);
  assert_int_equal(dt_canvas_tool_toggled(DT_CANVAS_TOOL_NONE, (dt_canvas_tool_t)-3), DT_CANVAS_TOOL_NONE);
}

/** The line and the curve draw a line; the connector joins two frames and the rest draw nothing. */
static void _the_line_and_the_curve_are_the_line_tools(void **state)
{
  (void)state;
  assert_true(dt_canvas_tool_draws_line(DT_CANVAS_TOOL_LINE));
  assert_true(dt_canvas_tool_draws_line(DT_CANVAS_TOOL_CURVE));
  assert_false(dt_canvas_tool_draws_line(DT_CANVAS_TOOL_CONNECTOR));
  assert_false(dt_canvas_tool_draws_line(DT_CANVAS_TOOL_RECTANGLE));
  assert_false(dt_canvas_tool_draws_line(DT_CANVAS_TOOL_NONE));
  assert_false(dt_canvas_tool_draws_line(DT_CANVAS_TOOL_COUNT));
}

/**
 * Every tool but the connector draws a new object with one gesture of its own, which is what the
 * crosshair, the start marker and "a press draws instead of picking" all ask. The connector joins
 * two frames that are there already and answers a click on each.
 */
static void _every_tool_but_the_connector_draws_an_object(void **state)
{
  (void)state;
  assert_true(dt_canvas_tool_draws_shape(DT_CANVAS_TOOL_RECTANGLE));
  assert_false(dt_canvas_tool_draws_shape(DT_CANVAS_TOOL_LINE));
  assert_false(dt_canvas_tool_draws_shape(DT_CANVAS_TOOL_CONNECTOR));
  assert_false(dt_canvas_tool_draws_shape(DT_CANVAS_TOOL_NONE));
  for(int tool = DT_CANVAS_TOOL_CONNECTOR; tool < DT_CANVAS_TOOL_COUNT; tool++)
    assert_int_equal(dt_canvas_tool_draws((dt_canvas_tool_t)tool), tool != DT_CANVAS_TOOL_CONNECTOR);
  assert_false(dt_canvas_tool_draws(DT_CANVAS_TOOL_NONE));
  assert_false(dt_canvas_tool_draws(DT_CANVAS_TOOL_COUNT));
}

/**
 * The names ARE the paths the user's own bindings are saved under, so the published ones are spelled
 * here once and for all: a rename orphans the key a user gave that action, silently. The two tools
 * added with the drawing modes must have a name, or their shortcut is never registered at all.
 */
static void _published_accel_names_are_fixed(void **state)
{
  (void)state;
  assert_string_equal(dt_canvas_action_accel_name(DT_CANVAS_ACTION_NEW), "New canvas");
  assert_string_equal(dt_canvas_action_accel_name(DT_CANVAS_ACTION_OPEN), "Open a canvas");
  assert_string_equal(dt_canvas_action_accel_name(DT_CANVAS_ACTION_SAVE), "Save the canvas");
  assert_string_equal(dt_canvas_action_accel_name(DT_CANVAS_ACTION_SAVE_AS), "Save the canvas as");
  assert_string_equal(dt_canvas_action_accel_name(DT_CANVAS_ACTION_EXPORT), "Export the canvas...");
  assert_string_equal(dt_canvas_action_accel_name(DT_CANVAS_ACTION_ADD_TEXT), "Add a text frame");
  assert_string_equal(dt_canvas_action_accel_name(DT_CANVAS_ACTION_ADD_NOTES),
                      "Add the text notes of the selected images");
  assert_string_equal(dt_canvas_action_accel_name(DT_CANVAS_ACTION_ADD_MAP), "Add a map");
  assert_string_equal(dt_canvas_action_accel_name(DT_CANVAS_ACTION_ADD_SVG), "Place a drawing");
  assert_string_equal(dt_canvas_action_accel_name(DT_CANVAS_ACTION_ZOOM_FIT), "Fit the view to the canvas");
  assert_string_equal(dt_canvas_action_accel_name(DT_CANVAS_ACTION_ZOOM_100), "Zoom to 100%");
  assert_string_equal(dt_canvas_action_accel_name(DT_CANVAS_ACTION_SYNC_CHECK),
                      "Check the images against the library");
  assert_string_equal(dt_canvas_action_accel_name(DT_CANVAS_ACTION_SYNC_REFRESH_STALE), "Refresh the stale images");
  assert_string_equal(dt_canvas_action_accel_name(DT_CANVAS_ACTION_LAYOUT_GRID), "Arrange as a grid");
  assert_string_equal(dt_canvas_action_accel_name(DT_CANVAS_ACTION_LAYOUT_MASONRY), "Arrange as a masonry");
  assert_string_equal(dt_canvas_action_accel_name(DT_CANVAS_ACTION_LAYOUT_ROW), "Arrange as a row");
  assert_string_equal(dt_canvas_action_accel_name(DT_CANVAS_ACTION_LAYOUT_COLUMN), "Arrange as a column");
  assert_string_equal(dt_canvas_action_accel_name(DT_CANVAS_ACTION_TOGGLE_GRID), "Toggle the grid");
  assert_string_equal(dt_canvas_action_accel_name(DT_CANVAS_ACTION_TOGGLE_SNAP), "Toggle snapping to the grid");
  assert_string_equal(dt_canvas_action_accel_name(DT_CANVAS_ACTION_UNDO), "Undo");
  assert_string_equal(dt_canvas_action_accel_name(DT_CANVAS_ACTION_REDO), "Redo");
  assert_string_equal(dt_canvas_action_accel_name(DT_CANVAS_ACTION_CONNECT_MODE), "Draw a connector");
  assert_string_equal(dt_canvas_action_accel_name(DT_CANVAS_ACTION_PROPERTIES),
                      "Show the properties of the selected object");
  assert_string_equal(dt_canvas_action_accel_name(DT_CANVAS_ACTION_DRAW_LINE), "Draw a line");
  assert_string_equal(dt_canvas_action_accel_name(DT_CANVAS_ACTION_DRAW_CURVE), "Draw a curve");
  assert_string_equal(dt_canvas_action_accel_name(DT_CANVAS_ACTION_DRAW_RECTANGLE), "Draw a rectangle");
  assert_string_equal(dt_canvas_action_accel_scope(), "Canvas/Actions");
  // Every tool is reachable from the keyboard, and no two actions share a path.
  for(int action = 0; action < DT_CANVAS_ACTION_LAST; action++)
  {
    const char *name = dt_canvas_action_accel_name((dt_canvas_action_t)action);
    if(dt_canvas_tool_for_action((dt_canvas_action_t)action) != DT_CANVAS_TOOL_NONE) assert_non_null(name);
    if(IS_NULL_PTR(name)) continue;
    assert_true(name[0] != '\0');
    for(int other = 0; other < action; other++)
    {
      const char *earlier = dt_canvas_action_accel_name((dt_canvas_action_t)other);
      if(IS_NULL_PTR(earlier)) continue;
      if(g_strcmp0(name, earlier) == 0) fail_msg("actions %d and %d share the accel name \"%s\"", other, action, name);
    }
  }
  assert_null(dt_canvas_action_accel_name(DT_CANVAS_ACTION_LAST));
  assert_null(dt_canvas_action_accel_name((dt_canvas_action_t)-1));
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_each_tool_action_arms_its_own_tool),
    cmocka_unit_test(_asking_for_the_armed_tool_puts_it_away),
    cmocka_unit_test(_the_line_and_the_curve_are_the_line_tools),
    cmocka_unit_test(_every_tool_but_the_connector_draws_an_object),
    cmocka_unit_test(_published_accel_names_are_fixed),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
