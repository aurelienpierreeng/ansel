# OpenCL kernel build options and numerical accuracy

> **First checked 2026-09-29.** This file was mechanically checked against `8f4638a04e` on 2026-09-29 — every `file:line` citation resolved, every backticked symbol looked up in the tree, every OPEN/planned status claim tested, and every gate or baseline number it quotes compared with `tools/check_module_boundaries.sh` and `tools/include_baseline.txt`. **No per-claim semantic read was done**: a citation that resolves can still describe the wrong thing, so this is a floor, not a verification. Nothing was found wrong by those checks.

## Summary

Ansel used to build every OpenCL kernel with
`-cl-fast-relaxed-math -cl-no-signed-zeros -cl-unsafe-math-optimizations`, for every vendor.
`-cl-unsafe-math-optimizations` (which `-cl-fast-relaxed-math` implies) lets the driver
substitute a low-precision implementation for any libm function. **Whether that is harmless or
catastrophic is a property of the driver, not of the flag.**

On an Intel HD Graphics P630 it is catastrophic: `erf()` returns **exactly 0.0** for
`|x| < 1e-3`. `iop/rawdenoiseai.c`'s GELU is `0.5·x·(1 + erf(x/√2))` and a convolutional network
spends most of its activations in precisely that band, so the whole denoiser drifts — which is
what users reported as a grid or mesh on X-Trans files (discussion #1104).

The default is now `-cl-mad-enable -cl-no-signed-zeros` for **all** vendors. It costs nothing
measurable (see below) and licenses no substitute libm.

## The measurement

`tools/opencl-math-accuracy.c` regenerates everything here on any machine:

```sh
gcc -O2 -o opencl-math-accuracy tools/opencl-math-accuracy.c -lOpenCL -lm
./opencl-math-accuracy
```

It reports, per device and per build-option set, the worst error over `x ∈ [-8, 8]` against a
double-precision reference. The denominator is floored at a thousandth of each function's own
peak, because a plain relative error is meaningless for GELU — it is a difference of two nearly
equal numbers once `erf` saturates, so *every* implementation, including a correct one, scores
badly there. That flooring deliberately does **not** blunt the failure being hunted: `erf` peaks
at 1, so its floor is `1e-3`, and returning 0 where the true value is `1.13e-3` still scores 1.0.

A correct single-precision implementation lands near `1e-7`. Past `1e-4` the driver is running a
different function, not rounding differently.

### Intel HD Graphics P630 (NEO 24.35)

| build options | erf | GELU | exp | log | pow | verdict |
|---|---|---|---|---|---|---|
| (none) | 1.39e-07 | 1.81e-05 | 1.76e-07 | 8.54e-08 | 5.47e-07 | ok |
| `-cl-mad-enable` | 1.39e-07 | 1.81e-05 | 1.76e-07 | 8.54e-08 | 5.44e-07 | ok |
| `-cl-no-signed-zeros` | 1.39e-07 | 1.81e-05 | 1.76e-07 | 8.54e-08 | 5.47e-07 | ok |
| `-cl-denorms-are-zero` | 1.39e-07 | 1.81e-05 | 1.76e-07 | 8.54e-08 | 5.47e-07 | ok |
| `-cl-finite-math-only` | 1.39e-07 | 1.81e-05 | 1.76e-07 | 8.54e-08 | 5.47e-07 | ok |
| **`-cl-unsafe-math-optimizations`** | **1.00e+00** | 1.68e-02 | 1.76e-07 | 8.54e-08 | **4.14e-01** | **BROKEN** |
| **`-cl-fast-relaxed-math`** | **1.00e+00** | 1.68e-02 | 5.21e-07 | 4.41e-06 | 7.18e-07 | **BROKEN** |
| **legacy Ansel default** | **1.00e+00** | 1.68e-02 | 5.21e-07 | 4.41e-06 | 7.18e-07 | **BROKEN** |
| **new default** (`mad-enable + no-signed-zeros`) | 1.39e-07 | 1.81e-05 | 1.76e-07 | 8.54e-08 | 5.44e-07 | ok |

`pow()` is 41 % wrong under `-cl-unsafe-math-optimizations` alone, which matters well beyond the
denoiser — gamma curves are everywhere in this pipeline.

What `erf()` actually returns on that device:

| x | host `erf(x)` | Intel, unsafe |
|---|---|---|
| 1e-1 | 1.1246e-01 | 1.1421e-01 (1.6 % high) |
| 1e-2 | 1.1283e-02 | 8.8153e-03 (22 % low) |
| 1e-3 and below | 1.1284e-03 … | **0.0000e+00** |

### NVIDIA Quadro M2200 (CUDA 3.0)

`erf()` stays exact (7.97e-08) under every option set, but `log()` degrades from 8.54e-08 to
3.46e-05 under the unsafe flag. Nothing in the tree currently depends on that, and nobody had
audited it — which is the point: the flag's blast radius is per-driver and unknowable without
measuring.

### CPU control

The same expressions compiled at `-O0`, `-O2`, `-O3`, `-O2 -ffast-math`, `-O3 -ffast-math`, and
Ansel's own flag set (`-O3 -ffast-math -ffp-contract=fast -march=native`) all score 7.18e-08 on
`erf` and ~1e-5 on GELU. **The CPU path does not lose accuracy at any optimisation level** —
GCC keeps a real libm call for `erff`, so `-ffast-math` does not have the OpenCL flag's effect.
The CPU output is therefore a sound reference to compare devices against.

## What it costs

Nothing measurable, on hardware old enough to show a difference if there were one:

| workload | unsafe | safe |
|---|---|---|
| P630, denoiser export | 96.8 s | 93.9 s |
| Quadro M2200, denoiser export | 8.10 s | 8.14 s |
| Quadro M2200, whole pipeline, denoiser **off** (3 runs) | 5.38 / 4.81 / 4.84 s | 5.27 / 4.92 / 4.89 s |

A 0.4 % spread, inside run-to-run noise. Dropping the flag moves the existing NVIDIA render by
0.00024 % mean over the whole pipeline, so it is not a visible change for anyone who was already
getting correct output.

## End-to-end effect on the denoiser

Same image, same model, `iop/rawdenoiseai.c`, half/single-scale, against the CPU render:

| | mean | p99 |
|---|---|---|
| NVIDIA Quadro | 0.0023 % | 0.0071 % |
| Intel P630, legacy default | **0.7849 %** | **3.1256 %** |
| Intel P630, new default | 0.0023 % | 0.0071 % |

Intel now lands on exactly the NVIDIA figure.

## Overriding per device

The defaults above are only what Ansel writes the **first time** it sees a device. The live value
is a per-device key in `anselrc` and is never overwritten once present — with one exception,
below:

```
cldevice_v4/<index>/<canonical-name>/building=-cl-mad-enable -cl-no-signed-zeros
```

`<canonical-name>` is the device name with every non-alphanumeric character removed, lowercased
(`_ascii_str_canonical()` in `src/common/opencl.c`) — e.g. `Intel(R) HD Graphics P630` becomes
`intelrhdgraphicsp630`. `tools/opencl-math-accuracy.c` prints the exact line for each device it
finds.

After changing it, drop the compiled kernel cache so it rebuilds:

```sh
rm -rf ~/.cache/ansel/cached_kernels_for_*
```

**The key is sticky, except for the legacy default.** A key that still reads exactly
`-cl-fast-relaxed-math -cl-no-signed-zeros -cl-unsafe-math-optimizations` — the default before
`d90978a75c` — is replaced by the current default at the next start (`dt_opencl_device_init()`);
any other value, i.e. one a user wrote, is kept. The key is written before `opencl` is read, so
an install that ran a build older than `d90978a75c` carries it whether OpenCL was enabled or
not. Testing any *future* default change requires a fresh `--configdir` or a migration of the
same kind.

The migration matters because the compensated sums of `data/kernels/compensated.h` and the
double-float of `hl_real.h` rely on the compiler not reassociating, and the legacy default
licenses it. Whether a compiler actually uses the licence is a property of the compiler. The
error term of `csum_two_sum()` under each option set:

| where | `-cl-mad-enable -cl-no-signed-zeros` | legacy default |
|---|---|---|
| clang 18 and 23 offline, `-cc1 -triple spir64-unknown-unknown -O2 -emit-llvm` | 6 flops | `store float 0.0` (also under `-cl-unsafe-math-optimizations` or `-cl-fast-relaxed-math` alone) |
| NVIDIA GTX 1060, proprietary driver (has fp64) † | not reported | **0** |
| Mesa 25.2.8 rusticl, Radeon 780M radeonsi, LLVM 20 (no fp64) | exact | exact |
| Mesa rusticl, llvmpipe † | not reported | exact |
| Apple OpenCL, Apple M1, macOS 26.7.1 (no fp64) | exact; double-float intact ‡ | **0**, and the double-float collapses to fp32 |
| Apple OpenCL, Intel Macs | not measured | not measured |

"exact" means the probe read back the true rounding error (1 for `1e8 + 1`, 1e-8 for `3 + 1e-8`)
from the device. † measured by a reviewer with their own probe, 2026-11; the rusticl radeonsi row on
2026-11-02 against `bca94b57fd`, the Apple M1 row on 2026-11-02 against `b2dfd3ce9b`, clang 23 on
2026-10-28 against `8e628de1c2`.

On the M1 the probe also ran the double-float of `hl_real.h`, the arithmetic of the sparse solvers
on this fp64-less device, through `clBuildProgram()` with each option set and the kernel directory
as `-I`. Under the current default `hl_add(1e8, 1)` reads `(1e8, 1)` and `hl_div(1, 3)` is off by
8.9e-16; under the legacy default they read `(1e8, 0)` and 3.0e-08 — single precision. Two runs,
same figures.

‡ Beyond the M1, not probed directly: read from the sparse solver self-test (`HL_SPCL_TEST`, the
GPU Cholesky in `hl_real_t` against its CPU `double` twin) of the community test campaign, 2026-09-30 to
2026-10-20, builds of the branch from before the migration but each run on a fresh
`--configdir`, hence under the current default. Eight runs, M2 Pro, M3 Pro, M4, M4 Pro, M4 Max
and M5, all read 4.25e-10 maximum relative difference, two orders of magnitude below single
precision's unit roundoff (6e-8), which a solve carried out in plain fp32 cannot reach. For
comparison, the same test reads 3.1e-12 with native double (seven NVIDIA cards), 1.2e-14 on an
RX 6600 under AMD's driver, and 3.0e-10 with the double-float on rusticl. None of the campaign's
runs used the legacy options: on those chips the legacy column is the M1's, assumed to hold for
the same compiler, not measured.

So two compilers are measured to fold it, with different costs. On the NVIDIA card what is lost
is the compensation of the reductions; its solvers run in native double. On Apple Silicon, which
has no fp64, the double-float folds down to plain fp32 as well: the biharmonic systems, conditioned
around 3e8, would then be solved in single precision. That is the case the migration exists for:
until it, every Mac that had run a build older than `d90978a75c` carried the legacy key, OpenCL
enabled or not, and would have built these kernels with it as soon as OpenCL came on by default.

Why rusticl does not fold, read from Mesa's dumps (`CLC_DEBUG=dump_llvm,dump_spirv`,
`AMD_DEBUG=cs,nir,asm`) and its source at `mesa-25.2.8`: clang emits `fadd fast`, but Mesa runs
it at `-O0` (`src/compiler/clc/clc_helpers.cpp`), so no LLVM pass uses the flags; the SPIR-V
carries them as the legacy `FPFastMathMode Fast` bit (0x10); and `spirv_to_nir`
(`src/compiler/spirv/vtn_alu.c`, `handle_fp_fast_math()`) marks an operation `exact` unless it
carries all four `SPV_KHR_float_controls2` bits (`AllowRecip`, `AllowContract`, `AllowReassoc`,
`AllowTransform`), which `Fast` does not. The final RDNA3 code keeps all six additions. A Mesa
whose translator emits the `float_controls2` bits would no longer be protected by this.

No `#error` tripwire on `__FAST_RELAXED_MATH__` guards those headers: a program that fails to
build fails `dt_opencl_device_init()` as a whole, which would cost the user the GPU, not just
this module.

## If you are adding a kernel

Assume only IEEE single precision and the two flags above. Do not rely on a `native_*` function
being accurate, and do not reintroduce `-cl-fast-relaxed-math` for a vendor without a measurement
in this document showing both that it is accurate there *and* that it is worth it.
