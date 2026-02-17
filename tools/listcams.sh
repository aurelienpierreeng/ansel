#!/bin/bash
#   This file is part of darktable,
#   Copyright (C) 2012-2013 johannes hanika.
#   Copyright (C) 2015 Tobias Ellinghaus.
#   Copyright (C) 2017, 2020 Heiko Bauke.
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
# TODO: extract basecurves, color matrices etc and create a complete html table for the web:
echo "cameras with profiled presets for denoising:"
grep '"model"' < data/noiseprofiles.json | sed 's/.*"model"[ \t]*:[ \t]*"\([^"]*\).*/\1/' | tr "[:upper:]" "[:lower:]" | sort | sed 's/$/<\/li>/' | sed 's/^/<li>/'
