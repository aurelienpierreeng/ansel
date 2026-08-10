#!/usr/bin/env python3
"""Build the "code health" panel published alongside the Doxygen API docs.

The panel answers one question: how manageable is this codebase, in numbers that
mean the same thing on darktable and on Ansel. It is generated identically in both
repositories so the two published sites can be read side by side.

Inputs, all optional except the first — a missing tool degrades its own section to
"not available" instead of failing the build:

  doc/api/sqlite3/doxygen_sqlite3.db   Doxygen's own symbol table (GENERATE_SQLITE3),
                                       produced by a fast first Doxygen pass. Gives
                                       symbols per file and the include graph.
  lizard                               cyclomatic complexity (CCN) per function.
  cppcheck                             static analysis without needing a build.
  <clang-tidy report>.json/.txt        clang-tidy findings, when a separate job that
                                       can produce compile_commands.json has run.

Outputs:

  doc/code-health.md          a Doxygen page (picked up by INPUT, themed, searchable)
  doc/code-health.json        the same numbers, machine-readable, for cross-repo diffing

Usage:
  python3 tools/code_health.py --project darktable --source-dir src \\
      [--db doc/api/sqlite3/doxygen_sqlite3.db] [--clang-tidy-log FILE]
"""

import argparse
import csv
import json
import os
import re
import shutil
import sqlite3
import subprocess
import sys
from collections import Counter, defaultdict

# Vendored third-party code and dead trees, excluded everywhere so the numbers describe
# the code each repository actually authors. Kept in step with the sonar.exclusions line
# in .sonarcloud.properties.
#
# This is the UNION of what darktable and Ansel each need, so that this file stays
# byte-identical in both repositories and "are the two panels measuring the same thing?"
# is answerable with cmp(1). A path that exists in only one tree costs nothing in the
# other.
EXCLUDED_DIR_PARTS = [
    "/external/",              # both: vendored code that is NOT a submodule either
                               # (lua/, LuaAutoC/, cie_colorimetric_tables.c, ...)
    "/apps/ansel-chart/",      # Ansel: dead code, no build target compiles it
]

# Every git submodule is added to that list at startup, read from .gitmodules rather
# than hardcoded. A submodule is an upstream project pinned at a commit: its
# complexity, its defects and its size belong to whoever wrote it, and counting them
# describes someone else's codebase. Reading the list means it cannot drift when a
# release adds, drops or moves one - which is exactly what happens across a version
# upgrade of the reference tree.


def load_submodule_exclusions(repo_root="."):
    """Extend EXCLUDED_DIR_PARTS with every path declared in .gitmodules."""
    path = os.path.join(repo_root, ".gitmodules")
    found = []
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                line = line.strip()
                if not line.startswith("path"):
                    continue
                _key, _sep, value = line.partition("=")
                value = value.strip().strip("/")
                if value:
                    found.append(value)
    except OSError:
        return []
    added = []
    for sub in found:
        part = "/" + sub.replace(os.sep, "/").strip("/") + "/"
        if part not in EXCLUDED_DIR_PARTS:
            EXCLUDED_DIR_PARTS.append(part)
            added.append(sub)
    return added


def excluded_globs():
    """Shell-glob form of the exclusion list, for tools that filter by pattern.

    Derived on demand, never written out twice, so it always reflects the submodule
    paths loaded from .gitmodules.
    """
    return tuple("*%s*" % part for part in EXCLUDED_DIR_PARTS)

# What counts as production code: an ALLOWLIST, not a list of things to skip.
#
# Only these are compiled into the application and run on a user's machine. Everything
# else in either repository - Python and shell helpers under tools/, YAML workflows,
# CMake and build glue, Markdown documentation, XML and JSON resources - is developer
# or build material. Measuring it reports the health of the toolbox rather than of the
# software, and the two projects keep very differently sized toolboxes, so counting it
# actively distorts the comparison.
#
# An allowlist is deliberate: anything new that appears in either tree is excluded
# until someone decides it ships, rather than silently joining the measurements.
SOURCE_SUFFIXES = (".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hxx", ".m", ".mm")

# cloc identifies languages by content, not only by extension, so a helper script with
# no suffix and a #!/usr/bin/python3 line is still reported as Python. The size section
# therefore filters on cloc's own language name, using the same allowlist idea.
PRODUCTION_LANGUAGES = frozenset((
    "C", "C/C++ Header", "C++", "Objective-C", "Objective-C++",
))


def is_production_file(path):
    """True for a file that is compiled into the shipped application."""
    return path.lower().endswith(SOURCE_SUFFIXES)


def is_excluded(path):
    """True for anything that must not be measured: vendored, dead, or not shipped."""
    p = "/" + path.replace(os.sep, "/").lstrip("/")
    if not is_production_file(p):
        return True
    return any(part in p for part in EXCLUDED_DIR_PARTS)


def run(cmd, **kw):
    """Run a command, returning (ok, stdout). Never raises on a non-zero exit."""
    try:
        r = subprocess.run(cmd, capture_output=True, text=True,
                           errors="replace", check=False, **kw)
        return r.returncode == 0, r.stdout
    except (OSError, subprocess.SubprocessError) as exc:
        return False, str(exc)


# --------------------------------------------------------------------------- symbols


def collect_symbols(db_path):
    """Symbols per file, from Doxygen's SQLite output.

    memberdef.kind is Doxygen's own vocabulary: 'function', 'variable', 'typedef',
    'macro definition', 'enumeration'. Note it is 'macro definition', not 'define'.
    """
    if not db_path or not os.path.exists(db_path):
        return None
    con = sqlite3.connect("file:%s?mode=ro" % db_path, uri=True)
    try:
        rows = con.execute(
            """
            SELECT p.name AS path, m.kind AS kind, COUNT(*) AS n
            FROM memberdef m JOIN path p ON p.rowid = m.file_id
            GROUP BY p.name, m.kind
            """
        ).fetchall()
    except sqlite3.Error as exc:
        sys.stderr.write("code_health: symbol query failed: %s\n" % exc)
        return None
    finally:
        con.close()

    per_file = defaultdict(Counter)
    for path, kind, n in rows:
        if is_excluded(path):
            continue
        per_file[path][kind] += n

    out = []
    for path, kinds in per_file.items():
        out.append(
            {
                "file": path,
                "total": sum(kinds.values()),
                "functions": kinds.get("function", 0),
                "variables": kinds.get("variable", 0),
                "typedefs": kinds.get("typedef", 0),
                "macros": kinds.get("macro definition", 0),
                "enums": kinds.get("enumeration", 0),
            }
        )
    out.sort(key=lambda r: (-r["total"], r["file"]))
    return out


