/*
 *    This file is part of darktable,
 *    Copyright (C) 2012 Richard Wonka.
 *    Copyright (C) 2012-2016 Tobias Ellinghaus.
 *    Copyright (C) 2015 Edouard Gomez.
 *    Copyright (C) 2016 Roman Lebedev.
 *    Copyright (C) 2019 Jakub Filipowicz.
 *    Copyright (C) 2020, 2022 Chris Elston.
 *    Copyright (C) 2020-2022 Diederik Ter Rahe.
 *    Copyright (C) 2020 Pascal Obry.
 *    Copyright (C) 2020, 2022 Ralf Brown.
 *    Copyright (C) 2021-2022 Aldric Renaudin.
 *    Copyright (C) 2022 Martin Bařinka.
 *    Copyright (C) 2022 Nicolas Auffray.
 *    Copyright (C) 2023-2025 Aurélien PIERRE.
 *    Copyright (C) 2025 Guillaume Stutin.
 *    
 *    darktable is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *    
 *    darktable is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *    
 *    You should have received a copy of the GNU General Public License
 *    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <glib.h>

#include "widgets/bauhaus.h"
#include "common/conf.h"
#include "common/utility.h"
#include "gui/guides.h"
#include "widgets/draw.h"
#include "control/control.h"
#include "develop/develop.h"  // DT_DEV_OVERLAY_*, previously reached through gui/draw.h
#include "develop/develop.h"
