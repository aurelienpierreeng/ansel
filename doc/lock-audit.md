# Lock audit

*Audited 2026-08-27, against the pinned submodules.*

Every mutex and rwlock in `src/`, what it protects, and whether the reason still holds.
Three locks were removed as a result; the reasoning is recorded here rather than in a commit
message so that the next person to wonder "can this go?" has the answer without repeating
the investigation.

## The rule the tree now follows

**Locks are recursive by default.** `dt_pthread_mutex_init(..., NULL)` — 54 of the 56 init
sites — produces a recursive mutex, and `dt_pthread_rwlock_t` tracks same-thread writer
depth. A thread re-entering a lock it already holds cannot race itself: no other thread is
inside the critical section, so the data is as safe at depth 2 as at depth 1. A
non-recursive mutex answers that situation with a deadlock, which is worse than what it
prevents.

Two consequences, both live:

- `pthread_cond_wait()` releases a mutex **once**. Waiting at depth > 1 never releases it,
  so nothing can signal and the wait never returns. All eight wait sites here wait at depth
  1. See the warning on `dt_pthread_cond_wait()`.
- Recursion hides one real class of bug: a function that breaks an invariant, then calls
  something that re-enters and reads the half-updated state. A deadlock would have exposed
  that. This is a deliberate trade, not an oversight.

Two locks are **not** recursive and cannot be: `_main_message_lock` (`darktable.c`) and the
pixelpipe wait queue (`caches/pixelpipe_cache_wait.c`) use a static
`PTHREAD_MUTEX_INITIALIZER`, which takes no attribute, and no portable static recursive
initialiser exists — glibc hides `PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP` behind
`_GNU_SOURCE`, mingw spells it without the `_NP`, and neither is visible in our include
environment. Both are static so they are valid from program start; that property is worth
more than recursion here.

## Where per-image serialization actually lives

**The image cache entry lock.** `dt_image_cache_get(imgid, 'w')` guards *this image's
database rows and its XMP sidecar together* — it is the per-image critical section for all
of an image's persistent state, not just for one library. `dt_image_write_sidecar_file()`
takes it before writing, so two threads cannot write the same sidecar: the second gets
`DT_IMAGE_WRITE_SIDECAR_CACHE_BUSY`.

This matters because it is the reason several *global* locks turned out to be redundant.
A global lock that exists to stop two threads touching the same file is solving, badly, a
problem that a per-image lock already solves precisely — and it serializes every unrelated
image as a side effect.

## Removed

### `readFile_mutex` — RawSpeed `readFile()`

Comment claimed: *"RawSpeed readFile() method is apparently not thread-safe."* "Apparently"
was doing a lot of work. It serialized **every raw file read in the application**.

Read the pinned source (`src/external/rawspeed`, v3.5-2921):
`FileReader::readFile()` opens a local `FILE*`, allocates a local vector, `fread`s into it
and closes. No shared mutable state; `fileName` is a read-only member. Distinct `FILE*`
streams do not interfere under POSIX.

The one plausible hazard is the exception formatter, and upstream already handled it:

```c++
#if defined(HAVE_CXX_THREAD_LOCAL)
  static thread_local std::array<char, bufSize> buf;
```

and our generated `rawspeedconfig.h` carries `#define HAVE_CXX_THREAD_LOCAL`, so that is
the branch we compile. (The `#else` fallback warns that exception *text* may be garbled —
cosmetic, and not our configuration.)

**Verdict: unnecessary. Removed.**

### `exiv2_threadsafe` — exiv2 reads (13 sites) and writes (4 sites)

The struct member carried its own doubt: *"Exiv2 readMetadata() was not thread-safe prior to
0.27. **FIXME: Is it now?**"*. Answered here.

Exiv2 0.27.7's own `README.md` §2.14:

> The Exif and IPTC code is reentrant. The XMP code uses the Adobe XMP toolkit (XMP SDK),
> which according to its documentation is thread-safe. It actually uses mutexes to serialize
> critical sections. However, the XMP SDK initialisation function is not mutex protected,
> thus `Exiv2::XmpParser::initialize` is not thread-safe. In addition,
> `Exiv2::XmpProperties::registerNs` writes to a static class variable, and is also not
> thread-safe.

