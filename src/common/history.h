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

#pragma once

#include <gtk/gtk.h>
#include <inttypes.h>
#include <sqlite3.h>

#include "common/image_cache.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dt_history_copy_item_t
{
  GList *selops;
  GtkTreeView *items;
  int copied_imageid;
  gboolean full_copy;
  gboolean copy_iop_order;
} dt_history_copy_item_t;

/** helper function to free a GList of dt_history_item_t */
void dt_history_item_free(gpointer data);

/** delete all history for the given image */
void dt_history_delete_on_image(int32_t imgid);

/** as above but control whether to record undo/redo */
void dt_history_delete_on_image_ext(int32_t imgid, gboolean undo);

/** copy history from imgid and pasts on selected images, merge or overwrite... */
gboolean dt_history_copy(int32_t imgid);
gboolean dt_history_copy_parts(int32_t imgid);
gboolean dt_history_paste_on_list(const GList *list, gboolean undo);
gboolean dt_history_paste_parts_on_list(const GList *list, gboolean undo);

/** load a dt file and applies to selected images */
int dt_history_load_and_apply_on_list(gchar *filename, const GList *list);

/** load a dt file and applies to specified image */
int dt_history_load_and_apply(int32_t imgid, gchar *filename, int history_only);

/** delete historystack of selected images */
gboolean dt_history_delete_on_list(const GList *list, gboolean undo);

/** compress history stack */
int dt_history_compress_on_list(const GList *imgs);
void dt_history_compress_on_image(const int32_t imgid);

/** truncate history stack */
void dt_history_truncate_on_image(const int32_t imgid, const int32_t history_end);

/** read history_end from database for an image (main.images.history_end) */
int32_t dt_history_get_end(const int32_t imgid);

/* duplicate an history list */
GList *dt_history_duplicate(GList *hist);



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

/* check if a module exists in the history of corresponding image */
gboolean dt_history_check_module_exists(int32_t imgid, const char *operation, gboolean enabled);
/** cleanup cached statements */
void dt_history_cleanup(void);

/** update mipmap hash in database from cached image history hash */
void dt_history_hash_set_mipmap(const int32_t imgid, const dt_image_cache_write_mode_t mode);

#ifdef __cplusplus
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
