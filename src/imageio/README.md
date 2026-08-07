# `src/imageio/` — image files in and out

Reading and writing image **files**: codecs, container formats, and the format/storage
module APIs. Layer **6**, alongside `iop/`.

## What lives here

| area | files |
|---|---|
| core | `imageio_core.{c,h}` |
| decoders / encoders | `imageio_{jpeg,png,tiff,pnm,rgbe,j2k,avif,heif,exr,gm,im,dng,pfm,libraw,rawspeed,qoi,webp}.*` |
| module APIs | `format/`, `storage/` |

## Rules

**`imageio_core.h`, never `imageio.h`.** The file was renamed for a reason: macOS ships
`<ImageIO/ImageIO.h>`, and on a case-insensitive filesystem a header named `imageio.h` in the
include path **shadows the Apple framework**, breaking the macOS build in a way that
reproduces nowhere else. That bug cost a bisect. Do not reintroduce the name.

**The `*_api.h` headers are X-macro headers.** `format/imageio_format_api.h` and
`storage/imageio_storage_api.h` have **no include guard**: they are re-included several times
per translation unit with different macros defined, and expand *inside struct bodies* (see
`dt_imageio_module_format_t` in `imageio_module.h`).

Consequently an `#include` at the top level of one would land inside a struct. The rule is
therefore precise rather than absolute:

* Real `#include`s must sit inside the `#ifdef FULL_API_H` block. That macro is defined only
  in full-API mode; the struct-body expansion defines `INCLUDE_API_FROM_MODULE_H` instead, so
  the block is skipped and nothing lands in the struct.
* Only *other X-macro headers* may be included unguarded — `common/module_api.h` is, and it
  has no includes of its own.

Symbols used outside that block are the consuming `.c` file's responsibility. See `CLAUDE.md`.

**Codecs are leaves.** A decoder converts bytes to pixels. It does not touch the database,
the history stack, or the GUI.
