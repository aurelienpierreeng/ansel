# Neural raw denoising (rawdenoiseai)

This page documents the design of the `rawdenoiseai` module: what the neural
network actually is, what problem it solves, what is computed offline versus
at runtime, and why none of it involves sending anything anywhere.

**Executive summary for the worried:** this module is the next generation of
content-aware denoising and, mathematically, the next generation of
least-squares fitting. It shares no technology, no infrastructure and no
failure modes with chatbots or image generators. It is a fixed list of numbers,
fitted once on public data, applied deterministically to your pixels, entirely
on your machine.

[TOC]

## A neural network is a curve fit, not a chatbot

Photographers have been using fitted models for decades without calling them
"AI". It is worth walking up the ladder of familiar examples, because the
neural denoiser sits on the same ladder — just higher.

**Rung 1 — the least-squares line.** Given observed pairs $(x_k, y_k)$, fitting

$$y = a x + b$$

means finding the two numbers $(a, b)$ that minimize the total error

$$(\hat a,\hat b) = \arg\min_{a,b} \sum_k \left(y_k - (a x_k + b)\right)^2 .$$

Two things happen at two different times: an *optimizer* finds the parameters
once (fitting), then the model is *applied* forever after by plain arithmetic
(evaluation). Applying $y = \hat a x + \hat b$ involves no optimizer, no data,
no judgment — it is a pocket-calculator operation.

**Rung 2 — the ICC input matrix.** A camera input profile contains a $3\times3$
color matrix: nine numbers fitted, by exactly the same least-squares logic,
against measured color-checker patches, so that

$$\begin{pmatrix} X \\ Y \\ Z \end{pmatrix} = M \begin{pmatrix} R \\ G \\ B \end{pmatrix}$$

maps camera RGB to a standard space with minimal error over the patches. Nine
fitted parameters instead of two; applied to every pixel of every photograph
you develop; nobody has ever called a display profile "artificial
intelligence". The profile is a bag of numbers produced by an optimizer that
ran once, elsewhere, on reference data.

**Rung 3 — the neural denoiser.** Same recipe, three quantitative changes:

| | least-squares line | ICC matrix | neural denoiser |
|---|---|---|---|
| parameters | 2 | 9 | ~1.9 M (distilled) / ~7.6 M (full) |
| model form | linear | linear | stacked $3\times3$ convolutions + pointwise nonlinearity |
| fitted against | observed $(x, y)$ pairs | measured color patches | millions of (noisy, clean) image patch pairs |
| fitting happens | once, offline | once, at profiling time | once, offline (`ansel-denoise` repo) |
| evaluation | arithmetic | one matrix per pixel | convolutions per pixel |

"Training" *is* the fitting step. "Inference" *is* the evaluation step. The
epistemology is identical to the least-squares line: an optimizer minimizes an
error over known pairs, and what comes out is a **fixed set of numbers**.

**Why millions of parameters?** Because noise and fine detail are both
high-frequency signals. Any *linear* filter that attenuates high frequencies
attenuates both: that is why classical denoising always trades detail for
smoothness. Telling them apart is a *statistical judgment about context* —
"this oscillation continues the foliage texture around it; that one is
uncorrelated with anything and has exactly the amplitude the sensor's noise
model predicts at this brightness". A judgment conditioned on local context
cannot be encoded in 2 or 9 numbers; it needs a model expressive enough to
represent "what natural image textures look like, at every scale, versus what
sensor noise looks like". The millions of parameters buy a *content-aware,
spatially adaptive* filter — nothing more mystical than that.

## The task: fitting noisy against ground truth

The sensor noise model is the same one Ansel has shipped for years in
`data/noiseprofiles.json`: for a normalized raw value $x$, the observed value is

$$y = x + n, \qquad \mathrm{Var}(n) = a\,x + b$$

per RGB channel — $a$ is the photon (shot) noise gain, $b$ the read-out
variance, both measured per camera per ISO by the community profiling effort.

The denoiser $f_\theta$ is fitted to undo that corruption:

$$\hat\theta = \arg\min_\theta \;\; \mathbb{E}\; \bigl| f_\theta(y, \sigma) - x \bigr|,
\qquad \sigma(y) = \sqrt{\max(a\,y + b,\, 0)}$$

