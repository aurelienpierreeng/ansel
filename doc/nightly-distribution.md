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

Homebrew publishes no bottles for Intel macOS any more — `brew` says so outright, `You are
using macOS on Intel x86_64` — so `macos-15-intel` builds a large part of the tree from
source. Two formulae dominate it, measured on the nightlies of 2026-09-10 to 09-14 against
GitHub's 6-hour job ceiling:

| formula | size | build | whose |
|---|---|---|---|
| `llvm` | 1.7 GB | 3 h 43 | ours, for `TESTBUILD_OPENCL_PROGRAMS` |
| `llvm@22` | 1.6 GB | 4 h 04 – **over 4 h 54** | `librsvg`'s, through `rust` |
| `rust` | 438 MB | 1 h 05 – 1 h 29 | `librsvg`'s — it is written in Rust |
| everything else together | — | ~50 min | |

Four nightlies in a row were killed at the wall, on 09-10 with the dependency step alone at
5 h 52. The same step takes **1 min 17 s** on arm64, which pours 53 bottles and builds nothing.

**Both llvms were scheduled on every one of those runs, and no run ever got through more than
one** — `Would install 1 dependency for llvm: z3` for ours, and `llvm@22` inside
`Would install 12 dependencies for librsvg`. That is about eight hours of llvm against a
six-hour ceiling, and it is why reading the wall as one slow formula is wrong.

Two measures, and only together do they fit:

- **`install-deps-macos.sh` skips `llvm` on Intel**, and `mac-nightly.yml` passes
  `-DTESTBUILD_OPENCL_PROGRAMS=OFF` there to say so rather than lean on CMake's absent-LLVM
  fallback. All that option buys is test-compiling the OpenCL kernels at build time, which the
  arm64 CI does on every commit. Worth ~4 h.
- **The kegs `librsvg` pulls in are cached**, because they cannot be dropped: `llvm@22` and
  `rust`. Without the cache, those two plus the ~50 min of everything else still come to about
  6 h 23 — over the ceiling on their own. This is the load-bearing half.

So the Intel job caches those kegs. `tools/brew_cache_key.sh` owns the keys, and both
`mac-nightly.yml` and `mac-brew-cache.yml` call it — an entry saved by one must be the entry
the other looks for. Three things about that cache are not obvious:

- **A keg is cacheable without relocation** because a given architecture's prefix is identical
  on every runner (`/usr/local` on Intel, `/opt/homebrew` on arm64), so a restored keg sits at
  the paths it was built for. `brew` decides a formula is installed by reading
  `INSTALL_RECEIPT.json` inside the keg, and that file travels with it.
- **The key carries the version brew WOULD install**, never the installed one: `brew info
  --json=v2` reports both, and keying on the latter restores a keg a formula bump has already
  made useless — wasting the quota and hiding the bump. Each formula gets its own entry, so a
  bump in one does not discard the others.
- **`brew link --overwrite` after the restore, and never `--force`.** A restored keg carries
  its own `opt` link, which is all a keg-only formula needs, but `rust` also publishes `cargo`
  and `rustc` into the prefix's `bin`, and those symlinks live outside the cached paths.
  `link` refuses on a keg-only formula, which is the right answer there. `--force` would put
  `llvm`'s clang in the prefix's `bin` ahead of Xcode's, and the nightly compiles with `CC=cc`.

**The kegs are banked as soon as they exist, not at the end of the job.** `actions/cache`'s own
save is a post-step, and a job cancelled at the 6-hour ceiling never reaches it — visible in a
killed run's step list as `Post Restore the rust keg`, still pending. On 2026-09-10 the
dependency step *succeeded* at 5 h 52 and the budget then ran out during the build, so the four
hours just spent on llvm went with the job. `actions/cache/restore` and an explicit
`actions/cache/save` placed right after the dependency step mean a run that gets that far warms
the cache for the next one even when it dies afterwards.

**llvm@22's build time is not a constant, and that is the margin the warm-up lives on.** The
nightly of 2026-09-15 — the first with our own llvm dropped, so llvm@22 had the step almost to
itself — sat **4 h 54 inside a single `cmake --build .`** and was still there when the run was
cancelled, having completed only `openssl@3`, `xz`, `libssh2` and `pkgconf` (about 14 min) beside
it. `mac-brew-cache.yml` therefore runs at `timeout-minutes: 355`, nearly the whole ceiling, and
a dispatch that still runs out is retried rather than reasoned about.

**A cold cache still cannot warm itself from the nightly**, because the dependency step is what
does not fit: llvm@22 + rust + the rest come to about 6 h 31 against the 5 h 55 the job has left
after checkout, so the save is never reached either. That is what
`mac-brew-cache.yml` is for — it builds nothing but the formulae it is given, so each gets the
whole 6 hours. Warm in two dispatches, `llvm@22` then `rust`: their 5 h 41 together does not fit
under one timeout, and the second dispatch restores `llvm@22` from the cache instead of building
it again. Asking for both at once is the situation the workflow exists to escape.

