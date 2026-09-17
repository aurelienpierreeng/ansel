/*
    This file is part of Ansel,
    Copyright (C) 2026 - Aurélien PIERRE.

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

/**
 * @file widgets/length_field.h
 * @brief A spin button whose VALUE is points and whose TEXT is a length with a unit.
 *
 * One widget, so that the properties' card, its strip and the guides cannot come to disagree
 * about what "210mm" is: they all read `common/length.h` through this.
 *
 * IT IS A `GtkSpinButton`. Every consumer goes on calling `gtk_spin_button_get_value()`,
 * `gtk_spin_button_set_value()` and `gtk_spin_button_update()` in POINTS, connects to
 * `value-changed` as before, and needs no change of its own -- the unit lives in the text and
 * never in the number. What it adds is that the person may TYPE a unit into it, and that the
 * unit they typed is the one it goes on showing.
 */

#ifndef DT_WIDGETS_LENGTH_FIELD_H
#define DT_WIDGETS_LENGTH_FIELD_H

#include <gtk/gtk.h>

/**
 * @brief A spin button that reads and writes lengths.
 *
 * @param unit_key where the unit it is left in is remembered, through the host's string
 *        storage -- `widgets/` keeps no preferences of its own. NULL remembers nothing.
 * @param unit the unit to start in when nothing is remembered. NULL starts in points. A name
 *        that is not a unit is refused and points are used, since a field that cannot parse
 *        its own fallback would refuse every bare number typed into it.
 * @param digits decimals of the DISPLAY unit to show. The field shows this or as many as the
 *        unit needs to resolve one point, whichever is more: a border stored to a tenth of a
 *        point must not read as "0 pt", and a page typed in centimetres must still be able to
 *        say which point it means.
 * @param minimum @param maximum @param step all in POINTS, like the value.
 * @return the spin button, floating like any GTK widget.
 */
GtkWidget *dt_length_field_new(const char *unit_key, const char *unit, int digits, double minimum,
                               double maximum, double step);

/** @brief The unit this field is showing, canonically spelled. Never NULL for a length field. */
const char *dt_length_field_get_unit(GtkWidget *field);

/**
 * @brief Show it in this unit from now on, and remember that.
 *
 * The VALUE does not change -- a length is a length -- only the text and what a bare number
 * typed into it will be read as. A name that is not a unit is ignored.
 */
void dt_length_field_set_unit(GtkWidget *field, const char *unit);

#endif // DT_WIDGETS_LENGTH_FIELD_H
