#!/bin/bash
#   This file is part of darktable,
#   Copyright (C) 2012 johannes hanika.
#   Copyright (C) 2016 Matthieu Volat.
#   Copyright (C) 2020 Heiko Bauke.
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

REF="reference.pfm"

# benchmark the performance of denoising algorithms, in terms of PSNR.
# 
# instructions:
# shoot a static scene with all interesting iso settings.
# shoot several at lowest iso (like 3x iso 100).
# combine the iso 100 shots as a hdr in lt mode (to average out noise),
# this will be reference.pfm
# export them all as pfm.
#

for i in *.pfm
do
  if [ "$i" == "$REF" ]
  then
    continue
  fi
  echo "$i : $(compare -metric PSNR $i $REF /dev/null 2>&1)"
done

