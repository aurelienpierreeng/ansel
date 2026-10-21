# Abandoning a module mid-run

> **Measured 2026-10-11 on top of `74ee852d4c`**, RelWithDebInfo, clang 19, 8 cores, NVIDIA
> GeForce GTX 1060 6GB (CUDA OpenCL). Every figure below says how it was obtained. Re-measure
> before acting on one older than the code you are changing, and re-date this line when you do.

The pipe reads its killswitch, `dt_dev_pixelpipe_has_shutdown()` (`develop/pixelpipe_hb.h:695`),
between two modules. A module that takes seconds therefore runs to its end once the pipe no
longer wants its output: the image changed, a parameter moved, the darkroom closed. The modules
below also read it inside their own `process()` / `process_cl()`, at a step that leaves them
nothing to clean up but their own scratch.

## The contract

A module that sees the killswitch returns an error, and its output buffer holds garbage. Nothing
else is needed from it: the pipe already handles a failed module without any help.

- **CPU.** `pixelpipe_process_on_CPU()` returns the error. With the killswitch raised, it logs
  `[pixelpipe] %s process on CPU stopped: the pipe abandoned its run` under `-d pipe` only, as quiet
  as a stop between two modules; otherwise it prints `... returned with an error` on stdout
  (`develop/pixelpipe_cpu.c:143`). The tiled path returns a tile's error as is, without running the
  other tiles (`develop/tiling.c:448`).
- **OpenCL.** A failing `process_cl()` normally falls back to CPU and tells the user OpenCL
  failed. When the killswitch is raised, `pixelpipe_process_on_GPU()` aborts instead
  (`develop/pixelpipe_gpu.c:730`): rerunning on CPU a run nobody wants would only redo the work.
  The tiled OpenCL path ends in the same error label.
- **Either way**, `dt_dev_pixelpipe_process_rec()` flags the output cacheline auto-destroy while it
  still holds it (`develop/pixelpipe_hb.c:1233`), so it is never published, and no alert is shown:
  an abandoned run is not a failure anybody needs to hear about.

A `NULL` pipe never abandons: `dt_dev_pixelpipe_has_shutdown(NULL)` is `FALSE`. A module that is
also called outside a pipe (the NL-means core takes its pipe through `dt_nlmeans_param_t`) can
pass `NULL` and always run to the end.

## Where a module can stop

### On CPU: at a step, or at a row of a parallel loop

Where the work is a sequence of full-image passes (scales, iterations, regions), the check sits at
the top of each pass and the function frees its scratch and returns 1.

Where the work is one OpenMP loop, a `return` is not allowed inside it, so each row (or chunk) asks
first and skips its work once the run is abandoned:

```c
dt_atomic_int abandoned = FALSE;
__OMP_PARALLEL_FOR__(shared(abandoned))
for(int i = 0; i < height; i++)
{
  if(dt_atomic_get_int(&abandoned) || dt_dev_pixelpipe_has_shutdown(pipe))
  {
    dt_atomic_set_int(&abandoned, TRUE);
    continue;
  }
  ...
}
return dt_atomic_get_int(&abandoned);
```

**The latch is required.** The killswitch can drop again before the loop ends: the darkroom
resets it when the pipe starts a new run (`develop/develop.c:731`), and the thumbnail fetcher
resets `shutdown_ext` as soon as a new request is queued (`views/view.c:712`). Without the latch,
the rows skipped while it was up and the rows run after it dropped would make an output that is
returned as success with holes in it.

**`shared(abandoned)` is required too.** `__OMP_PARALLEL_FOR__` is `default(firstprivate)`
(`system/openmp.h:65`): without the clause, each thread latches its own copy, the rows are skipped
all the same, and the function returns the untouched `FALSE` — a holed output, published as a
regular cacheline. That is the defect above, by another route, and it compiles without a warning.
It was caught by the mid-run test below (blurs stopped 42 ms after the killswitch yet was logged
"blended on CPU", i.e. it had returned 0), not by reading the code.

