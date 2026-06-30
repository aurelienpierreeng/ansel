# Event supervisor — linked NDJSON tracing of async state

Ansel's GUI, history and pixelpipe live in separate threads and identify their
objects by content-addressed hashes (see `reorganisation.md` and
`pipeline-cache.md`). Reconstructing *which cacheline a widget painted from*, or
*which history state a pipeline node committed*, from ordinary logs is nearly
impossible. The **supervisor** solves this by linking those objects through the
hashes they already carry, and emitting one line of NDJSON per CRUD event with
the links already resolved.

Code: `src/develop/supervisor.{c,h}`. Enabled with `-d supervisor`; off by
default and effectively free when off (one predicted-false branch per site).

## The model

### Two hash families do the linking

- **Parameter identity** (`history_item.hash` == `module.hash` == `piece.hash`) —
  the `module → history item` spine and a cacheline's `params` edge.
- **Output identity** (`piece.global_hash` == `cache_entry.hash` ==
  `backbuf.hash`) — the cache key; a cacheline, the backbuffer that promotes it
  and the widget that paints it all share this hash.

### Pipeline nodes are topology, not runtime

A pipeline node is a *topology* object: created once when the pipe builds its
nodes and immutable until the topology changes. It is therefore keyed by a
**stable synthetic key** — `dt_supervisor_node_key(pipe_type, op, multi_priority)`
— and emitted only at `create_nodes` (CREATE) and `cleanup_nodes` (DELETE),
**never** from the processing recursion. The per-run object is the *cacheline*
(the node's output for a given parameter/ROI state), which is created at runtime
publish and carries the edge back to its producing node.

### Registry as memory: entries never die, they go `alive: false`

Every object registers itself by its hash; entries **live until the session
ends and are never removed**. A delete event flips an `alive` flag to `false`
instead of dropping the entry. This makes the registry a memory:

- a CREATE or READ landing on a hash whose entry is `alive: false` is a
  **reuse-after-delete**, tagged `"resurrected": true` in the record;
- `describe()` and edge resolutions mark dead references `[deleted]`, so a node
  consuming an evicted input is visible even after the fact.

This is the main tool for hunting use-after-free / use-after-evict bugs.

### Rekeying is delete-old + add-new

Two objects mutate their identity in place: a **pipeline cacheline** rekeyed for
writable reuse, and a **history item** overwritten so its parameter hash changes
(e.g. dragging a slider reuses the top history entry). The supervisor models
both as `dt_supervisor_rekey(old, new)`: the old entry goes `alive: false` with a
`"rekeyed_to"` link (a `delete` record), and a new entry inherits the old
metadata and is created with a `"rekeyed_from"` link (a `create` record). The two
hashes are therefore explicitly chained instead of one silently mutating.

A cacheline rekey is triggered by a parameter change, so the new cacheline must
reference the *triggering* history item, not the old one. The producing node was
bound to the new history item at synchronization (before the rekey runs), so the
rekey takes the `params` from the node's current binding rather than the stale
old cacheline param, and the rekey records carry clickable `node` and `params`
links (`cacheline → node → params`).

### Thread safety

One supervisor mutex guards the registry. Emitters value-copy their arguments;
the registry never dereferences a live foreign object and resolves at most two
edge hops. The mutex is a leaf — at the cache read chokepoints it is taken under
the pixelpipe-cache lock, and that ordering is never inverted.

## Running it

```sh
ansel -d supervisor 2> events.ndjson
```

One compact JSON object per line on **stderr**. The join is done in-app, so the
lines are already linked; `jq` is for filtering/shaping, not joining.

## GUI: the event supervisor window

*Help → Event supervisor…* opens a browsable view of the in-memory event log
(`src/gui/actions/supervisor_window.c`), laid out as a `GtkNotebook` (Timeline /
Grouped / Memory / Search) with a global toolbar (Record, Refresh, Clear, Group
by, event count, and a **search entry**). It does not need the `-d supervisor`
flag: a **Record** check button toggles runtime capture
(`dt_supervisor_set_recording`). Capture is **on while the window is open** and
stops when it closes — so events are recorded live from when you open it (do the
actions you want to trace with the window open; e.g. open it, then edit in
darkroom to see history/node/cacheline/backbuf/widget events stream in).

- **Timeline** — events in chronological order, **live-updating** (polled every
  300 ms; auto-scrolls to the tail when you are already at the bottom). One
  collapsible row each: the collapsed line shows the common fields (ts, op,
  colored domain, a per-domain **mnemonic** — module/instance for
  cacheline/node/history, image id for mipmap/image/thumbnail, widget tag for
  widget, pipe for backbuf — own hash, thread); expanding reveals the linked-object hashes
  and the full pretty-printed record (built lazily on first expand). The timeline
  keeps the most recent rows (older ones are trimmed; the full log stays in the
  supervisor).
- **Grouped** — the same rows bucketed and **collapsible** by group, keyed by a
  selectable field (*domain / thread / op*). **Live**: new events are appended
  into their bucket incrementally (preserving folds), like the timeline. A full
  rebuild happens only on a group-by change or Clear.
- **Memory** — live state of the two RAM caches, queried directly (not from the
  event log): for the **pipeline cache** and the **mipmap cache**, a usage bar
  (*current / max MiB*, max being the user-specified budget) and the stored items
  sorted by size. Pipeline items show their cacheline `hash`, size, refcount and
  hits; mipmap items show `image #id · mip N` and size. When the build has OpenCL
  and it is enabled, the pipeline cache also shows a second **vRAM** bar (device
  buffers attached to cachelines, summed via `dt_opencl_get_mem_object_size()`,
  over the total device memory), and each pipeline item that holds GPU buffers
  notes `+N MiB vRAM (k buf)` in a right-aligned column. A third **image cache**
  section lists the cached `dt_image_t` objects (`image #id` + filename), each
  linking to its `image` event. Each item is a link:
  clicking jumps to that object's event in the timeline (a pipeline item to its
  cacheline `create`, a mipmap item to its `mipmap` object event — whose detail
  shows the `dt_image_t` properties — via the synthetic `(imgid, mip)` key).
  Refreshed when shown and throttled (~1 s) while visible.
  Introspection comes from `dt_dev_pixelpipe_cache_get_{usage,entries_stats}()`
  and `dt_mipmap_cache_get_{usage,entries_stats}()`.
