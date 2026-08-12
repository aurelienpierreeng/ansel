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

#include "develop/dev_history_gui.h"

#include "develop/blend_gui.h"
#include "develop/dev_history.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/pixelpipe_hb.h"
#include "system/macros.h"

#include <gtk/gtk.h>

/* Undoing an edit also undoes what the user was LOOKING at: the recorded mask-edit view
 * for the focused module. The data half (dt_masks_set_edit_mode, request_mask_display) is
 * restored by the engine; this pokes the blending panel's widgets to match. */
static void _undo_restore_gui(dt_develop_t *dev, const int mask_edit_mode,
                              const int request_mask_display)
{
  (void)mask_edit_mode;   // consumed by the engine's dt_masks_set_edit_mode() call

  dt_iop_gui_update_blendif(dev->gui_module);
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)(dev->gui_module->blend_data);
  if(bd)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->showmask),
                                 request_mask_display == DT_DEV_PIXELPIPE_DISPLAY_MASK);
}

void dt_dev_history_gui_init(void)
{
  dt_dev_history_set_undo_restore_gui_handler(_undo_restore_gui);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
