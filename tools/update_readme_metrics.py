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
import csv
import json
import re
import shutil
import subprocess
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


# A block the script owns entirely, regenerated on every run. The marker carries its
# own specification - which projects, under which column headings, and which directory
# to subtract - so the README stays the single source of truth for what it displays.
BLOCK = re.compile(
    r"(?P<open><!-- BEGIN GENERATED (?P<name>[\w-]+):(?P<spec>[^>]*)-->\n)"
    r"(?P<body>.*?)"
    r"(?P<close><!-- END GENERATED (?P=name) -->)",
    re.DOTALL)


# Engine figures for the Darktable releases, measured once with the tooling below on the
# tagged source trees, and frozen because a release does not change. Ansel's column is
# re-measured on every run from the working tree, which is the only one that moves.
#
# Measured with: lizard (cyclomatic complexity, summed over every function outside
# src/iop) and cloc (lines of code and comment lines, C/C++/Objective-C only), with
# vendored code and git submodules excluded. Reproduce any column with
# tools/code_health.py on the corresponding tag.
FROZEN_ENGINE = {
    "Darktable 3.8": {"tag": "release-3.8.1", "complexity": 35300,
                      "code": 200385, "comment": 28934},
    "Darktable 4.0": {"tag": "release-4.0.0", "complexity": 37212,
                      "code": 207869, "comment": 32107},
    "Darktable 5.0": {"tag": "release-5.0.0", "complexity": 38072,
                      "code": 229813, "comment": 34661},
    "Darktable 5.6": {"tag": "release-5.6.0", "complexity": 44146,
                      "code": 261163, "comment": 41193},
}

# src/external holds the git submodules - rawspeed, LibRaw, sentry-native and the rest -
# which are upstream projects pinned at a commit, not this repository's code. They are
# 64% of the functions under src/ when the submodules are checked out, so leaving them in
# would not skew the figures, it would replace them. A git worktree does not populate
# submodules, which is exactly why this filter must be tested against a full checkout
# rather than assumed to work.
ENGINE_EXCLUDE = ("/external/", "/apps/ansel-chart/", "/iop/",
                  "/tests/integration/", "/image_test/samples/",
                  "/doxygen-awesome-css/")
ENGINE_SUFFIXES = (".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hxx")
ENGINE_LANGUAGES = frozenset(("C", "C/C++ Header", "C++"))


def _engine_excluded(path):
    p = "/" + path.replace("\\", "/").lstrip("/")
    if not p.lower().endswith(ENGINE_SUFFIXES):
        return True
    return any(part in p for part in ENGINE_EXCLUDE)


def measure_engine(source_dir="src"):
    """Measure this working tree's engine with lizard and cloc.

    Returns None if either tool is missing, so the table is left untouched rather than
    written with half of it guessed.
    """
    if not (shutil.which("lizard") and shutil.which("cloc")):
        sys.stderr.write("readme-metrics: lizard and cloc are needed for the engine table\n")
        return None

    cmd = ["lizard", "--csv", "-l", "c", "-l", "cpp"]
    for part in ENGINE_EXCLUDE:
        cmd += ["-x", "*%s*" % part]
    cmd.append(source_dir)
    out = subprocess.run(cmd, capture_output=True, text=True,
                         errors="replace", check=False).stdout
    complexity = 0
    for parts in csv.reader(out.splitlines()):
        # lizard quotes its path fields; csv, never a bare split
        if len(parts) < 8:
            continue
        try:
            ccn = int(parts[1])
        except ValueError:
            continue
        if _engine_excluded(parts[6]):
            continue
        complexity += ccn

    out = subprocess.run(["cloc", "--quiet", "--json", "--by-file", source_dir],
                         capture_output=True, text=True, errors="replace",
                         check=False).stdout
    try:
        data = json.loads(out)
    except ValueError:
        return None
    data.pop("header", None)
    data.pop("SUM", None)
    code = comment = 0
    for path, v in data.items():
        if v.get("language") not in ENGINE_LANGUAGES or _engine_excluded(path):
            continue
        code += v.get("code", 0)
        comment += v.get("comment", 0)
    if not complexity or not code:
        return None
    return {"complexity": complexity, "code": code, "comment": comment}


def engine_table(spec):
    """The engine comparison: local tooling for size and complexity, Sonar for cognitive.

    Comparing the projects as a whole compares their feature sets: the set of pixel
    operations under src/iop has diverged between the forks, and those modules are
    independent of one another, so their bulk says little about maintainability.
    Subtracting them compares the engine, which is what both projects need whatever
    their module set.

    Cyclomatic complexity, lines of code and comment ratio come from ONE tool applied
    identically to every version, because SonarCloud and lizard do not define
    cyclomatic complexity the same way and mixing them silently compares nothing.
    Cognitive complexity has no local equivalent, so it is reported from SonarCloud for
    the three versions that have a project there, and left blank for the rest rather
    than approximated.
    """
    # Each entry is "<sonar project or -> = <column label>". Whether a column is
    # measured locally or read from the frozen table is decided by its label, not by
    # its Sonar key: Ansel is measured locally AND has a Sonar project, and an earlier
    # version of this code used the key to decide both and silently blanked Ansel's
    # cognitive complexity.
    columns, sonar = [], {}
    for item in spec.split(","):
        item = item.strip()
        if not item or item.startswith("exclude="):
            continue
        key, _, label = item.partition("=")
        label = label.strip() or key.strip()
        columns.append(label)
        key = key.strip()
        sonar[label] = key if key and key != "-" else None

    local = measure_engine()
    if local is None:
        raise RuntimeError("local engine measurement unavailable")

    data = {}
    for label in columns:
        if label in FROZEN_ENGINE:
            data[label] = dict(FROZEN_ENGINE[label])
        else:
            data[label] = dict(local)          # the tree this script is running in
        key = sonar.get(label)
        cog = None
        if key:
            try:
                total = fetch(key, ["cognitive_complexity"])
                part = fetch("%s:src/iop" % key, ["cognitive_complexity"])
                cog = (int(float(total.get("cognitive_complexity", 0)))
                       - int(float(part.get("cognitive_complexity", 0))))
            except Exception:                  # noqa: BLE001 - blank beats a wrong number
                cog = None
        data[label]["cognitive"] = cog

    def ratio(d):
        return "%.1f %%" % (100.0 * d["comment"] / max(1, d["comment"] + d["code"]))

    rows = [("Cyclomatic complexity", lambda d: "{:,}".format(d["complexity"])),
            ("Lines of code", lambda d: "{:,}".format(d["code"])),
            ("Ratio of comments", ratio),
            ("Cognitive complexity",
             lambda d: "{:,}".format(d["cognitive"]) if d["cognitive"] else "—")]
    out = ["| Metric | " + " | ".join(columns) + " |",
           "| ------ | " + " | ".join("-----------:" for _ in columns) + " |"]
    for label, render in rows:
        out.append("| " + label + " | " + " | ".join(render(data[c]) for c in columns) + " |")
    return "\n".join(out) + "\n"


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

    def regenerate(m):
        if m.group("name") != "engine-metrics":
            return m.group(0)
        try:
            body = engine_table(m.group("spec"))
        except Exception as exc:                       # noqa: BLE001
            sys.stderr.write("readme-metrics: engine table failed (%s), left as is\n" % exc)
            return m.group(0)
        if body.strip() != m.group("body").strip():
            changes.append(("engine-metrics", "generated block", "stale", "refreshed"))
        return m.group("open") + body + m.group("close")

    updated = BLOCK.sub(regenerate, updated)

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
