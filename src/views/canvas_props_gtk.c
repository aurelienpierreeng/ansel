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

#include "views/canvas_props_gtk.h"

#include "canvas/canvas.h"            // dt_canvas_object_t, dt_canvas_color(), the text feature helpers
#include "canvas/canvas_actions.h"    // DT_CANVAS_COLOR_HISTORY_KEY
#include "system/macros.h"            // IS_NULL_PTR
#include "system/mem_alloc.h"         // dt_free
#include "widgets/accelerators.h"     // dt_accels_block_plain_keys_inside
#include "widgets/bauhaus.h"          // the sliders and the combobox
#include "widgets/button.h"           // dtgtk_button_new
#include "widgets/chooser_button.h"   // the colour and font buttons
#include "widgets/collapsible_section.h"
#include "widgets/container.h"        // dt_gui_flow_box_as_layout
#include "widgets/paint.h"            // the glyphs
#include "widgets/togglebutton.h"     // dtgtk_togglebutton_new
#include "widgets/widget_settings.h"  // DT_PIXEL_APPLY_DPI, dt_gui_widget_freeze
#include "widgets/widget_style.h"     // dt_gui_add_class, dt_capitalize_label

#include <glib/gi18n.h>
#include <math.h>
#include <string.h>

/** The narrowest the properties are, strip and card alike: what a card's rows are designed to fit. */
#define PROPS_MIN_WIDTH_PIXELS 340
/** How long a keyboard or wheel edit waits for the next step before it is one undo step. */
#define PROPS_DEBOUNCE_MS 400
/** The width of a card row's label, in characters: every row's control starts at the same place. */
#define PROPS_LABEL_CHARS 9
/** The font button's label: long names ellipsise inside it rather than widening the strip. */
#define PROPS_FONT_CHARS 14
/** The strip's description of a picture or a drawing, ellipsised in the middle to keep the extension. */
#define PROPS_INFO_CHARS 30
/** The gap between the strip's controls. */
#define PROPS_STRIP_SPACING_PIXELS 4
/** The kind glyph opening the strip. */
#define PROPS_GLYPH_PIXELS 16
/** The most choices a row of glyphs offers: a cutout's five shapes are the most any property has. */
#define PROPS_ICONS_MAX 8
/** OpenType features side by side: three check buttons of a long name still fit the narrowest card. */
#define PROPS_FEATURES_PER_LINE 3
/** Where a feature check keeps its tag, and the id of its handler. */
#define PROPS_FEATURE_TAG_KEY "dt-canvas-props-feature"
#define PROPS_FEATURE_HANDLER_KEY "dt-canvas-props-feature-handler"

/* --- glyphs ------------------------------------------------------------------------------- */

/** A glyph the table names by id, as a paint function and its variant. */
typedef struct props_glyph_t
{
  const char *id;
  DTGTKCairoPaintIconFunc paint;
  gint flags;
} props_glyph_t;

static const props_glyph_t _glyphs[] = {
  { "text_align_left", dtgtk_cairo_paint_text_align, CPF_TEXT_ALIGN_LEFT },
  { "text_align_center", dtgtk_cairo_paint_text_align, CPF_TEXT_ALIGN_CENTER },
  { "text_align_right", dtgtk_cairo_paint_text_align, CPF_TEXT_ALIGN_RIGHT },
  { "text_align_justify", dtgtk_cairo_paint_text_align, CPF_TEXT_ALIGN_JUSTIFY },
  { "text_valign_top", dtgtk_cairo_paint_text_valign, CPF_TEXT_VALIGN_TOP },
  { "text_valign_middle", dtgtk_cairo_paint_text_valign, CPF_TEXT_VALIGN_MIDDLE },
  { "text_valign_bottom", dtgtk_cairo_paint_text_valign, CPF_TEXT_VALIGN_BOTTOM },
  { "route_straight", dtgtk_cairo_paint_route, CPF_ROUTE_STRAIGHT },
  { "route_square", dtgtk_cairo_paint_route, CPF_ROUTE_SQUARE },
  { "route_cubic", dtgtk_cairo_paint_route, CPF_ROUTE_CUBIC },
  { "arrowhead_start", dtgtk_cairo_paint_arrowhead, CPF_ARROWHEAD_START },
  { "arrowhead_end", dtgtk_cairo_paint_arrowhead, CPF_ARROWHEAD_END },
  { "waypoint", dtgtk_cairo_paint_waypoint, CPF_NONE },
  { "reverse", dtgtk_cairo_paint_reverse, CPF_NONE },
  { "refresh", dtgtk_cairo_paint_refresh, CPF_NONE },
  { "link", dtgtk_cairo_paint_link, CPF_NONE },
  { "cancel", dtgtk_cairo_paint_cancel, CPF_NONE },
  { "masks_circle", dtgtk_cairo_paint_masks_circle, CPF_NONE },
  { "masks_ellipse", dtgtk_cairo_paint_masks_ellipse, CPF_NONE },
  { "masks_polygon", dtgtk_cairo_paint_masks_polygon, CPF_NONE },
  { "masks_gradient", dtgtk_cairo_paint_masks_gradient, CPF_NONE },
  { "masks_inverse", dtgtk_cairo_paint_masks_inverse, CPF_NONE },
  { "masks_edit", dtgtk_cairo_paint_masks_edit, CPF_NONE },
};

/** The glyph for an id; a text label for one this frontend does not know, so a new id shows as something. */
static const props_glyph_t *_glyph(const char *glyph_id)
{
  static const props_glyph_t unknown = { NULL, dtgtk_cairo_paint_text_label, CPF_NONE };
  if(IS_NULL_PTR(glyph_id)) return &unknown;
  for(size_t idx = 0; idx < G_N_ELEMENTS(_glyphs); idx++)
  {
    if(strcmp(_glyphs[idx].id, glyph_id) == 0) return &_glyphs[idx];
  }
  return &unknown;
}

/* --- the widget's own state ----------------------------------------------------------------- */

/** One property's control. Indexed by property id: `bindings[id]`. */
typedef struct props_binding_t
{
  struct dt_canvas_props_gtk_t *owner;
  const dt_canvas_prop_t *prop;
  GtkWidget *widget;           ///< the control: a slider, a spin, a linked box of toggles, a chooser, a label...
  GtkWidget *row;              ///< what is shown and hidden for it; shared by the two halves of a pair
  GtkWidget *toggles[PROPS_ICONS_MAX]; ///< ICONS: one toggle per choice
  gulong toggle_handlers[PROPS_ICONS_MAX];
  int toggle_count;
  gulong handler;              ///< the control's value handler, blocked while a refill writes
  gboolean in_strip;
  gboolean pressed;            ///< a button is held on the control
  gboolean stale;              ///< that button went down for an object no longer shown: nothing it does is reported
  gboolean double_clicked;     ///< the button held is the second of a double click
  gboolean typing;             ///< digits typed into a spin button and not applied yet
  double rest_number;          ///< TUNE, MEASURE: the value last shown to or reported by the control
  double press_number;         ///< TUNE, MEASURE: the value the control held once the button went down
} props_binding_t;

typedef struct props_section_t
{
  struct dt_canvas_props_gtk_t *owner;
  dt_canvas_prop_section_t section;
  dt_gui_collapsible_section_t collapsible;
  GtkWidget *summary;
  GtkWidget *own;              ///< the group's switch, beside the header's fold toggle
  gulong own_handler;
  gulong toggle_handler;
  GtkWidget *essential_box;
  GtkWidget *more_box;         ///< the rows under the dotted rule
  gboolean present;            ///< one of this section's own card rows applies to the object shown
} props_section_t;

struct dt_canvas_props_gtk_t
{
  dt_canvas_props_host_t host;
  GtkWidget *root;             ///< the event box handed out, referenced so a destroyed overlay leaves it freeable
  GtkWidget *frame;            ///< the framed box inside it: a GtkEventBox counts no CSS border
  GtkWidget *strip;
  GtkWidget *strip_strut;      ///< a column as tall as the tallest control any kind's strip can hold
  GtkWidget *kind_glyph;
  GtkWidget *content_button;
  GtkWidget *card_toggle;
  GtkWidget *close_button;
  GtkWidget *card;             ///< the scrolled window
  GtkWidget *sections_box;
  gulong card_toggle_handler;
  props_binding_t bindings[DT_CANVAS_PROP_COUNT];
  props_section_t sections[DT_CANVAS_SECTION_COUNT];

  /* the pair being built: the first half waits for its partner */
  GtkWidget *pair_grid;
  dt_canvas_prop_id_t pair_first;

  /* the target, as the last refill saw it */
  gboolean has_target;
  uint32_t object_id;
  uint32_t kind;
  uint8_t applies[DT_CANVAS_PROP_COUNT];
  gboolean structure_known;
  char face[DT_CANVAS_PROP_TEXT_LEN];     ///< the face the features were listed for
  char features[DT_CANVAS_PROP_TEXT_LEN]; ///< the feature string the checks show
  gboolean inset_linked;                  ///< one inset for four sides: derived when the target changes

  /* the gesture in flight */
  dt_canvas_prop_id_t live;    ///< the property a LIVE session is open on, NONE otherwise
  gboolean live_group_inherited; ///< the live property's group was the canvas's when the session opened
  guint commit_source;         ///< closes a session nothing holds: a burst of steps, or a click awaiting its second
  int open_section;            ///< the accordion's open section, -1 for none
  gboolean group_inherits[DT_CANVAS_GROUP_COUNT]; ///< as the last refill read them
  gboolean pointer_reported;   ///< what the host was last told of the pointer

  /* the card */
  gboolean grow_up;
  int strip_strut_height;      ///< 0 until measured; the theme changing measures again
  int card_max_content;        ///< the max-content-height last set, -1 for none

  GtkWidget *features_empty;   ///< "this font offers nothing" in the features row
  gboolean closing;            ///< nothing reaches the host or the controls any more
  gboolean closing_dialogs;    ///< the dialogs are being closed for the host, which hears nothing of it
  gboolean destroyed;          ///< the root and every control are gone, destroyed by whoever held them
};

typedef struct dt_canvas_props_gtk_t props_t;

/* --- talking to the host ----------------------------------------------------------------------- */

static void _host_edit(props_binding_t *binding, const dt_canvas_prop_value_t *value,
                       const dt_canvas_edit_phase_t phase)
{
  props_t *props = binding->owner;
  if(props->closing || !props->has_target || IS_NULL_PTR(props->host.edit)) return;
  props->host.edit(props->host.data, binding->prop->id, value, phase);
}

/** Two numbers read the same on a control showing `digits` decimals. */
static gboolean _shown_equal(const dt_canvas_prop_t *prop, const double first, const double second)
{
  const double quantum = pow(10.0, -(double)MAX(prop->digits, 0));
  return fabs(first - second) < quantum * 0.5;
}

/** How long the toolkit waits for the second click of a double click. */
static guint _double_click_ms(GtkWidget *widget)
{
  gint delay_ms = 400;
  g_object_get(gtk_widget_get_settings(widget), "gtk-double-click-time", &delay_ms, NULL);
  return (guint)MAX(delay_ms, 1);
}

/** What a control shows right now, as a value of its property. */
static void _binding_value(const props_binding_t *binding, dt_canvas_prop_value_t *value)
{
  memset(value, 0, sizeof(*value));
  if(IS_NULL_PTR(binding->widget)) return;
  switch(binding->prop->widget)
  {
    case DT_CANVAS_WIDGET_TUNE:
      value->number = dt_bauhaus_slider_get(binding->widget);
      break;
    case DT_CANVAS_WIDGET_CHOICE:
      value->choice = dt_bauhaus_combobox_get(binding->widget);
      break;
    case DT_CANVAS_WIDGET_MEASURE:
      value->number = gtk_spin_button_get_value(GTK_SPIN_BUTTON(binding->widget));
      break;
    case DT_CANVAS_WIDGET_ICONS:
      for(int choice = 0; choice < binding->toggle_count; choice++)
      {
        if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(binding->toggles[choice]))) value->choice = choice;
      }
      break;
    case DT_CANVAS_WIDGET_ICON_FLAG:
    case DT_CANVAS_WIDGET_FLAG:
      value->flag = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(binding->widget));
      break;
    case DT_CANVAS_WIDGET_COLOR:
    {
      GdkRGBA rgba;
      dt_chooser_button_get_color(binding->widget, &rgba);
      value->color = dt_canvas_color((float)rgba.red, (float)rgba.green, (float)rgba.blue, (float)rgba.alpha);
      break;
    }
    case DT_CANVAS_WIDGET_FONT:
      g_strlcpy(value->text, dt_chooser_button_get_font(binding->widget), sizeof(value->text));
      break;
    case DT_CANVAS_WIDGET_FEATURES:
      g_strlcpy(value->text, binding->owner->features, sizeof(value->text));
      break;
    default:
      break;
  }
}

static void _debounce_remove(props_t *props)
{
  if(props->commit_source == 0) return;
  g_source_remove(props->commit_source);
  props->commit_source = 0;
}

