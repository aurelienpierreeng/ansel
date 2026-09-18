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
 * @file libs/tools/canvas_toolbar.c
 * @brief The Canvas atelier's toolbar: what belongs to the whole canvas.
 *
 * @details The canvas and object menus, the guides, the buttons that add frames, the plane's
 * background and texture, the defaults every frame inherits (borders, corners, shadows), the zoom
 * and the layouts. What belongs to ONE object is not here: it is in that object's floating
 * properties, which the view owns (views/canvas_props_gtk.h).
 *
 * The toolbar holds no document state. Every button asks the view for an action through
 * `proxy.canvas` (see canvas/canvas_actions.h), and every control that mirrors a document setting is
 * refilled from the document on DT_SIGNAL_CANVAS_CHANGED and on entering the atelier. A refill
 * blocks every handler it could wake, one stored handler id at a time, so a value written into a
 * control never reaches the view back as an edit.
 *
 * A slider needs more than that, in both directions, and both are paid for here. What it SENDS is
 * one value per motion event, so the settings it edits are asked for through the phase-aware
 * `proxy.canvas.edit_number` and a whole gesture is one undo step. What it is GIVEN rewrites its
 * display range, so a refill skips a slider already showing what the document holds rather than
 * merely blocking its handler -- writing it back would collapse the range under the pointer.
 */

#include "canvas/canvas.h"
#include "canvas/canvas_actions.h"
#include "canvas/canvas_props.h"      // dt_canvas_edit_phase_t, the rows the number sliders read
#include "common/conf.h"
#include "common/gui_module_api.h"    // DT_GUI_MODULE
#include "common/module_versioning.h"
#include "control/signal.h"
#include "gui/window_manager.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "system/macros.h"
#include "system/mem_alloc.h"
#include "views/view.h"
#include "widgets/accelerators.h"     // dt_accels_block_plain_keys_inside, dt_accels_build_path
#include "widgets/bauhaus.h"          // the popover sliders
#include "widgets/button.h"           // dtgtk_button_new
#include "widgets/chooser_button.h"
#include "widgets/length_field.h"    // the guides are set in a unit, not in bare numbers
#include "widgets/paint.h"            // the glyphs the icon groups show
#include "widgets/togglebutton.h"     // dtgtk_togglebutton_new
#include "widgets/widget_settings.h"
#include "widgets/widget_style.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <math.h>

DT_MODULE(1)

/** A handler on a control the document refills, kept so a refill can block it. */
typedef struct dt_lib_canvas_toolbar_handler_t
{
  GObject *instance;
  gulong handler_id;
} dt_lib_canvas_toolbar_handler_t;

/** How many sliders edit a canvas-wide number: the border's width and the corners' radius, the
 * shadow's two offsets and its blur, and the line's width. `_prop_slider()` builds NO slider
 * once this is full rather than overrun the array, so a row added without raising it is a row
 * that silently does not appear. */
#define DT_CANVAS_TOOLBAR_NUMBERS 6

/** The biggest a custom page may be, in points: ten metres, past any press and any screen. */
#define CANVAS_TOOLBAR_CUSTOM_PAPER_MAX 28346.0

/** How long a gesture nothing holds -- a wheel step, an arrow key, the fine-tune popup -- waits
 * for the next step before it counts as over. */
#define DT_CANVAS_TOOLBAR_DEBOUNCE_MS 400

/**
 * One popover slider bound to a canvas-wide number, and whatever gesture the user has open on it.
 * The slider is what shows the number; the document is what holds it, and the view is asked for
 * every change through `proxy.canvas.edit_number` -- see `_number_edit_live()`.
 */
typedef struct dt_lib_canvas_number_t
{
  dt_lib_module_t *self;
  GtkWidget *slider;
  int prop;                ///< the dt_canvas_prop_id_t the slider edits
  gboolean pressed;        ///< button 1 is down on it
  gboolean double_clicked; ///< the press now down is the second click of a double click
  float press_number;      ///< what the slider showed when that button went down
} dt_lib_canvas_number_t;

typedef struct dt_lib_canvas_toolbar_t
{
  /** The DRAW group, indexed by the tool each toggle arms; the slot of DT_CANVAS_TOOL_NONE stays
   * NULL, since no button offers "no tool" -- putting one away is the armed button pressed again. */
  GtkWidget *tool_toggles[DT_CANVAS_TOOL_COUNT];
  // the guides popover
  GtkWidget *grid_show;
  GtkWidget *grid_snap;
  GtkWidget *grid_size;
  GtkWidget *grid_color;
  GtkWidget *page_show;
  GtkWidget *page_over;
  GtkWidget *resolution;
  GtkWidget *spread_cols;
  GtkWidget *spread_rows;
  GtkWidget *bind_gutter;
  GtkWidget *page_snap;
  GtkWidget *page_size;
  GtkWidget *page_orientation;
  GtkWidget *page_color;
  GtkWidget *padding_snap;
  GtkWidget *padding_size;
  GtkWidget *margin_show;
  GtkWidget *margin_snap;
  GtkWidget *margin_size;
  GtkWidget *margin_color;
  GtkWidget *bleed_show;
  GtkWidget *bleed_snap;
  GtkWidget *bleed_size;
  GtkWidget *bleed_color;
  GtkWidget *padding_show;
  GtkWidget *padding_color;
  GtkWidget *size_snap;
  // the texture popover
  GtkWidget *texture_contrast;
  GtkWidget *texture_detail;
  GtkWidget *texture_scale;
  GtkWidget *texture_grain;
  // the shadow popover
  GtkWidget *shadow_color;
  // the rest of the row
  GtkWidget *background_color;
  GtkWidget *background_style;
  GtkWidget *border_color;
  GtkWidget *line_color;
  GtkWidget *custom_width;
  GtkWidget *custom_height;
  GtkWidget *layout;
  GtkWidget *sort;
  /** The sliders of the Borders and Shadows popovers, in the order they were built. */
  dt_lib_canvas_number_t numbers[DT_CANVAS_TOOLBAR_NUMBERS];
  int number_count;
  int number_live;            ///< the dt_canvas_prop_id_t a LIVE session is open on, NONE for none
  guint number_commit_source; ///< the timer ending a session nothing holds
  GArray *refilled_handlers;  ///< dt_lib_canvas_toolbar_handler_t: every handler a refill blocks
} dt_lib_canvas_toolbar_t;

const char *name(dt_lib_module_t *self)
{
  return _("Canvas");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = { "canvas", NULL };
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_TOP_SECOND_ROW;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position()
{
  return 1001;
}

/* --- talking to the view -------------------------------------------------------------- */

static dt_view_t *_canvas_view(void)
{
  return dt_view_manager_get_global()->proxy.canvas.view;
}

static const dt_canvas_t *_document(void)
{
  dt_view_t *view = _canvas_view();
  if(IS_NULL_PTR(view) || IS_NULL_PTR(dt_view_manager_get_global()->proxy.canvas.document)) return NULL;
  return dt_view_manager_get_global()->proxy.canvas.document(view);
}

static void _ask(const dt_canvas_action_t action)
{
  dt_view_t *view = _canvas_view();
  if(IS_NULL_PTR(view) || IS_NULL_PTR(dt_view_manager_get_global()->proxy.canvas.action)) return;
  dt_view_manager_get_global()->proxy.canvas.action(view, action);
}

static void _action_clicked(GtkWidget *widget, gpointer user_data)
{
  _ask((dt_canvas_action_t)GPOINTER_TO_INT(user_data));
}

/** The atelier's view, when there is one to send an edit to. */
static gboolean _live(dt_view_t **view)
{
  *view = _canvas_view();
  return !IS_NULL_PTR(*view);
}

/**
 * Connect a handler on a control the document refills. Every such handler goes through here and
 * nowhere else, so none can be missed by the refill that must block it: a handler left awake writes
 * the value being refilled straight back into the document, and a handler that reads SEVERAL controls
 * -- the margin with the bleed, the three spread spins -- writes every one of them as the refill left
 * it so far, which is not the document yet.
 */
static void _connect_refilled_data(dt_lib_module_t *self, GtkWidget *widget, const char *signal,
                                   GCallback callback, gpointer user_data)
{
  dt_lib_canvas_toolbar_t *toolbar = (dt_lib_canvas_toolbar_t *)self->data;
  dt_lib_canvas_toolbar_handler_t handler;
  handler.instance = G_OBJECT(widget);
  handler.handler_id = g_signal_connect(widget, signal, callback, user_data);
  g_array_append_val(toolbar->refilled_handlers, handler);
}

/** The usual form: the handler is handed the module, as every control but the number sliders wants. */
static void _connect_refilled(dt_lib_module_t *self, GtkWidget *widget, const char *signal, GCallback callback)
{
  _connect_refilled_data(self, widget, signal, callback, self);
}

static void _refilled_handlers_block(dt_lib_canvas_toolbar_t *toolbar, const gboolean block)
{
  for(guint idx = 0; idx < toolbar->refilled_handlers->len; idx++)
  {
    const dt_lib_canvas_toolbar_handler_t *handler
        = &g_array_index(toolbar->refilled_handlers, dt_lib_canvas_toolbar_handler_t, idx);
    if(block)
      g_signal_handler_block(handler->instance, handler->handler_id);
    else
      g_signal_handler_unblock(handler->instance, handler->handler_id);
  }
}

/** Show a document colour on its button. The button reports nothing when told, so no handler is blocked. */
static void _rgba_to(GtkWidget *button, const dt_canvas_color_t *color, const gboolean with_alpha)
{
  GdkRGBA rgba = { color->red, color->green, color->blue, with_alpha ? color->alpha : 1.0 };
  dt_chooser_button_set_color(button, &rgba);
}

/* --- handlers ------------------------------------------------------------------------- */

/** The tool the view has armed: the toggles show it and never keep a state of their own. */
static dt_canvas_tool_t _armed_tool(void)
{
  dt_view_t *view = _canvas_view();
  if(IS_NULL_PTR(view) || IS_NULL_PTR(dt_view_manager_get_global()->proxy.canvas.armed_tool))
    return DT_CANVAS_TOOL_NONE;
  return (dt_canvas_tool_t)dt_view_manager_get_global()->proxy.canvas.armed_tool(view);
}

/** Where a tool's toggle keeps the action that arms it; the tool itself is the toggle's index. */
#define TOOLBAR_TOOL_ACTION_KEY "dt-canvas-tool-action"

static void _tool_toggled(GtkToggleButton *button, gpointer user_data)
{
  dt_view_t *view = NULL;
  if(!_live(&view)) return;
  const dt_canvas_action_t action
      = (dt_canvas_action_t)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), TOOLBAR_TOOL_ACTION_KEY));
  const dt_canvas_tool_t tool = dt_canvas_tool_for_action(action);
  // A toggle with no action on it reads as action 0, which is New canvas: nothing here may act on an
  // action that arms no tool, or a lost tag would throw the document away instead of doing nothing.
  if(tool == DT_CANVAS_TOOL_NONE) return;
  // Asked only when the button and the view disagree: a tool's action is a toggle, and a button
  // already showing what the view holds would otherwise put the tool away again.
  if(gtk_toggle_button_get_active(button) != (_armed_tool() == tool)) _ask(action);
}

