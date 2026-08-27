# Studio Capture

Studio Capture is a view (an "atelier") for shooting tethered sessions: it
monitors a folder for incoming images, imports them automatically (optionally
applying styles), and shows the latest shot full-size with the filmstrip
below — without the darkroom's editing layer.

## Layout

Same base layout as Lighttable (left panel, filmstrip at the bottom instead
of a thumbnail grid), minus the top toolbar's collection filters, since
browsing/filtering the library is not this atelier's job.

Left panel, top to bottom:

- **Scopes** — histogram/waveform/vectorscope, same module as darkroom's.
- **Import** — the folder survey engine's settings (see below).
- **Style** — styles auto-applied to every imported image.
- **Tags**, **Metadata**, **Notes**, **Datetime and GPS**, **Exif and IPTC** —
  the same modules used elsewhere in the library, made visible in this view
  via their `views()` lists.

At the bottom of the left panel: darkroom's **module toolbox**
(`libs/tools/module_toolbox.c`), made visible here the same way — its
`views()` list includes `"studio_capture"`. The toolbox itself is just a
flow-box container; each button docked into it is tagged with the view(s) it
should appear in when registered through `dt_view_manager_module_toolbox_add()`.
It's a single lib instance shared across every view for the whole app
lifetime — buttons accumulate in it as each view's `gui_init()` runs (once
per view, not per visit), and `view_enter()` only shows/hides each one to
match the current view; nothing is ever removed. Hiding must target the
`GtkFlowBoxChild` wrapper `gtk_container_add()`/`gtk_flow_box_insert()`
auto-creates around each button, not the button itself: a visible-but-empty
wrapper still reserves a cell and its spacing in the flow layout, which used
to leave gaps and push later buttons rightward whenever a view hid some of
its siblings (visible in darkroom as the mask manager button drifting away
from the rest, and in Studio Capture as its buttons failing to pack flush
left) — `view_enter()` now hides/shows `gtk_widget_get_parent()` of the
button when that parent is a `GtkFlowBoxChild`.

Raw overexposed, clipping, soft-proof and gamut check, ISO 12646 and Picture
display are built by `views/dev_toolbox.c`/`.h` — a shared unit extracted out
of `darkroom.c`, designed so any future atelier with its own `dt_develop_t`
can reuse the same buttons, not just these two. A single entry point,
`dt_dev_toolbox_create(dev, views, buttons, n_buttons)`, takes an array of
`dt_dev_toolbox_button_t` values and creates all of them — button *and*
options popover — for the given `dev` in one call. Every button is wired to
one shared "clicked" handler (`_button_clicked`) that reads back *which*
button fired from a tag set on the widget at creation (`g_object_set_data`),
instead of connecting a dedicated callback function per button; popovers
(and the sliders/combos inside them) are equally shared, anchored through
`dt_dev_toolbox_connect_popover()`/`dt_dev_toolbox_show_popup()` — the same
generic show/anchor plumbing darkroom's own guides and auto-set popovers use
via `dt_dev_toolbox_popover_set_preshow()` for their own pre-show refresh.
Each button still ends up stored in its matching `dt_develop_t` field
(`dev->overexposed.button`, `dev->profile.softproof_button`, etc.) as it
always did, since those fields are plain, already-shared `dt_develop_t`
members, not darkroom-specific state.

Only one thing is NOT built by `dt_dev_toolbox_create()`, left for the
caller to add on top of the buttons it returns: **view-specific popover
extras**. Picture display's popover holds only background brightness and
margins (generic); darkroom fetches that same content box with
`gtk_bin_get_child(GTK_BIN(dev->display.floating_window))` and packs its own
rendering-size combo and mask-preview-checkerboard section into it before
calling `gtk_widget_show_all()` itself (which `dt_dev_toolbox_create()` does
not do for Display's popover, precisely so a caller can still append to it
first — Studio Capture, appending nothing, just calls `gtk_widget_show_all()`
right away).