Confirmed in the bundled toolkit (`src/external/exiv2/xmpsdk/src/XMPCore_Impl.hpp`): every
public entry point goes through `XMP_ENTER_WRAPPER`, which takes an `XMP_AutoMutex` on the
global `sXMPCoreLock`. Only `XMP_ENTER_WRAPPER_NO_LOCK` skips it, and its own comment says
it is for `WXMPMeta_Initialize_1`.

So the unsafe surface is exactly two functions, and Ansel confines both to startup and
shutdown:

| Function | Called from | When |
| --- | --- | --- |
| `XmpParser::initialize()` | `dt_exif_init()` (`exif.cc:2408`) | `darktable.c:1400` |
| `XmpProperties::registerNs()` x3 | `dt_exif_init()` | `darktable.c:1400` |
| `XmpParser::terminate()` | `dt_exif_cleanup()` | `darktable.c`, shutdown |

The first worker thread is created by `dt_control_init()` at `darktable.c:1583` — 183 lines
later — and nothing threads before it. That is precisely the discipline exiv2 asks for:
*"it has to be initialized and terminated before and after starting any threads."*

**This is an invariant to preserve, not an accident.** If a namespace ever needs registering
at runtime, or `XmpParser::initialize()` is ever reached lazily from a worker thread, the
reasoning above stops holding.

**Verdict: unnecessary. Removed, reads and writes alike.**

#### Why the crash that motivated it does not contradict this

Sentry #129978857 (fix `a7890b7054`, 2026-06-25): SIGABRT, heap corruption in `free()`
during import, *"up to four worker threads inside `dt_exif_xmp_write_with_imgpath()` at
once"*. Two things about it:

- Those four threads were importing four **different** images into four **different**
  sidecars. It was never a same-file race — the corruption was in shared library state.
- It predates the exiv2 submodule (pinned 2026-08-25, `6ac9d02a3a`) by two months. It
  happened against whatever exiv2 the build linked then: unknown version, unknown
  configuration, possibly the full Adobe SDK rather than the bundled one.

What can be shown is that the *currently pinned* source is safe for the calls we make. What
cannot be shown is what crashed in June. If concurrent-import heap corruption reappears,
this section is the first place to look, and the answer is more likely to be a runtime
`registerNs` than `readMetadata`.

## Kept, with findings

### `plugin_threadsafe` — removed; it was four unrelated concerns on one global lock

Named as though it serialized plugins. It had four consumers doing four different things,
none of which is per-image — so the image cache entry lock, which is keyed on `imgid`,
could not substitute for any of them:

| Consumer | What it guarded | Resolution |
| --- | --- | --- |
| `imageio/imageio_rawspeed.cc` | one-time init of the `CameraMetaData` singleton | **lock deleted**, `std::call_once` |
| `iop/watermark.c` | rsvg/cairo global font state | module-owned `_rsvg_lock` |
| `imageio/storage/disk.c` | a shared expansion context + filename allocation | **shared state removed**; module-owned lock for what is left |
| `iop/lens.c` | modifier construction | **lock deleted** — LensSerious is stateless |

**A correction worth recording**, because it was nearly missed: `global_mutexes.h` listed
`iop/lens.cc` among its consumers, and that file no longer exists. It is tempting to read
that as a stale reference to a dead consumer. It was not — the file was RENAMED to
`iop/lens.c` in the LensSerious migration and still took the lock in four places. The header
was under-listing a live consumer, not over-listing a dead one. A missing filename is not
evidence of a missing caller; grep for the symbol, not the file.

#### rawspeed: the lock was hiding a broken double-checked lock

```c
static CameraMetaData *meta = NULL;
if(IS_NULL_PTR(meta))                                   // unsynchronised read
{
  dt_pthread_mutex_lock(dt_plugin_threadsafe_mutex());
  if(IS_NULL_PTR(meta)) meta = new CameraMetaData(camfile);
```

