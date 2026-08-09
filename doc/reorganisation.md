# Ansel code base reorganization

## The initial problem

Darktable was not modular, as the [/src dependency graph](@ref src) shows: everything is wired
to the GUI, IOP "modules" are all aware of everything. Modifying anything __somewhere__
typically broke something unexpected __somewhere else__.

The end goal is a backend that does not know a GUI exists, and a frontend thin enough that
replacing GTK with Qt is a sizeable job rather than a rewrite. Everything below serves that.

**Where the code stands is in [§ Module map](#module-map) and the rules that keep it there are
in [§ The rules](#the-rules). Read those two before changing anything structural.** The rest of
this document describes the architecture those rules protect.

## Definitions

We should here make distinctions between:

- __libs__ : basic shared libraries, within the scope of the project, that do basic-enough reusable operation, unaware of app-wise datatypes and data.
- __modules__ (in the programming sense) : self-enclosed code units that cover high-level functionnality and don't know anything about other modules and know is little as possible about the core.
- __core__ : set of high-level managers dispatching data between modules and tracking data consistency/lifecycle.


### IOP modules

IOP (_Image OPerations_) modules are more like plugins: it's actually how they are referred to in many early files. They define both a _pipeline node_ (aka pixel filtering code) and a GUI widget in darkroom. The early design shows initial intent of making them re-orderable in the pipelpine, and to allow third-party plugins. As such, the core was designed to be unaware of IOP internals.

As that initial project seemed to be abandonned, IOP modules became less and less enclosed from the core, which allowed some lazy mixes and confusions between what belongs to the scope of the pipeline, and what belongs to the scope of modules. The pipeline output profile can therefore be retrieved from the _colorout_ module or from the pipeline data. _color calibration_ reads the input profile from _colorin_.

IOP modules also commit their parameter history directly using [`darktable.develop`](@ref darktable_t) global data, instead of using their private link [`(dt_iop_module_t *)->dev`](@ref dt_iop_module_t), which is documented in an old comment to be the only thread-safe way of doing it.

### History

The editing history is the snapshot of parameters for each IOP. It gets saved to the database. It gets read and flattened to copy parameters to pipeline nodes.

### Develop

The [development](@ref /src/develop/develop.h) is an hybrid thing joining history with pipeline. This object will take care of reading the image cache to grab the buffer, reading the database history, initing a pipeline, and starting a new computing job.

## Ansel architecture

\htmlonly
<pre class="mermaid">
flowchart TD;
  subgraph Modules
    M((IOP)) 
    G{GUI}
    F{Defaults}
  end
  subgraph Develoment
    P((Pipeline))
    H{History}
  end
  P --update--> M
  P --read--> H
  G --read---> C
  G --write---> H
  G --read---> H
  M --fetch--> C
  M --push--> C
  H --read--> D
  H --write--> D
  D[Database]
  C[Pipe cache]
  I[Image cache]
  A[Mipmap cache]
  I --read---> D
  A --write--> I
  F --read--> I
  H --read--> F
  L{Lighttable} --write--> I
</pre>
\endhtmlonly

- Modules are :
  - part pipeline (Image OPeration -> [`process()`](@ref iop_api "process()") and [`process_cl()`](@ref iop_api "process_cl()"))
  - part GUI (`gui_init()`, `gui_update()`, `gui_changed()`, `gui_cleanup()` and [`module->params`](@ref dt_iop_module_t.params)).
- GUI and history live in the same thread (diamond nodes). The GUI part (including [`module->params`](@ref dt_iop_module_t.params)) is __not thread-safe__ and should only be handled from GUI functions.
- Pipeline and IOPs live in their own realtime thread (round nodes). IOPs are thread-safe in the sense that they are implemented as private nodes ([`piece`](@ref dt_dev_pixelpipe_iop_t)) in [`pipe->nodes`](@ref dt_dev_pixelpipe_t.nodes). But the `piece` object holds a reference to the base [`module`](@ref dt_dev_pixelpipe_iop_t.module) which should never be used for write operations from within pipelines because it lives in GUI thread,
- All caches (rectangular nodes) are thread-safe by design, through a read/write locking mechanism (more below).

We control the pipeline through the history, not through the GUI. So color-pickers work by having modules GUI wait for pipeline output, sample and write new parameters into history. Modules and pipeline are opaque to the history.

## Ansel data flow

### Caches

Caches are high-level manager that host a list of cachelines (whether as `GList` or as `GHashtable`). They all have an high-level `dt_pthread_mutex_lock_t` lock which is held for a minimal amount of time while changing the size of the cachelines list (add/remove cachelines) or looking for cachelines (in which case we don't want the list to change size in the process). Once a cacheline is found or created, it has its own read/write `dt_pthread_rw_lock_t` lock : writing is slow because it waits for all reading threads to complete, and only one thread can write at any given time, but reading is fast because it only waits for writing to complete and several threads can concurrently read. Write locks must therefore be held for a minimal amount of time to not freeze the whole application unnecessarily.

As a general rule, thread locks should be promoted to read/write locks when it is safe for several threads to concurrently read from the same buffers.

#### Database {#database}

The database is handled through SQLite3, which is not thread-safe in itself, so writing the same data for the same image from concurrent threads leads to undefined behaviour. We achieve thread-safety through locking in write mode the image cache entry of the manipulated image whenever performing read/write DB operations. But that needs to be carefully implemented in each layer (development history, tagging, rating, colorlabels, metadata, etc.) and I can't guarantee I didn't forget some spots.

At some point, we will need to choose between:

1. the database API having its own per-image read/write locks that are global to the whole application, whithout relying on an external cache,
2. or all database read/write operations going through the [image cache](@ref image_cache.h) API and being effectively hidden from the rest of the application.

In any case, direct database reads/writes should be removed entirely from all the application because ensuring thread safety can only happen if they all happen from a central place, exposed to modules and core through a public API that prevents messing with the internals.

#### Image cache

The [image cache](@ref image_cache.h) holds the basic representation of an [image](@ref dt_image_t), fetched from the database : filename, path, filmroll ID, ratings, color labels, size, date/time, flags (RAW type, history inited status, etc.), orientation. It is therefore a symbolic representation of the image.

The one thing that it does not grab from database is the [image buffer descriptor](@ref dt_image_t.dsc), which is inited by the image codecs when the actual image file is loaded into the mipmap cache. This design is brittle and should be fixed (by saving `dt_image_t.dsc` to database too), because the current design means that we can never assume having a `dt_image_t` object for a particular image ID means this object is completely defined when we read it, and it will never be if the image is not present on disk at the moment (but hosted on unavailable NAS or unplugged harddrive). The function `dt_dev_ensure_image_storage()` will chain loading both the image cache and the mipmap cache for a particular image as to guarantee that.

IOP modules' `default_params()` method, that initialize the internal parameters when building fresh histories for new pictures, need complete `dt_image_t` object, including the `dt_image_t.dsc` buffer descriptor, which is used in particular to force demosaicing on RAW images or disable it on non-RAW images.

The two-phase (provisional → resolved) lifecycle that lets the application classify an image before it is decoded — and the canonical, non-overlapping type API (`dt_image_pipe_class()` and the orthogonal `dt_image_needs_*` predicates) that replaces the overlapping `dt_image_is_*` heuristics — are documented separately in `image-type-detection.md`.

The GUI layer interfacing with `dt_image_t` object for user read/write is the [lighttable](@ref lighttable.c), through the `thumbnails.c` and `thumbtable.c` libraries.

#### Mipmap cache

The [mipmap cache](@ref mipmap_cache.h) loads images from the disk, decodes them through codecs from `src/imageio/imageio_*` and stores them in RAM. It is used for cached thumbnails as well as input RAW images. As mentionned above, the codecs update the `dt_image_t.dsc` buffer descriptor.

The mipmap cache is used when initing histories in `dt_dev_load_image()` and in `thumbnail.c` when fetching the base image to display in thumbnails.


#### Pipeline cache

IOP modules read their pixel input from the pipeline cache, and write their pixel output into it. They also publish side-band raster masks there under dedicated keys. Then, the GUI parts that need image surfaces read them directly from the pipeline cache too, as well as all the histograms and color-picker samplings.

Regular image cachelines are indexed by the `dt_dev_pixelpipe_iop_t.global_hash` of their pipeline node `piece` object. To find the pipeline node connected to a certain module in a certain pipeline, use the function `dt_dev_pixelpipe_get_module_piece()`. From there, two different functions can fetch the data buffer connected to the cacheline associated to that piece:

- from GUI, `dt_dev_pixelpipe_cache_peek_gui()` will either return the cacheline and data buffer if available, or queue a *cache-wait* and request a partial pipeline recompute (up to this module) to create it if missing,
- from backend, `dt_dev_pixelpipe_cache_peek()` will return the cacheline and data buffer if available, or nothing.

Note that a module's *input* is the *output* of the previous enabled module (`dt_dev_pixelpipe_get_prev_enabled_piece()`), so GUI code that needs a module's input must fetch the previous piece's cacheline, not the module's own. The full mechanics of the GUI fetch — the cache-wait manager, the `DT_SIGNAL_CACHELINE_READY` retry protocol, the partial-recompute request, and the pitfalls of using raw `dt_dev_pixelpipe_cache_peek()` from GUI — are documented separately in `pipeline-cache.md`.

Raster masks use the same global cache but a different identity and lifecycle. `dt_dev_pixelpipe_raster_mask_hash()` derives a dedicated key from the provider's `global_mask_hash`, a namespace tag and the provider-local mask ID. The provider publishes one immutable single-channel float buffer; consumers copy it under a read lock and apply downstream `distort_mask()` callbacks to their private working copy. The pipe retains the mask cachelines required by its current graph in `dt_dev_pixelpipe_t.raster_mask_hashes`, so preview, full and export pipelines can reuse masks even when the provider image is an exact cache hit. A rare mask-only eviction in an interactive pipe triggers one targeted `DT_DEV_PIPE_REENTRY` pass from the provider, without rebuilding the pipeline nodes or flushing upstream image states. Export enumerates the mask IDs declared by each provider and reads the same dedicated cachelines directly. See the *Raster masks are dedicated side-band cachelines* section in `pipeline-cache.md`.

This new pipeline architecture allows for fully asynchronous pipelines, where IOP modules can grab their input from the output of any arbitrary module (even non-sequential), pipelines can have parallel branches, and we can easily run partial pipelines (starting or ending at any arbitrary node).

The protocol creation of the `dt_dev_pixelpipe_iop_t.global_hash` hash is detailed in the sequence of functions that create it:

- For IOP modules :
  - `dt_iop_compute_blendop_hash()`, writing `dt_iop_module_t.blendop_hash` from internal blending and masking parameters,
  - `dt_iop_compute_module_hash()`, writing `dt_iop_module_t.hash` from internal parameters,
- For histories :
  - `dt_dev_history_item_update_from_params()` and `dt_dev_read_history_ext()`, initing `dt_dev_history_item_t.hash` with `dt_iop_module_t.hash`,
  - `dt_dev_set_history_hash()` aggregating all the history items `dt_dev_history_item_t.hash` over the whole history stack to produce one single integrity checksum for the current history. Note: that checksum is used by the darkroom [pipelines](@ref dt_dev_darkroom_pipeline()) to trigger new recomputations if needed (aka checksum changed)
- For pipelines :
  - `dt_iop_commit_params()`, writing the piece-wise (pipeline node) `dt_dev_pixelpipe_iop_t.hash` and `dt_dev_pixelpipe_iop_t.blendop_hash` aggregating both the static hashes `dt_iop_module_t.hash` and `dt_iop_module_t.blendop_hash`, with dynamic, runtime-defined parameters inited at `commit_params()` time.
  - `dt_pixelpipe_get_global_hash()`, writing the `dt_dev_pixelpipe_iop_t.global_hash` from `dt_dev_pixelpipe_iop_t.hash` and the `dt_dev_pixelpipe_t` states (mask preview mode, cache bypass modes, etc.), finally used as cacheline ID.

So the application doesn't have explicit pipeline rendering triggers anymore or invalidation flags, checksums/hashes track the internal states of all data structures across the software, and GUI updating functions as well as pipeline rendering functions keep track of the checksum of the objects they are interested in, use them to fetch the associated pixel buffers from the pipeline cache, or to trigger internal updates/renderings if they are not available on the cache.

This removes a lot of pressure on multi-threading synchronization because threads can live entirely in their own timeline without having to wait each other or start each other. We just declare data states through checksums and let each thread decide what it should do with it.

Also, when a new pipe cacheline is written, it raises the signal `DT_SIGNAL_CACHELINE_READY` with the hash of the cacheline, which means that all GUI places waiting for a particular buffer rendering can connect on this signal and immediately refresh their internal state without having to wait for a full pipeline to complete. GUI consumers should not subscribe to this signal directly: the shared cache-wait manager (`dt_dev_pixelpipe_cache_peek_gui()` + `dt_dev_pixelpipe_cache_wait_t`) already centralizes the subscription, the deduplication of pending requests, the busy-cursor feedback and the resume callbacks. See `pipeline-cache.md`.

### Data lifecycle

As stated above, reads are "fast" and writes are "slow", insofar as concurrent reads are thread-safe while writes need to wait for each other and for reads to finish. Moreover, it usually doesn't make sense that several threads would write the same data : threads are specialized for one single task. Beyond developer OCD, this allows to track reliably the lifecycle of data and expose the minimum amount of API outside of each library, while keeping data management as centralized and as private as possible.

Some places violate this principle, and shouldn't be used as precedents to justify and legitimate further extension of bad design, but should be taken as `TODO` to fix the architecture and its APIs.

Here is a complete image lifecycle, assuming it is already imported into database:

1. load a `dt_image_t` object from database to image cache through `dt_image_cache_get()`,
2. load the pixel buffer from disk to mipmap cache and update the `dt_image_t.dsc` structure through `dt_mipmap_cache_get()`,
3. load the image history from database into `dt_develop_t.history` through `dt_dev_read_history_ext()` which, internally:
  1. initialize a boilerplate history through `dt_dev_init_default_history()`, with module default parameters, auto-presets and mandatory modules for the image type,
  2. deserialize SQLite3 rows into history items through `dt_history_db_foreach_history_row()`,
  3. fetch the masks history from DB through `dt_masks_read_masks_history()` and attach it to `dt_develop_t.history` items,
  4. init/update all history-related hashes.
4. From there, we have 2 branches :
  - GUI :
    1. write ("pop") the history items into IOP module parameters through `dt_dev_pop_history_items_ext()`,
    2. update the GUI widgets values to reflect the state of internal IOP module parameters through `dt_dev_history_gui_update()`,
    4. fetch images to draw in darkroom and navigation thumbnail from pipeline cache through `dt_dev_pixelpipe_cache_peek_gui()`
  - Pipeline :
    1. create pipeline nodes for current history (handling multi-instanciation) through `dt_dev_pixelpipe_create_nodes()`,
    2. write the history items from `dt_develop_t.history` to `dt_dev_pixelpipe_iop_t` pipe pieces (nodes) through `dt_dev_pixelpipe_synch_all()` (update hashes internally),
    3. validate the compatibility of previous/next module by checking input/output between pieces `dt_iop_buffer_dsc_t`, auto-disabling incompatible modules, through `dt_dev_pixelpipe_propagate_formats()`,
    4. seal the module output RAM/OpenCL caching policy per node with `_seal_opencl_cache_policy()`,
    5. process modules if the pipeline history hash is not up-to-date with the development history hash ; modules write their image output and any requested raster-mask side-band output directly into pipeline cache,
    6. publish the final module output as the backbuffer hash `dt_dev_pixelpipe_t.backbuf` (this is descriptive, there is no buffer copy).
5. On user parameter changes in GUI :
  1. commit to history through `dt_dev_add_history_item()` :
    1. create a new history entry for this module or updates the previous one,
    2. snapshot the internal module parameters in undo/redo stack and in the history entry,
    3. snapshot the state of pipeline forms (drawn masks) `dt_develop_t.forms`,
    4. update the global history hash and request a new pipeline <- history resynchronization (partial or complete),
    5. request an asynchronous history write to database from a parallel thread (job) to not freeze the GUI — coalesced so rapid-fire commits (drag, scroll) collapse into a single write job instead of one per commit.
  2. pipeline will catch the global history hash change and figure out if it needs to run a new render,
  3. the pipeline rendering is recursive, so we call it from the last module :
    - if the hash of the last module output is found in pipeline cache, the rendering returns immediately and promote this output as `dt_dev_pixelpipe_t.backbuf`,
    - otherwise, the last module calls the previous module, which calls the previous, etc. recursively until we find a cacheline. From the found cacheline, we use it as input and process each module sitting between the one that produced that cacheline and the end of the pipeline.

Notes: XMP files are interfaced with the library database, they are never used directly.


---

# Module map {#module-map}

`src/` is sorted so that a file's directory answers two questions without opening it: **what
layer is it** and **does it hold state**. Both are enforced (see [§ The rules](#the-rules));
neither is a convention you have to remember.

Layers run low to high. A file may include from its own layer or below, never above.

| layer | directory | holds state? | what belongs there |
|---|---|---|---|
| 0 | `system/` | **no — guaranteed** | platform and machine facts: allocation, SIMD, OpenMP, thread wrappers, dynamic loading, resource limits, the arena allocator, device-scaled cairo surface arithmetic |
| 0 | `win/`, `external/` | — | Windows shims for POSIX; vendored third-party |
| 1 | `math/` | **no — guaranteed** | pure algorithms: splines, expression evaluation, topological sort |
| 1 | `common/` | yes | application services: database, caches, config, logging, image metadata, styles, tags |
| 1 | `colorprofiles/` | yes | ICC profiles: the LittleCMS2 work, the camera matrices, the transform engine, and the application-wide profile list |
| 2 | `pixel/` | no file holds its own | pixel maths: filters, wavelets, interpolation, colour transforms |
| 3 | `control/` | yes | jobs, signals, progress, the control loop |
| 4 | `widgets/` | two named registries only | the GTK widget set — reusable, and **may not include `gui/`** |
| 4 | `gui/` | yes | this application's windows, panels, theme, screen metrics |
| 5 | `develop/` | yes | history, pipeline, masks |
| 6 | `iop/`, `imageio/` | yes | image operations; codecs and export |
| 7 | `libs/`, `views/` | yes | panel modules and views |
| 9 | `src/` root | yes | `darktable.c`, the orchestrator |

Measured with `tools/statelessness_audit.py`: 739 files, 316 headers, **0 include cycles**,
217 layering violations.

## What "stateless" buys, and why it is worth enforcing

A stateless module can be reused, tested, threaded and ported without dragging the application
behind it. The point of sorting by it is that **the check becomes mechanical**: anything built
only from `system/` and `math/` is stateless too, and nobody has to re-derive that per file.

That inference is only sound while those directories stay closed, which is why
`tools/check_module_boundaries.sh` pins what `system/` may include. One exception would cost
every reader the check the arrangement exists to avoid.

## Two exceptions, both deliberate

**`widgets/` cannot be stateless.** Ten of its files hold GObject type registration —
`G_DEFINE_TYPE`'s `_private_offset`/`_parent_class`/`static_g_define_type_id`, the cached
`g_type_register_static` id, `g_signal_new` ids. Registering a type once per process and
caching the id is *what defining a GTK widget class is*. The rule instead is: **no state
outside `widget_settings.c` and `accelerators.c`**, its two named registries.

**`common/` is where state is allowed to live.** It is not a dumping ground — it is the answer
to "this needs a database connection / a config file / a cache". If something there holds no
state, it belongs in `system/`, `math/` or a module of its own.

# The rules {#the-rules}

Five gates run in CI. Each exists because the thing it checks broke something that no other
gate could see, and all five fail the build rather than warn.

| gate | rule |
|---|---|
| `check_layering.sh` | no include from a higher layer; no include cycles. A ratchet — the count may fall, never rise |
| `check_module_boundaries.sh` | `system/` includes nothing that could bring state with it; `widgets/` does not include `gui/` |
| `check_statelessness.sh` | `system/` and `math/` hold no state; `widgets/` holds none outside its two registries. Measured from the compiled objects |
| `check_unused_includes.sh` | no *newly added* include is unused. Renames and pre-existing findings are reported, not gated |
| `check_conditional_includes.sh` | no *newly added* include sits inside a conditional block |

Plus the two invariants that predate them: **no `#pragma once`** (explicit guards make cyclic
includes greppable; see `include-graph.md`), and **`darktable.h` has a re-inclusion tripwire
rather than a guard** — if you hit it, give the file the specific library it needs.

## Header discipline

**A header includes only what its own declarations need. Everything else belongs in the `.c`.**

A header that includes more becomes a supply line its consumers never asked for and cannot see.
They compile because something upstream happened to pull in what they use, and the day anyone
tidies that include away, the breakage surfaces somewhere else entirely — in a file nobody
touched. Two real instances:

- removing `gui/gtk.h` broke a dozen IOPs, because it had been the only thing pulling
  `sqlite3.h` in ahead of `common/points.h`, whose vendored SFMT `#define N` then collided with
  `sqlite3_compileoption_get(int N)`;
- deleting an unused `widgets/label.h` from `widgets/dialog.c` removed `dt_free`, which had been
  arriving through `label.h` → `system/mem_alloc.h` in a file that named neither.

Implementations do not belong in headers either: five `static inline` helpers in
`widgets/label.h` forced four extra includes on every one of its ~30 consumers. The one
legitimate exception is a header whose published interface *is* inline code (`widgets/draw.h`),
which necessarily includes what that code calls. Keep those rare and honest — they are a
performance trade, not a convenience.

## Inverting a dependency

When a lower layer needs something only a higher layer knows, do not include upward. Declare a
handler and let the higher layer register it:

```c
/* in the lower layer's header */
typedef void (*dt_widget_message_handler_t)(const char *message);
void dt_widget_set_message_handler(dt_widget_message_handler_t handler);
void dt_widget_message(const char *message);   /* no-op until registered */
```

Unregistered must be a defined no-op, so headless runs and early startup work without a single
"is there a GUI?" test. `widgets/widget_settings.h` is the worked example — cursor shape, toast
messages, natural width, per-widget persistence and the root window all arrive this way.

For a *value* rather than a callback, push it down instead: the application resolves it once and
stores it where the consumer already lives. Screen DPI works like that — `widget_settings` owns
it because every widget reads it on every draw, and `gui/screen_metrics.c` forwards under the
`dt_screen_*` names for the few readers below layer 4.

# Working on this codebase {#working-on-it}

## Diagnostics

| tool | answers |
|---|---|
| `include_graph.py --summary` | cycles, layering violations, closure sizes |
| `statelessness_audit.py --dir src/x` | which files hold or reach state, and the chain that gets there |
| `header_consumers.py <header>` | what each includer actually takes from a header — its own symbols vs. what it merely forwards |
| `header_includes_audit.py <header>` | includes a header does not need, and types it reaches transitively |
| `fix_missing_includes.py` | reads a compiler log, adds the header declaring each missing symbol |
| `check_windows_syntax.sh` | syntax-checks changed files under the Windows preprocessor via MinGW, without a Windows build |

## Verifying a structural change

A green build on one configuration proves very little here.

- **Build four configurations**: Release, `nofeatures` (mirrors CI's reduced dependency set),
  Debug, and a **clang** Debug. Compilers disagree in ways that matter — the statelessness gate
  once passed three GCC builds and failed the LLVM job, because clang names function-local
  statics `<function>.<variable>` where GCC uses `<variable>.<n>`.
- **Check the platforms you cannot build.** Anything selected by `_WIN32`, `__APPLE__` or
  `GDK_WINDOWING_*` is invisible on a Linux desktop. `check_windows_syntax.sh` covers the
  preprocessor-branch case for about a second per file.
- **Run it.** `tools/check_it_runs.sh` exports one small PNG, exactly as CI's "Check if it runs"
  step does. Every other check here is static and none of them can see a double free or a
  use-after-free. A change that passed four build configurations and every gate has already
  aborted all eight CI runners with heap corruption.
- **Compare symbols, not line counts.** For any change that moves code, diff the set of
  functions `ctags` finds before and after. Line counts and build status both miss silent
  deletion — a view truncated to its include block still compiles, it just stops being a view.

## Two failure modes worth naming

**Silent supply.** Most breakage in this series was not the code that changed; it was code that
had been compiling by accident and stopped. Expect a structural change to surface unrelated
files, and read that as the point of the exercise rather than as collateral damage.

**Gates that tax the wrong thing.** A gate scoped to "every file this change touches" reports a
file's entire inherited backlog when a refactor rewrites one include line in 180 files, and a
gate that cries wolf gets switched off. Scope to what the change *adds*; report the rest as
notes. The same applies to renames — a moved header is an added line, and demanding the file
justify an include it has always carried is how mechanical work starts dragging cleanup along.

# What remains {#what-remains}

Roughly in dependency order. Layering violations (217) fall as these land.

**Extract `database`, `caches`, `metadata` from `common/`.** `common/` is 63 translation units
and remains the largest undifferentiated module. Database access in particular should be behind
one API with its own per-image locking, so thread-safety stops depending on every caller
remembering to lock the image cache first (see [§ Database](#database)).

**Move the GUI half of `imageio/`.** `imageio/format/`, `imageio/storage/` and
`imageio_module.c` are export dialogs and settings widgets, not codecs. They belong in `gui/`.

**Split `develop/imageop_math.h`.** The curve-fitting maths is pure and belongs in `math/`.

**Remove `dev->proxy`.** It is an anti-pattern that breaks modularity by design — a grab-bag of
function pointers letting anything reach anything.

**Make `pixel/` stateless.** No file there holds state of its own; all 13 that reach state do so
through `common/opencl.c` (the device registry) or `develop/pixelpipe_cache.c`. Inverting those
two dependencies would make the tree's whole pixel-maths layer stateless.

**Close `system/` fully.** It no longer includes anything outside itself, but the gate is what
guarantees that rather than the structure. Nothing to do unless it regresses.

---

# Architectural notes and open problems

These concern the *runtime* architecture rather than the module layout above: known contention,
and directions the pipeline could take. They are design intent and measured problems, not
descriptions of finished work — anything marked **open** is unclaimed.

### History item refcounting and the `history_mutex` contention

`dt_dev_history_item_t` is now refcounted the same way `dt_masks_form_t` already was (see
`masks_history.c`): `dt_dev_history_item_create()` is the only constructor (initializes
`refcount` to 1), `dt_dev_history_item_ref()` takes a reference, `dt_dev_free_history_item()` is
unref-and-free-at-zero, and `dt_dev_history_cow_touch()` clones a shared item before mutating it
in place (mirrors `dt_masks_cow_touch()`). This exists so that any future code that needs to hold
a snapshot of `dev->history` across a slow operation (pipe resync, DB write job) can do so
without a deep copy and without racing a GUI-thread mutation of the same item.

The refcounting was introduced to attack a real, measured problem: `dt_dev_pixelpipe_change()`
(the worker-thread resync function, called from `dt_dev_darkroom_pipeline()`) holds
`dev->history_mutex` as a *reader* for the entire O(nodes × history) resync of a pipe, which
routinely takes tens of ms and, under mask-heavy history stacks combined with active editing, has
been measured over 200ms. Because `dt_dev_add_history_item_ext()` (the GUI-thread commit path)
needs the *writer* lock on the same `history_mutex`, and the writer-preferring rwlock policy
blocks new readers once a writer is queued, every GUI edit (a scroll on exposure, a mask drag)
can stall for the full duration of whatever resync the worker thread happens to be mid-flight on.
This is directly observable with the named-rwlock diagnostic added to `dt_pthread_rwlock_t`
(`common/dtpthread.h`: `dt_pthread_rwlock_set_name()` + wait-time logging, opt-in per lock —
`dev->history_mutex` is named in `dt_dev_init()`) combined with `-d history`.

**Status: open.** The refcounting/COW infrastructure above is a prerequisite for resyncing a
pipe against a snapshot of `dev->history` instead of holding `history_mutex` for the whole
resync, which is the direction to pursue to remove this stall.

### Where the pipeline architecture could go

The following are **proposals**, enabled by the pipeline-cache design described above but not
implemented. They are recorded because the cache rewrite was done partly to make them possible.

The history is so far incrusted into the `dt_develop_t` object, with its own `history_end` and `history_mutex` lock. It mixes both module parameters history, and masks/forms history in a weird fashion. The history use to be scattered all over the software, with parts handled in SQL and parts handled in C. Now that everything is handled in C and the history handling methods have been contained in `history.c` and `dev_history.c`, it might be a good idea to move it entirely out of the `dt_develop_t` object to have it managed globally, like the pipeline cache, but behind an API that allows to track precisely the lifecycle of data and concurrent accesses. Writing history to database and to XMP is currently handled in separate, short-lived threads, this completely enclosed architecture would allow to have all history tasks (including reading) handled in parallel, while ensuring thread safety. 

The current architecture has the pipeline rendering triggered implicitely from history changes. This means we need to resync the history for all relevant modules all the time, and relevant modules, when working with masks, are all modules. This puts a solid 30-50 ms delay before starting any pipeline on history changes, or figuring out that no recompute is needed. But the current architecture of the pipeline cache allows us to start and stop pipelines absolutely how we want, because we can probe and fetch the cachelines from any thread. It means that, when changing GUI parameters in modules, modules could self-start their `process()`/`process_cl()` directly in pipeline, calling `dt_iop_commit_params()` on themselves, and fetching their own input straight from cache, therefore bypassing history (history would still have to be written, but the pipeline render wouldn't have to wait for it in its own thread). `drawlayer.c` would greatly benefit from such low latency.

A direct module -> pipeline trigger would also avoid the messy business of having to handle mask preview GUI states within the pipeline recursion, which means that we have to hack the `piece->global_hash` to account for GUI states and properly recompute pipelines when switching on/off mask previews. That introduces lots of edge-cases to handle through heuristics and bypasses. Raster-mask providers already publish dedicated side-band cachelines, independently from their image outputs; the remaining architectural step would be a specialized pipeline that only runs the `distort_mask()` methods instead of performing those transforms while the consuming module blends. Note that color-pickers have also been completely removed from the rendering pixel pipeline and now deal directly with cachelines, from the GUI thread, which avoids having to recompute a pipe just to refresh their values.

The new architecture doesn't force modules to take their input from the previous module output either, now they can take input from any module output in the pipeline as long as we know the global hash of the module, to fetch its output buffer on the cache. So we could have a new module, _masking & merging_ that could take input from several modules and blend them over each other with alpha, meaning we could have parallel branches within pipelines and not stay limited by single sequences of modules. Along with the new nodal graph viewer, that would allow a full nodal workflow.