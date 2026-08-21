/*
    This file is part of darktable,
    Copyright (C) 2010-2011 Henrik Andersson.
    Copyright (C) 2011-2016 Tobias Ellinghaus.
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

#include <stdio.h>
#include <string.h>

#include "database/style_repository.h"
#include "database/database.h"
#include "database/sql_debug.h"
#include "system/dtpthread.h"
#include "system/macros.h"
#include "system/mem_alloc.h"

// Two statements are hot enough to keep prepared: the style list (the styles panel rebuilds it on
// every collection change) and the apply-items read (once per image in a batch style application).
// Everything else here runs at most once per user action and is prepared on the spot.
static sqlite3_stmt *_styles_get_list_stmt = NULL;
static sqlite3_stmt *_styles_apply_items_stmt = NULL;
static dt_pthread_mutex_t _styles_stmt_mutex;
static gsize _styles_stmt_mutex_inited = 0;

static inline void _styles_stmt_mutex_ensure(void)
{
  if(g_once_init_enter(&_styles_stmt_mutex_inited))
  {
    dt_pthread_mutex_init(&_styles_stmt_mutex, NULL);
    g_once_init_leave(&_styles_stmt_mutex_inited, 1);
  }
}

/** "num IN (1,2,3)" / "num NOT IN (1,2,3)" for a GList of GINT_TO_POINTER nums.
 *
 *  The callers used to build this with g_strlcat into a 2048-byte buffer, which truncates in
 *  silence once a style has enough items -- a truncated "num IN (12,3" is not even valid SQL, so
 *  the prepare fails and the operation does nothing. A GString has no such cap. Returns NULL for
 *  an empty list, which every caller reads as "no filter". */
static char *_num_set_predicate(GList *nums, const gboolean negate)
{
  if(IS_NULL_PTR(nums)) return NULL;

  GString *s = g_string_new(negate ? "num NOT IN (" : "num IN (");
  for(GList *l = nums; l; l = g_list_next(l))
  {
    if(l != nums) g_string_append_c(s, ',');
    g_string_append_printf(s, "%d", GPOINTER_TO_INT(l->data));
  }
  g_string_append_c(s, ')');
  return g_string_free(s, FALSE);
}

/* ---------------------------------------------------------------------------------------------
 * data.styles
 * ------------------------------------------------------------------------------------------ */

int32_t dt_style_repository_get_id_by_name(const char *name)
{
  if(IS_NULL_PTR(name)) return 0;

  int id = 0;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT id FROM data.styles WHERE name=?1 ORDER BY id DESC LIMIT 1", -1, &stmt,
                              NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    id = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);
  return id;
}

char *dt_style_repository_get_description(const int32_t styleid)
{
  if(styleid == 0) return NULL;

  gchar *description = NULL;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), "SELECT description FROM data.styles WHERE id=?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, styleid);
  sqlite3_step(stmt);
  description = (char *)sqlite3_column_text(stmt, 0);
  if(description) description = g_strdup(description);
  sqlite3_finalize(stmt);
  return description;
}

char *dt_style_repository_get_iop_list(const char *name)
{
  if(IS_NULL_PTR(name)) return NULL;

  char *iop_list_txt = NULL;
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT iop_list"
                              " FROM data.styles"
                              " WHERE name=?1",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  // No row and a NULL column both report SQLITE_NULL here, which is why a style that does not
  // exist correctly answers "carries no module order".
  if(sqlite3_column_type(stmt, 0) != SQLITE_NULL)
    iop_list_txt = g_strdup((const char *)sqlite3_column_text(stmt, 0));
  sqlite3_finalize(stmt);
  return iop_list_txt;
}

