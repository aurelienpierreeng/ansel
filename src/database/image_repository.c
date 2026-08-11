/*
    This file is part of darktable,
    Copyright (C) 2025 Aurélien PIERRE.

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

#include <string.h>

#include "database/image_repository.h"

#include "database/colorlabel_repository.h"
#include "database/database.h"
#include "common/datetime.h"
#include "database/sql_debug.h"
#include "common/image.h"
#include "common/logging.h"
#include "system/dtpthread.h"
#include "system/macros.h"

#include <inttypes.h>
#include <sqlite3.h>

/* One statement cache for the whole process, guarded by one mutex, exactly as the image cache
 * held it before the split: these statements are reused on every image load and every write,
 * and re-preparing them per call was measurable. The mutex is what makes reuse safe, and it
 * is why the calls below serialise against each other. */
static sqlite3_stmt *_image_load_stmt = NULL;
static sqlite3_stmt *_image_write_history_hash_stmt = NULL;
static sqlite3_stmt *_image_write_timestamp_select_stmt = NULL;
static sqlite3_stmt *_image_write_timestamp_update_stmt = NULL;
static sqlite3_stmt *_image_set_flags_stmt = NULL;
static dt_pthread_mutex_t _image_stmt_mutex;
static gsize _image_stmt_mutex_inited = 0;

static inline void _image_stmt_mutex_ensure(void)
{
  if(g_once_init_enter(&_image_stmt_mutex_inited))
  {
    dt_pthread_mutex_init(&_image_stmt_mutex, NULL);
    g_once_init_leave(&_image_stmt_mutex_inited, 1);
  }
}

static sqlite3_stmt *_image_get_stmt(void)
{
  if(IS_NULL_PTR(_image_load_stmt))
  {
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(
        dt_database_get_sqlite3_global(),
        "SELECT i.id, i.group_id, "
        "       (SELECT COUNT(id) FROM main.images WHERE group_id = i.group_id), "
        "       (SELECT COUNT(imgid) FROM main.history WHERE imgid = i.id), "
        "       COALESCE((SELECT current_hash FROM main.history_hash WHERE imgid = i.id), -1), "
        "       COALESCE((SELECT mipmap_hash FROM main.history_hash WHERE imgid = i.id), -1), "
        "       i.film_id, i.version, i.width, i.height, i.orientation, i.flags, "
        "       i.import_timestamp, i.change_timestamp, i.export_timestamp, i.print_timestamp, "
        "       i.exposure, i.exposure_bias, i.aperture, i.iso, i.focal_length, i.focus_distance, "
        "       i.datetime_taken, i.longitude, i.latitude, i.altitude, "
        "       i.filename, f.folder || '" G_DIR_SEPARATOR_S "' || i.filename, "
        "       i.maker, i.model, i.lens, f.folder, "
        "       COALESCE((SELECT SUM(1 << color) FROM main.color_labels WHERE imgid=i.id), 0), "
        "       i.crop, i.raw_parameters, i.color_matrix, i.colorspace, "
        "       i.raw_black, i.raw_maximum, i.aspect_ratio, i.output_width, i.output_height"
        "  FROM main.images AS i"
        "  LEFT JOIN main.film_rolls AS f ON f.id = i.film_id"
        "  WHERE i.id = ?1",
        -1, &_image_load_stmt, NULL);
    // clang-format on
  }

  sqlite3_reset(_image_load_stmt);
  sqlite3_clear_bindings(_image_load_stmt);
  return _image_load_stmt;
}

static void dt_image_from_stmt(dt_image_t *img, sqlite3_stmt *stmt)
{
  if(sqlite3_column_type(stmt, 0) != SQLITE_NULL) img->id = sqlite3_column_int(stmt, 0);
  if(sqlite3_column_type(stmt, 1) != SQLITE_NULL) img->group_id = sqlite3_column_int(stmt, 1);
  if(sqlite3_column_type(stmt, 2) != SQLITE_NULL) img->group_members = (uint32_t)sqlite3_column_int(stmt, 2);
  if(sqlite3_column_type(stmt, 3) != SQLITE_NULL) img->history_items = (uint32_t)sqlite3_column_int(stmt, 3);
  if(sqlite3_column_type(stmt, 4) != SQLITE_NULL) img->history_hash = sqlite3_column_int64(stmt, 4);
  if(sqlite3_column_type(stmt, 5) != SQLITE_NULL) img->mipmap_hash = sqlite3_column_int64(stmt, 5);
  if(sqlite3_column_type(stmt, 6) != SQLITE_NULL) img->film_id = sqlite3_column_int(stmt, 6);
  if(sqlite3_column_type(stmt, 7) != SQLITE_NULL) img->version = sqlite3_column_int(stmt, 7);
  if(sqlite3_column_type(stmt, 8) != SQLITE_NULL) img->width = sqlite3_column_int(stmt, 8);
  if(sqlite3_column_type(stmt, 9) != SQLITE_NULL) img->height = sqlite3_column_int(stmt, 9);
  if(sqlite3_column_type(stmt, 10) != SQLITE_NULL) img->orientation = sqlite3_column_int(stmt, 10);
  if(sqlite3_column_type(stmt, 11) != SQLITE_NULL) img->flags = sqlite3_column_int(stmt, 11);
  if(sqlite3_column_type(stmt, 12) != SQLITE_NULL) img->import_timestamp = sqlite3_column_int64(stmt, 12);
  if(sqlite3_column_type(stmt, 13) != SQLITE_NULL) img->change_timestamp = sqlite3_column_int64(stmt, 13);
  if(sqlite3_column_type(stmt, 14) != SQLITE_NULL) img->export_timestamp = sqlite3_column_int64(stmt, 14);
  if(sqlite3_column_type(stmt, 15) != SQLITE_NULL) img->print_timestamp = sqlite3_column_int64(stmt, 15);
  if(sqlite3_column_type(stmt, 16) != SQLITE_NULL) img->exif_exposure = sqlite3_column_double(stmt, 16);
  if(sqlite3_column_type(stmt, 17) != SQLITE_NULL) img->exif_exposure_bias = sqlite3_column_double(stmt, 17);
  if(sqlite3_column_type(stmt, 18) != SQLITE_NULL) img->exif_aperture = sqlite3_column_double(stmt, 18);
  if(sqlite3_column_type(stmt, 19) != SQLITE_NULL) img->exif_iso = sqlite3_column_double(stmt, 19);
  if(sqlite3_column_type(stmt, 20) != SQLITE_NULL) img->exif_focal_length = sqlite3_column_double(stmt, 20);
  if(sqlite3_column_type(stmt, 21) != SQLITE_NULL) img->exif_focus_distance = sqlite3_column_double(stmt, 21);
  if(sqlite3_column_type(stmt, 22) != SQLITE_NULL) img->exif_datetime_taken = sqlite3_column_int64(stmt, 22);
  if(sqlite3_column_type(stmt, 23) != SQLITE_NULL) img->geoloc.longitude = sqlite3_column_double(stmt, 23);
  if(sqlite3_column_type(stmt, 24) != SQLITE_NULL) img->geoloc.latitude = sqlite3_column_double(stmt, 24);
  if(sqlite3_column_type(stmt, 25) != SQLITE_NULL) img->geoloc.elevation = sqlite3_column_double(stmt, 25);

  const char *filename = (const char *)sqlite3_column_text(stmt, 26);
  if(filename) g_strlcpy(img->filename, filename, sizeof(img->filename));
  const char *fullpath = (const char *)sqlite3_column_text(stmt, 27);
  if(fullpath) g_strlcpy(img->fullpath, fullpath, sizeof(img->fullpath));
  const char *maker = (const char *)sqlite3_column_text(stmt, 28);
  if(maker) g_strlcpy(img->exif_maker, maker, sizeof(img->exif_maker));
  const char *model = (const char *)sqlite3_column_text(stmt, 29);
  if(model) g_strlcpy(img->exif_model, model, sizeof(img->exif_model));
  const char *lens = (const char *)sqlite3_column_text(stmt, 30);
  if(lens) g_strlcpy(img->exif_lens, lens, sizeof(img->exif_lens));
  const char *folder = (const char *)sqlite3_column_text(stmt, 31);
  if(folder) g_strlcpy(img->folder, folder, sizeof(img->folder));

  if(sqlite3_column_type(stmt, 32) != SQLITE_NULL) img->color_labels = sqlite3_column_int(stmt, 32);

  if(sqlite3_column_type(stmt, 33) != SQLITE_NULL) img->exif_crop = sqlite3_column_double(stmt, 33);
  if(sqlite3_column_type(stmt, 34) != SQLITE_NULL)
  {
    uint32_t tmp = sqlite3_column_int(stmt, 34);
    memcpy(&img->legacy_flip, &tmp, sizeof(dt_image_raw_parameters_t));
  }
  const void *color_matrix = sqlite3_column_blob(stmt, 35);
  if(color_matrix) memcpy(img->d65_color_matrix, color_matrix, sizeof(img->d65_color_matrix));
  if(sqlite3_column_type(stmt, 36) != SQLITE_NULL) img->colorspace = sqlite3_column_int(stmt, 36);
  if(sqlite3_column_type(stmt, 37) != SQLITE_NULL) img->raw_black_level = sqlite3_column_int(stmt, 37);
  if(sqlite3_column_type(stmt, 38) != SQLITE_NULL) img->raw_white_point = sqlite3_column_int(stmt, 38);

  if(img->fullpath[0])
    dt_image_local_copy_paths_from_fullpath(img->fullpath, img->id, img->local_copy_path,
                                            sizeof(img->local_copy_path), img->local_copy_legacy_path,
                                            sizeof(img->local_copy_legacy_path));

  img->exif_inited = (img->exif_focus_distance >= 0 && img->orientation >= 0);

  if(img->folder[0])
    g_strlcpy(img->filmroll, dt_image_film_roll_name(img->folder), sizeof(img->filmroll));
  
  dt_datetime_gtimespan_to_local(img->datetime, sizeof(img->datetime), img->exif_datetime_taken, FALSE, FALSE);

  // img->dsc are written by imageio drivers : never (re)set them from DB,
  // they are not saved anyway. Until the codec decodes the file, seed a provisional
  // descriptor from the (extension-derived) pipeline class so the image can be reasoned
  // about and the first pipeline stage has a usable contract even before decoding.
  dt_image_set_provisional_dsc(img);

  /* Everything past the columns -- rating, monochrome and HDR predicates, the
   * extension cross-check, makermodel -- is DERIVED, not stored, and needs symbols from
   * imageio/ and views/. A row mapper that reached two layers up for them is what kept this
   * code in common/. Callers run dt_image_derive_fields() next; see image_repository.h. */

}

