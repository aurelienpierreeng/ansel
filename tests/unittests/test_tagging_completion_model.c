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

#include "testdb.h"

#include "libs/tagging_completion.h"
#include "metadata/tags.h"

static gboolean _store_contains(GtkTreeModel *model, const char *path)
{
  GtkTreeIter iter;
  gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
  while(valid)
  {
    char *candidate = NULL;
    gtk_tree_model_get(model, &iter, 0, &candidate, -1);
    const gboolean found = !g_strcmp0(candidate, path);
    g_free(candidate);
    if(found) return TRUE;
    valid = gtk_tree_model_iter_next(model, &iter);
  }
  return FALSE;
}

static void test_refresh_exposes_new_hierarchical_tag(void **state)
{
  (void)state;
  GtkListStore *store = gtk_list_store_new(1, G_TYPE_STRING);
  guint tagid = 0;

  assert_true(dt_tag_new("subjects|animals", &tagid));
  dt_tagging_completion_refresh(store);
  assert_true(_store_contains(GTK_TREE_MODEL(store), "subjects|animals"));

  assert_true(dt_tag_new("subjects|animals|beetles", &tagid));
  assert_false(_store_contains(GTK_TREE_MODEL(store), "subjects|animals|beetles"));

  dt_tagging_completion_refresh(store);
  assert_true(_store_contains(GTK_TREE_MODEL(store), "subjects|animals|beetles"));
  g_object_unref(store);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_refresh_exposes_new_hierarchical_tag),
  };
  return cmocka_run_group_tests(tests, testdb_setup, testdb_teardown);
}
