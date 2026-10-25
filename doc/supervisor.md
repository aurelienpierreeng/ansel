# Event supervisor — linked NDJSON tracing of async state

> **First checked 2026-09-29.** This file was mechanically checked against `8f4638a04e` on 2026-09-29 — every `file:line` citation resolved, every backticked symbol looked up in the tree, every OPEN/planned status claim tested, and every gate or baseline number it quotes compared with `tools/check_module_boundaries.sh` and `tools/include_baseline.txt`. **No per-claim semantic read was done**: a citation that resolves can still describe the wrong thing, so this is a floor, not a verification. One symbol name corrected (`dt_dev_lock_pipe_surface()`).

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
(`src/gui/actions/supervisor_window.c`), laid out as a `GtkNotebook` (Memory /
Timeline / Grouped / Search) with a global toolbar (Record, Refresh, Clear, Group
by, event count, and a **search entry**). It does not need the `-d supervisor`
flag: a **Record** check button toggles runtime capture
(`dt_supervisor_set_recording`). Capture is **on while the window is open** and
stops when it closes — so events are recorded live from when you open it (do the
actions you want to trace with the window open; e.g. open it, then edit in
darkroom to see history/node/cacheline/backbuf/widget events stream in).
Unticking **Record** is kept, so the next window opens with capture off. The
page shown, the *Group by* key and the Record box are kept in the conf as the user
changes them -- `ui/supervisor/page` (by name: `memory`, `timeline`, `grouped`,
`search`, so that it survives a change in the order of the tabs),
`ui/supervisor/group_by`, `ui/supervisor/record` -- and restored when the window
opens; with nothing stored, the Memory page, grouped by domain, recording. The
page is selected before the notebook's `switch-page` handler is connected:
appending the first page makes it current, which would otherwise be written over
the stored page.
The window opens where it was left and as large: its geometry is written to
`ui/supervisor/window_{x,y,width,height}` on every `configure-event` -- not on
close, since Ansel can quit with the window open -- except while it is maximized
or fullscreen, and restored before it is mapped, the position clamped into the
work area of the monitor it lands on. Under Wayland, which gives a client neither
its own position nor the right to set it, the size only (as the masks panel,
`libs/shape_manager.c`).

