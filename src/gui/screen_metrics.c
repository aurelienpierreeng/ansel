/*
    This file is part of Ansel,
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

#include "gui/screen_metrics.h"
#include "widgets/widget_settings.h"

/* Written once at startup by whoever can interrogate the display, read everywhere after.
 * The defaults are the neutral ones, so a reader that runs before the push -- or in a
 * headless process where the push never happens -- gets unscaled geometry rather than
 * zeroes. Setters reject non-positive values for the same reason: a bad probe must not be
 * able to collapse every length in the application to 0. */
/* Forwarders. The store lives in widgets/widget_settings.c -- see the comment there for why
 * the toolkit owns it rather than the GUI. These names exist for the readers below layer 4
 * (the crash reporter, the telemetry payload) and for the cairo helpers in the header. */

double dt_screen_dpi(void) { return dt_widget_dpi(); }
void dt_screen_set_dpi(double dpi) { dt_widget_set_dpi(dpi); }

double dt_screen_dpi_factor(void) { return dt_widget_dpi_factor(); }
void dt_screen_set_dpi_factor(double factor) { dt_widget_set_dpi_factor(factor); }

double dt_screen_ppd(void) { return dt_widget_ppd(); }
void dt_screen_set_ppd(double ppd) { dt_widget_set_ppd(ppd); }

gboolean dt_screen_metrics_probed(void) { return dt_widget_metrics_probed(); }

double dt_screen_em_size(void) { return dt_widget_em_size(); }
void dt_screen_set_em_size(double em) { dt_widget_set_em_size(em); }

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
