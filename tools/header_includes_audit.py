#!/usr/bin/env python3
"""Check that a header includes only what its own declarations need.

A header that includes more than its signatures require becomes a supply line its consumers
never asked for and cannot see: they compile because something upstream happened to pull in
what they use, and the day anyone tidies that include away the breakage surfaces somewhere
else entirely, in a file that was never touched.

Two findings, opposite directions:

  UNUSED  - the header includes something none of its own declarations reference. Its
            consumers may nonetheless be relying on it; that is the problem, not a reason to
            keep it. Move the include to the .c, and give each consumer what it actually uses.
  MISSING - the header names a type it does not include a definition for, and is getting it
            transitively. It works today and breaks when the chain shortens.

Reported symbol by symbol so each finding can be checked rather than trusted. Matching is
textual -- it cannot see through macros, and it reads one preprocessor branch like every other
tool here -- so treat MISSING as "verify this", and check UNUSED against a build before acting
on it. A platform-only use (`#ifdef _WIN32`) will read as UNUSED on Linux; removing such an
include is how this tree broke its Windows build once already.

Usage:
    tools/header_includes_audit.py src/gui/application.h [more headers...]
    tools/header_includes_audit.py --all          # every header under src/
"""

import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "src")

COMMENT_BLOCK = re.compile(r"/\*.*?\*/", re.S)
COMMENT_LINE = re.compile(r"//[^\n]*")
INCLUDE_LINE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)

DECLARE = [
    re.compile(r"^\s*#\s*define\s+([A-Za-z_]\w*)"),
    re.compile(r"^\s*}[^;]*?\b([A-Za-z_]\w*)\s*;"),   # `} name;` and `} ATTR(..) name;`
    re.compile(r"^\s*typedef\s+.*?\b([A-Za-z_]\w*)\s*;"),
    re.compile(r"^\s*typedef\s+.*\(\s*\*\s*([A-Za-z_]\w*)\s*\)\s*\("),  # function-pointer typedef
    re.compile(r"^\s*(?:struct|union|enum)\s+([A-Za-z_]\w*)\s*[;{]"),
    re.compile(r"^\s*(?:[A-Za-z_][\w \t*]*?[ \t*])([A-Za-z_]\w*)\s*\("),
]
ENUM_MEMBER = re.compile(r"^\s*([A-Z][A-Z0-9_]{2,})\s*(?:=|,|$)")

# Keywords and primitives are matched by both sides and would pair every header with every
# other one.
NOISE = {
    "if", "for", "while", "switch", "return", "sizeof", "defined", "else", "do",
    "static", "inline", "const", "struct", "union", "enum", "typedef", "extern",
    "void", "int", "char", "float", "double", "long", "short", "unsigned", "signed",
    "gboolean", "gint", "guint", "gchar", "gpointer", "gdouble", "gfloat", "gsize",
    "TRUE", "FALSE", "NULL",
}


def strip_comments(text):
    return COMMENT_LINE.sub(" ", COMMENT_BLOCK.sub(" ", text))


def read(path):
    try:
        with open(path, errors="ignore") as fh:
            return fh.read()
    except OSError:
        return ""


def supplied_by(path):
    """Identifiers `path` defines."""
    found = set()
    for line in strip_comments(read(path)).split("\n"):
        for pat in DECLARE:
            m = pat.match(line)
            if m:
                found.add(m.group(1))
        m = ENUM_MEMBER.match(line)
        if m:
            found.add(m.group(1))
    return {s for s in found if s not in NOISE and len(s) > 2}


def resolve(inc, from_dir):
    for cand in (os.path.join(SRC, inc), os.path.join(from_dir, inc)):
        cand = os.path.normpath(cand)
        if os.path.isfile(cand):
            return cand
    return None


def closure(header, seen=None):
    if seen is None:
        seen = set()
    for inc in INCLUDE_LINE.findall(read(header)):
        target = resolve(inc, os.path.dirname(header))
        if target and target not in seen:
            seen.add(target)
            closure(target, seen)
    return seen


def audit(header):
    text = read(header)
    body = strip_comments(text)
    body = re.sub(r"^\s*#\s*include.*$", " ", body, flags=re.M)
    words = set(re.findall(r"\b[A-Za-z_]\w*\b", body))

    rel = os.path.relpath(header, REPO)
    unused, used = [], []
    for inc in INCLUDE_LINE.findall(text):
        target = resolve(inc, os.path.dirname(header))
        if not target:
            continue
        hit = sorted(supplied_by(target) & words)
        (used if hit else unused).append((inc, hit))

    # A type the header names but no direct include defines: arriving transitively.
    direct = set()
    for inc in INCLUDE_LINE.findall(text):
        target = resolve(inc, os.path.dirname(header))
        if target:
            direct |= supplied_by(target)
    missing = []
    for dep in closure(header):
        for sym in (supplied_by(dep) & words) - direct:
            missing.append((sym, os.path.relpath(dep, SRC)))

    if not unused and not missing:
        return 0
    print(f"--- {rel} ---")
    for inc, _ in unused:
        print(f"  UNUSED   {inc}  (no declaration here references it)")
    for sym, owner in sorted(set(missing)):
        print(f"  MISSING  {sym}  -- reached transitively; {owner} defines it")
    for inc, hit in used:
        print(f"  ok       {inc}  <- {', '.join(hit[:6])}")
    print()
    return len(unused) + len(set(missing))


def main():
    if "--all" in sys.argv:
        headers = []
        for dirpath, dirnames, filenames in os.walk(SRC):
            dirnames[:] = [d for d in dirnames if d not in ("external", "build")]
            headers += [os.path.join(dirpath, f) for f in filenames if f.endswith(".h")]
    else:
        headers = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not headers:
        sys.exit(__doc__)

    total = sum(audit(h) for h in sorted(headers))
    print(f"{total} finding(s) across {len(headers)} header(s).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
