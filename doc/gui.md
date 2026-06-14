# Ansel GUI

## Foreword and general concepts

### Color perception

Ansel is a workflow app focused on image content. Our perception of image color is biased by the surround color, brightness, contrast and details. In Ansel, the surround is all the GUI controls around the image. Text is secondary and should not hinder color perception of the image content, but it will because it's there. 

There is a belief that, to make color appearance consistent across devices, you just have to use ICC profiles to normalize all devices to the same ground truth, that is correct their individual color deviation.[^1] The fact is vewing conditions matter just as well, which was ground for the ISO 12646 standardized viewing conditions for proofing prints.

[^1]: Note that, when correcting color deviations of display devices, you also shrink their gamut coverage, so this has a cost. Also, end-users are rarely qualify to assess the _quality_ of their custom ICC profiles, so they have no idea whether they actually make things better or worse by calibrating/profiling their screen themselves.

For these reasons, image editing should happen in viewing conditions that are as close as possible from the final product (typically: the print) viewing conditions. We don't look at (still) pictures in movie theaters, but typically printed and hanged on a wall. In a such environnement, the surround lightness is anywhere between 50% and 80%, and the typical contrast ratio of a print is (much) less than 200:1.[^2] dark GUI are a no-go because images viewed against a dark background:

[^2]: Contrast ratio is defined as the luminance of the brightest spot in the image (naked paper), divided by the luminance of the darkest spot (satured ink).

1. will appear less saturated than viewed against average background (Hunt effect). This will lead users to over-saturate their pictures, which might cause gamut issues when printing.
2. will appear brighter and more contrasted than viewed against average background (Bartleson-Breneman effect). This will lead users to under-expose their pictures at editing time, and then blame the darkness of their prints on the printer driver or ICC profile.

The background color of Ansel default theme is therefore middle-grey ± 5%, to provide a neutral and illusion-free color assessment environment, as to minimize bad surprises when printing. But that comes at a price:

1. forget the sexy dark UI that try to sell you "professional",
2. text contrast is at most 50 % (difference between background and text lightness in CIE Lab 1976), which guys above 50 often find not contrasted enough to read.

Designing an image-processing GUI is therefore a difficult balance to find between limiting opportunities of color-perception illusions, while ensuring text and controls remain legible enough. The trade-off will be different depending on the age/eyesight of the user and whether the user has a color-blindness.

### Screen resolution, UI size

A lot of Ansel design, UI and pipeline alike, accounts for the most demanding usecase: mobile usage. That can be digital nomads or photo-reporters, working from their car on in the train, with a laptop litteraly on their lap, running on battery. 

Ansel GUI should fit a 14 inches display with some trade-offs, and should comfortably fit a 15.6 inches screen. Anything larger is considered bonus, but the design doesn't take it for granted. Anything smaller than 14 inches unfortunately becomes very hard to support.

Screen resolution is not a design given, because a 4K display fitting a 15.6 inches screen will most likely need an hiDPI scaling factor of 2, which makes it an actual 1080p display. Final legibility of controls cares about their physical size once displayed on screen, which is a product of display pixel density, hiDPI scaling factor, and initial pixel size of the UI.

## GUI semantics

The initial set of GUI semantics chosen was Google Material Design, because:

1. it provided a readily-available and explicit set of meaning <-> visuals mappings,
2. it relies on hardware/physical metaphors that are globally understood,
3. Android devices cover more than half the smartphone markets, so users are already used to Material Design.

The problem of Material Design is, it fits the requirements of applications meant for content _consumption_ rather than content _production_:

1. margins are too big, spacing too wide, so the density of on-screen controls is too low and you need to scroll a lot within viewport to access all controls,
2. controls are "too much there", especially buttons: they take too much focus, they are highlighted too much, and so you get too many flashy things in your visual field competing for attention.

From Material Design, Ansel GUI semantics have evolved in the direction taken by Visual Studio Code, which is a great example of a _production_ application that manages to stay legible while having an high density of controls.

