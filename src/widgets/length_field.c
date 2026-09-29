/*
    This file is part of Ansel,
    Copyright (C) 2026 - Aurélien PIERRE.

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

#include "widgets/length_field.h"

#include "common/length.h"
#include "widgets/widget_settings.h"

#include <glib.h>
#include <glib/gi18n.h>

#define DT_LENGTH_FIELD_DATA "dt-length-field"

typedef struct dt_length_field_t
{
  const char *unit;      ///< into the static table in common/length.c, so never owned
  gchar *unit_key;       ///< where it is remembered, or NULL
  int digits;            ///< what the caller asked to see, whatever the unit needs on top
  GtkWidget *chooser;    ///< the combo naming the unit, or NULL; weak, it is the field's sibling
  gulong chooser_handler; ///< blocked while the field writes into it, so it never answers back
} dt_length_field_t;

static void _field_free(gpointer data)
{
  dt_length_field_t *field = (dt_length_field_t *)data;
  if(field == NULL) return;
  /*
   * The weak pointer is a live write permission GObject holds on these bytes, and it must be
   * withdrawn BEFORE they go back to the allocator -- a combo destroyed after this struct would
   * otherwise have GLib write NULL into freed memory, which lands on the allocator's own
   * bookkeeping and kills the process in an innocent caller much later.
   */
  if(field->chooser != NULL) g_object_remove_weak_pointer(G_OBJECT(field->chooser), (gpointer *)&field->chooser);
  g_free(field->unit_key);
  g_free(field);
}

static dt_length_field_t *_field_of(GtkWidget *widget)
{
  if(!GTK_IS_SPIN_BUTTON(widget)) return NULL;
  return (dt_length_field_t *)g_object_get_data(G_OBJECT(widget), DT_LENGTH_FIELD_DATA);
}

/**
 * Adopt a unit: remember it, and tell the combo if there is one.
 *
 * The one place `field->unit` is written, so a unit TYPED into the field and a unit PICKED from
 * the combo cannot disagree. The combo's own handler is blocked while it is written to, the way
 * every refill in this tree blocks by stored id rather than by a flag the handler reads.
 */
static void _adopt_unit(dt_length_field_t *field, const char *unit)
{
  if(field == NULL || unit == NULL || unit == field->unit) return;
  field->unit = unit;
  if(field->unit_key != NULL) dt_widget_store_string(field->unit_key, field->unit);
  if(field->chooser == NULL) return;
  g_signal_handler_block(field->chooser, field->chooser_handler);
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(field->chooser), field->unit);
  g_signal_handler_unblock(field->chooser, field->chooser_handler);
}

/** As many figures as the caller asked for, or as many as the unit needs to reach a point. */
static int _shown_digits(const dt_length_field_t *field)
{
  return MAX(field->digits, dt_length_unit_digits(field->unit));
}

/** The one place the text is written, so the unit's spelling cannot drift between the two. */
static void _render(GtkSpinButton *spin, const dt_length_field_t *field)
{
  char text[64];
  dt_length_format(gtk_spin_button_get_value(spin), field->unit, _shown_digits(field), text, sizeof(text));
  gtk_entry_set_text(GTK_ENTRY(spin), text);
}

/**
 * What was typed, as a length in points.
 *
 * MEASURED, because nothing else in this tree connects this signal and two readings of it were
 * wrong: it does NOT fire per keystroke -- typing three characters fired it not once -- so it
 * cannot fight a caller that is watching the text as it is typed. It fires from
 * `gtk_spin_button_update()`, which Return, the arrows and a focus-out reach.
 *
 * And it NEVER answers `GTK_INPUT_ERROR`, whatever nonsense it is handed. That is documented as
 * making the spin restore the value it held; measured, it sets the value to ZERO -- a field
 * holding 42 pt, given "not a number", came back 0. Keeping what the field already has and
 * reporting success is the behaviour that was wanted, and it is `dt_length_parse()`'s own
 * contract: it leaves the length alone unless it read one.
 */
static gint _on_input(GtkSpinButton *spin, gdouble *value, gpointer data)
{
  (void)data;
  dt_length_field_t *field = _field_of(GTK_WIDGET(spin));
  if(field == NULL) return FALSE;
  double points = 0.0;
  const char *used = NULL;
  if(!dt_length_parse(gtk_entry_get_text(GTK_ENTRY(spin)), field->unit, &points, &used))
  {
    *value = gtk_spin_button_get_value(spin);
    return TRUE;
  }
  // A unit typed in is the unit this field speaks from now on, combo and all.
  if(used != NULL) _adopt_unit(field, used);
  *value = points;
  return TRUE;
}

static gboolean _on_output(GtkSpinButton *spin, gpointer data)
{
  (void)data;
  const dt_length_field_t *field = _field_of(GTK_WIDGET(spin));
  if(field == NULL) return FALSE;
  _render(spin, field);
  return TRUE;
}

/**
 * Commit what is in the entry when the focus leaves it.
 *
 * GtkSpinButton is documented to do this itself, and this could NOT be measured here to say
 * whether it does: an offscreen window -- the only kind these checks may open -- delivers no
 * focus event at all, so the handler fired zero times in a probe that connected it. Asking for
 * the update outright covers both answers, and costs a second parse of text that has already
 * been parsed to the same number when GTK has done it first.
 */