Accelerators are shared too, via a second entry point:
`dt_dev_toolbox_add_accels(dev, accel_group, category, buttons, n_buttons)`.
The action names ("Toggle clipping indication", "Focus softproof
options"...) are the same regardless of caller, since the buttons themselves
are shared — only the accelerator group and category differ. darkroom's own
`gui_init()` calls `dt_dev_toolbox_create()` once with its `dev`,
`DT_VIEW_DARKROOM` and all six button kinds, then `dt_dev_toolbox_add_accels()`
with `darktable.gui->accels->darkroom_accels` and `N_("Darkroom/Toolbox")`.
Studio Capture's own `gui_init()` calls the *same* two functions with its own
`d->dev`, `DT_VIEW_STUDIO_CAPTURE`, and — since its `enter()` connects
`lighttable_accels` as its active group (see `dt_accels_connect_active_group(...,
"lighttable")`), not `darkroom_accels` — `darktable.gui->accels->lighttable_accels`
and `N_("Studio capture/Toolbox")`, so its keyboard shortcuts actually fire
while this view is active. Either way the toolbox ends up with two separate
button instances per kind (same icon/position, different `dev`), and shows
only the one matching the active view.

This makes both the toggles' *state* and their popovers' controls correct
here — clicking flips this view's own `dev->overexposed.enabled`, moving a
slider writes this view's own `dev->overexposed.lower`, not darkroom's — but
the effect is still **not visible**: it's darkroom's own `expose()` that
paints these overlays directly onto its pixelpipe backbuffer, a path Studio
Capture's center never touches since it renders through the surface fetcher
(mipmap cache) instead. Making the effect itself show up here needs
replicating that overlay-drawing logic against the surface fetcher — in
practice, the same darkroom-expose-core extraction already flagged as a
deliberately deferred, separate task. Picture display's button isn't a
toggle at all — it does nothing on click, its only role is to anchor its
popover — so in Studio Capture it opens a working popover whose sliders
persist to conf correctly but, like the toggles, have no visible effect yet.

Guides and the guides popover are a single, already-global `GtkWidget`
(`darktable.view_manager->guides_toggle`, created once in `darkroom.c`'s
`gui_init()` and registered with `DT_VIEW_DARKROOM | DT_VIEW_STUDIO_CAPTURE`)
rather than a per-`dev` field, so it didn't need extracting: the same widget
instance is simply shown or hidden by the toolbox depending on the active
view, and its state (a global toggle, not tied to any one `dev`) is correct
in both views already.

Auto-set, the pipeline node graph (both darkroom-editing concepts) and the
mask manager popup (`libs/masks.c`) stay `DT_VIEW_DARKROOM` only, unextracted.

## Import module

Session controls at the top, then two tabs:

- At the top: the scan frequency and the session status label. The source
  folder and the scan frequency are locked while the session runs, since
  changing the monitored folder behind the engine's back would invalidate its
  baseline.
- *Source* tab: the folder chooser of the monitored folder.
- *Destination* tab: file handling (add vs copy) and, only when copying,
  delete-after-verify, **on conflict** policy (skip / overwrite / create
  unique filename), project date, jobcode, base directory and the two naming
  patterns with `$(` variable auto-completion, plus a live destination
  preview. The copy-only settings are hidden when images are added in place.
- The session **Start/Stop** button is the notebook's action widget
  (`gtk_notebook_set_action_widget`), sitting on the tab row itself, to the
  right of the *Source*/*Destination* labels, instead of taking its own row.

Settings are read from conf at each scan, so most changes take effect on the
next pass; the scan interval is applied the next time the session starts.
Monitoring never starts by itself: the user must press **Start** (or accept
the resume proposal below) in every application session.

**Start** is grayed out until the configuration has the minimum
`dt_folder_survey_can_start()` requires to succeed: a readable source folder,
a valid project date, and — only when copying — an existing, writable base
directory outside the source folder with non-empty naming patterns that
expand to a valid destination path. The status label shows the specific
reason it's disabled; the same check gates `dt_folder_survey_start()` itself,
so the button's state is never out of sync with what pressing it would do.

The project date field is validated through `dt_datetime_entry_to_exif()` ->
`dt_string_to_datetime()` (`common/datetime.c`), which overlays the typed
text byte-for-byte onto the `"0001-01-01 00:00:00.000"` template: a partial
value is valid (the untyped tail keeps the template's defaults, so `"2026"`
becomes `2026-01-01 00:00:00.000`), but the format itself — dashes/space/
colons/dot at those exact character positions — must match, since it's a
positional overlay, not a tolerant parser. Its tooltip spells the format
out explicitly for whoever types a full date/time by hand. The field also
carries the same calendar-popover date picker as the regular Import
dialog (`attach_popover()`, `gui/gtk.c`): picking a day writes it back as
`YYYY-MM-DD`, which always matches the overlay's expected format and lets
the untyped time-of-day tail fall back to the template's defaults.
Every field that affects this check re-evaluates it on change; **on
conflict** and **delete original file** don't, since they only affect what
happens to a conflicting/copied file, never whether the session can start.

The **Base directory of all projects** setting has its own conf key
(`studio_capture/base_directory_pattern`), independent from the regular
Import dialog's `session/base_directory_pattern` — editing one does not
affect the other. The only exception is a one-time seed:
`dt_folder_survey_init()` adopts the regular Import dialog's current base
directory the first time Studio Capture's own key is still empty, so the
field starts with a sensible value instead of blank.

## Style module

Two treeviews, **Pool** on top and **Styles** below it. Both carry per-row
action icons in dedicated fixed-width columns to the right of the name
column (which is packed with `expand=TRUE` to push them flush right),
clicked by column identity rather than by current selection — the same
pattern `develop/blend_gui.c` uses for its mask group rows' unlink/delete
icons:

- **Pool** — a flat, ordered treeview of the styles actually applied. This is
  the list used both on import and by the manual apply button below it: the
  first entry replaces the freshly imported history (paste mode *replace*),
  the following ones stack on top in pool order. Each row carries an up
  (`go-up-symbolic`), down (`go-down-symbolic`) and remove
  (`list-remove-symbolic`) icon, acting directly on the clicked row (no prior
  selection needed).
- **Styles** — every available style, grouped into a category tree by
  splitting the style name on `|` (same convention, and the same tree-building
  code path as `libs/styles.c`'s own style browser). Leaf rows (actual styles)
  carry an add icon (`list-add-symbolic`) that appends the style to the end of
  the pool; category rows have no fullname set, so the icon column hides
  itself on them. A leaf already in the pool is grayed out and shows a dimmed
  add icon so it reads as already queued.

  Graying is computed live, per row, by `gtk_tree_view_column_set_cell_data_func()`
  callbacks (`_studio_style_name_cell_data_func` for the text, gray via
  "foreground"/"foreground-set", and `_studio_style_add_cell_data_func` for the
  icon, swapping in a pre-dimmed pixbuf) rather than stored model columns or a
  "sensitive" binding: individual `GtkCellRenderer`-rendered cells inside a
  `GtkTreeView` are not separate CSS nodes, so the theme's `*:disabled { opacity }`
  rule never reaches them — a `sensitive=FALSE` cell disables clicks correctly but
  renders pixel-identical to an enabled one. The two dimmed/enabled icon variants
  are pre-rendered once at `gui_init` (`_studio_style_load_icon`, alpha-blended via
  Cairo) and freed in `gui_cleanup`. Adding or removing a style just calls
  `gtk_widget_queue_draw()` on the styles treeview, re-evaluating both
  cell_data_funcs for every visible row — no explicit row lookup needed, and
  expanded categories are left untouched.

The pool (an ordered `GList` of style names in `studio_style.c`) is the sole
source of truth, persisted to `studio_capture/styles` separated by the ASCII
unit separator (0x1F) since style names may contain any printable character.
The Styles tree is fully rebuilt (and the pool pruned of any name whose style
vanished) on `DT_SIGNAL_STYLE_CHANGED`, since the available styles themselves
may have changed.

## Center view

The center renders the displayed image through the asynchronous surface
fetcher (`dt_view_image_get_surface_async`), fit-to-window or at 100% with
panning (double-click, middle-click or scroll toggles the zoom; the arrow
keys pan at 100%, and Ctrl+/- toggles zoom the same as double-click since
there is no continuous scale to step through). Every new import becomes the
displayed image, so the view follows the shooting session.

That following is this atelier's own doing, and only here. An automatic import
carries the `DT_COLLECTION_IMPORT_VIEW_KEEP` policy
(`dt_collection_load_filmroll()`, `common/collection.c`), which moves nothing:
no view switch, no re-pointing of the collection onto the capture folder, no
forced hover or selection. The survey keeps running after the user leaves this
atelier — a capture landing while they browse or edit elsewhere must not drag
them anywhere. Studio Capture is the one exception on the collection side, so
its filmstrip still follows the session, and `_studio_set_image()` below is
what keeps the selection and the hovered image on the displayed capture.

Keyboard navigation must go through `d->zoom`/`pan_x`/`pan_y` — the same
fields the mouse handlers use — never through `d->dev->roi`: that develop
only feeds the Scopes module in the background (see below) and never reaches
the screen, so panning or zooming it has no visible effect.

Behind that display, the view owns its own `dt_develop_t` (`studio_capture.c`)
that is published as `darktable.develop` for as long as the atelier is
active, mirroring the pipeline-relevant subset of darkroom's `enter`/`leave`
(module loading, history load, pipeline start/teardown) without the darkroom
editing layer (IOP GUIs, undo, accels). Its only purpose is to give the
**Scopes** module — and any other module reading
`darktable.develop->preview_pipe` — a running pipeline to source data from;
the center display itself still comes from the surface fetcher, not from this
pipeline's backbuffer. The pipeline is resized from `expose()` rather than the
view's `configure()` callback, since `configure()` only fires on window
resizes and not on view entry.

A full develop-pipeline center (the darkroom's own live zoom/pan and expose
machinery) remains a possible later upgrade; it would require extracting that
machinery into a shared unit.

### Opening darkroom

Double-clicking (or pressing Return on) a filmstrip thumbnail only *previews*
that capture here, without leaving the atelier — this raises the same
`DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE` signal lighttable uses to open
darkroom, but Studio Capture's handler (`_studio_filmstrip_activate_callback`)
just calls `_studio_set_image()` instead, deliberately unchanged so a session
can be reviewed without ever leaving it.

`_studio_set_image()` — the single function every image-display change goes
through (filmstrip activate, a new import, the initial selection on
`enter()`) — keeps `darktable.selection` in sync with whatever it displays,
via `dt_selection_select_single()`, alongside its own active-image tracking.
This matters for two reasons: darkroom's `try_enter()` falls back to the
library selection (not to this view's own active-image tracking) whenever
nothing is under the mouse, and a newly auto-imported capture should become
the selected image the same way clicking it in the filmstrip would.

It also mirrors the displayed image into `dt_control_set_mouse_over_id()` and
`dt_control_set_keyboard_over_id()`, the same pair darkroom's own `enter()`
sets right before loading an image. darkroom's `try_enter()` checks
`mouse_over_id` *first*, ahead of the selection fallback above — without this,
a stale `mouse_over_id` left over from hovering a filmstrip thumbnail could
win over the selection and send darkroom to the wrong image.

Pressing Return while the *center* has focus (not the filmstrip) opens the
currently displayed capture in darkroom, via the view's own `key_pressed()`.
This only fires when the filmstrip hasn't already consumed the key: the
filmstrip's own key-press-event handler returns `TRUE` for Return, which
stops GTK from ever reaching the fallback `key_pressed()` path that
`dt_control_key_pressed()`/`dt_view_manager_key_pressed()` dispatch to the
current view.

### Color picker

The Scopes module's color picker works in this view too: point mode, and box
mode via ctrl+click or right-click on the picker button, exactly as in
darkroom. Clicking the center view sets `color_picker.primary_sample` in
image-normalized coordinates computed from the surface's own on-screen
placement (`_studio_widget_to_image_norm`), independent of the surface's
pixel resolution (thumbnail-sized in fit mode, full-res at 100%). Box corners
can be grabbed to resize; right-click reuses an overlapping live sample's
geometry, mirroring darkroom's `button_pressed`. In point mode, the picker
follows the pointer for as long as the button stays down. The overlay drawing
(crosshair/box, handles, dashing, swatch) replicates darkroom's
`_darkroom_pickers_draw` visual style using this view's own coordinate
mapping instead of darkroom's zoom/pan transform.

Three module-level singletons complicate reusing the picker outside darkroom:

- `dev->color_picker.histogram_module` / `.refresh_global_picker`: the Scopes
  module binds these once, at application startup, onto whichever develop is
  active then (always darkroom's, since that develop is created first).
- The Scopes panel's "current pick" readout widgets (numeric label + color
  swatch) are created once at startup and wired, via GTK signal `user_data`,
  to that same startup-time develop's `primary_sample` struct specifically —
  not to `darktable.develop->color_picker.primary_sample` dynamically. A
  separately-allocated `primary_sample` (as `dt_dev_init` gives every
  `gui_attached` develop) computes correct values but has no attached
  widgets, so nothing ever appears on screen.
- `dev->color_picker.picker`: the toggle button's own `dt_iop_color_picker_t*`
  closure, one physical widget shared by every view. `_refresh_active_picker()`
  (`gui/color_picker_proxy.c`) — the handler behind pipe-finished /
  cacheline-ready re-sampling — bails out unconditionally whenever
  `dev->color_picker.picker` is `NULL`, regardless of `.enabled`.

`enter()` copies the `histogram_module`/`refresh_global_picker`/`.picker`
bindings onto the viewer's own develop, and additionally **shares darkroom's
`primary_sample` instance** for as long as the atelier is active (stashing
the viewer's own instance in `own_primary_sample`). Live samples added while
the atelier is active copy from that shared instance and get freshly created
widgets of their own, so they work independently of the sharing. `leave()`
restores `own_primary_sample` before any pipeline teardown; `cleanup()`
restores it defensively too, in case the application quits while this view
is still active (so `dt_dev_cleanup()` frees the viewer's own instance,
never darkroom's shared one). `dt_iop_color_picker_reset()` in `leave()`
clears `d->dev->color_picker.picker` back to `NULL` as part of turning the
toggle off, so there is nothing to restore for it symmetrically to
`own_primary_sample`.

Without the `.picker` copy, switching to Studio Capture with the picker left
enabled in darkroom left `d->dev->color_picker.picker` at its zero-initialized
`NULL`: the picker still drew at the right place (drawing only reads
`primary_sample`, which is shared), but its value never advanced past
whatever darkroom had last sampled, since automatic re-sampling requires
`.picker` to be non-`NULL`.

Applying a style (`libs/studio_style.c`) writes straight to the DB through a
throwaway `dt_develop_t` (`common/styles.c`), so it never touches `d->dev`'s
in-memory history or pipeline. `_studio_history_changed_callback` — the
`DT_SIGNAL_DEVELOP_HISTORY_CHANGE` handler raised right after — reloads
`d->dev`'s history from DB and rebuilds its pipeline (`dt_dev_reload_history_items`
+ `dt_dev_history_pixelpipe_update`) before invalidating the fetcher, so the
scopes/color-picker samples (sourced from `d->dev`'s preview pipe) refresh
alongside the center image instead of staying stuck on the pre-style values.
It skips `dt_dev_history_gui_update()`: that call walks `dev->iop` expecting
module GUIs to update, and this style-apply path never needs any (the
scopes/color-picker read the pipe, not module widgets).

That said, "this viewer never initializes module GUIs" is not a hard
guarantee elsewhere: `darktable.develop` is swapped to point at `d->dev`
for as long as Studio Capture is active (see `enter()`/`leave()` below), and
a few global menu actions read `darktable.develop` directly instead of a
locally-scoped `dev` (`dt_apply_dev_history_update()`'s callers in
`gui/actions/edit.c` and `styles.c` — e.g. "Delete history", "Compress
history"). Running one of those while Studio Capture is active *does* call
`dt_dev_history_gui_update()` on `d->dev`, which builds real
`header`/`expander` widgets (`dt_iop_gui_init()` /
`dt_iop_gui_set_expander()`) for its modules, each weak-ref'd back to the
module (`imageop.c`'s `_iop_gui_widget_gone()`). `_studio_dev_teardown()`
therefore now runs `dt_iop_gui_cleanup_module()` on every module before
freeing it, exactly like darkroom's `leave()`, instead of assuming there is
never anything to clean up: skipping it left those widgets outliving
`dt_free(module)`, use-after-freed the next time something (e.g. a
darkroom `enter()`) destroyed them.

## Session resume

At startup, if the previous application session quit while monitoring, Ansel
proposes to resume: accepting switches to this view and starts the session
(see below). Declining clears the resume marker so the question is not asked
again. Stopping the session manually also clears the marker.

## Pending-import prompt

Whenever monitoring starts — a plain **Start**, a folder change, or an
accepted resume — `dt_folder_survey_start()` checks whether the source
folder already holds images the survey does not know about yet, and if so
asks whether to import them right away (with the delete-after-verified-copy
option): a never-before-surveyed folder's existing content (which the first
scan would otherwise silently absorb into the baseline without importing —
see Detection model below), or files that appeared while Ansel was closed
during a resumed session. This one prompt (`_folder_survey_offer_pending_import`)
covers both cases identically; declining absorbs every currently observed
file into the baseline so a later scan does not import it behind the user's
back.

## Folder survey engine

The monitoring itself is a persistent ingest comparison (`folder_survey.c`),
configured entirely through the Import module above; its settings live in the
`studio_capture/*` conf keys. The Import and Style modules stay in sync with
the engine through the `DT_SIGNAL_FOLDER_SURVEY_CHANGED` signal, raised
whenever monitoring starts or stops.

### Detection model

The first scan for a newly configured source folder records all supported
images as the baseline. It does not import those existing files — unless the
pending-import prompt above already seeded an initialized, empty baseline
because the user chose to import them, in which case the scan treats them as
new pending files instead. Later scans recursively compare the current
folder contents with the persisted list stored in `folder-survey-state.ini`
in the user configuration directory.

A new path is not imported immediately. Its size and modification time must
remain unchanged for one complete scan interval. This prevents Ansel from
opening a file while a camera, tethering tool, or synchronization process is
still writing it. The default scan interval is 10 seconds.

Files queued for import have a distinct state so overlapping scans cannot
enqueue them twice. An interrupted queued state is converted back to pending
when Ansel starts again.

### Conflict policy

When copy-on-import produces a destination path that already exists,
`studio_capture/on_conflict` selects the behaviour: **skip** keeps and imports
the existing destination (legacy behaviour), **overwrite** replaces it with
the source, **create unique filename** copies the source under a `_NN`
suffixed name (default — a tethering session's naming patterns commonly have
no variable that changes shot to shot, so skip/overwrite would otherwise
silently drop or clobber every subsequent capture).

### Source deletion

Deleting the source is available only when the import copies the image to
another directory. The source and destination sizes and complete byte streams
are compared after the destination has been imported successfully. The source
is removed only when both files are identical.

The destination base directory cannot be inside the surveyed source
directory. This prevents copied outputs from being detected as new input
files.
