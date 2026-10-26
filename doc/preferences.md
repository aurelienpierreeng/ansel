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
