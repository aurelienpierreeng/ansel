<!-- Provenance: every finding carries the commit it was established against. -->

# Flat field correction (`iop/flatfield.c`)

> **Written on top of `fa3c2765ab`, 2026-11-18.** How the reference is found again: on top of
> `0e599f0898`, 2026-11-23.
>
> What the module divides by, how it finds its files again once they moved, how the gain map is
> built and shared between pipes, and what it assumes about the reference.

[TOC]

## What it corrects

A flat field is a raw of an evenly lit, featureless field — a light table with no film on it, a
diffuser over the lens — shot with the camera, lens and aperture of the images it corrects.
Everything uneven in it was put there by the optics and the light: vignetting, a source brighter
in its centre, a colour drift across the field, dust. Dividing the image by it, normalised,
removes all of that at once.

An optional **dark frame**, a raw shot without light at the same settings, is subtracted from the
reference and from the image before the division. It only makes sense when its exposure matches
both; the module does not check that.

## Where it sits

At `iop_order` 1.5 in every order table, between `rawprepare` and `invert`, with order rules on
both sides (`develop/iop_order.c`). It works on the sensor mosaic, per sensel, before any colour is
mixed: `demosaic`, `highlights`, `temperature` and the raw denoisers all see the flattened mosaic.
It is offered for mosaiced raws only (`dt_image_needs_demosaic()`), like `hotpixels`.

The reference is decoded at full size by `dt_imageio_open_standalone()` and must share the image's
sensor grid: one channel, the same CFA (`filters`, and the X-Trans table), the same rawprepare
border trim (`crop_x`, `crop_y`), and a trimmed size equal to the module's whole input
(`piece->buf_in`). Anything else — a camera with another sensor, a JPEG, a TIFF — is refused with
a window (`Module failed`, whose item is the file and the image) and the image passes through
unchanged. Only the
image's *default* trim is compared: an image whose rawprepare crop was edited by hand no longer
matches.

The camera model is not part of that test: a reference whose grid matches is used whatever shot it.
When its `camera_makermodel` differs from the image's, it is used under a "Flat field reference
from another camera" window, whose item is the path and the reference's model — for the dark frame
as for the flat. Both models must be known: a decoder that leaves `camera_model` empty (rawspeed
fills it, libraw may not) disables the check. Two bodies of one model are not told apart; the serial
number is not compared.

## How the reference is found again

*On top of `0e599f0898`, 2026-11-23.*

A library moves: to another disk, to another computer, from Linux to Windows. Its images are found
again through the library, but a path in a module's params is not, so each file of the module — the
flat, the dark frame — is stored as three things (`dt_iop_flatfield_file_t`, filled by `_set_file()`
when it is chosen):

- its **absolute path**, as chosen;
- its **path relative to the image's folder**, `/`-separated, with `..` for each level up, so that it
  reads the same on both systems. There is none, and the field is empty, when nothing leads from one
  to the other: another drive or another network share on Windows;
- **what it is**: its size, and the SHA-256 of its bytes.

Finding a file from these is the job of `common/file_reference.c`; the module only asks.
`commit_params()` asks where each file is now, seen from the folder of the image the pipe runs on,
and puts that path in the source, so the map is keyed on the file that is actually read. The file is
looked for, in this order:

1. **at its relative path** from the image's folder. That survives moving the whole tree, and it
   comes first: a reference that sits at the same place relative to each image's folder is that
   folder's own, even when the history was pasted from an image of another folder;
2. **at its absolute path**. That survives moving the image alone, and pasting the history onto an
   image elsewhere on the same computer;
3. **by its name**, ignoring the case of ASCII letters: in the image's folder and the three folders
   above it, each with the folders directly in it — no deeper, and no hidden ones — the nearest
   first. A file of its name is taken only if it has the size and the SHA-256 of the file chosen.
   The size, from a stat, is compared first, so a file is read whole only when it could be that one.

A file found at 1 or 2 is used as it is, without looking at what it is: a reference replaced in place
keeps being used, as it always was.

What a search by name finds is kept for the session, keyed on the folder, the name, the size and the
SHA-256, so the pipes of an image, its panel and the thumbnails of a film roll list the folders once.
A file it found and that has gone since is looked for again. A search that found nothing is not tried
again before the next session.

When a file is not found:

- **no file of its name either**: the module is not applied, under a `Module failed` window whose item
  is the path as chosen and the image;
- **files of its name, none of them that file**: in the darkroom, the panel asks. A question window
  lists those files, nearest first, with *Use this file* (Enter, or a double click), *Choose another
  file...* and *Not now*; until it is answered the module is not applied, and the darkroom's own
  pipes open no window of their own. It is asked once per image and file, the flat before the dark
  frame, when the image is loaded or its history changes, and only while the module is on. The
  answer is the only thing that writes the params: finding a file again never touches the history.
  Anywhere else — an export, a thumbnail, `ansel-cli` — no one can answer, so the module is not
  applied, under a `Module failed` window that says to open the image in the darkroom to choose.

