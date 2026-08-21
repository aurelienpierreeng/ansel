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
| curve interpolation | `splines.{cpp,h}` |
| graphs | `topological_sort.{c,h}` |
| expression evaluation | `calculator.{c,h}` |
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
primitives — while `math.h` hand-rolls `__m128` intrinsics instead of using them, and
`matrices.h` uses neither: the `mat3SSEinv` / `mat3SSEmul` / `transpose_3xSSE` names are
historical, the bodies are plain loops over the padded `dt_colormatrix_t`, and its
`dot_product` is `dt_mat3x4_mul_vec4` written a second time.

The general 3×3 inverse is not here at all. `mat3inv()` / `mat3inv_float()` are exported from
`colorprofiles/colorspaces.h`, with the same body as `matrices.h`'s `mat3SSEinv` over a
different storage layout (9 contiguous floats, not a padded 4×4), plus a `double` variant the
same macro generates but does not export. Only one of its two callers is colour code
(`imageio/imageio_rgbe.c`, inverting an RGB→XYZ primaries matrix); the other, `iop/ashift.c`,
inverts a homography. Do not add a further copy here; if you need one, move it.

Unifying these is outstanding; see `doc/include-hygiene-roadmap.md`.
