#!/bin/sh
#   This file is part of darktable,
#   Copyright (C) 2016-2017 Roman Lebedev.
#   Copyright (C) 2017, 2020 Heiko Bauke.
#   Copyright (C) 2017 luzpaz.
#   Copyright (C) 2017 Tobias Ellinghaus.
#   Copyright (C) 2020 Matthieu Volat.
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
#   
#   
#   
#   
#   
#   
#   
#   
#   
#   
set -e

DT_SRC_DIR=$(dirname "$0")
DT_SRC_DIR=$(cd "$DT_SRC_DIR/../" && pwd -P)

C_FILE="$1"
NEW_VERSION="$2"

VERSION_C_NEEDS_UPDATE=1
if [ -z "$NEW_VERSION" ]; then
  NEW_VERSION=$(./tools/get_git_version_string.sh)
fi

if echo "$NEW_VERSION" | grep -q Format; then
  NEW_VERSION="unknown-version"
fi

# version.c exists => check if it contains the up-to-date version
if [ -f "$C_FILE" ]; then
  OLD_VERSION=$(./tools/parse_version_c.sh "$C_FILE")
  if [ "${OLD_VERSION}" = "${NEW_VERSION}" ]; then
    VERSION_C_NEEDS_UPDATE=0
  fi
fi

MAJOR_VERSION=0
MINOR_VERSION=0
PATCH_VERSION=0
N_COMMITS=0
if echo "$NEW_VERSION" | grep -q "^[0-9]\+\.[0-9]\+\.[0-9]\+"; then
  MAJOR_VERSION=$(echo "$NEW_VERSION" | sed "s/^\([0-9]\+\)\.\([0-9]\+\)\.\([0-9]\+\).*/\1/")
  MINOR_VERSION=$(echo "$NEW_VERSION" | sed "s/^\([0-9]\+\)\.\([0-9]\+\)\.\([0-9]\+\).*/\2/")
  PATCH_VERSION=$(echo "$NEW_VERSION" | sed "s/^\([0-9]\+\)\.\([0-9]\+\)\.\([0-9]\+\).*/\3/")
fi
if echo "$NEW_VERSION" | grep -q "^[0-9]\+\.[0-9]\+\.[0-9]\++[0-9]\+"; then
  N_COMMITS=$(echo "$NEW_VERSION" | sed "s/^\([0-9]\+\)\.\([0-9]\+\)\.\([0-9]\+\)+\([0-9]\+\).*/\4/")
fi

LAST_COMMIT_YEAR=$("${DT_SRC_DIR}/tools/get_last_commit_year.sh")

if [ $VERSION_C_NEEDS_UPDATE -eq 1 ]; then
# when changing format, you must also update tools/get_git_version_string.sh !!!
  {
    echo "#ifndef RC_BUILD"
    echo "  #ifdef HAVE_CONFIG_H"
    echo "    #include \"config.h\""
    echo "  #endif"

    echo "  const char darktable_package_version[] = \"${NEW_VERSION}\";"
    echo "  const char darktable_package_string[] = PACKAGE_NAME \" ${NEW_VERSION}\";"
    echo "  const char darktable_last_commit_year[] = \"${LAST_COMMIT_YEAR}\";"
    echo "#else"
    echo "  #define DT_MAJOR ${MAJOR_VERSION}"
    echo "  #define DT_MINOR ${MINOR_VERSION}"
    echo "  #define DT_PATCH ${PATCH_VERSION}"
    echo "  #define DT_N_COMMITS ${N_COMMITS}"
    echo "  #define LAST_COMMIT_YEAR \"${LAST_COMMIT_YEAR}\""
    echo "#endif";
  } > "$C_FILE"

fi

echo "Version string: ${NEW_VERSION}"