/**
 * End the LIVE session, if one is open, with the control's final value. The session is closed
 * BEFORE the host hears of it, so a refill the host runs from the commit writes the control back
 * to what the document settled on.
 */
static void _commit_live(props_t *props)
{
  _debounce_remove(props);
  if(props->live == DT_CANVAS_PROP_NONE) return;
  props_binding_t *binding = &props->bindings[props->live];
  props->live = DT_CANVAS_PROP_NONE;
  dt_canvas_prop_value_t value;
  _binding_value(binding, &value);
  _host_edit(binding, &value, DT_CANVAS_EDIT_COMMIT);
}

/**
 * A control moving: one step of a LIVE session, which ends any other control's session first.
 * Whatever timer an earlier step left is removed, and only a step nothing holds arms one again,
 * after this: a button held on the control continues the session, and a timer left by an arrow key
 * pressed a moment before must not commit it half-way through the drag.
 */
static void _edit_live(props_binding_t *binding, const dt_canvas_prop_value_t *value)
{
  props_t *props = binding->owner;
  if(props->live != DT_CANVAS_PROP_NONE && props->live != binding->prop->id) _commit_live(props);
  _debounce_remove(props);
  if(props->live != binding->prop->id)
  {
    props->live = binding->prop->id;
    const dt_canvas_prop_group_t group = binding->prop->group;
    props->live_group_inherited = group != DT_CANVAS_GROUP_NONE && props->group_inherits[group];
  }
  _host_edit(binding, value, DT_CANVAS_EDIT_LIVE);
}

static gboolean _debounce_fired(gpointer user_data)
{
  props_t *props = (props_t *)user_data;
  props->commit_source = 0;
  _commit_live(props);
  return G_SOURCE_REMOVE;
}

/** Close the session later, unless a step or a press comes first. */
static void _commit_later(props_t *props, const guint delay_ms)
{
  _debounce_remove(props);
  props->commit_source = g_timeout_add(delay_ms, _debounce_fired, props);
}

/**
 * Forget the gesture in flight without reporting it. For a new target: the host has already
 * committed what it had pending before it asked for the refill, and a commit sent now would carry
 * the previous object's value to the next one.
 *
 * A button still held on a control is not let go of here, though, since the control still holds
 * the pointer and goes on moving with it: forgotten outright, its remaining motions would read as
 * steps nothing holds, and be sent -- and committed -- to the next object. The drag is marked stale
 * instead, reports nothing until its button comes up, and the control then goes back to the value
 * the refill gave it.
 */
static void _drop_live(props_t *props)
{
  _debounce_remove(props);
  props->live = DT_CANVAS_PROP_NONE;
  for(int prop_id = DT_CANVAS_PROP_NONE + 1; prop_id < DT_CANVAS_PROP_COUNT; prop_id++)
  {
    props_binding_t *binding = &props->bindings[prop_id];
    if(binding->pressed) binding->stale = TRUE;
    binding->typing = FALSE;
  }
}

/**
 * A step with no button held -- an arrow key, the wheel, a slider's fine-tuning popup. Nothing
 * says when such a gesture ends, so it ends when the steps stop coming: a burst of them is one
 * undo step, and the renders and the configuration are paid once, after it.
 */
static void _edit_live_debounced(props_binding_t *binding, const dt_canvas_prop_value_t *value)
{
  _edit_live(binding, value);
  _commit_later(binding->owner, PROPS_DEBOUNCE_MS);
}

/**
 * Abandon the LIVE session open on this control, if it is: the host puts the document back as the
 * session found it and records nothing. A session already committed -- the host ends every gesture
 * pending before an undo, a close or a new object -- is left alone: what it wrote stands as recorded.
 */
static void _cancel_live(props_binding_t *binding, const dt_canvas_prop_value_t *value)
{
  props_t *props = binding->owner;
  if(props->live != binding->prop->id) return;
  _debounce_remove(props);
  props->live = DT_CANVAS_PROP_NONE;
  _host_edit(binding, value, DT_CANVAS_EDIT_CANCEL);
}

/** A whole gesture at once: a click, a pick, a typed number. */
static void _edit_once(props_binding_t *binding, const dt_canvas_prop_value_t *value)
{
  _commit_live(binding->owner);
  _host_edit(binding, value, DT_CANVAS_EDIT_ONCE);
}

/**
 * The control is being handled by the user, and a refill must leave what it shows alone. A stale
 * drag is not: what it shows belongs to an object no longer shown.
 */
static gboolean _binding_busy(const props_binding_t *binding)
{
  return (binding->pressed && !binding->stale) || binding->typing || binding->owner->live == binding->prop->id;
}

/** Put back the value the control last showed for the object on screen, reporting nothing. */
static void _restore_rest(props_binding_t *binding)
{
  GtkWidget *widget = binding->widget;
  if(binding->prop->widget == DT_CANVAS_WIDGET_TUNE)
  {
    dt_gui_widget_freeze();
    g_signal_handler_block(widget, binding->handler);
    dt_bauhaus_slider_set(widget, (float)binding->rest_number);
    g_signal_handler_unblock(widget, binding->handler);
  }
  else if(binding->prop->widget == DT_CANVAS_WIDGET_MEASURE)
  {
    g_signal_handler_block(widget, binding->handler);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), binding->rest_number);
    g_signal_handler_unblock(widget, binding->handler);
  }
}

/* --- handlers ------------------------------------------------------------------------------------ */

static void _slider_changed(GtkWidget *widget, gpointer user_data)
{
  props_binding_t *binding = (props_binding_t *)user_data;
  if(binding->owner->closing || binding->stale) return;
  dt_canvas_prop_value_t value;
  _binding_value(binding, &value);
  // A slider announces its value on every release and every step, moved or not: one it already
  // showed is no edit, and would record an undo step for nothing.
  if(_shown_equal(binding->prop, value.number, binding->rest_number)) return;
  binding->rest_number = value.number;
  if(binding->pressed)
    _edit_live(binding, &value);
  else
    _edit_live_debounced(binding, &value);
}

/**
 * A double click reset a slider inside an override group, in the slider's own handler, already
 * reported as a step. Written while the object inherits, that value changes nothing and the object
 * goes on inheriting -- but the first click of the double click came first, and gave the object
 * values of its own wherever it landed. When the session was opened while the group was the
 * canvas's, the reset therefore gives it back, inside the same gesture: what a double click on a
 * slider means is "put it back as it was", and as it was is inheriting.
 */
static void _double_click_reset(props_binding_t *binding)
{
  props_t *props = binding->owner;
  const dt_canvas_prop_group_t group = binding->prop->group;
  if(binding->prop->widget != DT_CANVAS_WIDGET_TUNE || group == DT_CANVAS_GROUP_NONE) return;
  if(props->live != binding->prop->id || !props->live_group_inherited) return;
  if(!props->has_target || IS_NULL_PTR(props->host.group_own)) return;
  props->host.group_own(props->host.data, group, FALSE);
}

/**
 * A button pressed on or released from a slider or a spin button. Read AFTER the control handled
 * the event: a slider moves to a click on its bar in its own press handler, which then stops the
 * press from reaching any other handler, resets itself on a double click in the same handler, and
 * emits its last value from its own release handler. `event-after` is emitted whatever those
 * returned, and once they have run.
 */
static void _control_event_after(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  props_binding_t *binding = (props_binding_t *)user_data;
  props_t *props = binding->owner;
  if(props->closing) return;
  if(event->type == GDK_BUTTON_PRESS && event->button.button == 1)
  {
    binding->pressed = TRUE;
    binding->stale = FALSE;
    binding->double_clicked = FALSE;
    // A held button is a gesture of its own. On the control a session is already open on, it
    // continues that session, and the timer a key step or a first click left must not commit it
    // under the button; a session open on any other control ends here, as its focus leaving would.
    if(props->live == binding->prop->id)
      _debounce_remove(props);
    else
      _commit_live(props);
    dt_canvas_prop_value_t value;
    _binding_value(binding, &value);
    binding->press_number = value.number;
    // A slider moves to a click on its bar without announcing it: the session starts here, so the
    // host holds the properties still and the canvas follows the click without waiting for a drag.
    // A press that moved nothing -- on the label, beside the bar, on the link -- is no edit at all.
    if(binding->prop->widget == DT_CANVAS_WIDGET_TUNE
       && !_shown_equal(binding->prop, value.number, binding->rest_number))
    {
      binding->rest_number = value.number;
      _edit_live(binding, &value);
    }
  }
  else if(event->type == GDK_2BUTTON_PRESS && event->button.button == 1)
  {
    binding->double_clicked = TRUE;
    if(!binding->stale) _double_click_reset(binding);
  }
  else if(event->type == GDK_BUTTON_RELEASE && event->button.button == 1 && binding->pressed)
  {
    binding->pressed = FALSE;
    if(binding->stale)
    {
      binding->stale = FALSE;
      _restore_rest(binding);
      return;
    }
    if(props->live != binding->prop->id) return;
    dt_canvas_prop_value_t value;
    _binding_value(binding, &value);
    // A click that did not drag may be the first half of a double click, whose reset belongs to
    // the same gesture: it is committed once no second click came. A drag, or the double click
    // itself, is committed as its button comes up.
    const gboolean clicked = binding->prop->widget == DT_CANVAS_WIDGET_TUNE && !binding->double_clicked
                             && _shown_equal(binding->prop, value.number, binding->press_number);
    if(clicked)
      _commit_later(props, _double_click_ms(widget));
    else
      _commit_live(props);
  }
}

/** Leaving a control ends whatever was in flight on it: a drag cut short, digits never applied. */
static gboolean _control_focus_out(GtkWidget *widget, GdkEventFocus *event, gpointer user_data)
{
  props_binding_t *binding = (props_binding_t *)user_data;
  props_t *props = binding->owner;
  if(props->closing) return FALSE;
  binding->pressed = FALSE;
  binding->typing = FALSE;
  if(binding->stale)
  {
    binding->stale = FALSE;
    _restore_rest(binding);
  }
  if(props->live == binding->prop->id) _commit_live(props);
  return FALSE;
}

/**
 * A combobox changes one notch of the wheel, one arrow key or one pick from its list at a time,
 * and nothing tells those apart: every change is a step, and a burst of them is one undo step --
 * for a map style, one fetch of its tiles and one write of the next map's defaults, not one per notch.
 */
static void _combobox_changed(GtkWidget *widget, gpointer user_data)
{
  props_binding_t *binding = (props_binding_t *)user_data;
  if(binding->owner->closing) return;
  dt_canvas_prop_value_t value;
  _binding_value(binding, &value);
  _edit_live_debounced(binding, &value);
}

/**
 * A key typed into a spin button: nothing is applied until Return or the focus leaves, and until
 * then a refill must not write over the digits. Read on the key rather than on the text changing,
 * since the spin button rewrites its own text every time it formats a value.
 */
static gboolean _spin_key_pressed(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
  props_binding_t *binding = (props_binding_t *)user_data;
  const guint shortcut_modifiers = GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_SUPER_MASK | GDK_META_MASK;
  const gboolean shortcut = (event->state & shortcut_modifiers) != 0;
  const guint32 character = gdk_keyval_to_unicode(event->keyval);
  const gboolean printable = character >= 0x20 && character != 0x7f;
  const gboolean deletion = event->keyval == GDK_KEY_BackSpace || event->keyval == GDK_KEY_Delete
                            || event->keyval == GDK_KEY_KP_Delete;
  if((printable || deletion) && !shortcut) binding->typing = TRUE;
  return FALSE;
}

static void _spin_changed(GtkSpinButton *spin, gpointer user_data)
{
  props_binding_t *binding = (props_binding_t *)user_data;
  if(binding->owner->closing || binding->stale) return;
  dt_canvas_prop_value_t value;
  _binding_value(binding, &value);
  binding->rest_number = value.number;
  if(binding->typing)
  {
    binding->typing = FALSE;
    _edit_once(binding, &value);
  }
  else if(binding->pressed)
  {
    _edit_live(binding, &value);
  }
  else
  {
    _edit_live_debounced(binding, &value);
  }
}

/** Return on digits that parse to the value already there applies nothing, and holds nothing either. */
static void _spin_activated(GtkEntry *entry, gpointer user_data)
{
  props_binding_t *binding = (props_binding_t *)user_data;
  binding->typing = FALSE;
}

static void _icon_toggled(GtkToggleButton *toggle, gpointer user_data)
{
  props_binding_t *binding = (props_binding_t *)user_data;
  if(binding->owner->closing) return;
  const int picked = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(toggle), "dt-canvas-props-choice"));
  if(!gtk_toggle_button_get_active(toggle))
  {
    // One of the choices is always the one: a click on the pressed button keeps it pressed.
    g_signal_handler_block(toggle, binding->toggle_handlers[picked]);
    gtk_toggle_button_set_active(toggle, TRUE);
    g_signal_handler_unblock(toggle, binding->toggle_handlers[picked]);
    return;
  }
  for(int choice = 0; choice < binding->toggle_count; choice++)
  {
    if(choice == picked) continue;
    g_signal_handler_block(binding->toggles[choice], binding->toggle_handlers[choice]);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(binding->toggles[choice]), FALSE);
    g_signal_handler_unblock(binding->toggles[choice], binding->toggle_handlers[choice]);
  }
  dt_canvas_prop_value_t value;
  memset(&value, 0, sizeof(value));
  value.choice = picked;
  _edit_once(binding, &value);
}