gboolean dt_style_repository_insert_header(const char *name, const char *description,
                                           const char *iop_list_txt)
{
  if(IS_NULL_PTR(name)) return FALSE;

  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get_sqlite3_global(),
      "INSERT INTO data.styles (name, description, id, iop_list)"
      " VALUES (?1, ?2, (SELECT COALESCE(MAX(id),0)+1 FROM data.styles), ?3)", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_STATIC);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, description, -1, SQLITE_STATIC);
  if(iop_list_txt)
  {
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, iop_list_txt, -1, SQLITE_STATIC);
  }
  else
    sqlite3_bind_null(stmt, 3);

  const gboolean ok = (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
  return ok;
}

gboolean dt_style_repository_set_iop_list(const int32_t styleid, const char *iop_list_txt)
{
  if(styleid == 0) return FALSE;

  sqlite3_stmt *stmt;
  if(iop_list_txt)
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "UPDATE data.styles SET iop_list=?1 WHERE id=?2", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, iop_list_txt, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, styleid);
  }
  else
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "UPDATE data.styles SET iop_list=NULL WHERE id=?1", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, styleid);
  }

  const gboolean ok = (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
  return ok;
}

gboolean dt_style_repository_update_header(const int32_t styleid, const char *newname,
                                           const char *description)
{
  if(styleid == 0) return FALSE;

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "UPDATE data.styles SET name=?1, description=?2 WHERE id=?3", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, newname, -1, SQLITE_STATIC);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, description, -1, SQLITE_STATIC);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, styleid);
  const gboolean ok = (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
  return ok;
}

gboolean dt_style_repository_delete(const int32_t styleid)
{
  if(styleid == 0) return FALSE;

  gboolean ok = TRUE;
  sqlite3_stmt *stmt;

  /* delete the style */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), "DELETE FROM data.styles WHERE id = ?1", -1, &stmt,
                              NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, styleid);
  ok &= (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);

  /* delete style_items belonging to style */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), "DELETE FROM data.style_items WHERE styleid = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, styleid);
  ok &= (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);

  return ok;
}

void dt_style_repository_foreach_style(const char *filter, dt_style_repository_style_cb cb,
                                       void *user_data)
{
  if(IS_NULL_PTR(cb)) return;

  char filterstring[512] = { 0 };
  snprintf(filterstring, sizeof(filterstring), "%%%s%%", filter);

  _styles_stmt_mutex_ensure();
  dt_pthread_mutex_lock(&_styles_stmt_mutex);
  if(!_styles_get_list_stmt)
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(
        dt_database_get_sqlite3_global(),
        "SELECT name, description FROM data.styles WHERE name LIKE ?1 OR description LIKE ?1 ORDER BY name", -1,
        &_styles_get_list_stmt, NULL);
  }
  sqlite3_stmt *stmt = _styles_get_list_stmt;
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, filterstring, -1, SQLITE_TRANSIENT);

  while(sqlite3_step(stmt) == SQLITE_ROW)
    cb(user_data, (const char *)sqlite3_column_text(stmt, 0), (const char *)sqlite3_column_text(stmt, 1));

  dt_pthread_mutex_unlock(&_styles_stmt_mutex);
}

gboolean dt_style_repository_get_style(const char *name, dt_style_repository_style_cb cb,
                                       void *user_data)
{
  if(IS_NULL_PTR(name) || IS_NULL_PTR(cb)) return FALSE;

  gboolean found = FALSE;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT name, description FROM data.styles WHERE name = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_STATIC);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    cb(user_data, (const char *)sqlite3_column_text(stmt, 0), (const char *)sqlite3_column_text(stmt, 1));
    found = TRUE;
  }
  sqlite3_finalize(stmt);
  return found;
}

/* ---------------------------------------------------------------------------------------------
 * data.style_items
 * ------------------------------------------------------------------------------------------ */

