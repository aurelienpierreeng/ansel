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

#include "database/preset_repository.h"

#include "database/database.h"
#include "database/sql_debug.h"
#include "system/macros.h"
#include "system/mem_alloc.h"

#include <float.h>
#include <math.h>
#include <sqlite3.h>
#include <string.h>

void dt_preset_free(dt_preset_t *preset)
{
  if(IS_NULL_PTR(preset)) return;

  dt_free(preset->name);
  dt_free(preset->description);
  dt_free(preset->operation);
  dt_free(preset->model);
  dt_free(preset->maker);
  dt_free(preset->lens);
  dt_free(preset->multi_name);
  dt_free(preset->op_params);
  dt_free(preset->blendop_params);
  dt_free(preset);
}

/* Never NULL: the callers format these with "%s" and bind them with strlen(). */
static gchar *_column_text(sqlite3_stmt *stmt, const int col)
{
  const char *v = (const char *)sqlite3_column_text(stmt, col);
  return g_strdup(v ? v : "");
}

/* Unlike _column_text(), a NULL column comes back as NULL. For callers that pass the value
 * on to something which distinguishes "no value" from "the empty string". */
static gchar *_column_text_or_null(sqlite3_stmt *stmt, const int col)
{
  const char *v = (const char *)sqlite3_column_text(stmt, col);
  return v ? g_strdup(v) : NULL;
}

static void *_column_blob(sqlite3_stmt *stmt, const int col, int *size)
{
  const int n = sqlite3_column_bytes(stmt, col);
  const void *src = sqlite3_column_blob(stmt, col);
  *size = 0;
  if(n <= 0 || IS_NULL_PTR(src)) return NULL;

  void *copy = g_malloc(n);
  if(IS_NULL_PTR(copy)) return NULL;

  memcpy(copy, src, n);
  *size = n;
  return copy;
}

dt_preset_t *dt_preset_repository_get_by_rowid(const int rowid)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT op_params, blendop_params, name, description, operation,"
                              "   autoapply, model, maker, lens, iso_min, iso_max, exposure_min,"
                              "   exposure_max, aperture_min, aperture_max, focal_length_min,"
                              "   focal_length_max, op_version, blendop_version, enabled,"
                              "   multi_priority, multi_name, filter, def, format "
                              " FROM data.presets"
                              " WHERE rowid = ?1",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, rowid);

  dt_preset_t *p = NULL;
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    p = (dt_preset_t *)g_malloc0(sizeof(dt_preset_t));
    if(p)
    {
      p->op_params = _column_blob(stmt, 0, &p->op_params_size);
      p->blendop_params = _column_blob(stmt, 1, &p->blendop_params_size);
      p->name = _column_text(stmt, 2);
      p->description = _column_text(stmt, 3);
      p->operation = _column_text(stmt, 4);
      p->autoapply = sqlite3_column_int(stmt, 5);
      p->model = _column_text(stmt, 6);
      p->maker = _column_text(stmt, 7);
      p->lens = _column_text(stmt, 8);
      p->iso_min = sqlite3_column_double(stmt, 9);
      p->iso_max = sqlite3_column_double(stmt, 10);
      p->exposure_min = sqlite3_column_double(stmt, 11);
      p->exposure_max = sqlite3_column_double(stmt, 12);
      p->aperture_min = sqlite3_column_double(stmt, 13);
      p->aperture_max = sqlite3_column_double(stmt, 14);
      /* REAL columns read as int, as they always have been */
      p->focal_length_min = sqlite3_column_double(stmt, 15);
      p->focal_length_max = sqlite3_column_double(stmt, 16);
      p->op_version = sqlite3_column_int(stmt, 17);
      p->blendop_version = sqlite3_column_int(stmt, 18);
      p->enabled = sqlite3_column_int(stmt, 19);
      p->multi_priority = sqlite3_column_int(stmt, 20);
      p->multi_name = _column_text(stmt, 21);
      p->filter = sqlite3_column_double(stmt, 22);
      p->def = sqlite3_column_double(stmt, 23);
      p->format = sqlite3_column_double(stmt, 24);
    }
  }
  sqlite3_finalize(stmt);
  return p;
}

/* Bind a string that may legitimately be NULL. The previous code called strlen() on these
 * unguarded, so a preset file missing an element -- which the XML reader answers with NULL
 * rather than "" -- crashed on import instead of importing an empty field. */
static void _bind_text(sqlite3_stmt *stmt, const int idx, const char *value)
{
  const char *v = value ? value : "";
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, idx, v, strlen(v), SQLITE_TRANSIENT);
}

