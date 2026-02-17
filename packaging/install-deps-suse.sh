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

ZYPPER_PACKAGES=(
  Mesa-devel
  OpenEXR-devel
  SDL2-devel
  atk-devel
  cairo-devel
  clang
  cmake
  cmark-devel
  colord-devel
  colord-gtk-devel
  cmocka-devel
  dbus-1-glib-devel
  desktop-file-utils
  doxygen
  fdupes
  fuse
  gcc-c++
  gdb
  gettext-tools
  git
  gstreamer
  gmic
  gnome-keyring-devel
  graphviz
  GraphicsMagick-devel
  gdk-pixbuf-devel
  gtk2-devel
  gtk3-devel
  intltool
  iso-codes
  json-glib-devel
  lensfun-devel
  libavif-devel
  libcurl-devel
  libde265-devel
  libexiv2-devel
  libexif-devel
  libheif-devel
  libicu-devel
  libjpeg-devel
  libjxl-devel
  liblcms2-devel
  libomp-devel
  libpng16-devel
  libraw-devel
  librsvg-devel
  libsecret-devel
  libsoup2-devel
  libtiff-devel
  libwebp-devel
  libX11-devel
  libxcb-devel
  libxkbcommon-devel
  libxml2-devel
  libxslt-devel
  libxshmfence-devel
  llvm-devel
  libgomp-devel
  lua53-devel
  make
  ninja
  ocl-icd-devel
  opencl-headers
  openjpeg2-devel
  osm-gps-map-devel
  pango-devel
  perl
  pixman-devel
  pkg-config
  po4a
  pugixml-devel
  python3
  python3-jsonschema
  python3-pip
  saxon
  sqlite3-devel
  squashfs
  update-desktop-files
  x265-devel
  zlib-devel
)

"${SUDO[@]}" zypper --non-interactive install --no-recommends "${ZYPPER_PACKAGES[@]}"
