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

# pacman fetches a package's .sig from the mirror that served the package, with no other server
# to fall back on (lib/libalpm/dload.c, pacman 6.1): one stalled mirror fails the whole
# transaction. mirror.msys2.org picks a mirror per request, so asking again lands elsewhere, and
# what already arrived stays in the cache; a package cached without its .sig is fetched again.
pacman_retry() {
  local attempt=1
  until pacman "$@"; do
    if [ "${attempt}" -ge 3 ]; then
      echo "pacman failed ${attempt} times, giving up." >&2
      return 1
    fi
    echo "pacman failed (attempt ${attempt} of 3), retrying in 10 s." >&2
    attempt=$((attempt + 1))
    sleep 10
  done
}

pacman_retry -Suy --noconfirm

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
pacman_retry -S --needed --noconfirm "${MSYS_PACKAGES[@]}"

MINGW_FULL_PACKAGES=()
for pkg in "${MINGW_PACKAGES[@]}"; do
  MINGW_FULL_PACKAGES+=("${MINGW_PREFIX}-${pkg}")
done

pacman_retry -S --needed --noconfirm "${MINGW_FULL_PACKAGES[@]}"
