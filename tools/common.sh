#!/bin/bash
#   This file is part of darktable,
#   Copyright (C) 2011 Tobias Ellinghaus.
#   Copyright (C) 2020 Heiko Bauke.
#   Copyright (C) 2021 lrkwz.
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
# Partly stolen from http://imagejdocu.tudor.lu/doku.php?id=diverse:commandline:imagej
# Detect readlink or realpath version
# if GNU readlink is known to be installed, this code can be replaced by "alias ReadLink='readlink -f'"
ReadLink='readlink' # Default
OS=$(uname)
if [ "$OS" = "Darwin" -o "$OS" = "FreeBSD" ] && which greadlink >/dev/null 2>&1; then
        # Using GNU readlink on MacOS X or FreeBSD
        ReadLink='greadlink -f'
elif [ "$OS" = "Darwin" -o "$OS" = "FreeBSD" -o "$OS" = "Linux" ] && which realpath >/dev/null 2>&1; then
        if [ -f "$(which readlink)" ] && readlink --version | grep coreutils >/dev/null 2>&1; then
                ReadLink='readlink -f' # use GNU readlink
        else
                # Using realpath on MacOS X or FreeBSD
                ReadLink='realpath'
        fi
else
        ReadLink='echo'
        echo "Please install realpath or GNU readlink. Symbolic links may not be resolved properly" >&2
fi
