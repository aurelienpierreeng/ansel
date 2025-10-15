#!/bin/bash
#
# Script to build and install ansel with custom configuration
#

# Exit in case of error
set -e -o pipefail
trap 'echo "${BASH_SOURCE[0]}{${FUNCNAME[0]}}:${LINENO}: Error: command \`${BASH_COMMAND}\` failed with exit code $?"' ERR

# Go to directory of script
scriptDir=$(dirname "$0")
cd "$scriptDir"/
scriptDir=$(pwd)

# Set variables
buildDir="${scriptDir}/../../build"
installDir="${scriptDir}/../../install"

homebrewHome=$(brew --prefix)

# Build and install ansel here
# ../../build.sh --install --build-type Release --prefix ${PWD}

# Check for previous attempt and clean
if [[ -d "$buildDir" ]]; then
    echo "Deleting directory $buildDir ... "
    rm -rf "$buildDir"
fi

# Check for previous attempt and clean
if [[ -d "$installDir" ]]; then
    echo "Deleting directory $installDir ... "
    rm -rf "$installDir"
fi

# Create directory
mkdir "$installDir"
mkdir "$buildDir"
cd "$buildDir"

# Version of MacOsX
osx_target=$(sw_vers | grep ProductVersion | cut -d':' -f2 | tr -d '\t' | tr -d ' ')

# Configure build
cmake .. \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=$osx_target \
    -DCMAKE_CXX_FLAGS=-stdlib=libc++ \
    -DCMAKE_OBJCXX_FLAGS=-stdlib=libc++ \
    -DBINARY_PACKAGE_BUILD=ON \
    -DRAWSPEED_ENABLE_LTO=ON \
    -DBUILD_CURVE_TOOLS=OFF \
    -DBUILD_NOISE_TOOLS=OFF \
    -DUSE_LUA=OFF \
    -DUSE_BUNDLED_LUA=OFF \
    -DUSE_LIBRAW=ON \
    -DUSE_BUNDLED_LIBRAW=OFF \
    -DUSE_GRAPHICSMAGICK=ON \
    -DUSE_IMAGEMAGICK=OFF \
    -DBUILD_SSE2_CODEPATHS=OFF \
    -DCMAKE_BUILD_TYPE=Release \
    -DUSE_COLORD=OFF \
    -DUSE_KWALLET=OFF \
    -DBUILD_CMSTEST=OFF \
    -DBUILD_BENCHMARKING=OFF \
    -DCMAKE_INSTALL_PREFIX="$installDir"

# Build using all available cores
make -j"$(sysctl -n hw.ncpu)"

# Install
make install