gboolean dt_preset_repository_insert(const dt_preset_t *preset)
{
  if(IS_NULL_PTR(preset)) return FALSE;

  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
     "INSERT OR REPLACE"
     "  INTO data.presets"
     "    (name, description, operation, autoapply,"
     "     model, maker, lens, iso_min, iso_max, exposure_min, exposure_max,"
     "     aperture_min, aperture_max, focal_length_min, focal_length_max,"
     "     op_params, op_version, blendop_params, blendop_version, enabled,"
     "     multi_priority, multi_name, filter, def, format, writeprotect)"
     "  VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, "
     "          ?15, ?16, ?17, ?18, ?19, ?20, ?21, ?22, ?23, ?24, ?25, 0)",
     -1, &stmt, NULL);
  // clang-format on

  _bind_text(stmt, 1, preset->name);
  _bind_text(stmt, 2, preset->description);
  _bind_text(stmt, 3, preset->operation);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, preset->autoapply);
  _bind_text(stmt, 5, preset->model);
  _bind_text(stmt, 6, preset->maker);
  _bind_text(stmt, 7, preset->lens);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 8, preset->iso_min);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 9, preset->iso_max);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 10, preset->exposure_min);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 11, preset->exposure_max);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 12, preset->aperture_min);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 13, preset->aperture_max);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 14, preset->focal_length_min);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 15, preset->focal_length_max);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 16, preset->op_params, preset->op_params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 17, preset->op_version);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 18, preset->blendop_params, preset->blendop_params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 19, preset->blendop_version);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 20, preset->enabled);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 21, preset->multi_priority);
  _bind_text(stmt, 22, preset->multi_name);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 23, preset->filter);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 24, preset->def);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 25, preset->format);

  const gboolean ok = (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
  return ok;
}


/* ---------------------------------------------------------------------------------------
 *  Module presets
 * ------------------------------------------------------------------------------------- */

/* Three of these run on every preset-menu popup, so they keep their statements, as
 * libs/lib.c did before. The rest are one per user action. */

/* The two inserts below differ in ONE digit of exposure_max -- 1e8 for a preset created
 * from the menu, 1e7 for one registered by a module at startup -- and have since they were
 * written. Reproduced rather than unified: with autoapply=0 the bound is never consulted,
 * but the edit dialog opens on a just-created preset and shows it, so unifying them is a
 * user-visible data change and belongs in its own commit. */
#define _PRESET_COLUMNS                                                              \
  "(name, description, operation, op_version, op_params, blendop_params, "           \
  " blendop_version, enabled, model, maker, lens, iso_min, iso_max, exposure_min, "  \
  " exposure_max, aperture_min, aperture_max, focal_length_min, focal_length_max, "  \
  " writeprotect, autoapply, filter, def, format)"

void dt_module_preset_free(gpointer data)
{
  dt_module_preset_t *p = (dt_module_preset_t *)data;
  if(IS_NULL_PTR(p)) return;
  dt_free(p->name);
  dt_free(p->description);
  dt_free(p->op_params);
  // The IOP readers fill this one; a lib preset leaves it NULL. Missing it leaked the blend
  // blob of every IOP preset row listed by gui/presets.c and libs/lib.c.
  dt_free(p->blendop_params);
  dt_free(p);
}

GList *dt_preset_repository_list_for_module(const char *operation, const int op_version,
                                            const gboolean with_description,
                                            const gboolean shipped_first)
{
  sqlite3_stmt *stmt = NULL;
  gchar *query = NULL;

  if(with_description)
  {
    // clang-format off
    query = g_strdup_printf("SELECT name, op_params, writeprotect, description"
                            " FROM data.presets"
                            " WHERE operation=?1 AND op_version=?2"
                            " ORDER BY writeprotect %s, LOWER(name), rowid",
                            shipped_first ? "DESC" : "ASC");
    // clang-format on
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), query, -1, &stmt, NULL);
  }
  else
  {
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "SELECT name, op_params, writeprotect FROM data.presets"
                                " WHERE operation=?1 AND op_version=?2",
                                -1, &stmt, NULL);
    // clang-format on
  }
  if(IS_NULL_PTR(stmt))
  {
    dt_free(query);
    return NULL;
  }

  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, op_version);

  GList *presets = NULL;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_module_preset_t *p = g_malloc0(sizeof(dt_module_preset_t));
    if(p)
    {
      p->name = _column_text(stmt, 0);
      p->op_params = _column_blob(stmt, 1, &p->op_params_size);
      p->writeprotect = sqlite3_column_int(stmt, 2) != 0;
      p->description = with_description ? _column_text(stmt, 3) : g_strdup("");
      p->op_version = op_version;
      p->rowid = -1;
      presets = g_list_prepend(presets, p);
    }
  }

  sqlite3_finalize(stmt);
  dt_free(query); // NULL in the plain branch; dt_free is NULL-safe

  return g_list_reverse(presets); // the ORDER BY is the point; keep it
}

GList *dt_preset_repository_list_all_versions(const char *operation)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT rowid, op_version, op_params, name FROM data.presets"
                              " WHERE operation=?1"
                              " ORDER BY writeprotect DESC, LOWER(name), rowid",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, operation, -1, SQLITE_TRANSIENT);

  GList *presets = NULL;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_module_preset_t *p = g_malloc0(sizeof(dt_module_preset_t));
    if(p)
    {
      p->rowid = sqlite3_column_int(stmt, 0);
      p->op_version = sqlite3_column_int(stmt, 1);
      p->op_params = _column_blob(stmt, 2, &p->op_params_size);
      p->name = _column_text(stmt, 3);
      p->description = g_strdup("");
      presets = g_list_prepend(presets, p);
    }
  }
  sqlite3_finalize(stmt);

  /* Reversed so the caller iterates in the order the rows came back, which is what it did
   * when it stepped the statement itself. */
  return g_list_reverse(presets);
}