**The cached llvm is now `llvm`, not `llvm@22`** (2026-10-02, against `a4eac9fe11`). homebrew-core
bumped `rust` to 1.99.0 on 2026-10-01, three days after "rust: do not hardcode LLVM version": it
now `depends_on "llvm"` (23.1.2). The nightly of 10-02 restored `llvm@22` from the cache and then
scheduled `llvm` beside it — `Installing librsvg dependency: llvm`, then `rust`, which died on
`/usr/local/opt/llvm not present or broken`. `llvm@22` left the tree; the keys, the cache steps
and `mac-brew-cache.yml`'s default now name `llvm`, and the skip in `install-deps-macos.sh` saves
nothing while rust shares it. **The cache needs warming again: `llvm`, then `rust`.**

**Homebrew builds with network access denied, so a download that failed the prefetch is lost.**
`brew install` fetches everything first (`Fetching downloads for: …`), then builds each formula
in a sandbox; homebrew-core added `deny_network_access!` to `llvm` on 2026-09-21. Anything not
fetched up front is retried inside the build, where DNS does not resolve. Measured on the nightly
of 10-02: exactly two downloads were attempted after the prefetch, and they were exactly the two
the prefetch had marked `✘` — both then failed on DNS (`Could not resolve host: github.com`,
`nodename nor servname provided`), while every prefetched one worked. The two causes:

- **netpbm is fetched with `svn`**, and `svn` did not exist yet at prefetch time (`You must: brew
  install svn`). It is in the tree because `libsoup` 3.8.0 has no Intel bottle, so its build
  dependency `vala` brings `graphviz` → `netpbm`: osm-gps-map's plan went from 23 dependencies on
  09-30 to 40 on 10-01. This alone killed 10-01. `install-deps-macos.sh` now installs
  `subversion` on its own first, on Intel.
- **`llvm` 23.1.2 carries a patch whose checksum no longer matches** (`Patch reports different
  checksum: f6dafd76…`, the `compare/1381ad49…40a8c7c0.diff` from GitHub — a generated diff, not
  a stable file). **The content did not change, only the abbreviation**: GitHub now writes the
  blob hashes on its `index` lines with 14 characters instead of 13. Measured 2026-10-02: the
  diff as served hashes to `fa40bc0a…`, and the same diff with those hashes truncated to 13
  characters hashes to exactly `f6dafd76…`. `?full_index=1` has no effect on a compare `.diff`,
  and the compare `.patch` form is a different file altogether (15-character hashes, other blobs).
  Homebrew closed the only report of the symptom (homebrew-core #314418, 2026-09-30) as an
  unsupported configuration, without the cause. `tools/brew_seed_patches.sh` works around it:
  it downloads each external patch outside the sandbox and, when the checksum differs, writes
  the shorter-abbreviation form at the patch's cached location — but only when it reproduces
  the formula's own sha256, which brew then verifies itself. `install-deps-macos.sh` (Intel)
  and `mac-brew-cache.yml` run it for `llvm` before installing. Tested 2026-10-02 against the
  real patch, outside brew. Its first run on a Mac (mac-brew-cache run 37020674433, 2026-10-02,
  `d4a2120092`) seeded nothing: brew refuses to load a formula from a file outside a tap
  (`Homebrew requires formulae to be in a tap, rejecting: …/llvm.rb`), so the script reported
  `could not list its patches` and the build failed on the same checksum. The source now goes
  into a throwaway local tap (`ansel/brew-seed`, removed on exit). Its run (37025480414,
  `9dea2adcc3`) got past that and was refused at the next guard: `Refusing to load formula
  ansel/brew-seed/llvm from untrusted tap`. The script now runs `brew trust` on the tap, and
  `brew untrust` on exit; that version has not yet run.

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

So the Intel measures below decide whether an Intel DMG exists, and no longer whether a macOS
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

Homebrew ships no bottles for Intel macOS any more, so `macos-15-intel` builds from source and
four nightlies in a row died at GitHub's 6-hour ceiling — the same dependency step takes 77 s on
arm64. **Two llvms were scheduled on every one of them**, four hours and more each — and not a stable
four: `llvm@22` sat 4 h 54 in one `cmake --build .` on 2026-09-15 without finishing. One is ours,
for `TESTBUILD_OPENCL_PROGRAMS`; the other is `llvm@22`, which `librsvg` pulls in through `rust`. So two
measures, and only together do they fit — `install-deps-macos.sh` skips ours on Intel (with
`-DTESTBUILD_OPENCL_PROGRAMS=OFF` to say so), and `librsvg`'s two, which cannot be dropped, are
cached. `tools/brew_cache_key.sh` owns the keys for both workflows that touch them, and
`mac-brew-cache.yml` exists because `actions/cache` saves only on success: a cold cache cannot
warm itself from the job the builds are killing. The traps — why a keg needs no relocation, why
the key must carry the version brew *would* install, and why `brew link` must never be given
`--force` — are in `doc/nightly-distribution.md`. That file also carries the other half: an
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