static void _flag_toggled(GtkToggleButton *toggle, gpointer user_data)
{
  props_binding_t *binding = (props_binding_t *)user_data;
  if(binding->owner->closing) return;
  dt_canvas_prop_value_t value;
  _binding_value(binding, &value);
  _edit_once(binding, &value);
}

/**
 * The colour window: one opening is one gesture. Its changes are the steps of a LIVE session, so the
 * canvas shows the colour while it is dragged and the document is snapshot once; its closing keeping a
 * colour commits the session, one undo step however many drags it held; and its closing giving the
 * colour back cancels it, the document going back to the snapshot with nothing recorded.
 */
static void _color_changed(GtkWidget *button, const GdkRGBA *color, const dt_chooser_color_phase_t phase,
                           gpointer user_data)
{
  props_binding_t *binding = (props_binding_t *)user_data;
  props_t *props = binding->owner;
  // A window closed for the host gives back nothing: the host commits what it has pending before it
  // closes them, and a CANCEL arriving from inside a refill would restore the document under it.
  if(props->closing || props->closing_dialogs) return;
  dt_canvas_prop_value_t value;
  memset(&value, 0, sizeof(value));
  value.color = dt_canvas_color((float)color->red, (float)color->green, (float)color->blue, (float)color->alpha);
  switch(phase)
  {
    case DT_CHOOSER_COLOR_LIVE:
      _edit_live(binding, &value);
      break;
    case DT_CHOOSER_COLOR_COMMIT:
      // Carried as a last step, then committed: the session may have been ended already -- the host
      // commits what is pending before an undo or a view switch -- and the colour kept must still land.
      _edit_live(binding, &value);
      _commit_live(props);
      break;
    case DT_CHOOSER_COLOR_CANCEL:
      _cancel_live(binding, &value);
      break;
  }
}

static void _font_picked(GtkWidget *button, const char *font, gpointer user_data)
{
  props_binding_t *binding = (props_binding_t *)user_data;
  if(binding->owner->closing) return;
  dt_canvas_prop_value_t value;
  memset(&value, 0, sizeof(value));
  g_strlcpy(value.text, font, sizeof(value.text));
  _edit_once(binding, &value);
}

static void _feature_toggled(GtkToggleButton *check, gpointer user_data)
{
  props_binding_t *binding = (props_binding_t *)user_data;
  if(binding->owner->closing) return;
  const char *feature_tag = g_object_get_data(G_OBJECT(check), PROPS_FEATURE_TAG_KEY);
  if(IS_NULL_PTR(feature_tag)) return;
  dt_canvas_prop_value_t value;
  _binding_value(binding, &value);
  // Edited at the length the record holds, so a tag that would not fit is refused whole rather
  // than stored as half a tag, which Pango reads as no features at all.
  dt_canvas_text_feature_set(value.text, DT_CANVAS_TEXT_FEATURES_LEN, feature_tag,
                             gtk_toggle_button_get_active(check));
  _edit_once(binding, &value);
}

static void _action_clicked(GtkButton *button, gpointer user_data)
{
  props_binding_t *binding = (props_binding_t *)user_data;
  if(binding->owner->closing) return;
  dt_canvas_prop_value_t value;
  memset(&value, 0, sizeof(value));
  _edit_once(binding, &value);
}

/** The uniform inset's link: linked, one slider sets four sides; unlinked, the four show under it. */
static void _inset_quad_pressed(GtkWidget *widget, gpointer user_data)
{
  props_binding_t *binding = (props_binding_t *)user_data;
  props_t *props = binding->owner;
  if(props->closing) return;
  props->inset_linked = dt_bauhaus_widget_get_quad_active(widget);
  const gboolean linked = props->inset_linked;
  const dt_canvas_prop_id_t sides[4] = { DT_CANVAS_PROP_TEXT_INSET_TOP, DT_CANVAS_PROP_TEXT_INSET_RIGHT,
                                         DT_CANVAS_PROP_TEXT_INSET_BOTTOM, DT_CANVAS_PROP_TEXT_INSET_LEFT };
  for(int side = 0; side < 4; side++)
  {
    props_binding_t *side_binding = &props->bindings[sides[side]];
    if(IS_NULL_PTR(side_binding->row)) continue;
    gtk_widget_set_visible(side_binding->row, !linked && props->applies[sides[side]]);
  }
  // The section grew or shrank by four rows: to the host that is the same as opening it again.
  if(!IS_NULL_PTR(props->host.section_toggled))
    props->host.section_toggled(props->host.data, DT_CANVAS_SECTION_TEXT_BOX, TRUE);
}

static void _own_toggled(GtkToggleButton *toggle, gpointer user_data)
{
  props_section_t *section = (props_section_t *)user_data;
  props_t *props = section->owner;
  if(props->closing || !props->has_target) return;
  _commit_live(props);
  const dt_canvas_prop_group_t group = dt_canvas_prop_section_group(section->section, props->kind);
  if(group == DT_CANVAS_GROUP_NONE || IS_NULL_PTR(props->host.group_own)) return;
  props->host.group_own(props->host.data, group, gtk_toggle_button_get_active(toggle));
}

/** The accordion: opening a section folds the one that was open, and only the opening is reported. */
static void _section_toggled(GtkToggleButton *toggle, gpointer user_data)
{
  props_section_t *section = (props_section_t *)user_data;
  props_t *props = section->owner;
  if(props->closing) return;
  const gboolean open = gtk_toggle_button_get_active(toggle);
  const int index = (int)section->section;
  if(open)
  {
    for(int other = 0; other < DT_CANVAS_SECTION_COUNT; other++)
    {
      props_section_t *other_section = &props->sections[other];
      if(other == index || IS_NULL_PTR(other_section->collapsible.toggle)) continue;
      if(!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(other_section->collapsible.toggle))) continue;
      g_signal_handler_block(other_section->collapsible.toggle, other_section->toggle_handler);
      dt_gui_collapsible_section_set_expanded(&other_section->collapsible, FALSE);
      g_signal_handler_unblock(other_section->collapsible.toggle, other_section->toggle_handler);
    }
    props->open_section = index;
  }
  else if(props->open_section == index)
  {
    props->open_section = -1;
  }
  if(!IS_NULL_PTR(props->host.section_toggled))
    props->host.section_toggled(props->host.data, section->section, open);
}

static void _card_toggled(GtkToggleButton *toggle, gpointer user_data)
{
  props_t *props = (props_t *)user_data;
  if(props->closing) return;
  if(!IS_NULL_PTR(props->host.card_toggled))
    props->host.card_toggled(props->host.data, gtk_toggle_button_get_active(toggle));
}

static void _content_clicked(GtkButton *button, gpointer user_data)
{
  props_t *props = (props_t *)user_data;
  if(props->closing) return;
  _commit_live(props);
  if(!IS_NULL_PTR(props->host.action)) props->host.action(props->host.data, DT_CANVAS_PROPS_ACTION_CONTENT);
}

static void _close_clicked(GtkButton *button, gpointer user_data)
{
  props_t *props = (props_t *)user_data;
  if(props->closing) return;
  _commit_live(props);
  if(!IS_NULL_PTR(props->host.action)) props->host.action(props->host.data, DT_CANVAS_PROPS_ACTION_CLOSE);
}

static void _report_pointer(props_t *props, const gboolean inside)
{
  props->pointer_reported = inside;
  if(!IS_NULL_PTR(props->host.pointer_inside)) props->host.pointer_inside(props->host.data, inside);
}

/**
 * The pointer crossed the properties' edge; moving onto or off one of their own controls is not that.
 *
 * Nor is a grab. A slider's fine-tuning popup, a combobox's list and a modal colour dialog each take
 * a grab, and GTK tells every widget the grab shadows that the pointer left it, wherever the pointer
 * is: told so, the host would stop holding the properties still and could move them out from under
 * the popup the user is working in. A grab ending is read, though, since the crossings the pointer
 * made while it held went to the grab and not here: see _root_grab_notify().
 */
static gboolean _root_crossed(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  props_t *props = (props_t *)user_data;
  if(props->closing || event->detail == GDK_NOTIFY_INFERIOR) return FALSE;
  if(event->mode == GDK_CROSSING_GRAB || event->mode == GDK_CROSSING_GTK_GRAB
     || event->mode == GDK_CROSSING_STATE_CHANGED)
    return FALSE;
  _report_pointer(props, event->type == GDK_ENTER_NOTIFY);
  return FALSE;
}

/** Whether the pointer is over the properties right now, whatever window is on top of them. */
static gboolean _pointer_over_root(props_t *props)
{
  GdkWindow *window = gtk_widget_get_window(props->root);
  if(IS_NULL_PTR(window) || !gtk_widget_get_mapped(props->root)) return FALSE;
  GdkSeat *seat = gdk_display_get_default_seat(gdk_window_get_display(window));
  GdkDevice *pointer = IS_NULL_PTR(seat) ? NULL : gdk_seat_get_pointer(seat);
  if(IS_NULL_PTR(pointer)) return FALSE;
  double pointer_x = -1.0;
  double pointer_y = -1.0;
  gdk_window_get_device_position_double(window, pointer, &pointer_x, &pointer_y, NULL);
  return pointer_x >= 0.0 && pointer_y >= 0.0 && pointer_x < gtk_widget_get_allocated_width(props->root)
         && pointer_y < gtk_widget_get_allocated_height(props->root);
}

/**
 * A grab elsewhere let go of the properties. Whatever the pointer did while it held -- leaving for
 * the popup, and further -- was told to the grab, so where it is now is asked of the pointer.
 */
static void _root_grab_notify(GtkWidget *widget, gboolean was_grabbed, gpointer user_data)
{
  props_t *props = (props_t *)user_data;
  // FALSE when a grab starts shadowing the properties, TRUE when it stops.
  if(props->closing || !was_grabbed) return;
  const gboolean inside = _pointer_over_root(props);
  if(inside != props->pointer_reported) _report_pointer(props, inside);
}

/**
 * Whoever held the properties destroyed them before letting go of them: every control is going now.
 * Nothing still pending -- the timer of a session, a focus leaving a control on its way out -- may
 * reach the host or those controls from here on.
 */
static void _root_destroyed(GtkWidget *widget, gpointer user_data)
{
  props_t *props = (props_t *)user_data;
  props->closing = TRUE;
  props->destroyed = TRUE;
  _debounce_remove(props);
}

/** A wheel no control used stops here: over the properties it never zooms the canvas behind them. */
static gboolean _root_scrolled(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  return TRUE;
}

/** The theme changed: the tallest strip control may be another height now. */
static void _strip_style_updated(GtkWidget *widget, gpointer user_data)
{
  props_t *props = (props_t *)user_data;
  props->strip_strut_height = 0;
}

static gboolean _kind_glyph_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  const props_glyph_t *glyph = (const props_glyph_t *)g_object_get_data(G_OBJECT(widget), "dt-canvas-props-glyph");
  if(IS_NULL_PTR(glyph)) return FALSE;
  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  GdkRGBA color;
  gtk_style_context_get_color(context, gtk_widget_get_state_flags(widget), &color);
  gdk_cairo_set_source_rgba(cr, &color);
  const int width = gtk_widget_get_allocated_width(widget);
  const int height = gtk_widget_get_allocated_height(widget);
  glyph->paint(cr, 0, 0, width, height, glyph->flags, NULL);
  return TRUE;
}

/* --- building the rows ------------------------------------------------------------------------ */

/**
 * Apply the digits typed into any spin button, through the spin button's own handler, as its focus
 * leaving would: one ONCE edit each. Digits that parse to the value already there report nothing and
 * hold nothing either. `typing` is cleared only after the update, since the handler reads it to tell
 * a typed value from a step.
 */
static void _apply_typing(props_t *props)
{
  for(int prop_id = DT_CANVAS_PROP_NONE + 1; prop_id < DT_CANVAS_PROP_COUNT; prop_id++)
  {
    props_binding_t *binding = &props->bindings[prop_id];
    if(!binding->typing || IS_NULL_PTR(binding->widget) || binding->prop->widget != DT_CANVAS_WIDGET_MEASURE) continue;
    gtk_spin_button_update(GTK_SPIN_BUTTON(binding->widget));
    binding->typing = FALSE;
  }
}

/**
 * A press on a control that does not take the focus applies the digits typed into a spin button first.
 * A spin button applies what was typed when it loses the focus, and a click that leaves the focus where
 * it was never takes it away: without this, a width typed and then followed by a click on the card
 * button, a switch or a colour would stay unapplied behind that click, and be applied -- as a later undo
 * step than the click's -- whenever the focus happened to move.
 */
