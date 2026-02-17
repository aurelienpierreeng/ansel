/*
    This file is part of the Ansel project.
    Copyright (C) 2025 Aurélien PIERRE.
    
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
#include "icon_cell_renderer.h"

struct _DtGtkCellRendererButton
{
  GtkCellRendererPixbuf parent_instance; /* mandatory first member */
                                         /* add private data here if you need it */
};

enum
{
  DTGTK_RENDERER_BUTTON_ACTIVATE,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

static gboolean dtgtk_cell_renderer_button_activate(GtkCellRenderer *cell, GdkEvent *event, GtkWidget *widget,
                                                    const gchar *path, const GdkRectangle *bg,
                                                    const GdkRectangle *area, GtkCellRendererState flags)
{
  gboolean handled = FALSE;

  g_signal_emit(cell, signals[DTGTK_RENDERER_BUTTON_ACTIVATE], 0, /* detail */
                event, widget, path, bg, area, flags, &handled);  /* return value from handlers */

  /* propagate TRUE/FALSE back to GTK    */
  return handled;
}

G_DEFINE_TYPE(DtGtkCellRendererButton, dtgtk_cell_renderer_button, GTK_TYPE_CELL_RENDERER_PIXBUF)

static void dtgtk_cell_renderer_button_class_init(DtGtkCellRendererButtonClass *klass)
{
  GtkCellRendererClass *cell_class = GTK_CELL_RENDERER_CLASS(klass);
  cell_class->activate = dtgtk_cell_renderer_button_activate;

  signals[DTGTK_RENDERER_BUTTON_ACTIVATE]
      = g_signal_new("activate",                                                    /* signal name   */
                     G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION, /* act-like      */
                     0,                                                             /* class_offset  */
                     NULL, NULL, NULL,                                              /* no accumulator*/
                     G_TYPE_BOOLEAN,                                                /* return type   */
                     6,                                                             /* n_params      */
                     GDK_TYPE_EVENT,                                                /* event         */
                     GTK_TYPE_WIDGET,                                               /* widget        */
                     G_TYPE_STRING,                                                 /* path          */
                     GDK_TYPE_RECTANGLE,                                            /* background    */
                     GDK_TYPE_RECTANGLE,                                            /* cell_area     */
                     GTK_TYPE_CELL_RENDERER_STATE);                                 /* flags enum    */
}

static void dtgtk_cell_renderer_button_init(DtGtkCellRendererButton *self)
{
  g_object_set(self, "mode", GTK_CELL_RENDERER_MODE_ACTIVATABLE, NULL);
}

GtkCellRenderer *dtgtk_cell_renderer_button_new(void)
{
  return g_object_new(DTGTK_TYPE_CELL_RENDERER_BUTTON, NULL);
}
