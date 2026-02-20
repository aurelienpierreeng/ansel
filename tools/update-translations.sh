#!/bin/bash
#   This file is part of the Ansel project.
#   Copyright (C) 2024-2025 Aurélien PIERRE.
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
#   
#   
#   
#   
#   
#   
#   
#   
#   
#  go to project root
PROJECT_ROOT="$(cd `dirname $0`/..; pwd -P)"
cd "$PROJECT_ROOT"

cd po

# Update from source code
intltool-update -m
intltool-update -p -g ansel

# Remove old translations
for f in *.po ; do
  echo "$f"
  msgmerge -U $f ansel.pot
done

# Report
intltool-update -g ansel -r