GList *dt_preset_repository_list_for_version(const char *operation, const int op_version)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT rowid, op_version, op_params, name FROM data.presets"
                              " WHERE operation=?1 AND op_version=?2"
                              " ORDER BY writeprotect DESC, LOWER(name), rowid",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, op_version);

  GList *presets = NULL;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_module_preset_t *p = g_malloc0(sizeof(dt_module_preset_t));
    if(p)
    {
      p->rowid = sqlite3_column_int(stmt, 0);
      p->op_version = sqlite3_column_int(stmt, 1);
      p->op_params = _column_blob(stmt, 2, &p->op_params_size);
      p->name = _column_text(stmt, 3);
      p->description = g_strdup("");
      presets = g_list_prepend(presets, p);
    }
  }
  sqlite3_finalize(stmt);

  /* Reversed so the caller iterates in the order the rows came back, which is what it did
   * when it stepped the statement itself. */
  return g_list_reverse(presets);
}

gboolean dt_preset_repository_module_preset_exists(const char *operation, const int op_version,
                                                   const char *name)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT name FROM data.presets"
                              " WHERE operation = ?1 AND op_version = ?2 AND name = ?3",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, op_version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, name, -1, SQLITE_TRANSIENT);

  const gboolean found = (sqlite3_step(stmt) == SQLITE_ROW);
  sqlite3_finalize(stmt);
  return found;
}

int dt_preset_repository_find_rowid(const char *operation, const int op_version, const char *name)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT rowid FROM data.presets"
                              " WHERE name = ?1 AND operation = ?2 AND op_version = ?3",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, op_version);

  int rowid = -1;
  if(sqlite3_step(stmt) == SQLITE_ROW) rowid = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return rowid;
}

dt_module_preset_t *dt_preset_repository_get_module_preset(const char *operation, const int op_version,
                                                           const char *name)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT op_params, writeprotect FROM data.presets"
                              " WHERE operation = ?1 AND op_version = ?2 AND name = ?3",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, op_version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, name, -1, SQLITE_TRANSIENT);

  dt_module_preset_t *p = NULL;
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    p = g_malloc0(sizeof(dt_module_preset_t));
    if(p)
    {
      p->op_params = _column_blob(stmt, 0, &p->op_params_size);
      p->writeprotect = sqlite3_column_int(stmt, 1) != 0;
      p->name = g_strdup(name);
      p->description = g_strdup("");
      p->op_version = op_version;
      p->rowid = -1;
    }
  }
  sqlite3_finalize(stmt);
  return p;
}

void dt_preset_repository_add_module_preset(const char *name, const char *operation,
                                            const int op_version, const void *params,
                                            const int params_size)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
      "INSERT INTO data.presets " _PRESET_COLUMNS
      " VALUES (?1, '', ?2, ?3, ?4, NULL, 0, 1, '%', '%', '%', 0, "
      "         340282346638528859812000000000000000000, 0, 100000000, 0, 100000000, "
      "         0, 1000, 0, 0, 0, 0, 0)",
      -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, op_version);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 4, params, params_size, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_preset_repository_add_shipped_preset(const char *name, const char *operation,
                                             const int op_version, const void *params,
                                             const int params_size, const int writeprotect)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
      "INSERT INTO data.presets " _PRESET_COLUMNS
      " VALUES (?1, '', ?2, ?3, ?4, NULL, 0, 1, '%', '%', '%', 0, "
      "         340282346638528859812000000000000000000, 0, 10000000, 0, 100000000, "
      "         0, 1000, ?5, 0, 0, 0, 0)",
      -1, &stmt, NULL);
  // clang-format on
  if(IS_NULL_PTR(stmt)) return;

  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, op_version);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 4, params, params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 5, writeprotect);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_preset_repository_update_module_params(const char *operation, const char *name,
                                               const int op_version, const void *params,
                                               const int params_size)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "UPDATE data.presets SET op_version=?2, op_params=?3"
                              " WHERE name=?4 AND operation=?1",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, op_version);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 3, params, params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_preset_repository_set_module_version(const char *operation, const char *name,
                                             const int op_version)
{
  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "UPDATE data.presets SET op_version=?1 WHERE operation=?2 AND name=?3",
                              -1, &stmt, NULL);
  if(IS_NULL_PTR(stmt)) return;

  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, op_version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_preset_repository_set_blend_params(const char *operation, const char *name,
                                           const int blendop_version, const void *blend_params,
                                           const int blend_params_size)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), "UPDATE data.presets "
                                                                "SET blendop_version=?1, blendop_params=?2 "
                                                                "WHERE operation=?3 AND name=?4",
                              -1, &stmt, NULL);
  // clang-format on
  if(IS_NULL_PTR(stmt)) return;

  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, blendop_version);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 2, blend_params, blend_params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

