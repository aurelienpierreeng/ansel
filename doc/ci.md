# What CI actually checks, and what it cannot

> **Established 2026-09-29 against `070f47fd3d`.** The numbers here were measured, not read
> off the workflow — several claims that *were* read off it turned out wrong, and those are
> recorded below rather than quietly corrected.

## The merge gate

`master` requires **one** status check: **`CI Gate`** (`.github/workflows/ci.yml`). It is a
summary job — it builds nothing itself and aggregates the three matrix legs.

Two things about its design are deliberate and easy to undo by accident.

**The `pull_request` trigger carries no `paths-ignore`, and must not regain one.** A workflow
skipped by path filtering does not *report* a check; it leaves it pending, and GitHub blocks a
merge on a required check that never reports. The trigger used to ignore `**.md`, so a
documentation-only pull request ran no job here — the moment anything in the file became
required, every documentation PR would have been permanently unmergeable. The filtering lives
in the `changes` job instead: the workflow always starts, the build legs still skip, and the
gate still reports.

**`CI Gate` is required rather than the matrix jobs themselves**, because the matrix job names
embed their matrix values — `Linux.noble.GNU14.skiptest.Debug.Ninja`. Requiring one of those
means the next edit to `os.code`, `compiler` or `btype` silently removes a required check and
the protection lapses with no error anywhere. That is the same failure mode as a ratchet whose
grep pattern stopped matching: the count goes quiet and reads as success.

The gate runs `if: always()`, which is what makes it report when the legs were skipped, or when
one failed and `fail-fast: true` cancelled its healthy siblings:

- a **skipped** leg is a **pass** — the documentation-only case `changes` exists to produce;
- a **cancelled** leg is a **failure**. Treating cancellation as neutral is exactly how a red
  matrix comes to read as mergeable.

## What does not build

`changes` decides. Only these skip the build legs: `po/**` (which has its own
`po-check.yml` running `msgfmt -c`), `data/latex/**`, `data/lua/**`, `data/pswp/**`,
`data/style/**`, `data/themes/**`, `data/watermarks/**`, and `**.md`.

Two paths that *used* to skip now build, because CI itself consumes them:

- **`packaging/**`** holds `install-deps-{ubuntu,macos,windows-msys2}.sh`, which the jobs in
  `ci.yml` execute, and a CMake subdirectory the build descends into
  (`CMakeLists.txt:846 add_subdirectory(packaging)`). A PR changing only
  `packaging/install-deps-windows-msys2.sh` once ran no matrix cell and merged.
- **`data/pixmaps/**`** holds `256x256/ansel.png`, the input image every "Check if it runs"
  smoke test exports.

The `push` trigger keeps its own `paths-ignore`, including `**.md` and `**.yml`. A push does
not gate a merge, so a doc commit on master still builds nothing — by design. **Do not report
CI status for documentation-only work; there is none.**

## The five gates do not all run on every build

`reorganisation.md` says "Five gates run in CI ... all five fail the build rather than warn."
The second half is true of every gate that runs. The first half is not, measured on
`070f47fd3d`:

| gate | when it runs |
|---|---|
| `check_layering.sh` | every build |
| `check_module_boundaries.sh` | every build |
| `check_statelessness.sh` | every build |
| `check_conditional_includes.sh` | **pull requests only** (`ci.yml`, `if: github.event_name == 'pull_request'`) |
| `check_unused_includes.sh` | **pull requests only, and only the `LLVM20` + `skiptest` cell** — 1 of 13 |

Both exceptions are defensible: they gate a *diff*, and a diff needs a base ref. Knowing which
is which is not, though — a push to master is checked by three gates, not five.

## The unit suite runs on the Linux and Windows Debug cells

*Linux established 2026-10-22 against `23b9511b02`; Windows 2026-10-27, on top of `b0f10525fd`.*

The step "Run unit tests" in `ci.yml` runs `ctest` on the six Linux Debug cells and the two
Windows Debug cells, the ones where the suite is built: `CMakeLists.txt` turns `BUILD_TESTING` on
by default in Debug when cmocka is found (Linux and Windows, not Apple), and
`install-deps-ubuntu.sh` and `install-deps-windows-msys2.sh` install it. The Release cells build
no tests. On GitHub, run 40409162489 (`b0f10525fd`): the Linux GNU14 `skiptest` cell passed 31 of
31 in 44 s, and both Windows Debug cells (GCC, UCRT64) linked all 31 test executables,
`test_filmicrgb`'s `--wrap` mocks included, without running them. Their first run crashed
`test_import_jobs` on every attempt: the Windows taskbar progress read the GUI that ansel-cli and
the tests do not have (`unit-tests-windows.md`). With that fixed, run 40442514809: both Windows
Debug cells pass 31 of 31.

