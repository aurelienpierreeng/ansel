/*
    This file is part of darktable,
    Copyright (C) 2009-2011 johannes hanika.
    Copyright (C) 2010 Henrik Andersson.
    Copyright (C) 2011, 2014-2016 Tobias Ellinghaus.
    Copyright (C) 2012 Frédéric Grollier.
    Copyright (C) 2012, 2019-2022 Pascal Obry.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2016 Roman Lebedev.
    Copyright (C) 2018 Edgardo Hoszowski.
    Copyright (C) 2019-2020 Aldric Renaudin.
    Copyright (C) 2019, 2022 Hanno Schwalm.
    Copyright (C) 2020 Chris Elston.
    Copyright (C) 2020-2021 Hubert Kowalski.
    Copyright (C) 2020 Philippe Weyland.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2025 Alynx Zhou.
    Copyright (C) 2025-2026 Aurélien PIERRE.
    
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

#ifndef DT_COMMON_HISTORY_H
#define DT_COMMON_HISTORY_H

#include <gtk/gtk.h>
#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dt_history_copy_item_t
{
  GList *selops;
  GtkTreeView *items;
  int copied_imageid;
} dt_history_copy_item_t;

/** helper function to free a GList of dt_history_item_t */
void dt_history_item_free(gpointer data);

/** delete all history for the given image */
void dt_history_delete_on_image(int32_t imgid);

/** as above but control whether to record undo/redo */
void dt_history_delete_on_image_ext(int32_t imgid, gboolean undo);

/* dt_history_duplicate() used to be declared here. It walks dt_dev_history_item_t and calls
 * dt_iop_get_module(), both of which develop/ owns, so its definition cannot come down to
 * this layer -- the declaration went up to develop/dev_history.h instead, beside it. */



typedef struct dt_history_item_t
{
  guint num;
  gchar *op;
  gchar *name;
  gboolean enabled;
} dt_history_item_t;

/** get list of history items for image */
GList *dt_history_get_items(int32_t imgid, gboolean enabled);

/** get list of history items for image as a nice string */
char *dt_history_get_items_as_string(int32_t imgid);

/** get a single history item as string with enabled status */
char *dt_history_item_as_string(const char *name, gboolean enabled);

#ifdef __cplusplus
}
#endif

#endif // DT_COMMON_HISTORY_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