So here are the guidelines, requirements and choices made in Ansel GUI.

### Static text

Static text is labels or information that cannot be interacted with. It can dynamically be updated upon runtime changes.

- regular font: `font-weight: 400`, `font-stretch: normal`, `font-family: roman`,
- colored white,
- doesn't react on hover event.

### Variable values

Variable values are text, checkboxes, sliders that represent the state of image-processing user parameters. They are attached to a widget that can record user events to change the values (checkbox button, combobox/slider, etc.)

- regular font: `font-weight: 400`, `font-stretch: normal`, `font-family: roman`,
- colored orange,
- react on hover event like their parent: `font-height: 500`, brighter color if possible,
- react on focus event like their parent: see _focused widget_.

### Focused widget

A widget is focused when it is the one currently capturing user input. In GTK, that means keyboard strokes, but Ansel extends that to scroll events too. Only one widget can be focused at any given time, so attributing the focus to a new widget removes it from the previous. 

Focus is attributed to widgets on click, through actions, or by cycling through focusable widgets using `Tab`. Once one widget is focused, focusing the next/previous is natively handled by Gtk through arrow keys. To handle the case where no widget is focused, Ansel adds a `Ctrl+Arrow` shortcut that focuses the first widget of the first module on key `Down` or the last widget of the last module on key `Up`, then behaves like GTK native arrow shortcuts.

- bold font: `font-weight: 700`, `font-stretch: normal`, `font-family: roman`,
- colored brighter if possible (note that using bold font already increases the percieved brightness).

### Actionnable text

Actionnable text are typically text buttons, but also comboboxes. Material Design would recommend to give them a background or border to signify that they are not merely information. While it is a good idea to advertise that "this is something you can act on", Ansel GUI would quickly degenerates into a mosaic of buttons, which is not possible.

- Comboboxes already have an arrow that suggests interactivity, so they get no additional background/border,
- Text buttons get a discrete border, to set them aside of static text.

- regular font: `font-weight: 400`, `font-stretch: normal`, `font-family: roman`,
- react on hover event with a brighter background, and a text color adjusted for legibility against the brighter background,
- react on focus: see _focused widget_.

### Icon buttons

Icons are a great way to reduce the GUI footprint when they can convey meaning using universal symbols that are globally recognised. Problem is there are few of them in an imaging app. A rule of thumb is: if you don't find the icon you are thinking of in a general-purpose icon library, don't. Use text. There is no point in using an icon that needs 3 lines of tooltip to explain.

- icon buttons use no border and no margin,
- icon buttons should be square and have the size of a line of text. When inserted into a text line, they should not stretch it: `min-height: 1em` and `min-width: 1em`,
- icon drawings may need to be streched depending on their content and visual reach in order to _perceptually_ match the text size (align on optical margins),
- react on hover event: same as _actionnable text_,
- don't react on focus,
- toggle buttons: toggled state is materialized with either a brighter background (adjust color for legibility), or a different icon, or possibly both.

### Collapsible sections

When a container owns some children widgets that are not always used, or match a niche usage, you may tidy up the UI by "hiding" them into a collapsible section. However, when you end up nesting collapsible sections one more than one level, you might consider what you are doing carefully, and especially consider using:

1. a popup window, tied to a button,
2. a context menu, bounded to right click. 

In practice, collapsible sections should be considered a viable option only for _deliberately hiding_ controls that are discouraged to use or deprecated. Advanced controls and niche features should preferably go into tab. The exception to this are the color matching features of _color calibration_ (channel mixer RGB) and _exposure_ modules. Using tabs in exposure would be overkill and, in color calibration, you need the color-matching features visible at the same time as the notebook controls they set, but not always. But they could definitely go into a popup window.

## Modules vs. Toolboxes

Modules are image-processing artifacts binding pixel filters (aka pipeline nodes) and GUI widgets together. They can be reorded, and reordering them into the GUI also reorders their corresponding node in the pipeline. They can be turned ON/OFF, which also disables/enables their node in pipeline. They are stacked bottom to top in a layer logic. They live in the right sidebar of the darkroom view.

