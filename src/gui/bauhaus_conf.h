/*
 *    This file is part of Ansel,
 *    Copyright (C) 2026 Aurélien PIERRE.
 *
 *    Ansel is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    Ansel is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with Ansel.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef DT_GUI_BAUHAUS_CONF_H
#define DT_GUI_BAUHAUS_CONF_H

#include "widgets/bauhaus.h"

/* Preference-bound bauhaus widgets.
 *
 * A convenience layer over the widget, not part of it: it reads the <enum> declaration from
 * anselconfig.xml (dt_confgen_*), fills the combobox from the declared values, and writes the
 * selection straight back to conf. All of that is application configuration, which is why it
 * lives here rather than with the widget it builds on. */

/** A combobox whose entries and current value come from the `confkey` <enum> config entry.
 *  Returns NULL, with a message on stderr, if `confkey` is not declared as an enum. */
GtkWidget *dt_bauhaus_combobox_from_conf(dt_bauhaus_t *bh, dt_gui_module_t *self, const char *confkey);

#endif // DT_GUI_BAUHAUS_CONF_H
