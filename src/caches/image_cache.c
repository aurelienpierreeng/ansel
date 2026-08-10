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

#include "database/image_repository.h"
#include "caches/image_cache.h"
#include "system/macros.h"
#include "system/mem_alloc.h"
#include "common/hash.h"
#include "common/logging.h"
#include "common/paths.h"
#include "common/image.h"
#include "imageio/imageio_core.h"
#include "common/datetime.h"
#include "control/control.h"
#include "control/signal.h"
#include "develop/supervisor.h"

#include <inttypes.h>
#include "colorprofiles/colorspaces.h"   // dt_colorspaces_free_image_profile

/* Set once by dt_image_cache_init(); see the header for why it is not read live. */
static gboolean _verbose = FALSE;

/* Statement-safe: a bare `if(_verbose) dt_print(...)` swallows a following `else`. */
#define _cache_print(channel, ...)                    \
  do {                                                \
    if(_verbose) dt_print((channel), __VA_ARGS__);     \
  } while(0)


/* PRIVATE. Nothing outside this file reads a field of it -- the header exposes the
 * type as an opaque handle so that it cannot. */
typedef struct dt_image_cache_t
{
  dt_cache_t cache;
}
dt_image_cache_t;


/* The cache instance, owned HERE. It used to be calloc'd by darktable.c and hung off the
 * application struct, so the accessor lived in the orchestrator and the struct was reachable
 * from anything that included darktable.h. */
static dt_image_cache_t *_image_cache = NULL;

gboolean dt_image_cache_is_ready(void)
{
  return !IS_NULL_PTR(_image_cache);
}



static inline uint64_t _image_cache_self_hash(const dt_image_t *img)
{
  if(IS_NULL_PTR(img) || img->id <= 0) return 0;

  struct
  {
    dt_image_orientation_t orientation;
    float exif_exposure;
    float exif_exposure_bias;
    float exif_aperture;
    float exif_iso;
    float exif_focal_length;
    float exif_focus_distance;
    float exif_crop;
    char exif_maker[64];
    char exif_model[64];
    char exif_lens[128];
    GTimeSpan exif_datetime_taken;
    char filename[DT_MAX_FILENAME_LEN];
    int32_t width, height;
    int32_t crop_x, crop_y, crop_width, crop_height;
    int32_t flags, film_id, id, group_id, version;
    uint64_t history_hash;
    float d65_color_matrix[9];
    dt_image_colorspace_t colorspace;
    dt_image_raw_parameters_t legacy_flip;
    dt_image_geoloc_t geoloc;
    uint16_t raw_black_level;
    uint32_t raw_white_point;
    int color_labels;
  } persisted = { 0 };

  /*
   * Track only the dt_image_t fields that are explicitly written back to the database
   * from dt_image_cache_write_release(), plus the history hash and color labels that
   * are flushed through their own SQL paths right below it. Runtime-only fields such
   * as cached ICC pointers, DNG gain-map lists, loader state or derived path strings
   * must stay out of this hash, otherwise merely opening an image can look like an
   * edit and spuriously bump change_timestamp.
   */
  persisted.orientation = img->orientation;
  persisted.exif_exposure = img->exif_exposure;
  persisted.exif_exposure_bias = img->exif_exposure_bias;
  persisted.exif_aperture = img->exif_aperture;
  persisted.exif_iso = img->exif_iso;
  persisted.exif_focal_length = img->exif_focal_length;
  persisted.exif_focus_distance = img->exif_focus_distance;
  persisted.exif_crop = img->exif_crop;
  memcpy(persisted.exif_maker, img->exif_maker, sizeof(persisted.exif_maker));
  memcpy(persisted.exif_model, img->exif_model, sizeof(persisted.exif_model));
  memcpy(persisted.exif_lens, img->exif_lens, sizeof(persisted.exif_lens));
  persisted.exif_datetime_taken = img->exif_datetime_taken;
  memcpy(persisted.filename, img->filename, sizeof(persisted.filename));
  persisted.width = img->width;
  persisted.height = img->height;
  persisted.crop_x = img->crop_x;
  persisted.crop_y = img->crop_y;
  persisted.crop_width = img->crop_width;
  persisted.crop_height = img->crop_height;
  persisted.flags = img->flags;
  persisted.film_id = img->film_id;
  persisted.id = img->id;
  persisted.group_id = img->group_id;
  persisted.version = img->version;
  persisted.history_hash = img->history_hash;
  memcpy(persisted.d65_color_matrix, img->d65_color_matrix, sizeof(persisted.d65_color_matrix));
  persisted.colorspace = img->colorspace;
  persisted.legacy_flip = img->legacy_flip;
  persisted.geoloc = img->geoloc;
  persisted.raw_black_level = img->raw_black_level;
  persisted.raw_white_point = img->raw_white_point;
  persisted.color_labels = img->color_labels;

  return dt_hash(5381, (const char *)&persisted, sizeof(persisted));
}

