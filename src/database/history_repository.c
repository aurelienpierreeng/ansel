/*
    This file is part of darktable,
    Copyright (C) 2009-2011 johannes hanika.
    Copyright (C) 2010 Henrik Andersson.
    Copyright (C) 2011, 2014-2016 Tobias Ellinghaus.
    Copyright (C) 2012, 2019-2022 Pascal Obry.
    Copyright (C) 2018 Edgardo Hoszowski.
    Copyright (C) 2025-2026 Aurélien PIERRE.

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

#include <float.h>
#include <math.h>
#include <string.h>

#include "database/history_repository.h"
#include "database/database.h"
#include "database/sql_debug.h"
#include "system/dtpthread.h"
#include "system/macros.h"
#include "system/mem_alloc.h"

// Every statement below is prepared on first use and kept until dt_history_repository_cleanup().
// One mutex covers all of them: a cached sqlite3_stmt carries its own bindings and its own row
// cursor, so two threads stepping the same statement would interleave both.



static sqlite3_stmt *_history_check_module_exists_stmt = NULL;
static sqlite3_stmt *_history_count_items_stmt = NULL;
static sqlite3_stmt *_history_get_end_stmt = NULL;
static sqlite3_stmt *_history_set_end_stmt = NULL;
static sqlite3_stmt *_history_get_next_num_stmt = NULL;

static sqlite3_stmt *_history_delete_history_stmt = NULL;
static sqlite3_stmt *_history_delete_masks_stmt = NULL;
static sqlite3_stmt *_history_shift_history_nums_stmt = NULL;
static sqlite3_stmt *_history_select_history_stmt = NULL;
static sqlite3_stmt *_history_select_num_stmt = NULL;
static sqlite3_stmt *_history_insert_num_stmt = NULL;
static sqlite3_stmt *_history_update_item_stmt = NULL;

static sqlite3_stmt *_history_auto_presets_stmt = NULL;
static sqlite3_stmt *_history_auto_presets_legacy_stmt = NULL;
static sqlite3_stmt *_history_auto_ioporder_stmt = NULL;

static sqlite3_stmt *_module_order_select_stmt = NULL;
static sqlite3_stmt *_module_order_select_version_stmt = NULL;
static sqlite3_stmt *_module_order_insert_stmt = NULL;
static sqlite3_stmt *_module_order_update_list_stmt = NULL;
static sqlite3_stmt *_module_order_update_null_stmt = NULL;
static dt_pthread_mutex_t _history_stmt_mutex;
static gsize _history_stmt_mutex_inited = 0;

static inline void _history_stmt_mutex_ensure(void)
{
  if(g_once_init_enter(&_history_stmt_mutex_inited))
  {
    dt_pthread_mutex_init(&_history_stmt_mutex, NULL);
    g_once_init_leave(&_history_stmt_mutex_inited, 1);
  }
}



int32_t dt_history_repository_get_end(const int32_t imgid)
{
  if(imgid <= 0) return 0;

  int32_t end = 0;
  _history_stmt_mutex_ensure();
  dt_pthread_mutex_lock(&_history_stmt_mutex);
  if(!_history_get_end_stmt)
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "SELECT history_end FROM main.images WHERE id=?1", -1,
                                &_history_get_end_stmt, NULL);
  }
  sqlite3_stmt *stmt = _history_get_end_stmt;
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL)
    end = sqlite3_column_int(stmt, 0);
  dt_pthread_mutex_unlock(&_history_stmt_mutex);

  return end;
}

gboolean dt_history_repository_set_end(const int32_t imgid, const int32_t history_end)
{
  if(imgid <= 0) return FALSE;

  _history_stmt_mutex_ensure();
  dt_pthread_mutex_lock(&_history_stmt_mutex);
  if(!_history_set_end_stmt)
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "UPDATE main.images SET history_end = ?1 WHERE id = ?2", -1,
                                &_history_set_end_stmt, NULL);
  }
  sqlite3_stmt *stmt = _history_set_end_stmt;
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, history_end);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
  const gboolean ok = (sqlite3_step(stmt) == SQLITE_DONE);
  dt_pthread_mutex_unlock(&_history_stmt_mutex);
  return ok;
}

int32_t dt_history_repository_get_next_num(const int32_t imgid)
{
  if(imgid <= 0) return 0;

  int32_t next_num = 0;
  _history_stmt_mutex_ensure();
  dt_pthread_mutex_lock(&_history_stmt_mutex);
  if(!_history_get_next_num_stmt)
  {
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "SELECT IFNULL(MAX(num)+1, 0) FROM main.history"
                                " WHERE imgid = ?1",
                                -1, &_history_get_next_num_stmt, NULL);
    // clang-format on
  }
  sqlite3_stmt *stmt = _history_get_next_num_stmt;
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
    next_num = sqlite3_column_int(stmt, 0);
  dt_pthread_mutex_unlock(&_history_stmt_mutex);
  return next_num;
}

gboolean dt_history_repository_shift_nums(const int32_t imgid, const int delta)
{
  if(imgid <= 0 || delta == 0) return TRUE;

  _history_stmt_mutex_ensure();
  dt_pthread_mutex_lock(&_history_stmt_mutex);
  if(!_history_shift_history_nums_stmt)
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "UPDATE main.history SET num = num + ?2 WHERE imgid = ?1", -1,
                                &_history_shift_history_nums_stmt, NULL);
  }
  sqlite3_stmt *stmt = _history_shift_history_nums_stmt;
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, delta);
  const gboolean ok = (sqlite3_step(stmt) == SQLITE_DONE);
  dt_pthread_mutex_unlock(&_history_stmt_mutex);
  return ok;
}

gboolean dt_history_repository_write_item(const int32_t imgid, const int num, const char *operation, const void *op_params,
                                         const int op_params_size, const int module_version, const gboolean enabled,
                                         const void *blendop_params, const int blendop_params_size,
                                         const int blendop_version, const int multi_priority, const char *multi_name)
{
  if(imgid <= 0 || num < 0 || IS_NULL_PTR(operation)) return FALSE;

  gboolean ok = TRUE;

  _history_stmt_mutex_ensure();
  dt_pthread_mutex_lock(&_history_stmt_mutex);

  if(!_history_select_num_stmt)
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "SELECT num FROM main.history WHERE imgid = ?1 AND num = ?2", -1,
                                &_history_select_num_stmt, NULL);
  sqlite3_stmt *stmt = _history_select_num_stmt;
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, num);
  if(sqlite3_step(stmt) != SQLITE_ROW)
  {
    if(!_history_insert_num_stmt)
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                  "INSERT INTO main.history (imgid, num) VALUES (?1, ?2)", -1,
                                  &_history_insert_num_stmt, NULL);
    stmt = _history_insert_num_stmt;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, num);
    ok &= (sqlite3_step(stmt) == SQLITE_DONE);
  }

  if(!_history_update_item_stmt)
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "UPDATE main.history"
                                " SET operation = ?1, op_params = ?2, module = ?3, enabled = ?4, "
                                "     blendop_params = ?7, blendop_version = ?8, multi_priority = ?9, multi_name = ?10"
                                " WHERE imgid = ?5 AND num = ?6",
                                -1, &_history_update_item_stmt, NULL);
    // clang-format on
  stmt = _history_update_item_stmt;
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 2, op_params, op_params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, module_version);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, enabled);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 5, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 6, num);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 7, blendop_params, blendop_params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 8, blendop_version);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 9, multi_priority);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 10, multi_name ? multi_name : "", -1, SQLITE_TRANSIENT);
  ok &= (sqlite3_step(stmt) == SQLITE_DONE);

  dt_pthread_mutex_unlock(&_history_stmt_mutex);
  return ok;
}

gboolean dt_history_repository_delete_history(const int32_t imgid)
{
  if(imgid <= 0) return FALSE;

  _history_stmt_mutex_ensure();
  dt_pthread_mutex_lock(&_history_stmt_mutex);
  if(!_history_delete_history_stmt)
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "DELETE FROM main.history WHERE imgid = ?1", -1,
                                &_history_delete_history_stmt, NULL);
  sqlite3_stmt *stmt = _history_delete_history_stmt;
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  const gboolean ok = (sqlite3_step(stmt) == SQLITE_DONE);
  dt_pthread_mutex_unlock(&_history_stmt_mutex);
  return ok;
}

gboolean dt_history_repository_delete_masks_history(const int32_t imgid)
{
  if(imgid <= 0) return FALSE;

  _history_stmt_mutex_ensure();
  dt_pthread_mutex_lock(&_history_stmt_mutex);
  if(!_history_delete_masks_stmt)
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "DELETE FROM main.masks_history WHERE imgid = ?1", -1,
                                &_history_delete_masks_stmt, NULL);
  sqlite3_stmt *stmt = _history_delete_masks_stmt;
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  const gboolean ok = (sqlite3_step(stmt) == SQLITE_DONE);
  dt_pthread_mutex_unlock(&_history_stmt_mutex);
  return ok;
}

gboolean dt_history_repository_delete_dev_history(const int32_t imgid)
{
  if(imgid <= 0) return FALSE;
  gboolean ok = TRUE;
  ok &= dt_history_repository_delete_history(imgid);
  ok &= dt_history_repository_delete_masks_history(imgid);
  return ok;
}

void dt_history_repository_foreach_row(const int32_t imgid, dt_history_repository_row_cb cb, void *user_data)
{
  if(imgid <= 0 || IS_NULL_PTR(cb)) return;

  _history_stmt_mutex_ensure();
  dt_pthread_mutex_lock(&_history_stmt_mutex);
  if(!_history_select_history_stmt)
  {
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "SELECT imgid, num, module, operation,"
                                "       op_params, enabled, blendop_params,"
                                "       blendop_version, multi_priority, multi_name"
                                " FROM main.history"
                                " WHERE imgid = ?1"
                                " ORDER BY num",
                                -1, &_history_select_history_stmt, NULL);
    // clang-format on
  }

  sqlite3_stmt *stmt = _history_select_history_stmt;
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int32_t id = sqlite3_column_int(stmt, 0);
    const int num = sqlite3_column_int(stmt, 1);
    const int modversion = sqlite3_column_int(stmt, 2);
    const char *operation = (const char *)sqlite3_column_text(stmt, 3);
    const void *module_params = sqlite3_column_blob(stmt, 4);
    const gboolean enabled = sqlite3_column_int(stmt, 5) != 0; // ensure casting to gboolean
    const void *blendop_params = sqlite3_column_blob(stmt, 6);
    const int blendop_version = sqlite3_column_int(stmt, 7);
    const int multi_priority = sqlite3_column_int(stmt, 8);
    const char *multi_name = (const char *)sqlite3_column_text(stmt, 9);
    const int param_length = sqlite3_column_bytes(stmt, 4);
    const int bl_length = sqlite3_column_bytes(stmt, 6);

    cb(user_data, id, num, modversion, operation, module_params, param_length, enabled,
       blendop_params, bl_length, blendop_version, multi_priority, multi_name, "");
  }

  dt_pthread_mutex_unlock(&_history_stmt_mutex);
}

void dt_history_repository_foreach_auto_preset_row(const int32_t imgid, const dt_image_t *image, const char *workflow_preset,
                                          const int iformat, const int excluded, dt_history_repository_row_cb cb, void *user_data)
{
  if(imgid <= 0 || IS_NULL_PTR(image) || !workflow_preset || IS_NULL_PTR(cb)) return;

  _history_stmt_mutex_ensure();
  dt_pthread_mutex_lock(&_history_stmt_mutex);

  const gboolean use_modern_presets = (image->flags & DT_IMAGE_NO_LEGACY_PRESETS);
  sqlite3_stmt **stmt_ptr = use_modern_presets ? &_history_auto_presets_stmt : &_history_auto_presets_legacy_stmt;
  if(!*stmt_ptr)
  {
    const char *table = use_modern_presets ? "data.presets" : "main.legacy_presets";

    // clang-format off
    char *query = g_strdup_printf(
      " SELECT ?1, 0, op_version, operation, op_params,"
      "       enabled, blendop_params, blendop_version, multi_priority, multi_name, name"
      " FROM %s"
      " WHERE ( (autoapply=1"
      "          AND ((?2 LIKE model AND ?3 LIKE maker) OR (?4 LIKE model AND ?5 LIKE maker))"
      "          AND ?6 LIKE lens AND ?7 BETWEEN iso_min AND iso_max"
      "          AND ?8 BETWEEN exposure_min AND exposure_max"
      "          AND ?9 BETWEEN aperture_min AND aperture_max"
      "          AND ?10 BETWEEN focal_length_min AND focal_length_max"
      "          AND (format = 0 OR (format & ?11 != 0 AND ~format & ?12 != 0)))"
      "        OR (name = ?13))"
      "   AND operation NOT IN"
      "        ('ioporder', 'metadata', 'modulegroups', 'export', 'tagging', 'collect', 'basecurve')"
      " ORDER BY writeprotect DESC, LENGTH(model), LENGTH(maker), LENGTH(lens)",
      table);
    // clang-format on

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), query, -1, stmt_ptr, NULL);
    dt_free(query);
  }

  sqlite3_stmt *stmt = *stmt_ptr;
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, image->exif_model, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, image->exif_maker, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, image->camera_alias, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, image->camera_maker, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 6, image->exif_lens, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 7, fmaxf(0.0f, fminf(FLT_MAX, image->exif_iso)));
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 8, fmaxf(0.0f, fminf(1000000, image->exif_exposure)));
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 9, fmaxf(0.0f, fminf(1000000, image->exif_aperture)));
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 10, fmaxf(0.0f, fminf(1000000, image->exif_focal_length)));
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 11, iformat);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 12, excluded);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 13, workflow_preset, -1, SQLITE_TRANSIENT);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int32_t id = sqlite3_column_int(stmt, 0);
    const int num = sqlite3_column_int(stmt, 1);
    const int modversion = sqlite3_column_int(stmt, 2);
    const char *operation = (const char *)sqlite3_column_text(stmt, 3);
    const void *module_params = sqlite3_column_blob(stmt, 4);
    const int enabled = sqlite3_column_int(stmt, 5);
    const void *blendop_params = sqlite3_column_blob(stmt, 6);
    const int blendop_version = sqlite3_column_int(stmt, 7);
    const int multi_priority = sqlite3_column_int(stmt, 8);
    const char *multi_name = (const char *)sqlite3_column_text(stmt, 9);
    const char *preset_name = (const char *)sqlite3_column_text(stmt, 10);
    const int param_length = sqlite3_column_bytes(stmt, 4);
    const int bl_length = sqlite3_column_bytes(stmt, 6);

    cb(user_data, id, num, modversion, operation, module_params, param_length, enabled,
       blendop_params, bl_length, blendop_version, multi_priority, multi_name, preset_name);
  }

  dt_pthread_mutex_unlock(&_history_stmt_mutex);
}

gboolean dt_history_repository_get_autoapply_ioporder_params(const int32_t imgid, const dt_image_t *image,
                                                    const int iformat, const int excluded, void **params,
                                                    int32_t *params_len)
{
  if(imgid <= 0 || IS_NULL_PTR(image) || IS_NULL_PTR(params) || !params_len) return FALSE;
  *params = NULL;
  *params_len = 0;

  _history_stmt_mutex_ensure();
  dt_pthread_mutex_lock(&_history_stmt_mutex);

  if(!_history_auto_ioporder_stmt)
  {
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "SELECT op_params"
                                " FROM data.presets"
                                " WHERE autoapply=1"
                                "       AND ((?2 LIKE model AND ?3 LIKE maker) OR (?4 LIKE model AND ?5 LIKE maker))"
                                "       AND ?6 LIKE lens AND ?7 BETWEEN iso_min AND iso_max"
                                "       AND ?8 BETWEEN exposure_min AND exposure_max"
                                "       AND ?9 BETWEEN aperture_min AND aperture_max"
                                "       AND ?10 BETWEEN focal_length_min AND focal_length_max"
                                "       AND (format = 0 OR (format & ?11 != 0 AND ~format & ?12 != 0))"
                                "       AND operation = 'ioporder'"
                                " ORDER BY writeprotect DESC, LENGTH(model), LENGTH(maker), LENGTH(lens)",
                                -1, &_history_auto_ioporder_stmt, NULL);
    // clang-format on
  }

  sqlite3_stmt *stmt = _history_auto_ioporder_stmt;
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, image->exif_model, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, image->exif_maker, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, image->camera_alias, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, image->camera_maker, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 6, image->exif_lens, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 7, fmaxf(0.0f, fminf(FLT_MAX, image->exif_iso)));
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 8, fmaxf(0.0f, fminf(1000000, image->exif_exposure)));
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 9, fmaxf(0.0f, fminf(1000000, image->exif_aperture)));
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 10, fmaxf(0.0f, fminf(1000000, image->exif_focal_length)));
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 11, iformat);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 12, excluded);

  gboolean ok = FALSE;
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const void *blob = sqlite3_column_blob(stmt, 0);
    const int32_t blob_len = sqlite3_column_bytes(stmt, 0);
    if(blob && blob_len > 0)
    {
      *params = g_malloc(blob_len);
      memcpy(*params, blob, blob_len);
      *params_len = blob_len;
      ok = TRUE;
    }
  }

  dt_pthread_mutex_unlock(&_history_stmt_mutex);
  return ok;
}

int dt_history_repository_count_items(const int32_t imgid)
{
  int found_it = 0;

  _history_stmt_mutex_ensure();
  dt_pthread_mutex_lock(&_history_stmt_mutex);
  if(IS_NULL_PTR(_history_count_items_stmt))
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "SELECT COUNT(imgid) FROM main.history WHERE imgid = ?1", -1,
                                &_history_count_items_stmt, NULL);
  }
  sqlite3_stmt *stmt = _history_count_items_stmt;
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
    found_it = sqlite3_column_int(stmt, 0);

  dt_pthread_mutex_unlock(&_history_stmt_mutex);

  return found_it;
}

gboolean dt_history_repository_get_last_enabled_params(const int32_t imgid, const char *operation,
                                                       void **params, int32_t *params_len)
{
  if(imgid <= 0 || IS_NULL_PTR(operation) || IS_NULL_PTR(params) || IS_NULL_PTR(params_len))
    return FALSE;

  *params = NULL;
  *params_len = 0;

  gboolean ok = FALSE;
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
    dt_database_get_sqlite3_global(),
    "SELECT op_params, enabled"
    " FROM main.history"
    " WHERE imgid=?1 AND operation=?2"
    " ORDER BY num DESC LIMIT 1", -1,
    &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, operation, -1, SQLITE_TRANSIENT);
  if(sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 1) != 0)
  {
    // The blob dies with the statement and the caller reads it through module introspection
    // well after, so it is copied out rather than pointed at.
    const void *blob = sqlite3_column_blob(stmt, 0);
    const int32_t len = sqlite3_column_bytes(stmt, 0);
    if(blob && len > 0)
    {
      *params = g_malloc(len);
      memcpy(*params, blob, len);
      *params_len = len;
      ok = TRUE;
    }
  }
  sqlite3_finalize(stmt);
  return ok;
}

gboolean dt_history_repository_module_exists(const int32_t imgid, const char *operation)
{
  gboolean result = FALSE;
  _history_stmt_mutex_ensure();
  dt_pthread_mutex_lock(&_history_stmt_mutex);
  if(!_history_check_module_exists_stmt)
  {
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get_sqlite3_global(),
      "SELECT imgid"
      " FROM main.history"
      " WHERE imgid= ?1 AND operation = ?2",
      -1, &_history_check_module_exists_stmt, NULL);
    // clang-format on
  }
  sqlite3_stmt *stmt = _history_check_module_exists_stmt;
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, operation, -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) == SQLITE_ROW) result = TRUE;
  dt_pthread_mutex_unlock(&_history_stmt_mutex);

  return result;
}


gboolean dt_history_repository_delete_all_for_image(const int32_t imgid)
{
  if(imgid <= 0) return FALSE;

  // Not cached: this runs once per "discard history", never in a loop, and five more permanent
  // statements would cost more than the preparation does.
  gboolean ok = TRUE;
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "DELETE FROM main.history WHERE imgid = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  ok &= (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "DELETE FROM main.module_order WHERE imgid = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  ok &= (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "UPDATE main.images"
                              " SET history_end = 0, aspect_ratio = 0.0"
                              " WHERE id = ?1",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  ok &= (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "DELETE FROM main.masks_history WHERE imgid = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  ok &= (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "DELETE FROM main.history_hash WHERE imgid = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  ok &= (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);

  return ok;
}


void dt_history_repository_foreach_last_item(const int32_t imgid, const gboolean enabled,
                                             dt_history_repository_item_cb cb, void *user_data)
{
  if(imgid <= 0 || IS_NULL_PTR(cb)) return;

  sqlite3_stmt *stmt;

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT num, operation, enabled, multi_name"
                              " FROM main.history"
                              " WHERE imgid=?1"
                              "   AND num IN (SELECT MAX(num)"
                              "               FROM main.history hst2"
                              "               WHERE hst2.imgid=?1"
                              "                 AND hst2.operation=main.history.operation"
                              "               GROUP BY multi_priority)"
                              "   AND enabled in (1, ?2)"
                              " ORDER BY num DESC",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, enabled ? 1 : 0);

  while(sqlite3_step(stmt) == SQLITE_ROW)
    cb(user_data, sqlite3_column_int(stmt, 0), (const char *)sqlite3_column_text(stmt, 1),
       sqlite3_column_int(stmt, 2), (const char *)sqlite3_column_text(stmt, 3));

  sqlite3_finalize(stmt);
}

void dt_history_repository_foreach_item(const int32_t imgid, dt_history_repository_item_cb cb,
                                        void *user_data)
{
  if(imgid <= 0 || IS_NULL_PTR(cb)) return;

  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get_sqlite3_global(),
      "SELECT operation, enabled, multi_name"
      " FROM main.history"
      " WHERE imgid=?1 ORDER BY num DESC", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  while(sqlite3_step(stmt) == SQLITE_ROW)
    cb(user_data, 0, (const char *)sqlite3_column_text(stmt, 0), sqlite3_column_int(stmt, 1),
       (const char *)sqlite3_column_text(stmt, 2));

  sqlite3_finalize(stmt);
}

gboolean dt_history_repository_write_mask_item(const int32_t imgid, const int num, const int formid,
                                               const int form, const char *name, const int version,
                                               const void *points, const int points_len,
                                               const int points_count, const void *source,
                                               const int source_len)
{
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
    dt_database_get_sqlite3_global(),
                              "INSERT INTO main.masks_history (imgid, num, formid, form, name, version, points, points_count, source) "
                              "VALUES (?1, ?9, ?2, ?3, ?4, ?5, ?6, ?7, ?8)",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, formid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, form);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 5, version);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 6, points, points_len, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 7, points_count);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 8, source, source_len, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 9, num);

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return (rc == SQLITE_DONE);
}

int dt_history_repository_count_mask_items(const int32_t imgid)
{
  int num_masks = 0;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT COUNT(*) FROM main.masks_history WHERE imgid = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
    num_masks = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return num_masks;
}

void dt_history_repository_foreach_mask_item(const int32_t imgid,
                                             dt_history_repository_mask_cb cb, void *user_data)
{
  if(IS_NULL_PTR(cb)) return;

  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get_sqlite3_global(),
      "SELECT imgid, formid, form, name, version, points, points_count, source, num"
      " FROM main.masks_history"
      " WHERE imgid = ?1"
      " ORDER BY num",
      -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    cb(user_data, sqlite3_column_int(stmt, 8), sqlite3_column_int(stmt, 1),
       sqlite3_column_int(stmt, 2), (const char *)sqlite3_column_text(stmt, 3),
       sqlite3_column_int(stmt, 4), sqlite3_column_blob(stmt, 5), sqlite3_column_bytes(stmt, 5),
       sqlite3_column_int(stmt, 6), sqlite3_column_blob(stmt, 7), sqlite3_column_bytes(stmt, 7));
  }
  sqlite3_finalize(stmt);
}

int dt_history_repository_find_version_for_params(const char *operation, const void *op_params,
                                                  const int op_params_size)
{
  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT module FROM main.history WHERE operation = ?1 AND op_params = ?2",
                              -1, &stmt, NULL);
  if(IS_NULL_PTR(stmt)) return 0;

  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 2, op_params, op_params_size, SQLITE_TRANSIENT);

  const int version = (sqlite3_step(stmt) == SQLITE_ROW) ? sqlite3_column_int(stmt, 0) : 0;
  sqlite3_finalize(stmt);

  return version;
}

void dt_history_repository_foreach_active_module(const int32_t imgid,
                                                 dt_history_repository_active_module_cb cb,
                                                 void *user_data)
{
  if(imgid <= 0 || IS_NULL_PTR(cb)) return;

  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT MIN(num) AS num, operation, multi_name "
                              "FROM main.history "
                              "WHERE imgid = ?1 AND enabled = 1 "
                              "GROUP BY operation, multi_name "
                              "ORDER BY MIN(num) ASC", -1, &stmt, NULL);
  // clang-format on
  if(IS_NULL_PTR(stmt)) return;

  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  while(sqlite3_step(stmt) == SQLITE_ROW)
    cb(user_data, sqlite3_column_int(stmt, 0), (const char *)sqlite3_column_text(stmt, 1),
       (const char *)sqlite3_column_text(stmt, 2));
  sqlite3_finalize(stmt);
}

/* ---- main.module_order --------------------------------------------------------------- */

