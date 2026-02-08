# Picture resizing and scaling

This document describes how ROI (Region of Interest) sizes are computed in the pixelpipe, what assumptions are made, and which modules affect the ROI.

**ROI model**
- `dt_iop_roi_t` fields are `x`, `y`, `width`, `height`, `scale` (see `src/develop/imageop.h`).
- `x,y` are the top-left pixel of the ROI in the current buffer, `width/height` are in pixels.
- `scale` is the global factor between the full-resolution input and the current ROI. `1.0` means 1:1.
- ROI values are integers; rounding occurs whenever a float scale is applied.

**Two ROI passes**
- Forward (input → output): `dt_dev_pixelpipe_get_roi_out()` walks the modules in order and calls each `modify_roi_out()`. It starts from the full input `(0,0,width_in,height_in,1.0)` and computes the full-image processed size. It also fills `piece->buf_in/buf_out` for modules. This pass is only used to compute `pipe->processed_width/height` for GUI sizing and export sizing.
- Backward (output → input): `dt_dev_pixelpipe_get_roi_in()` walks the modules in reverse and calls each `modify_roi_in()`. It starts from the requested output ROI and computes the minimal upstream ROI needed for that request. It stores `piece->planned_roi_out/in`, which are then used for cache hashing and for actual processing in `dt_dev_pixelpipe_process_rec()`.

If a module is disabled, or temporarily bypassed while editing (distortion bypass), both passes treat it as a no-op.

**Where the requested ROI comes from**
- Darkroom main preview (`DT_DEV_PIXELPIPE_FULL`): `_update_darkroom_roi()` first calls `dt_dev_pixelpipe_get_roi_out()` to compute full-image processed size. It then computes `scale = natural_scale * dev->roi.scaling`, where `natural_scale` fits the full image into the viewport and `dev->roi.scaling` is the zoom factor. The requested ROI is the visible window: `width/height = min(processed * scale, viewport)` and `x,y` are computed from the normalized ROI center `dev->roi.x/y`. Only that ROI is processed; the base buffer is cropped to it before any module runs.
- Darkroom preview pipe (`DT_DEV_PIXELPIPE_PREVIEW`): the same processed size and `natural_scale` are used, but `dev->roi.scaling` is not applied and `x=y=0`. The preview always renders the full image at a scale that fits the viewport.
- Export and thumbnail: `dt_dev_pixelpipe_get_roi_out()` computes the full-image processed size after distortions/crops/borders. `_get_export_size()` then computes the target output size and `scale = target/full`. The requested ROI is `{0,0,target_w,target_h,scale}`. For export, `finalscale` forces upstream processing at scale 1.0 and resamples at the end. For thumbnails, `initialscale` does the early resampling and `finalscale` is disabled.

**Modules that change ROI**
- `iop/rawprepare.c`: trims sensor margins. `modify_roi_out()` shrinks width/height and resets `x,y` to 0. `modify_roi_in()` expands the request to include the cropped margins.
- `iop/demosaic.c`: no scaling. `modify_roi_out()` snaps `x,y` to the mosaic origin (currently `0,0`). `modify_roi_in()` aligns `x,y` to the Bayer/X-Trans grid unless passthrough.
- `iop/initialscale.c`: early resampling for non-export pipes. `modify_roi_in()` requests the full input buffer (`x=y=0`, `width/height=buf_in`, `scale=1.0`) so the module can resample down to the requested ROI. It does not change the forward size.
- `iop/lens.cc`: geometry correction padding. `modify_roi_in()` expands the ROI based on lensfun distortion and interpolation width; `modify_roi_out()` is a passthrough.
- `iop/crop.c`: rectangular crop. `modify_roi_out()` computes the cropped rectangle in the input. `modify_roi_in()` offsets `x,y` by the crop origin; width/height are unchanged.
- `iop/clipping.c`: rotation/keystone + crop. `modify_roi_out()` computes the output bounding box and crop offsets. `modify_roi_in()` back-transforms the output AABB into input space and adds 1 px padding, with a fast path for pure cropping.
- `iop/ashift.c`: perspective/homography. `modify_roi_out()` computes the output bounds, then applies the internal crop factors. `modify_roi_in()` back-transforms output corners to input space and pads by interpolation width.
- `iop/borders.c`: frame/border. `modify_roi_out()` expands width/height to include borders. `modify_roi_in()` subtracts borders so only image pixels are requested from upstream.
- `iop/finalscale.c`: export and GUI upscaling. `modify_roi_in()` maps `roi_out` back to scale 1.0 when exporting or when `roi_out.scale > 1`, so upstream modules stay at 1:1 and finalscale does the resampling. It does not change the forward size.

**Assumptions and pitfalls**
- Cache hashes include `planned_roi_in/out`, so any rounding mismatch between forward/backward ROI passes causes cache misses or incorrect reuse. Modules generally clamp and add interpolation padding to mitigate this.
- ROI is integer-based but `scale` is float. All resampling is handled by `initialscale` and `finalscale`; the base buffer path only supports `scale == 1.0` for the raw/full input copy.
- During GUI editing, distortion modules can be bypassed to give an undistorted view for the active module. This affects ROI planning and hashes by design.
