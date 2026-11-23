# Nightly distribution

> **First checked 2026-09-29.** This file was mechanically checked against `8f4638a04e` on 2026-09-29 — every `file:line` citation resolved, every backticked symbol looked up in the tree, every OPEN/planned status claim tested, and every gate or baseline number it quotes compared with `tools/check_module_boundaries.sh` and `tools/include_baseline.txt`. **No per-claim semantic read was done**: a citation that resolves can still describe the wrong thing, so this is a floor, not a verification. Nothing was found wrong by those checks.

How a nightly build gets from a GitHub runner to a user's machine, on every OS, and how
it gets updated once it is there. Decided 2026-08-29 (issue #1320); this is the manual.

## The shape of it

```
 lin-nightly ─┐                                  ┌─► ansel-website  data/nightly.json ─► download buttons
 mac-nightly ─┤   monthly pre-release            │                  ansel.photos/nightly.json ─► in-app check
 win-nightly ─┼─► nightly-YYYY-MM  ─► nightly-manifest ─┼─► homebrew-ansel Casks/ansel-nightly.rb
 flatpak     ─┤   (GitHub Releases)   (nightly.json)   └─► scoop-ansel   bucket/ansel-nightly.json
 docker      ─┘        │
                       └─► nightly-prune: delete months older than the window
```

Five things, each with one job:

| piece | where | job |
|---|---|---|
| **Monthly release** | `nightly-YYYY-MM` pre-release, one per month | the host. GitHub caps a *release* at 1000 assets; releases, total size and bandwidth are unlimited. Five formats a night filled the old rolling `v0.0.0` in under a year. Every nightly computes the tag at run time (`Compute the release tag` step) and `aurelienpierreeng/tip` creates it on first use. |
| **Manifest** | `.github/workflows/nightly-manifest.yml` → `tools/nightly_manifest.py` | the one file that says what "latest" means: newest asset per format, with url, size, sha256, version and full commit. Runs after each nightly finishes (`workflow_run`), pushes the file to the website and regenerates the cask and the Scoop manifest. |
| **Retention** | `.github/workflows/nightly-prune.yml` | first of the month, deletes `nightly-*` releases older than 12 months (tag included). Dry-run and window are workflow inputs. Never matches a real version tag. |
| **In-app check** | `src/common/updates.c` | nightly channel + GUI only, once a day, GET of the manifest, compare commit. Toast + *Help ▸ Update to the latest nightly build*. |
| **Package managers** | Flatpak repo on R2, Homebrew tap, Scoop bucket | `flatpak update`, `brew upgrade`, `scoop update` — the OS-native path. |

The per-OS workflows stay dumb: build, upload to the month's release, notify Matrix.
Each of them first asks the releases whether tonight's commit is already there, and builds
only what is missing — see *A commit already published is not built again* below.
Everything that knows about downstream lives in `nightly-manifest.yml`, so adding a
consumer is one `publish` line there and nothing in five workflows.

## nightly.json

```json
{
 "schema": 1, "generated": "2026-08-30T06:41:12Z", "channel": "nightly",
 "repo": "aurelienpierreeng/ansel", "download_page": "https://ansel.photos/en/download/",
 "formats": {
  "appimage":  { "name": "Ansel-0.0.0+4810.gabc…-x86_64.AppImage", "url": "…", "size": 103…, "sha256": "…",
                 "uploaded": "…", "release": "nightly-2026-08", "version": "0.0.0+4810.gabc…",
                 "commit_short": "abc…", "commit": "<full sha>" },
  "flatpak":   { … }, "dmg-arm64": { … }, "dmg-i386": { … }, "exe": { … },
  "docker":    { …, "image": "aurelienpierre/ansel", "pull": "aurelienpierre/ansel:0.0.0-4810.gabc…", "digest": "sha256:…" }
 }
}
```

Formats are recognised by filename shape in `tools/nightly_manifest.py` (`FORMATS`), and
the running application names its own format the same way in `dt_updates_runtime_format()`.
**Change one, change the other.** "Newest" is the most recently *uploaded* matching asset,
walking releases newest-first — not the first asset the API lists, which is not date order.

Docker: the image is pushed to Docker Hub as `:current` and `:<version with + as ->` (a tag
cannot contain `+`), *and* saved with `docker save | zstd` onto the month's release as
`Ansel-<version>-docker.tar.zst`, so a given night's image is retrievable months later and
without Docker Hub. The manifest entry is the release asset, enriched with the hub's pull
reference and digest when the hub has the same version. A release asset must be under
2 GiB; the workflow fails loudly if the archive is not, which is the cue to make the
Dockerfile multi-stage (it currently ships the whole build toolchain).

## What each format does when a newer build exists

| format | the user sees | in place? |
|---|---|---|
| AppImage | toast + Help menu → download. `LDAI_UPDATE_INFORMATION` is `gh-releases-zsync|…|latest-pre|…`, so `AppImageUpdate` / `appimageupdatetool` fetches a zsync delta from the newest pre-release | yes, with the external tool |
| Flatpak | `flatpak update` once the R2 repository is added | yes |
| dmg | toast + Help menu → download; or `brew upgrade` with the tap | download+drag (unsigned) |
| exe | toast + Help menu → download; or `scoop update` with the bucket | installer (unsigned) |
| Docker | `docker pull aurelienpierre/ansel:current` | yes |

## The in-app check, precisely

`dt_updates_init()` runs at the end of startup, after the privacy dialog. It returns
immediately unless all of: GUI, `DT_BUILD_CHANNEL == "nightly"`, `updates/enabled`, and
more than 24 h since `updates/last_check`. Then one thread GETs `updates/manifest_url`
(default `https://ansel.photos/nightly.json`, 5 s connect / 10 s total, 1 MiB cap), picks
`formats[<runtime format>]`, and compares its `commit` with `darktable_commit_hash`. The
nightly channel is monotonic, so *different* means *newer*. A hit posts a toast on the GUI
thread and arms `dt_updates_get_download_url()`, which *Help ▸ Update to the latest
nightly build* opens; with nothing armed the entry opens the download page, so it is
useful even before the check has run or when it is off.

What it sends: one GET with `User-Agent: Ansel/<version> (nightly)`. No identifier, no
body, nothing shared with the crash-report or analytics toggles. It is the third line of
the first-launch privacy dialog (default **on**, nightly builds only) and a toggle in
Preferences ▸ Storage ▸ Privacy. `-d control` logs every decision.

Self-builds and distribution packages never check: whoever built them updates them.

## The Intel mac build cannot afford its own dependencies

*Measured on the nightlies of 2026-09-10 to 2026-11-18. The prebuilt-Rust path was written on top
of `fa3c2765ab` on 2026-11-19, and measured on `macos-15-intel` the same day by a dispatch of the
branch carrying it (run 44535679929, on top of `abbc37447c`).*

Homebrew publishes no bottles for Intel macOS any more — `brew` says so outright, `You are
using macOS on Intel x86_64` — so `macos-15-intel` builds a large part of the tree from
source. Two formulae dominate it, against GitHub's 6-hour job ceiling:

| formula | build | whose |
|---|---|---|
| `llvm` | 3 h 43 (09-10) · 4 h 11 (11-17) · **over 4 h 12, unfinished** (11-18) | ours, for `TESTBUILD_OPENCL_PROGRAMS`; and `rust`'s, since rust 1.99.0 |
| `llvm`'s own dependencies (`cmake`, `z3`, `python@3.15`, `openssl@4`, …) | 1 h 02 (11-17) · 1 h 40 (11-18) | `llvm`'s |
| `rust` | 1 h 05 – 1 h 29 (September) · 1 h 41 (11-17), 3 h 02 with its own dependencies | `librsvg`'s — it is written in Rust |

The same dependency step takes **1 min 17 s** on arm64, which pours bottles and builds nothing.
Runners differ by half: on 11-18 the same `cmake` took 36 min against 21 the day before, `z3`
30 against 20.

Two measures keep both out of the Intel build:

- **`install-deps-macos.sh` skips our `llvm` on Intel**, and `mac-nightly.yml` passes
  `-DTESTBUILD_OPENCL_PROGRAMS=OFF` there to say so rather than lean on CMake's absent-LLVM
  fallback. All that option buys is test-compiling the OpenCL kernels at build time, which the
  arm64 CI does on every commit.
- **`librsvg` and `cargo-c` are built against the Rust project's prebuilt toolchain**, by
  `tools/brew_install_without_rust.sh`, before anything else is installed. With them installed,
  nothing in the tree schedules `rust`, so nothing schedules `llvm` or `z3` either.

### Why installing two formulae removes rust from every plan

Read in Homebrew's `FormulaInstaller#expand_dependencies_for_formula_once` on 2026-11-19: a
build dependency is **pruned, with everything below it, once the formula that needs it is
installed at its current version**. `adwaita-icon-theme` needs `librsvg`; once `librsvg` is
installed at the version homebrew-core wants, its build dependencies `rust` and `cargo-c` are
dropped from `adwaita-icon-theme`'s plan, and `rust`'s `llvm` with them. The nightly of
2026-10-30 shows what it replaces: `Installing dependencies for adwaita-icon-theme: …, z3,
cargo-c and librsvg`, `z3` alone 26 min, with the rust and llvm kegs already warm.

**A rust keg merely present does not do it**, though it looks like the obvious move. An
installed dependency is *skipped*, not pruned, and `Dependency.expand` still walks a skipped
dependency's own dependencies — `rust`'s include `llvm`, which would then be built.

So the script builds the two formulae themselves, as homebrew-core writes them:

- **The toolchain** is `rustc`, `cargo` and `rust-std` for `x86_64-apple-darwin` from
  `static.rust-lang.org`, at the version homebrew-core's `rust` formula is at, each checked
  against its published sha256 and installed with its own `install.sh`.
- **It lives under `$(brew --prefix)/var/ansel`, not under `$HOME`.** Homebrew builds in a
  sandbox that denies reading the home directory (`Sandbox#deny_read_home`); the prefix, the
  cache and the runner's workspace are readable. A formula's `fetch` (`cargo fetch`, with the
  network) and its `install` (offline) each run in such a sandbox. The same goes for the runner
  image's own rustup in `~/.cargo`, which is why it is not used.
- **The formula is homebrew-core's own source** (`brew cat`), with `depends_on "rust" => :build`
  and the `bottle` block removed and the toolchain prepended to `PATH` in `fetch` and `install` —
  needed because superenv rebuilds `PATH` from the dependencies' `opt/bin` alone. It goes into
  a local tap, `ansel/prebuilt-rust`, because Homebrew refuses to load a formula from a file
  outside a tap, and the tap is `brew trust`ed, because a local tap is refused as untrusted
  otherwise — both measured on the mac CI of 2026-10-02, with the same mechanism, in an earlier
  script. The keg has the name and version homebrew-core expects, which is what the pruning
  above looks at.
- **The tap stays.** The kegs record it as their origin. One consequence, from Homebrew's
  `install/check.rb`: an explicit `brew install librsvg` then refuses — *Formulae with the same
  name from different taps cannot be installed at the same time* — so `install-deps-macos.sh`
  takes `librsvg` off its list on Intel. A formula that merely depends on it is unaffected.
- **Two tripwires stop a 4-hour build before it starts.** The script refuses a source that no
  longer has exactly one `depends_on "rust" => :build`, and refuses to install a formula whose
  `brew install --dry-run` plan still names `rust` or `llvm`. `install-deps-macos.sh` reads the
  dry-run plan of the whole list too, and stops if `rust` or `llvm` is in it. A failure then
  takes minutes and says why, instead of running into the ceiling six hours later.

Its first run, on 2026-11-19 (run 44535679929), from the job log:

| step | time |
|---|---|
| the toolchain: three downloads, checksums, `install.sh` | 24 s |
| `cargo-c`, with `libssh2`, `llhttp`, `libgit2` | 13 min (its own build: 10) |
| `librsvg`, with 24 dependencies — `glib`, `cairo`, `pango`, … | 41 min (its own build: 9) |
| the whole dependency step | **3 h 48** |
| the whole job, DMG included | **4 h 08** |

`rust`, `llvm` and `z3` appear nowhere in the log — not installed, not fetched, not planned —
and neither tripwire fired. The dependency step took about as long as it did with both kegs warm
(3 h 41 on 10-29, 4 h 14 on 10-30); 77 formulae were built from source and 39 poured, the
longest builds being `boost` (25 min), `cmake` (23) and `x265` (16).

### What it replaces: a keg cache, which a bump could outrun

From 2026-09 to 2026-11-19 the Intel job instead restored `llvm` and `rust` kegs from
`actions/cache`, and a separate workflow, `mac-brew-cache.yml`, built a cold one, one formula per
run with nearly the whole 6 hours each. It failed on its own terms:

- **Every version bump was a cold key.** homebrew-core went from llvm `23.1.5_1` to `23.1.6_0`
  between 2026-11-17 and 18: two llvm builds in two days.
- **llvm no longer fitted in one run.** The warm-up of 2026-11-18 (run 44317497594) spent 1 h 40
  on llvm's own dependencies, which nothing cached, then was killed at its 355-minute timeout
  4 h 12 into llvm's `cmake --build`. On a good runner the day before, the same run took 5 h 11:
  45 minutes of margin.
- **The nightly still built llvm's dependencies.** With both kegs warm, `z3` (26 min),
  `cargo-c` (12 min) and their own were rebuilt every night, because `rust` still pulled in
  `llvm`, and an installed `llvm` still pulled in its dependencies.

Two lessons from that period are still true. **A job killed at the ceiling never runs
`actions/cache`'s post-step save**, so whatever it built is lost. And **Homebrew builds with the
network denied**, so a download that failed the up-front fetch fails again in the sandbox, on
DNS (`Could not resolve host: github.com`), which reads like an outage and is not one. Two such
downloads were measured on 2026-10-02:

- **netpbm is fetched with `svn`**, and `svn` did not exist yet at prefetch time (`You must: brew
  install svn`). It is in the tree because `libsoup` 3.8.0 has no Intel bottle, so its build
  dependency `vala` brings `graphviz` → `netpbm`. `install-deps-macos.sh` installs `subversion`
  on its own first, on Intel.
- **A GitHub compare diff changed checksum without changing content.** llvm 23.1.2 patched itself
  from `compare/1381ad49…40a8c7c0.diff`; GitHub had started writing the blob hashes on its
  `index` lines with 14 characters instead of 13, and truncating them back reproduced the
  formula's sha256 exactly. A compare diff is generated, not stored, so this can happen to any
  formula patching from one. The workaround, `tools/brew_seed_patches.sh`, went with the keg
  cache, since llvm is no longer built on Intel.

**OPEN — what grows now is everything else.** With rust and llvm gone, the Intel job still builds
every other formula from source, every night: 67 of them on 2026-10-30, a job of 4 h 40 with
the kegs warm, of which `z3` and `cargo-c` were the part this removes. Each homebrew-core bump
drops that formula's Intel bottle, so the share built from source only moves one way: 77 built
against 39 poured on 2026-11-19, a job of 4 h 08 — 1 h 52 under the ceiling, on one run.

**None of this is what a failing Intel job used to cost.** `upload_to_release` carried `needs:
MacOS` over the whole matrix with no `if:`, and `fail-fast` is off — so arm64 succeeded on
09-10 through 09-14 and all five of its DMGs were discarded with the Intel job, and no macOS
nightly shipped at all for five days, Apple Silicon included. It now runs under `always()` and
fetches each architecture's artifact separately, so one of them is published whatever became of
the other. `always()` and not the usually-preferred `!cancelled()`, because a job killed by the
ceiling is *reported* as cancelled and that is the case this has to publish through; the cost is
that a hand-cancelled run also publishes what it had. A check between the downloads and the
release refuses to publish nothing, since with both downloads allowed to fail an empty workspace
would otherwise reach `tip` as an empty file list and pass.

So the Intel measures above decide whether an Intel DMG exists, and no longer whether a macOS
nightly exists at all.

**Each package is published by its own step, and the release is then asked what arrived.**
`tip` ends on a bare loop — `for artifact in artifacts: gh_release.upload_asset(artifact)`, with
no `try`/`except` — so the first upload to raise kills the process and every file queued behind
it is never attempted. Handing it both packages at once let that order decide which architecture
was lost: on 2026-09-15 arm64 came first alphabetically, collided with itself, and the Intel
package behind it was never tried, though it was new and had nothing to collide with. One step
per architecture, each `continue-on-error`, then a check that reads the release's asset list and
fails by name on anything missing — after both have had their turn. Whether the run succeeded is
that check's answer, not the upload steps', because the release is the only thing that cannot be
wrong about what it holds.

**The packages are renamed to the spelling the release will use, before `tip` sees them.**
GitHub reduces an asset name to `[A-Za-z0-9._-]`, so the `~` a version string carries is filed
as `.`. `tip` decides what to *replace* by comparing its local filenames against the release's
assets (`asset.name == Path(artifact).name`), so a file still spelled with `~` never matches the
asset it is meant to replace: it is treated as new, GitHub normalises the name onto the one
already there, and the `422 already_exists` that comes back is not caught on the new-asset path.
Measured on 2026-09-15: the step died on the arm64 name and took a perfectly good Intel DMG with
it. The rename is also the one place that knows this spelling, so the Matrix notification uses
the names verbatim instead of converting them again.

## Homebrew links one libsoup at a time, and it must be osm-gps-map's

*Established 2026-11-13 against `6a9005ef9c`.*

`libsoup@2` and `libsoup` (3) install into the same Homebrew prefix, so only one is linked at a
time: installing or linking either one unlinks the other (`Unlinking
/opt/homebrew/Cellar/libsoup@2/2.74.3_1... 79 symlinks removed.`). `install-deps-macos.sh` used to
end with `brew link --force libomp libsoup@2`, unconditionally.

On 2026-11-13 the arm64 CI pulled `osm-gps-map` 1.2.2, whose `.pc` requires `libsoup-3.0`. The
final link put libsoup 2 back and left libsoup 3 unlinked, so `pkg-config` could no longer resolve
`osmgpsmap-1.0` (`Package 'libsoup-3.0', required by 'osmgpsmap-1.0', not found`, four times). The
map's include directories came back empty, and the build stopped in `generate_iop_static` on
`view.h:63: fatal error: 'osm-gps-map.h' file not found` — on PR #1635, whose diff touches only
`src/gui/dtgtk/preview_window.c`; the other two macOS jobs were then cancelled.

The same configure printed `Forcing libsoup3`, then `Found libsoup2 2.74.3`:
`FindLibSoup.cmake` fell back to libsoup 2 when the libsoup 3 it had just required was missing —
the very mix (libsoup 2 and 3 in one process) its header explains aborts at the first plugin load.

Fixed in three places:

- `install-deps-macos.sh` read `osmgpsmap-1.0.pc` and linked the libsoup it named. It now installs
  and links libsoup 3 only, see the next paragraph.
- `FindLibSoup.cmake` no longer falls back to libsoup 2 when libsoup 3 is required; when
  osm-gps-map is what requires it, configure stops with a message naming `brew link libsoup` and
  `-DUSE_MAP=OFF`.
- The probe of osm-gps-map's requirement now passes `--print-errors` and reads stderr. Measured with
  pkgconf 1.8.1: when the required libsoup is missing, `--print-requires` prints nothing and exits
  1, so the probe saw no constraint at all.

Tested 2026-11-13 by configuring `FindLibSoup.cmake` alone against fake `.pc` trees, five layouts:

| layout | result |
|---|---|
| map on libsoup 3, both installed | libsoup 3 |
| map on libsoup 3, only libsoup 2 | configure error (was: libsoup 2, silently) |
| map on libsoup 2, both installed | libsoup 2 |
| map on libsoup 2, only libsoup 3 | not found, HTTP server off (unchanged) |
| no map | libsoup 3 |

The Homebrew side is untested outside CI: there is no Mac here.

### Homebrew installs no libsoup 2 any more

*Seen in the arm64 CI on 2026-11-22 and 23, against `9f5530e1be`.*

Between those two days Homebrew turned a warning into a refusal. On 2026-11-22 the step warned
`libsoup@2 has been deprecated because it is not supported upstream! It was disabled on
2026-09-05.` and still poured `libsoup@2` 2.74.3_1. On 2026-11-23 the same line read `has been
disabled` and came as an error: Homebrew installed everything else, and `install-deps-macos.sh`
stopped on `Missing Homebrew packages after install: libsoup@2`. The other two macOS jobs were then
cancelled by the matrix, on a branch whose diff touches no build file.

Nothing needed libsoup 2 any more: Homebrew's `osm-gps-map` requires `libsoup-3.0` since 1.2.2, and
`FindLibSoup.cmake` follows osm-gps-map. The script now lists `libsoup` (3) instead, and unlinks a
`libsoup@2` an image may still carry before linking libsoup 3. The `opt/libsoup@2` entry of
`CMAKE_PREFIX_PATH` in `CMakeLists.txt` stays: it names a directory that is absent on a fresh
Homebrew, and still serves a local build whose osm-gps-map predates 1.2.2.

## A commit already published is not built again

*Measured 2026-09-22 and 2026-10-14; re-checked against `73ec46a31b` on 2026-10-14. The runs
quoted are on `master`.*

The cron fires whether master moved or not, so a night with no new commit rebuilt the commit of
the night before. `f484018567` was built four nights running (2026-09-18 to 21), the Intel half
at up to six hours each. What each nightly then did with it, measured on 2026-10-04, the second
night on `65e7a3adea`:

| workflow | built? | upload | Matrix |
|---|---|---|---|
| win-nightly (run 37196468832) | yes, 25 min | **failed**, `422 already_exists` | not sent |
| lin-nightly (run 37196715197) | yes, 20 min | **failed**, same | not sent |
| flatpak-nightly (run 37200713054) | yes, 18 min | **failed**, same | not sent |
| mac-nightly (run 37196608646) | yes, arm64 14 min, **Intel 2 h 27** | **replaced** both DMGs | sent |
| docker-image (run 37196674752) | yes, 25 min | **replaced** the image on Docker Hub and the archive | it sends none |

The three failures are the `~` of the local filename (see the rename above): `tip` takes the
asset for a new one and GitHub refuses the duplicate. macOS had lost that accident when its
packages were renamed to the release's spelling, so it replaced identical DMGs, stayed green and
announced a package nobody had changed. The archive Docker attaches is already spelled with `.`.

So every nightly starts with a `published` job on `ubuntu-latest` that asks
`tools/nightly_published.sh`, one script for all five so that they agree on what "already
published" means:

- **It matches the commit hash in the asset names, never the rebuilt version string.** Both
  halves of `0.0.0+5241.g7230d1b355` depend on the clone rather than on the commit: the count on
  the depth of the history, and the abbreviation on the number of objects in it. Measured
  2026-09-22: the same commit abbreviates to 9 digits in a blobless clone and to 10 in the
  build's. Measured 2026-10-14: a 5000-commit fetch made the way `actions/checkout` makes it, at
  `7230d1b355`, answers `0.0.0+5241~g7230d1b355` (297 665 objects in the pack) — right today,
  but only as long as nobody changes a depth. Matching the hash needs neither, so the job checks
  out at depth 1.
- **It looks in every `nightly-*` release**, not only this month's: a commit built on the 30th is
  not new on the 1st, and the manifest walks the releases newest-first, so it still finds that
  package.
- The templates are `ansel-@VERSION@-win64.exe`, `Ansel-@VERSION@-x86_64.AppImage` with its
  `.zsync`, `Ansel-@VERSION@-x86_64.flatpak`, `Ansel-@VERSION@-docker.tar.zst`, and
  `Ansel-@VERSION@-<arch>.dmg` with `<arch>` what `$(arch)` prints — `arm64`, or `i386` on Intel.
- **What else a workflow publishes is not asked about**, because it goes out earlier in the same
  job and a failure there stops the job before the release asset is uploaded: the Flatpak OSTree
  repository on R2 (when R2 is configured), and the image on Docker Hub.
- **The build checks out the sha the `published` job judged**, not the branch a second time, so
  a push landing between the two jobs cannot have one commit checked and another built.
- **A release list that cannot be read counts as "not published"**, with a warning: the nightly
  then builds as it did before it asked, since a wasted build costs less than a night without a
  package.
- **A manual dispatch skips the question and builds everything.** Windows, Linux and Flatpak
  rename `~` to `.` before `tip`, as macOS does, so a dispatched rebuild of a published commit
  replaces its package instead of failing on `422`.
- **Only master publishes** (macOS). A dispatch on another branch builds and tests both
  architectures and keeps the DMGs as the run's artifacts; `upload_to_release` and the Sentry
  symbol upload are skipped, so testing a change to the mac build never replaces a nightly.

Windows, Linux, Flatpak and Docker have one package each: `published` outputs `build=true|false`,
and a skipped build skips what publishes with it — green run, nothing published, nothing
announced, nothing pushed to Docker Hub.

**macOS decides per architecture**, because the two fail independently: on 2026-10-06 and
2026-10-08 only arm64 shipped, and on those commits the next night builds Intel alone. The
`published` job therefore owns the matrix, defined there as JSON, and hands the build only the
missing architectures. When nothing is left, the build job is skipped — an empty matrix is an
error to GitHub, not a no-op — and so is publishing, which would otherwise fail on finding no
DMG. The Matrix message names only the packages the release check found there, and is sent
whenever there is at least one: a package that arrived is announced even when the other did not,
while the job still fails for the missing one.

GitHub can also not start a scheduled nightly at all: from 2026-10-09 to 10-14 it started none —
Windows, Linux, Docker and macOS alike have no run on those days.

## GitHub starts a scheduled run hours after its cron

*Measured 2026-11-17 on top of `bbdfcc53ac`, from the `created_at` of every `schedule` run the
Actions API lists (up to 100 per workflow) against the time its cron names. `created_at` is when
GitHub queued the run, before any job, so the whole delay is GitHub's.*

Median delay, in hours, of the crons as they stood then — all on the hour:

| workflow (cron, UTC) | Jul | Aug | Sep | Oct | Nov |
|---|---|---|---|---|---|
| Nightly Linux PKG (05:00) | 2.6 | 1.1 | 4.7 | 5.6 | 7.8 |
| Build and Push Docker Image (05:00) | 2.6 | 1.1 | 4.7 | 5.6 | 7.9 |
| Docs building (Sunday 00:00) | 1.4 | 0.7 | 1.9 | 4.1 | 5.6 |
| `LensSerious` Lens database (03:00) | – | 6.4 | 5.3 | 6.3 | 7.5 |

Every cron drifts the same way whatever its hour, and `LensSerious` — another repository of the
organisation, **not** a fork — drifts with them, so the delay is neither the workflow files nor
`ansel` being a fork. `ansel-website`, on a 4-hour cron, ran about 3.4 times a day in November
instead of 6: a run that arrives as late as the next slot seems to take its place. Whether
GitHub delays its whole platform this much or only this organisation (free plan) was not
measured; it would take a repository of another owner.

GitHub documents that `schedule` events are delayed under load, and the start of an hour is the
busiest slot. Every cron therefore sits on an odd minute (`7 5`, `11 5`, `17 5`, `29 5`, `17 6`,
`41 0`, `43 4`), keeping its hour and the order between them. Whether this buys back hours rather
than minutes is **not yet measured** — re-run the table above a few weeks after the change. A
build that must start at a fixed time can only come from outside: a `workflow_dispatch` through
the API is queued at once, like a push.

**Re-enabling a fork does not restore its crons.** When GitHub disabled Actions on this fork
(2026-11-06, about 14:07 UTC) and the banner of the Actions tab re-enabled them (first run
2026-11-09 00:46 UTC), push and pull-request runs resumed, but no `schedule` run was created for
any of the seven workflows that have a cron: the last nightly stayed `3ce1ec6716` of 11-06, and
`docs.yml`, untouched, missed Sunday 11-15. The API reported every workflow `active` meanwhile.
On 2026-11-17 each of them was disabled and enabled again (`gh workflow disable` / `enable`);
whether that alone restored the crons is the next night's run to read:
`gh api "repos/aurelienpierreeng/ansel/actions/runs?event=schedule&per_page=5"`.

## Secrets and settings to create

All on the `ansel` repository. Every one is optional in the sense that its step is
skipped, with a warning, when it is absent — nothing fails a nightly for want of a secret.

| name | kind | used by | what it is |
|---|---|---|---|
| `NIGHTLY_PUBLISH_TOKEN` | secret | nightly-manifest | fine-grained PAT, **Contents: read & write** on `ansel-website`, `homebrew-ansel`, `scoop-ansel`. Nothing else. |
| `R2_ACCOUNT_ID` `R2_ACCESS_KEY_ID` `R2_SECRET_ACCESS_KEY` `R2_BUCKET` | secrets | flatpak-nightly | an R2 bucket (public, custom domain) and an API token scoped to it. Free tier: 10 GB, 1 M writes and 10 M reads a month, **egress free**. A pruned repo is a few hundred MB. |
| `FLATPAK_REPO_URL` | variable | flatpak-nightly | the public URL of that bucket, default `https://flatpak.ansel.photos` |
| `NIGHTLY_STATS_TOKEN` | secret | nightly-manifest | fine-grained PAT with **Administration: read** on `aurelienpierreeng/ansel` and nothing else — the one permission GitHub lists for the traffic endpoints (repository clones per day). The workflow's built-in `GITHUB_TOKEN` cannot read them: the first production run produced a series with no `traffic` block. Without this secret the download statistics are complete except for clones. |
| `FLATPAK_GPG_KEY` `FLATPAK_GPG_KEY_ID` | secrets | flatpak-nightly | an ASCII-armoured private key made for this and its id. Signs the repo and the bundle, and is embedded (public half) in `ansel.flatpakrepo`, so clients add the remote without `--no-gpg-verify`. `gpg --quick-gen-key "Ansel nightly <nightly@ansel.photos>" ed25519 sign never` then `gpg --export-secret-keys --armor <id>`. |

Docker Hub keeps its existing `DOCKERHUB_USERNAME` / `DOCKERHUB_TOKEN`.

Users then run, once:

```sh
flatpak remote-add --if-not-exists ansel https://flatpak.ansel.photos/ansel.flatpakrepo
flatpak install ansel photos.ansel.Ansel
```

## GHCR, for later

`ghcr.io/aurelienpierreeng/ansel` would need no separate credentials — `GITHUB_TOKEN`
with `packages: write` pushes to it — and ties the image to the repository's own
visibility and retention. The change is three lines in `docker-image.yml`: log in to
`ghcr.io` with `${{ github.actor }}` / `${{ secrets.GITHUB_TOKEN }}`, add a
`ghcr.io/aurelienpierreeng/ansel:<tag>` line under `tags:`, and grant
`packages: write` under `permissions:`. `tools/nightly_manifest.py` reads Docker Hub's
public tags API for the pull reference; GHCR's equivalent needs a token, so the manifest
would carry the release asset only, or the workflow would write the pull reference into
a small file on the release. Decision deferred: Docker Hub stays for now.

## Stores that need a real release

**winget** and **Chocolatey** want a versioned manifest with a pinned hash per version,
submitted through a moderated PR. That is a stable release, not a nightly; when there is a
tag, `wingetcreate` generates the manifest from the exe URL in one command. **Flathub** is
the same story and is written up in `packaging/flatpak/README.md`.

## Signing: procedure and cost

Both nightly installers are unsigned. Nothing above requires signing, but three things stop
short of "updates like any other app" without it, and users see a warning on every first
launch.

### macOS — Apple Developer ID + notarization

| step | what | cost |
|---|---|---|
| 1 | Apple Developer Program membership (individual or organisation) | **US$99 / year** |
| 2 | In the developer account, create a **Developer ID Application** certificate; export it with its private key as a `.p12` | — |
| 3 | Store the `.p12` (base64) and its password as secrets; in `mac-nightly.yml`, import it into a temporary keychain on the runner (`security create-keychain` / `security import`) | — |
| 4 | `packaging/macosx/3_make_hb_ansel_package.sh` and `4_make_hb_ansel_dmg.sh` already call `codesign … -s "${CODECERT}"` when `CODECERT` is set — set it to the certificate's identity | — |
| 5 | Notarize the dmg: `xcrun notarytool submit Ansel-*.dmg --apple-id … --team-id … --password <app-specific password> --wait`, then `xcrun stapler staple Ansel-*.dmg` | — |

After that: no Gatekeeper prompt, the cask needs no quarantine note, and **Sparkle** (the
standard in-app updater on macOS, EdDSA-signed appcast) becomes possible — it is what
would turn the toast into a one-click in-place update on macOS.

### Windows — Azure Trusted Signing (or a code-signing certificate)

| option | what | cost |
|---|---|---|
| **Azure Trusted Signing** | Microsoft-managed certificate, identity validation once, signing via `signtool` with the Trusted Signing dlib in CI. Public-trust certs, SmartScreen reputation accrues to the identity | **~US$10 / month** (Basic) |
| OV code-signing certificate | from a CA, on an HSM token since 2023 (so CI signing needs a cloud HSM or a self-hosted runner with the token) | ~US$200–400 / year + the HSM |
| EV certificate | immediate SmartScreen reputation | ~US$300–500 / year |

With any of them: `signtool sign /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 …`
on `ansel-*.exe` in `win-nightly.yml` before upload. CPack's NSIS generator has
`CPACK_NSIS_EXECUTABLE_SIGN` hooks for signing the uninstaller too.

### Linux

Nothing to buy. The Flatpak repo is GPG-signed by the key above; the AppImage can carry a
GPG signature (`linuxdeploy-plugin-appimage` honours `SIGN=1` with a key in the runner)
which `AppImageUpdate` verifies. Neither gates anything today.

### Recommendation

Windows first — Trusted Signing is cheap and SmartScreen is the warning most users hit.
macOS is the larger cost and the larger payoff (Sparkle). Budget both together at about
**US$220 / year**.

---

## Rules carried over from CLAUDE.md

*Found `c6039f4078`, 2026-09-14. Verified against `42eca0e8fe`, 2026-09-29.*

The Sentry **environment** is `<build channel>-<platform>` (`nightly-windows`,
`self-build-linux`, `package-fedora-macos`): the sessions API groups crash-free rates by
release and environment and by nothing else, so this is how the website shows a build's crash
rate per package. Split on the LAST hyphen — a channel can contain hyphens. Sessions from before
this carry the bare channel; readers must accept both.

Nightlies land in one `nightly-YYYY-MM` pre-release per month (GitHub caps a *release* at
1000 assets; five formats a night filled the old rolling `v0.0.0` in under a year).
`tools/nightly_manifest.py` writes `nightly.json` — the newest build per format — after
every nightly, and that file drives the website buttons, the in-app check
(`src/common/updates.c`), the Homebrew cask and the Scoop manifest. The format keys in the
script's `FORMATS` and in `dt_updates_runtime_format()` must agree. `doc/nightly-distribution.md`
is the manual: secrets, retention, R2, signing costs, GHCR for later.

Homebrew ships no bottles for Intel macOS any more, so `macos-15-intel` builds from source, and
the same dependency step that takes 77 s on arm64 ran into GitHub's 6-hour ceiling there. Two
formulae made the difference, `llvm` (over four hours) and `rust` (up to two), both rebuilt at
every homebrew-core bump. `install-deps-macos.sh` skips our `llvm` on Intel (with
`-DTESTBUILD_OPENCL_PROGRAMS=OFF` to say so), and builds `librsvg` and `cargo-c` — the only
formulae needing `rust` — against the Rust project's prebuilt toolchain, through
`tools/brew_install_without_rust.sh`; once those two are installed, Homebrew prunes `rust`, and
with it `llvm`, from every other plan. Why a `rust` keg merely present would not do it, why the
toolchain may not live under `$HOME`, and why `brew install librsvg` must then not be called on
Intel are in `doc/nightly-distribution.md`. That file also carries the other half: an
Intel failure used to discard the arm64 DMG too, `upload_to_release` having `needs: MacOS` over
the whole matrix with no `if:`, which cost five good Apple Silicon packages in five nights. It
publishes per architecture now, so the Intel measures decide whether an Intel DMG exists and no
longer whether a macOS nightly exists at all.
### Only the macOS bundle is assembled by hand, and it may not name a binary

Windows packages with CPack (`--target package`, the `DTApplication` component whole) and
Linux with `make install`; neither enumerates anything, so a command gated on a build option
ships wherever it was built. macOS is the exception: `packaging/macosx/3_make_hb_ansel_package.sh`
copies an install tree into the `.app` itself.

It used to do that from a hardcoded list — two of them, in fact, and both were wrong.
`ansel-lens-db-update` (needs liblensfun) and `ansel-nn-parity` (needs OpenCL) were built,
installed into `bin/`, and absent from `/Applications/Ansel.app/Contents/MacOS/`, while the
Windows and Linux packages of the same commit carried them. The second list, `dtExecutables`,
drives `install_dependencies()` and `reset_exec_path()`, so **fixing only the copy ships a
binary that cannot start**: `ansel-lens-db-update` is the one binary linking liblensfun, and
that list is what pulls the dylib into the bundle at all. It also named three binaries that no
longer exist, and the `libexec/ansel/tools` test beside it asked about the script's own
directory rather than the install tree, so it was never true.

The copy is now `bin/*` and the roster is read back from the bundle. `tools/check_macos_bundle.sh`
holds it, run from the nightly between the package and the DMG: every executable in `bin/` is
in the `.app`, and every load command of every bundled binary and dylib resolves inside the
bundle or to a system library. It is static on purpose — running each command would rebuild a
database (`ansel-lens-db-update`) or write thumbnails (`ansel-generate-cache`), and
`ansel --version` exercises no library resolution the loader would report.

It exits **2 for "nothing was checked"** — not Darwin, no `otool`, no bundle — which is the same
convention `check_it_runs.sh` uses and is not a pass. Only the macOS nightly calls it, and there
it never hits that branch; adding it to a Linux gate run would fail the build on exit 2. The
homebrew prefix it looks for is `/opt/homebrew` **and** `/usr/local` whatever `brew --prefix`
says, since guessing one architecture's prefix turns the load-command half into a silent pass.

`packaging/macosx/ansel.bundle` is a *second* macOS path — the MacPorts/gtk-mac-bundler route
of `BUILD.txt`, used by no CI — and it is a manifest, so it does enumerate. It had the same
omission. `src/apps/README.md` is the roster of what exists and what gates each one.
