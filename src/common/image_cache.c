/*
    This file is part of darktable,
    Copyright (C) 2009-2012, 2014 johannes hanika.
    Copyright (C) 2010-2011 Henrik Andersson.
    Copyright (C) 2010-2014, 2016 Tobias Ellinghaus.
    Copyright (C) 2011 Robert Bieber.
    Copyright (C) 2011 Rostyslav Pidgornyi.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2013 Simon Spannagel.
    Copyright (C) 2013 Ulrich Pegelow.
    Copyright (C) 2014-2015 Pedro Côrte-Real.
    Copyright (C) 2014-2016 Roman Lebedev.
    Copyright (C) 2018 Edgardo Hoszowski.
    Copyright (C) 2019 Andreas Schneider.
    Copyright (C) 2019 August Schwerdfeger.
    Copyright (C) 2019 Bill Ferguson.
    Copyright (C) 2019-2020 Hanno Schwalm.
    Copyright (C) 2019-2022 Pascal Obry.
    Copyright (C) 2020 Heiko Bauke.
    Copyright (C) 2020 JP Verrue.
    Copyright (C) 2020, 2022 Philippe Weyland.
    Copyright (C) 2021 Aldric Renaudin.
    Copyright (C) 2021 Vincent THOMAS.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 paolodepetrillo.
    Copyright (C) 2022 Philipp Lutz.
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

#include "common/image_cache.h"
#include "common/colorlabels.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/exif.h"
#include "common/image.h"
#include "common/datetime.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "control/signal.h"
#include "develop/develop.h"

#include <sqlite3.h>
#include <inttypes.h>
#include <math.h>

static sqlite3_stmt *_image_cache_load_stmt = NULL;
static sqlite3_stmt *_image_cache_write_history_hash_stmt = NULL;
static dt_pthread_mutex_t _image_cache_stmt_mutex;
static gsize _image_cache_stmt_mutex_inited = 0;

static inline void _image_cache_stmt_mutex_ensure(void)
{
  if(g_once_init_enter(&_image_cache_stmt_mutex_inited))
  {
    dt_pthread_mutex_init(&_image_cache_stmt_mutex, NULL);
    g_once_init_leave(&_image_cache_stmt_mutex_inited, 1);
  }
}

static inline uint64_t _image_cache_self_hash(const dt_image_t *img)
{
  dt_image_t tmp = *img;

  // These should be constant with regard to self integrity checks
  // change_timestamp will be auto-updated if the hash changed,
  // so it's handled out of the scope of what we do here
  tmp.self_hash = 0;
  tmp.mipmap_hash = 0;
  tmp.change_timestamp = 0;
  tmp.print_timestamp = 0;
  tmp.import_timestamp = 0;
  tmp.export_timestamp = 0;

  return dt_hash(5381, (const char *)&tmp, sizeof(dt_image_t));
}

static inline void _image_cache_lock_init(dt_image_t *img)
{
  img->self_hash = _image_cache_self_hash(img);
}

static void _image_cache_write_history_hash(const dt_image_t *img)
{
  if(!img || img->id <= 0) return;

  _image_cache_stmt_mutex_ensure();
  dt_pthread_mutex_lock(&_image_cache_stmt_mutex);
  if(!_image_cache_write_history_hash_stmt)
  {
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(
        dt_database_get(darktable.db),
        "INSERT INTO main.history_hash (imgid, current_hash, basic_hash, auto_hash, mipmap_hash)"
        " VALUES (?1, ?2, NULL, NULL, ?3)"
        " ON CONFLICT (imgid)"
        " DO UPDATE SET current_hash = ?2, basic_hash = NULL, auto_hash = NULL, mipmap_hash = ?3",
        -1, &_image_cache_write_history_hash_stmt, NULL);
    // clang-format on
  }
  sqlite3_stmt *stmt = _image_cache_write_history_hash_stmt;
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, img->id);
  DT_DEBUG_SQLITE3_BIND_INT64(stmt, 2, (sqlite3_int64)img->history_hash);
  DT_DEBUG_SQLITE3_BIND_INT64(stmt, 3, (sqlite3_int64)img->mipmap_hash);
  sqlite3_step(stmt);
  dt_pthread_mutex_unlock(&_image_cache_stmt_mutex);
}

static sqlite3_stmt *_image_cache_get_stmt(void)
{
  if(!_image_cache_load_stmt)
  {
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(
        dt_database_get(darktable.db),
        "SELECT i.id, i.group_id, "
        "       (SELECT COUNT(id) FROM main.images WHERE group_id = i.group_id), "
        "       (SELECT COUNT(imgid) FROM main.history WHERE imgid = i.id), "
        "       COALESCE((SELECT current_hash FROM main.history_hash WHERE imgid = i.id), 0), "
        "       COALESCE((SELECT mipmap_hash FROM main.history_hash WHERE imgid = i.id), 0), "
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
        -1, &_image_cache_load_stmt, NULL);
    // clang-format on
  }

  sqlite3_reset(_image_cache_load_stmt);
  sqlite3_clear_bindings(_image_cache_load_stmt);
  return _image_cache_load_stmt;
}

void dt_image_from_stmt(dt_image_t *img, sqlite3_stmt *stmt)
{
  dt_image_init(img);

  img->id = sqlite3_column_int(stmt, 0);
  img->group_id = sqlite3_column_int(stmt, 1);
  img->group_members = (uint32_t)sqlite3_column_int(stmt, 2);
  img->history_items = (uint32_t)sqlite3_column_int(stmt, 3);
  img->history_hash = sqlite3_column_int64(stmt, 4);
  img->mipmap_hash = sqlite3_column_int64(stmt, 5);
  img->film_id = sqlite3_column_int(stmt, 6);
  img->version = sqlite3_column_int(stmt, 7);
  img->width = sqlite3_column_int(stmt, 8);
  img->height = sqlite3_column_int(stmt, 9);
  img->orientation = sqlite3_column_int(stmt, 10);
  img->p_width = 0;
  img->p_height = 0;
  img->flags = sqlite3_column_int(stmt, 11);
  img->loader = LOADER_UNKNOWN;
  img->import_timestamp = sqlite3_column_int64(stmt, 12);
  img->change_timestamp = sqlite3_column_int64(stmt, 13);
  img->export_timestamp = sqlite3_column_int64(stmt, 14);
  img->print_timestamp = sqlite3_column_int64(stmt, 15);
  img->exif_exposure = sqlite3_column_double(stmt, 16);
  if(sqlite3_column_type(stmt, 17) == SQLITE_FLOAT)
    img->exif_exposure_bias = sqlite3_column_double(stmt, 17);
  else
    img->exif_exposure_bias = NAN;
  img->exif_aperture = sqlite3_column_double(stmt, 18);
  img->exif_iso = sqlite3_column_double(stmt, 19);
  img->exif_focal_length = sqlite3_column_double(stmt, 20);
  img->exif_focus_distance = sqlite3_column_double(stmt, 21);
  img->exif_datetime_taken = sqlite3_column_int64(stmt, 22);
  if(sqlite3_column_type(stmt, 23) == SQLITE_FLOAT)
    img->geoloc.longitude = sqlite3_column_double(stmt, 23);
  else
    img->geoloc.longitude = NAN;
  if(sqlite3_column_type(stmt, 24) == SQLITE_FLOAT)
    img->geoloc.latitude = sqlite3_column_double(stmt, 24);
  else
    img->geoloc.latitude = NAN;
  if(sqlite3_column_type(stmt, 25) == SQLITE_FLOAT)
    img->geoloc.elevation = sqlite3_column_double(stmt, 25);
  else
    img->geoloc.elevation = NAN;

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

  img->color_labels = sqlite3_column_int(stmt, 32);

  img->exif_crop = sqlite3_column_double(stmt, 33);
  uint32_t tmp = sqlite3_column_int(stmt, 34);
  memcpy(&img->legacy_flip, &tmp, sizeof(dt_image_raw_parameters_t));
  const void *color_matrix = sqlite3_column_blob(stmt, 35);
  if(color_matrix)
    memcpy(img->d65_color_matrix, color_matrix, sizeof(img->d65_color_matrix));
  else
    img->d65_color_matrix[0] = NAN;
  img->colorspace = sqlite3_column_int(stmt, 36);
  img->raw_black_level = sqlite3_column_int(stmt, 37);
  img->raw_white_point = sqlite3_column_int(stmt, 38);

  if(img->fullpath[0])
    dt_image_local_copy_paths_from_fullpath(img->fullpath, img->id, img->local_copy_path,
                                            sizeof(img->local_copy_path), img->local_copy_legacy_path,
                                            sizeof(img->local_copy_legacy_path));

  if(img->exif_focus_distance >= 0 && img->orientation >= 0) img->exif_inited = 1;

  img->crop_x = img->crop_y = img->crop_width = img->crop_height = 0;

  for(uint8_t i = 0; i < 4; i++) 
    img->raw_black_level_separate[i] = 0;

  if(img->folder[0])
    g_strlcpy(img->filmroll, dt_image_film_roll_name(img->folder), sizeof(img->filmroll));
  
  dt_datetime_gtimespan_to_local(img->datetime, sizeof(img->datetime), img->exif_datetime_taken, FALSE, FALSE);

  // buffer size? colorspace?
  if(img->flags & DT_IMAGE_LDR)
  {
    img->buf_dsc.channels = 4;
    img->buf_dsc.datatype = TYPE_FLOAT;
    img->buf_dsc.cst = IOP_CS_RGB;
  }
  else if(img->flags & DT_IMAGE_HDR)
  {
    if(img->flags & DT_IMAGE_RAW)
    {
      img->buf_dsc.channels = 1;
      img->buf_dsc.datatype = TYPE_FLOAT;
      img->buf_dsc.cst = IOP_CS_RAW;
    }
    else
    {
      img->buf_dsc.channels = 4;
      img->buf_dsc.datatype = TYPE_FLOAT;
      img->buf_dsc.cst = IOP_CS_RGB;
    }
  }
  else
  {
    // raw
    img->buf_dsc.channels = 1;
    img->buf_dsc.datatype = TYPE_UINT16;
    img->buf_dsc.cst = IOP_CS_RAW;
  }

  img->has_localcopy = (img->flags & DT_IMAGE_LOCAL_COPY);
  img->has_audio = (img->flags & DT_IMAGE_HAS_WAV);
  img->rating = dt_image_get_xmp_rating_from_flags(img->flags);
  img->is_bw = dt_image_monochrome_flags(img);
  img->is_bw_flow = dt_image_use_monochrome_workflow(img);
  img->is_hdr = dt_image_is_hdr(img);

  dt_image_refresh_makermodel(img);
}

static void _image_cache_reload_from_db(dt_image_t *img, const uint32_t imgid)
{
  _image_cache_stmt_mutex_ensure();
  dt_pthread_mutex_lock(&_image_cache_stmt_mutex);

  sqlite3_stmt *stmt = _image_cache_get_stmt();
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_image_from_stmt(img, stmt);

    /* Deprecated:
    if(sqlite3_column_type(stmt, 37) == SQLITE_FLOAT)
      img->aspect_ratio = sqlite3_column_double(stmt, 37);
    else
      img->aspect_ratio = 0.0;
    */

    /* Deprecated:
    img->final_width = sqlite3_column_int(stmt, 33);
    img->final_height = sqlite3_column_int(stmt, 34);
    */
  }
  else
  {
    img->id = -1;
    fprintf(stderr, "[image_cache_reload] failed to open image %" PRIu32 " from database: %s\n", imgid,
            sqlite3_errmsg(dt_database_get(darktable.db)));
  }

  dt_pthread_mutex_unlock(&_image_cache_stmt_mutex);
}

