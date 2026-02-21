/*
    This file is part of darktable,
    Copyright (C) 2009-2011, 2014 johannes hanika.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012, 2014, 2016 Tobias Ellinghaus.
    Copyright (C) 2013 Simon Spannagel.
    Copyright (C) 2020 Hanno Schwalm.
    Copyright (C) 2020 JP Verrue.
    Copyright (C) 2020-2021 Pascal Obry.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2025 Alynx Zhou.
    
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

#pragma once

#include "common/cache.h"
#include "common/image.h"

#include <sqlite3.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dt_image_cache_t
{
  dt_cache_t cache;
}
dt_image_cache_t;

// what to do if an image struct is
// released after writing.
typedef enum dt_image_cache_write_mode_t
{
  // write to db and queue xmp write
  DT_IMAGE_CACHE_SAFE = 0,
  // only write to db
  DT_IMAGE_CACHE_RELAXED = 1,
  // only release the lock (no db write, no xmp)
  // use that for multi-threading data safety
  DT_IMAGE_CACHE_MINIMAL = 2
}
dt_image_cache_write_mode_t;

void dt_image_cache_init(dt_image_cache_t *cache);
void dt_image_cache_cleanup(dt_image_cache_t *cache);
void dt_image_cache_print(dt_image_cache_t *cache);

// blocks until it gets the image struct with this id for reading.
// also does the sql query if the image is not in cache atm.
// if id < 0, a newly wiped image struct shall be returned (for import).
// this will silently start the garbage collector and free long-unused
// cachelines to free up space if necessary.
// if an entry is swapped out like this in the background, this is the latest
// point where sql and xmp can be synched (unsafe setting).
dt_image_t *dt_image_cache_get(dt_image_cache_t *cache, const int32_t imgid, char mode);

// same as read_get, but doesn't block and returns NULL if the image
// is currently unavailable.
dt_image_t *dt_image_cache_testget(dt_image_cache_t *cache, const int32_t imgid, char mode);

// like dt_image_cache_get/testget, but always reloads the image data from the database
// before returning the cache entry.
dt_image_t *dt_image_cache_get_reload(dt_image_cache_t *cache, const int32_t imgid, char mode);

// seed an image cache entry from an already-populated dt_image_t (no SQL).
// returns 0 on insert, 1 if already present, -1 on failure.
int dt_image_cache_seed(dt_image_cache_t *cache, const dt_image_t *img);

// Populate the common dt_image_t subset from a SQL row (shared with thumbtable).
// Expected column order:
// id, group_id, group_members, history_items, history_hash, mipmap_hash, film_id, version, width, height, orientation, flags,
// import_timestamp, change_timestamp, export_timestamp, print_timestamp, exposure, exposure_bias, aperture, iso,
// focal_length, focus_distance, datetime_taken, longitude, latitude, altitude, filename, fullpath, maker, model,
// lens, folder, color_labels, crop, raw_parameters, color_matrix, colorspace, raw_black, raw_maximum,
// aspect_ratio, output_width, output_height.
//
// IMPORTANT: this does not call dt_image_init(). Fields not present in the SQL row are left unchanged.
void dt_image_from_stmt(dt_image_t *info, sqlite3_stmt *stmt);

struct dt_control_signal_t;
// Register an IMAGE_INFO_CHANGED handler that force-reloads image cache entries.
// This must be connected before any other handler, so everyone observes fresh data.
void dt_image_cache_connect_info_changed_first(const struct dt_control_signal_t *ctlsig);

// drops the read lock on an image struct
void dt_image_cache_read_release(dt_image_cache_t *cache, const dt_image_t *img);

// drops the write privileges on an image struct.
// this triggers a write-through to sql, and if the setting
// is present, also to xmp sidecar files (safe setting).
// minimal mode only releases the lock without any write.
void dt_image_cache_write_release(dt_image_cache_t *cache, dt_image_t *img, dt_image_cache_write_mode_t mode);

// remove the image from the cache
void dt_image_cache_remove(dt_image_cache_t *cache, const int32_t imgid);

// register timestamps in cache
void dt_image_cache_set_export_timestamp(dt_image_cache_t *cache, const int32_t imgid);
void dt_image_cache_set_print_timestamp(dt_image_cache_t *cache, const int32_t imgid);

// return 1 if the image is invalid so we can bail out early
int dt_image_invalid(const dt_image_t *img);

#ifdef __cplusplus
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
