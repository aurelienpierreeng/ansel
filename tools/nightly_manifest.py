#!/usr/bin/env python3
#   This file is part of the Ansel project.
#   Copyright (C) 2026 Aurélien PIERRE.
#
#   Ansel is free software: you can redistribute it and/or modify
#   it under the terms of the GNU General Public License as published by
#   the Free Software Foundation, either version 3 of the License, or
#   (at your option) any later version.
#
#   Ansel is distributed in the hope that it will be useful,
#   but WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#   GNU General Public License for more details.
#
#   You should have received a copy of the GNU General Public License
#   along with Ansel.  If not, see <http://www.gnu.org/licenses/>.

"""The nightly manifest: one JSON file naming the newest build of every format.

Everything downstream of the nightly builds reads this file instead of the GitHub
API: the website's download buttons, the in-app update check, the Homebrew cask and
the Scoop manifest. That is the point of it -- one place that knows what "latest"
means, written once per night by CI, served as a static file with no rate limit and
no dependency on GitHub being reachable from a user's machine.

    nightly_manifest.py manifest  > nightly.json     # from the GitHub releases
    nightly_manifest.py cask   nightly.json          # Homebrew cask (stdout)
    nightly_manifest.py scoop  nightly.json          # Scoop manifest (stdout)

The manifest walks the `nightly-YYYY-MM` pre-releases newest first and takes, per
format, the most recently uploaded asset. Formats are recognised by filename shape,
which is also what the running application matches itself against (see
src/common/updates.c), so the two must agree: change one, change the other.

sha256 is computed by downloading each asset. GitHub's API started publishing a
`digest` for release assets in 2025, and it is used when present; the download is the
fallback that keeps the manifest complete on assets uploaded before that.
"""

import argparse
import datetime
import hashlib
import json
import os
import re
import sys
import urllib.request

REPO = "aurelienpierreeng/ansel"
API = "https://api.github.com"
DOCKER_IMAGE = "aurelienpierre/ansel"
DOCKER_HUB = "https://hub.docker.com/v2/repositories"
TAG_RE = re.compile(r"^nightly-\d{4}-\d{2}$")

# Format key -> filename predicate. Keys are stable identifiers: the app, the cask and
# the Scoop manifest all address the manifest by them.
FORMATS = {
    "appimage":  lambda n: n.endswith("-x86_64.AppImage"),
    "flatpak":   lambda n: n.endswith("-x86_64.flatpak"),
    "dmg-arm64": lambda n: n.endswith("-arm64.dmg"),
    "dmg-i386":  lambda n: n.endswith("-i386.dmg"),
    "exe":       lambda n: n.endswith("-win64.exe"),
    # `docker save | zstd` of the image the same night pushed to Docker Hub.
    "docker":    lambda n: n.endswith("-docker.tar.zst"),
}

# Ansel-0.0.0+4802.gd5a317e072-x86_64.flatpak -> ("0.0.0+4802.gd5a317e072", "d5a317e072")
VERSION_RE = re.compile(r"^[Aa]nsel-(?P<version>[0-9][^-]*?\.g(?P<hash>[0-9a-f]+))-")
# The commit count after "+": monotonic by construction, which upload time is not --
# an asset moved between releases, or re-uploaded, gets a fresh timestamp.
COMMITS_RE = re.compile(r"\+(\d+)[.~]g")


def build_rank(name, uploaded):
    """Sort key for "newest": commit count first, upload time to break ties."""
    m = COMMITS_RE.search(name)
    return (int(m.group(1)) if m else -1, uploaded or "")
# The same version string as a bare Docker tag: 0.0.0+4802.gd5a317e072 -- except that a
# Docker tag cannot contain "+", so the workflow writes it as 0.0.0-4802.gd5a317e072.
VERSION_TAG_RE = re.compile(r"^[0-9][^-]*-\d+\.g(?P<hash>[0-9a-f]+)$")


def api(path, token):
    req = urllib.request.Request(f"{API}{path}", headers={
        "Accept": "application/vnd.github+json",
        "User-Agent": "ansel-nightly-manifest",
        **({"Authorization": f"Bearer {token}"} if token else {}),
    })
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.loads(r.read().decode("utf-8"))