/** A guides checkbox: its flag bit is in "guide-flag". */
static void _guide_flag_toggled(GtkToggleButton *button, gpointer user_data)
{
  dt_view_t *view = NULL;
  if(!_live(&view)) return;
  if(IS_NULL_PTR(dt_view_manager_get_global()->proxy.canvas.set_guides)) return;
  const int flag = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "guide-flag"));
  dt_view_manager_get_global()->proxy.canvas.set_guides(view, flag, gtk_toggle_button_get_active(button) ? flag : 0);
}

static void _spread_changed(GtkSpinButton *spin, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_canvas_toolbar_t *toolbar = (dt_lib_canvas_toolbar_t *)self->data;
  dt_view_t *view = NULL;
  if(!_live(&view)) return;
  if(IS_NULL_PTR(dt_view_manager_get_global()->proxy.canvas.set_spread)) return;
  dt_view_manager_get_global()->proxy.canvas.set_spread(
      view, (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(toolbar->spread_cols)),
      (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(toolbar->spread_rows)),
      (float)gtk_spin_button_get_value(GTK_SPIN_BUTTON(toolbar->bind_gutter)));
}

static void _resolution_changed(GtkSpinButton *spin, gpointer user_data)
{
  dt_view_t *view = NULL;
  if(!_live(&view)) return;
  if(IS_NULL_PTR(dt_view_manager_get_global()->proxy.canvas.set_resolution)) return;
  dt_view_manager_get_global()->proxy.canvas.set_resolution(view, (float)gtk_spin_button_get_value(spin));
}

static void _grid_size_changed(GtkSpinButton *spin, gpointer user_data)
{
  dt_view_t *view = NULL;
  if(!_live(&view)) return;
  if(IS_NULL_PTR(dt_view_manager_get_global()->proxy.canvas.set_grid_size)) return;
  dt_view_manager_get_global()->proxy.canvas.set_grid_size(view, (float)gtk_spin_button_get_value(spin));
}

static void _padding_changed(GtkSpinButton *spin, gpointer user_data)
{
  dt_view_t *view = NULL;
  if(!_live(&view)) return;
  if(IS_NULL_PTR(dt_view_manager_get_global()->proxy.canvas.set_padding)) return;
  dt_view_manager_get_global()->proxy.canvas.set_padding(view, (float)gtk_spin_button_get_value(spin));
}

/** Where a colour button keeps the canvas colour it edits. */
#define TOOLBAR_COLOR_TARGET_KEY "dt-canvas-color-target"

/**
 * A colour window of the toolbar. While it is open the view shows each change and records nothing;
 * its closing sets the colour through the view, as one undo step where the colour had one before,
 * or gives the colour it found back.
 */
static void _color_changed(GtkWidget *button, const GdkRGBA *color, const dt_chooser_color_phase_t phase,
                           gpointer user_data)
{
  dt_view_t *view = NULL;
  if(!_live(&view)) return;
  if(IS_NULL_PTR(dt_view_manager_get_global()->proxy.canvas.edit_color)) return;
  const int target = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), TOOLBAR_COLOR_TARGET_KEY));
  const float rgba[4] = { (float)color->red, (float)color->green, (float)color->blue, (float)color->alpha };
  int edit_phase = DT_CANVAS_EDIT_LIVE;
  if(phase == DT_CHOOSER_COLOR_COMMIT)
    edit_phase = DT_CANVAS_EDIT_COMMIT;
  else if(phase == DT_CHOOSER_COLOR_CANCEL)
    edit_phase = DT_CANVAS_EDIT_CANCEL;
  dt_view_manager_get_global()->proxy.canvas.edit_color(view, target, rgba, edit_phase);
}

/**
 * A colour button of the toolbar, sharing the atelier's recent colours. `title` heads its window, where
 * the tooltip would be too long.
 */
static GtkWidget *_color_button(const char *title, const char *tooltip, const dt_canvas_color_target_t target,
                                const gboolean use_alpha, dt_lib_module_t *self)
{
  GtkWidget *button
      = dt_chooser_button_color_new(title, use_alpha, DT_CANVAS_COLOR_HISTORY_KEY, _color_changed, self);
  g_object_set_data(G_OBJECT(button), TOOLBAR_COLOR_TARGET_KEY, GINT_TO_POINTER(target));
  gtk_widget_set_tooltip_text(button, tooltip);
  return button;
}

/**
 * Whether a slider already shows a given number, at the precision its property is shown to. Two
 * questions are asked with it: has the document already got what the slider is announcing (a slider
 * announces the value it just sent -- once more as a drag's button comes up, and on every motion
 * whether the pointer moved or not), and has a refill anything to write into a slider at all.
 *
 * Asked at the SHOWN precision, as the card's own sliders ask it (`_shown_equal()`,
 * views/canvas_props_gtk.c), because a slider keeps a normalised position and multiplies it out
 * again: measured, one refilled with 3 answers 2.99999714 and one refilled with 9 answers
 * 9.00000191, which no exact comparison stops.
 * Nothing found is no match, so an edit is let through rather than lost.
 */
static gboolean _slider_shows(GtkWidget *slider, const dt_canvas_prop_id_t prop_id, const float held)
{
  const dt_canvas_prop_t *prop = dt_canvas_prop_get(prop_id);
  if(IS_NULL_PTR(prop) || IS_NULL_PTR(slider)) return FALSE;
  const double quantum = pow(10.0, -(double)MAX(prop->digits, 0));
  return fabs((double)dt_bauhaus_slider_get(slider) - (double)held) < quantum * 0.5;
}

/** What the document holds for one of the numbers those sliders edit. */
static gboolean _number_held(const dt_canvas_t *canvas, const int prop, float *held)
{
  if(IS_NULL_PTR(canvas) || IS_NULL_PTR(held)) return FALSE;
  switch(prop)
  {
    case DT_CANVAS_PROP_BORDER_WIDTH:
      *held = canvas->border_width;
      return TRUE;
    case DT_CANVAS_PROP_CORNER_RADIUS:
      *held = canvas->corner_radius;
      return TRUE;
    case DT_CANVAS_PROP_SHADOW_OFFSET_X:
      *held = canvas->shadow.offset_x;
      return TRUE;
    case DT_CANVAS_PROP_SHADOW_OFFSET_Y:
      *held = canvas->shadow.offset_y;
      return TRUE;
    case DT_CANVAS_PROP_LINE_WIDTH:
      // What the line is DRAWN with, never the raw field: a canvas that has never been told
      // holds zero, and the slider would read nothing where the built-in line is on screen.
      dt_canvas_object_effective_line(canvas, NULL, NULL, held);
      return TRUE;
    case DT_CANVAS_PROP_SHADOW_BLUR:
      *held = canvas->shadow.blur;
      return TRUE;
    default:
      return FALSE;
  }
}

/** Whether the slider already shows what the document holds for the number it edits. */
static gboolean _number_at_rest(const dt_lib_canvas_number_t *number)
{
  float held = 0.0f;
  if(IS_NULL_PTR(number) || !_number_held(_document(), number->prop, &held)) return FALSE;
  return _slider_shows(number->slider, (dt_canvas_prop_id_t)number->prop, held);
}

/* --- one gesture on a number slider is one undo step ------------------------------------- */

/**
 * A step of the gesture, told to the view. The whole point of going through `edit_number` rather
 * than the setting's own setter is that a dragged slider reports every value it passes through:
 * LIVE steps write and show the number and cost nothing else, and the COMMIT that ends the gesture
 * is what writes the configuration and records the one undo step the gesture owes.
 */
static void _number_host_edit(const dt_lib_canvas_number_t *number, const float value, const int phase)
{
  dt_view_t *view = NULL;
  if(IS_NULL_PTR(number) || !_live(&view)) return;
  if(IS_NULL_PTR(dt_view_manager_get_global()->proxy.canvas.edit_number)) return;
  dt_view_manager_get_global()->proxy.canvas.edit_number(view, number->prop, value, phase);
}

static dt_lib_canvas_number_t *_number_by_prop(dt_lib_canvas_toolbar_t *toolbar, const int prop)
{
  if(prop == DT_CANVAS_PROP_NONE) return NULL;
  for(int idx = 0; idx < toolbar->number_count; idx++)
    if(toolbar->numbers[idx].prop == prop) return &toolbar->numbers[idx];
  return NULL;
}

static void _number_debounce_remove(dt_lib_canvas_toolbar_t *toolbar)
{
  if(toolbar->number_commit_source == 0) return;
  g_source_remove(toolbar->number_commit_source);
  toolbar->number_commit_source = 0;
}

/**
 * End the open session, if there is one, with what its slider now shows. The session is closed
 * BEFORE the view hears of it, so the refill the COMMIT raises writes the slider back to whatever
 * the document settled on rather than finding a gesture still in flight.
 */
static void _number_commit_live(dt_lib_canvas_toolbar_t *toolbar)
{
  _number_debounce_remove(toolbar);
  const dt_lib_canvas_number_t *number = _number_by_prop(toolbar, toolbar->number_live);
  toolbar->number_live = DT_CANVAS_PROP_NONE;
  if(IS_NULL_PTR(number)) return;
  _number_host_edit(number, dt_bauhaus_slider_get(number->slider), DT_CANVAS_EDIT_COMMIT);
}

static gboolean _number_debounce_fired(gpointer user_data)
{
  dt_lib_canvas_toolbar_t *toolbar = (dt_lib_canvas_toolbar_t *)user_data;
  toolbar->number_commit_source = 0;
  _number_commit_live(toolbar);
  return G_SOURCE_REMOVE;
}

