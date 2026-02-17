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
#pragma once
#include <gtk/gtk.h>

G_BEGIN_DECLS

#define DTGTK_TYPE_CELL_RENDERER_BUTTON (dtgtk_cell_renderer_button_get_type())
G_DECLARE_FINAL_TYPE(DtGtkCellRendererButton, dtgtk_cell_renderer_button, DTGTK, CELL_RENDERER_BUTTON,
                     GtkCellRendererPixbuf)

/* convenience constructor */
GtkCellRenderer *dtgtk_cell_renderer_button_new(void);

G_END_DECLS
