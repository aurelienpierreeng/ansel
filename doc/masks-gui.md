<!-- Provenance: every finding carries the commit it was established against. -->

# Masks: the on-canvas GUI and the shape manager

> **Verified against `42eca0e8fe` on 2026-09-29.**
>
> Which gesture does what, where that decision is made, and why each is made in exactly one place.
>
> Each finding below is dated with the commit that established it. A finding is only as
> good as its hash: before acting on one older than the code you are changing, re-measure
> it — and re-date it here when you do. Where an earlier version of a claim was **wrong**,
> that is recorded rather than quietly corrected: how a claim was wrong is usually the more
> useful thing to know.

## The mouse wheel edits the property the user mapped it to; shapes never read modifiers

*Found `0175dac0f2`, 2026-08-28.*

Which property the wheel edits is resolved **once**, by `dt_masks_events_mouse_scrolled()`
(`masks_gui.c`), and handed to the shape through the `interaction` parameter
`dt_masks_functions_t.mouse_scrolled` already carried. Each shape's handler is a `switch` on
`dt_masks_interaction_t`: it acts on the property it is given, ignores one it does not own (a
circle has no rotation), and `DT_MASKS_INTERACTION_UNDEF` means "this combination is unmapped,
do nothing". **No shape may go back to reading `state`** — the key state stays in the signature
only because the callback is shared.

The mapping is one conf key per wheel/modifier combination
(`plugins/darkroom/masks/scroll/{plain,shift,primary,primary_shift}`, enum values `none` |
`size` | `fading` | `opacity` | `rotation`, declared in `data/anselconfig.xml.in`) behind
`dt_masks_scroll_mapping_get/set()` and `dt_masks_scroll_get_interaction()` (`masks_gui.h`).
Those enum values are a storage format: never translate them, never reorder them against
`dt_masks_interaction_t`. The mapping is **application-wide** — which property the wheel edits
is a user habit, not a property of a shape or of the module owning the mask — and its defaults
reproduce the historical modifier behaviour, so a user who never opens the panel sees no change.

A gradient spells one of the shared properties its own way: `FADING` is the curvature. `SIZE`
is the fade extent but keeps the shared name, since it is the shape's size in the only sense a
gradient has one. That is also how the context-menu sliders name them, and why
`dt_masks_interaction_alias_name()` exists — it answers `NULL` for every property a gradient
names like everyone else.

Scope is the selection's business, not the wheel's: `dt_masks_gui_change_affects_selected_node_or_all()`
already restricts a size/fading change to the selected node. A shape must not additionally
*substitute* a property based on selection state — the polygon used to force fading on a plain
wheel whenever a node was selected, which made the mapping unreachable for that shape.

The GUI is the "Mouse wheel" collapsible section of the Drawn tab (`blend_gui.c`), one radio
group per row. It holds no state of its own and re-reads conf on `map`: every module's blending
panel shows the same application-wide mapping, so a panel that becomes visible must display what
is stored, not what it was built with. A display refresh must never write conf back, or a stale
panel becomes the authority.

Consequence for the on-canvas hints (`set_hint_message`, per shape): they document **gestures
only** — drag, ctrl+click, right-click, Del, Enter, Esc — never the wheel, which the panel now
shows. Their branches follow the hit-test order of `dt_masks_find_closest_handle_common()`
(border handle, curve handle, node, segment, then the shape), because the innermost target under
the cursor is what the next click acts on. Two traps when editing them: a node's Del and
ctrl+click only work once that node is *selected* (a mere hover gets the shorter message), and
`dt_hinter_set_message()` joins `\n` into `, `, so each line must read as a clause of one
sentence.

## A right click targets, it never drags — and the context menu leans on that

*Found `15f4e0b9c8`, 2026-09-07.*

`_apply_gui_button_pressed_state()` (`masks_gui.c`) does two things, and they answer to different
buttons. It rebuilds the fine-grained selection from the current hover target — for the LEFT and
the RIGHT button both, so that every `_selected` flag names what the cursor is on — and it then
arms a drag, which is the **left button's alone**: `dt_masks_gui_set_dragging()` sits behind
`if(button != 1) return;` and is that function's only caller anywhere. So no right-button motion
can move a node, a segment, a handle, a shape or a clone source; the per-shape `mouse_moved()`
handlers never read `which` at all, they branch on the `*_dragging` flags only. The one other
place a `*_dragging` flag is written is `polygon.c`'s creation path, itself under `which == 1`.

