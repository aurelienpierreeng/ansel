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
global. **Never `g_get_user_cache_dir()` directly**, for two independent reasons:

- **It is the wrong directory on Windows.** GLib returns `FOLDERID_InternetCache` there —
  `%LOCALAPPDATA%\Microsoft\Windows\INetCache`, the shell folder still labelled "Temporary
  Internet Files" — which Storage Sense clears by default on Windows 11 and Disk Cleanup targets.
  A thumbnail cache written there is deleted by the OS behind the user's back, which reads as "the
  disk cache never works on Windows", and it is invisible to a user who goes looking (#1473). The
  default is now `%LOCALAPPDATA%\ansel\cache` on Windows and `g_get_user_cache_dir()/ansel`
  elsewhere, with an absolute `XDG_CACHE_HOME` winning on every platform. `%LOCALAPPDATA%` is read
  from the environment rather than through `g_get_user_data_dir()`, which resolves to the same
  folder but takes `XDG_DATA_HOME` first — a variable that says where data goes and must not steer
  a cache.
- **It ignores `--cachedir`.** That is how `libs/textnotes.c` came to build a download's path under
  `g_get_user_cache_dir()/ansel/downloads` while creating the directory under
  `dt_loc_cachedir()/downloads`: with `--cachedir` given, it made one directory and wrote into
  another, which did not exist.

`tests/unittests/test_loc_cache_dir.c` pins the XDG precedence and that the default is not a
shell-managed temporary folder. The thumbnail cache path itself is built on top of this by
`dt_mipmap_cache_get_filename()`, which hashes the library's absolute path into the directory
name so two libraries do not share thumbnails.

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