GList *dt_preset_repository_list_for_upgrade(const char *operation)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get_sqlite3_global(),
      "SELECT name, op_version, op_params, blendop_version, blendop_params FROM data.presets WHERE operation = ?1",
      -1, &stmt, NULL);
  // clang-format on
  if(IS_NULL_PTR(stmt)) return NULL;

  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, operation, -1, SQLITE_TRANSIENT);

  GList *presets = NULL;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_module_preset_t *p = g_malloc0(sizeof(dt_module_preset_t));
    if(IS_NULL_PTR(p)) break;
    p->name = _column_text(stmt, 0);
    p->op_version = sqlite3_column_int(stmt, 1);
    p->op_params = _column_blob(stmt, 2, &p->op_params_size);
    p->blendop_version = sqlite3_column_int(stmt, 3);
    p->blendop_params = _column_blob(stmt, 4, &p->blendop_params_size);
    p->description = g_strdup("");
    presets = g_list_prepend(presets, p);
  }
  sqlite3_finalize(stmt);

  return g_list_reverse(presets);  // row order, as the cursor loop this replaced saw it
}

void dt_preset_repository_rename_module_preset(const char *operation, const int op_version,
                                               const char *old_name, const char *new_name,
                                               const char *description, const void *params,
                                               const int params_size)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "UPDATE data.presets SET name = ?1, description = ?2, op_params = ?3"
                              " WHERE operation = ?4 AND op_version = ?5 AND name = ?6",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, new_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, description ? description : "", -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 3, params, params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 5, op_version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 6, old_name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_preset_repository_duplicate_module_preset(const char *operation, const int op_version,
                                                  const char *name, const char *new_name)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
      "INSERT INTO data.presets " _PRESET_COLUMNS
      " SELECT ?1, description, operation, op_version, op_params, blendop_params,"
      "        blendop_version, enabled, model, maker, lens, iso_min, iso_max,"
      "        exposure_min, exposure_max, aperture_min, aperture_max,"
      "        focal_length_min, focal_length_max, 0, autoapply, filter, def, format"
      " FROM data.presets"
      " WHERE operation = ?2 AND op_version = ?3 AND name = ?4",
      -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, new_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, op_version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_preset_repository_delete_module_preset(const char *operation, const int op_version,
                                               const char *name)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "DELETE FROM data.presets"
                              " WHERE name=?1 AND operation=?2 AND op_version=?3 AND writeprotect=0",
                              -1, &stmt, NULL);
  // clang-format on
  if(IS_NULL_PTR(stmt)) return;

  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, op_version);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_preset_repository_delete_all_for_module(const char *operation)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "DELETE FROM data.presets WHERE operation=?1",
                              -1, &stmt, NULL);
  // clang-format on
  if(IS_NULL_PTR(stmt)) return;

  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, operation, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_preset_repository_update_params_by_rowid(const int rowid, const int op_version,
                                                 const void *params, const int params_size)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "UPDATE data.presets SET op_version=?1, op_params=?2 WHERE rowid=?3",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, op_version);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 2, params, params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, rowid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_preset_repository_delete_by_rowid(const int rowid)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "DELETE FROM data.presets WHERE rowid=?1", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, rowid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}


/* ---------------------------------------------------------------------------------------
 *  Auto-apply conditions
 * ------------------------------------------------------------------------------------- */

/* One literal query per column, indexed by the enum rather than assembled, so
 * `grep "iso_min" src/database` still finds the one that writes it. Prepared per call:
 * the cached statements these replaced were unlocked cross-thread shared state. */

static const char *const _range_query[DT_PRESET_RANGE_LAST] = {
  [DT_PRESET_RANGE_ISO] = "UPDATE data.presets"
                          " SET iso_min=?1, iso_max=?2"
                          " WHERE operation=?3 AND op_version=?4 AND name=?5",
  [DT_PRESET_RANGE_APERTURE] = "UPDATE data.presets"
                               " SET aperture_min=?1, aperture_max=?2"
                               " WHERE operation=?3 AND op_version=?4 AND name=?5",
  [DT_PRESET_RANGE_EXPOSURE] = "UPDATE data.presets"
                               " SET exposure_min=?1, exposure_max=?2"
                               " WHERE operation=?3 AND op_version=?4 AND name=?5",
  [DT_PRESET_RANGE_FOCAL_LENGTH] = "UPDATE data.presets"
                                   " SET focal_length_min=?1, focal_length_max=?2"
                                   " WHERE operation=?3 AND op_version=?4 AND name=?5",
};

static const char *const _flag_query[DT_PRESET_FLAG_LAST] = {
  [DT_PRESET_FLAG_FORMAT] = "UPDATE data.presets"
                            " SET format=?1"
                            " WHERE operation=?2 AND op_version=?3 AND name=?4",
  [DT_PRESET_FLAG_AUTOAPPLY] = "UPDATE data.presets"
                               " SET autoapply=?1"
                               " WHERE operation=?2 AND op_version=?3 AND name=?4",
  [DT_PRESET_FLAG_FILTER] = "UPDATE data.presets"
                            " SET filter=?1"
                            " WHERE operation=?2 AND op_version=?3 AND name=?4",
};

