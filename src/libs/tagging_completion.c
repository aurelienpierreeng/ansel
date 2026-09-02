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

void dt_tagging_entry_clear_icon(GtkEntry *entry, const GtkEntryIconPosition position,
                                 GdkEvent *event, gpointer user_data)
{
  if(position != GTK_ENTRY_ICON_SECONDARY) return;

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
  gtk_widget_grab_focus(GTK_WIDGET(entry));
}
