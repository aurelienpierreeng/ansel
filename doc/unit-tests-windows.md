# The unit suite on Windows

> **Established 2026-10-03 against `7e5db63669`** (plus the uncommitted changes that made the
> suite build), on MSYS2 UCRT64: clang++ 22.1.8 with **libstdc++**, Ninja, Debug, in a separate
> build tree. Every number below was measured on that run.

Until this date the suite had never been built on Windows: `BUILD_TESTING` defaulted to ON in
Debug on Linux only (`fa2b44a500`), and nothing ran `ctest` in CI anyway (see `ci.md`). The
first build turned up the problems below, in this order. Result after the fixes: **24 of 25
ctest entries pass** reproducibly; `masks_geometry` is intermittent (last section).

## What it took to build

- **`-municode` reaches every test.** `ansel_deps` carries it as an INTERFACE link option and
  `lib_ansel` links `ansel_deps` PUBLIC, so any executable linking `lib_ansel` gets the wide
  CRT startup, which calls `wmain()`. A test with only `main()` fails to link (`undefined
  symbol: wWinMain` under lld). Every test therefore includes `win/main_wrapper.h` under
  `_WIN32`, and declares `int main(int argc, char *argv[])` — the wrapper's prototype, so
  `main(void)` is a conflicting declaration. Two tests had locals called `argc`/`argv`.
  (The old CMake comment said the wrapper was `wWinMain`; it is `wmain`, see
  `src/CMakeLists.txt` near `-mwindows`.)
- **`<CL/cl.h>` not found.** `tests/CMakeLists.txt` defines `HAVE_OPENCL` for the tests but
  had no path to the bundled headers `src/` adds for itself. Linux hid it with system headers.
- **`libansel.dll` copied by 23 targets at once.** Each test had a POST_BUILD copy of the DLL
  into the same directory; under `ninja` they race and Windows answers `Error copying file
  ... Permission denied` to the loser — a random 1 to 3 links failed per build, and passed on
  re-run. Now one custom target copies it and every test depends on it.

## What it took to pass

- **`std::call_once` deadlocks when the initialiser throws** (GCC PR 66146). On targets
  without futex — MinGW among them — libstdc++ builds `call_once` on `pthread_once`, and an
  exception leaves the once "in progress": the next caller waits forever. Found as a 300 s
  timeout in `test_image_cache_flags_writeback`, whose datadir has no `cameras.xml`; gdb on
  the hung process showed the main thread in `std::call_once<dt_rawspeed_load_meta()::$_0>`.
  This was a **production** bug, not a test one: `src/imageio/imageio_rawspeed.cc` now uses a
  plain mutex, which keeps the retry-after-throw semantics the comment there wanted.
  **Do not reintroduce `std::call_once` with a throwing initialiser.**
- **Paths join with `G_DIR_SEPARATOR_S`.** The image repository builds full paths in SQL as
  `folder || G_DIR_SEPARATOR_S || filename`, so `/testdb/paths` + `a.raw` is
  `/testdb/paths\a.raw` on Windows. An expectation built with `g_build_filename()` does NOT
  match: GLib reuses the separator already present in the first element (`/`).
- **`_dup2()` returns 0 on success**, not the target descriptor as POSIX `dup2()` does.
- **The LensSerious submodule's tests** (`knots`, `vendor`, `parity`, `db_*`) were listed in
  Ansel's ctest as "Not Run" on every platform: the submodule is added `EXCLUDE_FROM_ALL`, so
  their executables are never built. `src/CMakeLists.txt` now sets `LENSSERIOUS_TESTS OFF`.

## `check_statelessness.sh` on Windows

`tools/statelessness_audit.py` only knew ELF: it looked for `.o` files and parsed
`objdump -t`'s ELF layout. CMake writes `.obj` on Windows, and those are COFF, where
`objdump -t` gives each symbol a section NUMBER (`(sec  3)`) instead of a name. The audit now
picks the suffix by platform and reads both layouts, mapping section numbers to names
through the one symbol objdump emits per section; `.rdata` counts as read-only, `.tls` as
state, and the `__imp_` prefix of a call into another DLL is dropped. Accepting `.obj` alone
would have been worse than nothing: the ELF pattern matches no COFF line, so every object
would have read as symbol-free and the gate would have passed without checking.

Measured on the Debug test tree: 571 translation units read, 288 holding state of their own.
**First finding (resolved the same day):** `src/system/resource_limits.c` reached state through
`src/win/rlimit.c` (`static BOOL rInitialized`, `static rlimit_t rlimits[]`) — Windows-only
code, so the Linux CI never saw it. That file emulated `getrlimit`/`setrlimit` over a table,
and had no effect: its `getrlimit(RLIMIT_STACK)` answered `RLIM_INFINITY`, so
`dt_set_rlimits_stack()` never called `setrlimit`, and even that only wrote the table — Windows
fixes the main thread's stack at link time and no call changes it afterwards. Its other
exports (`rfwrite`, `_rwrite`) had no caller (grep). `win/rlimit.{c,h}` are deleted and
`dt_set_rlimits()` is empty on Windows.

## `masks_geometry` (OPEN)

Two separate things.

**Baseline drift, reproducible.** The committed baselines were rendered on Linux. On UCRT64,
nine renders differ from them on every run: at worst 25 per channel
(`brush-1313-cusp-5184x3888-overlay`, 369 px) and at most 0.0243 % of the pixels
(`polygon-comb-overlay`), including one alpha mask (`brush-1074-flare`), so not cairo alone.
All coverage checks pass on the same run. `tests/masks/masks_geometry.c` has Windows bounds
(delta 32, share 0.0004) for this.

**Large differences, intermittent — not explained.** Twice in about 45 runs, one case (a
different one each time) came out with 1 to 7 % of its pixels at a delta of 255
(`brush-1313-cusp-5198x3904`, `ellipse-rotated-overlay`); 26 runs in a row then passed.
The coverage oracle passed on the failing runs: it judges the mask from one
`dt_masks_debug_rasterise()` call, while each PNG comes from another rasterisation inside
`dt_masks_debug_write_png()`, so it is a later rasterisation that differs. The output
buffer is zeroed (`dt_calloc_align_float`). Both failures happened while the machine was
busy (a `ninja` had just finished; other files in the tree were changing), which points at
a thread race in `dt_masks_get_mask_roi()` — **unconfirmed**: no failing image was captured.
Next step: loop the test under load or with varied `OMP_NUM_THREADS` and keep the PNGs of
the first failure.
