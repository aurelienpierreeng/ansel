#!/usr/bin/env bash
#
# An #include added inside a platform #ifdef silently does nothing everywhere else.
#
# It compiles on whichever machine happens to define that macro, and fails on every other
# platform with "call to undeclared function" pointing at a use hundreds of lines away. Nothing
# else here catches it: check_unused_includes.sh asks whether an include is needed, not whether
# it is reachable, and the compiler only complains on the platform you are not building.
#
# It has now happened twice in this series. The second time,
# `#include "system/surface_scaling.h"` landed inside `#ifdef GDK_WINDOWING_WAYLAND` in
# widgets/bauhaus.c -- fine on a Wayland desktop, broken on macOS and Windows, and it took a
# full CI matrix to find out. tools/fix_missing_includes.py already refuses to insert there;
# this catches the case where someone (or some ad-hoc script) does it by hand.
#
# Only includes the diff ADDS are checked, and only where the header was not already included
# somewhere in the same file -- moving or renaming an include that was always inside a
# conditional is not a new problem.
#
# Usage:
#   tools/check_conditional_includes.sh <base-ref>

set -uo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}" || exit 2
BASE="${1:?usage: check_conditional_includes.sh <base-ref>}"

# This compares <base-ref>..HEAD, so uncommitted work is invisible to it. Running it on a dirty
# tree and reading "OK" as approval is a trap I fell into: the check passed locally, then failed
# on every Linux runner as soon as the same change was committed. Say so rather than answer a
# question that was not asked.
if [ -n "$(git status --porcelain -- 'src/*.c' 'src/*.cc' 'src/*.h' 2>/dev/null)" ]; then
  echo "warning: uncommitted changes under src/ are NOT checked -- this compares ${BASE}..HEAD."
  echo "         Commit first, then re-run, or the result means nothing for what you just wrote."
  echo
fi

"${PYTHON:-python3}" - "${BASE}" <<'PYEOF'
import os, re, subprocess, sys

base = sys.argv[1]
# Two dots: CI checks out shallow, so there may be no merge base to compute from.
diff = subprocess.run(["git", "diff", "-U0", base, "HEAD", "--", "src/"],
                      capture_output=True, text=True).stdout

added, removed, path = {}, {}, None
for line in diff.split("\n"):
    if line.startswith("+++ b/"):
        path = line[6:]
    elif line.startswith(("+#include", "-#include")) and path:
        m = re.search(r'["<]([^">]+)[">]', line)
        if not m:
            continue
        bucket = added if line.startswith("+") else removed
        bucket.setdefault(path, set()).add(m.group(1))

def depth_at(text, offset, is_header):
    d = 0
    for m in re.finditer(r'^\s*#\s*(if|ifdef|ifndef|endif)\b', text[:offset], re.M):
        d += -1 if m.group(1) == "endif" else 1
    # A header's own include guard wraps the whole file and is not a conditional in this sense.
    return d - 1 if is_header and d > 0 else d

findings = []
for p, incs in added.items():
    if not os.path.isfile(p):
        continue
    with open(p, errors="ignore") as fh:
        text = fh.read()
    is_header = p.endswith((".h", ".hpp"))
    # Renamed or moved in place: the header was already included here under another path, so a
    # conditional position is not something this change introduced.
    gone = {os.path.basename(x) for x in removed.get(p, ())}

    # Look the include up in the CURRENT file rather than matching the diff text: the line may
    # have been edited since (to add the marker below, for one).
    for m in re.finditer(r'^[ \t]*#[ \t]*include[ \t]*["<]([^">]+)[">].*$', text, re.M):
        header = m.group(1)
        if header not in incs or os.path.basename(header) in gone:
            continue
        # An include that is genuinely conditional -- an OS header used only inside the same
        # #ifdef -- is marked, so the exception is visible in the source rather than in a list
        # somewhere else.
        if "conditional-ok:" in m.group(0):
            continue
        if depth_at(text, m.start(), is_header) > 0:
            findings.append(f"{p}: {m.group(0).strip()}")

if not findings:
    print("OK: no include added inside a conditional block.")
    sys.exit(0)

print("Includes added inside a conditional block:\n")
for f in findings:
    print(f"  {f}")
print("""
Each of these is compiled only where that #ifdef is true. If the symbol it supplies is used
outside the same conditional, every other platform fails to build -- with an error pointing at
the use, not at the include.

Move it into the file's unconditional include block. If it genuinely belongs inside the
conditional (an OS-specific header used only there), the surrounding code must be inside the
same conditional too -- append `// conditional-ok: <reason>` to the include line, so the
exception is visible where it applies rather than in a list somewhere else.
""")
sys.exit(1)
PYEOF
