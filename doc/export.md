<!-- Provenance: every finding carries the commit it was established against. -->

# Export

> **Verified against `42eca0e8fe` on 2026-09-29.**
>
> How an export request's size is resolved, and where that decision must happen.
>
> Each finding below is dated with the commit that established it. A finding is only as
> good as its hash: before acting on one older than the code you are changing, re-measure
> it — and re-date it here when you do. Where an earlier version of a claim was **wrong**,
> that is recorded rather than quietly corrected: how a claim was wrong is usually the more
> useful thing to know.

## The export size is resolved once, by whoever dispatches the export

*Found `6a99540222`, 2026-09-26.*

`libs/export.c` offers five ways to size an export -- a pixel box, a print size in cm or inch at a
dpi, a scale factor, the original resolution -- and each keeps settings of its own: `width` /
`height`, `print_width` / `print_height` (always stored in cm, shown in the unit selected) with
`print_dpi`, `resizing_factor`; `dimensions_type` picks one. Switching modes changes nothing but
`dimensions_type`, so no mode can erase or rewrite another's size, and what a mode shows is what it
exports.

`_resolve_export_size()` turns the selected mode into one size when the export is dispatched, and
that size travels by value: `dt_control_export()` -> the job -> `store()` -> `dt_imageio_export()`
-> `dt_imageio_export_with_flags()`, as `max_width` x `max_height` or a `scale_factor` (> 0 reduces
by it, capped at 1; otherwise the image fits the box, 0 x 0 meaning full size). **Nothing below the
export module reads its configuration**: a job runs on a worker thread, possibly long after it was
queued, so a size read there would be whatever the settings say by then, not what the user
exported with. For the same reason every other caller -- `ansel-cli`, the gallery's thumbnails, the
HDR merge, print, the mipmap and drawlayer exports -- states its own size and passes `0.0`.

The two `print_*` keys are deliberately not in the confgen: `dt_conf_key_exists()` is true for any
confgen key, and their absence is what lets `_ensure_print_size_conf()` derive them once from the
pixel box of an older configuration.

The preset blob holds a pixel box only: `get_params()` stores the size the selected mode resolves
to, or the pixel box in scale mode, which a blob cannot express.
