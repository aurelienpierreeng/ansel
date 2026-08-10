/*
    This file is part of darktable,
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

/** @file database/preset_repository.h
 *
 * @brief `data.presets`: a module's saved parameters, with the conditions under which it
 * auto-applies.
 *
 * @warning **Partial.** `gui/presets.c` (281 SQL references), `libs/lib.c` (141),
 * `develop/imageop.c` and `libs/export.c` all query this table directly, and two of those
 * are a standing rule violation -- CLAUDE.md says `src/libs/` and `src/views/` contain no
 * raw SQL. They belong here. Extend this file; do not start a second preset repository.
 */

#ifndef DT_DATABASE_PRESET_REPOSITORY_H
#define DT_DATABASE_PRESET_REPOSITORY_H

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

/**
 * @brief One row of `data.presets`, owned by the caller.
 *
 * @details The whole row, because both users of it want the whole row: exporting a preset
 * to a `.dtpreset` file writes every field, and importing one supplies every field. The
 * table is wide and untyped-ish -- `filter`, `def` and `format` are stored as REAL and
 * read as int -- and that is reproduced rather than corrected here.
 *
 * Strings are never NULL in a struct that came from dt_preset_repository_get_by_rowid();
 * an absent column reads as "".
 */
typedef struct dt_preset_t
{
  gchar *name;
  gchar *description;
  gchar *operation;
  int autoapply;

  /* the conditions under which this preset auto-applies */
  gchar *model;
  gchar *maker;
  gchar *lens;
  float iso_min, iso_max;
  float exposure_min, exposure_max;
  float aperture_min, aperture_max;
  int focal_length_min, focal_length_max;

  /* the module parameters themselves, as raw blobs */
  void *op_params;
  int op_params_size;
  int op_version;
  void *blendop_params;
  int blendop_params_size;
  int blendop_version;

  int enabled;
  int multi_priority;
  gchar *multi_name;
  int filter;
  int def;
  int format;
} dt_preset_t;

/** @brief Release a ::dt_preset_t and everything it owns. NULL-safe. */
void dt_preset_free(dt_preset_t *preset);

/** @brief The preset at `rowid`, or NULL when there is none. Free with dt_preset_free(). */
dt_preset_t *dt_preset_repository_get_by_rowid(const int rowid);

/**
 * @brief Insert @p preset, replacing any row that collides with it.
 *
 * @details Always writes `writeprotect = 0`: a preset arriving from a file is the user's,
 * never one of the shipped read-only ones.
 * @return TRUE when the row was written.
 */
gboolean dt_preset_repository_insert(const dt_preset_t *preset);

G_END_DECLS

#endif // DT_DATABASE_PRESET_REPOSITORY_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
