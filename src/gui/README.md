# `src/gui/` — everything the user sees

Layer **4**. GUI toolkit code is *infrastructure used by modules*, so it sits below
`develop/`, `iop/`, `libs/` and `views/` — an IOP's `gui_init()` legitimately calls into
here. It sits above `common/`, `colorprofiles/`, `math/`, `pixel/` and `control/`, and
alongside `widgets/`.

## How files are sorted

**Pure GUI → the root of `src/gui/`.**
A widget, a dialog, a panel — anything whose whole job is display and interaction. That a
file once lived in `src/common/` is historical accident, not structure worth preserving:
`lut_viewer.c`, `import.c` and `privacy_consent.c` are all dialogs that happened to be filed
as backend.

**A backend that also needs a frontend → `src/gui/<subsystem>/<name>_gui.{c,h}`.**
Mirrors `src/<subsystem>/<name>.{c,h}` — the subsystem directory is part of the mirror, so a
`develop/` backend gets a `gui/develop/` frontend, not a `gui/common/` one. Use this *only*
when a submodule genuinely has both halves.

| backend | frontend |
|---|---|
| `common/collection.c` | `gui/common/collection_gui.c` |
| `common/database.c` | `gui/common/database_gui.c` |
| `common/film.c` | `gui/common/film_gui.c` |
| `common/folder_survey.c` | `gui/common/folder_survey_gui.c` |
| `common/history_actions.c` | `gui/common/history_actions_gui.c` |
| `common/styles.c` | `gui/common/styles_gui.c` |
| `develop/history_merge.c` | `gui/develop/history_merge_gui.c` |

**Widgets that carry application state → `gui/dtgtk/`**: the thumbnail/thumbtable family,
the filmstrip, the file manager and the preview window. Each reaches for the collection, the
image cache, the database or `dt_conf_*`, which is what keeps it here. A widget that needs
none of that is toolkit, not application code, and belongs in `src/widgets/` (bauhaus, the
accelerators, the cairo icon primitives) — see `src/widgets/README.md` for the rule that
separates the two.

**The global menu keeps its own directory**: `gui/actions/`, one file per top-level menu
plus `menu.{c,h}`, the machinery each of them registers entries into.

## The `_gui` suffix is mandatory, and load-bearing

A quoted `#include` searches the **including file's own directory first**. So for any file
under `src/gui/`, the spelling

```c
#include "common/styles.h"
```

would resolve to `src/gui/common/styles.h` if such a file existed — shadowing the real
backend header. The failure surfaces as implicit-declaration errors in an *unrelated* file,
naming symbols nobody touched.

41 files under `src/gui/` include a `common/` header and 20 include a `develop/` one, so
this is a property of the layout, not an accident. **Invariant: no basename may appear in
both `src/<subsystem>/` and `src/gui/<subsystem>/`.** The `_gui` suffix is what guarantees
it.

## Backends must not call into here

`common/`, `colorprofiles/` and `math/` are layer 1; reaching up into layer 4 is an
inversion. Two mechanisms replace it, and which one applies depends on **who starts**:

* **The GUI starts** → relocate the function here, or pass what it needs as a parameter.
  `colorprofiles/`'s `dt_colorspaces_set_display_profile()` takes the `GtkWidget *` whose
  monitor it should read, rather than calling `dt_gui_center_widget()` itself.
* **The backend starts** → the backend declares a handler type and owns the slot; the GUI
  registers itself in `dt_gui_gtk_init()`. See `common/thumbnail_notify.h`,
  `common/startup_progress.h`, `dt_film_set_confirm_rmdir_handler()` and
  `dt_colorspaces_set_profile_changed_handler()`.

Note that routing through `control/`'s signal bus does **not** fix an inversion — it trades
`common → gui` for `common → control`, which is an inversion of its own.

An unregistered handler is a no-op, which is why backends need no "is there a GUI?" test.
