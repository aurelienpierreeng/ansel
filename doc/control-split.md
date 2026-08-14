# Decomposing `src/control` — the god-header, the thread pool, and the GTK half

Measured 2026-08-15 by five parallel censuses (struct fields, scheduler, signal bus, header
fan-in, upward reach), each attacked by a checker. `src/control` is 8168 lines over 19 files and
carries **38 of the tree's 184 layering violations** — 21% of the debt in 2% of the code.

Six of the load-bearing claims below were then re-measured independently before this file landed,
because a census that agrees with itself proves nothing: `control.h:56 → libs/lib.h`; the
`main_message` use-after-free; the mutex census (9 declared, 8 initialised, 7 destroyed —
`global_mutex` initialised and used at `control.c:938-974` but never destroyed, `image_mutex`
declared and nothing else); the fan-in figures; control's 38-of-184 share (41 of 187 before
`widgets/` moved to 2.5, the difference being exactly the three `control/ → widgets/` edges this
file already accounts for); and `('control', 1)` → **165**.

The `main_message` use-after-free is **already fixed**, separately and ahead of this plan — it is
a live crash, not a layering concern, and had no business waiting for a 13-PR sequence. PR6 below
therefore no longer closes it.

## Why it bites

**`control/control.h:56 #include "libs/lib.h"` is the whole mechanism.** `dt_control_t`
(`control.h:220-321`) is four objects in one `calloc`: a GTK input/cursor/message facade, a
progress service, a thread pool, a lifecycle flag. The progress vtable types its callbacks in
`dt_lib_module_t` (`control.h:296-302`) — a layer-7 type in a layer-3 struct — and that one line
puts **24 project headers** (`libs/lib.h`, `views/view.h`, `common/image.h`, `history/history.h`,
`metadata/geo.h`, `common/cups_print.h`, …) into **all 125 includers**. Of those, **72 name only
`dt_control_log`**, and ten of the message-only files are `iop/*.c` plugin `.so`s compiling the
view API for one `printf`. Removing the edge collapses `libs/lib.h` from 141 compiling TUs to 37,
`control/jobs.h` from 136 to 32, `control/progress.h` from 130 to **2**.

**The supply lines, not the fan-out, are the trap.** Removing the edge and recomputing
reachability: 123 files reach `control/progress.h` only through `control.h`, 99 reach
`libs/lib.h` and `control/jobs.h`, 79 reach `views/view.h`. By *symbol* the residue is small but
real: **9 files name `dt_view_t` in their own code** while reaching `views/view.h` only through
`control.h` (`views/dev_toolbox.c`, `libs/duplicate.c`, `libs/histogram.c`, `develop/develop.c`,
`gui/guides.c`, `gui/actions/{edit,views}.c`, `control/jobs/control_jobs.c`,
`common/folder_survey.c`) — the `colorprofiles/colorspaces.h` failure verbatim: green in Release
*and* Debug, red in `build-nofeatures`.

**Four edges point at `develop/`, and that is what costs this refactor by name.**
`control.c:61 → develop/develop.h` — transitive fan-in **195**, dragged in for two field reads
(`darktable.develop->progress.{total,completed}`, `control.c:593-597`); `control_jobs.c:93` and
`import_jobs.c:23 → develop/history_merge.h` for one enum and one batch-state type;
`control_jobs.c:105 → develop/imageop_math.h` for one `static inline` (`FCxtrans()`, used at
`control_jobs.c:418`). None is a control concern; all four are type placement. Another **22
inbound violations block the layer-1 closures** — `common/` 18, `caches/` 3, `pixel/` 1, the last
being `pixel/fast_guided_filter.h`, a layer-2 **header** dragging the god-header into five
consumers for one `dt_control_log()` at line 298.

**Cross-thread state shares the struct.** Nine mutexes declared, eight initialised, seven
destroyed: `global_mutex` leaks on every GUI run, `image_mutex` is referenced by nothing
tree-wide. Headless initialises **two** (`darktable.c:1549-1550`) then locks six zeroed ones on
reachable paths (`ansel-cli --import` → `film_jobs.c:96` → `progress.c:279`). One live
use-after-free: `dt_control_draw_busy_msg` reads `darktable.main_message` unlocked at four
external sites (`gui/dtgtk/thumbnail.c:745`, `preview_window.c:148`, `views/slideshow.c:492`,
`views/studio_capture.c:858`) while the pipeline worker `dt_free`s it per module per frame
(`pixelpipe_hb.c:975` → `darktable.c:750-756`).

## The design: evict the GTK half, keep the directory, then re-layer