- **`--no-tests=error`** is what makes a suite that was not built fail. Without it, ctest 3.28
  returns 0 on a build directory holding no test at all (measured: 0 without the option, 8 with
  it), and a lost `BUILD_TESTING` would read as a green step.
- **`xvfb-run`** gives the GTK tests a display on Linux. Without one, `test_tagging_completion`
  returns 77, which its `SKIP_RETURN_CODE` turns into a ctest "Skipped", not a failure. Xvfb has no
  window manager; the suite was measured under Xephyr, which has none either: 29 of 29 pass.
  Windows needs no display server: GTK draws through Win32.
- **`masks_geometry` compares no baseline in CI.** Its renders are checked against the baseline
  PNGs only when the sample bank `tests/image_test/samples` is cloned (`_baseline_check()` returns
  TRUE without it), and no workflow clones it. In CI the test runs its coverage oracle alone.
- **Cost**: the whole suite takes 90 s on the Debug build here, 60 s of it `masks_geometry` and
  24 s `test_drawlayer_batch_raster`. The thread count barely moves those two: together 83 s with
  `OMP_NUM_THREADS=4` and 86 s with 1, so the `nofeatures` cells, built without OpenMP, should
  cost about the same. On GitHub's Windows runners (run 40442514809) it takes 53 s in the
  `nofeatures` cell and 89 s in `skiptest`, of which `masks_geometry` 30 and 53 s,
  `test_drawlayer_batch_raster` 15 and 27 s.

**Windows was left out on 2026-10-22, for a reason that did not hold.** This file said
`masks_geometry` fails intermittently on Windows (`unit-tests-windows.md`). It does, but the
failures are differences against the baseline PNGs — the coverage oracle passed on the failing
runs — and CI compares no baseline (above), so that failure cannot occur in CI. The other reason
given, that the suite had been measured on Windows with clang only, was true: the GCC cells are
measured by running them.

Not run, and why:

- **macOS.** `CMakeLists.txt` does not default `BUILD_TESTING` on for Apple: ld64 has no `--wrap`,
  so the mock tests cannot link.
- **`.ci/ci-script.sh`'s own `ctest`**, in `target_build()`, is reached only through `TARGET=build`,
  which only `manualrun.yml` sets, on runner images GitHub has retired. It also reruns failed tests
  and takes the rerun's status, so a test that fails once and then passes would read as green.

## macOS: the Release cell builds through `build.sh`

*Established 2026-10-30, on top of `11fe9d529a`, then run on a Mac the same day on `db893eef9f`.*

The two Debug cells go through `.ci/ci-script.sh`. The Release cell runs
`build.sh --install --sudo --skip-lensfun`, so what someone types on a Mac is built, installed and
smoke-tested on each pull request, by the `/bin/bash` 3.2 and the BSD tools a Mac has. `--sudo` is
for `/usr/local/bin`, where `--install` links `ansel` and `ansel-cli`. `--skip-lensfun` keeps the
network out of the cell, and `lensfun-update-data` exits 1 when it finds nothing newer.

Where `build.sh` differs on macOS:

- **The compilers default to `cc` and `c++`**, as in both macOS workflows.
- **The deployment target is the running system**, unless `MACOSX_DEPLOYMENT_TARGET` is set, which
  CMake reads by itself.
- **`--install` leaves `/usr/share/applications` alone.** `/usr/share` is on the read-only system
  volume, for root too, and `set -e` would stop the script there, after the install.
- **`--build-package --install` makes the package**, as it does on Windows: the install, then
  `3_make_hb_ansel_package.sh`, `check_macos_bundle.sh` and `4_make_hb_ansel_dmg.sh`, in the
  nightly's order. Those read the install tree from `INSTALL_PREFIX`. They write beside it and the
  last one drives the Finder, so the tree defaults to `install/` in the sources rather than
  `/opt/ansel`, `--sudo` is refused before anything is built, and nothing is linked in
  `/usr/local/bin`. A relative `--prefix` is anchored on the current directory first: CMake would
  read it from the build directory, where `build.sh` runs it (measured with CMake 3.28), `3_` from
  its own, and `check_macos_bundle.sh` and `4_` from the caller's.

