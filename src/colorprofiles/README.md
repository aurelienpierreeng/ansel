# `src/colorprofiles/` — ICC colour profiles

Everything LittleCMS2: which profiles exist, what they are called, and how to apply one to
pixels. The module owns its state and answers questions about it — nothing outside
`src/colorprofiles/` names the profile list, its rwlock or its cached transforms.

Contents:

| file                | what it does                                                     |
|---------------------|------------------------------------------------------------------|
| `profile_types.h`   | the vocabulary and nothing else: the profile-type, intent, colour-mode and direction enums. Pulls in neither `<lcms2.h>` nor `<pthread.h>` |
| `colorspaces.c/h`   | the module API and the module's state: building, opening and naming ICC profiles, the profile list, the display/soft-proof settings, and the prepared display transforms |
| `colormatrices.c`   | 102 camera colour-matrix presets, `#include`d as *data* by `colorspaces.c` and `iop/colorin.c` |
| `iop_profile.c/h`   | the derived form of a profile — two 3×3 matrices and six 65536-entry tone-curve LUTs — and the pixel loops over it, SIMD and OpenCL |
| `printprof.c/h`     | the one-shot conversion of an exported buffer into the printer's profile, for the print job |

## CRUDE on metadata, Lock and Apply on data

The API is split in two halves, and that split is the design.

**CRUDE (metadata).** `dt_colorspaces_enumerate_profiles()`, `dt_colorspaces_profile_index()`,
`dt_colorspaces_profile_at()` and `dt_colorspaces_profile_exists()` answer questions *about* a
profile — `{type, filename, name}` for a `direction` — and answer them with **value copies**. No
lcms2 type crosses, no caller walks the list, no lock is taken: the list is built once at init
and the one datum that mutates afterwards is not something these read.

**Lock and Apply (data).** `dt_colorspaces_lock_profiles()` / `dt_colorspaces_unlock_profiles()`
pin the profile handles while a caller derives from one (`cmsGetColorSpace`, `cmsCreateTransform`,
matrix extraction). `dt_colorspaces_apply_profile()` and its siblings in `iop_profile.h` run the
pixel loop themselves, branching *internally* between the vectorised matrix + LUT path and the
lcms2 fallback, so callers neither choose nor see which one ran.

Lifetime here is answered by a lock, not by a copy, and that is a measurement rather than a
taste: **there is no `cmsDupProfile` in lcms2.** The only true deep copy is serialise-and-reopen
— about 0.005 ms for a built-in, but 1.02 ms for a real colord display profile — and copying a
prepared `cmsHTRANSFORM` means rebuilding it, 2.2 to 38 ms, with nothing to amortise it against.
The module's four *prepared* display transforms therefore never leave it; the two
`dt_colorspaces_transform_rgba_float_*()` helpers take a transform the **caller** built and owns,
which is a different thing.

## What deliberately did NOT come here

**`develop/iop_profile.c/h` — the pipeline-facing half.** It resolves which profile a module,
pipe or export should use. Everything it declares takes a `develop/` type (`dt_develop_t`,
`dt_dev_pixelpipe_t`, `dt_iop_module_t`), which is exactly why it sits at layer 5 and not here.

**`imageio/imageio_profile.c/h` — choosing a profile *for an image*.** Reading the ICC a JPEG
embeds, mapping an AVIF's CICP block, falling back to a RAW's camera matrix: codec work, not
colour management, and the only reason this code would ever include codec headers.

## Layer 1

That is not a preference — measured with `tools/include_graph.py --what-if`, layer 2 costs
**+6** violations and layer 0 costs **+13**.

The cap is five layer-1 headers reaching in — `common/cups_print.h`, `common/exif.h` and
`common/mipmap_cache.h` for the enums in `profile_types.h`, `common/color_picker.h` and
`common/histogram.h` for the derived-profile struct in `iop_profile.h`. The floor is
`colorspaces.c` needing `common/conf.h`, `common/file_location.h`, `common/utility.h` and
`common/debug.h`.

`iop_profile.h` could not simply move up to `develop/` with the rest of the resolution logic
because `pixel/rgb_norms.h` and `pixel/colorequal_shared.h` (layer 2) consume it. It is also
where both of the module's remaining outward violations live: `iop_profile.h` takes
`pixel/format.h` for `dt_iop_colorspace_type_t`, and `iop_profile.c` takes
`develop/imageop_math.h` for `dt_iop_estimate_exp()` — pure curve fitting, misfiled at layer 5.

**Include the smallest header that answers your question.** A translation unit that only needs a
profile type to store in its params or an intent to pass along wants `profile_types.h`;
`colorspaces.h` drags `<lcms2.h>` and `<pthread.h>` in behind it. 42 files include
`colorspaces.h`, 12 include `profile_types.h`, and 255 of 425 translation units see
`<lcms2.h>` transitively.

## State

