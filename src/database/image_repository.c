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

#include "database/image_repository.h"

#include "common/colorlabels.h"
#include "common/database.h"
#include "common/datetime.h"
#include "common/debug.h"
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

void dt_image_from_stmt(dt_image_t *img, sqlite3_stmt *stmt)
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

  dt_colorlabels_set_labels(img->id, img->color_labels);
  _image_write_history_hash(img);
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
