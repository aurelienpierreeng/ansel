<!-- Provenance: every finding carries the commit it was established against. -->

# Architectural rules

> **Verified against `42eca0e8fe` on 2026-09-29.**
>
> The five rules CI enforces on every change, and the evidence behind each. `CLAUDE.md` states them as imperatives; this file says why, and what each one cost to learn.
>
> Each finding below is dated with the commit that established it. A finding is only as
> good as its hash: before acting on one older than the code you are changing, re-measure
> it — and re-date it here when you do. Where an earlier version of a claim was **wrong**,
> that is recorded rather than quietly corrected: how a claim was wrong is usually the more
> useful thing to know.

Related: [`include-graph.md`](include-graph.md), [`reorganisation.md`](reorganisation.md), [`include-hygiene-roadmap.md`](include-hygiene-roadmap.md).

## `#pragma once` is FORBIDDEN — use an include guard

*Found `8de7446ff7`, 2026-08-06.*

Every header in `src/` uses an explicit `#ifndef DT_<PATH>_H` / `#define` / `#endif` guard,
named after the path relative to `src/` (`src/develop/masks/masks_history.h` →
`DT_DEVELOP_MASKS_MASKS_HISTORY_H`). **Do not add `#pragma once` to an existing header, and do
not start a new one with it.**

This is not a style preference. `#pragma once` and an include guard behave identically at the
preprocessor level, but `#pragma once` *silently* makes a cyclic include graph compile: a
header re-entered mid-definition is skipped, and the first inclusion finishes with whatever it
had at that point. That is how three include cycles survived unnoticed in this codebase for
years, each one a header trailing-including a header that includes it back (see
`doc/include-graph.md`). Explicit guards make the same situation greppable and reviewable
instead of invisible.

Enforcement: `python3 tools/pragma_once_to_guards.py --verify` exits non-zero if any
`#pragma once` reappears, and runs in CI's "Check include hygiene" step. It sweeps every
header spelling — `.h`, `.hh`, `.hpp`, `.hxx` — from the repository root the tool itself sits
in, not from the working directory: a sweep restricted to `.h`, or one run from the wrong
directory, is a gate that passes by finding nothing. `python3 tools/include_graph.py
--summary` must keep reporting `cycles 0`.

**`darktable.h` (at `src/`, not in a module) has no guard either — it has a TRIPWIRE.** It ends up included by
at most one path per translation unit (an entry point calling `dt_init()`, or a subsystem
that owns one of the `darktable` members), so a *second* inclusion is never legitimate: it
means the header arrived through a path nobody intended. A guard would absorb that
silently; instead the file `#error`s on re-inclusion. If you hit it, do not add a guard —
find who included it and give that code the specific lib it needs (`common/logging.h`,
`system/mem_alloc.h`, …) or the accessor for the global it wants (`dt_dev_get_global()`,
`dt_control_get_global()`, …). **No header may include it**; as of this writing none does.

**When auditing this, grep for `darktable\.h"` and check the spelling.** Includes can be
written relative to the including file's own directory, which is how several files hid from
earlier audits while the header still lived in `src/common/`. It now sits at `src/`, so
`#include "darktable.h"` IS the canonical root-relative spelling. Three files (and one *header*, `common/colorchecker.h`) hid behind
that spelling through several audits of this series; the compile-time tripwire is what
finally caught them. `tools/include_graph.py` resolves both spellings and was right when the
ad-hoc greps were wrong.

**Five headers deliberately have NO guard at all** and must never get one:
`common/module_api.h`, `views/view_api.h`, `libs/lib_api.h`,
`imageio/format/imageio_format_api.h`, `imageio/storage/imageio_storage_api.h`. They are
X-macro headers, re-included several times in the *same* translation unit with different macros
defined, and expanded *inside struct bodies* to generate members. For the same reason a
**top-level `#include` in one lands inside those structs**. The precise rule, as the imageio
pair actually implement it: real includes must sit inside the `#ifdef FULL_API_H` block —
that macro is defined only in full-API mode, while the struct-body expansion defines
`INCLUDE_API_FROM_MODULE_H` instead and skips the block — and only *other* X-macro headers
(`common/module_api.h`) may be included unguarded.

> **Two corrections, 2026-09-29.** This used to say `common/module_api.h` "has no includes
> itself". It has one — `#include <glib.h>` at `:32`, four lines below its own comment saying no
> include may ever be added. It is harmless only by accident: glib's own guard makes the
> in-struct re-expansion empty. And only the **imageio pair** actually implement the rule as
> stated; `views/view_api.h:27` and `libs/lib_api.h:26` carry real system includes
> (`<gtk/gtk.h>`, `<glib.h>`) at struct-body-expansion level. So a reader auditing all five
> against this paragraph will find two that do not conform, which the paragraph did not say. Symbols used
outside that block (`dt_version()`, `dt_print()`, `IS_NULL_PTR`) are the consuming `.c` file's
responsibility.

## A header includes only what its own declarations need

*Found `8400a289b4`, 2026-08-08.*

Everything else belongs in the `.c`. A header that includes more becomes a supply line its
consumers never asked for and cannot see: they compile because something upstream happened to
pull in what they use, and the day anyone tidies that include away the breakage surfaces
somewhere else entirely, in a file that was never touched.

