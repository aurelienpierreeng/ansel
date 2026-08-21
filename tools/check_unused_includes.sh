#!/usr/bin/env bash
#
# Report #includes that the file does not use, via clang-tidy's misc-include-cleaner.
#
# Scope is the include lines a change ADDS, not every include in a file it touches, and not
# the whole tree. A measured ~0.78 unused includes per translation unit means the tree carries
# several hundred of them; a blocking whole-tree gate would demand that cleanup up front and
# would simply be turned off.
#
# "Every file you touch comes out clean" was the original rule, and it is the right one for a
# normal change. It stops being the right one when a refactor rewrites a single include line
# in 180 files: the gate then reports that file's whole inherited backlog as if the change
# introduced it. Measured on the gtk.h removal: 95 findings, of which 12 were on lines the
# branch actually added. Judging added lines keeps the gate prescriptive -- you may not
# introduce an unused include -- without making unrelated cleanup the price of a path rewrite.
#
# Findings on lines the change did not touch are summarised by file rather than printed one per
# line: a hundred notes scrolling past on every run is not visibility, it is noise that trains
# people to skip the whole step. Set UNUSED_INCLUDES_VERBOSE=1 to see each one.
#
# Usage:
#   tools/check_unused_includes.sh --changed <base-ref>   # lines added since <base-ref>
#   tools/check_unused_includes.sh <file>...              # explicit files, every include
#
# Requires a configured build directory with compile_commands.json (BUILD_DIR, default ./build).

set -uo pipefail

BUILD_DIR="${BUILD_DIR:-build}"
CLANG_TIDY="${CLANG_TIDY:-clang-tidy}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}" || exit 2

if [ ! -f "${BUILD_DIR}/compile_commands.json" ]; then
  echo "error: no ${BUILD_DIR}/compile_commands.json -- configure the build first" >&2
  exit 2
fi
if ! command -v "${CLANG_TIDY}" >/dev/null 2>&1; then
  echo "error: ${CLANG_TIDY} not found" >&2
  exit 2
fi

files=()
if [ "${1:-}" = "--changed" ]; then
  base="${2:?--changed needs a base ref}"
  while IFS= read -r f; do
    [ -n "$f" ] && files+=("$f")
  # Two dots, not three. CI checks out with fetch-depth 1, so there is no common ancestor to
  # compute a merge base from and "${base}...HEAD" would fail outright. Comparing the two tips
  # directly can pull in a file that changed on the base branch rather than here, which errs
  # toward checking one file too many -- the safe direction for a gate.
  done < <(git diff --name-only --diff-filter=d "${base}" HEAD -- 'src/*.c' 'src/*.cc' \
           | grep -v '^src/external/')
else
  files=("$@")
fi

# Line numbers this change added, per file, in new-file coordinates -- the same coordinates
# clang-tidy reports. Empty when files were named explicitly, which means "check them all".
# Headers the change RENAMED rather than introduced, as "<file>:<basename>". A path rewrite
# (common/macros.h -> system/macros.h) shows up as an added line, and gating it would demand
# the file justify an include it has always carried -- which is how a mechanical rename starts
# dragging unrelated cleanup along with it. Measured: one finding on the branch that prompted
# this, common/cups_print.c, whose include was correct all along (it uses IS_NULL_PTR;
# clang-tidy attributes the macro to whichever header happens to reach it first).
renamed_headers=""
added_lines=""
if [ "${1:-}" = "--changed" ]; then
  added_lines="$(git diff -U0 --diff-filter=d "${2}" HEAD -- 'src/*.c' 'src/*.cc' \
                 | ${PYTHON:-python3} -c '
import re, sys
path = None
for line in sys.stdin:
    if line.startswith("+++ b/"):
        path = line[6:].strip()
    elif line.startswith("@@") and path:
        m = re.match(r"@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@", line)
        if m:
            start = int(m.group(1))
            count = int(m.group(2)) if m.group(2) is not None else 1
            for n in range(start, start + count):
                print(f"{path}:{n}")
')"
  renamed_headers="$(git diff -U0 --diff-filter=d "${2}" HEAD -- 'src/*.c' 'src/*.cc' \
                     | ${PYTHON:-python3} -c '
import os, re, sys
path, added, removed = None, {}, {}
for line in sys.stdin:
    if line.startswith("+++ b/"):
        path = line[6:].strip()
    elif line.startswith(("+#include", "-#include")) and path:
        m = re.search(r"[\"<]([^\">]+)[\">]", line)
        if not m:
            continue
        (added if line.startswith("+") else removed).setdefault(path, set()).add(
            os.path.basename(m.group(1)))
for p, names in added.items():
    for n in sorted(names & removed.get(p, set())):
        print(f"{p}:{n}")
')"
fi