gboolean dt_history_repository_get_module_order_version(const int32_t imgid, int *version)
{
  if(imgid <= 0 || IS_NULL_PTR(version)) return FALSE;

  _history_stmt_mutex_ensure();
  dt_pthread_mutex_lock(&_history_stmt_mutex);
  if(!_module_order_select_version_stmt)
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "SELECT version FROM main.module_order WHERE imgid = ?1", -1,
                                &_module_order_select_version_stmt, NULL);
  }
  sqlite3_stmt *stmt = _module_order_select_version_stmt;
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  const gboolean found = (sqlite3_step(stmt) == SQLITE_ROW);
  if(found) *version = sqlite3_column_int(stmt, 0);
  dt_pthread_mutex_unlock(&_history_stmt_mutex);

  return found;
}

gboolean dt_history_repository_has_module_order(const int32_t imgid)
{
  int version = 0;
  return dt_history_repository_get_module_order_version(imgid, &version);
}

gboolean dt_history_repository_get_module_order(const int32_t imgid, dt_module_order_row_t *row)
{
  if(IS_NULL_PTR(row)) return FALSE;

  row->version = 0;
  row->iop_list = NULL;
  if(imgid <= 0) return FALSE;

  _history_stmt_mutex_ensure();
  dt_pthread_mutex_lock(&_history_stmt_mutex);
  if(!_module_order_select_stmt)
  {
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "SELECT version, iop_list"
                                " FROM main.module_order"
                                " WHERE imgid=?1", -1, &_module_order_select_stmt, NULL);
    // clang-format on
  }
  sqlite3_stmt *stmt = _module_order_select_stmt;
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  const gboolean found = (sqlite3_step(stmt) == SQLITE_ROW);
  if(found)
  {
    row->version = sqlite3_column_int(stmt, 0);
    if(sqlite3_column_type(stmt, 1) != SQLITE_NULL)
    {
      const char *buf = (const char *)sqlite3_column_text(stmt, 1);
      // the text points into the statement, which the next caller resets -- copy it out
      if(buf) row->iop_list = g_strdup(buf);
    }
  }
  dt_pthread_mutex_unlock(&_history_stmt_mutex);

  return found;
}