void dt_image_cache_allocate(void *data, dt_cache_entry_t *entry)
{
  entry->cost = sizeof(dt_image_t);

  dt_image_t *img = (dt_image_t *)g_malloc(sizeof(dt_image_t));
  entry->data = img;
  dt_image_init(img);
  _image_cache_reload_from_db(img, entry->key);

  img->cache_entry = entry; // init backref
}

void dt_image_cache_deallocate(void *data, dt_cache_entry_t *entry)
{
  dt_image_t *img = (dt_image_t *)entry->data;
  g_free(img->profile);
  g_list_free_full(img->dng_gain_maps, g_free);
  g_free(img);
}

void dt_image_cache_init(dt_image_cache_t *cache)
{
  // the image cache does no serialization.
  // (unsafe. data should be in db/xmp, not in any other additional cache,
  // also, it should be relatively fast to get the image_t structs from sql.)
  // TODO: actually an independent conf var?
  //       too large: dangerous and wasteful?
  //       can we get away with a fixed size?
  const uint32_t size = 50;
  const uint32_t max_mem = size * 1024 * 1024;
  const uint32_t num = (uint32_t)(1.5f * max_mem / sizeof(dt_image_t));
  dt_cache_init(&cache->cache, sizeof(dt_image_t), max_mem);
  dt_cache_set_allocate_callback(&cache->cache, &dt_image_cache_allocate, cache);
  dt_cache_set_cleanup_callback(&cache->cache, &dt_image_cache_deallocate, cache);

  dt_print(DT_DEBUG_CACHE, "[image_cache] has %d entries (%u MiB)\n", num, size);
}