if [ ${#files[@]} -eq 0 ]; then
  echo "No source files changed; nothing to check."
  exit 0
fi

# Files that are not translation units in the compilation database cannot be checked here,
# and must not be reported as clean. Three kinds show up:
#   - platform sources (src/win/*.c) that this build never compiles;
#   - IOP modules, whose .c is #included by a generated introspection_*.c wrapper rather than
#     compiled directly;
#   - anything excluded by the current cmake options.
# Running clang-tidy on them anyway makes it guess a compile command, so the preprocessor
# takes whichever branch that guess implies -- on a Linux database, the wrong one for
# Windows sources. That is the same class of mistake that broke this tree's macOS and
# Windows builds. Skip them, and say so.
tu_list="$(${PYTHON:-python3} - "${BUILD_DIR}/compile_commands.json" <<'PYEOF'
import json, sys
print("\n".join(sorted({e["file"] for e in json.load(open(sys.argv[1]))})))
PYEOF
)"

findings=0
checked=0
skipped_not_tu=0
pre_existing=0
pre_existing_files=""

for f in "${files[@]}"; do
  [ -f "$f" ] || continue

  # HEADERS ARE NOT COVERED, and there is no way to cover them with this check.
  # include-cleaner analyses the symbols referenced by the *main file* of a translation unit.
  # A header is not one. --header-filter does not help: it selects which files' diagnostics
  # are printed, not which files are analysed -- measured, it reports the .c's unused includes
  # and says nothing about the .h. Compiling the header as a synthetic TU is worse than
  # useless: that TU references nothing, so every one of the header's includes comes out
  # "unused".
  #
  # This matters, because the case that motivated adding this check -- common/metadata.h
  # including gui/gtk.h and using no GTK symbol -- is exactly the case it cannot see.
  # tools/include_graph.py is what catches that class; see check_layering.sh next to this file.
  case "$f" in
    *.h) continue ;;
  esac

  # No pipe here, deliberately. Piping into `grep -q` makes grep exit on the first match and
  # SIGPIPE the writer; under `set -o pipefail` that failing writer becomes the pipeline's
  # status, so a file that IS in the database reads as absent and gets silently skipped.
  # Measured in CI: 42 obvious translation units -- darktable.c, dev_history.c, most of iop/ --
  # reported as "not a translation unit" while printf logged a broken pipe for each.
  if [[ "${tu_list}" != *"/${f}"* ]]; then
    if [ -n "${UNUSED_INCLUDES_VERBOSE:-}" ]; then
      echo "note: $f is not a translation unit in this build; not checked"
    fi
    skipped_not_tu=$((skipped_not_tu + 1))
    continue
  fi

  out="$("${CLANG_TIDY}" -p "${BUILD_DIR}" --quiet "$f" 2>/dev/null \
         | grep "is not used directly" | grep -F "${f}:")"

  checked=$((checked + 1))
  [ -n "$out" ] || continue

  if [ -z "${added_lines}" ]; then
    # Explicit file list: no diff to attribute against, so every finding counts.
    printf '%s\n' "$out"
    findings=$((findings + $(printf '%s\n' "$out" | wc -l)))
    continue
  fi

  while IFS= read -r finding; do
    # clang-tidy prints an absolute path; the diff speaks in repo-relative ones.
    lineno="$(printf '%s' "$finding" | sed -n "s|.*/${f}:\([0-9]*\):.*|\1|p")"
    header="$(printf '%s' "$finding" | sed -n 's|.*included header \([^ ]*\) is not used.*|\1|p')"
    if [ -n "${header}" ] && printf '%s\n' "${renamed_headers}" | grep -qxF "${f}:${header}"; then
      if [ -n "${UNUSED_INCLUDES_VERBOSE:-}" ]; then
        printf 'note (renamed in place, not introduced here): %s\n' "$finding"
      fi
      pre_existing=$((pre_existing + 1))
    elif [ -n "${lineno}" ] && printf '%s\n' "${added_lines}" | grep -qxF "${f}:${lineno}"; then
      printf '%s\n' "$finding"
      findings=$((findings + 1))
    else
      if [ -n "${UNUSED_INCLUDES_VERBOSE:-}" ]; then
        printf 'note (pre-existing, not introduced here): %s\n' "$finding"
      fi
      pre_existing_files="${pre_existing_files}${f}\n"
      pre_existing=$((pre_existing + 1))
    fi
  done <<< "$out"
done

echo
echo "Checked ${checked} file(s); ${findings} unused include(s) introduced by this change."
if [ "${pre_existing}" -gt 0 ]; then
  echo "${pre_existing} pre-existing unused include(s) in touched files -- not gated, and not"
  echo "this change's to fix. Worst offenders:"
  printf '%b' "${pre_existing_files}" | grep -v '^$' | sort | uniq -c | sort -rn | head -5 \
    | sed 's/^/    /'
  echo "  (UNUSED_INCLUDES_VERBOSE=1 lists every one)"
fi
if [ "${skipped_not_tu}" -gt 0 ]; then
  echo "${skipped_not_tu} file(s) skipped: not translation units in this build -- IOP modules"
  echo "  (compiled through a generated wrapper), platform sources, and anything this cmake"
  echo "  configuration excludes. Not a finding; set UNUSED_INCLUDES_VERBOSE=1 to list them."
fi

# Findings differ between clang-tidy releases -- 19 and 20 disagree about which symbols
# <stdlib.h> owns, for one. CI pins clang-tidy-20; reproduce with CLANG_TIDY=clang-tidy-20 if
# a local run of a different version disagrees with it.
if [ "${findings}" -gt 0 ]; then
  cat <<'MSG'

Remove them, or -- if the include is there for a symbol clang-tidy cannot attribute to it --
replace it with the header that actually declares what this file uses. Do not silence it by
adding a use.
MSG
  exit 1
fi
exit 0