void dt_style_repository_foreach_apply_item(const int32_t styleid,
                                            dt_style_repository_item_cb cb, void *user_data)
{
  if(IS_NULL_PTR(cb)) return;

  _styles_stmt_mutex_ensure();
  dt_pthread_mutex_lock(&_styles_stmt_mutex);
  if(IS_NULL_PTR(_styles_apply_items_stmt))
  {
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "SELECT num, module, operation, op_params, enabled,"
                                "  blendop_params, blendop_version, multi_priority, multi_name"
                                " FROM data.style_items WHERE styleid=?1 "
                                " ORDER BY num, operation, multi_priority",
                                -1, &_styles_apply_items_stmt, NULL);
    // clang-format on
  }

  sqlite3_stmt *stmt = _styles_apply_items_stmt;
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, styleid);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    // selimg_num is 0 here, not -1: this is the one reader that used it as "no image matched"
    // rather than "not asked", and it wrote 0.
    cb(user_data, sqlite3_column_int(stmt, 0), sqlite3_column_int(stmt, 7), sqlite3_column_int(stmt, 1),
       (const char *)sqlite3_column_text(stmt, 2), sqlite3_column_int(stmt, 4),
       sqlite3_column_blob(stmt, 3), sqlite3_column_bytes(stmt, 3),
       sqlite3_column_blob(stmt, 5), sqlite3_column_bytes(stmt, 5),
       sqlite3_column_int(stmt, 6), (const char *)sqlite3_column_text(stmt, 8), 0, 0.0);
  }

  sqlite3_reset(stmt);
  dt_pthread_mutex_unlock(&_styles_stmt_mutex);
}

void dt_style_repository_foreach_item_for_export(const int32_t styleid,
                                                 dt_style_repository_item_cb cb, void *user_data)
{
  if(IS_NULL_PTR(cb)) return;

  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT num, module, operation, op_params, enabled,"
                              "  blendop_params, blendop_version, multi_priority, multi_name"
                              " FROM data.style_items"
                              " WHERE styleid =?1",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, styleid);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    cb(user_data, sqlite3_column_int(stmt, 0), sqlite3_column_int(stmt, 7), sqlite3_column_int(stmt, 1),
       (const char *)sqlite3_column_text(stmt, 2), sqlite3_column_int(stmt, 4),
       sqlite3_column_blob(stmt, 3), sqlite3_column_bytes(stmt, 3),
       sqlite3_column_blob(stmt, 5), sqlite3_column_bytes(stmt, 5),
       sqlite3_column_int(stmt, 6), (const char *)sqlite3_column_text(stmt, 8), -1, 0.0);
  }
  sqlite3_finalize(stmt);
}

void dt_style_repository_foreach_item_with_params(const int32_t styleid,
                                                  dt_style_repository_item_cb cb, void *user_data)
{
  if(IS_NULL_PTR(cb)) return;

  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT num, multi_priority, module, operation, enabled, op_params, blendop_params, "
                              "       multi_name, blendop_version"
                              " FROM data.style_items"
                              " WHERE styleid=?1 ORDER BY num DESC",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, styleid);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int num = (sqlite3_column_type(stmt, 0) == SQLITE_NULL) ? -1 : sqlite3_column_int(stmt, 0);
    // column 8 twice, once as int and once as double -- exactly what the original did
    cb(user_data, num, sqlite3_column_int(stmt, 1), sqlite3_column_int(stmt, 2),
       (const char *)sqlite3_column_text(stmt, 3), sqlite3_column_int(stmt, 4),
       sqlite3_column_blob(stmt, 5), sqlite3_column_bytes(stmt, 5),
       sqlite3_column_blob(stmt, 6), sqlite3_column_bytes(stmt, 6),
       sqlite3_column_int(stmt, 8), (const char *)sqlite3_column_text(stmt, 7), -1,
       sqlite3_column_double(stmt, 8));
  }
  sqlite3_finalize(stmt);
}

