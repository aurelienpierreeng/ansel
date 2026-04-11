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
#include "control/signal.h"
#include "develop/pixelpipe_cache.h"
#include "develop/pixelpipe.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/opencl.h"
#include "develop/format.h"

static __thread const char *dt_pixelpipe_cache_current_module = NULL;

static dt_pixel_cache_entry_t *_non_threadsafe_cache_get_entry(dt_dev_pixelpipe_cache_t *cache, GHashTable *table,
                                                               const uint64_t key);

static inline const char *_cache_debug_module_name(void)
{
  return dt_pixelpipe_cache_current_module ? dt_pixelpipe_cache_current_module : "-";
}

static void _trace_exact_hit(const char *phase, const uint64_t hash, dt_pixel_cache_entry_t *cache_entry,
                             void *data, void *cl_mem_output, const int preferred_devid, const gboolean verbose)
{
  if(!(darktable.unmuted & DT_DEBUG_PIPECACHE)) return;
  if(verbose && !(darktable.unmuted & DT_DEBUG_VERBOSE)) return;

  dt_print(DT_DEBUG_PIPECACHE,
           "[pixelpipe_cache] exact-hit %s req=%" PRIu64 " entry=%" PRIu64 "/%" PRIu64
           " data=%p cl=%p refs=%i auto=%i dev=%i module=%s name=%s\n",
           phase, hash, cache_entry ? cache_entry->hash : DT_PIXELPIPE_CACHE_HASH_INVALID,
           cache_entry ? cache_entry->serial : 0, data, cl_mem_output,
           cache_entry ? dt_atomic_get_int(&cache_entry->refcount) : -1,
           cache_entry ? cache_entry->auto_destroy : -1, preferred_devid, _cache_debug_module_name(),
           (cache_entry && cache_entry->name) ? cache_entry->name : "-");
}

const char *dt_pixelpipe_cache_set_current_module(const char *module)
{
  const char *previous = dt_pixelpipe_cache_current_module;
  dt_pixelpipe_cache_current_module = module;
  return previous;
}

typedef struct dt_cache_clmem_t
{
  void *host_ptr;
  int devid;
  int width;
  int height;
  int bpp;
  int flags;
  void *mem;
  int refs;
} dt_cache_clmem_t;

typedef enum dt_pixel_cache_materialize_source_rank_t
{
  DT_PIXEL_CACHE_MATERIALIZE_SOURCE_NONE = 0,
  DT_PIXEL_CACHE_MATERIALIZE_SOURCE_SECONDARY_ANY = 1,
  DT_PIXEL_CACHE_MATERIALIZE_SOURCE_SECONDARY_PREFERRED = 2,
  DT_PIXEL_CACHE_MATERIALIZE_SOURCE_PRIMARY_ANY = 3,
  DT_PIXEL_CACHE_MATERIALIZE_SOURCE_PRIMARY_PREFERRED = 4,
} dt_pixel_cache_materialize_source_rank_t;


void _non_thread_safe_cache_ref_count_entry(dt_dev_pixelpipe_cache_t *cache, gboolean lock,
                                            dt_pixel_cache_entry_t *cache_entry);
static void _free_cache_entry(dt_pixel_cache_entry_t *cache_entry);
static void _pixelpipe_cache_finalize_entry(dt_pixel_cache_entry_t *cache_entry, void **data,
                                            const char *message);
int _non_thread_safe_cache_remove(dt_dev_pixelpipe_cache_t *cache, const gboolean force,
                                  dt_pixel_cache_entry_t *cache_entry, GHashTable *table);
static void _cache_remove_payloadless_entry_locked(dt_dev_pixelpipe_cache_t *cache,
                                                   dt_pixel_cache_entry_t *cache_entry,
                                                   const char *message);
static dt_pixel_cache_entry_t *_pixelpipe_cache_create_entry_locked(dt_dev_pixelpipe_cache_t *cache,
                                                                    const uint64_t hash, const size_t size,
                                                                    const char *name, const int id);
static dt_pixel_cache_entry_t *dt_pixel_cache_new_entry(const uint64_t hash, const size_t size,
                                                        const char *name, const int id,
                                                        dt_dev_pixelpipe_cache_t *cache, gboolean alloc,
                                                        GHashTable *table);
static void _cache_entry_clmem_flush_device(dt_pixel_cache_entry_t *entry, const int devid, void *keep);
static gboolean _cache_entry_materialize_host_data_locked(dt_pixel_cache_entry_t *entry, int preferred_devid,
                                                          gboolean prefer_device_payload);
static int dt_dev_pixelpipe_cache_flush_old(dt_dev_pixelpipe_cache_t *cache);

#ifdef HAVE_OPENCL
static gboolean _cache_entry_clmem_flush_host_pinned_locked(dt_pixel_cache_entry_t *entry, void *host_ptr, int devid);
#endif

static dt_pixel_cache_entry_t *_cache_entry_for_host_ptr_locked(dt_dev_pixelpipe_cache_t *cache, void *host_ptr)
{
  if(IS_NULL_PTR(cache) || IS_NULL_PTR(host_ptr)) return NULL;

  const uint64_t hash = (uint64_t)(uintptr_t)host_ptr;
  dt_pixel_cache_entry_t *entry = _non_threadsafe_cache_get_entry(cache, cache->external_entries, hash);
  if(entry && entry->external_alloc && entry->data == host_ptr) return entry;
  return NULL;
}

dt_pixel_cache_entry_t *_non_threadsafe_cache_get_entry(dt_dev_pixelpipe_cache_t *cache, GHashTable *table,
                                                        const uint64_t key)
{
  dt_pixel_cache_entry_t *entry = (dt_pixel_cache_entry_t *)g_hash_table_lookup(table, &key);
  return entry;
}


dt_pixel_cache_entry_t *dt_dev_pixelpipe_cache_get_entry(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash)
{
  if(hash == DT_PIXELPIPE_CACHE_HASH_INVALID) return NULL;
  dt_pthread_mutex_lock(&cache->lock);
  dt_pixel_cache_entry_t *entry = _non_threadsafe_cache_get_entry(cache, cache->entries, hash);
  dt_pthread_mutex_unlock(&cache->lock);
  return entry;
}


static size_t _pixel_cache_get_size(dt_pixel_cache_entry_t *cache_entry)
{
  return cache_entry->size / (1024 * 1024);
}


static void _pixel_cache_message(dt_pixel_cache_entry_t *cache_entry, const char *message, gboolean verbose)
{
  if(!(darktable.unmuted & DT_DEBUG_PIPECACHE)) return;
  if(verbose && !(darktable.unmuted & DT_DEBUG_VERBOSE)) return;
  dt_print(DT_DEBUG_PIPECACHE,
           "[pixelpipe] cache entry %" PRIu64 "/%" PRIu64 ": %s (data=%p - %lu MiB - age %" PRId64
           " - hits %i - refs %i - auto %i - ext %i - id %i - module %s) %s\n",
           cache_entry->hash, cache_entry->serial,
           cache_entry->name ? cache_entry->name : "-", cache_entry->data,
           _pixel_cache_get_size(cache_entry), cache_entry->age, cache_entry->hits,
           dt_atomic_get_int(&cache_entry->refcount), cache_entry->auto_destroy,
           cache_entry->external_alloc, cache_entry->id, _cache_debug_module_name(), message);
}

static void _pixelpipe_cache_finalize_entry(dt_pixel_cache_entry_t *cache_entry, void **data,
                                            const char *message)
{
  cache_entry->age = g_get_monotonic_time(); // Update MRU timestamp
  if(data)
    *data = cache_entry->data ? __builtin_assume_aligned(cache_entry->data, DT_CACHELINE_BYTES) : NULL;
  _pixel_cache_message(cache_entry, message, FALSE);
}

static void _cache_remove_payloadless_entry_locked(dt_dev_pixelpipe_cache_t *cache,
                                                   dt_pixel_cache_entry_t *cache_entry,
                                                   const char *message)
{
  if(IS_NULL_PTR(cache) || IS_NULL_PTR(cache_entry) || cache_entry->external_alloc) return;
  if(_non_threadsafe_cache_get_entry(cache, cache->entries, cache_entry->hash) != cache_entry) return;
  if(!IS_NULL_PTR(cache_entry->data)) return;

  gboolean has_clmem = FALSE;
  dt_pthread_mutex_lock(&cache_entry->cl_mem_lock);
  has_clmem = (!IS_NULL_PTR(cache_entry->cl_mem_list));
  dt_pthread_mutex_unlock(&cache_entry->cl_mem_lock);

  if(has_clmem) return;

  _pixel_cache_message(cache_entry, message, FALSE);
  _non_thread_safe_cache_remove(cache, FALSE, cache_entry, cache->entries);
}


// remove the cache entry with the given hash and update the cache memory usage
// WARNING: not internally thread-safe, protect its calls with mutex lock
// return 0 on success, 1 on error
int _non_thread_safe_cache_remove(dt_dev_pixelpipe_cache_t *cache, const gboolean force,
                                  dt_pixel_cache_entry_t *cache_entry, GHashTable *table)
{
  if(cache_entry)
  {
    // Returns 1 if the lock is captured by another thread
    // 0 if WE capture the lock, and then need to release it
    gboolean locked = dt_pthread_rwlock_trywrlock(&cache_entry->lock);
    if(!locked) dt_pthread_rwlock_unlock(&cache_entry->lock);
    gboolean used = dt_atomic_get_int(&cache_entry->refcount) > 0;

    if((!used || force) && !locked)
    {
      // Note: the free callback takes care of flushing OpenCL buffers too
      g_hash_table_remove(table, &cache_entry->hash);
      return 0;
    }
    else if(used)
      _pixel_cache_message(cache_entry, "cannot remove: used", TRUE);
    else if(locked)
      _pixel_cache_message(cache_entry, "cannot remove: locked", TRUE);
  }
  else
  {
    dt_print(DT_DEBUG_PIPECACHE, "[pixelpipe] cache entry not found, will not be removed\n");
  }
  return 1;
}


int dt_dev_pixelpipe_cache_remove(dt_dev_pixelpipe_cache_t *cache, const gboolean force,
                                  dt_pixel_cache_entry_t *cache_entry)
{
  dt_pthread_mutex_lock(&cache->lock);
  int error = _non_thread_safe_cache_remove(cache, force, cache_entry, cache->entries);
  dt_pthread_mutex_unlock(&cache->lock);
  return error;
}