### On OpenCL: only after `dt_opencl_finish()`

The host enqueues kernels far faster than the device runs them, and OpenCL cannot take back an
enqueued kernel. A check at enqueue time runs while the device has not started, and a raised
killswitch then saves nothing. So a CL loop reads the killswitch only after waiting for the
previous step:

```c
if(i > 0 && dt_opencl_finish(devid) && dt_dev_pixelpipe_has_shutdown(pipe))
{
  err = DT_OPENCL_DEFAULT_ERROR;
  goto error;
}
```

This costs one device sync per step, so the step must be long enough for the sync to be noise.
`i > 0` keeps a single-step run free of syncs. `dt_opencl_finish()` returns `FALSE` when the sync
fails, and also, per the code, for the rest of the run once any earlier event failed: it returns
the summary of `dt_opencl_events_flush()`, which only `dt_opencl_events_reset()` clears. The check
is then skipped and the module runs to its end as it did before: a late stop, never a wrong output.

## The modules

| Module | CPU stops between | OpenCL stops between |
|---|---|---|
| AI denoise (`rawdenoiseai`) | U-Net levels, via `dt_nn_set_cancel()` (`common/nn_model.h`) | U-Net levels, after `dt_opencl_finish()` |
| diffuse or sharpen (`diffuse`) | two scales, of the decomposition and of the synthesis (`iop/diffuse.c:1007`, `:1056`) | two iterations (`iop/diffuse.c:1579`) |
| blurs (`blurs`) | rows (`iop/blurs.c:563`) | — |
| astrophoto denoise (`nlmeans`) | chunks of the NL-means core (`pixel/nlmeans_core.c:362`) | two patches (`iop/nlmeans.c:312`) |
| denoise (profiled), non-local means (`denoiseprofile`) | chunks of the same core | two patches (`iop/denoiseprofile.c:2029`) |
| denoise (profiled), wavelets (`denoiseprofile`) | two scales (`iop/denoiseprofile.c:1404`) | — |
| contrast equalizer (`atrous`) | two scales (`iop/atrous.c:368`) | — |
| highlight reconstruction, guided laplacians (`highlights`) | two iterations (`iop/highlights/laplacian.c:531`) | two iterations, all three CFA paths (`laplacian.c:956`, `:1235`, `:1512`) |
| highlight reconstruction, harmonic (`highlights`) | two scales of the knee estimate (`iop/highlights/knee.c:201`), then before each clipped region (`process.c:97`) | before each region (`process.c:577`) |

Notes per module:

- **blurs** used `collapse(2)` over rows and columns. It is a plain row loop now, since the check
  has to be per row, and measured no slower (below).
- **NL-means on OpenCL does not run the core.** `nlmeans.c` and `denoiseprofile.c` both define
  `USE_NEW_IMPL_CL 0`, so their `process_cl()` runs its own patch loop (`nlmeans.c:241`,
  `denoiseprofile.c:1925`) and never calls `nlmeans_denoise_cl()` or
  `nlmeans_denoiseprofile_cl()`. Those two share one patch loop, `nlmeans_cl_run()`, which carries
  the check (`nlmeans_core.c:679`) for the day the switch flips, and nothing exercises it. A check placed only in the core does
  nothing on a GPU: the mid-run test below caught astrophoto denoise running to its end that way.
- **NL-means on CPU** does run the core. `nlmeans_denoise()` returns `int`: besides an abandoned
  run, it fails on a scratch allocation failure, from which it used to return silently with
  `outbuf` unwritten. `dt_nlmeans_param_t.pipe` replaced `pipetype`, which nothing read.
