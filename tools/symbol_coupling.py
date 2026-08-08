#!/usr/bin/env python3
"""Module coupling measured at the SYMBOL level, from the linker's point of view.

tools/include_graph.py measures what a file *reads* -- textual `#include` edges. This
measures what actually *links*: for every compiled object, which symbols it defines and
which it leaves undefined, resolved against the module that defines them. The result is
the real API surface between subsystems.

Why it is worth having both: an include edge can be an accident (a header pulled in for
one typedef), and removing it changes nothing about the program. A symbol edge is a call
that exists at runtime. When `include_graph.py` reports `common/ -> control/ : 104`, this
tool answers the question that actually matters for modularisation -- WHICH functions,
so the inversion becomes a work list rather than a number.

Reads .o files from the build directory, so it needs a completed build and reflects the
configuration that produced it (a `nofeatures` build will show fewer edges).

Usage:
  python3 tools/symbol_coupling.py                     # module x module summary
  python3 tools/symbol_coupling.py --edge common develop   # the symbols behind one edge
  python3 tools/symbol_coupling.py --inversions        # only upward edges (layer breaks)
  python3 tools/symbol_coupling.py --build build-nofeatures
"""
import collections
import os
import re
import subprocess
import sys

# Same layer order as tools/include_graph.py -- keep them in sync.
LAYER = {'external': 0, 'win': 0, 'system': 0, 'common': 1, 'math': 1, 'colorprofiles': 1, 'pixel': 2, 'control': 3,
         'gui': 4, 'widgets': 4,   # widgets/ = reusable GTK widgets, no app state 'develop': 5,
         'iop': 6, 'imageio': 6, 'libs': 7, 'views': 7, 'chart': 7,
         'apps': 10,   # executables link the orchestrator, so they sit ABOVE it 'app': 9}

OBJ_RE = re.compile(r'\.dir/(.*)\.(?:c|cc|cpp)\.o$')


def module_of(obj_path):
    """The subsystem an object belongs to, from its path inside the build tree."""
    m = OBJ_RE.search(obj_path)
    rel = m.group(1) if m else obj_path
    parts = [p for p in rel.split('/') if p not in ('.', '..')]
    for p in parts:
        if p in LAYER:
            return p
    # iop modules build as their own target, so the subsystem is not in the path
    return 'iop' if '/iop/' in obj_path or obj_path.startswith('src/iop') else parts[0]


def nm(obj, args):
    r = subprocess.run(['nm', '-g'] + args + [obj], capture_output=True, text=True)
    return {ln.split()[-1] for ln in r.stdout.splitlines() if ln.strip()}


def collect(build):
    objs = []
    for root, _, names in os.walk(build):
        for n in names:
            if n.endswith('.o'):
                objs.append(os.path.join(root, n))
    if not objs:
        print('no object files under %s -- build first' % build, file=sys.stderr)
        raise SystemExit(2)

    owner, undef = {}, {}
    for o in objs:
        mod = module_of(o)
        for s in nm(o, ['--defined-only']):
            owner.setdefault(s, mod)
        undef[o] = (mod, nm(o, ['-u']))
    return owner, undef, len(objs)


def edges(owner, undef):
    per_edge = collections.defaultdict(set)
    for _, (src, syms) in undef.items():
        for s in syms:
            dst = owner.get(s)
            if dst and dst != src:
                per_edge[(src, dst)].add(s)
    return per_edge


def main():
    build = 'build'
    if '--build' in sys.argv:
        build = sys.argv[sys.argv.index('--build') + 1]
    owner, undef, n = collect(build)
    per_edge = edges(owner, undef)
    print('%d objects, %d exported symbols, %d cross-module edges\n' % (n, len(owner), len(per_edge)))

    if '--edge' in sys.argv:
        i = sys.argv.index('--edge')
        a, b = sys.argv[i + 1], sys.argv[i + 2]
        syms = sorted(per_edge.get((a, b), ()))
        print('%s -> %s : %d symbols' % (a, b, len(syms)))
        for s in syms:
            print('   ', s)
        return 0

    rows = []
    for (a, b), syms in per_edge.items():
        la, lb = LAYER.get(a), LAYER.get(b)
        inverted = la is not None and lb is not None and lb > la
        if '--inversions' in sys.argv and not inverted:
            continue
        rows.append((len(syms), a, b, inverted))
    rows.sort(reverse=True)

    print('%-14s %-14s %8s' % ('from', 'to', 'symbols'))
    for cnt, a, b, inverted in rows[:30]:
        print('%-14s %-14s %8d %s' % (a, b, cnt, '  <-- LAYER INVERSION' if inverted else ''))

    inv = [r for r in rows if r[3]]
    if inv and '--inversions' not in sys.argv:
        print('\n%d inverted edges carrying %d symbols in total.' % (len(inv), sum(r[0] for r in inv)))
        print('Run --edge <from> <to> to list the functions behind one of them.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
