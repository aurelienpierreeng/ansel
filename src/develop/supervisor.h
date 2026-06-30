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

#pragma once

#include <glib.h>
#include <stdint.h>

#include "common/darktable.h"

struct dt_image_t;
struct dt_iop_module_t;
struct dt_develop_blend_params_t;
struct dt_masks_form_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * High-level event supervisor.
 *
 * Ansel is deeply asynchronous: GUI, history and pixelpipe live in different
 * threads and identify their objects by content-addressed hashes. The
 * supervisor links those objects through the hashes they already carry and
 * emits one line of newline-delimited JSON (NDJSON) per CRUD event, with the
 * links already resolved.
 *
 * Registry as memory
 * ------------------
 * Every tracked object registers itself by its own hash, and declares (by hash)
 * the object it consumes and the parameter identity it derives from. Entries
 * live in the registry until the session ends — they are NEVER removed. A delete
 * event does not drop the entry; it flips an `alive` flag to FALSE. This turns
 * the registry into a memory: a CREATE or READ landing on a hash whose entry is
 * `alive == FALSE` is a *reuse-after-delete*, flagged `"resurrected": true` in
 * the emitted record. Such use-after-free patterns are otherwise invisible.
 *
 * Two hash families do all the linking (see reorganisation.md):
 *   - parameter identity: history_item.hash == module.hash == piece.hash;
 *   - output identity:     piece.global_hash == cache_entry.hash == backbuf.hash.
 * Pipeline nodes are topology objects, keyed by a stable synthetic key
 * (pipe, module instance) computed by dt_supervisor_node_key().
 *
 * Thread safety
 * -------------
 * The registry is guarded by ONE supervisor mutex. Emitters value-copy the
 * fields they are passed; the registry never holds a pointer to a live foreign
 * object and resolves at most two edge hops, so it never walks across lock
 * domains. The supervisor mutex is always taken as a leaf (never while another
 * supervisor call is in flight); taking it under the pixelpipe-cache lock at a
 * read chokepoint is safe because the ordering is never inverted.
 *
 * Everything is gated by `-d supervisor` (DT_DEBUG_SUPERVISOR). When off,
 * dt_supervisor_active() short-circuits each call site to one predicted-false
 * branch. See doc/supervisor.md for the schema and the jq recipes.
 */

// CRUD verb for an event.
typedef enum dt_sv_op_t
{
  DT_SV_CREATE = 0,
  DT_SV_UPDATE,
  DT_SV_READ,
  DT_SV_DELETE,
} dt_sv_op_t;

// Lifecycle, called once from dt_init()/dt_cleanup().
void dt_supervisor_init(void);
void dt_supervisor_cleanup(void);

// Runtime capture flag, set by the GUI (the supervisor window). The supervisor
// captures events whenever this is set OR `-d supervisor` is active. Read via
// the inline gate below; set via dt_supervisor_set_recording().
extern gint dt_supervisor_recording;

// Fast gate. TRUE when `-d supervisor` is enabled OR the GUI is recording. Guard
// call sites with this so the per-event metadata gathering is skipped when off.
static inline gboolean dt_supervisor_active(void)
{
  return (darktable.unmuted & DT_DEBUG_SUPERVISOR) || g_atomic_int_get(&dt_supervisor_recording);
}

/**
 * One retained event, as exposed to the GUI by dt_supervisor_events_snapshot().
 * `links` holds the resolved edge hashes (params/input/node/consumes/rekey) so
 * the GUI can make every id clickable and navigate to its declaration.
 */
typedef struct dt_sv_link_t
{
  char label[16]; // edge name, e.g. "params", "input", "node", "rekeyed_to"
  uint64_t hash;
} dt_sv_link_t;

typedef struct dt_sv_logged_event_t
{
  uint64_t seq;   // monotonic capture sequence, for incremental GUI updates
  double ts;
  char thread[24];
  char op[12];
  char domain[16];
  char mnemonic[64]; // human label for the row (module, imgid, widget, pipe, …)
  uint64_t hash;  // the event's own object hash
  GArray *links;  // of dt_sv_link_t
  gchar *json;    // the full NDJSON record (compact), for the detail view
} dt_sv_logged_event_t;

