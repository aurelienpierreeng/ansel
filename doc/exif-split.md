# Splitting `common/exif.cc`

Measured on the 4772-line file at `refactor/exif-split`, by brace-matched function
parsing plus transitive call-graph closure (`tools/include_graph.py` has no notion of
intra-file structure, so this was done ad hoc; the script is in the PR discussion).

## The file is two modules

`exif.cc` handles two unrelated things that meet only in the XMP document:

| half | roots | fns | own lines |
|---|---|---|---|
| EXIF/IPTC tags — pure metadata | 25 | 34 exclusive | 789 |
| XMP sidecar carrying the **development** | 5 | 34 exclusive | 2555 |
| seam (needed by both) | — | 4 | 127 |

The five history-side roots are the whole XMP sidecar API plus one blob writer:

    dt_exif_xmp_read              383   reads history, masks, module order back
    dt_exif_read_blob             351   dt_imageio_dng_write_tiff_header
    dt_exif_xmp_attach_export     253
    dt_exif_xmp_write_with_imgpath 113
    dt_exif_xmp_read_string        69

They reach eleven `dt_ioppr_*` symbols, `dt_develop_blend_params_t` and
`dt_masks_form_group_t` — i.e. `develop/`, layer 5. That is why `exif.cc` cannot move
into `src/metadata` (layer 1) whole, and why the metadata module gate holds at zero
without it.

## Verified, and worth not re-deriving

* `dt_exif_read` — the main EXIF reader — is **NOT** history-tainted. An earlier
  classification said otherwise; it is reachable *from* a history root without being one,
  which is a different thing. Confirmed by BFS from `dt_exif_read` finding no tainted
  callee.
* The seam is only four functions: `dt_exif_xmp_encode`, `dt_exif_xmp_encode_internal`
  (both already public in `exif.h`, so the history half can simply include it),
  `dt_remove_exif_keys` and `add_mask_entry_to_db` (both `static` today — these two are
  the only ones needing a private header).

## The cut

1. `src/metadata/exif.{cc,h}` — the 25 clean roots. Module gate stays at zero.
2. `src/common/xmp_sidecar.{cc,h}` — the 5 history roots and their 34 helpers. Stays in
   `common/` until point 3 gives it a home in `src/history`, where it belongs: it
   serialises the development, not the photograph's own description.
3. `src/common/exif_internal.h` — `dt_remove_exif_keys` and `add_mask_entry_to_db` only.

**Do the cut with a brace-matched byte-range script, and assert afterwards that every one
of the 72 parsed functions appears in exactly one output file and that the two outputs'
line counts sum to the original.** A forward token search across this file has already
destroyed 117 lines once (see `[[scripted-region-replacement]]` in the project notes):
the failure mode is silent, and only `-Werror=unused-but-set-variable` in the Debug build
caught it.
