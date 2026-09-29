<!-- Provenance: every finding carries the commit it was established against. -->

# Colour management (`src/colorprofiles`)

> **Verified against `42eca0e8fe` on 2026-09-29.**
>
> The module owns its profile list, its locks and its prepared transforms. These are the traps that cost work, and the two places this file was WRONG until 2026-09-29.
>
> Each finding below is dated with the commit that established it. A finding is only as
> good as its hash: before acting on one older than the code you are changing, re-measure
> it — and re-date it here when you do. Where an earlier version of a claim was **wrong**,
> that is recorded rather than quietly corrected: how a claim was wrong is usually the more
> useful thing to know.

## The module owns its state

*Found `6bc232ea6f`, 2026-08-09.*

The module owns its state: a single `dt_colorspaces_t` is file-static in
`colorprofiles/colorspaces.c`, built by `dt_colorprofiles_init()` and torn down by
`dt_colorprofiles_cleanup()`. Nothing outside `src/colorprofiles/` names the profile list, its
`xprofile_lock` or its cached LCMS transforms, and `darktable_t` does not know profiles exist.
`tools/check_module_boundaries.sh` holds that: external `dt_colorspaces_get_global()` is ratcheted at baseline **0**, and a count that
*falls* must lower the baseline in the same commit.

> **Correction, 2026-09-29.** This used to name a second counter, "external `xprofile_lock`
> acquisitions", also at baseline 0. **There is no `xprofile_lock` in the tree** — the module's
> locks are `_transforms_lock`, `_settings_lock` (both file-static in `colorspaces.c`) and a
> per-entry `lock` in each profile. The gate's regex
> (`tools/check_module_boundaries.sh:108`) therefore matches nothing, inside the module or out:
> it reports 0 because the token is gone, and neither branch of its ratchet can fire. It is a
> dead check presented as a live boundary. The `dt_colorspaces_get_global()` half IS live. `src/colorprofiles/README.md` is the full map.

**The standing regression check is `tools/check_export_pixels.sh <ref-a> <ref-b>`**, which decodes
both exports and compares the pixel arrays. Do NOT sha256 the exported PNG: it carries the build's
version string in its metadata, so two builds differ by a few bytes of compressed text with every
pixel identical. For colour management "it still runs" is a very low bar — the actual failure mode
is a one-LSB hue shift.

`colorprofiles/colorspaces.h` drags `<lcms2.h>` and `<pthread.h>` in behind it. A translation unit
that only needs the vocabulary — a profile type to store in its params, an intent to pass along —
includes `colorprofiles/profile_types.h` instead, which is the reason that header exists.

The module's own map is [`src/colorprofiles/README.md`](../src/colorprofiles/README.md);
read it first. What follows is the hard-won part.

## The `role` argument is load-bearing, never a formality

*Found `07429f0dab`, 2026-08-09.*

`DT_COLORSPACE_SRGB` is registered **twice** — a v4 parametric-curve entry valid only as INPUT,
and a v2 point-TRC entry carrying output/monitor/working — and the two are distinguished by
nothing but which `roles` mask each carries (the five `int *_pos` fields this used to describe were replaced by one `dt_colorspaces_profile_role_t roles`, `colorspaces.h:242`). A multi-bit role mask resolves to the **first** match in
registration order, which for sRGB is the v4 input entry. Resolving the *working* profile with
`DT_PROFILE_ROLE_ANY` therefore hands back the input-only variant; pass
`DT_PROFILE_ROLE_WORKING`. The index-valued calls (`dt_colorspaces_profile_index()`,
`dt_colorspaces_profile_at()`) require a single bit outright — an index means nothing outside the
enumeration that produced it, and an index taken from `INPUT|OUTPUT` equals neither `in_pos` nor
`out_pos`.

**They are roles, not directions**, and the enum was renamed to say so
(`dt_colorspaces_profile_role_t`). A profile is RGB→PCS or PCS→RGB and nothing else; what these
bits select is which *menu* an entry appears in. The menus genuinely differ:
`DT_PROFILE_ROLE_MONITOR` is the curated eligibility list for the monitor-profile menu and
diverges from `DT_PROFILE_ROLE_OUTPUT` on **3** of the 21 built-in registrations by default
(`DISPLAY`, `REC709`, `ITUR_BT1886`) and on 5 only when `allow_lab_output` is on, which it is
not by default — `XYZ` and `LAB` take OUTPUT from that conf key (`colorspaces.c:1906`, `:1910`), so substituting one for the other is a behaviour change,
not a rename. Two further bits, `CATEGORY` and `DISPLAY2`, were declared and never tested by any
lookup — the first had a `category_pos` no predicate consulted, the second had no backing field
at all — so `ANY` claimed six meanings and had four. They are gone; nothing changed at runtime.

