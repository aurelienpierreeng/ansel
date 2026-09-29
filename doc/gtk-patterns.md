<!-- Provenance: every finding carries the commit it was established against. -->

# GTK and UI patterns

> **Verified against `42eca0e8fe` on 2026-09-29.**
>
> Layout, focus, repaint and threading patterns in the GUI, most of them measured offscreen rather than reasoned about — because in each case the obvious guess was wrong.
>
> Each finding below is dated with the commit that established it. A finding is only as
> good as its hash: before acting on one older than the code you are changing, re-measure
> it — and re-date it here when you do. Where an earlier version of a claim was **wrong**,
> that is recorded rather than quietly corrected: how a claim was wrong is usually the more
> useful thing to know.

Related: [`darkroom-redraw.md`](darkroom-redraw.md), [`overlay-raster.md`](overlay-raster.md), [`thumbtable.md`](thumbtable.md), [`gui.md`](gui.md).

## Thumbtable scrolled-window sizing

*Found `22f623c0be`, 2026-06-25.*

Three separate, mostly-invisible per-cell overheads must be budgeted for the thumbtable
`GtkFixed` grid to fit flush:

1. **scrollbar-spacing** — GtkScrolledWindow legacy GtkWidget *style property* (default 3px),
   NOT a CSS box property. Zero it via `-GtkScrolledWindow-scrollbar-spacing: 0` in CSS on
   `#thumbtable-scroll` / `#panel-scroll`.
2. **frame borders** — `GTK_SHADOW_ETCHED_IN` + the implicit GtkViewport's `GTK_SHADOW_IN` both
   add a `.frame` class. Set both to `GTK_SHADOW_NONE`.
3. **per-cell decoration** — `.thumb-cell { border: 4px solid transparent; margin: -2px; }` (`data/themes/ansel.css:1498`) makes each
   cell ~4px wider than the `thumb_width` stride. Budget it:
   `thumb_width = floor((new_width - deco) / cols)`.

**Critical:** `dt_thumbtable_configure` is the single source of truth for thumb geometry. Pass
the already-computed `new_thumbs_per_row/new_thumb_width/new_thumb_height` to `_grid_configure`,
which must only STORE them, never re-derive. If two code paths compute thumb geometry with
different formulas, `thumbs_changed` is true on every idle tick → full grid repopulate every
tick → ~20% idle CPU.

**Filmstrip-specific:** the filmstrip `scroll_window` must be the MAIN child of `parent_overlay`
(via `gtk_container_add`), NOT an overlay child. Overlay children on Wayland use an offscreen
path and go stale/blank until a pointer event invalidates them. The filmstrip vertical scroll
policy must be `GTK_POLICY_EXTERNAL` + `set_min_content_height(1)` +
`set_propagate_natural_height(FALSE)` to allow the resize handle to shrink the panel.

**Re-entry init — and a correction.** This used to say `dt_thumbtable_show` "must reset
`last_parent_width/height` and `last_h_scrollbar_height/last_v_scrollbar_width` to -1". It does
not, and never has: `dt_thumbtable_show()` is a static inline (`gui/dtgtk/thumbtable.h:395`)
whose whole body is three `gtk_widget_show()` calls and a `dt_thumbtable_queue_update()`. The
only initialiser is the constructor (`gui/dtgtk/thumbtable.c:2011`), and it sets the two parent
dimensions to **0**, not -1 — only the two scrollbar fields are -1. A reader following the old
text would have added a reset that never existed, with a sentinel wrong for half the fields.
What actually guards a same-size re-entry is the size-allocate handler (`thumbtable.c:267`)
together with `filemanager.c:216` and `filmstrip.c:193`.

## The mask overlay's outlines are rasterised directly, not stroked by cairo

*Found `30ff989bc7`, 2026-09-07.*

A shape's outline reaches the drawing code already sampled at one point per device pixel;
handing it to cairo as a path to stroke twice was raster → vector → raster, 7–18 ms per shape
per frame at fit zoom. `widgets/stroke_raster.c` paints the polyline itself (a per-segment
capsule distance field, two passes, dashes cut by arc length, the same one-pixel ramp as
`CAIRO_ANTIALIAS_FAST`) into the ARGB32 surface `cr` draws on — the current group's surface,
offset included — and `dt_draw_shape_lines()` tries it before falling back to the cairo strokes
for any other target. `masks_gui.c` draws everything into a persistent device-pixel canvas and
composites only the painted rectangle. Nodes, handles, arrows and the creation trace stay with
cairo, on the same canvas. `doc/overlay-raster.md` is the account; the numbers and the pixel
comparison come from `ansel-test-masks-geometry --time-overlay` (add `MASKS_DEBUG=1` for the
per-stage traces). Three traps: stamping discs instead of capsules scallops thin lines; inside
a pushed group the writable surface is `cairo_get_group_target()`, not `cairo_get_target()`; and
**cairo's device space is not the pixel grid** — `cairo_user_to_device()` stops before the
surface's own transform (pixel = device × device_scale + device_offset), so on a 2x screen every
pixel derived from it must be multiplied by `cairo_surface_get_device_scale()`. The first version
did not, was correct on every 1x screen and every unit test, and drew the whole overlay at half
size in the top-left quadrant of a HiDPI darkroom; `test_stroke_raster` and the harness's
`hidpi placement` check now paint on device-scaled surfaces.

