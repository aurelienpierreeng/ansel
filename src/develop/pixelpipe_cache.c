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

#include <inttypes.h>
#include <glib.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

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

typedef struct dt_free_run_t
{
  uint32_t start;
  uint32_t length;
} dt_free_run_t;

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
static dt_pixel_cache_entry_t *dt_pixel_cache_new_entry(const uint64_t hash, const size_t size,
                                                        const dt_iop_buffer_dsc_t dsc, const char *name, const int id,
                                                        dt_dev_pixelpipe_cache_t *cache, gboolean alloc);
static void _cache_entry_clmem_flush_device(dt_pixel_cache_entry_t *entry, const int devid);

dt_pixel_cache_entry_t *_non_threadsafe_cache_get_entry(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash)
{
  dt_pixel_cache_entry_t *entry = (dt_pixel_cache_entry_t *)g_hash_table_lookup(cache->entries, GINT_TO_POINTER(hash));
  if(entry) entry->hits++;
  return entry;
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
  dt_print(DT_DEBUG_CACHE, "[pixelpipe] cache entry %" PRIu64 ": %s (%lu MiB - age %" PRId64 " - hits %i - refs %i) %s\n", 
           cache_entry->hash, cache_entry->name, dt_pixel_cache_get_size(cache_entry), 
           cache_entry->age, cache_entry->hits, dt_atomic_get_int(&cache_entry->refcount), message);
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
      g_hash_table_remove(cache->entries, GINT_TO_POINTER(cache_entry->hash));
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
  int error = _non_thread_safe_cache_remove(cache, hash, force, cache_entry);
  dt_pthread_mutex_unlock(&cache->lock);
  return error;
}

