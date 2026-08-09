# `src/colorprofiles/` — ICC colour profiles

LittleCMS2 work: building, loading and describing ICC profiles. Not pixel processing — a
profile is a description of a colour space, and applying one to an image is somebody else's
job.

Contents:

| file                | what it does                                                     |
|---------------------|------------------------------------------------------------------|
| `colorspaces.c/h`   | the LittleCMS2 work: building, opening, naming and applying ICC profiles, and the application-wide profile list |
| `colormatrices.c`   | 102 camera colour-matrix presets, `#include`d as data by `colorspaces.c` and `iop/colorin.c` |
| `printprof.c`       | resolves the printer/soft-proof profile for an output device      |

One more belongs here and is not yet moved: `pixel/iop_profile.c`, the pixel-level half of
profile handling — the SIMD/OpenCL transform engine. (The *pipeline*-facing half is
`develop/iop_profile.c`, which resolves which profile a module or pipe should use; it takes
develop/ types, stays at layer 5, and is not a candidate for this module.)

**What deliberately did NOT come here.** Choosing a profile *for an image* — reading the ICC a
JPEG embeds, mapping an AVIF's CICP block, falling back to a RAW's camera matrix — is codec
work, not colour management, and lives in `imageio/imageio_profile.c`. It was the only reason
this code ever included codec headers.

The module sits at **layer 1**. That is not a preference: three layer-1 headers include
`colorspaces.h` for an enum, which caps it, while `colorspaces.c` needs `common/conf.h` and
`common/file_location.h`, which floors it. Measured with `include_graph.py --what-if`, layer 2
costs +2 violations and layer 0 costs +10.

This directory is **not** stateless and is not meant to be: `colorspaces.c` owns the
application-wide profile list. That list is built once by `dt_colorspaces_init()` and must stay
read-only afterwards — ~23 places walk it without a lock. A profile belonging to one image goes
in `dt_image_t.embedded_profile`, not here; see CLAUDE.md for what happened the last time
something appended to it at runtime.

Measure with `tools/statelessness_audit.py --dir src/colorprofiles`.