static inline void _image_cache_lock_init(dt_image_t *img)
{
  img->self_hash = _image_cache_self_hash(img);
}




/* Fields the database does not store because they are computed from the ones it does:
 * the rating, the monochrome and HDR predicates, the extension cross-check, makermodel.
 * They live here rather than in database/image_repository.c because they need
 * imageio/ and views/ symbols, and a row mapper reaching two layers up for them is exactly
 * what kept this code in common/. Every path that produces a dt_image_t from a database row
 * calls this next -- the repository's own load, and gui/dtgtk/thumbtable.c's bulk query. */
void dt_image_derive_fields(dt_image_t *img)
{
  if(IS_NULL_PTR(img)) return;
  img->has_localcopy = (img->flags & DT_IMAGE_LOCAL_COPY);
  img->has_audio = (img->flags & DT_IMAGE_HAS_WAV);
  int xmp_rating = dt_image_get_xmp_rating_from_flags(img->flags);
  img->rating = (xmp_rating == -1) ? DT_VIEW_REJECT : xmp_rating;
  img->is_bw = dt_image_monochrome_flags(img);
  img->is_bw_flow = dt_image_use_monochrome_workflow(img);
  img->is_hdr = dt_image_is_hdr(img);

  // Instrumentation: the LDR/HDR flags are now pure flag predicates (no filename sniffing at read
  // time). Historically the flag could have been mis-set at import, so cross-check the flags loaded
  // from the DB against the unambiguous extension hint and log any contradiction. Containers whose
  // dynamic range only the decoder can settle (TIFF/AVIF/HEIF/DNG ...) return 0 here and are skipped
  // to avoid false positives. This surfaces stale/garbage flags persisted in the database.
  {
    const gchar *ext = g_strrstr(img->filename, ".");
    const dt_image_flags_t ext_hint = ext ? dt_imageio_get_type_from_extension(ext) : 0;
    const gboolean db_hdr = (img->flags & DT_IMAGE_HDR) != 0;
    const gboolean db_ldr = (img->flags & DT_IMAGE_LDR) != 0;

    if(ext_hint == DT_IMAGE_HDR && !db_hdr)
      _cache_print(DT_DEBUG_IMAGEIO,
               "[image_cache] DB flag mismatch: id=%d filename='%s' has an HDR extension but stored "
               "flags=0x%08x lack DT_IMAGE_HDR (ldr=%d hdr=%d)\n",
               img->id, img->filename, (unsigned int)img->flags, db_ldr, db_hdr);
    else if((ext_hint == DT_IMAGE_LDR || ext_hint == DT_IMAGE_RAW) && db_hdr)
      _cache_print(DT_DEBUG_IMAGEIO,
               "[image_cache] DB flag mismatch: id=%d filename='%s' has a %s extension but stored "
               "flags=0x%08x carry DT_IMAGE_HDR (ldr=%d hdr=%d)\n",
               img->id, img->filename, (ext_hint == DT_IMAGE_RAW) ? "RAW" : "LDR",
               (unsigned int)img->flags, db_ldr, db_hdr);
  }

  dt_image_refresh_makermodel(img);
}

static void _image_cache_reload_from_db(dt_image_t *img, const uint32_t imgid, const dt_sv_op_t sv_op)
{
  dt_image_repository_load((int32_t)imgid, img);
  dt_image_derive_fields(img);

  // The image's dt_image_t was just (re)loaded from the database: `create` on
  // first allocation, `update` on a reload (get_reload / IMAGE_INFO_CHANGED).
  // Notifying is the cache's job, not the repository's -- the repository does not
  // know what a supervisor is.
  if(dt_supervisor_active()) dt_supervisor_image(sv_op, (int32_t)imgid, img);
}

