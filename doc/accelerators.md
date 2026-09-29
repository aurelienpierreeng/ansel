<!-- Provenance: every finding carries the commit it was established against. -->

# Keyboard shortcuts and accelerators

> **Verified against `42eca0e8fe` on 2026-09-29.**
>
> How a keystroke reaches an action in Ansel, and the four ways that chain has broken.
>
> Each finding below is dated with the commit that established it. A finding is only as
> good as its hash: before acting on one older than the code you are changing, re-measure
> it — and re-date it here when you do. Where an earlier version of a claim was **wrong**,
> that is recorded rather than quietly corrected: how a claim was wrong is usually the more
> useful thing to know.

## Widget shortcuts need their own closure — GTK's native accel-group activation is unreachable

*Found `50f077b360`, 2026-07-31.*

`src/widgets/accelerators.c` offers two ways to register a shortcut: a "generic" one
(`dt_accels_new_action_shortcut`, `dt_accels_new_virtual_shortcut`/`_instance`) that builds a
`GClosure` via `dt_shortcut_set_closure()`, and a "widget" one (`dt_accels_new_widget_shortcut`)
that instead calls `gtk_widget_add_accelerator(widget, signal, accel_group, key, mods, flags)`,
relying on GTK's own `gtk_window_activate_key()` to fire `widget`'s signal when the key is
pressed — which only works if `accel_group` is attached to a `GtkWindow` via
`gtk_window_add_accel_group()`.

