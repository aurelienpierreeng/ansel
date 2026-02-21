/*
    This file is part of darktable,
    Copyright (C) 2010-2011 Henrik Andersson.
    Copyright (C) 2010-2012, 2014 johannes hanika.
    Copyright (C) 2010-2016, 2019 Tobias Ellinghaus.
    Copyright (C) 2012-2014, 2019-2022 Aldric Renaudin.
    Copyright (C) 2012 Frédéric Grollier.
    Copyright (C) 2012-2015, 2018-2022 Pascal Obry.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012 Ulrich Pegelow.
    Copyright (C) 2013 José Carlos García Sogo.
    Copyright (C) 2013 Simon Spannagel.
    Copyright (C) 2014-2016 Roman Lebedev.
    Copyright (C) 2015 Jan Kundrát.
    Copyright (C) 2018-2019 Edgardo Hoszowski.
    Copyright (C) 2019 Alexander Blinne.
    Copyright (C) 2019, 2022 Hanno Schwalm.
    Copyright (C) 2019-2020 Heiko Bauke.
    Copyright (C) 2019-2020 Philippe Weyland.
    Copyright (C) 2020 Chris Elston.
    Copyright (C) 2020-2021 Hubert Kowalski.
    Copyright (C) 2020 JP Verrue.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2022-2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2023 Luca Zulberti.
    
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
#include "common/dtpthread.h"
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
#include "develop/dev_history.h"
#include "develop/develop.h"
#include "develop/masks.h"
#include "gui/hist_dialog.h"

#define DT_IOP_ORDER_INFO (darktable.unmuted & DT_DEBUG_IOPORDER)

static sqlite3_stmt *_history_check_module_exists_stmt = NULL;
static sqlite3_stmt *_history_hash_set_mipmap_stmt = NULL;
static sqlite3_stmt *_history_get_end_stmt = NULL;
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

void dt_history_item_free(gpointer data)
{
  dt_history_item_t *item = (dt_history_item_t *)data;
  g_free(item->op);
  g_free(item->name);
  item->op = NULL;
  item->name = NULL;
  g_free(item);
}

static void _remove_preset_flag(const int32_t imgid)
{
  dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'w');

  // clear flag
  image->flags &= ~DT_IMAGE_AUTO_PRESETS_APPLIED;

  // write through to sql+xmp
  dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_SAFE);
}

void dt_history_delete_on_image_ext(int32_t imgid, gboolean undo)
{
  dt_undo_lt_history_t *hist = undo ? dt_history_snapshot_item_init() : NULL;

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
  dt_mipmap_cache_remove(darktable.mipmap_cache, imgid, TRUE);

  /* remove darktable|style|* tags */
  dt_tag_detach_by_string("darktable|style|%", imgid, FALSE, FALSE);
  dt_tag_detach_by_string("darktable|changed", imgid, FALSE, FALSE);

  // signal that the mipmap need to be updated
  dt_thumbtable_refresh_thumbnail(darktable.gui->ui->thumbtable_lighttable, imgid, TRUE);

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

int dt_history_load_and_apply(const int32_t imgid, gchar *filename, int history_only)
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
    dt_mipmap_cache_remove(darktable.mipmap_cache, imgid, TRUE);
  }

  // signal that the mipmap need to be updated
  dt_thumbtable_refresh_thumbnail(darktable.gui->ui->thumbtable_lighttable, imgid, TRUE);
  return 0;
}

int dt_history_load_and_apply_on_list(gchar *filename, const GList *list)
{
  int res = 0;
  dt_undo_start_group(darktable.undo, DT_UNDO_LT_HISTORY);
  for(GList *l = (GList *)list; l; l = g_list_next(l))
  {
    const int32_t imgid = GPOINTER_TO_INT(l->data);
    if(dt_history_load_and_apply(imgid, filename, 1)) res = 1;
  }
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_IMAGE_INFO_CHANGED, g_list_copy((GList *)list));
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
    char *clean_name = delete_underscore(name);
    items = g_list_prepend(items, clean_name);

    g_free(iname);
    g_free(name);
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

  end = dt_history_get_end(imgid);

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
  - It compresses history through dt_dev_history_compress() (develop/dev_history.c)
