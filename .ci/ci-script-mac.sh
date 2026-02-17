#   This file is part of the Ansel project.
#   Copyright (C) 2024 jakehl.
#   Copyright (C) 2025 Miguel Moquillon.
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


# set -ex
#   
# CPU_ARCHITECTURE=""
# if [[ `uname -a` =~ ^Darwin.* ]] && [[ `uname -a` =~ .*arm64$ ]]
# then
#     CPU_ARCHITECTURE="ARM64"
#     CMAKE_MORE_OPTIONS="${CMAKE_MORE_OPTIONS} -DBUILD_SSE2_CODEPATHS=OFF"
# else
# 	CPU_ARCHITECTURE="Intel"
# fi;
#   
# mkdir build
cd "$BUILD_DIR"

cmake .. \
    -G "$GENERATOR" \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX"\
    -DCMAKE_BUILD_TYPE=$CMAKE_BUILD_TYPE \
    -DRAWSPEED_ENABLE_LTO=ON \
    -DBUILD_CURVE_TOOLS=OFF \
    -DBUILD_NOISE_TOOLS=OFF \
    -DUSE_LUA=OFF \
    -DUSE_BUNDLED_LUA=OFF \
    -DUSE_LIBRAW=ON \
    -DUSE_BUNDLED_LIBRAW=OFF \
    -DBUILD_SSE2_CODEPATHS=OFF \
    -DCMAKE_BUILD_TYPE=Release \
    -DUSE_COLORD=OFF \
    -DUSE_KWALLET=OFF \
    -DBUILD_CMSTEST=OFF \
    -DBUILD_BENCHMARKING=OFF \
    $ECO 

cmake --build "$BUILD_DIR" --target install



