    /*
    This file is part of darktable,
    Copyright (C) 2009-2011, 2014 johannes hanika.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2016 Tobias Ellinghaus.
    Copyright (C) 2020, 2022 Diederik Ter Rahe.
    Copyright (C) 2020-2021 Pascal Obry.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2025 Alynx Zhou.
    
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

#ifndef DT_DEVELOP_IMAGEOP_GUI_H
#define DT_DEVELOP_IMAGEOP_GUI_H

#include "develop/imageop.h"
#include "widgets/paint.h"   // DTGTKCairoPaintIconFunc, named in three declarations below

#ifdef __cplusplus
extern "C" {
#endif

GtkWidget *dt_bauhaus_slider_from_params(dt_iop_module_t *self, const char *param);

GtkWidget *dt_bauhaus_combobox_from_params(dt_iop_module_t *self, const char *param);

GtkWidget *dt_bauhaus_toggle_from_params(dt_iop_module_t *self, const char *param);

GtkWidget *dt_iop_togglebutton_new(dt_iop_module_t *self, const char *section, const gchar *label, const gchar *ctrl_label,
                                   GCallback callback, gboolean local, guint accel_key, GdkModifierType mods,
                                   DTGTKCairoPaintIconFunc paint, GtkWidget *box);
/** Build an IOP toggle button without registering it for module GUI refresh. */
GtkWidget *dt_iop_togglebutton_new_no_register(dt_iop_module_t *self, const char *section, const gchar *label,
                                               const gchar *ctrl_label, GCallback callback, gboolean local,
                                               guint accel_key, GdkModifierType mods,
                                               DTGTKCairoPaintIconFunc paint, GtkWidget *box);

GtkWidget *dt_iop_button_new(dt_iop_module_t *self, const gchar *label,
                             GCallback callback, gboolean local, guint accel_key, GdkModifierType mods,
                             DTGTKCairoPaintIconFunc paint, gint paintflags, GtkWidget *box);

/* returns up or !up depending on the masks_updown preference */
gboolean dt_mask_scroll_increases(int up);


/* The IOP-parameter flavour of widgets/resetlabel.h: a label that, when double-clicked,
 * restores one parameter to its default, refreshes the module GUI and records history.
 *
 * The widget itself knows none of that -- it only emits "reset". This wrapper is where the
 * IOP meaning is attached, which is why it lives in develop/ and not in widgets/. */
GtkWidget *dt_iop_gui_reset_label_new(const gchar *label, dt_iop_module_t *module, void *param,
                                      int param_size);

#ifdef __cplusplus
}
#endif

#endif // DT_DEVELOP_IMAGEOP_GUI_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
