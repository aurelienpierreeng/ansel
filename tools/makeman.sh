#!/bin/sh
#   This file is part of darktable,
#   Copyright (C) 2010 johannes hanika.
#   Copyright (C) 2014-2015 Tobias Ellinghaus.
#   Copyright (C) 2016 Roman Lebedev.
#   Copyright (C) 2017, 2020 Heiko Bauke.
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
set -e;

input="$1"
authors="$2"
output="$3"

r=$(sed -n 's,.*\$Release: \(.*\)\$$,\1,p' "$input")
d=$(sed -n 's,/,-,g;s,.*\$Date: \(..........\).*,\1,p' "$input")
D=""
if [ -n "$d" ]; then
  D="--date=$d"
fi

pod2man --utf8 --release="darktable $r" --center="darktable" "$D" "$input" \
  | sed -e '/.*DREGGNAUTHORS.*/r '"$authors" | sed -e '/.*DREGGNAUTHORS.*/d' \
  > "$output" || rm "$output"
