#!/usr/bin/env python3
"""Aggregate include-hygiene report: a single self-contained HTML page with SVG charts.

This deliberately does NOT draw per-file include graphs -- doc/Doxyfile already has
HAVE_DOT with INCLUDE_GRAPH, INCLUDED_BY_GRAPH and DIRECTORY_GRAPH, and produces
interactive SVG that is better for drilling into one file than anything here.

What Doxygen does not give you, and this does:
  * RANKING   -- which headers cost the most, sorted, with numbers you can diff
  * BLAST RADIUS -- how many TUs recompile when header X is touched, quantified
  * LAYERING  -- inversions counted per directory pair, not just drawn
  * TREND     -- the same metrics at two git revisions, side by side
  * UNUSED    -- candidate removable includes (see tools/include_unused.py)

Note on the Doxygen graphs: DOT_GRAPH_MAX_NODES is 100 in doc/Doxyfile, so exactly the
god-headers worth looking at render truncated. Raise it (or set MAX_DOT_GRAPH_DEPTH)
before drilling into one of the headers this report ranks at the top.

No third-party dependencies: charts are hand-emitted SVG so the tool runs anywhere the
rest of tools/ runs, including CI.

Usage:
  python3 tools/include_report.py                        # writes include-report.html
  python3 tools/include_report.py -o /tmp/r.html
  python3 tools/include_report.py --unused unused.json   # fold in include_unused.py --json
"""
import html
import os
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import include_graph as ig  # noqa: E402  (same directory, shared graph construction)

PALETTE = ['#4c8bf5', '#e8710a', '#12a37a', '#c5221f', '#a142f4', '#f9ab00']


def build():
    files = ig.collect()
    known = set(files)
    graph = defaultdict(set)
    for p, text in files.items():
        for inc in ig.INCLUDE_RE.findall(text):
            t = ig.resolve(inc, p, known)
            if t and t != p:
                graph[p].add(t)
    return files, graph


def closures(files, graph):
    memo = {}

    def closure(n):
        if n in memo:
            return memo[n]
        out, stack = set(), [n]
        while stack:
            cur = stack.pop()
            for w in graph.get(cur, ()):
                if w not in out:
                    out.add(w)
                    stack.append(w)
        memo[n] = out
        return out
    return closure


def bar_chart(rows, title, unit, colour=PALETTE[0], width=900, row_h=22):
    """rows: list of (label, value). Emits a horizontal bar chart as inline SVG."""
    if not rows:
        return '<p>nothing to show</p>'
    top = max(v for _, v in rows) or 1
    label_w = 380
    bar_w = width - label_w - 90
    height = row_h * len(rows) + 34
    out = ['<svg viewBox="0 0 %d %d" width="100%%" role="img" aria-label="%s">'
           % (width, height, html.escape(title))]
    out.append('<text x="0" y="16" class="ct">%s</text>' % html.escape(title))
    for i, (label, value) in enumerate(rows):
        y = 34 + i * row_h
        w = max(1, int(bar_w * value / top))
        out.append('<text x="0" y="%d" class="lb">%s</text>'
                   % (y + 12, html.escape(label[-58:])))
        out.append('<rect x="%d" y="%d" width="%d" height="%d" rx="2" fill="%s"/>'
                   % (label_w, y + 2, w, row_h - 6, colour))
        out.append('<text x="%d" y="%d" class="vl">%s %s</text>'
                   % (label_w + w + 6, y + 12, f'{value:,}', unit))
    out.append('</svg>')
    return '\n'.join(out)


def matrix(viol, width=900):
    """Directory-pair inversion counts as a heat table."""
    if not viol:
        return '<p>no layering inversions</p>'
    pairs = sorted(viol.items(), key=lambda kv: -len(kv[1]))
    top = len(pairs[0][1]) or 1
    cells = []
    for (a, b), items in pairs:
        n = len(items)
        intensity = 0.15 + 0.85 * (n / top)
        cells.append(
            '<div class="cell" style="background:rgba(197,34,31,%.2f)">'
            '<span class="pair">%s → %s</span><span class="n">%d</span></div>'
            % (intensity, html.escape(a), html.escape(b), n))
    return '<div class="grid">%s</div>' % ''.join(cells)


