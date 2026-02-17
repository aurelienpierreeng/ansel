#!/bin/bash
#   This file is part of darktable,
#   Copyright (C) 2021-2022 Pascal Obry.
#   Copyright (C) 2023 Alynx Zhou.
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
# Get the name of the last two translators in the 2 past years for a
# all languages. This is expected to be used to send reminder e-mail
# for translating ansel.
#   
cd po

SINCE_DATE="2 years"
NB_LAST_TRANSLATOR="3"

for TR in *.po; do
    # check if some output
    RES=$(git log --since="$SINCE_DATE" --format="%an <%ae>" $TR | grep -v noreply | grep -v "@nowhere" | uniq | head -$NB_LAST_TRANSLATOR )

    if [[ -z $RES ]]; then
        # no translator since 2 years, get the last one
        git log -100 --format="%an <%ae>" $TR | grep -v noreply | grep -v "@nowhere" | uniq | head -1
    else
        git log --since="$SINCE_DATE" --format="%an <%ae>" $TR | grep -v noreply | grep -v "@nowhere" | uniq | head -$NB_LAST_TRANSLATOR
    fi
done | sort | uniq | while read OUT; do
    echo -n "$OUT, "
done
