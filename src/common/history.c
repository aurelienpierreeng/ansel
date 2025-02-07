/*
    This file is part of darktable,
    Copyright (C) 2010-2022 darktable developers.

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

#include "common/history.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/exif.h"
#include "common/history_snapshot.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/mipmap_cache.h"
#include "common/tags.h"
#include "common/undo.h"
#include "common/utility.h"
#include "control/control.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/masks.h"
#include "gui/hist_dialog.h"

#define DT_IOP_ORDER_INFO (darktable.unmuted & DT_DEBUG_IOPORDER)

void dt_history_item_free(gpointer data)
{
  dt_history_item_t *item = (dt_history_item_t *)data;
  g_free(item->op);
  g_free(item->name);
  item->op = NULL;
  item->name = NULL;
  g_free(item);
}

static void _remove_preset_flag(const int imgid)
{
  dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'w');

  // clear flag
  image->flags &= ~DT_IMAGE_AUTO_PRESETS_APPLIED;

  // write through to sql+xmp
  dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_SAFE);
}

void dt_history_delete_on_image_ext(int32_t imgid, gboolean undo)
{
  dt_undo_lt_history_t *hist = undo?dt_history_snapshot_item_init():NULL;

  if(undo)
  {
    hist->imgid = imgid;
    dt_history_snapshot_undo_create(hist->imgid, &hist->before, &hist->before_history_end);
  }

  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM main.history WHERE imgid = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM main.module_order WHERE imgid = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE main.images"
                              " SET history_end = 0, aspect_ratio = 0.0"
                              " WHERE id = ?1",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM main.masks_history WHERE imgid = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM main.history_hash WHERE imgid = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  _remove_preset_flag(imgid);

  /* make sure mipmaps are recomputed */
  dt_mipmap_cache_remove(darktable.mipmap_cache, imgid);

  /* remove darktable|style|* tags */
  dt_tag_detach_by_string("darktable|style|%", imgid, FALSE, FALSE);
  dt_tag_detach_by_string("darktable|changed", imgid, FALSE, FALSE);

  /* unset change timestamp */
  dt_image_cache_unset_change_timestamp(darktable.image_cache, imgid);

  // signal that the mipmap need to be updated
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_MIPMAP_UPDATED, imgid);

  // update history hash
  dt_history_hash_write_from_history(imgid, DT_HISTORY_HASH_CURRENT);

  if(undo)
  {
    dt_history_snapshot_undo_create(hist->imgid, &hist->after, &hist->after_history_end);

    dt_undo_start_group(darktable.undo, DT_UNDO_LT_HISTORY);
    dt_undo_record(darktable.undo, NULL, DT_UNDO_LT_HISTORY, (dt_undo_data_t)hist,
                   dt_history_snapshot_undo_pop, dt_history_snapshot_undo_lt_history_data_free);
    dt_undo_end_group(darktable.undo);
  }
}

void dt_history_delete_on_image(int32_t imgid)
{
  dt_history_delete_on_image_ext(imgid, TRUE);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_TAG_CHANGED);
}

int dt_history_load_and_apply(const int imgid, gchar *filename, int history_only)
{
  dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'w');
  if(img)
  {
    dt_undo_lt_history_t *hist = dt_history_snapshot_item_init();
    hist->imgid = imgid;
    dt_history_snapshot_undo_create(hist->imgid, &hist->before, &hist->before_history_end);

    if(dt_exif_xmp_read(img, filename, history_only))
    {
      dt_image_cache_write_release(darktable.image_cache, img,
                                   // ugly but if not history_only => called from crawler - do not write the xmp
                                   history_only ? DT_IMAGE_CACHE_SAFE : DT_IMAGE_CACHE_RELAXED);
      return 1;
    }
    dt_history_snapshot_undo_create(hist->imgid, &hist->after, &hist->after_history_end);
    dt_undo_start_group(darktable.undo, DT_UNDO_LT_HISTORY);
    dt_undo_record(darktable.undo, NULL, DT_UNDO_LT_HISTORY, (dt_undo_data_t)hist,
                   dt_history_snapshot_undo_pop, dt_history_snapshot_undo_lt_history_data_free);
    dt_undo_end_group(darktable.undo);

    dt_image_cache_write_release(darktable.image_cache, img,
    // ugly but if not history_only => called from crawler - do not write the xmp
                                 history_only ? DT_IMAGE_CACHE_SAFE : DT_IMAGE_CACHE_RELAXED);
    dt_mipmap_cache_remove(darktable.mipmap_cache, imgid);
  }
  // signal that the mipmap need to be updated
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_MIPMAP_UPDATED, imgid);
  return 0;
}