`meta` is a plain pointer, not atomic: the outer read raced the store, and nothing ordered
publication of the pointer against construction of the object. Now `std::call_once`, which
needs no lock of ours, publishes correctly, and — unlike `pthread_once` — leaves the flag
unset if the initialiser throws, so a corrupt `cameras.xml` is retried instead of latched
as "done".

#### disk.c: the shared state went away instead of being locked

The genuine race was not the filename. It was this:

```c
d->vp->filename = input_dir;
d->vp->jobcode  = "export";
d->vp->imgid    = imgid;      // ONE struct, reused by every image in the export
d->vp->sequence = num;
gchar *result_filename = dt_variables_expand(d->vp, pattern, TRUE);
```

`d->vp` is allocated once in the storage module's params and shared by every `store()` call,
which runs in parallel. Two threads exporting different images both wrote `->imgid` and then
expanded — the lock was all that stopped a file being named from another image's variables.
`store()` now builds its own context and destroys it on every exit path: no shared state, so
nothing to serialize.

What still needs a lock is the part that genuinely spans images — picking a filename no
other thread has taken:

```c
while(g_file_test(filename, G_FILE_TEST_EXISTS))
  snprintf(c, filename_free_space, "_%.2d.%s", seq, ext);
```

That is check-then-create *across* images, which no per-image lock can cover. It is now a
module-owned lock, and it is still not airtight: another process can create the file between
the test and the format writer opening it. **The airtight version claims the name with
`O_CREAT|O_EXCL` and retries on `EEXIST`**, which requires the format writers to accept a
descriptor rather than a path. That is the real fix and it is not done here.

#### lens.c: deleted

The lock around `get_modifier()` was vestigial from the lensfun era, when
`lf_modifier_new()` touched a shared `lfDatabase`. LensSerious, which replaced it, is
stateless and thread-safe by design — confirmed by its author — so modifier construction
serialises against nothing and needs no lock.

Worth keeping as a method note: this audit initially kept that lock, on the grounds that
`get_modifier()` *looked* safe (a `const` per-piece `d`, caller-local outputs, a reader
documented lock-free with a thread-local handle) but had not been proven so across every
resolver path. That was the right default for something read from the outside. It was also
answerable in one sentence by the person who wrote the library — which is worth more than
another hour of reading, and worth asking for before assuming.

### `pipeline_threadsafe`

Not a third-party lock. It stops concurrent export/thumbnail pipelines *deliberately*: the
CPU is the bottleneck and the pixel code is already parallel through OpenMP, so it buys no
throughput — it bounds peak memory. Keep.

### `capabilities_threadsafe`

Guards `g_list_append`/remove on `darktable.capabilities`. Internal, small, correct. Keep.

## Inventory

55 `dt_pthread_mutex_t` and 6 `dt_pthread_rwlock_t` declarations remain, plus two raw
`pthread_mutex_t` that bypass the wrapper:

- `system/atomic.c:50` — `dt_atom_mutex`, the fallback path for platforms without atomics.
  Deliberate: it must not depend on anything above it.
- `colorprofiles/iop_profile.c:1094` — `_profile_info_lock`, raw *because* the `_DEBUG`
  wrapper used to be a fatter struct that a static initialiser could not fill. **That reason
  is gone** now that there is one implementation with a single member. **Recommended
  follow-up:** move it to `dt_pthread_mutex_t` and regain the `-Wthread-safety` annotations.

## Why the wrapper exists at all

Worth stating, because "it is just a pass-through, delete it" is a reasonable first
reaction. In release every `dt_pthread_mutex_*` **is** a one-line pass-through. What the
wrapper carries is:

- the `CAPABILITY`/`ACQUIRE`/`RELEASE` annotations that drive clang's `-Wthread-safety`
  (`cmake/compiler-warnings.cmake`). A bare `pthread_mutex_t` cannot carry those attributes:
  the wrapper struct is what makes lock discipline checkable at compile time at all.
