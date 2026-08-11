#!/usr/bin/env python3
"""Catch the doubled g_list_prepend across a repository boundary.

Splitting a cursor loop into "repository builds the list / domain post-processes it" is the
standard move of the src/database migration, and it has a standing trap: BOTH halves prepend.
Two prepends cancel, so the public function silently returns its list in the opposite order to
the single-loop version it replaced -- with byte-identical SQL, a clean build in every
configuration, and green CI. It has bitten twice:

  * dt_tag_get_images() / dt_tag_get_images_from_list(), where the repository dropped the
    reversal the callers used to do (fixed in "database: restore the list order
    dt_tag_get_images() returns");
  * dt_map_location_get_locations_by_path() and _map_location_find_images(), where the
    repository kept reverse-row order AND the caller prepended again.

The rule this enforces: a repository function that builds its result with g_list_prepend and
returns it WITHOUT reversing hands back reverse-row order, which is only correct when its
consumer passes the list straight through. If the consumer prepends again, exactly one of the
two must reverse.

Reported as a list that must stay empty -- not a ratchet. There is no legitimate instance:
where the flip is genuinely wanted, reverse in the repository and say so, so the intent is in
the code rather than in the interaction of two files.
"""

import re
import subprocess
import sys
from pathlib import Path


def _functions(text):
    """(name, body) for every function definition, brace-matched, strings and comments skipped."""
    out = []
    for m in re.finditer(r"\n((?:[\w][\w \*]*?)\b(\w+)\s*\([^;{]*?\)\s*\n?\{)", text):
        try:
            i = text.index("{", m.start(1))
        except ValueError:
            continue
        depth, n, closed = 0, len(text), False
        while i < n:
            c = text[i]
            if c in "\"'":
                quote = c
                i += 1
                while i < n and text[i] != quote:
                    i += 2 if text[i] == "\\" else 1
            elif text.startswith("//", i):
                i = text.index("\n", i)
            elif text.startswith("/*", i):
                i = text.index("*/", i) + 2
            elif c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    closed = True
                    break
            i += 1
        if closed:
            out.append((m.group(2), text[m.end():i]))
    return out


def main(argv):
    src = Path(argv[1] if len(argv) > 1 else "src")

    # 1. repository functions that prepend and never reverse -> they return reverse-row order
    unreversed = {}
    for path in sorted((src / "database").glob("*_repository.c")):
        for name, body in _functions(path.read_text(encoding="utf-8", errors="replace")):
            if "g_list_prepend" in body and "g_list_reverse" not in body:
                unreversed[name] = path

    if not unreversed:
        print("OK: no unreversed prepend-built lists leave src/database.")
        return 0

    # 2. consumers outside the module that prepend the result AGAIN
    listing = subprocess.run(
        ["git", "ls-files", "--", f"{src}/*.c", f"{src}/*.cc"],
        capture_output=True, text=True, cwd=src.parent if src.name == "src" else None,
    )
    files = [Path(p) for p in listing.stdout.split() if p and "/external/" not in p]
    if not files:  # not a git checkout: fall back to a walk
        files = [p for p in src.rglob("*.c") if "external" not in p.parts]
        files += [p for p in src.rglob("*.cc") if "external" not in p.parts]

    findings = []
    for path in files:
        if "database" in path.parts:
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        if not any(name in text for name in unreversed):
            continue
        for caller, body in _functions(text):
            if "g_list_prepend" not in body:
                continue
            for name in unreversed:
                if name + "(" in body:
                    findings.append((name, unreversed[name], path, caller))

    if not findings:
        print(f"OK: {len(unreversed)} unreversed list(s) leave src/database, none double-prepended.")
        return 0

    print("FAILED: a repository list is prepended twice -- its order is flipped vs the "
          "single-loop original.\n")
    for name, defined_in, path, caller in findings:
        print(f"  {name}()  [{defined_in}]")
        print(f"    prepended again by {path}:{caller}()")
    print("\nExactly one of the two must reverse. Reverse in the REPOSITORY (return row order) "
          "\nand say why in a comment beside the return.")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
