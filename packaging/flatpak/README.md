# Flatpak packaging

`photos.ansel.ansel.json` is the Flatpak manifest. `build-flatpak.sh` builds it from the
local working tree.

The app id is `photos.ansel.ansel`, and it is load-bearing in four places that must agree:
the `app-id` key in the manifest, the `<id>` in `data/photos.ansel.ansel.appdata.xml.in`, the
name of the `.desktop` file `data/CMakeLists.txt` installs, and the file name of the appdata
itself. Because CMake already installs both files under that name, the manifest needs no
`rename-desktop-file` / `rename-appdata-file` — only `rename-icon`, since the icon is
installed as `ansel`. The script reads the id back out of the manifest rather than keeping
its own copy; three independent copies of it is how they drifted apart before.

## Building

```sh
packaging/flatpak/build-flatpak.sh
```

Requires `flatpak`, `flatpak-builder`, `git` and `jq` on the host. The GNOME SDK and the
Flathub-hosted dependencies are pulled on first run, so the first build is long and needs a
few GB of free disk.

It produces, in `build/flatpak/`:

| Output | Purpose |
| --- | --- |
| `repo/` | An OSTree repository. Serve it over HTTP and it is a Flatpak remote — this is what gives users `flatpak update`. |
| `Ansel-<version>-x86_64.flatpak` | A single-file bundle, for one-off installs. Installs with `flatpak install --user Ansel-*.flatpak`. A bundle carries no remote, so it never updates itself. |

Useful environment variables: `SOURCE_DIR` (tree to build, defaults to the repository root),
`BUILD_DIR`, `REPO_DIR`, `BUNDLE_NAME`, `VERSION`, and the signing pair below.

## Signing

Signing is opt-in and off by default:

```sh
GPG_KEY_ID=<key-id> GPG_HOMEDIR=~/.gnupg packaging/flatpak/build-flatpak.sh
```

Both the repository and the bundle are then signed. An unsigned repository still works, but
every client has to add it with `--no-gpg-verify`, which is not something to ask of users.

## Publishing an updatable repository

The nightly workflow (`.github/workflows/flatpak-nightly.yml`) attaches the bundle to the
rolling `v0.0.0` release, next to the AppImage. That covers installation but not updates.

For updates, `build/flatpak/repo` has to be rsynced to a static HTTP host (it is a plain
directory tree; no server-side software is involved) and advertised through a `.flatpakrepo`
file:

```ini
[Flatpak Repo]
Title=Ansel
Url=https://<host>/flatpak/
Homepage=https://ansel.photos/
Comment=Nightly builds of Ansel
Icon=https://<host>/ansel.svg
GPGKey=<base64 of the exported public key, single line>
```

with `GPGKey` produced by `gpg --export <key-id> | base64 -w0`. Users then run
`flatpak remote-add --if-not-exists ansel https://<host>/ansel.flatpakrepo` once.

`build-update-repo --prune` already runs at the end of each build, so a repository
republished every night does not accumulate objects no ref points at any more. GitHub Pages
is not a viable host for this: a full repository runs to the better part of a gigabyte,
against a 1 GB per-site limit.

## What is still missing for a Flathub submission

Flathub is a separate step from the above, and the manifest is not submittable as it stands:

- **The app module builds from `"type": "dir"`.** Flathub requires a reproducible source —
  a `git` or `archive` source pinned to a tag and commit. That in turn wants a real version
  tag; `v0.0.0` is a rolling nightly pointer, not a release.
- **`<releases>` in the appdata** carries a single development entry for the same reason.
  Flathub wants a release entry matching the submitted version.
- **The app module builds with `--share=network`.** Flathub forbids network access during a
  build. Ansel needs it because `data/CMakeLists.txt` fetches
  `https://lensfun.github.io/db/version_1.tar.bz2` at configure time, under `FETCH_LENS_DB`,
  and converts it into the `lenses.db` that LensSerious reads — deliberately, so a build
  carries the current calibrations rather than the packager's snapshot. A Flathub submission
  would have to pass `-DFETCH_LENS_DB=OFF` and let the importer fall back to the database
  liblensfun itself installs, pinning lens coverage to whatever the `lensfun` module ships.
  That is a real trade, not a formality: it is the difference between shipping today's
  calibrations and shipping the ones frozen into the manifest.
- **`--filesystem=host` and `--device=all`** are flagged during Flathub review. Both are
  defensible for a photo editor that opens arbitrary directories and uses OpenCL, and
  darktable ships with the same, but expect to argue them.
- **The screenshot** is from 2022 and no longer shows the current interface.

One former blocker is already gone: the manifest no longer requests
`--own-name=org.darktable.service`. `dt_dbus_init()` is disabled in `src/darktable.c`, so
nothing ever owned that name, and a name not prefixed by the app id would have been rejected.

## Inherited from darktable, and removed

The manifest started as Flathub's darktable one, and carried three things Ansel has no use
for. They are gone; do not restore them without a reason.

- **`exiftool` and `perl`.** Nothing in Ansel calls exiftool — the only occurrence in the
  tree is a documentation URL in a comment in `src/imageio/format/png.c`. `perl` existed
  solely to build it. Between them they were the longest part of the build, and the
  `exiftool` module was unbuildable anyway: it sourced an `exiftool-sources.json` that was
  never vendored into this repository.
- **The `lensfun` module's database refresh.** Its `post-install` pip-installed lxml and ran
  a `lensfun_convert_db.py` (likewise never vendored here) over a git checkout of the lensfun
  database, to give darktable current calibrations at run time. Ansel reads none of it:
  nothing in the pixel pipeline links liblensfun, and the calibrations come from `lenses.db`,
  built at configure time from the XML the previous section describes. The module is now
  built purely for its headers and its `.pc` file.
