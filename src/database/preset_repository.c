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

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
