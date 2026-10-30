# Decomposing `src/control` — the god-header, the thread pool, and the GTK half

> **Corrected against `2959bd4ec1` on 2026-09-29.** The audit before that found 13 claims in
> this file wrong of the tree and 33 stale. Most were stale *by success*: **PR1 and PR2 have
> landed and PR3 is half landed**, which moved the line numbers, the includer counts and the
> symbol census the plan was written against. Everything below now reflects the tree as it is;
> the figures re-measured for this pass are marked **(2026-09-29)**. Re-measure before acting on
> a claim older than the code you are changing, and re-date this line when you do.

Measured 2026-08-15 by five parallel censuses (struct fields, scheduler, signal bus, header
fan-in, upward reach), each attacked by a checker. `src/control` was 8168 lines over 19 files and
carried **38 of the tree's 184 layering violations** — 21% of the debt in 2% of the code. The
tree-wide count is **183 (2026-09-29)**; the landed PRs were ratchet-neutral by design, so
control's share has not moved.

Six of the load-bearing claims below were then re-measured independently before this file landed,
because a census that agrees with itself proves nothing: `control.h → libs/lib.h`; the
`main_message` use-after-free; the mutex census (9 declared, 8 initialised, 7 destroyed —
`global_mutex` initialised and used but never destroyed, `image_mutex`
declared and nothing else); the fan-in figures; control's 38-of-184 share (41 of 187 before
`widgets/` moved to 2.5, the difference being exactly the three `control/ → widgets/` edges this
file already accounts for); and `('control', 1)` → **165**. That last one still holds exactly:
re-measured 2026-09-29, the layer move gives **183 → 165, cycles 0**.

The `main_message` use-after-free is **already fixed**, separately and ahead of this plan — it is
a live crash, not a layering concern, and had no business waiting for a 13-PR sequence. PR6 below
therefore no longer closes it.

## Why it bites

**`control/control.h`'s `#include "libs/lib.h"` is the whole mechanism** — line **58**
(2026-09-29; it was :56 when this was written, and PR1 and PR2 inserted two includes above it).
`dt_control_t` (`control.h:204-303`) is four objects in one `calloc`: a GTK input/cursor/message
facade, a progress service, a thread pool, a lifecycle flag. The progress vtable types its
callbacks in `dt_lib_module_t` — a layer-7 type in a layer-3 struct — and that one line puts
**24 project headers** (`libs/lib.h`, `views/view.h`, `common/image.h`, `history/history.h`,
`metadata/geo.h`, `common/cups_print.h`, …) into every includer of `control.h`: **84** of them
(2026-09-29, down from 125 — that is PR1 and PR2 working).

**The `dt_control_log` half of the argument is spent, and that is what PR1 bought.** Of the 84
includers left, **none** names `dt_control_log` as its only `control.h` symbol, and none is under
`src/iop/`. The message API is `control/user_message.h` (`<glib.h>` only), with 37 direct
includers of its own; `control/redraw.h` has 11 and `control/input.h` 5. What remains behind the
god-header is genuine control use.

**Removing the edge now collapses `libs/lib.h` from 102 compiling TUs to 38 and `views/view.h`
from 109 to 62 (2026-09-29).** It does **not** touch `control/jobs.h` (88) or
`control/progress.h` (84): the earlier "136 → 32" and "130 → **2**" were wrong of the tree even
when written, because `control.h` includes both of those *directly* (`:54` and `:57`), so no
change to the `libs/lib.h` edge can reach them. Their fan-in falls when `control.h` itself stops
being universally included, not before.

**The supply lines, not the fan-out, are the trap.** By *symbol* the residue is small but real:
removing `control/control.h` from the graph and re-deriving reach, **6 files name `dt_view_t` in
their own code** while losing `views/view.h` (2026-09-29): `common/folder_survey.c`,
`control/jobs/control_jobs.c`, `develop/develop.c`, `gui/actions/edit.c`, `gui/actions/views.c`,
and `control/control.h` itself. (The list was 9 when written; `views/dev_toolbox.c`,
`libs/duplicate.c`, `libs/histogram.c` and `gui/guides.c` have since gained the include or
stopped naming the type.) This is the `colorprofiles/colorspaces.h` failure verbatim: green in
Release *and* Debug, red in `build-nofeatures`.

