/*
    This file is part of the Ansel project.
    Copyright (C) 2026 Aurélien PIERRE.
    
    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    
    Ansel is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    along with Ansel.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "dtgtk/thumbtable_info.h"

#include "common/darktable.h"
#include "common/datetime.h"
#include "common/debug.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/ratings.h"
#include "develop/imageop.h"
#include "views/view.h"

#include <glib.h>
#include <glib/gi18n.h>
#include <math.h>
#include <string.h>

static sqlite3_stmt *_thumbtable_collection_stmt = NULL;

void dt_thumbtable_copy_image(dt_image_t *info, const dt_image_t *img)
{
  if(!info || !img) return;

  memcpy(info, img, sizeof(dt_image_t));
}

void dt_thumbtable_info_seed_image_cache(const dt_image_t *info)
{
  if(!info || info->id <= 0) return;

  if(!darktable.image_cache) return;

  dt_image_cache_seed(darktable.image_cache, info);
}

sqlite3_stmt *dt_thumbtable_info_get_collection_stmt(void)
{
  if(!_thumbtable_collection_stmt)
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(
        dt_database_get(darktable.db),
        // Batch-fetch thumbnail metadata in one SQL query to avoid one query per image
        // through the image cache. This keeps scrolling lightweight and predictable.
        "SELECT im.id, im.group_id, "
        "(SELECT COUNT(id) FROM main.images WHERE group_id=im.group_id), "
        "(SELECT COUNT(imgid) FROM main.history WHERE imgid=c.imgid), "
        "im.film_id, im.version, im.width, im.height, im.orientation, "
        "im.flags, "
        "im.import_timestamp, im.change_timestamp, im.export_timestamp, im.print_timestamp, "
        "im.exposure, im.exposure_bias, im.aperture, im.iso, im.focal_length, im.focus_distance, "
        "im.datetime_taken, "
        "im.longitude, im.latitude, im.altitude, "
        "im.filename, fr.folder || '" G_DIR_SEPARATOR_S "' || im.filename, "
        "im.maker, im.model, im.lens, fr.folder, "
        "COALESCE((SELECT SUM(1 << color) FROM main.color_labels WHERE imgid=im.id), 0), "
        "im.crop, im.raw_parameters, im.color_matrix, im.colorspace, "
        "im.raw_black, im.raw_maximum, im.aspect_ratio, im.output_width, im.output_height "
        "FROM main.images AS im "
        "JOIN memory.collected_images AS c ON im.id = c.imgid "
        "LEFT JOIN main.film_rolls AS fr ON fr.id = im.film_id "
        "ORDER BY c.rowid ASC",
        -1, &_thumbtable_collection_stmt, NULL);
  }

  sqlite3_reset(_thumbtable_collection_stmt);
  sqlite3_clear_bindings(_thumbtable_collection_stmt);
  return _thumbtable_collection_stmt;
}

void dt_thumbtable_info_cleanup(void)
{
  if(_thumbtable_collection_stmt)
  {
    sqlite3_finalize(_thumbtable_collection_stmt);
    _thumbtable_collection_stmt = NULL;
  }
}

#ifndef NDEBUG
static gboolean _thumbtable_float_equal(const float a, const float b)
{
  return (isnan(a) && isnan(b)) || a == b;
}

static gboolean _thumbtable_double_equal(const double a, const double b)
{
  return (isnan(a) && isnan(b)) || a == b;
}

void dt_thumbtable_info_debug_assert_matches_cache(const dt_image_t *sql_info,
                                                   uint32_t history_items, uint32_t group_members)
{
  if(!sql_info || sql_info->id <= 0) return;

  const dt_image_t *img = dt_image_cache_get(darktable.image_cache, sql_info->id, 'r');
  if(!img) return;

  dt_image_t tmp = {0};
  dt_thumbtable_copy_image(&tmp, img);
  tmp.group_id = sql_info->group_id;
  tmp.group_members = group_members;
  tmp.history_items = history_items;

  dt_image_t cache_info = {0};
  dt_thumbtable_copy_image(&cache_info, &tmp);
  dt_image_cache_read_release(darktable.image_cache, img);

  dt_image_t sql_copy = *sql_info;
  g_assert_cmpint(sql_copy.id, ==, cache_info.id);
  g_assert_cmpint(sql_info->film_id, ==, cache_info.film_id);
  g_assert_cmpint(sql_info->group_id, ==, cache_info.group_id);
  g_assert_cmpint(sql_info->group_members, ==, cache_info.group_members);
  g_assert_cmpint(sql_info->history_items, ==, cache_info.history_items);
  g_assert_cmpint(sql_info->version, ==, cache_info.version);
  g_assert_cmpint(sql_info->width, ==, cache_info.width);
  g_assert_cmpint(sql_info->height, ==, cache_info.height);
  g_assert_cmpint(sql_info->orientation, ==, cache_info.orientation);
  g_assert_cmpint(sql_info->p_width, ==, cache_info.p_width);
  g_assert_cmpint(sql_info->p_height, ==, cache_info.p_height);
  g_assert_cmpint(sql_info->flags, ==, cache_info.flags);
  g_assert_cmpint(sql_info->loader, ==, cache_info.loader);
  g_assert_cmpint(sql_info->rating, ==, cache_info.rating);
  g_assert_cmpint(sql_info->color_labels, ==, cache_info.color_labels);
  g_assert(sql_info->has_localcopy == cache_info.has_localcopy);
  g_assert(sql_info->has_audio == cache_info.has_audio);
  g_assert(sql_info->is_bw == cache_info.is_bw);
  g_assert(sql_info->is_bw_flow == cache_info.is_bw_flow);
  g_assert(sql_info->is_hdr == cache_info.is_hdr);
  g_assert((int64_t)sql_info->import_timestamp == (int64_t)cache_info.import_timestamp);
  g_assert((int64_t)sql_info->change_timestamp == (int64_t)cache_info.change_timestamp);
  g_assert((int64_t)sql_info->export_timestamp == (int64_t)cache_info.export_timestamp);
  g_assert((int64_t)sql_info->print_timestamp == (int64_t)cache_info.print_timestamp);
  g_assert(_thumbtable_float_equal(sql_info->exif_exposure, cache_info.exif_exposure));
  g_assert(_thumbtable_float_equal(sql_info->exif_exposure_bias, cache_info.exif_exposure_bias));
  g_assert(_thumbtable_float_equal(sql_info->exif_aperture, cache_info.exif_aperture));
  g_assert(_thumbtable_float_equal(sql_info->exif_iso, cache_info.exif_iso));
  g_assert(_thumbtable_float_equal(sql_info->exif_focal_length, cache_info.exif_focal_length));
  g_assert(_thumbtable_float_equal(sql_info->exif_focus_distance, cache_info.exif_focus_distance));
  g_assert((int64_t)sql_info->exif_datetime_taken == (int64_t)cache_info.exif_datetime_taken);
  g_assert(_thumbtable_double_equal(sql_info->geoloc.latitude, cache_info.geoloc.latitude));
  g_assert(_thumbtable_double_equal(sql_info->geoloc.longitude, cache_info.geoloc.longitude));
  g_assert(_thumbtable_double_equal(sql_info->geoloc.elevation, cache_info.geoloc.elevation));
  g_assert_cmpstr(sql_copy.filename, ==, cache_info.filename);
  g_assert_cmpstr(sql_copy.fullpath, ==, cache_info.fullpath);
  g_assert_cmpstr(sql_copy.local_copy_path, ==, cache_info.local_copy_path);
  g_assert_cmpstr(sql_copy.local_copy_legacy_path, ==, cache_info.local_copy_legacy_path);
  g_assert_cmpstr(sql_copy.filmroll, ==, cache_info.filmroll);
  g_assert_cmpstr(sql_copy.folder, ==, cache_info.folder);
  g_assert_cmpstr(sql_copy.datetime, ==, cache_info.datetime);
  g_assert_cmpstr(sql_copy.camera_makermodel, ==, cache_info.camera_makermodel);
  g_assert_cmpstr(sql_copy.exif_maker, ==, cache_info.exif_maker);
  g_assert_cmpstr(sql_copy.exif_model, ==, cache_info.exif_model);
  g_assert_cmpstr(sql_copy.exif_lens, ==, cache_info.exif_lens);
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
