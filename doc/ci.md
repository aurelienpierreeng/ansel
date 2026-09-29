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

## Known holes (open)

Found by audit on 2026-09-29 and **not fixed**. Each is the same shape: a check that reports
success without having checked.

- **`.ci/ci-script.sh:82`** ends in a stray `{` where the `skiptest` case below it has `\`.
  `{` is a reserved word only in command position, so as an argument it is an ordinary word:
  `cmake` runs with `-DCMAKE_INSTALL_PREFIX=... {` and the continuation lines parse as separate
  commands. `-DBUILD_TESTING=ON` is in that block and never reaches cmake.
- **The unit suite never runs.** `ctest` exists (`.ci/ci-script.sh:58`) inside `target_build()`,
  called only from the `"build"` case, reachable only via `TARGET=build`, which appears only in
  `manualrun.yml`, which runs on retired runner images (`ubuntu-18.04`, `ubuntu-20.04`) with
  retired compilers. Six independent reasons; fixing any one alone still yields no test run.
  Tracked in issue #1484.
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
