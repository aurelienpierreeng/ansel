# Quitting — what the process waits for once the window is gone

> **Measured 2026-09-30 on top of `cc0cd199d6`**, RelWithDebInfo, clang 19, 8 cores, 4 pool
> workers. Every figure below says how it was obtained. Re-measure before acting on one older than
> the code you are changing, and re-date this line when you do.

Closing the main window hides it at once. The process ends later: when the jobs that were running
have returned, and the rest of `dt_cleanup()` has run. This file is about that interval — what
fills it, what the user sees during it, and what it still gets wrong.

## The sequence

`dt_control_quit()` (`control/control.c:452`) is how a quit starts: the window's `delete-event`,
the Quit menu entry and the macOS dock all call it. It hides the main window
(`dt_gui_gtk_quit()`), clears `running`, broadcasts the workers' condition and stops `gtk_main()`.
`dt_gui_gtk_run()` then calls `dt_cleanup()`, which — for the part that matters here — asks its
last questions (database maintenance and snapshot), leaves the current view, and calls
`dt_control_shutdown()` (`control/control.c:475`). That is where the workers are joined.

**Clearing `running` cancels nothing.** A worker finishes the job it is running and takes no
other; the jobs still queued are disposed of, unrun, by `dt_control_jobs_drain()`. So a quit waits
for the running jobs, for as long as they run: a thumbnail pipeline goes to its end, and an export
of N images goes to its last image, since `dt_control_export_job_run()` loops on the job's
cancelled state alone (`control/jobs/control_jobs.c:1413`) and nothing sets it.

## What it costs

The quit is requested by calling `dt_control_quit()` on the GUI thread, from an idle source armed
by an `LD_PRELOAD`ed shim N seconds after start — what `delete-event` does, without driving the
window. "Quit to exit" runs from that call to the shim's destructor. The library is a scratch one:
60 copies of a 24 MP NEF, each with a 14-module history, so that every thumbnail is a pipeline.

| Situation | Quit to exit | Of which, joining the workers |
|---|---|---|
| Lighttable, nothing running | **0.32 – 0.35 s** (five quits, 0.5 s apart) | none |
| Darkroom, nothing running | **0.49 – 0.51 s** (three quits) | none |
| Lighttable, four thumbnails rendering | **1.4 – 7.6 s** (six quits) | all but 0.5 – 0.6 s |

The third row is the job, not the quit: the wait ends with the last `[run_job-]` line of `-d
control`, to within 0.1 s, every time. Under gdb, with a breakpoint on each stage of
`dt_cleanup()`, one such quit took 4.98 s of which `dt_control_shutdown()` held 3.94 s; leaving
the view took 0.21 s, `dt_lib_cleanup()` 0.23 s, and the stretch from `dt_conf_cleanup()` to
`dt_opencl_cleanup()` 0.31 s.

Twelve further start-and-quit cycles, the quit 2.0 to 5.3 s after launch in steps of 0.3 s so
that it lands at every stage of the startup jobs, all exited with status 0, in 0.23 to 1.84 s.

**Not measured:** a quit during an export, which cannot be started without the GUI. That it runs
to the last image is read from the loop above, not timed. Nor the network flushes at the top of
`dt_cleanup()` (`dt_sentry_shutdown()`, `dt_telemetry_shutdown()`), which a self-build compiles
out.

## The wait is spent in the main loop

`dt_control_shutdown()` does not go straight to `pthread_join()`. It first calls the handler
installed with `dt_control_set_shutdown_wait_handler()` (`control/control.h:323`), which returns
once `dt_control_workers_alive()` (`control/jobs.c:544`) reads 0 — after which the joins return
at once. The GUI registers `dt_gui_closing_wait()` (`gui/closing.c`) next to its other handlers
(`gui/application.c:1412`); a headless run registers nothing, and its joins block as they always
did. `control/` reaches the GUI through that pointer and includes nothing of it.

`dt_gui_closing_wait()` runs a `GMainLoop` on the default context and polls the count every
100 ms. Two things follow from the loop turning rather than the thread blocking.

**The user is told.** After one second of waiting, a small window comes up — *Ansel is finishing
its work before closing* — with the number of jobs still running and, for those that publish a
progress (`dt_control_progress_foreach()`, `control/progress.c:411`), what they say they are
doing: `exporting 3 / 20 to disk`. It cannot be closed and it goes away with the last job. A quit
that is over within the second shows nothing.

Under *Details*, collapsed by default, the window lists the jobs themselves, from
`dt_control_jobs_foreach()` (`control/jobs.c:549`): the running ones one by one, the queued ones
counted by kind, under a header that says they will not run. A job's kind is the queue it was added
to: *Image operation*, *Thumbnail*, *Background task*, *Export* (prints share that queue),
*Maintenance*, and *Darkroom rendering* for the reserved worker. Beside a running job stands the
description it was created with, written for the debug log in English, and translated only where
it is also a catalog string, as the generic image jobs' are. A reserved worker's running job is
held nowhere else, so `dt_control_run_job_res()` records it in `_job_res_running[]`, under
`res_mutex`, for as long as it runs.

Measured on top of `cc1400c7b9`, with the method of *What it costs* and the quit 5 s after launch, a trace of
the grid's rows at 1.03 s into the wait read: running, three *Thumbnail* (`get image 2`, `3`, `4`);
queued, 19 *Image operation* and 16 *Thumbnail*. The 19 were `save xmp` jobs of the import: of
the 60 it queued, 41 had run, counted by their `[run_job-]` lines.