- **Search** — query events by hash *or* free text. Type into the global search
  entry (which auto-switches to the Search page): a hash (with or without `0x`, or
  any substring of the hex) matches the events whose own hash or any linked hash
  contains it; any other text is matched case-insensitively against the whole
  record (module/op/domain/widget/filename/parameters/…). It updates on input /
  Refresh (not on a timer, so expanded rows are not collapsed under you).
- **Every hash is a link.** The hash sits in a label separate from the expander
  toggle, so clicking it activates the link (rather than collapsing the row): it
  switches to the timeline and jumps to the *declaration* (the `create` event) of
  that object, expanding and selecting its row — so you can walk
  `widget → backbuf → cacheline → params → history` by clicking. **Right-clicking**
  a hash offers *Search for this hash*, which fills the search entry and switches
  to the Search page.

**Refresh** reloads from a fresh snapshot, **Clear** empties the log. The window
reads through `dt_supervisor_events_snapshot{,_since}()` (deep-copied,
thread-safe).

## Programmatic interface

Beyond the NDJSON stream, any tracked object can be rendered to a human sentence
in-process:

```c
gchar *s = dt_supervisor_describe(hash);   // caller g_free()s; NULL if unknown/off
// e.g. "cacheline 0x71c4… (exposure/0, preview, 1920x1280, cpu)
//       computed from input 0x55ab… (colorin/0); params 0x9f3a… (exposure/0)"
```

This is the same narrative the NDJSON encodes, available for a future tracker
widget, an assertion message, or ad-hoc logging.

## Event schema

Common envelope:

| field | meaning |
| --- | --- |
| `ts` | seconds since app start (same clock as `dt_print`) |
| `thread` | calling thread tag |
| `op` | `create` \| `update` \| `read` \| `delete` |
| `domain` | `history` \| `node` \| `cacheline` \| `backbuf` \| `widget` \| `thumbnail` |
| `hash` | this object's own hash (`0x…`) |
| `alive` | whether the represented object still exists |
| `resurrected` | present (`true`) only on a reuse-after-delete |
| `pipe` / `imgid` | when applicable |