int dt_history_load_and_apply_on_list(gchar *filename, const GList *list)
{
  int res = 0;
  dt_undo_start_group(darktable.undo, DT_UNDO_LT_HISTORY);
  for(GList *l = (GList *)list; l; l = g_list_next(l))
  {
    const int imgid = GPOINTER_TO_INT(l->data);
    if(dt_history_load_and_apply(imgid, filename, 1)) res = 1;
  }
  dt_undo_end_group(darktable.undo);
  return res;
}

char *dt_history_item_as_string(const char *name, gboolean enabled)
{
  return g_strconcat(enabled ? "\342\227\217" : "\342\227\213", "  ", name, NULL);
}

GList *dt_history_get_items(const int32_t imgid, gboolean enabled)
{
  GList *result = NULL;
  sqlite3_stmt *stmt;

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
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
  {
    if(strcmp((const char*)sqlite3_column_text(stmt, 1), "mask_manager") == 0) continue;

    char name[512] = { 0 };
    dt_history_item_t *item = g_malloc(sizeof(dt_history_item_t));
    const char *op = (char *)sqlite3_column_text(stmt, 1);
    item->num = sqlite3_column_int(stmt, 0);
    item->enabled = sqlite3_column_int(stmt, 2);

    char *mname = g_strdup((gchar *)sqlite3_column_text(stmt, 3));

    if(strcmp(mname, "0") == 0)
      g_snprintf(name, sizeof(name), "%s", dt_iop_get_localized_name(op));
    else
      g_snprintf(name, sizeof(name), "%s %s",
                 dt_iop_get_localized_name(op),
                 (char *)sqlite3_column_text(stmt, 3));
    item->name = g_strdup(name);
    item->op = g_strdup(op);
    result = g_list_prepend(result, item);

    g_free(mname);
  }
  sqlite3_finalize(stmt);
  return g_list_reverse(result);   // list was built in reverse order, so un-reverse it
}

char *dt_history_get_items_as_string(const int32_t imgid)
{
  GList *items = NULL;
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT operation, enabled, multi_name"
      " FROM main.history"
      " WHERE imgid=?1 ORDER BY num DESC", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  // collect all the entries in the history from the db
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    char *multi_name = NULL;
    const char *mn = (char *)sqlite3_column_text(stmt, 2);

    if(mn && *mn && g_strcmp0(mn, " ") != 0 && g_strcmp0(mn, "0") != 0)
      multi_name = g_strconcat(" ", sqlite3_column_text(stmt, 2), NULL);

    char *iname = dt_history_item_as_string
      (dt_iop_get_localized_name((char *)sqlite3_column_text(stmt, 0)),
       sqlite3_column_int(stmt, 1));

    char *name = g_strconcat(iname, multi_name ? multi_name : "", NULL);
    items = g_list_prepend(items, name);

    g_free(iname);
    g_free(multi_name);
  }
  sqlite3_finalize(stmt);
  items = g_list_reverse(items); // list was built in reverse order, so un-reverse it
  char *result = dt_util_glist_to_str("\n", items);
  g_list_free_full(items, g_free);
  return result;
}

