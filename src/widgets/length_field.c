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

#define DT_LENGTH_FIELD_DATA "dt-length-field"

typedef struct dt_length_field_t
{
  const char *unit;  ///< into the static table in common/length.c, so never owned
  gchar *unit_key;   ///< where it is remembered, or NULL
  int digits;        ///< what the caller asked to see, whatever the unit needs on top
} dt_length_field_t;

static void _field_free(gpointer data)
{
  dt_length_field_t *field = (dt_length_field_t *)data;
  if(field == NULL) return;
  g_free(field->unit_key);
  g_free(field);
}

static dt_length_field_t *_field_of(GtkWidget *widget)
{
  if(!GTK_IS_SPIN_BUTTON(widget)) return NULL;
  return (dt_length_field_t *)g_object_get_data(G_OBJECT(widget), DT_LENGTH_FIELD_DATA);
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
  // A unit typed in is the unit this field speaks from now on.
  if(used != NULL && used != field->unit)
  {
    field->unit = used;
    if(field->unit_key != NULL) dt_widget_store_string(field->unit_key, field->unit);
  }
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
  field->unit = wanted;
  if(field->unit_key != NULL) dt_widget_store_string(field->unit_key, field->unit);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(widget), _shown_digits(field));
  // The length has not moved, only the words for it.
  _render(GTK_SPIN_BUTTON(widget), field);
}