def include_edges(db_path):
    """Every (including file, included file) pair inside this tree.

    Doxygen's `includes` table is the same data its "included by" graphs are drawn
    from, so the numbers derived here and the graphs on the file pages cannot drift
    apart.
    """
    if not db_path or not os.path.exists(db_path):
        return None
    con = sqlite3.connect("file:%s?mode=ro" % db_path, uri=True)
    try:
        tables = {r[0] for r in con.execute(
            "SELECT name FROM sqlite_master WHERE type IN ('table','view')")}
        if "includes" not in tables:
            return None
        # Doxygen's `includes` table links a including file to an included one. Column
        # names have moved between versions, so resolve them rather than assume.
        cols = {r[1] for r in con.execute("PRAGMA table_info(includes)")}
        src = "src_id" if "src_id" in cols else ("including_id" if "including_id" in cols else None)
        dst = "dst_id" if "dst_id" in cols else ("included_id" if "included_id" in cols else None)
        if not src or not dst:
            return None
        # path.local distinguishes files belonging to this tree (1) from system headers
        # resolved outside it (0). Counting <stdlib.h>'s fan-in says nothing about how
        # entangled this codebase is, so restrict both ends to local files.
        rows = con.execute(
            "SELECT ps.name, pd.name FROM includes i "
            "JOIN path ps ON ps.rowid = i.%s JOIN path pd ON pd.rowid = i.%s "
            "WHERE ps.local = 1 AND pd.local = 1" % (src, dst)
        ).fetchall()
    except sqlite3.Error as exc:
        sys.stderr.write("code_health: include query failed: %s\n" % exc)
        return None
    finally:
        con.close()

    return [(a, b) for a, b in rows if not is_excluded(a) and not is_excluded(b)]


def collect_includers(edges):
    """How many files include each header, directly.

    This is the number behind the "included by" graphs: the fan-in of a header, and
    the single clearest measure of how entangled a codebase's headers are.
    """
    if not edges:
        return None
    fan_in = Counter()
    for _including, included in edges:
        fan_in[included] += 1
    return [{"file": f, "included_by": n} for f, n in fan_in.most_common()]


# ----------------------------------------------------------------------- layering


# Declared layer order, low to high. A module may depend on its own layer or on any
# layer BELOW it; an include pointing the other way is a layer inversion.
#
# This is a policy, not a measurement, so it is written down rather than inferred, and
# it is the UNION of both trees' module names so the file stays byte-identical in the
# darktable and Ansel repositories. A directory that exists in only one of them costs
# nothing in the other, and the two projects are therefore judged against exactly the
# same architectural expectation - which is the only way the inversion counts can be
# compared at all.
#
# The order encodes the ordinary layering of a photo editor: freestanding primitives
# know nothing of the application, the pixel pipeline knows nothing of the GUI, and
# entry points sit on top of everything.
LAYERS = {
    # 0 - freestanding primitives: allocation, maths, SIMD pixel helpers
    "system": 0, "math": 0, "pixel": 0,
    # 1 - shared services on top of those primitives.
    #     "ai" is darktable 5.6's ONNX inference backend. It is placed here because
    #     that is what it is used as - src/common/ is its main consumer, alongside
    #     gui/ and lua/ - not because of what it currently depends on. Ansel has no
    #     counterpart, so this rank only ever affects the darktable side.
    "common": 1, "colorprofiles": 1, "ai": 1,
    # 2 - GUI TOOLKIT: custom widgets and drawing primitives. These are LEAF
    #     libraries with no application knowledge - a slider does not know what a
    #     pixel pipeline is - so they sit LOW, next to the other shared services,
    #     and everything with a user interface is entitled to use them. In
    #     particular an IOP has a GUI by definition, so iop -> bauhaus, iop ->
    #     dtgtk and iop -> widgets are ordinary downward dependencies, NOT
    #     inversions. Ranking the toolkit above the pipeline instead (an earlier
    #     mistake here) flagged ~60% of all "inversions" on both codebases, almost
    #     all of them legitimate.
    "dtgtk": 2, "bauhaus": 2, "widgets": 2,
    # 3 - reading and writing images
    "imageio": 3,
    # 4 - the pixel pipeline and image development
    "develop": 4,
    # 5 - pipeline modules, which the pipeline dispatches to
    "iop": 5,
    # 6 - job system and application control
    "control": 6,
    # 7 - the GUI SHELL: main window, panels, accelerators. Unlike the toolkit this
    #     does know about the application, so a module reaching up into it
    #     (iop -> gui) IS an inversion, and a deliberate one to report.
    "gui": 7,
    # 8 - GUI modules and views built on the shell
    "libs": 8, "views": 8,
    # 9 - bindings and side tools built on everything above
    "lua": 9, "chart": 9,
    # 10 - platform glue and entry points
    "apps": 10, "cli": 10, "cltest": 10, "cmstest": 10, "generate-cache": 10,
    "osx": 10, "win": 10, "ppc64le": 10, "tests": 10,
    # 11 - the application-global header itself, where one sits at the source root
    "(root)": 11,
}

UNRANKED_LAYER = None    # modules with no declared rank take part in cycles, not in
                         # the inversion count: ranking them would be inventing policy


def module_of(path, source_dir="src"):
    """The module a file belongs to: its first path component under the source dir."""
    p = path.replace(os.sep, "/")
    marker = "/" + source_dir.strip("/") + "/"
    if p.startswith(source_dir.strip("/") + "/"):
        rest = p[len(source_dir.strip("/")) + 1:]
    elif marker in p:
        rest = p.split(marker, 1)[1]
    else:
        return None
    parts = rest.split("/")
    return parts[0] if len(parts) > 1 else "(root)"


def strongly_connected(nodes, succ):
    """Tarjan's SCC, iterative so a deep include chain cannot blow the stack."""
    index, low, on_stack, stack, comps = {}, {}, set(), [], []
    counter = [0]
    for root in nodes:
        if root in index:
            continue
        work = [(root, iter(succ.get(root, ())))]
        index[root] = low[root] = counter[0]
        counter[0] += 1
        stack.append(root)
        on_stack.add(root)
        while work:
            node, it = work[-1]
            advanced = False
            for nxt in it:
                if nxt not in index:
                    index[nxt] = low[nxt] = counter[0]
                    counter[0] += 1
                    stack.append(nxt)
                    on_stack.add(nxt)
                    work.append((nxt, iter(succ.get(nxt, ()))))
                    advanced = True
                    break
                if nxt in on_stack:
                    low[node] = min(low[node], index[nxt])
            if advanced:
                continue
            work.pop()
            if work:
                low[work[-1][0]] = min(low[work[-1][0]], low[node])
            if low[node] == index[node]:
                comp = []
                while True:
                    w = stack.pop()
                    on_stack.discard(w)
                    comp.append(w)
                    if w == node:
                        break
                comps.append(comp)
    return comps