## The darkroom centre paints into GTK's buffer, once per source frame, and a mask motion repaints a rectangle

*Found `a678989c33`, 2026-09-09.*

`doc/darkroom-redraw.md` prices every pass of the repaint path with cairo at a 2560x1440
window at device scale 1 and 2. Before it, every centre repaint allocated and freed a
full-window surface, filled two full-window backgrounds and blitted the window three times
through two intermediate surfaces, whatever had changed: about 10 ms at 1x and 59 ms at 2x
per frame before any overlay, and a zoom or pan while the main pipe caught up scaled the
preview with cairo's default filter, 262 ms at 2x, because `CAIRO_FILTER_NEAREST` was set on
the context's default source before the surface replaced it. Four rules now:

- **`dt_control_expose(cr, w, h)` paints into GTK's own `cr`**, which is a double buffer
  already and arrives clipped to what was invalidated. No intermediate surface, no pixmap.
  A view that paints every pixel returns `VIEW_FLAGS_PAINTS_WHOLE_AREA` from `flags()` and
  the toplevel skips its background fill under it.
- **The darkroom composes `dev->image_surface` once per (source hash, viewport, colours,
  border, size) key** (`_darkroom_compose_locked()` / `_darkroom_compose_fallback()`), fills
  the background as the four bands around the image and the ISO 12646 frame as a ring, and
  sets a scaling filter on the pattern that scales. Set `cairo_pattern_set_filter()` AFTER
  `cairo_set_source_surface()`, never on the context's default source.
- **A motion the masks handled invalidates a rectangle**: `dt_masks_overlay_queue_redraw()`
  asks for the last composited overlay rectangle grown by the pointer's motion, and
  `_overlay_damage_record()` asks for whatever a frame painted outside the expose's clip, so
  an under-estimate costs one more small frame and never an unpainted overlay. A module's own
  overlay knows no rectangle and keeps the full redraw.
- **The overlay canvas is sized to the view, never to `cr`'s clip**, which a rectangle redraw
  narrows; and a creation session's frame is bounded to the session's box and the live shape,
  where it was the whole window before.
- **A group's unselected members live in the static layer** (`_static_layer_ensure()`,
  `masks_gui.c`): stroked once into a view-sized surface, composited under the live canvas with
  one clipped blit, keyed on the view matrix, the group, the selection, the overlay colours and
  every other member's outline signature (counts plus every 32nd sample). Only the selected member
  is stroked per frame. A rebuild marks the whole frame dirty, so a selection change under a small
  invalidation gets its one full repaint. Cairo's bound covers the selected member's header alone,
  since that is all cairo draws for a group. Measured on the harness's 11-member group: 14.1 → 4.0
  ms a full frame with nothing selected, 16.4 → 5.7 with one selected. The harness's `group-11`
  case is the regression check; its "build" column, 340 ms, is the outline rebuild and is #1391's
  next item.

The views are plugins: `ninja ansel` does NOT compile `src/views/*.c`. Build every target
(`ninja`) before trusting a darkroom edit; a use of an undeclared variable in `darkroom.c`
survived an `ansel` build here.

## A GTK class overriding `style_updated` must chain up, or CSS opacity freezes

*Found `b35c95990d`, 2026-09-16.*

`GtkWidget`'s own `style_updated` is what turns the CSS `opacity` into the alpha the widget is
painted with (`gtk_widget_update_alpha()`), besides queuing the resize/redraw a style change owes.
A subclass that replaces the vfunc without calling `GTK_WIDGET_CLASS(parent_class)->style_updated`
keeps the opacity of the state its style was first computed in. The theme dims `*:disabled` to 0.5,
so a bauhaus slider created insensitive and enabled later -- the blend panel's boost factor --
stayed half transparent while fully sensitive: it looked disabled and took every input. Measured,
not guessed: the widget and every ancestor reported sensitive, with the same text colour as its
neighbours; only the painted alpha differed. `_style_updated()` (`widgets/bauhaus.c`) chains up
first.