- **Timeline** — events in chronological order, **live-updating** (polled every
  300 ms; auto-scrolls to the tail when you are already at the bottom). One
  collapsible row each: the collapsed line shows the common fields (ts, op,
  colored domain, a per-domain **mnemonic** — module/instance for
  cacheline/node/history, image id for mipmap/image/thumbnail, widget tag for
  widget, pipe for backbuf, `owner→target` for cache-wait — own hash, thread);
  expanding reveals the linked-object hashes
  and the full pretty-printed record (built lazily on first expand). The timeline
  keeps the most recent 4000 rows (`TIMELINE_MAX_ROWS`; the full log, up to 20000
  events, stays in the supervisor). In GTK 3 a row appended to a list box in the
  window costs time in proportion to the rows already there, and removing the top
  rows restyles every other one (about 0.45 s at 4000 rows, however few go;
  measured on a mock with the theme, 2026-10-19). So the work of one poll is
  bounded: a batch never adds more than the cap, older events being trimmed
  right away; a batch over half the cap -- the backlog of a poll that fell
  behind -- is built with the list out of the window, where every row costs the
  same (4000 rows: 2.9 s, against 4.2 s in place); and the list grows up to a
  quarter past the cap before it is trimmed back to it in one go. A batch
  appended whole before trimming hangs the GUI thread in GTK's style recursion:
  seen 2026-10-19 on a hung session, whose backtrace (taken through the SIGSEGV
  handler's gdb) showed the recursion about 12500 rows deep under
  `_timeline_append()`.
- **Grouped** — the same rows bucketed and **collapsible** by group, keyed by a
  selectable field (*domain / thread / op*). **Live**: new events are appended
  into their bucket incrementally (preserving folds), like the timeline. A full
  rebuild happens only on a group-by change or Clear.
- **Memory** — live state of the two RAM caches, queried directly (not from the
  event log). On top, one **RAM** bar over the total Ansel plans against
  (`dt_get_total_mem()`): what the caches hold in RAM, packed from the left in
  their own colours -- the pipeline cache (its lines in RAM, its allocations,
  and what lines kept in vRAM only reserve of its budget: no RAM, but the cache
  evicts and refuses on it as on the rest, so it is drawn solid and its legend
  gives it apart), the mipmap cache -- and the headroom left to the OS and
  other applications, hatched at the right end; the track between them is what
  the cache budgets still allow. Every bar of the view is drawn the same way and
  gives a thing the same colour wherever it appears: the pipeline cache's own bar
  splits its budget into lines in RAM, allocations (their own colour) and the
  size reserved by lines kept in vRAM only (the pipeline colour, hatched: counted,
  not RAM); the mipmap cache's bar wears the mipmap colour. A bar of several
  segments has a legend of blocks, each a label above its size; a bar of one is
  named by its title, which gives what its owner holds over its total. The headroom is
  the total minus the mipmap budget and the pipeline cache's configured budget
  (`dt_dev_pixelpipe_cache_get_max_memory()`, which kernel pressure does not
  lower): the startup plan gives the pipeline cache whatever the headroom
  preference and the mipmap budget leave, so that difference is the preference as
  applied (7500 MiB of 23979 MiB, matching `memory_os_headroom=7500`, measured
  2026-10-19). The colours are categorical slots checked against the window's
  grey; none reaches 3:1 on it, so a legend names every segment with its size and
  a tooltip names the one under the pointer. Then, for the **pipeline cache** and
  the **mipmap cache**, a usage bar
  (*current / max MiB*, max being the budget) and the stored items sorted by size.
  The pipeline cache's bar is its **RAM budget**, which is not all RAM: a line an
  OpenCL module keeps on the device is created without a host buffer and only
  reserves its size, and what is allocated through
  `dt_pixelpipe_cache_alloc_align_*()` counts in it without being lines. Those
  allocations are not all working buffers: besides a module's buffers for one
  run, the allocator holds what is kept for the session -- demosaic's two LMMSE
  gamma tables (`iop/demosaic.c:1193-1194`, 0.25 MiB each, freed in
  `cleanup_global()`) and the histogram's own state struct (`libs/histogram.c:3075`,
  freed in `gui_cleanup()`), seen at rest on 2026-10-19 -- so the view calls them
  *allocations*, not working buffers, and the legend splits the budget into *lines
  in RAM · allocations outside the lines · reserved by lines in vRAM only*. The
  usage is exactly the sizes of the cache's two tables summed -- its lines
  (`entries`) and the allocations (`external_entries`) -- so
  `dt_dev_pixelpipe_cache_get_entries_stats()` returns both, the allocations
  flagged `allocation`, under one hold of the cache lock, and the bar's segments
  are sums over that one snapshot. Under it, the section lists what the pipeline
  cache holds in RAM: every line with a host buffer and every allocation, each
  after a swatch in its segment's colour, held entries first, then by RAM size,
  with a monospace column: the RAM size (no `RAM:` prefix: the section names the
  memory), `stale` when that host buffer still holds the pixels of a hash the
  line no longer answers to, and an age, one phrase aligned right -- `used … ago`
  for a line, `allocated … ago` for an allocation; padded between verb and
  duration, the verb read as a column title. Both are the entry's `age`
  (`age_us` in the stats), the MRU stamp `_pixel_cache_touch()` sets: a line's
  last use; an allocation's allocation, since the only lookup that would touch
  one afterwards, `dt_dev_pixelpipe_cache_get_entry_by_data()`, has no caller
  (grep, 2026-10-19). The age is what tells a working buffer, which lives for one
  module's run, from what is kept for longer. An allocation is named after where
  it was made (`file:line`, the path from `src/` on: the stats cut the absolute
  root of `__FILE__`, which would push the line number out of the 64-byte name)
  and links nowhere: its hash is its address, which no event names. With OpenCL enabled, every device gets a section
  after the pipeline cache's bar: a **vRAM** bar titled with the device's name,
  over its total memory, splitting what Ansel holds there by its own accounting
  (`dt_opencl_get_device_memory_in_use()`) into the pipeline lines' buffers on
  that device and everything else, *allocations outside the lines* (modules'
  working buffers and what they keep there), with the
  device's headroom hatched at the right end (`dt_opencl_get_device_headroom()`:
  its `forced_headroom`, from its own config key or `memory_opencl_headroom`, what
  Ansel leaves to other applications there and plans the device without); then the lines
  holding buffers on it, each with its `hash`, refcount, hits, name and, in a
  right-aligned monospace column, its size there. A line kept on a device only
  is listed under that device alone; one with buffers in both memories, under
  both, each time with what it holds in that memory. What other
  applications hold on a device is not known to OpenCL, so its track counts it as
  free. A buffer's device and size come from what `dt_opencl_memory_statistics()`
  recorded at its allocation (`dt_opencl_get_mem_record()`), as the release path
  reads them: some Windows drivers fault in `clGetMemObjectInfo()` under vRAM
  pressure. The driver is asked only about a buffer nothing recorded. Checked on a
  single device only. Mipmap items show `image #id · mip N` and size. A pipeline
  line nobody holds (`refs=0`) -- what the LRU, the pressure valve and the flushes
  may free -- wears the theme's disabled look (`.dt_supervisor_evictable`, next to
  `*:disabled` in `ansel.css`) without being insensitive, so its link still works;
  held lines come first, each group sorted by size. The LRU also skips a line
  someone has locked, which the view does not probe: whoever writes or reads a
  line holds it too. Every item's label is ellipsized: all rows get the width of the
  widest minimum among them, so one file name too long for the window would push
  every column out of it. A third **image cache**
  section lists the cached `dt_image_t` objects (`image #id` + filename), each
  linking to its `image` event. Each item is a link:
  clicking jumps to that object's event in the timeline (a pipeline item to its
  cacheline `create`, a mipmap item to its `mipmap` object event — whose detail
  shows the `dt_image_t` properties — via the synthetic `(imgid, mip)` key).
  Every section has a title on a `GtkExpander` -- a top section
  (**RAM**, each **vRAM** device) open by default, a section under one folded --
  with its bar and
  legend under it, which stay when it is folded: it folds only its list and its
  subsections. The expander holds the title alone and no child -- the bar and
  legend are packed after it in the section's box -- because given a box of title,
  bar and legend as its label, GTK 3.24 centres the arrow on that box, against
  the bar (seen on a mock, 2026-10-19). The bar and legend line up with the title's
  text: their indent is the expander's natural width minus its label's -- the
  arrow and its spacing, 16 px under GTK 3.24's default theme -- plus the label's
  6 px margin, read from the shown widgets (a hidden widget's preferred width is
  0). The sections nest by the memory that
  holds them: **RAM** first, with the
  pipeline cache, mipmap cache and image cache sections indented under its bar;
  then each **vRAM** device, with under its bar a *Pipeline cache* section (no bar
  of its own: its size is the device bar's first segment) holding the device's
  list. The view is rebuilt at every refresh, so which sections are folded is not
  widget state but the conf, by section key: `ui/supervisor/memory/<key>/folded`
  (`ram`, `ram/pipeline`, `ram/mipmap`, `ram/image`, `vram-0`, `vram-0/pipeline`,
  …), so it outlasts the window and the session. What a folded section folds is
  not built, and folding or opening one rebuilds the view from an idle source,
  not from the expander's own signal, since the rebuild destroys that expander.
  Each list sits in a recessed `dt_ui_scroll_wrap()` area with a grip on its
  bottom edge, of a fixed height (`DT_UI_RESIZE_STATIC`) whatever it holds:
  `ui/supervisor/memory/<key>/height`, which, until the grip writes it, is set
  once to 20 rows (`MEMORY_LIST_ROWS` times the height of a probe row built like
  the real ones, in the window so the theme reaches it, then destroyed: the list
  may still be empty). Fitted to its rows (`DT_UI_RESIZE_DYNAMIC`), a list first
  seen with one row stayed that tall as it filled (2026-10-19). The areas are
  not rebuilt with the view: `_g.mem_lists` keeps each by section key, the rebuild
  unpacks them before destroying the sections (destroying a container destroys its
  children, referenced or not) and packs them again around new rows. The rows are
  swapped between two allocations, so the area keeps its height and its scroll
  position: rebuilt with the view, a list would go back to its top at every
  refresh. A refresh is also skipped while a mouse button is held over the window
  (`gdk_window_get_device_position()`): a drag on a grip would be cut short by the
  grip being unpacked under the pointer.
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
| `domain` | `history` \| `node` \| `cacheline` \| `backbuf` \| `widget` \| `thumbnail` \| `cache-wait` |
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
- **cache-wait** — one queued GUI fetch through the cache-wait manager
  (`dev_pixelpipe.c`, see `pipeline-cache.md` §4). Keyed by a synthetic key over
  the wait's monotonic `request_id`, so the queue / serve / cancel of one request
  fold onto a single object and successive requests from the same consumer stay
  distinct. Carries `request_id`, `owner` (the consumer tag, e.g.
  `color-picker-input`), `target` (the awaited module, absent for a backbuf
  request), `request_emitted` (whether this event also emitted a `CACHE_REQUEST`),
  an optional `note`, and — the crucial edge — `awaits`, resolving the cacheline
  hash the GUI is blocked on. Ops:
  - `create` — the wait was **queued** (`peek_gui()` miss on a new target);
    `request_emitted: true`.
  - `read` — a **deduped re-poll**: the consumer peeked again for a wait already in
    flight, so no new `CACHE_REQUEST` was emitted (`request_emitted: false`,
    `note: "dedup-poll"`). This is the event that the retry protocol emits on every
    expose while a wait is unsatisfied.
  - `delete` — the wait was **served** or **cancelled**. `note` distinguishes them:
    `"served"` (its exact awaited hash published), `"served (drift: node-key)"` (its
    awaited hash never published, but its target module did — the manager matched on
    the producing node key and the restart re-read the current hash; the recovery of
    the §8 drift case), or `"cancelled: <reason>"` (teardown / reset / target change).

  The `awaits` edge is what turns an invisible hang into a one-click diagnosis. If
  it resolves to a live cacheline, the buffer exists and the bug is downstream
  (the consumer did not reopen it). If it **degrades to a raw `awaits_hash`** — no
  cacheline was ever published under that hash — the pipe produced a *different*
  output hash than the GUI is waiting on, so `CACHELINE_READY` never matches the
  pending record: the "order to grab a cacheline is never processed and never
  finishes" case. A run of `read` (dedup-poll) events with no matching `delete`
  (served), whose `awaits` never resolves, is its exact fingerprint.
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

Cache-wait lifecycle, per request (queue → dedup-polls → serve/cancel):

```sh
jq -c 'select(.domain=="cache-wait") | {ts,op,request_id,owner,target,
       awaits:(.awaits.hash // .awaits_hash),note}' events.ndjson
```

Stuck cache-waits — queued or re-polled but **never served**, and whose awaited
hash was **never published** as a cacheline (issues #955 / #957). This lists every
awaited hash and subtracts those a cacheline `create` ever produced:

```sh
comm -23 \
  <(jq -r 'select(.domain=="cache-wait") | (.awaits.hash // .awaits_hash) // empty' events.ndjson | sort -u) \
  <(jq -r 'select(.domain=="cacheline" and .op=="create") | .hash' events.ndjson | sort -u)
```

Every hash it prints is a buffer the GUI blocked on that the pipe never produced —
a producer/consumer hash mismatch. Feed one back to
`jq -c 'select((.awaits.hash // .awaits_hash)==\"0x…\")'` to see which consumer,
module and pipe is starving.

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
| cache-wait | create | wait queued in `dt_dev_pixelpipe_cache_peek_gui()` (`dev_pixelpipe.c`) |
| cache-wait | read | deduped re-poll (same wait in flight, request suppressed) in `dt_dev_pixelpipe_cache_peek_gui()` |
| cache-wait | delete | served in `_dt_dev_pixelpipe_cache_wait_ready_callback()`; cancelled in `dt_dev_pixelpipe_cache_wait_cleanup()` (`dev_pixelpipe.c`) |
| backbuf | update | after `dt_dev_set_backbuf()` (`pixelpipe_hb.c`) |
| widget | read | surface rebind in `dt_dev_lock_pipe_surface()` (`views/darkroom.c`) |
| thumbnail | read/update | inside `_view_image_get_surface_internal()` (`views/view.c`), where the real mip level is known |
| mipmap | create/delete | `dt_mipmap_cache_allocate_dynamic()` / `dt_mipmap_cache_deallocate_dynamic()` (`caches/mipmap_cache.c`); create fetches the `dt_image_t` for its properties |
| image | create/delete | `dt_image_cache_allocate()` / `dt_image_cache_deallocate()` (`caches/image_cache.c`) |
| form | create | `dt_masks_create()` (`develop/masks/masks.c`); update from history snapshots |

## Limits / TODO

- The registry grows for the whole session by design (it is the memory). On very
  long sessions an LRU cap that preserves `alive: false` tombstones would bound
  it.
- Cache **read** events are high frequency (every module input acquisition). They
  are the price of "track all hits"; filter with `jq 'select(.op!="read")'` for a
  topology-level view.
- The supervisor only *links* state; it never changes behaviour.
