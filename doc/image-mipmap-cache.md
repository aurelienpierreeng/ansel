<!-- Provenance: every finding carries the commit it was established against. -->

# The image and mipmap caches

> **Verified against `42eca0e8fe` on 2026-09-29.**
>
> When a thumbnail is regenerated, what releasing an image cache entry actually returns, and the race a duplicate runs against its own thumbnail.
>
> Each finding below is dated with the commit that established it. A finding is only as
> good as its hash: before acting on one older than the code you are changing, re-measure
> it — and re-date it here when you do. Where an earlier version of a claim was **wrong**,
> that is recorded rather than quietly corrected: how a claim was wrong is usually the more
> useful thing to know.

## Mipmap invalidation is explicit, not hash-driven

*Found `22f623c0be`, 2026-06-25.*

The mipmap cache get path (`_generate_blocking` in `caches/mipmap_cache.c`) does NOT compare
`history_hash` vs `mipmap_hash` to detect staleness. Regeneration only happens after an explicit
`dt_mipmap_cache_remove(imgid, TRUE)`. *(This file gave that call a leading `cache` argument
until 2026-09-29; the cache is file-static and the documented call did not compile —
`caches/mipmap_cache.h:203`.)*

Every operation that mutates an image's history/development MUST explicitly:
1. `dt_mipmap_cache_remove`
2. Refresh the cached image metadata so `history_items` is correct (`_write_mipmap_to_disk` uses
   `img->history_items > 0` as the "altered" flag for the embedded-JPEG-vs-raw decision)
3. `dt_thumbtable_refresh_thumbnail`

The darkroom/paste path does this via `dt_dev_history_notify_change` (`dev_history.c`). Paths that
write history straight to DB (XMP load, `dt_image_set_flip`) bypass it and need the fix pattern:
`dt_image_cache_get_reload`, then remove mipmap + refresh thumbnail.

Do NOT refresh the filmstrip from darkroom write paths — it competes with the realtime main
preview pipeline. Lighttable ops may refresh both.

**`dt_mipmap_cache_remove()` drops the THUMBNAILS, never the decoded raw.** Its loop stops at
`DT_MIPMAP_F`, and `dt_mipmap_cache_remove_at_size()` refuses `DT_MIPMAP_F`/`DT_MIPMAP_FULL`
outright, so those two — the unprocessed input, RAM-only, every disk write being gated on
`mip < DT_MIPMAP_F` — are reachable only through `dt_mipmap_cache_remove_all_sizes()`. That is
the right default for the list above: a development change does not invalidate the decoded raw,
and dropping it on every history commit would re-read and re-demosaic the file per slider tick.

An image LEAVING the library is the other case, and the only caller of the all-sizes form.
Its input buffer otherwise outlives the row, with nothing but memory pressure to reclaim it,
and `basebuffer` — which slices that buffer — is handed the stale entry when the image comes
back on Ctrl+Z. It reports `invalid cache entry size 0 for module basebuffer`, the mipmap get
path answers with an 8x8 husk, and no later render replaces it. **Only a developed image shows
this**: an unaltered one is drawn from the embedded JPEG and never asks for the input at all,
which is why the symptom reads as "one broken thumbnail" rather than as a cache bug.

## Releasing an image cache entry returns the LOCK, not the image

*Found `2a0a1efe91`, 2026-09-06.*

`dt_image_cache_read_release()` and `dt_image_cache_write_release()` (`caches/image_cache.c`)
guard on a NULL pointer and nothing else. They used to read `if(IS_NULL_PTR(img) || img->id <= 0)
return;` — which is `dt_image_invalid()` spelled out — and that skipped the release for precisely
the entries most likely to have one outstanding.

An entry whose row has gone stays in the cache with `id == UNKNOWN_IMAGE` (-1): the allocator
runs `dt_image_repository_load()`, that fails with `no more rows available`, and `dt_image_init()`
has already left the id there. Anything holding such an entry then called release, got nothing,
and left it locked forever. `dt_cache_get()` spins on `trywrlock` with a `g_usleep(5)` retry, and
`try*` locks report busy even on same-thread reentry (see the rwlock section below), so the next
writer hangs the GUI thread with no error and no stack anywhere else — every other thread sits
idle in `dt_pthread_cond_wait`. Measured: a whole film roll removed and undone froze in
`dt_image_history_changed()` waiting on an entry nobody held.

`dt_image_cache_testget()` is the other half and now carries the validity check its two siblings
(`dt_image_cache_get()`, `dt_image_cache_get_reload()`) always had: handing out a LOCKED invalid
image is what creates the leak, because the caller has no way to release what it was told is not
an image.