// Toggle runtime capture into the in-memory log (the stderr NDJSON dump stays
// governed by `-d supervisor`).
void dt_supervisor_set_recording(gboolean on);

// GUI thread: deep-copy snapshot of the retained log, oldest-first. Free with
// dt_supervisor_events_free(). Empty (not NULL) when nothing was captured.
GPtrArray *dt_supervisor_events_snapshot(void);

// Incremental snapshot: only events captured after `after_seq` (oldest-first).
// `out_last_seq` receives the highest seq returned (or `after_seq` if none), so
// a poller can advance its cursor. Free with dt_supervisor_events_free().
GPtrArray *dt_supervisor_events_snapshot_since(uint64_t after_seq, uint64_t *out_last_seq);

// Number of events currently retained.
guint dt_supervisor_events_count(void);

void dt_supervisor_events_free(GPtrArray *events);

// Drop all retained events.
void dt_supervisor_events_clear(void);

/**
 * Stable identity of a pipeline topology node: a function of the pipe type and
 * the module instance (op + multi_priority), independent of parameters or ROI.
 * The producer (create_nodes) and the consumers (a cacheline declaring its
 * producer) call this to agree on the same key. Never returns INVALID.
 */
uint64_t dt_supervisor_node_key(int pipe_type, const char *op_name, int multi_priority);

// Key under which thumbnail events for (imgid, mip) are registered. Lets the GUI
// memory view navigate from a mipmap cache item to its thumbnail event.
uint64_t dt_supervisor_thumbnail_key(int32_t imgid, int mip);

// Key under which mipmap cache objects (imgid, mip) are registered. Distinct
// from the thumbnail key; used by the GUI memory view to navigate from a mipmap
// cache item to the mipmap object's properties.
uint64_t dt_supervisor_mipmap_key(int32_t imgid, int mip);

// Key under which image cache objects (the canonical dt_image_t for an imgid)
// are registered. Distinct from the mipmap key.
uint64_t dt_supervisor_image_key(int32_t imgid);

/**
 * History item event — keyed by its parameter hash (== module hash == node
 * param hash), the join target nodes/cachelines resolve their `params` edge to.
 * DELETE flips `alive` to FALSE without dropping the entry.
 */
// `module`/`params` are optional: when the module exposes introspection, the
// parameters are rendered human-legibly under "parameters" (pass NULL/NULL to skip).
// `blend_params`, when given, is rendered under "blendop" (skipped when blending
// is disabled).
void dt_supervisor_history(dt_sv_op_t op, uint64_t param_hash, const char *op_name,
                           int multi_priority, const char *multi_name, int iop_order,
                           int history_index, int32_t imgid, gboolean enabled,
                           const struct dt_iop_module_t *module, const void *params,
                           const struct dt_develop_blend_params_t *blend_params, GList *forms);

// Key under which a mask form is registered (by its formid).
uint64_t dt_supervisor_form_key(int formid);

/**
 * Mask form lifecycle event — keyed by dt_supervisor_form_key(form->formid).
 * `create` at allocation (dt_masks_create), `update` when a history snapshot
 * carries the form (name/members filled in). Carries id, name, type and, for
 * groups, the member form ids.
 */
void dt_supervisor_form(dt_sv_op_t op, const struct dt_masks_form_t *form);

/**
 * Pipeline topology node event — keyed by dt_supervisor_node_key().
 * - CREATE when the pipe builds its nodes (topology), with param_hash INVALID;
 * - UPDATE when the node is synchronized with a history entry in
 *   dt_dev_pixelpipe_change(): `param_hash` is the committed piece->hash, which
 *   ties the node (and the cachelines it produces) to its history item;
 * - DELETE when the nodes are torn down.
 * Not emitted from the processing recursion.
 */
void dt_supervisor_node(dt_sv_op_t op, uint64_t node_hash, uint64_t param_hash,
                        uint64_t predecessor_hash, const char *op_name, int multi_priority,
                        int iop_order, int pipe_type, int32_t imgid);

