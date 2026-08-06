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

# A file is "platform guarded" if it contains code THIS build does not compile, so a
# clean local build cannot authorise removing an include that branch needs. Flagging
# every `#if` (include guards included) would exclude the whole tree; flagging none let
# three separate breakages reach CI during the darktable.h series.
#
# Two distinct cases, and conflating them makes the tool useless:
#   * an OS/compiler conditional (_WIN32, __APPLE__ ...) -- its body is never compiled
#     here, so the file is risky whether or not it has an #else;
#   * a feature conditional (HAVE_OPENCL ...) -- with the feature ON locally the body IS
#     compiled and is verified; only its #else/#elif is unverified.
OS_MACROS = (r'_WIN32|WIN32|__WIN32__|__MINGW\w*|_MSC_VER|__APPLE__|__MACH__'
             r'|GDK_WINDOWING_QUARTZ|__FreeBSD__|__NetBSD__|__OpenBSD__|__DragonFly__')
OS_COND_RE = re.compile(r'^[ \t]*#[ \t]*(?:if|ifdef|ifndef|elif)\b[^\n]*\b(?:' + OS_MACROS + r')\b', re.M)
COND_RE = re.compile(r'^[ \t]*#[ \t]*(if|ifdef|ifndef|elif|else|endif)\b([^\n]*)', re.M)
FEATURE_RE = re.compile(r'\b(?:HAVE_[A-Z0-9_]+|__SSE2__|__ARM_NEON|_OPENMP)\b')


def has_unbuilt_branch(text):
    """True if the file contains a branch this build does not compile."""
    if OS_COND_RE.search(text):
        return True
    stack = []
    for m in COND_RE.finditer(text):
        kind, rest = m.group(1), m.group(2)
        if kind in ('if', 'ifdef', 'ifndef'):
            stack.append(bool(FEATURE_RE.search(rest)))
        elif kind == 'endif':
            if stack:
                stack.pop()
        elif kind in ('else', 'elif'):
            # the alternative branch of a feature conditional is not compiled here
            if stack and stack[-1]:
                return True
    return False


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


ATTRIBUTE_RE = re.compile(r'__attribute__\s*\(\((?:[^()]|\([^()]*\))*\)\)')


def declared_names(text):
    names = set()
    body = strip_comments_and_strings(text)
    # Drop __attribute__((...)) before looking for declarations: in
    #   static inline __attribute__((always_inline)) void dt_Lab_to_XYZ(...)
    # the first identifier followed by "(" is __attribute__, so every attributed inline
    # function was invisible. That made common/colorspaces_inline_conversions.h look as
    # though it declared nothing, and its consumers were wrongly told they did not need
    # it -- which broke three files in the CI nofeatures configuration.
    body = ATTRIBUTE_RE.sub(' ', body)
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
        guarded = has_unbuilt_branch(text[p])
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
            annotated = bool(re.search(
                r'^[ \t]*#[ \t]*include[ \t]+"%s"[ \t]*(?://|/\*)' % re.escape(inc),
                text[p], re.M))
            cands.append({'include': inc, 'header': target,
                          'platform_guarded': guarded, 'annotated': annotated})
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

    if '--push-down' in sys.argv:
        return push_down(results)

    if '--apply' in sys.argv:
        prefix = sys.argv[sys.argv.index('--prefix') + 1] if '--prefix' in sys.argv else 'src/'
        return apply_mode(results, prefix, '--include-guarded' not in sys.argv)

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




# ---------------------------------------------------------------------------
# --apply: remove candidates and prove the result still builds.
#
# Doing one compile per candidate would be ~760 builds. Instead:
#   1. strip every candidate in the tranche at once, then run ONE full build;
#   2. whatever that build implicates, restore -- file by file -- and retry;
#   3. for each restored file, retry alone (all of its candidates at once), and
#      only if THAT fails fall back to removing its candidates one at a time.
# The happy path costs one build for a whole directory; the pathological path
# degenerates to the naive per-include cost for the few files that need it.
# ---------------------------------------------------------------------------

def _strip(path, incs):
    """Remove whole #include lines. Returns the ones actually removed.

    Must match the LINE, not the exact string `#include "x"\n`: an include carrying a
    trailing comment would silently fail to match, and the caller would then report a
    removal that never happened -- which is how this tool once claimed 7 removals for a
    3-line diff."""
    s = read(path)
    done = []
    for inc in incs:
        pat = re.compile(r'^[ \t]*#[ \t]*include[ \t]+"%s"[^\n]*\n' % re.escape(inc), re.M)
        s, n = pat.subn('', s, count=1)
        if n:
            done.append(inc)
    with open(path, 'w', encoding='utf-8') as fh:
        fh.write(s)
    return done


