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

/* A spin button whose value is points and whose text is a length with a unit.
 *
 * Driven through the widget rather than around it: the text is set and gtk_spin_button_update()
 * is called, which is what Return, an arrow and a focus-out all reach, and the VALUE is read
 * back the way every consumer reads it. Nothing here calls the signal handlers itself.
 *
 * It needs a display and skips without one (77). */

#include "widgets/length_field.h"

#include "widgets/widget_settings.h"

#include <glib.h>
#include <gtk/gtk.h>
#include <math.h>
#include <setjmp.h>  // NOLINT(misc-include-cleaner): cmocka requires it ahead of its own header
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <cmocka.h>

/* The host's string storage, which is where a field's unit is remembered: widgets/ keeps no
 * preferences of its own, so the test plays the host. */
static GHashTable *_stored = NULL;

static gchar *_get_string(const char *key)
{
  const char *value = _stored != NULL ? g_hash_table_lookup(_stored, key) : NULL;
  return value != NULL ? g_strdup(value) : NULL;
}

static void _set_string(const char *key, const char *value)
{
  if(_stored == NULL) _stored = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  g_hash_table_insert(_stored, g_strdup(key), g_strdup(value));
}

static void _pump(void)
{
  while(gtk_events_pending()) gtk_main_iteration();
}

/** A field in a window, since a spin button not in one does not lay out or emit. */
static GtkWidget *_field(GtkWidget **window, const char *key, const char *unit, const int digits)
{
  *window = gtk_offscreen_window_new();
  GtkWidget *field = dt_length_field_new(key, unit, digits, -1000.0, 10000.0, 1.0);
  gtk_container_add(GTK_CONTAINER(*window), field);
  gtk_widget_show_all(*window);
  _pump();
  return field;
}

/** What a person typing does: put it in the entry, then commit it. */
static void _type(GtkWidget *field, const char *text)
{
  gtk_entry_set_text(GTK_ENTRY(field), text);
  _pump();
  gtk_spin_button_update(GTK_SPIN_BUTTON(field));
  _pump();
}

static double _value(GtkWidget *field)
{
  return gtk_spin_button_get_value(GTK_SPIN_BUTTON(field));
}

/**
 * What the field shows, against a number and a unit.
 *
 * Built with the same printf rather than compared with a literal: gtk_init() puts the process
 * in the person's own locale, so the decimal separator here is theirs -- a comma across most of
 * Europe -- and that IS what a field should show, since it is also what they will type back.
 */
static void _assert_shows(GtkWidget *field, const int digits, const double amount, const char *unit)
{
  gchar *wanted = g_strdup_printf("%.*f %s", digits, amount, unit);
  const char *shown = gtk_entry_get_text(GTK_ENTRY(field));
  if(g_strcmp0(shown, wanted) != 0)
  {
    print_error("the field shows \"%s\", wanted \"%s\"\n", shown, wanted);
    g_free(wanted);
    fail();
  }
  g_free(wanted);
}

static void _assert_close(const double got, const double wanted, const double tolerance, const char *what)
{
  if(fabs(got - wanted) > tolerance)
  {
    print_error("%s: got %.6f, wanted %.6f\n", what, got, wanted);
    fail();
  }
}

static void _a_field_starts_in_a_unit_it_can_parse(void **state)
{
  (void)state;
  GtkWidget *window = NULL;
  // What the caller asked for.
  GtkWidget *field = _field(&window, NULL, "mm", 0);
  assert_string_equal(dt_length_field_get_unit(field), "mm");
  gtk_widget_destroy(window);

  // Nothing asked for is points.
  field = _field(&window, NULL, NULL, 0);
  assert_string_equal(dt_length_field_get_unit(field), "pt");
  gtk_widget_destroy(window);

  /* And a fallback that is not a unit is points, never itself: dt_length_parse() refuses a
   * fallback it does not know, so a field keeping one would refuse every bare number typed
   * into it -- which reads as a field that has stopped taking numbers at all. */
  field = _field(&window, NULL, "furlong", 0);
  assert_string_equal(dt_length_field_get_unit(field), "pt");
  _type(field, "12");
  _assert_close(_value(field), 12.0, 1e-9, "a bare number in a field with a bad fallback");
  gtk_widget_destroy(window);

  // An alias starts in the spelling it is printed in.
  field = _field(&window, NULL, "inch", 0);
  assert_string_equal(dt_length_field_get_unit(field), "in");
  gtk_widget_destroy(window);
}

static void _a_typed_unit_is_read_and_then_kept(void **state)
{
  (void)state;
  GtkWidget *window = NULL;
  GtkWidget *field = _field(&window, NULL, "pt", 0);

  _type(field, "210mm");
  _assert_close(_value(field), 210.0 * 72.0 / 25.4, 1e-6, "210mm");
  assert_string_equal(dt_length_field_get_unit(field), "mm");
  // And the text comes back in that unit, with enough figures to reach a point.
  _assert_shows(field, 1, 210.0, "mm");

  // A BARE number now means millimetres, which is the whole point of remembering.
  _type(field, "100");
  _assert_close(_value(field), 100.0 * 72.0 / 25.4, 1e-6, "100 after 210mm");

  // Until another unit is typed.
  _type(field, "1 in");
  _assert_close(_value(field), 72.0, 1e-9, "1 in");
  assert_string_equal(dt_length_field_get_unit(field), "in");

  gtk_widget_destroy(window);
}

