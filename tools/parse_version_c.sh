#!/bin/sh
#   This file is part of darktable,
#   Copyright (C) 2016 parafin.
#   Copyright (C) 2016-2017 Roman Lebedev.
#   Copyright (C) 2017, 2020 Heiko Bauke.
#   Copyright (C) 2017 luzpaz.
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

H_FILE="$1"

# version.c exists => check if it contains the up-to-date version
if [ -f "$H_FILE" ]; then
  OLD_VERSION=$(tr '"' '\n' < "$H_FILE" | sed '8q;d')
  echo $OLD_VERSION
fi
