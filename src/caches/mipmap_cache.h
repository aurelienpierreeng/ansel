/*
    This file is part of darktable,
    Copyright (C) 2009-2014, 2016 johannes hanika.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012, 2014-2016 Tobias Ellinghaus.
    Copyright (C) 2014-2015 Pedro Côrte-Real.
    Copyright (C) 2014-2016 Roman Lebedev.
    Copyright (C) 2019, 2021 Aldric Renaudin.
    Copyright (C) 2020-2021 Pascal Obry.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2025 Alynx Zhou.
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

#ifndef DT_CACHES_MIPMAP_CACHE_H
#define DT_CACHES_MIPMAP_CACHE_H

#include "system/atomic.h"
#include "common/paths.h"   // DT_PATH_MAX
#include "caches/cache.h"
#include "colorprofiles/profile_types.h"
#include "common/image.h"

#ifdef __cplusplus
extern "C" {
#endif

// sizes stored in the mipmap cache, set to fixed values in mipmap_cache.c
typedef enum dt_mipmap_size_t {
  DT_MIPMAP_0,               // 360x225 px
  DT_MIPMAP_1,               // 720x450 px
  DT_MIPMAP_2,               // 1440x900 px
  DT_MIPMAP_3,               // Full HD 1080p
  DT_MIPMAP_4,               // 2560x1440 px
  DT_MIPMAP_5,               // 4K/UHD -
  DT_MIPMAP_6,               // 5K
  DT_MIPMAP_7,               // 6K
  DT_MIPMAP_8,               // 8K
  DT_MIPMAP_F,               // unprocessed input float image downscaled to 720x450 or 1440x900 px for performance
  DT_MIPMAP_FULL,            // unprocessed input float image at original resolation
  DT_MIPMAP_NONE
} dt_mipmap_size_t;

// type to be passed to getter functions
typedef enum dt_mipmap_get_flags_t
{
  // only return when the requested buffer is loaded.
  // blocks until that happens.
  DT_MIPMAP_BLOCKING = 0,
  // don't actually acquire the lock if it is not
  // in cache (i.e. would have to be loaded first)
  DT_MIPMAP_TESTLOCK = 1
} dt_mipmap_get_flags_t;

// struct to be alloc'ed by the client, filled by dt_mipmap_cache_get()
typedef struct dt_mipmap_buffer_t
{
  dt_mipmap_size_t size;
  int32_t imgid;
  int32_t width, height;
  float iscale;
  uint8_t *buf;
  dt_colorspaces_color_profile_type_t color_space;
  dt_cache_entry_t *cache_entry;
} dt_mipmap_buffer_t;

typedef struct dt_mipmap_cache_one_t dt_mipmap_cache_one_t;

/* The cache instance is a file-static in mipmap_cache.c and there is no accessor for it: no
 * function below takes one, so nothing outside needs the handle. The type survives only
 * because dt_mipmap_cache_one_t names it; both are opaque. */
typedef struct dt_mipmap_cache_t dt_mipmap_cache_t;

// dynamic memory allocation interface for imageio backend: a write locked
// mipmap buffer is passed in, it might already contain a valid buffer. this
// function takes care of re-allocating, if necessary.
void *dt_mipmap_cache_alloc(dt_mipmap_buffer_t *buf, const dt_image_t *img);

/**
 * @brief Everything about the mipmap cache that the USER decides.
 *
 * @details These four were read from conf at the point of use, so the cache depended on the
 * configuration system and its behaviour could change under it mid-decode. They cross the
 * boundary as one value now: the application reads conf, the cache is told. That also makes
 * the lifecycle visible -- set once at startup, set again when the user changes a preference,
 * and never anywhere else.
 */
typedef struct dt_mipmap_cache_settings_t
{
  /** @brief RAM budget for the thumbnail LRU, in bytes. A soft quota, not a hard limit. */
  size_t max_memory;
  /** @brief Write generated thumbnails to the on-disk cache (`cache_disk_backend`). */
  gboolean disk_backend;
  /** @brief Whether to prefer the embedded JPEG over decoding (`lighttable/embedded_jpg`). */
  int embedded_jpg;
  /** @brief JPEG quality for thumbnails written to disk (`database_cache_quality`). */
  int cache_quality;
} dt_mipmap_cache_settings_t;

/**
 * @brief Apply new settings to a running cache.
 *
 * @details Every field takes effect immediately, including ::max_memory -- the LRU quota is
 * soft, so lowering it makes the next insertions evict harder rather than freeing anything
 * synchronously. Call it whenever the user changes one of the four; the application does that
 * from its DT_SIGNAL_PREFERENCES_CHANGE handler.
 *
 * @param settings the new values. NULL is a no-op.
 */
void dt_mipmap_cache_set_settings(const dt_mipmap_cache_settings_t *settings);

/**
 * @brief Read back the settings in force. Snapshot, taken under the same lock the setter
 * takes, so the four fields are always consistent with each other.
 */
void dt_mipmap_cache_get_settings(dt_mipmap_cache_settings_t *settings);

