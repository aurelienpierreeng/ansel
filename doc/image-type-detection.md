# Image type detection & the early-pipeline contract

## Why this exists

The "type" of an input image — what early decoding it needs (demosaic? black/white
normalization? a camera input profile?) — drives which modules auto-enable, what their default
parameters are, and which buffer format flows out of the first pipeline stages. Historically
this was encoded ad-hoc and discovered too late, which is the root cause behind issues #77
(monochrome raws not treated as raw), #729 (linear/DxO DNGs rendering green), #733 (a module
wrongly disabled on an "unexpected input buffer format") and #849 (the DNG master issue: a DNG
can be almost anything).

A DNG is the stress test because the same `.dng` extension covers mosaiced raw, already
demosaiced *linear DNG / sRAW*, 16-bit or float, monochrome or color, with or without an
embedded matrix or gain map.

## Two places type lives

1. **`img->flags`** (`dt_image_flags_t` in `common/image.h`) — coarse, integer, **persisted in
   the database**. Set provisionally from the file extension at import
   (`dt_imageio_get_type_from_extension()`), then authoritatively by the codecs at decode.
2. **`img->dsc`** (`dt_iop_buffer_dsc_t`) — the precise buffer descriptor (channels, datatype,
   `filters`, colorspace) that actually drives the pipeline. Populated **only** by codecs at
   decode time and **never stored in the database**.

Because `dsc` is not in the database, a `dt_image_t` loaded for an image on remote/unplugged
storage is incomplete until it is decoded. The classification therefore has to be derivable
from `flags` alone, with `dsc` only refining it.

## Two-phase lifecycle: provisional → resolved

| phase | when | source | what is known |
|-------|------|--------|----------------|
| **provisional** | image instantiated from DB / import, not yet decoded | file extension → `flags` | raw vs raster, and LDR/HDR **only for unambiguous containers** |
| **resolved** | after the first successful `dt_imageio_open()` | decoded `dsc` → `flags` | exact class incl. mosaic vs linear-raw and real dynamic range |

The extension is a weak signal and `dt_imageio_get_type_from_extension()` is deliberately
conservative about it. It commits a dynamic-range flag only for containers whose sample format is
fixed — integer-only (`jpg`, `png`, `gif`, `webp`, `pnm`…) → `LDR`, float-only (`exr`, `hdr`,
`pfm`) → `HDR`. Containers that can hold either — **TIFF** (8/16-bit int *or* 16/32-bit float),
**AVIF / HEIF / HEIC** (usually 8–12-bit SDR, sometimes PQ/HLG HDR) and **DNG** — return `0`
(unknown) and carry no LDR/HDR flag until decoded. Trying to guess their type from the extension was
brittle and wrong (a float TIFF mislabelled LDR, an SDR HEIF mislabelled HDR); the datatype check in
`dt_image_buffer_resolve_flags()` settles them authoritatively. Note this is distinct from decoder
*routing* (`raster_formats[]` / `hdr_formats[]` in `imageio.c`), which still lists those formats so
the correct codec runs.

- At DB read (`image_cache.c`), `dt_image_set_provisional_dsc()` seeds a placeholder `img->dsc`
  from the extension-derived class so the image can be reasoned about and the first pipeline
  stage has a usable contract before any decode.
- `dt_exif_read()` runs before the buffer is decoded — sometimes on a freshly `dt_image_init()`'d
  object (import preview, path-pattern expansion). Because it probes `dt_image_is_ldr()` /
  `dt_image_is_hdr()` while decoding the EXIF, it seeds the same extension-derived provisional flag
  first (only when nothing is classified yet), so those predicates are meaningful at that point.
  `dt_image_buffer_resolve_flags()` later overwrites it with the datatype-derived truth.