The panel names each file where it is now, with its full path in the tooltip, and with "(not found)"
after its name when it is not.

A failure stops being true when its file is replaced — another file chosen, the question answered,
the dark frame removed. `_set_file()` then takes back, with `dt_control_alert_withdraw()`, the line
that file's failures put in a `Module failed` window for this image, whatever the failure was; a
window emptied that way closes. That is why each line names the image as well as the file: a
reference missing for a whole film roll lists the images whose thumbnails it failed on, and choosing
the file again for one of them takes back that one's line only. A file that comes back by itself —
copied back where it was — gets its map at the next resync, but its window stays until it is closed.

**Measured** (2026-11-23, Windows 11, GLib 2.90.1 of MSYS2): GLib's SHA-256 reads 217 to 233 MB/s
from the page cache, 0.10 s for a 23 MB CR2. Two CR2 of one camera, one shot apart, are 22 886 255
and 23 016 125 bytes: the size alone sorts most other shots out before anything is read, but it does
not tell two files apart for sure, and the SHA-256 does.

## How the gain map is built

1. **Normalise like rawprepare.** Each file is read through its own black levels (per 2x2 site of
   the untrimmed sensor) and its own white point, so the reference and the dark frame land in the
   units rawprepare gives the image. The dark, when there is one, is subtracted sensel by sensel.
2. **Split by CFA phase.** One sensel of each phase per period-by-period tile — 4 phases on a 2x2
   Bayer, 36 on a 6x6 X-Trans — is gathered into a regular single-colour plane of its own. Nothing
   is ever averaged across colours or across the two Bayer greens.
3. **Smooth.** Each plane is box-blurred three times (`dt_box_mean()`, a near-gaussian) with the
   `smoothing` radius divided by the period, so the radius is in sensels. At 0 nothing is blurred
   and every sensel is corrected by its own value, dust included, noise of the reference included.
4. **Normalise by the centre.** Each plane is divided by its own mean over the middle eighth of the
   frame on each axis. The centre therefore keeps its values, and with them its white balance.
   A plane whose centre mean is not above 1e-6 makes the reference "too dark" and is refused.
5. **Colour or not.** With `correct color`, each phase keeps its own shape, so a hue drift of the
   light is flattened too. Without it, the shapes of the phases are averaged per tile and every
   phase reads that average: the falloff is corrected, the colour drift is left alone.
6. **Gain = 1 / shape**, per sensel, in the input's own row-major layout; a shape not above zero
   gives a gain of 1. Sensels past the last whole tile, at the right and bottom edges, read the last
   tile.

Step 4 makes the reference's exposure level irrelevant: twice as bright or as dark gives the
same map, and so does any colour of its light, each phase being divided by its own centre. What
does matter is clipping — where the reference saturated it did not record the falloff — so a flat
with more than one sensel in 10000 at its sensor's saturation is used all the same, under a "Flat
field reference clipped" window naming the file. The margin lets hot pixels and specks of light
through. Saturation is told by its shape, not by the declared white point: clipped sensels all read
the same code, a pile at the very top of their 2x2 site, so `_count_clipped()` counts per site from
the site's maximum down to a thousandth of its range, on the raw values, before the dark frame.
The white point would be wrong both ways: rawspeed's can sit below where the sensor saturates, and
a sensor can saturate below it.

`process()` then writes `(in - dark) * (1 + strength * (gain - 1))`. The strength is a linear mix
of the gain, applied per pixel, so it is not part of what the map is a function of.

The box blur's window shrinks at the image border (`blur_horizontal_1ch()` averages what is
inside), so near the edges the smoothed reference leans towards the brighter interior and the
correction there falls a little short. The larger the radius, the wider that band.

## How the map is shared

The map lives in the shared pixelpipe cache, keyed on a hash of `dt_iop_flatfield_source_t`:
both paths — where the files are now, not where they were chosen — whether each was found, each
file's `mtime` and size, the radius, the colour switch, the input size, the trim and the CFA. Every pipe of an image asks for the same key; the first builds the map under the
entry's write lock, the others take the read lock, which waits for it (`_get_map()`), then read
without a lock — once built, a map is never written again. Two images sharing one reference and one
sensor grid share one map. The strength is not in the key, so its slider reuses the map.

The entry is a 64-byte header (status, whether a dark follows), then 4 bytes of gain per sensel,
then 4 bytes of dark per sensel when there is one: about 100 MB on a 24 Mpx sensor, twice that
with a dark frame. Building it takes two more transient buffers of the same order (the decoded raw, at 2
bytes per sensel, and the phase planes, at 4).

