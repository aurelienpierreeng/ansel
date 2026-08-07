# `src/gui/` — everything the user sees

Layer **4**. GUI toolkit code is *infrastructure used by modules*, so it sits below
`develop/`, `iop/`, `libs/` and `views/` — an IOP's `gui_init()` legitimately calls into
here. It sits above `common/`, `pixel/` and `control/`.

## How files are sorted

**Pure GUI → the root of `src/gui/`.**
A widget, a dialog, a panel — anything whose whole job is display and interaction. That a
file once lived in `src/common/` is historical accident, not structure worth preserving:
`lut_viewer.c`, `import.c` and `privacy_consent.c` are all dialogs that happened to be filed
as backend.

**A backend that also needs a frontend → `src/gui/<subsystem>/<name>_gui.{c,h}`.**
Mirrors `src/<subsystem>/<name>.{c,h}`. Use this *only* when a submodule genuinely has both
halves — currently `history_merge`, `styles` and `film`.

| backend | frontend |
|---|---|
| `common/history_merge.c` | `gui/common/history_merge_gui.c` |
| `common/styles.c` | `gui/common/styles_gui.c` |
| `common/film.c` | `gui/common/film_gui.c` |

**Widget toolkits keep their own directory**: `gui/dtgtk/` (Ansel's widgets) and
`gui/bauhaus.{c,h}` (the slider/combobox toolkit — two files, so no directory).

## The `_gui` suffix is mandatory, and load-bearing

A quoted `#include` searches the **including file's own directory first**. So for any file
under `src/gui/`, the spelling

```c
#include "common/styles.h"
```

would resolve to `src/gui/common/styles.h` if such a file existed — shadowing the real
backend header. The failure surfaces as implicit-declaration errors in an *unrelated* file,
naming symbols nobody touched.

47 files under `src/gui/` include a `common/` header, so this is a property of the layout,
not an accident. **Invariant: no basename may appear in both `src/common/` and
`src/gui/common/`.** The `_gui` suffix is what guarantees it.

## Backends must not call into here

`common/` is layer 1; reaching up into layer 4 is an inversion. Two mechanisms replace it,
and which one applies depends on **who starts**:

* **The GUI starts** → relocate the function here, or pass what it needs as a parameter.
  `dt_colorspaces_set_display_profile()` takes the `GtkWidget *` rather than calling
  `dt_gui_center_widget()` itself.
* **The backend starts** → the backend declares a handler type and owns the slot; the GUI
  registers itself in `dt_gui_gtk_init()`. See `common/thumbnail_notify.h`,
  `common/startup_progress.h`, and `dt_film_set_confirm_rmdir_handler()`.

Note that routing through `control/`'s signal bus does **not** fix an inversion — it trades
`common → gui` for `common → control`, which is an inversion of its own.

An unregistered handler is a no-op, which is why backends need no "is there a GUI?" test.