/** Close the session later, unless another step or a press comes first. */
static void _number_commit_later(dt_lib_canvas_toolbar_t *toolbar, const guint delay_ms)
{
  _number_debounce_remove(toolbar);
  toolbar->number_commit_source = g_timeout_add(delay_ms, _number_debounce_fired, toolbar);
}

/** One step of a session on this slider, ending any other slider's session first. */
static void _number_edit_live(dt_lib_canvas_number_t *number, const float value)
{
  dt_lib_canvas_toolbar_t *toolbar = (dt_lib_canvas_toolbar_t *)number->self->data;
  if(toolbar->number_live != DT_CANVAS_PROP_NONE && toolbar->number_live != number->prop)
    _number_commit_live(toolbar);
  _number_debounce_remove(toolbar);
  toolbar->number_live = number->prop;
  _number_host_edit(number, value, DT_CANVAS_EDIT_LIVE);
}

/** How long the toolkit waits for the second click of a double click. */
static guint _double_click_ms(GtkWidget *widget)
{
  gint delay_ms = 400;
  g_object_get(gtk_widget_get_settings(widget), "gtk-double-click-time", &delay_ms, NULL);
  return (guint)MAX(delay_ms, 1);
}

/**
 * A number slider announcing a value. A button held on it is a gesture whose end its release says;
 * a step nothing holds -- the wheel, an arrow key, the fine-tune popup -- has nothing to say when
 * it is over, so a burst of them ends when the steps stop coming and is one undo step.
 */
static void _number_changed(GtkWidget *widget, gpointer user_data)
{
  dt_lib_canvas_number_t *number = (dt_lib_canvas_number_t *)user_data;
  dt_lib_canvas_toolbar_t *toolbar = (dt_lib_canvas_toolbar_t *)number->self->data;
  // A slider announces the value it already sent -- once more as a drag's button comes up, and on
  // every motion whether the pointer moved or not -- and the document already holds it.
  if(_number_at_rest(number)) return;
  _number_edit_live(number, dt_bauhaus_slider_get(widget));
  if(!number->pressed) _number_commit_later(toolbar, DT_CANVAS_TOOLBAR_DEBOUNCE_MS);
}

/**
 * A button pressed on or released from a number slider. Read AFTER the slider handled the event: it
 * moves to a click on its bar in its own press handler and stops the press from reaching anything
 * else, resets itself on a double click in that same handler, and emits its last value from its own
 * release handler. `event-after` runs once those have, whatever they returned.
 *
 * A click that did not drag is committed only once no second click came, because GDK delivers a
 * double click as a press, a RELEASE, a press and then the double-click press: committed on that
 * release, the position clicked would be an undo step of its own and the reset another, and one
 * Ctrl+Z after a double click would put back a number the user never chose.
 */
static void _number_event_after(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  dt_lib_canvas_number_t *number = (dt_lib_canvas_number_t *)user_data;
  dt_lib_canvas_toolbar_t *toolbar = (dt_lib_canvas_toolbar_t *)number->self->data;
  if(event->type == GDK_BUTTON_PRESS && event->button.button == 1)
  {
    number->pressed = TRUE;
    number->double_clicked = FALSE;
    number->press_number = dt_bauhaus_slider_get(widget);
    // A held button continues the session already open on this slider, and the timer a wheel step
    // or a first click left must not close it under the button; a session open on any other slider
    // ends here.
    if(toolbar->number_live == number->prop)
      _number_debounce_remove(toolbar);
    else
      _number_commit_live(toolbar);
    // The slider moved to the click on its bar without announcing it: opening the session here is
    // what makes the plane follow the click instead of waiting for a drag.
    if(!_number_at_rest(number)) _number_edit_live(number, number->press_number);
  }
  else if(event->type == GDK_2BUTTON_PRESS && event->button.button == 1)
  {
    number->double_clicked = TRUE;
  }
  else if(event->type == GDK_BUTTON_RELEASE && event->button.button == 1 && number->pressed)
  {
    number->pressed = FALSE;
    if(toolbar->number_live != number->prop) return;
    // A press that ends where it began moved nothing: it is a click, and may yet be half of a
    // double one.
    const gboolean clicked = !number->double_clicked && _slider_shows(widget, (dt_canvas_prop_id_t)number->prop,
                                                                     number->press_number);
    if(clicked)
      _number_commit_later(toolbar, _double_click_ms(widget));
    else
      _number_commit_live(toolbar);
  }
}

static void _page_guides_changed(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_canvas_toolbar_t *toolbar = (dt_lib_canvas_toolbar_t *)self->data;
  dt_view_t *view = NULL;
  if(!_live(&view)) return;
  if(IS_NULL_PTR(dt_view_manager_get_global()->proxy.canvas.set_page_guides)) return;
  dt_view_manager_get_global()->proxy.canvas.set_page_guides(
      view, (float)gtk_spin_button_get_value(GTK_SPIN_BUTTON(toolbar->margin_size)),
      (float)gtk_spin_button_get_value(GTK_SPIN_BUTTON(toolbar->bleed_size)));
}

static void _page_changed(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_canvas_toolbar_t *toolbar = (dt_lib_canvas_toolbar_t *)self->data;
  dt_view_t *view = NULL;
  if(!_live(&view)) return;
  if(IS_NULL_PTR(dt_view_manager_get_global()->proxy.canvas.set_paper)) return;
  // The list's order is not the stored value: a size appended to the enum shows where it belongs.
  const int position = gtk_combo_box_get_active(GTK_COMBO_BOX(toolbar->page_size));
  dt_view_manager_get_global()->proxy.canvas.set_paper(view, (int)dt_canvas_paper_code(position),
                                                     gtk_combo_box_get_active(GTK_COMBO_BOX(toolbar->page_orientation)));
}

/**
 * The background's style. It sends no colour, so a paper can bring its own; the colour button sends its
 * colour through the colour edits, and no style.
 */
static void _background_changed(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_canvas_toolbar_t *toolbar = (dt_lib_canvas_toolbar_t *)self->data;
  dt_view_t *view = NULL;
  if(!_live(&view)) return;
  if(IS_NULL_PTR(dt_view_manager_get_global()->proxy.canvas.set_background)) return;
  // The list's order is not the stored value: a background appended to the enum shows where
  // it belongs, which is how Transparent came to head a list it joined last.
  const int position = gtk_combo_box_get_active(GTK_COMBO_BOX(toolbar->background_style));
  dt_view_manager_get_global()->proxy.canvas.set_background(view, NULL, (int)dt_canvas_background_code(position));
}

static void _refill(dt_lib_module_t *self);

static void _texture_changed(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_canvas_toolbar_t *toolbar = (dt_lib_canvas_toolbar_t *)self->data;
  dt_view_t *view = NULL;
  if(!_live(&view)) return;
  if(IS_NULL_PTR(dt_view_manager_get_global()->proxy.canvas.set_texture)) return;
  const float contrast = dt_bauhaus_slider_get(toolbar->texture_contrast);
  const float detail = dt_bauhaus_slider_get(toolbar->texture_detail);
  const float scale = dt_bauhaus_slider_get(toolbar->texture_scale);
  const float grain = dt_bauhaus_slider_get(toolbar->texture_grain);
  // A bauhaus slider announces the value it already sent: once more when a drag's button comes up,
  // and on every motion whether the pointer moved or not -- measured, a drag over two positions
  // announced four times. Each announcement would cost the view four conf writes and a recomposite
  // of the whole canvas for a paper that did not change, so the document is asked first. It holds
  // the setter's clamped value, which a slider bounded by the same clamps sends back unchanged.
  float held_contrast = 1.0f;
  float held_detail = 1.0f;
  float held_scale = 1.0f;
  float held_grain = 1.0f;
  dt_canvas_texture_get(_document(), &held_contrast, &held_detail, &held_scale, &held_grain);
  if(contrast == held_contrast && detail == held_detail && scale == held_scale && grain == held_grain) return;
  dt_view_manager_get_global()->proxy.canvas.set_texture(view, contrast, detail, scale, grain);
}

static void _texture_reset(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_view_t *view = NULL;
  if(!_live(&view)) return;
  if(IS_NULL_PTR(dt_view_manager_get_global()->proxy.canvas.set_texture)) return;
  dt_view_manager_get_global()->proxy.canvas.set_texture(view, 1.0f, 1.0f, 1.0f, 1.0f);
  // Every other caller of set_texture is one of the four sliders sending its own value, and
  // refilling under a slider the user is still holding would fight the pointer -- so the
  // setter stays quiet and the ONE caller that writes all four behind their backs refreshes
  // them itself. Without it the reset reached the document and nothing else: the sliders
  // kept the old positions, so it read as doing nothing at all, and the next touch of any
  // slider sent all four stale values back and undid it.
  _refill(self);
}

static void _sort_changed(GtkComboBox *combo, gpointer user_data)
{
  dt_conf_set_int("canvas/layout_sort", CLAMP(gtk_combo_box_get_active(combo), 0, DT_CANVAS_SORT_LAST - 1));
}

static void _layout_apply(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_canvas_toolbar_t *toolbar = (dt_lib_canvas_toolbar_t *)self->data;
  static const dt_canvas_action_t layouts[] = { DT_CANVAS_ACTION_LAYOUT_GRID, DT_CANVAS_ACTION_LAYOUT_MASONRY,
                                                DT_CANVAS_ACTION_LAYOUT_ROW, DT_CANVAS_ACTION_LAYOUT_COLUMN };
  const int choice = gtk_combo_box_get_active(GTK_COMBO_BOX(toolbar->layout));
  if(choice < 0 || choice >= (int)G_N_ELEMENTS(layouts)) return;
  _ask(layouts[choice]);
}

/* --- refilling from the document --------------------------------------------------------- */

