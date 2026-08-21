/*
    This file is part of Ansel.
    Copyright (C) 2026 Aurélien Pierre.

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Ansel is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Ansel.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "caches/pixelpipe_cache_wait.h"
#include "caches/pixelpipe_cache.h"   // DT_PIXELPIPE_CACHE_HASH_INVALID
#include "system/macros.h"            // IS_NULL_PTR
#include "system/mem_alloc.h"         // dt_free

#include <stdlib.h>                   // calloc

/* The queue lives here and nothing else does. No control/, no gui/, no develop/: this file is
 * layer 1 and stays that way, which is what lets tools/check_module_boundaries.sh keep its
 * caches_upcall_baseline at 9.
 */

typedef struct dt_pixelpipe_cache_wait_record_t
{
  dt_pixelpipe_cache_wait_t *wait;
  uint64_t request_id;
  int64_t queued_at_us;
} dt_pixelpipe_cache_wait_record_t;

typedef struct dt_pixelpipe_cache_wait_queue_t
{
  dt_pthread_mutex_t lock;
  GList *pending;
  uint64_t next_request_id;
  uint64_t queued_requests;
  uint64_t served_requests;
  uint64_t cancelled_requests;
  uint64_t immediate_hits;
  uint64_t misses;
} dt_pixelpipe_cache_wait_queue_t;

static dt_pixelpipe_cache_wait_queue_t _queue
    = { .lock = { PTHREAD_MUTEX_INITIALIZER }, .pending = NULL, .next_request_id = 1,
        .queued_requests = 0, .served_requests = 0, .cancelled_requests = 0,
        .immediate_hits = 0, .misses = 0 };

/** Unlink one record without touching the wait it points at. Caller holds the lock. */
static void _unlink_locked(GList *link)
{
  dt_pixelpipe_cache_wait_record_t *record = link->data;
  _queue.pending = g_list_delete_link(_queue.pending, link);
  dt_free(record);
}

/** Find the record linking @p wait, or NULL. Caller holds the lock. */
static GList *_find_locked(const dt_pixelpipe_cache_wait_t *wait)
{
  for(GList *iter = _queue.pending; iter; iter = g_list_next(iter))
  {
    const dt_pixelpipe_cache_wait_record_t *record = iter->data;
    if(!IS_NULL_PTR(record) && record->wait == wait) return iter;
  }
  return NULL;
}

gboolean dt_pixelpipe_cache_wait_enqueue(dt_pixelpipe_cache_wait_t *wait)
{
  if(IS_NULL_PTR(wait)) return FALSE;

  dt_pthread_mutex_lock(&_queue.lock);

  const gboolean was_empty = IS_NULL_PTR(_queue.pending);

  // Already queued for this exact target: nothing to add. The caller re-asks on every redraw,
  // and each of those must not grow the queue.
  if(wait->connected && !IS_NULL_PTR(_find_locked(wait)))
  {
    dt_pthread_mutex_unlock(&_queue.lock);
    return FALSE;
  }

  dt_pixelpipe_cache_wait_record_t *record
      = (dt_pixelpipe_cache_wait_record_t *)calloc(1, sizeof(dt_pixelpipe_cache_wait_record_t));
  if(IS_NULL_PTR(record))
  {
    dt_pthread_mutex_unlock(&_queue.lock);
    return FALSE;
  }

  wait->request_id = _queue.next_request_id++;
  wait->connected = TRUE;
  record->wait = wait;
  record->request_id = wait->request_id;
  record->queued_at_us = g_get_monotonic_time();
  _queue.pending = g_list_prepend(_queue.pending, record);
  _queue.queued_requests++;

  dt_pthread_mutex_unlock(&_queue.lock);
  return was_empty;
}

