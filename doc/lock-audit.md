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

### `plugin_threadsafe` — three unrelated concerns on one global lock

Named as though it serializes plugins. It has three consumers doing three different things:

| Consumer | What it guards | Third-party safety? |
| --- | --- | --- |
| `imageio/imageio_rawspeed.cc:96` | lazy init of the `CameraMetaData` singleton | no — a one-time init guard |
| `imageio/storage/disk.c:333` | the export filename **sequence counter** | no — an Ansel invariant |
| `iop/watermark.c:594` | *"rsvg (or some part of cairo…) isn't thread safe… when handling fonts"* | **yes** |

Consequence: an export computing a filename blocks a watermark render, and both block
camera-metadata initialisation. Only one of the three is a library-safety lock.

`global_mutexes.h` also still names `iop/lens.cc` as a consumer. That file no longer exists
(replaced by LensSerious) and lens takes no such lock. **Corrected in this pass.**

**Recommended follow-up:** split into three locks named for what they guard. Not done here
— it is a behaviour change to export sequencing and deserves its own change.

### The rawspeed singleton is a broken double-checked lock

```c
static CameraMetaData *meta = NULL;
if(IS_NULL_PTR(meta))            // unsynchronised read
{
  dt_pthread_mutex_lock(dt_plugin_threadsafe_mutex());
  if(IS_NULL_PTR(meta)) meta = new CameraMetaData(camfile);
  ...
```

`meta` is a plain pointer, not atomic. The outer read races the store; nothing orders the
publication of the pointer against the construction of the object. Benign in practice on
x86, undefined by the standard, and the kind of thing that changes behaviour under a new
compiler. **Recommended follow-up:** `dt_atomic` or a `pthread_once`.

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