The omission is a GObject trap, not a typo: a `"style-updated"` *signal handler* runs after the
class handler, so connecting one keeps GTK's work; a *vfunc override* replaces it. `53f1ae442a`
moved bauhaus from the first to the second. It stayed invisible for four years because the theme
greyed `*:disabled` with a `color`, which bauhaus reads from the current state at every draw;
`73fbeacda9` switched that rule to `opacity: 0.5`, the one property the override had stopped
updating. So a theme rule moving from one CSS property to another can expose a widget class that
only ever honoured the first: after such a change, check every class overriding `style_updated`
(`grep -rn "style_updated = " src`) chains up.

## A rotated GtkLabel sizes the column it sits in

*Found `0175dac0f2`, 2026-08-28.*

A `GtkLabel` with `gtk_label_set_angle()` requests the width of its *slanted* bounding box, so a
diagonal column title makes its whole column that wide — measured on the masks wheel-mapping
grid: 102 px for "Fading/Curvature" at 45° against 24 px for the radio button underneath it.
Zeroing `column-spacing` does not help, because the spacing was never what separated the cells.

Two things fix it, and both are needed. Give the grid `GTK_ALIGN_START`: handed more width than
it needs (`gtk_box_pack_start(..., TRUE, TRUE, 0)` does exactly that), `GtkGrid` spreads the
surplus over its columns and re-centres every title in a cell wider than itself. Then attach each
title **spanning the columns to its right** — the direction it leans into, whose header space is
free — so its width constrains that sum rather than one column, with one extra `hexpand` column
at the end to absorb the last title's overhang. Measured: 24 px columns at any spacing, so the
panel's usual gutter can stay.

Verify this class of layout by measuring, not by looking: build the widget in a
`gtk_offscreen_window_new()`, pump `gtk_events_pending()`/`gtk_main_iteration()`, and print
`gtk_widget_get_allocation()` for the cells. It answers in seconds what several rebuild-and-look
round trips do not.

## A height/width request must cover the CSS border, not just the padding

*Found `63f63475e1`, 2026-08-31.*

A widget's size request is its whole CSS box: padding *and* border come out of the allocation
before the content sees any of it. Code that sizes an area to fit its content therefore has to
add both back, and `gtk_style_context_get_padding()` without the matching
`gtk_style_context_get_border()` leaves the content short by exactly one border.

Two pixels is enough to be visible, because the widgets that care answer a shortfall with a
whole scrollbar rather than a clipped pixel. `dt_ui_scroll_wrap()` (`widgets/scroll_wrap.c`)
sizes every list and textview in the application to `clamp(min(content, cap), min_size, 75%
window)`, snapped to whole rows so it never shows a half-row — no slack anywhere — and its
`GtkScrolledWindow` carries `.dt_recessed_scroll`, which the theme gives `padding: 2px` over a
`border: 1px`. Counting the padding alone handed the viewport 123 px for 125 px of content, and
`GTK_POLICY_AUTOMATIC` did the rest. Measured, same rows and same CSS, border omitted then
counted: `page=123 < upper=125, scrollbar` → `page=125 = upper=125, none`.

A list that must show where it ends asks for one blank row past its content with
`dt_ui_scroll_wrap_reserve_trailing_row()` — the shape manager's two lists do, since a list
filled edge to edge cannot be told from one with rows hidden below. It is part of the sizing rule,
not a resize of the window around the list: a window grown by a row right after `show_all()` is
snapped back to its content as soon as the lists realize and compute their height, and would
otherwise gain a row at every opening once its geometry is saved and restored.

Reproduce this class of bug offscreen in seconds: build the widget with the theme's CSS on it,
pump the main loop, then compare the scrolled window's vadjustment `page_size` against `upper`.
A scrollbar that appears for a couple of pixels looks like a content-height miscount and is
usually a frame the request forgot.

## A UTILITY window must ask for its position, transient-for is not enough

*Found `7032b624e8`, 2026-09-01.*

`gtk_window_set_transient_for()` ties a window to its parent for stacking and focus, but the
window manager still decides *where* to map it. For an ordinary toplevel it places it over the
parent; give it `GDK_WINDOW_TYPE_HINT_UTILITY` and it drops the window at the root origin
instead — the leftmost monitor on a multi-head setup, whichever screen the application is
actually on. Measured on X11 with two monitors (primary at x=1920), same parent and same
transient hint throughout: transient alone lands at (2020, 129), transient + UTILITY at (0, 0),
and the focus flags (`set_focus_on_map`, `set_accept_focus`) change nothing either way.

So a UTILITY window states its position itself, `GTK_WIN_POS_CENTER_ON_PARENT`, on every
platform — not inside a `#ifdef GDK_WINDOWING_QUARTZ` block, which is how the shape manager
panel (`libs/shape_manager.c`) came to open on the wrong screen while the module-order graph
(`libs/ioporder.c`), the tag manager (`libs/tagging.c`) and the event supervisor
(`gui/actions/supervisor_window.c`) — none of which set the UTILITY hint — opened correctly.

