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

"""Derive the Flathub manifest from the nightly one.

A Flathub build gets no network: every input has to be declared in the manifest and
hashed up front. The nightly manifest is the opposite by design -- it builds the working
tree and lets `data/CMakeLists.txt` fetch the current lens database and denoise models at
configure time, so a nightly always carries today's calibrations.

The two differ in four places and nowhere else, which is why this is a generator and not a
second manifest to keep in sync:

  * the app module builds a pinned git tag instead of the working directory,
  * it drops `--share=network`,
  * the payloads it used to fetch become `type: file` sources, hashed,
  * and it is told where they landed, with the fetches switched off.

The hashes are read from the manifests the fetches themselves read, so this is never
hand-maintained: LensSerious publishes `db/v<schema>/manifest.json` and ansel-denoise
publishes `models/manifest.json`, both carrying a sha256 per file.

Usage:
    make-flathub-manifest.py --tag v1.0.0 [--commit SHA] [-o photos.ansel.Ansel.json]

With no --commit, the tag is resolved against the local repository.
"""

import argparse
import json
import re
import subprocess
import sys
import urllib.request
from collections import OrderedDict
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent

NIGHTLY_MANIFEST = HERE / "photos.ansel.Ansel.json"
ANSEL_GIT = "https://github.com/aurelienpierreeng/ansel.git"

LENS_SCHEMA_SQL = ROOT / "src" / "external" / "LensSerious" / "src" / "schema.sql"
LENS_BASE = "https://raw.githubusercontent.com/aurelienpierreeng/LensSerious/main/db"
MODELS_BASE = "https://raw.githubusercontent.com/aurelienpierreeng/ansel-denoise/master/models"

# flatpak-builder builds each module in /run/build/<module name>, and a source's "dest" is
# relative to that. The build itself happens in _flatpak_build beneath it (builddir: true),
# which is why these are absolute rather than relative to the compiler's working directory.
BUILD_ROOT = "/run/build/ansel"
LENS_DEST = "lens-db"
MODELS_DEST = "nn-models"


def fetch_json(url):
    with urllib.request.urlopen(url, timeout=60) as response:
        return json.loads(response.read().decode("utf-8"))


def lens_schema_version():
    """The schema the pinned LensSerious reads, which is the database directory to use."""
    text = LENS_SCHEMA_SQL.read_text(encoding="utf-8")
    match = re.search(r"PRAGMA\s+user_version\s*=\s*(\d+)", text)
    if not match:
        sys.exit(f"no PRAGMA user_version in {LENS_SCHEMA_SQL}")
    return int(match.group(1))


def resolve_commit(tag):
    result = subprocess.run(["git", "-C", str(ROOT), "rev-list", "-n", "1", tag],
                            capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"cannot resolve tag {tag!r} in {ROOT}: {result.stderr.strip()}")
    return result.stdout.strip()


def payload_sources():
    """Every file the nightly build would have downloaded, as hashed manifest sources."""
    sources = []

    schema = lens_schema_version()
    lens_manifest = fetch_json(f"{LENS_BASE}/v{schema}/manifest.json")
    for name, entry in sorted(lens_manifest["files"].items()):
        sources.append(OrderedDict([
            ("type", "file"),
            ("url", f"{LENS_BASE}/v{schema}/{name}"),
            ("sha256", entry["sha256"]),
            ("dest", LENS_DEST),
        ]))

    models_manifest = fetch_json(f"{MODELS_BASE}/manifest.json")
    for name, entry in sorted(models_manifest["models"].items()):
        sources.append(OrderedDict([
            ("type", "file"),
            ("url", f"{MODELS_BASE}/{name}"),
            ("sha256", entry["sha256"]),
            ("dest", MODELS_DEST),
        ]))

    return sources, schema


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tag", required=True, help="the release tag to build")
    parser.add_argument("--commit", help="the commit the tag points at (resolved locally if omitted)")
    parser.add_argument("-o", "--output", help="write here instead of stdout")
    args = parser.parse_args()

    commit = args.commit or resolve_commit(args.tag)

    manifest = json.loads(NIGHTLY_MANIFEST.read_text(encoding="utf-8"),
                          object_pairs_hook=OrderedDict)

    sources, schema = payload_sources()

    for module in manifest["modules"]:
        if not isinstance(module, dict) or module.get("name") != "ansel":
            continue

        module["sources"] = [OrderedDict([
            ("type", "git"),
            ("url", ANSEL_GIT),
            ("tag", args.tag),
            ("commit", commit),
        ])] + sources

        # No network during the build, so nothing may be fetched from inside it.
        module["config-opts"] += [
            "-DFETCH_LENS_DB=OFF",
            f"-DLENS_DB_DIR={BUILD_ROOT}/{LENS_DEST}",
            "-DFETCH_NN_MODELS=OFF",
            f"-DNN_MODELS_DIR={BUILD_ROOT}/{MODELS_DEST}",
        ]

        build_options = module.get("build-options", {})
        build_options.pop("build-args", None)
        if build_options:
            module["build-options"] = build_options
        else:
            module.pop("build-options", None)
        break
    else:
        sys.exit("no module named 'ansel' in the nightly manifest")

    text = json.dumps(manifest, indent=4, ensure_ascii=False) + "\n"
    if args.output:
        Path(args.output).write_text(text, encoding="utf-8")
        print(f"wrote {args.output}: {args.tag} at {commit[:12]}, "
              f"lens database v{schema}, {len(sources)} payload sources", file=sys.stderr)
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