static int dt_history_end_attop(const int32_t imgid)
{
  int size=0;
  int end=0;
  sqlite3_stmt *stmt;

  // get highest num in history
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
    "SELECT MAX(num) FROM main.history WHERE imgid=?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  if (sqlite3_step(stmt) == SQLITE_ROW)
    size = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  // get history_end
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
    "SELECT history_end FROM main.images WHERE id=?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if (sqlite3_step(stmt) == SQLITE_ROW)
    end = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  // fprintf(stderr,"\ndt_history_end_attop for image %i: size %i, end %i",imgid,size,end);

  // a special case right after removing all history
  // It must be absolutely fresh and untouched so history_end is always on top
  if ((size==0) && (end==0)) return -1;

  // return 1 if end is larger than size
  if (end > size) return 1;

  // no compression as history_end is right in the middle of stack
  return 0;
}


/* Please note: dt_history_compress_on_image
  - is used in lighttable and darkroom mode
  - It compresses history *exclusively* in the database and does *not* touch anything on the history stack
*/
void dt_history_compress_on_image(const int32_t imgid)
{
  sqlite3_stmt *stmt;

  dt_print(DT_DEBUG_HISTORY, "[dt_history_compress_on_image] compressing history for image %i\n", imgid);

  // get history_end for image
  int my_history_end = 0;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
    "SELECT history_end FROM main.images WHERE id=?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  if (sqlite3_step(stmt) == SQLITE_ROW)
    my_history_end = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  if(my_history_end == 0)
  {
    dt_history_delete_on_image(imgid);
    return;
  }

  int masks_count = 0;
  const char *op_mask_manager = "mask_manager";
  gboolean manager_position = FALSE;

  dt_database_start_transaction(darktable.db);

  // We must know for sure whether there is a mask manager at slot 0 in history
  // because only if this is **not** true history nums and history_end must be increased
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
    "SELECT COUNT(*) FROM main.history WHERE imgid = ?1 AND operation = ?2 AND num = 0", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, op_mask_manager, -1, SQLITE_TRANSIENT);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    if (sqlite3_column_int(stmt, 0) == 1) manager_position = TRUE;
  }
  sqlite3_finalize(stmt);

  // compress history, keep disabled modules as documented
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM main.history"
                              " WHERE imgid = ?1 AND num NOT IN"
                              "   (SELECT MAX(num) FROM main.history"
                              "     WHERE imgid = ?1 AND num < ?2"
                              "     GROUP BY operation, multi_priority)",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, my_history_end);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // delete all mask_manager entries
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
    "DELETE FROM main.history WHERE imgid = ?1 AND operation = ?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, op_mask_manager, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // compress masks history
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM main.masks_history"
                              " WHERE imgid = ?1 "
                              "   AND num NOT IN (SELECT MAX(num)"
                              "                   FROM main.masks_history"
                              "                   WHERE imgid = ?1 AND num < ?2)", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, my_history_end);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // if there are masks create a mask manager entry, so we need to count them
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
    "SELECT COUNT(*) FROM main.masks_history WHERE imgid = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW) masks_count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  if(masks_count > 0)
  {
    // Set num in masks history to make sure they are owned by the manager at slot 0.
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
      "UPDATE main.masks_history SET num = 0 WHERE imgid = ?1", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (!manager_position)
    {
      // make room for mask manager history entry
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
        "UPDATE main.history SET num=num+1 WHERE imgid = ?1", -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);

      // update history end
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
        "UPDATE main.images SET history_end = history_end+1 WHERE id = ?1", -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
    }

    // create a mask manager entry in history as first entry
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "INSERT INTO main.history (imgid, num, operation, op_params, module, enabled, "
                                "                          blendop_params, blendop_version, multi_priority, multi_name) "
                                " VALUES(?1, 0, ?2, NULL, 1, 0, NULL, 0, 0, '')",
                                -1, &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, op_mask_manager, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  dt_history_hash_write_from_history(imgid, DT_HISTORY_HASH_CURRENT);

  dt_database_release_transaction(darktable.db);

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_MIPMAP_UPDATED, imgid);
}

