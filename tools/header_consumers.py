#!/usr/bin/env python3
"""Report what each includer of a header actually consumes through it.

`gui/gtk.h` is included by 140 files, but almost none of them want 140 files' worth of
header. Most want two or three symbols; a good number want *nothing it declares* and are
only there for what it drags in transitively (that is how an earlier split attempt broke
`control/control.h`, which was getting `dt_control_t` through this chain and lost it when
the chain was shortened).

Splitting a god-header safely needs that distinction made explicit per file, so this
separates three cases for every includer:

  OWN    - uses a symbol the header itself declares/defines. Needs whichever new header
           that symbol lands in.
  VIA    - uses no own symbol, but uses a symbol from a header this one includes. The
           include is a transitive supply line; the file needs that header from somewhere
           else. Only headers it cannot already reach through its *other* includes are
           reported, so the list is what actually has to be added, not everything it happens
           to touch.
  UNUSED - uses nothing from the header or its transitive closure. The include can go.

Symbols are collected per header (functions, macros, types, enums, struct tags) and matched
against each includer by word-boundary search outside comments and strings. That over-counts
slightly -- a name mentioned in a comment-like context, or a symbol also reachable from a
different header -- so treat VIA as "candidate direct include", not gospel.

Usage:
    tools/header_consumers.py gui/gtk.h [--json] [--only own|via|unused]
"""

import collections
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "src")

COMMENT_BLOCK = re.compile(r"/\*.*?\*/", re.S)
COMMENT_LINE = re.compile(r"//[^\n]*")
STRING_LIT = re.compile(r'"(?:\\.|[^"\\])*"')

INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"')

# What counts as a symbol this header supplies.
DECLARE_PATTERNS = [
    re.compile(r"^\s*#\s*define\s+([A-Za-z_]\w*)"),
    re.compile(r"^\s*}[^;]*?\b([A-Za-z_]\w*)\s*;"),   # `} name;` and `} ATTR(..) name;`
    re.compile(r"^\s*typedef\s+.*?\b([A-Za-z_]\w*)\s*;"),
    re.compile(r"^\s*typedef\s+.*\(\s*\*\s*([A-Za-z_]\w*)\s*\)\s*\("),  # function-pointer typedef       # typedef one-liner
    re.compile(r"^\s*(?:struct|union|enum)\s+([A-Za-z_]\w*)\s*[;{]"),
    # A declaration or definition at file scope: <type stuff> name(
    re.compile(r"^\s*(?:[A-Za-z_][\w \t*]*?[ \t*])([A-Za-z_]\w*)\s*\("),
]

# Enum members are supplied too, and they are how DT_GUI_COLOR_* / DT_UI_CONTAINER_* travel.
ENUM_MEMBER = re.compile(r"^\s*(DT_[A-Z0-9_]+)\s*(?:=|,|$)")

# Keywords and primitive types get matched by the declaration patterns (a `void foo(` line
# yields both) and by every consumer, so they would attribute every file to every header.
NOISE = {
    "if", "for", "while", "switch", "return", "sizeof", "defined", "else", "do",
    "static", "inline", "const", "struct", "union", "enum", "typedef", "extern",
    "void", "int", "char", "float", "double", "long", "short", "unsigned", "signed",
    "gboolean", "gint", "guint", "gchar", "gpointer", "gdouble", "gfloat", "gsize",
    "size_t", "ssize_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
    "int8_t", "int16_t", "int32_t", "int64_t", "va_list", "FILE",
    "TRUE", "FALSE", "NULL",
}


def strip_noise(text):
    text = COMMENT_BLOCK.sub(" ", text)
    text = COMMENT_LINE.sub(" ", text)
    return STRING_LIT.sub('""', text)


def read(path):
    try:
        with open(path, errors="ignore") as fh:
            return fh.read()
    except OSError:
        return ""


def symbols_of(path):
    """Every identifier `path` supplies to whoever includes it."""
    text = strip_noise(read(path))
    found = set()
    for line in text.split("\n"):
        for pat in DECLARE_PATTERNS:
            m = pat.match(line)
            if m:
                found.add(m.group(1))
        m = ENUM_MEMBER.match(line)
        if m:
            found.add(m.group(1))
    return {s for s in found if s not in NOISE and len(s) > 2}


def resolve(inc, from_dir):
    """An include is written either relative to src/ or to the including file's directory."""
    for cand in (os.path.join(SRC, inc), os.path.join(from_dir, inc)):
        cand = os.path.normpath(cand)
        if os.path.isfile(cand):
            return cand
    return None