Domain specifics:

- **history** — `module`, `iop_order`, `history_index`, `enabled`, and (when the
  module exposes introspection) the human-legible module parameters under
  `parameters` (rendered the same way `libs/history.c` does its tooltips, via
  `module->get_introspection()`). The blend/masking parameters are rendered under
  `blendop` (manually, since blend params have no introspection — mask mode,
  colorspace, blend mode/operation, opacity, feathering, mask blur/contrast/
  brightness, drawn/raster mask ids; omitted when blending is disabled). The
  drawn masks attached to the item (`hist->forms`) are listed under `forms`
  (each: `id`, `name`, `type`, and group `members`), and each is also registered
  as its own `form` object. Also holds the image `filename`. Keyed by the
  parameter hash. `delete` flips `alive`.
- **form** — a mask form, keyed by `dt_supervisor_form_key(formid)`. `create` at
  `dt_masks_create()` (type only; name/points not filled yet), `update` whenever
  a history snapshot carries it (name + group members filled in). Carries `id`,
  `name`, `type` (circle/ellipse/path/brush/gradient/group) and, for groups, the
  `members` (each `{ id, hash }`, the hash being a **clickable link** to the
  member form). The history item's inline `forms` entries are likewise clickable
  links to the form objects. (Free is not hooked: `dt_masks_free_form` also fires for the
  many transient history-snapshot copies that share a `formid`, so it is not a
  reliable delete signal — forms persist in the registry as memory.)
- **node** — `module`, `iop_order`. Topology object, keyed by the synthetic node
  key. `create` at `create_nodes` (no params yet) — also carries a `predecessor`
  edge to the node before it in the pipeline (iop_order order); `update` at
  history synchronization (`dt_dev_pixelpipe_change()` → the sync loops), where
  the committed `piece->hash` binds the node to its history item, so the `update`
  carries a resolved `params` link; `delete` at `cleanup_nodes`.
- **cacheline** — `create` at output publish with full linkage (`device`, `roi`,
  resolved `params`/`input`/`node`); `read` on **every** cache hit (resolved
  `params`/`node`/`input` from memory, so every related hash is clickable from a
  read too); `delete` on eviction (resolved `params`/`node`, flips `alive`).
  A cacheline is thus tied to its history item *through the node that produced
  it*: `cacheline → node → params (history)`, plus the direct `cacheline → params`
  edge — all clickable in the GUI from the cacheline event.

  > **Why the link goes through the node.** A piece's `param_hash` is `piece->hash`,
  > which equals `module->hash` (the history entry key) *only* for modules that do
  > not opt into `runtime_data_hash()`. Modules that do (e.g. colorbalancergb,
  > ashift, crop, drawlayer) fold their committed `piece->data` into `piece->hash`,
  > so it no longer matches any history entry. The node is therefore bound at
  > synchronization to the matched history item's own hash (`hist->hash`), and the
  > cacheline resolves its `params` edge from the node's binding rather than from
  > `piece->hash` — so cachelines of `runtime_data_hash()` modules are tied to
  > history too, not just the simple ones.
- **backbuf** — `device`, `history_hash`, `size` `[w,h,bpp]`, resolved `module`/
  `params`; merges into the cacheline entry sharing its hash.
- **widget** — `widget` tag, resolved `consumes`/`params`. The consumed hash is
  `hash`. Emitted on surface rebind, not every expose.
- **thumbnail** — `mip`, `size`, `success`; keyed by a synthetic `(imgid, mip)`
  key. `read` when generation starts, `update` with the result. It also carries a
  `mipmap` edge (the `dt_supervisor_mipmap_key(imgid, mip)` of the buffer it
  displays), so the displayed mipmap hash is visible and clickable. The pipeline
  render behind a thumbnail miss surfaces through the generic node/cacheline/
  backbuf events of the thumbnail pipe.
- **mipmap** — a mipmap *cache object*, keyed by a distinct synthetic
  `(imgid, mip)` key. `create` when the buffer is allocated/loaded, `delete` on
  eviction. It does **not** duplicate the `dt_image_t`; instead it carries an
  `image` edge linking to the image cache object (below), which holds those
  properties. This is the object the **Memory** view's mipmap items link to.
