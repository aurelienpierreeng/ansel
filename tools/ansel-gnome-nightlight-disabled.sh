#!/bin/bash
#   This file is part of darktable,
#   Copyright (C) 2019 Wolfgang Goetz.
#   Copyright (C) 2022 Aurélien PIERRE.
#   Copyright (C) 2025 Hubert Figuière.
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
 command line: put this file in path before darktable as: /usr/local/bin/darktable
 desktop icon: edit /usr/share/applications/photos.ansel.ansel.desktop: Exec and TryExec pointing to /usr/local/bin/darktable
 (ubuntu18.04, dt from git)

#   
[ "${FLOCKER}" != "$0" ] && exec env FLOCKER="$0" flock -en "$0" "$0" "$@" || :

gnomenightlight="org.gnome.settings-daemon.plugins.color night-light-enabled"

trap "gsettings set ${gnomenightlight} $(gsettings get ${gnomenightlight})" EXIT

gsettings set ${gnomenightlight} false

/opt/ansel/bin/darktable "$@"
