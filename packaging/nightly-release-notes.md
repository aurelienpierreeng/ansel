## Welcome

This pre-release collects the packages produced by the nightly builds during the month in its name. Nightly builds run automatically every morning around 05:00–06:00 UTC from the `master` branch (the "fairly stable" channel), and produce packages for:

- **Linux, AppImage** (`.AppImage`) — any distribution shipping at least glibc 2.39 (Ubuntu 24.04, Debian 13, Fedora 40, openSUSE Leap 16, Arch, …). Runs anywhere without installing.
- **Linux, Flatpak** (`.flatpak`) — a single-file bundle: `flatpak install --user Ansel-*.flatpak`. Needs the `org.gnome.Platform//49` runtime from Flathub.
- **Windows** (`.exe`) — an installer for Windows 10 and 11.
- **macOS** (`.dmg`) — two packages: `arm64` for Apple Silicon (M1 and later), `i386` for Intel Macs.
- **Docker** — the image is pushed to [Docker Hub](https://hub.docker.com/r/aurelienpierre/ansel) (`aurelienpierre/ansel:current`, and one tag per build) and also saved here as `Ansel-*-docker.tar.zst`, so a given night's image stays retrievable: `zstd -d < Ansel-*-docker.tar.zst | docker load`.

This is meant for continuous testing and continuous delivery, in a "quickly broken, quickly fixed" way. [Learn more about Ansel channels](https://ansel.photos/en/doc/install/). For maximum stability, wait for production releases.

## One release per month

Each month gets its own pre-release, `nightly-YYYY-MM`, and every night's packages are added to the current month. Months older than a year are deleted. The newest package of each kind is always listed on the [download page](https://ansel.photos/en/), and a nightly build tells you itself when a newer one exists (*Help ▸ Update to the latest nightly build*).

You can also let your package manager track the nightlies:

- macOS: `brew tap aurelienpierreeng/ansel && brew install --cask ansel-nightly`
- Windows: `scoop bucket add ansel https://github.com/aurelienpierreeng/scoop-ansel && scoop install ansel-nightly`
- Linux AppImage: [AppImageUpdate](https://github.com/AppImageCommunity/AppImageUpdate) fetches the newest build as an incremental download.

## Understanding package names

All packages follow the same naming convention: `Ansel-x.y.z+N.g0000000000-architecture.extension`

- `x.y.z` is the tag of the latest stable release. It only ever increases, and tells you how old or recent a package is compared to the one you have installed.
- `+N` is the number of commits on the branch since that release. It also only ever increases: a higher `N` is a newer build.
- `.g` means the package comes straight from Git,
- the ten hexadecimal characters after `.g` identify the exact commit the package was built from. They follow no order, but they pin a precise state of the source code, which is what to quote when reporting a bug. The same commit can be found in the Git log and in the application's *About* dialog.
- `architecture` tells you which CPU the package is built for: `x86_64` or `win64` for 64-bit Intel/AMD, `arm64` for Apple Silicon, `i386` for Intel Macs.
- `.extension` is the OS-flavoured package: `.AppImage`, `.flatpak`, `.exe`, `.dmg`, or `-docker.tar.zst`.

The `.AppImage.zsync` files exist for auto-updaters, to allow incremental updates. They have to be here for technical reasons; end users should not download them.