- **image** — an image *cache object* (the canonical `dt_image_t` for an imgid),
  keyed by a distinct synthetic `imgid` key. `create` when loaded from the
  database, `delete` on eviction; same `properties` subset as `mipmap`. The
  Memory view's image-cache items link to it.

### Example: a node computed its output

```json
{"ts":12.4,"thread":"thread-0x7f…","op":"create","domain":"cacheline",
 "pipe":"preview","imgid":42,"hash":"0x71c4…","alive":true,
 "module":"exposure/0","iop_order":14,"size":9830400,"device":"cpu","roi":[1920,1280],
 "params":{"hash":"0x9f3a…","module":"exposure/0","history_index":7,"enabled":true,"alive":true},
 "input":{"hash":"0x55ab…","module":"colorin/0","iop_order":12,"alive":true},
 "node":{"hash":"0x3e10…","module":"exposure/0","iop_order":14,"alive":true}}
```

### Example: reuse after delete

```json
{"ts":13.0,"op":"read","domain":"cacheline","hash":"0x55ab…","alive":true,
 "resurrected":true,"module":"colorin/0","size":9830400}
```
Something read `0x55ab…` after it had been evicted — exactly the pattern behind
"data pending" / stale-buffer bugs.

## jq recipes

Every reuse-after-delete in the session:

```sh
jq -c 'select(.resurrected==true)' events.ndjson
```

Lifecycle of one cacheline (create → reads → delete):

```sh
jq -c 'select(.hash=="0x71c4…") | {ts,op,alive}' events.ndjson
```

Which history state each painted frame came from:

```sh
jq -c 'select(.domain=="widget") | {ts,widget,frame:.hash,
       state:.params.history_index}' events.ndjson
```

## Instrumented sites

| domain | op | site |
| --- | --- | --- |
| history | create | `dt_dev_read_history_ext()` finalize loop (each item registered at read time, so resync can map nodes to it) |
| history | create/update | `dt_dev_add_history_item_ext()` (`dev_history.c`); in-place hash change → rekey |
| history | delete | canonical removals: `dt_dev_history_free_history()` (clear/compress), `dt_dev_history_truncate()` loop, and the leak-removal path — undo-snapshot copies are deliberately not hooked |
| rekey | delete+create | `dt_dev_pixelpipe_cache_rekey()` and `_cache_try_rekey_reuse_locked()` (`pixelpipe_cache.c`); history in-place overwrite (`dev_history.c`) |
| node | create/delete | `dt_dev_pixelpipe_create_nodes()` / `dt_dev_pixelpipe_cleanup_nodes()` (`pixelpipe_hb.c`) |
| node | update (history bind) | `_sync_pipe_nodes_from_history{,_from_node}()` after `_commit_piece_contract()` (`dev_pixelpipe.c`) |
| cacheline | create | output publish in `dt_dev_pixelpipe_process_rec()` (`pixelpipe_hb.c`) |
| cacheline | read | the three cache-hit chokepoints in `pixelpipe_cache.c` |
| cacheline | delete | `_free_cache_entry()` (`pixelpipe_cache.c`) |
| backbuf | update | after `dt_dev_set_backbuf()` (`pixelpipe_hb.c`) |
| widget | read | surface rebind in `_lock_pipe_surface()` (`views/darkroom.c`) |
| thumbnail | read/update | inside `_view_image_get_surface_internal()` (`views/view.c`), where the real mip level is known |
| mipmap | create/delete | `dt_mipmap_cache_allocate_dynamic()` / `dt_mipmap_cache_deallocate_dynamic()` (`common/mipmap_cache.c`); create fetches the `dt_image_t` for its properties |
| image | create/delete | `dt_image_cache_allocate()` / `dt_image_cache_deallocate()` (`common/image_cache.c`) |
| form | create | `dt_masks_create()` (`develop/masks/masks.c`); update from history snapshots |

## Limits / TODO

- The registry grows for the whole session by design (it is the memory). On very
  long sessions an LRU cap that preserves `alive: false` tombstones would bound
  it.
- Cache **read** events are high frequency (every module input acquisition). They
  are the price of "track all hits"; filter with `jq 'select(.op!="read")'` for a
  topology-level view.
- The supervisor only *links* state; it never changes behaviour.