**Three registered entries have `profile == NULL`.** `DT_COLORSPACE_WORK`, `_EXPORT` and
`_SOFTPROOF` name a user *setting* rather than a colour space and exist only to occupy a combo
row. Dozens of call sites write `dt_colorspaces_get_profile(...)->profile` with no NULL check;
what keeps them safe is precisely that the lookup never tests `category_pos`, so a category entry
can never be returned. Do not "fix" the predicate to consult it, and do not give categories a
role of their own, without auditing those sites first.

## Lifetime is answered by a lock, not by a copy

*Found `6bc232ea6f`, 2026-08-09.*

There is no `cmsDupProfile` in lcms2. The only true deep copy is serialise-and-reopen — ~0.005 ms
for a built-in but 1.02 ms for a real colord display profile — and copying a prepared
`cmsHTRANSFORM` means rebuilding it, 2.2–38 ms, with nothing to amortise it against. So the
module's four prepared display transforms never leave it (`dt_colorprofiles_xyz_to_display()`,
`dt_colorprofiles_rgba8_to_display_bgra8()`, `dt_colorprofiles_srgb_to_display_strided()` run the
pixel loop inside), and a caller deriving from a profile handle holds
`dt_colorspaces_lock_profile()` / `dt_colorspaces_unlock_profile()` across the derivation.

> **Correction, 2026-09-29.** The plural spellings this file used (`..._lock_profiles()`) do not
> exist and never resolve — only doc comments mention them. The real pair is **singular**
> (`colorspaces.h:369`) and locks **one entry**, not the module: `colorspaces.h:210` says so
> outright, "Per ENTRY, not per module ... so a thumbnail conversion running against sRGB does
> not stand between a monitor change and the display entry." The advice is unchanged; the API
> name and its granularity were both wrong. An
LCMS transform does not retain the profiles it was built from, so the span ends at the
`cmsCreateTransform()` call, not at the transform's lifetime.

That lock is not ceremony. The `DT_COLORSPACE_DISPLAY` entry's `cmsHPROFILE` and the four
transforms derived from it are the only things in the list that mutate after init, and they are
replaced on every window move or resize that lands on a different monitor — i.e. on exactly the
events that repaint. **The display setters take `_transforms_lock` for WRITING** (`colorspaces.c:1324`, `:1347`):
never call `dt_colorprofiles_set_display_profile_choice()` /
`dt_colorprofiles_set_display_intent()` while holding a profile's own read lock — rebuilding the transforms
`cmsDeleteTransform()`s four handles a legitimate reader may still be using.

**Lock order where both are involved: `_transforms_lock` OUTER, `_settings_lock` INNER**
(`colorspaces.c:1324`, `:1347`). Nothing takes them the other way round.

## Read the display / soft-proof settings as one snapshot, and render from the one you hashed

*Found `6bc232ea6f`, 2026-08-09.*

The seven fields (colour mode, display triple, soft-proof pair) cross the module boundary only
together, as `dt_colorprofiles_settings_t` via `dt_colorprofiles_get_settings()`, and are written
only through the setters. Reading them one at a time lets a reader pair a new profile type with
the previous filename, and a 512-byte filename read while `g_strlcpy()` is writing it is a **torn**
string, not merely a stale one.

A pipeline module that folds this state into its cache key must then **render from the same
snapshot**. Snapshotting it in `commit_params()` for the hash and re-reading the live state from
`process()`/`process_cl()` — once per tile — renders from state the cache key does not describe.
`dt_colorprofiles_settings_t.generation` advances on every accepted change and is the cheap thing
to hash: one number that cannot go stale field by field.

Every setter returns "did it actually change", and that return value is the point. A caller that
decides for itself, against a value it read separately, is how re-selecting the **already active**
display profile came to reset the user to the system profile — an inherited "profile not found"
fallback firing on the one case where nothing should happen.

## The derived-profile memo is module-owned; image-derived profiles are not in it

*Found `6bc232ea6f`, 2026-08-09.*

