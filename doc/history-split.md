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
3. Split `history_actions.c` (22 lines of pipeline in 514) and `styles.c` (64 in 1278) with
   the byte-conservation method from `doc/exif-split.md`. Both are small enough that the
   seam should be a handful of functions.
4. Decide `xmp_sidecar.cc` separately: its four `develop/` includes are `blend.h`,
   `iop_order.h`, `masks.h` and `imageio_core.h`, and the reason it exists is that the XMP
   document carries the development. It may simply belong at layer 5 next to
   `history_merge.c`.

## Do not re-derive

`doc/exif-split.md` records the three ways an intra-file call graph lies — function
pointers, function-like macros as edges, and a `:(` in a comment parsing as a parameter
list — and the byte-conservation assertions that make a cut safe. Read it before step 3.
