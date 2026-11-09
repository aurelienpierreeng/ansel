<!-- Provenance: every finding carries the commit it was established against. -->

# Highlight reconstruction (harmonic)

> **Verified against `42eca0e8fe` on 2026-09-29.**
>
> Three failure families, each found by measurement after plausible-but-wrong theories died — and the bench that judges them.
>
> Each finding below is dated with the commit that established it. A finding is only as
> good as its hash: before acting on one older than the code you are changing, re-measure
> it — and re-date it here when you do. Where an earlier version of a claim was **wrong**,
> that is recorded rather than quietly corrected: how a claim was wrong is usually the more
> useful thing to know.

## Highlights (harmonic) on OpenCL: the device middle outgrows the working set it declares

> **Established 2026-10-24 against `2fadf8a366`.** GTX 1060 6 GB (driver 580.178.04), i7-3770K
> (8 threads), thumbnail pipes of a 51-image collection rendered from the raw, the desktop on the
> same card.

`tiling_callback()` gives harmonic the guided-laplacian budget,
`factor_cl = 2 + 3·4 + 6·4/DS_FACTOR = 20` times the single-channel input. Its comment said
harmonic "allocates region buffers well under this envelope". **That was wrong.** On a 5208×3476
input (72.4 MB as one float plane, so 1.45 GB declared), Ansel's own vRAM accounting
(`-d opencl -d memory`) went from 932 MB at the module's entry to 4.5–4.97 GB through the device
middle: the module held at least 4.0 GB, about 2.8× what it declares.

The admission check (`dt_opencl_image_fits_device_reason()`) compares the declaration with
`used_available − memory_in_use`, `used_available` being the device's memory minus its
`forced_headroom`. OpenCL publishes nothing about what other processes hold on a discrete device,
so the headroom is the only allowance for them. Here the desktop held 978 MiB before Ansel
started, and the driver's context about 140 MiB more that Ansel does not count. `nvidia-smi`,
sampled every 100 ms, read 5856 of 6063 usable MiB while the middle ran. The next 265 MB image did
not fit, and the driver said so on its first use: `copy_buffer_to_image` returned `-4`
(`CL_MEM_OBJECT_ALLOCATION_FAILURE`), then "harmonic GPU middle failed (-4), using the host
middle".

The host middle runs the reconstruction on the CPU at full sensor resolution: 43.8 to 74.2 s for
one thumbnail, against 0.005 to 10 s on the device, for one or two images of the collection per
run. It runs inside the pipe, which keeps the device and `dt_pipeline_threadsafe_mutex()`
throughout, so every other thumbnail waits.

Raising the device's headroom does not prevent it. With `forced_headroom` at 900 and at 1500 MiB,
the same image failed at the same copy (host middle 68.9 s and 72.7 s), and vRAM peaked at 5920
and 5947 MiB. The headroom lowers the budget the entry check reads; nothing checks the middle's
allocations against it once the module is admitted.

### What holds the memory: one merged window over a chain of small holes

> **Established 2026-10-25 against `2fadf8a366`**, with temporary prints around each region of
> `_harmonic_reconstruct_cl()` and `_harmonic_reconstruct_regions()`, and in the merge of
> `_segment_clipped_regions()`; same machine, collection and `forced_headroom` 900 MiB.

The working set follows one region's padded window, not the frame. `_segment_clipped_regions()`
merges every pair of regions whose padded read boxes intersect, and rebuilds each group as the
union of its members' bboxes, padded by the group's largest pad. A chain of small holes whose
pads touch thus becomes one window over most of the frame:

| image (5208×3476) | regions after merge | largest window | members | clipped px in its bbox | largest member bbox |
|---|---|---|---|---|---|
| 18477 | 555 (of 2216) | 4749×3476, 16.5 Mpx (91 %) | 705 | 1.10 Mpx | 1.55 Mpx |
| 18977 | 403 (of 1571) | 4694×3469, 16.3 Mpx (90 %) | 617 | 1.00 Mpx | 1.48 Mpx |