A map that could not be built is flagged auto-destroy before its write lock goes, so the pipes
waiting on it read its status and the last of them removes it. Each pipe also remembers the hash it
failed on (`failed_hash`) and passes the image through without decoding again until the source
changes. The window is opened once, by the pipe that tried to build.

The file's `mtime` and size are read in `commit_params()`, where the file is looked for too. A
reference replaced in place, or found again, therefore gets a new map at the next history resync,
not before: until then the module's own output, keyed on its params, is still served from the cache.

## On the GPU

`process_cl()` uses the same map, built on the CPU by `_get_map()`. The map goes to the device
whole — the gain, then the dark below it, as one float image of `height` or `2 * height` rows,
stacked as in host memory — and is attached to the map's cache entry
(`dt_dev_pixelpipe_cache_get_cl_buffer()` with no host pointer, released with `keep`). The kernel,
`flatfield` in `data/kernels/basic.cl`, reads it at the input's absolute position
(`roi_in->x/y`) and the dark at `dark_row` rows below, so a tile or a cropped ROI needs no copy of
its own.

The device image outlives the run only as long as the pipe: `dt_dev_pixelpipe_cleanup()` flushes
the idle device buffers of every cache entry (`dt_dev_pixelpipe_cache_flush_clmem_for_pipe()`), so
each export pipe uploads the map again, while the darkroom pipes, which live as long as the view,
find it on the device. A map taller than the device's largest image fails to allocate and the
pipeline falls back to `process()`; on a GTX 1060 that limit is 32768 rows, which only a dark frame
on a sensor taller than 16384 rows reaches.

## What is not there

- **A reference renamed and moved** is not found: the search is by name, and what the file is only
  decides between the files of that name.
- **Interaction with highlight reconstruction.** Gains above 1 at the edges push sensels that were
  just below the white point above it, and `highlights`, downstream, takes them for clipped. The
  embedded DNG GainMap of rawprepare has the same property.
- **X-Trans is untested**: no X-Trans raw was at hand. The code path is the Bayer one with a period
  of 6.

## Measured

*On top of `fa3c2765ab`, 2026-11-18, GTX 1060, `ansel-cli` exports at 2000 px of a Nikon Z6 NEF
(6064x4040, cropped by its history so the RAW-domain ROI is 5121x3453 at a non-zero offset), with
an XMP made of the image's history plus a flatfield entry, `-d perf -d opencl`. "CPU" is the same
tree without `process_cl()`, where the GPU pipe runs `process()` on the CPU.*

- **GPU against CPU**: the image as its own reference (smoothing 0 with colour on and off,
  smoothing 32, strength 50 %), a missing reference, a NEF from another camera, a dark frame:
  bit-identical in 8-bit. A flat from another Z6 at smoothing 64 differs by 1/255 in 1.2 M pixels,
  and in 24 with dithering off: rounding of the float outputs, spread by the dither.
- **Clipping** (2026-11-19, counted with `rawpy` and by the export; the threshold is 2449 sensels):
  the image as its own reference (ISO 200) piles 1442 to 2931 sensels per site at exactly 16383,
  8854 in the band, and opens the clipped window. A light-table flat at ISO 100 has 370 878 blue
  sensels above the declared white point 15520, in a smooth bell centred near 15350 whose highest
  value, 16310, is reached once: 8 in the band, no window — counting against the white point had
  opened one. A third Z6 flat has 5, no window. The exports are bit-identical to the same exports
  under the white-point count.
- **Another camera** (2026-11-19): a copy of the Z6 NEF whose IFD0 `Model` was rewritten to
  `NIKON Z 6_2` (same crop and CFA in rawspeed's `cameras.xml`), as the image's own reference, opens
  the window with `…, shot with Nikon Z 6_2`, and its export is bit-identical to the untouched NEF's;
  the untouched NEF opens none. A rewritten dark frame under a third Z6's flat opens it too, its
  export bit-identical to the unrewritten dark's and 255/255 away from the export without the module.
- **Refusals**: the missing file and the other camera's NEF are bit-identical to the export without
  the module, each with its message and path on stderr.
- **Dark frame**: the image as its own dark frame, under another Z6's flat, gives black (max 1/255,
  the dither), at strength 100 % and 50 %.
- **Cost**, two Z6 NEFs exported in one run on one reference, twice: the first image 0.598 and
  0.595 s on the GPU, 0.659 and 0.642 s on the CPU, decoding the reference and building the map
  included; the second image, on the same map, 0.021 s on the GPU, 0.035 and 0.034 s on the CPU.
  `-d verbose` shows the device buffer allocated again for the second image: the export pipe's
  cleanup had flushed it, so the upload of the 93 MiB map is in the 0.021 s.