`src/control` survives, redefined as **work that has not happened yet** — the scheduler, the
progress objects that describe it, the signal bus that announces it, the flag that says whether
the loop is alive. `jobs.c` already names nothing but dtpthread, a clock, a logger and a thread
count; `signal.c` touches control in exactly two places (`signal.c:372`, `:444`). What leaves is
the GTK: the input router, the cursor, the view-switch shims, the log/toast rendering,
`crawler.c`'s 580-line GtkTreeView, `control_jobs.c`'s 324 GTK lines.

Three grafts decide the shape. **Every header split lands in place, at the same layer, before any
file moves** — `control/user_message.h` sits at layer 3 exactly like `control/control.h`, so the
wide include-repoint PRs are ratchet-neutral *by construction*, and nothing is renamed (a
tree-wide `dt_control_log` rename would detonate the in-flight `t4b…t6a` stack across its 311
call sites in 84 files). **The closure is a ratchet in `check_module_boundaries.sh`**, alongside
the six that already exist. **The final act is a one-line layer move, measured not assumed**:
`('control', 1)` in `tools/include_graph.py` gives **184 → 165, cycles 0** today — the 22 inbound
violations retire while the three `control/ → widgets/` includes (`control.c:57`, `crawler.c:49,50`,
legal only because widgets is 2.5) flip. Land it **after** the GTK half is out, or the ratchet
stops measuring the debt it exists to measure.

## The sequence

Each PR builds and passes the gates alone. Δ is the expected `layering_violations`, re-measured
with `include_graph.py --what-if` before the PR opens; any PR whose count falls commits
`tools/include_baseline.txt` in the same commit (the gate fails on a fall too).

| PR | content | verified by | Δ |
|---|---|---|---|
| **1** | Fix `tools/header_consumers.py:73-76` (strips `//` before string literals, so a URL eats its closing quote — 7550 of 9491 chars blanked on `gui/actions/help.c`, reported as using *nothing* from `control.h` while calling six of its symbols). Add `control/user_message.h` (`<glib.h>` only, layer 3), move the 8 message declarations, `control.h` includes it, repoint the 27 message-only includers. No rename, no field touched, no CMake edit (`src/CMakeLists.txt:247` globs `control/*.h`). | re-run the tool on `help.c`; `check_unused_includes.sh`; four configs incl. `build-nofeatures` | **184** |
| **2** | `control/redraw.h` — the five `SIGNAL_RAISE` one-liners (`control.c:865-888`); 55 touchers, 18 exclusive. Same shape. | as PR1 | **184** |
| **3** | `control/input.h` + `cursor.h`. Extend `dt_control_pointer_input_t` (`control.h:71-88`) with the button fields it lacks; convert the 8 raw readers; `views/darkroom.c`'s 8 drag-anchor writes become darkroom state. Delete `button_type`, `history_start`, `last_expose_time`, `image_mutex`, `global_mutex`. **Add the gate: no `src/iop/` file dereferences `dt_control_t`.** | crop/clipping/vignette drags by hand — live state machines, see traps | **184** |
| **4** | Progress vtable → `dt_progress_handlers_t { void *ctx; … }`, installed and retracted as one call; `control.h` still includes `libs/lib.h`. Fixes `libs/backgroundjobs.c:162-166` (nulls 5 of 6 slots) and `progress.c:344-359` (cancel destroys the mutex it holds — user-triggerable on a queued job). | cancel a not-yet-started import under ASAN | **184** |
| **5** | **Delete `control.h:56`.** Whole content: the 9 `dt_view_t` files plus a resolver for `common/folder_survey.c:281` (layer 1 — adding the include there would *raise* the ratchet). Delete `dt_ctl_switch_mode_to_by_view` (zero callers, sole `dt_view_t` user in the header). | per-file table in the PR body; `build-nofeatures` | **183** |
| **6** | `control.c`'s GUI half → `gui/` (expose, busy paint, event router, view-switch shims, log/toast rendering). `develop->progress.*` inverts through the existing `develop/pipeline_notify.h`; `control.c:58 darktable.h` goes (`dt_get_main_message()` is declared 426 lines above the site that ignores it). Closes the `main_message` UAF. | `-d control`; thumbnail + slideshow expose during a darkroom render | **180** |
| **7** | `crawler.c` splits at line 245: scanner (168 lines, 0 GTK) → `database/`; dialog (580 lines) → `gui/dialogs/`. | crawler run on a scratch library | **179** |
| **8** | `control/jobs/` dissolves **per job, never wholesale**: `control_jobs.c` → `imageio/export_job.c` + `gui/actions/` + `common/`; `film_jobs.c`, `import_jobs.c` likewise; delete `jobs/image_jobs.c` (93 lines, zero callers). | export + HDR-merge pixel A/B; folder import | **≈172** |
| **9** | Type relocation: `dt_history_merge_strategy_t`/`dt_hm_batch_state_t` → `history/`; `FC`/`FCxtrans` → `pixel/`; `DT_CTL_WORKER_RESERVED` → `system/sys_resources.h`. Delete `control/settings.h` (both its types have zero users; its 9 includers want `control/signal.h`). | four configs | **≈168** |
| **10** | Lifecycle symmetry: one init/cleanup pair, every mutex initialised and destroyed on both paths, matched allocator (`calloc` at `darktable.c:897` vs `dt_free` at `:2006`), teardown reordered above `dt_control_signal_cleanup`. Delete `proxy.hinter` and the ignored `s` parameter (−21 accessor sites). | clean `rm -rf build && ninja install`, **staged** binary, ASAN | **≈168** |
| **11** | **`('control', 1)` in `tools/include_graph.py`.** One line. | the printed summary, not the argument | **≈149** |
| **12** | Seal: `control/control_private.h` holds the struct, `control.h` publishes an opaque typedef and seven functions; ratchet in `check_module_boundaries.sh` — `control_private_baseline=0`, `control_fields_baseline=45`, `control_upcalls_baseline=16`, `toolkit_control_baseline=5`, all measured today. | plant a `dt_control_get_global()->running` in `libs/`, confirm the gate fails | **≈149** |
| **13** | The scheduler's synchronisation, alone: predicate and wait under one mutex, **delete the `sleep(2)` kicker** (`jobs.c:571-583`), broadcast inside the lock in `dt_control_shutdown` (`control.c:455-456`), drain `job_res[]`, rename `dt_control_flush_jobs_queue` to what it does. | enqueue from 8 threads, no job queued > 50 ms; 100 start/quit cycles, no hang | **≈149** |

