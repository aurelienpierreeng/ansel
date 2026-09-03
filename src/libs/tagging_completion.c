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

#include "metadata/tags.h"
#include "system/macros.h"
#include "system/mem_alloc.h"

void dt_tagging_completion_refresh(GtkListStore *store)
{
  gtk_list_store_clear(store);
  GList *tags = NULL;
  dt_tag_get_with_usage(&tags);
  for(GList *tag_iter = tags; tag_iter; tag_iter = g_list_next(tag_iter))
  {
    const dt_tag_t *tag = (const dt_tag_t *)tag_iter->data;
    if(IS_NULL_PTR(tag->tag)) continue;
    GtkTreeIter store_iter;
    gtk_list_store_append(store, &store_iter);
    gtk_list_store_set(store, &store_iter, 0, tag->tag, -1);
  }
  dt_tag_free_result(&tags);
}

gboolean dt_tagging_completion_match_selected(GtkEntryCompletion *completion, GtkTreeModel *model,
                                              GtkTreeIter *iter, gpointer user_data)
{
  const int column = gtk_entry_completion_get_text_column(completion);
  if(gtk_tree_model_get_column_type(model, column) != G_TYPE_STRING) return TRUE;

  GtkEditable *entry = GTK_EDITABLE(gtk_entry_completion_get_entry(completion));
  if(!GTK_IS_EDITABLE(entry)) return FALSE;

  char *tag = NULL;
  gtk_tree_model_get(model, iter, column, &tag, -1);
  gint cursor_position = gtk_editable_get_position(entry);
  gchar *current_text = gtk_editable_get_chars(entry, 0, -1);
  const gchar *last_tag = g_strrstr(current_text, ",");
  const gint cut_off = last_tag
                         ? (int)(g_utf8_strlen(current_text, -1) - g_utf8_strlen(last_tag, -1)) + 1
                         : 0;
  dt_free(current_text);

  const gboolean inline_completion = gtk_entry_completion_get_inline_completion(completion);
  gtk_entry_completion_set_inline_completion(completion, FALSE);
  gtk_editable_delete_text(entry, cut_off, cursor_position);
  cursor_position = cut_off;
  gtk_editable_insert_text(entry, tag, -1, &cursor_position);
  gtk_editable_set_position(entry, cursor_position);
  gtk_entry_completion_set_inline_completion(completion, inline_completion);
  dt_free(tag);
  return TRUE;
}

void dt_tagging_entry_clear(GtkEntry *entry)
{
  GtkEntryCompletion *completion = gtk_entry_get_completion(entry);
  if(!IS_NULL_PTR(completion))
  {
    g_object_ref(completion);
    gtk_entry_set_completion(entry, NULL);
  }
  gtk_entry_set_text(entry, "");
  if(!IS_NULL_PTR(completion))
  {
    gtk_entry_set_completion(entry, completion);
    g_object_unref(completion);
  }
}
