/*
    This file is part of darktable,
    Copyright (C) 2009-2020 darktable developers.
    Copyright (C) 2022-2025 Aurélien PIERRE.

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

#include <inttypes.h>

struct dt_dev_pixelpipe_t;
struct dt_iop_buffer_dsc_t;
struct dt_iop_roi_t;


/**
 * @file pixelpipe_cache.h
 * @brief Pixelpipe cache for storing intermediate results in the pixelpipe.
 *
 * This cache can be used locally (in the pixelpipe) or globally (in the whole app).
 * Current implementation is global, using `darktable.pipeline_threadsafe` mutex lock
 * to protect cache entries addition/removal accross threads. The mutex lock
 * protects the whole recursive pixelpipe, so no internal locking is needed nor implemented here.
 */

typedef struct dt_dev_pixelpipe_cache_t
{
  GHashTable *entries;
  uint64_t queries;
  uint64_t hits;
  size_t max_memory;
  size_t current_memory;
  dt_pthread_mutex_t lock; // mutex to protect the cache entries
} dt_dev_pixelpipe_cache_t;

/** constructs a new cache with given cache line count (entries) and float buffer entry size in bytes.
  \param[out] returns 0 if fail to allocate mem cache.
*/
dt_dev_pixelpipe_cache_t *dt_dev_pixelpipe_cache_init(size_t max_memory);
void dt_dev_pixelpipe_cache_cleanup(dt_dev_pixelpipe_cache_t *cache);

struct dt_pixel_cache_entry_t;

/**
 * @brief Get an internal reference to the cache entry matching hash.
 * If you are going to access this entry more than once, keeping the reference and using
 * it instead of hashes will prevent redundant lookups.
 *
 * @param cache
 * @param hash
 * @return struct dt_pixel_cache_entry_t*
 */
struct dt_pixel_cache_entry_t *dt_dev_pixelpipe_cache_get_entry(dt_dev_pixelpipe_cache_t *cache,
                                                                const uint64_t hash);


/**
 * @brief Get a cache line from the cache.
 *
 * WARNING: This internally increases the reference count,
 * so you have to manually decrease it using `dt_dev_pixelpipe_ref_count_entry()` once
 * the cache line content has been consumed or it will never be freed.
 *
 * WARNING: if the cache line was newly allocated, a write lock is put on
 * straight away. You will have to release it from the same calling thread next,
 * to avoid dead locks.
 *
 * @param cache
 * @param hash State checksum of the cache line.
 * @param size Buffer size in bytes.
 * @param name Name of the cache line (for debugging).
 * @param id ID of the pipeline owning the cache line.
 * @param data Pointer to the buffer pointer (returned).
 * @param dsc Pointer to the buffer descriptor (returned).
 * @param alloc Whether or not we should actually alloc the buffer, or simply reserve it. If FALSE
 * use `dt_pixel_cache_alloc()` when you actually need the buffer.
 * @param cache_entry a reference to the cache entry, to be reused later. Can be NULL. The caller
  doesn't own the data and shouldn't free it.
 * @return int 1 if the cache line was freshly allocated, 0 if it was found in the cache.
 */
int dt_dev_pixelpipe_cache_get(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash, const size_t size,
                               const char *name, const int id, void **data, struct dt_iop_buffer_dsc_t **dsc,
                               struct dt_pixel_cache_entry_t **entry, const gboolean alloc);

                    
/**
 * @brief Actually allocate the memory buffer attached to the cache entry once you create it with
 * `dt_dev_pixelpipe_cache_get()`. Sizes and everything are already saved in the entry, and the 
 * cache will have the needed space reserved. 
 * 
 * @param cache 
 * @param entry the cache entry 
 * @return void* Pointer to the allocated data buffer.
 */
void *dt_pixel_cache_alloc(dt_dev_pixelpipe_cache_t *cache, struct dt_pixel_cache_entry_t *entry);


/**
 * @brief Get an existing cache line from the cache. This is similar to `dt_dev_pixelpipe_cache_get`,
 * but it does not create a new cache line if it is not found.
 *
 * This internally increases the reference count,
 * so you have to manually decrease it using `dt_dev_pixelpipe_ref_count_entry()` once
 * the cache line content has been consumed or it will never be freed.
 *
 * @param cache
 * @param hash
 * @param data
 * @param dsc
 * @return int TRUE if found, FALSE if not found.
 */
int dt_dev_pixelpipe_cache_get_existing(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash, void **data,
                                        struct dt_iop_buffer_dsc_t **dsc, struct dt_pixel_cache_entry_t **entry);

/**
 * @brief Remove cache lines matching id. Entries locked in read/write or having reference
 * count greater than 0 are not removed.
 *
 * @param cache
 * @param id ID of the pipeline owning the cache line, or -1 to remove all lines.
 */
void dt_dev_pixelpipe_cache_flush(dt_dev_pixelpipe_cache_t *cache, const int id);


/**
 * @brief Free cache entries older than 3 min, that are not locked and have been used
 * 3 times or less.
 * 
 * @param cache 
 */
int dt_dev_pixelpipe_cache_flush_old(dt_dev_pixelpipe_cache_t *cache);