static void _image_write_history_hash(const dt_image_t *img)
{
  if(IS_NULL_PTR(img) || img->id <= 0) return;

  _image_stmt_mutex_ensure();
  dt_pthread_mutex_lock(&_image_stmt_mutex);
  if(!_image_write_history_hash_stmt)
  {
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(
        dt_database_get_sqlite3_global(),
        "INSERT INTO main.history_hash (imgid, current_hash, basic_hash, auto_hash, mipmap_hash)"
        " VALUES (?1, ?2, NULL, NULL, ?3)"
        " ON CONFLICT (imgid)"
        " DO UPDATE SET current_hash = ?2, basic_hash = NULL, auto_hash = NULL, mipmap_hash = ?3",
        -1, &_image_write_history_hash_stmt, NULL);
    // clang-format on
  }
  sqlite3_stmt *stmt = _image_write_history_hash_stmt;
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, img->id);
  DT_DEBUG_SQLITE3_BIND_INT64(stmt, 2, (sqlite3_int64)img->history_hash);
  DT_DEBUG_SQLITE3_BIND_INT64(stmt, 3, (sqlite3_int64)img->mipmap_hash);
  sqlite3_step(stmt);
  dt_pthread_mutex_unlock(&_image_stmt_mutex);
}


gboolean dt_image_repository_load(const int32_t imgid, dt_image_t *img)
{
  if(IS_NULL_PTR(img)) return FALSE;

  gboolean found = FALSE;
  _image_stmt_mutex_ensure();
  dt_pthread_mutex_lock(&_image_stmt_mutex);

  sqlite3_stmt *stmt = _image_get_stmt();
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_image_from_stmt(img, stmt);
    found = TRUE;
  }
  else
  {
    img->id = -1;
    fprintf(stderr, "[image_repository_load] failed to open image %" PRId32 " from database: %s\n", imgid,
            sqlite3_errmsg(dt_database_get_sqlite3_global()));
  }

  dt_pthread_mutex_unlock(&_image_stmt_mutex);
  return found;
}

void dt_image_repository_foreach_collected(dt_image_repository_collected_cb cb, void *user_data)
{
  if(IS_NULL_PTR(cb)) return;

  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get_sqlite3_global(),
      // Same columns, same order, as the single-image load above -- dt_image_from_stmt() maps
      // both. Only the source differs: the collection instead of one id.
      "SELECT i.id, i.group_id, "
      "       (SELECT COUNT(id) FROM main.images WHERE group_id = i.group_id), "
      "       (SELECT COUNT(imgid) FROM main.history WHERE imgid = i.id), "
      "       COALESCE((SELECT current_hash FROM main.history_hash WHERE imgid = i.id), -1), "
      "       COALESCE((SELECT mipmap_hash FROM main.history_hash WHERE imgid = i.id), -1), "
      "       i.film_id, i.version, i.width, i.height, i.orientation, i.flags, "
      "       i.import_timestamp, i.change_timestamp, i.export_timestamp, i.print_timestamp, "
      "       i.exposure, i.exposure_bias, i.aperture, i.iso, i.focal_length, i.focus_distance, "
      "       i.datetime_taken, i.longitude, i.latitude, i.altitude, "
      "       i.filename, f.folder || '" G_DIR_SEPARATOR_S "' || i.filename, "
      "       i.maker, i.model, i.lens, f.folder, "
      "       COALESCE((SELECT SUM(1 << color) FROM main.color_labels WHERE imgid=i.id), 0), "
      "       i.crop, i.raw_parameters, i.color_matrix, i.colorspace, "
      "       i.raw_black, i.raw_maximum, i.aspect_ratio, i.output_width, i.output_height"
      "  FROM main.images AS i"
      "  JOIN memory.collected_images AS c ON i.id = c.imgid"
      "  LEFT JOIN main.film_rolls AS f ON f.id = i.film_id"
      "  ORDER BY c.rowid ASC",
      -1, &stmt, NULL);
  // clang-format on
  if(IS_NULL_PTR(stmt)) return;

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_image_t info;
    dt_image_init(&info);
    dt_image_from_stmt(&info, stmt);
    // No lock is held here on purpose: see the header. cb() reaches the selection and the
    // image cache, and either can come back through this function.
    cb(user_data, &info);
  }
  sqlite3_finalize(stmt);
}


