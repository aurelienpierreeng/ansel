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

"""Download statistics for the nightly packages, as a daily series.

    download_stats.py snapshot                > today.json
    download_stats.py append series.json today.json > series.json

GitHub keeps one lifetime `download_count` per release asset and nothing else -- delete
the asset (the monthly prune does, and so did moving August 2026 between releases) and
its count is gone. Docker Hub keeps one lifetime `pull_count` per repository. Neither is
a time series. This turns them into one: a snapshot of every counter, once a day, appended
to a list that only ever grows. Downloads *between* two dates are the difference of two
snapshots; the loss on deletion stops mattering once the day before is on record.

Every route a user takes ends up in these counters: the website's buttons, the in-app
"Update to the latest nightly build", the Homebrew cask and the Scoop manifest all
download the release asset, and AppImageUpdate fetches the .zsync and ranges of the
AppImage. Neither Homebrew nor Scoop has analytics for third-party taps; there is
nothing to add from their side.

The build month of an asset is read from its release tag (nightly-YYYY-MM) when the tag
carries one, and from the asset's creation date otherwise -- an asset moved between
releases is created anew, so its date is the day it was moved, not the night it was built.
"""

import collections
import datetime
import json
import re
import os
import sys
import urllib.request

REPO = "aurelienpierreeng/ansel"
API = "https://api.github.com"
DOCKER_IMAGE = "aurelienpierre/ansel"
DOCKER_HUB = "https://hub.docker.com/v2/repositories"
MONTH_TAG_RE = re.compile(r"^nightly-(\d{4}-\d{2})$")

# Filename suffix -> format key. Same vocabulary as nightly_manifest.py, plus the
# updater's .zsync, which is its own line: those are AppImageUpdate fetching the newest
# build incrementally, not a person clicking a button.
FORMATS = [
    (".AppImage.zsync", "zsync"),
    ("-x86_64.AppImage", "appimage"),
    ("-x86_64.flatpak", "flatpak"),
    ("-arm64.dmg", "dmg-arm64"),
    ("-i386.dmg", "dmg-i386"),
    ("-win64.exe", "exe"),
    ("-docker.tar.zst", "docker-archive"),
]


def format_of(name):
    for suffix, key in FORMATS:
        if name.endswith(suffix):
            return key
    return "other"


def get_json(url, token=None):
    req = urllib.request.Request(url, headers={
        "Accept": "application/vnd.github+json", "User-Agent": "ansel-download-stats",
        **({"Authorization": f"Bearer {token}"} if token else {}),
    })
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.loads(r.read().decode("utf-8"))


def github_releases(token):
    page, out = 1, []
    while True:
        batch = get_json(f"{API}/repos/{REPO}/releases?per_page=100&page={page}", token)
        if not batch:
            return out
        out.extend(batch)
        page += 1


def snapshot(token):
    by_format = collections.Counter()
    by_month = collections.Counter()
    by_release = collections.Counter()
    assets = 0
    for rel in github_releases(token):
        tag = rel["tag_name"]
        m = MONTH_TAG_RE.match(tag)
        for a in rel["assets"]:
            n = int(a.get("download_count") or 0)
            month = m.group(1) if m else (a.get("created_at") or "")[:7]
            by_format[format_of(a["name"])] += n
            by_month[month] += n
            by_release[tag] += n
            assets += 1

    # Repository clones, per day, from the traffic API: the closest thing to "downloads"
    # for people who build from source. Only the last 14 days are exposed and the call
    # needs write access to the repository, so it is best effort: a token without it
    # (possibly the workflow's own GITHUB_TOKEN) yields a warning and no traffic block,
    # and the snapshot is otherwise complete. The raw count is dominated by CI -- every
    # workflow job clones -- so `uniques` (distinct cloners) is the figure to read.
    traffic = None
    try:
        clones = get_json(f"{API}/repos/{REPO}/traffic/clones?per=day", token)
        traffic = {c["timestamp"][:10]: {"clones": c["count"], "uniques": c["uniques"]} for c in clones.get("clones", [])}
    except Exception as e:  # noqa: BLE001
        print(f"traffic API unavailable ({e}); snapshot has no clone counts", file=sys.stderr)

    docker = None
    try:
        hub = get_json(f"{DOCKER_HUB}/{DOCKER_IMAGE}/")
        docker = {"pull_count": int(hub.get("pull_count") or 0), "star_count": int(hub.get("star_count") or 0)}
    except Exception as e:  # noqa: BLE001 -- Docker Hub down must not lose the GitHub half
        print(f"docker hub unreachable: {e}", file=sys.stderr)

    return {
        "date": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d"),
        "github": {
            "total": sum(by_format.values()),
            "assets": assets,
            "by_format": dict(sorted(by_format.items())),
            "by_month": dict(sorted(by_month.items())),
            "by_release": dict(sorted(by_release.items())),
        },
        "docker_hub": docker,
        "traffic": traffic,
    }


def append(series, snap):
    """One entry per date, the newest snapshot for a date winning; sorted by date.

    Clone traffic is a rolling 14-day window, so each snapshot's `traffic` is folded
    into the series' own by-date map (`series[-1]["traffic_by_day"]` carries it forward):
    a day seen in several snapshots keeps the last value, and days older than the
    window survive because they were recorded when they were inside it."""
    entries = {s["date"]: s for s in series if isinstance(s, dict) and "date" in s}
    by_day = {}
    for s in series:
        by_day.update(s.get("traffic_by_day") or {})
        by_day.update(s.get("traffic") or {})
    by_day.update(snap.get("traffic") or {})
    snap = dict(snap)
    snap["traffic_by_day"] = dict(sorted(by_day.items()))
    entries[snap["date"]] = snap
    return [entries[d] for d in sorted(entries)]


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "snapshot":
        json.dump(snapshot(os.environ.get("GITHUB_TOKEN")), sys.stdout, indent=1)
        sys.stdout.write("\n")
    elif cmd == "append" and len(sys.argv) == 4:
        try:
            series = json.load(open(sys.argv[2], encoding="utf-8"))
        except (FileNotFoundError, json.JSONDecodeError):
            series = []
        if not isinstance(series, list):
            series = []
        snap = json.load(open(sys.argv[3], encoding="utf-8"))
        json.dump(append(series, snap), sys.stdout, separators=(",", ":"))
        sys.stdout.write("\n")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