void dt_image_cache_cleanup(dt_image_cache_t *cache)
{
  if(_image_cache_load_stmt)
  {
    sqlite3_finalize(_image_cache_load_stmt);
    _image_cache_load_stmt = NULL;
  }
  if(_image_cache_write_history_hash_stmt)
  {
    sqlite3_finalize(_image_cache_write_history_hash_stmt);
    _image_cache_write_history_hash_stmt = NULL;
  }
  if(_image_cache_stmt_mutex_inited)
  {
    dt_pthread_mutex_destroy(&_image_cache_stmt_mutex);
    _image_cache_stmt_mutex_inited = 0;
  }

  dt_cache_cleanup(&cache->cache);
}

void dt_image_cache_print(dt_image_cache_t *cache)
{
  printf("[image cache] fill %.2f/%.2f MB (%.2f%%)\n", cache->cache.cost / (1024.0 * 1024.0),
         cache->cache.cost_quota / (1024.0 * 1024.0),
         (float)cache->cache.cost / (float)cache->cache.cost_quota);
}

dt_image_t *dt_image_cache_get(dt_image_cache_t *cache, const int32_t imgid, char mode)
{
  if(imgid <= 0) return NULL;
  dt_cache_entry_t *entry = dt_cache_get(&cache->cache, (uint32_t)imgid, mode);
  ASAN_UNPOISON_MEMORY_REGION(entry->data, sizeof(dt_image_t));
  dt_image_t *img = (dt_image_t *)entry->data;
  img->cache_entry = entry;

  if(dt_image_invalid(img))
  {
    dt_cache_release(&cache->cache, entry);
    return NULL;
  }

  _image_cache_lock_init(img);
  return img;
}