/**
 * @brief Arbitrarily remove the cache entry matching hash. Entries
 * having a reference count > 0 (inter-thread locked) or being having their read/write lock
 * locked will be ignored. If force is TRUE, we ignore reference count, but not locks.
 *
 * @param cache
 * @param hash
 * @param force
 */
int dt_dev_pixelpipe_cache_remove(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash, const gboolean force,
                                  struct dt_pixel_cache_entry_t *entry);


/** print out cache lines/hashes (debug). */
void dt_dev_pixelpipe_cache_print(dt_dev_pixelpipe_cache_t *cache);

/** remove the least used cache entry
 * @return 0 on success, 1 on error
 */
int dt_dev_pixel_pipe_cache_remove_lru(dt_dev_pixelpipe_cache_t *cache);

/**
 * @brief Increase/Decrease the reference count on the cache line as to prevent
 * LRU item removal. This function should be called within a read/write lock-protected
 * section to avoid changing an entry while or after it is deleted in parallel.
 *
 * WARNING: cache entries whose reference count is greater than 0 will never be deleted from cache.
 *
 * @param cache
 * @param hash
 * @param lock TRUE to lock, FALSE to unlock
 */
void dt_dev_pixelpipe_cache_ref_count_entry(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash, gboolean lock,
                                            struct dt_pixel_cache_entry_t *entry);

/**
 * @brief Find the hash of the cache entry holding the buffer data
 *
 * @param cache
 * @param data
 * @param cache_entry a reference to the cache entry, to be reused later. Can be NULL. The caller
 * doesn't own the data and shouldn't free it.
 * @return uint64_t defaults to 0 if nothing was found.
 */
uint64_t dt_dev_pixelpipe_cache_get_hash_data(dt_dev_pixelpipe_cache_t *cache, void *data,
                                              struct dt_pixel_cache_entry_t **entry);


/**
 * @brief Return a reference to the cache entry holding the data buffer, or NULL if not found.
 *
 * WARNING: this immediately puts a read lock on the entry cache, if found. You will
 * need to manually release it later with `dt_dev_pixelpipe_cache_rdlock_entry()`
 * or you will get a deadlock and this entry will never be deleted.
 *
 * @param cache
 * @param data
 * @return dt_pixel_cache_entry_t*
 */
struct dt_pixel_cache_entry_t *dt_dev_pixelpipe_cache_get_entry_from_data(dt_dev_pixelpipe_cache_t *cache, void *data);


/**
 * @brief Lock or release the write lock on the entry
 *
 * @param cache
 * @param hash
 * @param lock TRUE to lock, FALSE to release
 */
void dt_dev_pixelpipe_cache_wrlock_entry(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash, gboolean lock,
                                         struct dt_pixel_cache_entry_t *entry);


/**
 * @brief Lock or release the read lock on the entry
 *
 * @param cache
 * @param hash checksum of the cache entry to fetch. Optional if `entry` is not NULL.
 * @param lock TRUE to lock, FALSE to release
 * @param entry The cache entry object to lock, if a reference is already known. Can be NULL, but then you
 * need to pass a hash.
 */
void dt_dev_pixelpipe_cache_rdlock_entry(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash, gboolean lock,
                                         struct dt_pixel_cache_entry_t *entry);


/**
 * @brief Flag the cache entry matching hash as "auto_destroy". This is useful for short-lived/disposable
 * cache entries, that won't be needed in the future. These will be freed out of the typical LRU, aged-based
 * garbage collection. The thread that tagged this entry as "auto_destroy" is responsible for freeing it
 * as soon as it is done with it, using `dt_dev_pixelpipe_cache_auto_destroy_apply()`.
 * If not manually freed this way, the entry will be caught using the generic LRU garbage collection.
 *
 * @param cache
 * @param hash
 */
void dt_dev_pixelpipe_cache_flag_auto_destroy(dt_dev_pixelpipe_cache_t *cache, uint64_t hash,
                                              struct dt_pixel_cache_entry_t *entry);

/**
 * @brief Free the entry matching hash if it has the flag "auto_destroy".
 * See `dt_dev_pixelpipe_cache_flag_auto_destroy()`.
 * This will not check reference count nor read/write locks, so it has to happen in the thread that created the
 * entry, flagged it and owns it. Ensure your hashes are truly unique and not shared between pipelines to ensure
 * another thread will not free this or that another thread ends up using it.
 *
 * @param cache
 * @param hash
 */
void dt_dev_pixelpipe_cache_auto_destroy_apply(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash,
                                               struct dt_pixel_cache_entry_t *entry);


/**
* @brief Find the existing cache entry linked to hash if any, lock it in read mode and increase its ref_count
* all at once. 
* 
* @param cache 
* @param hash 
* @param cache_entry Found cache entry if any, this is written by the function 
* @return void* Data buffer associated with the cache entry, or NULL.
*/
void *dt_dev_pixelpipe_cache_get_read_only(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash, 
                                           struct dt_pixel_cache_entry_t **cache_entry);

/**
 * @brief Decrease the ref_count and release the read lock over cache_entry all at once.
 * 
 * @param cache 
 * @param hash 
 * @param cache_entry 
 */
void dt_dev_pixelpipe_cache_close_read_only(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash, 
                                            struct dt_pixel_cache_entry_t *cache_entry);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
