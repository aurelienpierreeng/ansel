#!/usr/bin/env python3
"""Static analysis of the project's #include graph.

Run from the repository root:  python3 tools/include_graph.py

Builds the DIRECT include graph from source (project includes only), then reports:
  1. cycles (strongly connected components > 1) -- these are exactly what #pragma once
     hides; the guards can only be removed once this section reports none;
  2. layering violations against the declared layer order below;
  3. god-headers by transitive fan-in (how many files rebuild when you touch it);
  4. headers with the largest transitive closure (what including one costs).

It reads sources, not the build, so it needs no compilation and covers every
configuration at once -- including the #ifdef branches your own build does not take.
The trade-off is that it counts includes inside conditional blocks unconditionally.
For a single translation unit's REAL expansion, use the compiler instead:
    gcc -H -fsyntax-only <flags from build/compile_commands.json> file.c
and for unused includes, clang-include-cleaner or include-what-you-use.
"""
import os, re, sys
from collections import defaultdict

SRC = 'src'
INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)

# layer order: a file may include its own layer and any layer BELOW it (higher index = higher level)
# GUI toolkit code (gui/, dtgtk/, bauhaus/) is INFRASTRUCTURE used by modules, so it sits
# below them, not above: an iop's gui_init() legitimately calls gtk/bauhaus helpers.
LAYERS = [
    ('external', 0), ('win', 0), ('system', 0),
    ('common', 1), ('math', 1), ('colorprofiles', 1),
    # pixel/: image-processing primitives (wavelets, guided filters, colour adaptation,
    # interpolation). Above common/ because they are a domain library rather than
    # infrastructure, below control/ because they must never reach the control loop.
    # metadata/: what a photograph says about itself -- EXIF/IPTC/XMP, ratings, colour
    # labels, tags, geotags. Layer 1 measured, not assumed: at layer 2 the move costs +20
    # violations, because its consumers (common/, caches/) sit at 1.
    ('caches', 1), ('database', 1), ('metadata', 1), ('history', 1),
    ('pixel', 2),
    # widgets/: reusable GTK widgets that hold no application state. It was at 4, beside gui/,
    # on the assumption that "GTK" and "the application's GUI" are one layer. They are not:
    # widgets/ depends only on system/, common/, metadata/ and pixel/ (focus_peaking.c reads
    # pixel/eigf.h), so it is a leaf library that happens to be written against GTK, and 2.5 --
    # above pixel/, below control/ -- is where its own dependencies already put it. Measured,
    # not assumed: the move creates ZERO new violations and removes three (control/ -> widgets/),
    # 187 -> 184. At 1.5 it would cost one, because pixel/ would then be above it.
    #
    # It does NOT follow that a lower layer may now use GTK freely. Dependency order and
    # toolkit-freedom are different properties and this table measures only the first; the
    # second is what the Qt port needs and belongs in its own gate.
    ('widgets', 2.5),
    ('control', 3),
    ('gui', 4),
    ('develop', 5),
    ('iop', 6), ('imageio', 6),
    ('libs', 7), ('views', 7), ('chart', 7),
    ('apps', 10),   # executables link the orchestrator, so they sit ABOVE it
    ('app', 9),                       # main.c, darktable.c/h -- directly in src/
]
LAYER = dict(LAYERS)

def layer_of(path):
    parts = path.split(os.sep)
    if len(parts) < 2:
        return None
    # A file directly in src/ (main.c, darktable.c/h) is the application root: the
    # orchestrator sits ABOVE every module, so nothing it includes can be an inversion.
    if len(parts) == 2:
        return LAYER['app']
    return LAYER.get(parts[1])

def collect():
    files = {}
    for root, _, names in os.walk(SRC):
        parts = root.split(os.sep)
        # skip vendored code and archived, non-built directories: neither is part of
        # the program, and counting attic/ made dead code register as live inversions
        if 'external' in parts or 'attic' in parts:
            continue
        for n in names:
            if n.endswith(('.c', '.h', '.cc', '.cpp', '.hpp')):
                p = os.path.join(root, n)
                try:
                    files[p] = open(p, encoding='utf-8', errors='replace').read()
                except OSError:
                    pass
    return files

def resolve(inc, from_path, known):
    # includes are written relative to src/, or occasionally to the including file's dir
    cand = os.path.normpath(os.path.join(SRC, inc))
    if cand in known:
        return cand
    cand2 = os.path.normpath(os.path.join(os.path.dirname(from_path), inc))
    if cand2 in known:
        return cand2
    return None

