#!/usr/bin/env bash
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
  libsoup
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
pacman -S --needed --noconfirm "${MSYS_PACKAGES[@]}"

MINGW_FULL_PACKAGES=()
for pkg in "${MINGW_PACKAGES[@]}"; do
  MINGW_FULL_PACKAGES+=("${MINGW_PREFIX}-${pkg}")
done

pacman -S --needed --noconfirm "${MINGW_FULL_PACKAGES[@]}"