Rebuilding the selection on both buttons is what keeps ONE state instead of two. Three of those
flags — `form_selected`, `border_selected`, `source_selected` — are written by `update_hover()`
and follow the cursor whatever the button; had the node/segment/handle ones stayed on the left
button, they would still hold wherever the last left click landed, which says nothing about where
the right click that opened a menu went. The context menu is the one consumer reading both
families at once, and is where such a split shows. One user-visible consequence is deliberate: a
right click on a node makes it the selected node, so the wheel then edits that node alone
(`dt_masks_gui_change_affects_selected_node_or_all()`) rather than the whole shape.

The context menu is what makes this load-bearing rather than cosmetic. Its title names
`gui->node_hovered` / `gui->seg_hovered` and its node entries gate on `node_hovered >= 0`, while
polygon's and brush's "Add a node here" gates on `seg_selected` — which says the same thing only
because the right click rebuilt it. Put the rebuild back behind `button != 1` and that entry
silently leaves the menu for any segment the user has not left-clicked first, while the title goes
on announcing the segment; the whole menu then reads as the shape's. Gating it on `seg_hovered`
instead would survive that, since `dt_masks_gui_selected_segment_index()` (`masks_gui.h`) returns
`seg_hovered` outright and consults no selection flag — but two spellings of one question is what
this section exists to prevent, so the flags are kept honest at the source instead.

## A shape toolbar's pressed button is a view on the creation state, not a state of its own

*Found `1083ebf27b`, 2026-09-01.*

Which shape button looks armed is derived, never remembered. `dt_masks_creation_mode_enter()`
(`masks/masks_gui.c`) ends by raising `DT_SIGNAL_MASK_SHAPE_BUTTONS_SYNC`
(`dt_masks_shape_buttons_sync_all()`), and every toolbar built by `dt_masks_shape_buttons_create()`
answers by recomputing each of its buttons from `_masks_shape_button_is_current_creation()` against
`dev->form_gui`. `dt_masks_form_exit_creation()` is the symmetric half and raises
`..._DEACTIVATE`. That is what lets creation be armed from places that own no button at all — the
shape manager's "Add new shape ..." context menu (`libs/shape_manager.c`), the keyboard shortcuts,
`iop/spots.c` — without each of them having to find and press a widget.

So a new way to arm a shape needs no toolbar code, and a toolbar must not track what was clicked.
The buttons act on `button-press-event`, not `toggled`, which is why the sync may set them freely
without re-entering the press handler; and the sync is raised *after* the whole creation state is
written, so a handler that asks is told the truth.

**A toolbar with a NULL `creation_module` is the shape manager's and belongs to no module**, so it
reports any creation whose form is not a retouch/spot one (`DT_MASKS_IS_RETOUCHE`) — those never
appear in its tree. Every other toolbar carries its owner in `creation_module` and lights up only
for that module. The distinction matters because the manager's context menu arms creation with the
*selected group's* module, not with the manager's own NULL: a strict identity test would leave the
button that menu just chose unpressed.

`_masks_shape_button_defs` is the one table naming a shape's icon and its two button tooltips
(`label`, `ctrl_label`, phrased as actions) plus its bare `name`. Menu entries offering a shape are
built from it through `dt_masks_shape_menu_item_new()`, so a menu and the button offering the same
shape cannot drift apart — they did, as "add path" against "add polygon".

## The shape manager's two lists, and the graph questions behind them

*Found `fdcee2ad8d`, 2026-09-04.*