- **On the device**, 18477's window raised Ansel's accounting from 932 MB to 4971 MB (`-d memory`),
  `nvidia-smi` read 5755 of 6063 MiB, and the region failed after 3.0 s with `-5`
  (`CL_OUT_OF_RESOURCES`, events of `hl_fill_jacobi` unreadable). Reproduced in two sessions. The
  46 regions before it were small (at most 36 kpx), took the CPU route and 1.0 s in all.
- **On the CPU**, 18977's window took 70.1 s of the 77.5 s its 403 regions took. Running only that
  window on the host would therefore save little time.
- **After that `-5`, the device stays unusable for the session**: every later kernel on it fails
  with `-5` (`colorin`, `exposure`, `interpolation_resample`, …), and every module falls back to
  the CPU. In one session, 55 module runs were on the GPU before it and none of 337 after.
  The `-5` is a kernel reading a NULL buffer, not the memory: see "a gate-0 region read a NULL
  buffer and killed the device" below.

Open: the declaration does not describe the device middle, and the merged window can be about 15
times the clipped area it reconstructs. Raising `factor_cl` to the measured working set would send
the module to the CPU from the start on this card, at about the same cost. The region loop instead
checks each big window against what the device has left and sends only an oversized one to the host
(see "a window that does not fit the device takes the CPU offload" below). Merging less would
shrink the window, but the merge exists to avoid seams between neighbouring fills, so it changes
the result.

## Highlights (harmonic): per-channel saturation floors imprint the floors' own chroma — magenta

*Found `e4195dec51`, 2026-08-05.*