#ifdef HAVE_OPENCL
static gboolean _cache_entry_materialize_host_data_locked(dt_pixel_cache_entry_t *entry, int preferred_devid,
                                                          gboolean prefer_device_payload)
{
  if(IS_NULL_PTR(entry)) return FALSE;
  dt_cache_clmem_t *source = NULL;
  gboolean ok = FALSE;
  dt_pthread_mutex_lock(&entry->cl_mem_lock);

  /* We materialize RAM from the most authoritative cached payload in one pass instead of
   * walking the list multiple times with slightly different predicates:
   * - when RAM existed before, prefer pinned host-backed payloads first because they should
   *   already alias the cacheline or be the cheapest path back to host,
   * - when RAM has just been allocated for a GPU-only cacheline, prefer device payloads first,
   * - if a preferred OpenCL device is known, rank payloads from that device ahead of the rest.
   * This keeps the fallback order explicit without scattering it over six loops. */
  dt_pixel_cache_materialize_source_rank_t best_rank = DT_PIXEL_CACHE_MATERIALIZE_SOURCE_NONE;
  for(GList *l = entry->cl_mem_list; l;)
  {
    GList *next = g_list_next(l);
    dt_cache_clmem_t *c = (dt_cache_clmem_t *)l->data;
    if(IS_NULL_PTR(c) || IS_NULL_PTR(c->mem))
    {
      l = next;
      continue;
    }

    /* We are looking for one authoritative cached payload to materialize back to RAM.
     * Only consider records whose live OpenCL context still matches the recorded device.
     * Other cached payloads may belong to a different pipeline/device and must stay untouched. */
    const int mem_devid = dt_opencl_get_mem_context_id((cl_mem)c->mem);
    if(mem_devid != c->devid)
    {
      l = next;
      continue;
    }

    const cl_mem_flags flags = dt_opencl_get_mem_flags((cl_mem)c->mem);
    const gboolean host_backed = (c->host_ptr == entry->data) && (flags & CL_MEM_USE_HOST_PTR);
    const gboolean device_only = (IS_NULL_PTR(c->host_ptr));
    if(!host_backed && !device_only)
    {
      l = next;
      continue;
    }

    dt_pixel_cache_materialize_source_rank_t rank = DT_PIXEL_CACHE_MATERIALIZE_SOURCE_NONE;
    if(!prefer_device_payload)
    {
      if(host_backed && (preferred_devid < 0 || c->devid == preferred_devid))
        rank = DT_PIXEL_CACHE_MATERIALIZE_SOURCE_PRIMARY_PREFERRED;
      else if(device_only && preferred_devid >= 0 && c->devid == preferred_devid)
        rank = DT_PIXEL_CACHE_MATERIALIZE_SOURCE_SECONDARY_PREFERRED;
      else if(device_only)
        rank = DT_PIXEL_CACHE_MATERIALIZE_SOURCE_SECONDARY_ANY;
      else if(host_backed)
        rank = DT_PIXEL_CACHE_MATERIALIZE_SOURCE_PRIMARY_ANY;
    }
    else
    {
      if(device_only && preferred_devid >= 0 && c->devid == preferred_devid)
        rank = DT_PIXEL_CACHE_MATERIALIZE_SOURCE_PRIMARY_PREFERRED;
      else if(device_only)
        rank = DT_PIXEL_CACHE_MATERIALIZE_SOURCE_PRIMARY_ANY;
      else if(host_backed && (preferred_devid < 0 || c->devid == preferred_devid))
        rank = DT_PIXEL_CACHE_MATERIALIZE_SOURCE_SECONDARY_PREFERRED;
      else if(host_backed)
        rank = DT_PIXEL_CACHE_MATERIALIZE_SOURCE_SECONDARY_ANY;
    }

    if(rank > best_rank)
    {
      best_rank = rank;
      source = c;
      if(rank == DT_PIXEL_CACHE_MATERIALIZE_SOURCE_PRIMARY_PREFERRED) break;
    }

    l = next;
  }

  if(source)
  {
    const cl_mem_flags flags = dt_opencl_get_mem_flags((cl_mem)source->mem);
    if((flags & CL_MEM_USE_HOST_PTR) && source->host_ptr == entry->data)
    {
      void *mapped = dt_opencl_map_image(source->devid, (cl_mem)source->mem, TRUE, CL_MAP_READ,
                                         source->width, source->height, source->bpp);
      if(!mapped)
      {
        ok = FALSE;
      }
      else
      {
        const cl_int unmap_err = dt_opencl_unmap_mem_object(source->devid, (cl_mem)source->mem, mapped);
        dt_opencl_finish(source->devid);

        if(unmap_err != CL_SUCCESS)
        {
          ok = FALSE;
        }
        else
        {
          /* dt_opencl_map_image() does not expose the mapped row pitch, so a non-zero-copy mapping
           * cannot be copied back with a flat memcpy without risking row-stride corruption. Only the
           * true zero-copy case guarantees that `entry->data` already aliases the authoritative pixels.
           * Otherwise, fall back to the explicit device->host transfer helper, which handles image rows
           * correctly through the OpenCL runtime. */
          if(mapped == entry->data)
            ok = TRUE;
          else
          {
            ok = (dt_opencl_read_host_from_device(source->devid, entry->data, source->mem,
                                                  source->width, source->height, source->bpp) == CL_SUCCESS);
          }
        }
      }
    }
    else
    {
      ok = (dt_opencl_read_host_from_device(source->devid, entry->data, source->mem,
                                            source->width, source->height, source->bpp) == CL_SUCCESS);
    }
  }

  dt_pthread_mutex_unlock(&entry->cl_mem_lock);
  return ok;
}
#else
static gboolean _cache_entry_materialize_host_data_locked(dt_pixel_cache_entry_t *entry, int preferred_devid,
                                                          gboolean prefer_device_payload)
{
  (void)preferred_devid;
  (void)prefer_device_payload;
  return entry && !IS_NULL_PTR(entry->data);
}
#endif

static gboolean _cache_entry_materialize_host_data(dt_dev_pixelpipe_cache_t *cache, int preferred_devid,
                                                   dt_pixel_cache_entry_t *entry)
{
  if(IS_NULL_PTR(cache) || IS_NULL_PTR(entry)) return FALSE;
  if(preferred_devid < 0 && dt_pixel_cache_entry_get_data(entry) == NULL) return FALSE;

  dt_dev_pixelpipe_cache_wrlock_entry(cache, TRUE, entry);
  const gboolean had_host_payload = (dt_pixel_cache_entry_get_data(entry) != NULL);
  if(!had_host_payload)
    dt_pixel_cache_alloc(cache, entry);
  const gboolean ok = _cache_entry_materialize_host_data_locked(entry, preferred_devid, !had_host_payload);
  dt_dev_pixelpipe_cache_wrlock_entry(cache, FALSE, entry);

  return ok;
}

#ifdef HAVE_OPENCL
static gboolean _cache_entry_clmem_has_host_pinned_locked(dt_pixel_cache_entry_t *entry, void *host_ptr, int devid)
{
  if(IS_NULL_PTR(entry) || IS_NULL_PTR(host_ptr)) return FALSE;

  gboolean found = FALSE;
  dt_pthread_mutex_lock(&entry->cl_mem_lock);
  for(GList *l = entry->cl_mem_list; l; l = g_list_next(l))
  {
    dt_cache_clmem_t *c = (dt_cache_clmem_t *)l->data;
    const int mem_devid = (c && c->mem) ? dt_opencl_get_mem_context_id((cl_mem)c->mem) : -1;
    const cl_mem_flags flags = c ? dt_opencl_get_mem_flags((cl_mem)c->mem) : 0;

    if(c && c->mem && mem_devid != c->devid) continue;

    if(c && c->mem && c->refs == 0 && c->host_ptr == host_ptr && (devid < 0 || c->devid == devid)
       && (flags & CL_MEM_USE_HOST_PTR))
    {
      found = TRUE;
      break;
    }
  }
  dt_pthread_mutex_unlock(&entry->cl_mem_lock);

  return found;
}

static gboolean _cache_entry_clmem_flush_host_pinned_locked(dt_pixel_cache_entry_t *entry, void *host_ptr, int devid)
{
  if(IS_NULL_PTR(entry) || IS_NULL_PTR(host_ptr)) return FALSE;

  gboolean flushed = FALSE;
  dt_pthread_mutex_lock(&entry->cl_mem_lock);
  for(GList *l = entry->cl_mem_list; l;)
  {
    GList *next = g_list_next(l);
    dt_cache_clmem_t *c = (dt_cache_clmem_t *)l->data;
    const int mem_devid = (c && c->mem) ? dt_opencl_get_mem_context_id((cl_mem)c->mem) : -1;
    const cl_mem_flags flags = c ? dt_opencl_get_mem_flags((cl_mem)c->mem) : 0;

    if(c && c->mem && mem_devid != c->devid)
    {
      l = next;
      continue;
    }

    if(c && c->host_ptr == host_ptr && (devid < 0 || c->devid == devid) && (flags & CL_MEM_USE_HOST_PTR))
    {
      if(c->refs > 0)
      {
        l = next;
        continue;
      }

      entry->cl_mem_list = g_list_delete_link(entry->cl_mem_list, l);
      dt_opencl_release_mem_object(c->mem);
      dt_free(c);
      flushed = TRUE;
    }

    l = next;
  }
  dt_pthread_mutex_unlock(&entry->cl_mem_lock);
  return flushed;
}
#endif

void dt_dev_pixelpipe_cache_flush_clmem(dt_dev_pixelpipe_cache_t *cache, const int devid, void *keep)
{
  if(devid >= 0) dt_opencl_events_wait_for(devid);

  dt_pthread_mutex_lock(&cache->lock);
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, cache->entries);
  while(g_hash_table_iter_next(&iter, &key, &value))
  {
    dt_pixel_cache_entry_t *entry = (dt_pixel_cache_entry_t *)value;
    /* Realtime display paths use cached cl_mem as scratch storage. Flushing VRAM
     * must stay lightweight and must not wait on per-entry writer locks, otherwise
     * allocation fallback can deadlock against in-flight GPU renders that already
     * hold cache entry locks. */
    _cache_entry_clmem_flush_device(entry, devid, keep);
    _cache_remove_payloadless_entry_locked(cache, entry, "dropping payload-less entry after clmem flush");
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
static void _cache_get_oldest(gpointer key, gpointer value, gpointer user_data)
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
      _pixel_cache_message(cache_entry, "candidate for deletion", TRUE);
    }
    else if(used)
      _pixel_cache_message(cache_entry, "cannot be deleted: used", TRUE);
    else if(locked)
      _pixel_cache_message(cache_entry, "cannot be deleted: locked", TRUE);
  }
}

static void _print_cache_lines(gpointer key, gpointer value, gpointer user_data)
{
  dt_pixel_cache_entry_t *cache_entry = (dt_pixel_cache_entry_t *)value;
  _pixel_cache_message(cache_entry, "", FALSE);
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
    error = _non_thread_safe_cache_remove(cache, FALSE, lru->cache_entry, cache->entries);
    if(error)
      dt_print(DT_DEBUG_PIPECACHE, "[pixelpipe] couldn't remove LRU %" PRIu64 "\n", lru->hash);
    else
      dt_print(DT_DEBUG_PIPECACHE, "[pixelpipe] LRU %" PRIu64 " removed. Total cache size: %li MiB\n",
               lru->hash, cache->current_memory / (1024 * 1024));
  }
  else
  {
    dt_print(DT_DEBUG_PIPECACHE, "[pixelpipe] couldn't remove LRU, %i items and all are used\n", g_hash_table_size(cache->entries));
    g_hash_table_foreach(cache->entries, _print_cache_lines, NULL);
  }

  dt_free(lru);
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

#ifdef HAVE_OPENCL
static void *_pixel_cache_clmem_get(dt_pixel_cache_entry_t *entry, void *host_ptr, int devid,
                                    int width, int height, int bpp, int flags)
{
  dt_pthread_mutex_lock(&entry->cl_mem_lock);
  for(GList *l = entry->cl_mem_list; l;)
  {
    GList *next = g_list_next(l);
    dt_cache_clmem_t *c = (dt_cache_clmem_t *)l->data;
    if(c && c->host_ptr == host_ptr && c->width == width && c->height == height
       && c->bpp == bpp && c->flags == flags)
    {
      if(c->refs > 0)
      {
        l = next;
        continue;
      }

      /* Reuse must stay on the same OpenCL device. Be defensive and verify the
       * live cl_mem context too, not only the cached metadata. */
      const int mem_devid = dt_opencl_get_mem_context_id((cl_mem)c->mem);
      if(c->devid != devid || mem_devid != devid)
      {
        entry->cl_mem_list = g_list_delete_link(entry->cl_mem_list, l);
        dt_opencl_release_mem_object((cl_mem)c->mem);
        dt_free(c);
        l = next;
        continue;
      }

      entry->cl_mem_list = g_list_delete_link(entry->cl_mem_list, l);
      void *mem = c->mem;
      dt_free(c);
      dt_pthread_mutex_unlock(&entry->cl_mem_lock);
      return mem;
    }
    l = next;
  }
  dt_pthread_mutex_unlock(&entry->cl_mem_lock);

  return NULL;
}
#endif

void *dt_dev_pixelpipe_cache_borrow_cl_payload(dt_pixel_cache_entry_t *entry, void *host_ptr, int devid,
                                               int width, int height, int bpp, int flags)
{
#ifdef HAVE_OPENCL

  dt_pthread_mutex_lock(&entry->cl_mem_lock);
  for(GList *l = entry->cl_mem_list; l; l = g_list_next(l))
  {
    dt_cache_clmem_t *c = (dt_cache_clmem_t *)l->data;
    if(c && c->host_ptr == host_ptr && c->width == width && c->height == height
       && c->bpp == bpp && c->flags == flags)
    {
      const int mem_devid = dt_opencl_get_mem_context_id((cl_mem)c->mem);
      if(c->devid != devid || mem_devid != devid)
        continue;

      c->refs++;
      void *mem = c->mem;
      dt_pthread_mutex_unlock(&entry->cl_mem_lock);
      return mem;
    }
  }
  dt_pthread_mutex_unlock(&entry->cl_mem_lock);

#endif

  return NULL;
}

void dt_dev_pixelpipe_cache_return_cl_payload(dt_pixel_cache_entry_t *entry, void *mem)
{
#ifdef HAVE_OPENCL

  if(IS_NULL_PTR(entry) || IS_NULL_PTR(mem)) return;

  dt_pthread_mutex_lock(&entry->cl_mem_lock);
  for(GList *l = entry->cl_mem_list; l; l = g_list_next(l))
  {
    dt_cache_clmem_t *c = (dt_cache_clmem_t *)l->data;
    if(c && c->mem == mem)
    {
      if(c->refs > 0) c->refs--;
      break;
    }
  }
  dt_pthread_mutex_unlock(&entry->cl_mem_lock);

#else
  (void)entry;
  (void)mem;
#endif
}

#ifdef HAVE_OPENCL
static void _pixel_cache_clmem_put(dt_pixel_cache_entry_t *entry, void *host_ptr, int devid,
                                   int width, int height, int bpp, int flags, void *mem)
{
  const int mem_devid = dt_opencl_get_mem_context_id((cl_mem)mem);
  if(mem_devid < 0 || mem_devid != devid)
  {
    dt_print(DT_DEBUG_OPENCL,
             "[pixelpipe_cache] refusing to cache cl_mem %p on recorded device %d (live device %d)\n",
             mem, devid, mem_devid);
    dt_opencl_release_mem_object((cl_mem)mem);
    return;
  }

  dt_pthread_mutex_lock(&entry->cl_mem_lock);
  for(GList *l = entry->cl_mem_list; l; l = g_list_next(l))
  {
    dt_cache_clmem_t *c = (dt_cache_clmem_t *)l->data;
    if(c->mem == mem)
    {
      dt_pthread_mutex_unlock(&entry->cl_mem_lock);
      return;
    }
    if(c->host_ptr == host_ptr && c->devid == devid && c->width == width && c->height == height
       && c->bpp == bpp && c->flags == flags)
    {
      if(c->refs > 0) continue;

      void *old = c->mem;
      c->mem = mem;
      dt_pthread_mutex_unlock(&entry->cl_mem_lock);
      dt_opencl_release_mem_object(old);
      return;
    }
  }

  dt_cache_clmem_t *c = (dt_cache_clmem_t *)g_malloc0(sizeof(*c));
  if(IS_NULL_PTR(c))
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
  c->mem = mem;
  entry->cl_mem_list = g_list_prepend(entry->cl_mem_list, c);
  dt_pthread_mutex_unlock(&entry->cl_mem_lock);
}

