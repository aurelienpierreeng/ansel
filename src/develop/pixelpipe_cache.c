/*
    This file is part of Ansel
    Copyright (C) 2025 - Aurélien PIERRE

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Ansel is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "control/control.h"
#include "develop/pixelpipe_cache.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "develop/format.h"
#include "develop/pixelpipe_hb.h"
#include <inttypes.h>
#include <glib.h>
#include <stdlib.h>


typedef struct dt_pixel_cache_entry_t
{
  uint64_t hash;            // unique identifier of the entry
  void *data;               // buffer holding pixels... or anything else
  size_t size;              // size of the data buffer
  dt_iop_buffer_dsc_t dsc;  // metadata of the data buffer
  int64_t age;              // timestamp of creation. Oldest entry will be the first freed if it's not locked
  char *name;               // name of the cache entry, for debugging
  int id;                   // id of the pipeline owning this entry. Used when flushing, a pipe can only flush its own.
  dt_atomic_int refcount;   // reference count for the cache entry, to avoid freeing it while still in use
  dt_pthread_rwlock_t lock; // read/write lock to avoid threads conflicts
  gboolean auto_destroy;    // TRUE for auto-destruction the next time it's used. Used for short-lived entries (transient states).
} dt_pixel_cache_entry_t;


void _non_thread_safe_cache_ref_count_entry(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash, gboolean lock,
                                            dt_pixel_cache_entry_t *cache_entry);

dt_pixel_cache_entry_t *_non_threadsafe_cache_get_entry(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash)
{
  return (dt_pixel_cache_entry_t *)g_hash_table_lookup(cache->entries, GINT_TO_POINTER(hash));
}


dt_pixel_cache_entry_t *dt_dev_pixelpipe_cache_get_entry(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash)
{
  dt_pthread_mutex_lock(&cache->lock);
  dt_pixel_cache_entry_t *entry = _non_threadsafe_cache_get_entry(cache, hash);
  dt_pthread_mutex_unlock(&cache->lock);
  return entry;
}


size_t dt_pixel_cache_get_size(dt_pixel_cache_entry_t *cache_entry)
{
  return cache_entry->size / (1024 * 1024);
}


void dt_pixel_cache_message(dt_pixel_cache_entry_t *cache_entry, const char *message, gboolean verbose)
{
  if(!(darktable.unmuted & DT_DEBUG_CACHE)) return;
  if(verbose && !(darktable.unmuted & DT_DEBUG_VERBOSE)) return;
  dt_print(DT_DEBUG_CACHE, "[pixelpipe] cache entry %" PRIu64 ": %s (%lu MiB - age %" PRId64 ") %s\n", cache_entry->hash,
           cache_entry->name, dt_pixel_cache_get_size(cache_entry), cache_entry->age, message);
}


// remove the cache entry with the given hash and update the cache memory usage
// WARNING: not internally thread-safe, protect its calls with mutex lock
// return 0 on success, 1 on error
int _non_thread_safe_cache_remove(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash, const gboolean force,
                                  dt_pixel_cache_entry_t *cache_entry)
{
  if(cache_entry == NULL)
    cache_entry = _non_threadsafe_cache_get_entry(cache, hash);

  if(cache_entry)
  {
    // Returns 1 if the lock is captured by another thread
    // 0 if WE capture the lock, and then need to release it
    gboolean locked = dt_pthread_rwlock_trywrlock(&cache_entry->lock);
    if(!locked) dt_pthread_rwlock_unlock(&cache_entry->lock);
    gboolean used = dt_atomic_get_int(&cache_entry->refcount) > 0;

    if((!used || force) && !locked)
    {
      cache->current_memory -= cache_entry->size;
      g_hash_table_remove(cache->entries, GINT_TO_POINTER(hash));
      return 0;
    }
    else if(used)
      dt_pixel_cache_message(cache_entry, "cannot remove: used", TRUE);
    else if(locked)
      dt_pixel_cache_message(cache_entry, "cannot remove: locked", TRUE);
  }
  else
  {
    dt_print(DT_DEBUG_PIPE, "[pixelpipe] cache entry %" PRIu64 " not found, will not be removed\n", hash);
  }
  return 1;
}


int dt_dev_pixelpipe_cache_remove(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash, const gboolean force,
                                  dt_pixel_cache_entry_t *cache_entry)
{
  dt_pthread_mutex_lock(&cache->lock);
  int error = _non_thread_safe_cache_remove(cache, hash, force, cache_entry);
  dt_pthread_mutex_unlock(&cache->lock);
  return error;
}

typedef struct _cache_lru_t
{
  int64_t max_age;
  uint64_t hash;
  dt_pixel_cache_entry_t *cache_entry;
} _cache_lru_t;


// find the cache entry hash with the oldest use
void _cache_get_oldest(gpointer key, gpointer value, gpointer user_data)
{
  dt_pixel_cache_entry_t *cache_entry = (dt_pixel_cache_entry_t *)value;
  _cache_lru_t *lru = (_cache_lru_t *)user_data;

  // Don't remove LRU entries that are still in use
  // NOTE: with all the killswitches mechanisms and safety measures,
  // we might have more things decreasing refcount than increasing it.
  // It's no big deal though, as long as the (final output) backbuf
  // is checked for NULL and not reused if pipeline is DIRTY.
  if(cache_entry->age < lru->max_age)
  {
    // Returns 1 if the lock is captured by another thread
    // 0 if WE capture the lock, and then need to release it
    gboolean locked = dt_pthread_rwlock_trywrlock(&cache_entry->lock);
    if(!locked) dt_pthread_rwlock_unlock(&cache_entry->lock);
    gboolean used = dt_atomic_get_int(&cache_entry->refcount) > 0;

    if(!locked && !used)
    {
      lru->max_age = cache_entry->age;
      lru->hash = cache_entry->hash;
      lru->cache_entry = cache_entry;
      dt_pixel_cache_message(cache_entry, "candidate for deletion", TRUE);
    }
    else if(used)
      dt_pixel_cache_message(cache_entry, "cannot be deleted: used", TRUE);
    else if(locked)
      dt_pixel_cache_message(cache_entry, "cannot be deleted: locked", TRUE);
  }
}


// remove the least used cache entry
// return 0 on success, 1 on error
// error is : we couldn't find a candidate for deletion because all entries are either locked or in use
// or we found one but failed to remove it.
static int _non_thread_safe_pixel_pipe_cache_remove_lru(dt_dev_pixelpipe_cache_t *cache)
{
  _cache_lru_t *lru = (_cache_lru_t *)malloc(sizeof(_cache_lru_t));
  lru->max_age = g_get_monotonic_time();
  lru->hash = 0;
  lru->cache_entry = NULL;
  int error = 1;
  g_hash_table_foreach(cache->entries, _cache_get_oldest, lru);

  if(lru->hash > 0)
    error = _non_thread_safe_cache_remove(cache, lru->hash, FALSE, lru->cache_entry);
  else
    dt_print(DT_DEBUG_PIPE, "[pixelpipe] couldn't remove LRU, %i items and all are used\n", g_hash_table_size(cache->entries));

  free(lru);
  return error;
}

// return 0 on success 1 on error
int dt_dev_pixel_pipe_cache_remove_lru(dt_dev_pixelpipe_cache_t *cache)
{
  dt_pthread_mutex_lock(&cache->lock);
  int error = _non_thread_safe_pixel_pipe_cache_remove_lru(cache);
  dt_pthread_mutex_unlock(&cache->lock);
  return error;
}

// WARNING: not thread-safe, protect its calls with mutex lock
static dt_pixel_cache_entry_t *dt_pixel_cache_new_entry(const uint64_t hash, const size_t size,
                                                        const dt_iop_buffer_dsc_t dsc, const char *name, const int id,
                                                        dt_dev_pixelpipe_cache_t *cache)
{
  // Dynamically update the max cache size depending on remaining free memory on system
  const size_t remaining_mem = dt_get_available_mem();
  const size_t safety_margin = 4 * dt_get_singlebuffer_mem();
  if(remaining_mem < safety_margin)
  {
    cache->max_memory
        = MIN(MAX((int64_t)remaining_mem - (int64_t)safety_margin, (int64_t)2 * dt_get_singlebuffer_mem()),
              remaining_mem);
    //fprintf(stdout, "new pipeline cache size : %lu MiB\n", cache->max_memory / (1024 * 1024));
    if(cache->max_memory == 2 * dt_get_singlebuffer_mem())
      dt_control_log(_("Your system RAM is nearly saturated.\n"
                       "Processing full-resolution images may not "
                       "possible anymore.\n"));
  }

  // Free up space if needed to match the max memory limit
  // If error, all entries are currently locked or in use, so we cannot free space to allocate a new entry.
  int error = 0;
  while(cache->current_memory + size > cache->max_memory && g_hash_table_size(cache->entries) > 0 && !error)
    error = _non_thread_safe_pixel_pipe_cache_remove_lru(cache);

  if(cache->current_memory + size > cache->max_memory)
  {
    dt_print(DT_DEBUG_PIPE, "[pixelpipe] cache is full, cannot allocate new entry %" PRIu64 " (%s)\n", hash, name);
    return NULL; // not enough memory
  }

  dt_pixel_cache_entry_t *cache_entry = (dt_pixel_cache_entry_t *)malloc(sizeof(dt_pixel_cache_entry_t));
  if(!cache_entry) return NULL;

  // allocate the data buffer
  cache_entry->data = dt_alloc_align(size);

  // if allocation failed, remove the least recently used cache entry, then try again
  while(cache_entry->data == NULL && g_hash_table_size(cache->entries) > 0)
  {
    _non_thread_safe_pixel_pipe_cache_remove_lru(cache);
    cache_entry->data = dt_alloc_align(size);
  }

  if(!cache_entry->data)
  {
    free(cache_entry);
    return NULL;
  }

  cache_entry->size = size;
  cache_entry->age = 0;
  cache_entry->dsc = dsc;
  cache_entry->hash = hash;
  cache_entry->id = id;
  cache_entry->name = g_strdup(name);
  cache_entry->refcount = 0;
  cache_entry->auto_destroy = FALSE;
  dt_pthread_rwlock_init(&cache_entry->lock, NULL);

  g_hash_table_insert(cache->entries, GINT_TO_POINTER(hash), cache_entry);
  cache->current_memory += size;

  return cache_entry;
}


static void _free_cache_entry(dt_pixel_cache_entry_t *cache_entry)
{
  if(!cache_entry) return;
  dt_pixel_cache_message(cache_entry, "freed", FALSE);
  dt_free_align(cache_entry->data);
  cache_entry->data = NULL;
  dt_pthread_rwlock_destroy(&cache_entry->lock);
  g_free(cache_entry->name);
  free(cache_entry);
}


dt_dev_pixelpipe_cache_t * dt_dev_pixelpipe_cache_init(size_t max_memory)
{
  dt_dev_pixelpipe_cache_t *cache = (dt_dev_pixelpipe_cache_t *)malloc(sizeof(dt_dev_pixelpipe_cache_t));
  dt_pthread_mutex_init(&cache->lock, NULL);
  cache->entries = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)_free_cache_entry);
  cache->max_memory = max_memory;
  cache->current_memory = 0;
  cache->queries = cache->hits = 0;
  return cache;
}


void dt_dev_pixelpipe_cache_cleanup(dt_dev_pixelpipe_cache_t *cache)
{
  if(!cache) return;
  dt_pthread_mutex_destroy(&cache->lock);
  g_hash_table_destroy(cache->entries);
  cache->entries = NULL;
}


int dt_dev_pixelpipe_cache_get(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash,
                               const size_t size, const char *name, const int id,
                               void **data, dt_iop_buffer_dsc_t **dsc,
                               dt_pixel_cache_entry_t **entry)
{
  // Search or create cache entry (under cache lock)
  dt_pthread_mutex_lock(&cache->lock);
  cache->queries++;

  dt_pixel_cache_entry_t *cache_entry = _non_threadsafe_cache_get_entry(cache, hash);
  const gboolean cache_entry_found = (cache_entry != NULL);

  if(cache_entry_found)
  {
    // Existing entry: increment hit counter
    cache->hits++;

    // Increase ref_count, consumer will have to decrease it
    _non_thread_safe_cache_ref_count_entry(darktable.pixelpipe_cache, hash, TRUE, cache_entry);
    
    // Release cache lock BEFORE acquiring entry locks to avoid deadlock
    dt_pthread_mutex_unlock(&cache->lock);

    // Acquire read lock (wait for any write to complete)
    dt_dev_pixelpipe_cache_rdlock_entry(cache, hash, TRUE, cache_entry);
    dt_dev_pixelpipe_cache_rdlock_entry(cache, hash, FALSE, cache_entry);
  }
  else
  {
    // New entry: create and allocate
    cache_entry = dt_pixel_cache_new_entry(hash, size, **dsc, name, id, cache);
    
    if(!cache_entry)
    {
      dt_print(DT_DEBUG_PIPE, "couldn't allocate new cache entry %" PRIu64 "\n", hash);
      dt_pthread_mutex_unlock(&cache->lock);
      if(entry) *entry = NULL;
      return 1;
    }

    // Increase ref_count, consumer will have to decrease it
    _non_thread_safe_cache_ref_count_entry(darktable.pixelpipe_cache, hash, TRUE, cache_entry);
    
    // Acquire write lock so caller can populate data safely
    dt_dev_pixelpipe_cache_wrlock_entry(cache, hash, TRUE, cache_entry);

    // Release cache lock AFTER acquiring entry locks to prevent other threads to capture it in-between
    dt_pthread_mutex_unlock(&cache->lock);

    dt_print(DT_DEBUG_CACHE,"[pixelpipe_cache] Write-lock on entry (new cache entry %" PRIu64 " for %s pipeline)\n", hash, name);
  }

  // Finalize and return
  cache_entry->age = g_get_monotonic_time(); // Update MRU timestamp
  *data = cache_entry->data;
  *dsc = &cache_entry->dsc;
  dt_pixel_cache_message(cache_entry, cache_entry_found ? "found" : "created", FALSE);

  if(entry) *entry = cache_entry;
  return !cache_entry_found;
}

int dt_dev_pixelpipe_cache_get_existing(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash,
                                        void **data, dt_iop_buffer_dsc_t **dsc, dt_pixel_cache_entry_t **entry)
{
  // Find the cache entry for this hash, if any
  dt_pthread_mutex_lock(&cache->lock);
  cache->queries++;
  dt_pixel_cache_entry_t *cache_entry = _non_threadsafe_cache_get_entry(cache, hash);

  if(cache_entry)
  {
    cache->hits++;
    // Increment refcount while we still hold cache->lock
    _non_thread_safe_cache_ref_count_entry(darktable.pixelpipe_cache, hash, TRUE, cache_entry);
    
    // IMPORTANT: Release cache->lock BEFORE trying to acquire entry locks to avoid deadlock
    dt_pthread_mutex_unlock(&cache->lock);

    // Block and wait for write events to finish, aka try to take a read lock
    dt_dev_pixelpipe_cache_rdlock_entry(cache, hash, TRUE, cache_entry);
    dt_dev_pixelpipe_cache_rdlock_entry(cache, hash, FALSE, cache_entry);

    // Set the time after we get the lock
    cache_entry->age = g_get_monotonic_time(); // this is the MRU entry
    *data = cache_entry->data;
    *dsc = &cache_entry->dsc;
    dt_pixel_cache_message(cache_entry, "found", FALSE);
  }
  else
  {
    dt_pthread_mutex_unlock(&cache->lock);
  }

  if(entry) *entry = cache_entry;
  return cache_entry != NULL;
}


gboolean _for_each_remove(gpointer key, gpointer value, gpointer user_data)
{
  dt_pixel_cache_entry_t *cache_entry = (dt_pixel_cache_entry_t *)value;
  const int id = GPOINTER_TO_INT(user_data);

  // Returns 1 if the lock is captured by another thread
  // 0 if WE capture the lock, and then need to release it
  gboolean locked = dt_pthread_rwlock_trywrlock(&cache_entry->lock);
  if(!locked) dt_pthread_rwlock_unlock(&cache_entry->lock);
  gboolean used = dt_atomic_get_int(&cache_entry->refcount) > 0;

  return (cache_entry->id == id || id == -1) && !used && !locked;
}

void dt_dev_pixelpipe_cache_flush(dt_dev_pixelpipe_cache_t *cache, const int id)
{
  dt_pthread_mutex_lock(&cache->lock);
  g_hash_table_foreach_remove(cache->entries, _for_each_remove, GINT_TO_POINTER(id));
  cache->current_memory = 0;
  dt_pthread_mutex_unlock(&cache->lock);
}

typedef struct _cache_invalidate_t
{
  void *data;
  size_t size;
} _cache_invalidate_t;


uint64_t _non_thread_safe_cache_get_hash_data(dt_dev_pixelpipe_cache_t *cache, void *data, dt_pixel_cache_entry_t **entry)
{
  GHashTableIter iter;
  gpointer key, value;
  uint64_t hash = 0;
  if(entry) *entry = NULL;

  g_hash_table_iter_init(&iter, cache->entries);
  while(g_hash_table_iter_next(&iter, &key, &value))
  {
    dt_pixel_cache_entry_t *cache_entry = (dt_pixel_cache_entry_t *)value;
    if(cache_entry->data == data)
    {
      hash = cache_entry->hash;
      if(entry) *entry = cache_entry;
      break;
    }
  }

  return hash;
}


uint64_t dt_dev_pixelpipe_cache_get_hash_data(dt_dev_pixelpipe_cache_t *cache, void *data, dt_pixel_cache_entry_t **entry)
{
  dt_pthread_mutex_lock(&cache->lock);
  uint64_t hash = _non_thread_safe_cache_get_hash_data(cache, data, entry);
  dt_pthread_mutex_unlock(&cache->lock);
  return hash;
}


dt_pixel_cache_entry_t *dt_dev_pixelpipe_cache_get_entry_from_data(dt_dev_pixelpipe_cache_t *cache, void *data)
{
  dt_pixel_cache_entry_t *cache_entry;
  dt_pthread_mutex_lock(&cache->lock);

  _non_thread_safe_cache_get_hash_data(cache, data, &cache_entry);
  if(cache_entry)
  {
    dt_pthread_rwlock_rdlock(&cache_entry->lock);
    dt_pixel_cache_message(cache_entry, "read lock", TRUE);
  }

  dt_pthread_mutex_unlock(&cache->lock);
  return cache_entry;
}


void _non_thread_safe_cache_ref_count_entry(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash, gboolean lock,
                                            dt_pixel_cache_entry_t *cache_entry)
{
  if(cache_entry == NULL)
    cache_entry = _non_threadsafe_cache_get_entry(cache, hash);

  if(cache_entry)
  {
    if(lock)
    {
      dt_atomic_add_int(&cache_entry->refcount, 1);
      dt_pixel_cache_message(cache_entry, "ref count ++", TRUE);
    }
    else
    {
      dt_atomic_sub_int(&cache_entry->refcount, 1);
      dt_pixel_cache_message(cache_entry, "ref count --", TRUE);
    }
  }
}


void dt_dev_pixelpipe_cache_ref_count_entry(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash, gboolean lock,
                                            dt_pixel_cache_entry_t *cache_entry)
{
  dt_pthread_mutex_lock(&cache->lock);
  _non_thread_safe_cache_ref_count_entry(cache, hash, lock, cache_entry);
  dt_pthread_mutex_unlock(&cache->lock);
}


void dt_dev_pixelpipe_cache_wrlock_entry(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash, gboolean lock,
                                         dt_pixel_cache_entry_t *cache_entry)
{
  if(cache_entry == NULL)
    cache_entry = dt_dev_pixelpipe_cache_get_entry(cache, hash);

  if(cache_entry)
  {
    if(lock)
    {
      dt_pthread_rwlock_wrlock(&cache_entry->lock);
      dt_pixel_cache_message(cache_entry, "write lock", TRUE);
    }
    else
    {
      dt_pthread_rwlock_unlock(&cache_entry->lock);
      dt_pixel_cache_message(cache_entry, "write unlock", TRUE);
    }
  }
}


void dt_dev_pixelpipe_cache_rdlock_entry(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash, gboolean lock,
                                         dt_pixel_cache_entry_t *cache_entry)
{
  if(cache_entry == NULL)
    cache_entry = dt_dev_pixelpipe_cache_get_entry(cache, hash);

  if(cache_entry)
  {
    if(lock)
    {
      dt_pthread_rwlock_rdlock(&cache_entry->lock);
      dt_pixel_cache_message(cache_entry, "read lock", TRUE);
    }
    else
    {
      dt_pthread_rwlock_unlock(&cache_entry->lock);
      dt_pixel_cache_message(cache_entry, "read unlock", TRUE);
    }
  }
}


void dt_dev_pixelpipe_cache_flag_auto_destroy(dt_dev_pixelpipe_cache_t *cache, uint64_t hash,
                                              dt_pixel_cache_entry_t *cache_entry)
{
  dt_pthread_mutex_lock(&cache->lock);
  if(cache_entry == NULL)
    cache_entry = _non_threadsafe_cache_get_entry(cache, hash);

  if(cache_entry) cache_entry->auto_destroy = TRUE;
  dt_pthread_mutex_unlock(&cache->lock);
}


void dt_dev_pixel_pipe_cache_auto_destroy_apply(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash, const int id,
                                                dt_pixel_cache_entry_t *cache_entry)
{
  dt_pthread_mutex_lock(&cache->lock);
  if(cache_entry == NULL)
    cache_entry = _non_threadsafe_cache_get_entry(cache, hash);

  if(cache_entry && cache_entry->auto_destroy && id == cache_entry->id)
  {
    cache->current_memory -= cache_entry->size;
    g_hash_table_remove(cache->entries, GINT_TO_POINTER(hash));
  }
  dt_pthread_mutex_unlock(&cache->lock);
}


void dt_dev_pixelpipe_cache_print(dt_dev_pixelpipe_cache_t *cache)
{
  if(!(darktable.unmuted & DT_DEBUG_PIPE)) return;

  dt_print(DT_DEBUG_PIPE, "[pixelpipe] cache hit rate so far: %.3f%% - size: %lu MiB over %lu MiB - %i items\n", 100. * (cache->hits) / (float)cache->queries, cache->current_memory / (1024 * 1024), cache->max_memory / (1024 * 1024), g_hash_table_size(cache->entries));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
