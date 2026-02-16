#!/usr/bin/env bash
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
  pkg-config
  cmocka
  curl
  desktop-file-utils
  exiv2
  gettext
  git
  glib
  gmic
  graphicsmagick
  gtk-mac-integration
  gtk+3
  icu4c
  intltool
  iso-codes
  jpeg-turbo
  jpeg-xl
  json-glib
  lensfun
  libavif
  libheif
  libomp
  libraw
  librsvg
  libsecret
  libsoup@2
  little-cms2
  llvm
  lua
  ninja
  openexr
  openjpeg
  osm-gps-map
  perl
  po4a
  pugixml
  sdl2
  webp
)

brew install "${HB_PACKAGES[@]}"

# Handle keg-only libs.
brew link --force libomp libsoup@2