The "harmonic reconstruction leaves highlights magenta" complaint (MAC25640.RAF) survived three
plausible-but-wrong theories (X-Trans phase mismatch, demosaic bleed, coefficient-fit slope error —
each killed by measurement, per the retouch note's warning about reasoning from source alone). The
actual mechanism, found by dumping the module's raw output and comparing per-channel means over
clipped photosites against the per-channel clip thresholds:

- Highlights receives **WB-applied** data (`temperature` runs before it; `clips[c] = 0.995 * clip *
  processed_maximum[c]` and `processed_maximum` carries the WB coefficients, e.g. `{2.29, 1.0, 1.31}`).
  A *white* surround is therefore `R≈G≈B` in this domain, and each channel saturates at a *different*
  level — the WB coefficients themselves.
- Every reconstruction stage ended by flooring each clipped channel independently at its own
  saturation level (`max(est_c, clip0_c)`, soft or hard). On a multi-clip pixel this snaps the
  clipped subset to the floors' chroma — the WB coefficients — i.e. neutral-sensor ≡ **magenta**,
  regardless of the hue the solver produced (measured: clipped R snapped to its 2.29 floor while
  reconstructed G stayed at 1.16 → G the lowest channel). The solver's chroma was fine; the floors
  destroyed it. Laplacian looked whiter only because it **violates** the floor (outputs clipped R at
  1.67 < 2.29): dimmer but not magenta — a trade, not a reference.
- Fix candidate: the **joint (hue-preserving) floor** — one scalar lift of the clipped subset,
  `s = max_k(t_k/e_k)` (t_k the per-channel soft-floor target), then `e_k ← s·e_k`: brightness from
  the most-demanding floor, hue preserved; identical to the per-channel floor for 1-clip pixels;
  capped (8×) with the per-channel floor kept after as the safety net. An early attempt applied it
  UNGATED at all three floor sites: it fixed MAC visually but regressed the whole article bench (all
  six original synthetic cases run at UNIT WB with EQUAL clips, where the per-channel floor imprint
  is neutral ≈ truth), so it was reverted pending the gate below.
- STATUS 2026-08 (implemented, CPU + CL + python prototype): the **clip-asymmetry floor gate**
  `_hl_floor_gate(clips)` (`highlights/common.h`) — `g = smoothstep((A − 1.25)/0.75)`,
  `A = max_c(clip_c)/min_c(clip_c)` — blends per-channel ↔ joint at every floor site, plus three
  surround-chroma refinements at full gate: a cmean pull of the self-dome ratios (β = 0.5g), a
  chroma-decoupled self-dome recombine, and a clip0-rehue in the joint core (redistribute the
  all-clip floor to the mean valid chromaticity, sum-preserving). Every site takes a
  `g ≤ 1e-6 → per-channel` early-out that is bit-exact approved behavior. TWO TRAPS measured here:
  (1) the ramp must NOT start at A=1 — `clips` inherit `processed_maximum`, which carries a ~10%
  non-WB profile wiggle even at unit WB (measured A=1.145 on the AsShotNeutral=(1,1,1) bench DNGs);
  a ramp from 1.0 opened the gate to 0.2 on the unit-WB bench and silently contaminated a whole
  candidate-export round. Real cameras sit at A≈2–2.6, so the [1.25 → 2.0] ramp separates cleanly.
  (2) the bench DNGs CANNOT exercise the gate through ansel-cli: `temperature` ignores their
  AsShotNeutral (pmax constant across cases), so pipeline clips stay near-equal even for the
  wbclips case — gate validation lives on real raws and in the python prototype's WB-regime leg
  (`py_reconstruct(..., wb=…)`, which scales the working domain and drives the same smoothstep).
  Measured: MAC neons' G/((R+B)/2) top band 0.839 → 0.931 (the magenta fix), python WB-regime
  wbclips chroma-share RMSE halved; but the surround-importing refinements (cmean pull, rehue,
  decouple) HURT self-coloured emitters (magentasun WB-regime cRMSE 1.29 → 2.95) — they trust the
  surround like the cgrad continuation does. They are therefore gated by their own trusted-ring
  vote `_hl_ring_flat_mean_vote` (common.h; `hl_ring_vote` kernel; `py_flat_mean_vote`):
  t = |ring_mean_shares − bright_mean_shares|_L1 / max(ring_dispersion, 0.02), vote = exp(−(t/5)²),
  refinement strength = floor_gate × vote. Three designs died by measurement first: (1) per-pixel
  agreement averaging — real rings' noise/texture spread caps the vote at ~0.3 at ANY tolerance
  tight enough to keep the emitter closed (tol 0.10 strangled MAC/sunrise at 14/11% of the
  refinement span; tol 0.25 leaked magentasun and fully opened gradsky); (2) the ALL-valid window
  mean as reference — polluted by dark unrelated content, closes the vote on exactly the scenes
  the refinements are for (the cgrad-anchor lesson again: use BRIGHT valid pixels ≥ 0.35 × blown-
  zone plateau; in `_joint_core` this bright mean must be a SEPARATE quantity from the approved
  all-valid mean that seeds the screened-Poisson solver); (3) a bright-surround-dispersion factor
  to close gradient skies — measured INVERTED (synthetic gradsky's bright dispersion 0.059 is
  LOWER than MAC's 0.11–0.20). The t-form works because a self-coloured emitter shifts the whole
  ring COHERENTLY (magentasun bias 0.21 / dispersion 0.007) while real scenes scatter AROUND the
  mean (MAC/sunrise t = 0.3–1.5). Measured outcome: magentasun ≈ floors-only (protected), wbclips
  ≈ full gate, sunrise 100% of the refinement span, MAC 74%, gradsky opens (statistically
  indistinguishable from sunrise in every ring/surround statistic measured — accepted trade,
  flagged in review). The vote lives entirely inside `floor_gate > 1e-6` blocks: the unit-WB
  bench stays bit-identical (spot-checked at run-to-run noise).

Diagnostics that settle this class of bug in one pass: dump the module's raw output (`ovoid`) and
bucket per-CFA-channel means by "own channel clipped" vs the per-channel `clips[]` — if the output
means sit at the clip levels, the floors are authoring the color, not the solver.

## Highlights (harmonic): a 1-clip pixel is only "measured" if the fit actually lifted it

*Found `c5c34c3ecb`, 2026-08-20.*

Third failure family, on a blown sky meeting a dark ridge (DSC_1267.NEF): a magenta rim hugging the
mountain's edge. The cgrad stage's survivor-anchored reprojection deliberately skipped 1-clip pixels
as "measured-correct where the fit spoke" — but *the fit does not always speak*. Where the sky dims
toward an occluder it drags the guides, the colour-line prediction lands **below** the pixel's own
saturation level, and `fmaxf(pred, clip0)` pins the channel AT the floor — the minimum compatible
value, carrying the floor's chroma (i.e. the WB coefficients: magenta) instead of the material's.
Measured: **96.5%** of the 1-clip-G pixels 4–6 px from the rock, and 90.4% at 8–12 px, exit the whole
chain still at their floor (median lift 1.000), against 27.9% far from it.

Such a pixel holds exactly what a *partial multi-clip* pixel holds — two measured channels plus a
surround chromaticity — so it takes the same survivor-anchored reprojection. Ring profile (R+B)/2G by
distance from the ridge: 1.032 / **1.046** / 1.030 → 1.004 / **1.005** / 1.007.

**Two traps, both paid for in a full bench round each:**

*Never a hard threshold.* The first version admitted pixels via `est <= 1.03 * clip0`. Every metric
went flat and the render grew a NEW hard-edged pink region — the test printing its own contour along
the boundary between mostly-floored and mostly-lifted pixels. It is now an inverted smoothstep on the
lift (`CF_AUTHORED_RAMP`). The numbers alone would have shipped the bad version; only the picture
caught it.

*Anything that imports surround chroma MUST be gated on `_hl_floor_gate(clips)`.* Ungated, this
regressed the whole unit-WB bench — pk1synth +140% RMSE, balls +26%, occluded +18% — because at
unit WB every channel saturates at the same level, so a floored channel's chroma is NEUTRAL ≈ truth
and reprojecting it replaces a correct value with a guess. This is the SECOND time this exact
omission cost a bench round (the joint floor's first attempt, documented above, died the same way).
Gated, the six unit-WB cases return **bit-identical** to baseline (A = 1.145 vs a ramp starting at
1.25 → gate exactly 0) while real raws (A ≈ 2–2.6) keep the full fix. Note the same function already
consulted `floor_gate` to decide which pixels may *vote* — consulting it for the voters and not for
the pixels being *acted on* is the shape of the mistake.

## Highlights (harmonic): value-continuing estimators inherit the fence-band chromaticity — blotches

*Found `e4195dec51`, 2026-08-05.*

Second failure family, found on a blown sunrise laced with branches (Brandon Woods RW2): blotchy
pink/lavender patches and branch-shaped chroma ghosting inside the reconstruction. Root cause chain,
each link established by measurement (fit-plane dumps, per-clip-count chroma buckets against a
bright-sky-only reference — the all-valid mean is contaminated by dark foreground and is the wrong
reference):

- The pair-fit slope field itself carries the blotches (dumped `a`/`d`/`R²` planes show branch
  skeletons and low-slope blobs), and even the windowed **mean-ratio** is off: every value-continuing
  estimator (colour-line fits, harmonic ratio fills) anchors on the **last unclipped band before the
  clip contour**, which is unrepresentative — sensor-rolloff/flare-whitened, and threaded with
  occluder penumbras. Hardening the dim-anchor occlusion gate makes it *worse* (starves windows;
  dark anchors were warm, not the contaminant). Shrinking slopes toward the mean-ratio prior
  plateaus ~30% short: the prior inherits the same fence hue.
- Fix: `_chromaticity_gradient` (core.c) + `_chromaticity_gradient_stage_cl` / `hl_cgrad_*` kernels, the LAST
  region stage. Extend the **bright** fully-valid surround's chroma shares into the blown zone with
  the **biharmonic** (gradient-extending) dome — the same operator the luminance already uses — then
  reproject each multi-clip pixel's clipped subset onto the extended field and re-assert the joint floor.
  Details that took iterations to get right: (1) anchors = fully-valid AND ≥35% of the blown-zone
  plateau luminance AND clear of a **thin** guard ring around the clip contour (σ=4 blur of the
  any-clip mask, threshold 0.05 — a wide moat exiles anchors to unrepresentative far content and
  inverts the fix); (2) partial pixels use **survivor-anchored** scaling (valid channels set the
  brightness against the field, clipped channels take the field shares outright) — a
  magnitude-preserving reprojection cannot work there because the clipped subset's magnitude IS the
  under-prediction; all-clip pixels keep the dome-luminance magnitude and redistribute; (3) 1-clip
  pixels **are** reprojected, ramped by an inverted smoothstep on the lift
  (`highlights/core.c:664-681`, `reproject_w = gate_w * authored_w`) — this bullet said "left
  untouched" until 2026-09-29, which the very next section of this file contradicts and which
  stopped being true when that ramp landed; (4) bail when there is
  no usable bright surround (emitter in darkness) — the fence chromaticity is then all there is.
- STATUS after the article-bench review: the stage is gated and defensive. Reprojection excludes
  ALL-CLIP pixels only — 1-clip and partial multi-clip both go through it (all-clip pixels keep the joint-core dome — redistributing a poor
  core total by the field's shares is unstable: measured negative pixels on the gradsky bench case),
  and the survivor-anchored scale is capped at 4× the pixel's current magnitude. Note
  `_region_blur1_cl` (device gaussian) operates on **images**, not buffers — allocate with
  `dt_opencl_alloc_device`, write via `write_imagef` (the -1 selftest sentinel with a silent
  full-host fallback is what a buffer/image mismatch looks like).
- **Content gate (1-clip-annulus validation)**: the continuation prior is a scene assumption — true
  for gradient skies, false for self-coloured emitters whose blown core carries its OWN chromaticity
  (a magenta sun, coloured lamps: the article-bench regressions). The stage self-arbitrates against
  the method's trusted zone: at 1-clip pixels (reconstructed from TWO measured guides, measured
  chromaticity-correct everywhere), compare the extended field's shares to the solver's; weight
  `w = exp(-(L1_err/0.10)^2)`, diffused inward by normalized convolution (`σ = radius/4`), blends the
  reprojection, shrunk toward the REGION-LEVEL ring vote as local evidence thins
  (`w = (blur_w + λ·vote)/(blur_m + λ)`, λ=0.05 — a hard no-evidence→0 fallback kills the stage in
  the deep interior of large blown zones, beyond blur reach of the thin ring). Mirrored in
  `hl_cgrad_gate` + gated `hl_cgrad_reproject` (CL) and `py_chromaticity_gradient` (prototype).
  Measured: the gate correctly identifies MAC's white lamps as self-coloured and disables the
  continuation there — the MAC magenta fix therefore CANNOT come from this stage; it belongs to the
  joint-floor family and its clip-asymmetry gate (now implemented, see the previous section).

## Highlights: Apple's OpenCL defines `cl_khr_fp64` without having it — never guard on that macro

*Found against `e83ba5865d`, 2026-09-26, measured on an Apple M1 (macOS OpenCL 1.2).*

`double` had three uses in this module's kernels, and only one is double precision: the sparse
Cholesky factor and solves of `highlights_sparse.cl` (`sparse_chol_*`) and the second-member
vector they consume (`hl_pde_*`, `hl_aniso_*`). The nine reduction kernels — the four stage-2
finalizers of `highlights_harmonic.cl` and the CG dot products of `highlights_sparse.cl` —
accumulated in double and narrowed every result to float on the way out, so the double only
ever bought the accuracy of the SUMMATION. They now sum in compensated single precision
(`data/kernels/compensated.h`: a `float2` of value + running error, Knuth two-sum, no multiply
so `-cl-mad-enable` cannot contract it) and sit outside any fp64 guard: available on every
device, the host twins agreeing to a float ulp rather than bit for bit. A new kernel header
must be listed in BOTH `clincludes[]` (`common/opencl.c`, the kernel-cache checksum) and
`DT_OPENCL_EXTRA` (`data/kernels/CMakeLists.txt`, the install); an omission in the first keeps
serving yesterday's binaries, in the second the kernel does not build at all in a package.

Measured on Apple M1 (macOS OpenCL 1.2, no `cl_khr_fp64` in `CL_DEVICE_EXTENSIONS`,
`CL_DEVICE_DOUBLE_FP_CONFIG` = 0): the runtime DEFINES the `cl_khr_fp64` preprocessor macro
anyway. A `#if defined(cl_khr_fp64)` block therefore compiles, `clBuildProgram` succeeds, and
every kernel that loads, stores or computes a `double` fails at `clCreateKernel` with -48
(`CL_INVALID_KERNEL_NAME`) — per kernel, not per program — the runtime logging `UNSUPPORTED
(log once): createKernel: newComputePipelineState failed` once, for the first. A `double` that
appears only in a signature is fine. The guard thus does not do on Apple what its comment said
it does elsewhere, and the tell in a `-d opencl` log is a kernel INSIDE the guard that does not
fail: `hl_cg_beta_step` named no double and was the one survivor of its block. When fp64 has to
be selected, select it host-side from `CL_DEVICE_DOUBLE_FP_CONFIG` and pass a define the way
`-DNVIDIA_SM_20=1` is passed (`opencl.c`, into `options` AND `options_md5`) — never from the
macro. `ansel-cli` takes `-d` through `--core` (`ansel-cli in out --core -d opencl -d perf`).

### The eight solver kernels compute in a real type the host picks per device

*Found against `e83ba5865d`, 2026-09-26; the per-operation figures were measured on an Apple M1.*

That is now how the eight solver kernels work. `data/kernels/hl_real.h` defines `hl_real_t`
and `hl_add/sub/mul/div/sqrt`: native `double` when the host passed `-DDT_DEVICE_FP64=1`
(`cl->dev[dev].fp64`, from `CL_DEVICE_DOUBLE_FP_CONFIG != 0`, `dt_opencl_device_has_fp64()`),
a `float2` (hi, lo) double-float otherwise — Knuth two-sum, fma two-product, Newton division
and square root, measured on the M1 at 5e-15..1.2e-14 relative per operation, i.e. ~46 bits.
Both are 8 bytes (`DT_HL_REAL_BYTES`), so no device buffer changes size; a host `double`
vector crosses through `_sp_cl_upload_real()` / `_sp_cl_read_real()`
(`math/sparse_cholesky_cl.h`), which split and rejoin only where the device lacks fp64. A
device with fp64 therefore runs the exact code it always ran. The precision budget that makes
df32 acceptable is thin: the biharmonic systems are conditioned like h⁻⁴, about 3e8 at
`DT_HL_SPARSE_MAX` (16384 unknowns), so ~45 bits are needed and df32 has ~46-47 — raising that
cap without redoing the arithmetic silently loses digits on fp64-less devices only.
`HL_SPCL_TEST=1` is the parity check (GPU solver vs the CPU `double` twin on a 13-point
biharmonic disc); it also runs from the CPU `process_harmonic()` when a device exists, because
a run whose memory budget sends the module to the CPU never enters `process_harmonic_cl()` (an
8 GB Apple M1 when its free RAM is short, see the next section). Do not write a new `.cl`
reduction in `double` for accuracy: `compensated.h` already covers that case in fp32.

Both headers rely on the compiler not reassociating: under `-cl-unsafe-math-optimizations` or
`-cl-fast-relaxed-math`, clang folds the error term of a two-sum to `0.0` (measured with clang
23, see `doc/opencl-math-accuracy.md`, which also covers the migration of the per-device
`building` key away from the pre-2026-08-18 default that carried them). `hl_real.h` takes its
two-sum from `compensated.h`, so there is one copy. Under `DT_DEVICE_FP64` it enables
`cl_khr_fp64`, or `cl_amd_fp64` where only that one is defined, as the old guard did — the host
has then read real fp64 off the device, so the macros can be trusted. CI test-compiles
`highlights_sparse.cl` a second time with `-DDT_DEVICE_FP64=1` (`data/kernels/CMakeLists.txt`),
since the native half is otherwise compiled only by users' drivers.

### On Apple Silicon the device budget is not `max_mem_alloc / 2`

*Measured 2026-09-26 to 2026-10-02 on six Apple Silicon Macs (8 to 36 GB), base `e83ba5865d`;
the device field below added 2026-10-28 against `8e628de1c2`; its vendor test measured on an
M1 the same day, and on a tester's M4 Pro with `clinfo` (discussion #1466, comment 19248411).*

`dt_opencl_get_device_available()` (`common/opencl.c`) bounds a host-unified device's working
set by `max_mem_alloc / 2`, a proxy for Intel NEO's per-context aperture calibrated on a P630.
Apple's OpenCL on Apple Silicon reports `CL_DEVICE_MAX_MEM_ALLOC_SIZE` as 3/16 of its global
memory: 1024 of 5461 MB (8 GB M1), 2048 of 10923 (16 GB), 2557 of 13640 (18 GB), 3410 of 18186
(24 GB), 5391 of 28754 (36 GB). The clamp therefore made the budget about a sixteenth of the
RAM: 512 MiB on the 8 GB M1, 1.7 GiB on a 24 GB M4 Pro (`vRAM has 1704 MiB left` in a master
log, where highlights, demosaic and denoise went to the CPU). Do not assume
max alloc is a fixed figure on Apple: it scales with the RAM.

The clamp is skipped where `dt_opencl_device_t.aperture_clamp` is unset, which
`dt_opencl_device_init()` decides once: platform name AND device vendor both "Apple" (measured
on an M1 and an M4 Pro: platform `Apple`, vendor `Apple`, vendor id `0x1027f00` on both; M2, M3
and M5 not checked yet — the `DEVICE VENDOR` line of the `-d opencl` log shows it). The platform alone would
also take in Intel Macs, which publish the same "Apple" platform for GPUs whose vendor is
"Intel Inc." or "AMD", and where nothing was measured. It is deliberately not keyed on the
crash-attribution table (`_gpu_runtimes[]`): that table's module column must name a runtime's
own object, and on macOS the only candidate, `OpenCL.framework`, is the loader every OpenCL call
goes through. The same reasoning keeps OpenCL off by default for any build that can run on
Intel (`data/CMakeLists.txt`).

Without the clamp the budget is the declared memory less the headroom, bounded by the RAM
actually free. On the 8 GB M1 it read 1.2 to 2.1 GiB, and the harmonic reconstruction of
a 24 Mpx raw (1734 MiB asked) ran on the GPU in most exports and on the CPU when free RAM was
short; profiled denoise (3.5 to 3.8 GiB) stayed on the CPU. On the five 16 to 36 GB machines the
whole pipeline ran on the GPU in every export. The self-tests are still
called from the CPU entry point as well, for the runs that do not reach `process_harmonic_cl()`.

## The article bench (guided-laplacian-highlights-research) — traps and extensions

*Found `e4195dec51`, 2026-08-05.*

- `ansel-cli` NEVER overwrites an existing export — it silently appends `_01`. Any script that
  re-exports over old files reads STALE results (one debugging session was burned on catastrophic
  bench numbers that were v1-scene exports scored against v2 ground truth). Always `rm -f` the
  target (and its `_*.tif` siblings) before exporting.
- The bench runs with `python3.12` (cv2/scipy live in ~/.local for 3.12; plain `python3` is 3.13
  and lacks them).
- The original six synthetic cases all write `AsShotNeutral=(1,1,1)` → the pipeline runs with UNIT
  white balance and EQUAL per-channel clips, a regime no real camera occupies; and none of them
  models a blown gradient sky with occluders. Two cases added 2026-08 (`make_new_cases.py`):
  `wbclips` (white emitter over textured amber behind mullion occluders, AsShotNeutral for
  wb=[2.2,1,1.5] — the MAC magenta mechanism needs unreliable fits + truth-white core + WB clips,
  a smooth well-anchored WB scene does NOT reproduce it) and `gradsky` (the sunrise geometry).
  Linear RMSE is magnitude-dominated and nearly blind to the hue failures users report — judge
  chroma with a chromaticity-share RMSE alongside it.

## Highlights (harmonic) on OpenCL: a gate-0 region read a NULL buffer and killed the device

> **Established 2026-10-25 against `2fadf8a366`.** GTX 1060 6 GB, driver 580.178.04.

`hl_dome_blend` reads `refine[0]` (the refine gate) on its first line, whatever the gate.
`_selfdome_stage_cl()` (`iop/highlights/core.c`) allocated `refine_dev` only inside its
`floor_gate > 1e-6` block, so with near-equal per-channel clips (asymmetry under 1.25,
`_hl_floor_gate()`) the kernel got a NULL buffer. Only regions whose padded window exceeds
`DT_HL_CL_CPU_REGION_PX` take this device path, which is why only one region in hundreds hit it.

On NVIDIA the read is an MMU fault, not an error code: the kernel log records
`NVRM: Xid 31 … MMU Fault … faulted @ 0x0_00000000 … ACCESS_TYPE_READ` at the second of the failure,
the queue returns `-5` (`CL_OUT_OF_RESOURCES`), and the context stays dead: every later kernel on the
device fails with `-5` and every module falls back to the CPU for the rest of the session (55
module runs on the GPU before it in one session, none of 337 after). An asynchronous queue does not
say which kernel faulted; `clFinish()` after each enqueue named `hl_dome_blend`, and a check in
`dt_opencl_set_kernel_arg()` found its argument 13 NULL.

The buffer is now allocated at gate 0 too and zeroed by `hl_set_scalar`, which takes the kernel's
gate-0 path; the same defect aborted the process on radeonsi under rusticl. With it allocated and
zeroed at gate 0, measured on the same 51-image session from an empty cache: no Xid, the
16.5 Mpx window of image 18477 reconstructed on the device in 23.1 s (70 s for the same kind of window
on the CPU), all five harmonic images kept the device middle, and 376 module runs stayed on the GPU.
`nvidia-smi` still peaked at 5960 of 6063 MiB.

When a pipe falls back to the CPU on `-5` with no allocation failure in sight, look for an Xid in
`journalctl -k` before suspecting memory.

### A window that does not fit the device takes the CPU offload

> **Established 2026-10-25 against `2fadf8a366`**, with `refine_dev` allocated and zeroed at gate 0
> (previous section).

A region on the device path holds about 64 floats per pixel of its padded window: 256.4 to
257.2 bytes per pixel on windows of 2.8, 16.3 and 16.5 Mpx (`-d memory`, peak over the region minus
the accounting at its start), linear in the window. Nothing checks those allocations against the
headroom once the module is admitted, so the region loop of `_harmonic_reconstruct_cl()` now asks
`dt_opencl_image_fits_device()` for `DT_HL_CL_REGION_FLOATS_PER_PX` (65) floats per window pixel
before a big region, and sends a window that does not fit to `_region_cpu_offload_cl()`, which needs
9 floats per pixel on the device. Only that window goes to the host; the other regions and the rest
of the pipe stay on the device.

Measured on the same session from an empty cache. With `forced_headroom` at 2500 MiB, both 16 Mpx
windows were sent to the CPU (`harmonic region window 4749x3476 does not fit the device`), the
module took 76.1 s on image 18477, `nvidia-smi` peaked at 2398 MiB, and nothing failed. With 900 MiB
both fit, stayed on the device (31.2 s on 18477, peak 5909 MiB), and 376 module runs were on the GPU.
