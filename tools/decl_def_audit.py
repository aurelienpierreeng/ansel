#!/usr/bin/env python3
"""Find functions declared in one module's header but defined in another module's source.

The convention this checks is `example.h` declares, `example.c` defines. Where a definition
sits somewhere else, the definition is not where anyone looks for it and nothing warns --
`common/history_merge.c` carried two such cases in opposite directions, and neither surfaced
until an unrelated include had to be removed (roadmap section 13).

Parsing is Universal Ctags, not a regex: prototypes (kind `prototype`) are declarations,
`function` tags in a .c/.cc are definitions. Requires ctags on PATH; this is an audit that
produces a list to read, not a CI gate, so that dependency is fine.

Usage:
    tools/decl_def_audit.py [--all] [--json]

By default only *cross-module* mismatches are reported -- a definition in a different
directory than its declaring header, which is where the real traps live. --all additionally
lists same-directory mismatches (declared in a.h, defined in b.c next door), which are often
deliberate.
"""

import collections
import json
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "src")


def run_ctags():
    cmd = [
        "ctags", "--languages=C,C++",
        "--kinds-C=+p", "--kinds-C++=+p",
        "--fields=+n", "--output-format=json",
        "-R", "--exclude=external", "--exclude=build", "src/",
    ]
    try:
        out = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True, check=True).stdout
    except FileNotFoundError:
        sys.exit("error: ctags not found. Install universal-ctags.")
    except subprocess.CalledProcessError as e:
        sys.exit(f"error: ctags failed: {e.stderr[:400]}")

    for line in out.splitlines():
        if not line.startswith("{"):
            continue
        try:
            yield json.loads(line)
        except json.JSONDecodeError:
            continue


def module_of(path):
    """The 'module' a file belongs to: its directory plus its basename without extension."""
    d = os.path.dirname(path)
    stem = os.path.basename(path)
    for ext in (".h", ".hpp", ".c", ".cc", ".cpp"):
        if stem.endswith(ext):
            stem = stem[: -len(ext)]
            break
    return d, stem


# Not every cross-directory definition is a mistake, and reporting them as one buries the few
# that are. Three shapes recur and each means something different.
ORCHESTRATOR = "defined in darktable.c: the singleton-accessor pattern"
SUBDIR = "implementation lives in a subdirectory of the header"
PARENTDIR = "header lives in a subdirectory of the implementation"
UNRELATED = "unrelated directories"


def categorise(hdr, src):
    hdir, sdir = os.path.dirname(hdr), os.path.dirname(src)
    if src == "src/darktable.c":
        return ORCHESTRATOR
    if sdir.startswith(hdir + "/"):
        return SUBDIR
    if hdir.startswith(sdir + "/"):
        return PARENTDIR
    return UNRELATED


def main():
    show_all = "--all" in sys.argv
    as_json = "--json" in sys.argv

    decls = collections.defaultdict(list)   # name -> [(path, line)]
    defs = collections.defaultdict(list)

    for tag in run_ctags():
        path, name, kind = tag.get("path", ""), tag.get("name", ""), tag.get("kind", "")
        # Ignore anything nested in a class/struct/namespace: C++ members follow their own
        # placement rules and are not what this convention is about.
        if tag.get("scope"):
            continue
        if kind == "prototype" and path.endswith((".h", ".hpp")):
            decls[name].append((path, tag.get("line", 0)))
        elif kind == "function" and path.endswith((".c", ".cc", ".cpp")):
            # ctags marks file-scoped (static) definitions with "file": true. A static
            # function cannot be what a header declaration resolves to, and skipping them
            # removes a whole class of false positive from generic names -- two unrelated
            # files each with a static swap() looked like a mismatch against
            # common/points.h's declaration of the same name.
            if tag.get("file"):
                continue
            defs[name].append((path, tag.get("line", 0)))

    # A symbol one header declares and many sources define is a plugin interface, not a
    # misplaced definition: every IOP defines tiling_callback() against develop/tiling.h, and
    # every entry point defines main() against win/main_wrapper.h. Reporting those buries the
    # real findings under a hundred lines of by-design.
    INTERFACE_MIN_IMPLS = 3

    findings = []
    for name, places in sorted(decls.items()):
        if name not in defs:
            continue                      # declared but not defined here: another audit
        if len(defs[name]) >= INTERFACE_MIN_IMPLS:
            continue                      # an interface with many implementors
        for hdr, hline in places:
            hdir, hstem = module_of(hdr)
            # Defined in the sibling source? Then it follows the convention; nothing to say.
            if any(module_of(p) == (hdir, hstem) for p, _ in defs[name]):
                continue
            for src, sline in defs[name]:
                sdir, _ = module_of(src)
                cross = sdir != hdir
                if cross or show_all:
                    findings.append({
                        "symbol": name,
                        "declared_in": f"{hdr}:{hline}",
                        "defined_in": f"{src}:{sline}",
                        "cross_module": cross,
                        "category": categorise(hdr, src),
                    })

    if as_json:
        print(json.dumps(findings, indent=2))
        return 0

    cross = [f for f in findings if f["cross_module"]]
    same = [f for f in findings if not f["cross_module"]]

    by_cat = collections.Counter(f["category"] for f in cross)
    print(f"{len(cross)} cross-directory mismatch(es)"
          + (f", {len(same)} same-directory" if show_all else "")
          + "\n")
    for cat, n in by_cat.most_common():
        print(f"  {n:4d}  {cat}")
    print()

    # Only the last category is listed symbol by symbol. The other three are structural: they
    # describe how a module is laid out, not a definition anyone will fail to find. They are
    # summarised by header/source pair so a genuinely odd one still stands out.
    for cat in (SUBDIR, PARENTDIR, ORCHESTRATOR):
        rows = [f for f in cross if f["category"] == cat]
        if not rows:
            continue
        pairs = collections.Counter(
            (f["declared_in"].split(":")[0], f["defined_in"].split(":")[0]) for f in rows
        )
        print(f"--- {cat} ({len(rows)}) ---")
        for (h, s), n in pairs.most_common():
            print(f"  {n:3d}  {h}  ->  {s}")
        print()

    rows = [f for f in cross if f["category"] == UNRELATED]
    print(f"--- {UNRELATED} ({len(rows)}) : the ones worth looking at ---")
    for f in sorted(rows, key=lambda x: (x["declared_in"], x["symbol"])):
        print(f"  {f['symbol']}")
        print(f"      declared {f['declared_in']}")
        print(f"      defined  {f['defined_in']}")

    if show_all and same:
        print("\n--- same directory, different file (often deliberate) ---")
        for f in sorted(same, key=lambda x: (x["declared_in"], x["symbol"])):
            print(f"  {f['symbol']}: {f['declared_in']} -> {f['defined_in']}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
