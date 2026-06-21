# Crash reporting and telemetry with Sentry

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
| `dt_sentry_init(have_gui)` | end of `dt_init()` | consent gate, then `sentry_init()` |
| `dt_sentry_shutdown()` | start of `dt_cleanup()` | records the clean session, flushes, `sentry_close()` |
| `dt_sentry_backtrace_captured()` | from the signal handler | tells the local gdb fallback to stand down |

### Consent (opt-in)

On the **very first launch** with a GUI, a dialog asks the user whether to enable crash reporting,
explaining what is collected. The answer is stored and never asked again. The toggle also lives in
**Preferences ▸ Storage ▸ Privacy** ("Send anonymous crash reports").

Two configuration keys are involved:

- `sentry/enabled` — the user-facing boolean. It is a *confgen* key, so it appears in Preferences.
- `sentry/consent_asked` — a sentinel that records whether the user has answered yet. It is
  **deliberately not** a confgen key: because confgen defaults are always "present",
  `dt_conf_key_exists()` could not otherwise distinguish "never asked" from "defaulted to off".

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
- **tags** (searchable/filterable in the dashboard): `opencl`, `opencl_device`, and on Linux/BSD
  `display_server` (x11/wayland), `desktop_environment` (GNOME/KDE/…), `gdk_backend` (what GTK
  actually renders on).

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

## How to disable it

- **For a build/distribution**: configure with `-DUSE_SENTRY=OFF` (no Sentry code at all), or
  `-DSENTRY_DSN=""` (code present but uploads disabled).
- **For a user**: untick *Preferences ▸ Storage ▸ Privacy ▸ Send anonymous crash reports*, or set
  `sentry/enabled=FALSE` in `anselrc`. Declining the first-launch dialog has the same effect.

## File map

| Path | Role |
|---|---|
| `src/external/sentry-native` | the `sentry-native` client (git submodule) |
| `DefineOptions.cmake` | `USE_SENTRY`, `SENTRY_DSN` |
| `src/external/CMakeLists.txt` | builds the submodule (inproc, static) |
| `src/CMakeLists.txt` | links `sentry`, defines `HAVE_SENTRY` / `SENTRY_DSN` |
| `src/common/sentry.c` / `.h` | init/shutdown, consent, context, sessions, `on_crash` gdb attach |
| `src/common/system_signal_handling.c` | local gdb fallback; defers to Sentry when it captured |
| `src/common/darktable.c` | calls `dt_sentry_init()` / `dt_sentry_shutdown()` |
| `data/anselconfig.xml.in` / `.dtd`, `tools/generate_prefs.xsl` | the `sentry/enabled` preference |
| `tools/sentry-upload-symbols.sh` | debug-file (symbol) upload |
| `.github/workflows/{lin,mac,win}-nightly.yml` | call the upload script with the CI secret |