gboolean dt_history_repository_has_custom_module_order(const int32_t imgid)
{
  dt_module_order_row_t row = { 0 };
  const gboolean found = dt_history_repository_get_module_order(imgid, &row);
  const gboolean has_list = found && !IS_NULL_PTR(row.iop_list);
  dt_module_order_row_cleanup(&row);
  return has_list;
}

void dt_module_order_row_cleanup(dt_module_order_row_t *row)
{
  if(IS_NULL_PTR(row)) return;
  dt_free(row->iop_list);
  row->iop_list = NULL;
}

gboolean dt_history_repository_set_module_order(const int32_t imgid, const int version,
                                                const char *iop_list)
{
  if(imgid <= 0) return FALSE;

  _history_stmt_mutex_ensure();
  dt_pthread_mutex_lock(&_history_stmt_mutex);

  if(!_module_order_insert_stmt)
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "INSERT OR REPLACE INTO main.module_order VALUES (?1, 0, NULL)",
                                -1, &_module_order_insert_stmt, NULL);
  }
  sqlite3_reset(_module_order_insert_stmt);
  sqlite3_clear_bindings(_module_order_insert_stmt);
  DT_DEBUG_SQLITE3_BIND_INT(_module_order_insert_stmt, 1, imgid);
  gboolean ok = (sqlite3_step(_module_order_insert_stmt) == SQLITE_DONE);

  if(ok && !IS_NULL_PTR(iop_list))
  {
    if(!_module_order_update_list_stmt)
    {
      // clang-format off
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                  "UPDATE main.module_order SET version = ?2, iop_list = ?3"
                                  " WHERE imgid = ?1",
                                  -1, &_module_order_update_list_stmt, NULL);
      // clang-format on
    }
    sqlite3_stmt *stmt = _module_order_update_list_stmt;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, version);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, iop_list, -1, SQLITE_TRANSIENT);
    ok = (sqlite3_step(stmt) == SQLITE_DONE);
  }
  else if(ok)
  {
    if(!_module_order_update_null_stmt)
    {
      // clang-format off
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                  "UPDATE main.module_order SET version = ?2, iop_list = NULL"
                                  " WHERE imgid = ?1",
                                  -1, &_module_order_update_null_stmt, NULL);
      // clang-format on
    }
    sqlite3_stmt *stmt = _module_order_update_null_stmt;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, version);
    ok = (sqlite3_step(stmt) == SQLITE_DONE);
  }

  dt_pthread_mutex_unlock(&_history_stmt_mutex);
  return ok;
}