/* Please note: dt_history_truncate_on_image
  - can be used in lighttable and darkroom mode
  - It truncates history *exclusively* in the database and does *not* touch anything on the history stack
*/
void dt_history_truncate_on_image(const int32_t imgid, const int32_t history_end)
{
  sqlite3_stmt *stmt;

  if (history_end == 0)
  {
    dt_history_delete_on_image(imgid);
    return;
  }

  dt_database_start_transaction(darktable.db);

  // delete end of history
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM main.history"
                              " WHERE imgid = ?1 "
                              "   AND num >= ?2", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, history_end);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // delete end of masks history
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM main.masks_history"
                              " WHERE imgid = ?1 "
                              "   AND num >= ?2", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, history_end);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // update history end
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE main.images"
                              " SET history_end = ?1"
                              " WHERE id = ?2 ", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, history_end);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  dt_history_hash_write_from_history(imgid, DT_HISTORY_HASH_CURRENT);

  dt_database_release_transaction(darktable.db);

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_MIPMAP_UPDATED, imgid);
}

int dt_history_compress_on_list(const GList *imgs)
{
  int uncompressed=0;

  // Get the list of selected images
  for(const GList *l = imgs; l; l = g_list_next(l))
  {
    const int imgid = GPOINTER_TO_INT(l->data);
    const int test = dt_history_end_attop(imgid);
    if(test == 1) // we do a compression and we know for sure history_end is at the top!
    {
      dt_history_compress_on_image(imgid);

      // now the modules are in right order but need renumbering to remove leaks
      int max=0;    // the maximum num in main_history for an image
      int size=0;   // the number of items in main_history for an image
      int done=0;   // used for renumbering index

      sqlite3_stmt *stmt2;

      // get highest num in history
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
        "SELECT MAX(num) FROM main.history WHERE imgid=?1", -1, &stmt2, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt2, 1, imgid);
      if (sqlite3_step(stmt2) == SQLITE_ROW)
        max = sqlite3_column_int(stmt2, 0);
      sqlite3_finalize(stmt2);

      // get number of items in main.history
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
        "SELECT COUNT(*) FROM main.history WHERE imgid = ?1", -1, &stmt2, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt2, 1, imgid);
      if(sqlite3_step(stmt2) == SQLITE_ROW)
        size = sqlite3_column_int(stmt2, 0);
      sqlite3_finalize(stmt2);

      if ((size>0) && (max>0))
      {
        for (int index=0;index<(max+1);index++)
        {
          sqlite3_stmt *stmt3;
          DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
            "SELECT num FROM main.history WHERE imgid=?1 AND num=?2", -1, &stmt3, NULL);
          DT_DEBUG_SQLITE3_BIND_INT(stmt3, 1, imgid);
          DT_DEBUG_SQLITE3_BIND_INT(stmt3, 2, index);
          if (sqlite3_step(stmt3) == SQLITE_ROW)
          {
            sqlite3_stmt *stmt4;
            // step by step set the correct num
            DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
              "UPDATE main.history SET num = ?3 WHERE imgid = ?1 AND num = ?2", -1, &stmt4, NULL);
            DT_DEBUG_SQLITE3_BIND_INT(stmt4, 1, imgid);
            DT_DEBUG_SQLITE3_BIND_INT(stmt4, 2, index);
            DT_DEBUG_SQLITE3_BIND_INT(stmt4, 3, done);
            sqlite3_step(stmt4);
            sqlite3_finalize(stmt4);

            done++;
          }
          sqlite3_finalize(stmt3);
        }
      }
      // update history end
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
        "UPDATE main.images SET history_end = ?2 WHERE id = ?1", -1, &stmt2, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt2, 1, imgid);
      DT_DEBUG_SQLITE3_BIND_INT(stmt2, 2, done);
      sqlite3_step(stmt2);
      sqlite3_finalize(stmt2);

      dt_control_save_xmp(imgid);
    }
    if(test == 0) // no compression as history_end is right in the middle of history
      uncompressed++;

    dt_history_hash_write_from_history(imgid, DT_HISTORY_HASH_CURRENT);
  }

  return uncompressed;
}

