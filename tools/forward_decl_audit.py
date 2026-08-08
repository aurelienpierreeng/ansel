#!/usr/bin/env python3
"""Find forward declarations that let a header reach a type from a HIGHER layer.

A forward declaration is normally a good thing: `struct dt_foo_t;` lets a header mention a
type without including the header that defines it, which is how include graphs stay small.
But it also silently defeats the layering check, because there is no #include to count.

`pixel/format.h` was the case that prompted this: it forward-declared `dt_iop_module_t`,
`dt_dev_pixelpipe_t` and `dt_dev_pixelpipe_iop_t` -- three develop/ types, two layers up --
purely so a layer-2 header could declare three functions over them. `tools/include_graph.py`
saw nothing, because nothing was included.

This reports every forward declaration whose type is really defined at a higher layer. Most
findings are legitimate opaque handles: passing a pointer through without touching its
fields costs nothing and creates no real dependency. The ones to look at are where the header
also *declares functions* over the type -- that is the shape that turned out to be an API in
the wrong place.

Usage:
    tools/forward_decl_audit.py [--all] [--json]

--all also lists same-layer and downward forward declarations, which are never a problem.
"""

import collections
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "src")

# Kept in step with tools/include_graph.py deliberately: a second, disagreeing copy of the
# layer order would be worse than none.
LAYERS = [
    ('external', 0), ('win', 0), ('system', 0),
    ('common', 1), ('math', 1), ('colorprofiles', 1),
    ('pixel', 2),
    ('control', 3),
    ('gui', 4), ('widgets', 4),
    ('develop', 5),
    ('iop', 6), ('imageio', 6),
    ('libs', 7), ('views', 7), ('chart', 7),
    ('apps', 10),
    ('app', 9),
]
LAYER = dict(LAYERS)

FWD = re.compile(r'^\s*(?:struct|union|enum)\s+([A-Za-z_]\w*)\s*;\s*$')
# `} name;` closes a struct/union/enum definition; `struct name {` opens one.
DEF_CLOSE = re.compile(r'^\s*\}\s*([A-Za-z_]\w*)\s*;')
DEF_OPEN = re.compile(r'^\s*(?:typedef\s+)?(?:struct|union|enum)\s+([A-Za-z_]\w*)\s*\{')


COMMENT_BLOCK = re.compile(r'/\*.*?\*/', re.S)
COMMENT_LINE = re.compile(r'//[^\n]*')


def count_mentions(path, name):
    """Lines mentioning `name` outside comments and outside its own forward declaration."""
    try:
        text = open(path, errors="ignore").read()
    except OSError:
        return 0
    text = COMMENT_BLOCK.sub("", text)
    text = COMMENT_LINE.sub("", text)
    pat = re.compile(r'\b' + re.escape(name) + r'\b')
    return sum(1 for line in text.split("\n")
               if pat.search(line) and not FWD.match(line))


def layer_of(relpath):
    parts = relpath.split(os.sep)
    if len(parts) < 2:
        return None
    if len(parts) == 2:
        return LAYER['app']
    return LAYER.get(parts[1])


def walk_sources():
    for dirpath, dirnames, filenames in os.walk(SRC):
        dirnames[:] = [d for d in dirnames if d not in ("external", "build")]
        for fn in filenames:
            if fn.endswith((".h", ".hpp", ".c", ".cc", ".cpp")):
                full = os.path.join(dirpath, fn)
                yield os.path.relpath(full, REPO), full


def main():
    show_all = "--all" in sys.argv
    as_json = "--json" in sys.argv

    definitions = {}                      # type name -> relpath where it is defined
    forwards = collections.defaultdict(list)   # type name -> [(relpath, line)]

    for rel, full in walk_sources():
        try:
            lines = open(full, errors="ignore").read().split("\n")
        except OSError:
            continue
        is_header = rel.endswith((".h", ".hpp"))
        for n, line in enumerate(lines, 1):
            m = FWD.match(line)
            if m and is_header:
                forwards[m.group(1)].append((rel, n))
                continue
            for pat in (DEF_CLOSE, DEF_OPEN):
                d = pat.match(line)
                if d:
                    # First definition wins; headers are walked before their .c in practice,
                    # and a type defined twice is a different problem than this one.
                    definitions.setdefault(d.group(1), rel)

    findings = []
    for name, places in forwards.items():
        home = definitions.get(name)
        if not home:
            continue                       # opaque everywhere, or defined outside src/
        home_layer = layer_of(home)
        if home_layer is None:
            continue
        for rel, line in places:
            here = layer_of(rel)
            if here is None or rel == home:
                continue
            upward = home_layer > here
            if upward or show_all:
                # Does this header also declare functions mentioning the type? That is what
                # separates "opaque pointer passed through" from "API declared in the wrong
                # place", and it is the difference worth acting on.
                #
                # Counted by mentions outside comments, NOT by matching a prototype: this
                # codebase wraps prototypes across lines, and a single-line regex reported
                # common/iop_profile.h as bare when it declares six functions over the type.
                uses = count_mentions(os.path.join(REPO, rel), name)
                findings.append({
                    "type": name,
                    "forward_declared_in": f"{rel}:{line}",
                    "declaring_layer": here,
                    "defined_in": home,
                    "defining_layer": home_layer,
                    "upward": upward,
                    "prototypes_using_it": uses,
                })

    if as_json:
        print(json.dumps(findings, indent=2))
        return 0

    up = [f for f in findings if f["upward"]]
    with_api = [f for f in up if f["prototypes_using_it"] > 0]
    passthrough = [f for f in up if f["prototypes_using_it"] == 0]

    print(f"{len(up)} forward declaration(s) reaching a higher layer "
          f"({len(with_api)} with prototypes over the type, {len(passthrough)} bare)\n")

    print("--- reaching UP and used in prototypes: an API that may be in the wrong place ---")
    for f in sorted(with_api, key=lambda x: (-x["defining_layer"] + x["declaring_layer"],
                                             x["forward_declared_in"])):
        print(f"  {f['type']}  (layer {f['declaring_layer']} -> {f['defining_layer']}, "
              f"{f['prototypes_using_it']} prototype(s))")
        print(f"      declared {f['forward_declared_in']}")
        print(f"      defined  {f['defined_in']}")

    print(f"\n--- reaching UP, bare (opaque handles; usually fine) : {len(passthrough)} ---")
    by_pair = collections.Counter(
        (f["forward_declared_in"].split(":")[0], f["defined_in"]) for f in passthrough)
    for (h, d), n in by_pair.most_common(20):
        print(f"  {n:3d}  {h}  ->  {d}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