static void _pixel_cache_clmem_remove(dt_pixel_cache_entry_t *entry, void *mem)
{
  if(IS_NULL_PTR(entry) || IS_NULL_PTR(mem)) return;

  dt_pthread_mutex_lock(&entry->cl_mem_lock);
  for(GList *l = entry->cl_mem_list; l;)
  {
    GList *next = g_list_next(l);
    dt_cache_clmem_t *c = (dt_cache_clmem_t *)l->data;
    if(c && c->mem == mem)
    {
      if(c->refs > 0)
      {
        dt_pthread_mutex_unlock(&entry->cl_mem_lock);
        return;
      }

      entry->cl_mem_list = g_list_delete_link(entry->cl_mem_list, l);
      dt_free(c);
    }
    l = next;
  }
  dt_pthread_mutex_unlock(&entry->cl_mem_lock);
}
#endif

void dt_dev_pixelpipe_cache_flush_entry_clmem(dt_pixel_cache_entry_t *entry)
{
  dt_pthread_mutex_lock(&entry->cl_mem_lock);
  for(GList *l = entry->cl_mem_list; l;)
  {
    GList *next = g_list_next(l);
    dt_cache_clmem_t *c = (dt_cache_clmem_t *)l->data;
    if(c->refs > 0)
    {
      l = next;
      continue;
    }

    entry->cl_mem_list = g_list_delete_link(entry->cl_mem_list, l);
    dt_opencl_release_mem_object(c->mem);
    dt_free(c);
    l = next;
  }
  dt_pthread_mutex_unlock(&entry->cl_mem_lock);
}

#ifdef HAVE_OPENCL
void *dt_dev_pixelpipe_cache_get_pinned_image(dt_dev_pixelpipe_cache_t *cache, void *host_ptr,
                                              dt_pixel_cache_entry_t *entry_hint, int devid,
                                              int width, int height, int bpp, int flags,
                                              gboolean *out_reused)
{
  if(out_reused) *out_reused = FALSE;
  if(IS_NULL_PTR(cache) || IS_NULL_PTR(host_ptr) || devid < 0 || width <= 0 || height <= 0 || bpp <= 0) return NULL;

  dt_pixel_cache_entry_t *entry = entry_hint;
  if(IS_NULL_PTR(entry))
  {
    dt_pthread_mutex_lock(&cache->lock);
    entry = _cache_entry_for_host_ptr_locked(cache, host_ptr);
    dt_pthread_mutex_unlock(&cache->lock);
  }

  void *mem = NULL;
  if(entry)
  {
    mem = _pixel_cache_clmem_get(entry, host_ptr, devid, width, height, bpp, flags);
    if(mem && out_reused) *out_reused = TRUE;
  }

  if(IS_NULL_PTR(mem))
  {
    mem = dt_opencl_alloc_device_use_host_pointer(devid, width, height, bpp, host_ptr, flags);
    if(IS_NULL_PTR(mem)) return NULL;
    return mem;
  }

  const cl_mem clmem = (cl_mem)mem;
  const cl_mem_flags mem_flags = dt_opencl_get_mem_flags(clmem);
  gboolean synced = FALSE;

  if(mem_flags & CL_MEM_USE_HOST_PTR)
  {
    void *mapped = dt_opencl_map_image(devid, clmem, TRUE, CL_MAP_WRITE, width, height, bpp);
    if(mapped)
    {
      const gboolean zero_copy = (mapped == host_ptr);
      if(dt_opencl_unmap_mem_object(devid, clmem, mapped) == CL_SUCCESS)
      {
        dt_opencl_finish(devid);
        synced = zero_copy;
      }
    }
  }

  if(!synced && dt_opencl_write_host_to_device(devid, host_ptr, mem, width, height, bpp) != CL_SUCCESS)
  {
    if(entry) _pixel_cache_clmem_remove(entry, mem);
    dt_opencl_release_mem_object(mem);
    return NULL;
  }

  return mem;
}

void dt_dev_pixelpipe_cache_put_pinned_image(dt_dev_pixelpipe_cache_t *cache, void *host_ptr,
                                             dt_pixel_cache_entry_t *entry_hint, void **mem)
{
  if(IS_NULL_PTR(mem) || !*mem) return;

  dt_pixel_cache_entry_t *entry = entry_hint;
  if(IS_NULL_PTR(entry) && cache && host_ptr)
  {
    dt_pthread_mutex_lock(&cache->lock);
    entry = _cache_entry_for_host_ptr_locked(cache, host_ptr);
    dt_pthread_mutex_unlock(&cache->lock);
  }

  cl_mem clmem = (cl_mem)*mem;
  const cl_mem_flags flags = dt_opencl_get_mem_flags(clmem);
  const gboolean can_cache = (entry && host_ptr && (flags & CL_MEM_USE_HOST_PTR));
  if(can_cache)
  {
    const int devid = dt_opencl_get_mem_context_id(clmem);
    const int width = dt_opencl_get_image_width(clmem);
    const int height = dt_opencl_get_image_height(clmem);
    const int bpp = dt_opencl_get_image_element_size(clmem);
    _pixel_cache_clmem_put(entry, host_ptr, devid, width, height, bpp, (int)flags, clmem);
  }
  else
  {
    if(entry) _pixel_cache_clmem_remove(entry, clmem);
    dt_opencl_release_mem_object(clmem);
  }

  *mem = NULL;
}

gboolean dt_dev_pixelpipe_cache_flush_host_pinned_image(dt_dev_pixelpipe_cache_t *cache, void *host_ptr,
                                                        dt_pixel_cache_entry_t *entry_hint, int devid)
{
  if(IS_NULL_PTR(cache) || IS_NULL_PTR(host_ptr)) return FALSE;

  dt_pixel_cache_entry_t *entry = entry_hint;
  if(IS_NULL_PTR(entry))
  {
    dt_pthread_mutex_lock(&cache->lock);
    entry = _cache_entry_for_host_ptr_locked(cache, host_ptr);
    dt_pthread_mutex_unlock(&cache->lock);
  }

  if(IS_NULL_PTR(entry)) return FALSE;
  if(!_cache_entry_clmem_has_host_pinned_locked(entry, host_ptr, devid)) return FALSE;

  if(devid >= 0) dt_opencl_events_wait_for(devid);
  dt_dev_pixelpipe_cache_ref_count_entry(cache, TRUE, entry);
  const gboolean flushed = _cache_entry_clmem_flush_host_pinned_locked(entry, host_ptr, devid);
  dt_dev_pixelpipe_cache_ref_count_entry(cache, FALSE, entry);
  return flushed;
}

#else

void dt_dev_pixelpipe_cache_put_pinned_image(dt_dev_pixelpipe_cache_t *cache, void *host_ptr,
                                             dt_pixel_cache_entry_t *entry_hint, void **mem)
{
  (void)cache;
  (void)host_ptr;
  (void)entry_hint;
  if(mem) *mem = NULL;
}

gboolean dt_dev_pixelpipe_cache_flush_host_pinned_image(dt_dev_pixelpipe_cache_t *cache, void *host_ptr,
                                                        dt_pixel_cache_entry_t *entry_hint, int devid)
{
  (void)cache;
  (void)host_ptr;
  (void)entry_hint;
  (void)devid;
  return FALSE;
}

void dt_dev_pixelpipe_cache_resync_host_pinned_image(dt_dev_pixelpipe_cache_t *cache, void *host_ptr,
                                                     dt_pixel_cache_entry_t *entry_hint, int devid)
{
  (void)cache;
  (void)host_ptr;
  (void)entry_hint;
  (void)devid;
}
#endif

/**
 * @brief Determine whether a `CL_MEM_USE_HOST_PTR` OpenCL image is truly "zero-copy".
 *
 * @param devid OpenCL device index.
 * @param mem OpenCL image object (`cl_mem`).
 * @param host_ptr The CPU pointer we passed to `CL_MEM_USE_HOST_PTR`.
 * @param roi Image dimensions.
 * @param bpp Bytes per pixel.
 *
 * @return TRUE if the OpenCL driver mapped the image to the exact same `host_ptr` pointer, meaning
 *         the device is directly accessing the host memory (true zero-copy). FALSE otherwise.
 *
 * @details
 * Many drivers accept `CL_MEM_USE_HOST_PTR` but still create an internal device-side copy.
 * In that case, host memory is not automatically in sync with device memory, and explicit transfers
 * are required for correctness.
 *
 * We use a conservative runtime test:
 *
 * - map the image (blocking),
 * - compare the returned pointer with the original `host_ptr`,
 * - unmap and `clFinish` to ensure the unmap/synchronization completed.
 *
 * If the pointer matches, we treat the image as zero-copy and keep the cache entry locked while GPU
 * work is in flight.
 */
#ifdef HAVE_OPENCL
static gboolean _cl_is_zero_copy_image(const int devid, cl_mem mem, void *host_ptr, const dt_iop_roi_t *roi,
                                       const size_t bpp)
{
  if(devid < 0 || IS_NULL_PTR(mem) || IS_NULL_PTR(host_ptr) || IS_NULL_PTR(roi) || roi->width <= 0 || roi->height <= 0 || bpp == 0) return FALSE;

  void *mapped = dt_opencl_map_image(devid, mem, TRUE, CL_MAP_READ, roi->width, roi->height, (int)bpp);
  if(IS_NULL_PTR(mapped)) return FALSE;

  const gboolean ptr_matches = (mapped == host_ptr);
  const gboolean is_zero_copy = ptr_matches;
  const cl_int unmap_err = dt_opencl_unmap_mem_object(devid, mem, mapped);
  if(unmap_err != CL_SUCCESS) return FALSE;

  // Use clFinish rather than event wait: some drivers disable event tracking, but we still need to guarantee
  // the unmap (and implicit sync) is complete before touching host memory or unlocking the cache entry.
  dt_opencl_finish(devid);

  return is_zero_copy;
}

static inline gboolean _is_gamma_rgba8_output(const dt_iop_module_t *module, const size_t bpp,
                                              const char *message)
{
  return module && message && bpp == 4 * sizeof(uint8_t) && strcmp(module->op, "gamma") == 0
         && strcmp(message, "output") == 0;
}

/**
 * @brief Allocate a pure device-side OpenCL image, retrying once after flushing cached pinned buffers.
 *
 * @details
 * This is used when we intentionally do not want a pinned host-backed image (e.g. output buffers that we do
 * not plan to cache in RAM). Allocation failure triggers a clmem cache flush and one retry.
 *
 * @param keep OpenCL buffer to NOT flush, typically the input
 */
void *dt_dev_pixelpipe_cache_alloc_cl_device_buffer(int devid, const dt_iop_roi_t *roi, const size_t bpp,
                                                    const dt_iop_module_t *module, const char *message,
                                                    void *keep)
{
  const gboolean gamma_rgba8 = _is_gamma_rgba8_output(module, bpp, message);
  const int cl_bpp = gamma_rgba8 ? DT_OPENCL_BPP_ENCODE_RGBA8((int)bpp) : (int)bpp;
  void *mem = dt_opencl_alloc_device(devid, roi->width, roi->height, cl_bpp);
  if(IS_NULL_PTR(mem))
  {
    dt_dev_pixelpipe_cache_flush_clmem(darktable.pixelpipe_cache, devid, keep);
    mem = dt_opencl_alloc_device(devid, roi->width, roi->height, cl_bpp);
  }
  return mem;
}

/**
 * @brief Initialize an OpenCL buffer for the pixelpipe.
 *
 * @param devid OpenCL device index.
 * @param host_ptr If non-NULL, request a pinned host-backed image (`CL_MEM_USE_HOST_PTR`).
 * @param roi Image dimensions.
 * @param bpp Bytes per pixel.
 * @param module Module for debug messages.
 * @param message Human-readable context for debug messages.
 * @param cache_entry Pixelpipe cache entry owning `host_ptr`, used to reuse/categorize pinned allocations.
 * @param reuse_pinned If TRUE and `host_ptr` is non-NULL, attempt to reuse a cached pinned allocation.
 * @param reuse_device If TRUE and `host_ptr` is NULL, attempt to reuse a cached pure device allocation.
 * @param[out] out_reused Optional flag set to TRUE when the OpenCL image came from the cache.
 *
 * @return An OpenCL image (`cl_mem`) as a `void *`, or NULL on failure.
 *
 * @details
 * If `IS_NULL_PTR(host_ptr)`, we allocate a plain device image and rely on explicit copies when needed.
 * If `!IS_NULL_PTR(host_ptr)`, we allocate a pinned host-backed image, enabling (potentially) true zero-copy.
 */