static gboolean _apply_typing_pressed(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  props_t *props = (props_t *)user_data;
  if(props->closing) return FALSE;
  _apply_typing(props);
  return FALSE;
}

/** Digits typed into a spin button are applied by a press on this control, which takes no focus itself. */
static void _apply_typing_on_press(props_t *props, GtkWidget *widget)
{
  gtk_widget_add_events(widget, GDK_BUTTON_PRESS_MASK);
  g_signal_connect(widget, "button-press-event", G_CALLBACK(_apply_typing_pressed), props);
}

/**
 * A control reached by the keyboard, never by a click: a click leaves the focus where it was, and applies
 * what was typed into a spin button on its way.
 */
static void _no_focus_on_click(props_t *props, GtkWidget *widget)
{
  gtk_widget_set_focus_on_click(widget, FALSE);
  _apply_typing_on_press(props, widget);
}

static GtkWidget *_row_label(const dt_canvas_prop_t *prop, const gboolean fixed_width)
{
  GtkWidget *label = gtk_label_new(_(prop->label));
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  if(fixed_width)
  {
    // One width for every row's label, so the controls line up and a longer label ellipsises
    // rather than widening the card.
    gtk_label_set_width_chars(GTK_LABEL(label), PROPS_LABEL_CHARS);
    gtk_label_set_max_width_chars(GTK_LABEL(label), PROPS_LABEL_CHARS);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  }
  gtk_widget_set_tooltip_text(label, _(prop->tooltip));
  return label;
}

static GtkWidget *_unit_label(const dt_canvas_prop_t *prop)
{
  GtkWidget *label = gtk_label_new(IS_NULL_PTR(prop->unit) ? "" : _(prop->unit));
  dt_gui_add_class(label, "dt_canvas_summary");
  return label;
}

/** A row of a label and a control, the control against the right edge. */
static GtkWidget *_labelled_row(const dt_canvas_prop_t *prop, GtkWidget *control, GtkWidget *unit)
{
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(PROPS_STRIP_SPACING_PIXELS));
  gtk_box_pack_start(GTK_BOX(row), _row_label(prop, TRUE), FALSE, FALSE, 0);
  if(!IS_NULL_PTR(unit)) gtk_box_pack_end(GTK_BOX(row), unit, FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(row), control, FALSE, FALSE, 0);
  return row;
}

static GtkWidget *_build_tune(props_binding_t *binding)
{
  const dt_canvas_prop_t *prop = binding->prop;
  const double neutral = isnan(prop->neutral) ? prop->min : prop->neutral;
  GtkWidget *slider = dt_bauhaus_slider_new_with_range(dt_bauhaus_get_global(), DT_GUI_MODULE(NULL), (float)prop->min,
                                                       (float)prop->max, (float)prop->step, (float)neutral,
                                                       prop->digits);
  dt_bauhaus_slider_set_soft_range(slider, (float)prop->soft_min, (float)prop->soft_max);
  if(!IS_NULL_PTR(prop->unit))
  {
    gchar *format = g_strdup_printf(" %s", _(prop->unit));
    dt_bauhaus_slider_set_format(slider, format);
    dt_free(format);
  }
  dt_bauhaus_widget_set_label(slider, _(prop->label));
  gtk_widget_set_tooltip_text(slider, _(prop->tooltip));
  binding->widget = slider;
  binding->handler = g_signal_connect(slider, "value-changed", G_CALLBACK(_slider_changed), binding);
  g_signal_connect(slider, "event-after", G_CALLBACK(_control_event_after), binding);
  g_signal_connect_after(slider, "focus-out-event", G_CALLBACK(_control_focus_out), binding);
  if(prop->id == DT_CANVAS_PROP_TEXT_INSET)
  {
    dt_bauhaus_widget_set_quad_paint(slider, dtgtk_cairo_paint_link, CPF_NONE, NULL);
    dt_bauhaus_widget_set_quad_toggle(slider, TRUE);
    g_signal_connect(slider, "quad-pressed", G_CALLBACK(_inset_quad_pressed), binding);
  }
  return slider;
}

static GtkWidget *_build_choice(props_binding_t *binding)
{
  const dt_canvas_prop_t *prop = binding->prop;
  GtkWidget *combobox = dt_bauhaus_combobox_new(dt_bauhaus_get_global(), DT_GUI_MODULE(NULL));
  dt_bauhaus_widget_set_label(combobox, _(prop->label));
  const int count = dt_canvas_prop_choice_count(prop);
  for(int choice = 0; choice < count; choice++)
  {
    const char *label = dt_canvas_prop_choice_label(prop, choice);
    // A fixed list is ours to translate; a map style is the provider's own name.
    dt_bauhaus_combobox_add(combobox, IS_NULL_PTR(prop->choices) ? label : _(label));
  }
  gtk_widget_set_tooltip_text(combobox, _(prop->tooltip));
  binding->widget = combobox;
  binding->handler = g_signal_connect(combobox, "value-changed", G_CALLBACK(_combobox_changed), binding);
  return combobox;
}

/** How many characters a spin button needs for its range at its precision, sign and point included. */
static int _spin_chars(const dt_canvas_prop_t *prop)
{
  const double magnitude = MAX(fabs(prop->soft_min), fabs(prop->soft_max));
  const int integer_digits = (int)floor(log10(MAX(magnitude, 1.0))) + 1;
  const int sign = prop->soft_min < 0.0 ? 1 : 0;
  const int decimals = prop->digits > 0 ? prop->digits + 1 : 0;
  // A plane position is typed, not read at a glance: six characters show every page of a document
  // and the entry scrolls for the rest.
  return CLAMP(integer_digits + sign + decimals, 2, 6);
}

static GtkWidget *_build_spin(props_binding_t *binding)
{
  const dt_canvas_prop_t *prop = binding->prop;
  GtkWidget *spin = gtk_spin_button_new_with_range(prop->min, prop->max, prop->step > 0.0 ? prop->step : 1.0);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin), (guint)MAX(prop->digits, 0));
  gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(spin), TRUE);
  gtk_entry_set_width_chars(GTK_ENTRY(spin), _spin_chars(prop));
  gtk_widget_set_tooltip_text(spin, _(prop->tooltip));
  binding->widget = spin;
  binding->handler = g_signal_connect(spin, "value-changed", G_CALLBACK(_spin_changed), binding);
  g_signal_connect(spin, "key-press-event", G_CALLBACK(_spin_key_pressed), binding);
  g_signal_connect_after(spin, "activate", G_CALLBACK(_spin_activated), binding);
  g_signal_connect(spin, "event-after", G_CALLBACK(_control_event_after), binding);
  // After the spin button's own handler, which applies the digits typed: they are the edit.
  g_signal_connect_after(spin, "focus-out-event", G_CALLBACK(_control_focus_out), binding);
  return spin;
}

static GtkWidget *_build_icons(props_binding_t *binding)
{
  const dt_canvas_prop_t *prop = binding->prop;
  GtkWidget *linked = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  dt_gui_add_class(linked, "linked");
  const int count = MIN(dt_canvas_prop_choice_count(prop), (int)G_N_ELEMENTS(binding->toggles));
  for(int choice = 0; choice < count; choice++)
  {
    const props_glyph_t *glyph = _glyph(IS_NULL_PTR(prop->icons) ? NULL : prop->icons[choice]);
    GtkWidget *toggle = dtgtk_togglebutton_new(glyph->paint, glyph->flags, NULL);
    _no_focus_on_click(binding->owner, toggle);
    gtk_widget_set_tooltip_text(toggle, _(dt_canvas_prop_choice_label(prop, choice)));
    g_object_set_data(G_OBJECT(toggle), "dt-canvas-props-choice", GINT_TO_POINTER(choice));
    binding->toggles[choice] = toggle;
    binding->toggle_handlers[choice] = g_signal_connect(toggle, "toggled", G_CALLBACK(_icon_toggled), binding);
    gtk_box_pack_start(GTK_BOX(linked), toggle, FALSE, FALSE, 0);
  }
  binding->toggle_count = count;
  binding->widget = linked;
  return linked;
}

static GtkWidget *_build_icon_flag(props_binding_t *binding)
{
  const dt_canvas_prop_t *prop = binding->prop;
  const props_glyph_t *glyph = _glyph(IS_NULL_PTR(prop->icons) ? NULL : prop->icons[0]);
  GtkWidget *toggle = dtgtk_togglebutton_new(glyph->paint, glyph->flags, NULL);
  _no_focus_on_click(binding->owner, toggle);
  gtk_widget_set_tooltip_text(toggle, _(prop->tooltip));
  binding->widget = toggle;
  binding->handler = g_signal_connect(toggle, "toggled", G_CALLBACK(_flag_toggled), binding);
  return toggle;
}

static GtkWidget *_build_flag(props_binding_t *binding)
{
  const dt_canvas_prop_t *prop = binding->prop;
  GtkWidget *check = gtk_check_button_new_with_label(_(prop->label));
  _no_focus_on_click(binding->owner, check);
  gtk_widget_set_tooltip_text(check, _(prop->tooltip));
  binding->widget = check;
  binding->handler = g_signal_connect(check, "toggled", G_CALLBACK(_flag_toggled), binding);
  return check;
}

static GtkWidget *_build_color(props_binding_t *binding)
{
  const dt_canvas_prop_t *prop = binding->prop;
  GtkWidget *button = dt_chooser_button_color_new(_(prop->tooltip), TRUE, DT_CANVAS_COLOR_HISTORY_KEY,
                                                  _color_changed, binding);
  // The chooser takes no focus on a click already.
  _apply_typing_on_press(binding->owner, button);
  gtk_widget_set_tooltip_text(button, _(prop->tooltip));
  binding->widget = button;
  return button;
}

static GtkWidget *_build_font(props_binding_t *binding)
{
  const dt_canvas_prop_t *prop = binding->prop;
  GtkWidget *button = dt_chooser_button_font_new(_(prop->label), PROPS_FONT_CHARS, _font_picked, binding);
  _apply_typing_on_press(binding->owner, button);
  gtk_widget_set_tooltip_text(button, _(prop->tooltip));
  binding->widget = button;
  return button;
}

static GtkWidget *_build_features(props_binding_t *binding)
{
  const dt_canvas_prop_t *prop = binding->prop;
  GtkWidget *flow = gtk_flow_box_new();
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), PROPS_FEATURES_PER_LINE);
  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
  gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flow), FALSE);
  gtk_widget_set_tooltip_text(flow, _(prop->tooltip));
  binding->widget = flow;
  GtkWidget *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(column), _row_label(prop, FALSE), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(column), flow, FALSE, FALSE, 0);
  binding->owner->features_empty = gtk_label_new(_("This font offers no OpenType features to choose."));
  gtk_label_set_line_wrap(GTK_LABEL(binding->owner->features_empty), TRUE);
  gtk_label_set_xalign(GTK_LABEL(binding->owner->features_empty), 0.0f);
  dt_gui_add_class(binding->owner->features_empty, "dt_canvas_summary");
  gtk_box_pack_start(GTK_BOX(column), binding->owner->features_empty, FALSE, FALSE, 0);
  return column;
}

static GtkWidget *_build_info(props_binding_t *binding, const gboolean in_strip)
{
  GtkWidget *label = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
  if(in_strip)
  {
    // As wide whatever it says: a long file name must not widen the strip from one refill to the next.
    gtk_label_set_width_chars(GTK_LABEL(label), PROPS_INFO_CHARS);
    gtk_label_set_max_width_chars(GTK_LABEL(label), PROPS_INFO_CHARS);
  }
  dt_gui_add_class(label, "dt_canvas_summary");
  binding->widget = label;
  return label;
}

static GtkWidget *_build_action(props_binding_t *binding, const gboolean in_strip)
{
  const dt_canvas_prop_t *prop = binding->prop;
  GtkWidget *button = NULL;
  if(in_strip)
  {
    const props_glyph_t *glyph = _glyph(IS_NULL_PTR(prop->icons) ? NULL : prop->icons[0]);
    button = dtgtk_button_new(glyph->paint, glyph->flags, NULL);
  }
  else
  {
    // In the card an action says what it does in words: there is room, and "add a map" has no glyph.
    button = gtk_button_new_with_label(_(prop->label));
    gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
    gtk_widget_set_halign(button, GTK_ALIGN_START);
  }
  _no_focus_on_click(binding->owner, button);
  gtk_widget_set_tooltip_text(button, _(prop->tooltip));
  binding->widget = button;
  binding->handler = g_signal_connect(button, "clicked", G_CALLBACK(_action_clicked), binding);
  return button;
}

