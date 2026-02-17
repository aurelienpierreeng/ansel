/*
    This file is part of darktable,
    Copyright (C) 2010 Henrik Andersson.
    Copyright (C) 2010, 2012 johannes hanika.
    Copyright (C) 2011 Robert Bieber.
    Copyright (C) 2011-2014, 2016 Tobias Ellinghaus.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2013 Jérémy Rosen.
    Copyright (C) 2013 Simon Spannagel.
    Copyright (C) 2019-2020 Philippe Weyland.
    Copyright (C) 2020 Aldric Renaudin.
    Copyright (C) 2021 Diederik Ter Rahe.
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

#ifdef __cplusplus
extern "C" {
#endif

/** array of names and constant to ease label manipulation */
typedef enum dt_colorlables_enum
{
  DT_COLORLABELS_RED,
  DT_COLORLABELS_YELLOW,
  DT_COLORLABELS_GREEN,
  DT_COLORLABELS_BLUE,
  DT_COLORLABELS_PURPLE,
  DT_COLORLABELS_LAST,
} dt_colorlabels_enum;
/** array with all names as strings, terminated by a NULL entry */
extern const char *dt_colorlabels_name[];

/** get the assigned colorlabels of imgid*/
int dt_colorlabels_get_labels(const int32_t imgid);
/** remove labels associated to imgid */
void dt_colorlabels_remove_labels(const int32_t imgid);
/** assign a color label to imgid - no undo no image group*/
void dt_colorlabels_set_label(const int32_t imgid, const int color);
/** save all assigned color labels from cached dt_image_t to database */
void dt_colorlabels_set_labels(const int32_t imgid, const int colors);
/** assign a color label to the list of image*/
void dt_colorlabels_toggle_label_on_list(GList *list, const int color, const gboolean undo_on);
/** remove a color label from imgid */
void dt_colorlabels_remove_label(const int32_t imgid, const int color);
/** get the name of the color for a given number (could be replaced by an array) */
const char *dt_colorlabels_to_string(int label);
/** check if an image has a color label */
int dt_colorlabels_check_label(const int32_t imgid, const int color);
/** cleanup cached statements */
void dt_colorlabels_cleanup(void);

#ifdef __cplusplus
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
