#!/usr/bin/env python3
"""Add the include that supplies each symbol the compiler says is missing.

Removing a god-header leaves files that were quietly living off what it dragged in. The
compiler names every one of them precisely -- `implicit declaration of function 'dt_print'`,
`'DT_DEBUG_CONTROL' undeclared` -- so the repair is mechanical: find which header declares the
symbol, add it, repeat until the build is clean.

Reads a compiler log on stdin (or a file), extracts the missing symbols per file, resolves
each against an index of every header under src/, and inserts the includes. A symbol declared
in more than one header is resolved by preferring the lowest layer -- a leaf library over a
module that re-exports it -- which is the include you want anyway.

Usage:
    ninja -k 0 2>&1 | tools/fix_missing_includes.py [--dry-run]
"""

import collections
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "src")

# Same order as tools/include_graph.py: lower is more fundamental, and a more fundamental
# header is the better place to take a symbol from.
LAYER = {
    'external': 0, 'win': 0, 'system': 0,
    'common': 1, 'math': 1,
    'pixel': 2,
    'control': 3,
    'gui': 4, 'widgets': 4,
    'develop': 5,
    'iop': 6, 'imageio': 6,
    'libs': 7, 'views': 7, 'chart': 7,
    'apps': 10,
}

# Symbols that come from outside the tree. Without these the index guesses at whichever
# project header happens to mention the name, which is how a layer-1 header gets told to
# include control/jobs.h because it used a GList.
EXTERNAL = [
    (re.compile(r"^(?:GList|GSList|GHashTable|GError|GString|GArray|GPtrArray|GQueue|"
                r"GValue|GObject|GType|GFile|GTimer|GThread|GMutex|GCond|GRegex|GKeyFile|"
                r"GDateTime|GMainLoop|GMainContext|GSource|GBytes|GVariant|GCancellable|"
                r"g_[a-z_]+|G_[A-Z_]+)$"), "<glib.h>"),
    (re.compile(r"^(?:Gtk\w+|Gdk\w+|Pango\w+|gtk_\w+|gdk_\w+|pango_\w+|GTK_\w+|GDK_\w+|"
                r"PANGO_\w+)$"), "<gtk/gtk.h>"),
    (re.compile(r"^(?:cairo_\w+|CAIRO_\w+)$"), "<cairo.h>"),
    (re.compile(r"^(?:u?int(?:8|16|32|64)_t|u?intptr_t|SIZE_MAX|(?:U?INT(?:8|16|32|64)_(?:MAX|MIN)))$"),
     "<stdint.h>"),
    (re.compile(r"^(?:_|N_|Q_|C_)$"), "<glib/gi18n.h>"),
    (re.compile(r"^(?:printf|fprintf|snprintf|vsnprintf|fopen|fclose|fflush|stdout|stderr)$"),
     "<stdio.h>"),
    (re.compile(r"^(?:malloc|calloc|realloc|free|abs|qsort|getenv|exit)$"), "<stdlib.h>"),
    (re.compile(r"^(?:memcpy|memset|strlen|strcmp|strncmp|strdup|strstr|strchr)$"), "<string.h>"),
]


def external_header(sym):
    for pat, header in EXTERNAL:
        if pat.match(sym):
            return header
    return None


MISSING = [
    re.compile(r"^(?P<file>[^:]+):\d+:\d+: (?:error|warning): implicit declaration of function '(?P<sym>\w+)'"),
    re.compile(r"^(?P<file>[^:]+):\d+:\d+: error: '(?P<sym>\w+)' undeclared"),
    re.compile(r"^(?P<file>[^:]+):\d+:\d+: error: unknown type name '(?P<sym>\w+)'"),
    re.compile(r"^(?P<file>[^:]+):\d+:\d+: error: '(?P<sym>\w+)' was not declared in this scope"),
]

DECLARE = [
    re.compile(r"^\s*#\s*define\s+([A-Za-z_]\w*)"),
    re.compile(r"^\s*}[^;]*?\b([A-Za-z_]\w*)\s*;"),   # `} name;` and `} ATTR(..) name;`
    re.compile(r"^\s*typedef\s+.*?\b([A-Za-z_]\w*)\s*;"),
    re.compile(r"^\s*typedef\s+.*\(\s*\*\s*([A-Za-z_]\w*)\s*\)\s*\("),  # function-pointer typedef
    re.compile(r"^\s*(?:struct|union|enum)\s+([A-Za-z_]\w*)\s*[;{]"),
    re.compile(r"^\s*(?:[A-Za-z_][\w \t*]*?[ \t*])([A-Za-z_]\w*)\s*\("),
]
ENUM_MEMBER = re.compile(r"^\s*([A-Z][A-Z0-9_]{2,})\s*(?:=|,|$)")

COMMENT_BLOCK = re.compile(r"/\*.*?\*/", re.S)
COMMENT_LINE = re.compile(r"//[^\n]*")