- **harmonic.** On CPU, the knee estimate (`_hl_knee_estimate()`) runs ahead of every region and
  measured 1.83 s of a 3.6 s module under gdb on the test image below, so it stops between its
  five scales and returns 1 for the driver to fail. The region loop, with its check, is
  `_harmonic_reconstruct_regions()`, run by the CPU driver and by the host middle of the OpenCL
  one. A region's own solve is not cut:
  `_region_guided_filter()` and its callees are all `void`, and a finer stop would need a return
  code through every one of them. On OpenCL the knee estimate runs on the device and is not cut;
  the first region check needs no sync, since it follows the blocking upload of the depth map. A
  device middle that stopped on the killswitch goes neither to the host middle nor to the host
  roundtrip, which would redo the run for nobody (`process.c:1011`, `:1179`).

## What is not covered, and why

Measured on the bench below, OpenCL, at 30 MP (`-d perf`, the `took` line of each module, median
of three runs):

| Module, OpenCL path | Whole module |
|---|---|
| denoise (profiled), wavelets | 0.38 s |
| contrast equalizer | 0.34 s |
| blurs | 0.21 s |

A third of a second is not worth stopping, and a sync per scale would cost a share of it. The
blurs kernel is also a single launch, with no step to stop between.

Any module not in the table of *The modules* still runs to its end: the pipe stops at the next
module.

## Measurements

### What the checks cost when nothing abandons

`ansel-cli` full-size export of `tests/benchmark/darktable-bench-3.8.xmp` on a 30.7 MP NEF,
`-d perf`, the `took` line of each module; median of three runs, before and after interleaved,
each with a fresh `--configdir` and `--library :memory:`. "Before" is `74ee852d4c`.

| Module | CPU before → after | OpenCL before → after |
|---|---|---|
| denoise (profiled), NL-means | 15.25 → 15.53 s | 2.954 → 2.968 s |
| astrophoto denoise | 9.50 → 9.50 s | 2.410 → 2.427 s |
| denoise (profiled), wavelets | 5.83 → 5.97 s | 0.378 → 0.372 s (path unchanged) |
| contrast equalizer | 5.56 → 5.78 s | 0.339 → 0.340 s (path unchanged) |
| diffuse or sharpen, 1 iteration | 4.19 → 4.23 s | 0.660 → 0.663 s (check never reached) |
| blurs | 3.04 → 2.97 s | 0.214 → 0.216 s (path unchanged) |

The two NL-means OpenCL rows are the one cost measured: +0.5 % and +0.7 %, with no overlap
between the runs (2.951–2.961 against 2.968–2.977 s), about 15 ms for one sync per patch. The
three unchanged OpenCL paths give the noise floor on GPU. On CPU, every difference is inside one
version's own spread (the contrast equalizer's +3.8 %: 5.34–5.77 s before, 5.61–5.96 s after) but
NL-means, whose +1.8 % does not overlap (15.23–15.41 against 15.47–16.08 s); an earlier series of
three runs, on a build that differed from the final one only elsewhere, gave it 15.49 → 15.50 s.
One atomic read per chunk of a 15 s loop does not cost 0.3 s; this is not settled by three runs.

The bench runs diffuse for one iteration and highlights in clip mode, which reaches neither a
diffuse OpenCL check nor any highlights check. Measured separately, the highlights rows on the
same NEF with a light history (exposure, lens, filmic, colour balance) and highlights switched to
the mode in question, median of three runs unless stated:

| Case | before → after |
|---|---|
| diffuse, bench with 10 iterations, `--width 3000` (9.9 MP), OpenCL, 5 runs each | 2.620 → 2.629 s |
| guided laplacians, 30 iterations, OpenCL | 0.449 → 0.458 s (runs overlap) |
| guided laplacians, 30 iterations, CPU | 4.09 → 4.12 s |
| harmonic, 13 regions, OpenCL | 0.966 → 0.937 s |
| harmonic, CPU | 2.05 → 1.93 s |

