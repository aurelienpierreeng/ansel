#!/bin/bash
#   This file is part of darktable,
#   Copyright (C) 2011, 2013 johannes hanika.
#   Copyright (C) 2011 Markus Jung.
#   Copyright (C) 2012 Jean-Sébastien Pédron.
#   Copyright (C) 2015 Omri Har-Shemesh.
#   Copyright (C) 2015 parafin.
#   Copyright (C) 2015 Roman Lebedev.
#   Copyright (C) 2015 Tobias Ellinghaus.
#   Copyright (C) 2016, 2019-2020 Pascal Obry.
#   Copyright (C) 2017, 2020 Heiko Bauke.
#   Copyright (C) 2020 Hubert Kowalski.
#   Copyright (C) 2020 Victor Engmark.
#   Copyright (C) 2023 Maurizio Paglia.
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
#   
# Usage: purge_non_existing_images [-p]
#        -p  do the purge, otherwise only display non existing images

#   
if ! command -v sqlite3 >/dev/null
then
    echo "Error: please install sqlite3 binary".
    exit 1
fi

if pgrep -x "ansel" >/dev/null
then
    echo "Error: Ansel is running, please exit first"
    exit 1
fi

configdir="${HOME}/.config/ansel"
DBFILE="${configdir}/library.db"
dryrun=1
library=""

# remember the command line to show it in the end when not purging
commandline="$0 $*"

# handle command line arguments
while [ "$#" -ge 1 ]
do
    option="$1"
    case "$option" in
    -h | --help)
        echo "Delete non existing images from Ansel's database"
        echo "Usage:   ${0} [options]"
        echo ""
        echo "Options:"
        echo "  -c|--configdir <path>    path to the Ansel config directory"
        echo "                           (default: '${configdir}')"
        echo "  -l|--library <path>      path to the library.db"
        echo "                           (default: '${DBFILE}')"
        echo "  -p|--purge               actually delete the tags instead of just finding them"
        exit 0
        ;;
    -l | --library)
        library="$2"
        shift
        ;;
    -c | --configdir)
        configdir="$2"
        shift
        ;;
    -p | --purge)
        dryrun=0
        ;;
    *)
        echo "Warning: ignoring unknown option ${option}"
        ;;
    esac
    shift
done

DBFILE="$configdir/library.db"

if [ "$library" != "" ]
then
    DBFILE="$library"
fi

if [ ! -f "$DBFILE" ]
then
    echo "Error: library db '${DBFILE}' doesn't exist"
    exit 1
fi

QUERY="SELECT images.id, film_rolls.folder || '/' || images.filename FROM images JOIN film_rolls ON images.film_id = film_rolls.id"

echo "Removing the following non existent file(s):"

while read -r -u 9 id path
do
    if ! [ -f "$path" ]
    then
        echo "  ${path} with ID = ${id}"
        ids="${ids+${ids},}${id}"
    fi
done 9< <(sqlite3 -separator $'\t' "$DBFILE" "$QUERY")

if [ "$dryrun" -eq 0 ]
then
    for table in images meta_data
    do
        sqlite3 "$DBFILE" <<< "DELETE FROM ${table} WHERE id IN ($ids)"
    done

    for table in color_labels history masks_history selected_images tagged_images history_hash module_order
    do
        sqlite3 "$DBFILE" <<< "DELETE FROM ${table} WHERE imgid in ($ids)"
    done

    # delete now-empty film rolls
    sqlite3 "$DBFILE" "DELETE FROM film_rolls WHERE NOT EXISTS (SELECT 1 FROM images WHERE images.film_id = film_rolls.id)"
    sqlite3 "$DBFILE" "VACUUM; ANALYZE"
else
    echo
    echo Remove following now-empty filmrolls:
    sqlite3 "$DBFILE" "SELECT folder FROM film_rolls WHERE NOT EXISTS (SELECT 1 FROM images WHERE images.film_id = film_rolls.id)"

    echo
    echo to really remove non existing images from the database call:
    echo "${commandline} --purge"
fi
