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
  int op_version;     /**< filled by the *_all_versions() and the IOP readers */
  int rowid;          /**< only by dt_preset_repository_list_all_versions() */
  gboolean writeprotect;

  /* IOP presets only -- a lib module has no blending. NULL / 0 otherwise. */
  void *blendop_params;
  int blendop_params_size;
  int blendop_version;
  int enabled;
} dt_module_preset_t;

/** @brief Release one ::dt_module_preset_t. Suits `g_list_free_full()`. */
void dt_module_preset_free(gpointer data);

/**
 * @brief Every preset of `(operation, op_version)`.
 *
 * @param with_description also read the description column. When FALSE the rows come back
 *        UNORDERED and @p shipped_first is ignored -- faithful to the caller this branch
 *        replaced (get_active_preset_name), which scans every row for a params match and
 *        never cared about order.
 * @param shipped_first order the read-only presets before the user's, rather than after.
 *        A boolean rather than a sort direction: the caller used to interpolate "DESC" or
 *        "ASC" into the query text itself. Only meaningful with @p with_description.
 */
GList *dt_preset_repository_list_for_module(const char *operation, const int op_version,
                                            const gboolean with_description,
                                            const gboolean shipped_first);

/** @brief Every preset of @p operation at ANY version, with `rowid` and `op_version`
 *  filled in, SHIPPED FIRST then by name then by rowid.
 *
 *  The order is part of the contract: libs/ioporder.c takes the FIRST preset whose serialised
 *  module order matches the current one, so two presets with identical content must resolve to
 *  the shipped one. The startup upgrade pass acts per row by rowid and does not care. */
GList *dt_preset_repository_list_all_versions(const char *operation);

/** @brief As above, restricted to one @p op_version. Same columns, same order. */
GList *dt_preset_repository_list_for_version(const char *operation, const int op_version);

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

/**
 * @brief Set ONLY the version of `(operation, name)`, leaving the parameters alone.
 *
 * @warning Not a special case of dt_preset_repository_update_module_params(): that one also
 * writes `op_params`. The startup upgrade pass uses this to stamp a version onto a preset
 * whose blob is already correct, and writing the blob there would be wrong.
 *
 * Like every `(operation, name)` predicate in this file, this can match more than one row --
 * `data.presets` is UNIQUE on `(name, operation, op_version)`, so the same name may exist at
 * several versions. That is what the caller this replaced did, and it is preserved.
 */
void dt_preset_repository_set_module_version(const char *operation, const char *name,
                                             const int op_version);

/** @brief Replace the blend parameters (and blend version) of `(operation, name)`, at any
 *  version. The op_params side is dt_preset_repository_update_module_params(). */
void dt_preset_repository_set_blend_params(const char *operation, const char *name,
                                           const int blendop_version, const void *blend_params,
                                           const int blend_params_size);

/**
 * @brief Every preset of @p operation at any version, with both parameter blobs.
 *
 * @details For the startup legacy-upgrade pass, which reads a preset, converts it and writes
 * it straight back. It is the only reader that wants the blend blob alongside the op blob,
 * and the only one that must not be holding a cursor on `data.presets` while it writes to it
 * -- so this materialises the rows and the caller's loop touches no statement.
 *
 * Unordered, as that pass has always been: it acts on each row independently.
 */
GList *dt_preset_repository_list_for_upgrade(const char *operation);

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

/* ---------------------------------------------------------------------------------------
 *  Auto-apply conditions
 *
 *  The preset edit dialog changes one condition at a time, so these are one statement per
 *  field rather than a whole-row write. Grouped into two families by shape: a numeric
 *  range, and a single integer. The enum picks the columns; nothing here builds SQL from a
 *  string, so every query in this file stays greppable.
 * ------------------------------------------------------------------------------------- */

typedef enum dt_preset_range_t
{
  DT_PRESET_RANGE_ISO = 0,
  DT_PRESET_RANGE_APERTURE,
  DT_PRESET_RANGE_EXPOSURE,
  DT_PRESET_RANGE_FOCAL_LENGTH,
  DT_PRESET_RANGE_LAST
} dt_preset_range_t;

typedef enum dt_preset_flag_t
{
  DT_PRESET_FLAG_FORMAT = 0, /**< which of raw/LDR/HDR the preset applies to */
  DT_PRESET_FLAG_AUTOAPPLY,
  DT_PRESET_FLAG_FILTER,     /**< whether the preset is offered as a filter at all */
  DT_PRESET_FLAG_LAST
} dt_preset_flag_t;