A first series on harmonic OpenCL, on an earlier build, measured +20 % over three runs; six
alternating runs then gave −18 % (1.40 → 1.15 s, runs 1.07–1.49 s). On this image all 13 regions
go through the CPU offload, whose host-side solve varies more than a sync per region can cost.
The device-resident path (regions above `DT_HL_CL_CPU_REGION_PX`) was not exercised.

### Output

Pixel identity cannot be the check here: the CPU export of this bench is not deterministic.
At `--width 2000`, ten runs of `74ee852d4c` gave one output eight times and two others once each,
nine runs of an intermediate build of the change gave the same majority output seven times and two others once each (max
difference 1 in 8 bits, on about 1.8 of 5.7 Mpx). Turning off astrophoto denoise, denoise
(profiled) or the contrast equalizer each made one before/after pair identical, blurs or diffuse
did not; one pair per case, so this only suggests where to look. Not investigated further.

### How long a module runs on once the killswitch is raised

`ansel-cli` under gdb: a breakpoint at the module's `process()` or `process_cl()` (its second call
for a module's second instance), then a thread writes 1 into `pipe->shutdown` through
`/proc/<pid>/mem` N seconds later and the process continues. On CPU, the figure is the module's
`took` minus N, an upper bound since it includes the stop at the breakpoint. On OpenCL, an
abandoned `process_cl()` goes through the abort path, which prints no `took`, so the figure is the
time from the write to the process exit, teardown included, for both builds. gdb slows the modules
down (diffuse took 6.4 s under it against 4.3 s in the bench). Same images and histories as
above; one run per row.

| Module, path | Raised after | Before | After |
|---|---|---|---|
| diffuse or sharpen, 1 iteration, CPU | 1 s | 5.43 s | 0.26 s |
| blurs, CPU | 1 s | 2.94 s | 0.02 s |
| denoise (profiled), NL-means, CPU | 3 s | 14.28 s | 0.09 s |
| denoise (profiled), wavelets, CPU | 1 s | 5.00 s | 0.20 s |
| astrophoto denoise, CPU | 2 s | 9.03 s | 0.15 s |
| contrast equalizer, CPU | 1.5 s | 6.05 s | 0.48 s |
| guided laplacians, 128 iterations, CPU | 2 s | 19.94 s | 0.09 s |
| harmonic, CPU | 0.5 s | 2.74 s | 0.03 s |
| diffuse or sharpen, 10 iterations, 9.9 MP, OpenCL (to exit) | 0.8 s | 2.36 s | 0.42 s |
| denoise (profiled), NL-means, OpenCL (to exit) | 1 s | 2.12 s | 0.18 s |
| astrophoto denoise, OpenCL (to exit) | 1 s | 2.16 s | 0.34 s |
| guided laplacians, 512 iterations, OpenCL (to exit) | 2 s | 5.82 s | 0.23 s |
| harmonic, OpenCL (to exit) | 0.3 s | 1.34 s | 0.41 s |

In every "after" run the module failed (on CPU, the build measured printed `returned with an error`;
see *The contract* for what it logs now), the export wrote no file,
and nothing fell back from OpenCL to CPU. In every "before" run the module ran to its end and the
pipe stopped at the next module. The contrast equalizer's 0.48 s is one full-resolution scale.

### In the darkroom

By hand on 2026-10-11, on the same NEF and bench history, window fitted (12 %), with the build of
these changes. On OpenCL the heavy modules took 5–30 ms each in the darkroom pipes, so nothing was
long enough to be stopped mid-run; the test only showed no artefact, no module failure and no
fallback. With `--disable-opencl` and diffuse set to 100 iterations, a full diffuse run took
5.4–5.6 s in the preview pipe. Moving the exposure during it stopped it twice at 2.5 s
(`returned with an error`, no blending), the next run started about 0.5 s later and went to its
end, and the image shown was complete. On opening, denoise (profiled), 3.8 s when it runs through,
was stopped four times, after 3.3, 2.4, 0.4 and 0.4 s.
