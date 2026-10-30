# `src/apps/` — the executables

One directory per program, **named after the binary it produces**.

| directory | binary | built when |
|---|---|---|
| `ansel/` | `ansel` — the application | always |
| `ansel-cli/` | `ansel-cli` — headless export | always |
| `ansel-generate-cache/` | `ansel-generate-cache` — thumbnail pre-rendering | always |
| `ansel-cltest/` | `ansel-cltest` — OpenCL diagnostics | `USE_OPENCL` |
| `ansel-nn-parity/` | `ansel-nn-parity` — torch/CPU/OpenCL parity of the `.anselnn` executor | `USE_OPENCL` |
| `ansel-lens-db-update/` | `ansel-lens-db-update` — rebuilds `lenses.db` from this machine's lensfun profiles | liblensfun found |
| `ansel-cmstest/` | `ansel-cmstest` — colour-management diagnostics | `BUILD_CMSTEST` |
| `ansel-chart/` | *(none — see below)* | never |

A binary gated on a build option is absent from a package built without it, and that is the
only legitimate reason for one to be missing: **packaging must never enumerate this table.**
`make install` puts every one of them in `bin/`, CPack ships the `DTApplication` component
whole, and `packaging/macosx/3_make_hb_ansel_package.sh` copies `bin/` as it finds it.

Layer **10** — above everything, including the orchestrator. Each program's `main.c`
includes `darktable.h` and calls into the library; nothing depends on `apps/`.

## Rules

**`main.c` only.** A program's entry point sets up arguments and calls the library. Anything
with logic worth testing belongs in a subsystem, not here.

**`src/darktable.{c,h}` is NOT an app.** It is the orchestrator *library* that every one of
these executables links, and it lives at `src/`. `apps/ansel/main.c` is only the entry point
that calls `dt_init()`.

**The source lists are `FILE(GLOB)` patterns.** An entry matching no file is dropped with no
configure error, so a wrong path here does not fail the build — it silently drops the file
and surfaces as an undefined reference at link, or not at all. Check paths by hand.

**Generated headers are two levels up.** Sub-`CMakeLists.txt` reference
`${CMAKE_CURRENT_BINARY_DIR}/../../` for `version_gen.c` and the generated headers, because
`apps/<name>/` is one deeper than the old layout.

## `ansel-chart` is dead

No build target compiles it. The chart tool was dropped; the one file still live — the
homography solver — is now `src/math/homography.{c,h}`. The rest stopped compiling some time
ago (`DT_GUI_BOX_SPACING` undeclared, `dt_Lab_to_prophotorgb` implicit) and is excluded from
SonarCloud analysis. Kept for reference only; delete rather than repair.