void dt_preset_repository_update_range(const char *operation, const int op_version, const char *name,
                                       const dt_preset_range_t range,
                                       const double min, const double max)
{
  if(range < 0 || range >= DT_PRESET_RANGE_LAST) return;

  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), _range_query[range],
                              -1, &stmt, NULL);
  if(IS_NULL_PTR(stmt)) return;

  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 1, min);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 2, max);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, op_version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_preset_repository_update_flag(const char *operation, const int op_version, const char *name,
                                      const dt_preset_flag_t flag, const int value)
{
  if(flag < 0 || flag >= DT_PRESET_FLAG_LAST) return;

  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), _flag_query[flag],
                              -1, &stmt, NULL);
  if(IS_NULL_PTR(stmt)) return;

  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, value);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, op_version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_preset_repository_update_camera(const char *operation, const int op_version, const char *name,
                                        const char *maker, const char *model, const char *lens)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "UPDATE data.presets"
                              " SET maker='%' || ?1 || '%', model=?2, lens=?3"
                              " WHERE operation=?4 AND op_version=?5 AND name=?6",
                              -1, &stmt, NULL);
  // clang-format on
  if(IS_NULL_PTR(stmt)) return;

  /* An empty model or lens means "any", and the auto-apply query matches with LIKE, so it
   * has to be stored as the wildcard rather than as "". */
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, maker, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, (model && *model) ? model : "%", -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, (lens && *lens) ? lens : "%", -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 5, op_version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 6, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}


/* ---------------------------------------------------------------------------------------
 *  IOP presets
 * ------------------------------------------------------------------------------------- */

/* The predicate shared by the menu and the auto-apply query: does this preset's stored
 * camera/exposure/format match the image? Bound parameters ?2..?12 throughout, so the
 * three call sites bind identically. */
#define _PRESET_MATCH_PREDICATE                                                    \
  "((?2 LIKE model AND ?3 LIKE maker) OR (?4 LIKE model AND ?5 LIKE maker))"       \
  " AND ?6 LIKE lens"                                                              \
  " AND ?7 BETWEEN iso_min AND iso_max"                                            \
  " AND ?8 BETWEEN exposure_min AND exposure_max"                                  \
  " AND ?9 BETWEEN aperture_min AND aperture_max"                                  \
  " AND ?10 BETWEEN focal_length_min AND focal_length_max"                         \
  " AND (format = 0 OR (format&?11 != 0 AND ~format&?12 != 0))"

static void _bind_match(sqlite3_stmt *stmt, const dt_preset_match_t *m, const gboolean clamp)
{
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, m->exif_model, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, m->exif_maker, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, m->camera_alias, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, m->camera_maker, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 6, m->exif_lens, -1, SQLITE_TRANSIENT);

  /* The auto-apply path clamps these and the menu path does not. That difference is
   * inherited and kept: a NaN or an infinity from a broken EXIF block makes every BETWEEN
   * false, which silently applies nothing -- so the path that decides an image's initial
   * history guards against it and the one that only draws a menu never bothered. */
  if(clamp)
  {
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 7, fmax(0.0, fmin(FLT_MAX, m->iso)));
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 8, fmax(0.0, fmin(1000000.0, m->exposure)));
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 9, fmax(0.0, fmin(1000000.0, m->aperture)));
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 10, fmax(0.0, fmin(1000000.0, m->focal_length)));
  }
  else
  {
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 7, m->iso);
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 8, m->exposure);
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 9, m->aperture);
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 10, m->focal_length);
  }

  DT_DEBUG_SQLITE3_BIND_INT(stmt, 11, m->format);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 12, m->excluded);
}

/* Read the seven-column menu row, or the five-column list row when `description` and the
 * trailing columns are absent. */
static dt_module_preset_t *_iop_row(sqlite3_stmt *stmt, const gboolean with_description)
{
  dt_module_preset_t *p = g_malloc0(sizeof(dt_module_preset_t));
  if(IS_NULL_PTR(p)) return NULL;

  p->rowid = -1;
  p->name = _column_text(stmt, 0);
  p->op_params = _column_blob(stmt, 1, &p->op_params_size);
  p->writeprotect = sqlite3_column_int(stmt, 2) != 0;

  if(with_description)
  {
    p->description = _column_text(stmt, 3);
    p->blendop_params = _column_blob(stmt, 4, &p->blendop_params_size);
    p->op_version = sqlite3_column_int(stmt, 5);
    p->enabled = sqlite3_column_int(stmt, 6);
  }
  else
  {
    p->description = g_strdup("");
    p->blendop_params = _column_blob(stmt, 3, &p->blendop_params_size);
    p->enabled = sqlite3_column_int(stmt, 4);
  }
  return p;
}

void dt_preset_repository_add_iop_preset(const char *name, const char *operation, const int op_version,
                                         const void *params, const int params_size,
                                         const void *blend_params, const int blend_params_size,
                                         const int blendop_version, const int enabled)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
      "INSERT OR REPLACE"
      " INTO data.presets (name, description, operation, op_version, op_params, enabled,"
      "                    blendop_params, blendop_version, multi_priority, multi_name,"
      "                    model, maker, lens, iso_min, iso_max, exposure_min, exposure_max,"
      "                    aperture_min, aperture_max, focal_length_min, focal_length_max,"
      "                    writeprotect, autoapply, filter, def, format)"
      " VALUES (?1, '', ?2, ?3, ?4, ?5, ?6, ?7, 0, '', '%', '%', '%', 0,"
      "         340282346638528859812000000000000000000, 0, 10000000, 0, 100000000, 0,"
      "         1000, 1, 0, 0, 0, 0)",
      -1, &stmt, NULL);
  // clang-format on
  if(IS_NULL_PTR(stmt)) return;

  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, op_version);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 4, params, params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 5, enabled);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 6, blend_params, blend_params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 7, blendop_version);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

