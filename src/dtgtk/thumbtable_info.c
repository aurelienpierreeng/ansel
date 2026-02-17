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
#include "views/view.h"

#include <glib.h>
#include <glib/gi18n.h>
#include <math.h>
#include <string.h>

static sqlite3_stmt *_thumbtable_collection_stmt = NULL;

static void _thumbtable_info_finalize_expensive(dt_thumbnail_image_info_t *info)
{
  if(!info) return;

  if(!info->camera[0])
  {
    const char *maker = info->exif_maker;
    const char *model = info->exif_model;
    char mk[64] = { 0 };
    char md[64] = { 0 };
    char al[64] = { 0 };

    if((info->exif_maker[0] || info->exif_model[0])
       && dt_imageio_lookup_makermodel(info->exif_maker, info->exif_model,
                                       mk, sizeof(mk), md, sizeof(md), al, sizeof(al)))
    {
      if(mk[0]) maker = mk;
      if(md[0]) model = md;
    }

    g_strlcpy(info->camera, maker, sizeof(info->camera));
    const size_t len = strlen(maker);
    if(len < sizeof(info->camera) - 1)
      info->camera[len] = ' ';
    g_strlcpy(info->camera + len + 1, model, sizeof(info->camera) - len - 1);
  }

  if(!info->datetime[0])
    dt_datetime_gtimespan_to_local(info->datetime, sizeof(info->datetime), info->exif_datetime_taken, FALSE, FALSE);

  if(!info->filmroll[0])
  {
    if(info->film_id < 0 || !info->folder[0])
      g_strlcpy(info->filmroll, _("orphaned image"), sizeof(info->filmroll));
    else
      g_strlcpy(info->filmroll, dt_image_film_roll_name(info->folder), sizeof(info->filmroll));
  }

  if(!info->fullpath[0] && info->folder[0] && info->filename[0])
    g_snprintf(info->fullpath, sizeof(info->fullpath), "%s" G_DIR_SEPARATOR_S "%s", info->folder, info->filename);
}

void dt_thumbtable_info_finalize(dt_thumbnail_image_info_t *info, gboolean expensive)
{
  if(!info) return;

  // Minimal copy of dt_image_t struct to use typical functions
  dt_image_t tmp = {0};
  tmp.flags = info->flags;
  g_strlcpy(tmp.filename, info->filename, sizeof(tmp.filename));

  info->has_localcopy = (info->flags & DT_IMAGE_LOCAL_COPY);
  info->has_audio = (info->flags & DT_IMAGE_HAS_WAV);
  info->rating = (info->flags & DT_IMAGE_REJECTED) ? DT_VIEW_REJECT : (info->flags & DT_VIEW_RATINGS_MASK);
  info->is_bw = dt_image_monochrome_flags(&tmp);
  info->is_bw_flow = dt_image_use_monochrome_workflow(&tmp);
  info->is_hdr = dt_image_is_hdr(&tmp);

  if(expensive)
    _thumbtable_info_finalize_expensive(info);
}