dt_image_t *dt_image_cache_testget(dt_image_cache_t *cache, const int32_t imgid, char mode)
{
  if(imgid <= 0) return NULL;
  dt_cache_entry_t *entry = dt_cache_testget(&cache->cache, (uint32_t)imgid, mode);
  if(!entry) return 0;
  ASAN_UNPOISON_MEMORY_REGION(entry->data, sizeof(dt_image_t));
  dt_image_t *img = (dt_image_t *)entry->data;
  img->cache_entry = entry;
  _image_cache_lock_init(img);
  return img;
}

// Always reload the cache entry from DB before returning it.
// This is critical for IMAGE_INFO_CHANGED: other handlers will read from the cache.
dt_image_t *dt_image_cache_get_reload(dt_image_cache_t *cache, const int32_t imgid, char mode)
{
  if(imgid <= 0) return NULL;

  // We must take a write lock to reload in-place, then demote to read if requested.
  dt_cache_entry_t *entry = dt_cache_get(&cache->cache, (uint32_t)imgid, 'w');
  ASAN_UNPOISON_MEMORY_REGION(entry->data, sizeof(dt_image_t));
  dt_image_t *img = (dt_image_t *)entry->data;
  _image_cache_reload_from_db(img, (uint32_t)imgid);
  img->cache_entry = entry;

  if(dt_image_invalid(img))
  {
    dt_cache_release(&cache->cache, entry);
    return NULL;
  }

  if(mode == 'r')
  {
    // demote the lock to read mode (see mipmap cache for rationale)
    entry->_lock_demoting = 1;
    dt_cache_release(&cache->cache, entry);
    entry = dt_cache_get(&cache->cache, (uint32_t)imgid, 'r');
    entry->_lock_demoting = 0;
    ASAN_UNPOISON_MEMORY_REGION(entry->data, sizeof(dt_image_t));
    img = (dt_image_t *)entry->data;
    img->cache_entry = entry;
  }

  _image_cache_lock_init(img);
  return img;
}