**A job that needs the GUI thread gets it.** Two waits in the tree block a worker until the GUI
thread has run something for it: a synchronous signal raised from a worker (`control/signal.c`,
the `g_cond_wait()` after `g_main_context_invoke()`), and the deletion job's error dialog
(`control/jobs/control_jobs.c`, `_dt_delete_file_display_modal_dialog()`). Against a GUI thread
sitting in `pthread_join()` on that very worker, either one never returns: the window is gone and
the process stays. This is read from the code, not reproduced — it needs the quit to land inside
a window a few instructions wide.

The loop turning has a cost, and a grab pays it. The windows the main one may leave on screen —
the tag manager, the shape manager's popup, the module order window, the supervisor — could not
answer while the GUI thread was blocked. `dt_gui_closing_wait()` holds a GTK grab on an invisible
widget for as long as it waits, so they still cannot: input events and `delete-event` alike go
to the grab. The widget is realised, because GTK delivers events to realised widgets only, and
never shown, because a mapped one is a window on screen.

Sources already queued on the main context — idles posted by the jobs, timers — are dispatched
during the wait, with the view left and `running` cleared. That is the state in which
`dt_cleanup()` has always drained them, right after the joins; what changes is that the timers
that fall due during a long wait fire too.

## macOS

Nothing here has been run on macOS. What follows is read from the code of this tree and from
what GTK's Quartz backend is known to do.

**The quit arrives the same way.** Cmd+Q, the application menu and the Dock's Quit all reach
`applicationShouldTerminate:`, which gtk-mac-integration turns into the `NSApplicationBlockTermination`
signal; `_osx_quit_callback()` (`gui/application.c:301`) calls `dt_control_quit()` and returns
TRUE, which cancels the Cocoa termination and leaves the exit to `dt_cleanup()`. So the wait is
reached, and a second Cmd+Q during it is harmless: it hides a window already hidden, clears a
flag already clear, finds no `gtk_main()` to stop, and cancels the termination again.

**The loop turns there too.** Quartz drives its event loop from a poll function installed on the
default main context, so any loop on that context dispatches it, not only `gtk_main()`. The
database-maintenance question that `dt_cleanup()` asks runs a nested `gtk_main()` at the same
point, which is the precedent.

**The window is brought forward.** A quit from the Dock, or from the application switcher, leaves
another application active, and a window opened by an inactive application opens behind the
active one's windows. The notice is therefore presented and the application activated
(`dt_osx_focus_window()`), as `dt_gui_refocus_parent()` does for dialogs. It also gets
`dt_osx_disallow_fullscreen()`, as every other window does: it must not become a full-screen space
of its own, and must show over the one the main window may have left.

**Open on macOS:** a quit while Ansel is hidden (Cmd+H). Activation unhides the application;
whether AppKit then brings back the main window it had recorded at hide time, although it has
since been ordered out, has not been checked.

## The scheduler's side

`dt_control_workers_alive()` counts the threads of `control/jobs.c` — pool, reserved, kicker —
that have started and not returned. Once `running` is cleared it only falls, and it is the number
of jobs still running. Two things had to hold for it to reach 0 on its own.

**A quit wakes every thread that is waiting.** A worker reads `running` at the top of its loop,
finds no job, then waits on the condition. A quit landing between the two used to broadcast to
nobody; the worker slept until the kicker's next turn. The workers now re-read `running` under
`cond_mutex` — the mutex it is cleared under — right before waiting (`control/jobs.c:615`,
`:670`), so the quit either finds them waiting or stops them from starting to.

**The kicker does not sleep through a quit.** It used to `sleep(2)` between two broadcasts, and
`dt_control_shutdown()` joined it first: every quit waited for what was left of those two
seconds. Measured on an idle lighttable before the change, quits requested 0.5 s apart took 0.90,
0.37, 1.86, 1.39 and 0.89 s — a sawtooth of period 2 s. The kicker now spends its two seconds in
`pthread_cond_timedwait()` on the workers' condition (`control/jobs.c:625`): the quit's broadcast
ends the wait, any other broadcast resumes it against the same deadline, so the kicks keep their
pace. The same five quits take 0.32 to 0.35 s.

The kicker still exists, and so does the race it papers over: a job queued between a worker's
empty-handed `dt_control_run_job()` and its wait is not seen until the next kick. Closing that
needs the queue test under `cond_mutex`, which is PR 13 of `control-split.md`.

## Open

- **Queued jobs are dropped.** A second export queued behind the first never runs if the user
  quits, and neither do the sidecar writes an import leaves queued: 19 of 60 in the measurement
  above. The window's details say so, once the window is up; a quit over within the second drops
  them unseen. Whether a quit should run some queues to their end is a decision
  nobody has taken.
- **A thumbnail being rendered cannot be abandoned.** The 1.4 – 7.6 s above are spent finishing
  four images nobody will look at. `dt_dev_pixelpipe_has_shutdown()` is what a pipeline polls,
  and nothing raises it for a thumbnail pipe at quit.
- **The window offers no way to stop the work.** An export shown there runs to its end. The
  progress objects of the cancellable jobs carry what a button would need
  (`dt_control_progress_cancel()`).
- **What comes after the joins is still silent**: about 0.3 s of `dt_cleanup()` on the GUI
  thread, with no window. Nothing can be shown there without pumping the loop from
  `darktable.c`.
- **A quit under a nested main loop does not end the process.** `dt_control_quit()` calls
  `gtk_main_quit()` only when `gtk_main_level() > 0`. Seen once: asked to quit while the privacy
  consent dialog of a first launch was up inside `dt_init()`, the main window was hidden,
  `running` cleared, and `gtk_main()` then started and never returned.
