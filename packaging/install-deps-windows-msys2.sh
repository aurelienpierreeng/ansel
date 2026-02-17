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

MINGW_PREFIX="${MINGW_PACKAGE_PREFIX:-mingw-w64-x86_64}"

MSYS_PACKAGES=(
  base-devel
  git
  intltool
  po4a
)

MINGW_PACKAGES=(
  toolchain
  clang
  cmake
  cmark
  cmocka
  curl
  dbus-glib
  drmingw
  exiv2
  flickcurl
  gcc-libs
  gettext
  gdb
  gmic
  graphicsmagick
  gtk3
  icu
  imath
  iso-codes
  lcms2
  lensfun
  libavif
  libexif
  libheif
  libinih
  libjpeg-turbo
  libjxl
  librsvg
  libsecret
  libtiff
  libwebp
  libxml2
  libxslt
  lua
  ninja
  nsis
  openexr
  openjpeg2
  openmp
  osm-gps-map
  pugixml
  python
  python-jsonschema
  python-setuptools
  python-six
  sqlite3
  zlib
)

pacman -Sy --noconfirm

# libsoup package name differs across MSYS2 repositories.
LIBSOUP_PKG=""
for cand in libsoup libsoup2; do
  if pacman -Si "${MINGW_PREFIX}-${cand}" >/dev/null 2>&1; then
    LIBSOUP_PKG="${cand}"
    break
  fi
done
if [ -n "${LIBSOUP_PKG}" ]; then
  MINGW_PACKAGES+=("${LIBSOUP_PKG}")
else
  echo "Warning: no libsoup package found for ${MINGW_PREFIX} (tried libsoup, libsoup2)." >&2
fi
pacman -S --needed --noconfirm "${MSYS_PACKAGES[@]}"

MINGW_FULL_PACKAGES=()
for pkg in "${MINGW_PACKAGES[@]}"; do
  MINGW_FULL_PACKAGES+=("${MINGW_PREFIX}-${pkg}")
done

pacman -S --needed --noconfirm "${MINGW_FULL_PACKAGES[@]}"
