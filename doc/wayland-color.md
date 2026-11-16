<!-- Provenance: every finding carries the commit it was established against. -->

# Colour management with a Wayland compositor

> **Measured 2026-11-17 on top of `5c05c61c73`**, Fedora 44, GTK 3.24.52, lcms2 2.16,
> libwayland 1.26, against KWin 6.8.2 and Weston 15.0.1. Every figure below says how it was
> obtained, and what was only read says so. Re-measure before acting on one older than the code
> you are changing, and re-date this line when you do.

The code is [`src/gui/wayland_color.c`](../src/gui/wayland_color.c) and three functions of
[`src/colorprofiles/colorspaces.c`](../src/colorprofiles/colorspaces.c). The protocol is
`wp_color_management_v1`, vendored in
[`src/external/wayland-protocols/`](../src/external/wayland-protocols/README.md).

## What the protocol asks of a client

A compositor that manages colour converts every surface from the *image description* the surface
declares to the one of the output. A surface that declares nothing is read as sRGB. The
compositor also publishes, per surface, the description it would *prefer* to receive — the
output's own, as a rule — so that a client able to render in it keeps the monitor's gamut and
costs the compositor no conversion.

Two things follow, and the first version of this work missed both.

**Reading the preferred description and declaring it are one act.** Rendering in the preferred
space while declaring nothing makes the compositor convert pixels that are already converted.
So the profile Ansel renders with and the description its windows declare must always be the
same thing, or both absent. That is why this is not a fourth reader next to the X atom in
`system/display_profile.c`: a reader has no window to declare anything on.

**The preferred description is not, in general, what an SDR window may declare.** The protocol
says so (`get_preferred`): a parametric description is to be set on a surface *only if it is an
exact match with the client content*, and a client whose content needs less than the announced
target volume *should create its own*. The description of this laptop's panel, read with
`wayland-info -i wp_color_manager_v1` on the live KWin session (read only):

    primaries  R 0.643 0.326  G 0.199 0.705  B 0.148 0.058  W 0.313 0.329
    tf_named   gamma22
    luminances min 0  max 281  reference 281
    target     max 844     max_cll 844

Its pixels are encoded for 281 cd/m², and it announces content up to 844. Set as it is on an
8-bit window, it claims HDR highlights the window does not have; KWin acts on that claim by
making room for them (read in KWin's source by the review of PR #1389, not run — it would have
driven the backlight of the machine in use).

## What is declared, case by case

`_description_received()` decides, once the compositor has said everything about the preferred
description:

| the compositor's preferred description | what Ansel renders with | what the windows declare |
|---|---|---|
| an ICC file | that file | the preferred description itself |
| parametric, a curve an 8-bit window can use, nothing brighter than its reference white, target volume equal to the encoding volume | a profile built from its primaries and curve | the preferred description itself |
| parametric, anything else | a profile built from its primaries, and its curve when usable, else gamma 2.2 | a description of our own (below) |
| neither, or primaries that span no area | nothing: Ansel keeps what it had | nothing: read as sRGB |

"A curve an 8-bit window can use" is `gamma22`, `gamma28`, `bt1886` (as 2.4), `srgb` (the
piecewise curve) or a power between 1 and 10. PQ, HLG, linear light and the logarithms are
replaced by gamma 2.2 in the same primaries, which the compositor places at its reference white.

**A description of our own is made from the ICC profile itself when the compositor takes
profiles (`icc_v2_v4`), from numbers otherwise** — the preferred primaries (by name when the
compositor named them, else as chromaticities), the curve, and `set_luminances(min, reference,
reference)`: the window's white is the reference white, and on an SDR output that is the
preferred description again.

The profile-first order is not a preference on Weston. **Weston 15.0.1 announces the parametric
creator, answers `ready`, then closes the connection on the first commit of a surface carrying
the result**: its log says `color-lcms error: failed to create a color transformation`, and the
client gets `wl_display.error(no memory)`. Measured with five descriptions, including one equal
to its own output's (sRGB by name, gamma22, 0.2/80/80): 5 of 5 fatal. Through the ICC creator,
the same five and a sixth: 6 of 6 run to the end. The same failure stops Weston from enabling an
output with a parametric profile at all (5 of 5 `[color-profile]` sections tried, headless
backend).

