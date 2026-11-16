<!-- Provenance: every finding carries the commit it was established against. -->

# The preferences system

> **Verified against `42eca0e8fe` on 2026-09-29.**
>
> A new preferences SECTION takes three edits and missing one fails silently; an ordinary entry takes one.
>
> Each finding below is dated with the commit that established it. A finding is only as
> good as its hash: before acting on one older than the code you are changing, re-measure
> it — and re-date it here when you do. Where an earlier version of a claim was **wrong**,
> that is recorded rather than quietly corrected: how a claim was wrong is usually the more
> useful thing to know.

*Found `22f623c0be`, 2026-06-25.*

Adding a new preferences **section** requires three edits. Adding an *entry to an existing
section* requires only the first.

> **Correction, 2026-09-29.** This said "Adding a Preferences entry requires **three** edits, not
> one" without qualification, and that is false for the ordinary case — as items 2 and 3 below
> already half-admit by saying "a new section value must be added here". Measured over the
> 108-commit history of `data/anselconfig.xml.in`: of the **45** commits that added a `<dtconfig>`
> entry, **40 touched only the XML**. The 5 that touched the DTD or XSL are exactly the ones
> introducing a new section (e.g. `8db319612e` adding `privacy`).

The three edits, when the entry introduces a new section:

1. `data/anselconfig.xml.in` — the `<dtconfig prefs="..." section="...">` entry.
2. `data/anselconfig.dtd` — the `section` attribute is an enumerated list; a new section value
   must be added here or `xmllint` fails the build (`USE_XMLLINT=ON`).
3. `tools/generate_prefs.xsl` — the GUI is generated into `build/src/preferences_gen.h`; each
   tab renders only **explicitly enumerated** `<xsl:for-each select="...@section='X'">` blocks.
   A section not listed in the XSL is silently dropped from the UI even if valid in XML/DTD.

## When the settings reach the disk

*Established 2026-10-26 on top of `1a1a9a279b`.*

`dt_conf_set_*()` writes the in-memory table only. `anselrc` is written by `_conf_save()`
(`src/common/conf.c:960`), private to that file and reached through `dt_conf_flush()` or
`dt_conf_cleanup()`, and nothing else. The callers:

| Where | When |
|---|---|
| `common/opencl.c:1011` | a new OpenCL device was configured at startup |
| `gui/preferences.c:805` | the preferences dialog closes |
| `darktable.c:1914` | `dt_cleanup()`, after its last question to the user and before the teardown |
| `dt_conf_cleanup()`, `darktable.c:1997` | the tail of `dt_cleanup()`, for what the teardown itself set |

A setting changed anywhere else lives in memory until one of these runs: a process that dies
earlier loses it.

A failed write is printed on stderr with its reason, and `dt_conf_flush()` returns 1. Only the
preferences dialog acts on it, with an alert window (*Settings not saved*, the configuration
folder as its item): the user has just changed settings and is there to see it. The other callers
run at startup or during the teardown, where the stderr line is all there is.

**The file is replaced, never rewritten in place.** The table is serialised under `cf->mutex`,
then written with `g_file_set_contents()`, which writes `anselrc.XXXXXX` beside it and renames it
over `anselrc`. A process killed during a save leaves the previous file whole. Checked under gdb on
`ansel-cli`: killed at the `rename()`, `anselrc` kept its 12825 bytes; with the `fopen("wb")` of
`1a1a9a279b`, killed after the open, it was 0 bytes. The temporary file of an interrupted save
stays behind; nothing removes it.

`tests/unittests/test_conf_save.c` pins this without killing anything: a handle opened on the old
file before a flush must still read the old content after it, which a rewrite in place breaks.
The same file checks that a flush writes what was set, leaves no temporary file, and returns 1
when it cannot write.

The write happens under `cf->mutex`, so two saves land in the order they read the table. GLib calls
`fsync()` before the rename, which sets the cost: the mutex is held **4–7 ms** per save, measured
with a monotonic-clock probe around the save over 10 `ansel-cli` runs, 15892-byte `anselrc`, ext4
on a SATA SSD. Any thread reading a setting waits that long.

## Conf defaults

Conf defaults: `dt_conf_key_exists()` returns true for any confgen key even on first run (defaults
are loaded at startup). To detect "user has never decided", use a **non-confgen** sentinel key
written only after the user acts.

> **The worked example moved, and this pointed at dead code until 2026-09-29.**
> `DT_SENTRY_ASKED_KEY "sentry/consent_asked"` still sits at `src/common/sentry.c:66` but is
> referenced nowhere — the same is true of `telemetry.c:50`. The live implementation is
> `src/gui/privacy_consent.c`: `DT_PRIVACY_ASKED_KEY "privacy/consent_asked"` (`:37`), the
> sentinel test `if(dt_conf_key_exists(DT_PRIVACY_ASKED_KEY)) return;` (`:46`), and the writes
> after the user acts (`:58`, `:132`). Consent is now gathered once at startup by
> `dt_privacy_ask_consent()` for both sentry and analytics; `sentry.c:583` says so in a comment.
> The pattern the rule teaches is unchanged and still practised — only the pointer was stale.

