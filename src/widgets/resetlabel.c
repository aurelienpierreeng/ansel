/*
    This file is part of darktable,
    Copyright (C) 2010 Bruce Guenter.
    Copyright (C) 2010-2011 johannes hanika.
    Copyright (C) 2011 Antony Dovgal.
    Copyright (C) 2011, 2014, 2016 Tobias Ellinghaus.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2013 Roman Lebedev.
    Copyright (C) 2020 Diederik Ter Rahe.
    Copyright (C) 2020 Pascal Obry.
    Copyright (C) 2021 Victor Forsiuk.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2026 Aurélien PIERRE.
    
    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    
    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "widgets/resetlabel.h"

#include <glib/gi18n.h>

static void _reset_label_class_init(GtkDarktableResetLabelClass *klass);
static void _reset_label_init(GtkDarktableResetLabel *label);

static guint _reset_signal = 0;

static void _reset_label_class_init(GtkDarktableResetLabelClass *klass)
{
  // The widget announces the double-click; the consumer decides what resetting means.
  _reset_signal = g_signal_new("reset", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                               g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0);
}

static void _reset_label_init(GtkDarktableResetLabel *label)
{
}

static gboolean _reset_label_callback(GtkDarktableResetLabel *label, GdkEventButton *event, gpointer user_data)
{
  if(event->type == GDK_2BUTTON_PRESS)
  {
    g_signal_emit(label, _reset_signal, 0);
    return TRUE;
  }
  return FALSE;
}

// public functions
GtkWidget *dtgtk_reset_label_new(const gchar *text)
{
  GtkDarktableResetLabel *label;
  label = g_object_new(dtgtk_reset_label_get_type(), NULL);

  label->lb = GTK_LABEL(gtk_label_new(text));
  gtk_widget_set_halign(GTK_WIDGET(label->lb), GTK_ALIGN_START);
  gtk_label_set_ellipsize(GTK_LABEL(label->lb), PANGO_ELLIPSIZE_END);
  gtk_event_box_set_visible_window(GTK_EVENT_BOX(label), FALSE);
  gtk_widget_set_tooltip_text(GTK_WIDGET(label), _("double-click to reset"));
  gtk_container_add(GTK_CONTAINER(label), GTK_WIDGET(label->lb));
  gtk_widget_add_events(GTK_WIDGET(label), GDK_BUTTON_PRESS_MASK);
  g_signal_connect(G_OBJECT(label), "button-press-event", G_CALLBACK(_reset_label_callback), (gpointer)NULL);

  return (GtkWidget *)label;
}

GType dtgtk_reset_label_get_type()
{
  static GType dtgtk_reset_label_type = 0;
  if(!dtgtk_reset_label_type)
  {
    static const GTypeInfo dtgtk_reset_label_info = {
      sizeof(GtkDarktableResetLabelClass), (GBaseInitFunc)NULL, (GBaseFinalizeFunc)NULL,
      (GClassInitFunc)_reset_label_class_init, NULL, /* class_finalize */
      NULL,                                          /* class_data */
      sizeof(GtkDarktableResetLabel), 0,             /* n_preallocs */
      (GInstanceInitFunc)_reset_label_init,
    };
    dtgtk_reset_label_type
        = g_type_register_static(GTK_TYPE_EVENT_BOX, "GtkDarktableResetLabel", &dtgtk_reset_label_info, 0);
  }
  return dtgtk_reset_label_type;
}

void dtgtk_reset_label_set_text(GtkDarktableResetLabel *label, const gchar *str)
{
  gtk_label_set_text(label->lb, str);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

