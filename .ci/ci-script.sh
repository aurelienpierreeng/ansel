#!/bin/bash
#   This file is part of darktable,
#   Copyright (C) 2016-2017 Roman Lebedev.
#   Copyright (C) 2016 Tobias Ellinghaus.
#   Copyright (C) 2020 Andreas Schneider.
#   Copyright (C) 2020 Heiko Bauke.
#   Copyright (C) 2020 Hubert Kowalski.
#   Copyright (C) 2022-2023 Aurélien PIERRE.
#   
#   darktable is free software: you can redistribute it and/or modify
#   it under the terms of the GNU General Public License as published by
#   the Free Software Foundation, either version 3 of the License, or
#   (at your option) any later version.
#   
#   darktable is distributed in the hope that it will be useful,
#   but WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#   GNU General Public License for more details.
#   
#   You should have received a copy of the GNU General Public License
#   along with darktable.  If not, see <http://www.gnu.org/licenses/>.
# it is supposed to be run by travis-ci
 expects a few env variables to be set:
#   BUILD_DIR - the working directory, where to build
#   INSTALL_DIR - the installation prefix.
#   SRC_DIR - read-only directory with git checkout to compile
#   CC, CXX, CFLAGS, CXXFLAGS are not required, should make sense too
#   TARGET - either build, skiptest, nofeatures or usermanual
#   ECO - some other flags for cmake
#   
set -ex

if [ "$GENERATOR" = "Ninja" ];
then
  VERBOSE="-v"
  KEEPGOING="-k0"
  JOBS=""
fi;

if [ "$GENERATOR" = "Unix Makefiles" ];
then
  VERBOSE="VERBOSE=1";
  KEEPGOING="-k"
  JOBS="-j2"
fi;

if [ "$GENERATOR" = "MSYS Makefiles" ];
then
  VERBOSE="VERBOSE=1";
  KEEPGOING="-k"
  JOBS="-j2"
fi;

target_build()
{
  cmake --build "$BUILD_DIR" -- $JOBS "$VERBOSE" "$KEEPGOING"
  ctest --output-on-failure || ctest --rerun-failed -V -VV
  cmake --build "$BUILD_DIR" --target install -- $JOBS "$VERBOSE" "$KEEPGOING"
}

target_notest()
{
  cmake --build "$BUILD_DIR" -- $JOBS "$VERBOSE" "$KEEPGOING"
  cmake --build "$BUILD_DIR" --target install -- $JOBS "$VERBOSE" "$KEEPGOING"
}

diskspace()
{
  df
  du -hcs "$SRC_DIR"
  du -hcs "$BUILD_DIR"
  du -hcs "$INSTALL_PREFIX"
}

diskspace

cd "$BUILD_DIR"

case "$TARGET" in
  "build")
    cmake -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" -G"$GENERATOR" -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" "$ECO" -DVALIDATE_APPDATA_FILE=ON -DBUILD_TESTING=ON -DTESTBUILD_OPENCL_PROGRAMS=ON "$SRC_DIR" || (cat "$BUILD_DIR"/CMakeFiles/CMakeOutput.log; cat "$BUILD_DIR"/CMakeFiles/CMakeError.log)
    target_build
    ;;
  "skiptest")
    cmake -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" -G"$GENERATOR" -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" $ECO "$SRC_DIR" || (cat "$BUILD_DIR"/CMakeFiles/CMakeOutput.log; cat "$BUILD_DIR"/CMakeFiles/CMakeError.log)
    target_notest
    ;;
  "nofeatures")
    cmake -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
      -G"$GENERATOR" \
      -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" \
      -DUSE_OPENMP=OFF \
      -DUSE_OPENCL=OFF \
      -DUSE_LUA=OFF \
      -DUSE_NLS=OFF \
      -DUSE_GRAPHICSMAGICK=OFF \
      -DUSE_OPENJPEG=OFF \
      -DUSE_WEBP=OFF \
      -DUSE_AVIF=OFF \
      -DUSE_XCF=OFF \
      -DBUILD_CMSTEST=OFF \
      -DUSE_OPENEXR=OFF \
      -DBUILD_PRINT=OFF \
      -DBUILD_RS_IDENTIFY=OFF \
      -DUSE_LENSFUN=OFF \
      -DUSE_GMIC=OFF \
      -DUSE_LIBSECRET=OFF \
      $ECO "$SRC_DIR" || (cat "$BUILD_DIR"/CMakeFiles/CMakeOutput.log; cat "$BUILD_DIR"/CMakeFiles/CMakeError.log)
    target_notest
    ;;
  "nofeatures_nosse")
    cmake -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
      -G"$GENERATOR" \
      -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" \
      -DUSE_OPENMP=OFF \
      -DUSE_OPENCL=OFF \
      -DUSE_LUA=OFF \
      -DUSE_NLS=OFF \
      -DUSE_GRAPHICSMAGICK=OFF \
      -DUSE_OPENJPEG=OFF \
      -DUSE_WEBP=OFF \
      -DUSE_AVIF=OFF \
      -DUSE_XCF=OFF \
      -DBUILD_CMSTEST=OFF \
      -DUSE_OPENEXR=OFF \
      -DBUILD_PRINT=OFF \
      -DBUILD_RS_IDENTIFY=OFF \
      -DUSE_LENSFUN=OFF \
      -DUSE_GMIC=OFF \
      -DUSE_LIBSECRET=OFF \
      -DBUILD_SSE2_CODEPATHS=OFF \
      $ECO "$SRC_DIR" || (cat "$BUILD_DIR"/CMakeFiles/CMakeOutput.log; cat "$BUILD_DIR"/CMakeFiles/CMakeError.log)
    target_notest
    ;;
  *)
    exit 1
    ;;
esac

diskspace