This is reachable whenever a row disappears while the GUI still refers to it — removal, and the
lighttable refreshing a thumbnail right after. Grouped images make it far likelier, since
`_add_thumbnail_group_borders()` re-reads every member.

## Duplicating an image races its own thumbnail generation against the history copy

*Found `48f8e58e0a`, 2026-07-31.*

Lighttable "Duplicate" (`dt_control_duplicate_images_job_run`, `control_jobs.c`) creates the new
DB row via `dt_image_duplicate()`, then copies the source's history onto it via
`dt_history_copy_and_paste_on_image(..., DT_HISTORY_MERGE_REPLACE, ...)`. `dt_image_duplicate()`
(`common/image.c`) used to call `dt_collection_update_query(..., DT_COLLECTION_CHANGE_RELOAD, ...)`
unconditionally, right after inserting the row — i.e. *before* the caller had copied any history
onto it. That reload makes the new image visible to the lighttable grid, which can create its
thumbnail widget and request a render immediately, against the row's momentary real state: zero
history.

Confirmed with `-d cache -d history -d lighttable`: for a freshly duplicated image, the first
`[mipmap_cache] compute mip size 0 ... from original file` log line landed ~650ms *before* the
matching `[dt_dev_write_history_ext] writing history for image N` line. The mipmap cache is not
hash-driven (previous section), so once that first, historyless render is cached, only an
explicit `dt_mipmap_cache_remove` + refresh recovers — and even when that recovery path runs
correctly and a second, correct render finishes and gets cached, nothing guarantees a timely
repaint of it (the thumbnail widget can be left showing the first render under a permanent "busy"
overlay for several seconds, until an unrelated GUI event forces a redraw). Patching the
recovery/notification side (adding a missing GUI-thread redraw request on one early-return path
in `gui/dtgtk/thumbnail.c`'s `_get_image_buffer()`) did not fix this reliably and was reverted — the
actual fix is to not let the race start in the first place.

Fixed by `dt_image_duplicate_no_reload()` (`common/image.c`): same as `dt_image_duplicate()` but
skips the immediate collection reload. Both call sites that duplicate-then-copy-history
(`dt_control_duplicate_images_job_run` in `control_jobs.c`, `_history_style_apply`'s
duplicate-and-apply-style branch in `history_actions.c`) now use it and trigger exactly one
`dt_collection_update_query(..., DT_COLLECTION_CHANGE_RELOAD, ...)` themselves, after the
history copy/delete completes — so the very first time the duplicate becomes visible, it already
carries its final history. Any future caller of `dt_image_duplicate()` that will mutate the new
image's history afterward (a style, a batch edit, ...) should do the same rather than let the
default immediate reload race its own follow-up write.

## Why a thumbnail is a skull, and how it says so

*Found `8bef6908d9`, 2026-10-05, by reading every path that zeroes a thumbnail's size (#1531).*

A skull is painted by one function: `_paint_skulls()` (`caches/mipmap_cache.c`) replaces any
thumbnail whose descriptor came out of generation as `0x0` with the 8x8 `dead_image_8()`. Every
cause therefore goes through zeroing `dsc->width/height`, and until this change the cause was
lost there — stderr only got `could not process thumbnail!`. The causes, each now kept as a
`dt_imageio_retval_t` in `dsc->status` and handed to readers in `dt_mipmap_buffer_t.status`:

| Where | Cause | Status |
|---|---|---|
| `_init_8` | neither the original nor a local copy exists, or the image left the image cache | `FILE_NOT_FOUND` |
| `_init_8` | embedded-JPEG mode "always" (2) and no embedded/companion JPEG could be read: the pipe is forbidden | `NO_EMBEDDED_THUMBNAIL` |
| `_generate_blocking`, `DT_MIPMAP_FULL` | the loader failed; its own value (`UNSUPPORTED_CAMERA`, `FILE_CORRUPTED`, …) used to be dropped | the loader's |
| `dt_imageio_export_with_flags` | pipe init failed, or the pipe returned an error / no backbuf | `PROCESSING_FAILED` |
| `dt_imageio_export_with_flags` | the pipe returned an error after the pixelpipe cache refused it memory | `CACHE_FULL` |
| `dt_imageio_export_with_flags` | the pipe was stopped (`dt_dev_pixelpipe_process` returns the same `1` for a kill-switch stop as for a failure) | `ABORTED` |
| `dt_imageio_export_with_flags` | the pipe's output was gone from the pixelpipe cache before it could be referenced | `PROCESSING_FAILED` |
| `dt_imageio_export_with_flags` | the output buffer could not be allocated | `CACHE_FULL` |
| `dt_imageio_export_with_flags` | `format->write_image` failed | `IOERROR` |

Not causes: an **external** stop (`shutdown_ext`, a thumbnail scrolled out of view) restores the
descriptor and keeps the entry flagged for generation, so it never leaves a skull; the disk cache
never holds one (only `> 8x8` entries are written, and an unreadable `.jpg` is deleted and
regenerated); `_init_8`'s early return for `width < 16` does not zero the size.

### A pipe that failed for lack of memory says so

*Established on `8bef6908d9` with this change, 2026-10-05; the cache's side measured by
`tests/unittests/test_pipe_cache_alloc_refusals.c`, the export's side not run.*

A run the pixelpipe cache refuses a buffer fails like any other: the module or
`dt_dev_pixelpipe_cache_get_writable()` gets NULL and the pipe returns `1`. Telling the two apart
afterwards needs the cache, which is the only one that knows. It counts its refusals per thread
in `dt_pixelpipe_cache_get_alloc_refusals()`, at the two places where it already tells the user
"the pipeline cache is full":

- `_free_space_to_alloc()` returning an error: the budget is spent and every line is in use, so
  nothing can be evicted;
- `_log_arena_allocation_failure()`: the arena has no free run long enough, or
  `_system_memory_pressure_valve()` refused because the system is out of RAM.

The export reads the count before `dt_dev_pixelpipe_process()` and again on failure. The pipe
runs on its caller's thread, so a change is this run's refusal, not another pipe's. The test
measures that: a granted buffer is not counted, a refusal is counted exactly once on each of the
two paths, and a refusal on another thread leaves this one's count unchanged.

Still `PROCESSING_FAILED`: memory a module allocates outside the cache (plain `dt_alloc_align`),
allocations on OpenMP worker threads, and OpenCL allocations. Device memory is not the cache's,
and a device failure falls back to the CPU anyway. One known mislabel goes the other way: a refusal
the run recovered from, followed by an unrelated failure in the same run, reads as `CACHE_FULL`.

A skull stays in RAM until the entry is evicted or invalidated, **whatever the cause** — a
transient one (`CACHE_FULL`) included. The label makes that visible; it does not change it.

`_view_image_get_surface_internal()` (`views/view.c`) paints the label: every consumer
(lighttable, filmstrip, preview window, map, print, slideshow) goes through it. It runs in a
worker thread, so it builds its own Pango font description rather than borrowing bauhaus's, which
`dt_bauhaus_load_theme()` frees from the GUI thread.

### The label wraps onto half the skull at most

*Measured on `8bef6908d9` with this change, 2026-10-05, outside the application: a standalone
pangocairo program running the label block of `view.c` line for line over `dead_image_8()`, scaled
with the nearest filter as `view.c` does, at square sizes from 32 to 160 px (FIT scales the 8x8
skull uniformly, so the surface is always square). Pango 1.58.2, cairo 1.18.6, the Win32 font map;
"sans bold" at 12 px resolved to DejaVu Sans Bold, one line 15 px tall. The final code ran on ten
labels at eight sizes from 32 to 128 px: four English ones; "No embedded thumbnail" in French,
Japanese, Chinese, Thai and Russian (the strings are new, so these were not taken from `po/`); and
"Speicherzugriffsfehler", for a word wider than the thumbnail. Not yet seen in the application.*

The label wraps onto as many lines as half the thumbnail holds (`pango_layout_set_height(layout,
img_height / 2 * PANGO_SCALE)`), and Pango ellipsizes the last. Half keeps the skull's eyes clear:
they end at 3/8 of its height. At this font size, half holds a single line below 56 px, which is
the ellipsized line of before. Over the 80 label × size pairs, the band reached the eyes once, by
half a pixel: Chinese at 60 px, whose two lines take 34 px.

Half is approximate, by a pixel or two. 27 px holds one line, 28 px already two (30 px), and three
start at 45 px. 28 is twice the font's metric height, 13.97 px, against 15 px for a laid-out line.
A line count instead (`pango_layout_set_height(layout, -n)`, with `n` from a first one-line
measure) was tried and dropped. Over 35 label × size pairs it gave the same lines as the pixel
height in all but one: Thai at 60 px, one line instead of two, the eyes clear either way. Nor is it
more exact: the one line it measures need not be as tall as the lines that wrap. Chinese at 60 px
measured 15 px and wrapped onto 34.

`PANGO_WRAP_WORD_CHAR`, not `PANGO_WRAP_WORD`. With `WORD`, a word wider than the thumbnail stays
on one line, overflows both edges and is not ellipsized: "Speicherzugriffsfehler" at 72 px was
drawn from −41 to 113 px, "Unsupported" from −9 to 81. `WORD_CHAR` breaks such a word inside, with
a hyphen, and still breaks between words where it can.