void *dt_dev_pixelpipe_cache_get_cl_buffer(int devid, void *const host_ptr, const dt_iop_roi_t *roi,
                                           const size_t bpp, dt_iop_module_t *module,
                                           const char *message, dt_pixel_cache_entry_t *cache_entry,
                                           const gboolean reuse_pinned, const gboolean reuse_device,
                                           gboolean *out_reused, void *keep)
{
  // Need to use read-write mode because of in-place color space conversions.
  void *cl_mem_input = NULL;
  gboolean reused_from_cache = FALSE;
  const gboolean gamma_rgba8 = _is_gamma_rgba8_output(module, bpp, message);
  const int cl_bpp = gamma_rgba8 ? DT_OPENCL_BPP_ENCODE_RGBA8((int)bpp) : (int)bpp;

  if(out_reused) *out_reused = FALSE;

  if(host_ptr)
  {
    const int flags = CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR;

    if(reuse_pinned && cache_entry)
    {
      cl_mem_input = _pixel_cache_clmem_get(cache_entry, host_ptr, devid, roi->width, roi->height,
                                            (int)bpp, flags);
      reused_from_cache = (!IS_NULL_PTR(cl_mem_input));
    }

    if(IS_NULL_PTR(cl_mem_input))
      cl_mem_input = dt_opencl_alloc_device_use_host_pointer(devid, roi->width, roi->height, cl_bpp,
                                                             host_ptr, flags);

    if(IS_NULL_PTR(cl_mem_input))
    {
      dt_dev_pixelpipe_cache_flush_clmem(darktable.pixelpipe_cache, devid, NULL);
      if(reuse_pinned && cache_entry)
      {
        cl_mem_input = _pixel_cache_clmem_get(cache_entry, host_ptr, devid, roi->width, roi->height,
                                              (int)bpp, flags);
        reused_from_cache = (!IS_NULL_PTR(cl_mem_input));
      }
      if(IS_NULL_PTR(cl_mem_input))
        cl_mem_input = dt_opencl_alloc_device_use_host_pointer(devid, roi->width, roi->height, cl_bpp,
                                                               host_ptr, flags);
    }
  }
  else
  {
    if(reuse_device && cache_entry)
    {
      /* Device-only allocations are tracked with a NULL host_ptr key and a normalized READ_WRITE
       * flag so scratch buffers can be reused deterministically across drivers. */
      cl_mem_input = _pixel_cache_clmem_get(cache_entry, NULL, devid, roi->width, roi->height,
                                            (int)bpp, CL_MEM_READ_WRITE);
      reused_from_cache = (!IS_NULL_PTR(cl_mem_input));
    }

    if(IS_NULL_PTR(cl_mem_input))
      cl_mem_input = dt_dev_pixelpipe_cache_alloc_cl_device_buffer(devid, roi, bpp, module, message, keep);
  }

  if(IS_NULL_PTR(cl_mem_input))
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_pixelpipe] couldn't generate %s buffer for module %s\n", message,
             module ? module->op : "unknown");
  }
  else if(reuse_pinned && cache_entry && host_ptr)
  {
    static dt_atomic_int clmem_reuse_hits;
    static dt_atomic_int clmem_reuse_misses;
    if(out_reused) *out_reused = reused_from_cache;
    if(reused_from_cache)
    {
      const int hits = dt_atomic_add_int(&clmem_reuse_hits, 1) + 1;
      const int misses = dt_atomic_get_int(&clmem_reuse_misses);
      dt_print(DT_DEBUG_OPENCL,
               "[opencl_pixelpipe] %s reused pinned input from cache (hits=%d, misses=%d)\n",
               module ? module->name() : "unknown", hits, misses);
    }
    else
    {
      (void)dt_atomic_add_int(&clmem_reuse_misses, 1);
    }
  }
  else if(reuse_device && cache_entry && !host_ptr && out_reused)
  {
    *out_reused = reused_from_cache;
  }

  return cl_mem_input;
}
#endif

#ifdef HAVE_OPENCL
/**
 * @brief Release or cache an OpenCL image associated with a host cache line.
 *
 * @param[in,out] cl_mem_buffer Pointer to a `cl_mem` stored as `void*`.
 * @param cache_entry Pixelpipe cache entry the host pointer belongs to (may be NULL).
 * @param host_ptr Host pointer backing the OpenCL image (may be NULL).
 * @param cache_device Allow caching pure device-side buffers for scratch reuse.
 *
 * @details
 * This helper is a *single point of truth* for OpenCL image lifetime management in the pixelpipe:
 *
 * - If the image is host-backed (`CL_MEM_USE_HOST_PTR`) and we have both `cache_entry` and `host_ptr`,
 *   we put it in the cache entry's `cl_mem_list` for reuse.
 * - Otherwise, we release it immediately.
 * - Pure device allocations may also be cached for scratch-pad reuse. When those cached `cl_mem`
 *   objects are later evicted, the cache layer is responsible for materializing host RAM first if
 *   no authoritative host buffer exists yet.
 *
 * Additionally, when we release an image, we must ensure there is no stale pointer in `cl_mem_list`
 * (for example, if some earlier path cached it and we are now deciding to free it). We call
 * `_pixel_cache_clmem_remove()` before releasing to keep the cache bookkeeping coherent.
 */
void dt_dev_pixelpipe_cache_release_cl_buffer(void **cl_mem_buffer, dt_pixel_cache_entry_t *cache_entry,
                                              void *host_ptr, const gboolean cache_device)
{
  if(cl_mem_buffer && !IS_NULL_PTR(*cl_mem_buffer))
  {
    cl_mem mem = *cl_mem_buffer;
    const cl_mem_flags flags = dt_opencl_get_mem_flags(mem);
    const gboolean can_cache_pinned = (cache_entry && host_ptr && (flags & CL_MEM_USE_HOST_PTR));
    const gboolean can_cache_device = (cache_entry && !host_ptr && cache_device
                                       && !(flags & CL_MEM_USE_HOST_PTR));
    const gboolean can_cache = (can_cache_pinned || can_cache_device);
    if(can_cache)
    {
      const int devid = dt_opencl_get_mem_context_id(mem);
      const int width = dt_opencl_get_image_width(mem);
      const int height = dt_opencl_get_image_height(mem);
      const int bpp = dt_opencl_get_image_element_size(mem);
      const int tracked_flags = can_cache_device ? CL_MEM_READ_WRITE : (int)flags;
      _pixel_cache_clmem_put(cache_entry, host_ptr, devid, width, height, bpp, tracked_flags, mem);
    }
    else
    {
      if(cache_entry) _pixel_cache_clmem_remove(cache_entry, mem);
      dt_opencl_release_mem_object(mem);
    }
    *cl_mem_buffer = NULL;
  }
}

/**
 * @brief Synchronize between host memory and a pinned OpenCL image.
 *
 * @param devid OpenCL device index.
 * @param host_ptr Host pointer to read from / write to.
 * @param cl_mem_buffer OpenCL image.
 * @param roi Image dimensions.
 * @param cl_mode `CL_MAP_WRITE` for host→device, `CL_MAP_READ` for device→host.
 * @param bpp Bytes per pixel.
 * @param module Module for debug logs (may be NULL).
 * @param message Context string for debug logs.
 *
 * @return 0 on success, 1 on failure.
 *
 * @details
 * This function intentionally tries a hierarchy of synchronization mechanisms:
 *
 * 1. For `CL_MEM_USE_HOST_PTR` images, we *attempt* a map/unmap cycle. If the mapped pointer equals `host_ptr`,
 *    we treat it as true zero-copy and the map/unmap acts as a synchronization barrier (fast, avoids extra copies).
 * 2. Otherwise, we fall back to explicit blocking transfers (`dt_opencl_write_host_to_device` /
 *    `dt_opencl_read_host_from_device`), which already guarantee that the copied pixels are visible
 *    on the host/device side when the helper returns.
 *
 * The map/unmap approach is used as a synchronization barrier because on many drivers it will:
 *
 * - flush CPU caches / invalidate as needed,
 * - ensure GPU work touching that memory is completed (for blocking map),
 * - and potentially avoid a full copy when true zero-copy is supported.
 */
int dt_dev_pixelpipe_cache_sync_cl_buffer(const int devid, void *host_ptr, void *cl_mem_buffer,
                                          const dt_iop_roi_t *roi, int cl_mode, size_t bpp,
                                          dt_iop_module_t *module, const char *message)
{
  if(IS_NULL_PTR(host_ptr) || IS_NULL_PTR(cl_mem_buffer)) return 1;

  const cl_mem mem = (cl_mem)cl_mem_buffer;
  const cl_mem_flags flags = dt_opencl_get_mem_flags(mem);

  // Fast path for true zero-copy pinned images: map/unmap is enough to synchronize host<->device.
  if(flags & CL_MEM_USE_HOST_PTR)
  {
    void *mapped = dt_opencl_map_image(devid, mem, TRUE, cl_mode, roi->width, roi->height, (int)bpp);
    if(mapped)
    {
      const gboolean ptr_matches = (mapped == host_ptr);
      const cl_int unmap_err = dt_opencl_unmap_mem_object(devid, mem, mapped);
      if(unmap_err != CL_SUCCESS) return 1;

      // Ensure unmap (and any implicit sync) completed before we possibly enqueue explicit transfers.
      // When event tracking is disabled, clFinish is the only reliable barrier.
      dt_opencl_finish(devid);

      if(ptr_matches)
      {
        dt_print(DT_DEBUG_OPENCL,
                 "[opencl_pixelpipe] successfully synced image %s via map/unmap for module %s (%s)\n",
                 (cl_mode == CL_MAP_WRITE) ? "host to device" : "device to host",
                 (module) ? module->op : "base buffer", message);
        return 0;
      }
    }
  }

  // Fallback: explicit blocking transfers (safe on all drivers).
  cl_int err = CL_SUCCESS;
  if(cl_mode == CL_MAP_WRITE)
    err = dt_opencl_write_host_to_device(devid, host_ptr, mem, roi->width, roi->height, (int)bpp);
  else if(cl_mode == CL_MAP_READ)
    err = dt_opencl_read_host_from_device(devid, host_ptr, mem, roi->width, roi->height, (int)bpp);
  else
    return 1;

  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_pixelpipe] couldn't copy image %s for module %s (%s)\n",
             (cl_mode == CL_MAP_WRITE) ? "host to device" : "device to host",
             (module) ? module->op : "base buffer", message);
    return 1;
  }

  dt_print(DT_DEBUG_OPENCL, "[opencl_pixelpipe] successfully copied image %s for module %s (%s)\n",
           (cl_mode == CL_MAP_WRITE) ? "host to device" : "device to host",
           (module) ? module->op : "base buffer", message);
  return 0;
}

/**
 * @brief Force device → host resynchronization of the pixelpipe input cache line.
 *
 * @details
 * This is used when we are about to switch from GPU processing to CPU processing for a given module.
 * In that scenario, the most recent correct pixels may only exist in `cl_mem_input` (GPU-only intermediate),
 * while `input` (host pointer) is either NULL or stale.
 *
 * The function:
 *
 * - write-locks the cache entry (we are modifying host memory),
 * - performs a device→host copy (map/unmap if possible, explicit copy otherwise),
 * - updates the buffer descriptor colorspace tag.
 */
float *dt_dev_pixelpipe_cache_restore_cl_buffer(dt_dev_pixelpipe_t *pipe, float *input, void *cl_mem_input,
                                                const dt_iop_roi_t *roi_in, dt_iop_module_t *module,
                                                const size_t in_bpp, dt_pixel_cache_entry_t *input_entry,
                                                const char *message)
{
  if(IS_NULL_PTR(cl_mem_input)) return input;
  dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, TRUE, input_entry);

  const int fail = dt_dev_pixelpipe_cache_sync_cl_buffer(pipe->devid, input, cl_mem_input, roi_in,
                                                         CL_MAP_READ, in_bpp, module, message);
  dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, FALSE, input_entry);
  return fail ? NULL : input;
}

/**
 * @brief Prepare/obtain the OpenCL input image for a module.
 *
 * @param pipe Current pixelpipe (provides device id + global settings).
 * @param module Module being processed (for debug logs).
 * @param input Host input pointer (may be NULL on GPU-only paths).
 * @param[in,out] cl_mem_input OpenCL input image; may already be set by the previous module.
 * @param roi_in ROI for the input buffer.
 * @param in_bpp Input bytes-per-pixel.
 * @param input_entry Pixelpipe cache entry corresponding to the input hash.
 * @param[out] locked_input_entry If non-NULL on return, the caller must unlock it after GPU work completed.
 * @param keep OpenCL buffer to keep alive while flushing device scratch allocations.
 *
 * @return 0 on success, 1 on failure.
 *
 * @details
 * There are two major cases:
 *
 * 1) `!IS_NULL_PTR(*cl_mem_input)`:
 *    The previous module already produced an OpenCL buffer and we are continuing on GPU. We may still need to
 *    keep the cache entry write-locked if it is a true zero-copy pinned image, because in-place OpenCL
 *    colorspace transforms can mutate the host-backed buffer before the current module runs.
 *
 * 2) `IS_NULL_PTR(*cl_mem_input)`:
 *    We start from a host cache buffer (`input`). We allocate (or reuse) a pinned image backed by that host buffer,
 *    and if it is not true zero-copy we push host→device once before running kernels.
 */
