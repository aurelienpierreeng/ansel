#!/usr/bin/env python3
"""Allocator/deallocator pairing: dt_alloc_align* must be freed by dt_free_align, and
nothing else may be.

WHY THIS EXISTS, and why a build cannot replace it. dt_alloc_align() is _aligned_malloc()
on Windows and posix_memalign() everywhere else; dt_free_align() is _aligned_free() on
Windows and g_free() everywhere else. So on Linux and macOS BOTH families end at free(),
every mismatch works perfectly, and no test, sanitizer or review on those platforms will
ever see one. On Windows the same code corrupts the heap, and it crashes somewhere else
entirely, later, in whatever unlucky code touches the heap next.

That is a bug class you cannot find by running the program on the machine you develop on,
which is what makes it worth a static check. Both directions are reported:

  * something from malloc/calloc/g_malloc/g_new/strdup freed with dt_free_align()
  * something from dt_alloc_align()/dt_calloc_align() freed with free()/g_free()/dt_free()

Scope: matches allocations and frees of the SAME expression within the SAME file. That is
deliberately narrow -- matching by name across files reports `data`, `buffer` and `img` in
unrelated translation units and buries the real findings. It cannot see a struct field
allocated in one file and freed in another; the generic-destructor case that produced the
original bug report is prevented in the API instead (see dt_cache_seed in caches/cache.h),
which is the better fix where it is available.

Usage:  python3 tools/check_alloc_pairing.py [--quiet]
Exits non-zero if any mismatch is found.
"""
import re, os, collections

ALIGNED_ALLOC = re.compile(r'\b(dt_alloc_align\w*|dt_calloc_align\w*|dt_alloc_perthread\w*|dt_realloc_align\w*)\s*\(')
PLAIN_ALLOC   = re.compile(r'(?<![_\w])(malloc|calloc|realloc|strdup|strndup|g_malloc\d*|g_try_malloc\d*|g_realloc|g_new\d*|g_strdup\w*|g_slice_new\w*)\s*\(')
ALIGNED_FREE  = re.compile(r'\bdt_free_align(?:_ptr)?\s*\(\s*([^,;)]*)')
PLAIN_FREE    = re.compile(r'(?<![_\w])(free|g_free|dt_free|dt_free_gpointer)\s*\(\s*([^,;)]*)')
ASSIGN        = re.compile(r'([A-Za-z_][\w\.\->\[\]\s]*?)\s*=\s*(?:\([^)]*\)\s*)*$')

def norm(expr):
    e = expr.strip()
    e = re.sub(r'^\(+\s*|\s*\)+$', '', e)
    e = re.sub(r'\(\s*[\w\s]*\*+\s*\)', '', e)      # drop casts
    e = e.strip().lstrip('&').strip()
    e = re.sub(r'\[[^\]]*\]', '[]', e)              # normalise indices
    e = re.sub(r'\s+', '', e)
    return e

files = []
for root, dirs, fs in os.walk('src'):
    if 'external' in root.split(os.sep): continue
    for f in fs:
        if f.endswith(('.c','.cc','.h')): files.append(os.path.join(root,f))

findings = []
for path in sorted(files):
    if path.endswith('system/mem_alloc.h'): continue
    try: lines = open(path, encoding='utf-8', errors='replace').read().split('\n')
    except Exception: continue
    A = collections.defaultdict(list); P = collections.defaultdict(list)
    AF = collections.defaultdict(list); PF = collections.defaultdict(list)
    for i, ln in enumerate(lines, 1):
        code = re.sub(r'//.*$', '', ln)
        st = code.strip()
        if st.startswith('*') or st.startswith('/*') or st.startswith('#'): continue
        for rx, b in ((ALIGNED_ALLOC, A), (PLAIN_ALLOC, P)):
            m = rx.search(code)
            if m:
                a = ASSIGN.search(code[:m.start()])
                if a:
                    k = norm(a.group(1))
                    if k and not k.endswith('='): b[k].append((i, ln.strip()[:120]))
        m = ALIGNED_FREE.search(code)
        if m:
            k = norm(m.group(1))
            if k: AF[k].append((i, ln.strip()[:120]))
        for m in PLAIN_FREE.finditer(code):
            k = norm(m.group(2))
            if k: PF[k].append((i, ln.strip()[:120]))
    for k in set(P) & set(AF):
        findings.append(("PLAIN alloc -> dt_free_align", path, k, P[k], AF[k]))
    for k in set(A) & set(PF):
        findings.append(("ALIGNED alloc -> plain free", path, k, A[k], PF[k]))

import sys
quiet = "--quiet" in sys.argv

if not findings:
    if not quiet:
        print("OK: every dt_alloc_align* is freed by dt_free_align, and nothing else is.")
    sys.exit(0)

print(f"Allocator/deallocator mismatches: {len(findings)}")
print()
for kind, path, k, al, fr in findings:
    print(f"[{kind}]  {path}   `{k}`")
    for i,t in al[:2]: print(f"    alloc :{i}  {t}")
    for i,t in fr[:2]: print(f"    free  :{i}  {t}")
    print()
print("dt_alloc_align is _aligned_malloc on Windows and dt_free_align is _aligned_free;")
print("everywhere else both end at free(). Each of these works on Linux and corrupts the")
print("heap on Windows, crashing later in unrelated code.")
sys.exit(1)