/** @brief Set the `<range>_min` / `<range>_max` pair of `(operation, op_version, name)`. */
void dt_preset_repository_update_range(const char *operation, const int op_version, const char *name,
                                       const dt_preset_range_t range,
                                       const double min, const double max);

/** @brief Set one integer condition of `(operation, op_version, name)`. */
void dt_preset_repository_update_flag(const char *operation, const int op_version, const char *name,
                                      const dt_preset_flag_t flag, const int value);

/**
 * @brief Set the camera match of `(operation, op_version, name)`.
 *
 * @param maker stored wrapped in `%` on both sides, so it matches as a substring.
 * @param model,lens an empty string is stored as `%`, i.e. "any". That substitution is
 *        here rather than in the caller because it is about how the row has to look for
 *        the auto-apply query to match it.
 */
void dt_preset_repository_update_camera(const char *operation, const int op_version, const char *name,
                                        const char *maker, const char *model, const char *lens);

/* ---------------------------------------------------------------------------------------
 *  IOP presets
 *
 *  Same table again, seen by a pixel module's preset menu. Two things distinguish it from
 *  the lib half: the rows carry blend parameters, and the menu filters by whether a preset
 *  matches the image being edited -- so three queries share one predicate over the camera,
 *  the exposure triangle and the file format.
 * ------------------------------------------------------------------------------------- */

/**
 * @brief The facts about one image that decide whether a preset matches it.
 *
 * @details Every field is bound, never interpolated. The `_alias`/`_maker` pair exists
 * because a preset may name either the EXIF strings or rawspeed's canonical camera names,
 * and the query tries both.
 */
typedef struct dt_preset_match_t
{
  const char *exif_model;
  const char *exif_maker;
  const char *camera_alias;
  const char *camera_maker;
  const char *exif_lens;
  double iso;
  double exposure;
  double aperture;
  double focal_length;
  int format;   /**< FOR_RAW / FOR_LDR / FOR_HDR of the image */
  int excluded; /**< FOR_NOT_MONO / FOR_NOT_COLOR of the image */
} dt_preset_match_t;

/** @brief Store an IOP preset, blend parameters included, replacing any of the same name. */
void dt_preset_repository_add_iop_preset(const char *name, const char *operation, const int op_version,
                                         const void *params, const int params_size,
                                         const void *blend_params, const int blend_params_size,
                                         const int blendop_version, const int enabled);

/**
 * @brief Every preset of `(operation, op_version)`, with blend parameters.
 *
 * @details Ordered `writeprotect ASC` -- user presets before shipped ones. The comment at
 * the original call site explains why that direction: with DESC, a user's copy of a
 * write-protected preset would resolve to the protected one and could not be deleted.
 */
GList *dt_preset_repository_list_for_iop(const char *operation, const int op_version);

/**
 * @brief Presets of @p operation to offer in a menu, in display order.
 *
 * @param match the image being edited, or NULL for "no particular image, show everything".
 *        When given, a preset with `filter = 0` is always offered and the rest must match.
 * @param shipped_first order the read-only presets before the user's.
 */
GList *dt_preset_repository_list_for_menu(const char *operation, const dt_preset_match_t *match,
                                          const gboolean shipped_first);

/** @brief One IOP preset by name, blend parameters included, or NULL. */
dt_module_preset_t *dt_preset_repository_get_iop_preset(const char *operation, const int op_version,
                                                        const char *name);

/**
 * @brief Names of the presets that should be applied to this image automatically.
 *
 * @param always_name a preset name matched regardless of the conditions -- the workflow
 *        default. Pass a string no preset can be called to disable that leg.
 * @return a `GList` of newly allocated names, in database order. Free with
 *         `g_list_free_full(l, g_free)`.
 */
GList *dt_preset_repository_find_autoapply(const char *operation, const int op_version,
                                           const dt_preset_match_t *match, const char *always_name);

/* ---------------------------------------------------------------------------------------
 *  The preset edit dialog
 *
 *  It shows and writes the whole condition set at once, so these take it as one struct
 *  rather than sixteen parameters.
 * ------------------------------------------------------------------------------------- */

/** Everything the edit dialog shows. Strings are owned by the struct when it comes out of
 *  dt_preset_repository_get_conditions(), and borrowed when it goes in. */