`libs/shape_manager.c` shows the forms in two trees, split by one question: is this group some
module's drawn mask, or nobody's yet? The left list is the inventory — every shape and every
group, module masks included — and the right one is the assignment, holding only the groups a
module actually renders. A module mask is therefore listed in BOTH: once in the inventory, where
it can be picked up like any other group (nested into another mask through the "+", or attached
to more modules through the chooser) and reused, and once in the assignment, under the module that
renders it. A shape a module uses appears in both for the same reason, once as itself and once as
a member — the arrangement the Drawn tab already uses. Membership is derived per rebuild by
`_group_is_module_mask()`, never stored, so a group's assignment-list row appears or disappears the
moment a module claims or releases it — which is why both stores are always rebuilt together.

Every tree handler is handed a `dt_shape_manager_list_t`, not the module, because the first thing
each needs to know is which tree the gesture came from. **That includes the context menu items**:
connecting them with the module instead is what once made every menu action dereference arbitrary
memory.

**A row's module (`TREE_MODULE`) is the module of the mask the row sits in, not the owner of the
group the row names.** It is NULL for a group in the inventory that no module renders, and the
root mask's module for a nested group. A handler that changes a group and must refresh the module
panels showing it asks `_modules_owning_group()` for the modules whose mask *is* that group —
those are the ones whose "N shapes used" moved; every panel's member list is rebuilt by the
signal anyway.

**`DT_SIGNAL_MASK_CHANGED` with `DT_MASKS_EVENT_CHANGE` and ids `(0, 0)` means "rebuild".** The
manager's handler first looks for the row the ids name; `(0, 0)` names none, so its not-found
branch rebuilds on a CHANGE as it does on a deletion. The handlers that raise the signal instead
of broadcasting it (adding an existing shape to a group, renaming, changing a combine operation)
count on that rebuild; the ones that rebuild the tree themselves go through
`_shape_manager_broadcast()`, whose `gui_reset` makes the handler's rebuild a no-op.

**Whether a form can join a group is one question, `_form_can_join_group()`**, asked by a row's
"+", by the "Attach to the group" and "Attach shape ..." menus to grey an entry, and again by each
action, form by form, over the whole selection. A destination that cannot take anything — it
already holds the shape at any depth, or taking a group would close a cycle — is listed greyed,
never left out: the menu is where the user reads that a shape is already there. The groups a menu
offers come by value from `dt_masks_group_list()` (`develop/masks_group.h`), not from a walk over
`dev->forms`, which section 9 of `tools/check_module_boundaries.sh` counts.

A module mask is shown FLAT in the inventory -- one row, no expander, its members not appended
under it -- and expandable in the module list, which is where that subtree belongs. `_tree_row_t`
carries the `flat` flag that stops `_shape_manager_list_recurs()` after the row itself.

Both lists order module masks by **reverse `iop_order`**, walking `dev->iop` from `g_list_last()`
backwards -- the "bottom of the stack first" convention the module chooser and the module groups
panel's Pipeline tab already use, so a mask sits where its module does everywhere else. That walk
is scoped like `_modules_owning_group()`, every module in `dev->iop` rather than only the ones
`dt_iop_module_is_in_pipeline()` shows: the "unclaimed groups" pass afterwards skips anything
`_group_is_module_mask()` claims, so a narrower scope here would drop a hidden instance's mask
from both passes. The order is read at each rebuild, so a reorder must trigger one: the panel
rebuilds on `DT_SIGNAL_DEVELOP_MODULE_MOVED`, which every reorder path raises — drag and drop, an
order preset or reset (all through `dt_iop_gui_commit_iop_order_change()`), history navigation.

**Three GTK behaviours here were measured offscreen, not reasoned about, and each one contradicted
the obvious guess:**

- **Names carry no `ellipsize`, and that is what guarantees they are never compressed.** A
  `GtkTreeView` with no ellipsize reports its true full-content preferred width, that width
  propagates through a `GTK_POLICY_NEVER` scrolled window (a `min-content-width` smaller than the
  content does NOT lower it), and `gtk_window_resize()` -- which restores the persisted panel
  width -- cannot force a window below its content minimum: GTK clamps it back up. So the width
  floor is structural, not something the panel has to compute.
- **A `GtkPaned` with `resize=TRUE` on both children splits new width between them**, drifting the
  divider on every window resize. The inventory is packed `resize=FALSE` so the divider holds and
  the module list absorbs the growth; a manual drag still repositions it.
