#!/bin/bash

# Build Ansel within an AppDir directory
# Then package it as an .AppImage
# Call this script from the Ansel root folder like `sh .ci/ci-script-appimage.sh`
# Copyright (c) Aurélien Pierre - 2022

# For local builds, purge and clean build pathes if any
if [ -d "build" ];
then yes | rm -R build;
fi;

if [ -d "AppDir" ];
then yes | rm -R AppDir;
fi;

mkdir build
mkdir AppDir
cd build

export CXXFLAGS="-O3 -fno-strict-aliasing "
export CFLAGS="$CXXFLAGS"

## AppImages require us to install everything in /usr, where root is the AppDir
export DESTDIR=../AppDir
cmake .. -DCMAKE_INSTALL_PREFIX=/usr -G Ninja -DCMAKE_BUILD_TYPE=Release -DBINARY_PACKAGE_BUILD=1 -DCMAKE_INSTALL_LIBDIR=lib64
cmake --build . --target install

## Replace relative pathes to executable in ansel.desktop
## The pathes will be handled by AppImage.
sed -i 's/\/usr\/bin\///' ../AppDir/usr/share/applications/photos.ansel.app.desktop

## Get the latest Linuxdeploy and its Gtk plugin to package everything
wget -c "https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh"
wget -c "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
chmod +x linuxdeploy-x86_64.AppImage linuxdeploy-plugin-gtk.sh

export DEPLOY_GTK_VERSION="3"
export VERSION=$(sh ../tools/get_git_version_string.sh)

# Our plugins link against libansel, it's not in system, so tell linuxdeploy
# where to find it. Don't use LD_PRELOAD here, linuxdeploy cannot see preloaded
# libraries.
export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:../AppDir/usr/lib64/ansel/"
# Using `--deploy-deps-only` to tell linuxdeploy also collect dependencies for
# libraries in this dir, but don't copy those libraries. On the contrary,
# `--library` will copy both libraries and their dependencies, which is not what
# we want, we already installed our plugins.
./linuxdeploy-x86_64.AppImage --appdir ../AppDir --plugin gtk --deploy-deps-only ../AppDir/usr/lib64/ansel/plugins --output appimage