def derive_layer_order(mod_edges):
    """Derive a layer order from the include graph itself, and measure against it.

    No hand-written ranks. The order is computed, and the modules that break it are
    whatever the computation cannot accommodate.

    The obvious approach - topologically sort the dependency graph - measures nothing:
    a topological order has no backward edges by construction, so it would always
    report zero inversions. It also does not exist here, because the module graph is
    not acyclic (see the cycle counts above).

    So the question is posed the way it actually matters: order the modules so that as
    FEW includes as possible point backwards. The edges still pointing backwards
    afterwards are the minimum set of dependencies that would have to be removed for a
    layering to exist at all - a minimum feedback arc set - and they are the inversions,
    established without anyone declaring anything.

    Computed with the Eades-Lin-Smyth greedy algorithm, weighted by include count:
    repeatedly strip sinks to the back and sources to the front, and when neither
    exists - which is exactly when a cycle is in the way - remove the module with the
    largest outgoing-minus-incoming weight. It runs in linear time and guarantees at
    most |E|/2 - |V|/6 backward edges, which is far better than anything this graph
    needs.

    What this CANNOT do, and why the declared table is still reported next to it: a
    derived order describes the code as it is. If a questionable dependency is
    pervasive enough, the algorithm accommodates it by ordering around it rather than
    flagging it. The declared order describes intent, so it can object to something the
    code does consistently. They answer different questions and disagreeing is useful.
    """
    if not mod_edges:
        return None

    nodes = set()
    out_w, in_w = defaultdict(int), defaultdict(int)
    succ, pred = defaultdict(set), defaultdict(set)
    for (a, b), n in mod_edges.items():
        nodes.add(a)
        nodes.add(b)
        out_w[a] += n
        in_w[b] += n
        succ[a].add(b)
        pred[b].add(a)

    remaining = set(nodes)
    o_w = dict(out_w)
    i_w = dict(in_w)
    o_w.update({n: o_w.get(n, 0) for n in nodes})
    i_w.update({n: i_w.get(n, 0) for n in nodes})

    def drop(u):
        remaining.discard(u)
        for v in succ[u]:
            if v in remaining:
                i_w[v] -= mod_edges.get((u, v), 0)
        for v in pred[u]:
            if v in remaining:
                o_w[v] -= mod_edges.get((v, u), 0)

    head, tail = [], []
    while remaining:
        moved = True
        while moved:
            moved = False
            for u in sorted(remaining):
                if o_w.get(u, 0) == 0:          # sink: nothing depends on it downward
                    tail.append(u)
                    drop(u)
                    moved = True
                    break
            for u in sorted(remaining):
                if u in remaining and i_w.get(u, 0) == 0:   # source: nothing needs it
                    head.append(u)
                    drop(u)
                    moved = True
                    break
        if remaining:
            u = max(sorted(remaining), key=lambda m: o_w.get(m, 0) - i_w.get(m, 0))
            head.append(u)
            drop(u)

    order = head + tail[::-1]
    # A module that depends on nothing sits at the BOTTOM of a layer stack, so the
    # sequence above - which puts the biggest dependers first - is reversed to read
    # the way the declared table does: rank 0 is the foundation.
    order.reverse()
    pos = {m: i for i, m in enumerate(order)}

    back, weighted, total = [], 0, 0
    for (a, b), n in mod_edges.items():
        total += n
        if pos[a] < pos[b]:                     # lower in the derived stack reaching up
            back.append({"pair": "%s -> %s" % (a, b), "includes": n,
                         "from_rank": pos[a], "to_rank": pos[b]})
            weighted += n
    back.sort(key=lambda v: -v["includes"])

    return {
        "order": [{"rank": i, "module": m} for i, m in enumerate(order)],
        "back_edges": len(back),
        "back_includes": weighted,
        "back_ratio": round(100.0 * weighted / max(1, total), 1),
        "worst": back[:20],
    }


def compute_stability(mod_edges):
    """Robert Martin's instability metric, and the violations it implies.

    This exists because LAYERS is hand-written. Someone decided that bauhaus sits
    below iop and that gui sits above it, and a hand-written table can be wrong -
    this one was, ranking the GUI toolkit above the pixel pipeline and flagging
    roughly 60% of both codebases' "inversions" for dependencies that were in fact
    perfectly ordinary. Nothing about the inversion count can catch that, because
    it is measured against the very table in question.

    So this measures the same idea with NO declared order, derived from the graph
    alone, and the two numbers should be read together:

        Ca (afferent)  how many modules depend on this one
        Ce (efferent)  how many modules this one depends on
        I  = Ce / (Ca + Ce)     instability, 0 .. 1

    I = 0 is a module everyone depends on and that depends on nothing: maximally
    stable, expensive to change, and it had better be a leaf library. I = 1 is a
    module nobody depends on: free to change, and it had better be a leaf consumer.

    The Stable Dependencies Principle says a module should only depend on modules
    at least as stable as itself. An edge A -> B with I(A) < I(B) breaks it: the
    harder-to-change module was made to depend on the easier-to-change one, so the
    volatile module's churn propagates into the stable one. That is the same defect
    "layer inversion" is looking for, established without anyone declaring a layer.

    Note the two can legitimately disagree, and where they do is interesting rather
    than wrong: a widely used module that itself reaches into a volatile one scores
    badly here even if the declared layers approve of it.
    """
    if not mod_edges:
        return None
    afferent, efferent = defaultdict(set), defaultdict(set)
    for (a, b) in mod_edges:
        efferent[a].add(b)
        afferent[b].add(a)

    modules = sorted(set(afferent) | set(efferent))
    inst = {}
    for m in modules:
        ca, ce = len(afferent[m]), len(efferent[m])
        inst[m] = (ce / float(ca + ce)) if (ca + ce) else 0.0

    violations, weighted, ranked = [], 0, 0
    for (a, b), n in mod_edges.items():
        ranked += n
        if inst[a] < inst[b] - 1e-9:          # stable depending on less stable
            violations.append({"pair": "%s -> %s" % (a, b), "includes": n,
                               "from_I": round(inst[a], 2), "to_I": round(inst[b], 2)})
            weighted += n
    violations.sort(key=lambda v: -v["includes"])

    table = [{"module": m, "Ca": len(afferent[m]), "Ce": len(efferent[m]),
              "I": round(inst[m], 2)} for m in modules]
    table.sort(key=lambda r: (r["I"], -r["Ca"]))
    return {
        "modules": table,
        "violating_edges": len(violations),
        "violating_includes": weighted,
        "violation_ratio": round(100.0 * weighted / max(1, ranked), 1),
        "worst": violations[:20],
    }