*/
void dt_history_compress_on_image(const int32_t imgid)
{
  dt_print(DT_DEBUG_HISTORY, "[dt_history_compress_on_image] compressing history for image %i\n", imgid);
  if(imgid <= 0) return;

  const int my_history_end = dt_history_get_end(imgid);

  if(my_history_end <= 0)
  {
    dt_history_delete_on_image(imgid);
    return;
  }

  dt_develop_t dev;
  dt_dev_init(&dev, FALSE);
  dt_dev_read_history_ext(&dev, imgid, TRUE);
  dt_dev_set_history_end(&dev, my_history_end);
  dt_dev_pop_history_items_ext(&dev);
  dt_dev_history_compress(&dev);
  dt_dev_history_notify_change(&dev, imgid);
  dt_dev_cleanup(&dev);
}

/* Please note: dt_history_truncate_on_image
  - can be used in lighttable and darkroom mode
  - It truncates history through develop/dev_history.c and rewrites DB/XMP.
*/
void dt_history_truncate_on_image(const int32_t imgid, const int32_t history_end)
{
  if(imgid <= 0) return;

  if(history_end <= 0)
  {
    dt_history_delete_on_image(imgid);
    return;
  }

  dt_develop_t dev;
  dt_dev_init(&dev, FALSE);
  dt_pthread_rwlock_wrlock(&dev.history_mutex);
  if(!dt_dev_read_history_ext(&dev, imgid, TRUE))
  {
    dt_pthread_rwlock_unlock(&dev.history_mutex);
    dt_dev_cleanup(&dev);
    return;
  }

  // Clamp to history size.
  dt_dev_set_history_end(&dev, history_end);
  const int32_t end = dt_dev_get_history_end(&dev);

  // Remove tail entries (num >= end).
  GList *link = g_list_nth(dev.history, end);
  while(link)
  {
    GList *next = g_list_next(link);
    dt_dev_free_history_item(link->data);
    dev.history = g_list_delete_link(dev.history, link);
    link = next;
  }

  dt_dev_write_history_ext(&dev, imgid);
  dt_pthread_rwlock_unlock(&dev.history_mutex);
  dt_dev_history_notify_change(&dev, imgid);
  dt_dev_cleanup(&dev);
}

int dt_history_compress_on_list(const GList *imgs)
{
  int uncompressed=0;

  // Get the list of selected images
  for(const GList *l = imgs; l; l = g_list_next(l))
  {
    const int32_t imgid = GPOINTER_TO_INT(l->data);
    const int test = dt_history_end_attop(imgid);
    if(test == 1) // we do a compression and we know for sure history_end is at the top!
    {
      dt_history_compress_on_image(imgid);
    }
    if(test == 0) // no compression as history_end is right in the middle of history
      uncompressed++;
  }

  return uncompressed;
}

gboolean dt_history_check_module_exists(int32_t imgid, const char *operation, gboolean enabled)
{
  gboolean result = FALSE;
  _history_stmt_mutex_ensure();
  dt_pthread_mutex_lock(&_history_stmt_mutex);
  if(!_history_check_module_exists_stmt)
  {
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT imgid"
      " FROM main.history"
      " WHERE imgid= ?1 AND operation = ?2 AND enabled in (1, ?3)",
      -1, &_history_check_module_exists_stmt, NULL);
    // clang-format on
  }
  sqlite3_stmt *stmt = _history_check_module_exists_stmt;
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, enabled);
  if (sqlite3_step(stmt) == SQLITE_ROW) result = TRUE;
  dt_pthread_mutex_unlock(&_history_stmt_mutex);

  return result;
}

