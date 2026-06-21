# Usage analytics (telemetry) with PostHog {#telemetry}

[TOC]

## What this is, in one paragraph

Separately from crash reporting ([Sentry](@ref sentry)), Ansel can send **anonymous usage
analytics** so the maintainers can see which features and platforms are actually used and decide
where to spend effort. This goes to [PostHog](https://posthog.com), a product-analytics service, in
its **European Union** region (`https://eu.i.posthog.com`). It is **opt-in**, separate from crash
reporting, and can be turned off at any time. No images, file names, or personal data are ever sent —
only counts and coarse system facts, keyed by a random per-installation id.

This page is for maintainers. The user-facing explanation of everything collected and why lives in
the [Data privacy](https://ansel.photos/en/data-privacy/) page of the user manual.

## Vocabulary

- **Project API key** — a *public, write-only* ingest key (prefix `phc_`). It only allows sending
  events; it cannot read data. Like the Sentry DSN, it is compiled into the binary and is not a
  secret.
- **Event** — one named record (e.g. `session_start`) with a free-form JSON `properties` object.
- **`distinct_id`** — PostHog's identity for an event. We use a **random per-installation UUID**
  (`telemetry/install_id`), generated on first run. It is not tied to any account or machine
  identifier, and lets PostHog group a single installation's events without identifying the user.

## The client

There is no third-party SDK. The whole implementation is `src/common/telemetry.c` / `telemetry.h`,
~200 lines, built on dependencies Ansel already has:

- **json-glib** to serialize event bodies,
- **libcurl** to POST them.

Events are sent asynchronously so the UI never blocks on the network: `dt_telemetry_capture()`
serializes the event on the caller's thread and pushes the JSON string onto a `GAsyncQueue`; a
background `GThread` (`_telemetry_worker`) pops bodies and POSTs them to `{POSTHOG_HOST}/capture/`.
Network errors are ignored (best-effort). `dt_telemetry_shutdown()` pushes a stop sentinel, joins the
worker (so the last in-flight POST completes), and frees state.

> The worker relies on `curl_global_init()` having already run; it has, because Sentry's transport /
> `curl_tools` initialize curl earlier in startup.

### Build wiring

- `DefineOptions.cmake` declares `USE_TELEMETRY` (default **ON**) and `POSTHOG_HOST` (the EU host).
- The **project API key is hardcoded** in `src/common/telemetry.c` (`POSTHOG_API_KEY`), the single
  source of truth — analogous to the baked-in Sentry DSN. Set it to `""` there to disable uploads
  while keeping the code.
- `src/CMakeLists.txt` adds the source and defines `HAVE_TELEMETRY=1` and `POSTHOG_HOST="…"`.

When `USE_TELEMETRY=OFF` the module compiles to no-ops, so callers never need `#ifdef`s.

## Runtime behavior

The public API:

| Function | When | What it does |
|---|---|---|
| `dt_telemetry_init(have_gui)` | end of `dt_init()` | honors `telemetry/enabled`, starts the worker, sends `session_start` |
| `dt_telemetry_shutdown()` | start of `dt_cleanup()` | sends `session_end`, drains & joins the worker |
| `dt_telemetry_capture(event, props)` | anywhere | queues a custom event (takes ownership of `props`) |
| `dt_telemetry_record_module_usage(category, name)` | GUI thread | counts a view/lib/iop use for this session |
| `dt_telemetry_record_file_type(img, pipeline)` | pipeline threads | records the kind of image processed |

### Consent (opt-in)

Telemetry shares the **single** first-launch consent dialog with crash reporting
(`dt_privacy_ask_consent()` in `src/common/privacy_consent.c`): it has one checkbox per built-in data
flow and a link to the user-facing Data privacy page. `dt_telemetry_init()` itself does not prompt —
it only reads the toggle. Keys:

- `telemetry/enabled` — user-facing boolean; a *confgen* key, shown in **Preferences ▸ Storage ▸
  Privacy** ("Share anonymous usage statistics").
- `telemetry/install_id` — the random `distinct_id` (non-confgen; created lazily on first enabled
  run).
- `privacy/consent_asked` — the shared sentinel that records the dialog has been answered (see the
  [Sentry doc](@ref sentry) for why it must not be a confgen key).

Telemetry initializes only if `telemetry/enabled` is true **and** a non-empty `POSTHOG_API_KEY` was
compiled in.

### What is collected

Four event types: a per-run start/end pair, plus two discrete "something happened" events.

**`module_used`** is sent the **first time** each module is used in a session (deduped: one event per
distinct module per session), with properties `category` (`view`/`lib`/`iop`) and `name`. **`file_opened`**
is sent the **first time** each file extension is processed in a session, with `extension` and the
type flags `raw`/`ldr`/`hdr`/`monochrome`/`needs_demosaic` and `pipeline`.

These two are the answer to "I crashed-tested and saw no usage data": unlike `session_end` (below),
which is only emitted on a *clean* exit, discrete events are sent **immediately** as usage happens, so
they survive a later crash. They are also the idiomatic PostHog shape — you count/break-down the
events directly (e.g. `file_opened` broken down by `raw`, or `module_used` by `name`).

**`session_start`** carries the machine profile (`_telemetry_system_properties()`), so every session
— healthy or crashing — is represented for population statistics (something Sentry sessions can't
give you, since they have a fixed schema):

- `app_version` (human version string), `commit` (full git SHA — the cross-build-consistent
  release id, mirrors Sentry's release), `build_type`, `build_channel` (`nightly` for official
  builds, `self-build` otherwise — filter on this to exclude local/dev builds from population stats),
- `os` (pretty name), `cpu_cores`, `ram_gb`,
- `opencl` (bool) and `gpu` (first OpenCL device name),
- on Linux/BSD: `display_server` (x11/wayland), `desktop_environment`,
- `dpi`, `ppd`, `screen_width`, `screen_height`.

**`session_end`** (clean exit only) carries the session aggregate
(`_telemetry_session_end_properties()`):

- `session_seconds`,
- **module usage** as *flat* numeric properties: `mod_view_<name>`, `mod_lib_<plugin>`,
  `mod_iop_<op>` → use counts,
- **file types** as *flat* numeric properties: `ext_<extension>` → count (never names or paths),
- `images_processed`, `raw_images`, `nonraw_images`, `mosaiced_images` (distinct image+pipeline
  combinations seen).

> **Why flat, not nested?** PostHog only filters / breaks down / aggregates on *top-level scalar*
> event properties. Nested JSON objects are ingested and visible in a single event's detail, but do
> **not** appear as dimensions you can build insights on — so module usage and file types are emitted
> as flat `mod_*` / `ext_*` properties (`_flatten_counts()`), each character outside `[A-Za-z0-9_]`
> replaced by `_`.

The two `record_*` functions accumulate into in-memory hash tables / counters guarded by a mutex
(`_stats_lock`), because module usage is recorded on the GUI thread while file types are recorded
from pipeline worker threads. `record_file_type` de-duplicates per image+pipeline, mirroring the
crash-context dedup, so darkroom's constant reprocessing of the same image counts once. The same
call sites also feed Sentry's crash context — see the table below.

### Shared instrumentation points

The usage data is recorded at the same choke points that feed Sentry's crash context, with a
telemetry call placed next to each Sentry call:

| Site | Sentry call | Telemetry call |
|---|---|---|
| `src/views/view.c` (view entered) | `dt_sentry_record_module_usage("view", …)` | `dt_telemetry_record_module_usage("view", …)` |
| `src/libs/lib.c` (panel expanded) | `dt_sentry_record_module_usage("lib", …)` | `dt_telemetry_record_module_usage("lib", …)` |
| `src/develop/imageop.c` (module enabled) | `dt_sentry_record_module_usage("iop", …)` | `dt_telemetry_record_module_usage("iop", …)` |
| `src/develop/pixelpipe_hb.c` (pipeline start) | `dt_sentry_set_processed_image(…)` | `dt_telemetry_record_file_type(…)` |

The difference in scope: Sentry keeps these as **crash context** (the state at the moment of a
crash), while PostHog receives the **per-session aggregate** at clean shutdown.

## Viewing the data

PostHog project (EU): create insights under **Product analytics**. Useful breakdowns:

- session counts over time and by `app_version` / `os` / `build_type`,
- `display_server`, `desktop_environment`, `gpu` distributions (where to focus QA),
- count `module_used` broken down by `name` to rank which views/panels/modules are used, and
  `file_opened` broken down by `extension` or `raw` to see the formats people edit — these work even
  for sessions that crashed,
- on `session_end` events (clean exits), sum the `mod_*` / `ext_*` properties for full per-session
  counts.

## How to disable it

- **For a build/distribution**: configure with `-DUSE_TELEMETRY=OFF` (no analytics code at all), or
  set `POSTHOG_API_KEY` to `""` in `src/common/telemetry.c` (code present, uploads disabled).
- **For a user**: untick *Preferences ▸ Storage ▸ Privacy ▸ Share anonymous usage statistics*, or
  set `telemetry/enabled=FALSE` in `anselrc`. Declining the first-launch dialog has the same effect.

## File map

| Path | Role |
|---|---|
| `DefineOptions.cmake` | `USE_TELEMETRY`, `POSTHOG_HOST` |
| `src/CMakeLists.txt` | adds the source, defines `HAVE_TELEMETRY` / `POSTHOG_HOST` |
| `src/common/telemetry.c` / `.h` | init/shutdown, async POST worker, events, per-session aggregates |
| `src/common/privacy_consent.c` / `.h` | shared first-launch consent dialog (crash + analytics) |
| `src/common/darktable.c` | calls `dt_telemetry_init()` / `dt_telemetry_shutdown()` |
| `src/views/view.c`, `src/libs/lib.c`, `src/develop/imageop.c`, `src/develop/pixelpipe_hb.c` | the recording call sites |
| `data/anselconfig.xml.in` / `.dtd`, `tools/generate_prefs.xsl` | the `telemetry/enabled` preference |