GList *dt_preset_repository_list_for_iop(const char *operation, const int op_version)
{
  sqlite3_stmt *stmt = NULL;
  /* writeprotect ASC, not DESC: a user's copy of a shipped preset must resolve to the
   * copy, otherwise the name that comes back is the write-protected one and the caller
   * cannot delete it. */
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
      "SELECT name, op_params, writeprotect, blendop_params, enabled"
      " FROM data.presets"
      " WHERE operation=?1 AND op_version=?2"
      " ORDER BY writeprotect ASC, LOWER(name), rowid",
      -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, op_version);

  GList *presets = NULL;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_module_preset_t *p = _iop_row(stmt, FALSE);
    if(p)
    {
      p->op_version = op_version;
      presets = g_list_prepend(presets, p);
    }
  }
  sqlite3_finalize(stmt);
  return g_list_reverse(presets);
}

GList *dt_preset_repository_list_for_menu(const char *operation, const dt_preset_match_t *match,
                                          const gboolean shipped_first)
{
  sqlite3_stmt *stmt = NULL;
  gchar *query = NULL;

  if(match)
  {
    // clang-format off
    query = g_strdup_printf(
        "SELECT name, op_params, writeprotect, description, blendop_params, "
        "  op_version, enabled"
        " FROM data.presets"
        " WHERE operation=?1"
        "   AND (filter=0"
        "          OR"
        "       (" _PRESET_MATCH_PREDICATE "))"
        " ORDER BY writeprotect %s, LOWER(name), rowid",
        shipped_first ? "DESC" : "ASC");
    // clang-format on
  }
  else
  {
    // don't know for which image. show all we got:
    // clang-format off
    query = g_strdup_printf(
        "SELECT name, op_params, writeprotect, "
        "       description, blendop_params, op_version, enabled"
        " FROM data.presets"
        " WHERE operation=?1"
        " ORDER BY writeprotect %s, LOWER(name), rowid",
        shipped_first ? "DESC" : "ASC");
    // clang-format on
  }

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, operation, -1, SQLITE_TRANSIENT);
  if(match) _bind_match(stmt, match, FALSE);
  dt_free(query);

  GList *presets = NULL;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_module_preset_t *p = _iop_row(stmt, TRUE);
    if(p) presets = g_list_prepend(presets, p);
  }
  sqlite3_finalize(stmt);
  return g_list_reverse(presets);
}

dt_module_preset_t *dt_preset_repository_get_iop_preset(const char *operation, const int op_version,
                                                        const char *name)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
     "SELECT op_params, enabled, blendop_params, blendop_version, writeprotect"
     " FROM data.presets"
     " WHERE operation = ?1 AND op_version = ?2 AND name = ?3",
     -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, op_version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, name, -1, SQLITE_TRANSIENT);

  dt_module_preset_t *p = NULL;
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    p = g_malloc0(sizeof(dt_module_preset_t));
    if(p)
    {
      p->rowid = -1;
      p->name = g_strdup(name);
      p->description = g_strdup("");
      p->op_version = op_version;
      p->op_params = _column_blob(stmt, 0, &p->op_params_size);
      p->enabled = sqlite3_column_int(stmt, 1);
      p->blendop_params = _column_blob(stmt, 2, &p->blendop_params_size);
      p->blendop_version = sqlite3_column_int(stmt, 3);
      p->writeprotect = sqlite3_column_int(stmt, 4) != 0;
    }
  }
  sqlite3_finalize(stmt);
  return p;
}

GList *dt_preset_repository_find_autoapply(const char *operation, const int op_version,
                                           const dt_preset_match_t *match, const char *always_name)
{
  if(IS_NULL_PTR(match)) return NULL;

  sqlite3_stmt *stmt = NULL;
  /* A plain literal. This was assembled with snprintf() into a 2024-byte buffer despite
   * having no format specifier in it -- a copy that did nothing, and a `%` away from being
   * a bug. */
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
     "SELECT name"
     " FROM data.presets"
     " WHERE operation = ?1"
     "        AND ((autoapply=1"
     "           AND " _PRESET_MATCH_PREDICATE
     "           AND operation NOT IN"
     "               ('ioporder', 'metadata', 'export', 'tagging', 'collect', 'basecurve'))"
     "  OR (name = ?13)) AND op_version = ?14",
     -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, operation, -1, SQLITE_TRANSIENT);
  _bind_match(stmt, match, TRUE);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 13, always_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 14, op_version);

  GList *names = NULL;
  while(sqlite3_step(stmt) == SQLITE_ROW)
    names = g_list_prepend(names, g_strdup((const char *)sqlite3_column_text(stmt, 0)));
  sqlite3_finalize(stmt);
  return g_list_reverse(names);
}


