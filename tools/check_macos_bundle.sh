#!/usr/bin/env bash
#
# Verify that the macOS .app carries every executable the build installed, and that each one
# can actually find its libraries inside the bundle.
#
# Both halves earned their place. packaging/macosx/3_make_hb_ansel_package.sh used to name the
# binaries it copied, so `ansel-lens-db-update' and `ansel-nn-parity' -- built and installed
# like every other command -- were simply absent from /Applications/Ansel.app, while CPack on
# Windows and `make install' on Linux, neither of which enumerates anything, carried them. The
# script held a SECOND list for the dependency install and the load-path rewriting, so copying
# a binary without adding it there ships one that cannot start: `ansel-lens-db-update' is the
# only binary linking liblensfun, and that list is what pulls the dylib into the bundle. A
# check on the roster alone would have passed a half-fix.
#
# Static on purpose. Running each bundled command would be the obvious test and is the wrong
# one: `ansel-lens-db-update' rebuilds a database, `ansel-generate-cache' writes thumbnails,
# and `ansel --version' needs no library resolution the loader would report. Reading the load
# commands proves the closure instead -- every dependency either lives in the bundle or is a
# system library -- with no side effect and no display.
#
# macOS only -- it reads Mach-O load commands. Run it after 3_make_hb_ansel_package.sh and
# before the DMG, which is where the nightly puts it.
#
# Usage:
#   tools/check_macos_bundle.sh [INSTALL_DIR]     # default: <repo>/install
#
# Exit codes, the same three check_it_runs.sh uses:
#   0  the bundle is complete and every load command resolves
#   1  a defect in the bundle -- this is the gate failing
#   2  nothing was checked: another platform, no otool, or no bundle to look at. NOT a pass.
#      Anywhere but the macOS nightly, a caller running every gate must expect 2 here.

set -uo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

INSTALL_DIR="${1:-${INSTALL_PREFIX:-${REPO_ROOT}/install}}"
BIN_DIR="${INSTALL_DIR}/bin"
APP_DIR="${INSTALL_DIR}/package/Ansel.app"
EXEC_DIR="${APP_DIR}/Contents/MacOS"
LIB_DIR="${APP_DIR}/Contents/Resources/lib"

if [ "$(uname -s)" != "Darwin" ]; then
  echo "note: $(uname -s) is not macOS -- there is no .app here to check. Nothing was verified."
  exit 2
fi

if ! command -v otool >/dev/null 2>&1; then
  echo "note: otool not found on macOS -- install the Xcode command line tools."
  echo "      Nothing was verified."
  exit 2
fi

for d in "${BIN_DIR}" "${EXEC_DIR}" "${LIB_DIR}"; do
  if [ ! -d "${d}" ]; then
    echo "note: ${d} not found."
    echo "      Build and install, then:  packaging/macosx/3_make_hb_ansel_package.sh"
    exit 2
  fi
done

failures=0
fail() { echo "FAIL: $*"; failures=$((failures + 1)); }

# --- 1. Every installed executable is in the bundle ------------------------------------------
#
# The bundle may legitimately hold more than bin/ does (the libexec tools land there too), so
# this is containment, not equality.
echo "== executables =="
for installed in "${BIN_DIR}"/*; do
  [ -f "${installed}" ] || continue
  name="$(basename "${installed}")"
  if [ -f "${EXEC_DIR}/${name}" ]; then
    echo "  ok      ${name}"
  else
    fail "${name} was installed in bin/ but is not in the .app"
  fi
done

# --- 2. Every load command resolves inside the bundle or to the system -----------------------
#
# What a missing entry in the packaging script's second list looks like: a dependency still
# named by its homebrew path, which exists on the build host and on no user's machine.
echo "== load commands =="
hbPrefixes="/opt/homebrew /usr/local"
hbFromBrew="$(brew --prefix 2>/dev/null)"
[ -n "${hbFromBrew}" ] && hbPrefixes="${hbFromBrew} ${hbPrefixes}"

# Is this path inside any homebrew prefix? Asking brew alone is not enough -- it answers
# /opt/homebrew on arm64 and /usr/local on x86_64, and the wrong guess makes this check pass
# a binary still loading from the build host. Neither prefix is a system library location, so
# suspecting both costs nothing.
is_homebrew_path() {
  local p="$1" prefix
  for prefix in ${hbPrefixes}; do
    case "${p}" in "${prefix}"/*) return 0 ;; esac
  done
  return 1
}

check_load_commands() {
  local file="$1" fileDir deps dep resolved
  fileDir="$(dirname "${file}")"

  # Same selection the packaging script uses: dependency lines carry a compatibility version,
  # the header line naming the file itself does not.
  deps="$(otool -L "${file}" 2>/dev/null | grep compatibility | cut -d\( -f1 \
          | sed 's/^[[:blank:]]*//;s/[[:blank:]]*$//' | sort -u)"

  while IFS= read -r dep; do
    [ -n "${dep}" ] || continue
    case "${dep}" in
      /usr/lib/*|/System/*)
        # Shipped with macOS; nothing to bundle.
        ;;
      @executable_path/*)
        resolved="${EXEC_DIR}/${dep#@executable_path/}"
        [ -e "${resolved}" ] || fail "$(basename "${file}") loads ${dep}, which is not in the bundle"
        ;;
      @loader_path/*)
        resolved="${fileDir}/${dep#@loader_path/}"
        [ -e "${resolved}" ] || fail "$(basename "${file}") loads ${dep}, which is not in the bundle"
        ;;
      @rpath/*)
        # reset_exec_path() points every rpath at Resources/lib.
        resolved="${LIB_DIR}/${dep#@rpath/}"
        [ -e "${resolved}" ] || fail "$(basename "${file}") loads ${dep}, absent from Resources/lib"
        ;;
      /*)
        if is_homebrew_path "${dep}"; then
          fail "$(basename "${file}") still loads ${dep} -- not rewritten into the bundle"
        elif [ ! -e "${dep}" ]; then
          fail "$(basename "${file}") loads ${dep}, which does not exist"
        fi
        ;;
    esac
  done <<< "${deps}"
}

checked=0
for f in "${EXEC_DIR}"/*; do
  [ -f "${f}" ] || continue
  check_load_commands "${f}"
  checked=$((checked + 1))
done
while IFS= read -r f; do
  [ -n "${f}" ] || continue
  check_load_commands "${f}"
  checked=$((checked + 1))
done <<< "$(find "${LIB_DIR}" -type f \( -name '*.dylib' -o -name '*.so' \) 2>/dev/null)"
echo "  checked ${checked} binaries"

if [ "${failures}" -ne 0 ]; then
  echo
  echo "${failures} problem(s) in ${APP_DIR}."
  echo "Neither list in 3_make_hb_ansel_package.sh may name a binary: it copies bin/ whole and"
  echo "reads the roster back from the bundle, so both stay right as build options change."
  exit 1
fi

echo
echo "macOS bundle: every installed executable present, every load command resolved."