/** The control of a property, whatever its nature, without its row. */
static GtkWidget *_build_control(props_binding_t *binding, const gboolean in_strip)
{
  switch(binding->prop->widget)
  {
    case DT_CANVAS_WIDGET_TUNE:
      return _build_tune(binding);
    case DT_CANVAS_WIDGET_CHOICE:
      return _build_choice(binding);
    case DT_CANVAS_WIDGET_MEASURE:
      return _build_spin(binding);
    case DT_CANVAS_WIDGET_ICONS:
      return _build_icons(binding);
    case DT_CANVAS_WIDGET_ICON_FLAG:
      return _build_icon_flag(binding);
    case DT_CANVAS_WIDGET_FLAG:
      return _build_flag(binding);
    case DT_CANVAS_WIDGET_COLOR:
      return _build_color(binding);
    case DT_CANVAS_WIDGET_FONT:
      return _build_font(binding);
    case DT_CANVAS_WIDGET_FEATURES:
      return _build_features(binding);
    case DT_CANVAS_WIDGET_INFO:
      return _build_info(binding, in_strip);
    case DT_CANVAS_WIDGET_ACTION:
      return _build_action(binding, in_strip);
    default:
      return NULL;
  }
}

/** A strip control stands alone, with its unit beside it when it has one. */
static void _build_strip_row(props_t *props, props_binding_t *binding)
{
  GtkWidget *control = _build_control(binding, TRUE);
  if(IS_NULL_PTR(control)) return;
  GtkWidget *row = control;
  if(binding->prop->widget == DT_CANVAS_WIDGET_MEASURE && !IS_NULL_PTR(binding->prop->unit))
  {
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(2));
    gtk_box_pack_start(GTK_BOX(row), control, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), _unit_label(binding->prop), FALSE, FALSE, 0);
  }
  gtk_widget_set_valign(row, GTK_ALIGN_CENTER);
  binding->row = row;
  binding->in_strip = TRUE;
  gtk_box_pack_start(GTK_BOX(props->strip), row, FALSE, FALSE, 0);
}

/**
 * The first half of an exact pair opens a grid of three columns -- first spin, what sits between
 * them, second spin -- each spin under its label, and the second half closes it. A switch the table
 * puts between the two halves (a frame keeping its proportions, between its width and its height)
 * takes the middle. Labels go above rather than beside: beside, "Latitude" and "Longitude" and two
 * spin buttons need 371 px at 1x, more than the narrowest card has.
 */
static GtkWidget *_pair_grid(props_t *props, props_binding_t *binding, GtkWidget *control)
{
  const dt_canvas_prop_t *prop = binding->prop;
  if(props->pair_first == DT_CANVAS_PROP_NONE || props->pair_first != prop->pair_with)
  {
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(PROPS_STRIP_SPACING_PIXELS));
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), FALSE);
    gtk_grid_attach(GTK_GRID(grid), _row_label(prop, FALSE), 0, 0, 1, 1);
    gtk_widget_set_hexpand(control, TRUE);
    gtk_grid_attach(GTK_GRID(grid), control, 0, 1, 1, 1);
    props->pair_grid = grid;
    props->pair_first = prop->id;
    return grid;
  }
  GtkWidget *grid = props->pair_grid;
  gtk_grid_attach(GTK_GRID(grid), _row_label(prop, FALSE), 2, 0, 1, 1);
  gtk_widget_set_hexpand(control, TRUE);
  gtk_grid_attach(GTK_GRID(grid), control, 2, 1, 1, 1);
  props->pair_grid = NULL;
  props->pair_first = DT_CANVAS_PROP_NONE;
  return NULL;
}

static void _build_card_row(props_t *props, props_binding_t *binding)
{
  const dt_canvas_prop_t *prop = binding->prop;
  props_section_t *section = &props->sections[prop->section];
  GtkWidget *box = prop->tier == DT_CANVAS_TIER_MORE ? section->more_box : section->essential_box;

  // A switch between the halves of an open pair sits between them.
  if(!IS_NULL_PTR(props->pair_grid) && prop->widget == DT_CANVAS_WIDGET_ICON_FLAG
     && prop->section == props->bindings[props->pair_first].prop->section)
  {
    GtkWidget *control = _build_control(binding, FALSE);
    gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
    gtk_grid_attach(GTK_GRID(props->pair_grid), control, 1, 1, 1, 1);
    binding->row = control;
    return;
  }

  GtkWidget *control = _build_control(binding, FALSE);
  if(IS_NULL_PTR(control)) return;
  GtkWidget *row = NULL;
  switch(prop->widget)
  {
    case DT_CANVAS_WIDGET_MEASURE:
      if(prop->pair_with != DT_CANVAS_PROP_NONE)
      {
        GtkWidget *grid = _pair_grid(props, binding, control);
        if(IS_NULL_PTR(grid))
        {
          // The second half joins the first half's row and is shown with it.
          binding->row = props->bindings[prop->pair_with].row;
          return;
        }
        row = grid;
      }
      else
      {
        row = _labelled_row(prop, control, IS_NULL_PTR(prop->unit) ? NULL : _unit_label(prop));
      }
      break;
    case DT_CANVAS_WIDGET_ICONS:
    case DT_CANVAS_WIDGET_ICON_FLAG:
    case DT_CANVAS_WIDGET_COLOR:
    case DT_CANVAS_WIDGET_INFO:
      row = _labelled_row(prop, control, NULL);
      break;
    default:
      // Sliders and comboboxes carry their own label, a check button its own text, the features
      // their own heading and an action its own words.
      row = control;
      break;
  }
  binding->row = row;
  gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
}

static void _build_section(props_t *props, const dt_canvas_prop_section_t section_id)
{
  props_section_t *section = &props->sections[section_id];
  section->owner = props;
  section->section = section_id;
  dt_gui_new_collapsible_section(&section->collapsible, NULL, _(dt_canvas_prop_section_label(section_id, 0)),
                                 GTK_BOX(props->sections_box), GTK_PACK_START);
  dt_gui_collapsible_section_t *collapsible = &section->collapsible;
  _no_focus_on_click(props, collapsible->toggle);

  // The heading reads "Border  canvas default · 2 pt": the summary sits inside the part a click
  // folds the section from, next to the title, and takes what width is left.
  GtkWidget *event_box = gtk_widget_get_parent(collapsible->label);
  g_object_ref(collapsible->label);
  gtk_container_remove(GTK_CONTAINER(event_box), collapsible->label);
  GtkWidget *title = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(6));
  gtk_widget_set_halign(collapsible->label, GTK_ALIGN_START);
  gtk_label_set_xalign(GTK_LABEL(collapsible->label), 0.0f);
  gtk_box_pack_start(GTK_BOX(title), collapsible->label, FALSE, FALSE, 0);
  g_object_unref(collapsible->label);
  section->summary = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(section->summary), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(section->summary), PANGO_ELLIPSIZE_END);
  // Ellipsised from nothing, so a long summary never makes the card wider than the strip.
  gtk_label_set_width_chars(GTK_LABEL(section->summary), 0);
  dt_gui_add_class(section->summary, "dt_canvas_summary");
  gtk_box_pack_start(GTK_BOX(title), section->summary, TRUE, TRUE, 0);
  gtk_container_add(GTK_CONTAINER(event_box), title);

  // The group's switch sits outside that event box, so flipping it never folds the section.
  section->own = dtgtk_togglebutton_new(dtgtk_cairo_paint_switch, CPF_NONE, NULL);
  _no_focus_on_click(props, section->own);
  gtk_widget_set_tooltip_text(section->own, _("Give this object its own values instead of the canvas's"));
  gtk_box_pack_start(GTK_BOX(collapsible->header), section->own, FALSE, FALSE, 0);
  gtk_box_reorder_child(GTK_BOX(collapsible->header), section->own, 1);
  gtk_widget_set_no_show_all(section->own, TRUE);
  section->own_handler = g_signal_connect(section->own, "toggled", G_CALLBACK(_own_toggled), section);

  section->essential_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_PIXEL_APPLY_DPI(2));
  section->more_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_PIXEL_APPLY_DPI(2));
  dt_gui_add_class(section->more_box, "dt_canvas_more");
  gtk_box_pack_start(collapsible->container, section->essential_box, FALSE, FALSE, 0);
  gtk_box_pack_start(collapsible->container, section->more_box, FALSE, FALSE, 0);

  section->toggle_handler
      = g_signal_connect_after(collapsible->toggle, "toggled", G_CALLBACK(_section_toggled), section);
}

static void _build_strip(props_t *props)
{
  props->strip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(PROPS_STRIP_SPACING_PIXELS));
  dt_gui_add_class(props->strip, "dt-canvas-props-strip");
  g_signal_connect(props->strip, "style-updated", G_CALLBACK(_strip_style_updated), props);

  props->strip_strut = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(props->strip), props->strip_strut, FALSE, FALSE, 0);

  props->kind_glyph = gtk_drawing_area_new();
  gtk_widget_set_size_request(props->kind_glyph, DT_PIXEL_APPLY_DPI(PROPS_GLYPH_PIXELS),
                              DT_PIXEL_APPLY_DPI(PROPS_GLYPH_PIXELS));
  gtk_widget_set_valign(props->kind_glyph, GTK_ALIGN_CENTER);
  g_signal_connect(props->kind_glyph, "draw", G_CALLBACK(_kind_glyph_draw), NULL);
  gtk_box_pack_start(GTK_BOX(props->strip), props->kind_glyph, FALSE, FALSE, 0);

  // Packed from the right edge: close, then the card, then the content action. Hiding the action
  // for a kind that has none moves neither of the other two.
  props->close_button = dtgtk_button_new(dtgtk_cairo_paint_cancel, CPF_NONE, NULL);
  _no_focus_on_click(props, props->close_button);
  gtk_widget_set_tooltip_text(props->close_button, _("Close the properties (Escape)"));
  gtk_widget_set_valign(props->close_button, GTK_ALIGN_CENTER);
  g_signal_connect(props->close_button, "clicked", G_CALLBACK(_close_clicked), props);
  gtk_box_pack_end(GTK_BOX(props->strip), props->close_button, FALSE, FALSE, 0);

  props->card_toggle = dtgtk_togglebutton_new(dtgtk_cairo_paint_solid_arrow, CPF_DIRECTION_DOWN, NULL);
  _no_focus_on_click(props, props->card_toggle);
  gtk_widget_set_tooltip_text(props->card_toggle, _("All the properties"));
  gtk_widget_set_valign(props->card_toggle, GTK_ALIGN_CENTER);
  props->card_toggle_handler = g_signal_connect(props->card_toggle, "toggled", G_CALLBACK(_card_toggled), props);
  gtk_box_pack_end(GTK_BOX(props->strip), props->card_toggle, FALSE, FALSE, 0);

  props->content_button = dtgtk_button_new(dtgtk_cairo_paint_edit_text, CPF_NONE, NULL);
  _no_focus_on_click(props, props->content_button);
  gtk_widget_set_valign(props->content_button, GTK_ALIGN_CENTER);
  gtk_widget_set_no_show_all(props->content_button, TRUE);
  g_signal_connect(props->content_button, "clicked", G_CALLBACK(_content_clicked), props);
  gtk_box_pack_end(GTK_BOX(props->strip), props->content_button, FALSE, FALSE, 0);
}

