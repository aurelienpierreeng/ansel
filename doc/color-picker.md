# Module color pickers: where the minimum and maximum come from

*Established on top of `fda814dd0c`, 2026-11-14. Measurements on top of `6a9005ef9c`, 2026-11-13.*

## What a module receives

`_sample_picker_from_cache()` (`gui/color_picker_proxy.c`) samples the module's input and output
cachelines in the preview pipe and hands the module `picked_color`, `picked_color_min` and
`picked_color_max` for its input (and the `picked_output_*` triple for its output).
`dt_color_picker_helper()` blurs the area first (`blur_2D_Bspline`) and then takes the mean,
minimum and maximum **of each channel independently**. A module that reads the minimum or the
maximum therefore gets, in general, three values from three different pixels, and most reduce them
over channels themselves (negadoctor's `v_maxf()` / `v_minf()`). When the module's quantity is
monotone in each channel, its value equals the one it would compute on a single pixel per channel:
the one holding that channel's extreme.

## Locating the extremes

A picker declares which extremes its module reads with `dt_color_picker_set_extremes()`. For such a
picker, and only for the input sample, the helper also scans the same blurred buffer for the first
pixel in raster order holding each channel's minimum and maximum (strict `<` / `>`), so the pixel
found holds exactly the value the module received. It records the pixel's buffer coordinates and
all four of its channels (`dt_color_picker_extremes_t`). The proxy converts the coordinates back to
raw-norm with the inverse of the picker box's own chain: the pixel centre over `roi->scale`, then
`dt_dev_distort_transform_plus()` from the module's `iop_order` (`FORW_INCL` for the input), then
image-abs → image-norm → raw-norm. It stores them in `dev->color_picker` with the picker they
belong to; a new input sample, a reset, a deactivation or the picker widget's destruction clears
them.

- `dt_iop_color_picker_set_deciding_channel()` lets the module name the channel whose extreme it
  kept; the darkroom draws that channel's mark as a +, the others as smaller, dimmed crosses.
- `dt_iop_color_picker_get_extreme_pixel()` returns all the channels of one extreme's pixel, for a
  module that needs the pixel itself rather than one channel of it.
- The marks are drawn only when `ui_last/colorpicker_display_extremes` is set. Locating does not
  depend on it, since a module may read the pixel.

Locating costs one sequential pass over the area per sample; the statistics themselves already
read every pixel of it.

## Why per-channel extremes and not one pixel by luminance

Measured by dumping the blurred module input of negadoctor on 20250921_0007 (preview pipe) and
replaying its D max, bias, paper black and print exposure pickers in Python, against one pixel
chosen by its luminance (the darkest or the brightest one):

- On 300 random boxes the two agree closely: median differences below 0.006 D and 0.003 EV. But in
  the worst boxes 31 % to 46 % of the pixels lie beyond the luminance pixel's value in the channel
  the module keeps, so that pixel no longer bounds the area.
- On a paper black box covering most of the frame, the three channels' maxima sat within 2 px of
  each other: per channel 0.100 / 0.080 / 0.106 (blue decides), 0.098 from the luminance pixel.
- A speck of dust in the sky made D max 1.605 per channel; the luminance pixel gave 0.187 D less.
  Once the dust was retouched (998 px changed), the two differed by 0.014 D. The dust, not the
  method, was the outlier: dust stops light, so it only reaches the pickers reading the densest
  pixel.
- Trimming instead of taking the strict extreme neutralizes the dust for D max (1.425 with it at
  the 99.99th percentile, 1.420 without) but moves the paper black away from the blacks: 0.087 at
  the 0.01 % trim and −0.007 at 0.1 %, against 0.106.

So the strict per-channel extreme stays, and the marks show which pixel each value comes from.

---

# On the image: a click places a point, a drag draws an area

*Established on top of `aad29756a2`, 2026-11-16. Every figure below was measured that day in the
running GUI, on `tests/image_test/samples/Nikon/D700/0004.NEF`; the method is at the end.*

## What a picker declares, and who decides

A picker's button turns it on and off, and that is all it does. Whether the user gets a point or
an area is decided **on the image, by the gesture**. Until this change it was decided on the
button: Ctrl+click or right-click turned a `DT_COLOR_PICKER_POINT_AREA` picker on in area mode,
and a plain click in point mode.

| Kind | A click | A drag | First time it is turned on |
|---|---|---|---|
| `DT_COLOR_PICKER_POINT` | places the point | moves it | a point |
| `DT_COLOR_PICKER_AREA` | drops a square 2 % of the image wide | draws the area | the image, less a 64 px margin |
| `DT_COLOR_PICKER_POINT_AREA` | places a point | draws an area | a point |
| `DT_COLOR_PICKER_AREA_POINT` | places a point | draws an area | the image, less a 64 px margin |

After the first time, a picker reopens on whatever it last showed (`pick_is_box`).

*The area-only row said "1 % of the image wide" until 2026-11-17 (`974a9dc5b6`). The square is
1 % of the image width **either side** of the click (`half_x = 0.01f` in
`dt_iop_color_picker_pointer_pressed()`), so 2 % across; the figure was copied from the half-width
without reading what it was half of. Its size was never measured in the GUI, only that the click
leaves an area (`min != max`).*

**The rule for choosing a kind: a picker whose module reads `picked_color_min` or
`picked_color_max` is `DT_COLOR_PICKER_AREA`.** A point has no extent to take an extreme over; its
minimum, maximum and mean are one pixel. A picker whose module reads only the mean can take
either. The last two kinds differ only in the right-hand column, and that column is why there are
two: exposure and color calibration were `AREA` and now take a point as well, but turning them on
must go on metering the whole frame, as it always has — while the global picker, the curves and
the blending have always opened on a point.

Audited across the 66 creation sites: none of the 8 files declaring only point pickers (13 pickers)
reads an extreme. The `POINT_AREA` pickers that do — tonecurve, rgbcurve, colorzones, relight, the
blending's — read it as the extent of a range they *draw*, which a point degenerates to a line;
that was already reachable before and is by design.

## A press decides nothing

For a picker that takes either, a press cannot tell a click from the start of a drag, so it
samples nothing: the gesture is `DT_COLOR_PICKER_POINTER_PENDING` until the pointer travels more
than 6 widget pixels (an area, with the press as its fixed corner) or the button comes up (a
point, where it went down).

Sampling at the press and correcting afterwards is not equivalent. A module **acts on every
sample it is handed** — exposure and color calibration each write a history item per sample
(`history commit source=auto_set_illuminant`) — so a point sampled at the press and replaced by an
area a moment later is one edit made and one taken back. Measured: with the press deferred, a
Ctrl+click produces exactly one `[picker] sampled` line, with `min == max == avg`, and it comes
after the release.

A press on a corner of the area that is already there takes hold of that corner, before anything
else the press could mean; otherwise an area could be drawn and never adjusted.

One implementation serves every view (`dt_iop_color_picker_pointer_pressed/moved/released()`).
Darkroom and studio capture each carried a copy, with its own anchor — darkroom's lived in
`primary_sample->point`, borrowed from the sample it was editing.

## The picking modifier

While the primary modifier is held (Ctrl; Cmd on macOS), a click in the image samples for **the
one picker there is no doubt about**, without it having been turned on first, and a drag samples
an area for it. Releasing the modifier turns that picker off.

Which picker is decided from what is on screen, in `_gesture_target()`:

- the focused module **shows exactly one** picker of its own → that one;
- it shows **several** → none; the modifier does nothing, and does *not* fall back to the global
  picker (filmic's scene tab: three pickers, measured: no picker event at all);
- it shows **none**, or no module has the focus → the global picker, if its panel shows it —
  unless the module handles clicks on the image itself (`button_pressed` non-NULL), in which case
  the click is its own business.

"Shown" is mapped and sensitive. A picker on another notebook page, in a collapsed module or
section, or in a closed panel is never turned on this way: the user could neither see it come on
nor turn it off. Measured both ways on the global picker — with the Scopes panel open but its
*Color picker* section collapsed, a Ctrl+click does nothing.

Three decisions that are easy to get wrong:

- **The blending's pickers are not the module's.** `develop/blend_gui.c` creates two for every
  module that blends, with that module as their owner. Counted, they leave no module with a
  single picker, exposure and color calibration included, and the modifier with nothing it could
  ever choose. `dt_color_picker_exclude_from_gesture()` takes them out.
- **The modifier going down turns nothing on.** The same key begins half the shortcuts there
  are, and turning a picker on makes its module act on a sample. Only the cursor changes; the
  click does the rest.
- **It is last in line.** `views/darkroom.c:button_pressed()` offers the click to the masks, then
  to the focused module, and only then to the modifier, so it cannot take a Ctrl+click away from
  either. The cursor that promises the pick and the click that makes it read one predicate
  (`_darkroom_modifier_would_pick()`), so they cannot disagree.

A module that claims every click (drawlayer, to paint) never lets the view reach that point, so it
begins the gesture itself with `dt_iop_color_picker_gesture_begin(its_picker, …)`. From the next
event the picker is on and the view gives it the pointer before asking the module.

## The modifier comes up before the sample does

A sample is taken from the pipeline's cache and often has to wait for a recompute. Measured from
button release to `[picker] sampled`:

| Situation | Latency |
|---|---|
| idle pipeline, module input cached | 12 ms |
| idle pipeline, cache miss | 33 – 70 ms |
| a brush stroke begun under 1 ms after the click | 290 ms, 715 ms |

A hand lets go of Ctrl inside that window routinely. Two wrong answers bracket the right one, and
this code has been through both:

- **Turn the picker off at once** and the click is thrown away: the pending sample is cancelled.
  That is what drawlayer's own version did before this.
- **Keep the picker on until the sample lands** and the picker takes the click that follows —
  for the user who picked a colour to paint with, the first dab of the stroke.

So the two things "on" meant are separated. When the modifier comes up
(`dt_iop_color_picker_gesture_end()`), the picker **stops owning the image at once** —
`dt_iop_color_picker_is_visible()` answers FALSE, so the view draws the module's overlay again and
routes clicks to it — but stays **registered** until its sample lands, and is only then reset.
Measured with Ctrl released in the same instant as the button: `sampled` 12 ms later, `gesture
end` 37 µs after the module's apply. With a stroke begun in between: the stroke is whole, and the
pick lands 290 ms later.

The wait is bounded (`DT_COLOR_PICKER_GESTURE_WAIT_MS`, 3 s). A sample is not guaranteed to land
— see the last section for one that never does — and a registered picker keeps its module's cache
bypassed and its button lit. Measured: `gesture gave up waiting for its sample` 3 s after the
modifier came up, and a plain click 0.5 s into the wait was *not* taken by the picker.

**`request_color_pick` is not "the picker owns the image".** It says the module's picker is
registered, which now outlives ownership by the latency above. drawlayer's `mouse_moved()` used it
to abort strokes "while the picker is active", and aborted the stroke begun inside the window on
its first motion: one dab where a line was painted. It asks
`dt_iop_color_picker_is_visible()` now, as toneequal and colorequal already did. Any other canvas
handler tempted to read `request_color_pick` for this should not.

## A key release is not always delivered

The modifier coming up is heard as a key release (`key_released()`, plumbed for this), and it is
not always delivered: the key can come up over another window. A picker left on by that would
take every click on the image. So `mouse_moved()` in both views treats the pointer moving with the
modifier no longer down as the release it stands for, and drawlayer does the same for its own
picking flag.

## How this was measured

Reading the code had been wrong about this gesture twice, so it was driven in the real GUI, on a
display of its own so nothing touched the session it ran from:

```sh
weston --backend=headless --socket=wl-test --width=1700 --height=1000 --no-config &
WAYLAND_DISPLAY=wl-test Xwayland :99 -geometry 1700x1000 -noreset &
DISPLAY=:99 openbox &
env -u WAYLAND_DISPLAY GDK_BACKEND=x11 DISPLAY=:99 \
  build/stage/opt/ansel/bin/ansel --configdir <scratch> --library <scratch>/library.db \
  --disable-opencl -t 2 -d dev -d input <a COPY of a raw>
import -display :99 -window root shot.png      # what is on screen, to find coordinates
```

Input goes in through XTest (`libXtst`'s `XTestFakeMotionEvent`, `XTestFakeButtonEvent`,
`XTestFakeKeyEvent`, reachable from Python with `ctypes`), and `-d dev` prints every picker event
with a timestamp: `gesture begin`, `request update`, `sampled … avg/min/max`, the module's
`apply`, `gesture end`. `min == max` in a `sampled` line is a point; anything else is an area.

Four things that cost time:

- **`-u WAYLAND_DISPLAY` is not optional.** Without it GTK opens on the session's own compositor
  and the test runs on the user's desktop.
- **A fresh profile opens the telemetry consent dialog**, both boxes ticked. Untick them before
  confirming; a test profile has nothing to report.
- **Copy the raw first.** Opening one writes an `.xmp` beside it, and drawlayer a `.ansel.tiff`.
- **Never stop the test instance by name.** `pkill -f <pattern>` matches the shell running it,
  whose command line contains the pattern; a bare process name matches the user's own Ansel.
  Record the PID at launch and stop that.

Cases exercised: Ctrl+click and Ctrl+drag for exposure, color calibration, the global picker and
drawlayer; Ctrl released in the same instant as the button, and in the middle of a drag; a plain
click after, which must not pick; a button-armed picker reopening on what it last showed, taking
a click, a drag and a corner, and surviving Ctrl; filmic's scene tab (nothing) and look tab (the
global picker); an area-only picker under a click (a small box, `min != max`). Studio capture
shares the code and was **not** driven.

## Seen on the way: the scopes drew nothing on a new profile

On a fresh profile the Scopes panel drew nothing and the global picker read `0 0 0`, turned on by
its button or by the modifier alike. Launched with `--conf plugin/darkroom/histogram/op=gamma` the
scope drew and the picker read (137 / 111 / 85 on a cloud). This is what made the bounded wait
above necessary.

> **What this section said about the cause was wrong.** It said "the stage defaults to
> `initialscale`". It does not: `initialscale` is only what the legacy value `demosaic` is
> migrated to. On a new profile the stage was the **empty string**, because the code read
> `plugin/darkroom/histogram/op` while `data/anselconfig.xml.in` declared
> `plugins/darkroom/histogram/op`, and a setting read without being declared is created empty.
> The mismatch was reported correctly; the mechanism was guessed from one line of
> `_set_params()` instead of read back from a profile.

Fixed in the commit after this one, with two more settings of the same module that had the same
mistake. [`preferences.md`](preferences.md), *A setting read without being declared*, has the
account and what it showed about declaring a setting late.