- `dt_pthread_rwlock_t`'s same-thread recursive-writer tracking, which is a deadlock fix,
  not a diagnostic.

The `_DEBUG` arm — names, timings, contention tables — was deleted, because a second
implementation nothing builds verifies nothing. `caches/cache.c` carries the epitaph of the
previous attempt: *"the non-`_DEBUG` arm stopped compiling — and nobody found out."*

## Machine-checked lock discipline

`cmake/compiler-warnings.cmake` has asked for `-Wthread-safety` all along, and the mutex
wrapper has carried `CAPABILITY`/`ACQUIRE`/`RELEASE` for as long. It was checking nothing
whatsoever, for two reasons — and the first one is the embarrassing one:

**The flag never reached the compiler.** `CHECK_COMPILER_FLAG_AND_ENABLE_IT(-Wthread-safety)`
built the probe's result variable out of the flag text and passed it as `-D`:

    -DC_COMPILER_UNDERSTANDS_-Wthread-safety

which is not a valid macro name, so the probe compile failed, the flag was recorded as
unsupported, and it was dropped. Silently — a failed probe looks exactly like a compiler that
lacks the flag. Every flag containing a character illegal in an identifier went the same way.
Worse, clang does not resolve a thread-safety attribute's argument when the analysis is off,
so annotations that could not possibly work still compiled clean locally. Fixed by sanitising
the flag text into an identifier; the analysis then found real defects on its first run.

**And there was no data annotated:**

    GUARDED_BY across the tree:  0

`GUARDED_BY` is the annotation that does the work — where it is available. Clang's analysis
is **declarative**: you state that a field is guarded by a lock and it proves every access
holds it, as opposed to symbolic execution guessing at lock state from control flow. With no
data annotated it only verified that locks balance within a function, and never that any data
was protected.

**In C, `GUARDED_BY` is available only for file-scope locks.** Ours are per-instance —
`history_mutex` lives in `dt_develop_t`, one per develop context — and for a lock that is a
struct member the annotation cannot be satisfied by anything. In C++ a member's
`guarded_by(mu)` implicitly means `this->mu`, so an access to `obj.field` requires `obj.mu`
and a method's `REQUIRES(mu)` matches. C has no `this` and clang does not synthesise one: the
requirement stays the bare identifier. Measured on clang 19, two files, one variable each:

| Lock | Annotated function | Unannotated function |
| --- | --- | --- |
| file-scope | satisfied, no warning | warned — correct |
| struct member | **warned** | warned |

Clang states it outright — `requires holding rw 'm'`, unqualified, never `s->m`. Before
clang 19 it was worse: the attribute was rejected at parse time with "use of undeclared
identifier", which is how it reached master and broke the build for every distro clang.

So the declarative half is off the table here, and what remains is the contract half.

This also answers "can we do better than suppressing SonarCloud's pthread findings?".
`c:S5486` and friends are symbolic-execution rules whose own documentation says they *assume
non-recursive mutexes*; ours are recursive by design, so they cannot model this code and say
so themselves. Clang's analysis has no such assumption — it is not counting acquisitions,
it is checking a declared contract — and it runs on every LLVM build we already do.

### Done

`dt_pthread_rwlock_t` is a `CAPABILITY`, so the locks guarding the most concurrency-sensitive
state can be named at all. The functions that run with `history_mutex` held declare
`REQUIRES`/`REQUIRES_SHARED` — including the ones whose names already claimed it (`_ext`,
`_locked`) and enforced nothing. Clang then verifies the lock is held on every path into such
a function and released on every path out.

That is weaker than guarding the fields, and it still found real defects — which is the
argument for keeping it. `dev_history.c` went from **26 findings to 0**, and the fixes were
not all bookkeeping:

- `dt_dev_history_commit_item_now()` released the write lock and then walked `dev->history`
  twice with nothing held, while the background write job can be walking and dropping
  references on the same items. Reading `hist->forms` off an item that job is releasing is a
  use-after-free, not a stale read.
