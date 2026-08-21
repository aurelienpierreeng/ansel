/*
    This file is part of darktable,
    Copyright (C) 2010-2011 Henrik Andersson.
    Copyright (C) 2010, 2012 johannes hanika.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2013, 2019-2020 Pascal Obry.
    Copyright (C) 2014-2015 Jérémy Rosen.
    Copyright (C) 2014, 2016 Tobias Ellinghaus.
    Copyright (C) 2016 Roman Lebedev.
    Copyright (C) 2019 Heiko Bauke.
    Copyright (C) 2019 Philippe Weyland.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2023 Aurélien PIERRE.
    Copyright (C) 2024 Guillaume Stutin.
    
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

#ifndef DT_GUI_IMPORT_H
#define DT_GUI_IMPORT_H

#include <glib.h>

/** Open the image importer popup and process user input **/
struct dt_variables_params_t;
void dt_images_import();

/** @brief Register the GUI-side import handlers (the discarded-files recap dialog). */
void dt_gui_import_init_handlers(void);

/** Mirrors the 3 GtkFileFilter entries built by _file_filters() in gui/import.c: the file-type
 * choice offered by the Import dialog, and reused wherever else the app needs to ask the same
 * question (e.g. importing a dropped folder via drag-and-drop). */
typedef enum dt_import_filter_type_t
{
  DT_IMPORT_FILTER_ALL = 0,
  DT_IMPORT_FILTER_RAW,
  DT_IMPORT_FILTER_RASTER
} dt_import_filter_type_t;

/** TRUE if pathname is a supported image file matching filter_type. The single source of truth
 * for that question -- shared with the Import dialog's own recursive scan -- so a caller adding
 * a second filtered folder-scan (e.g. drag-and-drop) can never drift from what the dialog itself
 * considers a match. */
gboolean dt_import_passes_filter(const dt_import_filter_type_t filter_type, const gchar *pathname);

#endif // DT_GUI_IMPORT_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