static void _nonsense_leaves_the_length_alone(void **state)
{
  (void)state;
  /*
   * The trap this widget exists around. GTK_INPUT_ERROR is documented as making the spin
   * restore the value it held; MEASURED, it sets the value to zero -- a field holding 42 came
   * back 0 -- so the handler answers with the value it already has instead.
   */
  GtkWidget *window = NULL;
  GtkWidget *field = _field(&window, NULL, "pt", 0);
  _type(field, "42");
  _assert_close(_value(field), 42.0, 1e-9, "42");

  static const char *nonsense[] = { "not a number", "", " ", "mm", "12 furlongs", "nan", "inf" };
  for(guint at = 0; at < G_N_ELEMENTS(nonsense); at++)
  {
    _type(field, nonsense[at]);
    if(fabs(_value(field) - 42.0) > 1e-9)
    {
      print_error("\"%s\" left the field at %.6f\n", nonsense[at], _value(field));
      fail();
    }
    // And the text is put back to what the field holds, rather than left as the nonsense.
    _assert_shows(field, 0, 42.0, "pt");
  }
  gtk_widget_destroy(window);
}

static void _a_field_shows_enough_figures_for_its_unit_and_its_caller(void **state)
{
  (void)state;
  GtkWidget *window = NULL;
  // Points with no decimals asked for: whole points, since a point is the unit of account.
  GtkWidget *field = _field(&window, NULL, "pt", 0);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(field), 612.0);
  _pump();
  _assert_shows(field, 0, 612.0, "pt");
  gtk_widget_destroy(window);

  // A caller that keeps tenths of a point gets them, whatever the unit would settle for.
  field = _field(&window, NULL, "pt", 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(field), 0.5);
  _pump();
  _assert_shows(field, 1, 0.5, "pt");
  gtk_widget_destroy(window);

  // An inch needs three to say which point it means, even from a caller that asked for none.
  field = _field(&window, NULL, "in", 0);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(field), 612.0);
  _pump();
  _assert_shows(field, 3, 8.5, "in");
  gtk_widget_destroy(window);
}

static void _setting_the_unit_moves_the_words_and_not_the_length(void **state)
{
  (void)state;
  GtkWidget *window = NULL;
  GtkWidget *field = _field(&window, NULL, "pt", 0);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(field), 595.2755905511812);
  _pump();
  const double before = _value(field);

  dt_length_field_set_unit(field, "mm");
  _pump();
  assert_string_equal(dt_length_field_get_unit(field), "mm");
  _assert_shows(field, 1, 210.0, "mm");
  _assert_close(_value(field), before, 0.0, "the length after the unit changed");

  // A name that is not a unit is ignored rather than obeyed.
  dt_length_field_set_unit(field, "furlong");
  _pump();
  assert_string_equal(dt_length_field_get_unit(field), "mm");
  gtk_widget_destroy(window);
}

static void _a_field_comes_back_in_the_unit_it_was_left_in(void **state)
{
  (void)state;
  /* The remembering, through the host's own storage -- widgets/ keeps no preferences. */
  GtkWidget *window = NULL;
  GtkWidget *field = _field(&window, "canvas/test/margin", "pt", 0);
  assert_string_equal(dt_length_field_get_unit(field), "pt");
  _type(field, "21cm");
  assert_string_equal(dt_length_field_get_unit(field), "cm");
  gtk_widget_destroy(window);

  // A second field on the same key opens where the first was left.
  field = _field(&window, "canvas/test/margin", "pt", 0);
  assert_string_equal(dt_length_field_get_unit(field), "cm");
  gtk_widget_destroy(window);

  // A different key is a different field and is untouched by it.
  field = _field(&window, "canvas/test/bleed", "pt", 0);
  assert_string_equal(dt_length_field_get_unit(field), "pt");
  gtk_widget_destroy(window);

  // And what was remembered is sanitised, never trusted: a key holding rubbish is points.
  _set_string("canvas/test/rubbish", "furlong");
  field = _field(&window, "canvas/test/rubbish", "mm", 0);
  assert_string_equal(dt_length_field_get_unit(field), "mm");
  gtk_widget_destroy(window);
}

static void _the_unit_lives_in_the_text_and_never_in_the_number(void **state)
{
  (void)state;
  /* Every consumer goes on reading and writing points, which is what lets this widget be
   * dropped in where a plain spin button was without touching one handler. */
  GtkWidget *window = NULL;
  GtkWidget *field = _field(&window, NULL, "mm", 0);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(field), 72.0);
  _pump();
  _assert_close(_value(field), 72.0, 1e-9, "what was set in points");
  _assert_shows(field, 1, 25.4, "mm");
  _assert_close(gtk_adjustment_get_value(gtk_spin_button_get_adjustment(GTK_SPIN_BUTTON(field))), 72.0, 1e-9,
                "the adjustment");
  gtk_widget_destroy(window);
}

int main(int argc, char **argv)
{
  if(!gtk_init_check(&argc, &argv)) return 77;
  dt_widget_set_string_storage_handlers(_get_string, _set_string);
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_a_field_starts_in_a_unit_it_can_parse),
    cmocka_unit_test(_a_typed_unit_is_read_and_then_kept),
    cmocka_unit_test(_nonsense_leaves_the_length_alone),
    cmocka_unit_test(_a_field_shows_enough_figures_for_its_unit_and_its_caller),
    cmocka_unit_test(_setting_the_unit_moves_the_words_and_not_the_length),
    cmocka_unit_test(_a_field_comes_back_in_the_unit_it_was_left_in),
    cmocka_unit_test(_the_unit_lives_in_the_text_and_never_in_the_number),
  };
  const int failures = cmocka_run_group_tests(tests, NULL, NULL);
  if(_stored != NULL) g_hash_table_destroy(_stored);
  return failures;
}
