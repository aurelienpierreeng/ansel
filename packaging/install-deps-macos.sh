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
  pkgconf            # `pkg-config' is the old name of this formula
  cmocka
  curl
  desktop-file-utils
  expat
  gettext
  git
  glib
  gtk-mac-integration
  gtk+3
  icu4c@78           # `icu4c' is an alias for the current versioned formula
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
  sdl2-compat        # `sdl2' is an alias for this formula
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

# Ask brew which of these another one already pulls in, and request only the rest. A single
# `brew install' with a formula named both on its own and inside another's dependency set builds
# it TWICE: measured on the Intel nightlies of 2026-09-10 and 09-14, librsvg (9-10 min) and
# gtk+3 (3 min) each finished two installations of the same version, the second ending on
# "This keg was marked linked already, continuing anyway". brew plans both before building
# anything -- its plan names librsvg as a top-level formula AND as adwaita-icon-theme's
# dependency -- and never reconciles the two. Ten of the entries above are redundant that way;
# only the ones the runner image did not already carry actually cost a rebuild.
#
# The list above stays the declaration of what Ansel needs. This only trims what is redundant at
# install time, from brew's own graph rather than from an assumption about it, which is what
# makes it self-correcting: the day adwaita-icon-theme stops depending on librsvg, `brew deps'
# stops naming it and it is requested again. Nothing trimmed here can be lost to `brew
# autoremove' either, since by construction it is a dependency of something still requested.
# And the verification below still checks every DECLARED package, however it arrived.
#
# Runtime dependencies only, no --include-build: a build-only dependency is not guaranteed to
# stay, so trimming on that basis would be trading a rebuild for an absence. The comparison is
# textual against brew's output, which is why the entries above must be canonical names -- an
# alias would never match what `brew deps' prints.
install_list=("${HB_PACKAGES[@]}")
if implied="$(brew deps --union "${HB_PACKAGES[@]}" 2>/dev/null)"; then
  kept=()
  trimmed=()
  for package in "${HB_PACKAGES[@]}"; do
    if grep -qxF -- "${package}" <<< "${implied}"; then
      trimmed+=("${package}")
    else
      kept+=("${package}")
    fi
  done
  if (( ${#trimmed[@]} > 0 )); then
    printf 'Already pulled in by another requested formula, not requested again: %s\n' "${trimmed[*]}"
    install_list=("${kept[@]}")
  fi
else
  echo "brew deps failed; requesting every formula as listed." >&2
fi

brew_install_status=0
if brew install "${install_list[@]}"; then
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
  # Never exit 0 here. `${brew_install_status:-1}' reads as a fallback to 1 and is not one:
  # the variable is always SET, and it is 0 on the ordinary path where brew reported success
  # and a package is nonetheless absent -- which is exactly the case this branch exists for.
  # So the missing packages were printed and the script passed. This is also what makes the
  # dedupe above safe: a formula trimmed from the request because another was expected to pull
  # it in has nothing but this check standing behind it.
  exit $(( brew_install_status == 0 ? 1 : brew_install_status ))
fi

if (( brew_install_status != 0 )); then
  echo "brew install reported a post-install failure, but all requested packages are present." >&2
fi

# Handle keg-only libs.
brew link --force libomp libsoup@2