dt_canvas_props_gtk_t *dt_canvas_props_gtk_new(const dt_canvas_props_host_t *host)
{
  props_t *props = g_malloc0(sizeof(props_t));
  if(!IS_NULL_PTR(host)) props->host = *host;
  props->live = DT_CANVAS_PROP_NONE;
  props->open_section = -1;
  props->card_max_content = -1;
  props->inset_linked = TRUE;

  props->root = gtk_event_box_new();
  g_object_ref_sink(props->root);
  gtk_event_box_set_visible_window(GTK_EVENT_BOX(props->root), TRUE);
  // Filling, not aligned: GTK shrinks an aligned overlay child to its natural size inside the rectangle
  // it is given, and the placement's rectangle must be the widget's, to the pixel.
  gtk_widget_set_halign(props->root, GTK_ALIGN_FILL);
  gtk_widget_set_valign(props->root, GTK_ALIGN_FILL);
  gtk_widget_add_events(props->root, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK | GDK_SCROLL_MASK
                                         | GDK_SMOOTH_SCROLL_MASK);
  g_signal_connect(props->root, "enter-notify-event", G_CALLBACK(_root_crossed), props);
  g_signal_connect(props->root, "leave-notify-event", G_CALLBACK(_root_crossed), props);
  g_signal_connect(props->root, "scroll-event", G_CALLBACK(_root_scrolled), props);
  g_signal_connect(props->root, "grab-notify", G_CALLBACK(_root_grab_notify), props);
  g_signal_connect(props->root, "destroy", G_CALLBACK(_root_destroyed), props);
  gtk_widget_set_name(props->root, "canvas-props");
  dt_accels_block_plain_keys_inside(props->root);

  props->frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  dt_gui_add_class(props->frame, "dt-canvas-props");
  gtk_container_add(GTK_CONTAINER(props->root), props->frame);

  _build_strip(props);
  gtk_box_pack_start(GTK_BOX(props->frame), props->strip, FALSE, FALSE, 0);

  props->card = gtk_scrolled_window_new(NULL, NULL);
  dt_gui_add_class(props->card, "dt-canvas-props-card");
  // The host gives the card its whole height, and cuts it short only in a view too short for it: the
  // wheel scrolls that one, and no scrollbar is ever drawn. A scrollbar the card could show would take
  // its width from the rows and read as a card with more in it than a view with room had shown.
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(props->card), GTK_POLICY_NEVER, GTK_POLICY_EXTERNAL);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(props->card), TRUE);
  gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(props->card), GTK_SHADOW_NONE);
  props->sections_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(props->card), props->sections_box);
  GtkWidget *viewport = gtk_bin_get_child(GTK_BIN(props->card));
  if(GTK_IS_VIEWPORT(viewport)) gtk_viewport_set_shadow_type(GTK_VIEWPORT(viewport), GTK_SHADOW_NONE);
  gtk_box_pack_start(GTK_BOX(props->frame), props->card, FALSE, FALSE, 0);

  for(int section_id = 0; section_id < DT_CANVAS_SECTION_COUNT; section_id++)
    _build_section(props, (dt_canvas_prop_section_t)section_id);

  // Every row of every kind, once, in table order: the order rows are shown in.
  props->pair_first = DT_CANVAS_PROP_NONE;
  size_t count = 0;
  const dt_canvas_prop_t *table = dt_canvas_props(&count);
  for(size_t idx = 0; idx < count; idx++)
  {
    props_binding_t *binding = &props->bindings[table[idx].id];
    binding->owner = props;
    binding->prop = &table[idx];
    if(table[idx].tier == DT_CANVAS_TIER_STRIP)
      _build_strip_row(props, binding);
    else
      _build_card_row(props, binding);
  }
  // The spacer between the kind's controls and the trailing group, so the latter keeps to the edge.
  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spacer, TRUE);
  gtk_box_pack_start(GTK_BOX(props->strip), spacer, TRUE, TRUE, 0);

  gtk_widget_show_all(props->frame);
  // Rows are shown and hidden by the refill alone: an outside show_all must not bring back another
  // kind's rows, nor the card the host has not placed.
  for(int prop_id = DT_CANVAS_PROP_NONE + 1; prop_id < DT_CANVAS_PROP_COUNT; prop_id++)
  {
    props_binding_t *binding = &props->bindings[prop_id];
    if(IS_NULL_PTR(binding->row)) continue;
    gtk_widget_hide(binding->row);
    gtk_widget_set_no_show_all(binding->row, TRUE);
  }
  for(int section_id = 0; section_id < DT_CANVAS_SECTION_COUNT; section_id++)
  {
    props_section_t *section = &props->sections[section_id];
    gtk_widget_hide(section->collapsible.expander);
    gtk_widget_set_no_show_all(section->collapsible.expander, TRUE);
    gtk_widget_set_no_show_all(section->more_box, TRUE);
    gtk_widget_set_no_show_all(section->essential_box, TRUE);
  }
  gtk_widget_hide(props->card);
  gtk_widget_set_no_show_all(props->card, TRUE);
  return props;
}

GtkWidget *dt_canvas_props_gtk_root(dt_canvas_props_gtk_t *props)
{
  return IS_NULL_PTR(props) ? NULL : props->root;
}

/* --- refilling --------------------------------------------------------------------------------- */

/** The kind glyph opening the strip, and the content action closing it. */
static void _structure_kind(props_t *props)
{
  static const props_glyph_t text_glyph = { NULL, dtgtk_cairo_paint_text_label, CPF_NONE };
  static const props_glyph_t image_glyph = { NULL, dtgtk_cairo_paint_camera, CPF_NONE };
  static const props_glyph_t map_glyph = { NULL, dtgtk_cairo_paint_map_pin, CPF_NONE };
  static const props_glyph_t svg_glyph = { NULL, dtgtk_cairo_paint_draw_structure, CPF_NONE };
  static const props_glyph_t connector_glyph = { NULL, dtgtk_cairo_paint_route, CPF_ROUTE_CUBIC };
  const props_glyph_t *glyph = NULL;
  switch(props->kind)
  {
    case DT_CANVAS_OBJECT_TEXT:
      glyph = &text_glyph;
      dtgtk_button_set_paint(DTGTK_BUTTON(props->content_button), dtgtk_cairo_paint_edit_text, CPF_NONE, NULL);
      break;
    case DT_CANVAS_OBJECT_IMAGE:
      glyph = &image_glyph;
      dtgtk_button_set_paint(DTGTK_BUTTON(props->content_button), dtgtk_cairo_paint_darkroom, CPF_NONE, NULL);
      break;
    case DT_CANVAS_OBJECT_SVG:
      glyph = &svg_glyph;
      dtgtk_button_set_paint(DTGTK_BUTTON(props->content_button), dtgtk_cairo_paint_refresh, CPF_NONE, NULL);
      break;
    case DT_CANVAS_OBJECT_MAP:
      glyph = &map_glyph;
      break;
    default:
      glyph = &connector_glyph;
      break;
  }
  const char *content_tooltip = dt_canvas_props_content_action_tooltip(props->kind);
  gtk_widget_set_tooltip_text(props->content_button, IS_NULL_PTR(content_tooltip) ? NULL : _(content_tooltip));
  g_object_set_data(G_OBJECT(props->kind_glyph), "dt-canvas-props-glyph", (gpointer)glyph);
  gtk_widget_queue_draw(props->kind_glyph);
  gtk_widget_set_visible(props->content_button, dt_canvas_props_has_content_action(props->kind));
}

/** Whether a row shows: its property applies, and a side of the inset only while the inset is unlinked. */
static gboolean _row_wanted(const props_t *props, const dt_canvas_prop_id_t prop_id)
{
  if(!props->applies[prop_id]) return FALSE;
  switch(prop_id)
  {
    case DT_CANVAS_PROP_TEXT_INSET_TOP:
    case DT_CANVAS_PROP_TEXT_INSET_RIGHT:
    case DT_CANVAS_PROP_TEXT_INSET_BOTTOM:
    case DT_CANVAS_PROP_TEXT_INSET_LEFT:
      return !props->inset_linked;
    default:
      return TRUE;
  }
}

/** Fold every section, telling nobody: what a new kind starts from. */
static void _fold_all(props_t *props)
{
  for(int section_id = 0; section_id < DT_CANVAS_SECTION_COUNT; section_id++)
  {
    props_section_t *section = &props->sections[section_id];
    g_signal_handler_block(section->collapsible.toggle, section->toggle_handler);
    dt_gui_collapsible_section_set_expanded(&section->collapsible, FALSE);
    g_signal_handler_unblock(section->collapsible.toggle, section->toggle_handler);
  }
  props->open_section = -1;
}

/**
 * Which rows and sections exist for this object. Run when the object, its kind or anything a row
 * depends on changed -- never for a value alone, so an edit does not hide a row under the pointer.
 */
static void _structure_pass(props_t *props, const dt_canvas_t *canvas, const dt_canvas_object_t *object,
                            const gboolean retarget, const gboolean kind_changed)
{
  if(kind_changed) _structure_kind(props);
  if(retarget)
  {
    // The inset is one number while its four sides agree; that is decided when the object is
    // shown, not while its sides are being edited apart.
    props->inset_linked = dt_canvas_props_inset_uniform(object);
    props_binding_t *inset = &props->bindings[DT_CANVAS_PROP_TEXT_INSET];
    if(!IS_NULL_PTR(inset->widget)) dt_bauhaus_widget_set_quad_active(inset->widget, props->inset_linked);
  }

  gboolean row_shown[DT_CANVAS_PROP_COUNT];
  memset(row_shown, 0, sizeof(row_shown));
  for(int prop_id = DT_CANVAS_PROP_NONE + 1; prop_id < DT_CANVAS_PROP_COUNT; prop_id++)
  {
    props_binding_t *binding = &props->bindings[prop_id];
    if(IS_NULL_PTR(binding->row)) continue;
    // A row two properties share shows when either does.
    gboolean shown = _row_wanted(props, (dt_canvas_prop_id_t)prop_id);
    if(binding->prop->pair_with != DT_CANVAS_PROP_NONE && props->bindings[binding->prop->pair_with].row == binding->row)
      shown = shown || _row_wanted(props, binding->prop->pair_with);
    row_shown[prop_id] = shown;
    if(gtk_widget_get_visible(binding->row) != shown) gtk_widget_set_visible(binding->row, shown);
  }

  for(int section_id = 0; section_id < DT_CANVAS_SECTION_COUNT; section_id++)
  {
    props_section_t *section = &props->sections[section_id];
    gboolean essential = FALSE;
    gboolean more = FALSE;
    gboolean any_row_applies = FALSE;
    for(int prop_id = DT_CANVAS_PROP_NONE + 1; prop_id < DT_CANVAS_PROP_COUNT; prop_id++)
    {
      const props_binding_t *binding = &props->bindings[prop_id];
      if(IS_NULL_PTR(binding->prop) || binding->in_strip || (int)binding->prop->section != section_id) continue;
      // A section is here because one of its rows applies to this object, not because its kind owns
      // rows in the table: a section whose every row a shape or a switch has closed has nothing left
      // to show, and a heading over nothing is a heading that lies.
      if(props->applies[prop_id]) any_row_applies = TRUE;
      if(!row_shown[prop_id]) continue;
      if(binding->prop->tier == DT_CANVAS_TIER_MORE)
        more = TRUE;
      else
        essential = TRUE;
    }
    // A section with nothing to show is absent, never greyed out.
    section->present = any_row_applies;
    gtk_widget_set_visible(section->essential_box, essential);
    gtk_widget_set_visible(section->more_box, more);
    // The rule is drawn above the extras only when something sits above it.
    if(essential)
      dt_gui_add_class(section->more_box, "dt_canvas_more");
    else
      dt_gui_remove_class(section->more_box, "dt_canvas_more");
    gtk_widget_set_visible(section->collapsible.expander, any_row_applies);
    if(kind_changed)
    {
      gchar *title = g_strdup(_(dt_canvas_prop_section_label((dt_canvas_prop_section_t)section_id, props->kind)));
      dt_capitalize_label(title);
      gtk_label_set_text(GTK_LABEL(section->collapsible.label), title);
      dt_free(title);
      const dt_canvas_prop_group_t group
          = dt_canvas_prop_section_group((dt_canvas_prop_section_t)section_id, props->kind);
      gtk_widget_set_visible(section->own, group != DT_CANVAS_GROUP_NONE);
    }
  }

  if(kind_changed)
  {
    _fold_all(props);
  }
  else if(retarget && props->open_section >= 0)
  {
    // The next object's section stays open only where the table says an open section may.
    props_section_t *open = &props->sections[props->open_section];
    if(!open->present || !dt_canvas_prop_section_stays_open(canvas, object, open->section)) _fold_all(props);
  }
}

/** What a closed section holds, read without opening it. */
static void _section_summary(const dt_canvas_t *canvas, const dt_canvas_object_t *object, props_section_t *section)
{
  char line[DT_CANVAS_PROP_TEXT_LEN] = { 0 };
  dt_canvas_prop_section_summary(canvas, object, section->section, line, sizeof(line));
  if(g_strcmp0(gtk_label_get_text(GTK_LABEL(section->summary)), line) != 0)
    gtk_label_set_text(GTK_LABEL(section->summary), line);
}

static void _feature_add(const char *feature_tag, const char *label, const char *hint, const gboolean is_on,
                         gpointer user_data)
{
  props_binding_t *binding = (props_binding_t *)user_data;
  GtkWidget *check = gtk_check_button_new_with_label(label);
  _no_focus_on_click(binding->owner, check);
  gtk_widget_set_tooltip_text(check, hint);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), is_on);
  g_object_set_data_full(G_OBJECT(check), PROPS_FEATURE_TAG_KEY, g_strdup(feature_tag), dt_free_gpointer);
  const gulong handler = g_signal_connect(check, "toggled", G_CALLBACK(_feature_toggled), binding);
  g_object_set_data(G_OBJECT(check), PROPS_FEATURE_HANDLER_KEY, GSIZE_TO_POINTER(handler));
  gtk_flow_box_insert(GTK_FLOW_BOX(binding->widget), check, -1);
}

/**
 * The face a font description names, without its size: which features a font offers is the face's
 * business, and a description carries the size the text is set at too, which a size spin changes
 * twenty times a second while its button is held.
 */
static void _face_of(const char *font, char *face, const size_t length)
{
  PangoFontDescription *description = pango_font_description_from_string(IS_NULL_PTR(font) ? "" : font);
  pango_font_description_unset_fields(description, PANGO_FONT_MASK_SIZE);
  gchar *spelled = pango_font_description_to_string(description);
  g_strlcpy(face, spelled, length);
  dt_free(spelled);
  pango_font_description_free(description);
}

/**
 * The features the frame's face offers, listed again only when the face changes: ticking a box
 * must not destroy the box being ticked. The checks are keyed on the tag, never on a position.
 */