static void _refill(dt_lib_module_t *self)
{
  dt_lib_canvas_toolbar_t *toolbar = (dt_lib_canvas_toolbar_t *)self->data;
  const dt_canvas_t *canvas = _document();
  if(IS_NULL_PTR(canvas) || IS_NULL_PTR(toolbar)) return;
  _refilled_handlers_block(toolbar, TRUE);
  // Which tool is armed is the view's answer and the toolbar keeps none of its own, so exactly one
  // toggle can be pressed however the arming happened -- a key, a menu, the canvas being left.
  const dt_canvas_tool_t armed = _armed_tool();
  for(int tool = DT_CANVAS_TOOL_NONE + 1; tool < DT_CANVAS_TOOL_COUNT; tool++)
  {
    if(IS_NULL_PTR(toolbar->tool_toggles[tool])) continue;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolbar->tool_toggles[tool]), tool == (int)armed);
  }

  const uint32_t flags = canvas->grid_flags;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolbar->grid_show), (flags & DT_CANVAS_GRID_VISIBLE) != 0);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolbar->grid_snap), (flags & DT_CANVAS_GRID_SNAP) != 0);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(toolbar->grid_size), canvas->grid_size);
  _rgba_to(toolbar->grid_color, &canvas->grid_color, TRUE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolbar->page_show), (flags & DT_CANVAS_PAGE_VISIBLE) != 0);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolbar->page_snap), (flags & DT_CANVAS_SNAP_PAGE) != 0);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolbar->page_over), (flags & DT_CANVAS_GUIDES_OVER) != 0);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(toolbar->resolution), dt_canvas_resolution(canvas));
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(toolbar->spread_cols), canvas->spread_cols);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(toolbar->spread_rows), canvas->spread_rows);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(toolbar->bind_gutter), canvas->bind_gutter);
  gtk_combo_box_set_active(GTK_COMBO_BOX(toolbar->page_size), dt_canvas_paper_position(canvas->paper_size));
  /* The Custom page's own two sides, shown from what the DOCUMENT holds and never from the
   * combo's handler: the toolbar owns no state, so which rows exist is derived here or it is
   * state by another name. */
  const gboolean custom = canvas->paper_size == DT_CANVAS_PAPER_CUSTOM;
  gtk_widget_set_visible(toolbar->custom_width, custom);
  gtk_widget_set_visible(toolbar->custom_height, custom);
  double custom_width = 0.0;
  double custom_height = 0.0;
  dt_canvas_paper_custom(canvas, &custom_width, &custom_height);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(toolbar->custom_width), custom_width);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(toolbar->custom_height), custom_height);
  gtk_combo_box_set_active(GTK_COMBO_BOX(toolbar->page_orientation), canvas->paper_landscape ? 1 : 0);
  _rgba_to(toolbar->page_color, &canvas->page_color, TRUE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolbar->padding_snap), (flags & DT_CANVAS_SNAP_PADDING) != 0);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(toolbar->padding_size), canvas->padding);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolbar->size_snap), (flags & DT_CANVAS_SNAP_SIZE) != 0);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolbar->padding_show), (flags & DT_CANVAS_PADDING_VISIBLE) != 0);
  _rgba_to(toolbar->padding_color, &canvas->padding_color, TRUE);
  // The margin and the bleed are one setter, and a change to either sends both spins. Never refilled,
  // they showed 0 whatever the document or the last session held, and the first edit of one wrote the
  // other's 0 into the document.
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolbar->margin_show), (flags & DT_CANVAS_MARGIN_VISIBLE) != 0);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolbar->margin_snap), (flags & DT_CANVAS_SNAP_MARGIN) != 0);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(toolbar->margin_size), canvas->page_margin);
  _rgba_to(toolbar->margin_color, &canvas->margin_color, TRUE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolbar->bleed_show), (flags & DT_CANVAS_BLEED_VISIBLE) != 0);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolbar->bleed_snap), (flags & DT_CANVAS_SNAP_BLEED) != 0);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(toolbar->bleed_size), canvas->page_bleed);
  _rgba_to(toolbar->bleed_color, &canvas->bleed_color, TRUE);

  _rgba_to(toolbar->shadow_color, &canvas->shadow.color, TRUE);
  _rgba_to(toolbar->border_color, &canvas->border_color, TRUE);
  /* What the line is DRAWN with, never the raw field: a canvas that has never been told holds
   * zeros, and the well would show black-transparent where the built-in grey is on screen. */
  dt_canvas_color_t effective_line;
  dt_canvas_object_effective_line(canvas, NULL, &effective_line, NULL);
  _rgba_to(toolbar->line_color, &effective_line, TRUE);
  // A slider already showing what the document holds is left strictly alone, and NOT merely
  // blocked. `dt_bauhaus_slider_set()` rewrites the display range around the value it is given, so
  // a slider showing a number past its soft end -- a 120 pt border, where a drag covers 50 -- would
  // have that range collapse onto the value under the pointer mid-drag: the handle jumps to the far
  // end while the pointer has not moved, and the next motion reads against a range eight times
  // smaller. A gesture keeps the document at the value the slider shows, so this is precisely the
  // slider being dragged; the `pressed` test covers the other way in, a refill raised by something
  // else entirely -- an undo, a document opened -- while a button is down.
  for(int idx = 0; idx < toolbar->number_count; idx++)
  {
    dt_lib_canvas_number_t *number = &toolbar->numbers[idx];
    float held = 0.0f;
    if(IS_NULL_PTR(number->slider) || number->pressed) continue;
    if(!_number_held(canvas, number->prop, &held)) continue;
    if(_slider_shows(number->slider, (dt_canvas_prop_id_t)number->prop, held)) continue;
    dt_bauhaus_slider_set(number->slider, held);
  }
  _rgba_to(toolbar->background_color, &canvas->background, FALSE);
  gtk_combo_box_set_active(GTK_COMBO_BOX(toolbar->background_style),
                           dt_canvas_background_position(canvas->background_style));
  float contrast = 1.0f;
  float detail = 1.0f;
  float scale = 1.0f;
  float grain = 1.0f;
  dt_canvas_texture_get(canvas, &contrast, &detail, &scale, &grain);
  // These four are written unconditionally, unlike the numbers above, and may be: `set_texture`
  // raises nothing, so no refill ever runs under a texture slider the user is holding. They rely on
  // the block alone -- awake, each would send all four back to the document while the ones after it
  // still showed the previous paper.
  dt_bauhaus_slider_set(toolbar->texture_contrast, contrast);
  dt_bauhaus_slider_set(toolbar->texture_detail, detail);
  dt_bauhaus_slider_set(toolbar->texture_scale, scale);
  dt_bauhaus_slider_set(toolbar->texture_grain, grain);
  _refilled_handlers_block(toolbar, FALSE);
}

static void _canvas_changed(gpointer instance, gpointer user_data)
{
  _refill((dt_lib_module_t *)user_data);
}

/* --- building ------------------------------------------------------------------------------ */

/**
 * A control's tooltip: what the gesture is, and under it the key the user has for the same thing.
 * Only the first half is written here -- the widget is tagged with the accel path the view
 * registered the action under, and the global query-tooltip hook appends the binding at hover
 * time, so a key rebound in the shortcuts dialog shows without anything being rebuilt. The path is
 * asked of `canvas_actions.h` rather than spelled again, since a name spelled twice is a tooltip
 * that quietly stops finding its shortcut the day one of the two is edited.
 */
static void _tooltip_with_accel(GtkWidget *widget, const char *tooltip, const dt_canvas_action_t action)
{
  gtk_widget_set_tooltip_text(widget, tooltip);
  const char *action_name = dt_canvas_action_accel_name(action);
  if(IS_NULL_PTR(action_name)) return;
  g_object_set_data_full(G_OBJECT(widget), "accel-path",
                         dt_accels_build_path(dt_canvas_action_accel_scope(), action_name), dt_free_gpointer);
}

/**
 * A button here asks for one thing and gives the keyboard straight back: the plane is what answers
 * to the arrow keys, to Escape and to Space, and a button holding the focus answers to them first --
 * GtkWindow offers a key to the focus widget and to its own move-focus bindings before the
 * application's handler ever sees it (gui/application.c connects _key_pressed *after* the class
 * handler). The properties strip makes the same arrangement for the same widgets, through
 * `_no_focus_on_click()` (views/canvas_props_gtk.c). Tab still reaches every one of them.
 */
static void _no_focus_on_click(GtkWidget *widget)
{
  gtk_widget_set_focus_on_click(widget, FALSE);
}

static GtkWidget *_button(GtkWidget *box, const char *label, const char *tooltip, const dt_canvas_action_t action)
{
  GtkWidget *button = gtk_button_new_with_label(label);
  _no_focus_on_click(button);
  _tooltip_with_accel(button, tooltip, action);
  g_signal_connect(button, "clicked", G_CALLBACK(_action_clicked), GINT_TO_POINTER(action));
  gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);
  return button;
}

/** A box of buttons the theme draws as one control, squaring the corners they share. */
static GtkWidget *_linked_group(GtkWidget *box)
{
  GtkWidget *group = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  dt_gui_add_class(group, "linked");
  gtk_box_pack_start(GTK_BOX(box), group, FALSE, FALSE, 0);
  return group;
}

/** A glyph button of a group: one action, the gesture it takes, and the key it answers to. */
static GtkWidget *_icon_button(GtkWidget *box, DTGTKCairoPaintIconFunc paint, const gint flags,
                               const char *tooltip, const dt_canvas_action_t action)
{
  GtkWidget *button = dtgtk_button_new(paint, flags, NULL);
  _no_focus_on_click(button);
  _tooltip_with_accel(button, tooltip, action);
  g_signal_connect(button, "clicked", G_CALLBACK(_action_clicked), GINT_TO_POINTER(action));
  gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);
  return button;
}

/**
 * A glyph toggle showing whether its tool is the one armed. It holds no state: the handler asks the
 * view for the action and the refill puts the answer back, so arming from a key or from a menu
 * presses the same button as clicking it does.
 */
static GtkWidget *_tool_toggle(dt_lib_module_t *self, GtkWidget *box, DTGTKCairoPaintIconFunc paint,
                               const gint flags, const char *tooltip, const dt_canvas_action_t action)
{
  dt_lib_canvas_toolbar_t *toolbar = (dt_lib_canvas_toolbar_t *)self->data;
  const dt_canvas_tool_t tool = dt_canvas_tool_for_action(action);
  if(tool <= DT_CANVAS_TOOL_NONE || tool >= DT_CANVAS_TOOL_COUNT) return NULL;
  GtkWidget *toggle = dtgtk_togglebutton_new(paint, flags, NULL);
  _no_focus_on_click(toggle);
  _tooltip_with_accel(toggle, tooltip, action);
  g_object_set_data(G_OBJECT(toggle), TOOLBAR_TOOL_ACTION_KEY, GINT_TO_POINTER(action));
  _connect_refilled(self, toggle, "toggled", G_CALLBACK(_tool_toggled));
  gtk_box_pack_start(GTK_BOX(box), toggle, FALSE, FALSE, 0);
  toolbar->tool_toggles[tool] = toggle;
  return toggle;
}