gboolean dt_history_check_module_exists(int32_t imgid, const char *operation, gboolean enabled)
{
  gboolean result = FALSE;
  sqlite3_stmt *stmt;

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
    dt_database_get(darktable.db),
    "SELECT imgid"
    " FROM main.history"
    " WHERE imgid= ?1 AND operation = ?2 AND enabled in (1, ?3)",
    -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, enabled);
  if (sqlite3_step(stmt) == SQLITE_ROW) result = TRUE;
  sqlite3_finalize(stmt);

  return result;
}

gboolean dt_history_check_module_exists_list(GList *hist, const char *operation, gboolean enabled)
{
  for(GList *h = g_list_first(hist); h; h = g_list_next(h))
  {
    const dt_history_item_t *item = (dt_history_item_t *)(h->data);

    if(!g_strcmp0(item->op, operation) && (item->enabled || !enabled))
      return TRUE;
  }
  return FALSE;
}

#if 0
// for debug
static gchar *_hash_history_to_string(guint8 *hash, const gsize checksum_len)
{
  char *hash_text = NULL;
  guint8 *p = hash;
  for(int i=0; i<checksum_len; i++)
  {
    uint8_t byte = p[0];
    hash_text = dt_util_dstrcat(hash_text, "%02x", byte);
    p++;
  }
  return hash_text;
}
#endif

// if the image has no history return 0
static gsize _history_hash_compute_from_db(const int32_t imgid, guint8 **hash)
{
  if(imgid == -1) return 0;

  GChecksum *checksum = g_checksum_new(G_CHECKSUM_MD5);
  gsize hash_len = 0;

  sqlite3_stmt *stmt;

  // get history end
  int history_end = 0;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT history_end FROM main.images WHERE id = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    if(sqlite3_column_type(stmt, 0) != SQLITE_NULL)
      history_end = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);

  // get history. the active history for an image are all the latest operations (MAX(num))
  // which are enabled. this is important here as we want the hash to represent the actual
  // developement of the image.
  gboolean history_on = FALSE;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT operation, op_params, blendop_params, enabled, MAX(num)"
                              " FROM main.history"
                              " WHERE imgid = ?1 AND num <= ?2"
                              " GROUP BY operation, multi_priority"
                              " ORDER BY num",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, history_end);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int enabled = sqlite3_column_int(stmt, 3);
    if(enabled)
    {
      // operation
      char *buf = (char *)sqlite3_column_text(stmt, 0);
      if(buf) g_checksum_update(checksum, (const guchar *)buf, -1);
      // op_params
      buf = (char *)sqlite3_column_blob(stmt, 1);
      int params_len = sqlite3_column_bytes(stmt, 1);
      if(buf) g_checksum_update(checksum, (const guchar *)buf, params_len);
      // blendop_params
      buf = (char *)sqlite3_column_blob(stmt, 2);
      params_len = sqlite3_column_bytes(stmt, 2);
      if(buf) g_checksum_update(checksum, (const guchar *)buf, params_len);
      history_on = TRUE;
    }
  }
  sqlite3_finalize(stmt);

  if(history_on)
  {
    // get module order
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT version, iop_list"
                                " FROM main.module_order"
                                " WHERE imgid = ?1",
                                -1, &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      const int version = sqlite3_column_int(stmt, 0);
      g_checksum_update(checksum, (const guchar *)&version, sizeof(version));
      if(version == DT_IOP_ORDER_CUSTOM)
      {
        // iop_list
        const char *buf = (char *)sqlite3_column_text(stmt, 1);
        if(buf) g_checksum_update(checksum, (const guchar *)buf, -1);
      }
    }
    sqlite3_finalize(stmt);

    const gsize checksum_len = g_checksum_type_get_length(G_CHECKSUM_MD5);
    *hash = g_malloc(checksum_len);
    hash_len = checksum_len;
    g_checksum_get_digest(checksum, *hash, &hash_len);
  }
  g_checksum_free(checksum);

  return hash_len;
}

