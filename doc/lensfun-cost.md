# What lensfun actually costs, and what was done about it

Measured on lensfun 0.3.4, a stock Fedora database (`/usr/share/lensfun/version_1`, 4.5 MB,
57 files) plus the user's own updates (3.9 MB), against a microbenchmark linked straight to
`liblensfun` — no Ansel in the loop, so none of this is confounded by pipeline machinery.

| operation | cost |
|---|---|
| `new lfDatabase` | 0.02 ms |
| `lfDatabase::Load()` — parse the whole database | **89–102 ms**, +4 MB RSS |
| what that yields | 1051 cameras, 1562 lenses |
| `FindCamerasExt()` | **0.35–0.42 ms** |
| `FindLenses()`, hit | 0.055 ms |
| `FindLenses()`, miss | **0.84 ms** (a miss scans everything) |
| `new lfModifier` + `Initialize()` | 0.0007 ms — free |
| `ApplySubpixelGeometryDistortion()`, 6016×4016 | 278 ms single-threaded (87 Mpx/s) |

Three things follow, and only the first two are about *profiles*.

## 1. The database was parsed at startup, in every session

`init_global()` called `Load()`, so every launch paid 89–102 ms and 4 MB to parse XML —
including every lighttable-only session that never corrects a lens. Nothing can need it that
early: `reload_defaults()` sets `workflow_enabled` per image, so the first possible consumer
is an image being loaded.

**Fixed**: the database is built on first use, behind `_lensfun_db()` (`iop/lens.cc`). Every
consumer goes through that accessor; `init_global()` only creates OpenCL kernels now.
`db_tried` makes a failed load final, so a broken installation stays broken instead of
becoming slow. The construction lock is never held while the plugin mutex is taken, so the two
cannot deadlock.

Measured, headless startup, 5 interleaved runs of each — this on top of linking the IOP
modules in (see `static-iop.md`), which is the other half of the number:

| | master | + static IOP | + lazy lensfun |
|---|---|---|---|
| startup | 0.293–0.340 s | 0.206–0.211 s | **0.097–0.112 s** |

The 90-odd ms does not disappear, but it is not left on the critical path either:
`init_global()` starts `_lensfun_db_warm()` on a background thread, so the parse overlaps the
rest of startup and is normally done before any image asks. Whoever arrives first builds it
and the other waits on the same lock — there is one construction either way, and the accessor
was already thread-safe. The thread is **joined** in `cleanup_global()`, not detached: it must
not still be parsing when the database it is writing gets freed.

Measured with the pre-warm in place, 5 interleaved runs: startup 0.109–0.115 s against
master's 0.345–0.356 s — i.e. the background parse costs the startup path nothing.

## 2. The same lookup was repeated on every pipe resync

`commit_params()` → `_lens_build_data()` resolved the camera and the lens from the database on
**every resync, for every pipe**, taking the global plugin mutex to do it. That is 0.4–1.3 ms
of fuzzy string scanning to re-answer a question whose answer cannot change: the camera and
lens of an open image are fixed.

**Fixed**: `_lensfun_find_camera()` / `_lensfun_find_lens()` memoise the resolved pointers,
keyed on the strings that were searched for. Misses are cached too — a miss is the *expensive*
case to establish and it will not change. The results are pointers into the database, which is
built once and never reloaded, so they stay valid for the process's life; the memos are
destroyed before the database in `cleanup_global()`. Both are read under the plugin mutex the
lookups already took, so no new lock and no new lock order.

An uncontended hash lookup replaces 0.4 ms of scanning per resync. `reload_defaults()` keeps
the array-returning API — it walks the whole candidate list to pick a fixed-lens camera's
shortest model name — and runs once per image load, not per resync.

## 3. What is NOT a profile problem

The module's ~0.9 s in a full-resolution export is **not** database work. It is
`ApplySubpixelGeometryDistortion()` building the coordinate map — 278 ms of lensfun's own
polynomial maths per full frame, single-threaded, before any resampling — plus Ansel's
interpolation over the result. No amount of profile caching touches it. Reducing that means
caching the coordinate map across frames, or not asking lensfun for per-pixel maths, and both
are a different project.

## The `lens` slowdown under static linking is code placement, not code quality

Linking the modules into `lib_ansel` made `Lens correction` consistently slower: +10.7%
(p = 0.003, n = 13), reproduced across three separate sessions. It is the only module that
moved significantly. Chased to the end, because it looked like the one real cost of that
change:

* Disabling LTO for the `lens` target alone did **not** help (+11.7%). Not inlining in the
  module's own objects.
* Profiling put the cost in `dt_interpolation_compute_sample` — which lives in `lib_ansel` in
  *both* builds, and which lens leans on for resampling. Absolute cycles: 10.05 G → 10.82 G,
  +7.6%, matching the module delta.
* **Its disassembly is identical.** 883 instructions in both, same 15 `ymm`, same 50 FMA, byte
  for byte the same except addresses. Only its placement changed: `0x350de0` (32-byte aligned)
  in a 32 MB library, `0x725290` (16-byte aligned) in a 42 MB one.
* Whole-process counters confirm the front-end: the static build retires **4% fewer
  instructions** at **lower IPC**, takes **9.3% fewer µops from the DSB** (µop-cache coverage
  58.3% → 55.3%), leans more on legacy decode, and eats **11.8% more `icache_64b.iftag_stall`**.

So this is the alignment/µop-cache lottery, not a regression in anything anyone wrote. It
would reshuffle on a different link order, and total pipeline cycles moved +0.8% — i.e. not at
all. `-falign-functions=64`, PGO or BOLT are the levers if it is ever worth reclaiming; a
per-module workaround is not.