/**
 * @brief Initialise the cache.
 *
 * @param verbose whether this cache traces to the log. Read ONCE, here, from the session's
 *        debug flags by the orchestrator -- the cache does not consult them itself, so it
 *        neither depends on the debug machinery at runtime nor changes behaviour halfway
 *        through a session. `-d cache` still works: darktable.c turns it into this argument.
 * @param settings the user's choices; see ::dt_mipmap_cache_settings_t. NULL uses zeroes,
 *        which is only sensible in a test.
 */
void dt_mipmap_cache_init(const dt_mipmap_cache_settings_t *settings, const gboolean verbose);
void dt_mipmap_cache_cleanup(void);
void dt_mipmap_cache_print(void);

// Interim accessor (Strategy B, doc/globals-migration.md): implemented by the orchestrator; long-term the handle should be carried on the job/view context (Strategy C).

// One cached mipmap buffer, for the GUI memory view.
typedef struct dt_mipmap_cache_stats_entry_t
{
  int32_t imgid;
  int mip;       // dt_mipmap_size_t
  size_t size;   // bytes
} dt_mipmap_cache_stats_entry_t;

// Current/max bytes used across all mipmap sub-caches.
void dt_mipmap_cache_get_usage(size_t *current, size_t *max);

// Snapshot of all cached buffers (newly-allocated GArray of
// dt_mipmap_cache_stats_entry_t; free with g_array_free()).
GArray *dt_mipmap_cache_get_entries_stats(void);

// get a buffer and lock according to mode ('r' or 'w').
// see dt_mipmap_get_flags_t for explanation of the exact
// behaviour. pass 0 as flags for the default (best effort)
#define dt_mipmap_cache_get(B,C,D,E,F) dt_mipmap_cache_get_with_caller(B,C,D,E,F,__FILE__,__LINE__)
void dt_mipmap_cache_get_with_caller(
    dt_mipmap_buffer_t *buf,
    const int32_t imgid,
    const dt_mipmap_size_t mip,
    const dt_mipmap_get_flags_t flags,
    const char mode,
    const char *file,
    int line);

#define dt_mipmap_cache_get_with_shutdown(B,C,D,E,F,G) \
  dt_mipmap_cache_get_with_caller_and_shutdown(B,C,D,E,F,G,__FILE__,__LINE__)
void dt_mipmap_cache_get_with_caller_and_shutdown(
    dt_mipmap_buffer_t *buf,
    const int32_t imgid,
    const dt_mipmap_size_t mip,
    const dt_mipmap_get_flags_t flags,
    const char mode,
    dt_atomic_int *shutdown,
    const char *file,
    int line);

// convenience function with fewer params
#define dt_mipmap_cache_write_get(B,C,D) dt_mipmap_cache_write_get_with_caller(B,C,D,__FILE__,__LINE__)
void dt_mipmap_cache_write_get_with_caller(
    dt_mipmap_buffer_t *buf,
    const int32_t imgid,
    const int mip,
    const char *file,
    int line);

// drop a lock
#define dt_mipmap_cache_release(B) dt_mipmap_cache_release_with_caller(B, __FILE__, __LINE__)
void dt_mipmap_cache_release_with_caller(dt_mipmap_buffer_t *buf, const char *file,
                                         int line);

// remove thumbnails, so they will be regenerated:
void dt_mipmap_cache_remove(const int32_t imgid, const gboolean flush_disk);
void dt_mipmap_cache_remove_at_size(const int32_t imgid, const dt_mipmap_size_t mip, const gboolean flush_disk);

// evict thumbnails from cache. They will be written to disc if not existing
void dt_mimap_cache_evict(const int32_t imgid);

// return the closest mipmap size
// for the given window you wish to draw.
// a dt_mipmap_size_t has always a fixed resolution associated with it,
// depending on the user parameter for the maximum thumbnail dimensions.
// actual resolution depends on the image and is only known after
// the thumbnail is loaded.
dt_mipmap_size_t dt_mipmap_cache_get_matching_size( const int32_t width, const int32_t height, const uint32_t imgid);

// return the closest mipmap size fitting within the width × height boundary box.
// Use that to flush a darkroom pipeline output into a cache line
dt_mipmap_size_t dt_mipmap_cache_get_fitting_size(const int32_t width,
                                                   const int32_t height, const uint32_t imgid);

// Manually swap the image buffer of a mipmap cacheline from an existing uint8_t image
void dt_mipmap_cache_swap_at_size(const int32_t imgid, 
                                  const dt_mipmap_size_t mip, const uint8_t *const buffer, 
                                  const int32_t width, const int32_t height, dt_colorspaces_color_profile_type_t profile);

// copy over thumbnails. used by file operation that copies raw files, to speed up thumbnail generation.
// only copies over the jpg backend on disk, doesn't directly affect the in-memory cache.
void dt_mipmap_cache_copy_thumbnails(const uint32_t dst_imgid, const uint32_t src_imgid);

// get the full path of a cached thumbnail
void dt_mipmap_get_cache_filename(char path[DT_PATH_MAX], dt_mipmap_size_t mip, const int32_t imgid);

// get just the dir
void dt_mipmap_get_cache_dir(char path[DT_PATH_MAX], dt_mipmap_size_t mip);


#ifdef __cplusplus
}
#endif

#endif // DT_CACHES_MIPMAP_CACHE_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