static void _separator(GtkWidget *box)
{
  GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_margin_start(separator, DT_PIXEL_APPLY_DPI(6));
  gtk_widget_set_margin_end(separator, DT_PIXEL_APPLY_DPI(6));
  gtk_box_pack_start(GTK_BOX(box), separator, FALSE, FALSE, 0);
}

static GtkWidget *_menu_entry(GtkWidget *menu, const char *label, GCallback callback, gpointer data)
{
  GtkWidget *item = gtk_menu_item_new_with_label(label);
  g_signal_connect(item, "activate", callback, data);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
  return item;
}

static void _action_item(GtkWidget *menu, const char *label, const dt_canvas_action_t action)
{
  _menu_entry(menu, label, G_CALLBACK(_action_clicked), GINT_TO_POINTER(action));
}

/** A toolbar button that drops a menu. */
/** A menu button styled as a text label with an ellipsis: what opens something else, not an action. */
static GtkWidget *_flat_menu_button(GtkWidget *box, const char *label, const char *tooltip)
{
  GtkWidget *button = gtk_menu_button_new();
  gchar *text = g_strdup_printf("%s\xe2\x80\xa6", label);
  gtk_button_set_label(GTK_BUTTON(button), text);
  dt_free(text);
  gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
  gtk_widget_set_tooltip_text(button, tooltip);
  gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);
  return button;
}

static GtkWidget *_menu_button(GtkWidget *box, const char *label, const char *tooltip, GtkWidget *menu)
{
  GtkWidget *button = _flat_menu_button(box, label, tooltip);
  gtk_widget_show_all(menu);
  gtk_menu_button_set_popup(GTK_MENU_BUTTON(button), menu);
  return button;
}

static GtkWidget *_popover_button(GtkWidget *box, const char *label, const char *tooltip, GtkWidget *popover)
{
  GtkWidget *button = _flat_menu_button(box, label, tooltip);
  gtk_menu_button_set_popover(GTK_MENU_BUTTON(button), popover);
  return button;
}

/**
 * The half every popover slider shares: its name, what it does, the handler a refill blocks, and
 * its place in the popover.
 *
 * No width is asked for. A popover's parent is the window, not a side panel, so a slider takes
 * bauhaus's own fallback, 300 pixels at the screen's density, and a popover comes out about as
 * wide as the rows it replaces. A size request would change nothing: the slider rewrites its own
 * on every style update, and a request only ever raises a natural width, never lowers it.
 */
static GtkWidget *_popover_slider(dt_lib_module_t *self, GtkWidget *box, GtkWidget *slider, const char *label,
                                  const char *tooltip, GCallback callback, gpointer user_data)
{
  dt_bauhaus_widget_set_label(slider, label);
  gtk_widget_set_tooltip_text(slider, tooltip);
  _connect_refilled_data(self, slider, "value-changed", callback, user_data);
  gtk_box_pack_start(GTK_BOX(box), slider, FALSE, FALSE, 0);
  return slider;
}

/**
 * A texture slider of the popover. The hard range is what the view's setter clamps to, so the
 * fine-tune popup can type any value the document accepts and a document holding one past the soft
 * span shows it rather than a slider pinned at its end; the soft range is the span a drag covers.
 */
static GtkWidget *_texture_slider(dt_lib_module_t *self, GtkWidget *box, const char *label, const float hard_min,
                                  const float hard_max, const float soft_min, const float soft_max,
                                  const char *tooltip)
{
  GtkWidget *slider = dt_bauhaus_slider_new_with_range(dt_bauhaus_get_global(), DT_GUI_MODULE(NULL), hard_min,
                                                       hard_max, 0.05f, 1.0f, 2);
  dt_bauhaus_slider_set_soft_range(slider, soft_min, soft_max);
  return _popover_slider(self, box, slider, label, tooltip, G_CALLBACK(_texture_changed), self);
}

/**
 * A slider for one canvas-wide default, described by the property table row the same setting has
 * on an object's own card: the hard range nothing outside of is ever stored, the soft range a drag
 * covers, the step, the precision and the unit. Read there rather than written again here, so what
 * a frame inherits and what a card overrides it with cannot come to offer different numbers -- the
 * border spin stopped at 200 where the card's row goes to 500, and the corner spin offered every
 * whole unit up to 5000 where the card covers 200 and is typed past.
 *
 * The label and the tooltip stay the toolbar's own: these edit what every frame INHERITS, which the
 * card's wording, written for one object, does not say. The value a double click goes back to is 0
 * -- no border, square corners, no shadow -- and expressly not the row's minimum, which for a
 * shadow offset is -500: the number one unblocked refill once wrote into the document.
 *
 * Every such slider is registered as a number the gestures above are open on, so the value it sends
 * reaches the view through `edit_number` and not through a setter of its own: see
 * `_number_edit_live()` for why a dragged control may not be given one.
 */
static GtkWidget *_prop_slider(dt_lib_module_t *self, GtkWidget *box, const dt_canvas_prop_id_t prop_id,
                               const char *label, const char *tooltip)
{
  dt_lib_canvas_toolbar_t *toolbar = (dt_lib_canvas_toolbar_t *)self->data;
  // An id this file does not name a row for, or one slider more than there is room for, would be a
  // slider with no range and no document field behind it: better no row than a dead one.
  const dt_canvas_prop_t *prop = dt_canvas_prop_get(prop_id);
  if(IS_NULL_PTR(prop) || toolbar->number_count >= DT_CANVAS_TOOLBAR_NUMBERS) return NULL;
  GtkWidget *slider = dt_bauhaus_slider_new_with_range(dt_bauhaus_get_global(), DT_GUI_MODULE(NULL), (float)prop->min,
                                                       (float)prop->max, (float)prop->step, 0.0f, prop->digits);
  dt_bauhaus_slider_set_soft_range(slider, (float)prop->soft_min, (float)prop->soft_max);
  if(!IS_NULL_PTR(prop->unit))
  {
    gchar *format = g_strdup_printf(" %s", _(prop->unit));
    dt_bauhaus_slider_set_format(slider, format);
    dt_free(format);
  }
  dt_lib_canvas_number_t *number = &toolbar->numbers[toolbar->number_count++];
  number->self = self;
  number->slider = slider;
  number->prop = (int)prop_id;
  // The buttons are watched outside the refill's blocking: a refill synthesises no pointer event,
  // and what this handler reads is the gesture, never a value.
  g_signal_connect(slider, "event-after", G_CALLBACK(_number_event_after), number);
  return _popover_slider(self, box, slider, label, tooltip, G_CALLBACK(_number_changed), number);
}

/**
 * Both sides of a Custom page at once, the way the margin and the bleed share one setter: each
 * is read from its own field, so neither can send the other's stale value.
 */
static void _custom_paper_changed(GtkWidget *widget, gpointer user_data)
{
  (void)widget;
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_canvas_toolbar_t *toolbar = (dt_lib_canvas_toolbar_t *)self->data;
  dt_view_t *view = NULL;
  if(!_live(&view)) return;
  if(IS_NULL_PTR(dt_view_manager_get_global()->proxy.canvas.set_custom_paper)) return;
  dt_view_manager_get_global()->proxy.canvas.set_custom_paper(
      view, (float)gtk_spin_button_get_value(GTK_SPIN_BUTTON(toolbar->custom_width)),
      (float)gtk_spin_button_get_value(GTK_SPIN_BUTTON(toolbar->custom_height)));
}

/** A guides checkbox bound to one flag bit. */
static GtkWidget *_guide_check(dt_lib_module_t *self, GtkWidget *grid, const int row, const int col,
                               const char *label, const int flag)
{
  GtkWidget *check = gtk_check_button_new_with_label(label);
  g_object_set_data(G_OBJECT(check), "guide-flag", GINT_TO_POINTER(flag));
  _connect_refilled(self, check, "toggled", G_CALLBACK(_guide_flag_toggled));
  gtk_grid_attach(GTK_GRID(grid), check, col, row, 1, 1);
  return check;
}

/** A popover's bold heading. */
static GtkWidget *_bold_label(const char *text)
{
  GtkWidget *label = gtk_label_new(NULL);
  gchar *markup = g_markup_printf_escaped("<b>%s</b>", text);
  gtk_label_set_markup(GTK_LABEL(label), markup);
  dt_free(markup);
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  return label;
}

static GtkWidget *_section_label(GtkWidget *grid, const int row, const char *text)
{
  GtkWidget *label = _bold_label(text);
  gtk_widget_set_margin_top(label, DT_PIXEL_APPLY_DPI(row == 0 ? 0 : 8));
  gtk_grid_attach(GTK_GRID(grid), label, 0, row, 4, 1);
  return label;
}

/**
 * The keys the view reads from the main window itself, typed at a control of a toolbar popover that
 * did not take them. They are no shortcut, so the tag does not stop them: once every widget has
 * declined Delete or BackSpace, the view deletes the selected objects behind the popover. Connected
 * after the popover's own handler, which is what hands a key to the focused control first, so a spin
 * button still edits its number with both. Measured, the arrows and Return never get this far: the
 * window's own key bindings take them first, to move the focus and to activate the default.
 */
static gboolean _popover_key_pressed(GtkWidget *popover, GdkEventKey *event, gpointer user_data)
{
  const guint key = event->keyval;
  return key == GDK_KEY_Delete || key == GDK_KEY_KP_Delete || key == GDK_KEY_BackSpace;
}

/**
 * Give the focus to the popover's first control when opening it left the focus outside. A modal
 * popover asks GTK for its first focusable child, and a bauhaus slider answers yes without taking
 * the focus, so a popover opening on one kept none -- the keys then went where they would with no
 * popover at all, a T adding a text frame behind it. Connected after the popover's own show, which
 * is where GTK makes that choice -- measured, a focus given here is still there once the popover is
 * up; a control GTK did focus keeps it.
 */
