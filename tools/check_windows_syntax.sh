#!/usr/bin/env bash
#
# Syntax-check changed sources against the WINDOWS preprocessor state, locally, without a
# Windows build.
#
# Everything that ships here is built on Linux, so any code selected by a platform macro --
# _WIN32, __APPLE__, GDK_WINDOWING_QUARTZ, GDK_WINDOWING_WAYLAND -- is invisible until CI runs
# the other platforms. That gap has now cost this project two red matrices in one series: an
# osx/osx.h left behind under quartz, and an #include placed inside #ifdef GDK_WINDOWING_WAYLAND
# that compiled on a Wayland desktop and on nothing else.
#
# A full MinGW build is a different and much larger problem (rawspeed's zlib and pugixml probes
# do not cross-compile out of the box). It is also not what is needed: the bugs are in which
# preprocessor branch gets taken, and `-fsyntax-only` with the MinGW toolchain answers that
# exactly. It needs no linking, no dependency tree beyond headers, and takes about a second per
# file.
#
# Requires the Fedora mingw64 packages (mingw64-gcc, mingw64-gtk3, mingw64-lcms2, ...).
# Skips itself with a note when they are absent, so it is safe to call unconditionally.
#
# Usage:
#   tools/check_windows_syntax.sh --changed <base-ref>
#   tools/check_windows_syntax.sh <file>...

set -uo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}" || exit 2

CC_WIN="${CC_WIN:-x86_64-w64-mingw32-gcc}"
PKGCONFIG_WIN="${PKGCONFIG_WIN:-mingw64-pkg-config}"

if ! command -v "${CC_WIN}" >/dev/null 2>&1 || ! command -v "${PKGCONFIG_WIN}" >/dev/null 2>&1; then
  echo "note: ${CC_WIN} / ${PKGCONFIG_WIN} not found; skipping the Windows syntax check."
  echo "      On Fedora: dnf install mingw64-gcc mingw64-gtk3 mingw64-lcms2 mingw64-sqlite \\"
  echo "                             mingw64-exiv2 mingw64-cairo mingw64-glib2"
  exit 0
fi

# config.h is generated; without a configured build there is nothing to check against.
BUILD_DIR="${BUILD_DIR:-build}"
if [ ! -f "${BUILD_DIR}/src/config.h" ]; then
  echo "note: no ${BUILD_DIR}/src/config.h; configure a build first. Skipping."
  exit 0
fi

files=()
if [ "${1:-}" = "--changed" ]; then
  base="${2:?--changed needs a base ref}"
  while IFS= read -r f; do
    [ -n "$f" ] && files+=("$f")
  done < <(git diff --name-only --diff-filter=d "${base}" HEAD -- 'src/*.c' \
           | grep -v '^src/external/')
else
  files=("$@")
fi

if [ ${#files[@]} -eq 0 ]; then
  echo "No source files to check."
  exit 0
fi

CFLAGS_WIN="$(${PKGCONFIG_WIN} --cflags gtk+-3.0 lcms2 sqlite3 2>/dev/null)"

checked=0
skipped=0
failed=0
for f in "${files[@]}"; do
  [ -f "$f" ] || continue

  out="$("${CC_WIN}" -fsyntax-only -std=c11 ${CFLAGS_WIN} \
         -I src -I "${BUILD_DIR}/src" -DHAVE_CONFIG_H "$f" 2>&1)"
  status=$?

  # A file whose dependencies are not packaged for mingw cannot be judged here, and reporting
  # it as broken would be worse than saying nothing. A missing header is that case; anything
  # else is a real finding.
  if [ $status -ne 0 ] && printf '%s' "$out" | grep -q "fatal error:.*No such file or directory"; then
    missing="$(printf '%s' "$out" | sed -n 's/.*fatal error: \(.*\): No such file.*/\1/p' | head -1)"
    echo "note: $f not checked (no mingw ${missing})"
    skipped=$((skipped + 1))
    continue
  fi

  checked=$((checked + 1))
  if [ $status -ne 0 ]; then
    printf '%s\n' "$out" | grep -E "error:" | head -5
    failed=$((failed + 1))
  fi
done

echo
echo "Windows syntax check: ${checked} file(s) checked, ${skipped} skipped, ${failed} with errors."
if [ "${failed}" -gt 0 ]; then
  cat <<'MSG'

These compile on Linux and not on Windows, which means a platform macro selects different
code. The usual causes, in order of how often they happen here:

  - an #include sitting inside #ifdef GDK_WINDOWING_WAYLAND / _X11, so the declaration exists
    only on this desktop;
  - a symbol used under #ifdef _WIN32 whose header was never included;
  - a function that exists only in the POSIX branch.

tools/check_conditional_includes.sh catches the first class from the diff alone.
MSG
  exit 1
fi
exit 0