int dt_dev_pixelpipe_cache_prepare_cl_input(dt_dev_pixelpipe_t *pipe, dt_iop_module_t *module,
                                            float *input, void **cl_mem_input,
                                            const dt_iop_roi_t *roi_in, const size_t in_bpp,
                                            dt_pixel_cache_entry_t *input_entry,
                                            dt_pixel_cache_entry_t **locked_input_entry, void *keep)
{
  if(IS_NULL_PTR(locked_input_entry)) return 1;
  *locked_input_entry = NULL;

  if(!IS_NULL_PTR(*cl_mem_input))
  {
    // We passed the OpenCL memory buffer through directly on vRAM from previous module.
    // This is fast and efficient.
    // If it's a true zero-copy pinned image, keep the input cache entry read-locked until kernels complete,
    // otherwise another thread may overwrite host memory while the GPU is still reading it.
    dt_print(DT_DEBUG_OPENCL, "[dev_pixelpipe] %s will use its input directly from vRAM\n", module->name());
    const cl_mem mem = (cl_mem)*cl_mem_input;
    const cl_mem_flags flags = dt_opencl_get_mem_flags(mem);
    if(flags & CL_MEM_USE_HOST_PTR)
      if(_cl_is_zero_copy_image(pipe->devid, mem, input, roi_in, in_bpp))
      {
        dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, TRUE, input_entry);
        *locked_input_entry = input_entry;
      }
    return 0;
  }

  if(IS_NULL_PTR(input))
  {
    dt_print(DT_DEBUG_OPENCL, "[dev_pixelpipe] %s has no input (cache)\n", module->name());
    return 1;
  }

  dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, TRUE, input_entry);

  // Try to reuse a cached pinned buffer; otherwise allocate a new pinned image backed by `input`.
  gboolean input_reused_from_cache = FALSE;
  *cl_mem_input = dt_dev_pixelpipe_cache_get_cl_buffer(pipe->devid, input, roi_in, in_bpp, module,
                                                       "input", input_entry, TRUE, FALSE,
                                                       &input_reused_from_cache, keep);
  int fail = (IS_NULL_PTR(*cl_mem_input));

  // If the input is true zero-copy, the GPU will access host memory asynchronously: keep the cache
  // entry read-locked until all kernels have completed. If not, drivers may use a device-side copy
  // which must be synchronized from the host before running kernels.
  gboolean keep_lock = FALSE;
  cl_mem mem = NULL;
  if(!fail && *cl_mem_input)
  {
    mem = (cl_mem)*cl_mem_input;
    const cl_mem_flags flags = dt_opencl_get_mem_flags(mem);
    if(flags & CL_MEM_USE_HOST_PTR)
      keep_lock = _cl_is_zero_copy_image(pipe->devid, mem, input, roi_in, in_bpp);
  }

  /* A reused cached pinned image already carries the authoritative device payload from the
   * previous module output. Re-uploading host RAM here would overwrite that valid vRAM state
   * with whatever stale contents the host buffer still has when the previous stage stayed GPU-only.
   * Only freshly allocated pinned inputs need an explicit host->device copy. */
  if(!fail && mem && !keep_lock && !input_reused_from_cache)
  {
    const cl_int err = dt_opencl_write_host_to_device(pipe->devid, input, mem, roi_in->width, roi_in->height,
                                                      (int)in_bpp);
    if(err != CL_SUCCESS)
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_pixelpipe] couldn't copy image host to device for module %s (%s)\n",
               (module) ? module->op : "base buffer", "cache to input");
      fail = TRUE;
    }
    else
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_pixelpipe] successfully copied image host to device for module %s (%s)\n",
               (module) ? module->op : "base buffer", "cache to input");
    }
  }

  // Enforce sync with the CPU/RAM cache so lock validity is guaranteed.
  dt_opencl_events_wait_for(pipe->devid);

  if(keep_lock)
    *locked_input_entry = input_entry;
  else
    dt_dev_pixelpipe_cache_rdlock_entry(darktable.pixelpipe_cache, FALSE, input_entry);

  return fail ? 1 : 0;
}
#else
void *dt_dev_pixelpipe_cache_get_cl_buffer(int devid, void *host_ptr, const dt_iop_roi_t *roi,
                                           size_t bpp, dt_iop_module_t *module, const char *message,
                                           dt_pixel_cache_entry_t *entry, gboolean reuse_pinned,
                                           gboolean reuse_device, gboolean *out_reused, void *keep)
{
  (void)devid;
  (void)host_ptr;
  (void)roi;
  (void)bpp;
  (void)module;
  (void)message;
  (void)entry;
  (void)reuse_pinned;
  (void)reuse_device;
  (void)keep;
  if(out_reused) *out_reused = FALSE;
  return NULL;
}

void *dt_dev_pixelpipe_cache_alloc_cl_device_buffer(int devid, const dt_iop_roi_t *roi, size_t bpp,
                                                    const dt_iop_module_t *module,
                                                    const char *message, void *keep)
{
  (void)devid;
  (void)roi;
  (void)bpp;
  (void)module;
  (void)message;
  (void)keep;
  return NULL;
}

void dt_dev_pixelpipe_cache_release_cl_buffer(void **cl_mem_buffer, dt_pixel_cache_entry_t *entry,
                                              void *host_ptr, gboolean cache_device)
{
  (void)entry;
  (void)host_ptr;
  (void)cache_device;
  if(cl_mem_buffer) *cl_mem_buffer = NULL;
}

int dt_dev_pixelpipe_cache_sync_cl_buffer(int devid, void *host_ptr, void *cl_mem_buffer,
                                          const dt_iop_roi_t *roi, int cl_mode, size_t bpp,
                                          dt_iop_module_t *module, const char *message)
{
  (void)devid;
  (void)host_ptr;
  (void)cl_mem_buffer;
  (void)roi;
  (void)cl_mode;
  (void)bpp;
  (void)module;
  (void)message;
  return 1;
}

float *dt_dev_pixelpipe_cache_restore_cl_buffer(dt_dev_pixelpipe_t *pipe, float *input,
                                                void *cl_mem_input, const dt_iop_roi_t *roi_in,
                                                dt_iop_module_t *module, size_t in_bpp,
                                                dt_pixel_cache_entry_t *input_entry,
                                                const char *message)
{
  (void)pipe;
  (void)cl_mem_input;
  (void)roi_in;
  (void)module;
  (void)in_bpp;
  (void)input_entry;
  (void)message;
  return input;
}

int dt_dev_pixelpipe_cache_prepare_cl_input(dt_dev_pixelpipe_t *pipe, dt_iop_module_t *module,
                                            float *input, void **cl_mem_input,
                                            const dt_iop_roi_t *roi_in, size_t in_bpp,
                                            dt_pixel_cache_entry_t *input_entry,
                                            dt_pixel_cache_entry_t **locked_input_entry,
                                            void *keep)
{
  (void)pipe;
  (void)module;
  (void)input;
  (void)cl_mem_input;
  (void)roi_in;
  (void)in_bpp;
  (void)input_entry;
  (void)locked_input_entry;
  (void)keep;
  return 1;
}
#endif

dt_pixel_cache_entry_t *dt_dev_pixelpipe_cache_ref_entry_for_host_ptr(dt_dev_pixelpipe_cache_t *cache,
                                                                      void *host_ptr)
{
  if(IS_NULL_PTR(cache) || IS_NULL_PTR(host_ptr)) return NULL;

  dt_pthread_mutex_lock(&cache->lock);
  dt_pixel_cache_entry_t *entry = _cache_entry_for_host_ptr_locked(cache, host_ptr);
  if(entry)
    _non_thread_safe_cache_ref_count_entry(cache, TRUE, entry);
  dt_pthread_mutex_unlock(&cache->lock);

  return entry;
}

// Attempt to allocate from the arena; if fragmentation prevents it, evict LRU cache lines
// until a sufficiently large contiguous run is available (or nothing remains to evict).
static inline void *_arena_alloc_with_defrag(dt_dev_pixelpipe_cache_t *cache, size_t request_size,
                                             size_t *actual_size)
{
  void *buf = dt_cache_arena_alloc(&cache->arena, request_size, actual_size);
  if(buf) return buf;

  uint32_t pages_needed = 0;
  if(dt_cache_arena_calc(&cache->arena, request_size, &pages_needed, NULL))
  {
    dt_pthread_mutex_lock(&cache->lock);
    uint32_t total_free_pages = 0, largest_free_run_pages = 0;
    dt_cache_arena_stats(&cache->arena, &total_free_pages, &largest_free_run_pages);

    while(largest_free_run_pages < pages_needed && g_hash_table_size(cache->entries) > 0)
    {
      if(_non_thread_safe_pixel_pipe_cache_remove_lru(cache)) break;
      dt_cache_arena_stats(&cache->arena, &total_free_pages, &largest_free_run_pages);
    }
    dt_pthread_mutex_unlock(&cache->lock);
  }

  return dt_cache_arena_alloc(&cache->arena, request_size, actual_size);
}

static inline void _arena_stats_bytes(dt_dev_pixelpipe_cache_t *cache, uint32_t *total_pages,
                                      uint32_t *largest_pages, size_t *total_bytes, size_t *largest_bytes)
{
  dt_cache_arena_stats(&cache->arena, total_pages, largest_pages);
  const size_t page_size = cache->arena.page_size ? cache->arena.page_size : 1;
  if(total_bytes) *total_bytes = (size_t)(*total_pages) * page_size;
  if(largest_bytes) *largest_bytes = (size_t)(*largest_pages) * page_size;
}

static inline void _log_arena_allocation_failure(dt_dev_pixelpipe_cache_t *cache, size_t request_size,
                                                 const char *entry_name, const char *module, uint64_t hash,
                                                 gboolean name_is_file)
{
  uint32_t total_free_pages = 0, largest_free_run_pages = 0;
  size_t total_free_bytes = 0, largest_free_bytes = 0;
  _arena_stats_bytes(cache, &total_free_pages, &largest_free_run_pages, &total_free_bytes, &largest_free_bytes);

  if(entry_name)
    fprintf(stdout,
            "[pixelpipe_cache] failed to allocate %zu bytes for entry %" PRIu64 " (%s, module=%s) "
            "[arena largest=%zu MiB, total=%zu MiB, cache=%zu/%zu MiB]\n",
            request_size, hash, entry_name, module ? module : "unknown",
            largest_free_bytes / (1024 * 1024), total_free_bytes / (1024 * 1024),
            cache->current_memory / (1024 * 1024), cache->max_memory / (1024 * 1024));
  else
    fprintf(stdout,
            "[pixelpipe_cache] failed to allocate %zu bytes for entry %" PRIu64 " (module=%s) "
            "[arena largest=%zu MiB, total=%zu MiB, cache=%zu/%zu MiB]\n",
            request_size, hash, module ? module : "unknown",
            largest_free_bytes / (1024 * 1024), total_free_bytes / (1024 * 1024),
            cache->current_memory / (1024 * 1024), cache->max_memory / (1024 * 1024));

  if(entry_name && module)
    dt_control_log(_("The pipeline cache is full while allocating `%s` (module `%s`). Either your RAM settings are too frugal or your RAM is too small."),
                   entry_name, module);
  else if(entry_name)
    dt_control_log(_("The pipeline cache is full while allocating `%s`. Either your RAM settings are too frugal or your RAM is too small."),
                   entry_name);
  else if(module)
    dt_control_log(_("The pipeline cache is full while processing module `%s`. Either your RAM settings are too frugal or your RAM is too small."),
                   module);
  else
    dt_control_log(_("The pipeline cache is full. Either your RAM settings are too frugal or your RAM is too small."));

  (void)name_is_file; // kept for signature symmetry if future callers need it.
}

// keep: OpenCL buffer to NOT release
#ifdef HAVE_OPENCL
static void _cache_entry_clmem_flush_device(dt_pixel_cache_entry_t *entry, const int devid, void *keep)
{
  dt_pthread_mutex_lock(&entry->cl_mem_lock);
  GList *l = entry->cl_mem_list;
  while(l)
  {
    GList *next = g_list_next(l);
    dt_cache_clmem_t *c = (dt_cache_clmem_t *)l->data;
    const int mem_devid = (c && c->mem) ? dt_opencl_get_mem_context_id((cl_mem)c->mem) : -1;
    if(c && c->mem && mem_devid != c->devid)
    {
      l = next;
      continue;
    }

    if(c && (devid < 0 || (c->devid == devid && c->mem != keep)))
    {
      /* Device-only payloads on hostless cache entries are the authoritative pixels until some
       * downstream CPU stage materializes RAM or the entry gets destroyed. A global OpenCL flush
       * may run between 2 modules of the same thumbnail batch, so only looking at `c->refs` is
       * not enough here: a published entry can be live through its cache refcount while its
       * device payload has not been borrowed yet by the next module. Flushing it in that window
       * turns a valid GPU-only intermediate into an empty cacheline that later aborts on consume. */
      if(IS_NULL_PTR(c->host_ptr) && IS_NULL_PTR(entry->data) && dt_atomic_get_int(&entry->refcount) > 0)
      {
        l = next;
        continue;
      }

      if(c->refs > 0)
      {
        l = next;
        continue;
      }

      entry->cl_mem_list = g_list_delete_link(entry->cl_mem_list, l);
      dt_opencl_release_mem_object(c->mem);
      dt_free(c);
    }
    l = next;
  }
  dt_pthread_mutex_unlock(&entry->cl_mem_lock);
}
#else 
static void _cache_entry_clmem_flush_device(dt_pixel_cache_entry_t *entry, const int devid, void *keep)
{
  return;
}
#endif

