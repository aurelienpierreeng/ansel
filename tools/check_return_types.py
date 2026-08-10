#!/usr/bin/env python3
"""Find `return FALSE` in a function that returns a pointer, and its mirror.

The compiler cannot help here. GLib defines `FALSE` as `0`, and `0` is a valid null
pointer constant in C -- so `return FALSE;` from a `char *` function compiles silently and
means exactly `return NULL;`. It is only wrong to a reader, which is the worst kind of
wrong: it says "this function answers a yes/no question" about a function that hands back
an object.

The mirror cases are already covered by the compiler: `return TRUE;` from a pointer
function and `return NULL;` from an integer one both trip -Wint-conversion. This script
deliberately reports ONLY the FALSE-for-NULL direction, because it is the only one nothing
else catches -- and because the mirror cannot be checked textually anyway. `cmsHPROFILE`,
`cl_mem` and `gpointer` are pointers with no `*` in the spelling, so a check for
"`return NULL` in a non-pointer function" reports seventeen correct lines in this tree and
nothing else. A gate that cries wolf gets switched off.

    tools/check_return_types.py [path ...]        # default: src/

Exits non-zero if anything is found.
"""
import os
import re
import sys

# A function definition: return type and name at column 0, the parameter list, then a
# brace on that line or the next. Deliberately conservative -- it is better to miss an
# oddly-formatted definition than to mis-parse a macro and report nonsense.
DEF_RE = re.compile(
    r'^(?P<ret>[A-Za-z_][\w \t]*?[\w \t*]*?)'      # return type, possibly with * at the end
    r'(?P<name>[A-Za-z_]\w*)'                      # function name
    r'\s*\((?P<args>[^;]*?)\)\s*$',                # parameter list, no trailing semicolon
    re.M)

SKIP_DIRS = {'external', 'attic'}


def _strip_comments(text):
    """Blank out comments, preserving every byte offset and line number.

    Necessary, not cosmetic: an apostrophe in a comment -- "can't", "doesn't", which this
    codebase is full of -- reads as an opening character literal to the scanner below, and
    everything up to the next apostrophe gets skipped, braces included. The first version
    of this script reported five confident findings in mipmap_cache.c that were all that
    bug swallowing a function boundary.
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        two = text[i:i + 2]
        if two == '/*':
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(''.join('\n' if c == '\n' else ' ' for c in text[i:j]))
            i = j
        elif two == '//':
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
        elif text[i] in ('"', "'"):
            quote, j = text[i], i + 1
            while j < n and text[j] != quote:
                j += 2 if text[j] == '\\' else 1
            j = min(j + 1, n)
            out.append(text[i:j])
            i = j
        else:
            out.append(text[i])
            i += 1
    return ''.join(out)


def _body_of(text, open_brace):
    """Return (body, offset_of_body_start) for the block opening at `open_brace`."""
    depth, i, n = 0, open_brace, len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            quote, i = c, i + 1
            while i < n and text[i] != quote:
                i += 2 if text[i] == '\\' else 1
        elif c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return text[open_brace + 1:i], open_brace + 1
        i += 1
    return '', open_brace + 1


def _returns_pointer(ret):
    ret = ret.strip()
    if ret.endswith('*'):
        return True
    # `GList *foo(` puts the star on the name side; the regex hands us "GList *"
    return '*' in ret


def scan(path):
    """Report every return-value/return-type mismatch in one file."""
    try:
        with open(path, encoding='utf-8', errors='replace') as fp:
            text = _strip_comments(fp.read())
    except OSError:
        return []

    findings = []
    for m in DEF_RE.finditer(text):
        ret = m.group('ret')
        if not ret.strip():
            continue
        # skip control keywords that look like definitions (`if (...)` etc.)
        if re.match(r'\s*(if|for|while|switch|return|else)\b', m.group(0)):
            continue

        # the body must open on this line or the next non-blank one
        after = text[m.end():]
        brace = after.find('{')
        if brace < 0 or after[:brace].strip():
            continue

        body, body_start = _body_of(text, m.end() + brace)
        pointer = _returns_pointer(ret)

        for rm in re.finditer(r'\breturn\s+(TRUE|FALSE|NULL)\s*;', body):
            what = rm.group(1)
            # absolute offset, so the line number cannot drift with formatting
            line = text[:body_start + rm.start()].count('\n') + 1
            if pointer and what in ('TRUE', 'FALSE'):
                findings.append((path, line, m.group('name'), ret.strip(), what, 'NULL'))
    return findings


def main():
    """Scan the given paths (default src/) and print what does not line up."""
    roots = sys.argv[1:] or ['src']
    findings = []
    for root in roots:
        if os.path.isfile(root):
            findings += scan(root)
            continue
        for dirpath, dirnames, names in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
            for n in names:
                if n.endswith(('.c', '.cc', '.cpp', '.h')):
                    findings += scan(os.path.join(dirpath, n))

    for path, line, name, ret, what, want in sorted(findings):
        print(f'{path}:{line}: {name}() returns `{ret}` but says `return {what};` '
              f'-- should be `return {want};`')

    if findings:
        print(f'\n{len(findings)} mismatched return value(s).')
        return 1

    print('OK: no return-value/return-type mismatches.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
