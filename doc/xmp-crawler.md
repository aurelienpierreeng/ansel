# The XMP crawler

`src/control/crawler.c` answers one question about every image in the library — *is the sidecar
on disk newer than the row, and does the image still have its `.txt` / `.wav` companions?* — and
shows what it found in a dialog the user acts on. It runs once per session, when
`run_crawler_on_start` is set.

This note is the reasoning behind the shape it has now. The numbers are measured on one machine
and one library, named where they are used; the library is on a GVFS/SMB mount, which is what
made every cost here visible in the first place.

The machine used in the tests is a Thinkpad P16v Gen1 (AMD Ryzen 7 PRO 7840HS, without GPU card)
running under LinuxMint 22.3.

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

## This file should be several modules

`crawler.c` holds five jobs at once: deciding which images to report, the GTK dialog that
reports them, the directory inventory that decision reads, the background job driving the whole
thing, and keeping the image cache in step with the rows it writes. That is 1155 lines — 570 of
scanner and 585 of dialog — sharing one include list.

The inventory is the piece that most clearly belongs elsewhere, and the evidence for that sits
in the tree rather than in taste. **Seventeen** files in `src/` (22 counting vendored code)
enumerate a directory by hand. The "name of the file beside this one" computation — scan back to
the last `.`, put another extension there — is written out at **five** sites in three files:
three in `common/image.c` (`dt_image_get_audio_path_from_path()`,
`_text_path_legacy_if_exists()`, `_text_path_legacy_build()`), one in
`control/jobs/control_jobs.c`, and one here.

That duplication is not free. The copy here searched the last `.` of the *file name* where the
ones it was modelled on searched the whole *path*; for a name with no extension in a folder
whose path has a dot, the two answer differently — the older spelling points beside the folder,
outside the image's own directory — and it took a review to notice. An inventory module
answering *does this name exist in this folder, and when was it last written*, carrying the
casefold-then-`stat()` rule and one sibling-name helper, turns the fifth copy into the first
shared one.

**Why the split did not come with the code.** When this was written, the project had not settled
the rule that a new job gets a new module — it was being written down while this work was
reviewed — and what landed here was a series of behavioural fixes. Folding a refactor into them
would have made both harder to judge and harder to revert. So the split is owed, as a piece of
work of its own, and this section is the argument for it rather than a note about how it was
avoided.

What stays with the crawler when that happens is the policy — which images to report, how the
flags are reconciled with the cache — and the job that drives it. The GTK dialog leaves on its
own account, as PR 7 of `control-split.md`.

## What the manual runs showed

Run on 2026-09-17 against the installed build (`0.0.0+5028~gcbfd00a510`) on the machine these
numbers come from: the real library, 1963 images in 18 film rolls on the GVFS/SMB share.

They are read with `-d control`. The job traces bracket the crawl — `[run_job+]` and
`[run_job-]`, both carrying the job's description, `crawl XMP files` — and the walk itself ends
with one summary line: `[crawler] done: N images, M folder listings, K to report, T s`, or
`cancelled:` when it stopped early. The brackets alone would not do, since they say that the
function returned and not that it walked anything. The per-image and per-folder lines are for
the exceptions only: a missing image, a newer sidecar, a folder that could not be listed.

**The job no longer gates the window.** The crawl was picked up at 3.066 s, `[init] startup
took` printed at 3.068 s, and the walk ended at 3.641 s with `done: 1963 images, 18 folder
listings, 0 to report, 0.58 s`. The two overlap: initialisation finished while the walk was
still running, and the walk outlived it by 0.57 s. Against the 98-102 s it used to hold the
window back, the measurement that matters here is not the duration but the overlap.

**The dialog was exercised twice, and both times it wrote.** An earlier run of the same test
found a genuinely newer sidecar in the library, `2026-08-13_DSF2454.RAF.xmp`, and reported `1 to
report`; the dialog opened and the newer XMP was taken into the database. The next run reported
none. The synthetic case behaved the same — a copied folder with a `touch`ed sidecar under a
throwaway configdir: `done: 7 images, 1 folder listings, 1 to report, 0.01 s`, the dialog
listing that one image, synchronised from it.

**Cancellation stops the walk; a call already in flight is what a quit still waits for.** With
the SMB daemon frozen (`pkill -STOP gvfsd-smb`) and Ansel asked to quit, the window closed and
the process stayed alive, blocked inside a listing, then exited once the daemon was thawed:
`cancelled: 1758 images, 18 folder listings, 0 to report, 21.24 s` — 205 images short of the
library, the last crawler line naming the folder it was in. One attempt of several took a few
seconds more to exit after the thaw, which is the same limitation seen from the other side: the
blocked call has to return before anything can be noticed.

**A folder that cannot be listed loses no flag.** Share unmounted before launch: 18 `cannot
list`, 1963 `is missing`, `done: 1963 images, 18 folder listings, 0 to report, 0.03 s`. Share
killed mid-crawl, the daemon stopped and then killed: two folders unreadable and exactly their
215 images (9 + 206) read as missing, the rest of the walk unaffected, `done: 1963 images, 18
folder listings, 0 to report, 2.57 s`. The flag snapshot taken around both runs is identical,
image for image — nothing was cleared, which is the claim those two runs exist to check.

**Still not exercised: a listing that fails part-way.** Killing the daemon made the enumeration
call itself fail — `cannot list`, the folder memoised empty — rather than breaking a read
already under way, so the `failed part-way` branch has never run. Reaching it needs a filesystem
that fails mid-read on cue.

A SIGSEGV was seen once during that session, on quit, in `libs/modulegroups.c`'s
`_ensure_page_widgets()` reached from `dt_cleanup()`'s main-context drain. It is recorded here
only so that nobody repeating these runs attributes it to the crawl: the dump carries 32 threads
and not one of them is anywhere in this file.

What the unit tests pin, alongside all of the above: the walk's early exit
(`test_image_repository.c`), the cache-writeback rules and the miscased-name answer
(`test_image_cache_flags_writeback.c`, which skips itself on a filesystem that folds case). The
listing rewrite was checked against the per-file `stat()` version on the real library — 1969
images at the time, **0 divergences** — before it replaced it.