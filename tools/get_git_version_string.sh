#!/bin/sh
#   This file is part of darktable,
#   Copyright (C) 2016-2017 Roman Lebedev.
#   Copyright (C) 2020 Heiko Bauke.
#   Copyright (C) 2020 Matthieu Volat.
#   Copyright (C) 2022 Aurélien PIERRE.
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
VERSION="$(git describe --tags)"

if [ $? -eq 0 ] ;
then
  echo "$VERSION" | sed 's,^v,,;s,-,+,;s,-,~,;'
  exit 0
fi

# with shallow clones, there may be no tags, so the first ^
# try to get version string will fail

# in that case let's at least return the commit hash

VERSION="$(git describe --always)"
if [ $? -eq 0 ] ;
then
  echo "$VERSION"
  exit 0
fi

# failed for some reason. let's just propagate
echo "unknown-version"
exit 0 # to not fail the whole build.
