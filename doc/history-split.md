# Splitting history out of `develop/` — point 3, fork (b)

Measured on `refactor/history-presentation`, after that branch removed the *accidental*
upward edges (`control/`, `views/`, `libs/`, `widgets/`). What is left is `develop/` only,
and this is the plan for it.

## Why this is not the `src/metadata` shape

A history item holds a `dt_iop_module_t *` and a params blob typed by module. That is not
an accident of layering that an inversion can remove; it is what a history item *is*. So
`src/history` cannot be a layer-1 module the way `src/metadata` is — unless each file is
cut the way `common/exif.cc` was, into the half that serialises and the half that drives
the pipeline.

## Measurement

Lines naming any `dt_dev*`/`dt_iop*`/`dt_ioppr*`/`dt_masks*`/`dt_develop_blend*` symbol:

| file | lines | naming develop/ | verdict |
|---|---|---|---|
| `common/history_snapshot.c` | 124 | **0** | moves whole |
| `common/presets.c` | 244 | **0** | moves whole |
| `common/history.c` | 192 | 3 | one symbol; invert, then moves whole |
| `common/history_actions.c` | 514 | 22 | split |
| `common/styles.c` | 1278 | 64 | split |
| `develop/history_merge.c` | 1964 | 85 | **stays** — it *is* the pipeline merger |
| `common/xmp_sidecar.cc` | 2687 | 4 includes | split, or stays at layer 5 |

Three of the six are already at layer 1 or one symbol away from it. That was not obvious
before measuring: the two files with the loudest include lists (`history_actions.c`,
`styles.c`) are not the ones with the most pipeline in them, and `history_merge.c` — which
looks like the biggest problem — is the one file that should not move at all.

## The one symbol standing between `history.c` and layer 1

Three calls to `dt_iop_get_localized_name(operation)`, all turning an operation string into
a display name. That function (`develop/imageop.c`) lazily builds a `GHashTable` from
`darktable.iop`, the loaded module list — layer-5 *data*, but the question history is
asking is "what is this operation called?", which is vocabulary, not pipeline state.

Inverted as a resolver, the same shape as `dt_presets_set_autoapply_resolver()`. With none
installed the answer is the raw operation string, which is a legible degradation rather
than a wrong one; in practice `dt_init()` installs it before anything reads history, and
`ansel-cli` loads the IOP list too.

## Order of work

1. The `dt_iop_get_localized_name` resolver. Unblocks `history.c`.
2. Create `src/history` and move the three whole files (`history_snapshot`, `presets`,
   `history`) plus `history_notify`. Register `history` at layer 1 in
   `tools/include_graph.py` — **before** measuring anything with `--what-if`, or every
   edge from the moved files is silently uncounted and the report flatters.
3. ~~Split `history_actions.c` and `styles.c`.~~ **Measured per function, and the
   whole-file counts were misleading. See below.**
4. Decide `xmp_sidecar.cc` separately: its four `develop/` includes are `blend.h`,
   `iop_order.h`, `masks.h` and `imageio_core.h`, and the reason it exists is that the XMP
   document carries the development. It may simply belong at layer 5 next to
   `history_merge.c`.

## What per-function measurement says, against the per-file counts

`tools/`-style line counting says `history_actions.c` is 22/514 and `styles.c` 64/1278 —
both look like a small pipeline core in a large clean file. Per function it is the reverse.

**`history_actions.c` must NOT be split.** Its "clean" functions are 6-to-7-line public
wrappers (`dt_history_paste_on_image`, `dt_history_compress_on_list`,
`dt_history_delete_on_list`, ...) that dispatch through `_history_action_on_list()` to an
`_apply` callback — and the callbacks are exactly the develop-touching ones
(`_history_copy_and_paste_on_image_merge` 8 refs, `_history_compress_apply` 6,
`_get_user_mod_list` 5). Splitting separates every public entry point from the operation it
performs, leaving two halves that call each other constantly. 211 clean lines against 261,
but the clean ones are the door and the others are the room. This file is one module —
"apply a history operation to a list of images" — and it drives the pipeline by definition.
It belongs at layer 5, beside `history_merge.c`.

**`styles.c` genuinely is two things**, 552 lines against 650:

* *applying* a style — `_dt_styles_apply_item_to_module`, `_styles_rebuild_history_from_items`
  (10 develop refs), `_styles_sync_pipeline_from_items`, `dt_styles_apply_to_image_merge`,
  and the `_styles_*_source_dev` helpers. Pipeline work, layer 5.
* the style *document* — create, delete, list, save to XML, import from XML,
  `dt_styles_get_item_list*`, the four SAX handlers. Layer 1, except for three functions.

**But those three functions are the point.** `dt_styles_save_to_file` needs
`dt_ioppr_serialize_text_iop_order_list`, `dt_styles_style_text_handler` needs
`dt_ioppr_deserialize_text_iop_order_list` and `dt_ioppr_insert_missing_modules`, and
`_prepend_item` needs the operation-name lookup (already inverted, see above). What a style
document *is* includes the module order, so serialising one requires the codec for a
`develop/` type.

That is the same wall `common/xmp_sidecar.cc` hits, and it is the real finding of fork (b):
**the "serialisation half" of styles and of the XMP sidecar is not layer-1 material, because
what it serialises IS the development.** Splitting those files buys a layer-1 fragment that
still cannot save or load without reaching up.

So fork (b) terminates here rather than continuing file by file. `src/history` at layer 1 is
what does not name a `develop/` type at all: history records as DB rows, presets, snapshots,
notifications, vocabulary. That module exists and is closed. Everything that applies or
serialises a development is layer-5 work and should sit together — `history_actions.c`,
`styles.c`, `xmp_sidecar.cc`, `history_merge.c` — sealed by API if that is wanted, which is
fork (a) applied to a smaller and better-defined set than the original six.

The remaining choice is whether to serialise through injected codecs (a resolver for the
iop-order text format, as `dt_history_operation_name` already does for module names), which
would let the document half descend to layer 1. That is a design decision about who owns the
XMP/XML format, not a mechanical split, and it is worth taking deliberately.

## Do not re-derive

`doc/exif-split.md` records the three ways an intra-file call graph lies — function
pointers, function-like macros as edges, and a `:(` in a comment parsing as a parameter
list — and the byte-conservation assertions that make a cut safe. Read it before step 3.