Toolboxes are arbitrary collections of widgets that can be collapsed or uncollapsed, displayed in arbitrary order. Toolboxes use to be named as modules too, so we had 2 different kinds of modules, which was difficult to explain to users and confusing anyway. 

Now:

- toolboxes are square and take the whole sidebar space (no margins),
- modules are rounded and are recessed into the sidebar (margins), as to suggest they are nodes that can move within the container.

## GUI requirements

1. Modules should fit vertically entirely within the viewport of a 15.6 inch screen using 16px font size at 1920x1080 px.
2. Between 2 design variants, the best one is the one:
    - that requires the fewest interactions (clicks, scroll steps),
    - that requires the smallest displacement (moving cursor),
3. Controls should be fully accessible from keyboard alone and fully accessible from mouse/pointing devices alone, but never require a mix of both. Combinations of keyboard and mouse events should only be shortcuts alternatives to faster access features that are already fully accessible from one or the other entirely.[^3]

[^3]: There are still many parts in the GUI where buttons require Ctrl+click to trigger special behaviours that cannot be accessed otherwise. These should be fixed.

## Implementation

### Pixel scaling

GTK3 exposes two *independent* scaling mechanisms, and the codebase has to keep them straight:

- **Integer scale-factor** (`gtk_widget_get_scale_factor()`, cached as `darktable.gui->ppd`): the HiDPI factor (1–4) reported by the compositor (Wayland) or the macOS backing scale. GTK applies it *automatically* at render time to every logical-px sink — CSS `px`, widget size requests, box spacing, the window default size — and we apply it to raw cairo buffers via `cairo_surface_set_device_scale()`.
- **Font/screen DPI** (`gdk_screen_get_resolution()` / 96, cached as `darktable.gui->dpi_factor`): the classic X11 `Xft.dpi` knob. On plain X11 the scale-factor stays at 1, so this is the *only* HiDPI lever available there; it scales point-sized fonts but, by GTK design, it does **not** touch CSS `px`.

Because the right factor depends on the **destination sink** (not on the platform), pixel sizes go through one of two intent-named macros in `src/gui/gtk.h`. Their inputs are device-independent px at the 96 DPI baseline:

- `DT_UI_SCALE_UI(px)` — for logical-px GUI sinks (`gtk_widget_set_size_request()`, window geometry, anything fed to a GTK widget geometry setter). GTK already multiplies these by `ppd`, so this macro must **not** pre-apply `ppd`; it only adds the `dpi_factor` UI zoom.
- `DT_UI_SCALE_DEVICE(px)` — for raw device-pixel buffers (cairo image surfaces, `gdk_pixbuf_*_at_size`, mouse hit-test radii). The toolkit does not auto-scale these, so the macro carries both `dpi_factor` and `ppd`.

`DT_PIXEL_APPLY_DPI()` / `DT_PIXEL_APPLY_DPI_DPP()` remain as deprecated aliases of `DT_UI_SCALE_UI` / `DT_UI_SCALE_DEVICE` so existing call sites keep compiling; prefer the intent-named macros in new code.

**Raw `darktable.gui->ppd` is *not* automatically a smell.** It legitimately appears in two situations that are **not** "scale a design constant" and must stay raw:

- **Coordinate-space transforms** — converting between image space and screen space (e.g. `dt_dev_get_zoom_level(dev) / ppd`, `roi.scaling / ppd`, mipmap buffer sizing `thumb_width * ppd`, `cairo_translate(... * ppd)`). These are space conversions, not the scaling of a fixed pixel size; wrapping them in a scaling macro would be meaningless.
- **Self-managed device buffers** — code that sizes a cairo buffer in device pixels itself and publishes `cairo_surface_set_device_scale(s, ppd, ppd)` (see "Cairo image surfaces" above).