static void _popover_shown(GtkWidget *popover, gpointer user_data)
{
  GtkWidget *first_control = GTK_WIDGET(user_data);
  GtkWidget *window = gtk_widget_get_toplevel(popover);
  if(!GTK_IS_WINDOW(window)) return;
  GtkWidget *focused = gtk_window_get_focus(GTK_WINDOW(window));
  if(!IS_NULL_PTR(focused) && gtk_widget_is_ancestor(focused, popover)) return;
  gtk_widget_grab_focus(first_control);
}

/**
 * A toolbar popover around its content. The view binds single letters and digits -- T, M, D, 1 to
 * 4 -- and a slider, a toggle or a check box of the popover keeps the focus once clicked, so without
 * the tag every one of those keys typed at a control would act on the canvas behind it. The tag is
 * read from the focus widget up, so it covers every control inside, added now or later -- once the
 * focus IS inside, which is what the first control is for. And it stops shortcuts only; the keys the
 * view reads itself are what the key handler is for.
 * @param first_control the control focused on opening when GTK focused none: pass the first one.
 */
static GtkWidget *_popover_around(GtkWidget *content, GtkWidget *first_control)
{
  GtkWidget *popover = gtk_popover_new(NULL);
  dt_accels_block_plain_keys_inside(popover);
  g_signal_connect_after(popover, "key-press-event", G_CALLBACK(_popover_key_pressed), NULL);
  g_signal_connect_after(popover, "show", G_CALLBACK(_popover_shown), first_control);
  gtk_container_add(GTK_CONTAINER(popover), content);
  gtk_widget_show_all(content);
  return popover;
}

/**
 * A row of a popover laid out as a box: what the control is at the start, the control itself at the
 * end, so a colour button lines up with the right edge of the sliders above it.
 */
static GtkWidget *_labelled_row(GtkWidget *box, const char *label, GtkWidget *widget)
{
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(4));
  GtkWidget *text = gtk_label_new(label);
  gtk_widget_set_halign(text, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(row), text, FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(row), widget, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
  return widget;
}

static GtkWidget *_labelled(GtkWidget *grid, const int row, const int col, const char *label, GtkWidget *widget)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(4));
  gtk_box_pack_start(GTK_BOX(box), gtk_label_new(label), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), widget, FALSE, FALSE, 0);
  gtk_grid_attach(GTK_GRID(grid), box, col, row, 1, 1);
  return widget;
}

/** The guides popover: the grid, the page borders and the paddings, each with its show, snap, size and colour. */
static GtkWidget *_guides_popover(dt_lib_module_t *self)
{
  dt_lib_canvas_toolbar_t *toolbar = (dt_lib_canvas_toolbar_t *)self->data;
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(4));
  gtk_grid_set_column_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(10));
  gtk_container_set_border_width(GTK_CONTAINER(grid), DT_PIXEL_APPLY_DPI(10));

  _section_label(grid, 0, _("Grid"));
  toolbar->grid_show = _guide_check(self, grid, 1, 0, _("Show"), DT_CANVAS_GRID_VISIBLE);
  toolbar->grid_snap = _guide_check(self, grid, 1, 1, _("Snap"), DT_CANVAS_GRID_SNAP);
  toolbar->grid_size = dt_length_field_new("canvas/guides/unit/grid_size", "pt", 0, 5.0, 1000.0, 5.0);
  gtk_widget_set_tooltip_text(toolbar->grid_size, _("Grid spacing. Type a unit -- mm, cm, in -- and it is kept."));
  _connect_refilled(self, toolbar->grid_size, "value-changed", G_CALLBACK(_grid_size_changed));
  _labelled(grid, 1, 2, _("Size"), toolbar->grid_size);
  toolbar->grid_color = _color_button(_("Grid colour"), _("Colour of the grid dots"),
                                      DT_CANVAS_COLOR_GRID, TRUE, self);
  _labelled(grid, 1, 3, _("Colour"), toolbar->grid_color);

  _section_label(grid, 2, _("Page borders"));
  // It belongs beside the page size because it is what the page will be PRINTED at, but it is
  // not a property of the plane: the plane is measured in points and this moves nothing on it.
  toolbar->resolution = gtk_spin_button_new_with_range(18.0, 2400.0, 1.0);
  gtk_widget_set_tooltip_text(toolbar->resolution,
                              _("Dots per inch the page is rasterised at when it is exported. It moves nothing "
                                "on the canvas -- the plane is measured in points, so a size on it is a size on "
                                "the paper -- and only decides how many pixels an export carries."));
  _connect_refilled(self, toolbar->resolution, "value-changed", G_CALLBACK(_resolution_changed));
  _labelled(grid, 2, 2, _("Export DPI"), toolbar->resolution);
  toolbar->page_show = _guide_check(self, grid, 3, 0, _("Show"), DT_CANVAS_PAGE_VISIBLE);
  toolbar->page_snap = _guide_check(self, grid, 3, 1, _("Snap"), DT_CANVAS_SNAP_PAGE);
  toolbar->page_over = _guide_check(self, grid, 4, 0, _("Over"), DT_CANVAS_GUIDES_OVER);
  gtk_widget_set_tooltip_text(toolbar->page_over,
                              _("Draw the page borders, margins and bleed over the content rather than under it, so a "
                                "frame that crosses a page break can still be placed against them"));
  toolbar->page_size = gtk_combo_box_text_new();
  for(int position = 0; position < dt_canvas_paper_count(); position++)
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(toolbar->page_size), dt_canvas_paper_name(position));
  gtk_widget_set_tooltip_text(toolbar->page_size,
                              _("Divide the canvas into pages of this size, one exported page each. One canvas unit is one "
                                "point, so a print size is its size in points and a screen size is its size in pixels at 72 dpi."));
  _connect_refilled(self, toolbar->page_size, "changed", G_CALLBACK(_page_changed));
  _labelled(grid, 3, 2, _("Size"), toolbar->page_size);
  toolbar->page_color = _color_button(_("Page border colour"), _("Colour of the page borders"),
                                      DT_CANVAS_COLOR_TRIM, TRUE, self);
  _labelled(grid, 3, 3, _("Colour"), toolbar->page_color);
  toolbar->page_orientation = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(toolbar->page_orientation), _("Portrait"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(toolbar->page_orientation), _("Landscape"));
  _connect_refilled(self, toolbar->page_orientation, "changed", G_CALLBACK(_page_changed));
  _labelled(grid, 4, 2, _("Orientation"), toolbar->page_orientation);
  /*
   * The one page size the canvas carries itself. Shown by `_refill()` alone, from what the
   * DOCUMENT holds -- the toolbar owns no state, so the combo's own handler never decides this.
   */
  toolbar->custom_width = dt_length_field_new("canvas/guides/unit/custom_paper", "mm", 0, 0.0,
                                              CANVAS_TOOLBAR_CUSTOM_PAPER_MAX, 1.0);
  gtk_widget_set_tooltip_text(toolbar->custom_width, _("How wide a Custom page is. Type a unit -- mm, cm, in."));
  _connect_refilled(self, toolbar->custom_width, "value-changed", G_CALLBACK(_custom_paper_changed));
  _labelled(grid, 5, 2, _("Custom W"), toolbar->custom_width);
  toolbar->custom_height = dt_length_field_new("canvas/guides/unit/custom_paper", "mm", 0, 0.0,
                                               CANVAS_TOOLBAR_CUSTOM_PAPER_MAX, 1.0);
  gtk_widget_set_tooltip_text(toolbar->custom_height, _("How tall a Custom page is. Type a unit -- mm, cm, in."));
  _connect_refilled(self, toolbar->custom_height, "value-changed", G_CALLBACK(_custom_paper_changed));
  _labelled(grid, 5, 3, _("H"), toolbar->custom_height);
  gtk_widget_set_no_show_all(toolbar->custom_width, TRUE);
  gtk_widget_set_no_show_all(toolbar->custom_height, TRUE);

  _section_label(grid, 6, _("Spread"));
  toolbar->spread_cols = gtk_spin_button_new_with_range(0.0, 64.0, 1.0);
  gtk_widget_set_tooltip_text(toolbar->spread_cols,
                              _("Pages across one sheet. A book is 2, a poster taped together as many as it takes. "
                                "0 tiles the plane uniformly, with no fold anywhere."));
  _connect_refilled(self, toolbar->spread_cols, "value-changed", G_CALLBACK(_spread_changed));
  _labelled(grid, 7, 0, _("Across"), toolbar->spread_cols);
  toolbar->spread_rows = gtk_spin_button_new_with_range(0.0, 64.0, 1.0);
  gtk_widget_set_tooltip_text(toolbar->spread_rows, _("Pages down one sheet. 0 tiles the plane uniformly."));
  _connect_refilled(self, toolbar->spread_rows, "value-changed", G_CALLBACK(_spread_changed));
  _labelled(grid, 7, 1, _("Down"), toolbar->spread_rows);
  toolbar->bind_gutter = dt_length_field_new("canvas/guides/unit/bind_gutter", "pt", 0, 0.0, 2000.0, 1.0);
  gtk_widget_set_tooltip_text(toolbar->bind_gutter,
                              _("The binding's own allowance, kept clear inside a page AT A FOLD only -- what a "
                                "perfect binding swallows out of the middle of a picture crossing it. It is added "
                                "to the page margin on those sides, and to no others."));
  _connect_refilled(self, toolbar->bind_gutter, "value-changed", G_CALLBACK(_spread_changed));
  _labelled(grid, 7, 2, _("Bind gutter"), toolbar->bind_gutter);

  _section_label(grid, 8, _("Page margins"));
  toolbar->margin_show = _guide_check(self, grid, 9, 0, _("Show"), DT_CANVAS_MARGIN_VISIBLE);
  toolbar->margin_snap = _guide_check(self, grid, 9, 1, _("Snap"), DT_CANVAS_SNAP_MARGIN);
  toolbar->margin_size = dt_length_field_new("canvas/guides/unit/margin_size", "pt", 0, 0.0, 2000.0, 1.0);
  gtk_widget_set_tooltip_text(toolbar->margin_size,
                              _("Kept clear inside every page edge. A guide and a snapping rule only: nothing is moved and "
                                "the page is unchanged. Type a unit -- mm, cm, in -- and it is kept."));
  _connect_refilled(self, toolbar->margin_size, "value-changed", G_CALLBACK(_page_guides_changed));
  _labelled(grid, 9, 2, _("Size"), toolbar->margin_size);
  toolbar->margin_color = _color_button(_("Margin colour"), _("Colour of the margin lines"),
                                        DT_CANVAS_COLOR_MARGIN, TRUE, self);
  _labelled(grid, 9, 3, _("Colour"), toolbar->margin_color);

  _section_label(grid, 10, _("Bleed"));
  toolbar->bleed_show = _guide_check(self, grid, 11, 0, _("Show"), DT_CANVAS_BLEED_VISIBLE);
  toolbar->bleed_snap = _guide_check(self, grid, 11, 1, _("Snap"), DT_CANVAS_SNAP_BLEED);
  toolbar->bleed_size = dt_length_field_new("canvas/guides/unit/bleed_size", "pt", 0, 0.0, 2000.0, 1.0);
  gtk_widget_set_tooltip_text(toolbar->bleed_size,
                              _("How far past every page edge the sheet keeps going, in canvas units. A frame a page break "
                                "cuts in two carries on into the bleed on both sheets, which is what a binding folds around "
                                "and a trim cuts into. The export writes it."));
  _connect_refilled(self, toolbar->bleed_size, "value-changed", G_CALLBACK(_page_guides_changed));
  _labelled(grid, 11, 2, _("Size"), toolbar->bleed_size);
  toolbar->bleed_color = _color_button(_("Bleed colour"), _("Colour of the bleed lines"),
                                       DT_CANVAS_COLOR_BLEED, TRUE, self);
  _labelled(grid, 11, 3, _("Colour"), toolbar->bleed_color);

  _section_label(grid, 12, _("Paddings"));
  toolbar->padding_show = _guide_check(self, grid, 13, 0, _("Show"), DT_CANVAS_PADDING_VISIBLE);
  gtk_widget_set_tooltip_text(toolbar->padding_show,
                              _("Draw each frame's clear margin around it. Two frames snapped side by side meet on one shared line, two paddings apart."));
  toolbar->padding_snap = _guide_check(self, grid, 13, 1, _("Snap"), DT_CANVAS_SNAP_PADDING);
  toolbar->padding_size = dt_length_field_new("canvas/guides/unit/padding_size", "pt", 0, 0.0, 500.0, 1.0);
  gtk_widget_set_tooltip_text(toolbar->padding_size,
                              _("The clear margin every frame keeps around itself. Side by side, two frames are two of these "
                                "apart. Type a unit -- mm, cm, in -- and it is kept."));
  _connect_refilled(self, toolbar->padding_size, "value-changed", G_CALLBACK(_padding_changed));
  _labelled(grid, 13, 2, _("Size"), toolbar->padding_size);
  toolbar->padding_color = _color_button(_("Padding colour"), _("Colour of the padding frames"),
                                         DT_CANVAS_COLOR_PADDING, TRUE, self);
  _labelled(grid, 13, 3, _("Colour"), toolbar->padding_color);
  toolbar->size_snap = _guide_check(self, grid, 14, 0, _("Snap sizes to neighbours"), DT_CANVAS_SNAP_SIZE);
  gtk_widget_set_hexpand(toolbar->size_snap, TRUE);

  return _popover_around(grid, toolbar->grid_show);
}

