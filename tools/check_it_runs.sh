#!/usr/bin/env bash
#
# Run the application once, the way CI's "Check if it runs" step does.
#
# Every other check here is static: it compiles, it links, the includes are honest, the layers
# hold. None of that catches a use-after-free, a double free or a bad pointer, because those
# only exist at run time. This one does, and it is cheap -- one export of a small PNG.
#
# It earned its place: a change that made an image own its embedded ICC profile passed four
# build configurations and every gate, then aborted with "corrupted size vs prev_size" on all
# eight CI runners. The container was closing an LCMS2 profile it had borrowed rather than
# created. Nothing static could have seen that.
#
# Uses an isolated --configdir and --library: ansel-cli writes to the real user configuration
# otherwise, and has done (see doc, and the skull-thumbnail incident).
#
# Usage:
#   tools/check_it_runs.sh [BUILD_DIR]      # default: build

set -uo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}" || exit 2

BUILD_DIR="${1:-${BUILD_DIR:-build}}"
CLI="${BUILD_DIR}/stage/opt/ansel/bin/ansel-cli"

if [ ! -x "${CLI}" ]; then
  echo "note: ${CLI} not found."
  echo "      Build, then stage it:  (cd ${BUILD_DIR} && DESTDIR=\$PWD/stage cmake -P cmake_install.cmake)"
  echo "      A stale stage is worse than none -- it will happily test the previous commit."
  exit 2
fi

# Report which commit is actually being run. Testing a stale stage and believing the result is
# an easy and expensive mistake.
echo "running: $(${CLI} --version 2>/dev/null | head -1)"

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

# The same invocation as .github/workflows/ci.yml's "Check if it runs".
"${CLI}" \
  --width 2048 --height 2048 \
  --apply-custom-presets false \
  "${REPO_ROOT}/data/pixmaps/256x256/ansel.png" \
  "${WORK}/output.png" \
  --core --disable-opencl \
  --configdir "${WORK}/config" --library "${WORK}/config/library.db" \
  --conf host_memory_limit=8192 --conf worker_threads=4 -t 4 \
  --conf plugins/lighttable/export/force_lcms2=FALSE \
  --conf plugins/lighttable/export/iccintent=0 \
  > "${WORK}/log" 2>&1
status=$?

if [ ${status} -ne 0 ]; then
  echo
  tail -25 "${WORK}/log"
  for f in /tmp/ansel_bt_*; do
    [ -f "$f" ] && { echo "===== $f ====="; cat "$f"; }
  done
  echo
  echo "FAILED (exit ${status}). 134 is SIGABRT -- usually glibc catching heap corruption at"
  echo "shutdown, i.e. a double free or an overrun, not a fault where the message points."
  exit 1
fi

[ -s "${WORK}/output.png" ] || { echo "FAILED: exited 0 but wrote no output."; exit 1; }
echo "OK: exported $(stat -c%s "${WORK}/output.png") bytes and exited cleanly."

# Same again, from a path that is not ASCII.
#
# On Windows a path is not just bytes: the narrow CRT reads it in the process ANSI code page,
# so anything above 0x7F has to reach each library through its wide API. Exiv2 lost that API
# in 0.28 and Windows users lost all EXIF below any folder with an accent in its name -- twice,
# in 2025 and again in 2026, both times found months later by a user with a screenshot
# (issue #474, and doc/exiv2.md). On Linux and macOS this always passes; it is here so that
# anyone running this under MSYS2 gets the answer, and so the invariant is written down.
PROBE="${WORK}/Épreuve — тест"
mkdir -p "${PROBE}"
cp "${REPO_ROOT}/data/pixmaps/256x256/ansel.png" "${PROBE}/ansel.png"

"${CLI}" \
  --width 512 --height 512 \
  --apply-custom-presets false \
  "${PROBE}/ansel.png" \
  "${PROBE}/output.png" \
  --core --disable-opencl \
  --configdir "${WORK}/config2" --library "${WORK}/config2/library.db" \
  --conf host_memory_limit=8192 --conf worker_threads=4 -t 4 \
  > "${WORK}/log-unicode" 2>&1
status=$?

if grep -q "Failed to open the data source" "${WORK}/log-unicode"; then
  echo
  grep -m3 "Failed to open the data source" "${WORK}/log-unicode"
  echo "FAILED: exiv2 could not open a non-ASCII path. See doc/exiv2.md."
  exit 1
fi

if [ ${status} -ne 0 ] || [ ! -s "${PROBE}/output.png" ]; then
  echo
  tail -25 "${WORK}/log-unicode"
  echo "FAILED: export from a non-ASCII path (exit ${status})."
  exit 1
fi
echo "OK: exported from a non-ASCII path too."