static void _fill_features(props_t *props, props_binding_t *binding, const dt_canvas_t *canvas,
                           const dt_canvas_object_t *object)
{
  dt_canvas_prop_value_t font;
  dt_canvas_prop_read(canvas, object, DT_CANVAS_PROP_TEXT_FONT, &font);
  char face[DT_CANVAS_PROP_TEXT_LEN] = { 0 };
  _face_of(font.text, face, sizeof(face));
  dt_canvas_prop_value_t features;
  dt_canvas_prop_read(canvas, object, DT_CANVAS_PROP_TEXT_FEATURES, &features);
  g_strlcpy(props->features, features.text, sizeof(props->features));
  GtkWidget *flow = binding->widget;
  if(g_strcmp0(props->face, face) != 0)
  {
    g_strlcpy(props->face, face, sizeof(props->face));
    GList *children = gtk_container_get_children(GTK_CONTAINER(flow));
    for(GList *child = children; !IS_NULL_PTR(child); child = g_list_next(child))
      gtk_widget_destroy(GTK_WIDGET(child->data));
    g_list_free(children);
    const uint32_t offered = dt_canvas_props_text_features(canvas, object, _feature_add, binding);
    dt_gui_flow_box_as_layout(GTK_FLOW_BOX(flow));
    gtk_widget_show_all(flow);
    gtk_widget_set_visible(props->features_empty, offered == 0);
    return;
  }
  GList *children = gtk_container_get_children(GTK_CONTAINER(flow));
  for(GList *child = children; !IS_NULL_PTR(child); child = g_list_next(child))
  {
    GtkWidget *check = gtk_bin_get_child(GTK_BIN(child->data));
    const char *feature_tag = IS_NULL_PTR(check) ? NULL : g_object_get_data(G_OBJECT(check), PROPS_FEATURE_TAG_KEY);
    if(IS_NULL_PTR(feature_tag)) continue;
    const gboolean is_on = dt_canvas_text_feature_is_on(props->features, feature_tag);
    if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check)) == is_on) continue;
    const gulong handler = (gulong)GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(check), PROPS_FEATURE_HANDLER_KEY));
    g_signal_handler_block(check, handler);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), is_on);
    g_signal_handler_unblock(check, handler);
  }
  g_list_free(children);
}

/** Write one property's value into its control, its handlers blocked, skipping a value already shown. */
static void _fill_binding(props_t *props, props_binding_t *binding, const dt_canvas_t *canvas,
                          const dt_canvas_object_t *object)
{
  const dt_canvas_prop_t *prop = binding->prop;
  dt_canvas_prop_value_t value;
  dt_canvas_prop_read(canvas, object, prop->id, &value);
  if(!IS_NULL_PTR(props->host.view_value)) props->host.view_value(props->host.data, prop->id, &value);
  GtkWidget *widget = binding->widget;
  switch(prop->widget)
  {
    case DT_CANVAS_WIDGET_TUNE:
    {
      if(prop->group != DT_CANVAS_GROUP_NONE)
      {
        // Inside an override group, a double click resets to the canvas's value: written while
        // inheriting it changes nothing, so the reset keeps the object inheriting.
        dt_canvas_prop_value_t inherited;
        dt_canvas_prop_read_inherited(canvas, object, prop->id, &inherited);
        dt_bauhaus_slider_set_default(widget, (float)inherited.number);
      }
      if(!_shown_equal(prop, dt_bauhaus_slider_get(widget), value.number))
      {
        dt_gui_widget_freeze();
        g_signal_handler_block(widget, binding->handler);
        dt_bauhaus_slider_set(widget, (float)value.number);
        g_signal_handler_unblock(widget, binding->handler);
      }
      binding->rest_number = dt_bauhaus_slider_get(widget);
      break;
    }
    case DT_CANVAS_WIDGET_CHOICE:
    {
      if(dt_bauhaus_combobox_get(widget) == value.choice) break;
      dt_gui_widget_freeze();
      g_signal_handler_block(widget, binding->handler);
      dt_bauhaus_combobox_set(widget, value.choice);
      g_signal_handler_unblock(widget, binding->handler);
      break;
    }
    case DT_CANVAS_WIDGET_MEASURE:
      if(!_shown_equal(prop, gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget)), value.number))
      {
        g_signal_handler_block(widget, binding->handler);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), value.number);
        g_signal_handler_unblock(widget, binding->handler);
      }
      binding->rest_number = gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget));
      break;
    case DT_CANVAS_WIDGET_ICONS:
      for(int choice = 0; choice < binding->toggle_count; choice++)
      {
        const gboolean active = choice == value.choice;
        GtkToggleButton *toggle = GTK_TOGGLE_BUTTON(binding->toggles[choice]);
        if(gtk_toggle_button_get_active(toggle) == active) continue;
        g_signal_handler_block(toggle, binding->toggle_handlers[choice]);
        gtk_toggle_button_set_active(toggle, active);
        g_signal_handler_unblock(toggle, binding->toggle_handlers[choice]);
      }
      break;
    case DT_CANVAS_WIDGET_ICON_FLAG:
    case DT_CANVAS_WIDGET_FLAG:
      if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)) == (value.flag != FALSE)) break;
      g_signal_handler_block(widget, binding->handler);
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), value.flag != FALSE);
      g_signal_handler_unblock(widget, binding->handler);
      break;
    case DT_CANVAS_WIDGET_COLOR:
    {
      // The chooser reports nothing when told what to show.
      const GdkRGBA rgba = { value.color.red, value.color.green, value.color.blue, value.color.alpha };
      dt_chooser_button_set_color(widget, &rgba);
      break;
    }
    case DT_CANVAS_WIDGET_FONT:
    {
      if(g_strcmp0(dt_chooser_button_get_font(widget), value.text) != 0) dt_chooser_button_set_font(widget, value.text);
      // A font the frame takes from the canvas reads dim and slanted: the canvas's, not the frame's.
      const gboolean inherits = dt_canvas_group_state(canvas, object, prop->group) == DT_CANVAS_OWN_INHERIT;
      if(inherits)
        dt_gui_add_class(widget, "dt_canvas_inherited");
      else
        dt_gui_remove_class(widget, "dt_canvas_inherited");
      break;
    }
    case DT_CANVAS_WIDGET_FEATURES:
      _fill_features(props, binding, canvas, object);
      break;
    case DT_CANVAS_WIDGET_INFO:
      if(g_strcmp0(gtk_label_get_text(GTK_LABEL(widget)), value.text) == 0) break;
      gtk_label_set_text(GTK_LABEL(widget), value.text);
      gtk_widget_set_tooltip_text(widget, value.text);
      break;
    default:
      break;
  }
  const gboolean sensitive = dt_canvas_prop_sensitive(prop, canvas, object);
  if(gtk_widget_get_sensitive(widget) != sensitive) gtk_widget_set_sensitive(widget, sensitive);
}

void dt_canvas_props_gtk_refill(dt_canvas_props_gtk_t *props, const dt_canvas_t *canvas,
                                const dt_canvas_object_t *object)
{
  if(IS_NULL_PTR(props) || props->closing) return;
  if(IS_NULL_PTR(object))
  {
    if(props->has_target) dt_canvas_props_gtk_close_dialogs(props);
    _drop_live(props);
    props->has_target = FALSE;
    props->object_id = 0;
    props->structure_known = FALSE;
    return;
  }
  const gboolean retarget = !props->has_target || props->object_id != object->id;
  const gboolean kind_changed = !props->has_target || props->kind != object->kind || !props->structure_known;
  // A dialog left open for the previous object would write its pick to this one.
  if(retarget && props->has_target) dt_canvas_props_gtk_close_dialogs(props);
  if(retarget) _drop_live(props);
  props->has_target = TRUE;
  props->object_id = object->id;
  props->kind = object->kind;

  gboolean structure_changed = retarget || kind_changed;
  for(int prop_id = DT_CANVAS_PROP_NONE + 1; prop_id < DT_CANVAS_PROP_COUNT; prop_id++)
  {
    const props_binding_t *binding = &props->bindings[prop_id];
    const uint8_t applies = IS_NULL_PTR(binding->prop) ? 0 : (uint8_t)dt_canvas_prop_applies(binding->prop, object);
    if(applies != props->applies[prop_id]) structure_changed = TRUE;
    props->applies[prop_id] = applies;
  }
  if(structure_changed || !props->structure_known)
  {
    _structure_pass(props, canvas, object, retarget, kind_changed);
    props->structure_known = TRUE;
  }

  for(int prop_id = DT_CANVAS_PROP_NONE + 1; prop_id < DT_CANVAS_PROP_COUNT; prop_id++)
  {
    props_binding_t *binding = &props->bindings[prop_id];
    if(IS_NULL_PTR(binding->widget) || !props->applies[prop_id]) continue;
    // What the user is holding keeps what they gave it; its partner still follows the document.
    if(_binding_busy(binding)) continue;
    _fill_binding(props, binding, canvas, object);
  }

  // What a session opened from here on starts from: whether a double click's reset gives a group back.
  for(int group = DT_CANVAS_GROUP_NONE + 1; group < DT_CANVAS_GROUP_COUNT; group++)
    props->group_inherits[group]
        = dt_canvas_group_state(canvas, object, (dt_canvas_prop_group_t)group) == DT_CANVAS_OWN_INHERIT;

  for(int section_id = 0; section_id < DT_CANVAS_SECTION_COUNT; section_id++)
  {
    props_section_t *section = &props->sections[section_id];
    if(!section->present) continue;
    _section_summary(canvas, object, section);
    const dt_canvas_prop_group_t group = dt_canvas_prop_section_group(section->section, props->kind);
    if(group == DT_CANVAS_GROUP_NONE) continue;
    const dt_canvas_own_state_t state = dt_canvas_group_state(canvas, object, group);
    const gboolean own = state != DT_CANVAS_OWN_INHERIT;
    if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(section->own)) != own)
    {
      g_signal_handler_block(section->own, section->own_handler);
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(section->own), own);
      g_signal_handler_unblock(section->own, section->own_handler);
    }
    // On, but still what the kind is born with: lit dimly, since nobody chose those values.
    if(state == DT_CANVAS_OWN_KIND_DEFAULT)
      dt_gui_add_class(section->own, "dt_canvas_inherited");
    else
      dt_gui_remove_class(section->own, "dt_canvas_inherited");
  }

  if(dt_canvas_props_card_altered(canvas, object))
    dt_gui_add_class(props->card_toggle, "dt_canvas_altered");
  else
    dt_gui_remove_class(props->card_toggle, "dt_canvas_altered");
  gtk_widget_queue_draw(props->root);
}

/* --- measuring and placing ----------------------------------------------------------------------- */

static void _css_box(GtkWidget *widget, GtkBorder *box)
{
  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  const GtkStateFlags state = gtk_widget_get_state_flags(widget);
  GtkBorder border;
  GtkBorder padding;
  gtk_style_context_get_border(context, state, &border);
  gtk_style_context_get_padding(context, state, &padding);
  box->left = border.left + padding.left;
  box->right = border.right + padding.right;
  box->top = border.top + padding.top;
  box->bottom = border.bottom + padding.bottom;
}

/**
 * Pin the font button's label to one width whichever way it reads. A font the frame takes from the
 * canvas is set in italic, and an italic face has other character widths, so a label a fixed number
 * of characters wide came out 242 px while inheriting and 256 px once owned (measured at 192 dpi):
 * the strip changed width with a value, and a placement solved for one object missed the next one's.
 *
 * The width is taken from the font's metrics, upright and italic, the wider of the two -- the same
 * per-character width GtkLabel sizes itself with. Measuring the button in both looks instead does
 * not work: a class toggled and measured in one call reads the style GTK has not recomputed yet.
 */
static void _settle_font_width(props_binding_t *binding)
{
  GtkWidget *label = dt_chooser_button_get_label(binding->widget);
  if(IS_NULL_PTR(label)) return;
  PangoContext *context = gtk_widget_get_pango_context(label);
  PangoFontDescription *font = NULL;
  gtk_style_context_get(gtk_widget_get_style_context(label), GTK_STATE_FLAG_NORMAL, "font", &font, NULL);
  if(IS_NULL_PTR(font)) return;
  int widest_character = 0;
  const PangoStyle styles[2] = { PANGO_STYLE_NORMAL, PANGO_STYLE_ITALIC };
  for(int idx = 0; idx < 2; idx++)
  {
    PangoFontDescription *face = pango_font_description_copy(font);
    pango_font_description_set_style(face, styles[idx]);
    PangoFontMetrics *metrics = pango_context_get_metrics(context, face, pango_context_get_language(context));
    const int character = MAX(pango_font_metrics_get_approximate_char_width(metrics),
                              pango_font_metrics_get_approximate_digit_width(metrics));
    widest_character = MAX(widest_character, character);
    pango_font_metrics_unref(metrics);
    pango_font_description_free(face);
  }
  pango_font_description_free(font);
  // No width in characters any more, and nothing natural past the ellipsis: the request is the width.
  gtk_label_set_width_chars(GTK_LABEL(label), -1);
  gtk_label_set_max_width_chars(GTK_LABEL(label), 0);
  gtk_widget_set_size_request(label, PANGO_PIXELS_CEIL(widest_character * PROPS_FONT_CHARS), -1);
}

