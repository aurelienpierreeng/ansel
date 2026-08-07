# `src/pixel/` — pixel algorithms

Operations on buffers of pixels that are **not** tied to a specific IOP module: shared
filters, decompositions and colour primitives that several modules build on.

Layer **2** — above `common/` and `math/`, below `control/` and everything visual.

## Rules

**No module state, no pipeline knowledge.** Code here takes buffers and parameters and
returns buffers. It does not know what a `dt_iop_module_t` is, does not read history, and
does not decide when it runs — that is `develop/`'s job.

**No GUI, no database, no files.** A pixel algorithm that needs to report progress or ask a
question is in the wrong place, or needs its caller to do the asking.

**SIMD and alignment come from `system/`.** Use `system/simd.h` for the aligned pixel type
and load/store helpers rather than hand-rolling intrinsics, and `system/openmp.h` for the
pragma shorthands.

## Related

* `src/math/` — arithmetic and solvers with no notion of a pixel.
* `src/iop/` — the modules themselves, with parameters, GUI and pipeline integration.