void dt_image_repository_store(const dt_image_t *img)
{
  if(IS_NULL_PTR(img) || img->id <= 0) return;

  union {
      struct dt_image_raw_parameters_t s;
      uint32_t u;
  } flip;

  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "UPDATE main.images"
                              " SET width = ?1, height = ?2, filename = ?3, maker = ?4, model = ?5,"
                              "     lens = ?6, exposure = ?7, aperture = ?8, iso = ?9, focal_length = ?10,"
                              "     focus_distance = ?11, film_id = ?12, datetime_taken = ?13, flags = ?14,"
                              "     crop = ?15, orientation = ?16, raw_parameters = ?17, group_id = ?18,"
                              "     longitude = ?19, latitude = ?20, altitude = ?21, color_matrix = ?22,"
                              "     colorspace = ?23, raw_black = ?24, raw_maximum = ?25,"
                              "     aspect_ratio = ROUND(?26,1), exposure_bias = ?27,"
                              "     import_timestamp = ?28, change_timestamp = ?29, export_timestamp = ?30,"
                              "     print_timestamp = ?31, output_width = ?32, output_height = ?33"
                              " WHERE id = ?34",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, img->width);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, img->height);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, img->filename, -1, SQLITE_STATIC);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, img->exif_maker, -1, SQLITE_STATIC);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, img->exif_model, -1, SQLITE_STATIC);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 6, img->exif_lens, -1, SQLITE_STATIC);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 7, img->exif_exposure);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 8, img->exif_aperture);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 9, img->exif_iso);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 10, img->exif_focal_length);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 11, img->exif_focus_distance);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 12, img->film_id);
  if(img->exif_datetime_taken)
    DT_DEBUG_SQLITE3_BIND_INT64(stmt, 13, img->exif_datetime_taken);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 14, img->flags);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 15, img->exif_crop);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 16, img->orientation);
  flip.s = img->legacy_flip;
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 17, flip.u);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 18, img->group_id);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 19, img->geoloc.longitude);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 20, img->geoloc.latitude);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 21, img->geoloc.elevation);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 22, &img->d65_color_matrix, sizeof(img->d65_color_matrix), SQLITE_STATIC);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 23, img->colorspace);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 24, img->raw_black_level);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 25, img->raw_white_point);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 26, 0.); // img->aspect_ratio deprecated
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 27, img->exif_exposure_bias);
  if(img->import_timestamp)
    DT_DEBUG_SQLITE3_BIND_INT64(stmt, 28, img->import_timestamp);
  if(img->change_timestamp)
    DT_DEBUG_SQLITE3_BIND_INT64(stmt, 29, img->change_timestamp);
  if(img->export_timestamp)
    DT_DEBUG_SQLITE3_BIND_INT64(stmt, 30, img->export_timestamp);
  if(img->print_timestamp)
    DT_DEBUG_SQLITE3_BIND_INT64(stmt, 31, img->print_timestamp);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 32, 0); // img->final_width deprecated
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 33, 0); // img->final_height deprecated
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 34, img->id);
  const int rc = sqlite3_step(stmt);
  if(rc != SQLITE_DONE) fprintf(stderr, "[image_cache_write_release] sqlite3 error %d\n", rc);
  sqlite3_finalize(stmt);

  /* Straight to the table. This used to call dt_colorlabels_set_labels() in common/, i.e.
   * the persistence layer reaching up into the domain to have it issue the queries the
   * persistence layer is for -- the only edge in that direction here. */
  for(int color = 0; color < 5; color++)
  {
    if(img->color_labels & (1 << color))
      dt_colorlabel_repository_set(img->id, color);
    else
      dt_colorlabel_repository_remove(img->id, color);
  }
  _image_write_history_hash(img);
}


/* ---------------------------------------------------------------------------------------
 *  Grouping
 * ------------------------------------------------------------------------------------- */

/* Collect the `id` column of a stepped statement into a list, dropping @p exclude_imgid. */
static GList *_collect_ids(sqlite3_stmt *stmt, const int32_t exclude_imgid)
{
  GList *ids = NULL;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int32_t id = sqlite3_column_int(stmt, 0);
    if(id != exclude_imgid) ids = g_list_prepend(ids, GINT_TO_POINTER(id));
  }
  sqlite3_finalize(stmt);
  return g_list_reverse(ids);
}

GList *dt_image_repository_get_group_members(const int32_t group_id, const int32_t exclude_imgid)
{
  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT id FROM main.images WHERE group_id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, group_id);
  return _collect_ids(stmt, exclude_imgid);
}

void dt_image_group_member_free(gpointer data)
{
  dt_image_group_member_t *member = (dt_image_group_member_t *)data;
  if(IS_NULL_PTR(member)) return;
  dt_free(member->filename);
  dt_free(member);
}

GList *dt_image_repository_get_group_member_rows(const int32_t group_id)
{
  GList *members = NULL;
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT id, version, filename"
                              " FROM main.images"
                              " WHERE group_id = ?1", -1, &stmt,
                              NULL);
  // clang-format on
  if(IS_NULL_PTR(stmt)) return NULL;

  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, group_id);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_image_group_member_t *member = (dt_image_group_member_t *)calloc(1, sizeof(dt_image_group_member_t));
    if(IS_NULL_PTR(member)) break;
    member->imgid = sqlite3_column_int(stmt, 0);
    member->version = sqlite3_column_int(stmt, 1);
    const char *filename = (const char *)sqlite3_column_text(stmt, 2);
    member->filename = filename ? g_strdup(filename) : NULL;
    members = g_list_prepend(members, member);
  }
  sqlite3_finalize(stmt);

  // Row order: the caller walks it once to build a tooltip listing the group, and the order
  // it reads is the order the rows came in.
  return g_list_reverse(members);
}

int dt_image_repository_count_in_id_range(const int32_t min_imgid, const int32_t max_imgid)
{
  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT COUNT(*) FROM main.images WHERE id >= ?1 AND id <= ?2", -1,
                              &stmt, 0);
  if(IS_NULL_PTR(stmt)) return -1;

  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, min_imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, max_imgid);
  const int count = (sqlite3_step(stmt) == SQLITE_ROW) ? sqlite3_column_int(stmt, 0) : -1;
  sqlite3_finalize(stmt);

  return count;
}

void dt_image_repository_foreach_in_id_range(const int32_t min_imgid, const int32_t max_imgid,
                                             dt_image_repository_id_filename_cb cb,
                                             void *user_data)
{
  if(IS_NULL_PTR(cb)) return;

  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT id, filename FROM main.images WHERE id >= ?1 AND id <= ?2", -1,
                              &stmt, 0);
  if(IS_NULL_PTR(stmt)) return;

  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, min_imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, max_imgid);
  while(sqlite3_step(stmt) == SQLITE_ROW)
    cb(sqlite3_column_int(stmt, 0), (const char *)sqlite3_column_text(stmt, 1), user_data);
  sqlite3_finalize(stmt);
}


void dt_image_repository_reassign_group(const int32_t from_group_id, const int32_t to_group_id,
                                        const int32_t exclude_imgid)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "UPDATE main.images SET group_id = ?1 WHERE group_id = ?2 AND id != ?3",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, to_group_id);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, from_group_id);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, exclude_imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

