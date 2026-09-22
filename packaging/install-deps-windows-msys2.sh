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
  perl
  perl-XML-Parser
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
  expat
  flickcurl
  gcc-libs
  gettext
  gdb
  gtk3
  icu
  imath
  iso-codes
  lcms2
  lensfun
  lld
  llvm
  llvm-openmp
  libavif
  libexif
  libheif
  libinih
  libjpeg-turbo
  libjxl
  librsvg
  libtiff
  libwebp
  libxml2
  libxslt
  ninja
  nsis
  openexr
  openjpeg2
  osm-gps-map
  pugixml
  python
  python-jsonschema
  python-setuptools
  python-six
  sqlite3
  zlib
)

pacman -Suy --noconfirm

# libsoup package name differs across MSYS2 repositories.
LIBSOUP_PKG=""
for cand in libsoup3 libsoup libsoup2; do
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

# cairo is held one release back. 1.18.6 crashes Ansel while it builds its menus, inside
# the DirectWrite font path: starting with PANGOCAIRO_BACKEND=fontconfig, which bypasses
# that path entirely, is enough to avoid it, which is what places the fault there. That
# release is where DirectWrite gained COLRv1 colour-font support and had its thread safety
# reworked, and where MSYS2 rebased its own D2D glyph-bitmap patch onto the file those two
# changed. Ansel reaches it early: the colour-label entries are a `⬤` glyph, which Windows
# resolves through Segoe UI Emoji -- a font carrying COLRv1, COLRv0 and monochrome glyphs
# at once, so the same installed file is read through the new code from now on.
#
# 1.18.4-4 is the last build predating all three, and every dependency on cairo in the
# repository is unversioned, so installing it over the resolved one breaks nothing. This
# runs last because `pacman -S` above would otherwise resolve the pin away again.
#
# Drop this block once a repaired cairo reaches the repository.
CAIRO_PINNED_VERSION="1.18.4-4"
MSYS2_REPO="$(echo "${MSYSTEM:-UCRT64}" | tr '[:upper:]' '[:lower:]')"
pacman -U --noconfirm \
  "https://repo.msys2.org/mingw/${MSYS2_REPO}/${MINGW_PREFIX}-cairo-${CAIRO_PINNED_VERSION}-any.pkg.tar.zst"