def collect_layering(edges, source_dir="src"):
    """Layer inversions and dependency cycles, at module and at file level.

    Two different questions, deliberately reported side by side:

    - Cycles are OBJECTIVE. If module A depends on B and B depends on A, no layering
      of the two can exist, whatever anyone declares. Counted as strongly connected
      components of the dependency graph.
    - Inversions are POLICY. They count include edges that point from a lower declared
      layer to a higher one, against the LAYERS table above.
    """
    if not edges:
        return None

    mod_edges = Counter()
    file_succ = defaultdict(set)
    files = set()
    for a, b in edges:
        files.add(a)
        files.add(b)
        file_succ[a].add(b)
        ma, mb = module_of(a, source_dir), module_of(b, source_dir)
        if ma and mb and ma != mb:
            mod_edges[(ma, mb)] += 1

    # ---- inversions against the declared order
    inversions, offenders, by_pair = 0, Counter(), Counter()
    ranked_edges = 0
    for (ma, mb), n in mod_edges.items():
        la, lb = LAYERS.get(ma, UNRANKED_LAYER), LAYERS.get(mb, UNRANKED_LAYER)
        if la is None or lb is None:
            continue
        ranked_edges += n
        if la < lb:                       # a lower layer depends on a higher one
            inversions += n
            by_pair[("%s -> %s" % (ma, mb))] += n
            offenders[ma] += n

    # ---- cycles between modules
    mod_succ = defaultdict(set)
    for (ma, mb) in mod_edges:
        mod_succ[ma].add(mb)
    mod_cycles = [sorted(c) for c in strongly_connected(sorted(mod_succ), mod_succ)
                  if len(c) > 1]
    mod_cycles.sort(key=len, reverse=True)

    # ---- cycles between individual files
    file_cycles = [c for c in strongly_connected(sorted(files), file_succ) if len(c) > 1]
    file_cycles.sort(key=len, reverse=True)

    # ---- the two policy-free views: a derived order, and stability
    derived = derive_layer_order(mod_edges)
    stability = compute_stability(mod_edges)

    return {
        "module_edges": len(mod_edges),
        "module_include_count": sum(mod_edges.values()),
        "ranked_include_count": ranked_edges,
        "inversions": inversions,
        "inversion_ratio": round(100.0 * inversions / max(1, ranked_edges), 1),
        "derived": derived,
        "stability": stability,
        "inverted_pairs": by_pair.most_common(25),
        "worst_offenders": offenders.most_common(15),
        "module_cycles": mod_cycles[:15],
        "module_cycle_count": len(mod_cycles),
        "modules_in_cycles": sum(len(c) for c in mod_cycles),
        "file_cycle_count": len(file_cycles),
        "files_in_cycles": sum(len(c) for c in file_cycles),
        "largest_file_cycle": sorted(file_cycles[0]) if file_cycles else [],
    }


def collect_god_header(edges):
    """Who includes the application-global header.

    darktable's src/common/darktable.h and Ansel's src/darktable.h are the same file
    by descent. The number that matters is how many HEADERS include it: a .c doing so
    is a choice local to that file, a .h doing so pushes the whole application into
    every file downstream of it.
    """
    if not edges:
        return None
    target = None
    for _a, b in edges:
        if b.replace(os.sep, "/").endswith("/darktable.h") or b == "darktable.h":
            target = b
            break
    if not target:
        return None
    headers = [a for a, b in edges if b == target and a.endswith((".h", ".hpp"))]
    sources = [a for a, b in edges if b == target and not a.endswith((".h", ".hpp"))]
    return {
        "header": target,
        "included_by_headers": len(headers),
        "included_by_sources": len(sources),
        "total": len(headers) + len(sources),
        "headers": sorted(headers)[:40],
    }


# --------------------------------------------------------------------------- lizard


def collect_ccn(source_dir):
    """Per-function cyclomatic complexity, via lizard.

    The distribution matters more than the total: a codebase's maintenance cost lives
    in its tail, not its mean, so the thresholds below are reported as counts.
    """
    if not shutil.which("lizard"):
        return None
    cmd = ["lizard", "--csv", "-l", "c", "-l", "cpp"]
    for glob in excluded_globs():
        cmd += ["-x", glob]
    cmd.append(source_dir)
    ok, out = run(cmd)
    if not out.strip():
        sys.stderr.write("code_health: lizard produced no output\n")
        return None

    funcs = []
    # Parsed with the csv module, NOT by splitting on commas: lizard quotes the
    # location, file, name and long_name fields, and long_name holds the parameter
    # list, which is full of commas. A naive split also leaves the surrounding
    # quotation marks on the path, so "src/foo.c" no longer ends in .c and every
    # function silently fails the production-file allowlist - which is exactly how
    # this whole section once vanished from the panel without any error.
    for parts in csv.reader(out.splitlines()):
        # nloc,ccn,token,param,length,location,file,name,long_name,start,end
        if len(parts) < 8:
            continue
        try:
            nloc, ccn, _tok, params, length = (int(parts[i]) for i in range(5))
        except ValueError:
            continue  # header row
        path, name = parts[6], parts[7]
        if is_excluded(path):
            continue
        funcs.append(
            {"file": path, "name": name, "ccn": ccn, "nloc": nloc,
             "params": params, "length": length}
        )
    if not funcs:
        return None

    ccns = sorted(f["ccn"] for f in funcs)
    nlocs = [f["nloc"] for f in funcs]

    def pct(p):
        if not ccns:
            return 0
        idx = min(len(ccns) - 1, max(0, int(round((p / 100.0) * (len(ccns) - 1)))))
        return ccns[idx]

    worst = sorted(funcs, key=lambda f: (-f["ccn"], -f["nloc"]))[:40]
    longest = sorted(funcs, key=lambda f: -f["nloc"])[:20]
    return {
        "functions": len(funcs),
        "ccn_total": sum(ccns),
        "ccn_mean": round(sum(ccns) / float(len(ccns)), 2),
        "ccn_median": pct(50),
        "ccn_p90": pct(90),
        "ccn_p99": pct(99),
        "ccn_max": ccns[-1],
        "nloc_total": sum(nlocs),
        "nloc_mean": round(sum(nlocs) / float(len(nlocs)), 1),
        "over_15": sum(1 for c in ccns if c > 15),
        "over_25": sum(1 for c in ccns if c > 25),
        "over_50": sum(1 for c in ccns if c > 50),
        "over_100": sum(1 for c in ccns if c > 100),
        "long_over_100_lines": sum(1 for n in nlocs if n > 100),
        "long_over_300_lines": sum(1 for n in nlocs if n > 300),
        "params_over_7": sum(1 for f in funcs if f["params"] > 7),
        "worst": worst,
        "longest": longest,
    }


