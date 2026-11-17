<!-- Provenance: every finding carries the commit it was established against. -->

# Per-module notes (`src/iop`)

> **Verified against `42eca0e8fe` on 2026-09-29.**
>
> Findings that belong to one image-operation module and cost real work to establish.
>
> Each finding below is dated with the commit that established it. A finding is only as
> good as its hash: before acting on one older than the code you are changing, re-measure
> it — and re-date it here when you do. Where an earlier version of a claim was **wrong**,
> that is recorded rather than quietly corrected: how a claim was wrong is usually the more
> useful thing to know.

The drawlayer module has its own full account in [`drawlayer.md`](drawlayer.md); retouch's per-shape memo in [`retouch-result-memo.md`](retouch-result-memo.md).

## ashift: preview buffer and crop geometry

*Found `22f623c0be`, 2026-06-25.*

The reference for ashift edit-mode is the **crop module** (`src/iop/crop.c`) — same "show the
full uncropped image while editing a clipping module" problem.

**Show the full image by neutralizing the crop in `commit_params()`** (crop: `cx=cy=0,cw=ch=1`;
ashift: `cl=0,cr=1,ct=0,cb=1`). Output, input, view, and size caches must all describe the same
full frame. Do NOT widen only `roi_in` while leaving `roi_out` cropped — the pipe renders a
cropped output while the view wants the full image → preview aborts at `initialscale` and
restarts forever.

**`g->buf` comes from `process()` capture, not `peek_gui`.** During edit ashift is cache-bypass,
so `process()` runs on every render and copies its input into `g->buf`. Do NOT use
`dt_dev_pixelpipe_cache_peek_gui()` for ashift's own input — it never runs `process()`, the
intermediate is evicted before the GUI reads it → re-request loop.

**Auto-crop geometry needs only size, not pixels.** Use `piece->buf_in.width/height`
(crop-independent), NOT `roi_in`/`g->buf` dims. Use the preview pipe's `buf_in`, not the virtual
pipe.

**`dt_dev_pixelpipe_has_preview_output()` requires matching both portrait and landscape.** ashift runs before
`flip` (iop_order 16 < 20), so on portrait images its `roi_out` is landscape while preview dims
are post-flip portrait. The guard must also accept the swapped match
(`width==preview_height && height==preview_width`).

## channelmixerrgb: the illuminant detection is a GUI request, answered from the preview input

*Found `9e9b1fc76a`, 2026-11-26.*

The two "(AI) detect from image surfaces/edges" illuminants are not a state the pipeline renders.
Choosing one asks the GUI to detect the illuminant: `_detect_illuminant()` reads the module input
in the preview pipe with `dt_dev_pixelpipe_cache_peek_gui_retained()`, as `autoset()` does, runs
`auto_detect_WB()` on it, and writes the result into `x`, `y` and the illuminant (daylight, black
body or custom). When the input is not in cache yet, the request waits on `detect_wait` and
`_detect_illuminant_restart()` finishes it, unless another illuminant was chosen meanwhile. A
detection state still in a history renders as a custom illuminant at its stored `x`, `y`.

The preview pipe is the one that always holds the whole frame. The detection used to run in
`process()` on the main pipe only, and a `DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED` handler copied the
result into the parameters. That broke in two ways, measured in the darkroom on `9e9b1fc76a` with
`-d dev -d pipe -d perf`, on a Sony raw:

- at zoom == fit, the main pipe has the preview's ROI, so it computes the same hashes and takes the
  preview's outputs from cache without running the module (no `processed 'color calibration'` line
  for it). The handler still fired and committed the result field as initialised, `xy=[nan 0]`:
  the image went black;
- zoomed in, the module ran on the visible part only (602×709 at 100 %) and gave `x=0.4806
  y=0.3618`, where the whole frame gives `x=0.3458 y=0.3587`.

Read from the preview input (1198×797 here), the detection gives `x=0.3458 y=0.3587` at fit and at
100 % alike, and one undo takes it back.

## colorbalancergb: an alignment promised to the compiler must be one the struct guarantees

*Found `0baeb88ed8`, 2026-10-16.*

`process()` tells the compiler that seven 4-float vectors of `dt_iop_colorbalancergb_data_t` are
16-byte aligned (`__builtin_assume_aligned(d->…, 16)`), and `scalar_product()` repeats it for its
second argument (`aligned(v_2:16)`). Those fields are therefore declared `dt_aligned_pixel_t`, at
the head of the struct, so the type carries the promise and no padding lands in the hashed prefix.
Declared as plain `float[4]` among scalars, `chroma`, `saturation` and `brilliance` sat at offsets
0x48, 0x64 and 0x78 while `global` at 0 was also assumed aligned: two contradictory promises,
undefined behaviour the compiler resolves as it likes.

Measured on the Windows nightly DLLs: Clang 23.1.3 (nightly `0baeb88ed8`) derived from `chroma`
that offset 0xa8 (`checker_color_1`) was aligned and loaded it with `movaps`, unconditionally at the
top of `process()`; Clang 22.1.8 (nightlies `a4eac9fe11`, `7230d1b355`, same source for those
files) and a local clang-19 build used `movups`. `piece->data` comes from `dt_calloc_align()`, so
0xa8 is 8 mod 16 and every call of `process()` faulted — it surfaced as a crash at startup, on a
thumbnail rendered on the CPU. Nothing in the source changed: a compiler upgrade is enough to turn
such a promise into a crash, on one platform only.