Every request of a creator is a *protocol error* — the connection is closed, and with it the
application — when the compositor did not announce the capability it needs. Hence the checks
before the first request in `_create_description()`, and no request that is not behind one.

## Who decides whether the compositor's profile is used

Not the GUI. It *offers* the profile (`dt_colorspaces_offer_compositor_display_profile()`), and
`colorprofiles/` takes it or not according to `ui_last/display_profile_source`:

| value | effect |
|---|---|
| `all` (default) | colord when it has a profile for the monitor; else the compositor's |
| `colord` | never the compositor's |
| `xatom` | never the compositor's (and on Wayland there is no atom: no system profile) |
| `wayland` | the compositor's, and colord is not asked |

**colord has the last word under `all`, and that is deliberate.** What colord holds is the
monitor's response, measured or read from its EDID. What a compositor prefers is the space it
composites in — on Mutter, by the review's reading of its source (48.0, 48.4, 49.0; not run),
plain sRGB whatever the panel, the assigned ICC profile being used for the calibration ramp
only. There the classic arrangement is the correct one: the application compensates for the
monitor and the compositor passes pixels through. So the compositor's profile is the fallback
for a monitor colord knows nothing about, which is also the one case where, before this, there
was no system profile at all — KWin sessions, where nothing feeds colord.

colord answers asynchronously, and after the compositor: a compositor describes a window as soon
as it is mapped, before the monitor has been probed once. An offer that arrives before colord
was asked (`colord_asked`), or while a query is in flight (`colord_pending`), waits, and is
installed by the colord callback when that answer comes back empty. Without that, every GNOME
start would render in the compositor's space for a few frames and then switch.

The windows are declared only while `dt_colorspaces_display_follows_compositor()` is true: the
installed monitor profile is the compositor's offer **and** the user's display profile choice is
the system one. A user who tells Ansel to treat the monitor as some other profile gets
undeclared windows, as before.

## What belongs to GDK

*Read in GTK 3.24's `gdk/wayland/gdkwindow-wayland.c`.*

- **The surface.** GDK creates a window's `wl_surface` with the window and destroys it when the
  window is hidden (`gdk_wayland_window_hide_surface()`, reached from hide, withdraw and
  destroy). Objects attached to it become inert, and any request on an inert object other than
  `destroy` is a protocol error. `GtkWidget::map` and `::unmap` run their class handler first,
  so an emission hook on them sees the surface already made, or already gone, with no compositor
  event dispatched in between. That hook is how windows GTK creates on its own are reached.
- **The commit.** A description set on a surface is pending state. Ansel never commits: it
  queues a redraw and GDK commits its next frame. Committing from outside pushes whatever GDK
  has staged without its buffer; the first version did, and a window mapped on a second output
  of scale 2 with an odd size died 20 times out of 20 with `surface size (1281x721) is not an
  integer multiple of scale (2)`. Same setup, this code: 20 of 20 run to the end.
- **Popups.** GDK destroys and recreates a popup's surface behind the widget — on
  `xdg_popup.done`, and when it is resized before its first configure — and decides what is a
  popup by window type and three type hints (`should_map_as_popup()`). `_is_popup()` repeats that
  rule: a popup is declared once, when it is mapped, and never touched again.
- **The position.** A Wayland client never learns where its window is: the review measured 0 of
  544 configure events carrying one. A move to another monitor is announced by
  `preferred_changed` and by nothing else, which is why following it cannot hang on a window
  event.

## Three things about lcms2

*Measured with lcms2 2.16.*

- `cmsBuildGamma(NULL, 0.0)` succeeds. The profile built on it maps sRGB black, mid-grey and
  white all to (255, 255, 255). `dt_colorspaces_build_display_profile()` refuses an exponent
  outside [1, 10], and primaries that span no area.
