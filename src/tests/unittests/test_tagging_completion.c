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

static void test_clear_action_cancels_pending_inline_completion(void **state)
{
  (void)state;
  GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  GtkWidget *entry = gtk_entry_new();
  GtkListStore *store = gtk_list_store_new(1, G_TYPE_STRING);
  GtkTreeIter iter;
  gtk_list_store_append(store, &iter);
  gtk_list_store_set(store, &iter, 0, "subjects|animals", -1);
  GtkEntryCompletion *completion = gtk_entry_completion_new();
  gtk_entry_completion_set_model(completion, GTK_TREE_MODEL(store));
  gtk_entry_completion_set_text_column(completion, 0);
  gtk_entry_completion_set_inline_completion(completion, TRUE);
  gtk_entry_completion_set_popup_completion(completion, TRUE);
  gtk_entry_completion_set_minimum_key_length(completion, 1);
  gtk_entry_set_completion(GTK_ENTRY(entry), completion);
  g_signal_connect(entry, "icon-release", G_CALLBACK(dt_tagging_entry_clear_icon), NULL);
  gtk_container_add(GTK_CONTAINER(window), entry);
  gtk_widget_show_all(window);
  gtk_widget_grab_focus(entry);

  gtk_entry_set_text(GTK_ENTRY(entry), "subjects");
  g_signal_emit_by_name(entry, "icon-release", GTK_ENTRY_ICON_SECONDARY, NULL);
  while(gtk_events_pending()) gtk_main_iteration();

  gboolean popup_completion = FALSE, popup_mapped = FALSE;
  g_object_get(completion, "popup-completion", &popup_completion, NULL);
  GList *toplevels = gtk_window_list_toplevels();
  for(GList *toplevel = toplevels; toplevel; toplevel = g_list_next(toplevel))
  {
    GtkWidget *widget = GTK_WIDGET(toplevel->data);
    if(widget != window && gtk_widget_get_mapped(widget)) popup_mapped = TRUE;
  }
  g_list_free(toplevels);
  assert_string_equal(gtk_entry_get_text(GTK_ENTRY(entry)), "");
  assert_true(gtk_widget_is_focus(entry));
  assert_ptr_equal(gtk_entry_get_completion(GTK_ENTRY(entry)), completion);
  assert_true(popup_completion);
  assert_false(popup_mapped);

  gtk_widget_destroy(window);
  g_object_unref(completion);
  g_object_unref(store);
}

int main(int argc, char **argv)
{
  if(!gtk_init_check(&argc, &argv)) return 77;
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_clear_action_cancels_pending_inline_completion),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
