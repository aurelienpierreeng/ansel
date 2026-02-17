#!/bin/bash
#   This file is part of darktable,
#   Copyright (C) 2020 Hubert Kowalski.
#   Copyright (C) 2021 luzpaz.
#   Copyright (C) 2022 Uwe Ohse.
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
# Usage: extract_wb_from_images

#   
commandline="$0 $*"

# handle command line arguments
option="$1"
if [ "${option}" = "-h" ] || [ "${option}" = "--help" ]; then
  echo "Extract White Balance preset info from images"
  echo "Usage:   $0 <file1> [file2] ..."
  echo ""
  echo "This tool will generate archive with white balance"
  echo "presets extracted from provided image files"
  exit 0
fi

tmp_dir=$(mktemp -d -t dt-wb-XXXXXXXXXX)
cur_dir=$(pwd)

tarball="$cur_dir"/ansel-whitebalance-$(date +'%Y%m%d').tar.gz

echo "Extracting WB presets."
for image in "$@"
do
    echo -n "."
    exiftool -Make -Model "-WBType*" "-WB_*" "-ColorTemp*"                     \
      -WhiteBalance -WhiteBalance2 -WhitePoint -ColorCompensationFilter        \
      -WBShiftAB -WBShiftAB_GM -WBShiftAB_GM_Precise -WBShiftGM -WBScale       \
      -WhiteBalanceFineTune -WhiteBalanceComp -WhiteBalanceSetting             \
      -WhiteBalanceBracket -WhiteBalanceBias -WBMode -WhiteBalanceMode         \
      -WhiteBalanceTemperature -WhiteBalanceDetected -ColorTemperature         \
      -WBShiftIntelligentAuto -WBShiftCreativeControl -WhiteBalanceSetup       \
      -WBRedLevel -WBBlueLevel -WBGreenLevel -RedBalance -BlueBalance          \
      "${image}" > "${tmp_dir}/${image}.txt"
done

echo
echo "preparing tarball..."

tar -czf "${tarball}" -C ${tmp_dir} .

echo "cleaning up..."
rm -rf $tmp_dir

echo

echo "Extracting wb presets done, post the following file to us:"
echo $tarball
