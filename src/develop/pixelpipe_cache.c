/*
    This file is part of darktable,
    Copyright (C) 2009-2012, 2015 johannes hanika.
    Copyright (C) 2010-2011 Henrik Andersson.
    Copyright (C) 2011 Robert Bieber.
    Copyright (C) 2011 Rostyslav Pidgornyi.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012-2014, 2016 Tobias Ellinghaus.
    Copyright (C) 2013-2014, 2016 Roman Lebedev.
    Copyright (C) 2014 Ulrich Pegelow.
    Copyright (C) 2019, 2023-2026 Aurélien PIERRE.
    Copyright (C) 2019-2021 Pascal Obry.
    Copyright (C) 2020, 2022 Hanno Schwalm.
    Copyright (C) 2020 Ralf Brown.
    Copyright (C) 2021 Aldric Renaudin.
    Copyright (C) 2021 Dan Torop.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2023 lologor.
    Copyright (C) 2024 Alynx Zhou.
    Copyright (C) 2025-2026 Guillaume Stutin.
    Copyright (C) 2025 Miguel Moquillon.
    
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

#include <inttypes.h>
#include <glib.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>

#include "control/control.h"
#include "develop/pixelpipe_cache.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/opencl.h"
#include "develop/format.h"
#include "develop/pixelpipe_hb.h"

static __thread const char *dt_pixelpipe_cache_current_module = NULL;

const char *dt_pixelpipe_cache_set_current_module(const char *module)
{
  const char *previous = dt_pixelpipe_cache_current_module;
  dt_pixelpipe_cache_current_module = module;
  return previous;
}

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
  gboolean external_alloc;  // TRUE for external buffers tracked in the cache
  int hits;                 // number of times this entry was hit (utility score)
  dt_dev_pixelpipe_cache_t *cache; // reference to parent cache object
  GList *cl_mem_list;       // reusable OpenCL pinned buffers tied to this entry
  dt_pthread_mutex_t cl_mem_lock;
} dt_pixel_cache_entry_t;

typedef struct dt_cache_clmem_t
{
  void *host_ptr;
  int devid;
  int width;
  int height;
  int bpp;
  int flags;
  int cst;
  void *mem;
} dt_cache_clmem_t;


void _non_thread_safe_cache_ref_count_entry(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash, gboolean lock,
                                            dt_pixel_cache_entry_t *cache_entry);
static void _free_cache_entry(dt_pixel_cache_entry_t *cache_entry);
static void _pixelpipe_cache_finalize_entry(dt_pixel_cache_entry_t *cache_entry, void **data,
                                            dt_iop_buffer_dsc_t **dsc, const char *message);
static dt_pixel_cache_entry_t *_pixelpipe_cache_create_entry_locked(dt_dev_pixelpipe_cache_t *cache,
                                                                    const uint64_t hash, const size_t size,
                                                                    const dt_iop_buffer_dsc_t *dsc,
                                                                    const char *name, const int id);
static dt_pixel_cache_entry_t *dt_pixel_cache_new_entry(const uint64_t hash, const size_t size,
                                                        const dt_iop_buffer_dsc_t dsc, const char *name, const int id,
                                                        dt_dev_pixelpipe_cache_t *cache, gboolean alloc,
                                                        GHashTable *table);
static void _cache_entry_clmem_flush_device(dt_pixel_cache_entry_t *entry, const int devid);
uint64_t _non_thread_safe_cache_get_hash_data(dt_dev_pixelpipe_cache_t *cache, void *data, dt_pixel_cache_entry_t **entry);

dt_pixel_cache_entry_t *_non_threadsafe_cache_get_entry(dt_dev_pixelpipe_cache_t *cache, GHashTable *table,
                                                        const uint64_t key)
{
  dt_pixel_cache_entry_t *entry = (dt_pixel_cache_entry_t *)g_hash_table_lookup(table, &key);
  if(entry) entry->hits++;
  return entry;
}


dt_pixel_cache_entry_t *dt_dev_pixelpipe_cache_get_entry(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash)
{
  dt_pthread_mutex_lock(&cache->lock);
  dt_pixel_cache_entry_t *entry = _non_threadsafe_cache_get_entry(cache, cache->entries, hash);
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
  dt_print(DT_DEBUG_CACHE, "[pixelpipe] cache entry %" PRIu64 ": %s (%lu MiB - age %" PRId64 " - hits %i - refs %i) %s\n", 
           cache_entry->hash, cache_entry->name, dt_pixel_cache_get_size(cache_entry), 
           cache_entry->age, cache_entry->hits, dt_atomic_get_int(&cache_entry->refcount), message);
}

static void _pixelpipe_cache_finalize_entry(dt_pixel_cache_entry_t *cache_entry, void **data,
                                            dt_iop_buffer_dsc_t **dsc, const char *message)
{
  cache_entry->age = g_get_monotonic_time(); // Update MRU timestamp
  if(data)
    *data = cache_entry->data ? __builtin_assume_aligned(cache_entry->data, DT_CACHELINE_BYTES) : NULL;
  if(dsc) *dsc = &cache_entry->dsc;
  dt_pixel_cache_message(cache_entry, message, FALSE);
}


// remove the cache entry with the given hash and update the cache memory usage
// WARNING: not internally thread-safe, protect its calls with mutex lock
// return 0 on success, 1 on error
int _non_thread_safe_cache_remove(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash, const gboolean force,
                                  dt_pixel_cache_entry_t *cache_entry, GHashTable *table)
{
  if(cache_entry == NULL)
    cache_entry = _non_threadsafe_cache_get_entry(cache, table, hash);

  if(cache_entry)
  {
    // Returns 1 if the lock is captured by another thread
    // 0 if WE capture the lock, and then need to release it
    gboolean locked = dt_pthread_rwlock_trywrlock(&cache_entry->lock);
    if(!locked) dt_pthread_rwlock_unlock(&cache_entry->lock);
    gboolean used = dt_atomic_get_int(&cache_entry->refcount) > 0;

    if((!used || force) && !locked)
    {
      g_hash_table_remove(table, &cache_entry->hash);
      return 0;
    }
    else if(used)
      dt_pixel_cache_message(cache_entry, "cannot remove: used", TRUE);
    else if(locked)
      dt_pixel_cache_message(cache_entry, "cannot remove: locked", TRUE);
  }
  else
  {
    dt_print(DT_DEBUG_CACHE, "[pixelpipe] cache entry %" PRIu64 " not found, will not be removed\n", hash);
  }
  return 1;
}


int dt_dev_pixelpipe_cache_remove(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash, const gboolean force,
                                  dt_pixel_cache_entry_t *cache_entry)
{
  dt_pthread_mutex_lock(&cache->lock);
  int error = _non_thread_safe_cache_remove(cache, hash, force, cache_entry, cache->entries);
  dt_pthread_mutex_unlock(&cache->lock);
  return error;
}

void dt_dev_pixelpipe_cache_flush_clmem(dt_dev_pixelpipe_cache_t *cache, const int devid)
{
  if(devid >= 0) dt_opencl_events_wait_for(devid);
  dt_pthread_mutex_lock(&cache->lock);
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, cache->entries);
  while(g_hash_table_iter_next(&iter, &key, &value))
  {
    dt_pixel_cache_entry_t *entry = (dt_pixel_cache_entry_t *)value;
    _cache_entry_clmem_flush_device(entry, devid);
  }
  dt_pthread_mutex_unlock(&cache->lock);
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

void _print_cache_lines(gpointer key, gpointer value, gpointer user_data)
{
  dt_pixel_cache_entry_t *cache_entry = (dt_pixel_cache_entry_t *)value;
  dt_pixel_cache_message(cache_entry, "", FALSE);
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
  {
    error = _non_thread_safe_cache_remove(cache, lru->hash, FALSE, lru->cache_entry, cache->entries);
    if(error)
      dt_print(DT_DEBUG_CACHE, "[pixelpipe] couldn't remove LRU %" PRIu64 "\n", lru->hash);
    else
      dt_print(DT_DEBUG_CACHE, "[pixelpipe] LRU %" PRIu64 " removed. Total cache size: %li MiB\n",
               lru->hash, cache->current_memory / (1024 * 1024));
  }
  else
  {
    dt_print(DT_DEBUG_CACHE, "[pixelpipe] couldn't remove LRU, %i items and all are used\n", g_hash_table_size(cache->entries));
    g_hash_table_foreach(cache->entries, _print_cache_lines, NULL);
  }

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

void *dt_pixel_cache_clmem_get(dt_pixel_cache_entry_t *entry, void *host_ptr, int devid,
                               int width, int height, int bpp, int flags, int *out_cst)
{
  if(out_cst) *out_cst = -1;

  dt_pthread_mutex_lock(&entry->cl_mem_lock);
  for(GList *l = entry->cl_mem_list; l; l = l->next)
  {
    dt_cache_clmem_t *c = (dt_cache_clmem_t *)l->data;
    if(c->host_ptr == host_ptr && c->devid == devid && c->width == width && c->height == height
       && c->bpp == bpp && c->flags == flags)
    {
      entry->cl_mem_list = g_list_delete_link(entry->cl_mem_list, l);
      void *mem = c->mem;
      if(out_cst) *out_cst = c->cst;
      g_free(c);
      dt_pthread_mutex_unlock(&entry->cl_mem_lock);
      return mem;
    }
  }
  dt_pthread_mutex_unlock(&entry->cl_mem_lock);
  return NULL;
}

void dt_pixel_cache_clmem_put(dt_pixel_cache_entry_t *entry, void *host_ptr, int devid,
                              int width, int height, int bpp, int flags, int cst, void *mem)
{

  dt_pthread_mutex_lock(&entry->cl_mem_lock);
  for(GList *l = entry->cl_mem_list; l; l = l->next)
  {
    dt_cache_clmem_t *c = (dt_cache_clmem_t *)l->data;
    if(c->mem == mem)
    {
      c->cst = cst;
      dt_pthread_mutex_unlock(&entry->cl_mem_lock);
      return;
    }
    if(c->host_ptr == host_ptr && c->devid == devid && c->width == width && c->height == height
       && c->bpp == bpp && c->flags == flags)
    {
      void *old = c->mem;
      c->mem = mem;
      c->cst = cst;
      dt_pthread_mutex_unlock(&entry->cl_mem_lock);
      dt_opencl_release_mem_object(old);
      return;
    }
  }

  dt_cache_clmem_t *c = (dt_cache_clmem_t *)g_malloc0(sizeof(*c));
  if(!c)
  {
    dt_pthread_mutex_unlock(&entry->cl_mem_lock);
    dt_opencl_release_mem_object(mem);
    return;
  }

  c->host_ptr = host_ptr;
  c->devid = devid;
  c->width = width;
  c->height = height;
  c->bpp = bpp;
  c->flags = flags;
  c->cst = cst;
  c->mem = mem;
  entry->cl_mem_list = g_list_prepend(entry->cl_mem_list, c);
  dt_pthread_mutex_unlock(&entry->cl_mem_lock);
}

void dt_pixel_cache_clmem_flush(dt_pixel_cache_entry_t *entry)
{
  dt_pthread_mutex_lock(&entry->cl_mem_lock);
  for(GList *l = entry->cl_mem_list; l; l = l->next)
  {
    dt_cache_clmem_t *c = (dt_cache_clmem_t *)l->data;
    dt_opencl_release_mem_object(c->mem);
    g_free(c);
  }
  g_list_free(entry->cl_mem_list);
  entry->cl_mem_list = NULL;
  dt_pthread_mutex_unlock(&entry->cl_mem_lock);
}

static void _cache_entry_clmem_flush_device(dt_pixel_cache_entry_t *entry, const int devid)
{
  dt_pthread_mutex_lock(&entry->cl_mem_lock);
  GList *l = entry->cl_mem_list;
  while(l)
  {
    GList *next = l->next;
    dt_cache_clmem_t *c = (dt_cache_clmem_t *)l->data;
    if(devid < 0 || c->devid == devid)
    {
      entry->cl_mem_list = g_list_delete_link(entry->cl_mem_list, l);
      dt_opencl_release_mem_object(c->mem);
      g_free(c);
    }
    l = next;
  }
  dt_pthread_mutex_unlock(&entry->cl_mem_lock);
}


void *dt_pixel_cache_alloc(dt_dev_pixelpipe_cache_t *cache, dt_pixel_cache_entry_t *cache_entry)
{
  // allocate the data buffer
  if(!cache_entry->data)
    cache_entry->data = dt_cache_arena_alloc(&cache->arena, cache_entry->size, &cache_entry->size);

  return cache_entry->data;
}

// WARNING: non thread-safe
static int _free_space_to_alloc(dt_dev_pixelpipe_cache_t *cache, const size_t size, const uint64_t hash,
                                const char *name)
{
  // Free up space if needed to match the max memory limit
  // If error, all entries are currently locked or in use, so we cannot free space to allocate a new entry.
  int error = 0;
  while(cache->current_memory + size > cache->max_memory && g_hash_table_size(cache->entries) > 0 && !error)
    error = _non_thread_safe_pixel_pipe_cache_remove_lru(cache);

  if(cache->current_memory + size > cache->max_memory)
  {
    const char *module = dt_pixelpipe_cache_current_module;
    const gboolean name_is_file = (name != NULL) && (strchr(name, '/') != NULL) && (strchr(name, ':') != NULL);
    if(!name) name = g_strdup("unknown");
    
    if(hash)
      fprintf(stdout, "[pixelpipe] cache is full, cannot allocate new entry %" PRIu64 " (%s)\n", hash, name);
    else
      fprintf(stdout, "[pixelpipe] cache is full, cannot allocate new entry (%s)\n", name);
    if(name && module && name_is_file)
      dt_control_log(_("The pipeline cache is full while allocating `%s` (module `%s`). Either your RAM settings are too frugal or your RAM is too small."), name, module);
    else if(name)
      dt_control_log(_("The pipeline cache is full while allocating `%s`. Either your RAM settings are too frugal or your RAM is too small."), name);
    else if(module)
      dt_control_log(_("The pipeline cache is full while processing module `%s`. Either your RAM settings are too frugal or your RAM is too small."), module);
    else
      dt_control_log(_("The pipeline cache is full. Either your RAM settings are too frugal or your RAM is too small."));
  }

  return error;
}

void *dt_pixelpipe_cache_alloc_align_cache_impl(dt_dev_pixelpipe_cache_t *cache, size_t size, int id,
                                                const char *name)
{
  // Free up space if needed to match the max memory limit
  // If error, all entries are currently locked or in use, so we cannot free space to allocate a new entry.
  dt_pthread_mutex_lock(&cache->lock);
  int error = _free_space_to_alloc(cache, size, 0, name);
  dt_pthread_mutex_unlock(&cache->lock);

  if(error) return NULL;

  // Page size is the desired size + AVX/SSE rounding
  size_t page_size = 0;
  void *buf = dt_cache_arena_alloc(&cache->arena, size, &page_size);
  if(!buf) return NULL;

  void *aligned = __builtin_assume_aligned(buf, DT_CACHELINE_BYTES);
  dt_iop_buffer_dsc_t dsc = {0};

  const uint64_t hash = (uint64_t)(uintptr_t)(aligned);

  dt_pthread_mutex_lock(&cache->lock);
  dt_pixel_cache_entry_t *cache_entry
      = dt_pixel_cache_new_entry(hash, page_size, dsc, name, id, cache, FALSE, cache->external_entries);

  if(!cache_entry)
  {
    dt_pthread_mutex_unlock(&cache->lock);
    dt_cache_arena_free(&cache->arena, buf, page_size);
    return NULL;
  }

  // Protect this entry from LRU/flush removal while in use.
  _non_thread_safe_cache_ref_count_entry(cache, hash, TRUE, cache_entry);
  dt_pthread_rwlock_wrlock(&cache_entry->lock);
  cache_entry->data = aligned;
  cache_entry->age = g_get_monotonic_time();
  cache_entry->external_alloc = TRUE;
  dt_pthread_mutex_unlock(&cache->lock);
  // Keep the lock held for the lifetime of this buffer.
  return aligned;
}

void dt_pixelpipe_cache_free_align_cache(dt_dev_pixelpipe_cache_t *cache, void **mem, const char *message)
{
  if(!mem || !*mem) return;

  dt_pthread_mutex_lock(&cache->lock);
  const uint64_t hash = (uint64_t)(uintptr_t)(*mem);
  dt_pixel_cache_entry_t *cache_entry = _non_threadsafe_cache_get_entry(cache, cache->external_entries, hash);
  if(!cache_entry || !cache_entry->external_alloc)
  {
    dt_pthread_mutex_unlock(&cache->lock);
    fprintf(stdout, "error while freeing cache entry: no entry found but we have a buffer, %s.\n", message);
    raise(SIGSEGV); // triggers dt_set_signal_handlers() backtrace on Unix
    return;
  }

  _non_thread_safe_cache_ref_count_entry(cache, cache_entry->hash, FALSE, cache_entry);
  dt_pthread_rwlock_unlock(&cache_entry->lock);
  g_hash_table_remove(cache->external_entries, &cache_entry->hash);
  *mem = NULL;

  dt_pthread_mutex_unlock(&cache->lock);
}


// WARNING: not thread-safe, protect its calls with mutex lock
static dt_pixel_cache_entry_t *dt_pixel_cache_new_entry(const uint64_t hash, const size_t size,
                                                        const dt_iop_buffer_dsc_t dsc, const char *name, const int id,
                                                        dt_dev_pixelpipe_cache_t *cache, gboolean alloc,
                                                        GHashTable *table)
{
  uint32_t pages_needed = 0;
  size_t rounded_size = 0;
  if(!dt_cache_arena_calc(&cache->arena, size, &pages_needed, &rounded_size))
  {
    fprintf(stderr, "[pixelpipe] invalid cache entry size %zu for %s\n", size, name);
    return NULL;
  }

  int error = _free_space_to_alloc(cache, rounded_size, hash, name);
  if(error) return NULL;

  dt_pixel_cache_entry_t *cache_entry = (dt_pixel_cache_entry_t *)malloc(sizeof(dt_pixel_cache_entry_t));
  if(!cache_entry) return NULL;

  // Metadata, easy to free in batch if need be
  cache_entry->size = rounded_size;
  cache_entry->age = 0;
  cache_entry->hits = 0;
  cache_entry->dsc = dsc;
  cache_entry->hash = hash;
  cache_entry->id = id;
  cache_entry->refcount = 0;
  cache_entry->auto_destroy = FALSE;
  cache_entry->external_alloc = FALSE;
  cache_entry->data = NULL;
  cache_entry->cache = cache;
  cache_entry->cl_mem_list = NULL;
  dt_pthread_mutex_init(&cache_entry->cl_mem_lock, NULL);

  // Optionally alloc the actual buffer, but still record its size in cache
  if(alloc) dt_pixel_cache_alloc(cache, cache_entry);

  if(alloc && !cache_entry->data)
  {
    free(cache_entry);
    return NULL;
  }
  
  // Metadata that need alloc
  cache_entry->name = g_strdup(name);
  dt_pthread_rwlock_init(&cache_entry->lock, NULL);

  uint64_t *key = g_malloc(sizeof(*key));
  if(!key)
  {
    dt_pthread_rwlock_destroy(&cache_entry->lock);
    g_free(cache_entry->name);
    dt_pthread_mutex_destroy(&cache_entry->cl_mem_lock);
    free(cache_entry);
    return NULL;
  }
  *key = hash;
  g_hash_table_insert(table, key, cache_entry);

  // Note : we grow the cache size even though the data buffer is not yet allocated
  // This is planning
  cache->current_memory += rounded_size;

  return cache_entry;
}


static void _free_cache_entry(dt_pixel_cache_entry_t *cache_entry)
{
  dt_pixel_cache_message(cache_entry, "freed", FALSE);

  if(cache_entry->data)
  {
    dt_cache_arena_free(&cache_entry->cache->arena, cache_entry->data, cache_entry->size);
  }

  dt_pixel_cache_clmem_flush(cache_entry);

  cache_entry->data = NULL;
  cache_entry->cache->current_memory -= cache_entry->size;
  dt_pthread_rwlock_destroy(&cache_entry->lock);
  dt_pthread_mutex_destroy(&cache_entry->cl_mem_lock);
  g_free(cache_entry->name);
  free(cache_entry);
}

static int garbage_collection = 0;

dt_dev_pixelpipe_cache_t * dt_dev_pixelpipe_cache_init(size_t max_memory)
{
  dt_dev_pixelpipe_cache_t *cache = (dt_dev_pixelpipe_cache_t *)malloc(sizeof(dt_dev_pixelpipe_cache_t));
  dt_pthread_mutex_init(&cache->lock, NULL);
  cache->entries = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, (GDestroyNotify)_free_cache_entry);
  cache->external_entries = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, (GDestroyNotify)_free_cache_entry);
  cache->max_memory = max_memory;
  cache->current_memory = 0;
  cache->queries = cache->hits = 0;

  if(!cache->entries || !cache->external_entries)
  {
    if(cache->entries) g_hash_table_destroy(cache->entries);
    if(cache->external_entries) g_hash_table_destroy(cache->external_entries);
    dt_pthread_mutex_destroy(&cache->lock);
    free(cache);
    return NULL;
  }

  if(dt_cache_arena_init(&cache->arena, cache->max_memory))
  {
    dt_pthread_mutex_destroy(&cache->lock);
    g_hash_table_destroy(cache->external_entries);
    g_hash_table_destroy(cache->entries);
    free(cache);
    return NULL;
  }

  // Run every 5 minutes
  garbage_collection = g_timeout_add(5 * 60 * 1000, (GSourceFunc)dt_dev_pixelpipe_cache_flush_old, cache);
  return cache;
}


void dt_dev_pixelpipe_cache_cleanup(dt_dev_pixelpipe_cache_t *cache)
{
  g_hash_table_destroy(cache->external_entries);
  g_hash_table_destroy(cache->entries);
  cache->external_entries = NULL;
  cache->entries = NULL;
  dt_pthread_mutex_destroy(&cache->lock);
  dt_cache_arena_cleanup(&cache->arena);

  if(garbage_collection != 0)
  {
    g_source_remove(garbage_collection);
    garbage_collection = 0;
  }
}

static dt_pixel_cache_entry_t *_pixelpipe_cache_create_entry_locked(dt_dev_pixelpipe_cache_t *cache,
                                                                    const uint64_t hash, const size_t size,
                                                                    const dt_iop_buffer_dsc_t *dsc,
                                                                    const char *name, const int id)
{
  dt_pixel_cache_entry_t *cache_entry = dt_pixel_cache_new_entry(hash, size, *dsc, name, id, cache, FALSE, cache->entries);
  if(!cache_entry) return NULL;

  // Increase ref_count, consumer will have to decrease it
  _non_thread_safe_cache_ref_count_entry(cache, hash, TRUE, cache_entry);

  // Acquire write lock so caller can populate data safely
  dt_dev_pixelpipe_cache_wrlock_entry(cache, hash, TRUE, cache_entry);

  return cache_entry;
}


int dt_dev_pixelpipe_cache_get(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash,
                               const size_t size, const char *name, const int id,
                               void **data, dt_iop_buffer_dsc_t **dsc,
                               dt_pixel_cache_entry_t **entry)
{
  // Search or create cache entry (under cache lock)
  dt_pthread_mutex_lock(&cache->lock);
  cache->queries++;

  dt_pixel_cache_entry_t *cache_entry = _non_threadsafe_cache_get_entry(cache, cache->entries, hash);
  if(cache_entry)
  {
    cache->hits++;
    _non_thread_safe_cache_ref_count_entry(cache, hash, TRUE, cache_entry);
    dt_pthread_mutex_unlock(&cache->lock);
    _pixelpipe_cache_finalize_entry(cache_entry, data, dsc, "found");
    if(entry) *entry = cache_entry;
    return 0;
  }

  cache_entry = _pixelpipe_cache_create_entry_locked(cache, hash, size, *dsc, name, id);
  if(!cache_entry)
  {
    dt_print(DT_DEBUG_CACHE, "couldn't allocate new cache entry %" PRIu64 "\n", hash);
    dt_pthread_mutex_unlock(&cache->lock);
    if(entry) *entry = NULL;
    return 1;
  }

  // Release cache lock AFTER acquiring entry locks to prevent other threads to capture it in-between
  dt_pthread_mutex_unlock(&cache->lock);

  // Alloc after releasing the lock for better runtimes
  dt_pixel_cache_alloc(cache, cache_entry);

  dt_print(DT_DEBUG_CACHE, "[pixelpipe_cache] Write-lock on entry (new cache entry %" PRIu64 " for %s pipeline)\n",
           hash, name);
  _pixelpipe_cache_finalize_entry(cache_entry, data, dsc, "created");

  if(entry) *entry = cache_entry;
  return 1;
}

int dt_dev_pixelpipe_cache_get_existing(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash,
                                        void **data, dt_iop_buffer_dsc_t **dsc, dt_pixel_cache_entry_t **entry)
{
  // Find the cache entry for this hash, if any
  dt_pthread_mutex_lock(&cache->lock);
  cache->queries++;
  dt_pixel_cache_entry_t *cache_entry = _non_threadsafe_cache_get_entry(cache, cache->entries, hash);

  if(cache_entry)
  {
    cache->hits++;
    _pixelpipe_cache_finalize_entry(cache_entry, data, dsc, "found");
  }

  dt_pthread_mutex_unlock(&cache->lock);

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

  return (cache_entry->id == id || id == -1) && !locked;
}


void dt_dev_pixelpipe_cache_flush(dt_dev_pixelpipe_cache_t *cache, const int id)
{
  dt_pthread_mutex_lock(&cache->lock);
  g_hash_table_foreach_remove(cache->entries, _for_each_remove, GINT_TO_POINTER(id));
  dt_pthread_mutex_unlock(&cache->lock);
}


gboolean _for_each_remove_old(gpointer key, gpointer value, gpointer user_data)
{
  dt_pixel_cache_entry_t *cache_entry = (dt_pixel_cache_entry_t *)value;

  // Returns 1 if the lock is captured by another thread
  // 0 if WE capture the lock, and then need to release it
  gboolean locked = dt_pthread_rwlock_trywrlock(&cache_entry->lock);
  if(!locked) dt_pthread_rwlock_unlock(&cache_entry->lock);
  gboolean used = dt_atomic_get_int(&cache_entry->refcount) > 0;

  // in microseconds
  int64_t delta = g_get_monotonic_time() - cache_entry->age;

  // 3 min in microseconds
  const int64_t ten_min = 3 * 60 * 1000 * 1000;

  gboolean too_old = (delta > ten_min) && (cache_entry->hits < 4);

  return too_old && !used && !locked;
}

int dt_dev_pixelpipe_cache_flush_old(dt_dev_pixelpipe_cache_t *cache)
{
  // Don't hang the GUI thread if the cache is locked by a pipeline.
  // Better luck next time.
  if(dt_pthread_mutex_trylock(&cache->lock)) return G_SOURCE_CONTINUE;
  g_hash_table_foreach_remove(cache->entries, _for_each_remove_old, NULL);
  dt_pthread_mutex_unlock(&cache->lock);
  return G_SOURCE_CONTINUE;
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
      cache_entry->hits++;
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


void _non_thread_safe_cache_ref_count_entry(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash, gboolean lock,
                                            dt_pixel_cache_entry_t *cache_entry)
{
  if(cache_entry == NULL)
    cache_entry = _non_threadsafe_cache_get_entry(cache, cache->entries, hash);

  if(cache_entry == NULL) return;

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


void dt_dev_pixelpipe_cache_rdlock_entry(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash, gboolean lock,
                                         dt_pixel_cache_entry_t *cache_entry)
{
  if(cache_entry == NULL)
    cache_entry = dt_dev_pixelpipe_cache_get_entry(cache, hash);

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


void dt_dev_pixelpipe_cache_flag_auto_destroy(dt_dev_pixelpipe_cache_t *cache, uint64_t hash,
                                              dt_pixel_cache_entry_t *cache_entry)
{
  dt_pthread_mutex_lock(&cache->lock);
  if(cache_entry == NULL)
    cache_entry = _non_threadsafe_cache_get_entry(cache, cache->entries, hash);

  cache_entry->auto_destroy = TRUE;
  dt_pthread_mutex_unlock(&cache->lock);
}


void dt_dev_pixel_pipe_cache_auto_destroy_apply(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash,
                                                dt_pixel_cache_entry_t *cache_entry)
{
  dt_pthread_mutex_lock(&cache->lock);
  if(cache_entry == NULL)
    cache_entry = _non_threadsafe_cache_get_entry(cache, cache->entries, hash);

  if(cache_entry->auto_destroy)
  {
    const uint64_t key = hash;
    g_hash_table_remove(cache->entries, &key);
  }
  
  dt_pthread_mutex_unlock(&cache->lock);
}

void *dt_dev_pixelpipe_cache_get_read_only(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash, 
                                           dt_pixel_cache_entry_t **cache_entry, 
                                           dt_develop_t *dev, dt_dev_pixelpipe_t *pipe)
{
  void *data = NULL;
  if(!dt_dev_pixelpipe_cache_get_existing(cache, hash, &data, NULL, cache_entry))
  {
    // Ask for a new recompute if cacheline is missing
    dt_dev_process(dev, pipe);
    return NULL;
  } 

  // Assuming this function is called from GUI, we don't want to make it hang
  // if our entry data is getting written by something heavy in a pipe thread.
  // It should be tried again when the pipeline returns and emits "finished" signal.
  // Meanwhile, abort for now.
  gboolean locked = dt_pthread_rwlock_tryrdlock(&((*cache_entry)->lock));
  if(locked) return NULL;
  // else: trylock also locks it.

  dt_dev_pixelpipe_cache_ref_count_entry(cache, hash, TRUE, *cache_entry);

  return data ? __builtin_assume_aligned(data, DT_CACHELINE_BYTES) : NULL;
}

void dt_dev_pixelpipe_cache_close_read_only(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash, 
                                            dt_pixel_cache_entry_t *cache_entry)
{
  dt_dev_pixelpipe_cache_ref_count_entry(cache, hash, FALSE, cache_entry);
  dt_dev_pixelpipe_cache_rdlock_entry(cache, hash, FALSE, cache_entry);
}

void dt_dev_pixelpipe_cache_unref_hash(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash)
{
  if(hash == (uint64_t)-1) return;

  dt_pthread_mutex_lock(&cache->lock);
  cache->queries++;
  dt_pixel_cache_entry_t *cache_entry = _non_threadsafe_cache_get_entry(cache, cache->entries, hash);
  dt_pthread_mutex_unlock(&cache->lock);

  if(cache_entry)
    dt_dev_pixelpipe_cache_ref_count_entry(cache, hash, FALSE, cache_entry);
}


void dt_dev_pixelpipe_cache_print(dt_dev_pixelpipe_cache_t *cache)
{
  if(!(darktable.unmuted & DT_DEBUG_CACHE)) return;

  dt_print(DT_DEBUG_CACHE, "[pixelpipe] cache hit rate so far: %.3f%% - size: %lu MiB over %lu MiB - %i items\n", 100. * (cache->hits) / (float)cache->queries, cache->current_memory / (1024 * 1024), cache->max_memory / (1024 * 1024), g_hash_table_size(cache->entries));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