void dt_style_repository_foreach_item(const int32_t styleid, dt_style_repository_item_cb cb,
                                      void *user_data)
{
  if(IS_NULL_PTR(cb)) return;

  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT num, multi_priority, module, operation, enabled, 0, 0, multi_name"
                              " FROM data.style_items"
                              " WHERE styleid=?1 ORDER BY num DESC",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, styleid);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int num = (sqlite3_column_type(stmt, 0) == SQLITE_NULL) ? -1 : sqlite3_column_int(stmt, 0);
    // there is no column 8 in this query; the original read one anyway and got 0 for both the
    // blendop_version and the iop_order. Kept, so the two paths still agree.
    cb(user_data, num, sqlite3_column_int(stmt, 1), sqlite3_column_int(stmt, 2),
       (const char *)sqlite3_column_text(stmt, 3), sqlite3_column_int(stmt, 4),
       NULL, 0, NULL, 0,
       sqlite3_column_int(stmt, 8), (const char *)sqlite3_column_text(stmt, 7), -1,
       sqlite3_column_double(stmt, 8));
  }
  sqlite3_finalize(stmt);
}

void dt_style_repository_foreach_item_against_image(const int32_t styleid, const int32_t imgid,
                                                    dt_style_repository_item_cb cb,
                                                    void *user_data)
{
  if(IS_NULL_PTR(cb)) return;

  sqlite3_stmt *stmt;
  // get all items from the style
  //    UNION
  // get all items from history, not in the style : select only the last operation, that is max(num)
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get_sqlite3_global(),
      "SELECT num, multi_priority, module, operation, enabled,"
      "       (SELECT MAX(num)"
      "        FROM main.history"
      "        WHERE imgid=?2 "
      "          AND operation=data.style_items.operation"
      "          AND multi_priority=data.style_items.multi_priority),"
      "       0, multi_name, blendop_version"
      " FROM data.style_items"
      " WHERE styleid=?1"
      " UNION"
      " SELECT -1,main.history.multi_priority,main.history.module,main.history.operation,main.history.enabled, "
      "        main.history.num,0,multi_name, blendop_version"
      " FROM main.history"
      " WHERE imgid=?2 AND main.history.enabled=1"
      "   AND (main.history.operation NOT IN (SELECT operation FROM data.style_items WHERE styleid=?1))"
      " GROUP BY operation HAVING MAX(num) ORDER BY num DESC", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, styleid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int num = (sqlite3_column_type(stmt, 0) == SQLITE_NULL) ? -1 : sqlite3_column_int(stmt, 0);
    const int selimg_num = (sqlite3_column_type(stmt, 5) == SQLITE_NULL) ? -1 : sqlite3_column_int(stmt, 5);
    cb(user_data, num, sqlite3_column_int(stmt, 1), sqlite3_column_int(stmt, 2),
       (const char *)sqlite3_column_text(stmt, 3), sqlite3_column_int(stmt, 4),
       NULL, 0, NULL, 0,
       sqlite3_column_int(stmt, 8), (const char *)sqlite3_column_text(stmt, 7), selimg_num,
       sqlite3_column_double(stmt, 8));
  }
  sqlite3_finalize(stmt);
}

gboolean dt_style_repository_copy_items_from_history(const int32_t styleid, const int32_t imgid,
                                                     GList *nums)
{
  if(styleid == 0) return FALSE;

  char *predicate = _num_set_predicate(nums, FALSE);
  char *query = NULL;
  sqlite3_stmt *stmt;

  if(predicate)
  {
    // clang-format off
    query = g_strdup_printf(
             "INSERT INTO data.style_items"
             " (styleid,num,module,operation,op_params,enabled,blendop_params,"
             "  blendop_version,multi_priority,multi_name)"
             " SELECT ?1, num,module,operation,op_params,enabled,blendop_params,blendop_version,"
             "  multi_priority,multi_name"
             " FROM main.history"
             " WHERE imgid=?2 AND %s",
             predicate);
    // clang-format on
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), query, -1, &stmt, NULL);
  }
  else
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "INSERT INTO data.style_items"
                                "  (styleid,num,module,operation,op_params,enabled,blendop_params,"
                                "   blendop_version,multi_priority,multi_name)"
                                " SELECT ?1, num,module,operation,op_params,enabled,blendop_params,blendop_version,"
                                "   multi_priority,multi_name"
                                " FROM main.history"
                                " WHERE imgid=?2",
                                -1, &stmt, NULL);
    // clang-format on

  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, styleid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
  const gboolean ok = (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);

  dt_free(query);
  dt_free(predicate);
  return ok;
}