GList *dt_image_repository_get_ratings(const int32_t imgid)
{
  sqlite3_stmt *stmt = NULL;

  if(imgid < 0)
  {
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "SELECT flags FROM main.images WHERE id IN "
                                "(SELECT imgid FROM main.selected_images)",
                                -1, &stmt, NULL);
    // clang-format on
  }
  else
  {
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "SELECT flags FROM main.images WHERE id = ?1", -1, &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  }

  GList *result = NULL;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int stars = (sqlite3_column_int(stmt, 0) & 0x7) - 1;
    result = g_list_prepend(result, GINT_TO_POINTER(stars));
  }
  sqlite3_finalize(stmt);

  return g_list_reverse(result);
}

/* ---------------------------------------------------------------------------------------------
 * Identity lookups
 * ------------------------------------------------------------------------------------------ */

int32_t dt_image_repository_find_by_film_and_filename(const int32_t film_id, const char *filename)
{
  if(IS_NULL_PTR(filename)) return -1;

  int32_t id = -1;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT id FROM main.images WHERE film_id = ?1 AND filename = ?2",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, film_id);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, filename, -1, SQLITE_TRANSIENT);
  if(sqlite3_step(stmt) == SQLITE_ROW) id=sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return id;
}

int32_t dt_image_repository_find_by_folder_and_filename(const char *folder, const char *filename)
{
  if(IS_NULL_PTR(folder) || IS_NULL_PTR(filename)) return -1;

  int32_t id = -1;
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT images.id"
                              " FROM main.images, main.film_rolls"
                              " WHERE film_rolls.folder = ?1"
                              "       AND images.film_id = film_rolls.id"
                              "       AND images.filename = ?2",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, folder, -1, SQLITE_STATIC);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, filename, -1, SQLITE_STATIC);
  if(sqlite3_step(stmt) == SQLITE_ROW) id=sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  return id;
}


/* ---------------------------------------------------------------------------------------------
 * Versions, flags and the write timestamp
 * ------------------------------------------------------------------------------------------ */

int dt_image_repository_get_version(const int32_t imgid)
{
  int version = 0;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), "SELECT version FROM main.images WHERE id = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  if(sqlite3_step(stmt) == SQLITE_ROW) version = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return version;
}

gboolean dt_image_repository_set_version(const int32_t imgid, const int version)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2
    (dt_database_get_sqlite3_global(),
     "UPDATE main.images SET version=?1, max_version = ?1 WHERE id = ?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, version);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
  const gboolean ok = (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
  return ok;
}

int dt_image_repository_count_others_with_flag(const int32_t imgid, const int flag)
{
  sqlite3_stmt *stmt;
  int result = 1;

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT COUNT(*)"
                              " FROM main.images"
                              " WHERE id!=?1 AND flags&?2=?2"
                              "   AND film_id=(SELECT film_id"
                              "                FROM main.images"
                              "                WHERE id=?1)"
                              "   AND filename=(SELECT filename"
                              "                 FROM main.images"
                              "                 WHERE id=?1);",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, flag);
  if(sqlite3_step(stmt) == SQLITE_ROW) result = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  return result;
}

/** "1,2,3" for a list of image ids, or NULL when the list is empty.
 *
 *  Built from ints, so there is nothing to escape and nothing to bind: an id set cannot be a
 *  bound parameter in SQLite, which is the trap the callers of these three fell into. */
static char *_id_set(GList *imgids)
{
  if(IS_NULL_PTR(imgids)) return NULL;

  GString *set = g_string_new(NULL);
  for(GList *l = imgids; l; l = g_list_next(l))
  {
    if(l != imgids) g_string_append_c(set, ',');
    g_string_append_printf(set, "%d", GPOINTER_TO_INT(l->data));
  }
  return g_string_free(set, FALSE);
}

GList *dt_image_repository_get_ids_with_flag_among(GList *imgids, const int flag)
{
  char *set = _id_set(imgids);
  if(IS_NULL_PTR(set)) return NULL;

  GList *ids = NULL;
  sqlite3_stmt *stmt;
  char *query = g_strdup_printf("SELECT id FROM main.images WHERE id IN (%s) AND flags&?1=?1", set);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, flag);
  while(sqlite3_step(stmt) == SQLITE_ROW)
    ids = g_list_prepend(ids, GINT_TO_POINTER(sqlite3_column_int(stmt, 0)));
  sqlite3_finalize(stmt);
  dt_free(query);
  dt_free(set);
  return g_list_reverse(ids);   // row order
}

gboolean dt_image_repository_set_flag_among(GList *imgids, const int flag)
{
  char *set = _id_set(imgids);
  if(IS_NULL_PTR(set)) return FALSE;

  sqlite3_stmt *stmt;
  char *query = g_strdup_printf("UPDATE main.images SET flags = (flags|?1) WHERE id IN (%s)", set);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, flag);
  const gboolean ok = (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
  dt_free(query);
  dt_free(set);
  return ok;
}

GList *dt_image_repository_get_full_paths(GList *imgids)
{
  char *set = _id_set(imgids);
  if(IS_NULL_PTR(set)) return NULL;

  GList *list = NULL;
  sqlite3_stmt *stmt;
  // clang-format off
  char *query = g_strdup_printf("SELECT DISTINCT folder || '" G_DIR_SEPARATOR_S "' || filename FROM "
                                "main.images i, main.film_rolls f "
                                "ON i.film_id = f.id WHERE i.id IN (%s)", set);
  // clang-format on
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), query, -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
    list = g_list_prepend(list, g_strdup((const gchar *)sqlite3_column_text(stmt, 0)));
  sqlite3_finalize(stmt);
  dt_free(query);
  dt_free(set);
  return g_list_reverse(list);  // list was built in reverse order, so un-reverse it
}

GList *dt_image_repository_get_ids_with_flag(const int flag)
{
  GList *ids = NULL;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), "SELECT id FROM main.images WHERE flags&?1=?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, flag);

  while(sqlite3_step(stmt) == SQLITE_ROW)
    ids = g_list_prepend(ids, GINT_TO_POINTER(sqlite3_column_int(stmt, 0)));
  sqlite3_finalize(stmt);

  // Row order, NOT reverse-row: the caller filters this list and prepends into its own, and
  // that second prepend reproduces the single prepend the original did off the cursor.
  return g_list_reverse(ids);
}

gboolean dt_image_repository_set_flags(const int32_t imgid, const int flags)
{
  if(imgid <= 0) return FALSE;

  _image_stmt_mutex_ensure();
  dt_pthread_mutex_lock(&_image_stmt_mutex);
  if(IS_NULL_PTR(_image_set_flags_stmt))
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "UPDATE main.images SET flags = ?1 WHERE id = ?2",
                                -1, &_image_set_flags_stmt, NULL);
  }
  if(IS_NULL_PTR(_image_set_flags_stmt))
  {
    dt_pthread_mutex_unlock(&_image_stmt_mutex);
    return FALSE;
  }

  DT_DEBUG_SQLITE3_BIND_INT(_image_set_flags_stmt, 1, flags);
  DT_DEBUG_SQLITE3_BIND_INT(_image_set_flags_stmt, 2, imgid);
  const gboolean ok = (sqlite3_step(_image_set_flags_stmt) == SQLITE_DONE);
  sqlite3_reset(_image_set_flags_stmt);
  sqlite3_clear_bindings(_image_set_flags_stmt);
  dt_pthread_mutex_unlock(&_image_stmt_mutex);

  return ok;
}

