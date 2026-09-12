# Font install notes (IBM Plex Sans dev setup)

Ansel's CSS can *reference* a font family, but GTK/Pango only renders it if the font is
actually resolvable on the machine — otherwise the stack silently falls through to the next
name (which is how the stock theme silently falls back from Roboto to Segoe UI Light today).
Swapping the font is therefore two separate problems: the CSS stack, and getting the font to
actually resolve.

## What we did (dev machine only)

1. Sourced the official OFL-licensed release archives from `IBM/plex` on GitHub: Sans 1.1.0,
   Sans Condensed 2.0.0, Mono 2.5.0. Extracted Regular/Medium/SemiBold/Light (Sans),
   Regular/Medium (Condensed), Regular (Mono), plus each family's `LICENSE.txt`.
2. Installed the 7 `.ttf` files as **per-user** Windows fonts (right-click -> Install, no admin
   needed) into `%LOCALAPPDATA%\Microsoft\Windows\Fonts\`.
3. Updated `data/themes/ansel.css` section 2 to reference them. TTF name tables truncate
   legacy family names to the old 31-char limit, so the CSS uses the exact truncated strings
   fontconfig resolves to (verified with `fc-match`, using the app's own bundled
   `ansel-dev/bin/fc-match.exe`, not a system one) rather than trusting automatic weight
   matching:
   - `"IBM Plex Sans Light"` (body, weight 300)
   - `"IBM Plex Sans Medm"` (emphasized labels, weight 500 — legacy name for "Medium")
   - `"IBM Plex Sans Cond"` (condensed panel labels — legacy name for "Condensed")
   - `"IBM Plex Sans"` (weight 400 / plain fallback)
   - `"IBM Plex Mono"` (`.dt_monospace`)

## Constraint this leaves open

This only renders correctly **on this machine**, because it depends on the OS-level per-user
font install done by hand above. A fresh install, another user's machine, or a packaged
nightly would silently fall back to Segoe UI, same as the pre-existing Roboto gap.

macOS already solves this properly and needs no C code: `packaging/macosx/Info.plist`'s
`UIAppFonts` + `ATSApplicationFontsPath` bundles fonts into the app and registers them via
Apple's own font-bundling mechanism. Windows has no equivalent bundle metadata. The portable
fix is a `FcConfigAppFontAddDir()` call at startup (before the CSS provider/theme loads),
registering a bundled `fonts/` directory with fontconfig the same way `ATSApplicationFontsPath`
does on macOS — not yet implemented; deferred as its own C change + packaging/install-rule
change, not done as part of this CSS pass.
