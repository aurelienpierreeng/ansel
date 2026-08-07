#!/usr/bin/env python3
"""Find files that live in the wrong directory.

A file whose only consumers are in ONE other subsystem is not shared infrastructure --
it belongs to that subsystem. This reports those, and refuses to report the ones that
would merely move the problem.

Two checks make the difference between a useful list and a misleading one:

  * SIBLING USE. A header used by `iop/` looks like an `iop/` file until you notice that
    three files in its own directory also use it. Moving it alone then creates a new
    layering inversion where there was none. Such files are reported separately as
    "cluster" candidates: they can only move together with the siblings that use them.

  * LAYER DIRECTION. Moving a file UP the layer stack (common/ -> iop/) removes an
    inversion. Moving it DOWN would create one. Only the former is proposed.

Usage:
  python3 tools/misplaced_files.py                 # every source directory
  python3 tools/misplaced_files.py --dir common    # one directory
  python3 tools/misplaced_files.py --clusters      # show the blocked ones and why
"""
import collections
import os
import re
import sys

SRC = 'src'
LAYER = {'external': 0, 'win': 0, 'system': 0, 'common': 1, 'math': 1, 'pixel': 2, 'control': 3,
         'gui': 4, 'widgets': 4,   # widgets/ = reusable GTK widgets, no app state 'develop': 5,
         'iop': 6, 'imageio': 6, 'libs': 7, 'views': 7, 'chart': 7,
         'apps': 10,   # executables link the orchestrator, so they sit ABOVE it 'app': 9}

# Headers included for side effects, or re-included X-macro headers: never propose these.
NEVER_MOVE = {'module_api.h', 'view_api.h', 'lib_api.h', 'imageio_format_api.h',
              'imageio_storage_api.h', 'poison.h', 'win.h', 'darktable.h', 'config.h'}


def subsystem(path):
    parts = path.split(os.sep)
    return parts[1] if len(parts) > 1 else None


def sources():
    out = []
    for root, dirs, names in os.walk(SRC):
        dirs[:] = [d for d in dirs if d not in ('external', 'attic')]
        for n in names:
            if n.endswith(('.c', '.cc', '.cpp', '.h', '.hh', '.hpp')):
                out.append(os.path.join(root, n))
    return sorted(out)


def build_index():
    files = sources()
    text = {}
    for f in files:
        try:
            text[f] = open(f, encoding='utf-8', errors='replace').read()
        except OSError:
            text[f] = ''
    # who includes what, by resolved path
    known = set(files)
    users = collections.defaultdict(set)
    for f, t in text.items():
        for inc in re.findall(r'^\s*#\s*include\s+"([^"]+)"', t, re.M):
            for cand in (os.path.normpath(os.path.join(SRC, inc)),
                         os.path.normpath(os.path.join(os.path.dirname(f), inc))):
                if cand in known and cand != f:
                    users[cand].add(f)
                    break
    return files, users


def main():
    only_dir = sys.argv[sys.argv.index('--dir') + 1] if '--dir' in sys.argv else None
    files, users = build_index()

    movable, clusters = [], []
    for f in files:
        if not f.endswith(('.h', '.hh', '.hpp')):
            continue
        if os.path.basename(f) in NEVER_MOVE:
            continue
        home = subsystem(f)
        if home is None or (only_dir and home != only_dir):
            continue
        consumers = collections.Counter(subsystem(u) for u in users.get(f, ()))
        own = consumers.pop(home, 0)
        if len(consumers) != 1:
            continue
        dest, n = next(iter(consumers.items()))
        if LAYER.get(dest, 99) <= LAYER.get(home, 0):
            continue                      # would move DOWN the stack: creates an inversion
        impl = [f[:-2] + e for e in ('c', 'cc') if os.path.exists(f[:-2] + e)]
        rec = (n, f, dest, own, impl)
        (clusters if own else movable).append(rec)

    movable.sort(reverse=True)
    clusters.sort(reverse=True)

    print('=== MOVABLE: only one consuming subsystem, and nothing in its own directory '
          'uses it ===')
    by_dest = collections.defaultdict(list)
    for n, f, dest, _, impl in movable:
        by_dest[dest].append((f, n, impl))
    for dest, items in sorted(by_dest.items(), key=lambda kv: -len(kv[1])):
        print('\n  -> %s/  (%d)' % (dest, len(items)))
        for f, n, impl in items:
            print('     %-44s %d includer(s)%s'
                  % (f, n, '  + ' + ', '.join(os.path.basename(i) for i in impl) if impl else ''))

    if '--clusters' in sys.argv:
        print('\n=== BLOCKED: one external consumer, but siblings use it too ===')
        print('    (moving these alone creates an inversion -- move the cluster or nothing)')
        for n, f, dest, own, _ in clusters:
            print('     %-44s -> %-9s %d external, %d sibling(s)' % (f, dest, n, own))
    else:
        print('\n%d further files have a single external consumer but are also used by '
              'siblings; run --clusters to see them.' % len(clusters))
    return 0


if __name__ == '__main__':
    sys.exit(main())