The report read `Reading from location FFFFFFFFFFFFFFFF`; how to go from such a report to the
instruction is in [`sentry.md`](sentry.md#windows-crash-report).

## drawlayer: realtime stroke correctness

*Found `22f623c0be`, 2026-06-25.*

**Stroke truncation:** `dt_drawlayer_commit_dabs` must guard on `painting_active` for BOTH quiet
and record-history commits. `_build_runtime_schedule` schedules quiet commits for `GUI_SCROLL`
and `GUI_SYNC_TEMP_BUFFERS` — these fire during active strokes and will truncate the path if
`commit_dabs` does not early-return. The fix: `if(g->manager.painting_active){ ... return TRUE; }`.

**Realtime trigger / hover thrash:** `_update_realtime_state` must track `painting_active` only.
The `GUI_RAW_INPUT` / `SAMPLE` override that set `realtime_active=TRUE` regardless of
`painting_active` caused hover mouse-moves to toggle realtime ON/OFF on every pixel → ~44ms
`resync_history_main` at each stroke boundary. Only an actual stroke (`STROKE_BEGIN`/`STROKE_END`)
should enter/leave realtime.

**Partial composite gate:** gate the damage-limited resample on the stable per-layer identity
`process->base_patch.cache_hash` (NOT on `piece->global_hash` — the heartbeat bumps
`stroke_commit_hash` every frame, so `global_hash` changes every realtime frame).

**Transient-params channel:** the realtime heartbeat (`_publish_backend_progress` in `worker.c`)
publishes via `dt_dev_transient_params_set` instead of `add_history_item`, avoiding per-heartbeat
undo/DB churn. History is written only at the real commit. Crop/ashift use `resync_history_all`
(full, all pipes); drawlayer heartbeat raises `TOP_CHANGED` + redraw (fast, non-geometry). The
two must NOT be mixed — routing crop's geometry through `_sync_focused_in_place` (partial)
mishandles the warm cropped→uncropped geometry change.

## negadoctor: the shadows color cast scales the bias in v1; v2 has one black point per channel

*Found on top of `6a9005ef9c`, 2026-11-13; v2's black points on top of the same commit, 2026-11-14; the black point picker's pixel on top of `8e77b45cff`, 2026-11-14; what the black points and the illuminant gain do to each tone on top of `8e77b45cff`, 2026-11-15; v2's saturation on top of `b13472e7f4`, 2026-11-16.*

`process()` prints `exposure · (1 + black) − exposure · 10^(wb_high[c] · (B[c] − D[c] / D_max))`,
`D[c]` being the density above the film base. The density printed at the paper black
(`exposure · black`) is therefore `B[c] · D_max` in each channel, `B[c]` being `_black_point()`:
`offset · wb_low[c]` in v1, `black_point[c]` in v2. The `version` param selects it.

The pickers solve against the coefficients `process()` applies: `_data_from_params()` derives them
from the params, `commit_params()` and the seven pickers that read the film's densities call it, and the
paper black and print exposure pickers evaluate the density through `_corrected_density()`, the same
expression as `process()`. A picker that rebuilds the formula from the params by itself drifts from
the render. The exposure picker used to leave `wb_high` out of the black point term, an error of
`(wb_high[c] − 1) · B[c]` in corrected density, nil only with `wb_high` at 1: replayed on the module
input of 20250921_0007 (v2, bias 0.135) with `wb_high` 1/1.1/1.25, its densest pixel printed at 0.953
instead of 0.96, and at 0.96 once the term is in. On black and white film,
every picker used the three `Dmin` components that the Dmin picker stores, while the render uses only
the first. Measured on 2026-11-12, on top of `6a9005ef9c`, by replaying both versions in Python on a
black and white pick with `Dmin` stored as (1, 0.45, 0.25): with the D max picker built from the
params, the densest pick landed at 1.25 × D max instead of 1; with the exposure picker built the same
way, its print reached 1.012 instead of the 0.96 target.

In v1 a shadows offset moves the black point by `(wb_low[c] − 1) · offset`: nothing at zero bias, and
the other way with a negative bias, such as −0.05 (−0.10 D), the bias default of params v1 and v2.
The default is now 0, Cineon's reference black: code value 95 is the film base, 0.000 D above D-min
in Kodak's recorder aims (H-387). The bias corrects how the film was exposed when shooting, not the scan:
a scan exposure scales D min and the pixel alike and cancels in `log10(D min / pixel)` once the base is
picked on the same scan (measured on 20250921_0007: +1 EV gives the same render). Measured by replaying
`process()` in Python on a grey ramp, with image 18364's D min, D max and gamma, paper black and print
exposure picked once per bias and then kept: a red shadows offset of 1.2 changes the red output at
0.4 D above the base by −0.62 EV at bias +0.134 D, 0 at bias 0 and +0.41 EV at −0.102 D.

In v2 the bias and the cast are one parameter, `black_point[c]`, the density of the blacks above the
film base in each channel, so the cast is a difference between channels rather than a multiplier
that needs a bias. v2 first kept the bias and added a dye density per channel,
`B[c] = offset − shadows_density[c]`: dye in the layer channel c mostly sees, as a color filter in the
enlarger, which the same replay showed to match 0.027 D really added to the film's red density, to
7e-20. A black point slider acts the other way from such a dye: raising it darkens that channel of
the print, so the blacks get the complementary color.

A black point moves the shadows and hardly the rest, so it does not correct the tint of the image: the
print curve `(1 + black − E)^grade` turns a density offset into a large change near the blacks only.
Measured by replaying `process()` in Python (D max 2.05, paper black 0.0755, grade 4, gloss 0.75), a black
point lowered by 0.05 D in one channel raises that channel of the print by 1.93 EV at 0.05 D above the
blacks, 0.59 EV at 0.30 D, 0.21 EV at 0.75 D (Cineon's mid grey) and 0.03 EV at 2.05 D. Under an enlarger
the same filter moves the mid-tones as well, the paper's curve being an S in log exposure.

The illuminant gain `wb_high[c]` is neither a paper tint nor a preflash: it multiplies the slope of the
paper's log exposure against the film's density in one channel, about the black point, so it is a contrast
per layer, which corrects crossed curves and which an enlarger has no control for. Measured by replaying
`process()` in Python (D max 2.05, paper black 0.0755, grade 4, gloss 0.75), gain 1.46 against 1: the print
is the same at the black point and at the paper white (10 D above the blacks), and 2.71, 2.18 and 1.27 times
higher at 0.30, 0.75 and 2.05 D. A tint would scale every value alike, white included; a preflash, a uniform
exposure, is in this model a paper black per channel.

The black point picker takes the pixel of the
area whose densest channel is the least dense above the film base, the pixel clear in all three
channels (`DT_COLOR_PICKER_EXTREMES_MAX_OF_MIN` on `log10(pixel / Dmin)`, see
[`color-picker.md`](color-picker.md)), and gives its three densities to the three black points, so
that it prints neutral at the paper black.

It first took the clearest pixel of the clearest channel, the channel the bias picker keeps, which is
wrong on a saturated color: a red object is clear on the film in blue and green but dense in red.
Measured on top of `8e77b45cff`, 2026-11-14, by dumping the module input of image 51159 (Agfa HDC 200,
D max 2.050) on a box holding a red glass and replaying both rules in Python: the old one took a pixel
of the glass, black points 0.811 / 0.190 / −0.031 D, with 94.8 % of the box printing below the red black
point; the pixel clear in all channels gives 0.016 / 0.033 / 0.033 D, 0.56 % of the box below it in
blue at most. On 20250921_0007 (D max 1.429), on a box of blacks, both rules land within 3 px of each
other and within 0.01 D (0.198 / 0.211 / 0.220 D against 0.192 / 0.207 / 0.228 D). Keeping the rule
but choosing among the three per-channel clearest pixels the one clear in all channels is not enough:
on 300 random boxes of 51159 it differed from the scan of every pixel by more than 0.05 D in 34, by
0.26 D at most.

The paper black picker has the same flaw and the same fix. It takes `black` as the highest, over the
channels, of `0.1 − (1 − 10^corrected density)`; it used to evaluate it on each channel's clearest value,
so a saturated color lifted the blacks to keep its darkest channel above the paper. It now takes the
pixel the print makes dark in all three channels, `DT_COLOR_PICKER_EXTREMES_MAX_OF_MIN` on the corrected
density `_corrected_density()` gives, and evaluates the three channels of that pixel. Measured on top of
`8e77b45cff`, 2026-11-14, on 300 random boxes of the module input of 51159 with the black points picked
as above (`wb_high` 1.461 / 1.085 / 1): the old rule set `black` higher by 0.013 in median, 0.092 at the
95th percentile and 0.214 at most. The black point picker's scale, `1 / Dmin` alone, is not the print's:
`wb_high` and the black points differ between channels, and that pixel's `black` was off from the exact
one by 0.037 at the 95th percentile and 0.136 at most. Both pickers ask negadoctor for their scale
(`_picker_channel_scale()`) at each sample. The helper's C finds the same pixel as the exact rule in
float64 Python on all 1,200 boxes tried (300 per picker, on 51159 and 20250921_0007).

`legacy_params()` maps params v1 and v2 to `DT_NEGADOCTOR_V1`, so older edits render as before, and
fills `black_point[c] = offset · wb_low[c]`; new instances and the built-in presets are v2. Switching
in the GUI converts with the same product one way, which keeps the image. The other way takes the
black point farthest from the base as the bias and divides the others by it, which keeps the image
except where v1 cannot hold it: black points of opposite signs, all at zero but one, or ratios outside
`wb_low`'s 0.05–2. v1 keeps its own sliders, swatch, normalize and shadows picker, hidden in v2.

The paper grade raises the colors with the contrast: `process()` used to raise each channel of the print to
`gamma`, so the ratios between channels, the saturation, grew as fast as the tones. Replayed on the module
input of 20261114_0007_01 (51150, the Agfa HDC 200 tree, with its own input profile) with the wall kept at
the same print value, the 95th percentile of the reds' chroma went from 103 to 110 and 121 at grades 4.5, 6
and 8.47, and the reds outside sRGB from 30 % to 42 % and 56 %. v2 prints `Y^gamma · (RGB / Y)^color_gamma`
instead, `Y` being the luminance of the print in the working RGB (`_working_luminance()`), and
`color_gamma = gamma · (1 + saturation)`. A neutral has `RGB = Y` and prints as before at any saturation; at
0 the expression is each channel raised to `gamma`, and `process()` takes that path, so that a v2 edit at 0
renders bit for bit as before (measured with `ansel-cli` on 51150, both builds). At grade 8.47 with the colors
kept at grade 4.5 (saturation −47 %), the replay gave a chroma of 55 and 31 % of the reds outside sRGB, and
the same tones as grade 8.47 (L 0 / 72 at the 10th and 90th percentiles). A saturation of −100 % is not a
neutral rendering of the scene but a grey print: at an exponent of 1, the reds fell to a chroma of 9, since
the grade is what gives the negative its colors.

The saturation is stored in `offset`, which v2 does not use: params keep their size, and v2 edits, all at
`offset` 0 when this was written (24 of them in the library this was measured on), keep their render. The
field keeps its name and description, so whatever shows params by their introspection names shows a change of
the v2 saturation as one of "film exposure bias". Switching from v1 to v2 sets it to 0 after building the
black points from it; switching back rebuilds it from the black points. Its slider is a second
`dt_bauhaus_slider_from_params()` on the same field, relabelled and shown in v2 only.

In v2 the wand sets the saturation too, from the same aims: the colors keep the scene's ratio between
grey and white, 0.90 / 0.18, one to one, as the tones do, so `color_gamma = log(0.90 / 0.18) / (print_white −
print_grey)`, the grade the wand would take without the paper gloss. Grade 4, the module's default, comes with
no source and was not used. The aims say nothing about the colors themselves, so the rule
is an extension of them, not a quote: what it takes away is only the gloss's share of the grade. With the
gloss at 0.75 that is about −5 %; on 51150 with the gloss at 0 and the paper black at 0.1017, the wand's grade
is 12.92 and the saturation −34 %, replayed in Python on top of `b13472e7f4`, 2026-11-16. It does not remove
what makes saturated reds stand out on 51150: their green and blue sit 0.26 and 0.15 D above the blacks, where
the print curve's log slope times the grade is 8.8 and 14.4 per D, against 2.3 at Cineon's absolute grey (0.75 D) and 2.4 for
their red. That is the shape of the print curve in the shadows, which no single exponent corrects.

Units: a density is the log10 of a transmission ratio, so 1 D = 10 dB = 3.32 EV; D max and the bias
used to be labelled " dB", ten times off. `offset` and `black_point` are stored as fractions of
D max and shown as densities through the bauhaus factor, in D or in printer points (0.025 log exposure, so
40 to the density unit), as `plugins/darkroom/negadoctor/density_unit` says. Both are true units of what the
sliders hold: `process()` takes `10^corrected = (10^(B[c] · D_max) · 10^(−D[c]))^(wb_high[c] / D_max)`, so the
black point multiplies the light through the negative as a printer's lamp does, and in D it is that
printing log exposure. CC (Kodak color compensating filters, 0.01 D) was offered too and was removed: it
counts the density of a colored filter, whereas the bias is the same in all channels (a neutral filter is
counted in D, as ND) and a black point reads with the sign opposite to the filter it stands for, raising it
being removing filtration. The channels are those of the working RGB, which negadoctor processes in after
the input profile, not Kodak printing density: a value in points is a lab printer's only as far as these
channels are the print stock's sensitivities. Neither the factor nor the unit is a param: whatever sets D max
without its slider callback (its picker, under `dt_gui_freeze_begin()`) calls
`_density_sliders_update()` itself.

The wand of print properties is an area picker ("grade and exposure from the area", whole frame by
default). It sets print exposure and grade from the film settings and the paper black as they stand, which
it leaves alone: the area's mean prints at 18 % and a density 0.43 D above it at 90 %, Cineon's mid grey
(code 470) and diffuse white (code 685) 215 codes of 0.002 D apart, in geometric mean over RGB: two equations
linear in grade and `log(exposure)`. The mean is that of the transmittance, the large-area integrated density
by which printers set their exposure frame by frame (Evans' integration to grey). A paper black low enough to
print the grey at zero or below in some channel leaves its log mean infinite or NaN, and the wand then keeps
exposure and grade.

It used to anchor the grey and white at Cineon's absolute densities, 0.75 and 1.18 D above the blacks, and
printed underexposed frames dark. Measured on two Agfa HDC 200 frames, 51150 and 51154, replayed in Python
from `ansel-cli` float exports with negadoctor off: their integrated densities are 0.41 and 0.26 D and their
diffuse white (a white dog) about 0.6 D, while the roll's fogged leader reads 1.96 D above the base on the
D max picker, against 1.86 D for Cineon's code 1023: the film fills Cineon's scale, so the frames sit 0.35
to 0.6 D under a normal exposure. The absolute aims gave grades 8.99 and 9.86, the dog at L* 45 and 32 and
12 % and 36 % of black pixels; anchored on the frame they give 4.31 and 3.58, the dog at L* 87 and 93 with the
shadows open. In the GUI on 51150, with D max 1.99 and the paper black at 0.10, the picker gave grade 4.29 and
+0.39 EV, the dog at L* 90 on the screenshot. Anchoring the grey on D max instead (D max − 1.106 D, Cineon's
1023 − 470) puts it at 0.85 D with the leader's value, grades 11.3 and 12.2, the dog at L* 32 and 20: a roll
constant cannot follow the exposure of each frame, which is what the printer's exposure is for. Measured on
top of `b13472e7f4`, 2026-11-16.

The aims crush the shadows wherever they sit: the print curve, `log(output)` against density, is concave
whatever its parameters, so no grade, exposure and black fit grey, white and a shadow point of Cineon's
line together. With the absolute aims, 0.30 D above the blacks printed at 0.001 to 0.003 against 0.033 on
that line.

## retouch: what a shape costs per frame is its geometry, and most of it is ROI planning

*Found `1019fcd2e0`, 2026-09-24.*

Nothing about a shape's rasterisation reads a pixel. Its bounding box, its clone source's box and
its mask resampled into the layer are pure functions of the shape's own geometry and of the
transformation chain above the module, so moving ONE shape leaves every other shape's answers
bit-identical. They were recomputed anyway, on every frame, in both pipes — and at full sensor
resolution whatever the zoom, since `_circle_get_area()` and friends work in
`pipe->iwidth`/`iheight`: a 500 px shape back-transforms 250 000 points through every distorting
module above, for a mask that `rt_fill_scaled_mask()` then samples at one pixel in a hundred.

**The cost is dominated by `modify_roi_in()`, not by `process()`**, and that is the part the
obvious reading of the module misses. `rt_extend_roi_in_for_clone()` calls
`rt_extend_roi_in_from_source_clones()` per shape, which walks the shapes again: O(shapes²) calls
to `dt_masks_get_source_area()`, and for a brush or a polygon each one regenerates the whole
outline. Measured on a 141-shape image exported at 2000 px, with `-d perf -d masks`: 3.5 s of
mask rasterisation, 3.1 s of it in ROI planning, and a second export of the same image in the
same process still paid the 3.1 s although the module's output was served from the cache.

Two memos, both in the shared pixelpipe cache, both keyed on `rt_geometry_base_hash()` —
`piece->upstream_hash` plus this module's `iop_order`, `pipe->iwidth`/`iheight` and
`pipe->mask_rasterization_step`, which reach no hash of the pipeline's own — and then on the
shape's own `dt_masks_form_get_own_hash()`:

- **the two boxes** (`rt_shape_box()`), one entry per shape. Keyed on nothing pipe-specific, so
  the FULL, preview and export pipes share it. It is what turns that O(shapes²) inner loop into
  arithmetic.
- **the scaled mask** (`rt_shape_scaled_mask()`), one entry per shape, per ROI, per source
  offset. Its entry carries the area it was rasterised from in a `RT_MASK_MEMO_HEADER`-wide
  header ahead of the pixels, so a hit answers without calling `dt_masks_get_area()` at all —
  the point being that for a brush that call *is* the outline generation, i.e. most of what the
  memo exists to avoid. The header is a cache line wide so the pixels keep their alignment.

Same measurement afterwards: 0.34 s on the first export, 0 on the second. Exports are
bit-identical, CPU and OpenCL alike.

Four things a reviewer would otherwise change:

- **Without a memo, compute only the box that was asked for.** `rt_shape_box()` takes an
  `rt_box_t`; the entry holds both because another pass over the same shapes wants the other one,
  but a caller that cannot memoise must not pay for a second outline nothing will read. Computing
  both unconditionally doubled the measurement above, exactly, before this was split.
- **There is no separate "is the shape in this layer" test any more.** `rt_scaled_mask_roi()`
  intersects the shape's area with the layer, source offset included, and a shape that draws
  nothing there comes out too small to have an effect — the same answer
  `dt_masks_form_is_in_roi()` gave, reached without rasterising the area a second time.
- **A hash identifies content, never a size.** A mask entry is used only when the header is sane
  and the line is large enough for the ROI derived from it; the arena rounds a request up, so the
  test is `>=`, never `==`.
- **The mask buffer is read-only and shared.** Every consumer reads it through a `const float *`,
  which is what lets several pipes hold the same line; `rt_release_shape()` is the only place that
  hands it back, dropping the read lock and the reference for a memoised one and freeing a local
  buffer otherwise.

**`-d perf` prints what a frame cost, in two lines the pipeline's own timings cannot give.** One
per ROI planning pass and one per render, per pipe:

```
[retouch] FULL     modify_roi_in: 1 stabilisation pass(es), boxes 10010 (10009 memo / 1 rasterised), 0.017 s
[retouch] PREVIEW  process on GPU 823x885: 140 shape(s), masks 140 (139 memo / 1 rasterised),
                   boxes 1 (1 memo / 0 rasterised), algorithms 0.471 s, total 0.520 s
```

The pipeline's `processed \`Retouch'` line covers `process()` only, so on its own it hides the
half of the cost that used to dominate. The counters are built only when the channel is on
(`rt_perf_enabled()`, `ctx->stats` NULL otherwise), and `boxes 1` in a render line is how you see
the mask memo's header paying: the 139 memoised masks needed no area of their own.

**`algorithms` is a sum, and a sum cannot say which shape is expensive.** A drag re-solves the
moved shape at a new position while every other one is bit-identical, and `_heal_laplace_loop()`
(`pixel/heal.c`) runs Gauss-Seidel to `max_iter` but breaks once the squared residual falls under
its threshold, so one shape's cost follows the content it lands on: measured at a working zoom, the
same three shapes cost between 18 and 178 ms per frame, of which the dragged one alone is 9 to
168 ms while the two beside it are 8 to 30 ms together. Nothing in this line separates the two.
Before concluding from it that memoising the static shapes would pay, record the costliest single
member -- a few lines around the existing `algo_start` timing, kept only as long as the question
is open. Reading the sum as if it described the static shapes is how the figure of 86-88 % below
came about.

Measured on the 141-shape image, dragging one shape in the darkroom: ROI planning down to 1-18 ms
with every box answered from the memo, and the mask memo answering 139 of 140 whenever the layer
ROI holds still.

**What is left is not worth memoising, and the arithmetic says so at the zoom people retouch at.**
A drag at 1:1 to 4:1 puts only a handful of shapes inside the viewport -- three of a hundred and
forty, measured -- and of those three the cost is the one being dragged, whose heal is re-solved
at a new position every frame: `dt_heal()`'s Gauss-Seidel loop stops on convergence
(`pixel/heal.c`), so it swings between 9 and 168 ms with the content it lands on, while the static
shapes beside it cost 8 to 30 ms together. The module is then 5 % of a 550 ms frame, 92 % of which
is a downstream `diffuse or sharpen` instance doing legitimate work. A memo of each shape's
*result* would buy those 8 to 30 ms, for a transitive dependency key and patches in the shared
cache. `doc/retouch-result-memo.md` is the design, the measurements and the verdict; the short
version is that the dependency graph is ROI-relative, so it shrinks with the very zoom that makes
the memo worth having, and the two effects cancel.

**Two traps in reading `-d perf` here**, both of which cost a wrong conclusion before the counter
was fixed. The aggregate `algorithms` figure cannot tell "the static shapes are expensive" from
"the one being dragged is", which is why the line also reports the costliest single shape and the
sum of the others. And the FULL and preview pipes do NOT both render per drag frame: only FULL
does, the preview rendering once when the button comes up -- so a preview figure is a per-gesture
cost, not a per-frame one. An earlier reading of these lines put the algorithms at 86-88 % of a
drag frame; that was a fit-zoom aggregate read as if it were the interactive case.

## retouch and spots: everything on the pipeline thread resolves shapes through `pipe->forms`, never `self->dev->forms`

*Found `02d93f76ff`, 2026-09-18.*

Two families of retouch code run on the pipeline/worker/CL thread, not the GUI thread: the
`dwt_decompose()`/`dwt_decompose_cl()` callbacks `rt_process_forms()`/`rt_process_forms_cl()`, which
apply each shape's clone/heal/blur/fill, and the ROI planning behind `modify_roi_in()`
(`rt_compute_roi_in()`, `rt_extend_roi_in_for_clone()`, `rt_extend_roi_in_from_source_clones()`),
which widens the input to cover every source area. Both resolve the module's mask group and each
shape in `pipe->forms` — the refcounted, frozen snapshot of the run (see "Forms are refcounted, not
deep-copied" above) — through `dt_masks_get_from_id_in_pipe()` (`develop/masks.h`), wrapped by
`rt_pipe_group_members()` and `rt_pipe_member_form()`, and read the group id from
`piece->blendop_data`, never from `self->blend_params`. `iop/spots.c`'s `modify_roi_in()` and
`_process()` (which `distort_mask()` reuses) follow the same rule with the same resolver. The CPU and OpenCL callbacks share their whole per-shape preamble (lookup,
scale and layer checks, mask, source offset) through `rt_prepare_shape()`, so the two paths cannot
drift on which shapes they apply.

`dt_masks_get_from_id(self->dev, id)` reads the live, GUI-owned `dev->forms` with no lock and no
reference held, and that is unsafe even for the long-lived darkroom `dev`: while the user edits a
shape, the GUI thread's copy-on-write replaces it in `dev->forms` and drops the old one, so a
pipeline walking that shape's `points` reads freed memory. That is how the ROI planning crashed, in
`g_list_length()` under `_polygon_get_area()`, while the GUI thread was committing the image's
history. Export and snapshot devs (`imageio_core.c`, `dev_snapshot.c`'s `frozen`) are the other
reason: they are built and torn down around a single run.

The snapshot exists during ROI planning because the pipeline guarantees it there:
`dt_dev_pixelpipe_process()` takes it BEFORE `dt_dev_pixelpipe_get_roi_in()`, so planning and
processing see the same shapes, and `dt_dev_pixelpipe_get_roi_in()` itself takes a temporary one
for the length of the walk when called with none (the darkroom's `_update_darkroom_roi` path in
`develop.c` plans outside any run). `commit_params()` and `rt_resynch_params()` run on the GUI side
and fall back to a lock-guarded `self->dev->forms` when `pipe->forms` is not populated.
`rt_masks_get_delta_to_destination()`, `dt_masks_get_area()` and
`dt_masks_get_mask()` take an already-resolved `dt_masks_form_t*` and do not look up by id.

## dev_snapshot.c: the `history_override` path must resync `frozen->forms` too, not just `frozen->history`

*Found `af036c7f1b`, 2026-08-09.*

The darkroom "Snapshot" feature (`libs/snapshots.c`'s `_lib_snapshot_capture_state()`) captures the
**live, possibly-uncommitted** `dev->history` — a duplicate taken under `history_mutex` — and hands
it to `dt_dev_snapshot_capture()` as `history_override`, precisely so a shape drawn a second ago,
before any history commit, still shows up in the frozen comparison. `dt_dev_snapshot_capture()`
splices that duplicate straight into a fresh `frozen` dev's `frozen->history`/`iop_order_list`, and
resolves each `hist->module` — but never touches `frozen->forms`. It stays at whatever
`dt_dev_load_image(frozen, imgid)` read a few lines earlier from the image's *saved*
`main.masks_history` — the on-disk state as of the last commit, not the override's live one.

Module params/blend_params don't have this problem: `dt_dev_pixelpipe_synch_all()` →
`_sync_pipe_nodes_from_history()` (`dev_pixelpipe.c`) walks `pipe->dev->history` itself and commits
`hist->params`/`hist->blend_params` per node independently of `dt_dev_load_image()`'s earlier read,
so a module's own param blob — retouch's `rt_forms[]` array included, with the freshly-drawn shape's
`formid`/`scale`/`algorithm` — is correctly the override's. But that blob only names the shape by
id; the geometry lives in `dev->forms`/`pipe->forms` (see the entry above), and a module needing mask
history resolves `blend_params->mask_id` against a group that `pipe->forms` (snapshotted from
`frozen->forms` at `dt_dev_pixelpipe_process()` start) doesn't contain. The shape's params exist,
its parent group doesn't — `dt_masks_get_from_id_ext(pipe->forms, mask_id)` returns `NULL`, and
`rt_process_forms()`/`_cl()` return early with no shapes applied, no error printed either (a
`grp == NULL` group lookup is a silent no-op by design, not a logged failure).

Fixed by re-deriving `frozen->forms` inside the `history_override` block with the same accumulation
rule `dt_dev_pop_history_items_ext()` uses elsewhere: walk the (just-spliced) `frozen->history` up to
`history_end_override`, keep the last non-`NULL` `hist->forms`, and call
`dt_masks_replace_current_forms(frozen, forms)` before `dt_dev_set_history_end_ext()`. `hist->forms`
is already a per-commit snapshot (refcounted, shared by reference — see "Forms are refcounted, not
deep-copied"), so this is a cheap re-point, not a copy. `duplicate.c`'s call site
(`dt_dev_snapshot_capture(&d->preview, dev, imgid, NULL, NULL, -1)`) passes no override and never
enters this block — it already gets correct forms from `dt_dev_load_image()`'s normal DB read, since
it is snapshotting an already-saved image, not a live in-progress edit.

## retouch: the "Square root" heal algorithm interpolates in the square-root domain, and only where there is a level

*Found `7210d07b65`, 2026-09-21.*

`dt_heal()` (`pixel/heal.c`) solves Laplace on the destination − source difference and adds the
harmonic correction back to the source. The "Linear" algorithm (`DT_HEAL_DOMAIN_LINEAR`) does it on
scene-linear values, so a source brighter than its target keeps its *absolute* noise on a darker
base; the display encoding is steeper in the shadows, and the patch comes out visibly noisier than
what surrounds it. Measured on a heal whose source was ~2× brighter in linear: high-pass noise ×1.49 /
×1.35 / ×1.34 (R/G/B) against the target, matching the predicted `(Ls/Lt)^(1 − 1/2.4)` exactly.

The "Square root" algorithm (`DT_HEAL_DOMAIN_SQRT`) does the same on `sqrt(max(x, 0) + 1e-3)`, the
variance-stabilising transform of shot noise, which brought that patch's noise back to the target's
own (0.0208 vs 0.0214). A log (multiplicative) domain was measured too and rejected: it scales noise by the level
ratio rather than its square root and smooths the patch (0.016). The fourth channel stays linear,
and a negative source value keeps its negative part.

`heal_algorithm` is a module parameter (combobox "Linear"/"Square root", default Square root);
`legacy_params()` maps every older params version to Linear, so an existing edit renders
bit-identically. v4 has shipped in no round-numbered release, so it is still open (see the
stored-format version rule under "Architectural rules"): a new param is appended to the end of v4,
and every conversion in `legacy_params()` starts from the defaults, so it needs no change there.
`rt_heal_domain()` applies the square root only to scale 0 and the wavelet residual: detail scales
and merged layers are signed, zero-mean differences, and they heal linearly whatever the algorithm.
The OpenCL path runs the same CPU `dt_heal()`, so it takes the domain as an argument rather than a
kernel of its own.

## retouch: combining the mask/wavelet-scale/suppress preview toggles

*Found `041466765a`, 2026-07-31.*

`bt_showmask` (`g->mask_display`), `bt_display_wavelet_scale` (`g->display_wavelet_scale`), and
`bt_suppress` (`g->suppress_mask`, "temporarily switch off shapes") are three independent preview
toggles. Getting any *pair* of them to combine correctly required three separate fixes, found only
by adding `dt_print(DT_DEBUG_ALWAYS, ...)` traces (never raw `fprintf(stderr, ...)` — it isn't
flushed and is easily lost if the process doesn't exit cleanly) at each stage and, for the final
one, an actual GPU buffer readback (`dt_opencl_read_host_from_device_raw`) — reasoning about the
hash/cache chain from source alone kept landing on plausible-but-wrong theories.

**1. `bypass_cache_variant` must be gated to the FULL pipe, like `request_mask_display` already is.**
`dt_iop_module_t.bypass_cache` is a single shared boolean: switching between combinations of the
three toggles that all keep it `TRUE` (e.g. suppress toggled on top of an already-active
wavelet-scale preview) doesn't change it, so the pipeline hash doesn't change either, and the
stale pre-toggle frame keeps being served. Fixed by adding `dt_iop_module_t.bypass_cache_variant`
(an opaque per-module int any module can set to disambiguate *which* combination is active,
alongside `dt_iop_set_cache_bypass()`) and folding it into `dt_pixelpipe_get_global_hash()`. That
alone still wasn't enough: retouch's actual preview effect only ever applies to `pipe ==
self->dev->pipe` (the darkroom FULL pipe) — `preview`/`virtual-preview` always render as if none
of the toggles were active — but `bypass_cache`/`bypass_cache_variant` live on the shared
`dt_iop_module_t` and so read the same non-zero value for every pipe type. Left ungated, a
preview-pipe run with the same ROI (e.g. at zoom == fit) computes the identical hash chain despite
publishing different pixels, and the pixel cache's cross-pipe "another pipe already owns this
exact hash" reuse path (`DT_DEV_PIXELPIPE_CACHE_WRITABLE_EXACT_HIT` in
`dt_dev_pixelpipe_cache_get_writable`) lets either pipe silently serve the other's stale content.
`bypass_cache_variant`'s hash contribution must be zeroed for non-FULL pipes exactly like
`request_mask_display` already is, in the same `if(pipe->type == DT_DEV_PIXELPIPE_FULL)` block in
`dt_pixelpipe_get_global_hash()`.

**2. `process_cl()`'s "expose mask" condition must match `process_internal()`'s exactly.** The CPU
path gates on `g->mask_display || display_wavelet_scale`; the OpenCL path had drifted to
`g->mask_display` alone. A wavelet-only OpenCL preview therefore never cleared alpha, never set
`pipe->mask_display`, and so never made the downstream color-pipeline modules take the
mask-display passthrough shortcut in `develop/pixelpipe_hb.c:994` — they ran their normal
processing (color management etc.) on the wavelet-domain buffer instead of being skipped. Same
class of bug as the CFA-phase and highlights-reconstruction CPU/OpenCL divergences documented
above: any GUI-only branch condition duplicated between a module's `process()` and `process_cl()`
is a standing invitation for exactly this drift, since nothing forces the two to be reviewed
together.

**3. `rt_adjust_levels()` clobbers the alpha channel — the actual root cause of "mask + wavelet
scale together shows nothing but checkerboard."** This function (shared verbatim by both the CPU
path and `rt_adjust_levels_cl`, which round-trips through it on a host-side copy of the GPU
buffer) is called whenever *any* single wavelet scale is being previewed
(`dwt_p->return_layer > 0`), to contrast-stretch the near-zero detail coefficients into a viewable
image. It round-trips each pixel through `dt_linearRGB_to_XYZ`/`dt_XYZ_to_Lab` (or the
`work_profile` matrix equivalents) and back. Those conversions — like most of the
`dt_aligned_pixel_t`-based color primitives in `colorspaces_inline_conversions.h` — store their
result via 4-wide SIMD (`dt_apply_transposed_color_matrix`'s `dt_store_simd_aligned`), which writes
*all four* lanes even though the color math is only 3-channel; the 4th lane ends up holding
leftover matrix-multiply output, not the caller's original value. For most pipeline buffers that
4th channel is meaningless padding and nobody notices. Here it is retouch's own mask-display
alpha, painted a few lines up the call chain via `rt_copy_mask_to_alpha`/`_cl` — so every pixel's
alpha got silently reset by the *next* operation in the same `process()` call, regardless of
scale-matching or hash correctness upstream. This is why fixes #1 and #2 above were both real bugs
worth fixing but neither actually resolved the reported symptom: content was being computed
correctly and served fresh, then destroyed by `rt_adjust_levels()` before publish. Only triggers
when previewing a wavelet scale (`return_layer > 0`) *and* something reads alpha for display
(`show mask`, or — before fix #2 — a would-be-`PASSTHRU` OpenCL frame that never got the memo).
Fixed by saving `img_src[i+3]` before the round trip and restoring it after. Any other per-pixel
loop in this codebase that round-trips through these color conversion primitives on a buffer whose
4th channel is meaningful (alpha, a mask, anything other than padding) has the same exposure.

## toneequal: the GUI samples a pipeline buffer whose size it does not choose

*Found `6e3d03a02a`, 2026-09-07.*

The luminance mask every GUI reader consumes — the cursor exposure readout, the on-canvas exposure
cursor, the histogram, the colour picker — is the module's own pipeline buffer, published in the
shared pixelpipe cache under `dt_hash(piece->global_hash, "toneequal:luminance")`. Two properties
of that arrangement are not visible from the GUI code, and each one is a way to read the wrong
pixels.

**The mask's dimensions are the ROI the pipe planned, which is NOT the preview size.**
`iop/finalscale.c` enables itself whenever `darkroom/render_size != 1` (render at 1:1, downscale
at the very end) and then requests `roi_in = roi_out / roi_out.scale` — the full sensor resolution
— so every module above it runs full-resolution *in the preview pipe too*. Measured on a
7979x5319 raw with `render_size = 0`: the preview pipe's ROI is 1003x669 while `piece->roi_in` at
tone equalizer is 7979x5319 and the luminance mask is 170 MB. The same module runs at 1003x669 the
moment `finalscale` disables itself (zoom exactly 1:1, or `render_size == 1`) — one zoom step
away. The cursor is therefore stored NORMALIZED (`g->cursor_pos_x/y` in [0, 1[) and resolved
against the dimensions of whichever buffer is attached, through `get_luminance_at_norm()` — the
convention `_sample_picker_luminance_mask()` already uses for the picker's box and point. Storing
preview pixels instead samples the top-left ~12% of the image in full-resolution mode; storing
full-resolution pixels reads far out of bounds in the other.

**A hash says nothing about a buffer's size.** `dt_dev_pixelpipe_cache_get()` ignores its `size`
argument on a hit, and the GUI attach paths resolve the cacheline by hash alone while reading the
dimensions from a separate look at `piece->roi_in`. The worker thread replans that piece between
two such reads, so one plan's hash gets paired with another plan's dimensions — and across the
`finalscale` toggle the two plans differ by a factor of 8 per axis, i.e. a 170 MB read into a
2.7 MB buffer, off the end of the cache arena. So every attach site (`process()`, `gui_focus()`,
the `DT_SIGNAL_HISTORY_RESYNC` callback, the cacheline-ready callback) takes the hash and the
dimensions from ONE call to `_current_preview_luminance_hash()`, and refuses any entry that cannot
hold `width * height` floats (`luminance_entry_fits()`, over `dt_pixel_cache_entry_get_size()`).
That check is what makes the geometry an invariant of `g->thumb_preview_entry` /
`g->thumb_preview_buf_width` / `_height`, so the samplers can trust the triple under the GUI lock
without re-deriving it. Any new GUI reader of a cacheline resolved by hash owes the same check:
the hash identifies the content, never the size.

---

## toneequal: `sanity_check()` is not a predicate — it disables the module from the pipeline thread (OPEN)

> Found 2026-09-29 against `45e189f28b`, while verifying `doc/develop-split.md`. Not fixed.

`sanity_check()` (`iop/toneequal.c:628`) reads like a question — it returns 0 or 1, it is
`static inline __attribute__((always_inline))`, and five of its six call sites use it as a
guard. It is not a question. When the module sits before `flip` in `iop_order`, the body at
:641-652 **writes `self->enabled = 0`, calls `dt_dev_add_history_item(self->dev, self, FALSE,
TRUE)`, and calls `gtk_toggle_button_set_active()`** on `self->gui->off`.

Two of the six call sites are `toneeq_process()` (`:1019` and `:1202`) — the **pixel
processing path**, which runs on the pipeline worker thread. So on a raw whose history puts
tone equalizer above `flip`, the worker thread:

- writes `self->enabled`, which is GUI-thread state the pipeline must never touch (see
  `doc/pipeline-history.md`: the only thread-safe interface between the pipe and a module is
  history, under `dev->history_mutex`);
- commits a history item from the worker, re-entering the history engine from the side that
  is supposed to only *read* a snapshot of it;
- calls GTK from a non-GUI thread. The `self->dev->gui_attached` test at :644 does not help —
  that flag is TRUE in the darkroom, which is exactly when the worker is running. The
  `dt_gui_freeze_begin()`/`_end()` pair around it suppresses *signal emission*, not the
  cross-thread call.

The three GUI call sites (`_switch_cursors`, `mouse_moved`, `scrolled`) are on the right
thread and are what the write was presumably written for. `match_color_to_background()`
(`:2274`) is a cairo drawing helper, also GUI.

**The shape of the fix** is the one this tree has used three times already: the predicate
answers, and the *caller* acts. Split it — a pure `_is_after_flip(self)` that every site may
call from any thread, and a GUI-thread-only `_disable_with_warning(self)` that the three GUI
entry points call when the predicate fails. The pipeline path must do neither: a module in
the wrong pipeline position should render as a no-op for that frame and let the GUI disable
it on the next user interaction, which is what the user sees anyway.

Not attempted here because it needs the `dt_control_log()` toast and the history commit
re-homed together, and because the failure is conditional on a history the default pipeline
order does not produce — it wants a reproduction before a patch.

---
