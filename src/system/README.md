# `src/system/` — the machine

Code that describes, interrogates or adapts to **the hardware and the platform**. If it
answers "what is this computer, and what can it do?", it belongs here. If it answers
anything about photographs, it does not.

Layer **0** — the bottom. Everything may depend on `system/`; `system/` should depend on
as little as possible.

## What lives here

| area | files |
|---|---|
| CPU instruction sets | `simd.h`, `target_clones.h`, `openmp.h`, `ppc64le/altivec.h` |
| platform validation | `is_supported_platform.h` |
| memory substrate | `mem_alloc.h`, `memory_arena.{c,h}`, `atomic.{c,h}`, `fp_mode.h` |
| machine budgets | `sys_resources.h` |
| runtime capabilities | `capabilities.h` |
| GPU / display hardware | `nvidia_gpus.h`, `opencl_drivers_blacklist.h`, `display_profile.{c,h}` |

## Rules

**External libraries are fine; higher layers are not.** `display_profile.c` uses GDK to ask
which monitor a window is on, and `nvidia_gpus.h` knows about OpenCL. Both are *external*
dependencies. What must never appear is an `#include` from `common/`, `gui/`, `develop/` or
above — that would invert the layer order.

**Four such upward includes exist today and are known**: `memory_arena.{c,h}` and
`mem_alloc.h` reach into `common/` for `dtpthread.h`, `macros.h` and `logging.h`. They are
pre-existing coupling that only became visible when these files left `common/`. Do not add
more; `dtpthread` is itself a candidate to move down here.

**Header-only is common here.** Several of these (`sys_resources.h`, `capabilities.h`)
declare accessors whose storage is owned by the orchestrator, `src/darktable.c`. That is
deliberate: low-level compute units include a small header instead of the whole application.

**No guard, no include — five files.** Not applicable here, but see `CLAUDE.md`: the X-macro
API headers elsewhere in the tree have their own rules.

## What does NOT belong here

* Pixel maths — that is `src/math/` (arithmetic, matrices, solvers) or `src/pixel/`.
* Anything with an image, a file format, or a database row in it.
