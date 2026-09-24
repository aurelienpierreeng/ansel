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

# Resolved from the tool's own location, not from the working directory: CI runs the
# gates from wherever the build left it, and a sweep that silently finds no header is
# a gate that silently passes.
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'src')
SKIP_DIRS = {'external'}
# Every spelling a header can carry in this tree. The C++ ones are rare -- one file
# as of this writing -- which is exactly why they have to be listed: a rule enforced
# over `.h` alone is a rule a `.hh` walks straight through, and the extension is part
# of the guard name, so no two spellings of one basename can collide.
HEADER_SUFFIXES = ('.h', '.hh', '.hpp', '.hxx')
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

# darktable.h has a TRIPWIRE, not a guard: it #errors on re-inclusion, because a second
# inclusion in one translation unit means the header arrived through a path nobody
# intended. Guarding it would absorb that silently -- and the guard's own #define would
# collide with the macro the tripwire tests. It must never be guarded.
TRIPWIRE_HEADERS = {
    'src/darktable.h',
}

# Everything that must stay without an #ifndef guard, for its own reason.
UNGUARDED_BY_DESIGN = XMACRO_HEADERS | TRIPWIRE_HEADERS

# Trailing editor modelines are conventionally the last thing in these files; the
# #endif has to go above them to stay inside the guarded region only if the file's
# content does. Keeping the modeline block outside the guard is harmless and matches
# how the hand-written guards in the tree already look.
#
# Two spellings open that block: most files start it with `// clang-format off`, the
# oldest ones inherited from darktable start straight at `// modelines:`. Looking only
# for the long one puts the #endif BELOW the block on exactly those files. Longest
# marker first, so a file carrying both is split above the clang-format line rather
# than inside the block.
MODELINES = ('// clang-format off\n// modelines:', '// modelines:')


def rel(path):
    return os.path.relpath(path, ROOT).replace(os.sep, '/')


def trailer_index(text):
    """Where the trailing modeline block starts, or -1."""
    for marker in MODELINES:
        idx = text.rfind(marker)
        if idx != -1:
            return idx
    return -1


def guard_name(path):
    from_src = os.path.relpath(path, SRC)
    return 'DT_' + re.sub(r'[^A-Za-z0-9]', '_', from_src).upper()


def headers():
    for root, dirs, names in os.walk(SRC):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for n in names:
            if n.endswith(HEADER_SUFFIXES):
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

    idx = trailer_index(tail)
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

    idx = trailer_index(rest)
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
            print('%s: #pragma once is forbidden, use an include guard' % rel(p), file=sys.stderr)
        return 1 if offenders else 0

    changed = skipped = 0
    seen = {}
    for p in sorted(headers()):
        text = open(p, encoding='utf-8').read()
        if '#pragma once' not in text:
            if not add_missing or rel(p) in UNGUARDED_BY_DESIGN:
                continue
            if GUARD_RE.search(text):
                continue
            g = guard_name(p)
            changed += 1
            if check:
                print('%s -> %s (was UNGUARDED)' % (rel(p), g))
            else:
                open(p, 'w', encoding='utf-8').write(wrap_unguarded(text, g))
            continue
        g = guard_name(p)
        if g in seen:
            print('COLLISION: %s and %s both map to %s' % (rel(p), rel(seen[g]), g), file=sys.stderr)
            return 1
        seen[g] = p
        try:
            out = convert(text, g)
        except ValueError as e:
            print('SKIP %s: %s' % (rel(p), e), file=sys.stderr)
            skipped += 1
            continue
        if out is None:
            continue
        changed += 1
        if check:
            print('%s -> %s' % (rel(p), g))
        else:
            open(p, 'w', encoding='utf-8').write(out)
    print('%s %d headers (%d skipped)' % ('would convert' if check else 'converted', changed, skipped))
    return 0


if __name__ == '__main__':
    sys.exit(main())