The CI does not run that last path through `build.sh`: the nightly calls the same three scripts
itself, with the Sentry upload between the build and the bundle.

Measured on Linux only, under bash 5.2, with `uname`, `sudo`, `cmake`, `ln` and the Apple tools
replaced by shims and `/usr/share` refusing writes: the script reaches its end where it stopped on
`/usr/share/applications` before, the failing bundle check leaves no DMG, and the real `3_` and
`4_` give the same 94-file tree with and without `INSTALL_PREFIX`. On real Linux, `--skip-config`
prints the same commands as before for five option sets.

Measured on a Mac: run 41135192327, on a `macos-15` arm64 runner with AppleClang 17. `build.sh`
built, installed and reached its end, past the `/usr/local/bin` links, and `ansel-cli` exported.
Still not run on a Mac: `--build-package` through `build.sh`, and `log()`, which that cell never
calls, so its BSD `sed` path is unmeasured.

## MSYS2: one stalled mirror fails the Windows dependency step

Measured 2026-10-15 against `22b2e62997`. Of the last 40 failed `win-nightly.yml` runs, three
failed in "Install Base Dependencies" (2026-09-19, 2026-09-24, 2026-10-15), all with the same
line: `failed retrieving file '<package>.pkg.tar.zst.sig' from ftp2.osuosl.org : Operation too
slow`. Always a `.sig`, always one mirror, and pacman never tried another.

That is pacman 6.1 (`lib/libalpm/dload.c`, read at tag `v6.1.0`): once a package arrives, its
`.sig` is queued as a new download of the package's *effective* URL plus `.sig`, with no server
list, so `curl_retry_next_server()` has nothing to retry with. MSYS2's mirrorlist starts with
`mirror.msys2.org`, a redirector that picks a mirror per request (six `HEAD` requests from
France got four different mirrors). The package came from whichever mirror the redirect chose;
when that mirror stalls on the signature, the transaction is lost.

Running the same `pacman -S` again recovers: the redirector is asked again, what already arrived
stays in the cache, and `find_dl_candidates()` (`sync.c`) fetches a package again together with
its `.sig` when the cache holds the one without the other. `packaging/install-deps-windows-msys2.sh`
therefore runs each `pacman` call through `pacman_retry`, three attempts ten seconds apart.
`ci.yml`, `win-nightly.yml` and `manualrun.yml` all go through that script. The `pacman -Syuu`
that `msys2/setup-msys2` runs itself has no such retry; none of those 40 runs stopped there.

## Known holes (open)

Found by audit on 2026-09-29 and **not fixed**. Each is the same shape: a check that reports
success without having checked.

- **Gates that exit 0 when they could not check.** `check_unused_includes.sh` sends
  clang-tidy's stderr to `/dev/null`, never inspects its exit status, and increments its
  "Checked N file(s)" counter regardless — so the count its own header tells you to read as
  proof of coverage counts failures as successes. `check_conditional_includes.sh` ignores
  `git diff`'s return code, so an unresolvable base ref reads as "no violations".
  `check_statelessness.sh` reads `objdump -t` on `.o` files, and under `-flto` GCC emits slim
  objects with no `.data`/`.bss` at all. `check_return_types.py` and `check_alloc_pairing.py`
  walk a relative `src` against the process CWD; `os.walk` on a nonexistent path yields nothing
  and raises nothing.
- **Two gates nothing runs.** `check_alloc_pairing.py` is invoked by no workflow and no CMake
  target. `check_list_order.py` gates its cross-boundary phase behind its first phase finding
  something.
- **`mac-nightly.yml`'s runtime smoke test is neutered** — both commands end in `|| true`, and
  every later step is gated on `success()`, which that guarantees.

**The rule these share**: a gate must distinguish "checked N things, all clean" from "checked
nothing". If it prints a count, the count must be of things actually examined; if it cannot
run, it must say so loudly rather than exit 0. See `doc/README.md` for the same lesson applied
to documentation claims.