def _build(targets=None):
    cmd = ['ninja', '-C', BUILD, '-k', '0'] + (targets or [])
    p = subprocess.run(cmd, capture_output=True, text=True)
    return p.returncode, (p.stdout + p.stderr)


def _implicated(log, owned):
    """Source files in `owned` that the build log blames, directly or through a
    generated introspection_*.c that textually includes them."""
    hit = set()
    for m in re.finditer(r'([\w./\\-]+\.(?:c|cc|cpp|h|hpp))[:(]', log):
        f = m.group(1).replace('\\', '/')
        for o in owned:
            if f.endswith('/' + os.path.basename(o)) or f.endswith(o):
                hit.add(o)
        base = os.path.basename(f)
        if base.startswith('introspection_'):
            stem = base[len('introspection_'):]
            for o in owned:
                if os.path.basename(o) == stem:
                    hit.add(o)
    return hit


def _apply_only(subset, originals, targets):
    """Restore every touched file, then re-strip just `subset`."""
    for p, text in originals.items():
        with open(p, 'w', encoding='utf-8') as fh:
            fh.write(text)
    for p in subset:
        _strip(p, [c['include'] for c in targets[p]])


def _bisect_set(files, originals, targets):
    """Largest subset of `files` whose removals still build. Removals are independent
    in practice, so recursive halving is sound; the caller re-verifies the union."""
    if not files:
        return []
    _apply_only(files, originals, targets)
    rc, _ = _build()
    if rc == 0:
        return list(files)
    if len(files) == 1:
        return []
    mid = len(files) // 2
    left = _bisect_set(files[:mid], originals, targets)
    right = _bisect_set(files[mid:], originals, targets)
    good = left + right
    _apply_only(good, originals, targets)
    rc, _ = _build()
    if rc != 0:                     # halves interact: keep the larger half only
        good = left if len(left) >= len(right) else right
        _apply_only(good, originals, targets)
        _build()
    return good


def apply_mode(results, prefix, skip_guarded):
    targets = {p: v for p, v in results.items() if p.startswith(prefix)}
    if skip_guarded:
        targets = {p: v for p, v in targets.items()
                   if not any(c['platform_guarded'] for c in v)}
    # A trailing comment on an include line is the author stating why it is there --
    # usually a transitive need the static pass cannot see ("needed by dwt.h",
    # "for dt_pthread_mutex_t"). Never remove those silently.
    annotated = [(p, c['include']) for p, v in targets.items() for c in v if c.get('annotated')]
    targets = {p: [c for c in v if not c.get('annotated')] for p, v in targets.items()}
    targets = {p: v for p, v in targets.items() if v}
    for p, i in annotated:
        print('  skipping annotated include (documented intent): %s: %s' % (p, i))
    if not targets:
        print('nothing to do for prefix %r' % prefix)
        return 0

    originals = {p: read(p) for p in targets}
    print('applying %d candidate removals across %d files (prefix %r)'
          % (sum(len(v) for v in targets.values()), len(targets), prefix))
    for p, cands in targets.items():
        _strip(p, [c['include'] for c in cands])

    removed = dict(targets)
    for attempt in range(6):
        rc, log = _build()
        if rc == 0:
            break
        bad = _implicated(log, list(removed))
        if not bad:
            # Blame-mapping is impossible for headers: when foo.h stops including bar.h,
            # the error lands in some baz.c that used bar.h's symbols through foo.h, and
            # the log never names foo.h. Fall back to bisecting the FILE SET instead of
            # giving up: O(bad x log n) builds rather than one per file.
            print('  build fails and blames nothing we edited -- bisecting the file set')
            keep = _bisect_set(sorted(removed), originals, targets)
            removed = {p: targets[p] for p in keep}
            break
        print('  round %d: restoring %d implicated file(s)' % (attempt + 1, len(bad)))
        for p in bad:
            open(p, 'w', encoding='utf-8').write(originals[p])
            removed.pop(p, None)
    else:
        print('did not converge; reverting all', file=sys.stderr)
        for p, s in originals.items():
            open(p, 'w', encoding='utf-8').write(s)
        _build()
        return 1

    # Retry each restored file on its own, then per-include.
    salvaged = 0
    for p in [f for f in targets if f not in removed]:
        cands = [c['include'] for c in targets[p]]
        _strip(p, cands)
        rc, _ = _build()
        if rc == 0:
            removed[p] = targets[p]
            salvaged += len(cands)
            continue
        open(p, 'w', encoding='utf-8').write(originals[p])
        kept = []
        for inc in cands:
            _strip(p, [inc])
            rc, _ = _build()
            if rc == 0:
                kept.append(inc)
            else:
                open(p, 'w', encoding='utf-8').write(originals[p])
                for k in kept:
                    _strip(p, [k])
        if kept:
            removed[p] = [{'include': i} for i in kept]
            salvaged += len(kept)

    rc, _ = _build()
    total = sum(len(v) for v in removed.values())
    print('\nREMOVED %d includes from %d files (%d salvaged by bisection); final build rc=%d'
          % (total, len(removed), salvaged, rc))
    return 0 if rc == 0 else 1




