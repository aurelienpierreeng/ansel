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

## Where the on-disk thumbnail cache lives, and the one way to ask

> **Established 2026-09-30 against `aac2ee1321`.**

`dt_loc_cachedir()` is the answer, always — or `dt_loc_default_user_cache_dir()`
(`src/common/file_location.c`) for the two callers that run before `dt_loc_init()` has set the
global. **Never `g_get_user_cache_dir()` directly**, for three independent reasons:

- **It is the wrong directory on Windows.** GLib returns `FOLDERID_InternetCache` there —
  `%LOCALAPPDATA%\Microsoft\Windows\INetCache`, the shell folder still labelled "Temporary
  Internet Files" — a shell-managed container for the browser cache, not a general-purpose
  per-user cache. Storage Sense empties it whenever it runs, of every file not *written* in the
  last week or so however recently it was read (measured below), so a thumbnail cache written
  there — written once, read for months — is deleted behind the user's back, which reads as "the
  disk cache never works on Windows". It also carries the Hidden and System attributes, so
  Explorer does not show it even with hidden files shown, and a thumbnail cache or a log written
  there is invisible to a user who goes looking (#1473). The default is now
  `%LOCALAPPDATA%\cache\ansel` on Windows and `~/.cache/ansel` elsewhere, with an absolute
  `XDG_CACHE_HOME` winning on every platform. On Windows it sits
  *beside* the config folder `%LOCALAPPDATA%\ansel` (`g_get_user_config_dir()` is `%LOCALAPPDATA%`
  there), not inside it, as `~/.cache/ansel` sits beside `~/.config/ansel`: a cache nested in the
  config goes with every backup or deletion of `anselrc` and `library.db`. The branch first had
  `ansel\cache`; it was changed on 2026-10-07, before it shipped, so only test builds wrote there.
  `%LOCALAPPDATA%` is read from the environment rather than through `g_get_user_data_dir()`, which
  resolves to the same folder but takes `XDG_DATA_HOME` first — a variable that says where data
  goes and must not steer a cache.

  *Measured 2026-10-07 against `ea59057346` plus #1493*, on one Windows 11 machine with Storage
  Sense on (`StoragePolicy` `01=1`, temporary files `04=1`) at its default cadence (`2048=0`,
  "when disk space is low"), by running it by hand — "Run Storage Sense now", recycle bin and
  Downloads set to "Never" — over the 118 files an older build had left in `INetCache\ansel`
  (backed up first):

  - every last-access time set back to 2026-07-01: it deleted the 109 files last written
    2026-04-30 .. 2026-09-28 and kept the 9 written the day before (the log, two OpenCL kernels,
    six thumbnails);
  - the 109 restored with their write and creation times unchanged and their last-access time
    set to the minute before the run: it deleted the same 109 again and kept the same 9.

  So the criterion is the last write, somewhere between one and nine days old, and a recent read
  does not protect a file. The attributes were read on the same machine (`INetCache`: Hidden,
  System), with Explorer showing hidden files but not protected system ones (`Hidden=1`,
  `ShowSuperHidden=0`). Disk Cleanup was not tried.

  *How this claim went wrong, then right.* The first version of this section said Storage Sense
  clears the folder "by default", without measuring it. The review of #1493 then called that
  unestablished, because those same 118 files had sat there for five months, and an earlier
  commit of the PR took it out. The runs above put it back. The five months were the cadence, not
  Storage Sense sparing the folder: at "when disk space is low" it had most likely never run on
  that machine — which also means "by default" overstates how often it happens, not what happens.
- **It ignores `--cachedir`.** That is how `libs/textnotes.c` came to build a download's path under
  `g_get_user_cache_dir()/ansel/downloads` while creating the directory under
  `dt_loc_cachedir()/downloads`: with `--cachedir` given, it made one directory and wrote into
  another, which did not exist.
- **It takes `XDG_CACHE_HOME` literally, relative included, and keeps its first answer.**
  *Established 2026-10-07 against `ea59057346` plus #1493.* `g_build_user_cache_dir()`
  (`glib/gutils.c`) copies the variable whenever it is non-empty, on every platform, with no
  `g_path_is_absolute()` test; `g_get_user_cache_dir()` memoises that for the life of the process. Read from the GLib
  source (main) and measured on Windows with a probe (GLib 2.90, MSYS2 UCRT64): with
  `XDG_CACHE_HOME=relative-cache` it answers `relative-cache`, and a second call after changing the
  variable answers the same. So the non-Windows default is built from `g_get_home_dir()` +
  `.cache`, which is what GLib answers for an unset or empty variable — the only cases left once an
  absolute one has returned. *The first version of this branch had it wrong:* it kept
  `g_get_user_cache_dir()/ansel` for Linux and macOS behind an "absolute only" guard, and a
  relative value walked around the guard and came back through that call as `relative-cache/ansel`.
  Its test could not see it: the empty-variable case ran first and froze GLib on `~/.cache`.

`tests/unittests/test_loc_cache_dir.c` pins the XDG precedence and that the default is not a
shell-managed temporary folder. Its relative-variable case runs first, before anything in the
process can have asked GLib for its cache directory, so a return to `g_get_user_cache_dir()` would
fail it on Linux. The thumbnail cache path itself is built on top of this by
`dt_mipmap_cache_get_filename()`, which hashes the library's absolute path into the directory
name so two libraries do not share thumbnails.

## What the eviction write persists, and how it handles its files

> **Established 2026-09-30 against `aac2ee1321`.**

`dt_mipmap_cache_deallocate_dynamic()` is the only place a thumbnail reaches the disk cache — on LRU
eviction and, for everything still resident, from `dt_cache_cleanup()` at exit. Four things about it
were wrong, and each one is a way to end up with a cached file that should not exist or cannot be
removed.

**The write predicate is about the entry's state, not its size.** It used to be `dsc->width > 8 &&
dsc->height > 8` and nothing else, which caught the skull and missed the other two cases:

- **An entry that was never generated** carries `DT_MIPMAP_BUFFER_DSC_FLAG_GENERATE` over an
  uninitialised `dt_alloc_align()` block, at the mip's full dimensions — so the size test passed and
  the JPEG encoder was handed whatever the allocator returned. `_generate_blocking()` deliberately
  leaves such an entry behind whenever the `shutdown` atomic is raised mid-render. Only the async
  surface fetcher passes one (`dt_view_image_get_surface_async()`: the preview window, slideshow,
  map, print, studio capture), and it raises it on a resize, an image change, or when its widget
  goes; lighttable and filmstrip thumbnails go through `dt_view_image_get_surface()`, which passes
  none, and never abort. The next get of the same key renders over the entry again, so an aborted
  entry survives only when nothing asks that size of that image again — a preview window enlarged
  and then closed before its render ended. Eviction and exit wrote it out as a thumbnail of
  uninitialised heap at the box's full dimensions, which the next session then loaded as valid. The
  first version of this section called this the common case and said scrolling a filmstrip produced
  them; the filmstrip never aborts a render (measured below).
- **An invalidated entry** (`..._FLAG_INVALIDATE`) unlinked its file and then, with no `else` between
  the two branches, immediately wrote the very buffer that had just been declared stale. The flag
  is set on a thumbnail only by `dt_mipmap_cache_remove_at_size()` with `flush_disk` TRUE, which
  unlinks a *second* time once `dt_cache_remove()` has returned, so no stale file outlived the call.
  What it cost was a JPEG encode and write per removed size, and a window: `dt_cache_remove()`
  releases the cache lock before that second unlink, and a get of the same key in between runs the
  allocate callback, which reads the disk cache — the stale file — under that lock. *Read from
  `src/caches/cache.c` against `2fadf8a366`, not measured.* The first version of this section said
  that with `flush_disk` FALSE the stale thumbnail was persisted; with `flush_disk` FALSE the flag
  is never set, and the write is the ordinary eviction write.

**An entry still flagged `GENERATE` is absent to a `DT_MIPMAP_TESTLOCK` get as well.** Keeping it
off the disk is not enough on its own: unless the embedded JPEG is used (the default for an unedited
raw), `_init_8()` builds a size from the next larger one a `TESTLOCK` get finds, and that get handed
back an aborted entry's buffer at the box's full dimensions. The smaller size was downsampled from
nothing and became an ordinary thumbnail, shown and written to disk. A `TESTLOCK` get now releases
such an entry and answers as if it were not there, which is also what the other caller, the
darkroom's "is the cache ready for this size" test, means by it.

*Measured 2026-10-23 against `2fadf8a366` plus #1495*, with a program linked against `libansel` that
requests size 2 of one NEF with the shutdown atomic already raised, then size 1
(`lighttable/embedded_jpg=0`, `--disable-opencl`, fresh `--configdir`, `--cachedir` and `--library`):

- **`2fadf8a366`:** a `TESTLOCK` get of size 2 returns 1440×900 of zeros; size 1 is downsampled from
  it ("generate mip size 1 for image 1 from mip size 2"), 720×450 and all zeros; both are written to
  the disk cache, black.
- **With the eviction write fixed only:** size 2 is withheld ("never generated"), and the black size
  1 is still written.
- **With the `TESTLOCK` change too:** the get returns no buffer, size 1 is rendered from scratch
  (300×450, mean byte value 127) and that is what is written.

The buffer read as zeros here, a fresh allocation; nothing guarantees that in a long session. With
`ansel-generate-cache` and `lighttable/embedded_jpg=0` (one size rendered from scratch, two
downsampled from a larger one), the three files are bit-identical with and without the change.

**`g_unlink()` must come after `fclose()`, in both halves.** On Windows `DeleteFileW` refuses a file
that any handle still holds open without `FILE_SHARE_DELETE`, which the CRT's `fopen` never sets. The
read path unlinked a file it had judged corrupt while its own read handle was open, and the write
path's error branch unlinked while holding a handle of its own — so on Windows a bad disk-cache entry
could never be cleared: it was re-read, re-rejected and re-logged on every start, permanently. This
is invisible on POSIX, where unlinking an open file is ordinary.

**The output file was opened twice.** `dt_imageio_jpeg_write()` opens, writes and closes the path
itself; the caller also opened it `"wb"` purely to test writability, which truncated it before the
free-space check ran and held a second handle across the whole write. That outer open is gone — the
function's return value is the only answer needed.

**A free-space probe that cannot answer must not veto the write.** It ran `statvfs()` on the output
file (so the file had to exist first, which is why the redundant open was there) and treated an
unanswerable probe as "no room". `src/win/statvfs.c` built a drive root out of `path[0]`, which is
right for `C:\...` and wrong for a UNC share, a redirected `%LOCALAPPDATA%` or a drive mounted as a
folder — and there *every* thumbnail write failed silently. The probe now runs on the directory
before anything is created, returns TRUE when it cannot answer, and the Windows shim uses
`GetDiskFreeSpaceExW`, which takes any directory rather than a drive root. Its documentation asks a
UNC path to end in a backslash, and the cache directory does not, so on a UNC share the probe may
still fail — now without consequence, since the write goes ahead. Not tried on Windows. That shim
reports its counts in bytes with `f_frsize == 1`: every consumer computes `f_frsize * f_bavail`,
which is satisfied exactly, but the true cluster geometry is gone — do not add a consumer that wants
it without a second query.

The skip is logged under `-d cache` ("is not written to the disk cache: never generated | a skull"),
because how often an aborted render leaves an entry behind is a question only a real session
answers.

*Measured 2026-10-23 in the GUI against `2fadf8a366` plus #1495*: twelve hardlinks of one NEF,
`lighttable/embedded_jpg=0`, fresh `--configdir`, `--cachedir` and `--library`, `-d cache`, aborts
counted by their `DT_IMAGEIO_ABORTED` ("error 11") line. Scrolling the lighttable and the filmstrip
for 27 minutes at five thumbnails per row aborted nothing. The preview window opens at 350×350, a
size the lighttable usually holds already, so it shows at once and renders nothing; enlarging it asks for
size 2. Opening it on an image the window had not shown yet, enlarging it and closing it within a
second, on three images, at three thumbnails per row:

- **With master's `src/caches/mipmap_cache.c`:** 14 aborts; size 2 of images 2, 6 and 8 never
  completed, and all three were written at exit as 1440×900 JPEGs of zeros, where a real render of
  that image is 600×900; size 1 of images 6 and 8, which the window asked for first and which was
  aborted too, was written as 720×450 of zeros, where a real one is 300×450.
- **With #1495:** 11 aborts; the four entries they left behind (size 2 of images 2, 4 and 6, size 1
  of image 6) are logged "never generated" and not written, and the 13 files written are all real
  images.

Neither session built a size from an aborted larger one, so the `TESTLOCK` half is measured only by
the program above.

*Measured 2026-10-23 against `2fadf8a366` plus #1495*, with `ansel-generate-cache --min-mip 0
--max-mip 2` on one NEF, fresh `--configdir`, `--cachedir` and `--library`, `--disable-opencl`: the
three thumbnails are written and the skip line appears 0 times; a second run leaves them
byte-identical; a thumbnail truncated to 0 bytes is reported ("Reason: empty file"), unlinked and
rewritten identically; and the three files are bit-identical (sha256) to the ones a build of
`src/caches/mipmap_cache.c` from `2fadf8a366` writes for the same image.

## A thumbnail read from the disk cache is not written back

> **Established 2026-10-24 against `2fadf8a366`.**

`dt_mipmap_cache_deallocate_dynamic()` writes a thumbnail to the disk cache on LRU eviction and, for
everything still resident, from `dt_cache_cleanup()` at exit. The read path used to leave no trace
on the entry (`flags = 0`), so that write could not tell a thumbnail read from its file from a fresh
render, and re-encoded it at every session, at `database_cache_quality` (89 by default). The comment
above the write said "Don't write existing files as both performance and quality (lossy jpg)
suffer", and the FIXME under it that it did anyway.

The entry now carries `DT_MIPMAP_BUFFER_DSC_FLAG_ON_DISK` from the read until new pixels are written
into it, and the write is skipped while it does. Only `dt_mipmap_cache_swap_at_size()`, the
darkroom's copy, writes new pixels into an entry read from disk, and it clears the flag; generation
runs only on an entry flagged `GENERATE`, which a read never leaves, and
`dt_mipmap_cache_write_get()` has no caller. A file deleted from outside behind a resident entry so
flagged is not written again at eviction; the next session renders it.

*Measured 2026-10-23 and 2026-10-24*, with a program linked against `libansel` that asks sizes 0 to
2 of one NEF and quits, run in a loop on one cache (`lighttable/embedded_jpg=0`,
`--disable-opencl`, fresh `--configdir`, `--cachedir` and `--library`):

- **Before the flag** (`2fadf8a366`, 30 sessions, and a branch writing the same bytes session for
  session, 150 sessions): every session from the second on read the three files and wrote them
  back. Sizes 0 and 1 reached a fixed point: the file stopped changing after session 8 and session
  12, at 54.6 and 54.9 dB PSNR from the first file. Size 2 did not: one 8×8 block (x 352–359,
  y 736–743 of 600×900) gained one level per session, 155 to 254, and stopped only once near white,
  after about 100 sessions.
- **With the flag** (`2fadf8a366` plus this change): the three files were byte-identical over 30
  sessions, and the first one byte-identical to `2fadf8a366`'s; an entry read from disk and then
  overwritten through `dt_mipmap_cache_swap_at_size()` was written back with the new pixels.

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
The one `DT_MIPMAP_FULL` entry it does drop holds no raw: a load that failed (see "A purge tries
a failed load again" below).

An image LEAVING the library is the other case, and the only caller of the all-sizes form.
Its input buffer otherwise outlives the row, with nothing but memory pressure to reclaim it,
and `basebuffer` — which slices that buffer — is handed the stale entry when the image comes
back on Ctrl+Z. It reports `invalid cache entry size 0 for module basebuffer`, the mipmap get
path answers with an 8x8 husk, and no later render replaces it. **Only a developed image shows
this**: an unaltered one is drawn from the embedded JPEG and never asks for the input at all,
which is why the symptom reads as "one broken thumbnail" rather than as a cache bug.

## The embedded-JPEG mode is what the cache was told, not what conf says

> **Established 2026-10-24 against `2fadf8a366`.**

The cache never reads `lighttable/embedded_jpg`. `_init_8()` decides between the embedded JPEG and
the raw from `dt_mipmap_cache_settings_t.embedded_jpg`, which `dt_mipmap_cache_set_settings()`
sets at startup and on `DT_SIGNAL_PREFERENCES_CHANGE`. The lighttable's thumbnail menu
(`_jpg_combobox_changed()`, `gui/actions/display.c`) changes the mode without that signal, so it
tells the cache itself, and does so **before** it purges: a thumbnail regenerated between the
purge and the change would be rendered in the old mode and cached.

The purge drops the thumbnails on disk too (`flush_disk = TRUE`). A file in the disk cache does
not record which mode rendered it, and the disk read in the get path takes it as it is.

Measured with `-d cache` on a copy of a 51-image collection, switching from "Always process the
RAW" to "Always use embedded JPG". When the menu wrote only the conf key, the purged thumbnails
were rendered again from the raw (`fetch … float32 … from original file I/O`, none
`from embedded jpeg`), for more than four minutes, one pipe at a time. With the cache told, 21
came from the embedded JPEG, one image with none became a skull
(`embedded JPEG mode forbids raw processing`), no raw was decoded, and all were done 1.1 s after
the purge.

### A render in flight across the change is not kept

> **Established 2026-10-25 against `a8d4f8e203`.**

The purge cannot reach a thumbnail that is being rendered. `_generate_blocking()` runs under the
entry's write lock, and `dt_mipmap_cache_remove_at_size()` takes the entry with
`dt_cache_testget(..., 'w')`: a locked entry is neither flagged nor removed, only its file on disk
is unlinked. The render cannot be stopped either. The lighttable asks for its thumbnails through
`dt_view_image_get_surface()`, which passes no shutdown flag, and the refresh after the purge only
orphans the job, whose cache entry the render still fills. A worker waiting on
`dt_pipeline_threadsafe_mutex()` already holds its entry and has already chosen the raw, so a
change usually finds several renders in flight.

`_generate_blocking()` therefore reads the mode once, hands it to `_init_8()`, and reads it again
when `_init_8()` returns. If it changed, the result is dropped the way a stopped pipe's is: the
descriptor is restored, the buffer invalidated, `DT_MIPMAP_BUFFER_DSC_FLAG_GENERATE` kept, and the
next request renders it in the new mode (`-d cache` prints `rendered in embedded JPEG mode 0, now
2: not kept`). Waiting for the renders in the purge instead would hold the GUI thread for as long as
the slowest of them, more than a minute here. The renders still run to their end; only their result
is discarded.

Measured with `-d cache` on the same copy, a session started in "Always process the RAW" on an
empty cache, switched from gdb as the menu does (conf, `dt_mipmap_cache_set_settings()`, purge of
the 51 images, thumbtable refresh) once the first raw thumbnail was done. Both runs had seven
`DT_MIPMAP_0` renders in `_generate_blocking()` at the switch. Without the check, all seven
finished from the raw 108 to 131 s after the switch and were kept, and the lighttable then read
each of them from RAM (`will skip disk I/O, found in RAM cache`). With it, all seven finished 14 to
104 s after the switch, were dropped, and were rendered again from the embedded JPEG 0.05 to 0.66 s
after the drop; no raw thumbnail was kept.

Two windows remain, of the same kind and present before: a change between the second read and the
end of `_generate_blocking()`, a few microseconds, and an entry a reader holds read-locked while
the purge passes, which any purge skips.

## The darkroom copies a frame into the thumbnail only when it carries no overlay

*Found `ea59057346`, 2026-10-07.*

At the end of a darkroom run, `dt_dev_darkroom_pipeline()` (`develop/develop.c`) copies the
backbuffer into the mipmap cache (`dt_dev_resync_mipmap_cache()`, then
`dt_mipmap_cache_swap_at_size()`) whenever the run has the preview's output size. Until
`07aea1a673` (2026-06-19) only the preview pipe did this. Since then the main pipe does it too at
zoom == fit, where both pipes render the same frame. `8f7c553fb6` (2026-06-21) kept mask previews
out of that copy, but not the overlays that only the main pipe draws:

- the clipping and raw clipping indicators (`overexposed` and `rawoverexposed` disable themselves
  on any other pipe);
- soft proof and gamut check (`colorout` applies the proofing mode to the FULL pipe only).

After an edit at fit, the main pipe finished after the preview pipe and overwrote its clean copy.
The disk cache then kept the overlay across restarts. A user reported clipping colours in a
thumbnail.

`requested_overlay` now gates the copy. Like `requested_mask_preview`, it is a snapshot taken
before the run.

Measured with `-d perf -d cache` on a 5208x3912 raw at fit. A copy prints `will fit a mip size 1`
followed by `is synchronized from pipeline`. A lone `will fit` comes from `_preview_pipe_finished()`
(`views/darkroom.c`) answering the finished signal of a run that copied nothing.

- **Before** (`-d cache` alone): two copies per edit, about 50 ms apart (106.91/106.97 s,
  113.21/113.27 s, 114.49/114.69 s, 124.49/124.54 s).
- **After, an edit with an overlay on:** the pipes run preview, main, preview. The main run copies
  nothing and the preview run after it does (23.20/23.28 s, 24.77/24.84 s, 30.61/30.68 s,
  31.82/31.87 s).
- **After, toggling the overlay:** the toolbox resyncs the main pipe alone, in 1-34 ms runs. Each
  switch-off produces a copy (25.93, 34.86, 40.60, 43.71 s) and each switch-on produces none (27.29,
  39.71, 42.31 s).
- **After, an edit with no overlay:** both pipes copy, as before (36.76/36.80 s).

The thumbnails stayed clean. Two limits, read from the source:

- The snapshot reads the GUI state when the run starts, while the modules read it when they
  commit, earlier. An overlay switched off between the two lets one overlay frame through. The
  resync that the switch-off queues replaces it on the next run.
- A thumbnail polluted before the fix stays until something rewrites it. Opening its image in the
  darkroom does: the first run on entry copies a clean frame.

The second size in a `synchronized` line is the entry's size before the downscale. It reads 720x450
for an entry that was just allocated and the fitted size otherwise, so it does not mean the image
was stretched.

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
| `_generate_blocking`, `DT_MIPMAP_FULL` | every decoder failed | `dt_imageio_open()`'s reason: see "Which loader failure a skull names" |
| `_generate_blocking`, `DT_MIPMAP_FULL` | no source any more: the file went after `_init_8`'s check, or the image left the image cache | `FILE_NOT_FOUND` |
| `dt_imageio_open` | the path exists but is not a regular file | `FILE_NOT_FOUND` |
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

### "File not found" is about the source, never the cached thumbnail

*Found `c1c4123fcb`, 2026-10-20, by reading every path that returns `DT_IMAGEIO_FILE_NOT_FOUND`;
the table's two `FILE_NOT_FOUND` rows after the first come from this reading. The `EXISTS` and
`IS_REGULAR` columns below are measured with `g_file_test()`, GLib 2.80.0, Linux, as uid 1000 —
not root, which passes every permission check. The labels are by reading.*

`FILE_NOT_FOUND` reaches a thumbnail by three paths, all about the image's source file: the
original, or its local copy.

1. `_init_8()`, before anything else: `_write_mipmap_to_disk()` asks
   `dt_image_choose_input_path()` for a source and none exists. The candidates, in order: the
   local copy (then its legacy path) when the image is flagged `DT_IMAGE_LOCAL_COPY`, then
   `fullpath`, then the local copy again as a last chance. An image that left the image cache
   gives no `dt_image_t`, hence no path either.
2. `_generate_blocking()` at `DT_MIPMAP_FULL`, reached through the export pipe `_init_8()`
   starts: the same lookup fails there, so the file went in between — a drive unplugged
   mid-render. `dt_imageio_export_with_flags()` hands the full-size buffer's status back as is.
3. `dt_imageio_open()`: a path that exists but is not a regular file returns `!DT_IMAGEIO_OK`,
   which is `1`, which is `FILE_NOT_FOUND` by its position in the enum rather than by choice.

"Exists" is `g_file_test(G_FILE_TEST_EXISTS)`, which is `access(F_OK)` on POSIX:

| The source path | `EXISTS` | `IS_REGULAR` | Label |
|---|---|---|---|
| absent: deleted, moved, renamed, drive unplugged, share not mounted | 0 | 0 | File not found (path 1) |
| a broken symlink | 0 | 0 | File not found (path 1) |
| a file in a directory without search (`x`) permission | 0 | 0 | File not found (path 1) |
| a file without read permission (mode `000`) | 1 | 1 | a decoder's failure, not "File not found" |
| a directory named like an image | 1 | 0 | File not found (path 3) |

Two `FILE_NOT_FOUND` returns never reach a thumbnail: `dt_imageio_open_j2k()`'s (it cannot open
the file or read its header) runs inside `dt_imageio_open_raster()`, whose chain ends on
`FILE_CORRUPTED`; and the `dt_imageio_open_libraw()` stub of a build without LibRaw is never
called, since `dt_imageio_open_raw()` calls it only under `HAVE_LIBRAW`.

The disk cache never produces it. In the cache's allocate callback, a missing `.jpg` leaves the
entry flagged for generation, and so does an unreadable one, which is deleted; `_init_8()` then
renders from the source. A `.jpg` that loads is shown without the source being looked at, and so
is an entry already in RAM: a missing original keeps showing its last thumbnail, and the skull
appears only when no thumbnail is left to show.

Like every skull, it stays in RAM until the entry is evicted or invalidated. The file coming back
does not change that: the full-size entry stays flagged for generation, but the thumbnail entry
that asked for it does not, and the mount signal `libs/collect.c` listens to refreshes the
folders' accessibility (`dt_film_set_folder_status()`), not the mipmap cache.

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
`dt_mipmap_cache_remove()` invalidates it, together with the failed full-size load behind it.

`_view_image_get_surface_internal()` (`views/view.c`) paints the label: every consumer
(lighttable, filmstrip, preview window, map, print, slideshow) goes through it. It runs in a
worker thread, so it builds its own Pango font description rather than borrowing bauhaus's, which
`dt_bauhaus_load_theme()` frees from the GUI thread.

### Which loader failure a skull names

*Measured 2026-10-20 on `c1c4123fcb`, before and with this change, by a scratch program linked
against `libansel.so`: it imports a good file into a `:memory:` library, breaks the file, asks for
a `DT_MIPMAP_2` thumbnail and the `DT_MIPMAP_FULL` buffer and prints their `status`, repairs the
file, calls `dt_mipmap_cache_remove()`, and asks again. `--disable-opencl`,
`lighttable/embedded_jpg=0`, so every thumbnail goes through the export. The table's broken files
are made from one Nikon Z 6 NEF, which rawspeed decodes. Labels from `view.c`. Seen in the
lighttable the same day, in French, on the same files imported whole and then broken on disk:
"Boîtier non supporté", "Erreur d'E/S" twice, "Fichier corrompu" twice.*

When every decoder fails, `dt_imageio_open()` returns the first failure that says why a decoder
which recognised the file could not load it — `UNSUPPORTED_FEATURE`, `UNSUPPORTED_CAMERA`,
`LOAD_FAILED` or `IOERROR` — and `FILE_CORRUPTED` otherwise (`_decoder_reason()`). Each chain
(`dt_imageio_open_raster/_raw/_hdr()`) applies the same rule to its own decoders.

The other two failures say nothing once the whole cascade has run. Every raster and HDR decoder,
and LibRaw, answer `FILE_CORRUPTED` both to another format and to a broken file in their own;
rawspeed answers `UNSUPPORTED_FORMAT` to a file it cannot parse, a cut header included. The
cascade tries every decoder on every file, so kept, these would make a truncated JPEG read
"Unsupported format": rawspeed, tried after the raster chain, does not know JPEG. "Unsupported
format" therefore never reaches a skull.

| Broken file | rawspeed | LibRaw | Before | Now |
|---|---|---|---|---|
| model renamed to an unknown `NIKON Z 0` | `UNSUPPORTED_CAMERA` | decodes it | no skull | no skull |
| the same, cut to 4 MB | `UNSUPPORTED_CAMERA` | I/O error | Corrupted file | Unsupported camera |
| 0 bytes | `FileIOException`: `IOERROR` | fails | Corrupted file | I/O error |
| mode `000` | `FileIOException`: `IOERROR` | fails | Corrupted file | I/O error |
| cut to 4 MB | `RawDecoderException`: `UNSUPPORTED_FORMAT` | fails | Corrupted file | Corrupted file |
| cut to 300 bytes, inside the TIFF header | `IOException` | fails | Corrupted file | Corrupted file |
| 1 MB of `A` | no decoder: `UNSUPPORTED_FORMAT` | fails | Corrupted file | Corrupted file |

**Before, the table row above that says "the loader's" status was wrong in effect.** The loader's
value reached `dsc->status`, but every chain ended on `return DT_IMAGEIO_FILE_CORRUPTED` and the
cascade on `dt_imageio_open_hdr()`'s, so a failed load always read "Corrupted file".

LibRaw follows rawspeed on every raw, not only on CR3 (`dt_imageio_open_raw()`). A camera or a
feature rawspeed does not support leaves a skull only when LibRaw fails too: the first row.

rawspeed's `IOException` is `FILE_CORRUPTED`, not `IOERROR`. rawspeed throws it on reading past
the end of the copy `FileReader::readFile()` made of the whole file, so it means cut or corrupt;
failing to read the file throws `FileIOException`. `decodeRaw()`, `decodeMetaData()` and
`checkSupport()` rewrap it as a `RawDecoderException`, so only `RawParser::getDecoder()` lets it
out, on a header cut short. Cut to 60 %, an ARW, a CR2, a RAF, an ORF, an RW2 and a DNG all
failed in rawspeed through `RawDecoderException`, before and now. LibRaw then decoded the ARW, the
ORF and the DNG anyway (under gdb, `dt_imageio_open_libraw()` returned `DT_IMAGEIO_OK`): the first
two gave a picture, the DNG a `PROCESSING_FAILED` skull from the pipe.

Reachable by reading, not measured: `UNSUPPORTED_FEATURE` (rawspeed: a sample type or size it does
not handle, a `RawDecoderException` that mentions "supported", a `CameraMetadataException`) and
`LOAD_FAILED` (rawspeed: a CFA image with more than one component per pixel).

Two statuses of the table above never reach a skull, by reading. `IOERROR` from
`format->write_image`: the thumbnail's format copies into the cache buffer and returns `0`.
`ABORTED`: the thumbnail's pipe is local to `dt_imageio_export_with_flags()`, so only its
`shutdown_ext` can stop it, and that external stop restores the entry.

### A purge tries a failed load again

*Measured 2026-10-20 on `c1c4123fcb`, before and with this change, as above. Seen in the
lighttable the same day: once the five broken files were repaired, "Purge selected thumbnails from
cache" brought them back without a restart.*

`dt_mipmap_cache_remove()` — "Purge selected thumbnails from cache", an embedded-JPEG mode change,
any history change — drops the thumbnails, and also the `DT_MIPMAP_FULL` entry when its `status` is
not `DT_IMAGEIO_OK`. Without that, the purged thumbnail was regenerated, but the export behind it
asked for `DT_MIPMAP_FULL` and read the failure back. Fourteen broken files whose load failed —
the NEF renamed and cut, emptied, made unreadable, cut to 4 MB and filled with `A`; the NEF cut at
six sizes from 100 bytes to 50 kB; and a CR2, a RAF and an RW2 cut to 60 % — were repaired before
the purge: the thumbnail stayed a skull with the same status for all fourteen. Only eviction or
`dt_mipmap_cache_remove_all_sizes()`, an image leaving the library, dropped that entry. Now all
fourteen give their picture back after the purge.

Eviction does retry a failure by chance once enough other images are loaded. The full-size cache
holds `worker_threads` + 1 entries and is collected above 80 % of that, by reading: in the
lighttable, with seven images and `worker_threads=4`, each broken file was decoded twice before
any purge, once per thumbnail request. A failure stuck in the cache is therefore the case of a
few images, and the purge is what retries it there.

The decoded raw of a healthy image still survives a purge: over the same import, thumbnail,
purge, thumbnail sequence, `-d cache` logs one `from original file I/O`, before and now.

So does a raw decoded from a broken file, since its load succeeded. LibRaw decodes the DNG cut to
60 % and the pipe fails on it: the `PROCESSING_FAILED` skull came back after the repair and the
purge, before and now. The cut ARW and ORF gave a picture, which a purge keeps in the same way.
Nothing in the cache notices that the file changed.

The failure stays cached until then rather than not being cached at all. A thumbnail's export
loads the image through `dt_dev_load_image()`, which already asks for `DT_MIPMAP_FULL`
(`_dt_dev_mipmap_prefetch_full()`), then asks for it again itself. Uncached, every thumbnail of a
broken file would run the whole decoder cascade twice, and again for each thumbnail size.

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