Only when you take a fixed device-independent pixel constant and need it in raw device pixels (the mouse hit-test radius is the canonical example) should you use `DT_UI_SCALE_DEVICE()` rather than hand-writing `× dpi_factor × ppd`.

Likewise, **`darktable.gui->dpi`** (the raw screen DPI) is used correctly only for its own configuration, the `em` computation, and setting the resolution of cairo-drawn text — for the latter, use `dt_gui_set_pango_resolution(layout)` instead of hand-writing `pango_cairo_context_set_resolution(..., darktable.gui->dpi)`, so the screen-DPI dependency lives in one place.

> **Direction of travel.** `dpi_factor` is essentially the X11-era manual-HiDPI trick; on Wayland, macOS, GTK4 and Qt the toolkit owns physical scaling through the single scale-factor. The long-term plan is to demote `dpi_factor` to a pure text/UI-zoom factor routed through the toolkit's font scaling, after which logical-px sinks need no macro at all and only `DT_UI_SCALE_DEVICE` (`= ×ppd`) survives as the single entry point.

#### Testing HiDPI on a 1× machine

Most pixel-arithmetic bugs come from developers working on `ppd == 1` / `dpi_factor == 1` displays, where a forgotten scale factor still looks correct — the breakage only shows on a user's HiDPI screen, by which point it has often propagated through the coordinate pipeline. **Always smoke-test GUI/coordinate work under a non-unity scale before committing:**

- `GDK_SCALE=2 ansel` forces the integer scale-factor (`gui->ppd == 2`), exercising every device-pixel buffer and the toolkit's CSS/geometry scaling. This is the single most effective check.
- Set the *screen DPI* override in `Preferences → General` (or `screen_dpi_overwrite` in the config) to a value like 144 or 192 to exercise the `dpi_factor` / font path.
- Combine the two to reproduce a fractional-HiDPI laptop (e.g. `GDK_SCALE=2` with a 144 DPI override).

If a widget looks right at 1× but blurry or mis-sized at `GDK_SCALE=2`, the surface or size very likely bypassed the scaling API documented above.

### Spacing within boxes, grids and flowboxes

Within `GtkBox`, `GtkGrid` and `GtkFlowBox` containers, it is not possible to define spacing between children using CSS `margin`/`padding`: that would not honour the boundaries of the container, so widgets sitting on the container's edges would end up recessed compared to its contours while inner widgets would not. Spacing between children therefore has to be set in C at box-creation time, using the `DT_GUI_BOX_SPACING` macro (`src/gui/gtk.h`), so it is managed consistently from a single place.

`DT_GUI_BOX_SPACING` is expressed as a fraction of `1em` (`DT_GUI_BOX_SPACING_EM = 0.625`, i.e. 10px at the 16px reference font). The current `1em` size in px is resolved from the active theme/font by `dt_gui_update_em()` and cached in `darktable.gui->em`; it is refreshed whenever the theme/font or the screen DPI changes. Because the font's point→px conversion already folds in the screen DPI, the spacing needs **no** `DT_PIXEL_APPLY_DPI` on top — and because it is `em`-based, the inner gutters now scale with the user's font-size setting exactly like the `em`-based margins/paddings in `data/themes/ansel.css`.

`gtk_box_set_spacing()` (and the grid/flowbox equivalents) bake the value into the widget at creation time, so reloading the CSS does *not* update the gutters of already-built containers. To make a runtime font/DPI change take effect without restarting, `dt_gui_update_em()` walks every toplevel and re-applies the new `DT_GUI_BOX_SPACING` to the containers that still carry the previous standard value (deliberate `0`-spacing and custom-spacing containers are left untouched, matched by value). New code therefore does **not** need to do anything special: keep passing `DT_GUI_BOX_SPACING` at creation and the live refresh handles the rest. Note that fixed pixel `gtk_widget_set_size_request()` geometry has the same one-shot nature but is *not* yet re-applied live — changing the DPI override still requires a restart for those to take full effect.

