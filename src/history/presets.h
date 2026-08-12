/*
    This file is part of darktable,
    Copyright (C) 2010-2011 Henrik Andersson.
    Copyright (C) 2010, 2012 johannes hanika.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2013, 2019-2020 Pascal Obry.
    Copyright (C) 2014-2015 Jérémy Rosen.
    Copyright (C) 2014, 2016 Tobias Ellinghaus.
    Copyright (C) 2016 Roman Lebedev.
    Copyright (C) 2021 Aldric Renaudin.
    Copyright (C) 2022 Martin Bařinka.
    
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

#ifndef DT_HISTORY_PRESETS_H
#define DT_HISTORY_PRESETS_H

#include <glib.h>

/**
 * @brief Which image kinds a preset applies to. Persisted in the presets database
 * (`database/preset_repository.h` documents its `format` and `excluded` row fields in
 * terms of these), matched against the image at auto-apply time
 * (`develop/dev_history.c`), and set by each module's preset registration.
 *
 * @details The FOR_NOT_ variants are negated to keep existing presets valid. Lived in
 * `gui/presets.h` historically, but nothing about it is GUI: it is preset vocabulary,
 * written to disk.
 */
typedef enum dt_presets_format_flag_t
{
  FOR_LDR = 1 << 0,
  FOR_RAW = 1 << 1,
  FOR_HDR = 1 << 2,
  FOR_NOT_MONO = 1 << 3,
  FOR_NOT_COLOR = 1 << 4
} dt_presets_format_flag_t;

/** save preset to file */
void dt_presets_save_to_file(const int rowid, const char *preset_name, const char *filedir);

/** load preset from file */
int dt_presets_import_from_file(const char *preset_path);

// does the module support autoapplying presets ?
gboolean dt_presets_module_can_autoapply(const gchar *operation);

/**
 * @brief Answers whether the panel named @p operation allows its presets to auto-apply.
 *
 * @details Only the side of the application that owns the panels can know. This used to be
 * answered inline by walking `dt_lib_get_global()->plugins` -- a call from here into
 * `libs/` (layer 7) for one boolean. With no resolver installed the answer is TRUE, which
 * is what the loop returned for an operation matching no panel, and so what ansel-cli and
 * the unit tests already got.
 */
typedef gboolean (*dt_presets_autoapply_resolver_t)(const gchar *operation);
void dt_presets_set_autoapply_resolver(dt_presets_autoapply_resolver_t resolver);

#endif // DT_HISTORY_PRESETS_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