# --------------------------------------------------------------------------- cppcheck


def collect_cppcheck(source_dir, jobs):
    """cppcheck findings by severity and by rule id.

    cppcheck is used rather than clang-tidy for the always-on panel because it needs
    no compile_commands.json, so it runs in the docs job on both repositories under
    identical conditions. clang-tidy findings, which need a configured build tree,
    are folded in from --clang-tidy-log when a job that can produce one has run.
    """
    if not shutil.which("cppcheck"):
        return None
    cmd = [
        "cppcheck", "--quiet", "--enable=all", "--inline-suppr",
        "--suppress=missingInclude", "--suppress=missingIncludeSystem",
        "--suppress=unmatchedSuppression", "--suppress=checkersReport",
        "--template={severity}|{id}|{file}",
        "-j", str(jobs),
        source_dir,
    ]
    # cppcheck filters by path prefix. Feed it every excluded directory - submodules
    # included - that actually exists, so no vendored translation unit is analysed.
    for part in EXCLUDED_DIR_PARTS:
        rel = part.strip("/")
        for candidate in (rel, os.path.join(source_dir, os.path.basename(rel))):
            if os.path.isdir(candidate):
                cmd[-1:-1] = ["-i", candidate]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    except (OSError, subprocess.SubprocessError) as exc:
        sys.stderr.write("code_health: cppcheck failed: %s\n" % exc)
        return None

    by_sev, by_id = Counter(), Counter()
    for line in r.stderr.splitlines():          # cppcheck reports on stderr
        parts = line.split("|")
        if len(parts) < 3:
            continue
        sev, rule, path = parts[0], parts[1], parts[2]
        if is_excluded(path):
            continue
        by_sev[sev] += 1
        by_id[rule] += 1
    if not by_sev:
        return None
    return {
        "total": sum(by_sev.values()),
        "by_severity": dict(by_sev.most_common()),
        "top_rules": by_id.most_common(25),
    }


# --------------------------------------------------------------------------- clang-tidy


CLANG_TIDY_LINE = re.compile(
    r"^(?P<file>[^:\s]+):\d+:\d+:\s+(?P<sev>warning|error):"
    r"\s+.*\[(?P<check>[a-zA-Z0-9_.\-,]+)\]\s*$"
)


