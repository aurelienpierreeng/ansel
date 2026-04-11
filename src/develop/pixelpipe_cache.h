/*
    This file is part of darktable,
    Copyright (C) 2009-2010 johannes hanika.
    Copyright (C) 2010-2011 Henrik Andersson.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012, 2014, 2016 Tobias Ellinghaus.
    Copyright (C) 2014 Ulrich Pegelow.
    Copyright (C) 2016 Roman Lebedev.
    Copyright (C) 2020 Pascal Obry.
    Copyright (C) 2020 Ralf Brown.
    Copyright (C) 2022 Hanno Schwalm.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2023, 2025-2026 Aurélien PIERRE.
    
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

#include "common/memory_arena.h"
#include "common/atomic.h"
#include "develop/format.h"
#include <inttypes.h>
#include <glib.h>
#include <stddef.h>

struct dt_dev_pixelpipe_t;
struct dt_iop_module_t;
struct dt_iop_roi_t;

#define DT_PIXELPIPE_CACHE_HASH_INVALID ((uint64_t)-1)


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
  // External (temporary) buffers keyed by address hash, separate from pipeline cache entries.
  GHashTable *external_entries;
  uint64_t next_serial;
  uint64_t queries;
  uint64_t hits;
  size_t max_memory;
  size_t current_memory;
  dt_pthread_mutex_t lock; // mutex to protect the cache entries
  dt_cache_arena_t arena;
} dt_dev_pixelpipe_cache_t;

typedef enum dt_dev_pixelpipe_cache_writable_status_t
{
  DT_DEV_PIXELPIPE_CACHE_WRITABLE_ERROR = -1,
  DT_DEV_PIXELPIPE_CACHE_WRITABLE_EXACT_HIT = -2,
  DT_DEV_PIXELPIPE_CACHE_WRITABLE_CREATED = 1,
  DT_DEV_PIXELPIPE_CACHE_WRITABLE_REKEYED = 2
} dt_dev_pixelpipe_cache_writable_status_t;

/** constructs a new cache with given cache line count (entries) and float buffer entry size in bytes.
  \param[out] returns 0 if fail to allocate mem cache.
*/
dt_dev_pixelpipe_cache_t *dt_dev_pixelpipe_cache_init(size_t max_memory);
void dt_dev_pixelpipe_cache_cleanup(dt_dev_pixelpipe_cache_t *cache);

/* Public for by-value snapshots in pipeline pieces (for example realtime
 * output cacheline reuse/rekey). Ownership still belongs to pixelpipe_cache.
 * External code must treat this as metadata only and never free internals. */
