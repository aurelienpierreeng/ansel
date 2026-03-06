/*
    This file is part of the Ansel project.
    Copyright (C) 2026 Aurélien PIERRE.

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

#pragma once

#include "iop/iop_api.h"
#include "iop/drawlayer/paint.h"

/** @file
 *  @brief Background stroke worker API for drawlayer realtime painting.
 */

/** @brief Opaque worker state (thread, queue, stroke runtime). */
typedef struct dt_drawlayer_worker_t dt_drawlayer_worker_t;
/** @brief Callback processing one finished stroke on the deferred full-resolution worker. */
typedef gboolean (*dt_drawlayer_worker_finished_stroke_cb)(dt_iop_module_t *self,
                                                           const GArray *history,
                                                           float distance_percent);

/** @brief Initialize worker and bind external state mirrors. */
void dt_drawlayer_worker_init(dt_iop_module_t *self,
                              dt_drawlayer_worker_t **worker,
                              gboolean *painting,
                              gboolean *finish_commit_pending,
                              guint *stroke_sample_count,
                              uint32_t *current_stroke_batch,
                              dt_drawlayer_worker_finished_stroke_cb finished_stroke_cb);
/** @brief Stop worker and release all resources. */
void dt_drawlayer_worker_cleanup(dt_drawlayer_worker_t **worker);
/** @brief Query whether realtime/backend worker still has pending activity. */
gboolean dt_drawlayer_worker_active(const dt_drawlayer_worker_t *worker);
/** @brief Query whether any worker still has pending activity, including full-resolution replay. */
gboolean dt_drawlayer_worker_any_active(const dt_drawlayer_worker_t *worker);
/** @brief Request asynchronous commit once queues become idle. */
void dt_drawlayer_worker_request_commit(dt_drawlayer_worker_t *worker);
/** @brief Flush pending events and force commit transition. */
void dt_drawlayer_worker_flush_pending(dt_drawlayer_worker_t *worker);
/** @brief Wait until deferred full-resolution replay queue is idle. */
void dt_drawlayer_worker_flush_finished_strokes(dt_drawlayer_worker_t *worker);
/** @brief Clear preserved stroke runtime/history after a completed commit. */
void dt_drawlayer_worker_reset_stroke(dt_drawlayer_worker_t *worker);
/** @brief Read-only access to preserved emitted dab history (valid only while worker is idle). */
GArray *dt_drawlayer_worker_history(dt_drawlayer_worker_t *worker);
/** @brief Read-only access to preserved stroke runtime (valid only while worker is idle). */
dt_drawlayer_paint_stroke_t *dt_drawlayer_worker_stroke(dt_drawlayer_worker_t *worker);
/** @brief Query whether the current preserved stroke has already been handed off for full-resolution replay. */
gboolean dt_drawlayer_worker_finished_stroke_queued(const dt_drawlayer_worker_t *worker);

/** @brief Enqueue one raw input event (FIFO, no coalescing). */
gboolean dt_drawlayer_worker_enqueue_input(dt_drawlayer_worker_t *worker,
                                           const dt_drawlayer_paint_raw_input_t *input);
/** @brief Enqueue stroke-end marker carrying final raw input sample. */
gboolean dt_drawlayer_worker_enqueue_stroke_end(dt_drawlayer_worker_t *worker,
                                                const dt_drawlayer_paint_raw_input_t *input);
