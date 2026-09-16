# The XMP crawler

`src/control/crawler.c` answers one question about every image in the library — *is the sidecar
on disk newer than the row, and does the image still have its `.txt` / `.wav` companions?* — and
shows what it found in a dialog the user acts on. It runs once per session, when
`run_crawler_on_start` is set.

This note is the reasoning behind the shape it has now. The numbers are measured on one machine
and one library, named where they are used; the library is on a GVFS/SMB mount, which is what
made every cost here visible in the first place.

## The cost is round-trips, not work

The crawl used to ask the filesystem up to six questions per image, one `stat()` each: does the
image exist, does its XMP exist, when was the XMP written, is there a `.txt`, a `.TXT`, a
`.wav`, a `.WAV`. On a local disk that is free. On a network mount the round-trip **is** the
cost.

Measured, 1969 images over 18 film rolls, 3967 files, GVFS/SMB:

| approach | time |
|---|---|
| six `stat()` per image (before) | **102 s** |
| `scandir` + `stat` per entry | 45.3 s |
| `scandir`, names only | 2.97 s |
| **one GIO listing per folder, names + mtimes** | **1.1 s** |

One directory listing answers all six questions for every image in that folder, and SMB returns
the modification times inside the listing itself, so the XMP timestamp costs nothing beyond it.

**Do NOT "improve" this by parallelising the per-file lookups instead.** That was measured on
the same share and does not work: `gvfsd-fuse` multiplexes every FUSE request through a single
daemon. Over 300 images — 1 thread 9.86 s, 4 threads 9.47 s, 8 threads 9.55 s, 16 threads 10.27
s, 32 threads 12.44 s, 64 threads 18.93 s. Four threads gained 4%, inside the noise; 64 ran
twice as slow as one. What this path needs is fewer round-trips, not overlapping ones.

The same argument moved the crawl off the startup path entirely. It used to run to completion
inside `dt_init()`, before `dt_control_init()` and before the main window existed. Same library,
same share, to the same startup milestone: `[screen resolution]` at 98.01 s → **0.29 s**, the
imageio modules loaded at 99.80 s → **1.32 s**. It is a `DT_JOB_QUEUE_SYSTEM_BG` job now, and it
posts its dialog to the GUI thread only if it found something.

## A miscased name is a question for the filesystem, not for the hash table

Windows and macOS resolve a filename without regard to case, and so does an SMB server: `stat()`
found `IMG.NEF.XMP` when asked for `IMG.NEF.xmp`. An exact lookup in a listing does not, so a
folder carries a second index of casefolded names — **used as a filter, never as an answer**. It
says some file exists under another spelling; a `g_stat()` of the database's own spelling then
decides, which is the question the per-file code asked, put to the same filesystem. Found where
the filesystem folds case, missing on ext4 — and rightly: there the database's name really names
no file, and taking the other one would hand that image another file's sidecar and companions.

It costs one `stat()` per name that exists only under another spelling, which on a library whose
names agree with the database is none. The key is Unicode-normalised before it is folded, since
macOS stores names decomposed and a database may carry the composed spelling of the same name;
the confirming `stat()` is what makes a generous key safe.

A listing that fails part-way — a share dropping mid-read — is discarded whole. Kept as far as
it got, it would read an image whose `.txt` came after the break as having none, and clear its
flag.

## The row is not the only copy of `flags`

`main.images.flags` holds the two companion-file bits the crawl owns **and** the rating and
colour label the user owns. Two rules follow, and both were paid for.

*The write is masked, never a whole word.* `flags` is read from the row before the folder is
listed, and that listing is a filesystem round-trip — up to a second on a network share, longer
on one that has gone away. A star set in that window lives in the same word, and writing the
word back would silently revert it. `dt_image_repository_set_flags_masked()` exists for that.

*An image the user has looked at has a cache entry, and that entry is the copy that wins.*
Releasing an image-cache entry writes the whole `dt_image_t` back — that is how a rating reaches
the database at all (`metadata/ratings.c`). So a row updated behind a stale entry is not
durable: the next rating overwrites it, at a moment nothing connects to the crawl. When the
image has an entry, the entry is therefore what the crawl compares against **and** what it
edits; its write lock is the one the rating path takes, which is what serialises the two. The
row is compared against only when there is no entry, which is what makes the row the only copy.

Two traps inside that, each with a test in `tests/unittests/test_image_cache_flags_writeback.c`:

- **Compare `cached->flags`, not the cursor's `flags`.** The two can disagree on these very bits
  — a row written behind the entry earlier — and a guard reading the row then finds nothing to
  do, leaves the entry stale, and lets its next release write the stale word back over the row:
  the same failure, inverted.
