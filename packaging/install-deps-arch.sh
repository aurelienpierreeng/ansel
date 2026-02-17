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

if [ "${EUID:-$(id -u)}" -ne 0 ]; then
  SUDO=(sudo)
else
  SUDO=()
fi

PACMAN_PACKAGES=(
  adwaita-icon-theme
  appstream
  at-spi2-core
  atk
  base-devel
  cairo
  clang
  cmake
  cmark
  colord
  colord-gtk
  cmocka
  cups
  curl
  dbus-glib
  desktop-file-utils
  doxygen
  gcc-libs
  exiv2
  fuse2
  gdb
  gettext
  git
  gmic
  graphviz
  graphicsmagick
  gdk-pixbuf2
  glib2
  gstreamer
  gtk3
  intltool
  iso-codes
  json-glib
  lensfun
  libavif
  libde265
  libexif
  libheif
  libicu
  libjpeg-turbo
  libjxl
  libomp
  libpng
  libraw
  librsvg
  libsecret
  libsoup
  libtiff
  libwebp
  libx11
  libxcb
  libxkbcommon
  libxml2
  libxslt
  libxshmfence
  llvm
  lua
  make
  ninja
  ocl-icd
  opencl-headers
  openexr
  openjpeg2
  osm-gps-map
  pango
  perl
  pixman
  pkgconf
  po4a
  pugixml
  python
  python-jsonschema
  python-pip
  sdl2
  sqlite
  squashfs-tools
  x265
  zlib
)

"${SUDO[@]}" pacman -Syu --needed --noconfirm "${PACMAN_PACKAGES[@]}"