To stay visually consistent with it, CSS margins and paddings that contribute to layout/spacing should be expressed in `em` too (use `calc(<n>em - 1px)` where a fixed hairline border has to be accounted for; GTK3 supports `calc()` with mixed units). Hairline borders themselves stay in `px` so they render crisp. Anything related to text or font metrics already follows the font size naturally through `em`.

> **Migration status.** The C side (`DT_GUI_BOX_SPACING`) is `em`-derived. The `data/themes/ansel.css` layout values are still being migrated from `px` to `em`; at the reference 16px font the two are identical (`0.625em == 10px`), so they only diverge once the user scales the font, which is the behaviour the migration is converging toward.

The deliberate exception is scrollbar sliders, which are sized in `em` so their grip grows with the font size for legibility.

### Cairo image surfaces

Cairo image surfaces that are **drawn in logical coordinates and blitted to the screen** (widget icons, scopes, graphs, overlays, navigation thumbnails) must be created through the wrappers in `src/gui/gtk.h` rather than the native cairo/gdk calls:

| Use this wrapper | instead of the native call |
|---|---|
| `dt_cairo_image_surface_create()` | `cairo_image_surface_create()` |
| `dt_cairo_image_surface_create_for_data()` | `cairo_image_surface_create_for_data()` |
| `dt_cairo_image_surface_create_from_png()` | `cairo_image_surface_create_from_png()` |
| `dt_cairo_image_surface_get_width()` / `_get_height()` | `cairo_image_surface_get_width()` / `_get_height()` |
| `dt_gdk_cairo_surface_create_from_pixbuf()` | `gdk_cairo_surface_create_from_pixbuf()` |
| `dt_gdk_pixbuf_new_from_file_at_size()` | `gdk_pixbuf_new_from_file_at_size()` |

The wrappers exist to centralize HiDPI handling: they allocate the backing buffer at `width * ppd` × `height * ppd` device pixels and call `cairo_surface_set_device_scale(surface, ppd, ppd)`, so the caller draws in **logical** coordinates and the result is crisp on HiDPI displays. The matching `_get_width()` / `_get_height()` wrappers divide back by `ppd` so callers reason in logical pixels too. Doing this by hand (raw `create()` + a separate `set_device_scale()`) is what we are migrating *away* from, because it is easy to forget the device-scale call (→ blurry output) or to mismatch the buffer dimensions.

**Exceptions — these must stay on the raw cairo calls:**

- **Image/pipeline/export/print surfaces** — watermark, print, the chart tool, and module previews drawn at the *image* resolution. `ppd` is a screen concept; multiplying an image-space buffer by it is wrong. These operate in image pixels, not logical GUI pixels.
- **Self-managed device-pixel buffers** — a few GUI paths (the darkroom image surface in `views/view.c`, snapshots, the drawlayer cursor/preview) deliberately compute their buffer size in *device* pixels themselves (for direct pixel-buffer access or cache sizing) and then *publish* the scale with a bare `cairo_surface_set_device_scale(s, ppd, ppd)`. That is the legitimate low-level form of the same contract; it does not go through the wrapper because the wrapper would re-multiply already-device dimensions.

The rule of thumb: **if you pass logical dimensions and want a screen-crisp surface, use the wrapper. If you are working in image/export space, or you have already sized the buffer in device pixels yourself, stay raw.**

### Drawing text on Cairo surfaces

Several widgets (the bauhaus controls, iop graphs/scopes) draw their own text with Pango/Cairo rather than letting GTK lay out a `GtkLabel`. To make that text match the rest of the UI — same font family/weight, DPI, anti-aliasing, hinting, subpixel order and kerning — use the two app-level helpers instead of configuring Cairo/Pango by hand:

