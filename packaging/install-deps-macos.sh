#!/usr/bin/env bash
#   This file is part of the Ansel project.
#   Copyright (C) 2026 Aurélien PIERRE.
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
  libsoup@2
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
# Homebrew publishes no bottles any more: about four hours, of a CI job's six. All it buys is
# test-compilation of the OpenCL kernels at build time -- CMakeLists.txt's
# TESTBUILD_OPENCL_PROGRAMS, which turns itself off with a warning when LLVM is absent -- and
# the arm64 CI does that on every commit. So Intel does without it, and mac-nightly.yml passes
# -DTESTBUILD_OPENCL_PROGRAMS=OFF there to say so rather than lean on the fallback.
#
# This does NOT remove the other four-hour llvm build. librsvg and adwaita-icon-theme pull in
# llvm@22, a different formula that no list here mentions, and it was scheduled alongside this
# one on every measured nightly -- which is why the keg cache is the load-bearing fix and this
# is only the margin on top of it. See doc/nightly-distribution.md.
if [ "$(uname -s)" = "Darwin" ] && [ "$(uname -m)" = "x86_64" ]; then
  echo "Intel macOS: skipping llvm, a ~4 h source build here. It only enables OpenCL kernel"
  echo "test-compilation, which the arm64 CI performs on every commit."
  _kept=()
  for _pkg in "${HB_PACKAGES[@]}"; do
    [ "${_pkg}" = "llvm" ] || _kept+=("${_pkg}")
  done
  HB_PACKAGES=("${_kept[@]}")
  unset _kept _pkg
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
  exit "${brew_install_status:-1}"
fi

if (( brew_install_status != 0 )); then
  echo "brew install reported a post-install failure, but all requested packages are present." >&2
fi

# Handle keg-only libs.
brew link --force libomp libsoup@2