/** The shadow popover: the default drop shadow of every object that has no shadow of its own. */
static GtkWidget *_shadow_popover(dt_lib_module_t *self)
{
  dt_lib_canvas_toolbar_t *toolbar = (dt_lib_canvas_toolbar_t *)self->data;
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_PIXEL_APPLY_DPI(4));
  gtk_container_set_border_width(GTK_CONTAINER(box), DT_PIXEL_APPLY_DPI(10));
  gtk_box_pack_start(GTK_BOX(box), _bold_label(_("Shadow")), FALSE, FALSE, 0);
  GtkWidget *first = _prop_slider(self, box, DT_CANVAS_PROP_SHADOW_OFFSET_X, _("Right"),
                                  _("Offset to the right, in canvas units"));
  _prop_slider(self, box, DT_CANVAS_PROP_SHADOW_OFFSET_Y, _("Down"), _("Offset downwards, in canvas units"));
  _prop_slider(self, box, DT_CANVAS_PROP_SHADOW_BLUR, _("Radius"),
               _("Radius, in canvas units: 0 is no shadow, positive drops it outside every object, negative casts it inside along their edges. An object's own properties can override it."));
  toolbar->shadow_color = _color_button(_("Default shadow colour"), _("Colour and strength of the shadow"),
                                        DT_CANVAS_COLOR_SHADOW, TRUE, self);
  _labelled_row(box, _("Colour"), toolbar->shadow_color);
  return _popover_around(box, first);
}

/** The borders popover: the uniform border of every frame that has none of its own. */
static GtkWidget *_borders_popover(dt_lib_module_t *self)
{
  dt_lib_canvas_toolbar_t *toolbar = (dt_lib_canvas_toolbar_t *)self->data;
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_PIXEL_APPLY_DPI(4));
  gtk_container_set_border_width(GTK_CONTAINER(box), DT_PIXEL_APPLY_DPI(10));
  gtk_box_pack_start(GTK_BOX(box), _bold_label(_("Borders")), FALSE, FALSE, 0);
  GtkWidget *first = _prop_slider(self, box, DT_CANVAS_PROP_BORDER_WIDTH, _("Width"),
                                  _("Default border width of the frames, in canvas units"));
  toolbar->border_color = _color_button(_("Default border colour"), _("Default border colour of the frames"),
                                        DT_CANVAS_COLOR_BORDER, TRUE, self);
  _labelled_row(box, _("Colour"), toolbar->border_color);
  _prop_slider(self, box, DT_CANVAS_PROP_CORNER_RADIUS, _("Corners"),
               _("Default radius of the frames' rounded corners, in canvas units; 0 is square"));
  return _popover_around(box, first);
}

/**
 * The line every connector and every free line takes until it is given one of its own.
 *
 * The borders' popover with a line in it: the same `_prop_slider()` reading the same property
 * table, so the toolbar's slider, a connector's own card and the document cannot spell one
 * setting three ways.
 */
static GtkWidget *_lines_popover(dt_lib_module_t *self)
{
  dt_lib_canvas_toolbar_t *toolbar = (dt_lib_canvas_toolbar_t *)self->data;
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_PIXEL_APPLY_DPI(4));
  gtk_container_set_border_width(GTK_CONTAINER(box), DT_PIXEL_APPLY_DPI(10));
  gtk_box_pack_start(GTK_BOX(box), _bold_label(_("Lines")), FALSE, FALSE, 0);
  GtkWidget *first = _prop_slider(self, box, DT_CANVAS_PROP_LINE_WIDTH, _("Width"),
                                  _("Default width of every connector and free line, in canvas units"));
  toolbar->line_color = _color_button(_("Default line colour"),
                                      _("Default colour of every connector and free line"), DT_CANVAS_COLOR_LINE,
                                      TRUE, self);
  _labelled_row(box, _("Colour"), toolbar->line_color);
  return _popover_around(box, first);
}

/**
 * The texture popover: the four degrees of freedom every paper answers to. Contrast weighs
 * the body of the relief, detail its fine structure, scale sizes its features, grain the
 * dither that finishes it; 1 everywhere is the paper as designed.
 */