typedef struct dt_pixel_cache_entry_t
{
  uint64_t hash;            // unique identifier of the entry
  uint64_t serial;          // stable identity across rekeys, changes on fresh allocations
  void *data;               // buffer holding pixels... or anything else
  size_t size;              // size of the data buffer
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

/**
 * @brief Set the current module name for cache diagnostics (thread-local).
 *
 * @param module Module op name or NULL to clear.
 * @return const char* Previous module name.
 */
const char *dt_pixelpipe_cache_set_current_module(const char *module);

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
 * @param alloc Whether or not we should actually alloc the buffer, or simply reserve it. If FALSE
 * use `dt_pixel_cache_alloc()` when you actually need the buffer.
 * @param cache_entry a reference to the cache entry, to be reused later. Can be NULL. The caller
  doesn't own the data and shouldn't free it.
 * @return int 1 if the cache line was freshly allocated, 0 if it was found in the cache.
 */
int dt_dev_pixelpipe_cache_get(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash, const size_t size,
                               const char *name, const int id, const gboolean alloc,
                               void **data,
                               struct dt_pixel_cache_entry_t **entry);

/**
 * @brief Acquire a writable cache line for module output.
 *
 * @details
 * This is the cache-side companion of pixelpipe output computation. The caller already decided that the
 * published output for `hash` is not immediately reusable and needs to be overwritten.
 *
 * The cache then resolves how to provide that writable line:
 * - if an entry already exists at `hash`, that hash is already published and must not be overwritten.
 *   The caller must exact-hit it instead of recomputing,
 * - else, if `allow_rekey_reuse` is TRUE and `reuse_hint` still points to a live cacheline with the right size,
 *   that old cacheline is rekeyed to `hash`, write-locked, and returned,
 * - else, a new cacheline is created.
 *
 * In all successful cases:
 * - the returned entry refcount is incremented,
 * - the returned entry is write-locked,
 * - and `alloc` may materialize the host buffer if requested.
 *
 * The caller must later release the write lock and refcount from the same control flow.
 *
 * @param cache Pixelpipe cache.
 * @param hash Target output hash for the module output.
 * @param size Required buffer size in bytes.
 * @param name Debug label for the cache line.
 * @param id Owning pipeline id.
 * @param alloc Whether a host buffer must be materialized immediately.
 * @param allow_rekey_reuse Whether the cache may reuse the piece-local cached output line by rekeying it.
 * @param reuse_hint Snapshot of the previously attached piece cacheline metadata, or NULL.
 * @param[out] data Returned host pointer when available.
 * @param[out] entry Returned cache entry.
 * @return dt_dev_pixelpipe_cache_writable_status_t `DT_DEV_PIXELPIPE_CACHE_WRITABLE_CREATED`
 *         when creating a new entry, `DT_DEV_PIXELPIPE_CACHE_WRITABLE_REKEYED` when rekeying
 *         `reuse_hint`, `DT_DEV_PIXELPIPE_CACHE_WRITABLE_EXACT_HIT` when a published entry already
 *         exists at `hash` and the caller must exact-hit it, `DT_DEV_PIXELPIPE_CACHE_WRITABLE_ERROR`
 *         on error.
 */
dt_dev_pixelpipe_cache_writable_status_t
dt_dev_pixelpipe_cache_get_writable(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash,
                                    const size_t size, const char *name, const int id,
                                    const gboolean alloc, const gboolean allow_rekey_reuse,
                                    const struct dt_pixel_cache_entry_t *reuse_hint,
                                    void **data,
                                    struct dt_pixel_cache_entry_t **entry);

/**
 * @brief Borrow a cached OpenCL payload attached to a cache entry.
 *
 * @details
 * This reopens a cached `cl_mem` image already tracked by the cache entry and increments its
 * internal borrow counter so cache flush/eviction paths will keep it alive until the caller
 * returns it with `dt_dev_pixelpipe_cache_return_cl_payload()`.
 *
 * @param entry Cache entry owning the payload.
 * @param host_ptr Host pointer key for pinned payloads, or NULL for device-only payloads.
 * @param devid OpenCL device id.
 * @param width Image width.
 * @param height Image height.
 * @param bpp Bytes per pixel.
 * @param flags Tracked OpenCL flags used when the payload was cached.
 * @return void* Borrowed `cl_mem`, or NULL if no matching cached payload exists.
 */
void *dt_dev_pixelpipe_cache_borrow_cl_payload(struct dt_pixel_cache_entry_t *entry, void *host_ptr,
                                               int devid, int width, int height, int bpp, int flags);

/**
 * @brief Return a borrowed cached OpenCL payload to its cache entry.
 *
 * @param entry Cache entry owning the payload.
 * @param mem Borrowed `cl_mem` to release back to cache bookkeeping.
 */
void dt_dev_pixelpipe_cache_return_cl_payload(struct dt_pixel_cache_entry_t *entry, void *mem);

/**
 * @brief Flush all reusable OpenCL payloads cached on one cache entry.
 *
 * @details
 * Borrowed payloads are preserved until all borrowers returned them.
 *
 * @param entry Cache entry whose cached `cl_mem` payloads should be flushed.
 */
void dt_dev_pixelpipe_cache_flush_entry_clmem(struct dt_pixel_cache_entry_t *entry);

/**
 * @brief Materialize a host payload for a live cache entry from its cached device payload.
 *
 * @details
 * Recursive pixelpipe stages reopen transient upstream cache entries directly, before those
 * entries become valid exact-hits. When such an entry only carries a GPU payload, the caller
 * may still need a host pointer for CPU fallback or for a non-OpenCL module. This helper keeps
 * the recovery local to the cache layer:
 *
 * - allocate host RAM for `entry` when needed,
 * - copy the most relevant cached device payload back to host,
 * - return the restored host pointer through `data`.
 *
 * The function leaves the cache entry owned by the caller. It does not change refcounts.
 *
 * @param cache Pixelpipe cache.
 * @param entry Live cache entry to restore.
 * @param preferred_devid Preferred OpenCL device id, or `-1` for any.
 * @param[out] data Restored host pointer, or NULL on failure.
 * @return gboolean TRUE when host data is available after the call, FALSE otherwise.
 */
gboolean dt_dev_pixelpipe_cache_restore_host_payload(dt_dev_pixelpipe_cache_t *cache,
                                                     struct dt_pixel_cache_entry_t *entry,
                                                     int preferred_devid, void **data);

/**
 * @brief Acquire a pinned OpenCL image for a host buffer tracked by the pixelpipe cache.
 *
 * @details
 * This is the public helper for modules that manage their own host buffers but still want
 * the pixelpipe cache to own the reusable pinned `cl_mem` images. The helper accepts an
 * optional cache-entry hint for normal cache lines. If that hint is NULL, it will try to
 * resolve the host pointer against the cache's privately-owned external allocations.
 *
 * On reuse, the helper synchronizes host memory to the OpenCL image before returning.
 *
 * @param cache Pixelpipe cache.
 * @param host_ptr Host-backed image data.
 * @param entry_hint Optional owning cache entry for regular cache lines, or NULL.
 * @param devid OpenCL device id.
 * @param width Image width.
 * @param height Image height.
 * @param bpp Bytes per pixel.
 * @param flags OpenCL allocation flags (must include `CL_MEM_USE_HOST_PTR`).
 * @param[out] out_reused Optional flag set TRUE when an existing pinned image was reused.
 * @return void* OpenCL image (`cl_mem`) or NULL on failure.
 */
void *dt_dev_pixelpipe_cache_get_pinned_image(dt_dev_pixelpipe_cache_t *cache, void *host_ptr,
                                              struct dt_pixel_cache_entry_t *entry_hint, int devid,
                                              int width, int height, int bpp, int flags,
                                              gboolean *out_reused);

/**
 * @brief Release or cache a pinned OpenCL image acquired with
 * `dt_dev_pixelpipe_cache_get_pinned_image()`.
 *
 * @details
 * If the image is still a host-backed pinned allocation and an owning cache entry can be
 * resolved, it is returned to that cache entry for reuse. Otherwise it is released.
 *
 * @param cache Pixelpipe cache.
 * @param host_ptr Host-backed image data.
 * @param entry_hint Optional owning cache entry for regular cache lines, or NULL.
 * @param[in,out] mem Pointer to the `cl_mem` handle (cleared on return).
 */
void dt_dev_pixelpipe_cache_put_pinned_image(dt_dev_pixelpipe_cache_t *cache, void *host_ptr,
                                             struct dt_pixel_cache_entry_t *entry_hint, void **mem);

/**
 * @brief Drop cached pinned OpenCL images associated with a given host buffer.
 *
 * @details
 * This is meant for host buffers that remain alive and continue to be mutated by the CPU
 * (for example, privately owned working patches). Reusing a stale `CL_MEM_USE_HOST_PTR`
 * image for such buffers can hide later host-side edits on some drivers. Flushing the
 * cached pinned images forces the next use to rebind the host storage.
 *
 * @param cache Pixelpipe cache.
 * @param host_ptr Host-backed image data.
 * @param entry_hint Optional owning cache entry for regular cache lines, or NULL.
 * @param devid Device id to flush, or -1 for all cached devices for that host buffer.
 * @return gboolean TRUE if at least one pinned image was flushed, FALSE otherwise.
 */
gboolean dt_dev_pixelpipe_cache_flush_host_pinned_image(dt_dev_pixelpipe_cache_t *cache, void *host_ptr,
                                                        struct dt_pixel_cache_entry_t *entry_hint, int devid);

/**
 * @brief Allocate or reuse an OpenCL buffer for one cache entry payload.
 *
 * @details
 * This is the cache-owned buffer acquisition helper used by the OpenCL pixelpipe backend:
 *
 * - if `!IS_NULL_PTR(host_ptr)`, it may reuse or allocate a pinned `CL_MEM_USE_HOST_PTR` image,
 * - if `IS_NULL_PTR(host_ptr)`, it may reuse or allocate a device-only scratch image,
 * - allocation failures may trigger a cache-side `cl_mem` flush and one retry.
 *
 * @param devid OpenCL device id.
 * @param host_ptr Host backing store for pinned images, or NULL for device-only images.
 * @param roi Buffer dimensions.
 * @param bpp Bytes per pixel.
 * @param module Module for debug messages.
 * @param message Human-readable allocation context.
 * @param entry Owning cache entry.
 * @param reuse_pinned Whether pinned images may be reused from cache.
 * @param reuse_device Whether device-only images may be reused from cache.
 * @param[out] out_reused Optional flag set TRUE when the buffer came from cache reuse.
 * @param keep Optional OpenCL buffer that must not be flushed during retry.
 * @return void* OpenCL image (`cl_mem`) or NULL on failure.
 */
void *dt_dev_pixelpipe_cache_get_cl_buffer(int devid, void *host_ptr, const struct dt_iop_roi_t *roi,
                                           size_t bpp, struct dt_iop_module_t *module,
                                           const char *message, struct dt_pixel_cache_entry_t *entry,
                                           gboolean reuse_pinned, gboolean reuse_device,
                                           gboolean *out_reused, void *keep);

/**
 * @brief Allocate a temporary device-only OpenCL image, retrying once after cache flush.
 *
 * @param devid OpenCL device id.
 * @param roi Buffer dimensions.
 * @param bpp Bytes per pixel.
 * @param module Module for debug messages.
 * @param message Human-readable allocation context.
 * @param keep Optional OpenCL buffer that must not be flushed during retry.
 * @return void* OpenCL image (`cl_mem`) or NULL on failure.
 */
void *dt_dev_pixelpipe_cache_alloc_cl_device_buffer(int devid, const struct dt_iop_roi_t *roi, size_t bpp,
                                                    const struct dt_iop_module_t *module,
                                                    const char *message, void *keep);

/**
 * @brief Release or cache an OpenCL image associated with one cache entry.
 *
 * @details
 * Pinned host-backed images and reusable device-only images are returned to the cache entry when possible.
 * Otherwise the OpenCL object is released immediately and any stale cache bookkeeping for it is dropped.
 *
 * @param[in,out] cl_mem_buffer Pointer to the `cl_mem` handle. Cleared on return.
 * @param entry Owning cache entry, or NULL when the buffer is not cache-backed.
 * @param host_ptr Host pointer backing pinned images, or NULL for device-only images.
 * @param cache_device Whether device-only images may be kept for reuse.
 */
void dt_dev_pixelpipe_cache_release_cl_buffer(void **cl_mem_buffer, struct dt_pixel_cache_entry_t *entry,
                                              void *host_ptr, gboolean cache_device);

/**
 * @brief Synchronize between host memory and a pinned OpenCL image.
 *
 * @param devid OpenCL device id.
 * @param host_ptr Host pointer to read from / write to.
 * @param cl_mem_buffer OpenCL image.
 * @param roi Buffer dimensions.
 * @param cl_mode `CL_MAP_WRITE` for host→device, `CL_MAP_READ` for device→host.
 * @param bpp Bytes per pixel.
 * @param module Module for debug logs.
 * @param message Human-readable sync context.
 * @return int 0 on success, 1 on failure.
 */
int dt_dev_pixelpipe_cache_sync_cl_buffer(int devid, void *host_ptr, void *cl_mem_buffer,
                                          const struct dt_iop_roi_t *roi, int cl_mode, size_t bpp,
                                          struct dt_iop_module_t *module, const char *message);

/**
 * @brief Resynchronize one OpenCL input payload back into its cache-backed host buffer.
 *
 * @param pipe Current pixelpipe.
 * @param input Host cache buffer.
 * @param cl_mem_input OpenCL image holding the authoritative pixels.
 * @param roi_in Buffer dimensions.
 * @param module Module for debug logs.
 * @param in_bpp Bytes per pixel.
 * @param input_entry Owning cache entry.
 * @param message Human-readable sync context.
 * @return float* `input` on success, NULL on failure.
 */
float *dt_dev_pixelpipe_cache_restore_cl_buffer(struct dt_dev_pixelpipe_t *pipe, float *input,
                                                void *cl_mem_input, const struct dt_iop_roi_t *roi_in,
                                                struct dt_iop_module_t *module, size_t in_bpp,
                                                struct dt_pixel_cache_entry_t *input_entry,
                                                const char *message);

/**
 * @brief Prepare the OpenCL input image corresponding to one cache-backed module input.
 *
 * @details
 * This centralizes the decision tree for:
 *
 * - continuing from an already-live GPU payload,
 * - reopening or allocating a pinned input image from host RAM,
 * - taking the read lock required by true zero-copy buffers,
 * - and pushing host→device sync only when needed.
 *
 * @param pipe Current pixelpipe.
 * @param module Module being processed.
 * @param input Host input pointer.
 * @param[in,out] cl_mem_input OpenCL input image.
 * @param roi_in Buffer dimensions.
 * @param in_bpp Bytes per pixel.
 * @param input_entry Owning cache entry.
 * @param[out] locked_input_entry Returned cache entry that stayed read-locked for zero-copy safety.
 * @param keep Optional OpenCL buffer that must not be flushed during retry.
 * @return int 0 on success, 1 on failure.
 */
int dt_dev_pixelpipe_cache_prepare_cl_input(struct dt_dev_pixelpipe_t *pipe,
                                            struct dt_iop_module_t *module,
                                            float *input, void **cl_mem_input,
                                            const struct dt_iop_roi_t *roi_in, size_t in_bpp,
                                            struct dt_pixel_cache_entry_t *input_entry,
                                            struct dt_pixel_cache_entry_t **locked_input_entry,
                                            void *keep);

/**
 * @brief Resolve and retain the cache entry owning a host pointer.
 *
 * @details
 * This helper searches both regular and external pixelpipe cache tables for
 * the entry whose host buffer pointer exactly matches `host_ptr`.
 *
 * On success it increments the entry refcount before returning, so callers can
 * safely use the entry across asynchronous OpenCL operations.
 * Release it with:
 * `dt_dev_pixelpipe_cache_ref_count_entry(cache, FALSE, entry)`.
 *
 * @param cache Pixelpipe cache.
 * @param host_ptr Host buffer pointer to resolve.
 * @return dt_pixel_cache_entry_t* Owning cache entry with retained refcount, or NULL.
 */
struct dt_pixel_cache_entry_t *dt_dev_pixelpipe_cache_ref_entry_for_host_ptr(dt_dev_pixelpipe_cache_t *cache,
                                                                              void *host_ptr);

/** Peek the host data pointer of a cache entry without allocating. */
void *dt_pixel_cache_entry_get_data(struct dt_pixel_cache_entry_t *entry);

/**
 * @brief Peek the size (in bytes) reserved for the host buffer of a cache entry.
 *
 * @details
 * The pixelpipe cache treats `dt_pixel_cache_entry_t` as an internal/private structure. External users
 * (such as the pixelpipe implementation) should not access struct fields directly.
 *
 * This accessor is intentionally "peek-only": it does not allocate and it does not change ownership.
 */
size_t dt_pixel_cache_entry_get_size(struct dt_pixel_cache_entry_t *entry);

	                    
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
 * @brief Allocate aligned memory tracked by the pixelpipe cache. This allows
 * LRU cache entries to be evicted if needed to make room.
 *
 * @param cache Pixelpipe cache to manage.
 * @param size Buffer size in bytes.
 * @param id Pipeline type owning the buffer.
 * @param name Human-readable name.
 * @return void* Pointer to the allocated buffer, or NULL on failure.
 */
void *dt_pixelpipe_cache_alloc_align_cache_impl(dt_dev_pixelpipe_cache_t *cache, size_t size, int id,
                                                const char *name);

/**
 * @brief Free aligned memory allocated with dt_pixelpipe_cache_alloc_align_cache.
 *
 * @param cache Pixelpipe cache to manage.
 * @param mem Pointer to the buffer pointer. Set to NULL on successful free.
 */
void dt_pixelpipe_cache_free_align_cache(dt_dev_pixelpipe_cache_t *cache, void **mem, const char *message);

/**
 * @brief Non-owning lookup of an existing cache line.
 *
 * @details
 * This does not create a new cache line and does not change reference counts or entry locks.
 * Callers that need lifetime guarantees must retain the entry explicitly with
 * `dt_dev_pixelpipe_cache_ref_count_entry()` and/or `dt_dev_pixelpipe_cache_rdlock_entry()`.
 *
 * If `!IS_NULL_PTR(cl_mem_output)`, the lookup becomes authoritative for exact-hit consumers:
 * - write-locked / auto-destroy entries are rejected,
 * - host data is restored from cached device state when possible,
 * - device data is restored into `cl_mem_output` when requested,
 * - broken entries with neither authoritative RAM nor vRAM payload are removed.
 */
gboolean dt_dev_pixelpipe_cache_peek(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash, void **data,
                                     struct dt_pixel_cache_entry_t **entry, const int preferred_devid,
                                     void **cl_mem_output);

/**
 * @brief Remove cache lines matching id. Entries locked in read/write or having reference
 * count greater than 0 are not removed.
 *
 * @param cache
 * @param id ID of the pipeline owning the cache line, or -1 to remove all lines.
 */
void dt_dev_pixelpipe_cache_flush(dt_dev_pixelpipe_cache_t *cache, const int id);

/**
 * @brief Release cached OpenCL buffers for a device (-1 for all).
 *
 * @details
 * This is intentionally a lightweight VRAM-pressure/retry helper: it drops cached
 * `cl_mem` objects without taking per-entry write locks. Realtime paths rely on it
 * to free scratch OpenCL buffers without stalling in-flight renders.
 * 
 * @param keep OpenCL buffer to keep. Typically, you want to keep the input if you
 * are trying to make room for the output.
 */
void dt_dev_pixelpipe_cache_flush_clmem(dt_dev_pixelpipe_cache_t *cache, const int devid, void *keep);


/**
 * @brief Arbitrarily remove the cache entry matching hash. Entries
 * having a reference count > 0 (inter-thread locked) or being having their read/write lock
 * locked will be ignored. If force is TRUE, we ignore reference count, but not locks.
 *
 * @param cache
 * @param force
 */
int dt_dev_pixelpipe_cache_remove(dt_dev_pixelpipe_cache_t *cache, const gboolean force,
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
 * @param lock TRUE to lock, FALSE to unlock
 */
void dt_dev_pixelpipe_cache_ref_count_entry(dt_dev_pixelpipe_cache_t *cache, gboolean lock,
                                            struct dt_pixel_cache_entry_t *entry);

/**
 * @brief Lock or release the write lock on the entry
 *
 * @param cache
 * @param lock TRUE to lock, FALSE to release
 */
void dt_dev_pixelpipe_cache_wrlock_entry(dt_dev_pixelpipe_cache_t *cache, gboolean lock,
                                         struct dt_pixel_cache_entry_t *entry);


/**
 * @brief Lock or release the read lock on the entry
 *
 * @param cache
 * @param lock TRUE to lock, FALSE to release
 * @param entry The cache entry object to lock.
 */
void dt_dev_pixelpipe_cache_rdlock_entry(dt_dev_pixelpipe_cache_t *cache, gboolean lock,
                                         struct dt_pixel_cache_entry_t *entry);


/**
 * @brief Flag the cache entry as "auto_destroy". This is useful for short-lived/disposable
 * cache entries, that won't be needed in the future. These will be freed out of the typical LRU, aged-based
 * garbage collection. The thread that tagged this entry as "auto_destroy" is responsible for freeing it
 * as soon as it is done with it, using `dt_dev_pixelpipe_cache_auto_destroy_apply()`.
 * If not manually freed this way, the entry will be caught using the generic LRU garbage collection.
 *
 * @param cache
 */
void dt_dev_pixelpipe_cache_flag_auto_destroy(dt_dev_pixelpipe_cache_t *cache,
                                              struct dt_pixel_cache_entry_t *entry);

/**
 * @brief Free the entry if it has the flag "auto_destroy".
 * See `dt_dev_pixelpipe_cache_flag_auto_destroy()`.
 * This only removes entries whose reference count already dropped to 0 and whose lock is currently free.
 * Call it right after the final consumer releases its refcount, from the same control flow that flagged the
 * entry for auto-destruction. If another consumer still owns the entry, this becomes a no-op and generic cache
 * eviction or a later explicit retry will reap it once ownership reaches 0.
 *
 * @param cache
 */
void dt_dev_pixelpipe_cache_auto_destroy_apply(dt_dev_pixelpipe_cache_t *cache,
                                               struct dt_pixel_cache_entry_t *entry);

/**
 * @brief Find the entry matching hash, and decrease its ref_count if found.
 * 
 * @param cache 
 * @param hash 
 */
void dt_dev_pixelpipe_cache_unref_hash(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash);

/**
 * @brief Change the hash/key of an existing cache line in place, without
 * freeing, reallocating or invalidating the underlying entry.
 *
 * @details
 * This is useful when a cache line remains the authoritative buffer but the
 * caller needs its identity to follow a new logical snapshot (for example a new
 * module-parameter hash). The entry contents, locks, refcount and storage are
 * preserved; only the hash-table key and `entry->hash` are updated.
 *
 * The operation fails if:
 * - the source entry cannot be found,
 * - or another different entry already exists at `new_hash`.
 *
 * @param cache
 * @param old_hash Current key of the entry.
 * @param new_hash Desired new key.
 * @param entry Optional direct entry reference. May be NULL.
 * @return int 0 on success, 1 on error.
 */
int dt_dev_pixelpipe_cache_rekey(dt_dev_pixelpipe_cache_t *cache, const uint64_t old_hash,
                                 const uint64_t new_hash, struct dt_pixel_cache_entry_t *entry);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
