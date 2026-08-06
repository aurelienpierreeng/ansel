#!/usr/bin/env python3
"""Find `#include`s that a file does not need.

Two stages, because neither alone is trustworthy:

  1. STATIC (fast, whole tree, no build).  Index every project header by the names it
     DECLARES (macros, typedefs, tags, enum constants, function and variable
     declarations).  For each file F and each header H that F includes directly, if F
     mentions none of H's own names, H is a *candidate* for removal.

     This over-reports: F may include H legitimately to reach something H itself
     includes (a transitive dependency).  That is exactly the coupling we want to make
     explicit, but removing such an include still breaks the build, so a candidate is
     a question, never a verdict.

  2. VERIFY (slow, exact w.r.t. the current build config).  Actually comment the
     include out, recompile the affected object with ninja, and keep the removal only
     if it still compiles.  Run this on the candidates from stage 1.

The verify stage cannot prove an include is unneeded on OTHER platforms: an include
used only inside `#ifdef _WIN32` looks removable on Linux and is not.  Candidates whose
symbol use is under a conditional are flagged `platform-guarded` and must never be
removed on the strength of a Linux-only build.  See doc/include-hygiene-roadmap.md.

Usage:
  python3 tools/include_unused.py                       # static pass, summary
  python3 tools/include_unused.py --headers             # only .h files
  python3 tools/include_unused.py --sources             # only .c/.cc files
  python3 tools/include_unused.py --file src/iop/x.c    # one file, verbose
  python3 tools/include_unused.py --json out.json       # machine-readable
  python3 tools/include_unused.py --verify --limit 40   # empirically test candidates
"""
import json
import os
import re
import subprocess
import sys
from collections import defaultdict

SRC = 'src'
BUILD = 'build'

INCLUDE_RE = re.compile(r'^([ \t]*#[ \t]*include[ \t]+")([^"]+)(".*)$', re.M)
IDENT_RE = re.compile(r'\b[A-Za-z_][A-Za-z0-9_]*\b')

# Headers included for their SIDE EFFECTS, not for names they declare. A static pass
# will always call these unused; they must never be reported.
SIDE_EFFECT_HEADERS = {
    'config.h',
    'common/poison.h',          # #pragma-poisons forbidden libc calls
    'win/win.h',                # #undefs legacy windows.h macros
    'common/module_api.h',      # X-macro, generates struct members
    'views/view_api.h',
    'libs/lib_api.h',
    'imageio/format/imageio_format_api.h',
    'imageio/storage/imageio_storage_api.h',
    'external/ThreadSafetyAnalysis.h',
    'common/darktable.h',       # the orchestrator: handled by its own migration
}

# Declaration shapes. Deliberately generous: a missed declaration turns into a false
# "unused" report, which the verify stage then has to spend a compile to reject.
DECL_PATTERNS = [
    re.compile(r'^[ \t]*#[ \t]*define[ \t]+([A-Za-z_][A-Za-z0-9_]*)', re.M),
    re.compile(r'^[ \t]*}[ \t]*([A-Za-z_][A-Za-z0-9_]*)[ \t]*;', re.M),          # } dt_foo_t;
    re.compile(r'\btypedef\b[^;{]*?\b([A-Za-z_][A-Za-z0-9_]*)[ \t]*;', re.M),     # typedef X dt_y_t;
    re.compile(r'\b(?:struct|union|enum)[ \t]+([A-Za-z_][A-Za-z0-9_]*)', re.M),
    re.compile(r'^[A-Za-z_][A-Za-z0-9_ \t\*]*?\b([A-Za-z_][A-Za-z0-9_]*)[ \t]*\(', re.M),
    re.compile(r'^[ \t]*extern[^;=]*?\b([A-Za-z_][A-Za-z0-9_]*)[ \t]*(?:\[|;)', re.M),
]

CONDITIONAL_RE = re.compile(r'^[ \t]*#[ \t]*(if|ifdef|ifndef|elif|else)\b', re.M)


def read(path):
    with open(path, encoding='utf-8', errors='replace') as fh:
        return fh.read()


def project_files():
    out = []
    for root, dirs, names in os.walk(SRC):
        dirs[:] = [d for d in dirs if d != 'external']
        for n in names:
            if n.endswith(('.c', '.cc', '.cpp', '.h', '.hpp')):
                out.append(os.path.join(root, n))
    return sorted(out)


def strip_comments_and_strings(text):
    """Crude but adequate: we only need identifier presence, not exact syntax."""
    text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
    text = re.sub(r'//[^\n]*', ' ', text)
    text = re.sub(r'"(?:\\.|[^"\\])*"', ' ', text)
    return text


def declared_names(text):
    names = set()
    body = strip_comments_and_strings(text)
    for pat in DECL_PATTERNS:
        for m in pat.finditer(body):
            names.add(m.group(1))
    # enum constants: every identifier inside an enum block
    for m in re.finditer(r'\benum\b[^{;]*\{(.*?)\}', body, flags=re.S):
        for ident in IDENT_RE.findall(m.group(1)):
            names.add(ident)
    names.discard('')
    return names


def resolve(inc, from_path, known):
    cand = os.path.normpath(os.path.join(SRC, inc))
    if cand in known:
        return cand
    cand2 = os.path.normpath(os.path.join(os.path.dirname(from_path), inc))
    if cand2 in known:
        return cand2
    return None


