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

### Spacing within boxes, grids and flowboxes

Within `GtkBox`, `GtkGrid` and `GtkFlowBox` containers, it is not possible to define spacing between children using CSS `margin`/`padding`: that would not honour the boundaries of the container, so widgets sitting on the container's edges would end up recessed compared to its contours while inner widgets would not. Spacing between children therefore has to be hardcoded in C at box-creation time, using the `DT_GUI_BOX_SPACING` macro (`src/gui/gtk.h`), so it is managed consistently from a single place.

`DT_GUI_BOX_SPACING` is defined in device pixels (10px) and is intentionally *not* rescaled through `DT_PIXEL_APPLY_DPI()`. To stay visually consistent with it, CSS margins and paddings that contribute to layout/spacing should also be expressed in `px`, which the window manager and GTK rescale for HiDPI the same way as `DT_GUI_BOX_SPACING`. Anything related to text or font metrics (padding around labels, line spacing, etc.) should instead be expressed in `em`, so it follows the user's font-size setting.

The deliberate exception is scrollbar sliders, which are sized in `em` so their grip grows with the font size for legibility.

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