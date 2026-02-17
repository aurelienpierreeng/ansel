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

APT_PACKAGES=(
  adwaita-icon-theme
  appstream-util
  at-spi2-core
  build-essential
  cmake
  curl
  debianutils
  desktop-file-utils
  doxygen
  gdb
  gettext
  git
  graphviz
  gstreamer1.0-tools
  intltool
  iso-codes
  libatk1.0-dev
  libavif-dev
  libavifile-0.7-dev
  libcairo2-dev
  libcolord-dev
  libcolord-gtk-dev
  libcmocka-dev
  libcmark-dev
  libcups2-dev
  libcurl4-gnutls-dev
  libdbus-glib-1-dev
  libde265-dev
  libexiv2-dev
  libexif-dev
  libfuse2
  libgdk-pixbuf2.0-dev
  libglib2.0-dev
  libgmic-dev
  libgraphicsmagick1-dev
  libgomp1
  libgtk-3-dev
  libheif-dev
  libicu-dev
  libimage-exiftool-perl
  libinih-dev
  libjpeg-dev
  libjxl-dev
  libjson-glib-dev
  liblcms2-dev
  liblensfun-bin
  liblensfun-data-v1
  liblensfun-dev
  liblensfun1
  liblua5.2-dev
  liblua5.3-dev
  libopenexr-dev
  libopenjp2-7-dev
  libosmgpsmap-1.0-dev
  libpango1.0-dev
  libpixman-1-dev
  libpng-dev
  libpugixml-dev
  libraw-dev
  librsvg2-dev
  libsaxon-java
  libsecret-1-dev
  libsdl2-dev
  libsoup2.4-dev
  libsqlite3-dev
  libtiff5-dev
  libwebp-dev
  libx11-dev
  libx265-dev
  libxcb1-dev
  libxkbcommon-dev
  libxml2-dev
  libxml2-utils
  libxshmfence-dev
  libxslt1-dev
  ninja-build
  ocl-icd-opencl-dev
  opencl-headers
  perl
  pkg-config
  po4a
  python3
  python3-jsonschema
  python3-pip
  squashfs-tools
  xsltproc
  zlib1g-dev
)

remove_pkg() {
  local remove="$1"
  local new=()
  for pkg in "${APT_PACKAGES[@]}"; do
    if [ "${pkg}" != "${remove}" ]; then
      new+=("${pkg}")
    fi
  done
  APT_PACKAGES=("${new[@]}")
}

if [ -n "${LLVM_VER:-}" ]; then
  APT_PACKAGES+=(
    "clang-${LLVM_VER}"
    "libc++-${LLVM_VER}-dev"
    "libclang-common-${LLVM_VER}-dev"
    "libomp-${LLVM_VER}-dev"
    "llvm-${LLVM_VER}-dev"
  )
else
  # Fallback to the distro default OpenMP runtime when LLVM_VER is not pinned.
  APT_PACKAGES+=(
    "clang"
    "libclang-dev"
    "libomp-dev"
    "llvm"
  )
fi

if [ -n "${GCC_VER:-}" ]; then
  APT_PACKAGES+=(
    "gcc-${GCC_VER}"
    "g++-${GCC_VER}"
    "libomp-dev"
    # LLVM/CLang is still required for OpenCL test-build
    "llvm"
    "clang"
  )
fi

"${SUDO[@]}" apt-get update

# If libjxl-dev is not available in the current repositories, skip it so the
# rest of the dependencies can still be installed.
if ! apt-cache show libjxl-dev >/dev/null 2>&1; then
  remove_pkg libjxl-dev
fi

"${SUDO[@]}" apt-get install -y "${APT_PACKAGES[@]}"