void dt_history_hash_write_from_history(const int32_t imgid, const dt_history_hash_t type)
{
  if(imgid == -1) return;

  guint8 *hash = NULL;
  gsize hash_len = _history_hash_compute_from_db(imgid, &hash);
  if(hash_len)
  {
    char *fields = NULL;
    char *values = NULL;
    char *conflict = NULL;
    if(type & DT_HISTORY_HASH_BASIC)
    {
      fields = g_strdup_printf("%s,", "basic_hash");
      values = g_strdup("?2,");
      conflict = g_strdup("basic_hash=?2,");
    }
    if(type & DT_HISTORY_HASH_AUTO)
    {
      fields = dt_util_dstrcat(fields, "%s,", "auto_hash");
      values = dt_util_dstrcat(values, "?2,");
      conflict = dt_util_dstrcat(conflict, "auto_hash=?2,");
    }
    if(type & DT_HISTORY_HASH_CURRENT)
    {
      fields = dt_util_dstrcat(fields, "%s,", "current_hash");
      values = dt_util_dstrcat(values, "?2,");
      conflict = dt_util_dstrcat(conflict, "current_hash=?2,");
    }
    // remove the useless last comma
    if(fields) fields[strlen(fields) - 1] = '\0';
    if(values) values[strlen(values) - 1] = '\0';
    if(conflict) conflict[strlen(conflict) - 1] = '\0';

    if(fields)
    {
      sqlite3_stmt *stmt;
#ifdef HAVE_SQLITE_324_OR_NEWER
      // clang-format off
      char *query = g_strdup_printf("INSERT INTO main.history_hash"
                                    " (imgid, %s) VALUES (?1, %s)"
                                    " ON CONFLICT (imgid)"
                                    " DO UPDATE SET %s",
                                    fields, values, conflict);
      // clang-format on
#else
      char *query = NULL;
      // clang-format off
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "SELECT imgid FROM main.history_hash"
                                  " WHERE imgid = ?1",
                                   -1, &stmt, NULL);
      // clang-format on
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
      if(sqlite3_step(stmt) == SQLITE_ROW)
      {
        sqlite3_finalize(stmt);
        // clang-format off
        query = g_strdup_printf("UPDATE main.history_hash"
                                " SET %s"
                                " WHERE imgid = ?1",
                                conflict);
        // clang-format on
      }
      else
      {
        sqlite3_finalize(stmt);
        // clang-format off
        query = g_strdup_printf("INSERT INTO main.history_hash"
                                " (imgid, %s) VALUES (?1, %s)",
                                fields, values);
        // clang-format on
      }
#endif
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
      DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 2, hash, hash_len, SQLITE_TRANSIENT);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
      g_free(query);
      g_free(fields);
      g_free(values);
      g_free(conflict);
    }
    g_free(hash);
  }
}

void dt_history_hash_write(const int32_t imgid, dt_history_hash_values_t *hash)
{
  if(hash->basic || hash->auto_apply || hash->current)
  {
    sqlite3_stmt *stmt;
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "INSERT OR REPLACE INTO main.history_hash"
                                " (imgid, basic_hash, auto_hash, current_hash)"
                                " VALUES (?1, ?2, ?3, ?4)",
                                -1, &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 2, hash->basic, hash->basic_len, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 3, hash->auto_apply, hash->auto_apply_len, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 4, hash->current, hash->current_len, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    g_free(hash->basic);
    g_free(hash->auto_apply);
    g_free(hash->current);
  }
}

