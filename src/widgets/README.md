# `src/widgets/` — reusable GTK widgets

Custom GTK widgets and drawing primitives that **know nothing about Ansel**. Drop any file
here into another GTK application and it would compile.

Layer **4**, alongside `gui/`. Depends on GTK, cairo and glib — and on nothing else in this
repository.

## The rule that defines this directory

A file belongs here only if it has **no application state**:

* no `darktable.*` globals,
* no `dt_*_get_global()` accessor of any kind,
* no `dt_conf_*` — a widget is configured by its caller, not by reading preferences,
* no `#include` from `common/`, `develop/`, `control/`, `views/`, `libs/` or `imageio/`.

Everything currently here satisfies all four, verified rather than assumed. **If a change
would introduce any of them, the file does not belong here** — either pass the value in as a
parameter, or the widget is really an application component and belongs in `gui/`.

| file | what it is |
|---|---|
| `button.c`, `togglebutton.c` | buttons that paint themselves with a `paint.h` callback |
| `icon.c`, `icon_cell_renderer.c` | icon widget and its `GtkCellRenderer` |
| `paint.c` | ~3800 lines of cairo icon-drawing primitives |
| `drawingarea.c` | aspect-ratio-preserving drawing area |
| `expander.c` | the collapsible section header |
| `thumbnail_btn.c` | the small overlay button used on thumbnails |

## What stayed behind in `gui/dtgtk/`, and why

`dtgtk/` was never one thing. These are application components that happen to be widgets,
and each fails the rule above for a concrete reason:

| file | what blocks it |
|---|---|
| `thumbtable.c` | `dt_collection_get_global`, `dt_image_cache_get_global`, `dt_gui_get_global`, `dt_conf_*` |
| `thumbnail.c` | `dt_control_get_global`, `dt_database_get*` |
| `preview_window.c`, `thumbtable_info.c` | image cache, database, conf |
| `filemanager.c`, `filmstrip.c` | include `views/`, `control/` |
| `gradientslider.c`, `focus_peaking.c` | `dt_gui_get_global` |
| `resetlabel.c` | `dt_dev_get_global` |
| `sidepanel.c` | `dt_conf_*` |

The last four need one small decoupling each and would then qualify. The thumbnail/thumbtable
family is genuinely application code and should not move.

**`gui/bauhaus.c` is not a reusable widget today** despite looking like one: 3981 lines
reaching into `develop/`, `control/`, `dt_conf_*` and `dt_dev_get_global`. Making it one is a
real piece of work, not a move.