void dt_image_repository_foreach_with_path(dt_image_repository_path_row_cb cb, void *user_data)
{
  if(IS_NULL_PTR(cb)) return;

  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT i.id, write_timestamp, version,"
                              "       folder || '" G_DIR_SEPARATOR_S "' || filename, flags"
                              " FROM main.images i, main.film_rolls f"
                              " ON i.film_id = f.id"
                              " ORDER BY f.id, filename",
                              -1, &stmt, NULL);
  // clang-format on
  if(IS_NULL_PTR(stmt)) return;

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    // No lock is held here on purpose: cb() writes back through this repository.
    cb(sqlite3_column_int(stmt, 0),
       sqlite3_column_int64(stmt, 1),
       sqlite3_column_int(stmt, 2),
       (const char *)sqlite3_column_text(stmt, 3),
       sqlite3_column_int(stmt, 4),
       user_data);
  }
  sqlite3_finalize(stmt);
}

int *dt_image_repository_count_distinct_fields(GList *imgids)
{
  char *set = _id_set(imgids);
  if(IS_NULL_PTR(set)) return NULL;

  // One subquery per metadata key, each needing the id set again -- nine occurrences in all.
  // clang-format off
  char *query = g_strdup_printf("SELECT COUNT(DISTINCT film_id), "
                                       "COUNT(DISTINCT film_id), "
                                       "2, " // imgid always different
                                       "COUNT(DISTINCT group_id), "
                                       "COUNT(DISTINCT filename), "
                                       "COUNT(DISTINCT version), "
                                       "COUNT(DISTINCT film_id || '/' || filename), " //path
                                       "COUNT(DISTINCT flags & 2048), " //local copy
                                       "COUNT(DISTINCT import_timestamp), "
                                       "COUNT(DISTINCT change_timestamp), "
                                       "COUNT(DISTINCT export_timestamp), "
                                       "COUNT(DISTINCT print_timestamp), "
                                       "COUNT(DISTINCT flags), "
                                       "COUNT(DISTINCT model), "
                                       "COUNT(DISTINCT maker), "
                                       "COUNT(DISTINCT lens), "
                                       "COUNT(DISTINCT aperture), "
                                       "COUNT(DISTINCT exposure), "
                                       "COUNT(DISTINCT IFNULL(exposure_bias, '')), "
                                       "COUNT(DISTINCT focal_length), "
                                       "COUNT(DISTINCT focus_distance), "
                                       "COUNT(DISTINCT iso), "
                                       "COUNT(DISTINCT datetime_taken), "
                                       "COUNT(DISTINCT width), "
                                       "COUNT(DISTINCT height), "
                                       "COUNT(DISTINCT IFNULL(output_width, '')), " //exported width
                                       "COUNT(DISTINCT IFNULL(output_height, '')), " //exported height
                                       "(SELECT COUNT(DISTINCT IFNULL(value,'')) FROM images LEFT JOIN meta_data ON meta_data.id = images.id AND key = 2 WHERE images.id in (%s)), " //title
                                       "(SELECT COUNT(DISTINCT IFNULL(value,'')) FROM images LEFT JOIN meta_data ON meta_data.id = images.id AND key = 3 WHERE images.id in (%s)), " //description
                                       "(SELECT COUNT(DISTINCT IFNULL(value,'')) FROM images LEFT JOIN meta_data ON meta_data.id = images.id AND key = 0 WHERE images.id in (%s)), " //creator
                                       "(SELECT COUNT(DISTINCT IFNULL(value,'')) FROM images LEFT JOIN meta_data ON meta_data.id = images.id AND key = 1 WHERE images.id in (%s)), " //publisher
                                       "(SELECT COUNT(DISTINCT IFNULL(value,'')) FROM images LEFT JOIN meta_data ON meta_data.id = images.id AND key = 4 WHERE images.id in (%s)), " //rights
                                       "(SELECT COUNT(DISTINCT IFNULL(value,'')) FROM images LEFT JOIN meta_data ON meta_data.id = images.id AND key = 5 WHERE images.id in (%s)), " //notes
                                       "(SELECT COUNT(DISTINCT IFNULL(value,'')) FROM images LEFT JOIN meta_data ON meta_data.id = images.id AND key = 6 WHERE images.id in (%s)), " //version name
                                       "(SELECT COUNT(DISTINCT IFNULL(value,'')) FROM images LEFT JOIN meta_data ON meta_data.id = images.id AND key = 7 WHERE images.id in (%s)), " //image id
                                       "COUNT(DISTINCT IFNULL(latitude, '')), "
                                       "COUNT(DISTINCT IFNULL(longitude, '')), "
                                       "COUNT(DISTINCT IFNULL(altitude, '')) "
                                       "FROM main.images "
                                       "WHERE id IN (%s)",
                                 set, set, set, set, set, set, set, set, set);
  // clang-format on

  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), query, -1, &stmt, NULL);
  dt_free(query);
  dt_free(set);
  if(IS_NULL_PTR(stmt)) return NULL;

  int *counts = NULL;
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int col_count = sqlite3_column_count(stmt);
    counts = (int *)calloc(DT_IMAGE_FIELD_COUNT, sizeof(int));
    if(counts)
    {
      for(int i = 0; i < DT_IMAGE_FIELD_COUNT && i < col_count; i++)
        counts[i] = sqlite3_column_int(stmt, i);
    }
  }
  sqlite3_finalize(stmt);

  return counts;
}

gboolean dt_image_repository_get_collected_geo_bounds(dt_image_geo_bounds_t *bounds)
{
  if(IS_NULL_PTR(bounds)) return FALSE;

  bounds->min_latitude = INFINITY;
  bounds->max_latitude = -INFINITY;
  bounds->min_longitude = INFINITY;
  bounds->max_longitude = -INFINITY;
  bounds->count = 0;

  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT MIN(latitude), MAX(latitude),"
                              "       MIN(longitude), MAX(longitude), COUNT(*)"
                              " FROM main.images AS i "
                              " JOIN memory.collected_images AS l ON l.imgid = i.id "
                              " WHERE latitude NOT NULL AND longitude NOT NULL",
                              -1, &stmt, NULL);
  // clang-format on
  if(IS_NULL_PTR(stmt)) return FALSE;

  const gboolean ok = (sqlite3_step(stmt) == SQLITE_ROW);
  if(ok)
  {
    bounds->min_latitude = sqlite3_column_double(stmt, 0);
    bounds->max_latitude = sqlite3_column_double(stmt, 1);
    bounds->min_longitude = sqlite3_column_double(stmt, 2);
    bounds->max_longitude = sqlite3_column_double(stmt, 3);
    bounds->count = sqlite3_column_int(stmt, 4);
  }
  sqlite3_finalize(stmt);

  return ok;
}

dt_image_geo_point_t *dt_image_repository_get_collected_geo_points(const double lon1, const double lon2,
                                                                   const double lat1, const double lat2,
                                                                   int *count)
{
  if(IS_NULL_PTR(count)) return NULL;
  *count = 0;

  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT * FROM"
                              " (SELECT i.id, i.longitude, i.latitude "
                              "   FROM main.images i INNER JOIN memory.collected_images c ON i.id = c.imgid"
                              "   WHERE longitude >= ?1 AND longitude <= ?2"
                              "           AND latitude <= ?3 AND latitude >= ?4 "
                              "           AND longitude NOT NULL AND latitude NOT NULL)"
                              "   ORDER BY longitude ASC",  // critical to make dbscan work
                              -1, &stmt, NULL);
  // clang-format on
  if(IS_NULL_PTR(stmt)) return NULL;

  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 1, lon1);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 2, lon2);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 3, lat1);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 4, lat2);

  // One pass into a growing array, where the caller used to step the whole cursor once to
  // count the rows and a second time to fill an exactly-sized buffer.
  GArray *points = g_array_new(FALSE, FALSE, sizeof(dt_image_geo_point_t));
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_image_geo_point_t p = { .imgid = sqlite3_column_int(stmt, 0),
                               .longitude = sqlite3_column_double(stmt, 1),
                               .latitude = sqlite3_column_double(stmt, 2) };
    g_array_append_val(points, p);
  }
  sqlite3_finalize(stmt);

  *count = (int)points->len;
  if(points->len == 0)
  {
    g_array_free(points, TRUE);
    return NULL;
  }

  return (dt_image_geo_point_t *)g_array_free(points, FALSE);  // hand over the buffer
}