def collect_clang_tidy(log_path):
    """Aggregate a clang-tidy run's console log by check name.

    Deliberately parses the log rather than running clang-tidy: producing
    compile_commands.json needs a configured build tree and the project's full
    dependency set, which does not belong in the docs job.
    """
    if not log_path or not os.path.exists(log_path):
        return None
    by_check, by_sev, files = Counter(), Counter(), set()
    seen = set()
    with open(log_path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = CLANG_TIDY_LINE.match(line.strip())
            if not m:
                continue
            path = m.group("file")
            if is_excluded(path):
                continue
            # clang-tidy repeats a finding once per translation unit that includes
            # the header it lives in; dedupe on the whole location+check.
            key = line.strip()
            if key in seen:
                continue
            checks = m.group("check").split(",")
            # clang-tidy reports the flags a GCC-oriented build passes that clang does
            # not know as findings. They are about build flags, not about the code, and
            # would otherwise dominate the tally.
            if checks[0] == "clang-diagnostic-unknown-warning-option":
                continue
            seen.add(key)
            # A finding tagged [bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp]
            # is ONE finding reported under a check and its aliases. Count the primary
            # name only, or every aliased check inflates the table three-fold.
            by_check[checks[0]] += 1
            by_sev[m.group("sev")] += 1
            files.add(path)
    if not by_check:
        return None
    return {
        "total": sum(by_sev.values()),
        "files_with_findings": len(files),
        "by_severity": dict(by_sev.most_common()),
        "top_checks": by_check.most_common(25),
    }


# --------------------------------------------------------------------------- cloc


def collect_cloc(source_dir):
    """Lines of code, counted per file and filtered with this module's own predicate.

    cloc's --not-match-d is NOT used to drop vendored code: depending on the cloc
    version it matches a single path component rather than a subtree, so
    src/external/rawspeed/src/... survives a --not-match-d on "external". That went
    unnoticed locally and inflated the published figures to 1,012,392 lines against
    the real 331,243. Counting --by-file and filtering through is_excluded() is the
    only way this section agrees with every other section of the panel.
    """
    if not shutil.which("cloc"):
        return None
    ok, out = run(["cloc", "--quiet", "--json", "--by-file", source_dir])
    if not ok or not out.strip():
        return None
    try:
        data = json.loads(out)
    except ValueError:
        return None
    data.pop("header", None)
    data.pop("SUM", None)

    totals = Counter()
    per_lang = defaultdict(Counter)
    for path, v in data.items():
        lang = v.get("language", "unknown")
        # Two independent gates. is_excluded() drops vendored and non-shipping paths;
        # the language allowlist additionally catches files cloc classifies by content
        # rather than by extension - a suffixless helper with a python shebang, for
        # instance - which no path rule can see.
        if lang not in PRODUCTION_LANGUAGES or is_excluded(path):
            continue
        for key in ("blank", "comment", "code"):
            totals[key] += v.get(key, 0)
            per_lang[lang][key] += v.get(key, 0)
        totals["nFiles"] += 1
        per_lang[lang]["nFiles"] += 1
    if not totals:
        return None

    langs = sorted(
        ({"language": k, **dict(v)} for k, v in per_lang.items()),
        key=lambda d: -d.get("code", 0),
    )[:12]
    return {"sum": dict(totals), "languages": langs}


# --------------------------------------------------------------------------- report


def md_table(headers, rows, aligns=None):
    if not rows:
        return "_no data_\n"
    aligns = aligns or ["---"] * len(headers)
    out = ["| " + " | ".join(headers) + " |", "| " + " | ".join(aligns) + " |"]
    for r in rows:
        out.append("| " + " | ".join(str(c) for c in r) + " |")
    return "\n".join(out) + "\n"


def build_markdown(project, data):
    L = []
    A = L.append
    A("Code health {#code_health}")
    A("============")
    A("")
    A("Generated with `tools/code_health.py` during the documentation build. The same")
    A("script, with the same thresholds and the same third-party exclusions, runs in the")
    A("Ansel repository and in the frozen darktable 5.0 reference tree, so the two")
    A("published panels can be read side by side.")
    A("")
    A("Vendored code is excluded throughout (`src/external/`, the integration-test data,")
    A("the Doxygen theme), matching `sonar.exclusions` in `.sonarcloud.properties`. Every")
    A("number below therefore describes code this repository actually authors.")
    A("")

    # ---- size
    cloc = data.get("cloc")
    A("[TOC]")
    A("")
    A("Size {#ch_size}")
    A("----")
    A("")
    if cloc and cloc.get("sum"):
        s = cloc["sum"]
        A(md_table(
            ["Measure", "Value"],
            [["Files", "{:,}".format(int(s.get("nFiles", 0)))],
             ["Lines of code", "{:,}".format(int(s.get("code", 0)))],
             ["Comment lines", "{:,}".format(int(s.get("comment", 0)))],
             ["Blank lines", "{:,}".format(int(s.get("blank", 0)))],
             ["Comment ratio", "{:.1f} %".format(
                 100.0 * s.get("comment", 0) / max(1, s.get("code", 0) + s.get("comment", 0)))]],
            ["---", "--:"]))
        A("")
        A(md_table(
            ["Language", "Files", "Code", "Comment"],
            [[l["language"], "{:,}".format(l.get("nFiles", 0)),
              "{:,}".format(l.get("code", 0)), "{:,}".format(l.get("comment", 0))]
             for l in cloc["languages"]],
            ["---", "--:", "--:", "--:"]))
    else:
        A("_cloc not available._")
    A("")

    # ---- complexity
    ccn = data.get("ccn")
    A("Cyclomatic complexity {#ch_ccn}")
    A("---------------------")
    A("")
    if ccn:
        A("Per-function CCN, measured by `lizard`. The mean is the least interesting number")
        A("here: maintenance cost lives in the tail, so the counts above each threshold are")
        A("what to compare.")
        A("")
        A(md_table(
            ["Measure", "Value"],
            [["Functions", "{:,}".format(ccn["functions"])],
             ["Total CCN", "{:,}".format(ccn["ccn_total"])],
             ["Mean CCN", ccn["ccn_mean"]],
             ["Median CCN", ccn["ccn_median"]],
             ["90th percentile", ccn["ccn_p90"]],
             ["99th percentile", ccn["ccn_p99"]],
             ["Maximum CCN", ccn["ccn_max"]],
             ["Mean function length (NLOC)", ccn["nloc_mean"]]],
            ["---", "--:"]))
        A("")
        total = float(max(1, ccn["functions"]))
        A(md_table(
            ["Threshold", "Functions", "Share"],
            [["CCN > 15 (worth refactoring)", "{:,}".format(ccn["over_15"]),
              "{:.1f} %".format(100 * ccn["over_15"] / total)],
             ["CCN > 25 (hard to test)", "{:,}".format(ccn["over_25"]),
              "{:.1f} %".format(100 * ccn["over_25"] / total)],
             ["CCN > 50", "{:,}".format(ccn["over_50"]),
              "{:.1f} %".format(100 * ccn["over_50"] / total)],
             ["CCN > 100", "{:,}".format(ccn["over_100"]),
              "{:.1f} %".format(100 * ccn["over_100"] / total)],
             ["Longer than 100 lines", "{:,}".format(ccn["long_over_100_lines"]),
              "{:.1f} %".format(100 * ccn["long_over_100_lines"] / total)],
             ["Longer than 300 lines", "{:,}".format(ccn["long_over_300_lines"]),
              "{:.1f} %".format(100 * ccn["long_over_300_lines"] / total)],
             ["More than 7 parameters", "{:,}".format(ccn["params_over_7"]),
              "{:.1f} %".format(100 * ccn["params_over_7"] / total)]],
            ["---", "--:", "--:"]))
        A("")
        A("### Most complex functions {#ch_ccn_worst}")
        A("")
        A(md_table(
            ["CCN", "NLOC", "Params", "Function", "File"],
            [[f["ccn"], f["nloc"], f["params"], "`%s`" % f["name"], f["file"]]
             for f in ccn["worst"]],
            ["--:", "--:", "--:", "---", "---"]))
        A("")
        A("### Longest functions {#ch_ccn_longest}")
        A("")
        A(md_table(
            ["NLOC", "CCN", "Function", "File"],
            [[f["nloc"], f["ccn"], "`%s`" % f["name"], f["file"]]
             for f in ccn["longest"]],
            ["--:", "--:", "---", "---"]))
    else:
        A("_lizard not available._")
    A("")

    # ---- layering
    lay = data.get("layering")
    god = data.get("god_header")
    A("Layering {#ch_layering}")
    A("--------")
    A("")
    if lay:
        A("Two different questions, reported side by side.")
        A("")
        A("**Cycles are objective.** If module A depends on B and B depends on A, no")
        A("layering of the two exists, whatever anyone declares. Every cycle is a set of")
        A("modules that can only be understood, built and reasoned about as one unit.")
        A("")
        A("**Inversions come in two flavours**, reported separately below: measured")
        A("against an order DERIVED from the graph, and against one DECLARED by hand.")
        A("For the declared one, an include `A -> B` counts as an inversion when")
        A("`rank(A) < rank(B)`: something lower in the stack reaching *up*. The ranks are")
        A("declared below, are identical in both repositories, and are the whole of the")
        A("policy - there is nothing else to the calculation.")
        A("")
        A("Two consequences worth stating, because they are easy to get backwards:")
        A("")
        A("- The GUI **toolkit** (`bauhaus`, `dtgtk`, `widgets`) sits LOW. A slider does not")
        A("  know what a pixel pipeline is, so it is a leaf library and anything with a user")
        A("  interface may use it. An IOP has a GUI by definition, so `iop -> bauhaus` is an")
        A("  ordinary downward dependency and is **not** counted.")
        A("- The GUI **shell** (`gui`) sits HIGH, because it does know about the application.")
        A("  So `iop -> gui` **is** counted: a pipeline module reaching into the main window,")
        A("  panels and accelerators is the coupling this metric exists to find.")
        A("")
        dv = lay.get("derived")
        if dv:
            A("### Derived layer order {#ch_layering_derived}")
            A("")
            A("This order is COMPUTED from the include graph, with nothing declared by hand.")
            A("")
            A("Topologically sorting the graph would measure nothing - a topological order has")
            A("no backward edges by construction - and no such order exists here anyway, since")
            A("the module graph is not acyclic. So the question is posed the way it matters:")
            A("order the modules so that as few includes as possible point backwards. What")
            A("still points backwards is the minimum set of dependencies that would have to")
            A("go for a layering to exist at all, computed with the Eades-Lin-Smyth algorithm")
            A("weighted by include count.")
            A("")
            A("Rank 0 is the foundation:")
            A("")
            A("> " + " &lt; ".join("`%s`" % m["module"] for m in dv["order"]))
            A("")
            A(md_table(
                ["Measure", "Value"],
                [["Dependencies pointing backwards", "{:,}".format(dv["back_edges"])],
                 ["Includes on them", "{:,}".format(dv["back_includes"])],
                 ["Share of cross-module includes", "{} %".format(dv["back_ratio"])]],
                ["---", "--:"]))
            A("")
            if dv["worst"]:
                A(md_table(
                    ["Includes", "Points backwards", "From rank", "To rank"],
                    [[v["includes"], "`%s`" % v["pair"], v["from_rank"], v["to_rank"]]
                     for v in dv["worst"]],
                    ["--:", "---", "--:", "--:"]))
                A("")
            A("What this cannot do, and why the declared order is still reported below: a")
            A("derived order describes the code as it is. A questionable dependency that is")
            A("pervasive enough gets accommodated by ordering around it rather than flagged -")
            A("which is why the two disagree about `control` and `common` on this tree. A")
            A("declared order describes intent, so it can object to something the code does")
            A("consistently. They answer different questions.")
            A("")
        A("### Declared layer order {#ch_layering_table}")
        A("")
        by_rank = defaultdict(list)
        for mod, rank in LAYERS.items():
            by_rank[rank].append(mod)
        A(md_table(
            ["Rank", "Modules"],
            [[r, ", ".join("`%s`" % m for m in sorted(by_rank[r]))]
             for r in sorted(by_rank)],
            ["--:", "---"]))
        A("")
        A("A module with no rank - anything not listed - takes part in the cycle detection")
        A("but is left out of the inversion count, because ranking it would be inventing")
        A("policy rather than applying it.")
        A("")
        st = lay.get("stability")
        if st:
            A("### The same question without a declared order {#ch_layering_stability}")
            A("")
            A("The ranks above are hand-written, and a hand-written table can be wrong: this")
            A("one was, and the inversion count could not possibly have caught it, being")
            A("measured against that very table. So the same defect is also measured here")
            A("with no declared order at all, from the dependency graph alone.")
            A("")
            A("For each module, `Ca` counts the modules that depend on it and `Ce` the modules")
            A("it depends on. Instability is `I = Ce / (Ca + Ce)`. `I = 0` means everyone")
            A("depends on it and it depends on nothing - maximally stable, expensive to")
            A("change. `I = 1` means nobody depends on it - free to change.")
            A("")
            A("The Stable Dependencies Principle says a module should depend only on modules")
            A("at least as stable as itself. An edge `A -> B` with `I(A) < I(B)` breaks it:")
            A("the harder-to-change module was made to depend on the easier-to-change one, so")
            A("the volatile module's churn propagates into the stable one.")
            A("")
            A(md_table(
                ["Measure", "Value"],
                [["Edges breaking the principle", "{:,}".format(st["violating_edges"])],
                 ["Includes on those edges", "{:,}".format(st["violating_includes"])],
                 ["Share of cross-module includes", "{} %".format(st["violation_ratio"])]],
                ["---", "--:"]))
            A("")
            A("Where this and the declared order disagree is informative rather than wrong: a")
            A("widely used module that itself reaches into a volatile one scores badly here")
            A("even when the declared layers approve of it.")
            A("")
            A(md_table(
                ["Includes", "Stable depends on less stable", "I(from)", "I(to)"],
                [[v["includes"], "`%s`" % v["pair"], v["from_I"], v["to_I"]]
                 for v in st["worst"]],
                ["--:", "---", "--:", "--:"]))
            A("")
            A("#### Module stability {#ch_layering_stability_table}")
            A("")
            A("Sorted most stable first. A module near the top is one the rest of the")
            A("codebase rests on, and is the most expensive place for a defect to live.")
            A("")
            A(md_table(
                ["Module", "Ca", "Ce", "I"],
                [["`%s`" % r["module"], r["Ca"], r["Ce"], r["I"]] for r in st["modules"]],
                ["---", "--:", "--:", "--:"]))
            A("")
        A(md_table(
            ["Measure", "Value"],
            [["Cross-module include edges", "{:,}".format(lay["module_edges"])],
             ["Cross-module includes", "{:,}".format(lay["module_include_count"])],
             ["Layer inversions", "{:,}".format(lay["inversions"])],
             ["Inversion ratio", "{} %".format(lay["inversion_ratio"])],
             ["Module dependency cycles", "{:,}".format(lay["module_cycle_count"])],
             ["Modules caught in a cycle", "{:,}".format(lay["modules_in_cycles"])],
             ["File include cycles", "{:,}".format(lay["file_cycle_count"])],
             ["Files caught in a cycle", "{:,}".format(lay["files_in_cycles"])]],
            ["---", "--:"]))
        A("")
        if lay["inverted_pairs"]:
            A("### Inverted dependencies {#ch_layering_inv}")
            A("")
            A(md_table(["Includes", "Lower layer depends on higher"],
                       [["{:,}".format(n), "`%s`" % p] for p, n in lay["inverted_pairs"]],
                       ["--:", "---"]))
            A("")
        if lay["module_cycles"]:
            A("### Module dependency cycles {#ch_layering_cycles}")
            A("")
            A(md_table(["Modules", "Cycle"],
                       [[len(c), ", ".join("`%s`" % m for m in c)]
                        for c in lay["module_cycles"]],
                       ["--:", "---"]))
            A("")
        if lay["largest_file_cycle"]:
            A("### Largest file include cycle {#ch_layering_filecycle}")
            A("")
            A("%d files that mutually include one another, directly or transitively:"
              % len(lay["largest_file_cycle"]))
            A("")
            for f in lay["largest_file_cycle"][:40]:
                A("- `%s`" % f)
            A("")
    else:
        A("_include data not available (needs Doxygen's SQLite output)._")
        A("")
    if god:
        A("### The application-global header {#ch_layering_god}")
        A("")
        A("`%s` is the header every fork of this codebase inherits. A `.c` including it"
          % god["header"])
        A("is a choice local to that file; a **header** including it pushes the whole")
        A("application into every file downstream, which is how an include graph stops")
        A("being a graph and becomes a mesh.")
        A("")
        A(md_table(
            ["Included by", "Count"],
            [["Headers", "{:,}".format(god["included_by_headers"])],
             ["Source files", "{:,}".format(god["included_by_sources"])],
             ["Total", "{:,}".format(god["total"])]],
            ["---", "--:"]))
        A("")
        if god["headers"]:
            A("Headers that include it:")
            A("")
            for h in god["headers"]:
                A("- `%s`" % h)
            A("")

    # ---- coupling
    inc = data.get("includers")
    A("Header coupling {#ch_coupling}")
    A("---------------")
    A("")
    if inc:
        A("Direct fan-in: how many files include each header. This is the number behind the")
        A('"included by" graphs on each file page, and the clearest single measure of how')
        A("entangled the headers are. A header near the top of this table cannot be changed")
        A("without rebuilding, and re-reviewing, most of the codebase.")
        A("")
        A(md_table(
            ["Included by", "Header"],
            [[r["included_by"], r["file"]] for r in inc[:40]],
            ["--:", "---"]))
    else:
        A("_include data not available (needs Doxygen's SQLite output)._")
    A("")

    # ---- symbols
    sym = data.get("symbols")
    A("Symbols per file {#ch_symbols}")
    A("----------------")
    A("")
    if sym:
        tot = sum(r["total"] for r in sym)
        A("From Doxygen's own symbol table. A file with a very large symbol count is doing")
        A("more than one job; a header with one is an interface.")
        A("")
        A(md_table(
            ["Measure", "Value"],
            [["Files with symbols", "{:,}".format(len(sym))],
             ["Symbols total", "{:,}".format(tot)],
             ["Mean per file", "{:.1f}".format(tot / float(max(1, len(sym))))],
             ["Files with > 100 symbols", "{:,}".format(sum(1 for r in sym if r["total"] > 100))],
             ["Files with > 50 symbols", "{:,}".format(sum(1 for r in sym if r["total"] > 50))]],
            ["---", "--:"]))
        A("")
        A("### Largest interfaces {#ch_symbols_top}")
        A("")
        A(md_table(
            ["Symbols", "Functions", "Variables", "Typedefs", "Macros", "Enums", "File"],
            [[r["total"], r["functions"], r["variables"], r["typedefs"],
              r["macros"], r["enums"], r["file"]] for r in sym[:60]],
            ["--:", "--:", "--:", "--:", "--:", "--:", "---"]))
        A("")
        A("The complete per-file table is in `code-health.json`, published next to this page.")
    else:
        A("_symbol data not available (needs Doxygen's SQLite output)._")
    A("")

    # ---- static analysis
    A("Static analysis {#ch_static}")
    A("---------------")
    A("")
    cpp = data.get("cppcheck")
    A("### cppcheck {#ch_cppcheck}")
    A("")
    if cpp:
        A(md_table(["Severity", "Findings"],
                   [[k, "{:,}".format(v)] for k, v in cpp["by_severity"].items()],
                   ["---", "--:"]))
        A("")
        A(md_table(["Findings", "Rule"],
                   [["{:,}".format(n), "`%s`" % r] for r, n in cpp["top_rules"]],
                   ["--:", "---"]))
    else:
        A("_cppcheck not available._")
    A("")
    ct = data.get("clang_tidy")
    A("### clang-tidy {#ch_clang_tidy}")
    A("")
    if ct:
        A(md_table(["Measure", "Value"],
                   [["Findings", "{:,}".format(ct["total"])],
                    ["Files with findings", "{:,}".format(ct["files_with_findings"])]],
                   ["---", "--:"]))
        A("")
        A(md_table(["Findings", "Check"],
                   [["{:,}".format(n), "`%s`" % c] for c, n in ct["top_checks"]],
                   ["--:", "---"]))
    else:
        A("_No clang-tidy report was supplied to this build. clang-tidy needs a configured")
        A("build tree (`compile_commands.json`), which the documentation job does not")
        A("produce; the separate code-health workflow supplies it when it has run._")
    A("")
    A("Elsewhere {#ch_elsewhere}")
    A("---------")
    A("")
    A("SonarCloud carries the findings this panel does not: rule-level issues, duplication,")
    A("cognitive complexity and technical debt, with the same third-party exclusions.")
    A("")
    A("- darktable 5.0: <https://sonarcloud.io/project/overview?id=aurelienpierreeng_darktable-5>")
    A("- Ansel: <https://sonarcloud.io/project/overview?id=aurelienpierreeng_ansel>")
    A("")
    return "\n".join(L) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--project", default="project")
    ap.add_argument("--source-dir", default="src")
    ap.add_argument("--repo-root", default=".",
                    help="where to read .gitmodules from (default: cwd)")
    ap.add_argument("--db", default="doc/api/sqlite3/doxygen_sqlite3.db")
    ap.add_argument("--clang-tidy-log", default=None)
    ap.add_argument("--out-md", default="doc/code-health.md")
    ap.add_argument("--out-json", default="doc/code-health.json")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2)))
    ap.add_argument("--skip-cppcheck", action="store_true")
    args = ap.parse_args()

    added = load_submodule_exclusions(args.repo_root)
    sys.stderr.write("code_health: excluding %d submodule(s): %s\n"
                     % (len(added), ", ".join(added) or "none"))

    data = {"project": args.project, "excluded_submodules": added}

    def step(name, fn):
        sys.stderr.write("code_health: %s ... " % name)
        sys.stderr.flush()
        try:
            v = fn()
        except Exception as exc:                      # never fail the docs build
            sys.stderr.write("failed (%s)\n" % exc)
            return None
        sys.stderr.write("ok\n" if v else "unavailable\n")
        return v

    data["cloc"] = step("cloc", lambda: collect_cloc(args.source_dir))
    data["ccn"] = step("lizard", lambda: collect_ccn(args.source_dir))
    data["symbols"] = step("symbols", lambda: collect_symbols(args.db))
    edges = step("includes", lambda: include_edges(args.db))
    data["includers"] = step("fan-in", lambda: collect_includers(edges))
    data["layering"] = step("layering", lambda: collect_layering(edges, args.source_dir))
    data["god_header"] = step("global header", lambda: collect_god_header(edges))
    if not args.skip_cppcheck:
        data["cppcheck"] = step("cppcheck",
                                lambda: collect_cppcheck(args.source_dir, args.jobs))
    data["clang_tidy"] = step("clang-tidy", lambda: collect_clang_tidy(args.clang_tidy_log))

    for path, payload in ((args.out_json, json.dumps(data, indent=1, sort_keys=True)),
                          (args.out_md, build_markdown(args.project, data))):
        d = os.path.dirname(path)
        if d:
            os.makedirs(d, exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(payload)
        sys.stderr.write("code_health: wrote %s\n" % path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
