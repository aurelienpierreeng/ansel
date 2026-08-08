#!/usr/bin/env bash
#
# Report #includes that the file does not use, via clang-tidy's misc-include-cleaner.
#
# Scope is deliberately the files a change touches, not the whole tree. A measured ~0.78
# unused includes per translation unit means the tree carries several hundred of them; a
# blocking whole-tree gate would demand that cleanup up front and would simply be turned off.
# Gating the diff instead means every file anyone touches comes out clean, with no baseline
# file to drift out of date.
#
# Usage:
#   tools/check_unused_includes.sh --changed <base-ref>   # files changed since <base-ref>
#   tools/check_unused_includes.sh <file>...              # explicit files
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

  if ! printf '%s\n' "${tu_list}" | grep -qF "/${f}"; then
    echo "note: $f is not a translation unit in this build; not checked"
    skipped_not_tu=$((skipped_not_tu + 1))
    continue
  fi

  out="$("${CLANG_TIDY}" -p "${BUILD_DIR}" --quiet "$f" 2>/dev/null \
         | grep "is not used directly" | grep -F "${f}:")"

  checked=$((checked + 1))
  if [ -n "$out" ]; then
    printf '%s\n' "$out"
    findings=$((findings + $(printf '%s\n' "$out" | wc -l)))
  fi
done

echo
echo "Checked ${checked} file(s); ${findings} unused include(s) found."
if [ "${skipped_not_tu}" -gt 0 ]; then
  echo "${skipped_not_tu} file(s) skipped: not translation units in this build (see above)."
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
