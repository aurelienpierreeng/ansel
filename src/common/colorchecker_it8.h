/*
    This file is part of darktable,
    Copyright (C) 2020 darktable developers.

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

#include <glib.h>

/**
 * @brief Check if the IT8 file is valid and compatible with Ansel
 * 
 * @param filename The path to the IT8 file
 * @return gboolean TRUE if the file is valid, FALSE otherwise
 */
gboolean dt_colorchecker_it8_valid(const cmsHANDLE hIT8);

/**
 * @brief Create a color checker from an IT8 file to be used in Color Calibration
 * 
 * @param filename path to the IT8 file
 * @return NULL if error, otherwise dt_color_checker_t* the color checker structure filled with the data from the IT8 file
 */
dt_color_checker_t *dt_colorchecker_it8_create(const char *filename);

void dt_colorchecker_it8_cleanup(dt_color_checker_t *checker);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
