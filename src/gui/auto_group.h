/*
    This file is part of Ansel,
    Copyright (C) 2026 Aurélien PIERRE.

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#ifndef DT_GUI_AUTO_GROUP_H
#define DT_GUI_AUTO_GROUP_H

#include <gtk/gtk.h>

/** @brief Open the modal Auto Group dialog without a nested GTK main loop.
 * @details Owns preview scheduling and lifetime. Planning uses immutable snapshots on a worker;
 * GTK and database access stay on the main thread. Apply always collects a fresh snapshot.
 */
void dt_gui_auto_group_show(GtkWindow *parent);

#endif