gboolean dt_style_repository_copy_items_from_style(const int32_t dest_styleid,
                                                   const int32_t source_styleid, GList *nums)
{
  if(dest_styleid == 0 || source_styleid == 0) return FALSE;

  char *predicate = _num_set_predicate(nums, FALSE);
  char *query = NULL;
  sqlite3_stmt *stmt;

  if(predicate)
  {
    // clang-format off
    query = g_strdup_printf(
             "INSERT INTO data.style_items "
             "  (styleid,num,module,operation,op_params,enabled,blendop_params,blendop_version,"
             "   multi_priority,multi_name)"
             " SELECT ?1, num,module,operation,op_params,enabled,blendop_params,blendop_version,"
             "   multi_priority,multi_name"
             " FROM data.style_items"
             " WHERE styleid=?2 AND %s",
             predicate);
    // clang-format on
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), query, -1, &stmt, NULL);
  }
  else
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "INSERT INTO data.style_items "
                                "  (styleid,num,module,operation,op_params,enabled,blendop_params,"
                                "   blendop_version,multi_priority,multi_name)"
                                " SELECT ?1, num,module,operation,op_params,enabled,blendop_params,"
                                "        blendop_version,multi_priority,multi_name"
                                " FROM data.style_items"
                                " WHERE styleid=?2",
                                -1, &stmt, NULL);
    // clang-format on

  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dest_styleid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, source_styleid);
  const gboolean ok = (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);

  dt_free(query);
  dt_free(predicate);
  return ok;
}

gboolean dt_style_repository_delete_items_except(const int32_t styleid, GList *nums)
{
  if(styleid == 0) return FALSE;

  char *predicate = _num_set_predicate(nums, TRUE);
  if(IS_NULL_PTR(predicate)) return FALSE;

  char *query = g_strdup_printf("DELETE FROM data.style_items WHERE styleid=?1 AND %s", predicate);
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, styleid);
  const gboolean ok = (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);

  dt_free(query);
  dt_free(predicate);
  return ok;
}

void dt_style_repository_update_item_from_history(const int32_t styleid, const int item_num,
                                                  const int32_t imgid, const int history_num)
{
  if(styleid == 0) return;

  // Every column is copied from one history row, so the SET list is generated rather than typed
  // out twice. The original built the same list the same way.
  static const char *const fields[] = { "op_params",       "module",         "enabled",    "blendop_params",
                                        "blendop_version", "multi_priority", "multi_name", NULL };

  GString *q = g_string_new("UPDATE data.style_items SET ");
  for(int k = 0; fields[k]; k++)
  {
    if(k != 0) g_string_append_c(q, ',');
    g_string_append_printf(q, "%s=(SELECT %s FROM main.history WHERE imgid=%d AND num=%d)", fields[k],
                           fields[k], imgid, history_num);
  }
  g_string_append_printf(q, " WHERE styleid=%d AND data.style_items.num=%d", styleid, item_num);

  char *query = g_string_free(q, FALSE);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get_sqlite3_global(), query, NULL, NULL, NULL);
  dt_free(query);
}

void dt_style_repository_append_item_from_history(const int32_t styleid, const int32_t imgid,
                                                  const int history_num)
{
  if(styleid == 0) return;

  // clang-format off
  char *query = g_strdup_printf(
           "INSERT INTO data.style_items "
           "  (styleid, num, module, operation, op_params, enabled, blendop_params,"
           "   blendop_version, multi_priority, multi_name)"
           " SELECT %d,"
           "    (SELECT num+1 "
           "     FROM data.style_items"
           "     WHERE styleid=%d"
           "     ORDER BY num DESC LIMIT 1), "
           "   module, operation, op_params, enabled, blendop_params, blendop_version,"
           "   multi_priority, multi_name"
           " FROM main.history"
           " WHERE imgid=%d AND num=%d",
           styleid, styleid, imgid, history_num);
  // clang-format on

  DT_DEBUG_SQLITE3_EXEC(dt_database_get_sqlite3_global(), query, NULL, NULL, NULL);
  dt_free(query);
}

