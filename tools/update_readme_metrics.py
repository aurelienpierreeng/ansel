#!/usr/bin/env python3
"""Refresh the SonarCloud figures quoted in README.md, in place.

The README compares Ansel against darktable release by release, with every number
linked to the SonarCloud measure it came from. Those numbers go stale silently, and a
stale number in a document meant to be read by people deciding whether to trust the
project is worse than no number at all.

Rather than keeping a second copy of the table here, this reads the README itself.
Every SonarCloud link already names the component it refers to, so the file IS the
specification: the script finds each link, asks SonarCloud what that component measures
today, and rewrites the figure. Descriptions, ordering, footnotes and prose are never
touched, and a row added by hand is picked up on the next run with no change here.

Two cell shapes are recognised:

    [61,373](https://sonarcloud.io/component_measures?metric=complexity&id=PROJECT)
        a single measure; the metric comes from the URL.

    [536](https://sonarcloud.io/...&selected=PROJECT:src/x.c...) / 2206
        the per-file shape used in the comparison tables: cyclomatic complexity,
        then lines of code. The trailing number is updated too. The metric named in
        the URL is IGNORED for these - a few of the hand-written links say
        metric=ncloc while displaying complexity, and the position is what the
        surrounding table promises the reader.

Only public projects are read, over the anonymous API, so this needs no token.

Usage:
  python3 tools/update_readme_metrics.py [--readme README.md] [--check]

  --check  report what would change and exit non-zero if anything is stale, without
           writing. Suitable for CI.
"""

import argparse
import json
import re
import sys
import urllib.parse
import urllib.request

API = "https://sonarcloud.io/api/measures/component"
TREE = "https://sonarcloud.io/api/components/tree"

# [number](sonarcloud url) optionally followed by " / number"
CELL = re.compile(
    r"\[(?P<value>[\d.,]+)(?P<pct>%?)\]\((?P<url>https://sonarcloud\.io/[^)]+)\)"
    r"(?P<tail>\s*/\s*(?P<second>[\d,]+))?"
)


def fetch(component, metrics):
    """Ask SonarCloud for one component's measures. Returns {metric: raw string}."""
    query = urllib.parse.urlencode({"component": component,
                                    "metricKeys": ",".join(sorted(metrics))})
    req = urllib.request.Request(API + "?" + query,
                                 headers={"User-Agent": "ansel-readme-metrics"})
    with urllib.request.urlopen(req, timeout=30) as fh:
        payload = json.load(fh)
    return {m["metric"]: m["value"]
            for m in payload.get("component", {}).get("measures", [])}


def relocate(project, path):
    """Find a file that has moved, by basename, within the same project.

    Ansel reorganises: bauhaus.c went from src/bauhaus/ to src/widgets/, mipmap_cache.c
    to src/caches/, and so on. The README then points at components that 404, and the
    figures beside them quietly stop being refreshed - which is exactly the failure this
    script exists to prevent. Searching the project tree by basename recovers them, but
    only when the answer is unambiguous: two files of the same name are left alone for a
    human to resolve rather than guessed at.
    """
    basename = path.rsplit("/", 1)[-1]
    query = urllib.parse.urlencode({"component": project, "q": basename,
                                    "qualifiers": "FIL", "ps": "10"})
    req = urllib.request.Request(TREE + "?" + query,
                                 headers={"User-Agent": "ansel-readme-metrics"})
    try:
        with urllib.request.urlopen(req, timeout=30) as fh:
            payload = json.load(fh)
    except Exception:                                  # noqa: BLE001
        return None
    hits = [c["key"] for c in payload.get("components", [])
            if c["key"].rsplit("/", 1)[-1] == basename]
    return hits[0] if len(hits) == 1 else None


def component_of(url):
    """The component a measure link points at, and the metric it names."""
    parts = urllib.parse.parse_qs(urllib.parse.urlparse(url).query)
    project = (parts.get("id") or [""])[0]
    selected = (parts.get("selected") or [""])[0]
    metric = (parts.get("metric") or ["complexity"])[0]
    return (selected or project), metric


