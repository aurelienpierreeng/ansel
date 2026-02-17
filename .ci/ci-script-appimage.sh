#!/bin/bash
#   This file is part of the Ansel project.
#   Copyright (C) 2022-2023, 2025-2026 Aurélien PIERRE.
#   Copyright (C) 2023 Alynx Zhou.
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










#   
# Build Ansel within an AppDir directory
# Then package it as an .AppImage
 #Call this script from the Ansel root folder like `sh .ci/ci-script-appimage.sh`
# Copyright (c) Aurélien Pierre - 2022
#   
# For local builds, purge and clean build pathes if any
#if [ -d "build" ];
#then yes | rm -R build;
#fi;

if [ -d "AppDir" ];
then yes | rm -R AppDir;
fi;

mkdir build
mkdir AppDir
cd build

export CXXFLAGS="-g -O3 -fno-strict-aliasing -ffast-math -fno-finite-math-only"
export CFLAGS="$CXXFLAGS"

## AppImages require us to install everything in /usr, where root is the AppDir
export DESTDIR=../AppDir
cmake .. -DCMAKE_INSTALL_PREFIX=/usr -G Ninja -DCMAKE_BUILD_TYPE=Release -DBINARY_PACKAGE_BUILD=1 -DCMAKE_INSTALL_LIBDIR=lib64
cmake --build . --target install --parallel $(nproc)

# Grab lensfun database. You should run `sudo lensfun-update-data` before making
# AppImage, we did this in CI.
mkdir -p ../AppDir/usr/share/lensfun
cp -a /var/lib/lensfun-updates/* ../AppDir/usr/share/lensfun

## Get the latest Linuxdeploy and its Gtk plugin to package everything
wget -c "https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh"
wget -c "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
chmod +x linuxdeploy-x86_64.AppImage linuxdeploy-plugin-gtk.sh

export DEPLOY_GTK_VERSION="3"
export LINUXDEPLOY_OUTPUT_VERSION=$(sh ../tools/get_git_version_string.sh)

export LDAI_UPDATE_INFORMATION="gh-releases-zsync|aurelienpierreeng|ansel|v0.0.0|Ansel-*-x86_64.AppImage.zsync"

# Fix https://github.com/linuxdeploy/linuxdeploy/issues/272 on Fedora
export NO_STRIP=true

# Our plugins link against libansel, it's not in system, so tell linuxdeploy
# where to find it. Don't use LD_PRELOAD here, linuxdeploy cannot see preloaded
# libraries.
ANSEL_LIBDIR="$(find ../AppDir/usr -type d -name ansel -path "../AppDir/usr/lib*" | head -n 1)"
if [ -z "${ANSEL_LIBDIR}" ]; then
  echo "ERROR: Could not locate installed ansel libraries in AppDir." >&2
  find ../AppDir/usr -maxdepth 4 -type d -name ansel >&2
  exit 1
fi
ANSEL_LIBROOT="$(dirname "${ANSEL_LIBDIR}")"
export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:${ANSEL_LIBROOT}/"
# Using `--deploy-deps-only` to tell linuxdeploy also collect dependencies for
# libraries in this dir, but don't copy those libraries. On the contrary,
# `--library` will copy both libraries and their dependencies, which is not what
# we want, we already installed our plugins.
# `--depoly-deps-only` apparently doesn't recurse in subfolders, everything
# needs to be declared.
./linuxdeploy-x86_64.AppImage \
  --appdir ../AppDir \
  --plugin gtk \
  --deploy-deps-only "${ANSEL_LIBDIR}" \
  --deploy-deps-only "${ANSEL_LIBDIR}/views" \
  --deploy-deps-only "${ANSEL_LIBDIR}/plugins" \
  --deploy-deps-only "${ANSEL_LIBDIR}/plugins/imageio/format" \
  --deploy-deps-only "${ANSEL_LIBDIR}/plugins/imageio/storage" \
  --deploy-deps-only "${ANSEL_LIBDIR}/plugins/lighttable" \
  --output appimage