# ---------------------------------------------------------------------------
# --push-down: enforce "a header includes only what its own declarations need".
#
# tools/include_unused.py --apply keeps an include the header does not need when a
# CONSUMER is reaching through the header to get it. That is the wrong reason to keep
# it: the consumer should include it itself. This mode removes it from the header and
# adds it to whichever translation units actually break.
#
# The exception is honoured, not overridden: if EVERY consumer of the header turns out
# to need the include, pushing it down is pure boilerplate, so it stays in the header.
# ---------------------------------------------------------------------------

def _object_to_source():
    """Reverse-map ninja object targets back to source files, via compdb."""
    fmap = {}
    r = subprocess.run(['ninja', '-C', BUILD, '-t', 'compdb'], capture_output=True, text=True)
    if r.returncode != 0:
        return fmap
    for e in json.loads(r.stdout):
        out = e.get('output') or ''
        f = e.get('file') or ''
        if not out or not f:
            continue
        base = os.path.basename(f)
        if base.startswith('introspection_'):
            stem = base[len('introspection_'):]
            for d in ('src/iop/', 'src/libs/', 'src/imageio/format/', 'src/imageio/storage/'):
                if os.path.exists(d + stem):
                    f = d + stem
                    break
        fmap[out] = os.path.relpath(f, os.getcwd()) if os.path.isabs(f) else f
    return fmap


def _failing_sources(log, obj2src):
    out = set()
    for m in re.finditer(r'^FAILED: (?:\[[^\]]*\] )?(\S+)', log, re.M):
        src = obj2src.get(m.group(1))
        if src:
            out.add(os.path.normpath(src))
    return out


def _consumers_of(header):
    """Files that include `header` directly (either spelling)."""
    rel = header[len(SRC) + 1:] if header.startswith(SRC + os.sep) else header
    base = os.path.basename(header)
    hits = set()
    for root, dirs, names in os.walk(SRC):
        dirs[:] = [d for d in dirs if d != 'external']
        for n in names:
            if not n.endswith(('.c', '.cc', '.cpp', '.h', '.hpp')):
                continue
            p = os.path.join(root, n)
            t = read(p)
            if '#include "%s"' % rel in t or '#include "%s"' % base in t:
                hits.add(os.path.normpath(p))
    return hits


def push_down(results):
    headers = {p: v for p, v in results.items() if p.endswith(('.h', '.hpp'))}
    obj2src = _object_to_source()
    moved = kept_universal = reverted = 0

    for hdr, cands in sorted(headers.items()):
        consumers = _consumers_of(hdr)
        for c in cands:
            inc = c['include']
            original_hdr = read(hdr)
            _strip(hdr, [inc])
            rc, log = _build()
            if rc == 0:
                print('  %s: dropped %s (nobody needed it)' % (hdr, inc))
                moved += 1
                continue

            needy = _failing_sources(log, obj2src)
            c_consumers = {f for f in consumers if f.endswith(('.c', '.cc', '.cpp'))}
            if needy and c_consumers and needy >= c_consumers:
                # every consumer needs it -- pushing it down is pure boilerplate
                open(hdr, 'w', encoding='utf-8').write(original_hdr)
                _build()
                print('  %s: KEPT %s (all %d consumers need it)' % (hdr, inc, len(c_consumers)))
                kept_universal += 1
                continue

            patched = []
            for f in sorted(needy):
                t = read(f)
                if '#include "%s"' % inc in t:
                    continue
                lines = [l for l in t.splitlines() if l.startswith('#include "')]
                if not lines:
                    continue
                anchor = lines[0] + '\n'
                t = t.replace(anchor, anchor + '#include "%s"\n' % inc, 1)
                with open(f, 'w', encoding='utf-8') as fh:
                    fh.write(t)
                patched.append(f)

            rc2, _ = _build()
            if rc2 == 0:
                print('  %s: moved %s down into %d consumer(s)' % (hdr, inc, len(patched)))
                moved += 1
            else:
                open(hdr, 'w', encoding='utf-8').write(original_hdr)
                for f in patched:
                    t = read(f).replace('#include "%s"\n' % inc, '', 1)
                    with open(f, 'w', encoding='utf-8') as fh:
                        fh.write(t)
                _build()
                print('  %s: reverted %s (did not converge)' % (hdr, inc))
                reverted += 1

    rc, _ = _build()
    print('\nmoved=%d  kept-as-universal=%d  reverted=%d  final build rc=%d'
          % (moved, kept_universal, reverted, rc))
    return 0 if rc == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
