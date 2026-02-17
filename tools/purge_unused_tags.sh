#!/bin/bash
#   This file is part of darktable,
#   Copyright (C) 2013-2014, 2016, 2019-2020 Pascal Obry.
#   Copyright (C) 2017, 2020 Heiko Bauke.
#   Copyright (C) 2020 Hubert Kowalski.
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
# Usage: purge_unused_tags [-p]
#        -p  do the purge, otherwise only display unused tags

#   
if ! which sqlite3 > /dev/null; then
    echo "Error: please install sqlite3 binary".
    exit 1
fi

if pgrep -x "ansel" > /dev/null ; then
    echo "Error: Ansel is running, please exit first"
    exit 1
fi

configdir="$HOME/.config/ansel"
LIBDB="$configdir/library.db"
dryrun=1
library=""

# remember the command line to show it in the end when not purging
commandline="$0 $*"

# handle command line arguments
while [ "$#" -ge 1 ] ; do
  option="$1"
  case ${option} in
  -h|--help)
    echo "Delete unused tags from Ansel's database"
    echo "Usage:   $0 [options]"
    echo ""
    echo "Options:"
    echo "  -c|--configdir <path>    path to the Ansel config directory"
    echo "                           (default: '${configdir}')"
    echo "  -l|--library <path>      path to the library.db"
    echo "                           (default: '${LIBDB}')"
    echo "  -p|--purge               actually delete the tags instead of just finding them"
    exit 0
    ;;
  -l|--library)
    library="$2"
    shift
    ;;
  -c|--configdir)
    configdir="$2"
    shift
    ;;
  -p|--purge)
    dryrun=0
    ;;
  *)
    echo "Warning: ignoring unknown option $option"
    ;;
  esac
    shift
done

LIBDB="$configdir/library.db"
DATADB="$configdir/data.db"

if [ "$library" != "" ]; then
    LIBDB="$library"
fi

if [ ! -f "$LIBDB" ]; then
    echo "Error: library db '${LIBDB}' doesn't exist"
    exit 1
fi

if [ ! -f "$DATADB" ]; then
    echo "Error: data db '${DATADB}' doesn't exist"
    exit 1
fi

# tags not used
Q1C="
ATTACH DATABASE \"$LIBDB\" as lib;
ATTACH DATABASE \"$DATADB\" as data;
SELECT name FROM data.tags WHERE id NOT IN (SELECT tagid FROM tagged_images);
"

Q1="
ATTACH DATABASE \"$LIBDB\" as lib;
ATTACH DATABASE \"$DATADB\" as data;
DELETE FROM data.tags WHERE id NOT IN (SELECT tagid FROM tagged_images);
"

if [ ${dryrun} -eq 0 ]; then
    echo Purging tags...
    echo "$Q1C" | sqlite3
    echo "$Q1" | sqlite3

# since sqlite3 up until version 3.15 didn't support vacuuming
# attached databases we'll do them separately.

   sqlite3 "$LIBDB" "VACUUM; ANALYZE;"
   sqlite3 "$DATADB" "VACUUM; ANALYZE"

else
    echo The following tags are not used:
    echo "$Q1C" | sqlite3
    echo
    echo to really purge from the database call:
    echo "${commandline} --purge"
fi