/**
 * Cacheline output created at runtime publish — keyed by the produced output
 * hash (piece->global_hash). Carries the full linkage so later READ/DELETE
 * events (which only know the hash) resolve against the registry memory.
 * @param node_hash   producing topology node (dt_supervisor_node_key()).
 * @param param_hash  piece->hash, links to the history/module identity.
 * @param input_hash  previous enabled piece's output hash, or INVALID/0.
 * @param devid       OpenCL device id, or < 0 for CPU.
 */
void dt_supervisor_cacheline_create(uint64_t hash, uint64_t node_hash, uint64_t param_hash,
                                    uint64_t input_hash, const char *op_name, int multi_priority,
                                    int iop_order, int pipe_type, int32_t imgid, int roi_w,
                                    int roi_h, int devid, size_t size, const char *name);

// Cacheline read — every cache hit. Hash-only hot path; resolves from memory.
void dt_supervisor_cacheline_read(uint64_t hash, size_t size);

// Cacheline eviction/free — flips `alive` to FALSE, keeps the entry as memory.
void dt_supervisor_cacheline_delete(uint64_t hash, size_t size, int owner_pipe_id, const char *name);

/**
 * Rekey: an object's hash changed in place (a pipeline cacheline rekeyed for
 * reuse, or a history item overwritten so its parameter hash changed). Modelled
 * as "delete old hash + add new hash": the old entry is marked `alive == FALSE`
 * (emitting a `delete` with `rekeyed_to`), and the new entry inherits the old
 * metadata and is created/revived (emitting a `create` with `rekeyed_from`).
 * Domain-agnostic; the domain is derived from the old entry's facets.
 */
void dt_supervisor_rekey(uint64_t old_hash, uint64_t new_hash);

/**
 * Backbuffer event — keyed by the cacheline hash it promotes as the pipe output.
 * Merges into the cacheline entry sharing that hash, so the producing module is
 * resolved automatically.
 */
void dt_supervisor_backbuf(dt_sv_op_t op, uint64_t hash, uint64_t history_hash, int w, int h,
                           int bpp, int pipe_type, int devid);

/**
 * Widget paint event — declares the backbuf/cacheline hash the widget consumed.
 * The module/history narrative is resolved from the registry memory.
 */
void dt_supervisor_widget(dt_sv_op_t op, const char *widget_tag, uint64_t consumed_hash,
                          int pipe_type, int32_t imgid);

/**
 * Thumbnail generation event — a thumbnail widget fetching its mipmap (and the
 * pipeline render behind it, which surfaces through the generic node/cacheline/
 * backbuf events of the thumbnail pipe). Keyed by a synthetic (imgid, mip) key.
 */
void dt_supervisor_thumbnail(dt_sv_op_t op, int32_t imgid, int width, int height, int mip,
                             gboolean success);

/**
 * Mipmap cache object event — keyed by dt_supervisor_mipmap_key(). `create` when
 * the mipmap buffer is allocated/loaded, `delete` on eviction. It links to the
 * image cache object (an `image` edge) rather than duplicating the `dt_image_t`.
 */
void dt_supervisor_mipmap(dt_sv_op_t op, int32_t imgid, int mip);

/**
 * Image cache object event — the canonical dt_image_t for an imgid, keyed by
 * dt_supervisor_image_key(). `create` when the entry is loaded from the
 * database, `delete` on eviction. Its properties are its `dt_image_t`.
 */
void dt_supervisor_image(dt_sv_op_t op, int32_t imgid, const struct dt_image_t *img);

/**
 * Programmatic human-readable rendering of any tracked object, by hash.
 * Produces the same narrative the NDJSON encodes, e.g.
 *   "cacheline 0x71c4 (exposure/0, preview, 1920x1280, cpu) computed from
 *    input 0x55ab (colorin/0); params from history state #7"
 * Returns a newly-allocated string (caller g_free()s), or NULL if the hash is
 * unknown or the supervisor is inactive.
 */
gchar *dt_supervisor_describe(uint64_t hash);

#ifdef __cplusplus
}
#endif