This is not theoretical. Removing `gui/gtk.h` broke a dozen IOPs because it had been the only
thing pulling `sqlite3.h` in ahead of `common/points.h` (whose vendored SFMT `#define N` then
collided with `sqlite3_compileoption_get(int N)`). And within the same series, deleting an
unused `widgets/label.h` from `widgets/dialog.c` removed `dt_free` — arriving through
`label.h` -> `system/mem_alloc.h` — from a file that had never named either header.

Concretely: if a header declares `void f(GtkWidget *w)`, it includes `<gtk/gtk.h>` and nothing
more. Implementations do not belong there either — `widgets/label.h` carried five `static
inline` helpers, and those five forced four extra includes on all ~30 of its consumers.
Moving them to `label.c` left the header needing only `<gtk/gtk.h>`.

The one legitimate exception is a header whose published interface *is* inline code
(`widgets/draw.h`), which necessarily includes what that code calls. Keep those rare, and keep
them honest: they are a deliberate performance trade, not a convenience.

`tools/check_unused_includes.sh` gates the include lines a change adds.
`tools/header_consumers.py` reports what each includer of a header actually takes from it,
separating a header's own symbols from what it merely forwards.

**`header_consumers.py`'s "files using nothing from it" bucket does NOT mean the file can
drop the include.** It means *this include is redundant — the file reaches those symbols
through one of its other includes*. Those are exactly the files that are relying on the
supply line described above, so under this tree's rule they need the include **added
explicitly**, not removed. Deleting all seven such includes when `colorprofiles/colorspaces.h`
was split still compiled in Release *and* Debug, and broke `build-nofeatures`, where
`control/jobs/control_jobs.h` lost the two types `dt_control_export()` is declared with — the
other supplier only existed in the feature-full configurations. Confirm against the symbols
the file actually names (`grep` for the header's types and functions) before removing
anything, and never trust one build configuration to prove an include is unnecessary.

## No SQL in GUI modules

*Found `22f623c0be`, 2026-06-25.*

`src/libs/` and `src/views/` modules must contain no raw SQL. Database access belongs behind
named functions in `src/common/` (e.g. `common/collection.c`, `common/film.c`). When a GUI
module needs data, add or extend a `dt_collection_*` / `dt_film_*` / `dt_tag_*` function and
call it. Reuse existing helpers (`dt_collection_get_extended_where`, `dt_film_get_id`,
`dt_selection_select_list`) rather than re-issuing SQL.

Examples added during the collect rewrite: `dt_collection_get_property_values()`,
`dt_collection_get_images_for_rule()`, `dt_film_relocate()`.

## Pipeline↔module interface is history

*Found `22f623c0be`, 2026-06-25.*

The ONLY thread-safe interface between the pixel pipeline and an IOP module is **history**
(guarded by `dev->history_mutex`). `module->params` and `module->blend_params` belong to the
GUI thread and are NOT thread-safe — the pipeline thread must never read or write them.

Do NOT call `dt_iop_commit_params(module, module->params, ...)` from pipeline code. Commit
from the history snapshot (`hist->params`), never the live module params.

To push live/transient state to the pipe (e.g. drawlayer realtime stroke, ashift edit mode),
either (a) write it through history under `history_mutex`, or (b) use the transient-resync
interface `dt_dev_transient_params_{set,clear,get,active}` in `dev_history.{h,c}`.

See `doc/reorganisation.md` for the threading model (GUI diamond nodes vs. pipeline round nodes).

## A stored format's version is bumped only once it has shipped in a round-numbered release

*Found `af78aa4e42`, 2026-09-21.*

Three formats outlive the build that writes them: a module's params (`DT_MODULE_INTROSPECTION`), the
database schema (`CURRENT_DATABASE_VERSION_LIBRARY` / `_DATA`, `database/database.c`) and the XMP
sidecar (`DT_XMP_EXIF_VERSION`, `common/xmp_sidecar.cc`). Bump one only if its current version was
distributed in a round-numbered release: Ansel 1.0, 2.0, … or, for a version inherited from
darktable, a darktable release. A version that has not shipped in one is still open, and a change
goes into it without a bump. For the database schema and the XMP, the bump itself is made as late
as possible, just before Ansel's version number changes.

A bump rejects nothing: the build that makes it converts every older version (`legacy_params()`,
the schema migration steps, the readers of older XMP versions). What it adds is permanent, since
each version keeps its conversion code for good, so versions follow round-numbered releases instead
of piling up one per change in nightlies.

Changing an open version in place has its own conditions, since nightlies have already written it:

- **Module params**: append, never insert. The conversions from older versions commonly copy the
  old layout as a prefix over the defaults, so existing members keep their offsets. A blob of the
  same version but another size makes `_sync_params()` call `legacy_params(N, N)`, which is not
  told the stored size and has no branch for it, so the step is dropped: an edit saved with the
  shorter layout loses that module. `_sync_params()` does not fall back to copying the common
  prefix.
- **Database schema**: a library already at version N never re-runs the step that brought it there
  (`_upgrade_library_schema_step()`), so what is added to N in place must also be applied,
  idempotently, to a library already at N: `_sanitize_db()` runs at every open.
- **XMP**: a new key is optional, its absence meaning the default, and an existing key keeps its
  meaning: a sidecar written earlier under the same version still reads, and an older build
  reading a newer one skips what it does not know.

Schema 36, data 9 and XMP 5 come from darktable releases. The bumps planned for Ansel 1.0 (schema
37, XMP 6) wait on the `masks-history-dedup` branch, see `doc/masks_history_dedup.md`.

---