void dt_image_cache_allocate(void *data, dt_cache_entry_t *entry)
{
  entry->cost = sizeof(dt_image_t);

  dt_image_t *img = (dt_image_t *)g_malloc(sizeof(dt_image_t));
  entry->data = img;
  dt_image_init(img);
  _image_cache_reload_from_db(img, entry->key, DT_SV_CREATE); // emits the create event

  img->cache_entry = entry; // init backref
}

void dt_image_cache_deallocate(void *data, dt_cache_entry_t *entry)
{
  dt_image_t *img = (dt_image_t *)entry->data;

  if(dt_supervisor_active()) dt_supervisor_image(DT_SV_DELETE, (int32_t)entry->key, NULL);

  dt_free(img->profile);
  dt_colorspaces_free_image_profile(img->embedded_profile);
  img->embedded_profile = NULL;
  g_list_free_full(img->dng_gain_maps, dt_free_gpointer);
  img->dng_gain_maps = NULL;
  dt_free(img);
}

void dt_image_cache_init(const gboolean verbose)
{
  _verbose = verbose;
  if(_image_cache) return;
  _image_cache = (dt_image_cache_t *)calloc(1, sizeof(dt_image_cache_t));
  if(IS_NULL_PTR(_image_cache)) return;
  dt_image_cache_t *cache = _image_cache;
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

  _cache_print(DT_DEBUG_CACHE, "[image_cache] has %d entries (%u MiB)\n", num, size);
}

void dt_image_cache_cleanup(void)
{
  dt_image_cache_t *cache = _image_cache;
  if(IS_NULL_PTR(cache)) return;
  dt_image_repository_cleanup();

  dt_cache_cleanup(&cache->cache);

  dt_free(_image_cache);
  _image_cache = NULL;
}

void dt_image_cache_print(void)
{
  const dt_image_cache_t *const cache = _image_cache;
  printf("[image cache] fill %.2f/%.2f MB (%.2f%%)\n", cache->cache.cost / (1024.0 * 1024.0),
         cache->cache.cost_quota / (1024.0 * 1024.0),
         (float)cache->cache.cost / (float)cache->cache.cost_quota);
}

void dt_image_cache_get_usage(size_t *current, size_t *max)
{
  dt_image_cache_t *cache = _image_cache;
  if(current) *current = 0;
  if(max) *max = 0;
  if(IS_NULL_PTR(cache)) return;
  dt_pthread_mutex_lock(&cache->cache.lock);
  if(current) *current = cache->cache.cost;
  if(max) *max = cache->cache.cost_quota;
  dt_pthread_mutex_unlock(&cache->cache.lock);
}

GArray *dt_image_cache_get_entries_stats(void)
{
  dt_image_cache_t *cache = _image_cache;
  GArray *out = g_array_new(FALSE, FALSE, sizeof(dt_image_cache_stats_entry_t));
  if(IS_NULL_PTR(cache)) return out;

  dt_pthread_mutex_lock(&cache->cache.lock);
  GHashTableIter it;
  gpointer key, value;
  g_hash_table_iter_init(&it, cache->cache.hashtable);
  while(g_hash_table_iter_next(&it, &key, &value))
  {
    const dt_cache_entry_t *ce = (const dt_cache_entry_t *)value;
    if(IS_NULL_PTR(ce)) continue;
    dt_image_cache_stats_entry_t s = { 0 };
    s.imgid = (int32_t)ce->key;
    s.size = ce->cost ? ce->cost : ce->data_size;
    const dt_image_t *img = (const dt_image_t *)ce->data;
    if(img && img->filename[0]) g_strlcpy(s.filename, img->filename, sizeof(s.filename));
    g_array_append_val(out, s);
  }
  dt_pthread_mutex_unlock(&cache->cache.lock);
  return out;
}