def layer_of(rel):
    parts = rel.split(os.sep)
    return LAYER.get(parts[0], 9) if len(parts) > 1 else 9


def build_index():
    """symbol -> [header relpaths], best candidate first."""
    index = collections.defaultdict(list)
    for dirpath, dirnames, filenames in os.walk(SRC):
        dirnames[:] = [d for d in dirnames if d not in ("external", "build")]
        for fn in filenames:
            if not fn.endswith((".h", ".hpp")) or fn.endswith(".cmake.h"):
                continue
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, SRC)
            try:
                with open(full, errors="ignore") as fh:
                    text = fh.read()
            except OSError:
                continue
            text = COMMENT_LINE.sub(" ", COMMENT_BLOCK.sub(" ", text))
            for line in text.split("\n"):
                for pat in DECLARE:
                    m = pat.match(line)
                    if m:
                        index[m.group(1)].append(rel)
                m = ENUM_MEMBER.match(line)
                if m:
                    index[m.group(1)].append(rel)
    for sym, headers in index.items():
        # Deduplicate, then prefer the lowest layer and, within a layer, the shortest path --
        # `common/logging.h` over `common/some/deep/wrapper.h`.
        seen = sorted(set(headers), key=lambda h: (layer_of(h), h.count(os.sep), len(h)))
        index[sym] = seen
    return index


def _conditional_depth(text, offset):
    """How many #if/#ifdef blocks enclose `offset`."""
    depth = 0
    for m in re.finditer(r'^\s*#\s*(if|ifdef|ifndef|endif)\b', text[:offset], re.M):
        depth += -1 if m.group(1) == "endif" else 1
    return max(0, depth)


def insert_includes(path, headers):
    with open(path) as fh:
        text = fh.read()
    existing = set(re.findall(r'^\s*#\s*include\s+["<]([^">]+)[">]', text, re.M))
    todo = [h for h in headers if h.strip("<>") not in existing]
    if not todo:
        return []
    block = "".join(f"#include {h}\n" if h.startswith("<") else f'#include "{h}"\n'
                    for h in sorted(todo))

    # Insert into the file's *leading* include block. Anchoring on the last include anywhere
    # is wrong: several files re-include a header mid-file on purpose (iop/iop_api.h is
    # expanded twice in develop/imageop.c), and an include placed there is below every use.
    first_code = len(text)
    for m in re.finditer(r'^[A-Za-z_].*$', text, re.M):
        line = m.group(0)
        if line.startswith(("#", "//", "/*", "*")):
            continue
        if re.match(r'^(?:extern|G_BEGIN_DECLS|G_END_DECLS)\b', line):
            continue
        first_code = m.start()
        break

    # Only anchor on an include that is unconditionally compiled. Landing inside an
    # `#ifdef GDK_WINDOWING_QUARTZ` block -- which is where the last include of several files
    # sits -- means the include silently does nothing on every other platform, and the symbol
    # stays missing with no new error to explain why.
    anchors = [m for m in re.finditer(r'^\s*#\s*include\s+["<][^">]+[">].*\n', text, re.M)
               if m.end() <= first_code and _conditional_depth(text, m.start()) == 0]
    if anchors:
        at = anchors[-1].end()
    else:
        guard = re.search(r'^\s*#\s*define\s+\w+_H\w*\s*\n', text, re.M)
        at = guard.end() if guard else 0
    with open(path, "w") as fh:
        fh.write(text[:at] + block + text[at:])
    return todo


def main():
    dry = "--dry-run" in sys.argv
    log = sys.stdin.read()

    wanted = collections.defaultdict(set)
    for line in log.split("\n"):
        line = line.replace(REPO + "/", "")
        for pat in MISSING:
            m = pat.match(line.strip())
            if m:
                wanted[m.group("file")].add(m.group("sym"))
                break

    if not wanted:
        print("no missing symbols found in the log")
        return 0

    index = build_index()
    unresolved = collections.Counter()
    for path, syms in sorted(wanted.items()):
        if not os.path.exists(path):
            continue
        self_header = os.path.splitext(path)[0] + ".h"
        headers = []
        for sym in sorted(syms):
            ext = external_header(sym)
            if ext:
                headers.append(ext)
                continue
            cands = index.get(sym)
            if not cands:
                unresolved[sym] += 1
                continue
            # Never suggest the file's own header: if the symbol were there it would resolve.
            pick = next((c for c in cands
                         if os.path.normpath(os.path.join(SRC, c)) != os.path.normpath(self_header)),
                        None)
            if pick:
                headers.append(pick)
        headers = sorted(set(headers))
        if not headers:
            continue
        if dry:
            print(f"{path}: + {', '.join(headers)}")
        else:
            added = insert_includes(path, headers)
            if added:
                print(f"{path}: + {', '.join(added)}")

    if unresolved:
        print("\nunresolved symbols (no header declares them):", file=sys.stderr)
        for sym, n in unresolved.most_common(20):
            print(f"  {n:3d}  {sym}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