void dt_history_repository_cleanup(void)
{
  _history_stmt_mutex_ensure();
  dt_pthread_mutex_lock(&_history_stmt_mutex);
  if(_module_order_select_stmt)
  {
    sqlite3_finalize(_module_order_select_stmt);
    _module_order_select_stmt = NULL;
  }
  if(_module_order_select_version_stmt)
  {
    sqlite3_finalize(_module_order_select_version_stmt);
    _module_order_select_version_stmt = NULL;
  }
  if(_module_order_insert_stmt)
  {
    sqlite3_finalize(_module_order_insert_stmt);
    _module_order_insert_stmt = NULL;
  }
  if(_module_order_update_list_stmt)
  {
    sqlite3_finalize(_module_order_update_list_stmt);
    _module_order_update_list_stmt = NULL;
  }
  if(_module_order_update_null_stmt)
  {
    sqlite3_finalize(_module_order_update_null_stmt);
    _module_order_update_null_stmt = NULL;
  }
  if(_history_check_module_exists_stmt)
  {
    sqlite3_finalize(_history_check_module_exists_stmt);
    _history_check_module_exists_stmt = NULL;
  }
  if(_history_count_items_stmt)
  {
    sqlite3_finalize(_history_count_items_stmt);
    _history_count_items_stmt = NULL;
  }
  if(_history_get_end_stmt)
  {
    sqlite3_finalize(_history_get_end_stmt);
    _history_get_end_stmt = NULL;
  }
  if(_history_set_end_stmt)
  {
    sqlite3_finalize(_history_set_end_stmt);
    _history_set_end_stmt = NULL;
  }
  if(_history_get_next_num_stmt)
  {
    sqlite3_finalize(_history_get_next_num_stmt);
    _history_get_next_num_stmt = NULL;
  }
  if(_history_delete_history_stmt)
  {
    sqlite3_finalize(_history_delete_history_stmt);
    _history_delete_history_stmt = NULL;
  }
  if(_history_delete_masks_stmt)
  {
    sqlite3_finalize(_history_delete_masks_stmt);
    _history_delete_masks_stmt = NULL;
  }
  if(_history_shift_history_nums_stmt)
  {
    sqlite3_finalize(_history_shift_history_nums_stmt);
    _history_shift_history_nums_stmt = NULL;
  }
  if(_history_select_history_stmt)
  {
    sqlite3_finalize(_history_select_history_stmt);
    _history_select_history_stmt = NULL;
  }
  if(_history_select_num_stmt)
  {
    sqlite3_finalize(_history_select_num_stmt);
    _history_select_num_stmt = NULL;
  }
  if(_history_insert_num_stmt)
  {
    sqlite3_finalize(_history_insert_num_stmt);
    _history_insert_num_stmt = NULL;
  }
  if(_history_update_item_stmt)
  {
    sqlite3_finalize(_history_update_item_stmt);
    _history_update_item_stmt = NULL;
  }
  if(_history_auto_presets_stmt)
  {
    sqlite3_finalize(_history_auto_presets_stmt);
    _history_auto_presets_stmt = NULL;
  }
  if(_history_auto_presets_legacy_stmt)
  {
    sqlite3_finalize(_history_auto_presets_legacy_stmt);
    _history_auto_presets_legacy_stmt = NULL;
  }
  if(_history_auto_ioporder_stmt)
  {
    sqlite3_finalize(_history_auto_ioporder_stmt);
    _history_auto_ioporder_stmt = NULL;
  }
  dt_pthread_mutex_unlock(&_history_stmt_mutex);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
