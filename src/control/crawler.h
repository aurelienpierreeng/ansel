/*
    This file is part of darktable,
    Copyright (C) 2014, 2016 Tobias Ellinghaus.
    Copyright (C) 2020 Pascal Obry.
    Copyright (C) 2022 Martin Bařinka.
    
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

#ifndef DT_CONTROL_CRAWLER_H
#define DT_CONTROL_CRAWLER_H

#include <glib.h>

// this function iterates over ALL images from the database and checks whether
// - the XMP file on disk is newer than the timestamp from db
// - there is a .txt or .wav file associated with the image and mark so in the db
//   or if such a file no longer exists
// it returns the list of images with a (supposedly) updated xmp file to let the user decide
//
// It costs one directory listing per film roll, so it is bounded by filesystem latency and by
// the size of the library -- NOT fast, whatever the comment here used to claim. Call it
// synchronously only from a user action that asked for it; the startup path uses the
// background form below.
GList *dt_control_crawler_run();

// the same crawl, as a background job: it shows the popup itself, on the GUI thread, if
// anything turned up. This is what startup uses -- waiting for the crawl before building the
// main window held it back for 102 s on a network-mounted library of 1969 images.
void dt_control_crawler_run_in_background(void);

// show a popup with the images, let the user decide what to do and free the list afterwards
void dt_control_crawler_show_image_list(GList *images);

#endif // DT_CONTROL_CRAWLER_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

