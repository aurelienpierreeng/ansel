/*
 *    This file is part of Ansel,
 *    Copyright (C) 2026 Aurélien PIERRE.
 *
 *    Ansel is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    Ansel is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with Ansel.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "widgets/notebook.h"

#include "system/macros.h"            // IS_NULL_PTR
#include "widgets/widget_style.h"     // dt_capitalize_label
#include "widgets/widget_settings.h"  // DT_PIXEL_APPLY_DPI, dt_widget_notebook_page_changed
#include "widgets/widget_style.h"     // dt_gui_add_class
#include <glib/gi18n.h>
#include "system/mem_alloc.h"

static void _notebook_size_callback(GtkNotebook *notebook, GdkRectangle *allocation, gpointer *data)
{
  const int n = gtk_notebook_get_n_pages(notebook);
  g_return_if_fail(n > 0);

  GtkRequestedSize *sizes = g_malloc_n(n, sizeof(GtkRequestedSize));

  for(int i = 0; i < n; i++)
  {
    sizes[i].data = gtk_notebook_get_tab_label(notebook, gtk_notebook_get_nth_page(notebook, i));
    sizes[i].minimum_size = 0;
    GtkRequisition natural_size;
    gtk_widget_get_preferred_size(sizes[i].data, NULL, &natural_size);
    sizes[i].natural_size = natural_size.width;
  }

  GtkAllocation first, last;
  gtk_widget_get_allocation(sizes[0].data, &first);
  gtk_widget_get_allocation(sizes[n - 1].data, &last);

  const gint total_space = last.x + last.width - first.x; // ignore tab padding; CSS sets padding for label

  if(total_space > 0)
  {
    gtk_distribute_natural_allocation(total_space, n, sizes);

    for(int i = 0; i < n; i++)
      gtk_widget_set_size_request(sizes[i].data, sizes[i].minimum_size, -1);

    gtk_widget_size_allocate(GTK_WIDGET(notebook), allocation);

    for(int i = 0; i < n; i++)
      gtk_widget_set_size_request(sizes[i].data, -1, -1);
  }

  dt_free(sizes);
}

// GTK_STATE_FLAG_PRELIGHT does not seem to get set on the label on hover so
// state-flags-changed cannot update dt_control_get_global()->element for shortcut mapping
static gboolean _notebook_motion_notify_callback(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  GtkAllocation notebook_alloc, label_alloc;
  gtk_widget_get_allocation(widget, &notebook_alloc);

  GtkNotebook *notebook = GTK_NOTEBOOK(widget);
  const int n = gtk_notebook_get_n_pages(notebook);
  for(int i = 0; i < n; i++)
  {
    gtk_widget_get_allocation(gtk_notebook_get_tab_label(notebook, gtk_notebook_get_nth_page(notebook, i)), &label_alloc);
  }

  return FALSE;
}

GtkNotebook *dt_ui_notebook_new()
{
  return GTK_NOTEBOOK(gtk_notebook_new());
}

GtkWidget *dt_ui_notebook_page(GtkNotebook *notebook, const char *text, const char *tooltip)
{
  gchar *text_cpy = g_strdup(_(text));
  dt_capitalize_label(text_cpy);
  GtkWidget *label = gtk_label_new(text_cpy);
  dt_free(text_cpy);
  GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);
  if(strlen(text) > 2)
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_tooltip_text(label, tooltip ? tooltip : _(text));
  gtk_widget_set_has_tooltip(GTK_WIDGET(notebook), FALSE);

  gint page_num = gtk_notebook_append_page(notebook, page, label);
  gtk_container_child_set(GTK_CONTAINER(notebook), page, "tab-expand", TRUE, "tab-fill", TRUE, NULL);
  if(page_num == 1 &&
     !g_signal_handler_find(G_OBJECT(notebook), G_SIGNAL_MATCH_FUNC, 0, 0, NULL, _notebook_size_callback, NULL))
  {
    g_signal_connect(G_OBJECT(notebook), "size-allocate", G_CALLBACK(_notebook_size_callback), NULL);
    g_signal_connect(G_OBJECT(notebook), "motion-notify-event", G_CALLBACK(_notebook_motion_notify_callback), NULL);
  }

  return page;
}

static void _notebook_switch_page_signal_relay(GtkNotebook *notebook, GtkWidget *page, guint page_num,
                                               gpointer user_data)
{
  dt_widget_notebook_page_changed(user_data);
}

void dt_ui_notebook_set_picker_owner(GtkNotebook *notebook, gpointer owner)
{
  if(IS_NULL_PTR(notebook)) return;

  // Guard against connecting twice on the same notebook (e.g. a caller re-entering
  // gui_init after a GUI rebuild); user_data is set per-connection, so re-registering
  // a different owner on an already-relayed notebook would otherwise leak the old
  // connection instead of updating it.
  const gulong existing = g_signal_handler_find(G_OBJECT(notebook), G_SIGNAL_MATCH_FUNC, 0, 0, NULL,
                                                G_CALLBACK(_notebook_switch_page_signal_relay), NULL);
  if(existing) g_signal_handler_disconnect(G_OBJECT(notebook), existing);

  g_signal_connect(G_OBJECT(notebook), "switch_page", G_CALLBACK(_notebook_switch_page_signal_relay), owner);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
