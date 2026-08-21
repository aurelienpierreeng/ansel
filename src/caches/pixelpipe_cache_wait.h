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

/** @file caches/pixelpipe_cache_wait.h
 *
 * @brief The queue of consumers waiting for a pixel cacheline that does not exist yet.
 *
 * @details A GUI consumer that asks the cache for an output the pipeline has not published
 * yet -- a histogram, a colour picker, the darkroom surface, autoset -- leaves a request
 * here and is called back when a matching cacheline appears. The queue, its lock, its
 * counters and the rule for deciding which waiters a publication satisfies are cache state,
 * and this is where they live.
 *
 * What is deliberately NOT here: how the "a cacheline became ready" fact travels, and what a
 * pending queue looks like to the user. Those belong to whoever is running the cache.
 * `develop/` still owns both, because the transport must stay ASYNCHRONOUS and that is not a
 * style preference -- the ready fact is emitted from
 * dt_dev_pixelpipe_cache_wrlock_entry(FALSE, ...), which _cache_try_rekey_reuse_locked()
 * calls while holding the process-wide `cache->lock'. Restart callbacks re-enter the cache.
 * Serving them inline would re-lock a non-recursive mutex on the same thread and hang the
 * pipeline worker holding it, taking every other pipe with it.
 *
 * So this module hands back the list of waiters a publication satisfies and lets the caller
 * run them, on whatever thread the caller has already arranged to be safe.
 */

#ifndef DT_CACHES_PIXELPIPE_CACHE_WAIT_H
#define DT_CACHES_PIXELPIPE_CACHE_WAIT_H

#include <glib.h>
#include <stdint.h>

#include "system/dtpthread.h"

/** @brief Called when the awaited cacheline finally exists. Runs on the caller's thread. */
typedef void (*dt_pixelpipe_cache_ready_callback_t)(gpointer user_data);

/**
 * @brief One consumer's outstanding request, owned by that consumer, not by the queue.
 *
 * @details Callers embed this (dt_develop_t holds two) or keep one per widget, and hand its
 * address in. The queue only ever links it, matches it and hands it back.
 */
typedef struct dt_pixelpipe_cache_wait_t
{
  /* Identity only, never dereferenced: which pipe and which module this waiter is about, so a
   * re-request can be recognised as the same one and a teardown can cancel by owner. The queue
   * outlives neither object, which is why the labels below are copies rather than reads. */
  const void *pipe;
  const void *module;

  /* Captured when the wait is queued, for debug traces and telemetry keys. */
  char module_op[20];
  int module_multi_priority;
  int pipe_type;
  int32_t pipe_imgid;

  uint64_t hash;

  /** Producer node identity of the awaited output. Lets a waiter be served when its target
   *  module publishes, even if the exact awaited hash drifted before the publish -- the GUI
   *  predicts a hash, the worker recomputes them all. INVALID for a backbuf target, which has
   *  no single producing node and keeps to the exact-hash match. */
  uint64_t target_node_key;

  dt_pixelpipe_cache_ready_callback_t restart;
  gpointer user_data;
  const char *owner_tag;
  gpointer owner_object;
  uint64_t request_id;
  gboolean connected;
} dt_pixelpipe_cache_wait_t;

/**
 * @brief Queue @p wait, or refresh it in place if it is already queued for the same target.
 *
 * @return TRUE when the queue went from empty to non-empty, so the caller can raise whatever
 * "the user is waiting" state it owns. FALSE otherwise, including when the wait was already
 * queued unchanged.
 */
gboolean dt_pixelpipe_cache_wait_enqueue(dt_pixelpipe_cache_wait_t *wait);

/**
 * @brief Take every waiter satisfied by a publication of @p hash from node @p producer_node_key.
 *
 * @details Matches on the exact hash OR on the producing node, which is what makes the
 * protocol survive hash drift. Removes them from the queue and marks them disconnected.
 *
 * @param drained Set to TRUE when this emptied the queue.
 * @return A caller-owned GList of dt_pixelpipe_cache_wait_t*, to run and then free. Callbacks
 * are deliberately NOT run here: see the file comment on why the caller owns that step.
 */
GList *dt_pixelpipe_cache_wait_take_matching(uint64_t hash, uint64_t producer_node_key,
                                             gboolean *drained);

/**
 * @brief Remove @p wait from the queue if it is there, and reset it to an inert state.
 *
 * @param drained Set to TRUE when this emptied the queue.
 * @return TRUE when the wait was actually queued and has now been removed.
 */
gboolean dt_pixelpipe_cache_wait_cancel(dt_pixelpipe_cache_wait_t *wait, gboolean *drained);

/** @brief How many requests are outstanding. Diagnostics only. */
guint dt_pixelpipe_cache_wait_pending_count(void);

/** @brief Snapshot of the lifetime counters, for the dump. Any pointer may be NULL. */
void dt_pixelpipe_cache_wait_get_stats(uint64_t *queued, uint64_t *served, uint64_t *cancelled,
                                       uint64_t *immediate_hits, uint64_t *misses);

/** @brief Count one cache hit that never needed to queue. */
void dt_pixelpipe_cache_wait_count_immediate_hit(void);

/** @brief Count one miss that is about to queue. */
void dt_pixelpipe_cache_wait_count_miss(void);

/**
 * @brief Walk the outstanding requests. @p callback is invoked under the queue lock, so it
 * must not re-enter this module or the cache.
 */
typedef void (*dt_pixelpipe_cache_wait_visitor_t)(const dt_pixelpipe_cache_wait_t *wait,
                                                  int64_t age_us, gpointer user_data);
void dt_pixelpipe_cache_wait_foreach_pending(dt_pixelpipe_cache_wait_visitor_t callback,
                                             gpointer user_data);

#endif // DT_CACHES_PIXELPIPE_CACHE_WAIT_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