- **Of two renderers packed with `gtk_tree_view_column_pack_end()`, the FIRST packed lands at the
  true right edge**, each later one closer to the content. The "used by" icon is therefore packed
  BEFORE the note text to end up to its right.

**`_shape_manager_selection_change_in()` must reveal by path, never `gtk_tree_view_expand_all()`.**
The recursive search walks the MODEL, which holds every row whatever the view has collapsed, so it
needs nothing expanded; expanding everything was only about making the match visible afterwards,
and it blew every unrelated group open to do it -- visible as "creating a shape expands all the
groups". `gtk_tree_view_expand_to_path()` on the found row does the same job. The paired
`collapse_all()` on the not-found branch went with it: without an `expand_all()` to undo, it would
have destroyed the user's own expansions on any selection event that missed.

**Outside the darkroom the panel is empty and insensitive.** It is a toplevel that outlives the
view, while what its rows point at does not: the darkroom's `leave()` frees `dev->iop`, and a row
stores its module by address (`TREE_MODULE`). `_shape_manager_recreate_list()` is the one place
that decides it (`_shape_manager_is_active()`: the current view is the darkroom), so no path that
rebuilds the lists — a mask signal, the develop proxy — can refill them from another view. A view
switch reaches it through `DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED`, because a `special` lib is never
handed `view_enter()`/`view_leave()`; that signal is raised after the new view's `enter()`, so the
darkroom's modules are loaded by the time the rows are built. The emptied lists get an empty store,
not a NULL model: the handlers read the model without checking it.

**`dev->form_gui` can be NULL while this panel is open.** It is allocated on entering darkroom and
freed back to NULL on leaving it (`views/darkroom.c`, `views/studio_capture.c`), and this panel is
a standalone toplevel that outlives that. `dt_masks_change_form_gui()` is NULL-safe throughout and
does NOT allocate one, so a caller cannot assume it has one afterwards -- `_tree_selection_change()`
dereferenced it unguarded and crashed (SIGSEGV, observed live).

**Renaming takes a double-click, so the name renderer's `editable` property is NOT bound to the
`TREE_EDITABLE` column.** GtkTreeView has a built-in behaviour: when a cell is editable, a single
click on a row that is already selected opens the text editor. But a single click on a selected
row is also how the user starts a drag, a Ctrl+click or a right-click, so with the property bound,
each of those gestures could open the editor by accident. The property therefore stays FALSE, and
`TREE_EDITABLE` only records whether a row may be renamed at all (top-level rows only).
`_tree_start_name_editing()` is the one place that opens the editor: called on a double-click
(`row-activated` on the name column) and for a freshly created group, it sets `editable` to TRUE,
opens the editor with `gtk_tree_view_set_cursor_on_cell()`, and sets it back to FALSE at once.
Measured offscreen: while the property is FALSE nothing opens the editor, and an editor opened
this way stays open after the property goes back to FALSE and still emits `edited` when validated.

**The panel takes the keyboard focus for exactly as long as a name is being edited.** It is built
with `gtk_window_set_accept_focus(FALSE)` so its drawing tools act on the main window, which
therefore stays the active window — and every key typed "into" the editor went there instead,
through `dt_accels_dispatch()` and the view's `key_pressed()`: a letter fired its shortcut, Escape
left the darkroom for the lighttable. `_tree_name_editing_started()` makes the panel accept the
focus and presents it, and the entry's `editing-done` (Enter, Escape or focus loss alike) turns
that off again and, if the panel still holds the focus, hands it back to the main window. Both
main-window key handlers act only while that window is active, so nothing else needs to know an
edit is in progress; key *releases* landing on it after the hand-back fire nothing.

The graph questions live in `develop/masks_group.h`, id-keyed and by value like the rest of that
header — `dt_masks_group_contains()` (cycle guard: wiring a group into one that already holds it
makes every walk non-terminating), `dt_masks_group_covers_shapes()`, `dt_masks_group_first_use()`.
They walk `->points` where `->points` belongs. Asking them from `libs/` by hand is what
`tools/check_module_boundaries.sh` section 9 counts and refuses.