/**
 * Make the strip as tall as the tallest control any kind can put in it, so every kind's strip is
 * one height and a solved placement holds for the next object. A hidden widget reports no size,
 * so each row is shown for the time it is measured -- once, until the theme changes. The font
 * button's width is pinned in the same pass, for the same reason.
 */
static void _settle_strip_size(props_t *props)
{
  if(props->strip_strut_height > 0) return;
  for(int prop_id = DT_CANVAS_PROP_NONE + 1; prop_id < DT_CANVAS_PROP_COUNT; prop_id++)
  {
    props_binding_t *binding = &props->bindings[prop_id];
    if(binding->in_strip && !IS_NULL_PTR(binding->row) && binding->prop->widget == DT_CANVAS_WIDGET_FONT)
      _settle_font_width(binding);
  }
  int tallest = 0;
  for(int prop_id = DT_CANVAS_PROP_NONE + 1; prop_id < DT_CANVAS_PROP_COUNT; prop_id++)
  {
    props_binding_t *binding = &props->bindings[prop_id];
    if(!binding->in_strip || IS_NULL_PTR(binding->row)) continue;
    const gboolean visible = gtk_widget_get_visible(binding->row);
    if(!visible) gtk_widget_show(binding->row);
    int minimum = 0;
    int natural = 0;
    gtk_widget_get_preferred_height(binding->row, &minimum, &natural);
    if(!visible) gtk_widget_hide(binding->row);
    tallest = MAX(tallest, MAX(minimum, natural));
  }
  GtkWidget *trail[3] = { props->content_button, props->card_toggle, props->close_button };
  for(int idx = 0; idx < 3; idx++)
  {
    const gboolean visible = gtk_widget_get_visible(trail[idx]);
    if(!visible) gtk_widget_show(trail[idx]);
    int minimum = 0;
    int natural = 0;
    gtk_widget_get_preferred_height(trail[idx], &minimum, &natural);
    if(!visible) gtk_widget_hide(trail[idx]);
    tallest = MAX(tallest, MAX(minimum, natural));
  }
  props->strip_strut_height = MAX(tallest, 1);
  gtk_widget_set_size_request(props->strip_strut, 0, props->strip_strut_height);
}

void dt_canvas_props_gtk_measure(dt_canvas_props_gtk_t *props, const int width, int *strip_height,
                                 int *card_content_height)
{
  if(IS_NULL_PTR(props) || props->closing) return;
  _settle_strip_size(props);
  GtkBorder frame_box;
  _css_box(props->frame, &frame_box);
  const int inner_width = MAX(width - frame_box.left - frame_box.right, 1);
  int minimum = 0;
  int natural = 0;
  gtk_widget_get_preferred_height_for_width(props->strip, inner_width, &minimum, &natural);
  if(!IS_NULL_PTR(strip_height)) *strip_height = MAX(minimum, natural) + frame_box.top + frame_box.bottom;

  if(IS_NULL_PTR(card_content_height)) return;
  // The card's whole height, uncapped: its content at this width, plus the boxes around it. Asked of
  // the content rather than of the scrolled window, whose natural height is already capped.
  GtkBorder card_box;
  _css_box(props->card, &card_box);
  GtkWidget *viewport = gtk_bin_get_child(GTK_BIN(props->card));
  GtkBorder viewport_box = { 0, 0, 0, 0 };
  if(!IS_NULL_PTR(viewport)) _css_box(viewport, &viewport_box);
  const int content_width
      = MAX(inner_width - card_box.left - card_box.right - viewport_box.left - viewport_box.right, 1);
  gtk_widget_get_preferred_height_for_width(props->sections_box, content_width, &minimum, &natural);
  *card_content_height
      = MAX(minimum, natural) + card_box.top + card_box.bottom + viewport_box.top + viewport_box.bottom;
}

int dt_canvas_props_gtk_strip_width(dt_canvas_props_gtk_t *props)
{
  if(IS_NULL_PTR(props) || props->closing) return 0;
  _settle_strip_size(props);
  GtkBorder frame_box;
  _css_box(props->frame, &frame_box);
  int minimum = 0;
  int natural = 0;
  gtk_widget_get_preferred_width(props->strip, &minimum, &natural);
  const int strip = MAX(minimum, natural) + frame_box.left + frame_box.right;
  return MAX(strip, (int)DT_PIXEL_APPLY_DPI(PROPS_MIN_WIDTH_PIXELS));
}

/**
 * The least height the card can be drawn at, whatever cap it is given: its own boxes, and a scrollbar
 * if a theme or a policy ever gives it one. Asked of the card shown, since a hidden widget reports no
 * size at all.
 */
static int _card_minimum_height(props_t *props)
{
  const gboolean visible = gtk_widget_get_visible(props->card);
  if(!visible) gtk_widget_show(props->card);
  int minimum = 0;
  int natural = 0;
  gtk_widget_get_preferred_height(props->card, &minimum, &natural);
  if(!visible) gtk_widget_hide(props->card);
  return minimum;
}

void dt_canvas_props_gtk_set_card(dt_canvas_props_gtk_t *props, const gboolean shown, const gboolean grow_up,
                                  const int max_height, const gboolean clipped)
{
  if(IS_NULL_PTR(props) || props->closing) return;
  if(props->grow_up != grow_up)
  {
    props->grow_up = grow_up;
    gtk_box_reorder_child(GTK_BOX(props->frame), props->card, grow_up ? 0 : 1);
  }
  // The arrow points at the side the card is on.
  dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(props->card_toggle), dtgtk_cairo_paint_solid_arrow,
                               grow_up ? CPF_DIRECTION_UP : CPF_DIRECTION_DOWN, NULL);
  // Given less than its own least, a card would come out taller than the room it was given, over
  // whatever the host kept that room clear of: it is not shown, and says it had no room.
  const gboolean fits = !shown || max_height >= _card_minimum_height(props);
  const gboolean card_shown = shown && fits;
  if(clipped || !fits)
  {
    dt_gui_add_class(props->card_toggle, "dt_canvas_clipped");
    gtk_widget_set_tooltip_text(props->card_toggle,
                                _("No room for the card here: zoom out, or click to show it anyway"));
  }
  else
  {
    dt_gui_remove_class(props->card_toggle, "dt_canvas_clipped");
    gtk_widget_set_tooltip_text(props->card_toggle, _("All the properties"));
  }
  if(card_shown)
  {
    // The height given counts the card's own boxes. The cap applies to the viewport inside them,
    // whose natural height already counts its own box, so that one is not taken off again.
    GtkBorder card_box;
    _css_box(props->card, &card_box);
    const int content = MAX(max_height - card_box.top - card_box.bottom, 1);
    if(content != props->card_max_content)
    {
      props->card_max_content = content;
      gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(props->card), content);
    }
  }
  if(gtk_widget_get_visible(props->card) != card_shown) gtk_widget_set_visible(props->card, card_shown);
  // The button shows what is on screen, so its next click asks for the other: a card the host hid
  // on its own -- every open showing the strip alone, a clipped card -- is opened by one click, not two.
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(props->card_toggle)) != card_shown)
  {
    g_signal_handler_block(props->card_toggle, props->card_toggle_handler);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(props->card_toggle), card_shown);
    g_signal_handler_unblock(props->card_toggle, props->card_toggle_handler);
  }
  gtk_widget_queue_draw(props->root);
}

void dt_canvas_props_gtk_set_section(dt_canvas_props_gtk_t *props, const int section_index)
{
  if(IS_NULL_PTR(props) || props->closing) return;
  if(section_index < 0 || section_index >= DT_CANVAS_SECTION_COUNT)
  {
    _fold_all(props);
    return;
  }
  props_section_t *section = &props->sections[section_index];
  if(!section->present) return;
  if(!dt_canvas_prop_section_opens_by_itself(section->section, props->kind)) return;
  for(int other = 0; other < DT_CANVAS_SECTION_COUNT; other++)
  {
    props_section_t *other_section = &props->sections[other];
    g_signal_handler_block(other_section->collapsible.toggle, other_section->toggle_handler);
    dt_gui_collapsible_section_set_expanded(&other_section->collapsible, other == section_index);
    g_signal_handler_unblock(other_section->collapsible.toggle, other_section->toggle_handler);
  }
  props->open_section = section_index;
}

gboolean dt_canvas_props_gtk_card_open(dt_canvas_props_gtk_t *props)
{
  if(IS_NULL_PTR(props) || props->closing) return FALSE;
  return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(props->card_toggle));
}

gboolean dt_canvas_props_gtk_typing(dt_canvas_props_gtk_t *props)
{
  if(IS_NULL_PTR(props) || props->closing) return FALSE;
  for(int prop_id = DT_CANVAS_PROP_NONE + 1; prop_id < DT_CANVAS_PROP_COUNT; prop_id++)
  {
    if(props->bindings[prop_id].typing) return TRUE;
  }
  return FALSE;
}

void dt_canvas_props_gtk_commit(dt_canvas_props_gtk_t *props)
{
  if(IS_NULL_PTR(props) || props->closing) return;
  _apply_typing(props);
  // A number pasted rather than typed is not tracked as typing, and is applied as the focus leaving
  // would apply it: as a step, whose session is committed just below.
  GtkWidget *toplevel = gtk_widget_get_toplevel(props->root);
  GtkWidget *focus = GTK_IS_WINDOW(toplevel) ? gtk_window_get_focus(GTK_WINDOW(toplevel)) : NULL;
  if(!IS_NULL_PTR(focus) && GTK_IS_SPIN_BUTTON(focus) && gtk_widget_is_ancestor(focus, props->root))
    gtk_spin_button_update(GTK_SPIN_BUTTON(focus));
  // Removes the timer with the session: left armed, it would send the control's value later, onto
  // whatever the document has become by then -- an undo, a frame dragged by its corner.
  _commit_live(props);
}

void dt_canvas_props_gtk_forget(dt_canvas_props_gtk_t *props)
{
  if(IS_NULL_PTR(props) || props->closing) return;
  _drop_live(props);
}

gboolean dt_canvas_props_gtk_focus_inside(dt_canvas_props_gtk_t *props)
{
  if(IS_NULL_PTR(props) || props->closing) return FALSE;
  GtkWidget *toplevel = gtk_widget_get_toplevel(props->root);
  if(!GTK_IS_WINDOW(toplevel)) return FALSE;
  GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(toplevel));
  if(IS_NULL_PTR(focus)) return FALSE;
  return focus == props->root || gtk_widget_is_ancestor(focus, props->root);
}

void dt_canvas_props_gtk_focus_first(dt_canvas_props_gtk_t *props)
{
  if(IS_NULL_PTR(props) || props->closing) return;
  for(int prop_id = DT_CANVAS_PROP_NONE + 1; prop_id < DT_CANVAS_PROP_COUNT; prop_id++)
  {
    props_binding_t *binding = &props->bindings[prop_id];
    if(!binding->in_strip || IS_NULL_PTR(binding->row) || !gtk_widget_get_visible(binding->row)) continue;
    GtkWidget *target = binding->prop->widget == DT_CANVAS_WIDGET_ICONS && binding->toggle_count > 0
                            ? binding->toggles[0]
                            : binding->widget;
    if(!gtk_widget_get_can_focus(target) || !gtk_widget_is_sensitive(target)) continue;
    gtk_widget_grab_focus(target);
    return;
  }
  gtk_widget_grab_focus(props->card_toggle);
}

void dt_canvas_props_gtk_close_dialogs(dt_canvas_props_gtk_t *props)
{
  if(IS_NULL_PTR(props) || props->closing) return;
  props->closing_dialogs = TRUE;
  for(int prop_id = DT_CANVAS_PROP_NONE + 1; prop_id < DT_CANVAS_PROP_COUNT; prop_id++)
  {
    props_binding_t *binding = &props->bindings[prop_id];
    if(IS_NULL_PTR(binding->prop) || IS_NULL_PTR(binding->widget)) continue;
    if(binding->prop->widget != DT_CANVAS_WIDGET_COLOR && binding->prop->widget != DT_CANVAS_WIDGET_FONT) continue;
    dt_chooser_button_close(binding->widget);
  }
  props->closing_dialogs = FALSE;
}

void dt_canvas_props_gtk_free(dt_canvas_props_gtk_t *props)
{
  if(IS_NULL_PTR(props)) return;
  // Destroying the controls emits focus-out and toggles on the way: none of that is the user.
  props->closing = TRUE;
  _debounce_remove(props);
  // Destroying the root closes every dialog its buttons opened, and closes it without raising a
  // window, which may be going too. Already destroyed -- its overlay went first -- there is nothing
  // left of it but the reference this struct holds.
  if(!props->destroyed) gtk_widget_destroy(props->root);
  g_signal_handlers_disconnect_by_data(props->root, props);
  g_object_unref(props->root);
  dt_free(props);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
