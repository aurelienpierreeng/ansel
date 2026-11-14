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
