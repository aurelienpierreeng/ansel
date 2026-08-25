# Documentation screenshots (`--doc`)

Refreshing the manual's illustrations used to mean taking a full-screen capture and cropping
each widget out of it by hand, once per language. This mode lets the application draw the
widgets itself, straight into the documentation tree, with the language code already in the
file name.

## Using it

```sh
ansel --doc ~/src/ansel-doc
```

The directory is optional and only consumed when it really is a directory, so
`ansel --doc IMG_1234.RAW` still opens the raw file. Without it, the panel opens on the
user's Pictures folder and the destination can be picked in the panel.

`--doc` unlocks one entry at the bottom of the **Run** menu, *Capture widget screenshots…*.
Nothing else changes, and without the flag the entry does not exist.

The panel is a tree going from the whole window down to a single slider:

* the top level lists the entry points a manual actually starts from — the tool modules, the
  darkroom modules, the panels, the window;
* every row unfolds into the real GTK children of its widget, built on demand the first time
  it is opened;
* a row whose widget is not currently displayed is greyed out. Modules belonging to the view
  you are not in stay listed, so the inventory reads the same in lighttable and in darkroom —
  but you have to be in the right view to capture them.

Selecting a row draws it into the **preview** beside the tree, with its pixel size. That is
the reliable way to tell rows apart: names can only go so far on anonymous toolkit plumbing,
and the picture cannot be misread.

Every row that has a widget can be checked, **whether or not it is on screen right now**. A
check says "capture this one", and that intent is worth expressing for a module of the other
view or a collapsed module's body. The selection is remembered, and the capture reports what
was not displayed as *skipped* — so the natural pass is: tick everything, capture, switch
view, capture again.

Check the rows you want, then **Capture**.

### Refresh keeps your place

**Refresh** rebuilds every row — that is what it is for, after loading an image or switching
view — but it does not fold the tree back to its roots. What was unfolded is unfolded again,
the cursor returns to the same row, and the preview comes back with it, so the loop "change
something in the application, refresh, look at the same widget again" actually works.

Rows are matched back by the chain of labels from the root down to them, not by tree position:
a rebuilt model makes a saved `GtkTreePath` meaningless, while the labels are derived from the
widgets themselves. A chain that no longer resolves — its module unloaded, its widget moved —
is quietly dropped. This lives in memory only: closing the panel forgets it, while the checked
rows persist in conf.

### How rows are named

In order of preference: the name the application knows the widget by (a module, a panel), its
own text, its CSS name from `ansel.css`, and failing all that its type with the first text
found underneath — `GtkBox: Add to library` rather than a twentieth `GtkBox`. A container with
no text at all falls back to its rank among its siblings, `GtkBox #3`.

Chains of pure layout — `GtkScrolledWindow > GtkViewport > GtkBox` — are folded into one row.
Each link covers the same pixels as the next, so one row per link would be three rows standing
for a single picture. The row captures the outermost of the chain (padding included) and takes
its name and its children from the innermost. Only boxes, grids, scrolled windows, viewports
and overlays are folded away: a `GtkButton` also holds a single child, and folding it would
delete the very row someone wants.

## The widget-to-page map

Put a file named `screenshots.map` at the root of the destination folder:

```
# <row label as the tree shows it> = <path of the illustration, relative to this file>
Exposure          = content/modules/exposure/exposure.jpg
Tone equalizer    = content/modules/tone-equalizer/tone-equalizer.jpg
Left panel        = content/interface/left-panel.png
Main window       = content/interface/overview.jpg
```

The value carries the documentation's **own** extension, and that is the point: a tree that
ships `.jpg` illustrations has to be refreshable in place — a `.png` dropped beside them
would not replace them. PNG is written by cairo (lossless, which is what text-heavy UI
captures want); anything else goes through gdk-pixbuf, JPEG at quality 95.

The panel shows the mapped page next to each row, and **Select mapped** checks exactly the
rows the map names — that is the documentation pass. Missing intermediate directories are
created at capture time.

Rows with no mapping are still capturable: they land at the root of the destination as
`<row label>.<lang>.png`, with repeats numbered so two anonymous `GtkBox` rows do not
overwrite each other.

## One language at a time

The language code comes from the GUI's own setting (`dt_l10n_get_current_code()`), not from
the system locale, and is inserted before the extension:
`content/modules/exposure/exposure.fr.jpg`. English writes `en`, not the `C` the untranslated
locale calls itself.

A capture that cannot differ between languages does **not** get the code: an icon button, a
colour swatch, a separator renders the same picture everywhere, and `toolbar-zoom.png` is
written once instead of being scattered as identical copies through the tree. The test is
conservative — anything that owns text, plus every custom drawing surface that paints its own
without telling us (bauhaus controls included, whose numeric value carries a localised decimal
separator), counts as localised. Guessing the other way is the expensive mistake: every
language would write the same file and only the last one captured would survive.

So a full refresh is one pass per language: set the language in preferences, restart, open the
panel, *Capture*. The selection is remembered, so the second language captures exactly what the
first one did.

## The remembered selection

Checked rows are written to the conf key `doc_screenshot/selection` (labels joined by `\x1f`,
the same convention as `studio_capture/styles`) on every change — a toggle, a select button —
rather than at capture time, so a session that ends without capturing still leaves the work
behind. It is not a confgen key and does not appear in Preferences: it is developer-mode state,
meaningless to anyone who never passed `--doc`.

Rows are remembered by label, the same key the map uses. Two rows sharing a label — two
anonymous `GtkBox` rows, say — come back checked together; the rows that matter to a
documentation pass are the mapped ones, whose labels are unique by construction.

**The selection spans views on purpose.** *Select all* and *Select mapped* leave rows they
cannot see alone, because a pass captures the lighttable modules from lighttable and the
darkroom ones from darkroom, and the second half must not drop what the first half picked.
*Select none* is the eraser, and the only thing that forgets off-screen rows.

A checked row whose widget is not on screen is reported as **skipped**, not failed — it is
pending, not broken. `%d written, %d skipped, %d failed` after each capture tells you what is
left to do: switch view, capture again.

## What it does not do

* **No batch mode.** Capture needs the widget to be mapped on screen, and a module from
  another view is not. Automating the whole documentation in one run would mean driving the
  view changes too; today the operator does that, guided by the skipped count.
* **No screen grab.** Widgets are re-drawn offscreen with `gtk_widget_draw()`, so the capture
  panel sitting on top of the application does not appear in the result. It also means a
  widget is captured at its current allocation — resize the window before capturing if the
  documentation wants a particular width.
* **No `gtk_widget_show_all()` before capture.** It is the obvious way to make a hidden
  widget measurable, and it is wrong here: every target is live inside the main window, and
  showing it recursively would reveal every child the application deliberately hides
  (collapsed module bodies, conditional buttons, mask indicators) and would not undo itself
  afterwards. Unmapped widgets are refused instead.

## Where it lives

`src/gui/actions/doc_screenshot.{h,c}` — the module owns the flag, the directory and the
panel; `src/darktable.c` only announces the command line, and `src/gui/actions/run.c` asks
whether to offer the menu entry. Nothing else in the application knows the mode exists.