The hint costs nothing for a panel shown and hidden repeatedly: GTK consults it on the first
mapping only, so a window the user has dragged elsewhere keeps the place they gave it across
later hide/show cycles.

## A window a toggle button opens has exactly one state: the button's

*Found `4793b08e64`, 2026-09-01.*

`gtk_widget_hide_on_delete()` is the usual answer to a window whose widgets and state must
survive being closed, but it hides the window behind the back of whatever opened it. When the
opener is a `GtkToggleButton` — the shape manager panel's toolbox button (`libs/shape_manager.c`) — the
button stays pressed after a window-manager close, and the next click reads that state as "the
panel is open" and hides an already-hidden window: it takes two clicks to bring the panel back.

So the button's `active` flag is the panel's only state. Visibility is driven from `toggled`,
never from `clicked`, and the `delete-event` handler hides nothing itself — it un-presses the
button and returns `TRUE`, leaving that same `toggled` handler to save the geometry and hide.
The order matters: `gtk_toggle_button_set_active()` emits `clicked` as well as `toggled`, so a
`clicked` handler flipping visibility would re-show the window it was just asked to close. The
handler compares `active` against the window's actual visibility and returns when the two agree,
which is what makes it safe to re-enter from the close path.

## Modal dialogs must explicitly refocus their parent on close

*Found `ca1f9f95f4`, 2026-07-20.*

`gtk_window_set_transient_for()` at dialog creation is not enough to guarantee focus returns to
the parent window once the dialog is destroyed — on macOS/quartz in particular, GTK does not
reliably hand keyboard focus back the way X11 window managers do with transient hints.

Every top-level modal dialog (one whose transient parent is the main window, not another
still-open dialog) must call `dt_gui_refocus_parent()` (`widgets/dialog.c:64`, declared in `widgets/dialog.h` — it lived in `gui/gtk.{h,c}` until `55954b77ea` deleted that file) right after
`gtk_widget_destroy()`. It falls back to the main window if no valid parent is passed, and
handles the macOS-specific `dt_osx_focus_window()` call internally. Mechanical pattern (capture
the parent *before* destroying the dialog, since the widget is invalid afterwards):

```c
GtkWindow *dialog_parent = gtk_window_get_transient_for(GTK_WINDOW(dialog));
gtk_widget_destroy(dialog);
dt_gui_refocus_parent(dialog_parent);
```

Do NOT apply this to a nested dialog (e.g. a warning/confirm popup) whose transient parent is
another dialog still open at that point — GTK already hands focus back to a live parent window
correctly; this only matters for the final return to the application. Also skip
`GtkFileChooserDialog`/native choosers, which are a separate, already-correct subsystem. Any
dialog created without a transient parent at all (e.g. a popup menu action, which cannot
legitimately use the popup's own toplevel — see `gui/dtgtk/thumbnail.c`'s "Active modules" dialog)
should instead be parented directly to `dt_ui_main_window(darktable.gui->ui)` at creation time.

## Worker-thread → GUI-thread deferred callbacks referencing a shared struct need a refcount

*Found `de27912d4d`, 2026-07-22.*

The `g_main_context_invoke(NULL, callback, params)` pattern (worker thread schedules `callback` to
run later on the GUI thread) is used throughout the codebase to touch GTK widgets safely from a
non-GUI thread. When `params` carries a pointer into a struct that can also be torn down
independently through the same pattern (e.g. `libs/backgroundjobs.c`'s per-job
`dt_lib_backgroundjob_element_t`, updated via `.updated`/`.message_updated`/`.cancellable` and torn
down via `.destroyed`, all reachable concurrently from worker threads doing pixel/import work), a
"destroy" callback can run — and free the struct — while an "update" callback scheduled earlier for
the same struct is still queued, waiting for its turn on the GTK main loop. The queued update then
dereferences freed memory (Sentry issue 130394919: `EXCEPTION_ACCESS_VIOLATION` in
`gtk_label_set_text`, called from a stale `dt_lib_backgroundjob_element_t*`).

`control->progress_system.mutex` (`control/progress.c`) only serializes the *scheduling* of these
callbacks against each other — it says nothing about their relative *execution* order on the GUI
thread, and does not protect a struct shared across independent worker threads that don't otherwise
synchronize with each other before calling into the progress API.

Fix pattern (mirrors the forms/history-item refcounting above): give the shared struct a
`dt_atomic_int refcount`. Every proxy function that schedules a callback referencing it takes a
reference first; every callback drops its reference on the way out, freeing the struct only when
the count reaches zero — whichever callback that happens to be. The "destroy" callback additionally
NULLs every GTK widget pointer in the struct (right after removing/destroying them) instead of
freeing the struct outright, so any other callback still queued for the same struct sees NULL and
skips the now-invalid widgets instead of touching freed GTK objects.

---