dt_image_t *dt_image_cache_get(const int32_t imgid, char mode)
{
  dt_image_cache_t *cache = _image_cache;
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

dt_image_t *dt_image_cache_testget(const int32_t imgid, char mode)
{
  dt_image_cache_t *cache = _image_cache;
  if(imgid <= 0) return NULL;
  dt_cache_entry_t *entry = dt_cache_testget(&cache->cache, (uint32_t)imgid, mode);
  if(IS_NULL_PTR(entry)) return 0;
  ASAN_UNPOISON_MEMORY_REGION(entry->data, sizeof(dt_image_t));
  dt_image_t *img = (dt_image_t *)entry->data;
  img->cache_entry = entry;
  _image_cache_lock_init(img);
  return img;
}

// Always reload the cache entry from DB before returning it.
// This is critical for IMAGE_INFO_CHANGED: other handlers will read from the cache.
dt_image_t *dt_image_cache_get_reload(const int32_t imgid, char mode)
{
  dt_image_cache_t *cache = _image_cache;
  if(imgid <= 0) return NULL;

  // We must take a write lock to reload in-place, then demote to read if requested.
  dt_cache_entry_t *entry = dt_cache_get(&cache->cache, (uint32_t)imgid, 'w');
  ASAN_UNPOISON_MEMORY_REGION(entry->data, sizeof(dt_image_t));
  dt_image_t *img = (dt_image_t *)entry->data;
  _image_cache_reload_from_db(img, (uint32_t)imgid, DT_SV_UPDATE);

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
  return (IS_NULL_PTR(img) || img->id <= 0);
}

int dt_image_cache_seed(const dt_image_t *img)
{
  dt_image_cache_t *cache = _image_cache;
  if(IS_NULL_PTR(cache) || dt_image_invalid(img)) return -1;

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

    dt_image_t *img = dt_image_cache_get_reload(imgid, 'r');
    if(img)
      dt_image_cache_read_release(img);
  }
}

void dt_image_cache_connect_info_changed_first(const struct dt_control_signal_t *ctlsig)
{
  // Must be connected early to run before any other handler.
  dt_control_signal_connect(ctlsig, DT_SIGNAL_IMAGE_INFO_CHANGED,
                            G_CALLBACK(_image_cache_info_changed_reload_callback), NULL);
}

// drops the read lock on an image struct
void dt_image_cache_read_release(const dt_image_t *img)
{
  dt_image_cache_t *cache = _image_cache;
  if(IS_NULL_PTR(img) || img->id <= 0) return;
  const uint64_t self_hash = _image_cache_self_hash(img);
  if(self_hash != img->self_hash)
    g_error("[image_cache] read lock modified image %d, you need to use a write lock\n", img->id);

    // just force the dt_image_t struct to make sure it has been locked before.
  dt_cache_release(&cache->cache, img->cache_entry);
}

// drops the write privileges on an image struct.
// this triggers a write-through to sql, and optionally queues xmp sidecar writing.
void dt_image_cache_write_release(dt_image_t *img, dt_image_cache_write_mode_t mode)
{
  dt_image_cache_t *cache = _image_cache;
  if(IS_NULL_PTR(img) || img->id <= 0) return;

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
    dt_free(dir);
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

  img->has_localcopy = (img->flags & DT_IMAGE_LOCAL_COPY);
  img->has_audio = (img->flags & DT_IMAGE_HAS_WAV);
  int xmp_rating = dt_image_get_xmp_rating_from_flags(img->flags);
  img->rating = (xmp_rating == -1) ? DT_VIEW_REJECT : xmp_rating;
  img->is_bw = dt_image_monochrome_flags(img);
  img->is_bw_flow = dt_image_use_monochrome_workflow(img);
  img->is_hdr = dt_image_is_hdr(img);

  dt_image_repository_store(img);

  const int32_t imgid = img->id;
  dt_cache_release(&cache->cache, img->cache_entry);

  if(mode == DT_IMAGE_CACHE_SAFE && dt_image_get_xmp_mode())
    dt_control_save_xmp(imgid);
  
  // FIXME: that a memory leak ?
  GList *imgs = NULL;
  imgs = g_list_prepend(imgs, GINT_TO_POINTER(img->id));
  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_IMAGE_INFO_CHANGED, imgs);
}


// remove the image from the cache
void dt_image_cache_remove(const int32_t imgid)
{
  dt_image_cache_t *cache = _image_cache;
  dt_cache_remove(&cache->cache, imgid);
}

void dt_image_cache_set_export_timestamp(const int32_t imgid)
{
  if(imgid <= 0) return;
  dt_image_t *img = dt_image_cache_get(imgid, 'w');
  if(IS_NULL_PTR(img)) return;
  img->export_timestamp = dt_datetime_now_to_gtimespan();
  dt_image_cache_write_release(img, DT_IMAGE_CACHE_SAFE);
}

void dt_image_cache_set_print_timestamp(const int32_t imgid)
{
  if(imgid <= 0) return;
  dt_image_t *img = dt_image_cache_get(imgid, 'w');
  if(IS_NULL_PTR(img)) return;
  img->print_timestamp = dt_datetime_now_to_gtimespan();
  dt_image_cache_write_release(img, DT_IMAGE_CACHE_SAFE);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