int dt_image_invalid(const dt_image_t *img)
{
  return (img == NULL || img->id <= 0);
}

int dt_image_cache_seed(dt_image_cache_t *cache, const dt_image_t *img)
{
  if(!cache || dt_image_invalid(img)) return -1;

  dt_image_t seeded = *img;

  // Avoid ownership issues for pointers that the cache cleanup would free.
  seeded.profile = NULL;
  seeded.profile_size = 0;
  seeded.dng_gain_maps = NULL;
  seeded.cache_entry = NULL;

  return dt_cache_seed(&cache->cache, (uint32_t)seeded.id, &seeded, sizeof(dt_image_t), sizeof(dt_image_t), FALSE);
}

// This callback must run before any other DT_SIGNAL_IMAGE_INFO_CHANGED handler.
// The signal notifies about DB changes, and most listeners read image info from the cache.
// We therefore force a DB reload here so every subsequent handler sees up-to-date data.
static void _image_cache_info_changed_reload_callback(gpointer instance, gpointer imgs, gpointer user_data)
{
  for(GList *l = g_list_first((GList *)imgs); l; l = g_list_next(l))
  {
    const int32_t imgid = GPOINTER_TO_INT(l->data);
    if(imgid <= 0) continue;

    dt_image_t *img = dt_image_cache_get_reload(darktable.image_cache, imgid, 'r');
    if(img)
      dt_image_cache_read_release(darktable.image_cache, img);
  }
}

void dt_image_cache_connect_info_changed_first(const struct dt_control_signal_t *ctlsig)
{
  // Must be connected early to run before any other handler.
  dt_control_signal_connect(ctlsig, DT_SIGNAL_IMAGE_INFO_CHANGED,
                            G_CALLBACK(_image_cache_info_changed_reload_callback), NULL);
}

// drops the read lock on an image struct
void dt_image_cache_read_release(dt_image_cache_t *cache, const dt_image_t *img)
{
  if(!img || img->id <= 0) return;
  const uint64_t self_hash = _image_cache_self_hash(img);
  if(self_hash != img->self_hash)
    g_error("[image_cache] read lock modified image %d, you need to use a write lock\n", img->id);

    // just force the dt_image_t struct to make sure it has been locked before.
  dt_cache_release(&cache->cache, img->cache_entry);
}

