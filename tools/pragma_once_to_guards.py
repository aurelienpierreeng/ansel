#!/usr/bin/env python3
"""Replace `#pragma once` with explicit include guards across src/.

`#pragma once` silently makes a cyclic include graph compile: a header that is
re-entered mid-definition is simply skipped, leaving the first inclusion to finish
with whatever it had. Explicit guards behave the same way at the preprocessor level,
but they are greppable, portable, and -- crucially -- they make the anti-pattern
visible in review instead of hiding it.

Guard names are derived from the path relative to src/, e.g.
  src/develop/masks/masks_history.h  ->  DT_DEVELOP_MASKS_MASKS_HISTORY_H

Usage:
  python3 tools/pragma_once_to_guards.py --check        # list what would change, touch nothing
  python3 tools/pragma_once_to_guards.py                # rewrite in place
  python3 tools/pragma_once_to_guards.py --add-missing  # also guard headers that have NO guard
  python3 tools/pragma_once_to_guards.py --verify       # exit 1 if any #pragma once came back
"""
import os
import re
import sys

SRC = 'src'
SKIP_DIRS = {'external'}
PRAGMA_RE = re.compile(r'^[ \t]*#[ \t]*pragma[ \t]+once[ \t]*\r?\n', re.M)
GUARD_RE = re.compile(r'^[ \t]*#[ \t]*ifndef[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]*\r?\n'
                      r'[ \t]*#[ \t]*define[ \t]+', re.M)

# X-macro headers: deliberately re-included several times in the SAME translation unit
# with different macros defined, and expanded inside struct bodies to generate members.
# They must have NEITHER a guard NOR any #include of their own.
XMACRO_HEADERS = {
    'src/common/module_api.h',
    'src/views/view_api.h',
    'src/libs/lib_api.h',
    'src/imageio/format/imageio_format_api.h',
    'src/imageio/storage/imageio_storage_api.h',
}

# Trailing editor modelines are conventionally the last thing in these files; the
# #endif has to go above them to stay inside the guarded region only if the file's
# content does. Keeping the modeline block outside the guard is harmless and matches
# how the hand-written guards in the tree already look.
MODELINE = '// clang-format off\n// modelines:'


def guard_name(path):
    rel = os.path.relpath(path, SRC)
    return 'DT_' + re.sub(r'[^A-Za-z0-9]', '_', rel).upper()


def headers():
    for root, dirs, names in os.walk(SRC):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for n in names:
            if n.endswith(('.h', '.hpp')):
                yield os.path.join(root, n)


def convert(text, guard):
    m = PRAGMA_RE.search(text)
    if not m:
        return None
    # Count DIRECTIVES, not substrings: several headers legitimately mention
    # "#pragma once" inside an explanatory comment.
    if len(PRAGMA_RE.findall(text)) > 1:
        raise ValueError('more than one #pragma once directive')

    head = text[:m.start()]
    tail = text[m.end():]
    opening = '#ifndef %s\n#define %s\n' % (guard, guard)

    idx = tail.rfind(MODELINE)
    if idx == -1:
        closing = '\n#endif // %s\n' % guard
        return head + opening + tail.rstrip('\n') + closing
    # place #endif just above the trailing modeline block
    body, trailer = tail[:idx], tail[idx:]
    return head + opening + body.rstrip('\n') + '\n\n#endif // %s\n\n' % guard + trailer


def wrap_unguarded(text, guard):
    """Guard a header that has neither #pragma once nor an #ifndef/#define pair.

    The opening goes after the leading licence block comment (so the guard wraps the
    actual content, not the whole file including its header comment); the #endif goes
    just above the trailing modeline block, mirroring convert().
    """
    start = 0
    stripped = text.lstrip()
    if stripped.startswith('/*'):
        end = text.find('*/')
        if end != -1:
            start = text.index('\n', end) + 1 if '\n' in text[end:] else len(text)

    opening = '\n#ifndef %s\n#define %s\n' % (guard, guard)
    head, rest = text[:start], text[start:]

    idx = rest.rfind(MODELINE)
    if idx == -1:
        return head + opening + rest.rstrip('\n') + '\n\n#endif // %s\n' % guard
    body, trailer = rest[:idx], rest[idx:]
    return head + opening + body.rstrip('\n') + '\n\n#endif // %s\n\n' % guard + trailer


def main():
    check = '--check' in sys.argv
    add_missing = '--add-missing' in sys.argv

    if '--verify' in sys.argv:
        offenders = [p for p in sorted(headers())
                     if PRAGMA_RE.search(open(p, encoding='utf-8').read())]
        for p in offenders:
            print('%s: #pragma once is forbidden, use an include guard' % p, file=sys.stderr)
        return 1 if offenders else 0

    changed = skipped = 0
    seen = {}
    for p in sorted(headers()):
        text = open(p, encoding='utf-8').read()
        if '#pragma once' not in text:
            if not add_missing or p.replace(os.sep, '/') in XMACRO_HEADERS:
                continue
            if GUARD_RE.search(text):
                continue
            g = guard_name(p)
            changed += 1
            if check:
                print('%s -> %s (was UNGUARDED)' % (p, g))
            else:
                open(p, 'w', encoding='utf-8').write(wrap_unguarded(text, g))
            continue
        g = guard_name(p)
        if g in seen:
            print('COLLISION: %s and %s both map to %s' % (p, seen[g], g), file=sys.stderr)
            return 1
        seen[g] = p
        try:
            out = convert(text, g)
        except ValueError as e:
            print('SKIP %s: %s' % (p, e), file=sys.stderr)
            skipped += 1
            continue
        if out is None:
            continue
        changed += 1
        if check:
            print('%s -> %s' % (p, g))
        else:
            open(p, 'w', encoding='utf-8').write(out)
    print('%s %d headers (%d skipped)' % ('would convert' if check else 'converted', changed, skipped))
    return 0


if __name__ == '__main__':
    sys.exit(main())