void *dt_pixel_cache_alloc(dt_dev_pixelpipe_cache_t *cache, dt_pixel_cache_entry_t *cache_entry)
{
  // allocate the data buffer
  if(IS_NULL_PTR(cache_entry->data))
  {
    cache_entry->data = _arena_alloc_with_defrag(cache, cache_entry->size, &cache_entry->size);

    if(IS_NULL_PTR(cache_entry->data))
    {
      const char *module = dt_pixelpipe_cache_current_module;
      _log_arena_allocation_failure(cache, cache_entry->size, cache_entry->name, module,
                                    cache_entry->hash, FALSE);
    }
  }

  return cache_entry->data;
}

void *dt_pixel_cache_entry_get_data(dt_pixel_cache_entry_t *entry)
{
  return entry ? entry->data : NULL;
}

size_t dt_pixel_cache_entry_get_size(dt_pixel_cache_entry_t *entry)
{
  return entry ? entry->size : 0;
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
    const gboolean name_is_file = (!IS_NULL_PTR(name)) && (strchr(name, '/') != NULL) && (strchr(name, ':') != NULL);
    if(IS_NULL_PTR(name)) name = g_strdup("unknown");
    
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
  void *buf = _arena_alloc_with_defrag(cache, size, &page_size);

  if(IS_NULL_PTR(buf))
  {
    _log_arena_allocation_failure(cache, size, name, NULL, 0, FALSE);
    return NULL;
  }

  void *aligned = __builtin_assume_aligned(buf, DT_CACHELINE_BYTES);

  const uint64_t hash = (uint64_t)(uintptr_t)(aligned);

  dt_pthread_mutex_lock(&cache->lock);
  dt_pixel_cache_entry_t *cache_entry
      = dt_pixel_cache_new_entry(hash, page_size, name, id, cache, FALSE, cache->external_entries);

  if(IS_NULL_PTR(cache_entry))
  {
    dt_pthread_mutex_unlock(&cache->lock);
    dt_cache_arena_free(&cache->arena, buf, page_size);
    return NULL;
  }

  // Keep this entry marked as "used" for diagnostics/bookkeeping.
  // Note that external_entries are not subject to LRU eviction, so we must not keep
  // a thread-owned rwlock held across the lifetime of the buffer (it may be freed
  // from a different thread during cleanup paths).
  _non_thread_safe_cache_ref_count_entry(cache, TRUE, cache_entry);
  cache_entry->data = aligned;
  cache_entry->age = g_get_monotonic_time();
  cache_entry->external_alloc = TRUE;
  dt_pthread_mutex_unlock(&cache->lock);
  return aligned;
}

void dt_pixelpipe_cache_free_align_cache(dt_dev_pixelpipe_cache_t *cache, void **mem, const char *message)
{
  if(IS_NULL_PTR(mem) || !*mem) return;

  dt_pthread_mutex_lock(&cache->lock);
  const uint64_t hash = (uint64_t)(uintptr_t)(*mem);
  dt_pixel_cache_entry_t *cache_entry = _non_threadsafe_cache_get_entry(cache, cache->external_entries, hash);
  if(IS_NULL_PTR(cache_entry) || !cache_entry->external_alloc)
  {
    dt_pthread_mutex_unlock(&cache->lock);
    fprintf(stdout, "error while freeing cache entry: no entry found but we have a buffer, %s.\n", message);
    raise(SIGSEGV); // triggers dt_set_signal_handlers() backtrace on Unix
    return;
  }

  _non_thread_safe_cache_ref_count_entry(cache, FALSE, cache_entry);
  g_hash_table_remove(cache->external_entries, &cache_entry->hash);
  *mem = NULL;

  dt_pthread_mutex_unlock(&cache->lock);
}


