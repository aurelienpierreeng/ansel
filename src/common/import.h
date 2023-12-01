/*
    This file is part of Ansel.
    Copyright (C) 2023 Aurélien Pierre.

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

#include "gui/gtk.h"

typedef enum exif_fields_t {
  EXIF_DATETIME_FIELD = 0,
  EXIF_SEPARATOR1_FIELD,
  EXIF_MODEL_FIELD,
  EXIF_MAKER_FIELD,
  EXIF_LENS_FIELD,
  EXIF_FOCAL_LENS_FIELD,
  EXIF_SEPARATOR2_FIELD,
  EXIF_EXPOSURE_FIELD,
  EXIF_SEPARATOR3_FIELD,
  EXIF_INLIB_FIELD,
  EXIF_LAST_FIELD
} exif_fields_t;


typedef struct dt_lib_import_t
{
  GtkWidget *file_chooser;
  GtkWidget *preview;
  GtkWidget *exif;

  GtkWidget *exif_info[EXIF_LAST_FIELD];
  GtkWidget *datetime;
  GtkWidget *dialog;
} dt_lib_import_t;


/** Open the image importer popup **/
void dt_images_import();

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