void dt_history_hash_read(const int32_t imgid, dt_history_hash_values_t *hash)
{
  hash->basic = hash->auto_apply = hash->current = NULL;
  hash->basic_len = hash->auto_apply_len = hash->current_len = 0;
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT basic_hash, auto_hash, current_hash"
                              " FROM main.history_hash"
                              " WHERE imgid = ?1",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    void *buf = (void *)sqlite3_column_blob(stmt, 0);
    hash->basic_len = sqlite3_column_bytes(stmt, 0);
    if(buf)
    {
      hash->basic = malloc(hash->basic_len);
      memcpy(hash->basic, buf, hash->basic_len);
    }
    buf = (void *)sqlite3_column_blob(stmt, 1);
    hash->auto_apply_len = sqlite3_column_bytes(stmt, 1);
    if(buf)
    {
      hash->auto_apply = malloc(hash->auto_apply_len);
      memcpy(hash->auto_apply, buf, hash->auto_apply_len);
    }
    buf = (void *)sqlite3_column_blob(stmt, 2);
    hash->current_len = sqlite3_column_bytes(stmt, 2);
    if(buf)
    {
      hash->current = malloc(hash->current_len);
      memcpy(hash->current, buf, hash->current_len);
    }
  }
  sqlite3_finalize(stmt);
}

gboolean dt_history_hash_is_mipmap_synced(const int32_t imgid)
{
  gboolean status = FALSE;
  if(imgid == -1) return status;
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT CASE"
                              "  WHEN mipmap_hash == current_hash THEN 1"
                              "  ELSE 0 END AS status"
                              " FROM main.history_hash"
                              " WHERE imgid = ?1",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    status = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);
  return status;
}

void dt_history_hash_set_mipmap(const int32_t imgid)
{
  if(imgid == -1) return;
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE main.history_hash"
                              " SET mipmap_hash = current_hash"
                              " WHERE imgid = ?1",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

dt_history_hash_t dt_history_hash_get_status(const int32_t imgid)
{
  dt_history_hash_t status = 0;
  if(imgid == -1) return status;
  sqlite3_stmt *stmt;
  // clang-format off
  char *query = g_strdup_printf("SELECT CASE"
                                "  WHEN basic_hash == current_hash THEN %d"
                                "  WHEN auto_hash == current_hash THEN %d"
                                "  WHEN (basic_hash IS NULL OR current_hash != basic_hash) AND"
                                "       (auto_hash IS NULL OR current_hash != auto_hash) THEN %d"
                                "  ELSE %d END AS status"
                                " FROM main.history_hash"
                                " WHERE imgid = %d",
                                DT_HISTORY_HASH_BASIC, DT_HISTORY_HASH_AUTO,
                                DT_HISTORY_HASH_CURRENT, DT_HISTORY_HASH_BASIC, imgid);
  // clang-format on
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              query, -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    status = sqlite3_column_int(stmt, 0);
  }
  // if no history_hash basic status
  else status = DT_HISTORY_HASH_BASIC;
  sqlite3_finalize(stmt);
  g_free(query);
  return status;
}

gboolean dt_history_copy(int imgid)
{
  // note that this routine does not copy anything, it just setup the copy_paste proxy
  // with the needed information that will be used while pasting.

  if(imgid <= 0) return FALSE;

  darktable.view_manager->copy_paste.copied_imageid = imgid;
  darktable.view_manager->copy_paste.full_copy = TRUE;

  return TRUE;
}

gboolean dt_history_copy_parts(int imgid)
{
  if(dt_history_copy(imgid))
  {
    // we want to copy all history and let user select the parts needed
    darktable.view_manager->copy_paste.full_copy = FALSE;

    // run dialog, it will insert into selops the selected moduel

    if(dt_gui_hist_dialog_new(&(darktable.view_manager->copy_paste), imgid, TRUE) == GTK_RESPONSE_CANCEL)
      return FALSE;
    return TRUE;
  }
  else
    return FALSE;
}

