/*
    This file is part of Ansel,
    Copyright (C) 2026 Paolo SANTUCCI.

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

#include "libs/tagging_completion.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>  // NOLINT(misc-include-cleaner)
#include <stdint.h>
#include <cmocka.h>

typedef struct dt_tagging_completion_test_t
{
  GtkWidget *window;
  GtkEntry *entry;
  GtkListStore *store;
  GtkEntryCompletion *completion;
} dt_tagging_completion_test_t;

typedef struct dt_return_handler_state_t
{
  guint entry_calls;
  gchar *text;
} dt_return_handler_state_t;

static void _append_completion(dt_tagging_completion_test_t *test, const char *text)
{
  GtkTreeIter iter;
  gtk_list_store_append(test->store, &iter);
  gtk_list_store_set(test->store, &iter, 0, text, -1);
}

static void _drain_gtk_events()
{
  while(gtk_events_pending()) gtk_main_iteration();
}

static gboolean _quit_main_loop(GMainLoop *loop)
{
  g_main_loop_quit(loop);
  return G_SOURCE_REMOVE;
}

static void _wait_for_completion_sources()
{
  GMainLoop *loop = g_main_loop_new(NULL, FALSE);
  g_timeout_add(250, (GSourceFunc)_quit_main_loop, loop);
  g_main_loop_run(loop);
  g_main_loop_unref(loop);
}

static gboolean _completion_popup_is_mapped(GtkWidget *entry_window)
{
  gboolean mapped = FALSE;
  GList *toplevels = gtk_window_list_toplevels();
  for(GList *toplevel = toplevels; toplevel; toplevel = g_list_next(toplevel))
  {
    GtkWidget *widget = GTK_WIDGET(toplevel->data);
    if(widget != entry_window && gtk_widget_get_mapped(widget)) mapped = TRUE;
  }
  g_list_free(toplevels);
  return mapped;
}

static gboolean _send_key(GtkWidget *widget, const guint keyval)
{
  GdkEventKey event = { 0 };
  event.type = GDK_KEY_PRESS;
  event.window = g_object_ref(gtk_widget_get_window(widget));
  event.send_event = TRUE;
  event.time = GDK_CURRENT_TIME;
  event.keyval = keyval;
  gboolean handled = FALSE;
  g_signal_emit_by_name(widget, "key-press-event", &event, &handled);
  g_object_unref(event.window);
  return handled;
}

static gboolean _match_last_hierarchical_token(GtkEntryCompletion *completion, const gchar *key,
                                               GtkTreeIter *iter, gpointer user_data)
{
  gchar *candidate = NULL;
  gtk_tree_model_get(gtk_entry_completion_get_model(completion), iter, 0, &candidate, -1);
  const gchar *token = g_strrstr(key, ",");
  token = token ? token + 1 : key;
  while(*token == ' ') token++;
  const gboolean matches = g_str_has_prefix(candidate, token);
  g_free(candidate);
  return matches;
}

static int _setup_completion(void **state)
{
  dt_tagging_completion_test_t *test = g_malloc0(sizeof(*test));
  test->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  test->entry = GTK_ENTRY(gtk_entry_new());
  test->store = gtk_list_store_new(1, G_TYPE_STRING);
  _append_completion(test, "subjects|animals");
  _append_completion(test, "places|france|paris");
  test->completion = gtk_entry_completion_new();
  gtk_entry_completion_set_model(test->completion, GTK_TREE_MODEL(test->store));
  gtk_entry_completion_set_text_column(test->completion, 0);
  gtk_entry_completion_set_inline_completion(test->completion, TRUE);
  gtk_entry_completion_set_popup_completion(test->completion, TRUE);
  gtk_entry_completion_set_minimum_key_length(test->completion, 1);
  gtk_entry_completion_set_match_func(test->completion, _match_last_hierarchical_token, NULL, NULL);
  g_signal_connect(test->completion, "match-selected",
                   G_CALLBACK(dt_tagging_completion_match_selected), NULL);
  gtk_entry_set_completion(test->entry, test->completion);
  gtk_container_add(GTK_CONTAINER(test->window), GTK_WIDGET(test->entry));
  gtk_widget_show_all(test->window);
  gtk_window_set_focus(GTK_WINDOW(test->window), GTK_WIDGET(test->entry));
  gtk_window_present(GTK_WINDOW(test->window));
  gdk_window_focus(gtk_widget_get_window(test->window), GDK_CURRENT_TIME);
  _wait_for_completion_sources();
  GdkEvent *focus_event = gdk_event_new(GDK_FOCUS_CHANGE);
  focus_event->focus_change.window = g_object_ref(gtk_widget_get_window(GTK_WIDGET(test->entry)));
  focus_event->focus_change.in = TRUE;
  gtk_widget_send_focus_change(GTK_WIDGET(test->entry), focus_event);
  gdk_event_free(focus_event);
  assert_true(gtk_widget_has_focus(GTK_WIDGET(test->entry)));
  *state = test;
  return 0;
}

static int _teardown_completion(void **state)
{
  dt_tagging_completion_test_t *test = *state;
  gtk_entry_set_completion(test->entry, NULL);
  gtk_widget_destroy(test->window);
  _wait_for_completion_sources();
  g_object_unref(test->completion);
  g_object_unref(test->store);
  g_free(test);
  return 0;
}

static gboolean _record_plain_return(GtkWidget *entry, GdkEventKey *event, dt_return_handler_state_t *state)
{
  if(event->keyval != GDK_KEY_Return) return FALSE;
  state->entry_calls++;
  g_free(state->text);
  state->text = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry)));
  return FALSE;
}

static void test_clear_cancels_completion_before_model_refresh(void **state)
{
  dt_tagging_completion_test_t *test = *state;

  gtk_entry_set_text(test->entry, "subjects");
  dt_tagging_entry_clear(test->entry);
  gtk_list_store_clear(test->store);
  _append_completion(test, "subjects|animals");
  _append_completion(test, "subjects|architecture");
  _wait_for_completion_sources();

  assert_string_equal(gtk_entry_get_text(test->entry), "");
  assert_ptr_equal(gtk_entry_get_completion(test->entry), test->completion);
  assert_false(_completion_popup_is_mapped(test->window));
}

static void test_completion_survives_repeated_single_and_hierarchical_clears(void **state)
{
  dt_tagging_completion_test_t *test = *state;

  gtk_entry_set_text(test->entry, "subjects");
  dt_tagging_entry_clear(test->entry);
  _wait_for_completion_sources();
  assert_string_equal(gtk_entry_get_text(test->entry), "");
  assert_false(_completion_popup_is_mapped(test->window));

  gtk_entry_set_text(test->entry, "subjects|animals, places");
  dt_tagging_entry_clear(test->entry);
  _wait_for_completion_sources();
  assert_string_equal(gtk_entry_get_text(test->entry), "");
  assert_false(_completion_popup_is_mapped(test->window));

  gint position = 0;
  gtk_editable_insert_text(GTK_EDITABLE(test->entry), "places", -1, &position);
  _wait_for_completion_sources();
  gtk_entry_completion_insert_prefix(test->completion);
  _drain_gtk_events();

  assert_ptr_equal(gtk_entry_get_completion(test->entry), test->completion);
  assert_string_equal(gtk_entry_get_text(test->entry), "places|france|paris");
}

static void test_clear_supports_entry_without_completion(void **state)
{
  GtkEntry *entry = GTK_ENTRY(gtk_entry_new());
  gtk_entry_set_text(entry, "places|france");

  dt_tagging_entry_clear(entry);

  assert_string_equal(gtk_entry_get_text(entry), "");
  assert_null(gtk_entry_get_completion(entry));
  gtk_widget_destroy(GTK_WIDGET(entry));
}

static void test_selected_completion_replaces_only_the_active_token(void **state)
{
  dt_tagging_completion_test_t *test = *state;
  gtk_entry_completion_set_inline_completion(test->completion, FALSE);
  gtk_entry_set_text(test->entry, "subjects|animals, pla");
  gtk_editable_set_position(GTK_EDITABLE(test->entry), -1);
  gtk_entry_completion_set_inline_completion(test->completion, TRUE);
  GtkTreeIter iter;
  assert_true(gtk_tree_model_get_iter_first(GTK_TREE_MODEL(test->store), &iter));
  assert_true(gtk_tree_model_iter_next(GTK_TREE_MODEL(test->store), &iter));

  gboolean handled = FALSE;
  g_signal_emit_by_name(test->completion, "match-selected",
                        GTK_TREE_MODEL(test->store), &iter, &handled);
  _wait_for_completion_sources();

  assert_true(handled);
  assert_string_equal(gtk_entry_get_text(test->entry),
                      "subjects|animals,places|france|paris");
  assert_true(gtk_entry_completion_get_inline_completion(test->completion));
  assert_ptr_equal(gtk_entry_get_completion(test->entry), test->completion);
  assert_false(_completion_popup_is_mapped(test->window));
}

static void test_completion_keeps_return_priority_after_reattach(void **state)
{
  dt_tagging_completion_test_t *test = *state;
  dt_return_handler_state_t handler_state = { 0 };
  if(!gtk_window_is_active(GTK_WINDOW(test->window))) skip();

  gulong handler_id = g_signal_connect(test->entry, "key-press-event",
                                       G_CALLBACK(_record_plain_return), &handler_state);

  g_signal_handler_disconnect(test->entry, handler_id);
  dt_tagging_entry_clear(test->entry);
  handler_id = g_signal_connect(test->entry, "key-press-event",
                                G_CALLBACK(_record_plain_return), &handler_state);
  gint position = 0;
  gtk_editable_insert_text(GTK_EDITABLE(test->entry), "subjects|animals, pla", -1, &position);
  _wait_for_completion_sources();
  if(!_completion_popup_is_mapped(test->window))
  {
    g_signal_handler_disconnect(test->entry, handler_id);
    skip();
  }
  assert_true(_send_key(GTK_WIDGET(test->entry), GDK_KEY_Down));
  assert_true(_send_key(GTK_WIDGET(test->entry), GDK_KEY_Return));
  _wait_for_completion_sources();

  assert_string_equal(gtk_entry_get_text(test->entry),
                      "subjects|animals,places|france|paris");
  assert_int_equal(handler_state.entry_calls, 0);
  assert_false(_completion_popup_is_mapped(test->window));
  assert_true(gtk_entry_completion_get_inline_completion(test->completion));

  _send_key(GTK_WIDGET(test->entry), GDK_KEY_Return);
  assert_int_equal(handler_state.entry_calls, 1);
  assert_string_equal(handler_state.text, "subjects|animals,places|france|paris");

  g_signal_handler_disconnect(test->entry, handler_id);
  g_free(handler_state.text);
}

int main(int argc, char **argv)
{
  if(!gtk_init_check(&argc, &argv)) return 77;
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(test_clear_cancels_completion_before_model_refresh,
                                    _setup_completion, _teardown_completion),
    cmocka_unit_test_setup_teardown(test_completion_survives_repeated_single_and_hierarchical_clears,
                                    _setup_completion, _teardown_completion),
    cmocka_unit_test(test_clear_supports_entry_without_completion),
    cmocka_unit_test_setup_teardown(test_selected_completion_replaces_only_the_active_token,
                                    _setup_completion, _teardown_completion),
    cmocka_unit_test_setup_teardown(test_completion_keeps_return_priority_after_reattach,
                                    _setup_completion, _teardown_completion),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