## A setting read without being declared

*Established on top of `db34f066f5`, 2026-11-16. Measured in the GUI on scratch profiles, each
launched, quit with Ctrl+Q so `anselrc` is flushed, and read back; the display recipe is in
[`color-picker.md`](color-picker.md), *How this was measured*.*

Reading a setting that `data/anselconfig.xml.in` does not declare is **not an error**. Nothing is
logged. `_dt_conf_get_var_locked()` inserts an empty value under that name and returns it, and the
empty value is then written to `anselrc` with everything else.

That is how the scopes came to draw nothing on every new profile. `libs/histogram.c` read
`plugin/darkroom/histogram/{op,display,zoom}`; the file declared them as `plugins/…`. A fresh
profile, after one launch:

```
plugin/darkroom/histogram/display=
plugin/darkroom/histogram/op=
plugin/darkroom/histogram/scope_height=250      <- the one declared under the name it is read by
plugin/darkroom/histogram/zoom=
```

`op` empty names no stage, so the scopes had no buffer and the global colour picker read 0. The
*Show data from* menu would meanwhile have shown "Final display" ticked: `_backbuf_op_to_int()`
answers 2 for anything it does not know (read from the code; the menu was not opened). `zoom` empty fell to the bottom of the module's own clamp, 32,
instead of the declared 220.

**The stored profiles decide which side moves.** Every existing profile holds the spelling the
code writes, so the declaration was renamed, not the code; renaming the code would have orphaned
every stage, scope type and zoom anyone had chosen.

Three things about declaring a setting late, each measured:

| Stored by the old build | After the name is declared | Why |
|---|---|---|
| `display=` (int, empty) | `0`, the default | `dt_conf_get_int_fast()` reads garbage, takes the default **and writes it back** |
| `zoom=` (float, empty) | `220.0`, the default | same, in `dt_conf_get_float_fast()` |
| `op=` (string, empty) | **stays empty** | a string is never garbage; the stored value wins over the default |

The third row is why the fix needed code and not only the declaration: `_set_params()` now reads a
stage name it does not know as "never chosen". The control was a setting nothing here touches —
`session/sub_directory_pattern`, declared as a string with a non-empty default, stored empty, and
still empty after a launch and a quit.

**A declared bound is applied when the setting is read, not when the profile is loaded.**
`dt_conf_get_int()` and `dt_conf_get_float()` clamp what they return to the declared `min`/`max`.
Nothing clamps the stored value: `scope_height=5000`, declared `max="3000"`, came back from a
launch and a quit as `5000`.

> **`_sanitize_confgen()` does nothing on load, and its comment says it does.** `dt_conf_init()`
> runs it on every line of `anselrc` "to ensure that numbers are properly clamped", but it looks
> each name up in the declarations table, and `darktable.c` fills that table with
> `dt_confgen_init()` *after* `dt_conf_init()` has read the file (`:1397`, then `:1403`). On load
> the table is empty, every lookup misses, and every value passes through untouched. I believed
> the comment, and first wrote here and in the XML that bounds were enforced at load; the
> `scope_height=5000` run is what showed otherwise. Left as it is — making it work would clamp
> stored values across the whole application, which is its own change.

**A declaration under the wrong name can carry wrong bounds that only bite once the name is
right.** `display` was declared `max="2"` for an enum of six scope types. Harmless while nothing
read it; with the name corrected, `dt_conf_get_int()` would have returned 2 for every stored 3, 4
or 5, turning each parade and vectorscope into a vertical waveform on every start. The declaration
carries no bound now: the module clamps to its own enum, the only place that knows how many there
are. Measured with `display=5` stored: the vectorscope is drawn, and `5` is still there after.
*(The `max="2"` outcome itself is read from `dt_conf_get_int()`, four lines; it was not built and
run.)*

**Is this the only one?** For this exact mistake, yes. Every literal name passed to a
`dt_conf_get*`, `dt_conf_set*` or `dt_conf_key_exists` call under `src/` (338 of them, a regular
expression over the sources, so names built at run time are not counted) was compared with the
declared names: three are undeclared while their `plugin/` ↔ `plugins/` twin is declared, and they
are these three. The same pass counts **100** literal names that are read or written with no
declaration at all — not this mistake, and not looked into, but each one is a setting that is
created empty on a new profile.

**What changes for someone who never touched the scopes.** The stage becomes "Final display" for
real, and the scopes draw. A vectorscope zoom that was never set goes from 32 to the declared 220,
which is a visibly different view — at 32 the plot is magnified well past the primaries' markers,
at 220 the whole gamut circle is shown. Scrolling on the vectorscope sets it, as before.