gboolean dt_history_paste_on_list(const GList *list, gboolean undo)
{
  if(darktable.view_manager->copy_paste.copied_imageid <= 0) return FALSE;
  if(!list) // do we have any images to receive the pasted history?
    return FALSE;

  if(undo) dt_undo_start_group(darktable.undo, DT_UNDO_LT_HISTORY);
  for(GList *l = g_list_first((GList *)list); l; l = g_list_next(l))
  {
    const int dest = GPOINTER_TO_INT(l->data);
    dt_history_copy_and_paste_on_image(darktable.view_manager->copy_paste.copied_imageid,
                                       dest,
                                       darktable.view_manager->copy_paste.selops,
                                       darktable.view_manager->copy_paste.copy_iop_order,
                                       darktable.view_manager->copy_paste.full_copy);
  }

  if(undo) dt_undo_end_group(darktable.undo);

  return TRUE;
}

gboolean dt_history_paste_parts_on_list(const GList *list, gboolean undo)
{
  if(darktable.view_manager->copy_paste.copied_imageid <= 0) return FALSE;
  if(!list) // do we have any images to receive the pasted history?
    return FALSE;

  // at the time the dialog is started, some signals are sent and this in turn call
  // back dt_view_get_images_to_act_on() which free list and create a new one.

  // we launch the dialog
  const int res = dt_gui_hist_dialog_new(&(darktable.view_manager->copy_paste),
                                         darktable.view_manager->copy_paste.copied_imageid, FALSE);

  if(res != GTK_RESPONSE_OK)
  {
    return FALSE;
  }

  if(undo) dt_undo_start_group(darktable.undo, DT_UNDO_LT_HISTORY);
  for (const GList *l = g_list_first((GList *)list); l; l = g_list_next(l))
  {
    const int dest = GPOINTER_TO_INT(l->data);
    dt_history_copy_and_paste_on_image(darktable.view_manager->copy_paste.copied_imageid,
                                       dest,
                                       darktable.view_manager->copy_paste.selops,
                                       darktable.view_manager->copy_paste.copy_iop_order,
                                       darktable.view_manager->copy_paste.full_copy);
  }
  if(undo) dt_undo_end_group(darktable.undo);

  return TRUE;
}

gboolean dt_history_delete_on_list(const GList *list, gboolean undo)
{
  if(!list)  // do we have any images on which to operate?
    return FALSE;

  if(undo) dt_undo_start_group(darktable.undo, DT_UNDO_LT_HISTORY);

  for(GList *l = g_list_first((GList *)list); l; l = g_list_next(l))
  {
    const int imgid = GPOINTER_TO_INT(l->data);
    dt_undo_lt_history_t *hist = dt_history_snapshot_item_init();

    hist->imgid = imgid;
    dt_history_snapshot_undo_create(hist->imgid, &hist->before, &hist->before_history_end);

    dt_history_delete_on_image_ext(imgid, FALSE);

    dt_history_snapshot_undo_create(hist->imgid, &hist->after, &hist->after_history_end);
    dt_undo_record(darktable.undo, NULL, DT_UNDO_LT_HISTORY, (dt_undo_data_t)hist, dt_history_snapshot_undo_pop,
                   dt_history_snapshot_undo_lt_history_data_free);

    /* update the aspect ratio if the current sorting is based on aspect ratio, otherwise the aspect ratio will be
       recalculated when the mimpap will be recreated */
    if(darktable.collection->params.sort == DT_COLLECTION_SORT_ASPECT_RATIO)
      dt_image_set_aspect_ratio(imgid, FALSE);
  }

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_TAG_CHANGED);

  if(undo) dt_undo_end_group(darktable.undo);
  return TRUE;
}

#undef DT_IOP_ORDER_INFO
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
