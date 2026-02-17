#!/bin/bash
#   This file is part of darktable,
#   Copyright (C) 2011-2012 Pascal de Bruijn.
#   Copyright (C) 2012 Jean-Sébastien Pédron.
#   Copyright (C) 2014 Roman Lebedev.
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
 Script updates our wb_presets which we regularly steal from UFRaw.

#   
TEMP_FILE=$(tempfile -p dtwb -s .c)
OUT_FILE="../src/external/wb_presets.c"

echo "Downloading new wb_presets.c into ${TEMP_FILE}"

wget http://ufraw.cvs.sourceforge.net/viewvc/ufraw/ufraw/wb_presets.c?content-type=text%2Fplain -O "$TEMP_FILE"

echo "Processing ${TEMP_FILE} into ${OUT_FILE}, this may take a while"

IFS="
"
cat "$TEMP_FILE" | while read -r LINE; do
  if [ "${LINE}" = '#include "ufraw.h"' ]; then
    echo '#ifdef HAVE_CONFIG_H'
    echo '#include "config.h"'
    echo '#endif'
    echo ''
  elif [ "${LINE}" = '#include <glib/gi18n.h>' ]; then
    echo '#include <glib.h>'
    echo '#include <glib/gi18n.h>'
    echo ''
    echo 'typedef struct'
    echo '{'
    echo '  const char *make;'
    echo '  const char *model;'
    echo '  const char *name;'
    echo '  int tuning;'
    echo '  double channel[4];'
    echo '}'
    echo 'wb_data;'
  else
    echo "${LINE}" | grep -v 'K", ' | grep -v ', uf_'
    echo "${LINE}" | grep '"2700K",'
    echo "${LINE}" | grep '"3000K",'
    echo "${LINE}" | grep '"3300K",'
    echo "${LINE}" | grep '"5000K",'
    echo "${LINE}" | grep '"5500K",'
    echo "${LINE}" | grep '"6500K",'
  fi
done > "$OUT_FILE"

rm "$TEMP_FILE"

