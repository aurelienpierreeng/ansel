# Splitting `common/exif.cc`

Done. `common/exif.cc` (4775 lines) is now `metadata/exif.cc` (2225) and
`common/xmp_sidecar.cc` (2683), with `metadata/exif_internal.h` holding what genuinely
spans both.

Measured by brace-matched function parsing plus transitive reference closure
(`tools/include_graph.py` has no notion of intra-file structure, so this was done ad hoc;
the scripts are in the PR discussion).

## The file was two modules

`exif.cc` handled two unrelated things that meet only in the XMP document:

| half | fns | own lines |
|---|---|---|
| EXIF/IPTC tags — what the photograph says about itself | 27 | 1673 |
| XMP sidecar carrying the **development** | 36 | 2442 |
| seam — wanted by both | 8 | 429 |
| unreachable from either | 2 | 37 |

The five history-side roots are the whole XMP sidecar API plus one blob writer:

    dt_exif_xmp_read              reads history, masks, module order back
    dt_exif_read_blob             dt_imageio_dng_write_tiff_header
    dt_exif_xmp_attach_export
    dt_exif_xmp_write_with_imgpath
    dt_exif_xmp_read_string

They reach eleven `dt_ioppr_*` symbols, `dt_develop_blend_params_t`,
`dt_masks_form_group_t` and `dt_imageio_dng_write_tiff_header` — i.e. `develop/` (layer 5)
and `imageio/` (layer 6). The tag half reaches **nothing above layer 1**, which is what
made the cut worth making: `metadata/exif.cc` sits in the module and the gate still reads
zero.

## Three ways the call graph lied, each caught by measurement

Worth not re-deriving, because each one silently moved functions into the wrong half:

* **Function pointers.** A `name\s*\(` regex misses every `GHFunc`, `GDestroyNotify` and
  log handler. Twelve helpers looked unreachable — `_xmp_append_history`,
  `free_mask_entry`, `dt_exif_log_handler` and friends. Match bare identifiers instead.
* **Function-like macros are edges.** `FIND_EXIF_TAG` is used 78 times in the tag half and
  expands to `_exif_read_exif_tag`; without expanding it, that function files under
  whichever half happens to name it directly (the sidecar, twice). All 7 function-like
  macros in the file were checked; 4 reach a function.
* **`:(` in a comment parses as a parameter list.** Take the name from comment-masked text,
  or `read_history_v1` comes out anonymous.

Two functions are reachable from no root at all and are simply dead:
`print_history_entry` (already `__attribute__((unused))`) and `_get_max_multi_priority`.
Both went with the sidecar; neither is called.

Also verified: `dt_exif_read` is **NOT** history-tainted. An earlier classification said
otherwise; it is reachable *from* a history root without being one, which is a different
thing.

## What the cut actually produced

1. `src/metadata/exif.h` / `.cc` — the tag half plus the seam. 15 public functions.
2. `src/common/xmp_sidecar.h` / `.cc` — the 5 sidecar roots and their helpers.
3. `src/metadata/exif_internal.h` — `class Lock`, `read_metadata_threadsafe`, and three
   functions: `dt_remove_exif_keys`, `dt_exif_read_exif_tag`, `dt_exif_decode_xmp_data`.
   Private to those two `.cc` files by convention; a third includer is the signal to
   promote something into `exif.h` deliberately instead.

`#include "darktable.h"` is gone from both halves. Its only real use was
`darktable.exiv2_threadsafe` inside `class Lock`, which now goes through the existing
`dt_exiv2_threadsafe_mutex()` accessor in `common/global_mutexes.h`; every other
`darktable.` in the file is an XMP key string (`Xmp.darktable.history`).

Five deliberate edits beyond pure code motion, and nothing else:

* `_exif_decode_xmp_data` → `dt_exif_decode_xmp_data`, `_exif_read_exif_tag` →
  `dt_exif_read_exif_tag`. They stop being `static`, and a leading underscore at global
  scope is reserved.
* those two and `dt_remove_exif_keys` lose `static`.
* `class Lock` takes the mutex through the accessor.
* `_exif_get_exiv2_tag_type` reads the tag list through `dt_exif_get_exiv2_taglist()`
  rather than the `exiv2_taglist` file-static it can no longer see. The accessor builds
  the list if it is empty, so this is if anything more robust; `dt_init()` builds it long
  before any sidecar work, so in practice the two are identical.