// drops the write privileges on an image struct.
// this triggers a write-through to sql, and optionally queues xmp sidecar writing.
void dt_image_cache_write_release(dt_image_cache_t *cache, dt_image_t *img, dt_image_cache_write_mode_t mode)
{
  union {
      struct dt_image_raw_parameters_t s;
      uint32_t u;
  } flip;
  if(img->id <= 0) return;

  const uint64_t self_hash = _image_cache_self_hash(img);
  const gboolean changed = (self_hash != img->self_hash);

  if(changed)
    img->change_timestamp = dt_datetime_now_to_gtimespan();

  // even if nothing changed, we might need to write export/print timestamps 
  // and mipmap hash, so we can't exit just yet.

  if(mode == DT_IMAGE_CACHE_MINIMAL)
  {
    if(changed)
      g_error("[image_cache] minimal write release modified image %d, you need to commit those changes to DB.\n", img->id);
    
    dt_cache_release(&cache->cache, img->cache_entry);
    return;
  }

  // Recompute full/local copy paths (and derived folder/filmroll/datetime) from possibly updated filename.
  // Avoid SQL here; rely on the cached folder/fullpath, or leave fields empty if they can't be rebuilt.
  char folder[PATH_MAX] = { 0 };
  if(img->folder[0])
  {
    g_strlcpy(folder, img->folder, sizeof(folder));
  }
  else if(img->fullpath[0])
  {
    gchar *dir = g_path_get_dirname(img->fullpath);
    if(dir && dir[0] && strcmp(dir, "."))
      g_strlcpy(folder, dir, sizeof(folder));
    g_free(dir);
  }

  if(img->filename[0] && folder[0])
  {
    g_snprintf(img->fullpath, sizeof(img->fullpath), "%s" G_DIR_SEPARATOR_S "%s", folder, img->filename);
    g_strlcpy(img->folder, folder, sizeof(img->folder));
  }
  else
  {
    img->fullpath[0] = '\0';
    img->folder[0] = '\0';
  }
  if(img->folder[0])
    g_strlcpy(img->filmroll, dt_image_film_roll_name(img->folder), sizeof(img->filmroll));
  else if(img->film_id < 0)
    g_strlcpy(img->filmroll, _("orphaned image"), sizeof(img->filmroll));
  else
    img->filmroll[0] = '\0';
  dt_datetime_gtimespan_to_local(img->datetime, sizeof(img->datetime), img->exif_datetime_taken, FALSE, FALSE);
  dt_image_local_copy_paths_from_fullpath(img->fullpath, img->id, img->local_copy_path,
                                          sizeof(img->local_copy_path), img->local_copy_legacy_path,
                                          sizeof(img->local_copy_legacy_path));

  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
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
  _image_cache_write_history_hash(img);

  const int32_t imgid = img->id;
  dt_cache_release(&cache->cache, img->cache_entry);

  if(mode == DT_IMAGE_CACHE_SAFE && dt_image_get_xmp_mode())
    dt_control_save_xmp(imgid);
}


// remove the image from the cache
void dt_image_cache_remove(dt_image_cache_t *cache, const int32_t imgid)
{
  dt_cache_remove(&cache->cache, imgid);
}

void dt_image_cache_set_export_timestamp(dt_image_cache_t *cache, const int32_t imgid)
{
  if(imgid <= 0) return;
  dt_image_t *img = dt_image_cache_get(cache, imgid, 'w');
  if(!img) return;
  img->export_timestamp = dt_datetime_now_to_gtimespan();
  dt_image_cache_write_release(cache, img, DT_IMAGE_CACHE_SAFE);
}

void dt_image_cache_set_print_timestamp(dt_image_cache_t *cache, const int32_t imgid)
{
  if(imgid <= 0) return;
  dt_image_t *img = dt_image_cache_get(cache, imgid, 'w');
  if(!img) return;
  img->print_timestamp = dt_datetime_now_to_gtimespan();
  dt_image_cache_write_release(cache, img, DT_IMAGE_CACHE_SAFE);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