int64_t dt_image_repository_get_write_timestamp(const int32_t imgid)
{
  if(imgid <= 0) return 0;

  _image_stmt_mutex_ensure();
  dt_pthread_mutex_lock(&_image_stmt_mutex);
  if(IS_NULL_PTR(_image_write_timestamp_select_stmt))
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "SELECT write_timestamp FROM main.images WHERE id = ?1",
                                -1, &_image_write_timestamp_select_stmt, NULL);
  }
  if(IS_NULL_PTR(_image_write_timestamp_select_stmt))
  {
    dt_pthread_mutex_unlock(&_image_stmt_mutex);
    return 0;
  }

  int64_t write_timestamp = 0;
  DT_DEBUG_SQLITE3_BIND_INT(_image_write_timestamp_select_stmt, 1, imgid);
  if(sqlite3_step(_image_write_timestamp_select_stmt) == SQLITE_ROW)
    write_timestamp = sqlite3_column_int64(_image_write_timestamp_select_stmt, 0);
  sqlite3_reset(_image_write_timestamp_select_stmt);
  sqlite3_clear_bindings(_image_write_timestamp_select_stmt);
  dt_pthread_mutex_unlock(&_image_stmt_mutex);

  return write_timestamp;
}

void dt_image_repository_touch_write_timestamp(const int32_t imgid)
{
  if(imgid <= 0) return;

  _image_stmt_mutex_ensure();
  dt_pthread_mutex_lock(&_image_stmt_mutex);
  if(!_image_write_timestamp_update_stmt)
  {
    DT_DEBUG_SQLITE3_PREPARE_V2
      (dt_database_get_sqlite3_global(),
       "UPDATE main.images SET write_timestamp = STRFTIME('%s', 'now') WHERE id = ?1",
       -1, &_image_write_timestamp_update_stmt, NULL);
  }
  if(_image_write_timestamp_update_stmt)
  {
    DT_DEBUG_SQLITE3_BIND_INT(_image_write_timestamp_update_stmt, 1, imgid);
    sqlite3_step(_image_write_timestamp_update_stmt);
    sqlite3_reset(_image_write_timestamp_update_stmt);
    sqlite3_clear_bindings(_image_write_timestamp_update_stmt);
  }
  dt_pthread_mutex_unlock(&_image_stmt_mutex);
}

gboolean dt_image_repository_set_write_timestamp(const int32_t imgid, const int64_t timestamp)
{
  if(imgid <= 0) return FALSE;

  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "UPDATE main.images SET write_timestamp = ?2 WHERE id = ?1",
                              -1, &stmt, NULL);
  if(IS_NULL_PTR(stmt)) return FALSE;

  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT64(stmt, 2, timestamp);
  const gboolean ok = (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);

  return ok;
}

gboolean dt_image_repository_delete(const int32_t imgid)
{
  gboolean ok = TRUE;
  sqlite3_stmt *stmt;

  // due to foreign keys added in db version 33,
  // all entries from tables having references to the images are deleted as well
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), "DELETE FROM main.images WHERE id = ?1", -1, &stmt,
                              NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  ok &= (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);   // the original overwrote `stmt` here and leaked this statement

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "DELETE FROM main.meta_data WHERE id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  ok &= (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);

  return ok;
}

int32_t dt_image_repository_duplicate(const int32_t imgid, const int32_t newversion)
{
  sqlite3_stmt *stmt;
  int32_t newid = -1;

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT a.id"
                              "  FROM main.images AS a JOIN main.images AS b"
                              "  WHERE a.film_id = b.film_id AND a.filename = b.filename"
                              "   AND b.id = ?1 AND a.version = ?2"
                              "  ORDER BY a.id DESC",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, newversion);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    newid = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);

  // requested version is already present in DB, so we just return it
  if(newid != -1) return newid;

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2
    (dt_database_get_sqlite3_global(),
     "INSERT INTO main.images"
     "  (id, group_id, film_id, width, height, filename, maker, model, lens, exposure,"
     "   aperture, iso, focal_length, focus_distance, datetime_taken, flags,"
     "   output_width, output_height, crop, raw_parameters, raw_denoise_threshold,"
     "   raw_auto_bright_threshold, raw_black, raw_maximum,"
     "   license, sha1sum, orientation, histogram, lightmap,"
     "   longitude, latitude, altitude, color_matrix, colorspace, version, max_version, history_end,"
     "   aspect_ratio, exposure_bias, import_timestamp)"
     " SELECT NULL, group_id, film_id, width, height, filename, maker, model, lens,"
     "       exposure, aperture, iso, focal_length, focus_distance, datetime_taken,"
     "       flags, output_width, output_height, crop, raw_parameters, raw_denoise_threshold,"
     "       raw_auto_bright_threshold, raw_black, raw_maximum,"
     "       license, sha1sum, orientation, histogram, lightmap,"
     "       longitude, latitude, altitude, color_matrix, colorspace, NULL, NULL, 0,"
     "       aspect_ratio, exposure_bias, import_timestamp"
     " FROM main.images WHERE id = ?1",
     -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT a.id, a.film_id, a.filename, b.max_version"
                              "  FROM main.images AS a JOIN main.images AS b"
                              "  WHERE a.film_id = b.film_id AND a.filename = b.filename AND b.id = ?1"
                              "  ORDER BY a.id DESC",
    -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  int32_t film_id = UNKNOWN_IMAGE;
  int32_t max_version = -1;
  gchar *filename = NULL;
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    newid = sqlite3_column_int(stmt, 0);
    film_id = sqlite3_column_int(stmt, 1);
    filename = g_strdup((gchar *)sqlite3_column_text(stmt, 2));
    max_version = sqlite3_column_int(stmt, 3);
  }
  sqlite3_finalize(stmt);

  if(newid != -1)
  {
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "INSERT INTO main.color_labels (imgid, color)"
                                "  SELECT ?1, color FROM main.color_labels WHERE imgid = ?2",
                                -1, &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "INSERT INTO main.meta_data (id, key, value)"
                                "  SELECT ?1, key, value FROM main.meta_data WHERE id = ?2",
                                -1, &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "INSERT INTO main.tagged_images (imgid, tagid, position)"
                                "  SELECT ?1, tagid, "
                                "        (SELECT (IFNULL(MAX(position),0) & 0xFFFFFFFF00000000)"
                                "         FROM main.tagged_images)"
                                "         + (ROW_NUMBER() OVER (ORDER BY imgid) << 32)"
                                " FROM main.tagged_images AS ti"
                                " WHERE imgid = ?2",
                                -1, &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "INSERT INTO main.module_order (imgid, iop_list, version)"
                                "  SELECT ?1, iop_list, version FROM main.module_order WHERE imgid = ?2",
                                -1, &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // set version of new entry and max_version of all involved duplicates (with same film_id and filename)
    // this needs to happen before we do anything with the image cache, as version isn't updated through the cache
    const int32_t version = (newversion != -1) ? newversion : max_version + 1;
    max_version = (newversion != -1) ? MAX(max_version, newversion) : max_version + 1;

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), "UPDATE main.images SET version=?1 WHERE id = ?2",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, version);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, newid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "UPDATE main.images SET max_version=?1 WHERE film_id = ?2 AND filename = ?3", -1,
                                &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, max_version);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, film_id);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, filename, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    dt_free(filename);
  }
  return newid;
}

