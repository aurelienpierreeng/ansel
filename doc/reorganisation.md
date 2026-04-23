# Ansel code base reorganization

## The initial problem

Darktable was not modular, as the [/src dependency graph](@ref src) shows: everything is wired to the GUI, IOP "modules" are all aware of everything. Modifying anything __somewhere__ typically broke something unexpected __somewhere else__.

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

#### Database

The database is handled through SQLite3, which is not thread-safe in itself, so writing the same data for the same image from concurrent threads leads to undefined behaviour. We achieve thread-safety through locking in write mode the image cache entry of the manipulated image whenever performing read/write DB operations. But that needs to be carefully implemented in each layer (development history, tagging, rating, colorlabels, metadata, etc.) and I can't guarantee I didn't forget some spots.

At some point, we will need to choose between:

1. the database API having its own per-image read/write locks that are global to the whole application, whithout relying on an external cache,
2. or all database read/write operations going through the [image cache](@ref image_cache.h) API and being effectively hidden from the rest of the application.

In any case, direct database reads/writes should be removed entirely from all the application because ensuring thread safety can only happen if they all happen from a central place, exposed to modules and core through a public API that prevents messing with the internals.

#### Image cache

The [image cache](@ref image_cache.h) holds the basic representation of an [image](@ref dt_image_t), fetched from the database : filename, path, filmroll ID, ratings, color labels, size, date/time, flags (RAW type, history inited status, etc.), orientation. It is therefore a symbolic representation of the image.

The one thing that it does not grab from database is the [image buffer descriptor](@ref dt_image_t.dsc), which is inited by the image codecs when the actual image file is loaded into the mipmap cache. This design is brittle and should be fixed (by saving `dt_image_t.dsc` to database too), because the current design means that we can never assume having a `dt_image_t` object for a particular image ID means this object is completely defined when we read it, and it will never be if the image is not present on disk at the moment (but hosted on unavailable NAS or unplugged harddrive). The function `dt_dev_ensure_image_storage()` will chain loading both the image cache and the mipmap cache for a particular image as to guarantee that.

IOP modules' `default_params()` method, that initialize the internal parameters when building fresh histories for new pictures, need complete `dt_image_t` object, including the `dt_image_t.dsc` buffer descriptor, which is used in particular to force demosaicing on RAW images or disable it on non-RAW images.

The GUI layer interfacing with `dt_image_t` object for user read/write is the [lighttable](@ref lighttable.c), through the `thumbnails.c` and `thumbtable.c` libraries.

#### Mipmap cache

The [mipmap cache](@ref mipmap_cache.h) loads images from the disk, decodes them through codecs from `src/common/imageio_*` and stores them in RAM. It is used for cached thumbnails as well as input RAW images. As mentionned above, the codecs update the `dt_image_t.dsc` buffer descriptor.

The mipmap cache is used when initing histories in `dt_dev_load_image()` and in `thumbnail.c` when fetching the base image to display in thumbnails.


#### Pipeline cache

IOP modules read their pixel input into the pipeline cache, and write their pixel output into it. Then, the GUI parts that need image surface read them directly from the pipeline cache too, as well as all the histograms and color-picker samplings.

Cachelines on the pipeline cache are indexed by the `dt_dev_pixelpipe_iop_t.global_hash` of their pipeline node `piece` object. To find the pipeline node connected to a certain module in a certain pipeline, use the function `dt_dev_pixelpipe_get_module_piece()`. From there, two different functions can fetch the data buffer connected to the cacheline associated to that piece:

- from GUI, `dt_dev_pixelpipe_cache_peek_gui()` will either return the cacheline and data buffer if available, or request a partial pipeline recompute (up to this module) to create it if missing,
- from backend, `dt_dev_pixelpipe_cache_peek()` will return the cacheline and data buffer if available, or nothing.

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

Also, when a new pipe cacheline is written, it raises the signal `DT_SIGNAL_CACHELINE_READY` with the hash of the cacheline, which means that all GUI places waiting for a particular buffer rendering can connect on this signal and immediately refresh their internal state without having to wait for a full pipeline to complete.

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
    3. validate the compatibility of previous/next module through `_seal_opencl_cache_policy()` by checking input/output between pieces `dt_iop_buffer_dsc_t`,
    4. seal the module output RAM caching policy with `_seal_opencl_cache_policy()`,
    5. process modules if the pipeline history hash is not up-to-date with the development history hash ; modules write their output directly into pipeline cache,
    6. publish the final module output as the backbuffer hash `dt_dev_pixelpipe_t.backbuf` (this is descriptive, there is no buffer copy).
5. On user parameter changes in GUI :
  1. commit to history through `dt_dev_add_history_item()` :
    1. create a new history entry for this module or updates the previous one,
    2. snapshot the internal module parameters in undo/redo stack and in the history entry,
    3. snapshot the state of pipeline forms (drawn masks) `dt_develop_t.forms`,
    4. update the global history hash and request a new pipeline <- history resynchronization (partial or complete),
    5. immediately write the changed history in database from a parallel thread to not freeze the GUI.
  2. pipeline will catch the global history hash change and figure out if it needs to run a new render,
  3. the pipeline rendering is recursive, so we call it from the last module :
    - if the hash of the last module output is found in pipeline cache, the rendering returns immediately and promote this output as `dt_dev_pixelpipe_t.backbuf`,
    - otherwise, the last module calls the previous module, which calls the previous, etc. recursively until we find a cacheline. From the found cacheline, we use it as input and process each module sitting between the one that produced that cacheline and the end of the pipeline.

Notes: XMP files are interfaced with the library database, they are never used directly.