/*
    This file is part of darktable,
    Copyright (C) 2009-2015 johannes hanika.
    Copyright (C) 2010-2012 Henrik Andersson.
    Copyright (C) 2010-2018 Tobias Ellinghaus.
    Copyright (C) 2011 Antony Dovgal.
    Copyright (C) 2011 Jérémy Rosen.
    Copyright (C) 2011 Omari Stephens.
    Copyright (C) 2011 Robert Bieber.
    Copyright (C) 2011 Rostyslav Pidgornyi.
    Copyright (C) 2011 Simon Spannagel.
    Copyright (C) 2012 Christian Tellefsen.
    Copyright (C) 2012-2014 José Carlos García Sogo.
    Copyright (C) 2012 Petr Styblo.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012 Ulrich Pegelow.
    Copyright (C) 2013 Eckhart Pedersen.
    Copyright (C) 2013 Jochem Kossen.
    Copyright (C) 2013-2016, 2018-2021 Pascal Obry.
    Copyright (C) 2013 Pierre Le Magourou.
    Copyright (C) 2013-2016 Roman Lebedev.
    Copyright (C) 2014-2015, 2019-2022 Aldric Renaudin.
    Copyright (C) 2014 Matthias Gehre.
    Copyright (C) 2014 Mikhail Trishchenkov.
    Copyright (C) 2014 moopmonster.
    Copyright (C) 2014-2015 Pedro Côrte-Real.
    Copyright (C) 2015 Jan Kundrát.
    Copyright (C) 2015 JohnnyRun.
    Copyright (C) 2016 Asma.
    Copyright (C) 2017 Dan Torop.
    Copyright (C) 2017 itinerarium.
    Copyright (C) 2017, 2019 luzpaz.
    Copyright (C) 2017, 2019 Marcello Mamino.
    Copyright (C) 2017 Matthieu Moy.
    Copyright (C) 2017 parafin.
    Copyright (C) 2017-2018 Peter Budai.
    Copyright (C) 2018 Frederic Chanal.
    Copyright (C) 2018-2019 Heiko Bauke.
    Copyright (C) 2018 Mario Lueder.
    Copyright (C) 2018 Rick Yorgason.
    Copyright (C) 2018-2019 Rikard Öxler.
    Copyright (C) 2019-2020, 2022-2023, 2025 Aurélien PIERRE.
    Copyright (C) 2019 Edgardo Hoszowski.
    Copyright (C) 2019 jakubfi.
    Copyright (C) 2019, 2022 Philippe Weyland.
    Copyright (C) 2019 Sam Smith.
    Copyright (C) 2019 vacaboja.
    Copyright (C) 2020 Bill Ferguson.
    Copyright (C) 2020-2021 Chris Elston.
    Copyright (C) 2020-2022 Diederik Ter Rahe.
    Copyright (C) 2020 EdgarLux.
    Copyright (C) 2020 Hanno Schwalm.
    Copyright (C) 2020 Hubert Kowalski.
    Copyright (C) 2021 domosbg.
    Copyright (C) 2021 Fabio Heer.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Sakari Kapanen.
    Copyright (C) 2022 solarer.
    
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
/** this is the view for the lighttable module.  */

#include "widgets/bauhaus.h"
#include "common/collection.h"
#include "common/history.h"
#include "common/module_versioning.h"
#include "common/undo.h"
#include "control/control.h"
#include "control/jobs.h"
#include "gui/dtgtk/thumbtable.h"

#include "widgets/draw.h"
#include "views/view.h"
#include "views/view_api.h"
#include "control/signal.h"  // DT_SIGNAL_* / dt_control_signal_*, previously reached through gui/draw.h
#include "control/signal.h"