void dt_image_version_free(gpointer data)
{
  dt_image_version_t *v = (dt_image_version_t *)data;
  if(IS_NULL_PTR(v)) return;
  dt_free(v->version_name);
  dt_free(v);
}

GList *dt_image_repository_get_versions(const int32_t film_id, const char *filename,
                                        const int name_keyid)
{
  if(IS_NULL_PTR(filename)) return NULL;

  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT i.version, i.id, m.value"
                              " FROM images AS i"
                              " LEFT JOIN meta_data AS m ON m.id = i.id AND m.key = ?3"
                              " WHERE film_id = ?1 AND filename = ?2"
                              " ORDER BY i.version",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, film_id);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, filename, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, name_keyid);

  GList *versions = NULL;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_image_version_t *v = g_malloc0(sizeof(dt_image_version_t));
    if(v)
    {
      v->version = sqlite3_column_int(stmt, 0);
      v->imgid = sqlite3_column_int(stmt, 1);
      const char *name = (const char *)sqlite3_column_text(stmt, 2);
      v->version_name = name ? g_strdup(name) : NULL;
      versions = g_list_prepend(versions, v);
    }
  }
  sqlite3_finalize(stmt);

  return g_list_reverse(versions);   // version order, as the query returns it
}

GList *dt_image_repository_get_duplicate_ids(const int32_t imgid)
{
  GList *ids = NULL;
  sqlite3_stmt *stmt;
  // statement for getting ids of the image to be moved and its duplicates
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2
    (dt_database_get_sqlite3_global(),
     "SELECT id"
     " FROM main.images"
     " WHERE filename IN (SELECT filename FROM main.images WHERE id = ?1)"
     "   AND film_id IN (SELECT film_id FROM main.images WHERE id = ?1)",
     -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  while(sqlite3_step(stmt) == SQLITE_ROW)
    ids = g_list_prepend(ids, GINT_TO_POINTER(sqlite3_column_int(stmt, 0)));
  sqlite3_finalize(stmt);

  // Row order: the original prepended off the cursor and then reversed, and its caller walks
  // the list without prepending again.
  return g_list_reverse(ids);
}

int32_t dt_image_repository_copy_to_film(const int32_t imgid, const int32_t filmid,
                                        const char *new_filename, const char *old_filename)
{
  int32_t newid = -1;
  gchar *filename = NULL;
  sqlite3_stmt *stmt;

// update database
// clang-format off
DT_DEBUG_SQLITE3_PREPARE_V2
  (dt_database_get_sqlite3_global(),
   "INSERT INTO main.images"
   "  (id, group_id, film_id, width, height, filename, maker, model, lens, exposure,"
   "   aperture, iso, focal_length, focus_distance, datetime_taken, flags,"
   "   output_width, output_height, crop, raw_parameters, raw_denoise_threshold,"
   "   raw_auto_bright_threshold, raw_black, raw_maximum,"
   "   license, sha1sum, orientation, histogram, lightmap,"
   "   longitude, latitude, altitude, color_matrix, colorspace, version, max_version,"
   "   aspect_ratio, exposure_bias)"
   " SELECT NULL, group_id, ?1 as film_id, width, height, ?2 as filename, maker, model, lens,"
   "        exposure, aperture, iso, focal_length, focus_distance, datetime_taken,"
   "        flags, width, height, crop, raw_parameters, raw_denoise_threshold,"
   "        raw_auto_bright_threshold, raw_black, raw_maximum,"
   "        license, sha1sum, orientation, histogram, lightmap,"
   "        longitude, latitude, altitude, color_matrix, colorspace, -1, -1,"
   "        aspect_ratio, exposure_bias"
   " FROM main.images"
   " WHERE id = ?3",
  -1, &stmt, NULL);
// clang-format on
DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, filmid);
DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, new_filename, -1, SQLITE_TRANSIENT);
DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, imgid);
sqlite3_step(stmt);
sqlite3_finalize(stmt);
// clang-format off
DT_DEBUG_SQLITE3_PREPARE_V2
  (dt_database_get_sqlite3_global(),
   "SELECT a.id, a.filename"
   " FROM main.images AS a"
   " JOIN main.images AS b"
   "   WHERE a.film_id = ?1 AND a.filename = ?2 AND b.filename = ?3 AND b.id = ?4"
   "   ORDER BY a.id DESC",
   -1, &stmt, NULL);
// clang-format on
DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, filmid);
DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, new_filename, -1, SQLITE_TRANSIENT);
DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, old_filename, -1, SQLITE_TRANSIENT);
DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, imgid);

if(sqlite3_step(stmt) == SQLITE_ROW)
{
  newid = sqlite3_column_int(stmt, 0);
  filename = g_strdup((gchar *)sqlite3_column_text(stmt, 1));
}
sqlite3_finalize(stmt);

if(newid != -1)
{
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "INSERT INTO main.color_labels (imgid, color)"
                              " SELECT ?1, color"
                              " FROM main.color_labels"
                              " WHERE imgid = ?2",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "INSERT INTO main.meta_data (id, key, value)"
                              " SELECT ?1, key, value"
                              " FROM main.meta_data"
                              " WHERE id = ?2",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "INSERT INTO main.tagged_images (imgid, tagid, position)"
                              " SELECT ?1, tagid, "
                              "        (SELECT (IFNULL(MAX(position),0) & 0xFFFFFFFF00000000)"
                              "         FROM main.tagged_images)"
                              "         + (ROW_NUMBER() OVER (ORDER BY imgid) << 32)"
                              " FROM main.tagged_images AS ti"
                              " WHERE imgid = ?2",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // get max_version of image duplicates in destination filmroll
  int32_t max_version = -1;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2
    (dt_database_get_sqlite3_global(),
     "SELECT MAX(a.max_version)"
     " FROM main.images AS a"
     " JOIN main.images AS b"
     "   WHERE a.film_id = b.film_id AND a.filename = b.filename AND b.id = ?1",
     -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);

  if(sqlite3_step(stmt) == SQLITE_ROW) max_version = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  // set version of new entry and max_version of all involved duplicates (with same film_id and
  // filename)
  max_version = (max_version >= 0) ? max_version + 1 : 0;
  int32_t version = max_version;

  DT_DEBUG_SQLITE3_PREPARE_V2
    (dt_database_get_sqlite3_global(),
     "UPDATE main.images SET version=?1 WHERE id = ?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, version);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, newid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2
    (dt_database_get_sqlite3_global(),
     "UPDATE main.images SET max_version=?1 WHERE film_id = ?2 AND filename = ?3",
     -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, max_version);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, filmid);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, filename, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // image group handling follows
  // get group_id of potential image duplicates in destination filmroll
  int32_t new_group_id = -1;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2
    (dt_database_get_sqlite3_global(),
     "SELECT DISTINCT a.group_id"
     " FROM main.images AS a"
     " JOIN main.images AS b"
     "   WHERE a.film_id = b.film_id AND a.filename = b.filename"
     "     AND b.id = ?1 AND a.id != ?1",
     -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, newid);

  if(sqlite3_step(stmt) == SQLITE_ROW) new_group_id = sqlite3_column_int(stmt, 0);

  // then check if there are further duplicates belonging to different group(s)
  if(sqlite3_step(stmt) == SQLITE_ROW) new_group_id = -1;
  sqlite3_finalize(stmt);

  // rationale:
  // if no group exists or if the image duplicates belong to multiple groups, then the
  // new image builds a group of its own, else it is added to the (one) existing group
  if(new_group_id == -1) new_group_id = newid;

  // make copied image belong to a group
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "UPDATE main.images SET group_id=?1 WHERE id = ?2", -1, &stmt, NULL);

  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, new_group_id);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, newid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  }

  dt_free(filename);
  return newid;
}