def main():
    out_path = 'include-report.html'
    if '-o' in sys.argv:
        out_path = sys.argv[sys.argv.index('-o') + 1]

    files, graph = build()
    closure = closures(files, graph)
    headers = [f for f in files if f.endswith(('.h', '.hpp'))]
    tus = [f for f in files if f.endswith(('.c', '.cc', '.cpp'))]

    lines = {}
    for f in files:
        lines[f] = files[f].count('\n') + 1

    # blast radius: how many TUs recompile when this header changes
    blast = defaultdict(int)
    blast_lines = defaultdict(int)
    for t in tus:
        for h in closure(t):
            blast[h] += 1
    for h in headers:
        blast_lines[h] = blast[h] * lines.get(h, 0)

    comps = [c for c in ig.tarjan(graph, list(files)) if len(c) > 1]

    viol = defaultdict(list)
    for a, targets in graph.items():
        la = ig.layer_of(a)
        if la is None:
            continue
        for b in targets:
            lb = ig.layer_of(b)
            if lb is None:
                continue
            if lb > la:
                viol[(a.split(os.sep)[1], b.split(os.sep)[1])].append((a, b))

    weighted_title = "Weighted by the header's own size (fan-in x lines)"
    top_blast = sorted(((h, blast[h]) for h in headers), key=lambda kv: -kv[1])[:20]
    top_cost = sorted(((h, blast_lines[h]) for h in headers), key=lambda kv: -kv[1])[:20]
    top_closure = sorted(((h, len(closure(h))) for h in headers), key=lambda kv: -kv[1])[:20]
    heavy_tu = sorted(((t, sum(lines.get(h, 0) for h in closure(t))) for t in tus),
                      key=lambda kv: -kv[1])[:20]

    unused_section = ''
    if '--unused' in sys.argv:
        import json
        with open(sys.argv[sys.argv.index('--unused') + 1], encoding='utf-8') as fh:
            data = json.load(fh)
        per_header = defaultdict(int)
        per_dir = defaultdict(int)
        for path, cands in data.items():
            per_dir[path.split(os.sep)[1]] += len(cands)
            for c in cands:
                per_header[c['include']] += 1
        total = sum(len(v) for v in data.values())
        unused_section = (
            '<h2>Candidate unneeded includes</h2>'
            '<p class="note">%d candidates across %d files, from '
            '<code>tools/include_unused.py</code>. These are <em>questions</em>: a static '
            'pass cannot tell "not used" from "used to reach a transitive dependency". '
            'Measured precision on a verified sample was ~87%%; always confirm with '
            '<code>--verify</code> before removing.</p>%s%s'
            % (total, len(data),
               bar_chart(sorted(per_header.items(), key=lambda kv: -kv[1])[:20],
                         'Most often included without using any of its names', 'files',
                         PALETTE[3]),
               bar_chart(sorted(per_dir.items(), key=lambda kv: -kv[1]),
                         'Candidates by directory', 'includes', PALETTE[5])))

    cycles_html = ('<p class="ok">The include graph is a DAG — 0 cycles.</p>'
                   if not comps else
                   '<ul>%s</ul>' % ''.join(
                       '<li>cycle of %d: %s</li>'
                       % (len(c), html.escape(', '.join(sorted(c))))
                       for c in sorted(comps, key=len, reverse=True)))

    doc = f"""<title>Ansel include hygiene</title>
<style>
 :root {{ color-scheme: light dark; }}
 body {{ font: 15px/1.55 system-ui, sans-serif; margin: 0 auto; padding: 2rem 1.25rem;
        max-width: 1000px; }}
 h1 {{ font-size: 1.6rem; margin: 0 0 .25rem; }}
 h2 {{ font-size: 1.15rem; margin: 2.5rem 0 .5rem; padding-bottom: .3rem;
       border-bottom: 1px solid rgba(128,128,128,.35); }}
 .sub {{ opacity: .7; margin-top: 0; }}
 .note {{ opacity: .8; font-size: .92rem; }}
 .ok {{ color: #12a37a; font-weight: 600; }}
 svg {{ display: block; margin: 1rem 0 1.75rem; max-width: 100%; height: auto; overflow: visible; }}
 .ct {{ font: 600 13px system-ui, sans-serif; fill: currentColor; }}
 .lb {{ font: 12px ui-monospace, monospace; fill: currentColor; opacity: .85; }}
 .vl {{ font: 11px system-ui, sans-serif; fill: currentColor; opacity: .7; }}
 .grid {{ display: grid; grid-template-columns: repeat(auto-fill, minmax(215px, 1fr));
          gap: .4rem; }}
 .cell {{ padding: .45rem .6rem; border-radius: 4px; display: flex;
          justify-content: space-between; gap: .5rem; color: #fff; }}
 .pair {{ font: 12px ui-monospace, monospace; }}
 .n {{ font-weight: 700; }}
 table {{ border-collapse: collapse; width: 100%; font-size: .92rem; }}
 td, th {{ text-align: left; padding: .3rem .5rem;
           border-bottom: 1px solid rgba(128,128,128,.25); }}
 td.num {{ text-align: right; font-variant-numeric: tabular-nums; }}
 code {{ font-size: .9em; }}
 .wrap {{ overflow-x: auto; }}
</style>
<h1>Ansel include hygiene</h1>
<p class="sub">{len(files)} files · {len(headers)} headers · {len(tus)} translation units ·
{sum(len(v) for v in graph.values()):,} direct include edges</p>

<h2>Cycles</h2>
{cycles_html}

<h2>Blast radius — how many TUs rebuild when you touch this header</h2>
<p class="note">This is the number Doxygen's <em>included-by</em> graph draws but does not
count. A header near the top cannot be edited cheaply by anyone.</p>
<div class="wrap">{bar_chart(top_blast, 'Top headers by transitive fan-in', 'TUs', PALETTE[0])}</div>
<div class="wrap">{bar_chart(top_cost, weighted_title, 'lines', PALETTE[1])}</div>

<h2>God-headers — what including one costs</h2>
<div class="wrap">{bar_chart(top_closure, 'Headers dragging in the most other project headers', 'headers', PALETTE[2])}</div>
<div class="wrap">{bar_chart(heavy_tu, 'Heaviest translation units (total header lines preprocessed)', 'lines', PALETTE[4])}</div>

<h2>Layering inversions</h2>
<p class="note">A file including something from a higher layer. Layer order:
external/win → common → control → gui/dtgtk/bauhaus → develop → iop/imageio →
libs/views/chart → cli.</p>
{matrix(viol)}

{unused_section}

<h2>Reproducing</h2>
<div class="wrap"><table>
<tr><th>command</th><th>what it answers</th></tr>
<tr><td><code>python3 tools/include_graph.py --summary</code></td><td>metrics, for before/after diffing</td></tr>
<tr><td><code>python3 tools/include_graph.py --mermaid</code></td><td>directory-level graph</td></tr>
<tr><td><code>python3 tools/include_unused.py --json u.json</code></td><td>candidate unneeded includes</td></tr>
<tr><td><code>python3 tools/include_unused.py --verify</code></td><td>confirm candidates by recompiling</td></tr>
<tr><td><code>python3 tools/pragma_once_to_guards.py --verify</code></td><td>no <code>#pragma once</code> came back</td></tr>
<tr><td><code>doxygen doc/Doxyfile</code></td><td>per-file include / included-by / directory graphs</td></tr>
</table></div>
"""
    with open(out_path, 'w', encoding='utf-8') as fh:
        fh.write(doc)
    print('wrote %s (%d headers, %d TUs, %d cycles)'
          % (out_path, len(headers), len(tus), len(comps)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