void dt_thumbtable_info_from_image(dt_thumbnail_image_info_t *info, const dt_image_t *img)
{
  if(!info || !img) return;

  memset(info, 0, sizeof(*info));

  info->imgid = img->id;
  info->film_id = img->film_id;
  info->groupid = img->group_id;
  info->group_members = img->group_members;
  info->history_items = img->history_items;
  info->version = img->version;
  info->width = img->width;
  info->height = img->height;
  info->orientation = img->orientation;
  info->p_width = img->p_width;
  info->p_height = img->p_height;
  info->flags = img->flags;
  info->loader = img->loader;
  info->import_timestamp = img->import_timestamp;
  info->change_timestamp = img->change_timestamp;
  info->export_timestamp = img->export_timestamp;
  info->print_timestamp = img->print_timestamp;
  info->exif_exposure = img->exif_exposure;
  info->exif_exposure_bias = img->exif_exposure_bias;
  info->exif_aperture = img->exif_aperture;
  info->exif_iso = img->exif_iso;
  info->exif_focal_length = img->exif_focal_length;
  info->exif_focus_distance = img->exif_focus_distance;
  info->exif_datetime_taken = img->exif_datetime_taken;
  info->geoloc_latitude = img->geoloc.latitude;
  info->geoloc_longitude = img->geoloc.longitude;
  info->geoloc_elevation = img->geoloc.elevation;
  g_strlcpy(info->filename, img->filename, sizeof(info->filename));
  g_strlcpy(info->exif_maker, img->exif_maker, sizeof(info->exif_maker));
  g_strlcpy(info->exif_model, img->exif_model, sizeof(info->exif_model));
  g_strlcpy(info->exif_lens, img->exif_lens, sizeof(info->exif_lens));
  g_strlcpy(info->camera, img->camera_makermodel, sizeof(info->camera));
  dt_image_film_roll_directory(img, info->folder, sizeof(info->folder));
  info->colorlabels = img->color_labels;

  dt_thumbtable_info_finalize(info, FALSE);
}

void dt_thumbtable_info_from_stmt(dt_thumbnail_image_info_t *info, sqlite3_stmt *stmt,
                                  uint32_t history_items, uint32_t group_members)
{
  if(!info || !stmt) return;

  memset(info, 0, sizeof(*info));

  info->imgid = sqlite3_column_int(stmt, 0);
  info->film_id = sqlite3_column_int(stmt, 5);
  info->groupid = sqlite3_column_int(stmt, 1);
  info->group_members = group_members;
  info->history_items = history_items;
  info->version = sqlite3_column_int(stmt, 6);
  info->width = sqlite3_column_int(stmt, 7);
  info->height = sqlite3_column_int(stmt, 8);
  info->orientation = sqlite3_column_int(stmt, 9);
  info->p_width = 0;
  info->p_height = 0;
  info->flags = sqlite3_column_int(stmt, 10);
  info->loader = LOADER_UNKNOWN;
  info->import_timestamp = sqlite3_column_int64(stmt, 11);
  info->change_timestamp = sqlite3_column_int64(stmt, 12);
  info->export_timestamp = sqlite3_column_int64(stmt, 13);
  info->print_timestamp = sqlite3_column_int64(stmt, 14);
  info->exif_exposure = sqlite3_column_double(stmt, 15);
  if(sqlite3_column_type(stmt, 16) == SQLITE_FLOAT)
    info->exif_exposure_bias = sqlite3_column_double(stmt, 16);
  else
    info->exif_exposure_bias = NAN;
  info->exif_aperture = sqlite3_column_double(stmt, 17);
  info->exif_iso = sqlite3_column_double(stmt, 18);
  info->exif_focal_length = sqlite3_column_double(stmt, 19);
  info->exif_focus_distance = sqlite3_column_double(stmt, 20);
  info->exif_datetime_taken = sqlite3_column_int64(stmt, 21);
  if(sqlite3_column_type(stmt, 22) == SQLITE_FLOAT)
    info->geoloc_longitude = sqlite3_column_double(stmt, 22);
  else
    info->geoloc_longitude = NAN;
  if(sqlite3_column_type(stmt, 23) == SQLITE_FLOAT)
    info->geoloc_latitude = sqlite3_column_double(stmt, 23);
  else
    info->geoloc_latitude = NAN;
  if(sqlite3_column_type(stmt, 24) == SQLITE_FLOAT)
    info->geoloc_elevation = sqlite3_column_double(stmt, 24);
  else
    info->geoloc_elevation = NAN;

  const char *filename = (const char *)sqlite3_column_text(stmt, 25);
  if(filename) g_strlcpy(info->filename, filename, sizeof(info->filename));
  const char *maker = (const char *)sqlite3_column_text(stmt, 26);
  if(maker) g_strlcpy(info->exif_maker, maker, sizeof(info->exif_maker));
  const char *model = (const char *)sqlite3_column_text(stmt, 27);
  if(model) g_strlcpy(info->exif_model, model, sizeof(info->exif_model));
  const char *lens = (const char *)sqlite3_column_text(stmt, 28);
  if(lens) g_strlcpy(info->exif_lens, lens, sizeof(info->exif_lens));
  const char *folder = (const char *)sqlite3_column_text(stmt, 29);
  if(folder) g_strlcpy(info->folder, folder, sizeof(info->folder));

  info->colorlabels = sqlite3_column_int(stmt, 30);

  dt_thumbtable_info_finalize(info, FALSE);
}

