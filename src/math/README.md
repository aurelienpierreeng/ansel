# `src/math/` — arithmetic, algebra, solvers

Pure mathematics on numbers. No images, no files, no user, no GUI.

Layer **1**, alongside `common/`.

## What lives here

| area | files |
|---|---|
| scalar / vector helpers | `math.h`, `openmp_maths.h` |
| matrices | `matrices.h` |
| linear solvers | `gaussian_elimination.h`, `QR_decomp.h`, `choleski.h`, `sparse_cholesky.{h,_cl.h}`, `svd.h` |
| decompositions & optimisation | `polar_decomposition.h`, `nelder_mead_simplex.h` |
| geometry | `homography.{c,h}` |
| archived | `attic/` |

## Rules

**No reporting, no allocation policy.** A solver returns an error code; it does not call
`dt_control_log()` to tell the user, and it does not decide how memory is obtained. Both were
removed from `choleski.h` and `sparse_cholesky.h` for exactly this reason — the caller knows
what a failure means, the solver does not.

**`attic/` is dead code, kept for reference.** Nothing builds it. Do not repair it, do not
extend it, do not include from it.

## Known overlap, unresolved

`system/simd.h` holds `dt_mat3x4_mul_vec4` — a matrix operation living among load/store
primitives — while `matrices.h` and `math.h` hand-roll intrinsics instead of using those
primitives. `common/colormatrices.c` is a third maths library still filed under `common/`.
Unifying the three is outstanding; see `doc/include-hygiene-roadmap.md`.
