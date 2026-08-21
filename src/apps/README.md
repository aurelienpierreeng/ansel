# `src/apps/` — the executables

One directory per program, **named after the binary it produces**.

| directory | binary |
|---|---|
| `ansel/` | `ansel` — the application |
| `ansel-cli/` | `ansel-cli` — headless export |
| `ansel-cltest/` | `ansel-cltest` — OpenCL diagnostics |
| `ansel-cmstest/` | `ansel-cmstest` — colour-management diagnostics |
| `ansel-generate-cache/` | `ansel-generate-cache` — thumbnail pre-rendering |
| `ansel-chart/` | *(none — see below)* |

Layer **10** — above everything, including the orchestrator. Each program's `main.c`
includes `darktable.h` and calls into the library; nothing depends on `apps/`.

## Rules

**`main.c` only.** A program's entry point sets up arguments and calls the library. Anything
with logic worth testing belongs in a subsystem, not here.

**`src/darktable.{c,h}` is NOT an app.** It is the orchestrator *library* that all five
executables link, and it lives at `src/`. `apps/ansel/main.c` is only the entry point that
calls `dt_init()`.

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