GList *dt_pixelpipe_cache_wait_take_matching(const uint64_t hash, const uint64_t producer_node_key,
                                             gboolean *drained)
{
  GList *taken = NULL;
  const gboolean node_key_valid
      = producer_node_key != 0 && producer_node_key != DT_PIXELPIPE_CACHE_HASH_INVALID;

  dt_pthread_mutex_lock(&_queue.lock);
  for(GList *iter = _queue.pending; iter;)
  {
    GList *next = g_list_next(iter);
    const dt_pixelpipe_cache_wait_record_t *record = iter->data;
    dt_pixelpipe_cache_wait_t *wait = !IS_NULL_PTR(record) ? record->wait : NULL;

    // Exact hash, or the node that produced it. The node match is what survives hash drift:
    // the waiter registered the hash it predicted, the worker published a different one for
    // the same module output, and the waiter still wants waking -- its restart re-reads the
    // module's current output hash and hits.
    const gboolean node_match
        = node_key_valid && !IS_NULL_PTR(wait) && wait->target_node_key == producer_node_key;
    if(!IS_NULL_PTR(wait) && wait->connected && (wait->hash == hash || node_match))
    {
      _unlink_locked(iter);
      _queue.served_requests++;
      wait->connected = FALSE;
      taken = g_list_prepend(taken, wait);
    }
    iter = next;
  }
  if(!IS_NULL_PTR(drained)) *drained = IS_NULL_PTR(_queue.pending);
  dt_pthread_mutex_unlock(&_queue.lock);

  return taken;
}

gboolean dt_pixelpipe_cache_wait_cancel(dt_pixelpipe_cache_wait_t *wait, gboolean *drained)
{
  if(IS_NULL_PTR(wait)) return FALSE;

  dt_pthread_mutex_lock(&_queue.lock);
  GList *link = _find_locked(wait);
  const gboolean was_queued = !IS_NULL_PTR(link);
  if(was_queued)
  {
    _unlink_locked(link);
    _queue.cancelled_requests++;
  }
  if(!IS_NULL_PTR(drained)) *drained = IS_NULL_PTR(_queue.pending);
  dt_pthread_mutex_unlock(&_queue.lock);

  wait->connected = FALSE;
  return was_queued;
}

guint dt_pixelpipe_cache_wait_pending_count(void)
{
  dt_pthread_mutex_lock(&_queue.lock);
  const guint count = g_list_length(_queue.pending);
  dt_pthread_mutex_unlock(&_queue.lock);
  return count;
}

void dt_pixelpipe_cache_wait_get_stats(uint64_t *queued, uint64_t *served, uint64_t *cancelled,
                                       uint64_t *immediate_hits, uint64_t *misses)
{
  dt_pthread_mutex_lock(&_queue.lock);
  if(!IS_NULL_PTR(queued)) *queued = _queue.queued_requests;
  if(!IS_NULL_PTR(served)) *served = _queue.served_requests;
  if(!IS_NULL_PTR(cancelled)) *cancelled = _queue.cancelled_requests;
  if(!IS_NULL_PTR(immediate_hits)) *immediate_hits = _queue.immediate_hits;
  if(!IS_NULL_PTR(misses)) *misses = _queue.misses;
  dt_pthread_mutex_unlock(&_queue.lock);
}

void dt_pixelpipe_cache_wait_count_immediate_hit(void)
{
  // trylock, as the counter is diagnostic only: a consumer reopening an already-available
  // cacheline must not block behind a queue operation to record it.
  if(!dt_pthread_mutex_trylock(&_queue.lock))
  {
    _queue.immediate_hits++;
    dt_pthread_mutex_unlock(&_queue.lock);
  }
}

void dt_pixelpipe_cache_wait_count_miss(void)
{
  if(!dt_pthread_mutex_trylock(&_queue.lock))
  {
    _queue.misses++;
    dt_pthread_mutex_unlock(&_queue.lock);
  }
}

void dt_pixelpipe_cache_wait_foreach_pending(dt_pixelpipe_cache_wait_visitor_t callback,
                                             gpointer user_data)
{
  if(IS_NULL_PTR(callback)) return;

  const int64_t now = g_get_monotonic_time();
  dt_pthread_mutex_lock(&_queue.lock);
  for(const GList *iter = _queue.pending; iter; iter = g_list_next(iter))
  {
    const dt_pixelpipe_cache_wait_record_t *record = iter->data;
    if(IS_NULL_PTR(record) || IS_NULL_PTR(record->wait)) continue;
    callback(record->wait, now - record->queued_at_us, user_data);
  }
  dt_pthread_mutex_unlock(&_queue.lock);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