**Four edges point at `develop/`, and that is what costs this refactor by name.**
`control.c → develop/develop.h` — transitive fan-in **195**, dragged in for two field reads
(`darktable.develop->progress.{total,completed}`, `control.c:656,660` — the last two
cross-module `darktable.develop` references in the tree, see `doc/globals-migration.md`);
`control_jobs.c:93` and
`import_jobs.c:23 → develop/history_merge.h` for one enum and one batch-state type;
`control_jobs.c:105 → develop/imageop_math.h` for one `static inline` (`FCxtrans()`, used at
`control_jobs.c:418`). None is a control concern; all four are type placement. Another **22
inbound violations block the layer-1 closures** — `common/` 18, `caches/` 3, `pixel/` 1. That
last one is **fixed**: `pixel/fast_guided_filter.h:43` now includes `control/user_message.h`
(81 lines, `<glib.h>` only) for its one `dt_control_log()` at :298, instead of dragging the
god-header into its five consumers (`iop/cacorrectrgb.c`, `iop/highlights/laplacian.c`,
`iop/toneequal.c`, `pixel/box_filters.c`, `pixel/eigf.h`). It is the worked example of what PR1
does at scale. The `common/ -> control/` count is **17 (2026-09-29)**.

**Cross-thread state shares the struct.** Nine mutexes declared, eight initialised, seven
destroyed: `global_mutex` leaks on every GUI run, `image_mutex` is referenced by nothing
tree-wide. Headless initialises **two** then locks six zeroed ones on reachable paths
(`ansel-cli --import` → `film_jobs.c:96` → `progress.c:280`).

**`global_mutex` is live and must not be deleted** (corrects PR3 below, which listed it for
deletion). It is initialised at `control.c:114` and guards the four mouse-over / keyboard-over id
accessors at `control.c:1003-1039` — `dt_control_{get,set}_mouse_over_id()` and their keyboard
twins. What it needs is a `destroy`, not a delete. Genuinely dead by contrast, and safe to
delete: `button_type`, `history_start`, `last_expose_time`, `image_mutex` — each has its
declaration and no reader. (`grep global_mutex` also hits `common/global_mutexes.h` includes in
six unrelated files; those are a different thing.)

One live
use-after-free — **since fixed**: `darktable.c:581` now holds a `static dt_pthread_mutex_t
_main_message_lock` and `:583` publishes `dt_get_main_message_copy()`, which `control.c:557`
calls. Before that, `dt_control_draw_busy_msg` read `darktable.main_message` unlocked at four
external sites (`gui/dtgtk/thumbnail.c:745`, `preview_window.c:148`, `views/slideshow.c:492`,
`views/studio_capture.c:858`) while the pipeline worker `dt_free`s it per module per frame
(`pixelpipe_hb.c:975` → `darktable.c:750-756`).

## The design: evict the GTK half, keep the directory, then re-layer

`src/control` survives, redefined as **work that has not happened yet** — the scheduler, the
progress objects that describe it, the signal bus that announces it, the flag that says whether
the loop is alive. `jobs.c` already names nothing but dtpthread, a clock, a logger and a thread
count; `signal.c` touches control in exactly two places (`signal.c:372`, `:444`). What leaves is
the GTK: the input router, the cursor, the view-switch shims, the log/toast rendering,
`crawler.c`'s 585-line GtkTreeView, `control_jobs.c`'s 324 GTK lines.