void dt_dev_pixelpipe_cache_flush_clmem(dt_dev_pixelpipe_cache_t *cache, const int devid)
{
  if(!cache) return;
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
    error = _non_thread_safe_cache_remove(cache, lru->hash, FALSE, lru->cache_entry);
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

static inline size_t align_up(size_t v, size_t a)
{
  return (v + a - 1) & ~(a - 1);
}

static inline gboolean dt_cache_arena_calc(const dt_cache_arena_t *a,
                                           size_t size,
                                           uint32_t *out_pages,
                                           size_t *out_size)
{
  if(!a || !a->base || !out_pages || a->page_size == 0 || a->num_pages == 0) return FALSE;
  if(size == 0) return FALSE;
  if(size > SIZE_MAX - (a->page_size - 1)) return FALSE;

  const size_t pages = (size + a->page_size - 1) / a->page_size;
  if(pages > a->num_pages || pages > UINT32_MAX) return FALSE;
  if(out_size)
  {
    if(pages > SIZE_MAX / a->page_size) return FALSE;
    *out_size = pages * a->page_size;
  }

  *out_pages = (uint32_t)pages;
  return TRUE;
}

/*
 * Allocate from the arena in page-sized chunks.
 * Uses a best-fit scan over the sorted free-run list, then consumes from
 * the beginning of the selected run. On success, returns a pointer into the
 * arena and writes the page-rounded allocation size to out_size.
 */
static void *dt_cache_arena_alloc(dt_cache_arena_t *a,
                                  size_t size,
                                  size_t *out_size)
{
  if(!out_size) return NULL;
  *out_size = 0;

  uint32_t pages_needed = 0;
  size_t rounded_size = 0;
  if(!dt_cache_arena_calc(a, size, &pages_needed, &rounded_size)) return NULL;

  dt_pthread_mutex_lock(&a->lock);

  /* scan free_runs (sorted by start) for the smallest run that fits */
  guint best_index = G_MAXUINT;
  uint32_t best_length = UINT32_MAX;
  for(guint i = 0; i < a->free_runs->len; i++)
  {
    dt_free_run_t *r =
      &g_array_index(a->free_runs, dt_free_run_t, i);

    if(r->length >= pages_needed && r->length < best_length)
    {
      best_index = i;
      best_length = r->length;
      if(best_length == pages_needed) break; // exact fit
    }
  }

  if(best_index == G_MAXUINT)
  {
    dt_pthread_mutex_unlock(&a->lock);
    return NULL;
  }

  dt_free_run_t *r = &g_array_index(a->free_runs, dt_free_run_t, best_index);
  const uint32_t first = r->start;

  /* consume from the front of the run so the list stays sorted */
  r->start += pages_needed;
  r->length -= pages_needed;

  /* remove empty run after consumption */
  if(r->length == 0)
    g_array_remove_index(a->free_runs, best_index);

  dt_pthread_mutex_unlock(&a->lock);

  *out_size = rounded_size;
  return a->base + (size_t)first * a->page_size;
}


/*
 * Return a previously allocated region to the arena.
 * The pointer must refer to the arena base, and size is rounded up to pages.
 * The freed run is inserted in order and coalesced with adjacent runs.
 */
static void dt_cache_arena_free(dt_cache_arena_t *a,
                                void *ptr,
                                size_t size)
{
  if(!a || !a->base || !a->free_runs || a->page_size == 0 || a->num_pages == 0)
    return;
  if(!ptr || size == 0) return;

  const uintptr_t base = (uintptr_t)a->base;
  const uintptr_t addr = (uintptr_t)ptr;
  if(addr < base || addr >= base + a->size)
  {
    fprintf(stderr, "[pixelpipe] arena free: pointer out of range\n");
    return;
  }
  if(((addr - base) % a->page_size) != 0)
  {
    fprintf(stderr, "[pixelpipe] arena free: pointer not page-aligned\n");
    return;
  }

  uint32_t pages = 0;
  if(!dt_cache_arena_calc(a, size, &pages, NULL))
  {
    fprintf(stderr, "[pixelpipe] arena free: invalid size\n");
    return;
  }

  const size_t first_sz = (addr - base) / a->page_size;
  if(first_sz >= a->num_pages || pages > a->num_pages - first_sz)
  {
    fprintf(stderr, "[pixelpipe] arena free: range out of bounds\n");
    return;
  }

  const uint32_t first = (uint32_t)first_sz;

  dt_pthread_mutex_lock(&a->lock);

  /* insert a new free run, keeping free_runs sorted by start page */
  guint i = 0;
  while(i < a->free_runs->len &&
        g_array_index(a->free_runs, dt_free_run_t, i).start < first)
    i++;

  if(i > 0)
  {
    dt_free_run_t *prev = &g_array_index(a->free_runs, dt_free_run_t, i - 1);
    if(prev->start + prev->length > first)
    {
      dt_pthread_mutex_unlock(&a->lock);
      fprintf(stderr, "[pixelpipe] arena free: overlap with previous run\n");
      return;
    }
  }
  if(i < a->free_runs->len)
  {
    dt_free_run_t *next = &g_array_index(a->free_runs, dt_free_run_t, i);
    if(first + pages > next->start)
    {
      dt_pthread_mutex_unlock(&a->lock);
      fprintf(stderr, "[pixelpipe] arena free: overlap with next run\n");
      return;
    }
  }

  dt_free_run_t new = { first, pages };
  g_array_insert_val(a->free_runs, i, new);

  /* coalesce with next run if adjacent */
  if(i + 1 < a->free_runs->len)
  {
    dt_free_run_t *cur = &g_array_index(a->free_runs, dt_free_run_t, i);
    dt_free_run_t *next = &g_array_index(a->free_runs, dt_free_run_t, i + 1);
    if(cur->start + cur->length == next->start)
    {
      cur->length += next->length;
      g_array_remove_index(a->free_runs, i + 1);
    }
  }

  /* coalesce with previous run if adjacent */
  if(i > 0)
  {
    dt_free_run_t *prev = &g_array_index(a->free_runs, dt_free_run_t, i - 1);
    dt_free_run_t *cur  = &g_array_index(a->free_runs, dt_free_run_t, i);
    if(prev->start + prev->length == cur->start)
    {
      prev->length += cur->length;
      g_array_remove_index(a->free_runs, i);
    }
  }

  dt_pthread_mutex_unlock(&a->lock);
}

void *dt_pixel_cache_clmem_get(dt_pixel_cache_entry_t *entry, void *host_ptr, int devid,
                               int width, int height, int bpp, int flags, int *out_cst)
{
  if(!entry || !host_ptr || width <= 0 || height <= 0 || bpp <= 0) return NULL;
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
  if(!entry || !mem || !host_ptr || width <= 0 || height <= 0 || bpp <= 0)
  {
    dt_opencl_release_mem_object(mem);
    return;
  }

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
  if(!entry) return;

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
  if(!entry) return;

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
static int _free_space_to_alloc(dt_dev_pixelpipe_cache_t *cache, const size_t size, const uint64_t hash, const char *name)
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

    fprintf(stdout, "[pixelpipe] cache is full, cannot allocate new entry %" PRIu64 " (%s)\n", hash, name);
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

  size_t page_size = 0;
  void *buf = dt_cache_arena_alloc(&cache->arena, size, &page_size);
  if(!buf) return NULL;

  void *aligned = __builtin_assume_aligned(buf, DT_CACHELINE_BYTES);
  dt_iop_buffer_dsc_t dsc = {0};

  const uintptr_t addr = (uintptr_t)aligned;
  uint64_t hash = dt_hash(5381, (const char *)&addr, sizeof(addr));

  dt_pthread_mutex_lock(&cache->lock);

  dt_pixel_cache_entry_t *cache_entry
      = dt_pixel_cache_new_entry(hash, page_size, dsc, name, id, cache, FALSE);

  if(!cache_entry)
  {
    dt_pthread_mutex_unlock(&cache->lock);
    dt_cache_arena_free(&cache->arena, buf, page_size);
    return NULL;
  }

  // Protect this entry from LRU/flush removal while in use.
  _non_thread_safe_cache_ref_count_entry(cache, hash, TRUE, cache_entry);
  dt_pthread_rwlock_wrlock(&cache_entry->lock);
  dt_pthread_mutex_unlock(&cache->lock);

  cache_entry->data = aligned;
  cache_entry->age = g_get_monotonic_time();
  cache_entry->external_alloc = TRUE;
  // Keep the lock held for the lifetime of this buffer.
  return aligned;
}

void dt_pixelpipe_cache_free_align_cache(dt_dev_pixelpipe_cache_t *cache, void *mem, const char *message)
{
  if(!mem) return;

  const uintptr_t addr = (uintptr_t)mem;
  uint64_t hash = dt_hash(5381, (const char *)&addr, sizeof(addr));

  dt_pthread_mutex_lock(&cache->lock);

  dt_pixel_cache_entry_t *cache_entry = _non_threadsafe_cache_get_entry(cache, hash);
  if(cache_entry)
  {
    if(cache_entry->external_alloc)
    {
      _non_thread_safe_cache_ref_count_entry(cache, hash, FALSE, cache_entry);
      dt_pthread_rwlock_unlock(&cache_entry->lock);
      g_hash_table_remove(cache->entries, GINT_TO_POINTER(cache_entry->hash));
      mem = NULL;
    }
  }
  else
  {
    fprintf(stdout, "error while freeing cache entry: no entry found but we have a buffer, %s.\n", message);
    raise(SIGSEGV); // triggers dt_set_signal_handlers() backtrace on Unix
  }

  dt_pthread_mutex_unlock(&cache->lock);
}


// WARNING: not thread-safe, protect its calls with mutex lock
static dt_pixel_cache_entry_t *dt_pixel_cache_new_entry(const uint64_t hash, const size_t size,
                                                        const dt_iop_buffer_dsc_t dsc, const char *name, const int id,
                                                        dt_dev_pixelpipe_cache_t *cache, gboolean alloc)
{
  uint32_t pages_needed = 0;
  size_t rounded_size = 0;
  if(!dt_cache_arena_calc(&cache->arena, size, &pages_needed, &rounded_size))
  {
    fprintf(stderr, "[pixelpipe] invalid cache entry size %zu for %s\n", size, name);
    return NULL;
  }

  int error = _free_space_to_alloc(cache, rounded_size, 0, name);
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

  // Add this entry to the table
  g_hash_table_insert(cache->entries, GINT_TO_POINTER(hash), cache_entry);

  // Note : we grow the cache size even though the data buffer is not yet allocated
  // This is planning
  cache->current_memory += size;

  return cache_entry;
}


static void _free_cache_entry(dt_pixel_cache_entry_t *cache_entry)
{
  if(!cache_entry) return;
  dt_pixel_cache_message(cache_entry, "freed", FALSE);

  if(cache_entry->data) 
    dt_cache_arena_free(&cache_entry->cache->arena, cache_entry->data, cache_entry->size);

  dt_pixel_cache_clmem_flush(cache_entry);

  cache_entry->data = NULL;
  cache_entry->cache->current_memory -= cache_entry->size;
  dt_pthread_rwlock_destroy(&cache_entry->lock);
  dt_pthread_mutex_destroy(&cache_entry->cl_mem_lock);
  g_free(cache_entry->name);
  free(cache_entry);
}

static void dt_cache_arena_cleanup(dt_cache_arena_t *a)
{
  if(!a) return;

  dt_pthread_mutex_lock(&a->lock);
  g_array_free(a->free_runs, TRUE);
  dt_pthread_mutex_unlock(&a->lock);

  dt_pthread_mutex_destroy(&a->lock);

  /* 5. Release the virtual memory block */
  if(a->base && a->size)
  {
#ifdef _WIN32
    VirtualFree(a->base, 0, MEM_RELEASE);
#else
    munmap(a->base, a->size);
#endif
  }

  /* 6. Poison the struct (defensive) */
  a->base = NULL;
  a->size = 0;
  a->num_pages = 0;
  a->page_size = 0;
}

static int garbage_collection = 0;

// return 0 on success 1 on error
static int dt_cache_arena_init(dt_cache_arena_t *a, size_t total_size)
{
  const size_t page_size = 64 * 1024; // 64 KiB cache pages
  const size_t pages = total_size / page_size;

#ifdef _WIN32
  a->base = (uint8_t *)VirtualAlloc(NULL, total_size,
                                    MEM_RESERVE | MEM_COMMIT,
                                    PAGE_READWRITE);
  if(!a->base)
  {
    const DWORD err = GetLastError();
    fprintf(stderr, "couldn't alloc map (VirtualAlloc error %lu)\n", (unsigned long)err);
    return 1;
  }
#else
  a->base = mmap(NULL, total_size,
                 PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS,
                 -1, 0);

  if(a->base == MAP_FAILED)
  {
    a->base = NULL;
    fprintf(stderr, "couldn't alloc map (mmap error %d: %s)\n", errno, strerror(errno));
    return 1;
  }
#endif

  a->size = total_size;
  a->page_size = page_size;
  a->num_pages = pages;

  a->free_runs = g_array_new(FALSE, FALSE, sizeof(dt_free_run_t));
  if(!a->free_runs)
  {
#ifdef _WIN32
    VirtualFree(a->base, 0, MEM_RELEASE);
#else
    munmap(a->base, a->size);
#endif
    a->base = NULL;
    a->size = 0;
    a->page_size = 0;
    a->num_pages = 0;
    fprintf(stderr, "couldn't alloc free run list\n");
    return 1;
  }

  /* start with one free run covering the whole arena */
  dt_free_run_t full = {
    .start  = 0,
    .length = a->num_pages
  };

  g_array_append_val(a->free_runs, full);

  dt_pthread_mutex_init(&a->lock, NULL);
  return 0;
}


dt_dev_pixelpipe_cache_t * dt_dev_pixelpipe_cache_init(size_t max_memory)
{
  dt_dev_pixelpipe_cache_t *cache = (dt_dev_pixelpipe_cache_t *)malloc(sizeof(dt_dev_pixelpipe_cache_t));
  dt_pthread_mutex_init(&cache->lock, NULL);
  cache->entries = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)_free_cache_entry);
  cache->max_memory = max_memory;
  cache->current_memory = 0;
  cache->queries = cache->hits = 0;

  if(dt_cache_arena_init(&cache->arena, cache->max_memory))
  {
    dt_pthread_mutex_destroy(&cache->lock);
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
  if(!cache) return;
  dt_pthread_mutex_destroy(&cache->lock);
  g_hash_table_destroy(cache->entries);
  cache->entries = NULL;
  dt_cache_arena_cleanup(&cache->arena);

  if(garbage_collection != 0)
  {
    g_source_remove(garbage_collection);
    garbage_collection = 0;
  }
}