- A profile lcms2 creates carries its creation time in bytes 24 to 35 of the header: two builds
  of the same description two seconds apart differ in one byte. The display profile is replaced
  when its bytes differ, so the date is overwritten with a fixed one.
- A v2 profile stores a power law as an 8.8 fixed-point number: a 2.2 exponent written and read
  back is 2.199219. Written as v4 it is 2.199997, so the profile is built as v4.

`tests/unittests/test_display_profile_build.c` holds the first two; removing either fix makes one
of its tests fail.

## How it was run

There is no Wayland session in CI, and the session on the machine it was written on was not
touched. Everything ran against **private compositors**: `weston --backend=headless` and
`kwin_wayland --virtual`, each on its own socket in its own `XDG_RUNTIME_DIR`, with `DISPLAY`,
`WAYLAND_DISPLAY` and the session bus removed from the environment. The client was a 250-line
GTK program around the unmodified `wayland_color.c`, standing in for `colorprofiles/` (with the
real profile builder) and for the signal bus, under AddressSanitizer and `WAYLAND_DEBUG=1`. Its
scenario: map a dialog and a popup, stop following the compositor, follow it again, destroy
both, clean up, hide the main window.

| compositor | preferred description | result |
|---|---|---|
| Weston, default | sRGB by name, gamma22, 0.2/80/80 | declared as it is; dialog and popup declared at map; undeclared and redeclared on the two toplevels only |
| Weston, output with an ICC file | `icc_file`, 21012 bytes | that file is the profile; declared as it is |
| KWin, virtual output | sRGB by name, gamma22, 0.2/80/80, max_cll 80 | declared as it is |
| nested Weston, two outputs, scale 1 and 2 | sRGB, then the second output's ICC file | 20 of 20 runs to the end; the profile follows the window to the other output with no resize |

Neither compositor would produce a description that needs one of our own, so six were written
into the client between the compositor's answer and the module (one inserted call): this
laptop's panel as above, a target volume brighter than reference, BT.2020 PQ, BT.1886, a 2.4
power, linear light. On KWin each was answered with the parametric creator and `ready`; on
Weston, see above. A seventh, the CIE XYZ "primaries", is refused before anything is sent.

**Not run:** a GNOME session, the live KWin session, an HDR output, a real second monitor.
The arbitration with colord was compiled and read, not exercised end to end — the client above
replaces `colorprofiles/`.

## What the first version did

Recorded because each one looked reasonable. PR #1389 put the client in
`system/display_profile.c`, as a fourth branch of `dt_display_profile_read()`:

- it returned the preferred description as a profile through the X atom's code path, with no
  say for colord: after the first resize the compositor's profile replaced a calibrated one, and
  colord never re-asserted, its callback being keyed on an unchanged filename;
- it declared the preferred description as it was, on the main window only — dialogs, menus and
  the preview window stayed sRGB, so the same colour differed between two windows;
- it committed GDK's surface (above);
- it mapped one named tone curve and left the exponent at 0 for every other (above);
- it rebuilt the profile on every call, so the clock made it a new one every second;
- it kept the surface and its objects in a process global, never released: an event after the
  window was hidden at quit answered on a dead surface;
- it re-read the profile only when the centre widget changed size, while `preferred_changed`
  re-declared the surface at once;
- the X11 block still ran first on Wayland and its `source` string leaked on every call;
- the build required wayland-protocols without checking for it, linked libwayland-client through
  libgdk's dependency only, and exported 21 unprefixed symbols from libansel;
- and `src/system` may neither hold state nor include a generated header, which is what failed
  its CI.

## Open

- Popups keep the description they were mapped with, and GDK subsurfaces are never declared.
- Under `all`, colord keeps the display profile once it has given one, even if it later has
  none for the monitor the window moved to. That predates this work and applies to X11 too.
- The Weston failure is a property of 15.0.1; a later Weston may make the parametric creator
  usable there, and nothing in the code depends on it staying broken.
