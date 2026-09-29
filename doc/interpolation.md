<!-- Provenance: every finding carries the commit it was established against. -->

# Pixel interpolation

> **Verified against `42eca0e8fe` on 2026-09-29.**
>
> Which resampling kernel the pipeline uses, why, and the one configuration trap that fails quietly.
>
> Each finding below is dated with the commit that established it. A finding is only as
> good as its hash: before acting on one older than the code you are changing, re-measure
> it — and re-date it here when you do. Where an earlier version of a claim was **wrong**,
> that is recorded rather than quietly corrected: how a claim was wrong is usually the more
> useful thing to know.

*Found `22f623c0be`, 2026-06-25.*

Mitchell-Netravali (B=C=1/3) is the pipeline interpolator. Lanczos has been removed entirely.
Rationale: Lanczos has large negative side-lobes → halos at high-contrast edges and pushes
premultiplied alpha out of [0,1]. Mitchell is near-halo-free (~3% residual undershoot), sharp,
and a separable partition-of-unity kernel that fits the existing tap machinery for CPU and GPU.

The pipeline's interpolation architecture in `src/pixel/interpolation.c` is separable — each
kernel registers a 1D `maketaps`, and both `dt_interpolation_resample` (CPU) and
`dt_interpolation_resample_cl` (GPU) consume the same CPU-computed taps. A new separable kernel
is automatically CPU+GPU.

The drawlayer brush matte still forces `BILINEAR` explicitly — premultiplied alpha wants strictly
zero overshoot.

Config option strings in `anselconfig.xml.in` MUST equal the kernel `.name` field exactly;
`USERPREF` resolves by strcmp. A mismatch (e.g. `"bicubic (Catmull-Rom)"` vs `"bicubic"`)
silently falls back to default instead of erroring.

## Two stale references to Lanczos remain in the tree

*Found `42eca0e8fe`, 2026-09-29 — during the verification pass that produced this file.*

Lanczos is gone from the enum, the kernels and the configuration, but two places still name it,
and one of them is a live defect rather than a stale comment:

- **`tools/benchmark_darkroom_rc.sh:110-111` writes `lanczos2` / `lanczos3` into the ANSEL rc**
  for `pixel_interpolator_warp` and `pixel_interpolator`. By the rule stated above — a config
  string must equal the kernel's `.name` exactly, or `USERPREF` silently falls back to the
  default — the Ansel half of that benchmark is not measuring the interpolator it reports.
  Lines 139-140 set the *darktable* rc, where those values are still valid, so only the first
  pair is wrong.
- `src/iop/drawlayer.c:808` justifies forcing bilinear for the brush matte with "unlike the
  user-pref default (Lanczos)". The default is `mitchell`. The code is right; only the
  parenthetical is stale.

Neither is fixed here: this file's commit is documentation only.

