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

/* ---------------------------------------------------------------------------------------
 *  Module presets
 *
 *  The same table, seen the way a preset menu sees it: one module's presets, keyed by
 *  `(operation, op_version)`. A preset created this way is a name plus a parameter blob;
 *  every auto-apply condition is set to a wildcard, since a menu preset is applied by
 *  being picked, not by matching an image.
 * ------------------------------------------------------------------------------------- */

/** One row as a preset menu wants it. */
typedef struct dt_module_preset_t
{
  gchar *name;
  gchar *description; /**< "" when the caller did not ask for it */
  void *op_params;
  int op_params_size;
  int op_version;     /**< only filled by dt_preset_repository_list_all_versions() */
  int rowid;          /**< likewise */
  gboolean writeprotect;
} dt_module_preset_t;

/** @brief Release one ::dt_module_preset_t. Suits `g_list_free_full()`. */
void dt_module_preset_free(gpointer data);

/**
 * @brief Every preset of `(operation, op_version)`.
 *
 * @param with_description also read the description column.
 * @param shipped_first order the read-only presets before the user's, rather than after.
 *        A boolean rather than a sort direction: the caller used to interpolate "DESC" or
 *        "ASC" into the query text itself.
 */
GList *dt_preset_repository_list_for_module(const char *operation, const int op_version,
                                            const gboolean with_description,
                                            const gboolean shipped_first);

/** @brief Every preset of @p operation at ANY version, with `rowid` and `op_version`
 *  filled in. Used by the startup pass that upgrades presets to the current version. */
GList *dt_preset_repository_list_all_versions(const char *operation);

/** @brief TRUE when `(operation, op_version, name)` exists. */
gboolean dt_preset_repository_module_preset_exists(const char *operation, const int op_version,
                                                   const char *name);

/** @brief `rowid` of `(operation, op_version, name)`, or -1. */
int dt_preset_repository_find_rowid(const char *operation, const int op_version, const char *name);

/** @brief One preset by name, or NULL. Free with dt_module_preset_free(). */
dt_module_preset_t *dt_preset_repository_get_module_preset(const char *operation, const int op_version,
                                                           const char *name);

/**
 * @brief Create an empty user preset holding @p params, as the "new preset" menu item does.
 *
 * @warning Its auto-apply bounds are wildcards EXCEPT `exposure_max`, which is 1e8 here and
 * 1e7 in dt_preset_repository_add_shipped_preset(). The two inserts have disagreed since
 * they were written. Neither matters while `autoapply` is 0, which is the case for both --
 * but the preset edit dialog opens on this row immediately after creation and shows the
 * bound, so it is user-visible and the two are kept exactly as they were. Making them
 * agree is a data change and belongs in its own commit.
 */
void dt_preset_repository_add_module_preset(const char *name, const char *operation,
                                            const int op_version, const void *params,
                                            const int params_size);

/**
 * @brief Create a preset from code rather than from the menu -- the built-in presets a
 *        module registers at startup.
 *
 * @param writeprotect non-zero marks it as shipped and therefore undeletable.
 * @warning `exposure_max` is 1e7 here. See dt_preset_repository_add_module_preset().
 */
void dt_preset_repository_add_shipped_preset(const char *name, const char *operation,
                                             const int op_version, const void *params,
                                             const int params_size, const int writeprotect);

/** @brief Replace the parameters (and version) of `(operation, name)`. */
void dt_preset_repository_update_module_params(const char *operation, const char *name,
                                               const int op_version, const void *params,
                                               const int params_size);

/** @brief Replace name, description and parameters of `(operation, op_version, old_name)`. */
void dt_preset_repository_rename_module_preset(const char *operation, const int op_version,
                                               const char *old_name, const char *new_name,
                                               const char *description, const void *params,
                                               const int params_size);

/** @brief Copy `(operation, op_version, name)` to @p new_name, unprotected. */
void dt_preset_repository_duplicate_module_preset(const char *operation, const int op_version,
                                                  const char *name, const char *new_name);

/** @brief Delete `(operation, op_version, name)` unless it is write-protected. */
void dt_preset_repository_delete_module_preset(const char *operation, const int op_version,
                                               const char *name);

/** @brief Delete every preset of @p operation, whatever its version or protection. */
void dt_preset_repository_delete_all_for_module(const char *operation);

/** @brief Replace the parameters of one row, by rowid. */
void dt_preset_repository_update_params_by_rowid(const int rowid, const int op_version,
                                                 const void *params, const int params_size);

/** @brief Delete one row by rowid. */
void dt_preset_repository_delete_by_rowid(const int rowid);

/** @brief Finalise the cached statements. See dt_colorlabel_repository_cleanup(). */
void dt_preset_repository_cleanup(void);

G_END_DECLS

#endif // DT_DATABASE_PRESET_REPOSITORY_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
