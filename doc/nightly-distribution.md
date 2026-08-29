# Nightly distribution

How a nightly build gets from a GitHub runner to a user's machine, on every OS, and how
it gets updated once it is there. Decided 2026-08-29 (issue #1320); this is the manual.

## The shape of it

```
 lin-nightly ─┐                                  ┌─► ansel-website  data/nightly.json ─► download buttons
 mac-nightly ─┤   monthly pre-release            │                  ansel.photos/nightly.json ─► in-app check
 win-nightly ─┼─► nightly-YYYY-MM  ─► nightly-manifest ─┼─► homebrew-ansel Casks/ansel-nightly.rb
 flatpak     ─┤   (GitHub Releases)   (nightly.json)   └─► scoop-ansel   bucket/ansel-nightly.json
 docker      ─┘        │
                       └─► nightly-prune: delete months older than the window
```

Five things, each with one job:

| piece | where | job |
|---|---|---|
| **Monthly release** | `nightly-YYYY-MM` pre-release, one per month | the host. GitHub caps a *release* at 1000 assets; releases, total size and bandwidth are unlimited. Five formats a night filled the old rolling `v0.0.0` in under a year. Every nightly computes the tag at run time (`Compute the release tag` step) and `aurelienpierreeng/tip` creates it on first use. |
| **Manifest** | `.github/workflows/nightly-manifest.yml` → `tools/nightly_manifest.py` | the one file that says what "latest" means: newest asset per format, with url, size, sha256, version and full commit. Runs after each nightly finishes (`workflow_run`), pushes the file to the website and regenerates the cask and the Scoop manifest. |
| **Retention** | `.github/workflows/nightly-prune.yml` | first of the month, deletes `nightly-*` releases older than 12 months (tag included). Dry-run and window are workflow inputs. Never matches a real version tag. |
| **In-app check** | `src/common/updates.c` | nightly channel + GUI only, once a day, GET of the manifest, compare commit. Toast + *Help ▸ Update to the latest nightly build*. |
| **Package managers** | Flatpak repo on R2, Homebrew tap, Scoop bucket | `flatpak update`, `brew upgrade`, `scoop update` — the OS-native path. |

The per-OS workflows stay dumb: build, upload to the month's release, notify Matrix.
Everything that knows about downstream lives in `nightly-manifest.yml`, so adding a
consumer is one `publish` line there and nothing in five workflows.

## nightly.json

```json
{
 "schema": 1, "generated": "2026-08-30T06:41:12Z", "channel": "nightly",
 "repo": "aurelienpierreeng/ansel", "download_page": "https://ansel.photos/en/download/",
 "formats": {
  "appimage":  { "name": "Ansel-0.0.0+4810.gabc…-x86_64.AppImage", "url": "…", "size": 103…, "sha256": "…",
                 "uploaded": "…", "release": "nightly-2026-08", "version": "0.0.0+4810.gabc…",
                 "commit_short": "abc…", "commit": "<full sha>" },
  "flatpak":   { … }, "dmg-arm64": { … }, "dmg-i386": { … }, "exe": { … },
  "docker":    { …, "image": "aurelienpierre/ansel", "pull": "aurelienpierre/ansel:0.0.0-4810.gabc…", "digest": "sha256:…" }
 }
}
```

Formats are recognised by filename shape in `tools/nightly_manifest.py` (`FORMATS`), and
the running application names its own format the same way in `dt_updates_runtime_format()`.
**Change one, change the other.** "Newest" is the most recently *uploaded* matching asset,
walking releases newest-first — not the first asset the API lists, which is not date order.

Docker: the image is pushed to Docker Hub as `:current` and `:<version with + as ->` (a tag
cannot contain `+`), *and* saved with `docker save | zstd` onto the month's release as
`Ansel-<version>-docker.tar.zst`, so a given night's image is retrievable months later and
without Docker Hub. The manifest entry is the release asset, enriched with the hub's pull
reference and digest when the hub has the same version. A release asset must be under
2 GiB; the workflow fails loudly if the archive is not, which is the cue to make the
Dockerfile multi-stage (it currently ships the whole build toolchain).

## What each format does when a newer build exists

| format | the user sees | in place? |
|---|---|---|
| AppImage | toast + Help menu → download. `LDAI_UPDATE_INFORMATION` is `gh-releases-zsync|…|latest-pre|…`, so `AppImageUpdate` / `appimageupdatetool` fetches a zsync delta from the newest pre-release | yes, with the external tool |
| Flatpak | `flatpak update` once the R2 repository is added | yes |
| dmg | toast + Help menu → download; or `brew upgrade` with the tap | download+drag (unsigned) |
| exe | toast + Help menu → download; or `scoop update` with the bucket | installer (unsigned) |
| Docker | `docker pull aurelienpierre/ansel:current` | yes |

## The in-app check, precisely

`dt_updates_init()` runs at the end of startup, after the privacy dialog. It returns
immediately unless all of: GUI, `DT_BUILD_CHANNEL == "nightly"`, `updates/enabled`, and
more than 24 h since `updates/last_check`. Then one thread GETs `updates/manifest_url`
(default `https://ansel.photos/nightly.json`, 5 s connect / 10 s total, 1 MiB cap), picks
`formats[<runtime format>]`, and compares its `commit` with `darktable_commit_hash`. The
nightly channel is monotonic, so *different* means *newer*. A hit posts a toast on the GUI
thread and arms `dt_updates_get_download_url()`, which *Help ▸ Update to the latest
nightly build* opens; with nothing armed the entry opens the download page, so it is
useful even before the check has run or when it is off.

What it sends: one GET with `User-Agent: Ansel/<version> (nightly)`. No identifier, no
body, nothing shared with the crash-report or analytics toggles. It is the third line of
the first-launch privacy dialog (default **on**, nightly builds only) and a toggle in
Preferences ▸ Storage ▸ Privacy. `-d control` logs every decision.

Self-builds and distribution packages never check: whoever built them updates them.

## Secrets and settings to create

All on the `ansel` repository. Every one is optional in the sense that its step is
skipped, with a warning, when it is absent — nothing fails a nightly for want of a secret.

| name | kind | used by | what it is |
|---|---|---|---|
| `NIGHTLY_PUBLISH_TOKEN` | secret | nightly-manifest | fine-grained PAT, **Contents: read & write** on `ansel-website`, `homebrew-ansel`, `scoop-ansel`. Nothing else. |
| `R2_ACCOUNT_ID` `R2_ACCESS_KEY_ID` `R2_SECRET_ACCESS_KEY` `R2_BUCKET` | secrets | flatpak-nightly | an R2 bucket (public, custom domain) and an API token scoped to it. Free tier: 10 GB, 1 M writes and 10 M reads a month, **egress free**. A pruned repo is a few hundred MB. |
| `FLATPAK_REPO_URL` | variable | flatpak-nightly | the public URL of that bucket, default `https://flatpak.ansel.photos` |
| `NIGHTLY_STATS_TOKEN` | secret | nightly-manifest | fine-grained PAT with **Administration: read** on `aurelienpierreeng/ansel` and nothing else — the one permission GitHub lists for the traffic endpoints (repository clones per day). The workflow's built-in `GITHUB_TOKEN` cannot read them: the first production run produced a series with no `traffic` block. Without this secret the download statistics are complete except for clones. |
| `FLATPAK_GPG_KEY` `FLATPAK_GPG_KEY_ID` | secrets | flatpak-nightly | an ASCII-armoured private key made for this and its id. Signs the repo and the bundle, and is embedded (public half) in `ansel.flatpakrepo`, so clients add the remote without `--no-gpg-verify`. `gpg --quick-gen-key "Ansel nightly <nightly@ansel.photos>" ed25519 sign never` then `gpg --export-secret-keys --armor <id>`. |

Docker Hub keeps its existing `DOCKERHUB_USERNAME` / `DOCKERHUB_TOKEN`.

Users then run, once:

```sh
flatpak remote-add --if-not-exists ansel https://flatpak.ansel.photos/ansel.flatpakrepo
flatpak install ansel photos.ansel.Ansel
```

## GHCR, for later

`ghcr.io/aurelienpierreeng/ansel` would need no separate credentials — `GITHUB_TOKEN`
with `packages: write` pushes to it — and ties the image to the repository's own
visibility and retention. The change is three lines in `docker-image.yml`: log in to
`ghcr.io` with `${{ github.actor }}` / `${{ secrets.GITHUB_TOKEN }}`, add a
`ghcr.io/aurelienpierreeng/ansel:<tag>` line under `tags:`, and grant
`packages: write` under `permissions:`. `tools/nightly_manifest.py` reads Docker Hub's
public tags API for the pull reference; GHCR's equivalent needs a token, so the manifest
would carry the release asset only, or the workflow would write the pull reference into
a small file on the release. Decision deferred: Docker Hub stays for now.

## Stores that need a real release

**winget** and **Chocolatey** want a versioned manifest with a pinned hash per version,
submitted through a moderated PR. That is a stable release, not a nightly; when there is a
tag, `wingetcreate` generates the manifest from the exe URL in one command. **Flathub** is
the same story and is written up in `packaging/flatpak/README.md`.

## Signing: procedure and cost

Both nightly installers are unsigned. Nothing above requires signing, but three things stop
short of "updates like any other app" without it, and users see a warning on every first
launch.

### macOS — Apple Developer ID + notarization

| step | what | cost |
|---|---|---|
| 1 | Apple Developer Program membership (individual or organisation) | **US$99 / year** |
| 2 | In the developer account, create a **Developer ID Application** certificate; export it with its private key as a `.p12` | — |
| 3 | Store the `.p12` (base64) and its password as secrets; in `mac-nightly.yml`, import it into a temporary keychain on the runner (`security create-keychain` / `security import`) | — |
| 4 | `packaging/macosx/3_make_hb_ansel_package.sh` and `4_make_hb_ansel_dmg.sh` already call `codesign … -s "${CODECERT}"` when `CODECERT` is set — set it to the certificate's identity | — |
| 5 | Notarize the dmg: `xcrun notarytool submit Ansel-*.dmg --apple-id … --team-id … --password <app-specific password> --wait`, then `xcrun stapler staple Ansel-*.dmg` | — |

After that: no Gatekeeper prompt, the cask needs no quarantine note, and **Sparkle** (the
standard in-app updater on macOS, EdDSA-signed appcast) becomes possible — it is what
would turn the toast into a one-click in-place update on macOS.

### Windows — Azure Trusted Signing (or a code-signing certificate)

| option | what | cost |
|---|---|---|
| **Azure Trusted Signing** | Microsoft-managed certificate, identity validation once, signing via `signtool` with the Trusted Signing dlib in CI. Public-trust certs, SmartScreen reputation accrues to the identity | **~US$10 / month** (Basic) |
| OV code-signing certificate | from a CA, on an HSM token since 2023 (so CI signing needs a cloud HSM or a self-hosted runner with the token) | ~US$200–400 / year + the HSM |
| EV certificate | immediate SmartScreen reputation | ~US$300–500 / year |

With any of them: `signtool sign /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 …`
on `ansel-*.exe` in `win-nightly.yml` before upload. CPack's NSIS generator has
`CPACK_NSIS_EXECUTABLE_SIGN` hooks for signing the uninstaller too.

### Linux

Nothing to buy. The Flatpak repo is GPG-signed by the key above; the AppImage can carry a
GPG signature (`linuxdeploy-plugin-appimage` honours `SIGN=1` with a key in the runner)
which `AppImageUpdate` verifies. Neither gates anything today.

### Recommendation

Windows first — Trusted Signing is cheap and SmartScreen is the warning most users hit.
macOS is the larger cost and the larger payoff (Sparkle). Budget both together at about
**US$220 / year**.