// WARNING: not thread-safe, protect its calls with mutex lock
static dt_pixel_cache_entry_t *dt_pixel_cache_new_entry(const uint64_t hash, const size_t size,
                                                        const char *name, const int id,
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
  if(IS_NULL_PTR(cache_entry)) return NULL;

  // Metadata, easy to free in batch if need be
  cache_entry->size = rounded_size;
  cache_entry->age = 0;
  cache_entry->hits = 0;
  cache_entry->hash = hash;
  cache_entry->serial = cache->next_serial++;
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

  if(alloc && IS_NULL_PTR(cache_entry->data))
  {
    dt_free(cache_entry);
    return NULL;
  }
  
  // Metadata that need alloc
  cache_entry->name = g_strdup(name);
  dt_pthread_rwlock_init(&cache_entry->lock, NULL);

  uint64_t *key = g_malloc(sizeof(*key));
  if(IS_NULL_PTR(key))
  {
    dt_pthread_rwlock_destroy(&cache_entry->lock);
    dt_free(cache_entry->name);
    dt_pthread_mutex_destroy(&cache_entry->cl_mem_lock);
    dt_free(cache_entry);
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
  _pixel_cache_message(cache_entry, "freed", FALSE);

  if(cache_entry->data)
  {
#ifdef HAVE_OPENCL
    dt_pthread_mutex_lock(&cache_entry->cl_mem_lock);
    for(GList *l = cache_entry->cl_mem_list; l; l = g_list_next(l))
    {
      dt_cache_clmem_t *c = (dt_cache_clmem_t *)l->data;
      if(IS_NULL_PTR(c) || c->host_ptr != cache_entry->data) continue;
      
      /* Host-backed OpenCL images may still dereference `cache_entry->data` asynchronously until their
        * queued work completes. We therefore wait for the owning device before releasing the host arena slot,
        * otherwise an auto-destroyed intermediate can be recycled into another module output while the GPU
        * is still reading the previous pixels. */
      const int mem_devid = dt_opencl_get_mem_context_id((cl_mem)c->mem);
      dt_opencl_finish(mem_devid >= 0 ? mem_devid : c->devid);
    }
    dt_pthread_mutex_unlock(&cache_entry->cl_mem_lock);
#endif

    dt_dev_pixelpipe_cache_flush_entry_clmem(cache_entry);
    dt_cache_arena_free(&cache_entry->cache->arena, cache_entry->data, cache_entry->size);
  }
  else
  {
    dt_dev_pixelpipe_cache_flush_entry_clmem(cache_entry);
  }

  cache_entry->data = NULL;
  cache_entry->cache->current_memory -= cache_entry->size;
  dt_pthread_rwlock_destroy(&cache_entry->lock);
  dt_pthread_mutex_destroy(&cache_entry->cl_mem_lock);
  dt_free(cache_entry->name);
  dt_free(cache_entry);
}

static int garbage_collection = 0;

static gboolean _cache_entry_has_device_payload(dt_pixel_cache_entry_t *cache_entry,
                                                const int preferred_devid);

dt_dev_pixelpipe_cache_t * dt_dev_pixelpipe_cache_init(size_t max_memory)
{
  dt_dev_pixelpipe_cache_t *cache = (dt_dev_pixelpipe_cache_t *)malloc(sizeof(dt_dev_pixelpipe_cache_t));
  dt_pthread_mutex_init(&cache->lock, NULL);
  cache->entries = g_hash_table_new_full(g_int64_hash, g_int64_equal, dt_free_gpointer, (GDestroyNotify)_free_cache_entry);
  cache->external_entries = g_hash_table_new_full(g_int64_hash, g_int64_equal, dt_free_gpointer, (GDestroyNotify)_free_cache_entry);
  cache->max_memory = max_memory;
  cache->current_memory = 0;
  cache->next_serial = 1;
  cache->queries = cache->hits = 0;

  if(IS_NULL_PTR(cache->entries) || IS_NULL_PTR(cache->external_entries))
  {
    if(cache->entries) g_hash_table_destroy(cache->entries);
    if(cache->external_entries) g_hash_table_destroy(cache->external_entries);
    dt_pthread_mutex_destroy(&cache->lock);
    dt_free(cache);
    return NULL;
  }

  if(dt_cache_arena_init(&cache->arena, cache->max_memory))
  {
    dt_pthread_mutex_destroy(&cache->lock);
    g_hash_table_destroy(cache->external_entries);
    g_hash_table_destroy(cache->entries);
    dt_free(cache);
    return NULL;
  }

  // Run every 3 minutes
  garbage_collection = g_timeout_add(3 * 60 * 1000, (GSourceFunc)dt_dev_pixelpipe_cache_flush_old, cache);
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
                                                                    const char *name, const int id)
{
  dt_pixel_cache_entry_t *cache_entry = dt_pixel_cache_new_entry(hash, size, name, id, cache, FALSE, cache->entries);
  if(IS_NULL_PTR(cache_entry)) return NULL;

  // Increase ref_count, consumer will have to decrease it
  _non_thread_safe_cache_ref_count_entry(cache, TRUE, cache_entry);

  // Acquire write lock so caller can populate data safely
  dt_dev_pixelpipe_cache_wrlock_entry(cache, TRUE, cache_entry);

  return cache_entry;
}

static dt_pixel_cache_entry_t *_cache_try_rekey_reuse_locked(dt_dev_pixelpipe_cache_t *cache,
                                                             const uint64_t new_hash, const size_t size,
                                                             const dt_pixel_cache_entry_t *reuse_hint)
{
  if(IS_NULL_PTR(cache) || IS_NULL_PTR(reuse_hint)) return NULL;

  const uint64_t old_hash = reuse_hint->hash;
  if(old_hash == DT_PIXELPIPE_CACHE_HASH_INVALID || old_hash == new_hash) return NULL;
  if(reuse_hint->size < size) return NULL;

  dt_pixel_cache_entry_t *cache_entry = _non_threadsafe_cache_get_entry(cache, cache->entries, old_hash);
  if(IS_NULL_PTR(cache_entry)) return NULL;
  if(cache_entry->serial != reuse_hint->serial) return NULL;
  if(cache_entry->auto_destroy) return NULL;
  if(cache_entry->size < size) return NULL;
  if(_non_threadsafe_cache_get_entry(cache, cache->entries, new_hash)) return NULL;

  _non_thread_safe_cache_ref_count_entry(cache, TRUE, cache_entry);
  dt_dev_pixelpipe_cache_wrlock_entry(cache, TRUE, cache_entry);

  /* Rekey reuse transfers the RAM arena slot to a completely different hash. Any cached OpenCL payload
   * still attached to the previous owner would otherwise remain reachable through the new hash and could
   * later be materialized as if it belonged to the new module output. Bail out if some GPU path is still
   * borrowing one of those payloads, otherwise flush the stale bookkeeping before publishing the new hash. */
  dt_pthread_mutex_lock(&cache_entry->cl_mem_lock);
  for(GList *l = cache_entry->cl_mem_list; l; l = g_list_next(l))
  {
    dt_cache_clmem_t *c = (dt_cache_clmem_t *)l->data;
    if(c && c->refs > 0)
    {
      dt_pthread_mutex_unlock(&cache_entry->cl_mem_lock);
      dt_dev_pixelpipe_cache_wrlock_entry(cache, FALSE, cache_entry);
      dt_dev_pixelpipe_cache_ref_count_entry(cache, FALSE, cache_entry);
      return NULL;
    }
  }
  dt_pthread_mutex_unlock(&cache_entry->cl_mem_lock);
  dt_dev_pixelpipe_cache_flush_entry_clmem(cache_entry);

  gpointer stolen_key = NULL;
  gpointer stolen_value = NULL;
  if(!g_hash_table_steal_extended(cache->entries, &old_hash, &stolen_key, &stolen_value)
     || stolen_value != cache_entry)
  {
    if(stolen_key && stolen_value) g_hash_table_insert(cache->entries, stolen_key, stolen_value);
    dt_dev_pixelpipe_cache_wrlock_entry(cache, FALSE, cache_entry);
    dt_dev_pixelpipe_cache_ref_count_entry(cache, FALSE, cache_entry);
    return NULL;
  }

  *(uint64_t *)stolen_key = new_hash;
  cache_entry->hash = new_hash;
  g_hash_table_insert(cache->entries, stolen_key, cache_entry);

  dt_print(DT_DEBUG_PIPECACHE,
           "[pixelpipe_cache] writable rekey old=%" PRIu64 " new=%" PRIu64 " entry=%" PRIu64 "/%" PRIu64
           " refs=%i auto=%i data=%p module=%s\n",
           old_hash, new_hash, cache_entry->hash, cache_entry->serial,
           dt_atomic_get_int(&cache_entry->refcount), cache_entry->auto_destroy, cache_entry->data,
           _cache_debug_module_name());
  return cache_entry;
}


int dt_dev_pixelpipe_cache_get(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash,
                               const size_t size, const char *name, const int id,
                               const gboolean alloc, void **data,
                               dt_pixel_cache_entry_t **entry)
{
  if(hash == DT_PIXELPIPE_CACHE_HASH_INVALID)
  {
    dt_print(DT_DEBUG_PIPECACHE, "[pixelpipe_cache] refusing invalid hash allocation for %s\n",
             name ? name : "unknown");
    if(data) *data = NULL;
    if(entry) *entry = NULL;
    return 1;
  }

  // Search or create cache entry (under cache lock)
  dt_pthread_mutex_lock(&cache->lock);
  cache->queries++;

  dt_pixel_cache_entry_t *cache_entry = _non_threadsafe_cache_get_entry(cache, cache->entries, hash);
  if(cache_entry && cache_entry->auto_destroy)
  {
    _pixel_cache_message(cache_entry, "dropping auto-destroy entry before cache_get reuse", FALSE);
    if(_non_thread_safe_cache_remove(cache, FALSE, cache_entry, cache->entries) == 0)
      cache_entry = NULL;
  }

  if(cache_entry)
  {
    cache->hits++;
    cache_entry->hits++;
    _non_thread_safe_cache_ref_count_entry(cache, TRUE, cache_entry);
    dt_pthread_mutex_unlock(&cache->lock);

    // Allocate on demand if requested (e.g. when falling back from vRAM-only buffers).
    if(alloc && IS_NULL_PTR(cache_entry->data))
    {
      dt_dev_pixelpipe_cache_wrlock_entry(cache, TRUE, cache_entry);
      dt_pixel_cache_alloc(cache, cache_entry);
      dt_dev_pixelpipe_cache_wrlock_entry(cache, FALSE, cache_entry);
    }

    _pixelpipe_cache_finalize_entry(cache_entry, data, "found");
    if(entry) *entry = cache_entry;
    return 0;
  }

  cache_entry = _pixelpipe_cache_create_entry_locked(cache, hash, size, name, id);
  if(IS_NULL_PTR(cache_entry))
  {
    dt_print(DT_DEBUG_PIPECACHE, "couldn't allocate new cache entry %" PRIu64 "\n", hash);
    dt_pthread_mutex_unlock(&cache->lock);
    if(entry) *entry = NULL;
    return 1;
  }

  // Release cache lock AFTER acquiring entry locks to prevent other threads to capture it in-between
  dt_pthread_mutex_unlock(&cache->lock);

  // Alloc after releasing the lock for better runtimes
  if(alloc) dt_pixel_cache_alloc(cache, cache_entry);

  dt_print(DT_DEBUG_PIPECACHE, "[pixelpipe_cache] Write-lock on entry (new cache entry %" PRIu64 " for %s pipeline)\n",
           hash, name);
  _pixelpipe_cache_finalize_entry(cache_entry, data, "created");

  if(entry) *entry = cache_entry;
  return 1;
}

dt_dev_pixelpipe_cache_writable_status_t
dt_dev_pixelpipe_cache_get_writable(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash,
                                    const size_t size, const char *name, const int id,
                                    const gboolean alloc, const gboolean allow_rekey_reuse,
                                    const dt_pixel_cache_entry_t *reuse_hint,
                                    void **data,
                                    dt_pixel_cache_entry_t **entry)
{
  if(hash == DT_PIXELPIPE_CACHE_HASH_INVALID)
  {
    if(data) *data = NULL;
    if(entry) *entry = NULL;
    return DT_DEV_PIXELPIPE_CACHE_WRITABLE_ERROR;
  }

  dt_pthread_mutex_lock(&cache->lock);
  cache->queries++;

  dt_pixel_cache_entry_t *cache_entry = _non_threadsafe_cache_get_entry(cache, cache->entries, hash);
  if(cache_entry && cache_entry->auto_destroy)
  {
    _pixel_cache_message(cache_entry, "dropping auto-destroy entry before writable reuse", FALSE);
    if(_non_thread_safe_cache_remove(cache, FALSE, cache_entry, cache->entries) == 0)
      cache_entry = NULL;
  }

  if(cache_entry)
  {
    /* A hash match alone is not enough to skip recomputation here. Hostless GPU-only intermediates can stay
     * published in the table after a later VRAM flush has already dropped their last `cl_mem`, which leaves a
     * metadata-only cacheline under a perfectly valid hash. Returning EXACT_HIT for such an entry makes the
     * recursion resume at the next module, which then reopens the entry directly and discovers that it has no
     * RAM buffer and no recoverable device payload anymore. Recompute instead of treating an empty cacheline as
     * authoritative output. */
    if(IS_NULL_PTR(cache_entry->data) && !_cache_entry_has_device_payload(cache_entry, -1))
    {
      _pixel_cache_message(cache_entry, "dropping payload-less entry before writable exact-hit", FALSE);
      if(_non_thread_safe_cache_remove(cache, FALSE, cache_entry, cache->entries) == 0)
        cache_entry = NULL;
    }
  }

  if(cache_entry)
  {
    dt_pthread_mutex_unlock(&cache->lock);
    if(data) *data = NULL;
    if(entry) *entry = NULL;
    return DT_DEV_PIXELPIPE_CACHE_WRITABLE_EXACT_HIT;
  }

  if(allow_rekey_reuse)
  {
    cache_entry = _cache_try_rekey_reuse_locked(cache, hash, size, reuse_hint);
    if(cache_entry)
    {
      dt_pthread_mutex_unlock(&cache->lock);
      if(alloc && IS_NULL_PTR(cache_entry->data)) dt_pixel_cache_alloc(cache, cache_entry);
      _pixelpipe_cache_finalize_entry(cache_entry, data, "writable-rekeyed");
      if(entry) *entry = cache_entry;
      return DT_DEV_PIXELPIPE_CACHE_WRITABLE_REKEYED;
    }
  }

  cache_entry = _pixelpipe_cache_create_entry_locked(cache, hash, size, name, id);
  if(IS_NULL_PTR(cache_entry))
  {
    dt_pthread_mutex_unlock(&cache->lock);
    if(data) *data = NULL;
    if(entry) *entry = NULL;
    return DT_DEV_PIXELPIPE_CACHE_WRITABLE_ERROR;
  }

  dt_pthread_mutex_unlock(&cache->lock);

  if(alloc) dt_pixel_cache_alloc(cache, cache_entry);
  _pixelpipe_cache_finalize_entry(cache_entry, data, "writable-created");
  if(entry) *entry = cache_entry;
  return DT_DEV_PIXELPIPE_CACHE_WRITABLE_CREATED;
}

static gboolean _cache_entry_has_device_payload(dt_pixel_cache_entry_t *cache_entry,
                                                const int preferred_devid)
{
  if(IS_NULL_PTR(cache_entry)) return FALSE;

#ifdef HAVE_OPENCL
  gboolean has_payload = FALSE;
  dt_pthread_mutex_lock(&cache_entry->cl_mem_lock);
  for(GList *l = cache_entry->cl_mem_list; l;)
  {
    GList *next = g_list_next(l);
    dt_cache_clmem_t *c = (dt_cache_clmem_t *)l->data;
    if(IS_NULL_PTR(c) || IS_NULL_PTR(c->mem))
    {
      l = next;
      continue;
    }

    const int mem_devid = dt_opencl_get_mem_context_id((cl_mem)c->mem);
    if(mem_devid != c->devid)
    {
      l = next;
      continue;
    }

    if(IS_NULL_PTR(c->host_ptr) && (preferred_devid < 0 || c->devid == preferred_devid))
    {
      has_payload = TRUE;
      break;
    }

    l = next;
  }
  dt_pthread_mutex_unlock(&cache_entry->cl_mem_lock);
  return has_payload;
#else
  (void)preferred_devid;
  return FALSE;
#endif
}

static dt_pixel_cache_entry_t *_cache_lookup_existing(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash,
                                                      void **data)
{
  dt_pthread_mutex_lock(&cache->lock);
  cache->queries++;
  dt_pixel_cache_entry_t *cache_entry = _non_threadsafe_cache_get_entry(cache, cache->entries, hash);

  if(cache_entry)
  {
    cache->hits++;
    cache_entry->hits++;
    _pixelpipe_cache_finalize_entry(cache_entry, data, "found");
  }

  dt_pthread_mutex_unlock(&cache->lock);
  return cache_entry;
}

#ifdef HAVE_OPENCL
static gboolean _cache_try_restore_device_payload(dt_pixel_cache_entry_t *cache_entry,
                                                  const int preferred_devid, void **cl_mem_output)
{
  if(IS_NULL_PTR(cache_entry) || IS_NULL_PTR(cl_mem_output) || !IS_NULL_PTR(*cl_mem_output) || preferred_devid < 0)
    return FALSE;

  dt_pthread_mutex_lock(&cache_entry->cl_mem_lock);
  for(GList *l = cache_entry->cl_mem_list; l;)
  {
    GList *next = g_list_next(l);
    dt_cache_clmem_t *c = (dt_cache_clmem_t *)l->data;
    if(c && IS_NULL_PTR(c->host_ptr) && c->devid == preferred_devid && c->flags == CL_MEM_READ_WRITE)
    {
      const int mem_devid = dt_opencl_get_mem_context_id((cl_mem)c->mem);
      if(mem_devid != preferred_devid)
      {
        l = next;
        continue;
      }

      if(c->refs > 0)
      {
        l = next;
        continue;
      }

      cache_entry->cl_mem_list = g_list_delete_link(cache_entry->cl_mem_list, l);
      *cl_mem_output = c->mem;
      dt_free(c);
      break;
    }
    l = next;
  }
  dt_pthread_mutex_unlock(&cache_entry->cl_mem_lock);

  return !IS_NULL_PTR(*cl_mem_output);
}
#else
static gboolean _cache_try_restore_device_payload(dt_pixel_cache_entry_t *cache_entry,
                                                  const int preferred_devid, void **cl_mem_output)
{
  return FALSE;
}
#endif

gboolean dt_dev_pixelpipe_cache_restore_host_payload(dt_dev_pixelpipe_cache_t *cache,
                                                     dt_pixel_cache_entry_t *cache_entry,
                                                     const int preferred_devid, void **data)
{
  if(data) *data = NULL;
  if(IS_NULL_PTR(cache) || IS_NULL_PTR(cache_entry)) return FALSE;

  if(dt_pixel_cache_entry_get_data(cache_entry) != NULL)
  {
    if(data) *data = dt_pixel_cache_entry_get_data(cache_entry);
    return TRUE;
  }

  if(!_cache_entry_materialize_host_data(cache, preferred_devid, cache_entry))
    return FALSE;

  if(data) *data = dt_pixel_cache_entry_get_data(cache_entry);
  return dt_pixel_cache_entry_get_data(cache_entry) != NULL;
}

gboolean dt_dev_pixelpipe_cache_peek(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash, void **data,
                                     dt_pixel_cache_entry_t **entry, const int preferred_devid,
                                     void **cl_mem_output)
{
  if(data) *data = NULL;
  if(entry) *entry = NULL;
  if(cl_mem_output) *cl_mem_output = NULL;

  if(hash == DT_PIXELPIPE_CACHE_HASH_INVALID)
    return FALSE;

  dt_pixel_cache_entry_t *cache_entry = _cache_lookup_existing(cache, hash, data);
  if(IS_NULL_PTR(cache_entry)) return FALSE;

  if(data) *data = dt_pixel_cache_entry_get_data(cache_entry);

  /* Exact-hit callers treat the returned payload as already published. Reject
   * cachelines that are still write-locked: reusable output cachelines are
   * rekeyed to their new hash before recompute starts, so exposing them here
   * would let concurrent pipes consume stale or half-written buffers. */
  if(dt_pthread_rwlock_tryrdlock(&cache_entry->lock) != 0)
  {
    _trace_exact_hit("locked", hash, cache_entry, data ? *data : NULL,
                     cl_mem_output ? *cl_mem_output : NULL, preferred_devid, FALSE);
    if(data) *data = NULL;
    return FALSE;
  }
  dt_pthread_rwlock_unlock(&cache_entry->lock);

  if(IS_NULL_PTR(data) && IS_NULL_PTR(cl_mem_output))
  {
    if(entry) *entry = cache_entry;
    return TRUE;
  }

  /* Picker-triggered aborts can leave a cacheline temporarily present under its
   * hash while it is already marked auto-destroy. Those entries must never exact-hit:
   * they belong to the aborted lifecycle and must force a rebuild on the next run. */
  if(cache_entry->auto_destroy)
  {
    _trace_exact_hit("auto-destroy", hash, cache_entry, data ? *data : NULL,
                     cl_mem_output ? *cl_mem_output : NULL, preferred_devid, FALSE);
    if(data) *data = NULL;
    return FALSE;
  }

  if(dt_pixel_cache_entry_get_data(cache_entry) != NULL)
  {
    if(data) *data = dt_pixel_cache_entry_get_data(cache_entry);
    _cache_try_restore_device_payload(cache_entry, preferred_devid, cl_mem_output);

    _trace_exact_hit("host", hash, cache_entry, data ? *data : NULL,
                     cl_mem_output ? *cl_mem_output : NULL, preferred_devid, FALSE);
    if(entry) *entry = cache_entry;
    return TRUE;
  }

  /* `preferred_devid < 0` means the caller is on a CPU path and does not own any
   * OpenCL device. In that case, hostless cachelines are not consumable here:
   * reopening device-only payloads would enqueue hidden GPU work without a locked
   * device, while reporting a device-only exact-hit would let CPU callers sample
   * an uninitialized host buffer. */
  if(preferred_devid < 0)
  {
    _trace_exact_hit("cpu-no-device", hash, cache_entry, NULL, NULL, preferred_devid, FALSE);
    return FALSE;
  }

  if(_cache_try_restore_device_payload(cache_entry, preferred_devid, cl_mem_output))
  {
    _trace_exact_hit("device", hash, cache_entry, data ? *data : NULL,
                     cl_mem_output ? *cl_mem_output : NULL, preferred_devid, FALSE);
    if(entry) *entry = cache_entry;
    return TRUE;
  }

  if(data && dt_dev_pixelpipe_cache_restore_host_payload(cache, cache_entry, preferred_devid, data))
  {
    _trace_exact_hit("restore-host", hash, cache_entry, data ? *data : NULL,
                     cl_mem_output ? *cl_mem_output : NULL, preferred_devid, FALSE);
    if(entry) *entry = cache_entry;
    return TRUE;
  }

  if(data && _cache_entry_has_device_payload(cache_entry, preferred_devid))
  {
    if(data) *data = NULL;
    _trace_exact_hit("device-only", hash, cache_entry, NULL, NULL, preferred_devid, FALSE);
    return FALSE;
  }

  _trace_exact_hit("drop-invalid", hash, cache_entry, data ? *data : NULL,
                   cl_mem_output ? *cl_mem_output : NULL, preferred_devid, FALSE);
  dt_print(DT_DEBUG_PIPECACHE,
           "[pixelpipe] cache entry %" PRIu64 " has no authoritative RAM nor vRAM payload and will be removed\n",
           hash);
  dt_dev_pixelpipe_cache_remove(cache, TRUE, cache_entry);
  if(data) *data = NULL;
  return FALSE;
}


static gboolean _for_each_remove(gpointer key, gpointer value, gpointer user_data)
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


static gboolean _for_each_remove_old(gpointer key, gpointer value, gpointer user_data)
{
  dt_pixel_cache_entry_t *cache_entry = (dt_pixel_cache_entry_t *)value;

  // Returns 1 if the lock is captured by another thread
  // 0 if WE capture the lock, and then need to release it
  gboolean locked = dt_pthread_rwlock_trywrlock(&cache_entry->lock);
  if(!locked) dt_pthread_rwlock_unlock(&cache_entry->lock);
  gboolean used = dt_atomic_get_int(&cache_entry->refcount) > 0;

  // in microseconds
  int64_t delta = g_get_monotonic_time() - cache_entry->age;

  // 5 min in microseconds
  const int64_t three_min = 5 * 60 * 1000 * 1000;

  gboolean too_old = (delta > three_min) && (cache_entry->hits < 4);

  return too_old && !used && !locked;
}

static int dt_dev_pixelpipe_cache_flush_old(dt_dev_pixelpipe_cache_t *cache)
{
  // Don't hang the GUI thread if the cache is locked by a pipeline.
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


void _non_thread_safe_cache_ref_count_entry(dt_dev_pixelpipe_cache_t *cache, gboolean lock,
                                            dt_pixel_cache_entry_t *cache_entry)
{
  if(IS_NULL_PTR(cache_entry)) return;

  if(lock)
  {
    dt_atomic_add_int(&cache_entry->refcount, 1);
    _pixel_cache_message(cache_entry, "ref count ++", TRUE);
  }
  else
  {
    dt_atomic_sub_int(&cache_entry->refcount, 1);
    _pixel_cache_message(cache_entry, "ref count --", TRUE);
  }
}


void dt_dev_pixelpipe_cache_ref_count_entry(dt_dev_pixelpipe_cache_t *cache, gboolean lock,
                                            dt_pixel_cache_entry_t *cache_entry)
{
  dt_pthread_mutex_lock(&cache->lock);
  _non_thread_safe_cache_ref_count_entry(cache, lock, cache_entry);
  dt_pthread_mutex_unlock(&cache->lock);
}


void dt_dev_pixelpipe_cache_wrlock_entry(dt_dev_pixelpipe_cache_t *cache, gboolean lock,
                                         dt_pixel_cache_entry_t *cache_entry)
{
  if(lock)
  {
    dt_pthread_rwlock_wrlock(&cache_entry->lock);
    _pixel_cache_message(cache_entry, "write lock", TRUE);
  }
  else
  {
    dt_pthread_rwlock_unlock(&cache_entry->lock);
    _pixel_cache_message(cache_entry, "write unlock", TRUE);
    if(cache_entry && cache_entry->hash != DT_PIXELPIPE_CACHE_HASH_INVALID)
      DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_CACHELINE_READY, cache_entry->hash);
  }
}


void dt_dev_pixelpipe_cache_rdlock_entry(dt_dev_pixelpipe_cache_t *cache, gboolean lock,
                                         dt_pixel_cache_entry_t *cache_entry)
{
  if(lock)
  {
    dt_pthread_rwlock_rdlock(&cache_entry->lock);
    _pixel_cache_message(cache_entry, "read lock", TRUE);
  }
  else
  {
    dt_pthread_rwlock_unlock(&cache_entry->lock);
    _pixel_cache_message(cache_entry, "read unlock", TRUE);
  }
}


void dt_dev_pixelpipe_cache_flag_auto_destroy(dt_dev_pixelpipe_cache_t *cache,
                                              dt_pixel_cache_entry_t *cache_entry)
{
  dt_pthread_mutex_lock(&cache->lock);
  if(IS_NULL_PTR(cache_entry))
  {
    dt_pthread_mutex_unlock(&cache->lock);
    return;
  }

  cache_entry->auto_destroy = TRUE;
  _pixel_cache_message(cache_entry, "auto destroy flagged", TRUE);
  dt_pthread_mutex_unlock(&cache->lock);
}


void dt_dev_pixelpipe_cache_auto_destroy_apply(dt_dev_pixelpipe_cache_t *cache,
                                               dt_pixel_cache_entry_t *cache_entry)
{
  dt_pthread_mutex_lock(&cache->lock);
  if(IS_NULL_PTR(cache_entry))
  {
    dt_pthread_mutex_unlock(&cache->lock);
    return;
  }

  if(cache_entry->auto_destroy)
  {
    /* `auto_destroy` is still a normal cache lifecycle: the creator flags a transient entry, then the final
     * consumer decrements its refcount and asks the cache to reap it. Only remove it once no consumer owns
     * it anymore and nobody still holds the entry lock, otherwise teardown paths can free cachelines that
     * still report `refs>0` and hide ownership bugs instead of exposing them. */
    const gboolean locked = dt_pthread_rwlock_trywrlock(&cache_entry->lock);
    if(!locked) dt_pthread_rwlock_unlock(&cache_entry->lock);
    const gboolean used = dt_atomic_get_int(&cache_entry->refcount) > 0;

    if(!used && !locked)
    {
      _pixel_cache_message(cache_entry, "auto destroy removing", FALSE);
      g_hash_table_remove(cache->entries, &cache_entry->hash);
    }
    else if(used)
    {
      _pixel_cache_message(cache_entry, "auto destroy postponed: used", TRUE);
    }
    else
    {
      _pixel_cache_message(cache_entry, "auto destroy postponed: locked", TRUE);
    }
  }
  else
  {
    _pixel_cache_message(cache_entry, "auto destroy skipped", TRUE);
  }
  
  dt_pthread_mutex_unlock(&cache->lock);
}

void dt_dev_pixelpipe_cache_unref_hash(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash)
{
  if(hash == DT_PIXELPIPE_CACHE_HASH_INVALID) return;

  dt_pthread_mutex_lock(&cache->lock);
  cache->queries++;
  dt_pixel_cache_entry_t *cache_entry = _non_threadsafe_cache_get_entry(cache, cache->entries, hash);
  dt_pthread_mutex_unlock(&cache->lock);

  if(cache_entry)
    dt_dev_pixelpipe_cache_ref_count_entry(cache, FALSE, cache_entry);
}

int dt_dev_pixelpipe_cache_rekey(dt_dev_pixelpipe_cache_t *cache, const uint64_t old_hash,
                                 const uint64_t new_hash, dt_pixel_cache_entry_t *entry)
{
  if(IS_NULL_PTR(cache)) return 1;
  if(old_hash == new_hash) return 0;

  dt_pthread_mutex_lock(&cache->lock);

  if(IS_NULL_PTR(entry)) entry = _non_threadsafe_cache_get_entry(cache, cache->entries, old_hash);
  if(IS_NULL_PTR(entry))
  {
    dt_print(DT_DEBUG_PIPECACHE,
             "[pixelpipe_cache] rekey miss old=%" PRIu64 " new=%" PRIu64 " module=%s\n",
             old_hash, new_hash, _cache_debug_module_name());
    dt_pthread_mutex_unlock(&cache->lock);
    return 1;
  }

  dt_pixel_cache_entry_t *conflict = _non_threadsafe_cache_get_entry(cache, cache->entries, new_hash);
  if(conflict && conflict != entry)
  {
    dt_print(DT_DEBUG_PIPECACHE,
             "[pixelpipe_cache] rekey conflict old=%" PRIu64 " new=%" PRIu64
             " entry=%" PRIu64 "/%" PRIu64 " conflict=%" PRIu64 "/%" PRIu64 " module=%s\n",
             old_hash, new_hash, entry->hash, entry->serial, conflict->hash, conflict->serial,
             _cache_debug_module_name());
    dt_pthread_mutex_unlock(&cache->lock);
    return 1;
  }

  gpointer stolen_key = NULL;
  gpointer stolen_value = NULL;
  if(!g_hash_table_steal_extended(cache->entries, &old_hash, &stolen_key, &stolen_value))
  {
    dt_print(DT_DEBUG_PIPECACHE,
             "[pixelpipe_cache] rekey steal-miss old=%" PRIu64 " new=%" PRIu64
             " entry=%" PRIu64 "/%" PRIu64 " module=%s\n",
             old_hash, new_hash, entry->hash, entry->serial, _cache_debug_module_name());
    dt_pthread_mutex_unlock(&cache->lock);
    return 1;
  }

  if(stolen_value != entry)
  {
    dt_print(DT_DEBUG_PIPECACHE,
             "[pixelpipe_cache] rekey stolen-entry mismatch old=%" PRIu64 " new=%" PRIu64
             " expected=%" PRIu64 "/%" PRIu64 " got=%" PRIu64 "/%" PRIu64 " module=%s\n",
             old_hash, new_hash, entry->hash, entry->serial,
             ((dt_pixel_cache_entry_t *)stolen_value)->hash, ((dt_pixel_cache_entry_t *)stolen_value)->serial,
             _cache_debug_module_name());
    g_hash_table_insert(cache->entries, stolen_key, stolen_value);
    dt_pthread_mutex_unlock(&cache->lock);
    return 1;
  }

  /* Explicit rekeying also changes cacheline ownership. The OpenCL payload cache is only valid for the
   * previous hash, so do not let the new hash inherit stale device-side state. If some GPU code is still
   * borrowing one of these payloads, refuse the rekey instead of publishing an ambiguous cache entry. */
  dt_pthread_mutex_lock(&entry->cl_mem_lock);
  for(GList *l = entry->cl_mem_list; l; l = g_list_next(l))
  {
    dt_cache_clmem_t *c = (dt_cache_clmem_t *)l->data;
    if(c && c->refs > 0)
    {
      dt_pthread_mutex_unlock(&entry->cl_mem_lock);
      g_hash_table_insert(cache->entries, stolen_key, stolen_value);
      dt_pthread_mutex_unlock(&cache->lock);
      return 1;
    }
  }
  dt_pthread_mutex_unlock(&entry->cl_mem_lock);
  dt_dev_pixelpipe_cache_flush_entry_clmem(entry);

  *(uint64_t *)stolen_key = new_hash;
  entry->hash = new_hash;
  g_hash_table_insert(cache->entries, stolen_key, stolen_value);
  dt_print(DT_DEBUG_PIPECACHE,
           "[pixelpipe_cache] rekey old=%" PRIu64 " new=%" PRIu64 " entry=%" PRIu64 "/%" PRIu64
           " refs=%i auto=%i data=%p module=%s\n",
           old_hash, new_hash, entry->hash, entry->serial, dt_atomic_get_int(&entry->refcount),
           entry->auto_destroy, entry->data, _cache_debug_module_name());

  dt_pthread_mutex_unlock(&cache->lock);
  return 0;
}


void dt_dev_pixelpipe_cache_print(dt_dev_pixelpipe_cache_t *cache)
{
  if(!(darktable.unmuted & DT_DEBUG_PIPECACHE)) return;

  dt_print(DT_DEBUG_PIPECACHE, "[pixelpipe] cache hit rate so far: %.3f%% - size: %lu MiB over %lu MiB - %i items\n", 100. * (cache->hits) / (float)cache->queries, cache->current_memory / (1024 * 1024), cache->max_memory / (1024 * 1024), g_hash_table_size(cache->entries));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