static GtkWidget *_texture_popover(dt_lib_module_t *self)
{
  dt_lib_canvas_toolbar_t *toolbar = (dt_lib_canvas_toolbar_t *)self->data;
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_PIXEL_APPLY_DPI(4));
  gtk_container_set_border_width(GTK_CONTAINER(box), DT_PIXEL_APPLY_DPI(10));
  gtk_box_pack_start(GTK_BOX(box), _bold_label(_("Paper texture")), FALSE, FALSE, 0);
  // The hard ranges are _proxy_set_texture()'s clamps (views/canvas.c): keep the two together.
  toolbar->texture_contrast = _texture_slider(self, box, _("Contrast"), 0.05f, 8.0f, 0.05f, 4.0f,
                                              _("The relief's body: the mottle, the tooth, the clouds. 1 is the paper as designed."));
  toolbar->texture_detail = _texture_slider(self, box, _("Detail"), 0.0f, 8.0f, 0.0f, 4.0f,
                                            _("The fine structure: fibres, pores, wrinkles, the mesh's imprint. 0 leaves only the body."));
  toolbar->texture_scale = _texture_slider(self, box, _("Scale"), 0.1f, 8.0f, 0.25f, 4.0f,
                                           _("The size of the features: 2 makes them twice as large. Rebuilds the paper."));
  toolbar->texture_grain = _texture_slider(self, box, _("Grain"), 0.0f, 8.0f, 0.0f, 4.0f,
                                           _("The pixel-level grain that finishes the paper, scaled with the zoom"));
  GtkWidget *reset = gtk_button_new_with_label(_("Reset"));
  gtk_widget_set_tooltip_text(reset, _("The paper as designed"));
  gtk_widget_set_halign(reset, GTK_ALIGN_END);
  g_signal_connect(reset, "clicked", G_CALLBACK(_texture_reset), self);
  gtk_box_pack_start(GTK_BOX(box), reset, FALSE, FALSE, 0);
  return _popover_around(box, toolbar->texture_contrast);
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_canvas_toolbar_t *toolbar = g_new0(dt_lib_canvas_toolbar_t, 1);
  toolbar->refilled_handlers = g_array_new(FALSE, FALSE, sizeof(dt_lib_canvas_toolbar_handler_t));
  toolbar->number_live = DT_CANVAS_PROP_NONE;
  self->data = toolbar;

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(4));
  self->widget = box;
  dt_gui_add_class(box, "dt-canvas-toolbar");

  GtkWidget *canvas_menu = gtk_menu_new();
  _action_item(canvas_menu, _("New"), DT_CANVAS_ACTION_NEW);
  _action_item(canvas_menu, _("Open..."), DT_CANVAS_ACTION_OPEN);
  _action_item(canvas_menu, _("Save"), DT_CANVAS_ACTION_SAVE);
  _action_item(canvas_menu, _("Save as..."), DT_CANVAS_ACTION_SAVE_AS);
  gtk_menu_shell_append(GTK_MENU_SHELL(canvas_menu), gtk_separator_menu_item_new());
  _action_item(canvas_menu, _("Export..."), DT_CANVAS_ACTION_EXPORT);
  _menu_button(box, _("Canvas"), _("New, open, save and export the canvas"), canvas_menu);

  GtkWidget *object_menu = gtk_menu_new();
  _action_item(object_menu, _("Check against the library"), DT_CANVAS_ACTION_SYNC_CHECK);
  _action_item(object_menu, _("Refresh the stale images and the notes"), DT_CANVAS_ACTION_SYNC_REFRESH_STALE);
  _action_item(object_menu, _("Refresh every image"), DT_CANVAS_ACTION_SYNC_REFRESH_ALL);
  _menu_button(box, _("Object"), _("Keep the images and notes in step with the library"), object_menu);

  _popover_button(box, _("Guides"), _("The grid, the page borders and the paddings: what shows and what snaps"),
                  _guides_popover(self));
  _separator(box);

  // What an object is made of, and how: a group of buttons that each place one, and a group of
  // toggles that each arm a tool to draw one. Each icon shows what it makes, so a caption reading
  // "Add" over four pictures of what they add would only say it a second time; what a picture
  // cannot say -- the gesture, and the key -- is in the tooltip.
  GtkWidget *add_group = _linked_group(box);
  _icon_button(add_group, dtgtk_cairo_paint_text_label, CPF_NONE,
               _("Add a text frame at the centre of the view"), DT_CANVAS_ACTION_ADD_TEXT);
  _icon_button(add_group, dtgtk_cairo_paint_note, CPF_NONE,
               _("Add the .txt notes of the selected images as text frames (of every image when none is selected)"),
               DT_CANVAS_ACTION_ADD_NOTES);
  _icon_button(add_group, dtgtk_cairo_paint_map_marker, CPF_NONE,
               _("Add a map frame at the centre of the view; an image's context menu adds a map of where it was taken"),
               DT_CANVAS_ACTION_ADD_MAP);
  _icon_button(add_group, dtgtk_cairo_paint_drawing_svg, CPF_NONE,
               _("Place a drawing read from an SVG file, at the size the file states. Its own bytes travel in the "
                 "canvas, so the document carries the drawing; its context menu reads the file again when it has "
                 "been edited since."),
               DT_CANVAS_ACTION_ADD_SVG);
  _separator(box);

  GtkWidget *draw_group = _linked_group(box);
  _tool_toggle(self, draw_group, dtgtk_cairo_paint_route, CPF_ROUTE_CUBIC | CPF_ROUTE_FRAMES,
               _("Draw a connector: click an anchor point on one frame, then on another. The tool "
                 "stays armed for the next connector; Escape or a right click leaves it"),
               DT_CANVAS_ACTION_CONNECT_MODE);
  _tool_toggle(self, draw_group, dtgtk_cairo_paint_route, CPF_ROUTE_STRAIGHT | CPF_ROUTE_FREE,
               _("Draw a line between two points of the plane: drag from one end to the other, or click to "
                 "place one. Ctrl holds it to 45 degrees, Shift to 15. It takes the width, colour, dashes and "
                 "arrowheads of the last line; the tool stays armed until Escape or a right click"),
               DT_CANVAS_ACTION_DRAW_LINE);
  _tool_toggle(self, draw_group, dtgtk_cairo_paint_route, CPF_ROUTE_CUBIC | CPF_ROUTE_FREE,
               _("Draw a curve between two points of the plane: the same gesture as a line, bent into an arc "
                 "over the chord the drag gives"),
               DT_CANVAS_ACTION_DRAW_CURVE);
  _tool_toggle(self, draw_group, dtgtk_cairo_paint_shape, CPF_SHAPE_RECTANGLE,
               _("Draw a rectangle: drag its box, or click to place one. Ctrl for a square, Shift from the "
                 "centre. It takes the fill, border and corners of the last shape; the tool stays armed until "
                 "Escape or a right click"),
               DT_CANVAS_ACTION_DRAW_RECTANGLE);
  _tool_toggle(self, draw_group, dtgtk_cairo_paint_shape, CPF_SHAPE_POLYGON,
               _("Draw a regular polygon: drag its box, or click to place one; Shift from the centre. It takes "
                 "the sides and corner rounding of the last polygon, and the fill, border and corners of the "
                 "last shape"),
               DT_CANVAS_ACTION_DRAW_POLYGON);
  _tool_toggle(self, draw_group, dtgtk_cairo_paint_shape, CPF_SHAPE_STAR,
               _("Draw a star: drag its box, or click to place one; Shift from the centre. It takes the points, "
                 "notch depth and corner rounding of the last star, and the fill, border and corners of the "
                 "last shape"),
               DT_CANVAS_ACTION_DRAW_STAR);
  _separator(box);

  gtk_box_pack_start(GTK_BOX(box), gtk_label_new(_("Background")), FALSE, FALSE, DT_PIXEL_APPLY_DPI(4));
  toolbar->background_style = gtk_combo_box_text_new();
  for(int position = 0; position < dt_canvas_background_count(); position++)
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(toolbar->background_style), dt_canvas_background_name(position));
  gtk_widget_set_tooltip_text(toolbar->background_style,
                              _("What the canvas is painted with. Transparent leaves it a hole, shown here as a chequerboard "
                                "and carried out by any export format with an alpha channel."));
  _connect_refilled(self, toolbar->background_style, "changed", G_CALLBACK(_background_changed));
  gtk_box_pack_start(GTK_BOX(box), toolbar->background_style, FALSE, FALSE, 0);
  toolbar->background_color = _color_button(_("Background colour"),
                                            _("Background colour: the plain colour, or the paper's own"),
                                            DT_CANVAS_COLOR_BACKGROUND, FALSE, self);
  gtk_box_pack_start(GTK_BOX(box), toolbar->background_color, FALSE, FALSE, 0);
  _popover_button(box, _("Texture"), _("The paper's relief, detail, scale and grain"), _texture_popover(self));
  _separator(box);

  // No caption over these two: what each opens is named on the button itself, and "Frames" over
  // "Borders..." and "Shadows..." was a heading for a list of two that already read as one.
  _popover_button(box, _("Borders"), _("The uniform border of every frame without one of its own"), _borders_popover(self));
  _popover_button(box, _("Shadows"), _("The default shadow of every object without one of its own"), _shadow_popover(self));
  _popover_button(box, _("Lines"), _("The line every connector and free line takes without one of its own"),
                  _lines_popover(self));
  _separator(box);

  // No caption over these two, and no glyph either. The rule the groups above are built on is that
  // an icon shows what it makes; the one picture the toolkit has for zooming is a magnifying glass,
  // which says "zoom" over an action that means "fit", beside a "1:1" that is also a zoom -- and
  // the caption that used to tell the pair apart is what this step removes. Two words say it.
  _button(box, _("Fit"), _("Fit the view to the canvas"), DT_CANVAS_ACTION_ZOOM_FIT);
  _button(box, _("1:1"), _("Zoom to 100%"), DT_CANVAS_ACTION_ZOOM_100);
  _separator(box);

  gtk_box_pack_start(GTK_BOX(box), gtk_label_new(_("Arrange")), FALSE, FALSE, DT_PIXEL_APPLY_DPI(4));
  toolbar->layout = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(toolbar->layout), _("Square grid"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(toolbar->layout), _("Masonry"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(toolbar->layout), _("Row"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(toolbar->layout), _("Column"));
  gtk_combo_box_set_active(GTK_COMBO_BOX(toolbar->layout), 0);
  gtk_widget_set_tooltip_text(toolbar->layout, _("How to arrange the selected frames, or all of them"));
  gtk_box_pack_start(GTK_BOX(box), toolbar->layout, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), gtk_label_new(_("Sort by")), FALSE, FALSE, DT_PIXEL_APPLY_DPI(4));
  toolbar->sort = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(toolbar->sort), _("canvas order"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(toolbar->sort), _("filename"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(toolbar->sort), _("captured"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(toolbar->sort), _("id"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(toolbar->sort), _("full path"));
  gtk_combo_box_set_active(GTK_COMBO_BOX(toolbar->sort), CLAMP(dt_conf_get_int("canvas/layout_sort"), 0, DT_CANVAS_SORT_LAST - 1));
  gtk_widget_set_tooltip_text(toolbar->sort,
                              _("The order the frames are arranged in: the canvas's own, or a key of the images as in the lighttable; frames that are not images follow"));
  g_signal_connect(toolbar->sort, "changed", G_CALLBACK(_sort_changed), self);
  gtk_box_pack_start(GTK_BOX(box), toolbar->sort, FALSE, FALSE, 0);
  GtkWidget *arrange = gtk_button_new_with_label(_("Auto"));
  gtk_widget_set_tooltip_text(arrange, _("Arrange the frames in the chosen layout and order"));
  g_signal_connect(arrange, "clicked", G_CALLBACK(_layout_apply), self);
  gtk_box_pack_start(GTK_BOX(box), arrange, FALSE, FALSE, 0);

  gtk_widget_show_all(box);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_CANVAS_CHANGED,
                                  G_CALLBACK(_canvas_changed), self);
  _refill(self);
}

/**
 * A session still open when the atelier is left is CLOSED here, not dropped. Its LIVE steps are
 * already written into the document, so dropping it would leave the number the user landed on in
 * place with no undo step recording it. The view's own `leave()` runs first and leaves both the
 * document and `proxy.canvas.view` alone -- only a new document resets the view's half of the
 * session -- so the commit still lands on the document the gesture belonged to.
 */
void view_leave(dt_lib_module_t *self, dt_view_t *old_view, dt_view_t *new_view)
{
  dt_lib_canvas_toolbar_t *toolbar = (dt_lib_canvas_toolbar_t *)self->data;
  if(IS_NULL_PTR(toolbar)) return;
  // A button cannot still be down once the popover holding it has gone with the view.
  for(int idx = 0; idx < toolbar->number_count; idx++) toolbar->numbers[idx].pressed = FALSE;
  _number_commit_live(toolbar);
}

void view_enter(dt_lib_module_t *self, dt_view_t *old_view, dt_view_t *new_view)
{
  _refill(self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(dt_control_signal_get_global(), G_CALLBACK(_canvas_changed), self);
  dt_lib_canvas_toolbar_t *toolbar = (dt_lib_canvas_toolbar_t *)self->data;
  if(!IS_NULL_PTR(toolbar))
  {
    // The timer holds the toolbar it would commit through: it must not outlive it.
    _number_debounce_remove(toolbar);
    g_array_free(toolbar->refilled_handlers, TRUE);
  }
  dt_free(self->data);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