def sha256_of_url(url):
    h = hashlib.sha256()
    req = urllib.request.Request(url, headers={"User-Agent": "ansel-nightly-manifest"})
    with urllib.request.urlopen(req, timeout=600) as r:
        for chunk in iter(lambda: r.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def docker_entry():
    """The newest versioned tag on Docker Hub, or None if the image has no such tag yet.

    The workflow tags each push twice, `current` and the version string; `current` is
    what a user pulls, the version tag is what tells us which nightly it is. Sorted by
    push time rather than by name so a re-push of an older version cannot win."""
    try:
        data = api_raw(f"{DOCKER_HUB}/{DOCKER_IMAGE}/tags?page_size=100&ordering=last_updated")
    except Exception as e:  # noqa: BLE001 -- Docker Hub down must not fail the manifest
        print(f"docker hub unreachable: {e}", file=sys.stderr)
        return None
    tags = [t for t in data.get("results", []) if t["name"] != "current" and VERSION_TAG_RE.match(t["name"])]
    if not tags:
        return None
    t = max(tags, key=lambda t: t["tag_last_pushed"])
    m = VERSION_TAG_RE.match(t["name"])
    digest = t.get("digest") or next((i.get("digest") for i in t.get("images", []) if i.get("digest")), None)
    return {
        "image": DOCKER_IMAGE,
        "tag": t["name"],
        "pull": f"{DOCKER_IMAGE}:{t['name']}",
        "digest": digest,
        "size": t.get("full_size"),
        "uploaded": t["tag_last_pushed"],
        "version": t["name"],
        "commit_short": m.group("hash"),
    }


def api_raw(url):
    req = urllib.request.Request(url, headers={"User-Agent": "ansel-nightly-manifest"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.loads(r.read().decode("utf-8"))


def build_manifest(token, with_hashes=True):
    releases = [r for r in api(f"/repos/{REPO}/releases?per_page=30", token)
                if TAG_RE.match(r["tag_name"]) and not r["draft"]]
    releases.sort(key=lambda r: r["tag_name"], reverse=True)

    # Per format, the matching asset with the highest commit count (build_rank), not
    # the first one the API lists -- a release holds a month of nightlies and asset
    # order is not build order -- and not the most recently uploaded either: an asset
    # moved from the retired rolling release into its month carries a fresh upload
    # time, and would have outranked that night's genuinely newer build. Releases are
    # walked newest first, and a format found in a newer release is never displaced
    # by an older one.
    newest = {}
    for rel in releases:
        for a in rel["assets"]:
            for key, match in FORMATS.items():
                if not match(a["name"]):
                    continue
                cur = newest.get(key)
                if cur and (cur["release"] > rel["tag_name"]
                            or build_rank(cur["name"], cur["uploaded"]) >= build_rank(a["name"], a["updated_at"])):
                    continue
                m = VERSION_RE.match(a["name"])
                newest[key] = {
                    "name": a["name"],
                    "url": a["browser_download_url"],
                    "size": a["size"],
                    "uploaded": a["updated_at"],
                    "release": rel["tag_name"],
                    "version": m.group("version") if m else None,
                    "commit_short": m.group("hash") if m else None,
                    "sha256": (a.get("digest") or "").removeprefix("sha256:") or None,
                }
        if len(newest) == len(FORMATS):
            break

    if with_hashes:
        for key, entry in newest.items():
            if not entry["sha256"]:
                print(f"hashing {entry['name']} ...", file=sys.stderr)
                entry["sha256"] = sha256_of_url(entry["url"])

    # The release asset is the record; Docker Hub is the convenient way to get it.
    # Merge the hub's pull reference and digest into the asset entry when the hub has
    # the same version, and keep the asset alone when it does not (hub down, or a
    # push that failed after the save).
    hub = docker_entry()
    if "docker" in newest:
        if hub and hub["version"].replace("-", "+", 1) == newest["docker"]["version"]:
            newest["docker"].update({"image": hub["image"], "pull": hub["pull"], "digest": hub["digest"]})
    elif hub:
        newest["docker"] = hub

    # The full commit for the newest build, resolved once: the app compares against its
    # own darktable_commit_hash, which is the full SHA, and filenames carry ten digits.
    commits = {}
    for entry in newest.values():
        short = entry["commit_short"]
        if short and short not in commits:
            try:
                commits[short] = api(f"/repos/{REPO}/commits/{short}", token)["sha"]
            except Exception as e:  # noqa: BLE001 -- best effort, the short hash still works
                print(f"could not resolve {short}: {e}", file=sys.stderr)
                commits[short] = None
        entry["commit"] = commits.get(short)

    return {
        "schema": 1,
        "generated": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "channel": "nightly",
        "repo": REPO,
        "download_page": "https://ansel.photos/en/download/",
        "formats": dict(sorted(newest.items())),
    }


def render_cask(manifest):
    arm = manifest["formats"].get("dmg-arm64")
    intel = manifest["formats"].get("dmg-i386")
    if not (arm and intel):
        sys.exit("cask needs both dmg-arm64 and dmg-i386 in the manifest")

    # Each architecture pins its own version, sha256 and URL: the two are built by
    # separate runners and one can fail a night the other succeeded. A shared version
    # with an #{arch} URL template would 404 on the arch that lagged.
    #
    # Homebrew wants a comparable version, and the nightly string already is one
    # (0.0.0+4802.gd5a317e072 sorts by commit count). A cask with `version :latest`
    # is skipped by plain `brew upgrade`, which is why this file is regenerated
    # nightly with a real version instead.
    def block(entry):
        return (f'    version "{entry["version"]}"\n'
                f'    sha256 "{entry["sha256"]}"\n'
                f'    url "{entry["url"]}",\n'
                f'        verified: "github.com/{REPO}/"\n')

    return f'''# Generated by tools/nightly_manifest.py in aurelienpierreeng/ansel -- do not edit.
cask "ansel-nightly" do
  on_arm do
{block(arm)}  end
  on_intel do
{block(intel)}  end

  name "Ansel (nightly)"
  desc "Photo editor and library manager for digital negatives, nightly build"
  homepage "https://ansel.photos/"

  livecheck do
    url "https://ansel.photos/nightly.json"
    strategy :json do |json|
      json.dig("formats", "dmg-arm64", "version")
    end
  end

  # Nightly builds are not signed or notarized: Gatekeeper quarantines them, and the
  # first launch needs a right-click > Open. See doc/nightly-distribution.md.
  app "Ansel.app"

  zap trash: [
    "~/.config/ansel",
    "~/.cache/ansel",
  ]
end
'''


def render_scoop(manifest):
    exe = manifest["formats"].get("exe")
    if not exe:
        sys.exit("scoop needs exe in the manifest")
    doc = {
        "version": exe["version"],
        "description": "Photo editor and library manager for digital negatives (nightly build)",
        "homepage": "https://ansel.photos/",
        "license": "GPL-3.0-or-later",
        "url": exe["url"],
        "hash": exe["sha256"],
        # The NSIS installer; Scoop runs it silently into its own app directory.
        "innosetup": False,
        "installer": {"args": ["/S", "/D=$dir"]},
        "bin": [["bin\\ansel.exe", "ansel"], ["bin\\ansel-cli.exe", "ansel-cli"]],
        "shortcuts": [["bin\\ansel.exe", "Ansel (nightly)"]],
        # checkver lets `scoop status` see a newer nightly. No `autoupdate` block on
        # purpose: this manifest is regenerated by Ansel's own CI every night, so
        # Scoop's bucket-side auto-bump has nothing to do and must not fight it.
        "checkver": {
            "url": "https://ansel.photos/nightly.json",
            "jsonpath": "$.formats.exe.version",
        },
    }
    return json.dumps(doc, indent=4) + "\n"


def render_summary(manifest):
    """A table for the run summary, one line per format; says so when there is none."""
    formats = manifest.get("formats", {})
    if not formats:
        return "(no nightly-* release carries any asset yet)\n"
    return "".join(f"{k:10s} {v.get('version') or '?':32s} {v.get('uploaded') or ''}\n"
                   for k, v in formats.items())


def check(manifest):
    """Exit status 0 when the manifest names at least one build, 1 when it is empty.

    The workflow gates publishing on this: an empty manifest -- no nightly-* release
    yet, or a GitHub API hiccup that returned nothing -- must never overwrite a good
    file downstream. The first run on master did exactly that to the website's data
    file before this existed."""
    return 0 if manifest.get("formats") else 1


def render_oneline(manifest):
    """One line for a commit message: `appimage 0.0.0+4810.g..., exe 0.0.0+4810.g...`."""
    formats = manifest.get("formats", {})
    return ", ".join(f"{k} {v.get('version') or '?'}" for k, v in formats.items()) or "no builds"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("manifest"); m.add_argument("--no-hashes", action="store_true")
    for name in ("cask", "scoop", "summary", "oneline", "check"):
        sub.add_parser(name).add_argument("manifest")
    args = ap.parse_args()

    if args.cmd == "manifest":
        doc = build_manifest(os.environ.get("GITHUB_TOKEN"), with_hashes=not args.no_hashes)
        json.dump(doc, sys.stdout, indent=1); sys.stdout.write("\n")
        return
    doc = json.load(open(args.manifest, encoding="utf-8"))
    if args.cmd == "check":
        sys.exit(check(doc))
    render = {"cask": render_cask, "scoop": render_scoop, "summary": render_summary, "oneline": render_oneline}
    sys.stdout.write(render[args.cmd](doc))


if __name__ == "__main__":
    main()
