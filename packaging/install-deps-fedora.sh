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

DNF_PACKAGES=(
  adwaita-icon-theme
  appstream-util
  at-spi2-core
  atk-devel
  cairo-devel
  clang
  clang-devel
  cmake
  cmark-devel
  colord-devel
  colord-gtk-devel
  cmocka-devel
  cups-devel
  curl
  dbus-glib-devel
  desktop-file-utils
  doxygen
  exiv2-devel
  fuse
  gdb
  gettext
  git
  gmic
  graphviz
  GraphicsMagick-devel
  libgomp
  libgomp-devel
  gstreamer1-plugins-base-tools
  gdk-pixbuf2-devel
  glib2-devel
  gtk3-devel
  intltool
  iso-codes
  json-glib-devel
  lensfun-devel
  libavif-devel
  libcurl-devel
  libde265-devel
  libexif-devel
  libheif-devel
  libicu-devel
  libjpeg-turbo-devel
  libjxl-devel
  liblcms2-devel
  librsvg2-devel
  libsoup-devel
  libomp-devel
  libpng-devel
  libraw-devel
  libsecret-devel
  libtiff-devel
  libwebp-devel
  libxkbcommon-devel
  libxml2
  libxml2-devel
  libxslt
  libxslt-devel
  libX11-devel
  libxcb-devel
  libxshmfence-devel
  lua-devel
  make
  ninja-build
  ocl-icd-devel
  opencl-headers
  openexr-devel
  openjpeg2-devel
  osm-gps-map-devel
  pango-devel
  perl
  pixman-devel
  pkgconf-pkg-config
  po4a
  pugixml-devel
  python3
  python3-jsonschema
  python3-pip
  saxon
  SDL2-devel
  squashfs-tools
  sqlite-devel
  x265-devel
  zlib-devel
)

"${SUDO[@]}" dnf install -y "${DNF_PACKAGES[@]}"
