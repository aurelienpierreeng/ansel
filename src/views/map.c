/*
    This file is part of darktable,
    Copyright (C) 2011 Henrik Andersson.
    Copyright (C) 2012 Jean-Sébastien Pédron.
    Copyright (C) 2012-2018 Tobias Ellinghaus.
    Copyright (C) 2013-2014, 2016-2017, 2019-2021 Pascal Obry.
    Copyright (C) 2013-2017 Roman Lebedev.
    Copyright (C) 2013-2014 Ronny Kahl.
    Copyright (C) 2014-2017 Jérémy Rosen.
    Copyright (C) 2014 parafin.
    Copyright (C) 2017, 2021 luzpaz.
    Copyright (C) 2017 Sven Claussner.
    Copyright (C) 2018 Rikard Öxler.
    Copyright (C) 2019-2021 Aldric Renaudin.
    Copyright (C) 2019 Heiko Bauke.
    Copyright (C) 2019-2021 Philippe Weyland.
    Copyright (C) 2020 Chris Elston.
    Copyright (C) 2020, 2022 Diederik Ter Rahe.
    Copyright (C) 2020-2021 Hubert Kowalski.
    Copyright (C) 2020-2021 Ralf Brown.
    Copyright (C) 2021 Paolo Benvenuto.
    Copyright (C) 2022-2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Miloš Komarčević.
    Copyright (C) 2022 Nicolas Auffray.
    Copyright (C) 2023 Ricky Moon.
    
    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    
    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/collection.h"
#include "common/act_on.h"
#include "common/debug.h"
#include "common/gpx.h"
#include "common/geo.h"
#include "common/module_versioning.h"
#include "common/selection.h"
#include "common/times.h"
#include "common/utility.h"
#include "common/conf.h"
#include "control/control.h"
#include "gui/dtgtk/thumbtable.h"

#include "gui/drag_and_drop.h"
#include "widgets/draw.h"
#include "views/view.h"
#include "views/view_api.h"
#include "control/signal.h"  // DT_SIGNAL_* / dt_control_signal_*, previously reached through gui/draw.h
#include "control/signal.h"
