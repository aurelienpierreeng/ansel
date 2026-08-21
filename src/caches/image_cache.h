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

#ifndef DT_CACHES_IMAGE_CACHE_H
#define DT_CACHES_IMAGE_CACHE_H

#include "caches/cache.h"
#include "common/image.h"

#include <sqlite3.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque, and there is no accessor: no function below takes a cache handle, so nothing
 * outside this module needs one. */
typedef struct dt_image_cache_t dt_image_cache_t;

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

/**
 * @brief Initialise the cache.
 *
 * @param verbose whether this cache traces to the log. Read ONCE, here, from the session's
 *        debug flags by the orchestrator -- the cache does not consult them itself, so it
 *        neither depends on the debug machinery at runtime nor changes behaviour halfway
 *        through a session. `-d cache` still works: darktable.c turns it into this argument.
 */
void dt_image_cache_init(const gboolean verbose);
void dt_image_cache_cleanup(void);
void dt_image_cache_print(void);

// Interim accessor (Strategy B, doc/globals-migration.md): implemented by the orchestrator; long-term the handle should be carried on the job/view context (Strategy C).
/** @brief Has the image cache been initialised? Callers that run before dt_image_cache_init()
 * or after its cleanup -- early startup, late teardown -- ask this instead of testing a handle
 * they should not have. */
gboolean dt_image_cache_is_ready(void);

// One cached image (dt_image_t), for the GUI memory view.
typedef struct dt_image_cache_stats_entry_t
{
  int32_t imgid;
  size_t size;        // bytes
  char filename[128];
} dt_image_cache_stats_entry_t;

// Current/max bytes used by the image cache.
void dt_image_cache_get_usage(size_t *current, size_t *max);

// Snapshot of all cached images (newly-allocated GArray of
// dt_image_cache_stats_entry_t; free with g_array_free()).
GArray *dt_image_cache_get_entries_stats(void);

// blocks until it gets the image struct with this id for reading.
// also does the sql query if the image is not in cache atm.
// if id < 0, a newly wiped image struct shall be returned (for import).
// this will silently start the garbage collector and free long-unused
// cachelines to free up space if necessary.
// if an entry is swapped out like this in the background, this is the latest
// point where sql and xmp can be synched (unsafe setting).
dt_image_t *dt_image_cache_get(const int32_t imgid, char mode);

// same as read_get, but doesn't block and returns NULL if the image
// is currently unavailable.
dt_image_t *dt_image_cache_testget(const int32_t imgid, char mode);

// like dt_image_cache_get/testget, but always reloads the image data from the database
// before returning the cache entry.
dt_image_t *dt_image_cache_get_reload(const int32_t imgid, char mode);

// seed an image cache entry from an already-populated dt_image_t (no SQL).
// returns 0 on insert, 1 if already present, -1 on failure.
int dt_image_cache_seed(const dt_image_t *img);

// Populate the common dt_image_t subset from a SQL row (shared with thumbtable).
// Expected column order:
// id, group_id, group_members, history_items, history_hash, mipmap_hash, film_id, version, width, height, orientation, flags,
// import_timestamp, change_timestamp, export_timestamp, print_timestamp, exposure, exposure_bias, aperture, iso,
// focal_length, focus_distance, datetime_taken, longitude, latitude, altitude, filename, fullpath, maker, model,
// lens, folder, color_labels, crop, raw_parameters, color_matrix, colorspace, raw_black, raw_maximum,
// aspect_ratio, output_width, output_height.
//
// IMPORTANT: this does not call dt_image_init(). Fields not present in the SQL row are left unchanged.
/**
 * @brief Compute the fields the database does not store: rating, monochrome and HDR
 * predicates, makermodel, and the extension cross-check that logs stale stored flags.
 *
 * @details Call this on any ::dt_image_t freshly filled from a database row --
 * dt_image_repository_load() does it for its own callers, and gui/dtgtk/thumbtable.c must do
 * it after dt_image_from_stmt() on each row of its bulk query. Skipping it leaves `rating`,
 * `is_bw`, `is_hdr` and `exif_makermodel` stale from whatever the struct held before.
 *
 * @param img image to complete, in place. NULL is a no-op.
 */
void dt_image_derive_fields(dt_image_t *img);

struct dt_control_signal_t;
// Register an IMAGE_INFO_CHANGED handler that force-reloads image cache entries.
// This must be connected before any other handler, so everyone observes fresh data.
void dt_image_cache_connect_info_changed_first(const struct dt_control_signal_t *ctlsig);

// drops the read lock on an image struct
void dt_image_cache_read_release(const dt_image_t *img);

// drops the write privileges on an image struct.
// this triggers a write-through to sql, and if the setting
// is present, also to xmp sidecar files (safe setting).
// minimal mode only releases the lock without any write.
void dt_image_cache_write_release(dt_image_t *img, dt_image_cache_write_mode_t mode);

// remove the image from the cache
void dt_image_cache_remove(const int32_t imgid);

// register timestamps in cache
void dt_image_cache_set_export_timestamp(const int32_t imgid);
void dt_image_cache_set_print_timestamp(const int32_t imgid);

// return 1 if the image is invalid so we can bail out early
int dt_image_invalid(const dt_image_t *img);

#ifdef __cplusplus
}
#endif

#endif // DT_CACHES_IMAGE_CACHE_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