typedef struct dt_preset_conditions_t
{
  gchar *name;
  gchar *description;
  gchar *model;
  gchar *maker;
  gchar *lens;
  double iso_min, iso_max;
  double exposure_min, exposure_max;
  double aperture_min, aperture_max;
  /* REAL columns, like the pairs above: read and bound as doubles so a fractional stored
   * bound survives an open-and-save of the edit dialog. The dialog displays whole
   * millimetres, but display rounding is the dialog's, not the store's. */
  double focal_length_min, focal_length_max;
  int autoapply;
  int filter;
  int format;
} dt_preset_conditions_t;

/** @brief Release the strings owned by @p c (not @p c itself). */
void dt_preset_conditions_free(dt_preset_conditions_t *c);

/**
 * @brief Read the conditions of `(operation, op_version, name)`.
 * @param rowid receives the row's id, or -1 when there is no such preset.
 * @return TRUE when a row was found; @p c is only filled then.
 */
gboolean dt_preset_repository_get_conditions(const char *operation, const int op_version,
                                             const char *name, dt_preset_conditions_t *c, int *rowid);

/** @brief Overwrite the conditions of the row at @p rowid. */
void dt_preset_repository_update_conditions(const int rowid, const dt_preset_conditions_t *c);

/** @brief Create a preset from a full condition set plus its module payload. */
void dt_preset_repository_insert_with_conditions(const dt_preset_conditions_t *c,
                                                 const char *operation, const int op_version,
                                                 const void *params, const int params_size,
                                                 const int enabled,
                                                 const void *blend_params, const int blend_params_size,
                                                 const int blendop_version);

/** @brief The `(operation, op_version)` a rowid belongs to. Returns FALSE if there is none;
 *  `*operation` is newly allocated on success. */
gboolean dt_preset_repository_get_identity(const int rowid, gchar **operation, int *op_version);

/** @brief Replace the module payload of `(operation, name)`, at any version. */
void dt_preset_repository_update_iop_params(const char *operation, const char *name,
                                            const int op_version,
                                            const void *params, const int params_size,
                                            const int enabled,
                                            const void *blend_params, const int blend_params_size,
                                            const int blendop_version);

/** @brief Delete the row at @p rowid unless it is write-protected. */
void dt_preset_repository_delete_by_rowid_unprotected(const int rowid);

/** @brief Delete every write-protected preset -- the auto-generated ones, dropped at
 *  startup so module code can regenerate them. */
void dt_preset_repository_delete_shipped(void);

/* ---------------------------------------------------------------------------------------
 *  Listing presets for the Preferences dialog
 *
 *  The preferences tree shows every preset in the database, grouped by module, with the
 *  auto-apply conditions rendered as ranges. These two return whole rows rather than a
 *  cursor, because the caller builds GTK widgets from them and must not be stepping a
 *  statement while it does.
 * ------------------------------------------------------------------------------------- */

/** @brief One row of the Preferences preset tree, with its auto-apply conditions. */
typedef struct dt_preset_row_t
{
  int rowid;
  char *name;
  char *operation;
  gboolean autoapply;
  char *model;
  char *maker;
  char *lens;
  float iso_min, iso_max;
  float exposure_min, exposure_max;
  float aperture_min, aperture_max;
  int focal_length_min, focal_length_max;  /**< stored as REAL, read as int, as the tree shows them */
  gboolean writeprotect;
} dt_preset_row_t;

/** @brief Free one dt_preset_row_t. Suits g_list_free_full(). */
void dt_preset_row_free(gpointer data);

/** @brief Every preset, ordered by (operation, name) -- the grouping the tree relies on to
 *  start a new module node whenever `operation` changes. */
GList *dt_preset_repository_list_all(void);

/** @brief Identity of one preset, for callers listing rather than loading. */
typedef struct dt_preset_identity_t
{
  int rowid;
  char *name;
  char *operation;
} dt_preset_identity_t;

/** @brief Free one dt_preset_identity_t. Suits g_list_free_full(). */
void dt_preset_identity_free(gpointer data);

/** @brief Every preset the user may edit (`writeprotect = 0`), in row order. */
GList *dt_preset_repository_list_editable(void);

/** @brief Finalise the cached statements. See dt_colorlabel_repository_cleanup(). */
void dt_preset_repository_cleanup(void);

G_END_DECLS

#endif // DT_DATABASE_PRESET_REPOSITORY_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