def closure(header, seen=None):
    """Headers reachable from `header`, excluding itself."""
    if seen is None:
        seen = set()
    for line in read(header).split("\n"):
        m = INCLUDE.match(line)
        if not m:
            continue
        target = resolve(m.group(1), os.path.dirname(header))
        if target and target not in seen:
            seen.add(target)
            closure(target, seen)
    return seen


def walk_sources():
    for dirpath, dirnames, filenames in os.walk(SRC):
        dirnames[:] = [d for d in dirnames if d not in ("external", "build")]
        for fn in filenames:
            if fn.endswith((".h", ".hpp", ".c", ".cc", ".cpp")):
                yield os.path.join(dirpath, fn)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        sys.exit(__doc__)
    rel = args[0]
    as_json = "--json" in sys.argv
    only = None
    if "--only" in sys.argv:
        only = sys.argv[sys.argv.index("--only") + 1]

    header = os.path.join(SRC, rel)
    if not os.path.isfile(header):
        sys.exit(f"error: no such header: {header}")

    own = symbols_of(header)
    # Which included header supplies which symbol, so a VIA finding names its replacement.
    supplier = {}
    for dep in closure(header):
        for sym in symbols_of(dep) - own:
            supplier.setdefault(sym, os.path.relpath(dep, SRC))

    basename = os.path.basename(rel)
    # Both spellings: a project header included as <gui/gtk.h> counts exactly the same, and
    # two headers in this tree are written that way.
    include_re = re.compile(
        r'^\s*#\s*include\s+["<](?:.*/)?' + re.escape(basename) + r'[">]', re.M)

    rows = []
    for path in walk_sources():
        if os.path.samefile(path, header):
            continue
        raw = read(path)
        if not include_re.search(raw):
            continue
        body = strip_noise(include_re.sub(" ", raw))
        words = set(re.findall(r"\b[A-Za-z_]\w*\b", body))

        # What this file can already reach without the header under audit. A symbol available
        # through one of its own other includes is not something it needs to add.
        # Scanned on the raw text: strip_noise() blanks string literals, which is where an
        # include path lives.
        already = set()
        for line in include_re.sub(" ", raw).split("\n"):
            m = INCLUDE.match(line)
            if not m:
                continue
            target = resolve(m.group(1), os.path.dirname(path))
            if target:
                already.add(target)
                already |= closure(target)
        already = {os.path.relpath(h, SRC) for h in already}

        used_own = sorted(own & words)
        used_via = collections.defaultdict(list)
        for sym in words & supplier.keys():
            if supplier[sym] in already:
                continue
            used_via[supplier[sym]].append(sym)

        kind = "own" if used_own else ("via" if used_via else "unused")
        rows.append({
            "file": os.path.relpath(path, REPO),
            "kind": kind,
            "own_symbols": used_own,
            "via": {k: sorted(v) for k, v in sorted(used_via.items())},
        })

    if only:
        rows = [r for r in rows if r["kind"] == only]

    if as_json:
        print(json.dumps(rows, indent=2))
        return 0

    counts = collections.Counter(r["kind"] for r in rows)
    print(f"{len(rows)} file(s) include {rel}: "
          f"{counts['own']} use its own symbols, {counts['via']} only pull others through it, "
          f"{counts['unused']} use nothing\n")

    # Which of this header's own symbols are actually wanted, and by how many files. This is
    # the split plan: symbols nobody uses can just go, and symbols used together tend to
    # belong in the same new header.
    demand = collections.Counter()
    for r in rows:
        demand.update(r["own_symbols"])
    print(f"--- own symbols in demand ({len(demand)} of {len(own)} declared) ---")
    for sym, n in demand.most_common():
        print(f"  {n:4d}  {sym}")
    unwanted = sorted(own - set(demand))
    print(f"\n--- declared but used by no includer ({len(unwanted)}) ---")
    print("  " + ", ".join(unwanted) if unwanted else "  (none)")

    print(f"\n--- files that only pull other headers through it ({counts['via']}) ---")
    for r in sorted(rows, key=lambda x: x["file"]):
        if r["kind"] != "via":
            continue
        print(f"  {r['file']}")
        for hdr, syms in r["via"].items():
            print(f"      {hdr}: {', '.join(syms[:6])}"
                  + (" ..." if len(syms) > 6 else ""))

    print(f"\n--- files using nothing from it ({counts['unused']}) ---")
    for r in sorted(rows, key=lambda x: x["file"]):
        if r["kind"] == "unused":
            print(f"  {r['file']}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