- `dt_gui_set_pango_resolution(layout)` — sets the layout resolution to the screen DPI (`darktable.gui->dpi`), so point sizes convert to px the same way as native widgets.
- `dt_gui_cairo_set_font_options(cr, widget)` — pushes the system text-rendering options (anti-aliasing, hinting, subpixel order, hint-metrics/kerning) onto the Cairo context. These come from `widget`'s Pango context, which GTK populates from `GtkSettings`/Xft/fontconfig — the identical source native widgets use. An off-screen Cairo surface otherwise defaults to "anti-aliasing on" regardless of system settings, so text drawn on it looks subtly different from the rest of the UI.

The canonical recipe for cairo-drawn text:

```c
dt_gui_cairo_set_font_options(cr, widget);                 // system AA/hinting/kerning
PangoLayout *layout = pango_cairo_create_layout(cr);
PangoFontDescription *desc = NULL;                          // font family/weight/style from CSS
gtk_style_context_get(context, state, GTK_STYLE_PROPERTY_FONT, &desc, NULL);
pango_layout_set_font_description(layout, desc);            // (optionally override the size)
dt_gui_set_pango_resolution(layout);                        // screen DPI
/* ... set_text, update_layout, show_layout ... */
```

Font *family/weight/style* therefore always come from CSS (the `GTK_STYLE_PROPERTY_FONT` of the widget's style context), never hardcoded — which keeps cairo-drawn text on the same theming path as everything else. The bauhaus text renderer (`src/bauhaus/bauhaus.c`) is the reference implementation; it additionally merges only the style fields of the CSS font while keeping its own resolved size.

### GtkTextView: background, border and padding

`GtkTextView` doesn't expose its internal "text" CSS node through any public API, so styling it the same way as `entry`/`treeview` requires working around a few GTK3 quirks (also documented next to the `textview` rules in `data/themes/ansel.css`):

- `background-color` has to be set on the outer `textview` node.
- `border` only gets painted when applied to the inner `textview text` node — a `border` declared on `textview` itself reserves layout space but is never drawn.
- `padding` on the outer `textview` node *does* shift the inner "text" node (and the glyphs with it) inward, but a `border` on `textview text` would then be painted at that shifted position, i.e. *outside* the padding instead of around it — the opposite of the `entry`/`treeview` look (border flush with the widget edge, padding between the border and the text).
- `padding` declared directly on `textview text` is parsed without error but has **no effect** on text layout at all.

Because of this, CSS alone cannot reproduce the `entry`/`treeview` recessed look for a `GtkTextView`. The CSS only provides the background and the border (`textview { padding: 0; background-color: @recessed_color_bg; }` and `textview text { border: 1px solid @recessed_color_border; }`); the actual 2px/4px text inset is applied in C with `dt_gui_textview_set_padding()` (`src/gui/gtk.c`), which calls `gtk_text_view_set_{left,right,top,bottom}_margin()`. Every `GtkTextView` created in the codebase should call this helper so it stays visually consistent with `entry`/`treeview`.

### Scrollbar slider spacing

`scrollbar slider` cannot use `margin` in CSS (GTK ignores it for this node), so the gap around the slider is faked with a transparent `border` instead (see `data/themes/ansel.css`).

## Font

We use the font Roboto, because: 

1. it was designed for legibility on hiDPI screens,
2. is has a wide language support,
3. it has a wide font weight & variants support,
4. this allows to fine-tune the display better than trying to account for any system font.

If Roboto is not found on the system, we try to reuse the system font: Segoe UI on Windows, SF Pro on Mac, Ubuntu on Ubuntu, Cantarell on Gnome etc. But depending on how the font manager is configured, for different font weight, we might actually need to change the `font-family` name (like "Roboto Display" for `font-weight: 300`), and not merely the weight. So that makes for cumbersome fonts definitions that need to account for every combination on every OS.

TODO: ship Roboto in pre-built packages.

## Developping

Start `GTK_DEBUG=interactive ansel` to get the GTK inspector. Before changing any styling rule, identify which line from `ansel.css` is last overriding it, from GTK node inspector nodes list on the target widget. Then find if you can mutualize/refactor global rules and existing overrides, rather than hacking another override on top.