def tarjan(graph, nodes):
    index, low, on, stack, out, counter = {}, {}, set(), [], [], [0]
    def strong(v):
        work = [(v, 0)]
        while work:
            node, pi = work[-1]
            if pi == 0:
                index[node] = low[node] = counter[0]; counter[0] += 1
                stack.append(node); on.add(node)
            recurse = False
            succs = list(graph.get(node, ()))
            for i in range(pi, len(succs)):
                w = succs[i]
                if w not in index:
                    work[-1] = (node, i + 1); work.append((w, 0)); recurse = True; break
                elif w in on:
                    low[node] = min(low[node], index[w])
            if recurse:
                continue
            if low[node] == index[node]:
                comp = []
                while True:
                    w = stack.pop(); on.discard(w); comp.append(w)
                    if w == node: break
                out.append(comp)
            work.pop()
            if work:
                parent = work[-1][0]
                low[parent] = min(low[parent], low[node])
    for n in nodes:
        if n not in index:
            strong(n)
    return out

def main():
    files = collect()
    known = set(files)
    graph = defaultdict(set)
    for p, text in files.items():
        for inc in INCLUDE_RE.findall(text):
            t = resolve(inc, p, known)
            if t and t != p:
                graph[p].add(t)

    headers = [f for f in files if f.endswith(('.h', '.hpp'))]

    print(f"nodes: {len(files)}  ({len(headers)} headers)   direct edges: {sum(len(v) for v in graph.values())}\n")

    # 1. cycles
    comps = [c for c in tarjan(graph, list(files)) if len(c) > 1]
    print(f"=== 1. INCLUDE CYCLES: {len(comps)} ===")
    for c in sorted(comps, key=len, reverse=True)[:10]:
        print(f"  cycle of {len(c)}:")
        for f in sorted(c)[:8]:
            print(f"     {f}")
        if len(c) > 8: print(f"     ... +{len(c)-8}")
    if not comps:
        print("  none — the include graph is a DAG")

    # 2. layering violations
    print("\n=== 2. LAYERING VIOLATIONS (a file including something from a HIGHER layer) ===")
    viol = defaultdict(list)
    for a, targets in graph.items():
        la = layer_of(a)
        if la is None: continue
        for b in targets:
            lb = layer_of(b)
            if lb is None: continue
            if lb > la:
                viol[(a.split(os.sep)[1], b.split(os.sep)[1])].append((a, b))
    for (da, db), items in sorted(viol.items(), key=lambda kv: -len(kv[1])):
        print(f"  {da}/ -> {db}/ : {len(items)}")
        for a, b in items[:3]:
            print(f"      {a}  ->  {b}")
    if not viol:
        print("  none")

    # 3/4. transitive closures
    memo = {}
    def closure(n, seen=None):
        if n in memo: return memo[n]
        out, stack = set(), [n]
        while stack:
            cur = stack.pop()
            for w in graph.get(cur, ()):
                if w not in out:
                    out.add(w); stack.append(w)
        memo[n] = out
        return out

    fan_in = defaultdict(int)
    for f in files:
        for h in closure(f):
            fan_in[h] += 1
    print("\n=== 3. GOD-HEADERS by transitive fan-in (TUs+headers that pull it in) ===")
    for h, n in sorted(fan_in.items(), key=lambda kv: -kv[1])[:15]:
        if h.endswith(('.h', '.hpp')):
            print(f"  {n:5d}  {h}   (drags in {len(closure(h))} project headers)")

    print("\n=== 4. HEAVIEST HEADERS by transitive closure (what including it costs) ===")
    for h in sorted(headers, key=lambda x: -len(closure(x)))[:12]:
        print(f"  {len(closure(h)):5d}  {h}")

    if '--summary' in sys.argv:
        print("\n=== SUMMARY ===")
        summary(files, graph, headers, closure, comps, viol)
    if '--what-if' in sys.argv:
        what_if(files, graph)
    if '--mermaid' in sys.argv:
        print("\n=== DIRECTORY GRAPH (dotted = layering inversion) ===")
        mermaid(graph, set(viol.keys()))



