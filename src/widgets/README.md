# `src/widgets/` — reusable GTK widgets

Custom GTK widgets and drawing primitives that **know nothing about Ansel**. Drop any file
here into another GTK application and it would compile.

Layer **4**, alongside `gui/`. Depends on GTK, cairo and glib — and on nothing else in this
repository.

## The rule that defines this directory

A file belongs here only if it carries **no application state**:

* no `darktable.*` globals,
* no `dt_*_get_global()` accessor of any kind,
* no `dt_conf_*` — a widget is configured by its caller, not by reading preferences,
* no `#include` from `common/`, `develop/`, `control/`, `views/`, `libs/` or `imageio/`
  *except* pure macro headers that carry no state (`common/macros.h` for `IS_NULL_PTR`),
  which sit at a lower layer and are legitimate to depend on.

**Every file here satisfies the first three — verified, zero violations.** Configuration
arrives through setters (`dtgtk_side_panel_set_min_width()`), shared toolkit state lives in
`widget_settings.h`, and behaviour that needs the application is announced as a signal for
the caller to act on (`resetlabel` emits `"reset"`; `develop/imageop_gui.c` attaches the
IOP meaning).

### Not yet true: independence from `gui/`

Seven files still include `gui/gtk.h`, `gui/bauhaus.h`, `gui/draw.h` or `gui/gdkkeys.h`.
`gui/` is the *same* layer, so this is not a layering violation, but it does mean these
files are not yet droppable into another GTK application. The dependency is small and
entirely toolkit-level — measured, not estimated:

| symbol | uses | where it should end up |
|---|---:|---|
| `dt_gui_add_class` | 7 | a CSS helper — belongs in `widgets/` |
| `dt_bauhaus_get_global` | 7 | `paint.c` reads bauhaus colours; needs bauhaus reworked first |
| `dt_draw_star` | 3 | a drawing primitive — belongs in `widgets/` |
| `dt_gui_widgets_suppressed`, `dt_gui_get_scroll_unit_delta` | 3 | toolkit helpers — `widget_settings` |
| `dt_pixelpipe_cache_alloc_*` | 3 | `focus_peaking.c` buffer allocation — should take a caller-provided buffer |

Until those move, treat "reusable" as *"holds no application state"*, which is true and
enforced, rather than *"compiles standalone"*, which is not yet.

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