gboolean dt_style_repository_insert_item(const int32_t styleid, const int num,
                                         const int module_version, const char *operation,
                                         const void *params, const int32_t params_size,
                                         const int enabled, const void *blendop_params,
                                         const int32_t blendop_params_size,
                                         const int blendop_version, const int multi_priority,
                                         const char *multi_name)
{
  if(styleid == 0) return FALSE;

  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "INSERT INTO data.style_items "
                              " (styleid, num, module, operation, op_params, enabled, blendop_params,"
                              "  blendop_version, multi_priority, multi_name)"
                              " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, styleid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, num);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, module_version);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 5, params, params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 6, enabled);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 7, blendop_params, blendop_params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 8, blendop_version);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 9, multi_priority);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 10, multi_name, -1, SQLITE_TRANSIENT);

  const gboolean ok = (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
  return ok;
}

gboolean dt_style_repository_normalize_multi_priority(const int32_t styleid)
{
  if(styleid == 0) return FALSE;

  sqlite3_stmt *stmt;
  GList *list = NULL;
  struct _data
  {
    int rowid;
    int mi;
  };
  char last_operation[128] = { 0 };
  int last_mi = 0;

  /* let's clean-up the style multi-instance. What we want to do is have a unique multi_priority value for
     each iop.
     Furthermore this value must start to 0 and increment one by one for each multi-instance of the same
     module. On
     SQLite there is no notion of ROW_NUMBER, so we use rather resource consuming SQL statement, but as a
     style has
     never a huge number of items that's not a real issue. */

  /* 1. read all data for the style and record multi_instance value. */

  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get_sqlite3_global(),
      "SELECT rowid,operation FROM data.style_items WHERE styleid=?1 ORDER BY operation, multi_priority ASC", -1,
      &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, styleid);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    struct _data *d = malloc(sizeof(struct _data));
    const char *operation = (const char *)sqlite3_column_text(stmt, 1);

    if(strncmp(last_operation, operation, 128) != 0)
    {
      last_mi = 0;
      g_strlcpy(last_operation, operation, sizeof(last_operation));
    }
    else
      last_mi++;

    d->rowid = sqlite3_column_int(stmt, 0);
    d->mi = last_mi;
    list = g_list_prepend(list, d);
  }
  sqlite3_finalize(stmt);
  list = g_list_reverse(list);   // list was built in reverse order, so un-reverse it

  /* 2. now update all multi_instance values previously recorded */

  gboolean ok = TRUE;
  for(GList *list_iter = list; list_iter; list_iter = g_list_next(list_iter))
  {
    struct _data *d = (struct _data *)list_iter->data;

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "UPDATE data.style_items SET multi_priority=?1 WHERE rowid=?2", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, d->mi);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, d->rowid);
    ok &= (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
  }

  /* 3. free the list we built in step 1 */
  g_list_free_full(list, dt_free_gpointer);

  return ok;
}

void dt_style_repository_cleanup(void)
{
  _styles_stmt_mutex_ensure();
  dt_pthread_mutex_lock(&_styles_stmt_mutex);
  if(_styles_get_list_stmt)
  {
    sqlite3_finalize(_styles_get_list_stmt);
    _styles_get_list_stmt = NULL;
  }
  if(_styles_apply_items_stmt)
  {
    sqlite3_finalize(_styles_apply_items_stmt);
    _styles_apply_items_stmt = NULL;
  }
  dt_pthread_mutex_unlock(&_styles_stmt_mutex);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