- **`dt_image_cache_testget()` returns NULL for two different situations**, no entry and an
  entry someone holds this instant. Writing the row in the second case writes behind a live
  entry whose release reverts it. `dt_image_cache_get_existing()` tells them apart through
  `dt_cache_contains()`, which does not take the entry's lock, and waits for a held entry
  instead — while still never creating one, so a crawl over the whole library does not pull the
  whole library into the cache.

Releasing with `DT_IMAGE_CACHE_RELAXED` writes the row without queueing an XMP write, which the
one job whose purpose is to find out whether the sidecars are in sync must not do;
`DT_IMAGE_CACHE_MINIMAL` gives the lock back with no write at all when nothing changed.

## Stopping

**Nothing cancels a running job when Ansel quits.** `dt_control_quit()` and
`dt_control_shutdown()` only clear `running`, then `pthread_join()` the workers. So a job that
consults `dt_control_job_get_state()` alone — as `preload_image_cache()` does — never reacts to
a quit, and the quit waits for it: 1.1 s on a live share, the mount timeout *per folder* on one
that has gone away. The crawl checks `dt_control_running()` as well, before each image, between
the entries of a listing, and again after the listing.

That last check is the one that matters for correctness rather than latency: **a listing a
cancel cut short must never be acted on**, since a name missing from it reads as a file missing
from disk and would clear the companion flags of every image whose `.txt` was not listed yet. A
cancelled crawl frees its partial result instead of showing it.

One stop stays out of reach: a single `opendir()` or `readdir()` blocked on a dead mount cannot
be interrupted from user space, so a quit can still wait for that one call to time out. What
changed is that it waits for one call, not for every remaining folder.

`dt_image_repository_foreach_with_path()`'s callback returns `gboolean` for this, and the walk
ends at the first `FALSE`. `dt_control_crawler_run()` — the menu's synchronous entry point — has
no job and runs to its end, as it always did.

## The modularity decision, taken on purpose

This branch added three genuinely new jobs to `crawler.c`: the folder inventory, the
background-job orchestration, and keeping the image cache in step. The file already had two of
its own — deciding what to report, and the GTK dialog that reports it — and is **1155 lines**,
570 of scanner and 585 of dialog sharing one include list.

**The decision is that the folder inventory becomes a module of its own, and that it does not
happen on this branch.** Both halves are deliberate.

It should be a module because nothing about it is the crawler's. **Seventeen** files in `src/`
(22 counting vendored code) enumerate a directory by hand today. And the "name of the file
beside this one" computation — scan back to the last `.`, put another extension there — is
written out at **five** sites in three files: three in `common/image.c`
(`dt_image_get_audio_path_from_path()`, `_text_path_legacy_if_exists()`,
`_text_path_legacy_build()`), one in `control/jobs/control_jobs.c`, and the one added here,
which is literally the fifth copy.

That duplication is not free: the copy in this file searched the last `.` of the *file name*
where the ones it was modelled on searched the whole *path*. For a name with no extension in a
folder whose path has a dot, the two answer differently — the older spelling points outside the
image's own directory — and it took a review to notice. An inventory answering *does this name
exist in this folder, and when was it last written* — with the casefold-then-`stat()` rule and
one sibling-name helper inside it — turns the fifth copy into the first shared one.

It does not happen here because this branch is a reviewed series of behavioural fixes, and
folding a refactor into it would make both harder to judge and harder to revert. The repo's
habit is one subject per branch. What stays with the crawler afterwards is the policy — which
images to report, how the flags are reconciled with the cache — and the job that drives it; the
GTK dialog leaves separately, as PR 7 of `control-split.md`.

## What has NOT been exercised

These runs are read with `-d control`. The job traces bracket the crawl (`[run_job+]` and
`[run_job-]`, both carrying the job's description, `crawl XMP files`), and the crawl itself ends
with one summary line — `[crawler] done: N images, M folder listings, K to report, T s`, or
`cancelled:` when it stopped early. The brackets alone would not do: they say the function
returned, not that it walked anything, so a crawl that stopped at its first check looks exactly
like one that visited the whole library. The per-image and per-folder lines stay for the
exceptions only — a missing image, a newer sidecar, a folder that could not be listed.

The unit tests cover the logic, not the threading move this work exists for. Still to run by
hand, and to record here when they have been:

- the background job under a live GUI, on a real network share;
- quitting mid-crawl;
- the dialog itself, on a library where a sidecar really is newer than the row;
- a share that goes offline between two folders.

What the tests do pin: the walk's early exit (`test_image_repository.c`), the cache-writeback
rules and the miscased-name answer (`test_image_cache_flags_writeback.c`, which skips itself on
a filesystem that folds case). The listing rewrite was checked against the per-file `stat()`
version on the real library — 1969 images, **0 divergences** — before it replaced it.