sqlite3_stmt *dt_thumbtable_info_get_collection_stmt(void)
{
  if(!_thumbtable_collection_stmt)
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(
        dt_database_get(darktable.db),
        // Batch-fetch thumbnail metadata in one SQL query to avoid one query per image
        // through the image cache. This keeps scrolling lightweight and predictable.
        "SELECT im.id, im.group_id, c.rowid, "
        "(SELECT COUNT(id) FROM main.images WHERE group_id=im.group_id), "
        "(SELECT COUNT(imgid) FROM main.history WHERE imgid=c.imgid), "
        "im.film_id, im.version, im.width, im.height, im.orientation, "
        "im.flags, "
        "im.import_timestamp, im.change_timestamp, im.export_timestamp, im.print_timestamp, "
        "im.exposure, im.exposure_bias, im.aperture, im.iso, im.focal_length, im.focus_distance, "
        "im.datetime_taken, "
        "im.longitude, im.latitude, im.altitude, "
        "im.filename, im.maker, im.model, im.lens, fr.folder, "
        "COALESCE((SELECT SUM(1 << color) FROM main.color_labels WHERE imgid=im.id), 0) "
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

void dt_thumbtable_info_debug_assert_matches_cache(const dt_thumbnail_image_info_t *sql_info,
                                                   uint32_t history_items, uint32_t group_members)
{
  if(!sql_info || sql_info->imgid <= 0) return;

  const dt_image_t *img = dt_image_cache_get(darktable.image_cache, sql_info->imgid, 'r');
  if(!img) return;

  dt_image_t tmp = *img;
  tmp.group_id = sql_info->groupid;
  tmp.group_members = group_members;
  tmp.history_items = history_items;

  dt_thumbnail_image_info_t cache_info = {0};
  dt_thumbtable_info_from_image(&cache_info, &tmp);
  dt_thumbtable_info_finalize(&cache_info, TRUE);
  dt_image_cache_read_release(darktable.image_cache, img);

  dt_thumbnail_image_info_t sql_copy = *sql_info;
  dt_thumbtable_info_finalize(&sql_copy, TRUE);

  g_assert_cmpint(sql_copy.imgid, ==, cache_info.imgid);
  g_assert_cmpint(sql_info->film_id, ==, cache_info.film_id);
  g_assert_cmpint(sql_info->groupid, ==, cache_info.groupid);
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
  g_assert_cmpint(sql_info->colorlabels, ==, cache_info.colorlabels);
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
  g_assert(_thumbtable_double_equal(sql_info->geoloc_latitude, cache_info.geoloc_latitude));
  g_assert(_thumbtable_double_equal(sql_info->geoloc_longitude, cache_info.geoloc_longitude));
  g_assert(_thumbtable_double_equal(sql_info->geoloc_elevation, cache_info.geoloc_elevation));
  g_assert_cmpstr(sql_copy.filename, ==, cache_info.filename);
  g_assert_cmpstr(sql_copy.fullpath, ==, cache_info.fullpath);
  g_assert_cmpstr(sql_copy.filmroll, ==, cache_info.filmroll);
  g_assert_cmpstr(sql_copy.folder, ==, cache_info.folder);
  g_assert_cmpstr(sql_copy.datetime, ==, cache_info.datetime);
  g_assert_cmpstr(sql_copy.camera, ==, cache_info.camera);
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