/* ---------------------------------------------------------------------------------------
 *  The preset edit dialog
 * ------------------------------------------------------------------------------------- */

void dt_preset_conditions_free(dt_preset_conditions_t *c)
{
  if(IS_NULL_PTR(c)) return;
  dt_free(c->name);
  dt_free(c->description);
  dt_free(c->model);
  dt_free(c->maker);
  dt_free(c->lens);
}

gboolean dt_preset_repository_get_conditions(const char *operation, const int op_version,
                                             const char *name, dt_preset_conditions_t *c, int *rowid)
{
  if(IS_NULL_PTR(c) || IS_NULL_PTR(rowid)) return FALSE;
  *rowid = -1;

  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
     "SELECT rowid, description, model, maker, lens, iso_min, iso_max, "
     "       exposure_min, exposure_max, aperture_min, aperture_max, focal_length_min, "
     "       focal_length_max, autoapply, filter, format"
     " FROM data.presets"
     " WHERE name = ?1 AND operation = ?2 AND op_version = ?3",
     -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, op_version);

  gboolean found = FALSE;
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    found = TRUE;
    *rowid = sqlite3_column_int(stmt, 0);
    c->name = g_strdup(name);
    c->description = _column_text(stmt, 1);
    c->model = _column_text(stmt, 2);
    c->maker = _column_text(stmt, 3);
    c->lens = _column_text(stmt, 4);
    c->iso_min = sqlite3_column_double(stmt, 5);
    c->iso_max = sqlite3_column_double(stmt, 6);
    c->exposure_min = sqlite3_column_double(stmt, 7);
    c->exposure_max = sqlite3_column_double(stmt, 8);
    c->aperture_min = sqlite3_column_double(stmt, 9);
    c->aperture_max = sqlite3_column_double(stmt, 10);
    c->focal_length_min = sqlite3_column_double(stmt, 11);
    c->focal_length_max = sqlite3_column_double(stmt, 12);
    c->autoapply = sqlite3_column_int(stmt, 13);
    c->filter = sqlite3_column_int(stmt, 14);
    c->format = sqlite3_column_int(stmt, 15);
  }
  sqlite3_finalize(stmt);
  return found;
}

/* Bind the sixteen condition columns, which the update and the insert share in the same
 * order. */
static void _bind_conditions(sqlite3_stmt *stmt, const dt_preset_conditions_t *c)
{
  _bind_text(stmt, 1, c->name);
  _bind_text(stmt, 2, c->description);
  _bind_text(stmt, 3, c->model);
  _bind_text(stmt, 4, c->maker);
  _bind_text(stmt, 5, c->lens);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 6, c->iso_min);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 7, c->iso_max);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 8, c->exposure_min);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 9, c->exposure_max);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 10, c->aperture_min);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 11, c->aperture_max);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 12, c->focal_length_min);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 13, c->focal_length_max);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 14, c->autoapply);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 15, c->filter);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 16, c->format);
}

void dt_preset_repository_update_conditions(const int rowid, const dt_preset_conditions_t *c)
{
  if(IS_NULL_PTR(c)) return;

  sqlite3_stmt *stmt = NULL;
  /* rowid is bound as ?17. It used to be pasted in with g_strdup_printf("... rowid=%d"),
   * which meant building the query text on every save for a value that is an integer from
   * the database. */
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "UPDATE data.presets "
                              "SET"
                              " name=?1, description=?2,"
                              " model=?3, maker=?4, lens=?5, iso_min=?6, iso_max=?7, exposure_min=?8,"
                              " exposure_max=?9, aperture_min=?10,"
                              " aperture_max=?11, focal_length_min=?12, focal_length_max=?13, autoapply=?14,"
                              " filter=?15, format=?16 "
                              "WHERE rowid=?17",
                              -1, &stmt, NULL);
  // clang-format on
  _bind_conditions(stmt, c);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 17, rowid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_preset_repository_insert_with_conditions(const dt_preset_conditions_t *c,
                                                 const char *operation, const int op_version,
                                                 const void *params, const int params_size,
                                                 const int enabled,
                                                 const void *blend_params, const int blend_params_size,
                                                 const int blendop_version)
{
  if(IS_NULL_PTR(c)) return;

  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "INSERT INTO data.presets"
                              " (name, description, "
                              "  model, maker, lens, iso_min, iso_max, exposure_min, exposure_max, aperture_min,"
                              "  aperture_max, focal_length_min, focal_length_max, autoapply,"
                              "  filter, format, def, writeprotect, operation, op_version, op_params, enabled,"
                              "  blendop_params, blendop_version, multi_priority, multi_name) "
                              "VALUES"
                              " (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, 0, 0, ?17,"
                              "  ?18, ?19, ?20, ?21, ?22, 0, '')",
                              -1, &stmt, NULL);
  // clang-format on
  _bind_conditions(stmt, c);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 17, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 18, op_version);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 19, params, params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 20, enabled);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 21, blend_params, blend_params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 22, blendop_version);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