static gboolean _on_focus_out(GtkWidget *widget, GdkEvent *event, gpointer data)
{
  (void)event;
  (void)data;
  if(GTK_IS_SPIN_BUTTON(widget)) gtk_spin_button_update(GTK_SPIN_BUTTON(widget));
  return FALSE;
}

GtkWidget *dt_length_field_new(const char *unit_key, const char *unit, const int digits, const double minimum,
                               const double maximum, const double step)
{
  GtkWidget *spin = gtk_spin_button_new_with_range(minimum, maximum, step);
  dt_length_field_t *field = g_malloc0(sizeof(dt_length_field_t));
  field->digits = MAX(digits, 0);
  field->unit_key = g_strdup(unit_key);

  /*
   * What it was left in, then what the caller asked for, then points. Each is sanitised
   * through the table rather than trusted: a stored string was written by whatever wrote it,
   * and a fallback that is not a unit makes dt_length_parse() refuse every bare number typed
   * into the field -- which would read as a field that has stopped accepting numbers.
   */
  if(field->unit_key != NULL)
  {
    gchar *remembered = dt_widget_stored_string(field->unit_key);
    field->unit = dt_length_unit_canonical(remembered);
    g_free(remembered);
  }
  if(field->unit == NULL) field->unit = dt_length_unit_canonical(unit);
  if(field->unit == NULL) field->unit = dt_length_unit_canonical("pt");

  g_object_set_data_full(G_OBJECT(spin), DT_LENGTH_FIELD_DATA, field, _field_free);
  // Without this a unit typed into it is refused before this code ever sees it.
  gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(spin), FALSE);
  /*
   * Measured: this rounds nothing -- 595.2756 comes back whole at every setting -- so it is the
   * text GTK would have written by itself, which `output` replaces anyway. It is set so the
   * widget agrees with itself for anything that reads it.
   */
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin), _shown_digits(field));
  g_signal_connect(spin, "input", G_CALLBACK(_on_input), NULL);
  g_signal_connect(spin, "output", G_CALLBACK(_on_output), NULL);
  g_signal_connect(spin, "focus-out-event", G_CALLBACK(_on_focus_out), NULL);
  _render(GTK_SPIN_BUTTON(spin), field);
  return spin;
}

/** The combo answered: this field speaks that unit from now on. */
static void _chooser_changed(GtkComboBox *combo, gpointer data)
{
  GtkWidget *widget = GTK_WIDGET(data);
  const char *chosen = gtk_combo_box_get_active_id(combo);
  if(chosen != NULL) dt_length_field_set_unit(widget, chosen);
}

GtkWidget *dt_length_field_unit_chooser(GtkWidget *widget)
{
  dt_length_field_t *field = _field_of(widget);
  if(field == NULL) return NULL;
  GtkWidget *combo = gtk_combo_box_text_new();
  /*
   * The units the PARSER knows, so a name that can be typed can also be picked and the two
   * cannot drift apart. The aliases are skipped -- "inch" and the double prime are both spelled
   * "in" -- and the order is by SIZE, smallest first, which is a fact of the table rather than a
   * second list to keep in step with it.
   */
  size_t count = 0;
  const dt_length_unit_t *units = dt_length_units(&count);
  const dt_length_unit_t *order[16];
  size_t listed = 0;
  for(size_t at = 0; at < count && listed < G_N_ELEMENTS(order); at++)
    if(units[at].canonical == NULL) order[listed++] = &units[at];
  for(size_t a = 0; a + 1 < listed; a++)
    for(size_t b = a + 1; b < listed; b++)
      if(order[b]->points < order[a]->points)
      {
        const dt_length_unit_t *swap = order[a];
        order[a] = order[b];
        order[b] = swap;
      }
  for(size_t at = 0; at < listed; at++)
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), order[at]->name, order[at]->name);
  gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), field->unit);
  gtk_widget_set_tooltip_text(combo, _("The unit this length is shown in. The value does not change."));
  // The field keeps no reference: the two are built together and packed side by side, and the
  // combo outliving the field it writes to is the one arrangement this must not be used in.
  field->chooser = combo;
  field->chooser_handler = g_signal_connect(combo, "changed", G_CALLBACK(_chooser_changed), widget);
  g_object_add_weak_pointer(G_OBJECT(combo), (gpointer *)&field->chooser);
  return combo;
}

const char *dt_length_field_get_unit(GtkWidget *widget)
{
  const dt_length_field_t *field = _field_of(widget);
  return field != NULL ? field->unit : NULL;
}

void dt_length_field_set_unit(GtkWidget *widget, const char *unit)
{
  dt_length_field_t *field = _field_of(widget);
  if(field == NULL) return;
  const char *wanted = dt_length_unit_canonical(unit);
  if(wanted == NULL || wanted == field->unit) return;
  _adopt_unit(field, wanted);
  /*
   * NOT gtk_spin_button_set_digits() here, though the figures shown do change with the unit:
   * MEASURED, it emits `value-changed` whenever the count differs, with the value untouched.
   * A control's `value-changed` is an EDIT to whatever it is wired to -- and the guides' margin
   * and bleed share one setter that sends BOTH spins -- so choosing centimetres would write the
   * document, take an undo step and recomposite the page for a change of words. `_render()`
   * takes the figures from `_shown_digits()` itself, so the display is right either way, and
   * what set_digits buys is GTK's own agreement about text this field replaces anyway.
   */
  // The length has not moved, only the words for it.
  _render(GTK_SPIN_BUTTON(widget), field);
}
