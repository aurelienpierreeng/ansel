#!/usr/bin/env bash
# Created: 2026-02-16
set -euo pipefail

if [ "${EUID:-$(id -u)}" -ne 0 ]; then
  SUDO="sudo"
else
  SUDO=""
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
    "libgomp-${GCC_VER}-dev"
  )
fi


"${SUDO}" apt-get update
"${SUDO}" apt-get install -y "${APT_PACKAGES[@]}"