int dt_dev_pixelpipe_cache_get(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash,
                               const size_t size, const char *name, const int id,
                               void **data, dt_iop_buffer_dsc_t **dsc,
                               dt_pixel_cache_entry_t **entry, const gboolean alloc)
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
    _non_thread_safe_cache_ref_count_entry(cache, hash, TRUE, cache_entry);
    
    // Release cache lock BEFORE acquiring entry locks to avoid deadlock
    dt_pthread_mutex_unlock(&cache->lock);
  }
  else
  {
    // New entry: create and allocate
    cache_entry = dt_pixel_cache_new_entry(hash, size, **dsc, name, id, cache, FALSE);
    
    if(!cache_entry)
    {
      dt_print(DT_DEBUG_CACHE, "couldn't allocate new cache entry %" PRIu64 "\n", hash);
      dt_pthread_mutex_unlock(&cache->lock);
      if(entry) *entry = NULL;
      return 1;
    }

    // Increase ref_count, consumer will have to decrease it
    _non_thread_safe_cache_ref_count_entry(cache, hash, TRUE, cache_entry);
    
    // Acquire write lock so caller can populate data safely
    dt_dev_pixelpipe_cache_wrlock_entry(cache, hash, TRUE, cache_entry);

    // Release cache lock AFTER acquiring entry locks to prevent other threads to capture it in-between
    dt_pthread_mutex_unlock(&cache->lock);

    // Alloc after releasing the lock for better runtimes
    if(alloc) dt_pixel_cache_alloc(cache, cache_entry);

    dt_print(DT_DEBUG_CACHE,"[pixelpipe_cache] Write-lock on entry (new cache entry %" PRIu64 " for %s pipeline)\n", hash, name);
  }

  // Finalize and return
  cache_entry->age = g_get_monotonic_time(); // Update MRU timestamp
  *data = cache_entry->data ? __builtin_assume_aligned(cache_entry->data, DT_CACHELINE_BYTES) : NULL;
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

    // Set the time after we get the lock
    cache_entry->age = g_get_monotonic_time(); // this is the MRU entry
    if(data) *data = cache_entry->data ? __builtin_assume_aligned(cache_entry->data, DT_CACHELINE_BYTES) : NULL;
    if(dsc) *dsc = &cache_entry->dsc;
    dt_pixel_cache_message(cache_entry, "found", FALSE);
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


void dt_dev_pixel_pipe_cache_auto_destroy_apply(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash,
                                                dt_pixel_cache_entry_t *cache_entry)
{
  dt_pthread_mutex_lock(&cache->lock);
  if(cache_entry == NULL)
    cache_entry = _non_threadsafe_cache_get_entry(cache, hash);

  if(cache_entry && cache_entry->auto_destroy)
    g_hash_table_remove(cache->entries, GINT_TO_POINTER(hash));
  
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
  dt_pthread_mutex_lock(&cache->lock);
  cache->queries++;
  dt_pixel_cache_entry_t *cache_entry = _non_threadsafe_cache_get_entry(cache, hash);
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
