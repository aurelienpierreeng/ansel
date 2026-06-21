# Crash reporting with Sentry {#sentry}

[TOC]

## What is Sentry, in one paragraph

[Sentry](https://sentry.io) is a hosted *error and crash monitoring* service. An application
links a small client library; when the application crashes (or wants to report an event), the
client packages up a description of what happened — a stack trace, the OS and app version, some
context — and uploads it to a Sentry *project* over HTTPS. Maintainers then browse those reports
on a web dashboard instead of waiting for users to file (usually incomplete) bug reports. Sentry
also aggregates *sessions* to tell you how often the app runs without crashing ("release health").

Ansel uses Sentry for exactly two things:

1. **Crash reports** — when Ansel segfaults, we get the backtrace automatically.
2. **Release health / basic telemetry** — how long sessions last, how many end cleanly vs. crash,
   and the kind of machine Ansel runs on (OS, CPU/RAM/GPU, display server, screen/window size).

It is **opt-in**: nothing is sent unless the user agrees on first launch, and it can be turned off
at any time. No images, file names, or personal data are ever sent.

> **Note** — Sentry is only one of Ansel's two optional data flows. Anonymous *usage analytics*
> (which features are used, on which platforms) go to a separate service, PostHog; see
> [Usage analytics (telemetry)](@ref telemetry). The two share a single first-launch consent dialog
> (one checkbox each) but are otherwise independent and separately toggleable. The user-facing
> explanation of everything collected lives in the
> [Data privacy](https://ansel.photos/en/data-privacy/) page of the user manual.

Project dashboard: <https://aurelienpierreeng.sentry.io/projects/ansel/>
Crash-free monitor: <https://aurelienpierreeng.sentry.io/monitors/1388118/?project=4511598693253200&statsPeriod=24h>

## Vocabulary you will meet

- **DSN** — "Data Source Name", a URL that identifies the Sentry *project* to send to. It contains
  a public key; it is not a secret (it is compiled into the binary).
- **Event** — one report (a crash, or a message).
- **Session** — one run of the app, from start to exit/crash. Used for "crash-free rate".
- **Backend** — the strategy the client uses to capture a crash. We use **`inproc`** (in-process):
  the crash is caught inside the dying process by a signal handler. (The alternatives, `crashpad`
  and `breakpad`, run an out-of-process helper and need an extra executable shipped; we avoid that.)
- **Debug-information file / debug-id** — to turn a raw memory address in a crash into a function
  name + file + line, Sentry needs the program's debug symbols (DWARF/PDB). Each binary has a
  unique **build-id**; Sentry matches uploaded symbols to a crash by that id. See
  [Symbolication](#symbolication).

## The client library

We vendor [`sentry-native`](https://github.com/getsentry/sentry-native) as a git submodule at
`src/external/sentry-native`, pinned to a release tag. It is built **statically** with the
**`inproc`** backend and linked into `libansel`. Its HTTP transport reuses the `libcurl` dependency
Ansel already requires on Linux/macOS, and uses WinHTTP on Windows — so no new system dependency.

### Build wiring

- `DefineOptions.cmake` declares:
  - `USE_SENTRY` (default **ON**) — turns the whole feature on/off.
  - `SENTRY_DSN` (cache string) — the project DSN, compiled into the binary. It can be overridden
    (`-DSENTRY_DSN=...`) or emptied to disable uploads while keeping the code.
- `src/external/CMakeLists.txt` builds the submodule (`SENTRY_BACKEND=inproc`, static). It also
  pre-seeds `CMAKE_SIZEOF_LONG=4` on Windows, where `sentry-native`'s own size probe can fail under
  the clang/UCRT64 toolchain.
- `src/CMakeLists.txt` links `sentry` into `lib_ansel` and defines `HAVE_SENTRY=1` and
  `SENTRY_DSN="..."` for the C code.

When `USE_SENTRY=OFF` the module compiles to no-ops, so the rest of the code never needs `#ifdef`s.

## What the application does at runtime

All of this lives in `src/common/sentry.c` / `sentry.h`. The public API is three functions:

| Function | When | What it does |
|---|---|---|
| `dt_sentry_init(have_gui)` | end of `dt_init()` | honors `sentry/enabled`, then `sentry_init()` |
| `dt_sentry_shutdown()` | start of `dt_cleanup()` | records the clean session, flushes, `sentry_close()` |
| `dt_sentry_backtrace_captured()` | from the signal handler | tells the local gdb fallback to stand down |

### Consent (opt-in)

Consent is gathered **once**, by a single dialog shared with usage analytics, implemented in
`src/common/privacy_consent.c` (`dt_privacy_ask_consent()`). On the **very first launch** with a GUI
it shows one checkbox per built-in data flow ("Send crash reports" and "Share anonymous usage
statistics") plus a link to the user-facing
[Data privacy](https://ansel.photos/en/data-privacy/) page, then writes the per-feature toggles.
`dt_sentry_init()` itself no longer prompts — it only reads `sentry/enabled`. The toggle also lives
in **Preferences ▸ Storage ▸ Privacy** ("Send anonymous crash reports").

Two configuration keys are involved:

- `sentry/enabled` — the user-facing boolean. It is a *confgen* key, so it appears in Preferences.
- `privacy/consent_asked` — a single sentinel (shared with analytics) that records whether the user
  has answered the consent dialog yet. It is **deliberately not** a confgen key: because confgen
  defaults are always "present", `dt_conf_key_exists()` could not otherwise distinguish "never asked"
  from "defaulted to off". Once it exists, the dialog is never shown again.

Sentry is initialised only if `sentry/enabled` is true. In non-GUI tools (CLI) the consent dialog
cannot be shown, so Sentry initialises only if the user already opted in during a GUI session.

### Why `dt_sentry_init` runs last in `dt_init`

The `inproc` backend installs its own `SIGSEGV` handler. Ansel also has a long-standing local
handler in `src/common/system_signal_handling.c` that forks `gdb` to write a backtrace file.
Signal handlers chain in reverse install order, so Sentry must be installed **after** the last
`dt_set_signal_handlers()` call (which itself runs after GraphicsMagick clobbers handlers). That is
why `dt_sentry_init()` sits at the very end of `dt_init()`: Sentry runs first on a crash, then
chains down into the local handler.

### Context attached to every report

After init we attach environment context (never any user content):

- **device** context: logical CPU cores, OpenMP thread count, total RAM, OpenCL enabled + device
  names.
- **display** context (GUI only): `dpi`, `dpi_factor`, `ppd`, the main window size and the primary
  monitor resolution + scale factor.
- **tags** (searchable/filterable in the dashboard): `opencl`, `opencl_device`, `build_channel`
  (see [Release & build channel](#release-and-build-channel)), and on Linux/BSD `display_server`
  (x11/wayland), `desktop_environment` (GNOME/KDE/…), `gdk_backend` (what GTK actually renders on).
- **module_usage** context: per-session counts of how often each module was used — views entered,
  iop modules enabled, lib panels opened — keyed `view/<name>`, `iop/<op>`, `lib/<plugin_name>`. It
  shows which modules were exercised in the session that crashed. Counts are recorded via
  `dt_sentry_record_module_usage()` from the GUI thread and mirrored into the scope on each change,
  so the crash handler never reads the live table. This is crash *context* only — Ansel does not do
  all-session feature analytics (Sentry is not an analytics product).
- **processed_image** context: what image was on the pipeline when it crashed — the file
  **extension** (never the name or path), whether it is `raw`/`ldr`/`hdr`/`monochrome`,
  `needs_demosaic` (the buffer still carries a CFA mosaic), the dimensions, and the `pipeline`
  (`darkroom`, `darkroom-preview`, `export`, `thumbnail`). Set via
  `dt_sentry_set_processed_image()` at the start of `dt_dev_pixelpipe_process()` — the single choke
  point both darkroom and export pipelines share — and deduplicated so darkroom's constant
  reprocessing of the same image does not churn.

### Release & build channel {#release-and-build-channel}

Every event carries a **release** and an **environment**, which drive grouping and crash-free metrics:

- **Release** = `ansel@<commit-sha>`, the **full 40-char git commit SHA** (`darktable_commit_hash`).
  This is deliberately *not* the human version string: that string is `git describe`-derived and
  differs across clone types — a shallow/tag-less self-build can't know the tag or commit count, and
  even the abbreviated hash length varies — so using it would scatter one commit across several
  "releases". The full SHA, from `git rev-parse HEAD`, is identical on every clone (shallow or full),
  so all builds of a given commit collapse into exactly one release. It is baked by
  `tools/create_version_c.sh` alongside `darktable_package_version`. The human version
  (`0.0.0+3848~gf82a9a82e1`, or just an abbreviated hash on a shallow clone) is still attached as the
  `version` tag for readability, and is what still drives **package names** (`PROJECT_VERSION` /
  `CPACK_PACKAGE_VERSION` via `tools/get_git_version_string.sh`).
- **Environment** = `DT_BUILD_CHANNEL`: **`nightly`** for official CI builds, **`self-build`** for
  anything compiled locally (possibly a development build). This is the dimension to filter on so
  self-builds/dev noise doesn't pollute the stats for the binaries you actually ship. It is set from
  the `BUILD_CHANNEL` CMake cache var (`DefineOptions.cmake`, default `self-build`); the three nightly
  workflows pass `-DBUILD_CHANNEL=nightly` (Linux via `.ci/ci-script-appimage.sh`, macOS/Windows via
  the matrix `eco`). The same value is also attached as the `build_channel` tag.

The compiler/optimization build type (`Release`/`RelWithDebInfo`/`Debug`) is kept separately as the
`build_type` extra — it is orthogonal to the channel.

### Sessions and session length

We enable `auto_session_tracking`, so Sentry records a session per run and computes its duration
and outcome (exited vs. crashed) — this feeds the crash-free rate on the dashboard.

In addition, every event is stamped with `session_seconds` (a tag + numeric extra) by the
`before_send`/`on_crash` hooks, computed from `darktable.start_wtime`. For crashes this is the exact
time-to-crash. Clean sessions also update local counters mirrored onto later events:
`sentry/clean_sessions`, `sentry/last_session_seconds`, `sentry/total_session_seconds`.

### Crashes: the gdb backtrace attachment

On a crash, the `on_crash` hook (Linux) forks `gdb` against the dying process, captures
`thread apply all bt full`, and attaches it to the report as `gdb-backtrace.txt`. Because `gdb`
resolves symbols **locally** using the on-disk debug info, this attachment is human-readable even
when server-side [symbolication](#symbolication) is not set up.

To avoid running `gdb` twice, `on_crash` sets a flag (`dt_sentry_backtrace_captured()`); the local
handler in `system_signal_handling.c` checks it and skips its own `gdb` run when Sentry already
captured one. When Sentry is disabled (opted-out users, CLI, CI) the local handler runs as before
and writes its `/tmp/ansel_bt_*.txt` file. (Windows/macOS don't use `gdb`; there the native Sentry
stack trace is the report.)

### When does a crash actually reach Sentry?

With the `inproc` backend, the crash is written to a local database
(`~/.cache/ansel/sentry-native/`) **during** the crash, and uploaded **on the next launch** of
Ansel (a dying process cannot reliably complete an HTTPS upload). So: crash → reopen Ansel → the
report appears. This is normal and is not a bug.

## Symbolication {#symbolication}

A native crash event from `inproc` contains raw instruction **addresses** plus the list of loaded
modules — not function names. Sentry turns addresses into `function @ file:line` **server-side**,
but only if it has the matching **debug-information files**. If they were never uploaded, the
dashboard shows frames as addresses and marks the modules *"missing: no debug information could be
found"*.

So readable native stack traces require **uploading debug files** for the exact binaries you ship.
(The `gdb-backtrace.txt` attachment is the fallback that does not depend on this.)

Symbols are matched to a crash by **build-id**, which survives stripping. So you upload the debug
info from the build tree (which has DWARF) and it matches the stripped, shipped binary.

### The upload script: `tools/sentry-upload-symbols.sh`

This is the single source of truth used by both CI and humans. It:

- reads the **auth token** from `SENTRY_AUTH_TOKEN` (the only real secret);
- has the org/project hardcoded (`aurelienpierreeng` / `ansel`), overridable via `SENTRY_ORG` /
  `SENTRY_PROJECT`;
- finds `sentry-cli` on `PATH` or downloads a local copy;
- runs `sentry-cli debug-files upload --include-sources <paths…>`;
- **skips gracefully (exit 0)** if `SENTRY_AUTH_TOKEN` is unset (e.g. forks without secrets).

```sh
# upload symbols for a local build
SENTRY_AUTH_TOKEN="$(cat .sentry-auth)" tools/sentry-upload-symbols.sh build
```

It is intentionally **not** wired into the build: building via `build.sh`, the regular PR CI, or
plain `cmake --build` never uploads anything. Uploads happen only when the script is run explicitly.

## How CI uses Sentry

Only the **nightly** workflows (which build the artifacts users actually download) upload symbols,
as a dedicated step after the build:

- `.github/workflows/lin-nightly.yml`
- `.github/workflows/mac-nightly.yml`
- `.github/workflows/win-nightly.yml`

Each passes `SENTRY_AUTH_TOKEN` (a repository secret) into the script. The regular PR CI
(`ci.yml`) and `build.sh` do **not** upload.

The Linux nightly additionally installs the Ubuntu `dbgsym` debug packages for **glib2** and
**gtk3** and scans `/usr/lib/debug`, so frames in those bundled libraries also resolve. Their
build-ids are stable for a given nightly because the AppImage bundles the runner's copies. `libc`
is *not* bundled (it comes from each user's host), so it cannot be symbolicated centrally — the
`gdb-backtrace.txt` attachment covers it instead. macOS/Windows bundle stripped glib/gtk from
Homebrew/MSYS2 with no separate debug info, so only Ansel's own frames resolve there.

### Required CI secret

In **GitHub ▸ Settings ▸ Secrets and variables ▸ Actions**, set one secret:

- `SENTRY_AUTH_TOKEN` — an *organization* auth token with `project:releases` + `project:write`
  scopes.

Create one at <https://aurelienpierreeng.sentry.io/settings/auth-tokens/>.

## Testing it locally

1. **Create an auth token** at <https://aurelienpierreeng.sentry.io/settings/auth-tokens/>
   (scopes `project:releases` + `project:write`) and save it to a local file `.sentry-auth` at the
   repository root. That file is git-ignored.

2. **Build** in `RelWithDebInfo` (the default) so debug info exists, and install/run the app with
   crash reporting enabled (answer "yes" to the consent dialog, or set `sentry/enabled=TRUE` in
   `anselrc`).

3. **Upload the symbols** for the binaries you are running:
   ```sh
   SENTRY_AUTH_TOKEN="$(cat .sentry-auth)" tools/sentry-upload-symbols.sh build
   ```
   Symbols must match the binaries you crash. After any rebuild you intend to debug, re-upload.
   To also resolve system libraries on your machine, install their debuginfo and add the path, e.g.
   on Fedora:
   ```sh
   sudo dnf debuginfo-install glibc glib2 gtk3
   SENTRY_AUTH_TOKEN="$(cat .sentry-auth)" tools/sentry-upload-symbols.sh build /usr/lib/debug
   ```

4. **Trigger a crash**, then **relaunch** Ansel so the pending report uploads:
   ```sh
   pkill -SEGV -x ansel      # or: kill -SEGV <pid>
   ansel                     # relaunch; the crash from the previous run is sent now
   ```

5. **Look at the dashboard**: the event should show resolved frames and a `gdb-backtrace.txt`
   attachment. Watch the crash-free monitor at
   <https://aurelienpierreeng.sentry.io/monitors/1388118/?project=4511598693253200&statsPeriod=24h>.

Tip: run with `-d control` to see `[sentry] crash reporting initialized` and confirm the feature is
active. The local crash database is at `~/.cache/ansel/sentry-native/`; a pending `*.envelope` there
means a captured crash that has not been uploaded yet (it will be, on the next launch).

## Fetching and fixing an issue

To turn a Sentry issue into a fix without clicking through the web UI, use
`tools/sentry-fetch-issue.sh`. It pulls an issue's **latest event** (stack trace, tags, device /
display / `module_usage` / `processed_image` contexts) and downloads its **attachments** — including
the `gdb-backtrace.txt` Ansel attaches on Linux crashes — into a folder, and prints a readable
summary on stdout:

```sh
tools/sentry-fetch-issue.sh 129371422
# or paste the whole URL:
tools/sentry-fetch-issue.sh https://aurelienpierreeng.sentry.io/issues/129371422/
```

Output (default `sentry-issue-<id>/`): `summary.txt` (read first), `event.json` (full event),
`issue.json` (metadata), and any attachments (`gdb-backtrace.txt`, …).

**Auth/scopes.** This reads issue data, so it needs a token with `event:read`, `project:read` and
`org:read` — the organization token used for [symbol upload](#symbolication)
(`project:releases`/`project:write`) is **not** sufficient and returns HTTP 403. Create a **User Auth
Token** with those scopes (it can also carry the release/write scopes, so one token serves both
scripts) at <https://aurelienpierreeng.sentry.io/settings/account/api/auth-tokens/> and put it in
`.sentry-auth` (git-ignored) or `SENTRY_AUTH_TOKEN`. The region host defaults to `https://de.sentry.io`
to match the project's EU data residency (override with `SENTRY_HOST`).

**The "fix it" loop.** Pair the fetch with an AI coding assistant (e.g. Claude Code) for a tight loop:

1. `tools/sentry-fetch-issue.sh <id>` to pull the backtrace and crash context locally.
2. Ask the assistant to read `sentry-issue-<id>/summary.txt` and `gdb-backtrace.txt`, locate the
   faulting frame in the tree, and propose a fix on a branch.
3. The `processed_image` / `module_usage` context usually points at the reproduction (which pipeline,
   which file type, which modules were active).
4. After releasing the fix, mark the issue **Resolved** in Sentry (optionally "resolved in the next
   release" so a regression reopens it).

**Triage from CI.** `.github/workflows/sentry-triage.yml` is a manual (`workflow_dispatch`) wrapper:
from the Actions tab, "Run workflow", paste an issue id/URL. It runs the fetch script, attaches the
backtrace bundle as an artifact, prints the summary to the run's step summary, and **opens a GitHub
issue once** (deduplicated by a `sentry-issue:<id>` marker) so the crash lands in the tracker. It only
fetches and surfaces — it does **not** change code or open a PR.

It needs a repository secret **`SENTRY_READ_TOKEN`**: a Sentry *User Auth Token* with
`event:read` + `project:read` + `org:read`. This is **separate** from the `SENTRY_AUTH_TOKEN` used by
the nightly workflows for [symbol upload](#symbolication) — that one is an organization token with
release/write scopes and returns HTTP 403 on issue reads. (A single User Auth Token carrying both sets
of scopes could serve both, but keeping them separate scopes each minimally.)

For a more hands-off setup, Sentry's own **Seer** (AI root-cause / autofix) and the **GitHub
integration** can suggest fixes and open PRs directly from an issue; and an issue alert can
auto-create a GitHub issue. Those run server-side and are configured in the Sentry project settings,
independently of this script.

## How to disable it

- **For a build/distribution**: configure with `-DUSE_SENTRY=OFF` (no Sentry code at all), or
  `-DSENTRY_DSN=""` (code present but uploads disabled).
- **For a user**: untick *Preferences ▸ Storage ▸ Privacy ▸ Send anonymous crash reports*, or set
  `sentry/enabled=FALSE` in `anselrc`. Declining the first-launch dialog has the same effect.

## File map

| Path | Role |
|---|---|
| `src/external/sentry-native` | the `sentry-native` client (git submodule) |
| `DefineOptions.cmake` | `USE_SENTRY`, `SENTRY_DSN`, `BUILD_CHANNEL` |
| `src/config.cmake.h` | bakes `DT_BUILD_CHANNEL`/`DT_BUILD_TYPE`; declares `darktable_commit_hash` |
| `tools/create_version_c.sh` | bakes `darktable_commit_hash` (full SHA, the Sentry/PostHog release) |
| `tools/get_git_version_string.sh` | human version string / package name (`describe`-based) |
| `src/external/CMakeLists.txt` | builds the submodule (inproc, static) |
| `src/CMakeLists.txt` | links `sentry`, defines `HAVE_SENTRY` / `SENTRY_DSN` |
| `src/common/sentry.c` / `.h` | init/shutdown, context, sessions, `on_crash` gdb attach |
| `src/common/privacy_consent.c` / `.h` | the shared first-launch consent dialog (crash + analytics) |
| `src/common/system_signal_handling.c` | local gdb fallback; defers to Sentry when it captured |
| `src/common/darktable.c` | calls `dt_sentry_init()` / `dt_sentry_shutdown()` |
| `data/anselconfig.xml.in` / `.dtd`, `tools/generate_prefs.xsl` | the `sentry/enabled` preference |
| `tools/sentry-upload-symbols.sh` | debug-file (symbol) upload |
| `tools/sentry-fetch-issue.sh` | pull an issue's backtrace + attachments locally to fix it |
| `.github/workflows/sentry-triage.yml` | manual CI triage: fetch backtrace, artifact, auto-open a GitHub issue |
| `.github/workflows/{lin,mac,win}-nightly.yml` | call the upload script with the CI secret |
