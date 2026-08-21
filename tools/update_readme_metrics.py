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
import importlib.util
import json
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile
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
    "Darktable 3.8": {"tag": "release-3.8.1", "complexity": 35244,
                      "code": 199820, "comment": 28736},
    "Darktable 4.0": {"tag": "release-4.0.0", "complexity": 37156,
                      "code": 207304, "comment": 31877},
    "Darktable 5.0": {"tag": "release-5.0.0", "complexity": 38016,
                      "code": 229248, "comment": 34431},
    "Darktable 5.6": {"tag": "release-5.6.0", "complexity": 44059,
                      "code": 260318, "comment": 40879},
}

# src/external holds the git submodules - rawspeed, LibRaw, sentry-native and the rest -
# which are upstream projects pinned at a commit, not this repository's code. They are
# 64% of the functions under src/ when the submodules are checked out, so leaving them in
# would not skew the figures, it would replace them. A git worktree does not populate
# submodules, which is exactly why this filter must be tested against a full checkout
# rather than assumed to work.
ENGINE_EXCLUDE = ("/external/", "/apps/ansel-chart/", "/iop/",
                  "/tests/", "/image_test/samples/",
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


def engine_table(spec, previous=""):
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
            # Subtract src/external as well as src/iop. The three projects do NOT
            # configure the same exclusions - aurelienpierre_darktable analyses its
            # vendored submodules while the other two exclude them - so trusting each
            # project's own scope compares different bodies of code. Left uncorrected
            # this inflated Darktable 4.0's engine by 4,242 cyclomatic and 3,810
            # cognitive, enough to reverse its ranking against Ansel and to make the
            # SonarCloud figures appear to contradict the local ones. A component that
            # is already excluded simply 404s and contributes zero.
            try:
                m = ["cognitive_complexity"]
                total = fetch(key, m)
                cog = int(float(total.get("cognitive_complexity", 0)))
                for sub in ("src/iop", "src/external"):
                    try:
                        part = fetch("%s:%s" % (key, sub), m)
                        cog -= int(float(part.get("cognitive_complexity", 0)))
                    except Exception:          # noqa: BLE001 - absent means excluded
                        pass
            except Exception:                  # noqa: BLE001 - blank beats a wrong number
                cog = None
        data[label]["cognitive"] = cog

    def ratio(d):
        return "%.1f %%" % (100.0 * d["comment"] / max(1, d["comment"] + d["code"]))

    # Documentation coverage needs Doxygen's symbol table. When it has not been built,
    # keep whatever the README already shows rather than blanking a real figure.
    kept_docs, kept_other = {}, {}
    for line in (previous or "").splitlines():
        for prefix, store in (("| Functions carrying documentation", kept_docs),
                              ("| Types, constants and macros carrying documentation",
                               kept_other)):
            if line.startswith(prefix):
                cells = [c.strip() for c in line.strip().strip("|").split("|")]
                for label, value in zip(columns, cells[1:]):
                    if value and value != "—":
                        store[label] = value
    live_docs = measure_docs(DOXYGEN_DB[0])
    if live_docs is None:
        # Nothing prepared for us: build the symbol table rather than give up on the row.
        built = build_doxygen_db()
        if built:
            sys.stderr.write("readme-metrics: built a Doxygen symbol table for "
                             "documentation coverage\n")
            live_docs = measure_docs(built)
    for label in columns:
        d = data[label]
        if label in FROZEN_DOCS:
            f = FROZEN_DOCS[label]
            d["docs"] = "%.1f %%" % (100.0 * f["documented"] / f["functions"])
        elif live_docs:
            d["docs"] = "%.1f %%" % (100.0 * live_docs["documented"] / live_docs["functions"])
        else:
            d["docs"] = kept_docs.get(label, "—")
        if label in FROZEN_DOCS_OTHER:
            f = FROZEN_DOCS_OTHER[label]
            d["docs_other"] = "%.1f %%" % (100.0 * f["documented"] / f["symbols"])
        elif live_docs and live_docs.get("other_symbols"):
            d["docs_other"] = "%.1f %%" % (100.0 * live_docs["other_documented"]
                                           / live_docs["other_symbols"])
        else:
            d["docs_other"] = kept_other.get(label, "—")

    rows = [("Cyclomatic complexity", lambda d: "{:,}".format(d["complexity"])),
            ("Lines of code", lambda d: "{:,}".format(d["code"])),
            ("Comment lines", lambda d: "{:,}".format(d["comment"])),
            ("Ratio of comments", ratio),
            ("Cognitive complexity",
             lambda d: "{:,}".format(d["cognitive"]) if d["cognitive"] else "—"),
            ("Functions carrying documentation", lambda d: d["docs"]),
            ("Types, constants and macros carrying documentation",
             lambda d: d["docs_other"])]
    out = ["| Metric | " + " | ".join(columns) + " |",
           "| ------ | " + " | ".join("-----------:" for _ in columns) + " |"]
    for label, render in rows:
        out.append("| " + label + " | " + " | ".join(render(data[c]) for c in columns) + " |")
    return "\n".join(out) + "\n"



# ---------------------------------------------------------------- frozen release data
#
# Everything below describes released Darktable versions, measured once with the tooling
# in this file and in tools/code_health.py on the corresponding tag. A release does not
# change, so re-measuring it on every run would mean keeping four Darktable checkouts
# around to compute constants. Ansel's column is measured live, because it is the only
# one that moves.
#
# Reproduce any of these with:
#     git clone https://github.com/darktable-org/darktable && cd darktable
#     git checkout <tag>
#     python3 <ansel>/tools/code_health.py --source-dir src --repo-root .

# Share of engine functions carrying a documentation comment, from Doxygen's own record.
# Doxygen counts functions differently from lizard - it sees static inline definitions in
# headers, and function-like macros - so these totals do not match the per-function table
# above. Only the ratio is published, for that reason.
FROZEN_DOCS = {
    "Darktable 3.8": {"functions": 9308, "documented": 1997},
    "Darktable 4.0": {"functions": 9600, "documented": 2008},
    "Darktable 5.0": {"functions": 10212, "documented": 2048},
    "Darktable 5.6": {"functions": 11316, "documented": 2228},
}

# Everything that is not a function: types, constants, enumerations and macros. Reported
# separately because the two behave nothing alike - a codebase can explain what its
# functions do while leaving every type and macro bare, and that is what all five of these
# versions do.
FROZEN_DOCS_OTHER = {
    "Darktable 3.8": {"symbols": 6363, "documented": 332},
    "Darktable 4.0": {"symbols": 6695, "documented": 329},
    "Darktable 5.0": {"symbols": 7373, "documented": 349},
    "Darktable 5.6": {"symbols": 8190, "documented": 404},
}

FROZEN_FUNCTIONS = {
    "Darktable 3.8": {"functions": 7242, "mean": 4.87, "max": 194, "over15": 428, "over50": 45},
    "Darktable 4.0": {"functions": 7484, "mean": 4.96, "max": 210, "over15": 456, "over50": 48},
    "Darktable 5.0": {"functions": 7759, "mean": 4.90, "max": 252, "over15": 453, "over50": 48},
    "Darktable 5.6": {"functions": 8691, "mean": 5.07, "max": 249, "over15": 522, "over50": 63},
}

FROZEN_INCLUDES = {
    "Darktable 3.8": {"med_dep": 14.5, "avg_aff": 84, "over25": 32,
                      "cycles": 4, "trapped": 17, "god": 30},
    "Darktable 4.0": {"med_dep": 13.4, "avg_aff": 83, "over25": 31,
                      "cycles": 4, "trapped": 17, "god": 30},
    "Darktable 5.0": {"med_dep": 15.0, "avg_aff": 95, "over25": 34,
                      "cycles": 4, "trapped": 17, "god": 36},
    "Darktable 5.6": {"med_dep": 14.1, "avg_aff": 96, "over25": 32,
                      "cycles": 4, "trapped": 17, "god": 38},
}

# Jaccard similarity, normalised tokens, between released versions only. The cells
# involving Ansel move with Ansel and are recomputed when --darktable-trees is given.
FROZEN_SIMILARITY = {
    ("Darktable 3.8", "Darktable 4.0"): 87.3,
    ("Darktable 3.8", "Darktable 5.6"): 39.6,
    ("Darktable 4.0", "Darktable 5.6"): 43.2,
}

DOXYGEN_DB = [None]          # set from the command line before any table is built

TREE_DIRS = {"Darktable 3.8": "dt38", "Darktable 4.0": "dt40",
             "Darktable 5.0": "dt50", "Darktable 5.6": "dt56"}


def _code_health():
    """Import the sibling analysis module, which owns the include-graph measurement."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "code_health.py")
    spec = importlib.util.spec_from_file_location("code_health", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def measure_functions(source_dir="src"):
    """Per-function complexity of this tree's engine, via lizard."""
    if not shutil.which("lizard"):
        return None
    cmd = ["lizard", "--csv", "-l", "c", "-l", "cpp"]
    for part in ENGINE_EXCLUDE:
        cmd += ["-x", "*%s*" % part]
    cmd.append(source_dir)
    out = subprocess.run(cmd, capture_output=True, text=True,
                         errors="replace", check=False).stdout
    ccns = []
    for parts in csv.reader(out.splitlines()):
        if len(parts) < 8:
            continue
        try:
            ccn = int(parts[1])
        except ValueError:
            continue
        if _engine_excluded(parts[6]):
            continue
        ccns.append(ccn)
    if not ccns:
        return None
    ccns.sort()
    n = len(ccns)
    return {"functions": n, "mean": round(sum(ccns) / n, 2), "max": ccns[-1],
            "over15": sum(1 for c in ccns if c > 15),
            "over50": sum(1 for c in ccns if c > 50)}


def measure_includes(repo_root=".", source_dir="src"):
    """Include-graph exposure of this tree's engine."""
    ch = _code_health()
    ch.EXCLUDED_DIR_PARTS[:] = [p for p in ENGINE_EXCLUDE]
    ch.load_submodule_exclusions(repo_root)
    cwd = os.getcwd()
    os.chdir(repo_root)
    try:
        edges = ch.source_include_edges(".", source_dir) or []
    finally:
        os.chdir(cwd)
    if not edges:
        return None

    succ, pred, nodes = {}, {}, set()
    for a, b in edges:
        succ.setdefault(a, set()).add(b)
        pred.setdefault(b, set()).add(a)
        nodes.update((a, b))

    def closure(adj, start):
        seen, stack = set(), [start]
        while stack:
            u = stack.pop()
            for v in adj.get(u, ()):
                if v not in seen:
                    seen.add(v)
                    stack.append(v)
        seen.discard(start)
        return seen

    n = len(nodes)
    headers = [f for f in nodes if f.lower().endswith((".h", ".hpp", ".hxx"))]
    sources = [f for f in nodes if f not in headers]
    dep = sorted(len(closure(succ, f)) / n * 100 for f in sources)
    aff = [len(closure(pred, h)) for h in headers]
    cycles = [c for c in ch.strongly_connected(sorted(nodes), succ) if len(c) > 1]
    god = len({a for a, b in edges
               if b.endswith("darktable.h") and a.lower().endswith((".h", ".hpp"))})
    return {"med_dep": round(dep[len(dep) // 2], 1),
            "avg_aff": int(round(sum(aff) / max(1, len(aff)))),
            "over25": int(round(100.0 * sum(1 for a in aff if a / n > 0.25) / len(headers))),
            "cycles": len(cycles),
            "trapped": sum(len(c) for c in cycles),
            "god": god}


def build_doxygen_db(doxyfile="doc/Doxyfile"):
    """Produce Doxygen's symbol table, when one has not been built already.

    The documentation build makes this in its first pass, but running this script by
    hand should not require having run that first. Only the SQLite output is asked for -
    no HTML, no graphs - which takes seconds rather than minutes.
    """
    if not (shutil.which("doxygen") and os.path.exists(doxyfile)):
        return None
    out = tempfile.mkdtemp(prefix="readme-metrics-doxygen-")
    with open(doxyfile, encoding="utf-8", errors="replace") as fh:
        config = fh.read()
    config += "\n".join([
        "", "OUTPUT_DIRECTORY = %s" % out,
        "GENERATE_HTML = NO", "GENERATE_AUTOGEN_DEF = NO", "HAVE_DOT = NO",
        "GENERATE_SQLITE3 = YES", "QUIET = YES", "WARNINGS = NO",
        "WARN_IF_UNDOCUMENTED = NO", "WARN_IF_DOC_ERROR = NO",
        "WARN_IF_INCOMPLETE_DOC = NO", ""])
    try:
        subprocess.run(["doxygen", "-"], input=config, text=True,
                       capture_output=True, check=False)
    except (OSError, subprocess.SubprocessError):
        return None
    db = os.path.join(out, "sqlite3", "doxygen_sqlite3.db")
    return db if os.path.exists(db) else None


def measure_docs(db_path):
    """Share of engine functions carrying a documentation comment.

    Reads the SQLite symbol table Doxygen produces (GENERATE_SQLITE3), which the
    documentation build already generates in its first pass, and filters to the engine
    the same way everything else here does. A function counts as documented when Doxygen
    recorded a brief or detailed description for it - that is, when it carries a real
    doc-comment rather than an ordinary one.
    """
    if not db_path or not os.path.exists(db_path):
        return None
    try:
        con = sqlite3.connect("file:%s?mode=ro" % db_path, uri=True)
        rows = con.execute(
            "SELECT p.name, m.kind, "
            "  TRIM(COALESCE(m.briefdescription,'')) || TRIM(COALESCE(m.detaileddescription,'')) "
            "FROM memberdef m JOIN path p ON p.rowid = m.file_id").fetchall()
        con.close()
    except sqlite3.Error:
        return None
    fn_total = fn_doc = other_total = other_doc = 0
    for path, kind, text in rows:
        if _engine_excluded(path):
            continue
        has = bool((text or "").strip())
        if kind == "function":
            fn_total += 1
            fn_doc += 1 if has else 0
        else:
            other_total += 1
            other_doc += 1 if has else 0
    if not fn_total:
        return None
    return {"functions": fn_total, "documented": fn_doc,
            "other_symbols": other_total, "other_documented": other_doc}


def _columns(spec):
    out = []
    for item in spec.split(","):
        item = item.strip()
        if not item or "=" not in item:
            continue
        key, _, label = item.partition("=")
        out.append(label.strip())
    return out


def functions_table(spec):
    """Per-function engine complexity. Ansel measured live, releases frozen."""
    cols = _columns(spec)
    live = measure_functions()
    if live is None:
        raise RuntimeError("lizard unavailable")
    data = {c: dict(FROZEN_FUNCTIONS[c]) if c in FROZEN_FUNCTIONS else dict(live)
            for c in cols}
    rows = [("Functions", lambda d: "{:,}".format(d["functions"])),
            ("Average complexity", lambda d: "%.2f" % d["mean"]),
            ("Worst single function", lambda d: "{:,}".format(d["max"])),
            ("Functions above 15 — awkward to test", lambda d: "{:,}".format(d["over15"])),
            ("Functions above 50 — effectively untestable",
             lambda d: "{:,}".format(d["over50"]))]
    out = ["| Engine only | " + " | ".join(cols) + " |",
           "| ----------- | " + " | ".join("-----------:" for _ in cols) + " |"]
    for label, render in rows:
        out.append("| " + label + " | " + " | ".join(render(data[c]) for c in cols) + " |")
    return "\n".join(out) + "\n"


def includes_table(spec):
    """Include-graph exposure. Ansel measured live, releases frozen."""
    cols = _columns(spec)
    live = measure_includes()
    if live is None:
        raise RuntimeError("include measurement unavailable")
    data = {c: dict(FROZEN_INCLUDES[c]) if c in FROZEN_INCLUDES else dict(live)
            for c in cols}
    rows = [("A source file depends on this share of the engine, median",
             lambda d: "%.1f %%" % d["med_dep"]),
            ("Changing one header forces re-reading this many files, average",
             lambda d: "{:,}".format(d["avg_aff"])),
            ("Headers whose change exposes over a quarter of the engine",
             lambda d: "%d %%" % d["over25"]),
            ("Circular include groups", lambda d: "{:,}".format(d["cycles"])),
            ("Files trapped in those groups", lambda d: "{:,}".format(d["trapped"])),
            ("Headers including the application-wide `darktable.h`",
             lambda d: "{:,}".format(d["god"]))]
    out = ["| Engine only | " + " | ".join(cols) + " |",
           "| ----------- | " + " | ".join("-----------:" for _ in cols) + " |"]
    for label, render in rows:
        out.append("| " + label + " | " + " | ".join(render(data[c]) for c in cols) + " |")
    return "\n".join(out) + "\n"


def similarity_table(spec, trees=None, previous="", source_dir="src"):
    """Upper-triangle similarity matrix.

    Release-to-release cells are frozen. The cells involving Ansel move with Ansel and
    need the Darktable sources to recompute, so they are refreshed only when
    --darktable-trees points at a directory holding dt38/ dt40/ dt50/ dt56/ checkouts.
    Without it the values already in the README are KEPT, not blanked: a table that
    loses real numbers because an optional input was missing is worse than one that is
    slightly out of date, and the omission is reported on stderr either way.
    """
    kept = {}
    for line in (previous or "").splitlines():
        if not line.startswith("| **Ansel**"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        labels = _columns(spec)
        for label, value in zip(labels, cells[1:]):
            if value and value not in ("—", "?"):
                kept[label] = value
    cols = _columns(spec)
    live = {}
    if trees:
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "clone_detect.py")
        spec_cd = importlib.util.spec_from_file_location("clone_detect", path)
        cdm = importlib.util.module_from_spec(spec_cd)
        spec_cd.loader.exec_module(cdm)
        _f, ansel_corpus, _t = cdm.scan(source_dir, 20, 12, True)
        for label in cols:
            sub = TREE_DIRS.get(label)
            if not sub:
                continue
            root = os.path.join(trees, sub, "src")
            if not os.path.isdir(root):
                sys.stderr.write("readme-metrics: no tree for %s at %s\n" % (label, root))
                continue
            _f2, other, _t2 = cdm.scan(root, 20, 12, True)
            j = 100.0 * len(ansel_corpus & other) / max(1, len(ansel_corpus | other))
            live[label] = round(j, 1)

    def cell(a, b):
        if a == b:
            return "—"
        if a == "Ansel" or b == "Ansel":
            other = b if a == "Ansel" else a
            if other in live:
                return "%.1f %%" % live[other]
            return kept.get(other)          # keep what the README already had
        return "%.1f %%" % FROZEN_SIMILARITY.get((a, b), FROZEN_SIMILARITY.get((b, a), 0.0))

    out = ["| | " + " | ".join(cols) + " |",
           "| --- | " + " | ".join("---:" for _ in cols) + " |"]
    for i, a in enumerate(cols):
        cells = []
        for j, b in enumerate(cols):
            value = "" if j < i else cell(a, b)
            if value is None:
                raise RuntimeError(
                    "no value for %s x %s and none in the README; pass --darktable-trees"
                    % (a, b))
            cells.append(value)
        out.append("| **" + a + "** | " + " | ".join(cells) + " |")
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--readme", default="README.md")
    ap.add_argument("--doxygen-db",
                    default="doc/api/sqlite3/doxygen_sqlite3.db",
                    help="Doxygen SQLite symbol table, for documentation coverage; "
                         "produced by the docs build's first pass")
    ap.add_argument("--darktable-trees", default=None,
                    help="directory holding dt38/ dt40/ dt50/ dt56/ Darktable checkouts, "
                         "needed only to refresh the Ansel row of the similarity matrix")
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

    DOXYGEN_DB[0] = args.doxygen_db
    builders = {"engine-metrics": engine_table,
                "engine-complexity": functions_table,
                "engine-includes": includes_table,
                "similarity-matrix": lambda sp, prev: similarity_table(
                    sp, args.darktable_trees, prev)}

    def regenerate(m):
        build = builders.get(m.group("name"))
        if build is None:
            return m.group(0)
        try:
            needs_prev = build in (builders["similarity-matrix"],
                                   builders["engine-metrics"])
            body = (build(m.group("spec"), m.group("body")) if needs_prev
                    else build(m.group("spec")))
        except Exception as exc:                       # noqa: BLE001
            sys.stderr.write("readme-metrics: %s failed (%s), left as is\n"
                             % (m.group("name"), exc))
            return m.group(0)
        if body.strip() != m.group("body").strip():
            changes.append((m.group("name"), "generated block", "stale", "refreshed"))
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
