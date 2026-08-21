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

#ifndef DT_WIDGETS_COLLAPSIBLE_SECTION_H
#define DT_WIDGETS_COLLAPSIBLE_SECTION_H

/* A titled section that folds away, remembering whether the user left it open. */

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct dt_gui_collapsible_section_t
{
  GtkBox *parent;       // the parent widget
  const char *confname; // configuration name for the toggle status
  GtkWidget *toggle;    // toggle button
  GtkWidget *expander;  // the expander
  GtkBox *container;    // the container for all widgets in the section
  GtkWidget *label;     // the section label
} dt_gui_collapsible_section_t;

/**
 * @brief Create a collapsible section and pack it into the parent box.
 *
 * The `pack` argument makes the insertion side explicit so callers control layout order
 * without reordering children later.
 *
 * @param cs section storage owned by the caller.
 * @param confname configuration key used to persist the expanded state.
 * @param label UI label for the section header.
 * @param parent GtkBox that receives the section.
 * @param pack either `GTK_PACK_START` or `GTK_PACK_END` to choose insertion side.
 */
void dt_gui_new_collapsible_section(dt_gui_collapsible_section_t *cs,
                                    const char *confname, const char *label,
                                    GtkBox *parent, GtkPackType pack);
// routine to be called from gui_update
void dt_gui_update_collapsible_section(dt_gui_collapsible_section_t *cs);

// routine to hide the collapsible section
void dt_gui_hide_collapsible_section(dt_gui_collapsible_section_t *cs);

G_END_DECLS

#endif // DT_WIDGETS_COLLAPSIBLE_SECTION_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