- `dt_dev_history_notify_change()` walked `dev->history` with no lock, from all eight of its
  call sites — one of which unlocks on the line immediately above the call.

Four contracts were also inverted: the function acquired the lock itself *and* demanded it
from callers. Those annotations described something the code did not do, which is worse than
no annotation, and they were caught the same way — by turning the analysis on and reading
what it said rather than trusting the labels.

The lesson for the rest of this backlog: an annotation is a claim to verify, not documentation
to add. Four of twenty here were wrong on the first pass.

### The backlog, measured

A scan of all **499** non-vendored translation units with `-Wthread-safety` reports **30
findings in 5 files**:

| File | Findings |
| --- | ---: |
| `caches/pixelpipe_cache.c` | 12 |
| `database/database.c` | 8 |
| `gui/lut_viewer.c` | 5 |
| `caches/cache.c` | 3 |
| `pixel/colorequal_shared.c` | 2 |

`src/tests/unittests/test_dtpthread_recursive.c` is deliberately **not** in that table. Its
four cases re-acquire a lock they already hold, which is the property under test and exactly
what the analysis is built to reject — it models a lock as held or not held, so the second
acquisition reads as "already held" and the matching release as "was not held". The bodies
carry `NO_THREAD_SAFETY_ANALYSIS`. Left unexempted they contributed 8 findings that nobody
could ever fix, which is precisely the noise that hides the ones that can be.

**They are not pre-existing.** Every one names an `rwlock`, and they exist because
`dt_pthread_rwlock_t` became a `CAPABILITY` in this same work — before that, clang had
nothing to check on those locks and reported nothing. An earlier draft of this document
claimed they predated the annotations and were merely invisible; that was wrong, and the
tell was in the messages all along.

They are also not warnings everywhere. Debug builds compile with `-Werror`, so under any
clang they are hard errors:

    error: rwlock 'cache_entry->lock' is not held on every path through here
           [-Werror,-Wthread-safety-analysis]

which is why the macOS and LLVM CI jobs failed while every GCC job passed — GCC ignores
`-Wthread-safety` entirely. A local GCC build cannot vouch for any of this.

Until they are resolved, the Debug flags in `CMakeLists.txt` carry
`-Wno-error=thread-safety-analysis`: the findings stay visible on every clang build but do
not break it. That flag is a ratchet to remove, not a setting to keep.

It is appended **only for clang**. GCC has no such warning, and `-Wno-error=` naming an option
it does not know is a hard error there, not a no-op — adding it unguarded broke every GCC
Debug build at once. It also has to be appended after `CMAKE_C_FLAGS`, since
`CMAKE_C_FLAGS_DEBUG` lands later on the command line and a `-Werror` after it would win.

All are conditional-locking shapes: *"not held on every path through here"*, *"expecting
rwlock to be held at start of each loop"*, *"releasing rwlock that was not held"*. Those are
**not automatically bugs** — they are patterns clang cannot prove — but each needs an
individual answer to "is this conditional locking actually correct, or does some path release
what it never took?", and they sit in the two subsystems least forgiving of a wrong answer.

Reproduce with the compile database, stripping the GCC-only flags clang rejects
(`-floop-nest-optimize`, `-ftree-loop-im`, `-fira-loop-pressure`,
`-fvariable-expansion-in-unroller`, `-flto*`) — leave them in and every compile fails before
analysis, which reports a very convincing zero.

### Next annotations, by value

- `dt_iop_module_t::params` — CLAUDE.md says it "belongs to the GUI thread and is NOT
  thread-safe; the pipeline thread must never read or write it". `GUARDED_BY` would make a
  violation a build error.
- pixelpipe cache entries under `cache->lock`.
- `dt_conf_t`'s tables under its mutex.
- `ACQUIRED_BEFORE`/`ACQUIRED_AFTER` for documented lock ordering — this file records
  "`xprofile_lock` OUTER, settings lock INNER" as prose; it is exactly a declarable order.
- ThreadSanitizer in CI for the dynamic half, which no static analysis can cover.