def what_if(files, graph):
    """Re-count layering violations as if some files lived elsewhere.

    Relocation is cheap to do and expensive to undo, and intuition is unreliable here:
    moving the history/* cluster into develop/ LOOKS obviously right (it calls
    dt_dev_* constantly) and measures at +15 violations, because its own consumers sit
    below develop/. Simulate first.

    Usage:  --what-if src/common/foo.c=develop src/common/foo.h=develop
    """
    moves = {}
    for a in sys.argv:
        if a.startswith('src/') and '=' in a:
            src, dst = a.split('=', 1)
            moves[os.path.normpath(src)] = dst
    if not moves:
        print('  pass moves as src/path/file.c=destdir')
        return

    def home(p, mv):
        p = os.path.normpath(p)
        if p in mv:
            return mv[p]
        parts = p.split(os.sep)
        return parts[1] if len(parts) > 1 else None

    def count(mv):
        n = 0
        for a, targets in graph.items():
            la = LAYER.get(home(a, mv))
            if la is None:
                continue
            for b in targets:
                lb = LAYER.get(home(b, mv))
                if lb is not None and lb > la:
                    n += 1
        return n

    base = count({})
    after = count(moves)
    print('\n=== WHAT-IF ===')
    for s_, d in moves.items():
        print('  %s -> %s/' % (s_, d))
    print('  layering violations %d -> %d  (%+d)' % (base, after, after - base))


def summary(files, graph, headers, closure, comps, viol):
    """One-line-per-metric output, for before/after comparison."""
    orphan_headers = [h for h in headers if not any(h in graph.get(f, ()) for f in files)]
    print(f"nodes\t{len(files)}")
    print(f"headers\t{len(headers)}")
    print(f"direct_edges\t{sum(len(v) for v in graph.values())}")
    print(f"cycles\t{len(comps)}")
    print(f"cycle_nodes\t{sum(len(c) for c in comps)}")
    print(f"layering_violations\t{sum(len(v) for v in viol.values())}")
    print(f"max_closure\t{max((len(closure(h)) for h in headers), default=0)}")
    print(f"mean_closure\t{sum(len(closure(h)) for h in headers) / max(len(headers), 1):.1f}")
    tot = sum(len(closure(f)) for f in files)
    print(f"total_transitive_edges\t{tot}")

    # Per-TU cost is the metric that actually tracks compile-time coupling. The raw
    # totals above are SUMS over all nodes, so splitting one god-header into several
    # small ones inflates them even as every individual file gets cheaper -- use these
    # for before/after comparison instead.
    tus = [f for f in files if f.endswith(('.c', '.cc', '.cpp'))]
    tu_costs = sorted(len(closure(f)) for f in tus)
    if tu_costs:
        print(f"tus\t{len(tus)}")
        print(f"tu_mean_closure\t{sum(tu_costs) / len(tu_costs):.1f}")
        print(f"tu_median_closure\t{tu_costs[len(tu_costs) // 2]}")
        print(f"tu_max_closure\t{tu_costs[-1]}")

    # Counting NODES rewards monoliths: one 1300-line god-header is a single node,
    # while the same content split into 11 honest headers counts as up to 11. Weigh
    # each header by its own line count to measure what the compiler actually eats.
    lines = {}
    for f in files:
        try:
            lines[f] = sum(1 for _ in open(f, encoding='utf-8', errors='replace'))
        except OSError:
            lines[f] = 0
    tu_line_costs = sorted(sum(lines.get(h, 0) for h in closure(f)) for f in tus)
    if tu_line_costs:
        print(f"tu_mean_closure_lines\t{sum(tu_line_costs) // len(tu_line_costs)}")
        print(f"tu_median_closure_lines\t{tu_line_costs[len(tu_line_costs) // 2]}")
        print(f"tu_max_closure_lines\t{tu_line_costs[-1]}")

    # How far the application orchestrator still reaches.
    dt_h = os.path.join(SRC, 'darktable.h')   # the orchestrator now lives at src/
    if dt_h in files:
        reach = sum(1 for f in files if dt_h in closure(f))
        direct = sum(1 for f in files if dt_h in graph.get(f, ()))
        print(f"darktable_h_reach\t{reach}")
        print(f"darktable_h_direct_includers\t{direct}")
        print(f"darktable_h_closure\t{len(closure(dt_h))}")

def mermaid(graph, viol_pairs):
    """Directory-level aggregate, renderable inline in a GitHub comment."""
    agg = defaultdict(int)
    for a, targets in graph.items():
        da = a.split(os.sep)[1] if len(a.split(os.sep)) > 1 else '?'
        for b in targets:
            db = b.split(os.sep)[1] if len(b.split(os.sep)) > 1 else '?'
            if da != db:
                agg[(da, db)] += 1
    print("```mermaid")
    print("graph LR")
    for (a, b), n in sorted(agg.items(), key=lambda kv: -kv[1]):
        if n < 5:
            continue
        bad = (a, b) in viol_pairs
        arrow = "-. %d .->" % n if bad else "-- %d -->" % n
        print(f"  {a} {arrow} {b}")
    print("```")

if __name__ == '__main__':
    main()
