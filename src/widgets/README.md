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
  *except* pure macro headers that carry no state (`system/macros.h` for `IS_NULL_PTR`),
  which sit at a lower layer and are legitimate to depend on.

**Every file here satisfies the first three — verified, zero violations.** Configuration
arrives through setters (`dt_widget_set_min_panel_width()`), shared toolkit state lives in
`widget_settings.h`, and behaviour that needs the application is announced as a signal for
the caller to act on (`resetlabel` emits `"reset"`; `develop/imageop_gui.c` attaches the
IOP meaning).

### Independence from `gui/`: achieved

No file here includes anything from `gui/`. What each of them needed came out with it:

| was | now |
|---|---|
| `dt_gui_add_class` / `remove_class` (gui/gtk.c) | `widget_style.{c,h}` |
| widget-freeze depth + `dt_gui_widgets_suppressed` | `widget_settings` — the counter left `dt_gui_gtk_t` |
| `dt_gui_get_scroll_unit_delta(s)` | `widget_settings` |
| `dt_draw_star`, `dt_draw_line`, `set_color` | `draw.h` |
| bauhaus colour-label palette | `dt_widget_colorlabel()` + `DT_WIDGET_COLORLABEL_*` |
| `gui/gdkkeys.h` (pure keysym mapping) | `widgets/gdkkeys.h` |
| pipeline-tracked allocator in `focus_peaking` | plain `dt_alloc_align` |

`gui/application.h` includes `widget_settings.h` and `widget_style.h`, so the 45 files that used those
names keep compiling unchanged.

**What the application registers at startup**, in `dt_gui_gtk_init()` unless noted:

| call | why |
|---|---|
| `dt_widget_set_gui_thread()` | freezing is a no-op off the GUI thread, and inert until registered |
| `dt_widget_set_scroll_reversed()` | a user preference; a widget does not read conf |
| `dt_widget_set_dpi_factor/_ppd/_em_size()` | screen and theme metrics, pushed as they are resolved |
| `dt_accels_set_global()` | the shortcut system's instance |
| `dt_accels_set_top_offset_handler()` | where the host wants the shortcut window placed |
| `dt_accels_set_refocus_handler()` | return focus to the host's main area |
| `dt_accels_set_recent_handlers()` | persistence for the shortcut-search history |
| `dt_widget_set_root_window_handler()` | a window for things that exist before any widget does |
| `dt_widget_set_natural_width_handler()` | how wide the host's panel wants a widget |
| `dt_widget_set_cursor_handler()` | pointer shape during a drag |
| `dt_widget_set_message_handler()` | transient user-facing messages |

Unregistered, each degrades to an inert default — which is what makes headless runs work
without a single "is there a GUI?" test.

**The colour-label indices are pinned.** `widgets/` declares its own so it needs no
application header; `gui/gtk.c` carries a `_Static_assert` tying them to `dt_colorlabels_enum`,
because that is the only place both are visible. They cannot drift silently.

### `gui/bauhaus.h` is gone from here too

`paint.c` drew colour labels from `dt_bauhaus_get_global()->colorlabels[]`. The palette is now
`dt_widget_colorlabel()`, so `bauhaus` could itself become a widget rather than a dependency
of one.

### What is still depended on, and legitimately

Downward includes only: `system/` (allocation, SIMD), `math/`, `system/macros.h` for
`IS_NULL_PTR`, and — in `focus_peaking.c` alone — `pixel/eigf.h` for the guided filter. All
sit below layer 4. That last one means `focus_peaking.c` is not portable to another
application as-is, unlike the rest.

| file | what it is |
|---|---|
| `button.c`, `togglebutton.c` | buttons that paint themselves with a `paint.h` callback |
| `icon.c`, `icon_cell_renderer.c` | icon widget and its `GtkCellRenderer` |
| `paint.c` | ~3800 lines of cairo icon-drawing primitives |
| `drawingarea.c` | aspect-ratio-preserving drawing area |
| `expander.c` | the collapsible section header |
| `thumbnail_btn.c` | the small overlay button used on thumbnails |
| `gradientslider.c` | the gradient slider |
| `sidepanel.c` | the collapsible side panel |
| `focus_peaking.c` | focus-peaking overlay rendering |
| `resetlabel.c` | a label emitting `"reset"` on double-click |
| `accelerators.{c,h}` | the whole keyboard-shortcut system (3224 lines) |
| `bauhaus.{c,h}` | the slider/combobox toolkit (3982 lines) |
| `resize_handle.{c,h}` | the drag grip that resizes a panel, the scope, the filmstrip |
| `gtkentry.c` | `GtkEntry` completion helper |
| `gdkkeys.h` | keysym mapping (numpad/main-pad equivalence) |
| `widget_settings.{c,h}` | toolkit state: scroll, DPI/em metrics, freeze depth, palette |
| `widget_style.{c,h}` | CSS class helpers, label capitalisation |
| `draw.h` | `dt_draw_star`, `dt_draw_line`, `set_color` |

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