gboolean dt_image_repository_insert_import(const int32_t film_id, const char *filename,
                                           const int flags, const int64_t import_timestamp)
{
  sqlite3_stmt *stmt;
  //insert a v0 record (which may be updated later if no v0 xmp exists)
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2
    (dt_database_get_sqlite3_global(),
     "INSERT INTO main.images (id, film_id, filename, license, sha1sum, flags, version, "
     "                         max_version, history_end, position, import_timestamp)"
     " SELECT NULL, ?1, ?2, '', '', ?3, 0, 0, 0, (IFNULL(MAX(position),0) & 0xFFFFFFFF00000000)  + (1 << 32), ?4 "
     " FROM images",
     -1, &stmt, NULL);
  // clang-format on

  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, film_id);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, filename, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, flags);
  DT_DEBUG_SQLITE3_BIND_INT64(stmt, 4, import_timestamp);

  const int rc = sqlite3_step(stmt);
  if(rc != SQLITE_DONE) fprintf(stderr, "sqlite3 error %d\n", rc);
  sqlite3_finalize(stmt);
  return (rc == SQLITE_DONE);
}

int32_t dt_image_repository_find_group_for_pattern(const int32_t film_id,
                                                   const char *filename_pattern,
                                                   const gboolean leader_only,
                                                   const int32_t exclude_imgid)
{
  int32_t group_id = -1;
  sqlite3_stmt *stmt;

  if(leader_only)
  {
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2
      (dt_database_get_sqlite3_global(),
       "SELECT group_id"
       " FROM main.images"
       " WHERE film_id = ?1 AND filename LIKE ?2 AND id = group_id", -1, &stmt,
      NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, film_id);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, filename_pattern, -1, SQLITE_TRANSIENT);
  }
  else
  {
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2
      (dt_database_get_sqlite3_global(),
       "SELECT group_id"
       " FROM main.images"
       " WHERE film_id = ?1 AND filename LIKE ?2 AND id != ?3", -1, &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, film_id);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, filename_pattern, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, exclude_imgid);
  }

  if(sqlite3_step(stmt) == SQLITE_ROW) group_id = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return group_id;
}

gboolean dt_image_repository_set_group(const int32_t imgid, const int32_t group_id)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2
    (dt_database_get_sqlite3_global(),
     "UPDATE main.images SET group_id = ?1 WHERE id = ?2",
     -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, group_id);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
  const gboolean ok = (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
  return ok;
}

gboolean dt_image_repository_get_xmp_row(const int32_t imgid, dt_image_xmp_row_t *row)
{
  if(IS_NULL_PTR(row)) return FALSE;
  memset(row, 0, sizeof(*row));

  gboolean found = FALSE;
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT filename, flags, raw_parameters, "
                              "       longitude, latitude, altitude, history_end, datetime_taken"
                              " FROM main.images"
                              " WHERE id = ?1",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    // copied, not borrowed: the caller used to read it before finalising the statement, and
    // owning it removes the constraint rather than moving it across a module boundary
    const char *f = (const char *)sqlite3_column_text(stmt, 0);
    row->filename = f ? g_strdup(f) : NULL;
    row->flags = sqlite3_column_int(stmt, 1);
    row->raw_parameters = sqlite3_column_int(stmt, 2);
    if(sqlite3_column_type(stmt, 3) == SQLITE_FLOAT)
    {
      row->longitude = sqlite3_column_double(stmt, 3);
      row->has_longitude = TRUE;
    }
    if(sqlite3_column_type(stmt, 4) == SQLITE_FLOAT)
    {
      row->latitude = sqlite3_column_double(stmt, 4);
      row->has_latitude = TRUE;
    }
    if(sqlite3_column_type(stmt, 5) == SQLITE_FLOAT)
    {
      row->altitude = sqlite3_column_double(stmt, 5);
      row->has_altitude = TRUE;
    }
    row->history_end = sqlite3_column_int(stmt, 6);
    row->datetime_taken = sqlite3_column_int64(stmt, 7);
    found = TRUE;
  }
  sqlite3_finalize(stmt);
  return found;
}

void dt_image_repository_xmp_row_cleanup(dt_image_xmp_row_t *row)
{
  if(IS_NULL_PTR(row)) return;
  dt_free(row->filename);
  row->filename = NULL;
}

gboolean dt_image_repository_get_timestamps(const int32_t imgid, dt_image_timestamps_t *ts)
{
  if(IS_NULL_PTR(ts)) return FALSE;
  memset(ts, 0, sizeof(*ts));

  gboolean found = FALSE;
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get_sqlite3_global(),
      "SELECT import_timestamp, change_timestamp, export_timestamp, print_timestamp"
      " FROM main.images"
      " WHERE id = ?1",
      -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  // clang-format on

  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    if(sqlite3_column_type(stmt, 0) != SQLITE_NULL)
    {
      ts->import_timestamp = sqlite3_column_int64(stmt, 0);
      ts->has_import = TRUE;
    }
    if(sqlite3_column_type(stmt, 1) != SQLITE_NULL)
    {
      ts->change_timestamp = sqlite3_column_int64(stmt, 1);
      ts->has_change = TRUE;
    }
    if(sqlite3_column_type(stmt, 2) != SQLITE_NULL)
    {
      ts->export_timestamp = sqlite3_column_int64(stmt, 2);
      ts->has_export = TRUE;
    }
    if(sqlite3_column_type(stmt, 3) != SQLITE_NULL)
    {
      ts->print_timestamp = sqlite3_column_int64(stmt, 3);
      ts->has_print = TRUE;
    }
    found = TRUE;
  }
  sqlite3_finalize(stmt);
  return found;
}

void dt_image_repository_cleanup(void)
{
  if(_image_load_stmt)
  {
    sqlite3_finalize(_image_load_stmt);
    _image_load_stmt = NULL;
  }
  if(_image_write_history_hash_stmt)
  {
    sqlite3_finalize(_image_write_history_hash_stmt);
    _image_write_history_hash_stmt = NULL;
  }
  if(_image_write_timestamp_select_stmt)
  {
    sqlite3_finalize(_image_write_timestamp_select_stmt);
    _image_write_timestamp_select_stmt = NULL;
  }
  if(_image_write_timestamp_update_stmt)
  {
    sqlite3_finalize(_image_write_timestamp_update_stmt);
    _image_write_timestamp_update_stmt = NULL;
  }
  if(_image_set_flags_stmt)
  {
    sqlite3_finalize(_image_set_flags_stmt);
    _image_set_flags_stmt = NULL;
  }

  if(_image_stmt_mutex_inited)
  {
    dt_pthread_mutex_destroy(&_image_stmt_mutex);
    _image_stmt_mutex_inited = 0;
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