- At the end of `dt_imageio_open()` (the single decode chokepoint),
  `dt_image_buffer_resolve_flags()` maps the freshly decoded descriptor to the persisted flags and
  sets `DT_IMAGE_BUFFER_RESOLVED`:
  - `dsc.filters != 0` → `DT_IMAGE_MOSAIC` (the demosaic axis);
  - `dsc.datatype == TYPE_FLOAT` → `DT_IMAGE_HDR` (a 16- or 32-bit float buffer, both decoded into
    `TYPE_FLOAT` in RAM, carries high dynamic range); an integer **non-raw** buffer → `DT_IMAGE_LDR`;
    integer raw is neither. This replaces the old per-codec / filename-extension HDR/LDR guessing
    with the authoritative datatype actually loaded.

  These bits round-trip to the database through the regular image-cache writeback
  (`mipmap_cache.c`), so later sessions know the precise class **without re-decoding**.

The two persisted bits added for this:

- `DT_IMAGE_MOSAIC` (`1 << 21`) — the decoded buffer carries a CFA mosaic (`dsc.filters != 0`).
  This is the missing axis (issue #77): it separates a *mosaiced raw* from an *already
  demosaiced raw* (sRAW / linear DNG) without needing `dsc.filters`, which the DB does not store.
- `DT_IMAGE_BUFFER_RESOLVED` (`1 << 22`) — the codec has decoded the file at least once, so the
  mosaic bit and `dsc` are authoritative rather than a provisional extension guess.

## The classification API

`dt_image_pipe_class(img)` returns one mutually-exclusive `dt_image_pipe_class_t`:

| class | colorimetry | mosaiced | needs rawprepare | needs demosaic | typical files |
|-------|-------------|----------|------------------|----------------|---------------|
| `MOSAIC_RAW` | raw | yes | yes | yes | OOC sensor raws, mosaiced DNG, mono-bayer |
| `LINEAR_RAW` | raw | no | yes | no | sRAW, DxO/linear DNG, mono raw already demosaiced |
| `RGB_LDR` | display | no | no | no | jpg, png, tiff8 |
| `RGB_HDR` | scene/linear | no | no | no | exr, pfm, hdr, float tiff |
| `UNKNOWN` | — | — | — | — | undecoded with no extension hint |

Decision order in `dt_image_pipe_class()` (`common/image.c`):

1. resolved + raw colorimetry → `MOSAIC_RAW` if `DT_IMAGE_MOSAIC` else `LINEAR_RAW`;
2. `DT_IMAGE_LDR` → `RGB_LDR`; `DT_IMAGE_HDR` → `RGB_HDR`
   (tested *after* raw colorimetry so a float mosaiced raw, which is flagged both `RAW` and
   `HDR`, is not mistaken for an RGB HDR file);
3. provisional raw colorimetry → `MOSAIC_RAW` (best guess; corrected on first decode);
4. otherwise `UNKNOWN`.

Alongside the class, **orthogonal predicates** each test exactly one independent fact — no
overlapping conditions, **no filename-extension sniffing**:

- `dt_image_needs_rawprepare(img)` — raw colorimetry (`RAW | S_RAW`).
- `dt_image_needs_demosaic(img)` — class is `MOSAIC_RAW`.
- `dt_image_is_mosaiced(img)` — `DT_IMAGE_MOSAIC` (authoritative only once resolved).
- `dt_image_is_sraw(img)` — `DT_IMAGE_S_RAW`.
- `dt_image_is_monochrome(img)` — monochrome flags (unchanged).
- `dt_image_is_matrix_correction_supported(img)` — raw colorimetry (mosaiced *or* linear) that
  is not monochrome and carries a usable camera matrix. This drives white balance, the color
  calibration "as shot in camera" illuminant and the colorin input matrix. It deliberately does
  **not** look at the mosaic axis, so an already-demosaiced raw (sRAW / linear DNG) still gets
  matrix correction (issue #729).

- `dt_image_is_hdr(img)` / `dt_image_is_ldr(img)` — flag-only tests of `DT_IMAGE_HDR` /
  `DT_IMAGE_LDR`. The filename sniffing they used to do is gone; the flags are now set from the
  **decoded buffer datatype** (see below), so they report what was actually loaded rather than what
  the extension suggested. Kept as API so callers don't open-code the bitmask test. A float
  *mosaiced* raw is flagged both `RAW` and `HDR`, so the class decision tests raw colorimetry
  *before* HDR (see the decision order above).

## Debugging

`dt_image_print_debug_info(img, context)` prints the flags, the decoded `dsc`, and a
`class=… state=provisional|resolved …` line under `DT_DEBUG_IMAGEIO`. Useful invocations:

```
ansel -d imageio   # type detection: provisional at import, resolved after first decode
ansel -d history   # how the class drives history init / module auto-enable
ansel -d pipe      # per-node dsc-in / dsc-out propagation + module format heuristics
ansel -d nan       # flags the first module whose output contains NaN/Inf
```

The lighttable/darkroom "EXIF and IPTC" panel exposes a *pipeline type* row for quick visual
confirmation, including an `(unclassified)` marker before the first decode.

### Tracing module heuristics: `[iop-fmt]` and `[dsc]`

Reverse-engineering "which module silently mishandled this image" from pixel output alone is not
viable. Two complementary traces on the `-d pipe` channel make the decisions explicit:

- **`[iop-fmt]`** — emitted by the `dt_iop_fmt_log(module, fmt, …)` macro
  (`develop/imageop.h`). Call it at *every* spot where a module branches on the image or buffer
  type — self enable/disable, default params, code-path or descriptor choice. Each migrated
  module logs its `reload_defaults` / `force_enable` / `commit_params` reasoning, e.g.

  ```
  [iop-fmt] demosaic   reload_defaults: class=mosaic-raw needs_demosaic=1 filters=… -> default_enabled=1
  [iop-fmt] temperature commit: class=mosaic-raw coeffs=[…] g2_in=nan g2_usable=0 -> enabled=1
  ```

- **`[dsc]`** — the per-node buffer-format contract printed during the format-propagation pass
  (`dev_pixelpipe.c`). It now includes **`cst`** (colorspace) in and out:

  ```
  [dsc] module=colorbalance enabled=1 in=(cst=lab ch=4 bpp=16 …) out=(cst=lab …)
  ```

  Watching `cst` is the fastest way to catch a Lab-domain module whose input was not converted
  (see "The colorspace contract" below): a Lab module that prints `in=(cst=rgb …)` is broken.

Diffing the `[iop-fmt]` / `[dsc]` lines of a working vs. broken render (ideally duplicates of the
same raw with different histories) localizes a regression to a single module and decision in one
pass — this is how #729 (a `nan` second-green WB coefficient) and the Lab-conversion regression
below were both found.

## Module enable discipline: `reload_defaults` / `force_enable` / `commit_params`

A module's "does this image support me" decision must be taken **as early as possible** — at the
history level, not on the pipeline node — so the answer is consistent, gives GUI feedback and is
sanitized into the history. The three hooks have distinct, non-overlapping roles:

| hook | runs when | scope | use it for |
|------|-----------|-------|-----------|
| `reload_defaults()` | building a fresh history / refreshing GUI | sets `default_enabled` and `hide_enable_button` | the default on/off state and whether the button is shown for this image type |
| `force_enable()` | **every** history read (load, style, copy-paste) | clamps the proposed `enabled` state | sanitizing a history applied across image types (e.g. a raw-only module pasted onto a JPEG) — decisions that rely **only on image metadata** |
| `commit_params()` | per pipeline node, every render | last-resort, no history write, no GUI feedback | decisions that need **runtime context** only (param values, ROI), or modules that keep no history |

Rules of thumb, applied across the decoding modules:

- If `reload_defaults()` and `force_enable()` would compute the same "supported?" predicate,
  factor it into one shared helper (e.g. `_cacorrect_supported(img)`,
  `_hotpixels_supported(img)`, highlights' `enable(img)`) so the two can never drift.
- A module that self-disabled on image type inside `commit_params()` was doing it in the wrong
  place: it gives no GUI feedback and does not sanitize the history. Move the metadata test to
  `force_enable()` (+ `reload_defaults()`) and leave only genuine runtime checks in
  `commit_params()`. Examples kept in `commit_params()`: hotpixels' `strength == 0` no-op,
  rawprepare's OpenCL-readiness gate.
- The CFA-domain modules (`hotpixels`, `cacorrect`, `rawdenoise`) gate on
  `dt_image_needs_demosaic()` (the mosaic axis). The raw-colorimetry modules (`highlights`,
  `rawprepare`, scene-referred `filmicrgb` defaults, `exposure` deflicker) gate on
  `dt_image_needs_rawprepare()` so they also cover sRAW / linear DNG.

## The colorspace contract: `dsc_in.cst` and automatic Lab↔RGB conversion

Many legacy modules work in CIE Lab (`default_colorspace() == IOP_CS_LAB`: old *color balance*,
*color checker*, *atrous*, *bilat*, …) even though the working pipe is RGB. They are **not**
expected to convert their own input: the pixelpipe inserts the conversion automatically. In
`pixelpipe_cpu.c` / `pixelpipe_gpu.c` the rule is, in essence:

```c
if(process_input_dsc.cst != piece->dsc_in.cst        // the buffer's cst differs from
   && !(is_rgb(process_input_dsc.cst)                // what the module declared it wants
        && is_rgb(piece->dsc_in.cst)))               // (RGB→RGB needs no transform)
  dt_ioppr_transform_image_colorspace(... process_input_dsc.cst -> piece->dsc_in.cst ...);
```

The whole mechanism therefore hinges on **`piece->dsc_in.cst` carrying the colorspace the module
actually consumes** — `IOP_CS_LAB` for a Lab module — which is what `input_format()` (→
`default_input_format()` → `default_colorspace()`) sets.

> **Invariant — never publish the upstream descriptor as a node's `dsc_in`.**
> `dsc_in` must be the module's *declared* input descriptor (from `input_format()`), which already
> inherits the upstream runtime fields (`processed_maximum`, `rawprepare`, `temperature`, CFA
> phase) because it is seeded from the upstream descriptor before `input_format()` runs. Copying the
> raw upstream descriptor instead silently resets `dsc_in.cst` to the upstream colorspace.

Violating that invariant caused a regression worth recording. The incremental sync path
(`_sync_pipe_nodes_from_history_from_node()` in `dev_pixelpipe.c`, used on `DT_DEV_PIPE_TOP_CHANGED`)
computed the declared input into a scratch descriptor for its mismatch check, then on the
compatible path published the **upstream** descriptor as `dsc_in`. For a Lab module this reset
`dsc_in.cst` from `lab` back to the upstream `rgb`, so `process_input_dsc.cst == piece->dsc_in.cst`
and the auto-conversion was skipped: the Lab module consumed RGB bytes as Lab and produced
garbled or solid-colour output. Because only the top-changed path was affected (the full resync
in `_prepare_piece_input_contract()` always sets `dsc_in` from `input_format()`), the bug appeared
*intermittently* — duplicates of one raw with different histories rendered correctly or not
depending on which sync path their last edit took. The fix publishes the declared descriptor;
the `cst` column added to the `[dsc]` log makes a recurrence a one-line diff.

## White balance: the second-green (4-colour) coefficient

`img->wb_coeffs[3]` is the **second green** multiplier. It is only meaningful for true 4-colour
sensors (`DT_IMAGE_4BAYER`, e.g. CYGM / RGBE). For ordinary RGGB Bayer and X-Trans both green
sites share the *first* green's multiplier, and decoders legitimately leave `wb_coeffs[3]` as
`NaN` or `0` (RawSpeed's DNG decoder fills only three planes; `dt_image_init()` defaults the slot
to `NaN`). `find_coeffs()` in `temperature.c` even notes "the fourth is usually NAN for RGB".

A `NaN`/`0`/`Inf` fourth coefficient must therefore never reach the pixels: in a RGGB mosaic it
multiplies the G2 site (Bayer index 3 via `FC()`), so a single bad value poisons half the green
channel → green or, after clamping, black output (issue #729's second face). The convention is to
**mirror the first green** for non-4-colour sensors:

- `temperature.c` — `find_coeffs()` sets `coeffs[3] = coeffs[1]` when the raw value is not normal,
  and `commit_params()` guards `d->coeffs[3] = isnormal(p->g2) ? p->g2 : p->green` so histories
  that already stored `g2 = NaN` are rescued without re-initialisation.
- `channelmixerrgb.c` — `get_white_balance_coeff()` forces the bogus matrix-derived 4th
  coefficient to the green reference instead of propagating it.

## Roadmap (staged)

- **A (done)** — the canonical API, the provisional→resolved lifecycle, the persisted
  bits, the provisional `dsc` seeding, the debug line, and this document. No pipeline behaviour
  change yet; legacy predicates remain as shims.
- **B (done)** — the decoding modules and history init/sanitization now key on the canonical
  predicates:
  - `demosaic` self-enables on `dt_image_needs_demosaic()` (the mosaic axis) in both
    `reload_defaults` and `force_enable`, so they agree — already-demosaiced sRAW / linear DNG is
    no longer demosaiced and monochrome Bayer still is (#77).
  - `dt_image_is_matrix_correction_supported()` dropped the `S_RAW && filters == 0 → FALSE`
    exclusion, so white balance, color calibration and the colorin input matrix re-enable for
    linear DNGs (#729). `temperature`'s `prepare_matrices()` likewise gates on
    `dt_image_needs_rawprepare()` instead of the mosaic-only RAW flag, so it uses the camera
    matrix rather than assuming sRGB.
  - `rawprepare` gates on `dt_image_needs_rawprepare()`, and `dev_history.c` auto-preset/sanitize
    picks the scene-referred workflow and `FOR_RAW` input format for linear DNGs through the same
    predicates. (`dt_image_is_rawprepare_supported()` has since been removed — see stage D.)
- **C (done)** — `dev_pixelpipe.c` now derives the buffer-format contract in a single
  direction-independent forward pass, `_propagate_pipe_formats()`, run once after every sync path
  (full / top-only / realtime-in-place) in `dt_dev_pixelpipe_change()`, before the OpenCL cache
  seal and ROI planning. The per-node history-commit loop only sets `dsc_in` (needed by some
  `commit_params()`) and no longer auto-disables on a possibly-stale upstream descriptor; the
  authoritative pass is the *only* place a module is disabled for an incompatible input, taken
  once on the fully-threaded consistent chain (fixes the spurious "unexpected input buffer
  format" disable, #733). Only the first stage (`basebuffer`) reads the input image type; every
  later node derives its contract from its upstream piece, and rawprepare's CFA *phase* shift
  remains the lone ROI-dependent refinement in `modify_roi_*()`.
- **D (done)** — full migration of the IOP modules off the overlapping legacy predicates onto the
  canonical API, with tracing and enable-discipline cleanup:
  - The CFA-domain modules (`hotpixels`, `cacorrect`, `rawdenoise`) now gate on
    `dt_image_needs_demosaic()` and the raw-colorimetry modules (`highlights`, `filmicrgb`
    scene-referred defaults, `exposure` deflicker, `ashift` structure) on
    `dt_image_needs_rawprepare()` — replacing the ambiguous `dt_image_is_raw()` that conflated the
    two axes and wrongly enabled CFA operations on already-demosaiced linear DNGs.
  - `dt_image_is_rawprepare_supported()` removed; all callers use `dt_image_needs_rawprepare()`.
  - Each module's image-type decision was flattened into `reload_defaults()` + `force_enable()`
    sharing one helper, with `commit_params()` reduced to runtime-only checks — see *Module enable
    discipline* above.
  - `dt_iop_fmt_log()` instrumentation added across the decoding/colour modules, and `cst` added to
    the `[dsc]` log — see *Debugging*.
  - White-balance second-green and the Lab-conversion contract regression fixed — see the two
    sections above.
  - `dt_image_is_ldr()` / `dt_image_is_hdr()` keep their API but drop the filename sniffing: the
    flags are now set from the decoded buffer datatype (float → HDR, integer non-raw → LDR) in
    `dt_image_buffer_resolve_flags()`. No filename-extension sniffing remains in any type-detection
    predicate.
