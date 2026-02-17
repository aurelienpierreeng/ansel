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