Three grafts decide the shape. **Every header split lands in place, at the same layer, before any
file moves** — `control/user_message.h` sits at layer 3 exactly like `control/control.h`, so the
wide include-repoint PRs are ratchet-neutral *by construction*, and nothing is renamed (a
tree-wide `dt_control_log` rename would detonate the in-flight `t4b…t6a` stack across its 311
call sites in 84 files). **The closure is a ratchet in `check_module_boundaries.sh`**, alongside
the six that already exist. **The final act is a one-line layer move, measured not assumed**:
`('control', 1)` in `tools/include_graph.py` gives **184 → 165, cycles 0** today — the 22 inbound
violations retire while the three `control/ → widgets/` includes (`control.c:57`, and
`crawler.c`'s `widget_settings.h` and `widget_style.h`, legal only because widgets is 2.5)
flip. Land it **after** the GTK half is out, or the ratchet stops measuring the debt it
exists to measure.

## The sequence

Each PR builds and passes the gates alone. Δ is the expected `layering_violations`, re-measured
before the PR opens; any PR whose count falls commits `tools/include_baseline.txt` in the same
commit (the gate fails on a fall too). Note `--what-if` takes `path=layer` pairs and cannot
express a change to the layer *table* — for PR11, edit `('control', 3)` in a copy of
`tools/include_graph.py` and run `--summary`.

**Status (2026-09-29): PR1 ✅ landed, PR2 ✅ landed, PR3 ◐ half landed** — `control/input.h`
exists with 5 includers and no `src/iop/` file dereferences `dt_control_t` any more (the gate
that PR3 asks for would pass today), but `cursor.h` was not split out and the dead-field
deletion has not happened. **PR4 onward are open.** `control.h:58` still includes `libs/lib.h`;
`tools/include_graph.py:52` still says `('control', 3)`; `check_module_boundaries.sh` has no
control section.

| PR | content | verified by | Δ |
|---|---|---|---|
| ~~**1**~~ ✅ | **Landed.** Fixed `tools/header_consumers.py` (it stripped `//` before string literals, so a URL ate its closing quote — 7550 of 9491 chars blanked on `gui/actions/help.c`, reported as using *nothing* from `control.h` while calling six of its symbols; `strip_noise` is now ONE pass and carries that measurement inline). Added `control/user_message.h` (`<glib.h>` only, layer 3, 7 declarations at :56-69), `control.h` includes it at :55, and the message-only includers were repointed — 37 direct includers today. No rename, no field touched, no CMake edit (`src/CMakeLists.txt:266` globs `control/*.h`). | re-run the tool on `help.c`; `check_unused_includes.sh`; four configs incl. `build-nofeatures` | **184** |
| ~~**2**~~ ✅ | **Landed.** `control/redraw.h:47-59` — the five `SIGNAL_RAISE` one-liners; `control.h:56` includes it; 11 direct includers today. Same shape. | as PR1 | **184** |
| **3** ◐ | **Half landed**: `control/input.h` exists (5 includers) and **no `src/iop/` file dereferences `dt_control_t`** — but the gate was never added, `cursor.h` was not split out, and no field was deleted. Remaining: extend `dt_control_pointer_input_t` (the struct is `control.h:73-90`, the getter `:109`) with the button fields it lacks; convert the raw readers; `views/darkroom.c`'s drag-anchor writes become darkroom state. Delete `button_type`, `history_start`, `last_expose_time`, `image_mutex` — **but NOT `global_mutex`, which is live** (see above). Add the gate. | crop/clipping/vignette drags by hand — live state machines, see traps | **183** |
| **4** | Progress vtable → `dt_progress_handlers_t { void *ctx; … }`, installed and retracted as one call; `control.h` still includes `libs/lib.h`. Fixes `libs/backgroundjobs.c:162-166` (nulls 5 of 6 slots) and `progress.c:345-360` (cancel destroys the mutex it holds — user-triggerable on a queued job). | cancel a not-yet-started import under ASAN | **184** |
| **5** | **Delete the `libs/lib.h` edge — `control.h:58` today, not :56.** Whole content: the 6 `dt_view_t` files plus a resolver for `common/folder_survey.c` (layer 1 — adding the include there would *raise* the ratchet). Delete `dt_ctl_switch_mode_to_by_view` (still zero callers, still the sole `dt_view_t` user in the header). | per-file table in the PR body; `build-nofeatures` | **183** |
| **6** | `control.c`'s GUI half → `gui/` (expose, busy paint, event router, view-switch shims, log/toast rendering). `develop->progress.*` inverts through the existing `develop/pipeline_notify.h`; `control.c:58 darktable.h` goes — note that is no longer free, since `control.c` dereferences `darktable.control->` throughout (127 sites) and `darktable.view_manager` (16). The `main_message` UAF is already closed, separately. | `-d control`; thumbnail + slideshow expose during a darkroom render | **180** |
| **7** | `crawler.c` splits at line 571: scanner (570 lines, 0 GTK) → a module of its own; dialog (585 lines) → `gui/dialogs/`. **Not `database/` any more**: the scanner runs as a background job and names `control/control.h` and `control/jobs.h`, so it sits at layer 3 like the dialog it leaves. It also enumerates directories, which 17 other files in the tree do by hand — `xmp-crawler.md` argues the inventory is the module, and the scanner its first caller. | crawler run on a scratch library | **179** |
| **8** | `control/jobs/` dissolves **per job, never wholesale**: `control_jobs.c` → `imageio/export_job.c` + `gui/actions/` + `common/`; `film_jobs.c`, `import_jobs.c` likewise; delete `jobs/image_jobs.c` (93 lines, zero callers). | export + HDR-merge pixel A/B; folder import | **≈172** |
| **9** | Type relocation: `dt_history_merge_strategy_t`/`dt_hm_batch_state_t` → `history/`; `FC`/`FCxtrans` → `pixel/`; `DT_CTL_WORKER_RESERVED` → `system/sys_resources.h`. Delete `control/settings.h` (both its types have zero users; its 9 includers want `control/signal.h`). | four configs | **≈168** |
| **10** | Lifecycle symmetry: one init/cleanup pair, every mutex initialised and destroyed on both paths, matched allocator (`calloc` at `darktable.c:897` vs `dt_free` at `:2006`), teardown reordered above `dt_control_signal_cleanup`. Delete `proxy.hinter` and the ignored `s` parameter (−21 accessor sites). | clean `rm -rf build && ninja install`, **staged** binary, ASAN | **≈168** |
| **11** | **`('control', 1)` in `tools/include_graph.py`.** One line. | the printed summary, not the argument | **≈149** |
| **12** | Seal: `control/control_private.h` holds the struct, `control.h` publishes an opaque typedef and seven functions; ratchet in `check_module_boundaries.sh` — `control_private_baseline=0`, `control_fields_baseline=45`, `control_upcalls_baseline=16`, `toolkit_control_baseline=5`, all measured today. | plant a `dt_control_get_global()->running` in `libs/`, confirm the gate fails | **≈149** |
| **13** | The scheduler's synchronisation, alone: the *queue* predicate and its wait under one mutex, then **delete the kicker** (`jobs.c:632-654`) that exists to cover for it, broadcast inside the lock in `dt_control_shutdown` (`control.c:481-482`), drain `job_res[]`, rename `dt_control_flush_jobs_queue` to what it does. The `running` half is in place (2026-09-30): the workers re-read it under `cond_mutex` before waiting, and the kicker waits on the condition instead of sleeping, so a quit no longer waits for either — see `shutdown.md`. | enqueue from 8 threads, no job queued > 50 ms; 100 start/quit cycles, no hang | **≈149** |

## Traps

**`proxy.hinter` cannot be the cause of its own TODO.** It is the **last member** of
`dt_control_t` (`control.h:305-321`), nothing reads or writes it tree-wide, and
`sizeof(dt_control_t)` has exactly one consumer (`darktable.c:897`) in the always-rebuilt main
binary — so no offset shifts and no stale plugin writes past a smaller allocation. The
stale-`.so` hypothesis is dead on arrival; what survives is pre-existing corruption whose
landing site moved when the allocation shrank ("it crashes wherever the next unlucky reader
lands"). Delete it in PR10 *with* the allocator pairing fixed, never as a standalone experiment.

**The ABI-relevant fields are the ones that PRECEDE the button flags**, not the dead ones. The
struct is `control.h:204-303` and opens `int32_t width, height;` (:207), `pthread_t gui_thread;`
(:208), `double button_x, button_y;`, `int history_start;`, then the over-ids. (An earlier draft
named `tabborder` first; there is no such member and there has not been one — the only
`tabborder` in the tree is a comment at `gui/application.c:1431` recording its removal.) The
window-geometry concern that PR6 sends to `gui/` is the move that shifts a stale plugin's read —
which is why PR3's gate lands first.

**`dt_control_get_pointer_input()` is no longer unused** (corrects an earlier claim): the struct
is `control.h:73-90`, the getter `:109`, and `iop/drawlayer.c` calls it at :2648, :3540, :3739
and :3795. It still lacks the button fields, and it is still a snapshot copy via out-param —
which is the half of the trap that survives. Live readers now go through the
`dt_control_button_down(n)` accessor rather than a raw deref (`iop/clipping.c:2983,2985` sets
`g->straightening` that way mid-drag), so the remaining question PR3 must decide is
snapshot-vs-live, not accessor-vs-field. Snapshot-vs-live is a silent GUI defect, not a crash.

**Moving the job files wholesale measures worse than doing nothing**: `film_jobs`/`import_jobs`
into `common/` = −1, `control_jobs.c` into `libs/` = −3, against −4 for the header work alone.
Split per job, each body to the module that owns its data. Same lesson as the `history/` cluster.

**A directory absent from `LAYERS` is invisible in both directions** (`include_graph.py:168-170`
skips the edge when `lb is None`) — `src/osx` is already in that state, which is why three
`osx/osx.h → <gtk/gtk.h>` edges out of `control/` go unmeasured. Any new directory lands its
`LAYERS` entry in the same commit, or the ratchet reports an improvement because it stopped
looking.

**The signal bus is dead in every headless run, and now says so.** `darktable.c:1540` reads
`darktable.signals = init_gui ? dt_control_signal_init() : NULL;` (commit `b2f672b75d`, "signals:
only the GUI gets the signal system"), so headless has no bus to raise on. The second gate,
`dt_control_running()`, was the *only* thing stopping it when this was written, and is false
there too: `running` is set by `dt_control_jobs_init()` (`jobs.c:722`), which only
`dt_control_init()` calls, and only under `init_gui` (2026-09-29). Either way the bus never
fires headless — the structural reason the four notify/handler seams had to be invented. A
dropped signal still has its arguments collected, and the four that take ownership of a
`GList`/`gchar*` run their destructor on them (`_signal_release_unsent()`, 2026-09-29), so the
list `dt_image_cache_write_release()` hands the bus in every CLI export is freed. Flipping the
bus live headless is a behaviour change for 52 signals and belongs in its own PR.

**`log_busy` gates the cursor** — `dt_control_commit_cursor` early-returns on it
(`control.c:346`), `dt_control_expose` picks the progress cursor from it (`control.c:644-651`).
PR1 moves the counter, PR6 moves the cursor; the arbitration must become an explicit call or the
watch cursor silently stops appearing during `iop-autoset`. And **`dt_control_log` is not a
no-op headless**, despite the comment at `darktable.c:737-738`: it writes the ring and arms
`g_timeout_add`/`g_idle_add` on a main context that never runs, i.e. unbounded GSource
accumulation in every batch export. After PR1 a NULL handler drops it — a fix, but a behaviour
change, and it belongs in the PR text rather than in someone's bisect.

## Open questions

**Keep the scheduler in `control/`, or give it a new layer-1 directory?** Keep it. `src/runtime/`
was proposed; its name is a weight class, not a concern, and "everything with no heavy
dependencies" accepts anything — which is how `common/` happened. The surviving directory needs a
one-sentence definition and a ratchet, not a new name.

**PR11 before or after PR8?** After. The −19 is partly bookkeeping — 22 inbound edges become
same-layer-legal while the real coupling, 125 TUs compiling `views/view.h` through `control.h`,
is what PRs 1-6 remove. Banking the number early stops the gate measuring the debt.

**Is `darktable.control == NULL` headless the end state?** Yes, but not in this plan.
`develop/dev_pixelpipe.c:67,76` already uses `IS_NULL_PTR(dt_control_get_global())` as its headless
probe and that probe is false today. Making it true turns every unguarded deref into a loud CLI
crash; schedule it after PR12, when the dispatchers no longer need the struct.

**Does the 42% of the signal bus that is genuinely one-to-many stay a bus?** Yes, and not here.
30 of 52 signals have one consumer file or none (3 dead, 4 emit-only, 1 listen-only, 2 self, 15
point-to-point, 5 fan-in); converting them to the notify seam is a second project with its own
census. This plan only makes the bus honest — gate after `va_start`, teardown order, the missing
`g_cond` predicate loop and `g_cond_clear` (`signal.c:451-460`).