def format_like(old, value, metric):
    """Render a fresh value the way the README already renders that column."""
    if metric == "comment_lines_density":
        return "%.1f" % float(value)
    n = int(float(value))
    return "{:,}".format(n) if "," in old else str(n)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--readme", default="README.md")
    ap.add_argument("--check", action="store_true",
                    help="report staleness without writing; non-zero exit if stale")
    args = ap.parse_args()

    with open(args.readme, encoding="utf-8") as fh:
        text = fh.read()

    # Collect every component the README refers to, and what it needs from each, so
    # the API is called once per component rather than once per cell.
    wanted = {}
    for m in CELL.finditer(text):
        comp, metric = component_of(m.group("url"))
        if not comp:
            continue
        needs = wanted.setdefault(comp, set())
        if m.group("second"):
            needs.update(("complexity", "ncloc"))
        else:
            needs.add(metric)

    sys.stderr.write("readme-metrics: %d components to refresh\n" % len(wanted))
    measures, failed, moved = {}, [], {}
    for i, (comp, metrics) in enumerate(sorted(wanted.items()), 1):
        try:
            measures[comp] = fetch(comp, metrics)
        except Exception as exc:                       # noqa: BLE001 - report, continue
            found = None
            if ":" in comp:
                project, path = comp.split(":", 1)
                found = relocate(project, path)
            if found:
                try:
                    measures[comp] = fetch(found, metrics)
                    moved[comp] = found
                    sys.stderr.write("readme-metrics: %s moved to %s\n"
                                     % (comp.split(":")[-1], found.split(":")[-1]))
                except Exception as exc2:              # noqa: BLE001
                    failed.append((comp, str(exc2)))
                    measures[comp] = {}
            else:
                failed.append((comp, str(exc)))
                measures[comp] = {}
        if i % 20 == 0:
            sys.stderr.write("readme-metrics:   %d/%d\n" % (i, len(wanted)))

    changes = []

    def replace(m):
        comp, metric = component_of(m.group("url"))
        have = measures.get(comp, {})
        old_value, old_second = m.group("value"), m.group("second")
        # Per-file cells are "complexity / ncloc" by position, whatever the URL says.
        key = "complexity" if old_second else metric
        fresh = have.get(key)
        if fresh is None:
            return m.group(0)
        new_value = format_like(old_value, fresh, key)
        new_second = old_second
        if old_second:
            ncloc = have.get("ncloc")
            if ncloc is not None:
                new_second = format_like(old_second, ncloc, "ncloc")
        if new_value != old_value or new_second != old_second:
            changes.append((comp, key,
                            "%s%s" % (old_value, " / " + old_second if old_second else ""),
                            "%s%s" % (new_value, " / " + new_second if new_second else "")))
        url = m.group("url")
        if comp in moved:
            url = url.replace(urllib.parse.quote(comp, safe=""),
                              urllib.parse.quote(moved[comp], safe=""))
            url = url.replace(comp, moved[comp])
        out = "[%s%s](%s)" % (new_value, m.group("pct"), url)
        if old_second:
            out += " / " + new_second
        return out

    updated = CELL.sub(replace, text)

    for comp, err in failed:
        sys.stderr.write("readme-metrics: WARNING could not read %s (%s)\n" % (comp, err))
    for comp, metric, old, new in changes:
        sys.stderr.write("  %-58s %-22s %s -> %s\n"
                         % (comp.split(":")[-1], metric, old, new))
    sys.stderr.write("readme-metrics: %d figures changed, %d unreadable\n"
                     % (len(changes), len(failed)))

    if args.check:
        return 1 if changes else 0
    if changes:
        with open(args.readme, "w", encoding="utf-8") as fh:
            fh.write(updated)
        sys.stderr.write("readme-metrics: wrote %s\n" % args.readme)
    return 0


if __name__ == "__main__":
    sys.exit(main())