`dt_iop_order_iccprofile_info_t` (two 3×3 matrices plus six eagerly allocated 65536-float LUTs,
~1.5 MB) is a pure function of `(type, filename)`, so `dt_colorspaces_add_profile()` memoises it
process-wide under its own mutex — find-or-create as one critical section, because it is reached
per tile from `process()`/`process_cl()` (`iop/lut3d.c`, `iop/tonecurve.c`) and from the GUI
thread (`iop/colorin.c`). It returns a pointer the **module** owns, valid until
`dt_colorspaces_flush_profile_memo()` (or, for `DT_COLORSPACE_DISPLAY`,
`dt_colorspaces_invalidate_display_profile_memo()`).

It must stay sole-owned: `develop/blend.c` shallow-`memcpy`s the struct, aliasing all six LUT
pointers. Tear one down only through `dt_ioppr_cleanup_profile_info()`, which frees the LUTs as
well as the struct — freeing the struct alone leaks 1.5 MB per failure.

`intent` is **not** part of the memo key, so the first caller to ask for a given
`(type, filename)` fixes the intent every later caller gets.

Profiles derived from ONE IMAGE — `DT_COLORSPACE_EMBEDDED_ICC` through
`DT_COLORSPACE_ALTERNATE_MATRIX`, enum 9..14 — are **not** registered in the profile list and
cannot be resolved by identity: their matrices come from that image's own camera data via
`iop/colorin.c`. They must not be memoised either, since a `(type, "")` key would be shared by
every image of the same camera-matrix kind. The pipe that builds one owns it
(`dt_dev_pixelpipe_t.owned_input_profile_info`, freed with the pipe).

## An embedded ICC profile belongs to its image, not to the application

*Found `6bc232ea6f`, 2026-08-09.*

`dt_colorspaces_t.profiles` (`colorprofiles/colorspaces.h`) is the application-wide profile list.
It is built once by `dt_colorprofiles_init()` and **never appended to at runtime** — registration
order is what enumeration reproduces and what every stored combo index in every preset and conf
key refers to, and the whole CRUDE (metadata) half of the API reads the list lock-free on that
basis. **It must stay that way.**

It did not used to be. `_build_embedded_profile()` (`imageio/imageio_profile.c`), reached from
`dt_colorspaces_get_output_profile()`, appended a container for an image's embedded ICC to that
list at runtime — from export jobs, which run in parallel. Three defects in one function:

- an unsynchronised `g_list_append` against a list every reader walks without a lock
  (`xprofile_lock` does not cover this; it guards the *display* profile, and the readers of
  `profiles` never take it);
- unbounded growth — one entry per exported image, held until shutdown;
- an outright leak whenever the profile was not newly created, because only the `new_profile`
  branch ever registered the container it had already allocated.

An embedded profile is a property of one image, so the image owns it:
`dt_image_t.embedded_profile`, written under the image cache entry's own lock, freed by
`dt_image_cache_deallocate()`, and reused on the next export of the same image rather than
rebuilt. Its container is built with `dt_colorspaces_new_image_profile()` and released with
`dt_colorspaces_free_image_profile()` — that pair exists so an image-owned container never touches
the list. The list is init-only — verified by checking that every `profiles = g_list_append` sits
inside `_colorspaces_build()`.

**Two traps, both paid for once already:**

*Do not "fix" such a race by locking the append alone.* With that many unlocked readers it
relocates the unsynchronised write rather than removing it. Either lock every reader, or — better,
and what was done here — stop mutating the shared structure at runtime and give the data to
whatever actually owns it.

*A `cmsHPROFILE` in a container is not necessarily the container's to close.* Several branches
of `dt_image_find_best_color_profile()` (`imageio/imageio_profile.c`) return a profile **borrowed**
from the application-wide list (`dt_colorspaces_get_profile(...)->profile`) and leave its
`new_profile` out-parameter FALSE; only the branches that build one set it TRUE. Giving the
container to the image and closing its profile on eviction therefore double-freed every borrowed
profile — the list closes it again at shutdown. `dt_colorspaces_color_profile_t.owns_profile`
records which case a container is in, and only owning containers close.

That second one aborted all eight CI runners with `corrupted size vs prev_size in fastbins`
after passing four build configurations and every static gate. Nothing static can see it:
`tools/check_it_runs.sh` runs the binary once, which does.

The same "profile-creating function used as a predicate" shape is what leaked three profiles per
resolve in `imageio/imageio_profile.c`: calling a function that *builds* a profile to ask whether
one exists, then calling it again for the value, discards the first. And a `goto finish` that
skips every write to an out-parameter — including the sRGB fallback — returns an **uninitialised**
`cmsHPROFILE` straight into `cmsCreateTransform()`. Both live in the profile-for-an-image cascade;
check the early-out paths there before adding another branch to it.

---
