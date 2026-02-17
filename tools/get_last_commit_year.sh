#!/bin/sh
#   This file is part of darktable,
#   Copyright (C) 2017 Roman Lebedev.
#   Copyright (C) 2017 Tobias Ellinghaus.
#   Copyright (C) 2020 Heiko Bauke.
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
DT_SRC_DIR=$(dirname "$0")
DT_SRC_DIR=$(cd "$DT_SRC_DIR/../" && pwd -P)

LAST_COMMIT_YEAR=$(git --git-dir="${DT_SRC_DIR}/.git" log -n1 --pretty=%ci)

if [ $? -eq 0 ]; then
  echo "${LAST_COMMIT_YEAR}" | cut -b 1-4
  exit 0
fi

# fallback in case git above failed

date -u "+%Y"