That attachment was intentionally removed on 2025-04-02 (`2e693e6b3`, "Accels: do not use Gtk
window connection for accel groups... avoids crashes... Fix #484"): the app now handles every
keystroke itself through `dt_accels_dispatch()` → `_key_pressed()` → `_call_shortcut_cclosure()`,
which looks up `dt_shortcut_get_closure(shortcut)` and does nothing if it's `NULL` — it never
falls back to GTK's native accel-group activation. An attempt two days later to restore just the
global accel-group attachment (`c8770a367`, 2025-04-04 20:57:31, "Still connect global accels to
window") was reverted **two minutes** later (`7273a1371`, 20:59:31, commit message "Nope").
The original removal is `2e693e6b3`, 2025-04-02 — the "same day" belongs to the
attempt-and-revert pair, not to the pair relative to the removal. Re-attaching accel groups to the window is a
dead end that was already tried and abandoned; it is not the way back in.

`dt_accels_new_widget_shortcut()` was never updated for the migration: it still leaves
`shortcut->closure = NULL`, so any shortcut registered only through it is keyboard-dead — clicking
the widget still works (plain `"clicked"`/`"toggled"` GTK signal), but the accelerator silently
does nothing, with no error anywhere. Confirmed dead in practice for the only two default-keybound
consumers of this path in the whole codebase (`dt_accels_new_widget_shortcut` has four call
sites, all in that one file; the other two pass no default key): `src/libs/tools/filter.c`'s "Reload current
collection" (Ctrl+R) and "Toggle culling mode" (Ctrl+S).

Fixed by giving widget shortcuts a real closure too (`_widget_shortcut_callback()`, wired via
`dt_shortcut_set_closure()` inside `dt_accels_new_widget_shortcut()`), which just does
`g_signal_emit_by_name(shortcut->widget, shortcut->signal)` — the same activation path every other
shortcut type already uses. Any future direct caller of `gtk_widget_add_accelerator()` for a
keyboard shortcut in this codebase has the same problem: it needs a closure the internal
dispatcher can invoke, not just a GTK-level accelerator that no window will ever activate.

## An accel path absent from the user's config is not a shortcut the user cleared

*Found `81cae9d54a`, 2026-09-05.*

`_insert_accel()` (`src/widgets/accelerators.c`) used to seed every accel path with
`gtk_accel_map_add_entry(path, 0, 0)` and let `gtk_accel_map_load()` fill in the keys. That
reads the user's config correctly and loses the app's own defaults: the load runs *before* any
widget registers (`gui/application.c`), so a path the config has never heard of comes back with
key 0, and `_update_shortcut_state()` — comparing that 0 against a non-zero app default with
`accels->init` FALSE, i.e. "a config file exists" — files it under "the user changed this" and
records the shortcut as permanently unbound. **Every newly added default shortcut was born dead
for anyone with an existing `keyboardrc`**, silently, with the menu simply showing no
accelerator.

Accel pathes are built from **translated** GUI labels — that is why the config file is
localized, one per language — so the same thing happens when a translation lands or changes for
a label that already had a shortcut. F5 stopped applying the purple colour label in French when
`po/fr.po` gained "⬤ Violet" for a menu entry that had been falling back to the English "⬤
Purple" (commit `b7dcf716`): the path moved, the F5 saved under the old one was orphaned, and
the new one was read as user-cleared. Every other colour kept its key because its translation
had not moved. The orphaned line stays in the file — GTK has no API to delete a map entry — but
it is inert: `_find_path_for_keys()` scans `accels->acceleratables`, not the accel map, and
`gtk_accel_map_change_entry(..., replace=FALSE)` only reports a conflict against entries an
accel group actually uses, so it does not block rebinding those keys either (measured, not
assumed).

Fixed by registering the app default **as the accel-map entry's own default**,
`gtk_accel_map_add_entry(shortcut->path, shortcut->key, shortcut->mods)`. `add_entry` only sets
the current value when it *creates* the entry, so anything the config supplied still wins; and
it makes GTK's changed/unchanged bookkeeping mean what this code needs on the way out, since
`gtk_accel_map_save()` comments out an entry sitting at its default and writes every other one
as a live line. A shortcut the user cleared is therefore saved as a real `(path "")` line and
read back as a *known* path with key 0 — still cleared, this time because the file says so
rather than because the file is silent. `tests/unittests/test_accel_map_defaults.c` pins
all four cases, including the old `(0, 0)` spelling's inability to tell the two apart.

One migration cost, paid once: a config written by an older build recorded a cleared default as
a *commented* line, which reads back as "unknown path", so such a shortcut returns to its
default on the first run after this change. It is then saved under the new spelling and stays
cleared from there on. There is no marker in the file to distinguish the two eras, so this is
not avoidable — only bounded.

**Do not verify this class of change by reading GTK's source or reasoning about it.** Every
question here (does the parser register an empty entry? does `add_entry` overwrite a loaded
value? how is a cleared entry saved?) is answered in seconds by a ten-line program calling
`gtk_accel_map_load`/`add_entry`/`lookup_entry`/`save` and printing the file — and two
plausible readings of that API were wrong when measured.

## A weak pointer must be removed before the struct holding it is freed

*Found `8d2ea7719d`, 2026-09-05.*

`dt_shortcut_set_closure()` (`src/widgets/accelerators.c`) registers `&pc->widget` — the third
member of a 24-byte `PayloadClosure` — with `g_object_add_weak_pointer()`, so that a widget
destroyed while its closure is still listed reads back as NULL rather than as a dangling pointer.
That registration is a live write permission GObject holds on those eight bytes, and it must be
withdrawn before the bytes go back to the allocator.

The struct has **two** teardown paths and only one honoured that. `_g_list_closure_unref()`, the
`GDestroyNotify` handed to `g_list_free_full()`, drops the weak pointer first and carries the
comment saying why. `dt_shortcut_remove_closure()` open-coded the same teardown a hundred lines
below and left that one line out. Any future third path has the same obligation: call the
destructor, do not re-spell it.

The two ends of the bug meet inside a single function, seventy lines apart:
`dt_iop_gui_cleanup_module()` (`develop/imageop_gui.c:1090`) removes the module's accels at
:1110 — freeing the payload — and destroys the widget tree at :1162/:1171/:1179, at which point
GObject fires
`g_nullify_pointer()` and writes NULL sixteen bytes into a freed twenty-four-byte block. That
lands on glibc's chunk metadata, and the process dies at the next `malloc()` large enough to
trigger `malloc_consolidate()` — in a different, innocent caller every run
(`dt_preset_repository_list_for_upgrade`, `dt_image_from_stmt` and `dt_image_repository_load`
were all observed). It fires from `_init_module_so()`'s startup probe loop, which builds and
tears down every module's GUI once to register accelerators, so it presented as a plain
startup crash: the packaged build aborted four times in a row on the same library.

**AddressSanitizer cannot see this class of bug, and its silence means nothing here.** ASAN only
instruments code compiled with it; the faulting store executes inside libglib's
`g_nullify_pointer()`. A full ASAN startup reports zero errors while glibc aborts reliably —
ASAN also replaces the allocator outright, so the metadata checks that *were* catching it no
longer run. `valgrind --tool=memcheck` instruments the system libraries too and named the
allocation, the free and the write in a single pass; it was the only invalid write in the whole
startup. Reach for memcheck, not ASAN, whenever a corruption's likely writer is inside GTK,
GObject, GLib or sqlite3.

**`malloc_trim()` probes do NOT localise a heap overflow.** Breaking on a per-module function and
calling `malloc_trim(0)` looks like a clean bisect and is not one: `malloc_consolidate()` walks
**free** chunks only, so it fires when the *victim* is freed, not when the overflow happens.
Successive runs of the identical script blamed `atrous`, then `bilateral`, then `colorbalancergb`.
The module such a probe names tracks the heap layout, not the bug. For the same reason a hardware
watchpoint on the corrupted address does not survive a re-run: the worker threads make the layout
differ every time.

The reproduction trigger is worth keeping too, because the crash otherwise looks intermittent.
`dt_gui_presets_init()` (`gui/presets.c`) re-enables preset auto-generation whenever
`<version>|<UI language>` differs from `ui_last/presets_autogen_signature`, and that signature
only reaches `anselrc` on a **clean** exit — so a crash during startup loses it and every
relaunch replays the same path. Forcing that conf key to a bogus value in a throwaway
`--configdir` reproduces the whole startup on demand without touching the user's library.

## A pointer event's modifier state must reach the handlers whole

*Found `afa3b254fb`, 2026-09-07.*

`_button_pressed()`, `_button_released()` and `_mouse_moved()` (`gui/application.c`) pass
`event->state` to `dt_control_button_pressed()` / `_released()` / `dt_control_mouse_moved()`
without narrowing it, and `_scrolled()` does the same. That is not incidental tidiness: masking
the state to its low four bits keeps SHIFT, LOCK, CONTROL and MOD1 — every modifier that
matters on X11 and Win32, and none of the one that matters on Quartz. A physical Cmd is
reported there as `GDK_MOD2_MASK` (0x10), on button and motion events exactly as on key events
(`get_keyboard_modifiers_from_ns_flags()` in GDK's own `gdkevents-quartz.c`), and that bit is
what `DT_PRIMARY_MASK` resolves to and what every shortcut is registered and matched against.
Drop it and every primary+click and primary+drag gesture in the application is unreachable on
macOS — inserting a mask node, constraining a shape — while the same code keeps working
everywhere else, because CONTROL survives such a mask. It is a whole-platform failure with no
error anywhere, and it looks like a bug in whatever feature is reported first.

Nothing downstream reads those bits raw. Every consumer goes through `dt_modifier_is()` /
`dt_modifiers_include()` (`widgets/widget_settings.h`), which mask with
`gtk_accelerator_get_default_mod_mask()` — the button bits a drag adds are not in it, and
neither is `GDK_MOD2_MASK` on X11, where that bit is NumLock. So the narrowing buys nothing the
consumers do not already do correctly per platform. Note the modifier state travels as the
parameter spelled `which` on the `mouse_moved` chain (`dt_control_mouse_moved()` through every
IOP's `mouse_moved()`); it is modifiers there too, not a button number — `iop/vignette.c` reads
it with `dt_modifier_is(which, DT_PRIMARY_MASK)`.

## The `-d input` keystroke trace hooks the generic `event` signal, and spells the primary modifier itself

*Found `d17e1e95cd`, 2026-09-07.*

`_log_key_event()` (`gui/application.c`) is a GTK emission hook, installed by `dt_gui_gtk_init()`
only when that channel is on. It is on **`GtkWidget::event`, not `key-press-event`**: a widget
emits the generic signal first and the specific one only if nothing handled it, and
`dt_accels_dispatch()` is connected to `event` on the main window and returns TRUE for every
keystroke that fires a shortcut — so a hook on `key-press-event` prints every key the program
ignores and none of the ones it acts on. An emission hook is also what makes the trace global:
keys go to whichever toplevel has the focus, each handles its own, and no single handler sees
them all. One keystroke reaches the hook several times as `gtk_propagate_event()` walks the focus
chain with the same `GdkEvent`, so the first emission is printed and the repeats are skipped.

Two things about a key event that the trace has to state rather than pass through:

- **`state` holds the modifiers as they were BEFORE the event**, so a modifier key's own press
  carries none of its bit and its release carries it, with the same keyval either way. That is
  why the primary modifier is announced from the KEY (`Control_L`/`Control_R`, `Meta_L`/`Meta_R`
  on Quartz — Cmd is reported as the Meta keysym) and not from the state.
- **The primary token comes from `DT_PRIMARY_MASK`, not from `gtk_accelerator_name()`.** On
  Quartz one physical Cmd sets `GDK_MOD2_MASK` — the bit every shortcut is registered and matched
  against — plus GDK's virtual `GDK_META_MASK` duplicate, and GTK names the pair as two separate
  modifiers (`<Primary><Mod2>`), spelling one keystroke twice. Both bits come out before naming
  and `<Primary>` is printed once, the same duplicate `_accels_keys_decode()` and
  `dt_modifier_is()` drop before matching. The raw `state` is printed alongside, in hex.
- **A toplevel is named by its title as well as its type.** Every toplevel is a `GtkWindow`, so
  the type alone cannot say whether a key reached the main window or a panel holding the keyboard
  (the shape manager while a name is edited) — which is the question such a trace is read for.

---