void dt_history_cleanup(void)
{
  _history_stmt_mutex_ensure();
  dt_pthread_mutex_lock(&_history_stmt_mutex);
  if(_history_hash_set_mipmap_stmt)
  {
    sqlite3_finalize(_history_hash_set_mipmap_stmt);
    _history_hash_set_mipmap_stmt = NULL;
  }
  if(_history_check_module_exists_stmt)
  {
    sqlite3_finalize(_history_check_module_exists_stmt);
    _history_check_module_exists_stmt = NULL;
  }
  if(_history_get_end_stmt)
  {
    sqlite3_finalize(_history_get_end_stmt);
    _history_get_end_stmt = NULL;
  }
  dt_pthread_mutex_unlock(&_history_stmt_mutex);
}

int32_t dt_history_get_end(const int32_t imgid)
{
  if(imgid <= 0) return 0;

  int32_t end = 0;
  _history_stmt_mutex_ensure();
  dt_pthread_mutex_lock(&_history_stmt_mutex);
  if(!_history_get_end_stmt)
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
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


void dt_history_hash_set_mipmap(const int32_t imgid, const dt_image_cache_write_mode_t mode)
{
  if(imgid <= 0) return;

  dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'w');
  if(!img) return;
  img->mipmap_hash = img->history_hash;
  dt_image_cache_write_release(darktable.image_cache, img, mode);
}

gboolean dt_history_copy(int32_t imgid)
{
  // note that this routine does not copy anything, it just setup the copy_paste proxy
  // with the needed information that will be used while pasting.

  if(imgid <= 0) return FALSE;

  darktable.view_manager->copy_paste.copied_imageid = imgid;
  darktable.view_manager->copy_paste.full_copy = TRUE;

  return TRUE;
}

gboolean dt_history_copy_parts(int32_t imgid)
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
    const int32_t dest = GPOINTER_TO_INT(l->data);
    dt_history_copy_and_paste_on_image(darktable.view_manager->copy_paste.copied_imageid,
                                       dest,
                                       darktable.view_manager->copy_paste.selops,
                                       darktable.view_manager->copy_paste.copy_iop_order,
                                       darktable.view_manager->copy_paste.full_copy);
  }

  if(undo) dt_undo_end_group(darktable.undo);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_IMAGE_INFO_CHANGED, g_list_copy((GList *)list));

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
    const int32_t dest = GPOINTER_TO_INT(l->data);
    dt_history_copy_and_paste_on_image(darktable.view_manager->copy_paste.copied_imageid,
                                       dest,
                                       darktable.view_manager->copy_paste.selops,
                                       darktable.view_manager->copy_paste.copy_iop_order,
                                       darktable.view_manager->copy_paste.full_copy);
  }
  if(undo) dt_undo_end_group(darktable.undo);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_IMAGE_INFO_CHANGED, g_list_copy((GList *)list));

  return TRUE;
}

gboolean dt_history_delete_on_list(const GList *list, gboolean undo)
{
  if(!list)  // do we have any images on which to operate?
    return FALSE;

  if(undo) dt_undo_start_group(darktable.undo, DT_UNDO_LT_HISTORY);

  for(GList *l = g_list_first((GList *)list); l; l = g_list_next(l))
  {
    const int32_t imgid = GPOINTER_TO_INT(l->data);
    dt_undo_lt_history_t *hist = dt_history_snapshot_item_init();

    hist->imgid = imgid;
    dt_history_snapshot_undo_create(hist->imgid, &hist->before, &hist->before_history_end);

    dt_history_delete_on_image_ext(imgid, FALSE);

    dt_history_snapshot_undo_create(hist->imgid, &hist->after, &hist->after_history_end);
    dt_undo_record(darktable.undo, NULL, DT_UNDO_LT_HISTORY, (dt_undo_data_t)hist, dt_history_snapshot_undo_pop,
                   dt_history_snapshot_undo_lt_history_data_free);
  }

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_TAG_CHANGED);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_IMAGE_INFO_CHANGED, g_list_copy((GList *)list));

  if(undo) dt_undo_end_group(darktable.undo);
  return TRUE;
}

#undef DT_IOP_ORDER_INFO
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
