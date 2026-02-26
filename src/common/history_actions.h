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

#pragma once

#include <glib.h>
#include <inttypes.h>

#include "common/history_merge.h"

/** copy history from imgid and pasts on selected images, merge or overwrite... */
gboolean dt_history_copy(int32_t imgid);
gboolean dt_history_copy_parts(int32_t imgid);
gboolean dt_history_paste_on_list(const GList *list);
gboolean dt_history_paste_parts_prepare(void);
gboolean dt_history_paste_parts_on_list(const GList *list);
gboolean dt_history_paste_on_image(const int32_t imgid);
gboolean dt_history_paste_parts_on_image(const int32_t imgid);

gboolean dt_history_copy_and_paste_on_image(const int32_t imgid, const int32_t dest_imgid, GList *ops,
                                            const gboolean copy_full, const dt_history_merge_strategy_t mode);

/** apply style to selected images */
gboolean dt_history_style_on_list(const GList *list, const char *name, const gboolean duplicate);
gboolean dt_history_style_on_image(const int32_t imgid, const char *name, const gboolean duplicate);

/** compress history stack */
int dt_history_compress_on_list(const GList *imgs);
void dt_history_compress_on_image(const int32_t imgid);

/** delete historystack of selected images */
gboolean dt_history_delete_on_list(const GList *list, gboolean undo);

/** load a dt file and applies to selected images */
int dt_history_load_and_apply_on_list(gchar *filename, const GList *list);

/** load a dt file and applies to specified image */
int dt_history_load_and_apply(const int32_t imgid, gchar *filename, int history_only);
int dt_history_load_and_apply_on_image(int32_t imgid, gchar *filename, int history_only);
