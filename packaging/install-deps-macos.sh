#!/usr/bin/env bash
#   This file is part of the Ansel project.
#   Copyright (C) 2026 Aurélien PIERRE.
#   Copyright (C) 2026 Guillaume Stutin.
#   
#   Ansel is free software: you can redistribute it and/or modify
#   it under the terms of the GNU General Public License as published by
#   the Free Software Foundation, either version 3 of the License, or
#   (at your option) any later version.
#   
#   Ansel is distributed in the hope that it will be useful,
#   but WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#   GNU General Public License for more details.
#   
#   You should have received a copy of the GNU General Public License
#   along with Ansel.  If not, see <http://www.gnu.org/licenses/>.

# Created: 2026-02-16
set -euo pipefail

if ! command -v brew >/dev/null 2>&1; then
  echo 'Homebrew not found. Install it from https://brew.sh/.' >&2
  exit 1
fi

brew update

HB_PACKAGES=(
  adwaita-icon-theme
  cmake
  cmark
  pkg-config
  cmocka
  curl
  desktop-file-utils
  expat
  gettext
  git
  glib
  gtk-mac-integration
  gtk+3
  icu4c
  intltool
  iso-codes
  jpeg-turbo
  jpeg-xl
  json-glib
  lensfun            # build-time only: the XML->SQLite importer reads through it
  libavif
  libheif
  libomp
  libraw
  librsvg
  libsoup
  little-cms2
  llvm
  ninja
  openexr
  openjpeg
  osm-gps-map
  perl
  po4a
  pugixml
  sdl2
  shared-mime-info
  webp
)

# llvm comes as a bottle on arm64 (seconds) and is built from source on Intel macOS, where
# Homebrew publishes no bottles any more: over four hours, of a CI job's six. All it buys is
# test-compilation of the OpenCL kernels at build time -- CMakeLists.txt's
# TESTBUILD_OPENCL_PROGRAMS, which turns itself off with a warning when LLVM is absent -- and
# the arm64 CI does that on every commit. So Intel does without it, and mac-nightly.yml passes
# -DTESTBUILD_OPENCL_PROGRAMS=OFF there to say so rather than lean on the fallback.
#
# librsvg is the other way llvm got in: it is written in Rust, Homebrew's rust depends on llvm,
# and on Intel both are source builds -- rust 1 h 41, llvm over 4 h, measured 2026-11-17 and 18. So
# on Intel, cargo-c and librsvg are installed first, against the Rust project's prebuilt
# toolchain, by tools/brew_install_without_rust.sh. Once they are installed at the version
# homebrew-core wants, Homebrew prunes their build dependencies from every other formula's plan,
# so nothing below schedules rust or llvm. librsvg then comes off the list below: it is
# installed from that script's tap, and Homebrew refuses an explicit install of the
# homebrew-core formula of the same name. See doc/nightly-distribution.md.
#
# subversion is installed first, on its own. Homebrew fetches every download before building
# anything, and builds with network access denied; netpbm (graphviz -> vala -> libsoup, which
# has no Intel bottle since 3.8.0) is fetched with svn, so a fetch attempted before svn exists
# is deferred into the sandboxed build and fails there on DNS. Nightlies of 2026-10-01 and 02.
if [ "$(uname -s)" = "Darwin" ] && [ "$(uname -m)" = "x86_64" ]; then
  brew install subversion
  "$(dirname "$0")/../tools/brew_install_without_rust.sh" cargo-c librsvg
  echo "Intel macOS: skipping llvm, a 4 h source build here. It only enables OpenCL kernel"
  echo "test-compilation, which the arm64 CI performs on every commit."
  _kept=()
  for _pkg in "${HB_PACKAGES[@]}"; do
    case "${_pkg}" in
      llvm|librsvg) ;;
      *) _kept+=("${_pkg}") ;;
    esac
  done
  HB_PACKAGES=("${_kept[@]}")
  unset _kept _pkg

  # Read before anything is built: rust or llvm in this plan is a build of four hours or more
  # that the job cannot finish, so it stops here, naming them, instead of at the 6-hour ceiling.
  # Matched between whitespace, as in brew_install_without_rust.sh, whatever the plan's layout.
  _plan="$(brew install --dry-run "${HB_PACKAGES[@]}" 2>&1)"
  if grep -qE '(^|[[:space:]])(rust|llvm|llvm@[0-9]+)([[:space:]]|$)' <<< "${_plan}"; then
    echo "${_plan}"
    echo "Intel macOS: Homebrew still plans rust or llvm (above) -- refusing to start." >&2
    exit 1
  fi
  unset _plan
fi

brew_install_status=0
if brew install "${HB_PACKAGES[@]}"; then
  :
else
  brew_install_status=$?
fi

# Homebrew may return a non-zero status when a formula post-install hook fails even if
# the formula itself was installed. We only continue when every requested dependency is
# present, because the build only needs the packages to exist in the Cellar.
missing_packages=()
for package in "${HB_PACKAGES[@]}"; do
  if ! brew list --formula "${package}" >/dev/null 2>&1; then
    missing_packages+=("${package}")
  fi
done

if (( ${#missing_packages[@]} > 0 )); then
  printf 'Missing Homebrew packages after install: %s\n' "${missing_packages[*]}" >&2
  # Not brew's status: it is 0 when brew reported success and a package is missing anyway.
  exit 1
fi

if (( brew_install_status != 0 )); then
  echo "brew install reported a post-install failure, but all requested packages are present." >&2
fi

# Handle keg-only libs.
brew link --force libomp

# libsoup 3 is the only libsoup Homebrew installs: it refuses libsoup@2, disabled upstream, and
# its osm-gps-map requires libsoup-3.0 since 1.2.2. libsoup 2 and 3 share a prefix and only one is
# linked at a time, so a libsoup@2 an image may still carry is unlinked first; it would leave
# osmgpsmap-1.0 unresolvable by pkg-config (see doc/nightly-distribution.md).
brew unlink libsoup@2 >/dev/null 2>&1 || true
brew link --overwrite libsoup