gboolean dt_preset_repository_get_identity(const int rowid, gchar **operation, int *op_version)
{
  if(IS_NULL_PTR(operation) || IS_NULL_PTR(op_version)) return FALSE;
  *operation = NULL;
  *op_version = 0;

  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT operation, op_version FROM data.presets WHERE rowid = ?1",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, rowid);

  gboolean found = FALSE;
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    found = TRUE;
    *operation = _column_text(stmt, 0);
    *op_version = sqlite3_column_int(stmt, 1);
  }
  sqlite3_finalize(stmt);
  return found;
}

void dt_preset_repository_update_iop_params(const char *operation, const char *name,
                                            const int op_version,
                                            const void *params, const int params_size,
                                            const int enabled,
                                            const void *blend_params, const int blend_params_size,
                                            const int blendop_version)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "UPDATE data.presets"
                              " SET op_version=?2, op_params=?3, enabled=?4,"
                              "     blendop_params=?5, blendop_version=?6"
                              " WHERE name=?7 AND operation=?1",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, op_version);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 3, params, params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, enabled);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 5, blend_params, blend_params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 6, blendop_version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 7, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_preset_repository_delete_by_rowid_unprotected(const int rowid)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "DELETE FROM data.presets WHERE rowid=?1 AND writeprotect=0",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, rowid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_preset_repository_delete_shipped(void)
{
  DT_DEBUG_SQLITE3_EXEC(dt_database_get_sqlite3_global(),
                        "DELETE FROM data.presets WHERE writeprotect = 1", NULL, NULL, NULL);
}

void dt_preset_row_free(gpointer data)
{
  dt_preset_row_t *row = (dt_preset_row_t *)data;
  if(IS_NULL_PTR(row)) return;
  dt_free(row->name);
  dt_free(row->operation);
  dt_free(row->model);
  dt_free(row->maker);
  dt_free(row->lens);
  dt_free(row);
}

GList *dt_preset_repository_list_all(void)
{
  GList *rows = NULL;
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT rowid, name, operation, autoapply, model, maker, lens, iso_min, "
                              "iso_max, exposure_min, exposure_max, aperture_min, aperture_max, "
                              "focal_length_min, focal_length_max, writeprotect FROM data.presets ORDER BY "
                              "operation, name",
                              -1, &stmt, NULL);
  // clang-format on
  if(IS_NULL_PTR(stmt)) return NULL;

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_preset_row_t *row = (dt_preset_row_t *)calloc(1, sizeof(dt_preset_row_t));
    if(IS_NULL_PTR(row)) break;
    row->rowid = sqlite3_column_int(stmt, 0);
    // NULL is preserved rather than normalised to "" by _column_text(): the caller passes
    // these straight to g_strcmp0()/g_strdup() and into a GtkTreeStore, all of which treat
    // the two differently, and the loop this replaced handed them the raw column.
    row->name = _column_text_or_null(stmt, 1);
    row->operation = _column_text_or_null(stmt, 2);
    row->autoapply = (sqlite3_column_int(stmt, 3) == 0 ? FALSE : TRUE);
    row->model = _column_text_or_null(stmt, 4);
    row->maker = _column_text_or_null(stmt, 5);
    row->lens = _column_text_or_null(stmt, 6);
    row->iso_min = sqlite3_column_double(stmt, 7);
    row->iso_max = sqlite3_column_double(stmt, 8);
    row->exposure_min = sqlite3_column_double(stmt, 9);
    row->exposure_max = sqlite3_column_double(stmt, 10);
    row->aperture_min = sqlite3_column_double(stmt, 11);
    row->aperture_max = sqlite3_column_double(stmt, 12);
    row->focal_length_min = sqlite3_column_double(stmt, 13);
    row->focal_length_max = sqlite3_column_double(stmt, 14);
    row->writeprotect = (sqlite3_column_int(stmt, 15) == 0 ? FALSE : TRUE);
    rows = g_list_prepend(rows, row);
  }
  sqlite3_finalize(stmt);

  // Row order, not reverse-row: the caller walks it once and starts a new module node every
  // time `operation` changes, which only works in the ORDER BY's own order.
  return g_list_reverse(rows);
}

void dt_preset_identity_free(gpointer data)
{
  dt_preset_identity_t *row = (dt_preset_identity_t *)data;
  if(IS_NULL_PTR(row)) return;
  dt_free(row->name);
  dt_free(row->operation);
  dt_free(row);
}

GList *dt_preset_repository_list_editable(void)
{
  GList *rows = NULL;
  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT rowid, name, operation FROM data.presets WHERE writeprotect = 0",
                              -1, &stmt, NULL);
  if(IS_NULL_PTR(stmt)) return NULL;

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_preset_identity_t *row = (dt_preset_identity_t *)calloc(1, sizeof(dt_preset_identity_t));
    if(IS_NULL_PTR(row)) break;
    row->rowid = sqlite3_column_int(stmt, 0);
    row->name = _column_text_or_null(stmt, 1);
    row->operation = _column_text_or_null(stmt, 2);
    rows = g_list_prepend(rows, row);
  }
  sqlite3_finalize(stmt);

  return g_list_reverse(rows);  // row order: the caller writes one file per row, in order
}

void dt_preset_repository_cleanup(void)
{
  /* Nothing cached any more: every statement in this file is prepared and finalised per
   * call. Kept because the connection's close order calls every repository's cleanup. */
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