## How the cut was made safe

A forward token search across this file has already destroyed 117 lines once (see
`[[scripted-region-replacement]]` in the project notes): the failure mode is silent, and
only `-Werror=unused-but-set-variable` in the Debug build caught it. So the cut was done by
assigning **every byte** of the original to exactly one destination and asserting, before
writing anything:

* consecutive segments abut exactly, and the last one ends at `len(text)` — no gap, no
  overlap;
* all 73 parsed functions are placed, and none is placed twice;
* the parts' line counts sum to the original's 4775.

One trap the assertions surfaced: a segment boundary falls where the previous statement
ended, which is **mid-line** whenever that line carries a trailing comment
(`static const guint dt_xmp_keys_n = ...; // the number of keys`). The comment then lands
in the other file. Every boundary is snapped forward to the next line start, and any
boundary with real code after it is reported rather than moved.

## Verification

* Four configurations build clean: Release, Debug (`-Werror`), nofeatures, and the
  unit-test build. `ctest` 7/7.
* `metadata: 0 includes from a higher layer` — the module gate holds.
* Layering violations 198 → 196 (`develop/imageop.h` and `darktable.h` both dropped).
* Export A/B against the pre-split build on `_DSC9410.NEF` with `--export_masks 1` — a
  57-node brush, so the XMP history *and* `masks_history` read paths both run, and the mask
  is written out as a second TIFF page. **Zero differing pixels**, on both pages
  (6016x4016x3, uint8), max abs diff 0.

  **Compare decoded pixel arrays, not file bytes.** `cmp -l` on the two TIFFs reports
  ~19.2 million differing bytes of 19.4 million — because the exported file embeds the
  build's version string and two timestamps, and when those change *length* every byte
  after them shifts. Two runs of the *same binary* differ that way; the files came out
  19396008, 19396010 and 19396012 bytes across three runs. A byte comparison here answers
  a different question than the one being asked, and answers it wrongly. This is the trap
  `CLAUDE.md` records for PNG exports, and it applies to TIFF for the same reason:

      ok, pages = cv2.imreadmulti(path, flags=cv2.IMREAD_UNCHANGED)   # both pages
      d = np.abs(a.astype(np.int64) - b.astype(np.int64))             # per page

## Left for later

The sidecar belongs in `src/history`: it serialises the development, not the photograph.
It stays in `common/` only until that module exists. Its five entry points keep their
`dt_exif_*` names so this cut stays reviewable; renaming them is that move's business.

## The scanners' findings on the moved code

Moving 2687 lines into a new file makes every line of it "new" to SonarCloud, so the split
drew nine code-scanning alerts on `common/xmp_sidecar.cc`. They are not all the same thing,
and the triage is the useful part:

* **One real defect.** `dt_exif_xmp_read()` computed `filename + strlen(filename) - 4`
  before testing the length, then guarded with `c >= filename`. Forming a pointer before
  the start of an array is undefined behaviour — only one-past-the-end is legal — so the
  guard tests a pointer that is already invalid. It happens to work on every real target,
  and the guard proves whoever wrote it knew the short-name case existed. The length is
  checked first now.
* **One misplaced NULL check**, in `_exif_get_exiv2_tag_type()`: `if(t)` sat *after* two
  dereferences of `t`. Not a crash, because `g_str_has_prefix()` NULL-checks internally and
  short-circuits — but it reads as a guard and is not one. Checked before use now, and the
  duplicated `strlen(tagname)` hoisted.
* **Four worth taking.** `strlen(s.c_str())` on a `std::string` rescans what the string
  already knows (`.size()`); `f(x->child_value(), strlen(x->child_value()))` calls the
  accessor twice; `g_strstr_len(s, strlen(s), n)` is `strstr(s, n)`.
* **Two false positives on bounds**, kept as they were: `t[tagname_len]` is in bounds
  precisely *because* the prefix comparison succeeded. That is now stated in a comment, so
  the next reader — human or scanner — does not have to re-derive it.
* **One false positive outright**: "IP addresses should not be hardcoded" on
  `xmpData["Xmp.exif.GPSVersionID"] = "2.2.0.0"`. That is the EXIF GPS tag version from the
  spec, not an address. Unchanged.

Re-verified the same way, on decoded pixel arrays: zero differing pixels against the
pre-split reference, both pages, max abs diff 0.
