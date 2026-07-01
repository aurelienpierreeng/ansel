# Thumbtable: the filemanager grid and the filmstrip {#thumbtable}

[TOC]

The **thumbtable** is the widget that lays out image thumbnails for the whole application. A single
type, @ref dt_thumbtable_t, backs two visually and behaviourally different frontends:

- the **filemanager** — the multi-column, vertically-scrolling grid of the lighttable view;
- the **filmstrip** — the single-row, horizontally-scrolling strip shared by the darkroom, map and
  print views.

Both are built from the same low-level thumbnail widget (@ref dt_thumbnail_t, `dtgtk/thumbnail.c`)
and share one engine, but their layout and scrolling are genuinely different. This page documents
how the responsibilities are split so the shared parts stay shared and the divergent parts stay
isolated.

## Why the split exists

Historically all of this lived in one `dtgtk/thumbtable.c` with a `mode` field and ~35 inline
`if(mode == FILEMANAGER / FILMSTRIP)` branches. That mode-sharing was a recurring source of bugs:
a change for one consumer silently altered the other, and the two could not express the layout
policies they actually needed. The most visible symptom was
[issue #877](https://github.com/aurelienpierreeng/ansel/issues/877) — at the default filmstrip
height the shared scroll container degenerated and no thumbnail painted until it was hovered.

The code is now an **engine + two frontends** connected by a **layout-ops vtable**. There is no
`if(mode == …)` behaviour branch left; every former branch is a call through `table->ops`.

## File layout

| File | Role |
|------|------|
| `dtgtk/thumbtable.h`          | Public API + the @ref dt_thumbtable_t struct (with an opaque `ops` pointer). This is the only header external callers include. |
| `dtgtk/thumbtable_internal.h` | **Private** interface shared by the three thumbtable translation units: the @ref dt_thumbtable_layout_ops_t vtable typedef, the shared-static prototypes and the `CLAMP_ROW` / `IS_COLLECTION_EDGE` helpers. Not included by callers. |
| `dtgtk/thumbtable.c`          | The **engine**: lifecycle, the collection LUT, the thumbnail hash, populate/resize, image refresh, drag-and-drop, selection, mouse-over dispatch, signal wiring, and the public API entry points that delegate to `ops`. |
| `dtgtk/filemanager.c`         | The **FILEMANAGER** frontend: grid ops + the grid-only public API (`dt_thumbtable_set_zoom` / `dt_thumbtable_get_zoom` / `dt_thumbtable_offset_zoom` / `dt_thumbtable_apply_grid_configuration`). |
| `dtgtk/filmstrip.c`           | The **FILMSTRIP** frontend: strip ops, backed by a GtkLayout scroll implementation. |
| `dtgtk/thumbnail.{c,h}`       | The individual thumbnail widget. Shared, mode-agnostic, unchanged by the split. |

\htmlonly
<pre class="mermaid">
flowchart TD;
  subgraph Callers
    LT[lighttable view]
    DR[darkroom / map / print views]
    WM[window_manager.c]
  end
  subgraph Engine
    E[thumbtable.c<br/>engine + public API]
    S[dt_thumbtable_t<br/>+ ops pointer]
  end
  subgraph Frontends
    FM[filemanager.c<br/>grid ops]
    FS[filmstrip.c<br/>strip ops]
  end
  T[thumbnail.c<br/>dt_thumbnail_t]

  LT --new FILEMANAGER--> E
  DR --new FILMSTRIP--> E
  WM --parent_overlay / grid / scroll_window--> S
  E --owns--> S
  S --ops-.->FM
  S --ops-.->FS
  E --creates / resizes--> T
  FM --GtkFixed--> T
  FS --GtkLayout--> T
</pre>
\endhtmlonly

## The shared handle

The struct @ref dt_thumbtable_t stays in the public header, but every former branch site now reads
`table->ops`, a `const` pointer to the frontend's vtable, bound once at construction. The vtable
type is only *forward-declared* in the public header, so callers keep an intact struct without
seeing the strategy internals.

A handful of struct fields are read directly by external code and are therefore load-bearing public
surface:

- `parent_overlay` — the root widget the views pack into their panels (`window_manager.c`);
- `grid` — the content widget, used for focus grabs (`gui/gtk.c`);
- `scroll_window` — the `GtkScrolledWindow`, used to wire a scroll-event handler
  (`libs/tools/lighttable.c`);
- `thumb_width` / `thumb_height` — the current thumbnail size (`gui/actions/run.c`).

Everything else about a table is manipulated only through the `dt_thumbtable_*` API or, within the
three thumbtable translation units, through the engine internals declared in
`thumbtable_internal.h`.

## The layout-ops vtable

@ref dt_thumbtable_layout_ops_t is the strategy interface. Each frontend exposes one `const`
instance via `dt_thumbtable_grid_ops()` / `dt_thumbtable_filmstrip_ops()`; the engine selects one in
`_ops_for_mode()` (the single mode dispatch point) and stores it on `table->ops`.

| Op | Purpose | Filemanager | Filmstrip |
|----|---------|-------------|-----------|
| `create_content_widget`      | Build `table->grid` | `GtkFixed` | `GtkLayout` |
| `configure_dims`             | Viewport size, per-row count, thumbnail size from the parent allocation | columns from config, vertical scroll | 1 row, height driven by the panel |
| `rowid_to_position` / `position_to_rowid` | Map a collection index ↔ pixel position | row/column grid | linear along X |
| `get_row_ids` / `is_rowid_visible` | Visible collection range at the current scroll step | vertical | horizontal |
| `update_content_size`        | Declare the virtual content extent to the scrollbars | `gtk_widget_set_size_request` | `gtk_layout_set_size` |
| `group_borders`              | Group-membership border flags for a thumbnail | four-neighbour | top+bottom always, left/right |
| `place_child` / `move_child` | Put / move a thumbnail widget | `gtk_fixed_put/move` | `gtk_layout_put/move` |
| `wants_scroll_value` / `wants_page_size_notify` / `relevant_scrollbar_changed` | Scrollbar-event predicates (the engine does the scheduling) | vertical adjustment | horizontal adjustment |
| `is_thumb_highlighted`       | Source of the "selected"-looking highlight | the lighttable selection | the active / developed image(s) |
| `on_thumbnail_added`         | Per-thumb selection/action state when it enters the viewport | selection-driven, actions enabled | active-image-driven, actions disabled |
| `on_drag_begin`              | Commit the hovered image at drag-begin | extend the selection | raise the filmstrip-drag signal |
| `setup_parent`               | Build the overlay scroll stack, name, help link, scroll policy | overlay child, `NEVER`/`ALWAYS` | overlay main child, `ALWAYS`/`EXTERNAL` |
| `grab_focus` *(nullable)*    | Grab keyboard focus for the content widget | grabs focus | — |
| `handle_key` *(nullable)*    | Consume a mode-specific navigation/selection key | Up/Down + space selection | — |
| `pre_activate` *(nullable)*  | Select an image before a Return/activate | select single | — |

Nullable ops are only meaningful in one mode; the engine null-checks before dispatching. All other
ops must be provided by both frontends.

## Data model

The engine keeps a **double reference** to the current collection (see the long comment at the top
of `thumbtable.c`):

- a **LUT** (`table->lut`, an array of @ref dt_thumbtable_cache_t) indexed by *rowid* — the
  position of an image in the collection (equivalently, `rowid - 1` in the SQLite result). This
  gives O(1) position → imgid/thumbnail lookups in C without re-querying SQLite, and is rebuilt only
  when the collection itself changes (filter or sort). It can hold hundreds of thousands of entries.
- a **hash** (`table->list`, keyed by imgid) of the thumbnails that are actually *materialised* —
  i.e. have a widget attached to the content widget.

Only thumbnails inside (or near) the current viewport are materialised; scrolling attaches and
detaches widgets dynamically, because attaching many thousands of children — and especially
detaching them — is prohibitively slow in GTK. Allocation and freeing always go through
`table->list`; the LUT only tracks references into it. `table->lock` guards the dynamically-sized
iterations so background signals and the GUI thread never mutate the lists concurrently.

## Thumbnail highlight

Both modes reuse the thumbnail's "selected" visual (a lighter frame plus a small triangle) but
drive it from **different sources**, expressed by the `is_thumb_highlighted` op:

- the **filemanager** highlights the lighttable **selection**;
- the **filmstrip** highlights the **active / developed** image(s) — the picture currently open in
  the darkroom — via `dt_view_active_images_has_imgid()`.

The engine repaints highlights from this op on *both* `DT_SIGNAL_SELECTION_CHANGED` and
`DT_SIGNAL_ACTIVE_IMAGES_CHANGE` (`_refresh_highlights`), so each mode follows whichever it cares
about and is not clobbered by the other. This matters because opening a new picture in the darkroom
**clears the selection** and **sets a new active image** in the same step: if the filmstrip tracked
the selection it would lose its developed-image marker on every navigation
([issue #954](https://github.com/aurelienpierreeng/ansel/issues/954)).

## Lifecycle

1. `dt_thumbtable_new(mode)` — allocates the table, binds `table->ops = _ops_for_mode(mode)`,
   builds the `GtkScrolledWindow`, creates the content widget through
   `ops->create_content_widget()`, wires all the shared signals to `table->grid`, then calls
   `dt_thumbtable_set_parent()`.
2. `dt_thumbtable_set_parent()` — creates `parent_overlay` and hands the mode-specific overlay/scroll
   assembly to `ops->setup_parent()`.
3. `dt_thumbtable_configure()` — on every size change, `ops->configure_dims()` derives the viewport
   and thumbnail geometry; the engine stores it and, when it changed, calls `_update_grid_area()`
   (→ `ops->update_content_size()`).
4. `dt_thumbtable_update()` — populates/resizes the visible thumbnails (`_populate_thumbnails`,
   `_resize_thumbnails`), placing them through `ops->place_child` / `ops->move_child`.

Layout and scroll updates are coalesced onto idle callbacks (`idle_update_id`, `focus_idle_id`) so
they never run inside draw handlers.

## Content widget and the #877 fix

The single most important difference between the two frontends is the content widget:

- The **filemanager** uses a `GtkFixed`. It pins its width to the viewport (horizontal policy
  `NEVER`) and only scrolls vertically, so the implicit `GtkViewport` that a non-scrollable child
  gets inside a `GtkScrolledWindow` is harmless.
- The **filmstrip** uses a `GtkLayout`. `GtkLayout` implements `GtkScrollable`, so the
  `GtkScrolledWindow` drives it **directly, with no implicit `GtkViewport`**, and its content extent
  is declared explicitly via `gtk_layout_set_size(count * thumb_width, view_height)`.

That viewport was the root cause of #877. With the old `GtkFixed`, the filmstrip's row height plus
horizontal scrollbar very nearly fill the panel; around the default ~120px height, rounding tipped
the content one pixel over the viewport, the scrolled window degenerated, **collapsed the content to
the viewport width and disabled horizontal scrolling**. Every thumbnail positioned past the first
screen was then clipped and never drew (only a hover, which repaints an individual thumbnail window,
revealed them). Because a `GtkLayout`'s extent is declared rather than negotiated from child sizes,
it cannot collapse, and the strip paints at every panel height.

## Rendering and Wayland notes

The filmstrip's `scroll_window` is the **main** child of its `parent_overlay`
(`gtk_container_add`), not an overlay child. On Wayland/Nvidia an overlay child gets its own
offscreen `GdkWindow` that goes stale/blank until a pointer event invalidates it; drawing into the
overlay's own window avoids that. The related fix for the disappearing global menu and the stale
window top border ([issue #888](https://github.com/aurelienpierreeng/ansel/issues/888)) makes the
darkroom center canvas the overlay main child for the same reason
(`dt_ui_init_main_table`, `gui/window_manager.c`).

A `GtkLayout` requests ~0px of its own, so making it the scrolled window's content does not pin the
panel size; the filmstrip panel can still be freely shrunk by its resize handle, and its thumbnail
height is derived from the panel allocation in `configure_dims`.

## Keyboard navigation

Key handling stays in the engine (`dt_thumbtable_key_pressed_grid`) because both modes share
Left/Right/Page/Home/End navigation and Return/activate. The engine first offers the event to
`ops->handle_key` (the filemanager consumes Up/Down row navigation and space selection; the
filmstrip has none), then handles the shared keys itself, delegating the pre-activate selection to
`ops->pre_activate`.

## Extending

- **Add a per-mode behaviour**: add a function pointer to @ref dt_thumbtable_layout_ops_t, implement
  it in both `filemanager.c` and `filmstrip.c`, and replace the engine site with
  `table->ops->your_op(...)`. Keep shared logic in the engine and pass only the divergent part
  through the op.
- **Share an engine helper with a frontend**: give it external linkage and prototype it in
  `thumbtable_internal.h` (the existing examples are `dt_thumbtable_thumb_cell_decoration`,
  `dt_thumbtable_schedule_focus`, `dt_thumbtable_find_rowid_from_imgid`,
  `dt_thumbtable_move_in_grid`).
- **Add a third frontend**: provide a `dt_thumbtable_*_ops()` accessor and a new
  `dt_thumbtable_mode_t` case in `_ops_for_mode()`. No engine branch is needed anywhere else.

## See also

- @ref dt_thumbtable_t, @ref dt_thumbtable_layout_ops_t, @ref dt_thumbnail_t
- `reorganisation.md` — the codebase modularity goals this split follows.
- `resizing-scaling.md` — thumbnail sizing / scaling context.
