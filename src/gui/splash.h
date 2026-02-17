/*
    This file is part of darktable,
    Copyright (C) 2009-2011 johannes hanika.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2016 Roman Lebedev.
    Copyright (C) 2016 Tobias Ellinghaus.
    Copyright (C) 2020 Pascal Obry.
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

#pragma once

#include <stdarg.h>

typedef struct _GtkWidget GtkWidget;

void dt_gui_splash_init(void);
void dt_gui_splash_update(const char *message);
void dt_gui_splash_updatef(const char *format, ...) __attribute__((format(printf, 1, 2)));
void dt_gui_splash_close(void);
void dt_gui_splash_set_transient_for(GtkWidget *parent);
