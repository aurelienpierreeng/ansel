# OpenCL kernel build options and numerical accuracy

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
is a per-device key in `anselrc` and is never overwritten once present:

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

**Because the key is sticky, an existing installation keeps the old unsafe flags.** Users who
already ran an affected build must either edit that key or delete it and let Ansel rewrite the
new default. Testing a default change therefore requires a fresh `--configdir`.

## If you are adding a kernel

Assume only IEEE single precision and the two flags above. Do not rely on a `native_*` function
being accurate, and do not reintroduce `-cl-fast-relaxed-math` for a vendor without a measurement
in this document showing both that it is accurate there *and* that it is worth it.