## Traps

**`proxy.hinter` cannot be the cause of its own TODO.** It is the **last member** of
`dt_control_t` (`control.h:305-321`), nothing reads or writes it tree-wide, and
`sizeof(dt_control_t)` has exactly one consumer (`darktable.c:897`) in the always-rebuilt main
binary — so no offset shifts and no stale plugin writes past a smaller allocation. The
stale-`.so` hypothesis is dead on arrival; what survives is pre-existing corruption whose
landing site moved when the allocation shrank ("it crashes wherever the next unlucky reader
lands"). Delete it in PR10 *with* the allocator pairing fixed, never as a standalone experiment.

**The ABI-relevant fields are `tabborder`, `width`, `height`, `gui_thread`** (`control.h:223-225`),
not the dead ones. All 8 external derefs read `button_down`/`button_down_which` at
`control.h:226`; only those four members precede them. The window-geometry concern that PR6 sends
to `gui/` is the move that shifts a stale plugin's read — which is why PR3's gate lands first.

**`dt_control_get_pointer_input()` (`control.h:71-103`) exists, is unused, and lacks the button
fields.** It is also a snapshot copy via out-param, while all 8 raw readers consult the flags
*live* inside drag state machines (`iop/clipping.c:2798-2800` sets `g->straightening` from
`button_down_which` mid-drag). Snapshot-vs-live is a silent GUI defect, not a crash; PR3 decides
it explicitly.

**Moving the job files wholesale measures worse than doing nothing**: `film_jobs`/`import_jobs`
into `common/` = −1, `control_jobs.c` into `libs/` = −3, against −4 for the header work alone.
Split per job, each body to the module that owns its data. Same lesson as the `history/` cluster.

**A directory absent from `LAYERS` is invisible in both directions** (`include_graph.py:168-170`
skips the edge when `lb is None`) — `src/osx` is already in that state, which is why three
`osx/osx.h → <gtk/gtk.h>` edges out of `control/` go unmeasured. Any new directory lands its
`LAYERS` entry in the same commit, or the ratchet reports an improvement because it stopped
looking.

**The signal bus is dead in every headless run.** `dt_control_signal_raise` early-returns on
`!dt_control_running()` (`signal.c:369-372`) and `running` is set by `dt_control_jobs_init()`
(`jobs.c:649`), which never runs headless — the structural reason the four notify/handler seams
had to be invented. The gate also returns *before* `va_start`, so the four ownership-transferring
signals leak their `GList`/`gchar*` on every drop (`caches/image_cache.c:542` leaks one node per
`dt_image_cache_write_release()` in every CLI export). Fixing the gate position is cheap; flipping
the bus live headless is a behaviour change for 52 signals and belongs in its own PR.

**`log_busy` gates the cursor** — `dt_control_commit_cursor` early-returns on it
(`control.c:330`), `dt_control_expose` picks the progress cursor from it (`control.c:581-588`).
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