def analyse():
    files = project_files()
    known = set(files)
    text = {p: read(p) for p in files}
    provides = {p: declared_names(text[p]) for p in files if p.endswith(('.h', '.hpp'))}

    results = {}
    for p in files:
        body = strip_comments_and_strings(text[p])
        used = set(IDENT_RE.findall(body))
        guarded = bool(CONDITIONAL_RE.search(text[p]))
        cands = []
        for m in INCLUDE_RE.finditer(text[p]):
            inc = m.group(2)
            if inc in SIDE_EFFECT_HEADERS:
                continue
            target = resolve(inc, p, known)
            if target is None or target == p:
                continue
            names = provides.get(target)
            if not names:
                continue           # header declares nothing we can see: stay silent
            if used & names:
                continue
            cands.append({'include': inc, 'header': target,
                          'platform_guarded': guarded})
        if cands:
            results[p] = cands
    return results


_TARGETS_CACHE = None


def _all_targets():
    global _TARGETS_CACHE
    if _TARGETS_CACHE is None:
        try:
            out = subprocess.run(['ninja', '-C', BUILD, '-t', 'targets', 'all'],
                                 capture_output=True, text=True, timeout=300).stdout
        except (OSError, subprocess.SubprocessError):
            out = ''
        _TARGETS_CACHE = [ln.split(':')[0].strip() for ln in out.splitlines()]
    return _TARGETS_CACHE


def ninja_target_for(path):
    """Best-effort mapping from a source file to one ninja object target."""
    base = os.path.basename(path)
    for tgt in _all_targets():
        if tgt.endswith(base + '.o') or tgt.endswith(base + '.obj'):
            return tgt
    return None


def verify(results, limit):
    """Comment each candidate out, rebuild its object, keep only what still compiles."""
    confirmed, rejected = [], []
    tested = 0
    for path, cands in sorted(results.items()):
        if not path.endswith(('.c', '.cc', '.cpp')):
            continue               # headers have no object of their own; see roadmap
        target = ninja_target_for(path)
        if target is None:
            continue
        original = read(path)
        for c in cands:
            if tested >= limit:
                break
            tested += 1
            patched = original.replace('#include "%s"' % c['include'],
                                       '/* IWYU-TEST */ //#include "%s"' % c['include'], 1)
            with open(path, 'w', encoding='utf-8') as fh:
                fh.write(patched)
            rc = subprocess.run(['ninja', '-C', BUILD, target],
                                capture_output=True, text=True).returncode
            with open(path, 'w', encoding='utf-8') as fh:
                fh.write(original)
            (confirmed if rc == 0 else rejected).append((path, c['include']))
        subprocess.run(['ninja', '-C', BUILD, target], capture_output=True, text=True)
        if tested >= limit:
            break
    return confirmed, rejected


def main():
    only_h = '--headers' in sys.argv
    only_c = '--sources' in sys.argv
    results = analyse()

    if '--file' in sys.argv:
        want = sys.argv[sys.argv.index('--file') + 1]
        for inc in results.get(want, []):
            print('%s: %s%s' % (want, inc['include'],
                                '   [platform-guarded]' if inc['platform_guarded'] else ''))
        if want not in results:
            print('%s: no candidates' % want)
        return 0

    if only_h:
        results = {k: v for k, v in results.items() if k.endswith(('.h', '.hpp'))}
    if only_c:
        results = {k: v for k, v in results.items() if k.endswith(('.c', '.cc', '.cpp'))}

    if '--json' in sys.argv:
        out = sys.argv[sys.argv.index('--json') + 1]
        with open(out, 'w', encoding='utf-8') as fh:
            json.dump(results, fh, indent=1, sort_keys=True)
        print('wrote %s' % out)

    if '--verify' in sys.argv:
        limit = 20
        if '--limit' in sys.argv:
            limit = int(sys.argv[sys.argv.index('--limit') + 1])
        confirmed, rejected = verify(results, limit)
        print('\n=== VERIFIED REMOVABLE (compiles without it) ===')
        for p, i in confirmed:
            print('  %s: %s' % (p, i))
        print('\n=== NEEDED AFTER ALL (transitive dependency) ===')
        for p, i in rejected:
            print('  %s: %s' % (p, i))
        print('\n%d removable / %d tested' % (len(confirmed), len(confirmed) + len(rejected)))
        return 0

    total = sum(len(v) for v in results.values())
    hdr = sum(len(v) for k, v in results.items() if k.endswith(('.h', '.hpp')))
    print('candidate unneeded includes: %d in %d files (%d in headers, %d in sources)'
          % (total, len(results), hdr, total - hdr))

    per_dir = defaultdict(int)
    for k, v in results.items():
        per_dir[k.split(os.sep)[1]] += len(v)
    print('\nby directory:')
    for d, n in sorted(per_dir.items(), key=lambda kv: -kv[1]):
        print('  %5d  %s' % (n, d))

    per_header = defaultdict(int)
    for v in results.values():
        for c in v:
            per_header[c['include']] += 1
    print('\nmost often included without being used:')
    for h, n in sorted(per_header.items(), key=lambda kv: -kv[1])[:20]:
        print('  %5d  %s' % (n, h))

    worst = sorted(results.items(), key=lambda kv: -len(kv[1]))[:15]
    print('\nfiles with the most candidates:')
    for p, v in worst:
        print('  %5d  %s' % (len(v), p))
    return 0


if __name__ == '__main__':
    sys.exit(main())