This directory is **not** stateless and is not meant to be. Two of its three translation units
hold state, and none of the three reaches another module's (`tools/statelessness_audit.py --dir
src/colorprofiles`).

**`colorspaces.c` — the profile list.** Built once by `dt_colorprofiles_init()`, torn down by
`dt_colorprofiles_cleanup()`, and **never appended to at runtime**: registration order is what
enumeration reproduces and what every stored combo index in every preset and conf key refers to.
Only three things mutate after init:

- the `DT_COLORSPACE_DISPLAY` entry's `cmsHPROFILE`;
- the four prepared display transforms derived from it — this and the above under `xprofile_lock`;
- the seven-field settings group (display triple, soft-proof pair, colour mode), under a
  separate lock private to the file.

Read the settings with `dt_colorprofiles_get_settings()`, which copies all seven under one lock,
and write them through the setters. Reading the fields one at a time lets a reader pair a new
profile type with the previous filename, and a 512-byte filename read while `g_strlcpy()` is
writing it is a **torn** string, not merely a stale one. A module that snapshots the group for
its cache hash must then render from that same snapshot, not re-read the live state from
`process()`.

**Lock order, where both are involved: `xprofile_lock` OUTER, the settings lock INNER.** The
display setters need both, because changing the display profile identity also rebuilds the four
transforms. Nothing takes them the other way round.

**`iop_profile.c` — the derived-profile memo.** Extracting a profile's matrices and LUTs is two
65536-entry passes and is a pure function of `(type, filename)`, so `dt_colorspaces_add_profile()`
memoises it process-wide under its own mutex. It returns a pointer the **module** owns, valid
until `dt_colorspaces_flush_profile_memo()` (or, for `DT_COLORSPACE_DISPLAY`,
`dt_colorspaces_invalidate_display_profile_memo()`).

A profile belonging to one image goes in `dt_image_t.embedded_profile`, not in the list; see
CLAUDE.md for what happened the last time something appended to the shared list at runtime.

## Traps

**Three registered entries have `profile == NULL`.** Of the 21 built-in registrations,
`DT_COLORSPACE_WORK`, `DT_COLORSPACE_EXPORT` and `DT_COLORSPACE_SOFTPROOF` name a user *setting*
rather than a colour space, and exist only to occupy a combo row. Roughly 40 call sites
dereference `->profile` without a NULL check; what keeps them safe is that lookup tests
`in_pos`/`out_pos`/`work_pos`/`display_pos` only and **never** `category_pos`, so a category
entry can never be returned. Do not "fix" the lookup predicate to consult `category_pos`, and do
not give categories a role of their own, without auditing those sites first.

**`DT_COLORSPACE_SRGB` is registered twice** — a v4 parametric-curve entry valid only as INPUT,
and a v2 point-TRC entry carrying output/monitor/working — and the two are distinguished by
nothing but which `*_pos` is −1. The role argument is what picks between them, so it can never be
omitted or approximated: a multi-bit mask resolves to the *first* match in registration order.
Ask for a working profile as `DT_PROFILE_ROLE_WORKING`, not as `DT_PROFILE_ROLE_ANY`.

**These are roles, not directions.** The enum used to be called
`dt_colorspaces_profile_direction_t`, which it never was: a profile is RGB→PCS or PCS→RGB and
nothing else. What the bits select is which *menu* an entry appears in, and the menus really do
differ — `DT_PROFILE_ROLE_MONITOR` diverges from `DT_PROFILE_ROLE_OUTPUT` on 5 of the 21
built-ins, so substituting one for the other is a behaviour change, not a rename. Two further
bits, `CATEGORY` and `DISPLAY2`, were declared and never tested by any lookup — one had a
position field no predicate consulted, the other had no field at all — and are gone.

**Image-derived profiles are not in the list.** `DT_COLORSPACE_EMBEDDED_ICC` through
`DT_COLORSPACE_ALTERNATE_MATRIX` (9..14) describe one image: their matrices come from that
image's own camera data via `iop/colorin.c`. They cannot be resolved by identity and must not be
memoised — a `(type, "")` key would be shared by every image of the same camera-matrix kind — so
the pipe that built one owns it (`dt_dev_pixelpipe_t.owned_input_profile_info`).

**`dt_iop_order_iccprofile_info_t` is ~1.5 MB and must stay sole-owned.** Its six eagerly
allocated 65536-float LUTs are pointers, and `develop/blend.c` shallow-`memcpy`s the struct,
aliasing all six.

## Gates

- `tools/check_module_boundaries.sh` — ratchets the module closed: external
  `dt_colorspaces_get_global()` and external `xprofile_lock` acquisitions, both baseline **0**.
  A count that rises fails; a count that falls must lower the baseline in the same commit.
- `tools/statelessness_audit.py --dir src/colorprofiles` — what holds state and what reaches it.
- `tools/check_export_pixels.sh <ref-a> <ref-b>` — decodes both exports and compares the pixel
  arrays, not the file bytes (the PNG carries the build's version string in its metadata). The
  standing regression check for anything touching colour management, where "it still runs" is a
  very low bar and a one-LSB hue shift is the actual failure mode.