which the attentive reader will recognize as least-*absolute*-deviation
fitting — the robust L1 sibling of least squares. The pairs $(y, x)$ do not
come from anybody's photo library: clean patches $x$ are harvested from
public, consensually licensed images (raw.pixls.us CC0 archive, the PlayRaw
community sharing category), and the noise $n$ is *synthesized* on them by
sampling $(a, b)$ across the entire noise-profile database. The ground truth
is manufactured, which is precisely what makes the method clean: we know the
right answer for every training pair because we corrupted it ourselves.

Feeding $\sigma$ to the network as an input (rather than baking a noise level
into the weights) is what makes **one set of weights serve every camera and
every ISO**: a newly profiled camera is supported the moment its $(a, b)$
entries exist, with no retraining.

## Where the training data comes from

Every training pair is manufactured from two community-built, consensually
licensed ingredients — no private data enters the pipeline at any point:

**Clean image content.** Small tiles ($256\times256$ photosites, CFA-aligned,
base-ISO, well-exposed, the most textured crops of each frame) are harvested
from:

- the [raw.pixls.us](https://raw.pixls.us) archive — raw sample files
  explicitly published under **CC0 (public domain)** by their photographers to
  support open-source raw software development. It covers hundreds of camera
  models, which is what teaches the network every CFA layout and sensor
  readout convention in the wild;
- the [PlayRaw](https://discuss.pixls.us/c/playraw/30) category of the
  pixls.us forum, where photographers post their own raws under **declared
  Creative Commons licenses** expressly for others to process. This
  contributes the *content diversity* — real skin, foliage, night scenes,
  wildlife — that a decoder-testing archive lacks. The license, author and
  source topic of every file are verified at harvest time and recorded per
  tile;
- optionally, maintainer-curated images selected one by one from a personal
  library — the explicit selection being the consent step.

The harvest strips everything but a technical whitelist: tiles carry black and
white levels, CFA pattern, white balance and ISO — **no GPS coordinates, no
serial numbers, no timestamps, no author-identifying EXIF** ever enters a
training shard, even though the source files contain them.

**Noise statistics.** The $(a, b)$ variance lines come from the darktable /
Ansel **community noise-profiling effort** (`data/noiseprofiles.json`) —
hundreds of cameras measured by their owners over more than a decade. The
training samples noise across this entire database, which is why the fitted
network generalizes across sensors.

The whole dataset is **reproducible without redistribution**: a ledger records
every harvest decision (file, license, acceptance or rejection and why), the
sources are pinned by content hash, and the harvested tiles are cached
publicly so anyone can retrain the same model from the same inputs — the same
standard of provenance the noise profiles themselves have always had.

## What the "model" actually is

A `.anselnn` file is, byte for byte:

    8 bytes    magic "ANSELDN1"
    4 bytes    length of a JSON header
    JSON       {"cfg": {architecture constants}, "tensors": [{name, shape, offset, size}, ...]}
    payload    plain float32 arrays — the fitted numbers

That is all. It has the same ontological status as an ICC profile, a lens
correction database entry, or `noiseprofiles.json` itself: **a bag of fitted
constants**. It contains no executable code, no images, no user data. It
cannot observe anything, remember anything, or transmit anything — it is an
argument to a convolution routine.

Nor can it "contain" the training images in any meaningful sense: ~7.6 million
numbers were fitted against billions of pixel observations. What survives the
fitting is the *statistics of the noise-versus-detail distinction*, not
pictures — exactly as $\hat a, \hat b$ of a least-squares line retain the trend
of the data points, not the points.

Finally, the architecture is structurally *subtractive*: the network outputs an
estimate of the noise layer, which is subtracted from the input,

$$\hat x = y - g_\theta(y, \sigma).$$

It can only remove an estimated corruption from your pixels; there is no
mechanism by which it could hallucinate content into them.

## Architecture

The network is a small U-Net: an encoder that halves resolution while doubling
channel count (so deeper levels see wider context at coarser scale), a
bottleneck, and a decoder that mirrors the encoder, re-injecting the encoder's
intermediate results ("skip connections") so fine detail is never lost to the
downsampling.

\htmlonly
<pre class="mermaid">
flowchart TD
  subgraph inputs [5 input planes]
    I1[mosaic]
    I2[R/G/B one-hot CFA planes]
    I3[sigma map]
  end
  inputs --> E0[encoder level 0<br/>2x conv3x3+GELU, 16 or 32 ch]
  E0 -->|downsample /2| E1[level 1, 2x ch]
  E1 -->|/2| E2[level 2, 4x ch]
  E2 -->|/2| E3[level 3, 8x ch]
  E3 -->|/2| B[bottleneck, 16x ch]
  B -->|upsample x2| D3[decoder level 3]
  E3 -->|skip| D3
  D3 -->|x2| D2[decoder level 2]
  E2 -->|skip| D2
  D2 -->|x2| D1[decoder level 1]
  E1 -->|skip| D1
  D1 -->|x2| D0[decoder level 0]
  E0 -->|skip| D0
  D0 --> H[head: conv3x3 -> noise estimate]
  I1 --> R[output = mosaic - noise estimate]
  H --> R
</pre>
\endhtmlonly

Every box above is built from one operation the reader already knows from
sharpening and blurring: the $3\times3$ convolution

$$z_o(p) = b_o + \sum_{i} \sum_{\delta \in 3\times3} w_{o,i,\delta}\; u_i(p + \delta)$$

— a weighted average of a pixel's neighborhood. The *only* difference from a
hand-written blur kernel is that the weights $w$ were fitted instead of chosen.
Between convolutions sits the one nonlinearity,

$$\mathrm{GELU}(t) = \tfrac{t}{2}\left(1 + \mathrm{erf}\bigl(t/\sqrt{2}\bigr)\right),$$

a smooth gate that lets a channel respond selectively — this is what upgrades
a stack of averages into a context-dependent filter.

The network operates **before demosaicing**, directly on the mosaic, where
sensor noise is still per-photosite independent. (After demosaicing, the
interpolation correlates noise across neighboring pixels, which destroys the
very statistical signature the fit relies on.) The CFA layout is passed as
explicit input planes, so the same weights handle Bayer and X-Trans.

## Division of labor: offline fitting vs. runtime evaluation

\htmlonly
<pre class="mermaid">
flowchart LR
  subgraph offline [OFFLINE - ansel-denoise repository, done by developers, once]
    A[harvest clean CC-licensed<br/>base-ISO raw tiles] --> C[synthesize noise from<br/>noiseprofiles.json]
    C --> D[gradient-descent fitting<br/>hours on one GPU]
    D --> E[export .anselnn<br/>bag of fitted numbers]
    E --> F[publish: models/ + manifest<br/>sha256, revision]
  end
  subgraph runtime [RUNTIME - your machine, per image, no network access]
    G[load .anselnn once] --> H[look up camera noise profile<br/>at the image ISO]
    H --> I[build sigma map<br/>sigma = noise_level * channel_corr * sqrt max a*y+b, 0]
    I --> J[tile + convolve<br/>CPU OpenMP or GPU OpenCL]
    J --> K[subtract noise estimate<br/>hand result to demosaic]
  end
  F -.->|fetched at BUILD time,<br/>bundled in packages| G
</pre>
\endhtmlonly

Everything expensive, data-hungry, and non-deterministic (the optimizer)
happens **offline**, in the [ansel-denoise](https://github.com/aurelienpierreeng/ansel-denoise)
repository, on public data. What ships to the user is the fitted result.

At **runtime**, Ansel evaluates a fixed function. The steps, per image:

1. `commit_params()` looks up the camera's noise profile at the image ISO
   (`dt_noiseprofile_get_matching` / `_interpolate`, generic fallback for
   unprofiled cameras) — the same infrastructure `denoiseprofile` uses.
2. `process()` builds the five input planes; the $\sigma$ map is scaled by
   exactly two user-visible factors on top of the profile prediction
   $\sqrt{a\,y+b}$ — **nothing multiplies the sigma behind the GUI's back**
   (maintainer rule: conditioning factors are either baked in the model or
   shown in the GUI, and the GUI wins):
   - the *global correction* parameter (100% = trust the sliders as-is);
   - the three **per-channel correction sliders**, whose defaults
     (R 2.82, G 3.94, B 2.96) carry the whole measured calibration:
     - an exact **×2 factor**: the profile fitting tool
       (`tools/noise/noiseprofile.c`) estimates noise from the HH band of a
       decimated *lifting* Haar transform whose normalization is
       $HH = (x_{00} - x_{01} - x_{10} + x_{11})/4$, so
       $\mathrm{std}(HH) = \sigma/2$ for iid noise; the fit squares this
       into $(a, b)$ uncorrected, and every shipped profile carries $1/4$ of
       the physical variance. `denoiseprofile` was historically tuned around
       these units end to end; this module is the first consumer that needs
       $(a, b)$ as absolute physical variance — the shared profile database
       cannot change without invalidating a decade of community fits;
     - times **per-channel demosaic-attenuation medians** (R 1.41, G 1.97,
       B 1.48): the profiles are fitted on *demosaiced* pixels, and
       interpolation averages away part of the high-frequency noise — most
       on the dense green lattice — while this module reads the mosaic where
       noise is at full strength. Calibrated by measuring flat-region noise
       on raw mosaics against the profile prediction over 253 profiled
       cameras plus 64 images across ISO 64–12800 on three local bodies;
       the deviation is ISO-stable.

   A model's cfg may document the sigma convention it was trained under
   (`sigma_calibration`), but the C loader deliberately ignores it: a hidden
   model-side factor stacked with visible sliders is how the calibration
   once got applied twice (the tungsten yellow-cast field bug).

   The separate *strength* parameter is the opacity of the correction — an
   alpha blend of the inferred noise residual:
   `out = in + strength * (denoised - in)`.
3. The pixelpipe tiles the image (the network's measured receptive field sets
   the tile overlap) and the convolutions run on CPU
   (`src/common/nn_model.c`) or GPU (`data/kernels/rawdenoiseai.cl`) — both
   paths produce the same result within float rounding.
4. The predicted noise plane is subtracted and the denoised mosaic proceeds to
   demosaicing.

The evaluation is **deterministic and versioned**: same raw file, same
parameters, same model version → the same output, today and in ten years. The
module stores the model *version* and *variant* (full / distilled) in the edit
history; a version that has shipped in a stable release is frozen forever, and
any retraining becomes a new version — so updating Ansel never silently
changes the rendering of an existing edit.

| concern | where | when |
|---|---|---|
| collecting training images | offline, public CC sources | once |
| noise synthesis, fitting, validation | offline (`ansel-denoise`) | once per model version |
| model distribution | `models/` dir + sha256 manifest, fetched at build time | at packaging |
| noise profile lookup, $\sigma$ map | runtime, `src/iop/rawdenoiseai.c` | per image |
| convolutions | runtime, CPU/OpenCL | per image |
| your photographs | **never leave the pixelpipe** | — |

## Privacy: everything runs on your computer

This deserves stating bluntly, because "AI" has come to connote cloud
services that harvest their users:

- **No network access at runtime.** The module performs arithmetic on buffers
  inside the pixelpipe, like every other IOP. There is no server, no account,
  no API key, no telemetry, no "usage analytics". Unplug the network cable and
  the module behaves identically.
- **Your images never leave your machine.** Inference is local; the weights
  are read-only at runtime; there is no training code in Ansel. The model
  cannot learn from, remember, or transmit your photographs — it has no
  mechanism to do any of those things, in the same way an ICC profile has no
  mechanism to phone home.
- **The only download is the model file itself**, fetched hash-verified at
  *build* time (like downloading the software), and bundled inside packaged
  builds. Users of nightly or release packages perform no download at all.
- **The training data respected the same ethics.** Clean tiles come from
  archives whose contributors explicitly published their raws under CC
  licenses for exactly this kind of community use; provenance (source, author,
  license) is recorded per training tile in the `ansel-denoise` repo. No
  scraped social media, no unconsenting photographers, no user data.

## Same result on CPU and GPU — and what can break that

The module is a fixed function, so CPU and OpenCL must agree. They are checked
against each other *and* against the training-side reference (PyTorch) on every
shipped model, both sensor layouts, by `src/apps/ansel-nn-parity`: measured
$9\times10^{-8}$ to $2\times10^{-7}$ maximum absolute error against a
$2\times10^{-4}$ tolerance. In real renders CPU and GPU land within 0.002 % of
each other.

One thing outside Ansel can break that, and it is worth knowing about because
the symptom looks like a module bug: **the accuracy of your graphics driver's
own math library**. Ansel used to compile its kernels with
`-cl-unsafe-math-optimizations`, which permits a driver to substitute a
low-precision implementation for any standard function. Most do so harmlessly.
Intel's integrated-GPU driver does not: under that flag its `erf()` returns
exactly `0.0` for arguments below about $10^{-3}$. The activation function in
this network is $\tfrac{1}{2}x\,(1 + \mathrm{erf}(x/\sqrt{2}))$, and a
convolutional network spends most of its activations in precisely that range,
so the entire model drifted — visibly, as a fine mesh on X-Trans files.

Ansel no longer requests that flag on any vendor (it bought no measurable
speed: 0.4 % across a whole pipeline, inside run-to-run noise). Two things
follow for users:

- **Upgrading alone does not fix an affected installation.** The kernel build
  options live in a per-device `anselrc` key that is written once and never
  overwritten:
  `cldevice_v4/<n>/<device-name>/building`. If yours still reads
  `-cl-fast-relaxed-math … -cl-unsafe-math-optimizations`, replace it with
  `-cl-mad-enable -cl-no-signed-zeros` and delete the compiled kernel cache
  (`~/.cache/ansel/cached_kernels_for_*`) so the kernels rebuild.
- **You can test your own hardware.** `tools/opencl-math-accuracy.c` is a
  standalone program (it does not link against Ansel) that scores every
  OpenCL device on your machine, under every build-option set, against a
  double-precision reference, and prints the exact `anselrc` line for any
  device that needs one. See `doc/opencl-math-accuracy.md`.

## Choosing a model size, especially on X-Trans

The three sizes are a quality/time trade, and they are **not** interchangeable
renders of the same function — each is a separately fitted model.

On X-Trans sensors the smaller models leave a residual pattern correlated with
the 6×6 colour filter array. Measured on a Fuji X100F frame as the strength of
sensor-lattice structure in a flat area, relative to an undenoised render of
the same frame: quarter ≈ 970×, half ≈ 84×, large ≈ 16×. This is a capacity
limit, not a bug in the CPU or GPU code — the two devices agree to three
significant figures, and the effect falls by a factor of 60 purely by giving
the network more parameters. A network with fewer parameters has a harder time
learning to treat all 36 distinct X-Trans site types identically.

Practical guidance: on X-Trans, prefer **large** where the processing time is
acceptable, and **half** as the reasonable compromise. On Bayer sensors the
smaller models are much better behaved, the colour filter array having only
four site types to equalise. Improving the small models on X-Trans is a
training-side problem, tracked upstream in `ansel-denoise`.

## What this is not

The public conversation about "AI" is dominated by large language models and
generative image tools. This module is neither, on every axis that matters:

| | LLM / generative AI | rawdenoiseai |
|---|---|---|
| output | invented text/images | your pixels minus estimated noise |
| can hallucinate content | yes, by design | no — structurally subtractive |
| runs | in a datacenter, on their terms | on your CPU/GPU, offline |
| deterministic | no | yes, bit-stable per version |
| size | 10⁹–10¹² parameters | 2–8 million (a 7–30 MB file) |
| training cost | GWh, months, undisclosed data | hours on one GPU, public CC data, reproducible from a public repo |
| relationship to your data | often trains on it | cannot see it |

The honest genealogy of this module is not ChatGPT; it is the least-squares
line, the ICC matrix, and the wavelet shrinkage threshold — fitted models all,
each generation using more parameters to make a finer-grained distinction.
This one has enough parameters to finally make the distinction photographers
actually care about: *noise is not detail*.

## Source map

| file | role |
|---|---|
| `src/common/nn_model.{h,c}` | `.anselnn` loader + CPU executor (self-contained, no pipeline deps) |
| `src/iop/rawdenoiseai.c` | the IOP: profile lookup, $\sigma$ map + calibration, tiling, params (strength, noise level, per-channel corrections, version, variant) |
| `data/kernels/rawdenoiseai.cl` | OpenCL kernels (convolution, upsampling) |
| `data/CMakeLists.txt` | build-time hash-verified model fetch (`FETCH_NN_MODELS`) |
| [ansel-denoise](https://github.com/aurelienpierreeng/ansel-denoise) | training pipeline, data harvesting, published models (`models/`) |
| `src/tests/nn_model_test.c` | torch-vs-CPU parity selftest, builds standalone without Ansel (tolerance $2\times10^{-4}$, measured $\sim 2\times10^{-7}$) |
| `src/apps/ansel-nn-parity` | torch vs CPU vs **OpenCL** on the same fixture; needs lib_ansel because the GPU path wants a compiled program number |
| `tools/opencl-math-accuracy.c` | standalone probe: scores each OpenCL device's math library per build-option set, prints the `anselrc` line to fix a bad one |
| `doc/opencl-math-accuracy.md` | the measurements behind the kernel build options, and how to override them per device |
