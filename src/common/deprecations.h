/*
    This file is part of Ansel,
    Copyright (C) 2021 Hanno Schwalm.
    Copyright (C) 2021 Pascal Obry.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2026 Aurélien PIERRE.
    
    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    
    Ansel is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    along with Ansel.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <string.h>

/**
 * @brief Modules definitively deprecated and removed from the code base,
 * by their `op` name.
 * 
 */
static const char *hard_deprecations[] = 
  {
    "clahe",          // deprecated: March 6th 2011. Code removed: April 13th 2026.
    "colortransfer",  // deprecated: April 6th 2013. Code removed: April 13th 2026.
    "equalizer",      // deprecated: March 23rd 2011. Code removed: April 13th 2026.
    NULL
  };

/**
 * @brief Modules without a proper IOP order should throw errors and log, except if they are deprecated
 * definitively.
 * 
 * @param op 
 * @return int 
 */
int dt_deprecated(const char *op)
{
  int i = 0;
  while(hard_deprecations[i])
  {
    if(!strcmp(hard_deprecations[i], op))
      return 1;
    i++;
  }
  return 0;
}
