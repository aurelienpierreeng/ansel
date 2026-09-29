# Ansel Dev doc {#mainpage}

[TOC]

Welcome on the new [Ansel](https://ansel.photos) developer documentation. As it is brand-new, it is still in progress, but we are getting there.

## Getting started

### Getting the source code

You will need Git:

```bash
git clone --recurse-submodules https://github.com/aurelienpierreeng/ansel.git
```

For cross-plateform building instructions, please refer to the [user doc](https://ansel.photos/en/doc/install/).

### Getting in sync with the project

Who are our users ? What are we trying to to ? How do we tackle problems ? How do we manage the project and its priorities ? [Read the contributor section](https://ansel.photos/en/contribute).

### Find issues to tackle

- [TODO list](todo.html) from code `//TODO` and `//FIXME` comments,
- [Github issues](https://github.com/aurelienpierreeng/ansel/issues)

## Scope and purpose of the present doc

At this stage, the present documentation is mostly the API reference automatically built by Doxygen from the (few) docstrings found in the source code, in particular in `.h` header files. Those documented objects aim at being reusable, so a documentation makes them public.

Dependencies and function calls graphs are also plotted for each object. These are useful to track bugs through the chain of callings without having to grep them in the code, in a less graphical way. They also show the shitshow inherited from upstream Darktable in terms of non-modular modules : you see how everything includes everything, so the spaghetti code becomes quite literally visible.

As time will go, we will add real dev documentation, explaining how the core tasks are handled, based on what assumptions and covering what use cases. This should prevent re-implementing the same feature, sometimes 4 times or more, as was seen in Darktable since 2020.

## The rules, and where the knowledge lives

[`../CLAUDE.md`](../CLAUDE.md) holds the seven binding rules and a table of contents into this
directory. It used to hold the knowledge too, and had grown to 3,177 lines; a verification pass
on 2026-09-29 found sixteen claims in it that were wrong, so the findings moved here on the
condition that **each one carries the commit it was established against**.

If you write a finding down, date it and name the commit. If you act on one older than the code
you are changing, re-measure it first and re-date it when you confirm it. A file nobody can date
is a file nobody can trust — that is the whole reason this directory now looks like this.

Files carrying findings migrated from CLAUDE.md on 2026-09-29, each verified against
`42eca0e8fe`: `architecture-rules.md`, `preferences.md`, `pipeline-cache.md` (appended),
`pipeline-history.md`, `image-mipmap-cache.md`, `raw-roi-cfa.md`,
`highlights-reconstruction.md`, `colorprofiles.md`, `interpolation.md`, `masks-geometry.md`,
`masks-gui.md`, `masks-history.md`, `iop-notes.md`, `gtk-patterns.md`, `accelerators.md`,
`collection.md`, `export.md`.

## Which files have been verified, and which have not

A file is only trustworthy to the extent someone has checked it against the code. As of
**2026-09-29, against `f37105c227`**:

- **Migrated and verified** (17 files) — written during the CLAUDE.md migration, every
  falsifiable claim checked, then independently audited. That audit found six further wrong
  claims, corrected in `9f91c0ceb8`: `accelerators.md`, `architecture-rules.md`, `collection.md`, `colorprofiles.md`, `export.md`, `gtk-patterns.md`, `highlights-reconstruction.md`, `image-mipmap-cache.md`, `interpolation.md`, `iop-notes.md`, `masks-geometry.md`, `masks-gui.md`, `masks-history.md`, `pipeline-cache.md`, `pipeline-history.md`, `preferences.md`, `raw-roi-cfa.md`.
- **Pre-existing and verified** (15 files) — swept on 2026-09-29. The sweep found **106 wrong
  claims, 58 of them HIGH severity**, across these files: `brush-boundary.md`, `color.md`, `control-split.md`, `darkroom-redraw.md`, `develop-split.md`, `doc-screenshots.md`, `drawlayer.md`, `exif-split.md`, `exiv2.md`, `filmic-agx.md`, `geometry-service.md`, `globals-migration.md`, `gui-sizing.md`, `history-split.md`, `image-type-detection.md`. Each now carries a header with its
  own counts; the corrected ones say what they used to say.
- **NOT YET VERIFIED** (25 files) — never checked against the tree by anyone. Treat every claim
  in them as unconfirmed: `gtk-decoupling.md`, `gui.md`, `include-graph.md`, `include-hygiene-roadmap.md`, `lensfun-cost.md`, `lock-audit.md`, `masks-enclosure-p2.md`, `masks_history_dedup.md`, `nightly-distribution.md`, `opencl-math-accuracy.md`, `overlay-raster.md`, `rawdenoiseai.md`, `removal-undo.md`, `reorganisation.md`, `resizing-scaling.md`, `retouch-result-memo.md`, `selection.md`, `sentry.md`, `static-iop.md`, `studio-capture.md`, `supervisor.md`, `telemetry.md`, `thumbnail_color_management.md`, `thumbtable.md`, `xmp-crawler.md`.

The single most valuable check is whether anything a file marks OPEN, TODO or "not yet fixed"
has in fact been fixed. A stale OPEN sends a reader to re-derive a landed fix, and that is the
specific failure that cost the most in CLAUDE.md.

## Working on the codebase structure

`reorganisation.md` is the entry point: the module map (what each `src/` directory is for, and
whether it holds state), the rules CI enforces, and what remains to be done. Read its *Module
map* and *The rules* sections before any structural change.

Related, more specific:

- `include-graph.md` — why include guards rather than `#pragma once`, and how cycles are measured
- `globals-migration.md` — dispatching the `darktable` global through function arguments
- `pipeline-cache.md` — the cache-wait protocol and raster-mask side-band cachelines
- `image-type-detection.md` — the provisional → resolved image lifecycle

## Useful links

- [User documentation](https://ansel.photos/en/doc/), in particular:
    - [Build and test on Linux](https://ansel.photos/en/doc/install/linux)
    - [Build and test on Windows](https://ansel.photos/en/doc/install/linux)
- [Contributing guidelines](https://ansel.photos/en/contribute/), in particular:
    - [Project organization](https://ansel.photos/en/contribute/organization/)
    - [Translating](https://ansel.photos/en/contribute/translating/)
    - [Coding style](https://ansel.photos/en/contribute/coding-style/)
- [Project news](https://ansel.photos/en/news/)
- [Community forum](https://community.ansel.photos/)
- [Matrix chatrooms](https://app.element.io/#/room/#ansel:matrix.org)
- [Support](https://ansel.photos/en/support/)
